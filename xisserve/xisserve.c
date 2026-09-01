/*
 * xisserve - GTK2 application launcher (kickoff/krunner-style), companion
 * to xispanel's `xisserve` widget (see ../xispanel/widgets/xisserve.c --
 * already implemented and shipping; `launcher` stays the simple
 * pin-a-shortcut widget, no search/no .desktop parsing).
 *
 * v1 scope: parse the argv contract PROTOCOL.md documents, enforce a
 * flock'd singleton (same pattern xisback/xisguard already use) that
 * relays a second invocation's argv to the running instance over a
 * control socket as a reposition+retheme+toggle-visibility, scan
 * .desktop files across $XDG_DATA_DIRS + ~/.local/share/applications
 * into a filterable list, and launch whichever entry is clicked (or
 * Enter-activated) via the same "shell out through sh -c, don't wait"
 * pattern xispanel's own run_detached() uses. No icon rendering yet, no
 * in-app action search (HUD) -- see README.md's "Planned scope".
 *
 * Positioning: the window is a GTK_WINDOW_POPUP (override-redirect,
 * unmanaged by the WM) so its on-screen position is exactly what we ask
 * for, per PROTOCOL.md's anchor/edge/output flags, without fighting a
 * WM's own placement policy.
 */
#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>

#include "xisserve.h"

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define XISSERVE_VERSION "0.1.4"

#define WIN_WIDTH 520
#define WIN_HEIGHT 460
#define CAT_PANE_WIDTH 140

/* Results list (right pane): VCOL_ICON+VCOL_MARKUP render one row
 * (icon, then name+subtitle -- small category/plugin label under the
 * name), VCOL_ENTRY is the backing ResultEntry* -- used to launch on
 * click/Enter and to resolve "Adicionar/Remover Favorito" on
 * right-click. ResultEntry itself is defined in xisserve.h -- plugins
 * build it directly. */
enum { VCOL_ICON = 0, VCOL_MARKUP, VCOL_ENTRY, N_VCOLS };

/* Category list (left pane): "favorites" and "all" are synthetic,
 * xisserve-only categories (see build_category_store()); everything
 * else is a bucketed freedesktop Categories= key from kCategoryDefs. */
enum { CCOL_KEY = 0, CCOL_LABEL, N_CCOLS };

/* LaunchArgs::page when no mode flag was passed -- the default search/
 * list view, which isn't a page in kPages (it's the launcher's own
 * widgets, not a swappable root). */
#define PAGE_LAUNCHER (-1)

/* Every page PROTOCOL.md's mode flags can select. Adding one is one row
 * here plus its own pages/<name>.c -- see xisserve.h's "pages" section.
 * The calendar keeps 0/0 (shrink to the widget's natural size, which is
 * what keeps a small popup small); the audio mixer asks for real room,
 * since its rows only make sense at a usable slider width. */
static const XisservePage kPages[] = {
    {"calendar", 0, 0, page_calendar_build, page_calendar_on_show, NULL},
    {"audio", 380, 480, page_audio_build, page_audio_on_show, page_audio_on_hide},
};
#define N_PAGES ((int)(sizeof(kPages) / sizeof(kPages[0])))

static GtkWidget *g_page_roots[N_PAGES];

typedef struct {
    int anchor_x, anchor_y, anchor_w, anchor_h;
    char edge[8];
    int output_x, output_y, output_w, output_h;
    char bg[10];
    char fg[10];
    char font[128];
    int font_size;
    int page; /* index into kPages, or PAGE_LAUNCHER for the default view -- see PROTOCOL.md's mode flags */

    /* --menu: not a page and not the launcher at all, but a one-shot
     * application-menu popup for one window (see appmenu.c). Handled in
     * main() before the singleton lock, since it neither is nor should
     * disturb the running launcher instance. `menu_window` of 0 means
     * the active window. */
    int menu_mode;
    unsigned long menu_window;
    int menu_x, menu_y;
} LaunchArgs;

static LaunchArgs g_args;
static GtkWidget *g_window;
static GtkWidget *g_entry;
static GtkWidget *g_cat_treeview;
static GtkWidget *g_cat_scroll;
static GtkWidget *g_treeview;
static GtkWidget *g_content_box;  /* cat_scroll + results scroll; launcher view only */
static GtkWidget *g_footer_sep;
static GtkWidget *g_footer;       /* power-action buttons; launcher view only */
/* Header strip, packed above everything and never hidden by
 * apply_view_mode() -- it's the one row shared by every view, so the pin
 * toggle in it needs no per-page duplicate. */
static GtkWidget *g_header;
static GtkWidget *g_pin_btn;
/* "Pinned": stay open until explicitly closed instead of vanishing on
 * the first click elsewhere -- see on_pin_toggled(). g_pin_managed
 * tracks whether the window is currently handed to the WM as a dock
 * (set_pin_window_mode()), which decides who owns its stacking. */
static gboolean g_pinned;
static gboolean g_pin_managed;
static GtkListStore *g_cat_store;
static GtkListStore *g_view_store;
static GPtrArray *g_apps;           /* ResultEntry*, persistent scanned apps, owned */
static GPtrArray *g_plugin_results; /* ResultEntry*, rebuilt every search, owned */
static GHashTable *g_favorites;     /* set of .desktop basenames (key owned, value unused) */
static char g_selected_category[32] = "favorites";
static GtkTreePath *g_hovered_cat_path;
static pid_t g_watch_pid;

/* ---- argv / JSON plumbing -------------------------------------------- */

enum {
    OPT_ANCHOR_X = 1000, OPT_ANCHOR_Y, OPT_ANCHOR_W, OPT_ANCHOR_H,
    OPT_EDGE, OPT_OUTPUT_X, OPT_OUTPUT_Y, OPT_OUTPUT_W, OPT_OUTPUT_H,
    OPT_BG, OPT_FG, OPT_FONT, OPT_FONT_SIZE,
    OPT_MENU, OPT_MENU_WINDOW, OPT_MENU_X, OPT_MENU_Y,
    /* Page mode flags occupy OPT_PAGE_BASE + <index into kPages>, so
     * kPages stays the single place a page's flag name is written. */
    OPT_PAGE_BASE = 2000,
};

static const struct option kFixedOpts[] = {
    {"anchor-x", required_argument, 0, OPT_ANCHOR_X},
    {"anchor-y", required_argument, 0, OPT_ANCHOR_Y},
    {"anchor-w", required_argument, 0, OPT_ANCHOR_W},
    {"anchor-h", required_argument, 0, OPT_ANCHOR_H},
    {"edge", required_argument, 0, OPT_EDGE},
    {"output-x", required_argument, 0, OPT_OUTPUT_X},
    {"output-y", required_argument, 0, OPT_OUTPUT_Y},
    {"output-w", required_argument, 0, OPT_OUTPUT_W},
    {"output-h", required_argument, 0, OPT_OUTPUT_H},
    {"bg", required_argument, 0, OPT_BG},
    {"fg", required_argument, 0, OPT_FG},
    {"font", required_argument, 0, OPT_FONT},
    {"font-size", required_argument, 0, OPT_FONT_SIZE},
    /* --menu takes its window/x/y either as these flags or as three
     * positional arguments after it, so a caller can write the short
     * form a WM's own config is comfortable with -- kiwm's documented
     * example is `xisserve --menu %w %x %y`. */
    {"menu", no_argument, 0, OPT_MENU},
    {"window", required_argument, 0, OPT_MENU_WINDOW},
    {"menu-x", required_argument, 0, OPT_MENU_X},
    {"menu-y", required_argument, 0, OPT_MENU_Y},
};
#define N_FIXED_OPTS ((int)(sizeof(kFixedOpts) / sizeof(kFixedOpts[0])))

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s --anchor-x=<px> --anchor-y=<px> --anchor-w=<px> --anchor-h=<px> "
            "--edge=top|bottom|left|right --output-x=<px> --output-y=<px> --output-w=<px> "
            "--output-h=<px> --bg=#RRGGBBAA --fg=#RRGGBBAA --font=<family> --font-size=<px>",
            argv0);
    for (int i = 0; i < N_PAGES; i++) {
        fprintf(stderr, " [--%s]", kPages[i].flag);
    }
    fprintf(stderr, "\n       %s --menu [<window> <x> <y>] "
                    "[--window=<id>] [--menu-x=<px>] [--menu-y=<px>]\n", argv0);
    fprintf(stderr, "       %s --version\n", argv0);
}

static int parse_argv(int argc, char **argv, LaunchArgs *a)
{
    memset(a, 0, sizeof(*a));
    snprintf(a->edge, sizeof(a->edge), "top");
    snprintf(a->bg, sizeof(a->bg), "#282828ff");
    snprintf(a->fg, sizeof(a->fg), "#ffffffff");
    snprintf(a->font, sizeof(a->font), "sans-serif");
    a->font_size = 12;
    a->output_w = 1920;
    a->output_h = 1080;
    a->page = PAGE_LAUNCHER;

    /* kFixedOpts plus one no_argument entry per page, plus getopt's
     * terminator -- assembled here rather than written out statically so
     * a new kPages row needs no second edit. */
    struct option opts[N_FIXED_OPTS + N_PAGES + 1];
    memcpy(opts, kFixedOpts, sizeof(kFixedOpts));
    for (int i = 0; i < N_PAGES; i++) {
        struct option *o = &opts[N_FIXED_OPTS + i];
        o->name = kPages[i].flag;
        o->has_arg = no_argument;
        o->flag = NULL;
        o->val = OPT_PAGE_BASE + i;
    }
    memset(&opts[N_FIXED_OPTS + N_PAGES], 0, sizeof(struct option));

    int c;
    optind = 1;
    /* PROTOCOL.md: "An xisserve that doesn't know a flag must ignore it
     * and open normally rather than fail to start -- the widget side
     * ships before the page does, every time." So an unrecognized
     * option is skipped rather than fatal, and opterr=0 keeps getopt
     * from printing its own complaint about it. This is live today:
     * xispanel's notif widget already passes --notifications, which has
     * no page here yet. */
    opterr = 0;
    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        if (c >= OPT_PAGE_BASE && c < OPT_PAGE_BASE + N_PAGES) {
            a->page = c - OPT_PAGE_BASE;
            continue;
        }
        switch (c) {
        case OPT_ANCHOR_X: a->anchor_x = atoi(optarg); break;
        case OPT_ANCHOR_Y: a->anchor_y = atoi(optarg); break;
        case OPT_ANCHOR_W: a->anchor_w = atoi(optarg); break;
        case OPT_ANCHOR_H: a->anchor_h = atoi(optarg); break;
        case OPT_EDGE: snprintf(a->edge, sizeof(a->edge), "%s", optarg); break;
        case OPT_OUTPUT_X: a->output_x = atoi(optarg); break;
        case OPT_OUTPUT_Y: a->output_y = atoi(optarg); break;
        case OPT_OUTPUT_W: a->output_w = atoi(optarg); break;
        case OPT_OUTPUT_H: a->output_h = atoi(optarg); break;
        case OPT_BG: snprintf(a->bg, sizeof(a->bg), "%s", optarg); break;
        case OPT_FG: snprintf(a->fg, sizeof(a->fg), "%s", optarg); break;
        case OPT_FONT: snprintf(a->font, sizeof(a->font), "%s", optarg); break;
        case OPT_FONT_SIZE: a->font_size = atoi(optarg); break;
        case OPT_MENU: a->menu_mode = 1; break;
        /* strtoul base 0: a window id is as likely to be written the way
         * xprop prints it (0x3800004) as the decimal kiwm substitutes. */
        case OPT_MENU_WINDOW: a->menu_window = strtoul(optarg, NULL, 0); break;
        case OPT_MENU_X: a->menu_x = atoi(optarg); break;
        case OPT_MENU_Y: a->menu_y = atoi(optarg); break;
        default: break; /* unknown flag -- ignored on purpose, see above */
        }
    }

    /* Positional form: `--menu <window> <x> <y>`. Only read in menu mode,
     * and only for the values no flag already gave, so the two spellings
     * can be mixed without either overriding the other by accident. */
    if (a->menu_mode) {
        int pos = 0;
        for (int i = optind; i < argc && pos < 3; i++) {
            const char *v = argv[i];
            if (pos == 0 && !a->menu_window) a->menu_window = strtoul(v, NULL, 0);
            else if (pos == 1 && !a->menu_x)  a->menu_x = atoi(v);
            else if (pos == 2 && !a->menu_y)  a->menu_y = atoi(v);
            pos++;
        }
    }
    return 0;
}

/* Same hand-rolled flat-JSON helpers xisguard.c's control socket uses
 * (json_escape_str/json_get_str/json_get_int) -- duplicated here rather
 * than shared since xisserve is its own binary with no common library. */
static void json_escape_str(char *dst, size_t dst_sz, const char *src)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_sz; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dst_sz) break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if ((unsigned char)src[i] >= 0x20) {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

static int json_get_str(const char *msg, const char *key, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= dst_sz) len = dst_sz - 1;
    strncpy(dst, p, len);
    dst[len] = '\0';
    return 1;
}

static int json_get_int(const char *msg, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return sscanf(p, "%d", out) == 1;
}

static int parse_json_args(const char *msg, LaunchArgs *a)
{
    memset(a, 0, sizeof(*a));
    int ok = 1;
    ok &= json_get_int(msg, "anchor_x", &a->anchor_x);
    ok &= json_get_int(msg, "anchor_y", &a->anchor_y);
    ok &= json_get_int(msg, "anchor_w", &a->anchor_w);
    ok &= json_get_int(msg, "anchor_h", &a->anchor_h);
    ok &= json_get_str(msg, "edge", a->edge, sizeof(a->edge));
    ok &= json_get_int(msg, "output_x", &a->output_x);
    ok &= json_get_int(msg, "output_y", &a->output_y);
    ok &= json_get_int(msg, "output_w", &a->output_w);
    ok &= json_get_int(msg, "output_h", &a->output_h);
    ok &= json_get_str(msg, "bg", a->bg, sizeof(a->bg));
    ok &= json_get_str(msg, "fg", a->fg, sizeof(a->fg));
    json_get_str(msg, "font", a->font, sizeof(a->font));
    json_get_int(msg, "font_size", &a->font_size);
    /* Carried by flag name rather than kPages index: the wire format
     * stays readable in a socket dump, and an unknown name degrades to
     * the launcher view the same way an unknown argv flag does. */
    char page[32] = "";
    json_get_str(msg, "page", page, sizeof(page));
    a->page = PAGE_LAUNCHER;
    for (int i = 0; i < N_PAGES; i++) {
        if (strcmp(kPages[i].flag, page) == 0) {
            a->page = i;
            break;
        }
    }
    return ok;
}

/* ---- singleton / control socket --------------------------------------- */

static int send_to_running(const char *sockpath, const LaunchArgs *a)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("xisserve: socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("xisserve: connect");
        close(fd);
        return -1;
    }

    char font_esc[256];
    json_escape_str(font_esc, sizeof(font_esc), a->font);
    char msg[768];
    int n = snprintf(msg, sizeof(msg),
                      "{\"anchor_x\":%d,\"anchor_y\":%d,\"anchor_w\":%d,\"anchor_h\":%d,\"edge\":\"%s\","
                      "\"output_x\":%d,\"output_y\":%d,\"output_w\":%d,\"output_h\":%d,"
                      "\"bg\":\"%s\",\"fg\":\"%s\",\"font\":\"%s\",\"font_size\":%d,\"page\":\"%s\"}\n",
                      a->anchor_x, a->anchor_y, a->anchor_w, a->anchor_h, a->edge, a->output_x, a->output_y,
                      a->output_w, a->output_h, a->bg, a->fg, font_esc, a->font_size,
                      a->page >= 0 ? kPages[a->page].flag : "");
    if (n > 0) {
        ssize_t written = write(fd, msg, (size_t)n);
        (void)written;
    }
    close(fd);
    return 0;
}

static int open_listen_socket(const char *sockpath)
{
    unlink(sockpath);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("xisserve: socket");
        return -1;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("xisserve: bind");
        close(fd);
        return -1;
    }
    if (listen(fd, 8) != 0) {
        perror("xisserve: listen");
        close(fd);
        return -1;
    }
    return fd;
}

/* ---- app launching ----------------------------------------------------- */

/* Same fork+setsid+execl-via-sh-c pattern xispanel.c's run_detached()
 * uses -- duplicated here since xisserve is a standalone binary.
 * Exported (see xisserve.h) so plugins can launch things too. */
void run_detached(const char *cmd)
{
    if (!cmd || !cmd[0]) return;
    pid_t pid = fork();
    if (pid < 0) {
        perror("xisserve: fork");
        return;
    }
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Same single-quote shell-escaping helper folder.c/xisserve widget.c
 * duplicate locally rather than share, per existing repo convention.
 * Exported (see xisserve.h) for plugins that build their own commands. */
void shell_quote(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    if (o + 1 < outsz) out[o++] = '\'';
    for (const char *p = in; *p; p++) {
        if (*p == '\'') {
            const char *seq = "'\\''";
            for (const char *s = seq; *s && o + 1 < outsz; s++) out[o++] = *s;
        } else if (o + 1 < outsz) {
            out[o++] = *p;
        }
    }
    if (o + 1 < outsz) out[o++] = '\'';
    out[o < outsz ? o : outsz - 1] = 0;
}

/* Same terminal fallback chain folder.c's run_terminal_at() uses. */
/* `-e` on xterm/most terminal emulators execs its remaining argv
 * directly, with no shell splitting -- passing the whole (possibly
 * multi-word) cmd as a single shell-quoted argv element would try to
 * execvp a program literally named e.g. "ls -la". Routing it through
 * `sh -c <quoted-cmd>` instead makes the shell -- not the terminal --
 * responsible for splitting/interpreting it, which works for both a
 * bare "htop" and a full "ls -la; echo done". Exported (see xisserve.h)
 * for plugins that want to launch something in a terminal too (see
 * plugins/terminal.c). */
void build_terminal_exec(const char *cmd, char *out, size_t outsz)
{
    char q[600];
    shell_quote(cmd, q, sizeof(q));
    snprintf(out, outsz,
             "(exec xdg-terminal-exec -- sh -c %s 2>/dev/null) || "
             "([ -n \"$TERMINAL\" ] && exec \"$TERMINAL\" -e sh -c %s) || "
             "(exec x-terminal-emulator -e sh -c %s 2>/dev/null) || "
             "(exec xterm -e sh -c %s)",
             q, q, q, q);
}

/* Drops %f/%F/%u/%U/%i/%c/%k/etc field codes per the .desktop spec (we
 * pass no files/URIs), keeps a literal %% as a single %. */
static void strip_exec_field_codes(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < outsz; p++) {
        if (*p == '%' && p[1]) {
            if (p[1] == '%' && o + 1 < outsz) out[o++] = '%';
            p++;
            continue;
        }
        out[o++] = *p;
    }
    out[o] = 0;
}

/* ---- icons -------------------------------------------------------------- */

static void hex_to_rgba(const char *hex, double *r, double *g, double *b, double *a); /* defined below, theming section */

/* spec -> resolved GdkPixbuf* (or the NULL "nothing resolves this"
 * result), keyed exactly as passed to xisserve_resolve_icon(). g_hash_
 * table_lookup_extended() (not a plain lookup()) is what lets a cached
 * NULL be told apart from "not in the cache yet" without a sentinel. */
static GHashTable *g_icon_cache;

GdkPixbuf *xisserve_resolve_icon(const char *spec, int size)
{
    if (!spec || !spec[0]) return NULL;
    if (!g_icon_cache) g_icon_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    gpointer cached = NULL;
    if (g_hash_table_lookup_extended(g_icon_cache, spec, NULL, &cached)) {
        return cached ? GDK_PIXBUF(g_object_ref(cached)) : NULL;
    }

    GdkPixbuf *pixbuf = NULL;
    if (spec[0] == '/') {
        pixbuf = gdk_pixbuf_new_from_file_at_size(spec, size, size, NULL);
    } else {
        pixbuf = gtk_icon_theme_load_icon(gtk_icon_theme_get_default(), spec, size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
    }
    /* The cache keeps its own reference (or NULL); every caller,
     * including this first one, gets back a fresh ref it owns. */
    g_hash_table_insert(g_icon_cache, g_strdup(spec), pixbuf);
    return pixbuf ? g_object_ref(pixbuf) : NULL;
}

void xisserve_get_fg_rgba(double *r, double *g, double *b, double *a)
{
    hex_to_rgba(g_args.fg, r, g, b, a);
}

void xisserve_get_bg_rgba(double *r, double *g, double *b, double *a)
{
    hex_to_rgba(g_args.bg, r, g, b, a);
}

/* The only correct way to free a ResultEntry -- see xisserve.h. */
void result_entry_free(ResultEntry *e)
{
    if (!e) return;
    if (e->icon) g_object_unref(e->icon);
    if (e->activate_data && e->activate_data_free) e->activate_data_free(e->activate_data);
    g_free(e);
}

/* ---- categories ------------------------------------------------------- */

/* Buckets a .desktop file's (possibly multi-valued) Categories= field
 * into one of freedesktop.org's main categories -- just enough of the
 * menu-spec list to sort real-world apps into a handful of sidebar
 * entries, not the full sub-category tree. First match wins; apps with
 * no recognized category fall into "Other". */
typedef struct {
    const char *token; /* as it appears in Categories= */
    const char *key;   /* bucket key -- also what's stored in ResultEntry::category_key */
    const char *label; /* sidebar label, pt-BR to match the rest of the UI */
} CategoryDef;

static const CategoryDef kCategoryDefs[] = {
    {"AudioVideo", "AudioVideo", "Áudio e Vídeo"},
    {"Audio", "AudioVideo", "Áudio e Vídeo"},
    {"Video", "AudioVideo", "Áudio e Vídeo"},
    {"Development", "Development", "Desenvolvimento"},
    {"Education", "Education", "Educação"},
    {"Game", "Game", "Jogos"},
    {"Graphics", "Graphics", "Gráficos"},
    {"Network", "Network", "Internet"},
    {"Office", "Office", "Escritório"},
    {"Science", "Science", "Ciência"},
    {"Settings", "Settings", "Configurações"},
    {"System", "System", "Sistema"},
    {"Utility", "Utility", "Acessórios"},
};
#define N_CATEGORY_DEFS ((int)(sizeof(kCategoryDefs) / sizeof(kCategoryDefs[0])))

static const char *category_label_for_key(const char *key)
{
    if (strcmp(key, "favorites") == 0) return "Favoritos";
    if (strcmp(key, "all") == 0) return "Todos os Programas";
    for (int i = 0; i < N_CATEGORY_DEFS; i++) {
        if (strcmp(kCategoryDefs[i].key, key) == 0) return kCategoryDefs[i].label;
    }
    return "Outros";
}

/* Buckets a raw "Cat1;Cat2;;" Categories= value into out_key/out_label
 * (both caller-owned buffers); falls back to the "Other" bucket if none
 * of its tokens match kCategoryDefs (includes an empty/missing field). */
static void bucket_categories(const char *raw, char *out_key, size_t out_key_sz)
{
    snprintf(out_key, out_key_sz, "Other");
    if (!raw || !raw[0]) return;
    char *copy = g_strdup(raw);
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ";", &saveptr); tok; tok = strtok_r(NULL, ";", &saveptr)) {
        for (int i = 0; i < N_CATEGORY_DEFS; i++) {
            if (strcmp(kCategoryDefs[i].token, tok) == 0) {
                snprintf(out_key, out_key_sz, "%s", kCategoryDefs[i].key);
                g_free(copy);
                return;
            }
        }
    }
    g_free(copy);
}

/* ---- favorites ---------------------------------------------------------- */

/* Same flat "$XDG_CONFIG_HOME (or ~/.config)/xisserve-favorites.conf"
 * naming xisback.c uses for its own config file -- one .desktop basename
 * per line. */
static void favorites_path(char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(out, outsz, "%s/xisserve-favorites.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    char configdir[PATH_MAX];
    snprintf(configdir, sizeof(configdir), "%s/.config", home ? home : "");
    mkdir(configdir, 0700);
    snprintf(out, outsz, "%s/xisserve-favorites.conf", configdir);
}

static void load_favorites(void)
{
    if (g_favorites) g_hash_table_destroy(g_favorites);
    g_favorites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char path[PATH_MAX];
    favorites_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (line[0]) g_hash_table_add(g_favorites, g_strdup(line));
    }
    fclose(f);
}

static void save_favorites(void)
{
    char path[PATH_MAX];
    favorites_path(path, sizeof(path));
    FILE *f = fopen(path, "w");
    if (!f) {
        perror("xisserve: save favorites");
        return;
    }
    GHashTableIter it;
    gpointer key, value;
    g_hash_table_iter_init(&it, g_favorites);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        (void)value;
        fprintf(f, "%s\n", (const char *)key);
    }
    fclose(f);
}

/* Toggles id's favorite status both in the persisted set and in its
 * live ResultEntry (found by scanning g_apps -- small enough, and only
 * called from a menu click, that a linear scan is fine), then saves. */
static void toggle_favorite(const char *id)
{
    if (!id || !id[0]) return;
    gboolean now_favorite = !g_hash_table_contains(g_favorites, id);
    if (now_favorite) {
        g_hash_table_add(g_favorites, g_strdup(id));
    } else {
        g_hash_table_remove(g_favorites, id);
    }
    for (guint i = 0; i < g_apps->len; i++) {
        ResultEntry *e = g_ptr_array_index(g_apps, i);
        if (strcmp(e->id, id) == 0) {
            e->is_favorite = now_favorite;
            break;
        }
    }
    save_favorites();
}

/* ---- plugin config -------------------------------------------------------- */

/* SearchPluginFn itself, ResultEntry, and every helper a plugin needs
 * are declared in xisserve.h -- see plugins/terminal.c and
 * plugins/globalmenu.c. Adding a new plugin is: write plugins/<name>.c
 * defining one function matching SearchPluginFn, declare it in
 * xisserve.h, add one line here (its config name plus its function),
 * and one line to the Makefile's SRCS. */
typedef struct {
    const char *name;
    SearchPluginFn search;
} SearchPlugin;

static const SearchPlugin kSearchPlugins[] = {
    {"terminal", plugin_terminal_search},
    {"globalmenu", plugin_globalmenu_search},
};
#define N_SEARCH_PLUGINS ((int)(sizeof(kSearchPlugins) / sizeof(kSearchPlugins[0])))

static gboolean g_plugin_enabled[N_SEARCH_PLUGINS];

/* Same flat "$XDG_CONFIG_HOME (or ~/.config)/xisserve.conf" naming
 * xisserve-favorites.conf/xisback.conf use. */
static void config_path(char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(out, outsz, "%s/xisserve.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    char configdir[PATH_MAX];
    snprintf(configdir, sizeof(configdir), "%s/.config", home ? home : "");
    mkdir(configdir, 0700);
    snprintf(out, outsz, "%s/xisserve.conf", configdir);
}

/* Every line of xisserve.conf is "<SECTION>\t<key>\t<value>" -- the same
 * tab-delimited "KEYWORD\tfield..." shape xisback.conf's LAYER lines
 * use. They're all read into one store keyed "SECTION\tkey", which both
 * the plugin toggles ("PLUGIN\t<name>\tno") and any page's own settings
 * ("AUDIO\tscroll_step\t5") read back out of, rather than each adding
 * its own pass over the file. Reloaded on every rescan_apps(), same as
 * favorites, so an edit takes effect on the next open without
 * restarting the daemon. */
static GHashTable *g_config; /* "SECTION\tkey" -> value, both owned */

/* Same flat "$XDG_CONFIG_HOME (or ~/.config)/xisserve.conf" naming
 * xisserve-favorites.conf/xisback.conf use. */
static void load_config(void)
{
    if (g_config) g_hash_table_destroy(g_config);
    g_config = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    char path[PATH_MAX];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            size_t l = strlen(line);
            while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
            if (line[0] == '#' || !line[0]) continue;
            char *fields[3];
            int nf = 0;
            char *p = line;
            fields[nf++] = p;
            while (nf < 3 && (p = strchr(p, '\t'))) {
                *p = 0;
                p++;
                fields[nf++] = p;
            }
            if (nf != 3) continue;
            g_hash_table_insert(g_config, g_strdup_printf("%s\t%s", fields[0], fields[1]), g_strdup(fields[2]));
        }
        fclose(f);
    }

    /* Every plugin defaults to enabled -- a fresh install needs no
     * config file at all to get all of them; only an explicit "no"
     * turns one off. */
    for (int i = 0; i < N_SEARCH_PLUGINS; i++) {
        char key[128];
        snprintf(key, sizeof(key), "PLUGIN\t%s", kSearchPlugins[i].name);
        const char *val = g_hash_table_lookup(g_config, key);
        g_plugin_enabled[i] = !val || strcasecmp(val, "no") != 0;
    }
}

int xisserve_config_get_int(const char *section, const char *key, int fallback)
{
    if (!g_config) return fallback;
    char full[128];
    snprintf(full, sizeof(full), "%s\t%s", section, key);
    const char *val = g_hash_table_lookup(g_config, full);
    if (!val || !val[0]) return fallback;
    char *end = NULL;
    long n = strtol(val, &end, 10);
    if (end == val) return fallback; /* not a number at all -- keep the default */
    return (int)n;
}

/* ---- .desktop scanning ---------------------------------------------------- */

static void parse_desktop_file(const char *path, const char *basename, GPtrArray *apps)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    char name[256] = "";
    char exec_raw[1024] = "";
    char try_exec[512] = "";
    char categories_raw[512] = "";
    char icon_raw[256] = "";
    int is_application = 1;
    int no_display = 0;
    int hidden = 0;
    int terminal = 0;
    int in_entry = 0;
    int seen_entry = 0;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        if (line[0] == '[') {
            if (strncmp(line, "[Desktop Entry]", 15) == 0) {
                in_entry = 1;
                seen_entry = 1;
            } else if (seen_entry) {
                break;
            } else {
                in_entry = 0;
            }
            continue;
        }
        if (!in_entry) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *key = line;
        const char *val = eq + 1;
        if (strcmp(key, "Name") == 0) snprintf(name, sizeof(name), "%s", val);
        else if (strcmp(key, "Exec") == 0) snprintf(exec_raw, sizeof(exec_raw), "%s", val);
        else if (strcmp(key, "Type") == 0) is_application = (strcmp(val, "Application") == 0);
        else if (strcmp(key, "NoDisplay") == 0) no_display = (strcasecmp(val, "true") == 0);
        else if (strcmp(key, "Hidden") == 0) hidden = (strcasecmp(val, "true") == 0);
        else if (strcmp(key, "Terminal") == 0) terminal = (strcasecmp(val, "true") == 0);
        else if (strcmp(key, "TryExec") == 0) snprintf(try_exec, sizeof(try_exec), "%s", val);
        else if (strcmp(key, "Categories") == 0) snprintf(categories_raw, sizeof(categories_raw), "%s", val);
        else if (strcmp(key, "Icon") == 0) snprintf(icon_raw, sizeof(icon_raw), "%s", val);
    }
    fclose(f);

    if (!name[0] || !exec_raw[0] || !is_application || no_display || hidden) return;
    if (try_exec[0]) {
        char *found = g_find_program_in_path(try_exec);
        if (!found) return;
        g_free(found);
    }

    char exec_clean[1024];
    strip_exec_field_codes(exec_raw, exec_clean, sizeof(exec_clean));

    ResultEntry *e = g_new0(ResultEntry, 1);
    snprintf(e->id, sizeof(e->id), "%s", basename);
    snprintf(e->name, sizeof(e->name), "%s", name);
    if (terminal) {
        build_terminal_exec(exec_clean, e->exec, sizeof(e->exec));
    } else {
        snprintf(e->exec, sizeof(e->exec), "%s", exec_clean);
    }
    bucket_categories(categories_raw, e->category_key, sizeof(e->category_key));
    snprintf(e->subtitle, sizeof(e->subtitle), "%s", category_label_for_key(e->category_key));
    e->is_favorite = g_hash_table_contains(g_favorites, e->id);
    e->from_desktop = TRUE;
    if (icon_raw[0]) e->icon = xisserve_resolve_icon(icon_raw, XISSERVE_ICON_PX);

    g_ptr_array_add(apps, e);
}

static void scan_dir_desktop_files(const char *dir, GPtrArray *apps, GHashTable *seen)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        size_t nlen = strlen(de->d_name);
        if (nlen < 9 || strcmp(de->d_name + nlen - 8, ".desktop") != 0) continue;
        if (g_hash_table_contains(seen, de->d_name)) continue;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        parse_desktop_file(path, de->d_name, apps);
        g_hash_table_add(seen, g_strdup(de->d_name));
    }
    closedir(d);
}

static gint compare_apps_by_name(gconstpointer a, gconstpointer b)
{
    const ResultEntry *ea = *(const ResultEntry **)a;
    const ResultEntry *eb = *(const ResultEntry **)b;
    return g_utf8_collate(ea->name, eb->name);
}

static gint compare_category_keys_by_label(gconstpointer a, gconstpointer b)
{
    const char *ka = *(const char **)a;
    const char *kb = *(const char **)b;
    return g_utf8_collate(category_label_for_key(ka), category_label_for_key(kb));
}

/* Rebuilds g_cat_store: "Favoritos" and "Todos os Programas" first
 * (always present, even with zero favorites yet), then every real
 * category actually in use among the scanned apps, alphabetized by
 * label. */
static void build_category_store(void)
{
    gtk_list_store_clear(g_cat_store);
    GtkTreeIter it;
    gtk_list_store_append(g_cat_store, &it);
    gtk_list_store_set(g_cat_store, &it, CCOL_KEY, "favorites", CCOL_LABEL, "Favoritos", -1);
    gtk_list_store_append(g_cat_store, &it);
    gtk_list_store_set(g_cat_store, &it, CCOL_KEY, "all", CCOL_LABEL, "Todos os Programas", -1);

    GHashTable *seen_keys = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *keys = g_ptr_array_new();
    for (guint i = 0; i < g_apps->len; i++) {
        ResultEntry *e = g_ptr_array_index(g_apps, i);
        if (!g_hash_table_contains(seen_keys, e->category_key)) {
            g_hash_table_add(seen_keys, e->category_key);
            g_ptr_array_add(keys, e->category_key);
        }
    }
    g_ptr_array_sort(keys, compare_category_keys_by_label);
    for (guint i = 0; i < keys->len; i++) {
        const char *key = g_ptr_array_index(keys, i);
        gtk_list_store_append(g_cat_store, &it);
        gtk_list_store_set(g_cat_store, &it, CCOL_KEY, key, CCOL_LABEL, category_label_for_key(key), -1);
    }
    g_hash_table_destroy(seen_keys);
    g_ptr_array_free(keys, TRUE);
}

/* Home dir first (XDG_DATA_HOME takes priority), then each entry of
 * XDG_DATA_DIRS in order -- basenames already seen are skipped so an
 * earlier, higher-priority directory's copy of a .desktop file wins. */
static void rescan_apps(void)
{
    if (g_apps) {
        for (guint i = 0; i < g_apps->len; i++) result_entry_free(g_ptr_array_index(g_apps, i));
        g_ptr_array_free(g_apps, TRUE);
    }
    g_apps = g_ptr_array_new();
    load_favorites();
    /* load_config() is *not* called here -- show_launcher() does it for
     * every view, not just this one. rescan_apps() only runs for the
     * launcher, so config-reading pages (the audio mixer's scroll step)
     * would otherwise never see a config file at all. */

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    char home_apps[PATH_MAX];
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    if (xdg_data_home && *xdg_data_home) {
        snprintf(home_apps, sizeof(home_apps), "%s/applications", xdg_data_home);
    } else {
        const char *home = getenv("HOME");
        snprintf(home_apps, sizeof(home_apps), "%s/.local/share/applications", home ? home : "");
    }
    scan_dir_desktop_files(home_apps, g_apps, seen);

    const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");
    if (!xdg_data_dirs || !*xdg_data_dirs) xdg_data_dirs = "/usr/local/share:/usr/share";
    char *dirs_copy = g_strdup(xdg_data_dirs);
    char *saveptr = NULL;
    for (char *tok = strtok_r(dirs_copy, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr)) {
        char dirpath[PATH_MAX];
        snprintf(dirpath, sizeof(dirpath), "%s/applications", tok);
        scan_dir_desktop_files(dirpath, g_apps, seen);
    }
    g_free(dirs_copy);
    g_hash_table_destroy(seen);

    g_ptr_array_sort(g_apps, compare_apps_by_name);
    build_category_store();
}

static void hide_launcher(void); /* defined below, alongside the pointer/keyboard grab it releases */

static void launch_iter(GtkTreeModel *model, GtkTreeIter *iter)
{
    ResultEntry *e = NULL;
    gtk_tree_model_get(model, iter, VCOL_ENTRY, &e, -1);
    if (e) {
        if (e->activate_fn) e->activate_fn(e);
        else if (e->exec[0]) run_detached(e->exec);
    }
    hide_launcher();
}

/* ---- theming / positioning --------------------------------------------- */

static void hex_to_rgba(const char *hex, double *r, double *g, double *b, double *a)
{
    unsigned ri = 0, gi = 0, bi = 0, ai = 255;
    if (hex && hex[0] == '#' && strlen(hex) >= 7) {
        sscanf(hex + 1, "%2x%2x%2x", &ri, &gi, &bi);
        if (strlen(hex) >= 9) sscanf(hex + 7, "%2x", &ai);
    }
    *r = ri / 255.0;
    *g = gi / 255.0;
    *b = bi / 255.0;
    *a = ai / 255.0;
}

static gboolean on_window_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    cairo_t *cr = gdk_cairo_create(w->window);
    gdk_cairo_region(cr, ev->region);
    cairo_clip(cr);
    double r, g, b, a;
    hex_to_rgba(g_args.bg, &r, &g, &b, &a);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_paint(cr);
    cairo_destroy(cr);
    return FALSE;
}

static void apply_theme(void)
{
    double r, g, b, a;
    hex_to_rgba(g_args.fg, &r, &g, &b, &a);
    GdkColor fg_color = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    hex_to_rgba(g_args.bg, &r, &g, &b, &a);
    GdkColor bg_color = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};

    gtk_widget_modify_text(g_entry, GTK_STATE_NORMAL, &fg_color);
    gtk_widget_modify_base(g_entry, GTK_STATE_NORMAL, &bg_color);
    gtk_widget_modify_text(g_treeview, GTK_STATE_NORMAL, &fg_color);
    gtk_widget_modify_base(g_treeview, GTK_STATE_NORMAL, &bg_color);
    gtk_widget_modify_text(g_cat_treeview, GTK_STATE_NORMAL, &fg_color);
    gtk_widget_modify_base(g_cat_treeview, GTK_STATE_NORMAL, &bg_color);

    PangoFontDescription *desc = pango_font_description_new();
    pango_font_description_set_family(desc, g_args.font[0] ? g_args.font : "sans-serif");
    if (g_args.font_size > 0) pango_font_description_set_absolute_size(desc, g_args.font_size * PANGO_SCALE);
    gtk_widget_modify_font(g_entry, desc);
    gtk_widget_modify_font(g_treeview, desc);
    gtk_widget_modify_font(g_cat_treeview, desc);

    /* Each page's root gets the same treatment. Only the root is
     * touched: GTK propagates a modified font/bg down to children that
     * haven't overridden it, and a page that wants finer control (the
     * audio mixer colors its own labels, since its rows are rebuilt
     * long after this runs) does that itself via
     * xisserve_get_fg_rgba(). */
    for (int i = 0; i < N_PAGES; i++) {
        if (!g_page_roots[i]) continue;
        gtk_widget_modify_bg(g_page_roots[i], GTK_STATE_NORMAL, &bg_color);
        gtk_widget_modify_text(g_page_roots[i], GTK_STATE_NORMAL, &fg_color);
        gtk_widget_modify_font(g_page_roots[i], desc);
    }
    pango_font_description_free(desc);

    gtk_widget_queue_draw(g_window);
}

/* Glues the popup to the panel's outer edge aligned with the anchor
 * rect, then clamps it inside the *output* rect (the RandR CRTC
 * xispanel's own panel lives on, per PROTOCOL.md's --output-* flags --
 * never the X screen as a whole, which on a multi-monitor setup spans
 * every output combined and would let the window drift onto a different
 * monitor than the one it was anchored on). Must run after
 * apply_view_mode() has settled which widget group is visible:
 * gtk_widget_size_request() below asks "what size do you actually need
 * right now", which for --calendar mode (no forced minimum, see
 * apply_view_mode()) depends entirely on GtkCalendar's own natural size
 * for the current font/locale -- a hardcoded guess here previously sent
 * part of the calendar off-screen whenever the real requisition came out
 * larger than the guess. */
static void reposition_window(void)
{
    GtkRequisition req;
    gtk_widget_size_request(g_window, &req);
    int ww = req.width, wh = req.height;
    int x, y;
    if (strcmp(g_args.edge, "top") == 0) {
        x = g_args.anchor_x;
        y = g_args.anchor_y + g_args.anchor_h;
    } else if (strcmp(g_args.edge, "bottom") == 0) {
        x = g_args.anchor_x;
        y = g_args.anchor_y - wh;
    } else if (strcmp(g_args.edge, "left") == 0) {
        x = g_args.anchor_x + g_args.anchor_w;
        y = g_args.anchor_y;
    } else {
        x = g_args.anchor_x - ww;
        y = g_args.anchor_y;
    }

    int min_x = g_args.output_x, max_x = g_args.output_x + g_args.output_w - ww;
    if (max_x < min_x) max_x = min_x;
    if (x < min_x) x = min_x;
    if (x > max_x) x = max_x;

    int min_y = g_args.output_y, max_y = g_args.output_y + g_args.output_h - wh;
    if (max_y < min_y) max_y = min_y;
    if (y < min_y) y = min_y;
    if (y > max_y) y = max_y;

    gtk_window_move(GTK_WINDOW(g_window), x, y);
}

/* Pointer grab uses owner_events=TRUE: clicks landing on one of our own
 * widgets are reported to that widget as usual (normal GTK event
 * delivery), while clicks on any *other* window on screen -- since
 * nothing else can steal the grab -- are reported to g_window itself,
 * which on_window_button_press() below treats as "clicked outside,
 * dismiss". Same idea GtkMenu's own popups use internally. Keyboard grab
 * is what guarantees this override-redirect window actually receives
 * key events on open, since it isn't WM-managed and so never goes
 * through the normal click-to-focus/WM_TAKE_FOCUS path. */
static void grab_input(void)
{
    guint32 t = gtk_get_current_event_time();
    GdkGrabStatus pg = gdk_pointer_grab(g_window->window, TRUE, GDK_BUTTON_PRESS_MASK, NULL, NULL, t);
    if (pg != GDK_GRAB_SUCCESS) {
        g_warning("xisserve: pointer grab failed (status %d), outside-click-to-close won't work", pg);
    }
    GdkGrabStatus kg = gdk_keyboard_grab(g_window->window, TRUE, t);
    if (kg != GDK_GRAB_SUCCESS) {
        g_warning("xisserve: keyboard grab failed (status %d)", kg);
    }
}

static void ungrab_input(void)
{
    guint32 t = gtk_get_current_event_time();
    gdk_pointer_ungrab(t);
    gdk_keyboard_ungrab(t);
}

/* Puts the pin back to its default (off, labelled "Fixar"). Clears
 * g_pinned *before* touching the button, because un-setting an active
 * toggle emits "toggled", and on_pin_toggled() keys its close-the-window
 * branch off g_pinned still being set -- clearing it first is what stops
 * a close from recursing back into another close. */
static void set_pin_window_mode(gboolean as_dock); /* defined below, with the EWMH reasoning */

static void reset_pin(void)
{
    g_pinned = FALSE;
    if (!g_pin_btn) {
        return;
    }
    gtk_button_set_label(GTK_BUTTON(g_pin_btn), "Fixar");
    gtk_widget_set_tooltip_text(g_pin_btn, "Manter aberto ao clicar fora");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_pin_btn), FALSE);
}

/* The pin control, shared by every view (see g_header).
 *
 * Unpressed ("Fixar", the default) is the popup behavior everything else
 * here is built around: an input grab, and the first click anywhere else
 * dismisses the window. Pressing it drops that grab and suppresses every
 * auto-dismiss path, so the window stays put and other applications can
 * be clicked and typed into normally -- a small always-on-top panel
 * rather than a popup.
 *
 * While pressed it reads "Fechar" and un-pressing it closes the window
 * outright rather than returning to popup mode. That's the useful
 * meaning of the second click: a pinned window is one the user is done
 * with only when they want it gone, and "revert to dismiss-on-next-
 * outside-click" would otherwise leave it hanging around waiting for a
 * stray click to notice. The pin resets to off on every close (see
 * reset_pin(), called from hide_launcher()), so each open starts in the
 * default popup mode.
 *
 * The window stays override-redirect either way rather than being
 * rebuilt as a WM-managed toplevel when pinned. Handing it to the WM
 * mid-session would mean unmapping and remapping it, then re-fighting
 * the placement policy that GTK_WINDOW_POPUP was chosen to avoid in the
 * first place (see the file header and reposition_window()), and would
 * put decorations and taskbar/pager entries in play. The cost is that
 * "always on top" has to be maintained by hand -- an unmanaged window
 * has no _NET_WM_STATE_ABOVE for the WM to honor -- which is what
 * on_window_visibility() below does. */
static void on_pin_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)data;
    if (gtk_toggle_button_get_active(btn)) {
        g_pinned = TRUE;
        gtk_button_set_label(GTK_BUTTON(btn), "Fechar");
        gtk_widget_set_tooltip_text(GTK_WIDGET(btn), "Fechar o xisserve");
        if (GTK_WIDGET_VISIBLE(g_window)) {
            /* Grab first: set_pin_window_mode() unmaps and remaps, and
             * dropping a grab held on a window being unmapped is not
             * something to leave to chance. */
            ungrab_input();
            set_pin_window_mode(TRUE);
        }
        return;
    }
    /* Un-pressed. Either the user clicked "Fechar" (g_pinned still set
     * -- close), or reset_pin() is putting the button back after the
     * window closed some other way (g_pinned already cleared -- nothing
     * left to do). */
    if (g_pinned) {
        hide_launcher();
    }
}

/* Fallback for keeping a pinned window on top when it could *not* be
 * handed to the WM as a dock (see set_pin_window_mode()): an unmanaged
 * window sits in the normal stacking order, so anything raised later
 * covers it, and re-raising whenever it becomes obscured is the standard
 * workaround. Deliberately not used once the window is a managed dock --
 * there the WM owns the stacking, and raising ourselves on top of it
 * would just start a fight with whatever it puts in the same layer
 * (xispanel's own panel, for one). Not a loop: the raise leaves it
 * unobscured, and this only acts on the obscured states. */
static gboolean on_window_visibility(GtkWidget *w, GdkEventVisibility *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (g_pinned && !g_pin_managed && ev->state != GDK_VISIBILITY_UNOBSCURED) {
        gdk_window_raise(g_window->window);
    }
    return FALSE;
}

/* Switches the toplevel between the two window kinds this popup needs.
 *
 * Unpinned it is override-redirect: invisible to the WM, positioned
 * exactly where reposition_window() puts it with no placement policy to
 * fight (which is why GTK_WINDOW_POPUP was chosen -- see the file
 * header). That is right for a popup that lives for one interaction
 * under an input grab, but it is exactly wrong for a pinned window: a
 * WM cannot layer what it does not manage. kiwm skips override-redirect
 * windows outright (client.c's manage(): `if (attr->override_redirect)
 * return;`), so a pinned window never reaches LAYER_DOCK and gets
 * covered as soon as the WM restacks anything else -- the "sometimes it
 * isn't on top" this fixes.
 *
 * Pinned, therefore, the window is handed to the WM as a real
 * _NET_WM_WINDOW_TYPE_DOCK: the same type xispanel's panel uses, so it
 * lands in the same always-on-top layer, by the WM's own rules rather
 * than by us re-raising over everyone.
 *
 * Nothing here is kiwm-specific. _NET_WM_WINDOW_TYPE_DOCK is plain
 * EWMH, and an always-on-top dock layer is what every compliant WM
 * implements -- it's the same mechanism that keeps plasmashell's panel
 * and kickoff above ordinary windows under kwin. Two more standard
 * hints go alongside it so the result degrades sensibly on WMs that
 * layer docks less strictly: _NET_WM_STATE_ABOVE (the explicit
 * "keep above" request, via gtk_window_set_keep_above) and
 * skip-taskbar/skip-pager, which docks are conventionally given anyway
 * and which a pinned popup wants regardless. A WM honoring any one of
 * the three keeps the window up front; on_window_visibility() above
 * still covers the case where the handover didn't take at all.
 *
 * The unmap/remap is required, not incidental: a WM only ever considers
 * a window at MapRequest, and override-redirect windows never send one.
 * Toggling the attribute on a mapped window would leave the WM none the
 * wiser. The hints are all set while unmapped, so they're already on the
 * window when the WM first looks at it. */
static void set_pin_window_mode(gboolean as_dock)
{
    GdkWindow *gw = g_window->window;
    if (!gw) {
        return;
    }
    gboolean visible = GTK_WIDGET_VISIBLE(g_window);
    if (visible) {
        gtk_widget_hide(g_window);
    }
    gdk_window_set_override_redirect(gw, !as_dock);
    gdk_window_set_type_hint(gw, as_dock ? GDK_WINDOW_TYPE_HINT_DOCK : GDK_WINDOW_TYPE_HINT_NORMAL);
    gtk_window_set_keep_above(GTK_WINDOW(g_window), as_dock);
    gtk_window_set_skip_taskbar_hint(GTK_WINDOW(g_window), as_dock);
    gtk_window_set_skip_pager_hint(GTK_WINDOW(g_window), as_dock);
    g_pin_managed = as_dock;
    if (visible) {
        gtk_widget_show(g_window);
        /* The remap is a fresh placement as far as the WM is concerned,
         * so the position has to be reasserted rather than assumed to
         * have survived it. */
        reposition_window();
        gdk_window_raise(gw);
        gdk_window_focus(gw, GDK_CURRENT_TIME);
    }
}

static void leave_current_page(void); /* defined below, next to the page-visibility bookkeeping it owns */

static void hide_launcher(void)
{
    /* A hidden popup is "left" as far as its page is concerned -- the
     * audio mixer's `pactl` poll in particular has no business running
     * against a window nobody can see. */
    leave_current_page();
    ungrab_input();
    gtk_widget_hide(g_window);
    /* Every close returns the pin to its default, whichever way the
     * close happened -- the "Fechar" button, Escape, a click outside, or
     * the panel button. So the next open is always a plain popup with
     * the button reading "Fixar" again, and (via set_pin_window_mode())
     * an override-redirect window again rather than a dock the WM is
     * still tracking. */
    if (g_pin_managed) {
        set_pin_window_mode(FALSE);
    }
    reset_pin();
}

/* ---- search plugins ------------------------------------------------------- */

/* kSearchPlugins/g_plugin_enabled/load_config() live earlier,
 * right after favorites -- see that section's comment. */

/* Builds a "Name\n<small>subtitle</small>" markup string for one
 * result row. */
static gchar *result_markup(const ResultEntry *e)
{
    gchar *name_esc = g_markup_escape_text(e->name, -1);
    gchar *sub_esc = g_markup_escape_text(e->subtitle, -1);
    gchar *markup = g_strdup_printf("%s\n<small>%s</small>", name_esc, sub_esc);
    g_free(name_esc);
    g_free(sub_esc);
    return markup;
}

static void append_result_row(GtkListStore *store, ResultEntry *e)
{
    gchar *markup = result_markup(e);
    GtkTreeIter it;
    gtk_list_store_append(store, &it);
    gtk_list_store_set(store, &it, VCOL_ICON, e->icon, VCOL_MARKUP, markup, VCOL_ENTRY, e, -1);
    g_free(markup);
}

/* The single source of truth for what the right pane shows: search mode
 * (query non-empty) filters every scanned app by name and runs every
 * plugin against the query, full width, category pane hidden; category
 * mode (query empty) lists only g_selected_category's apps, split with
 * the category pane. Called on every keystroke, every category
 * selection/hover change, and after a favorite toggle. */
static void rebuild_results(void)
{
    const char *query = gtk_entry_get_text(GTK_ENTRY(g_entry));
    gboolean searching = query && *query;

    gtk_list_store_clear(g_view_store);

    for (guint i = 0; i < g_plugin_results->len; i++) result_entry_free(g_ptr_array_index(g_plugin_results, i));
    g_ptr_array_set_size(g_plugin_results, 0);

    if (searching) {
        gtk_widget_hide(g_cat_scroll);
        gchar *ql = g_utf8_casefold(query, -1);
        for (guint i = 0; i < g_apps->len; i++) {
            ResultEntry *e = g_ptr_array_index(g_apps, i);
            gchar *nl = g_utf8_casefold(e->name, -1);
            if (strstr(nl, ql)) append_result_row(g_view_store, e);
            g_free(nl);
        }
        g_free(ql);
        for (int i = 0; i < N_SEARCH_PLUGINS; i++) {
            if (g_plugin_enabled[i]) kSearchPlugins[i].search(query, g_plugin_results);
        }
        for (guint i = 0; i < g_plugin_results->len; i++) {
            append_result_row(g_view_store, g_ptr_array_index(g_plugin_results, i));
        }
    } else {
        gtk_widget_show(g_cat_scroll);
        for (guint i = 0; i < g_apps->len; i++) {
            ResultEntry *e = g_ptr_array_index(g_apps, i);
            gboolean include;
            if (strcmp(g_selected_category, "all") == 0) include = TRUE;
            else if (strcmp(g_selected_category, "favorites") == 0) include = e->is_favorite;
            else include = strcmp(e->category_key, g_selected_category) == 0;
            if (include) append_result_row(g_view_store, e);
        }
    }

    GtkTreeIter first;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_view_store), &first)) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_treeview)), &first);
    }
}

/* Shows exactly one of the mutually-exclusive widget groups build_ui()
 * packed into the same vbox: the launcher's entry+content+footer, or a
 * single page's root (see kPages / PROTOCOL.md's mode flags). The
 * launcher keeps its own fixed WIN_WIDTH/WIN_HEIGHT floor (the
 * split-pane layout is designed around it); a page gets whatever floor
 * its kPages row asks for, or none at all (0/0 -- the calendar), in
 * which case the window ends up exactly that widget's natural size for
 * the active font/locale. reposition_window() (called right after this,
 * in show_launcher()) then queries the real resulting size rather than
 * guessing it, which is what actually keeps the popup fully inside its
 * output.
 *
 * Must run *after* gtk_widget_show_all(g_window) in show_launcher():
 * show_all() sets every child visible unconditionally, so these hide()
 * calls have to come later to stick. */
static void apply_view_mode(void)
{
    gboolean launcher = g_args.page == PAGE_LAUNCHER;

    for (int i = 0; i < N_PAGES; i++) {
        if (!g_page_roots[i]) continue;
        gboolean active = !launcher && i == g_args.page;
        if (active) {
            gtk_widget_show(g_page_roots[i]);
        } else {
            gtk_widget_hide(g_page_roots[i]);
        }
    }

    if (launcher) {
        gtk_widget_set_size_request(g_window, WIN_WIDTH, WIN_HEIGHT);
    } else {
        const XisservePage *p = &kPages[g_args.page];
        gtk_widget_set_size_request(g_window, p->min_width ? p->min_width : -1, p->min_height ? p->min_height : -1);
    }
    if (launcher) {
        gtk_widget_show(g_entry);
        gtk_widget_show(g_content_box);
        gtk_widget_show(g_footer_sep);
        gtk_widget_show(g_footer);
    } else {
        gtk_widget_hide(g_entry);
        gtk_widget_hide(g_content_box);
        gtk_widget_hide(g_footer_sep);
        gtk_widget_hide(g_footer);
    }

    /* set_size_request() alone only changes what GTK's layout engine
     * will *ask for* on the next negotiation -- it doesn't shrink an
     * already-mapped, already-allocated toplevel back down by itself
     * (nothing re-triggers that negotiation just because a minimum was
     * lowered). gtk_window_resize() forces the actual window to the
     * size we now know is right, which for GTK_WINDOW_POPUP (override-
     * redirect, no WM to negotiate with) takes effect immediately. */
    GtkRequisition req;
    gtk_widget_size_request(g_window, &req);
    gtk_window_resize(GTK_WINDOW(g_window), req.width, req.height);
}

/* Which page's on_hide() still owes a call -- a page is "left" both by
 * hiding the window and by a later invocation switching to a different
 * page, and only the page itself knows what that should stop (the audio
 * mixer's poll timer, say). Tracked separately from g_args.page because
 * g_args is overwritten by the new invocation's argv before the outgoing
 * page has been told anything. */
static int g_shown_page = PAGE_LAUNCHER;

static void leave_current_page(void)
{
    if (g_shown_page >= 0 && kPages[g_shown_page].on_hide) {
        kPages[g_shown_page].on_hide();
    }
    g_shown_page = PAGE_LAUNCHER;
}

static void show_launcher(void)
{
    if (g_hovered_cat_path) {
        gtk_tree_path_free(g_hovered_cat_path);
        g_hovered_cat_path = NULL;
    }

    leave_current_page();
    load_config(); /* before either branch -- pages read settings too, see rescan_apps() */

    if (g_args.page == PAGE_LAUNCHER) {
        rescan_apps();
        gtk_entry_set_text(GTK_ENTRY(g_entry), "");
        snprintf(g_selected_category, sizeof(g_selected_category), "favorites");

        GtkTreeIter cat_it;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_cat_store), &cat_it)) {
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_cat_treeview)), &cat_it);
        }
        rebuild_results();
    } else {
        /* Before apply_view_mode(): a page's on_show() is what fills it
         * with current data, and the window is sized from the result. */
        if (kPages[g_args.page].on_show) kPages[g_args.page].on_show();
        g_shown_page = g_args.page;
    }

    gtk_widget_show_all(g_window);
    apply_view_mode();
    reposition_window(); /* after apply_view_mode() -- needs its real, now-settled size */
    gtk_window_present(GTK_WINDOW(g_window));
    gdk_window_raise(g_window->window);
    gdk_window_focus(g_window->window, GDK_CURRENT_TIME);
    /* Normally always true: hide_launcher() resets the pin, so an open
     * starts unpinned and grabbing. Guarded anyway rather than calling
     * grab_input() unconditionally, so that a future path which shows an
     * already-pinned window can't silently re-grab it and undo the
     * pinning behind the toggle's back. */
    if (!g_pinned) grab_input();
    gtk_widget_grab_focus(g_args.page == PAGE_LAUNCHER ? g_entry : g_page_roots[g_args.page]);
}

static void toggle_visibility(void)
{
    if (GTK_WIDGET_VISIBLE(g_window)) {
        hide_launcher();
    } else {
        show_launcher();
    }
}

/* Reads /proc/<pid>/comm (trimmed). Returns 0 if the process is gone or
 * /proc isn't available, leaving comm untouched. */
static int read_proc_comm(pid_t pid, char *comm, size_t comm_sz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(comm, comm_sz, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    size_t l = strlen(comm);
    while (l > 0 && (comm[l - 1] == '\n' || comm[l - 1] == '\r')) comm[--l] = 0;
    return 1;
}

static pid_t read_proc_ppid(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[256];
    pid_t ppid = -1;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "PPid:", 5) == 0) {
            sscanf(line + 5, "%d", &ppid);
            break;
        }
    }
    fclose(f);
    return ppid;
}

/* xispanel's run_detached() forks a single "sh -c '<cmd>'" child and
 * never waits on it. Some shells tail-call-exec the last command of a
 * -c string, which would make that child become xisserve itself
 * (getppid() then reads as xispanel's own PID directly) -- but that
 * optimization isn't guaranteed, and in practice the shell here stays
 * alive as an intermediary, blocked waiting on us, so our direct parent
 * is that shell and never changes even after xispanel dies. Climb past
 * a shell-named direct parent to the grandparent, which is xispanel. */
static pid_t resolve_watch_pid(void)
{
    pid_t p = getppid();
    char comm[64];
    if (read_proc_comm(p, comm, sizeof(comm)) &&
        (strcmp(comm, "sh") == 0 || strcmp(comm, "bash") == 0 || strcmp(comm, "dash") == 0 ||
         strcmp(comm, "ash") == 0)) {
        pid_t gp = read_proc_ppid(p);
        if (gp > 0) p = gp;
    }
    return p;
}

/* Polled rather than event-driven: there's no portable "notify me when
 * this other process exits" primitive (Linux's PR_SET_PDEATHSIG only
 * covers one's own direct parent, which per resolve_watch_pid() above
 * isn't reliably xispanel here anyway, and still just delivers a signal
 * we'd have to poll for regardless). */
static gboolean check_parent_alive(gpointer data)
{
    (void)data;
    if (kill(g_watch_pid, 0) != 0 && errno == ESRCH) {
        gtk_main_quit();
    }
    return TRUE;
}

/* ---- power actions -------------------------------------------------------- */

typedef struct {
    const char *label;
    const char *probe_bin; /* must resolve via $PATH for this button to be shown; NULL = always shown */
    const char *cmd;       /* shell command run (after confirmation) on click; NULL = stub, no action yet */
    const char *confirm_msg;
} PowerAction;

/* systemd-logind's `loginctl`/`systemctl` cover shutdown/reboot/suspend/
 * lock on any systemd system without needing a desktop-specific tool;
 * "Trocar usuario" only has a real answer under LightDM's `dm-tool`, and
 * "Sair" (logout) has no generic single command at all for a
 * WM-agnostic session like this one -- both ship as visible buttons per
 * the ask, the latter always shown (cmd left NULL, a stub), the former
 * hidden unless dm-tool is actually installed. */
static const PowerAction kPowerActions[] = {
    {"Desligar", "systemctl", "systemctl poweroff", "Desligar o computador agora?"},
    {"Reiniciar", "systemctl", "systemctl reboot", "Reiniciar o computador agora?"},
    {"Suspender", "systemctl", "systemctl suspend", "Suspender o computador agora?"},
    {"Sair", NULL, NULL, "Encerrar a sessao atual?"},
    {"Trocar usuario", "dm-tool", "dm-tool switch-to-greeter", "Trocar de usuario agora?"},
    {"Bloquear tela", "loginctl", "loginctl lock-session", "Bloquear a tela agora?"},
};
#define N_POWER_ACTIONS ((int)(sizeof(kPowerActions) / sizeof(kPowerActions[0])))

static void on_power_button_clicked(GtkWidget *btn, gpointer user_data)
{
    (void)btn;
    const PowerAction *action = (const PowerAction *)user_data;
    hide_launcher();

    GtkWidget *dialog = gtk_message_dialog_new(NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s",
                                                action->confirm_msg);
    gtk_window_set_title(GTK_WINDOW(dialog), action->label);
    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if (resp == GTK_RESPONSE_YES && action->cmd) {
        run_detached(action->cmd);
    }
}

/* ---- control socket I/O -------------------------------------------------- */

static gboolean on_ctl_accept(GIOChannel *source, GIOCondition cond, gpointer data)
{
    (void)cond;
    (void)data;
    int listenfd = g_io_channel_unix_get_fd(source);
    int cfd = accept(listenfd, NULL, NULL);
    if (cfd < 0) return TRUE;

    char buf[1024];
    size_t len = 0;
    while (len + 1 < sizeof(buf)) {
        ssize_t n = recv(cfd, buf + len, sizeof(buf) - 1 - len, 0);
        if (n <= 0) break;
        len += (size_t)n;
        if (memchr(buf, '\n', len)) break;
    }
    buf[len] = 0;
    close(cfd);

    LaunchArgs newargs;
    if (parse_json_args(buf, &newargs)) {
        g_args = newargs;
        apply_theme();
        /* reposition_window() is no longer called standalone here -- it
         * needs the current view mode's real widget sizing settled
         * first (apply_view_mode(), inside show_launcher()) to clamp
         * correctly, and repositioning a window that's about to be
         * hidden anyway (the other toggle_visibility() branch) would be
         * wasted work besides. */
        toggle_visibility();
    }
    return TRUE;
}

/* ---- UI callbacks --------------------------------------------------------- */

static void on_entry_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    (void)data;
    rebuild_results();
}

static void on_entry_activate(GtkEntry *entry, gpointer data)
{
    (void)entry;
    (void)data;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(g_treeview));
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_treeview));
    GtkTreeIter iter;
    if (gtk_tree_selection_get_selected(sel, NULL, &iter)) {
        launch_iter(model, &iter);
        return;
    }
    if (gtk_tree_model_get_iter_first(model, &iter)) launch_iter(model, &iter);
}

static gboolean on_entry_key_press(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    if (ev->keyval == GDK_Escape) {
        hide_launcher();
        return TRUE;
    }
    if (ev->keyval == GDK_Up || ev->keyval == GDK_Down) {
        GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_treeview));
        GtkTreeModel *model = NULL;
        GtkTreeIter iter;
        GtkTreePath *path;
        if (!gtk_tree_selection_get_selected(sel, &model, &iter)) {
            model = gtk_tree_view_get_model(GTK_TREE_VIEW(g_treeview));
            if (!gtk_tree_model_get_iter_first(model, &iter)) return TRUE;
            path = gtk_tree_model_get_path(model, &iter);
        } else {
            path = gtk_tree_model_get_path(model, &iter);
            if (ev->keyval == GDK_Down) gtk_tree_path_next(path);
            else gtk_tree_path_prev(path);
        }
        if (gtk_tree_model_get_iter(model, &iter, path)) {
            gtk_tree_selection_select_iter(sel, &iter);
            gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_treeview), path, NULL, FALSE, 0, 0);
        }
        gtk_tree_path_free(path);
        return TRUE;
    }
    return FALSE;
}

static void on_favorite_menu_item(GtkWidget *item, gpointer user_data)
{
    (void)item;
    ResultEntry *e = (ResultEntry *)user_data;
    toggle_favorite(e->id);
    rebuild_results();
}

/* Set for the duration of our own right-click context menu -- see
 * on_window_grab_broken()'s comment for why this needs to be
 * distinguishable from a *foreign* grab theft. */
static gboolean g_context_menu_active = FALSE;

/* GtkMenu's own popup takes the X pointer/keyboard grab while shown,
 * superseding grab_input()'s explicit gdk_pointer_grab/gdk_keyboard_grab
 * on g_window (only one active grab can exist at a time) -- reclaim it
 * once the menu interaction ends (item picked or dismissed) so outside-
 * click-to-close and keyboard routing keep working afterwards. Also
 * frees the menu, which gtk_menu_popup() otherwise leaves to us. */
static void on_context_menu_selection_done(GtkWidget *menu, gpointer data)
{
    (void)data;
    gtk_widget_destroy(menu);
    g_context_menu_active = FALSE;
    if (GTK_WIDGET_VISIBLE(g_window)) grab_input();
}

/* Right-click on a real (from_desktop) result offers "Adicionar aos
 * Favoritos"/"Remover dos Favoritos" -- plugin-synthetic results (no
 * stable id to persist) don't get the menu at all. Left-click launches,
 * same as before. */
static gboolean on_tree_button_press(GtkWidget *tv, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->type != GDK_BUTTON_PRESS || (ev->button != 1 && ev->button != 3)) return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv), (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL)) {
        return FALSE;
    }
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_path_free(path);
        return FALSE;
    }

    if (ev->button == 1) {
        launch_iter(model, &iter);
        gtk_tree_path_free(path);
        return FALSE;
    }

    /* button == 3 */
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), &iter);
    ResultEntry *e = NULL;
    gtk_tree_model_get(model, &iter, VCOL_ENTRY, &e, -1);
    gtk_tree_path_free(path);
    if (!e || !e->from_desktop) return TRUE;

    GtkWidget *menu = gtk_menu_new();
    GtkWidget *item = gtk_menu_item_new_with_label(e->is_favorite ? "Remover dos Favoritos" : "Adicionar aos Favoritos");
    g_signal_connect(item, "activate", G_CALLBACK(on_favorite_menu_item), e);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    g_signal_connect(menu, "selection-done", G_CALLBACK(on_context_menu_selection_done), NULL);
    gtk_widget_show_all(menu);
    g_context_menu_active = TRUE;
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

/* Fires both for genuine outside clicks (owner_events=TRUE reports those
 * to the grab window, i.e. us, per grab_input()'s comment) and for
 * clicks landing on g_window's own background between child widgets
 * (the vbox has no GdkWindow of its own, so those land here too) --
 * only the former should close the popup, hence the bounds check. */
static gboolean on_window_button_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->type != GDK_BUTTON_PRESS) return FALSE;
    gboolean outside =
        ev->x < 0 || ev->y < 0 || ev->x >= w->allocation.width || ev->y >= w->allocation.height;
    if (outside) {
        /* Pinned: clicking elsewhere is meant to go to that other
         * window, not dismiss this one. (With no grab active this
         * handler barely sees outside clicks anyway, but a click can
         * still land here in the window between unpinning and the grab
         * being re-established.) */
        if (g_pinned) return FALSE;
        hide_launcher();
        return TRUE;
    }
    /* Clicked inside while pinned: take the keyboard back. Nothing else
     * will hand it over -- an override-redirect window is invisible to
     * the WM's click-to-focus, and while pinned there's no keyboard grab
     * routing keys here either, so without this the search entry and
     * every other control would be unusable after focusing another
     * application. */
    if (g_pinned) {
        gdk_window_focus(g_window->window, ev->time);
    }
    return FALSE;
}

/* The WM or another client can steal an active grab out from under us
 * (e.g. a different app opening its own grabbing popup); when that
 * happens we're no longer guaranteed input focus or outside-click
 * detection, so just close rather than linger in a half-working state.
 * BUT our own right-click context menu breaks our grab exactly the same
 * way (GtkMenu's popup takes its own grab while shown) -- that case is
 * expected and already handled by on_context_menu_selection_done()
 * reclaiming the grab once the menu closes, so it must NOT hide us here
 * too, or the main window vanishes the instant the context menu opens,
 * leaving only the little menu on screen with nothing behind it. */
static gboolean on_window_grab_broken(GtkWidget *w, GdkEventGrabBroken *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    if (g_context_menu_active) return FALSE;
    /* Pinned windows hold no grab by design, so losing one is not the
     * "we've been left in a half-working state" signal it otherwise is
     * -- it's just the expected consequence of pinning. */
    if (g_pinned) return FALSE;
    gtk_widget_hide(g_window);
    return FALSE;
}

/* Fires on click and on keyboard nav in the category list -- reads
 * whichever row ended up selected into g_selected_category and rebuilds
 * the right pane for it. */
static void on_category_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)data;
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter)) return;
    gchar *key = NULL;
    gtk_tree_model_get(model, &iter, CCOL_KEY, &key, -1);
    if (key) {
        snprintf(g_selected_category, sizeof(g_selected_category), "%s", key);
        g_free(key);
        rebuild_results();
    }
}

/* Hovering a category live-previews it (matches krunner/kickoff-style
 * category panes) -- selecting the row under the pointer piggybacks on
 * on_category_selection_changed() above rather than rebuilding here
 * directly. Only acts when the hovered row actually changes, so this
 * doesn't rebuild on every pixel of mouse movement within one row. */
static gboolean on_category_motion(GtkWidget *tv, GdkEventMotion *ev, gpointer data)
{
    (void)data;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(tv), (int)ev->x, (int)ev->y, &path, NULL, NULL, NULL)) {
        return FALSE;
    }
    if (g_hovered_cat_path && gtk_tree_path_compare(g_hovered_cat_path, path) == 0) {
        gtk_tree_path_free(path);
        return FALSE;
    }
    if (g_hovered_cat_path) gtk_tree_path_free(g_hovered_cat_path);
    g_hovered_cat_path = path; /* ownership taken */

    GtkTreeIter iter;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tv));
    if (gtk_tree_model_get_iter(model, &iter, path)) {
        gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)), &iter);
    }
    return FALSE;
}

static void build_ui(void)
{
    g_window = gtk_window_new(GTK_WINDOW_POPUP);
    gtk_widget_set_size_request(g_window, WIN_WIDTH, WIN_HEIGHT);
    /* GTK_WINDOW_POPUP is override-redirect (no WM decorations, so no
     * drag-to-resize border of its own), but resizable is otherwise an
     * independent property -- explicit here so a WM that resizes
     * windows by some other means regardless of decoration (e.g. kiwm's
     * own corner-resize) isn't refused by GTK on our end. */
    gtk_window_set_resizable(GTK_WINDOW(g_window), TRUE);
    gtk_widget_add_events(g_window, GDK_BUTTON_PRESS_MASK | GDK_VISIBILITY_NOTIFY_MASK);

    GdkScreen *screen = gtk_widget_get_screen(g_window);
    GdkColormap *cmap = gdk_screen_get_rgba_colormap(screen);
    if (cmap) gtk_widget_set_colormap(g_window, cmap);
    gtk_widget_set_app_paintable(g_window, TRUE);

    g_signal_connect(g_window, "expose-event", G_CALLBACK(on_window_expose), NULL);
    g_signal_connect(g_window, "button-press-event", G_CALLBACK(on_window_button_press), NULL);
    g_signal_connect(g_window, "grab-broken-event", G_CALLBACK(on_window_grab_broken), NULL);
    g_signal_connect(g_window, "visibility-notify-event", G_CALLBACK(on_window_visibility), NULL);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
    gtk_container_add(GTK_CONTAINER(g_window), vbox);

    /* Shared header: packed first and never touched by
     * apply_view_mode(), so it's the one strip common to the launcher
     * and to every page -- which is what lets the pin be a single
     * toggle rather than one copy per view. Right-aligned so it stays
     * out of the way of whatever each view puts below it. */
    g_header = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), g_header, FALSE, FALSE, 0);
    g_pin_btn = gtk_toggle_button_new_with_label("Fixar");
    gtk_widget_set_tooltip_text(g_pin_btn, "Manter aberto ao clicar fora");
    g_signal_connect(g_pin_btn, "toggled", G_CALLBACK(on_pin_toggled), NULL);
    gtk_box_pack_end(GTK_BOX(g_header), g_pin_btn, FALSE, FALSE, 0);

    g_entry = gtk_entry_new();
    g_signal_connect(g_entry, "changed", G_CALLBACK(on_entry_changed), NULL);
    g_signal_connect(g_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(g_entry, "key-press-event", G_CALLBACK(on_entry_key_press), NULL);
    gtk_box_pack_start(GTK_BOX(vbox), g_entry, FALSE, FALSE, 0);

    g_content_box = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), g_content_box, TRUE, TRUE, 0);

    /* Left pane: categories. Hidden while searching (rebuild_results()
     * toggles it) so the results pane can take the full width, matching
     * how a flat search result list looked before this split. */
    g_cat_store = gtk_list_store_new(N_CCOLS, G_TYPE_STRING, G_TYPE_STRING);

    g_cat_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_cat_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_cat_treeview), FALSE);
    GtkCellRenderer *cat_rend = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *cat_col = gtk_tree_view_column_new_with_attributes("Categoria", cat_rend, "text", CCOL_LABEL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_cat_treeview), cat_col);
    gtk_widget_add_events(g_cat_treeview, GDK_POINTER_MOTION_MASK);
    g_signal_connect(g_cat_treeview, "motion-notify-event", G_CALLBACK(on_category_motion), NULL);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(g_cat_treeview)), "changed",
                      G_CALLBACK(on_category_selection_changed), NULL);

    g_cat_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_cat_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(g_cat_scroll, CAT_PANE_WIDTH, -1);
    gtk_container_add(GTK_CONTAINER(g_cat_scroll), g_cat_treeview);
    gtk_box_pack_start(GTK_BOX(g_content_box), g_cat_scroll, FALSE, FALSE, 0);

    /* Right pane: results. One column packs an icon renderer (the app's
     * own icon, or a plugin's -- see xisserve_resolve_icon()/
     * ResultEntry::icon) beside a markup renderer for "Name" plus a
     * smaller subtitle line (category, or the plugin name for a
     * plugin-synthetic row) -- see result_markup(). */
    g_view_store = gtk_list_store_new(N_VCOLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_POINTER);

    g_treeview = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_view_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_treeview), FALSE);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *icon_rend = gtk_cell_renderer_pixbuf_new();
    gtk_tree_view_column_pack_start(col, icon_rend, FALSE);
    gtk_tree_view_column_add_attribute(col, icon_rend, "pixbuf", VCOL_ICON);
    GtkCellRenderer *rend = gtk_cell_renderer_text_new();
    gtk_tree_view_column_pack_start(col, rend, TRUE);
    gtk_tree_view_column_add_attribute(col, rend, "markup", VCOL_MARKUP);
    gtk_tree_view_column_set_title(col, "Programa");
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_treeview), col);
    g_signal_connect(g_treeview, "button-press-event", G_CALLBACK(on_tree_button_press), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_treeview);
    gtk_box_pack_start(GTK_BOX(g_content_box), scroll, TRUE, TRUE, 0);

    g_footer_sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(vbox), g_footer_sep, FALSE, FALSE, 0);

    g_footer = gtk_hbox_new(TRUE, 2);
    for (int i = 0; i < N_POWER_ACTIONS; i++) {
        const PowerAction *action = &kPowerActions[i];
        if (action->probe_bin) {
            gchar *found = g_find_program_in_path(action->probe_bin);
            if (!found) continue;
            g_free(found);
        }
        GtkWidget *btn = gtk_button_new_with_label(action->label);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_power_button_clicked), (gpointer)action);
        gtk_box_pack_start(GTK_BOX(g_footer), btn, TRUE, TRUE, 0);
    }
    gtk_box_pack_start(GTK_BOX(vbox), g_footer, FALSE, FALSE, 0);

    /* Every page's root goes into the same vbox as the launcher's own
     * widgets, all of them normally hidden -- apply_view_mode() shows
     * exactly one group. Built once, up front, rather than lazily on
     * first use: a page's build() is cheap (no data is fetched there --
     * that's on_show()'s job) and this keeps apply_theme() able to
     * assume every root already exists. */
    for (int i = 0; i < N_PAGES; i++) {
        g_page_roots[i] = kPages[i].build();
        gtk_box_pack_start(GTK_BOX(vbox), g_page_roots[i], TRUE, TRUE, 0);
    }
}

int main(int argc, char **argv)
{
    /* Checked before gtk_init() -- same reason most CLI tools handle
     * these first: they should work even with no display to connect to,
     * and shouldn't care whether any other flag is well-formed.
     *
     * --help is handled here rather than left to parse_argv()'s
     * ignore-unknown-flags rule (see its comment): that rule exists so a
     * mode flag from a newer xispanel doesn't stop the popup opening,
     * but applying it to --help would mean "xisserve --help" silently
     * opening the launcher instead of printing anything, which is
     * useless at a terminal. No widget ever passes --help, so carving it
     * out costs the contract nothing. */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0) {
            printf("xisserve %s\n", XISSERVE_VERSION);
            return 0;
        }
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    gtk_init(&argc, &argv);

    LaunchArgs args;
    if (parse_argv(argc, argv, &args) != 0) {
        usage(argv[0]);
        return 1;
    }

    /* --menu is a one-shot popup for another process's window (see
     * appmenu.c), so it stops here: no singleton lock, no control
     * socket, no launcher window -- and no disturbing a launcher that
     * happens to be running. */
    if (args.menu_mode)
        return appmenu_run(args.menu_window, args.menu_x, args.menu_y);

    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) rundir = "/tmp";
    char lockpath[PATH_MAX], sockpath[PATH_MAX];
    snprintf(lockpath, sizeof(lockpath), "%s/xisserve.lock", rundir);
    snprintf(sockpath, sizeof(sockpath), "%s/xisserve.sock", rundir);

    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (lockfd < 0) {
        perror("xisserve: open lock");
        return 1;
    }

    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        close(lockfd);
        if (send_to_running(sockpath, &args) != 0) {
            fprintf(stderr, "xisserve: another instance appears to be running but is not reachable at %s\n",
                    sockpath);
            return 1;
        }
        return 0;
    }

    /* We hold the lock: this invocation becomes the singleton daemon. */
    g_watch_pid = resolve_watch_pid();
    g_timeout_add_seconds(2, check_parent_alive, NULL);

    g_plugin_results = g_ptr_array_new();
    g_args = args;
    build_ui();
    apply_theme();
    /* Positioning happens inside show_launcher() (reposition_window(),
     * after apply_view_mode() settles the real size for whichever mode
     * this invocation asked for) rather than here. */

    int listenfd = open_listen_socket(sockpath);
    if (listenfd < 0) {
        fprintf(stderr,
                "xisserve: failed to open control socket %s, later invocations won't be able to "
                "toggle this instance\n",
                sockpath);
    } else {
        GIOChannel *chan = g_io_channel_unix_new(listenfd);
        g_io_add_watch(chan, G_IO_IN, on_ctl_accept, NULL);
        g_io_channel_unref(chan);
    }

    show_launcher();
    gtk_main();

    unlink(sockpath);
    flock(lockfd, LOCK_UN);
    close(lockfd);
    return 0;
}

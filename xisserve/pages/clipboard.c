/*
 * clipboard.c - the --clipboard page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section): kimemoryd's clipboard history, searchable, filtered to
 * the target window's app/window/document when asked, with paste, copy,
 * favourite and remove.
 *
 * History, provenance and the clipboard itself all live in kimemoryd
 * (../../kimemory/); this page is a plain client of its control socket
 * ($XDG_RUNTIME_DIR/kimemory-ctl.<display>.sock, one JSON line each way),
 * the same way pages/notifications.c is a client of xispanel's. Own small
 * flat-JSON reader here for the same "duplicated on purpose" reason that
 * file gives: one item object per line, string/int values only.
 *
 * The target window (xisserve_target_window()) is the one that was active
 * when xisserve was invoked. "Colar" asks kimemoryd to put the item on the
 * clipboard (it becomes the owner, so the paste is recorded against the
 * window that takes it), closes the popup, waits for the target to have
 * focus again and sends it Ctrl+V through XTest -- Ctrl+Shift+V for
 * terminals that paste that way, nothing for xterm/urxvt, which don't
 * paste CLIPBOARD from the keyboard at all. xisserve.conf's
 * "CLIPBOARD\tautopaste\t0" leaves the Ctrl+V to the user.
 */
#include "../xisserve.h"

#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <gdk/gdkkeysyms.h>
#include <gdk/gdkx.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CLIP_POLL_MS 2000
#define SEARCH_DEBOUNCE_MS 150
#define THUMB_PX 48
#define PREVIEW_CHARS 90

enum { SCOPE_ALL, SCOPE_APP, SCOPE_WINDOW, SCOPE_DOC, N_SCOPES };
static const char *SCOPE_NAMES[N_SCOPES] = { "all", "app", "window", "doc" };
static const char *SCOPE_LABELS[N_SCOPES] = { "Tudo", "App", "Janela", "Documento" };

enum { COL_ICON, COL_MARKUP, COL_ID, COL_FAV, COL_TYPE, N_COLS };

static GtkWidget *g_root;
static GtkWidget *g_entry;
static GtkWidget *g_scope_btn[N_SCOPES];
static GtkWidget *g_context;
static GtkWidget *g_tree;
static GtkListStore *g_store;
static GtkWidget *g_status;
static int g_scope = SCOPE_ALL;
static guint g_poll_id, g_search_id, g_paste_id;
static long g_last_count = -1, g_last_current = -1;
static GHashTable *g_thumbs;   /* item id -> GdkPixbuf thumbnail */

/* Autopaste in flight: the window to paste into and when we gave up. */
static struct {
    Window target;
    int shift;         /* Ctrl+Shift+V instead of Ctrl+V */
    gint64 deadline;
    int asked_focus;
} g_paste;

/* ---- kimemory-ctl client ---------------------------------------------- */

static Display *xdpy(void)
{
    return GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
}

/* The target is another program's window and can vanish at any moment
 * (the poll below keeps asking about it): every X call on it goes through
 * an error trap, or GTK's default handler kills xisserve on BadWindow. */
static gboolean target_class(Window w, char *res_class, size_t cls_sz, char *res_name, size_t name_sz)
{
    XClassHint ch;
    res_class[0] = res_name[0] = '\0';
    gdk_error_trap_push();
    Status ok = XGetClassHint(xdpy(), w, &ch);
    gint err = gdk_error_trap_pop();
    if (!ok || err)
        return FALSE;
    snprintf(res_class, cls_sz, "%s", ch.res_class ? ch.res_class : "");
    snprintf(res_name, name_sz, "%s", ch.res_name ? ch.res_name : "");
    if (ch.res_name) XFree(ch.res_name);
    if (ch.res_class) XFree(ch.res_class);
    return TRUE;
}

static void ctl_sockpath(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    const char *d = getenv("DISPLAY");
    const char *colon = d ? strrchr(d, ':') : NULL;
    snprintf(out, outsz, "%s/kimemory-ctl.%d.sock", rundir && *rundir ? rundir : "/tmp",
             colon ? atoi(colon + 1) : 0);
}

/* One round trip; malloc'd reply or NULL when kimemoryd isn't there. */
static char *ctl_request(const char *req)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    ctl_sockpath(addr.sun_path, sizeof(addr.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || write(fd, req, strlen(req)) < 0 ||
        write(fd, "\n", 1) < 0) {
        close(fd);
        return NULL;
    }
    size_t len = 0, cap = 65536;
    char *buf = g_malloc(cap);
    ssize_t n;
    while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 == cap)
            buf = g_realloc(buf, cap *= 2);
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

static const char *json_find(const char *msg, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(msg, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ')
        p++;
    return p;
}

static gboolean json_str(const char *msg, const char *key, char *out, size_t cap)
{
    const char *p = json_find(msg, key);
    out[0] = '\0';
    if (!p || *p != '"')
        return FALSE;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
            if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                c = (char)strtol(hex, NULL, 16);
                p += 4;
            }
            out[o++] = c;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
    return TRUE;
}

static long json_long(const char *msg, const char *key, long fallback)
{
    const char *p = json_find(msg, key);
    return p && (*p == '-' || (*p >= '0' && *p <= '9')) ? strtol(p, NULL, 10) : fallback;
}

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c >= 0x20) {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
}

static gboolean simple_cmd(const char *fmt, long id)
{
    char req[128];
    snprintf(req, sizeof(req), fmt, id);
    char *resp = ctl_request(req);
    gboolean ok = resp && strstr(resp, "\"ok\":true");
    g_free(resp);
    return ok;
}

/* ---- list ------------------------------------------------------------- */

static void style_fg(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    GdkColor c = { 0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535) };
    gtk_widget_modify_fg(w, GTK_STATE_NORMAL, &c);
    gtk_widget_modify_text(w, GTK_STATE_NORMAL, &c);
}

static void style_bg_base(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    GdkColor c = { 0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535) };
    gtk_widget_modify_base(w, GTK_STATE_NORMAL, &c);
}

static GdkPixbuf *thumb_for(long id)
{
    gpointer cached = g_hash_table_lookup(g_thumbs, GINT_TO_POINTER((int)id));
    if (cached)
        return g_object_ref(cached);
    char req[64], path[PATH_MAX];
    snprintf(req, sizeof(req), "{\"cmd\":\"GET\",\"id\":%ld}", id);
    char *resp = ctl_request(req);
    GdkPixbuf *pb = NULL;
    if (resp && json_str(resp, "file", path, sizeof(path)) && path[0])
        pb = gdk_pixbuf_new_from_file_at_scale(path, THUMB_PX, THUMB_PX, TRUE, NULL);
    g_free(resp);
    if (!pb)
        return xisserve_resolve_icon("image-x-generic", THUMB_PX);
    g_hash_table_insert(g_thumbs, GINT_TO_POINTER((int)id), g_object_ref(pb));
    return pb;
}

static GdkPixbuf *icon_for_type(const char *type)
{
    const char *name = !strcmp(type, "link") ? "text-html" : !strcmp(type, "files") ? "folder" : "text-x-generic";
    return xisserve_resolve_icon(name, XISSERVE_ICON_PX);
}

/* "14:02" today, "29/09 14:02" otherwise. */
static void short_time(long ts, char *out, size_t outsz)
{
    time_t t = (time_t)ts, now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    strftime(out, outsz, a.tm_yday == b.tm_yday && a.tm_year == b.tm_year ? "%H:%M" : "%d/%m %H:%M", &a);
}

static char *row_markup(const char *line, const char *type)
{
    char preview[1024], src[128], src_doc[256], dsts[512], when[32];
    json_str(line, "preview", preview, sizeof(preview));
    json_str(line, "src_app", src, sizeof(src));
    json_str(line, "src_doc", src_doc, sizeof(src_doc));
    json_str(line, "dst_apps", dsts, sizeof(dsts));
    short_time(json_long(line, "ts", 0), when, sizeof(when));
    long fav = json_long(line, "fav", 0), bytes = json_long(line, "bytes", 0);

    /* One line: newlines/tabs flattened, cut at PREVIEW_CHARS characters. */
    for (char *p = preview; *p; p++)
        if (*p == '\n' || *p == '\t' || *p == '\r')
            *p = ' ';
    char *first;
    if (!strcmp(type, "image")) {
        first = g_strdup_printf("Imagem \xc2\xb7 %ld KiB", (bytes + 1023) / 1024);
    } else {
        glong chars = g_utf8_strlen(preview, -1);
        char *cut = g_utf8_substring(g_strstrip(preview), 0, MIN(chars, PREVIEW_CHARS));
        first = g_strdup_printf("%s%s", cut, chars > PREVIEW_CHARS ? "\xe2\x80\xa6" : "");
        g_free(cut);
    }
    char *first_esc = g_markup_escape_text(first, -1);
    GString *meta = g_string_new(NULL);
    g_string_append_printf(meta, "de %s", src[0] ? src : "?");
    if (src_doc[0])
        g_string_append_printf(meta, ": %s", src_doc);
    g_string_append_printf(meta, " \xc2\xb7 %s", when);
    if (dsts[0])
        g_string_append_printf(meta, "  \xe2\x86\x92 %s", dsts);
    char *meta_esc = g_markup_escape_text(meta->str, -1);
    char *markup = g_strdup_printf("%s%s\n<small>%s</small>", fav ? "\xe2\x98\x85 " : "", first_esc, meta_esc);
    g_free(first);
    g_free(first_esc);
    g_free(meta_esc);
    g_string_free(meta, TRUE);
    return markup;
}

static long selected_id(void)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_tree));
    GtkTreeModel *model;
    GtkTreeIter it;
    guint id = 0;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, COL_ID, &id, -1);
    return (long)id;
}

static void select_id(long id)
{
    GtkTreeIter it;
    GtkTreeModel *model = GTK_TREE_MODEL(g_store);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_tree));
    gboolean ok = gtk_tree_model_get_iter_first(model, &it);
    gboolean first = ok;
    GtkTreeIter first_it = it;
    while (ok) {
        guint rid;
        gtk_tree_model_get(model, &it, COL_ID, &rid, -1);
        if ((long)rid == id) {
            gtk_tree_selection_select_iter(sel, &it);
            return;
        }
        ok = gtk_tree_model_iter_next(model, &it);
    }
    if (first)
        gtk_tree_selection_select_iter(sel, &first_it);
}

static void set_status(const char *text)
{
    gtk_label_set_text(GTK_LABEL(g_status), text);
}

static void update_context(const char *resp)
{
    char app[128] = "", doc[256] = "", title[256] = "";
    if (resp) {
        json_str(resp, "for_app", app, sizeof(app));
        json_str(resp, "for_doc", doc, sizeof(doc));
        json_str(resp, "for_title", title, sizeof(title));
    }
    if (!app[0] && xisserve_target_window()) {
        /* Scope "all" doesn't make kimemoryd describe the target: ask X. */
        char name[128];
        target_class(xisserve_target_window(), app, sizeof(app), name, sizeof(name));
    }
    char *text = app[0] ? g_strdup_printf("Colar em %s%s%s", app, doc[0] || title[0] ? ": " : "",
                                          doc[0] ? doc : title)
                        : g_strdup("Nenhuma janela de destino");
    gtk_label_set_text(GTK_LABEL(g_context), text);
    g_free(text);
}

static void reload(void)
{
    long keep = selected_id();
    const char *query = gtk_entry_get_text(GTK_ENTRY(g_entry));
    char qesc[512];
    json_escape(qesc, sizeof(qesc), query);
    char req[800];
    snprintf(req, sizeof(req), "{\"cmd\":\"LIST\",\"scope\":\"%s\",\"win\":\"%lu\",\"query\":\"%s\",\"limit\":%d}",
             SCOPE_NAMES[g_scope], xisserve_target_window(), qesc,
             xisserve_config_get_int("CLIPBOARD", "max_items", 200));
    char *resp = ctl_request(req);

    gtk_list_store_clear(g_store);
    if (!resp) {
        set_status("kimemoryd n\xc3\xa3o est\xc3\xa1 rodando");
        update_context(NULL);
        return;
    }
    if (!strstr(resp, "\"ok\":true")) {
        char err[128];
        json_str(resp, "error", err, sizeof(err));
        set_status(err[0] ? err : "erro");
        update_context(NULL);
        g_free(resp);
        return;
    }
    update_context(resp);

    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(resp, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *obj = strstr(line, "{\"id\":");
        if (!obj)
            continue;
        char type[16];
        json_str(obj, "type", type, sizeof(type));
        long id = json_long(obj, "id", 0);
        GdkPixbuf *icon = !strcmp(type, "image") ? thumb_for(id) : icon_for_type(type);
        char *markup = row_markup(obj, type);
        GtkTreeIter it;
        gtk_list_store_append(g_store, &it);
        gtk_list_store_set(g_store, &it, COL_ICON, icon, COL_MARKUP, markup, COL_ID, (guint)id, COL_FAV,
                           (int)json_long(obj, "fav", 0), COL_TYPE, type, -1);
        if (icon)
            g_object_unref(icon);
        g_free(markup);
        count++;
    }
    g_free(resp);
    char status[64];
    snprintf(status, sizeof(status), count == 1 ? "1 item" : "%d itens", count);
    set_status(status);
    select_id(keep);
}

/* ---- paste ------------------------------------------------------------ */

static void send_key(Display *dpy, KeySym sym, Bool down)
{
    KeyCode kc = XKeysymToKeycode(dpy, sym);
    if (kc)
        XTestFakeKeyEvent(dpy, kc, down, CurrentTime);
}

static Window active_window(Display *dpy)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    Window w = None;
    if (XGetWindowProperty(dpy, DefaultRootWindow(dpy), XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False), 0, 1, False,
                           XA_WINDOW, &type, &format, &n, &after, &data) == Success && data && n == 1)
        w = (Window) * (long *)(void *)data;
    if (data)
        XFree(data);
    return w;
}

static void request_focus(Display *dpy, Window w)
{
    XEvent ev = { 0 };
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = 2;   /* source: pager/tool, not an application */
    ev.xclient.data.l[1] = CurrentTime;
    gdk_error_trap_push();
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    gdk_error_trap_pop();
}

/* Polls until the target has focus back (the popup is gone), then types
 * the paste shortcut into it; gives up after the deadline. */
static gboolean on_paste_tick(gpointer data)
{
    (void)data;
    Display *dpy = xdpy();
    if (active_window(dpy) == g_paste.target) {
        send_key(dpy, XK_Control_L, True);
        if (g_paste.shift)
            send_key(dpy, XK_Shift_L, True);
        send_key(dpy, XK_v, True);
        send_key(dpy, XK_v, False);
        if (g_paste.shift)
            send_key(dpy, XK_Shift_L, False);
        send_key(dpy, XK_Control_L, False);
        XFlush(dpy);
        g_paste_id = 0;
        return FALSE;
    }
    if (g_get_monotonic_time() > g_paste.deadline) {
        g_paste_id = 0;
        return FALSE;
    }
    if (!g_paste.asked_focus && g_get_monotonic_time() > g_paste.deadline - 600000) {
        request_focus(dpy, g_paste.target);
        g_paste.asked_focus = 1;
    }
    return TRUE;
}

/* 0: Ctrl+V, 1: Ctrl+Shift+V, -1: no keyboard paste of CLIPBOARD. */
static int paste_style(Window w)
{
    static const char *shift_v[] = { "konsole", "gnome-terminal", "gnome-terminal-server", "xfce4-terminal",
                                     "terminator", "alacritty", "kitty", "tilix", "qterminal", "lxterminal",
                                     "mate-terminal", "st-256color", "sakura", "terminology", "yakuake", NULL };
    static const char *none[] = { "xterm", "uxterm", "urxvt", "rxvt", NULL };
    char cls[128], name[128];
    int style = 0;
    if (!target_class(w, cls, sizeof(cls), name, sizeof(name)))
        return -1;   /* gone: nothing to paste into */
    for (int i = 0; none[i]; i++)
        if (!strcasecmp(cls, none[i]) || !strcasecmp(name, none[i]))
            style = -1;
    for (int i = 0; shift_v[i] && style == 0; i++)
        if (!strcasecmp(cls, shift_v[i]) || !strcasecmp(name, shift_v[i]))
            style = 1;
    return style;
}

static void use_item(long id, gboolean paste)
{
    if (!id)
        return;
    if (!simple_cmd("{\"cmd\":\"SET\",\"id\":%ld}", id)) {
        set_status("N\xc3\xa3o foi poss\xc3\xadvel pegar a \xc3\xa1rea de transfer\xc3\xaancia");
        return;
    }
    Window target = xisserve_target_window();
    xisserve_hide();
    if (!paste || !target || !xisserve_config_get_int("CLIPBOARD", "autopaste", 1))
        return;
    int style = paste_style(target);
    if (style < 0)
        return;
    g_paste.target = target;
    g_paste.shift = style;
    g_paste.deadline = g_get_monotonic_time() + 1200000;
    g_paste.asked_focus = 0;
    if (g_paste_id)
        g_source_remove(g_paste_id);
    g_paste_id = g_timeout_add(40, on_paste_tick, NULL);
}

/* ---- callbacks -------------------------------------------------------- */

static gboolean on_search_timeout(gpointer data)
{
    (void)data;
    g_search_id = 0;
    reload();
    return FALSE;
}

static void on_entry_changed(GtkEditable *e, gpointer data)
{
    (void)e;
    (void)data;
    if (g_search_id)
        g_source_remove(g_search_id);
    g_search_id = g_timeout_add(SEARCH_DEBOUNCE_MS, on_search_timeout, NULL);
}

static void on_entry_activate(GtkEntry *e, gpointer data)
{
    (void)e;
    (void)data;
    long id = selected_id();
    GtkTreeIter it;
    if (!id && gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_store), &it)) {
        guint rid;
        gtk_tree_model_get(GTK_TREE_MODEL(g_store), &it, COL_ID, &rid, -1);
        id = rid;
    }
    use_item(id, TRUE);
}

static void move_selection(int delta)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_tree));
    GtkTreeModel *model;
    GtkTreeIter it;
    int n = gtk_tree_model_iter_n_children(GTK_TREE_MODEL(g_store), NULL);
    if (!n)
        return;
    int idx = 0;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &it);
        idx = gtk_tree_path_get_indices(p)[0] + delta;
        gtk_tree_path_free(p);
    }
    idx = CLAMP(idx, 0, n - 1);
    GtkTreePath *p = gtk_tree_path_new_from_indices(idx, -1);
    gtk_tree_selection_select_path(sel, p);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(g_tree), p, NULL, FALSE, 0, 0);
    gtk_tree_path_free(p);
}

static void toggle_fav(long id)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(g_tree));
    GtkTreeModel *model;
    GtkTreeIter it;
    int fav = 0;
    if (gtk_tree_selection_get_selected(sel, &model, &it))
        gtk_tree_model_get(model, &it, COL_FAV, &fav, -1);
    char req[96];
    snprintf(req, sizeof(req), "{\"cmd\":\"FAV\",\"id\":%ld,\"fav\":%d}", id, !fav);
    g_free(ctl_request(req));
    reload();
}

static void remove_item(long id)
{
    if (id && simple_cmd("{\"cmd\":\"REMOVE\",\"id\":%ld}", id)) {
        g_hash_table_remove(g_thumbs, GINT_TO_POINTER((int)id));
        reload();
    }
}

/* Keys shared by the search entry and the list: the entry keeps focus so
 * typing always searches; arrows/Enter/Delete act on the list. */
static gboolean on_key(GtkWidget *w, GdkEventKey *ev, gpointer data)
{
    (void)w;
    (void)data;
    gboolean ctrl = (ev->state & GDK_CONTROL_MASK) != 0;
    switch (ev->keyval) {
    case GDK_Down:      move_selection(1);  return TRUE;
    case GDK_Up:        move_selection(-1); return TRUE;
    case GDK_Page_Down: move_selection(8);  return TRUE;
    case GDK_Page_Up:   move_selection(-8); return TRUE;
    case GDK_Delete:
        if (ctrl || !gtk_entry_get_text(GTK_ENTRY(g_entry))[0]) {
            remove_item(selected_id());
            return TRUE;
        }
        return FALSE;
    case GDK_Return:
    case GDK_KP_Enter:
        /* Shift+Enter: only put it on the clipboard. */
        use_item(selected_id(), !(ev->state & GDK_SHIFT_MASK));
        return TRUE;
    case GDK_d:
    case GDK_D:
        if (ctrl) {
            toggle_fav(selected_id());
            return TRUE;
        }
        return FALSE;
    default:
        return FALSE;
    }
}

static void on_row_activated(GtkTreeView *tv, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)tv;
    (void)path;
    (void)col;
    (void)data;
    use_item(selected_id(), TRUE);
}

static void on_menu_paste(GtkMenuItem *mi, gpointer data) { (void)mi; use_item(GPOINTER_TO_INT(data), TRUE); }
static void on_menu_copy(GtkMenuItem *mi, gpointer data) { (void)mi; use_item(GPOINTER_TO_INT(data), FALSE); }
static void on_menu_fav(GtkMenuItem *mi, gpointer data) { (void)mi; toggle_fav(GPOINTER_TO_INT(data)); }
static void on_menu_remove(GtkMenuItem *mi, gpointer data) { (void)mi; remove_item(GPOINTER_TO_INT(data)); }

static void on_menu_deactivate(GtkMenuShell *m, gpointer data)
{
    (void)m;
    (void)data;
    xisserve_transient_popup_end();
}

static gboolean on_tree_button(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)data;
    if (ev->type != GDK_BUTTON_PRESS || ev->button != 3)
        return FALSE;
    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(w), (gint)ev->x, (gint)ev->y, &path, NULL, NULL, NULL))
        return FALSE;
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), path);
    gtk_tree_path_free(path);
    long id = selected_id();
    GtkTreeIter it;
    GtkTreeModel *model;
    int fav = 0;
    if (gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(w)), &model, &it))
        gtk_tree_model_get(model, &it, COL_FAV, &fav, -1);

    GtkWidget *menu = gtk_menu_new();
    struct { const char *label; GCallback cb; } items[] = {
        { "Colar", G_CALLBACK(on_menu_paste) },
        { "S\xc3\xb3 copiar", G_CALLBACK(on_menu_copy) },
        { fav ? "Desfavoritar" : "Favoritar", G_CALLBACK(on_menu_fav) },
        { "Remover", G_CALLBACK(on_menu_remove) },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(items); i++) {
        GtkWidget *mi = gtk_menu_item_new_with_label(items[i].label);
        g_signal_connect(mi, "activate", items[i].cb, GINT_TO_POINTER((int)id));
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), mi);
    }
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "deactivate", G_CALLBACK(on_menu_deactivate), NULL);
    xisserve_transient_popup_begin();
    gtk_menu_popup(GTK_MENU(menu), NULL, NULL, NULL, NULL, ev->button, ev->time);
    return TRUE;
}

static void on_scope_toggled(GtkToggleButton *b, gpointer data)
{
    if (!gtk_toggle_button_get_active(b))
        return;
    g_scope = GPOINTER_TO_INT(data);
    reload();
    gtk_widget_grab_focus(g_entry);
}

static void on_clear_clicked(GtkWidget *btn, gpointer data)
{
    (void)data;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(btn)), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                            "Limpar o hist\xc3\xb3rico da \xc3\xa1rea de transfer\xc3\xaancia?\n"
                                            "Os favoritos s\xc3\xa3o mantidos.");
    xisserve_transient_popup_begin();
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    xisserve_transient_popup_end();
    if (resp == GTK_RESPONSE_YES) {
        g_free(ctl_request("{\"cmd\":\"CLEAR\",\"keep_favs\":1}"));
        g_hash_table_remove_all(g_thumbs);
        reload();
    }
}

/* Refreshes only when something changed (a new copy, a paste, an item
 * put back), so the list and its selection don't flicker every tick. */
static gboolean status_changed(void)
{
    char *resp = ctl_request("{\"cmd\":\"STATUS\"}");
    long count = resp ? json_long(resp, "count", -2) : -2;
    long current = resp ? json_long(resp, "current", -2) : -2;
    g_free(resp);
    gboolean changed = count != g_last_count || current != g_last_current;
    g_last_count = count;
    g_last_current = current;
    return changed;
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    if (status_changed())
        reload();
    return TRUE;
}

/* ---- page interface ---------------------------------------------------- */

GtkWidget *page_clipboard_build(void)
{
    g_thumbs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_object_unref);

    g_root = gtk_vbox_new(FALSE, 4);

    g_entry = gtk_entry_new();
    style_fg(g_entry);
    style_bg_base(g_entry);
    g_signal_connect(g_entry, "changed", G_CALLBACK(on_entry_changed), NULL);
    g_signal_connect(g_entry, "activate", G_CALLBACK(on_entry_activate), NULL);
    g_signal_connect(g_entry, "key-press-event", G_CALLBACK(on_key), NULL);
    gtk_box_pack_start(GTK_BOX(g_root), g_entry, FALSE, FALSE, 0);

    GtkWidget *scopes = gtk_hbox_new(FALSE, 2);
    GSList *group = NULL;
    for (int i = 0; i < N_SCOPES; i++) {
        g_scope_btn[i] = gtk_radio_button_new_with_label(group, SCOPE_LABELS[i]);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(g_scope_btn[i]));
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(g_scope_btn[i]), FALSE);   /* look like toggle buttons */
        gtk_widget_set_can_focus(g_scope_btn[i], FALSE);
        g_signal_connect(g_scope_btn[i], "toggled", G_CALLBACK(on_scope_toggled), GINT_TO_POINTER(i));
        gtk_box_pack_start(GTK_BOX(scopes), g_scope_btn[i], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(g_root), scopes, FALSE, FALSE, 0);

    g_context = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_context), 0.0f, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(g_context), PANGO_ELLIPSIZE_END);
    style_fg(g_context);
    gtk_box_pack_start(GTK_BOX(g_root), g_context, FALSE, FALSE, 0);

    g_store = gtk_list_store_new(N_COLS, GDK_TYPE_PIXBUF, G_TYPE_STRING, G_TYPE_UINT, G_TYPE_INT, G_TYPE_STRING);
    g_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(g_tree), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(g_tree), FALSE);
    gtk_widget_set_can_focus(g_tree, FALSE);   /* typing stays in the search entry */
    style_fg(g_tree);
    style_bg_base(g_tree);
    GtkTreeViewColumn *col = gtk_tree_view_column_new();
    GtkCellRenderer *pix = gtk_cell_renderer_pixbuf_new();
    gtk_cell_renderer_set_padding(pix, 4, 2);
    gtk_tree_view_column_pack_start(col, pix, FALSE);
    gtk_tree_view_column_add_attribute(col, pix, "pixbuf", COL_ICON);
    GtkCellRenderer *txt = gtk_cell_renderer_text_new();
    /* GTK2 still requests an ellipsized cell's full text width: without
     * a width-chars floor one long copied line makes the page (and the
     * popup) wider than the placement code expects, pushing it off-screen. */
    g_object_set(txt, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 24, NULL);
    gtk_tree_view_column_pack_start(col, txt, TRUE);
    gtk_tree_view_column_add_attribute(col, txt, "markup", COL_MARKUP);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_tree), col);
    g_signal_connect(g_tree, "row-activated", G_CALLBACK(on_row_activated), NULL);
    g_signal_connect(g_tree, "button-press-event", G_CALLBACK(on_tree_button), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_tree);
    gtk_box_pack_start(GTK_BOX(g_root), scroll, TRUE, TRUE, 0);

    GtkWidget *bottom = gtk_hbox_new(FALSE, 6);
    g_status = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status), 0.0f, 0.5f);
    style_fg(g_status);
    gtk_box_pack_start(GTK_BOX(bottom), g_status, TRUE, TRUE, 0);
    GtkWidget *clear = gtk_button_new_with_label("Limpar");
    gtk_widget_set_can_focus(clear, FALSE);
    gtk_widget_set_tooltip_text(clear, "Remove tudo menos os favoritos");
    g_signal_connect(clear, "clicked", G_CALLBACK(on_clear_clicked), NULL);
    gtk_box_pack_end(GTK_BOX(bottom), clear, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_root), bottom, FALSE, FALSE, 0);

    gtk_widget_set_tooltip_text(g_tree, "Enter/duplo clique: colar \xc2\xb7 Shift+Enter: s\xc3\xb3 copiar \xc2\xb7 "
                                        "Ctrl+D: favoritar \xc2\xb7 Delete: remover");
    gtk_widget_show_all(g_root);
    return g_root;
}

void page_clipboard_on_show(void)
{
    /* Each open starts fresh: empty search, and the scope the invocation
     * asked for (--for-active/--for-window: the target's app). */
    g_signal_handlers_block_by_func(g_entry, on_entry_changed, NULL);
    gtk_entry_set_text(GTK_ENTRY(g_entry), "");
    g_signal_handlers_unblock_by_func(g_entry, on_entry_changed, NULL);
    int scope = xisserve_target_filter() && xisserve_target_window() ? SCOPE_APP : SCOPE_ALL;
    gboolean has_target = xisserve_target_window() != 0;
    for (int i = SCOPE_APP; i < N_SCOPES; i++)
        gtk_widget_set_sensitive(g_scope_btn[i], has_target);
    g_scope = scope;
    g_signal_handlers_block_by_func(g_scope_btn[scope], on_scope_toggled, GINT_TO_POINTER(scope));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_scope_btn[scope]), TRUE);
    g_signal_handlers_unblock_by_func(g_scope_btn[scope], on_scope_toggled, GINT_TO_POINTER(scope));

    gtk_list_store_clear(g_store);   /* so reload() selects the newest */
    status_changed();   /* baseline for on_poll() */
    reload();
    if (!g_poll_id)
        g_poll_id = g_timeout_add(CLIP_POLL_MS, on_poll, NULL);
    gtk_widget_grab_focus(g_entry);
}

void page_clipboard_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
    if (g_search_id) {
        g_source_remove(g_search_id);
        g_search_id = 0;
    }
}

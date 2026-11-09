/* syntax.c - syntax highlighting: loads the modules (kipad_syntax.h) and
 * paints their spans as GtkTextTags.
 *
 * Only the lines on screen, plus a margin, carry tags; scrolling or editing
 * repaints that range on idle. What a line needs from the lines above it is
 * the module's int state, kept per line (4 bytes a line) and recomputed from
 * the first edited line down, so a big file never gets tags all over. */
#include "syntax.h"
#include "kipad_syntax.h"

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

#ifndef KIPAD_SYNTAXDIR
#define KIPAD_SYNTAXDIR "/usr/local/lib/kipad/syntax"
#endif

#define MARGIN_LINES 60
#define LONG_LINE 10000 /* bytes; longer lines (minified files) stay plain */

struct SynDoc {
    GtkWidget *scroll;
    GtkTextView *view;
    GtkTextBuffer *buf;
    GtkAdjustment *vadj;
    char *path;
    const KipadSyntax *lang;
    GtkTextTag *tags[KS_COUNT];
    GArray *states; /* int: state at the end of line i, valid for i < len */
    gboolean tagged;
    guint idle;
    gulong sig_insert, sig_delete, sig_scroll, sig_alloc;
};

typedef struct {
    void *handle;
    const KipadSyntax *const *langs;
} Module;

static gboolean g_enabled = TRUE, g_loaded;
static GPtrArray *g_modules; /* Module * */
static GList *g_docs;        /* SynDoc * */
static void (*g_changed_cb)(void);

/* ---- modules --------------------------------------------------------- */

static void load_dir(const char *dir, GHashTable *seen)
{
    GDir *dh = g_dir_open(dir, 0, NULL);
    if (!dh)
        return;
    const char *name;
    while ((name = g_dir_read_name(dh))) {
        if (!g_str_has_suffix(name, ".so") || g_hash_table_contains(seen, name))
            continue;
        g_hash_table_add(seen, g_strdup(name));
        char *file = g_build_filename(dir, name, NULL);
        void *h = dlopen(file, RTLD_NOW | RTLD_LOCAL);
        const KipadSyntax *const *(*list)(void) = h ? (const KipadSyntax *const *(*)(void))dlsym(h, "kipad_syntax_list") : NULL;
        const KipadSyntax *const *langs = list ? list() : NULL;
        gboolean ok = langs != NULL;
        for (int i = 0; ok && langs[i]; i++)
            ok = langs[i]->abi == KIPAD_SYNTAX_ABI && langs[i]->name && langs[i]->detect && langs[i]->line;
        if (ok) {
            Module *m = g_new0(Module, 1);
            m->handle = h;
            m->langs = langs;
            g_ptr_array_add(g_modules, m);
        } else {
            fprintf(stderr, "kipad: syntax module %s not usable: %s\n", file, h ? "bad kipad_syntax_list" : dlerror());
            if (h)
                dlclose(h);
        }
        g_free(file);
    }
    g_dir_close(dh);
}

static void modules_load(void)
{
    if (g_loaded)
        return;
    g_loaded = TRUE;
    g_modules = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    const char *env = g_getenv("KIPAD_SYNTAX_PATH");
    if (env && *env) {
        char **dirs = g_strsplit(env, ":", -1);
        for (char **d = dirs; *d; d++)
            if (**d)
                load_dir(*d, seen);
        g_strfreev(dirs);
    }
    char *user = g_build_filename(g_get_home_dir(), ".local", "lib", "kipad", "syntax", NULL);
    load_dir(user, seen);
    g_free(user);
    load_dir(KIPAD_SYNTAXDIR, seen);
    g_hash_table_destroy(seen);
}

static void modules_unload(void)
{
    if (!g_loaded)
        return;
    for (guint i = 0; i < g_modules->len; i++) {
        Module *m = g_ptr_array_index(g_modules, i);
        dlclose(m->handle);
        g_free(m);
    }
    g_ptr_array_free(g_modules, TRUE);
    g_modules = NULL;
    g_loaded = FALSE;
}

static const KipadSyntax *detect(const char *path, const char *first_line)
{
    modules_load();
    char *base = g_path_get_basename(path);
    const KipadSyntax *best = NULL;
    int best_score = 0;
    for (guint i = 0; i < g_modules->len; i++) {
        const Module *m = g_ptr_array_index(g_modules, i);
        for (int j = 0; m->langs[j]; j++) {
            int s = m->langs[j]->detect(base, first_line);
            if (s > best_score) {
                best_score = s;
                best = m->langs[j];
            }
        }
    }
    g_free(base);
    return best;
}

/* ---- painting -------------------------------------------------------- */

static const struct {
    const char *light, *dark;
    gboolean bold, italic, underline;
} k_styles[KS_COUNT] = {
    [KS_COMMENT] = { "#7a7a7a", "#8b949e", FALSE, TRUE, FALSE },
    [KS_KEYWORD] = { "#0033b3", "#569cd6", TRUE, FALSE, FALSE },
    [KS_TYPE] = { "#00807a", "#4ec9b0", FALSE, FALSE, FALSE },
    [KS_STRING] = { "#067d17", "#ce9178", FALSE, FALSE, FALSE },
    [KS_NUMBER] = { "#1750eb", "#b5cea8", FALSE, FALSE, FALSE },
    [KS_PREPROC] = { "#8c6c00", "#c586c0", FALSE, FALSE, FALSE },
    [KS_FUNCTION] = { "#00627a", "#dcdcaa", FALSE, FALSE, FALSE },
    [KS_VARIABLE] = { "#871094", "#9cdcfe", FALSE, FALSE, FALSE },
    [KS_OPERATOR] = { "#8a3a00", "#d7ba7d", FALSE, FALSE, FALSE },
    [KS_ESCAPE] = { "#0037a6", "#d7ba7d", TRUE, FALSE, FALSE },
    [KS_KEY] = { "#0033b3", "#9cdcfe", FALSE, FALSE, FALSE },
    [KS_SECTION] = { "#a626a4", "#c586c0", TRUE, FALSE, FALSE },
    [KS_TAG] = { "#0033b3", "#569cd6", FALSE, FALSE, FALSE },
    [KS_EMPHASIS] = { NULL, NULL, FALSE, TRUE, FALSE },
    [KS_LINK] = { "#1a0dab", "#4fc1ff", FALSE, FALSE, TRUE },
    [KS_ERROR] = { "#d00000", "#f44747", FALSE, FALSE, TRUE },
};

static void create_tags(SynDoc *sd)
{
    if (sd->tags[KS_COMMENT])
        return;
    /* colours for a dark or a light background, by the view's base colour */
    GdkColor bg = gtk_widget_get_style(GTK_WIDGET(sd->view))->base[GTK_STATE_NORMAL];
    gboolean dark = (bg.red * 299u + bg.green * 587u + bg.blue * 114u) / 1000u < 0x8000;
    for (int i = 1; i < KS_COUNT; i++) {
        char name[16];
        g_snprintf(name, sizeof name, "syn-%d", i);
        const char *fg = dark ? k_styles[i].dark : k_styles[i].light;
        GtkTextTag *t = gtk_text_buffer_create_tag(sd->buf, name, NULL);
        if (fg)
            g_object_set(t, "foreground", fg, NULL);
        if (k_styles[i].bold)
            g_object_set(t, "weight", PANGO_WEIGHT_BOLD, NULL);
        if (k_styles[i].italic)
            g_object_set(t, "style", PANGO_STYLE_ITALIC, NULL);
        if (k_styles[i].underline)
            g_object_set(t, "underline", PANGO_UNDERLINE_SINGLE, NULL);
        sd->tags[i] = t;
    }
}

static void untag(SynDoc *sd)
{
    if (!sd->tagged)
        return;
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(sd->buf, &a, &b);
    for (int i = 1; i < KS_COUNT; i++)
        gtk_text_buffer_remove_tag(sd->buf, sd->tags[i], &a, &b);
    sd->tagged = FALSE;
}

typedef struct {
    SynDoc *sd;
    const char *text;
    int len, line;
} SpanCtx;

static void span_none(void *ctx, int start, int end, int style)
{
    (void)ctx;
    (void)start;
    (void)end;
    (void)style;
}

/* Snaps a module's byte offset inside the line and onto a character start. */
static int char_boundary(const SpanCtx *c, int off)
{
    off = CLAMP(off, 0, c->len);
    while (off < c->len && (c->text[off] & 0xC0) == 0x80)
        off++;
    return off;
}

static void span_paint(void *ctx, int start, int end, int style)
{
    SpanCtx *c = ctx;
    if (style <= KS_NONE || style >= KS_COUNT)
        return;
    start = char_boundary(c, start);
    end = char_boundary(c, end);
    if (start >= end)
        return;
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line_index(c->sd->buf, &a, c->line, start);
    gtk_text_buffer_get_iter_at_line_index(c->sd->buf, &b, c->line, end);
    gtk_text_buffer_apply_tag(c->sd->buf, c->sd->tags[style], &a, &b);
}

/* Runs the module over line `line`, painting it when `paint`; returns the
 * state at its end. */
static int run_line(SynDoc *sd, int line, int state, gboolean paint)
{
    GtkTextIter a, b;
    gtk_text_buffer_get_iter_at_line(sd->buf, &a, line);
    b = a;
    if (!gtk_text_iter_ends_line(&b))
        gtk_text_iter_forward_to_line_end(&b);
    char *text = gtk_text_iter_get_slice(&a, &b);
    int len = (int)strlen(text);
    if (len <= LONG_LINE) {
        SpanCtx c = { sd, text, len, line };
        state = sd->lang->line(text, len, state, paint ? span_paint : span_none, &c);
    }
    g_free(text);
    return state;
}

static gboolean refresh_idle(gpointer data)
{
    SynDoc *sd = data;
    sd->idle = 0;
    untag(sd);
    if (!sd->lang)
        return FALSE;
    GdkRectangle r;
    GtkTextIter it;
    gtk_text_view_get_visible_rect(sd->view, &r);
    gtk_text_view_get_line_at_y(sd->view, &it, r.y, NULL);
    int top = gtk_text_iter_get_line(&it);
    gtk_text_view_get_line_at_y(sd->view, &it, r.y + MAX(r.height, 1), NULL);
    int bottom = gtk_text_iter_get_line(&it);
    int n = gtk_text_buffer_get_line_count(sd->buf);
    int from = MAX(top - MARGIN_LINES, 0), to = MIN(MAX(bottom, top + MARGIN_LINES) + MARGIN_LINES, n - 1);
    for (int i = (int)sd->states->len; i < from; i++) {
        int s = run_line(sd, i, i ? g_array_index(sd->states, int, i - 1) : 0, FALSE);
        g_array_append_val(sd->states, s);
    }
    for (int i = from; i <= to; i++) {
        int s = run_line(sd, i, i ? g_array_index(sd->states, int, i - 1) : 0, TRUE);
        if (i < (int)sd->states->len)
            g_array_index(sd->states, int, i) = s;
        else
            g_array_append_val(sd->states, s);
    }
    sd->tagged = TRUE;
    return FALSE;
}

static void schedule(SynDoc *sd)
{
    /* below GTK's own layout validation, so the visible lines are known */
    if (sd->lang && !sd->idle)
        sd->idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, refresh_idle, sd, NULL);
}

static void invalidate_from(SynDoc *sd, int line)
{
    if (line < (int)sd->states->len)
        g_array_set_size(sd->states, (guint)line);
    schedule(sd);
}

static void on_insert(GtkTextBuffer *buf, GtkTextIter *at, char *text, int len, SynDoc *sd)
{
    (void)buf;
    (void)text;
    (void)len;
    invalidate_from(sd, gtk_text_iter_get_line(at));
}

static void on_delete(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, SynDoc *sd)
{
    (void)buf;
    (void)end;
    invalidate_from(sd, gtk_text_iter_get_line(start));
}

static void on_scroll(GtkAdjustment *adj, SynDoc *sd)
{
    (void)adj;
    schedule(sd);
}

static void on_alloc(GtkWidget *w, GtkAllocation *a, SynDoc *sd)
{
    (void)w;
    (void)a;
    schedule(sd);
}

/* ---- documents ------------------------------------------------------- */

static void set_lang(SynDoc *sd, const KipadSyntax *lang)
{
    if (sd->idle) {
        g_source_remove(sd->idle);
        sd->idle = 0;
    }
    untag(sd);
    g_array_set_size(sd->states, 0);
    sd->lang = lang;
    if (lang) {
        create_tags(sd);
        schedule(sd);
    }
    if (g_changed_cb)
        g_changed_cb();
}

static void redetect(SynDoc *sd)
{
    if (!g_enabled || !sd->path) {
        set_lang(sd, NULL);
        return;
    }
    GtkTextIter a, b;
    gtk_text_buffer_get_start_iter(sd->buf, &a);
    b = a;
    if (!gtk_text_iter_ends_line(&b))
        gtk_text_iter_forward_to_line_end(&b);
    char *first = gtk_text_iter_get_slice(&a, &b);
    set_lang(sd, detect(sd->path, first));
    g_free(first);
}

SynDoc *syn_doc_new(GtkWidget *scroll, GtkTextView *view)
{
    SynDoc *sd = g_new0(SynDoc, 1);
    sd->scroll = scroll;
    sd->view = g_object_ref(view);
    sd->buf = g_object_ref(gtk_text_view_get_buffer(view));
    sd->vadj = g_object_ref(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)));
    sd->states = g_array_new(FALSE, FALSE, sizeof(int));
    sd->sig_insert = g_signal_connect(sd->buf, "insert-text", G_CALLBACK(on_insert), sd);
    sd->sig_delete = g_signal_connect(sd->buf, "delete-range", G_CALLBACK(on_delete), sd);
    sd->sig_scroll = g_signal_connect(sd->vadj, "value-changed", G_CALLBACK(on_scroll), sd);
    sd->sig_alloc = g_signal_connect_after(view, "size-allocate", G_CALLBACK(on_alloc), sd);
    g_docs = g_list_prepend(g_docs, sd);
    return sd;
}

void syn_doc_free(SynDoc *sd)
{
    g_docs = g_list_remove(g_docs, sd);
    if (sd->idle)
        g_source_remove(sd->idle);
    g_signal_handler_disconnect(sd->buf, sd->sig_insert);
    g_signal_handler_disconnect(sd->buf, sd->sig_delete);
    g_signal_handler_disconnect(sd->vadj, sd->sig_scroll);
    if (g_signal_handler_is_connected(sd->view, sd->sig_alloc))
        g_signal_handler_disconnect(sd->view, sd->sig_alloc);
    g_object_unref(sd->vadj);
    g_object_unref(sd->buf);
    g_object_unref(sd->view);
    g_array_free(sd->states, TRUE);
    g_free(sd->path);
    g_free(sd);
}

void syn_doc_set_file(SynDoc *sd, const char *path)
{
    g_free(sd->path);
    sd->path = g_strdup(path);
    redetect(sd);
}

const char *syn_doc_language(const SynDoc *sd)
{
    return sd->lang ? sd->lang->name : NULL;
}

void syn_set_enabled(gboolean on)
{
    if (on == g_enabled)
        return;
    g_enabled = on;
    for (GList *l = g_docs; l; l = l->next)
        redetect(l->data);
    if (!on)
        modules_unload(); /* no doc points into a module any more */
}

void syn_set_changed_cb(void (*cb)(void))
{
    g_changed_cb = cb;
}

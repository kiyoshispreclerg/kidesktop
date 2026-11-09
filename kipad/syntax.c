/* syntax.c - syntax highlighting: loads the modules (kipad_syntax.h) and
 * paints their spans as GtkTextTags.
 *
 * Only a region around the lines on screen carries tags (between two marks,
 * so edits move it along). Scrolling paints just the lines coming into view
 * and an edit repaints from the edited line until the module's state comes
 * out as before. What a line needs from the lines above it is that int
 * state, kept per line (4 bytes a line), so a big file never gets tags all
 * over; states below the screen are computed in blocks of text, without
 * painting, only when the view gets there. */
#include "syntax.h"
#include "kipad_syntax.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef KIPAD_SYNTAXDIR
#define KIPAD_SYNTAXDIR "/usr/local/lib/kipad/syntax"
#endif

#define MARGIN_LINES 60
#define TAGGED_MAX 1000  /* lines: a tagged region growing past this starts over */
#define STATE_CHUNK 4096 /* lines per block of text read to compute states */
#define UNKNOWN INT_MIN  /* state of a line that edits made up */
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
    gboolean tagged; /* lines from ta's to tb's carry tags */
    GtkTextMark *ta, *tb;
    int dirty_from, dirty_to; /* lines to repaint after edits, -1: none */
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

static void untag_lines(SynDoc *sd, int a, int b)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line(sd->buf, &s, a);
    gtk_text_buffer_get_iter_at_line(sd->buf, &e, b);
    if (!gtk_text_iter_ends_line(&e))
        gtk_text_iter_forward_to_line_end(&e);
    for (int i = 1; i < KS_COUNT; i++)
        gtk_text_buffer_remove_tag(sd->buf, sd->tags[i], &s, &e);
}

static void untag(SynDoc *sd)
{
    if (!sd->tagged)
        return;
    GtkTextIter it;
    gtk_text_buffer_get_iter_at_mark(sd->buf, &it, sd->ta);
    int a = gtk_text_iter_get_line(&it);
    gtk_text_buffer_get_iter_at_mark(sd->buf, &it, sd->tb);
    untag_lines(sd, a, gtk_text_iter_get_line(&it));
    sd->tagged = FALSE;
}

static void set_tagged(SynDoc *sd, int a, int b)
{
    GtkTextIter s, e;
    gtk_text_buffer_get_iter_at_line(sd->buf, &s, a);
    gtk_text_buffer_get_iter_at_line(sd->buf, &e, b);
    if (!gtk_text_iter_ends_line(&e))
        gtk_text_iter_forward_to_line_end(&e);
    if (!sd->ta) {
        sd->ta = gtk_text_buffer_create_mark(sd->buf, NULL, &s, TRUE);
        sd->tb = gtk_text_buffer_create_mark(sd->buf, NULL, &e, FALSE);
    } else {
        gtk_text_buffer_move_mark(sd->buf, sd->ta, &s);
        gtk_text_buffer_move_mark(sd->buf, sd->tb, &e);
    }
    sd->tagged = TRUE;
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

static int tokenize(SynDoc *sd, const char *text, int len, int line, int state, gboolean paint)
{
    if (len > LONG_LINE)
        return state;
    SpanCtx c = { sd, text, len, line };
    return sd->lang->line(text, len, state, paint ? span_paint : span_none, &c);
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
    state = tokenize(sd, text, (int)strlen(text), line, state, paint);
    g_free(text);
    return state;
}

static int state_before(const SynDoc *sd, int line)
{
    return line ? g_array_index(sd->states, int, line - 1) : 0;
}

static void store_state(SynDoc *sd, int line, int s)
{
    if (line < (int)sd->states->len)
        g_array_index(sd->states, int, line) = s;
    else
        g_array_append_val(sd->states, s);
}

/* Makes the states of lines before `upto` known, reading the text in blocks
 * (one btree walk each) and splitting it into lines as GtkTextBuffer does. */
static void ensure_states(SynDoc *sd, int upto)
{
    while ((int)sd->states->len < upto) {
        int a = (int)sd->states->len, b = MIN(upto, a + STATE_CHUNK);
        GtkTextIter s, e;
        gtk_text_buffer_get_iter_at_line(sd->buf, &s, a);
        gtk_text_buffer_get_iter_at_line(sd->buf, &e, b);
        if (gtk_text_iter_get_line(&e) < b)
            gtk_text_buffer_get_end_iter(sd->buf, &e);
        char *text = gtk_text_iter_get_slice(&s, &e);
        const char *p = text;
        int rem = (int)strlen(text), state = state_before(sd, a);
        for (int i = a; i < b; i++) {
            int delim, next;
            pango_find_paragraph_boundary(p, rem, &delim, &next);
            state = tokenize(sd, p, delim, i, state, FALSE);
            g_array_append_val(sd->states, state);
            p += next;
            rem -= next;
        }
        g_free(text);
    }
}

static void paint_lines(SynDoc *sd, int a, int b)
{
    ensure_states(sd, a);
    for (int i = a; i <= b; i++)
        store_state(sd, i, run_line(sd, i, state_before(sd, i), TRUE));
}

/* Repaints from the first edited line until a line past the edits ends in
 * the state it had before; below the tagged region only states are kept, and
 * they are dropped from where they stop matching. */
static void repaint_dirty(SynDoc *sd, int ta, int tb, int n)
{
    int i = sd->dirty_from;
    if (i > (int)sd->states->len)
        return;
    int state = state_before(sd, i);
    for (; i < n; i++) {
        gboolean paint = sd->tagged && i >= ta && i <= tb;
        if (!paint && (!sd->tagged || i > tb)) {
            if (i < (int)sd->states->len)
                g_array_set_size(sd->states, (guint)i);
            return;
        }
        if (paint)
            untag_lines(sd, i, i);
        int old = i < (int)sd->states->len ? g_array_index(sd->states, int, i) : UNKNOWN;
        state = run_line(sd, i, state, paint);
        store_state(sd, i, state);
        if (i >= sd->dirty_to && state == old)
            return;
    }
}

static gboolean refresh_idle(gpointer data)
{
    SynDoc *sd = data;
    sd->idle = 0;
    if (!sd->lang)
        return FALSE;
    int n = gtk_text_buffer_get_line_count(sd->buf), ta = 0, tb = -1;
    if (sd->tagged) {
        GtkTextIter it;
        gtk_text_buffer_get_iter_at_mark(sd->buf, &it, sd->ta);
        ta = gtk_text_iter_get_line(&it);
        gtk_text_buffer_get_iter_at_mark(sd->buf, &it, sd->tb);
        tb = gtk_text_iter_get_line(&it);
    }
    if (sd->dirty_from >= 0) {
        repaint_dirty(sd, ta, tb, n);
        sd->dirty_from = sd->dirty_to = -1;
    }

    GdkRectangle r;
    GtkTextIter it;
    gtk_text_view_get_visible_rect(sd->view, &r);
    gtk_text_view_get_line_at_y(sd->view, &it, r.y, NULL);
    int top = gtk_text_iter_get_line(&it);
    gtk_text_view_get_line_at_y(sd->view, &it, r.y + MAX(r.height, 1), NULL);
    int bottom = gtk_text_iter_get_line(&it);
    int from = MAX(top - MARGIN_LINES, 0), to = MIN(MAX(bottom, top + MARGIN_LINES) + MARGIN_LINES, n - 1);

    if (!sd->tagged || to < ta - 1 || from > tb + 1 || MAX(tb, to) - MIN(ta, from) > TAGGED_MAX) {
        /* far from what is painted: start over */
        untag(sd);
        paint_lines(sd, from, to);
        set_tagged(sd, from, to);
    } else {
        if (from < ta)
            paint_lines(sd, from, ta - 1);
        if (to > tb)
            paint_lines(sd, tb + 1, to);
        set_tagged(sd, MIN(from, ta), MAX(to, tb));
    }
    return FALSE;
}

static void schedule(SynDoc *sd)
{
    /* below GTK's own layout validation, so the visible lines are known */
    if (sd->lang && !sd->idle)
        sd->idle = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, refresh_idle, sd, NULL);
}

/* An edit at `line` turned it into lines line..line+added (added < 0: it
 * removed -added lines after it). States move along with their lines, so the
 * repaint can tell when the old ones match again. */
static void edited(SynDoc *sd, int line, int added)
{
    if (!sd->lang)
        return;
    GArray *st = sd->states;
    if (line < (int)st->len) {
        if (added > 0) {
            int unknown = UNKNOWN;
            for (int k = 0; k < added; k++)
                g_array_insert_val(st, (guint)line, unknown);
        } else if (added < 0)
            g_array_remove_range(st, (guint)line, (guint)MIN(-added, (int)st->len - line));
    }
    int last = line + MAX(added, 0);
    if (sd->dirty_from < 0) {
        sd->dirty_from = line;
        sd->dirty_to = last;
    } else {
        if (sd->dirty_to >= line)
            sd->dirty_to = MAX(sd->dirty_to + added, line);
        sd->dirty_from = MIN(sd->dirty_from, line);
        sd->dirty_to = MAX(sd->dirty_to, last);
    }
    schedule(sd);
}

static int count_lines(const char *text, int len)
{
    int k = 0, delim, next;
    while (len > 0) {
        pango_find_paragraph_boundary(text, len, &delim, &next);
        if (next == delim) /* no terminator: last piece */
            break;
        k++;
        text += next;
        len -= next;
    }
    return k;
}

static void on_insert(GtkTextBuffer *buf, GtkTextIter *at, char *text, int len, SynDoc *sd)
{
    (void)buf;
    edited(sd, gtk_text_iter_get_line(at), count_lines(text, len < 0 ? (int)strlen(text) : len));
}

static void on_delete(GtkTextBuffer *buf, GtkTextIter *start, GtkTextIter *end, SynDoc *sd)
{
    (void)buf;
    int a = gtk_text_iter_get_line(start);
    edited(sd, a, -(gtk_text_iter_get_line(end) - a));
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
    sd->dirty_from = sd->dirty_to = -1;
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
    sd->dirty_from = sd->dirty_to = -1;
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
    if (sd->ta) {
        gtk_text_buffer_delete_mark(sd->buf, sd->ta);
        gtk_text_buffer_delete_mark(sd->buf, sd->tb);
    }
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

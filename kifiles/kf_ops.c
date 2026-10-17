/* kf_ops.c - file operations: copy/move/trash/delete run in a worker thread
 * (GIO sync calls, no GTK there) while the main loop polls the job for a
 * progress dialog; rename/new folder/restore are quick and run inline.
 * Everything that changed something pushes an undo entry.
 *
 * Name conflicts are resolved *before* the worker starts (one dialog per
 * conflicting top-level item, with "apply to all"), so the worker never has to
 * talk to the user. A folder merge overwrites or skips nested files according
 * to the answer given for that folder. */
#include "kf.h"
#include "../shared/xis_i18n.h"
#include <string.h>

typedef enum { JOB_COPY, JOB_MOVE, JOB_TRASH, JOB_DELETE } JobType;

typedef struct {
    GFile *src, *dst;
    gboolean replace;       /* overwrite / merge into an existing dst */
    gboolean rename;        /* a move within one file system (set by the worker) */
} Item;

typedef struct { GFile *a, *b; } Pair;

typedef enum { U_MOVE, U_COPY, U_TRASH, U_RENAME, U_NEW } UndoKind;

typedef struct {
    UndoKind kind;
    GList *pairs;           /* Pair* */
} Undo;

typedef struct {
    KfApp *app;
    JobType type;
    GList *items;           /* Item* */
    GCancellable *cancel;
    GThread *thread;
    GMutex lock;
    GCond cond;             /* the worker waits here for an answer from the user */
    int space_q;            /* not enough space: 0 no question, 1 to ask, 2 being asked */
    guint64 need, avail;
    gboolean go_on;         /* the answer */
    char *current;
    guint64 total, done;
    gboolean preparing;
    volatile gint finished;
    GList *results;         /* Pair* of what was done, for undo */
    GList *errors;          /* char* */
    GList *untrashable;     /* GFile* that could not be trashed */
    guint timer;
    gint64 started;
    gboolean paused;
    GtkWidget *dialog, *bar, *label, *pause_btn;
    KfJobView *view;        /* progress shown by the desktop instead of the dialog */
    char *src_desc, *dst_desc;  /* folders named in the view */
    GFile *dest_dir;
    gboolean empty_trash;   /* a delete job that is "Empty Trash" */
} Job;

#define UNDO_MAX 50

static void pair_free(gpointer p)
{
    Pair *pr = p;
    if (pr->a) g_object_unref(pr->a);
    if (pr->b) g_object_unref(pr->b);
    g_free(pr);
}

static void undo_free(gpointer p)
{
    Undo *u = p;
    g_list_free_full(u->pairs, pair_free);
    g_free(u);
}

static Pair *pair_new(GFile *a, GFile *b)
{
    Pair *p = g_new0(Pair, 1);
    p->a = a ? g_object_ref(a) : NULL;
    p->b = b ? g_object_ref(b) : NULL;
    return p;
}

static void undo_push(KfApp *app, UndoKind kind, GList *pairs)
{
    if (!pairs)
        return;
    Undo *u = g_new0(Undo, 1);
    u->kind = kind;
    u->pairs = pairs;
    g_queue_push_head(app->undo, u);
    while (g_queue_get_length(app->undo) > UNDO_MAX)
        undo_free(g_queue_pop_tail(app->undo));
    kf_app_pane_changed(app, app->active, KF_CHANGE_SELECTION);   /* refresh Undo sensitivity */
}

gboolean kf_ops_can_undo(KfApp *app) { return !g_queue_is_empty(app->undo); }

void kf_ops_init(KfApp *app) { app->undo = g_queue_new(); }

/* ---- small helpers ------------------------------------------------------ */

static char *display_name(GFile *f)
{
    char *b = g_file_get_basename(f);
    char *d = g_filename_display_name(b ? b : "?");
    g_free(b);
    return d;
}

static void add_error(Job *j, GFile *f, GError *err)
{
    char *n = display_name(f);
    char *msg = g_strdup_printf("%s: %s", n, err ? err->message : _("unknown error"));
    g_free(n);
    g_mutex_lock(&j->lock);
    j->errors = g_list_append(j->errors, msg);
    g_mutex_unlock(&j->lock);
}

static void set_current(Job *j, GFile *f)
{
    char *n = display_name(f);
    g_mutex_lock(&j->lock);
    g_free(j->current);
    j->current = n;
    g_mutex_unlock(&j->lock);
}

static void add_done(Job *j, guint64 n)
{
    g_mutex_lock(&j->lock);
    j->done += n;
    g_mutex_unlock(&j->lock);
}

/* "name (copy).ext", "name (copy 2).ext", ... free in `dir`. */
static GFile *unique_child(GFile *dir, const char *base)
{
    const char *dot = strrchr(base, '.');
    gboolean has_ext = dot && dot != base;
    char *stem = has_ext ? g_strndup(base, dot - base) : g_strdup(base);
    const char *ext = has_ext ? dot : "";
    GFile *f = NULL;
    for (int i = 1; i < 10000; i++) {
        char *name = i == 1 ? g_strdup_printf("%s (%s)%s", stem, _("copy"), ext)
                            : g_strdup_printf("%s (%s %d)%s", stem, _("copy"), i, ext);
        f = g_file_get_child(dir, name);
        g_free(name);
        if (!g_file_query_exists(f, NULL))
            break;
        g_object_unref(f);
        f = NULL;
    }
    g_free(stem);
    return f;
}

/* ---- worker ------------------------------------------------------------- */

typedef struct {
    Job *job;
    goffset last;
} Prog;

/* Called between files and between a file's chunks (via on_progress), so a
 * single big file pauses too; a rename or a delete is one call and doesn't. */
static void wait_if_paused(Job *j)
{
    g_mutex_lock(&j->lock);
    while (j->paused && !g_cancellable_is_cancelled(j->cancel))
        g_cond_wait(&j->cond, &j->lock);
    g_mutex_unlock(&j->lock);
}

static void on_progress(goffset cur, goffset total, gpointer data)
{
    (void)total;
    Prog *p = data;
    if (cur > p->last)
        add_done(p->job, cur - p->last);
    p->last = cur;
    wait_if_paused(p->job);
}

static guint64 scan_size(GFile *f, GCancellable *cancel)
{
    GFileInfo *info = g_file_query_info(f, "standard::type,standard::size", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                        cancel, NULL);
    if (!info)
        return 1;
    guint64 sz = 1;
    if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        cancel, NULL);
        if (en) {
            GFileInfo *ci;
            while (!g_cancellable_is_cancelled(cancel) && (ci = g_file_enumerator_next_file(en, cancel, NULL))) {
                GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
                sz += scan_size(c, cancel);
                g_object_unref(c);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    } else {
        sz += g_file_info_get_size(info);
    }
    g_object_unref(info);
    return sz;
}

static gboolean delete_recursive(Job *j, GFile *f)
{
    wait_if_paused(j);
    if (g_cancellable_is_cancelled(j->cancel))
        return FALSE;
    GFileInfo *info = g_file_query_info(f, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, NULL);
    gboolean ok = TRUE;
    if (info && g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY) {
        GFileEnumerator *en = g_file_enumerate_children(f, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        j->cancel, NULL);
        if (en) {
            GFileInfo *ci;
            while (!g_cancellable_is_cancelled(j->cancel) && (ci = g_file_enumerator_next_file(en, j->cancel, NULL))) {
                GFile *c = g_file_get_child(f, g_file_info_get_name(ci));
                if (!delete_recursive(j, c))
                    ok = FALSE;
                g_object_unref(c);
                g_object_unref(ci);
            }
            g_object_unref(en);
        }
    }
    if (info)
        g_object_unref(info);
    if (!ok)
        return FALSE;
    set_current(j, f);
    GError *err = NULL;
    if (!g_file_delete(f, j->cancel, &err)) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            add_error(j, f, err);
        g_clear_error(&err);
        return FALSE;
    }
    add_done(j, 1);
    return TRUE;
}

/* A copy that failed or was cancelled midway leaves a truncated file at dst
 * that looks like a finished one. Overwrites are safe (GIO writes a temporary
 * and keeps the old file), and EXISTS means dst was never ours. */
static void drop_partial(GFile *dst, gboolean replace, const GError *err)
{
    if (!replace && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS))
        g_file_delete(dst, NULL, NULL);
}

static gboolean copy_recursive(Job *j, GFile *src, GFile *dst, gboolean replace)
{
    wait_if_paused(j);
    if (g_cancellable_is_cancelled(j->cancel))
        return FALSE;
    GError *err = NULL;
    GFileInfo *info = g_file_query_info(src, "standard::type", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, j->cancel, &err);
    if (!info) {
        add_error(j, src, err);
        g_clear_error(&err);
        return FALSE;
    }
    gboolean isdir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(info);
    set_current(j, src);
    if (isdir) {
        if (!g_file_make_directory(dst, j->cancel, &err)) {
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_EXISTS)) {
                if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                    add_error(j, dst, err);
                g_clear_error(&err);
                return FALSE;
            }
            g_clear_error(&err);
        }
        gboolean ok = TRUE;
        GFileEnumerator *en = g_file_enumerate_children(src, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                                        j->cancel, &err);
        if (!en) {
            add_error(j, src, err);
            g_clear_error(&err);
            return FALSE;
        }
        GFileInfo *ci;
        while (!g_cancellable_is_cancelled(j->cancel) && (ci = g_file_enumerator_next_file(en, j->cancel, NULL))) {
            GFile *cs = g_file_get_child(src, g_file_info_get_name(ci));
            GFile *cd = g_file_get_child(dst, g_file_info_get_name(ci));
            /* Merging: without "replace", files already there are kept. */
            if (!replace && g_file_query_exists(cd, NULL) &&
                g_file_query_file_type(cd, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) != G_FILE_TYPE_DIRECTORY) {
                add_done(j, scan_size(cs, j->cancel));
            } else if (!copy_recursive(j, cs, cd, replace)) {
                ok = FALSE;
            }
            g_object_unref(cs);
            g_object_unref(cd);
            g_object_unref(ci);
        }
        g_object_unref(en);
        g_file_copy_attributes(src, dst, G_FILE_COPY_ALL_METADATA, j->cancel, NULL);
        add_done(j, 1);
        return ok && !g_cancellable_is_cancelled(j->cancel);
    }
    Prog pr = { j, 0 };
    GFileCopyFlags fl = G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA;
    if (replace)
        fl |= G_FILE_COPY_OVERWRITE;
    if (!g_file_copy(src, dst, fl, j->cancel, on_progress, &pr, &err)) {
        drop_partial(dst, replace, err);
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            add_error(j, src, err);
        g_clear_error(&err);
        return FALSE;
    }
    add_done(j, 1);
    return TRUE;
}

static void run_item(Job *j, Item *it)
{
    GError *err = NULL;
    wait_if_paused(j);
    if (g_cancellable_is_cancelled(j->cancel))
        return;
    switch (j->type) {
    case JOB_COPY:
        if (copy_recursive(j, it->src, it->dst, it->replace))
            j->results = g_list_append(j->results, pair_new(it->src, it->dst));
        break;
    case JOB_MOVE: {
        set_current(j, it->src);
        GFileCopyFlags fl = G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA;
        if (it->replace)
            fl |= G_FILE_COPY_OVERWRITE;
        Prog pr = { j, 0 };
        if (g_file_move(it->src, it->dst, fl, j->cancel, it->rename ? NULL : on_progress, &pr, &err)) {
            add_done(j, 1);
            j->results = g_list_append(j->results, pair_new(it->src, it->dst));
            break;
        }
        /* Across file systems, or merging into a folder: copy, then delete. */
        if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_RECURSE) ||
            g_error_matches(err, G_IO_ERROR, G_IO_ERROR_WOULD_MERGE) ||
            g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED)) {
            g_clear_error(&err);
            if (copy_recursive(j, it->src, it->dst, it->replace) && delete_recursive(j, it->src))
                j->results = g_list_append(j->results, pair_new(it->src, it->dst));
        } else {
            drop_partial(it->dst, it->replace, err);    /* across file systems it copied first */
            if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                add_error(j, it->src, err);
            g_clear_error(&err);
        }
        break;
    }
    case JOB_TRASH:
        set_current(j, it->src);
        if (g_file_trash(it->src, j->cancel, &err)) {
            add_done(j, 1);
            j->results = g_list_append(j->results, pair_new(it->src, NULL));
        } else {
            if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
                j->untrashable = g_list_append(j->untrashable, g_object_ref(it->src));
            else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
                add_error(j, it->src, err);
            g_clear_error(&err);
        }
        break;
    case JOB_DELETE:
        delete_recursive(j, it->src);
        break;
    }
}

static gboolean same_fs(GFile *a, GFile *b, GCancellable *cancel)
{
    GFileInfo *ia = g_file_query_info(a, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancel, NULL);
    GFileInfo *ib = g_file_query_info(b, G_FILE_ATTRIBUTE_ID_FILESYSTEM, G_FILE_QUERY_INFO_NONE, cancel, NULL);
    const char *x = ia ? g_file_info_get_attribute_string(ia, G_FILE_ATTRIBUTE_ID_FILESYSTEM) : NULL;
    const char *y = ib ? g_file_info_get_attribute_string(ib, G_FILE_ATTRIBUTE_ID_FILESYSTEM) : NULL;
    gboolean same = x && y && !strcmp(x, y);
    if (ia)
        g_object_unref(ia);
    if (ib)
        g_object_unref(ib);
    return same;
}

/* FALSE when the user chose not to go on. Free space is only an estimate
 * (block rounding, compression, quotas), so this asks instead of refusing. */
static gboolean enough_space(Job *j, guint64 need)
{
    GFileInfo *info = g_file_query_filesystem_info(j->dest_dir, G_FILE_ATTRIBUTE_FILESYSTEM_FREE, j->cancel, NULL);
    if (!info)
        return TRUE;
    gboolean known = g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    guint64 avail = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
    g_object_unref(info);
    if (!known || need <= avail)
        return TRUE;
    g_mutex_lock(&j->lock);
    j->need = need;
    j->avail = avail;
    j->space_q = 1;
    while (j->space_q && !g_cancellable_is_cancelled(j->cancel))
        g_cond_wait(&j->cond, &j->lock);
    gboolean go_on = j->go_on;
    g_mutex_unlock(&j->lock);
    return go_on;
}

static gpointer job_thread(gpointer data)
{
    Job *j = data;
    /* Size first, so the progress bar means something. A move within one
     * file system is a rename: no scan, no space needed (a folder merge is
     * a copy). */
    if (j->type == JOB_COPY || j->type == JOB_MOVE) {
        guint64 total = 0, need = 0;
        for (GList *l = j->items; l && !g_cancellable_is_cancelled(j->cancel); l = l->next) {
            Item *it = l->data;
            it->rename = j->type == JOB_MOVE && !it->replace && same_fs(it->src, j->dest_dir, j->cancel);
            if (it->rename) {
                total += 1;
            } else {
                guint64 sz = scan_size(it->src, j->cancel);
                total += sz;
                need += sz;
            }
        }
        g_mutex_lock(&j->lock);
        j->total = total;
        j->preparing = FALSE;
        g_mutex_unlock(&j->lock);
        if (need && !g_cancellable_is_cancelled(j->cancel) && !enough_space(j, need))
            g_cancellable_cancel(j->cancel);
    } else {
        g_mutex_lock(&j->lock);
        j->total = g_list_length(j->items);
        j->preparing = FALSE;
        g_mutex_unlock(&j->lock);
    }
    for (GList *l = j->items; l && !g_cancellable_is_cancelled(j->cancel); l = l->next)
        run_item(j, l->data);
    g_atomic_int_set(&j->finished, 1);
    return NULL;
}

/* ---- main-loop side of a job -------------------------------------------- */

static void item_free(gpointer p)
{
    Item *it = p;
    g_object_unref(it->src);
    if (it->dst)
        g_object_unref(it->dst);
    g_free(it);
}

static void job_free(Job *j)
{
    g_list_free_full(j->items, item_free);
    g_list_free_full(j->results, pair_free);
    g_list_free_full(j->errors, g_free);
    kf_files_free(j->untrashable);
    g_object_unref(j->cancel);
    g_mutex_clear(&j->lock);
    g_cond_clear(&j->cond);
    g_free(j->current);
    if (j->dest_dir)
        g_object_unref(j->dest_dir);
    g_free(j->src_desc);
    g_free(j->dst_desc);
    g_free(j);
}

#define RESPONSE_PAUSE 1

static void set_pause_button(Job *j, gboolean paused)
{
    if (!j->pause_btn)
        return;
    gtk_button_set_label(GTK_BUTTON(j->pause_btn), paused ? _("_Resume") : _("_Pause"));
    gtk_button_set_image(GTK_BUTTON(j->pause_btn),
                         gtk_image_new_from_stock(paused ? GTK_STOCK_MEDIA_PLAY : GTK_STOCK_MEDIA_PAUSE,
                                                  GTK_ICON_SIZE_BUTTON));
}

/* Both wake a waiting worker so it looks again. */
static void job_cancel(Job *j)
{
    g_cancellable_cancel(j->cancel);
    g_mutex_lock(&j->lock);
    g_cond_broadcast(&j->cond);
    g_mutex_unlock(&j->lock);
}

static void job_set_paused(Job *j, gboolean paused)
{
    g_mutex_lock(&j->lock);
    j->paused = paused;
    g_cond_broadcast(&j->cond);
    g_mutex_unlock(&j->lock);
    set_pause_button(j, paused);
}

/* Pause toggles; anything else (Cancel, X, Escape) cancels. */
static void on_job_response(GtkDialog *d, gint response, gpointer data)
{
    (void)d;
    Job *j = data;
    if (response == RESPONSE_PAUSE)
        job_set_paused(j, !j->paused);
    else
        job_cancel(j);
}

static void on_view_request(KfJobViewRequest what, gpointer data)
{
    Job *j = data;
    if (what == KF_JOBVIEW_CANCEL)
        job_cancel(j);
    else
        job_set_paused(j, what == KF_JOBVIEW_SUSPEND);
}

static void ask_space(Job *j)
{
    char *dest = display_name(j->dest_dir);
    char *need = g_format_size_full(j->need, G_FORMAT_SIZE_IEC_UNITS);
    char *avail = g_format_size_full(j->avail, G_FORMAT_SIZE_IEC_UNITS);
    GtkWidget *d = gtk_message_dialog_new(kf_app_parent(j->app), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, _("There may not be enough free space in \"%s\""), dest);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), _("%s needed, %s free."), need, avail);
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           _("Continue anyway"), GTK_RESPONSE_ACCEPT, NULL);
    gboolean go_on = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    g_free(dest);
    g_free(need);
    g_free(avail);
    g_mutex_lock(&j->lock);
    j->go_on = go_on;
    j->space_q = 0;
    g_cond_broadcast(&j->cond);
    g_mutex_unlock(&j->lock);
}

static void show_errors(KfApp *app, GList *errors)
{
    xis_sound_play("dialog-error");
    GString *s = g_string_new(NULL);
    int n = 0;
    for (GList *l = errors; l && n < 12; l = l->next, n++)
        g_string_append_printf(s, "%s\n", (char *)l->data);
    if (g_list_length(errors) > 12)
        g_string_append_printf(s, _("... and %u more.\n"), g_list_length(errors) - 12);
    kf_app_message(app, GTK_MESSAGE_ERROR, _("Some items could not be processed"), s->str);
    g_string_free(s, TRUE);
}

static void job_done(Job *j);
static void create_dialog(Job *j);

static const char *job_title(Job *j)
{
    return j->type == JOB_COPY ? _("Copying") : j->type == JOB_MOVE ? _("Moving") :
           j->type == JOB_TRASH ? _("Moving to the trash") : _("Deleting");
}

/* Where from and where to, for the desktop's view. */
static void describe_job(Job *j)
{
    Item *first = j->items ? j->items->data : NULL;
    GFile *src_dir = first ? g_file_get_parent(first->src) : NULL;
    if (src_dir) {
        j->src_desc = g_file_get_parse_name(src_dir);
        g_object_unref(src_dir);
    }
    if (j->dest_dir)
        j->dst_desc = g_file_get_parse_name(j->dest_dir);
}

static gboolean job_poll(gpointer data)
{
    Job *j = data;
    if (g_atomic_int_get(&j->finished)) {
        g_thread_join(j->thread);
        j->timer = 0;
        job_done(j);
        return FALSE;
    }
    g_mutex_lock(&j->lock);
    gboolean ask = j->space_q == 1;
    if (ask)
        j->space_q = 2;
    g_mutex_unlock(&j->lock);
    if (ask) {
        ask_space(j);       /* nested loop: this source is not re-entered meanwhile */
        return TRUE;
    }
    /* Only operations that last get a progress display at all, so a quick
     * copy leaves nothing behind (not even a "finished" notification). The
     * desktop's job server is preferred; the dialog is the fallback, also
     * when the server goes away mid-job. */
    gint64 elapsed = g_get_monotonic_time() - j->started;
    if (!j->dialog && !j->view && (elapsed > 600 * 1000 || j->app->quitting)) {
        j->view = kf_jobview_new(job_title(j), on_view_request, j);
        if (j->view)
            describe_job(j);
        else
            create_dialog(j);
    }
    if (j->view && kf_jobview_lost(j->view)) {
        kf_jobview_free(j->view);
        j->view = NULL;
        create_dialog(j);
    }
    g_mutex_lock(&j->lock);
    char *cur = g_strdup(j->current ? j->current : "");
    guint64 done = j->done, total = j->total;
    gboolean prep = j->preparing, paused = j->paused;
    g_mutex_unlock(&j->lock);
    if (j->view) {
        int percent = prep || total == 0 ? -1 : (int)MIN(done * 100 / total, 100);
        kf_jobview_update(j->view, cur, percent, done, total, j->type == JOB_COPY || j->type == JOB_MOVE, paused,
                          j->src_desc, j->dst_desc);
    }
    if (j->dialog) {
        gtk_label_set_text(GTK_LABEL(j->label), cur);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(j->bar), paused ? _("Paused") : NULL);
        if (prep || total == 0)
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(j->bar));
        else
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(j->bar), CLAMP((double)done / (double)total, 0.0, 1.0));
    }
    g_free(cur);
    return TRUE;
}

static void create_dialog(Job *j)
{
    j->dialog = gtk_dialog_new_with_buttons(job_title(j), kf_app_parent(j->app), 0, NULL);
    j->pause_btn = gtk_dialog_add_button(GTK_DIALOG(j->dialog), _("_Pause"), RESPONSE_PAUSE);
    gtk_dialog_add_button(GTK_DIALOG(j->dialog), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL);
    gtk_window_set_icon_name(GTK_WINDOW(j->dialog), "system-file-manager");
    gtk_window_set_default_size(GTK_WINDOW(j->dialog), 380, -1);
    j->label = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(j->label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_misc_set_alignment(GTK_MISC(j->label), 0, 0.5);
    j->bar = gtk_progress_bar_new();
    gtk_container_set_border_width(GTK_CONTAINER(GTK_DIALOG(j->dialog)->vbox), 10);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(j->dialog)->vbox), j->label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(j->dialog)->vbox), j->bar, FALSE, FALSE, 4);
    g_signal_connect(j->dialog, "response", G_CALLBACK(on_job_response), j);
    /* Closing it (X, Escape) only cancels via "response": the dialog must
     * live until the worker stops, since this poll keeps updating it. */
    g_signal_connect(j->dialog, "delete-event", G_CALLBACK(gtk_true), NULL);
    gtk_widget_show_all(j->dialog);
    set_pause_button(j, j->paused);     /* a fallback mid-job may start paused */
}

static void job_start(Job *j)
{
    j->cancel = g_cancellable_new();
    g_mutex_init(&j->lock);
    g_cond_init(&j->cond);
    j->preparing = TRUE;
    j->started = g_get_monotonic_time();
    j->thread = g_thread_new("kifiles-job", job_thread, j);
    j->timer = g_timeout_add(100, job_poll, j);
    j->app->jobs = g_list_append(j->app->jobs, j);
}

/* With no parent the dialog stands alone (own taskbar entry) instead of
 * following the hidden window; jobs without one yet get it on the next poll. */
void kf_ops_detach(KfApp *app)
{
    for (GList *l = app->jobs; l; l = l->next) {
        Job *j = l->data;
        if (j->dialog)
            gtk_window_set_transient_for(GTK_WINDOW(j->dialog), NULL);
    }
}

static void start_delete_job(KfApp *app, GList *files, gboolean empty_trash);

static void job_done(Job *j)
{
    KfApp *app = j->app;
    app->jobs = g_list_remove(app->jobs, j);
    gboolean shown = j->dialog || j->view;
    if (j->dialog)
        gtk_widget_destroy(j->dialog);
    if (j->view) {
        /* KJob's codes: 1 = cancelled by the user (no notification), 100 =
         * the app's own error. */
        gboolean cancelled = g_cancellable_is_cancelled(j->cancel);
        kf_jobview_finish(j->view, cancelled ? 1 : j->errors ? 100 : 0,
                          j->errors ? _("Some items could not be processed") : NULL);
        j->view = NULL;
    }
    if (j->errors)
        show_errors(app, j->errors);
    else if (j->empty_trash)
        xis_sound_play("trash-empty");
    else if ((j->type == JOB_COPY || j->type == JOB_MOVE) && shown && !g_cancellable_is_cancelled(j->cancel))
        xis_sound_play("complete");        /* only long jobs: the ones with a progress dialog */

    switch (j->type) {
    case JOB_COPY:
    case JOB_MOVE: {
        GList *pairs = j->results;
        j->results = NULL;
        undo_push(app, j->type == JOB_MOVE ? U_MOVE : U_COPY, pairs);
        /* Show what arrived. */
        GList *panes = kf_app_panes(app);
        for (GList *pl = panes; pl; pl = pl->next) {
            KfPane *p = pl->data;
            if (p->dir && j->dest_dir && g_file_equal(p->dir, j->dest_dir)) {
                for (GList *l = pairs; l; l = l->next) {
                    char *b = g_file_get_basename(((Pair *)l->data)->b);
                    kf_pane_select_when_added(p, b);
                    g_free(b);
                }
            }
        }
        g_list_free(panes);
        break;
    }
    case JOB_TRASH:
        if (j->results)
            xis_sound_play("file-trash");
        undo_push(app, U_TRASH, j->results);
        j->results = NULL;
        if (j->untrashable) {
            GtkWidget *d = gtk_message_dialog_new(kf_app_parent(app), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
                                                  GTK_BUTTONS_NONE, "%s",
                                                  _("These items cannot be moved to the trash. Delete them permanently?"));
            gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), _("This cannot be undone."));
            gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                   GTK_STOCK_DELETE, GTK_RESPONSE_ACCEPT, NULL);
            if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
                GList *copy = NULL;
                for (GList *l = j->untrashable; l; l = l->next)
                    copy = g_list_append(copy, g_object_ref(l->data));
                start_delete_job(app, copy, FALSE);
            }
            gtk_widget_destroy(d);
        }
        break;
    default:
        break;
    }
    kf_app_status_free(app, 0, 0);      /* free space changed */
    job_free(j);
    if (app->quitting && !app->jobs)
        gtk_main_quit();
}

static Job *job_new(KfApp *app, JobType t)
{
    Job *j = g_new0(Job, 1);
    j->app = app;
    j->type = t;
    return j;
}

static void start_delete_job(KfApp *app, GList *files, gboolean empty_trash)
{
    Job *j = job_new(app, JOB_DELETE);
    j->empty_trash = empty_trash;
    for (GList *l = files; l; l = l->next) {
        Item *it = g_new0(Item, 1);
        it->src = g_object_ref(l->data);
        j->items = g_list_append(j->items, it);
    }
    kf_files_free(files);
    job_start(j);
}

void kf_ops_trash(KfApp *app, GList *files)
{
    if (!files)
        return;
    Job *j = job_new(app, JOB_TRASH);
    for (GList *l = files; l; l = l->next) {
        Item *it = g_new0(Item, 1);
        it->src = g_object_ref(l->data);
        j->items = g_list_append(j->items, it);
    }
    job_start(j);
}

void kf_ops_delete(KfApp *app, GList *files)
{
    if (!files)
        return;
    guint n = g_list_length(files);
    char *msg = g_strdup_printf(ngettext("Delete %u item permanently?", "Delete %u items permanently?", n), n);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, "%s", msg);
    g_free(msg);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), _("This cannot be undone."));
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_DELETE,
                           GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gboolean go = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    if (!go)
        return;
    Job *j = job_new(app, JOB_DELETE);
    for (GList *l = files; l; l = l->next) {
        Item *it = g_new0(Item, 1);
        it->src = g_object_ref(l->data);
        j->items = g_list_append(j->items, it);
    }
    job_start(j);
}

void kf_ops_empty_trash(KfApp *app)
{
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
                                          GTK_BUTTONS_NONE, "%s", _("Empty the trash?"));
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d),
        _("All items in the trash will be deleted permanently."));
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("_Empty Trash"), GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    gboolean go = gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT;
    gtk_widget_destroy(d);
    if (!go)
        return;
    GFile *trash = g_file_new_for_uri("trash:///");
    GFileEnumerator *en = g_file_enumerate_children(trash, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    GList *all = NULL;
    if (en) {
        GFileInfo *ci;
        while ((ci = g_file_enumerator_next_file(en, NULL, NULL))) {
            all = g_list_append(all, g_file_get_child(trash, g_file_info_get_name(ci)));
            g_object_unref(ci);
        }
        g_object_unref(en);
    }
    g_object_unref(trash);
    if (all)
        start_delete_job(app, all, TRUE);
}

/* ---- transfer with conflict resolution ----------------------------------- */

typedef enum { ASK, ALL_REPLACE, ALL_SKIP, ALL_KEEP } Policy;

/* 1 = replace/merge, 2 = skip, 3 = keep both, 0 = cancel */
static int ask_conflict(KfApp *app, GFile *src, GFile *dst, gboolean both_dirs, gboolean multiple, Policy *policy)
{
    char *name = display_name(dst);
    GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(app->win), GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        both_dirs ? _("A folder named \"%s\" already exists here.") : _("\"%s\" already exists here."), name);
    g_free(name);
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(d), "%s",
        both_dirs ? _("Merging copies the contents into the existing folder; files with the same name are replaced.")
                  : _("Do you want to replace it, keep both files, or skip this one?"));
    gtk_dialog_add_buttons(GTK_DIALOG(d), GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, _("_Skip"), 2,
                           _("Keep _Both"), 3, both_dirs ? _("_Merge") : _("_Replace"), 1, NULL);
    GtkWidget *all = NULL;
    if (multiple) {
        all = gtk_check_button_new_with_mnemonic(_("_Apply to all"));
        gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), all, FALSE, FALSE, 4);
        gtk_widget_show(all);
    }
    (void)src;
    int r = gtk_dialog_run(GTK_DIALOG(d));
    if (all && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(all)) && r > 0)
        *policy = r == 1 ? ALL_REPLACE : r == 2 ? ALL_SKIP : ALL_KEEP;
    gtk_widget_destroy(d);
    return r > 0 ? r : 0;
}

void kf_ops_transfer(KfApp *app, GList *srcs, GFile *dest_dir, gboolean move)
{
    Job *j = job_new(app, move ? JOB_MOVE : JOB_COPY);
    j->dest_dir = g_object_ref(dest_dir);
    Policy policy = ASK;
    gboolean multiple = g_list_length(srcs) > 1;
    GString *bad = NULL;

    for (GList *l = srcs; l; l = l->next) {
        GFile *src = l->data;
        char *base = g_file_get_basename(src);
        GFile *dst = g_file_get_child(dest_dir, base);
        GFile *src_parent = g_file_get_parent(src);
        gboolean same_dir = src_parent && g_file_equal(src_parent, dest_dir);
        if (src_parent)
            g_object_unref(src_parent);
        gboolean replace = FALSE;
        gboolean skip = FALSE;

        if (g_file_equal(src, dest_dir) || g_file_has_prefix(dest_dir, src)) {
            if (!bad)
                bad = g_string_new(NULL);
            g_string_append_printf(bad, _("\"%s\" cannot be copied into itself.\n"), base);
            skip = TRUE;
        } else if (same_dir && move) {
            skip = TRUE;                        /* moving onto itself: nothing to do */
        } else if (same_dir) {
            g_object_unref(dst);                /* duplicate next to the original */
            dst = unique_child(dest_dir, base);
            if (!dst)
                skip = TRUE;
        } else if (g_file_query_exists(dst, NULL)) {
            gboolean sd = g_file_query_file_type(src, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
            gboolean dd = g_file_query_file_type(dst, G_FILE_QUERY_INFO_NONE, NULL) == G_FILE_TYPE_DIRECTORY;
            int r = policy == ALL_REPLACE ? 1 : policy == ALL_SKIP ? 2 : policy == ALL_KEEP ? 3
                    : ask_conflict(app, src, dst, sd && dd, multiple, &policy);
            if (r == 0) {                       /* cancel everything */
                g_free(base);
                g_object_unref(dst);
                if (bad) g_string_free(bad, TRUE);
                job_free(j);
                return;
            }
            if (r == 2)
                skip = TRUE;
            else if (r == 3) {
                g_object_unref(dst);
                dst = unique_child(dest_dir, base);
                if (!dst)
                    skip = TRUE;
            } else
                replace = TRUE;
        }
        if (!skip) {
            Item *it = g_new0(Item, 1);
            it->src = g_object_ref(src);
            it->dst = dst;
            it->replace = replace;
            j->items = g_list_append(j->items, it);
        } else if (dst) {
            g_object_unref(dst);
        }
        g_free(base);
    }
    if (bad) {
        kf_app_message(app, GTK_MESSAGE_ERROR, _("Cannot copy a folder into itself"), bad->str);
        g_string_free(bad, TRUE);
    }
    if (!j->items) {
        job_free(j);
        return;
    }
    job_start(j);
}

/* ---- inline operations ---------------------------------------------------- */

static GtkWidget *name_dialog(KfApp *app, const char *title, const char *label, const char *initial, gboolean select_stem)
{
    GtkWidget *d = gtk_dialog_new_with_buttons(title, GTK_WINDOW(app->win), GTK_DIALOG_MODAL,
                                               GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(d), GTK_RESPONSE_OK);
    GtkWidget *box = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    GtkWidget *lab = gtk_label_new(label);
    gtk_misc_set_alignment(GTK_MISC(lab), 0, 0.5);
    GtkWidget *e = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(e), initial);
    gtk_entry_set_activates_default(GTK_ENTRY(e), TRUE);
    gtk_widget_set_size_request(e, 320, -1);
    gtk_box_pack_start(GTK_BOX(box), lab, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), e, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(d)->vbox), box, TRUE, TRUE, 0);
    gtk_widget_show_all(box);
    g_object_set_data(G_OBJECT(d), "kf-entry", e);
    if (select_stem) {
        const char *dot = g_utf8_strrchr(initial, -1, '.');
        glong end = dot && dot != initial ? g_utf8_pointer_to_offset(initial, dot) : -1;
        gtk_editable_select_region(GTK_EDITABLE(e), 0, end);
    }
    return d;
}

static char *run_name_dialog(GtkWidget *d)
{
    GtkWidget *e = g_object_get_data(G_OBJECT(d), "kf-entry");
    char *out = NULL;
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_OK) {
        out = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(e))));
        if (!*out || strchr(out, '/')) {
            g_free(out);
            out = NULL;
        }
    }
    gtk_widget_destroy(d);
    return out;
}

void kf_ops_rename(KfApp *app, GFile *f)
{
    GFileInfo *info = g_file_query_info(f, "standard::display-name,standard::edit-name", G_FILE_QUERY_INFO_NONE, NULL, NULL);
    const char *cur = info ? (g_file_info_get_edit_name(info) ? g_file_info_get_edit_name(info) : g_file_info_get_display_name(info)) : NULL;
    char *fallback = cur ? NULL : g_file_get_basename(f);
    GtkWidget *d = name_dialog(app, _("Rename"), _("New name:"), cur ? cur : fallback, TRUE);
    g_free(fallback);
    if (info)
        g_object_unref(info);
    char *name = run_name_dialog(d);
    if (!name)
        return;
    GError *err = NULL;
    GFile *nf = g_file_set_display_name(f, name, NULL, &err);
    if (!nf) {
        kf_app_message(app, GTK_MESSAGE_ERROR, _("Could not rename"), err->message);
        g_clear_error(&err);
    } else {
        undo_push(app, U_RENAME, g_list_append(NULL, pair_new(f, nf)));
        char *b = g_file_get_basename(nf);
        kf_pane_select_when_added(app->active, b);
        g_free(b);
        g_object_unref(nf);
    }
    g_free(name);
}

static void make_new(KfApp *app, GFile *dir, gboolean folder)
{
    char *initial = folder ? g_strdup(_("New Folder")) : g_strdup(_("New File"));
    GFile *probe = g_file_get_child(dir, initial);
    if (g_file_query_exists(probe, NULL)) {
        g_object_unref(probe);
        probe = unique_child(dir, initial);
        if (probe) {
            g_free(initial);
            initial = g_file_get_basename(probe);
        }
    }
    if (probe)
        g_object_unref(probe);
    GtkWidget *d = name_dialog(app, folder ? _("New Folder") : _("New File"), folder ? _("Folder name:") : _("File name:"),
                               initial, !folder);
    g_free(initial);
    char *name = run_name_dialog(d);
    if (!name)
        return;
    GFile *nf = g_file_get_child(dir, name);
    GError *err = NULL;
    gboolean ok;
    if (folder) {
        ok = g_file_make_directory(nf, NULL, &err);
    } else {
        GFileOutputStream *os = g_file_create(nf, G_FILE_CREATE_NONE, NULL, &err);
        ok = os != NULL;
        if (os) {
            g_output_stream_close(G_OUTPUT_STREAM(os), NULL, NULL);
            g_object_unref(os);
        }
    }
    if (!ok) {
        kf_app_message(app, GTK_MESSAGE_ERROR, folder ? _("Could not create the folder") : _("Could not create the file"),
                       err->message);
        g_clear_error(&err);
    } else {
        undo_push(app, U_NEW, g_list_append(NULL, pair_new(nf, NULL)));
        kf_pane_select_when_added(app->active, name);
    }
    g_object_unref(nf);
    g_free(name);
}

void kf_ops_new_folder(KfApp *app, GFile *dir) { make_new(app, dir, TRUE); }
void kf_ops_new_file(KfApp *app, GFile *dir) { make_new(app, dir, FALSE); }

/* ---- trash: restore and undo -------------------------------------------- */

/* The trash:/// entry whose original path is `orig`; the most recently
 * deleted one if there are several. */
static GFile *find_in_trash(const char *orig)
{
    GFile *trash = g_file_new_for_uri("trash:///");
    GFileEnumerator *en = g_file_enumerate_children(trash, "standard::name,trash::orig-path,trash::deletion-date",
                                                    G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, NULL);
    GFile *best = NULL;
    char *best_date = NULL;
    if (en) {
        GFileInfo *ci;
        while ((ci = g_file_enumerator_next_file(en, NULL, NULL))) {
            const char *op = g_file_info_get_attribute_byte_string(ci, "trash::orig-path");
            if (op && !strcmp(op, orig)) {
                const char *date = g_file_info_get_attribute_string(ci, "trash::deletion-date");
                if (!best || g_strcmp0(date, best_date) > 0) {
                    if (best)
                        g_object_unref(best);
                    g_free(best_date);
                    best = g_file_get_child(trash, g_file_info_get_name(ci));
                    best_date = g_strdup(date);
                }
            }
            g_object_unref(ci);
        }
        g_object_unref(en);
    }
    g_free(best_date);
    g_object_unref(trash);
    return best;
}

static gboolean restore_one(GFile *trashed, const char *orig_path, GError **err)
{
    GFile *dest = g_file_new_for_path(orig_path);
    GFile *parent = g_file_get_parent(dest);
    if (parent) {
        g_file_make_directory_with_parents(parent, NULL, NULL);
        g_object_unref(parent);
    }
    gboolean ok = g_file_move(trashed, dest, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, err);
    g_object_unref(dest);
    return ok;
}

void kf_ops_restore(KfApp *app, GList *files)
{
    GList *errors = NULL;
    for (GList *l = files; l; l = l->next) {
        GFileInfo *info = g_file_query_info(l->data, "trash::orig-path", G_FILE_QUERY_INFO_NONE, NULL, NULL);
        const char *orig = info ? g_file_info_get_attribute_byte_string(info, "trash::orig-path") : NULL;
        GError *err = NULL;
        if (!orig || !restore_one(l->data, orig, &err)) {
            char *n = display_name(l->data);
            errors = g_list_append(errors, g_strdup_printf("%s: %s", n, err ? err->message : _("original location unknown")));
            g_free(n);
            g_clear_error(&err);
        }
        if (info)
            g_object_unref(info);
    }
    if (errors) {
        show_errors(app, errors);
        g_list_free_full(errors, g_free);
    }
}

/* ---- undo ---------------------------------------------------------------- */

static void move_back(KfApp *app, GList *pairs, GList **errors)
{
    for (GList *l = pairs; l; l = l->next) {
        Pair *p = l->data;       /* a = original, b = where it is now */
        GError *err = NULL;
        if (!g_file_move(p->b, p->a, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, &err)) {
            char *n = display_name(p->b);
            *errors = g_list_append(*errors, g_strdup_printf("%s: %s", n, err->message));
            g_free(n);
            g_clear_error(&err);
        }
    }
    (void)app;
}

void kf_ops_undo(KfApp *app)
{
    Undo *u = g_queue_pop_head(app->undo);
    if (!u)
        return;
    GList *errors = NULL;
    switch (u->kind) {
    case U_MOVE:
    case U_RENAME:
        move_back(app, u->pairs, &errors);
        break;
    case U_COPY:
    case U_NEW: {
        /* Undo a copy / creation by trashing what it made. */
        GList *made = NULL;
        for (GList *l = u->pairs; l; l = l->next) {
            Pair *p = l->data;
            made = g_list_append(made, u->kind == U_COPY ? p->b : p->a);
        }
        for (GList *l = made; l; l = l->next) {
            GError *err = NULL;
            if (!g_file_trash(l->data, NULL, &err)) {
                char *n = display_name(l->data);
                errors = g_list_append(errors, g_strdup_printf("%s: %s", n, err->message));
                g_free(n);
                g_clear_error(&err);
            }
        }
        g_list_free(made);
        break;
    }
    case U_TRASH:
        for (GList *l = u->pairs; l; l = l->next) {
            Pair *p = l->data;
            char *orig = g_file_get_path(p->a);
            GFile *t = orig ? find_in_trash(orig) : NULL;
            GError *err = NULL;
            if (!t || !restore_one(t, orig, &err)) {
                char *n = display_name(p->a);
                errors = g_list_append(errors, g_strdup_printf("%s: %s", n, err ? err->message : _("not found in the trash")));
                g_free(n);
                g_clear_error(&err);
            }
            if (t)
                g_object_unref(t);
            g_free(orig);
        }
        break;
    }
    if (errors) {
        show_errors(app, errors);
        g_list_free_full(errors, g_free);
    }
    undo_free(u);
    kf_app_pane_changed(app, app->active, KF_CHANGE_SELECTION);
}

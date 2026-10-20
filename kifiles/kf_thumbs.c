/* kf_thumbs.c - thumbnails for the file views.
 *
 * Follows the freedesktop.org thumbnail spec: ~/.cache/thumbnails/{normal,large}/
 * <md5 of the URI>.png, validated by the Thumb::URI / Thumb::MTime text
 * chunks, and ~/.cache/thumbnails/fail/kifiles/ for files that couldn't be
 * thumbnailed (so they aren't retried).
 *
 * Generators, tried in this order:
 *   1. gdk-pixbuf, for every image format it has a loader for;
 *   2. external thumbnailers: *.thumbnailer files from $XDG_DATA_DIRS/
 *      thumbnailers/ and ~/.local/share/thumbnailers/ (the same files GNOME
 *      and Thunar's tumbler read -- ffmpegthumbnailer, evince, librsvg, ...).
 *      This is how new formats are plugged in: drop a .thumbnailer file.
 *
 * Work happens in a small thread pool; results are delivered on the main
 * loop, and tasks whose cancellable fired (the user left the folder) are
 * dropped without a callback. */
#include "kf.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <glib/gstdio.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#define MAX_IMAGE_BYTES (128u * 1024 * 1024)
#define FAIL_DIR "kifiles"

typedef struct {
    GFile *file;
    char *ctype;
    gint64 mtime;
    int px;
    GCancellable *cancel;
    KfThumbDone cb;
    gpointer data;
    GdkPixbuf *result;
} Task;

static GThreadPool *pool;
static GHashTable *gdk_mimes;       /* mime -> 1, formats gdk-pixbuf can read */
static GHashTable *external;        /* mime -> exec template (char*) */
static GHashTable *support_cache;   /* ctype -> GINT_TO_POINTER(1|2), main thread only */

static void load_thumbnailers(void)
{
    external = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GPtrArray *dirs = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(dirs, g_build_filename(g_get_user_data_dir(), "thumbnailers", NULL));
    for (const char *const *d = g_get_system_data_dirs(); *d; d++)
        g_ptr_array_add(dirs, g_build_filename(*d, "thumbnailers", NULL));
    for (guint i = 0; i < dirs->len; i++) {
        GDir *dir = g_dir_open(g_ptr_array_index(dirs, i), 0, NULL);
        if (!dir)
            continue;
        const char *name;
        while ((name = g_dir_read_name(dir))) {
            if (!g_str_has_suffix(name, ".thumbnailer"))
                continue;
            char *path = g_build_filename(g_ptr_array_index(dirs, i), name, NULL);
            GKeyFile *kf = g_key_file_new();
            if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
                char *exec = g_key_file_get_string(kf, "Thumbnailer Entry", "Exec", NULL);
                gsize n = 0;
                char **mimes = g_key_file_get_string_list(kf, "Thumbnailer Entry", "MimeType", &n, NULL);
                if (exec && mimes) {
                    for (gsize k = 0; k < n; k++)
                        /* earlier directories (the user's own) win */
                        if (!g_hash_table_contains(external, mimes[k]))
                            g_hash_table_insert(external, g_strdup(mimes[k]), g_strdup(exec));
                }
                g_free(exec);
                g_strfreev(mimes);
            }
            g_key_file_free(kf);
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_ptr_array_free(dirs, TRUE);
}

static const char *external_for(const char *ctype)
{
    const char *e = g_hash_table_lookup(external, ctype);
    if (e)
        return e;
    /* A thumbnailer for a parent type (e.g. text/plain for text/x-python). */
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, external);
    while (g_hash_table_iter_next(&it, &k, &v))
        if (g_content_type_is_a(ctype, k))
            return v;
    return NULL;
}

/* ---- cache paths --------------------------------------------------------- */

static char *cache_dir(const char *sub)
{
    return g_build_filename(g_get_user_cache_dir(), "thumbnails", sub, NULL);
}

static char *thumb_name(const char *uri)
{
    char *sum = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
    char *name = g_strconcat(sum, ".png", NULL);
    g_free(sum);
    return name;
}

static gboolean pixbuf_matches(GdkPixbuf *pb, const char *uri, gint64 mtime)
{
    const char *u = gdk_pixbuf_get_option(pb, "tEXt::Thumb::URI");
    const char *m = gdk_pixbuf_get_option(pb, "tEXt::Thumb::MTime");
    return u && m && !strcmp(u, uri) && g_ascii_strtoll(m, NULL, 10) == mtime;
}

static void save_png(GdkPixbuf *pb, const char *dir, const char *name, const char *uri, gint64 mtime)
{
    g_mkdir_with_parents(dir, 0700);
    char *final = g_build_filename(dir, name, NULL);
    char *tmp = g_strdup_printf("%s.%d.tmp", final, (int)getpid());
    char *ms = g_strdup_printf("%" G_GINT64_FORMAT, mtime);
    if (gdk_pixbuf_save(pb, tmp, "png", NULL, "tEXt::Thumb::URI", uri, "tEXt::Thumb::MTime", ms,
                        "tEXt::Software", "kifiles", NULL)) {
        g_chmod(tmp, 0600);
        g_rename(tmp, final);
    } else {
        g_unlink(tmp);
    }
    g_free(ms);
    g_free(tmp);
    g_free(final);
}

/* ---- generators ---------------------------------------------------------- */

static GdkPixbuf *make_with_gdk(const char *path, int tsz)
{
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, tsz, tsz, TRUE, NULL);
    if (!pb)
        return NULL;
    GdkPixbuf *rot = gdk_pixbuf_apply_embedded_orientation(pb);
    g_object_unref(pb);
    return rot;
}

/* %s size, %u uri, %i input path, %o output path, %% a percent sign. */
static GdkPixbuf *make_with_external(const char *tmpl, const char *uri, const char *path, int tsz, const char *outpath)
{
    GString *cmd = g_string_new(NULL);
    for (const char *c = tmpl; *c; c++) {
        if (*c != '%' || !c[1]) {
            g_string_append_c(cmd, *c);
            continue;
        }
        c++;
        char *q = NULL;
        switch (*c) {
        case 's': g_string_append_printf(cmd, "%d", tsz); break;
        case 'u': q = g_shell_quote(uri); break;
        case 'i': q = g_shell_quote(path); break;
        case 'o': q = g_shell_quote(outpath); break;
        case '%': g_string_append_c(cmd, '%'); break;
        default: break;
        }
        if (q) {
            g_string_append(cmd, q);
            g_free(q);
        }
    }
    gint argc = 0;
    char **argv = NULL;
    GdkPixbuf *pb = NULL;
    if (g_shell_parse_argv(cmd->str, &argc, &argv, NULL)) {
        /* Hung thumbnailers (corrupt media) must not pin a worker forever. */
        char *timeout = g_find_program_in_path("timeout");
        char **full = g_new0(char *, argc + 3);
        int n = 0;
        if (timeout) {
            full[n++] = timeout;
            full[n++] = (char *)"30";
        }
        for (int i = 0; i < argc; i++)
            full[n++] = argv[i];
        gint status = 0;
        if (g_spawn_sync(NULL, full, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, &status, NULL) && status == 0)
            pb = gdk_pixbuf_new_from_file(outpath, NULL);
        g_free(timeout);
        g_free(full);
        g_strfreev(argv);
    }
    g_string_free(cmd, TRUE);
    return pb;
}

static GdkPixbuf *make_thumb(Task *t)
{
    char *path = g_file_get_path(t->file);
    if (!path)
        return NULL;                        /* remote: not worth fetching for a thumbnail */
    char *uri = g_file_get_uri(t->file);
    const char *sub = t->px <= 128 ? "normal" : "large";
    int tsz = t->px <= 128 ? 128 : 256;
    char *name = thumb_name(uri);
    char *dir = cache_dir(sub);
    GdkPixbuf *pb = NULL;

    /* 1. cache */
    char *cached = g_build_filename(dir, name, NULL);
    pb = gdk_pixbuf_new_from_file(cached, NULL);
    g_free(cached);
    if (pb && !pixbuf_matches(pb, uri, t->mtime)) {
        g_object_unref(pb);
        pb = NULL;
    }
    if (pb)
        goto out;

    /* 2. a previous failure for this exact version of the file */
    char *faildir = g_build_filename(g_get_user_cache_dir(), "thumbnails", "fail", FAIL_DIR, NULL);
    char *failed = g_build_filename(faildir, name, NULL);
    GdkPixbuf *fp = gdk_pixbuf_new_from_file(failed, NULL);
    g_free(failed);
    if (fp) {
        gboolean same = pixbuf_matches(fp, uri, t->mtime);
        g_object_unref(fp);
        if (same) {
            g_free(faildir);
            goto out;
        }
    }

    /* 3. generate */
    struct stat st;
    if (g_stat(path, &st) == 0 && (guint64)st.st_size <= MAX_IMAGE_BYTES && g_hash_table_contains(gdk_mimes, t->ctype)) {
        pb = make_with_gdk(path, tsz);
    } else {
        const char *tmpl = external_for(t->ctype);
        if (tmpl) {
            g_mkdir_with_parents(dir, 0700);
            char *out = g_strdup_printf("%s/%s.%d.gen.png", dir, name, (int)getpid());
            pb = make_with_external(tmpl, uri, path, tsz, out);
            g_unlink(out);
            g_free(out);
            if (pb && (gdk_pixbuf_get_width(pb) > tsz || gdk_pixbuf_get_height(pb) > tsz)) {
                GdkPixbuf *fit = kf_icon_fit(pb, tsz);
                g_object_unref(pb);
                pb = fit;
            }
        }
    }
    if (pb) {
        save_png(pb, dir, name, uri, t->mtime);
    } else {
        GdkPixbuf *dot = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, 1, 1);
        if (dot) {
            gdk_pixbuf_fill(dot, 0);
            save_png(dot, faildir, name, uri, t->mtime);
            g_object_unref(dot);
        }
    }
    g_free(faildir);
out:
    g_free(dir);
    g_free(name);
    g_free(uri);
    g_free(path);
    return pb;
}

/* ---- pool ---------------------------------------------------------------- */

/* Decoding a picture to thumbnail it allocates its full-size pixels in a
 * worker thread. glibc raises its mmap threshold after such a buffer is
 * freed, so the next ones come from the worker's malloc arena instead, and
 * arena memory stays resident after free: a few hundred thumbnails left
 * the process with hundreds of MB it no longer used. A fixed threshold
 * keeps big buffers in mmap (returned on free), and once the queue drains
 * whatever the arenas still hold free goes back too. */

static guint pending;           /* requests not delivered yet, main thread only */
static guint trim_timer;

static gboolean trim_cb(gpointer data)
{
    (void)data;
    trim_timer = 0;
#ifdef __GLIBC__
    malloc_trim(0);
#endif
    return G_SOURCE_REMOVE;
}

static gboolean deliver(gpointer data)
{
    Task *t = data;
    if (!g_cancellable_is_cancelled(t->cancel))
        t->cb(t->file, t->result, t->data);
    if (t->result)
        g_object_unref(t->result);
    g_object_unref(t->file);
    g_object_unref(t->cancel);
    g_free(t->ctype);
    g_free(t);
    if (--pending == 0) {
        if (trim_timer)
            g_source_remove(trim_timer);
        trim_timer = g_timeout_add_seconds(2, trim_cb, NULL);
    }
    return FALSE;
}

void kf_thumbs_trim_soon(void)
{
    if (pending)
        return;                 /* the last delivery schedules it */
    if (trim_timer)
        g_source_remove(trim_timer);
    trim_timer = g_timeout_add_seconds(2, trim_cb, NULL);
}

static void worker(gpointer data, gpointer user)
{
    (void)user;
    Task *t = data;
    if (!g_cancellable_is_cancelled(t->cancel))
        t->result = make_thumb(t);
    g_idle_add(deliver, t);
}

void kf_thumbs_init(void)
{
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
    gdk_mimes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GSList *fmts = gdk_pixbuf_get_formats();
    for (GSList *l = fmts; l; l = l->next) {
        char **mimes = gdk_pixbuf_format_get_mime_types(l->data);
        for (char **m = mimes; m && *m; m++)
            g_hash_table_replace(gdk_mimes, g_strdup(*m), GINT_TO_POINTER(1));
        g_strfreev(mimes);
    }
    g_slist_free(fmts);
    load_thumbnailers();
    support_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    pool = g_thread_pool_new(worker, NULL, 2, FALSE, NULL);
}

gboolean kf_thumbs_supported(const char *ctype)
{
    if (!ctype || !*ctype)
        return FALSE;
    gpointer v = g_hash_table_lookup(support_cache, ctype);
    if (v)
        return GPOINTER_TO_INT(v) == 1;
    gboolean ok = g_hash_table_contains(gdk_mimes, ctype) || external_for(ctype) != NULL;
    g_hash_table_insert(support_cache, g_strdup(ctype), GINT_TO_POINTER(ok ? 1 : 2));
    return ok;
}

void kf_thumbs_request(GFile *file, const char *ctype, gint64 mtime, int px, GCancellable *cancel,
                       KfThumbDone cb, gpointer data)
{
    Task *t = g_new0(Task, 1);
    t->file = g_object_ref(file);
    t->ctype = g_strdup(ctype);
    t->mtime = mtime;
    t->px = px;
    t->cancel = g_object_ref(cancel);
    t->cb = cb;
    t->data = data;
    pending++;
    g_thread_pool_push(pool, t, NULL);
}

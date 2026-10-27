/* kfd_search.c - walks the folder tree in a thread and collects matches.
 *
 * Only names and stat() data are looked at (no file is opened), so the
 * type categories come from the name, like a file manager's icons do. The
 * UI drains the results with kfd_search_take() on a timer. */
#define _GNU_SOURCE
#include "kifind.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct KfdSearch {
    gint ref;
    gint cancel;
    KfdQuery q;
    gunichar *pat;              /* folded pattern; "abc" without '*' became "*abc*" */
    glong patlen;
    GMutex lock;                /* guards the three below */
    GPtrArray *pending;
    char *current;
    gboolean done;
};

void kfd_result_free(gpointer p)
{
    KfdResult *r = p;
    g_free(r->dir);
    g_free(r->name);
    g_free(r);
}

void kfd_query_clear(KfdQuery *q)
{
    g_free(q->root);
    g_free(q->pattern);
    if (q->uids)
        g_array_free(q->uids, TRUE);
    if (q->gids)
        g_array_free(q->gids, TRUE);
    memset(q, 0, sizeof(*q));
}

/* ---- name matching ---------------------------------------------------------- */

/* NFC (and case folded unless case_sensitive) UCS-4 copy of a UTF-8 string. */
static gunichar *fold(const char *s, gboolean case_sensitive, glong *len)
{
    char *n = g_utf8_normalize(s, -1, G_NORMALIZE_NFC);
    if (!n)
        n = g_strdup(s);
    if (!case_sensitive) {
        char *f = g_utf8_casefold(n, -1);
        g_free(n);
        n = f;
    }
    gunichar *u = g_utf8_to_ucs4_fast(n, -1, len);
    g_free(n);
    return u;
}

/* '*' matches any run of characters (also none); everything else itself. */
static gboolean glob(const gunichar *p, glong m, const gunichar *s, glong n)
{
    glong i = 0, j = 0, star = -1, mark = 0;
    while (j < n) {
        if (i < m && p[i] == '*') {
            star = i++;
            mark = j;
        } else if (i < m && p[i] == s[j]) {
            i++;
            j++;
        } else if (star >= 0) {
            i = star + 1;
            j = ++mark;
        } else {
            return FALSE;
        }
    }
    while (i < m && p[i] == '*')
        i++;
    return i == m;
}

/* A pattern without '*' finds names containing it. */
static gunichar *compile(const char *pattern, gboolean case_sensitive, glong *len)
{
    char *full = strchr(pattern, '*') ? g_strdup(pattern) : g_strconcat("*", pattern, "*", NULL);
    gunichar *p = fold(full, case_sensitive, len);
    g_free(full);
    return p;
}

static gboolean name_matches(KfdSearch *s, const char *name)
{
    if (!s->pat)
        return TRUE;
    char *disp = g_filename_display_name(name);
    glong n = 0;
    gunichar *u = fold(disp, s->q.case_sensitive, &n);
    gboolean ok = glob(s->pat, s->patlen, u, n);
    g_free(u);
    g_free(disp);
    return ok;
}

gboolean kfd_name_matches(const char *pattern, gboolean case_sensitive, const char *name)
{
    KfdSearch s = { 0 };
    s.q.case_sensitive = case_sensitive;
    if (*pattern)
        s.pat = compile(pattern, case_sensitive, &s.patlen);
    gboolean ok = name_matches(&s, name);
    g_free(s.pat);
    return ok;
}

/* ---- the other criteria (the name has matched already) --------------------------------------------------------- */

static gboolean in_range(gint64 v, gint64 from, gint64 to)
{
    return (from == KFD_NO_LIMIT || v >= from) && (to == KFD_NO_LIMIT || v <= to);
}

static gboolean has_prefix_any(const char *s, const char *const *list)
{
    for (; *list; list++)
        if (g_str_has_prefix(s, *list))
            return TRUE;
    return FALSE;
}

static guint classify(const char *name, const struct stat *st)
{
    if (S_ISDIR(st->st_mode))
        return KFD_T_DIR;
    static const char *const docs[] = {
        "application/pdf", "application/vnd.oasis.opendocument", "application/msword",
        "application/vnd.ms-", "application/vnd.openxmlformats", "application/rtf",
        "application/epub", "application/x-mobipocket", "image/vnd.djvu", NULL,
    };
    static const char *const archives[] = {
        "application/zip", "application/x-tar", "application/gzip", "application/x-gzip",
        "application/x-compressed-tar", "application/x-bzip", "application/x-xz", "application/x-7z",
        "application/x-rar", "application/vnd.rar", "application/zstd", "application/x-zstd",
        "application/x-lzma", "application/x-lzip", "application/x-cpio", "application/x-archive",
        "application/x-iso9660-image", "application/vnd.debian.binary-package", "application/x-rpm",
        "application/x-cd-image", "application/java-archive", NULL,
    };
    guint t = 0;
    char *ct = g_content_type_guess(name, NULL, 0, NULL);
    char *mime = ct ? g_content_type_get_mime_type(ct) : NULL;
    const char *m = mime ? mime : "";
    if (g_str_has_prefix(m, "image/") && !has_prefix_any(m, docs))
        t |= KFD_T_IMAGE;
    if (g_str_has_prefix(m, "audio/"))
        t |= KFD_T_AUDIO;
    if (g_str_has_prefix(m, "video/"))
        t |= KFD_T_VIDEO;
    if (g_str_has_prefix(m, "text/") || (ct && g_content_type_is_a(ct, "text/plain")))
        t |= KFD_T_TEXT;
    if (has_prefix_any(m, docs))
        t |= KFD_T_DOC;
    if (has_prefix_any(m, archives))
        t |= KFD_T_ARCHIVE;
    if (S_ISREG(st->st_mode) && (st->st_mode & 0111))
        t |= KFD_T_EXEC;
    g_free(mime);
    g_free(ct);
    return t ? t : KFD_T_OTHER;
}

static gboolean id_in(GArray *a, guint v)
{
    for (guint i = 0; i < a->len; i++)
        if ((guint)g_array_index(a, int, i) == v)
            return TRUE;
    return FALSE;
}

static gboolean wanted(KfdSearch *s, int dirfd, const char *name, const struct stat *st)
{
    const KfdQuery *q = &s->q;
    gboolean is_dir = S_ISDIR(st->st_mode);
    if ((q->size_from != KFD_NO_LIMIT || q->size_to != KFD_NO_LIMIT) &&
        (is_dir || !in_range(st->st_size, q->size_from, q->size_to)))
        return FALSE;
    if (!in_range(st->st_mtime, q->mtime_from, q->mtime_to))
        return FALSE;
    if (q->uids && !id_in(q->uids, st->st_uid))
        return FALSE;
    if (q->gids && !id_in(q->gids, st->st_gid))
        return FALSE;
    if (q->types && !(classify(name, st) & q->types))
        return FALSE;
    if (q->btime_from != KFD_NO_LIMIT || q->btime_to != KFD_NO_LIMIT) {
        /* not every file system records it; those files can't match */
        struct statx sx;
        if (statx(dirfd, name, AT_SYMLINK_NOFOLLOW, STATX_BTIME, &sx) != 0 || !(sx.stx_mask & STATX_BTIME) ||
            !in_range(sx.stx_btime.tv_sec, q->btime_from, q->btime_to))
            return FALSE;
    }
    return TRUE;
}

/* ---- the walk ---------------------------------------------------------------- */

static void set_current(KfdSearch *s, const char *dir)
{
    g_mutex_lock(&s->lock);
    g_free(s->current);
    s->current = g_strdup(dir);
    g_mutex_unlock(&s->lock);
}

/* Kernel views that are huge or never end; skipped unless searched directly. */
static gboolean skip_dir(KfdSearch *s, const char *path)
{
    static const char *const virt[] = { "/proc", "/sys", "/dev", "/run/user", NULL };
    for (const char *const *v = virt; *v; v++)
        if (!strcmp(path, *v) && strcmp(s->q.root, *v))
            return TRUE;
    return FALSE;
}

static void scan_dir(KfdSearch *s, const char *path, GPtrArray *subdirs, GPtrArray *found)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        return;
    DIR *d = fdopendir(fd);
    if (!d) {
        close(fd);
        return;
    }
    struct dirent *e;
    while (!g_atomic_int_get(&s->cancel) && (e = readdir(d))) {
        const char *name = e->d_name;
        if (!strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        if (name[0] == '.' && !s->q.hidden)
            continue;
        /* readdir's type is enough to walk; only matching names cost a stat
         * (an inode read, a seek on a cold disk) */
        gboolean is_dir = e->d_type == DT_DIR;
        gboolean have_st = FALSE;
        struct stat st;
        if (e->d_type == DT_UNKNOWN) {
            if (fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
                continue;
            have_st = TRUE;
            is_dir = S_ISDIR(st.st_mode);
        }
        if (name_matches(s, name) && (have_st || fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) == 0) &&
            wanted(s, fd, name, &st)) {
            KfdResult *r = g_new0(KfdResult, 1);
            r->dir = g_strdup(path);
            r->name = g_strdup(name);
            r->size = st.st_size;
            r->mtime = st.st_mtime;
            r->is_dir = S_ISDIR(st.st_mode);
            g_ptr_array_add(found, r);
        }
        /* symlinked folders are not followed: no loops, no leaving the tree */
        if (is_dir && s->q.subfolders) {
            char *sub = g_build_filename(path, name, NULL);
            if (skip_dir(s, sub))
                g_free(sub);
            else
                g_ptr_array_add(subdirs, sub);
        }
    }
    closedir(d);
}

static void publish(KfdSearch *s, GPtrArray *found)
{
    if (!found->len)
        return;
    g_mutex_lock(&s->lock);
    for (guint i = 0; i < found->len; i++)
        g_ptr_array_add(s->pending, found->pdata[i]);
    g_mutex_unlock(&s->lock);
    g_ptr_array_set_size(found, 0);
}

static gpointer worker(gpointer data)
{
    KfdSearch *s = data;
    GPtrArray *stack = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *found = g_ptr_array_new();
    GPtrArray *subdirs = g_ptr_array_new();
    gint64 last = g_get_monotonic_time();
    g_ptr_array_add(stack, g_strdup(s->q.root));
    while (stack->len && !g_atomic_int_get(&s->cancel)) {
        char *dir = g_ptr_array_steal_index(stack, stack->len - 1);
        set_current(s, dir);
        scan_dir(s, dir, subdirs, found);
        g_free(dir);
        /* reversed, so the stack pops them in directory order */
        for (guint i = subdirs->len; i > 0; i--)
            g_ptr_array_add(stack, subdirs->pdata[i - 1]);
        g_ptr_array_set_size(subdirs, 0);
        gint64 now = g_get_monotonic_time();
        if (now - last > 50000) {
            publish(s, found);
            last = now;
        }
    }
    publish(s, found);
    g_ptr_array_free(found, TRUE);
    g_ptr_array_free(subdirs, TRUE);
    g_ptr_array_free(stack, TRUE);
    g_mutex_lock(&s->lock);
    s->done = TRUE;
    g_mutex_unlock(&s->lock);
    kfd_search_unref(s);
    return NULL;
}

KfdSearch *kfd_search_start(KfdQuery *q)
{
    KfdSearch *s = g_new0(KfdSearch, 1);
    s->ref = 2;                 /* the caller's and the worker's */
    s->q = *q;
    memset(q, 0, sizeof(*q));
    if (s->q.pattern && *s->q.pattern)
        s->pat = compile(s->q.pattern, s->q.case_sensitive, &s->patlen);
    g_mutex_init(&s->lock);
    s->pending = g_ptr_array_new();
    g_thread_unref(g_thread_new("kifind-search", worker, s));
    return s;
}

GPtrArray *kfd_search_take(KfdSearch *s, char **current, gboolean *done)
{
    g_mutex_lock(&s->lock);
    GPtrArray *out = s->pending;
    s->pending = g_ptr_array_new();
    if (current)
        *current = g_strdup(s->current);
    if (done)
        *done = s->done;
    g_mutex_unlock(&s->lock);
    return out;
}

void kfd_search_cancel(KfdSearch *s)
{
    g_atomic_int_set(&s->cancel, 1);
}

void kfd_search_unref(KfdSearch *s)
{
    if (!g_atomic_int_dec_and_test(&s->ref))
        return;
    g_ptr_array_set_free_func(s->pending, kfd_result_free);
    g_ptr_array_free(s->pending, TRUE);
    g_free(s->current);
    g_free(s->pat);
    g_mutex_clear(&s->lock);
    kfd_query_clear(&s->q);
    g_free(s);
}

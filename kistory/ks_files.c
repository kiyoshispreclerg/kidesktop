/* ks_files - see ks_files.h. */
#include "ks_files.h"
#include "ks_config.h"
#include "ks_log.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

#define FD_REPEAT_S 3600   /* same app+file from fd sampling: once an hour */

static int ino = -1;
static int wd_share = -1, wd_kde = -1;
static char share_dir[PATH_MAX], kde_dir[PATH_MAX], home[PATH_MAX];

/* href -> last seen "modified" stamp (xbel), or name (kde). */
typedef struct {
    char *key;
    char *stamp;
} Seen;

static Seen *xbel_seen;
static int n_xbel, cap_xbel;
static Seen *kde_seen;
static int n_kde, cap_kde;

typedef struct {
    char *app;
    char *path;
    time_t at;
} FdSeen;
static FdSeen *fd_seen;
static int n_fd, cap_fd;

/* ---- small helpers ---- */

static Seen *seen_find(Seen *arr, int n, const char *key)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(arr[i].key, key))
            return &arr[i];
    return NULL;
}

/* Returns 1 if key is new or its stamp changed (and records it). */
static int seen_update(Seen **arr, int *n, int *cap, const char *key, const char *stamp)
{
    Seen *s = seen_find(*arr, *n, key);
    if (s) {
        if (!strcmp(s->stamp, stamp))
            return 0;
        free(s->stamp);
        s->stamp = strdup(stamp);
        return 1;
    }
    if (*n == *cap) {
        int ncap = *cap ? *cap * 2 : 256;
        Seen *na = realloc(*arr, sizeof(Seen) * (size_t)ncap);
        if (!na)
            return 0;
        *arr = na;
        *cap = ncap;
    }
    (*arr)[*n].key = strdup(key);
    (*arr)[*n].stamp = strdup(stamp);
    (*n)++;
    return 1;
}

/* file:///a%20b -> /a b; other URLs are returned as they are. */
static void url_to_path(const char *url, char *out, size_t outsz)
{
    const char *p = url;
    if (!strncmp(p, "file://", 7))
        p += 7;
    else {
        snprintf(out, outsz, "%s", url);
        return;
    }
    size_t o = 0;
    for (; *p && o + 1 < outsz; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            out[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* Value of attr="..." inside [tag, end). */
static int xml_attr(const char *tag, const char *end, const char *attr, char *out, size_t outsz)
{
    char pat[64];
    snprintf(pat, sizeof(pat), " %s=\"", attr);
    const char *p = strstr(tag, pat);
    if (!p || p >= end)
        return 0;
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q || q > end)
        return 0;
    size_t n = (size_t)(q - p) < outsz - 1 ? (size_t)(q - p) : outsz - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    /* The few entities GLib writes in attribute values. */
    char *w = out;
    for (char *r = out; *r; ) {
        if (!strncmp(r, "&amp;", 5))       { *w++ = '&';  r += 5; }
        else if (!strncmp(r, "&quot;", 6)) { *w++ = '"';  r += 6; }
        else if (!strncmp(r, "&apos;", 6)) { *w++ = '\''; r += 6; }
        else if (!strncmp(r, "&lt;", 4))   { *w++ = '<';  r += 4; }
        else if (!strncmp(r, "&gt;", 4))   { *w++ = '>';  r += 4; }
        else                               *w++ = *r++;
    }
    *w = '\0';
    return 1;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *buf = len >= 0 ? malloc((size_t)len + 1) : NULL;
    if (buf) {
        size_t got = fread(buf, 1, (size_t)len, f);
        buf[got] = '\0';
    }
    fclose(f);
    return buf;
}

static void log_file(const char *app, const char *exe, const char *path, const char *src, time_t ts,
                     long desktop, const char *output)
{
    if (ks_excluded(app, app, exe) || ks_titles_hidden(app, app, exe, NULL))
        return;
    char detail[32];
    snprintf(detail, sizeof(detail), "src=%s", src);
    ks_log_event(ts, "file", app, exe, desktop, output, path, detail);
}

/* ---- recently-used.xbel ---- */

static void scan_xbel(int baseline)
{
    char path[PATH_MAX + 32];
    snprintf(path, sizeof(path), "%s/recently-used.xbel", share_dir);
    char *xml = read_file(path);
    if (!xml)
        return;

    for (char *b = strstr(xml, "<bookmark "); b; b = strstr(b + 1, "<bookmark ")) {
        char *tag_end = strchr(b, '>');
        char *close = strstr(b, "</bookmark>");
        if (!tag_end)
            break;
        if (!close)
            close = tag_end;
        char href[PATH_MAX], stamp[64] = "";
        if (!xml_attr(b, tag_end, "href", href, sizeof(href)))
            continue;
        xml_attr(b, tag_end, "modified", stamp, sizeof(stamp));

        /* The app that used it last: latest <bookmark:application modified=...>. */
        char app[128] = "", exe[256] = "", app_stamp[64] = "";
        for (char *a = strstr(b, "<bookmark:application "); a && a < close;
             a = strstr(a + 1, "<bookmark:application ")) {
            char *aend = strchr(a, '>');
            char m[64] = "";
            if (!aend || !xml_attr(a, aend, "modified", m, sizeof(m)) || strcmp(m, app_stamp) <= 0)
                continue;
            snprintf(app_stamp, sizeof(app_stamp), "%s", m);
            xml_attr(a, aend, "name", app, sizeof(app));
            char execl[512] = "";
            xml_attr(a, aend, "exec", execl, sizeof(execl));
            char *e = execl;
            while (*e == '\'' || *e == '"' || *e == ' ')
                e++;
            size_t n = strcspn(e, " '\"");
            snprintf(exe, sizeof(exe), "%.*s", (int)n, e);
        }
        char key_stamp[160];
        snprintf(key_stamp, sizeof(key_stamp), "%s/%s", stamp, app_stamp);
        if (!seen_update(&xbel_seen, &n_xbel, &cap_xbel, href, key_stamp) || baseline)
            continue;
        char p[PATH_MAX];
        url_to_path(href, p, sizeof(p));
        log_file(app, exe, p, "xbel", time(NULL), -1, "");
    }
    free(xml);
}

/* ---- KDE RecentDocuments ---- */

static void scan_kde(int baseline)
{
    DIR *d = opendir(kde_dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (n < 9 || strcmp(e->d_name + n - 8, ".desktop"))
            continue;
        char path[PATH_MAX + 300];
        snprintf(path, sizeof(path), "%s/%s", kde_dir, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0)
            continue;
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%ld", (long)st.st_mtime);
        if (!seen_update(&kde_seen, &n_kde, &cap_kde, e->d_name, stamp) || baseline)
            continue;
        char *txt = read_file(path);
        if (!txt)
            continue;
        char *url = strstr(txt, "\nURL");
        if (url) {
            url = strchr(url, '=');
            if (url) {
                url++;
                url[strcspn(url, "\n")] = '\0';
                char p[PATH_MAX];
                url_to_path(url, p, sizeof(p));
                log_file("kde", "", p, "kde", time(NULL), -1, "");
            }
        }
        free(txt);
    }
    closedir(d);
}

/* ---- open files of the focused app ---- */

static int fd_path_wanted(const char *p)
{
    static const char *skip_dirs[] = { "/.cache/", "/.local/", "/.config/", "/.mozilla/", "/.var/",
                                       "/.git/", "/.thunderbird/", "/.pki/", "/.dbus/", "/.gnupg/" };
    static const char *skip_ext[] = { ".sqlite", ".sqlite-wal", ".sqlite-shm", ".db", "-journal", ".lock",
                                      ".log", ".ttf", ".otf", ".ldb", ".tmp", ".swp", ".swx", ".cache" };
    size_t hl = strlen(home);
    if (strncmp(p, home, hl) || p[hl] != '/')
        return 0;
    for (size_t i = 0; i < sizeof(skip_dirs) / sizeof(skip_dirs[0]); i++)
        if (strstr(p + hl, skip_dirs[i]))
            return 0;
    size_t n = strlen(p);
    for (size_t i = 0; i < sizeof(skip_ext) / sizeof(skip_ext[0]); i++) {
        size_t el = strlen(skip_ext[i]);
        if (n > el && !strcmp(p + n - el, skip_ext[i]))
            return 0;
    }
    const char *base = strrchr(p, '/');
    if (base && base[1] == '.')
        return 0;   /* dotfiles */
    return 1;
}

static int fd_recently_logged(const char *app, const char *path, time_t now)
{
    for (int i = 0; i < n_fd; i++) {
        if (strcmp(fd_seen[i].path, path) || strcmp(fd_seen[i].app, app))
            continue;
        if (now - fd_seen[i].at < FD_REPEAT_S)
            return 1;
        fd_seen[i].at = now;
        return 0;
    }
    if (n_fd == cap_fd) {
        int ncap = cap_fd ? cap_fd * 2 : 64;
        FdSeen *na = realloc(fd_seen, sizeof(FdSeen) * (size_t)ncap);
        if (!na)
            return 0;
        fd_seen = na;
        cap_fd = ncap;
    }
    fd_seen[n_fd].app = strdup(app);
    fd_seen[n_fd].path = strdup(path);
    fd_seen[n_fd].at = now;
    n_fd++;
    return 0;
}

void ks_files_focus_end(const XisWinIdent *id, time_t start, time_t end, int titles_hidden)
{
    if (!ks_conf.fd_sampling || titles_hidden || id->pid <= 0 || end - start < ks_conf.min_dwell_s)
        return;
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)id->pid);
    DIR *d = opendir(dir);
    if (!d)
        return;
    const char *app = id->wm_class[0] ? id->wm_class : ks_exe_base(id->exe);
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char link[384], path[PATH_MAX];
        snprintf(link, sizeof(link), "%s/%s", dir, e->d_name);
        ssize_t n = readlink(link, path, sizeof(path) - 1);
        if (n <= 0)
            continue;
        path[n] = '\0';
        struct stat st;
        if (!fd_path_wanted(path) || stat(path, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (fd_recently_logged(app, path, end))
            continue;
        log_file(app, id->exe, path, "fd", end, id->desktop, id->output);
    }
    closedir(d);
}

/* ---- inotify ---- */

int ks_files_init(void)
{
    const char *h = getenv("HOME");
    snprintf(home, sizeof(home), "%s", h ? h : "");
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(share_dir, sizeof(share_dir), "%s", xdg);
    else
        snprintf(share_dir, sizeof(share_dir), "%.*s/.local/share", (int)(sizeof(share_dir) - 16), home);
    snprintf(kde_dir, sizeof(kde_dir), "%.*s/RecentDocuments", (int)(sizeof(kde_dir) - 20), share_dir);

    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (ino < 0)
        return 0;
    /* The directory, not the file: GLib replaces the xbel by rename. */
    wd_share = inotify_add_watch(ino, share_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
    wd_kde = inotify_add_watch(ino, kde_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
    scan_xbel(1);
    scan_kde(1);
    return 1;
}

int ks_files_fd(void)
{
    return ino;
}

void ks_files_handle(void)
{
    char buf[8192] __attribute__((aligned(__alignof__(struct inotify_event))));
    int xbel = 0, kde = 0;
    ssize_t n;
    while ((n = read(ino, buf, sizeof(buf))) > 0) {
        for (char *p = buf; p < buf + n; ) {
            struct inotify_event *ev = (struct inotify_event *)(void *)p;
            if (ev->wd == wd_share && ev->len && !strcmp(ev->name, "recently-used.xbel"))
                xbel = 1;
            else if (ev->wd == wd_kde)
                kde = 1;
            p += sizeof(*ev) + ev->len;
        }
    }
    if (xbel)
        scan_xbel(0);
    if (kde)
        scan_kde(0);
    /* RecentDocuments may only appear after the first KDE app runs. */
    if (wd_kde < 0 && access(kde_dir, F_OK) == 0) {
        wd_kde = inotify_add_watch(ino, kde_dir, IN_CLOSE_WRITE | IN_MOVED_TO);
        scan_kde(0);
    }
}

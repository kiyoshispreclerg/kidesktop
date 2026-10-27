#include "xis_safefile.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_FILE (64u * 1024 * 1024)

static void (*g_logger)(const char *msg);

void xis_safefile_set_logger(void (*fn)(const char *msg))
{
    g_logger = fn;
}

static void warn(const char *fmt, ...)
{
    char msg[PATH_MAX + 160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (g_logger)
        g_logger(msg);
    else
        fprintf(stderr, "%s\n", msg);
}

/* ---- validators ---------------------------------------------------------- */

bool xis_check_usable(const char *text, size_t len)
{
    return len > 0 && memchr(text, '\0', len) == NULL;
}

bool xis_check_lines(const char *text, size_t len)
{
    return xis_check_usable(text, len) && text[len - 1] == '\n';
}

bool xis_check_pem(const char *text, size_t len)
{
    return xis_check_usable(text, len) && strstr(text, "-----BEGIN ") && strstr(text, "-----END ");
}

bool xis_check_conf_clean(const char *text, size_t len)
{
    if (!xis_check_usable(text, len))
        return false;
    const char *p = text;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t n = eol ? (size_t)(eol - p) : strlen(p);
        const char *s = p;
        while (n && (*s == ' ' || *s == '\t' || *s == '\r')) {
            s++;
            n--;
        }
        if (n && *s != '#' && *s != '[' && !memchr(s, '=', n))
            return false;
        p = eol ? eol + 1 : p + strlen(p);
    }
    return true;
}

/* ---- helpers ------------------------------------------------------------- */

/* The file a symlinked config really lives in, so replacing it does not
 * replace the link. */
static const char *resolve(const char *path, char *buf)
{
    struct stat st;
    if (lstat(path, &st) == 0 && S_ISLNK(st.st_mode) && realpath(path, buf))
        return buf;
    return path;
}

/* Reads `path` whole. NULL with errno on failure. */
static char *read_all(const char *path, size_t *len, mode_t *mode)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size > MAX_FILE) {
        int e = S_ISREG(st.st_mode) ? EFBIG : EINVAL;
        close(fd);
        errno = e;
        return NULL;
    }
    char *buf = malloc((size_t)st.st_size + 1);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    buf[got] = '\0';
    *len = got;
    if (mode)
        *mode = st.st_mode & 07777;
    return buf;
}

static void sync_dir_of(const char *path)
{
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        snprintf(dir, sizeof dir, ".");
    else if (slash == dir)
        dir[1] = '\0';
    else
        *slash = '\0';
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        if (fsync(fd) < 0) {
            /* best effort: not every filesystem can sync a directory */
        }
        close(fd);
    }
}

int xis_flush_sync(FILE *f)
{
    if (fflush(f) != 0)
        return -1;
    if (fsync(fileno(f)) < 0 && errno != EINVAL && errno != EROFS)
        return -1;
    return 0;
}

static int write_fd_all(int fd, const char *data, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        off += (size_t)n;
    }
    return 0;
}

/* Atomically replaces `target` with data, giving it `mode`. */
static int replace_atomic(const char *target, const char *data, size_t len, mode_t mode)
{
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", target) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0)
        return -1;
    int rc = fchmod(fd, mode);
    if (rc == 0)
        rc = write_fd_all(fd, data, len);
    if (rc == 0 && fsync(fd) < 0 && errno != EINVAL && errno != EROFS)
        rc = -1;
    int e = errno;
    if (close(fd) < 0 && rc == 0) {
        rc = -1;
        e = errno;
    }
    if (rc == 0 && rename(tmp, target) < 0) {
        rc = -1;
        e = errno;
    }
    if (rc < 0) {
        unlink(tmp);
        errno = e;
        return -1;
    }
    sync_dir_of(target);
    return 0;
}

/* ---- backup and read ----------------------------------------------------- */

int xis_backup_file(const char *path, xis_check_fn ok)
{
    char rbuf[PATH_MAX];
    path = resolve(path, rbuf);
    if (!ok)
        ok = xis_check_usable;

    size_t len = 0;
    mode_t mode = 0600;
    char *cur = read_all(path, &len, &mode);
    if (!cur) {
        if (errno == ENOENT)
            return 0;
        warn("could not read %s to back it up: %s", path, strerror(errno));
        return -1;
    }
    int rc = 0;
    if (!ok(cur, len)) {
        warn("%s looks corrupt; keeping the previous %s.backup", path, path);
        rc = -1;
    } else {
        char bak[PATH_MAX];
        if (snprintf(bak, sizeof bak, "%s.backup", path) >= (int)sizeof bak) {
            errno = ENAMETOOLONG;
            rc = -1;
        } else if (replace_atomic(bak, cur, len, mode) < 0) {
            rc = -1;
        }
        if (rc < 0)
            warn("could not write %s.backup: %s", path, strerror(errno));
    }
    free(cur);
    return rc;
}

/* The content to use for `path`, or NULL with errno. *from_backup says
 * which file it came from. */
static char *pick(const char *path, xis_check_fn ok, size_t *len, bool *from_backup)
{
    char rbuf[PATH_MAX];
    path = resolve(path, rbuf);
    if (!ok)
        ok = xis_check_usable;
    *from_backup = false;

    char *main_txt = read_all(path, len, NULL);
    int main_errno = errno;
    if (main_txt && ok(main_txt, *len))
        return main_txt;

    char bak[PATH_MAX];
    if (snprintf(bak, sizeof bak, "%s.backup", path) < (int)sizeof bak) {
        size_t blen = 0;
        char *b = read_all(bak, &blen, NULL);
        if (b && ok(b, blen)) {
            if (main_txt)
                warn("%s is empty or corrupt; using %s", path, bak);
            else
                warn("%s is missing; using %s", path, bak);
            free(main_txt);
            *len = blen;
            *from_backup = true;
            return b;
        }
        free(b);
    }
    /* No usable copy: behave as a plain read would. */
    if (!main_txt)
        errno = main_errno;
    return main_txt;
}

char *xis_read_file_or_backup(const char *path, xis_check_fn ok, size_t *len)
{
    size_t n = 0;
    bool bak;
    char *txt = pick(path, ok, &n, &bak);
    if (len)
        *len = n;
    return txt;
}

FILE *xis_fopen_read(const char *path, xis_check_fn ok)
{
    size_t n;
    bool bak;
    char *txt = pick(path, ok, &n, &bak);
    free(txt);
    if (bak) {
        char b[PATH_MAX];
        char rbuf[PATH_MAX];
        snprintf(b, sizeof b, "%s.backup", resolve(path, rbuf));
        return fopen(b, "r");
    }
    return fopen(path, "r");
}

/* ---- writing ------------------------------------------------------------- */

void xis_write_tmpname(const char *path, char *tmp, size_t tmpsz)
{
    char rbuf[PATH_MAX];
    snprintf(tmp, tmpsz, "%s.tmp", resolve(path, rbuf));
}

FILE *xis_write_open(const char *tmp, const char *path)
{
    char rbuf[PATH_MAX];
    path = resolve(path, rbuf);
    struct stat st;
    bool have = stat(path, &st) == 0 && S_ISREG(st.st_mode);
    unlink(tmp);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, have ? (st.st_mode & 07777) : 0666);
    if (fd < 0)
        return NULL;
    if (have && fchmod(fd, st.st_mode & 07777) < 0) {
        int e = errno;
        close(fd);
        unlink(tmp);
        errno = e;
        return NULL;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        int e = errno;
        close(fd);
        unlink(tmp);
        errno = e;
    }
    return f;
}

void xis_write_abort(FILE *f, const char *tmp)
{
    if (f)
        fclose(f);
    unlink(tmp);
}

int xis_write_commit(FILE *f, const char *tmp, const char *path, xis_check_fn ok)
{
    char rbuf[PATH_MAX];
    path = resolve(path, rbuf);
    int rc = xis_flush_sync(f);
    int e = errno;
    if (fclose(f) != 0 && rc == 0) {
        rc = -1;
        e = errno;
    }
    if (rc < 0) {
        unlink(tmp);
        errno = e;
        return -1;
    }
    if (ok)
        xis_backup_file(path, ok);
    if (rename(tmp, path) < 0) {
        e = errno;
        unlink(tmp);
        errno = e;
        return -1;
    }
    sync_dir_of(path);
    return 0;
}

static int write_file(const char *path, const char *data, size_t len, xis_check_fn backup_ok, bool backup)
{
    char tmp[PATH_MAX];
    xis_write_tmpname(path, tmp, sizeof tmp);
    FILE *f = xis_write_open(tmp, path);
    if (!f)
        return -1;
    if (len && fwrite(data, 1, len, f) != len) {
        int e = errno;
        xis_write_abort(f, tmp);
        errno = e;
        return -1;
    }
    return xis_write_commit(f, tmp, path, backup ? (backup_ok ? backup_ok : xis_check_usable) : NULL);
}

int xis_write_file(const char *path, const char *data, size_t len, xis_check_fn ok)
{
    return write_file(path, data, len, ok, true);
}

int xis_write_export(const char *path, const char *data, size_t len)
{
    return write_file(path, data, len, NULL, false);
}

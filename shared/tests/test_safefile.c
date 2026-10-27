/* Unit tests for xis_safefile: the power-cut cases are simulated with
 * empty / NUL-filled / truncated files, not a real power failure. */
#include "../xis_safefile.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fail;
static char g_dir[256];

#define CHECK(cond)                                                                 \
    do {                                                                            \
        if (!(cond)) {                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            g_fail++;                                                               \
        }                                                                           \
    } while (0)

static void quiet(const char *msg)
{
    (void)msg;
}

static char *path_of(const char *name)
{
    static char bufs[8][512];
    static int n;
    char *b = bufs[n++ % 8];
    snprintf(b, sizeof bufs[0], "%s/%s", g_dir, name);
    return b;
}

static void put(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if (f) {
        fwrite(data, 1, len, f);
        fclose(f);
    }
}

static char *slurp(const char *path)
{
    size_t n;
    return xis_read_file_or_backup(path, xis_check_usable, &n);
}

static int plain_exists(const char *path)
{
    return access(path, F_OK) == 0;
}

static mode_t mode_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mode & 07777 : 0;
}

static void test_first_and_second_write(void)
{
    char *p = path_of("a.conf");
    char *b = path_of("a.conf.backup");
    CHECK(xis_write_file(p, "one\n", 4, xis_check_lines) == 0);
    CHECK(!plain_exists(b));
    chmod(p, 0600);
    CHECK(xis_write_file(p, "two\n", 4, xis_check_lines) == 0);
    char *cur = slurp(p), *bak = slurp(b);
    CHECK(cur && strcmp(cur, "two\n") == 0);
    CHECK(bak && strcmp(bak, "one\n") == 0);
    CHECK(mode_of(b) == 0600);
    CHECK(mode_of(p) == 0600);
    CHECK(!plain_exists(path_of("a.conf.tmp")));
    free(cur);
    free(bak);
}

static void test_corrupt_main_keeps_backup(void)
{
    char *p = path_of("b.conf");
    char *b = path_of("b.conf.backup");
    put(p, "good\n", 5);
    put(b, "good\n", 5);
    put(p, "", 0);
    CHECK(xis_write_file(p, "new\n", 4, xis_check_lines) == 0);
    char *bak = slurp(b), *cur = slurp(p);
    CHECK(bak && strcmp(bak, "good\n") == 0);
    CHECK(cur && strcmp(cur, "new\n") == 0);
    free(bak);
    free(cur);

    /* zeroed (NUL) main */
    put(p, "\0\0\0\0", 4);
    CHECK(xis_write_file(p, "newer\n", 6, xis_check_lines) == 0);
    bak = slurp(b);
    CHECK(bak && strcmp(bak, "good\n") == 0);
    free(bak);
}

static void test_read_fallback(void)
{
    char *p = path_of("c.conf");
    char *b = path_of("c.conf.backup");
    size_t n = 0;
    errno = 0;
    char *t = xis_read_file_or_backup(p, NULL, &n);
    CHECK(t == NULL && errno == ENOENT); /* neither: same as a plain read */

    put(b, "from backup\n", 12);
    t = xis_read_file_or_backup(p, NULL, &n); /* main missing */
    CHECK(t && strcmp(t, "from backup\n") == 0 && n == 12);
    free(t);

    put(p, "", 0); /* main empty */
    t = xis_read_file_or_backup(p, NULL, &n);
    CHECK(t && strcmp(t, "from backup\n") == 0);
    free(t);

    put(p, "main wins\n", 10); /* main intact */
    t = xis_read_file_or_backup(p, NULL, &n);
    CHECK(t && strcmp(t, "main wins\n") == 0);
    free(t);

    unlink(b); /* empty main and no backup: the plain read's answer */
    put(p, "", 0);
    t = xis_read_file_or_backup(p, NULL, &n);
    CHECK(t && n == 0);
    free(t);

    put(b, "from backup\n", 12);
    put(p, "x\0y", 3);
    FILE *f = xis_fopen_read(p, NULL);
    char line[64] = { 0 };
    CHECK(f && fgets(line, sizeof line, f) && strcmp(line, "from backup\n") == 0);
    if (f)
        fclose(f);
}

static void test_lines_check(void)
{
    char *p = path_of("d.tsv");
    char *b = path_of("d.tsv.backup");
    put(p, "a\tb\nc\td", 7); /* cut in the middle of a line */
    CHECK(xis_write_file(p, "ok\n", 3, xis_check_lines) == 0);
    CHECK(!plain_exists(b));
}

static void test_conf_clean_and_pem(void)
{
    CHECK(xis_check_conf_clean("# c\n[s]\nk=v\n\n", 13));
    CHECK(!xis_check_conf_clean("k=v\ntypo here\n", 14));
    CHECK(xis_check_pem("-----BEGIN X-----\nabc\n-----END X-----\n", 38));
    CHECK(!xis_check_pem("-----BEGIN X-----\nabc\n", 22));
    CHECK(!xis_check_usable("", 0));
    CHECK(!xis_check_usable("a\0b", 3));

    char *p = path_of("e.conf");
    char *b = path_of("e.conf.backup");
    put(p, "k=v\nbroken line\n", 16); /* user typo: not a backup candidate */
    CHECK(xis_write_file(p, "k=w\n", 4, xis_check_conf_clean) == 0);
    CHECK(!plain_exists(b));
    /* ...but readable as is */
    size_t n;
    put(p, "k=v\nbroken line\n", 16);
    char *t = xis_read_file_or_backup(p, xis_check_usable, &n);
    CHECK(t && strstr(t, "broken"));
    free(t);
}

static void test_export_and_stream(void)
{
    char *p = path_of("f.out");
    CHECK(xis_write_export(p, "x", 1) == 0);
    CHECK(xis_write_export(p, "y", 1) == 0);
    CHECK(!plain_exists(path_of("f.out.backup")));

    char *q = path_of("g.conf");
    char tmp[512];
    xis_write_tmpname(q, tmp, sizeof tmp);
    FILE *f = xis_write_open(tmp, q);
    CHECK(f != NULL);
    fputs("s1\n", f);
    CHECK(xis_write_commit(f, tmp, q, xis_check_lines) == 0);
    f = xis_write_open(tmp, q);
    fputs("s2\n", f);
    CHECK(xis_write_commit(f, tmp, q, xis_check_lines) == 0);
    char *bak = slurp(path_of("g.conf.backup"));
    CHECK(bak && strcmp(bak, "s1\n") == 0);
    free(bak);

    f = xis_write_open(tmp, q);
    xis_write_abort(f, tmp);
    CHECK(!plain_exists(tmp));
}

int main(void)
{
    xis_safefile_set_logger(quiet);
    snprintf(g_dir, sizeof g_dir, "/tmp/xis_safefile_test.XXXXXX");
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 2;
    }
    umask(022);
    test_first_and_second_write();
    test_corrupt_main_keeps_backup();
    test_read_fallback();
    test_lines_check();
    test_conf_clean_and_pem();
    test_export_and_stream();

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "could not remove %s\n", g_dir);
    if (g_fail) {
        fprintf(stderr, "%d check(s) failed\n", g_fail);
        return 1;
    }
    puts("xis_safefile: all tests passed");
    return 0;
}

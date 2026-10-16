/* kilock - starts i3lock with the options set in kiconf's lock screen tab.
 *
 * i3lock has no config file, only command-line options, so this reads
 * $XDG_CONFIG_HOME/kilock.conf (flat key = value) and execs i3lock with
 * them. Both xss-lock (the session's idle/suspend locker) and the lock
 * hotkey run `kilock`, so one place decides how the screen looks. Extra
 * arguments are passed on to i3lock after the configured ones (e.g.
 * `kilock -n`). Unknown keys and bad values are ignored: a broken config
 * must never leave the screen unlocked, so the worst case is plain i3lock. */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define KILOCK_VERSION "0.1.0"
#define MAX_ARGS 32

typedef struct {
    char color[8];       /* rrggbb, "" = i3lock's own default */
    char image[PATH_MAX];
    int tiling;
    int indicator;       /* the unlock indicator (default on) */
    int beep;
    int ignore_empty;
    int show_failed;
    int pointer_default; /* keep the mouse pointer visible */
} Config;

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = '\0';
    return s;
}

static int is_hex_color(const char *s)
{
    if (strlen(s) != 6)
        return 0;
    for (int i = 0; i < 6; i++)
        if (!isxdigit((unsigned char)s[i]))
            return 0;
    return 1;
}

/* i3lock only reads PNG; anything else would make it fail or ignore it. */
static int is_png(const char *path)
{
    static const unsigned char magic[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    unsigned char buf[8];
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return n == sizeof(buf) && !memcmp(buf, magic, sizeof(buf));
}

static void config_path(char *out, size_t sz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, sz, "%s/kilock.conf", xdg);
    else
        snprintf(out, sz, "%s/.config/kilock.conf", home ? home : "");
}

static void config_load(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->indicator = 1;
    char path[PATH_MAX], line[PATH_MAX + 64];
    config_path(path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#')
            continue;
        char *eq = strchr(l, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = trim(l), *val = trim(eq + 1);
        if (!strcmp(key, "color")) {
            if (*val == '#')
                val++;
            if (is_hex_color(val))
                snprintf(c->color, sizeof(c->color), "%s", val);
        } else if (!strcmp(key, "image")) {
            snprintf(c->image, sizeof(c->image), "%s", val);
        } else if (!strcmp(key, "tiling")) {
            c->tiling = atoi(val) != 0;
        } else if (!strcmp(key, "indicator")) {
            c->indicator = atoi(val) != 0;
        } else if (!strcmp(key, "beep")) {
            c->beep = atoi(val) != 0;
        } else if (!strcmp(key, "ignore_empty")) {
            c->ignore_empty = atoi(val) != 0;
        } else if (!strcmp(key, "show_failed")) {
            c->show_failed = atoi(val) != 0;
        } else if (!strcmp(key, "pointer_default")) {
            c->pointer_default = atoi(val) != 0;
        }
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("kilock %s - i3lock with the options from ~/.config/kilock.conf\n\n"
               "Usage: kilock [i3lock options]\n",
               KILOCK_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        printf("kilock %s\n", KILOCK_VERSION);
        return 0;
    }

    Config c;
    config_load(&c);

    char *args[MAX_ARGS + 1];
    int n = 0;
    args[n++] = "i3lock";
    if (c.color[0]) {
        args[n++] = "-c";
        args[n++] = c.color;
    }
    if (c.image[0]) {
        if (is_png(c.image)) {
            args[n++] = "-i";
            args[n++] = c.image;
            if (c.tiling)
                args[n++] = "-t";
        } else {
            fprintf(stderr, "kilock: '%s' is not a readable PNG, locking without it\n", c.image);
        }
    }
    if (!c.indicator)
        args[n++] = "-u";
    if (c.beep)
        args[n++] = "-b";
    if (c.ignore_empty)
        args[n++] = "-e";
    if (c.show_failed)
        args[n++] = "-f";
    if (c.pointer_default) {
        args[n++] = "-p";
        args[n++] = "default";
    }
    for (int i = 1; i < argc && n < MAX_ARGS; i++)
        args[n++] = argv[i];
    args[n] = NULL;

    execvp("i3lock", args);
    fprintf(stderr, "kilock: could not run i3lock\n");
    return 127;
}

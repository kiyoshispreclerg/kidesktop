/* xis_sound - see xis_sound.h. */
#define _GNU_SOURCE
#include "xis_sound.h"

#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_MAPPED 64
#define DEBOUNCE_MS 100
#define RECHECK_MS 2000
#define PATHBUF (PATH_MAX * 3 + 256)

/* KDE-style file names: event -> "<Event>" in <Theme>-<Event>.ogg. */
static const struct { const char *event, *kde; } kde_names[] = {
    { "window-minimize",         "Window-Minimize" },
    { "window-close",            "Window-Close" },
    { "window-maximize",         "Window-Maximize" },
    { "window-shade",            "Window-Shade-Down" },
    { "window-unshade",          "Window-Shade-Up" },
    { "window-all-desktops",     "Window-All-Desktops" },
    { "window-all-desktops-not", "Window-All-Desktops-Not" },
    { "window-move",             "Window-Move" },
    { "window-move-stop",        "Window-Move-Stop" },
    { "login",                   "Sys-Log-In" },
    { "logout",                  "Sys-Log-Out" },
    { "trash-empty",             "Sys-Trash-Emptied" },
};

struct xs_map {
    struct { char event[48]; char path[PATH_MAX]; } e[MAX_MAPPED];
    int n;
};

static struct {
    bool loaded, enabled;
    char theme[64];
    int volume;
    const char *player;
    long long checked_ms;
    time_t conf_mtime;
    struct xs_map map;
    /* sound.<event>= lines of the settings file: a per-event file chosen
     * over the theme's, or "none" for silence. */
    struct { char event[48]; char path[PATH_MAX]; } over[MAX_MAPPED];
    int nover;
    struct { char event[48]; long long ms; } last[MAX_MAPPED];
} S;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
        *--e = '\0';
    return s;
}

static bool in_path(const char *prog)
{
    const char *p = getenv("PATH");
    if (!p)
        p = "/usr/local/bin:/usr/bin:/bin";
    char full[PATHBUF];
    while (*p) {
        size_t n = strcspn(p, ":");
        if (n > 0 && n < PATH_MAX) {
            snprintf(full, sizeof(full), "%.*s/%s", (int)n, p, prog);
            if (access(full, X_OK) == 0)
                return true;
        }
        p += n + (p[n] ? 1 : 0);
    }
    return false;
}

static void config_path(char *out, size_t sz)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, sz, "%s/kidesktop-sound.conf", xdg);
    else
        snprintf(out, sz, "%s/.config/kidesktop-sound.conf", home ? home : "");
}

/* "<root>/<rel>" for each data root, home first. */
static int data_roots(char roots[3][PATH_MAX])
{
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    int n = 0;
    if (xdg && *xdg)
        snprintf(roots[n++], PATH_MAX, "%s", xdg);
    else if (home && *home)
        snprintf(roots[n++], PATH_MAX, "%s/.local/share", home);
    snprintf(roots[n++], PATH_MAX, "/usr/local/share");
    snprintf(roots[n++], PATH_MAX, "/usr/share");
    return n;
}

static void load_kidesktop_theme(const char *theme, struct xs_map *m)
{
    char roots[3][PATH_MAX];
    int nr = data_roots(roots);
    for (int r = 0; r < nr; r++) {
        char dir[PATHBUF], path[PATHBUF + 16];
        snprintf(dir, sizeof(dir), "%s/kidesktop/sounds/%s", roots[r], theme);
        snprintf(path, sizeof(path), "%s/sounds.conf", dir);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char line[PATHBUF];
        while (fgets(line, sizeof(line), f) && m->n < MAX_MAPPED) {
            char *s = trim(line);
            if (*s == '#' || !*s)
                continue;
            char *eq = strchr(s, '=');
            if (!eq)
                continue;
            *eq = '\0';
            char *key = trim(s), *val = trim(eq + 1);
            if (!*key || !*val || strlen(key) >= sizeof(m->e[0].event))
                continue;
            int n = m->n++;
            snprintf(m->e[n].event, sizeof(m->e[n].event), "%s", key);
            if (val[0] == '/')
                snprintf(m->e[n].path, sizeof(m->e[n].path), "%s", val);
            else
                snprintf(m->e[n].path, sizeof(m->e[n].path), "%.*s/%s",
                         PATH_MAX / 2, dir, val);
        }
        fclose(f);
        return;
    }
}

static void load_config(void)
{
    S.map.n = 0;
    S.nover = 0;
    S.theme[0] = '\0';
    S.enabled = false;
    S.volume = 80;
    S.conf_mtime = 0;

    char path[PATHBUF];
    config_path(path, sizeof(path));
    struct stat st;
    if (stat(path, &st) == 0)
        S.conf_mtime = st.st_mtime;
    FILE *f = fopen(path, "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char *s = trim(line);
            char *eq = strchr(s, '=');
            if (*s == '#' || !eq)
                continue;
            *eq = '\0';
            char *key = trim(s), *val = trim(eq + 1);
            if (strncmp(key, "sound.", 6) == 0) {
                if (S.nover < MAX_MAPPED && key[6] && strlen(key + 6) < sizeof(S.over[0].event)) {
                    snprintf(S.over[S.nover].event, sizeof(S.over[0].event), "%s", key + 6);
                    snprintf(S.over[S.nover].path, sizeof(S.over[0].path), "%s", val);
                    S.nover++;
                }
            } else if (strcmp(key, "theme") == 0)
                snprintf(S.theme, sizeof(S.theme), "%s", val);
            else if (strcmp(key, "enabled") == 0)
                S.enabled = atoi(val) != 0;
            else if (strcmp(key, "volume") == 0) {
                int v = atoi(val);
                S.volume = v < 0 ? 0 : v > 100 ? 100 : v;
            }
        }
        fclose(f);
    }

    S.player = in_path("pw-play") ? "pw-play" : in_path("paplay") ? "paplay" : NULL;
    if (S.enabled && S.theme[0] && !strchr(S.theme, '/'))
        load_kidesktop_theme(S.theme, &S.map);
    S.loaded = true;
}

static void maybe_reload(void)
{
    long long t = now_ms();
    if (S.loaded && t - S.checked_ms < RECHECK_MS)
        return;
    S.checked_ms = t;
    if (S.loaded) {
        char path[PATHBUF];
        struct stat st;
        config_path(path, sizeof(path));
        time_t m = stat(path, &st) == 0 ? st.st_mtime : 0;
        if (m == S.conf_mtime)
            return;
    }
    load_config();
}

void xis_sound_reload(void)
{
    S.loaded = false;
}

/* Looks <event> up in freedesktop theme <theme> and the themes it
 * inherits. Names come from index.theme; a theme with no Directories=
 * line is searched in "stereo". */
static bool find_in_fd_theme(const char *theme, const char *event, int depth,
                             char *out, size_t sz)
{
    if (depth > 4 || !*theme || strchr(theme, '/'))
        return false;
    char roots[3][PATH_MAX];
    int nr = data_roots(roots);
    for (int r = 0; r < nr; r++) {
        char base[PATHBUF], idx[PATHBUF + 16];
        snprintf(base, sizeof(base), "%s/sounds/%s", roots[r], theme);
        snprintf(idx, sizeof(idx), "%s/index.theme", base);
        FILE *f = fopen(idx, "r");
        if (!f)
            continue;
        char dirs[512] = "stereo", inherits[512] = "", line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *s = trim(line);
            if (strncmp(s, "Directories=", 12) == 0)
                snprintf(dirs, sizeof(dirs), "%s", trim(s + 12));
            else if (strncmp(s, "Inherits=", 9) == 0)
                snprintf(inherits, sizeof(inherits), "%s", trim(s + 9));
        }
        fclose(f);

        char *save = NULL;
        for (char *d = strtok_r(dirs, ",", &save); d; d = strtok_r(NULL, ",", &save)) {
            static const char *ext[] = { "oga", "ogg", "wav" };
            for (int e = 0; e < 3; e++) {
                char cand[PATHBUF * 2];
                int n = snprintf(cand, sizeof(cand), "%s/%s/%s.%s", base, trim(d), event, ext[e]);
                if (n > 0 && (size_t)n < sizeof(cand) && (size_t)n < sz &&
                    access(cand, R_OK) == 0) {
                    memcpy(out, cand, (size_t)n + 1);
                    return true;
                }
            }
        }
        save = NULL;
        for (char *p = strtok_r(inherits, ",", &save); p; p = strtok_r(NULL, ",", &save))
            if (find_in_fd_theme(trim(p), event, depth + 1, out, sz))
                return true;
    }
    return false;
}

/* The file `theme` gives `event` (map = that theme's sounds.conf, already
 * loaded), ignoring per-event overrides. */
static bool resolve_theme(const struct xs_map *map, const char *theme, const char *event,
                          char *out, size_t sz)
{
    for (int i = 0; i < map->n; i++) {
        if (strcmp(map->e[i].event, event) == 0) {
            snprintf(out, sz, "%s", map->e[i].path);
            return access(out, R_OK) == 0;
        }
    }
    if (find_in_fd_theme(theme, event, 0, out, sz))
        return true;
    for (size_t i = 0; i < sizeof(kde_names) / sizeof(kde_names[0]); i++) {
        if (strcmp(kde_names[i].event, event) == 0) {
            snprintf(out, sz, "/usr/share/sounds/%s-%s.ogg", theme, kde_names[i].kde);
            return access(out, R_OK) == 0;
        }
    }
    return false;
}

static bool resolve(const char *event, char *out, size_t sz)
{
    for (int i = 0; i < S.nover; i++) {
        if (strcmp(S.over[i].event, event) == 0) {
            if (strcmp(S.over[i].path, "none") == 0)
                return false;
            snprintf(out, sz, "%s", S.over[i].path);
            return access(out, R_OK) == 0;
        }
    }
    return resolve_theme(&S.map, S.theme, event, out, sz);
}

/* True if `event` already played within DEBOUNCE_MS; otherwise records it. */
static bool debounced(const char *event)
{
    long long t = now_ms();
    int slot = -1;
    for (int i = 0; i < MAX_MAPPED; i++) {
        if (S.last[i].event[0] && strcmp(S.last[i].event, event) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && (!S.last[i].event[0] || t - S.last[i].ms >= DEBOUNCE_MS))
            slot = i;
    }
    if (slot < 0)
        return false;
    if (S.last[slot].event[0] && strcmp(S.last[slot].event, event) == 0 &&
        t - S.last[slot].ms < DEBOUNCE_MS)
        return true;
    snprintf(S.last[slot].event, sizeof(S.last[slot].event), "%s", event);
    S.last[slot].ms = t;
    return false;
}

static void spawn_player(const char *file, int volume)
{
    char vol[32];
    if (strcmp(S.player, "pw-play") == 0)
        snprintf(vol, sizeof(vol), "--volume=%.2f", volume / 100.0);
    else
        snprintf(vol, sizeof(vol), "--volume=%d", volume * 65536 / 100);

    pid_t pid = fork();
    if (pid < 0)
        return;
    if (pid == 0) {
        /* Double fork: the grandchild is reparented to init, so the
         * caller never has a zombie to reap or a SIGCHLD to handle. */
        if (fork() != 0)
            _exit(0);
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
            if (nul > 2)
                close(nul);
        }
        execlp(S.player, S.player, vol, file, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

void xis_sound_play(const char *event)
{
    if (!event || !*event)
        return;
    maybe_reload();
    if (!S.enabled || !S.theme[0] || !S.player || S.volume == 0)
        return;

    char file[PATHBUF];
    if (!resolve(event, file, sizeof(file)) || debounced(event))
        return;
    spawn_player(file, S.volume);
}

bool xis_sound_theme_file(const char *theme, const char *event, char *out, size_t sz)
{
    if (!theme || !*theme || !event || !*event)
        return false;
    static struct xs_map map;
    map.n = 0;
    if (!strchr(theme, '/'))
        load_kidesktop_theme(theme, &map);
    return resolve_theme(&map, theme, event, out, sz);
}

void xis_sound_play_file(const char *path, int volume_pct)
{
    maybe_reload();
    if (!S.player || !path || !*path)
        return;
    spawn_player(path, volume_pct < 0 ? 0 : volume_pct > 100 ? 100 : volume_pct);
}

/*
 * xisguard_events.c - xisguard's state and report stream, for the `xisguard`
 * widget and for toasts.
 *
 * Talks to the guard only through its documented control socket
 * (xisguard-ctl.<display>.sock, see xisguard/README.md): one long-lived
 * SUBSCRIBE connection that delivers the current protection state in its
 * reply and then REPORT/REQUEST/STATE lines, so nothing here polls. Any
 * other desktop's tray could do the same from the same contract.
 *
 * xispanel's main loop folds xisguard_events_fd() into its select() like
 * audio_events_fd(): -1 while the guard is not running (normal; retried on
 * a backoff), readable when lines are waiting. DISABLE/ENABLE go out on a
 * short-lived connection of their own, the guard answers with a STATE event
 * on the subscription.
 *
 * Needs "xispanel" in the guard's `subscribers=` list (the default).
 * Keeps a short in-memory history only; the guard keeps none.
 */
#include "xispanel.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define XG_RECONNECT_MS 3000
#define XG_LINE_MAX 1024
/* A program is "constantly" using something when its REPORTs for the same
 * action keep coming: XG_BURST_COUNT within XG_BURST_WINDOW_S. One toast
 * per program+action, then quiet for XG_BURST_COOLDOWN_S even if it goes on. */
#define XG_BURST_COUNT 4
#define XG_BURST_WINDOW_S 20
#define XG_BURST_COOLDOWN_S 300
#define XG_BURST_KEYS 16

#ifndef N_
#define N_(s) (s)
#endif

static int g_fd = -1;
static uint64_t g_retry_at_ms = 0;
static char g_buf[XG_LINE_MAX * 2];
static size_t g_buf_len = 0;

static XisguardEvents g_state = { .enabled = 1 };

static void ctl_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    const char *disp = getenv("DISPLAY");
    int dispnum = (disp && disp[0] == ':') ? atoi(disp + 1) : 0;
    snprintf(out, outsz, "%s/xisguard-ctl.%d.sock", rundir, dispnum);
}

static int ctl_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return -1;
    }
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    ctl_path(addr.sun_path, sizeof(addr.sun_path));
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Value of a flat string key in one JSON line; 0 if absent. */
static int jstr(const char *line, const char *key, char *out, size_t outsz)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(line, pat);
    if (!p) {
        return 0;
    }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\' && p[1]) {
            p++;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return 1;
}

static int jbool(const char *line, const char *key, int defval)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(line, pat);
    if (!p) {
        return defval;
    }
    return strncmp(p + strlen(pat), "true", 4) == 0;
}

static void set_enabled(int enabled, int announce)
{
    if (g_state.enabled == enabled) {
        return;
    }
    g_state.enabled = enabled;
    g_state.generation++;
    if (!announce) {
        return;
    }
    cairo_surface_t *icon = xispanel_first_panel_icon(enabled ? "security-high" : "security-low", 32);
    if (enabled) {
        toast_show_osd(icon, _("XisGuard"), _("Prote\xc3\xa7\xc3\xa3o Xnotify reativada"), -1, TOAST_URGENCY_NORMAL, 0,
                       "xisguard-state");
    } else {
        toast_show_osd(icon, _("Prote\xc3\xa7\xc3\xa3o DESLIGADA"),
                       _("Reativa sozinha se o XisGuard parar"), -1, TOAST_URGENCY_CRITICAL, 8000, "xisguard-state");
    }
}

/* What a guard action means, as a verb phrase after the program's name. */
const char *xisguard_action_text(const char *action)
{
    static const struct {
        const char *name;
        const char *text;
    } map[] = {
        { "ATTACH", N_("usa mem\xc3\xb3ria compartilhada") },
        { "SELECTION", N_("acessa a \xc3\xa1rea de transfer\xc3\xaancia") },
        { "COMPOSITE", N_("acessa outras janelas") },
        { "SCREEN", N_("captura a tela") },
        { "RECORD", N_("grava eventos (teclas)") },
        { "CURSOR", N_("l\xc3\xaa o cursor do mouse") },
        { "INPUT_GRAB", N_("captura mouse/teclado") },
        { "INPUT_INJECT", N_("insere teclas") },
        { "HOTKEY", N_("registra atalhos globais") },
        { "INPUT", N_("captura entrada sem foco") },
        { "MANAGE", N_("lista outras janelas") },
        { "GRAB_OVERRIDE", N_("toma o controle de entrada") },
        { "WARP", N_("move o cursor do mouse") },
        { "FOCUS", N_("rouba o foco") },
        { "RANDR", N_("altera a configura\xc3\xa7\xc3\xa3o das telas") },
        { "OVERLAY", N_("cria janela sobreposta") },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(map[i].name, action) == 0) {
            return _(map[i].text);
        }
    }
    return action;
}

/* One entry per program (not per program+action): its recent REPORT burst
 * and the distinct actions seen, shown together in one toast. */
#define XG_BURST_ACTIONS 8
typedef struct {
    char exe[512];
    char acts[XG_BURST_ACTIONS][16];
    int n_acts;
    int count;
    time_t window_start, last_toast;
    char tag[32]; /* the toast's tag, so a later action updates it in place */
} BurstKey;
static BurstKey g_burst[XG_BURST_KEYS];

/* The toast's button: adds the program to the guard's ignore.conf. The exe
 * as the guard reports it is "path" or "path|args" and ignore.conf patterns
 * are plain globs over that whole string, so a program that was seen with
 * arguments gets both "path" and "path|*". */
static void ignore_program(void *ctx)
{
    const BurstKey *k = ctx;
    char path[512];
    snprintf(path, sizeof(path), "%s", k->exe);
    char *bar = strchr(path, '|');
    if (bar) {
        *bar = '\0';
    }
    if (!path[0]) {
        return;
    }
    xisguard_send_json_arg("ADD_IGNORED", path);
    if (bar) {
        char with_args[520];
        snprintf(with_args, sizeof(with_args), "%s|*", path);
        xisguard_send_json_arg("ADD_IGNORED", with_args);
    }
    toast_show_osd(xispanel_first_panel_icon("security-high", 32), _("XisGuard"), _("Programa ignorado"), -1,
                   TOAST_URGENCY_NORMAL, 0, "xisguard-ignore");
}

/* "firefox: captura a tela, grava eventos (teclas)" */
static void burst_text(const BurstKey *k, char *out, size_t outsz)
{
    const char *base = strrchr(k->exe, '/');
    size_t o = (size_t)snprintf(out, outsz, "%s: ", base ? base + 1 : (k->exe[0] ? k->exe : "?"));
    for (int i = 0; i < k->n_acts && o + 1 < outsz; i++) {
        int n = snprintf(out + o, outsz - o, "%s%s", i ? ", " : "", xisguard_action_text(k->acts[i]));
        if (n < 0) {
            break;
        }
        o += (size_t)n;
    }
}

/* One toast per program when it keeps using things (XG_BURST_COUNT REPORTs
 * of any action within XG_BURST_WINDOW_S). Another action from the same
 * program while that toast is still up rewrites it in place; once it is
 * gone, the program stays quiet for XG_BURST_COOLDOWN_S. */
static void burst_check(const char *action, const char *exe, time_t ts)
{
    BurstKey *k = NULL, *oldest = &g_burst[0];
    for (int i = 0; i < XG_BURST_KEYS; i++) {
        if (!strcmp(g_burst[i].exe, exe)) {
            k = &g_burst[i];
            break;
        }
        if (g_burst[i].window_start < oldest->window_start) {
            oldest = &g_burst[i];
        }
    }
    if (!k) {
        k = oldest;
        memset(k, 0, sizeof(*k));
        snprintf(k->exe, sizeof(k->exe), "%s", exe);
        snprintf(k->tag, sizeof(k->tag), "xisguard-use-%d", (int)(k - g_burst));
    }
    int showing = toast_has_tag(k->tag);
    if (k->count == 0 || ts - k->window_start > XG_BURST_WINDOW_S) {
        k->window_start = ts;
        k->count = 0;
        if (!showing) {
            k->n_acts = 0;
        }
    }
    k->count++;

    int known = 0;
    for (int i = 0; i < k->n_acts; i++) {
        known |= !strcmp(k->acts[i], action);
    }
    if (!known && k->n_acts < XG_BURST_ACTIONS) {
        snprintf(k->acts[k->n_acts++], sizeof(k->acts[0]), "%s", action);
    }

    char body[300];
    burst_text(k, body, sizeof(body));
    cairo_surface_t *icon = xispanel_first_panel_icon("security-medium", 32);
    if (showing) {
        if (!known) {
            toast_show_action(icon, _("Uso constante detectado"), body, 10000, _("Ignorar este programa"),
                              ignore_program, k, k->tag);
        }
        return;
    }
    if (k->count < XG_BURST_COUNT || (k->last_toast && ts - k->last_toast < XG_BURST_COOLDOWN_S)) {
        return;
    }
    k->last_toast = ts;
    k->count = 0;
    toast_show_action(icon, _("Uso constante detectado"), body, 10000, _("Ignorar este programa"), ignore_program, k,
                      k->tag);
}

/* ---- programs in the guard's ignore.conf that are acting ---- */

#define XG_IGNORED_KEYS 16
static struct {
    char exe[512];
    time_t ts;
} g_ignored[XG_IGNORED_KEYS];

static void ignored_seen(const char *exe, time_t ts)
{
    int slot = 0;
    for (int i = 0; i < XG_IGNORED_KEYS; i++) {
        if (!strcmp(g_ignored[i].exe, exe)) {
            slot = i;
            break;
        }
        if (g_ignored[i].ts < g_ignored[slot].ts) {
            slot = i;
        }
    }
    snprintf(g_ignored[slot].exe, sizeof(g_ignored[slot].exe), "%s", exe);
    g_ignored[slot].ts = ts;
}

int xisguard_events_ignored_active(void)
{
    int n = 0;
    time_t now = time(NULL);
    for (int i = 0; i < XG_IGNORED_KEYS; i++) {
        n += g_ignored[i].exe[0] && now - g_ignored[i].ts < XISGUARD_ACTIVE_S;
    }
    return n;
}

static void history_add(const char *ev, const char *action, const char *exe, long ts)
{
    XisguardHistoryEntry *h = &g_state.history[g_state.history_head];
    snprintf(h->ev, sizeof(h->ev), "%s", ev);
    snprintf(h->action, sizeof(h->action), "%s", action);
    snprintf(h->exe, sizeof(h->exe), "%s", exe);
    h->ts = (time_t)ts;
    g_state.history_head = (g_state.history_head + 1) % XISGUARD_HISTORY_MAX;
    if (g_state.history_count < XISGUARD_HISTORY_MAX) {
        g_state.history_count++;
    }
    g_state.generation++;
}

static void handle_line(const char *line)
{
    char ev[16] = "";
    if (jstr(line, "ev", ev, sizeof(ev))) {
        if (strcmp(ev, "STATE") == 0) {
            set_enabled(jbool(line, "enabled", 1), 1);
        } else if (jbool(line, "ignored", 0)) {
            /* In the guard's ignore.conf: only counted, never listed. */
            char exe[512] = "";
            jstr(line, "exe", exe, sizeof(exe));
            const char *t = strstr(line, "\"ts\":");
            ignored_seen(exe, t ? (time_t)atol(t + 5) : time(NULL));
        } else {
            char action[16] = "", exe[512] = "";
            jstr(line, "action", action, sizeof(action));
            jstr(line, "exe", exe, sizeof(exe));
            const char *t = strstr(line, "\"ts\":");
            long ts = t ? atol(t + 5) : (long)time(NULL);
            history_add(ev, action, exe, ts);
            if (strcmp(ev, "REPORT") == 0) {
                burst_check(action, exe, (time_t)ts);
            }
        }
        return;
    }
    /* The SUBSCRIBE reply: {"ok":true,"subscribed":true,"enabled":bool} */
    if (strstr(line, "\"subscribed\":true")) {
        g_state.connected = 1;
        g_state.generation++;
        /* Not announced: this is the baseline, not a change. */
        g_state.enabled = jbool(line, "enabled", 1);
    }
}

static void drop_connection(void)
{
    if (g_fd >= 0) {
        close(g_fd);
    }
    g_fd = -1;
    g_buf_len = 0;
    if (g_state.connected || !g_state.enabled) {
        g_state.connected = 0;
        /* The guard is gone, so the server re-enabled itself. */
        g_state.enabled = 1;
        g_state.generation++;
    }
    g_retry_at_ms = now_ms() + XG_RECONNECT_MS;
}

int xisguard_events_fd(void)
{
    if (g_fd >= 0) {
        return g_fd;
    }
    if (now_ms() < g_retry_at_ms) {
        return -1;
    }
    g_retry_at_ms = now_ms() + XG_RECONNECT_MS;
    int fd = ctl_connect();
    if (fd < 0) {
        return -1;
    }
    static const char req[] = "{\"cmd\":\"SUBSCRIBE\"}\n";
    if (write(fd, req, sizeof(req) - 1) != (ssize_t)(sizeof(req) - 1)) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    g_fd = fd;
    g_buf_len = 0;
    return g_fd;
}

void xisguard_events_poll(void)
{
    if (g_fd < 0) {
        return;
    }
    for (;;) {
        ssize_t n = recv(g_fd, g_buf + g_buf_len, sizeof(g_buf) - 1 - g_buf_len, MSG_DONTWAIT);
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            drop_connection();
            return;
        }
        if (n < 0) {
            break;
        }
        g_buf_len += (size_t)n;
        g_buf[g_buf_len] = '\0';
        char *start = g_buf, *nl;
        while ((nl = strchr(start, '\n'))) {
            *nl = '\0';
            handle_line(start);
            start = nl + 1;
        }
        g_buf_len = strlen(start);
        memmove(g_buf, start, g_buf_len + 1);
        if (g_buf_len >= sizeof(g_buf) - 1) { /* a line longer than we ever expect */
            g_buf_len = 0;
        }
    }
    /* A refused SUBSCRIBE ({"ok":false,...}) is closed by the guard, which
     * lands in the n == 0 branch above and retries on the backoff. */
}

const XisguardEvents *xisguard_events_get(void)
{
    return &g_state;
}

void xisguard_send_json_arg(const char *cmd, const char *pattern)
{
    char esc[1100] = "";
    size_t o = 0;
    for (const char *p = pattern; *p && o + 2 < sizeof(esc); p++) {
        if (*p == '"' || *p == '\\') {
            esc[o++] = '\\';
        }
        esc[o++] = *p;
    }
    esc[o] = '\0';
    int fd = ctl_connect();
    if (fd < 0) {
        return;
    }
    char req[1300];
    int len = snprintf(req, sizeof(req), "{\"cmd\":\"%s\",\"pattern\":\"%s\"}\n", cmd, esc);
    (void)!write(fd, req, (size_t)len);
    close(fd);
}

void xisguard_send_command(const char *cmd)
{
    int fd = ctl_connect();
    if (fd < 0) {
        return;
    }
    char req[64];
    int len = snprintf(req, sizeof(req), "{\"cmd\":\"%s\"}\n", cmd);
    (void)!write(fd, req, (size_t)len);
    close(fd);
}

/*
 * xiskeys - global hotkey daemon for KiDesktop.
 *
 * Owns XGrabKey() on the root window for *stateless, environment-level*
 * actions: nothing here needs any other daemon's live in-process state, it
 * just runs a shell command (task manager, launcher, lock, media/volume/
 * brightness keys, screenshot, power/session actions...). Reads a config
 * file mapping a key spec to a shell command; on KeyPress it forks the
 * matching command, same fire-and-forget spawn style as xisback's click
 * actions.
 *
 * Config: $XDG_CONFIG_HOME/xiskeys.conf (fallback ~/.config/xiskeys.conf),
 * tab-separated lines:
 *
 *   BIND\t<action-name>\t<key-spec>\t<shell command>
 *
 * <key-spec> is "<Mod>+<Mod>+...+<Key>" (Ctrl/Alt/Shift/Meta, X11 keysym
 * name for <Key>), the same grammar xispanel's hotkey.c uses for its own
 * widget-local grabs. <action-name> is just a label for logs/config
 * readability, not looked up anywhere (yet -- a future kiconf "Shortcuts"
 * tab would edit this file by action name).
 *
 * If the config file doesn't exist, a default one is written covering the
 * baseline set of shortcuts a usable desktop needs, so the daemon is
 * immediately useful and the file is there to edit. SIGHUP reloads the
 * config (ungrab everything, re-read, re-grab) without restarting.
 *
 * What xiskeys deliberately does NOT own: any hotkey whose action needs a
 * *specific other daemon's* live state to do anything meaningful --
 * xispanel's launcher popup needs to know its own on-screen anchor rect to
 * position xisserve (widgets/xisserve.c), and kiwm's window/desktop actions
 * (Alt+Tab, maximize, desktop grid...) need kiwm's live window/workspace
 * state. Those stay owned by the daemon that actually has that state, via
 * that daemon's OWN config (xispanel's `hotkey=<spec>` widget option,
 * already implemented in hotkey.c; kiwm's own equivalent once it exists),
 * grabbed locally by that daemon -- not relayed through xiskeys at
 * runtime. Earlier drafts of the KiDesktop plan had xiskeys grab
 * everything and forward events to whichever client needed them; this
 * turned out to be needless indirection once xispanel already proved the
 * "each daemon grabs its own widget-anchored/stateful hotkeys locally"
 * pattern works (xisserve/globalmenu/folder's hotkey= options). The one
 * real risk that model creates -- two processes silently racing for the
 * same XGrabKey, where the second grab just fails with no error -- is
 * handled by scan_external_hotkeys() below: xiskeys reads xispanel's (and,
 * once it exists, kiwm's) own config at load time and refuses to grab
 * anything already claimed there, logging why instead of attempting a
 * doomed XGrabKey.
 */

#include <X11/Xlib.h>
#include <X11/keysym.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>

#define XISKEYS_VERSION "0.2.0"
#define MAX_BINDINGS 128
#define LINE_MAX_LEN 768
#define CMD_MAX_LEN 512

typedef struct {
    char action[64];
    char spec[64];
    char command[CMD_MAX_LEN];
    KeyCode keycode;
    unsigned int modifiers; /* Control/Mod1/Shift/Mod4 bits only, Lock/NumLock stripped */
} Binding;

static Display *g_dpy;
static Window g_root;
static char g_configpath[PATH_MAX];
static volatile sig_atomic_t g_quit = 0;
static volatile sig_atomic_t g_reload = 0;

static Binding g_bindings[MAX_BINDINGS];
static int g_n_bindings = 0;

/* Hotkeys already claimed by another daemon's own config -- see the file
 * doc comment. Populated by scan_external_hotkeys(), consulted by
 * load_config() before every XGrabKey so xiskeys never fights another
 * process for the same key. */
#define MAX_EXTERNAL_HOTKEYS 64
typedef struct {
    char owner[16]; /* "xispanel", "kiwm" */
    KeyCode keycode;
    unsigned int modifiers;
} ExternalHotkey;

static ExternalHotkey g_external[MAX_EXTERNAL_HOTKEYS];
static int g_n_external = 0;

static void handle_signal(int sig)
{
    if (sig == SIGHUP) {
        g_reload = 1;
    } else {
        g_quit = 1;
    }
}

/* Xlib's default error handler calls exit() on any X error -- fine for a
 * one-shot tool, fatal for a long-running daemon. The one error xiskeys is
 * actually likely to hit in normal operation is BadAccess from XGrabKey:
 * scan_external_hotkeys() only knows about xispanel/kiwm's own configs, so
 * a *third* party (some other app, a stale grab left behind by a crashed
 * process, a typo'd duplicate BIND line) can still race for the same key.
 * Log it and keep running instead of taking the whole daemon down over one
 * bad binding. */
static int x_error_handler(Display *dpy, XErrorEvent *ev)
{
    char msg[128];
    XGetErrorText(dpy, ev->error_code, msg, sizeof(msg));
    fprintf(stderr, "xiskeys: X error on request %d (%s) -- probably a hotkey already grabbed by something else; ignoring\n",
            ev->request_code, msg);
    return 0;
}

/* Same NumLock-slot lookup as xispanel/hotkey.c: NumLock isn't always
 * Mod2Mask, it's whatever modifier slot the server's current modifier map
 * puts it in. Computed once and cached. */
static unsigned int numlock_mask(void)
{
    static unsigned int mask = 0;
    static int computed = 0;
    if (computed) {
        return mask;
    }
    computed = 1;
    KeyCode numlock_kc = XKeysymToKeycode(g_dpy, XK_Num_Lock);
    if (!numlock_kc) {
        return mask;
    }
    XModifierKeymap *map = XGetModifierMapping(g_dpy);
    if (!map) {
        return mask;
    }
    for (int mod = 0; mod < 8; mod++) {
        for (int k = 0; k < map->max_keypermod; k++) {
            if (map->modifiermap[mod * map->max_keypermod + k] == numlock_kc) {
                mask = 1u << mod;
            }
        }
    }
    XFreeModifiermap(map);
    return mask;
}

static int parse_hotkey_spec(const char *spec, unsigned int *out_mods, KeyCode *out_keycode)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);

    unsigned int mods = 0;
    char keyname[64] = "";
    char *save = NULL;
    char *tok = strtok_r(buf, "+", &save);
    while (tok) {
        char *next = strtok_r(NULL, "+", &save);
        if (!next) {
            snprintf(keyname, sizeof(keyname), "%s", tok);
            break;
        }
        if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) {
            mods |= ControlMask;
        } else if (!strcasecmp(tok, "Alt")) {
            mods |= Mod1Mask;
        } else if (!strcasecmp(tok, "Shift")) {
            mods |= ShiftMask;
        } else if (!strcasecmp(tok, "Meta") || !strcasecmp(tok, "Super") || !strcasecmp(tok, "Win")) {
            mods |= Mod4Mask;
        } else {
            fprintf(stderr, "xiskeys: unknown modifier '%s' in '%s', ignoring it\n", tok, spec);
        }
        tok = next;
    }
    if (!keyname[0]) {
        fprintf(stderr, "xiskeys: spec '%s' has no key, only modifiers\n", spec);
        return 0;
    }
    KeySym ks = XStringToKeysym(keyname);
    if (ks == NoSymbol) {
        fprintf(stderr, "xiskeys: unknown key name '%s' in spec '%s'\n", keyname, spec);
        return 0;
    }
    KeyCode kc = XKeysymToKeycode(g_dpy, ks);
    if (!kc) {
        fprintf(stderr, "xiskeys: key '%s' (spec '%s') has no keycode on this keyboard\n", keyname, spec);
        return 0;
    }
    *out_mods = mods;
    *out_keycode = kc;
    return 1;
}

/* Same $XDG_CONFIG_HOME/<name> resolution as resolve_configpath() below,
 * but for another daemon's config file -- read-only, best-effort, this
 * file may not exist at all (kiwm isn't built yet). */
static void resolve_other_conf_path(const char *name, char *out, size_t outsz)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        snprintf(out, outsz, "%s/%s", xdg_config, name);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    snprintf(out, outsz, "%s/.config/%s", home, name);
}

/* Extracts the value of "hotkey=" out of a WIDGET line's trailing
 * key=value tail (xispanel.conf's kv_get() grammar: space-separated,
 * `key=value`, value may not itself contain spaces -- matches every
 * hotkey= producer in the xispanel tree today: xisserve.c, globalmenu.c,
 * folder.c). Returns 1 and fills `out` if found. */
static int extract_kv_value(const char *line, const char *key, char *out, size_t outsz)
{
    char needle[32];
    snprintf(needle, sizeof(needle), "%s=", key);
    const char *p = strstr(line, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\r' && p[i] != '\n' && i < outsz - 1) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return i > 0;
}

static void add_external_hotkey(const char *owner, const char *spec)
{
    if (g_n_external >= MAX_EXTERNAL_HOTKEYS) {
        return;
    }
    unsigned int mods;
    KeyCode kc;
    /* Bare-modifier specs (xispanel's modtap.c "tap Meta alone" style,
     * e.g. hotkey=Meta) don't parse here on purpose -- they're not a real
     * XGrabKey combination and can't collide with one. */
    if (!parse_hotkey_spec(spec, &mods, &kc)) {
        return;
    }
    ExternalHotkey *e = &g_external[g_n_external++];
    snprintf(e->owner, sizeof(e->owner), "%s", owner);
    e->keycode = kc;
    e->modifiers = mods;
}

/* xispanel.conf: any WIDGET line with a hotkey=<spec> tail (hotkey.c grabs
 * these locally, see the file doc comment). kiwm.conf: reads a proposed
 * `HOTKEY\t<action>\t<spec>` line format for whenever kiwm exists and
 * grabs its own window/desktop shortcuts the same way -- harmless no-op
 * today since the file won't exist yet. Both are silently skipped if
 * missing/unreadable; this is advisory conflict-avoidance, not a hard
 * dependency on either daemon being installed. */
static void scan_external_hotkeys(void)
{
    g_n_external = 0;
    char path[PATH_MAX];
    char line[LINE_MAX_LEN];
    char val[64];

    resolve_other_conf_path("xispanel.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            if (!strncmp(line, "WIDGET\t", 7) && extract_kv_value(line, "hotkey", val, sizeof(val))) {
                add_external_hotkey("xispanel", val);
            }
        }
        fclose(f);
    }

    resolve_other_conf_path("kiwm.conf", path, sizeof(path));
    f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *save = NULL;
            char tmp[LINE_MAX_LEN];
            snprintf(tmp, sizeof(tmp), "%s", line);
            char *tag = strtok_r(tmp, "\t\r\n", &save);
            char *action = tag ? strtok_r(NULL, "\t\r\n", &save) : NULL;
            char *spec = action ? strtok_r(NULL, "\t\r\n", &save) : NULL;
            if (tag && spec && !strcmp(tag, "HOTKEY")) {
                add_external_hotkey("kiwm", spec);
            }
        }
        fclose(f);
    }
}

static const char *external_owner_of(KeyCode kc, unsigned int mods)
{
    for (int i = 0; i < g_n_external; i++) {
        if (g_external[i].keycode == kc && g_external[i].modifiers == mods) {
            return g_external[i].owner;
        }
    }
    return NULL;
}

static void grab_variants(KeyCode kc, unsigned int mods)
{
    unsigned int nl = numlock_mask();
    unsigned int variants[4] = {0, LockMask, nl, nl | LockMask};
    for (int i = 0; i < 4; i++) {
        XGrabKey(g_dpy, kc, mods | variants[i], g_root, True, GrabModeAsync, GrabModeAsync);
    }
}

static void ungrab_variants(KeyCode kc, unsigned int mods)
{
    unsigned int nl = numlock_mask();
    unsigned int variants[4] = {0, LockMask, nl, nl | LockMask};
    for (int i = 0; i < 4; i++) {
        XUngrabKey(g_dpy, kc, mods | variants[i], g_root);
    }
}

static void ungrab_all(void)
{
    for (int i = 0; i < g_n_bindings; i++) {
        ungrab_variants(g_bindings[i].keycode, g_bindings[i].modifiers);
    }
}

static void write_default_config(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "xiskeys: could not write default config '%s': %s\n", path, strerror(errno));
        return;
    }
    fprintf(f, "# xiskeys config -- one binding per line:\n");
    fprintf(f, "#   BIND\\t<action-name>\\t<Mod>+<Mod>+<Key>\\t<shell command>\n");
    fprintf(f, "#\n");
    fprintf(f, "# Only *stateless* environment actions belong here (a shell command that\n");
    fprintf(f, "# doesn't need any other daemon's live state). Window/desktop management\n");
    fprintf(f, "# (Alt+Tab, maximize, desktop grid...) belongs to kiwm's own config once it\n");
    fprintf(f, "# exists; xispanel's launcher/menu popups already use their own hotkey=\n");
    fprintf(f, "# widget option (see xispanel.conf). xiskeys reads both of those at load time\n");
    fprintf(f, "# and skips (with a log line) any binding here that collides with one there --\n");
    fprintf(f, "# see this file's top comment for why.\n");
    fprintf(f, "\n# --- system monitor / task manager -------------------------------------\n");
    fprintf(f, "BIND\ttask-manager\tCtrl+Escape\tqps\n");
    fprintf(f, "\n# --- screenshot (spectacle for now; a X-Density-aware xisshot is a\n");
    fprintf(f, "# planned future replacement, see XISDESKTOP_PLAN.md) -------------------\n");
    fprintf(f, "BIND\tscreenshot-full\tPrint\tspectacle\n");
    fprintf(f, "BIND\tscreenshot-region\tShift+Print\tspectacle -r\n");
    fprintf(f, "BIND\tscreenshot-window\tMeta+Shift+Print\tspectacle -a\n");
    fprintf(f, "\n# --- emoji picker / on-screen keyboard -----------------------------------\n");
    fprintf(f, "# No lightweight tool for either is installed/chosen yet -- left unbound on\n");
    fprintf(f, "# purpose (a dead hotkey is worse than no hotkey). Uncomment and point at a\n");
    fprintf(f, "# real command once one exists:\n");
    fprintf(f, "#BIND\temoji-picker\tMeta+period\trofimoji\n");
    fprintf(f, "#BIND\tvirtual-keyboard\tMeta+K\tonboard\n");
    fprintf(f, "\n# --- media keys (needs playerctl, MPRIS) ---------------------------------\n");
    fprintf(f, "BIND\tmedia-play-pause\tXF86AudioPlay\tplayerctl play-pause\n");
    fprintf(f, "BIND\tmedia-next\tXF86AudioNext\tplayerctl next\n");
    fprintf(f, "BIND\tmedia-prev\tXF86AudioPrev\tplayerctl previous\n");
    fprintf(f, "BIND\tmedia-stop\tXF86AudioStop\tplayerctl stop\n");
    fprintf(f, "\n# --- volume (xispanel's volume widget polls pactl state on its own tick,\n");
    fprintf(f, "# so it picks these changes up with no coordination needed) --------------\n");
    fprintf(f, "BIND\tvolume-up\tXF86AudioRaiseVolume\tpactl set-sink-volume @DEFAULT_SINK@ +5%%\n");
    fprintf(f, "BIND\tvolume-down\tXF86AudioLowerVolume\tpactl set-sink-volume @DEFAULT_SINK@ -5%%\n");
    fprintf(f, "BIND\tvolume-mute\tXF86AudioMute\tpactl set-sink-mute @DEFAULT_SINK@ toggle\n");
    fprintf(f, "BIND\tmic-mute\tXF86AudioMicMute\tpactl set-source-mute @DEFAULT_SOURCE@ toggle\n");
    fprintf(f, "#BIND\tmic-up\tMeta+Shift+Up\tpactl set-source-volume @DEFAULT_SOURCE@ +5%%\n");
    fprintf(f, "#BIND\tmic-down\tMeta+Shift+Down\tpactl set-source-volume @DEFAULT_SOURCE@ -5%%\n");
    fprintf(f, "\n# --- power / brightness --------------------------------------------------\n");
    fprintf(f, "BIND\tbrightness-up\tXF86MonBrightnessUp\tbrightnessctl set 5%%+\n");
    fprintf(f, "BIND\tbrightness-down\tXF86MonBrightnessDown\tbrightnessctl set 5%%-\n");
    fprintf(f, "BIND\tsuspend\tXF86Sleep\tsystemctl suspend\n");
    fprintf(f, "BIND\tpoweroff\tCtrl+Alt+End\tsystemctl poweroff\n");
    fprintf(f, "BIND\treboot\tCtrl+Alt+Delete\tsystemctl reboot\n");
    fprintf(f, "\n# --- session (i3lock per XISDESKTOP_PLAN.md's screen-locker choice) ------\n");
    fprintf(f, "BIND\tlock-session\tMeta+L\ti3lock\n");
    fprintf(f, "BIND\tlogout\tMeta+Shift+L\tloginctl terminate-session \"$XDG_SESSION_ID\"\n");
    fprintf(f, "# Fast user switching depends on whichever display manager/greeter\n");
    fprintf(f, "# KiDesktop ends up using (e.g. `dm-tool switch-to-greeter` for LightDM) --\n");
    fprintf(f, "# not chosen yet, left unbound.\n");
    fprintf(f, "#BIND\tswitch-user\tMeta+Shift+U\tdm-tool switch-to-greeter\n");
    fprintf(f, "\n# --- displays --------------------------------------------------------------\n");
    fprintf(f, "BIND\tdisplays\tMeta+P\tkiconf\n");
    fprintf(f, "\n# --- your own launchers/commands, add as many as you want -----------------\n");
    fprintf(f, "#BIND\tterminal\tMeta+Return\txterm\n");
    fprintf(f, "#BIND\tbrowser\tMeta+B\tfirefox\n");
    fclose(f);
}

static void load_config(void)
{
    if (access(g_configpath, F_OK) != 0) {
        fprintf(stderr, "xiskeys: no config at '%s', writing default\n", g_configpath);
        write_default_config(g_configpath);
    }

    FILE *f = fopen(g_configpath, "r");
    if (!f) {
        fprintf(stderr, "xiskeys: could not read '%s': %s\n", g_configpath, strerror(errno));
        return;
    }

    scan_external_hotkeys();

    char line[LINE_MAX_LEN];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        if (g_n_bindings >= MAX_BINDINGS) {
            fprintf(stderr, "xiskeys: too many bindings, ignoring rest of config\n");
            break;
        }

        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *action = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *spec = action ? strtok_r(NULL, "\t", &save) : NULL;
        char *cmd = spec ? strtok_r(NULL, "", &save) : NULL; /* rest of line, may itself contain no tabs */

        if (!tag || strcmp(tag, "BIND") != 0 || !action || !spec || !cmd || !cmd[0]) {
            fprintf(stderr, "xiskeys: skipping malformed line: '%s'\n", line);
            continue;
        }

        unsigned int mods;
        KeyCode kc;
        if (!parse_hotkey_spec(spec, &mods, &kc)) {
            continue;
        }

        const char *owner = external_owner_of(kc, mods);
        if (owner) {
            fprintf(stderr,
                    "xiskeys: '%s' (%s) is already grabbed by %s's own config -- skipping here to avoid a silent "
                    "XGrabKey conflict; edit it over there instead\n",
                    action, spec, owner);
            continue;
        }

        Binding *b = &g_bindings[g_n_bindings++];
        snprintf(b->action, sizeof(b->action), "%s", action);
        snprintf(b->spec, sizeof(b->spec), "%s", spec);
        snprintf(b->command, sizeof(b->command), "%s", cmd);
        b->keycode = kc;
        b->modifiers = mods;

        grab_variants(kc, mods);
        fprintf(stderr, "xiskeys: bound '%s' (%s) -> %s\n", action, spec, cmd);
    }
    fclose(f);
}

static void reload_config(void)
{
    fprintf(stderr, "xiskeys: reloading config\n");
    ungrab_all();
    g_n_bindings = 0;
    load_config();
}

static void run_action(const Binding *b)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("xiskeys: fork");
        return;
    }
    if (pid == 0) {
        setenv("XISKEYS_ACTION", b->action, 1);
        setsid();
        execl("/bin/sh", "sh", "-c", b->command, (char *)NULL);
        _exit(127);
    }
}

static void handle_keypress(const XKeyEvent *ev)
{
    unsigned int state = ev->state & ~(LockMask | numlock_mask());
    for (int i = 0; i < g_n_bindings; i++) {
        if (g_bindings[i].keycode == ev->keycode && g_bindings[i].modifiers == state) {
            run_action(&g_bindings[i]);
            return;
        }
    }
}

static void resolve_configpath(void)
{
    const char *xdg_config = getenv("XDG_CONFIG_HOME");
    if (xdg_config && *xdg_config) {
        mkdir(xdg_config, 0700);
        snprintf(g_configpath, sizeof(g_configpath), "%s/xiskeys.conf", xdg_config);
        return;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        home = "/tmp";
    }
    char configdir[PATH_MAX];
    snprintf(configdir, sizeof(configdir), "%s/.config", home);
    mkdir(configdir, 0700);
    snprintf(g_configpath, sizeof(g_configpath), "%s/xiskeys.conf", configdir);
}

int main(int argc, char **argv)
{
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("xiskeys %s - global hotkey daemon for KiDesktop\n", XISKEYS_VERSION);
        printf("Usage: xiskeys\n");
        printf("Config: $XDG_CONFIG_HOME/xiskeys.conf (fallback ~/.config/xiskeys.conf)\n");
        printf("SIGHUP reloads the config.\n");
        return 0;
    }

    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    char lockpath[PATH_MAX];
    snprintf(lockpath, sizeof(lockpath), "%s/xiskeys.lock", rundir);
    int lockfd = open(lockpath, O_CREAT | O_RDWR, 0600);
    if (lockfd >= 0 && flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "xiskeys: already running (lock held on '%s')\n", lockpath);
        return 1;
    }

    resolve_configpath();

    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        fprintf(stderr, "xiskeys: could not open X display\n");
        return 1;
    }
    g_root = DefaultRootWindow(g_dpy);
    XSetErrorHandler(x_error_handler);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGHUP, handle_signal);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    load_config();
    XSync(g_dpy, False);

    int xfd = ConnectionNumber(g_dpy);
    while (!g_quit) {
        if (g_reload) {
            g_reload = 0;
            reload_config();
            XSync(g_dpy, False);
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(xfd, &rfds);
        int r = select(xfd + 1, &rfds, NULL, NULL, NULL);
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("xiskeys: select");
            break;
        }

        while (XPending(g_dpy)) {
            XEvent ev;
            XNextEvent(g_dpy, &ev);
            if (ev.type == KeyPress) {
                handle_keypress(&ev.xkey);
            }
        }
    }

    ungrab_all();
    XCloseDisplay(g_dpy);
    fprintf(stderr, "xiskeys: shutting down\n");
    return 0;
}

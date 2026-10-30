#define _GNU_SOURCE   /* struct ucred for SO_PEERCRED */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdarg.h>
#include <ctype.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <dirent.h>
#include <limits.h>
#include <sys/wait.h>
#include <stdint.h>
#include <X11/Xlib.h>

#include "../shared/xis_i18n.h"
#include "../shared/xis_safefile.h"

#define MAX_ALERTS          100
#define MAX_IGNORED         200
#define BUF_SIZE            4096
#define CTL_BUF_SIZE        65536
#define REPORT_THROTTLE_S   1
#define MAX_SUBSCRIBERS     8

#define XISGUARD_VERSION    "0.4.24"

#define XNOTIFY_ATTACH           1
#define XNOTIFY_SELECTION        2
#define XNOTIFY_COMPOSITE        3
#define XNOTIFY_SCREEN           4
#define XNOTIFY_RECORD           5
#define XNOTIFY_CURSOR           6
#define XNOTIFY_INPUT_GRAB       7
#define XNOTIFY_INPUT_INJECT     8
#define XNOTIFY_HOTKEY           9
#define XNOTIFY_INPUT           10
#define XNOTIFY_MANAGE          11
#define XNOTIFY_GRAB_OVERRIDE   12
#define XNOTIFY_WARP            13
#define XNOTIFY_FOCUS           14
#define XNOTIFY_RANDR           15
#define XNOTIFY_OVERLAY         16

#define COMMAND_HEARTBEAT     1
#define COMMAND_ALLOW_ALL     2
#define COMMAND_ALLOW_ACTION  3
#define COMMAND_DENY          4
#define COMMAND_DENY_ALL      5
#define COMMAND_DENY_ACTION   6
#define COMMAND_ALLOW         0

/* Button labels shown on the Allow/Deny/Trust dialog (zenity or
 * xisserve --question). Plain #define string literals used to get
 * concatenated into the dialog command at compile time (`"--ok-label='"
 * BTN_ALLOW "' "`) -- cheap, but a macro expansion is invisible to
 * xgettext, so it could never be translated. N_() here only marks each
 * literal for extraction without looking it up (same convention as
 * kiconf's FixedShortcut.doc); show_zenity_dialog()/
 * show_xisserve_dialog() call _(BTN_ALLOW) at the point each label is
 * actually used, building the command with %s instead of compile-time
 * concatenation so the lookup happens at runtime. The same _(BTN_X) call
 * is used again when matching zenity's answer below, so the match is
 * always against whatever this session's catalog actually displayed --
 * never against the English text. See po/README.md. */
static const char *const BTN_ALLOW               = N_("Allow");
static const char *const BTN_DENY                = N_("Deny");
static const char *const BTN_TRUST               = N_("TRUST");
static const char *const BTN_DENY_SESSION        = N_("Deny (session)");
static const char *const BTN_ALLOW_SESSION       = N_("Allow (session)");
static const char *const BTN_TRUST_SESSION       = N_("Trust (session)");
static const char *const BTN_ALLOW_EXACT         = N_("Allow EXACT");
static const char *const BTN_TRUST_EXACT         = N_("Trust EXACT");
static const char *const BTN_ALLOW_EXACT_SESSION = N_("Allow EXACT (session)");
static const char *const BTN_TRUST_EXACT_SESSION = N_("Trust EXACT (session)");

/* Dialog backend used to ask the user Allow/Deny/Trust questions.
 * Detected once at startup: xisserve is the default, zenity the fallback
 * for systems without it. */
#define DIALOG_NONE     0
#define DIALOG_XISSERVE 1
#define DIALOG_ZENITY   2

static const struct {
    int   id;
    const char *name;
} action_names[] = {
    { XNOTIFY_ATTACH,        "ATTACH" },
    { XNOTIFY_SELECTION,     "SELECTION" },
    { XNOTIFY_COMPOSITE,     "COMPOSITE" },
    { XNOTIFY_SCREEN,        "SCREEN" },
    { XNOTIFY_RECORD,        "RECORD" },
    { XNOTIFY_CURSOR,        "CURSOR" },
    { XNOTIFY_INPUT_GRAB,    "INPUT_GRAB" },
    { XNOTIFY_INPUT_INJECT,  "INPUT_INJECT" },
    { XNOTIFY_HOTKEY,        "HOTKEY" },
    { XNOTIFY_INPUT,         "INPUT" },
    { XNOTIFY_MANAGE,        "MANAGE" },
    { XNOTIFY_GRAB_OVERRIDE, "GRAB_OVERRIDE" },
    { XNOTIFY_WARP,          "WARP" },
    { XNOTIFY_FOCUS,         "FOCUS" },
    { XNOTIFY_RANDR,         "RANDR" },
    { XNOTIFY_OVERLAY,       "OVERLAY" },
    { 0, NULL }
};

/* Display-only -- only ever read into the dialog text via
 * action_to_description(), which calls _() on its way out, so N_() here
 * just marks each literal for extraction (same convention as kiconf's
 * XNOTIFY_ACTIONS table). action_names above is the protocol token
 * instead, never translated. */
static const struct {
    int   id;
    const char *name;
} action_descriptions[] = {
    { XNOTIFY_ATTACH,        N_("Use shared memory") },
    { XNOTIFY_SELECTION,     N_("Access clipboard") },
    { XNOTIFY_COMPOSITE,     N_("Access other windows") },
    { XNOTIFY_SCREEN,        N_("Capture and draw to the screen") },
    { XNOTIFY_RECORD,        N_("Record events - like keystrokes") },
    { XNOTIFY_CURSOR,        N_("Access cursor (mouse) image and position") },
    { XNOTIFY_INPUT_GRAB,    N_("Grab mouse or keyboard") },
    { XNOTIFY_INPUT_INJECT,  N_("Insert keystrokes") },
    { XNOTIFY_HOTKEY,        N_("Register global hotkeys") },
    { XNOTIFY_INPUT,         N_("Capture input - even when unfocused") },
    { XNOTIFY_MANAGE,        N_("List and get properties of other windows") },
    { XNOTIFY_GRAB_OVERRIDE, N_("Allow to steal a grab (for screensavers)") },
    { XNOTIFY_WARP,          N_("Move the mouse cursor") },
    { XNOTIFY_FOCUS,         N_("Steal input focus") },
    { XNOTIFY_RANDR,         N_("Change display configuration") },
    { XNOTIFY_OVERLAY,       N_("Create overlay (transparent) window") },
    { 0, NULL }
};

static const char* action_to_string(int action_id) {
    if (action_id == -1 || action_id == 0)
        return "ALL";

    for (int i = 0; action_names[i].id > 0; i++) {
        if (action_names[i].id == action_id)
            return action_names[i].name;
    }
    return "UNKNOWN";
}

static const char* action_to_description(int action_id) {
    if (action_id == -1)
        return _("All the X server permissions");

    for (int i = 0; action_descriptions[i].id > 0; i++) {
        if (action_descriptions[i].id == action_id)
            return _(action_descriptions[i].name);
    }
    return _("UNKNOWN");
}

static int string_to_action(const char *str) {
    if (!str) return 0;
    if (strcasecmp(str, "ALL") == 0)
            return -1;

    for (int i = 0; action_names[i].name; i++) {
        if (strcasecmp(str, action_names[i].name) == 0)
            return action_names[i].id;
    }
    return 0;
}

static char SOCKET_PATH_BUF[108] = {0};
static char LOCK_SOCKET_PATH_BUF[108] = {0};
static char CTL_SOCKET_PATH_BUF[108] = {0};
static int lock_fd = -1;
static int server_fd = -1;
static int ctl_fd = -1;
static pthread_t file_monitor_thread;
static pthread_t processor_thread;
static pthread_t control_thread;
static volatile int should_exit = 0;
/* Set by SIGHUP, or by file_monitor_loop() when our binary was replaced on
 * disk: the main loop then exec()s the binary at that path again. */
static volatile sig_atomic_t reexec_requested = 0;
static char **saved_argv = NULL;
/* Inodes of the socket paths we bound, so shutdown only unlinks paths that
 * still belong to us (another instance may have bound them since). */
static ino_t sock_path_ino = 0, ctl_path_ino = 0, lock_path_ino = 0;

static int no_pause_mode = 0;      /* 1 = notify only, no SIGSTOP */
static int quiet_mode = 0;         /* 1 = no dialog, terminal logs only */
static int always_kill_mode = 0;   /* 1 = kill unknown processes immediately */
static int log_level = 2;          /* 0=silent, 1=clean, 2=normal, 3=verbose, 4=debug */
static int dialog_backend = DIALOG_NONE; /* resolved once at startup, see detect_dialog_backend() */

/* 1 = perms.conf is disabled: the only valid rules are the ones the X server
 * already loaded from its own SYSCONFDIR/xnotify.conf* files, plus whatever
 * is granted for the current session. A permanent Allow/Trust in this mode
 * is written to the server's own xnotify.conf.d instead, see save_rule(). */
static int secure_mode = 0;

/* 1 = permissive mode, runtime only (never persisted): the server's global
 * action mask allows every action, so nothing is asked or denied, but the
 * server still sends REPORTs. Any server RELOAD or DENY_ACTION wipes that
 * mask, so it is re-applied after every full sync, see
 * permissive_reapply(). */
static int permissive_mode = 0;

/* 1 = start in permissive mode. The default only for a new install (no
 * xnotify.conf yet); an existing xnotify.conf without the key keeps 0.
 * Changed with SET_PERMISSIVE "persist":1, saved as permissive_on_start. */
static int permissive_on_start = 0;

/* Control-socket clients allowed to SUBSCRIBE to the event stream, by
 * executable basename (comma-separated, persisted in xnotify.conf as
 * `subscribers=`). The socket is already owner-only; this also keeps any
 * other same-user process from quietly reading activity metadata. */
static char subscribers_allow[256] = "";
/* KiDesktop's own components are always allowed. They are not in the saved
 * list on purpose: xnotify.conf persists `subscribers=` on first run, so a
 * component added later would be refused by every existing install. */
/* Which actions are worth telling the user about (panel toast, the xisserve
 * page): a bit per action id like the subscriber masks. Others still reach
 * subscribers, flagged "muted". Default: screen / window capture. */
#define NOTIFY_DEFAULT_MASK ((1u << (XNOTIFY_SCREEN - 1)) | (1u << (XNOTIFY_COMPOSITE - 1)))
static uint32_t notify_mask = NOTIFY_DEFAULT_MASK;
/* KiDesktop's own programs, never reported (matched on the executable's
 * basename; not persisted, so an update can extend it). xisguard itself is
 * not here: it is the guard and has its own special permission. */
static const char *const ignored_builtin[] = {
    "kiwm", "kicomp", "kiconf", "kiconfd", "xispanel", "xisserve", "xisback", "xiskeys", "xismenu",
    "xis-sound", "kilock", "kisession", "kistoryd", "kimemoryd", NULL
};
static const char *const subscribers_builtin[] = { "kistoryd", "kimemoryd", "xispanel", NULL };

/* Server's real drop-in rules directory (SYSCONFDIR "/xnotify.conf.d"),
 * learned on demand from the X server itself since SYSCONFDIR is a
 * compile-time constant of the server, not of xisguard. Filled in by
 * handle_message() on a "CONFIG_PATH" reply, see request_config_path(). */
static char secure_conf_dir[512] = {0};
static char secure_conf_file[512] = {0}; /* the server's SYSCONFDIR/xnotify.conf, read next to the .d directory */
static pthread_mutex_t secure_conf_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  secure_conf_cond = PTHREAD_COND_INITIALIZER;

/* Bitmasks for per-action CLI overrides (bit N-1 = action N, 1-16).
 * Checked before perms.conf; deny wins if both bits are set for same action. */
static uint32_t cli_allow_mask = 0;
static uint32_t cli_deny_mask  = 0;

static int display = 0;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t ignored_lock = PTHREAD_MUTEX_INITIALIZER;

struct Alert {
    int action;
    pid_t pid;
    char  exe[PATH_MAX];
    char time_str[20];
    int paused;
};

struct Alert alert_queue[MAX_ALERTS];
int alert_count = 0;

struct IgnoredEntry {
    char exe_pattern[PATH_MAX];
    char action[64];
    int  rule_type;   /* 0 = ALLOW, 1 = DENY */
};

struct IgnoredEntry ignored_cmds[MAX_IGNORED];
int ignored_count = 0;

static char config_dir[256] = "";
static char perms_file[512] = {0};
static time_t last_config_mtime = 0;

static char ignore_file[512] = {0};
static time_t last_ignore_mtime = 0;

static char xnotify_conf_file[512] = {0};
static time_t last_xnotify_conf_mtime = 0;

struct IgnoredReport {
    char exe[PATH_MAX];
};

static struct IgnoredReport ignored_reports[MAX_IGNORED];
static int ignored_reports_count = 0;
static pthread_mutex_t report_ignore_lock = PTHREAD_MUTEX_INITIALIZER;

struct SeenAlert {
    char exe[PATH_MAX];
    pid_t pid;
    int action;
};

struct SeenAlert seen_alerts[MAX_ALERTS];
int seen_count = 0;

static struct {
    char  exe[PATH_MAX];
    int   action;
    time_t  last_time;
} last_report = {0};

/* ====================== LOGGING ====================== */

char* get_current_time() {
    static char buf[20];
    time_t now = time(NULL);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(buf, sizeof(buf), "%H:%M:%S", &tm_local);
    return buf;
}

void log_msg(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s] ", get_current_time());
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

static void log_filtered(int min_level, const char *format, ...) {
    if (log_level < min_level) return;
    va_list args;
    va_start(args, format);
    fprintf(stderr, "[%s] ", get_current_time());
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
    fflush(stderr);
}

/* ====================== UTILITIES ====================== */

char* trim(char *str) {
    char *end;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t')) end--;
    *(end + 1) = 0;
    return str;
}

time_t get_config_max_mtime(void) {
    time_t max_t = 0;
    struct stat st;
    if (stat(perms_file, &st) == 0 && st.st_mtime > max_t) max_t = st.st_mtime;
    return max_t;
}

static char *
trim_exe_for_log(const char *full_path)
{
    static char buffer[128];

    if (!full_path || *full_path == '\0') {
        strcpy(buffer, "?");
        return buffer;
    }

    if (strlen(full_path) < sizeof(buffer)) {
        strcpy(buffer, full_path);
        return buffer;
    }

    const char *last_slash = strrchr(full_path, '/');
    if (!last_slash) {
        strncpy(buffer, full_path, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        return buffer;
    }

    const char *exe_name = last_slash + 1;

    /* Encontra o componente pai (diretório imediatamente antes do exe) */
    const char *parent_end = last_slash;
    while (parent_end > full_path && *(parent_end - 1) != '/')
        parent_end--;
    size_t parent_len = (size_t)(last_slash - parent_end);

    /* Monta o sufixo ".../<parent>/<exe>" em buffer temporário */
    char suffix[128];
    int sfx_len = snprintf(suffix, sizeof(suffix), ".../%.*s/%s",
                           (int)parent_len, parent_end, exe_name);
    if (sfx_len <= 0) {
        strncpy(buffer, full_path, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        return buffer;
    }

    if ((size_t)sfx_len >= sizeof(buffer)) {
        /* Sufixo sozinho já estoura: trunca e retorna */
        strncpy(buffer, suffix, sizeof(buffer) - 1);
        buffer[sizeof(buffer) - 1] = '\0';
        return buffer;
    }

    /* Quantos bytes do prefixo cabem antes do sufixo */
    size_t avail   = sizeof(buffer) - 1 - (size_t)sfx_len;
    size_t pfx_len = (size_t)(parent_end - full_path);
    if (pfx_len > avail) pfx_len = avail;

    strncpy(buffer, full_path, pfx_len);
    strcpy(buffer + pfx_len, suffix);   /* sufixo já cabe — tamanho garantido acima */
    return buffer;
}

static int
pattern_matches(const char *s, const char *p) {
    if (!s || !p) return 0;

    const char *star = NULL;
    const char *ss = NULL;

    while (*s) {
        if (*p == '*') {
            star = p++;
            ss = s;
        } else if (*p == *s) {
            p++;
            s++;
        } else if (star) {
            p = star + 1;
            s = ++ss;
        } else {
            return 0;
        }
    }

    while (*p == '*') {
        p++;
    }

    return *p == '\0';
}

/* ====================== CONFIG (USER ONLY) ====================== */

void load_user_config(void) {
    if (secure_mode) {
        pthread_mutex_lock(&ignored_lock);
        ignored_count = 0;
        pthread_mutex_unlock(&ignored_lock);
        last_config_mtime = time(NULL);
        log_filtered(2, "Secure mode: %s is disabled, only the server's own rules and session grants apply", perms_file);
        return;
    }

    pthread_mutex_lock(&ignored_lock);
    ignored_count = 0;

    FILE *file = xis_fopen_read(perms_file, xis_check_usable);
    if (!file) {
        log_filtered(2, "No user config found at %s (normal on first run)", perms_file);
        pthread_mutex_unlock(&ignored_lock);
        return;
    }

    char line[BUF_SIZE];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        char *trimmed = trim(line);
        if (strlen(trimmed) == 0 || trimmed[0] == '#') continue;

        if (ignored_count >= MAX_IGNORED) {
            log_msg("Warning: maximum number of rules (%d) reached", MAX_IGNORED);
            continue;
        }

        char *cmd = strtok(trimmed, " \t\r\n");
        if (!cmd) continue;

        if (strcasecmp(cmd, "ALLOW") == 0 || strcasecmp(cmd, "DENY") == 0) {
            int is_allow = (strcasecmp(cmd, "ALLOW") == 0);
            char *token1 = strtok(NULL, " \t\r\n");   /* action or ALL */
            /* pattern = rest of the line: EXACT rules are "exe|args" and the
             * args hold spaces */
            char *token2 = strtok(NULL, "\r\n");
            if (token2) {
                token2 = trim(token2);
                if (*token2 == '\0') token2 = NULL;
            }

            if (!token1) continue;

            int action = string_to_action(token1);

            char pattern[PATH_MAX] = {0};
            char action_name[64] = {0};

            if (action == -1) {                      /* ALL */
                if (token2) {
                    strncpy(pattern, token2, sizeof(pattern)-1);
                    strcpy(ignored_cmds[ignored_count].exe_pattern, pattern);
                    ignored_cmds[ignored_count].action[0] = '\0';
                    ignored_cmds[ignored_count].rule_type = is_allow ? 0 : 1;
                    ignored_count++;
                }
            } else if (action > 0) {
                if (token2) {
                    strncpy(pattern, token2, sizeof(pattern)-1);
                    strncpy(action_name, token1, sizeof(action_name)-1);
                } else {
                    strcpy(pattern, "*");
                    strncpy(action_name, token1, sizeof(action_name)-1);
                }

                snprintf(ignored_cmds[ignored_count].exe_pattern, sizeof(ignored_cmds[ignored_count].exe_pattern), "%s", pattern);
                snprintf(ignored_cmds[ignored_count].action, sizeof(ignored_cmds[ignored_count].action), "%s", action_name);
                ignored_cmds[ignored_count].rule_type = is_allow ? 0 : 1;
                ignored_count++;
            }
        }
    }
    fclose(file);
    last_config_mtime = time(NULL);
    log_filtered(3, "%d rules loaded", ignored_count);
    pthread_mutex_unlock(&ignored_lock);
}

static void request_config_path(void);

/* Whether a rule (pattern + action name, "" = every action) covers exe for
 * action_name. A pattern without '|' matches any args; one with '|' needs an
 * exact exe|args match. */
static int rule_covers(const char *exe, const char *pattern, const char *rule_action, const char *action_name) {
    char exe_buf[PATH_MAX];
    const char *match_key = exe;
    if (strchr(pattern, '|') == NULL) {
        snprintf(exe_buf, sizeof(exe_buf), "%s", exe);
        char *p = strchr(exe_buf, '|');
        if (p) *p = '\0';
        match_key = exe_buf;
    }
    if (!pattern_matches(match_key, pattern))
        return 0;
    return rule_action[0] == '\0' || strcasecmp(rule_action, action_name) == 0;
}

/* Secure mode: the guard keeps no rules in memory, but the system rule files
 * (the server's xnotify.conf and xnotify.conf.d/ *.conf files) are readable. Returns
 * 1 if one of them has an explicit DENY covering exe/action, so a repeat
 * attempt is answered without a dialog. Needs the directory learned from
 * GET_CONFIG_PATH; until then returns 0 (and asks for it). */
static int system_rule_denies(const char *exe, int action_id) {
    const char *action_name = action_to_string(action_id);
    char dir[512], file[512];
    pthread_mutex_lock(&secure_conf_lock);
    snprintf(dir, sizeof(dir), "%s", secure_conf_dir);
    snprintf(file, sizeof(file), "%s", secure_conf_file);
    pthread_mutex_unlock(&secure_conf_lock);
    if (dir[0] == '\0') {
        request_config_path();
        return 0;
    }

    char *paths[64];
    int np = 0;
    if (file[0] != '\0')
        paths[np++] = strdup(file);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && np < 64) {
            size_t n = strlen(de->d_name);
            if (n < 6 || strcmp(de->d_name + n - 5, ".conf") != 0)
                continue;
            char full[1100];
            snprintf(full, sizeof(full), "%s/%s", dir, de->d_name);
            paths[np++] = strdup(full);
        }
        closedir(d);
    }

    int denied = 0;
    for (int i = 0; i < np; i++) {
        FILE *f = paths[i] ? fopen(paths[i], "r") : NULL;
        free(paths[i]);
        if (!f || denied) {
            if (f) fclose(f);
            continue;
        }
        char line[BUF_SIZE];
        while (!denied && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            char *t = trim(line);
            if (strncasecmp(t, "DENY", 4) != 0 || !isspace((unsigned char)t[4]))
                continue;
            t = trim(t + 4);
            char *sp = t;
            while (*sp && !isspace((unsigned char)*sp)) sp++;
            char act[64] = { 0 };
            snprintf(act, sizeof(act), "%.*s", (int)(sp - t) < 63 ? (int)(sp - t) : 63, t);
            char *pat = trim(sp);
            if (strcasecmp(act, "ALL") == 0) {
                if (*pat == '\0') continue;
                act[0] = '\0';
            } else if (string_to_action(act) <= 0) {
                continue;
            }
            if (*pat == '\0') pat = "*";
            if (rule_covers(exe, pat, act, action_name))
                denied = 1;
        }
        fclose(f);
    }
    return denied;
}

int get_preconfig_rule(const char *exe, int action_id, char *matched_pattern, size_t max_len) {
    if (!exe || *exe == '\0' || strcmp(exe, "?") == 0)
        return 0;

    if (matched_pattern && max_len > 0)
        matched_pattern[0] = '\0';

    const char *action_name = action_to_string(action_id);
    if (!action_name || strcmp(action_name, "UNKNOWN") == 0)
        return 0;

    pthread_mutex_lock(&ignored_lock);
    for (int i = 0; i < ignored_count; i++) {
        if (rule_covers(exe, ignored_cmds[i].exe_pattern, ignored_cmds[i].action, action_name)) {
            int ret = (ignored_cmds[i].rule_type == 0) ? 1 : 2;

            if (matched_pattern && max_len > 0) {
                strncpy(matched_pattern, ignored_cmds[i].exe_pattern, max_len - 1);
                matched_pattern[max_len - 1] = '\0';
            }

            pthread_mutex_unlock(&ignored_lock);
            return ret;   /* 1=allow, 2=deny */
        }
    }
    pthread_mutex_unlock(&ignored_lock);
    return 0;
}

/* ====================== SYNC PERMISSIONS TO X SERVER ====================== */

void send_permission(int action, const char *exe, pid_t pid, int command_type);
void permissive_reapply(void);
static void secure_save_rule(const char *exe, int action_id, int is_allow);

static int cli_action_denied(int action_id) {
    return action_id > 0 && action_id <= 16 && (cli_deny_mask & (1u << (action_id - 1)));
}

void send_all_permissions_to_xserver(void) {
    pthread_mutex_lock(&ignored_lock);

    int sent = 0;
    for (int i = 0; i < ignored_count; i++) {
        const char *pat = ignored_cmds[i].exe_pattern;
        const char *act_str = ignored_cmds[i].action;
        int action_id = string_to_action(act_str);
        
        if (ignored_cmds[i].rule_type == 0) {   /* ALLOW */
            /* The server grants a synced ALLOW without asking us, so it would
             * bypass --deny: leave the actions in cli_deny_mask out. */
            if (act_str[0] == '\0') {
                /* ALLOW ALL <pattern> */
                if (cli_deny_mask == 0) {
                    send_permission(0, pat, 0, COMMAND_ALLOW_ALL);       /* command_type 2 = ALLOW_ALL */
                } else {
                    for (int a = 0; action_names[a].id > 0; a++)
                        if (!cli_action_denied(action_names[a].id))
                            send_permission(action_names[a].id, pat, 0, COMMAND_ALLOW);
                }
            }
            else if (cli_action_denied(action_id)) {
                /* skipped, see above */
            }
            else if (strcmp(pat, "*") == 0) {
                /* ALLOW <ACTION>  (sem exe → allow_all para essa ação) */
                send_permission(action_id, "", 0, COMMAND_ALLOW_ACTION);
            } else {
                send_permission(action_id, pat, 0, COMMAND_ALLOW);   /* command_type 3 = ALLOW_ACTION */
            }
        } else {   /* DENY */
            if (act_str[0] == '\0') {
                send_permission(0, pat, 0, COMMAND_DENY_ALL);           /* DENY_ALL */
            } else if (strcmp(pat, "*") == 0) {
                send_permission(action_id, "", 0, COMMAND_DENY_ACTION);    /* DENY_ACTION */
            } else {
                send_permission(action_id, pat, 0, COMMAND_DENY);   /* DENY normal */
            }
        }
        sent++;
    }

    pthread_mutex_unlock(&ignored_lock);

    if (sent > 0)
        log_filtered(2, "Full sync: sent %d permission rules to X server", sent);
    else
        log_filtered(3, "Full sync: no rules to send (perms.conf empty)");

    permissive_reapply();
}

/* Sends ALLOW_ACTION for every known action while permissive mode is on, and
 * for every action in --allow (minus --deny) so the server grants those
 * without asking. The server checks its global mask before any rule, so this
 * beats static and user DENY rules alike. */
void permissive_reapply(void) {
    for (int i = 0; action_names[i].id > 0; i++) {
        int id = action_names[i].id;
        int cli_allowed = id <= 16 && (cli_allow_mask & (1u << (id - 1))) && !cli_action_denied(id);
        if (permissive_mode || cli_allowed)
            send_permission(id, "", 0, COMMAND_ALLOW_ACTION);
    }
}

/* ====================== SAVE RULES ====================== */

/* Appends one line to perms_file the crash-safe way: the whole file is
 * rewritten (tmp + fsync + rename) after the previous one is kept as
 * perms.conf.backup. Caller holds ignored_lock. Returns 1 on success. */
static int perms_append_line(const char *fmt, const char *a, const char *b, const char *c) {
    size_t len = 0;
    char *old = xis_read_file_or_backup(perms_file, xis_check_usable, &len);
    size_t cap = len + strlen(a) + strlen(b) + strlen(c) + 16;
    char *out = malloc(cap);
    if (!out) {
        free(old);
        return 0;
    }
    size_t n = 0;
    if (old && len) {
        memcpy(out, old, len);
        n = len;
        if (out[n - 1] != '\n')
            out[n++] = '\n';
    }
    n += (size_t)snprintf(out + n, cap - n, fmt, a, b, c);
    int ok = xis_write_file(perms_file, out, n, xis_check_lines) == 0;
    if (!ok)
        log_msg("Warning: could not write %s: %s", perms_file, strerror(errno));
    free(old);
    free(out);
    return ok;
}

void save_rule(const char *exe, int action_id, int is_allow) {
    if (!exe || *exe == '\0') return;

    if (secure_mode) {
        secure_save_rule(exe, action_id, is_allow);
        return;
    }

    const char *action_str = action_to_string(action_id);

    pthread_mutex_lock(&ignored_lock);
    for (int i = 0; i < ignored_count; i++) {
        if (strcmp(ignored_cmds[i].exe_pattern, exe) == 0 &&
            strcmp(ignored_cmds[i].action, action_str) == 0) {
            pthread_mutex_unlock(&ignored_lock);
            log_msg("Rule already exists: %s %s %s", is_allow ? "ALLOW" : "DENY", action_str, trim_exe_for_log(exe));
            return;
        }
    }

    /* Grava enquanto o mutex está preso — check+write atômico. */
    int wrote = perms_append_line("%s %s %s\n", is_allow ? "ALLOW" : "DENY", action_str, exe);
    pthread_mutex_unlock(&ignored_lock);

    if (wrote)
        load_user_config();
}

void save_denied_entry(const char *exe, int action_id) {
    save_rule(exe, action_id, 0);   /* is_allow = false */
}

/* Rewrites perms_file from the in-memory ignored_cmds array.
 * Caller must hold ignored_lock. */
static void rewrite_perms_file_locked(void) {
    char tmp[sizeof(perms_file) + 8];
    xis_write_tmpname(perms_file, tmp, sizeof(tmp));
    FILE *file = xis_write_open(tmp, perms_file);
    if (!file) {
        log_msg("Warning: could not rewrite %s: %s", perms_file, strerror(errno));
        return;
    }
    for (int i = 0; i < ignored_count; i++) {
        const char *type_str = (ignored_cmds[i].rule_type == 0) ? "ALLOW" : "DENY";
        if (ignored_cmds[i].action[0] == '\0')
            fprintf(file, "%s ALL %s\n", type_str, ignored_cmds[i].exe_pattern);
        else
            fprintf(file, "%s %s %s\n", type_str, ignored_cmds[i].action, ignored_cmds[i].exe_pattern);
    }
    if (xis_write_commit(file, tmp, perms_file, xis_check_lines) != 0)
        log_msg("Warning: could not rewrite %s: %s", perms_file, strerror(errno));
}

/* Removes a rule matching pattern + action_str (NULL/""/"ALL" = the ALL-actions rule)
 * + rule_type (0=ALLOW,1=DENY) from perms.conf. Returns 1 if a rule was removed. */
int remove_rule(const char *pattern, const char *action_str, int rule_type) {
    if (!pattern || *pattern == '\0') return 0;
    int is_all = (!action_str || *action_str == '\0' || strcasecmp(action_str, "ALL") == 0);

    pthread_mutex_lock(&ignored_lock);
    int removed = 0;
    for (int i = 0; i < ignored_count; i++) {
        int entry_is_all = (ignored_cmds[i].action[0] == '\0');
        if (strcmp(ignored_cmds[i].exe_pattern, pattern) != 0)
            continue;
        if (ignored_cmds[i].rule_type != rule_type)
            continue;
        if (entry_is_all != is_all)
            continue;
        if (!is_all && strcasecmp(ignored_cmds[i].action, action_str) != 0)
            continue;

        for (int j = i; j < ignored_count - 1; j++)
            ignored_cmds[j] = ignored_cmds[j + 1];
        ignored_count--;
        removed = 1;
        break;
    }

    if (removed)
        rewrite_perms_file_locked();

    pthread_mutex_unlock(&ignored_lock);

    if (removed)
        log_msg("Rule removed: %s %s %s", rule_type == 0 ? "ALLOW" : "DENY",
                 is_all ? "ALL" : action_str, trim_exe_for_log(pattern));

    return removed;
}

/* ====================== IGNORE REPORTS CONFIG ====================== */

void load_ignore_reports(void) {
    pthread_mutex_lock(&report_ignore_lock);
    ignored_reports_count = 0;

    FILE *file = xis_fopen_read(ignore_file, xis_check_usable);
    if (!file) {
        log_filtered(3, "No ignore.conf found at %s (normal)", ignore_file);
        pthread_mutex_unlock(&report_ignore_lock);
        return;
    }

    char line[BUF_SIZE];
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\n")] = 0;
        char *trimmed = trim(line);

        if (strlen(trimmed) == 0 || trimmed[0] == '#')
            continue;

        if (ignored_reports_count >= MAX_IGNORED) {
            log_msg("Warning: maximum ignore reports (%d) reached", MAX_IGNORED);
            continue;
        }

        strncpy(ignored_reports[ignored_reports_count].exe, trimmed, PATH_MAX - 1);
        ignored_reports[ignored_reports_count].exe[PATH_MAX - 1] = '\0';
        ignored_reports_count++;
    }

    fclose(file);
    last_ignore_mtime = time(NULL);
    log_filtered(3, "Loaded %d entries to ignore reports from %s", ignored_reports_count, ignore_file);
    pthread_mutex_unlock(&report_ignore_lock);
}

/* ---- who is using what: the last REPORTs, newest kept, for LIST_ACTIVE ---- */

#define ACTIVE_MAX 64
static struct { char exe[PATH_MAX]; int action; int pid; time_t ts; } active_tab[ACTIVE_MAX];
static pthread_mutex_t active_lock = PTHREAD_MUTEX_INITIALIZER;

static void active_seen(const char *exe, int action, int pid) {
    pthread_mutex_lock(&active_lock);
    int slot = 0;
    for (int i = 0; i < ACTIVE_MAX; i++) {
        if (active_tab[i].action == action && strcmp(active_tab[i].exe, exe) == 0) {
            slot = i;
            break;
        }
        if (active_tab[i].ts < active_tab[slot].ts)
            slot = i;
    }
    snprintf(active_tab[slot].exe, sizeof(active_tab[slot].exe), "%s", exe);
    active_tab[slot].action = action;
    active_tab[slot].pid = pid;
    active_tab[slot].ts = time(NULL);
    pthread_mutex_unlock(&active_lock);
}

/* Adds `pattern` to (or removes every line equal to it from) ignore.conf,
 * keeping comments and blank lines. Returns 1 if the file changed, 0 if
 * there was nothing to do, -1 on a write error. */
static int ignore_file_edit(int add, const char *pattern) {
    char *lines[MAX_IGNORED * 4];
    int n = 0, found = 0, changed = 0;
    FILE *f = xis_fopen_read(ignore_file, xis_check_usable);
    if (f) {
        char line[BUF_SIZE];
        while (n < (int)(sizeof(lines) / sizeof(lines[0])) && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            int same = (strcmp(trim(line), pattern) == 0);
            if (same) {
                found = 1;
                if (!add) {
                    changed = 1;
                    continue;
                }
            }
            lines[n++] = strdup(line);
        }
        fclose(f);
    }
    if (add && !found && n < (int)(sizeof(lines) / sizeof(lines[0]))) {
        lines[n++] = strdup(pattern);
        changed = 1;
    }
    int rc = changed ? 1 : 0;
    if (changed) {
        char itmp[sizeof(ignore_file) + 8];
        xis_write_tmpname(ignore_file, itmp, sizeof(itmp));
        f = xis_write_open(itmp, ignore_file);
        if (!f) {
            rc = -1;
        } else {
            for (int i = 0; i < n; i++)
                fprintf(f, "%s\n", lines[i]);
            if (xis_write_commit(f, itmp, ignore_file, xis_check_lines) != 0)
                rc = -1;
        }
    }
    for (int i = 0; i < n; i++)
        free(lines[i]);
    return rc;
}

int file_has_changed() {
    time_t current = get_config_max_mtime();
    if (current > last_config_mtime) {
        log_msg("Config file changed externally, reloading...");
        load_user_config();
        return 1;
    }
    return 0;
}

/* ====================== XNOTIFY CONF (PERSISTENT FLAGS) ====================== */

static uint32_t notify_mask_parse(const char *list);

static void safefile_log(const char *msg) {
    log_msg("%s", msg);
}

static void load_xnotify_conf(void) {
    FILE *f = xis_fopen_read(xnotify_conf_file, xis_check_usable);
    if (!f) {
        permissive_on_start = 1; /* new install */
        return;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        char *t = trim(line);
        if (!*t || *t == '#') continue;

        char key[64] = {0}, val[256] = {0};
        if (sscanf(t, "%63[^=]=%255s", key, val) != 2) continue;

        char *k = trim(key);
        char *v = trim(val);
        if (strcmp(k, "no_pause") == 0)
            no_pause_mode = atoi(v) ? 1 : 0;
        else if (strcmp(k, "quiet") == 0)
            quiet_mode = atoi(v) ? 1 : 0;
        else if (strcmp(k, "always_kill") == 0)
            always_kill_mode = atoi(v) ? 1 : 0;
        else if (strcmp(k, "log_level") == 0) {
            int lvl = atoi(v);
            if (lvl >= 0 && lvl <= 4) log_level = lvl;
        } else if (strcmp(k, "secure_mode") == 0)
            secure_mode = atoi(v) ? 1 : 0;
        else if (strcmp(k, "permissive_on_start") == 0)
            permissive_on_start = atoi(v) ? 1 : 0;
        else if (strcmp(k, "subscribers") == 0)
            snprintf(subscribers_allow, sizeof(subscribers_allow), "%s", v);
        else if (strcmp(k, "notify_actions") == 0)
            notify_mask = notify_mask_parse(v);
    }
    fclose(f);
    last_xnotify_conf_mtime = time(NULL);
    log_filtered(3, "Loaded xnotify.conf: no_pause=%d quiet=%d always_kill=%d log_level=%d secure_mode=%d",
                 no_pause_mode, quiet_mode, always_kill_mode, log_level, secure_mode);
}

/* "SCREEN,COMPOSITE" <-> bit mask; "NONE" or empty is no action at all. */
static uint32_t notify_mask_parse(const char *list) {
    uint32_t mask = 0;
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", list);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int a = string_to_action(trim(tok));
        if (a > 0 && a <= 16)
            mask |= 1u << (a - 1);
    }
    return mask;
}

static void notify_mask_format(char *out, size_t outsz) {
    size_t o = 0;
    out[0] = '\0';
    for (int i = 0; action_names[i].name && o + 1 < outsz; i++) {
        int id = action_names[i].id;
        if (id > 0 && id <= 16 && (notify_mask & (1u << (id - 1))))
            o += (size_t)snprintf(out + o, outsz - o, "%s%s", o ? "," : "", action_names[i].name);
    }
}

static void save_xnotify_conf(void) {
    if (!xnotify_conf_file[0]) return;
    char tmp[sizeof(xnotify_conf_file) + 8];
    xis_write_tmpname(xnotify_conf_file, tmp, sizeof(tmp));
    FILE *f = xis_write_open(tmp, xnotify_conf_file);
    if (!f) {
        log_msg("Warning: could not save xnotify.conf: %s", strerror(errno));
        return;
    }
    fprintf(f, "# XisGuard runtime configuration — edited automatically on startup\n");
    fprintf(f, "no_pause=%d\n", no_pause_mode);
    fprintf(f, "quiet=%d\n", quiet_mode);
    fprintf(f, "always_kill=%d\n", always_kill_mode);
    fprintf(f, "log_level=%d\n", log_level);
    fprintf(f, "secure_mode=%d\n", secure_mode);
    fprintf(f, "permissive_on_start=%d\n", permissive_on_start);
    fprintf(f, "subscribers=%s\n", subscribers_allow);
    char na[512];
    notify_mask_format(na, sizeof(na));
    fprintf(f, "notify_actions=%s\n", na[0] ? na : "NONE");
    if (xis_write_commit(f, tmp, xnotify_conf_file, xis_check_conf_clean) != 0) {
        log_msg("Warning: could not save xnotify.conf: %s", strerror(errno));
        return;
    }
    last_xnotify_conf_mtime = time(NULL);
    log_filtered(3, "Saved xnotify.conf");
}

static void check_xnotify_conf_changed(void) {
    if (!xnotify_conf_file[0]) return;
    struct stat st;
    if (stat(xnotify_conf_file, &st) != 0) return;
    if (st.st_mtime > last_xnotify_conf_mtime) {
        log_msg("xnotify.conf changed externally, reloading...");
        load_xnotify_conf();
    }
}

/* ====================== IGNORED CHECK ====================== */

int is_ignored(const char *exe, int action_id) {
    if (!exe || *exe == '\0' || strcmp(exe, "?") == 0) {
        return 0;
    }

    const char *action_name = action_to_string(action_id);
    if (!action_name || strcmp(action_name, "UNKNOWN") == 0)
        return 0;

    pthread_mutex_lock(&ignored_lock);
    for (int i = 0; i < ignored_count; i++) {
        char exe_buf[PATH_MAX];
        const char *match_key;
        if (strchr(ignored_cmds[i].exe_pattern, '|') == NULL) {
            strncpy(exe_buf, exe, sizeof(exe_buf) - 1);
            exe_buf[sizeof(exe_buf) - 1] = '\0';
            char *p = strchr(exe_buf, '|');
            if (p) *p = '\0';
            match_key = exe_buf;
        } else {
            match_key = exe;
        }

        if (!pattern_matches(match_key, ignored_cmds[i].exe_pattern))
            continue;

        if (ignored_cmds[i].action[0] == '\0' ||
            strcasecmp(ignored_cmds[i].action, action_name) == 0) {
            pthread_mutex_unlock(&ignored_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&ignored_lock);
    return 0;
}

int is_report_ignored(const char *exe) {
    if (!exe || *exe == '\0' || strcmp(exe, "?") == 0)
        return 0;

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s", exe);
    char *bar = strchr(path, '|');
    if (bar)
        *bar = '\0';
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; ignored_builtin[i]; i++)
        if (strcmp(base, ignored_builtin[i]) == 0)
            return 1;

    pthread_mutex_lock(&report_ignore_lock);
    for (int i = 0; i < ignored_reports_count; i++) {
        if (pattern_matches(exe, ignored_reports[i].exe)) {
            pthread_mutex_unlock(&report_ignore_lock);
            return 1;
        }
    }
    pthread_mutex_unlock(&report_ignore_lock);
    return 0;
}

/* ====================== DIALOG BACKENDS ====================== */

/* Escapa aspas simples para uso dentro de strings single-quoted no shell.
 * ' → '\'' (fecha aspas, barra+aspas literal, reabre aspas). */
static void shell_sq_escape(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_sz; i++) {
        if (src[i] == '\'') {
            if (j + 4 >= dst_sz) break;
            dst[j++] = '\'';
            dst[j++] = '\\';
            dst[j++] = '\'';
            dst[j++] = '\'';
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Replace '|' with space for display ("exe args" instead of "exe|args"),
 * then shell-escape into safe_exe. Shared by every dialog backend. */
static void prepare_display_exe(const struct Alert *alert, char *safe_exe, size_t safe_exe_sz) {
    char display_exe[PATH_MAX + 1024];
    strncpy(display_exe, alert->exe, sizeof(display_exe) - 1);
    display_exe[sizeof(display_exe) - 1] = '\0';
    char *pipe_pos = strchr(display_exe, '|');
    if (pipe_pos) *pipe_pos = ' ';

    shell_sq_escape(safe_exe, safe_exe_sz, trim_exe_for_log(display_exe));
}

/* Looks up prog in $PATH the same way execvp/the shell would, without
 * actually running it. Used at startup to pick a dialog backend. */
static int prog_in_path(const char *prog) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env)
        path_env = "/usr/local/bin:/usr/bin:/bin";

    char paths[4096];
    strncpy(paths, path_env, sizeof(paths) - 1);
    paths[sizeof(paths) - 1] = '\0';

    char *saveptr = NULL;
    for (char *dir = strtok_r(paths, ":", &saveptr); dir; dir = strtok_r(NULL, ":", &saveptr)) {
        char full[PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", dir, prog);
        if (access(full, X_OK) == 0)
            return 1;
    }
    return 0;
}

/* Picks which dialog program to use, once, at startup: xisserve is the
 * default (--question, see xisserve/PROTOCOL.md), zenity is the fallback
 * for systems that don't have it. */
void detect_dialog_backend(void) {
    if (prog_in_path("xisserve")) {
        dialog_backend = DIALOG_XISSERVE;
        log_msg("Dialog backend: xisserve (default)");
    } else if (prog_in_path("zenity")) {
        dialog_backend = DIALOG_ZENITY;
        log_msg("Dialog backend: zenity (fallback, xisserve not found in PATH)");
    } else {
        dialog_backend = DIALOG_NONE;
        log_msg("Dialog backend: none found (xisserve/zenity missing) - unauthorized processes will be denied for the session");
    }
}

static int show_zenity_dialog(const struct Alert *alert) {
    char zenity_cmd[8192];
    const char *action_str = action_to_string(alert->action);
    const char *action_desc = action_to_description(alert->action);

    char safe_exe[512];
    prepare_display_exe(alert, safe_exe, sizeof(safe_exe));

    int has_args = (strchr(alert->exe, '|') != NULL);

    /* Labels are %s-substituted (never compile-time concatenated) so
     * each is looked up via _() at runtime -- see the BTN_* doc comment
     * above. TRANSLATORS: these go inside single-quoted shell
     * arguments; don't use an apostrophe in a translated label. */
    if (has_args) {
        snprintf(zenity_cmd, sizeof(zenity_cmd),
            "zenity --question "
            "--title='XisGuard' "
            "--icon-name=dialog-warning "
            "--modal "
            "--timeout=90 "
            "--text='<b>%s:</b> %s (%s)\n"
                     "%s: <b>%s</b> (%d)' "
            "--ok-label='%s' "
            "--cancel-label='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--width=550 "
            "--no-wrap 2>/dev/null",
            _("Permission"), action_str, action_desc, _("Program"), safe_exe, alert->pid,
            _(BTN_ALLOW), _(BTN_DENY_SESSION), _(BTN_ALLOW_SESSION), _(BTN_DENY),
            _(BTN_ALLOW_EXACT), _(BTN_ALLOW_EXACT_SESSION), _(BTN_TRUST), _(BTN_TRUST_SESSION),
            _(BTN_TRUST_EXACT), _(BTN_TRUST_EXACT_SESSION));
    } else {
        snprintf(zenity_cmd, sizeof(zenity_cmd),
            "zenity --question "
            "--title='XisGuard' "
            "--icon-name=dialog-warning "
            "--modal "
            "--timeout=90 "
            "--text='<b>%s:</b> %s (%s)\n"
                     "%s: <b>%s</b> (%d)' "
            "--ok-label='%s' "
            "--cancel-label='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--extra-button='%s' "
            "--width=550 "
            "--no-wrap 2>/dev/null",
            _("Permission"), action_str, action_desc, _("Program"), safe_exe, alert->pid,
            _(BTN_ALLOW), _(BTN_DENY_SESSION), _(BTN_ALLOW_SESSION), _(BTN_DENY),
            _(BTN_TRUST), _(BTN_TRUST_SESSION));
    }

    log_msg("Showing Zenity dialog for %s %s (%d)",
            action_str, trim_exe_for_log(alert->exe), alert->pid);

    FILE *fp = popen(zenity_cmd, "r");
    if (!fp) {
        log_msg("ERROR: failed to call zenity");
        return 99;
    }

    char output[256] = {0};
    if (fgets(output, sizeof(output), fp) != NULL) {
        output[strcspn(output, "\n")] = '\0';
    }

    int status = pclose(fp);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    log_msg("Zenity returned %d | '%s'", code, output);

    /* Matched against _(BTN_X) -- the same translated text the dialog
     * actually displayed and zenity echoed back -- never against the
     * English literal, so this still works under any locale. */
    if (code == 0) {
        return 0;  /* BTN_ALLOW (exe only) */
    } else if (strstr(output, _(BTN_TRUST_EXACT_SESSION)) != NULL) {
        return 8;  /* Trust all this session exact (exe|args) */
    } else if (strstr(output, _(BTN_TRUST_EXACT)) != NULL) {
        return 7;  /* Trust all permanent exact (exe|args) */
    } else if (strstr(output, _(BTN_TRUST_SESSION)) != NULL) {
        return 6;  /* Trust all this session (exe only) */
    } else if (strstr(output, _(BTN_TRUST)) != NULL) {
        return 3;  /* Trust all permanent (exe only) */
    } else if (strstr(output, _(BTN_ALLOW_EXACT_SESSION)) != NULL) {
        return 5;  /* Allow exact this session (exe|args) */
    } else if (strstr(output, _(BTN_ALLOW_EXACT)) != NULL) {
        return 4;  /* Allow exact permanent (exe|args) */
    } else if (strstr(output, _(BTN_ALLOW_SESSION)) != NULL) {
        return 1;  /* Allow this session (exe only) */
    } else if (strstr(output, _(BTN_DENY)) != NULL) {
        return 2;  /* Deny permanent (exe only) */
    } else {
        return 99; /* BTN_DENY_SESSION (cancel/timeout) */
    }
}

/* xisserve --question takes at most 8 --button flags (see
 * xisserve/PROTOCOL.md), one short of the 10 choices the exe|args case
 * offers on zenity. The two dropped there are the rarest combination
 * (EXACT + session-only); everything else -- permanent EXACT included --
 * stays available. Each button's label:value pair answers with the same
 * response codes show_zenity_dialog()/process_next_alert() already use,
 * so no string matching is needed on the way back. */
static int show_xisserve_dialog(const struct Alert *alert) {
    char xisserve_cmd[8192];
    const char *action_str = action_to_string(alert->action);
    const char *action_desc = action_to_description(alert->action);

    char safe_exe[512];
    prepare_display_exe(alert, safe_exe, sizeof(safe_exe));

    int has_args = (strchr(alert->exe, '|') != NULL);

    /* The numeric :N values are the protocol -- xisserve prints the
     * value verbatim (see below), never the label -- so only the label
     * itself needs _(); the value stays exactly as process_next_alert()
     * already expects. */
    if (has_args) {
        snprintf(xisserve_cmd, sizeof(xisserve_cmd),
            "xisserve --question --title=XisGuard "
            "--text='%s: %s (%s)\n"
                    "%s: %s (%d)' "
            "--button='%s:0' "
            "--button='%s:2' "
            "--button='%s:1' "
            "--button='%s:99' "
            "--button='%s:3' "
            "--button='%s:6' "
            "--button='%s:4' "
            "--button='%s:7' "
            "2>/dev/null",
            _("Permission"), action_str, action_desc, _("Program"), safe_exe, alert->pid,
            _(BTN_ALLOW), _(BTN_DENY), _(BTN_ALLOW_SESSION), _(BTN_DENY_SESSION),
            _(BTN_TRUST), _(BTN_TRUST_SESSION), _(BTN_ALLOW_EXACT), _(BTN_TRUST_EXACT));
    } else {
        snprintf(xisserve_cmd, sizeof(xisserve_cmd),
            "xisserve --question --title=XisGuard "
            "--text='%s: %s (%s)\n"
                    "%s: %s (%d)' "
            "--button='%s:0' "
            "--button='%s:2' "
            "--button='%s:1' "
            "--button='%s:99' "
            "--button='%s:3' "
            "--button='%s:6' "
            "2>/dev/null",
            _("Permission"), action_str, action_desc, _("Program"), safe_exe, alert->pid,
            _(BTN_ALLOW), _(BTN_DENY), _(BTN_ALLOW_SESSION), _(BTN_DENY_SESSION),
            _(BTN_TRUST), _(BTN_TRUST_SESSION));
    }

    log_msg("Showing xisserve dialog for %s %s (%d)",
            action_str, trim_exe_for_log(alert->exe), alert->pid);

    FILE *fp = popen(xisserve_cmd, "r");
    if (!fp) {
        log_msg("ERROR: failed to call xisserve");
        return 99;
    }

    char output[64] = {0};
    int got_output = (fgets(output, sizeof(output), fp) != NULL);
    if (got_output) output[strcspn(output, "\n")] = '\0';

    int status = pclose(fp);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;

    int response;
    if (got_output && output[0] != '\0') {
        /* A button was clicked: xisserve prints <valor> straight to
         * stdout, no label matching needed. */
        response = atoi(output);
    } else {
        /* Dismissed (WM close / Escape) exits 1 with no stdout; a usage
         * error exits 2. Either way there's no answer, so treat it the
         * same as zenity's cancel/timeout. */
        response = 99;
    }

    log_msg("xisserve returned %d (exit %d) | '%s'", response, exit_code, output);
    return response;
}

/* Dispatches to whichever dialog backend detect_dialog_backend() picked
 * at startup. */
int show_dialog(const struct Alert *alert) {
    switch (dialog_backend) {
        case DIALOG_XISSERVE:
            return show_xisserve_dialog(alert);
        case DIALOG_ZENITY:
            return show_zenity_dialog(alert);
        default:
            log_msg("No dialog backend available; denying for this session");
            return 99;
    }
}


/* ====================== ALERT HANDLING ====================== */

void save_ignored_entry(const char *exe, int action_id) {
    if (!exe || *exe == '\0') return;

    const char *action_str = action_to_string(action_id);

    pthread_mutex_lock(&ignored_lock);

    for (int i = 0; i < ignored_count; i++) {
        if (strcmp(ignored_cmds[i].exe_pattern, exe) == 0 &&
            strcmp(ignored_cmds[i].action, action_str) == 0) {
            pthread_mutex_unlock(&ignored_lock);
            log_msg("Rule already exists: %s:%s", trim_exe_for_log(exe), action_str);
            return;
        }
    }

    /* Grava enquanto o mutex está preso — check+write atômico. */
    int wrote = perms_append_line("%s %s %s\n", "ALLOW", action_str, exe);
    pthread_mutex_unlock(&ignored_lock);

    if (wrote)
        load_user_config();
}

int is_seen(const char *exe, int action) {
    for (int i = 0; i < seen_count; i++) {
        if (strcmp(seen_alerts[i].exe, exe) == 0 && seen_alerts[i].action == action) {
            return 1;
        }
    }
    return 0;
}

void add_seen(const char *exe, int action) {
    if (seen_count < MAX_ALERTS) {
        snprintf(seen_alerts[seen_count].exe, sizeof(seen_alerts[seen_count].exe), "%s", exe);
        seen_alerts[seen_count].action = action;
        seen_count++;
    }
}

void remove_seen_for_exe(const char *exe, int action) {
    int i = 0;
    while (i < seen_count) {
        if (strcmp(seen_alerts[i].exe, exe) == 0 && seen_alerts[i].action == action) {
            for (int j = i; j < seen_count - 1; j++) {
                seen_alerts[j] = seen_alerts[j + 1];
            }
            seen_count--;
        } else {
            i++;
        }
    }
}

void remove_all_alerts_for_pid(pid_t pid) {
    int i = 0;
    while (i < alert_count) {
        if (alert_queue[i].pid == pid) {
            for (int j = i; j < alert_count - 1; j++) {
                alert_queue[j] = alert_queue[j + 1];
            }
            alert_count--;
        } else {
            i++;
        }
    }
}

/* Sends a raw JSON command to the X server's xperms.<display>.sock (the
 * same guard -> server channel used by send_permission()). Fire-and-forget,
 * like every other command on this socket; replies (if any) arrive later as
 * a datagram on our own xnotify.<display>.sock, handled by handle_message(). */
static void send_raw_command(const char *json) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *base_dir = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";

    char socket_path[108];
    snprintf(socket_path, sizeof(socket_path), "%s/xperms.%d.sock", base_dir, display);

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock == -1) return;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    sendto(sock, json, strlen(json), 0, (struct sockaddr*)&addr, sizeof(addr));
    close(sock);
}

void send_query_action(const char *action) {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "{\"command\":\"QUERY_ACTION\",\"action\":\"%s\"}", action);
    send_raw_command(msg);
}

/* Secure mode only: asks the X server for its real rule drop-in directory
 * (SYSCONFDIR "/xnotify.conf.d") since SYSCONFDIR is a compile-time constant
 * of the server, not of xisguard. The reply comes back as a "CONFIG_PATH"
 * message handled in handle_message(). */
static void request_config_path(void) {
    send_raw_command("{\"command\":\"GET_CONFIG_PATH\"}");
}

/* Asks the server to re-read xnotify.conf / xnotify.conf.d from disk, e.g.
 * right after secure_save_rule() has written a new drop-in file there. */
static void request_server_reload(void) {
    send_raw_command("{\"command\":\"RELOAD\"}");
    send_all_permissions_to_xserver();   /* the reload dropped the user rules and the global mask */
}

static void subscribers_broadcast_state(int enabled);
static void subscribers_broadcast_permissive(int enabled);

/* ====================== "XNOTIFY DISABLED" NOTICE ======================
 *
 * The X server lets us switch Xnotify off at runtime ("DISABLE"). That is only
 * acceptable if the user cannot forget about it, so the server is built to be
 * noisy and self-healing about it:
 *
 *   - while disabled, it re-sends a STATUS datagram ({"enabled":false}) to us
 *     every ~5 s (piggybacked on its guard-liveness timer);
 *   - the disable only lasts as long as our heartbeat (send_heartbeat()): if we
 *     crash, are killed or exit, the server re-enables itself within ~5 s.
 *
 * Our half of the contract is below. Every "enabled":false STATUS is treated as
 * "still disabled" and keeps a persistent dialog on screen that offers to
 * re-enable. When STATUS stops arriving, or says enabled:true, the dialog is
 * closed. Two rules to keep in mind when changing this code:
 *
 *   1. NEVER block the main loop here. It sends the heartbeat; if it stalls for
 *      more than the server's GUARD_HEARTBEAT_TIMEOUT (5 s) the server will
 *      declare us dead and re-enable. That is safe but confusing for the user.
 *      So the dialog lives in its own thread (disabled_notice_loop()).
 *   2. ENABLE/DISABLE must be sent from THIS process: the server authenticates
 *      the sender by SCM_CREDENTIALS + /proc/<pid>/exe, which must match the
 *      "ALLOW XNOTIFY" rule for xisguard. A helper child would be rejected.
 */

/* No STATUS for this long while we believe the server is disabled => assume it
 * re-enabled itself (e.g. our heartbeat lapsed) and drop the dialog. The server
 * sends one every ~5 s, so 12 s tolerates a lost datagram or two. */
#define NOTICE_STATUS_STALE_S   12
/* After the user dismisses the dialog without re-enabling, leave them alone
 * this long before showing it again (the server is still disabled). */
#define NOTICE_SNOOZE_S         60

/* Written by the main thread (STATUS handler / notice_tick()); read by it too.
 * The dialog thread only reads notice_close_requested. */
static int server_disabled = 0;               /* last STATUS said enabled:false */
static time_t last_disabled_status = 0;       /* when that STATUS arrived */
static time_t notice_snooze_until = 0;        /* don't re-open before this */

static pthread_t notice_thread;
static int notice_thread_valid = 0;           /* notice_thread needs a join */
static volatile int notice_active = 0;        /* dialog thread is running */
/* volatile sig_atomic_t: also set from cleanup(), which runs in a signal handler */
static volatile sig_atomic_t notice_close_requested = 0;

/* Dialog thread: runs xisserve/zenity as a child, waits for the user's answer
 * and, if they chose "re-enable", sends ENABLE. Polls its pipe with a short
 * timeout so it can notice notice_close_requested and kill the dialog. */
static void *disabled_notice_loop(void *arg) {
    (void)arg;

    char text[512];
    snprintf(text, sizeof(text), "%s\n%s",
        _("Xnotify protection is DISABLED."),
        _("It is re-enabled automatically if XisGuard stops or is closed."));

    char text_arg[600], b_enable[160], b_keep[160];
    snprintf(text_arg, sizeof(text_arg), "--text=%s", text);

    int is_xisserve = (dialog_backend == DIALOG_XISSERVE);
    /* argv is built before fork(): nothing but dup2/exec happens in the child. */
    char *argv_xisserve[] = {
        "xisserve", "--question", "--title=XisGuard", text_arg, b_enable, b_keep, NULL };
    char ok_arg[200], cancel_arg[200];
    char *argv_zenity[] = {
        "zenity", "--question", "--title=XisGuard", "--icon-name=dialog-warning",
        text_arg, ok_arg, cancel_arg, "--no-wrap", NULL };

    /* xisserve prints the clicked button's value ("1" = re-enable, "0" = keep
     * disabled); zenity signals the same through its exit code (0 = OK). */
    snprintf(b_enable, sizeof(b_enable), "--button=%s:1", _("Re-enable Xnotify"));
    snprintf(b_keep, sizeof(b_keep), "--button=%s:0", _("Keep disabled"));
    snprintf(ok_arg, sizeof(ok_arg), "--ok-label=%s", _("Re-enable Xnotify"));
    snprintf(cancel_arg, sizeof(cancel_arg), "--cancel-label=%s", _("Keep disabled"));

    int pfd[2];
    if (pipe(pfd) != 0) {
        log_msg("Disabled notice: pipe() failed: %s", strerror(errno));
        notice_active = 0;
        return NULL;
    }

    pid_t child = fork();
    if (child < 0) {
        log_msg("Disabled notice: fork() failed: %s", strerror(errno));
        close(pfd[0]); close(pfd[1]);
        notice_active = 0;
        return NULL;
    }
    if (child == 0) {
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[0]); close(pfd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) dup2(devnull, STDERR_FILENO);
        if (is_xisserve) execvp("xisserve", argv_xisserve);
        else             execvp("zenity", argv_zenity);
        _exit(127);
    }
    close(pfd[1]);

    log_msg("Showing 'Xnotify disabled' notice");

    char out[32] = {0};
    size_t n = 0;
    int closed_by_us = 0;
    for (;;) {
        if (notice_close_requested) {   /* server re-enabled, or we are exiting */
            kill(child, SIGTERM);
            closed_by_us = 1;
            break;
        }
        struct pollfd p = { .fd = pfd[0], .events = POLLIN };
        int r = poll(&p, 1, 500);
        if (r > 0) {
            ssize_t k = read(pfd[0], out + n, sizeof(out) - 1 - n);
            if (k <= 0) break;          /* EOF: the dialog closed */
            n += (size_t)k;
        } else if (r < 0 && errno != EINTR) {
            break;
        }
    }
    close(pfd[0]);

    int status = 0;
    waitpid(child, &status, 0);

    int reenable = 0;
    if (!closed_by_us) {
        reenable = is_xisserve ? (atoi(out) == 1)
                               : (WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }

    if (reenable) {
        log_msg("Disabled notice: user asked to re-enable Xnotify");
        send_raw_command("{\"command\":\"ENABLE\"}");
        /* The server answers with STATUS enabled:true, which clears our state. */
    } else if (!closed_by_us) {
        /* Dismissed / "Keep disabled": still disabled, so don't nag instantly. */
        notice_snooze_until = time(NULL) + NOTICE_SNOOZE_S;
        log_msg("Disabled notice dismissed; snoozed for %d s", NOTICE_SNOOZE_S);
    }

    notice_active = 0;
    return NULL;
}

/* Opens the dialog if the server is disabled and none is showing. Main thread. */
static void notice_ensure_open(void) {
    if (notice_active || should_exit)
        return;
    if (time(NULL) < notice_snooze_until)
        return;
    if (dialog_backend == DIALOG_NONE) {
        /* Nothing to show it with. The log is the only trace; the periodic
         * STATUS still reaches us, so keep it at a low-noise level. */
        log_filtered(2, "Xnotify is disabled but no dialog backend is available to tell the user");
        return;
    }

    /* The previous thread has finished (notice_active == 0): reap it first. */
    if (notice_thread_valid) {
        pthread_join(notice_thread, NULL);
        notice_thread_valid = 0;
    }
    notice_close_requested = 0;
    notice_active = 1;
    if (pthread_create(&notice_thread, NULL, disabled_notice_loop, NULL) == 0) {
        notice_thread_valid = 1;
    } else {
        notice_active = 0;
        log_msg("Disabled notice: pthread_create failed");
    }
}

/* Asks a running dialog to close itself. Async-signal-safe (used by cleanup()). */
static void notice_request_close(void) {
    notice_close_requested = 1;
}

/* Handles a server STATUS datagram. Main thread (called from handle_message). */
static void notice_on_status(int enabled) {
    if (enabled) {
        if (server_disabled) {
            log_msg("Xnotify is enabled again");
            subscribers_broadcast_state(1);
        }
        server_disabled = 0;
        notice_snooze_until = 0;        /* next disable should notify at once */
        notice_request_close();
        return;
    }
    if (!server_disabled) {
        log_msg("WARNING: Xnotify is DISABLED at runtime (re-enables if XisGuard stops)");
        subscribers_broadcast_state(0);
    }
    server_disabled = 1;
    last_disabled_status = time(NULL);
    notice_ensure_open();
}

/* Called every main-loop iteration: gives up on a "disabled" state whose STATUS
 * stream has gone quiet, and re-opens the dialog after a snooze expires. */
static void notice_tick(void) {
    if (!server_disabled)
        return;
    if (time(NULL) - last_disabled_status > NOTICE_STATUS_STALE_S) {
        log_msg("No Xnotify STATUS for %d s - assuming it re-enabled itself",
                NOTICE_STATUS_STALE_S);
        server_disabled = 0;
        subscribers_broadcast_state(1);
        notice_request_close();
        return;
    }
    notice_ensure_open();
}

/* Blocks (briefly) until secure_conf_dir is known, requesting it from the
 * server if needed. Returns 1 and fills out on success, 0 on timeout. */
static int wait_for_secure_conf_dir(char *out, size_t out_sz, int timeout_ms) {
    pthread_mutex_lock(&secure_conf_lock);
    if (secure_conf_dir[0] == '\0') {
        request_config_path();

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000;
        ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        while (secure_conf_dir[0] == '\0') {
            if (pthread_cond_timedwait(&secure_conf_cond, &secure_conf_lock, &ts) != 0)
                break;
        }
    }

    int ok = (secure_conf_dir[0] != '\0');
    if (ok)
        snprintf(out, out_sz, "%s", secure_conf_dir);
    pthread_mutex_unlock(&secure_conf_lock);
    return ok;
}

/* Secure mode only: writes exe/action_id/is_allow as a rule line into the
 * X server's own SYSCONFDIR/xnotify.conf.d/50-xisguard.conf, escalating via
 * polkit since that path normally requires root, then asks the server to
 * reload. exe/pattern content never touches a shell string: it is written
 * to a plain temp file and pkexec's helper script only ever sees the
 * directory and temp file paths as argv, so there is no injection vector. */
static void secure_save_rule(const char *exe, int action_id, int is_allow) {
    if (!exe || *exe == '\0') return;

    char dir[512];
    if (!wait_for_secure_conf_dir(dir, sizeof(dir), 2000)) {
        log_msg("Secure mode: could not learn the server's config directory in time; "
                "rule for %s was NOT saved permanently (this session only)", trim_exe_for_log(exe));
        return;
    }

    const char *action_str = (action_id == -1) ? "ALL" : action_to_string(action_id);

    char tmp_path[] = "/tmp/xisguard-rule-XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        log_msg("Secure mode: mkstemp failed: %s", strerror(errno));
        return;
    }
    FILE *f = fdopen(fd, "w");
    if (!f) {
        close(fd);
        unlink(tmp_path);
        return;
    }
    fprintf(f, "%s %s %s\n", is_allow ? "ALLOW" : "DENY", action_str, exe);
    fclose(f);
    chmod(tmp_path, 0644);

    log_msg("Secure mode: asking for admin rights (polkit) to write '%s %s %s' into %s/50-xisguard.conf",
            is_allow ? "ALLOW" : "DENY", action_str, trim_exe_for_log(exe), dir);

    pid_t pid = fork();
    if (pid == 0) {
        execlp("pkexec", "pkexec", "sh", "-c",
               "mkdir -p \"$1\" && cat \"$2\" >> \"$1/50-xisguard.conf\"",
               "sh", dir, tmp_path, (char*)NULL);
        _exit(127);
    }

    int ok = 0;
    if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    } else {
        log_msg("Secure mode: fork() failed: %s", strerror(errno));
    }

    unlink(tmp_path);

    if (ok) {
        log_msg("Secure mode: rule saved to %s/50-xisguard.conf, requesting server reload", dir);
        request_server_reload();
    } else {
        log_msg("Secure mode: could not save the rule permanently (polkit denied/cancelled, or write failed); "
                "'%s' stays allowed for this session only", trim_exe_for_log(exe));
    }
}

/* Escapa " e \ em strings JSON; descarta caracteres de controle. */
static void json_escape_str(char *dst, size_t dst_sz, const char *src) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 1 < dst_sz; i++) {
        if (src[i] == '"' || src[i] == '\\') {
            if (j + 2 >= dst_sz) break;
            dst[j++] = '\\';
            dst[j++] = src[i];
        } else if ((unsigned char)src[i] >= 0x20) {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
}

/* Extracts a "key":"value" string field from a flat JSON object into dst.
 * Returns 1 if found, 0 otherwise (dst left as empty string). */
static int json_get_str(const char *msg, const char *key, char *dst, size_t dst_sz) {
    dst[0] = '\0';
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    if (*p != '"') return 0;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return 0;
    size_t len = (size_t)(end - p);
    if (len >= dst_sz) len = dst_sz - 1;
    strncpy(dst, p, len);
    dst[len] = '\0';
    return 1;
}

/* Extracts a "key":<int> numeric field. Returns 1 if found. */
static int json_get_int(const char *msg, const char *key, int *out) {
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ') p++;
    return sscanf(p, "%d", out) == 1;
}

void send_permission(int action, const char *exe, pid_t pid, int command_type) {
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *base_dir = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";

    char socket_path[108];
    snprintf(socket_path, sizeof(socket_path), "%s/xperms.%d.sock", base_dir, display);

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock == -1) {
        log_msg("Failed to create permission socket: %s", strerror(errno));
        return;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path);

    char safe_exe[1024];
    json_escape_str(safe_exe, sizeof(safe_exe), exe ? exe : "");

    char msg[1280];
    const char *cmd_name = "ALLOW";

    switch (command_type) {
        case COMMAND_HEARTBEAT:  /* heartbeat */
            snprintf(msg, sizeof(msg), "{\"command\":\"XNOTIFY\",\"pid\":%d}", pid);
            cmd_name = "HEARTBEAT";
            break;
        case COMMAND_ALLOW_ALL:  /* ALLOW_ALL */
            snprintf(msg, sizeof(msg), "{\"command\":\"ALLOW_ALL\",\"exe\":\"%s\",\"action\":%d}", safe_exe, action);
            cmd_name = "ALLOW";
            break;
        case COMMAND_ALLOW_ACTION:  /* ALLOW_ACTION */
            snprintf(msg, sizeof(msg), "{\"command\":\"ALLOW_ACTION\",\"action\":%d}", action);
            cmd_name = "ALLOW ACTION";
            break;
        case COMMAND_DENY:  /* DENY */
            snprintf(msg, sizeof(msg), "{\"command\":\"DENY\",\"action\":%d,\"exe\":\"%s\"}", action, safe_exe);
            cmd_name = "DENY";
            break;
        case COMMAND_DENY_ALL: /* DENY_ALL */
            snprintf(msg, sizeof(msg), "{\"command\":\"DENY_ALL\",\"exe\":\"%s\"}", safe_exe);
            cmd_name = "DENY";
            break;
        case COMMAND_DENY_ACTION: /* DENY_ACTION */
            snprintf(msg, sizeof(msg), "{\"command\":\"DENY_ACTION\",\"action\":%d}", action);
            cmd_name = "DENY ACTION";
            break;
        default: /* ALLOW */
            snprintf(msg, sizeof(msg), "{\"command\":\"ALLOW\",\"action\":%d,\"exe\":\"%s\"}", action, safe_exe);
            cmd_name = "ALLOW";
            break;
    }

    ssize_t sent = sendto(sock, msg, strlen(msg), 0,
                          (struct sockaddr*)&addr, sizeof(addr));

    close(sock);

    if (sent == -1) {
        log_msg("Failed to send permission: %s", strerror(errno));
    } else if (command_type != 1) {
        log_filtered(2, "Sent: %s %s for %s", cmd_name, action_to_string(action), trim_exe_for_log(exe));
    }
}

static void subscribers_broadcast(const char *ev, int action, pid_t pid, const char *exe, int ignored);

void handle_message(const char *msg) {
    int action = 0;
    pid_t pid = 0;
    char exe[PATH_MAX] = "?";
    char args[1024] = {0};
    char command[64] = "?";

    char *p = strstr(msg, "\"command\":\"");
    if (p) {
        p += 11;
        char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (len >= sizeof(command)) len = sizeof(command) - 1;
            strncpy(command, p, len);
            command[len] = '\0';
        }
    }

    /* Reply to request_config_path(); has no action/pid, so handle it before
     * the action<=0||pid<=0 bail-out below. */
    if (strcmp(command, "CONFIG_PATH") == 0) {
        char dir[512];
        if (json_get_str(msg, "dir", dir, sizeof(dir))) {
            pthread_mutex_lock(&secure_conf_lock);
            snprintf(secure_conf_dir, sizeof(secure_conf_dir), "%s", dir);
            json_get_str(msg, "file", secure_conf_file, sizeof(secure_conf_file));
            pthread_cond_broadcast(&secure_conf_cond);
            pthread_mutex_unlock(&secure_conf_lock);
            log_filtered(2, "Secure mode: server config directory is %s", dir);
        }
        return;
    }

    /* Server's periodic/answer STATUS ({"command":"STATUS","enabled":bool,...}).
     * No action/pid either, so it must also be handled before the bail-out. It
     * drives the "Xnotify disabled" notice, see notice_on_status(). */
    if (strcmp(command, "STATUS") == 0) {
        const char *e = strstr(msg, "\"enabled\":");
        if (e)
            notice_on_status(strncmp(e + 10, "true", 4) == 0);
        return;
    }

    p = strstr(msg, "\"action\":");
    if (p)
        sscanf(p + 9, "%d", &action);

    p = strstr(msg, "\"pid\":");
    if (p)
        sscanf(p + 6, "%d", &pid);

    p = strstr(msg, "\"exe\":\"");
    if (p) {
        p += 7;
        char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (len >= sizeof(exe)) len = sizeof(exe) - 1;
            strncpy(exe, p, len);
            exe[len] = '\0';
        }
    }

    p = strstr(msg, "\"args\":\"");
    if (p) {
        p += 8;
        char *end = strchr(p, '"');
        if (end) {
            size_t len = (size_t)(end - p);
            if (len >= sizeof(args)) len = sizeof(args) - 1;
            strncpy(args, p, len);
            args[len] = '\0';
        }
    }

    if (exe[0] == '\0') {
        strcpy(exe, "?");
    }

    /* Build combined key exe|args when args are present */
    if (args[0] != '\0' && exe[0] != '\0' && strcmp(exe, "?") != 0) {
        size_t exe_len = strlen(exe);
        size_t args_len = strlen(args);
        if (exe_len + 1 + args_len < (size_t)(PATH_MAX - 1)) {
            exe[exe_len] = '|';
            strncpy(exe + exe_len + 1, args, PATH_MAX - exe_len - 2);
            exe[PATH_MAX - 1] = '\0';
        }
    }

    if (action <= 0 || pid <= 0) 
        return;

    const char *action_str = action_to_string(action);

    if (strcmp(command, "REPORT") == 0) {
        /* Still sent when ignore.conf matches, flagged "ignored":true:
         * subscribers decide for themselves (kistoryd logs everything, the
         * panel widget skips them). */
        int ignored = is_report_ignored(exe);
        active_seen(exe, action, pid);
        subscribers_broadcast("REPORT", action, pid, exe, ignored);
        if (ignored) {
            // Silencioso - não loga nada
            return;
        }
        log_msg("REPORT: %s is using %s (%d)", trim_exe_for_log(exe), action_str, action);
        time_t now = time(NULL);
        if (strcmp(exe, last_report.exe) == 0 && action == last_report.action &&
            now - last_report.last_time < REPORT_THROTTLE_S)
            return;

        snprintf(last_report.exe, sizeof(last_report.exe), "%s", exe);
        last_report.action = action;
        last_report.last_time = now;
        return;
    }

    log_msg("X server requested %s for %s", action_str, trim_exe_for_log(exe));
    subscribers_broadcast("REQUEST", action, pid, exe, 0);

    /* CLI global overrides: checked before perms.conf; deny wins over allow */
    if (action > 0 && action <= 16) {
        uint32_t bit = 1u << (action - 1);
        if (cli_deny_mask & bit) {
            log_filtered(2, "CLI override: DENY %s for %s", action_str, trim_exe_for_log(exe));
            send_permission(action, exe, pid, COMMAND_DENY);
            return;
        }
        if (cli_allow_mask & bit) {
            log_filtered(2, "CLI override: ALLOW %s for %s", action_str, trim_exe_for_log(exe));
            send_permission(action, exe, pid, COMMAND_ALLOW);
            return;
        }
    }

    /* Secure mode keeps no rules here, but the server only skips asking us
     * for a program it allows; a system DENY still reaches us. */
    if (secure_mode && system_rule_denies(exe, action)) {
        log_filtered(2, "System rule: DENY %s for %s", action_str, trim_exe_for_log(exe));
        send_permission(action, exe, pid, COMMAND_DENY);
        return;
    }

    char matched_rule[PATH_MAX] = {0};
    int pre = get_preconfig_rule(exe, action, matched_rule, sizeof(matched_rule));
    const char *rule_to_send = (matched_rule[0] != '\0') ? matched_rule : exe;
    
    if (pre == 1) {
        send_permission(action, rule_to_send, pid, COMMAND_ALLOW);
        return;
    }
    if (pre == 2) {
        send_permission(action, rule_to_send, pid, COMMAND_DENY);
        return;
    }
    
    if (is_seen(exe, action)) {
        log_filtered(3, "Duplicate request %s %s (%d) - ignoring", action_str, trim_exe_for_log(exe), pid);
        return;
    }

    int paused = 0;

    if (!no_pause_mode) {
        paused = (kill(pid, SIGSTOP) == 0);
        if (!paused && errno != ESRCH) {
            log_msg("Failed to pause PID %d: %s", pid, strerror(errno));
        }
    }

    add_seen(exe, action);

    pthread_mutex_lock(&queue_lock);
    if (alert_count < MAX_ALERTS) {
        alert_queue[alert_count].pid = pid;
        alert_queue[alert_count].action = action;
        strcpy(alert_queue[alert_count].exe, exe);
        strcpy(alert_queue[alert_count].time_str, get_current_time());
        alert_queue[alert_count].paused = paused;
        alert_count++;
        log_filtered(2, "Alert queued: %s : %s (%d pending)", trim_exe_for_log(exe), action_str, alert_count);
    } else {
        log_msg("Alert queue full! Dropping request for %s", trim_exe_for_log(exe));
    }
    pthread_mutex_unlock(&queue_lock);
}

/* PID paused (SIGSTOP) for the alert whose dialog is up right now, 0 if
 * none; guarded by queue_lock. A re-exec resumes it, see resume_paused_locked(). */
static pid_t inflight_paused_pid = 0;

void process_next_alert() {
    pthread_mutex_lock(&queue_lock);
    if (alert_count == 0) {
        pthread_mutex_unlock(&queue_lock);
        return;
    }

    struct Alert alert = alert_queue[0];

    for (int i = 0; i < alert_count - 1; i++) {
        alert_queue[i] = alert_queue[i + 1];
    }
    alert_count--;
    inflight_paused_pid = alert.paused ? alert.pid : 0;
    pthread_mutex_unlock(&queue_lock);

    int response;
    char conf_dir_probe[512];
    if (always_kill_mode || quiet_mode) {
        response = always_kill_mode ? -1 : 99;
    } else if (secure_mode && wait_for_secure_conf_dir(conf_dir_probe, sizeof(conf_dir_probe), 1500) &&
               system_rule_denies(alert.exe, alert.action)) {
        /* The directory was not known yet when the request arrived (right
         * after a guard restart), so handle_message() could not look. */
        log_filtered(2, "System rule: DENY %s for %s", action_to_string(alert.action), trim_exe_for_log(alert.exe));
        response = 99;
    } else {
        response = show_dialog(&alert);
    }

    /* Extract exe without args for non-exact rules */
    char exe_only[PATH_MAX];
    strncpy(exe_only, alert.exe, sizeof(exe_only) - 1);
    exe_only[sizeof(exe_only) - 1] = '\0';
    char *pipe_sep = strchr(exe_only, '|');
    if (pipe_sep) *pipe_sep = '\0';

    pthread_mutex_lock(&queue_lock);

    if (response == -1) {
        log_msg("ALWAYS-KILL mode: killing PID %d (%s)", alert.pid, trim_exe_for_log(exe_only));
        send_permission(alert.action, exe_only, alert.pid, COMMAND_DENY);
        kill(alert.pid, SIGKILL);
        remove_all_alerts_for_pid(alert.pid);
        remove_seen_for_exe(alert.exe, alert.action);
        inflight_paused_pid = 0;
        pthread_mutex_unlock(&queue_lock);
        return;
    }

    if (quiet_mode)
        log_msg("QUIET mode: denying this session for %s (PID %d)", trim_exe_for_log(exe_only), alert.pid);

    if (response == 0) {
        log_filtered(1, "ALLOWED: %s : %s", trim_exe_for_log(exe_only), action_to_string(alert.action));
        save_rule(exe_only, alert.action, 1);
        send_permission(alert.action, exe_only, alert.pid, COMMAND_ALLOW);
    } else if (response == 1) {
        log_filtered(1, "ALLOWED THIS SESSION: %s : %s", trim_exe_for_log(exe_only), action_to_string(alert.action));
        send_permission(alert.action, exe_only, alert.pid, COMMAND_ALLOW);
    } else if (response == 2) {
        log_filtered(1, "DENIED: %s : %s", trim_exe_for_log(exe_only), action_to_string(alert.action));
        save_denied_entry(exe_only, alert.action);
        send_permission(alert.action, exe_only, alert.pid, COMMAND_DENY);
    } else if (response == 3) {
        log_filtered(1, "ALL ALLOWED: %s", trim_exe_for_log(exe_only));
        save_rule(exe_only, -1, -1);
        send_permission(alert.action, exe_only, alert.pid, COMMAND_ALLOW_ALL);
        remove_all_alerts_for_pid(alert.pid);
    } else if (response == 4) {
        log_filtered(1, "ALLOWED EXACT: %s : %s", trim_exe_for_log(alert.exe), action_to_string(alert.action));
        save_rule(alert.exe, alert.action, 1);
        send_permission(alert.action, alert.exe, alert.pid, COMMAND_ALLOW);
    } else if (response == 5) {
        log_filtered(1, "ALLOWED EXACT THIS SESSION: %s : %s", trim_exe_for_log(alert.exe), action_to_string(alert.action));
        send_permission(alert.action, alert.exe, alert.pid, COMMAND_ALLOW);
    } else if (response == 6) {
        log_filtered(1, "ALL ALLOWED THIS SESSION: %s", trim_exe_for_log(exe_only));
        send_permission(alert.action, exe_only, alert.pid, COMMAND_ALLOW_ALL);
        remove_all_alerts_for_pid(alert.pid);
    } else if (response == 7) {
        log_filtered(1, "ALL ALLOWED EXACT: %s", trim_exe_for_log(alert.exe));
        save_rule(alert.exe, -1, -1);
        send_permission(alert.action, alert.exe, alert.pid, COMMAND_ALLOW_ALL);
        remove_all_alerts_for_pid(alert.pid);
    } else if (response == 8) {
        log_filtered(1, "ALL ALLOWED EXACT THIS SESSION: %s", trim_exe_for_log(alert.exe));
        send_permission(alert.action, alert.exe, alert.pid, COMMAND_ALLOW_ALL);
        remove_all_alerts_for_pid(alert.pid);
    } else {
        log_filtered(1, "DENIED THIS SESSION: %s : %s", trim_exe_for_log(exe_only), action_to_string(alert.action));
        send_permission(alert.action, exe_only, alert.pid, COMMAND_DENY);
    }

    if (alert.paused && !no_pause_mode)
        kill(alert.pid, SIGCONT);
    inflight_paused_pid = 0;

    remove_seen_for_exe(alert.exe, alert.action);

    if (kill(alert.pid, 0) != 0 && errno == ESRCH) {
        log_filtered(2, "PID %d no longer exists, discarding alert", alert.pid);
        remove_all_alerts_for_pid(alert.pid);
    }

    pthread_mutex_unlock(&queue_lock);
}

/* ====================== HEARTBEAT ====================== */

void send_heartbeat() {
    send_permission(0, "", getpid(), COMMAND_HEARTBEAT);
}

/* ====================== CONTROL SOCKET (local IPC, e.g. xisconf) ======================
 *
 * A separate SOCK_STREAM request/response channel, independent from the
 * xnotify.<display>.sock / xperms.<display>.sock pair used to talk to the X
 * server. Each connection sends one flat JSON request line and gets one JSON
 * response line back. Restricted to the owning user (mode 0600) since some
 * commands (ADD_RULE) can grant permissions that would otherwise require a
 * dialog confirmation.
 */

static void handle_control_message(const char *req, char *resp, size_t resp_sz) {
    char cmd[32] = {0};
    json_get_str(req, "cmd", cmd, sizeof(cmd));

    if (cmd[0] == '\0') {
        snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing cmd\"}\n");
        return;
    }

    if (strcasecmp(cmd, "PING") == 0) {
        snprintf(resp, resp_sz, "{\"ok\":true,\"pong\":true}\n");
        return;
    }

    if (strcasecmp(cmd, "GET_STATUS") == 0) {
        char na_buf[512];
        notify_mask_format(na_buf, sizeof(na_buf));
        snprintf(resp, resp_sz,
            "{\"ok\":true,\"version\":\"%s\",\"display\":%d,"
            "\"no_pause\":%d,\"quiet\":%d,\"always_kill\":%d,\"log_level\":%d,\"secure_mode\":%d,"
            "\"xnotify_enabled\":%d,\"notify_actions\":\"%s\",\"permissive\":%d,\"permissive_on_start\":%d}\n",
            XISGUARD_VERSION, display, no_pause_mode, quiet_mode, always_kill_mode, log_level, secure_mode,
            !server_disabled, na_buf, permissive_mode, permissive_on_start);
        return;
    }

    if (strcasecmp(cmd, "SET_STATUS") == 0) {
        int v;
        int secure_mode_changed = 0;
        if (json_get_int(req, "no_pause", &v))     no_pause_mode = v ? 1 : 0;
        if (json_get_int(req, "quiet", &v))         quiet_mode = v ? 1 : 0;
        if (json_get_int(req, "always_kill", &v))   always_kill_mode = v ? 1 : 0;
        if (json_get_int(req, "log_level", &v) && v >= 0 && v <= 4) log_level = v;
        char na[512];
        if (json_get_str(req, "notify_actions", na, sizeof(na)))
            notify_mask = notify_mask_parse(na);
        if (json_get_int(req, "secure_mode", &v) && (v ? 1 : 0) != secure_mode) {
            secure_mode = v ? 1 : 0;
            secure_mode_changed = 1;
        }
        save_xnotify_conf();
        /* Re-derives the in-memory rule set for the new mode: turning
         * secure mode on drops perms.conf's user rules (see its own
         * early-return branch above), turning it off reloads them. */
        if (secure_mode_changed) {
            load_user_config();
            /* The server still holds the old mode's rules: drop them and
             * send the ones that apply now. */
            request_server_reload();
        }
        log_msg("Control: SET_STATUS no_pause=%d quiet=%d always_kill=%d log_level=%d secure_mode=%d",
                no_pause_mode, quiet_mode, always_kill_mode, log_level, secure_mode);
        snprintf(resp, resp_sz,
            "{\"ok\":true,\"no_pause\":%d,\"quiet\":%d,\"always_kill\":%d,\"log_level\":%d,\"secure_mode\":%d}\n",
            no_pause_mode, quiet_mode, always_kill_mode, log_level, secure_mode);
        return;
    }

    /* Runtime on/off of the server's Xnotify enforcement. We only forward the
     * command: the server authenticates us, applies it and answers with a STATUS
     * datagram, which (not this reply) updates the "disabled" notice. Note that
     * a DISABLE only lasts while this guard keeps sending heartbeats. */
    if (strcasecmp(cmd, "DISABLE") == 0 || strcasecmp(cmd, "ENABLE") == 0) {
        int disable = (strcasecmp(cmd, "DISABLE") == 0);
        log_msg("Control: %s Xnotify requested", disable ? "DISABLE" : "ENABLE");
        send_raw_command(disable ? "{\"command\":\"DISABLE\"}"
                                 : "{\"command\":\"ENABLE\"}");
        snprintf(resp, resp_sz, "{\"ok\":true,\"sent\":true}\n");
        return;
    }

    /* Runtime permissive mode: ON sends ALLOW_ACTION for every action (global
     * mask, beats every rule; REPORTs keep flowing), OFF asks the server to
     * RELOAD, which zeroes the mask and restores the static rules. Clients
     * that already cached a grant keep it until they reconnect. */
    if (strcasecmp(cmd, "SET_PERMISSIVE") == 0) {
        int v;
        if (!json_get_int(req, "enabled", &v)) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing enabled\"}\n");
            return;
        }
        v = v ? 1 : 0;
        int persist;
        if (json_get_int(req, "persist", &persist) && persist && v != permissive_on_start) {
            permissive_on_start = v;
            save_xnotify_conf();
        }
        if (v != permissive_mode) {
            permissive_mode = v;
            if (v)
                permissive_reapply();
            else {
                send_raw_command("{\"command\":\"RELOAD\"}");
                send_all_permissions_to_xserver();   /* the reload dropped the user rules too */
            }
            log_msg("Control: permissive mode %s", v ? "ON" : "OFF");
            subscribers_broadcast_permissive(v);
        }
        snprintf(resp, resp_sz, "{\"ok\":true,\"permissive\":%d}\n", permissive_mode);
        return;
    }

    if (strcasecmp(cmd, "LIST_RULES") == 0) {
        size_t off = 0;
        char file_esc[1024];
        json_escape_str(file_esc, sizeof(file_esc), perms_file);
        /* "file" is where these rules are read from and saved to, "order" is
         * each rule's 1-based position there. */
        off += snprintf(resp + off, resp_sz - off, "{\"ok\":true,\"file\":\"%s\",\"enabled\":%s,\"rules\":[",
                        file_esc, secure_mode ? "false" : "true");
        pthread_mutex_lock(&ignored_lock);
        for (int i = 0; i < ignored_count && off < resp_sz; i++) {
            char pat_esc[PATH_MAX * 2];
            json_escape_str(pat_esc, sizeof(pat_esc), ignored_cmds[i].exe_pattern);
            const char *act = ignored_cmds[i].action[0] ? ignored_cmds[i].action : "ALL";
            off += snprintf(resp + off, resp_sz - off,
                "%s{\"order\":%d,\"type\":\"%s\",\"action\":\"%s\",\"pattern\":\"%s\"}",
                i == 0 ? "" : ",", i + 1,
                ignored_cmds[i].rule_type == 0 ? "ALLOW" : "DENY",
                act, pat_esc);
        }
        pthread_mutex_unlock(&ignored_lock);
        if (off < resp_sz) off += snprintf(resp + off, resp_sz - off, "]}\n");
        return;
    }

    if (strcasecmp(cmd, "ADD_RULE") == 0) {
        char action_str[64] = {0}, pattern[PATH_MAX] = {0}, type_str[16] = {0};
        json_get_str(req, "action", action_str, sizeof(action_str));
        json_get_str(req, "pattern", pattern, sizeof(pattern));
        json_get_str(req, "type", type_str, sizeof(type_str));

        if (pattern[0] == '\0' || type_str[0] == '\0') {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing pattern or type\"}\n");
            return;
        }
        int action_id = action_str[0] ? string_to_action(action_str) : -1;
        if (action_id == 0) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"unknown action\"}\n");
            return;
        }
        int is_allow = (strcasecmp(type_str, "ALLOW") == 0);
        save_rule(pattern, action_id, is_allow);
        send_all_permissions_to_xserver();
        log_msg("Control: ADD_RULE %s %s %s", is_allow ? "ALLOW" : "DENY",
                action_to_string(action_id), trim_exe_for_log(pattern));
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
        return;
    }

    if (strcasecmp(cmd, "REMOVE_RULE") == 0) {
        char action_str[64] = {0}, pattern[PATH_MAX] = {0}, type_str[16] = {0};
        json_get_str(req, "action", action_str, sizeof(action_str));
        json_get_str(req, "pattern", pattern, sizeof(pattern));
        json_get_str(req, "type", type_str, sizeof(type_str));

        if (pattern[0] == '\0' || type_str[0] == '\0') {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing pattern or type\"}\n");
            return;
        }
        int rule_type = (strcasecmp(type_str, "DENY") == 0) ? 1 : 0;
        int removed = remove_rule(pattern, action_str, rule_type);
        if (removed)
            request_server_reload();   /* the server still has the rule until it is dropped */
        snprintf(resp, resp_sz, "{\"ok\":%s}\n", removed ? "true" : "false");
        return;
    }

    /* ignore.conf: programs whose REPORTs are not shown. One pattern per
     * line (same glob matching as rules); comments and blank lines are kept
     * when rewriting. */
    if (strcasecmp(cmd, "LIST_IGNORED") == 0) {
        size_t off = 0;
        off += snprintf(resp + off, resp_sz - off, "{\"ok\":true,\"ignored\":[");
        pthread_mutex_lock(&report_ignore_lock);
        for (int i = 0; i < ignored_reports_count && off < resp_sz; i++) {
            char esc[PATH_MAX * 2];
            json_escape_str(esc, sizeof(esc), ignored_reports[i].exe);
            off += snprintf(resp + off, resp_sz - off, "%s\"%s\"", i == 0 ? "" : ",", esc);
        }
        pthread_mutex_unlock(&report_ignore_lock);
        off += snprintf(resp + off, resp_sz - off, "],\"builtin\":[");
        for (int i = 0; ignored_builtin[i] && off < resp_sz; i++)
            off += snprintf(resp + off, resp_sz - off, "%s\"%s\"", i ? "," : "", ignored_builtin[i]);
        if (off < resp_sz) off += snprintf(resp + off, resp_sz - off, "]}\n");
        return;
    }

    /* The programs that reported using something lately, newest first, with
     * whether ignore.conf hides them and whether a DENY rule already covers
     * that action. Memory only: gone when the guard restarts. */
    if (strcasecmp(cmd, "LIST_ACTIVE") == 0) {
        size_t off = 0;
        off += snprintf(resp + off, resp_sz - off, "{\"ok\":true,\"now\":%ld,\"active\":[", (long)time(NULL));
        pthread_mutex_lock(&active_lock);
        int order[ACTIVE_MAX], n = 0;
        for (int i = 0; i < ACTIVE_MAX; i++)
            if (active_tab[i].exe[0] && (notify_mask & (1u << (active_tab[i].action - 1))))
                order[n++] = i;
        for (int i = 1; i < n; i++) { /* insertion sort, newest first */
            int v = order[i], j = i - 1;
            while (j >= 0 && active_tab[order[j]].ts < active_tab[v].ts) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = v;
        }
        for (int k = 0; k < n && off < resp_sz; k++) {
            int i = order[k];
            char esc[PATH_MAX * 2];
            json_escape_str(esc, sizeof(esc), active_tab[i].exe);
            off += snprintf(resp + off, resp_sz - off,
                "%s{\"exe\":\"%s\",\"action\":\"%s\",\"pid\":%d,\"ts\":%ld,\"ignored\":%s,\"denied\":%s}",
                k == 0 ? "" : ",", esc, action_to_string(active_tab[i].action), active_tab[i].pid, (long)active_tab[i].ts,
                is_report_ignored(active_tab[i].exe) ? "true" : "false",
                is_ignored(active_tab[i].exe, active_tab[i].action) == 2 ? "true" : "false");
        }
        pthread_mutex_unlock(&active_lock);
        if (off < resp_sz) off += snprintf(resp + off, resp_sz - off, "]}\n");
        return;
    }

    /* Takes an action away from a program: drops its rules for that action
     * and puts a DENY first, so no earlier ALLOW (even "ALLOW ... *") still
     * wins. Permanent, like any rule: visible and removable in the rules
     * list. `pattern` is the program path without arguments, which matches
     * it whatever arguments it runs with. */
    if (strcasecmp(cmd, "REVOKE_ACCESS") == 0) {
        char action_str[64] = {0}, pattern[PATH_MAX] = {0};
        json_get_str(req, "action", action_str, sizeof(action_str));
        json_get_str(req, "pattern", pattern, sizeof(pattern));
        int action_id = string_to_action(action_str);
        if (pattern[0] == '\0' || action_id <= 0 || strchr(pattern, '|')) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"need a program path and an action\"}\n");
            return;
        }
        if (secure_mode) {
            secure_save_rule(pattern, action_id, 0);
        } else {
            pthread_mutex_lock(&ignored_lock);
            const char *an = action_to_string(action_id);
            for (int i = 0; i < ignored_count; ) {
                if (strcmp(ignored_cmds[i].exe_pattern, pattern) == 0 && strcasecmp(ignored_cmds[i].action, an) == 0) {
                    for (int j = i; j < ignored_count - 1; j++)
                        ignored_cmds[j] = ignored_cmds[j + 1];
                    ignored_count--;
                } else {
                    i++;
                }
            }
            if (ignored_count < MAX_IGNORED) {
                for (int j = ignored_count; j > 0; j--)
                    ignored_cmds[j] = ignored_cmds[j - 1];
                memset(&ignored_cmds[0], 0, sizeof(ignored_cmds[0]));
                snprintf(ignored_cmds[0].exe_pattern, sizeof(ignored_cmds[0].exe_pattern), "%s", pattern);
                snprintf(ignored_cmds[0].action, sizeof(ignored_cmds[0].action), "%s", an);
                ignored_cmds[0].rule_type = 1;
                ignored_count++;
            }
            rewrite_perms_file_locked();
            last_config_mtime = time(NULL);
            pthread_mutex_unlock(&ignored_lock);
        }
        send_all_permissions_to_xserver();
        log_msg("Control: REVOKE_ACCESS %s %s", action_str, trim_exe_for_log(pattern));
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
        return;
    }

    if (strcasecmp(cmd, "ADD_IGNORED") == 0 || strcasecmp(cmd, "REMOVE_IGNORED") == 0) {
        int add = (strcasecmp(cmd, "ADD_IGNORED") == 0);
        char pattern[PATH_MAX] = {0};
        json_get_str(req, "pattern", pattern, sizeof(pattern));
        char *pat = trim(pattern);
        if (*pat == '\0' || strchr(pat, '\n')) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"missing pattern\"}\n");
            return;
        }
        int changed = ignore_file_edit(add, pat);
        if (changed < 0) {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"cannot write ignore.conf\"}\n");
            return;
        }
        load_ignore_reports();
        snprintf(resp, resp_sz, "{\"ok\":true,\"changed\":%s}\n", changed ? "true" : "false");
        return;
    }

    if (strcasecmp(cmd, "RELOAD") == 0) {
        /* The server re-reads its xnotify.conf.d first (it also drops what we
         * had sent), then our own rules go back in. */
        load_user_config();
        load_ignore_reports();
        request_server_reload();   /* RELOAD, then our rules */
        snprintf(resp, resp_sz, "{\"ok\":true}\n");
        return;
    }

    /* Lets a control-socket client (kiconf's Permissoes tab) find and
     * read the system-wide rules for itself -- SYSCONFDIR is a compile-
     * time constant of the X server, not of xisguard or kiconf, so this
     * just forwards wait_for_secure_conf_dir()'s own query to whichever
     * client asks, the same one secure_save_rule() uses to learn where
     * to write. Read-only: only polkit (secure_save_rule()) writes here. */
    if (strcasecmp(cmd, "GET_SYSTEM_RULES_PATH") == 0) {
        char dir[512];
        if (wait_for_secure_conf_dir(dir, sizeof(dir), 2000)) {
            char edir[1024];
            json_escape_str(edir, sizeof(edir), dir);
            char efile[1024];
            json_escape_str(efile, sizeof(efile), secure_conf_file);
            snprintf(resp, resp_sz, "{\"ok\":true,\"dir\":\"%s\",\"file\":\"%s\"}\n", edir, efile);
        } else {
            snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"timeout\"}\n");
        }
        return;
    }

    snprintf(resp, resp_sz, "{\"ok\":false,\"error\":\"unknown command\"}\n");
}

/* ---- SUBSCRIBE: long-lived event stream on the same control socket ----
 *
 * `{"cmd":"SUBSCRIBE","actions":"SCREEN,RECORD"}` (actions optional, all
 * when absent) keeps the connection open instead of closing it after the
 * reply; from then on every REPORT/REQUEST from the X server is written to
 * it as one line: {"ev":"REPORT","action":"RECORD","pid":N,"exe":"...","ts":N}.
 * Writes are non-blocking; a subscriber that is gone or can't keep up is
 * dropped rather than stalling the X server message path. */

static struct { int fd; uint32_t mask; } subscribers[MAX_SUBSCRIBERS];
static int subscriber_count = 0;
static pthread_mutex_t subscribers_lock = PTHREAD_MUTEX_INITIALIZER;

static void subscriber_remove_locked(int i) {
    close(subscribers[i].fd);
    subscribers[i] = subscribers[--subscriber_count];
}

static void subscribers_broadcast(const char *ev, int action, pid_t pid, const char *exe, int ignored) {
    if (action <= 0 || action > 16)
        return;
    pthread_mutex_lock(&subscribers_lock);
    if (subscriber_count == 0) {
        pthread_mutex_unlock(&subscribers_lock);
        return;
    }
    char exe_esc[PATH_MAX * 2];
    json_escape_str(exe_esc, sizeof(exe_esc), exe);
    char line[PATH_MAX * 2 + 128];
    int muted = !(notify_mask & (1u << (action - 1)));
    int len = snprintf(line, sizeof(line),
                       "{\"ev\":\"%s\",\"action\":\"%s\",\"pid\":%d,\"exe\":\"%s\",\"ts\":%ld%s%s}\n",
                       ev, action_to_string(action), (int)pid, exe_esc, (long)time(NULL),
                       ignored ? ",\"ignored\":true" : "", muted ? ",\"muted\":true" : "");
    if (len >= (int)sizeof(line))
        len = (int)sizeof(line) - 1;
    uint32_t bit = 1u << (action - 1);
    for (int i = 0; i < subscriber_count; ) {
        if (subscribers[i].mask && !(subscribers[i].mask & bit)) {
            i++;
            continue;
        }
        ssize_t w = send(subscribers[i].fd, line, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (w != len) {
            log_filtered(2, "Subscriber fd %d dropped (%s)", subscribers[i].fd,
                         w < 0 ? strerror(errno) : "short write");
            subscriber_remove_locked(i);
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&subscribers_lock);
}

/* {"ev":"STATE","enabled":bool,"ts":N} to every subscriber, whatever its
 * action mask: the protection state is not an action report. */
static void subscribers_broadcast_state(int enabled) {
    char line[96];
    int len = snprintf(line, sizeof(line), "{\"ev\":\"STATE\",\"enabled\":%s,\"ts\":%ld}\n",
                       enabled ? "true" : "false", (long)time(NULL));
    pthread_mutex_lock(&subscribers_lock);
    for (int i = 0; i < subscriber_count; ) {
        if (send(subscribers[i].fd, line, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL) != len) {
            subscriber_remove_locked(i);
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&subscribers_lock);
}

/* {"ev":"PERMISSIVE","enabled":bool,"ts":N} to every subscriber, whatever its
 * action mask, like STATE. */
static void subscribers_broadcast_permissive(int enabled) {
    char line[96];
    int len = snprintf(line, sizeof(line), "{\"ev\":\"PERMISSIVE\",\"enabled\":%s,\"ts\":%ld}\n",
                       enabled ? "true" : "false", (long)time(NULL));
    pthread_mutex_lock(&subscribers_lock);
    for (int i = 0; i < subscriber_count; ) {
        if (send(subscribers[i].fd, line, (size_t)len, MSG_DONTWAIT | MSG_NOSIGNAL) != len) {
            subscriber_remove_locked(i);
            continue;
        }
        i++;
    }
    pthread_mutex_unlock(&subscribers_lock);
}

/* Peer must be this same uid and its executable's basename must be in
 * subscribers_allow. Fills exe_out with the basename for logging. */
static int subscriber_peer_allowed(int conn, char *exe_out, size_t exe_sz) {
    struct ucred cred;
    socklen_t len = sizeof(cred);
    exe_out[0] = '\0';
    if (getsockopt(conn, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return 0;
    if (cred.uid != getuid())
        return 0;

    char link[64], path[PATH_MAX];
    snprintf(link, sizeof(link), "/proc/%d/exe", (int)cred.pid);
    ssize_t n = readlink(link, path, sizeof(path) - 1);
    if (n <= 0)
        return 0;
    path[n] = '\0';
    char *del = strstr(path, " (deleted)");
    if (del) *del = '\0';
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t bl = strlen(base);
    if (bl >= exe_sz) bl = exe_sz - 1;
    memcpy(exe_out, base, bl);
    exe_out[bl] = '\0';

    for (int i = 0; subscribers_builtin[i]; i++) {
        if (strcmp(subscribers_builtin[i], base) == 0)
            return 1;
    }
    char list[sizeof(subscribers_allow)];
    snprintf(list, sizeof(list), "%s", subscribers_allow);
    char *save = NULL;
    for (char *tok = strtok_r(list, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (strcmp(trim(tok), base) == 0)
            return 1;
    }
    return 0;
}

/* 0: not a SUBSCRIBE request; 1: `conn` kept as a subscriber (caller must
 * not close it); -1: SUBSCRIBE refused, error reply already written. */
static int try_subscribe(int conn, const char *req) {
    char cmd[32] = {0};
    json_get_str(req, "cmd", cmd, sizeof(cmd));
    if (strcasecmp(cmd, "SUBSCRIBE") != 0)
        return 0;

    char exe[256];
    if (!subscriber_peer_allowed(conn, exe, sizeof(exe))) {
        log_msg("Control: SUBSCRIBE refused for %s", exe[0] ? exe : "?");
        const char *no = "{\"ok\":false,\"error\":\"not allowed\"}\n";
        (void)!write(conn, no, strlen(no));
        return -1;
    }

    uint32_t mask = 0;
    char actions[256] = {0};
    if (json_get_str(req, "actions", actions, sizeof(actions)) && actions[0]) {
        char *save = NULL;
        for (char *tok = strtok_r(actions, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
            int a = string_to_action(trim(tok));
            if (a > 0 && a <= 16)
                mask |= 1u << (a - 1);
        }
    }

    pthread_mutex_lock(&subscribers_lock);
    if (subscriber_count >= MAX_SUBSCRIBERS) {
        pthread_mutex_unlock(&subscribers_lock);
        const char *full = "{\"ok\":false,\"error\":\"too many subscribers\"}\n";
        (void)!write(conn, full, strlen(full));
        return -1;
    }
    /* The reply carries the current state, so a subscriber needs no separate
     * query and cannot miss a change between the two. */
    char ok[128];
    int ok_len = snprintf(ok, sizeof(ok), "{\"ok\":true,\"subscribed\":true,\"enabled\":%s,\"permissive\":%s}\n",
                          server_disabled ? "false" : "true", permissive_mode ? "true" : "false");
    (void)!write(conn, ok, (size_t)ok_len);
    fcntl(conn, F_SETFL, fcntl(conn, F_GETFL) | O_NONBLOCK);
    subscribers[subscriber_count].fd = conn;
    subscribers[subscriber_count].mask = mask;
    subscriber_count++;
    pthread_mutex_unlock(&subscribers_lock);
    log_msg("Control: %s subscribed to events", exe);
    return 1;
}

void* control_loop(void *arg) {
    (void)arg;
    log_msg("Control thread started - listening on %s", CTL_SOCKET_PATH_BUF);

    while (!should_exit) {
        /* Subscriber fds are polled only to notice them hanging up: they
         * never send anything after SUBSCRIBE. */
        struct pollfd pfds[1 + MAX_SUBSCRIBERS];
        int sub_fds[MAX_SUBSCRIBERS];
        int nsub;
        pfds[0].fd = ctl_fd;
        pfds[0].events = POLLIN;
        pthread_mutex_lock(&subscribers_lock);
        nsub = subscriber_count;
        for (int i = 0; i < nsub; i++) {
            sub_fds[i] = subscribers[i].fd;
            pfds[1 + i].fd = sub_fds[i];
            pfds[1 + i].events = POLLIN;
        }
        pthread_mutex_unlock(&subscribers_lock);

        int r = poll(pfds, (nfds_t)(1 + nsub), 500);
        if (r <= 0)
            continue;

        for (int i = 0; i < nsub; i++) {
            if (!(pfds[1 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            char junk[64];
            ssize_t n = recv(sub_fds[i], junk, sizeof(junk), MSG_DONTWAIT);
            if (n > 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
                continue;
            pthread_mutex_lock(&subscribers_lock);
            for (int j = 0; j < subscriber_count; j++) {
                if (subscribers[j].fd == sub_fds[i]) {
                    log_filtered(2, "Subscriber fd %d disconnected", sub_fds[i]);
                    subscriber_remove_locked(j);
                    break;
                }
            }
            pthread_mutex_unlock(&subscribers_lock);
        }

        if (!(pfds[0].revents & POLLIN))
            continue;

        int conn = accept4(ctl_fd, NULL, NULL, SOCK_CLOEXEC);
        if (conn < 0)
            continue;

        static char req[CTL_BUF_SIZE];
        static char resp[CTL_BUF_SIZE];
        ssize_t n = read(conn, req, sizeof(req) - 1);
        if (n > 0) {
            req[n] = '\0';
            int sub = try_subscribe(conn, req);
            if (sub == 1)
                continue;
            if (sub == 0) {
                handle_control_message(req, resp, sizeof(resp));
                (void)!write(conn, resp, strlen(resp));
            }
        }
        close(conn);
    }
    return NULL;
}

/* ====================== THREADS ====================== */

void* alert_processor_loop(void *arg) {
    (void)arg;
    while (!should_exit) {
        if (alert_count > 0)
            process_next_alert();
        usleep(150000);
    }
    return NULL;
}

/* Path of our own binary, without the " (deleted)" suffix the kernel adds
 * once it was unlinked. Returns 1 if that suffix was there. */
static int self_exe_path(char *out, size_t out_sz) {
    ssize_t n = readlink("/proc/self/exe", out, out_sz - 1);
    if (n <= 0) {
        out[0] = '\0';
        return 0;
    }
    out[n] = '\0';
    static const char suffix[] = " (deleted)";
    size_t sl = sizeof(suffix) - 1;
    if ((size_t)n > sl && strcmp(out + n - sl, suffix) == 0) {
        out[n - sl] = '\0';
        return 1;
    }
    return 0;
}

/* `make install` / a package upgrade replaces our binary. The server
 * identifies us by /proc/<pid>/exe, which now reads "... (deleted)", so it
 * rejects every heartbeat and, 5 s later, resets itself to the system rules
 * (dropping permissive mode and our rules). Re-exec the new binary once it
 * is in place: same PID, fresh identity, full resync at startup. */
static void check_self_replaced(void) {
    static off_t last_size = -1;
    static time_t last_mtime = 0;
    char path[PATH_MAX];
    struct stat st;

    if (reexec_requested || !self_exe_path(path, sizeof(path)))
        return;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) {
        last_size = -1;   /* not installed (yet) */
        return;
    }
    /* Wait one more tick with size and mtime unchanged, in case the
     * installer is still copying it. */
    if (st.st_size != last_size || st.st_mtime != last_mtime) {
        last_size = st.st_size;
        last_mtime = st.st_mtime;
        return;
    }
    log_msg("%s was replaced on disk - restarting into the new binary", path);
    reexec_requested = 1;
}

void* file_monitor_loop(void *arg) {
    (void)arg;
    log_msg("File monitor thread started");
    while (!should_exit) {
        file_has_changed();
        check_xnotify_conf_changed();
        check_self_replaced();
        sleep(1);
    }
    return NULL;
}

/* Watches the X server itself. The xnotify/xperms channels are datagram
 * sockets, so they never report that the server went away. A real
 * XOpenDisplay() is the probe: against a merely slow or overloaded server it
 * blocks until the server answers (the connection waits in the listen
 * backlog) and then succeeds; it only fails when the socket is gone or
 * refuses connections. Only XDISPLAY_MAX_FAILS such failures in a row, spaced
 * XDISPLAY_RETRY_SECS apart, make us give up and exit. If the server comes
 * back before that, the rules are pushed to it again. */
#define XDISPLAY_PROBE_SECS  2
#define XDISPLAY_RETRY_SECS  3
#define XDISPLAY_MAX_FAILS   5

static int sleep_unless_exit(int secs) {
    for (int i = 0; i < secs * 10 && !should_exit; i++)
        usleep(100000);
    return !should_exit;
}

void* xdisplay_watch_loop(void *arg) {
    (void)arg;
    char display_str[32];
    snprintf(display_str, sizeof(display_str), ":%d", display);
    int fails = 0;

    while (sleep_unless_exit(fails ? XDISPLAY_RETRY_SECS : XDISPLAY_PROBE_SECS)) {
        Display *dpy = XOpenDisplay(display_str);
        if (dpy) {
            XCloseDisplay(dpy);
            if (fails) {
                log_msg("X server on %s is back - re-sending rules", display_str);
                send_raw_command("{\"command\":\"RELOAD\"}");
                send_all_permissions_to_xserver();
                send_heartbeat();
            }
            fails = 0;
            continue;
        }
        fails++;
        log_msg("Cannot reach X server on %s (attempt %d/%d)", display_str, fails, XDISPLAY_MAX_FAILS);
        if (fails >= XDISPLAY_MAX_FAILS) {
            log_msg("X server on %s is gone. Exiting.", display_str);
            should_exit = 1;
            break;
        }
    }
    return NULL;
}

/* ====================== LOCK & CLEANUP ====================== */

/* Records the inode of a socket path we just bound (see unlink_if_ours()). */
static ino_t path_ino(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 ? st.st_ino : 0;
}

/* Unlinks a socket path only if it is still the one we bound: an instance
 * started after us may own it now, and removing it would cut that one off. */
static void unlink_if_ours(const char *path, ino_t ino) {
    if (ino != 0 && path_ino(path) == ino)
        unlink(path);
}

/* PID of the instance listening on the lock socket, 0 if there is none
 * (no socket file, or a stale one left by a crash). */
static pid_t lock_holder_pid(void) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LOCK_SOCKET_PATH_BUF);
    pid_t pid = 0;
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        struct ucred cred;
        socklen_t len = sizeof(cred);
        pid = (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 && cred.pid > 0) ? cred.pid : -1;
    }
    close(fd);
    return pid;
}

/* The lock is a listening socket: a live holder accepts connections, a dead
 * one leaves a file nobody listens on. Only that stale file is unlinked;
 * with `replace`, a live holder is asked to quit (SIGTERM) first. */
int acquire_lock(int replace) {
    pid_t holder = lock_holder_pid();
    if (holder != 0) {
        if (!replace) {
            log_msg("XisGuard is already running (PID %d). Use --replace to take over, "
                    "or send it SIGHUP to make it restart and resync. Exiting.", (int)holder);
            return 0;
        }
        if (holder < 0) {
            log_msg("XisGuard is already running (unknown PID), cannot replace it. Exiting.");
            return 0;
        }
        log_msg("Replacing running XisGuard (PID %d)", (int)holder);
        kill(holder, SIGTERM);
        for (int i = 0; i < 50 && lock_holder_pid() != 0; i++)
            usleep(100000);
        if (lock_holder_pid() != 0) {
            log_msg("PID %d did not exit within 5 s. Exiting.", (int)holder);
            return 0;
        }
    }

    lock_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (lock_fd < 0) {
        log_msg("Failed to create lock socket: %s", strerror(errno));
        return 0;
    }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", LOCK_SOCKET_PATH_BUF);

    unlink(LOCK_SOCKET_PATH_BUF);   /* stale: nobody answered above */

    if (bind(lock_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        log_msg("Failed to bind lock socket: %s", strerror(errno));
        close(lock_fd);
        return 0;
    }
    lock_path_ino = path_ino(LOCK_SOCKET_PATH_BUF);

    chmod(LOCK_SOCKET_PATH_BUF, 0666);
    listen(lock_fd, 1);
    return 1;
}

void release_lock() {
    if (lock_fd >= 0) {
        close(lock_fd);
        unlink_if_ours(LOCK_SOCKET_PATH_BUF, lock_path_ino);
    }
}

void cleanup(int sig) {
    log_msg("Shutting down (signal %d)...", sig);
    should_exit = 1;

    /* If we are leaving while Xnotify is disabled, re-enable it right away
     * instead of waiting up to ~5 s for the server's heartbeat timeout. */
    if (server_disabled)
        send_raw_command("{\"command\":\"ENABLE\"}");
    /* The global allow-all mask lives in the server and would outlive us. */
    if (permissive_mode)
        send_raw_command("{\"command\":\"RELOAD\"}");
    notice_request_close();
    if (notice_thread_valid)
        pthread_join(notice_thread, NULL);

    pthread_join(file_monitor_thread, NULL);
    pthread_join(processor_thread, NULL);
    pthread_join(control_thread, NULL);

    if (server_fd >= 0) {
        close(server_fd);
        unlink_if_ours(SOCKET_PATH_BUF, sock_path_ino);
    }

    if (ctl_fd >= 0) {
        close(ctl_fd);
        unlink_if_ours(CTL_SOCKET_PATH_BUF, ctl_path_ino);
    }

    release_lock();
    exit(0);
}

/* Restarts this process in place (same PID, same argv) from the binary now
 * at our path: after an upgrade (check_self_replaced()) or on SIGHUP. The
 * new image resyncs everything at startup, like a fresh start. Nothing is
 * sent to the server on the way out, so permissive mode and the rules stay
 * in effect across the gap. The processor thread may be stuck in a dialog,
 * so it is not joined: exec ends every thread, and the PIDs we paused are
 * resumed here instead of waiting for an answer that will never come. */
static void reexec_self(void) {
    char path[PATH_MAX];
    self_exe_path(path, sizeof(path));
    if (path[0] == '\0' || access(path, X_OK) != 0) {
        log_msg("Cannot restart: %s is not executable, keeping the current process", path[0] ? path : "/proc/self/exe");
        reexec_requested = 0;
        return;
    }
    log_msg("Restarting (%s)...", path);

    notice_request_close();
    if (notice_thread_valid)
        pthread_join(notice_thread, NULL);

    pthread_mutex_lock(&queue_lock);
    if (!no_pause_mode) {
        if (inflight_paused_pid > 0)
            kill(inflight_paused_pid, SIGCONT);
        for (int i = 0; i < alert_count; i++)
            if (alert_queue[i].paused)
                kill(alert_queue[i].pid, SIGCONT);
    }
    pthread_mutex_unlock(&queue_lock);

    /* Every fd we own is CLOEXEC (subscribers included), so control clients
     * see the hangup and reconnect; the new image rebinds the socket paths. */
    char *const *argv = saved_argv;
    char *fallback[] = { (char *)"xisguard", NULL };
    execv(path, argv ? argv : fallback);
    log_msg("Restart failed: execv(%s): %s", path, strerror(errno));
    reexec_requested = 0;
}

static void on_sighup(int sig) {
    (void)sig;
    reexec_requested = 1;
}

/* ====================== XNOTIFY EXTENSION CHECK ====================== */

static int check_xnotify_extension(void) {
    char display_str[32];
    snprintf(display_str, sizeof(display_str), ":%d", display);

    Display *dpy = XOpenDisplay(display_str);
    if (!dpy) {
        log_filtered(2, "Could not open display %s to check Xnotify extension", display_str);
        return 0;
    }

    int opcode, event_base, error_base;
    int found = XQueryExtension(dpy, "XNOTIFY", &opcode, &event_base, &error_base);
    XCloseDisplay(dpy);

    if (found)
        log_msg("Xnotify extension detected on display %s (opcode %d)", display_str, opcode);
    else
        log_msg("Warning: Xnotify extension not found on display %s - continuing via socket only", display_str);

    return found;
}

/* ====================== MAIN ====================== */

int main(int argc, char *argv[]) {
    xis_i18n_init("xisguard");

    saved_argv = argv;
    signal(SIGINT, cleanup);
    signal(SIGTERM, cleanup);
    signal(SIGHUP, on_sighup);
    signal(SIGPIPE, SIG_IGN);

    /* --version / --help don't need anything else — handle them first */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("xisguard %s\n", XISGUARD_VERSION);
            return 0;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf(_("Usage: %s [options]\n"), argv[0]);
            printf("%s\n", _("Options:"));
            printf("  --no-pause / --notify-only     %s\n", _("Do not send SIGSTOP/SIGCONT"));
            printf("  --quiet / --no-zenity          %s\n", _("No dialog prompts (xisserve/zenity); deny all unauthorized processes for the current session"));
            printf("  --always-kill                  %s\n", _("Kill (SIGKILL) all unauthorized processes immediately"));
            printf("  --secure-mode                  %s\n", _("Disable perms.conf; permanent Allow/Trust choices are written"));
            printf("                                   %s\n", _("to the X server's own xnotify.conf.d instead (via polkit)"));
            printf("  --conf <dir> or --conf=<dir>   %s\n", _("Base config directory (default: ~/.config/xisguard)"));
            printf("  --log-level N                  %s\n", _("Verbosity level (0-4)"));
            printf("  --allow ACTION                 %s\n", _("Always allow ACTION for any program (overrides perms.conf)"));
            printf("  --deny  ACTION                 %s\n", _("Always deny  ACTION for any program (overrides perms.conf)"));
            printf("                                 %s\n", _("Can be repeated. ACTION: ATTACH SELECTION COMPOSITE SCREEN"));
            printf("                                   %s\n", _("RECORD CURSOR INPUT_GRAB INPUT_INJECT HOTKEY INPUT"));
            printf("                                   %s\n", _("MANAGE GRAB_OVERRIDE WARP FOCUS RANDR OVERLAY ALL"));
            printf("  --replace                      %s\n", _("Stop the running instance (SIGTERM) and take its place"));
            printf("  --version / -V                 %s\n", _("Print version and exit"));
            printf("  --help / -h                    %s\n", _("Show this help"));
            return 0;
        }
    }

    /* Quick scan: resolve --conf so we can set up file paths before loading xnotify.conf */
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--conf=", 7) == 0) {
            strncpy(config_dir, argv[i] + 7, sizeof(config_dir) - 1);
            config_dir[sizeof(config_dir)-1] = '\0';
        } else if (strcmp(argv[i], "--conf") == 0 && i + 1 < argc) {
            strncpy(config_dir, argv[i + 1], sizeof(config_dir) - 1);
            config_dir[sizeof(config_dir)-1] = '\0';
        }
    }

    if (config_dir[0] == '\0') {
        const char *home = getenv("HOME");
        if (home && *home)
            snprintf(config_dir, sizeof(config_dir), "%s/.config/xisguard", home);
        else
            strcpy(config_dir, "/tmp/xisguard");
    }

    snprintf(perms_file, sizeof(perms_file), "%s/perms.conf", config_dir);
    snprintf(ignore_file, sizeof(ignore_file), "%s/ignore.conf", config_dir);
    snprintf(xnotify_conf_file, sizeof(xnotify_conf_file), "%s/xnotify.conf", config_dir);
    mkdir(config_dir, 0755);
    xis_safefile_set_logger(safefile_log);

    /* Load saved runtime flags; CLI args below will override them */
    load_xnotify_conf();

    /* Full CLI parse — overrides anything loaded from xnotify.conf */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-pause") == 0 ||
            strcmp(argv[i], "--notify-only") == 0) {
            no_pause_mode = 1;
            log_msg("NOTIFY-ONLY mode activated (no process pausing)");
        } else if (strcmp(argv[i], "--quiet") == 0 ||
                 strcmp(argv[i], "--no-zenity") == 0) {
            quiet_mode = 1;
            log_msg("QUIET mode activated (no dialog prompts)");
        } else if (strcmp(argv[i], "--always-kill") == 0) {
            always_kill_mode = 1;
            log_msg("ALWAYS-KILL mode activated (unknown processes will be killed)");
        } else if (strcmp(argv[i], "--secure-mode") == 0) {
            secure_mode = 1;
            log_msg("SECURE mode activated (perms.conf disabled; permanent rules go through polkit into the server's own config)");
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            int act = string_to_action(argv[++i]);
            if (act == -1) {
                cli_allow_mask = 0xFFFFu;
                log_msg("CLI override: ALLOW ALL actions");
            } else if (act > 0 && act <= 16) {
                cli_allow_mask |= (1u << (act - 1));
                log_msg("CLI override: ALLOW %s", argv[i]);
            } else {
                log_msg("Warning: unknown action '%s' for --allow", argv[i]);
            }
        } else if (strcmp(argv[i], "--deny") == 0 && i + 1 < argc) {
            int act = string_to_action(argv[++i]);
            if (act == -1) {
                cli_deny_mask = 0xFFFFu;
                log_msg("CLI override: DENY ALL actions");
            } else if (act > 0 && act <= 16) {
                cli_deny_mask |= (1u << (act - 1));
                log_msg("CLI override: DENY %s", argv[i]);
            } else {
                log_msg("Warning: unknown action '%s' for --deny", argv[i]);
            }
        } else if (strncmp(argv[i], "--conf=", 7) == 0 ||
                   (strcmp(argv[i], "--conf") == 0 && i + 1 < argc && ++i)) {
            /* already handled above */
        } else if (strcmp(argv[i], "--log-level") == 0 && i + 1 < argc) {
            int lvl = atoi(argv[i+1]);
            if (lvl >= 0 && lvl <= 4) log_level = lvl;
            i++;
        } else if (strncmp(argv[i], "--log-level=", 12) == 0) {
            int lvl = atoi(argv[i] + 12);
            if (lvl >= 0 && lvl <= 4) log_level = lvl;
        }
    }

    if (secure_mode && no_pause_mode) {
        log_msg("Warning: --secure-mode with --no-pause means an unauthorized process keeps running, "
                "unpaused, for as long as the polkit prompt for a permanent rule is up. Continuing anyway.");
    }

    /* Persist the final runtime flags for the next invocation */
    save_xnotify_conf();

    const char *display_env = getenv("DISPLAY");
    if (display_env && sscanf(display_env, ":%d", &display) == 1)
        log_msg("Connecting to display %d", display);
    else {
        log_msg("No display found. Exiting.");
        return 0;
    }

    if(!check_xnotify_extension()){
        log_msg("No XNOTIFY Extension found on display %d. Exiting.", display);
        return 0;
    }

    detect_dialog_backend();

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    const char *base_dir = (runtime_dir && *runtime_dir) ? runtime_dir : "/tmp";
    snprintf(SOCKET_PATH_BUF, sizeof(SOCKET_PATH_BUF), "%s/xnotify.%d.sock", base_dir, display);
    snprintf(LOCK_SOCKET_PATH_BUF, sizeof(LOCK_SOCKET_PATH_BUF), "%s/xnotify.%d.lock.sock", base_dir, display);
    snprintf(CTL_SOCKET_PATH_BUF, sizeof(CTL_SOCKET_PATH_BUF), "%s/xisguard-ctl.%d.sock", base_dir, display);

    log_msg("XisGuard starting - user config: %s", perms_file);

    int replace = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--replace") == 0)
            replace = 1;
    if (!acquire_lock(replace))
        return 1;

    load_user_config();
    load_ignore_reports();
    /* Drop a global mask left behind by a previous guard that died while
     * permissive mode was on; it is only turned back on below when
     * permissive_on_start says so. */
    send_raw_command("{\"command\":\"RELOAD\"}");
    send_all_permissions_to_xserver();
    if (permissive_on_start) {
        permissive_mode = 1;
        permissive_reapply();
        log_msg("Permissive mode ON at start (permissive_on_start)");
    }
    send_heartbeat();

    /* === Server socket setup === */
    server_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (server_fd == -1) {
        log_msg("Failed to create server socket: %s", strerror(errno));
        release_lock();
        return 1;
    }

    fcntl(server_fd, F_SETFL, O_NONBLOCK);

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCKET_PATH_BUF);

    unlink(SOCKET_PATH_BUF);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        log_msg("Failed to bind server socket: %s", strerror(errno));
        close(server_fd);
        release_lock();
        return 1;
    }

    sock_path_ino = path_ino(SOCKET_PATH_BUF);
    chmod(SOCKET_PATH_BUF, 0666);

    int rcvbuf = 4 * 1024 * 1024;  // 4 MB
    setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    log_msg("XisGuard ready - listening on %s", SOCKET_PATH_BUF);

    /* === Control socket setup (local IPC, e.g. xisconf) === */
    ctl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ctl_fd == -1) {
        log_msg("Failed to create control socket: %s", strerror(errno));
        close(server_fd);
        release_lock();
        return 1;
    }

    struct sockaddr_un ctl_addr = {0};
    ctl_addr.sun_family = AF_UNIX;
    snprintf(ctl_addr.sun_path, sizeof(ctl_addr.sun_path), "%s", CTL_SOCKET_PATH_BUF);

    unlink(CTL_SOCKET_PATH_BUF);
    if (bind(ctl_fd, (struct sockaddr*)&ctl_addr, sizeof(ctl_addr)) == -1) {
        log_msg("Failed to bind control socket: %s", strerror(errno));
        close(ctl_fd);
        close(server_fd);
        release_lock();
        return 1;
    }

    /* Owner-only: this channel can grant permissions without a dialog prompt. */
    ctl_path_ino = path_ino(CTL_SOCKET_PATH_BUF);
    chmod(CTL_SOCKET_PATH_BUF, 0600);
    listen(ctl_fd, 4);

    log_msg("XisGuard control socket ready - listening on %s", CTL_SOCKET_PATH_BUF);

    if (secure_mode)
        request_config_path();   /* prefetch, so it's likely cached before it's first needed */

    pthread_create(&file_monitor_thread, NULL, file_monitor_loop, NULL);
    pthread_create(&processor_thread, NULL, alert_processor_loop, NULL);
    pthread_create(&control_thread, NULL, control_loop, NULL);
    {
        /* Detached: it may be blocked in XOpenDisplay() on a hung server and
         * must never hold up shutdown. */
        pthread_t xdisplay_thread;
        if (pthread_create(&xdisplay_thread, NULL, xdisplay_watch_loop, NULL) == 0)
            pthread_detach(xdisplay_thread);
    }

    /* Main loop */
    struct pollfd pfd = {
        .fd = server_fd,
        .events = POLLIN,
    };

    while (!should_exit) {
        if (reexec_requested)
            reexec_self();   /* returns only if the exec failed */

        static time_t last_hb = 0;
        if (time(NULL) - last_hb >= 2) {
            send_heartbeat();
            last_hb = time(NULL);
        }

        notice_tick();

        int r = poll(&pfd, 1, 400);
        if (r > 0 && (pfd.revents & POLLIN)) {
            char buffer[BUF_SIZE] = {0};
            ssize_t n = recv(server_fd, buffer, sizeof(buffer)-1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                handle_message(buffer);
            }
        }
    }

    cleanup(0);
    return 0;
}
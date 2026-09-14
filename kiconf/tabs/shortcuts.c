/* kiconf - Atalhos tab: xiskeys.conf BIND lines.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

enum { COL_ACTION = 0, COL_SPEC, COL_COMMAND, N_SHORTCUT_COLS };

static GtkListStore *g_shortcuts_store;

/* ---- Atalhos fixos do sistema: kiwm.conf's key_* actions and kicomp.conf's
 * per-effect hotkey=/hotkey_next=/hotkey_prev= -- see the design discussion
 * in the project's history for why this isn't xiskeys.conf: both daemons
 * grab these themselves (kiwm on the root window, kicomp via input.c),
 * configured in their own config files, entirely separately from xiskeys.
 *
 * This table is kiconf's own copy of kiwm/keybind.c's `binds[]` and of the
 * five kicomp effect modules (expo/cover-switch/show-windows/stats/cube)
 * that call input_bind_hotkey() -- kept here rather than parsed out of
 * those programs at runtime because there is nothing to parse it from
 * (kiwm's defaults live in a C table, not a machine-readable file). Update
 * this table by hand in the same commit that adds a key_* action to
 * kiwm/keybind.c or a hotkey= to a kicomp effect module. */
typedef enum { FX_KIWM, FX_KICOMP } FixedSource;

typedef struct {
    FixedSource source;
    const char *conf_key;     /* kiwm: kiwm.conf's key_* name.
                                * kicomp: "hotkey"/"hotkey_next"/"hotkey_prev". */
    const char *effect;       /* kicomp only: [effect:<effect>]; NULL for kiwm. */
    const char *default_spec;
    int default_enabled;      /* kicomp only: the module's own default_enabled;
                                * ignored (always "on") for kiwm. */
    const char *doc;
} FixedShortcut;

static const FixedShortcut KIWM_CATALOG[] = {
    {FX_KIWM, "key_window_next", NULL, "Alt+Tab", 1, "Proxima janela (alternador)"},
    {FX_KIWM, "key_window_prev", NULL, "Alt+Shift+Tab", 1, "Janela anterior (alternador)"},
    {FX_KIWM, "key_desktop_next", NULL, "ModKey+Tab", 1, "Proxima area de trabalho (ordem)"},
    {FX_KIWM, "key_desktop_prev", NULL, "ModKey+Shift+Tab", 1, "Area de trabalho anterior (ordem)"},
    {FX_KIWM, "key_desktop_next_horizontal", NULL, "", 1, "Area de trabalho a direita (grade desktop_columns x desktop_rows)"},
    {FX_KIWM, "key_desktop_prev_horizontal", NULL, "", 1, "Area de trabalho a esquerda (grade)"},
    {FX_KIWM, "key_desktop_next_vertical", NULL, "", 1, "Area de trabalho abaixo (grade)"},
    {FX_KIWM, "key_desktop_prev_vertical", NULL, "", 1, "Area de trabalho acima (grade)"},
    {FX_KIWM, "key_maximize", NULL, "ModKey+Up", 1, "Maximizar/restaurar a janela focada"},
    {FX_KIWM, "key_maximize_horizontal", NULL, "", 1, "Maximizar/restaurar so a largura"},
    {FX_KIWM, "key_maximize_vertical", NULL, "", 1, "Maximizar/restaurar so a altura"},
    {FX_KIWM, "key_minimize", NULL, "ModKey+Down", 1, "Minimizar a janela focada"},
    {FX_KIWM, "key_tile_left", NULL, "ModKey+Left", 1, "Encaixar a esquerda (metade da tela, de novo restaura)"},
    {FX_KIWM, "key_tile_right", NULL, "ModKey+Right", 1, "Encaixar a direita (metade da tela, de novo restaura)"},
    {FX_KIWM, "key_fullscreen", NULL, "", 1, "Alternar tela cheia"},
    {FX_KIWM, "key_shade", NULL, "", 1, "Enrolar/desenrolar a janela na barra de titulo"},
    {FX_KIWM, "key_keep_above", NULL, "", 1, "Manter a janela sempre acima das outras"},
    {FX_KIWM, "key_keep_below", NULL, "", 1, "Manter a janela sempre abaixo das outras"},
    {FX_KIWM, "key_sticky", NULL, "", 1, "Mostrar a janela em todas as areas de trabalho"},
    {FX_KIWM, "key_close", NULL, "Alt+F4", 1, "Fechar a janela focada"},
    {FX_KIWM, "key_window_menu", NULL, "", 1, "Abrir o menu da janela focada, sob a barra de titulo"},
    {FX_KIWM, "key_desktop_1", NULL, "", 1, "Ir para a area de trabalho 1"},
    {FX_KIWM, "key_desktop_2", NULL, "", 1, "Ir para a area de trabalho 2"},
    {FX_KIWM, "key_desktop_3", NULL, "", 1, "Ir para a area de trabalho 3"},
    {FX_KIWM, "key_desktop_4", NULL, "", 1, "Ir para a area de trabalho 4"},
    {FX_KIWM, "key_desktop_5", NULL, "", 1, "Ir para a area de trabalho 5"},
    {FX_KIWM, "key_desktop_6", NULL, "", 1, "Ir para a area de trabalho 6"},
    {FX_KIWM, "key_desktop_7", NULL, "", 1, "Ir para a area de trabalho 7"},
    {FX_KIWM, "key_desktop_8", NULL, "", 1, "Ir para a area de trabalho 8"},
    {FX_KIWM, "key_move_to_desktop_next", NULL, "", 1, "Enviar a janela focada para a proxima area de trabalho, sem segui-la"},
    {FX_KIWM, "key_move_to_desktop_prev", NULL, "", 1, "Enviar a janela focada para a area de trabalho anterior"},
    {FX_KIWM, "key_move_to_desktop_1", NULL, "", 1, "Enviar a janela focada para a area de trabalho 1"},
    {FX_KIWM, "key_move_to_desktop_2", NULL, "", 1, "Enviar a janela focada para a area de trabalho 2"},
    {FX_KIWM, "key_move_to_desktop_3", NULL, "", 1, "Enviar a janela focada para a area de trabalho 3"},
    {FX_KIWM, "key_move_to_desktop_4", NULL, "", 1, "Enviar a janela focada para a area de trabalho 4"},
    {FX_KIWM, "key_move_to_desktop_5", NULL, "", 1, "Enviar a janela focada para a area de trabalho 5"},
    {FX_KIWM, "key_move_to_desktop_6", NULL, "", 1, "Enviar a janela focada para a area de trabalho 6"},
    {FX_KIWM, "key_move_to_desktop_7", NULL, "", 1, "Enviar a janela focada para a area de trabalho 7"},
    {FX_KIWM, "key_move_to_desktop_8", NULL, "", 1, "Enviar a janela focada para a area de trabalho 8"},
};

static const FixedShortcut KICOMP_CATALOG[] = {
    {FX_KICOMP, "hotkey", "expo", "Meta+E", 1, "Expo: mostrar todas as areas de trabalho de uma vez"},
    {FX_KICOMP, "hotkey", "cover-switch", "Meta+C", 1,
     "Alternador de janelas em capa (opcional -- o kiwm ja aciona isto sozinho via osd_cover_switch)"},
    {FX_KICOMP, "hotkey", "show-windows", "Meta+A, Meta+W", 1, "Mostrar todas as janelas da area de trabalho atual"},
    {FX_KICOMP, "hotkey", "stats", "Meta+F12", 0, "Mostrar estatisticas de desempenho do compositor"},
    {FX_KICOMP, "hotkey", "cube", "Ctrl+Meta+Button1", 1, "Girar o cubo de areas de trabalho (arrastar)"},
    {FX_KICOMP, "hotkey_next", "cube", "Ctrl+Meta+Right", 1, "Cubo: ir para a proxima face"},
    {FX_KICOMP, "hotkey_prev", "cube", "Ctrl+Meta+Left", 1, "Cubo: ir para a face anterior"},
};

#define N_KIWM_CATALOG ((int)(sizeof(KIWM_CATALOG) / sizeof(KIWM_CATALOG[0])))
#define N_KICOMP_CATALOG ((int)(sizeof(KICOMP_CATALOG) / sizeof(KICOMP_CATALOG[0])))

enum { COL_FX_LABEL = 0, COL_FX_DEFAULT, COL_FX_SPEC, COL_FX_SOURCE, COL_FX_CONFKEY, COL_FX_EFFECT, N_FIXED_COLS };

static GtkListStore *g_fixed_store;
static GtkWidget *g_fixed_status_label;


/* ---- Atalhos tab: xiskeys.conf (BIND action spec command) ------------ */

static void load_shortcuts(void)
{
    gtk_list_store_clear(g_shortcuts_store);

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[768];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (!line[0] || line[0] == '#') {
            continue;
        }
        char *save = NULL;
        char *tag = strtok_r(line, "\t", &save);
        char *action = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *spec = action ? strtok_r(NULL, "\t", &save) : NULL;
        char *cmd = spec ? strtok_r(NULL, "", &save) : NULL;
        if (!tag || strcmp(tag, "BIND") != 0 || !action || !spec || !cmd) {
            continue;
        }
        GtkTreeIter it;
        gtk_list_store_append(g_shortcuts_store, &it);
        gtk_list_store_set(g_shortcuts_store, &it,
                            COL_ACTION, action, COL_SPEC, spec, COL_COMMAND, cmd, -1);
    }
    fclose(f);
}

static void save_shortcuts_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char path[PATH_MAX];
    resolve_path("xiskeys.conf", path, sizeof(path));

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# xiskeys config -- one binding per line:\n");
    fprintf(f, "#   BIND\\t<action-name>\\t<Mod>+<Mod>+<Key>\\t<shell command>\n");

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_shortcuts_store), &it);
    while (valid) {
        gchar *action, *spec, *cmd;
        gtk_tree_model_get(GTK_TREE_MODEL(g_shortcuts_store), &it,
                            COL_ACTION, &action, COL_SPEC, &spec, COL_COMMAND, &cmd, -1);
        if (action && spec && cmd && *action && *spec && *cmd) {
            fprintf(f, "BIND\t%s\t%s\t%s\n", action, spec, cmd);
        }
        g_free(action);
        g_free(spec);
        g_free(cmd);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_shortcuts_store), &it);
    }
    fclose(f);
    rename(tmp, path);

    signal_daemon("xiskeys");
}

static void add_shortcut_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_shortcuts_store, &it);
    gtk_list_store_set(g_shortcuts_store, &it,
                        COL_ACTION, "novo-atalho", COL_SPEC, "Ctrl+F1", COL_COMMAND, "true", -1);
}

static void remove_shortcut_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_shortcuts_store, &it);
    }
}

static void shortcut_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_shortcuts_store), &it, path)) {
        gtk_list_store_set(g_shortcuts_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

/* ---- kiwm.conf: reading one key_* value, and the merge-write back ----- */

/* Looks up `key` (a top-level, no-section key=value line) in kiwm.conf;
 * returns 1 and fills `out` if found, 0 (out left untouched) otherwise --
 * a missing line means "kiwm's own built-in default applies", which is
 * exactly what an empty Atalho cell means here too (see build_shortcuts_tab()). */
static int kiwm_key_value(const char *key, char *out, size_t outsz)
{
    char path[PATH_MAX];
    resolve_path("kiwm.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int found = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (!strcmp(trim(l), key)) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Rewrites kiwm.conf's key_* lines from `store`'s FX_KIWM rows: an empty
 * Atalho cell drops that key_* line entirely (falls back to kiwm's own
 * built-in default), a non-empty one writes key_<name>=<spec>. Every
 * other line -- including anything Gerenciamento de janelas manages -- is
 * carried over untouched, same merge-not-rewrite approach as that tab. */
static void save_kiwm_shortcuts(void)
{
    char path[PATH_MAX];
    resolve_path("kiwm.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    FILE *in = fopen(path, "r");
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", line);
            char *l = trim(buf);
            if (*l && *l != '#') {
                char *eq = strchr(l, '=');
                if (eq) {
                    *eq = '\0';
                    if (!strncmp(trim(l), "key_", 4)) {
                        continue; /* dropped -- rewritten below from the store */
                    }
                }
            }
            fprintf(out, "%s\n", line);
        }
        fclose(in);
    }

    fprintf(out, "\n# Atalhos fixos do kiwm (aba Atalhos, secao 'Atalhos fixos do sistema').\n");
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_fixed_store), &it);
    while (valid) {
        gint source;
        gchar *confkey, *spec;
        gtk_tree_model_get(GTK_TREE_MODEL(g_fixed_store), &it,
                            COL_FX_SOURCE, &source, COL_FX_CONFKEY, &confkey, COL_FX_SPEC, &spec, -1);
        if (source == FX_KIWM && spec && *spec) {
            fprintf(out, "%s=%s\n", confkey, spec);
        }
        g_free(confkey);
        g_free(spec);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_fixed_store), &it);
    }

    fclose(out);
    rename(tmp, path);
}

/* ---- kicomp.conf: per-effect enabled state, one key's value, and the
 * surgical (section-preserving) write-back for hotkey=/hotkey_next=/
 * hotkey_prev= -------------------------------------------------------- */

/* True iff kicomp.conf's global `effects=` key is on (default: on). Only
 * the pre-section part of the file is global keys, same as config_pass()'s
 * own first pass. */
static int kicomp_effects_globally_enabled(void)
{
    char path[PATH_MAX];
    resolve_path("kicomp.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return 1;
    }
    int val = 1;
    int in_section = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        if (*l == '[') {
            in_section = 1;
            continue;
        }
        if (in_section) {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (!strcmp(trim(l), "effects")) {
            val = atoi(trim(eq + 1)) != 0;
        }
    }
    fclose(f);
    return val;
}

/* True iff [effect:<effect>] (the base instance, not a named one) is
 * enabled -- kicomp.conf's own `enabled=` if the section says so,
 * `fallback_enabled` (the module's compiled-in default_enabled) otherwise. */
static int kicomp_effect_enabled(const char *effect, int fallback_enabled)
{
    char path[PATH_MAX];
    resolve_path("kicomp.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return fallback_enabled;
    }
    int val = fallback_enabled;
    int in_target = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        if (*l == '[') {
            char *close = strchr(l, ']');
            in_target = 0;
            if (close) {
                *close = '\0';
                char *name = l + 1;
                if (!strncmp(name, "effect:", 7) && !strchr(name + 7, ':')) {
                    in_target = !strcmp(name + 7, effect);
                }
            }
            continue;
        }
        if (!in_target) {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (!strcmp(trim(l), "enabled")) {
            val = atoi(trim(eq + 1)) != 0;
        }
    }
    fclose(f);
    return val;
}

/* Looks up one key inside [effect:<effect>]'s base instance. */
static int kicomp_effect_key_value(const char *effect, const char *key, char *out, size_t outsz)
{
    char path[PATH_MAX];
    resolve_path("kicomp.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    int found = 0;
    int in_target = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        if (*l == '[') {
            char *close = strchr(l, ']');
            in_target = 0;
            if (close) {
                *close = '\0';
                char *name = l + 1;
                if (!strncmp(name, "effect:", 7) && !strchr(name + 7, ':')) {
                    in_target = !strcmp(name + 7, effect);
                }
            }
            continue;
        }
        if (!in_target) {
            continue;
        }
        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        if (!strcmp(trim(l), key)) {
            snprintf(out, outsz, "%s", trim(eq + 1));
            found = 1;
        }
    }
    fclose(f);
    return found;
}

typedef struct {
    const char *effect, *key, *value; /* value may be "" -- means "remove this key" */
} KicompHotkeyEdit;

/* Applies `edits` to kicomp.conf's [effect:<effect>] base sections without
 * touching anything else in them (enabled=, events=, duration=, whatever
 * the Efeitos tab or a hand edit put there) -- unlike that tab, this one
 * only ever knows about a handful of specific keys, so it patches instead
 * of fully regenerating every section. A section an edit targets but that
 * doesn't exist yet in the file is appended fresh at the end. */
static void save_kicomp_hotkeys(const KicompHotkeyEdit *edits, int n_edits)
{
    char path[PATH_MAX];
    resolve_path("kicomp.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    int done[64] = {0}; /* n_edits is always small (<= N_KICOMP_CATALOG) */
    char cur_effect[64] = "";
    int in_target = 0;

    FILE *in = fopen(path, "r");
    if (in) {
        char line[512];
        while (fgets(line, sizeof(line), in)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", line);
            char *l = trim(buf);

            if (*l == '[') {
                /* Leaving whatever section we were in: append any of its
                 * pending edits that never matched an existing line
                 * (a brand new key in an existing section). */
                if (in_target) {
                    for (int i = 0; i < n_edits; i++) {
                        if (!done[i] && !strcmp(edits[i].effect, cur_effect) && edits[i].value[0]) {
                            fprintf(out, "%s=%s\n", edits[i].key, edits[i].value);
                            done[i] = 1;
                        }
                    }
                }
                char *close = strchr(l, ']');
                in_target = 0;
                cur_effect[0] = '\0';
                if (close) {
                    char save = *close;
                    *close = '\0';
                    char *name = l + 1;
                    if (!strncmp(name, "effect:", 7) && !strchr(name + 7, ':')) {
                        snprintf(cur_effect, sizeof(cur_effect), "%s", name + 7);
                        in_target = 1;
                    }
                    *close = save;
                }
                fprintf(out, "%s\n", line);
                continue;
            }

            if (in_target && *l && *l != '#') {
                char *eq = strchr(l, '=');
                if (eq) {
                    *eq = '\0';
                    char *key = trim(l);
                    int matched = 0;
                    for (int i = 0; i < n_edits; i++) {
                        if (!strcmp(edits[i].effect, cur_effect) && !strcmp(edits[i].key, key)) {
                            matched = 1;
                            done[i] = 1;
                            if (edits[i].value[0]) {
                                fprintf(out, "%s=%s\n", key, edits[i].value);
                            } /* else: drop the line -- back to the module's default */
                            break;
                        }
                    }
                    if (matched) {
                        continue;
                    }
                }
            }
            fprintf(out, "%s\n", line);
        }
        if (in_target) {
            for (int i = 0; i < n_edits; i++) {
                if (!done[i] && !strcmp(edits[i].effect, cur_effect) && edits[i].value[0]) {
                    fprintf(out, "%s=%s\n", edits[i].key, edits[i].value);
                    done[i] = 1;
                }
            }
        }
        fclose(in);
    }

    /* Edits for an effect that had no [effect:...] section at all yet. */
    for (int i = 0; i < n_edits; i++) {
        if (done[i] || !edits[i].value[0]) {
            continue;
        }
        fprintf(out, "\n[effect:%s]\n%s=%s\n", edits[i].effect, edits[i].key, edits[i].value);
        done[i] = 1;
    }

    fclose(out);
    rename(tmp, path);
}

static void save_fixed_shortcuts_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    save_kiwm_shortcuts();

    /* Stack copies of each kicomp row's spec, so edits[].value stays valid
     * (and every gchar* out-param from gtk_tree_model_get() gets g_free()'d
     * the same way, kiwm or kicomp row alike) for as long as this function
     * runs -- well past the save_kicomp_hotkeys() call below. */
    KicompHotkeyEdit edits[N_KICOMP_CATALOG];
    char edit_values[N_KICOMP_CATALOG][128];
    int n_edits = 0;

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_fixed_store), &it);
    while (valid) {
        gint source;
        gchar *confkey, *effect, *spec;
        gtk_tree_model_get(GTK_TREE_MODEL(g_fixed_store), &it,
                            COL_FX_SOURCE, &source, COL_FX_CONFKEY, &confkey,
                            COL_FX_EFFECT, &effect, COL_FX_SPEC, &spec, -1);
        if (source == FX_KICOMP && n_edits < N_KICOMP_CATALOG) {
            for (int i = 0; i < N_KICOMP_CATALOG; i++) {
                if (!strcmp(KICOMP_CATALOG[i].conf_key, confkey) && !strcmp(KICOMP_CATALOG[i].effect, effect)) {
                    snprintf(edit_values[n_edits], sizeof(edit_values[n_edits]), "%s", spec ? spec : "");
                    edits[n_edits].effect = KICOMP_CATALOG[i].effect;
                    edits[n_edits].key = KICOMP_CATALOG[i].conf_key;
                    edits[n_edits].value = edit_values[n_edits];
                    n_edits++;
                    break;
                }
            }
        }
        g_free(confkey);
        g_free(effect);
        g_free(spec);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_fixed_store), &it);
    }
    save_kicomp_hotkeys(edits, n_edits);

    gtk_label_set_text(GTK_LABEL(g_fixed_status_label),
                        "Gravado. kiwm e kicomp precisam ser reiniciados para aplicar (abas "
                        "'Gerenciamento de janelas' / 'Efeitos do compositor').");
}

static void fixed_spec_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_fixed_store), &it, path)) {
        gtk_list_store_set(g_fixed_store, &it, COL_FX_SPEC, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static GtkWidget *build_fixed_shortcuts_view(void)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_fixed_store));

    GtkCellRenderer *label_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *label_col = gtk_tree_view_column_new_with_attributes("Acao", label_r, "text", COL_FX_LABEL, NULL);
    gtk_tree_view_column_set_expand(label_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), label_col);

    GtkCellRenderer *def_r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Padrao", def_r, "text", COL_FX_DEFAULT, NULL));

    GtkCellRenderer *spec_r = gtk_cell_renderer_text_new();
    g_object_set(spec_r, "editable", TRUE, NULL);
    g_signal_connect(spec_r, "edited", G_CALLBACK(fixed_spec_edited), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Atalho", spec_r, "text", COL_FX_SPEC, NULL));

    return view;
}

/* Fills g_fixed_store: every kiwm action (always available), plus every
 * kicomp effect hotkey whose effect is currently enabled (globally and by
 * its own [effect:...] enabled=/default_enabled). Atalho starts blank
 * unless kiwm.conf/kicomp.conf already has a line for it -- blank means
 * "nothing configured, that program's own built-in default (shown in
 * Padrao) applies", matching what leaving it blank on Aplicar does. */
static void load_fixed_shortcuts(void)
{
    gtk_list_store_clear(g_fixed_store);

    for (int i = 0; i < N_KIWM_CATALOG; i++) {
        const FixedShortcut *e = &KIWM_CATALOG[i];
        char spec[128] = "";
        kiwm_key_value(e->conf_key, spec, sizeof(spec));
        char label[192];
        snprintf(label, sizeof(label), "kiwm: %s", e->doc);
        GtkTreeIter it;
        gtk_list_store_append(g_fixed_store, &it);
        gtk_list_store_set(g_fixed_store, &it,
                            COL_FX_LABEL, label, COL_FX_DEFAULT, e->default_spec, COL_FX_SPEC, spec,
                            COL_FX_SOURCE, (gint)FX_KIWM, COL_FX_CONFKEY, e->conf_key, COL_FX_EFFECT, "", -1);
    }

    int effects_on = kicomp_effects_globally_enabled();
    char last_effect[64] = "";
    int last_effect_enabled = 0;
    for (int i = 0; i < N_KICOMP_CATALOG; i++) {
        const FixedShortcut *e = &KICOMP_CATALOG[i];
        if (strcmp(last_effect, e->effect) != 0) {
            snprintf(last_effect, sizeof(last_effect), "%s", e->effect);
            last_effect_enabled = kicomp_effect_enabled(e->effect, e->default_enabled);
        }
        if (!effects_on || !last_effect_enabled) {
            continue; /* not an active function right now -- nothing to bind */
        }
        char spec[128] = "";
        kicomp_effect_key_value(e->effect, e->conf_key, spec, sizeof(spec));
        char label[192];
        snprintf(label, sizeof(label), "kicomp (%s): %s", e->effect, e->doc);
        GtkTreeIter it;
        gtk_list_store_append(g_fixed_store, &it);
        gtk_list_store_set(g_fixed_store, &it,
                            COL_FX_LABEL, label, COL_FX_DEFAULT, e->default_spec, COL_FX_SPEC, spec,
                            COL_FX_SOURCE, (gint)FX_KICOMP, COL_FX_CONFKEY, e->conf_key, COL_FX_EFFECT, e->effect, -1);
    }
}

GtkWidget *build_shortcuts_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    /* Atalhos personalizados: xiskeys.conf, unchanged from before. */
    GtkWidget *vbox = gtk_vbox_new(FALSE, 6);

    g_shortcuts_store = gtk_list_store_new(N_SHORTCUT_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_shortcuts_store));

    const char *titles[N_SHORTCUT_COLS] = {"Acao", "Atalho", "Comando"};
    for (int col = 0; col < N_SHORTCUT_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        g_object_set(renderer, "editable", TRUE, NULL);
        g_signal_connect(renderer, "edited", G_CALLBACK(shortcut_cell_edited), GINT_TO_POINTER(col));
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 160);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label("Adicionar");
    GtkWidget *remove_btn = gtk_button_new_with_label("Remover");
    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xiskeys");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_shortcut_cb), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(remove_shortcut_cb), view);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_shortcuts_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btnbox, FALSE, FALSE, 0);

    load_shortcuts();
    gtk_box_pack_start(GTK_BOX(outer),
                        frame_with("Atalhos personalizados (xiskeys.conf, crie/edite/remova a vontade)", vbox),
                        TRUE, TRUE, 0);

    /* Atalhos fixos do sistema: kiwm.conf/kicomp.conf, catalog-driven. */
    GtkWidget *fixed_vbox = gtk_vbox_new(FALSE, 6);

    g_fixed_store = gtk_list_store_new(N_FIXED_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                        G_TYPE_INT, G_TYPE_STRING, G_TYPE_STRING);
    load_fixed_shortcuts();
    GtkWidget *fixed_view = build_fixed_shortcuts_view();
    GtkWidget *fixed_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fixed_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(fixed_scroll, -1, 220);
    gtk_container_add(GTK_CONTAINER(fixed_scroll), fixed_view);
    gtk_box_pack_start(GTK_BOX(fixed_vbox), fixed_scroll, TRUE, TRUE, 0);

    g_fixed_status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_fixed_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(fixed_vbox), g_fixed_status_label, FALSE, FALSE, 0);

    GtkWidget *fixed_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *fixed_save_btn = gtk_button_new_with_label("Aplicar (grava kiwm.conf e kicomp.conf)");
    g_signal_connect(fixed_save_btn, "clicked", G_CALLBACK(save_fixed_shortcuts_cb), NULL);
    gtk_box_pack_end(GTK_BOX(fixed_btnbox), fixed_save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fixed_vbox), fixed_btnbox, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer),
                        frame_with("Atalhos fixos do sistema (kiwm e efeitos ativos do kicomp -- em "
                                    "branco = usa o padrao daquele programa)",
                                    fixed_vbox),
                        FALSE, FALSE, 0);

    return outer;
}

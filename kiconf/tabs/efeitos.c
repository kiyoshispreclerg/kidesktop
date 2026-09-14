/* kiconf - Efeitos do compositor tab: kicomp.conf.
 * See kiconf.c's top doc comment for the overall design.
 *
 * Like kiwm, kicomp has no control socket and no config-reload signal
 * (SIGHUP means "shut down", see kicomp/src/main.c) -- so this is a plain
 * kicomp.conf editor plus a "Reiniciar kicomp agora" button that starts a
 * new `kicomp --replace`: the running instance answers losing
 * _NET_WM_CM_Sn by shutting down on its own.
 *
 * kicomp.conf's per-effect sections ([effect:name] or
 * [effect:name:instance]) can carry an open-ended set of module-specific
 * keys on top of the five universal ones (enabled/events/windows/easing/
 * duration) -- rather than a schema form per effect module (~19 of them),
 * each row's "Opcoes" is those keys as space-separated key=value tokens,
 * same simplification the Paineis tab already makes for xispanel widgets.
 * [output:NAME] sections (per-monitor scale overrides) aren't modeled
 * here at all and are carried over from the existing file untouched. */
#include "../common.h"
#include "../tabs.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KICOMP_CONF "kicomp.conf"
#define MAX_EFFECTS 128

typedef struct {
    int effects;
    double animation_duration;
    char renderer[16], presenter[16];
    int single_drawable, skip_wm_layers, unredirect_fullscreen;
    double claim_ms;
    int keep_hidden_contents;
    char live_windows[16];
} KicompGlobals;

typedef struct {
    int enabled;
    char windows[128];
    int radius;
    double opacity;
    int offset_x, offset_y;
    char color[16];
    int custom_inactive;
    int radius_i;
    double opacity_i;
    int offset_x_i, offset_y_i;
    char color_i[16];
} ShadowConfig;

typedef struct {
    char name[32], instance[32];
    int enabled;
    char opts[384];
} EffectRow;

static void globals_defaults(KicompGlobals *g)
{
    memset(g, 0, sizeof(*g));
    g->effects = 1;
    g->animation_duration = 160.0;
    snprintf(g->renderer, sizeof(g->renderer), "auto");
    snprintf(g->presenter, sizeof(g->presenter), "auto");
    g->unredirect_fullscreen = 1;
    g->claim_ms = 500.0;
    g->keep_hidden_contents = 1;
    snprintf(g->live_windows, sizeof(g->live_windows), "desktop");
}

static void shadow_defaults(ShadowConfig *s)
{
    memset(s, 0, sizeof(*s));
    snprintf(s->windows, sizeof(s->windows), "windows,menus");
    s->radius = 12;
    s->opacity = 0.45;
    s->offset_y = 6;
    snprintf(s->color, sizeof(s->color), "#000000");
    s->radius_i = s->radius;
    s->opacity_i = s->opacity;
    s->offset_y_i = s->offset_y;
    snprintf(s->color_i, sizeof(s->color_i), "%s", s->color);
}

/* Appends "key=value " to `opts` -- how a per-effect row's module-specific
 * keys (and events=/windows=/easing=/duration=, if present) are kept as
 * one editable field instead of a dynamic per-module schema. */
static void opts_append(char *opts, size_t optssz, const char *key, const char *val)
{
    size_t len = strlen(opts);
    if (len && len + 1 < optssz) {
        opts[len++] = ' ';
        opts[len] = '\0';
    }
    snprintf(opts + len, optssz - len, "%s=%s", key, val);
}

static EffectRow *find_or_add_row(EffectRow *rows, int *n, const char *name, const char *instance)
{
    for (int i = 0; i < *n; i++) {
        if (!strcmp(rows[i].name, name) && !strcmp(rows[i].instance, instance)) {
            return &rows[i];
        }
    }
    if (*n >= MAX_EFFECTS) {
        return NULL;
    }
    EffectRow *r = &rows[*n];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%s", name);
    snprintf(r->instance, sizeof(r->instance), "%s", instance);
    r->enabled = 1;
    (*n)++;
    return r;
}

/* One pass over kicomp.conf: fills `g`/`sh` from the global/[shadow] keys
 * and `rows` from every [effect:...] section. [output:...] sections are
 * skipped here (see file doc comment) -- they're carried over verbatim by
 * copy_output_sections() at save time instead. */
static void load_kicomp_conf(KicompGlobals *g, ShadowConfig *sh, EffectRow *rows, int *n_rows)
{
    globals_defaults(g);
    shadow_defaults(sh);
    *n_rows = 0;

    char path[PATH_MAX];
    resolve_path(KICOMP_CONF, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }

    enum { SEC_NONE, SEC_SHADOW, SEC_EFFECT, SEC_OTHER } section = SEC_NONE;
    EffectRow *cur = NULL;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *l = trim(line);
        if (!*l || *l == '#') {
            continue;
        }
        if (*l == '[') {
            char *close = strchr(l, ']');
            if (!close) {
                continue;
            }
            *close = '\0';
            char *name = l + 1;
            cur = NULL;
            if (!strcmp(name, "shadow")) {
                section = SEC_SHADOW;
            } else if (!strncmp(name, "effect:", 7)) {
                char *mod = name + 7;
                char *inst = strchr(mod, ':');
                if (inst) {
                    *inst++ = '\0';
                }
                cur = find_or_add_row(rows, n_rows, mod, inst ? inst : "");
                section = SEC_EFFECT;
            } else {
                section = SEC_OTHER; /* [output:...] and anything else */
            }
            continue;
        }

        char *eq = strchr(l, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trim(l);
        char *val = trim(eq + 1);

        if (section == SEC_EFFECT) {
            if (!cur) {
                continue;
            }
            if (!strcmp(key, "enabled")) {
                cur->enabled = atoi(val) != 0;
            } else {
                opts_append(cur->opts, sizeof(cur->opts), key, val);
            }
            continue;
        }

        if (section == SEC_SHADOW) {
#define SETS(field) snprintf(sh->field, sizeof(sh->field), "%s", val)
            if (!strcmp(key, "enabled")) sh->enabled = atoi(val) != 0;
            else if (!strcmp(key, "windows")) SETS(windows);
            else if (!strcmp(key, "radius")) sh->radius = atoi(val);
            else if (!strcmp(key, "opacity")) sh->opacity = atof(val);
            else if (!strcmp(key, "offset_x")) sh->offset_x = atoi(val);
            else if (!strcmp(key, "offset_y")) sh->offset_y = atoi(val);
            else if (!strcmp(key, "color")) SETS(color);
            else if (!strcmp(key, "radius_inactive")) { sh->radius_i = atoi(val); sh->custom_inactive = 1; }
            else if (!strcmp(key, "opacity_inactive")) { sh->opacity_i = atof(val); sh->custom_inactive = 1; }
            else if (!strcmp(key, "offset_x_inactive")) { sh->offset_x_i = atoi(val); sh->custom_inactive = 1; }
            else if (!strcmp(key, "offset_y_inactive")) { sh->offset_y_i = atoi(val); sh->custom_inactive = 1; }
            else if (!strcmp(key, "color_inactive")) { SETS(color_i); sh->custom_inactive = 1; }
#undef SETS
            continue;
        }

        if (section == SEC_NONE) {
#define SETS(field) snprintf(g->field, sizeof(g->field), "%s", val)
            if (!strcmp(key, "effects")) g->effects = atoi(val) != 0;
            else if (!strcmp(key, "animation_duration")) g->animation_duration = atof(val);
            else if (!strcmp(key, "renderer")) SETS(renderer);
            else if (!strcmp(key, "presenter")) SETS(presenter);
            else if (!strcmp(key, "single_drawable")) g->single_drawable = atoi(val) != 0;
            else if (!strcmp(key, "skip_wm_layers")) g->skip_wm_layers = atoi(val) != 0;
            else if (!strcmp(key, "unredirect_fullscreen")) g->unredirect_fullscreen = atoi(val) != 0;
            else if (!strcmp(key, "claim_ms")) g->claim_ms = atof(val);
            else if (!strcmp(key, "keep_hidden_contents")) g->keep_hidden_contents = atoi(val) != 0;
            else if (!strcmp(key, "live_windows")) SETS(live_windows);
#undef SETS
        }
    }
    fclose(f);
}

/* Copies every [output:...] section verbatim -- the one part of
 * kicomp.conf this tab reads past but never represents in the UI. */
static void copy_output_sections(FILE *out, FILE *in)
{
    int in_output = 0;
    char line[512];
    while (fgets(line, sizeof(line), in)) {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s", line);
        char *l = trim(buf);
        if (*l == '[') {
            in_output = !strncmp(l, "[output:", 8);
        }
        if (in_output) {
            fputs(line, out);
            if (!strchr(line, '\n')) {
                fputc('\n', out);
            }
        }
    }
}

/* Writes "k1=v1 k2=v2 ..." (space-separated, as build_effects_table()'s
 * Opcoes column holds it) as one "k=v\n" line per token. */
static void write_opts(FILE *out, const char *opts)
{
    char buf[384];
    snprintf(buf, sizeof(buf), "%s", opts);
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok; tok = strtok_r(NULL, " \t", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) {
            fprintf(out, "%s\n", tok);
        }
    }
}

/* Widgets */
static GtkWidget *g_status_label;
static GtkWidget *g_effects_chk, *g_anim_spin, *g_renderer_combo, *g_presenter_combo;
static GtkWidget *g_single_drawable_chk, *g_skip_wm_layers_chk, *g_unredirect_chk;
static GtkWidget *g_claim_spin, *g_keep_hidden_chk, *g_live_windows_combo;
static GtkWidget *g_sh_enabled_chk, *g_sh_windows_entry, *g_sh_radius_spin, *g_sh_opacity_spin;
static GtkWidget *g_sh_offx_spin, *g_sh_offy_spin, *g_sh_color_btn;
static GtkWidget *g_sh_custom_i_chk, *g_sh_radius_i_spin, *g_sh_opacity_i_spin;
static GtkWidget *g_sh_offx_i_spin, *g_sh_offy_i_spin, *g_sh_color_i_btn, *g_sh_inactive_frame;
static GtkListStore *g_fx_store;

enum { COL_FX_NAME = 0, COL_FX_INSTANCE, COL_FX_ENABLED, COL_FX_OPTS, N_FX_COLS };

static void refresh_status(void)
{
    gtk_label_set_text(GTK_LABEL(g_status_label),
                        process_running("kicomp")
                            ? "kicomp esta em execucao."
                            : "kicomp nao esta em execucao -- estas configuracoes valem para a proxima vez que ele iniciar.");
}

static const char *const RENDERER_OPTS[] = {"auto", "xrender", "glx", "egl", NULL};
static const char *const PRESENTER_OPTS[] = {"auto", "present", "copy", NULL};
static const char *const LIVE_WINDOWS_OPTS[] = {"desktop", "active", "all", NULL};

static void toggle_inactive_frame_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    gtk_widget_set_sensitive(g_sh_inactive_frame, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
}

static void fx_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_fx_store), &it, path)) {
        gtk_list_store_set(g_fx_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

static void fx_cell_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    (void)data;
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_fx_store), &it, path)) {
        gboolean cur;
        gtk_tree_model_get(GTK_TREE_MODEL(g_fx_store), &it, COL_FX_ENABLED, &cur, -1);
        gtk_list_store_set(g_fx_store, &it, COL_FX_ENABLED, !cur, -1);
    }
    gtk_tree_path_free(path);
}

static void add_effect_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GtkTreeIter it;
    gtk_list_store_append(g_fx_store, &it);
    gtk_list_store_set(g_fx_store, &it, COL_FX_NAME, "fade-in", COL_FX_INSTANCE, "",
                        COL_FX_ENABLED, TRUE, COL_FX_OPTS, "", -1);
}

static void remove_effect_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(data));
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_fx_store, &it);
    }
}

static GtkWidget *build_effects_view(void)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_fx_store));

    GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
    g_object_set(name_r, "editable", TRUE, NULL);
    g_signal_connect(name_r, "edited", G_CALLBACK(fx_cell_edited), GINT_TO_POINTER(COL_FX_NAME));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Efeito", name_r, "text", COL_FX_NAME, NULL));

    GtkCellRenderer *inst_r = gtk_cell_renderer_text_new();
    g_object_set(inst_r, "editable", TRUE, NULL);
    g_signal_connect(inst_r, "edited", G_CALLBACK(fx_cell_edited), GINT_TO_POINTER(COL_FX_INSTANCE));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Instancia", inst_r, "text", COL_FX_INSTANCE, NULL));

    GtkCellRenderer *en_r = gtk_cell_renderer_toggle_new();
    g_signal_connect(en_r, "toggled", G_CALLBACK(fx_cell_toggled), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Ativo", en_r, "active", COL_FX_ENABLED, NULL));

    GtkCellRenderer *opts_r = gtk_cell_renderer_text_new();
    g_object_set(opts_r, "editable", TRUE, NULL);
    g_signal_connect(opts_r, "edited", G_CALLBACK(fx_cell_edited), GINT_TO_POINTER(COL_FX_OPTS));
    GtkTreeViewColumn *opts_col = gtk_tree_view_column_new_with_attributes("Opcoes (events=/windows=/easing=/duration=/...)",
                                                                            opts_r, "text", COL_FX_OPTS, NULL);
    gtk_tree_view_column_set_expand(opts_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), opts_col);

    return view;
}

static void save_efeitos_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char path[PATH_MAX];
    resolve_path(KICOMP_CONF, path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *out = fopen(tmp, "w");
    if (!out) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    fprintf(out, "# kicomp configuration -- effects/renderer settings written by kiconf.\n");
    fprintf(out, "# [output:...] sections (per-monitor scale) are kept as they were.\n");
    fprintf(out, "# See kicomp/README.md for the full key/section reference.\n\n");
    fprintf(out, "effects=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_effects_chk)));
    fprintf_double(out, "animation_duration", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_anim_spin)), 1);
    fprintf(out, "renderer=%s\n", combo_text(g_renderer_combo, RENDERER_OPTS));
    fprintf(out, "presenter=%s\n", combo_text(g_presenter_combo, PRESENTER_OPTS));
    fprintf(out, "single_drawable=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_single_drawable_chk)));
    fprintf(out, "skip_wm_layers=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_skip_wm_layers_chk)));
    fprintf(out, "unredirect_fullscreen=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_unredirect_chk)));
    fprintf_double(out, "claim_ms", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_claim_spin)), 1);
    fprintf(out, "keep_hidden_contents=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_keep_hidden_chk)));
    fprintf(out, "live_windows=%s\n", combo_text(g_live_windows_combo, LIVE_WINDOWS_OPTS));

    fprintf(out, "\n[shadow]\n");
    fprintf(out, "enabled=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_sh_enabled_chk)));
    fprintf(out, "windows=%s\n", gtk_entry_get_text(GTK_ENTRY(g_sh_windows_entry)));
    fprintf(out, "radius=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_radius_spin)));
    fprintf_double(out, "opacity", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_sh_opacity_spin)), 2);
    fprintf(out, "offset_x=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_offx_spin)));
    fprintf(out, "offset_y=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_offy_spin)));
    char shcolor[16];
    color_button_hex(g_sh_color_btn, shcolor, sizeof(shcolor));
    fprintf(out, "color=%s\n", shcolor);
    if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_sh_custom_i_chk))) {
        fprintf(out, "radius_inactive=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_radius_i_spin)));
        fprintf_double(out, "opacity_inactive", gtk_spin_button_get_value(GTK_SPIN_BUTTON(g_sh_opacity_i_spin)), 2);
        fprintf(out, "offset_x_inactive=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_offx_i_spin)));
        fprintf(out, "offset_y_inactive=%d\n", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_sh_offy_i_spin)));
        char shcolor_i[16];
        color_button_hex(g_sh_color_i_btn, shcolor_i, sizeof(shcolor_i));
        fprintf(out, "color_inactive=%s\n", shcolor_i);
    }

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_fx_store), &it);
    while (valid) {
        gchar *name, *instance, *opts;
        gboolean enabled;
        gtk_tree_model_get(GTK_TREE_MODEL(g_fx_store), &it,
                            COL_FX_NAME, &name, COL_FX_INSTANCE, &instance,
                            COL_FX_ENABLED, &enabled, COL_FX_OPTS, &opts, -1);
        if (name && *name) {
            if (instance && *instance) {
                fprintf(out, "\n[effect:%s:%s]\n", name, instance);
            } else {
                fprintf(out, "\n[effect:%s]\n", name);
            }
            fprintf(out, "enabled=%d\n", enabled);
            if (opts) {
                write_opts(out, opts);
            }
        }
        g_free(name);
        g_free(instance);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_fx_store), &it);
    }

    FILE *in = fopen(path, "r");
    if (in) {
        fprintf(out, "\n");
        copy_output_sections(out, in);
        fclose(in);
    }

    fclose(out);
    rename(tmp, path);

    refresh_status();
}

static void restart_kicomp_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    spawn_replace("kicomp");
    refresh_status();
}

GtkWidget *build_efeitos_tab(void)
{
    KicompGlobals g;
    ShadowConfig sh;
    EffectRow rows[MAX_EFFECTS];
    int n_rows;
    load_kicomp_conf(&g, &sh, rows, &n_rows);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    g_status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_status_label, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *content = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(content), 2);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(scroll), content);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    /* Geral */
    GtkWidget *general_table = gtk_table_new(9, 2, FALSE);
    g_effects_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_effects_chk), g.effects);
    labeled_row(general_table, 0, "Efeitos habilitados:", g_effects_chk);
    g_anim_spin = gtk_spin_button_new_with_range(0.0, 2000.0, 10.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_anim_spin), g.animation_duration);
    labeled_row(general_table, 1, "Duracao base das animacoes (ms):", g_anim_spin);
    g_renderer_combo = make_options_combo(RENDERER_OPTS, g.renderer);
    labeled_row(general_table, 2, "Renderizador:", g_renderer_combo);
    g_presenter_combo = make_options_combo(PRESENTER_OPTS, g.presenter);
    labeled_row(general_table, 3, "Apresentador (present/copy):", g_presenter_combo);
    g_single_drawable_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_single_drawable_chk), g.single_drawable);
    labeled_row(general_table, 4, "Compor tudo em um unico drawable:", g_single_drawable_chk);
    g_skip_wm_layers_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_skip_wm_layers_chk), g.skip_wm_layers);
    labeled_row(general_table, 5, "Ignorar camadas de decoracao do WM:", g_skip_wm_layers_chk);
    g_unredirect_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_unredirect_chk), g.unredirect_fullscreen);
    labeled_row(general_table, 6, "Desviar (unredirect) janela em tela cheia:", g_unredirect_chk);
    g_claim_spin = gtk_spin_button_new_with_range(0.0, 5000.0, 50.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_claim_spin), g.claim_ms);
    labeled_row(general_table, 7, "Tempo para reivindicar janelas ja abertas (ms):", g_claim_spin);
    g_keep_hidden_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_keep_hidden_chk), g.keep_hidden_contents);
    labeled_row(general_table, 8, "Manter conteudo de janelas ocultas (expo/wall):", g_keep_hidden_chk);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Geral", general_table), FALSE, FALSE, 0);

    GtkWidget *live_table = gtk_table_new(1, 2, FALSE);
    g_live_windows_combo = make_options_combo(LIVE_WINDOWS_OPTS, g.live_windows);
    labeled_row(live_table, 0, "Janelas ao vivo em efeitos (expo/wall/cube):", g_live_windows_combo);
    gtk_box_pack_start(GTK_BOX(content), frame_with("Pre-visualizacao ao vivo", live_table), FALSE, FALSE, 0);

    /* Sombra */
    GtkWidget *shadow_table = gtk_table_new(6, 2, FALSE);
    g_sh_enabled_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_sh_enabled_chk), sh.enabled);
    labeled_row(shadow_table, 0, "Sombra habilitada:", g_sh_enabled_chk);
    g_sh_windows_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(g_sh_windows_entry), sh.windows);
    labeled_row(shadow_table, 1, "Tipos de janela (ex.: windows,menus):", g_sh_windows_entry);
    g_sh_radius_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_radius_spin), sh.radius);
    labeled_row(shadow_table, 2, "Raio do desfoque (px):", g_sh_radius_spin);
    g_sh_opacity_spin = gtk_spin_button_new_with_range(0.0, 1.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_sh_opacity_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_opacity_spin), sh.opacity);
    labeled_row(shadow_table, 3, "Opacidade:", g_sh_opacity_spin);
    GtkWidget *offset_box = gtk_hbox_new(FALSE, 4);
    g_sh_offx_spin = gtk_spin_button_new_with_range(-128, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_offx_spin), sh.offset_x);
    g_sh_offy_spin = gtk_spin_button_new_with_range(-128, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_offy_spin), sh.offset_y);
    gtk_box_pack_start(GTK_BOX(offset_box), gtk_label_new("X:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(offset_box), g_sh_offx_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(offset_box), gtk_label_new("Y:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(offset_box), g_sh_offy_spin, TRUE, TRUE, 0);
    labeled_row(shadow_table, 4, "Deslocamento (px):", offset_box);
    g_sh_color_btn = labeled_row(shadow_table, 5, "Cor:", make_color_button(sh.color));
    gtk_box_pack_start(GTK_BOX(content), frame_with("Sombra (janelas com foco)", shadow_table), FALSE, FALSE, 0);

    GtkWidget *shadow_i_outer = gtk_vbox_new(FALSE, 4);
    g_sh_custom_i_chk = gtk_check_button_new_with_label("Usar valores proprios para janelas sem foco");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_sh_custom_i_chk), sh.custom_inactive);
    gtk_box_pack_start(GTK_BOX(shadow_i_outer), g_sh_custom_i_chk, FALSE, FALSE, 0);
    GtkWidget *shadow_i_table = gtk_table_new(5, 2, FALSE);
    g_sh_radius_i_spin = gtk_spin_button_new_with_range(1, 64, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_radius_i_spin), sh.radius_i);
    labeled_row(shadow_i_table, 0, "Raio do desfoque (px):", g_sh_radius_i_spin);
    g_sh_opacity_i_spin = gtk_spin_button_new_with_range(0.0, 1.0, 0.05);
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(g_sh_opacity_i_spin), 2);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_opacity_i_spin), sh.opacity_i);
    labeled_row(shadow_i_table, 1, "Opacidade:", g_sh_opacity_i_spin);
    GtkWidget *offset_i_box = gtk_hbox_new(FALSE, 4);
    g_sh_offx_i_spin = gtk_spin_button_new_with_range(-128, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_offx_i_spin), sh.offset_x_i);
    g_sh_offy_i_spin = gtk_spin_button_new_with_range(-128, 128, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_sh_offy_i_spin), sh.offset_y_i);
    gtk_box_pack_start(GTK_BOX(offset_i_box), gtk_label_new("X:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(offset_i_box), g_sh_offx_i_spin, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(offset_i_box), gtk_label_new("Y:"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(offset_i_box), g_sh_offy_i_spin, TRUE, TRUE, 0);
    labeled_row(shadow_i_table, 2, "Deslocamento (px):", offset_i_box);
    g_sh_color_i_btn = labeled_row(shadow_i_table, 3, "Cor:", make_color_button(sh.color_i));
    g_sh_inactive_frame = frame_with("Sombra (janelas sem foco)", shadow_i_table);
    gtk_widget_set_sensitive(g_sh_inactive_frame, sh.custom_inactive);
    g_signal_connect(g_sh_custom_i_chk, "toggled", G_CALLBACK(toggle_inactive_frame_cb), NULL);
    gtk_box_pack_start(GTK_BOX(shadow_i_outer), g_sh_inactive_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), shadow_i_outer, FALSE, FALSE, 0);

    /* Efeitos (por secao [effect:nome] / [effect:nome:instancia]) */
    g_fx_store = gtk_list_store_new(N_FX_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_BOOLEAN, G_TYPE_STRING);
    for (int i = 0; i < n_rows; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_fx_store, &it);
        gtk_list_store_set(g_fx_store, &it,
                            COL_FX_NAME, rows[i].name, COL_FX_INSTANCE, rows[i].instance,
                            COL_FX_ENABLED, rows[i].enabled, COL_FX_OPTS, rows[i].opts, -1);
    }
    GtkWidget *fx_view = build_effects_view();
    GtkWidget *fx_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fx_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(fx_scroll, -1, 220);
    gtk_container_add(GTK_CONTAINER(fx_scroll), fx_view);
    GtkWidget *fx_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(fx_box), fx_scroll, TRUE, TRUE, 0);
    GtkWidget *fx_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *fx_add = gtk_button_new_with_label("Adicionar efeito");
    GtkWidget *fx_rem = gtk_button_new_with_label("Remover efeito");
    g_signal_connect(fx_add, "clicked", G_CALLBACK(add_effect_cb), NULL);
    g_signal_connect(fx_rem, "clicked", G_CALLBACK(remove_effect_cb), fx_view);
    gtk_box_pack_start(GTK_BOX(fx_btnbox), fx_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fx_btnbox), fx_rem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(fx_box), fx_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content),
                        frame_with("Efeitos (fade-in, fade-out, geometry, scale-in, scale-out, desktop-wall, "
                                   "minimize, cover-switch, dodge, dodge-raise, expo, cube, magic-lamp, shade, "
                                   "show-windows, smooth-move, wobbly, visual-bell, zoom, stats)",
                                   fx_box),
                        TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *restart_btn = gtk_button_new_with_label("Reiniciar kicomp agora");
    GtkWidget *apply_btn = gtk_button_new_with_label("Aplicar (grava kicomp.conf)");
    g_signal_connect(restart_btn, "clicked", G_CALLBACK(restart_kicomp_cb), NULL);
    g_signal_connect(apply_btn, "clicked", G_CALLBACK(save_efeitos_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), restart_btn, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btnbox), apply_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    refresh_status();
    return outer;
}

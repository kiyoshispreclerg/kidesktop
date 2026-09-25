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
    int density;
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
    g->density = 1;
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
            else if (!strcmp(key, "density")) g->density = atoi(val) != 0;
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
static GtkWidget *g_claim_spin, *g_keep_hidden_chk, *g_live_windows_combo, *g_density_chk;
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

/* ---- per-effect option schemas -------------------------------------------
 *
 * kicomp.conf's [effect:name] sections used to be one freeform "Opcoes"
 * text field per row (space-separated key=value tokens, still how they're
 * written to disk -- see write_opts()) because a schema form for ~19
 * modules looked like too much for what this tab used to be. This table
 * is that schema: every key each module actually reads, transcribed from
 * kicomp/README.md's own "## Configuration" reference (the authoritative
 * list -- keep this in sync with it, not the other way around), so the
 * "Adicionar/editar efeito" dialog below can offer a real widget per key
 * instead of asking the user to type "radius=12 opacity=0.8" by hand.
 *
 * "dodge-raise" is deliberately not one of the modules here: it's an
 * internal helper CompEffectOps dodge.c creates for itself (holding a
 * raise back while covering windows clear out), not a separately
 * registered CompEffectModule a [effect:dodge-raise] section could
 * configure -- see dodge.c. Every other registered module is listed. */
typedef enum { FT_BOOL, FT_INT, FT_DOUBLE, FT_STRING, FT_ENUM, FT_COLOR } EffFieldType;

typedef struct {
    const char *key;
    const char *label;
    EffFieldType type;
    const char *def;             /* default, as text -- same form the field serializes back to */
    double min, max, step;       /* FT_INT/FT_DOUBLE only */
    int digits;                  /* FT_DOUBLE only */
    const char *const *opts;     /* FT_ENUM only, NULL-terminated */
} EffectField;

typedef struct {
    const char *name;
    const EffectField *fields;
    int n_fields;
} EffectSchema;

#define FLD_BOOL(k, l, d) {k, l, FT_BOOL, d, 0, 0, 0, 0, NULL}
#define FLD_INT(k, l, d, mn, mx) {k, l, FT_INT, d, mn, mx, 1, 0, NULL}
#define FLD_DBL(k, l, d, mn, mx, st, dg) {k, l, FT_DOUBLE, d, mn, mx, st, dg, NULL}
#define FLD_STR(k, l, d) {k, l, FT_STRING, d, 0, 0, 0, 0, NULL}
#define FLD_ENUM(k, l, d, o) {k, l, FT_ENUM, d, 0, 0, 0, 0, o}
#define FLD_COLOR(k, l, d) {k, l, FT_COLOR, d, 0, 0, 0, 0, NULL}

static const char *const EASING_OPTS[] = {"linear", "in", "out", "in-out", "spring", NULL};
static const char *const ORIGIN3_OPTS[] = {"window", "pointer", "output", NULL};
static const char *const ORIGIN2_OPTS[] = {"window", "pointer", NULL};
static const char *const CROSSING_OPTS[] = {"fade", "hide", NULL};
static const char *const ARRANGE_OPTS[] = {"stack", "grid", NULL};
static const char *const CORNER_OPTS[] = {"top-left", "top-right", "bottom-left", "bottom-right", NULL};
static const char *const ORDER_OPTS[] = {"stack", "alpha", "mru", NULL};

static const EffectField GEOMETRY_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "maximize,unmaximize,move"),
    FLD_STR("windows", "Janelas", "normal,dialog"),
};
static const EffectField FADE_IN_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "open,restore,desktop-enter"),
    FLD_STR("windows", "Janelas", "all"),
};
static const EffectField FADE_OUT_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "close"),
    FLD_STR("windows", "Janelas", "all"),
};
static const EffectField SCALE_IN_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "open"),
    FLD_STR("windows", "Janelas", "windows,menus"),
    FLD_DBL("from", "Tamanho inicial (fracao)", "0.8", 0, 2, 0.05, 2),
    FLD_ENUM("origin", "Origem", "window", ORIGIN3_OPTS),
};
static const EffectField SCALE_OUT_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "close"),
    FLD_DBL("to", "Tamanho final (fracao)", "1.15", 0, 3, 0.05, 2),
    FLD_ENUM("origin", "Origem", "window", ORIGIN3_OPTS),
};
static const EffectField SHADE_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "shade,unshade"),
    FLD_STR("windows", "Janelas", "windows"),
};
static const EffectField MINIMIZE_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.0", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "minimize,restore"),
    FLD_STR("windows", "Janelas", "windows"),
    FLD_BOOL("fade", "Tambem esmaecer", "1"),
};
static const EffectField MAGIC_LAMP_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "2.5", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "minimize,restore"),
    FLD_INT("grid_res", "Resolucao da grade", "32", 4, 48),
    FLD_INT("waves", "Ondulacoes", "0", 0, 12),
    FLD_DBL("wave_amp", "Amplitude da ondulacao", "0.4", 0, 2, 0.05, 2),
};
static const EffectField DESKTOP_WALL_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.5", 0, 10, 0.1, 2),
    FLD_ENUM("easing", "Suavizacao", "in-out", EASING_OPTS),
    FLD_STR("events", "Eventos", "desktop-leave,desktop-enter"),
    FLD_STR("windows", "Janelas", "all"),
    FLD_DBL("distance", "Distancia (fracao da tela)", "1.0", 0, 5, 0.1, 2),
    FLD_BOOL("fade", "Tambem esmaecer", "0"),
    FLD_INT("parallax_delay", "Atraso de paralaxe (ms)", "100", 0, 2000),
    FLD_ENUM("crossing", "Ao cruzar para outra saida", "fade", CROSSING_OPTS),
};
static const EffectField DODGE_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "1.5", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "focus"),
    FLD_STR("windows", "Janelas", "windows"),
    FLD_ENUM("easing", "Suavizacao", "in-out", EASING_OPTS),
    FLD_DBL("strength", "Forca (fracao)", "1.0", 0, 3, 0.05, 2),
    FLD_INT("clearance", "Folga (px)", "8", 0, 128),
    FLD_INT("max_distance", "Distancia maxima (px, 0=sem limite)", "0", 0, 2000),
    FLD_DBL("raise_at", "Ponto do avanco (fracao)", "0.5", 0, 1, 0.05, 2),
};
static const EffectField SMOOTH_MOVE_FIELDS[] = {
    FLD_DBL("duration", "Constante de tempo (x base)", "0.35", 0, 10, 0.05, 2),
    FLD_STR("events", "Eventos", "move"),
    FLD_STR("windows", "Janelas", "windows"),
    FLD_INT("max_lag", "Atraso maximo (px)", "48", 0, 500),
    FLD_BOOL("resize", "Tambem suavizar redimensionamento", "0"),
};
static const EffectField WOBBLY_FIELDS[] = {
    FLD_STR("events", "Eventos", "move"),
    FLD_STR("windows", "Janelas", "windows"),
    FLD_DBL("stiffness", "Rigidez", "0.06", 0, 1, 0.01, 2),
    FLD_DBL("drag", "Arrasto (inercia)", "0.90", 0, 1, 0.01, 2),
    FLD_DBL("move_factor", "Fator de movimento", "0.10", 0, 1, 0.01, 2),
    FLD_INT("tessellation", "Celulas da malha", "12", 2, 16),
    FLD_BOOL("resize", "Tambem no redimensionamento", "0"),
};
static const EffectField EXPO_FIELDS[] = {
    FLD_STR("hotkey", "Atalho", "Meta+E"),
    FLD_DBL("duration", "Duracao (x base)", "1.5", 0, 10, 0.1, 2),
    FLD_ENUM("easing", "Suavizacao", "out", EASING_OPTS),
    FLD_INT("margin", "Margem (px)", "24", 0, 200),
    FLD_INT("padding", "Espacamento (px)", "24", 0, 200),
    FLD_DBL("dim", "Escurecimento dos nao selecionados", "0.82", 0, 1, 0.02, 2),
    FLD_ENUM("arrange", "Arranjo", "stack", ARRANGE_OPTS),
    FLD_COLOR("background", "Cor de fundo", "#000000"),
    FLD_ENUM("live_windows", "Janelas ao vivo", "desktop", LIVE_WINDOWS_OPTS),
};
static const EffectField STATS_FIELDS[] = {
    FLD_STR("hotkey", "Atalho", "Meta+F12"),
    FLD_INT("size", "Largura do painel (px)", "260", 50, 1000),
    FLD_ENUM("corner", "Canto", "top-right", CORNER_OPTS),
    FLD_INT("margin", "Margem (px)", "16", 0, 200),
};
static const EffectField ZOOM_FIELDS[] = {
    FLD_STR("zoom_in", "Atalho (ampliar)", "Meta+WheelUp"),
    FLD_STR("zoom_out", "Atalho (reduzir)", "Meta+WheelDown"),
    FLD_DBL("step", "Passo por clique", "0.25", 0.01, 2, 0.01, 2),
    FLD_DBL("max", "Ampliacao maxima", "8.0", 1, 32, 0.5, 2),
    FLD_DBL("duration", "Duracao (x base)", "0.6", 0, 10, 0.1, 2),
};
static const EffectField VISUAL_BELL_FIELDS[] = {
    FLD_DBL("duration", "Duracao (x base)", "0.9", 0, 10, 0.1, 2),
    FLD_STR("events", "Eventos", "bell"),
    FLD_DBL("amount", "Intensidade (fracao)", "0.035", 0, 1, 0.005, 3),
    FLD_INT("pulses", "Pulsos", "1", 1, 10),
    FLD_ENUM("origin", "Origem", "window", ORIGIN2_OPTS),
};
static const EffectField SHOW_WINDOWS_FIELDS[] = {
    FLD_STR("hotkey", "Atalho(s)", "Meta+A, Meta+W"),
    FLD_DBL("duration", "Duracao (x base)", "1.5", 0, 10, 0.1, 2),
    FLD_ENUM("easing", "Suavizacao", "out", EASING_OPTS),
    FLD_STR("windows", "Janelas", "windows"),
    FLD_DBL("dim", "Escurecimento dos nao selecionados", "0.78", 0, 1, 0.02, 2),
    FLD_INT("margin", "Margem (px)", "48", 0, 200),
    FLD_INT("padding", "Espacamento (px)", "16", 0, 200),
    FLD_ENUM("order", "Ordem", "stack", ORDER_OPTS),
    FLD_BOOL("labels", "Mostrar nomes", "1"),
    FLD_BOOL("other_outputs", "Incluir outras telas", "0"),
    FLD_BOOL("hide_docks", "Esconder paineis", "1"),
    FLD_INT("filter_debounce_ms", "Atraso do filtro (ms)", "100", 0, 2000),
    FLD_ENUM("live_windows", "Janelas ao vivo", "desktop", LIVE_WINDOWS_OPTS),
};
static const EffectField COVER_SWITCH_FIELDS[] = {
    FLD_STR("hotkey", "Atalho", "Meta+C"),
    FLD_DBL("duration", "Duracao (x base)", "0.9", 0, 10, 0.1, 2),
    FLD_ENUM("easing", "Suavizacao", "out", EASING_OPTS),
    FLD_INT("angle", "Angulo (graus)", "60", 0, 180),
    FLD_DBL("perspective", "Perspectiva", "1.2", 0.1, 5, 0.05, 2),
    FLD_DBL("cover", "Tamanho da capa frontal", "0.45", 0, 2, 0.01, 2),
    FLD_DBL("gap", "Distancia ate a primeira capa lateral", "0.17", 0, 2, 0.01, 2),
    FLD_DBL("step", "Distancia entre as demais", "0.055", 0, 2, 0.01, 2),
    FLD_INT("depth", "Profundidade (px)", "260", 0, 2000),
    FLD_INT("visible", "Capas visiveis de cada lado", "4", 1, 20),
    FLD_DBL("background", "Opacidade do fundo", "0.82", 0, 1, 0.02, 2),
    FLD_BOOL("labels", "Mostrar titulo selecionado", "1"),
    FLD_DBL("label_y", "Posicao vertical do titulo", "0.86", 0, 1, 0.01, 2),
    FLD_INT("label_width", "Largura maxima do titulo (px)", "640", 0, 2000),
    FLD_BOOL("wrap", "Dar a volta nas pontas", "1"),
    FLD_BOOL("other_desktops", "Incluir janelas de outras areas", "1"),
    FLD_ENUM("live_windows", "Janelas ao vivo", "all", LIVE_WINDOWS_OPTS),
    FLD_BOOL("follow_desktop", "Trocar wallpaper ao alcancar outra area", "1"),
};
static const EffectField CUBE_FIELDS[] = {
    FLD_STR("hotkey", "Atalho (girar arrastando)", "Ctrl+Meta+Button1"),
    FLD_STR("hotkey_next", "Atalho (face seguinte)", "Ctrl+Meta+Right"),
    FLD_STR("hotkey_prev", "Atalho (face anterior)", "Ctrl+Meta+Left"),
    FLD_DBL("duration", "Duracao (x base)", "1.2", 0, 10, 0.1, 2),
    FLD_ENUM("easing", "Suavizacao", "out", EASING_OPTS),
    FLD_DBL("zoom", "Afastamento do cubo", "0.55", 0, 2, 0.01, 2),
    FLD_DBL("flick_zoom", "Afastamento (giro por atalho)", "0.0", 0, 2, 0.01, 2),
    FLD_DBL("perspective", "Perspectiva", "0.7", 0.1, 5, 0.05, 2),
    FLD_DBL("turns", "Voltas por arrasto completo", "1.0", 0.1, 10, 0.1, 2),
    FLD_INT("tilt_max", "Inclinacao maxima (graus)", "90", 0, 180),
    FLD_INT("window_gap", "Distancia da 1a janela a face (px)", "40", 0, 500),
    FLD_INT("window_spacing", "Distancia entre as demais (px)", "26", 0, 500),
    FLD_COLOR("cap_color", "Cor das tampas", "#383838"),
    FLD_COLOR("background_color", "Cor de fundo", "#000000"),
    FLD_DBL("background", "Opacidade do fundo", "0.9", 0, 1, 0.02, 2),
    FLD_BOOL("flat_docks", "Paineis fixos na face", "1"),
    FLD_DBL("spin_transparency", "Transparencia ao girar (mouse)", "0", 0, 1, 0.05, 2),
    FLD_BOOL("shell", "Desenhar as tampas/lados", "1"),
    FLD_ENUM("live_windows", "Janelas ao vivo", "all", LIVE_WINDOWS_OPTS),
};

#define SCHEMA(n, f) {n, f, (int)(sizeof(f) / sizeof(f[0]))}
static const EffectSchema EFFECT_SCHEMAS[] = {
    SCHEMA("geometry", GEOMETRY_FIELDS),
    SCHEMA("fade-in", FADE_IN_FIELDS),
    SCHEMA("fade-out", FADE_OUT_FIELDS),
    SCHEMA("scale-in", SCALE_IN_FIELDS),
    SCHEMA("scale-out", SCALE_OUT_FIELDS),
    SCHEMA("shade", SHADE_FIELDS),
    SCHEMA("minimize", MINIMIZE_FIELDS),
    SCHEMA("magic-lamp", MAGIC_LAMP_FIELDS),
    SCHEMA("desktop-wall", DESKTOP_WALL_FIELDS),
    SCHEMA("dodge", DODGE_FIELDS),
    SCHEMA("smooth-move", SMOOTH_MOVE_FIELDS),
    SCHEMA("wobbly", WOBBLY_FIELDS),
    SCHEMA("expo", EXPO_FIELDS),
    SCHEMA("stats", STATS_FIELDS),
    SCHEMA("zoom", ZOOM_FIELDS),
    SCHEMA("visual-bell", VISUAL_BELL_FIELDS),
    SCHEMA("show-windows", SHOW_WINDOWS_FIELDS),
    SCHEMA("cover-switch", COVER_SWITCH_FIELDS),
    SCHEMA("cube", CUBE_FIELDS),
};
#undef SCHEMA
#define N_EFFECT_SCHEMAS ((int)(sizeof(EFFECT_SCHEMAS) / sizeof(EFFECT_SCHEMAS[0])))
#define EFFECT_MAX_FIELDS 20 /* cube has the most, at 19 */

static const char *const EFFECT_NAMES[] = {
    "geometry", "fade-in", "fade-out", "scale-in", "scale-out", "shade", "minimize", "magic-lamp",
    "desktop-wall", "dodge", "smooth-move", "wobbly", "expo", "stats", "zoom", "visual-bell",
    "show-windows", "cover-switch", "cube", NULL,
};

static const EffectSchema *find_schema(const char *name)
{
    for (int i = 0; i < N_EFFECT_SCHEMAS; i++) {
        if (!strcmp(EFFECT_SCHEMAS[i].name, name)) {
            return &EFFECT_SCHEMAS[i];
        }
    }
    return NULL;
}

/* Splits an existing row's "Opcoes" text (see write_opts()/opts_append())
 * back into key/value pairs, so the edit dialog can pre-fill each field
 * from whatever the row already had instead of always resetting to the
 * module's defaults. */
#define MAX_OPT_TOKENS 40
typedef struct {
    char key[32];
    char val[160];
} OptToken;

static int parse_opts_tokens(const char *opts, OptToken *toks, int max)
{
    char buf[384];
    snprintf(buf, sizeof(buf), "%s", opts ? opts : "");
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " \t", &save); tok && n < max; tok = strtok_r(NULL, " \t", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        snprintf(toks[n].key, sizeof(toks[n].key), "%s", tok);
        snprintf(toks[n].val, sizeof(toks[n].val), "%s", eq + 1);
        n++;
    }
    return n;
}

static const char *opts_tokens_find(const OptToken *toks, int n, const char *key)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(toks[i].key, key)) {
            return toks[i].val;
        }
    }
    return NULL;
}

/* FT_COLOR values may carry an 8-hex-digit "#rrggbbaa" (kicomp accepts
 * both, see cube's cap_color/background_color) but GdkColor/
 * make_color_button()/color_button_hex() (common.c) only round-trip the
 * 6-digit "#rrggbb" form -- same simplification the Sombra colors above
 * already make. Truncating here keeps an existing 8-digit value from
 * failing gdk_color_parse() and silently showing black instead. */
static void color_hex6(const char *in, char *out, size_t outsz)
{
    size_t n = strlen(in);
    if (n > 7) {
        n = 7;
    }
    memcpy(out, in, n < outsz ? n : outsz - 1);
    out[n < outsz ? n : outsz - 1] = '\0';
}

static GtkWidget *build_field_widget(const EffectField *f, const char *val)
{
    if (!val) {
        val = f->def;
    }
    switch (f->type) {
    case FT_BOOL: {
        GtkWidget *w = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), atoi(val) != 0);
        return w;
    }
    case FT_INT: {
        GtkWidget *w = gtk_spin_button_new_with_range(f->min, f->max, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), atoi(val));
        return w;
    }
    case FT_DOUBLE: {
        GtkWidget *w = gtk_spin_button_new_with_range(f->min, f->max, f->step);
        gtk_spin_button_set_digits(GTK_SPIN_BUTTON(w), f->digits);
        /* g_ascii_strtod, not atof(): `val` is always '.'-decimal text
         * (our own default strings, or a value kicomp.conf's own atof()-
         * based parser wrote/reads the same way) -- atof() reads it
         * through the current LC_NUMERIC instead, which under e.g.
         * pt_BR.UTF-8 (comma decimal) silently stops at the '.' and
         * misparses "0.1" as 0.0. Same reasoning as g_ascii_formatd in
         * field_widget_value() below, the other half of this round-trip. */
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), g_ascii_strtod(val, NULL));
        return w;
    }
    case FT_STRING: {
        GtkWidget *w = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(w), val);
        return w;
    }
    case FT_ENUM:
        return make_options_combo(f->opts, val);
    case FT_COLOR: {
        char hex6[8];
        color_hex6(val, hex6, sizeof(hex6));
        return make_color_button(hex6);
    }
    }
    return gtk_label_new("?");
}

/* Inverse of build_field_widget(): reads the widget's current state back
 * into text, in the exact form write_opts() will later write to
 * kicomp.conf (so a value round-trips through this dialog unchanged). */
static void field_widget_value(const EffectField *f, GtkWidget *w, char *out, size_t outsz)
{
    switch (f->type) {
    case FT_BOOL:
        snprintf(out, outsz, "%d", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)));
        break;
    case FT_INT:
        snprintf(out, outsz, "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)));
        break;
    case FT_DOUBLE: {
        /* g_ascii_formatd, not snprintf("%f") -- kicomp's own atof()
         * expects a '.' decimal point regardless of locale, same reason
         * common.c's fprintf_double() (used a few lines up for the
         * Sombra fields) doesn't use plain snprintf either. */
        char fmt[8];
        snprintf(fmt, sizeof(fmt), "%%.%df", f->digits);
        g_ascii_formatd(out, outsz, fmt, gtk_spin_button_get_value(GTK_SPIN_BUTTON(w)));
        break;
    }
    case FT_STRING:
        snprintf(out, outsz, "%s", gtk_entry_get_text(GTK_ENTRY(w)));
        break;
    case FT_ENUM:
        snprintf(out, outsz, "%s", combo_text(w, f->opts));
        break;
    case FT_COLOR:
        color_button_hex(w, out, outsz);
        break;
    }
}

/* "Adicionar/editar efeito" dialog state -- lives on open_effect_dialog()'s
 * stack for the duration of its (modal, gtk_dialog_run()-blocking) life,
 * so passing its address as callback user_data is safe. */
typedef struct {
    GtkWidget *effect_combo;
    GtkWidget *fields_box;               /* cleared and rebuilt per effect_combo selection */
    GtkWidget *field_widgets[EFFECT_MAX_FIELDS];
    const EffectSchema *schema;          /* the schema field_widgets[] currently matches */
} EffectDialogState;

/* (Re)builds the dynamic options area for whichever effect effect_combo
 * currently has selected. `toks`/`n_toks` (from the row being edited,
 * pass n_toks=0 when adding a new one or switching to a different effect
 * type mid-dialog) pre-fill each field from the existing value when that
 * key is present, falling back to the module's own default otherwise. */
static void rebuild_fields(EffectDialogState *st, const OptToken *toks, int n_toks)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(st->fields_box));
    for (GList *l = kids; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(kids);

    gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(st->effect_combo));
    st->schema = (name ? find_schema(name) : NULL);
    if (!st->schema) {
        st->schema = &EFFECT_SCHEMAS[0];
    }
    g_free(name);

    GtkWidget *table = gtk_table_new(st->schema->n_fields > 0 ? st->schema->n_fields : 1, 2, FALSE);
    for (int i = 0; i < st->schema->n_fields; i++) {
        const EffectField *f = &st->schema->fields[i];
        const char *val = n_toks > 0 ? opts_tokens_find(toks, n_toks, f->key) : NULL;
        st->field_widgets[i] = build_field_widget(f, val);
        labeled_row(table, i, f->label, st->field_widgets[i]);
    }
    gtk_box_pack_start(GTK_BOX(st->fields_box), table, FALSE, FALSE, 0);
    gtk_widget_show_all(st->fields_box);
}

/* Switching the effect combo mid-dialog always rebuilds at defaults
 * (n_toks=0), never the original row's values: those belonged to
 * whatever effect it was before the switch, and reusing them for a
 * different module's same-named-by-coincidence key (e.g. "duration" on
 * every module) would be more surprising than starting clean. */
static void on_effect_combo_changed(GtkWidget *combo, gpointer data)
{
    (void)combo;
    rebuild_fields((EffectDialogState *)data, NULL, 0);
}

/* Shared by "Adicionar efeito" (iter == NULL, appends a new row on OK) and
 * double-clicking an existing row (iter != NULL, updates that row in
 * place on OK) -- see add_effect_cb()/fx_row_activated() below. */
static void open_effect_dialog(GtkTreeIter *iter)
{
    gchar *cur_name = NULL, *cur_instance = NULL, *cur_opts = NULL;
    gboolean cur_enabled = TRUE;
    if (iter) {
        gtk_tree_model_get(GTK_TREE_MODEL(g_fx_store), iter, COL_FX_NAME, &cur_name, COL_FX_INSTANCE,
                            &cur_instance, COL_FX_ENABLED, &cur_enabled, COL_FX_OPTS, &cur_opts, -1);
    }
    const char *initial_name = (cur_name && find_schema(cur_name)) ? cur_name : EFFECT_NAMES[0];

    OptToken toks[MAX_OPT_TOKENS];
    int n_toks = parse_opts_tokens(cur_opts, toks, MAX_OPT_TOKENS);

    EffectDialogState st;
    memset(&st, 0, sizeof(st));

    GtkWidget *dialog = gtk_dialog_new_with_buttons(iter ? "Editar efeito" : "Adicionar efeito", NULL,
                                                     GTK_DIALOG_MODAL, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                     GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 380);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_box_pack_start(GTK_BOX(content_area), outer, TRUE, TRUE, 0);

    GtkWidget *top_table = gtk_table_new(3, 2, FALSE);
    st.effect_combo = make_options_combo(EFFECT_NAMES, initial_name);
    labeled_row(top_table, 0, "Efeito:", st.effect_combo);
    GtkWidget *instance_entry = gtk_entry_new();
    if (cur_instance) {
        gtk_entry_set_text(GTK_ENTRY(instance_entry), cur_instance);
    }
    labeled_row(top_table, 1, "Instancia (opcional):", instance_entry);
    GtkWidget *enabled_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled_chk), cur_enabled);
    labeled_row(top_table, 2, "Ativo:", enabled_chk);
    gtk_box_pack_start(GTK_BOX(outer), top_table, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), gtk_hseparator_new(), FALSE, FALSE, 0);

    GtkWidget *fields_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fields_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    st.fields_box = gtk_vbox_new(FALSE, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(fields_scroll), st.fields_box);
    gtk_box_pack_start(GTK_BOX(outer), fields_scroll, TRUE, TRUE, 0);

    rebuild_fields(&st, toks, n_toks);
    g_signal_connect(st.effect_combo, "changed", G_CALLBACK(on_effect_combo_changed), &st);

    gtk_widget_show_all(dialog);
    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK) {
        char opts[384] = "";
        for (int i = 0; i < st.schema->n_fields; i++) {
            char valbuf[160];
            field_widget_value(&st.schema->fields[i], st.field_widgets[i], valbuf, sizeof(valbuf));
            opts_append(opts, sizeof(opts), st.schema->fields[i].key, valbuf);
        }
        gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(st.effect_combo));
        gboolean enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(enabled_chk));

        GtkTreeIter target;
        if (iter) {
            target = *iter;
        } else {
            gtk_list_store_append(g_fx_store, &target);
        }
        gtk_list_store_set(g_fx_store, &target, COL_FX_NAME, name, COL_FX_INSTANCE,
                            gtk_entry_get_text(GTK_ENTRY(instance_entry)), COL_FX_ENABLED, enabled, COL_FX_OPTS,
                            opts, -1);
        g_free(name);
    }

    gtk_widget_destroy(dialog);
    g_free(cur_name);
    g_free(cur_instance);
    g_free(cur_opts);
}

/* Name/Instancia/Opcoes cells used to be inline-editable (GtkCellRendererText
 * "editable"); now that "Adicionar efeito" and double-clicking a row both
 * open open_effect_dialog()'s schema-driven form, that form is the only
 * way to edit a row's actual settings -- see fx_row_activated() below. Only
 * "Ativo" keeps its own direct toggle, cheap enough not to need the dialog
 * for a quick on/off. */
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
    open_effect_dialog(NULL);
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

/* Double-clicking (or Enter-activating) any row opens the same dialog
 * "Adicionar efeito" uses, pre-filled from that row -- see
 * open_effect_dialog(). Only reached when no cell renderer already
 * consumed the click, which is exactly why Name/Instancia/Opcoes gave up
 * their own inline "editable" above: a click landing on an editable text
 * cell would otherwise start editing that cell instead of ever reaching
 * this signal. */
static void fx_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)view;
    (void)col;
    (void)data;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_fx_store), &it, path)) {
        open_effect_dialog(&it);
    }
}

static GtkWidget *build_effects_view(void)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_fx_store));
    g_signal_connect(view, "row-activated", G_CALLBACK(fx_row_activated), NULL);

    GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Efeito", name_r, "text", COL_FX_NAME, NULL));

    GtkCellRenderer *inst_r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Instancia", inst_r, "text", COL_FX_INSTANCE, NULL));

    GtkCellRenderer *en_r = gtk_cell_renderer_toggle_new();
    g_signal_connect(en_r, "toggled", G_CALLBACK(fx_cell_toggled), NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Ativo", en_r, "active", COL_FX_ENABLED, NULL));

    GtkCellRenderer *opts_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *opts_col = gtk_tree_view_column_new_with_attributes("Opcoes", opts_r, "text", COL_FX_OPTS, NULL);
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
    fprintf(out, "density=%d\n", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_density_chk)));
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

    /* Geral + Sombra side by side (one row), Efeitos below (the other row)
     * -- Geral now also carries live_windows (used to be its own
     * one-row "Pre-visualizacao ao vivo" frame), and Sombra's focused/
     * unfocused halves now share a single frame instead of two, so this
     * tab is two rows total instead of the previous four-frame stack. */
    GtkWidget *top_row = gtk_hbox_new(FALSE, 8);
    gtk_box_pack_start(GTK_BOX(content), top_row, FALSE, FALSE, 0);

    /* Geral */
    GtkWidget *general_table = gtk_table_new(11, 2, FALSE);
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
    g_live_windows_combo = make_options_combo(LIVE_WINDOWS_OPTS, g.live_windows);
    labeled_row(general_table, 9, "Janelas ao vivo em efeitos (expo/wall/cube):", g_live_windows_combo);
    g_density_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_density_chk), g.density);
    labeled_row(general_table, 10, "X-DENSITY (decoracao/conteudo nitidos sob zoom):", g_density_chk);
    gtk_box_pack_start(GTK_BOX(top_row), frame_with("Geral", general_table), TRUE, TRUE, 0);

    /* Sombra -- focused fields directly in the frame, unfocused override
     * nested inside it right below, instead of each being its own
     * top-level frame. */
    GtkWidget *shadow_outer = gtk_vbox_new(FALSE, 8);

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
    gtk_box_pack_start(GTK_BOX(shadow_outer), shadow_table, FALSE, FALSE, 0);

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
    g_sh_inactive_frame = frame_with("Janelas sem foco", shadow_i_table);
    gtk_widget_set_sensitive(g_sh_inactive_frame, sh.custom_inactive);
    g_signal_connect(g_sh_custom_i_chk, "toggled", G_CALLBACK(toggle_inactive_frame_cb), NULL);
    gtk_box_pack_start(GTK_BOX(shadow_i_outer), g_sh_inactive_frame, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(shadow_outer), shadow_i_outer, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(top_row), frame_with("Sombra", shadow_outer), TRUE, TRUE, 0);

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
    gtk_box_pack_start(GTK_BOX(content), frame_with("Efeitos", fx_box), TRUE, TRUE, 0);

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

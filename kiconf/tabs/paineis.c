/* kiconf - Paineis tab: xispanel.conf editor + xispanel-ctl RELOAD.
 * See kiconf.c's top doc comment for the overall design. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_outputs.h"

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

/* Paineis tab (xispanel.conf) widgets + state.
 *
 * g_widgets_store holds *only the selected panel's* widgets, in on-screen
 * (== file/layout) order -- not every panel's widgets in one shared store
 * any more. This is what lets the list be plainly gtk_tree_view_set_
 * reorderable(): that convenience only drives a real GtkListStore's own
 * GtkTreeDragSource/Dest, which a GtkTreeModelFilter (the previous
 * approach, filtering one shared store down to the selected panel's rows)
 * does not implement, and reordering only ever makes sense *within* one
 * panel's list anyway (xispanel lays a panel's widgets out in the order
 * their WIDGET lines are encountered for *that panel specifically* --
 * see panel_add_widget() in xispanel.c -- so another panel's widgets
 * interleaved in the file never affected this panel's own order to begin
 * with). Every other panel's widgets, meanwhile, live in g_shelf[] --
 * see shelf_from_store()/shelf_to_store(), swapped in and out of
 * g_widgets_store by on_panel_selection_changed() exactly like
 * g_themes[] already does for the theme entry below it. */
static GtkListStore *g_panels_store;
static GtkListStore *g_widgets_store;
static GtkWidget *g_theme_options_entry;
static GtkWidget *g_theme_label;
/* bg/fg/spacing dedicated controls -- see THEME_BG_FIELD et al. below.
 * g_theme_bg_box/g_theme_fg_box are fixed table cells; the WT_COLOR_OPT
 * widget actually inside them is rebuilt (not just re-valued) on every
 * panel switch, same as open_panel_dialog()'s own fields, since
 * build_widget_field()/widget_field_value() are this file's only API for
 * that widget type. */
static GtkWidget *g_theme_bg_box, *g_theme_fg_box;
static GtkWidget *g_theme_bg_field, *g_theme_fg_field;
static GtkWidget *g_theme_spacing_spin;
static char g_selected_panel[NAME_LEN] = "";

/* Defined with the rest of the widget-options schema/dialog machinery
 * below (see "Adicionar/editar widget dialog"), forward-declared here
 * since add_widget_cb()/widget_row_activated() (down among the other UI
 * callbacks) call it before that point in the file. */
static void open_widget_dialog(GtkTreeIter *iter);
/* Same story, for "Adicionar/editar painel dialog" and add_panel_cb(). */
static void open_panel_dialog(GtkTreeIter *iter);
/* Same story, for the bg/fg/spacing THEME controls, defined near
 * build_widget_field() below but needed by save_panels_cb() above that. */
static void theme_opts_from_fields(char *out, size_t outsz);

enum { COL_PANEL_NAME = 0, COL_PANEL_OUTPUT, COL_PANEL_OPTIONS, N_PANEL_COLS };
enum { COL_WIDGET_TYPE = 0, COL_WIDGET_OPTIONS, N_WIDGET_COLS };

/* ---- Paineis tab: xispanel.conf editor + xispanel-ctl RELOAD ---------- */

#define MAX_PANELS 32
#define MAX_WIDGETS 256
typedef struct {
    char name[NAME_LEN], output[NAME_LEN], options[512];
} PanelRec;
typedef struct {
    char panel[NAME_LEN], type[64], options[512];
} WidgetRec;
typedef struct {
    char panel[NAME_LEN], options[512];
} ThemeRec;

/* Every panel's THEME line, kept in memory (not just the selected one) so
 * switching which panel is selected doesn't lose whatever was typed for
 * the previously selected one before Salvar is clicked -- see
 * theme_find_or_add()/on_panel_selection_changed(). */
static ThemeRec g_themes[MAX_PANELS];
static int g_n_themes;

static ThemeRec *theme_find(const char *panel)
{
    for (int i = 0; i < g_n_themes; i++) {
        if (!strcmp(g_themes[i].panel, panel)) {
            return &g_themes[i];
        }
    }
    return NULL;
}

static ThemeRec *theme_find_or_add(const char *panel)
{
    ThemeRec *r = theme_find(panel);
    if (r || g_n_themes >= MAX_PANELS) {
        return r;
    }
    r = &g_themes[g_n_themes++];
    snprintf(r->panel, sizeof(r->panel), "%s", panel);
    r->options[0] = '\0';
    return r;
}

/* Every *non-selected* panel's widgets, in their own on-screen order --
 * see the file doc comment above g_widgets_store. */
typedef struct {
    char type[64], options[512];
} WidgetEntry;
typedef struct {
    char panel[NAME_LEN];
    WidgetEntry entries[MAX_WIDGETS];
    int n_entries;
} PanelShelf;

static PanelShelf g_shelf[MAX_PANELS];
static int g_n_shelf;

static PanelShelf *shelf_find(const char *panel)
{
    for (int i = 0; i < g_n_shelf; i++) {
        if (!strcmp(g_shelf[i].panel, panel)) {
            return &g_shelf[i];
        }
    }
    return NULL;
}

static PanelShelf *shelf_find_or_add(const char *panel)
{
    PanelShelf *r = shelf_find(panel);
    if (r || g_n_shelf >= MAX_PANELS) {
        return r;
    }
    r = &g_shelf[g_n_shelf++];
    snprintf(r->panel, sizeof(r->panel), "%s", panel);
    r->n_entries = 0;
    return r;
}

/* Replaces `panel`'s shelved widget list with whatever's currently in
 * g_widgets_store, in its current (on-screen) order -- called right
 * before g_widgets_store stops representing that panel, i.e. just before
 * switching the Paineis selection away from it, and once more before
 * Salvar in case the selection was never actually changed since the last
 * edit. */
static void shelf_from_store(const char *panel)
{
    if (!panel[0]) {
        return;
    }
    PanelShelf *sh = shelf_find_or_add(panel);
    if (!sh) {
        return;
    }
    sh->n_entries = 0;
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_widgets_store), &it);
    while (valid && sh->n_entries < MAX_WIDGETS) {
        gchar *type, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_widgets_store), &it, COL_WIDGET_TYPE, &type, COL_WIDGET_OPTIONS, &opts,
                            -1);
        WidgetEntry *e = &sh->entries[sh->n_entries++];
        snprintf(e->type, sizeof(e->type), "%s", type ? type : "");
        snprintf(e->options, sizeof(e->options), "%s", opts ? opts : "");
        g_free(type);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_widgets_store), &it);
    }
}

/* Inverse of shelf_from_store(): clears g_widgets_store and repopulates it
 * from `panel`'s shelved list (empty if it has none yet, e.g. a freshly
 * added panel) -- called right after switching the Paineis selection to
 * it. */
static void shelf_to_store(const char *panel)
{
    gtk_list_store_clear(g_widgets_store);
    PanelShelf *sh = panel[0] ? shelf_find(panel) : NULL;
    if (!sh) {
        return;
    }
    for (int i = 0; i < sh->n_entries; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_widgets_store, &it);
        gtk_list_store_set(g_widgets_store, &it, COL_WIDGET_TYPE, sh->entries[i].type, COL_WIDGET_OPTIONS,
                            sh->entries[i].options, -1);
    }
}

static void load_xispanel_conf(PanelRec *panels, int *n_panels, WidgetRec *widgets, int *n_widgets,
                                 ThemeRec *themes, int *n_themes)
{
    *n_panels = *n_widgets = *n_themes = 0;
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *l = line;
        size_t len = strlen(l);
        while (len > 0 && (l[len - 1] == '\n' || l[len - 1] == '\r')) {
            l[--len] = '\0';
        }
        char *t = skip_ws(l);
        if (!*t || *t == '#') {
            continue;
        }
        char *cursor = t;
        char *tag = next_field(&cursor);
        if (!tag) {
            continue;
        }
        if (!strcmp(tag, "PANEL") && *n_panels < MAX_PANELS) {
            char *name = next_field(&cursor);
            char *output = next_field(&cursor);
            if (!name || !output) {
                continue;
            }
            PanelRec *r = &panels[(*n_panels)++];
            snprintf(r->name, sizeof(r->name), "%s", name);
            snprintf(r->output, sizeof(r->output), "%s", output);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "WIDGET") && *n_widgets < MAX_WIDGETS) {
            char *panel = next_field(&cursor);
            char *order = next_field(&cursor); /* positional only, see PROTOCOL.md */
            char *type = next_field(&cursor);
            if (!panel || !order || !type) {
                continue;
            }
            WidgetRec *r = &widgets[(*n_widgets)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->type, sizeof(r->type), "%s", type);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        } else if (!strcmp(tag, "THEME") && *n_themes < MAX_PANELS) {
            char *panel = next_field(&cursor);
            if (!panel) {
                continue;
            }
            ThemeRec *r = &themes[(*n_themes)++];
            snprintf(r->panel, sizeof(r->panel), "%s", panel);
            snprintf(r->options, sizeof(r->options), "%s", skip_ws(cursor));
        }
    }
    fclose(f);
}

static void save_xispanel_conf(void)
{
    char path[PATH_MAX];
    resolve_path("xispanel.conf", path, sizeof(path));
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &it);
    while (valid) {
        gchar *name, *output, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), &it,
                            COL_PANEL_NAME, &name, COL_PANEL_OUTPUT, &output, COL_PANEL_OPTIONS, &opts, -1);
        if (name && *name) {
            fprintf(f, "PANEL\t%s\t%s\t%s\n", name, output && *output ? output : "*", opts ? opts : "");
        }
        g_free(name);
        g_free(output);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_panels_store), &it);
    }

    /* g_widgets_store only ever holds the *selected* panel's widgets (see
     * the file doc comment above it) -- flush it to the shelf first so
     * this loop, which writes every panel's widgets from the shelf, sees
     * this panel's latest on-screen order too. */
    shelf_from_store(g_selected_panel);

    /* order is positional-only (see PROTOCOL.md -- layout order actually
     * comes from each WIDGET line's own position in the file, per panel),
     * so it's just each panel's 0-based count in shelf (== on-screen)
     * order, not edited directly anywhere in the UI. */
    valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &it);
    while (valid) {
        gchar *name;
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), &it, COL_PANEL_NAME, &name, -1);
        PanelShelf *sh = (name && *name) ? shelf_find(name) : NULL;
        if (sh) {
            for (int i = 0; i < sh->n_entries; i++) {
                if (sh->entries[i].type[0]) {
                    fprintf(f, "WIDGET\t%s\t%d\t%s\t%s\n", name, i, sh->entries[i].type, sh->entries[i].options);
                }
            }
        }
        g_free(name);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_panels_store), &it);
    }

    /* Sync whatever's currently in the entry back into g_themes[] for the
     * selected panel first -- covers clicking Salvar without having
     * switched panel selection since the last edit, which is otherwise
     * the only case that wouldn't already be there (see
     * on_panel_selection_changed()). */
    if (g_selected_panel[0]) {
        ThemeRec *cur = theme_find_or_add(g_selected_panel);
        if (cur) {
            theme_opts_from_fields(cur->options, sizeof(cur->options));
        }
    }
    for (int i = 0; i < g_n_themes; i++) {
        if (g_themes[i].panel[0] && g_themes[i].options[0]) {
            fprintf(f, "THEME\t%s\t%s\n", g_themes[i].panel, g_themes[i].options);
        }
    }

    fclose(f);
    rename(tmp, path);
}

static void save_panels_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    save_xispanel_conf();
    xispanel_reload();
}

static void add_panel_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    open_panel_dialog(NULL);
}

static void remove_panel_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_panels_store, &it);
    }
}

/* Renaming a panel (editing its Nome cell) has to cascade to every widget
 * row and theme entry that still refers to the old name, or they'd
 * silently go orphaned -- invisible before this tab filtered the widgets
 * list by selected panel (an orphaned row still showed up in the old
 * flat, unfiltered list), but now such a row would simply vanish from
 * view instead. */
/* ---- per-widget-type option schemas --------------------------------------
 *
 * Every xispanel WIDGET line's options used to be one freeform "Opcoes"
 * text cell (space-separated key=value tokens, still how they're written
 * to xispanel.conf) because a schema form for 13 widget types looked like
 * too much for what this tab used to be -- same story kicomp's effects
 * had in tabs/efeitos.c, which already got this treatment; this is the
 * paineis.c side of the same idea, independently implemented (not shared
 * code) because the two option grammars actually differ in ways worth
 * keeping separate rather than papering over: xispanel spells its
 * booleans "yes"/"no" (kicomp uses "1"/"0"), and a xispanel value may be
 * double-quoted to embed spaces (`cmd="xterm -e htop"`, see kv_get() in
 * xispanel.c) which kicomp's options never need.
 *
 * Every key below, and its default, is transcribed from
 * xispanel/PROTOCOL.md's own "WIDGET" reference (keep this table in sync
 * with it, not the other way around). A field's default is always spelled
 * out explicitly (never left as an implicit "just don't emit this key")
 * because every widget's own config parser was checked to treat an
 * explicitly-present default the same as the key being absent entirely
 * (kv_get_int()'s own `defval` parameter, or an early explicit struct
 * field init before the optional kv_get() override -- see e.g. spacer.c's
 * `fixed_size` or monitor.c's `mp->index`/`width_cfg`/`interval_ms`), so
 * writing every key on every OK does not change behavior. The one place
 * that isn't true is a handful of monitor's fields that gate a whole
 * feature on the key's mere *presence* (color/track_color/high_color/
 * high -- see monitor.c: `mp->has_color` etc. only ever becomes true
 * because the key was found at all) -- WT_COLOR_OPT and a plain optional
 * WT_STRING handle those: see the "skip empty (or ENUM's "none") values"
 * rule in open_widget_dialog() below, which is what makes leaving one of
 * those blank actually omit the key instead of writing a hollow default. */
typedef enum { WT_BOOL, WT_BOOL01, WT_INT, WT_STRING, WT_ENUM, WT_COLOR_OPT } WidgetFieldType;

typedef struct {
    const char *key;
    const char *label;
    WidgetFieldType type;
    const char *def;          /* default, as text */
    double min, max;          /* WT_INT only */
    const char *const *opts;  /* WT_ENUM only, NULL-terminated; "none" is the
                                 * magic "omit this key" value where used */
} WidgetField;

typedef struct {
    const char *name;
    const WidgetField *fields;
    int n_fields;
} WidgetSchema;

#define WF_BOOL(k, l, d) {k, l, WT_BOOL, d, 0, 0, NULL}
/* Same as WF_BOOL but spelled "0"/"1" on disk instead of "no"/"yes" --
 * PANEL's own tooltip_reuse is the one field in this whole schema table
 * that isn't a widget option (see PANEL_FIELDS below, kv_get_int()!=0 in
 * apply_panel_kv()), so it gets its own field type rather than forcing
 * every WT_BOOL consumer to know which spelling a given key uses. */
#define WF_BOOL01(k, l, d) {k, l, WT_BOOL01, d, 0, 0, NULL}
#define WF_INT(k, l, d, mn, mx) {k, l, WT_INT, d, mn, mx, NULL}
#define WF_STR(k, l, d) {k, l, WT_STRING, d, 0, 0, NULL}
#define WF_ENUM(k, l, d, o) {k, l, WT_ENUM, d, 0, 0, o}
#define WF_COLOR_OPT(k, l) {k, l, WT_COLOR_OPT, "", 0, 0, NULL}

static const char *const TASKLIST_MODE_OPTS[] = {"wide", "compact", NULL};
static const char *const MONITOR_METRIC_OPTS[] = {"cpu", "ram", "swap", "gpu", "vram", "cpu_temp", "gpu_temp", NULL};
static const char *const MONITOR_STYLE_OPTS[] = {"text", "bar", "both", NULL};
static const char *const ORIENTATION_OPTS[] = {"vertical", "horizontal", NULL};
static const char *const WINCTL_SHOW_OPTS[] = {"maximized", "always", NULL};
static const char *const WINCTL_SIDE_OPTS[] = {"start", "end", NULL};
static const char *const WINCTL_FALLBACK_OPTS[] = {"none", "clock", "username", "os", "os_version", "text", NULL};
static const char *const NOTIF_CORNER_OPTS[] = {
    "bottom-right", "bottom-left", "bottom-center", "top-right", "top-left", "top-center", "center-left",
    "center-right", NULL,
};
static const char *const GLOBALMENU_MODE_OPTS[] = {"open", "closed", NULL};

/* Panel-level THEME line's bg/fg/spacing, broken out of the free-text
 * THEME entry into dedicated color pickers + a spin button (xisconf.py
 * parity) -- other THEME keys (font_size, h_color, theme=<folder>, see
 * apply_theme_kv() in xispanel.c) stay in that free-text entry, which
 * keeps whatever it already had for those minus these 3 keys. */
static const WidgetField THEME_BG_FIELD = WF_COLOR_OPT("bg", "Definir:");
static const WidgetField THEME_FG_FIELD = WF_COLOR_OPT("fg", "Definir:");
static const WidgetField THEME_SPACING_FIELD = WF_INT("spacing", "Espacamento entre widgets (px)", "4", 0, 64);

static const WidgetField SPACER_FIELDS[] = {
    WF_INT("size", "Tamanho fixo (px, 0 = elastico)", "0", 0, 2000),
};
static const WidgetField CLOCK_FIELDS[] = {
    WF_STR("format", "Formato (strftime)", "%H:%M"),
    WF_BOOL("capitalize", "Iniciar linhas com maiuscula", "yes"),
    WF_INT("font_size", "Tamanho da fonte (px, 0 = automatico)", "0", 0, 200),
    WF_STR("tz", "Fuso horario (vazio = do sistema)", ""),
    WF_STR("tooltip_tz", "Fusos extras na dica (separados por virgula)", ""),
    WF_STR("cmd", "Comando ao clicar", "xisserve"),
};
static const WidgetField TASKLIST_FIELDS[] = {
    WF_ENUM("mode", "Modo", "wide", TASKLIST_MODE_OPTS),
    WF_BOOL("show_desktop_badge", "Emblema de area de trabalho", "no"),
    WF_BOOL("same_desktop", "So a area de trabalho atual", "no"),
    WF_BOOL("same_output", "So esta tela", "no"),
    WF_BOOL("minimized_only", "So janelas minimizadas", "no"),
    WF_INT("icon_padding", "Espacamento do icone (px)", "0", 0, 64),
    WF_BOOL("show_thumbs", "Miniaturas na dica", "no"),
    /* WF_STR (not WF_INT): blank = "key omitted" = tooltip.c's own
     * 200x130 default for that axis -- same "skip empty" convention the
     * container widget's own padding= field uses. */
    WF_STR("thumb_w", "Largura maxima da miniatura (px; vazio = 200)", ""),
    WF_STR("thumb_h", "Altura maxima da miniatura (px; vazio = 130)", ""),
    WF_BOOL("group", "Agrupar por aplicativo", "no"),
    WF_STR("pinned", "Apps fixados no inicio (wm_class1,wm_class2,...)", ""),
    WF_BOOL("fixed_first", "Bloco de fixados antes das janelas normais", "yes"),
    WF_BOOL("launch_feedback", "Animar icone fixado ao lancar", "no"),
    WF_STR("launch_feedback_zoom", "Ampliacao do icone ao lancar (ex: 2.0; minimo 1.05)", "2.0"),
    WF_INT("launch_feedback_ms", "Duracao da animacao ao lancar (ms)", "500", 16, 5000),
    WF_INT("recent_max", "Itens recentes na lista do app (0 = ocultar)", "5", 0, 50),
    /* Sidecar file xispanel itself writes/maintains on every pin/unpin
     * (see tasklist_persist_pinned() in widgets/tasklist.c) -- shown here
     * mainly so it's visible and doesn't get silently discarded, not
     * because it's meant to be hand-typed; blank until the first "Fixar"
     * click sets it. */
    WF_STR("fixed_list", "Arquivo de apps fixados (gerado ao fixar o primeiro)", ""),
};
static const WidgetField PAGER_FIELDS[] = {
    WF_BOOL("same_output_only", "So as areas de trabalho desta tela", "yes"),
    WF_BOOL("show_windows", "Desenhar contorno das janelas", "no"),
};
static const WidgetField MONITOR_FIELDS[] = {
    WF_ENUM("metric", "Metrica", "cpu", MONITOR_METRIC_OPTS),
    WF_STR("index", "Indice (nucleo/placa/sensor; vazio = padrao)", ""),
    WF_ENUM("style", "Estilo", "text", MONITOR_STYLE_OPTS),
    WF_ENUM("orientation", "Orientacao da barra", "vertical", ORIENTATION_OPTS),
    WF_INT("width", "Largura (px, 0 = automatico)", "0", 0, 2000),
    WF_INT("interval", "Intervalo de atualizacao (ms)", "1000", 100, 60000),
    WF_COLOR_OPT("color", "Cor da barra"),
    WF_COLOR_OPT("track_color", "Cor do fundo da barra"),
    WF_COLOR_OPT("high_color", "Cor de alerta"),
    WF_STR("high", "Limiar de alerta (vazio = nunca muda de cor)", ""),
    WF_STR("label", "Rotulo", ""),
    WF_INT("font_size", "Tamanho da fonte (px, 0 = automatico)", "0", 0, 200),
    WF_STR("hwmon", "Chip hwmon (vazio = automatico)", ""),
};
static const WidgetField WINCTL_FIELDS[] = {
    WF_STR("buttons", "Botoes (lista separada por virgula)", "min,max,close"),
    WF_ENUM("show", "Mostrar botoes", "maximized", WINCTL_SHOW_OPTS),
    WF_ENUM("side", "Lado dos botoes", "end", WINCTL_SIDE_OPTS),
    WF_BOOL("same_desktop", "So a area de trabalho atual", "no"),
    WF_BOOL("same_output", "So esta tela", "no"),
    WF_STR("width", "Largura fixa (px ou NN%; vazio = automatica)", ""),
    WF_ENUM("fallback", "Conteudo quando nao aplicavel", "none", WINCTL_FALLBACK_OPTS),
    WF_STR("fallback_format", "Formato do relogio (se fallback=clock)", "%H:%M"),
    WF_STR("fallback_text", "Texto (se fallback=text)", ""),
    WF_BOOL("collapse_buttons", "Recolher botoes ate passar o mouse", "no"),
};
static const WidgetField TRAY_FIELDS[] = {
    WF_INT("icon_padding", "Espacamento dos icones (px)", "0", 0, 64),
    WF_INT("rows", "Linhas da grade de icones", "1", 1, 16),
};
static const WidgetField LAUNCHER_FIELDS[] = {
    WF_STR("icon", "Icone (caminho)", ""),
    WF_STR("name", "Nome (dica)", ""),
    WF_STR("cmd", "Comando (clique esquerdo)", ""),
    WF_STR("cmd_middle", "Comando (clique do meio)", ""),
    WF_STR("cmd_right", "Comando (clique direito)", ""),
    WF_STR("cmd_scroll_up", "Comando (rolar para cima)", ""),
    WF_STR("cmd_scroll_down", "Comando (rolar para baixo)", ""),
};
static const WidgetField VOLUME_FIELDS[] = {
    WF_INT("step", "Passo do volume (%)", "5", 1, 50),
    WF_STR("cmd", "Comando ao clicar (xisserve)", ""),
    WF_STR("cmd_edit", "Comando (clique direito)", "pavucontrol"),
};
static const WidgetField ENERGY_FIELDS[] = {
    WF_STR("cmd", "Comando ao clicar (xisserve)", ""),
    WF_INT("interval", "Intervalo de leitura da bateria (ms)", "10000", 1000, 300000),
};
static const WidgetField NOTIF_FIELDS[] = {
    WF_ENUM("corner", "Canto dos alertas (toast)", "bottom-right", NOTIF_CORNER_OPTS),
    WF_INT("timeout", "Duracao do alerta (ms)", "5000", 500, 60000),
    WF_STR("cmd", "Comando ao clicar", ""),
};
static const WidgetField GLOBALMENU_FIELDS[] = {
    WF_ENUM("mode", "Modo", "closed", GLOBALMENU_MODE_OPTS),
    WF_BOOL("keep", "Manter o icone/espaco sem menu", "no"),
    WF_BOOL("same_desktop", "So a area de trabalho atual", "no"),
    WF_BOOL("same_output", "So esta tela", "no"),
    WF_BOOL("focused_only", "So com foco real do teclado", "no"),
    WF_STR("hotkey", "Atalho global (opcional)", ""),
};
static const WidgetField FOLDER_FIELDS[] = {
    WF_STR("path", "Pasta", ""),
    WF_STR("icon", "Icone (caminho; vazio = tema)", ""),
    WF_STR("name", "Nome (dica)", ""),
    WF_STR("hotkey", "Atalho global (opcional)", ""),
};
static const WidgetField XISSERVE_FIELDS[] = {
    WF_STR("cmd", "Comando (binario)", "xisserve"),
    WF_STR("icon", "Icone (caminho)", ""),
    WF_STR("name", "Nome (dica)", "Applications"),
    WF_STR("hotkey", "Atalho global (opcional)", ""),
};
/* name= is the mode=container PANEL this chevron opens -- the popup's
 * own widgets are edited by selecting *that* panel in the Paineis list,
 * same as any other panel's. */
static const WidgetField CONTAINER_FIELDS[] = {
    WF_STR("name", "Painel container que abre (nome)", ""),
    WF_STR("icon", "Icone (caminho; vazio = setinha)", ""),
    WF_STR("hotkey", "Atalho global (opcional)", ""),
    /* WF_STR (not WF_INT): blank means "key omitted", which is what tells
     * xispanel to fall back to the popup panel's own THEME spacing=
     * instead of forcing 0px -- see the WT_STRING "skip empty" rule the
     * OK handler below already applies, same convention width= (winctl)
     * and every other optional numeric-as-text key here uses. */
    WF_STR("padding", "Espacamento entre widgets dentro (px; vazio = espacamento do painel)", ""),
};

/* Extra row every widget gets while it sits *inside* a container panel
 * (see is_container_panel()): inline=no|urgent|yes, whether it also/only
 * shows on the owner bar -- see xispanel/PROTOCOL.md's "Container
 * popups". Not a per-type schema field since it's the panel it's on,
 * not the type, that decides whether the key means anything. */
static const char *const INLINE_OPTS[] = {"no", "urgent", "yes", NULL};
static const WidgetField CONTAINER_INLINE_FIELD =
    WF_ENUM("inline", "Mostrar no painel dono (inline)", "no", INLINE_OPTS);

#define WSCHEMA(n, f) {n, f, (int)(sizeof(f) / sizeof(f[0]))}
static const WidgetSchema WIDGET_SCHEMAS[] = {
    WSCHEMA("spacer", SPACER_FIELDS),
    WSCHEMA("clock", CLOCK_FIELDS),
    WSCHEMA("tasklist", TASKLIST_FIELDS),
    WSCHEMA("pager", PAGER_FIELDS),
    WSCHEMA("monitor", MONITOR_FIELDS),
    WSCHEMA("winctl", WINCTL_FIELDS),
    WSCHEMA("tray", TRAY_FIELDS),
    WSCHEMA("launcher", LAUNCHER_FIELDS),
    WSCHEMA("volume", VOLUME_FIELDS),
    WSCHEMA("energy", ENERGY_FIELDS),
    WSCHEMA("notif", NOTIF_FIELDS),
    WSCHEMA("globalmenu", GLOBALMENU_FIELDS),
    WSCHEMA("folder", FOLDER_FIELDS),
    WSCHEMA("xisserve", XISSERVE_FIELDS),
    WSCHEMA("container", CONTAINER_FIELDS),
};
#undef WSCHEMA
#define N_WIDGET_SCHEMAS ((int)(sizeof(WIDGET_SCHEMAS) / sizeof(WIDGET_SCHEMAS[0])))
#define WIDGET_MAX_FIELDS 18 /* tasklist has the most, at 17, plus the inline row */

static const char *const WIDGET_TYPE_NAMES[] = {
    "spacer", "clock", "tasklist", "pager", "monitor", "winctl", "tray", "launcher",
    "volume", "energy", "notif", "globalmenu", "folder", "xisserve", "container", NULL,
};
/* The only types xispanel accepts on a mode=container panel (its
 * `embeddable` PanelWidgetOps flag -- keep in sync with the widget files
 * under xispanel/widgets/). Deliberately no `container`: they don't nest. */
static const char *const EMBEDDABLE_TYPE_NAMES[] = {
    "monitor", "tray", "launcher", "volume", "energy", "notif", "folder", NULL,
};


static const WidgetSchema *find_widget_schema(const char *name)
{
    for (int i = 0; i < N_WIDGET_SCHEMAS; i++) {
        if (!strcmp(WIDGET_SCHEMAS[i].name, name)) {
            return &WIDGET_SCHEMAS[i];
        }
    }
    return NULL;
}

/* xispanel's own key=value grammar (see kv_get() in xispanel.c): space-
 * separated tokens, a value may be wrapped in double quotes to embed
 * spaces (no escaping inside them). This mirrors that exactly, unlike a
 * plain strtok_r(" ") which would shred `cmd="xterm -e htop"` into three
 * bogus tokens. */
#define MAX_WOPT_TOKENS 32
typedef struct {
    char key[32];
    char val[256];
} WOptToken;

static int parse_wopts_tokens(const char *opts, WOptToken *toks, int max)
{
    if (!opts) {
        return 0;
    }
    int n = 0;
    const char *p = opts;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        const char *tok_start = p;
        int in_quotes = 0;
        while (*p && (in_quotes || (*p != ' ' && *p != '\t'))) {
            if (*p == '"') {
                in_quotes = !in_quotes;
            }
            p++;
        }
        size_t tok_len = (size_t)(p - tok_start);
        const char *eq = memchr(tok_start, '=', tok_len);
        if (!eq) {
            continue;
        }
        size_t keylen = (size_t)(eq - tok_start);
        if (keylen >= sizeof(toks[n].key)) {
            keylen = sizeof(toks[n].key) - 1;
        }
        memcpy(toks[n].key, tok_start, keylen);
        toks[n].key[keylen] = '\0';

        const char *val_start = eq + 1;
        size_t vlen = tok_len - keylen - 1;
        if (vlen >= 2 && val_start[0] == '"' && val_start[vlen - 1] == '"') {
            val_start++;
            vlen -= 2;
        }
        if (vlen >= sizeof(toks[n].val)) {
            vlen = sizeof(toks[n].val) - 1;
        }
        memcpy(toks[n].val, val_start, vlen);
        toks[n].val[vlen] = '\0';
        n++;
    }
    return n;
}

static const char *wopts_tokens_find(const WOptToken *toks, int n, const char *key)
{
    for (int i = 0; i < n; i++) {
        if (!strcmp(toks[i].key, key)) {
            return toks[i].val;
        }
    }
    return NULL;
}

/* Appends "key=value" (quoting value if it contains whitespace, the same
 * convention kv_get() unwraps) to a space-separated options string. */
static void wopts_append(char *opts, size_t optssz, const char *key, const char *val)
{
    size_t len = strlen(opts);
    int needs_quotes = strpbrk(val, " \t") != NULL;
    int would_add = (int)strlen(key) + 1 + (int)strlen(val) + (needs_quotes ? 2 : 0) + (len ? 1 : 0);
    if (len + (size_t)would_add >= optssz) {
        return;
    }
    if (len) {
        opts[len++] = ' ';
    }
    len += (size_t)snprintf(opts + len, optssz - len, "%s=", key);
    if (needs_quotes) {
        snprintf(opts + len, optssz - len, "\"%s\"", val);
    } else {
        snprintf(opts + len, optssz - len, "%s", val);
    }
}

/* 1 if the PANEL row named `panel` in g_panels_store has mode=container
 * -- decides which type list and which extra fields the widget dialog
 * shows for the selected panel's widgets. */
static int is_container_panel(const char *panel)
{
    if (!panel || !panel[0]) {
        return 0;
    }
    int result = 0;
    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &it);
    while (valid && !result) {
        gchar *name, *opts;
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), &it, COL_PANEL_NAME, &name, COL_PANEL_OPTIONS, &opts, -1);
        if (name && !strcmp(name, panel)) {
            WOptToken toks[MAX_WOPT_TOKENS];
            int n = parse_wopts_tokens(opts, toks, MAX_WOPT_TOKENS);
            const char *mode = wopts_tokens_find(toks, n, "mode");
            result = mode && !strcmp(mode, "container");
        }
        g_free(name);
        g_free(opts);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_panels_store), &it);
    }
    return result;
}

static GtkWidget *build_widget_field(const WidgetField *f, const char *val)
{
    if (!val) {
        val = f->def;
    }
    switch (f->type) {
    case WT_BOOL: {
        GtkWidget *w = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), !strcmp(val, "yes"));
        return w;
    }
    case WT_BOOL01: {
        GtkWidget *w = gtk_check_button_new();
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(w), !strcmp(val, "1"));
        return w;
    }
    case WT_INT: {
        GtkWidget *w = gtk_spin_button_new_with_range(f->min, f->max, 1);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(w), atoi(val));
        return w;
    }
    case WT_STRING: {
        GtkWidget *w = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(w), val);
        return w;
    }
    case WT_ENUM:
        return make_options_combo(f->opts, val);
    case WT_COLOR_OPT: {
        /* An hbox(checkbox, color button): unchecked means this key is
         * omitted entirely on OK (see the has_color/has_track/
         * has_high_color gate these three monitor fields have -- any
         * *present* value turns the feature on, so "leave it off" can
         * only be represented by not writing the key at all, not by any
         * particular color). Checked and pre-filled when the existing
         * value actually parses as a color; unchecked (defaulting to
         * black, irrelevant while unchecked) otherwise. */
        GtkWidget *box = gtk_hbox_new(FALSE, 4);
        GtkWidget *chk = gtk_check_button_new_with_label("Definir:");
        char hex6[8] = "#000000";
        gboolean has_val = val[0] != '\0';
        if (has_val) {
            size_t n = strlen(val);
            if (n > 7) {
                n = 7;
            }
            memcpy(hex6, val, n);
            hex6[n] = '\0';
        }
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), has_val);
        GtkWidget *btn = make_color_button(hex6);
        gtk_widget_set_sensitive(btn, has_val);
        g_signal_connect_swapped(chk, "toggled", G_CALLBACK(gtk_widget_set_sensitive), btn);
        g_object_set_data(G_OBJECT(box), "chk", chk);
        g_object_set_data(G_OBJECT(box), "btn", btn);
        gtk_box_pack_start(GTK_BOX(box), chk, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
        return box;
    }
    }
    return gtk_label_new("?");
}

/* Inverse of build_widget_field(): reads the widget's current state back
 * into text. An empty `out` (WT_STRING left blank, or WT_COLOR_OPT
 * unchecked) or ENUM's magic "none" tells the caller to omit the key
 * entirely -- see open_widget_dialog()'s OK handling. */
static void widget_field_value(const WidgetField *f, GtkWidget *w, char *out, size_t outsz)
{
    switch (f->type) {
    case WT_BOOL:
        snprintf(out, outsz, "%s", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "yes" : "no");
        break;
    case WT_BOOL01:
        snprintf(out, outsz, "%s", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w)) ? "1" : "0");
        break;
    case WT_INT:
        snprintf(out, outsz, "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(w)));
        break;
    case WT_STRING:
        snprintf(out, outsz, "%s", gtk_entry_get_text(GTK_ENTRY(w)));
        break;
    case WT_ENUM:
        snprintf(out, outsz, "%s", combo_text(w, f->opts));
        break;
    case WT_COLOR_OPT: {
        GtkWidget *chk = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "chk"));
        GtkWidget *btn = GTK_WIDGET(g_object_get_data(G_OBJECT(w), "btn"));
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(chk))) {
            color_button_hex(btn, out, outsz);
        } else {
            out[0] = '\0';
        }
        break;
    }
    }
}

/* Rebuilds `*cur` (a WT_COLOR_OPT hbox previously returned by
 * build_widget_field(), or NULL the first time) inside `box`, the same
 * "destroy and recreate" approach rebuild_widget_fields() uses for the
 * widget-options dialog -- this file has no API to just re-value one of
 * these widgets in place. */
static void theme_rebuild_color_field(GtkWidget *box, GtkWidget **cur, const WidgetField *f, const char *val)
{
    if (*cur) {
        gtk_widget_destroy(*cur);
    }
    *cur = build_widget_field(f, val);
    gtk_box_pack_start(GTK_BOX(box), *cur, TRUE, TRUE, 0);
    gtk_widget_show_all(*cur);
}

/* Everything in `opts` except bg=/fg=/spacing= -- what's left in the
 * free-text THEME entry once those 3 keys get their own dedicated
 * controls below (font_size=, h_color=, theme=<folder>, see
 * apply_theme_kv() in xispanel.c). */
static void theme_opts_strip_known(const char *opts, char *out, size_t outsz)
{
    out[0] = '\0';
    WOptToken toks[MAX_WOPT_TOKENS];
    int n = parse_wopts_tokens(opts, toks, MAX_WOPT_TOKENS);
    for (int i = 0; i < n; i++) {
        if (!strcmp(toks[i].key, "bg") || !strcmp(toks[i].key, "fg") || !strcmp(toks[i].key, "spacing")) {
            continue;
        }
        wopts_append(out, outsz, toks[i].key, toks[i].val);
    }
}

/* Loads the bg/fg/spacing controls and the free-text "other options"
 * entry from `opts` (a panel's ThemeRec.options, or "" for none) --
 * called on every panel switch and when first building the tab. */
static void sync_theme_fields_from_opts(const char *opts)
{
    WOptToken toks[MAX_WOPT_TOKENS];
    int n = parse_wopts_tokens(opts, toks, MAX_WOPT_TOKENS);
    theme_rebuild_color_field(g_theme_bg_box, &g_theme_bg_field, &THEME_BG_FIELD, wopts_tokens_find(toks, n, "bg"));
    theme_rebuild_color_field(g_theme_fg_box, &g_theme_fg_field, &THEME_FG_FIELD, wopts_tokens_find(toks, n, "fg"));
    const char *spacing = wopts_tokens_find(toks, n, "spacing");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_theme_spacing_spin), spacing ? atoi(spacing) : 4);

    char leftover[512];
    theme_opts_strip_known(opts, leftover, sizeof(leftover));
    if (g_theme_options_entry) {
        gtk_entry_set_text(GTK_ENTRY(g_theme_options_entry), leftover);
    }
}

/* Inverse of sync_theme_fields_from_opts(): rebuilds a full THEME options
 * string from the bg/fg/spacing controls plus whatever's left in the
 * free-text entry (font_size=, h_color=, theme=, ...). */
static void theme_opts_from_fields(char *out, size_t outsz)
{
    out[0] = '\0';
    char *leftover = g_theme_options_entry ? gtk_editable_get_chars(GTK_EDITABLE(g_theme_options_entry), 0, -1) : NULL;
    if (leftover && leftover[0]) {
        snprintf(out, outsz, "%s", leftover);
    }
    g_free(leftover);

    char valbuf[256];
    widget_field_value(&THEME_BG_FIELD, g_theme_bg_field, valbuf, sizeof(valbuf));
    if (valbuf[0]) {
        wopts_append(out, outsz, "bg", valbuf);
    }
    widget_field_value(&THEME_FG_FIELD, g_theme_fg_field, valbuf, sizeof(valbuf));
    if (valbuf[0]) {
        wopts_append(out, outsz, "fg", valbuf);
    }
    snprintf(valbuf, sizeof(valbuf), "%d", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_theme_spacing_spin)));
    wopts_append(out, outsz, "spacing", valbuf);
}

/* "Adicionar/editar widget" dialog state -- see open_widget_dialog(). */
typedef struct {
    GtkWidget *type_combo;
    GtkWidget *fields_box;
    GtkWidget *field_widgets[WIDGET_MAX_FIELDS];
    const WidgetSchema *schema;
    int in_container;         /* editing a widget of a mode=container panel */
    GtkWidget *inline_widget; /* the CONTAINER_INLINE_FIELD row, when in_container */
} WidgetDialogState;

static void rebuild_widget_fields(WidgetDialogState *st, const WOptToken *toks, int n_toks)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(st->fields_box));
    for (GList *l = kids; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(kids);

    gchar *name = gtk_combo_box_get_active_text(GTK_COMBO_BOX(st->type_combo));
    st->schema = name ? find_widget_schema(name) : NULL;
    if (!st->schema) {
        st->schema = &WIDGET_SCHEMAS[0];
    }
    g_free(name);

    int n_rows = st->schema->n_fields + (st->in_container ? 1 : 0);
    GtkWidget *table = gtk_table_new(n_rows > 0 ? n_rows : 1, 2, FALSE);
    for (int i = 0; i < st->schema->n_fields; i++) {
        const WidgetField *f = &st->schema->fields[i];
        const char *val = n_toks > 0 ? wopts_tokens_find(toks, n_toks, f->key) : NULL;
        st->field_widgets[i] = build_widget_field(f, val);
        labeled_row(table, i, f->label, st->field_widgets[i]);
    }
    st->inline_widget = NULL;
    if (st->in_container) {
        const WidgetField *f = &CONTAINER_INLINE_FIELD;
        const char *val = n_toks > 0 ? wopts_tokens_find(toks, n_toks, f->key) : NULL;
        st->inline_widget = build_widget_field(f, val);
        labeled_row(table, st->schema->n_fields, f->label, st->inline_widget);
    }
    gtk_box_pack_start(GTK_BOX(st->fields_box), table, FALSE, FALSE, 0);
    gtk_widget_show_all(st->fields_box);
}

/* Switching the type combo mid-dialog always rebuilds at defaults, same
 * reasoning as efeitos.c's on_effect_combo_changed(): the old row's
 * values belonged to a different widget type, and reusing them for a
 * same-named-by-coincidence key (e.g. "cmd" means something different on
 * every widget that has one) would be more surprising than starting
 * clean. */
static void on_widget_type_changed(GtkWidget *combo, gpointer data)
{
    (void)combo;
    rebuild_widget_fields((WidgetDialogState *)data, NULL, 0);
}

/* Shared by "Adicionar widget" (iter == NULL, appends a new row on OK) and
 * double-clicking/activating an existing row (iter != NULL, updates that
 * row in place) -- see add_widget_cb()/widget_row_activated(). `iter`, if
 * given, is always a g_widgets_store iter (already converted from the
 * filtered view's own coordinates by the caller). */
static void open_widget_dialog(GtkTreeIter *iter)
{
    gchar *cur_type = NULL, *cur_opts = NULL;
    if (iter) {
        gtk_tree_model_get(GTK_TREE_MODEL(g_widgets_store), iter, COL_WIDGET_TYPE, &cur_type, COL_WIDGET_OPTIONS,
                            &cur_opts, -1);
    }
    /* A container panel only takes embeddable types (xispanel logs and
     * drops anything else), so don't even offer the rest there. */
    int in_container = is_container_panel(g_selected_panel);
    const char *const *type_names = in_container ? EMBEDDABLE_TYPE_NAMES : WIDGET_TYPE_NAMES;
    const char *initial_type = (cur_type && find_widget_schema(cur_type)) ? cur_type : type_names[0];

    WOptToken toks[MAX_WOPT_TOKENS];
    int n_toks = parse_wopts_tokens(cur_opts, toks, MAX_WOPT_TOKENS);

    WidgetDialogState st;
    memset(&st, 0, sizeof(st));
    st.in_container = in_container;

    GtkWidget *dialog = gtk_dialog_new_with_buttons(iter ? "Editar widget" : "Adicionar widget", NULL,
                                                     GTK_DIALOG_MODAL, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                     GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 380);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_box_pack_start(GTK_BOX(content_area), outer, TRUE, TRUE, 0);

    GtkWidget *top_table = gtk_table_new(1, 2, FALSE);
    st.type_combo = make_options_combo(type_names, initial_type);
    labeled_row(top_table, 0, "Tipo de widget:", st.type_combo);
    gtk_box_pack_start(GTK_BOX(outer), top_table, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), gtk_hseparator_new(), FALSE, FALSE, 0);

    GtkWidget *fields_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fields_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    st.fields_box = gtk_vbox_new(FALSE, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(fields_scroll), st.fields_box);
    gtk_box_pack_start(GTK_BOX(outer), fields_scroll, TRUE, TRUE, 0);

    rebuild_widget_fields(&st, toks, n_toks);
    g_signal_connect(st.type_combo, "changed", G_CALLBACK(on_widget_type_changed), &st);

    gtk_widget_show_all(dialog);
    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK) {
        char opts[512] = "";
        for (int i = 0; i < st.schema->n_fields; i++) {
            char valbuf[256];
            widget_field_value(&st.schema->fields[i], st.field_widgets[i], valbuf, sizeof(valbuf));
            /* Skip an empty value (WT_STRING left blank, WT_COLOR_OPT
             * unchecked) or ENUM's magic "none" -- both mean "omit this
             * key", see the file doc comment above WT_COLOR_OPT. */
            if (valbuf[0] && strcmp(valbuf, "none") != 0) {
                wopts_append(opts, sizeof(opts), st.schema->fields[i].key, valbuf);
            }
        }
        if (st.inline_widget) {
            char valbuf[256];
            widget_field_value(&CONTAINER_INLINE_FIELD, st.inline_widget, valbuf, sizeof(valbuf));
            if (valbuf[0] && strcmp(valbuf, "no") != 0) { /* "no" is xispanel's default: omit */
                wopts_append(opts, sizeof(opts), CONTAINER_INLINE_FIELD.key, valbuf);
            }
        }
        gchar *type = gtk_combo_box_get_active_text(GTK_COMBO_BOX(st.type_combo));

        /* Carry over any key from the widget's original options that
         * isn't one of the form fields just written above -- e.g.
         * tasklist's own `pinned=`/`fixed_first=`/`recent_max=`/
         * `launch_feedback*=`, or a key this schema table simply hasn't
         * caught up with yet. Without this, opts above is built from
         * *only* the schema fields, so anything else the widget's config
         * line carried (including state the widget itself writes back,
         * like tasklist's fixed_list= sidecar -- see its own field above)
         * would silently vanish the moment this dialog's OK is clicked,
         * even though nothing about it was ever shown or touched here.
         * Skipped when the type was changed: a key from the *previous*
         * type's options is meaningless (and potentially misleading) on
         * the new one. */
        if (cur_type && type && !strcmp(cur_type, type)) {
            for (int i = 0; i < n_toks; i++) {
                int known = st.inline_widget && !strcmp(CONTAINER_INLINE_FIELD.key, toks[i].key);
                for (int j = 0; !known && j < st.schema->n_fields; j++) {
                    known = !strcmp(st.schema->fields[j].key, toks[i].key);
                }
                if (!known) {
                    wopts_append(opts, sizeof(opts), toks[i].key, toks[i].val);
                }
            }
        }

        GtkTreeIter target;
        if (iter) {
            target = *iter;
        } else {
            gtk_list_store_append(g_widgets_store, &target);
        }
        gtk_list_store_set(g_widgets_store, &target, COL_WIDGET_TYPE, type, COL_WIDGET_OPTIONS, opts, -1);
        g_free(type);
    }

    gtk_widget_destroy(dialog);
    g_free(cur_type);
    g_free(cur_opts);
}

/* Renaming a panel (editing its Nome cell) only has to update the shelf
 * and theme entries that still refer to the old name now -- g_widgets_store
 * itself carries no panel reference any more (it's always just "whatever
 * the selected panel's widgets are"), so a plain rename doesn't touch it
 * at all, unlike before this tab kept every panel's widgets in one shared
 * store. */
static void rename_panel_everywhere(const char *old_name, const char *new_name)
{
    if (!old_name[0] || !strcmp(old_name, new_name)) {
        return;
    }
    PanelShelf *sh = shelf_find(old_name);
    if (sh) {
        snprintf(sh->panel, sizeof(sh->panel), "%s", new_name);
    }
    ThemeRec *th = theme_find(old_name);
    if (th) {
        snprintf(th->panel, sizeof(th->panel), "%s", new_name);
    }
    if (!strcmp(g_selected_panel, old_name)) {
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", new_name);
    }
}

/* ---- "Adicionar/editar painel" dialog -- same idea as the widget
 * dialog above, just simpler: PANEL has one fixed schema (no per-type
 * selector to switch/rebuild), plus a name field and an output picker
 * ahead of it. Every key here is always written on OK (never omitted
 * the way a blank WT_STRING/WT_COLOR_OPT is for widgets) -- none of
 * these fields have "presence alone" semantics, and every default below
 * is transcribed from alloc_panel()'s own field inits (see PROTOCOL.md's
 * "PANEL" reference, kept in sync with that, not the other way around),
 * so writing them all is exactly the same as leaving them unset. */
static const char *const PANEL_EDGE_OPTS[] = {"top", "bottom", "left", "right", NULL};
static const char *const PANEL_MODE_OPTS[] = {"dock", "overlay", "autohide", "container", NULL};
static const char *const PANEL_LAYOUT_OPTS[] = {"row", "grid", NULL};
static const char *const PANEL_ROTATE_OPTS[] = {"0", "90", "180", "270", NULL};

static const WidgetField PANEL_FIELDS[] = {
    WF_ENUM("edge", "Borda", "top", PANEL_EDGE_OPTS),
    WF_INT("pct", "Percentual da borda ocupado (1-100)", "100", 1, 100),
    WF_INT("thickness", "Espessura (px)", "32", 4, 500),
    WF_ENUM("mode", "Modo (container = popup de um widget container)", "dock", PANEL_MODE_OPTS),
    WF_ENUM("layout", "Disposicao (so mode=container)", "row", PANEL_LAYOUT_OPTS),
    WF_ENUM("rotate", "Rotacao do conteudo (graus)", "0", PANEL_ROTATE_OPTS),
    WF_INT("tooltip_delay", "Atraso pra abrir dica (ms)", "500", 0, 10000),
    WF_INT("tooltip_close_delay", "Atraso pra fechar dica (ms)", "300", 0, 10000),
    WF_BOOL01("tooltip_reuse", "Reaproveitar janela da dica entre widgets", "1"),
    WF_INT("padding_extra", "Espacamento extra da dica (px)", "0", 0, 100),
};
#define N_PANEL_FIELDS ((int)(sizeof(PANEL_FIELDS) / sizeof(PANEL_FIELDS[0])))

/* Shared by "Adicionar painel" (iter == NULL, appends a new row on OK)
 * and double-clicking/activating an existing row (iter != NULL, updates
 * that row in place) -- see add_panel_cb()/panel_row_activated(). */
static void open_panel_dialog(GtkTreeIter *iter)
{
    gchar *cur_name = NULL, *cur_output = NULL, *cur_opts = NULL;
    if (iter) {
        gtk_tree_model_get(GTK_TREE_MODEL(g_panels_store), iter, COL_PANEL_NAME, &cur_name, COL_PANEL_OUTPUT,
                            &cur_output, COL_PANEL_OPTIONS, &cur_opts, -1);
    }

    WOptToken toks[MAX_WOPT_TOKENS];
    int n_toks = parse_wopts_tokens(cur_opts, toks, MAX_WOPT_TOKENS);

    GtkWidget *dialog = gtk_dialog_new_with_buttons(iter ? "Editar painel" : "Adicionar painel", NULL,
                                                     GTK_DIALOG_MODAL, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                     GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 440, 420);
    GtkWidget *content_area = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_box_pack_start(GTK_BOX(content_area), outer, TRUE, TRUE, 0);

    GtkWidget *top_table = gtk_table_new(2, 2, FALSE);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(name_entry), cur_name && cur_name[0] ? cur_name : "novo-painel");
    labeled_row(top_table, 0, "Nome:", name_entry);
    GtkWidget *output_combo = make_output_combo(1, cur_output && cur_output[0] ? cur_output : "*");
    labeled_row(top_table, 1, "Output:", output_combo);
    gtk_box_pack_start(GTK_BOX(outer), top_table, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(outer), gtk_hseparator_new(), FALSE, FALSE, 0);

    GtkWidget *fields_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(fields_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *fields_box = gtk_vbox_new(FALSE, 4);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(fields_scroll), fields_box);
    gtk_box_pack_start(GTK_BOX(outer), fields_scroll, TRUE, TRUE, 0);

    GtkWidget *field_widgets[N_PANEL_FIELDS];
    GtkWidget *fields_table = gtk_table_new(N_PANEL_FIELDS, 2, FALSE);
    for (int i = 0; i < N_PANEL_FIELDS; i++) {
        const char *val = n_toks > 0 ? wopts_tokens_find(toks, n_toks, PANEL_FIELDS[i].key) : NULL;
        field_widgets[i] = build_widget_field(&PANEL_FIELDS[i], val);
        labeled_row(fields_table, i, PANEL_FIELDS[i].label, field_widgets[i]);
    }
    gtk_box_pack_start(GTK_BOX(fields_box), fields_table, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gint resp = gtk_dialog_run(GTK_DIALOG(dialog));
    if (resp == GTK_RESPONSE_OK) {
        const char *new_name_text = gtk_entry_get_text(GTK_ENTRY(name_entry));
        if (new_name_text[0]) {
            char output[XIS_OUTPUT_STR_LEN];
            output_combo_value(output_combo, output, sizeof(output));

            char opts[512] = "";
            for (int i = 0; i < N_PANEL_FIELDS; i++) {
                char valbuf[256];
                widget_field_value(&PANEL_FIELDS[i], field_widgets[i], valbuf, sizeof(valbuf));
                wopts_append(opts, sizeof(opts), PANEL_FIELDS[i].key, valbuf);
            }

            if (iter) {
                rename_panel_everywhere(cur_name ? cur_name : "", new_name_text);
                if (g_theme_label) {
                    char label_text[NAME_LEN + 16];
                    snprintf(label_text, sizeof(label_text), "Outras opcoes de '%s':", g_selected_panel);
                    gtk_label_set_text(GTK_LABEL(g_theme_label), label_text);
                }
                gtk_list_store_set(g_panels_store, iter, COL_PANEL_NAME, new_name_text, COL_PANEL_OUTPUT, output,
                                    COL_PANEL_OPTIONS, opts, -1);
            } else {
                GtkTreeIter target;
                gtk_list_store_append(g_panels_store, &target);
                gtk_list_store_set(g_panels_store, &target, COL_PANEL_NAME, new_name_text, COL_PANEL_OUTPUT, output,
                                    COL_PANEL_OPTIONS, opts, -1);
            }
        }
    }

    gtk_widget_destroy(dialog);
    g_free(cur_name);
    g_free(cur_output);
    g_free(cur_opts);
}

static void add_widget_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    open_widget_dialog(NULL);
}

static void remove_widget_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_widgets_store, &it);
    }
}

/* Double-clicking (or Enter-activating) a widget row opens the same
 * dialog "Adicionar widget" does, pre-filled from that row. */
static void widget_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)view;
    (void)col;
    (void)data;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_widgets_store), &it, path)) {
        open_widget_dialog(&it);
    }
}

/* Double-clicking (or Enter-activating) a panel row opens the same
 * dialog "Adicionar painel" does, pre-filled from that row -- same
 * change as widget_row_activated() below, and for the same reason: a
 * real form (output picker + schema fields) beats editing three flat
 * text cells in place, one of which (Opcoes) was the exact same raw
 * key=value string the widget dialog already replaced for WIDGET rows. */
static void panel_row_activated(GtkTreeView *view, GtkTreePath *path, GtkTreeViewColumn *col, gpointer data)
{
    (void)view;
    (void)col;
    (void)data;
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_panels_store), &it, path)) {
        open_panel_dialog(&it);
    }
}

static GtkWidget *build_panels_view(void)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_panels_store));
    g_signal_connect(view, "row-activated", G_CALLBACK(panel_row_activated), NULL);
    const char *titles[N_PANEL_COLS] = {"Nome", "Output", "Opcoes"};
    for (int col = 0; col < N_PANEL_COLS; col++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *tvcol = gtk_tree_view_column_new_with_attributes(titles[col], renderer, "text", col, NULL);
        gtk_tree_view_column_set_expand(tvcol, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), tvcol);
    }
    return view;
}

/* Tipo/Opcoes only (no Painel column -- g_widgets_store only ever holds
 * the one panel selected above, see the file doc comment near its
 * declaration) and neither is inline-editable any more:
 * open_widget_dialog() (Adicionar widget, or double-click/Enter on a row
 * -- widget_row_activated()) is the only way to edit a row's settings
 * now, same change efeitos.c's effects list already made for the same
 * reason (a real schema-driven form beats a freeform "Opcoes" text cell).
 *
 * gtk_tree_view_set_reorderable() is what answers "make the list
 * draggable" -- it wires up GtkListStore's own GtkTreeDragSource/Dest
 * implementation (a plain store supports this natively; a
 * GtkTreeModelFilter, which an earlier version of this tab used to show
 * one panel's rows out of a store shared by every panel, does not, which
 * is the main reason g_widgets_store was narrowed to one panel at a time
 * instead -- see shelf_from_store()/shelf_to_store()). Dragging a row
 * only ever needs to move it within g_widgets_store itself; nothing else
 * has to be told a drag happened; leaving the underlying model consistent
 * for whenever it's *saved* is shelf_from_store()'s job. */
static GtkWidget *build_widgets_view(void)
{
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_widgets_store));
    gtk_tree_view_set_reorderable(GTK_TREE_VIEW(view), TRUE);
    g_signal_connect(view, "row-activated", G_CALLBACK(widget_row_activated), NULL);

    GtkCellRenderer *type_r = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Tipo", type_r, "text", COL_WIDGET_TYPE, NULL));

    GtkCellRenderer *opts_r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *opts_col =
        gtk_tree_view_column_new_with_attributes("Opcoes", opts_r, "text", COL_WIDGET_OPTIONS, NULL);
    gtk_tree_view_column_set_expand(opts_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), opts_col);

    return view;
}

/* Selecting a different Paineis row: everything below (the widgets list,
 * the theme editor) follows that panel instead of showing a flat mix of
 * every panel's own -- see the file doc comment for the ask this answers. */
static void on_panel_selection_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)data;

    /* Save whatever's currently in the theme controls back to the panel
     * that was selected *before* this change -- otherwise switching away
     * loses an unsaved edit the moment another row is clicked. */
    if (g_selected_panel[0] && g_theme_bg_field) {
        ThemeRec *r = theme_find_or_add(g_selected_panel);
        if (r) {
            theme_opts_from_fields(r->options, sizeof(r->options));
        }
    }

    /* Likewise, shelve the outgoing panel's widgets (its on-screen order
     * included) before g_widgets_store gets cleared and repopulated for
     * the incoming one below. */
    shelf_from_store(g_selected_panel);

    GtkTreeIter it;
    GtkTreeModel *model;
    if (gtk_tree_selection_get_selected(sel, &model, &it)) {
        gchar *name;
        gtk_tree_model_get(model, &it, COL_PANEL_NAME, &name, -1);
        snprintf(g_selected_panel, sizeof(g_selected_panel), "%s", name ? name : "");
        g_free(name);
    } else {
        g_selected_panel[0] = '\0';
    }

    shelf_to_store(g_selected_panel);

    if (g_theme_bg_field) {
        ThemeRec *r = g_selected_panel[0] ? theme_find(g_selected_panel) : NULL;
        sync_theme_fields_from_opts(r ? r->options : "");
    }
    if (g_theme_label) {
        char label_text[NAME_LEN + 16];
        snprintf(label_text, sizeof(label_text), "Outras opcoes de '%s':", g_selected_panel[0] ? g_selected_panel : "?");
        gtk_label_set_text(GTK_LABEL(g_theme_label), label_text);
    }
}

GtkWidget *build_paineis_tab(void)
{
    /* g_shelf/g_themes/g_selected_panel are plain C statics, not GTK
     * objects -- kiconf.c tears down and fully rebuilds a tab's *widgets*
     * on every switch (see its own file doc comment), but that discards
     * nothing here on its own, since these three survive as ordinary
     * process memory across calls. Without resetting them, a second
     * visit's shelf_find_or_add() calls below just keep appending onto
     * whatever the *previous* visit already put there instead of
     * starting clean, and worse: on_panel_selection_changed()'s very
     * first line flushes g_widgets_store into shelf_from_store(g_selected_panel)
     * using the *stale* leftover panel name from before this function
     * even reads the file -- against the brand-new, still-empty
     * g_widgets_store created further down, which wipes that panel's
     * just-loaded widgets back out to nothing. That's the bug: revisiting
     * Paineis (drag-reorder or not) left whichever panel was selected
     * when you last left showing an empty widget list the moment it (or
     * whatever panel happens to be first) gets selected again. */
    g_n_shelf = 0;
    g_n_themes = 0;
    g_selected_panel[0] = '\0';
    g_theme_bg_field = NULL;
    g_theme_fg_field = NULL;

    PanelRec panels[MAX_PANELS];
    WidgetRec widgets[MAX_WIDGETS];
    ThemeRec themes[MAX_PANELS];
    int n_panels, n_widgets, n_themes;
    load_xispanel_conf(panels, &n_panels, widgets, &n_widgets, themes, &n_themes);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *note = gtk_label_new(
        "Editor direto de xispanel.conf (PANEL/WIDGET/THEME). Opcoes sao\n"
        "a string key=value bruta de cada linha -- ver xispanel/PROTOCOL.md.");
    gtk_misc_set_alignment(GTK_MISC(note), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), note, FALSE, FALSE, 0);

    g_panels_store = gtk_list_store_new(N_PANEL_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    for (int i = 0; i < n_panels; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_panels_store, &it);
        gtk_list_store_set(g_panels_store, &it,
                            COL_PANEL_NAME, panels[i].name, COL_PANEL_OUTPUT, panels[i].output,
                            COL_PANEL_OPTIONS, panels[i].options, -1);
    }
    GtkWidget *panels_view = build_panels_view();
    GtkTreeSelection *panels_sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(panels_view));
    GtkWidget *panels_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(panels_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(panels_scroll, -1, 100);
    gtk_container_add(GTK_CONTAINER(panels_scroll), panels_view);
    GtkWidget *panels_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_scroll, TRUE, TRUE, 0);
    GtkWidget *panels_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *padd = gtk_button_new_with_label("Adicionar painel");
    GtkWidget *prem = gtk_button_new_with_label("Remover painel");
    g_signal_connect(padd, "clicked", G_CALLBACK(add_panel_cb), NULL);
    g_signal_connect(prem, "clicked", G_CALLBACK(remove_panel_cb), panels_view);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), padd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_btnbox), prem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(panels_box), panels_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Paineis", panels_box), FALSE, FALSE, 0);

    /* g_widgets_store only ever holds one panel's widgets at a time (see
     * its own doc comment) -- every panel's widgets loaded from the file
     * go onto the shelf first; shelf_to_store() below then populates the
     * actual store for whichever panel ends up selected. Grouping by
     * panel here preserves each panel's own relative file order, since
     * `widgets[]` is already in file-encounter order and shelf_find_or_add()
     * only ever appends. */
    for (int i = 0; i < n_widgets; i++) {
        PanelShelf *sh = shelf_find_or_add(widgets[i].panel);
        if (sh && sh->n_entries < MAX_WIDGETS) {
            WidgetEntry *e = &sh->entries[sh->n_entries++];
            snprintf(e->type, sizeof(e->type), "%s", widgets[i].type);
            snprintf(e->options, sizeof(e->options), "%s", widgets[i].options);
        }
    }
    for (int i = 0; i < n_themes; i++) {
        ThemeRec *r = theme_find_or_add(themes[i].panel);
        if (r) {
            snprintf(r->options, sizeof(r->options), "%s", themes[i].options);
        }
    }

    g_widgets_store = gtk_list_store_new(N_WIDGET_COLS, G_TYPE_STRING, G_TYPE_STRING);
    g_signal_connect(panels_sel, "changed", G_CALLBACK(on_panel_selection_changed), NULL);

    /* Select the first panel (if any), which fires on_panel_selection_changed()
     * above and populates g_widgets_store for it via shelf_to_store() --
     * otherwise the widgets list would show nothing at all until the user
     * clicks a Paineis row themselves. */
    if (n_panels > 0) {
        GtkTreeIter first;
        if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_panels_store), &first)) {
            gtk_tree_selection_select_iter(panels_sel, &first);
        }
    }

    GtkWidget *widgets_view = build_widgets_view();
    GtkWidget *widgets_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(widgets_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(widgets_scroll, -1, 180);
    gtk_container_add(GTK_CONTAINER(widgets_scroll), widgets_view);
    GtkWidget *widgets_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_scroll, TRUE, TRUE, 0);
    GtkWidget *widgets_btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *wadd = gtk_button_new_with_label("Adicionar widget");
    GtkWidget *wrem = gtk_button_new_with_label("Remover widget");
    g_signal_connect(wadd, "clicked", G_CALLBACK(add_widget_cb), NULL);
    g_signal_connect(wrem, "clicked", G_CALLBACK(remove_widget_cb), widgets_view);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wadd, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_btnbox), wrem, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(widgets_box), widgets_btnbox, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Widgets do painel selecionado", widgets_box), TRUE, TRUE, 0);

    /* Theme now follows whichever panel is selected above (see
     * on_panel_selection_changed()) instead of always being "the first
     * panel with a THEME line" -- these controls are updated live on
     * every selection change; xispanel still defaults to the live system
     * theme when a panel has no THEME line at all (see PROTOCOL.md), so
     * most panels simply leave this blank. */
    GtkWidget *theme_table = gtk_table_new(4, 2, FALSE);
    GtkWidget *bg_label = gtk_label_new("Cor de fundo:");
    gtk_misc_set_alignment(GTK_MISC(bg_label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(theme_table), bg_label, 0, 1, 0, 1, GTK_FILL, GTK_FILL, 4, 3);
    g_theme_bg_box = gtk_hbox_new(FALSE, 0);
    gtk_table_attach(GTK_TABLE(theme_table), g_theme_bg_box, 1, 2, 0, 1, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);

    GtkWidget *fg_label = gtk_label_new("Cor do texto:");
    gtk_misc_set_alignment(GTK_MISC(fg_label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(theme_table), fg_label, 0, 1, 1, 2, GTK_FILL, GTK_FILL, 4, 3);
    g_theme_fg_box = gtk_hbox_new(FALSE, 0);
    gtk_table_attach(GTK_TABLE(theme_table), g_theme_fg_box, 1, 2, 1, 2, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);

    g_theme_spacing_spin = gtk_spin_button_new_with_range(THEME_SPACING_FIELD.min, THEME_SPACING_FIELD.max, 1);
    labeled_row(theme_table, 2, "Espacamento entre widgets (px):", g_theme_spacing_spin);

    g_theme_options_entry = gtk_entry_new();
    ThemeRec *initial_theme = g_selected_panel[0] ? theme_find(g_selected_panel) : NULL;
    char theme_label_text[NAME_LEN + 16];
    snprintf(theme_label_text, sizeof(theme_label_text), "Outras opcoes de '%s':", g_selected_panel[0] ? g_selected_panel : "?");
    g_theme_label = gtk_label_new(theme_label_text);
    gtk_misc_set_alignment(GTK_MISC(g_theme_label), 0.0, 0.5);
    gtk_table_attach(GTK_TABLE(theme_table), g_theme_label, 0, 1, 3, 4, GTK_FILL, GTK_FILL, 4, 3);
    gtk_table_attach(GTK_TABLE(theme_table), g_theme_options_entry, 1, 2, 3, 4, GTK_EXPAND | GTK_FILL, GTK_FILL, 4, 3);
    gtk_box_pack_start(GTK_BOX(outer), frame_with("Tema (cores/fonte do painel selecionado)", theme_table), FALSE, FALSE, 0);
    sync_theme_fields_from_opts(initial_theme ? initial_theme->options : "");

    GtkWidget *save_btn = gtk_button_new_with_label("Salvar e recarregar xispanel");
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_panels_cb), NULL);
    GtkWidget *save_btnbox = gtk_hbox_new(FALSE, 0);
    gtk_box_pack_end(GTK_BOX(save_btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), save_btnbox, FALSE, FALSE, 0);

    return outer;
}

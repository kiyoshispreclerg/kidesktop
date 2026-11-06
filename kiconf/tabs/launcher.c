/* kiconf - Lancador tab: the xisserve launcher's options in xisserve.conf.
 *
 * Search plugins on/off, the icon grid's column count and the launch
 * feedback toggle. xisserve.conf is written by hand too (tab-separated
 * "SECTION<TAB>key<TAB>value" lines, with comments), so Aplicar edits only
 * the lines this tab owns -- PLUGIN <name>, LAUNCHER grid_columns and
 * LAUNCHER launch_feedback -- and leaves everything else as it was. A
 * setting at its default is not written at all (its line is removed).
 *
 * The plugin list is fixed: xisserve's search plugins are compiled into its
 * binary (kSearchPlugins), unlike its pages, which can be installed as .so.
 * Keep SearchPlugins[] in step with that table. xisserve re-reads the file
 * on every open, so nothing needs restarting. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_safefile.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GRID_COLUMNS_DEFAULT 4

typedef struct {
    const char *name;   /* the PLUGIN <name> key */
    const char *label;
    const char *tip;
} SearchPluginInfo;

static const SearchPluginInfo SearchPlugins[] = {
    {"terminal", N_("Executar comando"),
     N_("Se a primeira palavra da busca for um programa do PATH, oferece executar o comando no terminal da sessao.")},
    {"globalmenu", N_("Menu da janela ativa"),
     N_("Procura o texto nos itens do menu global da janela ativa, em todos os submenus (estilo HUD).")},
    {"recent", N_("Arquivos recentes"), N_("Procura o texto na lista de arquivos recentes e abre o escolhido.")},
    {"kistory", N_("Historico de atividade"),
     N_("Procura arquivos abertos e titulos de janelas no historico do kistory (3 letras ou mais).")},
    {"process", N_("Processos"),
     N_("Se o texto for o nome de um processo seu, oferece finalizar, matar ou matar a arvore.")},
    {"calculator", N_("Calculadora"), N_("Se a busca for uma expressao aritmetica, mostra o resultado.")},
};
#define N_PLUGINS ((int)(sizeof(SearchPlugins) / sizeof(SearchPlugins[0])))

static GtkWidget *g_plugin_chk[N_PLUGINS];
static GtkWidget *g_cols_spin, *g_feedback_chk, *g_status;

/* ---- xisserve.conf: line-preserving read/edit ------------------------- */

/* The value of "<section>\t<key>\t<value>" in `text`, or NULL (g_free). */
static char *conf_get(const char *text, const char *section, const char *key)
{
    char *prefix = g_strdup_printf("%s\t%s\t", section, key);
    char *found = NULL;
    for (const char *p = text; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > strlen(prefix) && !strncmp(p, prefix, strlen(prefix))) {
            found = g_strndup(p + strlen(prefix), len - strlen(prefix));
            g_strstrip(found);
        }
        p = nl ? nl + 1 : NULL;
    }
    g_free(prefix);
    return found;
}

/* Sets "<section>\t<key>" to `value` in the line list (replacing the first
 * match, dropping any later duplicate), or removes it when value is NULL. */
static void conf_set(GPtrArray *lines, const char *section, const char *key, const char *value)
{
    char *prefix = g_strdup_printf("%s\t%s\t", section, key);
    int placed = 0;
    for (guint i = 0; i < lines->len;) {
        char *l = g_ptr_array_index(lines, i);
        if (strncmp(l, prefix, strlen(prefix)) != 0) {
            i++;
            continue;
        }
        if (value && !placed) {
            g_free(l);
            lines->pdata[i] = g_strdup_printf("%s%s", prefix, value);
            placed = 1;
            i++;
        } else {
            g_ptr_array_remove_index(lines, i);   /* the array frees the line */
        }
    }
    if (value && !placed)
        g_ptr_array_add(lines, g_strdup_printf("%s%s", prefix, value));
    g_free(prefix);
}

/* ---- load / apply ----------------------------------------------------- */

static void apply_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    char path[PATH_MAX];
    resolve_path("xisserve.conf", path, sizeof(path));

    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    size_t len = 0;
    char *text = xis_read_file_or_backup(path, xis_check_usable, &len);
    if (text) {
        char **split = g_strsplit(text, "\n", -1);
        int n = g_strv_length(split);
        if (n > 0 && !split[n - 1][0])
            n--;   /* the text's final newline, not an empty line */
        for (int i = 0; i < n; i++)
            g_ptr_array_add(lines, g_strdup(split[i]));
        g_strfreev(split);
        free(text);
    }

    for (int i = 0; i < N_PLUGINS; i++) {
        gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_plugin_chk[i]));
        conf_set(lines, "PLUGIN", SearchPlugins[i].name, on ? NULL : "no");
    }
    int cols = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(g_cols_spin));
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", cols);
    conf_set(lines, "LAUNCHER", "grid_columns", cols == GRID_COLUMNS_DEFAULT ? NULL : buf);
    conf_set(lines, "LAUNCHER", "launch_feedback",
             gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(g_feedback_chk)) ? NULL : "no");

    char tmp[PATH_MAX + 8];
    xis_write_tmpname(path, tmp, sizeof(tmp));
    FILE *out = xis_write_open(tmp, path);
    int ok = out != NULL;
    if (out) {
        for (guint i = 0; i < lines->len; i++)
            fprintf(out, "%s\n", (char *)g_ptr_array_index(lines, i));
        ok = xis_write_commit(out, tmp, path, xis_check_usable) == 0;
    }
    g_ptr_array_free(lines, TRUE);
    if (!ok) {
        gtk_label_set_text(GTK_LABEL(g_status), _("Nao foi possivel salvar xisserve.conf"));
        return;
    }
    gtk_label_set_text(GTK_LABEL(g_status), _("Aplicado (vale na proxima abertura do lancador)"));
    kiconf_track_applied(g_status);
}

GtkWidget *build_launcher_tab(void)
{
    char path[PATH_MAX];
    resolve_path("xisserve.conf", path, sizeof(path));
    size_t len = 0;
    char *text = xis_read_file_or_backup(path, xis_check_usable, &len);

    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *plugins = gtk_vbox_new(FALSE, 4);
    GtkWidget *hint = gtk_label_new(_("Cada plugin acrescenta resultados proprios a busca do lancador, alem dos programas."));
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_misc_set_alignment(GTK_MISC(hint), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(plugins), hint, FALSE, FALSE, 0);
    for (int i = 0; i < N_PLUGINS; i++) {
        char *v = text ? conf_get(text, "PLUGIN", SearchPlugins[i].name) : NULL;
        g_plugin_chk[i] = gtk_check_button_new_with_label(_(SearchPlugins[i].label));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_plugin_chk[i]), !v || strcasecmp(v, "no") != 0);
        gtk_widget_set_tooltip_text(g_plugin_chk[i], _(SearchPlugins[i].tip));
        gtk_box_pack_start(GTK_BOX(plugins), g_plugin_chk[i], FALSE, FALSE, 0);
        g_free(v);
    }
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Plugins de busca"), plugins), FALSE, FALSE, 0);

    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    char *cv = text ? conf_get(text, "LAUNCHER", "grid_columns") : NULL;
    int cols = cv ? atoi(cv) : GRID_COLUMNS_DEFAULT;
    g_free(cv);
    if (cols < 1)
        cols = GRID_COLUMNS_DEFAULT;
    g_cols_spin = gtk_spin_button_new_with_range(1, 12, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(g_cols_spin), cols);
    labeled_row(table, 0, _("Colunas na grade:"), g_cols_spin);
    gtk_widget_set_tooltip_text(g_cols_spin, _("Quantos icones por linha na vista em grade dos resultados"));

    char *fv = text ? conf_get(text, "LAUNCHER", "launch_feedback") : NULL;
    g_feedback_chk = gtk_check_button_new();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(g_feedback_chk), !fv || strcasecmp(fv, "no") != 0);
    labeled_row(table, 1, _("Animar icone ao lancar:"), g_feedback_chk);
    gtk_widget_set_tooltip_text(g_feedback_chk,
        _("Zoom e fade sobre o icone do item lancado. Ampliacao e duracao ficam em Efeitos do compositor. Precisa de compositor."));
    g_free(fv);
    free(text);
    gtk_box_pack_start(GTK_BOX(outer), frame_with(_("Resultados"), table), FALSE, FALSE, 0);

    GtkWidget *btns = gtk_hbox_new(FALSE, 6);
    g_status = gtk_label_new("");
    GtkWidget *apply = gtk_button_new_with_label(_("Aplicar"));
    g_signal_connect(apply, "clicked", G_CALLBACK(apply_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btns), g_status, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(btns), apply, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btns, FALSE, FALSE, 0);
    return outer;
}

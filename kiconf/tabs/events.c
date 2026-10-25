/* kiconf - Eventos tab: the user's own calendar events/reminders.
 * See kiconf.c's top doc comment for the overall design.
 *
 * Edits ki-events.conf directly, one "EVENT\t<date>\t<recurrence>\t
 * <name>\t<description>" line per row -- same tab-separated, full-
 * rewrite-on-save shape as Atalhos' own xiskeys.conf editor
 * (tabs/shortcuts.c), just four columns instead of three and a combo
 * cell for Recorrencia instead of free text.
 *
 * This is also the only writer of ki-events.conf: xisserve's
 * --calendar page (pages/calendar.c) only ever reads it, and its
 * "Abrir eventos..." button is exactly `kiconf --tab Eventos`. No
 * daemon to signal on save either -- that page re-reads the file every
 * time it's shown, there's nothing running in the background to nudge. */
#include "../common.h"
#include "../tabs.h"
#include "../../shared/xis_holidays.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { COL_EV_DATE = 0, COL_EV_RECUR, COL_EV_NAME, COL_EV_DESC, N_EV_COLS };

static GtkListStore *g_events_store;
static GtkWidget *g_events_status_label;

/* The only recurrence values pages/calendar.c's event_occurs() knows
 * about -- kept in sync with that table by hand, same as every other
 * "catalog mirrored across the two projects" spot in kiconf (see
 * tabs/shortcuts.c's own KIWM_CATALOG comment). NONE first so it's the
 * default a freshly-added row gets. */
static const char *const RECUR_OPTIONS[] = {"NONE", "YEARLY", "MONTHLY", "WEEKLY", "DAILY", NULL};

static void load_events(void)
{
    gtk_list_store_clear(g_events_store);

    char path[PATH_MAX];
    resolve_path("ki-events.conf", path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f) {
        return;
    }
    char line[1024];
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
        char *date = tag ? strtok_r(NULL, "\t", &save) : NULL;
        char *recur = date ? strtok_r(NULL, "\t", &save) : NULL;
        char *name = recur ? strtok_r(NULL, "\t", &save) : NULL;
        char *desc = name ? strtok_r(NULL, "", &save) : NULL;
        if (!tag || strcmp(tag, "EVENT") != 0 || !date || !recur || !name) {
            continue;
        }
        GtkTreeIter it;
        gtk_list_store_append(g_events_store, &it);
        gtk_list_store_set(g_events_store, &it,
                            COL_EV_DATE, date, COL_EV_RECUR, recur, COL_EV_NAME, name,
                            COL_EV_DESC, desc ? desc : "", -1);
    }
    fclose(f);
}

static void save_events_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;

    char path[PATH_MAX];
    resolve_path("ki-events.conf", path, sizeof(path));
    char tmp[PATH_MAX + 8]; /* path + ".tmp" */
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        g_warning("kiconf: could not write '%s': %s", tmp, strerror(errno));
        return;
    }
    fprintf(f, "# ki-events.conf -- one event per line:\n");
    fprintf(f, "#   EVENT\\t<YYYY-MM-DD>\\t<NONE|YEARLY|MONTHLY|WEEKLY|DAILY>\\t<name>\\t<description>\n");

    GtkTreeIter it;
    gboolean valid = gtk_tree_model_get_iter_first(GTK_TREE_MODEL(g_events_store), &it);
    while (valid) {
        gchar *date, *recur, *name, *desc;
        gtk_tree_model_get(GTK_TREE_MODEL(g_events_store), &it,
                            COL_EV_DATE, &date, COL_EV_RECUR, &recur,
                            COL_EV_NAME, &name, COL_EV_DESC, &desc, -1);
        if (date && *date && name && *name) {
            fprintf(f, "EVENT\t%s\t%s\t%s\t%s\n", date, (recur && *recur) ? recur : "NONE", name,
                    desc ? desc : "");
        }
        g_free(date);
        g_free(recur);
        g_free(name);
        g_free(desc);
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(g_events_store), &it);
    }
    fclose(f);
    rename(tmp, path);

    gtk_label_set_text(GTK_LABEL(g_events_status_label), _("Gravado."));
}

static void add_event_cb(GtkWidget *widget, gpointer data)
{
    (void)widget;
    (void)data;
    GDateTime *now = g_date_time_new_now_local();
    char today[16];
    snprintf(today, sizeof(today), "%04d-%02d-%02d", g_date_time_get_year(now),
             g_date_time_get_month(now), g_date_time_get_day_of_month(now));
    g_date_time_unref(now);

    GtkTreeIter it;
    gtk_list_store_append(g_events_store, &it);
    gtk_list_store_set(g_events_store, &it,
                        COL_EV_DATE, today, COL_EV_RECUR, "NONE", COL_EV_NAME, "Novo evento",
                        COL_EV_DESC, "", -1);
}

static void remove_event_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    (void)widget;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeIter it;
    if (gtk_tree_selection_get_selected(sel, NULL, &it)) {
        gtk_list_store_remove(g_events_store, &it);
    }
}

static void event_cell_edited(GtkCellRendererText *cell, gchar *path_str, gchar *new_text, gpointer data)
{
    (void)cell;
    gint col = GPOINTER_TO_INT(data);
    GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
    GtkTreeIter it;
    if (gtk_tree_model_get_iter(GTK_TREE_MODEL(g_events_store), &it, path)) {
        gtk_list_store_set(g_events_store, &it, col, new_text, -1);
    }
    gtk_tree_path_free(path);
}

/* Recorrencia's cell is a dropdown-only combo (has-entry=FALSE) over
 * RECUR_OPTIONS rather than free text, since -- unlike Nome/Descricao --
 * anything outside that fixed set is silently treated as NONE by
 * pages/calendar.c's own parser, so typos would just look ignored. */
static GtkTreeViewColumn *make_recur_column(void)
{
    GtkListStore *options = gtk_list_store_new(1, G_TYPE_STRING);
    for (int i = 0; RECUR_OPTIONS[i]; i++) {
        GtkTreeIter it;
        gtk_list_store_append(options, &it);
        gtk_list_store_set(options, &it, 0, RECUR_OPTIONS[i], -1);
    }

    GtkCellRenderer *renderer = gtk_cell_renderer_combo_new();
    g_object_set(renderer, "editable", TRUE, "model", options, "text-column", 0,
                 "has-entry", FALSE, NULL);
    g_object_unref(options);
    g_signal_connect(renderer, "edited", G_CALLBACK(event_cell_edited), GINT_TO_POINTER(COL_EV_RECUR));

    return gtk_tree_view_column_new_with_attributes("Recorrencia", renderer, "text", COL_EV_RECUR, NULL);
}

/* "Ver feriados": a plain read-only list of the public holidays of the
 * country the system timezone belongs to, for the chosen year. The lists are
 * data files (see shared/xis_holidays.h), not part of this program. */
static GtkListStore *g_hol_store;
static GtkWidget *g_hol_title;

static void fill_holidays(int year)
{
    char country[8];
    xis_holidays_country(country, sizeof(country));
    XisHoliday list[64];
    int n = xis_holidays_for_year(country, year, list, 64);
    gtk_list_store_clear(g_hol_store);
    for (int i = 0; i < n; i++) {
        struct tm tmv = {0};
        tmv.tm_year = year - 1900;
        tmv.tm_mon = list[i].month - 1;
        tmv.tm_mday = list[i].day;
        tmv.tm_hour = 12;
        mktime(&tmv);
        char date[32], wday[32];
        strftime(date, sizeof(date), "%d/%m", &tmv);
        strftime(wday, sizeof(wday), "%a", &tmv);
        GtkTreeIter it;
        gtk_list_store_append(g_hol_store, &it);
        gtk_list_store_set(g_hol_store, &it, 0, date, 1, wday, 2, list[i].name, -1);
    }
    char *title;
    if (!country[0]) {
        title = g_strdup(_("Pais desconhecido para este fuso horario."));
    } else if (n == 0) {
        title = g_strdup_printf(_("Nenhuma lista de feriados instalada para %s."), country);
    } else {
        title = g_strdup_printf(_("Feriados de %s"), country);
    }
    gtk_label_set_text(GTK_LABEL(g_hol_title), title);
    g_free(title);
}

static void holiday_year_changed(GtkSpinButton *spin, gpointer data)
{
    (void)data;
    fill_holidays(gtk_spin_button_get_value_as_int(spin));
}

static void show_holidays_cb(GtkWidget *widget, gpointer data)
{
    (void)data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Feriados"), GTK_WINDOW(gtk_widget_get_toplevel(widget)),
                                                 GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                                 GTK_STOCK_CLOSE, GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dlg), 340, 380);
    GtkWidget *box = GTK_DIALOG(dlg)->vbox;
    gtk_box_set_spacing(GTK_BOX(box), 6);

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    GtkWidget *row = gtk_hbox_new(FALSE, 6);
    g_hol_title = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_hol_title), 0.0, 0.5);
    GtkWidget *year = gtk_spin_button_new_with_range(1970, 2100, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(year), lt.tm_year + 1900);
    a11y_name(year, _("Ano"));
    gtk_box_pack_start(GTK_BOX(row), g_hol_title, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(row), year, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    g_hol_store = gtk_list_store_new(3, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_hol_store));
    a11y_name(view, _("Feriados"));
    const char *titles[3] = {_("Data"), _("Dia"), _("Feriado")};
    for (int i = 0; i < 3; i++) {
        GtkTreeViewColumn *col = gtk_tree_view_column_new_with_attributes(titles[i], gtk_cell_renderer_text_new(),
                                                                          "text", i, NULL);
        gtk_tree_view_column_set_expand(col, i == 2);
        gtk_tree_view_append_column(GTK_TREE_VIEW(view), col);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);

    g_signal_connect(year, "value-changed", G_CALLBACK(holiday_year_changed), NULL);
    fill_holidays(lt.tm_year + 1900);
    gtk_widget_show_all(dlg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_hol_store = NULL;
}

GtkWidget *build_events_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);

    GtkWidget *info = gtk_label_new(
        "Eventos e feriados pessoais mostrados na aba Calendario do xisserve. "
        "Data no formato AAAA-MM-DD -- para eventos recorrentes ela e so a data "
        "da primeira ocorrencia (o ano e ignorado exceto em NONE/YEARLY).");
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    gtk_label_set_line_wrap(GTK_LABEL(info), TRUE);
    gtk_box_pack_start(GTK_BOX(outer), info, FALSE, FALSE, 0);

    g_events_store = gtk_list_store_new(N_EV_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(g_events_store));
    a11y_name(view, _("Eventos"));

    GtkCellRenderer *date_r = gtk_cell_renderer_text_new();
    g_object_set(date_r, "editable", TRUE, NULL);
    g_signal_connect(date_r, "edited", G_CALLBACK(event_cell_edited), GINT_TO_POINTER(COL_EV_DATE));
    gtk_tree_view_append_column(GTK_TREE_VIEW(view),
        gtk_tree_view_column_new_with_attributes("Data", date_r, "text", COL_EV_DATE, NULL));

    gtk_tree_view_append_column(GTK_TREE_VIEW(view), make_recur_column());

    GtkCellRenderer *name_r = gtk_cell_renderer_text_new();
    g_object_set(name_r, "editable", TRUE, NULL);
    g_signal_connect(name_r, "edited", G_CALLBACK(event_cell_edited), GINT_TO_POINTER(COL_EV_NAME));
    GtkTreeViewColumn *name_col = gtk_tree_view_column_new_with_attributes("Nome", name_r, "text", COL_EV_NAME, NULL);
    gtk_tree_view_column_set_expand(name_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), name_col);

    GtkCellRenderer *desc_r = gtk_cell_renderer_text_new();
    g_object_set(desc_r, "editable", TRUE, NULL);
    g_signal_connect(desc_r, "edited", G_CALLBACK(event_cell_edited), GINT_TO_POINTER(COL_EV_DESC));
    GtkTreeViewColumn *desc_col = gtk_tree_view_column_new_with_attributes("Descricao", desc_r, "text", COL_EV_DESC, NULL);
    gtk_tree_view_column_set_expand(desc_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(view), desc_col);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(scroll, -1, 260);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    g_events_status_label = gtk_label_new("");
    gtk_misc_set_alignment(GTK_MISC(g_events_status_label), 0.0, 0.5);
    gtk_box_pack_start(GTK_BOX(outer), g_events_status_label, FALSE, FALSE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 6);
    GtkWidget *add_btn = gtk_button_new_with_label(_("Adicionar"));
    GtkWidget *remove_btn = gtk_button_new_with_label(_("Remover"));
    GtkWidget *save_btn = gtk_button_new_with_label(_("Salvar"));
    GtkWidget *holidays_btn = gtk_button_new_with_label(_("Ver feriados..."));
    g_signal_connect(holidays_btn, "clicked", G_CALLBACK(show_holidays_cb), NULL);
    g_signal_connect(add_btn, "clicked", G_CALLBACK(add_event_cb), NULL);
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(remove_event_cb), view);
    g_signal_connect(save_btn, "clicked", G_CALLBACK(save_events_cb), NULL);
    gtk_box_pack_start(GTK_BOX(btnbox), add_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), remove_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(btnbox), holidays_btn, FALSE, FALSE, 12);
    gtk_box_pack_end(GTK_BOX(btnbox), save_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    load_events();

    return outer;
}

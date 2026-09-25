/* kiconf - Associacoes de arquivos tab: which installed app opens each
 * file extension. See kiconf.c's top doc comment for the overall design.
 *
 * This is a friendlier, much wider version of Programas Padrao's 4 fixed
 * category rows: every extension known to the system's shared-mime-info
 * database (/usr/share/mime/globs) gets its own row, with a search box
 * since there are hundreds of them. Only simple "*.ext" glob patterns are
 * shown (a compound one like "*.tar.gz" or an extension-less one like
 * "Makefile*" wouldn't mean anything to a "type an extension" search box),
 * same simplification tradeoff as picking this view over a raw MIME-type
 * list.
 *
 * Reading/writing the actual default is the same standard mechanism
 * Programas Padrao uses (`xdg-mime query default` / `xdg-mime default`,
 * see common.c's mime_query_default() and programas.c's own doc comment)
 * -- nothing kiconf-specific, applies immediately, one row at a time. */
#include "../common.h"
#include "../tabs.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char ext[32];       /* ".pdf" */
    char mimetype[128]; /* "application/pdf" */
    char label[256];    /* human description, or mimetype as fallback */
} AssocEntry;

#define MAX_ASSOC 2048
static AssocEntry g_assoc[MAX_ASSOC];
static int g_n_assoc = 0;

static DesktopApp g_apps[MAX_DESKTOP_APPS];
static int g_n_apps = 0;

static GtkListStore *g_assoc_store;
static GtkTreeModelFilter *g_assoc_filter;
static GtkWidget *g_assoc_view;
static GtkWidget *g_search_entry;
enum { COL_A_EXT = 0, COL_A_LABEL, COL_A_APP, COL_A_ROW, N_A_COLS };

/* First bare <comment>...</comment> (no xml:lang=) in a shared-mime-info
 * type description file -- good enough for "PDF document" without
 * pulling in an XML parser, same spirit as desktop_entry_get(). */
static int mime_description(const char *mimetype, char *out, size_t outsz)
{
    char media[64], subtype[64];
    const char *slash = strchr(mimetype, '/');
    if (!slash) {
        return 0;
    }
    size_t medlen = (size_t)(slash - mimetype);
    if (medlen >= sizeof(media)) {
        return 0;
    }
    memcpy(media, mimetype, medlen);
    media[medlen] = '\0';
    snprintf(subtype, sizeof(subtype), "%s", slash + 1);

    char path[512];
    snprintf(path, sizeof(path), "/usr/share/mime/%s/%s.xml", media, subtype);
    FILE *f = fopen(path, "r");
    if (!f) {
        return 0;
    }
    char buf[8192];
    size_t total = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[total] = '\0';

    char *p = buf;
    while ((p = strstr(p, "<comment"))) {
        char *tagend = strchr(p, '>');
        if (!tagend) {
            break;
        }
        if (!strstr(p, "xml:lang=") || strstr(p, "xml:lang=") > tagend) {
            char *start = tagend + 1;
            char *end = strstr(start, "</comment>");
            if (end) {
                size_t len = (size_t)(end - start);
                if (len >= outsz) {
                    len = outsz - 1;
                }
                memcpy(out, start, len);
                out[len] = '\0';
                return 1;
            }
        }
        p = tagend + 1;
    }
    return 0;
}

static int assoc_has_ext(const char *ext)
{
    for (int i = 0; i < g_n_assoc; i++) {
        if (!strcmp(g_assoc[i].ext, ext)) {
            return 1;
        }
    }
    return 0;
}

static void scan_globs(void)
{
    g_n_assoc = 0;
    FILE *f = fopen("/usr/share/mime/globs", "r");
    if (!f) {
        return;
    }
    char line[512];
    while (g_n_assoc < MAX_ASSOC && fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) {
            *nl = '\0';
        }
        if (line[0] == '#' || !line[0]) {
            continue;
        }
        char *colon = strchr(line, ':');
        if (!colon) {
            continue;
        }
        *colon = '\0';
        const char *mimetype = line;
        const char *pattern = colon + 1;

        /* Only plain "*.ext" (a single dot-run after the star, no
         * further wildcard/dot games) -- see file doc comment. */
        if (pattern[0] != '*' || pattern[1] != '.') {
            continue;
        }
        const char *ext = pattern + 1; /* includes the leading '.' */
        if (!ext[1] || strpbrk(ext + 1, "*?[.")) {
            continue;
        }
        if (assoc_has_ext(ext)) {
            continue; /* first (highest-priority) mimetype for this ext wins */
        }

        AssocEntry *a = &g_assoc[g_n_assoc++];
        snprintf(a->ext, sizeof(a->ext), "%s", ext);
        snprintf(a->mimetype, sizeof(a->mimetype), "%s", mimetype);
        if (!mime_description(mimetype, a->label, sizeof(a->label))) {
            snprintf(a->label, sizeof(a->label), "%s", mimetype);
        }
    }
    fclose(f);
}

static int assoc_cmp(const void *pa, const void *pb)
{
    return strcmp(((const AssocEntry *)pa)->ext, ((const AssocEntry *)pb)->ext);
}

static int app_has_mimetype(const DesktopApp *app, const char *mimetype)
{
    const char *p = app->mimetypes;
    size_t len = strlen(mimetype);
    while (*p) {
        const char *semi = strchr(p, ';');
        size_t tlen = semi ? (size_t)(semi - p) : strlen(p);
        if (tlen == len && !strncmp(p, mimetype, len)) {
            return 1;
        }
        if (!semi) {
            break;
        }
        p = semi + 1;
    }
    return 0;
}

static const char *app_name_by_id(const char *id)
{
    for (int i = 0; i < g_n_apps; i++) {
        if (!strcmp(g_apps[i].id, id)) {
            return g_apps[i].name;
        }
    }
    return id;
}

static void refill_assoc_row(GtkTreeIter *it, int row)
{
    char current[NAME_LEN];
    mime_query_default(g_assoc[row].mimetype, current, sizeof(current));
    gtk_list_store_set(g_assoc_store, it, COL_A_APP, current[0] ? app_name_by_id(current) : "(nenhum)", -1);
}

static void refill_assoc_store(void)
{
    scan_globs();
    qsort(g_assoc, (size_t)g_n_assoc, sizeof(AssocEntry), assoc_cmp);
    g_n_apps = scan_all_apps(g_apps, MAX_DESKTOP_APPS);

    gtk_list_store_clear(g_assoc_store);
    for (int i = 0; i < g_n_assoc; i++) {
        GtkTreeIter it;
        gtk_list_store_append(g_assoc_store, &it);
        gtk_list_store_set(g_assoc_store, &it, COL_A_EXT, g_assoc[i].ext, COL_A_LABEL, g_assoc[i].label, COL_A_ROW, i,
                            -1);
        refill_assoc_row(&it, i);
    }
}

/* Case-insensitive substring search without relying on the GNU-only
 * strcasestr() -- kiconf's other case-insensitive compares stick to
 * plain glib (g_ascii_strcasecmp), nothing here pulls in _GNU_SOURCE. */
static int contains_ci(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack), nlen = strlen(needle);
    if (nlen == 0) {
        return 1;
    }
    for (size_t i = 0; i + nlen <= hlen; i++) {
        if (!g_ascii_strncasecmp(haystack + i, needle, nlen)) {
            return 1;
        }
    }
    return 0;
}

static gboolean assoc_filter_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)data;
    const gchar *needle = gtk_entry_get_text(GTK_ENTRY(g_search_entry));
    if (!needle || !*needle) {
        return TRUE;
    }
    gchar *ext, *label;
    gtk_tree_model_get(model, iter, COL_A_EXT, &ext, COL_A_LABEL, &label, -1);
    gboolean visible = (ext && contains_ci(ext, needle)) || (label && contains_ci(label, needle));
    g_free(ext);
    g_free(label);
    return visible;
}

static void search_changed_cb(GtkWidget *w, gpointer data)
{
    (void)w;
    (void)data;
    gtk_tree_model_filter_refilter(g_assoc_filter);
}

static void change_app_cb(GtkWidget *widget, gpointer data)
{
    GtkTreeView *view = GTK_TREE_VIEW(data);
    GtkWidget *window = gtk_widget_get_toplevel(widget);
    GtkTreeSelection *sel = gtk_tree_view_get_selection(view);
    GtkTreeModel *model;
    GtkTreeIter filter_it, it;
    if (!gtk_tree_selection_get_selected(sel, &model, &filter_it)) {
        return;
    }
    gtk_tree_model_filter_convert_iter_to_child_iter(g_assoc_filter, &it, &filter_it);

    gint row;
    gtk_tree_model_get(GTK_TREE_MODEL(g_assoc_store), &it, COL_A_ROW, &row, -1);
    if (row < 0 || row >= g_n_assoc) {
        return;
    }
    AssocEntry *entry = &g_assoc[row];

    char current[NAME_LEN];
    mime_query_default(entry->mimetype, current, sizeof(current));

    GtkWidget *dialog = gtk_dialog_new_with_buttons("Alterar programa padrao", GTK_WINDOW(window), GTK_DIALOG_MODAL,
                                                     GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, GTK_STOCK_OK,
                                                     GTK_RESPONSE_OK, NULL);
    GtkWidget *table = gtk_table_new(2, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(table), 8);
    GtkWidget *info = gtk_label_new(entry->ext);
    gtk_misc_set_alignment(GTK_MISC(info), 0.0, 0.5);
    labeled_row(table, 0, "Extensao:", info);

    GtkWidget *combo = gtk_combo_box_new_text();
    char ids[MAX_DESKTOP_APPS][NAME_LEN];
    int n_ids = 0;
    int active = -1;
    for (int i = 0; i < g_n_apps; i++) {
        if (!app_has_mimetype(&g_apps[i], entry->mimetype)) {
            continue;
        }
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), g_apps[i].name);
        snprintf(ids[n_ids], NAME_LEN, "%s", g_apps[i].id);
        if (!strcmp(g_apps[i].id, current)) {
            active = n_ids;
        }
        n_ids++;
    }
    if (active < 0 && current[0] && n_ids < MAX_DESKTOP_APPS) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(combo), current);
        snprintf(ids[n_ids], NAME_LEN, "%s", current);
        active = n_ids;
        n_ids++;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active >= 0 ? active : (n_ids > 0 ? 0 : -1));
    labeled_row(table, 1, "Abrir com:", combo);
    gtk_box_pack_start(GTK_BOX(GTK_DIALOG(dialog)->vbox), table, TRUE, TRUE, 0);
    gtk_widget_show_all(table);

    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        int idx = gtk_combo_box_get_active(GTK_COMBO_BOX(combo));
        if (idx >= 0 && idx < n_ids) {
            char *argv[] = {"xdg-mime", "default", ids[idx], entry->mimetype, NULL};
            run_fire(argv);
            refill_assoc_row(&it, row);
        }
    }
    gtk_widget_destroy(dialog);
}

GtkWidget *build_associacoes_tab(void)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 8);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 12);

    GtkWidget *search_row = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(search_row), gtk_label_new("Buscar:"), FALSE, FALSE, 0);
    g_search_entry = gtk_entry_new();
    g_signal_connect(g_search_entry, "changed", G_CALLBACK(search_changed_cb), NULL);
    gtk_box_pack_start(GTK_BOX(search_row), g_search_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), search_row, FALSE, FALSE, 0);

    g_assoc_store = gtk_list_store_new(N_A_COLS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT);
    refill_assoc_store();

    GtkTreeModel *filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(g_assoc_store), NULL);
    g_assoc_filter = GTK_TREE_MODEL_FILTER(filter);
    gtk_tree_model_filter_set_visible_func(g_assoc_filter, assoc_filter_visible, NULL, NULL);

    g_assoc_view = gtk_tree_view_new_with_model(filter);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(g_assoc_view),
        gtk_tree_view_column_new_with_attributes("Extensao", gtk_cell_renderer_text_new(), "text", COL_A_EXT, NULL));
    GtkTreeViewColumn *label_col = gtk_tree_view_column_new_with_attributes(
        "Tipo de arquivo", gtk_cell_renderer_text_new(), "text", COL_A_LABEL, NULL);
    gtk_tree_view_column_set_expand(label_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(g_assoc_view), label_col);
    gtk_tree_view_append_column(
        GTK_TREE_VIEW(g_assoc_view),
        gtk_tree_view_column_new_with_attributes("Abrir com", gtk_cell_renderer_text_new(), "text", COL_A_APP, NULL));

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), g_assoc_view);
    gtk_box_pack_start(GTK_BOX(outer), scroll, TRUE, TRUE, 0);

    GtkWidget *btnbox = gtk_hbox_new(FALSE, 0);
    GtkWidget *change_btn = gtk_button_new_with_label("Alterar programa...");
    g_signal_connect(change_btn, "clicked", G_CALLBACK(change_app_cb), g_assoc_view);
    gtk_box_pack_end(GTK_BOX(btnbox), change_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), btnbox, FALSE, FALSE, 0);

    return outer;
}

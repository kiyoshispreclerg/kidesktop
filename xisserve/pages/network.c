/*
 * network.c - the --network page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section): device status (wifi/ethernet, up/down) plus a wifi
 * scan/connect list, same "shell out to the system's own CLI" pattern
 * pages/power.c (upower/brightnessctl) and pages/pulse.c (pactl) already
 * use instead of linking libnm/dbus -- nmcli's terse `-t` output is
 * stable and machine-parseable, so there is nothing NetworkManager's own
 * D-Bus API would buy here that isn't already paid for by popen().
 *
 * nmcli -t escapes ':' (the field separator), '\\' and the record
 * terminator inside a field with a leading backslash -- split_terse()
 * below undoes that so a connection name or SSID containing ':' doesn't
 * desync the column count.
 */
#include "../xisserve.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NET_POLL_MS 5000
#define MAX_FIELDS 6

typedef struct {
    char device[64];
    char type[32];
    char state[64];
    char connection[128];
} NetDevice;

typedef struct {
    gboolean in_use;
    char ssid[128];
    int signal;
    char security[64];
} WifiNet;

static GtkWidget *g_root;
static GtkWidget *g_box;
static guint g_poll_id;
static gboolean g_has_wifi_device;

/* ---- nmcli -t parsing ---------------------------------------------------- */

/* Splits one nmcli -t line into up to `maxfields` unescaped fields,
 * writing them (NUL-terminated, truncated to fit) into `out[i]`. Returns
 * the number of fields found. Modifies nothing outside `out`. */
static int split_terse(const char *line, char out[][160], int maxfields)
{
    int field = 0;
    size_t o = 0;
    out[0][0] = 0;
    for (const char *p = line; *p && field < maxfields; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            if (o + 1 < sizeof(out[0])) out[field][o++] = *p;
            continue;
        }
        if (*p == ':') {
            out[field][o] = 0;
            field++;
            o = 0;
            if (field < maxfields) out[field][0] = 0;
            continue;
        }
        if (o + 1 < sizeof(out[0])) out[field][o++] = *p;
    }
    out[field][o] = 0;
    return field + 1;
}

static FILE *run(const char *cmd)
{
    return popen(cmd, "r");
}

static GPtrArray *fetch_devices(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    FILE *f = run("LC_ALL=C nmcli -t -f DEVICE,TYPE,STATE,CONNECTION device status 2>/dev/null");
    if (!f) return rows;
    char line[512];
    char fields[MAX_FIELDS][160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        int n = split_terse(line, fields, MAX_FIELDS);
        if (n < 3) continue;
        /* Skip device kinds that are never user-facing (loopback, the
         * wifi p2p shadow device NetworkManager exposes alongside every
         * real wifi adapter, bridges we didn't create ourselves). */
        if (strcmp(fields[1], "loopback") == 0 || strstr(fields[0], "p2p-dev-") == fields[0])
            continue;
        NetDevice *d = g_new0(NetDevice, 1);
        snprintf(d->device, sizeof(d->device), "%s", fields[0]);
        snprintf(d->type, sizeof(d->type), "%s", fields[1]);
        snprintf(d->state, sizeof(d->state), "%s", fields[2]);
        snprintf(d->connection, sizeof(d->connection), "%s", n > 3 ? fields[3] : "");
        g_ptr_array_add(rows, d);
    }
    pclose(f);
    return rows;
}

static GPtrArray *fetch_wifi(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    FILE *f = run("LC_ALL=C nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null");
    if (!f) return rows;
    char line[512];
    char fields[MAX_FIELDS][160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        int n = split_terse(line, fields, MAX_FIELDS);
        if (n < 2 || !fields[1][0]) continue; /* hidden/blank SSID: nothing to connect to by name */
        WifiNet *w = g_new0(WifiNet, 1);
        w->in_use = (fields[0][0] == '*');
        snprintf(w->ssid, sizeof(w->ssid), "%s", fields[1]);
        w->signal = n > 2 ? atoi(fields[2]) : 0;
        snprintf(w->security, sizeof(w->security), "%s", n > 3 ? fields[3] : "");
        g_ptr_array_add(rows, w);
    }
    pclose(f);
    return rows;
}

/* ---- actions -------------------------------------------------------------- */

static void rebuild(void);

static void on_device_toggle(GtkWidget *btn, gpointer data)
{
    (void)btn;
    NetDevice *d = data;
    char dq[128], cmd[256];
    shell_quote(d->device, dq, sizeof(dq));
    gboolean connected = strstr(d->state, "disconnected") == NULL && strstr(d->state, "unavailable") == NULL;
    snprintf(cmd, sizeof(cmd), "nmcli device %s %s", connected ? "disconnect" : "connect", dq);
    run_detached(cmd);
    rebuild();
}

static void on_rescan_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn; (void)data;
    FILE *f = run("nmcli dev wifi rescan 2>/dev/null");
    if (f) pclose(f);
    rebuild();
}

static void do_connect(const char *ssid, const char *password)
{
    char sq[256], cmd[1024];
    shell_quote(ssid, sq, sizeof(sq));
    if (password && password[0]) {
        char pq[256];
        shell_quote(password, pq, sizeof(pq));
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect %s password %s", sq, pq);
    } else {
        snprintf(cmd, sizeof(cmd), "nmcli device wifi connect %s", sq);
    }
    run_detached(cmd);
}

static void on_wifi_row_clicked(GtkWidget *btn, gpointer data)
{
    WifiNet *w = data;
    gboolean open_net = !w->security[0] || strcmp(w->security, "--") == 0;
    if (w->in_use) {
        return; /* already the active network -- nothing to do on click */
    }
    if (open_net) {
        do_connect(w->ssid, NULL);
        rebuild();
        return;
    }
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Senha da rede", GTK_WINDOW(gtk_widget_get_toplevel(btn)),
                                                  GTK_DIALOG_MODAL, GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                  GTK_STOCK_CONNECT, GTK_RESPONSE_OK, NULL);
    GtkWidget *label = gtk_label_new(w->ssid);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *vbox = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(vbox), label, FALSE, FALSE, 4);
    gtk_box_pack_start(GTK_BOX(vbox), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        do_connect(w->ssid, gtk_entry_get_text(GTK_ENTRY(entry)));
    }
    gtk_widget_destroy(dlg);
    rebuild();
}

/* ---- widget building ------------------------------------------------------ */

static void style_fg(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    GdkColor c = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    static const GtkStateType states[] = {GTK_STATE_NORMAL, GTK_STATE_ACTIVE, GTK_STATE_PRELIGHT, GTK_STATE_SELECTED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        gtk_widget_modify_fg(w, states[i], &c);
        gtk_widget_modify_text(w, states[i], &c);
    }
}

static const char *device_type_label(const char *type)
{
    if (strcmp(type, "wifi") == 0) return "Wi-Fi";
    if (strcmp(type, "ethernet") == 0) return "Cabo";
    if (strcmp(type, "bridge") == 0) return "Ponte";
    return type;
}

static GtkWidget *build_device_row(NetDevice *d)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);
    gboolean connected = strstr(d->state, "connected") == d->state || strstr(d->state, "connected") != NULL;
    gboolean can_toggle = strcmp(d->type, "wifi") == 0 || strcmp(d->type, "ethernet") == 0;

    GtkWidget *vbox = gtk_vbox_new(FALSE, 1);
    char header_markup[256];
    snprintf(header_markup, sizeof(header_markup), "<b>%s</b> \xc2\xb7 <small>%s</small>",
             d->connection[0] ? d->connection : d->device, device_type_label(d->type));
    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), header_markup);
    gtk_misc_set_alignment(GTK_MISC(header), 0.0f, 0.5f);
    style_fg(header);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    GtkWidget *sub = gtk_label_new(d->state);
    gtk_misc_set_alignment(GTK_MISC(sub), 0.0f, 0.5f);
    style_fg(sub);
    gtk_box_pack_start(GTK_BOX(vbox), sub, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    if (can_toggle) {
        GtkWidget *btn = gtk_button_new_with_label(connected ? "Desativar" : "Ativar");
        g_signal_connect(btn, "clicked", G_CALLBACK(on_device_toggle), d);
        gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    }
    return hbox;
}

static GtkWidget *build_wifi_row(WifiNet *w)
{
    GtkWidget *btn = gtk_button_new();
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);
    gtk_container_add(GTK_CONTAINER(btn), hbox);
    if (w->in_use) gtk_widget_set_sensitive(btn, FALSE);

    char label_markup[192];
    const char *lock = (!w->security[0] || strcmp(w->security, "--") == 0) ? "" : "\xf0\x9f\x94\x92 ";
    snprintf(label_markup, sizeof(label_markup), "%s%s%s", w->in_use ? "\xe2\x9c\x93 " : "", lock, w->ssid);
    GtkWidget *label = gtk_label_new(label_markup);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);

    char sig[16];
    snprintf(sig, sizeof(sig), "%d%%", w->signal);
    GtkWidget *sig_label = gtk_label_new(sig);
    gtk_box_pack_start(GTK_BOX(hbox), sig_label, FALSE, FALSE, 0);

    g_signal_connect(btn, "clicked", G_CALLBACK(on_wifi_row_clicked), w);
    return btn;
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_box));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

static void rebuild(void)
{
    clear_rows();

    GtkWidget *top = gtk_hbox_new(FALSE, 4);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Rede</b>");
    gtk_misc_set_alignment(GTK_MISC(title), 0.0f, 0.5f);
    style_fg(title);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    GtkWidget *rescan_btn = gtk_button_new_with_label("Atualizar");
    g_signal_connect(rescan_btn, "clicked", G_CALLBACK(on_rescan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), rescan_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);

    GPtrArray *devices = fetch_devices();
    g_has_wifi_device = FALSE;
    for (guint i = 0; i < devices->len; i++) {
        NetDevice *d = g_ptr_array_index(devices, i);
        if (strcmp(d->type, "wifi") == 0) g_has_wifi_device = TRUE;
        gtk_box_pack_start(GTK_BOX(g_box), build_device_row(d), FALSE, FALSE, 0);
    }

    if (g_has_wifi_device) {
        gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
        GtkWidget *wifi_title = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(wifi_title), "<b>Redes dispon\xc3\xadveis</b>");
        gtk_misc_set_alignment(GTK_MISC(wifi_title), 0.0f, 0.5f);
        style_fg(wifi_title);
        gtk_box_pack_start(GTK_BOX(g_box), wifi_title, FALSE, FALSE, 0);

        GPtrArray *wifi = fetch_wifi();
        if (wifi->len == 0) {
            GtkWidget *empty = gtk_label_new("Nenhuma rede encontrada.");
            gtk_misc_set_alignment(GTK_MISC(empty), 0.0f, 0.5f);
            style_fg(empty);
            gtk_box_pack_start(GTK_BOX(g_box), empty, FALSE, FALSE, 0);
        } else {
            for (guint i = 0; i < wifi->len; i++) {
                gtk_box_pack_start(GTK_BOX(g_box), build_wifi_row(g_ptr_array_index(wifi, i)), FALSE, FALSE, 0);
            }
        }
        g_ptr_array_free(wifi, TRUE);
    }

    g_ptr_array_free(devices, TRUE);
    gtk_widget_show_all(g_box);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    rebuild();
    return TRUE;
}

/* ---- page interface -------------------------------------------------------- */

GtkWidget *page_network_build(void)
{
    g_box = gtk_vbox_new(FALSE, 4);

    g_root = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_root), g_box);

    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(g_root));
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    double r, g, b, a;
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    GdkColor bg = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    gtk_widget_modify_bg(viewport, GTK_STATE_NORMAL, &bg);

    return g_root;
}

void page_network_on_show(void)
{
    rebuild();
    if (!g_poll_id) {
        g_poll_id = g_timeout_add(NET_POLL_MS, on_poll, NULL);
    }
}

void page_network_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}

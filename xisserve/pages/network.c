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
#include "qrcode.h"

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
    /* connected devices only, filled by fetch_device_details() */
    char mac[32], ip4[64], gateway[64], ip6[96];
    char ssid[128], freq[24], rate[24], security[64];
    char psk[128]; /* only fetched for the expanded device, to build the QR */
    int signal;
} NetDevice;

typedef struct {
    gboolean in_use;
    char ssid[128];
    int signal;
    char security[64];
    gboolean known; /* a saved connection profile with this SSID exists */
} WifiNet;

static GtkWidget *g_root;
static GtkWidget *g_box;
static guint g_poll_id;
static gboolean g_has_wifi_device;
static char g_expanded_dev[64]; /* device whose info box is open ("" = none) */
static GPtrArray *g_cur_devices, *g_cur_wifi; /* rows' callbacks point into these; replaced on each render */
static gboolean g_show_more; /* "Mostrar mais": available Wi-Fi networks + inactive devices */

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
        g_strlcpy(d->device, fields[0], sizeof(d->device));
        g_strlcpy(d->type, fields[1], sizeof(d->type));
        g_strlcpy(d->state, fields[2], sizeof(d->state));
        g_strlcpy(d->connection, n > 3 ? fields[3] : "", sizeof(d->connection));
        g_ptr_array_add(rows, d);
    }
    pclose(f);
    return rows;
}

static gboolean is_connected_state(const char *state)
{
    return strncmp(state, "connected", 9) == 0;
}

/* SSIDs with a saved profile ("connection show" lists profile names,
 * which NetworkManager defaults to the SSID). */
static GHashTable *fetch_known_wifi(void)
{
    GHashTable *known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    FILE *f = run("LC_ALL=C nmcli -t -f NAME,TYPE connection show 2>/dev/null");
    if (!f) return known;
    char line[512];
    char fields[MAX_FIELDS][160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (split_terse(line, fields, MAX_FIELDS) >= 2 && strcmp(fields[1], "802-11-wireless") == 0)
            g_hash_table_add(known, g_strdup(fields[0]));
    }
    pclose(f);
    return known;
}

static gint wifi_cmp(gconstpointer a, gconstpointer b)
{
    const WifiNet *x = *(WifiNet *const *)a, *y = *(WifiNet *const *)b;
    return y->signal - x->signal;
}

/* One entry per SSID (several access points share one), strongest
 * first; the in-use flag survives if any of the duplicates had it. */
/* IP/gateway/MAC from `nmcli device show`, plus frequency/rate/signal
 * of the access point in use for a Wi-Fi device. */
static void fetch_device_details(NetDevice *d)
{
    char dq[128], cmd[384];
    shell_quote(d->device, dq, sizeof(dq));
    snprintf(cmd, sizeof(cmd),
             "LC_ALL=C nmcli -t -f GENERAL.HWADDR,IP4.ADDRESS,IP4.GATEWAY,IP6.ADDRESS device show %s 2>/dev/null", dq);
    FILE *f = run(cmd);
    if (f) {
        char line[512];
        char fields[MAX_FIELDS][160];
        while (fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\n")] = 0;
            if (split_terse(line, fields, 2) < 2 || !fields[1][0]) continue;
            if (strcmp(fields[0], "GENERAL.HWADDR") == 0) g_strlcpy(d->mac, fields[1], sizeof(d->mac));
            else if (strncmp(fields[0], "IP4.ADDRESS", 11) == 0 && !d->ip4[0]) g_strlcpy(d->ip4, fields[1], sizeof(d->ip4));
            else if (strcmp(fields[0], "IP4.GATEWAY") == 0) g_strlcpy(d->gateway, fields[1], sizeof(d->gateway));
            else if (strncmp(fields[0], "IP6.ADDRESS", 11) == 0 && !d->ip6[0]) g_strlcpy(d->ip6, fields[1], sizeof(d->ip6));
        }
        pclose(f);
    }
    if (strcmp(d->type, "wifi") != 0) return;
    snprintf(cmd, sizeof(cmd), "LC_ALL=C nmcli -t -f IN-USE,SSID,FREQ,SIGNAL,SECURITY,RATE dev wifi list ifname %s 2>/dev/null", dq);
    f = run(cmd);
    if (!f) return;
    char line[512];
    char fields[MAX_FIELDS][160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (split_terse(line, fields, MAX_FIELDS) < 6 || fields[0][0] != '*') continue;
        g_strlcpy(d->ssid, fields[1], sizeof(d->ssid));
        g_strlcpy(d->freq, fields[2], sizeof(d->freq));
        d->signal = atoi(fields[3]);
        g_strlcpy(d->security, fields[4], sizeof(d->security));
        g_strlcpy(d->rate, fields[5], sizeof(d->rate));
        break;
    }
    pclose(f);
}

/* The saved passphrase of the active Wi-Fi profile, for the QR code.
 * Empty for open networks or when NetworkManager won't hand it over. */
static void fetch_psk(NetDevice *d)
{
    if (strcmp(d->type, "wifi") != 0 || !d->connection[0]) return;
    char cq[256], cmd[400];
    shell_quote(d->connection, cq, sizeof(cq));
    snprintf(cmd, sizeof(cmd), "LC_ALL=C nmcli -s -g 802-11-wireless-security.psk connection show %s 2>/dev/null", cq);
    FILE *f = run(cmd);
    if (!f) return;
    if (fgets(d->psk, sizeof(d->psk), f)) d->psk[strcspn(d->psk, "\n")] = 0;
    pclose(f);
}

static GPtrArray *fetch_wifi(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    FILE *f = run("LC_ALL=C nmcli -t -f IN-USE,SSID,SIGNAL,SECURITY dev wifi list 2>/dev/null");
    if (!f) return rows;
    GHashTable *known = fetch_known_wifi();
    char line[512];
    char fields[MAX_FIELDS][160];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = 0;
        if (!line[0]) continue;
        int n = split_terse(line, fields, MAX_FIELDS);
        if (n < 2 || !fields[1][0]) continue; /* hidden/blank SSID: nothing to connect to by name */
        WifiNet *w = NULL;
        for (guint i = 0; i < rows->len; i++) {
            WifiNet *o = g_ptr_array_index(rows, i);
            if (strcmp(o->ssid, fields[1]) == 0) { w = o; break; }
        }
        int sig = n > 2 ? atoi(fields[2]) : 0;
        if (!w) {
            w = g_new0(WifiNet, 1);
            g_strlcpy(w->ssid, fields[1], sizeof(w->ssid));
            g_strlcpy(w->security, n > 3 ? fields[3] : "", sizeof(w->security));
            w->known = g_hash_table_contains(known, w->ssid);
            g_ptr_array_add(rows, w);
        }
        if (sig > w->signal) w->signal = sig;
        if (fields[0][0] == '*') w->in_use = TRUE;
    }
    pclose(f);
    g_hash_table_destroy(known);
    g_ptr_array_sort(rows, wifi_cmp);
    return rows;
}

/* ---- actions -------------------------------------------------------------- */

static void rebuild(void);
static void render_loading(void);
static void start_job(gboolean rescan);

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
    render_loading();
    start_job(TRUE);
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
    if (open_net || w->known) {
        do_connect(w->ssid, NULL);
        rebuild();
        return;
    }
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("Senha da rede"), GTK_WINDOW(gtk_widget_get_toplevel(btn)),
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

/* The GTK theme's prelight is a near-opaque fill, which fights this
 * popup's own --bg. Flat buttons here paint nothing at rest and a faint
 * translucent wash plus hairline outline (in the popup's fg colour) under
 * the pointer or keyboard focus instead. */
static gboolean on_subtle_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)data;
    GtkStateType st = GTK_WIDGET_STATE(w);
    gboolean hot = st == GTK_STATE_PRELIGHT || st == GTK_STATE_ACTIVE;
    gboolean focus = GTK_WIDGET_HAS_FOCUS(w);
    if (hot || focus) {
        double r, g, b, a;
        xisserve_get_fg_rgba(&r, &g, &b, &a);
        cairo_t *cr = gdk_cairo_create(ev->window);
        gdk_cairo_rectangle(cr, &ev->area);
        cairo_clip(cr);
        cairo_rectangle(cr, w->allocation.x + 0.5, w->allocation.y + 0.5, w->allocation.width - 1,
                        w->allocation.height - 1);
        if (hot) {
            cairo_set_source_rgba(cr, r, g, b, st == GTK_STATE_ACTIVE ? 0.14 : 0.08);
            cairo_fill_preserve(cr);
        }
        cairo_set_source_rgba(cr, r, g, b, hot ? 0.25 : 0.4);
        cairo_set_line_width(cr, 1.0);
        cairo_stroke(cr);
        cairo_destroy(cr);
    }
    gtk_container_propagate_expose(GTK_CONTAINER(w), gtk_bin_get_child(GTK_BIN(w)), ev);
    return TRUE;
}

static void make_subtle_button(GtkWidget *btn)
{
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_widget_set_app_paintable(btn, TRUE);
    g_signal_connect(btn, "expose-event", G_CALLBACK(on_subtle_expose), NULL);
}

static const char *device_type_label(const char *type)
{
    if (strcmp(type, "wifi") == 0) return "Wi-Fi";
    if (strcmp(type, "ethernet") == 0) return "Cabo";
    if (strcmp(type, "bridge") == 0) return "Ponte";
    return type;
}

static void info_line(GtkWidget *table, guint *row, const char *key, const char *val)
{
    if (!val[0]) return;
    GtkWidget *k = gtk_label_new(key), *v = gtk_label_new(val);
    gtk_misc_set_alignment(GTK_MISC(k), 0.0f, 0.5f);
    gtk_misc_set_alignment(GTK_MISC(v), 0.0f, 0.5f);
    gtk_label_set_selectable(GTK_LABEL(v), TRUE);
    style_fg(k);
    style_fg(v);
    gtk_table_attach(GTK_TABLE(table), k, 0, 1, *row, *row + 1, GTK_FILL, GTK_FILL, 0, 0);
    gtk_table_attach(GTK_TABLE(table), v, 1, 2, *row, *row + 1, GTK_FILL | GTK_EXPAND, GTK_FILL, 0, 0);
    (*row)++;
}

/* Backslash-escapes the characters the WIFI: URI treats specially. */
static void wifi_escape(const char *in, GString *out)
{
    for (; *in; in++) {
        if (strchr("\\;,:\"", *in)) g_string_append_c(out, '\\');
        g_string_append_c(out, *in);
    }
}

static gboolean on_qr_expose(GtkWidget *w, GdkEventExpose *ev, gpointer data)
{
    (void)ev;
    const QrMatrix *m = data;
    const int quiet = 4;
    int cells = m->width + 2 * quiet;
    int px = MAX(2, MIN(w->allocation.width, w->allocation.height) / cells);
    cairo_t *cr = gdk_cairo_create(w->window);
    int x0 = (w->allocation.width - px * cells) / 2;
    int y0 = (w->allocation.height - px * cells) / 2;
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_rectangle(cr, x0, y0, px * cells, px * cells);
    cairo_fill(cr);
    cairo_set_source_rgb(cr, 0, 0, 0);
    for (int y = 0; y < m->width; y++)
        for (int x = 0; x < m->width; x++)
            if (m->modules[y * m->width + x] & 1)
                cairo_rectangle(cr, x0 + (x + quiet) * px, y0 + (y + quiet) * px, px, px);
    cairo_fill(cr);
    cairo_destroy(cr);
    return TRUE;
}

/* A scannable "join this network" code, or NULL when it can't be made
 * (no libqrencode, or a secured network whose password we couldn't read). */
static GtkWidget *build_wifi_qr(NetDevice *d)
{
    gboolean open_net = !d->security[0] || strcmp(d->security, "--") == 0;
    if (!d->ssid[0] || (!open_net && !d->psk[0])) return NULL;
    GString *s = g_string_new("WIFI:T:");
    if (open_net) g_string_append(s, "nopass");
    else if (strstr(d->security, "WEP")) g_string_append(s, "WEP");
    else if (strstr(d->security, "WPA3") && !strstr(d->security, "WPA2") && !strstr(d->security, "WPA1")) g_string_append(s, "SAE");
    else g_string_append(s, "WPA");
    g_string_append(s, ";S:");
    wifi_escape(d->ssid, s);
    if (!open_net) {
        g_string_append(s, ";P:");
        wifi_escape(d->psk, s);
    }
    g_string_append(s, ";;");
    QrMatrix *m = qr_encode(s->str);
    memset(s->str, 0, s->len); /* the passphrase was in there */
    g_string_free(s, TRUE);
    if (!m) return NULL;
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, 168, 168);
    g_signal_connect(area, "expose-event", G_CALLBACK(on_qr_expose), m);
    g_object_set_data_full(G_OBJECT(area), "qr", m, (GDestroyNotify)qr_free);
    xisserve_a11y(area, _("QR code da rede"), _("Escaneie para conectar outro aparelho a esta rede"));
    return area;
}

/* Details box shown under an expanded connected device. */
static GtkWidget *build_info(NetDevice *d)
{
    GtkWidget *table = gtk_table_new(1, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(table), 12);
    gtk_table_set_row_spacings(GTK_TABLE(table), 2);
    guint row = 0;
    char buf[64];
    info_line(table, &row, _("Interface"), d->device);
    if (d->ssid[0]) {
        info_line(table, &row, _("Rede"), d->ssid);
        snprintf(buf, sizeof(buf), "%d%%", d->signal);
        info_line(table, &row, _("Sinal"), buf);
        info_line(table, &row, _("Frequência"), d->freq);
        info_line(table, &row, _("Velocidade"), d->rate);
        info_line(table, &row, _("Segurança"), d->security[0] ? d->security : _("Aberta"));
    }
    info_line(table, &row, _("Endereço IP"), d->ip4);
    info_line(table, &row, _("Gateway"), d->gateway);
    info_line(table, &row, "IPv6", d->ip6);
    info_line(table, &row, _("MAC"), d->mac);
    GtkWidget *box = gtk_hbox_new(FALSE, 0);
    gtk_container_set_border_width(GTK_CONTAINER(box), 6);
    gtk_box_pack_start(GTK_BOX(box), table, TRUE, TRUE, 0);
    GtkWidget *qr = build_wifi_qr(d);
    if (qr) gtk_box_pack_end(GTK_BOX(box), qr, FALSE, FALSE, 0);
    return box;
}

static void on_info_toggle(GtkWidget *btn, gpointer data)
{
    (void)btn;
    NetDevice *d = data;
    if (strcmp(g_expanded_dev, d->device) == 0) g_expanded_dev[0] = 0;
    else g_strlcpy(g_expanded_dev, d->device, sizeof(g_expanded_dev));
    rebuild();
}

static GtkWidget *build_device_row(NetDevice *d)
{
    GtkWidget *outer = gtk_vbox_new(FALSE, 2);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(outer), hbox, FALSE, FALSE, 0);
    gboolean connected = is_connected_state(d->state);
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

    gboolean open = connected && strcmp(g_expanded_dev, d->device) == 0;
    if (connected) {
        /* The whole label block is a flat button: click to show/hide the details. */
        GtkWidget *info_btn = gtk_button_new();
        make_subtle_button(info_btn);
        gtk_container_add(GTK_CONTAINER(info_btn), vbox);
        g_signal_connect(info_btn, "clicked", G_CALLBACK(on_info_toggle), d);
        xisserve_a11y(info_btn, gtk_label_get_text(GTK_LABEL(header)), open ? _("ocultar detalhes") : _("mostrar detalhes"));
        gtk_box_pack_start(GTK_BOX(hbox), info_btn, TRUE, TRUE, 0);
    } else {
        gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);
    }

    if (can_toggle) {
        GtkWidget *btn = gtk_button_new_with_label(connected ? _("Desativar") : _("Ativar"));
        g_signal_connect(btn, "clicked", G_CALLBACK(on_device_toggle), d);
        xisserve_a11y(btn, NULL, gtk_label_get_text(GTK_LABEL(header)));
        gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    }
    if (open) gtk_box_pack_start(GTK_BOX(outer), build_info(d), FALSE, FALSE, 0);
    return outer;
}

static GtkWidget *build_wifi_row(WifiNet *w)
{
    GtkWidget *btn = gtk_button_new();
    make_subtle_button(btn);
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
    /* The visible text sits in an hbox inside the button, where GAIL
     * doesn't look for a button's name -- and the lock/check glyphs mean
     * nothing read aloud anyway. */
    char desc[96];
    snprintf(desc, sizeof(desc), "%s%s, %s", w->in_use ? _("conectada, ") : "", sig,
             lock[0] ? _("protegida") : _("aberta"));
    xisserve_a11y(btn, w->ssid, desc);
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

/* Header (title + "Atualizar") shared by the loading and the loaded view. */
static void build_header(void)
{
    GtkWidget *top = gtk_hbox_new(FALSE, 4);
    GtkWidget *title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>Rede</b>");
    gtk_misc_set_alignment(GTK_MISC(title), 0.0f, 0.5f);
    style_fg(title);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    GtkWidget *rescan_btn = gtk_button_new_with_label(_("Atualizar"));
    g_signal_connect(rescan_btn, "clicked", G_CALLBACK(on_rescan_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), rescan_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);
}

/* What the page shows until the first (or a rescan-requested) nmcli
 * round trip lands -- nmcli can take seconds, and a blank page looks like
 * xisserve hung. */
static void render_loading(void)
{
    clear_rows();
    build_header();
    GtkWidget *l = gtk_label_new(_("Buscando redes\xe2\x80\xa6"));
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    style_fg(l);
    gtk_box_pack_start(GTK_BOX(g_box), l, FALSE, FALSE, 4);
    gtk_widget_show_all(g_box);
}

static void section_title(const char *markup)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    style_fg(l);
    gtk_box_pack_start(GTK_BOX(g_box), l, FALSE, FALSE, 0);
}

static void on_more_clicked(GtkButton *btn, gpointer data)
{
    (void)btn; (void)data;
    g_show_more = !g_show_more;
    rebuild();
}

/* Connected devices first, like the KDE applet; everything else (the
 * other Wi-Fi networks in range, devices that are down) hides behind
 * "Mostrar mais". */
static void render_result(GPtrArray *devices, GPtrArray *wifi)
{
    clear_rows();
    build_header();

    g_has_wifi_device = FALSE;
    guint n_active = 0, n_inactive = 0, n_avail = 0;
    for (guint i = 0; i < devices->len; i++) {
        NetDevice *d = g_ptr_array_index(devices, i);
        if (strcmp(d->type, "wifi") == 0) g_has_wifi_device = TRUE;
        if (is_connected_state(d->state)) {
            n_active++;
            gtk_box_pack_start(GTK_BOX(g_box), build_device_row(d), FALSE, FALSE, 0);
        } else if (strcmp(d->state, "unmanaged") != 0) {
            n_inactive++;
        }
    }
    for (guint i = 0; i < wifi->len; i++) {
        if (!((WifiNet *)g_ptr_array_index(wifi, i))->in_use) n_avail++;
    }
    if (!n_active) {
        GtkWidget *none = gtk_label_new(_("Nenhuma rede conectada."));
        gtk_misc_set_alignment(GTK_MISC(none), 0.0f, 0.5f);
        style_fg(none);
        gtk_box_pack_start(GTK_BOX(g_box), none, FALSE, FALSE, 0);
    }

    if (n_avail || n_inactive) {
        GtkWidget *more = gtk_button_new_with_label(g_show_more ? _("Mostrar menos") : _("Mostrar mais"));
        g_signal_connect(more, "clicked", G_CALLBACK(on_more_clicked), NULL);
        GtkWidget *hb = gtk_hbox_new(FALSE, 0);
        gtk_box_pack_start(GTK_BOX(hb), more, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(g_box), hb, FALSE, FALSE, 4);
    }

    if (g_show_more) {
        if (g_has_wifi_device) {
            gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
            section_title(_("<b>Redes Wi-Fi dispon\xc3\xadveis</b>"));
            if (!n_avail) {
                GtkWidget *empty = gtk_label_new(_("Nenhuma rede encontrada."));
                gtk_misc_set_alignment(GTK_MISC(empty), 0.0f, 0.5f);
                style_fg(empty);
                gtk_box_pack_start(GTK_BOX(g_box), empty, FALSE, FALSE, 0);
            }
            for (guint i = 0; i < wifi->len; i++) {
                WifiNet *w = g_ptr_array_index(wifi, i);
                if (!w->in_use) gtk_box_pack_start(GTK_BOX(g_box), build_wifi_row(w), FALSE, FALSE, 0);
            }
        }
        if (n_inactive) {
            gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
            section_title(_("<b>Dispositivos inativos</b>"));
            for (guint i = 0; i < devices->len; i++) {
                NetDevice *d = g_ptr_array_index(devices, i);
                if (!is_connected_state(d->state) && strcmp(d->state, "unmanaged") != 0)
                    gtk_box_pack_start(GTK_BOX(g_box), build_device_row(d), FALSE, FALSE, 0);
            }
        }
    }
    gtk_widget_show_all(g_box);
}

/* nmcli runs on a worker thread so a slow wifi scan never blocks the GTK
 * main loop (which also serves every other page and the control socket). */
typedef struct {
    gboolean rescan;
    GPtrArray *devices, *wifi;
} NetJob;

static gboolean g_job_running;
static gboolean g_job_again;        /* a request arrived while one was running */
static gboolean g_job_again_rescan;
static gboolean g_have_data;

static void start_job(gboolean rescan);

static gboolean job_done(gpointer data)
{
    NetJob *job = data;
    render_result(job->devices, job->wifi);
    g_have_data = TRUE;
    /* The rows' callbacks hold pointers into these, so they live until the
     * next render replaces them (which also destroys those rows). */
    GPtrArray *old_dev = g_cur_devices, *old_wifi = g_cur_wifi;
    g_cur_devices = job->devices;
    g_cur_wifi = job->wifi;
    if (old_dev) g_ptr_array_free(old_dev, TRUE);
    if (old_wifi) g_ptr_array_free(old_wifi, TRUE);
    g_free(job);
    g_job_running = FALSE;
    if (g_job_again) {
        g_job_again = FALSE;
        start_job(g_job_again_rescan);
        g_job_again_rescan = FALSE;
    }
    return FALSE;
}

static gpointer job_thread(gpointer data)
{
    NetJob *job = data;
    if (job->rescan) {
        FILE *f = run("nmcli dev wifi rescan 2>/dev/null");
        if (f) pclose(f);
    }
    job->devices = fetch_devices();
    for (guint i = 0; i < job->devices->len; i++) {
        NetDevice *d = g_ptr_array_index(job->devices, i);
        if (is_connected_state(d->state)) {
            fetch_device_details(d);
            if (strcmp(d->device, g_expanded_dev) == 0) fetch_psk(d);
        }
    }
    gboolean has_wifi = FALSE;
    for (guint i = 0; i < job->devices->len; i++) {
        if (strcmp(((NetDevice *)g_ptr_array_index(job->devices, i))->type, "wifi") == 0) has_wifi = TRUE;
    }
    job->wifi = has_wifi ? fetch_wifi() : g_ptr_array_new_with_free_func(g_free);
    g_idle_add(job_done, job);
    return NULL;
}

static void start_job(gboolean rescan)
{
    if (g_job_running) {
        g_job_again = TRUE;
        g_job_again_rescan = g_job_again_rescan || rescan;
        return;
    }
    g_job_running = TRUE;
    NetJob *job = g_new0(NetJob, 1);
    job->rescan = rescan;
    GThread *t = g_thread_new("xisserve-net", job_thread, job);
    g_thread_unref(t);
}

static void rebuild(void)
{
    start_job(FALSE);
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
    if (!g_have_data) render_loading();
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

/*
 * notifications.c - the --notifications page (see ../PROTOCOL.md and
 * xisserve.h's "pages" section): the notification history xispanel's
 * `notif` widget's bell icon opens on left click. Replaces the
 * `xisnotif` skeleton (see the project's session notes) -- once this
 * page existed there was no reason for `xisnotif` to still be planned as
 * a second GTK2 process; everything it would have needed (control
 * socket, GTK2, theme args) xisserve already has for its other pages.
 *
 * Storage is NOT this page's job, and can't be: xispanel is the
 * `org.freedesktop.Notifications` DBus service itself (see
 * `../../xispanel/notifd.c`), so it's the only process a Notify() call
 * ever reaches. There is no DBus signal or property a second, independent
 * listener could read the history from after the fact -- the spec has no
 * such thing, and even if it did, two processes both claiming the
 * well-known bus name isn't possible (notifd.c's own doc comment). So
 * this page is a plain client of xispanel's existing control socket
 * (`$XDG_RUNTIME_DIR/xispanel-ctl.sock`, line-JSON -- see
 * `../../xispanel/PROTOCOL.md`): GET_NOTIFICATIONS on every show/poll,
 * DELETE_NOTIFICATION/CLEAR_NOTIFICATIONS from the per-row and "Limpar
 * tudo" buttons. Same "already have the plumbing" reasoning xisconf uses
 * to drive xisback/xisguard over their own control sockets.
 *
 * Own tiny flat-JSON reader here rather than sharing xisserve.c's static
 * json_get_str/json_get_int (same "duplicated on purpose" call that file
 * itself documents) -- plus string unescaping and a walk over a top-level
 * JSON array of objects, neither of which xisserve.c's own copy needs for
 * its own (flatter) messages.
 */
#include "../xisserve.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NOTIF_POLL_MS 4000

typedef struct {
    unsigned int id;
    char app_name[96];
    char summary[160];
    char body[320];
    unsigned long long received_ms;
} NotifRow;

static GtkWidget *g_root;   /* scrolled window, returned to xisserve.c */
static GtkWidget *g_box;    /* rebuilt in place, same pattern as pages/energy.c */
static guint g_poll_id;

/* ---- xispanel-ctl.sock client ------------------------------------------ */

static void ctl_sockpath(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    if (!rundir || !*rundir) {
        rundir = "/tmp";
    }
    snprintf(out, outsz, "%s/xispanel-ctl.sock", rundir);
}

/* One request/response round trip: connect, write `req` (a single
 * newline-terminated JSON line), read until the daemon closes its end,
 * close. Returns a freshly malloc'd NUL-terminated buffer the caller
 * frees, or NULL on any failure -- every caller degrades to "nothing to
 * show" rather than erroring out, matching pages/pulse.c's "no sound
 * server answering" placeholder philosophy. */
static char *ctl_request(const char *req)
{
    char sockpath[PATH_MAX];
    ctl_sockpath(sockpath, sizeof(sockpath));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sockpath);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return NULL;
    }
    struct timeval tvto = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tvto, sizeof(tvto));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tvto, sizeof(tvto));

    size_t reqlen = strlen(req);
    if (write(fd, req, reqlen) != (ssize_t)reqlen) {
        close(fd);
        return NULL;
    }
    shutdown(fd, SHUT_WR);

    size_t cap = 8192, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(fd);
                return NULL;
            }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            free(buf);
            close(fd);
            return NULL;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    buf[len] = 0;
    close(fd);
    return buf;
}

/* ---- minimal flat-JSON reading ------------------------------------------ */

static int jget_str(const char *msg, const char *key, char *dst, size_t dst_sz)
{
    dst[0] = 0;
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    size_t o = 0;
    while (*p && *p != '"' && o + 1 < dst_sz) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': dst[o++] = '\n'; break;
            case 'r': dst[o++] = '\r'; break;
            case 't': dst[o++] = '\t'; break;
            case '"': dst[o++] = '"'; break;
            case '\\': dst[o++] = '\\'; break;
            default: dst[o++] = *p; break;
            }
            p++;
        } else {
            dst[o++] = *p++;
        }
    }
    dst[o] = 0;
    return 1;
}

static int jget_uint(const char *msg, const char *key, unsigned int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    char *end;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = (unsigned int)v;
    return 1;
}

static int jget_ull(const char *msg, const char *key, unsigned long long *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    char *end;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = v;
    return 1;
}

/* Returns a freshly malloc'd copy of the next top-level `{...}` object
 * starting at or after `*pp`, advancing `*pp` past it -- or NULL once
 * there are none left. Tracks quoted-string state (honoring backslash
 * escapes) so a brace that happens to appear inside an escaped body/
 * summary string never miscounts nesting depth. */
static char *next_json_object(const char **pp)
{
    const char *p = *pp;
    while (*p && *p != '{') {
        p++;
    }
    if (!*p) {
        *pp = p;
        return NULL;
    }
    const char *start = p;
    int depth = 0;
    int in_str = 0;
    for (; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') {
                in_str = 0;
            }
            continue;
        }
        if (*p == '"') {
            in_str = 1;
            continue;
        }
        if (*p == '{') {
            depth++;
        } else if (*p == '}') {
            depth--;
            if (depth == 0) {
                p++;
                size_t len = (size_t)(p - start);
                char *obj = malloc(len + 1);
                if (obj) {
                    memcpy(obj, start, len);
                    obj[len] = 0;
                }
                *pp = p;
                return obj;
            }
        }
    }
    *pp = p;
    return NULL;
}

/* Fetches the current history from xispanel, newest first (already the
 * order GET_NOTIFICATIONS hands back). Caller frees with
 * g_ptr_array_free(arr, TRUE). Empty (never NULL) if xispanel isn't
 * running or answered nothing usable. */
static GPtrArray *fetch_notifications(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    char *resp = ctl_request("{\"cmd\":\"GET_NOTIFICATIONS\"}\n");
    if (!resp) {
        return rows;
    }
    const char *arr = strstr(resp, "\"notifications\":[");
    if (arr) {
        const char *cursor = arr + strlen("\"notifications\":[");
        char *obj;
        while ((obj = next_json_object(&cursor)) != NULL) {
            NotifRow *row = g_new0(NotifRow, 1);
            jget_uint(obj, "id", &row->id);
            jget_str(obj, "app_name", row->app_name, sizeof(row->app_name));
            jget_str(obj, "summary", row->summary, sizeof(row->summary));
            jget_str(obj, "body", row->body, sizeof(row->body));
            jget_ull(obj, "received_ms", &row->received_ms);
            g_ptr_array_add(rows, row);
            free(obj);
        }
    }
    free(resp);
    return rows;
}

static void delete_notification(unsigned int id)
{
    char req[64];
    snprintf(req, sizeof(req), "{\"cmd\":\"DELETE_NOTIFICATION\",\"id\":%u}\n", id);
    char *resp = ctl_request(req);
    free(resp);
}

static void clear_notifications(void)
{
    char *resp = ctl_request("{\"cmd\":\"CLEAR_NOTIFICATIONS\"}\n");
    free(resp);
}

/* ---- widget building ----------------------------------------------------- */

static void rebuild(void);

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

static void on_remove_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    unsigned int id = GPOINTER_TO_UINT(data);
    delete_notification(id);
    rebuild();
}

static void on_clear_all_clicked(GtkWidget *btn, gpointer data)
{
    (void)data;
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gtk_widget_get_toplevel(btn)), GTK_DIALOG_MODAL,
                                             GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO,
                                             "Remover todas as notifica\xc3\xa7\xc3\xb5""es do hist\xc3\xb3rico?");
    int resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_YES) {
        clear_notifications();
        rebuild();
    }
}

static GtkWidget *build_row(const NotifRow *n)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);

    GtkWidget *vbox = gtk_vbox_new(FALSE, 1);

    time_t sec = (time_t)(n->received_ms / 1000);
    struct tm tmv;
    localtime_r(&sec, &tmv);
    char when[32];
    strftime(when, sizeof(when), "%d/%m/%Y %H:%M", &tmv);

    char header_markup[256];
    const char *app = n->app_name[0] ? n->app_name : "?";
    snprintf(header_markup, sizeof(header_markup), "<b>%s</b> \xc2\xb7 <small>%s</small>", app, when);
    GtkWidget *header = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(header), header_markup);
    gtk_misc_set_alignment(GTK_MISC(header), 0.0f, 0.5f);
    style_fg(header);
    gtk_box_pack_start(GTK_BOX(vbox), header, FALSE, FALSE, 0);

    if (n->summary[0]) {
        GtkWidget *summary = gtk_label_new(n->summary);
        gtk_misc_set_alignment(GTK_MISC(summary), 0.0f, 0.5f);
        gtk_label_set_line_wrap(GTK_LABEL(summary), TRUE);
        style_fg(summary);
        gtk_box_pack_start(GTK_BOX(vbox), summary, FALSE, FALSE, 0);
    }
    if (n->body[0]) {
        GtkWidget *body = gtk_label_new(n->body);
        gtk_misc_set_alignment(GTK_MISC(body), 0.0f, 0.5f);
        gtk_label_set_line_wrap(GTK_LABEL(body), TRUE);
        style_fg(body);
        gtk_box_pack_start(GTK_BOX(vbox), body, FALSE, FALSE, 0);
    }

    gtk_box_pack_start(GTK_BOX(hbox), vbox, TRUE, TRUE, 0);

    GtkWidget *remove_btn = gtk_button_new_with_label("Remover");
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_remove_clicked), GUINT_TO_POINTER(n->id));
    gtk_box_pack_start(GTK_BOX(hbox), remove_btn, FALSE, FALSE, 0);

    return hbox;
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
    gtk_label_set_markup(GTK_LABEL(title), "<b>Notifica\xc3\xa7\xc3\xb5""es</b>");
    gtk_misc_set_alignment(GTK_MISC(title), 0.0f, 0.5f);
    style_fg(title);
    gtk_box_pack_start(GTK_BOX(top), title, TRUE, TRUE, 0);
    GtkWidget *clear_btn = gtk_button_new_with_label("Limpar tudo");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_all_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);

    GPtrArray *rows = fetch_notifications();
    if (rows->len == 0) {
        GtkWidget *empty = gtk_label_new("Nenhuma notifica\xc3\xa7\xc3\xa3o.");
        gtk_misc_set_alignment(GTK_MISC(empty), 0.0f, 0.5f);
        style_fg(empty);
        gtk_box_pack_start(GTK_BOX(g_box), empty, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(clear_btn, FALSE);
    } else {
        for (guint i = 0; i < rows->len; i++) {
            NotifRow *n = g_ptr_array_index(rows, i);
            gtk_box_pack_start(GTK_BOX(g_box), build_row(n), FALSE, FALSE, 0);
            if (i + 1 < rows->len) {
                gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 0);
            }
        }
    }
    g_ptr_array_free(rows, TRUE);

    gtk_widget_show_all(g_box);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    rebuild();
    return TRUE;
}

/* ---- page interface ------------------------------------------------------ */

GtkWidget *page_notifications_build(void)
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

void page_notifications_on_show(void)
{
    rebuild();
    if (!g_poll_id) {
        g_poll_id = g_timeout_add(NOTIF_POLL_MS, on_poll, NULL);
    }
}

void page_notifications_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}

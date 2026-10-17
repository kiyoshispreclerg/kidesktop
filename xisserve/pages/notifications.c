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
 * Running jobs (GET_JOBS, xispanel's jobviewd.c) sit above the history in
 * their own "Em andamento" section: rows updated in place rather than
 * rebuilt, polled every JOBS_POLL_MS while there are any, with pause/
 * cancel going back through JOB_ACTION. A job ending refreshes the history
 * right away, since that's where its "Concluído" lands.
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
#define JOBS_POLL_MS 500
#define JOBS_IDLE_POLL_MS 2000

typedef struct {
    unsigned int id;
    char app_name[96];
    char summary[160];
    char body[320];
    unsigned long long received_ms;
} NotifRow;

typedef struct {
    unsigned int id;
    char app_name[96];
    char title[160];
    char info[320];
    int percent;
    unsigned long long speed, processed_bytes, total_bytes;
    int killable, suspendable, suspended;
} JobRow;

/* One running job's widgets, kept across polls and updated in place. */
typedef struct {
    unsigned int id;
    int suspended;
    GtkWidget *box, *header, *info, *detail, *bar, *pause_btn;
} JobView;

static GtkWidget *g_root;      /* scrolled window, returned to xisserve.c */
static GtkWidget *g_box;       /* jobs section + history */
static GtkWidget *g_jobs_box;  /* "Em andamento": hidden while there are none */
static GtkWidget *g_jobs_list;
static GtkWidget *g_hist_box;  /* rebuilt in place, same pattern as pages/energy.c */
static GPtrArray *g_job_views; /* JobView* */
static guint g_poll_id;
static gint64 g_hist_at;       /* when the history was last rebuilt */

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
    if (!xis_sun_path(&addr, sockpath)) {
        close(fd);
        return NULL;
    }
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

static int jget_int(const char *msg, const char *key, int *out)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char *p = strstr(msg, needle);
    if (!p) {
        return 0;
    }
    p += strlen(needle);
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return 0;
    }
    *out = (int)v;
    return 1;
}

static int jget_bool(const char *msg, const char *key)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "\"%s\":true", key);
    return strstr(msg, needle) != NULL;
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

/* Running jobs from xispanel, in its order. Same ownership as
 * fetch_notifications(). */
static GPtrArray *fetch_jobs(void)
{
    GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
    char *resp = ctl_request("{\"cmd\":\"GET_JOBS\"}\n");
    if (!resp) {
        return rows;
    }
    const char *arr = strstr(resp, "\"jobs\":[");
    if (arr) {
        const char *cursor = arr + strlen("\"jobs\":[");
        char *obj;
        while ((obj = next_json_object(&cursor)) != NULL) {
            JobRow *row = g_new0(JobRow, 1);
            char value1[320];
            jget_uint(obj, "id", &row->id);
            jget_str(obj, "app_name", row->app_name, sizeof(row->app_name));
            jget_str(obj, "title", row->title, sizeof(row->title));
            jget_str(obj, "info", row->info, sizeof(row->info));
            jget_str(obj, "value1", value1, sizeof(value1));
            if (!row->info[0]) {
                snprintf(row->info, sizeof(row->info), "%s", value1);
            }
            row->percent = -1;
            jget_int(obj, "percent", &row->percent);
            jget_ull(obj, "speed", &row->speed);
            jget_ull(obj, "processed_bytes", &row->processed_bytes);
            jget_ull(obj, "total_bytes", &row->total_bytes);
            row->killable = jget_bool(obj, "killable");
            row->suspendable = jget_bool(obj, "suspendable");
            row->suspended = jget_bool(obj, "suspended");
            g_ptr_array_add(rows, row);
            free(obj);
        }
    }
    free(resp);
    return rows;
}

static void job_action(unsigned int id, const char *action)
{
    char req[96];
    snprintf(req, sizeof(req), "{\"cmd\":\"JOB_ACTION\",\"id\":%u,\"action\":\"%s\"}\n", id, action);
    char *resp = ctl_request(req);
    free(resp);
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

    GtkWidget *remove_btn = gtk_button_new_with_label(_("Remover"));
    g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_remove_clicked), GUINT_TO_POINTER(n->id));
    xisserve_a11y(remove_btn, NULL, n->summary[0] ? n->summary : n->body);
    gtk_box_pack_start(GTK_BOX(hbox), remove_btn, FALSE, FALSE, 0);

    return hbox;
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_hist_box));
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
    GtkWidget *clear_btn = gtk_button_new_with_label(_("Limpar tudo"));
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_clear_all_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(top), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_hist_box), top, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_hist_box), gtk_hseparator_new(), FALSE, FALSE, 2);

    GPtrArray *rows = fetch_notifications();
    if (rows->len == 0) {
        GtkWidget *empty = gtk_label_new(_("Nenhuma notifica\xc3\xa7\xc3\xa3o."));
        gtk_misc_set_alignment(GTK_MISC(empty), 0.0f, 0.5f);
        style_fg(empty);
        gtk_box_pack_start(GTK_BOX(g_hist_box), empty, FALSE, FALSE, 0);
        gtk_widget_set_sensitive(clear_btn, FALSE);
    } else {
        for (guint i = 0; i < rows->len; i++) {
            NotifRow *n = g_ptr_array_index(rows, i);
            gtk_box_pack_start(GTK_BOX(g_hist_box), build_row(n), FALSE, FALSE, 0);
            if (i + 1 < rows->len) {
                gtk_box_pack_start(GTK_BOX(g_hist_box), gtk_hseparator_new(), FALSE, FALSE, 0);
            }
        }
    }
    g_ptr_array_free(rows, TRUE);

    gtk_widget_show_all(g_hist_box);
    g_hist_at = g_get_monotonic_time();
}

/* ---- running jobs ------------------------------------------------------- */

static JobView *find_job_view(unsigned int id)
{
    for (guint i = 0; g_job_views && i < g_job_views->len; i++) {
        JobView *v = g_ptr_array_index(g_job_views, i);
        if (v->id == id) {
            return v;
        }
    }
    return NULL;
}

static void schedule_poll(guint ms);

static void on_job_pause_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    JobView *v = find_job_view(GPOINTER_TO_UINT(data));
    if (v) {
        job_action(v->id, v->suspended ? "resume" : "suspend");
        schedule_poll(100); /* show the app's answer without waiting a full poll */
    }
}

static void on_job_cancel_clicked(GtkWidget *btn, gpointer data)
{
    (void)btn;
    job_action(GPOINTER_TO_UINT(data), "cancel");
    schedule_poll(100);
}

static GtkWidget *job_label(void)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    style_fg(l);
    return l;
}

static JobView *build_job_view(const JobRow *j)
{
    JobView *v = g_new0(JobView, 1);
    v->id = j->id;
    v->suspended = -1; /* forces the first update to label the button */
    v->box = gtk_vbox_new(FALSE, 2);
    v->header = job_label();
    v->info = job_label();
    v->detail = job_label();
    v->bar = gtk_progress_bar_new();
    gtk_box_pack_start(GTK_BOX(v->box), v->header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v->box), v->info, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(v->box), v->bar, FALSE, FALSE, 2);
    GtkWidget *bottom = gtk_hbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(bottom), v->detail, TRUE, TRUE, 0);
    if (j->suspendable) {
        v->pause_btn = gtk_button_new_with_label("");
        g_signal_connect(v->pause_btn, "clicked", G_CALLBACK(on_job_pause_clicked), GUINT_TO_POINTER(j->id));
        gtk_box_pack_start(GTK_BOX(bottom), v->pause_btn, FALSE, FALSE, 0);
    }
    if (j->killable) {
        GtkWidget *cancel = gtk_button_new_with_label(_("Cancelar"));
        g_signal_connect(cancel, "clicked", G_CALLBACK(on_job_cancel_clicked), GUINT_TO_POINTER(j->id));
        xisserve_a11y(cancel, NULL, j->title);
        gtk_box_pack_start(GTK_BOX(bottom), cancel, FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(v->box), bottom, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_jobs_list), v->box, FALSE, FALSE, 4);
    gtk_widget_show_all(v->box);
    return v;
}

static void update_job_view(JobView *v, const JobRow *j)
{
    char *header = g_markup_printf_escaped("<b>%s</b>%s%s", j->app_name[0] ? j->app_name : "?",
                                           j->title[0] ? " \xc2\xb7 " : "", j->title);
    gtk_label_set_markup(GTK_LABEL(v->header), header);
    g_free(header);
    gtk_label_set_text(GTK_LABEL(v->info), j->info);

    char detail[160] = "";
    if (j->suspended) {
        snprintf(detail, sizeof(detail), "%s", _("Pausado"));
    } else {
        if (j->total_bytes) {
            char *done = g_format_size_full(j->processed_bytes, G_FORMAT_SIZE_IEC_UNITS);
            char *total = g_format_size_full(j->total_bytes, G_FORMAT_SIZE_IEC_UNITS);
            snprintf(detail, sizeof(detail), _("%s de %s"), done, total);
            g_free(done);
            g_free(total);
        }
        if (j->speed) {
            char *rate = g_format_size_full(j->speed, G_FORMAT_SIZE_IEC_UNITS);
            size_t o = strlen(detail);
            snprintf(detail + o, sizeof(detail) - o, "%s%s/s", o ? " \xc2\xb7 " : "", rate);
            g_free(rate);
        }
    }
    gtk_label_set_text(GTK_LABEL(v->detail), detail);

    int pct = j->percent >= 0 ? j->percent
              : j->total_bytes ? (int)(j->processed_bytes * 100 / j->total_bytes) : -1;
    if (pct >= 0) {
        char text[16];
        snprintf(text, sizeof(text), "%d%%", pct);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(v->bar), pct / 100.0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(v->bar), text);
    } else {
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(v->bar));
    }

    if (v->pause_btn && v->suspended != j->suspended) {
        gtk_button_set_label(GTK_BUTTON(v->pause_btn), j->suspended ? _("Retomar") : _("Pausar"));
    }
    v->suspended = j->suspended;
}

/* Brings the section in line with xispanel's list. Returns how many jobs
 * are running; *ended is set when one went away since the last call. */
static guint sync_jobs(int *ended)
{
    GPtrArray *rows = fetch_jobs();
    *ended = 0;
    for (guint i = g_job_views->len; i-- > 0;) {
        JobView *v = g_ptr_array_index(g_job_views, i);
        int alive = 0;
        for (guint k = 0; k < rows->len && !alive; k++) {
            alive = ((JobRow *)g_ptr_array_index(rows, k))->id == v->id;
        }
        if (!alive) {
            gtk_widget_destroy(v->box);
            g_ptr_array_remove_index(g_job_views, i);
            *ended = 1;
        }
    }
    for (guint k = 0; k < rows->len; k++) {
        JobRow *j = g_ptr_array_index(rows, k);
        JobView *v = find_job_view(j->id);
        if (!v) {
            v = build_job_view(j);
            g_ptr_array_add(g_job_views, v);
        }
        update_job_view(v, j);
    }
    guint n = rows->len;
    g_ptr_array_free(rows, TRUE);
    if (n) {
        gtk_widget_show(g_jobs_box);
    } else {
        gtk_widget_hide(g_jobs_box);
    }
    return n;
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    g_poll_id = 0;
    int ended;
    guint jobs = sync_jobs(&ended);
    if (ended || g_get_monotonic_time() - g_hist_at >= NOTIF_POLL_MS * 1000LL) {
        rebuild();
    }
    schedule_poll(jobs ? JOBS_POLL_MS : JOBS_IDLE_POLL_MS);
    return FALSE;
}

static void schedule_poll(guint ms)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
    }
    g_poll_id = g_timeout_add(ms, on_poll, NULL);
}

/* ---- page interface ------------------------------------------------------ */

GtkWidget *page_notifications_build(void)
{
    g_box = gtk_vbox_new(FALSE, 4);
    g_job_views = g_ptr_array_new_with_free_func(g_free);

    g_jobs_box = gtk_vbox_new(FALSE, 2);
    gtk_widget_set_no_show_all(g_jobs_box, TRUE); /* sync_jobs() shows it when there's something */
    GtkWidget *jobs_title = gtk_label_new(NULL);
    char *markup = g_markup_printf_escaped("<b>%s</b>", _("Em andamento"));
    gtk_label_set_markup(GTK_LABEL(jobs_title), markup);
    g_free(markup);
    gtk_misc_set_alignment(GTK_MISC(jobs_title), 0.0f, 0.5f);
    style_fg(jobs_title);
    g_jobs_list = gtk_vbox_new(FALSE, 2);
    GtkWidget *sep = gtk_hseparator_new();
    gtk_box_pack_start(GTK_BOX(g_jobs_box), jobs_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_jobs_box), g_jobs_list, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_jobs_box), sep, FALSE, FALSE, 4);
    gtk_widget_show(jobs_title);
    gtk_widget_show(g_jobs_list);
    gtk_widget_show(sep);
    g_hist_box = gtk_vbox_new(FALSE, 4);
    gtk_box_pack_start(GTK_BOX(g_box), g_jobs_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(g_box), g_hist_box, FALSE, FALSE, 0);
    gtk_widget_show(g_hist_box);
    gtk_widget_show(g_box);

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
    int ended;
    guint jobs = sync_jobs(&ended);
    schedule_poll(jobs ? JOBS_POLL_MS : JOBS_IDLE_POLL_MS);
}

void page_notifications_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}

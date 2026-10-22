/*
 * xisguard.c - the --xisguard page: which programs are using what, with a
 * "Remover acesso" button per program and action. A plain client of
 * xisguard's control socket (xisguard-ctl.<display>.sock): LIST_ACTIVE
 * for the list (the guard keeps the last reports in memory), GET_STATUS
 * for the protection state, REVOKE_ACCESS for the button, which puts a
 * permanent DENY rule first for that program and action (removable in
 * kiconf's Permissoes tab). Programs in the guard's ignore.conf are listed
 * last, dimmed. Opened by xispanel's `xisguard` widget.
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

#define XG_POLL_MS 2000
#define XG_MAX_ROWS 64

typedef struct {
    char exe[PATH_MAX];
    char action[24];
    long ts;
    gboolean ignored, denied;
} ActiveRow;

static GtkWidget *g_root;
static GtkWidget *g_box;
static guint g_poll_id;

/* ---- control socket ----------------------------------------------------- */

static void ctl_sockpath(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    const char *d = getenv("DISPLAY");
    const char *colon = d ? strrchr(d, ':') : NULL;
    snprintf(out, outsz, "%s/xisguard-ctl.%d.sock", rundir && *rundir ? rundir : "/tmp",
             colon ? atoi(colon + 1) : 0);
}

/* One round trip; malloc'd reply or NULL when xisguard isn't there. */
static char *ctl_request(const char *req)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    ctl_sockpath(addr.sun_path, sizeof(addr.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || write(fd, req, strlen(req)) < 0 ||
        write(fd, "\n", 1) < 0) {
        close(fd);
        return NULL;
    }
    size_t len = 0, cap = 65536;
    char *buf = g_malloc(cap);
    ssize_t n;
    while ((n = read(fd, buf + len, cap - len - 1)) > 0) {
        len += (size_t)n;
        if (len + 1 == cap)
            buf = g_realloc(buf, cap *= 2);
    }
    close(fd);
    buf[len] = '\0';
    return buf;
}

static const char *json_find(const char *msg, const char *key)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(msg, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ')
        p++;
    return p;
}

static gboolean json_str(const char *msg, const char *key, char *out, size_t cap)
{
    const char *p = json_find(msg, key);
    out[0] = '\0';
    if (!p || *p != '"')
        return FALSE;
    size_t o = 0;
    for (p++; *p && *p != '"' && o + 1 < cap; p++) {
        if (*p == '\\' && p[1])
            p++;
        out[o++] = *p;
    }
    out[o] = '\0';
    return TRUE;
}

static long json_long(const char *msg, const char *key, long fallback)
{
    const char *p = json_find(msg, key);
    return p && (*p == '-' || (*p >= '0' && *p <= '9')) ? strtol(p, NULL, 10) : fallback;
}

static gboolean json_bool(const char *msg, const char *key)
{
    const char *p = json_find(msg, key);
    return p && strncmp(p, "true", 4) == 0;
}

static void json_escape(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    for (; *in && o + 3 < cap; in++) {
        if (*in == '"' || *in == '\\')
            out[o++] = '\\';
        out[o++] = *in;
    }
    out[o] = '\0';
}

/* ---- data ----------------------------------------------------------------- */

/* Entries of "active":[{...},{...}] (each object is flat, no nested braces). */
static int parse_active(const char *resp, ActiveRow *rows, int max)
{
    const char *p = strstr(resp, "\"active\"");
    p = p ? strchr(p, '[') : NULL;
    int n = 0;
    while (p && n < max) {
        const char *s = strchr(p, '{');
        const char *e = s ? strchr(s, '}') : NULL;
        if (!e)
            break;
        char *obj = g_strndup(s, (gsize)(e - s) + 1);
        ActiveRow *r = &rows[n++];
        memset(r, 0, sizeof(*r));
        json_str(obj, "exe", r->exe, sizeof(r->exe));
        json_str(obj, "action", r->action, sizeof(r->action));
        r->ts = json_long(obj, "ts", 0);
        r->ignored = json_bool(obj, "ignored");
        r->denied = json_bool(obj, "denied");
        g_free(obj);
        p = e + 1;
    }
    return n;
}

static const char *action_text(const char *action)
{
    static const struct {
        const char *name;
        const char *text;
    } map[] = {
        { "ATTACH", N_("usa mem\xc3\xb3ria compartilhada") },
        { "SELECTION", N_("acessa a \xc3\xa1rea de transfer\xc3\xaancia") },
        { "COMPOSITE", N_("acessa outras janelas") },
        { "SCREEN", N_("captura a tela") },
        { "RECORD", N_("grava eventos (teclas)") },
        { "CURSOR", N_("l\xc3\xaa o cursor do mouse") },
        { "INPUT_GRAB", N_("captura mouse/teclado") },
        { "INPUT_INJECT", N_("insere teclas") },
        { "HOTKEY", N_("registra atalhos globais") },
        { "INPUT", N_("captura entrada sem foco") },
        { "MANAGE", N_("lista outras janelas") },
        { "GRAB_OVERRIDE", N_("toma o controle de entrada") },
        { "WARP", N_("move o cursor do mouse") },
        { "FOCUS", N_("rouba o foco") },
        { "RANDR", N_("altera a configura\xc3\xa7\xc3\xa3o das telas") },
        { "OVERLAY", N_("cria janela sobreposta") },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
        if (strcmp(map[i].name, action) == 0)
            return _(map[i].text);
    return action;
}

/* ---- widgets ------------------------------------------------------------------ */

static void rebuild(void);

static void style_fg(GtkWidget *w, double alpha)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    /* dimmed = blended towards the page background */
    double br, bg, bb, ba;
    xisserve_get_bg_rgba(&br, &bg, &bb, &ba);
    GdkColor c = {0, (guint16)((r * alpha + br * (1 - alpha)) * 65535), (guint16)((g * alpha + bg * (1 - alpha)) * 65535),
                  (guint16)((b * alpha + bb * (1 - alpha)) * 65535)};
    static const GtkStateType states[] = {GTK_STATE_NORMAL, GTK_STATE_ACTIVE, GTK_STATE_PRELIGHT, GTK_STATE_SELECTED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        gtk_widget_modify_fg(w, states[i], &c);
        gtk_widget_modify_text(w, states[i], &c);
    }
}

static GtkWidget *add_label(const char *markup, double alpha, gboolean wrap)
{
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(l), markup);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(l), wrap);
    style_fg(l, alpha);
    gtk_box_pack_start(GTK_BOX(g_box), l, FALSE, FALSE, 0);
    return l;
}

static void age_text(long ts, long now, char *out, size_t outsz)
{
    long d = now - ts;
    if (d < 0)
        d = 0;
    if (d < 60)
        snprintf(out, outsz, "%lds", d);
    else if (d < 3600)
        snprintf(out, outsz, "%ldmin", d / 60);
    else
        snprintf(out, outsz, "%ldh", d / 3600);
}

typedef struct {
    char exe[PATH_MAX];
    char action[24];
} RevokeCtx;

static void on_revoke(GtkWidget *btn, gpointer data)
{
    RevokeCtx *c = data;
    /* The rule is for the program, not for one set of arguments. */
    char path[PATH_MAX];
    g_strlcpy(path, c->exe, sizeof(path));
    char *bar = strchr(path, '|');
    if (bar)
        *bar = '\0';
    char pe[PATH_MAX * 2], req[PATH_MAX * 2 + 128];
    json_escape(pe, sizeof(pe), path);
    snprintf(req, sizeof(req), "{\"cmd\":\"REVOKE_ACCESS\",\"action\":\"%s\",\"pattern\":\"%s\"}", c->action, pe);
    char *resp = ctl_request(req);
    gboolean ok = resp && strstr(resp, "\"ok\":true");
    g_free(resp);
    if (ok) {
        gtk_button_set_label(GTK_BUTTON(btn), _("Acesso removido"));
        gtk_widget_set_sensitive(btn, FALSE);
    }
}

static GtkWidget *build_action_row(const ActiveRow *r, long now, gboolean dim)
{
    GtkWidget *hbox = gtk_hbox_new(FALSE, 6);
    char age[24], text[256];
    age_text(r->ts, now, age, sizeof(age));
    snprintf(text, sizeof(text), "%s \xc2\xb7 %s", action_text(r->action), age);
    GtkWidget *l = gtk_label_new(text);
    gtk_misc_set_alignment(GTK_MISC(l), 0.0f, 0.5f);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    style_fg(l, dim ? 0.55 : 0.9);
    gtk_box_pack_start(GTK_BOX(hbox), l, TRUE, TRUE, 12);

    GtkWidget *btn = gtk_button_new_with_label(r->denied ? _("Acesso removido") : _("Remover acesso"));
    gtk_widget_set_sensitive(btn, !r->denied);
    if (!r->denied) {
        RevokeCtx *c = g_new0(RevokeCtx, 1);
        g_strlcpy(c->exe, r->exe, sizeof(c->exe));
        g_strlcpy(c->action, r->action, sizeof(c->action));
        /* freed with the button, which rebuild() destroys every poll */
        g_object_set_data_full(G_OBJECT(btn), "ctx", c, g_free);
        g_signal_connect(btn, "clicked", G_CALLBACK(on_revoke), c);
    }
    char name[300];
    snprintf(name, sizeof(name), "%s: %s", r->exe, action_text(r->action));
    xisserve_a11y(btn, NULL, name);
    gtk_box_pack_start(GTK_BOX(hbox), btn, FALSE, FALSE, 0);
    return hbox;
}

/* Rows come newest first; a program's rows are listed together under it, in
 * the order of its newest report. */
static void add_group(const ActiveRow *rows, int n, gboolean ignored, long now)
{
    gboolean done[XG_MAX_ROWS] = { FALSE };
    gboolean any = FALSE;
    for (int i = 0; i < n; i++) {
        if (done[i] || rows[i].ignored != ignored)
            continue;
        if (!any && ignored) {
            gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 4);
            add_label(_("<small>Ignorados (ignore.conf)</small>"), 0.55, FALSE);
        } else if (any || (!ignored && i > 0)) {
            gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);
        }
        any = TRUE;
        const char *base = strrchr(rows[i].exe, '/');
        char path[PATH_MAX];
        g_strlcpy(path, rows[i].exe, sizeof(path));
        char *bar = strchr(path, '|');
        if (bar)
            *bar = '\0';
        char *name_esc = g_markup_escape_text(base ? base + 1 : rows[i].exe, -1);
        char *path_esc = g_markup_escape_text(path, -1);
        char *markup = g_strdup_printf("<b>%s</b>\n<small>%s</small>", name_esc, path_esc);
        add_label(markup, ignored ? 0.55 : 1.0, FALSE);
        g_free(markup);
        g_free(name_esc);
        g_free(path_esc);
        for (int j = i; j < n; j++) {
            if (!done[j] && rows[j].ignored == ignored && strcmp(rows[j].exe, rows[i].exe) == 0) {
                done[j] = TRUE;
                gtk_box_pack_start(GTK_BOX(g_box), build_action_row(&rows[j], now, ignored), FALSE, FALSE, 1);
            }
        }
    }
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_box));
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
}

static void rebuild(void)
{
    clear_rows();
    add_label(_("<b>XisGuard</b>"), 1.0, FALSE);

    char *status = ctl_request("{\"cmd\":\"GET_STATUS\"}");
    if (!status || !strstr(status, "\"ok\":true")) {
        g_free(status);
        add_label(_("O XisGuard n\xc3\xa3o est\xc3\xa1 em execu\xc3\xa7\xc3\xa3o."), 0.9, TRUE);
        gtk_widget_show_all(g_box);
        return;
    }
    gboolean enabled = json_long(status, "xnotify_enabled", 1) != 0;
    g_free(status);
    add_label(enabled ? _("Prote\xc3\xa7\xc3\xa3o ativa") : _("<b>Prote\xc3\xa7\xc3\xa3o DESLIGADA</b>"), 0.9, FALSE);
    gtk_box_pack_start(GTK_BOX(g_box), gtk_hseparator_new(), FALSE, FALSE, 2);

    ActiveRow rows[XG_MAX_ROWS];
    char *resp = ctl_request("{\"cmd\":\"LIST_ACTIVE\"}");
    int n = resp ? parse_active(resp, rows, XG_MAX_ROWS) : 0;
    long now = resp ? json_long(resp, "now", (long)time(NULL)) : (long)time(NULL);
    g_free(resp);

    if (n == 0) {
        add_label(_("Nenhum programa usou recursos protegidos desde que o XisGuard iniciou."), 0.9, TRUE);
    } else {
        add_group(rows, n, FALSE, now);
        add_group(rows, n, TRUE, now);
    }
    gtk_widget_show_all(g_box);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    rebuild();
    return TRUE;
}

/* ---- page interface ---------------------------------------------------------- */

GtkWidget *page_xisguard_build(void)
{
    g_box = gtk_vbox_new(FALSE, 4);
    gtk_container_set_border_width(GTK_CONTAINER(g_box), 6);

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

void page_xisguard_on_show(void)
{
    rebuild();
    if (!g_poll_id)
        g_poll_id = g_timeout_add(XG_POLL_MS, on_poll, NULL);
}

void page_xisguard_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}

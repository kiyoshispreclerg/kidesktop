/*
 * clipboard widget - an icon for kimemoryd's clipboard history
 * (../../kimemory/). Left-click opens `xisserve --clipboard` anchored to
 * the icon -- same "icon + tooltip + click opens the xisserve page" shape
 * as widgets/storage.c. The panel never takes focus, so the window that
 * is active at the click is still the user's, and xisserve takes it as
 * the paste target; `for_active=yes` also opens the list already
 * filtered to that window's app.
 *
 * Hovering asks kimemoryd's control socket for the newest item and the
 * history size -- only on hover, with short socket timeouts, so a
 * stuck daemon can't stall the panel. The icon is dimmed while the
 * socket isn't there (kimemoryd not running); that check is a stat(),
 * not a connection, because kimemoryd waits for a request line on every
 * connection it accepts.
 */
#include "../xispanel.h"

#include <X11/Xlib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define CLIPBOARD_POLL_MS 5000
#define CLIPBOARD_IO_TIMEOUT_MS 150
#define CLIPBOARD_PREVIEW_CHARS 60

typedef struct {
    char cmd[192];      /* xisserve binary opened on left click */
    int for_active;     /* open filtered to the active window's app */
    int alive;          /* kimemoryd's socket exists */
} ClipboardPriv;

static void sock_path(char *out, size_t outsz)
{
    const char *rundir = getenv("XDG_RUNTIME_DIR");
    const char *d = getenv("DISPLAY");
    const char *colon = d ? strrchr(d, ':') : NULL;
    snprintf(out, outsz, "%s/kimemory-ctl.%d.sock", rundir && *rundir ? rundir : "/tmp",
             colon ? atoi(colon + 1) : 0);
}

/* One request line, reply into `out`; 0 on any failure or timeout. */
static int ctl_request(const char *req, char *out, size_t outsz)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    sock_path(addr.sun_path, sizeof(addr.sun_path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return 0;
    struct timeval tv = { 0, CLIPBOARD_IO_TIMEOUT_MS * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    size_t len = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 && write(fd, req, strlen(req)) > 0 &&
        write(fd, "\n", 1) == 1) {
        ssize_t n;
        while (len + 1 < outsz && (n = read(fd, out + len, outsz - 1 - len)) > 0)
            len += (size_t)n;
    }
    close(fd);
    out[len] = '\0';
    return len > 0 && strstr(out, "\"ok\":true") != NULL;
}

/* Flat JSON string value, unescaped (\n etc. become spaces: tooltip). */
static void json_str(const char *msg, const char *key, char *out, size_t outsz)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(msg, pat);
    size_t o = 0;
    if (p) {
        for (p += strlen(pat); *p && *p != '"' && o + 1 < outsz; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'u' && p[1] && p[2] && p[3] && p[4]) {
                    p += 4;
                    out[o++] = ' ';
                    continue;
                }
                out[o++] = (*p == 'n' || *p == 't' || *p == 'r') ? ' ' : *p;
            } else {
                out[o++] = *p;
            }
        }
    }
    out[o] = '\0';
}

static long json_long(const char *msg, const char *key)
{
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *p = strstr(msg, pat);
    return p ? strtol(p + strlen(pat), NULL, 10) : 0;
}

static int check_alive(void)
{
    char path[108];
    struct stat st;
    sock_path(path, sizeof(path));
    return stat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static int clipboard_init(PanelWidget *w)
{
    ClipboardPriv *cp = w->priv;
    char buf[16];
    if (!kv_get(w->config_kv, "cmd", cp->cmd, sizeof(cp->cmd)) || !cp->cmd[0]) {
        snprintf(cp->cmd, sizeof(cp->cmd), "xisserve");
    }
    cp->for_active = kv_get(w->config_kv, "for_active", buf, sizeof(buf)) && !strcmp(buf, "yes");
    cp->alive = check_alive();
    w->next_tick_ms = now_ms() + CLIPBOARD_POLL_MS;
    return 0;
}

static int clipboard_on_tick(PanelWidget *w, uint64_t now)
{
    ClipboardPriv *cp = w->priv;
    w->next_tick_ms = now + CLIPBOARD_POLL_MS;
    int alive = check_alive();
    if (alive == cp->alive)
        return 0;
    cp->alive = alive;
    return 1;
}

static void clipboard_measure(PanelWidget *w, int cross_axis, int *out_len, int *out_min_len)
{
    (void)w;
    *out_len = cross_axis;
    *out_min_len = cross_axis;
}

/* A clipboard glyph for icon themes without edit-paste: board outline
 * plus the clip on top. */
static void draw_board(cairo_t *cr, double cx, double cy, double size, double r, double g, double b)
{
    cairo_save(cr);
    cairo_set_source_rgba(cr, r, g, b, 0.9);
    cairo_set_line_width(cr, 1.4);
    double bw = size * 0.6, bh = size * 0.72;
    cairo_rectangle(cr, cx - bw / 2, cy - bh / 2 + size * 0.05, bw, bh);
    cairo_stroke(cr);
    double cw = bw * 0.5, ch = size * 0.14;
    cairo_rectangle(cr, cx - cw / 2, cy - bh / 2 - ch / 2 + size * 0.05, cw, ch);
    cairo_fill(cr);
    for (int i = 1; i <= 3; i++) {
        double y = cy - bh / 2 + size * 0.05 + bh * i / 4.0;
        cairo_move_to(cr, cx - bw * 0.3, y);
        cairo_line_to(cr, cx + bw * 0.3, y);
    }
    cairo_stroke(cr);
    cairo_restore(cr);
}

static void clipboard_paint(PanelWidget *w, cairo_t *cr)
{
    ClipboardPriv *cp = w->priv;
    Panel *p = w->panel;
    int ox, oy, owidth, oheight;
    widget_get_rect(w, &ox, &oy, &owidth, &oheight);
    (void)owidth;
    (void)oheight;
    widget_paint_hover_bg(w, cr);

    /* Dimmed as a whole while kimemoryd isn't running. */
    cairo_push_group(cr);
    int icon_px = w->thickness > 6 ? w->thickness - 6 : 16;
    cairo_surface_t *themed = panel_theme_icon(p, "edit-paste", icon_px);
    if (themed) {
        draw_icon_scaled(cr, themed, ox + (w->thickness - icon_px) / 2, oy + (w->thickness - icon_px) / 2, icon_px);
    } else {
        draw_board(cr, ox + w->thickness / 2.0, oy + w->thickness / 2.0, w->thickness * 0.8, p->fg_r, p->fg_g,
                   p->fg_b);
    }
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, cp->alive ? 1.0 : 0.4);
}

static int clipboard_get_tooltip(PanelWidget *w, int local_x, char *buf, size_t bufsz, int *anchor_x,
                                  int *anchor_w, int *out_closable, void **out_ctx)
{
    (void)local_x;
    (void)out_closable;
    (void)out_ctx;
    char resp[4096];
    *anchor_x = 0;
    *anchor_w = w->len;
    if (!ctl_request("{\"cmd\":\"STATUS\"}", resp, sizeof(resp))) {
        snprintf(buf, bufsz, "\xc3\x81rea de transfer\xc3\xaancia: kimemoryd n\xc3\xa3o est\xc3\xa1 rodando");
        return 1;
    }
    long count = json_long(resp, "count");
    if (count == 0 || !ctl_request("{\"cmd\":\"LIST\",\"scope\":\"all\",\"limit\":1}", resp, sizeof(resp))) {
        snprintf(buf, bufsz, "\xc3\x81rea de transfer\xc3\xaancia vazia");
        return 1;
    }
    char type[16], preview[512], src[128];
    json_str(resp, "type", type, sizeof(type));
    json_str(resp, "preview", preview, sizeof(preview));
    json_str(resp, "src_app", src, sizeof(src));

    /* Cut the preview at a character boundary. */
    size_t n = 0, chars = 0;
    while (preview[n] && chars < CLIPBOARD_PREVIEW_CHARS) {
        n++;
        while (((unsigned char)preview[n] & 0xC0) == 0x80)
            n++;
        chars++;
    }
    int cut = preview[n] != '\0';
    preview[n] = '\0';
    snprintf(buf, bufsz, "%s%s\nde %s \xc2\xb7 %ld %s no hist\xc3\xb3rico",
             strcmp(type, "image") ? preview : "[imagem]", cut ? "\xe2\x80\xa6" : "", src[0] ? src : "?", count,
             count == 1 ? "item" : "itens");
    return 1;
}

static int clipboard_on_button(PanelWidget *w, int button, int local_x, int local_y, int root_x, int root_y)
{
    (void)local_x;
    (void)local_y;
    (void)root_x;
    (void)root_y;
    ClipboardPriv *cp = w->priv;
    if (button != Button1) {
        return 0;
    }
    xisserve_spawn_for_widget(w, cp->cmd, cp->for_active ? "--clipboard --for-active" : "--clipboard");
    return 1;
}

const PanelWidgetOps clipboard_ops = {
    .type_name = "clipboard",
    .embeddable = 1,
    .priv_size = sizeof(ClipboardPriv),
    .init = clipboard_init,
    .measure = clipboard_measure,
    .paint = clipboard_paint,
    .on_button = clipboard_on_button,
    .on_tick = clipboard_on_tick,
    .get_tooltip = clipboard_get_tooltip,
};

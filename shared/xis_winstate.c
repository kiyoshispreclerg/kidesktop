#include "xis_winstate.h"

#include <string.h>

#define SAVE_DELAY_MS 400
/* A geometry older than this has "settled": it is what the window was
 * before a maximize, whose own configure events may arrive ahead of the
 * state change that explains them. */
#define SETTLE_US (200 * 1000)

typedef struct {
    char *name;
    int what;
    int x, y, w, h;         /* current normal-state geometry */
    int sx, sy, sw, sh;     /* last settled geometry, put back on maximize */
    gint64 last_configure;
    gboolean have_geom;
    gboolean maximized;
    guint timer;
} WinState;

static char *state_path(const char *name)
{
    char *file = g_strdup_printf("winstate-%s.conf", name);
    char *path = g_build_filename(g_get_user_config_dir(), "kidesktop", file, NULL);
    g_free(file);
    return path;
}

static void state_save(WinState *s)
{
    GKeyFile *kf = g_key_file_new();
    if (s->have_geom && (s->what & XIS_WINSTATE_POS)) {
        g_key_file_set_integer(kf, "window", "x", s->x);
        g_key_file_set_integer(kf, "window", "y", s->y);
    }
    if (s->have_geom && (s->what & XIS_WINSTATE_SIZE)) {
        g_key_file_set_integer(kf, "window", "w", s->w);
        g_key_file_set_integer(kf, "window", "h", s->h);
    }
    g_key_file_set_boolean(kf, "window", "maximized", s->maximized);

    char *path = state_path(s->name);
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    char *data = g_key_file_to_data(kf, NULL, NULL);
    g_file_set_contents(path, data, -1, NULL);
    g_free(data);
    g_free(dir);
    g_free(path);
    g_key_file_free(kf);
}

static gboolean on_save_timer(gpointer data)
{
    WinState *s = data;
    s->timer = 0;
    state_save(s);
    return FALSE;
}

static void schedule_save(WinState *s)
{
    if (s->timer)
        g_source_remove(s->timer);
    s->timer = g_timeout_add(SAVE_DELAY_MS, on_save_timer, s);
}

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *ev, WinState *s)
{
    GdkWindowState st = w->window ? gdk_window_get_state(w->window) : 0;
    if (st & (GDK_WINDOW_STATE_MAXIMIZED | GDK_WINDOW_STATE_FULLSCREEN | GDK_WINDOW_STATE_ICONIFIED))
        return FALSE;
    gint64 now = g_get_monotonic_time();
    if (s->have_geom && now - s->last_configure > SETTLE_US) {
        s->sx = s->x;
        s->sy = s->y;
        s->sw = s->w;
        s->sh = s->h;
    }
    s->last_configure = now;
    gtk_window_get_position(GTK_WINDOW(w), &s->x, &s->y);
    s->w = ev->width;
    s->h = ev->height;
    s->have_geom = TRUE;
    schedule_save(s);
    return FALSE;
}

static gboolean on_window_state(GtkWidget *w, GdkEventWindowState *ev, WinState *s)
{
    (void)w;
    if (ev->changed_mask & GDK_WINDOW_STATE_MAXIMIZED) {
        s->maximized = (ev->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0;
        if (s->maximized && s->sw > 0) {
            s->x = s->sx;
            s->y = s->sy;
            s->w = s->sw;
            s->h = s->sh;
        }
        schedule_save(s);
    }
    return FALSE;
}

static void on_destroy(GtkWidget *w, WinState *s)
{
    (void)w;
    if (s->timer) {
        g_source_remove(s->timer);
        state_save(s);
    }
    g_free(s->name);
    g_free(s);
}

/* Moves the rectangle so most of it is on a monitor; FALSE when it is on
 * none (a monitor that is gone) and should be left to the window manager. */
static gboolean fit_on_screen(int *x, int *y, int w, int h)
{
    GdkScreen *scr = gdk_screen_get_default();
    int n = gdk_screen_get_n_monitors(scr);
    for (int i = 0; i < n; i++) {
        GdkRectangle m;
        gdk_screen_get_monitor_geometry(scr, i, &m);
        int cx = *x + w / 2, cy = *y + h / 2;
        if (cx < m.x || cx >= m.x + m.width || cy < m.y || cy >= m.y + m.height)
            continue;
        /* Keep the titlebar reachable: never above or past the edges. */
        if (*x + w > m.x + m.width)
            *x = m.x + m.width - w;
        if (*y + h > m.y + m.height)
            *y = m.y + m.height - h;
        if (*x < m.x)
            *x = m.x;
        if (*y < m.y)
            *y = m.y;
        return TRUE;
    }
    return FALSE;
}

void xis_winstate_restore(GtkWindow *win, const char *name, int what)
{
    char *path = state_path(name);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        GError *e = NULL;
        int w = 0, h = 0;
        if (what & XIS_WINSTATE_SIZE) {
            w = g_key_file_get_integer(kf, "window", "w", &e);
            if (e) {
                g_clear_error(&e);
                w = 0;
            }
            h = g_key_file_get_integer(kf, "window", "h", &e);
            if (e) {
                g_clear_error(&e);
                h = 0;
            }
            if (w > 0 && h > 0)
                gtk_window_set_default_size(win, w, h);
        }
        if (what & XIS_WINSTATE_POS) {
            int x = g_key_file_get_integer(kf, "window", "x", &e);
            gboolean ok = !e;
            g_clear_error(&e);
            int y = g_key_file_get_integer(kf, "window", "y", &e);
            ok = ok && !e;
            g_clear_error(&e);
            if (ok) {
                if (w <= 0 || h <= 0)
                    gtk_window_get_default_size(win, &w, &h);
                /* Without a known size, just require the corner to be on screen. */
                if (w <= 0 || h <= 0)
                    w = h = 1;
                if (fit_on_screen(&x, &y, w, h))
                    gtk_window_move(win, x, y);
            }
            if (g_key_file_get_boolean(kf, "window", "maximized", NULL))
                gtk_window_maximize(win);
        }
    }
    g_key_file_free(kf);
    g_free(path);
}

void xis_winstate_track(GtkWindow *win, const char *name, int what)
{
    WinState *s = g_new0(WinState, 1);
    s->name = g_strdup(name);
    s->what = what;
    /* Start from what is on disk, so a run that never moves the window
     * (opened maximized, say) does not forget the geometry behind it. */
    char *path = state_path(name);
    GKeyFile *kf = g_key_file_new();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        s->x = g_key_file_get_integer(kf, "window", "x", NULL);
        s->y = g_key_file_get_integer(kf, "window", "y", NULL);
        s->w = g_key_file_get_integer(kf, "window", "w", NULL);
        s->h = g_key_file_get_integer(kf, "window", "h", NULL);
        s->maximized = g_key_file_get_boolean(kf, "window", "maximized", NULL);
        s->have_geom = g_key_file_has_key(kf, "window", "x", NULL);
        s->sx = s->x;
        s->sy = s->y;
        s->sw = s->w;
        s->sh = s->h;
    }
    g_key_file_free(kf);
    g_free(path);
    g_signal_connect(win, "configure-event", G_CALLBACK(on_configure), s);
    g_signal_connect(win, "window-state-event", G_CALLBACK(on_window_state), s);
    g_signal_connect(win, "destroy", G_CALLBACK(on_destroy), s);
}

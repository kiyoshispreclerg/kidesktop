/*
 * keynav.c - keyboard navigation of the panels.
 *
 * focus_key= (default Ctrl+Alt+Tab) or `xispanel --focus` grabs the
 * keyboard on the bar under the pointer and puts a focus on one of its
 * items. An item is a whole widget, or one of the sub-items a widget
 * lists through PanelWidgetOps.key_item (a task button, a tray icon, a
 * pager cell). Focus is drawn by pointing the panel's ordinary hover
 * state at the item's center, so every widget's own hover highlight and
 * tooltip show it with no per-widget focus painting, and Enter is a
 * click at that same spot -- whatever a click does there (open a menu,
 * a container popup, xisserve) is what Enter does.
 *
 *   Left/Right (Up/Down on a vertical bar)  previous/next item, in screen order
 *   Up/Down (Left/Right on a vertical bar)  item above/below (tray/pager grids)
 *   Tab / Shift+Tab                         next/previous widget
 *   Home / End                              first/last item
 *   Enter, space                            click
 *   Shift+Enter                             middle click
 *   Menu, Shift+F10                         right click (context menu)
 *   Ctrl+Tab, F6                            next bar (other outputs too)
 *   Escape                                  leave (or close the open container popup)
 *
 * A menu opened this way runs its own keyboard navigation (menu.c) with
 * its own grab; Escape there comes back here, choosing an item ends
 * navigation (the action may have opened a window that wants the
 * keyboard). A container popup opened this way takes the focus into its
 * own widgets until Escape closes it. Any other click ends navigation,
 * since its target likely opened a window of its own.
 */
#include "xispanel.h"

#include "../shared/xis_direction.h"

#include <X11/keysym.h>

#include <stdio.h>
#include <string.h>

#define KEYNAV_MAX_ITEMS 256
#define KEYNAV_DEFAULT_KEY "Ctrl+Alt+Tab"


static int g_active;
static Panel *g_panel;       /* where the focus is: a bar, or an open container popup */
static Panel *g_bar;         /* the bar navigation started on (holds autohide open) */
static PanelWidget *g_focus; /* focused widget, and which of its items */
static int g_focus_item;
static int g_hotkey_bound;
static int g_keep; /* set by keynav_keep() during activate()'s click */

static int horizontal(const Panel *p)
{
    return p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM;
}

static int focusable(const PanelWidget *w)
{
    return w->ops->on_button || w->ops->get_tooltip;
}

static int widget_items(PanelWidget *w, KeyNavItem *out, int max)
{
    if (!focusable(w) || max <= 0) {
        return 0;
    }
    if (!w->ops->key_item) {
        out[0] = (KeyNavItem){w, 0, w->len, 0, w->thickness};
        return 1;
    }
    int x, len, y, thick;
    int n = w->ops->key_item(w, -1, &x, &len, &y, &thick);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (w->ops->key_item(w, i, &x, &len, &y, &thick) > i && len > 0 && thick > 0) {
            out[k++] = (KeyNavItem){w, x, len, y, thick};
        }
    }
    return k;
}

static int item_main(const KeyNavItem *it)
{
    return it->w->x + it->x;
}

static int item_cross(const KeyNavItem *it)
{
    return it->w->y + it->y;
}

/* Every item on p in screen order along the main axis (then the cross
 * axis), so Left/Right always move the way the arrow points -- also
 * under RTL, where panel_mirror_rtl() reversed where widgets sit. */
static int collect(Panel *p, KeyNavItem *out, int max)
{
    int n = 0;
    for (int i = 0; i < p->n_layout && n < max; i++) {
        n += widget_items(p->layout[i], out + n, max - n);
    }
    for (int i = 1; i < n; i++) {
        KeyNavItem t = out[i];
        int j = i - 1;
        while (j >= 0 && (item_main(&out[j]) > item_main(&t) ||
                          (item_main(&out[j]) == item_main(&t) && item_cross(&out[j]) > item_cross(&t)))) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = t;
    }
    return n;
}

static int find_focus(const KeyNavItem *items, int n);

int keynav_list_items(Panel *p, KeyNavItem *out, int max)
{
    return collect(p, out, max);
}

/* Spoken/shown name of an item: the widget's own label for it
 * (key_item_label), else its tooltip's text, else what kind of widget it
 * is -- so a screen reader never meets an unnamed button. */
void keynav_item_name(const KeyNavItem *it, int index_in_widget, char *buf, size_t bufsz)
{
    PanelWidget *w = it->w;
    buf[0] = 0;
    if (w->ops->key_item_label && w->ops->key_item_label(w, index_in_widget, buf, bufsz) && buf[0]) {
        return;
    }
    if (w->ops->get_tooltip) {
        char tip[256];
        int ax = 0, aw = 0, closable = 0;
        void *ctx = NULL;
        /* tray/pager pick the row from the hover's cross position */
        Panel *p = w->panel;
        PanelWidget *saved_w = p->hover_widget;
        int saved_x = p->hover_local_x, saved_y = p->hover_local_y;
        p->hover_widget = w;
        p->hover_local_x = it->x + it->len / 2;
        p->hover_local_y = it->y + it->thick / 2;
        int ok = w->ops->get_tooltip(w, it->x + it->len / 2, tip, sizeof(tip), &ax, &aw, &closable, &ctx);
        p->hover_widget = saved_w;
        p->hover_local_x = saved_x;
        p->hover_local_y = saved_y;
        if (ok && tip[0]) {
            for (char *c = tip; *c; c++) {
                if (*c == '\n') {
                    *c = ' ';
                }
            }
            snprintf(buf, bufsz, "%s", tip);
            return;
        }
    }
    static const struct {
        const char *type, *label;
    } kinds[] = {
        {"launcher", N_("Lancador")},        {"pager", N_("Area de trabalho")},
        {"tasklist", N_("Tarefa")},          {"clock", N_("Relogio")},
        {"tray", N_("Bandeja")},             {"volume", N_("Volume")},
        {"container", N_("Mais")},           {"winctl", N_("Janela ativa")},
        {"xisserve", N_("Menu de programas")}, {"globalmenu", N_("Menu global")},
        {"folder", N_("Pasta")},             {"notif", N_("Notificacoes")},
        {"network", N_("Rede")},             {"storage", N_("Armazenamento")},
        {"energy", N_("Energia")},           {"clipboard", N_("Area de transferencia")},
        {"monitor", N_("Monitor")},
    };
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (!strcmp(kinds[i].type, w->ops->type_name)) {
            snprintf(buf, bufsz, "%s", _(kinds[i].label));
            return;
        }
    }
    snprintf(buf, bufsz, "%s", w->ops->type_name);
}

int keynav_focus_index(const Panel *p, const KeyNavItem *items, int n)
{
    if (!g_active || p != g_panel) {
        return -1;
    }
    return find_focus(items, n);
}

/* Index of the focused item in items[], or -1 when it's gone (the
 * widget's item list shrank, a reload). Items of one widget are kept in
 * their own order by collect()'s stable sort, so the k-th item of g_focus
 * in items[] is its item k. */
static int find_focus(const KeyNavItem *items, int n)
{
    int k = 0, last = -1;
    for (int i = 0; i < n; i++) {
        if (items[i].w != g_focus) {
            continue;
        }
        if (k == g_focus_item) {
            return i;
        }
        last = i;
        k++;
    }
    return last; /* fewer items than before: the widget's last one */
}

static void set_focus(const KeyNavItem *items, int idx)
{
    const KeyNavItem *it = &items[idx];
    int k = 0;
    for (int i = 0; i < idx; i++) {
        if (items[i].w == it->w) {
            k++;
        }
    }
    g_focus = it->w;
    g_focus_item = k;
    a11y_focus_changed(g_panel, idx);
    int lx = it->x + it->len / 2, ly = it->y + it->thick / 2;
    panel_hover_set(g_panel, it->w, lx, ly);
    tooltip_show_at(g_panel, it->w->x + lx, it->w->y + ly);
    XFlush(g_dpy);
}

static int first_index(int n)
{
    return xis_direction_is_rtl() ? n - 1 : 0;
}

static void focus_first(void)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    if (n > 0) {
        set_focus(items, first_index(n));
    } else {
        g_focus = NULL;
        panel_hover_set(g_panel, NULL, 0, 0);
        tooltip_close();
    }
}

static int grab(Panel *p)
{
    return XGrabKeyboard(g_dpy, p->win, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
}

/* The bar on the output under the pointer, else the first bar. */
static Panel *pick_bar(void)
{
    Panel *list[64];
    int n = panel_list(list, 64);
    Window rw, cw;
    int rx = 0, ry = 0, wx, wy;
    unsigned int mask;
    int have_ptr = XQueryPointer(g_dpy, g_root, &rw, &cw, &rx, &ry, &wx, &wy, &mask);
    Panel *first = NULL;
    for (int i = 0; i < n; i++) {
        Panel *p = list[i];
        if (p->mode == MODE_CONTAINER || p->win == None) {
            continue;
        }
        if (!first) {
            first = p;
        }
        if (have_ptr && rx >= p->out_x && rx < p->out_x + p->out_w && ry >= p->out_y && ry < p->out_y + p->out_h) {
            return p;
        }
    }
    return first;
}

/* Moves navigation onto bar p: shows it if it autohides, takes the
 * keyboard there, focuses its first item. */
static int enter_bar(Panel *p)
{
    panel_autohide_hold(p, 1);
    if (!grab(p)) {
        panel_autohide_hold(p, 0);
        return 0;
    }
    if (g_bar && g_bar != p) {
        panel_hover_set(g_bar, NULL, 0, 0);
        panel_autohide_hold(g_bar, 0);
    }
    g_bar = p;
    g_panel = p;
    focus_first();
    return 1;
}

void keynav_start(Panel *p)
{
    if (g_active) {
        return;
    }
    if (panel_menu_is_open()) {
        panel_menu_close();
    }
    panel_container_close_all();
    if (!p) {
        p = pick_bar();
    }
    if (!p) {
        return;
    }
    g_active = 1;
    if (!enter_bar(p)) {
        fprintf(stderr, "xispanel: keynav: could not grab the keyboard\n");
        g_active = 0;
        g_bar = g_panel = NULL;
    }
}

void keynav_stop(void)
{
    if (!g_active) {
        return;
    }
    g_active = 0;
    if (g_panel) {
        panel_hover_set(g_panel, NULL, 0, 0);
    }
    if (g_bar) {
        panel_hover_set(g_bar, NULL, 0, 0);
        panel_autohide_hold(g_bar, 0);
    }
    tooltip_close();
    a11y_focus_cleared();
    /* An open container popup shares the one keyboard grab this client
     * can hold and still needs it for its own Escape. */
    if (!panel_open_container()) {
        XUngrabKeyboard(g_dpy, CurrentTime);
    }
    XFlush(g_dpy);
    g_panel = g_bar = NULL;
    g_focus = NULL;
}

int keynav_active(void)
{
    return g_active;
}

void keynav_keep(void)
{
    g_keep = 1;
}

int keynav_focus_item(const PanelWidget *w, int *x, int *len, int *y, int *thick)
{
    if (!g_active || w != g_focus || w->panel != g_panel) {
        return 0;
    }
    PanelWidget *fw = g_focus;
    if (fw->ops->key_item && fw->ops->key_item(fw, g_focus_item, x, len, y, thick) > g_focus_item) {
        return 1;
    }
    *x = 0;
    *len = fw->len;
    *y = 0;
    *thick = fw->thickness;
    return 1;
}

void keynav_panel_gone(Panel *p)
{
    if (g_active && (p == g_panel || p == g_bar)) {
        keynav_stop();
    }
}

void keynav_menu_closed(int by_selection)
{
    if (!g_active) {
        return;
    }
    if (by_selection) {
        keynav_stop();
        return;
    }
    if (!panel_open_container() && !grab(g_panel)) {
        keynav_stop();
        return;
    }
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    int cur = find_focus(items, n);
    if (cur >= 0) {
        set_focus(items, cur);
    } else {
        focus_first();
    }
}

static void hotkey_cb(PanelWidget *w)
{
    (void)w;
    if (g_active) {
        keynav_stop();
    } else {
        keynav_start(NULL);
    }
}

void keynav_configure(const char *spec)
{
    if (g_hotkey_bound) {
        hotkey_unregister_widget(NULL); /* the only hotkey registered without a widget */
        g_hotkey_bound = 0;
    }
    if (!spec || !*spec) {
        spec = KEYNAV_DEFAULT_KEY;
    }
    if (!strcmp(spec, "none")) {
        return;
    }
    g_hotkey_bound = hotkey_register(NULL, spec, hotkey_cb);
}

/* Enter/Menu: click the focused item. What happens next depends on what
 * the click opened -- see the file comment. */
static void activate(int button)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    int cur = find_focus(items, n);
    if (cur < 0) {
        return;
    }
    const KeyNavItem *it = &items[cur];
    Panel *before = panel_open_container();
    g_keep = 0;
    panel_click_at(g_panel, button, item_main(it) + it->len / 2, item_cross(it) + it->thick / 2);
    if (g_keep) {
        g_keep = 0;
        n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
        cur = find_focus(items, n);
        if (cur >= 0) {
            set_focus(items, cur);
        }
        return;
    }
    if (panel_menu_is_open()) {
        return; /* menu.c navigates it; keynav_menu_closed() picks up after */
    }
    Panel *q = panel_open_container();
    if (q && q != before) {
        panel_hover_set(g_panel, NULL, 0, 0);
        g_panel = q;
        focus_first();
        return;
    }
    keynav_stop();
}

static void move_linear(int delta)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    if (n == 0) {
        return;
    }
    int cur = find_focus(items, n);
    int next = cur < 0 ? first_index(n) : cur + delta;
    if (next < 0 || next >= n) {
        return; /* no wrap: an edge is a useful landmark without sight of the bar */
    }
    set_focus(items, next);
}

static void move_end(int last)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    if (n > 0) {
        set_focus(items, last ? n - 1 : 0);
    }
}

/* Tab: the first item of the next/previous widget in screen order. */
static void move_widget(int delta)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    int cur = find_focus(items, n);
    if (cur < 0) {
        if (n > 0) {
            set_focus(items, first_index(n));
        }
        return;
    }
    int i = cur;
    while (i >= 0 && i < n && items[i].w == items[cur].w) {
        i += delta;
    }
    if (i < 0 || i >= n) {
        return;
    }
    if (delta < 0) { /* back to that widget's own first item */
        while (i > 0 && items[i - 1].w == items[i].w) {
            i--;
        }
    }
    set_focus(items, i);
}

/* Across the bar's thickness: the nearest item above/below whose span
 * covers the focused item's center on the main axis -- the other row of
 * a two-row tray or pager. */
static void move_cross(int delta)
{
    KeyNavItem items[KEYNAV_MAX_ITEMS];
    int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
    int cur = find_focus(items, n);
    if (cur < 0) {
        return;
    }
    int c_main = item_main(&items[cur]) + items[cur].len / 2;
    int c_cross = item_cross(&items[cur]);
    int best = -1, best_d = 0;
    for (int i = 0; i < n; i++) {
        int m0 = item_main(&items[i]);
        if (i == cur || c_main < m0 || c_main >= m0 + items[i].len) {
            continue;
        }
        int d = (item_cross(&items[i]) - c_cross) * delta;
        if (d > 0 && (best < 0 || d < best_d)) {
            best = i;
            best_d = d;
        }
    }
    if (best >= 0) {
        set_focus(items, best);
    }
}

static void next_bar(void)
{
    Panel *list[64];
    int n = panel_list(list, 64);
    int start = 0;
    for (int i = 0; i < n; i++) {
        if (list[i] == g_bar) {
            start = i;
        }
    }
    for (int k = 1; k < n; k++) {
        Panel *p = list[(start + k) % n];
        if (p->mode != MODE_CONTAINER && p->win != None) {
            panel_container_close_all();
            panel_hover_set(g_panel, NULL, 0, 0);
            if (!enter_bar(p)) {
                keynav_stop();
            }
            return;
        }
    }
}

static void escape(void)
{
    Panel *q = panel_open_container();
    if (q && g_panel == q) {
        PanelWidget *owner = q->owner;
        panel_container_toggle(q); /* closes it, releasing the keyboard */
        g_panel = g_bar;
        if (!grab(g_panel)) {
            keynav_stop();
            return;
        }
        g_focus = owner;
        g_focus_item = 0;
        KeyNavItem items[KEYNAV_MAX_ITEMS];
        int n = collect(g_panel, items, KEYNAV_MAX_ITEMS);
        int cur = find_focus(items, n);
        if (cur >= 0) {
            set_focus(items, cur);
        } else {
            focus_first();
        }
        return;
    }
    keynav_stop();
}

static void handle_key(XKeyEvent *key)
{
    KeySym ks = XLookupKeysym(key, 0);
    int shift = (key->state & ShiftMask) != 0;
    int ctrl = (key->state & ControlMask) != 0;
    int horiz = horizontal(g_panel);
    KeySym main_prev = horiz ? XK_Left : XK_Up, main_next = horiz ? XK_Right : XK_Down;
    KeySym cross_prev = horiz ? XK_Up : XK_Left, cross_next = horiz ? XK_Down : XK_Right;

    if (ks == XK_Escape) {
        escape();
    } else if (ks == main_prev || ks == main_next) {
        move_linear(ks == main_next ? 1 : -1);
    } else if (ks == cross_prev || ks == cross_next) {
        move_cross(ks == cross_next ? 1 : -1);
    } else if ((ks == XK_Tab && ctrl) || ks == XK_F6) {
        next_bar();
    } else if (ks == XK_Tab || ks == XK_ISO_Left_Tab) {
        move_widget(shift || ks == XK_ISO_Left_Tab ? -1 : 1);
    } else if (ks == XK_Home || ks == XK_End) {
        move_end(ks == XK_End);
    } else if (ks == XK_Return || ks == XK_KP_Enter || ks == XK_space) {
        activate(shift ? Button2 : Button1);
    } else if (ks == XK_Menu || (ks == XK_F10 && shift)) {
        activate(Button3);
    }
}

int keynav_handle_event(const XEvent *ev)
{
    if (!g_active) {
        return 0;
    }
    if (ev->type == KeyPress) {
        XKeyEvent key = ev->xkey;
        handle_key(&key);
        return 1;
    }
    if (ev->type == KeyRelease) {
        return 1;
    }
    if (ev->type == ButtonPress) {
        keynav_stop(); /* and let the click through */
    }
    return 0;
}

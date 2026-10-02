/*
 * a11y.c - screen reader support: the panels as an ATK tree on AT-SPI.
 *
 * xispanel draws everything itself, so there is no toolkit to describe
 * it to a screen reader -- this file does, with three small AtkObject
 * types handed to atk-bridge:
 *
 *   application "xispanel"
 *     window "Painel <name>"      one per mapped panel (container popups too)
 *       push button "<name>"      one per keyboard-navigation item (keynav.c)
 *
 * Items are exactly what the arrow keys walk, named by keynav_item_name()
 * (the widget's own label, its tooltip, or its kind), each with a "click"
 * action and screen extents. Keyboard focus moving (keynav.c) becomes a
 * window activate + state-changed:focused pair, which is what Orca speaks;
 * nothing is announced for the mouse, same as any toolkit.
 *
 * Objects are cached by (panel, index) and re-read on every query, since
 * the items under an index change as tasks come and go. atk-bridge runs
 * on GLib's default main context, which a11y_fds()/a11y_dispatch() fold
 * into xispanel's own select() loop.
 *
 * Started only when the session asked for accessibility -- kisession's
 * a11y service sets QT_ACCESSIBILITY=1 and adds atk-bridge to GTK_MODULES
 * -- or XISPANEL_A11Y=1; XISPANEL_A11Y=0 keeps it off. Otherwise none of
 * this costs anything at runtime.
 */
#include "xispanel.h"

#include <atk-bridge.h>
#include <atk/atk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A11Y_MAX_PANELS 64
#define A11Y_MAX_ITEMS 256
#define A11Y_MAX_POLLFDS 32

static int g_on;
static AtkObject *g_root_obj;

typedef struct XpPanel XpPanel;

/* ---- item: one keynav item ------------------------------------------- */

typedef struct {
    AtkObject parent;
    XpPanel *owner; /* NULL once its panel is gone */
    int index;
    char name[256];
} XpItem;
typedef struct {
    AtkObjectClass parent;
} XpItemClass;

struct XpPanel {
    AtkObject parent;
    Panel *p; /* NULL once gone */
    XpItem *items[A11Y_MAX_ITEMS];
    char name[96];
};
typedef struct {
    AtkObjectClass parent;
} XpPanelClass;

typedef struct {
    AtkObject parent;
} XpApp;
typedef struct {
    AtkObjectClass parent;
} XpAppClass;

static void xp_item_action_init(AtkActionIface *iface);
static void xp_item_component_init(AtkComponentIface *iface);
static void xp_panel_component_init(AtkComponentIface *iface);
static void xp_panel_window_init(AtkWindowIface *iface)
{
    (void)iface; /* AtkWindow is only its activate/deactivate signals */
}

G_DEFINE_TYPE_WITH_CODE(XpItem, xp_item, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_ACTION, xp_item_action_init)
                            G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, xp_item_component_init))
G_DEFINE_TYPE_WITH_CODE(XpPanel, xp_panel, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_COMPONENT, xp_panel_component_init)
                            G_IMPLEMENT_INTERFACE(ATK_TYPE_WINDOW, xp_panel_window_init))
G_DEFINE_TYPE(XpApp, xp_app, ATK_TYPE_OBJECT)

/* The item at `index` right now, and its index within its own widget. */
static int item_lookup(XpItem *it, KeyNavItem *out, int *in_widget, Panel **out_p)
{
    if (!it->owner || !it->owner->p) {
        return 0;
    }
    Panel *p = it->owner->p;
    KeyNavItem items[A11Y_MAX_ITEMS];
    int n = keynav_list_items(p, items, A11Y_MAX_ITEMS);
    if (it->index >= n) {
        return 0;
    }
    int k = 0;
    for (int i = 0; i < it->index; i++) {
        if (items[i].w == items[it->index].w) {
            k++;
        }
    }
    *out = items[it->index];
    *in_widget = k;
    *out_p = p;
    return 1;
}

static const gchar *xp_item_get_name(AtkObject *obj)
{
    XpItem *it = (XpItem *)obj;
    KeyNavItem ki;
    int k;
    Panel *p;
    if (item_lookup(it, &ki, &k, &p)) {
        keynav_item_name(&ki, k, it->name, sizeof(it->name));
    }
    return it->name;
}

static AtkObject *xp_item_get_parent(AtkObject *obj)
{
    XpItem *it = (XpItem *)obj;
    return it->owner ? (AtkObject *)it->owner : NULL;
}

static gint xp_item_get_index_in_parent(AtkObject *obj)
{
    return ((XpItem *)obj)->index;
}

static AtkStateSet *xp_item_ref_state_set(AtkObject *obj)
{
    XpItem *it = (XpItem *)obj;
    AtkStateSet *s = atk_state_set_new();
    KeyNavItem ki;
    int k;
    Panel *p;
    if (!item_lookup(it, &ki, &k, &p)) {
        atk_state_set_add_state(s, ATK_STATE_DEFUNCT);
        return s;
    }
    atk_state_set_add_state(s, ATK_STATE_ENABLED);
    atk_state_set_add_state(s, ATK_STATE_SENSITIVE);
    atk_state_set_add_state(s, ATK_STATE_VISIBLE);
    atk_state_set_add_state(s, ATK_STATE_FOCUSABLE);
    if (p->mapped) {
        atk_state_set_add_state(s, ATK_STATE_SHOWING);
    }
    KeyNavItem items[A11Y_MAX_ITEMS];
    int n = keynav_list_items(p, items, A11Y_MAX_ITEMS);
    if (keynav_focus_index(p, items, n) == it->index) {
        atk_state_set_add_state(s, ATK_STATE_FOCUSED);
    }
    return s;
}

static void xp_item_init(XpItem *it)
{
    (void)it;
}

static void xp_item_class_init(XpItemClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_name = xp_item_get_name;
    ac->get_parent = xp_item_get_parent;
    ac->get_index_in_parent = xp_item_get_index_in_parent;
    ac->ref_state_set = xp_item_ref_state_set;
}

/* Panel-relative main/cross center -> physical window x/y. */
static void item_rect(Panel *p, const KeyNavItem *ki, gint *x, gint *y, gint *w, gint *h, AtkCoordType type)
{
    int horiz = (p->edge == EDGE_TOP || p->edge == EDGE_BOTTOM);
    int m = ki->w->x + ki->x, c = ki->w->y + ki->y;
    *x = horiz ? m : c;
    *y = horiz ? c : m;
    *w = horiz ? ki->len : ki->thick;
    *h = horiz ? ki->thick : ki->len;
    if (type == ATK_XY_SCREEN) {
        *x += p->x;
        *y += p->y;
    }
}

static void xp_item_get_extents(AtkComponent *c, gint *x, gint *y, gint *w, gint *h, AtkCoordType type)
{
    KeyNavItem ki;
    int k;
    Panel *p;
    *x = *y = *w = *h = 0;
    if (item_lookup((XpItem *)c, &ki, &k, &p)) {
        item_rect(p, &ki, x, y, w, h, type);
    }
}

static void xp_item_component_init(AtkComponentIface *iface)
{
    iface->get_extents = xp_item_get_extents;
}

static gboolean xp_item_do_action(AtkAction *a, gint i)
{
    KeyNavItem ki;
    int k;
    Panel *p;
    if (i != 0 || !item_lookup((XpItem *)a, &ki, &k, &p)) {
        return FALSE;
    }
    panel_click_at(p, Button1, ki.w->x + ki.x + ki.len / 2, ki.w->y + ki.y + ki.thick / 2);
    XFlush(g_dpy);
    return TRUE;
}

static gint xp_item_get_n_actions(AtkAction *a)
{
    (void)a;
    return 1;
}

static const gchar *xp_item_action_name(AtkAction *a, gint i)
{
    (void)a;
    return i == 0 ? "click" : NULL;
}

static const gchar *xp_item_action_localized_name(AtkAction *a, gint i)
{
    (void)a;
    return i == 0 ? _("Clicar") : NULL;
}

static void xp_item_action_init(AtkActionIface *iface)
{
    iface->do_action = xp_item_do_action;
    iface->get_n_actions = xp_item_get_n_actions;
    iface->get_name = xp_item_action_name;
    iface->get_localized_name = xp_item_action_localized_name;
}

/* ---- panel: one mapped panel ------------------------------------------ */

static XpPanel *g_panel_objs[A11Y_MAX_PANELS];
static int g_n_panel_objs;

/* Panels the tree shows right now: mapped ones, in config order. */
static int shown_panels(Panel **out, int max)
{
    Panel *all[A11Y_MAX_PANELS];
    int n = panel_list(all, A11Y_MAX_PANELS), k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (all[i]->mapped) {
            out[k++] = all[i];
        }
    }
    return k;
}

static XpPanel *panel_obj(Panel *p)
{
    for (int i = 0; i < g_n_panel_objs; i++) {
        if (g_panel_objs[i]->p == p) {
            return g_panel_objs[i];
        }
    }
    if (g_n_panel_objs >= A11Y_MAX_PANELS) {
        return NULL;
    }
    XpPanel *po = g_object_new(xp_panel_get_type(), NULL);
    po->p = p;
    atk_object_set_parent(ATK_OBJECT(po), g_root_obj);
    g_panel_objs[g_n_panel_objs++] = po;
    return po;
}

static XpItem *item_obj(XpPanel *po, int index)
{
    if (index < 0 || index >= A11Y_MAX_ITEMS) {
        return NULL;
    }
    if (!po->items[index]) {
        XpItem *it = g_object_new(xp_item_get_type(), NULL);
        it->owner = po;
        it->index = index;
        atk_object_set_role(ATK_OBJECT(it), ATK_ROLE_PUSH_BUTTON);
        po->items[index] = it;
    }
    return po->items[index];
}

static const gchar *xp_panel_get_name(AtkObject *obj)
{
    XpPanel *po = (XpPanel *)obj;
    if (po->p) {
        snprintf(po->name, sizeof(po->name), _("Painel %s"), po->p->name);
    }
    return po->name;
}

static gint xp_panel_get_n_children(AtkObject *obj)
{
    XpPanel *po = (XpPanel *)obj;
    if (!po->p) {
        return 0;
    }
    KeyNavItem items[A11Y_MAX_ITEMS];
    return keynav_list_items(po->p, items, A11Y_MAX_ITEMS);
}

static AtkObject *xp_panel_ref_child(AtkObject *obj, gint i)
{
    XpPanel *po = (XpPanel *)obj;
    if (i < 0 || i >= xp_panel_get_n_children(obj)) {
        return NULL;
    }
    XpItem *it = item_obj(po, i);
    return it ? g_object_ref(ATK_OBJECT(it)) : NULL;
}

static AtkObject *xp_panel_get_parent(AtkObject *obj)
{
    (void)obj;
    return g_root_obj;
}

static gint xp_panel_get_index_in_parent(AtkObject *obj)
{
    Panel *list[A11Y_MAX_PANELS];
    int n = shown_panels(list, A11Y_MAX_PANELS);
    for (int i = 0; i < n; i++) {
        if (list[i] == ((XpPanel *)obj)->p) {
            return i;
        }
    }
    return -1;
}

static AtkStateSet *xp_panel_ref_state_set(AtkObject *obj)
{
    XpPanel *po = (XpPanel *)obj;
    AtkStateSet *s = atk_state_set_new();
    if (!po->p) {
        atk_state_set_add_state(s, ATK_STATE_DEFUNCT);
        return s;
    }
    atk_state_set_add_state(s, ATK_STATE_ENABLED);
    atk_state_set_add_state(s, ATK_STATE_SENSITIVE); /* without it Orca says "grayed" */
    atk_state_set_add_state(s, ATK_STATE_VISIBLE);
    if (po->p->mapped) {
        atk_state_set_add_state(s, ATK_STATE_SHOWING);
    }
    KeyNavItem items[A11Y_MAX_ITEMS];
    int n = keynav_list_items(po->p, items, A11Y_MAX_ITEMS);
    if (keynav_focus_index(po->p, items, n) >= 0) {
        atk_state_set_add_state(s, ATK_STATE_ACTIVE);
    }
    return s;
}

static void xp_panel_get_extents(AtkComponent *c, gint *x, gint *y, gint *w, gint *h, AtkCoordType type)
{
    Panel *p = ((XpPanel *)c)->p;
    *x = *y = *w = *h = 0;
    if (p) {
        *x = type == ATK_XY_SCREEN ? p->x : 0;
        *y = type == ATK_XY_SCREEN ? p->y : 0;
        *w = p->w;
        *h = p->h;
    }
}

static void xp_panel_component_init(AtkComponentIface *iface)
{
    iface->get_extents = xp_panel_get_extents;
}

static void xp_panel_init(XpPanel *po)
{
    atk_object_set_role(ATK_OBJECT(po), ATK_ROLE_WINDOW);
}

static void xp_panel_class_init(XpPanelClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_name = xp_panel_get_name;
    ac->get_n_children = xp_panel_get_n_children;
    ac->ref_child = xp_panel_ref_child;
    ac->get_parent = xp_panel_get_parent;
    ac->get_index_in_parent = xp_panel_get_index_in_parent;
    ac->ref_state_set = xp_panel_ref_state_set;
}

/* ---- application root ------------------------------------------------- */

static gint xp_app_get_n_children(AtkObject *obj)
{
    (void)obj;
    Panel *list[A11Y_MAX_PANELS];
    return shown_panels(list, A11Y_MAX_PANELS);
}

static AtkObject *xp_app_ref_child(AtkObject *obj, gint i)
{
    (void)obj;
    Panel *list[A11Y_MAX_PANELS];
    int n = shown_panels(list, A11Y_MAX_PANELS);
    if (i < 0 || i >= n) {
        return NULL;
    }
    XpPanel *po = panel_obj(list[i]);
    return po ? g_object_ref(ATK_OBJECT(po)) : NULL;
}

static const gchar *xp_app_get_name(AtkObject *obj)
{
    (void)obj;
    return "xispanel";
}

static void xp_app_init(XpApp *a)
{
    atk_object_set_role(ATK_OBJECT(a), ATK_ROLE_APPLICATION);
}

static void xp_app_class_init(XpAppClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_n_children = xp_app_get_n_children;
    ac->ref_child = xp_app_ref_child;
    ac->get_name = xp_app_get_name;
}

static AtkObject *util_get_root(void)
{
    return g_root_obj;
}

static const gchar *util_toolkit_name(void)
{
    return "xispanel";
}

static const gchar *util_toolkit_version(void)
{
    return "1.0";
}

/* ---- focus events ----------------------------------------------------- */

static XpPanel *g_active_panel;
static XpItem *g_focused_item;

static void unfocus_item(void)
{
    if (g_focused_item) {
        atk_object_notify_state_change(ATK_OBJECT(g_focused_item), ATK_STATE_FOCUSED, FALSE);
        g_focused_item = NULL;
    }
}

void a11y_focus_changed(Panel *p, int item_index)
{
    if (!g_on) {
        return;
    }
    XpPanel *po = panel_obj(p);
    if (!po) {
        return;
    }
    if (po != g_active_panel) {
        unfocus_item();
        if (g_active_panel) {
            g_signal_emit_by_name(g_active_panel, "deactivate");
        }
        g_active_panel = po;
        g_signal_emit_by_name(po, "activate");
        atk_object_notify_state_change(ATK_OBJECT(po), ATK_STATE_ACTIVE, TRUE);
    }
    XpItem *it = item_obj(po, item_index);
    if (!it || it == g_focused_item) {
        if (it) { /* same index, possibly a different item now: say it again */
            g_object_notify(G_OBJECT(it), "accessible-name");
        }
        return;
    }
    unfocus_item();
    g_focused_item = it;
    atk_object_notify_state_change(ATK_OBJECT(it), ATK_STATE_FOCUSED, TRUE);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    atk_focus_tracker_notify(ATK_OBJECT(it));
    G_GNUC_END_IGNORE_DEPRECATIONS
}

void a11y_focus_cleared(void)
{
    if (!g_on) {
        return;
    }
    unfocus_item();
    if (g_active_panel) {
        atk_object_notify_state_change(ATK_OBJECT(g_active_panel), ATK_STATE_ACTIVE, FALSE);
        g_signal_emit_by_name(g_active_panel, "deactivate");
        g_active_panel = NULL;
    }
}

void a11y_reset(void)
{
    if (!g_on) {
        return;
    }
    a11y_focus_cleared();
    /* atk-bridge may still hold references; mark everything defunct
     * rather than pretend the objects are gone. */
    for (int i = 0; i < g_n_panel_objs; i++) {
        XpPanel *po = g_panel_objs[i];
        po->p = NULL;
        for (int k = 0; k < A11Y_MAX_ITEMS; k++) {
            if (po->items[k]) {
                po->items[k]->owner = NULL;
                g_object_unref(po->items[k]);
                po->items[k] = NULL;
            }
        }
        g_object_unref(po);
    }
    g_n_panel_objs = 0;
}

/* ---- startup and main loop -------------------------------------------- */

static int wanted(void)
{
    const char *v = getenv("XISPANEL_A11Y");
    if (v && *v) {
        return strcmp(v, "0") != 0;
    }
    const char *qt = getenv("QT_ACCESSIBILITY");
    const char *mods = getenv("GTK_MODULES");
    return (qt && !strcmp(qt, "1")) || (mods && strstr(mods, "atk-bridge"));
}

void a11y_init(void)
{
    if (g_on || !wanted()) {
        return;
    }
    AtkUtilClass *uc = g_type_class_ref(ATK_TYPE_UTIL);
    uc->get_root = util_get_root;
    uc->get_toolkit_name = util_toolkit_name;
    uc->get_toolkit_version = util_toolkit_version;
    g_root_obj = g_object_new(xp_app_get_type(), NULL);
    if (atk_bridge_adaptor_init(NULL, NULL) != 0) {
        fprintf(stderr, "xispanel: a11y: atk-bridge did not start, no screen reader support\n");
        return;
    }
    g_main_context_acquire(g_main_context_default());
    g_on = 1;
    fprintf(stderr, "xispanel: a11y: exposed on the accessibility bus\n");
}

static GPollFD g_pfds[A11Y_MAX_POLLFDS];
static int g_npfds;
static gint g_max_prio;
static int g_prepared;

int a11y_fds(fd_set *rfds, int maxfd, long *timeout_ms)
{
    if (!g_on) {
        return maxfd;
    }
    GMainContext *ctx = g_main_context_default();
    if (g_main_context_prepare(ctx, &g_max_prio)) {
        *timeout_ms = 0; /* something is ready to dispatch already */
    }
    gint timeout = -1;
    g_npfds = g_main_context_query(ctx, g_max_prio, &timeout, g_pfds, A11Y_MAX_POLLFDS);
    if (g_npfds > A11Y_MAX_POLLFDS) {
        g_npfds = A11Y_MAX_POLLFDS;
    }
    g_prepared = 1;
    for (int i = 0; i < g_npfds; i++) {
        if (g_pfds[i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR)) {
            FD_SET(g_pfds[i].fd, rfds);
            if (g_pfds[i].fd > maxfd) {
                maxfd = g_pfds[i].fd;
            }
        }
    }
    if (timeout >= 0 && (*timeout_ms < 0 || timeout < *timeout_ms)) {
        *timeout_ms = timeout;
    }
    return maxfd;
}

void a11y_dispatch(const fd_set *rfds)
{
    if (!g_on || !g_prepared) {
        return;
    }
    g_prepared = 0;
    for (int i = 0; i < g_npfds; i++) {
        g_pfds[i].revents = 0;
        if (rfds && FD_ISSET(g_pfds[i].fd, rfds)) {
            g_pfds[i].revents |= g_pfds[i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR);
        }
        /* select() here only watches reads; a socket wanting to write
         * is writable in practice, so let it. */
        g_pfds[i].revents |= g_pfds[i].events & G_IO_OUT;
    }
    GMainContext *ctx = g_main_context_default();
    if (g_main_context_check(ctx, g_max_prio, g_pfds, g_npfds)) {
        g_main_context_dispatch(ctx);
    }
}

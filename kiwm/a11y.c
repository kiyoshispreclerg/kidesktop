/*
 * a11y.c - Alt+Tab for screen readers.
 *
 * kiwm deliberately exposes nothing else over AT-SPI: screen readers
 * already follow window focus through the applications themselves. The
 * one thing only kiwm knows is which window the switcher is offering
 * while the modifier is still held -- with live preview off nothing has
 * focus yet, and even with it on Orca would just hear the app, not where
 * in the list it is. So while the switcher is open this publishes
 *
 *   application "kiwm"
 *     window "Alternar janelas"
 *       list item "<title>"      one per entry, focused = the selection
 *
 * and moves the focus with the selection: Orca reads "<title>, 2 of 5".
 * Titles are copied at open/step, never read back through the Client
 * pointers the switcher holds.
 *
 * Started only when the session asked for accessibility (QT_ACCESSIBILITY=1
 * or atk-bridge in GTK_MODULES, which kisession's a11y service sets; or
 * KIWM_A11Y=1, KIWM_A11Y=0 to force it off). atk-bridge runs on GLib's
 * default main context, folded into main.c's poll() by a11y_pollfds()/
 * a11y_dispatch(). a11y_stub.c replaces this file without atk-bridge.
 */
#include "a11y.h"

#include "../shared/xis_i18n.h"

#include <atk-bridge.h>
#include <atk/atk.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define A11Y_MAX_ITEMS 64
#define A11Y_MAX_POLLFDS 32

static int g_on;
static int g_open;
static int g_count;
static int g_selected = -1;
static char g_titles[A11Y_MAX_ITEMS][256];

typedef struct {
    AtkObject parent;
    int index;
} KwItem;
typedef struct {
    AtkObjectClass parent;
} KwItemClass;
typedef struct {
    AtkObject parent;
} KwWindow;
typedef struct {
    AtkObjectClass parent;
} KwWindowClass;
typedef struct {
    AtkObject parent;
} KwApp;
typedef struct {
    AtkObjectClass parent;
} KwAppClass;

static void kw_window_iface_init(AtkWindowIface *iface)
{
    (void)iface;
}

G_DEFINE_TYPE(KwItem, kw_item, ATK_TYPE_OBJECT)
G_DEFINE_TYPE_WITH_CODE(KwWindow, kw_window, ATK_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(ATK_TYPE_WINDOW, kw_window_iface_init))
G_DEFINE_TYPE(KwApp, kw_app, ATK_TYPE_OBJECT)

static AtkObject *g_app, *g_window;
static KwItem *g_items[A11Y_MAX_ITEMS];

static KwItem *item_obj(int i)
{
    if (i < 0 || i >= A11Y_MAX_ITEMS) {
        return NULL;
    }
    if (!g_items[i]) {
        KwItem *it = g_object_new(kw_item_get_type(), NULL);
        it->index = i;
        atk_object_set_role(ATK_OBJECT(it), ATK_ROLE_LIST_ITEM);
        atk_object_set_parent(ATK_OBJECT(it), g_window);
        g_items[i] = it;
    }
    return g_items[i];
}

/* ---- item ---- */

static const gchar *kw_item_get_name(AtkObject *obj)
{
    int i = ((KwItem *)obj)->index;
    return (g_open && i < g_count) ? g_titles[i] : "";
}

static gint kw_item_get_index_in_parent(AtkObject *obj)
{
    return ((KwItem *)obj)->index;
}

static AtkStateSet *kw_item_ref_state_set(AtkObject *obj)
{
    int i = ((KwItem *)obj)->index;
    AtkStateSet *s = atk_state_set_new();
    if (!g_open || i >= g_count) {
        atk_state_set_add_state(s, ATK_STATE_DEFUNCT);
        return s;
    }
    atk_state_set_add_state(s, ATK_STATE_ENABLED);
    atk_state_set_add_state(s, ATK_STATE_SENSITIVE);
    atk_state_set_add_state(s, ATK_STATE_VISIBLE);
    atk_state_set_add_state(s, ATK_STATE_SHOWING);
    atk_state_set_add_state(s, ATK_STATE_FOCUSABLE);
    atk_state_set_add_state(s, ATK_STATE_SELECTABLE);
    if (i == g_selected) {
        atk_state_set_add_state(s, ATK_STATE_FOCUSED);
        atk_state_set_add_state(s, ATK_STATE_SELECTED);
    }
    return s;
}

static void kw_item_init(KwItem *it)
{
    (void)it;
}

static void kw_item_class_init(KwItemClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_name = kw_item_get_name;
    ac->get_index_in_parent = kw_item_get_index_in_parent;
    ac->ref_state_set = kw_item_ref_state_set;
}

/* ---- window ---- */

static const gchar *kw_window_get_name(AtkObject *obj)
{
    (void)obj;
    return _("Alternar janelas");
}

static gint kw_window_get_n_children(AtkObject *obj)
{
    (void)obj;
    return g_open ? g_count : 0;
}

static AtkObject *kw_window_ref_child(AtkObject *obj, gint i)
{
    if (i < 0 || i >= kw_window_get_n_children(obj)) {
        return NULL;
    }
    KwItem *it = item_obj(i);
    return it ? g_object_ref(ATK_OBJECT(it)) : NULL;
}

static gint kw_window_get_index_in_parent(AtkObject *obj)
{
    (void)obj;
    return 0;
}

static AtkStateSet *kw_window_ref_state_set(AtkObject *obj)
{
    (void)obj;
    AtkStateSet *s = atk_state_set_new();
    atk_state_set_add_state(s, ATK_STATE_ENABLED);
    atk_state_set_add_state(s, ATK_STATE_SENSITIVE);
    if (g_open) {
        atk_state_set_add_state(s, ATK_STATE_VISIBLE);
        atk_state_set_add_state(s, ATK_STATE_SHOWING);
        atk_state_set_add_state(s, ATK_STATE_ACTIVE);
    }
    return s;
}

static void kw_window_init(KwWindow *w)
{
    atk_object_set_role(ATK_OBJECT(w), ATK_ROLE_WINDOW);
}

static void kw_window_class_init(KwWindowClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_name = kw_window_get_name;
    ac->get_n_children = kw_window_get_n_children;
    ac->ref_child = kw_window_ref_child;
    ac->get_index_in_parent = kw_window_get_index_in_parent;
    ac->ref_state_set = kw_window_ref_state_set;
}

/* ---- application ---- */

static gint kw_app_get_n_children(AtkObject *obj)
{
    (void)obj;
    return g_open ? 1 : 0;
}

static AtkObject *kw_app_ref_child(AtkObject *obj, gint i)
{
    (void)obj;
    return (g_open && i == 0) ? g_object_ref(g_window) : NULL;
}

static const gchar *kw_app_get_name(AtkObject *obj)
{
    (void)obj;
    return "kiwm";
}

static void kw_app_init(KwApp *a)
{
    atk_object_set_role(ATK_OBJECT(a), ATK_ROLE_APPLICATION);
}

static void kw_app_class_init(KwAppClass *klass)
{
    AtkObjectClass *ac = ATK_OBJECT_CLASS(klass);
    ac->get_n_children = kw_app_get_n_children;
    ac->ref_child = kw_app_ref_child;
    ac->get_name = kw_app_get_name;
}

static AtkObject *util_get_root(void)
{
    return g_app;
}

static const gchar *util_toolkit_name(void)
{
    return "kiwm";
}

static const gchar *util_toolkit_version(void)
{
    return "1.0";
}

/* ---- switcher hooks ---- */

static void copy_titles(const TabBoxState *state)
{
    g_count = state->count < A11Y_MAX_ITEMS ? state->count : A11Y_MAX_ITEMS;
    for (int i = 0; i < g_count; i++) {
        const Client *c = state->items[i];
        snprintf(g_titles[i], sizeof(g_titles[i]), "%s", (c && c->title[0]) ? c->title : _("Janela sem titulo"));
    }
}

static void focus_selected(int previous)
{
    if (previous >= 0 && previous != g_selected && previous < A11Y_MAX_ITEMS && g_items[previous]) {
        atk_object_notify_state_change(ATK_OBJECT(g_items[previous]), ATK_STATE_FOCUSED, FALSE);
    }
    KwItem *it = item_obj(g_selected);
    if (!it) {
        return;
    }
    atk_object_notify_state_change(ATK_OBJECT(it), ATK_STATE_FOCUSED, TRUE);
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    atk_focus_tracker_notify(ATK_OBJECT(it));
    G_GNUC_END_IGNORE_DEPRECATIONS
}

void a11y_tabbox_open(const TabBoxState *state)
{
    if (!g_on) {
        return;
    }
    copy_titles(state);
    g_selected = -1;
    g_open = 1;
    g_signal_emit_by_name(g_app, "children-changed::add", 0, g_window);
    g_signal_emit_by_name(g_window, "activate");
}

void a11y_tabbox_step(const TabBoxState *state)
{
    if (!g_on || !g_open) {
        return;
    }
    int previous = g_selected;
    copy_titles(state);
    g_selected = state->selected < g_count ? state->selected : -1;
    if (g_selected >= 0) {
        focus_selected(previous);
    }
}

void a11y_tabbox_close(void)
{
    if (!g_on || !g_open) {
        return;
    }
    if (g_selected >= 0 && g_selected < A11Y_MAX_ITEMS && g_items[g_selected]) {
        atk_object_notify_state_change(ATK_OBJECT(g_items[g_selected]), ATK_STATE_FOCUSED, FALSE);
    }
    g_signal_emit_by_name(g_window, "deactivate");
    g_open = 0;
    g_selected = -1;
    g_signal_emit_by_name(g_app, "children-changed::remove", 0, g_window);
}

/* ---- startup and main loop ---- */

static int wanted(void)
{
    const char *v = getenv("KIWM_A11Y");
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
    g_app = g_object_new(kw_app_get_type(), NULL);
    g_window = g_object_new(kw_window_get_type(), NULL);
    atk_object_set_parent(g_window, g_app);
    if (atk_bridge_adaptor_init(NULL, NULL) != 0) {
        fprintf(stderr, "kiwm: a11y: atk-bridge did not start, Alt+Tab won't be announced\n");
        return;
    }
    g_main_context_acquire(g_main_context_default());
    g_on = 1;
    fprintf(stderr, "kiwm: a11y: window switcher exposed on the accessibility bus\n");
}

static GPollFD g_pfds[A11Y_MAX_POLLFDS];
static int g_npfds;
static gint g_max_prio;
static int g_prepared;

int a11y_pollfds(struct pollfd *out, int max, int *timeout)
{
    if (!g_on) {
        return 0;
    }
    GMainContext *ctx = g_main_context_default();
    if (g_main_context_prepare(ctx, &g_max_prio)) {
        *timeout = 0;
    }
    gint t = -1;
    g_npfds = g_main_context_query(ctx, g_max_prio, &t, g_pfds, A11Y_MAX_POLLFDS);
    if (g_npfds > A11Y_MAX_POLLFDS) {
        g_npfds = A11Y_MAX_POLLFDS;
    }
    if (g_npfds > max) {
        g_npfds = max;
    }
    g_prepared = 1;
    for (int i = 0; i < g_npfds; i++) {
        out[i].fd = g_pfds[i].fd;
        out[i].events = (short)g_pfds[i].events; /* G_IO_* are the poll(2) bits on POSIX */
        out[i].revents = 0;
    }
    if (t >= 0 && (*timeout < 0 || t < *timeout)) {
        *timeout = t;
    }
    return g_npfds;
}

void a11y_dispatch(const struct pollfd *fds, int n)
{
    if (!g_on || !g_prepared) {
        return;
    }
    g_prepared = 0;
    for (int i = 0; i < g_npfds; i++) {
        g_pfds[i].revents = (fds && i < n) ? (gushort)fds[i].revents : 0;
    }
    GMainContext *ctx = g_main_context_default();
    if (g_main_context_check(ctx, g_max_prio, g_pfds, g_npfds)) {
        g_main_context_dispatch(ctx);
    }
}

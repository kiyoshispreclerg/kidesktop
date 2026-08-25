/*
 * globalmenu.c - second search plugin (see ../xisserve.h). Finds the
 * active window's exported application menu the same way xispanel's own
 * `globalmenu` widget does (../../xispanel/widgets/globalmenu.c) --
 * Qt/KF5 apps set _KDE_NET_WM_APPMENU_SERVICE_NAME/_OBJECT_PATH directly
 * on their own top-level window, no registrar service needed just to
 * read it -- then fetches the *entire* menu tree in one GetLayout call
 * (depth=-1) and matches every leaf item's label against the query,
 * across every submenu depth at once. That "search the whole tree by
 * label, activate the match directly" shape is deliberate: it's what
 * makes this a HUD in the old Ubuntu Unity sense (type "export", get
 * straight to Gimp's File > Export As... without knowing which submenu
 * it lives under) rather than just a menu browser bolted onto search.
 *
 * Activating a result sends that item's own DBusMenu Event -- the exact
 * same effect as clicking it for real in the app's own menu bar -- via
 * ResultEntry::activate_fn, not a shell command.
 */
#include "../xisserve.h"
#include "dbusmenu.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <gdk/gdkx.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static Atom g_atom_service = None;
static Atom g_atom_path = None;
static Atom g_atom_active_window = None;

static void ensure_atoms(Display *dpy)
{
    if (g_atom_service == None) {
        g_atom_service = XInternAtom(dpy, "_KDE_NET_WM_APPMENU_SERVICE_NAME", False);
        g_atom_path = XInternAtom(dpy, "_KDE_NET_WM_APPMENU_OBJECT_PATH", False);
        g_atom_active_window = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    }
}

/* Both properties are plain STRING (not UTF8_STRING) -- same as the
 * xispanel globalmenu widget's own read_string_prop(), confirmed there
 * against the krunner_appmenu.py reference script and real Qt/KF5 apps. */
static void read_string_prop(Display *dpy, Window w, Atom atom, char *buf, size_t bufsz)
{
    buf[0] = 0;
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, w, atom, 0, 1024, False, XA_STRING, &actual_type, &actual_format, &n_items,
                            &bytes_after, &prop) == Success &&
        prop) {
        if (actual_type == XA_STRING && n_items > 0) {
            size_t len = n_items < bufsz - 1 ? n_items : bufsz - 1;
            memcpy(buf, prop, len);
            buf[len] = 0;
        }
        XFree(prop);
    }
}

static Window get_active_window(Display *dpy, Window root)
{
    Window result = None;
    Atom actual_type;
    int actual_format;
    unsigned long n_items, bytes_after;
    unsigned char *prop = NULL;
    if (XGetWindowProperty(dpy, root, g_atom_active_window, 0, 1, False, XA_WINDOW, &actual_type, &actual_format,
                            &n_items, &bytes_after, &prop) == Success &&
        prop) {
        if (actual_type == XA_WINDOW && n_items > 0) {
            result = *(Window *)prop;
        }
        XFree(prop);
    }
    return result;
}

/* Fallback icon for a matched item that has no icon-name/icon-data of
 * its own -- three bars, same "hamburger" glyph the xispanel globalmenu
 * widget draws for its own closed-mode button, just rasterized once into
 * a GdkPixbuf instead of drawn live into a panel's cairo_t. Cached after
 * the first call (theme/size never change mid-run); drawn in the
 * launcher's current --fg color via xisserve_get_fg_rgba() so it stays
 * legible whatever the active theme is. */
/* GTK2's GdkPixbuf has no gdk_pixbuf_get_from_surface() (that's a GTK3
 * addition) -- only gdk_pixbuf_get_from_drawable(), which needs a real
 * GdkDrawable (X pixmap/window), not a plain in-memory cairo image
 * surface. Reading the surface's own premultiplied-ARGB32 pixels
 * straight into a freshly built pixbuf (un-premultiplying as it goes,
 * since GdkPixbuf's RGBA is never premultiplied) sidesteps needing one. */
static GdkPixbuf *pixbuf_from_argb32_surface(cairo_surface_t *surf)
{
    int w = cairo_image_surface_get_width(surf);
    int h = cairo_image_surface_get_height(surf);
    int src_stride = cairo_image_surface_get_stride(surf);
    unsigned char *src = cairo_image_surface_get_data(surf);

    GdkPixbuf *pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, w, h);
    if (!pixbuf) return NULL;
    int dst_stride = gdk_pixbuf_get_rowstride(pixbuf);
    unsigned char *dst = gdk_pixbuf_get_pixels(pixbuf);

    for (int y = 0; y < h; y++) {
        const uint32_t *srow = (const uint32_t *)(src + y * src_stride);
        unsigned char *drow = dst + y * dst_stride;
        for (int x = 0; x < w; x++) {
            uint32_t px = srow[x];
            unsigned a = (px >> 24) & 0xff;
            unsigned r = (px >> 16) & 0xff;
            unsigned g = (px >> 8) & 0xff;
            unsigned b = px & 0xff;
            if (a > 0 && a < 255) {
                r = r * 255 / a;
                g = g * 255 / a;
                b = b * 255 / a;
            }
            drow[x * 4 + 0] = (unsigned char)(r > 255 ? 255 : r);
            drow[x * 4 + 1] = (unsigned char)(g > 255 ? 255 : g);
            drow[x * 4 + 2] = (unsigned char)(b > 255 ? 255 : b);
            drow[x * 4 + 3] = (unsigned char)a;
        }
    }
    return pixbuf;
}

static GdkPixbuf *hamburger_icon(void)
{
    static GdkPixbuf *cached = NULL;
    if (cached) return g_object_ref(cached);

    int s = XISSERVE_ICON_PX;
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, s, s);
    cairo_t *cr = cairo_create(surf);
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_set_line_width(cr, 1.6);
    for (int i = -1; i <= 1; i++) {
        double y = s / 2.0 + i * (s / 3.0);
        cairo_move_to(cr, s * 0.15, y);
        cairo_line_to(cr, s * 0.85, y);
        cairo_stroke(cr);
    }
    cairo_destroy(cr);
    cached = pixbuf_from_argb32_surface(surf);
    cairo_surface_destroy(surf);
    return cached ? g_object_ref(cached) : NULL;
}

typedef struct {
    char busname[128];
    char objpath[128];
    int32_t id;
} MenuActivateCtx;

static void menu_item_activate(ResultEntry *self)
{
    MenuActivateCtx *ctx = (MenuActivateCtx *)self->activate_data;
    if (ctx) dbusmenu_send_event(ctx->busname, ctx->objpath, ctx->id);
}

void plugin_globalmenu_search(const char *query, GPtrArray *results)
{
    if (!query || !*query) return;

    GdkDisplay *gdk_dpy = gdk_display_get_default();
    if (!gdk_dpy) return;
    Display *dpy = GDK_DISPLAY_XDISPLAY(gdk_dpy);
    Window root = GDK_ROOT_WINDOW();
    ensure_atoms(dpy);

    Window active = get_active_window(dpy, root);
    if (active == None) return;

    char busname[128] = "", objpath[128] = "";
    read_string_prop(dpy, active, g_atom_service, busname, sizeof(busname));
    read_string_prop(dpy, active, g_atom_path, objpath, sizeof(objpath));
    if (!busname[0] || !objpath[0]) return;

    /* static: DBUSMENU_MAX_ITEMS DbusMenuItems (128-byte label each) is
     * a little large for a comfortable stack frame called on every
     * keystroke -- same reasoning as dbusmenu.c's own icon_data buffer. */
    static DbusMenuItem items[DBUSMENU_MAX_ITEMS];
    static int ids[DBUSMENU_MAX_ITEMS];
    static int depths[DBUSMENU_MAX_ITEMS];
    int n = dbusmenu_fetch(busname, objpath, 0, -1, items, ids, depths, DBUSMENU_MAX_ITEMS);
    if (n <= 0) return;

    gchar *ql = g_utf8_casefold(query, -1);
    char top_label[128] = "";
    for (int i = 0; i < n; i++) {
        if (depths[i] == 0) snprintf(top_label, sizeof(top_label), "%s", items[i].label);
        if (items[i].is_separator || !items[i].enabled || !items[i].label[0]) continue;

        gchar *nl = g_utf8_casefold(items[i].label, -1);
        int match = strstr(nl, ql) != NULL;
        g_free(nl);
        if (!match) continue;

        ResultEntry *e = g_new0(ResultEntry, 1);
        snprintf(e->name, sizeof(e->name), "%s", items[i].label);
        if (top_label[0] && strcmp(top_label, items[i].label) != 0) {
            snprintf(e->subtitle, sizeof(e->subtitle), "Menu - %s", top_label);
        } else {
            snprintf(e->subtitle, sizeof(e->subtitle), "Menu");
        }
        e->from_desktop = FALSE;
        e->icon = items[i].icon ? g_object_ref(items[i].icon) : hamburger_icon();

        MenuActivateCtx *ctx = g_new0(MenuActivateCtx, 1);
        snprintf(ctx->busname, sizeof(ctx->busname), "%s", busname);
        snprintf(ctx->objpath, sizeof(ctx->objpath), "%s", objpath);
        ctx->id = ids[i];
        e->activate_fn = menu_item_activate;
        e->activate_data = ctx;
        e->activate_data_free = g_free;

        g_ptr_array_add(results, e);
    }
    g_free(ql);
    dbusmenu_free_item_icons(items, n);
}

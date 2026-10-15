/* kf_icons.c - themed icon lookup with a small cache, shared by the file
 * views and the places sidebar. */
#include "kf.h"
#include <string.h>

static GHashTable *cache;       /* "icon-string@size" -> GdkPixbuf* (owned) */

static GdkPixbuf *fallback(int size)
{
    GtkIconTheme *th = gtk_icon_theme_get_default();
    const char *names[] = { "text-x-generic", "gtk-file", "unknown", NULL };
    for (int i = 0; names[i]; i++) {
        GdkPixbuf *pb = gtk_icon_theme_load_icon(th, names[i], size, GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
        if (pb)
            return pb;
    }
    return NULL;
}

GdkPixbuf *kf_icon_lookup_gicon(GIcon *icon, int size)
{
    if (!cache)
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    char *str = icon ? g_icon_to_string(icon) : NULL;
    char *key = g_strdup_printf("%s@%d", str ? str : "?", size);
    g_free(str);
    GdkPixbuf *pb = g_hash_table_lookup(cache, key);
    if (pb) {
        g_free(key);
        return pb;
    }
    if (icon) {
        GtkIconInfo *info = gtk_icon_theme_lookup_by_gicon(gtk_icon_theme_get_default(), icon, size,
                                                           GTK_ICON_LOOKUP_FORCE_SIZE);
        if (info) {
            pb = gtk_icon_info_load_icon(info, NULL);
            gtk_icon_info_free(info);
        }
    }
    if (!pb)
        pb = fallback(size);
    if (!pb) {
        g_free(key);
        return NULL;
    }
    g_hash_table_insert(cache, key, pb);   /* the cache keeps the only owning ref */
    return pb;
}

GdkPixbuf *kf_icon_lookup(const char *icon_str, int size)
{
    GIcon *icon = icon_str && *icon_str ? g_icon_new_for_string(icon_str, NULL) : NULL;
    GdkPixbuf *pb = kf_icon_lookup_gicon(icon, size);
    if (icon)
        g_object_unref(icon);
    return pb;
}

/* Scale `src` down to fit a size x size box (never up); returns a new ref. */
GdkPixbuf *kf_icon_fit(GdkPixbuf *src, int size)
{
    int w = gdk_pixbuf_get_width(src), h = gdk_pixbuf_get_height(src);
    if (w <= size && h <= size)
        return g_object_ref(src);
    double f = (double)size / (w > h ? w : h);
    int nw = (int)(w * f + 0.5), nh = (int)(h * f + 0.5);
    return gdk_pixbuf_scale_simple(src, nw > 0 ? nw : 1, nh > 0 ? nh : 1, GDK_INTERP_BILINEAR);
}

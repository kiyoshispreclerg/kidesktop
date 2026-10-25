/* kp_ora.c - OpenRaster (.ora) reading and writing.
 *
 * An .ora is a ZIP with `mimetype`, `stack.xml`, one PNG per layer,
 * `mergedimage.png` and a thumbnail. Writing builds the ZIP by hand without
 * compression (the PNGs are already compressed), with crc32 from zlib;
 * reading finds entries through the central directory and inflates with
 * zlib. The file is treated as untrusted: every offset and size is checked.
 * GIMP, Krita and MyPaint open what we write. */
#include "kp.h"

#include <string.h>
#include <zlib.h>

#define MAX_SIDE 32767
#define MAX_LAYERS 512
#define MAX_ENTRY_BYTES (1u << 30)
#define MAX_DOC_BYTES ((guint64)2 << 30)   /* all layers' pixels */

static GQuark ora_error(void)
{
    return GDK_PIXBUF_ERROR;
}

#define FAIL(err, ...) g_set_error(err, ora_error(), GDK_PIXBUF_ERROR_CORRUPT_IMAGE, __VA_ARGS__)

gboolean kp_ora_is(const char *path)
{
    const char *dot = strrchr(path, '.');
    return dot && !strchr(dot, '/') && !g_ascii_strcasecmp(dot, ".ora");
}

/* ---- writing --------------------------------------------------------- */

typedef struct {
    char *name;
    guint32 crc, size, offset;
} ZipEntry;

static void put16(GByteArray *a, guint v)
{
    guint8 b[2] = { v & 0xff, (v >> 8) & 0xff };
    g_byte_array_append(a, b, 2);
}

static void put32(GByteArray *a, guint32 v)
{
    put16(a, v & 0xffff);
    put16(a, v >> 16);
}

static gboolean zip_add(GByteArray *zip, GArray *entries, const char *name, const void *data, gsize len, GError **err)
{
    if (len > 0xffffffffu || zip->len > 0xffffffffu - len - 1024) {
        g_set_error(err, ora_error(), GDK_PIXBUF_ERROR_FAILED, _("The project is too large for an .ora file."));
        return FALSE;
    }
    ZipEntry e = { g_strdup(name), (guint32)crc32(crc32(0, NULL, 0), data, len), (guint32)len, zip->len };
    g_array_append_val(entries, e);
    put32(zip, 0x04034b50);
    put16(zip, 20);     /* version needed */
    put16(zip, 0);      /* flags */
    put16(zip, 0);      /* stored */
    put16(zip, 0);      /* time */
    put16(zip, 0x21);   /* date: 1980-01-01 */
    put32(zip, e.crc);
    put32(zip, e.size);
    put32(zip, e.size);
    put16(zip, strlen(name));
    put16(zip, 0);
    g_byte_array_append(zip, (const guint8 *)name, strlen(name));
    g_byte_array_append(zip, data, len);
    return TRUE;
}

static void zip_finish(GByteArray *zip, GArray *entries)
{
    guint32 start = zip->len;
    for (guint i = 0; i < entries->len; i++) {
        ZipEntry *e = &g_array_index(entries, ZipEntry, i);
        put32(zip, 0x02014b50);
        put16(zip, 20);
        put16(zip, 20);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0x21);
        put32(zip, e->crc);
        put32(zip, e->size);
        put32(zip, e->size);
        put16(zip, strlen(e->name));
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put16(zip, 0);
        put32(zip, 0);
        put32(zip, e->offset);
        g_byte_array_append(zip, (const guint8 *)e->name, strlen(e->name));
    }
    guint32 size = zip->len - start;
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, entries->len);
    put16(zip, entries->len);
    put32(zip, size);
    put32(zip, start);
    put16(zip, 0);
}

static gboolean add_pixbuf_png(GByteArray *zip, GArray *entries, const char *name, GdkPixbuf *pb, GError **err)
{
    gchar *buf = NULL;
    gsize len = 0;
    if (!gdk_pixbuf_save_to_buffer(pb, &buf, &len, "png", err, "compression", "6", NULL))
        return FALSE;
    gboolean ok = zip_add(zip, entries, name, buf, len, err);
    g_free(buf);
    return ok;
}

gboolean kp_ora_save(KpDoc *d, const char *path, GError **err)
{
    GByteArray *zip = g_byte_array_new();
    GArray *entries = g_array_new(FALSE, FALSE, sizeof(ZipEntry));
    GString *xml = g_string_new(NULL);
    char num[G_ASCII_DTOSTR_BUF_SIZE];
    gboolean ok = zip_add(zip, entries, "mimetype", "image/openraster", 16, err);

    g_string_append_printf(xml, "<?xml version='1.0' encoding='UTF-8'?>\n<image w=\"%d\" h=\"%d\" version=\"0.0.3\">\n"
                                " <stack>\n", d->w, d->h);
    /* stack.xml lists the top layer first */
    for (int i = (int)d->layers->len - 1; ok && i >= 0; i--) {
        KpLayer *l = g_ptr_array_index(d->layers, i);
        char name[32];
        g_snprintf(name, sizeof(name), "data/layer%d.png", i);
        GdkPixbuf *pb = kp_surface_to_pixbuf(l->surf, FALSE);
        ok = add_pixbuf_png(zip, entries, name, pb, err);
        g_object_unref(pb);
        char *esc = g_markup_escape_text(l->name, -1);
        g_ascii_formatd(num, sizeof(num), "%.4f", l->opacity);
        g_string_append_printf(xml, "  <layer name=\"%s\" src=\"%s\" x=\"0\" y=\"0\" opacity=\"%s\" "
                                    "visibility=\"%s\" composite-op=\"svg:src-over\"%s/>\n",
                               esc, name, num, l->visible ? "visible" : "hidden",
                               i == d->active ? " selected=\"true\"" : "");
        g_free(esc);
    }
    g_string_append(xml, " </stack>\n</image>\n");
    if (ok)
        ok = zip_add(zip, entries, "stack.xml", xml->str, xml->len, err);

    if (ok) {
        cairo_surface_t *flat = kp_doc_flatten(d);
        GdkPixbuf *merged = kp_surface_to_pixbuf(flat, FALSE);
        cairo_surface_destroy(flat);
        ok = add_pixbuf_png(zip, entries, "mergedimage.png", merged, err);
        if (ok) {
            double sc = MIN(1.0, 256.0 / MAX(d->w, d->h));
            GdkPixbuf *thumb = gdk_pixbuf_scale_simple(merged, MAX(1, (int)(d->w * sc)), MAX(1, (int)(d->h * sc)),
                                                       GDK_INTERP_BILINEAR);
            ok = add_pixbuf_png(zip, entries, "Thumbnails/thumbnail.png", thumb, err);
            g_object_unref(thumb);
        }
        g_object_unref(merged);
    }
    if (ok) {
        zip_finish(zip, entries);
        /* written to a temporary file and renamed: a failed save never
         * leaves a truncated project behind */
        ok = g_file_set_contents(path, (const char *)zip->data, zip->len, err);
    }
    for (guint i = 0; i < entries->len; i++)
        g_free(g_array_index(entries, ZipEntry, i).name);
    g_array_free(entries, TRUE);
    g_byte_array_free(zip, TRUE);
    g_string_free(xml, TRUE);
    return ok;
}

/* ---- reading --------------------------------------------------------- */

typedef struct {
    const guint8 *data;
    gsize len;
} Blob;

typedef struct {
    char *name;
    guint method, crc;
    guint64 csize, usize, offset;
} CdEntry;

static guint rd16(const guint8 *p)
{
    return p[0] | (p[1] << 8);
}

static guint32 rd32(const guint8 *p)
{
    return rd16(p) | ((guint32)rd16(p + 2) << 16);
}

static void cd_free(GPtrArray *cd)
{
    for (guint i = 0; i < cd->len; i++) {
        CdEntry *e = g_ptr_array_index(cd, i);
        g_free(e->name);
        g_free(e);
    }
    g_ptr_array_free(cd, TRUE);
}

/* The central directory, or NULL (with err set). */
static GPtrArray *zip_directory(Blob z, GError **err)
{
    if (z.len < 22) {
        FAIL(err, _("Not an OpenRaster file."));
        return NULL;
    }
    gsize eocd = G_MAXSIZE;
    gsize lowest = z.len > 22 + 0xffff ? z.len - 22 - 0xffff : 0;
    for (gsize i = z.len - 22 + 1; i-- > lowest;)
        if (rd32(z.data + i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == G_MAXSIZE) {
        FAIL(err, _("Not an OpenRaster file."));
        return NULL;
    }
    guint count = rd16(z.data + eocd + 10);
    guint64 cd_off = rd32(z.data + eocd + 16);
    GPtrArray *cd = g_ptr_array_new();
    gsize p = cd_off;
    for (guint i = 0; i < count; i++) {
        if (p > z.len || z.len - p < 46 || rd32(z.data + p) != 0x02014b50) {
            FAIL(err, _("The file is damaged."));
            cd_free(cd);
            return NULL;
        }
        guint nlen = rd16(z.data + p + 28), elen = rd16(z.data + p + 30), clen = rd16(z.data + p + 32);
        if (z.len - p - 46 < (gsize)nlen + elen + clen) {
            FAIL(err, _("The file is damaged."));
            cd_free(cd);
            return NULL;
        }
        CdEntry *e = g_new0(CdEntry, 1);
        e->name = g_strndup((const char *)z.data + p + 46, nlen);
        e->method = rd16(z.data + p + 10);
        e->crc = rd32(z.data + p + 16);
        e->csize = rd32(z.data + p + 20);
        e->usize = rd32(z.data + p + 24);
        e->offset = rd32(z.data + p + 42);
        g_ptr_array_add(cd, e);
        p += 46 + nlen + elen + clen;
    }
    return cd;
}

static CdEntry *cd_find(GPtrArray *cd, const char *name)
{
    for (guint i = 0; i < cd->len; i++) {
        CdEntry *e = g_ptr_array_index(cd, i);
        if (!strcmp(e->name, name))
            return e;
    }
    return NULL;
}

/* The uncompressed bytes of one entry, g_malloc'ed; NULL with err set. */
static guint8 *zip_extract(Blob z, const CdEntry *e, gsize *out_len, GError **err)
{
    if (e->usize > MAX_ENTRY_BYTES || e->offset > z.len || z.len - e->offset < 30 ||
        rd32(z.data + e->offset) != 0x04034b50) {
        FAIL(err, _("The file is damaged."));
        return NULL;
    }
    guint64 start = e->offset + 30 + rd16(z.data + e->offset + 26) + rd16(z.data + e->offset + 28);
    if (start > z.len || z.len - start < e->csize) {
        FAIL(err, _("The file is damaged."));
        return NULL;
    }
    const guint8 *src = z.data + start;
    guint8 *out = g_try_malloc(e->usize + 1);
    if (!out) {
        g_set_error(err, ora_error(), GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY, _("Not enough memory."));
        return NULL;
    }
    if (e->method == 0) {
        if (e->csize != e->usize) {
            FAIL(err, _("The file is damaged."));
            g_free(out);
            return NULL;
        }
        memcpy(out, src, e->usize);
    } else if (e->method == 8) {
        z_stream zs = { 0 };
        zs.next_in = (Bytef *)src;
        zs.avail_in = e->csize;
        zs.next_out = out;
        zs.avail_out = e->usize;
        int r = inflateInit2(&zs, -15);
        if (r == Z_OK) {
            r = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
        }
        if (r != Z_STREAM_END || zs.total_out != e->usize) {
            FAIL(err, _("The file is damaged."));
            g_free(out);
            return NULL;
        }
    } else {
        FAIL(err, _("Unsupported compression in the file."));
        g_free(out);
        return NULL;
    }
    if ((guint32)crc32(crc32(0, NULL, 0), out, e->usize) != e->crc) {
        FAIL(err, _("The file is damaged."));
        g_free(out);
        return NULL;
    }
    out[e->usize] = '\0';
    *out_len = e->usize;
    return out;
}

typedef struct {
    char *name, *src;
    int x, y;
    double opacity;
    gboolean visible, selected;
} LayerSpec;

typedef struct {
    int w, h;
    GArray *layers;   /* LayerSpec, in file order (top first) */
} Stack;

static void spec_free(LayerSpec *s)
{
    g_free(s->name);
    g_free(s->src);
}

static void stack_start(GMarkupParseContext *ctx, const gchar *el, const gchar **names, const gchar **values,
                        gpointer data, GError **err)
{
    (void)ctx;
    (void)err;
    Stack *st = data;
    if (!strcmp(el, "image")) {
        for (int i = 0; names[i]; i++) {
            if (!strcmp(names[i], "w"))
                st->w = (int)g_ascii_strtoll(values[i], NULL, 10);
            else if (!strcmp(names[i], "h"))
                st->h = (int)g_ascii_strtoll(values[i], NULL, 10);
        }
    } else if (!strcmp(el, "layer")) {
        LayerSpec s = { NULL, NULL, 0, 0, 1, TRUE, FALSE };
        for (int i = 0; names[i]; i++) {
            if (!strcmp(names[i], "name"))
                s.name = g_strdup(values[i]);
            else if (!strcmp(names[i], "src"))
                s.src = g_strdup(values[i]);
            else if (!strcmp(names[i], "x"))
                s.x = (int)CLAMP(g_ascii_strtoll(values[i], NULL, 10), -MAX_SIDE, MAX_SIDE);
            else if (!strcmp(names[i], "y"))
                s.y = (int)CLAMP(g_ascii_strtoll(values[i], NULL, 10), -MAX_SIDE, MAX_SIDE);
            else if (!strcmp(names[i], "opacity"))
                s.opacity = CLAMP(g_ascii_strtod(values[i], NULL), 0, 1);
            else if (!strcmp(names[i], "visibility"))
                s.visible = strcmp(values[i], "hidden") != 0;
            else if (!strcmp(names[i], "selected"))
                s.selected = !strcmp(values[i], "true");
        }
        if (!s.name)
            s.name = g_strdup(_("Layer"));
        g_array_append_val(st->layers, s);
    }
}

/* Decodes a layer PNG and places it, at (x,y), on a doc-size transparent
 * surface. */
static cairo_surface_t *layer_surface(const guint8 *png, gsize len, const LayerSpec *spec, int w, int h, GError **err)
{
    GdkPixbufLoader *ld = gdk_pixbuf_loader_new();
    GError *lerr = NULL;
    gboolean ok = gdk_pixbuf_loader_write(ld, png, len, &lerr);
    ok = gdk_pixbuf_loader_close(ld, ok ? &lerr : NULL) && ok;
    if (!ok) {
        if (lerr)
            g_propagate_error(err, lerr);
        else
            FAIL(err, _("A layer image is damaged."));
        g_object_unref(ld);
        return NULL;
    }
    GdkPixbuf *pb = gdk_pixbuf_loader_get_pixbuf(ld);
    if (!pb) {
        FAIL(err, _("A layer image is damaged."));
        g_object_unref(ld);
        return NULL;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        g_set_error(err, ora_error(), GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY, _("Not enough memory."));
        cairo_surface_destroy(s);
        g_object_unref(ld);
        return NULL;
    }
    cairo_t *cr = cairo_create(s);
    gdk_cairo_set_source_pixbuf(cr, pb, spec->x, spec->y);
    cairo_paint(cr);
    cairo_destroy(cr);
    g_object_unref(ld);
    return s;
}

KpDoc *kp_ora_load(const char *path, GError **err)
{
    GMappedFile *mf = g_mapped_file_new(path, FALSE, err);
    if (!mf)
        return NULL;
    Blob z = { (const guint8 *)g_mapped_file_get_contents(mf), g_mapped_file_get_length(mf) };
    KpDoc *doc = NULL;
    GPtrArray *cd = zip_directory(z, err);
    Stack st = { 0, 0, g_array_new(FALSE, FALSE, sizeof(LayerSpec)) };
    guint8 *buf = NULL;
    gsize len = 0;

    if (!cd)
        goto out;
    CdEntry *e = cd_find(cd, "stack.xml");
    if (!e || !(buf = zip_extract(z, e, &len, err))) {
        if (!e)
            FAIL(err, _("Not an OpenRaster file."));
        goto out;
    }
    GMarkupParser parser = { stack_start, NULL, NULL, NULL, NULL };
    GMarkupParseContext *ctx = g_markup_parse_context_new(&parser, 0, &st, NULL);
    gboolean parsed = g_markup_parse_context_parse(ctx, (const char *)buf, len, err) &&
                      g_markup_parse_context_end_parse(ctx, err);
    g_markup_parse_context_free(ctx);
    g_free(buf);
    buf = NULL;
    if (!parsed)
        goto out;
    if (st.w < 1 || st.h < 1 || st.w > MAX_SIDE || st.h > MAX_SIDE || st.layers->len == 0 ||
        st.layers->len > MAX_LAYERS) {
        FAIL(err, _("The project has no valid image size or layers."));
        goto out;
    }
    if ((guint64)st.w * st.h * 4 * st.layers->len > MAX_DOC_BYTES) {
        g_set_error(err, ora_error(), GDK_PIXBUF_ERROR_INSUFFICIENT_MEMORY, _("The project is too large (%d x %d, %u layers)."),
                    st.w, st.h, st.layers->len);
        goto out;
    }

    doc = g_new0(KpDoc, 1);
    doc->w = st.w;
    doc->h = st.h;
    doc->layered = TRUE;
    doc->layers = g_ptr_array_new();
    doc->active = -1;
    /* file order is top first; the document keeps the bottom first */
    for (int i = (int)st.layers->len - 1; i >= 0; i--) {
        LayerSpec *sp = &g_array_index(st.layers, LayerSpec, i);
        CdEntry *le = sp->src ? cd_find(cd, sp->src) : NULL;
        if (!le) {
            FAIL(err, _("A layer image is missing from the file."));
            kp_doc_free(doc);
            doc = NULL;
            goto out;
        }
        buf = zip_extract(z, le, &len, err);
        cairo_surface_t *surf = buf ? layer_surface(buf, len, sp, st.w, st.h, err) : NULL;
        g_free(buf);
        buf = NULL;
        if (!surf) {
            kp_doc_free(doc);
            doc = NULL;
            goto out;
        }
        KpLayer *l = g_new0(KpLayer, 1);
        l->surf = surf;
        l->w = st.w;
        l->h = st.h;
        l->name = sp->name;
        sp->name = NULL;
        l->visible = sp->visible;
        l->opacity = sp->opacity;
        g_ptr_array_add(doc->layers, l);
        if (sp->selected)
            doc->active = doc->layers->len - 1;
    }
    if (doc->active < 0)
        doc->active = doc->layers->len - 1;
out:
    for (guint i = 0; i < st.layers->len; i++)
        spec_free(&g_array_index(st.layers, LayerSpec, i));
    g_array_free(st.layers, TRUE);
    if (cd)
        cd_free(cd);
    g_mapped_file_unref(mf);
    return doc;
}

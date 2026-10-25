/* kp.h - kipaint's shared types and state.
 *
 * One process per window, one document per process. Pixels live in cairo
 * ARGB32 image surfaces (premultiplied), one per layer. Tools draw on the
 * active layer in image coordinates; the canvas maps them to the screen. */
#ifndef KP_H
#define KP_H

#include "../shared/xis_i18n.h"

#include <gtk/gtk.h>

#define KIPAINT_VERSION "0.1.0"
#define KP_PALETTE_SIZE 28

/* Non-premultiplied, 0..1. */
typedef struct {
    double r, g, b, a;
} KpColor;

typedef struct {
    cairo_surface_t *surf;
    int w, h;
    char *name;
    gboolean visible;
    double opacity;      /* 0..1 */
} KpLayer;

/* The editable, undoable properties of a layer. */
typedef struct {
    char *name;
    gboolean visible;
    double opacity;
} KpLayerProps;

typedef struct {
    int w, h;
    GPtrArray *layers;   /* KpLayer, bottom first */
    int active;
    char *path;          /* NULL: never saved */
    /* TRUE: a layered project (.ora), where Save writes the .ora and the
     * flat formats are Export. FALSE: a flat image, where Save rewrites the
     * file in its own format. See PLAN.md 1.1. */
    gboolean layered;
} KpDoc;

/* A tool's handlers get image coordinates (fractional) and the button that
 * started the stroke (1 = color 1, 3 = color 2). */
typedef struct {
    const char *id;      /* config key */
    const char *name;    /* N_() */
    guint key;           /* single-key shortcut on the canvas */
    void (*draw_icon)(cairo_t *cr);   /* into a 16x16 box */
    int default_size, max_size;       /* max_size 0: no size option */
    gboolean outline;    /* show the brush outline under the pointer */
    gboolean square;     /* ...as a square (aliased tools) */
    void (*press)(double x, double y, int button);
    void (*motion)(double x, double y);
    void (*release)(void);
} KpTool;

extern const KpTool kp_tool_pencil, kp_tool_brush, kp_tool_eraser, kp_tool_fill, kp_tool_picker;

typedef struct {
    KpDoc *doc;
    KpColor color[2];
    KpColor palette[KP_PALETTE_SIZE];
    const KpTool *tool, *prev_tool;
    int *sizes;                  /* per tool, indexed like kp_tools[] */
    gboolean eraser_transparent;
    gboolean grid;
    gboolean layers_panel;       /* shown */
    int new_w, new_h;
    GtkWidget *win;
} KpState;

extern KpState kp;
extern const KpTool *const kp_tools[];
extern const int kp_n_tools;

/* kp_layer.c */
KpLayer *kp_layer_new(int w, int h, const char *name);
void kp_layer_free(KpLayer *l);

/* kp_doc.c */
KpDoc *kp_doc_new(int w, int h, const KpColor *fill);
KpDoc *kp_doc_new_from_surface(cairo_surface_t *surf);
void kp_doc_free(KpDoc *d);
KpLayer *kp_doc_layer(KpDoc *d);
cairo_surface_t *kp_doc_flatten(KpDoc *d);
gboolean kp_doc_pick(KpDoc *d, int x, int y, KpColor *out);
/* Raw structure edits; the callers record them in the undo history. */
void kp_doc_layer_insert(KpDoc *d, KpLayer *l, int index);   /* takes l, makes it active */
KpLayer *kp_doc_layer_take(KpDoc *d, int index);              /* removes without freeing */
void kp_doc_layer_move(KpDoc *d, int from, int to);           /* makes it active */
void kp_layer_props_swap(KpLayer *l, KpLayerProps *p);
KpLayer *kp_layer_duplicate(const KpLayer *src);

/* kp_undo.c */
void kp_undo_begin(KpLayer *l);
void kp_undo_touch(int x, int y, int w, int h);
void kp_undo_end(void);
gboolean kp_undo_undo(void);
gboolean kp_undo_redo(void);
void kp_undo_layer_added(int index);                 /* already inserted in the doc */
void kp_undo_layer_removed(KpLayer *l, int index);   /* already taken out; the history owns it */
void kp_undo_layer_moved(int from, int to);          /* already moved */
void kp_undo_layer_props(KpLayer *l, KpLayerProps *old);   /* already changed; takes old.name */
void kp_undo_clear(void);
gboolean kp_undo_can_undo(void);
gboolean kp_undo_can_redo(void);
guint kp_undo_state(void);   /* identifies the current history position */

/* kp_paint.c */
guint32 kp_color_premul(const KpColor *c);
void kp_paint_square_line(KpLayer *l, int x0, int y0, int x1, int y1, int size, guint32 px, int rect[4]);
void kp_rect_union(int r[4], int x, int y, int w, int h);
void kp_square_stroke_begin(guint32 px, int size, double x, double y);
void kp_square_stroke_to(double x, double y);
void kp_square_stroke_end(void);

/* kp_layers_panel.c */
GtkWidget *kp_layers_panel_new(void);
void kp_layers_refresh(void);          /* the document's layer list changed */
void kp_layers_flush(void);            /* commit a pending opacity edit to the history */
void kp_layers_add(void);
void kp_layers_duplicate(void);
void kp_layers_delete(void);
void kp_layers_raise(void);
void kp_layers_lower(void);
void kp_layers_set_shown(gboolean shown);
gboolean kp_layers_shown(void);

/* kp_ora.c - OpenRaster */
gboolean kp_ora_is(const char *path);
KpDoc *kp_ora_load(const char *path, GError **err);
gboolean kp_ora_save(KpDoc *d, const char *path, GError **err);

/* kp_canvas.c */
GtkWidget *kp_canvas_new(void);
void kp_canvas_doc_changed(void);
void kp_canvas_damage(int x, int y, int w, int h);
void kp_canvas_redraw(void);
void kp_canvas_zoom_step(int dir);
void kp_canvas_zoom_100(void);
void kp_canvas_zoom_fit(void);
double kp_canvas_zoom(void);
void kp_canvas_tool_changed(void);
void kp_canvas_grab_focus(void);

/* kp_palette.c */
GtkWidget *kp_palette_new(void);
void kp_palette_refresh(void);

/* kp_toolbox.c */
GtkWidget *kp_toolbox_new(void);
void kp_toolbox_select(const KpTool *t);
int kp_tool_index(const KpTool *t);
int kp_tool_size(void);
void kp_tool_size_step(int dir);

/* kp_io.c */
cairo_surface_t *kp_io_load(const char *path, GError **err);
gboolean kp_io_save(KpDoc *d, const char *path, GError **err);
gboolean kp_io_writable(const char *path);   /* a flat format we write */
char *kp_io_format_name(const char *path);    /* "PNG", ... */
GdkPixbuf *kp_surface_to_pixbuf(cairo_surface_t *s, gboolean over_white);

/* kp_config.c */
void kp_config_load(void);
void kp_config_save(void);

/* kipaint.c */
void kp_status_pointer(int x, int y, gboolean inside);
void kp_doc_edited(void);   /* history changed: title, menu sensitivity */
gboolean kp_open_path(const char *path);

#endif /* KP_H */

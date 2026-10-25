/* kp_undo.c - tile-based undo.
 *
 * An operation keeps the original contents of only the 64x64 tiles it
 * touched; tools call kp_undo_touch() for a rectangle before drawing into
 * it. Undo and redo are the same thing: swap each saved tile with what the
 * layer has now, then move the operation to the other stack. History past
 * UNDO_LIMIT_BYTES drops its oldest operations. */
#include "kp.h"

#include <string.h>

#define TILE 64
#define UNDO_LIMIT_BYTES (64u * 1024 * 1024)

typedef struct {
    int tx, ty;
    guint32 *pix;   /* TILE*TILE, edge tiles only partly used */
} Tile;

/* Layer structure edits are operations too. The history owns a layer while
 * it is out of the document (`holds`), so undoing an add or redoing a
 * delete never loses pixels, and tile operations stay valid because they
 * are only reached after the structure edits made before them. */
typedef enum { OP_TILES, OP_ADD, OP_REMOVE, OP_MOVE, OP_PROPS } OpKind;

typedef struct {
    OpKind kind;
    KpLayer *layer;
    GArray *tiles;  /* Tile, OP_TILES only */
    guint id;
    int index, to;  /* OP_ADD/OP_REMOVE position; OP_MOVE from/to */
    gboolean holds; /* the layer is out of the document, owned by this op */
    KpLayerProps props;   /* OP_PROPS: the other state, swapped on each move */
} Op;

static GQueue g_undo = G_QUEUE_INIT, g_redo = G_QUEUE_INIT;   /* Op*, newest at head */
static size_t g_bytes;
static guint g_next_id = 1;
/* The state with every remaining operation undone: 0 is the image as
 * opened, otherwise the newest operation the limit dropped. */
static guint g_floor_id;

/* The operation being recorded. */
static Op *g_cur;
static guint8 *g_seen;   /* one byte per tile of the layer */
static int g_tiles_w, g_tiles_h;

static size_t op_bytes(const Op *op)
{
    if (op->kind != OP_TILES)
        return sizeof(Op);   /* a held layer is not counted: it is the user's data */
    return op->tiles->len * (sizeof(Tile) + TILE * TILE * 4);
}

static void op_free(Op *op)
{
    if (op->tiles) {
        for (guint i = 0; i < op->tiles->len; i++)
            g_free(g_array_index(op->tiles, Tile, i).pix);
        g_array_free(op->tiles, TRUE);
    }
    if (op->holds)
        kp_layer_free(op->layer);
    g_free(op->props.name);
    g_free(op);
}

static void stack_clear(GQueue *q)
{
    Op *op;
    while ((op = g_queue_pop_head(q))) {
        g_bytes -= op_bytes(op);
        op_free(op);
    }
}

/* Copies tile (tx,ty) between the layer and `buf`; with swap, both ways. */
static void tile_copy(KpLayer *l, int tx, int ty, guint32 *buf, gboolean swap)
{
    unsigned char *data = cairo_image_surface_get_data(l->surf);
    int stride = cairo_image_surface_get_stride(l->surf);
    int x0 = tx * TILE, y0 = ty * TILE;
    int w = MIN(TILE, l->w - x0), h = MIN(TILE, l->h - y0);
    guint32 tmp[TILE];
    for (int y = 0; y < h; y++) {
        guint32 *row = (guint32 *)(data + (y0 + y) * stride) + x0;
        guint32 *saved = buf + y * TILE;
        if (swap) {
            memcpy(tmp, row, w * 4);
            memcpy(row, saved, w * 4);
            memcpy(saved, tmp, w * 4);
        } else {
            memcpy(saved, row, w * 4);
        }
    }
}

void kp_undo_begin(KpLayer *l)
{
    if (g_cur)
        kp_undo_end();
    g_cur = g_new0(Op, 1);
    g_cur->kind = OP_TILES;
    g_cur->layer = l;
    g_cur->tiles = g_array_new(FALSE, FALSE, sizeof(Tile));
    g_tiles_w = (l->w + TILE - 1) / TILE;
    g_tiles_h = (l->h + TILE - 1) / TILE;
    g_seen = g_malloc0((size_t)g_tiles_w * g_tiles_h);
}

void kp_undo_touch(int x, int y, int w, int h)
{
    if (!g_cur)
        return;
    KpLayer *l = g_cur->layer;
    int x1 = MIN(x + w, l->w), y1 = MIN(y + h, l->h);
    x = MAX(x, 0);
    y = MAX(y, 0);
    if (x >= x1 || y >= y1)
        return;
    cairo_surface_flush(l->surf);
    for (int ty = y / TILE; ty <= (y1 - 1) / TILE; ty++)
        for (int tx = x / TILE; tx <= (x1 - 1) / TILE; tx++) {
            guint8 *seen = &g_seen[ty * g_tiles_w + tx];
            if (*seen)
                continue;
            *seen = 1;
            Tile t = { tx, ty, g_malloc(TILE * TILE * 4) };
            tile_copy(l, tx, ty, t.pix, FALSE);
            g_array_append_val(g_cur->tiles, t);
        }
}

static void push_op(Op *op);

void kp_undo_end(void)
{
    if (!g_cur)
        return;
    g_free(g_seen);
    g_seen = NULL;
    Op *op = g_cur;
    g_cur = NULL;
    if (op->tiles->len == 0) {
        op_free(op);
        return;
    }
    push_op(op);
}

static void push_op(Op *op)
{
    op->id = g_next_id++;
    stack_clear(&g_redo);
    g_queue_push_head(&g_undo, op);
    g_bytes += op_bytes(op);
    /* Keep at least the newest operation, however big. */
    while (g_bytes > UNDO_LIMIT_BYTES && g_queue_get_length(&g_undo) > 1) {
        Op *old = g_queue_pop_tail(&g_undo);
        g_floor_id = old->id;
        g_bytes -= op_bytes(old);
        op_free(old);
    }
    kp_doc_edited();
}

/* Undoes (backwards) or redoes a structure operation. */
static void apply_structure(Op *op, gboolean backwards)
{
    switch (op->kind) {
    case OP_ADD:
    case OP_REMOVE: {
        gboolean insert = (op->kind == OP_ADD) != backwards;
        if (insert) {
            kp_doc_layer_insert(kp.doc, op->layer, op->index);
            op->holds = FALSE;
        } else {
            kp_doc_layer_take(kp.doc, op->index);
            op->holds = TRUE;
        }
        break;
    }
    case OP_MOVE:
        if (backwards)
            kp_doc_layer_move(kp.doc, op->to, op->index);
        else
            kp_doc_layer_move(kp.doc, op->index, op->to);
        break;
    case OP_PROPS:
        kp_layer_props_swap(op->layer, &op->props);
        break;
    case OP_TILES:
        break;
    }
}

static gboolean move_op(GQueue *from, GQueue *to)
{
    Op *op = g_queue_pop_head(from);
    if (!op)
        return FALSE;
    if (op->kind != OP_TILES) {
        apply_structure(op, from == &g_undo);
        g_queue_push_head(to, op);
        kp_layers_refresh();
        kp_canvas_redraw();
        kp_doc_edited();
        return TRUE;
    }
    KpLayer *l = op->layer;
    cairo_surface_flush(l->surf);
    int x0 = l->w, y0 = l->h, x1 = 0, y1 = 0;
    for (guint i = 0; i < op->tiles->len; i++) {
        Tile *t = &g_array_index(op->tiles, Tile, i);
        tile_copy(l, t->tx, t->ty, t->pix, TRUE);
        x0 = MIN(x0, t->tx * TILE);
        y0 = MIN(y0, t->ty * TILE);
        x1 = MAX(x1, MIN((t->tx + 1) * TILE, l->w));
        y1 = MAX(y1, MIN((t->ty + 1) * TILE, l->h));
    }
    cairo_surface_mark_dirty(l->surf);
    g_queue_push_head(to, op);
    kp_canvas_damage(x0, y0, x1 - x0, y1 - y0);
    kp_doc_edited();
    return TRUE;
}

gboolean kp_undo_undo(void)
{
    kp_layers_flush();
    kp_undo_end();
    return move_op(&g_undo, &g_redo);
}

gboolean kp_undo_redo(void)
{
    kp_layers_flush();
    kp_undo_end();
    return move_op(&g_redo, &g_undo);
}

static Op *structure_op(OpKind kind, KpLayer *l)
{
    kp_undo_end();
    Op *op = g_new0(Op, 1);
    op->kind = kind;
    op->layer = l;
    return op;
}

void kp_undo_layer_added(int index)
{
    Op *op = structure_op(OP_ADD, g_ptr_array_index(kp.doc->layers, index));
    op->index = index;
    push_op(op);
}

void kp_undo_layer_removed(KpLayer *l, int index)
{
    Op *op = structure_op(OP_REMOVE, l);
    op->index = index;
    op->holds = TRUE;
    push_op(op);
}

void kp_undo_layer_moved(int from, int to)
{
    Op *op = structure_op(OP_MOVE, NULL);
    op->index = from;
    op->to = to;
    push_op(op);
}

void kp_undo_layer_props(KpLayer *l, KpLayerProps *old)
{
    Op *op = structure_op(OP_PROPS, l);
    op->props = *old;
    old->name = NULL;
    push_op(op);
}

void kp_undo_clear(void)
{
    if (g_cur) {
        op_free(g_cur);
        g_cur = NULL;
        g_free(g_seen);
        g_seen = NULL;
    }
    stack_clear(&g_undo);
    stack_clear(&g_redo);
    g_floor_id = 0;
}

gboolean kp_undo_can_undo(void)
{
    return !g_queue_is_empty(&g_undo);
}

gboolean kp_undo_can_redo(void)
{
    return !g_queue_is_empty(&g_redo);
}

guint kp_undo_state(void)
{
    Op *op = g_queue_peek_head(&g_undo);
    return op ? op->id : g_floor_id;
}

/* km_store - kimemoryd's clipboard history: items, where each came from
 * and was pasted into, and their plain-text persistence under
 * $XDG_DATA_HOME/kimemory/.
 *
 * On disk:
 *   index.tsv       one record per line, tab-separated, \t \n \\ escaped:
 *                     I  id  ts  type  bytes  fav  hash
 *                     R  id  n   target  type  format      (n -> items/<id>.<n>)
 *                     S  id  ts  win  app  title  doc     (copied from)
 *                     D  id  ts  win  app  title  doc     (pasted into)
 *   items/<id>.<n>  raw bytes of one representation (text, html, png...)
 *
 * index.tsv is rewritten whole (temp file + rename) on every change: it is
 * small, and that keeps removals trivially consistent. */
#ifndef KM_STORE_H
#define KM_STORE_H

#include <stddef.h>

#define KM_MAX_REFS 32

enum { KM_TEXT, KM_LINK, KM_FILES, KM_IMAGE };
enum { KM_PERSIST_ALL, KM_PERSIST_FAVORITES, KM_PERSIST_NONE };

typedef struct {
    long ts;
    unsigned long win;
    char *app;    /* WM_CLASS res_class, else exe basename */
    char *title;
    char *doc;
} KmRef;

/* One selection target's data, exactly as the owner handed it over. */
typedef struct {
    char *target;          /* target atom name ("UTF8_STRING", "image/png"...) */
    char *type;            /* property type atom name it came back as */
    int format;            /* 8, 16 or 32 */
    unsigned char *data;   /* NULL while it only lives on disk (see km_rep_data()) */
    size_t len;            /* bytes (format 32: nitems * sizeof(long), Xlib's in-memory layout) */
    int persist;           /* one of the canonical reps kept on disk; others only live while current */
    int disk_n;            /* items/<id>.<disk_n>, -1 if not on disk */
} KmRep;

typedef struct {
    unsigned id;
    long ts;                     /* last time it was copied */
    int type;                    /* KM_TEXT... */
    size_t bytes;                /* size of the primary rep */
    int fav;
    unsigned long long hash;     /* of the primary rep, for dedupe */
    int secret;                  /* never stored: only kept to know not to take it over */
    KmRep *reps;
    int nreps;
    KmRef srcs[KM_MAX_REFS];
    int nsrc;
    KmRef dsts[KM_MAX_REFS];
    int ndst;
} KmItem;

int  km_store_init(const char *dir, int max_entries, int persist_mode);
void km_store_set_limits(int max_entries, int persist_mode);
void km_store_load(void);

int     km_store_count(void);
KmItem *km_store_at(int i);            /* 0 = newest */
KmItem *km_store_get(unsigned id);
KmItem *km_store_find_hash(unsigned long long hash);

/* Takes ownership of `it` (allocated with km_item_new()), gives it an id,
 * puts it on top, trims the oldest non-favourites past the limit, saves. */
KmItem *km_store_add(KmItem *it);
void km_store_touch(KmItem *it);       /* copied again: to the top, ts = now */
void km_store_add_ref(KmItem *it, int dst, long ts, unsigned long win,
                      const char *app, const char *title, const char *doc);
int  km_store_set_fav(unsigned id, int fav);
int  km_store_remove(unsigned id);
int  km_store_clear(int keep_favs);    /* returns how many were removed */
void km_store_save(void);
size_t km_store_disk_bytes(void);

KmItem *km_item_new(void);
void    km_item_free(KmItem *it);
KmRep  *km_item_add_rep(KmItem *it, const char *target, const char *type, int format,
                        const unsigned char *data, size_t len, int persist);
KmRep  *km_item_find_rep(KmItem *it, const char *target);
/* Classifies (type, primary rep, bytes, hash) from the reps present. */
void    km_item_finish(KmItem *it);
/* Primary rep's text (text items) or NULL; loads from disk if needed. */
const char *km_item_text(KmItem *it);

/* Rep bytes, loaded from disk on first use. NULL if unreadable. */
const unsigned char *km_rep_data(KmItem *it, KmRep *r);
/* items/<id>.<n> path of a rep that is on disk; 0 if it only lives in RAM. */
int km_rep_path(KmItem *it, KmRep *r, char *out, size_t outsz);
/* Drops the in-memory copy of big on-disk reps again (after serving). */
void km_item_unload(KmItem *it);
/* Frees the reps that are never persisted (the extra targets mirrored
 * only to serve the item while it is the current clipboard). */
void km_item_drop_extras(KmItem *it);
/* Copied again: `from`'s reps replace `into`'s (fresh extras included,
 * old files deleted); `from` is freed. Keeps `into`'s id, fav and refs. */
void km_item_adopt_reps(KmItem *into, KmItem *from);

#endif /* KM_STORE_H */

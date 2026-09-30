/* km_store - see km_store.h. */
#include "km_store.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Reps bigger than this are dropped from memory after use and re-read
 * from disk when needed again (images, mostly). */
#define KM_KEEP_IN_RAM 65536

static char g_dir[512];
static char g_items_dir[600];
static int g_max_entries = 200;
static int g_persist = KM_PERSIST_ALL;
static KmItem **g_items;   /* newest first */
static int g_count, g_cap;
static unsigned g_next_id = 1;

/* ---- helpers ---- */

static char *xstrdup(const char *s)
{
    return strdup(s ? s : "");
}

static unsigned long long fnv1a(const unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void tsv_put(FILE *f, const char *s)
{
    for (; s && *s; s++) {
        if (*s == '\t')      fputs("\\t", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\\') fputs("\\\\", f);
        else                 fputc(*s, f);
    }
}

/* Splits one line in place into at most `max` unescaped fields. */
static int tsv_split(char *line, char **fields, int max)
{
    int n = 0;
    char *out = line, *start = line;
    for (char *p = line; ; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            *out++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p;
            continue;
        }
        if (*p == '\t' || *p == '\0' || *p == '\n') {
            char c = *p;
            *out++ = '\0';
            if (n < max)
                fields[n++] = start;
            if (c != '\t')
                break;
            start = out;
            continue;
        }
        *out++ = *p;
    }
    return n;
}

static int item_persisted(const KmItem *it)
{
    if (it->secret)
        return 0;
    return g_persist == KM_PERSIST_ALL || (g_persist == KM_PERSIST_FAVORITES && it->fav);
}

static void rep_path(char *buf, size_t sz, unsigned id, int n)
{
    snprintf(buf, sz, "%s/%u.%d", g_items_dir, id, n);
}

static void mkdir_p(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
}

/* ---- items ---- */

KmItem *km_item_new(void)
{
    return calloc(1, sizeof(KmItem));
}

static void ref_free(KmRef *r)
{
    free(r->app);
    free(r->title);
    free(r->doc);
}

void km_item_free(KmItem *it)
{
    if (!it)
        return;
    for (int i = 0; i < it->nreps; i++) {
        free(it->reps[i].target);
        free(it->reps[i].type);
        free(it->reps[i].data);
    }
    free(it->reps);
    for (int i = 0; i < it->nsrc; i++) ref_free(&it->srcs[i]);
    for (int i = 0; i < it->ndst; i++) ref_free(&it->dsts[i]);
    free(it);
}

KmRep *km_item_add_rep(KmItem *it, const char *target, const char *type, int format,
                       const unsigned char *data, size_t len, int persist)
{
    KmRep *nr = realloc(it->reps, sizeof(KmRep) * (size_t)(it->nreps + 1));
    if (!nr)
        return NULL;
    it->reps = nr;
    KmRep *r = &it->reps[it->nreps++];
    memset(r, 0, sizeof(*r));
    r->target = xstrdup(target);
    r->type = xstrdup(type);
    r->format = format;
    r->len = len;
    r->persist = persist;
    r->disk_n = -1;
    if (data) {
        r->data = malloc(len + 1);   /* +1: text reps are always NUL-terminated */
        if (r->data) {
            memcpy(r->data, data, len);
            r->data[len] = '\0';
        }
    }
    return r;
}

KmRep *km_item_find_rep(KmItem *it, const char *target)
{
    for (int i = 0; i < it->nreps; i++)
        if (strcmp(it->reps[i].target, target) == 0)
            return &it->reps[i];
    return NULL;
}

const unsigned char *km_rep_data(KmItem *it, KmRep *r)
{
    if (r->data || r->disk_n < 0)
        return r->data;
    char path[700];
    rep_path(path, sizeof(path), it->id, r->disk_n);
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    r->data = malloc(r->len + 1);
    if (r->data && fread(r->data, 1, r->len, f) != r->len) {
        free(r->data);
        r->data = NULL;
    }
    if (r->data)
        r->data[r->len] = '\0';
    fclose(f);
    return r->data;
}

void km_item_unload(KmItem *it)
{
    for (int i = 0; i < it->nreps; i++) {
        KmRep *r = &it->reps[i];
        if (r->disk_n >= 0 && r->len > KM_KEEP_IN_RAM) {
            free(r->data);
            r->data = NULL;
        }
    }
}

void km_item_drop_extras(KmItem *it)
{
    int w = 0;
    for (int i = 0; i < it->nreps; i++) {
        KmRep *r = &it->reps[i];
        if (!r->persist) {
            free(r->target);
            free(r->type);
            free(r->data);
            continue;
        }
        it->reps[w++] = *r;
    }
    it->nreps = w;
}

static void item_delete_files(KmItem *it);

void km_item_adopt_reps(KmItem *into, KmItem *from)
{
    item_delete_files(into);
    for (int i = 0; i < into->nreps; i++) {
        free(into->reps[i].target);
        free(into->reps[i].type);
        free(into->reps[i].data);
    }
    free(into->reps);
    into->reps = from->reps;
    into->nreps = from->nreps;
    into->type = from->type;
    into->bytes = from->bytes;
    into->hash = from->hash;
    from->reps = NULL;
    from->nreps = 0;
    km_item_free(from);
}

static KmRep *text_rep(KmItem *it)
{
    static const char *names[] = { "UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING" };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        KmRep *r = km_item_find_rep(it, names[i]);
        if (r)
            return r;
    }
    return NULL;
}

const char *km_item_text(KmItem *it)
{
    KmRep *r = text_rep(it);
    return r ? (const char *)km_rep_data(it, r) : NULL;
}

static int looks_like_link(const char *s, size_t len)
{
    while (len && (*s == ' ' || *s == '\n')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\n')) len--;
    if (!len || len > 4096)
        return 0;
    const char *colon = memchr(s, ':', len);
    if (!colon || colon == s || (size_t)(colon - s) > 16 || (size_t)(colon - s) + 3 > len ||
        colon[1] != '/' || colon[2] != '/')
        return 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] == ' ' || s[i] == '\n' || s[i] == '\t')
            return 0;
    return 1;
}

void km_item_finish(KmItem *it)
{
    KmRep *primary = NULL;
    KmRep *uris = km_item_find_rep(it, "text/uri-list");
    KmRep *gcf = km_item_find_rep(it, "x-special/gnome-copied-files");
    KmRep *png = km_item_find_rep(it, "image/png");
    KmRep *text = text_rep(it);

    if (uris || gcf) {
        it->type = KM_FILES;
        primary = uris ? uris : gcf;
    } else if (text) {
        primary = text;
        const unsigned char *d = km_rep_data(it, text);
        it->type = d && looks_like_link((const char *)d, text->len) ? KM_LINK : KM_TEXT;
    } else if (png) {
        it->type = KM_IMAGE;
        primary = png;
    }
    if (!primary && it->nreps)
        primary = &it->reps[0];
    if (!primary)
        return;
    const unsigned char *d = km_rep_data(it, primary);
    it->bytes = primary->len;
    it->hash = d ? fnv1a(d, primary->len) : 0;
}

/* ---- store ---- */

static void store_insert_top(KmItem *it)
{
    if (g_count == g_cap) {
        int ncap = g_cap ? g_cap * 2 : 64;
        KmItem **n = realloc(g_items, sizeof(KmItem *) * (size_t)ncap);
        if (!n)
            return;
        g_items = n;
        g_cap = ncap;
    }
    memmove(g_items + 1, g_items, sizeof(KmItem *) * (size_t)g_count);
    g_items[0] = it;
    g_count++;
}

static int store_index_of(const KmItem *it)
{
    for (int i = 0; i < g_count; i++)
        if (g_items[i] == it)
            return i;
    return -1;
}

static void item_delete_files(KmItem *it)
{
    for (int i = 0; i < it->nreps; i++) {
        KmRep *r = &it->reps[i];
        if (r->disk_n < 0)
            continue;
        char path[700];
        rep_path(path, sizeof(path), it->id, r->disk_n);
        unlink(path);
        r->disk_n = -1;
    }
}

static void store_remove_at(int i)
{
    KmItem *it = g_items[i];
    item_delete_files(it);
    km_item_free(it);
    memmove(g_items + i, g_items + i + 1, sizeof(KmItem *) * (size_t)(g_count - i - 1));
    g_count--;
}

static void store_trim(void)
{
    int nonfav = 0;
    for (int i = 0; i < g_count; i++)
        if (!g_items[i]->fav)
            nonfav++;
    for (int i = g_count - 1; i >= 0 && nonfav > g_max_entries; i--) {
        if (g_items[i]->fav)
            continue;
        store_remove_at(i);
        nonfav--;
    }
}

int km_store_init(const char *dir, int max_entries, int persist_mode)
{
    snprintf(g_dir, sizeof(g_dir), "%s", dir);
    snprintf(g_items_dir, sizeof(g_items_dir), "%s/items", dir);
    mkdir_p(g_items_dir);
    chmod(g_dir, 0700);
    km_store_set_limits(max_entries, persist_mode);
    return access(g_items_dir, W_OK) == 0;
}

void km_store_set_limits(int max_entries, int persist_mode)
{
    g_max_entries = max_entries > 0 ? max_entries : 1;
    g_persist = persist_mode;
}

int km_store_count(void) { return g_count; }
KmItem *km_store_at(int i) { return i >= 0 && i < g_count ? g_items[i] : NULL; }

KmItem *km_store_get(unsigned id)
{
    for (int i = 0; i < g_count; i++)
        if (g_items[i]->id == id)
            return g_items[i];
    return NULL;
}

KmItem *km_store_find_hash(unsigned long long hash)
{
    for (int i = 0; i < g_count; i++)
        if (g_items[i]->hash == hash)
            return g_items[i];
    return NULL;
}

KmItem *km_store_add(KmItem *it)
{
    it->id = g_next_id++;
    if (!it->ts)
        it->ts = (long)time(NULL);
    store_insert_top(it);
    store_trim();
    km_store_save();
    return km_store_get(it->id);
}

void km_store_touch(KmItem *it)
{
    int i = store_index_of(it);
    if (i < 0)
        return;
    memmove(g_items + 1, g_items, sizeof(KmItem *) * (size_t)i);
    g_items[0] = it;
    it->ts = (long)time(NULL);
    km_store_save();
}

void km_store_add_ref(KmItem *it, int dst, long ts, unsigned long win,
                      const char *app, const char *title, const char *doc)
{
    KmRef *refs = dst ? it->dsts : it->srcs;
    int *n = dst ? &it->ndst : &it->nsrc;

    /* Same place again: just refresh its time. */
    for (int i = 0; i < *n; i++) {
        if (refs[i].win == win && strcmp(refs[i].app, app ? app : "") == 0 &&
            strcmp(refs[i].doc, doc ? doc : "") == 0) {
            refs[i].ts = ts;
            free(refs[i].title);
            refs[i].title = xstrdup(title);
            km_store_save();
            return;
        }
    }
    if (*n == KM_MAX_REFS) {   /* full: forget the oldest */
        ref_free(&refs[0]);
        memmove(refs, refs + 1, sizeof(KmRef) * (KM_MAX_REFS - 1));
        (*n)--;
    }
    KmRef *r = &refs[(*n)++];
    r->ts = ts;
    r->win = win;
    r->app = xstrdup(app);
    r->title = xstrdup(title);
    r->doc = xstrdup(doc);
    km_store_save();
}

int km_store_set_fav(unsigned id, int fav)
{
    KmItem *it = km_store_get(id);
    if (!it)
        return 0;
    it->fav = fav ? 1 : 0;
    store_trim();
    km_store_save();
    return 1;
}

int km_store_remove(unsigned id)
{
    KmItem *it = km_store_get(id);
    if (!it)
        return 0;
    store_remove_at(store_index_of(it));
    km_store_save();
    return 1;
}

int km_store_clear(int keep_favs)
{
    int removed = 0;
    for (int i = g_count - 1; i >= 0; i--) {
        if (keep_favs && g_items[i]->fav)
            continue;
        store_remove_at(i);
        removed++;
    }
    km_store_save();
    return removed;
}

static void write_ref(FILE *f, char kind, unsigned id, const KmRef *r)
{
    fprintf(f, "%c\t%u\t%ld\t%lu\t", kind, id, r->ts, r->win);
    tsv_put(f, r->app);   fputc('\t', f);
    tsv_put(f, r->title); fputc('\t', f);
    tsv_put(f, r->doc);   fputc('\n', f);
}

void km_store_save(void)
{
    if (!g_dir[0])
        return;
    char path[700], tmp[720];
    snprintf(path, sizeof(path), "%s/index.tsv", g_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *f = fopen(tmp, "w");
    if (!f)
        return;
    fchmod(fileno(f), 0600);

    /* Oldest first, so a partial read still makes sense chronologically. */
    for (int i = g_count - 1; i >= 0; i--) {
        KmItem *it = g_items[i];
        if (!item_persisted(it)) {
            /* No longer kept on disk (unfavourited under persist=favorites):
             * pull its data back into memory before dropping the files. */
            for (int k = 0; k < it->nreps; k++)
                km_rep_data(it, &it->reps[k]);
            item_delete_files(it);
            continue;
        }
        fprintf(f, "I\t%u\t%ld\t%d\t%zu\t%d\t%016llx\n", it->id, it->ts, it->type, it->bytes, it->fav, it->hash);
        for (int k = 0; k < it->nreps; k++) {
            KmRep *r = &it->reps[k];
            if (!r->persist)
                continue;
            if (r->disk_n < 0 && r->data) {
                char rp[700];
                rep_path(rp, sizeof(rp), it->id, k);
                FILE *rf = fopen(rp, "wb");
                if (!rf)
                    continue;
                fchmod(fileno(rf), 0600);
                int ok = fwrite(r->data, 1, r->len, rf) == r->len;
                if (fclose(rf) != 0 || !ok) {
                    unlink(rp);
                    continue;
                }
                r->disk_n = k;
            }
            if (r->disk_n < 0)
                continue;
            fprintf(f, "R\t%u\t%d\t", it->id, r->disk_n);
            tsv_put(f, r->target); fputc('\t', f);
            tsv_put(f, r->type);   fprintf(f, "\t%d\t%zu\n", r->format, r->len);
        }
        for (int k = 0; k < it->nsrc; k++) write_ref(f, 'S', it->id, &it->srcs[k]);
        for (int k = 0; k < it->ndst; k++) write_ref(f, 'D', it->id, &it->dsts[k]);
        km_item_unload(it);
    }

    if (fclose(f) != 0 || rename(tmp, path) != 0)
        unlink(tmp);
}

void km_store_load(void)
{
    char path[700];
    snprintf(path, sizeof(path), "%s/index.tsv", g_dir);
    FILE *f = fopen(path, "r");
    if (!f)
        return;

    /* Read oldest first; inserting each at the top leaves newest first. */
    char *line = NULL;
    size_t cap = 0;
    KmItem *cur = NULL;
    while (getline(&line, &cap, f) > 0) {
        char *fl[8];
        int n = tsv_split(line, fl, 8);
        if (n < 3)
            continue;
        unsigned id = (unsigned)strtoul(fl[1], NULL, 10);
        if (fl[0][0] == 'I' && n >= 7) {
            if (cur)
                store_insert_top(cur);
            cur = km_item_new();
            cur->id = id;
            cur->ts = atol(fl[2]);
            cur->type = atoi(fl[3]);
            cur->bytes = (size_t)strtoull(fl[4], NULL, 10);
            cur->fav = atoi(fl[5]);
            cur->hash = strtoull(fl[6], NULL, 16);
            if (id >= g_next_id)
                g_next_id = id + 1;
        } else if (!cur || cur->id != id) {
            continue;
        } else if (fl[0][0] == 'R' && n >= 7) {
            KmRep *r = km_item_add_rep(cur, fl[3], fl[4], atoi(fl[5]), NULL, (size_t)strtoull(fl[6], NULL, 10), 1);
            if (r)
                r->disk_n = atoi(fl[2]);
        } else if ((fl[0][0] == 'S' || fl[0][0] == 'D') && n >= 7) {
            int dst = fl[0][0] == 'D';
            KmRef *refs = dst ? cur->dsts : cur->srcs;
            int *cnt = dst ? &cur->ndst : &cur->nsrc;
            if (*cnt < KM_MAX_REFS) {
                KmRef *r = &refs[(*cnt)++];
                r->ts = atol(fl[2]);
                r->win = strtoul(fl[3], NULL, 10);
                r->app = xstrdup(fl[4]);
                r->title = xstrdup(fl[5]);
                r->doc = xstrdup(fl[6]);
            }
        }
    }
    if (cur)
        store_insert_top(cur);
    free(line);
    fclose(f);
    store_trim();
}

size_t km_store_disk_bytes(void)
{
    size_t total = 0;
    struct stat st;
    char path[1024];
    snprintf(path, sizeof(path), "%s/index.tsv", g_dir);
    if (stat(path, &st) == 0)
        total += (size_t)st.st_size;
    DIR *d = opendir(g_items_dir);
    if (!d)
        return total;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", g_items_dir, e->d_name);
        if (stat(path, &st) == 0)
            total += (size_t)st.st_size;
    }
    closedir(d);
    return total;
}

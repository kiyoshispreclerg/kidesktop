/* km_clip - see km_clip.h. */
#include "km_clip.h"
#include "km_store.h"
#include "../shared/xis_winident.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define FETCH_TIMEOUT_MS 2000
#define INCR_TIMEOUT_MS  10000
#define PASTE_DEBOUNCE_MS 1500
#define MAX_TRANSFERS    16

static Display *dpy;
static Window win;            /* ours: requestor for fetches */
static int xfixes_ev;
static KmClipConfig cfg;
static int debug;             /* KIMEMORYD_DEBUG=1: log every fetch */

static void dbg_target(const char *what, Atom target)
{
    if (!debug)
        return;
    char *n = XGetAtomName(dpy, target);
    fprintf(stderr, "kimemoryd: debug: %s %s\n", what, n ? n : "?");
    if (n)
        XFree(n);
}

static Atom A_CLIPBOARD, A_TARGETS, A_TIMESTAMP, A_MULTIPLE, A_INCR, A_SAVE_TARGETS,
            A_DELETE, A_INSERT_SELECTION, A_INSERT_PROPERTY, A_UTF8, A_KM_TARGETS, A_KM_DATA,
            A_PASSWORD_HINT, A_KM_TIME, A_ATOM_PAIR, A_TEXT, A_COMPOUND_TEXT, A_TEXT_PLAIN, A_TEXT_PLAIN_UTF8;

/* Representations kept on disk (and fetched even for rich copies); every
 * other target is mirrored only for simple copies and only kept while
 * the item is the current clipboard. */
static const char *CANONICAL[] = {
    "UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING",
    "text/html", "text/uri-list", "x-special/gnome-copied-files",
    "application/x-kde-cutselection", "image/png",
};
#define N_CANONICAL (sizeof(CANONICAL) / sizeof(CANONICAL[0]))
#define N_TEXT_TARGETS 4   /* the first four above: only one of them is kept */

/* Targets whose presence marks a copy as app-private (rich). */
static const char *RICH_PREFIX[] = {
    "application/x-openoffice", "application/vnd.oasis", "application/x-qt",
    "application/x-gimp", "application/x-krita", "image/x-inkscape",
    "application/x-kde-", "application/x-blender", "image/",
};

static unsigned current_id;
static int owned;             /* we hold CLIPBOARD, serving current_id */
static Time own_time;

/* Outgoing INCR transfers (reps too big for one property). */
static struct {
    Window requestor;
    Atom property, type;
    int format;
    unsigned char *data;
    size_t len, off;
    int done_sent;        /* zero-length terminator already written */
    long deadline;
} xfer[MAX_TRANSFERS];
static int nxfer;

static struct {
    Window requestor;
    long at;
} last_paste;

static struct {
    int active;
    Window owner;
    Time time;
    Atom *want;
    int nwant, next;
    Atom pending;         /* target of the conversion in flight */
    int rich;
    int skipped;          /* a rep was dropped (size limit / timeout): can't mirror exactly */
    size_t mirrored;
    KmItem *item;
    XisWinIdent src;
    long deadline;        /* ms, monotonic */
    int incr;
    Atom incr_target;
    unsigned char *incr_buf;
    size_t incr_len;
    Atom incr_type;
    int incr_format;
} cap;

static long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int in_comma_list(const char *list, const char *name)
{
    if (!name || !*name)
        return 0;
    size_t n = strlen(name);
    for (const char *p = list; *p; ) {
        while (*p == ',' || *p == ' ')
            p++;
        const char *e = p;
        while (*e && *e != ',')
            e++;
        const char *te = e;
        while (te > p && te[-1] == ' ')
            te--;
        if ((size_t)(te - p) == n && strncasecmp(p, name, n) == 0)
            return 1;
        p = e;
    }
    return 0;
}

static const char *exe_base(const char *exe)
{
    const char *b = strrchr(exe, '/');
    return b ? b + 1 : exe;
}

static int canonical_index(const char *name)
{
    for (size_t i = 0; i < N_CANONICAL; i++)
        if (strcmp(CANONICAL[i], name) == 0)
            return (int)i;
    return -1;
}

static int is_meta_target(Atom a)
{
    /* INCR: xsel lists it as if it were a target. */
    return a == A_TARGETS || a == A_TIMESTAMP || a == A_MULTIPLE || a == A_SAVE_TARGETS ||
           a == A_DELETE || a == A_INSERT_SELECTION || a == A_INSERT_PROPERTY || a == A_INCR || a == None;
}

static int is_text_alias(Atom a);

/* Legacy text encodings: served from the UTF-8 rep, never mirrored. */
static int is_legacy_text(Atom a)
{
    return is_text_alias(a) || a == A_COMPOUND_TEXT;
}

static size_t elem_size(int format)
{
    return format == 8 ? 1 : format == 16 ? sizeof(short) : sizeof(long);
}

static void cap_reset(void)
{
    km_item_free(cap.item);
    free(cap.want);
    free(cap.incr_buf);
    memset(&cap, 0, sizeof(cap));
}

static void fetch_next(void);

static void take_ownership(void);

static void cap_finish(void)
{
    KmItem *it = cap.item;
    cap.item = NULL;
    km_item_finish(it);
    if (it->nreps == 0 || it->bytes == 0) {
        km_item_free(it);
        cap_reset();
        return;
    }

    const char *app = cap.src.wm_class[0] ? cap.src.wm_class : exe_base(cap.src.exe);
    KmItem *old = km_store_find_hash(it->hash);
    if (old) {
        km_item_adopt_reps(old, it);
        km_store_touch(old);
        it = old;
    } else {
        it = km_store_add(it);
    }
    if (it) {
        km_store_add_ref(it, 0, (long)time(NULL), cap.src.win, app, cap.src.title, cap.src.doc_hint);
        KmItem *prev = current_id && current_id != it->id ? km_store_get(current_id) : NULL;
        if (prev)
            km_item_drop_extras(prev);
        current_id = it->id;
        fprintf(stderr, "kimemoryd: #%u from %s (%s, %zu bytes, %d reps)\n", it->id, app,
                cap.rich ? "rich" : "simple", it->bytes, it->nreps);
        /* Mirrored exactly and the source still owns it: serve it from
         * here on, which is what lets pastes be traced to their window. */
        if (!cap.rich && !cap.skipped && cfg.takeover == KM_TAKEOVER_SIMPLE &&
            XGetSelectionOwner(dpy, A_CLIPBOARD) == cap.owner)
            take_ownership();
    }
    cap_reset();
}

static void fetch_next(void)
{
    if (cap.next >= cap.nwant) {
        cap_finish();
        return;
    }
    Atom t = cap.want[cap.next++];
    cap.pending = t;
    dbg_target("fetch", t);
    XDeleteProperty(dpy, win, A_KM_DATA);
    XConvertSelection(dpy, A_CLIPBOARD, t, A_KM_DATA, win, cap.time);
    cap.deadline = now_ms() + FETCH_TIMEOUT_MS;
}

static void cap_start(Window owner, Time t)
{
    cap_reset();
    KmItem *prev = current_id ? km_store_get(current_id) : NULL;
    if (prev)
        km_item_drop_extras(prev);
    current_id = 0;

    Window top = xis_winident_toplevel_for(dpy, owner);
    if (top == None)
        top = xis_winident_active(dpy);
    XisWinIdent src;
    if (!xis_winident_get(dpy, top, &src))
        memset(&src, 0, sizeof(src));
    if (in_comma_list(cfg.exclude, src.wm_class) || in_comma_list(cfg.exclude, src.wm_instance) ||
        in_comma_list(cfg.exclude, exe_base(src.exe)))
        return;   /* excluded: nothing read at all */

    cap.active = 1;
    cap.owner = owner;
    cap.time = t;
    cap.src = src;
    cap.item = km_item_new();
    cap.pending = A_TARGETS;
    XDeleteProperty(dpy, win, A_KM_TARGETS);
    XConvertSelection(dpy, A_CLIPBOARD, A_TARGETS, A_KM_TARGETS, win, t);
    cap.deadline = now_ms() + FETCH_TIMEOUT_MS;
}

/* TARGETS arrived (or failed: `atoms` NULL) -- decide what to fetch. */
static void cap_plan(Atom *atoms, unsigned long n)
{
    cap.want = calloc(n + N_CANONICAL + 1, sizeof(Atom));
    if (!cap.want) {
        cap_reset();
        return;
    }
    if (!atoms) {
        /* No TARGETS support (very old clients): just ask for text. */
        cap.rich = 1;
        cap.want[cap.nwant++] = A_UTF8;
        cap.want[cap.nwant++] = XA_STRING;
        fetch_next();
        return;
    }

    char **names = calloc(n ? n : 1, sizeof(char *));
    if (!names || (n && !XGetAtomNames(dpy, atoms, (int)n, names))) {
        free(names);
        cap_reset();
        return;
    }

    cap.rich = cfg.takeover == KM_TAKEOVER_NEVER || cfg.takeover == KM_TAKEOVER_ONEXIT ||
               in_comma_list(cfg.never_takeover, cap.src.wm_class) ||
               in_comma_list(cfg.never_takeover, cap.src.wm_instance) ||
               in_comma_list(cfg.never_takeover, exe_base(cap.src.exe)) || n > 48;
    int secret = 0;
    for (unsigned long i = 0; i < n; i++) {
        if (atoms[i] == A_PASSWORD_HINT)
            secret = 1;
        if (!names[i] || is_meta_target(atoms[i]) || canonical_index(names[i]) >= 0)
            continue;
        for (size_t k = 0; k < sizeof(RICH_PREFIX) / sizeof(RICH_PREFIX[0]); k++)
            if (strncmp(names[i], RICH_PREFIX[k], strlen(RICH_PREFIX[k])) == 0)
                cap.rich = 1;
    }
    if (secret) {
        for (unsigned long i = 0; i < n; i++)
            if (names[i]) XFree(names[i]);
        free(names);
        cap_reset();
        return;
    }

    /* Canonical first (in CANONICAL order, one text encoding only)... */
    int have_text = 0;
    for (size_t c = 0; c < N_CANONICAL; c++) {
        for (unsigned long i = 0; i < n; i++) {
            if (!names[i] || strcmp(names[i], CANONICAL[c]) != 0)
                continue;
            if (c < N_TEXT_TARGETS) {
                if (have_text)
                    break;
                have_text = 1;
            }
            cap.want[cap.nwant++] = atoms[i];
            break;
        }
    }
    /* ...then, for simple copies, every other target too. */
    if (!cap.rich) {
        for (unsigned long i = 0; i < n; i++) {
            if (!names[i] || is_meta_target(atoms[i]) || atoms[i] == A_PASSWORD_HINT ||
                (have_text && is_legacy_text(atoms[i])))
                continue;
            int dup = 0;
            for (int k = 0; k < cap.nwant; k++)
                if (cap.want[k] == atoms[i])
                    dup = 1;
            if (!dup)
                cap.want[cap.nwant++] = atoms[i];
        }
    }
    for (unsigned long i = 0; i < n; i++)
        if (names[i]) XFree(names[i]);
    free(names);
    fetch_next();
}

/* One target's full data is in; keep it or drop it per the size limits. */
static void cap_store_rep(Atom target, Atom type, int format, const unsigned char *data, size_t len)
{
    char *tname = XGetAtomName(dpy, target);
    char *tyname = type != None ? XGetAtomName(dpy, type) : NULL;
    if (!tname) {
        if (tyname) XFree(tyname);
        return;
    }
    int ci = canonical_index(tname);
    int is_image = strncmp(tname, "image/", 6) == 0;
    size_t limit = is_image ? cfg.max_image : ci >= 0 ? cfg.max_text : cfg.max_mirror;
    int persist = ci >= 0;
    /* One text encoding on disk: the first one fetched (UTF-8 when offered). */
    if (persist && ci < N_TEXT_TARGETS) {
        for (int i = 0; i < cap.item->nreps; i++)
            if (canonical_index(cap.item->reps[i].target) >= 0 &&
                canonical_index(cap.item->reps[i].target) < N_TEXT_TARGETS)
                persist = 0;
    }
    if (len > limit || cap.mirrored + len > cfg.max_mirror + cfg.max_image) {
        cap.skipped = 1;
    } else {
        km_item_add_rep(cap.item, tname, tyname ? tyname : "", format, data, len, persist);
        cap.mirrored += len;
    }
    XFree(tname);
    if (tyname) XFree(tyname);
}

static void on_selection_notify(XSelectionEvent *ev)
{
    if (!cap.active || ev->requestor != win || ev->selection != A_CLIPBOARD)
        return;
    /* A late answer to a conversion of an aborted capture. */
    if (ev->target != cap.pending || (cap.time != CurrentTime && ev->time != cap.time))
        return;

    if (ev->property == A_KM_TARGETS || (ev->target == A_TARGETS && ev->property == None)) {
        if (ev->property == None) {
            cap_plan(NULL, 0);
            return;
        }
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, win, A_KM_TARGETS, 0, 4096, True, AnyPropertyType, &type, &format,
                               &n, &after, &data) == Success && data && format == 32) {
            cap_plan((Atom *)(void *)data, n);
        } else {
            cap_plan(NULL, 0);
        }
        if (data)
            XFree(data);
        return;
    }

    if (ev->property == None) {
        /* The owner refused a target it listed (xsel does this for
         * legacy encodings): it just won't be offered when we serve. */
        dbg_target("refused", ev->target);
        fetch_next();
        return;
    }

    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, A_KM_DATA, 0, 0x7fffffff / 4, True, AnyPropertyType, &type, &format,
                           &n, &after, &data) != Success) {
        cap.skipped = 1;
        fetch_next();
        return;
    }
    if (type == A_INCR) {
        /* Big transfer: the owner now sends chunks, each announced by a
         * PropertyNotify(NewValue) on our window; the property delete
         * above already asked for the first one. */
        cap.incr = 1;
        cap.incr_target = ev->target;
        cap.incr_len = 0;
        cap.deadline = now_ms() + FETCH_TIMEOUT_MS;
        if (data)
            XFree(data);
        return;
    }
    if (data) {
        cap_store_rep(ev->target, type, format, data, n * elem_size(format));
        XFree(data);
    }
    fetch_next();
}

static void on_incr_chunk(void)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, win, A_KM_DATA, 0, 0x7fffffff / 4, True, AnyPropertyType, &type, &format,
                           &n, &after, &data) != Success)
        return;
    size_t bytes = data ? n * elem_size(format) : 0;
    cap.deadline = now_ms() + FETCH_TIMEOUT_MS;

    if (bytes == 0) {   /* zero-length chunk: transfer complete */
        if (data)
            XFree(data);
        if (cap.incr_buf)
            cap_store_rep(cap.incr_target, cap.incr_type, cap.incr_format, cap.incr_buf, cap.incr_len);
        else
            cap.skipped = 1;
        free(cap.incr_buf);
        cap.incr_buf = NULL;
        cap.incr = 0;
        fetch_next();
        return;
    }

    /* Past every limit: keep draining (the owner expects it) but drop. */
    size_t cap_limit = cfg.max_image > cfg.max_mirror ? cfg.max_image : cfg.max_mirror;
    if (cap.incr_len + bytes > cap_limit) {
        free(cap.incr_buf);
        cap.incr_buf = NULL;
        cap.incr_len = cap_limit + 1;
    } else if (cap.incr_len <= cap_limit) {
        unsigned char *nb = realloc(cap.incr_buf, cap.incr_len + bytes);
        if (nb) {
            memcpy(nb + cap.incr_len, data, bytes);
            cap.incr_buf = nb;
            cap.incr_len += bytes;
            cap.incr_type = type;
            cap.incr_format = format;
        }
    }
    XFree(data);
}


/* ---- serving (we own CLIPBOARD) ---- */

static Bool is_time_notify(Display *d, XEvent *ev, XPointer arg)
{
    (void)d;
    (void)arg;
    return ev->type == PropertyNotify && ev->xproperty.window == win && ev->xproperty.atom == A_KM_TIME;
}

/* A real server timestamp (ICCCM forbids CurrentTime for ownership):
 * a zero-length append to our own window, then its PropertyNotify. */
static Time server_time(void)
{
    unsigned char none = 0;
    XChangeProperty(dpy, win, A_KM_TIME, XA_STRING, 8, PropModeAppend, &none, 0);
    XEvent ev;
    XIfEvent(dpy, &ev, is_time_notify, NULL);
    return ev.xproperty.time;
}

static void take_ownership(void)
{
    if (!current_id || !km_store_get(current_id))
        return;
    Time t = server_time();
    XSetSelectionOwner(dpy, A_CLIPBOARD, win, t);
    owned = XGetSelectionOwner(dpy, A_CLIPBOARD) == win;
    if (owned) {
        own_time = t;
        fprintf(stderr, "kimemoryd: serving #%u\n", current_id);
    }
}

static int is_text_alias(Atom a)
{
    return a == A_UTF8 || a == XA_STRING || a == A_TEXT || a == A_TEXT_PLAIN || a == A_TEXT_PLAIN_UTF8;
}

static KmRep *rep_for_target(KmItem *it, Atom target, Atom *type_out)
{
    char *name = XGetAtomName(dpy, target);
    if (!name)
        return NULL;
    KmRep *r = km_item_find_rep(it, name);
    XFree(name);
    if (r) {
        *type_out = r->type[0] ? XInternAtom(dpy, r->type, False) : target;
        return r;
    }
    if (!is_text_alias(target))
        return NULL;
    static const char *text_names[] = { "UTF8_STRING", "text/plain;charset=utf-8", "text/plain", "STRING" };
    for (size_t i = 0; i < sizeof(text_names) / sizeof(text_names[0]); i++) {
        r = km_item_find_rep(it, text_names[i]);
        if (r) {
            *type_out = target == A_TEXT ? A_UTF8 : target;
            return r;
        }
    }
    return NULL;
}

static size_t chunk_max(void)
{
    long req = XExtendedMaxRequestSize(dpy);
    if (req <= 0)
        req = XMaxRequestSize(dpy);
    size_t max = (size_t)req * 4 - 1024;
    return max > 262144 ? 262144 : max;
}

static void xfer_remove(int i)
{
    XSelectInput(dpy, xfer[i].requestor, NoEventMask);
    free(xfer[i].data);
    xfer[i] = xfer[--nxfer];
}

static int xfer_start(Window requestor, Atom property, Atom type, int format,
                      const unsigned char *data, size_t len)
{
    if (nxfer == MAX_TRANSFERS)
        return 0;
    unsigned char *copy = malloc(len ? len : 1);
    if (!copy)
        return 0;
    memcpy(copy, data, len);
    xfer[nxfer].requestor = requestor;
    xfer[nxfer].property = property;
    xfer[nxfer].type = type;
    xfer[nxfer].format = format;
    xfer[nxfer].data = copy;
    xfer[nxfer].len = len;
    xfer[nxfer].off = 0;
    xfer[nxfer].done_sent = 0;
    xfer[nxfer].deadline = now_ms() + INCR_TIMEOUT_MS;
    nxfer++;
    XSelectInput(dpy, requestor, PropertyChangeMask);
    long total = (long)len;
    XChangeProperty(dpy, requestor, property, A_INCR, 32, PropModeReplace, (unsigned char *)&total, 1);
    return 1;
}

/* The requestor deleted the property: send the next chunk (or the end). */
static void xfer_continue(int i)
{
    size_t es = elem_size(xfer[i].format);
    if (xfer[i].done_sent) {
        xfer_remove(i);
        return;
    }
    size_t left = xfer[i].len - xfer[i].off;
    size_t n = left < chunk_max() ? left : chunk_max();
    n -= n % es;
    XChangeProperty(dpy, xfer[i].requestor, xfer[i].property, xfer[i].type, xfer[i].format,
                    PropModeReplace, xfer[i].data + xfer[i].off, (int)(n / es));
    xfer[i].off += n;
    if (n == 0)
        xfer[i].done_sent = 1;
    xfer[i].deadline = now_ms() + INCR_TIMEOUT_MS;
}

static void record_paste(Window requestor)
{
    long t = now_ms();
    if (requestor == last_paste.requestor && t - last_paste.at < PASTE_DEBOUNCE_MS) {
        last_paste.at = t;
        return;   /* same paste asking for several targets */
    }
    last_paste.requestor = requestor;
    last_paste.at = t;

    KmItem *it = km_store_get(current_id);
    if (!it)
        return;
    Window top = xis_winident_toplevel_for(dpy, requestor);
    if (top == None)
        top = xis_winident_active(dpy);
    XisWinIdent dst;
    if (!xis_winident_get(dpy, top, &dst))
        return;
    const char *app = dst.wm_class[0] ? dst.wm_class : exe_base(dst.exe);
    km_store_add_ref(it, 1, (long)time(NULL), dst.win, app, dst.title, dst.doc_hint);
    fprintf(stderr, "kimemoryd: #%u pasted into %s\n", it->id, app);
}

/* Writes one target into requestor's property; 1 on success. */
static int serve_target(KmItem *it, Window requestor, Atom property, Atom target)
{
    if (target == A_TARGETS) {
        Atom list[256];
        int n = 0;
        list[n++] = A_TARGETS;
        list[n++] = A_TIMESTAMP;
        list[n++] = A_MULTIPLE;
        int text = 0;
        for (int i = 0; i < it->nreps && n < 250; i++) {
            Atom a = XInternAtom(dpy, it->reps[i].target, False);
            list[n++] = a;
            text |= is_text_alias(a);
        }
        if (text) {
            Atom aliases[] = { A_UTF8, A_TEXT_PLAIN_UTF8, A_TEXT_PLAIN, XA_STRING, A_TEXT };
            for (size_t k = 0; k < sizeof(aliases) / sizeof(aliases[0]); k++) {
                int dup = 0;
                for (int j = 0; j < n; j++)
                    dup |= list[j] == aliases[k];
                if (!dup)
                    list[n++] = aliases[k];
            }
        }
        XChangeProperty(dpy, requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, n);
        return 1;
    }
    if (target == A_TIMESTAMP) {
        long t = (long)own_time;
        XChangeProperty(dpy, requestor, property, XA_INTEGER, 32, PropModeReplace, (unsigned char *)&t, 1);
        return 1;
    }

    Atom type;
    KmRep *r = rep_for_target(it, target, &type);
    const unsigned char *data = r ? km_rep_data(it, r) : NULL;
    if (!data)
        return 0;
    int format = r->format ? r->format : 8;
    if (r->len > chunk_max()) {
        if (!xfer_start(requestor, property, type, format, data, r->len))
            return 0;
    } else {
        XChangeProperty(dpy, requestor, property, type, format, PropModeReplace, data,
                        (int)(r->len / elem_size(format)));
    }
    record_paste(requestor);
    return 1;
}

static void on_selection_request(XSelectionRequestEvent *rq)
{
    XSelectionEvent reply = {
        .type = SelectionNotify, .display = dpy, .requestor = rq->requestor,
        .selection = rq->selection, .target = rq->target, .property = None, .time = rq->time,
    };
    /* Obsolete clients send property None: ICCCM says use the target. */
    Atom property = rq->property != None ? rq->property : rq->target;
    KmItem *it = owned && rq->selection == A_CLIPBOARD ? km_store_get(current_id) : NULL;

    if (it && rq->target == A_MULTIPLE) {
        Atom type;
        int format;
        unsigned long n, after;
        unsigned char *data = NULL;
        if (XGetWindowProperty(dpy, rq->requestor, property, 0, 1024, False, AnyPropertyType, &type,
                               &format, &n, &after, &data) == Success && data && format == 32) {
            Atom *pairs = (Atom *)(void *)data;
            for (unsigned long i = 0; i + 1 < n; i += 2)
                if (!serve_target(it, rq->requestor, pairs[i + 1], pairs[i]))
                    pairs[i + 1] = None;
            XChangeProperty(dpy, rq->requestor, property, A_ATOM_PAIR, 32, PropModeReplace, data, (int)n);
            reply.property = property;
        }
        if (data)
            XFree(data);
    } else if (it && serve_target(it, rq->requestor, property, rq->target)) {
        reply.property = property;
    }
    if (it)
        km_item_unload(it);
    XSendEvent(dpy, rq->requestor, False, NoEventMask, (XEvent *)&reply);
}

int km_clip_set_current(unsigned id)
{
    KmItem *it = km_store_get(id);
    if (!it)
        return 0;
    if (current_id && current_id != id) {
        KmItem *prev = km_store_get(current_id);
        if (prev)
            km_item_drop_extras(prev);
    }
    current_id = id;
    km_store_touch(it);
    take_ownership();
    return owned;
}

int km_clip_owned(void)
{
    return owned;
}

int km_clip_handle_event(XEvent *ev)
{
    if (ev->type == xfixes_ev + XFixesSelectionNotify) {
        XFixesSelectionNotifyEvent *se = (XFixesSelectionNotifyEvent *)ev;
        if (se->selection != A_CLIPBOARD)
            return 1;
        if (se->subtype == XFixesSetSelectionOwnerNotify && se->owner != None && se->owner != win)
            cap_start(se->owner, se->timestamp);
        /* The owner quit (window destroyed / client gone) and nothing
         * owns the clipboard now: keep its last copy alive from here. */
        else if ((se->subtype == XFixesSelectionWindowDestroyNotify ||
                  se->subtype == XFixesSelectionClientCloseNotify) &&
                 !owned && cfg.takeover != KM_TAKEOVER_NEVER && !cap.active &&
                 XGetSelectionOwner(dpy, A_CLIPBOARD) == None)
            take_ownership();
        return 1;
    }
    if (ev->type == SelectionRequest && ev->xselectionrequest.owner == win) {
        on_selection_request(&ev->xselectionrequest);
        return 1;
    }
    if (ev->type == SelectionClear && ev->xselectionclear.window == win) {
        if (ev->xselectionclear.selection == A_CLIPBOARD)
            owned = 0;
        return 1;
    }
    if (ev->type == PropertyNotify && ev->xproperty.state == PropertyDelete && ev->xproperty.window != win) {
        for (int i = 0; i < nxfer; i++) {
            if (xfer[i].requestor == ev->xproperty.window && xfer[i].property == ev->xproperty.atom) {
                xfer_continue(i);
                return 1;
            }
        }
    }
    if (ev->type == SelectionNotify && ev->xselection.requestor == win) {
        on_selection_notify(&ev->xselection);
        return 1;
    }
    if (ev->type == PropertyNotify && ev->xproperty.window == win) {
        if (cap.active && cap.incr && ev->xproperty.atom == A_KM_DATA &&
            ev->xproperty.state == PropertyNewValue)
            on_incr_chunk();
        return 1;
    }
    return 0;
}

int km_clip_timeout_ms(void)
{
    long deadline = cap.active ? cap.deadline : 0;
    for (int i = 0; i < nxfer; i++)
        if (!deadline || xfer[i].deadline < deadline)
            deadline = xfer[i].deadline;
    if (!deadline)
        return -1;
    long left = deadline - now_ms();
    return left > 0 ? (int)left : 0;
}

void km_clip_tick(void)
{
    long t = now_ms();
    for (int i = nxfer - 1; i >= 0; i--)
        if (t >= xfer[i].deadline)
            xfer_remove(i);   /* requestor stopped reading */
    if (!cap.active || t < cap.deadline)
        return;
    if (!cap.want) {   /* TARGETS never answered */
        cap_plan(NULL, 0);
        return;
    }
    cap.skipped = 1;
    dbg_target(cap.incr ? "timeout (INCR)" : "timeout", cap.pending);
    if (cap.incr) {
        free(cap.incr_buf);
        cap.incr_buf = NULL;
        cap.incr = 0;
    }
    fetch_next();
}

unsigned km_clip_current(void)
{
    return current_id;
}

void km_clip_set_config(const KmClipConfig *c)
{
    cfg = *c;
}

int km_clip_init(Display *d, const KmClipConfig *c)
{
    dpy = d;
    cfg = *c;
    debug = getenv("KIMEMORYD_DEBUG") && *getenv("KIMEMORYD_DEBUG") == '1';
    int err;
    if (!XFixesQueryExtension(dpy, &xfixes_ev, &err))
        return 0;

    A_CLIPBOARD        = XInternAtom(dpy, "CLIPBOARD", False);
    A_TARGETS          = XInternAtom(dpy, "TARGETS", False);
    A_TIMESTAMP        = XInternAtom(dpy, "TIMESTAMP", False);
    A_MULTIPLE         = XInternAtom(dpy, "MULTIPLE", False);
    A_INCR             = XInternAtom(dpy, "INCR", False);
    A_SAVE_TARGETS     = XInternAtom(dpy, "SAVE_TARGETS", False);
    A_DELETE           = XInternAtom(dpy, "DELETE", False);
    A_INSERT_SELECTION = XInternAtom(dpy, "INSERT_SELECTION", False);
    A_INSERT_PROPERTY  = XInternAtom(dpy, "INSERT_PROPERTY", False);
    A_UTF8             = XInternAtom(dpy, "UTF8_STRING", False);
    A_KM_TARGETS       = XInternAtom(dpy, "_KIMEMORY_TARGETS", False);
    A_KM_DATA          = XInternAtom(dpy, "_KIMEMORY_DATA", False);
    A_PASSWORD_HINT    = XInternAtom(dpy, "x-kde-passwordManagerHint", False);
    A_KM_TIME          = XInternAtom(dpy, "_KIMEMORY_TIME", False);
    A_ATOM_PAIR        = XInternAtom(dpy, "ATOM_PAIR", False);
    A_TEXT             = XInternAtom(dpy, "TEXT", False);
    A_COMPOUND_TEXT    = XInternAtom(dpy, "COMPOUND_TEXT", False);
    A_TEXT_PLAIN       = XInternAtom(dpy, "text/plain", False);
    A_TEXT_PLAIN_UTF8  = XInternAtom(dpy, "text/plain;charset=utf-8", False);

    Window root = DefaultRootWindow(dpy);
    win = XCreateSimpleWindow(dpy, root, -10, -10, 1, 1, 0, 0, 0);
    XStoreName(dpy, win, "kimemoryd");
    XSelectInput(dpy, win, PropertyChangeMask);
    XFixesSelectSelectionInput(dpy, root, A_CLIPBOARD,
                               XFixesSetSelectionOwnerNotifyMask |
                               XFixesSelectionWindowDestroyNotifyMask |
                               XFixesSelectionClientCloseNotifyMask);

    Window owner = XGetSelectionOwner(dpy, A_CLIPBOARD);
    if (owner != None)
        cap_start(owner, CurrentTime);
    return 1;
}

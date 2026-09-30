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

static Display *dpy;
static Window win;            /* ours: requestor for fetches */
static int xfixes_ev;
static KmClipConfig cfg;

static Atom A_CLIPBOARD, A_TARGETS, A_TIMESTAMP, A_MULTIPLE, A_INCR, A_SAVE_TARGETS,
            A_DELETE, A_INSERT_SELECTION, A_INSERT_PROPERTY, A_UTF8, A_KM_TARGETS, A_KM_DATA,
            A_PASSWORD_HINT;

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

static struct {
    int active;
    Window owner;
    Time time;
    Atom *want;
    int nwant, next;
    Atom pending;         /* target of the conversion in flight */
    int rich;
    int skipped;          /* a rep was dropped (size limit / failed): can't mirror exactly */
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
    return a == A_TARGETS || a == A_TIMESTAMP || a == A_MULTIPLE || a == A_SAVE_TARGETS ||
           a == A_DELETE || a == A_INSERT_SELECTION || a == A_INSERT_PROPERTY || a == None;
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
    XDeleteProperty(dpy, win, A_KM_DATA);
    XConvertSelection(dpy, A_CLIPBOARD, t, A_KM_DATA, win, cap.time);
    cap.deadline = now_ms() + FETCH_TIMEOUT_MS;
}

static void cap_start(Window owner, Time t)
{
    cap_reset();
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
                if (have_text && cap.rich)
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
            if (!names[i] || is_meta_target(atoms[i]) || atoms[i] == A_PASSWORD_HINT)
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

    if (ev->property == None) {   /* the owner refused this target */
        cap.skipped = 1;
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

int km_clip_handle_event(XEvent *ev)
{
    if (ev->type == xfixes_ev + XFixesSelectionNotify) {
        XFixesSelectionNotifyEvent *se = (XFixesSelectionNotifyEvent *)ev;
        if (se->selection != A_CLIPBOARD)
            return 1;
        if (se->subtype == XFixesSetSelectionOwnerNotify && se->owner != None && se->owner != win)
            cap_start(se->owner, se->timestamp);
        return 1;
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
    if (!cap.active)
        return -1;
    long left = cap.deadline - now_ms();
    return left > 0 ? (int)left : 0;
}

void km_clip_tick(void)
{
    if (!cap.active || now_ms() < cap.deadline)
        return;
    if (!cap.want) {   /* TARGETS never answered */
        cap_plan(NULL, 0);
        return;
    }
    cap.skipped = 1;
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

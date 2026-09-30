/* ks_windows - see ks_windows.h. */
#include "ks_windows.h"
#include "ks_config.h"
#include "ks_log.h"

#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_OUTPUTS 32
#define MAX_DWELL_TITLES 12

static Display *dpy;
static Window root;
static int randr_ev = -1;

static Atom A_CLIENT_LIST, A_ACTIVE, A_WM_NAME_NET, A_WM_DESKTOP, A_KIWM_WM_OUTPUT,
            A_KIWM_OUTPUT_DESKTOP, A_KIWM_OUTPUTS, A_CURRENT_DESKTOP;

typedef struct {
    Window win;
    XisWinIdent id;
    time_t opened;
} KsWin;

static KsWin *wins;
static int nwins, capwins;

static struct {
    int active;
    Window win;
    time_t start;
    XisWinIdent id;
    char titles[1024];
    int ntitles;
    int hidden;
} dwell;

static char outputs[MAX_OUTPUTS][64];
static int noutputs;
static long desktops[MAX_OUTPUTS];   /* per kiwm output index; [0] = EWMH desktop without kiwm */
static int ndesktops = -1;           /* -1: not read yet */

static const char *app_of(const XisWinIdent *id)
{
    return id->wm_class[0] ? id->wm_class : ks_exe_base(id->exe);
}

static int excluded(const XisWinIdent *id)
{
    return ks_excluded(id->wm_class, id->wm_instance, id->exe);
}

static KsWin *win_find(Window w)
{
    for (int i = 0; i < nwins; i++)
        if (wins[i].win == w)
            return &wins[i];
    return NULL;
}

static Window *get_windows(Atom prop, unsigned long *n)
{
    Atom type;
    int format;
    unsigned long after;
    unsigned char *data = NULL;
    *n = 0;
    if (XGetWindowProperty(dpy, root, prop, 0, 8192, False, XA_WINDOW, &type, &format, n, &after, &data) != Success ||
        !data || format != 32) {
        if (data)
            XFree(data);
        *n = 0;
        return NULL;
    }
    Window *out = malloc(sizeof(Window) * (*n ? *n : 1));
    for (unsigned long i = 0; out && i < *n; i++)
        out[i] = (Window)((long *)(void *)data)[i];
    XFree(data);
    return out;
}

/* ---- focus periods ---- */

static void dwell_add_title(const char *title)
{
    if (!title || !*title)
        return;
    if (ks_titles_hidden(dwell.id.wm_class, dwell.id.wm_instance, dwell.id.exe, title))
        dwell.hidden = 1;   /* one private title hides the whole period */

    /* Skip if already listed (as a whole " | "-separated item). */
    size_t tl = strlen(title);
    for (const char *p = dwell.titles; (p = strstr(p, title)) != NULL; p++) {
        int starts = p == dwell.titles || (p >= dwell.titles + 3 && !strncmp(p - 3, " | ", 3));
        int ends = p[tl] == '\0' || !strncmp(p + tl, " | ", 3);
        if (starts && ends)
            return;
    }
    if (dwell.ntitles >= MAX_DWELL_TITLES)
        return;
    size_t used = strlen(dwell.titles);
    if (used + tl + 4 >= sizeof(dwell.titles))
        return;
    if (used)
        strcat(dwell.titles, " | ");
    strcat(dwell.titles, title);
    dwell.ntitles++;
}

static void dwell_end(time_t now)
{
    if (!dwell.active)
        return;
    dwell.active = 0;
    if (excluded(&dwell.id) || now - dwell.start < ks_conf.min_dwell_s)
        return;
    char range[64];
    ks_log_range(range, sizeof(range), dwell.start, now);
    ks_log_event(dwell.start, "focus", app_of(&dwell.id), dwell.id.exe, dwell.id.desktop, dwell.id.output,
                 range, dwell.hidden ? "" : dwell.titles);
}

static void dwell_start(Window w, time_t now)
{
    KsWin *kw = win_find(w);
    XisWinIdent id;
    if (kw)
        id = kw->id;
    else if (!xis_winident_get(dpy, w, &id))
        return;
    memset(&dwell, 0, sizeof(dwell));
    dwell.active = 1;
    dwell.win = w;
    dwell.start = now;
    dwell.id = id;
    dwell.hidden = ks_titles_hidden(id.wm_class, id.wm_instance, id.exe, id.title);
    dwell_add_title(id.title);
}

void ks_windows_flush(void)
{
    dwell_end(time(NULL));
}

/* ---- windows opening/closing ---- */

static void log_win(const char *kind, const KsWin *kw, time_t ts, const char *extra)
{
    if (excluded(&kw->id))
        return;
    int hidden = ks_titles_hidden(kw->id.wm_class, kw->id.wm_instance, kw->id.exe, kw->id.title);
    char detail[96];
    snprintf(detail, sizeof(detail), "win=0x%lx%s%s", kw->win, extra && *extra ? " " : "", extra ? extra : "");
    ks_log_event(ts, kind, app_of(&kw->id), kw->id.exe, kw->id.desktop, kw->id.output,
                 hidden ? "" : kw->id.title, detail);
}

static void sync_clients(int at_start)
{
    unsigned long n;
    Window *list = get_windows(A_CLIENT_LIST, &n);
    time_t now = time(NULL);

    /* Gone. */
    for (int i = nwins - 1; i >= 0; i--) {
        int present = 0;
        for (unsigned long k = 0; k < n && !present; k++)
            present = list[k] == wins[i].win;
        if (present)
            continue;
        if (dwell.active && dwell.win == wins[i].win)
            dwell_end(now);
        char extra[32];
        snprintf(extra, sizeof(extra), "open=%lds", (long)(now - wins[i].opened));
        log_win("win_close", &wins[i], now, extra);
        wins[i] = wins[--nwins];
    }

    /* New. */
    for (unsigned long k = 0; k < n; k++) {
        if (win_find(list[k]))
            continue;
        KsWin kw = { .win = list[k], .opened = now };
        if (!xis_winident_get(dpy, list[k], &kw.id))
            continue;
        if (nwins == capwins) {
            int ncap = capwins ? capwins * 2 : 64;
            KsWin *nw = realloc(wins, sizeof(KsWin) * (size_t)ncap);
            if (!nw)
                break;
            wins = nw;
            capwins = ncap;
        }
        wins[nwins++] = kw;
        XSelectInput(dpy, list[k], PropertyChangeMask);
        log_win("win_open", &kw, now, at_start ? "at_start" : NULL);
    }
    free(list);
}

/* ---- desktops and outputs ---- */

static int read_outputs(char names[][64], int max)
{
    int n = 0, count = 0;
    XRRMonitorInfo *mons = XRRGetMonitors(dpy, root, True, &n);
    for (int i = 0; mons && i < n && count < max; i++) {
        char *name = XGetAtomName(dpy, mons[i].name);
        if (name) {
            snprintf(names[count++], 64, "%s", name);
            XFree(name);
        }
    }
    if (mons)
        XRRFreeMonitors(mons);
    return count;
}

static void sync_outputs(int at_start)
{
    char now_names[MAX_OUTPUTS][64];
    int n = read_outputs(now_names, MAX_OUTPUTS);
    time_t now = time(NULL);
    for (int i = 0; i < noutputs; i++) {
        int still = 0;
        for (int k = 0; k < n && !still; k++)
            still = !strcmp(outputs[i], now_names[k]);
        if (!still)
            ks_log_event(now, "output", "", "", -1, outputs[i], "disconnected", "");
    }
    for (int k = 0; k < n; k++) {
        int had = 0;
        for (int i = 0; i < noutputs && !had; i++)
            had = !strcmp(outputs[i], now_names[k]);
        if (!had)
            ks_log_event(now, "output", "", "", -1, now_names[k], "connected", at_start ? "at_start" : "");
    }
    memcpy(outputs, now_names, sizeof(now_names));
    noutputs = n;
}

/* kiwm: _KIWM_OUTPUT_DESKTOP (one desktop per output, names in
 * _KIWM_OUTPUTS); any other WM: _NET_CURRENT_DESKTOP. */
static void sync_desktops(int at_start)
{
    long now_desk[MAX_OUTPUTS];
    int n = 0;
    char names[MAX_OUTPUTS][64] = { { 0 } };

    Atom type;
    int format;
    unsigned long items, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(dpy, root, A_KIWM_OUTPUT_DESKTOP, 0, MAX_OUTPUTS, False, XA_CARDINAL, &type, &format,
                           &items, &after, &data) == Success && data && format == 32 && items > 0) {
        for (unsigned long i = 0; i < items && n < MAX_OUTPUTS; i++)
            now_desk[n++] = ((long *)(void *)data)[i];
        XFree(data);
        data = NULL;
        if (XGetWindowProperty(dpy, root, A_KIWM_OUTPUTS, 0, 4096, False, AnyPropertyType, &type, &format,
                               &items, &after, &data) == Success && data && format == 8) {
            unsigned long off = 0;
            for (int i = 0; i < n && off < items; i++) {
                snprintf(names[i], 64, "%s", (char *)data + off);
                off += strlen((char *)data + off) + 1;
            }
        }
    } else {
        if (data)
            XFree(data);
        data = NULL;
        if (XGetWindowProperty(dpy, root, A_CURRENT_DESKTOP, 0, 1, False, XA_CARDINAL, &type, &format,
                               &items, &after, &data) == Success && data && format == 32 && items > 0)
            now_desk[n++] = *(long *)(void *)data;
    }
    if (data)
        XFree(data);

    time_t now = time(NULL);
    for (int i = 0; i < n; i++) {
        if (ndesktops >= 0 && i < ndesktops && desktops[i] == now_desk[i])
            continue;
        ks_log_event(now, "desktop", "", "", now_desk[i], names[i], at_start ? "at_start" : "switch", "");
    }
    memcpy(desktops, now_desk, sizeof(long) * (size_t)n);
    ndesktops = n;
}

/* ---- events ---- */

int ks_windows_handle_event(XEvent *ev)
{
    if (randr_ev >= 0 && ev->type == randr_ev + RRScreenChangeNotify) {
        XRRUpdateConfiguration(ev);
        sync_outputs(0);
        return 1;
    }
    if (ev->type != PropertyNotify)
        return 0;
    XPropertyEvent *pe = &ev->xproperty;
    time_t now = time(NULL);

    if (pe->window == root) {
        if (pe->atom == A_CLIENT_LIST) {
            sync_clients(0);
        } else if (pe->atom == A_ACTIVE) {
            Window w = xis_winident_active(dpy);
            if (!dwell.active || w != dwell.win) {
                dwell_end(now);
                if (w != None)
                    dwell_start(w, now);
            }
        } else if (pe->atom == A_KIWM_OUTPUT_DESKTOP || pe->atom == A_CURRENT_DESKTOP) {
            sync_desktops(0);
        } else if (pe->atom == A_KIWM_OUTPUTS) {
            sync_outputs(0);
        }
        return 1;
    }

    KsWin *kw = win_find(pe->window);
    if (!kw)
        return 0;
    if (pe->atom == A_WM_NAME_NET || pe->atom == XA_WM_NAME || pe->atom == A_WM_DESKTOP ||
        pe->atom == A_KIWM_WM_OUTPUT) {
        XisWinIdent id;
        if (xis_winident_get(dpy, kw->win, &id))
            kw->id = id;
        if (dwell.active && dwell.win == kw->win && (pe->atom == A_WM_NAME_NET || pe->atom == XA_WM_NAME))
            dwell_add_title(kw->id.title);
    }
    return 1;
}

int ks_windows_init(Display *d)
{
    dpy = d;
    root = DefaultRootWindow(dpy);
    A_CLIENT_LIST         = XInternAtom(dpy, "_NET_CLIENT_LIST", False);
    A_ACTIVE              = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
    A_WM_NAME_NET         = XInternAtom(dpy, "_NET_WM_NAME", False);
    A_WM_DESKTOP          = XInternAtom(dpy, "_NET_WM_DESKTOP", False);
    A_KIWM_WM_OUTPUT      = XInternAtom(dpy, "_KIWM_WM_OUTPUT", False);
    A_KIWM_OUTPUT_DESKTOP = XInternAtom(dpy, "_KIWM_OUTPUT_DESKTOP", False);
    A_KIWM_OUTPUTS        = XInternAtom(dpy, "_KIWM_OUTPUTS", False);
    A_CURRENT_DESKTOP     = XInternAtom(dpy, "_NET_CURRENT_DESKTOP", False);

    int err;
    if (XRRQueryExtension(dpy, &randr_ev, &err))
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask);
    else
        randr_ev = -1;
    XSelectInput(dpy, root, PropertyChangeMask);

    sync_outputs(1);
    sync_desktops(1);
    sync_clients(1);
    Window w = xis_winident_active(dpy);
    if (w != None)
        dwell_start(w, time(NULL));
    return 1;
}

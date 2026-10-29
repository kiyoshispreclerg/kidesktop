/* ks.h - kishot: screenshots and screen recording for KiDesktop. */
#ifndef KS_H
#define KS_H

#include "../shared/xis_inputscale.h"
#include <gtk/gtk.h>
#include <stdint.h>
#include <xcb/xcb.h>

#define KISHOT_VERSION "0.1.17"

typedef enum {
    KS_AREA_FULL,       /* every monitor */
    KS_AREA_MONITOR,    /* the monitor under the pointer */
    KS_AREA_WINDOW,     /* the active window */
    KS_AREA_REGION,     /* a rectangle the user drags */
    KS_AREA_OUTPUT,     /* one named monitor (KsConfig.output) */
    KS_AREA_N
} KsArea;

typedef enum { KS_COPY_AUTO, KS_COPY_GPU, KS_COPY_SHM } KsCopyMode;
typedef enum { KS_BACKEND_AUTO, KS_BACKEND_PLUGIN, KS_BACKEND_CLI } KsBackend;

/* tabs of the main window, and the matching Settings pages */
typedef enum { KS_TAB_SHOT, KS_TAB_RECORD } KsTab;

typedef struct {
    KsArea area;                /* screenshots */
    KsArea rec_area;            /* recordings */
    char *output;               /* connector name for KS_AREA_OUTPUT, screenshots */
    char *rec_output;           /* ... and recordings */
    int delay;                  /* seconds before a screenshot */
    gboolean pointer;           /* draw the mouse pointer */
    gboolean decorations;       /* window mode: include the frame */
    gboolean auto_save;         /* save as soon as the shot is taken */
    gboolean auto_copy;         /* ... and/or copy it to the clipboard */
    char *shot_dir;
    char *shot_format;          /* gdk-pixbuf writer: png, jpeg, bmp, tiff, webp... */
    int jpeg_quality;
    char *editor;               /* command to edit a shot with; "" = the default image app */
    char *name_pattern;         /* strftime() pattern without extension */

    char *video_dir;
    char *container;            /* mkv, mp4, webm */
    char *vcodec;               /* encoder name or "auto" */
    int quality;                /* 0 (best) .. 51, CRF/QP-like */
    int fps;                    /* 0 = Auto: the refresh rate of the screen recorded */
    gboolean rec_pointer;
    gboolean audio_system;
    gboolean audio_mic;
    gboolean rec_preview;       /* live preview in the Recording tab */
    gboolean hide_on_record;    /* hide the window while recording (it is in the video otherwise) */
    KsCopyMode copy_mode;
    KsBackend backend;

    int win_w, win_h;
    int pane_pos;               /* preview | options divider, 0 = default */
} KsConfig;

/* ---- tray indicator while recording (ks_tray.c) ------------------------- */

typedef void (*KsTrayActivate)(void *data);
void ks_tray_show(KsTrayActivate cb, void *data);   /* click = cb */
void ks_tray_set_tooltip(const char *text);
void ks_tray_hide(void);

void ks_config_load(KsConfig *c);
void ks_config_save(const KsConfig *c);
const char *ks_area_key(KsArea a);
char *ks_config_expand_dir(const char *dir);

/* ---- geometry helpers (ks_grab.c) ---------------------------------------- */

typedef struct { int x, y, w, h; } KsRect;

gboolean ks_area_rect(KsArea area, gboolean decorations, KsRect *out);
/* Same, but an active window equal to `except` (ours) falls back to the monitor. */
gboolean ks_area_rect_except(KsArea area, gboolean decorations, unsigned long except, KsRect *out);
GdkPixbuf *ks_grab_rect(const KsRect *r, gboolean pointer);
/* r scaled to dw x dh on the server, for the live recording preview */
GdkPixbuf *ks_grab_scaled(const KsRect *r, int dw, int dh, gboolean pointer);
gboolean ks_rect_clip_to_screen(KsRect *r);
/* refresh rate in Hz of the screen a recording of r follows, 0 if unknown */
int ks_refresh_rate_for_rect(const KsRect *r);
int ks_monitor_count(void);
/* connector name of monitor i ("DP-1"), or "Monitor N"; g_free() it */
char *ks_monitor_name(int i);
/* geometry of the monitor with that name; FALSE when it is not connected */
gboolean ks_monitor_rect_by_name(const char *name, KsRect *out);

/* ---- censoring (ks_censor.c) --------------------------------------------- */

void ks_censor_init(void (*changed)(void *), void *data);
/* marks the window (a toplevel as returned by ks_censor_pick()) */
void ks_censor_add(unsigned long toplevel);
/* re-reads the marks and the stacking now, instead of waiting for the idle */
void ks_censor_refresh(void);
/* whether a compositing manager owns _NET_WM_CM_Sn */
gboolean ks_censor_compositor(void);
/* client windows carrying _KIWM_CAPTURE_EXCLUDE, for the list in the UI. The
 * list only grows from here: unmarking is done in the window's own menu. */
GArray *ks_censor_marked(void);
char *ks_censor_title(unsigned long toplevel);
/* Rectangles to black out, in root coordinates. Thread-safe copy. */
KsRect *ks_censor_rects(int *n);
/* logical rectangles -> the physical framebuffer on confined outputs; frees rs */
KsRect *ks_censor_to_physical(KsRect *rs, int *n, const XisScaleMap *m);
void ks_censor_paint(GdkPixbuf *pb, const KsRect *origin);
void ks_censor_paint_scaled(GdkPixbuf *pb, const KsRect *origin, double scale);
/* Lets the user click a window (xkill-style); returns its toplevel or 0. */
unsigned long ks_censor_pick(void);

/* ---- the compositor's capture protocol (ks_capture.c) --------------------- */

#define KS_CAP_MAX_OUT 8

/* What the compositor said about the outputs under an area, before a copy
 * of it. `ok` means the compositor can make everything under the area
 * censored: an output with nothing hidden showing is the screen as it is
 * (read it as always, then check it with ks_capture_unchanged()), and an
 * output where something hidden shows is read from a slot of the
 * compositor's censored picture (ks_capture_fetch()). Anything else -- no
 * compositor, no offer, no session, UNAVAILABLE -- is not ok, and the caller censors with its own
 * rectangles as it always has. */
typedef struct {
    gboolean ok;
    int n;                              /* outputs under the area */
    int out[KS_CAP_MAX_OUT];            /* their index in the session */
    uint32_t epoch[KS_CAP_MAX_OUT];
    gboolean via_slot[KS_CAP_MAX_OUT];  /* something hidden shows: read a slot */
    KsRect orect[KS_CAP_MAX_OUT];       /* the output, in root coordinates */
    int n_slot;
} KsCapView;

/* A slot of the compositor's picture of one output, held for us. */
typedef struct {
    int out, slot;
    xcb_pixmap_t pixmap;
    KsRect orect;
} KsCapSlot;

/* Main thread: opens (or keeps) a session when the compositor offers one.
 * Balanced by ks_capture_release(), which closes it a few seconds later. */
gboolean ks_capture_acquire(void);
void ks_capture_release(void);
/* The seqlock, over the connection that does the copy -- the server handles
 * one connection's requests in order, which is what makes it sound:
 *     view(c, area, &v);  ...copy the screen over c...;  [fetch + copy the slots];
 *     if (ok && unchanged(c, &v) && slots read) skip own censoring */
void ks_capture_view(xcb_connection_t *c, const KsRect *area, KsCapView *v);
gboolean ks_capture_unchanged(xcb_connection_t *c, const KsCapView *v);
/* Asks for the picture of each output that needs one and waits (a short
 * deadline) for the slots. FALSE if any could not be had -- nothing is held
 * then. Otherwise the caller reads them with one of the copies below and
 * gives them back with ks_capture_return() *after a round trip* following
 * its last read. */
gboolean ks_capture_fetch(const KsCapView *v, KsCapSlot *slots, int *n);
void ks_capture_return(const KsCapSlot *slots, int n);
/* The copies: the slots' pixels for the part of `area` each covers, onto
 * a pixmap (xcb, then Xlib), into a 4-byte-per-pixel buffer, or into a pixbuf. */
void ks_capture_copy_xcb(xcb_connection_t *c, const KsCapSlot *s, int n, const KsRect *area,
                         xcb_drawable_t dst, xcb_gcontext_t gc);
void ks_capture_copy_xlib(void *display, const KsCapSlot *s, int n, const KsRect *area,
                          unsigned long dst, void *gc);
gboolean ks_capture_copy_buffer(xcb_connection_t *c, const KsCapSlot *s, int n, const KsRect *area,
                                uint8_t *buf, int stride);
gboolean ks_capture_copy_pixbuf(const KsCapSlot *s, int n, const KsRect *area, GdkPixbuf *pb);
/* with KISHOT_DEBUG set, says on stderr which way a frame was censored */
void ks_capture_note(const KsCapView *v, gboolean censored, gboolean slots);
/* GDK's own X connection, for the copies GDK/Xlib make */
xcb_connection_t *ks_capture_gdk_connection(void);
/* True when a session is open and the compositor can render every output
 * censored with kishot's own windows left out of the pictures (HIDE_PID): the
 * main window then need not be hidden around a capture. */
gboolean ks_capture_hides_own_windows(void);
/* Does the compositor offer the protocol; its censor mode (0 hide, 1 fill) */
gboolean ks_capture_offered(int *mode, uint32_t *color);
/* One line for the main window; g_free() it */
char *ks_capture_status(void);

/* ---- region selection (ks_region.c) -------------------------------------- */

typedef void (*KsRegionDone)(gboolean ok, const KsRect *r, void *data);
void ks_region_select(GdkPixbuf *frozen, KsRegionDone done, void *data);

/* ---- recording (ks_rec.c) ------------------------------------------------ */

typedef struct {
    gboolean usable;            /* some way to record exists */
    gboolean plugin;            /* kishot-rec-av.so loaded */
    gboolean cli;               /* ffmpeg in PATH */
    gboolean audio;             /* libpulse-simple loaded */
    gboolean vaapi;             /* plugin opened a VAAPI device */
    char *why_not;              /* reason when !usable */
    GPtrArray *encoders;        /* names usable with the chosen backend */
} KsRecCaps;

const KsRecCaps *ks_rec_probe(const KsConfig *cfg);
typedef void (*KsRecFinished)(const char *path, const char *error, void *data);
gboolean ks_rec_start(const KsConfig *cfg, const KsRect *area, const char *path,
                      KsRecFinished done, void *data, char **error);
void ks_rec_stop(void);
gboolean ks_rec_active(void);
/* human-readable "how this recording is captured/encoded" */
const char *ks_rec_method(void);
double ks_rec_elapsed(void);

/* ---- audio (ks_audio.c) -------------------------------------------------- */

#define KS_AUDIO_RATE 48000
#define KS_AUDIO_CHANNELS 2
typedef void (*KsAudioSink)(const int16_t *pcm, int frames, void *data);
gboolean ks_audio_available(void);
gboolean ks_audio_start(gboolean system, gboolean mic, KsAudioSink sink, void *data);
void ks_audio_stop(void);

/* ---- main window (ks_window.c) ------------------------------------------- */

typedef struct KsApp KsApp;
KsApp *ks_app_new(KsConfig *cfg);
/* What a launch (or a second instance) asks for. */
typedef enum { KS_CMD_SHOT, KS_CMD_RECORD } KsCmdKind;
void ks_app_command(KsApp *app, KsCmdKind kind, int area /* -1 = last */, gboolean background);
void ks_prefs_run(GtkWindow *parent, KsConfig *cfg, KsTab page);

#endif /* KS_H */

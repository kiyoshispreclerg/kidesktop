/* ks.h - kishot: screenshots and screen recording for KiDesktop. */
#ifndef KS_H
#define KS_H

#include <gtk/gtk.h>
#include <stdint.h>

#define KISHOT_VERSION "0.1.4"

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
    char *name_pattern;         /* strftime() pattern without extension */

    char *video_dir;
    char *container;            /* mkv, mp4, webm */
    char *vcodec;               /* encoder name or "auto" */
    int quality;                /* 0 (best) .. 51, CRF/QP-like */
    int fps;
    gboolean rec_pointer;
    gboolean audio_system;
    gboolean audio_mic;
    gboolean rec_preview;       /* live preview in the Recording tab */
    KsCopyMode copy_mode;
    KsBackend backend;

    char **censor_classes;      /* WM_CLASS names always blacked out */
    gboolean censor_popups;     /* also menus/tooltips of a censored app */

    int win_w, win_h;
    int pane_pos;               /* preview | options divider, 0 = default */
} KsConfig;

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
int ks_monitor_count(void);
/* connector name of monitor i ("DP-1"), or "Monitor N"; g_free() it */
char *ks_monitor_name(int i);
/* geometry of the monitor with that name; FALSE when it is not connected */
gboolean ks_monitor_rect_by_name(const char *name, KsRect *out);

/* ---- censoring (ks_censor.c) --------------------------------------------- */

void ks_censor_init(KsConfig *cfg, void (*changed)(void *), void *data);
void ks_censor_add(unsigned long toplevel);
void ks_censor_remove(unsigned long toplevel);
void ks_censor_clear(void);
/* toplevel windows picked by hand (not by class), for the list in the UI */
GArray *ks_censor_picked(void);
char *ks_censor_title(unsigned long toplevel);
void ks_censor_set_classes(char **classes);
/* Rectangles to black out, in root coordinates. Thread-safe copy. */
KsRect *ks_censor_rects(int *n);
void ks_censor_paint(GdkPixbuf *pb, const KsRect *origin);
void ks_censor_paint_scaled(GdkPixbuf *pb, const KsRect *origin, double scale);
/* Lets the user click a window (xkill-style); returns its toplevel or 0. */
unsigned long ks_censor_pick(void);

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

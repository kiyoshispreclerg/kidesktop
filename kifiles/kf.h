/* kf.h - shared declarations between the kifiles source files. */
#ifndef KF_H
#define KF_H

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <gio/gio.h>
#include "../shared/xis_sound.h"

#define KIFILES_VERSION "0.1.20"

typedef enum { KF_VIEW_ICONS, KF_VIEW_LIST } KfViewMode;
typedef enum { KF_SORT_NAME, KF_SORT_SIZE, KF_SORT_MTIME, KF_SORT_TYPE, KF_SORT_EXT, KF_SORT_N } KfSortKey;

typedef struct {
    int view;                   /* KfViewMode */
    int zoom;                   /* icon size in px, 16..256 */
    int sort;                   /* KfSortKey */
    gboolean sort_desc;
    gboolean folders_first;
    gboolean show_hidden;
    gboolean thumbnails;
    gboolean split;
    gboolean show_places;
    gboolean hover;             /* highlight the item under the pointer */
    gboolean drop_menu;         /* ask Move/Copy/Link/Cancel after a drop instead of acting at once */
    int win_w, win_h, places_w;
    char **tabs;                /* saved session, NULL-terminated: per tab "URI" or "URI URI" (split view) */
    int cur_tab;                /* index into tabs of the tab that was shown */
} KfConfig;

#define KF_ZOOM_MIN 16
#define KF_ZOOM_MAX 256

/* Columns of a pane's GtkListStore. */
enum {
    KF_COL_FILE,        /* GFile* */
    KF_COL_NAME,        /* display name */
    KF_COL_ICON,        /* GdkPixbuf shown (themed icon or thumbnail at the shown size) */
    KF_COL_SIZE,        /* guint64 */
    KF_COL_MTIME,       /* gint64 */
    KF_COL_TYPEDESC,    /* "JPEG image" */
    KF_COL_ISDIR,
    KF_COL_HIDDEN,
    KF_COL_COLLATE,     /* g_utf8_collate_key_for_filename(name) */
    KF_COL_CTYPE,       /* content type */
    KF_COL_ICONSTR,     /* g_icon_to_string() */
    KF_COL_EXT,         /* lowercase extension, "" if none */
    KF_COL_THSTATE,     /* KfThumbState */
    KF_N_COLS
};

/* Per-row thumbnail state. Only the scaled copy in KF_COL_ICON is kept, so
 * a zoom change or an evicted row goes back to KF_TH_NONE and asks again
 * (a disk-cache hit). */
enum { KF_TH_NONE, KF_TH_PENDING, KF_TH_NOTHUMB, KF_TH_SHOWN };

typedef struct KfApp KfApp;
typedef struct KfPane KfPane;
typedef struct KfTab KfTab;

struct KfPane {
    KfApp *app;
    KfTab *tab;
    GtkWidget *box;             /* the pane's top-level widget */
    GtkWidget *crumbs;
    GtkWidget *active_bar;      /* thin strip marking the active pane in split view */
    GtkWidget *filter_bar, *filter_entry;
    GtkWidget *msg_bar, *msg_label;
    GtkWidget *sw_icons, *sw_list;
    GtkWidget *iconview, *treeview;
    GtkCellRenderer *icon_text_r, *icon_pix_r, *list_pix_r;

    GFile *dir;
    GList *back, *fwd;          /* GFile* */
    GtkListStore *store;
    GtkTreeModel *filter;       /* GtkTreeModelFilter */
    GtkTreeModel *sort;         /* GtkTreeModelSort */
    GHashTable *rows;           /* basename -> GtkTreeRowReference* (in store) */
    GHashTable *typedescs;      /* content type -> description */
    GCancellable *cancel;
    GFileMonitor *monitor;
    gboolean loading;
    char *filter_text;
    GList *pending_select;      /* basenames to select once they show up */
    guint thumbs_idle;
    guint stats_idle;
    guint sel_idle;
    int view;                   /* KfViewMode currently shown */
    GdkRectangle hover_rect;    /* item under the pointer, in view bin-window coordinates */
    gboolean hover_has;
    /* icon view's layout as GTK computes it (kf_hover.c), redone when !hl_valid */
    gboolean hl_valid;
    int hl_cols, hl_x0, hl_pitch, hl_item_w;
    GArray *hl_rows;            /* GdkRectangle per row: y and height */
};

/* A tab owns one or two panes (split view); the toolbar and places panel are
 * shared and follow the current tab (kf_tabs.c). */
struct KfTab {
    KfApp *app;
    GtkWidget *page;            /* notebook page; holds app->body while current */
    GtkWidget *panes_paned;     /* in app->panes_box, shown only while current */
    GtkWidget *label;
    KfPane *panes[2];
    KfPane *active;             /* the active pane, kept while in the background */
    gboolean center_split;      /* keep the split at half of panes_paned until the user drags it */
};

struct KfApp {
    KfConfig cfg;
    GtkWidget *win;
    GtkUIManager *ui;
    GtkActionGroup *actions;
    GtkWidget *notebook;        /* the tabs */
    GtkWidget *body;            /* toolbar + places + panes, moved into the current page */
    GtkWidget *main_paned, *places, *panes_box;
    GtkWidget *status_left, *status_free, *free_bar, *zoom_scale;
    GtkWidget *view_toggle_icons, *view_toggle_list;
    KfTab *tab;                 /* current tab */
    KfPane *active;             /* active pane of the current tab */
    GQueue *undo;               /* KfUndo* (kf_ops.c) */
    GtkWidget *op_box;          /* running-operations area of the status bar */
    GList *jobs;                /* running operations (kf_ops.c) */
    gboolean quitting;          /* window closed, waiting for the jobs to finish */
    gboolean updating_ui;       /* suppress action callbacks while syncing */
};

/* kf_config.c */
void kf_config_defaults(KfConfig *c);
void kf_config_load(KfConfig *c);
void kf_config_save(const KfConfig *c);

/* kf_icons.c */
GdkPixbuf *kf_icon_lookup(const char *icon_str, int size);
GdkPixbuf *kf_icon_lookup_gicon(GIcon *icon, int size);
GdkPixbuf *kf_icon_fit(GdkPixbuf *src, int size);       /* new ref */

/* kf_crumbs.c */
GtkWidget *kf_crumbs_new(KfPane *p);
void kf_crumbs_set_location(GtkWidget *crumbs, GFile *dir);
void kf_crumbs_edit(GtkWidget *crumbs);

/* kf_pane.c */
KfPane *kf_pane_new(KfApp *app);
void kf_pane_destroy(KfPane *p);
void kf_pane_navigate(KfPane *p, GFile *dir, gboolean push_history, GFile *select_after);
void kf_pane_go_back(KfPane *p);
void kf_pane_go_forward(KfPane *p);
void kf_pane_go_up(KfPane *p);
gboolean kf_pane_can_back(KfPane *p);
gboolean kf_pane_can_forward(KfPane *p);
gboolean kf_pane_can_up(KfPane *p);
void kf_pane_reload(KfPane *p);
void kf_pane_set_view(KfPane *p, KfViewMode m);
void kf_pane_apply_sort(KfPane *p);
void kf_pane_apply_filter(KfPane *p);
void kf_pane_apply_zoom(KfPane *p);
GList *kf_pane_selected(KfPane *p);             /* GList<GFile*>, free with kf_files_free() */
guint kf_pane_selection_count(KfPane *p);
void kf_pane_select_all(KfPane *p);
void kf_pane_select_none(KfPane *p);
void kf_pane_select_invert(KfPane *p);
void kf_pane_select_names(KfPane *p, GList *basenames);   /* those present now */
void kf_pane_select_when_added(KfPane *p, const char *name);   /* now, or when it shows up */
void kf_pane_show_filter(KfPane *p, gboolean show);
void kf_pane_focus_view(KfPane *p);
void kf_pane_set_active(KfPane *p, gboolean active, gboolean show_marker);
void kf_pane_stats(KfPane *p, gboolean selection_only, guint *folders, guint *files, guint64 *bytes);
gboolean kf_pane_file_info(KfPane *p, GFile *f, gboolean *is_dir, char **ctype);
void kf_pane_request_thumbs(KfPane *p);
void kf_pane_drop_thumbs(KfPane *p);       /* thumbnails turned off */

void kf_files_free(GList *files);

/* kf_hover.c -- pointer-over highlight (GTK2 views have none of their own) */
void kf_hover_setup(KfPane *p);
void kf_hover_apply(KfPane *p);         /* after the setting changed */
void kf_hover_clear(KfPane *p);
void kf_hover_refresh(KfPane *p);       /* the content moved under a still pointer */

/* kf_dnd.c */
void kf_dnd_setup_pane(KfPane *p);
/* Handles dropped text/uri-list data: copies or moves into `dest` according to
 * the action the drag settled on. Returns whether anything was dropped. */
gboolean kf_dnd_drop_uris(KfApp *app, GtkSelectionData *sel, GdkDragContext *ctx, GFile *dest);
/* Default action for dragging our own files onto `dest`: move on the same
 * file system, copy across file systems. */
GdkDragAction kf_dnd_default_action(KfApp *app, GFile *dest, GdkDragContext *ctx);

/* kf_clip.c */
void kf_clip_set(KfApp *app, GList *files, gboolean cut);
void kf_clip_paste(KfApp *app, GFile *dest_dir);

/* kf_ops.c -- every operation reports problems itself (dialogs). */
void kf_ops_transfer(KfApp *app, GList *srcs, GFile *dest_dir, gboolean move);   /* copy or move */
void kf_ops_trash(KfApp *app, GList *files);
void kf_ops_delete(KfApp *app, GList *files);           /* asks first */
void kf_ops_empty_trash(KfApp *app);
void kf_ops_restore(KfApp *app, GList *files);          /* from trash:/// */
void kf_ops_rename(KfApp *app, GFile *f);
void kf_ops_new_folder(KfApp *app, GFile *dir);
void kf_ops_new_file(KfApp *app, GFile *dir);
void kf_ops_undo(KfApp *app);
gboolean kf_ops_can_undo(KfApp *app);
void kf_ops_init(KfApp *app);
void kf_ops_detach(KfApp *app);         /* the window is going away: unparent the job dialogs */

/* kf_jobview.c: a job's progress shown by the desktop's job server */
typedef struct KfJobView KfJobView;
typedef enum { KF_JOBVIEW_CANCEL, KF_JOBVIEW_SUSPEND, KF_JOBVIEW_RESUME } KfJobViewRequest;
typedef void (*KfJobViewSignal)(KfJobViewRequest what, gpointer data);
KfJobView *kf_jobview_new(const char *title, KfJobViewSignal on_signal, gpointer data);  /* NULL: no server */
void kf_jobview_update(KfJobView *v, const char *info, int percent, guint64 done, guint64 total, gboolean bytes,
                       gboolean suspended, const char *source, const char *dest);
gboolean kf_jobview_lost(KfJobView *v);         /* the server went away */
void kf_jobview_finish(KfJobView *v, guint error, const char *message);    /* also frees */
void kf_jobview_free(KfJobView *v);

/* kf_thumbs.c */
typedef void (*KfThumbDone)(GFile *file, GdkPixbuf *pb, gpointer data);
void kf_thumbs_init(void);
void kf_thumbs_trim_soon(void);          /* return freed memory once no request is pending */
gboolean kf_thumbs_supported(const char *ctype);
/* cb runs on the main loop unless `cancel` was cancelled first; pb is NULL when
 * no thumbnail could be made. */
void kf_thumbs_request(GFile *file, const char *ctype, gint64 mtime, int px, GCancellable *cancel,
                       KfThumbDone cb, gpointer data);

/* kf_places.c */
GtkWidget *kf_places_new(KfApp *app);
void kf_places_set_current(GtkWidget *places, GFile *dir);   /* highlight the matching entry */
void kf_places_add_bookmark(GtkWidget *places, GFile *dir);
/* kf_usagecell.c -- text cell with a usage bar under it ("usage": 0..100, -1 none) */
GtkCellRenderer *kf_usage_cell_new(void);

/* kf_props.c -- Properties dialog: General text + Permissions tab (native files only) */
void kf_props_show(KfApp *app, const char *general_text, GList *files, gboolean permissions);

/* kf_tabs.c */
void kf_tabs_setup(KfApp *app);                     /* creates app->notebook */
KfTab *kf_tabs_open(KfApp *app, GFile *dir, gboolean switch_to);    /* after the current tab */
void kf_tabs_close(KfTab *t);                       /* the last one closes the window */
void kf_tabs_close_others(KfTab *keep);
void kf_tabs_detach(KfTab *t);                      /* into a new window */
void kf_tabs_step(KfApp *app, int dir);             /* +1 next, -1 previous, wrapping */
void kf_tabs_update_label(KfTab *t);
int kf_tabs_count(KfApp *app);
GList *kf_tabs_all(KfApp *app);                     /* KfTab*, in order; g_list_free() */

/* kifiles.c (called by panes and the other modules) */
typedef enum { KF_CHANGE_DIR, KF_CHANGE_SELECTION, KF_CHANGE_CONTENT, KF_CHANGE_FOCUS, KF_CHANGE_LOADED } KfChange;
void kf_app_pane_changed(KfApp *app, KfPane *p, KfChange what);
void kf_app_navigate(KfApp *app, GFile *dir);               /* in the active pane */
void kf_app_open(KfApp *app, KfPane *p, GFile *f);
void kf_app_popup_context(KfApp *app, KfPane *p, GdkEventButton *ev, gboolean on_item);
void kf_app_message(KfApp *app, GtkMessageType type, const char *primary, const char *secondary);
GFile *kf_app_dir(KfApp *app);                              /* active pane's folder */
void kf_app_zoom_step(KfApp *app, int dir);                /* +1 bigger icons, -1 smaller */
void kf_app_sort_by(KfApp *app, KfSortKey key);            /* same key flips the direction */
void kf_app_status_free(KfApp *app, guint64 free_b, guint64 total_b);
GList *kf_app_panes(KfApp *app);                            /* every tab's panes; g_list_free() */
void kf_app_setup_pane(KfApp *app, KfPane *p);              /* view/sort from the settings */
void kf_app_tab_switched(KfApp *app);                       /* app->tab changed */
void kf_app_open_window(GFile *dir);                        /* a new kifiles process */
char *kf_app_dir_name(GFile *dir);                          /* display name for titles */
void kf_app_quit(KfApp *app);                               /* waits for running jobs */
GtkWindow *kf_app_parent(KfApp *app);                       /* for dialogs; NULL once closed */

#endif

/* ks_window.c - the kishot window and what its commands do.
 *
 * Like Spectacle: launching takes a shot of the area used last time and
 * shows it here, with Save / Copy / New screenshot. The options are in two
 * tabs, Screenshot and Recording, each with its own area; the Recording tab
 * can show a live preview of what would be recorded. While a recording runs
 * the window hides itself (so it is not in the video) and a tray icon stops
 * it -- as does running `kishot --record` again, which is what the hotkey
 * does. Whatever a launch asks for picks the tab the window shows. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <gdk/gdkx.h>
#include <string.h>
#include <time.h>

struct KsApp {
    KsConfig *cfg;
    GtkWidget *win;
    GtkWidget *paned, *notebook;
    GtkWidget *preview, *preview_hint;
    GtkWidget *area_combo, *delay_spin, *pointer_check, *deco_check;
    GtkWidget *rec_area_combo, *rec_deco_check, *rec_pointer_check;
    GtkWidget *sys_check, *mic_check, *rec_preview_check;
    GtkWidget *rec_button, *save_button, *saveas_button, *copy_button;
    GtkWidget *status;
    GtkListStore *censor_store;
    GtkWidget *censor_view;
    GtkWidget *censor_note;
    GtkStatusIcon *tray;
    GdkPixbuf *shot;
    char *shot_path;            /* where the current shot was saved, if it was */
    char *last_dir;             /* folder of the last saved screenshot */
    char *last_video_dir;       /* ... and recording */
    KsTab tab;                  /* the tab shown */
    gboolean background;        /* launched with -b: no window, quit when done */
    gboolean busy;              /* a shot is being taken */
    guint rec_timer;
    guint live_timer;           /* live recording preview */
};

static void fill_area_combo(KsApp *app, gboolean record);
static void on_area_changed(GtkComboBox *c, KsApp *app);
static void on_rec_area_changed(GtkComboBox *c, KsApp *app);

static void set_status(KsApp *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text ? text : "");
}

/* ---- preview ------------------------------------------------------------- */

/* ks_area_rect() plus a named monitor, which falls back to the one under the
 * pointer when it is not connected any more */
static void area_rect(KsArea area, const char *output, gboolean deco, unsigned long except, KsRect *r)
{
    if (area == KS_AREA_OUTPUT) {
        if (ks_monitor_rect_by_name(output, r))
            return;
        area = KS_AREA_MONITOR;
    }
    ks_area_rect_except(area, deco, except, r);
}

/* the space the preview has, minus a small margin */
static void preview_room(KsApp *app, int *mw, int *mh)
{
    GtkAllocation a;
    gtk_widget_get_allocation(app->preview, &a);
    *mw = MAX(a.width - 8, 32);
    *mh = MAX(a.height - 8, 32);
}

static void live_frame(KsApp *app)
{
    GdkWindow *gw = gtk_widget_get_window(app->win);
    if (!gw || (gdk_window_get_state(gw) & GDK_WINDOW_STATE_ICONIFIED))
        return;
    KsRect r;
    /* "active window" while kishot itself is active falls back to the monitor */
    area_rect(app->cfg->rec_area, app->cfg->rec_output, app->cfg->decorations, GDK_WINDOW_XID(gw), &r);
    int mw, mh;
    preview_room(app, &mw, &mh);
    double sc = MIN(1.0, MIN((double)mw / r.w, (double)mh / r.h));
    GdkPixbuf *pb = ks_grab_scaled(&r, MAX(1, (int)(r.w * sc)), MAX(1, (int)(r.h * sc)), app->cfg->rec_pointer);
    if (!pb)
        return;
    gtk_image_set_from_pixbuf(GTK_IMAGE(app->preview), pb);
    g_object_unref(pb);
}

static gboolean live_tick(gpointer data)
{
    live_frame(data);
    return TRUE;
}

/* The live preview runs only while it can be seen and nothing is captured. */
static void live_sync(KsApp *app)
{
    gboolean want = app->tab == KS_TAB_RECORD && app->cfg->rec_preview && !app->busy &&
                    !ks_rec_active() && gtk_widget_get_mapped(app->win);
    if (want && !app->live_timer) {
        app->live_timer = g_timeout_add(66, live_tick, app);
        live_frame(app);
    } else if (!want && app->live_timer) {
        g_source_remove(app->live_timer);
        app->live_timer = 0;
    }
}

static void update_hint(KsApp *app)
{
    const char *hint = NULL;
    if (app->tab == KS_TAB_RECORD && app->cfg->rec_preview) {
        if (app->cfg->rec_area == KS_AREA_REGION)
            hint = _("The region is selected when the recording starts.");
        else if (app->cfg->rec_area == KS_AREA_WINDOW)
            hint = _("The window active when the recording starts is recorded.");
    }
    gtk_label_set_text(GTK_LABEL(app->preview_hint), hint ? hint : "");
    gtk_widget_set_visible(app->preview_hint, hint != NULL);
}

static void update_preview(KsApp *app)
{
    update_hint(app);
    if (app->tab == KS_TAB_RECORD) {
        if (!app->cfg->rec_preview)
            gtk_image_set_from_icon_name(GTK_IMAGE(app->preview), "camera-video", GTK_ICON_SIZE_DIALOG);
        else if (app->live_timer)
            live_frame(app);
        live_sync(app);
        return;
    }
    live_sync(app);
    if (!app->shot) {
        gtk_image_set_from_icon_name(GTK_IMAGE(app->preview), "camera-photo", GTK_ICON_SIZE_DIALOG);
        return;
    }
    int W = gdk_pixbuf_get_width(app->shot), H = gdk_pixbuf_get_height(app->shot);
    int mw, mh;
    preview_room(app, &mw, &mh);
    double s = MIN(1.0, MIN((double)mw / W, (double)mh / H));
    GdkPixbuf *scaled = gdk_pixbuf_scale_simple(app->shot, MAX(1, (int)(W * s)), MAX(1, (int)(H * s)),
                                                GDK_INTERP_BILINEAR);
    gtk_image_set_from_pixbuf(GTK_IMAGE(app->preview), scaled);
    g_object_unref(scaled);
}

static void on_preview_alloc(GtkWidget *w, GtkAllocation *a, KsApp *app)
{
    (void)w;
    static int lw, lh;
    if (a->width == lw && a->height == lh)
        return;
    lw = a->width;
    lh = a->height;
    update_preview(app);
}

static void update_sensitivity(KsApp *app)
{
    gboolean have = app->shot != NULL;
    /* Save / Copy are about the screenshot */
    gboolean shot_tab = app->tab == KS_TAB_SHOT;
    gtk_widget_set_visible(app->save_button, shot_tab);
    gtk_widget_set_visible(app->saveas_button, shot_tab);
    gtk_widget_set_visible(app->copy_button, shot_tab);
    gtk_widget_set_sensitive(app->save_button, have);
    gtk_widget_set_sensitive(app->saveas_button, have);
    gtk_widget_set_sensitive(app->copy_button, have);
    gtk_widget_set_sensitive(app->deco_check, app->cfg->area == KS_AREA_WINDOW);
    gtk_widget_set_sensitive(app->rec_deco_check, app->cfg->rec_area == KS_AREA_WINDOW);
}

/* ---- saving -------------------------------------------------------------- */

static const char *format_ext(const char *fmt)
{
    return !strcmp(fmt, "jpeg") ? "jpg" : !strcmp(fmt, "tiff") ? "tif" : fmt;
}

static char *unique_path(const char *dir_cfg, const char *pattern, const char *ext)
{
    char *dir = ks_config_expand_dir(dir_cfg);
    g_mkdir_with_parents(dir, 0755);
    char name[256];
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    if (!strftime(name, sizeof(name), pattern, &tm))
        g_strlcpy(name, "kishot", sizeof(name));
    char *path = NULL;
    for (int i = 0; i < 1000; i++) {
        char *base = i ? g_strdup_printf("%s_%d.%s", name, i, ext) : g_strdup_printf("%s.%s", name, ext);
        g_free(path);
        path = g_build_filename(dir, base, NULL);
        g_free(base);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            break;
    }
    g_free(dir);
    return path;
}

static gboolean save_to(KsApp *app, const char *path, const char *fmt, GError **err)
{
    gboolean ok;
    if (!strcmp(fmt, "jpeg")) {
        char q[8];
        g_snprintf(q, sizeof(q), "%d", app->cfg->jpeg_quality);
        /* jpeg has no alpha */
        GdkPixbuf *rgb = gdk_pixbuf_get_has_alpha(app->shot)
            ? gdk_pixbuf_composite_color_simple(app->shot, gdk_pixbuf_get_width(app->shot),
                                                gdk_pixbuf_get_height(app->shot), GDK_INTERP_NEAREST,
                                                255, 8, 0xffffff, 0xffffff)
            : g_object_ref(app->shot);
        ok = gdk_pixbuf_save(rgb, path, fmt, err, "quality", q, NULL);
        g_object_unref(rgb);
    } else {
        ok = gdk_pixbuf_save(app->shot, path, fmt, err, NULL);
    }
    if (ok) {
        g_free(app->shot_path);
        app->shot_path = g_strdup(path);
        g_free(app->last_dir);
        app->last_dir = g_path_get_dirname(path);
    }
    return ok;
}

static void notify(const char *summary, const char *body, const char *icon)
{
    char *prog = g_find_program_in_path("notify-send");
    if (!prog)
        return;
    char *argv[] = { prog, "-a", "kishot", "-i", (char *)icon, (char *)summary, (char *)body, NULL };
    g_spawn_async(NULL, argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                  NULL, NULL, NULL, NULL);
    g_free(prog);
}

static gboolean save_auto(KsApp *app)
{
    if (!app->shot)
        return FALSE;
    char *path = unique_path(app->cfg->shot_dir, app->cfg->name_pattern, format_ext(app->cfg->shot_format));
    GError *err = NULL;
    gboolean ok = save_to(app, path, app->cfg->shot_format, &err);
    char *msg = ok ? g_strdup_printf(_("Saved to %s"), path)
                   : g_strdup_printf(_("Could not save: %s"), err->message);
    set_status(app, msg);
    if (app->background)
        notify(ok ? _("Screenshot saved") : _("Screenshot failed"), ok ? path : err->message,
               ok ? "camera-photo" : "dialog-error");
    if (err)
        g_error_free(err);
    g_free(msg);
    g_free(path);
    return ok;
}

static void on_save(GtkWidget *w, KsApp *app)
{
    (void)w;
    save_auto(app);
}

static void on_save_as(GtkWidget *w, KsApp *app)
{
    (void)w;
    GtkWidget *dlg = gtk_file_chooser_dialog_new(_("Save Screenshot"), GTK_WINDOW(app->win),
                                                 GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                 GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    char *suggest = unique_path(app->cfg->shot_dir, app->cfg->name_pattern, format_ext(app->cfg->shot_format));
    char *dir = g_path_get_dirname(suggest), *base = g_path_get_basename(suggest);
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dlg), app->last_dir ? app->last_dir : dir);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), base);
    g_free(suggest);
    g_free(dir);
    g_free(base);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        /* the format follows the extension the user typed */
        const char *dot = strrchr(path, '.');
        const char *fmt = app->cfg->shot_format;
        if (dot) {
            if (!g_ascii_strcasecmp(dot, ".jpg") || !g_ascii_strcasecmp(dot, ".jpeg")) fmt = "jpeg";
            else if (!g_ascii_strcasecmp(dot, ".png")) fmt = "png";
            else if (!g_ascii_strcasecmp(dot, ".bmp")) fmt = "bmp";
            else if (!g_ascii_strcasecmp(dot, ".tif") || !g_ascii_strcasecmp(dot, ".tiff")) fmt = "tiff";
            else if (!g_ascii_strcasecmp(dot, ".webp")) fmt = "webp";
        }
        GError *err = NULL;
        char *msg = save_to(app, path, fmt, &err) ? g_strdup_printf(_("Saved to %s"), path)
                                                  : g_strdup_printf(_("Could not save: %s"), err->message);
        set_status(app, msg);
        g_free(msg);
        if (err)
            g_error_free(err);
        g_free(path);
    }
    gtk_widget_destroy(dlg);
}

static void on_copy(GtkWidget *w, KsApp *app)
{
    (void)w;
    if (!app->shot)
        return;
    gtk_clipboard_set_image(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), app->shot);
    set_status(app, _("Copied to the clipboard"));
}

static void on_open_folder(GtkWidget *w, KsApp *app)
{
    (void)w;
    char *dir;
    if (app->tab == KS_TAB_RECORD)
        dir = app->last_video_dir ? g_strdup(app->last_video_dir) : ks_config_expand_dir(app->cfg->video_dir);
    else
        dir = app->last_dir ? g_strdup(app->last_dir) : ks_config_expand_dir(app->cfg->shot_dir);
    g_mkdir_with_parents(dir, 0755);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri)
        gtk_show_uri(NULL, uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
    g_free(dir);
}

/* ---- taking a shot ------------------------------------------------------- */

typedef struct { KsApp *app; KsArea area; char *output; gboolean record; GdkPixbuf *frozen; } Pending;

static void shot_ready(KsApp *app, GdkPixbuf *pb)
{
    app->busy = FALSE;
    if (app->shot)
        g_object_unref(app->shot);
    app->shot = pb;
    g_free(app->shot_path);
    app->shot_path = NULL;
    set_status(app, pb ? NULL : _("The screen could not be read"));
    if (pb && app->cfg->auto_copy)
        on_copy(NULL, app);
    if (pb && (app->cfg->auto_save || app->background))
        save_auto(app);
    if (app->background) {
        /* a recording in progress keeps the process (its end quits it);
         * otherwise the clipboard owner has to outlive the copy a little,
         * for a clipboard manager (kimemory) to take it */
        if (!ks_rec_active())
            g_timeout_add(app->cfg->auto_copy ? 1500 : 0, (GSourceFunc)gtk_main_quit, NULL);
        return;
    }
    update_preview(app);
    update_sensitivity(app);
    gtk_window_present(GTK_WINDOW(app->win));
}

static void start_recording(KsApp *app, const KsRect *r);

static void region_done(gboolean ok, const KsRect *r, void *data)
{
    Pending *p = data;
    KsApp *app = p->app;
    KsRect rr = *r;
    if (!ok || !ks_rect_clip_to_screen(&rr)) {
        app->busy = FALSE;
        live_sync(app);
        if (app->background && !ks_rec_active())
            gtk_main_quit();
        else if (!ks_rec_active())
            gtk_window_present(GTK_WINDOW(app->win));
    } else if (p->record) {
        app->busy = FALSE;
        start_recording(app, &rr);
    } else {
        /* crop the frozen frame: it is what the user saw while selecting */
        GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(p->frozen, rr.x, rr.y, rr.w, rr.h);
        shot_ready(app, gdk_pixbuf_copy(sub));
        g_object_unref(sub);
    }
    g_object_unref(p->frozen);
    g_free(p->output);
    g_free(p);
}

static gboolean do_capture(gpointer data)
{
    Pending *p = data;
    KsApp *app = p->app;
    if (p->area == KS_AREA_REGION) {
        KsRect full;
        ks_area_rect(KS_AREA_FULL, FALSE, &full);
        GdkPixbuf *frozen = ks_grab_rect(&full, p->record ? FALSE : app->cfg->pointer);
        if (!frozen) {
            g_free(p->output);
            g_free(p);
            shot_ready(app, NULL);
            return FALSE;
        }
        p->frozen = frozen;
        ks_region_select(frozen, region_done, p);
        return FALSE;
    }
    KsRect r;
    area_rect(p->area, p->output, app->cfg->decorations, 0, &r);
    g_free(p->output);
    if (p->record) {
        app->busy = FALSE;
        start_recording(app, &r);
    } else {
        shot_ready(app, ks_grab_rect(&r, app->cfg->pointer));
    }
    g_free(p);
    return FALSE;
}

void ks_app_command(KsApp *app, KsCmdKind kind, int area, gboolean background)
{
    if (kind == KS_CMD_RECORD && ks_rec_active()) {
        ks_rec_stop();
        return;
    }
    if (app->busy)
        return;
    gboolean record = kind == KS_CMD_RECORD;
    /* the window comes back on the tab of what was asked for */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(app->notebook), record ? KS_TAB_RECORD : KS_TAB_SHOT);
    if (area >= 0 && area < KS_AREA_N) {
        *(record ? &app->cfg->rec_area : &app->cfg->area) = area;
        fill_area_combo(app, record);
        ks_config_save(app->cfg);
    }
    /* a second `kishot -b` while the window is open still shows it */
    app->background = background && !gtk_widget_get_visible(app->win);
    app->busy = TRUE;
    live_sync(app);
    gboolean was_visible = gtk_widget_get_visible(app->win);
    gtk_widget_hide(app->win);
    gdk_flush();
    Pending *p = g_new0(Pending, 1);
    p->app = app;
    p->area = record ? app->cfg->rec_area : app->cfg->area;
    p->output = g_strdup(record ? app->cfg->rec_output : app->cfg->output);
    p->record = record;
    /* give the compositor time to repaint without our window, plus the delay */
    int ms = (was_visible ? 300 : 0) + (p->record ? 0 : app->cfg->delay * 1000);
    if (ms)
        g_timeout_add(ms, do_capture, p);
    else
        g_idle_add(do_capture, p);
}

static void on_new_shot(GtkWidget *w, KsApp *app)
{
    (void)w;
    ks_app_command(app, KS_CMD_SHOT, -1, FALSE);
}

/* ---- recording ----------------------------------------------------------- */

static void refresh_rec_ui(KsApp *app)
{
    if (ks_rec_active()) {
        int s = (int)ks_rec_elapsed();
        char *label = g_strdup_printf(_("Stop (%d:%02d)"), s / 60, s % 60);
        gtk_button_set_label(GTK_BUTTON(app->rec_button), label);
        if (app->tray) {
            char *tip = g_strdup_printf(_("Recording %d:%02d -- click to stop\n%s"), s / 60, s % 60,
                                        ks_rec_method() ? ks_rec_method() : "");
            gtk_status_icon_set_tooltip_text(app->tray, tip);
            g_free(tip);
        }
        g_free(label);
    } else {
        gtk_button_set_label(GTK_BUTTON(app->rec_button), _("Record"));
    }
}

static gboolean rec_tick(gpointer data)
{
    refresh_rec_ui(data);
    return TRUE;
}

static void on_tray_activate(GtkStatusIcon *icon, KsApp *app)
{
    (void)icon;
    (void)app;
    ks_rec_stop();
}

static void rec_finished(const char *path, const char *error, void *data)
{
    KsApp *app = data;
    if (app->rec_timer)
        g_source_remove(app->rec_timer);
    app->rec_timer = 0;
    if (app->tray) {
        gtk_status_icon_set_visible(app->tray, FALSE);
        g_object_unref(app->tray);
        app->tray = NULL;
    }
    refresh_rec_ui(app);
    char *msg = path ? g_strdup_printf(_("Recording saved to %s"), path)
                     : g_strdup_printf(_("Recording failed: %s"), error ? error : "?");
    g_printerr("kishot: %s\n", msg);
    set_status(app, msg);
    notify(path ? _("Recording saved") : _("Recording failed"), path ? path : error,
           path ? "video-x-generic" : "dialog-error");
    g_free(msg);
    if (path) {
        g_free(app->last_video_dir);
        app->last_video_dir = g_path_get_dirname(path);
    }
    live_sync(app);
    if (app->background) {
        /* started with -b: no window, before or after */
        if (gtk_main_level())
            gtk_main_quit();
        return;
    }
    gtk_window_present(GTK_WINDOW(app->win));
}

static void start_recording(KsApp *app, const KsRect *r)
{
    const char *ext = app->cfg->container;
    char *path = unique_path(app->cfg->video_dir, app->cfg->name_pattern, ext);
    /* the pattern defaults to "Screenshot_..."; recordings read better as such */
    if (g_str_has_prefix(app->cfg->name_pattern, "Screenshot_")) {
        g_free(path);
        path = unique_path(app->cfg->video_dir, "Recording_%Y%m%d_%H%M%S", ext);
    }
    char *error = NULL;
    if (!ks_rec_start(app->cfg, r, path, rec_finished, app, &error)) {
        char *msg = g_strdup_printf(_("Recording failed: %s"), error ? error : "?");
        g_printerr("kishot: %s\n", msg);
        set_status(app, msg);
        notify(_("Recording failed"), error ? error : "?", "dialog-error");
        g_free(msg);
        g_free(error);
        g_free(path);
        live_sync(app);
        if (app->background)
            gtk_main_quit();
        else
            gtk_window_present(GTK_WINDOW(app->win));
        return;
    }
    g_free(path);
    g_printerr("kishot: recording %dx%d+%d+%d via %s\n", r->w, r->h, r->x, r->y, ks_rec_method());
    app->tray = gtk_status_icon_new_from_icon_name("media-record");
    g_signal_connect(app->tray, "activate", G_CALLBACK(on_tray_activate), app);
    gtk_status_icon_set_visible(app->tray, TRUE);
    app->rec_timer = g_timeout_add_seconds(1, rec_tick, app);
    refresh_rec_ui(app);
}

static void on_record(GtkWidget *w, KsApp *app)
{
    (void)w;
    ks_app_command(app, KS_CMD_RECORD, -1, FALSE);
}

/* ---- option widgets ------------------------------------------------------ */

/* The Area list: Full screen, Current monitor, one row per connected monitor,
 * Active window, Rectangular region. Combo row <-> (area, output name): */
static void combo_to_area(KsApp *app, int idx, KsArea *area, char **output)
{
    int n = ks_monitor_count();
    if (idx < 0)
        return;
    if (idx >= 2 && idx < 2 + n) {
        char *name = ks_monitor_name(idx - 2);
        g_free(*output);
        *output = name;
        *area = KS_AREA_OUTPUT;
    } else {
        *area = idx < 2 ? (KsArea)idx : (KsArea)(idx - n);
    }
    (void)app;
}

static void fill_area_combo(KsApp *app, gboolean record)
{
    GtkComboBox *c = GTK_COMBO_BOX(record ? app->rec_area_combo : app->area_combo);
    KsArea area = record ? app->cfg->rec_area : app->cfg->area;
    const char *output = record ? app->cfg->rec_output : app->cfg->output;
    int n = ks_monitor_count(), active = 0;
    GtkTreeModel *m = gtk_combo_box_get_model(c);
    /* rebuilding must not look like the user picking rows */
    g_signal_handlers_block_matched(c, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                    record ? (gpointer)on_rec_area_changed : (gpointer)on_area_changed, NULL);
    gtk_list_store_clear(GTK_LIST_STORE(m));
    gtk_combo_box_append_text(c, _("Full screen (all monitors)"));
    gtk_combo_box_append_text(c, _("Current monitor"));
    for (int i = 0; i < n; i++) {
        GdkRectangle g;
        gdk_screen_get_monitor_geometry(gdk_screen_get_default(), i, &g);
        char *name = ks_monitor_name(i);
        /* "Monitor N" is the stand-in for a monitor without a connector name */
        char *label = g_str_has_prefix(name, "Monitor ")
            ? g_strdup_printf(_("Monitor %d (%dx%d)"), i + 1, g.width, g.height)
            : g_strdup_printf(_("Monitor %d: %s (%dx%d)"), i + 1, name, g.width, g.height);
        gtk_combo_box_append_text(c, label);
        /* a config from before RandR names saved "Monitor N" */
        char *legacy = g_strdup_printf("Monitor %d", i + 1);
        if (area == KS_AREA_OUTPUT && output && (!strcmp(output, name) || !strcmp(output, legacy)))
            active = 2 + i;
        g_free(legacy);
        g_free(label);
        g_free(name);
    }
    gtk_combo_box_append_text(c, _("Active window"));
    gtk_combo_box_append_text(c, _("Rectangular region"));
    if (area == KS_AREA_WINDOW || area == KS_AREA_REGION)
        active = area + n;
    else if (area == KS_AREA_FULL || area == KS_AREA_MONITOR)
        active = area;
    /* a monitor that is not connected: shown as the current one, which is
     * what a capture falls back to */
    else if (!active)
        active = KS_AREA_MONITOR;
    gtk_combo_box_set_active(c, active);
    g_signal_handlers_unblock_matched(c, G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                      record ? (gpointer)on_rec_area_changed : (gpointer)on_area_changed, NULL);
}

static void on_monitors_changed(GdkScreen *scr, KsApp *app)
{
    (void)scr;
    fill_area_combo(app, FALSE);
    fill_area_combo(app, TRUE);
    update_preview(app);
}

static void on_area_changed(GtkComboBox *c, KsApp *app)
{
    combo_to_area(app, gtk_combo_box_get_active(c), &app->cfg->area, &app->cfg->output);
    update_sensitivity(app);
    ks_config_save(app->cfg);
}

static void on_rec_area_changed(GtkComboBox *c, KsApp *app)
{
    combo_to_area(app, gtk_combo_box_get_active(c), &app->cfg->rec_area, &app->cfg->rec_output);
    update_sensitivity(app);
    update_preview(app);
    ks_config_save(app->cfg);
}

/* one setting, a checkbox in each tab */
static void on_deco(GtkToggleButton *b, KsApp *app)
{
    app->cfg->decorations = gtk_toggle_button_get_active(b);
    GtkWidget *other = (GtkWidget *)b == app->deco_check ? app->rec_deco_check : app->deco_check;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(other), app->cfg->decorations);
}

static void on_rec_preview(GtkToggleButton *b, KsApp *app)
{
    app->cfg->rec_preview = gtk_toggle_button_get_active(b);
    update_preview(app);
}

static void on_switch_page(GtkNotebook *nb, gpointer page, guint num, KsApp *app)
{
    (void)nb;
    (void)page;
    app->tab = num == 1 ? KS_TAB_RECORD : KS_TAB_SHOT;
    update_preview(app);
    update_sensitivity(app);
}

static void on_map_changed(GtkWidget *w, KsApp *app)
{
    (void)w;
    live_sync(app);
}

static void on_toggle(GtkToggleButton *b, gboolean *field)
{
    *field = gtk_toggle_button_get_active(b);
}

static void on_delay(GtkSpinButton *s, KsApp *app)
{
    app->cfg->delay = gtk_spin_button_get_value_as_int(s);
}

static void refresh_caps(KsApp *app)
{
    const KsRecCaps *caps = ks_rec_probe(app->cfg);
    gtk_widget_set_sensitive(app->rec_button, caps->usable);
    gtk_widget_set_tooltip_text(app->rec_button, caps->usable ? NULL : caps->why_not);
    gboolean audio = caps->audio;
    gtk_widget_set_sensitive(app->sys_check, audio);
    gtk_widget_set_sensitive(app->mic_check, audio);
    const char *why = audio ? NULL : _("Recording sound needs libpulse");
    gtk_widget_set_tooltip_text(app->sys_check, why);
    gtk_widget_set_tooltip_text(app->mic_check, why);
    if (!caps->usable && !app->shot_path)
        set_status(app, caps->why_not);
}

static gboolean probe_idle(gpointer data)
{
    refresh_caps(data);
    return FALSE;
}

static void on_prefs(GtkWidget *w, KsApp *app)
{
    (void)w;
    ks_prefs_run(GTK_WINDOW(app->win), app->cfg, app->tab);
    ks_config_save(app->cfg);
    ks_censor_set_classes(app->cfg->censor_classes);
    refresh_caps(app);
}

/* ---- censor list --------------------------------------------------------- */

static void censor_refresh(void *data)
{
    KsApp *app = data;
    gtk_list_store_clear(app->censor_store);
    GArray *a = ks_censor_marked();
    for (guint i = 0; i < a->len; i++) {
        unsigned long w = g_array_index(a, unsigned long, i);
        char *title = ks_censor_title(w);
        GtkTreeIter it;
        gtk_list_store_append(app->censor_store, &it);
        gtk_list_store_set(app->censor_store, &it, 0, title, 1, w, -1);
        g_free(title);
    }
    /* no compositor to leave the windows out of the picture: kishot paints
     * black over the marked ones itself */
    char *note = ks_capture_status();
    gtk_label_set_text(GTK_LABEL(app->censor_note), note);
    g_free(note);
}

static void on_censor_add(GtkWidget *w, KsApp *app)
{
    (void)w;
    gtk_widget_hide(app->win);
    gdk_flush();
    unsigned long picked = ks_censor_pick();
    gtk_window_present(GTK_WINDOW(app->win));
    if (picked)
        ks_censor_add(picked);
}

/* ---- window -------------------------------------------------------------- */

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, KsApp *app)
{
    (void)ev;
    int ww, hh;
    gtk_window_get_size(GTK_WINDOW(w), &ww, &hh);
    app->cfg->win_w = ww;
    app->cfg->win_h = hh;
    app->cfg->pane_pos = gtk_paned_get_position(GTK_PANED(app->paned));
    ks_config_save(app->cfg);
    if (ks_rec_active()) {
        /* keep recording; the tray icon (or kishot --record) stops it */
        gtk_widget_hide(w);
        return TRUE;
    }
    gtk_main_quit();
    return TRUE;
}

static GtkWidget *button(const char *label, const char *icon, GCallback cb, KsApp *app)
{
    GtkWidget *b = gtk_button_new_with_mnemonic(label);
    if (icon)
        gtk_button_set_image(GTK_BUTTON(b), gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_BUTTON));
    g_signal_connect(b, "clicked", cb, app);
    return b;
}

static GtkWidget *check(GtkWidget *box, const char *label, gboolean active)
{
    GtkWidget *c = gtk_check_button_new_with_mnemonic(label);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(c), active);
    gtk_box_pack_start(GTK_BOX(box), c, FALSE, FALSE, 0);
    return c;
}

static GtkWidget *area_combo(GtkWidget *tbl, int row)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(_("_Area:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, row, row + 1, GTK_FILL, 0, 0, 0);
    GtkWidget *c = gtk_combo_box_new_text();
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), c);
    gtk_table_attach(GTK_TABLE(tbl), c, 1, 2, row, row + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    return c;
}

static GtkWidget *section(const char *title)
{
    GtkWidget *l = gtk_label_new(NULL);
    char *m = g_markup_printf_escaped("<b>%s</b>", title);
    gtk_label_set_markup(GTK_LABEL(l), m);
    g_free(m);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    return l;
}

KsApp *ks_app_new(KsConfig *cfg)
{
    KsApp *app = g_new0(KsApp, 1);
    app->cfg = cfg;
    app->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->win), "kishot");
    gtk_window_set_icon_name(GTK_WINDOW(app->win), "applets-screenshooter");
    gtk_window_set_role(GTK_WINDOW(app->win), "kishot");
    gtk_window_set_default_size(GTK_WINDOW(app->win), cfg->win_w, cfg->win_h);
    g_signal_connect(app->win, "delete-event", G_CALLBACK(on_delete), app);
    g_signal_connect(gdk_screen_get_default(), "monitors-changed", G_CALLBACK(on_monitors_changed), app);

    /* after the default handlers, so gtk_widget_get_mapped() is up to date */
    g_signal_connect_after(app->win, "map", G_CALLBACK(on_map_changed), app);
    g_signal_connect_after(app->win, "unmap", G_CALLBACK(on_map_changed), app);

    GtkWidget *outer = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(app->win), outer);
    app->paned = gtk_hpaned_new();
    gtk_box_pack_start(GTK_BOX(outer), app->paned, TRUE, TRUE, 0);

    /* preview: growing the window grows it, not the options */
    GtkWidget *left = gtk_vbox_new(FALSE, 4);
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    app->preview = gtk_image_new();
    gtk_widget_set_size_request(app->preview, 240, 160);
    g_signal_connect(app->preview, "size-allocate", G_CALLBACK(on_preview_alloc), app);
    gtk_container_add(GTK_CONTAINER(frame), app->preview);
    gtk_box_pack_start(GTK_BOX(left), frame, TRUE, TRUE, 0);
    app->preview_hint = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(app->preview_hint), TRUE);
    gtk_box_pack_start(GTK_BOX(left), app->preview_hint, FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(app->paned), left, TRUE, FALSE);

    /* side panel */
    GtkWidget *side = gtk_vbox_new(FALSE, 6);
    gtk_widget_set_size_request(side, 250, -1);
    gtk_paned_pack2(GTK_PANED(app->paned), side, FALSE, FALSE);
    if (cfg->pane_pos > 0)
        gtk_paned_set_position(GTK_PANED(app->paned), cfg->pane_pos);

    app->notebook = gtk_notebook_new();
    gtk_box_pack_start(GTK_BOX(side), app->notebook, FALSE, FALSE, 0);

    /* Screenshot tab */
    GtkWidget *page = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page, gtk_label_new_with_mnemonic(_("Scr_eenshot")));
    GtkWidget *tbl = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(tbl), 4);
    gtk_table_set_col_spacings(GTK_TABLE(tbl), 6);
    app->area_combo = area_combo(tbl, 0);
    fill_area_combo(app, FALSE);
    g_signal_connect(app->area_combo, "changed", G_CALLBACK(on_area_changed), app);
    GtkWidget *l = gtk_label_new_with_mnemonic(_("_Delay:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    app->delay_spin = gtk_spin_button_new_with_range(0, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->delay_spin), cfg->delay);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), app->delay_spin);
    g_signal_connect(app->delay_spin, "value-changed", G_CALLBACK(on_delay), app);
    gtk_table_attach(GTK_TABLE(tbl), app->delay_spin, 1, 2, 1, 2, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(page), tbl, FALSE, FALSE, 0);
    app->pointer_check = check(page, _("Include the mouse _pointer"), cfg->pointer);
    g_signal_connect(app->pointer_check, "toggled", G_CALLBACK(on_toggle), &cfg->pointer);
    app->deco_check = check(page, _("Include window _borders"), cfg->decorations);
    g_signal_connect(app->deco_check, "toggled", G_CALLBACK(on_deco), app);
    gtk_box_pack_end(GTK_BOX(page), button(_("_New Screenshot"), "camera-photo", G_CALLBACK(on_new_shot), app),
                     FALSE, FALSE, 0);

    /* Recording tab */
    page = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(page), 6);
    gtk_notebook_append_page(GTK_NOTEBOOK(app->notebook), page, gtk_label_new_with_mnemonic(_("Reco_rding")));
    tbl = gtk_table_new(1, 2, FALSE);
    gtk_table_set_col_spacings(GTK_TABLE(tbl), 6);
    app->rec_area_combo = area_combo(tbl, 0);
    fill_area_combo(app, TRUE);
    g_signal_connect(app->rec_area_combo, "changed", G_CALLBACK(on_rec_area_changed), app);
    gtk_box_pack_start(GTK_BOX(page), tbl, FALSE, FALSE, 0);
    app->rec_pointer_check = check(page, _("Record the mouse _pointer"), cfg->rec_pointer);
    g_signal_connect(app->rec_pointer_check, "toggled", G_CALLBACK(on_toggle), &cfg->rec_pointer);
    app->rec_deco_check = check(page, _("Include window _borders"), cfg->decorations);
    g_signal_connect(app->rec_deco_check, "toggled", G_CALLBACK(on_deco), app);
    app->sys_check = check(page, _("_System sound"), cfg->audio_system);
    g_signal_connect(app->sys_check, "toggled", G_CALLBACK(on_toggle), &cfg->audio_system);
    app->mic_check = check(page, _("_Microphone"), cfg->audio_mic);
    g_signal_connect(app->mic_check, "toggled", G_CALLBACK(on_toggle), &cfg->audio_mic);
    app->rec_preview_check = check(page, _("Show a _live preview"), cfg->rec_preview);
    g_signal_connect(app->rec_preview_check, "toggled", G_CALLBACK(on_rec_preview), app);
    app->rec_button = button(_("Record"), "media-record", G_CALLBACK(on_record), app);
    gtk_box_pack_end(GTK_BOX(page), app->rec_button, FALSE, FALSE, 0);
    g_signal_connect(app->notebook, "switch-page", G_CALLBACK(on_switch_page), app);

    gtk_box_pack_start(GTK_BOX(side), section(_("Hidden windows")), FALSE, FALSE, 4);
    app->censor_store = gtk_list_store_new(2, G_TYPE_STRING, G_TYPE_ULONG);
    app->censor_view = gtk_tree_view_new_with_model(GTK_TREE_MODEL(app->censor_store));
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(app->censor_view), FALSE);
    GtkCellRenderer *cr = gtk_cell_renderer_text_new();
    g_object_set(cr, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(app->censor_view),
                                gtk_tree_view_column_new_with_attributes(NULL, cr, "text", 0, NULL));
    gtk_widget_set_tooltip_text(app->censor_view,
                                _("These windows are painted black in screenshots and recordings. "
                                  "The screen itself is not changed. To stop hiding one, untick "
                                  "\"Hide from screen capture\" in its title-bar menu."));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), app->censor_view);
    gtk_widget_set_size_request(sw, -1, 70);
    gtk_box_pack_start(GTK_BOX(side), sw, TRUE, TRUE, 0);
    GtkWidget *cb = gtk_hbox_new(TRUE, 4);
    gtk_box_pack_start(GTK_BOX(cb), button(_("_Hide a Window..."), "list-add", G_CALLBACK(on_censor_add), app),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(side), cb, FALSE, FALSE, 0);
    app->censor_note = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(app->censor_note), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(app->censor_note), PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(side), app->censor_note, FALSE, FALSE, 0);

    /* bottom bar */
    GtkWidget *bar = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(outer), bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bar), button(_("Settings"), "preferences-system", G_CALLBACK(on_prefs), app),
                       FALSE, FALSE, 0);
    app->status = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(app->status), 0, 0.5);
    gtk_label_set_ellipsize(GTK_LABEL(app->status), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(bar), app->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(bar), button(_("_Open Folder"), "folder", G_CALLBACK(on_open_folder), app),
                       FALSE, FALSE, 0);
    app->copy_button = button(_("_Copy"), "edit-copy", G_CALLBACK(on_copy), app);
    gtk_box_pack_start(GTK_BOX(bar), app->copy_button, FALSE, FALSE, 0);
    app->saveas_button = button(_("Save _As..."), "document-save-as", G_CALLBACK(on_save_as), app);
    gtk_box_pack_start(GTK_BOX(bar), app->saveas_button, FALSE, FALSE, 0);
    app->save_button = button(_("_Save"), "document-save", G_CALLBACK(on_save), app);
    gtk_box_pack_start(GTK_BOX(bar), app->save_button, FALSE, FALSE, 0);

    gtk_widget_show_all(outer);
    update_preview(app);
    update_sensitivity(app);
    ks_censor_init(cfg, censor_refresh, app);
    /* loading the plugin and asking ffmpeg/VAAPI what they can do takes a
     * moment; never before the first shot */
    g_timeout_add(400, probe_idle, app);
    return app;
}

/* ks_window.c - the kishot window and what its commands do.
 *
 * Like Spectacle: launching takes a shot of the area used last time and
 * shows it here, with Save / Copy / New screenshot. Recording uses the same
 * area choice; while it runs the window hides itself (so it is not in the
 * video) and a tray icon stops it -- as does running `kishot --record`
 * again, which is what the hotkey does. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <string.h>
#include <time.h>

struct KsApp {
    KsConfig *cfg;
    GtkWidget *win;
    GtkWidget *preview;
    GtkWidget *area_combo, *delay_spin, *pointer_check, *deco_check;
    GtkWidget *sys_check, *mic_check;
    GtkWidget *rec_button, *save_button, *saveas_button, *copy_button;
    GtkWidget *status;
    GtkListStore *censor_store;
    GtkWidget *censor_view;
    GtkStatusIcon *tray;
    GdkPixbuf *shot;
    char *shot_path;            /* where the current shot was saved, if it was */
    char *last_dir;             /* folder of the last saved file */
    gboolean background;        /* launched with -b: no window, quit when done */
    gboolean busy;              /* a shot is being taken */
    guint rec_timer;
};

static void set_status(KsApp *app, const char *text)
{
    gtk_label_set_text(GTK_LABEL(app->status), text ? text : "");
}

/* ---- preview ------------------------------------------------------------- */

static void update_preview(KsApp *app)
{
    if (!app->shot) {
        gtk_image_set_from_icon_name(GTK_IMAGE(app->preview), "camera-photo", GTK_ICON_SIZE_DIALOG);
        return;
    }
    GtkAllocation a;
    gtk_widget_get_allocation(app->preview, &a);
    int W = gdk_pixbuf_get_width(app->shot), H = gdk_pixbuf_get_height(app->shot);
    int mw = MAX(a.width - 8, 32), mh = MAX(a.height - 8, 32);
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
    gtk_widget_set_sensitive(app->save_button, have);
    gtk_widget_set_sensitive(app->saveas_button, have);
    gtk_widget_set_sensitive(app->copy_button, have);
    gtk_widget_set_sensitive(app->deco_check,
                             gtk_combo_box_get_active(GTK_COMBO_BOX(app->area_combo)) == KS_AREA_WINDOW);
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
    char *dir = app->last_dir ? g_strdup(app->last_dir) : ks_config_expand_dir(app->cfg->shot_dir);
    g_mkdir_with_parents(dir, 0755);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri)
        gtk_show_uri(NULL, uri, GDK_CURRENT_TIME, NULL);
    g_free(uri);
    g_free(dir);
}

/* ---- taking a shot ------------------------------------------------------- */

typedef struct { KsApp *app; KsArea area; gboolean record; GdkPixbuf *frozen; } Pending;

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
            g_free(p);
            shot_ready(app, NULL);
            return FALSE;
        }
        p->frozen = frozen;
        ks_region_select(frozen, region_done, p);
        return FALSE;
    }
    KsRect r;
    ks_area_rect(p->area, app->cfg->decorations, &r);
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
    if (area >= 0 && area < KS_AREA_N) {
        app->cfg->area = area;
        gtk_combo_box_set_active(GTK_COMBO_BOX(app->area_combo), area);
        ks_config_save(app->cfg);
    }
    /* a second `kishot -b` while the window is open still shows it */
    app->background = background && !gtk_widget_get_visible(app->win);
    app->busy = TRUE;
    gboolean was_visible = gtk_widget_get_visible(app->win);
    gtk_widget_hide(app->win);
    gdk_flush();
    Pending *p = g_new0(Pending, 1);
    p->app = app;
    p->area = app->cfg->area;
    p->record = kind == KS_CMD_RECORD;
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
        g_free(app->last_dir);
        app->last_dir = g_path_get_dirname(path);
    }
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

static void on_area_changed(GtkComboBox *c, KsApp *app)
{
    app->cfg->area = gtk_combo_box_get_active(c);
    update_sensitivity(app);
    ks_config_save(app->cfg);
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
    ks_prefs_run(GTK_WINDOW(app->win), app->cfg);
    ks_config_save(app->cfg);
    ks_censor_set_classes(app->cfg->censor_classes);
    refresh_caps(app);
}

/* ---- censor list --------------------------------------------------------- */

static void censor_refresh(void *data)
{
    KsApp *app = data;
    gtk_list_store_clear(app->censor_store);
    GArray *a = ks_censor_picked();
    for (guint i = 0; i < a->len; i++) {
        unsigned long w = g_array_index(a, unsigned long, i);
        char *title = ks_censor_title(w);
        GtkTreeIter it;
        gtk_list_store_append(app->censor_store, &it);
        gtk_list_store_set(app->censor_store, &it, 0, title, 1, w, -1);
        g_free(title);
    }
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

static void on_censor_remove(GtkWidget *w, KsApp *app)
{
    (void)w;
    GtkTreeIter it;
    GtkTreeModel *m;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(app->censor_view)), &m, &it))
        return;
    gulong win;
    gtk_tree_model_get(m, &it, 1, &win, -1);
    ks_censor_remove(win);
}

/* ---- window -------------------------------------------------------------- */

static gboolean on_delete(GtkWidget *w, GdkEvent *ev, KsApp *app)
{
    (void)ev;
    int ww, hh;
    gtk_window_get_size(GTK_WINDOW(w), &ww, &hh);
    app->cfg->win_w = ww;
    app->cfg->win_h = hh;
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

    GtkWidget *outer = gtk_vbox_new(FALSE, 6);
    gtk_container_set_border_width(GTK_CONTAINER(outer), 8);
    gtk_container_add(GTK_CONTAINER(app->win), outer);
    GtkWidget *hbox = gtk_hbox_new(FALSE, 10);
    gtk_box_pack_start(GTK_BOX(outer), hbox, TRUE, TRUE, 0);

    /* preview */
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    app->preview = gtk_image_new();
    gtk_widget_set_size_request(app->preview, 240, 160);
    g_signal_connect(app->preview, "size-allocate", G_CALLBACK(on_preview_alloc), app);
    gtk_container_add(GTK_CONTAINER(frame), app->preview);
    gtk_box_pack_start(GTK_BOX(hbox), frame, TRUE, TRUE, 0);

    /* side panel */
    GtkWidget *side = gtk_vbox_new(FALSE, 6);
    gtk_widget_set_size_request(side, 250, -1);
    gtk_box_pack_start(GTK_BOX(hbox), side, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(side), section(_("Capture")), FALSE, FALSE, 0);
    GtkWidget *tbl = gtk_table_new(2, 2, FALSE);
    gtk_table_set_row_spacings(GTK_TABLE(tbl), 4);
    gtk_table_set_col_spacings(GTK_TABLE(tbl), 6);
    GtkWidget *l = gtk_label_new_with_mnemonic(_("_Area:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, 0, 1, GTK_FILL, 0, 0, 0);
    app->area_combo = gtk_combo_box_new_text();
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->area_combo), _("Full screen (all monitors)"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->area_combo), _("Current monitor"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->area_combo), _("Active window"));
    gtk_combo_box_append_text(GTK_COMBO_BOX(app->area_combo), _("Rectangular region"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(app->area_combo), cfg->area);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), app->area_combo);
    g_signal_connect(app->area_combo, "changed", G_CALLBACK(on_area_changed), app);
    gtk_table_attach(GTK_TABLE(tbl), app->area_combo, 1, 2, 0, 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    l = gtk_label_new_with_mnemonic(_("_Delay:"));
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, 1, 2, GTK_FILL, 0, 0, 0);
    app->delay_spin = gtk_spin_button_new_with_range(0, 60, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->delay_spin), cfg->delay);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), app->delay_spin);
    g_signal_connect(app->delay_spin, "value-changed", G_CALLBACK(on_delay), app);
    gtk_table_attach(GTK_TABLE(tbl), app->delay_spin, 1, 2, 1, 2, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    gtk_box_pack_start(GTK_BOX(side), tbl, FALSE, FALSE, 0);

    app->pointer_check = gtk_check_button_new_with_mnemonic(_("Include the mouse _pointer"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->pointer_check), cfg->pointer);
    g_signal_connect(app->pointer_check, "toggled", G_CALLBACK(on_toggle), &cfg->pointer);
    gtk_box_pack_start(GTK_BOX(side), app->pointer_check, FALSE, FALSE, 0);
    app->deco_check = gtk_check_button_new_with_mnemonic(_("Include window _borders"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->deco_check), cfg->decorations);
    g_signal_connect(app->deco_check, "toggled", G_CALLBACK(on_toggle), &cfg->decorations);
    gtk_box_pack_start(GTK_BOX(side), app->deco_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(side), button(_("_New Screenshot"), "camera-photo", G_CALLBACK(on_new_shot), app),
                       FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(side), section(_("Recording")), FALSE, FALSE, 4);
    app->sys_check = gtk_check_button_new_with_mnemonic(_("_System sound"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->sys_check), cfg->audio_system);
    g_signal_connect(app->sys_check, "toggled", G_CALLBACK(on_toggle), &cfg->audio_system);
    gtk_box_pack_start(GTK_BOX(side), app->sys_check, FALSE, FALSE, 0);
    app->mic_check = gtk_check_button_new_with_mnemonic(_("_Microphone"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(app->mic_check), cfg->audio_mic);
    g_signal_connect(app->mic_check, "toggled", G_CALLBACK(on_toggle), &cfg->audio_mic);
    gtk_box_pack_start(GTK_BOX(side), app->mic_check, FALSE, FALSE, 0);
    app->rec_button = button(_("Record"), "media-record", G_CALLBACK(on_record), app);
    gtk_box_pack_start(GTK_BOX(side), app->rec_button, FALSE, FALSE, 0);

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
                                  "The screen itself is not changed."));
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(sw), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(sw), app->censor_view);
    gtk_widget_set_size_request(sw, -1, 70);
    gtk_box_pack_start(GTK_BOX(side), sw, TRUE, TRUE, 0);
    GtkWidget *cb = gtk_hbox_new(TRUE, 4);
    gtk_box_pack_start(GTK_BOX(cb), button(_("_Hide a Window..."), "list-add", G_CALLBACK(on_censor_add), app),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(cb), button(_("Remo_ve"), "list-remove", G_CALLBACK(on_censor_remove), app),
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(side), cb, FALSE, FALSE, 0);

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

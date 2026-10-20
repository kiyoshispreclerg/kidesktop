/* ks_prefs.c - the Settings dialog: where files go, formats, codec, how
 * recordings are captured.
 * Options for something that isn't available here (no plugin, no ffmpeg,
 * no VAAPI) are listed but explained, never silently missing. */
#include "ks.h"
#include "../shared/xis_i18n.h"
#include <string.h>

typedef struct {
    KsConfig *cfg;
    GtkWidget *shot_dir, *format, *jpeg_q, *pattern, *auto_save, *auto_copy;
    GtkWidget *video_dir, *container, *codec, *quality, *fps, *copy, *backend;
    GtkWidget *caps_label;
    GPtrArray *formats;     /* writable gdk-pixbuf format names */
} Prefs;

static GtkWidget *row(GtkWidget *tbl, int r, const char *label, GtkWidget *w)
{
    GtkWidget *l = gtk_label_new_with_mnemonic(label);
    gtk_misc_set_alignment(GTK_MISC(l), 0, 0.5);
    gtk_label_set_mnemonic_widget(GTK_LABEL(l), w);
    gtk_table_attach(GTK_TABLE(tbl), l, 0, 1, r, r + 1, GTK_FILL, 0, 0, 0);
    gtk_table_attach(GTK_TABLE(tbl), w, 1, 2, r, r + 1, GTK_FILL | GTK_EXPAND, 0, 0, 0);
    return w;
}

static GtkWidget *table(void)
{
    GtkWidget *t = gtk_table_new(1, 2, FALSE);
    gtk_container_set_border_width(GTK_CONTAINER(t), 10);
    gtk_table_set_row_spacings(GTK_TABLE(t), 6);
    gtk_table_set_col_spacings(GTK_TABLE(t), 10);
    return t;
}

static GtkWidget *folder_button(const char *dir_cfg)
{
    GtkWidget *b = gtk_file_chooser_button_new(NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    char *dir = ks_config_expand_dir(dir_cfg);
    g_mkdir_with_parents(dir, 0755);
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(b), dir);
    g_free(dir);
    return b;
}

/* keep "$PICTURES/..." when the user didn't change the folder */
static void read_folder(GtkWidget *b, char **field)
{
    char *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(b));
    char *current = ks_config_expand_dir(*field);
    if (chosen && strcmp(chosen, current)) {
        g_free(*field);
        *field = chosen;
        chosen = NULL;
    }
    g_free(chosen);
    g_free(current);
}

static GtkWidget *combo_with(const char *const *items, int n, const char *active)
{
    GtkWidget *c = gtk_combo_box_new_text();
    int sel = 0;
    for (int i = 0; i < n; i++) {
        gtk_combo_box_append_text(GTK_COMBO_BOX(c), items[i]);
        if (active && !g_ascii_strcasecmp(items[i], active))
            sel = i;
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(c), sel);
    return c;
}

static void fill_codecs(Prefs *p)
{
    GtkComboBox *c = GTK_COMBO_BOX(p->codec);
    GtkTreeModel *m = gtk_combo_box_get_model(c);
    gtk_list_store_clear(GTK_LIST_STORE(m));
    gtk_combo_box_append_text(c, "auto");
    const KsRecCaps *caps = ks_rec_probe(p->cfg);
    int sel = 0;
    gboolean found = !strcmp(p->cfg->vcodec, "auto");
    for (guint i = 0; i < caps->encoders->len; i++) {
        gtk_combo_box_append_text(c, caps->encoders->pdata[i]);
        if (!strcmp(caps->encoders->pdata[i], p->cfg->vcodec)) {
            sel = i + 1;
            found = TRUE;
        }
    }
    if (!found) {
        /* keep a codec chosen earlier visible even if it can't be used now */
        gtk_combo_box_append_text(c, p->cfg->vcodec);
        sel = caps->encoders->len + 1;
    }
    gtk_combo_box_set_active(c, sel);

    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%s: %s\n", _("libav plugin"), caps->plugin ? _("loaded") : _("not available"));
    g_string_append_printf(s, "%s: %s\n", _("ffmpeg command"), caps->cli ? _("found") : _("not found"));
    g_string_append_printf(s, "%s: %s\n", _("VAAPI (GPU encoding)"), caps->vaapi ? _("available") : _("not available"));
    g_string_append_printf(s, "%s: %s", _("Sound (libpulse)"), caps->audio ? _("available") : _("not available"));
    if (!caps->usable)
        g_string_append_printf(s, "\n\n%s", caps->why_not);
    gtk_label_set_text(GTK_LABEL(p->caps_label), s->str);
    g_string_free(s, TRUE);
}

static void on_backend(GtkComboBox *c, Prefs *p)
{
    p->cfg->backend = gtk_combo_box_get_active(c);
    fill_codecs(p);
}

static GtkWidget *page_screenshots(Prefs *p)
{
    KsConfig *c = p->cfg;
    GtkWidget *t = table();
    int r = 0;
    p->shot_dir = row(t, r++, _("Save _in:"), folder_button(c->shot_dir));

    p->formats = g_ptr_array_new_with_free_func(g_free);
    GSList *fl = gdk_pixbuf_get_formats();
    static const char *const order[] = { "png", "jpeg", "webp", "bmp", "tiff" };
    for (unsigned i = 0; i < G_N_ELEMENTS(order); i++)
        for (GSList *f = fl; f; f = f->next) {
            char *name = gdk_pixbuf_format_get_name(f->data);
            if (!strcmp(name, order[i]) && gdk_pixbuf_format_is_writable(f->data)) {
                g_ptr_array_add(p->formats, name);
                name = NULL;
            }
            g_free(name);
        }
    g_slist_free(fl);
    p->format = row(t, r++, _("Image _format:"),
                    combo_with((const char *const *)p->formats->pdata, p->formats->len, c->shot_format));
    p->jpeg_q = row(t, r++, _("_JPEG quality:"), gtk_spin_button_new_with_range(1, 100, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->jpeg_q), c->jpeg_quality);
    p->pattern = row(t, r++, _("File _name:"), gtk_entry_new());
    gtk_entry_set_text(GTK_ENTRY(p->pattern), c->name_pattern);
    gtk_widget_set_tooltip_text(p->pattern, _("strftime() pattern: %Y year, %m month, %d day, %H%M%S time. "
                                              "The extension is added automatically."));
    p->auto_save = gtk_check_button_new_with_mnemonic(_("Save _automatically after taking a screenshot"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->auto_save), c->auto_save);
    gtk_table_attach(GTK_TABLE(t), p->auto_save, 0, 2, r, r + 1, GTK_FILL, 0, 0, 0);
    r++;
    p->auto_copy = gtk_check_button_new_with_mnemonic(_("Copy to the _clipboard automatically"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->auto_copy), c->auto_copy);
    gtk_table_attach(GTK_TABLE(t), p->auto_copy, 0, 2, r, r + 1, GTK_FILL, 0, 0, 0);
    return t;
}

static GtkWidget *page_recording(Prefs *p)
{
    KsConfig *c = p->cfg;
    GtkWidget *t = table();
    int r = 0;
    p->video_dir = row(t, r++, _("Save _in:"), folder_button(c->video_dir));
    static const char *const containers[] = { "mkv", "mp4", "webm" };
    p->container = row(t, r++, _("Con_tainer:"), combo_with(containers, 3, c->container));
    gtk_widget_set_tooltip_text(p->container, _("mkv survives a crash mid-recording; webm needs VP8/VP9/AV1"));
    p->codec = row(t, r++, _("Video _codec:"), gtk_combo_box_new_text());
    gtk_widget_set_tooltip_text(p->codec, _("auto: the GPU (VAAPI) when available, else x264"));
    p->quality = row(t, r++, _("_Quality (0 best, 51 worst):"), gtk_spin_button_new_with_range(0, 51, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->quality), c->quality);
    p->fps = row(t, r++, _("_Frames per second:"), gtk_spin_button_new_with_range(1, 240, 1));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(p->fps), c->fps);
    const char *copies[] = { _("Automatic"), _("GPU copy"), _("SHM (classic)") };
    p->copy = row(t, r++, _("Screen c_opy method:"), combo_with(copies, 3, NULL));
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->copy), c->copy_mode);
    gtk_widget_set_tooltip_text(p->copy,
        _("GPU copy has the X server copy the screen into a buffer the GPU encodes directly, without reading "
          "it back. Automatic uses it only when the server guarantees it (XiS); forcing it on another server "
          "can freeze the video."));
    const char *backends[] = { _("Automatic"), _("libav plugin"), _("ffmpeg command") };
    p->backend = row(t, r++, _("_Encoder:"), combo_with(backends, 3, NULL));
    gtk_combo_box_set_active(GTK_COMBO_BOX(p->backend), c->backend);
    p->caps_label = gtk_label_new(NULL);
    gtk_misc_set_alignment(GTK_MISC(p->caps_label), 0, 0);
    gtk_label_set_line_wrap(GTK_LABEL(p->caps_label), TRUE);
    gtk_table_attach(GTK_TABLE(t), p->caps_label, 0, 2, r, r + 1, GTK_FILL, 0, 0, 6);
    fill_codecs(p);
    g_signal_connect(p->backend, "changed", G_CALLBACK(on_backend), p);
    return t;
}

static char *combo_text(GtkWidget *c)
{
    return gtk_combo_box_get_active_text(GTK_COMBO_BOX(c));
}

void ks_prefs_run(GtkWindow *parent, KsConfig *cfg, KsTab page)
{
    Prefs p = { .cfg = cfg };
    KsBackend old_backend = cfg->backend;
    GtkWidget *dlg = gtk_dialog_new_with_buttons(_("kishot Settings"), parent, GTK_DIALOG_MODAL,
                                                 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                                                 GTK_STOCK_OK, GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);
    GtkWidget *nb = gtk_notebook_new();
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page_screenshots(&p), gtk_label_new(_("Screenshots")));
    gtk_notebook_append_page(GTK_NOTEBOOK(nb), page_recording(&p), gtk_label_new(_("Recording")));
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))), nb, TRUE, TRUE, 0);
    gtk_widget_show_all(dlg);
    /* the page of the tab the main window is on (pages only switch once shown) */
    gtk_notebook_set_current_page(GTK_NOTEBOOK(nb), page == KS_TAB_RECORD ? 1 : 0);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        read_folder(p.shot_dir, &cfg->shot_dir);
        char *s = combo_text(p.format);
        if (s) {
            g_free(cfg->shot_format);
            cfg->shot_format = s;
        }
        cfg->jpeg_quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p.jpeg_q));
        const char *pat = gtk_entry_get_text(GTK_ENTRY(p.pattern));
        if (*pat) {
            g_free(cfg->name_pattern);
            cfg->name_pattern = g_strdup(pat);
        }
        cfg->auto_save = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.auto_save));
        cfg->auto_copy = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p.auto_copy));

        read_folder(p.video_dir, &cfg->video_dir);
        if ((s = combo_text(p.container))) {
            g_free(cfg->container);
            cfg->container = s;
        }
        if ((s = combo_text(p.codec))) {
            g_free(cfg->vcodec);
            cfg->vcodec = s;
        }
        cfg->quality = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p.quality));
        cfg->fps = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(p.fps));
        cfg->copy_mode = gtk_combo_box_get_active(GTK_COMBO_BOX(p.copy));
        cfg->backend = gtk_combo_box_get_active(GTK_COMBO_BOX(p.backend));
    } else {
        cfg->backend = old_backend;
    }
    gtk_widget_destroy(dlg);
    g_ptr_array_free(p.formats, TRUE);
}

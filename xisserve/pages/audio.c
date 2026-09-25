/*
 * audio.c - the --audio page (see ../PROTOCOL.md and xisserve.h's
 * "pages" section): a mixer showing the streams currently playing or
 * recording, each with its own level and mute, plus the output and
 * input devices with level, mute, which one is default, and whether
 * it's active.
 *
 * All state comes from pulse.c (`pactl`, no libpulse/libpipewire link
 * -- see pulse.h). Nothing here is required at build time and nothing
 * fails at runtime without a sound server: pulse_available() reports 0
 * and this page renders an explanatory placeholder instead.
 *
 * Refresh model: a full rebuild whenever the page is shown, plus a
 * poll while it's visible so streams appearing/disappearing (a video
 * starting, a call ending) and volume changes made elsewhere show up
 * without reopening. The poll only rebuilds when the *set* of objects
 * changed (see snapshot_signature()); otherwise it just updates the
 * existing widgets' values in place, so a rebuild never yanks a slider
 * out from under a drag. It also stands down entirely while the user is
 * actually dragging, and briefly after any change this page itself
 * made -- pactl's effect isn't necessarily visible to the next `pactl
 * list` instantly, and without that pause a just-moved slider can
 * visibly snap back to the old value for one tick.
 */
#include "../xisserve.h"
#include "pulse.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define AUDIO_POLL_MS 1500
/* Caps how wide a stream's device_combo can grow -- GtkComboBox's
 * natural width otherwise follows its *longest* item's full text (a
 * device Description can run to 40+ chars), which forced the whole page
 * wider than the window and made every other row's content scroll
 * horizontally to see. The renderer's own PANGO_ELLIPSIZE_END (set where
 * the combo is built) is what lets it actually shrink to this instead of
 * just clipping mid-glyph; the full name is still there as a tooltip. */
#define AUDIO_DEVICE_COMBO_MAX_WIDTH 150
/* How long after a user-initiated change to leave the poll alone -- see
 * the file comment. */
#define AUDIO_SETTLE_US (900 * 1000)
#define AUDIO_VOLUME_MAX 150

static GtkWidget *g_root;      /* scrolled window handed to xisserve.c */
static GtkWidget *g_rows_box;  /* vbox rebuilt in place; everything below lives here */
static guint g_poll_id;
static gboolean g_dragging;    /* a slider is held down right now */
static gint64 g_last_action_us;
static gboolean g_updating;    /* set while populating widgets, so their own signals don't echo back to pactl */
static char g_signature[4096];

static void rebuild(void);

/* One built row, kept alive alongside its widgets so the callbacks know
 * which object they act on. The PulseEntry is a copy, not a pointer
 * into the snapshot, because the snapshot is freed as soon as the
 * rebuild finishes. */
typedef struct {
    PulseEntry entry;
    GtkWidget *scale;
    GtkWidget *mute;
    GtkWidget *use_default;
    GtkWidget *active;
    GtkWidget *device_combo; /* streams only: which sink/source this stream plays to/records from */
} AudioRow;

/* device_combo's model columns: DCOL_NAME is the target device's stable
 * pactl name (what pulse_move_stream() addresses it by), DCOL_LABEL its
 * Description. */
enum { DCOL_NAME = 0, DCOL_LABEL, N_DCOLS };

/* Applied to every state, not just GTK_STATE_NORMAL: a GtkCheckButton
 * draws its label in GTK_STATE_ACTIVE while checked and PRELIGHT under
 * the pointer, so a NORMAL-only override leaves those cases painted in
 * the GTK theme's own foreground -- which against this popup's dark
 * --bg renders "Padrão"/"Ativo" as good as invisible the moment the box
 * is ticked. */
static void style_fg(GtkWidget *w)
{
    double r, g, b, a;
    xisserve_get_fg_rgba(&r, &g, &b, &a);
    GdkColor c = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    static const GtkStateType states[] = {GTK_STATE_NORMAL, GTK_STATE_ACTIVE, GTK_STATE_PRELIGHT, GTK_STATE_SELECTED};
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        gtk_widget_modify_fg(w, states[i], &c);
        gtk_widget_modify_text(w, states[i], &c);
    }
}

static void note_user_action(void)
{
    g_last_action_us = g_get_monotonic_time();
}

/* ---- widget callbacks ---------------------------------------------------- */

/* How much one scroll notch moves a slider, from xisserve.conf's
 * "AUDIO\tscroll_step\t<percent>" line. Read per event rather than
 * cached so an edit takes effect on the next open with no extra
 * plumbing (load_config() already re-runs there). */
static int scroll_step(void)
{
    int step = xisserve_config_get_int("AUDIO", "scroll_step", 5);
    if (step < 1) {
        step = 1;
    }
    if (step > AUDIO_VOLUME_MAX) {
        step = AUDIO_VOLUME_MAX;
    }
    return step;
}

/* GtkRange's built-in scroll handling is defined in terms of the
 * widget's own axis, so on a *horizontal* slider scroll-up means "move
 * left", i.e. quieter -- backwards from what a volume control should do
 * (and from what the panel's own volume widget does on scroll). Handling
 * the event here and returning TRUE replaces that mapping entirely
 * rather than trying to correct it afterwards: up/right raises,
 * down/left lowers, by the configured step.
 *
 * gtk_range_set_value() emits "value-changed", so on_scale_changed()
 * still does the actual pulse_set_volume() -- this function only decides
 * the new number. */
static gboolean on_scale_scroll(GtkWidget *w, GdkEventScroll *ev, gpointer data)
{
    (void)data;
    double v = gtk_range_get_value(GTK_RANGE(w));
    int step = scroll_step();
    if (ev->direction == GDK_SCROLL_UP || ev->direction == GDK_SCROLL_RIGHT) {
        v += step;
    } else if (ev->direction == GDK_SCROLL_DOWN || ev->direction == GDK_SCROLL_LEFT) {
        v -= step;
    } else {
        return FALSE;
    }
    if (v < 0) {
        v = 0;
    }
    if (v > AUDIO_VOLUME_MAX) {
        v = AUDIO_VOLUME_MAX;
    }
    note_user_action();
    gtk_range_set_value(GTK_RANGE(w), v);
    return TRUE;
}

static gboolean on_scale_press(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    g_dragging = TRUE;
    return FALSE;
}

static gboolean on_scale_release(GtkWidget *w, GdkEventButton *ev, gpointer data)
{
    (void)w;
    (void)ev;
    (void)data;
    g_dragging = FALSE;
    note_user_action();
    return FALSE;
}

static void on_scale_changed(GtkRange *range, gpointer data)
{
    if (g_updating) {
        return;
    }
    AudioRow *row = data;
    note_user_action();
    pulse_set_volume(&row->entry, (int)gtk_range_get_value(range));
}

static void on_mute_toggled(GtkToggleButton *btn, gpointer data)
{
    if (g_updating) {
        return;
    }
    AudioRow *row = data;
    note_user_action();
    row->entry.muted = gtk_toggle_button_get_active(btn);
    pulse_set_mute(&row->entry, row->entry.muted);
}

/* GtkComboBox's dropdown list takes the X grab the same way our
 * right-click context menu does (see xisserve.h's
 * xisserve_transient_popup_begin()/_end()) -- "popup-shown" is the GTK2
 * property that fires for both opening and closing, so one handler
 * covers the whole pair. */
static void on_device_combo_popup_notify(GObject *combo, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    (void)data;
    gboolean shown = FALSE;
    g_object_get(combo, "popup-shown", &shown, NULL);
    if (shown) {
        xisserve_transient_popup_begin();
    } else {
        xisserve_transient_popup_end();
    }
}

static void on_device_combo_changed(GtkComboBox *combo, gpointer data)
{
    if (g_updating) {
        return;
    }
    GtkTreeIter iter;
    if (!gtk_combo_box_get_active_iter(combo, &iter)) {
        return;
    }
    AudioRow *row = data;
    gchar *target_name = NULL, *target_label = NULL;
    gtk_tree_model_get(gtk_combo_box_get_model(combo), &iter, DCOL_NAME, &target_name, DCOL_LABEL, &target_label, -1);
    if (!target_name) {
        g_free(target_label);
        return;
    }
    gtk_widget_set_tooltip_text(GTK_WIDGET(combo), target_label);
    note_user_action();
    pulse_move_stream(&row->entry, target_name);
    g_free(target_name);
    g_free(target_label);
}

static gboolean rebuild_idle(gpointer data)
{
    (void)data;
    rebuild();
    return FALSE;
}

static void on_default_toggled(GtkToggleButton *btn, gpointer data)
{
    if (g_updating) {
        return;
    }
    AudioRow *row = data;
    if (!gtk_toggle_button_get_active(btn)) {
        /* "Not default" isn't an action a sound server can take -- some
         * device always is. Un-ticking is therefore meaningless; put the
         * tick back and do nothing, leaving "make this one default" as
         * the only thing this control does. */
        g_updating = TRUE;
        gtk_toggle_button_set_active(btn, TRUE);
        g_updating = FALSE;
        return;
    }
    note_user_action();
    pulse_set_default(&row->entry);
    /* Whichever device *was* default has to lose its tick -- but not
     * from inside this handler: rebuild() destroys every row, including
     * the button being toggled right now and the AudioRow `row` still
     * points at (freed with the widget by g_object_set_data_full). Doing
     * that mid-emission would leave GTK and this function both walking
     * freed memory, so it's deferred to the next main-loop iteration. */
    g_idle_add(rebuild_idle, NULL);
}

static void on_active_toggled(GtkToggleButton *btn, gpointer data)
{
    if (g_updating) {
        return;
    }
    AudioRow *row = data;
    note_user_action();
    row->entry.suspended = !gtk_toggle_button_get_active(btn);
    pulse_set_suspended(&row->entry, row->entry.suspended);
}

/* ---- building ------------------------------------------------------------ */

static GtkWidget *section_header(const char *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    char markup[128];
    snprintf(markup, sizeof(markup), "<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    style_fg(label);
    return label;
}

/* Builds the sink/source picker for a stream row (PULSE_SINK_INPUT ->
 * PULSE_SINK choices, PULSE_SOURCE_OUTPUT -> PULSE_SOURCE choices),
 * pre-selected to the stream's current device (e->device_index, matched
 * against each candidate's own `index` from the same snapshot). NULL for
 * a device row, or a stream whose target device isn't in `all_entries`
 * (nothing to preselect against, so nothing to build). */
static GtkWidget *build_device_combo(const PulseEntry *e, GPtrArray *all_entries)
{
    PulseKind target_kind = e->kind == PULSE_SINK_INPUT ? PULSE_SINK : PULSE_SOURCE;

    GtkListStore *store = gtk_list_store_new(N_DCOLS, G_TYPE_STRING, G_TYPE_STRING);
    GtkTreeIter active_iter;
    gboolean have_active = FALSE;
    char active_label[256];
    active_label[0] = 0;
    for (guint i = 0; i < all_entries->len; i++) {
        PulseEntry *cand = g_ptr_array_index(all_entries, i);
        if (cand->kind != target_kind) {
            continue;
        }
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter, DCOL_NAME, cand->name, DCOL_LABEL, cand->label, -1);
        if (cand->index == e->device_index) {
            active_iter = iter;
            have_active = TRUE;
            snprintf(active_label, sizeof(active_label), "%s", cand->label);
        }
    }

    GtkWidget *combo = gtk_combo_box_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);
    GtkCellRenderer *rend = gtk_cell_renderer_text_new();
    gtk_cell_layout_pack_start(GTK_CELL_LAYOUT(combo), rend, TRUE);
    gtk_cell_layout_add_attribute(GTK_CELL_LAYOUT(combo), rend, "text", DCOL_LABEL);
    /* Ellipsize on the renderer is what actually lets the combo shrink
     * below its longest item's natural width -- the size_request below
     * this just picks the ceiling it shrinks (or is offered) to. Popup
     * rows aren't capped: the dropdown itself can still show full names. */
    g_object_set(rend, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    gtk_widget_set_size_request(combo, AUDIO_DEVICE_COMBO_MAX_WIDTH, -1);
    if (have_active) {
        gtk_combo_box_set_active_iter(GTK_COMBO_BOX(combo), &active_iter);
        gtk_widget_set_tooltip_text(combo, active_label);
    }
    return combo;
}

/* Re-selects device_combo's active item to match e->device_index against
 * `all_entries`, without touching the combo's own list of choices --
 * refresh_values()'s in-place counterpart to build_device_combo()'s
 * initial selection, for when a stream's target device changed (user
 * picked a new one here, or it moved some other way) but the set of
 * rows on screen didn't. */
static void sync_device_combo(AudioRow *row, GPtrArray *all_entries)
{
    if (!row->device_combo) {
        return;
    }
    PulseKind target_kind = row->entry.kind == PULSE_SINK_INPUT ? PULSE_SINK : PULSE_SOURCE;
    const char *target_name = NULL;
    for (guint i = 0; i < all_entries->len; i++) {
        PulseEntry *cand = g_ptr_array_index(all_entries, i);
        if (cand->kind == target_kind && cand->index == row->entry.device_index) {
            target_name = cand->name;
            break;
        }
    }
    if (!target_name) {
        return;
    }
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(row->device_combo));
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) {
        return;
    }
    do {
        gchar *name = NULL, *label = NULL;
        gtk_tree_model_get(model, &iter, DCOL_NAME, &name, DCOL_LABEL, &label, -1);
        gboolean match = name && strcmp(name, target_name) == 0;
        g_free(name);
        if (match) {
            gtk_combo_box_set_active_iter(GTK_COMBO_BOX(row->device_combo), &iter);
            gtk_widget_set_tooltip_text(row->device_combo, label);
            g_free(label);
            return;
        }
        g_free(label);
    } while (gtk_tree_model_iter_next(model, &iter));
}

/* Rows are structurally the same for every kind -- name/icon and mute on
 * top, slider below -- with devices growing one extra line of
 * device-only controls (default/active). Keeping one builder rather than
 * one per kind is what keeps the four sections visually consistent. */
static GtkWidget *build_row(const PulseEntry *e, GPtrArray *all_entries)
{
    AudioRow *row = g_new0(AudioRow, 1);
    row->entry = *e;

    GtkWidget *box = gtk_vbox_new(FALSE, 2);
    g_object_set_data_full(G_OBJECT(box), "audio-row", row, g_free);

    GtkWidget *top = gtk_hbox_new(FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    if (e->icon_name[0]) {
        GdkPixbuf *pix = xisserve_resolve_icon(e->icon_name, XISSERVE_ICON_PX);
        if (pix) {
            GtkWidget *img = gtk_image_new_from_pixbuf(pix);
            g_object_unref(pix);
            gtk_box_pack_start(GTK_BOX(top), img, FALSE, FALSE, 0);
        }
    }

    GtkWidget *label = gtk_label_new(NULL);
    gchar *name_esc = g_markup_escape_text(e->label, -1);
    gchar *markup;
    if (e->sublabel[0] && strcmp(e->sublabel, e->label) != 0) {
        gchar *sub_esc = g_markup_escape_text(e->sublabel, -1);
        markup = g_strdup_printf("%s\n<small>%s</small>", name_esc, sub_esc);
        g_free(sub_esc);
    } else {
        markup = g_strdup(name_esc);
    }
    g_free(name_esc);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.5f);
    style_fg(label);
    gtk_box_pack_start(GTK_BOX(top), label, TRUE, TRUE, 0);

    if (e->kind == PULSE_SINK_INPUT || e->kind == PULSE_SOURCE_OUTPUT) {
        row->device_combo = build_device_combo(e, all_entries);
        gtk_box_pack_start(GTK_BOX(top), row->device_combo, FALSE, FALSE, 0);
    }

    row->mute = gtk_toggle_button_new_with_label("Mudo");
    gtk_box_pack_start(GTK_BOX(top), row->mute, FALSE, FALSE, 0);

    row->scale = gtk_hscale_new_with_range(0, AUDIO_VOLUME_MAX, 1);
    gtk_scale_set_digits(GTK_SCALE(row->scale), 0);
    gtk_scale_set_value_pos(GTK_SCALE(row->scale), GTK_POS_RIGHT);
    /* Arrow keys and clicks in the trough move by the same configured
     * amount a scroll notch does, so every way of nudging a slider
     * agrees. (Scroll itself is handled by on_scale_scroll(), which
     * bypasses these increments entirely -- see its comment.) */
    gtk_range_set_increments(GTK_RANGE(row->scale), scroll_step(), scroll_step() * 2);
    style_fg(row->scale);
    gtk_box_pack_start(GTK_BOX(box), row->scale, FALSE, FALSE, 0);

    if (e->kind == PULSE_SINK || e->kind == PULSE_SOURCE) {
        GtkWidget *bottom = gtk_hbox_new(FALSE, 8);
        row->use_default = gtk_check_button_new_with_label("Padrão");
        style_fg(row->use_default);
        style_fg(gtk_bin_get_child(GTK_BIN(row->use_default)));
        gtk_box_pack_start(GTK_BOX(bottom), row->use_default, FALSE, FALSE, 0);

        row->active = gtk_check_button_new_with_label("Ativo");
        style_fg(row->active);
        style_fg(gtk_bin_get_child(GTK_BIN(row->active)));
        gtk_box_pack_start(GTK_BOX(bottom), row->active, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(box), bottom, FALSE, FALSE, 0);
    }

    /* Values first, handlers after -- otherwise setting the initial
     * state would fire each handler and echo it straight back to pactl
     * as if the user had done it. (g_updating guards the same hazard
     * during the poll's in-place updates, where the handlers are already
     * connected.) */
    gtk_range_set_value(GTK_RANGE(row->scale), e->volume_pct);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->mute), e->muted);
    if (row->use_default) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->use_default), e->is_default);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->active), !e->suspended);
    }

    g_signal_connect(row->scale, "value-changed", G_CALLBACK(on_scale_changed), row);
    g_signal_connect(row->scale, "button-press-event", G_CALLBACK(on_scale_press), NULL);
    g_signal_connect(row->scale, "button-release-event", G_CALLBACK(on_scale_release), NULL);
    g_signal_connect(row->scale, "scroll-event", G_CALLBACK(on_scale_scroll), NULL);
    g_signal_connect(row->mute, "toggled", G_CALLBACK(on_mute_toggled), row);
    if (row->device_combo) {
        g_signal_connect(row->device_combo, "changed", G_CALLBACK(on_device_combo_changed), row);
        g_signal_connect(row->device_combo, "notify::popup-shown", G_CALLBACK(on_device_combo_popup_notify), NULL);
    }
    if (row->use_default) {
        g_signal_connect(row->use_default, "toggled", G_CALLBACK(on_default_toggled), row);
        g_signal_connect(row->active, "toggled", G_CALLBACK(on_active_toggled), row);
    }
    return box;
}

/* Identifies *which objects exist*, not their values -- the poll uses a
 * change here to mean "rebuild", and no change to mean "just refresh
 * the values in place". Deliberately excludes volume/mute so a volume
 * change made elsewhere doesn't trigger a full rebuild. */
static void snapshot_signature(GPtrArray *entries, char *out, size_t outsz)
{
    size_t o = 0;
    out[0] = 0;
    for (guint i = 0; i < entries->len && o + 1 < outsz; i++) {
        const PulseEntry *e = g_ptr_array_index(entries, i);
        int n = snprintf(out + o, outsz - o, "%d:%d:%s:%s;", (int)e->kind, e->index, e->name, e->label);
        if (n < 0) {
            break;
        }
        o += (size_t)n;
    }
}

/* Placeholder shown when there's no sound server to talk to. ALSA-only
 * systems land here: `pactl` needs a PulseAudio-protocol server, and
 * bare ALSA has no notion of per-application streams at all (that
 * concept is precisely what a sound server introduces), so a mixer page
 * has nothing to show even where ALSA itself is working fine. Saying so
 * explicitly beats an empty window that reads as a bug. */
static GtkWidget *unavailable_placeholder(void)
{
    gboolean has_alsa = access("/proc/asound/cards", R_OK) == 0;
    GtkWidget *label = gtk_label_new(NULL);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_markup(GTK_LABEL(label),
                          has_alsa ? "<b>Nenhum servidor de áudio</b>\n\n"
                                     "O ALSA está presente, mas o PulseAudio/PipeWire não está respondendo.\n\n"
                                     "O mixer por aplicativo depende de um servidor de áudio: o ALSA sozinho "
                                     "não separa o som por programa."
                                   : "<b>Nenhum servidor de áudio</b>\n\n"
                                     "Instale o <tt>pactl</tt> (pulseaudio-utils ou pipewire-pulse) "
                                     "para usar esta tela.");
    gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.0f);
    style_fg(label);
    return label;
}

static void add_section(const char *title, GPtrArray *entries, PulseKind kind, gboolean *any)
{
    gboolean header_done = FALSE;
    for (guint i = 0; i < entries->len; i++) {
        PulseEntry *e = g_ptr_array_index(entries, i);
        if (e->kind != kind) {
            continue;
        }
        if (!header_done) {
            if (*any) {
                gtk_box_pack_start(GTK_BOX(g_rows_box), gtk_hseparator_new(), FALSE, FALSE, 4);
            }
            gtk_box_pack_start(GTK_BOX(g_rows_box), section_header(title), FALSE, FALSE, 0);
            header_done = TRUE;
            *any = TRUE;
        }
        gtk_box_pack_start(GTK_BOX(g_rows_box), build_row(e, entries), FALSE, FALSE, 0);
    }
}

static void clear_rows(void)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_rows_box));
    for (GList *l = children; l; l = l->next) {
        gtk_widget_destroy(GTK_WIDGET(l->data));
    }
    g_list_free(children);
}

static void rebuild(void)
{
    clear_rows();

    /* Re-probed rather than trusted from startup, so a server started
     * after xisserve was already running is picked up on the next open
     * instead of needing a restart. */
    pulse_invalidate_available();

    GPtrArray *entries = pulse_list();
    snapshot_signature(entries, g_signature, sizeof(g_signature));

    if (!pulse_available()) {
        gtk_box_pack_start(GTK_BOX(g_rows_box), unavailable_placeholder(), FALSE, FALSE, 0);
    } else {
        g_updating = TRUE;
        gboolean any = FALSE;
        add_section("Reproduzindo", entries, PULSE_SINK_INPUT, &any);
        add_section("Gravando", entries, PULSE_SOURCE_OUTPUT, &any);
        add_section("Saída", entries, PULSE_SINK, &any);
        add_section("Entrada", entries, PULSE_SOURCE, &any);
        g_updating = FALSE;

        if (!any) {
            GtkWidget *label = gtk_label_new("Nenhum dispositivo de áudio encontrado.");
            gtk_misc_set_alignment(GTK_MISC(label), 0.0f, 0.0f);
            style_fg(label);
            gtk_box_pack_start(GTK_BOX(g_rows_box), label, FALSE, FALSE, 0);
        }
    }

    pulse_entries_free(entries);
    gtk_widget_show_all(g_rows_box);
}

/* Value-only refresh: walks the rows already on screen and re-syncs each
 * one's widgets from a fresh snapshot, matching rows to entries by
 * kind+index. Never adds or removes a row -- rebuild() handles that when
 * the signature changes. */
static void refresh_values(GPtrArray *entries)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(g_rows_box));
    g_updating = TRUE;
    for (GList *l = children; l; l = l->next) {
        AudioRow *row = g_object_get_data(G_OBJECT(l->data), "audio-row");
        if (!row) {
            continue; /* a section header or separator */
        }
        for (guint i = 0; i < entries->len; i++) {
            PulseEntry *e = g_ptr_array_index(entries, i);
            if (e->kind != row->entry.kind) {
                continue;
            }
            /* Devices are identified by name, not index: a name is
             * stable for the life of the device, while indices are
             * handed out fresh and can shift when devices come and go.
             * Streams have no name, so they match on index -- fine,
             * since a stream that goes away takes its row with it via a
             * signature change rather than being re-matched. */
            gboolean is_device = e->kind == PULSE_SINK || e->kind == PULSE_SOURCE;
            if (is_device ? strcmp(e->name, row->entry.name) != 0 : e->index != row->entry.index) {
                continue;
            }
            row->entry = *e;
            gtk_range_set_value(GTK_RANGE(row->scale), e->volume_pct);
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->mute), e->muted);
            if (row->use_default) {
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->use_default), e->is_default);
                gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(row->active), !e->suspended);
            }
            sync_device_combo(row, entries);
            break;
        }
    }
    g_updating = FALSE;
    g_list_free(children);
}

static gboolean on_poll(gpointer data)
{
    (void)data;
    if (g_dragging || g_get_monotonic_time() - g_last_action_us < AUDIO_SETTLE_US) {
        return TRUE;
    }

    GPtrArray *entries = pulse_list();
    char sig[sizeof(g_signature)];
    snapshot_signature(entries, sig, sizeof(sig));
    if (strcmp(sig, g_signature) != 0) {
        pulse_entries_free(entries);
        rebuild();
        return TRUE;
    }
    refresh_values(entries);
    pulse_entries_free(entries);
    return TRUE;
}

/* ---- page interface ------------------------------------------------------ */

GtkWidget *page_audio_build(void)
{
    g_rows_box = gtk_vbox_new(FALSE, 6);

    g_root = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(g_root), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_add_with_viewport(GTK_SCROLLED_WINDOW(g_root), g_rows_box);

    /* The window paints its own --bg (xisserve.c's on_window_expose) and
     * most widgets are windowless, so they inherit it -- but a viewport
     * has a real GdkWindow of its own and would otherwise paint the GTK
     * theme's background over it, framing the page in a foreign color. */
    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(g_root));
    gtk_viewport_set_shadow_type(GTK_VIEWPORT(viewport), GTK_SHADOW_NONE);
    double r, g, b, a;
    xisserve_get_bg_rgba(&r, &g, &b, &a);
    GdkColor bg = {0, (guint16)(r * 65535), (guint16)(g * 65535), (guint16)(b * 65535)};
    gtk_widget_modify_bg(viewport, GTK_STATE_NORMAL, &bg);

    return g_root;
}

void page_audio_on_show(void)
{
    g_last_action_us = 0;
    g_dragging = FALSE;
    rebuild();
    if (!g_poll_id) {
        g_poll_id = g_timeout_add(AUDIO_POLL_MS, on_poll, NULL);
    }
}

/* Stopping the poll on hide matters more here than for a typical page:
 * each tick runs four `pactl list` subprocesses, which is fine a couple
 * of times a second while someone is looking at the mixer and pure
 * waste for a popup that spends most of its life hidden. */
void page_audio_on_hide(void)
{
    if (g_poll_id) {
        g_source_remove(g_poll_id);
        g_poll_id = 0;
    }
}

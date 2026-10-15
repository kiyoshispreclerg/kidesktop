/* See hotcorner.h. */
#include "hotcorner.h"
#include "effect.h"
#include "window.h"
#include "animation.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

CompHotcornerConfig comp_hotcorners;

static const char *const corner_keys[HOTCORNER_CORNERS] = {
    "top-left", "top-right", "bottom-left", "bottom-right",
};

typedef struct {
    xcb_window_t win;                 /* XCB_NONE: slot unused */
    const CompEffectInstance *inst;   /* what it activates */
    double due;                       /* dwell deadline (ms), <0 = none */
    bool inside;
    bool fired;                       /* once per visit */
} Corner;

static Corner corners[HOTCORNER_MAX];
static int corner_count;
static bool stack_dirty;

#define CORNER_EVENT_MASK (XCB_EVENT_MASK_ENTER_WINDOW | XCB_EVENT_MASK_LEAVE_WINDOW)

void hotcorner_config_defaults(void)
{
    memset(&comp_hotcorners, 0, sizeof(comp_hotcorners));
    comp_hotcorners.delay_ms = 150;
    comp_hotcorners.size = 1;
}

/* `output` is NULL for [hotcorners], the output's name for
 * [hotcorners:NAME]. */
bool hotcorner_config_key(const char *output, const char *key, const char *value)
{
    for (int i = 0; i < HOTCORNER_CORNERS; i++) {
        if (strcmp(key, corner_keys[i]))
            continue;
        if (!strcasecmp(value, "none") || !strcasecmp(value, "off"))
            value = "";

        HotcornerRule *r = &comp_hotcorners.base;
        if (output) {
            r = NULL;
            for (int k = 0; k < comp_hotcorners.n_rules; k++)
                if (!strcmp(comp_hotcorners.rules[k].name, output))
                    r = &comp_hotcorners.rules[k];
            if (!r) {
                if (comp_hotcorners.n_rules >= HOTCORNER_RULES) {
                    fprintf(stderr, "kicomp: config: too many [hotcorners:*] sections\n");
                    return true;
                }
                r = &comp_hotcorners.rules[comp_hotcorners.n_rules++];
                memset(r, 0, sizeof(*r));
                snprintf(r->name, sizeof(r->name), "%s", output);
            }
        }
        snprintf(r->action[i], sizeof(r->action[i]), "%s", value);
        r->set[i] = true;
        return true;
    }
    if (!output && !strcmp(key, "delay")) {
        int v = atoi(value);
        comp_hotcorners.delay_ms = v < 0 ? 0 : v > 2000 ? 2000 : v;
        return true;
    }
    if (!output && !strcmp(key, "size")) {
        int v = atoi(value);
        comp_hotcorners.size = v < 1 ? 1 : v > 32 ? 32 : v;
        return true;
    }
    return false;
}

/* What this output's corner does: its own [hotcorners:NAME] line if it
 * has one for that corner, otherwise the [hotcorners] default. */
static const char *action_for(const CompOutput *o, int corner)
{
    for (int k = 0; k < comp_hotcorners.n_rules; k++) {
        const HotcornerRule *r = &comp_hotcorners.rules[k];
        if (!strcmp(r->name, o->name) && r->set[corner])
            return r->action[corner];
    }
    return comp_hotcorners.base.action[corner];
}

static void corner_geometry(const CompOutput *o, int i, int *x, int *y)
{
    int s = comp_hotcorners.size;
    *x = (i & 1) ? o->rect.x + o->rect.w - s : o->rect.x;
    *y = (i & 2) ? o->rect.y + o->rect.h - s : o->rect.y;
}

static bool is_ours(xcb_window_t id)
{
    for (int i = 0; i < corner_count; i++)
        if (corners[i].win == id)
            return true;
    return false;
}

uint32_t hotcorner_event_mask(xcb_window_t id)
{
    return is_ours(id) ? CORNER_EVENT_MASK : 0;
}

static void create_corners(void)
{
    corner_count = 0;
    for (int oi = 0; oi < comp.output_count; oi++) {
        const CompOutput *o = &comp.outputs[oi];
        for (int i = 0; i < HOTCORNER_CORNERS; i++) {
            const char *name = action_for(o, i);
            if (!name[0] || corner_count >= HOTCORNER_MAX)
                continue;

            const CompEffectInstance *inst = effect_base_instance(name);
            if (!inst || !inst->module->activate) {
                fprintf(stderr, "kicomp: config: [hotcorners] %s: '%s' cannot be "
                                "activated from a corner\n", corner_keys[i], name);
                continue;
            }
            if (!inst->enabled) {
                fprintf(stderr, "kicomp: config: [hotcorners] %s: effect '%s' is "
                                "disabled\n", corner_keys[i], name);
                continue;
            }

            int x, y;
            corner_geometry(o, i, &x, &y);
            xcb_window_t win = xcb_generate_id(comp.conn);
            uint32_t values[] = {
                1,   /* override_redirect: the WM must not frame it */
                CORNER_EVENT_MASK,
            };
            xcb_create_window(comp.conn, 0, win, comp.root, (int16_t)x, (int16_t)y,
                              (uint16_t)comp_hotcorners.size,
                              (uint16_t)comp_hotcorners.size, 0,
                              XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT,
                              XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, values);
            xcb_map_window(comp.conn, win);

            Corner *c = &corners[corner_count++];
            memset(c, 0, sizeof(*c));
            c->win = win;
            c->inst = inst;
            c->due = -1.0;
            comp_log("hot corner %s of %s -> %s (%d ms)", corner_keys[i], o->name,
                     inst->name, comp_hotcorners.delay_ms);
        }
    }
    xcb_flush(comp.conn);
    stack_dirty = true;
}

static void destroy_corners(void)
{
    for (int i = 0; i < corner_count; i++)
        xcb_destroy_window(comp.conn, corners[i].win);
    corner_count = 0;
}

void hotcorner_init(void)
{
    create_corners();
    if (corner_count)
        comp_info("hot corners: %d", corner_count);
}

void hotcorner_shutdown(void)
{
    destroy_corners();
}

/* The outputs changed (or moved): the corners are rebuilt where they now are. */
void hotcorner_relayout(void)
{
    destroy_corners();
    create_corners();
}

void hotcorner_note_restack(xcb_window_t id)
{
    if (!is_ours(id))
        stack_dirty = true;
}

bool hotcorner_handle_event(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & 0x7f;
    if (type != XCB_ENTER_NOTIFY && type != XCB_LEAVE_NOTIFY)
        return false;

    /* Same layout for both. */
    xcb_enter_notify_event_t *e = (xcb_enter_notify_event_t *)ev;
    Corner *c = NULL;
    for (int i = 0; i < corner_count; i++)
        if (corners[i].win == e->event)
            c = &corners[i];
    if (!c)
        return false;

    if (type == XCB_LEAVE_NOTIFY) {
        /* Any kind of leave, grabs included: a mode that grabs the
         * pointer ends the visit, which is what lets the corner toggle
         * it off again after the user has moved away and back. */
        c->inside = false;
        c->fired = false;
        c->due = -1.0;
        return true;
    }

    /* Only a real arrival. The Enter that comes when a grab ends with
     * the pointer still sitting in the corner is not the user touching
     * it again. */
    if (e->mode != XCB_NOTIFY_MODE_NORMAL)
        return true;

    c->inside = true;
    comp_log("hot corner entered (dwell %d ms)", comp_hotcorners.delay_ms);
    if (c->fired)
        return true;

    if (comp_hotcorners.delay_ms <= 0) {
        c->fired = true;
        c->inst->module->activate(c->inst);
    } else {
        c->due = comp_now_ms() + comp_hotcorners.delay_ms;
    }
    return true;
}

static bool covered(void)
{
    /* comp.stack is bottom-most first: the last window is the top. */
    CompWindow *top = NULL;
    for (CompWindow *w = comp.stack; w; w = w->next)
        top = w;
    return top && !is_ours(top->id);
}

void hotcorner_tick(double now)
{
    if (stack_dirty) {
        stack_dirty = false;
        if (covered()) {
            uint32_t above = XCB_STACK_MODE_ABOVE;
            for (int i = 0; i < corner_count; i++)
                xcb_configure_window(comp.conn, corners[i].win,
                                         XCB_CONFIG_WINDOW_STACK_MODE, &above);
            xcb_flush(comp.conn);
        }
    }

    for (int i = 0; i < corner_count; i++) {
        Corner *c = &corners[i];
        if (c->due >= 0.0 && c->due <= now) {
            c->due = -1.0;
            if (c->inside && !c->fired) {
                c->fired = true;
                c->inst->module->activate(c->inst);
            }
        }
    }
}

int hotcorner_timeout_ms(double now)
{
    double best = -1.0;
    for (int i = 0; i < corner_count; i++)
        if (corners[i].due >= 0.0 && (best < 0.0 || corners[i].due < best))
            best = corners[i].due;
    if (best < 0.0)
        return -1;
    double d = best - now;
    return d <= 0.0 ? 0 : (int)d + 1;
}

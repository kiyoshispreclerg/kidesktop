/* See input.h. */
#include "input.h"
#include "animation.h"

#include <xcb/xcb_keysyms.h>
#include <X11/Xlib.h>          /* XStringToKeysym: a pure string lookup */
#include <X11/keysym.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define MAX_HOTKEYS 8

typedef struct {
    xcb_keycode_t keycode;
    uint16_t modifiers;
    void (*fn)(void *data);
    void *data;
} Hotkey;

static xcb_key_symbols_t *symbols;
static Hotkey hotkeys[MAX_HOTKEYS];
static int hotkey_count;

static const CompInputHandler *grab_handler;
static void *grab_data;
static double grab_ended_ms;

/* The modifier bits a hotkey should ignore. Caps Lock and Num Lock are
 * *states*, not modifiers anyone means when they write "Meta+W", so the
 * key is grabbed once for each combination of them -- the standard dance,
 * and the reason a hotkey that works normally stops working the moment
 * Num Lock is on if you skip it. */
static const uint16_t ignored_masks[] = {
    0,
    XCB_MOD_MASK_LOCK,
    XCB_MOD_MASK_2,
    XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2,
};
#define N_IGNORED ((int)(sizeof(ignored_masks) / sizeof(ignored_masks[0])))

bool input_init(void)
{
    if (symbols)
        return true;
    symbols = xcb_key_symbols_alloc(comp.conn);
    return symbols != NULL;
}

void input_shutdown(void)
{
    input_release();

    for (int i = 0; i < hotkey_count; i++)
        for (int m = 0; m < N_IGNORED; m++)
            xcb_ungrab_key(comp.conn, hotkeys[i].keycode, comp.root,
                           (uint16_t)(hotkeys[i].modifiers | ignored_masks[m]));
    hotkey_count = 0;

    if (symbols) {
        xcb_key_symbols_free(symbols);
        symbols = NULL;
    }
}

/* "Meta+Shift+W" -> a modifier mask and a keysym. The names are the ones
 * xiskeys.conf and xispanel.conf already use, so a user who has written
 * one hotkey has written them all. */
static bool parse_spec(const char *spec, uint16_t *mods_out, xcb_keysym_t *sym_out)
{
    uint16_t mods = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);

    char *save = NULL;
    char *last = NULL;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
        while (*tok == ' ')
            tok++;
        if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control"))
            mods |= XCB_MOD_MASK_CONTROL;
        else if (!strcasecmp(tok, "Shift"))
            mods |= XCB_MOD_MASK_SHIFT;
        else if (!strcasecmp(tok, "Alt"))
            mods |= XCB_MOD_MASK_1;
        else if (!strcasecmp(tok, "Meta") || !strcasecmp(tok, "Super") ||
                 !strcasecmp(tok, "Win"))
            mods |= XCB_MOD_MASK_4;
        else
            last = tok;
    }

    if (!last)
        return false;

    KeySym sym = XStringToKeysym(last);
    /* A bare letter is written as its capital in the keysym tables. */
    if (sym == NoSymbol && strlen(last) == 1) {
        char up[2] = { (char)toupper((unsigned char)last[0]), 0 };
        sym = XStringToKeysym(up);
    }
    if (sym == NoSymbol)
        return false;

    *mods_out = mods;
    *sym_out = (xcb_keysym_t)sym;
    return true;
}

bool input_bind_hotkey(const char *spec, void (*fn)(void *data), void *data)
{
    if (!spec || !*spec || !fn)
        return false;
    if (!input_init())
        return false;
    if (hotkey_count >= MAX_HOTKEYS)
        return false;

    uint16_t mods = 0;
    xcb_keysym_t sym = 0;
    if (!parse_spec(spec, &mods, &sym)) {
        fprintf(stderr, "kicomp: hotkey '%s': don't understand it\n", spec);
        return false;
    }

    xcb_keycode_t *codes = xcb_key_symbols_get_keycode(symbols, sym);
    if (!codes || codes[0] == XCB_NO_SYMBOL) {
        free(codes);
        fprintf(stderr, "kicomp: hotkey '%s': no key on this keyboard\n", spec);
        return false;
    }
    xcb_keycode_t code = codes[0];
    free(codes);

    bool ok = true;
    for (int m = 0; m < N_IGNORED; m++) {
        xcb_generic_error_t *err = xcb_request_check(comp.conn,
            xcb_grab_key_checked(comp.conn, 1, comp.root,
                                 (uint16_t)(mods | ignored_masks[m]), code,
                                 XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC));
        if (err) {
            free(err);
            ok = false;
        }
    }
    if (!ok) {
        fprintf(stderr, "kicomp: hotkey '%s': already taken by another program\n",
                spec);
        return false;
    }

    hotkeys[hotkey_count].keycode = code;
    hotkeys[hotkey_count].modifiers = mods;
    hotkeys[hotkey_count].fn = fn;
    hotkeys[hotkey_count].data = data;
    hotkey_count++;

    comp_info("hotkey %s", spec);
    return true;
}

bool input_grab(const CompInputHandler *handler, void *data)
{
    if (grab_handler || !handler)
        return false;

    xcb_grab_keyboard_reply_t *kb = xcb_grab_keyboard_reply(comp.conn,
        xcb_grab_keyboard(comp.conn, 0, comp.root, XCB_CURRENT_TIME,
                          XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC), NULL);
    bool have_kb = kb && kb->status == XCB_GRAB_STATUS_SUCCESS;
    free(kb);
    if (!have_kb) {
        /* Someone is already holding it -- a menu is open, a drag is in
         * progress. Starting a mode that cannot receive keys would leave
         * the user looking at a grid they can't dismiss. */
        fprintf(stderr, "kicomp: cannot take the keyboard right now\n");
        return false;
    }

    xcb_grab_pointer_reply_t *pt = xcb_grab_pointer_reply(comp.conn,
        xcb_grab_pointer(comp.conn, 0, comp.root,
                         XCB_EVENT_MASK_BUTTON_PRESS |
                         XCB_EVENT_MASK_BUTTON_RELEASE |
                         XCB_EVENT_MASK_POINTER_MOTION,
                         XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC,
                         XCB_NONE, XCB_NONE, XCB_CURRENT_TIME), NULL);
    bool have_pt = pt && pt->status == XCB_GRAB_STATUS_SUCCESS;
    free(pt);
    if (!have_pt) {
        /* The keyboard alone is enough to drive a mode; the pointer is a
         * convenience. Said out loud so a mode that behaves oddly under
         * an existing pointer grab is explainable. */
        comp_log("pointer grab refused; keyboard only");
    }

    grab_handler = handler;
    grab_data = data;
    xcb_flush(comp.conn);
    return true;
}

void input_release(void)
{
    if (!grab_handler)
        return;
    grab_ended_ms = comp_now_ms();
    grab_handler = NULL;
    grab_data = NULL;
    xcb_ungrab_keyboard(comp.conn, XCB_CURRENT_TIME);
    xcb_ungrab_pointer(comp.conn, XCB_CURRENT_TIME);
    xcb_flush(comp.conn);
}

bool input_grabbed(void)
{
    return grab_handler != NULL;
}

double input_mode_ended_ms(void)
{
    return grab_ended_ms;
}

bool input_pointer_position(int *root_x, int *root_y)
{
    xcb_query_pointer_reply_t *r = xcb_query_pointer_reply(comp.conn,
        xcb_query_pointer(comp.conn, comp.root), NULL);
    if (!r)
        return false;
    if (root_x)
        *root_x = r->root_x;
    if (root_y)
        *root_y = r->root_y;
    free(r);
    return true;
}

/* What a keypress produces as text, if anything. Deliberately small: the
 * keysym's Latin-1 range, which is what a filter box needs and all that
 * can be got without an input method. Anything else -- dead keys,
 * composition, other scripts -- types nothing rather than typing
 * something wrong, and the mode is still perfectly usable with the
 * arrows. */
static void keysym_text(xcb_keysym_t sym, uint16_t mods, char *out, size_t outsz)
{
    out[0] = '\0';

    unsigned long ch = 0;
    if (sym >= 0x20 && sym <= 0x7e)
        ch = sym;
    else if (sym >= 0xa0 && sym <= 0xff)
        ch = sym;
    else
        return;

    /* The keysym tables give the unshifted letter; shift makes it
     * uppercase, and Caps Lock is handled the same way round. */
    if (ch >= 'a' && ch <= 'z' && (mods & XCB_MOD_MASK_SHIFT))
        ch = (unsigned long)toupper((int)ch);
    else if (ch >= 'A' && ch <= 'Z' && !(mods & XCB_MOD_MASK_SHIFT))
        ch = (unsigned long)tolower((int)ch);

    if (ch < 0x80) {
        if (outsz < 2)
            return;
        out[0] = (char)ch;
        out[1] = '\0';
    } else if (outsz >= 3) {
        /* Latin-1 as UTF-8, two bytes. */
        out[0] = (char)(0xc0 | (ch >> 6));
        out[1] = (char)(0x80 | (ch & 0x3f));
        out[2] = '\0';
    }
}

bool input_handle_event(xcb_generic_event_t *ev)
{
    uint8_t type = ev->response_type & 0x7f;

    if (type == XCB_KEY_PRESS) {
        xcb_key_press_event_t *e = (xcb_key_press_event_t *)ev;
        xcb_keysym_t sym = symbols
            ? xcb_key_symbols_get_keysym(symbols, e->detail,
                                         (e->state & XCB_MOD_MASK_SHIFT) ? 1 : 0)
            : 0;

        /* Hotkeys are matched first, grab or no grab: the key that opens
         * a mode is the key that closes it. While the mode holds the
         * keyboard the server delivers the key here rather than to the
         * root grab, so without this the combination would be typed into
         * whatever the mode does with text -- pressing Meta+A again would
         * put an "a" in the filter box and leave the grid up. */
        uint16_t state = e->state & (uint16_t)~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2);
        for (int i = 0; i < hotkey_count; i++) {
            if (hotkeys[i].keycode == e->detail &&
                hotkeys[i].modifiers == state) {
                hotkeys[i].fn(hotkeys[i].data);
                return true;
            }
        }

        if (grab_handler) {
            char text[8];
            keysym_text(sym, e->state, text, sizeof(text));
            if (grab_handler->key)
                grab_handler->key(grab_data, sym, text, e->state);
            return true;
        }
        return false;
    }

    if (!grab_handler)
        return false;

    switch (type) {
    case XCB_KEY_RELEASE:
        return true;
    case XCB_MOTION_NOTIFY: {
        xcb_motion_notify_event_t *e = (xcb_motion_notify_event_t *)ev;
        if (grab_handler->motion)
            grab_handler->motion(grab_data, e->root_x, e->root_y);
        return true;
    }
    case XCB_BUTTON_PRESS: {
        xcb_button_press_event_t *e = (xcb_button_press_event_t *)ev;
        if (grab_handler->button)
            grab_handler->button(grab_data, e->root_x, e->root_y, e->detail, true);
        return true;
    }
    case XCB_BUTTON_RELEASE: {
        xcb_button_release_event_t *e = (xcb_button_release_event_t *)ev;
        if (grab_handler->button)
            grab_handler->button(grab_data, e->root_x, e->root_y, e->detail, false);
        return true;
    }
    default:
        break;
    }
    return false;
}

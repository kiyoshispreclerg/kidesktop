/*
 * power.h - backend for the --energy page: battery/AC/peripheral status
 * (via `upower`), screen backlight (via `brightnessctl`), and night
 * light (via `xsct`, https://github.com/faf0/sct).
 *
 * Same reasoning as pulse.h: shell out to the CLI tool rather than link
 * a client library (libupower-glib, or hand-rolling RandR gamma ramps
 * for night light), so nothing here is a build-time dependency and there
 * is nothing to ifdef. Each of the three backends is probed and cached
 * independently -- a system can have `upower` but not `xsct`, say -- and
 * every *_available() has its own *_invalidate_available() so a tool
 * installed after xisserve started is picked up on the page's next open
 * instead of needing a restart, exactly like pulse_invalidate_available().
 *
 * Every invocation forces LC_ALL=C for the same reason pulse.c does: a
 * pt_BR session prints "Estado: descarregando" instead of "state:
 * discharging", which this parses.
 */
#ifndef XISSERVE_POWER_H
#define XISSERVE_POWER_H

#include <glib.h>

typedef enum {
    POWER_STATE_UNKNOWN,
    POWER_STATE_CHARGING,
    POWER_STATE_DISCHARGING,
    POWER_STATE_FULL,
} PowerState;

typedef struct {
    gboolean present;      /* FALSE = no system battery (desktop/VM); rest is meaningless */
    PowerState state;
    int percentage;        /* 0-100 */
    char time_text[64];    /* upower's own "N.M hours" line, raw, "" if it didn't report one */
} PowerBattery;

/* One non-system battery UPower knows about: wireless mouse/keyboard,
 * a Bluetooth headset, a UPS, etc. -- everything `upower -e` lists other
 * than DisplayDevice (the system battery aggregate power_available()
 * and power_get_battery() read) and anything under a battery_BAT* path
 * (the real system battery, when DisplayDevice itself doesn't already
 * cover it). */
typedef struct {
    char label[128];  /* upower's "model:", falling back to the native-path */
    PowerState state;
    int percentage;
} PowerPeripheral;

/* 1 if `upower` exists and its daemon answers. Probed once and cached. */
int power_available(void);
void power_invalidate_available(void);

/* TRUE = on AC / no battery at all (nothing to run out); FALSE = running
 * on battery power right now. Meaningless (returns TRUE) if
 * !power_available(). */
gboolean power_ac_online(void);

/* Fills `out` from DisplayDevice, upower's own synthetic aggregate of
 * whatever real batteries the system has. out->present is FALSE (rest
 * zeroed) on a system with no battery at all -- every desktop and most
 * VMs, not just this one. */
void power_get_battery(PowerBattery *out);

/* Every other UPower device with a battery -- see PowerPeripheral's own
 * comment. Returns a GPtrArray of owned PowerPeripheral*, never NULL.
 * Free with power_peripherals_free(). */
GPtrArray *power_list_peripherals(void);
void power_peripherals_free(GPtrArray *peripherals);

/* 1 if a real backlight device is reachable via `brightnessctl -c
 * backlight` -- deliberately narrower than "brightnessctl is installed":
 * on a desktop/VM with no panel to dim, brightnessctl falls back to
 * whatever keyboard-LED or other non-backlight class it finds, which
 * this page has no business offering as "screen brightness". */
int brightness_available(void);
void brightness_invalidate_available(void);

/* 0-100, or -1 if unavailable/unreadable. */
int brightness_get_pct(void);
void brightness_set_pct(int pct);

/* 1 if `xsct` is on $PATH. */
int nightlight_available(void);
void nightlight_invalidate_available(void);

/* Sets the screen's color temperature in Kelvin (roughly 1000-10000;
 * xsct clamps to its own supported range). temp <= 0 resets to the
 * display's native/day temperature (xsct with no arguments). */
void nightlight_apply(int temp);

#endif

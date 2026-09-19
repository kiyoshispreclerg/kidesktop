/*
 * power.c - see power.h. Three independent CLI backends behind one file:
 * `upower` for battery/AC/peripherals, `brightnessctl` for the screen
 * backlight, `xsct` for night light.
 */
#include "power.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void rstrip(char *s)
{
    size_t l = strlen(s);
    while (l > 0 && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) {
        s[--l] = 0;
    }
}

/* "  key:          value" (upower -i's own field indentation, one or two
 * levels deep -- see the block comment on power_list_peripherals()) ->
 * value, trimmed. NULL if `line`'s key doesn't match. */
static const char *field_value(const char *line, const char *key)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    size_t keylen = strlen(key);
    if (strncmp(p, key, keylen) != 0) {
        return NULL;
    }
    p += keylen;
    if (*p != ':') {
        return NULL;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    return p;
}

static PowerState parse_state(const char *value)
{
    if (!strncmp(value, "charging", 8)) {
        return POWER_STATE_CHARGING;
    }
    if (!strncmp(value, "discharging", 11)) {
        return POWER_STATE_DISCHARGING;
    }
    if (!strncmp(value, "fully-charged", 13)) {
        return POWER_STATE_FULL;
    }
    return POWER_STATE_UNKNOWN;
}

/* ---- upower: battery / AC / peripherals ----------------------------- */

static int g_power_checked = 0;
static int g_power_available = 0;

void power_invalidate_available(void)
{
    g_power_checked = 0;
}

int power_available(void)
{
    if (g_power_checked) {
        return g_power_available;
    }
    g_power_checked = 1;
    FILE *f = popen("LC_ALL=C upower -e 2>/dev/null", "r");
    if (!f) {
        g_power_available = 0;
        return 0;
    }
    char buf[256];
    g_power_available = fgets(buf, sizeof(buf), f) != NULL;
    pclose(f);
    return g_power_available;
}

gboolean power_ac_online(void)
{
    FILE *f = popen("LC_ALL=C upower -d 2>/dev/null", "r");
    if (!f) {
        return TRUE;
    }
    gboolean on_battery = FALSE;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *v = field_value(line, "on-battery");
        if (v) {
            on_battery = !strncmp(v, "yes", 3);
            break;
        }
    }
    pclose(f);
    return !on_battery;
}

/* Fills `b` from `upower -i <path>`'s output -- the same field set for
 * DisplayDevice (power_get_battery()) and for a real per-device battery
 * (power_list_peripherals()). Only `state`/`percentage`/`time_text` are
 * used by peripherals; `time_text` is left empty for anything that isn't
 * the main battery (a mouse doesn't get "time to empty" from upower). */
static void parse_device_battery(const char *path, PowerBattery *b)
{
    memset(b, 0, sizeof(*b));
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "LC_ALL=C upower -i '%s' 2>/dev/null", path);
    FILE *f = popen(cmd, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *v;
        if ((v = field_value(line, "present"))) {
            b->present = !strncmp(v, "yes", 3);
        } else if ((v = field_value(line, "state"))) {
            b->state = parse_state(v);
        } else if ((v = field_value(line, "percentage"))) {
            b->percentage = atoi(v);
        } else if ((v = field_value(line, "time to empty")) || (v = field_value(line, "time to full"))) {
            snprintf(b->time_text, sizeof(b->time_text), "%s", v);
            rstrip(b->time_text);
        }
    }
    pclose(f);
}

void power_get_battery(PowerBattery *out)
{
    parse_device_battery("/org/freedesktop/UPower/devices/DisplayDevice", out);
    /* DisplayDevice reports present=no/type=unknown on a system with no
     * real battery (every desktop, most VMs) -- percentage/state from it
     * are noise in that case, not a genuine "0%, discharging". */
    if (!out->present) {
        memset(out, 0, sizeof(*out));
    }
}

/* `upower -e`'s device paths, one per line -- everything power_get_battery()
 * and power_list_peripherals() enumerate over. */
static GPtrArray *list_device_paths(void)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    FILE *f = popen("LC_ALL=C upower -e 2>/dev/null", "r");
    if (!f) {
        return paths;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        rstrip(line);
        if (line[0]) {
            g_ptr_array_add(paths, g_strdup(line));
        }
    }
    pclose(f);
    return paths;
}

/* Model name (falling back to the native-path) for a peripheral row --
 * a separate small scrape since parse_device_battery() only looks at the
 * battery-state fields, and "model:" sits above those in upower -i's
 * output either way. */
static void device_label(const char *path, char *out, size_t outsz)
{
    out[0] = '\0';
    char native_path[128] = "";
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "LC_ALL=C upower -i '%s' 2>/dev/null", path);
    FILE *f = popen(cmd, "r");
    if (!f) {
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        const char *v;
        if ((v = field_value(line, "model")) && *v) {
            snprintf(out, outsz, "%s", v);
            rstrip(out);
            break;
        }
        if ((v = field_value(line, "native-path")) && !native_path[0]) {
            snprintf(native_path, sizeof(native_path), "%s", v);
            rstrip(native_path);
        }
    }
    pclose(f);
    if (!out[0]) {
        snprintf(out, outsz, "%s", native_path[0] ? native_path : path);
    }
}

GPtrArray *power_list_peripherals(void)
{
    GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *paths = list_device_paths();
    for (guint i = 0; i < paths->len; i++) {
        const char *path = g_ptr_array_index(paths, i);
        /* DisplayDevice is the system battery aggregate, read separately
         * by power_get_battery(); a real battery_BAT* device would only
         * duplicate it. */
        if (strstr(path, "DisplayDevice") || strstr(path, "battery_BAT")) {
            continue;
        }
        PowerBattery b;
        parse_device_battery(path, &b);
        if (!b.present) {
            continue;
        }
        PowerPeripheral *p = g_new0(PowerPeripheral, 1);
        device_label(path, p->label, sizeof(p->label));
        p->state = b.state;
        p->percentage = b.percentage;
        g_ptr_array_add(out, p);
    }
    g_ptr_array_free(paths, TRUE);
    return out;
}

void power_peripherals_free(GPtrArray *peripherals)
{
    if (peripherals) {
        g_ptr_array_free(peripherals, TRUE);
    }
}

/* ---- brightnessctl: screen backlight --------------------------------- */

static int g_brightness_checked = 0;
static int g_brightness_available = 0;

void brightness_invalidate_available(void)
{
    g_brightness_checked = 0;
}

/* Restricted to `-c backlight` deliberately -- see power.h. Without it,
 * brightnessctl happily reports a keyboard LED or some other non-panel
 * class on a system with no real backlight device, which this page has
 * no business calling "screen brightness". */
int brightness_available(void)
{
    if (g_brightness_checked) {
        return g_brightness_available;
    }
    g_brightness_checked = 1;
    FILE *f = popen("LC_ALL=C brightnessctl -m -c backlight 2>/dev/null", "r");
    if (!f) {
        g_brightness_available = 0;
        return 0;
    }
    char buf[256];
    g_brightness_available = fgets(buf, sizeof(buf), f) != NULL;
    pclose(f);
    return g_brightness_available;
}

/* -m's machine-readable line: "<device>,<class>,<current>,<percent>%,<max>".
 * Only the percent field is wanted here -- the 4th comma-separated field,
 * with its trailing '%' stripped. */
int brightness_get_pct(void)
{
    FILE *f = popen("LC_ALL=C brightnessctl -m -c backlight 2>/dev/null", "r");
    if (!f) {
        return -1;
    }
    char line[256];
    int pct = -1;
    if (fgets(line, sizeof(line), f)) {
        rstrip(line);
        char *save = NULL;
        char *field = strtok_r(line, ",", &save);
        for (int i = 0; field; i++, field = strtok_r(NULL, ",", &save)) {
            if (i == 3) {
                pct = atoi(field);
                break;
            }
        }
    }
    pclose(f);
    return pct;
}

void brightness_set_pct(int pct)
{
    if (pct < 0) {
        pct = 0;
    }
    if (pct > 100) {
        pct = 100;
    }
    char cmd[128];
    /* -q: brightnessctl otherwise prints the device's new state to
     * stdout on every call, pure noise for a slider that calls this on
     * every drag tick. */
    snprintf(cmd, sizeof(cmd), "LC_ALL=C brightnessctl -q -c backlight set %d%% 2>/dev/null", pct);
    FILE *f = popen(cmd, "r");
    if (f) {
        pclose(f);
    }
}

/* ---- xsct: night light ------------------------------------------------ */

static int g_nightlight_checked = 0;
static int g_nightlight_available = 0;

void nightlight_invalidate_available(void)
{
    g_nightlight_checked = 0;
}

int nightlight_available(void)
{
    if (g_nightlight_checked) {
        return g_nightlight_available;
    }
    g_nightlight_checked = 1;
    char out[PATH_MAX];
    FILE *f = popen("command -v xsct 2>/dev/null", "r");
    if (!f) {
        g_nightlight_available = 0;
        return 0;
    }
    g_nightlight_available = fgets(out, sizeof(out), f) != NULL;
    pclose(f);
    return g_nightlight_available;
}

void nightlight_apply(int temp)
{
    char cmd[64];
    if (temp > 0) {
        snprintf(cmd, sizeof(cmd), "xsct %d >/dev/null 2>&1", temp);
    } else {
        /* No arguments resets xsct's RandR gamma ramp to the display's
         * native/day temperature. */
        snprintf(cmd, sizeof(cmd), "xsct >/dev/null 2>&1");
    }
    FILE *f = popen(cmd, "r");
    if (f) {
        pclose(f);
    }
}

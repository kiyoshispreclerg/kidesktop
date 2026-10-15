/* kt_config.h - kiterm settings: defaults, ~/.config/kiterm.conf, schemes. */
#ifndef KT_CONFIG_H
#define KT_CONFIG_H

#include <gtk/gtk.h>

typedef enum { KT_TABS_TOP, KT_TABS_BOTTOM, KT_TABS_LEFT, KT_TABS_RIGHT } KtTabsPos;

typedef struct {
    char font[128];
    int scrollback;
    double opacity;             /* 0.1 .. 1.0 */
    GdkColor fg, bg, cursor;
    GdkColor palette[16];
    int cursor_shape;           /* VteTerminalCursorShape */
    gboolean cursor_blink;
    gboolean audible_bell;
    gboolean urgent_on_bell;
    gboolean notify_done;       /* sound + tab marker when a background job ends */
    int notify_done_secs;       /* ... but only if it ran at least this long */
    gboolean scroll_on_output;
    gboolean scroll_on_keystroke;
    gboolean confirm_close;     /* ask before closing a tab with a running job */
    KtTabsPos tabs_pos;
    char shell[256];            /* empty = $SHELL */
    char word_chars[64];
    int cols, rows;
} KtConfig;

typedef struct {
    const char *id;
    const char *label;          /* marked N_() */
    const char *fg, *bg, *cursor;
    const char *palette[16];
} KtScheme;

extern const KtScheme kt_schemes[];
extern const int kt_n_schemes;

void kt_config_defaults(KtConfig *c);
void kt_config_apply_scheme(KtConfig *c, const KtScheme *s);
void kt_config_load(KtConfig *c);
gboolean kt_config_save(const KtConfig *c);

#endif

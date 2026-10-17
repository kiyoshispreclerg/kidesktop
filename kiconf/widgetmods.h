/* kiconf - xispanel widget modules, as far as the Paineis tab needs to know.
 *
 * An installable xispanel widget is <type>.so (see
 * ../xispanel/xispanel_widget.h). kiconf never opens it; it reads a
 * <type>.widget file next to it, a GKeyFile that says what the type's
 * options are so the "Adicionar widget" dialog can offer them:
 *
 *   [Widget]
 *   Embeddable=true            may sit in a container popup (default false)
 *
 *   [Field cmd]                one group per WIDGET key=value option, in
 *   Type=string                string | int | bool | enum  (default string)
 *   Name=Command               label; Name[pt_BR]=Comando works as usual
 *   Default=kiremote           as text; bool is yes/no
 *   Min=0   Max=100            int only
 *   Options=a;b;c              enum only
 *
 * Directories are the ones xispanel searches, same precedence: the
 * $XISPANEL_WIDGET_PATH entries, ~/.local/lib/xispanel/widgets, then
 * XISPANEL_WIDGETDIR. A .widget file without its .so beside it is
 * ignored, so an uninstalled module's file doesn't offer a type xispanel
 * would then refuse. Pure GLib, works without a display. */
#ifndef KICONF_WIDGETMODS_H
#define KICONF_WIDGETMODS_H

#include <glib.h>

typedef enum { KWM_STRING, KWM_INT, KWM_BOOL, KWM_ENUM } KiconfWidgetModFieldType;

typedef struct {
    char *key;
    char *label;
    KiconfWidgetModFieldType type;
    char *def;
    double min, max;
    char **opts; /* NULL-terminated, KWM_ENUM only */
} KiconfWidgetModField;

typedef struct {
    char *type;
    int embeddable;
    KiconfWidgetModField *fields;
    int n_fields;
} KiconfWidgetMod;

#define KICONF_WIDGETMOD_MAX_FIELDS 20

/* Scans the module directories; returns a NULL-terminated array that lives
 * for the whole run (nothing to free). `builtin` is a NULL-terminated list
 * of the type names kiconf already knows -- a module can't shadow those
 * (xispanel's built-in registry wins too). */
KiconfWidgetMod **widgetmods_scan(const char *const *builtin, int *count);

#endif

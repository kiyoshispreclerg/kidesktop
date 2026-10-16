/* kiconf - external tab plugins ("modules" in KDE/XFCE speak).
 *
 * A plugin is a <id>.desktop file in <datadir>/kiconf/tabs (user dir
 * first, so it can override a system one). Keys:
 *   Name / Name[xx]        title (localized the usual .desktop way)
 *   Comment / Comment[xx]  tooltip, also searched
 *   Icon                   icon-theme name or absolute path
 *   X-KiConf-Category      appearance|hardware|programs|system, or any
 *                          other text for a section of its own (localizable)
 *   X-KiConf-Keywords      extra search words, ';'-separated
 *   X-KiConf-Module        .so exporting `GtkWidget *kiconf_tab_build(void)`
 *                          (relative to the .desktop, or absolute)
 *   Exec                   alternative to Module: a separate program
 *                          embedded with XEmbed. "%x" is replaced by the
 *                          socket's window id, else "--plug=<id>" is
 *                          appended. See README.md. */
#ifndef KICONF_PLUGINS_H
#define KICONF_PLUGINS_H

#include <gtk/gtk.h>

typedef struct KiconfPlugin {
    char *id;       /* file basename without ".desktop"; --tab matches it */
    char *name;
    char *comment;
    char *icon;
    char *category;
    char *keywords;
    char *module;   /* absolute .so path, or NULL */
    char *exec;     /* command line, or NULL */
    void *handle;   /* GModule once loaded */
} KiconfPlugin;

/* Scans the tab directories; returns a NULL-terminated array (free with
 * nothing -- lives for the whole run). Pure GLib, works without a display. */
KiconfPlugin **plugins_scan(int *count);

/* Builds the plugin's page. Never returns NULL: failures become a label. */
GtkWidget *plugin_build(KiconfPlugin *p);

#endif

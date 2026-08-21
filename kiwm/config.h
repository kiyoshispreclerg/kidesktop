#ifndef KIWM_CONFIG_H
#define KIWM_CONFIG_H

/* Applies built-in defaults to wm's config-backed fields, then loads
 * $XDG_CONFIG_HOME/kiwm.conf (fallback ~/.config/kiwm.conf), writing a
 * default file first if none exists yet. Must run before setup_wm() does
 * anything that depends on wm.num_desktops/mod_cycle/mod_control/deco_*
 * (key grabs, decoration fallback colors, etc). */
void config_load(void);

#endif /* KIWM_CONFIG_H */

/*
 * kicomp - $XDG_CONFIG_HOME/kicomp.conf (fallback ~/.config/kicomp.conf).
 *
 * Same flat key=value format as kiwm.conf, and the same rule: the file is
 * optional and every key has a working default. Command line options are
 * applied after the file and win over it.
 */
#ifndef KICOMP_CONFIG_H
#define KICOMP_CONFIG_H

void config_load(void);

#endif /* KICOMP_CONFIG_H */

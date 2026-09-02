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

/* The scale configured for an output by name ([output:DP-1] scale = 2.0),
 * or a negative number when the file says nothing about it -- which is
 * what lets the server's own DPI property answer instead. Outputs are
 * matched by RandR name; `*` in the section header matches every output
 * that has no section of its own. */
float config_output_scale(const char *name);

#endif /* KICOMP_CONFIG_H */

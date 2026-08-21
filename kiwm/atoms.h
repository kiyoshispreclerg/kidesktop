#ifndef KIWM_ATOMS_H
#define KIWM_ATOMS_H

#include <xcb/xcb.h>

xcb_atom_t intern_atom(const char *name);
void ewmh_init_atoms(void);

#endif /* KIWM_ATOMS_H */

#ifndef KIWM_OUTPUT_H
#define KIWM_OUTPUT_H

#include <stdbool.h>

int primary_output_index(void);
int output_index_for_point(int x, int y);
int output_for_pointer(void);

void outputs_refresh(void);

void ewmh_set_current_desktop(int desktop);
void switch_workspace(int output_idx, int desktop);
void cycle_output_desktop(int direction);

#endif /* KIWM_OUTPUT_H */

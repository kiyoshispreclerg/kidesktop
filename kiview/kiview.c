/* SPDX-License-Identifier: GPL-3.0-or-later */
/* kiview - image and document viewer. */
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>

#include "../shared/xis_i18n.h"
#include "kv.h"

int main(int argc, char **argv)
{
    xis_i18n_init("kiview");
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--version")) {
            printf("kiview %s\n", KIVIEW_VERSION);
            return 0;
        }
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            printf("kiview %s - image and document viewer\n\nUsage: kiview [FILE|DIRECTORY]\n", KIVIEW_VERSION);
            return 0;
        }
    }
    gtk_init(&argc, &argv);
    GFile *file = NULL;
    if (argc > 1)
        file = g_file_new_for_commandline_arg(argv[1]);
    kv_window_run(file);
    if (file)
        g_object_unref(file);
    return 0;
}

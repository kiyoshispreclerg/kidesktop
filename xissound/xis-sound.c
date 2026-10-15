/* xis-sound - plays a sound-theme event from the command line, so a
 * shell script or an app with no KiDesktop code can use the desktop's
 * sound theme: `xis-sound window-minimize`. See shared/xis_sound.h for
 * the theme, settings and event names. Always exits 0: no sound is not
 * an error. */
#include "../shared/xis_sound.h"
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: xis-sound <event>\n");
        return 2;
    }
    xis_sound_play(argv[1]);
    return 0;
}

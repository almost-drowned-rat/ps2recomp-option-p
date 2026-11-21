/* launcher_main.c -- a port's launcher: its names from the build (CMakeLists.txt here: PS2_LAUNCHER_* from the
 * game's game.json and the port's release settings). */

#include "launcher.h"

#include <SDL3/SDL_main.h>

#ifndef PS2_LAUNCHER_GAME
#define PS2_LAUNCHER_GAME "the game"
#endif
#ifndef PS2_LAUNCHER_DISC
#define PS2_LAUNCHER_DISC PS2_LAUNCHER_GAME
#endif
#ifndef PS2_LAUNCHER_TITLE
#define PS2_LAUNCHER_TITLE PS2_LAUNCHER_GAME " - PC port"
#endif
#ifndef PS2_LAUNCHER_GAME_EXE
#define PS2_LAUNCHER_GAME_EXE "game.exe"
#endif

#ifndef PS2_LAUNCHER_EXTRA_DISC
#define PS2_LAUNCHER_EXTRA_DISC NULL
#endif
#ifndef PS2_LAUNCHER_EXTRA_KEY
#define PS2_LAUNCHER_EXTRA_KEY NULL
#endif
#ifndef PS2_LAUNCHER_FILE_LABEL
#define PS2_LAUNCHER_FILE_LABEL NULL
#define PS2_LAUNCHER_FILE_FILTER_NAME NULL
#define PS2_LAUNCHER_FILE_FILTER NULL
#define PS2_LAUNCHER_FILE_NAME NULL
#endif

#ifdef PS2_LAUNCHER_ICON_INC
static const unsigned char k_icon[] = {     /* -DPS2_LAUNCHER_ICON=<a BMP>: the setup window's icon */
#include "launcher_icon.inc"
};
#define PS2_LAUNCHER_ICON_DATA k_icon, sizeof k_icon
#else
#define PS2_LAUNCHER_ICON_DATA NULL, 0
#endif

int main(int argc, char **argv)
{
    static const PsLauncherConfig cfg = {
        PS2_LAUNCHER_GAME, PS2_LAUNCHER_DISC, PS2_LAUNCHER_TITLE, PS2_LAUNCHER_GAME_EXE, PS2_LAUNCHER_ICON_DATA,
        PS2_LAUNCHER_EXTRA_DISC, PS2_LAUNCHER_EXTRA_KEY,
        PS2_LAUNCHER_FILE_LABEL, PS2_LAUNCHER_FILE_FILTER_NAME, PS2_LAUNCHER_FILE_FILTER, PS2_LAUNCHER_FILE_NAME,
    };
    return ps_launcher_main(&cfg, argc, argv);
}

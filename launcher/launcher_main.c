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

int main(int argc, char **argv)
{
    static const PsLauncherConfig cfg = {
        PS2_LAUNCHER_GAME, PS2_LAUNCHER_DISC, PS2_LAUNCHER_TITLE, PS2_LAUNCHER_GAME_EXE, NULL, 0,
        PS2_LAUNCHER_EXTRA_DISC, PS2_LAUNCHER_EXTRA_KEY,
    };
    return ps_launcher_main(&cfg, argc, argv);
}

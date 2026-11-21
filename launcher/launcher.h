/*
 * launcher.h -- the one exe a player downloads (Option P design 7d, M7 "Release launcher"). It carries no game
 * code and no game data: the engine's libraries prebuilt, the port's own objects, the translator (the toolkit's
 * Python with an embedded Python), the game's metadata (analysis, forms table, references into the disc -
 * tools/player_package.py), the compiler and the licences - as a zip appended to the exe (the payload) - and
 * builds the game on the player's machine at the first start:
 *
 *   his disc image (beside the exe, in game/, the one the game last used, or picked) -> unpack the payload ->
 *   unpack the shipped compiler (SHA-256) -> check the disc -> translate (tools/player_build.py) -> compile ->
 *   link -> start the game.
 *
 * Nothing is downloaded and the image is only read, never moved or changed. Later starts start the game at once
 * (a stamp: the release + the image's size and the SHA-256 of its first 4 MiB - the same image moved builds
 * nothing again); `--repair` builds again. What to run is data in the payload
 * (lift.txt, build.txt, toolchain.txt, written by the port's release script from its real build), so this file
 * is the same for every PS2 port; the port gives only its names (PsLauncherConfig).
 *
 * The model is the Xbox toolkit's launcher (xboxrecomp_option_c src/launcher, the same author's): its payload
 * trailer, toolchain.txt format 2 and step files, without its installer (a PS2 port plays from the image itself).
 * SDL3 only (processes, dialogs, files).
 */
#ifndef PS_LAUNCHER_H
#define PS_LAUNCHER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *game;                   /* "Zone of the Enders": the name in the messages */
    const char *disc;                   /* "Zone of the Enders (USA, SLUS-201.48)": what the player provides */
    const char *window_title;           /* "Zone of the Enders - PC port" */
    const char *game_exe;               /* "ZOE.exe": built next to the launcher */
    const void *icon_bmp;               /* the progress window's icon (a BMP), or NULL */
    size_t      icon_bmp_size;
    /* An optional second disc on the setup page (a multi-version port's other version - ZOE: "Japanese disc
     * (optional)" for its Japanese voices, M9), or NULL: its line's label, and the pc_settings.ini key its path is
     * kept under for the game ("disc_ja"). Only read, never built from. */
    const char *extra_disc_label;
    const char *extra_disc_key;
    /* An optional file of the port's own on the setup page (ZOE: "Subtitles (optional):", its subtitle pack - a
     * download of its own), or NULL: its line's label, the file dialog's filter ("Subtitle pack (*.pak)" and
     * "*.pak"), and the name it is copied to beside the game ("ZOE-subtitles.pak", where the game looks for it). */
    const char *extra_file_label;
    const char *extra_file_filter_name;
    const char *extra_file_filter;
    const char *extra_file_name;
} PsLauncherConfig;

/* The whole launcher; returns the process exit code. */
int ps_launcher_main(const PsLauncherConfig *cfg, int argc, char **argv);

#ifdef __cplusplus
}
#endif

#endif

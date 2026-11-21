/*
 * game_setup_ui.h - a small SDL3 window for xr_game_setup: a progress bar
 * and a line of text, drawn with SDL_RenderDebugText (no font library).
 * Closing the window cancels the install. Messages (an error, an unknown
 * version) go into an SDL message box, which needs no window.
 *
 * Usage: ui = xr_setup_ui_open("THPS 2X"); opts.ctx = ui;
 * opts.progress = xr_setup_ui_progress; opts.cancelled = xr_setup_ui_cancelled;
 * xr_game_setup(&opts, &res); xr_setup_ui_close(ui);
 * The window opens only when there is something to install (on the first
 * progress call), so a normal start shows nothing.
 */
#ifndef XR_GAME_SETUP_UI_H
#define XR_GAME_SETUP_UI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XrSetupUi XrSetupUi;

XrSetupUi *xr_setup_ui_open(const char *title);
/* The window's icon (a BMP in memory, kept by the caller), before the first progress call. */
void xr_setup_ui_icon(XrSetupUi *ui, const void *bmp, size_t n);
void xr_setup_ui_progress(void *ui, double fraction, const char *text);
/* The setup's first page (Windows: game_setup_ui_win95.c, the Windows 95 look): the disc image line (`iso`
 * pre-filled, Browse...), Go and Quit; `note` a line above it (why it asks again), or NULL; with `extra_label` a
 * second, optional disc's line under it (`extra` pre-filled, may stay empty). 1: Go (`iso`, `extra` set); 0: Quit;
 * -1: no such page here (the SDL window, or no dialogs) - the launcher asks its own way. Then the target folder's line
 * (`target` pre-filled: the game's folder) and the box "copy the disc image into the target folder (game\)" (`copy`
 * in / out). */
int  xr_setup_ui_form(XrSetupUi *ui, const char *disc_name, char *iso, size_t n, const char *note,
                      const char *extra_label, char *extra, size_t extra_n, char *target, size_t target_n, int *copy);
/* An optional file's line on the first page, under the discs' (before the first xr_setup_ui_form): its label, the
 * file dialog's filter (a name and a pattern), and `path` (n bytes, pre-filled, set at Go; may stay empty). Only the
 * Windows 95 window has it. */
void xr_setup_ui_file_line(XrSetupUi *ui, const char *label, const char *filter_name, const char *filter, char *path,
                           size_t n);
int  xr_setup_ui_cancelled(void *ui);
void xr_setup_ui_close(XrSetupUi *ui);

/* A message box (error = 1: an error icon). Works with ui == NULL. Nothing
 * with RECOMP_NO_DIALOGS=1 (automated tests: errors go to the log only). */
void xr_setup_ui_message(XrSetupUi *ui, int error, const char *text);
int  xr_no_dialogs(void);

#ifdef __cplusplus
}
#endif

#endif /* XR_GAME_SETUP_UI_H */

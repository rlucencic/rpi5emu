#pragma once

/* Between the runtime's own files */
#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* emu.c */
bool emu_started(void);
void emu_quit(void);
const char *emu_message(void);
const char *emu_drive_info(void);
/* The picture as last drawn, as a PNG (path NULL: the next free
 * <data>/screenshots/<machine>-<n>.png); the file's path in `made`
 */
bool emu_screenshot(const char *path, char *made, size_t len);
/* A choice row set as the setup screen would: a running machine restarts
 * unless it's live (and with the old choices if it can't)
 */
bool emu_choose(int row, int index);
int emu_volume(void);
bool emu_muted(void);
void emu_set_volume(int volume);
void emu_set_muted(bool muted);
/* The status as the boards' /api/status has it (the machine's rows and drives) */
void emu_status_json(char *buf, size_t len);
/* Hold the emulation between frames (as emu_paused(), for as long as the
 * setup screen is open), and let it go on
 */
void emu_hold(void);
void emu_release(void);
/* Start again with the current settings, or with `before` (the choice rows'
 * values) if it can't
 */
bool emu_restart_or_revert(const int *before);
void emu_toggle_fullscreen(void);

/* ui.c: the setup screen (main thread) */
void ui_init(SDL_Renderer *renderer);
bool ui_is_open(void);
void ui_open(void);
bool ui_event(const SDL_Event *e);      /* while open: every event (renderer coordinates) */
bool ui_dirty(void);
void ui_touch(void);                    /* draw it again */
void ui_click(float x, float y);        /* canvas coordinates (1024 x 600) */
void ui_close_to_machine(void);         /* as Esc: back to a running machine */
/* From other threads: through the main thread (console's t and setup) */
void emu_ui_click(int x, int y);
void emu_ui_toggle(void);
void ui_draw(int width, int height);

/* web.c: the boards' web page and API on <port> (0: none) */
void emu_web_start(int port);

/* console.c */
void emu_console_start(int port, bool use_stdin);

/* png.c: RGB565 pixels as an 8-bit RGB PNG */
bool emu_write_png(const char *path, const uint16_t *pixels, int width, int height);

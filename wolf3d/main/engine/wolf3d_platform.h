// Viper: what the engine asks of the app around it (../wolf3d_app.cpp). The
// engine runs on a thread of its own and calls these from it; each that waits
// (a frame, a delay, a key) hands the turn back to the app's loop() until the
// next frame, so the engine and the app never run at the same time.
//
// Plain C with no <stdbool.h>: the engine has its own bool.
#ifndef WOLF3D_PLATFORM_H
#define WOLF3D_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// The time, in ms, as of the frame the engine is running in. It moves only
// when the engine waits for a frame, so a loop that watches it must wait.
uint32_t wolf3d_ticks(void);
// Waits `ms` (at least one frame when ms > 0), showing what's on screen
void wolf3d_delay(uint32_t ms);
// Waits for the next frame
void wolf3d_next_frame(void);

// Video: the engine's 320x200 screen, 8 bits a pixel. present() shows it with
// the palette last set (256 colors, 8 bits a component, r g b a) and waits
// for the next frame.
void wolf3d_set_palette(const uint8_t *rgba);
void wolf3d_present(const uint8_t *pixels);

// Input: the emulated keyboard's keys that went down (1) or up (0) since the
// last call, one at a time. Returns 0 when there are no more.
int wolf3d_poll_key(int *scancode, int *down);
// Whether the engine is playing (the 3D view, where A fires and B opens
// doors) or in a menu or screen (where A chooses and B goes back)
void wolf3d_set_playing(int playing);
// Whether the menu (the control panel) is open
void wolf3d_set_menu(int open);

// Files. The game's data (*.wl1) is in the app's folder; its saves and
// config in its data folder (NULL when there's none).
FILE *wolf3d_fopen(const char *name, const char *mode);
// How much of the page file (walls, sprites) to keep in memory, in bytes
size_t wolf3d_page_cache_size(void);
const char *wolf3d_save_dir(void);

// The engine's, for the app's tests: the weapon in hand, and whether the
// player is running
int wolf3d_weapon(void);
int wolf3d_running(void);

// Leaves the engine: back to the app, with `error` to show unless it's empty
void wolf3d_quit(const char *error) __attribute__((noreturn));

// Memory: every block the engine allocates, so closing the app frees all of
// them whatever state the engine was left in
void *wolf3d_malloc(size_t size);
void *wolf3d_calloc(size_t count, size_t size);
void *wolf3d_realloc(void *ptr, size_t size);
void wolf3d_free(void *ptr);
char *wolf3d_strdup(const char *s);

// Sound. The engine's mixer (SD_Mix() in id_sd.c) runs on its thread, called
// by the app, which keeps it ahead of the audio tap: an OPL2 for the AdLib
// sound effects and music, and channels of digitized sound.
#define WOLF3D_MIX_RATE 22050
#define WOLF3D_MIX_CHANNELS 8
void wolf3d_opl_write(uint32_t reg, uint8_t value);
// Writes `frames` of the OPL's output to `mono`
void wolf3d_opl_generate(int32_t *mono, int frames);

#ifdef __cplusplus
}
#endif

#endif

/* Between NetHack's side of the app (C: the window port, port/winviper.c,
 * and the platform, port/viper_sys.c, which include NetHack's headers) and
 * the app's (C++: nethack_engine.cpp, which doesn't).
 *
 * Everything here is called on the engine's thread, while the engine has the
 * turn (nethack_engine.cpp), so the game model needs no locks. The calls that
 * wait for the player hand the turn to the app's loop() until it has an
 * answer.
 */

#ifndef NH_PORT_H
#define NH_PORT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The map is COLNO x ROWNO cells (NetHack's), column 0 unused */
#define NHV_COLS 80
#define NHV_ROWS 21

/* What kind of window (NetHack's NHW_*) */
enum nhv_window_kind {
    NHV_WIN_MESSAGE = 1,
    NHV_WIN_STATUS,
    NHV_WIN_MAP,
    NHV_WIN_MENU,
    NHV_WIN_TEXT,
};

/* How many menu items can be chosen (NetHack's PICK_*) */
enum nhv_pick {
    NHV_PICK_NONE = 0,
    NHV_PICK_ONE,
    NHV_PICK_ANY,
};

/* Cell flags, from NetHack's glyph flags (MG_*) */
enum nhv_cell_flags {
    NHV_CELL_PET = 1 << 0,
    NHV_CELL_DETECTED = 1 << 1,
    NHV_CELL_INVERSE = 1 << 2,
    NHV_CELL_REMEMBERED = 1 << 3, /* not seen now (remembered or dark) */
    NHV_CELL_OBJPILE = 1 << 4,
    NHV_CELL_RIDDEN = 1 << 5,
};

/* The engine's entry: runs NetHack (port/viper_sys.c) until it exits */
void nhv_nethack_main(const char *app_dir, const char *data_dir, const char *name);

/* ---- the map ---- */
void nhv_map_clear(void);
/* ch: the character NetHack would show; color 0..15 (NetHack's CLR_*);
 * tile: its tile (tiles.bin, numbered as src/tile.c), -1 for none */
void nhv_map_glyph(int x, int y, int glyph, int ch, int color, unsigned flags, int tile);
/* Where the hero is (NetHack's cliparound) */
void nhv_map_hero(int x, int y);

/* ---- messages and status ---- */
void nhv_message(const char *text);
/* Called when NetHack wants the messages so far seen before going on */
void nhv_more(void);
/* The status, as lines of text (two or three) */
void nhv_status(const char *const *lines, int count);

/* ---- menus and text windows ---- */
int nhv_window_create(int kind);
void nhv_window_destroy(int win);
void nhv_window_clear(int win);
void nhv_window_text(int win, int attr, const char *text);
/* Shows a text or menu window that has only text, until it's dismissed */
void nhv_window_show(int win);
void nhv_menu_start(int win);
/* index: 1.. for a choosable item, 0 for a heading or a line of text */
void nhv_menu_add(int win, int index, char accelerator, int attr, int color, int selected,
                  const char *text);
void nhv_menu_end(int win, const char *prompt);
/* Waits for a choice. Fills indexes[] (and counts[], -1 for all) with up to
 * max chosen items; returns how many, or -1 if cancelled. */
int nhv_menu_select(int win, int how, int *indexes, long *counts, int max);

/* ---- input ---- */
/* Waits for a key (a command, a direction, a key in a prompt) */
int nhv_get_key(void);
/* Waits for one of choices (or ESC, or def) */
char nhv_yn(const char *query, const char *choices, char def);
/* Waits for a line of text; "\033" in buf if cancelled */
void nhv_getlin(const char *query, char *buf, size_t size);

/* ---- what the context action (A) acts on ---- */
enum nhv_here {
    NHV_HERE_OBJECTS = 1 << 0, /* objects the hero can reach */
    NHV_HERE_UPSTAIRS = 1 << 1,
    NHV_HERE_DOWNSTAIRS = 1 << 2,
    NHV_HERE_FOUNTAIN = 1 << 3,
    NHV_HERE_THRONE = 1 << 4,
    NHV_HERE_SINK = 1 << 5,
    NHV_HERE_ALTAR = 1 << 6,
};
/* Before each key is read, while a game is going: here, NHV_HERE_* at the
 * hero; doors, the closed doors the hero sees next to it, a bit per direction
 * in NetHack's order (xdir[]/ydir[]: west, northwest, north, northeast, east,
 * southeast, south, southwest) */
void nhv_context(unsigned here, unsigned doors);

/* ---- positions (getpos: travel, far look, throwing at a spot) ---- */
/* A position is being chosen (goal: what for), or no longer (NULL) */
void nhv_getpos(const char *goal);
/* Where the cursor is while one is chosen */
void nhv_map_cursor(int x, int y);

/* An extended command's name the app queued (keys up to a newline), if the
 * next keys queued are one; 0 if not */
int nhv_queued_line(char *buf, size_t size);

/* ---- the platform ---- */
/* NetHack is done: goes back to the top of the engine's thread */
#ifdef __GNUC__
__attribute__((noreturn))
#endif
void nhv_exit(int status);
void nhv_log(const char *text);
/* The seed a game starts from, 0 for one of the platform's (the time); the
 * tests set one (engine_set_seed()) so games repeat */
unsigned long nhv_random_seed(void);

#ifdef __cplusplus
}
#endif

#endif /* NH_PORT_H */

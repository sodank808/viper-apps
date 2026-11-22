// Viper: the little of SDL the engine uses, in place of <SDL.h>. The engine is
// Wolf4SDL; on Viper its video, input, timing and sound go through
// wolf3d_platform.h, implemented by the app (../wolf3d_app.cpp), and these
// are the types and helpers its code still names.
#ifndef WOLF_SDL_H
#define WOLF_SDL_H

#include <stdint.h>
#include <string.h>

#include "wolf3d_platform.h"

typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef uint32_t Uint32;
typedef int8_t Sint8;
typedef int16_t Sint16;
typedef int32_t Sint32;

typedef struct SDL_Color {
    Uint8 r, g, b, a;
} SDL_Color;

typedef struct SDL_Rect {
    int x, y, w, h;
} SDL_Rect;

// An 8-bit surface: the engine draws into screenBuffer and reads pixels and
// pitch
typedef struct SDL_Surface {
    int w, h;
    int pitch;
    void *pixels;
} SDL_Surface;

#define SDL_ALPHA_OPAQUE 255
#define SDL_MUSTLOCK(s) 0

static inline int SDL_LockSurface(SDL_Surface *s) { (void)s; return 0; }
static inline void SDL_UnlockSurface(SDL_Surface *s) { (void)s; }

static inline Uint32 SDL_GetTicks(void) { return wolf3d_ticks(); }
static inline void SDL_Delay(Uint32 ms) { wolf3d_delay(ms); }

SDL_Surface *SDL_CreateSurface8(int w, int h);
void SDL_FreeSurface(SDL_Surface *s);
int SDL_FillRect(SDL_Surface *s, const SDL_Rect *rect, Uint32 color);
int SDL_BlitSurface(SDL_Surface *src, const SDL_Rect *srcrect, SDL_Surface *dst, SDL_Rect *dstrect);

// Scan codes: SDL 2's values, for the keyboard the input layer emulates
typedef enum {
    SDL_SCANCODE_UNKNOWN = 0,
    SDL_SCANCODE_A = 4, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D, SDL_SCANCODE_E,
    SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J,
    SDL_SCANCODE_K, SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
    SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S, SDL_SCANCODE_T,
    SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W, SDL_SCANCODE_X, SDL_SCANCODE_Y,
    SDL_SCANCODE_Z,
    SDL_SCANCODE_1 = 30, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4, SDL_SCANCODE_5,
    SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8, SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN = 40, SDL_SCANCODE_ESCAPE, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_TAB,
    SDL_SCANCODE_SPACE,
    SDL_SCANCODE_MINUS = 45, SDL_SCANCODE_EQUALS,
    SDL_SCANCODE_COMMA = 54, SDL_SCANCODE_PERIOD,
    SDL_SCANCODE_CAPSLOCK = 57,
    SDL_SCANCODE_F1 = 58, SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5,
    SDL_SCANCODE_F6, SDL_SCANCODE_F7, SDL_SCANCODE_F8, SDL_SCANCODE_F9, SDL_SCANCODE_F10,
    SDL_SCANCODE_F11, SDL_SCANCODE_F12,
    SDL_SCANCODE_SCROLLLOCK = 71, SDL_SCANCODE_PAUSE, SDL_SCANCODE_INSERT, SDL_SCANCODE_HOME,
    SDL_SCANCODE_PAGEUP, SDL_SCANCODE_DELETE, SDL_SCANCODE_END, SDL_SCANCODE_PAGEDOWN,
    SDL_SCANCODE_RIGHT = 79, SDL_SCANCODE_LEFT, SDL_SCANCODE_DOWN, SDL_SCANCODE_UP,
    SDL_SCANCODE_KP_ENTER = 88, SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_3,
    SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_5, SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_7,
    SDL_SCANCODE_KP_8,
    SDL_SCANCODE_LCTRL = 224, SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LALT, SDL_SCANCODE_LGUI,
    SDL_SCANCODE_RCTRL, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RALT,
    SDL_NUM_SCANCODES = 256
} SDL_Scancode;

#define SDL_TEXTINPUTEVENT_TEXT_SIZE 32

// No mouse: the code that reads one runs only while the input is grabbed,
// which it never is here
#define SDL_BUTTON(x) (1 << ((x) - 1))
#define SDL_BUTTON_MIDDLE 2
#define SDL_BUTTON_RIGHT 3
static inline Uint32 SDL_GetRelativeMouseState(int *x, int *y)
{
    *x = *y = 0;
    return 0;
}

// No screenshots (the debug keys' F-key)
static inline int SDL_SaveBMP(SDL_Surface *s, const char *file)
{
    (void)s;
    (void)file;
    return -1;
}

// Messages go to the log; an error also ends the engine (Quit())
#define SDL_MESSAGEBOX_ERROR 0
#define SDL_MESSAGEBOX_INFORMATION 1
void SDL_ShowSimpleMessageBox(int flags, const char *title, const char *message, void *window);

#endif

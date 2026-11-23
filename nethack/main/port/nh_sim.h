/* What every NetHack and Lua source file gets on top of NetHack's own
 * settings: through nhviper.h for NetHack's, and luaconf.h (marked "Viper:")
 * for Lua's.
 *
 * - Memory: every allocation goes through nhv_malloc() and friends
 *   (nethack_engine.cpp), which keep a list, so that ending a game (or
 *   closing the app mid-game) frees everything NetHack and Lua hold, however
 *   they left. The prototypes in the C library's headers become these
 *   functions' too, with the same types.
 *
 * - The simulator links every ELF app into one program (VIPER_APP_STATIC_NAME
 *   is set there). NetHack's and Lua's global names would meet the
 *   firmware's own Lua and other apps', so nh_names.h (tools/gen_names.py)
 *   gives each the prefix nh_. And their variables would keep their values
 *   from one game to the next, where a device loads the app afresh: built
 *   with Clang, they go in sections of their own, which the app copies when
 *   it first starts the game and puts back before each new one.
 */

#ifndef NH_SIM_H
#define NH_SIM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void *nhv_malloc(size_t size);
void *nhv_calloc(size_t count, size_t size);
void *nhv_realloc(void *ptr, size_t size);
void nhv_free(void *ptr);
char *nhv_strdup(const char *s);
/* NetHack is done: back to the top of the engine's thread (nh_port.h) */
#ifdef __GNUC__
__attribute__((noreturn))
#endif
void nhv_exit(int status);
#ifdef __cplusplus
}
#endif

#define malloc(size) nhv_malloc(size)
#define calloc(count, size) nhv_calloc(count, size)
#define realloc(ptr, size) nhv_realloc(ptr, size)
#define free(ptr) nhv_free(ptr)
#define strdup(s) nhv_strdup(s)
#define exit(status) nhv_exit(status)

/* A device's C library calls, through one wrapper each (nh_libc.h) */
#include "nh_libc.h"

#if defined(VIPER_APP_STATIC_NAME)
#include "nh_names.h"
#if defined(__clang__)
#define NH_RESTART_STATE 1
#ifdef __APPLE__
#pragma clang section bss = "__DATA,__nh_bss" data = "__DATA,__nh_data"
#else
#pragma clang section bss = "nh_bss" data = "nh_data"
#endif
#endif
#endif

#endif /* NH_SIM_H */

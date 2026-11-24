/*
 * Viper: how gwenesis is built for the Genesis ELF app (genesis/). Every
 * gwenesis source includes this first, so the settings hold in the firmware's
 * ELF build and in the host simulator, which compiles the app's sources with
 * none of main/CMakeLists.txt's flags.
 */
#ifndef GWENESIS_PORT_H
#define GWENESIS_PORT_H

/* The firmware doesn't export __assert_func (first: before anything includes
 * assert.h) */
#ifndef NDEBUG
#define NDEBUG 1
#endif

/* The core leans on headers ESP-IDF's newlib pulls in by the way (uint8_t,
 * malloc()); the host's don't */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* esp-box-emu's dual-core split, built in both ways: genesis_threaded (set
 * when the machine starts, genesis_core.cpp) picks the 68000 and the VDP on
 * the app's thread with the Z80 and the sound chips on a thread of their own,
 * or everything in turn on the app's thread. The core's GENESIS_DUAL_CORE
 * blocks are runtime choices on it. */
#define GENESIS_DUAL_CORE 1
/* Line-accurate audio: the sound chips catch up once a scanline */
#define GWENESIS_AUDIO_ACCURATE 0
/* No esp-box-emu profiling (it reads the CPU's cycle counter) */
#define GENESIS_PERF 0
/* The 68000 clock setting (m68k->cycle_ratio) and the wait-loop skip */
#define M68K_OVERCLOCK_SHIFT 8
#define M68K_IDLE_SKIP 1

/* Code placement is the loader's business: an ELF app's code runs from PSRAM */
#ifndef IRAM_ATTR
#define IRAM_ATTR
#endif

/* The 68000 sees cartridge space as 0x000000-0x7FFFFF and gwenesis indexes the
 * ROM with the address as is. The app pads the ROM to a power of two and sets
 * this to its size - 1, so a read past the end mirrors the ROM, as on a
 * cartridge, instead of reading past the buffer (gwenesis_bus.c). */
#ifdef __cplusplus
extern "C" {
#endif
extern unsigned int gwenesis_rom_mask;
extern int genesis_threaded;
#ifdef __cplusplus
}
#endif

#endif /* GWENESIS_PORT_H */

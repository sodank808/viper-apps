#pragma once
/*
 * Where Doom's memory comes from (doom_memory.cpp).
 *
 * The zone (z_zone.c) gets its blocks from one big arena (the firmware's ROM
 * buffer, lent with viper_memory_scratch(), or the biggest block malloc()
 * gives). When it's full, Z_Malloc() purges cached lumps (the WAD's
 * textures, sprites, sounds) and asks again, so the arena's size decides how
 * often the WAD is reread, not whether Doom runs. Only when nothing's left to
 * purge does it take blocks from the PSRAM heap, while more than a reserve of
 * that is free.
 *
 * The engine's fixed tables (doom_statics.cpp) come from doom_static_alloc():
 * internal RAM when there's room, the heap after that.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets up the arena; false if there isn't even a small one */
int doom_memory_init(void);
/* Frees everything: the arena, the heap blocks, the fixed tables */
void doom_memory_shutdown(void);

/* From the arena; NULL when it's full */
void *doom_zone_alloc(size_t size);
/* From the PSRAM heap, while more than a reserve is free: for when the
 * arena is full of blocks in use */
void *doom_zone_alloc_heap(size_t size);
void doom_zone_free(void *ptr);

/* Zeroed memory for a table that lives until doom_memory_shutdown() */
void *doom_static_alloc(size_t size);

/* For the log: the arena's size, and what the zone holds in and out of it */
typedef struct {
    size_t arena_size;
    size_t arena_used;
    size_t arena_peak;
    size_t heap_used;
    size_t heap_peak;
    size_t static_fast;
    size_t static_heap;
    int arena_borrowed; /* 1: the firmware's ROM buffer */
} doom_memory_stats_t;
void doom_memory_stats(doom_memory_stats_t *stats);

#ifdef __cplusplus
}
#endif

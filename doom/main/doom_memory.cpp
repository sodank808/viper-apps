// Doom's memory: the zone's arena and heap blocks, and the engine's fixed
// tables. See doom_memory.h.
#include "doom_memory.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iterator>

#include "viper_app.h"

// The fixed tables go in the loader's shared internal RAM (.shared_dram)
// while they fit: the renderer reads several of them for every column. The
// simulator links apps statically, where the section means nothing.
#ifdef VIPER_APP_STATIC_NAME
#define DOOM_FAST_DATA
#else
#define DOOM_FAST_DATA __attribute__((section(".shared_dram")))
#endif

namespace {

// ---- the arena: first fit over a list of free blocks -----------------------
//
// Every block starts with a header giving its size and the size of the block
// before it, so freeing joins a block with free neighbours in constant time.
// A free block's place in the free list is in its header too, not in the
// memory it gave out: the engine writes NULL into blocks it has already freed
// (an owner pointer inside another freed block), as Doom's own zone lets it.

constexpr size_t kAlign = 16;
constexpr size_t kUsedBit = 1;

struct Block {
    size_t size;      // whole block, header included; kUsedBit when in use
    size_t prev_size; // the block before's size, 0 for the first
    Block *free_prev; // in the free list, while free
    Block *free_next;
};
constexpr size_t kHeader = (sizeof(Block) + kAlign - 1) & ~(kAlign - 1);
constexpr size_t kMinBlock = kHeader + kAlign;

// What PSRAM the zone leaves the rest of the system when it takes heap
// blocks (the SD card's buffers, LVGL, the system's own allocations).
constexpr uint32_t kHeapReserve = 256 * 1024;
// The arena when the firmware has no buffer to lend: the biggest of these
// that malloc() gives
constexpr size_t kOwnArenaSizes[] = {3u << 20, 2u << 20,   1536u << 10,
                                     1u << 20, 768u << 10, 512u << 10};

struct Arena {
    void *raw = nullptr; // what malloc() gave, for an arena of our own
    uint8_t *base = nullptr;
    size_t size = 0;
    bool borrowed = false; // the firmware's, not freed
    Block *free_head = nullptr;
    size_t used = 0;
    size_t peak = 0;
};
Arena s_arena;

// Zone blocks that came from the heap: a header in front of each keeps them
// in a list, so shutdown frees what Doom didn't
struct HeapHdr {
    HeapHdr *prev;
    HeapHdr *next;
    size_t size;
    size_t pad_;
};
static_assert(sizeof(HeapHdr) % kAlign == 0, "heap header keeps alignment");
HeapHdr s_heap_list = {&s_heap_list, &s_heap_list, 0, 0};
size_t s_heap_used = 0;
size_t s_heap_peak = 0;

inline size_t block_size(const Block *b) { return b->size & ~kUsedBit; }
inline bool block_used(const Block *b) { return b->size & kUsedBit; }
inline uint8_t *arena_end() { return s_arena.base + s_arena.size; }
inline Block *next_block(Block *b) {
    uint8_t *n = reinterpret_cast<uint8_t *>(b) + block_size(b);
    return n < arena_end() ? reinterpret_cast<Block *>(n) : nullptr;
}
inline Block *prev_block(Block *b) {
    return b->prev_size ? reinterpret_cast<Block *>(reinterpret_cast<uint8_t *>(b) - b->prev_size)
                        : nullptr;
}

void free_list_push(Block *b) {
    b->free_prev = nullptr;
    b->free_next = s_arena.free_head;
    if (s_arena.free_head) {
        s_arena.free_head->free_prev = b;
    }
    s_arena.free_head = b;
}

void free_list_remove(Block *b) {
    if (b->free_prev) {
        b->free_prev->free_next = b->free_next;
    } else {
        s_arena.free_head = b->free_next;
    }
    if (b->free_next) {
        b->free_next->free_prev = b->free_prev;
    }
}

void arena_setup(uint8_t *mem, size_t size, bool borrowed) {
    const uintptr_t start =
        (reinterpret_cast<uintptr_t>(mem) + kAlign - 1) & ~(uintptr_t)(kAlign - 1);
    size -= start - reinterpret_cast<uintptr_t>(mem);
    size &= ~(kAlign - 1);
    s_arena.raw = mem;
    s_arena.base = reinterpret_cast<uint8_t *>(start);
    s_arena.size = size;
    s_arena.borrowed = borrowed;
    s_arena.free_head = nullptr;
    s_arena.used = 0;
    s_arena.peak = 0;
    Block *b = reinterpret_cast<Block *>(s_arena.base);
    b->size = size;
    b->prev_size = 0;
    free_list_push(b);
}

void *arena_alloc(size_t size) {
    const size_t need = std::max(kMinBlock, (kHeader + size + kAlign - 1) & ~(kAlign - 1));
    for (Block *b = s_arena.free_head; b; b = b->free_next) {
        const size_t have = block_size(b);
        if (have < need) {
            continue;
        }
        free_list_remove(b);
        if (have - need >= kMinBlock) {
            // Split: the rest stays free
            Block *rest = reinterpret_cast<Block *>(reinterpret_cast<uint8_t *>(b) + need);
            rest->size = have - need;
            rest->prev_size = need;
            if (Block *after = next_block(rest)) {
                after->prev_size = rest->size;
            }
            free_list_push(rest);
            b->size = need;
        }
        b->size |= kUsedBit;
        s_arena.used += block_size(b);
        s_arena.peak = std::max(s_arena.peak, s_arena.used);
        return reinterpret_cast<uint8_t *>(b) + kHeader;
    }
    return nullptr;
}

void arena_free(void *ptr) {
    Block *b = reinterpret_cast<Block *>(static_cast<uint8_t *>(ptr) - kHeader);
    b->size &= ~kUsedBit;
    s_arena.used -= b->size;
    // Join the free neighbours
    if (Block *n = next_block(b); n && !block_used(n)) {
        free_list_remove(n);
        b->size += n->size;
    }
    if (Block *p = prev_block(b); p && !block_used(p)) {
        free_list_remove(p);
        p->size += b->size;
        b = p;
    }
    if (Block *n = next_block(b)) {
        n->prev_size = b->size;
    }
    free_list_push(b);
}

inline bool in_arena(const void *ptr) {
    const uint8_t *p = static_cast<const uint8_t *>(ptr);
    return s_arena.base && p >= s_arena.base && p < arena_end();
}

void *heap_alloc(size_t size) {
    uint32_t free_bytes = 0;
    if (viper_memory(VIPER_MEMORY_PSRAM, &free_bytes, nullptr)) {
        if (free_bytes < size + sizeof(HeapHdr) + kHeapReserve) {
            return nullptr;
        }
    }
    auto *h = static_cast<HeapHdr *>(malloc(sizeof(HeapHdr) + size));
    if (!h) {
        return nullptr;
    }
    h->size = size;
    h->prev = &s_heap_list;
    h->next = s_heap_list.next;
    s_heap_list.next->prev = h;
    s_heap_list.next = h;
    s_heap_used += size;
    s_heap_peak = std::max(s_heap_peak, s_heap_used);
    return h + 1;
}

void heap_free(void *ptr) {
    HeapHdr *h = static_cast<HeapHdr *>(ptr) - 1;
    h->prev->next = h->next;
    h->next->prev = h->prev;
    s_heap_used -= h->size;
    free(h);
}

// ---- the fixed tables ----------------------------------------------------------

constexpr size_t kFastSize = 40 * 1024;
alignas(16) uint8_t s_fast[kFastSize] DOOM_FAST_DATA;
size_t s_fast_used = 0;
// The ones that didn't fit, freed at shutdown
struct StaticHdr {
    StaticHdr *next;
    size_t size;
    size_t pad_[2];
};
StaticHdr *s_statics = nullptr;
size_t s_static_heap = 0;

} // namespace

extern "C" int doom_memory_init(void) {
    s_heap_list.prev = s_heap_list.next = &s_heap_list;
    s_heap_used = s_heap_peak = 0;
    s_fast_used = 0;
    s_statics = nullptr;
    s_static_heap = 0;

    size_t size = 0;
    if (void *lent = viper_memory_scratch(&size);
        lent && size >= kOwnArenaSizes[std::size(kOwnArenaSizes) - 1]) {
        arena_setup(static_cast<uint8_t *>(lent), size, true);
        return 1;
    }
    for (size_t try_size : kOwnArenaSizes) {
        if (void *mem = malloc(try_size)) {
            arena_setup(static_cast<uint8_t *>(mem), try_size, false);
            return 1;
        }
    }
    return 0;
}

extern "C" void doom_memory_shutdown(void) {
    while (s_heap_list.next != &s_heap_list) {
        heap_free(s_heap_list.next + 1);
    }
    while (s_statics) {
        StaticHdr *next = s_statics->next;
        free(s_statics);
        s_statics = next;
    }
    s_static_heap = 0;
    s_fast_used = 0;
    if (s_arena.raw && !s_arena.borrowed) {
        free(s_arena.raw);
    }
    s_arena = Arena{};
}

extern "C" void *doom_zone_alloc(size_t size) { return arena_alloc(size); }

extern "C" void *doom_zone_alloc_heap(size_t size) { return heap_alloc(size); }

extern "C" void doom_zone_free(void *ptr) {
    if (!ptr) {
        return;
    }
    if (in_arena(ptr)) {
        arena_free(ptr);
    } else {
        heap_free(ptr);
    }
}

extern "C" void *doom_static_alloc(size_t size) {
    size = (size + 15) & ~size_t(15);
    if (s_fast_used + size <= kFastSize) {
        void *p = s_fast + s_fast_used;
        s_fast_used += size;
        memset(p, 0, size);
        return p;
    }
    auto *h = static_cast<StaticHdr *>(calloc(1, sizeof(StaticHdr) + size));
    if (!h) {
        return nullptr;
    }
    h->next = s_statics;
    h->size = size;
    s_statics = h;
    s_static_heap += size;
    return h + 1;
}

extern "C" void doom_memory_stats(doom_memory_stats_t *stats) {
    stats->arena_size = s_arena.size;
    stats->arena_used = s_arena.used;
    stats->arena_peak = s_arena.peak;
    stats->heap_used = s_heap_used;
    stats->heap_peak = s_heap_peak;
    stats->static_fast = s_fast_used;
    stats->static_heap = s_static_heap;
    stats->arena_borrowed = s_arena.borrowed ? 1 : 0;
}

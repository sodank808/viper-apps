// The Genesis machine around gwenesis (genesis_core.h). The frame loop and the
// order of the core's setup follow esp-cpp/gwenesis's port/genesis.cpp (the
// single-core path), which this replaces.

#include "genesis_core.h"

#include "gwenesis/gwenesis_port.h" // first: the build settings the core's structs depend on

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <unistd.h>

extern "C" {
#include "gwenesis/genesis_dualcore.h"
#include "gwenesis/gwenesis_bus.h"
#include "gwenesis/gwenesis_io.h"
#include "gwenesis/gwenesis_savestate.h"
#include "gwenesis/gwenesis_sn76489.h"
#include "gwenesis/gwenesis_vdp.h"
#include "gwenesis/m68k.h"
#include "gwenesis/ym2612.h"
#include "gwenesis/z80inst.h"

void z80_irq_line(unsigned int value);
void m68k_idle_reset(void);
extern int m68k_idle_skip_enabled;

// The core's buffers and the port layer's globals, which gwenesis declares
// extern and leaves to the platform (esp-box-emu's genesis.cpp and
// genesis_shared_memory.cpp defined them)
extern unsigned char *VRAM;
extern unsigned short *CRAM;
extern unsigned short *CRAM565;
extern unsigned short *VSRAM;
extern unsigned char *SAT_CACHE;
extern unsigned char *gwenesis_vdp_regs;
extern unsigned short *fifo;
extern unsigned short gwenesis_vdp_status;
extern int screen_width, screen_height;
extern int hint_pending;
extern int zclk;
extern unsigned int gwenesis_cram_writes;
extern unsigned int *sin_tab;
extern unsigned short *m68ki_op_index;  // m68kcpu.h
extern unsigned short *m68ki_op_blocks; // m68kcpu.h

uint8_t *M68K_RAM = nullptr;
uint8_t *ZRAM = nullptr;
signed int *tl_tab = nullptr;
int16_t *lfo_pm_table = nullptr;
int16_t *gwenesis_sn76489_buffer = nullptr;
int16_t *gwenesis_ym2612_buffer = nullptr;
int sn76489_index;
int sn76489_clock;
int ym2612_index;
int ym2612_clock;
int system_clock;
int scan_line;
// Whether the sound unit runs on its own thread (gwenesis_port.h)
int genesis_threaded = 0;

// What the renderer draws from (gwenesis_vdp_gfx.c) and which of VRAM's
// 256-byte blocks the 68000 changed since it was last copied
// (gwenesis_vdp_mem.c)
extern unsigned char *gfx_vram;
extern unsigned char *gfx_regs;
extern unsigned short *gfx_vsram;
extern unsigned char *gfx_sat;
extern int gfx_screen_width, gfx_screen_height;
extern int mode_pal;
unsigned int gwenesis_vram_dirty[8];
}

namespace genesis {
namespace {

// The core's hot state goes in the loader's shared internal RAM
// (.shared_dram, 64 KB in all): in PSRAM every access shares the cache with
// the display's scan-out. In priority order, as esp-box-emu measured it: the
// YM2612's tables and state (read for every sample), the CPU, the Z80's RAM,
// the VDP's small tables and line buffers. The 68000's work RAM and VRAM (64
// KB each) don't fit beside them and go to PSRAM, as does the 128 KB opcode
// index. On the host (the simulator links apps statically, and the unit tests
// build this file alone) the section means nothing.
#ifdef __XTENSA__
#define GENESIS_FAST_DATA __attribute__((section(".shared_dram")))
#else
#define GENESIS_FAST_DATA
#endif

constexpr int kTlTabLen = 13 * 2 * 256;
constexpr int kSinLen = 1024;
constexpr int kLfoPmLen = 128 * 8 * 16;
constexpr int kAudioLength = 1056; // GWENESIS_AUDIO_BUFFER_LENGTH_PAL
constexpr int kLineBuffer = SCREEN_WIDTH + PIX_OVERFLOW * 2;
constexpr int kSampleStep = 2;
constexpr unsigned int kVintDelayCycles = 788; // see run_frame()

struct FastData {
    signed int tl_tab[kTlTabLen];
    unsigned int sin_tab[kSinLen];
    YM2612 ym2612;
    uint8_t opn_regs[512];
    m68ki_cpu_core m68k;
    uint8_t zram[MAX_Z80_RAM_SIZE];
    alignas(16) uint8_t render_buffer[kLineBuffer];
    alignas(16) uint8_t sprite_buffer[kLineBuffer];
    // The renderer's finished line, before it goes to the frame
    alignas(16) uint8_t line_out[SCREEN_WIDTH];
    alignas(16) uint8_t sat_cache[SAT_CACHE_MAX_SIZE];
    uint16_t cram[CRAM_MAX_SIZE];
    uint16_t cram565[CRAM_MAX_SIZE * 4];
    uint16_t vsram[VSRAM_MAX_SIZE];
    uint8_t vdp_regs[REG_SIZE];
    uint16_t fifo[FIFO_SIZE];
    int16_t sn76489_buffer[kAudioLength];
    int16_t ym2612_buffer[kAudioLength];
    // The 68000's sound chip writes, from it to the sound unit's thread
    uint32_t sound_queue[1024];
    // The opcode table's first level: read for every 68000 instruction
    uint16_t op_blocks[0x10000 >> 5];
};
GENESIS_FAST_DATA FastData g_fast;

// --- The sound unit on its own thread ---------------------------------------
// esp-box-emu's dual-core split (port/genesis.cpp): the app's thread runs the
// 68000 and the VDP, a second thread the Z80, the YM2612 and the PSG. Both
// step through the same scanlines; the sound thread doesn't start a line until
// the 68000 has run it (the leash), the 68000's writes to the sound chips go
// through a queue the sound thread applies in order, and the 68000 reads a
// copy of the YM2612's status the sound thread publishes each line. A Z80 bus
// request waits for the sound thread to stop between lines (z80inst.c).
//
// ELF apps can't pin a thread to a core, but on a device the app's thread
// runs on core 0 at a high priority, so the sound thread (the default
// priority, no affinity) runs on core 1 while it works. No condition variables
// are exported, so between frames the sound thread sleeps a tick at a time
// (core 1's idle task must run, or the task watchdog fires), and within a
// frame both sides spin.
constexpr uint32_t kSoundQueueMask = 1023;
std::atomic<uint32_t> g_queue_head{0}; // the sound thread's
std::atomic<uint32_t> g_queue_tail{0}; // the 68000's
std::atomic<uint32_t> g_queue_dropped{0};
std::atomic<unsigned int> g_ym_status{0};
// The last scanline the 68000 has run this frame
std::atomic<int> g_core0_line{-1};

struct SoundThread {
    pthread_t thread{};
    bool running = false;
    std::atomic<uint32_t> start{0}; // frames handed to it
    std::atomic<uint32_t> done{0};  // frames it finished
    std::atomic<bool> quit{false};
    int screen_height = 0;
    int lines = 0;
};
SoundThread g_sound;
bool g_want_threaded = false;

// Waits until the 68000 has run `line`. Briefly by spinning (a line is ~40
// us of the 68000's time), then by sleeping a tick at a time: the sound
// thread may fall behind the 68000 (the frame's end waits for it), and while
// it sleeps the display's video task gets core 1, which it shares and which
// the sound thread would otherwise hold whenever a frame is being emulated.
void sound_wait_for_line(int line) {
    for (int spins = 0; g_core0_line.load(std::memory_order_acquire) < line; spins++) {
        if (spins > 400) {
            usleep(1000);
        }
    }
}

void sound_scanline(int clock) {
    genesis_sound_queue_drain();
    z80_run(clock);
    gwenesis_SN76489_run(clock);
    ym2612_run(clock);
    genesis_ym2612_status_publish(YM2612Read(0));
}

// One frame of the sound unit, a scanline behind the 68000 at most
void sound_run_frame(int height, int lines) {
    int clock = 0;
    int line = 0;
    for (; line < height; ++line) {
        clock += VDP_CYCLES_PER_LINE;
        sound_wait_for_line(line);
        sound_scanline(clock);
    }
    // The Z80's V-int, held until it takes it (esp-box-emu: a pulse is
    // missed while the 68000 holds the Z80's bus through vblank)
    z80_irq_line(1);
    for (; line < lines; ++line) {
        clock += VDP_CYCLES_PER_LINE;
        sound_wait_for_line(line);
        sound_scanline(clock);
    }
    // Writes after the last line, then the Z80 counts as stopped until the
    // next frame, for a bus request that comes in between
    genesis_sound_queue_drain();
    z80_mark_quiescent();
}

void *sound_thread_main(void *) {
    // Frames from 1 (start_sound_thread() sets the count to 0): reading the
    // count here instead would take a frame handed over before this thread
    // first ran as already done, and both threads would wait forever
    uint32_t seen = 0;
    for (;;) {
        // Spin a little for the next frame, then sleep a tick at a time
        for (int spins = 0; g_sound.start.load(std::memory_order_acquire) == seen &&
                            !g_sound.quit.load(std::memory_order_acquire);
             spins++) {
            if (spins > 2000) {
                usleep(1000);
            }
        }
        if (g_sound.quit.load(std::memory_order_acquire)) {
            break;
        }
        seen = g_sound.start.load(std::memory_order_acquire);
        sound_run_frame(g_sound.screen_height, g_sound.lines);
        g_sound.done.store(seen, std::memory_order_release);
    }
    return nullptr;
}

// Starts the sound thread: the app's starter (set_thread_starter()), else a
// plain pthread
ThreadStarter g_thread_starter = nullptr;

bool start_sound_thread() {
    g_sound.quit.store(false);
    g_sound.start.store(0);
    g_sound.done.store(0);
    g_sound.running =
        (g_thread_starter
             ? g_thread_starter(&g_sound.thread, sound_thread_main, nullptr)
             : pthread_create(&g_sound.thread, nullptr, sound_thread_main, nullptr)) == 0;
    return g_sound.running;
}

void stop_sound_thread() {
    if (!g_sound.running) {
        return;
    }
    g_sound.quit.store(true, std::memory_order_release);
    pthread_join(g_sound.thread, nullptr);
    g_sound.running = false;
}

// The rest (PSRAM on a device): in the ROM's buffer past the padded ROM when
// there's room (slow_size()), else from the heap
struct SlowData {
    uint8_t *work_ram = nullptr;     // 64 KB
    uint8_t *vram = nullptr;         // 64 KB
    int16_t *lfo_pm_table = nullptr; // 32 KB
    uint16_t *op_index = nullptr;    // 128 KB
    bool in_rom_buffer = false;      // else malloc()ed
};
SlowData g_slow;

constexpr size_t kOpIndexBytes = 0x10000 * sizeof(uint16_t);
constexpr size_t kLfoPmBytes = kLfoPmLen * sizeof(int16_t);

// Takes the slow buffers from the heap, largest first: PSRAM on a device is
// fragmented, and the 128 KB one is the hardest to place
bool alloc_slow_from_heap() {
    g_slow.op_index = static_cast<uint16_t *>(std::malloc(kOpIndexBytes));
    g_slow.work_ram = static_cast<uint8_t *>(std::malloc(MAX_RAM_SIZE));
    g_slow.vram = static_cast<uint8_t *>(std::malloc(VRAM_MAX_SIZE));
    g_slow.lfo_pm_table = static_cast<int16_t *>(std::malloc(kLfoPmBytes));
    g_slow.in_rom_buffer = false;
    return g_slow.op_index && g_slow.work_ram && g_slow.vram && g_slow.lfo_pm_table;
}

// Carves them out of `area`, slow_size() bytes
void place_slow(uint8_t *area) {
    g_slow.op_index = reinterpret_cast<uint16_t *>(area);
    area += kOpIndexBytes;
    g_slow.work_ram = area;
    area += MAX_RAM_SIZE;
    g_slow.vram = area;
    area += VRAM_MAX_SIZE;
    g_slow.lfo_pm_table = reinterpret_cast<int16_t *>(area);
    g_slow.in_rom_buffer = true;
}

struct State {
    bool open = false;
    uint8_t *rom = nullptr;
    size_t rom_size = 0;
    int cpu_percent = 100;
    uint32_t buttons = 0;
    uint32_t frames = 0;
    // The frame's sound, mixed down
    int16_t audio[kAudioLength];
    uint32_t audio_count = 0;
    char title[49];
} g;

// Save states: the file gwenesis's saveGwenesisState*() calls read and write
FILE *g_state_file = nullptr;
long g_state_base = 0; // where the records start, after kStateMagic
int g_state_errors = 0;
constexpr char kStateMagic[8] = {'V', 'G', 'E', 'N', 'S', 'T', '0', '1'};

void free_slow() {
    if (!g_slow.in_rom_buffer) {
        std::free(g_slow.work_ram);
        std::free(g_slow.vram);
        std::free(g_slow.lfo_pm_table);
        std::free(g_slow.op_index);
    }
    g_slow = {};
}

// Points gwenesis's globals at the buffers
void attach_memory() {
    std::memset(&g_fast, 0, sizeof g_fast);
    tl_tab = g_fast.tl_tab;
    sin_tab = g_fast.sin_tab;
    ym2612 = &g_fast.ym2612;
    OPNREGS = g_fast.opn_regs;
    m68k = &g_fast.m68k;
    ZRAM = g_fast.zram;
    render_buffer = g_fast.render_buffer;
    sprite_buffer = g_fast.sprite_buffer;
    SAT_CACHE = g_fast.sat_cache;
    CRAM = g_fast.cram;
    CRAM565 = g_fast.cram565;
    VSRAM = g_fast.vsram;
    gwenesis_vdp_regs = g_fast.vdp_regs;
    fifo = g_fast.fifo;
    gwenesis_sn76489_buffer = g_fast.sn76489_buffer;
    gwenesis_ym2612_buffer = g_fast.ym2612_buffer;
    M68K_RAM = g_slow.work_ram;
    VRAM = g_slow.vram;
    lfo_pm_table = g_slow.lfo_pm_table;
    m68ki_op_index = g_slow.op_index;
    m68ki_op_blocks = g_fast.op_blocks;
}

void detach_memory() {
    tl_tab = nullptr;
    sin_tab = nullptr;
    ym2612 = nullptr;
    OPNREGS = nullptr;
    m68k = nullptr;
    ZRAM = nullptr;
    render_buffer = nullptr;
    sprite_buffer = nullptr;
    SAT_CACHE = nullptr;
    CRAM = nullptr;
    CRAM565 = nullptr;
    VSRAM = nullptr;
    gwenesis_vdp_regs = nullptr;
    fifo = nullptr;
    gwenesis_sn76489_buffer = nullptr;
    gwenesis_ym2612_buffer = nullptr;
    M68K_RAM = nullptr;
    VRAM = nullptr;
    lfo_pm_table = nullptr;
    m68ki_op_index = nullptr;
    m68ki_op_blocks = nullptr;
    ROM_DATA = nullptr;
    gwenesis_rom_mask = 0;
}

void apply_cpu_percent() {
    // Cycles charged per instruction scale by 100 / percent, so more
    // instructions fit in each line's share of the frame
    m68k->cycle_ratio = (100 << M68K_OVERCLOCK_SHIFT) / g.cpu_percent;
}

void reset_runtime_state() {
    system_clock = 0;
    scan_line = 0;
    zclk = 0;
    ym2612_index = 0;
    ym2612_clock = 0;
    sn76489_index = 0;
    sn76489_clock = 0;
    std::memset(gwenesis_sn76489_buffer, 0, sizeof g_fast.sn76489_buffer);
    std::memset(gwenesis_ym2612_buffer, 0, sizeof g_fast.ym2612_buffer);
    for (int i = 0; i < 8; i++) {
        gwenesis_io_pad_release_button(0, i);
    }
    g.buttons = 0;
    g.audio_count = 0;
}

void init_sound() {
    // The PSG at the YM2612's output rate (power_on() starts it at another)
    if (REG1_PAL) {
        gwenesis_SN76489_Init(
            3546895, (GWENESIS_AUDIO_BUFFER_LENGTH_PAL / kSampleStep) * GWENESIS_REFRESH_RATE_PAL,
            AUDIO_FREQ_DIVISOR * kSampleStep);
    } else {
        gwenesis_SN76489_Init(
            3579545, (GWENESIS_AUDIO_BUFFER_LENGTH_NTSC / kSampleStep) * GWENESIS_REFRESH_RATE_NTSC,
            AUDIO_FREQ_DIVISOR * kSampleStep);
    }
}

// The ROM is byte-swapped in memory (FETCH8ROM reads address ^ 1)
uint8_t rom_byte(uint32_t address) { return g.rom[(address ^ 1) & gwenesis_rom_mask]; }

void read_title() {
    auto read = [](uint32_t at, char *out) {
        int n = 0;
        for (int i = 0; i < 48; i++) {
            const uint8_t c = rom_byte(at + i);
            out[n++] = (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ';
        }
        // collapse runs of spaces and trim
        int w = 0;
        for (int i = 0; i < n; i++) {
            if (out[i] == ' ' && (w == 0 || out[w - 1] == ' ')) {
                continue;
            }
            out[w++] = out[i];
        }
        while (w > 0 && out[w - 1] == ' ') {
            w--;
        }
        out[w] = '\0';
        return w;
    };
    if (read(0x150, g.title) == 0) {
        read(0x120, g.title);
    }
}

// --- Drawing in spare time -------------------------------------------------
// The VDP draws from a copy of its state (gwenesis_vdp_gfx.c): VRAM and the
// sprite table as they were when the frame started (games change them in
// vertical blanking, before the frame), and each line's registers and VSRAM
// as the 68000 left them after running that line (raster effects: the line
// interrupt's scroll and register changes). Its copy of VRAM is kept between
// frames, and only the 256-byte blocks the 68000 changed since are copied
// into it (gwenesis_vram_dirty). So a frame's lines can be drawn after the
// 68000 has run them: on a device, while the 68000's thread waits for the
// sound thread at the end of each frame and between frames
// (set_spare_time_drawing()), instead of in the frame's own time. A frame is
// recorded only when the last one has been drawn, so the drawing takes the
// time there is and never slows the game; the frames between are skipped.
// Otherwise (the tests, the simulator's app) each line is drawn as soon as
// it's recorded.
//
// Writes to VRAM while a frame is shown (rare: the VDP only takes them at a
// trickle then) reach the next frame drawn instead of the lines below.
//
// Each pixel's top two bits pick one of the palette's four blocks of 64.
// The VDP draws its priority bits there, so each line is turned to the block
// of its palette version as it's copied to the frame: up to four versions a
// frame, one for each change partway down (the water line in Sonic).
struct LineState {
    uint8_t regs[REG_SIZE];
    uint16_t vsram[VSRAM_MAX_SIZE];
};

struct RenderData { // PSRAM on a device
    uint8_t vram[VRAM_MAX_SIZE];
    uint8_t sat[SAT_CACHE_MAX_SIZE];
    // The registers and VSRAM each distinct time they changed, and which of
    // those each line was drawn with
    LineState states[kMaxHeight];
    uint8_t line_state[kMaxHeight];
    // Each line's palette version (0-3)
    uint8_t line_version[kMaxHeight];
};

struct Renderer {
    RenderData *data = nullptr;
    bool vram_valid = false; // else the next frame copies all of it
    // The frame being drawn, from begin_frame()
    uint8_t *frame = nullptr;
    int stride = 0;
    int width = 0;
    int height = 0;
    int states = 0;
    uint32_t cram_writes = 0;
    uint16_t palette[256];
    int versions = 1;
    // The last line the 68000 has recorded, and the lines drawn
    int recorded = -1;
    int drawn = 0;
    // Drawing in spare time (set_spare_time_drawing()): the frame being
    // drawn, until it's presented to `sink`
    bool spare_time = false;
    bool active = false;
    const FrameSink *sink = nullptr;
};
Renderer g_render;
bool g_want_spare_time = false;

void mark_vram_changed() { g_render.vram_valid = false; }

// On the 68000's thread as the frame starts: the copy of VRAM and the sprite
// table, and the frame to draw in
void begin_frame(uint8_t *frame, int stride, int width, int height) {
    Renderer &r = g_render;
    RenderData &d = *r.data;
    if (!r.vram_valid) {
        std::memcpy(d.vram, VRAM, VRAM_MAX_SIZE);
        std::memset(gwenesis_vram_dirty, 0, sizeof gwenesis_vram_dirty);
        r.vram_valid = true;
    } else {
        for (int w = 0; w < 8; w++) {
            for (uint32_t bits = gwenesis_vram_dirty[w]; bits; bits &= bits - 1) {
                const int block = w * 32 + __builtin_ctz(bits);
                std::memcpy(d.vram + block * 256, VRAM + block * 256, 256);
            }
            gwenesis_vram_dirty[w] = 0;
        }
    }
    std::memcpy(d.sat, SAT_CACHE, SAT_CACHE_MAX_SIZE);
    r.frame = frame;
    r.stride = stride;
    r.width = width;
    r.height = height;
    r.states = 0;
    r.versions = 1;
    r.cram_writes = gwenesis_cram_writes;
    std::memcpy(r.palette, CRAM565, 64 * sizeof(uint16_t));
    r.recorded = -1;
    r.drawn = 0;
}

// On the 68000's thread after it ran `line`: what the line is drawn with
void record_line(int line) {
    Renderer &r = g_render;
    RenderData &d = *r.data;
    // A palette change since the last line starts a new version, while
    // there are blocks left (after four, the last takes the rest)
    if (gwenesis_cram_writes != r.cram_writes) {
        r.cram_writes = gwenesis_cram_writes;
        uint16_t *current = r.palette + (r.versions - 1) * 64;
        if (std::memcmp(current, CRAM565, 64 * sizeof(uint16_t)) != 0) {
            if (r.versions < 4) {
                r.versions++;
            }
            std::memcpy(r.palette + (r.versions - 1) * 64, CRAM565, 64 * sizeof(uint16_t));
        }
    }
    d.line_version[line] = static_cast<uint8_t>(r.versions - 1);
    // A new state only when the registers or VSRAM changed: the drawing may
    // be reading the last one, but never one past it
    const LineState *last = r.states ? &d.states[r.states - 1] : nullptr;
    if (!last || std::memcmp(last->regs, gwenesis_vdp_regs, REG_SIZE) != 0 ||
        std::memcmp(last->vsram, VSRAM, sizeof last->vsram) != 0) {
        LineState &next = d.states[r.states++];
        std::memcpy(next.regs, gwenesis_vdp_regs, REG_SIZE);
        std::memcpy(next.vsram, VSRAM, sizeof next.vsram);
    }
    d.line_state[line] = static_cast<uint8_t>(r.states - 1);
    r.recorded = line;
}

// Draws recorded line `line` into the frame, on whichever thread draws
void draw_line(int line) {
    Renderer &r = g_render;
    RenderData &d = *r.data;
    LineState &st = d.states[d.line_state[line]];
    gfx_regs = st.regs;
    gfx_vsram = st.vsram;
    if (line == 0) {
        gfx_vram = d.vram;
        gfx_sat = d.sat;
        gfx_screen_width = r.width;
        gfx_screen_height = r.height;
        gwenesis_vdp_rebuild_sat_cache();
        gwenesis_vdp_render_config();
    }
    uint8_t *row = r.frame + line * r.stride;
    const uint32_t bank = d.line_version[line] * 0x40404040u;
    // With the display off (REG0_DISABLE_DISPLAY) the VDP draws nothing: the
    // backdrop then
    if (st.regs[0] & 1) {
        std::memset(row, (st.regs[7] & 0x3F) | (bank & 0xC0), r.width);
        return;
    }
    gwenesis_vdp_set_buffer(g_fast.line_out);
    gwenesis_vdp_render_line(line);
    // Four pixels a word (widths are 256 or 320; rows are 4-byte aligned)
    const uint32_t *in = reinterpret_cast<const uint32_t *>(g_fast.line_out);
    uint32_t *out = reinterpret_cast<uint32_t *>(row);
    for (int i = 0; i < r.width / 4; i++) {
        out[i] = (in[i] & 0x3F3F3F3Fu) | bank;
    }
}

// After the last line: the blocks no version took repeat the last, for a
// stray index
void finish_frame() {
    Renderer &r = g_render;
    for (int v = r.versions; v < 4; v++) {
        std::memcpy(r.palette + v * 64, r.palette + (r.versions - 1) * 64, 64 * sizeof(uint16_t));
    }
}

// Draws the next recorded line, if there is one, and presents the frame
// after its last. True when it drew one.
bool draw_next_line() {
    Renderer &r = g_render;
    if (!r.active || r.drawn >= r.height || r.drawn > r.recorded) {
        return false;
    }
    draw_line(r.drawn++);
    if (r.drawn == r.height) {
        finish_frame();
        r.active = false;
        if (r.sink && r.sink->present) {
            r.sink->present(r.sink->ctx, r.palette);
        }
    }
    return true;
}

// The 68000 through a frame; unless `threaded`, the Z80 and the sound chips
// too, a line at a time. With `record`, each visible line's VDP state is
// recorded for the drawing, and with `draw` drawn right away.
void run_cpu_frame(bool record, bool draw, int lines_per_frame, bool threaded) {
    auto run_line = [&] {
        system_clock += VDP_CYCLES_PER_LINE;
        m68k_run(system_clock);
        if (threaded) {
            // The sound thread may run this line now: its writes are queued
            g_core0_line.store(scan_line, std::memory_order_release);
        } else {
            z80_run(system_clock);
            gwenesis_SN76489_run(system_clock);
            ym2612_run(system_clock);
        }
    };
    // The active lines: each line's 68000 (and sound), then its picture
    int hint_counter = REG10_LINE_COUNTER;
    for (scan_line = 0; scan_line < screen_height; ++scan_line) {
        run_line();
        // The line counter reloads from reg 10 at line 0 after the 68000 has
        // run the line: Sonic 3 sets its water line in V-int during line 0
        if (scan_line == 0) {
            hint_counter = REG10_LINE_COUNTER;
        }
        if (record) {
            record_line(scan_line);
            if (draw) {
                draw_line(scan_line);
            }
        } else {
            gwenesis_vdp_skip_line(scan_line);
        }
        if (--hint_counter < 0) {
            if (REG0_LINE_INTERRUPT != 0) {
                hint_pending = 1;
                if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0) {
                    m68k_update_irq(4);
                }
            }
            hint_counter = REG10_LINE_COUNTER;
        }
    }

    // V-int comes 788 master cycles into the first blanking line, so a line
    // interrupt from the last visible line is taken first (Sonic 2 and 3's
    // water palette)
    m68k_run(system_clock + kVintDelayCycles);
    if (REG1_VBLANK_INTERRUPT != 0) {
        gwenesis_vdp_status |= STATUS_VIRQPENDING;
        m68k_set_irq(6);
    }
    if (!threaded) {
        // Held until the Z80 takes it (the sound thread does this itself)
        z80_irq_line(1);
    }

    if (scan_line < lines_per_frame) {
        run_line();
        if (--hint_counter < 0 && REG0_LINE_INTERRUPT != 0) {
            hint_pending = 1;
            if ((gwenesis_vdp_status & STATUS_VIRQPENDING) == 0) {
                m68k_update_irq(4);
            }
        }
        ++scan_line;
    }
    for (; scan_line < lines_per_frame; ++scan_line) {
        run_line();
    }
}

} // namespace

size_t slow_size() { return kOpIndexBytes + MAX_RAM_SIZE + VRAM_MAX_SIZE + kLfoPmBytes; }

bool slow_in_rom_buffer() { return g_slow.in_rom_buffer; }

size_t rom_buffer_size(size_t size) {
    size_t p = 1;
    while (p < size) {
        p <<= 1;
    }
    return p;
}

OpenResult open(uint8_t *rom, size_t size, size_t buffer_size) {
    close();
    // The vector table and the header end at 0x200
    if (!rom || size < 0x200 || size > kMaxRomSize + 512 || buffer_size < rom_buffer_size(size)) {
        return OpenResult::BadSize;
    }
    const size_t padded = rom_buffer_size(size);
    if (buffer_size - padded >= slow_size()) {
        place_slow(rom + padded);
    } else if (!alloc_slow_from_heap()) {
        free_slow();
        return OpenResult::NoMemory;
    }
    // The renderer's copy of the VDP's state (PSRAM on a device)
    g_render.data = static_cast<RenderData *>(std::malloc(sizeof(RenderData)));
    if (!g_render.data) {
        free_slow();
        return OpenResult::NoMemory;
    }
    std::memset(g_render.data, 0, sizeof(RenderData));
    mark_vram_changed();
    std::memset(g_slow.vram, 0, VRAM_MAX_SIZE);
    attach_memory();

    // Past the end reads as open bus would, roughly: zeros, mirrored by the mask
    std::memset(rom + size, 0, padded - size);
    g.rom = rom;
    g.rom_size = size;
    gwenesis_rom_mask = static_cast<unsigned int>(padded - 1);

    // As esp-box-emu's init(): the sample step first, then the cartridge,
    // the chips, a reset, and the PSG at the output rate
    YM2612SetSampleStep(kSampleStep);
    load_cartridge(rom, size);
    power_on();
    reset_emulation();
    m68k_idle_reset();
    apply_cpu_percent();
    init_sound();
    reset_runtime_state();
    gwenesis_vdp_set_buffer(g_fast.render_buffer); // replaced per line when drawing
    read_title();
    g.frames = 0;
    g_render.versions = 1;
    std::memcpy(g_render.palette, CRAM565, sizeof g_render.palette);
    genesis_threaded = g_want_threaded ? 1 : 0;
    genesis_sound_queue_reset();
    if (genesis_threaded && !start_sound_thread()) {
        genesis_threaded = 0; // run it all on this thread
    }
    g_render.spare_time = g_want_spare_time;
    g_render.active = false;
    g.open = true;
    return OpenResult::Ok;
}

void close() {
    if (!g.open && !g_slow.work_ram) {
        return;
    }
    g.open = false;
    g_render.active = false;
    stop_sound_thread();
    genesis_threaded = 0;
    detach_memory();
    free_slow();
    std::free(g_render.data);
    g_render.data = nullptr;
    g_render.sink = nullptr;
    gfx_vram = nullptr;
    gfx_regs = nullptr;
    gfx_vsram = nullptr;
    gfx_sat = nullptr;
    g.rom = nullptr;
    g.rom_size = 0;
    g.title[0] = '\0';
}

bool is_open() { return g.open; }

void reset() {
    if (!g.open) {
        return;
    }
    finish_drawing();
    mark_vram_changed();
    // A power cycle without reloading the cartridge (it's already swapped)
    std::memset(M68K_RAM, 0, MAX_RAM_SIZE);
    std::memset(ZRAM, 0, MAX_Z80_RAM_SIZE);
    std::memset(VRAM, 0, VRAM_MAX_SIZE);
    gwenesis_io_reset();
    z80_set_memory(ZRAM);
    power_on();
    reset_emulation();
    m68k_idle_reset();
    apply_cpu_percent();
    init_sound();
    reset_runtime_state();
    g.frames = 0;
}

void set_cpu_percent(int percent) {
    g.cpu_percent = std::clamp(percent, 100, 200);
    if (g.open) {
        apply_cpu_percent();
    }
}

void set_threaded(bool on) { g_want_threaded = on; }
void set_thread_starter(ThreadStarter starter) { g_thread_starter = starter; }
bool is_threaded() { return g.open && genesis_threaded; }

void set_idle_skip(bool on) { m68k_idle_skip_enabled = on ? 1 : 0; }

bool is_pal() { return g.open && REG1_PAL; }
uint32_t sample_rate() { return is_pal() ? kSampleRatePal : kSampleRateNtsc; }
int refresh_rate() { return is_pal() ? GWENESIS_REFRESH_RATE_PAL : GWENESIS_REFRESH_RATE_NTSC; }

void set_buttons(uint32_t buttons) {
    if (!g.open || buttons == g.buttons) {
        return;
    }
    // gwenesis's pad bits, 0-7: up, down, left, right, C, B, A, Start
    for (int i = 0; i < 8; i++) {
        if (buttons & (1u << i)) {
            gwenesis_io_pad_press_button(0, i);
        } else {
            gwenesis_io_pad_release_button(0, i);
        }
    }
    g.buttons = buttons;
}

int frame_width() { return g.open && REG12_MODE_H40 ? 320 : 256; }
int frame_height() { return is_pal() ? 240 : 224; }

namespace {

// One frame. `frame` is drawn in as the frame runs; `sink` is asked for a
// frame to draw in (in spare time, only when the last one is done).
bool run_frame(uint8_t *frame, int stride, const FrameSink *sink) {
    if (!g.open) {
        return false;
    }
    const int lines_per_frame = REG1_PAL ? LINES_PER_FRAME_PAL : LINES_PER_FRAME_NTSC;
    screen_width = frame_width();
    screen_height = frame_height();
    mode_pal = REG1_PAL;

    system_clock = 0;
    zclk = 0;
    ym2612_clock = 0;
    ym2612_index = 0;
    sn76489_clock = 0;
    sn76489_index = 0;

    // Whether this frame is drawn, and where
    bool record = false;
    bool draw_now = false;
    if (sink && g_render.spare_time) {
        // A frame is recorded only once the last one is drawn
        if (!g_render.active) {
            uint8_t *target = sink->begin(sink->ctx, screen_width, screen_height);
            if (target) {
                begin_frame(target, screen_width, screen_width, screen_height);
                g_render.sink = sink;
                g_render.active = true;
                record = true;
            }
        }
    } else if (frame || sink) {
        uint8_t *target = frame ? frame : sink->begin(sink->ctx, screen_width, screen_height);
        if (target) {
            begin_frame(target, frame ? stride : screen_width, screen_width, screen_height);
            record = draw_now = true;
        }
    }

    if (genesis_threaded) {
        // The sound unit's frame on its thread, the 68000's here
        genesis_sound_queue_reset();
        g_core0_line.store(-1, std::memory_order_relaxed);
        g_sound.screen_height = screen_height;
        g_sound.lines = lines_per_frame;
        const uint32_t seq = g_sound.start.load(std::memory_order_relaxed) + 1;
        g_sound.start.store(seq, std::memory_order_release);
        run_cpu_frame(record, draw_now, lines_per_frame, true);
        // Waiting for the sound thread to finish the frame: time to draw
        while (g_sound.done.load(std::memory_order_acquire) != seq) {
            draw_next_line();
        }
    } else {
        run_cpu_frame(record, draw_now, lines_per_frame, false);
    }
    // The next frame's cycles count from 0
    m68k->cycles -= system_clock;

    if (draw_now) {
        finish_frame();
        if (!frame && sink->present) {
            sink->present(sink->ctx, g_render.palette);
        }
    }

    // The sound: the FM and the PSG, mixed to mono
    const int count = std::min(std::max(ym2612_index, sn76489_index), kAudioLength);
    for (int i = 0; i < count; i++) {
        const int fm = i < ym2612_index ? gwenesis_ym2612_buffer[i] : 0;
        const int psg = i < sn76489_index ? gwenesis_sn76489_buffer[i] : 0;
        g.audio[i] = static_cast<int16_t>(std::clamp(fm + psg, -32768, 32767));
    }
    g.audio_count = static_cast<uint32_t>(count);
    g.frames++;
    return true;
}

} // namespace

bool run_frame(uint8_t *frame, int stride) { return run_frame(frame, stride, nullptr); }

bool run_frame(const FrameSink &sink) { return run_frame(nullptr, 0, &sink); }

int draw_spare(int lines) {
    if (!g.open) {
        return 0;
    }
    int n = 0;
    while (n < lines && draw_next_line()) {
        n++;
    }
    return n;
}

void finish_drawing() {
    if (g.open) {
        while (draw_next_line()) {
        }
    }
}

void set_spare_time_drawing(bool on) { g_want_spare_time = on; }

bool spare_time_drawing() { return g.open && g_render.spare_time; }

const uint16_t *palette() { return g_render.palette; }
int palette_versions() { return g_render.versions; }

const int16_t *audio(uint32_t *count) {
    if (count) {
        *count = g.audio_count;
    }
    return g.audio;
}

bool save_state(FILE *f) {
    if (!g.open || !f) {
        return false;
    }
    if (std::fwrite(kStateMagic, sizeof kStateMagic, 1, f) != 1) {
        return false;
    }
    g_state_file = f;
    g_state_base = std::ftell(f);
    g_state_errors = 0;
    gwenesis_save_state();
    g_state_file = nullptr;
    return g_state_errors == 0 && std::fflush(f) == 0;
}

bool load_state(FILE *f) {
    if (!g.open || !f) {
        return false;
    }
    char magic[sizeof kStateMagic];
    if (std::fread(magic, sizeof magic, 1, f) != 1 ||
        std::memcmp(magic, kStateMagic, sizeof magic) != 0) {
        return false;
    }
    finish_drawing();
    mark_vram_changed();
    g_state_file = f;
    g_state_base = std::ftell(f);
    g_state_errors = 0;
    gwenesis_load_state();
    g_state_file = nullptr;
    // Whatever the state left in these, the next frame starts afresh
    reset_runtime_state();
    m68k_idle_reset();
    apply_cpu_percent();
    return g_state_errors == 0;
}

const char *rom_title() { return g.title; }

uint8_t work_ram(uint32_t address) { return g.open ? M68K_RAM[(address & 0xFFFF) ^ 1] : 0; }

uint32_t frame_count() { return g.frames; }

} // namespace genesis

// gwenesis's save state calls (gwenesis_savestate.h): records of a 28-byte
// key, a length and the data, in the file save_state() / load_state() set
extern "C" {

namespace {
struct StateRecord {
    char key[28];
    uint32_t length;
};
} // namespace

SaveState *saveGwenesisStateOpenForRead(const char *) { return reinterpret_cast<SaveState *>(1); }
SaveState *saveGwenesisStateOpenForWrite(const char *) { return reinterpret_cast<SaveState *>(1); }

void saveGwenesisStateSetBuffer(SaveState *, const char *tagName, const void *buffer, int length) {
    using namespace genesis;
    if (!g_state_file) {
        return;
    }
    StateRecord rec{};
    std::strncpy(rec.key, tagName, sizeof rec.key - 1);
    rec.length = static_cast<uint32_t>(length);
    if (std::fwrite(&rec, sizeof rec, 1, g_state_file) != 1 ||
        (length > 0 && std::fwrite(buffer, length, 1, g_state_file) != 1)) {
        g_state_errors++;
    }
}

void saveGwenesisStateGetBuffer(SaveState *, const char *tagName, void *buffer, int length) {
    using namespace genesis;
    if (!g_state_file) {
        return;
    }
    // The records are read in the order they were written, so search on from
    // where the last one ended, then once from the start
    FILE *f = g_state_file;
    const long start = std::ftell(f);
    bool wrapped = false;
    for (;;) {
        StateRecord rec;
        const long at = std::ftell(f);
        if (wrapped && at >= start) {
            break;
        }
        if (std::fread(&rec, sizeof rec, 1, f) != 1) {
            if (wrapped) {
                break;
            }
            std::fseek(f, g_state_base, SEEK_SET);
            wrapped = true;
            continue;
        }
        if (std::strncmp(rec.key, tagName, sizeof rec.key) == 0) {
            const uint32_t n = std::min<uint32_t>(rec.length, static_cast<uint32_t>(length));
            if (n && std::fread(buffer, n, 1, f) != 1) {
                g_state_errors++;
                return;
            }
            if (rec.length > n) {
                std::fseek(f, rec.length - n, SEEK_CUR);
            }
            return;
        }
        if (std::fseek(f, rec.length, SEEK_CUR) != 0) {
            break;
        }
    }
    g_state_errors++;
}

int saveGwenesisStateGet(SaveState *state, const char *tagName) {
    int value = 0;
    saveGwenesisStateGetBuffer(state, tagName, &value, sizeof value);
    return value;
}

void saveGwenesisStateSet(SaveState *state, const char *tagName, int value) {
    saveGwenesisStateSetBuffer(state, tagName, &value, sizeof value);
}

// The pad is set from set_buttons()
void gwenesis_io_get_buttons() {}

// The queue and the status copy between the 68000 and the sound thread
// (genesis_dualcore.h): entries are (kind << 16) | (addr << 8) | value.
// The line-accurate chips ignore the cycle, so order is all that counts.
void genesis_sound_queue_push(uint8_t kind, uint8_t addr, uint8_t value, int) {
    using namespace genesis;
    const uint32_t tail = g_queue_tail.load(std::memory_order_relaxed);
    const uint32_t head = g_queue_head.load(std::memory_order_acquire);
    if (tail - head > kSoundQueueMask) { // full: drop rather than stall the 68000
        g_queue_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_fast.sound_queue[tail & kSoundQueueMask] =
        (static_cast<uint32_t>(kind) << 16) | (static_cast<uint32_t>(addr) << 8) | value;
    g_queue_tail.store(tail + 1, std::memory_order_release);
}

void genesis_sound_queue_drain(void) {
    using namespace genesis;
    uint32_t head = g_queue_head.load(std::memory_order_relaxed);
    const uint32_t tail = g_queue_tail.load(std::memory_order_acquire);
    for (; head != tail; ++head) {
        const uint32_t e = g_fast.sound_queue[head & kSoundQueueMask];
        if (((e >> 16) & 0xFF) == GEN_SND_YM2612) {
            YM2612Write((e >> 8) & 0xFF, e & 0xFF, 0);
        } else {
            gwenesis_SN76489_Write(e & 0xFF, 0);
        }
    }
    g_queue_head.store(head, std::memory_order_release);
}

void genesis_sound_queue_reset(void) {
    using namespace genesis;
    g_queue_head.store(0, std::memory_order_relaxed);
    g_queue_tail.store(0, std::memory_order_relaxed);
}

uint32_t genesis_sound_queue_dropped(void) {
    return genesis::g_queue_dropped.load(std::memory_order_relaxed);
}

void genesis_ym2612_status_publish(unsigned int status) {
    genesis::g_ym_status.store(status, std::memory_order_relaxed);
}

unsigned int genesis_ym2612_status_peek(void) {
    return genesis::g_ym_status.load(std::memory_order_relaxed);
}

} // extern "C"

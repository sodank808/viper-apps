// The Genesis / Mega Drive machine: gwenesis (gwenesis/, vendored from
// esp-cpp/gwenesis) with what its ESP32 port layer did, minus the platform.
// It holds the buffers the core needs, runs a frame at a time, and hands back
// the picture's palette and the frame's sound. No viper_* calls, so the
// simulator's unit tests drive it directly; main.cpp is the app around it.
//
// gwenesis keeps its state in globals, so there is one machine.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <pthread.h>

namespace genesis {

// The pad, as bits of set_buttons()
constexpr uint32_t kUp = 1u << 0;
constexpr uint32_t kDown = 1u << 1;
constexpr uint32_t kLeft = 1u << 2;
constexpr uint32_t kRight = 1u << 3;
constexpr uint32_t kC = 1u << 4;
constexpr uint32_t kB = 1u << 5;
constexpr uint32_t kA = 1u << 6;
constexpr uint32_t kStart = 1u << 7;

// The largest picture: 320 wide (H40; H32 is 256) and 240 lines (PAL V30;
// NTSC is 224)
constexpr int kMaxWidth = 320;
constexpr int kMaxHeight = 240;

// Sound: mono, half the YM2612's native rate (esp-box-emu's sample step of 2)
constexpr uint32_t kSampleRateNtsc = 53267 / 2;
constexpr uint32_t kSampleRatePal = 52781 / 2;
// The most samples a frame makes (PAL)
constexpr int kMaxFrameSamples = 1056 / 2;

// The largest ROM: 4 MB, the most cartridge space without a mapper
constexpr size_t kMaxRomSize = 4 * 1024 * 1024;

// The buffer a ROM of `size` bytes needs: its size rounded up to a power of
// two, which ROM reads are masked to (cartridge space mirrors past the end)
size_t rom_buffer_size(size_t size);

// The rest of the machine's memory (work RAM, VRAM, the opcode index and a
// YM2612 table): 288 KB
size_t slow_size();

enum class OpenResult { Ok, BadSize, NoMemory };

// Starts the machine on the ROM in `rom`, `size` bytes in a buffer of
// `buffer_size`, at least rom_buffer_size(size). The buffer stays the
// machine's until close(); it's byte-swapped in place (and an .smd's
// interleaving undone), and padded. When the buffer has slow_size() to spare
// past the padded ROM, the rest of the machine's memory goes there; otherwise
// it comes from the heap (NoMemory when it can't).
OpenResult open(uint8_t *rom, size_t size, size_t buffer_size);
// Whether open() put that memory in the ROM's buffer
bool slow_in_rom_buffer();
void close();
bool is_open();

// Power-cycles the machine, as the console's reset button
void reset();

// The 68000's clock, as a percentage of the console's (100-200): more lets
// games that slow down keep up, at the cost of more host time per frame
void set_cpu_percent(int percent);
// Runs the Z80 and the sound chips on a thread of their own, beside the 68000
// and the VDP (esp-box-emu's dual-core split), from the next open(). Off by
// default: one thread is deterministic, which the tests want.
void set_threaded(bool on);
// How that thread is started: `starter(thread, fn, arg)` returns 0 or an
// errno, as pthread_create() (which is what it is unless set). The app sets
// one that names it and puts it on the other core (main.cpp).
using ThreadStarter = int (*)(pthread_t *thread, void *(*fn)(void *), void *arg);
void set_thread_starter(ThreadStarter starter);
// Whether the machine runs that way (false before open(), or when the thread
// couldn't be started)
bool is_threaded();
// Skips the 68000's wait-for-next-frame loops (on by default)
void set_idle_skip(bool on);

// PAL timing (313 lines, 50 frames a second): as upstream decides it, while
// the game has the VDP in its 240-line mode, which only PAL consoles have. It
// can change as the game sets the VDP up, so ask each frame.
bool is_pal();
uint32_t sample_rate();
// 60 or 50
int refresh_rate();

// The pad's buttons, held until the next call
void set_buttons(uint32_t buttons);

// The picture's size for the next frame, from the VDP's mode registers
int frame_width();
int frame_height();

// Draws the picture in the 68000's thread's spare time instead of as each
// line is run, from the next open() (genesis_core.cpp, "Drawing in spare
// time"): with run_frame(sink), a frame's lines are recorded as it runs and
// drawn while waiting for the sound thread, and by draw_spare(). Off by
// default.
void set_spare_time_drawing(bool on);
// Whether the machine draws that way (false before open())
bool spare_time_drawing();
// Runs one frame. With `frame` it draws the picture there, frame_width() x
// frame_height() bytes at `stride`, each an index into palette(); without, it
// only emulates (frame skip). Returns false before open().
bool run_frame(uint8_t *frame, int stride);
// Where run_frame(sink) draws. begin(ctx, width, height) is asked for a frame
// of width x height bytes (no padding) as a frame starts, and returns it, or
// null to skip this one; present(ctx, palette) is called once the picture is
// in it, with its 256 colors. The frame stays the machine's until then.
struct FrameSink {
    void *ctx;
    uint8_t *(*begin)(void *ctx, int width, int height);
    void (*present)(void *ctx, const uint16_t *palette);
};
// Runs one frame, drawn into the sink's frame. Drawing in spare time, a
// frame is asked for only when the last one has been drawn (the others are
// skipped), and present() is called once its last line is drawn, during this
// frame, a later one or draw_spare(); otherwise every frame is drawn and
// presented before this returns. `sink` must outlive the drawing
// (finish_drawing()).
bool run_frame(const FrameSink &sink);
// Draws up to `lines` more lines of the frame being drawn in spare time:
// for the time between frames. Returns how many it drew (0 when there's
// nothing to draw).
int draw_spare(int lines);
// Draws the rest of that frame and presents it: before the app draws to the
// screen itself
void finish_drawing();

// The 256 RGB565 colors run_frame()'s indices refer to. A frame uses up to
// four versions of the palette, one for each change partway through it (the
// water line in Sonic), in blocks of 64.
const uint16_t *palette();
// How many versions the last drawn frame used (1-4)
int palette_versions();

// The last frame's sound: mono samples at sample_rate()
const int16_t *audio(uint32_t *count);

// Save states, in gwenesis's own format. False on a file error, or for a file
// that isn't one.
bool save_state(FILE *f);
bool load_state(FILE *f);

// The ROM's name from its header (the overseas name, else the domestic one),
// trimmed; empty before open()
const char *rom_title();

// For the tests: the 68000's work RAM (64 KB) as the 68000 sees it, a byte at
// a time (gwenesis stores it byte-swapped)
uint8_t work_ram(uint32_t address);
// Frames run since open()
uint32_t frame_count();

} // namespace genesis

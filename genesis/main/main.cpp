// The Genesis / Mega Drive app: runs gwenesis (genesis_core.h) as an ELF app.
// `genesis://<rom>` plays a .md, .gen or .smd cartridge: the firmware looks a
// bare name up in the roms/ folders and hands the app the path as its first
// argument. The home screen icon (`genesis://` with no ROM) opens the ROM
// picker.
//
// The pad: the D-pad, Y/B/A as the Genesis A/B/C (their order on the
// Genesis pad, left to right), X as C too, and Start. Select opens the pause
// menu (holding it still closes the app, as everywhere).
//
// The core draws palette indices straight into the frame, which
// viper_frame_set_indexed() has the display stretch to the screen; the ROM
// lives in the firmware's emulator ROM buffer (viper_memory_scratch()). The
// sound (26.6 kHz mono) goes to the firmware's audio stream, which resamples
// it to the output's rate and paces it. See ../README.md.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "genesis_core.h"
#include "gfx/surface.hpp"
#include "gfx/ui.hpp"
#include "viper_app.h"

namespace {

// The most frames skipped for each one drawn
constexpr uint32_t kMaxFrameSkip = 3;

constexpr const char *kCpuNames[] = {"100%", "125%", "150%"};
constexpr int kCpuPercents[] = {100, 125, 150};

enum class Mode { Error, Running, Paused };
enum MenuRow { kResume, kSaveState, kLoadState, kCpu, kReset, kQuit, kRows };

// The menu's look: the system theme, twice the size on screens wide enough
gfx::ui::Theme theme_for(int screen_width) {
    gfx::ui::Theme t = gfx::ui::kSystemTheme;
    t.scale = screen_width >= 300 ? 2 : 1;
    return t;
}

// Half as bright
inline uint16_t dim(uint16_t c) { return (c >> 1) & 0x7BEF; }

class GenesisApp {
  public:
    void create();
    void destroy();
    uint32_t loop(const viper_input_t *now, const viper_input_t *previous);

    // For the simulator's tests
    Mode mode() const { return mode_; }
    const std::string &message() const { return message_; }
    int menu_selected() const { return menu_.selected; }
    uint32_t frames() const { return frames_; }
    uint32_t presented() const { return presented_; }
    bool rom_in_scratch() const { return rom_in_scratch_; }

  private:
    bool start(const char *path);
    void show_error(const std::string &text);
    void draw_error();
    void attach_display();
    uint32_t pad_buttons(uint32_t buttons);
    // One frame: input, emulation, sound, and the picture when it's drawn.
    // Returns how long the frame lasts, in microseconds.
    uint32_t run_frame(uint32_t buttons);
    void adjust_frame_skip(uint32_t frame_us, bool drawn);
    // The machine's frame sink (genesis::FrameSink)
    static uint8_t *sink_begin(void *ctx, int width, int height);
    static void sink_present(void *ctx, const uint16_t *palette);
    void log_timing(uint32_t frame_us, bool presented);

    void open_menu();
    void close_menu();
    bool menu_loop(const viper_input_t *now, const viper_input_t *previous);
    void draw_menu();
    std::string state_path() const;
    void save_state();
    void load_state();

    Mode mode_ = Mode::Error;
    std::string message_;
    std::string rom_name_; // the file's name without its folder or extension
    uint8_t *rom_ = nullptr;
    bool rom_in_scratch_ = false; // else malloc()ed
    bool sound_ = false;          // the stream is open
    uint32_t sound_rate_ = 0;     // at this rate

    // The indexed frame as last set: its size and palette
    int width_ = 0, height_ = 0;
    uint16_t palette_[256];

    uint32_t ignored_ = 0; // buttons held since a menu closed or the game opened
    uint32_t frames_ = 0;
    uint32_t presented_ = 0;
    genesis::FrameSink sink_{this, sink_begin, sink_present};
    bool begun_ = false; // this frame went to the drawing
    uint64_t next_frame_us_ = 0;
    uint32_t frame_us_ = 16667;
    uint32_t frame_skip_ = 0;
    uint32_t skipped_ = 0;  // since the last drawn frame
    uint32_t drawn_us_ = 0; // running averages (adjust_frame_skip())
    uint32_t skipped_us_ = 0;
    uint32_t frames_until_skip_check_ = 0;

    // Timing, logged every ~5 seconds
    struct Timing {
        uint32_t frames = 0, presented = 0;
        uint64_t drawn_us = 0, skipped_us = 0;
        uint32_t slowest_us = 0;
        uint64_t first_us = 0;
    } timing_;

    // The pause menu, drawn in RGB565 over a dimmed copy of the last picture
    std::vector<uint8_t> paused_frame_;
    gfx::ui::Menu menu_;
    gfx::ui::Item items_[kRows];
    int cpu_choice_ = 0;
};

GenesisApp *g_app = nullptr;

void GenesisApp::create() {
    int32_t cpu = 0;
    if (viper_prefs_get_int("cpu", &cpu)) {
        cpu_choice_ = std::clamp<int32_t>(cpu, 0, 2);
    }
    items_[kResume] = gfx::ui::Item::action("RESUME");
    items_[kSaveState] = gfx::ui::Item::action("SAVE STATE");
    items_[kLoadState] = gfx::ui::Item::action("LOAD STATE");
    items_[kCpu] = gfx::ui::Item::choice("68000", cpu_choice_, kCpuNames);
    items_[kReset] = gfx::ui::Item::action("RESET");
    items_[kQuit] = gfx::ui::Item::action("QUIT");
    items_[kQuit].gap_before = true;

    const char *rom = viper_app_arg(0);
    if (!rom || !*rom) {
        show_error("No ROM given.\nOpen a .md or .gen file\nfrom the ROM picker or Files.");
        return;
    }
    if (start(rom)) {
        mode_ = Mode::Running;
    }
}

bool GenesisApp::start(const char *path) {
    const char *slash = std::strrchr(path, '/');
    const std::string file = slash ? slash + 1 : path;
    rom_name_ = file.substr(0, file.rfind('.'));
    if (viper_display_width() < static_cast<uint32_t>(genesis::kMaxWidth) ||
        viper_display_height() < 224) {
        show_error("This screen is smaller\nthan the Genesis picture\n(320 x 224).");
        return false;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        show_error("ROM not found:\n" + file);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0x200 || static_cast<size_t>(size) > genesis::kMaxRomSize + 512) {
        std::fclose(f);
        show_error(size < 0x200 ? "Couldn't read\n" + file
                                : file + ":\ntoo big (games up to\n4 MB without a mapper)");
        return false;
    }
    // The firmware's emulator ROM buffer (2 MB) when the ROM fits, else the
    // heap, which on most devices doesn't have room for more
    const size_t need = genesis::rom_buffer_size(static_cast<size_t>(size));
    size_t scratch_size = 0;
    uint8_t *scratch = static_cast<uint8_t *>(viper_memory_scratch(&scratch_size));
    if (scratch && need <= scratch_size) {
        rom_ = scratch;
        rom_in_scratch_ = true;
    } else {
        rom_ = static_cast<uint8_t *>(std::malloc(need));
        rom_in_scratch_ = false;
    }
    if (!rom_) {
        std::fclose(f);
        show_error(file + ":\nnot enough memory\nfor a ROM this big");
        return false;
    }
    const bool read = std::fread(rom_, 1, size, f) == static_cast<size_t>(size);
    std::fclose(f);
    if (!read) {
        show_error("Couldn't read\n" + file);
        return false;
    }
    // The rest of the machine's memory goes in the ROM buffer too when the
    // ROM leaves room (ROMs up to 1 MB), since the heap rarely has 288 KB
    // in blocks big enough
    // The Z80 and the sound chips on a second thread, on the other core
    // (genesis_core.cpp), at a priority a frame can wait on: below only the
    // display's and the sound's own tasks there. Its stack is internal RAM;
    // the Z80 and the YM2612 need little.
    // The picture is drawn in this thread's spare time: while it waits for
    // the sound thread and between frames (run_frame()). Not in the
    // simulator, whose tests count on each frame being on the screen when
    // the next starts (the unit tests draw that way).
    genesis::set_threaded(true);
#ifdef __XTENSA__
    genesis::set_spare_time_drawing(true);
#endif
    genesis::set_thread_starter([](pthread_t *thread, void *(*fn)(void *), void *arg) {
        return viper_thread_create(thread, "genesis_snd", 1, VIPER_THREAD_PRIORITY_HIGH, 6 * 1024,
                                   fn, arg);
    });
    switch (genesis::open(rom_, static_cast<size_t>(size), rom_in_scratch_ ? scratch_size : need)) {
    case genesis::OpenResult::Ok:
        break;
    case genesis::OpenResult::BadSize:
        show_error(file + ":\nnot a Genesis ROM");
        return false;
    case genesis::OpenResult::NoMemory:
        show_error("Not enough memory\nto run the Genesis.");
        return false;
    }
    genesis::set_cpu_percent(kCpuPercents[cpu_choice_]);
    frame_us_ = 1000000u / static_cast<uint32_t>(genesis::refresh_rate());
    uint32_t free_psram = 0, total_psram = 0;
    viper_memory(VIPER_MEMORY_PSRAM, &free_psram, &total_psram);
    viper_log(
        "Genesis: playing %s (\"%s\"), %ld bytes, %s, ROM in %s, machine in %s, %s; PSRAM free %u",
        path, genesis::rom_title(), size, genesis::is_pal() ? "PAL" : "NTSC",
        rom_in_scratch_ ? "the ROM buffer" : "the heap",
        genesis::slow_in_rom_buffer() ? "the ROM buffer" : "the heap",
        genesis::is_threaded() ? "sound on its own thread" : "one thread",
        static_cast<unsigned>(free_psram));

    // The firmware plays the sound at its own rate and paces it, stretching it
    // when the game runs slow (viper_audio_stream_*())
    sound_rate_ = genesis::sample_rate();
    sound_ = viper_audio_stream_open(sound_rate_, 1);
    attach_display();
    viper_app_handle_back(1); // B is the Genesis B
    // The button that picked the ROM is still down
    ignored_ = ~0u;
    return true;
}

void GenesisApp::destroy() {
    if (sound_) {
        viper_audio_stream_close();
        sound_ = false;
    }
    if (genesis::is_open()) {
        genesis::finish_drawing();
        viper_log("Genesis: ran %u frames, drew %u", static_cast<unsigned>(frames_),
                  static_cast<unsigned>(presented_));
    }
    genesis::close();
    if (rom_ && !rom_in_scratch_) {
        std::free(rom_);
    }
    rom_ = nullptr;
    std::vector<uint8_t>().swap(paused_frame_);
}

void GenesisApp::attach_display() {
    width_ = genesis::frame_width();
    height_ = genesis::frame_height();
    std::memcpy(palette_, genesis::palette(), sizeof palette_);
    viper_frame_set_indexed(width_, height_, palette_);
    // Every drawn frame writes every pixel (genesis::run_frame()), so the
    // display can send one buffer while the next frame goes in the other,
    // instead of a 70 KB copy each frame
    viper_frame_set_swap(1);
}

uint32_t GenesisApp::loop(const viper_input_t *now, const viper_input_t *previous) {
    switch (mode_) {
    case Mode::Error:
        draw_error();
        return 100; // B closes the app
    case Mode::Paused:
        if (!menu_loop(now, previous)) {
            return 16; // closing
        }
        if (mode_ == Mode::Paused) {
            draw_menu();
        }
        return 16;
    case Mode::Running:
        break;
    }
    if (viper_button_pressed(now, previous, VIPER_BUTTON_SELECT)) {
        open_menu();
        draw_menu();
        return 16;
    }
    // Paced by the clock: run the frames that are due, at most two at a time
    // (more when the device can't keep up only makes it worse), and start
    // over when far behind (the first frame, or after the menu)
    uint64_t t = viper_time_us();
    if (next_frame_us_ == 0 || t > next_frame_us_ + 4 * frame_us_) {
        next_frame_us_ = t;
    }
    for (int i = 0; i < 2 && t >= next_frame_us_; i++) {
        next_frame_us_ += run_frame(now->buttons);
        t = viper_time_us();
    }
    // The time until the next frame is due goes to drawing (a line is ~40
    // us), leaving a little to spare
    while (t + 300 < next_frame_us_ && genesis::draw_spare(4) > 0) {
        t = viper_time_us();
    }
    const uint32_t wait_us = next_frame_us_ > t ? static_cast<uint32_t>(next_frame_us_ - t) : 0;
    return std::clamp<uint32_t>(wait_us / 1000, 1, 16);
}

uint32_t GenesisApp::pad_buttons(uint32_t buttons) {
    ignored_ &= buttons; // released buttons count again
    buttons &= ~ignored_;
    uint32_t pad = 0;
    pad |= (buttons & VIPER_BUTTON_UP) ? genesis::kUp : 0;
    pad |= (buttons & VIPER_BUTTON_DOWN) ? genesis::kDown : 0;
    pad |= (buttons & VIPER_BUTTON_LEFT) ? genesis::kLeft : 0;
    pad |= (buttons & VIPER_BUTTON_RIGHT) ? genesis::kRight : 0;
    pad |= (buttons & VIPER_BUTTON_Y) ? genesis::kA : 0;
    pad |= (buttons & VIPER_BUTTON_B) ? genesis::kB : 0;
    pad |= (buttons & (VIPER_BUTTON_A | VIPER_BUTTON_X)) ? genesis::kC : 0;
    pad |= (buttons & VIPER_BUTTON_START) ? genesis::kStart : 0;
    return pad;
}

uint32_t GenesisApp::run_frame(uint32_t buttons) {
    const uint64_t start_us = viper_time_us();
    frame_us_ = 1000000u / static_cast<uint32_t>(genesis::refresh_rate());
    genesis::set_buttons(pad_buttons(buttons));
    // Drawn when the frame skip says so. (Not skipped for the display being
    // busy, as esp-box-emu does: the picture reaches viper_frame_end() ~18 ms
    // after the frame starts, by when the Proto1's display, busy ~32 ms with
    // each picture, is free again if every other frame is drawn.)
    // Drawing in spare time, a frame is drawn whenever the last one is done
    // as it starts (sink_begin()), and presented once its last line is
    // (sink_present())
    const bool spare = genesis::spare_time_drawing();
    const bool draw = spare || skipped_ >= frame_skip_;
    begun_ = false;
    if (spare) {
        genesis::run_frame(sink_);
    } else {
        // The picture's size can change between frames (H32 and H40, NTSC
        // and PAL)
        if (draw && (genesis::frame_width() != width_ || genesis::frame_height() != height_)) {
            attach_display();
        }
        uint8_t *frame = draw ? reinterpret_cast<uint8_t *>(viper_frame_begin()) : nullptr;
        genesis::run_frame(frame, width_);
        begun_ = frame != nullptr;
    }
    frames_++;

    uint32_t count = 0;
    const int16_t *mono = genesis::audio(&count);
    if (count && sound_) {
        // A game switching to PAL timing changes the rate
        if (sound_rate_ != genesis::sample_rate()) {
            sound_rate_ = genesis::sample_rate();
            sound_ = viper_audio_stream_open(sound_rate_, 1);
        }
        viper_audio_stream_frame(mono, count);
    }
    if (!spare && begun_) {
        sink_present(this, genesis::palette());
        skipped_ = 0;
    } else if (!spare) {
        skipped_++;
    }
    const uint32_t frame_us = static_cast<uint32_t>(viper_time_us() - start_us);
    if (!spare) {
        adjust_frame_skip(frame_us, begun_);
    }
    log_timing(frame_us, begun_);

    // As long as the frame's sound lasts, which is the frame's own length
    const uint32_t rate = genesis::sample_rate();
    return count ? (count * 1000000u + rate / 2) / rate : frame_us_;
}

uint8_t *GenesisApp::sink_begin(void *ctx, int width, int height) {
    auto *app = static_cast<GenesisApp *>(ctx);
    // The picture's size can change between frames (H32 and H40, NTSC and
    // PAL). Nothing is being drawn or sent from the app's frame now.
    if (width != app->width_ || height != app->height_) {
        app->attach_display();
    }
    app->begun_ = true;
    return reinterpret_cast<uint8_t *>(viper_frame_begin());
}

void GenesisApp::sink_present(void *ctx, const uint16_t *palette) {
    auto *app = static_cast<GenesisApp *>(ctx);
    // A new palette needs setting before the frame goes out
    if (std::memcmp(app->palette_, palette, sizeof app->palette_) != 0) {
        std::memcpy(app->palette_, palette, sizeof app->palette_);
        viper_frame_set_indexed(app->width_, app->height_, app->palette_);
    }
    viper_frame_end(1);
    app->presented_++;
}

void GenesisApp::adjust_frame_skip(uint32_t frame_us, bool drawn) {
    // Running averages of a drawn frame and of a skipped one, which differ a
    // lot (drawing is about half a drawn frame, and a drawn one includes any
    // wait for the display). Each second, the fewest frames skipped whose
    // predicted average, (drawn + skip * skipped) / (skip + 1), fits in 95% of
    // the frame's length; a level is left only when it's past 100%, so it
    // doesn't flap. (Pacing to a running deadline absorbs the odd long frame.)
    uint32_t &average = drawn ? drawn_us_ : skipped_us_;
    average = average ? (average * 7 + frame_us) / 8 : frame_us;
    if (++frames_until_skip_check_ < 60 || !drawn_us_) {
        return;
    }
    frames_until_skip_check_ = 0;
    if (!skipped_us_) {
        // Nothing skipped yet to measure: skip one when drawing every frame
        // doesn't fit
        if (frame_skip_ == 0 && drawn_us_ > frame_us_) {
            frame_skip_ = 1;
            viper_log("Genesis: frame skip 1 (drawn frames take %u us)",
                      static_cast<unsigned>(drawn_us_));
        }
        return;
    }
    auto predicted = [&](uint32_t skip) { return (drawn_us_ + skip * skipped_us_) / (skip + 1); };
    const uint32_t budget = frame_us_ * 19 / 20;
    uint32_t skip = 0;
    while (skip < kMaxFrameSkip && predicted(skip) > budget) {
        skip++;
    }
    if (skip == frame_skip_ ||
        (skip > frame_skip_ && predicted(frame_skip_) <= budget + frame_us_ / 20)) {
        return;
    }
    frame_skip_ = skip;
    viper_log("Genesis: frame skip %u (drawn frames take %u us, skipped %u us)",
              static_cast<unsigned>(frame_skip_), static_cast<unsigned>(drawn_us_),
              static_cast<unsigned>(skipped_us_));
}

void GenesisApp::log_timing(uint32_t frame_us, bool presented) {
    // Where the time goes, averaged over 300 frames (~5 seconds): frames
    // drawn (emulating, drawing and presenting) and frames skipped
    // (emulating only)
    Timing &t = timing_;
    if (t.frames == 0) {
        t.first_us = viper_time_us() - frame_us;
    }
    t.frames++;
    if (presented) {
        t.presented++;
        t.drawn_us += frame_us;
    } else {
        t.skipped_us += frame_us;
    }
    t.slowest_us = std::max(t.slowest_us, frame_us);
    if (t.frames < 300) {
        return;
    }
    const uint32_t span_us = static_cast<uint32_t>(viper_time_us() - t.first_us);
    const unsigned fps10 = span_us ? t.frames * 10000000u / span_us : 0;
    const uint32_t skipped = t.frames - t.presented;
    viper_log(
        "Genesis: %u.%u fps; %u us a drawn frame (%u of %u), %u us a skipped one; slowest "
        "%u us; frame skip %u; %d palettes; sound stretched %u%%",
        fps10 / 10, fps10 % 10,
        static_cast<unsigned>(t.presented ? static_cast<uint32_t>(t.drawn_us) / t.presented : 0),
        static_cast<unsigned>(t.presented), static_cast<unsigned>(t.frames),
        static_cast<unsigned>(skipped ? static_cast<uint32_t>(t.skipped_us) / skipped : 0),
        static_cast<unsigned>(t.slowest_us), static_cast<unsigned>(frame_skip_),
        genesis::palette_versions(),
        static_cast<unsigned>(viper_audio_stream_stretch() * 100.0f + 0.5f));
    t = {};
}

void GenesisApp::show_error(const std::string &text) {
    viper_log("Genesis: %s", text.c_str());
    message_ = text;
    mode_ = Mode::Error;
    viper_app_handle_back(0); // B goes home
}

void GenesisApp::draw_error() {
    viper_frame_set_indexed(0, 0, nullptr);
    const int w = static_cast<int>(viper_display_width());
    const int h = static_cast<int>(viper_display_height());
    uint16_t *px = viper_frame_begin();
    if (!px) {
        return;
    }
    gfx::Surface s(px, w, h);
    s.fill(s.bounds(), 0);
    gfx::ui::Theme t = theme_for(w);
    // One centered line per '\n'
    // (by hand: std::string::find() calls memchr, which isn't exported)
    std::vector<std::string> lines(1);
    for (char c : message_) {
        if (c == '\n') {
            lines.emplace_back();
        } else {
            lines.back() += c;
        }
    }
    while (t.scale > 1 && std::any_of(lines.begin(), lines.end(), [&](const std::string &l) {
               return gfx::text_width(*t.font, l, t.scale) > w - 16;
           })) {
        t.scale--;
    }
    const int pitch = gfx::ui::Menu::pitch(t, t.scale);
    int y = (h - pitch * static_cast<int>(lines.size())) / 2;
    for (const auto &line : lines) {
        gfx::ui::text_centered(s, t, line, 0, w, y, gfx::rgb_hex(0xff6060), t.scale);
        y += pitch;
    }
    viper_frame_end(1);
}

void GenesisApp::open_menu() {
    genesis::finish_drawing();
    // Keep the picture to draw the menu over: the indexed frame as the last
    // drawn frame left it
    const auto *frame = reinterpret_cast<const uint8_t *>(viper_frame_begin());
    paused_frame_.assign(frame, frame + width_ * height_);
    menu_.selected = kResume;
    mode_ = Mode::Paused;
    // The menu draws in RGB565 over the whole screen
    viper_frame_set_indexed(0, 0, nullptr);
    viper_log("Genesis: paused");
}

void GenesisApp::close_menu() {
    std::vector<uint8_t>().swap(paused_frame_);
    attach_display();
    // The button that closed the menu isn't the game's
    ignored_ = ~0u;
    next_frame_us_ = 0;
    mode_ = Mode::Running;
}

std::string GenesisApp::state_path() const {
    const char *dir = viper_data_path();
    return dir ? std::string(dir) + "/" + rom_name_ + ".state" : std::string();
}

void GenesisApp::save_state() {
    const std::string path = state_path();
    if (path.empty()) {
        viper_log("Genesis: no storage for a save state");
        return;
    }
    // The folder and the ones above it, which may not be there yet (each
    // mkdir() fails harmlessly when its folder is)
    const std::string dir = viper_data_path();
    for (size_t i = 1; i <= dir.size(); i++) {
        if (i == dir.size() || dir[i] == '/') {
            mkdir(dir.substr(0, i).c_str(), 0777);
        }
    }
    // Written beside it, then put in its place, so a power cut leaves the
    // old state or the new one
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    bool ok = f && genesis::save_state(f);
    if (f) {
        ok = (std::fclose(f) == 0) && ok;
    }
    if (ok) {
        std::remove(path.c_str());
        ok = std::rename(tmp.c_str(), path.c_str()) == 0;
    } else {
        std::remove(tmp.c_str());
    }
    viper_log("Genesis: %s %s", ok ? "saved state to" : "couldn't save state to", path.c_str());
    viper_haptic(ok ? VIPER_HAPTIC_SHARP_CLICK : VIPER_HAPTIC_DOUBLE_CLICK);
}

void GenesisApp::load_state() {
    const std::string path = state_path();
    FILE *f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
    const bool ok = f && genesis::load_state(f);
    if (f) {
        std::fclose(f);
    }
    viper_log("Genesis: %s %s", ok ? "loaded state from" : "no state loaded from", path.c_str());
    viper_haptic(ok ? VIPER_HAPTIC_SHARP_CLICK : VIPER_HAPTIC_DOUBLE_CLICK);
}

bool GenesisApp::menu_loop(const viper_input_t *now, const viper_input_t *previous) {
    auto pressed = [&](uint32_t b) { return viper_button_pressed(now, previous, b) != 0; };
    gfx::ui::Keys keys;
    keys.up = pressed(VIPER_BUTTON_UP);
    keys.down = pressed(VIPER_BUTTON_DOWN);
    keys.left = pressed(VIPER_BUTTON_LEFT);
    keys.right = pressed(VIPER_BUTTON_RIGHT);
    keys.a = pressed(VIPER_BUTTON_A);
    keys.b = pressed(VIPER_BUTTON_B) || pressed(VIPER_BUTTON_SELECT);
    const int cpu_before = cpu_choice_;
    const gfx::ui::Result r = menu_.update(items_, keys);
    if (cpu_choice_ != cpu_before) {
        genesis::set_cpu_percent(kCpuPercents[cpu_choice_]);
        viper_prefs_set_int("cpu", cpu_choice_);
    }
    switch (r.what) {
    case gfx::ui::Result::Back:
        close_menu();
        break;
    case gfx::ui::Result::Chosen:
        if (r.item == kResume) {
            close_menu();
        } else if (r.item == kSaveState) {
            save_state();
            close_menu();
        } else if (r.item == kLoadState) {
            load_state();
            close_menu();
        } else if (r.item == kReset) {
            genesis::reset();
            viper_log("Genesis: reset");
            close_menu();
        } else if (r.item == kQuit) {
            viper_app_exit(); // destroy() runs once loop() returns
            return false;
        }
        break;
    default:
        break;
    }
    return true;
}

void GenesisApp::draw_menu() {
    const int w = static_cast<int>(viper_display_width());
    const int h = static_cast<int>(viper_display_height());
    uint16_t *px = viper_frame_begin();
    if (!px || paused_frame_.empty()) {
        return;
    }
    // The picture stretched to the screen as scan-out shows it, half as bright
    for (int y = 0; y < h; y++) {
        const uint8_t *src = paused_frame_.data() + (y * height_ / h) * width_;
        uint16_t *dst = px + y * w;
        for (int x = 0; x < w; x++) {
            dst[x] = dim(palette_[src[x * width_ / w]]);
        }
    }
    gfx::Surface s(px, w, h);
    const gfx::ui::Theme t = theme_for(w);
    int text_w = gfx::text_width(*t.font, "PAUSED", t.scale);
    char buf[16];
    for (const auto &item : items_) {
        std::string line = std::string("> ") + item.label;
        if (const char *v = gfx::ui::Menu::value_text(item, buf); *v) {
            line += std::string(" ") + v;
        }
        text_w = std::max(text_w, gfx::text_width(*t.font, line, t.scale));
    }
    const int box_w = std::min(w - 8, text_w + 16 * t.scale);
    const int box_h = gfx::ui::dialog_height(t, &menu_, items_);
    const gfx::Rect r{(w - box_w) / 2, (h - box_h) / 2, box_w, box_h};
    gfx::ui::dialog(s, r, t, "PAUSED", &menu_, items_);
    viper_frame_end(1);
}

void create(void *) {
    // Globals outlive a run in the simulator: start from nothing
    g_app = new GenesisApp();
    g_app->create();
}

void destroy() {
    if (g_app) {
        g_app->destroy();
        delete g_app;
        g_app = nullptr;
    }
}

uint32_t loop(const viper_input_t *now, const viper_input_t *previous) {
    return g_app ? g_app->loop(now, previous) : 16;
}

const viper_app_t kApp = {
    .magic = VIPER_APP_MAGIC,
    .abi_version = VIPER_APP_ABI_VERSION,
    .render_engine = VIPER_RENDER_VIDEO_BLIT,
    .create = create,
    .resume = nullptr,
    .pause = nullptr,
    .destroy = destroy,
    .loop = loop,
};

} // namespace

VIPER_APP(kApp)

#ifdef VIPER_APP_STATIC_NAME
// What the simulator's boot tests look at (tests/test_genesis_boot.cpp).
// The simulator links apps into one program, so these are plain functions.
// mode: -1 not running, 0 error, 1 running, 2 paused.
extern "C" int viper_sim_genesis_mode(void) { return g_app ? static_cast<int>(g_app->mode()) : -1; }
extern "C" const char *viper_sim_genesis_message(void) {
    return g_app ? g_app->message().c_str() : "";
}
extern "C" int viper_sim_genesis_menu_selected(void) { return g_app ? g_app->menu_selected() : -1; }
extern "C" uint32_t viper_sim_genesis_frames(void) { return g_app ? g_app->frames() : 0; }
extern "C" uint32_t viper_sim_genesis_presented(void) { return g_app ? g_app->presented() : 0; }
extern "C" bool viper_sim_genesis_rom_in_scratch(void) { return g_app && g_app->rom_in_scratch(); }
#endif

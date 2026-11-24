// The Genesis machine (genesis/main/genesis_core.h) on smoke.md, which the
// firmware repo's sim/tests/roms/make_test_roms.py builds: a white bar of tiles across row 12
// on a blue backdrop that an H-int turns red partway down, a green backdrop
// while A is held, a PSG tone from the 68000 and a square wave the Z80 plays
// through the YM2612's DAC. Tagged [genesis].

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "genesis_core.h"
#include "smoke_md.h"

namespace fs = std::filesystem;

namespace {

// gwenesis's CRAM to RGB565 (to_pixel() in gwenesis_vdp_mem.c)
uint16_t rgb565(uint16_t cram) {
    return static_cast<uint16_t>(((cram & 0xE00) >> 7) | ((cram & 0x0E0) << 3) |
                                 ((cram & 0x00E) << 12));
}

std::vector<uint8_t> read_rom(const char *name) {
    std::ifstream in(fs::path(VIPER_SIM_TEST_ROMS) / name, std::ios::binary);
    REQUIRE(in);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// The machine on smoke.md, closed again at the end of the test
struct Machine {
    std::vector<uint8_t> rom;
    std::vector<uint8_t> frame = std::vector<uint8_t>(genesis::kMaxWidth * genesis::kMaxHeight);

    explicit Machine(const char *name = "smoke.md") {
        const auto file = read_rom(name);
        rom.assign(genesis::rom_buffer_size(file.size()), 0xAA);
        std::copy(file.begin(), file.end(), rom.begin());
        REQUIRE(genesis::open(rom.data(), file.size(), rom.size()) == genesis::OpenResult::Ok);
    }
    ~Machine() { genesis::close(); }

    void run(int frames, bool draw = true) {
        for (int i = 0; i < frames; i++) {
            REQUIRE(genesis::run_frame(draw ? frame.data() : nullptr, genesis::frame_width()));
        }
    }
    uint16_t color_at(int x, int y) const {
        return genesis::palette()[frame[y * genesis::frame_width() + x]];
    }
    uint32_t frames_counted() const {
        uint32_t v = 0;
        for (uint32_t i = 0; i < 4; i++) {
            v = (v << 8) | genesis::work_ram(kSmokeMdFrames + i);
        }
        return v;
    }
};

} // namespace

TEST_CASE("Genesis: the ROM buffer is the ROM rounded up to a power of two", "[genesis]") {
    CHECK(genesis::rom_buffer_size(0x10000) == 0x10000);
    CHECK(genesis::rom_buffer_size(0x10001) == 0x20000);
    CHECK(genesis::rom_buffer_size(3 * 1024 * 1024) == 4 * 1024 * 1024);
    std::vector<uint8_t> small(0x100);
    CHECK(genesis::open(small.data(), small.size(), small.size()) == genesis::OpenResult::BadSize);
    CHECK_FALSE(genesis::is_open());
    std::vector<uint8_t> rom(0x10000);
    // A buffer too small for the padding
    CHECK(genesis::open(rom.data(), 0x8001, 0x8001) == genesis::OpenResult::BadSize);
    CHECK_FALSE(genesis::run_frame(nullptr, 0));
}

TEST_CASE("Genesis: the machine's memory goes in the ROM's buffer when there's room", "[genesis]") {
    const auto file = read_rom("smoke.md");
    for (const bool room : {false, true}) {
        INFO("room " << room);
        std::vector<uint8_t> buffer(file.size() + (room ? genesis::slow_size() : 0), 0xAA);
        std::copy(file.begin(), file.end(), buffer.begin());
        REQUIRE(genesis::open(buffer.data(), file.size(), buffer.size()) ==
                genesis::OpenResult::Ok);
        CHECK(genesis::slow_in_rom_buffer() == room);
        std::vector<uint8_t> frame(genesis::kMaxWidth * genesis::kMaxHeight);
        for (int i = 0; i < 10; i++) {
            REQUIRE(genesis::run_frame(frame.data(), genesis::frame_width()));
        }
        CHECK(genesis::work_ram(kSmokeMdZ80Mark) == kSmokeMdZ80MarkValue);
        CHECK(genesis::palette()[frame[(kSmokeMdBarRow * 8 + 4) * 320 + kSmokeMdBarCol * 8]] ==
              rgb565(kSmokeMdBarColor));
        genesis::close();
    }
}

TEST_CASE("Genesis: smoke.md reads its header and runs at NTSC", "[genesis]") {
    Machine m;
    CHECK(std::string(genesis::rom_title()) == "VIPER SMOKE TEST");
    CHECK_FALSE(genesis::is_pal());
    CHECK(genesis::refresh_rate() == 60);
    CHECK(genesis::sample_rate() == genesis::kSampleRateNtsc);
    m.run(30);
    // It set H40 and the display's 224 lines
    CHECK(genesis::frame_width() == 320);
    CHECK(genesis::frame_height() == 224);
    // A V-int a frame (the first comes once the program turns them on)
    const uint32_t counted = m.frames_counted();
    CHECK(counted >= 28);
    CHECK(counted <= 30);
    CHECK(genesis::frame_count() == 30);
}

TEST_CASE("Genesis: smoke.md draws its bar, and the H-int's backdrop below its line", "[genesis]") {
    Machine m;
    m.run(10);
    // The backdrop above the bar, the bar, and the backdrop between the bar
    // and the H-int's line
    const int bar_y = kSmokeMdBarRow * 8 + 4;
    const int bar_x0 = kSmokeMdBarCol * 8;
    const int bar_x1 = (kSmokeMdBarCol + kSmokeMdBarTiles) * 8;
    CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdrop));
    CHECK(m.color_at(bar_x0, bar_y) == rgb565(kSmokeMdBarColor));
    CHECK(m.color_at(bar_x1 - 1, bar_y) == rgb565(kSmokeMdBarColor));
    CHECK(m.color_at(bar_x0 - 1, bar_y) == rgb565(kSmokeMdBackdrop));
    CHECK(m.color_at(bar_x1, bar_y) == rgb565(kSmokeMdBackdrop));
    CHECK(m.color_at(160, kSmokeMdHintLine - 1) == rgb565(kSmokeMdBackdrop));
    // From the H-int's line on, the same index range draws in red: the frame
    // used a second palette version
    CHECK(genesis::palette_versions() == 2);
    CHECK(m.color_at(160, kSmokeMdHintLine) == rgb565(kSmokeMdBackdropLower));
    CHECK(m.color_at(160, genesis::frame_height() - 1) == rgb565(kSmokeMdBackdropLower));
    // Every pixel is one of the three colors
    for (int y = 0; y < genesis::frame_height(); y++) {
        for (int x = 0; x < genesis::frame_width(); x++) {
            const uint16_t c = m.color_at(x, y);
            INFO("at " << x << "," << y);
            REQUIRE((c == rgb565(kSmokeMdBackdrop) || c == rgb565(kSmokeMdBarColor) ||
                     c == rgb565(kSmokeMdBackdropLower)));
        }
    }
}

TEST_CASE("Genesis: the pad reaches the game", "[genesis]") {
    Machine m;
    m.run(5);
    // Nothing held: every button reads high
    CHECK((genesis::work_ram(kSmokeMdPadTh1) & 0x3F) == 0x3F);
    CHECK((genesis::work_ram(kSmokeMdPadTh0) & 0x30) == 0x30);
    genesis::set_buttons(genesis::kA);
    m.run(3);
    CHECK((genesis::work_ram(kSmokeMdPadTh0) & 0x10) == 0); // A
    CHECK((genesis::work_ram(kSmokeMdPadTh0) & 0x20) != 0); // Start
    // The game turned the backdrop green
    CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdropPressed));
    genesis::set_buttons(genesis::kB | genesis::kC | genesis::kUp | genesis::kRight |
                         genesis::kStart);
    m.run(2);
    // TH high: 0 0 C B R L D U, low when held
    CHECK((genesis::work_ram(kSmokeMdPadTh1) & 0x3F) == (0x3F & ~0x20 & ~0x10 & ~0x08 & ~0x01));
    CHECK((genesis::work_ram(kSmokeMdPadTh0) & 0x30) == 0x10); // Start held, A not
    CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdrop));
    genesis::set_buttons(0);
    m.run(2);
    CHECK((genesis::work_ram(kSmokeMdPadTh1) & 0x3F) == 0x3F);
}

TEST_CASE("Genesis: the Z80 runs its program and both sound chips play", "[genesis]") {
    Machine m;
    m.run(20);
    CHECK(genesis::work_ram(kSmokeMdZ80Mark) == kSmokeMdZ80MarkValue);
    const uint8_t loops = genesis::work_ram(kSmokeMdZ80Loops);
    m.run(5);
    CHECK(genesis::work_ram(kSmokeMdZ80Loops) != loops);

    // A frame's sound: 26,633 samples a second at 60 frames
    uint32_t count = 0;
    const int16_t *samples = genesis::audio(&count);
    CHECK(count >= 440);
    CHECK(count <= 448);
    const auto [lo, hi] = std::minmax_element(samples, samples + count);
    INFO("samples " << *lo << ".." << *hi);
    CHECK(*hi - *lo > 2000);
    // and it's not silent without drawing either
    m.run(1, false);
    samples = genesis::audio(&count);
    CHECK(count >= 440);
    const auto [lo2, hi2] = std::minmax_element(samples, samples + count);
    CHECK(*hi2 - *lo2 > 2000);
}

TEST_CASE("Genesis: frames that aren't drawn still run the game", "[genesis]") {
    Machine m;
    m.run(20, false);
    CHECK(m.frames_counted() >= 18);
    CHECK(genesis::work_ram(kSmokeMdZ80Mark) == kSmokeMdZ80MarkValue);
}

TEST_CASE("Genesis: a save state brings the machine back exactly", "[genesis]") {
    const fs::path path = fs::temp_directory_path() / "viper_genesis_test.state";
    Machine m;
    m.run(15);
    {
        FILE *f = std::fopen(path.string().c_str(), "wb");
        REQUIRE(f);
        CHECK(genesis::save_state(f));
        std::fclose(f);
    }
    // Run on with A held, and keep what comes out
    genesis::set_buttons(genesis::kA);
    m.run(10);
    const std::vector<uint8_t> frame_after = m.frame;
    const uint32_t counted_after = m.frames_counted();
    uint32_t count = 0;
    const int16_t *s = genesis::audio(&count);
    const std::vector<int16_t> audio_after(s, s + count);

    // Then go back and do the same again
    genesis::set_buttons(0);
    m.run(7);
    {
        FILE *f = std::fopen(path.string().c_str(), "rb");
        REQUIRE(f);
        CHECK(genesis::load_state(f));
        std::fclose(f);
    }
    genesis::set_buttons(genesis::kA);
    m.run(10);
    CHECK(m.frames_counted() == counted_after);
    CHECK(m.frame == frame_after);
    s = genesis::audio(&count);
    CHECK(std::vector<int16_t>(s, s + count) == audio_after);

    // Something that isn't a save state is refused
    {
        FILE *f = std::fopen(path.string().c_str(), "wb");
        REQUIRE(f);
        std::fputs("not a save state", f);
        std::fclose(f);
        f = std::fopen(path.string().c_str(), "rb");
        CHECK_FALSE(genesis::load_state(f));
        std::fclose(f);
    }
    fs::remove(path);
}

TEST_CASE("Genesis: reset starts the game over", "[genesis]") {
    Machine m;
    m.run(30);
    CHECK(m.frames_counted() >= 28);
    genesis::reset();
    CHECK(genesis::frame_count() == 0);
    m.run(5);
    CHECK(m.frames_counted() <= 5);
    CHECK(m.color_at(kSmokeMdBarCol * 8, kSmokeMdBarRow * 8) == rgb565(kSmokeMdBarColor));
}

TEST_CASE("Genesis: the 68000 clock setting gives the game more cycles", "[genesis]") {
    // Not much to see in smoke.md, which waits for V-int: it runs the same
    genesis::set_cpu_percent(150);
    {
        Machine m;
        m.run(10);
        CHECK(m.frames_counted() >= 8);
        CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdrop));
    }
    genesis::set_cpu_percent(100);
}

TEST_CASE("Genesis: the sound unit on its own thread plays the same game", "[genesis]") {
    genesis::set_threaded(true);
    {
        Machine m;
        REQUIRE(genesis::is_threaded());
        m.run(30);
        CHECK(m.frames_counted() >= 28);
        // The picture, the palette change partway down, the pad
        CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdrop));
        CHECK(m.color_at(160, kSmokeMdHintLine) == rgb565(kSmokeMdBackdropLower));
        genesis::set_buttons(genesis::kA);
        m.run(3);
        CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdropPressed));
        genesis::set_buttons(0);
        // The Z80 ran its program, and both chips play
        CHECK(genesis::work_ram(kSmokeMdZ80Mark) == kSmokeMdZ80MarkValue);
        const uint8_t loops = genesis::work_ram(kSmokeMdZ80Loops);
        m.run(5, false);
        CHECK(genesis::work_ram(kSmokeMdZ80Loops) != loops);
        uint32_t count = 0;
        const int16_t *samples = genesis::audio(&count);
        CHECK(count >= 440);
        const auto [lo, hi] = std::minmax_element(samples, samples + count);
        CHECK(*hi - *lo > 2000);
        // A reset and a save state round trip with the thread running
        genesis::reset();
        m.run(10);
        CHECK(m.color_at(kSmokeMdBarCol * 8, kSmokeMdBarRow * 8) == rgb565(kSmokeMdBarColor));
    }
    // Closing stopped it; the next machine is one thread again
    CHECK_FALSE(genesis::is_threaded());
    genesis::set_threaded(false);
}

namespace {

// A frame sink (genesis::FrameSink) with two buffers taking turns, as the
// device's display does, keeping the last presented picture as colors
struct Screen {
    std::vector<uint8_t> buffers[2] = {
        std::vector<uint8_t>(genesis::kMaxWidth * genesis::kMaxHeight),
        std::vector<uint8_t>(genesis::kMaxWidth *genesis::kMaxHeight)};
    int drawing = 0;
    int width = 0, height = 0;
    std::vector<uint16_t> shown;
    int begun = 0, presented = 0;
    genesis::FrameSink sink{this, begin, present};

    static uint8_t *begin(void *ctx, int width, int height) {
        auto *s = static_cast<Screen *>(ctx);
        s->width = width;
        s->height = height;
        s->begun++;
        return s->buffers[s->drawing].data();
    }
    static void present(void *ctx, const uint16_t *palette) {
        auto *s = static_cast<Screen *>(ctx);
        const auto &frame = s->buffers[s->drawing];
        s->shown.resize(static_cast<size_t>(s->width) * s->height);
        for (size_t i = 0; i < s->shown.size(); i++) {
            s->shown[i] = palette[frame[i]];
        }
        s->drawing ^= 1;
        s->presented++;
    }
    uint16_t color_at(int x, int y) const { return shown[y * width + x]; }
};

} // namespace

TEST_CASE("Genesis: drawing in spare time draws what drawing inline does", "[genesis]") {
    // The picture of frame 10 drawn on the 68000's thread
    std::vector<uint16_t> inline_colors;
    {
        Machine m;
        m.run(10);
        for (int y = 0; y < genesis::frame_height(); y++) {
            for (int x = 0; x < genesis::frame_width(); x++) {
                inline_colors.push_back(m.color_at(x, y));
            }
        }
    }
    // A sink otherwise draws every frame, before run_frame() returns
    {
        Machine m;
        CHECK_FALSE(genesis::spare_time_drawing());
        Screen screen;
        for (int i = 0; i < 10; i++) {
            REQUIRE(genesis::run_frame(screen.sink));
            CHECK(screen.presented == i + 1);
        }
        CHECK(screen.shown == inline_colors);
    }
    // In spare time (with the sound thread to wait for), frames are drawn
    // after the 68000 has run them, while it waits and between frames; the
    // smoke ROM's picture is the same from frame to frame
    genesis::set_threaded(true);
    genesis::set_spare_time_drawing(true);
    {
        Machine m;
        REQUIRE(genesis::spare_time_drawing());
        Screen screen;
        for (int i = 0; i < 30; i++) {
            REQUIRE(genesis::run_frame(screen.sink));
            // Every third frame, a few lines between frames
            if (i % 3 == 0) {
                genesis::draw_spare(20);
            }
        }
        genesis::finish_drawing();
        CHECK(screen.presented >= 1);
        CHECK(screen.presented == screen.begun);
        CHECK(screen.width == 320);
        CHECK(screen.height == 224);
        CHECK(screen.shown == inline_colors);
        // The pad reaches the picture: drawn frames keep up with the game
        genesis::set_buttons(genesis::kA);
        for (int i = 0; i < 10; i++) {
            REQUIRE(genesis::run_frame(screen.sink));
        }
        genesis::finish_drawing();
        CHECK(screen.color_at(10, 10) == rgb565(kSmokeMdBackdropPressed));
        genesis::set_buttons(0);
        // A reset while a frame may still be drawing
        REQUIRE(genesis::run_frame(screen.sink));
        genesis::reset();
        for (int i = 0; i < 10; i++) {
            REQUIRE(genesis::run_frame(screen.sink));
        }
        genesis::finish_drawing();
        CHECK(screen.color_at(kSmokeMdBarCol * 8, kSmokeMdBarRow * 8) == rgb565(kSmokeMdBarColor));
    }
    CHECK_FALSE(genesis::spare_time_drawing());
    genesis::set_spare_time_drawing(false);
    genesis::set_threaded(false);
}

TEST_CASE("Genesis: interlace mode 2 draws its even field", "[genesis]") {
    // smoke_im2.md is smoke.md with the VDP in interlace mode 2 (Sonic 2's
    // two-player mode), its tile 8x16: the bar comes out on the same lines,
    // where upstream drew only the backdrop
    Machine m("genesis/smoke_im2.md");
    m.run(10);
    CHECK(genesis::frame_height() == 224);
    const int bar_y = kSmokeMdBarRow * 8 + 4;
    CHECK(m.color_at(kSmokeMdBarCol * 8, bar_y) == rgb565(kSmokeMdBarColor));
    CHECK(m.color_at((kSmokeMdBarCol + kSmokeMdBarTiles) * 8 - 1, bar_y) ==
          rgb565(kSmokeMdBarColor));
    CHECK(m.color_at(10, 10) == rgb565(kSmokeMdBackdrop));
    CHECK(m.color_at(kSmokeMdBarCol * 8, kSmokeMdBarRow * 8 - 1) == rgb565(kSmokeMdBackdrop));
    CHECK(m.color_at(kSmokeMdBarCol * 8, kSmokeMdBarRow * 8 + 8) == rgb565(kSmokeMdBackdrop));
}

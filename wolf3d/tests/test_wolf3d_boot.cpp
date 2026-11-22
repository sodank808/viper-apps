// Wolfenstein 3D's boot tests: the app running on the whole OS in the sim
// (viper_sim_app_boot_tests; helpers in sim/tests/boot_test_util.hpp)
//
// The tests install the app themselves (Installed), as `make` would, so they
// run without a build. The game's data isn't the repo's (README.md): the test
// that plays needs the shareware *.WL1 files, which wolf3d/tools/
// fetch_shareware.sh puts in wolf3d/assets/, or the folder
// VIPER_SIM_WOLF3D_DATA names, and skips without them.
// VIPER_SIM_WOLF3D_SHOTS=<dir> saves its screens.

#include "boot_test_util.hpp"

#include "app_storage.hpp"
#include "elf_app.hpp"
#include "paths.hpp"
#include "wolf3d_app.h"

namespace {

constexpr const char *kBundle = "app.viper.wolf3d";

fs::path installed() { return viper::paths::fatfs() / "apps" / kBundle; }

void wolf_shot(const char *name) {
    if (const char *dir = std::getenv("VIPER_SIM_WOLF3D_SHOTS")) {
        write_ppm(viper_sim::panel_snapshot(),
                  (fs::path(dir) / (std::string(name) + ".ppm")).c_str());
    }
}

std::vector<fs::path> data_files(const fs::path &dir) {
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator(dir, ec)) {
        std::string ext = e.path().extension().string();
        for (auto &c : ext) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        if (ext == ".wl1") {
            files.push_back(e.path());
        }
    }
    return files;
}

// The app's folder in the repo (wolf3d/), from this file's
fs::path app_folder() { return fs::path(__FILE__).parent_path().parent_path(); }

// The app installed on the sim's fatfs, as `make` installs it, with the
// shareware data if it's been fetched (wolf3d/assets/, VIPER_SIM_WOLF3D_DATA),
// for as long as this lives. The sim runs the app's code compiled in, so the
// app.elf is a stand-in. An install that was there already (from `make
// apps`) stays.
class Installed {
  public:
    Installed() {
        const fs::path dir = installed();
        ours_ = !fs::exists(dir / "app.elf");
        fs::create_directories(dir);
        for (const char *f : {"info.json", "icon.png"}) {
            fs::copy_file(app_folder() / f, dir / f, fs::copy_options::overwrite_existing);
        }
        if (ours_) {
            std::ofstream(dir / "app.elf") << "ELF";
        }
        std::vector<fs::path> data = data_files(app_folder() / "assets");
        if (const char *from = std::getenv("VIPER_SIM_WOLF3D_DATA")) {
            data = data_files(from);
        }
        for (const auto &f : data) {
            fs::copy_file(f, dir / f.filename(), fs::copy_options::overwrite_existing);
        }
        elf_apps::rescan();
    }
    ~Installed() {
        if (ours_) {
            fs::remove_all(installed());
            elf_apps::rescan();
        }
    }
    Installed(const Installed &) = delete;
    Installed &operator=(const Installed &) = delete;

  private:
    bool ours_ = false;
};

// Whether the installed app has the shareware data
bool have_data() { return data_files(installed()).size() >= 8; }

wolf3d::Status status() { return wolf3d::status(); }

// The loudest sample the audio engine mixes over `frames` frames, pulled as
// the host's audio device would
int hear(int frames) {
    int loudest = 0;
    int16_t block[2 * 400];
    for (int i = 0; i < frames; i++) {
        run_frames(1);
        viper_sim::audio_pull(block, 400);
        for (int16_t s : block) {
            loudest = std::max(loudest, std::abs(static_cast<int>(s)));
        }
    }
    return loudest;
}

// Whether that's sound: the boards without any are silent throughout
bool sounds(int loudest) {
    int16_t probe[2];
    return !viper_sim::audio_pull(probe, 1) || loudest > 500;
}

GamepadState pad(GamepadState::Button b) { return button(b); }

// Opens the app and waits for its first frame
void open_wolf() {
    REQUIRE(System::get().launch_app(kBundle));
    REQUIRE(run_until([] { return status().frames > 0 || !status().message.empty(); }, 30s));
    INFO(status().message);
    REQUIRE(status().running);
}

// Presses `b`, then gives the engine `frames` frames (fades take ~30)
void press_and_wait(GamepadState::Button b, int frames) {
    press(pad(b));
    run_frames(frames);
}

// From the title screens to a new game: A past the signon, rating and title
// screens to the menu, which opens on Read This! in the shareware; A shows
// it and B comes back; then New Game, the episode (the shareware has one)
// and the difficulty the menu offers ("Bring 'em on!")
void start_new_game() {
    using B = GamepadState::Button;
    REQUIRE(run_until(
        [] {
            if (!status().menu) {
                press_and_wait(B::A, 10);
            }
            return status().menu;
        },
        120s));
    run_frames(60);
    wolf_shot("2_menu");

    press_and_wait(B::A, 120);
    REQUIRE(status().menu);
    REQUIRE(status().running);
    wolf_shot("3_read_this");
    press_and_wait(B::B, 90);

    // Change View, (Save Game, inactive), Load Game, Control, Sound, New Game
    for (int i = 0; i < 5; i++) {
        press_and_wait(B::UP, 5);
    }
    press_and_wait(B::A, 60);
    press_and_wait(B::A, 60);
    wolf_shot("4_difficulty");
    press(pad(B::A));
    REQUIRE(run_until([] { return status().playing; }, 60s));
    CHECK_FALSE(status().menu);
}

} // namespace

TEST_CASE("Wolfenstein 3D without its data says what's missing and closes",
          "[boot][apps][wolf3d]") {
    boot_to_home();
    Installed app;
    // Move any data aside, for this test
    const fs::path aside = test_root() / "wolf3d_data_aside";
    fs::create_directories(aside);
    for (const auto &f : data_files(installed())) {
        fs::rename(f, aside / f.filename());
    }

    REQUIRE(System::get().launch_app(kBundle));
    run_frames(10);
    REQUIRE(running(kBundle));
    const auto s = status();
    CHECK_FALSE(s.running);
    CHECK(s.message.find("vswap.wl1") != std::string::npos);
    CHECK(s.message.find(installed().string()) != std::string::npos);
    wolf_shot("0_no_data");

    // Any button closes it
    press(pad(GamepadState::Button::A));
    run_frames(10);
    CHECK_FALSE(app_running());
    CHECK(at_home());

    for (const auto &f : data_files(aside)) {
        fs::rename(f, installed() / f.filename());
    }
}

TEST_CASE("Wolfenstein 3D plays the shareware episode", "[boot][apps][wolf3d][wolf3d-play]") {
    using B = GamepadState::Button;
    boot_to_home();
    Installed app;
    if (!have_data()) {
        SKIP("no shareware data: wolf3d/tools/fetch_shareware.sh, or VIPER_SIM_WOLF3D_DATA");
    }

    // The first launch sets up what stays (the app's data folder, the OS's
    // caches), so what's measured is the second to the third
    viper_sim::ResourceSnapshot before{};
    for (int round = 0; round < 3; round++) {
        if (round == 1) {
            before = viper_sim::snapshot_resources();
        }
        // No config: the menu opens on Read This!, as it does the first time
        REQUIRE(app_storage::erase_app_data(kBundle));
        open_wolf();
        run_frames(30);
        wolf_shot("1_signon");
        start_new_game();
        // The level's music (the OPL), pulled as the host's audio device would
        CHECK(sounds(hear(120)));
        wolf_shot("5_playing");

        // Walking forward changes the view; it keeps drawing at a frame a
        // loop()
        const auto frames = status().frames;
        const auto still = viper_sim::panel_snapshot();
        viper_sim::set_gamepad_state(pad(B::UP));
        run_frames(60);
        viper_sim::set_gamepad_state({});
        CHECK(status().frames > frames + 50);
        CHECK(viper_sim::panel_snapshot().pixels != still.pixels);
        // Fire, turn, open the first door
        press(pad(B::A));
        viper_sim::set_gamepad_state(pad(B::LEFT));
        run_frames(30);
        viper_sim::set_gamepad_state({});
        press(pad(B::B));
        run_frames(30);
        wolf_shot("6_moved");
        CHECK(status().playing);
        INFO("heap peak " << status().heap_peak / 1024 << " KB");
        CHECK(status().heap_peak > 0);

        // Boards without X and Y: B twice is the next weapon, and Up twice,
        // held, runs. A single B opens doors (above), after waiting for a
        // second that doesn't come, and changes no weapon
        const int weapon = status().weapon;
        press(pad(B::B));
        run_frames(30);
        CHECK(status().weapon == weapon);
        press(pad(B::B));
        press(pad(B::B));
        run_frames(30);
        CHECK(status().weapon != weapon);
        press(pad(B::Y)); // and Y, where there is one
        run_frames(30);
        CHECK(status().weapon == weapon);

        viper_sim::set_gamepad_state(pad(B::UP));
        run_frames(10);
        CHECK_FALSE(status().run); // once: walking
        press(pad(B::UP));
        viper_sim::set_gamepad_state(pad(B::UP));
        run_frames(10);
        CHECK(status().run); // again, held: running
        viper_sim::set_gamepad_state({});
        run_frames(10);
        CHECK_FALSE(status().run);               // let go: neither
        viper_sim::set_gamepad_state(pad(B::X)); // and X, where there is one
        run_frames(10);
        CHECK(status().run);
        viper_sim::set_gamepad_state({});
        run_frames(10);

        // Start is the menu, mid-game; holding Select closes the app from
        // there, wherever the engine is
        press(pad(B::START));
        REQUIRE(run_until([] { return !status().playing; }, 10s));
        viper_sim::set_gamepad_state(pad(B::SELECT));
        REQUIRE(run_until([] { return !app_running(); }, 10s));
        viper_sim::set_gamepad_state({});
        run_frames(10);
        CHECK(status().open == false);
    }
    // Nothing grows. (Something the install's rescan showed can go away
    // meanwhile, so fewer LVGL objects is fine.)
    const auto d = viper_sim::delta(before, viper_sim::snapshot_resources());
    CHECK(d.lv_objects <= 0);
    CHECK(d.lv_timers <= 0);
    CHECK(d.heap_bytes <= 1024);
    CHECK(d.psram_bytes + d.internal_bytes <= 0);
}

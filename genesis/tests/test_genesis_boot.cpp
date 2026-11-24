// The Genesis app's boot tests: the app running on the whole OS in the sim
// (viper_sim_app_boot_tests; helpers in the firmware repo's
// sim/tests/boot_test_util.hpp)

#include "boot_test_util.hpp"

#include "smoke_md.h"

// What the tests look at, from genesis/main/main.cpp. Mode: -1 not
// running, 0 error, 1 running, 2 paused.
extern "C" {
int viper_sim_genesis_mode(void);
const char *viper_sim_genesis_message(void);
int viper_sim_genesis_menu_selected(void);
uint32_t viper_sim_genesis_frames(void);
uint32_t viper_sim_genesis_presented(void);
bool viper_sim_genesis_rom_in_scratch(void);
}

namespace {
constexpr const char *kBundleId = "app.viper.genesis";
enum Mode { kNotRunning = -1, kError = 0, kRunning = 1, kPaused = 2 };
// The pause menu's rows
enum MenuRow { kResume, kSaveState, kLoadState, kCpu, kReset, kQuit };

// gwenesis's CRAM to RGB565, as the scan-out shows it
uint16_t rgb565(uint16_t cram) {
    return static_cast<uint16_t>(((cram & 0xE00) >> 7) | ((cram & 0x0E0) << 3) |
                                 ((cram & 0x00E) << 12));
}

long pixels_of(uint16_t color) {
    const auto panel = viper_sim::panel_snapshot();
    return std::count(panel.pixels.begin(), panel.pixels.end(), color);
}
} // namespace

// Its icon opens the ROM picker on smoke.md (make_test_roms.py): a white bar
// on a blue backdrop that turns red below an H-int's line, green while the
// Genesis A (the Y button) is held. Select opens the pause menu.
// VIPER_SIM_GENESIS_SHOTS=<dir> saves the game and the menu.
TEST_CASE("The Genesis icon plays the cartridge you pick",
          "[boot][apps][genesis][rom-picker][smoke]") {
    REQUIRE(boot_to_home().size() == 4);
    const uint32_t rate_before = System::get().audio_sample_rate();
    const char *shots = std::getenv("VIPER_SIM_GENESIS_SHOTS");
    auto shot = [&](const char *name) {
        if (shots) {
            write_ppm(viper_sim::panel_snapshot(), (fs::path(shots) / name).string().c_str());
        }
    };

    // An ELF emulator: its manifest's "extensions" make genesis:// open its
    // ROMs, as a built-in emulator's URL handler does
    REQUIRE(app::AppManager::get().getApps().count(kBundleId));
    CHECK(app::AppManager::get().extensionsForScheme("genesis") ==
          std::vector<std::string>{".md", ".gen", ".smd"});
    CHECK(app::AppManager::get().urlForFile("/x/Game.GEN") == "genesis:///x/Game.GEN");
    REQUIRE(System::get().launch_app(kBundleId));
    REQUIRE(current_app_name() == "RomPickerApp");
    CHECK(app::AppManager::get().getCurrentApp()->get_arg(0) == "genesis");
    run_frames(10);
    // Only its ROMs are listed, so smoke.md is selected
    press(button(GamepadState::Button::A));
    REQUIRE(run_until([] { return current_app_name() == kBundleId; }, 5s));
    const auto app = app::AppManager::get().getCurrentApp();
    CHECK(app->get_arg(1) == (viper::paths::fatfs() / "roms" / "smoke.md").string());
    run_frames(1);
    INFO("message: " << viper_sim_genesis_message());
    REQUIRE(viper_sim_genesis_mode() == kRunning);
    // The ROM is in the firmware's emulator ROM buffer
    CHECK(viper_sim_genesis_rom_in_scratch());

    // Its picture, stretched from the 320x224 indexed frame: blue above the
    // H-int's line, red below, and the bar
    run_frames(10);
    const long blue = pixels_of(rgb565(kSmokeMdBackdrop));
    const long red = pixels_of(rgb565(kSmokeMdBackdropLower));
    const long white = pixels_of(rgb565(kSmokeMdBarColor));
    INFO("blue " << blue << ", red " << red << ", white " << white);
    CHECK(blue > 20000);
    CHECK(red > 20000);
    CHECK(white > 1200); // 8 lines of 192 pixels, stretched
    shot("genesis_smoke.ppm");
    CHECK(viper_sim_genesis_presented() > 0);
    // The app resamples its sound in its audio tap; the output's rate stays
    CHECK(System::get().audio_sample_rate() == rate_before);

    // The frames keep coming, and the game sees the gamepad: Y is the
    // Genesis A, which turns the backdrop green
    const uint32_t frames = viper_sim_genesis_frames();
    run_frames(10);
    CHECK(viper_sim_genesis_frames() > frames);
    GamepadState held;
    held.y = 1;
    viper_sim::set_gamepad_state(held);
    run_frames(6);
    CHECK(pixels_of(rgb565(kSmokeMdBackdropPressed)) > 20000);
    viper_sim::set_gamepad_state({});
    run_frames(6);
    CHECK(pixels_of(rgb565(kSmokeMdBackdropPressed)) == 0);
    // B is the Genesis B, not back
    press(button(GamepadState::Button::B));
    run_frames(4);
    REQUIRE(app_running());
    CHECK(viper_sim_genesis_mode() == kRunning);

    // Select pauses: the game stops and the menu draws over it, dimmed
    press(button(GamepadState::Button::SELECT));
    REQUIRE(viper_sim_genesis_mode() == kPaused);
    const uint32_t paused_at = viper_sim_genesis_frames();
    run_frames(10);
    CHECK(viper_sim_genesis_frames() == paused_at);
    CHECK(pixels_of(rgb565(kSmokeMdBackdrop)) == 0);
    shot("genesis_menu.ppm");

    // Save a state, then load it again
    press(button(GamepadState::Button::DOWN));
    REQUIRE(viper_sim_genesis_menu_selected() == kSaveState);
    press(button(GamepadState::Button::A));
    REQUIRE(viper_sim_genesis_mode() == kRunning);
    const fs::path state = viper::paths::data() / kBundleId / "smoke.state";
    CHECK(fs::exists(state));
    CHECK(fs::file_size(state) > 128 * 1024); // work RAM and VRAM, at least
    run_frames(5);
    press(button(GamepadState::Button::SELECT));
    press(button(GamepadState::Button::DOWN));
    press(button(GamepadState::Button::DOWN));
    REQUIRE(viper_sim_genesis_menu_selected() == kLoadState);
    press(button(GamepadState::Button::A));
    REQUIRE(viper_sim_genesis_mode() == kRunning);
    REQUIRE(run_until([] { return pixels_of(rgb565(kSmokeMdBarColor)) > 1200; }, 2s));

    // Reset: the game starts over, and draws its picture again
    press(button(GamepadState::Button::SELECT));
    press(button(GamepadState::Button::UP));
    press(button(GamepadState::Button::UP));
    REQUIRE(viper_sim_genesis_menu_selected() == kReset);
    press(button(GamepadState::Button::A));
    REQUIRE(viper_sim_genesis_mode() == kRunning);
    REQUIRE(run_until([] { return pixels_of(rgb565(kSmokeMdBarColor)) > 1200; }, 2s));

    // Quit, from the bottom of the menu (up from Resume wraps to it)
    press(button(GamepadState::Button::SELECT));
    press(button(GamepadState::Button::UP));
    REQUIRE(viper_sim_genesis_menu_selected() == kQuit);
    press(button(GamepadState::Button::A));
    run_frames(10);
    CHECK_FALSE(app_running());
    CHECK(at_home());
    CHECK(viper_sim_genesis_mode() == kNotRunning);
    CHECK(System::get().audio_sample_rate() == rate_before);
    fs::remove(state);
}

TEST_CASE("The Genesis app says what's wrong with a ROM it can't play", "[boot][apps][genesis]") {
    REQUIRE(boot_to_home().size() == 4);
    // Too small to hold the vectors and the header
    const fs::path tiny = viper::paths::fatfs() / "roms" / "tiny.md";
    std::ofstream(tiny, std::ios::binary) << std::string(100, '\0');
    for (const auto &[url, says] :
         {std::pair<std::string, std::string>{"genesis://missing.md", "not found"},
          {"genesis://tiny.md", "Couldn't read"}}) {
        INFO(url);
        REQUIRE(System::get().launch_app(url));
        REQUIRE(current_app_name() == kBundleId);
        run_frames(5);
        CHECK(viper_sim_genesis_mode() == kError);
        CHECK(std::string(viper_sim_genesis_message()).find(says) != std::string::npos);
        CHECK(distinct_colors(viper_sim::panel_snapshot()) > 1);
        // B goes home
        press(button(GamepadState::Button::B));
        run_frames(10);
        CHECK(at_home());
    }
    fs::remove(tiny);
}

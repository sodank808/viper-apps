// Doom's boot tests: the app running on the whole OS in the Viper firmware
// repo's host simulator (viper_sim_app_boot_tests, with this repo at
// apps_public/; helpers in its sim/tests/boot_test_util.hpp)

#include "boot_test_util.hpp"

#include <fstream>
#include <iterator>
#include <thread>

// Doom, an ELF app. What its tests look at, from its
// doom_app.cpp. State: -1 not running, 0 loading, 1 running, 2 a message,
// 3 quitting.
extern "C" {
int viper_sim_doom_state(void);
const char *viper_sim_doom_message(void);
int viper_sim_doom_gametic(void);
int viper_sim_doom_menu_active(void);
int viper_sim_doom_player_angle(void);
int viper_sim_doom_in_level(void);
int viper_sim_doom_wiping(void);
}

// Doom needs a WAD, and the repo has none (the shareware doom1.wad is 4 MB),
// so this runs only when VIPER_SIM_DOOM_WAD names one. It opens Doom on it
// as Files would, waits for the engine's setup (on a thread of its own, in
// real time), and checks that the attract demo plays and draws, that Start
// opens Doom's menu and a new game takes the D-pad, then closes Doom and
// opens it twice more: the engine starts afresh each time, and nothing it
// allocates is left behind. VIPER_SIM_DOOM_SHOTS=<dir> saves the demo and
// the game.
TEST_CASE("Doom plays the WAD you open, and opens again", "[boot][apps][doom]") {
    const char *wad = std::getenv("VIPER_SIM_DOOM_WAD");
    if (!wad) {
        SKIP("VIPER_SIM_DOOM_WAD isn't set");
    }
    REQUIRE(fs::exists(wad));
    REQUIRE(boot_to_home().size() == 4);
    const char *shots = std::getenv("VIPER_SIM_DOOM_SHOTS");
    auto shot = [&](const std::string &name) {
        if (shots) {
            write_ppm(viper_sim::panel_snapshot(), (fs::path(shots) / name).string().c_str());
        }
    };
    CHECK(app::AppManager::get().extensionsForScheme("doom") == std::vector<std::string>{".wad"});

    auto play = [&](int round) {
        INFO("round " << round);
        REQUIRE(System::get().launch_app(app::AppManager::get().urlForFile(wad)));
        REQUIRE(run_until([] { return current_app_name() == "app.viper.doom"; }, 5s));
        // The setup runs on its own thread, in real time
        const auto give_up = std::chrono::steady_clock::now() + 60s;
        while (viper_sim_doom_state() == 0 && std::chrono::steady_clock::now() < give_up) {
            run_frames(1);
            std::this_thread::sleep_for(2ms);
        }
        INFO("message: " << viper_sim_doom_message());
        REQUIRE(viper_sim_doom_state() == 1);

        // The title and demos: 35 tics a second on the simulated clock, but
        // for the screen wipes between them, which stop the game as they
        // always have
        const int tic = viper_sim_doom_gametic();
        run_frames(250); // 4 s, 140 tics
        CHECK(viper_sim_doom_gametic() - tic > 70);
        REQUIRE(viper_sim_doom_state() == 1);
        {
            const auto panel = viper_sim::panel_snapshot();
            const long lit = std::count_if(panel.pixels.begin(), panel.pixels.end(),
                                           [](uint16_t px) { return px != 0; });
            CHECK(lit > long(panel.pixels.size()) / 2);
        }
        if (round == 0) {
            shot("doom_demo.ppm");
        }

        // Start opens the menu; A, A, A starts a new game (New Game, the
        // episode, the skill)
        CHECK_FALSE(viper_sim_doom_menu_active());
        press(button(GamepadState::Button::START));
        run_frames(4);
        REQUIRE(viper_sim_doom_menu_active());
        for (int i = 0; i < 3; i++) {
            press(button(GamepadState::Button::A));
            run_frames(6);
        }
        REQUIRE(run_until([] { return viper_sim_doom_in_level() == 1; }, 5s));
        CHECK_FALSE(viper_sim_doom_menu_active());
        // The level melts in, taking no input meanwhile
        REQUIRE(run_until([] { return viper_sim_doom_wiping() == 0; }, 5s));
        run_frames(10);
        // The D-pad's left strafes, without turning; L turns
        const int angle = viper_sim_doom_player_angle();
        GamepadState held;
        held.left = 1;
        viper_sim::set_gamepad_state(held);
        run_frames(30);
        viper_sim::set_gamepad_state({});
        run_frames(4);
        CHECK(viper_sim_doom_player_angle() == angle);
        held = {};
        held.l = 1;
        viper_sim::set_gamepad_state(held);
        run_frames(30);
        viper_sim::set_gamepad_state({});
        run_frames(4);
        CHECK(viper_sim_doom_player_angle() != angle);
        CHECK(viper_sim_doom_state() == 1);
        if (round == 0) {
            shot("doom_game.ppm");
        }

        app::AppManager::get().stopCurrentApp();
        run_frames(3);
        CHECK_FALSE(app_running());
        CHECK(viper_sim_doom_state() == -1);
        // Closing saved the settings, which the next launch reads
        const auto cfg = viper::paths::data() / "app.viper.doom" / "prboom.cfg";
        REQUIRE(fs::exists(cfg));
        std::ifstream in(cfg);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CHECK(text.find("autorun") != std::string::npos);
    };

    // The first round may set things up for good, as in the leak test
    play(0);
    const auto s0 = viper_sim::snapshot_resources();
    play(1);
    const auto s1 = viper_sim::snapshot_resources();
    play(2);
    const auto s2 = viper_sim::snapshot_resources();
    const auto growth = viper_sim::describe_growth(
        viper_sim::steady(viper_sim::delta(s0, s1), viper_sim::delta(s1, s2)));
    INFO("grew every launch: " << growth);
    CHECK(growth.empty());
}

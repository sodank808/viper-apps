// NetHack's boot tests: the app running on the whole OS in the sim
// (viper_sim_app_boot_tests; helpers in sim/tests/boot_test_util.hpp)
//
// The tests install the app themselves (Installed), as `make` would, with
// the data the sim's build packs (tests.cmake: VIPER_SIM_NETHACK_DATA).
// VIPER_SIM_NETHACK_SHOTS=<dir> saves their screens.

#include "boot_test_util.hpp"

#include <fstream>

#include "app_storage.hpp"
#include "elf_app.hpp"
#include "nethack_app.h"
#include "nethack_engine.hpp"
#include "paths.hpp"
#include "system_ui.hpp"

namespace {

using namespace std::chrono_literals;
using B = GamepadState::Button;

constexpr const char *kBundle = "app.viper.nethack";

fs::path installed() { return viper::paths::fatfs() / "apps" / kBundle; }
fs::path data_dir() { return viper::paths::data() / kBundle; }
fs::path app_folder() { return fs::path(__FILE__).parent_path().parent_path(); }

void shot(const char *name) {
    if (const char *dir = std::getenv("VIPER_SIM_NETHACK_SHOTS")) {
        write_ppm(viper_sim::panel_snapshot(),
                  (fs::path(dir) / (std::string(name) + ".ppm")).c_str());
    }
}

// The app installed on the sim's fatfs, as `make` installs it, for as long
// as this lives. The sim runs the app's code compiled in, so app.elf is a
// stand-in.
class Installed {
  public:
    Installed() {
        const fs::path dir = installed();
        fs::create_directories(dir);
        for (const char *f : {"info.json", "icon.png"}) {
            fs::copy_file(app_folder() / f, dir / f, fs::copy_options::overwrite_existing);
        }
        std::ofstream(dir / "app.elf") << "ELF";
        for (const char *f : {"sysconf", "nethackrc"}) {
            fs::copy_file(app_folder() / "assets" / f, dir / f,
                          fs::copy_options::overwrite_existing);
        }
        fs::copy_file(fs::path(VIPER_SIM_NETHACK_DATA) / "nhdat", dir / "nhdat",
                      fs::copy_options::overwrite_existing);
        elf_apps::rescan();
    }
    ~Installed() {
        fs::remove_all(installed());
        elf_apps::rescan();
    }
    Installed(const Installed &) = delete;
    Installed &operator=(const Installed &) = delete;
};

const nethack::Model &model() { return nethack::engine_model(); }
nethack::Ask asked() { return model().request.ask; }

std::string screen_text() {
    std::string s;
    for (const auto &m : model().messages) {
        s += m + "\n";
    }
    return s;
}

bool hero_on_map() {
    const auto &m = model();
    return m.cell(m.hero_x, m.hero_y).ch == '@';
}

// Opens the app and waits for NetHack's first question
void open_nethack() {
    REQUIRE(System::get().launch_app(kBundle));
    REQUIRE(run_until(
        [] { return asked() != nethack::Ask::None || !nethack::app_status().message.empty(); },
        60s));
    INFO(nethack::app_status().message);
    REQUIRE(nethack::app_status().message.empty());
}

// Answers NetHack's questions with their defaults (A), and dismisses its
// windows, until it waits for a command with the hero on the map
void play_until_command() {
    REQUIRE(run_until(
        [] {
            if (asked() == nethack::Ask::Key && hero_on_map()) {
                return true;
            }
            if (asked() != nethack::Ask::None && asked() != nethack::Ask::Key) {
                press(button(B::A));
            }
            run_frames(2);
            return false;
        },
        120s));
}

// Closes the app as a player would, holding Select
void close_nethack() {
    viper_sim::set_gamepad_state(button(B::SELECT));
    REQUIRE(run_until([] { return !app_running(); }, 30s));
    viper_sim::set_gamepad_state({});
    run_frames(10);
}

} // namespace

TEST_CASE("NetHack starts a game, saves it on closing and restores it", "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    fs::remove_all(data_dir());

    open_nethack();
    shot("0_first_question");
    play_until_command();
    shot("1_playing");
    // The status lines (name, stats; level, gold, HP, ...)
    REQUIRE(model().status.size() >= 2);
    CHECK(model().status[1].find("Dlvl:1") != std::string::npos);

    // Walking: the hero moves or bumps, and time passes when it moves
    const int x0 = model().hero_x, y0 = model().hero_y;
    for (B dir : {B::LEFT, B::RIGHT, B::UP, B::DOWN, B::LEFT, B::LEFT, B::RIGHT, B::RIGHT}) {
        press(button(dir));
        run_frames(8);
        while (asked() == nethack::Ask::More || asked() == nethack::Ask::YesNo) {
            press(button(B::B));
            run_frames(4);
        }
    }
    INFO(screen_text());
    CHECK(hero_on_map());
    (void)x0;
    (void)y0;

    // The command list: Y opens it, B closes it
    press(button(B::Y));
    run_frames(3);
    CHECK(nethack::app_status().commands_open);
    shot("2_commands");
    press(button(B::B));
    run_frames(3);
    CHECK_FALSE(nethack::app_status().commands_open);

    // Closing the app saves the game
    const int hero_x = model().hero_x, hero_y = model().hero_y;
    close_nethack();
    // (NetHack names it <uid><name> on Unix)
    std::error_code ec;
    CHECK(std::distance(fs::directory_iterator(data_dir() / "save", ec),
                        fs::directory_iterator()) == 1);

    // Opening it again restores it, where the hero was
    open_nethack();
    play_until_command();
    INFO(screen_text());
    CHECK(screen_text().find("Restoring save file") != std::string::npos);
    CHECK(model().hero_x == hero_x);
    CHECK(model().hero_y == hero_y);
    shot("3_restored");

    close_nethack();
    fs::remove_all(data_dir());
}

// Dismisses --More--, text windows and menus until NetHack asks for a command
void back_to_commands() {
    REQUIRE(run_until(
        [] {
            if (asked() == nethack::Ask::Key) {
                return true;
            }
            if (asked() != nethack::Ask::None) {
                press(button(B::B));
            }
            run_frames(2);
            return false;
        },
        30s));
}

TEST_CASE("NetHack's buttons: context action, menus, inventory, positions",
          "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    fs::remove_all(data_dir());
    open_nethack();
    play_until_command();

    // A game starts on the up stairs, which A would climb
    CHECK((model().here & NHV_HERE_UPSTAIRS) != 0);

    // Start: the game menu; B closes it
    press(button(B::START));
    run_frames(3);
    CHECK(nethack::app_status().game_menu_open);
    shot("6_game_menu");
    press(button(B::B));
    run_frames(3);
    CHECK_FALSE(nethack::app_status().game_menu_open);

    // Its Options and Help are NetHack's own menus
    for (int line : {1, 3}) {
        press(button(B::START));
        run_frames(3);
        for (int i = 0; i < line; i++) {
            press(button(B::DOWN));
            run_frames(2);
        }
        press(button(B::A));
        REQUIRE(run_until([] { return asked() == nethack::Ask::Menu; }, 10s));
        const nethack::Window *w = model().window(model().request.window);
        REQUIRE(w);
        CHECK(w->items.size() > 5);
        shot(line == 1 ? "9_options" : "10_help");
        back_to_commands();
    }

    // X: the inventory; choosing an item offers what to do with it
    press(button(B::X));
    REQUIRE(run_until([] { return asked() == nethack::Ask::Menu; }, 10s));
    const int inventory = model().request.window;
    press(button(B::A));
    REQUIRE(run_until(
        [inventory] {
            return asked() == nethack::Ask::Menu && model().request.window != inventory;
        },
        10s));
    {
        const nethack::Window *w = model().window(model().request.window);
        REQUIRE(w);
        std::string items;
        for (const auto &item : w->items) {
            items += item.text + "\n";
        }
        INFO(items);
        CHECK(items.find("Drop") != std::string::npos);
    }
    shot("7_item_actions");
    back_to_commands();

    // A position (far look, ;): a cursor the d-pad moves, A picks
    nethack::engine_answer_key(';');
    std::string asks;
    const bool found = run_until(
        [&asks] {
            if (model().getpos && asked() == nethack::Ask::Key) {
                return true;
            }
            if (asked() != nethack::Ask::None) {
                asks +=
                    std::to_string(static_cast<int>(asked())) + " " + model().request.query + "\n";
            }
            if (asked() == nethack::Ask::More) {
                press(button(B::A));
            }
            run_frames(2);
            return false;
        },
        10s);
    INFO(asks);
    REQUIRE(found);
    const int x0 = model().cursor_x;
    CHECK(x0 == model().hero_x);
    press(button(B::RIGHT));
    REQUIRE(run_until([x0] { return model().cursor_x != x0; }, 5s));
    CHECK(model().cursor_x == x0 + 1);
    shot("8_position");
    press(button(B::A));
    REQUIRE(run_until([] { return !model().getpos; }, 5s));
    back_to_commands();

    // B on a yes/no question that has no Escape answers no, not Escape,
    // which NetHack would call an error ("Program in disorder")
    nethack::engine_queue_keys("pray\n");
    nethack::engine_answer_key('#');
    REQUIRE(run_until([] { return asked() == nethack::Ask::YesNo; }, 10s));
    CHECK(model().request.query.find("pray") != std::string::npos);
    press(button(B::B));
    back_to_commands();
    CHECK(screen_text().find("disorder") == std::string::npos);

    close_nethack();
    fs::remove_all(data_dir());
}

TEST_CASE("NetHack types a line on the system keyboard", "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    fs::remove_all(data_dir());
    open_nethack();
    play_until_command();

    // Engrave in the dust with a finger: E, then - (fingers), then the text
    nethack::engine_answer_key('E');
    std::string asks;
    REQUIRE(run_until(
        [&asks] {
            if (asked() == nethack::Ask::Line) {
                return true;
            }
            if (asked() != nethack::Ask::None) {
                asks += model().request.query + "\n";
            }
            if (asked() == nethack::Ask::More || asked() == nethack::Ask::Text) {
                press(button(B::A));
            } else if (asked() == nethack::Ask::YesNo) {
                // "What do you want to write with?" Fingers; no to the rest
                nethack::engine_answer_yn(
                    model().request.query.find("with") != std::string::npos ? '-' : 'n');
            } else if (asked() == nethack::Ask::Key) {
                nethack::engine_answer_key('-');
            }
            run_frames(2);
            return false;
        },
        30s));
    INFO(asks);
    INFO(model().request.query);
    CHECK(model().request.query.find("write") != std::string::npos);
    REQUIRE(run_until([] { return System::get().ui().text_entry() != nullptr; }, 5s));
    shot("4_typing");

    // A types the key under the cursor (q, where it starts), Start is done
    press(button(B::A));
    run_frames(3);
    press(button(B::A));
    run_frames(3);
    press(button(B::START));
    REQUIRE(run_until([] { return asked() == nethack::Ask::Key || asked() == nethack::Ask::More; },
                      10s));
    CHECK(System::get().ui().text_entry() == nullptr);
    while (asked() == nethack::Ask::More) {
        press(button(B::A));
        run_frames(4);
    }

    // Reading what's here: the engraving says what was typed
    REQUIRE(asked() == nethack::Ask::Key);
    nethack::engine_answer_key(':');
    REQUIRE(run_until([] { return asked() != nethack::Ask::None; }, 10s));
    run_frames(2);
    INFO(screen_text());
    CHECK(screen_text().find("\"qq\"") != std::string::npos);
    shot("5_engraved");

    while (asked() == nethack::Ask::More || asked() == nethack::Ask::Text) {
        press(button(B::A));
        run_frames(4);
    }
    close_nethack();
    fs::remove_all(data_dir());
}

TEST_CASE("NetHack frees everything when it closes", "[boot][apps][nethack][leaks]") {
    boot_to_home();
    Installed app;
    // The first game sets up what stays (in the sim, the copy of NetHack's
    // variables that each game starts from), so what's measured is the
    // second game to the third
    viper_sim::ResourceSnapshot before{};
    for (int round = 0; round < 3; round++) {
        if (round == 1) {
            before = viper_sim::snapshot_resources();
        }
        fs::remove_all(data_dir());
        open_nethack();
        play_until_command();
        press(button(B::A)); // a turn: search, or what's obvious here
        run_frames(10);
        close_nethack();
    }
    fs::remove_all(data_dir());
    const auto d = viper_sim::delta(before, viper_sim::snapshot_resources());
    CHECK(d.lv_objects <= 0);
    CHECK(d.lv_timers <= 0);
    CHECK(d.heap_bytes <= 1024);
    CHECK(d.psram_bytes + d.internal_bytes <= 0);
}

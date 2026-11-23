// NetHack's boot tests: the app running on the whole OS in the sim
// (viper_sim_app_boot_tests; helpers in sim/tests/boot_test_util.hpp)
//
// The tests install the app themselves (Installed), as `make` would, with
// the data the sim's build packs (tests.cmake: VIPER_SIM_NETHACK_DATA).
// VIPER_SIM_NETHACK_SHOTS=<dir> saves their screens.

#include "boot_test_util.hpp"

#include <cstdint>
#include <fstream>
#include <set>
#include <vector>

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
        for (const char *f : {"nhdat", "tiles.bin"}) {
            fs::copy_file(fs::path(VIPER_SIM_NETHACK_DATA) / f, dir / f,
                          fs::copy_options::overwrite_existing);
        }
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

// For as long as it lives, games start from `seed`: the same game each run
struct Seeded {
    explicit Seeded(unsigned long seed) { nethack::engine_set_seed(seed); }
    ~Seeded() { nethack::engine_set_seed(0); }
    Seeded(const Seeded &) = delete;
    Seeded &operator=(const Seeded &) = delete;
};

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
        // (A random game: whatever the walk brought up)
        back_to_commands();
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

TEST_CASE("NetHack's buttons: context action, menus, inventory, positions",
          "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    // The same game each run: what it checks depends on the hero (its first
    // item must be one it can drop)
    const Seeded seeded(1);
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
    for (int line : {2, 4}) {
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
        shot(line == 2 ? "9_options" : "10_help");
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

namespace {

// A short press of Select (a long one closes the app)
void tap_select() {
    viper_sim::set_gamepad_state(button(B::SELECT));
    run_frames(3);
    viper_sim::set_gamepad_state({});
    run_frames(3);
}

} // namespace

TEST_CASE("NetHack's views: Select cycles terminal, tiles, small tiles and first person",
          "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    fs::remove_all(data_dir());
    open_nethack();
    play_until_command();
    // Each view draws the map its own way; the game doesn't notice
    const auto turn = model().status;
    std::vector<std::vector<uint16_t>> screens;
    const char *const names[] = {"11_terminal", "12_tiles", "13_small_tiles", "14_first_person"};
    for (int view = 0; view < 4; view++) {
        CHECK(nethack::app_status().map_view == view);
        run_frames(2);
        shot(names[view]);
        screens.push_back(viper_sim::panel_snapshot().pixels);
        tap_select();
    }
    CHECK(nethack::app_status().map_view == 0); // round to the terminal
    // The game menu's first line is the next view too
    press(button(B::START));
    run_frames(3);
    press(button(B::A));
    run_frames(3);
    CHECK(nethack::app_status().map_view == 1);
    CHECK_FALSE(nethack::app_status().game_menu_open);
    tap_select();
    tap_select();
    tap_select();
    REQUIRE(nethack::app_status().map_view == 0);
    CHECK(screens[0] != screens[1]);
    CHECK(screens[1] != screens[2]);
    CHECK(screens[2] != screens[3]);
    CHECK(model().status == turn);
    CHECK(app_running());

    // The view is kept: the app opens in the one chosen last
    tap_select();
    REQUIRE(nethack::app_status().map_view == 1);
    close_nethack();
    open_nethack();
    play_until_command();
    CHECK(nethack::app_status().map_view == 1);
    tap_select();
    tap_select();
    tap_select(); // back to the terminal, for the tests after this one
    CHECK(nethack::app_status().map_view == 0);
    close_nethack();
    fs::remove_all(data_dir());
}

namespace {

// The game's state as the model has it: the map, where the hero is, the
// status lines and the messages (FNV-1a)
uint64_t state_hash() {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) {
        for (int i = 0; i < 8; i++) {
            h = (h ^ ((v >> (i * 8)) & 0xFF)) * 1099511628211ull;
        }
    };
    const auto &m = model();
    for (const auto &c : m.map) {
        mix(static_cast<uint64_t>(c.glyph) ^ (static_cast<uint64_t>(c.flags) << 32));
    }
    mix(static_cast<uint64_t>(m.hero_x) | (static_cast<uint64_t>(m.hero_y) << 32));
    for (const auto &line : m.status) {
        for (char ch : line) {
            mix(static_cast<unsigned char>(ch));
        }
    }
    for (const auto &line : m.messages) {
        for (char ch : line) {
            mix(static_cast<unsigned char>(ch));
        }
    }
    return h;
}

// A new game from `seed`, played by the same keys each time; the state
// after each turn. With switch_views, Select is tapped after every turn.
std::vector<uint64_t> scripted_game(unsigned long seed, bool switch_views) {
    fs::remove_all(data_dir());
    nethack::engine_set_seed(seed);
    open_nethack();
    play_until_command();
    std::vector<uint64_t> states{state_hash()};
    // NetHack's own keys: the d-pad means something else in the first-person
    // view (as the hero faces), and this is about drawing, not the buttons
    const char moves[] = "hhkljjlllkhjhklj";
    for (int round = 0; round < 2; round++) {
        for (const char *key = moves; *key; key++) {
            nethack::engine_answer_key(*key);
            run_frames(8);
            // Whatever it asks, the same answer each time
            REQUIRE(run_until(
                [] {
                    if (asked() == nethack::Ask::Key) {
                        return true;
                    }
                    if (asked() == nethack::Ask::More || asked() == nethack::Ask::Text) {
                        press(button(B::A));
                    } else if (asked() != nethack::Ask::None) {
                        press(button(B::B));
                    }
                    run_frames(2);
                    return false;
                },
                30s));
            if (switch_views) {
                tap_select();
            }
            states.push_back(state_hash());
        }
    }
    close_nethack();
    // Back to the terminal view, as the tests found it
    while (switch_views && nethack::app_status().map_view != 0) {
        open_nethack();
        tap_select();
        close_nethack();
    }
    nethack::engine_set_seed(0);
    fs::remove_all(data_dir());
    return states;
}

} // namespace

TEST_CASE("NetHack's views only draw: switching every turn plays the same game",
          "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    const auto plain = scripted_game(4242, false);
    const auto switching = scripted_game(4242, true);
    REQUIRE(plain.size() == switching.size());
    // The game moved on: most turns changed something
    CHECK(std::set<uint64_t>(plain.begin(), plain.end()).size() > plain.size() / 2);
    for (size_t i = 0; i < plain.size(); i++) {
        INFO("turn " << i);
        CHECK(plain[i] == switching[i]);
    }
    // And the seed is what made them the same: another plays another game
    CHECK(scripted_game(777, false).back() != plain.back());
}

TEST_CASE("NetHack's first-person view: turning, stepping the way the hero faces",
          "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    fs::remove_all(data_dir());
    const Seeded seeded(1);
    open_nethack();
    play_until_command();
    for (int i = 0; i < 3; i++) {
        tap_select();
    }
    REQUIRE(nethack::app_status().map_view == 3);
    run_frames(10);
    shot("15_first_person");

    // Left and Right turn the view: a quarter turn each, and no game turn
    const auto status = model().status;
    const int facing = nethack::app_status().facing;
    press(button(B::RIGHT));
    run_frames(12);
    CHECK(nethack::app_status().facing == (facing + 1) % 4);
    press(button(B::LEFT));
    run_frames(12);
    CHECK(nethack::app_status().facing == facing);
    CHECK(model().status == status);
    CHECK(asked() == nethack::Ask::Key);

    // Turn to face open floor with nothing on it, and step: the hero moves
    // that way
    static constexpr int kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
    const auto open_ahead = [] {
        const int f = nethack::app_status().facing;
        const auto &c = model().cell(model().hero_x + kDx[f], model().hero_y + kDy[f]);
        return c.terrain == NHV_TERRAIN_FLOOR && c.tile == c.terrain_tile;
    };
    for (int turns = 0; turns < 4 && !open_ahead(); turns++) {
        press(button(B::RIGHT));
        run_frames(12);
    }
    REQUIRE(open_ahead());
    const int f = nethack::app_status().facing;
    const int x = model().hero_x, y = model().hero_y;
    press(button(B::UP));
    run_frames(12);
    back_to_commands();
    CHECK(model().hero_x == x + kDx[f]);
    CHECK(model().hero_y == y + kDy[f]);
    run_frames(12);
    shot("16_first_person_step");

    // Back where the tests after this one expect the view
    tap_select();
    REQUIRE(nethack::app_status().map_view == 0);
    close_nethack();
    fs::remove_all(data_dir());
}

TEST_CASE("NetHack types a line on the system keyboard", "[boot][apps][nethack]") {
    boot_to_home();
    Installed app;
    // The same game each run (a random one may not let the hero write here)
    const Seeded seeded(1);
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

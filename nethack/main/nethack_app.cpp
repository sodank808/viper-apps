// NetHack as a Viper ELF app: the game (NetHack 5.0, nethack/) runs on the
// engine's thread (nethack_engine.cpp); this is the app around it. loop()
// gives the engine the turn, and when the engine waits for the player, reads
// the buttons into an answer and draws the game (nethack_view.cpp).
//
// The buttons, playing: the d-pad moves (two at once, diagonally), A does
// what's obvious where the hero is (stairs, picking up, opening a door; else
// searching), B is Escape, X the inventory (choose an item for what to do with
// it), Y the command list, L and R go up and down stairs, Start the game menu
// (options, help, extended commands, save). While a position is chosen
// (travel, far look), the d-pad moves a cursor and A picks.

#include <sys/stat.h>

#include <algorithm>
#include <string>
#include <vector>

#include "gfx/surface.hpp"
#include "nethack_app.h"
#include "nethack_engine.hpp"
#include "nethack_view.hpp"
#include "viper_app.h"

namespace {

using namespace nethack;

// How long loop() waits for the engine to answer before it returns and waits
// again in the next frame (a level being made). The simulator waits longer,
// so the tests' frames don't run ahead of a slow sanitized engine.
#ifdef VIPER_APP_STATIC_NAME
constexpr long kFrameWaitMs = 5000;
#else
constexpr long kFrameWaitMs = 100;
#endif

// Two directions pressed this close together are a diagonal
constexpr uint32_t kDiagonalMs = 70;
// A direction held this long repeats, this often
constexpr uint32_t kRepeatDelayMs = 350;
constexpr uint32_t kRepeatMs = 110;

// The hero: its save is found by this name
constexpr const char *kHeroName = "Viper";
// The longest line typed on the keyboard (NetHack's own buffer is BUFSZ, 256)
constexpr uint32_t kLineMax = 120;

constexpr uint32_t kDirections =
    VIPER_BUTTON_UP | VIPER_BUTTON_DOWN | VIPER_BUTTON_LEFT | VIPER_BUTTON_RIGHT;

// The command list (Y): what each line sends
struct Command {
    const char *label;
    const char *keys;
};
constexpr Command kCommands[] = {
    {"Pick up", ","},
    {"Look here", ":"},
    {"Search", "s"},
    {"Rest 20 turns", "20s"},
    {"Go down stairs", ">"},
    {"Go up stairs", "<"},
    {"Open a door", "o"},
    {"Close a door", "c"},
    {"Kick", "\x04"},
    {"Eat", "e"},
    {"Quaff a potion", "q"},
    {"Read", "r"},
    {"Wield a weapon", "w"},
    {"Wear armor", "W"},
    {"Take off armor", "T"},
    {"Put on an accessory", "P"},
    {"Remove an accessory", "R"},
    {"Apply a tool", "a"},
    {"Throw", "t"},
    {"Fire (quiver)", "f"},
    {"Zap a wand", "z"},
    {"Cast a spell", "Z"},
    {"Drop", "d"},
    {"Drop several", "D"},
    {"Pray", "#pray\n"},
    {"Far look", ";"},
    {"Travel", "_"},
    {"Engrave", "E"},
    {"What is here (all)", "\x0f"}, // ^O: overview
    {"Discoveries", "\\"},
    {"Help", "?"},
    {"Save and quit", "S"},
};
constexpr int kCommandCount = sizeof(kCommands) / sizeof(kCommands[0]);
// The commands used last come first in the list, this many
constexpr size_t kRecentCommands = 4;

// The game menu (Start)
constexpr Command kGameMenu[] = {
    {"Extended command (#)", "#"}, {"Options", "O"},
    {"Autopickup on/off", "@"},    {"Help", "?"},
    {"Discoveries", "\\"},         {"Overview of the dungeon", "\x0f"},
    {"Save and quit", "S"},
};
constexpr int kGameMenuCount = sizeof(kGameMenu) / sizeof(kGameMenu[0]);

// NetHack's direction keys in its own order (xdir[]/ydir[]: west, northwest,
// north, ...), as nh_port.h's doors bits are
constexpr char kDirKeys[] = "hykulnjb";

enum class List {
    None,
    Commands, // Y
    Game,     // Start
};

enum class State {
    Running,
    Message, // showing message (missing data, an error), waiting for a button
};

struct App {
    State state = State::Running;
    std::string message;
    bool dirty = true;
    uint32_t drawn_changes = ~0u;
    uint32_t request_id = ~0u;
    ViewState view;
    uint32_t frames = 0;

    // The command list or game menu, open over the map
    List list = List::None;
    int command_cursor = 0;
    int command_top = 0;
    std::vector<int> recent; // kCommands used last, newest first

    // The d-pad: a direction pressed waits kDiagonalMs for a second one
    uint32_t dir_pressed = 0; // when the first went down (0: none waiting)
    uint32_t dir_seen = 0;    // the directions down while waiting
    uint32_t dir_held_since = 0;
    uint32_t dir_repeat_at = 0;
    char dir_key = 0; // the key the d-pad sent last, while it's held

    // The system keyboard, open for a line of text (Ask::Line)
    bool typing = false;
};

App *a = nullptr;

bool file_exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void show_message(const std::string &text) {
    a->message = text;
    a->state = State::Message;
    a->dirty = true;
}

// NetHack's movement keys for the directions held
char direction_key(uint32_t buttons) {
    const bool up = buttons & VIPER_BUTTON_UP, down = buttons & VIPER_BUTTON_DOWN;
    const bool left = buttons & VIPER_BUTTON_LEFT, right = buttons & VIPER_BUTTON_RIGHT;
    if (up && left)
        return 'y';
    if (up && right)
        return 'u';
    if (down && left)
        return 'b';
    if (down && right)
        return 'n';
    if (up)
        return 'k';
    if (down)
        return 'j';
    if (left)
        return 'h';
    if (right)
        return 'l';
    return 0;
}

// The d-pad as NetHack's direction keys: 0, or a key to send now. A press
// waits kDiagonalMs for a second direction; one held repeats.
char read_direction(const viper_input_t *now, const viper_input_t *previous, bool repeat) {
    const uint32_t held = now->buttons & kDirections;
    const uint32_t pressed = held & ~previous->buttons;
    if (!held) {
        // A tap let go before the wait for a second direction ended: what
        // was pressed is the direction
        const char tapped = a->dir_pressed ? direction_key(a->dir_seen) : 0;
        a->dir_pressed = 0;
        a->dir_seen = 0;
        a->dir_key = 0;
        return tapped;
    }
    if (pressed && !a->dir_pressed && !a->dir_key) {
        a->dir_pressed = now->now_ms ? now->now_ms : 1;
    }
    if (a->dir_pressed) {
        a->dir_seen |= held;
        const bool two = (held & (held - 1)) != 0; // two directions down
        if (two || now->now_ms - a->dir_pressed >= kDiagonalMs) {
            a->dir_pressed = 0;
            a->dir_seen = 0;
            a->dir_key = direction_key(held);
            a->dir_held_since = now->now_ms;
            a->dir_repeat_at = now->now_ms + kRepeatDelayMs;
            return a->dir_key;
        }
        return 0;
    }
    if (repeat && a->dir_key && now->now_ms >= a->dir_repeat_at) {
        a->dir_repeat_at = now->now_ms + kRepeatMs;
        return direction_key(held);
    }
    return 0;
}

bool pressed(const viper_input_t *now, const viper_input_t *previous, uint32_t button) {
    return viper_button_pressed(now, previous, button);
}

// A new request: the view starts over
void begin_request(const Model &m) {
    a->view = ViewState{};
    a->list = List::None;
    if (m.request.ask == Ask::Menu) {
        if (const Window *w = m.window(m.request.window)) {
            const auto &items = w->items;
            a->view.marks.resize(items.size());
            for (size_t i = 0; i < items.size(); i++) {
                a->view.marks[i] = items[i].selected;
            }
            a->view.cursor = 0;
            while (a->view.cursor < static_cast<int>(items.size()) &&
                   !items[a->view.cursor].index) {
                a->view.cursor++;
            }
            if (a->view.cursor >= static_cast<int>(items.size())) {
                a->view.cursor = 0;
            }
        }
    }
    if (m.request.ask == Ask::YesNo) {
        const std::string &c = m.request.choices;
        const size_t d = c.find(m.request.def);
        a->view.choice = d == std::string::npos || m.request.def == 0 ? 0 : static_cast<int>(d);
    }
    a->dirty = true;
}

// Moves a cursor over `count` lines, `rows` a page; keeps it in view
void move_cursor(int &cursor, int &top, int count, int rows, int delta) {
    if (count <= 0) {
        return;
    }
    cursor = std::clamp(cursor + delta, 0, count - 1);
    if (cursor < top) {
        top = cursor;
    }
    if (cursor >= top + rows) {
        top = cursor - rows + 1;
    }
}

int screen_w() { return static_cast<int>(viper_display_width()); }
int screen_h() { return static_cast<int>(viper_display_height()); }

// ---- answering what the engine waits for ----

// What A does where the hero is: its label in the hints, and its keys
struct Action {
    const char *label;
    std::string keys;
};

Action context_action(const Model &m) {
    if (m.here & NHV_HERE_DOWNSTAIRS)
        return {"down", ">"};
    if (m.here & NHV_HERE_UPSTAIRS)
        return {"up", "<"};
    if (m.here & NHV_HERE_OBJECTS)
        return {"pick up", ","};
    if (m.doors) {
        // One door: open it; several: NetHack asks which way
        const bool one = (m.doors & (m.doors - 1)) == 0;
        std::string keys = "o";
        for (int i = 0; one && i < 8; i++) {
            if (m.doors & (1u << i)) {
                keys += kDirKeys[i];
            }
        }
        return {"open", keys};
    }
    if (m.here & NHV_HERE_FOUNTAIN)
        return {"drink", "q"};
    if (m.here & NHV_HERE_THRONE)
        return {"sit", "#sit\n"};
    if (m.here & NHV_HERE_ALTAR)
        return {"look", ":"};
    return {"search", "s"};
}

// Sends keys: the first answers the engine, the rest are queued for it
void send_keys(const std::string &keys) {
    if (keys.empty()) {
        return;
    }
    engine_queue_keys(keys.substr(1));
    engine_answer_key(static_cast<unsigned char>(keys[0]));
}

// The command list's order: the commands used last, then the rest
std::vector<int> command_order() {
    std::vector<int> order = a->recent;
    for (int i = 0; i < kCommandCount; i++) {
        if (std::find(order.begin(), order.end(), i) == order.end()) {
            order.push_back(i);
        }
    }
    return order;
}

int list_count() { return a->list == List::Game ? kGameMenuCount : kCommandCount; }

void open_list(List list) {
    a->list = list;
    a->command_cursor = 0;
    a->command_top = 0;
    a->dirty = true;
}

void input_list(const viper_input_t *now, const viper_input_t *previous) {
    const int count = list_count();
    const int rows = page_lines(screen_h()) - 1;
    int delta = 0;
    if (pressed(now, previous, VIPER_BUTTON_UP))
        delta = -1;
    if (pressed(now, previous, VIPER_BUTTON_DOWN))
        delta = 1;
    if (pressed(now, previous, VIPER_BUTTON_L) || pressed(now, previous, VIPER_BUTTON_LEFT))
        delta = -rows;
    if (pressed(now, previous, VIPER_BUTTON_R) || pressed(now, previous, VIPER_BUTTON_RIGHT))
        delta = rows;
    if (delta) {
        move_cursor(a->command_cursor, a->command_top, count, rows, delta);
        a->dirty = true;
    }
    if (pressed(now, previous, VIPER_BUTTON_B)) {
        a->list = List::None;
        a->dirty = true;
        return;
    }
    if (!pressed(now, previous, VIPER_BUTTON_A)) {
        return;
    }
    const List list = a->list;
    a->list = List::None;
    a->dirty = true;
    if (list == List::Game) {
        send_keys(kGameMenu[a->command_cursor].keys);
        return;
    }
    const int command = command_order()[a->command_cursor];
    auto &recent = a->recent;
    recent.erase(std::remove(recent.begin(), recent.end(), command), recent.end());
    recent.insert(recent.begin(), command);
    if (recent.size() > kRecentCommands) {
        recent.resize(kRecentCommands);
    }
    send_keys(kCommands[command].keys);
}

// Choosing a position (getpos): the d-pad moves the cursor, A picks, X and Y
// jump to the next monster and object, L and R to stairs
void input_position(const viper_input_t *now, const viper_input_t *previous) {
    if (char key = read_direction(now, previous, true)) {
        engine_answer_key(key);
        return;
    }
    struct Map {
        uint32_t button;
        int key;
    };
    static const Map kKeys[] = {
        {VIPER_BUTTON_A, '.'},     {VIPER_BUTTON_B, kEsc}, {VIPER_BUTTON_X, 'm'},
        {VIPER_BUTTON_Y, 'o'},     {VIPER_BUTTON_L, '<'},  {VIPER_BUTTON_R, '>'},
        {VIPER_BUTTON_START, '@'},
    };
    for (const Map &k : kKeys) {
        if (pressed(now, previous, k.button)) {
            engine_answer_key(k.key);
            return;
        }
    }
}

void input_key(const viper_input_t *now, const viper_input_t *previous) {
    const Model &m = engine_model();
    if (a->list != List::None) {
        input_list(now, previous);
        return;
    }
    if (m.getpos) {
        input_position(now, previous);
        return;
    }
    if (char key = read_direction(now, previous, true)) {
        engine_answer_key(key);
        return;
    }
    struct Map {
        uint32_t button;
        int key;
    };
    static const Map kKeys[] = {
        {VIPER_BUTTON_B, kEsc},
        {VIPER_BUTTON_X, 'i'},
        {VIPER_BUTTON_L, '<'},
        {VIPER_BUTTON_R, '>'},
    };
    for (const Map &k : kKeys) {
        if (pressed(now, previous, k.button)) {
            engine_answer_key(k.key);
            return;
        }
    }
    if (pressed(now, previous, VIPER_BUTTON_A)) {
        send_keys(context_action(m).keys);
    } else if (pressed(now, previous, VIPER_BUTTON_Y)) {
        open_list(List::Commands);
    } else if (pressed(now, previous, VIPER_BUTTON_START)) {
        open_list(List::Game);
    }
}

void input_yes_no(const viper_input_t *now, const viper_input_t *previous) {
    const Request &r = engine_model().request;
    const std::string choices = r.choices.substr(0, r.choices.find(kEsc));
    if (pressed(now, previous, VIPER_BUTTON_B)) {
        // Escape where the question takes it; otherwise q or n, as NetHack's
        // tty does (an answer outside the choices is an error, "Program in
        // disorder")
        char key = kEsc;
        if (!choices.empty() && r.choices.find(kEsc) == std::string::npos) {
            if (choices.find('q') != std::string::npos) {
                key = 'q';
            } else if (choices.find('n') != std::string::npos) {
                key = 'n';
            } else if (r.def) {
                key = r.def;
            }
        }
        engine_answer_yn(key);
        return;
    }
    if (choices.empty()) {
        // A free key: a direction ("In what direction?"), or an object's
        // letter ("What do you want to eat? [fg or ?*]"), which A lists
        if (char key = read_direction(now, previous, false)) {
            engine_answer_yn(key);
        } else if (pressed(now, previous, VIPER_BUTTON_A)) {
            const bool direction = r.query.find("direction") != std::string::npos;
            engine_answer_yn(direction ? '.' : '?');
        } else if (pressed(now, previous, VIPER_BUTTON_X)) {
            engine_answer_yn('*');
        }
        return;
    }
    const int count = static_cast<int>(choices.size());
    if (pressed(now, previous, VIPER_BUTTON_LEFT)) {
        a->view.choice = (a->view.choice + count - 1) % count;
        a->dirty = true;
    }
    if (pressed(now, previous, VIPER_BUTTON_RIGHT)) {
        a->view.choice = (a->view.choice + 1) % count;
        a->dirty = true;
    }
    if (pressed(now, previous, VIPER_BUTTON_A)) {
        engine_answer_yn(choices[std::clamp(a->view.choice, 0, count - 1)]);
    }
}

void input_menu(const viper_input_t *now, const viper_input_t *previous) {
    const Model &m = engine_model();
    const Window *w = m.window(m.request.window);
    if (!w) {
        engine_answer_menu({}, true);
        return;
    }
    const auto &items = w->items;
    const int count = static_cast<int>(items.size());
    const int rows = page_lines(screen_h()) - (w->prompt.empty() ? 0 : 1);
    int delta = 0;
    if (pressed(now, previous, VIPER_BUTTON_UP))
        delta = -1;
    if (pressed(now, previous, VIPER_BUTTON_DOWN))
        delta = 1;
    if (pressed(now, previous, VIPER_BUTTON_L) || pressed(now, previous, VIPER_BUTTON_LEFT)) {
        delta = -rows;
    }
    if (pressed(now, previous, VIPER_BUTTON_R) || pressed(now, previous, VIPER_BUTTON_RIGHT)) {
        delta = rows;
    }
    if (delta) {
        move_cursor(a->view.cursor, a->view.top, count, rows, delta);
        a->dirty = true;
    }
    if (pressed(now, previous, VIPER_BUTTON_B)) {
        engine_answer_menu({}, true);
        return;
    }
    const bool any = m.request.pick == NHV_PICK_ANY;
    if (pressed(now, previous, VIPER_BUTTON_A) && a->view.cursor < count) {
        const MenuItem &item = items[a->view.cursor];
        if (!item.index) {
            return;
        }
        if (!any) {
            engine_answer_menu({item.index}, false);
            return;
        }
        a->view.marks[a->view.cursor] = !a->view.marks[a->view.cursor];
        a->dirty = true;
    }
    if (any &&
        (pressed(now, previous, VIPER_BUTTON_START) || pressed(now, previous, VIPER_BUTTON_X))) {
        std::vector<int> chosen;
        for (int i = 0; i < count; i++) {
            if (a->view.marks[i] && items[i].index) {
                chosen.push_back(items[i].index);
            }
        }
        engine_answer_menu(chosen, false);
    }
}

void input_text(const viper_input_t *now, const viper_input_t *previous) {
    const Model &m = engine_model();
    const Window *w = m.window(m.request.window);
    const int count = w ? static_cast<int>(text_lines(*w, screen_w()).size()) : 0;
    const int rows = page_lines(screen_h());
    int top = a->view.top;
    if (pressed(now, previous, VIPER_BUTTON_UP))
        top -= 1;
    if (pressed(now, previous, VIPER_BUTTON_DOWN))
        top += 1;
    if (pressed(now, previous, VIPER_BUTTON_L) || pressed(now, previous, VIPER_BUTTON_LEFT)) {
        top -= rows;
    }
    if (pressed(now, previous, VIPER_BUTTON_R) || pressed(now, previous, VIPER_BUTTON_RIGHT)) {
        top += rows;
    }
    top = std::clamp(top, 0, std::max(count - rows, 0));
    if (top != a->view.top) {
        a->view.top = top;
        a->dirty = true;
    }
    if (pressed(now, previous, VIPER_BUTTON_A) || pressed(now, previous, VIPER_BUTTON_B)) {
        engine_answer_continue();
    }
}

// A line of text, on the system keyboard: opened when the engine asks, and
// read back once it closes. While it's open the OS has the buttons and the
// screen.
void input_line(const Model &m) {
    if (!a->typing) {
        viper_text_entry_t entry{};
        entry.size = sizeof(entry);
        entry.title = m.request.query.c_str();
        entry.text = "";
        entry.max_length = kLineMax;
        if (!viper_text_entry_open(&entry)) {
            engine_answer_line(std::string(1, kEsc));
            return;
        }
        a->typing = true;
        return;
    }
    char text[kLineMax + 1];
    const int state = viper_text_entry_poll(text, sizeof(text));
    if (state == VIPER_TEXT_ENTRY_DONE || state == VIPER_TEXT_ENTRY_CANCELLED ||
        state == VIPER_TEXT_ENTRY_NONE) {
        a->typing = false;
        a->dirty = true;
        engine_answer_line(state == VIPER_TEXT_ENTRY_DONE ? std::string(text)
                                                          : std::string(1, kEsc));
    }
}

// Reads the buttons into an answer, if the engine waits for one. True when
// the engine has the turn again.
bool answer(const viper_input_t *now, const viper_input_t *previous) {
    const Model &m = engine_model();
    if (m.request_id != a->request_id) {
        a->request_id = m.request_id;
        begin_request(m);
    }
    switch (m.request.ask) {
    case Ask::Key:
        input_key(now, previous);
        break;
    case Ask::YesNo:
        input_yes_no(now, previous);
        break;
    case Ask::Menu:
        input_menu(now, previous);
        break;
    case Ask::Text:
        input_text(now, previous);
        break;
    case Ask::More:
        if (pressed(now, previous, VIPER_BUTTON_A) || pressed(now, previous, VIPER_BUTTON_B)) {
            engine_answer_continue();
        }
        break;
    case Ask::Line:
        input_line(m);
        break;
    case Ask::None:
        break;
    }
    return !engine_waiting();
}

void draw_frame() {
    uint16_t *px = viper_frame_begin();
    if (!px) {
        return;
    }
    gfx::Surface s(px, static_cast<int>(viper_display_width()),
                   static_cast<int>(viper_display_height()));
    if (a->state == State::Message) {
        draw_message(s, a->message);
    } else if (a->list != List::None) {
        std::vector<std::string> labels;
        if (a->list == List::Game) {
            for (const Command &c : kGameMenu) {
                labels.emplace_back(c.label);
            }
        } else {
            for (int i : command_order()) {
                labels.emplace_back(kCommands[i].label);
            }
        }
        draw_list(s, a->list == List::Game ? "NetHack" : "Which command?", labels,
                  a->command_cursor, a->command_top, "A: do it  B: back  L/R: page");
    } else {
        const Model &m = engine_model();
        if (m.getpos) {
            a->view.hints = "A:pick B:cancel X:monster Y:object";
        } else {
            a->view.hints = std::string("A:") + context_action(m).label + " X:items Y:cmds St:menu";
        }
        draw(s, m, a->view);
    }
    viper_frame_end(1);
    a->frames++;
}

void create(void *) {
    a = new App;
    const std::string app_dir = viper_app_path() ? viper_app_path() : ".";
    const char *data = viper_data_path();
    if (!data || !*data) {
        show_message("NETHACK\n\nThere's nowhere to keep the game:\nno SD card or fatfs.");
        return;
    }
    if (!file_exists(app_dir + "/nhdat")) {
        show_message("NETHACK\n\nThe game's data (nhdat) is missing from\n" + app_dir +
                     "\n\nBuild it with nethack/tools/make_nhdat.sh");
        return;
    }
    viper_app_handle_back(1); // B is Escape; Select held closes the app
    if (!engine_start(app_dir, data, kHeroName)) {
        show_message("NETHACK\n\n" + engine_error());
    }
}

void destroy() {
    if (!a) {
        return;
    }
    engine_stop(); // saves the game
    delete a;
    a = nullptr;
}

uint32_t loop(const viper_input_t *now, const viper_input_t *previous) {
    if (a->state == State::Message) {
        if (a->dirty) {
            draw_frame();
            a->dirty = false;
        }
        constexpr uint32_t kAny = VIPER_BUTTON_A | VIPER_BUTTON_B | VIPER_BUTTON_START;
        if (viper_button_pressed(now, previous, kAny)) {
            viper_app_exit();
        }
        return 30;
    }

    bool waiting = engine_run(kFrameWaitMs);
    if (engine_finished()) {
        if (engine_error().empty()) {
            viper_app_exit();
        } else {
            show_message("NETHACK\n\n" + engine_error());
        }
        return 16;
    }
    if (waiting && answer(now, previous)) {
        waiting = engine_run(kFrameWaitMs);
    }
    // Drawn only while the engine waits: the model is the engine's otherwise
    if (waiting && !engine_finished()) {
        const Model &m = engine_model();
        if (m.request_id != a->request_id) {
            a->request_id = m.request_id;
            begin_request(m);
        }
        if (a->dirty || m.changes != a->drawn_changes) {
            draw_frame();
            a->drawn_changes = m.changes;
            a->dirty = false;
        }
    }
    return 16;
}

const viper_app_t app = {
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

nethack::AppStatus nethack::app_status() {
    AppStatus s;
    if (!a) {
        return s;
    }
    s.open = true;
    s.message = a->state == State::Message ? a->message : std::string();
    s.commands_open = a->list == List::Commands;
    s.game_menu_open = a->list == List::Game;
    s.frames = a->frames;
    return s;
}

VIPER_APP(app)

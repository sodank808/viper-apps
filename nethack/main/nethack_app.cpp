// NetHack as a Viper ELF app: the game (NetHack 5.0, nethack/) runs on the
// engine's thread (nethack_engine.cpp); this is the app around it. loop()
// gives the engine the turn, and when the engine waits for the player, reads
// the buttons into an answer and draws the game (nethack_view.cpp).
//
// The buttons, playing: the d-pad moves (two at once, diagonally), A opens a
// list of commands, B is Escape, X the inventory, Y searches, L and R go up
// and down stairs, Start the extended commands (#pray, #loot, ...).

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

constexpr uint32_t kDirections =
    VIPER_BUTTON_UP | VIPER_BUTTON_DOWN | VIPER_BUTTON_LEFT | VIPER_BUTTON_RIGHT;

// The command list (A): what each line sends
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

    // The command list, open over the map
    bool commands = false;
    int command_cursor = 0;
    int command_top = 0;

    // The d-pad: a direction pressed waits kDiagonalMs for a second one
    uint32_t dir_pressed = 0; // when the first went down (0: none waiting)
    uint32_t dir_seen = 0;    // the directions down while waiting
    uint32_t dir_held_since = 0;
    uint32_t dir_repeat_at = 0;
    char dir_key = 0; // the key the d-pad sent last, while it's held
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
    a->commands = false;
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

int screen_h() { return static_cast<int>(viper_display_height()); }

// ---- answering what the engine waits for ----

void input_key(const viper_input_t *now, const viper_input_t *previous) {
    const Model &m = engine_model();
    (void)m;
    if (a->commands) {
        const int rows = page_lines(screen_h()) - 1;
        int delta = 0;
        if (pressed(now, previous, VIPER_BUTTON_UP))
            delta = -1;
        if (pressed(now, previous, VIPER_BUTTON_DOWN))
            delta = 1;
        if (pressed(now, previous, VIPER_BUTTON_L))
            delta = -rows;
        if (pressed(now, previous, VIPER_BUTTON_R))
            delta = rows;
        if (delta) {
            move_cursor(a->command_cursor, a->command_top, kCommandCount, rows, delta);
            a->dirty = true;
        }
        if (pressed(now, previous, VIPER_BUTTON_B)) {
            a->commands = false;
            a->dirty = true;
        } else if (pressed(now, previous, VIPER_BUTTON_A)) {
            a->commands = false;
            const std::string keys = kCommands[a->command_cursor].keys;
            engine_queue_keys(keys.substr(1));
            engine_answer_key(static_cast<unsigned char>(keys[0]));
        }
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
        {VIPER_BUTTON_B, kEsc}, {VIPER_BUTTON_X, 'i'}, {VIPER_BUTTON_Y, 's'},
        {VIPER_BUTTON_L, '<'},  {VIPER_BUTTON_R, '>'}, {VIPER_BUTTON_START, '#'},
    };
    for (const Map &k : kKeys) {
        if (pressed(now, previous, k.button)) {
            engine_answer_key(k.key);
            return;
        }
    }
    if (pressed(now, previous, VIPER_BUTTON_A)) {
        a->commands = true;
        a->dirty = true;
    }
}

void input_yes_no(const viper_input_t *now, const viper_input_t *previous) {
    const Request &r = engine_model().request;
    const std::string choices = r.choices.substr(0, r.choices.find(kEsc));
    if (pressed(now, previous, VIPER_BUTTON_B)) {
        engine_answer_yn(kEsc);
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
    const int count = w ? static_cast<int>(w->items.size()) : 0;
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
    } else if (a->commands) {
        std::vector<std::string> labels;
        for (const Command &c : kCommands) {
            labels.emplace_back(c.label);
        }
        draw_list(s, "Which command?", labels, a->command_cursor, a->command_top,
                  "A: do it  B: back  L/R: page");
    } else {
        draw(s, engine_model(), a->view);
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
    s.commands_open = a->commands;
    s.frames = a->frames;
    return s;
}

VIPER_APP(app)

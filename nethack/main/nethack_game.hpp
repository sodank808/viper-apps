#pragma once

// The game as the views see it: what NetHack has put on the map, its
// messages and status, its open menus and text windows, and what it's
// waiting for the player to answer. The window port (port/winviper.c) fills
// it in through nethack_engine.cpp; the views (nethack_view.hpp) only read it.
// Plain C++, no NetHack or Viper headers, so the host's tests use it as is.

#include <array>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "port/nh_port.h"

namespace nethack {

constexpr int kCols = NHV_COLS;
constexpr int kRows = NHV_ROWS;
constexpr char kEsc = 27;

struct Cell {
    int glyph = -1; // NetHack's glyph number, -1 for nothing shown
    char ch = ' ';
    uint8_t color = 0; // NetHack's CLR_* (0..15)
    uint8_t flags = 0; // NHV_CELL_*
    int16_t tile = -1; // its tile in tiles.bin, -1 for none (dark, unexplored)
};

struct MenuItem {
    int index = 0; // 1.. for one that can be chosen, 0 for a heading or text
    char accelerator = 0;
    int attr = 0;
    int color = -1;
    bool selected = false;
    std::string text;
};

struct Window {
    bool open = false;
    int kind = 0;                   // NHV_WIN_*
    std::vector<std::string> lines; // a text window's
    std::vector<MenuItem> items;    // a menu's
    std::string prompt;
    bool menu = false;
};

// What the engine is waiting for
enum class Ask {
    None, // running, or not started
    Key,  // a key: a command, a direction, a key a prompt reads
    YesNo,
    Line,
    Menu, // a choice from window `window`
    Text, // window `window` read, then dismissed
    More, // the messages so far read
};

struct Request {
    Ask ask = Ask::None;
    std::string query;   // YesNo, Line
    std::string choices; // YesNo: the keys that answer it ("yn", "ynq", ...)
    char def = 0;        // YesNo
    int window = 0;      // Menu, Text
    int pick = 0;        // Menu: NHV_PICK_*
};

struct Model {
    std::array<Cell, kCols * kRows> map{};
    int hero_x = 0, hero_y = 0;
    std::deque<std::string> messages; // oldest first, up to kHistory
    size_t new_messages = 0;          // since the last key the player gave
    std::vector<std::string> status;
    // By NetHack's window id (small numbers; 0 unused). An array, not a map:
    // a device's firmware exports only some of the C++ runtime a map needs.
    static constexpr int kMaxWindows = 64;
    std::array<Window, kMaxWindows> windows{};
    // What the context action (A) acts on, as of the last key asked for
    // (nh_port.h's nhv_context())
    unsigned here = 0;  // NHV_HERE_*
    unsigned doors = 0; // closed doors next to the hero, by direction
    // A position being chosen (getpos): what for, and the cursor
    bool getpos = false;
    std::string getpos_goal;
    int cursor_x = 0, cursor_y = 0;
    Request request;
    uint32_t request_id = 0; // counts requests, so a view knows a new one began
    uint32_t changes = 0;    // counts every change, so a view knows to redraw

    static constexpr size_t kHistory = 200;

    Cell &cell(int x, int y) { return map[y * kCols + x]; }
    const Cell &cell(int x, int y) const { return map[y * kCols + x]; }

    void clear_map() {
        map.fill(Cell{});
        changes++;
    }
    void add_message(std::string text) {
        messages.push_back(std::move(text));
        if (messages.size() > kHistory) {
            messages.pop_front();
        }
        new_messages++;
        changes++;
    }
    // A new window: the lowest free id, or 0 if there's none
    int create_window(int kind) {
        for (int id = 1; id < kMaxWindows; id++) {
            if (!windows[id].open) {
                windows[id] = Window{};
                windows[id].open = true;
                windows[id].kind = kind;
                changes++;
                return id;
            }
        }
        return 0;
    }
    void destroy_window(int id) {
        if (id > 0 && id < kMaxWindows) {
            windows[id] = Window{};
            changes++;
        }
    }
    Window *window(int id) {
        return id > 0 && id < kMaxWindows && windows[id].open ? &windows[id] : nullptr;
    }
    const Window *window(int id) const {
        return id > 0 && id < kMaxWindows && windows[id].open ? &windows[id] : nullptr;
    }
};

// The choices a question names at its end when NetHack passes none ("Shall I
// pick ... for you? [ynaq] "): a bracketed run of letters. One with more in
// it ("[fgh or ?*]", an object's letter) takes any key: empty.
inline std::string choices_in_query(const std::string &query) {
    const size_t close = query.find_last_of(']');
    if (close == std::string::npos ||
        query.find_first_not_of(' ', close + 1) != std::string::npos) {
        return {};
    }
    const size_t open = query.find_last_of('[', close);
    if (open == std::string::npos || close - open < 2 || close - open > 9) {
        return {};
    }
    const std::string inside = query.substr(open + 1, close - open - 1);
    for (char c : inside) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) {
            return {};
        }
    }
    return inside;
}

} // namespace nethack

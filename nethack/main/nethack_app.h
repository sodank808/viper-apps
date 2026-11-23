#pragma once

// What the simulator's tests read of the app (tests/test_nethack_boot.cpp);
// the game itself they read through nethack_engine.hpp.

#include <cstdint>
#include <string>

namespace nethack {

struct AppStatus {
    bool open = false;
    std::string message; // the message shown instead of the game, if any
    bool commands_open = false;  // the command list (Y)
    bool game_menu_open = false; // the game menu (Start)
    int map_view = 0;            // MapView: terminal, tiles, small tiles, first person
    int facing = 0;              // first person: 0 north, 1 east, 2 south, 3 west
    int sprites = 0;             // first person: billboards in the last frame
    uint32_t frames = 0; // drawn
};

AppStatus app_status();

} // namespace nethack

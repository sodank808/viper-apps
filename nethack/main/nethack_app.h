#pragma once

// What the simulator's tests read of the app (tests/test_nethack_boot.cpp);
// the game itself they read through nethack_engine.hpp.

#include <cstdint>
#include <string>

namespace nethack {

struct AppStatus {
    bool open = false;
    std::string message; // the message shown instead of the game, if any
    bool commands_open = false;
    uint32_t frames = 0; // drawn
};

AppStatus app_status();

} // namespace nethack

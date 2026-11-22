#pragma once
// What wolf3d_app.cpp shares with wolf3d_sound.cpp and the app's tests

#include <cstddef>
#include <cstdint>
#include <string>

// Sound (wolf3d_sound.cpp): the audio tap and the ring it reads, which the
// engine's thread fills a frame ahead with the engine's mixer (SD_Mix())
void wolf3d_sound_start();
void wolf3d_sound_stop();
extern "C" void wolf3d_sound_fill(void);

namespace wolf3d {

// How the app is doing, for the tests
struct Status {
    bool open = false;    // the app is open
    bool running = false; // the engine is running
    std::string message;  // what the app shows instead (missing data, an error)
    bool playing = false; // the engine says it's playing (not in a menu)
    bool menu = false;    // the menu is open
    int weapon = 0;       // playing: the weapon in hand (0 knife, 1 pistol, ...)
    bool run = false;     // playing: the player runs
    uint32_t frames = 0;  // frames shown
    size_t heap_used = 0; // the engine's memory
    size_t heap_peak = 0;
};
Status status();

} // namespace wolf3d

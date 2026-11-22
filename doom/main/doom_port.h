#pragma once
// The pieces of the Doom app that talk to each other (C++ only; the engine
// calls into them through its I_* functions)

#include <cstdint>

// doom_statics.cpp: allocates the engine's tables (before D_DoomMainSetup())
void doom_statics_init();

// doom_sound.cpp: the sound effects and the OPL music, mixed on the game's
// thread into a ring the audio tap reads from
bool doom_sound_start(); // installs the tap; false on a board without sound
void doom_sound_pump();  // mixes what the tap will need next, once a frame
void doom_sound_pause(bool paused);
void doom_sound_stop(); // removes the tap and frees the music

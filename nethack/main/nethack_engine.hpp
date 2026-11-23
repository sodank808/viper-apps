#pragma once

// NetHack's engine: the game runs on a thread of its own, written as the
// program NetHack is, which waits for a key wherever it pleases. Wherever it
// waits for the player (nh_port.h's nhv_get_key(), nhv_yn(), nhv_menu_select()
// ...) it hands the turn to the app's loop() and waits for an answer. Only
// one side runs at a time, so the model needs no locks: the views read it
// only while the engine waits.

#include <cstdint>
#include <string>
#include <vector>

#include "nethack_game.hpp"

namespace nethack {

// Starts a game (restoring the hero's save, if there is one). app_dir has
// nhdat, sysconf and nethackrc; data_dir gets the levels, saves and scores.
bool engine_start(const std::string &app_dir, const std::string &data_dir,
                  const std::string &hero_name);

// Ends the game: saves it, if one is going, and waits for the engine to stop.
// Frees everything NetHack had.
void engine_stop();

// Runs the engine until it next waits for the player, or wait_ms passes (a
// level being made). True once it's waiting (or finished).
bool engine_run(long wait_ms);

bool engine_waiting();
bool engine_finished();
// Why NetHack ended, if it ended with an error
const std::string &engine_error();

// The game, to read while the engine waits
const Model &engine_model();

// Answers (each gives the engine the turn; engine_run() runs it)
void engine_answer_key(int key);
void engine_answer_yn(char key);
void engine_answer_line(const std::string &line); // "\033" to cancel
// The chosen items' indexes and counts (-1 for all); empty, or cancel, for none
void engine_answer_menu(const std::vector<int> &indexes, bool cancel);
void engine_answer_continue(); // Text, More

// Keys sent before the engine asks: each nhv_get_key() takes the next
// without waiting (a command from the command list, "20s")
void engine_queue_keys(const std::string &keys);

// Memory NetHack holds now, and the most it has held this game
size_t engine_heap_used();
size_t engine_heap_peak();

} // namespace nethack

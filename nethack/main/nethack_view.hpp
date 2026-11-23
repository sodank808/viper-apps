#pragma once

// Drawing the game: the terminal view (NetHack's characters in its colors,
// the map scrolled to keep the hero in view), with what the engine is
// waiting for drawn over it (a menu, a text window, a yes/no question, the
// messages to read). Plain gfx drawing into the frame, so the host's tests
// can draw into a buffer of their own.

#include <string>
#include <vector>

#include "gfx/surface.hpp"
#include "nethack_game.hpp"

namespace nethack {

// The view's own state: what the player is pointing at in a menu or text
// window, which yes/no choice is lit. Kept by the app, reset for each request.
struct ViewState {
    int cursor = 0;          // Menu: the item the cursor is on
    int top = 0;             // Menu, Text: the first line shown
    std::vector<bool> marks; // Menu (PICK_ANY): the items marked, by item
    int choice = 0;          // YesNo: the lit choice
    std::string note;        // a line over the hints (a command list's title)
    std::string hints;       // the controls line while playing
};

// Lines a menu or text window shows at once on an h-pixel-high screen
int page_lines(int h);

// A text window's lines as the view shows them, on a screen `width` pixels
// wide: lines too long for it wrapped, keeping their indent
std::vector<MenuItem> text_lines(const Window &w, int width);

void draw(gfx::Surface &s, const Model &m, const ViewState &v);

// A list filling the screen, the cursor on one line (the command list)
void draw_list(gfx::Surface &s, const std::string &title, const std::vector<std::string> &items,
               int cursor, int top, const std::string &hints);

// A message filling the screen (missing data, an error)
void draw_message(gfx::Surface &s, const std::string &text);

} // namespace nethack

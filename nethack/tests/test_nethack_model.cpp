// The game model NetHack's window port fills in (main/nethack_game.hpp):
// plain C++, tested without NetHack; and how the view lays out its text.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "nethack_game.hpp"
#include "nethack_tiles.hpp"
#include "nethack_view.hpp"

using nethack::choices_in_query;

TEST_CASE("NetHack's questions name their choices at the end", "[nethack]") {
    CHECK(choices_in_query("Shall I pick a character for you? [ynaq] ") == "ynaq");
    CHECK(choices_in_query("Really attack the shopkeeper? [yn]") == "yn");
    // An object's letter, or a list: any key
    CHECK(choices_in_query("What do you want to eat? [fg or ?*]").empty());
    CHECK(choices_in_query("What do you want to drop? [$a-d or ?*]").empty());
    // Nothing in brackets, or not at the end
    CHECK(choices_in_query("In what direction?").empty());
    CHECK(choices_in_query("[yn] is not the end of this").empty());
    CHECK(choices_in_query("Empty []").empty());
}

TEST_CASE("NetHack's game model keeps the map, messages and windows", "[nethack]") {
    nethack::Model m;
    m.cell(10, 5).ch = '@';
    CHECK(m.cell(10, 5).ch == '@');
    m.clear_map();
    CHECK(m.cell(10, 5).ch == ' ');
    CHECK(m.cell(10, 5).glyph == -1);

    // Messages: the newest kept, up to the history's length, and counted
    // until the player gives a key
    for (size_t i = 0; i < nethack::Model::kHistory + 5; i++) {
        m.add_message("message " + std::to_string(i));
    }
    CHECK(m.messages.size() == nethack::Model::kHistory);
    CHECK(m.messages.back() == "message " + std::to_string(nethack::Model::kHistory + 4));
    CHECK(m.new_messages == nethack::Model::kHistory + 5);

    // Windows take the lowest free id, as NetHack's small ids expect
    const int a = m.create_window(NHV_WIN_MENU);
    const int b = m.create_window(NHV_WIN_TEXT);
    CHECK(a == 1);
    CHECK(b == 2);
    m.destroy_window(a);
    CHECK(m.window(a) == nullptr);
    CHECK(m.window(b) != nullptr);
    CHECK(m.create_window(NHV_WIN_MENU) == 1);
}

TEST_CASE("NetHack's text windows wrap long lines, keeping their indent", "[nethack]") {
    nethack::Window w;
    auto add = [&w](const std::string &text) {
        nethack::MenuItem item;
        item.text = text;
        w.items.push_back(item);
    };
    add("Short line");
    add("");
    add("  This line is far too long to fit on a screen forty characters across");
    // 320 pixels: 40 cells, 39 of them for text
    const auto lines = nethack::text_lines(w, 320);
    REQUIRE(lines.size() == 4);
    CHECK(lines[0].text == "Short line");
    CHECK(lines[1].text.empty());
    CHECK(lines[2].text == "  This line is far too long to fit on a");
    CHECK(lines[3].text == "  screen forty characters across");
    for (const auto &line : lines) {
        CHECK(line.text.size() <= 39);
    }
}

TEST_CASE("NetHack's tiles are read from tiles.bin as they're needed", "[nethack]") {
    nethack::TileSet tiles;
    CHECK_FALSE(tiles.open("/nonexistent/tiles.bin"));
    CHECK(tiles.tile(0) == nullptr);
    REQUIRE(tiles.open(std::string(VIPER_SIM_NETHACK_DATA) + "/tiles.bin"));
    // As many as src/tile.c numbers (total_tiles_used)
    CHECK(tiles.count() == 2303);
    CHECK(tiles.tile(-1) == nullptr);
    CHECK(tiles.tile(tiles.count()) == nullptr);

    // A giant ant (tile 0): its corners are the background, drawn black,
    // and it isn't all background
    const gfx::Pixel *ant = tiles.tile(0);
    REQUIRE(ant);
    CHECK(ant[0] == 0);
    int drawn = 0;
    for (int i = 0; i < 16 * 16; i++) {
        drawn += ant[i] != 0;
    }
    CHECK(drawn > 20);
    // Its statue (the first generated tile) is the same shape in grays
    const std::vector<gfx::Pixel> ant_copy(ant, ant + 16 * 16);
    const gfx::Pixel *statue = tiles.tile(1515);
    REQUIRE(statue);
    for (int i = 0; i < 16 * 16; i++) {
        CHECK((statue[i] == 0) == (ant_copy[i] == 0));
        const gfx::Rgb p = gfx::unpack(statue[i]);
        CHECK(std::abs(p.r - p.g) < 24);
        CHECK(std::abs(p.g - p.b) < 24);
    }
    // The small one: each pixel the average of four
    const gfx::Pixel *small = tiles.tile(0, true);
    REQUIRE(small);
    CHECK(small[0] == 0);

    // More tiles than the cache holds, and back: still right
    for (int t = 0; t < 600; t++) {
        REQUIRE(tiles.tile(t));
    }
    const gfx::Pixel *again = tiles.tile(0);
    REQUIRE(again);
    CHECK(std::equal(ant_copy.begin(), ant_copy.end(), again));
}

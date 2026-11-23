// The game model NetHack's window port fills in (main/nethack_game.hpp):
// plain C++, tested without NetHack.

#include <catch2/catch_test_macros.hpp>

#include "nethack_game.hpp"

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

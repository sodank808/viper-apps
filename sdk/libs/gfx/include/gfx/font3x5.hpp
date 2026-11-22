#pragma once

// gfx::kFont3x5: a tiny font for scores, counters and HUDs. Digits (Cradle's
// and Marble Maze's), capitals and a little punctuation, 3x5 with a pixel
// between characters. Lower case draws as capitals; characters it doesn't
// have draw as spaces.
//
//     s.text(gfx::kFont3x5, "SCORE 120", 4, 4, gfx::rgb(255, 255, 255), 2);
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <array>
#include <cstdint>

#include "gfx/font.hpp"

namespace gfx {

namespace font3x5_detail {

constexpr char kFirst = ' ', kLast = '~';
constexpr int kCount = kLast - kFirst + 1;

struct Art {
    char c;
    const char *rows; // 15 characters, row by row: '#' is ink
};

// clang-format off
inline constexpr Art kArt[] = {
    {'0', "###" "# #" "# #" "# #" "###"}, {'1', " # " "## " " # " " # " "###"},
    {'2', "###" "  #" "###" "#  " "###"}, {'3', "###" "  #" "###" "  #" "###"},
    {'4', "# #" "# #" "###" "  #" "  #"}, {'5', "###" "#  " "###" "  #" "###"},
    {'6', "###" "#  " "###" "# #" "###"}, {'7', "###" "  #" "  #" "  #" "  #"},
    {'8', "###" "# #" "###" "# #" "###"}, {'9', "###" "# #" "###" "  #" "###"},
    {'A', " # " "# #" "###" "# #" "# #"}, {'B', "## " "# #" "## " "# #" "## "},
    {'C', " ##" "#  " "#  " "#  " " ##"}, {'D', "## " "# #" "# #" "# #" "## "},
    {'E', "###" "#  " "## " "#  " "###"}, {'F', "###" "#  " "## " "#  " "#  "},
    {'G', " ##" "#  " "# #" "# #" " ##"}, {'H', "# #" "# #" "###" "# #" "# #"},
    {'I', "###" " # " " # " " # " "###"}, {'J', "  #" "  #" "  #" "# #" " # "},
    {'K', "# #" "# #" "## " "# #" "# #"}, {'L', "#  " "#  " "#  " "#  " "###"},
    {'M', "# #" "###" "###" "# #" "# #"}, {'N', "## " "# #" "# #" "# #" "# #"},
    {'O', " # " "# #" "# #" "# #" " # "}, {'P', "## " "# #" "## " "#  " "#  "},
    {'Q', " # " "# #" "# #" "## " " ##"}, {'R', "## " "# #" "## " "# #" "# #"},
    {'S', " ##" "#  " " # " "  #" "## "}, {'T', "###" " # " " # " " # " " # "},
    {'U', "# #" "# #" "# #" "# #" "###"}, {'V', "# #" "# #" "# #" "# #" " # "},
    {'W', "# #" "# #" "###" "###" "# #"}, {'X', "# #" "# #" " # " "# #" "# #"},
    {'Y', "# #" "# #" " # " " # " " # "}, {'Z', "###" "  #" " # " "#  " "###"},
    {'.', "   " "   " "   " "   " " # "}, {',', "   " "   " "   " " # " "#  "},
    {':', "   " " # " "   " " # " "   "}, {'-', "   " "   " "###" "   " "   "},
    {'+', "   " " # " "###" " # " "   "}, {'=', "   " "###" "   " "###" "   "},
    {'/', "  #" "  #" " # " "#  " "#  "}, {'!', " # " " # " " # " "   " " # "},
    {'?', "## " "  #" " # " "   " " # "}, {'%', "# #" "  #" " # " "#  " "# #"},
    {'(', " # " "#  " "#  " "#  " " # "}, {')', " # " "  #" "  #" "  #" " # "},
    {'<', "  #" " # " "#  " " # " "  #"}, {'>', "#  " " # " "  #" " # " "#  "},
    {'\'', " # " " # " "   " "   " "   "}, {'*', "   " "# #" " # " "# #" "   "},
};
// clang-format on

struct Table {
    std::array<std::array<uint8_t, 5>, kCount> rows{};
    std::array<bool, kCount> has{};
};

constexpr Table make_table() {
    Table t;
    for (const Art &a : kArt) {
        const int i = a.c - kFirst;
        t.has[i] = true;
        for (int y = 0; y < 5; y++) {
            uint8_t bits = 0;
            for (int x = 0; x < 3; x++) {
                bits = static_cast<uint8_t>((bits << 1) | (a.rows[y * 3 + x] == '#' ? 1 : 0));
            }
            t.rows[i][y] = bits;
        }
    }
    return t;
}

inline constexpr Table kTable = make_table();

inline const uint8_t *rows(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    const int i = c - kFirst;
    return i >= 0 && i < kCount && kTable.has[i] ? kTable.rows[i].data() : nullptr;
}

} // namespace font3x5_detail

inline constexpr Font kFont3x5{3, 5, 4, &font3x5_detail::rows};

} // namespace gfx

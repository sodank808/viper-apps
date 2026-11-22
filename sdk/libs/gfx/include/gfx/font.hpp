#pragma once

// Bitmap fonts for drawing text into a framebuffer, with no LVGL needed (LVGL
// fonts' glyph bitmaps aren't exported to ELF apps). A font is a table of
// glyphs packed into bits at compile time: read-only data, no RAM. Fonts
// are fixed width unless they give an advance for each character.
//
// kFont5x7: capitals, digits and a little punctuation, from Blocks (and Turbo
// Blast's + and ,). Lower case draws as capitals; characters it doesn't have
// draw as spaces.
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <array>
#include <cstdint>
#include <string_view>

namespace gfx {

struct Font {
    uint8_t width; // glyph cell, in pixels (at most 8)
    uint8_t height;
    uint8_t advance; // from one character to the next; advance - width is the space between
    // The glyph's rows, top first, bit (width - 1) the leftmost pixel; nullptr
    // for a space or a character the font doesn't have
    const uint8_t *(*rows)(char c);
    // A proportional font's advance for each character (the space after it
    // included); nullptr when every character takes `advance`
    uint8_t (*advance_of)(char c) = nullptr;
};

// How far c moves the pen
inline int advance(const Font &font, char c) {
    return font.advance_of ? font.advance_of(c) : font.advance;
}

// Calls plot(x, y) for every pixel of s's ink, drawn with its top left at
// (x, y), each font pixel scale x scale. Returns the x after the last
// character. Surface::text draws with it; so can a canvas that isn't RGB565.
template <class Plot>
int text_pixels(const Font &font, std::string_view s, int x, int y, Plot &&plot, int scale = 1) {
    for (char ch : s) {
        if (const uint8_t *rows = font.rows(ch)) {
            for (int gy = 0; gy < font.height; gy++) {
                for (int gx = 0; gx < font.width; gx++) {
                    if ((rows[gy] >> (font.width - 1 - gx)) & 1) {
                        for (int sy = 0; sy < scale; sy++) {
                            for (int sx = 0; sx < scale; sx++) {
                                plot(x + gx * scale + sx, y + gy * scale + sy);
                            }
                        }
                    }
                }
            }
        }
        x += advance(font, ch) * scale;
    }
    return x;
}

namespace font5x7_detail {

struct Glyph {
    char c;
    std::array<uint8_t, 7> rows;
};

// 35 characters, row by row: '#' is ink
constexpr Glyph glyph(char c, const char (&art)[36]) {
    Glyph g{c, {}};
    for (int y = 0; y < 7; y++) {
        uint8_t bits = 0;
        for (int x = 0; x < 5; x++) {
            bits = static_cast<uint8_t>((bits << 1) | (art[y * 5 + x] == '#' ? 1 : 0));
        }
        g.rows[y] = bits;
    }
    return g;
}

// clang-format off
inline constexpr Glyph kGlyphs[] = {
    glyph('A', " ### "  "#   #"  "#   #"  "#####"  "#   #"  "#   #"  "#   #"),
    glyph('B', "#### "  "#   #"  "#   #"  "#### "  "#   #"  "#   #"  "#### "),
    glyph('C', " ### "  "#   #"  "#    "  "#    "  "#    "  "#   #"  " ### "),
    glyph('D', "#### "  "#   #"  "#   #"  "#   #"  "#   #"  "#   #"  "#### "),
    glyph('E', "#####"  "#    "  "#    "  "#### "  "#    "  "#    "  "#####"),
    glyph('F', "#####"  "#    "  "#    "  "#### "  "#    "  "#    "  "#    "),
    glyph('G', " ### "  "#   #"  "#    "  "# ###"  "#   #"  "#   #"  " ####"),
    glyph('H', "#   #"  "#   #"  "#   #"  "#####"  "#   #"  "#   #"  "#   #"),
    glyph('I', " ### "  "  #  "  "  #  "  "  #  "  "  #  "  "  #  "  " ### "),
    glyph('J', "  ###"  "   # "  "   # "  "   # "  "   # "  "#  # "  " ##  "),
    glyph('K', "#   #"  "#  # "  "# #  "  "##   "  "# #  "  "#  # "  "#   #"),
    glyph('L', "#    "  "#    "  "#    "  "#    "  "#    "  "#    "  "#####"),
    glyph('M', "#   #"  "## ##"  "# # #"  "# # #"  "#   #"  "#   #"  "#   #"),
    glyph('N', "#   #"  "#   #"  "##  #"  "# # #"  "#  ##"  "#   #"  "#   #"),
    glyph('O', " ### "  "#   #"  "#   #"  "#   #"  "#   #"  "#   #"  " ### "),
    glyph('P', "#### "  "#   #"  "#   #"  "#### "  "#    "  "#    "  "#    "),
    glyph('Q', " ### "  "#   #"  "#   #"  "#   #"  "# # #"  "#  # "  " ## #"),
    glyph('R', "#### "  "#   #"  "#   #"  "#### "  "# #  "  "#  # "  "#   #"),
    glyph('S', " ####"  "#    "  "#    "  " ### "  "    #"  "    #"  "#### "),
    glyph('T', "#####"  "  #  "  "  #  "  "  #  "  "  #  "  "  #  "  "  #  "),
    glyph('U', "#   #"  "#   #"  "#   #"  "#   #"  "#   #"  "#   #"  " ### "),
    glyph('V', "#   #"  "#   #"  "#   #"  "#   #"  "#   #"  " # # "  "  #  "),
    glyph('W', "#   #"  "#   #"  "#   #"  "# # #"  "# # #"  "# # #"  " # # "),
    glyph('X', "#   #"  "#   #"  " # # "  "  #  "  " # # "  "#   #"  "#   #"),
    glyph('Y', "#   #"  "#   #"  " # # "  "  #  "  "  #  "  "  #  "  "  #  "),
    glyph('Z', "#####"  "    #"  "   # "  "  #  "  " #   "  "#    "  "#####"),
    glyph('0', " ### "  "#   #"  "#  ##"  "# # #"  "##  #"  "#   #"  " ### "),
    glyph('1', "  #  "  " ##  "  "  #  "  "  #  "  "  #  "  "  #  "  " ### "),
    glyph('2', " ### "  "#   #"  "    #"  "   # "  "  #  "  " #   "  "#####"),
    glyph('3', "#####"  "   # "  "  #  "  "   # "  "    #"  "#   #"  " ### "),
    glyph('4', "   # "  "  ## "  " # # "  "#  # "  "#####"  "   # "  "   # "),
    glyph('5', "#####"  "#    "  "#### "  "    #"  "    #"  "#   #"  " ### "),
    glyph('6', "  ## "  " #   "  "#    "  "#### "  "#   #"  "#   #"  " ### "),
    glyph('7', "#####"  "    #"  "   # "  "  #  "  " #   "  " #   "  " #   "),
    glyph('8', " ### "  "#   #"  "#   #"  " ### "  "#   #"  "#   #"  " ### "),
    glyph('9', " ### "  "#   #"  "#   #"  " ####"  "    #"  "   # "  " ##  "),
    glyph(':', "     "  "  #  "  "  #  "  "     "  "  #  "  "  #  "  "     "),
    glyph('.', "     "  "     "  "     "  "     "  "     "  " ##  "  " ##  "),
    glyph('-', "     "  "     "  "     "  "#####"  "     "  "     "  "     "),
    glyph('>', " #   "  "  #  "  "   # "  "    #"  "   # "  "  #  "  " #   "),
    glyph('!', "  #  "  "  #  "  "  #  "  "  #  "  "  #  "  "     "  "  #  "),
    glyph('/', "     "  "    #"  "   # "  "  #  "  " #   "  "#    "  "     "),
    glyph('<', "   # "  "  #  "  " #   "  "#    "  " #   "  "  #  "  "   # "),
    glyph('+', "     "  "  #  "  "  #  "  "#####"  "  #  "  "  #  "  "     "),
    glyph(',', "     "  "     "  "     "  "     "  " ##  "  "  #  "  " #   "),
};
// clang-format on

// Where c's glyph is in kGlyphs, or -1. An index rather than a search: a
// range-for over kGlyphs makes a pointer one past its end, which the ELF
// loader can't always relocate when the table ends its section.
constexpr int index(char c) {
    if (c >= 'a' && c <= 'z') {
        c = static_cast<char>(c - 'a' + 'A');
    }
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= '0' && c <= '9') {
        return 26 + (c - '0');
    }
    switch (c) {
    case ':':
        return 36;
    case '.':
        return 37;
    case '-':
        return 38;
    case '>':
        return 39;
    case '!':
        return 40;
    case '/':
        return 41;
    case '<':
        return 42;
    case '+':
        return 43;
    case ',':
        return 44;
    default:
        return -1;
    }
}

constexpr bool indexes_match() {
    constexpr int n = sizeof(kGlyphs) / sizeof(kGlyphs[0]);
    for (int i = 0; i < n; i++) {
        if (index(kGlyphs[i].c) != i) {
            return false;
        }
    }
    return true;
}
static_assert(indexes_match(), "kGlyphs is out of step with index()");

inline const uint8_t *rows(char c) {
    const int i = index(c);
    return i < 0 ? nullptr : kGlyphs[i].rows.data();
}

} // namespace font5x7_detail

inline constexpr Font kFont5x7{5, 7, 6, &font5x7_detail::rows};

} // namespace gfx

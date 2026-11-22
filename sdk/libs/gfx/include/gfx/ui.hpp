#pragma once

// gfx::ui: menus for games that draw their own frames (VideoBlit apps and ELF
// games), where LVGL and the system UI kit aren't running. A pause menu, a
// settings screen, a box with a heading: the same logic every game wrote for
// itself, drawn in the game's own colors and font through a Theme.
//
//     gfx::ui::Menu menu_;
//     gfx::ui::Item items_[3] = {{"RESUME"}, {"RESTART"}, {"QUIT"}};
//
//     // each frame, with what was pressed this frame
//     const auto r = menu_.update(items_, keys);
//     if (r.what == gfx::ui::Result::Chosen && r.item == 0) { ... }
//     menu_.draw(s, area, items_, theme);
//
// The menu doesn't play sounds or read buttons: the app fills Keys from
// GamepadState or viper_input_t, and plays what it likes for each Result.
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <algorithm>
#include <cstdint>
#include <span>
#include <string_view>

#include "gfx/color.hpp"
#include "gfx/font.hpp"
#include "gfx/rect.hpp"
#include "gfx/surface.hpp"

namespace gfx::ui {

// The buttons pressed this frame (edges, not held)
struct Keys {
    bool up = false, down = false, left = false, right = false, a = false, b = false;
};

// Something drawn around text: a dark outline to keep it readable over a
// busy picture, or a glow
struct Halo {
    enum Kind : uint8_t { None, Outline, Glow };
    Kind kind = None;
    Pixel color = 0;
};

// Colors named as in the system UI's ui_theme.hpp, so a game can look like an
// LVGL app if it wants to
struct Theme {
    Pixel panel = rgb_hex(0x1c1f25);  // boxes
    Pixel line = rgb_hex(0x3a3f48);   // their borders
    Pixel text = rgb_hex(0xe8eaed);   // headings, and values on settings rows
    Pixel dim = rgb_hex(0x8a919c);    // rows that aren't selected
    Pixel accent = rgb_hex(0x2ed47a); // the selected row
    const Font *font = &kFont5x7;
    int scale = 1;                  // rows
    int selected_scale = 0;         // the selected row, if bigger (0: scale)
    int heading_scale = 0;          // dialog() headings (0: scale)
    int row_gap = 3;                // font pixels between rows, times scale
    bool arrows_both_sides = false; // "> RESUME <" rather than "> RESUME"
    Halo heading_halo, row_halo;
};

// The system UI's look, in the 5x7 font
inline constexpr Theme kSystemTheme{};

// A menu row
struct Item {
    enum class Kind : uint8_t { Action, Toggle, Choice, Number };
    const char *label = "";
    Kind kind = Kind::Action;
    bool *on = nullptr;                   // Toggle: shown ON or OFF
    int *value = nullptr;                 // Choice (an index into choices) and Number
    int min = 0, max = 0, step = 1;       // Number
    const char *const *choices = nullptr; // Choice: max + 1 names
    // Number: how to show the value, written into buf (at least 16 chars);
    // returns buf, or another string. nullptr shows the number.
    const char *(*format)(int value, char *buf) = nullptr;
    bool gap_before = false; // half a row of space above it (e.g. before Back)

    static Item action(const char *label) { return Item{label}; }
    static Item toggle(const char *label, bool &on) {
        Item i{label, Kind::Toggle};
        i.on = &on;
        return i;
    }
    static Item choice(const char *label, int &value, std::span<const char *const> names) {
        Item i{label, Kind::Choice};
        i.value = &value;
        i.choices = names.data();
        i.max = static_cast<int>(names.size()) - 1;
        return i;
    }
    static Item number(const char *label, int &value, int min, int max, int step = 1,
                       const char *(*format)(int, char *) = nullptr) {
        Item i{label, Kind::Number};
        i.value = &value;
        i.min = min;
        i.max = max;
        i.step = step;
        i.format = format;
        return i;
    }
};

struct Result {
    enum What : uint8_t {
        None,
        Moved,   // the highlight moved
        Changed, // a Toggle, Choice or Number row changed
        Chosen,  // A on an Action row
        Back,    // B
    };
    What what = None;
    int item = 0; // the row it happened on
};

// Text drawn with the theme's font and a halo
inline void text(Surface &s, const Theme &t, std::string_view str, int x, int y, Pixel c, int scale,
                 Halo halo = {}) {
    if (halo.kind == Halo::Outline) {
        // A pixel all around, and one more down and right as a shadow
        static constexpr int kOffsets[5][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {1, 1}};
        for (const auto &o : kOffsets) {
            s.text(*t.font, str, x + o[0], y + o[1], halo.color, scale);
        }
    } else if (halo.kind == Halo::Glow) {
        const int r = std::max(1, scale / 2);
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx || dy) {
                    s.text(*t.font, str, x + dx, y + dy, halo.color, scale);
                }
            }
        }
    }
    s.text(*t.font, str, x, y, c, scale);
}

// Centered across x .. x + w, as Surface::text_centered centers
inline void text_centered(Surface &s, const Theme &t, std::string_view str, int x, int w, int y,
                          Pixel c, int scale, Halo halo = {}) {
    text(s, t, str, x + (w - text_width(*t.font, str, scale)) / 2, y, c, scale, halo);
}

class Menu {
  public:
    int selected = 0;

    enum class Layout : uint8_t {
        Centered,   // each row centered across the box: pause menus
        LabelValue, // labels on the left, values against the right: settings
    };

    // Moves the highlight (wrapping around), changes the selected row's value
    // in place, and says what happened
    Result update(std::span<Item> items, const Keys &keys) {
        const int n = static_cast<int>(items.size());
        if (n == 0) {
            return {};
        }
        selected = std::clamp(selected, 0, n - 1);
        if (keys.up || keys.down) {
            selected = (selected + (keys.up ? n - 1 : 1)) % n;
            return {Result::Moved, selected};
        }
        if (keys.b) {
            return {Result::Back, selected};
        }
        Item &item = items[selected];
        const int step = keys.right ? 1 : keys.left ? -1 : 0;
        switch (item.kind) {
        case Item::Kind::Action:
            return keys.a ? Result{Result::Chosen, selected} : Result{};
        case Item::Kind::Toggle:
            if (step || keys.a) {
                *item.on = !*item.on;
                return {Result::Changed, selected};
            }
            return {};
        case Item::Kind::Choice:
            if (step || keys.a) {
                const int count = item.max + 1;
                *item.value = (*item.value + (step < 0 ? count - 1 : 1)) % count;
                return {Result::Changed, selected};
            }
            return {};
        case Item::Kind::Number: {
            const int v = std::clamp(*item.value + step * item.step, item.min, item.max);
            if (v != *item.value) {
                *item.value = v;
                return {Result::Changed, selected};
            }
            return {};
        }
        }
        return {};
    }

    // The value a row shows ("ON", a choice's name, a number), or "" for an
    // action. buf holds at least 16 characters.
    static const char *value_text(const Item &item, char *buf) {
        switch (item.kind) {
        case Item::Kind::Action:
            return "";
        case Item::Kind::Toggle:
            return *item.on ? "ON" : "OFF";
        case Item::Kind::Choice:
            return item.choices[std::clamp(*item.value, 0, item.max)];
        case Item::Kind::Number:
            if (item.format) {
                return item.format(*item.value, buf);
            }
            format_int(*item.value, buf);
            return buf;
        }
        return "";
    }

    // How far from one row's top to the next
    static int pitch(const Theme &t, int row_scale) {
        return t.font->height * row_scale + t.row_gap * t.scale;
    }

    // The height draw() takes for these rows (with pitch 0)
    int height(std::span<const Item> items, const Theme &t) const {
        int h = 0;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            h += row_pitch(t, i, 0) + (items[i].gap_before ? pitch(t, t.scale) / 2 : 0);
        }
        return h;
    }

    // Draws the rows from the top of box down. A `fixed_pitch` spaces them
    // evenly instead of by the font's height and the theme's gap.
    void draw(Surface &s, Rect box, std::span<const Item> items, const Theme &t,
              Layout layout = Layout::Centered, int fixed_pitch = 0) const {
        int y = box.y;
        for (int i = 0; i < static_cast<int>(items.size()); i++) {
            const Item &item = items[i];
            const bool on = i == selected;
            const int sc = on && t.selected_scale ? t.selected_scale : t.scale;
            if (item.gap_before) {
                y += (fixed_pitch ? fixed_pitch : pitch(t, t.scale)) / 2;
            }
            const Pixel c = on ? t.accent : t.dim;
            char buf[16];
            if (layout == Layout::Centered) {
                char line[48];
                int n = 0;
                auto add = [&](const char *str) {
                    while (*str && n < static_cast<int>(sizeof(line)) - 1) {
                        line[n++] = *str++;
                    }
                };
                if (on) {
                    add("> ");
                }
                add(item.label);
                const char *value = value_text(item, buf);
                if (*value) {
                    add(" ");
                    add(value);
                }
                if (on && t.arrows_both_sides) {
                    add(" <");
                }
                line[n] = 0;
                text_centered(s, t, line, box.x, box.w, y, c, sc, t.row_halo);
            } else {
                const Pixel label_c = on ? t.accent : t.text;
                if (on) {
                    text(s, t, ">", box.x - 2 * t.font->advance * sc, y, t.accent, sc, t.row_halo);
                }
                text(s, t, item.label, box.x, y, label_c, sc, t.row_halo);
                const char *value = value_text(item, buf);
                if (*value) {
                    text(s, t, value, box.right() - text_width(*t.font, value, sc), y, label_c, sc,
                         t.row_halo);
                }
            }
            y += row_pitch(t, i, fixed_pitch);
        }
    }

  private:
    int row_pitch(const Theme &t, int i, int fixed) const {
        if (fixed) {
            return fixed;
        }
        return pitch(t, i == selected && t.selected_scale ? t.selected_scale : t.scale);
    }

    static void format_int(int v, char *buf) {
        char digits[12];
        int n = 0;
        const bool negative = v < 0;
        unsigned u = negative ? 0u - static_cast<unsigned>(v) : static_cast<unsigned>(v);
        do {
            digits[n++] = static_cast<char>('0' + u % 10);
            u /= 10;
        } while (u);
        int o = 0;
        if (negative) {
            buf[o++] = '-';
        }
        while (n) {
            buf[o++] = digits[--n];
        }
        buf[o] = 0;
    }
};

// A box with a 1-pixel border: the panel a pause menu or a message sits on
inline void box(Surface &s, Rect r, const Theme &t) {
    s.fill(r.grow(1), t.line);
    s.fill(r, t.panel);
}

// From a dialog's heading to its first row: the heading, its gap, and a
// blank row
inline int heading_space(const Theme &t) {
    const int hs = t.heading_scale ? t.heading_scale : t.scale;
    return t.font->height * hs + t.row_gap * t.scale + Menu::pitch(t, t.scale);
}

// The height dialog() needs for a heading and a menu, with `pad` font pixels
// (times scale) above and below them
inline int dialog_height(const Theme &t, const Menu *menu, std::span<const Item> items,
                         int pad = 4) {
    const int hs = t.heading_scale ? t.heading_scale : t.scale;
    if (!menu || items.empty()) {
        return 2 * pad * t.scale + t.font->height * hs;
    }
    return 2 * pad * t.scale + heading_space(t) + menu->height(items, t) - t.row_gap * t.scale;
}

// A controller hint: the button's name on a chip ("A") and what it does
struct Hint {
    const char *button = "";
    const char *label = "";
};

// The system hint bar's look, for a framebuffer game: a strip in the panel
// color along `r`, each hint a chip with its button then its label, from the
// left; a hint whose button is "B" goes at the right end, as the system
// bar's Back does. Empty hints are skipped.
inline void hint_bar(Surface &s, Rect r, std::span<const Hint> hints, const Theme &t) {
    s.fill(r, t.panel);
    s.hline(r.x, r.right() - 1, r.y, t.line);
    const int fh = t.font->height * t.scale;
    const int y = r.y + (r.h - fh) / 2 + 1;
    const int pad = 3 * t.scale;
    auto chip = [&](const Hint &h, int x) {
        const int bw = text_width(*t.font, h.button, t.scale) + 2 * pad;
        s.fill(Rect{x, y - 2, bw, fh + 3}, t.line);
        text(s, t, h.button, x + pad, y, t.text, t.scale);
        text(s, t, h.label, x + bw + pad, y, t.dim, t.scale);
        return bw + 2 * pad + text_width(*t.font, h.label, t.scale);
    };
    auto width = [&](const Hint &h) {
        return text_width(*t.font, h.button, t.scale) + 4 * pad +
               text_width(*t.font, h.label, t.scale);
    };
    int x = r.x + 2 * pad;
    for (const Hint &h : hints) {
        if (!*h.button || std::string_view(h.button) == "B") {
            continue;
        }
        x += chip(h, x) + 4 * pad;
    }
    for (const Hint &h : hints) {
        if (std::string_view(h.button) == "B") {
            chip(h, r.right() - 2 * pad - width(h));
        }
    }
}

// A box with a heading, and a menu under it if there is one: a pause menu, or
// a "PAUSED" box on its own
inline void dialog(Surface &s, Rect r, const Theme &t, const char *heading,
                   const Menu *menu = nullptr, std::span<const Item> items = {}, int pad = 4) {
    box(s, r, t);
    const int hs = t.heading_scale ? t.heading_scale : t.scale;
    const int y = r.y + pad * t.scale;
    text_centered(s, t, heading, r.x, r.w, y, t.text, hs, t.heading_halo);
    if (menu && !items.empty()) {
        const int top = y + heading_space(t);
        menu->draw(s, Rect{r.x, top, r.w, r.bottom() - top}, items, t);
    }
}

} // namespace gfx::ui

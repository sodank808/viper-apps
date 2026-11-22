#pragma once

// A drawing surface over any RGB565 buffer: the back buffer a VideoBlit app
// gets each frame (viper_frame_begin() in an ELF app, System::back_buffer()
// in a built-in one) or an off-screen one. Every call keeps to `clip`.
//
//     gfx::Surface s(pixels, width, height);
//     s.fill({10, 10, 40, 20}, gfx::rgb(255, 128, 0));
//     s.text(gfx::kFont5x7, "SCORE 120", 12, 14, gfx::rgb(255, 255, 255), 2);
//
// The surface doesn't own the pixels or present them; the app does both.
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <algorithm>
#include <cstdlib>
#include <string_view>

#include "gfx/color.hpp"
#include "gfx/font.hpp"
#include "gfx/rect.hpp"

namespace gfx {

struct Surface {
    Pixel *px = nullptr; // the pixel at (0, 0)
    int w = 0, h = 0;    // the layout's size: bounds(), clear(), text_centered()
    int stride = 0;      // pixels from one row to the next
    Rect area;           // where px may be written: bounds(), or the whole buffer for centered()
    Rect clip;           // drawing stays inside this; set_clip(), then reset_clip()

    Surface() = default;
    Surface(Pixel *pixels, int width, int height)
        : Surface(pixels, width, height, Rect{0, 0, width, height}) {}
    Surface(Pixel *pixels, int width, int height, Rect clip_to)
        : px(pixels)
        , w(width)
        , h(height)
        , stride(width)
        , area{0, 0, width, height}
        , clip(clip_to.intersect(area)) {}

    // A layout_w x layout_h layout centered on this surface (a 320x240 game on
    // a bigger panel): (0, 0) is the layout's top left. It can still draw into
    // the margins around it, and clear() fills them too.
    Surface centered(int layout_w, int layout_h) const {
        Surface s = translated((w - layout_w) / 2, (h - layout_h) / 2);
        s.w = layout_w;
        s.h = layout_h;
        return s;
    }

    // The same pixels with (0, 0) moved to (dx, dy): a screen shake, or a
    // panel drawn in its own coordinates. It can still reach the whole buffer.
    Surface translated(int dx, int dy) const {
        Surface s = *this;
        s.px = px + dy * stride + dx;
        s.area = Rect{area.x - dx, area.y - dy, area.w, area.h};
        s.clip = Rect{clip.x - dx, clip.y - dy, clip.w, clip.h};
        return s;
    }

    void set_clip(Rect r) { clip = r.intersect(area); }
    void reset_clip() { clip = area; }
    Rect bounds() const { return Rect{0, 0, w, h}; }
    Pixel *row(int y) { return px + y * stride; }

    // Single pixels
    void set(int x, int y, Pixel c) {
        if (clip.contains(x, y)) {
            px[y * stride + x] = c;
        }
    }
    void add(int x, int y, Pixel c) {
        if (clip.contains(x, y)) {
            Pixel &d = px[y * stride + x];
            d = gfx::add(d, c);
        }
    }
    void blend(int x, int y, Pixel c, uint32_t a32) {
        if (clip.contains(x, y)) {
            Pixel &d = px[y * stride + x];
            d = gfx::blend(d, c, a32);
        }
    }

    // The whole surface, margins included
    void clear(Pixel c) { fill(area, c); }

    void fill(Rect r, Pixel c) {
        r = r.intersect(clip);
        for (int y = r.y; y < r.bottom(); y++) {
            std::fill(row(y) + r.x, row(y) + r.right(), c);
        }
    }

    // Toward c by a32 / 32 (gfx::blend)
    void fill_blend(Rect r, Pixel c, uint32_t a32) {
        r = r.intersect(clip);
        for (int y = r.y; y < r.bottom(); y++) {
            Pixel *p = row(y);
            for (int x = r.x; x < r.right(); x++) {
                p[x] = gfx::blend(p[x], c, a32);
            }
        }
    }

    // Keep keep32 / 32 of what's there: a dim overlay behind a menu or message
    void darken(Rect r, uint32_t keep32) {
        r = r.intersect(clip);
        for (int y = r.y; y < r.bottom(); y++) {
            Pixel *p = row(y);
            for (int x = r.x; x < r.right(); x++) {
                p[x] = gfx::blend(0, p[x], keep32);
            }
        }
    }

    // An outline `thickness` pixels wide, inside r
    void frame(Rect r, Pixel c, int thickness = 1) {
        const int t = std::min({thickness, r.w / 2 + 1, r.h / 2 + 1});
        fill(Rect{r.x, r.y, r.w, t}, c);
        fill(Rect{r.x, r.bottom() - t, r.w, t}, c);
        fill(Rect{r.x, r.y + t, t, r.h - 2 * t}, c);
        fill(Rect{r.right() - t, r.y + t, t, r.h - 2 * t}, c);
    }

    void hline(int x0, int x1, int y, Pixel c) {
        fill(Rect{std::min(x0, x1), y, std::abs(x1 - x0) + 1, 1}, c);
    }
    void vline(int x, int y0, int y1, Pixel c) {
        fill(Rect{x, std::min(y0, y1), 1, std::abs(y1 - y0) + 1}, c);
    }

    // Text with its top left at (x, y), each font pixel drawn scale x scale.
    // Returns the x after the last character.
    int text(const Font &font, std::string_view s, int x, int y, Pixel c, int scale = 1) {
        for (char ch : s) {
            const uint8_t *rows = font.rows(ch);
            if (rows) {
                for (int gy = 0; gy < font.height; gy++) {
                    for (int gx = 0; gx < font.width; gx++) {
                        if ((rows[gy] >> (font.width - 1 - gx)) & 1) {
                            fill(Rect{x + gx * scale, y + gy * scale, scale, scale}, c);
                        }
                    }
                }
            }
            x += advance(font, ch) * scale;
        }
        return x;
    }

    // Text centered across the surface
    void text_centered(const Font &font, std::string_view s, int y, Pixel c, int scale = 1);
};

// The width text draws at: from the first character's left edge to the last
// one's right edge, without the space after it
inline int text_width(const Font &font, std::string_view s, int scale = 1) {
    int w = 0;
    for (char c : s) {
        w += advance(font, c);
    }
    return s.empty() ? 0 : (w - (font.advance - font.width)) * scale;
}

inline void Surface::text_centered(const Font &font, std::string_view s, int y, Pixel c,
                                   int scale) {
    text(font, s, (w - text_width(font, s, scale)) / 2, y, c, scale);
}

} // namespace gfx

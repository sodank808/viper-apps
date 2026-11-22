#pragma once

// Shapes on a gfx::Surface: discs, rings, ellipses, lines at any angle,
// polygons and rounded rectangles.
//
//     gfx::disc(s, 40.5f, 30.5f, 12, gfx::rgb(250, 220, 60));
//     gfx::line(s, 10, 10, 90, 40, gfx::rgb(255, 255, 255), 3);
//     const gfx::Vec2 tri[] = {{50, 10}, {60, 28}, {40, 28}};
//     gfx::poly(s, tri, 3, gfx::rgb(230, 50, 50));
//
// Coordinates are floats, with a pixel's center at +0.5: a disc centered on
// (10.5, 10.5) is centered on pixel (10, 10), except pixel_line(), which
// steps along whole pixels. disc(), ring() and line() smooth
// their edges by blending the pixels they partly cover; the rest have hard
// edges. Every shape takes an alpha, a32 / 32 (gfx::blend), and keeps to the
// surface's clip.
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "gfx/surface.hpp"

namespace gfx {

struct Vec2 {
    float x = 0, y = 0;
};

namespace shapes_detail {

// One pixel, covered `coverage` (0..1) of the way, at alpha a32
inline void plot(Surface &s, int x, int y, Pixel c, uint32_t a32, float coverage) {
    const uint32_t a = static_cast<uint32_t>(static_cast<float>(a32) * coverage + 0.5f);
    if (a >= 32) {
        s.set(x, y, c);
    } else if (a > 0) {
        s.blend(x, y, c, a);
    }
}

// Polygons take gfx::Vec2 points or float[2] pairs
inline float x_of(const Vec2 &p) { return p.x; }
inline float y_of(const Vec2 &p) { return p.y; }
inline float x_of(const float (&p)[2]) { return p[0]; }
inline float y_of(const float (&p)[2]) { return p[1]; }

} // namespace shapes_detail

// The pixels of row y whose centers are between x0 and x1
inline void span(Surface &s, int y, float x0, float x1, Pixel c, uint32_t a32 = 32) {
    const int a = static_cast<int>(std::ceil(x0 - 0.5f));
    const int b = static_cast<int>(std::floor(x1 - 0.5f));
    if (b < a) {
        return;
    }
    if (a32 >= 32) {
        s.fill(Rect{a, y, b - a + 1, 1}, c);
    } else {
        s.fill_blend(Rect{a, y, b - a + 1, 1}, c, a32);
    }
}

// A ring between radii r_in and r_out, smoothed on both edges. r_in 0 is a disc.
inline void ring(Surface &s, float cx, float cy, float r_in, float r_out, Pixel c,
                 uint32_t a32 = 32) {
    const int y0 = static_cast<int>(std::floor(cy - r_out - 1));
    const int y1 = static_cast<int>(std::ceil(cy + r_out + 1));
    const float outer = r_out + 0.5f;
    for (int y = std::max(y0, s.clip.y); y <= std::min(y1, s.clip.bottom() - 1); ++y) {
        const float dy = y + 0.5f - cy;
        if (std::fabs(dy) > r_out + 1.f) {
            continue;
        }
        const float reach = std::sqrt(std::max(0.f, outer * outer - dy * dy));
        const int x0 = static_cast<int>(std::floor(cx - reach - 1));
        const int x1 = static_cast<int>(std::ceil(cx + reach + 1));
        for (int x = x0; x <= x1; ++x) {
            const float dx = x + 0.5f - cx;
            const float d = std::sqrt(dx * dx + dy * dy);
            float cover = std::clamp(outer - d, 0.f, 1.f);
            if (r_in > 0.f) {
                cover = std::min(cover, std::clamp(d - r_in + 0.5f, 0.f, 1.f));
            }
            if (cover > 0.f) {
                shapes_detail::plot(s, x, y, c, a32, cover);
            }
        }
    }
}

// A filled circle, smoothed on its edge
inline void disc(Surface &s, float cx, float cy, float r, Pixel c, uint32_t a32 = 32) {
    ring(s, cx, cy, 0.f, r, c, a32);
}

// A filled ellipse with hard edges. ellipse(s, cx, cy, r, r, c) is a hard-edged disc.
inline void ellipse(Surface &s, float cx, float cy, float rx, float ry, Pixel c,
                    uint32_t a32 = 32) {
    if (rx <= 0.f || ry <= 0.f) {
        return;
    }
    const int y0 = static_cast<int>(std::floor(cy - ry));
    const int y1 = static_cast<int>(std::ceil(cy + ry));
    for (int y = y0; y <= y1; ++y) {
        const float dy = (y + 0.5f - cy) / ry;
        if (dy * dy >= 1.f) {
            continue;
        }
        const float hw = rx * std::sqrt(1.f - dy * dy);
        span(s, y, cx - hw, cx + hw, c, a32);
    }
}

// A line `width` pixels wide with round ends, smoothed on its edges
inline void line(Surface &s, float x0, float y0, float x1, float y1, Pixel c, float width = 1.f,
                 uint32_t a32 = 32) {
    const float hw = width * 0.5f;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = dx * dx + dy * dy;
    const float len = std::sqrt(len2);
    const int ry0 = static_cast<int>(std::floor(std::min(y0, y1) - hw - 1));
    const int ry1 = static_cast<int>(std::ceil(std::max(y0, y1) + hw + 1));
    const int bx0 = static_cast<int>(std::floor(std::min(x0, x1) - hw - 1));
    const int bx1 = static_cast<int>(std::ceil(std::max(x0, x1) + hw + 1));
    for (int y = std::max(ry0, s.clip.y); y <= std::min(ry1, s.clip.bottom() - 1); ++y) {
        const float py = y + 0.5f;
        int xa = bx0, xb = bx1;
        if (std::fabs(dy) > 0.5f) {
            // Only the pixels near where the line crosses this row
            const float xc = x0 + (py - y0) / dy * dx;
            const float reach = (hw + 1.5f) * len / std::fabs(dy);
            xa = std::max(bx0, static_cast<int>(std::floor(xc - reach)));
            xb = std::min(bx1, static_cast<int>(std::ceil(xc + reach)));
        }
        for (int x = xa; x <= xb; ++x) {
            const float px = x + 0.5f;
            float t = len2 > 0.f ? ((px - x0) * dx + (py - y0) * dy) / len2 : 0.f;
            t = std::clamp(t, 0.f, 1.f);
            const float ex = px - (x0 + t * dx), ey = py - (y0 + t * dy);
            const float cover = std::clamp(hw + 0.5f - std::sqrt(ex * ex + ey * ey), 0.f, 1.f);
            if (cover > 0.f) {
                shapes_detail::plot(s, x, y, c, a32, cover);
            }
        }
    }
}

// A line along whole pixels (Bresenham), each step a width x width square:
// hard edges, the look of pixel art. line() is the smooth one.
inline void pixel_line(Surface &s, int x0, int y0, int x1, int y1, Pixel c, int width = 1,
                       uint32_t a32 = 32) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    const int h = width >> 1;
    int err = dx + dy;
    for (;;) {
        const Rect r{x0 - h, y0 - h, width, width};
        if (a32 >= 32) {
            s.fill(r, c);
        } else {
            s.fill_blend(r, c, a32);
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// A filled polygon, any simple one (even-odd rule), with hard edges. Up to 24
// edges may cross any one row. Points are gfx::Vec2 or float[2] pairs.
template <typename Point>
void poly(Surface &s, const Point *pts, int n, Pixel c, uint32_t a32 = 32) {
    using shapes_detail::x_of;
    using shapes_detail::y_of;
    if (n < 3) {
        return;
    }
    float ymin = y_of(pts[0]), ymax = y_of(pts[0]);
    for (int i = 1; i < n; ++i) {
        ymin = std::min(ymin, y_of(pts[i]));
        ymax = std::max(ymax, y_of(pts[i]));
    }
    constexpr int kMaxCrossings = 24;
    float xs[kMaxCrossings];
    const int y0 = std::max(static_cast<int>(std::floor(ymin)), s.clip.y);
    const int y1 = std::min(static_cast<int>(std::ceil(ymax)), s.clip.bottom() - 1);
    for (int y = y0; y <= y1; ++y) {
        const float py = y + 0.5f;
        int count = 0;
        for (int i = 0, j = n - 1; i < n && count < kMaxCrossings; j = i++) {
            const float yi = y_of(pts[i]), yj = y_of(pts[j]);
            if ((yi > py) != (yj > py)) {
                xs[count++] = x_of(pts[i]) + (py - yi) * (x_of(pts[j]) - x_of(pts[i])) / (yj - yi);
            }
        }
        std::sort(xs, xs + count);
        for (int k = 0; k + 1 < count; k += 2) {
            span(s, y, xs[k], xs[k + 1], c, a32);
        }
    }
}

// A polygon's outline, closed, as smoothed lines
template <typename Point>
void poly_outline(Surface &s, const Point *pts, int n, Pixel c, float width = 1.f,
                  uint32_t a32 = 32) {
    using shapes_detail::x_of;
    using shapes_detail::y_of;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        line(s, x_of(pts[j]), y_of(pts[j]), x_of(pts[i]), y_of(pts[i]), c, width, a32);
    }
}

// A filled rectangle with corners rounded to `radius`, hard-edged
inline void round_rect(Surface &s, Rect r, int radius, Pixel c, uint32_t a32 = 32) {
    radius = std::min({radius, r.w / 2, r.h / 2});
    for (int row = 0; row < r.h; ++row) {
        int inset = 0;
        const int dy = row < radius          ? radius - row
                       : row >= r.h - radius ? row - (r.h - radius - 1)
                                             : 0;
        if (dy > 0) {
            const float dx =
                std::sqrt(std::max(0.f, static_cast<float>(radius * radius - dy * dy)));
            inset = radius - static_cast<int>(dx + 0.5f);
        }
        const Rect line_r{r.x + inset, r.y + row, r.w - 2 * inset, 1};
        if (a32 >= 32) {
            s.fill(line_r, c);
        } else {
            s.fill_blend(line_r, c, a32);
        }
    }
}

} // namespace gfx

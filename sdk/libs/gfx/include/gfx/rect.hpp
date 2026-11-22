#pragma once

// Integer rectangles, for drawing, clipping and dirty areas. Part of gfx's
// platform-free drawing library; see components/gfx/README.md.

#include <algorithm>

namespace gfx {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;

    int right() const { return x + w; }
    int bottom() const { return y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    // n pixels bigger on every side (smaller for a negative n)
    Rect grow(int n) const { return Rect{x - n, y - n, w + 2 * n, h + 2 * n}; }
    // The part inside o; empty (w or h 0) when they don't overlap
    Rect intersect(const Rect &o) const {
        const int l = std::max(x, o.x), t = std::max(y, o.y);
        const int r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return Rect{l, t, std::max(0, r - l), std::max(0, b - t)};
    }
    bool overlaps(const Rect &o) const { return !intersect(o).empty(); }
    bool operator==(const Rect &o) const = default;
};

} // namespace gfx

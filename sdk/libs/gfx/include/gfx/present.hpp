#pragma once

// Fitting a game's own screen to the panel.
//
// fit_integer: the biggest whole-number scale a layout (usually 320x240) fits
// the panel at, and where it goes to be centered.
//
//     const gfx::Fit fit = gfx::fit_integer(320, 240, screen_w, screen_h);
//     scale_ = fit.scale;
//
// MonoPresenter: copies a black and white canvas (one byte per pixel, nonzero
// is black) to the panel, centered on black: 1:1 when it fits, scaled by
// nearest neighbor when the panel is at least 1.5 times bigger, and averaged
// to gray when the panel is smaller. MacCanvas presents through it.
//
//     gfx::MonoPresenter presenter_;
//     presenter_.present(ink.data(), 320, 240, s);
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "gfx/color.hpp"
#include "gfx/rect.hpp"
#include "gfx/surface.hpp"

namespace gfx {

struct Fit {
    int scale = 1;    // at least 1, even when the layout is bigger than the panel
    int x = 0, y = 0; // the scaled layout's top left on the panel
};

inline Fit fit_integer(int src_w, int src_h, int dst_w, int dst_h) {
    const int scale = std::max(1, std::min(dst_w / src_w, dst_h / src_h));
    return {scale, (dst_w - src_w * scale) / 2, (dst_h - src_h * scale) / 2};
}

class MonoPresenter {
  public:
    // Draws the w x h canvas `ink` over all of `dst` (its w x h, inside its
    // clip): the canvas centered, black around it.
    void present(const uint8_t *ink, int w, int h, Surface &dst) {
        if (w != src_w_ || h != src_h_ || dst.w != dst_w_ || dst.h != dst_h_) {
            setup(w, h, dst.w, dst.h);
        }
        constexpr Pixel kBlack = 0x0000, kWhite = 0xFFFF;
        const Rect o = out_;
        dst.fill({0, 0, dst.w, o.y}, kBlack);
        dst.fill({0, o.bottom(), dst.w, dst.h - o.bottom()}, kBlack);
        dst.fill({0, o.y, o.x, o.h}, kBlack);
        dst.fill({o.right(), o.y, dst.w - o.right(), o.h}, kBlack);

        const Rect r = o.intersect(dst.clip);
        for (int sy = r.y; sy < r.bottom(); ++sy) {
            const int oy = sy - o.y;
            Pixel *out = dst.px + sy * dst.stride;
            if (scale_ >= 1.0f) { // nearest neighbor
                const uint8_t *row = ink + src_y_[oy] * w;
                for (int sx = r.x; sx < r.right(); ++sx) {
                    out[sx] = row[src_x_[sx - o.x]] ? kBlack : kWhite;
                }
            } else { // a smaller panel: average the canvas pixels each one covers
                const int y0 = src_y_[oy], y1 = std::max(y0 + 1, src_y_[oy + 1]);
                for (int sx = r.x; sx < r.right(); ++sx) {
                    const int ox = sx - o.x;
                    const int x0 = src_x_[ox], x1 = std::max(x0 + 1, src_x_[ox + 1]);
                    int black = 0;
                    for (int y = y0; y < y1; ++y) {
                        for (int x = x0; x < x1; ++x) {
                            black += ink[y * w + x] ? 1 : 0;
                        }
                    }
                    const int g = 255 - black * 255 / ((y1 - y0) * (x1 - x0));
                    out[sx] = rgb(g, g, g);
                }
            }
        }
    }

    // Where the last present() put the canvas, in the surface's coordinates
    Rect out() const { return out_; }
    // Panel pixels per canvas pixel
    float scale() const { return scale_; }

  private:
    void setup(int w, int h, int dst_w, int dst_h) {
        src_w_ = w;
        src_h_ = h;
        dst_w_ = dst_w;
        dst_h_ = dst_h;
        const float fit = std::min(static_cast<float>(dst_w) / w, static_cast<float>(dst_h) / h);
        // 1:1 when it fits (crisp), bigger only when there's room for it
        scale_ = fit >= 1.5f ? fit : fit >= 1.0f ? 1.0f : fit;
        const int out_w = std::min(dst_w, static_cast<int>(w * scale_));
        const int out_h = std::min(dst_h, static_cast<int>(h * scale_));
        out_ = {(dst_w - out_w) / 2, (dst_h - out_h) / 2, out_w, out_h};
        src_x_.resize(out_w + 1);
        src_y_.resize(out_h + 1);
        for (int x = 0; x <= out_w; ++x) {
            src_x_[x] = std::min(w, static_cast<int>(x / scale_));
        }
        for (int y = 0; y <= out_h; ++y) {
            src_y_[y] = std::min(h, static_cast<int>(y / scale_));
        }
    }

    int src_w_ = 0, src_h_ = 0, dst_w_ = 0, dst_h_ = 0;
    float scale_ = 1;
    Rect out_;
    std::vector<int> src_x_, src_y_; // the canvas pixel each panel column and row starts at
};

} // namespace gfx

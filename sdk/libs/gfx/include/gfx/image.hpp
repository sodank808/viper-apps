#pragma once

// Images and drawing them: sprites, tiles, atlas cells.
//
//     static const uint8_t kShip[8 * 8] = {...};         // palette indices
//     static const gfx::Pixel kShipColors[] = {0, gfx::rgb(255, 255, 255), ...};
//     const auto ship = gfx::Image::indexed(kShip, kShipColors, 8, 8);
//     gfx::blit(s, ship, x, y);                         // 1:1
//     gfx::blit(s, ship, x, y, gfx::Flip::H, 3);        // mirrored, 3x
//     gfx::blit_tinted(s, ship, x + 1, y + 1, gfx::rgb(0, 0, 0), 12); // a shadow
//     gfx::blit_transformed(s, ship, {.x = cx, .y = cy, .ax = 4, .ay = 4, .angle = a},
//                           gfx::Sampling::Smooth);
//
// An Image doesn't own its pixels: they're usually const tables in flash, or
// a buffer the app keeps. Every draw keeps to the surface's clip.
//
// Part of gfx's platform-free drawing library; see components/gfx/README.md.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "gfx/surface.hpp"

namespace gfx {

struct Image {
    enum class Format : uint8_t {
        RGB565,    // data: a Pixel each, opaque
        RGB565_A8, // data: a Pixel each; alpha: a byte each, 0 clear to 255 opaque
        A8,        // data: a byte of coverage each, a shape for blit_tinted (white in blit)
        I8,        // data: a byte each indexing palette; index 0 is clear
    };

    Format format = Format::RGB565;
    int w = 0, h = 0;
    int stride = 0; // pixels from one row to the next, in data and alpha
    const void *data = nullptr;
    const uint8_t *alpha = nullptr; // RGB565_A8
    const Pixel *palette = nullptr; // I8

    static constexpr Image rgb565(const Pixel *px, int w, int h, int stride = 0) {
        return Image{Format::RGB565, w, h, stride ? stride : w, px, nullptr, nullptr};
    }
    static constexpr Image rgb565_a8(const Pixel *px, const uint8_t *alpha, int w, int h,
                                     int stride = 0) {
        return Image{Format::RGB565_A8, w, h, stride ? stride : w, px, alpha, nullptr};
    }
    static constexpr Image a8(const uint8_t *coverage, int w, int h, int stride = 0) {
        return Image{Format::A8, w, h, stride ? stride : w, coverage, nullptr, nullptr};
    }
    static constexpr Image indexed(const uint8_t *indices, const Pixel *palette, int w, int h,
                                   int stride = 0) {
        return Image{Format::I8, w, h, stride ? stride : w, indices, nullptr, palette};
    }

    // The same indices in other colors: an I8 image's palette swap
    Image with_palette(const Pixel *p) const {
        Image i = *this;
        i.palette = p;
        return i;
    }

    // The pixel at (x, y), which must be inside: its color, and its alpha
    // (0 clear to 255 opaque) returned
    uint8_t at(int x, int y, Pixel &c) const {
        const int i = y * stride + x;
        switch (format) {
        case Format::RGB565:
            c = static_cast<const Pixel *>(data)[i];
            return 255;
        case Format::RGB565_A8:
            c = static_cast<const Pixel *>(data)[i];
            return alpha[i];
        case Format::A8:
            c = 0xFFFF;
            return static_cast<const uint8_t *>(data)[i];
        case Format::I8: {
            const uint8_t k = static_cast<const uint8_t *>(data)[i];
            if (k == 0) {
                return 0;
            }
            c = palette[k];
            return 255;
        }
        }
        return 0;
    }
};

// Part of an image (an atlas cell, a frame of a strip), clipped to it. It
// shares the image's pixels.
inline Image sub(const Image &img, Rect r) {
    r = r.intersect(Rect{0, 0, img.w, img.h});
    Image s = img;
    s.w = r.w;
    s.h = r.h;
    const int offset = r.y * img.stride + r.x;
    const bool wide = img.format == Image::Format::RGB565 || img.format == Image::Format::RGB565_A8;
    s.data = wide ? static_cast<const void *>(static_cast<const Pixel *>(img.data) + offset)
                  : static_cast<const void *>(static_cast<const uint8_t *>(img.data) + offset);
    if (img.alpha) {
        s.alpha = img.alpha + offset;
    }
    return s;
}

enum class Flip : uint8_t { None = 0, H = 1, V = 2, HV = 3 };

namespace image_detail {

// Image alpha (0..255) times a32 (0..32), as gfx::blend's 0..32
inline uint32_t mix(uint32_t a255, uint32_t a32) { return (a255 * a32 + 127) / 255; }

inline void put(Pixel &d, Pixel c, uint32_t a) {
    if (a >= 32) {
        d = c;
    } else if (a > 0) {
        d = gfx::blend(d, c, a);
    }
}

// Draws img's (ix, iy) pixels, for every pixel of `dst`, through to_image
// (which maps a surface x or y to the image's), as `tint` if one is given
template <typename MapX, typename MapY>
void draw(Surface &s, const Image &img, Rect dst, MapX to_ix, MapY to_iy, const Pixel *tint,
          uint32_t a32) {
    dst = dst.intersect(s.clip);
    // The common cases, opaque and untinted, without a switch per pixel
    if (!tint && a32 >= 32 && img.format == Image::Format::I8) {
        const auto *idx = static_cast<const uint8_t *>(img.data);
        for (int y = dst.y; y < dst.bottom(); y++) {
            const uint8_t *src = idx + to_iy(y) * img.stride;
            Pixel *row = s.row(y);
            for (int x = dst.x; x < dst.right(); x++) {
                if (const uint8_t k = src[to_ix(x)]) {
                    row[x] = img.palette[k];
                }
            }
        }
        return;
    }
    if (!tint && a32 >= 32 && img.format == Image::Format::RGB565) {
        const auto *px = static_cast<const Pixel *>(img.data);
        for (int y = dst.y; y < dst.bottom(); y++) {
            const Pixel *src = px + to_iy(y) * img.stride;
            Pixel *row = s.row(y);
            for (int x = dst.x; x < dst.right(); x++) {
                row[x] = src[to_ix(x)];
            }
        }
        return;
    }
    for (int y = dst.y; y < dst.bottom(); y++) {
        const int iy = to_iy(y);
        Pixel *row = s.row(y);
        for (int x = dst.x; x < dst.right(); x++) {
            Pixel c;
            const uint8_t a = img.at(to_ix(x), iy, c);
            if (a) {
                put(row[x], tint ? *tint : c, mix(a, a32));
            }
        }
    }
}

template <typename Fn>
void blit_scaled(Surface &s, const Image &img, int x, int y, Flip flip, int scale, Fn &&draw_fn) {
    if (scale < 1 || img.w <= 0 || img.h <= 0) {
        return;
    }
    const bool fh = static_cast<uint8_t>(flip) & 1, fv = static_cast<uint8_t>(flip) & 2;
    const Rect dst{x, y, img.w * scale, img.h * scale};
    auto to_ix = [=](int sx) {
        const int i = (sx - x) / scale;
        return fh ? img.w - 1 - i : i;
    };
    auto to_iy = [=](int sy) {
        const int i = (sy - y) / scale;
        return fv ? img.h - 1 - i : i;
    };
    draw_fn(dst, to_ix, to_iy);
}

} // namespace image_detail

// The image with its top left at (x, y), flipped, each pixel drawn
// scale x scale. Clear pixels (alpha 0, I8 index 0) are skipped and partly
// clear ones blended.
inline void blit(Surface &s, const Image &img, int x, int y, Flip flip = Flip::None,
                 int scale = 1) {
    image_detail::blit_scaled(s, img, x, y, flip, scale, [&](Rect dst, auto ix, auto iy) {
        image_detail::draw(s, img, dst, ix, iy, nullptr, 32);
    });
}

// The image's shape in one color, at a32 / 32: a shadow, a silhouette, a
// flash on a hit, or an A8 glyph or mask
inline void blit_tinted(Surface &s, const Image &img, int x, int y, Pixel tint, uint32_t a32 = 32,
                        Flip flip = Flip::None, int scale = 1) {
    image_detail::blit_scaled(s, img, x, y, flip, scale, [&](Rect dst, auto ix, auto iy) {
        image_detail::draw(s, img, dst, ix, iy, &tint, a32);
    });
}

// Where blit_transformed() puts an image: its point (ax, ay) lands on the
// surface's (x, y), and it's scaled by (sx, sy) in its own frame and turned
// `angle` radians around that point (clockwise, as y points down).
struct Transform {
    float x = 0, y = 0;
    float ax = 0, ay = 0;
    float angle = 0;
    float sx = 1, sy = 1;
};

enum class Sampling : uint8_t {
    Nearest, // crisp: the image pixel each surface pixel lands in
    Smooth,  // the four nearest image pixels blended, edges smoothed
};

namespace image_detail {

inline void transformed(Surface &s, const Image &img, const Transform &t, Sampling sampling,
                        const Pixel *tint, uint32_t a32) {
    if (img.w <= 0 || img.h <= 0 || t.sx == 0.f || t.sy == 0.f) {
        return;
    }
    const float ca = std::cos(t.angle), sa = std::sin(t.angle);
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (float u : {0.f, static_cast<float>(img.w)}) {
        for (float v : {0.f, static_cast<float>(img.h)}) {
            const float lx = (u - t.ax) * t.sx, ly = (v - t.ay) * t.sy;
            const float px = t.x + lx * ca - ly * sa, py = t.y + lx * sa + ly * ca;
            minx = std::min(minx, px);
            maxx = std::max(maxx, px);
            miny = std::min(miny, py);
            maxy = std::max(maxy, py);
        }
    }
    const int x0 = std::max(static_cast<int>(std::floor(minx)) - 1, s.clip.x);
    const int x1 = std::min(static_cast<int>(std::ceil(maxx)), s.clip.right() - 1);
    const int y0 = std::max(static_cast<int>(std::floor(miny)) - 1, s.clip.y);
    const int y1 = std::min(static_cast<int>(std::ceil(maxy)), s.clip.bottom() - 1);
    const float isx = 1.f / t.sx, isy = 1.f / t.sy;
    const float fw = static_cast<float>(img.w), fh = static_cast<float>(img.h);
    for (int y = y0; y <= y1; y++) {
        Pixel *row = s.row(y);
        const float qy = y + 0.5f - t.y;
        for (int x = x0; x <= x1; x++) {
            // Each surface pixel's center, back into the image
            const float qx = x + 0.5f - t.x;
            const float u = (ca * qx + sa * qy) * isx + t.ax;
            const float v = (-sa * qx + ca * qy) * isy + t.ay;
            if (u < -1.f || v < -1.f || u > fw + 1.f || v > fh + 1.f) {
                continue;
            }
            if (sampling == Sampling::Nearest) {
                const int i = static_cast<int>(std::floor(u)), j = static_cast<int>(std::floor(v));
                if (i < 0 || j < 0 || i >= img.w || j >= img.h) {
                    continue;
                }
                Pixel c;
                if (const uint8_t a = img.at(i, j, c)) {
                    put(row[x], tint ? *tint : c, mix(a, a32));
                }
                continue;
            }
            // Smooth: the four pixels around (u, v), weighted by how near
            // they are and by their alpha
            const float fu = u - 0.5f, fv = v - 0.5f;
            const int i0 = static_cast<int>(std::floor(fu)), j0 = static_cast<int>(std::floor(fv));
            const float wx = fu - i0, wy = fv - j0;
            const float w[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
            float r = 0, g = 0, b = 0, a = 0;
            for (int k = 0; k < 4; k++) {
                const int i = i0 + (k & 1), j = j0 + (k >> 1);
                if (i < 0 || j < 0 || i >= img.w || j >= img.h) {
                    continue;
                }
                Pixel c;
                const uint8_t pa = img.at(i, j, c);
                if (!pa) {
                    continue;
                }
                const float wa = w[k] * pa * (1.f / 255.f);
                const Rgb rgb8 = unpack(c);
                r += rgb8.r * wa;
                g += rgb8.g * wa;
                b += rgb8.b * wa;
                a += wa;
            }
            if (a < 0.02f) {
                continue;
            }
            const Pixel c = tint ? *tint
                                 : rgb(static_cast<int>(r / a), static_cast<int>(g / a),
                                       static_cast<int>(b / a));
            put(row[x], c, static_cast<uint32_t>(std::min(a, 1.f) * a32 + 0.5f));
        }
    }
}

} // namespace image_detail

// The image scaled and turned (see Transform), sampled crisp or smooth
inline void blit_transformed(Surface &s, const Image &img, const Transform &t,
                             Sampling sampling = Sampling::Nearest, uint32_t a32 = 32) {
    image_detail::transformed(s, img, t, sampling, nullptr, a32);
}

// The same in one color: a silhouette or an outline drawn under it
inline void blit_transformed_tinted(Surface &s, const Image &img, const Transform &t, Pixel tint,
                                    Sampling sampling = Sampling::Nearest, uint32_t a32 = 32) {
    image_detail::transformed(s, img, t, sampling, &tint, a32);
}

} // namespace gfx

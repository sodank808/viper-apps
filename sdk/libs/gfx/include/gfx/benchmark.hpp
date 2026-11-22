#pragma once

// The cases the GFX Bench app (apps/gfx_bench) times: fills, text, shapes,
// blits and a 1-bit present, each run over and over into a frame. The app runs
// them twice, once compiled into itself (an ELF app, running from PSRAM) and
// once compiled into the firmware (viper_gfx_bench(), a test-only export), to
// see whether drawing from app code is much slower. That decides whether a few
// hot routines should be exported from the firmware instead
// (docs/plans/game_api_consolidation.md, Phase 2).
//
//     gfx::bench::Result r;
//     for (int i = 0; gfx::bench::run_case(i, pixels, w, h, now_us, r); i++) { ... }
//
// Each case draws into the w x h frame at `pixels` (what it leaves there is
// noise). The images they draw are made on the heap first, outside the
// timing. Not part of the drawing API; part of gfx so both sides build the
// same code.

#include <cstdint>
#include <vector>

#include "gfx/color.hpp"
#include "gfx/font.hpp"
#include "gfx/image.hpp"
#include "gfx/present.hpp"
#include "gfx/shapes.hpp"
#include "gfx/surface.hpp"

namespace gfx::bench {

struct Result {
    const char *name; // what was drawn, e.g. "fill 16x16"
    uint32_t ops;     // how many times
    uint32_t us;      // in all, in microseconds
};

inline constexpr int kCases = 10;

namespace detail {

constexpr int kSprite = 32;
constexpr int kMonoW = 320, kMonoH = 240;

// What the cases draw from: on the heap, so the firmware's copy of this code
// doesn't keep them in its RAM, and both copies get the same kind of memory
struct Data {
    std::vector<Pixel> rgb565 = std::vector<Pixel>(kSprite * kSprite);
    std::vector<uint8_t> indexed = std::vector<uint8_t>(kSprite * kSprite);
    std::vector<Pixel> palette = std::vector<Pixel>(16);
    std::vector<uint8_t> mono = std::vector<uint8_t>(kMonoW * kMonoH);

    Data() {
        for (int y = 0; y < kSprite; y++) {
            for (int x = 0; x < kSprite; x++) {
                rgb565[y * kSprite + x] = rgb(x * 8, y * 8, (x ^ y) * 8);
                const int dx = x - kSprite / 2, dy = y - kSprite / 2;
                // A disc of stripes, clear around it (index 0)
                indexed[y * kSprite + x] =
                    dx * dx + dy * dy < 15 * 15 ? static_cast<uint8_t>(1 + (x + y) % 15) : 0;
            }
        }
        for (int i = 0; i < 16; i++) {
            palette[i] = rgb(i * 16, 255 - i * 16, 128);
        }
        for (int i = 0; i < kMonoW * kMonoH; i++) {
            mono[i] = ((i / kMonoW) ^ (i % kMonoW)) & 4 ? 1 : 0;
        }
    }
};

} // namespace detail

// Runs case `index` (0 to kCases - 1) on the w x h frame at `pixels`, timing
// it with now_us(), and returns whether there was such a case. One case at a
// time, so an app can show progress between them.
inline bool run_case(int index, Pixel *pixels, int w, int h, uint64_t (*now_us)(), Result &out) {
    using namespace detail;
    const Data data;
    Surface s(pixels, w, h);
    const Image sprite = Image::rgb565(data.rgb565.data(), kSprite, kSprite);
    const Image indexed =
        Image::indexed(data.indexed.data(), data.palette.data(), kSprite, kSprite);
    MonoPresenter presenter;
    int k = 0;
    bool ran = false;
    // Case k: runs body(i) ops times, once more before timing to warm the cache
    auto time = [&](const char *name, uint32_t ops, auto &&body) {
        if (k++ != index) {
            return;
        }
        body(0u);
        const uint64_t t0 = now_us();
        for (uint32_t i = 0; i < ops; i++) {
            body(i);
        }
        out = Result{name, ops, static_cast<uint32_t>(now_us() - t0)};
        ran = true;
    };
    auto x_of = [&](uint32_t i, int size) { return static_cast<int>(i * 37 % (w - size)); };
    auto y_of = [&](uint32_t i, int size) { return static_cast<int>(i * 53 % (h - size)); };

    time("clear frame", 20, [&](uint32_t i) { s.clear(rgb(i * 10, 0, 0)); });
    time("fill 16x16", 2000, [&](uint32_t i) {
        s.fill({x_of(i, 16), y_of(i, 16), 16, 16}, static_cast<Pixel>(i * 2654435761u));
    });
    time("blend frame", 10, [&](uint32_t) { s.fill_blend(s.bounds(), rgb(0, 0, 255), 16); });
    time("text 5x7, 30", 200, [&](uint32_t i) {
        s.text(kFont5x7, "THE QUICK BROWN FOX 0123456789", x_of(i, 180), y_of(i, 8),
               rgb(255, 255, 255));
    });
    time("disc r12", 500, [&](uint32_t i) {
        disc(s, static_cast<float>(x_of(i, 30) + 15), static_cast<float>(y_of(i, 30) + 15), 12.f,
             rgb(255, 128, 0));
    });
    time("line w2", 500, [&](uint32_t i) {
        line(s, static_cast<float>(x_of(i, 1)), static_cast<float>(y_of(i, 1)),
             static_cast<float>(x_of(i * 7 + 3, 1)), static_cast<float>(y_of(i * 5 + 1, 1)),
             rgb(0, 255, 128), 2.f);
    });
    time("blit 565 32x32", 2000,
         [&](uint32_t i) { blit(s, sprite, x_of(i, kSprite), y_of(i, kSprite)); });
    time("blit I8 32x32", 2000,
         [&](uint32_t i) { blit(s, indexed, x_of(i, kSprite), y_of(i, kSprite)); });
    time("turn+scale 2x", 200, [&](uint32_t i) {
        Transform t;
        t.x = static_cast<float>(x_of(i, 64) + 32);
        t.y = static_cast<float>(y_of(i, 64) + 32);
        t.ax = t.ay = kSprite / 2.f;
        t.angle = static_cast<float>(i) * 0.1f;
        t.sx = t.sy = 2.f;
        blit_transformed(s, indexed, t, Sampling::Smooth);
    });
    time("1-bit present", 10,
         [&](uint32_t) { presenter.present(data.mono.data(), kMonoW, kMonoH, s); });
    return ran;
}

} // namespace gfx::bench

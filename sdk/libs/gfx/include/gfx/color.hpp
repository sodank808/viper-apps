#pragma once

// RGB565 pixels and color math, as the display and the back buffer use them:
// red in the top 5 bits, native byte order.
//
// Part of gfx's platform-free drawing library (gfx/*.hpp): header-only, no
// LVGL or ESP-IDF, so built-in apps, ELF apps (VIPER_LIBS gfx) and the sim's
// tests share it. See components/gfx/README.md.

#include <algorithm>
#include <cstdint>

namespace gfx {

using Pixel = uint16_t;

struct Rgb {
    uint8_t r = 0, g = 0, b = 0;
};

constexpr Pixel rgb(int r, int g, int b) {
    return static_cast<Pixel>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}
constexpr Pixel rgb(Rgb c) { return rgb(c.r, c.g, c.b); }
// From 0xRRGGBB
constexpr Pixel rgb_hex(uint32_t hex) {
    return rgb((hex >> 16) & 0xFF, (hex >> 8) & 0xFF, hex & 0xFF);
}

// Back to 8 bits a channel, filling the low bits so white stays 255
constexpr Rgb unpack(Pixel p) {
    const int r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
    return Rgb{static_cast<uint8_t>((r << 3) | (r >> 2)), static_cast<uint8_t>((g << 2) | (g >> 4)),
               static_cast<uint8_t>((b << 3) | (b >> 2))};
}

// c times k / 256, saturating (k > 256 brightens)
constexpr Rgb scale(Rgb c, int k256) {
    return Rgb{static_cast<uint8_t>(std::min(255, c.r * k256 >> 8)),
               static_cast<uint8_t>(std::min(255, c.g * k256 >> 8)),
               static_cast<uint8_t>(std::min(255, c.b * k256 >> 8))};
}

// From a toward b by t / 256
constexpr Rgb mix(Rgb a, Rgb b, int t256) {
    return Rgb{static_cast<uint8_t>(a.r + ((b.r - a.r) * t256 >> 8)),
               static_cast<uint8_t>(a.g + ((b.g - a.g) * t256 >> 8)),
               static_cast<uint8_t>(a.b + ((b.b - a.b) * t256 >> 8))};
}

// dst toward src by a32 / 32 (0 keeps dst, 32 is src). All three channels at
// once: spreading the pixel to 0x07E0F81F leaves room for each to multiply.
inline Pixel blend(Pixel dst, Pixel src, uint32_t a32) {
    uint32_t d = (dst | (static_cast<uint32_t>(dst) << 16)) & 0x07E0F81Fu;
    const uint32_t s = (src | (static_cast<uint32_t>(src) << 16)) & 0x07E0F81Fu;
    d = (d + (((s - d) * a32) >> 5)) & 0x07E0F81Fu;
    return static_cast<Pixel>(d | (d >> 16));
}

// dst + src, each channel saturating (glows, sparks)
inline Pixel add(Pixel dst, Pixel src) {
    const uint32_t r = std::min<uint32_t>(0x1F, ((dst >> 11) & 0x1F) + ((src >> 11) & 0x1F));
    const uint32_t g = std::min<uint32_t>(0x3F, ((dst >> 5) & 0x3F) + ((src >> 5) & 0x3F));
    const uint32_t b = std::min<uint32_t>(0x1F, (dst & 0x1F) + (src & 0x1F));
    return static_cast<Pixel>((r << 11) | (g << 5) | b);
}

} // namespace gfx

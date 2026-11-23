// NetHack's tiles, read from tiles.bin as they're needed (nethack_tiles.hpp).

#include "nethack_tiles.hpp"

#include <cstring>

namespace nethack {
namespace {

// Slots in each cache: more than the tiles one screen shows
constexpr int kSlots = 192;
constexpr int kHeader = 20; // magic, sizes, count, palette size

uint16_t u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t u32(const uint8_t *p) { return u16(p) | (static_cast<uint32_t>(u16(p + 2)) << 16); }

void setup(std::vector<gfx::Pixel> &data, std::vector<int> &tile_of, std::vector<int16_t> &slot,
           int pixels, int count) {
    data.assign(static_cast<size_t>(kSlots) * pixels, 0);
    tile_of.assign(kSlots, -1);
    slot.assign(static_cast<size_t>(count), -1);
}

} // namespace

bool TileSet::open(const std::string &path) {
    close();
    file_ = std::fopen(path.c_str(), "rb");
    if (!file_) {
        return false;
    }
    uint8_t header[kHeader];
    if (std::fread(header, 1, sizeof header, file_) != sizeof header ||
        std::memcmp(header, "NHTILES1", 8) != 0 || u16(header + 8) != kSize ||
        u16(header + 10) != kSize) {
        close();
        return false;
    }
    const int count = static_cast<int>(u32(header + 12));
    const int colors = u16(header + 16);
    std::vector<uint8_t> raw(static_cast<size_t>(colors) * 2);
    if (count <= 0 || count > 32767 || colors <= 0 || colors > 256 ||
        std::fread(raw.data(), 1, raw.size(), file_) != raw.size()) {
        close();
        return false;
    }
    palette_.assign(256, 0);
    for (int i = 0; i < colors; i++) {
        palette_[i] = u16(&raw[static_cast<size_t>(i) * 2]);
    }
    count_ = count;
    tiles_at_ = kHeader + static_cast<long>(raw.size());
    big_.pixels = kSize * kSize;
    small_.pixels = kSmall * kSmall;
    setup(big_.data, big_.tile_of, big_.slot, big_.pixels, count);
    setup(small_.data, small_.tile_of, small_.slot, small_.pixels, count);
    textures_.pixels = kSize * kSize;
    setup(textures_.data, textures_.tile_of, textures_.slot, textures_.pixels, count * 4);
    texture_.assign(kSlots, raycast::Texture{});
    return true;
}

void TileSet::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    count_ = 0;
    palette_.clear();
    big_ = Cache{};
    small_ = Cache{};
    textures_ = Cache{};
    texture_.clear();
}

bool TileSet::read(int t, uint8_t *indexes) {
    const long at = tiles_at_ + static_cast<long>(t) * kSize * kSize;
    return std::fseek(file_, at, SEEK_SET) == 0 &&
           std::fread(indexes, 1, kSize * kSize, file_) == kSize * kSize;
}

const gfx::Pixel *TileSet::cached(Cache &c, int t) {
    const int16_t s = c.slot[static_cast<size_t>(t)];
    if (s >= 0) {
        return &c.data[static_cast<size_t>(s) * c.pixels];
    }
    uint8_t indexes[kSize * kSize];
    if (!read(t, indexes)) {
        return nullptr;
    }
    const int slot = c.next;
    c.next = (c.next + 1) % kSlots;
    if (c.tile_of[slot] >= 0) {
        c.slot[static_cast<size_t>(c.tile_of[slot])] = -1;
    }
    c.tile_of[slot] = t;
    c.slot[static_cast<size_t>(t)] = static_cast<int16_t>(slot);
    gfx::Pixel *out = &c.data[static_cast<size_t>(slot) * c.pixels];
    if (c.pixels == kSize * kSize) {
        for (int i = 0; i < kSize * kSize; i++) {
            out[i] = palette_[indexes[i]];
        }
        return out;
    }
    // Each small pixel is the average of four
    for (int y = 0; y < kSmall; y++) {
        for (int x = 0; x < kSmall; x++) {
            int r = 0, g = 0, b = 0;
            for (int k = 0; k < 4; k++) {
                const int i = (y * 2 + (k >> 1)) * kSize + x * 2 + (k & 1);
                const gfx::Rgb p = gfx::unpack(palette_[indexes[i]]);
                r += p.r;
                g += p.g;
                b += p.b;
            }
            out[y * kSmall + x] = gfx::rgb(r / 4, g / 4, b / 4);
        }
    }
    return out;
}

const raycast::Texture *TileSet::texture(int t, Look look) {
    if (!file_ || t < 0 || t >= count_) {
        return nullptr;
    }
    const int key = t * 4 + static_cast<int>(look);
    Cache &c = textures_;
    const int16_t s = c.slot[static_cast<size_t>(key)];
    if (s >= 0) {
        return &texture_[static_cast<size_t>(s)];
    }
    uint8_t indexes[kSize * kSize];
    if (!read(t, indexes)) {
        return nullptr;
    }
    const int slot = c.next;
    c.next = (c.next + 1) % kSlots;
    if (c.tile_of[slot] >= 0) {
        c.slot[static_cast<size_t>(c.tile_of[slot])] = -1;
    }
    c.tile_of[slot] = key;
    c.slot[static_cast<size_t>(key)] = static_cast<int16_t>(slot);
    gfx::Pixel *out = &c.data[static_cast<size_t>(slot) * c.pixels];
    const bool sprite = look == Look::Sprite || look == Look::SpriteDim;
    const bool dim = look == Look::OpaqueDim || look == Look::SpriteDim;
    for (int y = 0; y < kSize; y++) {
        for (int x = 0; x < kSize; x++) {
            const uint8_t i = indexes[y * kSize + x];
            gfx::Pixel p = palette_[i];
            if (i == 0 && sprite) {
                p = raycast::kClear;
            } else if (dim) {
                p = gfx::rgb(gfx::scale(gfx::unpack(p), 110));
            }
            if (p == raycast::kClear && !(i == 0 && sprite)) {
                p ^= 0x0020; // a real pixel that happens to be the clear color
            }
            out[x * kSize + y] = p; // column-major
        }
    }
    texture_[static_cast<size_t>(slot)] = raycast::Texture{out, 4};
    return &texture_[static_cast<size_t>(slot)];
}

const gfx::Pixel *TileSet::tile(int t, bool small) {
    if (!file_ || t < 0 || t >= count_) {
        return nullptr;
    }
    return cached(small ? small_ : big_, t);
}

} // namespace nethack

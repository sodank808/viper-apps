#pragma once

// NetHack's tiles (tiles.bin, tools/make_tiles.py) for the tile views. All of
// them come to 590 KB, more than the app should hold in memory beside the
// game, so the file stays open and the tiles the map shows are read into a
// cache as they're needed: a screen shows a few dozen at most. The small
// (8x8) tiles are made from the 16x16 ones, each pixel the average of four,
// and the first-person view's textures (column-major, as the raycaster reads
// them) from them too.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "gfx/color.hpp"
#include "raycast/raycaster.hpp"

namespace nethack {

class TileSet {
  public:
    static constexpr int kSize = 16;
    static constexpr int kSmall = 8;

    TileSet() = default;
    ~TileSet() { close(); }
    TileSet(const TileSet &) = delete;
    TileSet &operator=(const TileSet &) = delete;

    // Opens tiles.bin and reads its palette. False if it's missing or isn't
    // one (the tile views are then left out).
    bool open(const std::string &path);
    void close();
    bool is_open() const { return file_ != nullptr; }
    int count() const { return count_; }

    // Tile t's pixels, kSize x kSize (or kSmall x kSmall), a row at a time;
    // nullptr if t is out of range or can't be read. Good until the cache
    // needs the room: draw it before asking for many more.
    const gfx::Pixel *tile(int t, bool small = false);

    // Tile t as a 16x16 texture for the raycaster (column-major): opaque
    // (the background black) for walls and floors, or a sprite (the
    // background raycast::kClear); dimmed for what the hero remembers but
    // doesn't see. nullptr if t is out of range or can't be read. Good until
    // the cache needs the room, as tile() is.
    enum class Look : uint8_t { Opaque, OpaqueDim, Sprite, SpriteDim };
    const raycast::Texture *texture(int t, Look look);

  private:
    struct Cache {
        int pixels = 0; // per tile
        std::vector<gfx::Pixel> data;
        std::vector<int> tile_of;  // by slot, -1 for empty
        std::vector<int16_t> slot; // by tile, -1 for not cached
        int next = 0;              // the slot to take next (round robin)
    };
    const gfx::Pixel *cached(Cache &c, int t);
    bool read(int t, uint8_t *indexes);

    std::FILE *file_ = nullptr;
    int count_ = 0;
    long tiles_at_ = 0; // where the first tile starts in the file
    std::vector<gfx::Pixel> palette_;
    Cache big_, small_;
    Cache textures_;                        // by t * 4 + look
    std::vector<raycast::Texture> texture_; // by slot
};

} // namespace nethack

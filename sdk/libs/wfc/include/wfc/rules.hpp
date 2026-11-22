#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace wfc {

/// Grid directions, clockwise. opposite(d) is two steps round; turning a tile
/// a quarter turn clockwise moves what faced d to face (d + 1) % 4.
enum Dir : uint8_t { North = 0, East = 1, South = 2, West = 3 };
inline constexpr std::array<int, 4> kDx{0, 1, 0, -1};
inline constexpr std::array<int, 4> kDy{-1, 0, 1, 0};
constexpr Dir opposite(Dir d) { return static_cast<Dir>((d + 2) & 3); }

/// A set of tile ids (a bitset sized for a tile count).
class Domain {
  public:
    Domain() = default;
    explicit Domain(int tile_count, bool full = false);
    static Domain all(int tile_count) { return Domain(tile_count, true); }
    static Domain of(int tile_count, std::initializer_list<int> tiles);

    int capacity() const { return size_; }
    void set(int tile) { words_[tile >> 6] |= uint64_t{1} << (tile & 63); }
    void reset(int tile) { words_[tile >> 6] &= ~(uint64_t{1} << (tile & 63)); }
    bool test(int tile) const { return (words_[tile >> 6] >> (tile & 63)) & 1; }
    int count() const;
    bool empty() const { return count() == 0; }
    std::vector<int> tiles() const;
    const std::vector<uint64_t> &words() const { return words_; }

    Domain &operator&=(const Domain &other);
    Domain &operator|=(const Domain &other);
    bool operator==(const Domain &other) const = default;

  private:
    int size_ = 0;
    std::vector<uint64_t> words_;
};

/// Which tiles may sit next to which, and how often each should be picked.
/// Adjacency is symmetric: allowing b on a's east side also allows a on b's
/// west side.
class Rules {
  public:
    explicit Rules(int tile_count = 0);

    int tile_count() const { return tile_count_; }
    /// 64-bit words in a domain of this many tiles
    int words() const { return words_; }

    void set_weight(int tile, float weight) { weights_[tile] = weight; }
    float weight(int tile) const { return weights_[tile]; }

    /// Lets b sit on a's d side (and so a on b's opposite side).
    void allow(int a, Dir d, int b);
    /// Undoes allow(): rules out b on a's d side (and a on b's opposite side).
    /// Handy for trimming rules built from sockets.
    void forbid(int a, Dir d, int b);
    bool allows(int a, Dir d, int b) const;
    /// The tiles allowed on a's d side, as words() bit words.
    const uint64_t *compatible(int a, Dir d) const {
        return &compatible_[(static_cast<size_t>(a) * 4 + d) * words_];
    }

    /// Simple tiled model: allows every adjacency seen in an example map of tile
    /// ids (row-major, -1 for "no tile") and sets each tile that appears to a
    /// weight of how often it does. With periodic, the example wraps around.
    void learn(const std::vector<int> &sample, int width, int height, bool periodic = false);

  private:
    int tile_count_ = 0;
    int words_ = 0;
    std::vector<float> weights_;
    std::vector<uint64_t> compatible_; // [tile][dir][word]
};

/// Whether a filled grid of tile ids follows the rules. -1 (no tile) fails.
bool consistent(const Rules &rules, const std::vector<int> &tiles, int width, int height,
                bool periodic = false);

} // namespace wfc

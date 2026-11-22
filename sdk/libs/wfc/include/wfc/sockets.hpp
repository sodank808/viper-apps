#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "wfc/rules.hpp"

namespace wfc {

/// Builds Rules from edge sockets, so a tileset is described tile by tile
/// instead of pair by pair.
///
/// Each tile labels its four edges (north, east, south, west), each read
/// clockwise round the tile: the north edge left to right, the east edge top
/// to bottom, the south edge right to left, the west edge bottom to top. Two
/// tiles fit side by side when one's shared edge reads as the other's read
/// backwards, the way two jigsaw pieces' outlines match. One character per
/// sample point works well:
///
/// - Edge features: "0T0" is a wire leaving the middle of an edge, "000" bare
///   board. Symmetric labels like these fit their own kind.
/// - Corner terrains: a north edge "G.S" has grass at the north-west corner
///   and sand at the north-east, so the tile above must have grass at its
///   south-west corner and sand at its south-east (its south edge "S.G").
/// - Asymmetric edges, such as the border of a chip with the chip on one side,
///   only fit the right way round.
///
/// add() can also generate quarter-turn rotations of a tile; the sockets turn
/// with it.
class SocketTiles {
  public:
    struct Variant {
        int base;     ///< index of the add() call this came from
        int rotation; ///< quarter turns clockwise
        std::array<std::string, 4> sockets;
        float weight;
    };

    /// Adds a tile and, with rotations 2 or 4, that many quarter-turn
    /// rotations of it (2 for tiles that look the same turned half way round,
    /// like a straight wire). Each variant gets weight. Returns the first
    /// variant's id; the others follow it.
    int add(std::string_view name, const std::array<std::string, 4> &sockets, float weight = 1.0f,
            int rotations = 1);

    int size() const { return static_cast<int>(variants_.size()); }
    const Variant &variant(int id) const { return variants_[id]; }
    const std::string &name(int id) const { return names_[variants_[id].base]; }
    /// The variant of a named tile at a rotation, or -1
    int find(std::string_view name, int rotation = 0) const;

    /// Whether b fits on a's d side
    bool fits(int a, Dir d, int b) const;
    Rules rules() const;

  private:
    std::vector<std::string> names_;
    std::vector<Variant> variants_;
};

} // namespace wfc

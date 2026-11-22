#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "wfc/rules.hpp"

namespace wfc {

enum class Status { Running, Done, Failed };

/// How the next cell to collapse is chosen.
enum class Heuristic {
    Entropy,  ///< lowest weighted Shannon entropy (classic WFC)
    MinCount, ///< fewest possible tiles
    Scanline, ///< first undecided cell in row order
};

struct Options {
    int width = 0;
    int height = 0;
    /// Opposite edges are neighbours, so the result tiles seamlessly
    bool periodic = false;
    uint32_t seed = 1;
    Heuristic heuristic = Heuristic::Entropy;
    /// Backtracks allowed per attempt before starting over with new choices
    int max_backtracks = 1000;
    /// Fresh attempts before giving up
    int max_restarts = 10;
};

/// Solves a grid of cells, each of which must become one tile, so that every
/// pair of neighbours is allowed by the rules (a finite-domain constraint
/// problem). Each cell keeps the set of tiles it could still be. A step
/// collapses one undecided cell to a single tile (picked by weight) and
/// propagates: every neighbour drops the tiles the cell's remaining tiles
/// don't allow next to them, and so on outward, so after each step every
/// remaining tile in every cell has some allowed tile beside it on each side
/// (arc consistency).
///
/// When a cell runs out of tiles, the solver backtracks: it undoes the last
/// choice, rules that tile out for that cell, and carries on, going further
/// back if that fails too. After max_backtracks it starts over from the
/// constraints with fresh random choices, max_restarts times, then fails. It
/// only reports Failed early when it has proven there is no solution.
///
/// Constraints (restrict(), ban(), fix()) go in before the first step(): they
/// are the starting point that restarts return to. That's how an existing map
/// gets filled in or retiled: limit each known cell to the tiles that match
/// it and leave the unknown ones open.
///
/// Rules are copied, so the solver doesn't depend on them staying around.
class Solver {
  public:
    /// Weight multiplier for a tile at a cell, on top of its rule weight
    using CellWeight = std::function<float(int x, int y, int tile)>;

    struct Stats {
        int decisions = 0;  ///< cells collapsed by choice (not forced)
        int backtracks = 0; ///< choices undone
        int restarts = 0;
    };

    Solver(const Rules &rules, const Options &options);

    /// Back to every tile possible everywhere (less whatever the rules alone
    /// rule out), forgetting constraints, with a new seed.
    void clear(uint32_t seed);
    /// Limits a cell to tiles in allowed. Returns false if that leaves some
    /// cell with no tile at all (the solver then reports Failed).
    bool restrict(int x, int y, const Domain &allowed);
    bool ban(int x, int y, int tile);
    bool fix(int x, int y, int tile);
    /// Says what lies beyond one edge of a non-periodic grid: the cells along
    /// it keep only tiles that fit beside one of the outside tiles. Without
    /// this, an edge accepts anything (a house's left half could sit on the
    /// right edge with its other half off the map).
    bool restrict_border(Dir side, const Domain &outside);
    void set_cell_weight(CellWeight weight);

    /// Collapses one cell and propagates (backtracking if that runs into a
    /// dead end). Returns the status after it.
    Status step();
    /// Steps until done or failed, or at most max_steps steps (-1 for no limit).
    Status run(int max_steps = -1);
    Status status() const { return status_; }

    int width() const { return width_; }
    int height() const { return height_; }
    const Rules &rules() const { return rules_; }
    const Stats &stats() const { return stats_; }

    /// How many tiles the cell could still be
    int count(int x, int y) const;
    bool possible(int x, int y, int tile) const;
    /// The cell's tile once it's down to one, else -1
    int tile(int x, int y) const;
    Domain domain(int x, int y) const;
    /// The cell's possible tiles as rules().words() bit words, without copying
    const uint64_t *domain_words(int x, int y) const { return words(index(x, y)); }
    /// Every cell's tile, row-major (-1 where undecided)
    std::vector<int> result() const;

  private:
    struct Decision {
        int cell;
        int tile;
        size_t trail_mark;
    };

    int index(int x, int y) const { return y * width_ + x; }
    uint64_t *words(int cell) { return &domains_[static_cast<size_t>(cell) * words_]; }
    const uint64_t *words(int cell) const { return &domains_[static_cast<size_t>(cell) * words_]; }
    int neighbour(int cell, Dir d) const;
    int cell_count(int cell) const;

    /// Intersects a cell with mask. False if that empties it.
    bool narrow(int cell, const uint64_t *mask);
    bool propagate();
    void enqueue(int cell);
    void undo_to(size_t mark);
    void recover();
    void restart();
    int pick_cell();
    int pick_tile(int cell);
    float tile_weight(int cell, int tile) const;
    float entropy(int cell);

    uint32_t next_random();
    float random_unit();

    Rules rules_;
    Options options_;
    int width_;
    int height_;
    int words_;
    std::vector<uint64_t> domains_;
    CellWeight cell_weight_;

    // Undo log: the old domain words of each change, newest last
    std::vector<int> trail_cells_;
    std::vector<uint64_t> trail_words_;
    size_t base_mark_ = 0;
    bool started_ = false;
    std::vector<Decision> decisions_;
    int attempt_backtracks_ = 0;

    std::vector<int> queue_;
    std::vector<uint8_t> queued_;
    // Scratch domains
    std::vector<uint64_t> support_;
    std::vector<uint64_t> current_;
    std::vector<uint64_t> mask_;
    std::vector<float> entropy_;
    std::vector<uint8_t> entropy_dirty_;

    Status status_ = Status::Running;
    Stats stats_;
    uint64_t rng_state_ = 0;
};

} // namespace wfc

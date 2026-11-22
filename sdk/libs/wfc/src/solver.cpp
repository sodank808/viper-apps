#include "wfc/solver.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace wfc {

Solver::Solver(const Rules &rules, const Options &options)
    : rules_(rules)
    , options_(options)
    , width_(options.width)
    , height_(options.height)
    , words_(rules.words())
    , domains_(static_cast<size_t>(width_) * height_ * words_)
    , queued_(static_cast<size_t>(width_) * height_, 0)
    , support_(words_)
    , current_(words_)
    , mask_(words_)
    , entropy_(static_cast<size_t>(width_) * height_, 0.0f)
    , entropy_dirty_(static_cast<size_t>(width_) * height_, 1) {
    clear(options.seed);
}

void Solver::clear(uint32_t seed) {
    rng_state_ = seed;
    status_ = Status::Running;
    stats_ = {};
    started_ = false;
    base_mark_ = 0;
    attempt_backtracks_ = 0;
    decisions_.clear();
    trail_cells_.clear();
    trail_words_.clear();
    queue_.clear();
    std::fill(queued_.begin(), queued_.end(), 0);

    const Domain full = Domain::all(rules_.tile_count());
    const int cells = width_ * height_;
    for (int c = 0; c < cells; ++c) {
        std::copy(full.words().begin(), full.words().end(), words(c));
        entropy_dirty_[c] = 1;
    }
    if (rules_.tile_count() == 0 && cells > 0) {
        status_ = Status::Failed;
        return;
    }
    // Rules alone can rule tiles out (one with nothing allowed to its east
    // can't be anywhere but the east edge)
    for (int c = 0; c < cells; ++c) {
        enqueue(c);
    }
    if (!propagate()) {
        status_ = Status::Failed;
    }
}

bool Solver::restrict(int x, int y, const Domain &allowed) {
    if (status_ == Status::Failed) {
        return false;
    }
    if (!narrow(index(x, y), allowed.words().data()) || !propagate()) {
        if (started_) {
            recover();
        } else {
            status_ = Status::Failed;
        }
    }
    return status_ != Status::Failed;
}

bool Solver::ban(int x, int y, int tile) {
    Domain d = Domain::all(rules_.tile_count());
    d.reset(tile);
    return restrict(x, y, d);
}

bool Solver::fix(int x, int y, int tile) {
    return restrict(x, y, Domain::of(rules_.tile_count(), {tile}));
}

bool Solver::restrict_border(Dir side, const Domain &outside) {
    // Tiles that may sit on the inner side of an outside tile
    Domain fits(rules_.tile_count());
    for (int t : outside.tiles()) {
        const uint64_t *allowed = rules_.compatible(t, opposite(side));
        for (int u = 0; u < rules_.tile_count(); ++u) {
            if ((allowed[u >> 6] >> (u & 63)) & 1) {
                fits.set(u);
            }
        }
    }
    const bool vertical = side == North || side == South;
    const int n = vertical ? width_ : height_;
    for (int i = 0; i < n && status_ != Status::Failed; ++i) {
        const int x = vertical ? i : (side == West ? 0 : width_ - 1);
        const int y = vertical ? (side == North ? 0 : height_ - 1) : i;
        restrict(x, y, fits);
    }
    return status_ != Status::Failed;
}

void Solver::set_cell_weight(CellWeight weight) {
    cell_weight_ = std::move(weight);
    std::fill(entropy_dirty_.begin(), entropy_dirty_.end(), 1);
}

Status Solver::step() {
    if (status_ != Status::Running) {
        return status_;
    }
    if (!started_) {
        started_ = true;
        base_mark_ = trail_cells_.size();
    }
    const int cell = pick_cell();
    if (cell < 0) {
        status_ = Status::Done;
        return status_;
    }
    const int tile = pick_tile(cell);
    decisions_.push_back({cell, tile, trail_cells_.size()});
    ++stats_.decisions;

    std::fill(support_.begin(), support_.end(), 0);
    support_[tile >> 6] = uint64_t{1} << (tile & 63);
    if (!narrow(cell, support_.data()) || !propagate()) {
        recover();
    }
    if (status_ == Status::Running && pick_cell() < 0) {
        status_ = Status::Done;
    }
    return status_;
}

Status Solver::run(int max_steps) {
    for (int i = 0; (max_steps < 0 || i < max_steps) && status_ == Status::Running; ++i) {
        step();
    }
    return status_;
}

int Solver::count(int x, int y) const { return cell_count(index(x, y)); }

bool Solver::possible(int x, int y, int tile) const {
    return (words(index(x, y))[tile >> 6] >> (tile & 63)) & 1;
}

int Solver::tile(int x, int y) const {
    const uint64_t *w = words(index(x, y));
    int found = -1;
    for (int i = 0; i < words_; ++i) {
        if (!w[i]) {
            continue;
        }
        if (found >= 0 || (w[i] & (w[i] - 1))) {
            return -1;
        }
        found = i * 64 + std::countr_zero(w[i]);
    }
    return found;
}

Domain Solver::domain(int x, int y) const {
    Domain d(rules_.tile_count());
    const uint64_t *w = words(index(x, y));
    for (int t = 0; t < rules_.tile_count(); ++t) {
        if ((w[t >> 6] >> (t & 63)) & 1) {
            d.set(t);
        }
    }
    return d;
}

std::vector<int> Solver::result() const {
    std::vector<int> out(static_cast<size_t>(width_) * height_);
    for (int y = 0; y < height_; ++y) {
        for (int x = 0; x < width_; ++x) {
            out[index(x, y)] = tile(x, y);
        }
    }
    return out;
}

int Solver::neighbour(int cell, Dir d) const {
    int x = cell % width_ + kDx[d];
    int y = cell / width_ + kDy[d];
    if (options_.periodic) {
        x = (x + width_) % width_;
        y = (y + height_) % height_;
    } else if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return -1;
    }
    return index(x, y);
}

int Solver::cell_count(int cell) const {
    const uint64_t *w = words(cell);
    int n = 0;
    for (int i = 0; i < words_; ++i) {
        n += std::popcount(w[i]);
    }
    return n;
}

bool Solver::narrow(int cell, const uint64_t *mask) {
    uint64_t *w = words(cell);
    bool changed = false;
    for (int i = 0; i < words_; ++i) {
        changed |= (w[i] & mask[i]) != w[i];
    }
    if (!changed) {
        return true;
    }
    // Before the first step there's nothing to undo back to
    if (started_) {
        trail_cells_.push_back(cell);
        trail_words_.insert(trail_words_.end(), w, w + words_);
    }
    uint64_t any = 0;
    for (int i = 0; i < words_; ++i) {
        w[i] &= mask[i];
        any |= w[i];
    }
    entropy_dirty_[cell] = 1;
    if (!any) {
        return false;
    }
    enqueue(cell);
    return true;
}

void Solver::enqueue(int cell) {
    if (!queued_[cell]) {
        queued_[cell] = 1;
        queue_.push_back(cell);
    }
}

bool Solver::propagate() {
    while (!queue_.empty()) {
        const int cell = queue_.back();
        queue_.pop_back();
        queued_[cell] = 0;
        std::copy(words(cell), words(cell) + words_, current_.begin());
        for (int d = 0; d < 4; ++d) {
            const int n = neighbour(cell, static_cast<Dir>(d));
            if (n < 0) {
                continue;
            }
            // What the cell's remaining tiles allow beside them on this side
            std::fill(support_.begin(), support_.end(), 0);
            for (int i = 0; i < words_; ++i) {
                for (uint64_t w = current_[i]; w; w &= w - 1) {
                    const int t = i * 64 + std::countr_zero(w);
                    const uint64_t *allowed = rules_.compatible(t, static_cast<Dir>(d));
                    for (int j = 0; j < words_; ++j) {
                        support_[j] |= allowed[j];
                    }
                }
            }
            if (!narrow(n, support_.data())) {
                for (int q : queue_) {
                    queued_[q] = 0;
                }
                queue_.clear();
                return false;
            }
        }
    }
    return true;
}

void Solver::undo_to(size_t mark) {
    while (trail_cells_.size() > mark) {
        const int cell = trail_cells_.back();
        trail_cells_.pop_back();
        std::copy(trail_words_.end() - words_, trail_words_.end(), words(cell));
        trail_words_.resize(trail_words_.size() - words_);
        entropy_dirty_[cell] = 1;
    }
}

void Solver::recover() {
    while (true) {
        if (decisions_.empty()) {
            // A dead end with no choices left to undo: the constraints (and
            // whatever was proven since) have no solution
            status_ = Status::Failed;
            return;
        }
        if (attempt_backtracks_ >= options_.max_backtracks) {
            restart();
            return;
        }
        ++attempt_backtracks_;
        ++stats_.backtracks;
        const Decision d = decisions_.back();
        decisions_.pop_back();
        undo_to(d.trail_mark);
        // That tile doesn't work there given the choices before it
        std::fill(mask_.begin(), mask_.end(), ~uint64_t{0});
        mask_[d.tile >> 6] &= ~(uint64_t{1} << (d.tile & 63));
        if (narrow(d.cell, mask_.data()) && propagate()) {
            return;
        }
    }
}

void Solver::restart() {
    undo_to(base_mark_);
    decisions_.clear();
    attempt_backtracks_ = 0;
    ++stats_.restarts;
    if (stats_.restarts > options_.max_restarts) {
        status_ = Status::Failed;
    }
}

int Solver::pick_cell() {
    const int cells = width_ * height_;
    int best = -1;
    float best_score = std::numeric_limits<float>::max();
    for (int c = 0; c < cells; ++c) {
        const int n = cell_count(c);
        if (n <= 1) {
            continue;
        }
        float score;
        switch (options_.heuristic) {
        case Heuristic::Scanline:
            return c;
        case Heuristic::MinCount:
            score = static_cast<float>(n);
            break;
        case Heuristic::Entropy:
        default:
            score = entropy(c);
            break;
        }
        // A little noise breaks ties, so it doesn't sweep in row order
        score += random_unit() * 1e-3f;
        if (score < best_score) {
            best_score = score;
            best = c;
        }
    }
    return best;
}

float Solver::tile_weight(int cell, int tile) const {
    float w = rules_.weight(tile);
    if (cell_weight_) {
        w *= cell_weight_(cell % width_, cell / width_, tile);
    }
    return std::max(w, 0.0f);
}

int Solver::pick_tile(int cell) {
    const uint64_t *w = words(cell);
    float total = 0;
    int n = 0;
    for (int i = 0; i < words_; ++i) {
        for (uint64_t bits = w[i]; bits; bits &= bits - 1) {
            total += tile_weight(cell, i * 64 + std::countr_zero(bits));
            ++n;
        }
    }
    // All weights zero: pick evenly
    const bool even = total <= 0;
    float r = random_unit() * (even ? static_cast<float>(n) : total);
    int last = -1;
    for (int i = 0; i < words_; ++i) {
        for (uint64_t bits = w[i]; bits; bits &= bits - 1) {
            last = i * 64 + std::countr_zero(bits);
            r -= even ? 1.0f : tile_weight(cell, last);
            if (r < 0) {
                return last;
            }
        }
    }
    return last; // rounding
}

float Solver::entropy(int cell) {
    if (entropy_dirty_[cell]) {
        const uint64_t *w = words(cell);
        // Float, not double: the S3 has no double FPU, and ELF apps get only
        // logf from the firmware (components/elf_loader/exports/libc.txt)
        float sum = 0, sum_log = 0;
        int n = 0;
        for (int i = 0; i < words_; ++i) {
            for (uint64_t bits = w[i]; bits; bits &= bits - 1) {
                const float weight = tile_weight(cell, i * 64 + std::countr_zero(bits));
                ++n;
                if (weight > 0) {
                    sum += weight;
                    sum_log += weight * std::log(weight);
                }
            }
        }
        entropy_[cell] =
            sum > 0 ? std::log(sum) - sum_log / sum : std::log(static_cast<float>(std::max(n, 1)));
        entropy_dirty_[cell] = 0;
    }
    return entropy_[cell];
}

uint32_t Solver::next_random() {
    // splitmix64
    uint64_t z = (rng_state_ += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return static_cast<uint32_t>((z ^ (z >> 31)) >> 32);
}

float Solver::random_unit() { return (next_random() >> 8) * (1.0f / 16777216.0f); }

} // namespace wfc

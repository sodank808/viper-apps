#include "wfc/rules.hpp"

#include <bit>

namespace wfc {

Domain::Domain(int tile_count, bool full)
    : size_(tile_count)
    , words_((tile_count + 63) / 64, 0) {
    if (full) {
        for (int t = 0; t < tile_count; ++t) {
            set(t);
        }
    }
}

Domain Domain::of(int tile_count, std::initializer_list<int> tiles) {
    Domain d(tile_count);
    for (int t : tiles) {
        d.set(t);
    }
    return d;
}

int Domain::count() const {
    int n = 0;
    for (uint64_t w : words_) {
        n += std::popcount(w);
    }
    return n;
}

std::vector<int> Domain::tiles() const {
    std::vector<int> out;
    for (size_t i = 0; i < words_.size(); ++i) {
        for (uint64_t w = words_[i]; w; w &= w - 1) {
            out.push_back(static_cast<int>(i * 64) + std::countr_zero(w));
        }
    }
    return out;
}

Domain &Domain::operator&=(const Domain &other) {
    for (size_t i = 0; i < words_.size() && i < other.words_.size(); ++i) {
        words_[i] &= other.words_[i];
    }
    for (size_t i = other.words_.size(); i < words_.size(); ++i) {
        words_[i] = 0;
    }
    return *this;
}

Domain &Domain::operator|=(const Domain &other) {
    for (size_t i = 0; i < words_.size() && i < other.words_.size(); ++i) {
        words_[i] |= other.words_[i];
    }
    return *this;
}

Rules::Rules(int tile_count)
    : tile_count_(tile_count)
    , words_((tile_count + 63) / 64)
    , weights_(tile_count, 1.0f)
    , compatible_(static_cast<size_t>(tile_count) * 4 * words_, 0) {}

void Rules::allow(int a, Dir d, int b) {
    compatible_[(static_cast<size_t>(a) * 4 + d) * words_ + (b >> 6)] |= uint64_t{1} << (b & 63);
    const Dir back = opposite(d);
    compatible_[(static_cast<size_t>(b) * 4 + back) * words_ + (a >> 6)] |= uint64_t{1} << (a & 63);
}

void Rules::forbid(int a, Dir d, int b) {
    compatible_[(static_cast<size_t>(a) * 4 + d) * words_ + (b >> 6)] &= ~(uint64_t{1} << (b & 63));
    const Dir back = opposite(d);
    compatible_[(static_cast<size_t>(b) * 4 + back) * words_ + (a >> 6)] &=
        ~(uint64_t{1} << (a & 63));
}

bool Rules::allows(int a, Dir d, int b) const { return (compatible(a, d)[b >> 6] >> (b & 63)) & 1; }

void Rules::learn(const std::vector<int> &sample, int width, int height, bool periodic) {
    std::vector<int> counts(tile_count_, 0);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int a = sample[y * width + x];
            if (a < 0) {
                continue;
            }
            ++counts[a];
            // East and south cover every pair once
            for (Dir d : {East, South}) {
                int nx = x + kDx[d], ny = y + kDy[d];
                if (periodic) {
                    nx = (nx + width) % width;
                    ny = (ny + height) % height;
                } else if (nx >= width || ny >= height) {
                    continue;
                }
                const int b = sample[ny * width + nx];
                if (b >= 0) {
                    allow(a, d, b);
                }
            }
        }
    }
    for (int t = 0; t < tile_count_; ++t) {
        if (counts[t] > 0) {
            weights_[t] = static_cast<float>(counts[t]);
        }
    }
}

bool consistent(const Rules &rules, const std::vector<int> &tiles, int width, int height,
                bool periodic) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int a = tiles[y * width + x];
            if (a < 0 || a >= rules.tile_count()) {
                return false;
            }
            for (Dir d : {East, South}) {
                int nx = x + kDx[d], ny = y + kDy[d];
                if (periodic) {
                    nx %= width;
                    ny %= height;
                } else if (nx >= width || ny >= height) {
                    continue;
                }
                const int b = tiles[ny * width + nx];
                if (b < 0 || b >= rules.tile_count() || !rules.allows(a, d, b)) {
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace wfc

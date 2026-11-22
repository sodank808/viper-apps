#pragma once

// A game's own random numbers: xorshift32, seeded once, so a game replays
// exactly from its seed and its input (Blocks' and Turbo Blast's replays rely
// on it). Seed it from the platform's random source: viper_random() in an ELF
// app, System::get().getRandomInt() in a built-in one.
//
// Header-only and free of platform code, so built-in apps, ELF apps
// (VIPER_LIBS game) and the sim's tests share it. See components/game/README.md.

#include <cstdint>

namespace game {

class Rng {
  public:
    explicit Rng(uint32_t seed = 1) { reseed(seed); }

    // A zero seed would stick at zero, so it's replaced with a fixed one
    void reseed(uint32_t seed) { state_ = seed ? seed : 0x9E3779B9u; }
    uint32_t state() const { return state_; }

    uint32_t next() {
        uint32_t x = state_;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return state_ = x;
    }

    // 0 .. n-1 (n > 0). A modulo, so very slightly uneven for large n; fine
    // for games, and it keeps the sequence Blocks' replays were made with.
    uint32_t below(uint32_t n) { return next() % n; }

    // lo .. hi, both included (lo <= hi)
    int32_t range(int32_t lo, int32_t hi) {
        const uint32_t span = static_cast<uint32_t>(hi) - static_cast<uint32_t>(lo) + 1u;
        if (span == 0) { // the whole int32 range
            return static_cast<int32_t>(next());
        }
        return static_cast<int32_t>(static_cast<uint32_t>(lo) + below(span));
    }

    // [0, 1), from the top 24 bits
    float unit() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }

    // True `percent` times in a hundred
    bool chance(uint32_t percent) { return below(100) < percent; }

    // Fisher-Yates. Indexes rather than pointer ranges: a pointer one past the
    // end of a global array can't always be relocated by the ELF loader.
    template <typename T> void shuffle(T *items, int count) {
        for (int i = count - 1; i > 0; i--) {
            const int j = static_cast<int>(below(static_cast<uint32_t>(i + 1)));
            T t = items[i];
            items[i] = items[j];
            items[j] = t;
        }
    }

  private:
    uint32_t state_ = 1;
};

} // namespace game

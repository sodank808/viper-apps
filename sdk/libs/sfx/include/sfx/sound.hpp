#pragma once

// A sound effect as data: a few segments played one after another, each a
// waveform whose pitch and volume slide in a straight line from start to end.
// Games keep their sounds in const tables and hand them to sfx::Synth.
//
//     constexpr sfx::Sound kJump = sfx::sound(1, sfx::seg(sfx::Wave::Pulse25, 330, 720, 90, 90,
//     0)); synth.play(kJump);

#include <array>
#include <cstdint>

namespace sfx {

// Pulse waves by duty cycle (12.5%, 25%, 50%), a triangle, noise, and a sine
enum class Wave : uint8_t { Pulse12, Pulse25, Pulse50, Triangle, Noise, Sine };

struct Segment {
    Wave wave = Wave::Pulse50;
    uint16_t from_hz = 0, to_hz = 0; // for Noise, how often the noise changes
    uint16_t ms = 0;
    uint8_t from_volume = 0, to_volume = 0; // 0-255
};

inline constexpr int kMaxSegments = 6;

struct Sound {
    // When every voice is busy, a sound replaces one playing of the same or
    // lower priority (the oldest of equals). All 0: the oldest is replaced.
    uint8_t priority = 0;
    uint8_t count = 0;
    // Nonzero: one at a time. A sound replaces whichever is playing on the
    // same channel, as an arcade's sound circuits did (`on_channel()`).
    uint8_t channel = 0;
    bool loops = false; // plays until Synth::stop() (`looped()`)
    // A warble: the pitch swings this many times a second, by up to this many
    // percent up and down (`vibrato()`)
    uint8_t vibrato_hz = 0, vibrato_percent = 0;
    std::array<Segment, kMaxSegments> segments{};
};

constexpr Segment seg(Wave wave, uint16_t from_hz, uint16_t to_hz, uint16_t ms, uint8_t from_volume,
                      uint8_t to_volume) {
    return Segment{wave, from_hz, to_hz, ms, from_volume, to_volume};
}

template <typename... S> constexpr Sound sound(uint8_t priority, S... segments) {
    static_assert(sizeof...(segments) >= 1 && sizeof...(segments) <= kMaxSegments,
                  "a sound has 1 to kMaxSegments segments");
    Sound s;
    s.priority = priority;
    s.count = sizeof...(segments);
    s.segments = {segments...};
    return s;
}

// Variants of a sound, for its table:
//     constexpr sfx::Sound kSiren = sfx::looped(sfx::vibrato(sfx::sound(0, seg(...)), 8, 30));
constexpr Sound on_channel(Sound s, uint8_t channel) {
    s.channel = channel;
    return s;
}

constexpr Sound looped(Sound s) {
    s.loops = true;
    return s;
}

constexpr Sound vibrato(Sound s, uint8_t hz, uint8_t percent) {
    s.vibrato_hz = hz;
    s.vibrato_percent = percent > 100 ? 100 : percent;
    return s;
}

// The length of a sound (once through, if it loops), in milliseconds
constexpr uint32_t length_ms(const Sound &s) {
    uint32_t ms = 0;
    for (int i = 0; i < s.count; i++) {
        ms += s.segments[i].ms;
    }
    return ms;
}

} // namespace sfx

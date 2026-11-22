#pragma once

// Music as data for sfx::Synth: four tracks of steps (sixteenth notes), a lead
// and an arpeggio on pulse waves, a triangle bass, and drums. Tracks are
// written as text and turned into steps at compile time:
//
//     constexpr sfx::ParsedTrack kLead = sfx::track("E5 . . . G5 . - . ");
//     ... kArp, kBass, kDrums ("K . H . S . H . ") ...
//     constexpr sfx::Song kTitle = sfx::song(112, kLead, kArp, kBass, kDrums);
//     static_assert(sfx::valid(kTitle), "the title's tracks differ in length, or one has a bad
//     step"); synth.music(&kTitle);
//
// A token is a note ("A4", "C#5"), "." to hold the last one, "-" to stop it,
// and on the drum track "K" kick, "S" snare, "H" hat, "O" open hat (a "." or
// "-" there is a rest). Tokens are separated by spaces. A song loops.

#include <array>
#include <cstddef>
#include <cstdint>

#include "sfx/sound.hpp"

namespace sfx {

inline constexpr int kTracks = 4;
enum Track : uint8_t { kTrackLead, kTrackArp, kTrackBass, kTrackDrums };
inline constexpr int kMaxSteps = 128;

// Steps: a MIDI note (12 and up) starts a note, kHold keeps the last one
// going, kOff stops it. On the drum track, kKick.. start a drum.
inline constexpr uint8_t kHold = 0;
inline constexpr uint8_t kOff = 1;
enum Drum : uint8_t { kKick = 2, kSnare, kHat, kOpenHat, kDrumEnd };

// The sounds of kKick .. kOpenHat
struct DrumKit {
    std::array<Sound, kDrumEnd - kKick> sounds{};
    const Sound &operator[](uint8_t step) const { return sounds[step - kKick]; }
};

inline constexpr DrumKit kDrumKit = {{
    sound(0, seg(Wave::Triangle, 180, 45, 70, 255, 0)),    // kick
    sound(0, seg(Wave::Noise, 9000, 7000, 110, 200, 0)),   // snare
    sound(0, seg(Wave::Noise, 30000, 30000, 30, 110, 0)),  // hat
    sound(0, seg(Wave::Noise, 30000, 30000, 120, 110, 0)), // open hat
}};

struct Song {
    uint16_t bpm = 120; // quarter notes a minute, at tempo kTempoNormal
    uint16_t steps = 0; // 0: not a valid song
    std::array<std::array<uint8_t, kMaxSteps>, kTracks> tracks{};
    const DrumKit *drums = &kDrumKit;
};

// A note's frequency in hundredths of a hertz (A4, MIDI 69, is 44000)
constexpr std::array<uint32_t, 128> make_note_table() {
    std::array<uint32_t, 128> table{};
    constexpr double kSemitone = 1.0594630943592953; // 2^(1/12)
    double up = 44000.0;
    for (int n = 69; n < 128; n++) {
        table[n] = static_cast<uint32_t>(up + 0.5);
        up *= kSemitone;
    }
    double down = 44000.0;
    for (int n = 69; n >= 0; n--) {
        table[n] = static_cast<uint32_t>(down + 0.5);
        down /= kSemitone;
    }
    return table;
}
inline constexpr std::array<uint32_t, 128> kNoteCentiHz = make_note_table();

// One token's step, or -1 if it isn't one
constexpr int parse_step(const char *s, int n) {
    if (n == 1) {
        switch (s[0]) {
        case '.':
            return kHold;
        case '-':
            return kOff;
        case 'K':
            return kKick;
        case 'S':
            return kSnare;
        case 'H':
            return kHat;
        case 'O':
            return kOpenHat;
        default:
            return -1;
        }
    }
    constexpr int kSemis[7] = {9, 11, 0, 2, 4, 5, 7}; // A B C D E F G
    if (n < 2 || n > 3 || s[0] < 'A' || s[0] > 'G') {
        return -1;
    }
    int semis = kSemis[s[0] - 'A'];
    int i = 1;
    if (n == 3) {
        if (s[1] != '#') {
            return -1;
        }
        semis++;
        i = 2;
    }
    if (s[i] < '0' || s[i] > '8') {
        return -1;
    }
    const int note = 12 * (s[i] - '0' + 1) + semis;
    return note < 12 || note > 127 ? -1 : note;
}

struct ParsedTrack {
    std::array<uint8_t, kMaxSteps> steps{};
    int count = 0; // -1 if a token is wrong or there are too many
};

template <size_t N> constexpr ParsedTrack track(const char (&text)[N]) {
    ParsedTrack t;
    int i = 0;
    const int end = static_cast<int>(N) - 1;
    while (i < end) {
        if (text[i] == ' ') {
            i++;
            continue;
        }
        int j = i;
        while (j < end && text[j] != ' ') {
            j++;
        }
        const int step = parse_step(text + i, j - i);
        if (step < 0 || t.count >= kMaxSteps) {
            t.count = -1;
            return t;
        }
        t.steps[t.count++] = static_cast<uint8_t>(step);
        i = j;
    }
    return t;
}

// A song from its tracks, which must be the same length; if they aren't, or
// one has a bad step, its steps is 0 (check with valid())
constexpr Song song(uint16_t bpm, const ParsedTrack &lead, const ParsedTrack &arp,
                    const ParsedTrack &bass, const ParsedTrack &drums,
                    const DrumKit &kit = kDrumKit) {
    Song s;
    s.bpm = bpm;
    const bool ok = lead.count > 0 && lead.count == arp.count && lead.count == bass.count &&
                    lead.count == drums.count;
    s.steps = ok ? static_cast<uint16_t>(lead.count) : 0;
    s.tracks = {lead.steps, arp.steps, bass.steps, drums.steps};
    s.drums = &kit;
    return s;
}

constexpr bool valid(const Song &s) { return s.steps > 0 && s.bpm > 0; }

} // namespace sfx

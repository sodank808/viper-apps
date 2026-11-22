#pragma once

// sfx::Synth: sound effects (sfx/sound.hpp) and music (sfx/song.hpp), mixed
// into an audio tap's buffer. Blocks' synthesizer, shared.
//
//     sfx::Synth<4> synth;                        // 4 effect voices (2 by default)
//     synth.start(viper_audio_sample_rate());     // before installing the tap
//     viper_audio_tap_install(
//         [](int16_t *s, uint32_t n, void *u) { static_cast<sfx::Synth<4> *>(u)->render(s, n); },
//         &synth);
//     synth.play(kJump);                          // from loop()
//     synth.music(&kTitle);
//
// Two threads use it. The app calls start() before the audio task can render,
// then play(), music() and the setters; the audio task calls render(). They
// share only 32-bit values and pointers that one side writes and the other
// reads (no read-modify-write, which the ESP32-S3 can't do atomically in
// PSRAM, where an ELF app's data lives), and a small queue of effects.
//
// No heap, no floating point and no 64-bit division: an ELF app can only call
// what the firmware exports. Sounds and songs are referred to, not copied, so
// they must outlive the synth (const tables do).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>

#include "sfx/clip.hpp"
#include "sfx/song.hpp"
#include "sfx/sound.hpp"

namespace sfx {

// Each music voice's amplitude at full volume, and effects' by default. All of
// them at once stay under int16's limit, before the system's volume scales the
// mix.
inline constexpr int kLeadLevel = 2400;
inline constexpr int kArpLevel = 1100;
inline constexpr int kBassLevel = 3800;
inline constexpr int kDrumLevel = 2600;
inline constexpr int kEffectLevel = 4200;

// 2^(n/12) x 1024, for n = 0 .. 11
inline constexpr uint16_t kSemitoneScale[12] = {1024, 1085, 1149, 1218, 1290, 1367,
                                                1448, 1534, 1625, 1722, 1825, 1933};

// sin(2 pi n / 64) x 127, for vibrato
inline constexpr int8_t kSine64[64] = {
    0,    12,   25,   37,   49,   60,   71,   81,  90,  98,  106,  112,  117,  122,  125,  126,
    127,  126,  125,  122,  117,  112,  106,  98,  90,  81,  71,   60,   49,   37,   25,   12,
    0,    -12,  -25,  -37,  -49,  -60,  -71,  -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71,  -60,  -49,  -37,  -25,  -12};

template <int EffectVoices = 2> class Synth {
    static_assert(EffectVoices >= 1 && EffectVoices <= 8);

  public:
    // Tempo: 256 is the song's own, 320 a quarter faster
    static constexpr uint32_t kTempoNormal = 256;
    static constexpr int kMaxSemitones = 36;

    // App side

    // Silence everything and get ready to render at sample_rate, with effects
    // at effect_level at full volume. Call while the tap isn't installed.
    void start(uint32_t sample_rate, int effect_level = kEffectLevel) {
        rate_ = sample_rate ? sample_rate : 48000;
        per_hz_ = 0xFFFFFFFFu / rate_;
        updates_per_second_ = std::max<uint32_t>(1, rate_ / kControlFrames);
        lfo_per_hz_ = 0xFFFFFFFFu / updates_per_second_;
        effect_level_ = effect_level;
        for (int n = 0; n < 128; n++) {
            // Hundredths of a hertz, in two parts so nothing overflows
            const uint32_t centi = std::min(kNoteCentiHz[n], rate_ / 2 * 100);
            note_steps_[n] = centi / 100 * per_hz_ + centi % 100 * per_hz_ / 100;
        }
        app_flags_ = kMusicOn | kEffectsOn;
        flags_.store(app_flags_);
        tempo_.store(kTempoNormal);
        music_count_ = 0;
        music_song_.store(nullptr);
        music_command_.store(0);
        seen_music_ = 0;
        queue_head_.store(0);
        queue_tail_.store(0);
        app_head_ = 0;
        song_ = nullptr;
        step_ = 0;
        until_step_ = 0;
        steps_played_ = 0;
        channels_ = {};
        channels_[kTrackLead].osc.wave = Wave::Pulse25;
        channels_[kTrackLead].level = kLeadLevel;
        channels_[kTrackLead].floor = 150;
        channels_[kTrackLead].decay = 2;
        channels_[kTrackArp].osc.wave = Wave::Pulse12;
        channels_[kTrackArp].level = kArpLevel;
        channels_[kTrackArp].floor = 50;
        channels_[kTrackArp].decay = 5;
        channels_[kTrackBass].osc.wave = Wave::Triangle;
        channels_[kTrackBass].level = kBassLevel;
        channels_[kTrackBass].floor = 256;
        drums_ = {};
        effects_ = {};
        effect_order_ = 0;
        until_control_ = 0;
    }

    // Play s, `semitones` (0 to kMaxSemitones) above its own pitch. Dropped if
    // effects are off, or if the queue is full (the audio task isn't running).
    void play(const Sound &s, int semitones = 0) {
        if (app_head_ - queue_tail_.load() >= kQueueSize) {
            return;
        }
        push(Queued{&s, nullptr, static_cast<uint8_t>(std::clamp(semitones, 0, kMaxSemitones)), 255,
                    false});
    }

    // Play a clip (sfx/clip.hpp, made at this synth's sample rate) on an
    // effect voice, at volume (255: as recorded). It plays from the clip's
    // memory: keep the clip alive and unchanged while it can play.
    void play(const Clip &c, uint8_t volume = 255) { push(Queued{nullptr, &c, 0, volume, false}); }

    // Stop s wherever it's playing: how a looped sound ends. Switching
    // effects off stops looped sounds too.
    void stop(const Sound &s) { push(Queued{&s, nullptr, 0, 0, true}); }
    void stop(const Clip &c) { push(Queued{nullptr, &c, 0, 0, true}); }

    // Start s from its beginning; nullptr stops the music
    void music(const Song *s) {
        music_song_.store(s);
        music_command_.store(++music_count_);
    }

    void set_tempo(uint32_t tempo) { tempo_.store(std::clamp<uint32_t>(tempo, 128, 512)); }

    // The music stops where it is while paused, and goes on after. Effects
    // still play.
    void set_paused(bool paused) {
        set_flags(paused ? app_flags_ | kPaused : app_flags_ & ~kPaused);
    }

    void set_enabled(bool music, bool effects) {
        set_flags((app_flags_ & kPaused) | (music ? kMusicOn : 0u) | (effects ? kEffectsOn : 0u));
    }

    // Audio side: add `frames` stereo frames into samples
    void render(int16_t *samples, uint32_t frames) {
        take_commands();
        for (uint32_t i = 0; i < frames; i++) {
            if (until_control_ == 0) {
                control();
                until_control_ = kControlFrames;
            }
            until_control_--;
            const uint32_t flags = flags_.load(std::memory_order_relaxed);
            int mix = 0;
            if (song_ && (flags & kMusicOn) && !(flags & kPaused)) {
                mix += channels_[0].osc.sample() + channels_[1].osc.sample() +
                       channels_[2].osc.sample() + drums_.osc.sample();
            }
            for (int v = 0; v < EffectVoices; v++) {
                if (effects_[v].clip) {
                    mix += effects_[v].clip_sample();
                } else if (effects_[v].sound) {
                    mix += effects_[v].osc.sample();
                }
            }
            if (mix != 0) {
                for (int ch = 0; ch < 2; ch++) {
                    int16_t &out = samples[2 * i + ch];
                    out = static_cast<int16_t>(std::clamp(out + mix, -32768, 32767));
                }
            }
        }
    }

    // For tests (audio side)
    uint32_t steps_played() const { return steps_played_; }
    int effects_playing() const {
        int n = 0;
        for (const Player &p : effects_) {
            n += p.busy();
        }
        return n;
    }

  private:
    // Samples between control updates (pitch slides, envelopes, steps)
    static constexpr uint32_t kControlFrames = 32;
    static constexpr uint32_t kQueueSize = 16;

    static constexpr uint32_t kMusicOn = 1 << 0;
    static constexpr uint32_t kEffectsOn = 1 << 1;
    static constexpr uint32_t kPaused = 1 << 2;

    struct Oscillator {
        uint32_t phase = 0;
        uint32_t step = 0; // phase increment a sample
        uint16_t lfsr = 1;
        Wave wave = Wave::Pulse50;
        int amplitude = 0;

        int sample() {
            const uint32_t before = phase;
            phase += step;
            switch (wave) {
            case Wave::Pulse12:
                return phase < 0x20000000u ? amplitude : -amplitude;
            case Wave::Pulse25:
                return phase < 0x40000000u ? amplitude : -amplitude;
            case Wave::Pulse50:
                return phase < 0x80000000u ? amplitude : -amplitude;
            case Wave::Triangle: {
                const int p = static_cast<int>(phase >> 20); // 0-4095
                const int t = p < 2048 ? p : 4095 - p;       // 0-2047
                return (t - 1024) * amplitude / 1024;
            }
            case Wave::Noise:
                if (phase < before) { // a new value each time the phase wraps
                    const uint16_t bit = (lfsr ^ (lfsr >> 1)) & 1;
                    lfsr = static_cast<uint16_t>((lfsr >> 1) | (bit << 14));
                }
                return (lfsr & 1) ? amplitude : -amplitude;
            case Wave::Sine: {
                // The vibrato table, with a straight line between entries
                const int i = static_cast<int>(phase >> 26);
                const int frac = static_cast<int>((phase >> 16) & 0x3FF); // 0-1023
                const int a = kSine64[i], b = kSine64[(i + 1) & 63];
                // In 1/(127 x 128)ths, so any int16 amplitude fits in 32 bits
                return ((a * 1024 + (b - a) * frac) >> 3) * amplitude / (127 * 128);
            }
            }
            return 0;
        }
    };

    // Plays a Sound (effects and drums) or a Clip (effects)
    struct Player {
        Oscillator osc;
        const Sound *sound = nullptr; // nullptr when idle
        const Clip *clip = nullptr;   // instead of a sound
        const int16_t *clip_data = nullptr;
        uint32_t clip_size = 0, clip_pos = 0;
        int clip_volume = 255;
        int segment = 0;
        uint32_t pos = 0, len = 0; // in control updates
        int level = 0;             // amplitude at full volume
        uint32_t shift = 1024;     // pitch, x 1024
        uint32_t started = 0;      // which started last, among equals
        uint32_t lfo = 0;          // vibrato's phase

        bool busy() const { return sound || clip; }
        uint8_t priority() const { return sound ? sound->priority : 0; }

        void begin_clip(const Clip &c, uint8_t volume, uint32_t order) {
            sound = nullptr;
            clip = &c;
            clip_data = c.samples.data();
            clip_size = static_cast<uint32_t>(c.samples.size());
            clip_pos = 0;
            clip_volume = volume;
            started = order;
            osc.amplitude = 0;
        }

        int clip_sample() {
            const int v = clip_data[clip_pos] * clip_volume / 255;
            if (++clip_pos >= clip_size) {
                clip = nullptr;
            }
            return v;
        }

        void begin(const Sound &s, int lvl, uint32_t order, uint8_t semitones = 0) {
            clip = nullptr;
            sound = &s;
            segment = 0;
            pos = 0;
            len = 0;
            level = lvl;
            shift = static_cast<uint32_t>(kSemitoneScale[semitones % 12]) << (semitones / 12);
            started = order;
            lfo = 0;
            osc.amplitude = 0;
        }

        void control(const Synth &synth) {
            while (sound) {
                const Segment &g = sound->segments[segment];
                if (len == 0) {
                    len = std::max<uint32_t>(1, g.ms * synth.updates_per_second_ / 1000);
                }
                if (pos >= len) {
                    segment++;
                    pos = 0;
                    len = 0;
                    if (segment >= sound->count) {
                        if (sound->loops) {
                            segment = 0;
                        } else {
                            sound = nullptr;
                            osc.amplitude = 0;
                        }
                    }
                    continue;
                }
                const int at = static_cast<int>(pos);
                const int span = static_cast<int>(len);
                int hz = g.from_hz + (g.to_hz - g.from_hz) * at / span;
                const int volume = g.from_volume + (g.to_volume - g.from_volume) * at / span;
                if (sound->vibrato_hz) {
                    // x (1 + percent/100 x sin), in 1024ths
                    const int sine = kSine64[lfo >> 26];
                    hz = hz * (1024 + sound->vibrato_percent * sine * 1024 / (100 * 127)) / 1024;
                    lfo += sound->vibrato_hz * synth.lfo_per_hz_;
                }
                osc.wave = g.wave;
                osc.step = synth.hz_step(shift == 1024 ? static_cast<uint32_t>(hz)
                                                       : static_cast<uint32_t>(hz) * shift / 1024);
                osc.amplitude = level * volume / 255;
                pos++;
                return;
            }
        }
    };

    // A pulse or triangle channel of the music
    struct Channel {
        Oscillator osc;
        int envelope = 0; // 0-256
        int floor = 0;    // where it decays to
        int decay = 0;    // a control update
        int level = 0;
        bool releasing = false;

        void control() {
            if (releasing) {
                envelope = std::max(0, envelope - 64);
            } else if (envelope > floor) {
                envelope = std::max(floor, envelope - decay);
            }
            osc.amplitude = level * envelope / 256;
        }
    };

    struct Queued {
        const Sound *sound = nullptr;
        const Clip *clip = nullptr;
        uint8_t semitones = 0;
        uint8_t volume = 255; // a clip's
        bool stop = false;
    };

    // Dropped if the queue is full (the audio task isn't running)
    void push(const Queued &q) {
        if (app_head_ - queue_tail_.load() >= kQueueSize) {
            return;
        }
        queue_[app_head_ % kQueueSize] = q;
        app_head_++;
        queue_head_.store(app_head_);
    }

    void set_flags(uint32_t flags) {
        if (flags != app_flags_) {
            app_flags_ = flags;
            flags_.store(flags);
        }
    }

    uint32_t hz_step(uint32_t hz) const { return std::min(hz, rate_ - 1) * per_hz_; }

    void note_on(int track, uint8_t note) {
        if (track == kTrackDrums) {
            if (note >= kKick && note < kDrumEnd) {
                drums_.begin((*song_->drums)[note], kDrumLevel, 0);
            }
            return;
        }
        Channel &c = channels_[track];
        if (note == kOff) {
            c.releasing = true;
        } else if (note != kHold) {
            c.osc.step = note_steps_[note & 127];
            c.envelope = 256;
            c.releasing = false;
        }
    }

    void advance_step() {
        for (int t = 0; t < kTracks; t++) {
            note_on(t, song_->tracks[t][step_]);
        }
        step_ = (step_ + 1) % song_->steps;
        steps_played_++;
    }

    void start_effect(const Queued &q) {
        // A clip has priority 0 and no channel
        const uint8_t priority = q.sound ? q.sound->priority : 0;
        const uint8_t channel = q.sound ? q.sound->channel : 0;
        // A free voice, or the least important one playing (the older of two
        // equals), if it's no more important than this
        int pick = -1;
        for (int i = 0; channel && i < EffectVoices; i++) {
            if (effects_[i].sound && effects_[i].sound->channel == channel) {
                pick = i;
                break;
            }
        }
        for (int i = 0; pick < 0 && i < EffectVoices; i++) {
            const Player &p = effects_[i];
            if (!p.busy()) {
                pick = i;
                break;
            }
        }
        if (pick < 0) {
            for (int i = 0; i < EffectVoices; i++) {
                const Player &p = effects_[i];
                if (p.priority() <= priority &&
                    (pick < 0 || p.priority() < effects_[pick].priority() ||
                     (p.priority() == effects_[pick].priority() &&
                      p.started < effects_[pick].started))) {
                    pick = i;
                }
            }
        }
        if (pick < 0) {
            return;
        }
        if (q.clip) {
            effects_[pick].begin_clip(*q.clip, q.volume, ++effect_order_);
        } else {
            effects_[pick].begin(*q.sound, effect_level_, ++effect_order_, q.semitones);
        }
    }

    void take_commands() {
        const uint32_t command = music_command_.load();
        if (command != seen_music_) {
            seen_music_ = command;
            const Song *s = music_song_.load();
            song_ = s && valid(*s) ? s : nullptr;
            step_ = 0;
            until_step_ = 0;
            for (Channel &c : channels_) {
                c.envelope = 0;
                c.releasing = false;
                c.osc.amplitude = 0;
            }
            drums_.sound = nullptr;
            drums_.osc.amplitude = 0;
        }
        const bool effects = flags_.load() & kEffectsOn;
        const uint32_t head = queue_head_.load();
        uint32_t tail = queue_tail_.load();
        while (tail != head) {
            const Queued &q = queue_[tail % kQueueSize];
            if (q.stop) {
                for (Player &p : effects_) {
                    if (q.sound && p.sound == q.sound) {
                        p.sound = nullptr;
                        p.osc.amplitude = 0;
                    }
                    if (q.clip && p.clip == q.clip) {
                        p.clip = nullptr;
                    }
                }
            } else if (effects &&
                       ((q.sound && q.sound->count > 0) || (q.clip && !q.clip->empty()))) {
                start_effect(q);
            }
            tail++;
        }
        queue_tail_.store(tail);
        if (!effects) {
            for (Player &p : effects_) {
                if (p.sound && p.sound->loops) {
                    p.sound = nullptr;
                    p.osc.amplitude = 0;
                }
            }
        }
    }

    void control() {
        const uint32_t flags = flags_.load();
        if (song_ && (flags & kMusicOn) && !(flags & kPaused)) {
            until_step_ -= static_cast<int32_t>(kControlFrames);
            if (until_step_ <= 0) {
                // A sixteenth note: rate * 60 / (bpm * 4) samples, over the tempo
                const uint32_t samples =
                    rate_ * 15 * kTempoNormal / (song_->bpm * std::max<uint32_t>(1, tempo_.load()));
                while (until_step_ <= 0) {
                    advance_step();
                    until_step_ +=
                        static_cast<int32_t>(std::max<uint32_t>(samples, kControlFrames));
                }
            }
        }
        for (Channel &c : channels_) {
            c.control();
        }
        drums_.control(*this);
        for (Player &p : effects_) {
            p.control(*this);
        }
    }

    // Written by the app, read by the audio task
    std::atomic<uint32_t> music_command_{0}; // how many times music() was called
    std::atomic<const Song *> music_song_{nullptr};
    std::atomic<uint32_t> tempo_{kTempoNormal};
    std::atomic<uint32_t> flags_{0};
    std::array<Queued, kQueueSize> queue_{};
    std::atomic<uint32_t> queue_head_{0}; // written by the app
    std::atomic<uint32_t> queue_tail_{0}; // written by the audio task
    uint32_t app_head_ = 0;               // the app's copy of queue_head_
    uint32_t music_count_ = 0;            // the app's own
    uint32_t app_flags_ = 0;              // the app's copy of flags_

    // The audio task's (and start()'s)
    uint32_t rate_ = 48000;
    uint32_t per_hz_ = 0; // phase step for 1 Hz
    uint32_t updates_per_second_ = 1500;
    uint32_t lfo_per_hz_ = 0; // vibrato's phase step a control update, for 1 Hz
    int effect_level_ = kEffectLevel;
    std::array<uint32_t, 128> note_steps_{};
    uint32_t seen_music_ = 0;
    const Song *song_ = nullptr;
    int step_ = 0;
    int32_t until_step_ = 0; // samples
    uint32_t steps_played_ = 0;
    std::array<Channel, 3> channels_{};
    Player drums_;
    std::array<Player, EffectVoices> effects_{};
    uint32_t effect_order_ = 0;
    uint32_t until_control_ = 0;
};

} // namespace sfx

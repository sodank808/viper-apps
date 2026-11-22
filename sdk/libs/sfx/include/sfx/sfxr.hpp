#pragma once

// sfxr: DrPetter's sound effect generator (and jsfxr's presets), rendered into
// an sfx::Clip for sfx::Synth to play. A sound is 23 parameters, from a
// preset or set by hand; render_sfxr() runs sfxr's synthesis at its own
// 44.1 kHz and resamples to the rate you ask for.
//
//     sfx::sfxr::Rng rng{seed};
//     const sfx::Clip coin = sfx::render_sfxr(sfx::sfxr::preset(sfx::sfxr::Preset::PickupCoin,
//     rng),
//                                             synth_rate);
//     synth.play(coin);
//
// Rendering uses floating point and the heap (the clip, and 4 KB for sfxr's
// phaser), so do it when loading, not on the audio task. ELF apps can: it
// calls only powf and sinf from libm, which the firmware exports.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>

#include "sfx/clip.hpp"

namespace sfx {
namespace sfxr {

enum class Wave : uint8_t { Square, Sawtooth, Sine, Noise };

// sfxr's parameters, by its own names. Most are 0 to 1; the ramps and a few
// others -1 to 1.
struct Params {
    Wave wave_type = Wave::Square;

    float p_env_attack = 0.0f;  // attack time
    float p_env_sustain = 0.3f; // sustain time
    float p_env_punch = 0.0f;   // sustain punch
    float p_env_decay = 0.4f;   // decay time

    float p_base_freq = 0.3f;  // start frequency
    float p_freq_limit = 0.0f; // minimum frequency: the sound ends there
    float p_freq_ramp = 0.0f;  // slide
    float p_freq_dramp = 0.0f; // delta slide

    float p_vib_strength = 0.0f; // vibrato depth
    float p_vib_speed = 0.0f;    // vibrato speed

    float p_arp_mod = 0.0f;   // change amount
    float p_arp_speed = 0.0f; // change speed

    float p_duty = 0.0f;      // square duty
    float p_duty_ramp = 0.0f; // duty sweep

    float p_repeat_speed = 0.0f;

    float p_pha_offset = 0.0f; // phaser offset
    float p_pha_ramp = 0.0f;   // phaser sweep

    float p_lpf_freq = 1.0f; // low-pass cutoff (1: off)
    float p_lpf_ramp = 0.0f;
    float p_lpf_resonance = 0.0f;

    float p_hpf_freq = 0.0f; // high-pass cutoff
    float p_hpf_ramp = 0.0f;

    float sound_vol = 0.5f;
};

// A small random number generator (xorshift32), so a seed makes the same
// sound every time
struct Rng {
    uint32_t state = 0x9E3779B9u;

    explicit Rng(uint32_t seed = 1)
        : state(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    // In [0, range)
    float frnd(float range) { return static_cast<float>(next() >> 8) * (range / 16777216.0f); }
    // In [0, max]
    int rnd(int max) { return static_cast<int>(next() % static_cast<uint32_t>(max + 1)); }
};

// sfxr's buttons
enum class Preset : uint8_t {
    PickupCoin,
    LaserShoot,
    Explosion,
    PowerUp,
    HitHurt,
    Jump,
    BlipSelect,
    Synth, // jsfxr's
    Tone,  // jsfxr's: a steady sine
    Click, // jsfxr's
    Random,
    Count,
};

inline const char *name(Preset p) {
    switch (p) {
    case Preset::PickupCoin:
        return "Pickup/Coin";
    case Preset::LaserShoot:
        return "Laser/Shoot";
    case Preset::Explosion:
        return "Explosion";
    case Preset::PowerUp:
        return "Power Up";
    case Preset::HitHurt:
        return "Hit/Hurt";
    case Preset::Jump:
        return "Jump";
    case Preset::BlipSelect:
        return "Blip/Select";
    case Preset::Synth:
        return "Synth";
    case Preset::Tone:
        return "Tone";
    case Preset::Click:
        return "Click";
    case Preset::Random:
        return "Random";
    case Preset::Count:
        break;
    }
    return "Unknown";
}

// A new random variation of a preset, as sfxr's buttons make
inline Params preset(Preset preset, Rng &rng) {
    Params p;
    auto frnd = [&](float range) { return rng.frnd(range); };
    auto rnd = [&](int max) { return rng.rnd(max); };
    auto sq = [](float x) { return x * x; };
    auto cube = [](float x) { return x * x * x; };
    switch (preset) {
    case Preset::PickupCoin:
        p.p_base_freq = 0.4f + frnd(0.5f);
        p.p_env_attack = 0.0f;
        p.p_env_sustain = frnd(0.1f);
        p.p_env_decay = 0.1f + frnd(0.4f);
        p.p_env_punch = 0.3f + frnd(0.3f);
        if (rnd(1)) {
            p.p_arp_speed = 0.5f + frnd(0.2f);
            p.p_arp_mod = 0.2f + frnd(0.4f);
        }
        break;
    case Preset::LaserShoot:
        p.wave_type = static_cast<Wave>(rnd(2));
        if (p.wave_type == Wave::Sine && rnd(1)) {
            p.wave_type = static_cast<Wave>(rnd(1));
        }
        p.p_base_freq = 0.5f + frnd(0.5f);
        p.p_freq_limit = std::max(p.p_base_freq - 0.2f - frnd(0.6f), 0.2f);
        p.p_freq_ramp = -0.15f - frnd(0.2f);
        if (rnd(2) == 0) {
            p.p_base_freq = 0.3f + frnd(0.6f);
            p.p_freq_limit = frnd(0.1f);
            p.p_freq_ramp = -0.35f - frnd(0.3f);
        }
        if (rnd(1)) {
            p.p_duty = frnd(0.5f);
            p.p_duty_ramp = frnd(0.2f);
        } else {
            p.p_duty = 0.4f + frnd(0.5f);
            p.p_duty_ramp = -frnd(0.7f);
        }
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.1f + frnd(0.2f);
        p.p_env_decay = frnd(0.4f);
        if (rnd(1)) {
            p.p_env_punch = frnd(0.3f);
        }
        if (rnd(2) == 0) {
            p.p_pha_offset = frnd(0.2f);
            p.p_pha_ramp = -frnd(0.2f);
        }
        if (rnd(1)) {
            p.p_hpf_freq = frnd(0.3f);
        }
        break;
    case Preset::Explosion:
        p.wave_type = Wave::Noise;
        if (rnd(1)) {
            p.p_base_freq = 0.1f + frnd(0.4f);
            p.p_freq_ramp = -0.1f + frnd(0.4f);
        } else {
            p.p_base_freq = 0.2f + frnd(0.7f);
            p.p_freq_ramp = -0.2f - frnd(0.2f);
        }
        p.p_base_freq *= p.p_base_freq;
        if (rnd(4) == 0) {
            p.p_freq_ramp = 0.0f;
        }
        if (rnd(2) == 0) {
            p.p_repeat_speed = 0.3f + frnd(0.5f);
        }
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.1f + frnd(0.3f);
        p.p_env_decay = frnd(0.5f);
        if (rnd(1) == 0) {
            p.p_pha_offset = -0.3f + frnd(0.9f);
            p.p_pha_ramp = -frnd(0.3f);
        }
        p.p_env_punch = 0.2f + frnd(0.6f);
        if (rnd(1)) {
            p.p_vib_strength = frnd(0.7f);
            p.p_vib_speed = frnd(0.6f);
        }
        if (rnd(2) == 0) {
            p.p_arp_speed = 0.6f + frnd(0.3f);
            p.p_arp_mod = 0.8f - frnd(1.6f);
        }
        break;
    case Preset::PowerUp:
        if (rnd(1)) {
            p.wave_type = Wave::Sawtooth;
        } else {
            p.p_duty = frnd(0.6f);
        }
        if (rnd(1)) {
            p.p_base_freq = 0.2f + frnd(0.3f);
            p.p_freq_ramp = 0.1f + frnd(0.4f);
            p.p_repeat_speed = 0.4f + frnd(0.4f);
        } else {
            p.p_base_freq = 0.2f + frnd(0.3f);
            p.p_freq_ramp = 0.05f + frnd(0.2f);
            if (rnd(1)) {
                p.p_vib_strength = frnd(0.7f);
                p.p_vib_speed = frnd(0.6f);
            }
        }
        p.p_env_attack = 0.0f;
        p.p_env_sustain = frnd(0.4f);
        p.p_env_decay = 0.1f + frnd(0.4f);
        break;
    case Preset::HitHurt:
        p.wave_type = static_cast<Wave>(rnd(2));
        if (p.wave_type == Wave::Sine) {
            p.wave_type = Wave::Noise;
        }
        if (p.wave_type == Wave::Square) {
            p.p_duty = frnd(0.6f);
        }
        p.p_base_freq = 0.2f + frnd(0.6f);
        p.p_freq_ramp = -0.3f - frnd(0.4f);
        p.p_env_attack = 0.0f;
        p.p_env_sustain = frnd(0.1f);
        p.p_env_decay = 0.1f + frnd(0.2f);
        if (rnd(1)) {
            p.p_hpf_freq = frnd(0.3f);
        }
        break;
    case Preset::Jump:
        p.wave_type = Wave::Square;
        p.p_duty = frnd(0.6f);
        p.p_base_freq = 0.3f + frnd(0.3f);
        p.p_freq_ramp = 0.1f + frnd(0.2f);
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.1f + frnd(0.3f);
        p.p_env_decay = 0.1f + frnd(0.2f);
        if (rnd(1)) {
            p.p_hpf_freq = frnd(0.3f);
        }
        if (rnd(1)) {
            p.p_lpf_freq = 1.0f - frnd(0.6f);
        }
        break;
    case Preset::BlipSelect:
        p.wave_type = static_cast<Wave>(rnd(1));
        if (p.wave_type == Wave::Square) {
            p.p_duty = frnd(0.6f);
        }
        p.p_base_freq = 0.2f + frnd(0.4f);
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.1f + frnd(0.1f);
        p.p_env_decay = frnd(0.2f);
        p.p_hpf_freq = 0.1f;
        break;
    case Preset::Synth:
        p.wave_type = static_cast<Wave>(rnd(1));
        if (p.wave_type == Wave::Square) {
            p.p_duty = frnd(0.6f);
        }
        p.p_base_freq = 0.3f + frnd(0.5f);
        p.p_env_attack = frnd(0.3f);
        p.p_env_sustain = frnd(0.2f);
        p.p_env_decay = 0.2f + frnd(0.3f);
        if (rnd(1)) {
            p.p_vib_strength = frnd(0.7f);
            p.p_vib_speed = frnd(0.6f);
        }
        if (rnd(2) == 0) {
            p.p_lpf_freq = 1.0f - frnd(0.5f);
            p.p_lpf_ramp = frnd(0.2f) - 0.1f;
        }
        break;
    case Preset::Tone:
        p.wave_type = Wave::Sine;
        p.p_base_freq = 0.35f;
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.6641f;
        p.p_env_decay = 0.0f;
        break;
    case Preset::Click:
        p.wave_type = Wave::Noise;
        p.p_base_freq = 0.5f;
        p.p_env_attack = 0.0f;
        p.p_env_sustain = 0.0f;
        p.p_env_decay = 0.02f;
        break;
    case Preset::Random:
    case Preset::Count:
        p.p_base_freq = sq(frnd(2.0f) - 1.0f);
        if (rnd(1)) {
            p.p_base_freq = cube(frnd(2.0f) - 1.0f) + 0.5f;
        }
        p.p_freq_limit = 0.0f;
        {
            const float x = frnd(2.0f) - 1.0f;
            p.p_freq_ramp = x * x * x * x * x;
        }
        if (p.p_base_freq > 0.7f && p.p_freq_ramp > 0.2f) {
            p.p_freq_ramp = -p.p_freq_ramp;
        }
        if (p.p_base_freq < 0.2f && p.p_freq_ramp < -0.05f) {
            p.p_freq_ramp = -p.p_freq_ramp;
        }
        p.p_freq_dramp = cube(frnd(2.0f) - 1.0f);
        p.p_duty = frnd(2.0f) - 1.0f;
        p.p_duty_ramp = cube(frnd(2.0f) - 1.0f);
        p.p_vib_strength = cube(frnd(2.0f) - 1.0f);
        p.p_vib_speed = frnd(2.0f) - 1.0f;
        p.p_env_attack = cube(frnd(2.0f) - 1.0f);
        p.p_env_sustain = sq(frnd(2.0f) - 1.0f);
        p.p_env_decay = frnd(2.0f) - 1.0f;
        p.p_env_punch = sq(frnd(0.8f));
        if (p.p_env_attack + p.p_env_sustain + p.p_env_decay < 0.2f) {
            p.p_env_sustain += 0.2f + frnd(0.3f);
            p.p_env_decay += 0.2f + frnd(0.3f);
        }
        p.p_lpf_resonance = frnd(2.0f) - 1.0f;
        p.p_lpf_freq = 1.0f - cube(frnd(1.0f));
        p.p_lpf_ramp = cube(frnd(2.0f) - 1.0f);
        if (p.p_lpf_freq < 0.1f && p.p_lpf_ramp < -0.05f) {
            p.p_lpf_ramp = -p.p_lpf_ramp;
        }
        {
            const float x = frnd(1.0f);
            p.p_hpf_freq = x * x * x * x * x;
            const float y = frnd(2.0f) - 1.0f;
            p.p_hpf_ramp = y * y * y * y * y;
        }
        p.p_pha_offset = cube(frnd(2.0f) - 1.0f);
        p.p_pha_ramp = cube(frnd(2.0f) - 1.0f);
        p.p_repeat_speed = frnd(2.0f) - 1.0f;
        p.p_arp_speed = frnd(2.0f) - 1.0f;
        p.p_arp_mod = frnd(2.0f) - 1.0f;
        break;
    }
    return p;
}

// Nudge the parameters like sfxr's Mutate button: each has an even chance of
// moving by up to +/-amount. Like sfxr, it leaves the frequency cutoff and
// the volume alone, and doesn't clamp.
inline void mutate(Params &p, Rng &rng, float amount = 0.05f) {
    for (float *value :
         {&p.p_base_freq,    &p.p_freq_ramp,     &p.p_freq_dramp, &p.p_duty,         &p.p_duty_ramp,
          &p.p_vib_strength, &p.p_vib_speed,     &p.p_env_attack, &p.p_env_sustain,  &p.p_env_decay,
          &p.p_env_punch,    &p.p_lpf_resonance, &p.p_lpf_freq,   &p.p_lpf_ramp,     &p.p_hpf_freq,
          &p.p_hpf_ramp,     &p.p_pha_offset,    &p.p_pha_ramp,   &p.p_repeat_speed, &p.p_arp_speed,
          &p.p_arp_mod}) {
        if (rng.next() & 1) {
            *value += rng.frnd(2 * amount) - amount;
        }
    }
}

// sfxr's synthesis, a sample at a time at its own rate (kNativeRate): its
// SynthSample() and ResetSample()
class Generator {
  public:
    static constexpr uint32_t kNativeRate = 44100;

    Generator(const Params &params, uint32_t seed = 1)
        : params_(params)
        , rng_(seed) {
        reset_sample(false);
    }

    // The next sample, -1 to 1, or false once the sound has ended
    bool next(float &out) {
        if (finished_) {
            return false;
        }

        rep_time_++;
        if (rep_limit_ != 0 && rep_time_ >= rep_limit_) {
            rep_time_ = 0;
            reset_sample(true);
        }

        // Frequency envelopes and arpeggio (which fires once)
        arp_time_++;
        if (arp_limit_ != 0 && arp_time_ >= arp_limit_) {
            arp_limit_ = 0;
            fperiod_ *= arp_mod_;
        }
        fslide_ += fdslide_;
        fperiod_ *= fslide_;
        if (fperiod_ > fmaxperiod_) {
            fperiod_ = fmaxperiod_;
            if (params_.p_freq_limit > 0.0f) {
                finished_ = true;
                return false;
            }
        }
        float rfperiod = fperiod_;
        if (vib_amp_ > 0.0f) {
            vib_phase_ += vib_speed_;
            rfperiod = fperiod_ * (1.0f + std::sin(vib_phase_) * vib_amp_);
        }
        period_ = static_cast<int>(rfperiod);
        if (period_ < 8) {
            period_ = 8;
        }
        square_duty_ = std::clamp(square_duty_ + square_slide_, 0.0f, 0.5f);

        // Volume envelope
        env_time_++;
        if (env_time_ > env_length_[env_stage_]) {
            env_time_ = 0;
            env_stage_++;
            if (env_stage_ == 3) {
                finished_ = true;
                return false;
            }
        }
        // A zero-length stage is left on the next sample; don't divide by zero meanwhile
        const float stage_pos =
            static_cast<float>(env_time_) / std::max<uint32_t>(env_length_[env_stage_], 1);
        if (env_stage_ == 0) {
            env_vol_ = stage_pos;
        } else if (env_stage_ == 1) {
            env_vol_ = 1.0f + (1.0f - stage_pos) * 2.0f * params_.p_env_punch;
        } else {
            env_vol_ = 1.0f - stage_pos;
        }

        // Phaser step
        fphase_ += fdphase_;
        iphase_ = std::min(std::abs(static_cast<int>(fphase_)), 1023);

        if (flthp_d_ != 0.0f) {
            flthp_ = std::clamp(flthp_ * flthp_d_, 0.00001f, 0.1f);
        }

        // 8x supersampling
        float ssample = 0.0f;
        for (int si = 0; si < 8; si++) {
            float sample = 0.0f;
            phase_++;
            if (phase_ >= period_) {
                phase_ %= period_;
                if (params_.wave_type == Wave::Noise) {
                    for (float &n : noise_buffer_) {
                        n = rng_.frnd(2.0f) - 1.0f;
                    }
                }
            }

            const float fp = static_cast<float>(phase_) / period_;
            switch (params_.wave_type) {
            case Wave::Square:
                sample = fp < square_duty_ ? 0.5f : -0.5f;
                break;
            case Wave::Sawtooth:
                sample = 1.0f - fp * 2.0f;
                break;
            case Wave::Sine:
                sample = std::sin(fp * kTwoPi);
                break;
            case Wave::Noise:
                sample = noise_buffer_[phase_ * 32 / period_];
                break;
            }

            // Low-pass filter (bypassed at full cutoff)
            const float pp = fltp_;
            fltw_ = std::clamp(fltw_ * fltw_d_, 0.0f, 0.1f);
            if (params_.p_lpf_freq != 1.0f) {
                fltdp_ += (sample - fltp_) * fltw_;
                fltdp_ -= fltdp_ * fltdmp_;
            } else {
                fltp_ = sample;
                fltdp_ = 0.0f;
            }
            fltp_ += fltdp_;

            // High-pass filter
            fltphp_ += fltp_ - pp;
            fltphp_ -= fltphp_ * flthp_;
            sample = fltphp_;

            // Phaser
            pha_buffer_[ipp_ & 1023] = sample;
            sample += pha_buffer_[(ipp_ - iphase_ + 1024) & 1023];
            ipp_ = (ipp_ + 1) & 1023;

            ssample += sample * env_vol_;
        }
        ssample = ssample / 8.0f * 2.0f * params_.sound_vol;
        out = std::clamp(ssample, -1.0f, 1.0f);
        return true;
    }

    // The longest it can be, in native samples (it can end sooner)
    uint32_t max_length() const { return env_length_[0] + env_length_[1] + env_length_[2] + 3; }

  private:
    static constexpr float kTwoPi = 3.14159265359f * 2.0f;

    // A restart (from the repeat parameter) only resets the pitch; the
    // envelope keeps running, so the sound still ends
    void reset_sample(bool restart) {
        if (!restart) {
            phase_ = 0;
        }
        fperiod_ = 100.0f / (params_.p_base_freq * params_.p_base_freq + 0.001f);
        period_ = static_cast<int>(fperiod_);
        fmaxperiod_ = 100.0f / (params_.p_freq_limit * params_.p_freq_limit + 0.001f);
        fslide_ = 1.0f - std::pow(params_.p_freq_ramp, 3.0f) * 0.01f;
        fdslide_ = -std::pow(params_.p_freq_dramp, 3.0f) * 0.000001f;
        square_duty_ = 0.5f - params_.p_duty * 0.5f;
        square_slide_ = -params_.p_duty_ramp * 0.00005f;
        if (params_.p_arp_mod >= 0.0f) {
            arp_mod_ = 1.0f - params_.p_arp_mod * params_.p_arp_mod * 0.9f;
        } else {
            arp_mod_ = 1.0f + params_.p_arp_mod * params_.p_arp_mod * 10.0f;
        }
        arp_time_ = 0;
        arp_limit_ = static_cast<int>(std::pow(1.0f - params_.p_arp_speed, 2.0f) * 20000 + 32);
        if (params_.p_arp_speed == 1.0f) {
            arp_limit_ = 0;
        }
        if (restart) {
            return;
        }

        // Filters
        fltp_ = 0.0f;
        fltdp_ = 0.0f;
        fltw_ = std::pow(params_.p_lpf_freq, 3.0f) * 0.1f;
        fltw_d_ = 1.0f + params_.p_lpf_ramp * 0.0001f;
        fltdmp_ = 5.0f / (1.0f + params_.p_lpf_resonance * params_.p_lpf_resonance * 20.0f) *
                  (0.01f + fltw_);
        if (fltdmp_ > 0.8f) {
            fltdmp_ = 0.8f;
        }
        fltphp_ = 0.0f;
        flthp_ = params_.p_hpf_freq * params_.p_hpf_freq * 0.1f;
        flthp_d_ = 1.0f + params_.p_hpf_ramp * 0.0003f;

        // Vibrato
        vib_phase_ = 0.0f;
        vib_speed_ = params_.p_vib_speed * params_.p_vib_speed * 0.01f;
        vib_amp_ = params_.p_vib_strength * 0.5f;

        // Envelope
        env_vol_ = 0.0f;
        env_stage_ = 0;
        env_time_ = 0;
        env_length_[0] =
            static_cast<uint32_t>(params_.p_env_attack * params_.p_env_attack * 100000.0f);
        env_length_[1] =
            static_cast<uint32_t>(params_.p_env_sustain * params_.p_env_sustain * 100000.0f);
        env_length_[2] =
            static_cast<uint32_t>(params_.p_env_decay * params_.p_env_decay * 100000.0f);

        // Phaser
        fphase_ = params_.p_pha_offset * params_.p_pha_offset * 1020.0f;
        if (params_.p_pha_offset < 0.0f) {
            fphase_ = -fphase_;
        }
        fdphase_ = params_.p_pha_ramp * params_.p_pha_ramp;
        if (params_.p_pha_ramp < 0.0f) {
            fdphase_ = -fdphase_;
        }
        iphase_ = std::abs(static_cast<int>(fphase_));
        ipp_ = 0;
        std::fill(std::begin(pha_buffer_), std::end(pha_buffer_), 0.0f);

        for (float &n : noise_buffer_) {
            n = rng_.frnd(2.0f) - 1.0f;
        }

        // Repeat
        rep_time_ = 0;
        rep_limit_ = static_cast<int>(std::pow(1.0f - params_.p_repeat_speed, 2.0f) * 20000 + 32);
        if (params_.p_repeat_speed == 0.0f) {
            rep_limit_ = 0;
        }
    }

    Params params_;
    Rng rng_;
    bool finished_ = false;

    int phase_ = 0;
    int period_ = 0;
    float fperiod_ = 0, fmaxperiod_ = 0, fslide_ = 0, fdslide_ = 0;
    float square_duty_ = 0, square_slide_ = 0;
    int arp_time_ = 0, arp_limit_ = 0;
    float arp_mod_ = 0;
    float vib_phase_ = 0, vib_speed_ = 0, vib_amp_ = 0;
    int rep_time_ = 0, rep_limit_ = 0;
    uint32_t env_stage_ = 0, env_time_ = 0;
    uint32_t env_length_[3] = {};
    float env_vol_ = 0;
    float fphase_ = 0, fdphase_ = 0;
    int iphase_ = 0, ipp_ = 0;
    float pha_buffer_[1024] = {};
    float fltp_ = 0, fltdp_ = 0, fltw_ = 0, fltw_d_ = 0, fltdmp_ = 0;
    float fltphp_ = 0, flthp_ = 0, flthp_d_ = 0;
    float noise_buffer_[32] = {}; // noise held for a period, so noise has a pitch
};

} // namespace sfxr

// Render an sfxr sound into a clip at sample_rate (resampled in a straight
// line from sfxr's 44.1 kHz, so it keeps its pitch and length). The seed picks
// the noise. Empty if the sound is silent from the start.
inline Clip render_sfxr(const sfxr::Params &params, uint32_t sample_rate, uint32_t seed = 1) {
    Clip clip;
    clip.sample_rate = sample_rate ? sample_rate : sfxr::Generator::kNativeRate;
    // 4 KB of phaser: on the heap, not the stack
    auto gen = std::make_unique<sfxr::Generator>(params, seed);
    const double step = static_cast<double>(sfxr::Generator::kNativeRate) / clip.sample_rate;
    clip.samples.reserve(static_cast<size_t>(gen->max_length() / step) + 2);

    // Two native samples at a time: a = native[index], b = native[index + 1]
    // (or a again past the end)
    float a = 0, b = 0;
    if (!gen->next(a)) {
        return clip;
    }
    bool more = gen->next(b);
    if (!more) {
        b = a;
    }
    size_t index = 0;
    for (size_t i = 0;; ++i) {
        const double pos = i * step;
        const auto want = static_cast<size_t>(pos);
        while (index < want) {
            if (!more) {
                return clip; // past the last native sample
            }
            a = b;
            more = gen->next(b);
            if (!more) {
                b = a;
            }
            index++;
        }
        if (!more && pos > static_cast<double>(index)) {
            return clip; // past the last native sample (a)
        }
        const float frac = static_cast<float>(pos - want);
        const float value = a + (b - a) * frac;
        clip.samples.push_back(static_cast<int16_t>(value * 32767.0f));
    }
}

} // namespace sfx

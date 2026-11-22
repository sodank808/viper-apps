#pragma once

// DSP building blocks shared by the voice models. Coefficients are computed
// from the sample rate when a voice is set up; the per-sample code uses only
// multiplies and adds (plus a polynomial sine), so it is cheap on the
// ESP32-S3's single-precision FPU and gives the same result for the same seed.

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace drum::dsp {

inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kTwoPi = 6.28318530717959f;

inline float db_to_gain(float db) { return std::pow(10.0f, db / 20.0f); }

inline float semitones_to_ratio(float st) { return std::exp2(st / 12.0f); }

inline float lerp(float a, float b, float t) { return a + (b - a) * t; }

/// Per-sample multiplier that falls 60 dB in `seconds`.
inline float decay_coef(float seconds, float sample_rate) {
    return std::exp(-6.9077553f / std::max(seconds * sample_rate, 1.0f));
}

/// Per-sample multiplier for an exponential with time constant `seconds`.
inline float tau_coef(float seconds, float sample_rate) {
    return std::exp(-1.0f / std::max(seconds * sample_rate, 1.0f));
}

/// sin(2 pi phase) for phase in [0, 1), to about 4e-6.
inline float sin2pi(float phase) {
    float t = phase - 0.5f; // sin(2 pi (t + 0.5)) = -sin(2 pi t)
    if (t > 0.25f) {
        t = 0.5f - t;
    } else if (t < -0.25f) {
        t = -0.5f - t;
    }
    const float x = kTwoPi * t;
    const float x2 = x * x;
    const float s =
        x * (1.0f + x2 * (-1.0f / 6.0f +
                          x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
    return -s;
}

/// tanh-like soft clip: odd, monotonic, reaches exactly +-1 at +-3.
inline float soft_clip(float x) {
    x = std::clamp(x, -3.0f, 3.0f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}

/// Hashes a seed so nearby seeds give unrelated streams.
inline uint32_t mix_seed(uint32_t seed) {
    uint32_t z = seed + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x85EBCA6Bu;
    z = (z ^ (z >> 13)) * 0xC2B2AE35u;
    z ^= z >> 16;
    return z ? z : 1u;
}

/// White noise in [-1, 1), xorshift32.
class Noise {
  public:
    explicit Noise(uint32_t seed = 1)
        : state_(mix_seed(seed)) {}
    float next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<float>(static_cast<int32_t>(state_)) * (1.0f / 2147483648.0f);
    }
    /// Uniform in [0, 1)
    float unit() { return next() * 0.5f + 0.5f; }

  private:
    uint32_t state_;
};

/// State-variable filter (Simper's trapezoidal SVF): low, band and high pass
/// from one state.
class Svf {
  public:
    struct Out {
        float lp;
        float bp; ///< peak gain Q at the center
        float hp;
        float bp_unity; ///< band pass with unity gain at the center
    };

    void set(float cutoff_hz, float q, float sample_rate) {
        cutoff_hz = std::clamp(cutoff_hz, 10.0f, 0.45f * sample_rate);
        const float g = std::tan(kPi * cutoff_hz / sample_rate);
        k_ = 1.0f / std::max(q, 0.05f);
        a1_ = 1.0f / (1.0f + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    Out process(float v0) {
        const float v3 = v0 - ic2_;
        const float v1 = a1_ * ic1_ + a2_ * v3;
        const float v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0f * v1 - ic1_;
        ic2_ = 2.0f * v2 - ic2_;
        return {v2, v1, v0 - k_ * v1 - v2, k_ * v1};
    }

  private:
    float k_ = 1.0f, a1_ = 0.0f, a2_ = 0.0f, a3_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

/// One-pole DC blocker.
class DcBlocker {
  public:
    void set(float cutoff_hz, float sample_rate) {
        r_ = 1.0f - std::min(kTwoPi * cutoff_hz / sample_rate, 0.5f);
    }
    float process(float x) {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }

  private:
    float r_ = 0.995f, x1_ = 0.0f, y1_ = 0.0f;
};

/// Sine oscillator; frequency can change every sample.
class SineOsc {
  public:
    float next(float increment) {
        const float s = sin2pi(phase_);
        phase_ += increment;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }
        return s;
    }

  private:
    float phase_ = 0.0f;
};

/// Band-limited square wave (PolyBLEP), so the metallic voices don't alias at
/// low output rates.
class SquareOsc {
  public:
    void set(float hz, float sample_rate, float phase) {
        increment_ = std::min(hz / sample_rate, 0.45f);
        phase_ = phase - std::floor(phase);
    }

    float next() {
        float v = phase_ < 0.5f ? 1.0f : -1.0f;
        v += blep(phase_);
        float shifted = phase_ + 0.5f;
        if (shifted >= 1.0f) {
            shifted -= 1.0f;
        }
        v -= blep(shifted);
        phase_ += increment_;
        if (phase_ >= 1.0f) {
            phase_ -= 1.0f;
        }
        return v;
    }

  private:
    float blep(float t) const {
        const float dt = increment_;
        if (t < dt) {
            t /= dt;
            return t + t - t * t - 1.0f;
        }
        if (t > 1.0f - dt) {
            t = (t - 1.0f) / dt;
            return t * t + t + t + 1.0f;
        }
        return 0.0f;
    }

    float phase_ = 0.0f;
    float increment_ = 0.0f;
};

} // namespace drum::dsp

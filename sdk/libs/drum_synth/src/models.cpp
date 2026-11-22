// The voice models: parameter tables and the synthesis for each.
//
// These are recipes informed by the TR-808 service notes, not circuit models.
// Changing what a model outputs for the same parameters means bumping its
// version in kModels.

#include "dsp.hpp"
#include "voice.hpp"

#include <algorithm>
#include <cmath>

namespace drum {

namespace {

using namespace dsp;

constexpr ParamInfo kKickParams[] = {
    {"tune", "Tune", Unit::Hertz, 30.0f, 120.0f, 50.0f},
    {"decay", "Decay", Unit::Seconds, 0.05f, 3.0f, 0.8f},
    {"sweep", "Sweep", Unit::Semitones, 0.0f, 36.0f, 14.0f},
    {"sweep_time", "Sweep time", Unit::Seconds, 0.002f, 0.2f, 0.03f},
    {"click", "Click", Unit::Amount, 0.0f, 1.0f, 0.4f},
    {"drive", "Drive", Unit::Amount, 0.0f, 1.0f, 0.2f},
};
static_assert(std::size(kKickParams) == kick::Count);

constexpr ParamInfo kSnareParams[] = {
    {"tune", "Tune", Unit::Hertz, 100.0f, 400.0f, 185.0f},
    {"tone", "Tone", Unit::Amount, 0.0f, 1.0f, 0.5f},
    {"body_decay", "Body decay", Unit::Seconds, 0.02f, 1.0f, 0.15f},
    {"noise_decay", "Noise decay", Unit::Seconds, 0.02f, 1.5f, 0.25f},
    {"snappy", "Snappy", Unit::Amount, 0.0f, 1.0f, 0.6f},
};
static_assert(std::size(kSnareParams) == snare::Count);

constexpr ParamInfo kHatParams[] = {
    {"tune", "Tune", Unit::Semitones, -12.0f, 12.0f, 0.0f},
    {"decay", "Decay", Unit::Seconds, 0.01f, 3.0f, 0.07f},
    {"tone", "Tone", Unit::Amount, 0.0f, 1.0f, 0.5f},
    {"noise", "Noise", Unit::Amount, 0.0f, 1.0f, 0.15f},
};
static_assert(std::size(kHatParams) == hat::Count);

constexpr ParamInfo kClapParams[] = {
    {"tone", "Tone", Unit::Hertz, 500.0f, 4000.0f, 1100.0f},
    {"spread", "Spread", Unit::Seconds, 0.004f, 0.03f, 0.011f},
    {"bursts", "Bursts", Unit::Count, 1.0f, 6.0f, 4.0f},
    {"decay", "Decay", Unit::Seconds, 0.05f, 2.0f, 0.3f},
    {"width", "Width", Unit::Amount, 0.0f, 1.0f, 0.5f},
};
static_assert(std::size(kClapParams) == clap::Count);

constexpr ModelInfo kModels[] = {
    {Model::Kick, "kick", "Kick", 1, kKickParams},
    {Model::Snare, "snare", "Snare", 1, kSnareParams},
    {Model::Hat, "hat", "Hi-hat", 1, kHatParams},
    {Model::Clap, "clap", "Clap", 1, kClapParams},
};
static_assert(std::size(kModels) == static_cast<size_t>(Model::Count));

// Each voice also seeds its noise and phases from the render seed mixed with
// its own constant, so two models never share a stream.

class KickVoice final : public Voice {
  public:
    KickVoice(const VoiceParams &p, float accent, uint32_t seed, float sr)
        : noise_(seed ^ 0x4B49434Bu) {
        inv_sr_ = 1.0f / sr;
        tune_ = p.get(kick::Tune);
        sweep_ = semitones_to_ratio(p.get(kick::Sweep)) - 1.0f;
        const float sweep_time = p.get(kick::SweepTime);
        pitch_coef_ = tau_coef(sweep_time, sr);
        amp_coef_ = decay_coef(p.get(kick::Decay), sr);
        click_level_ = p.get(kick::Click) * (0.6f + 0.4f * accent);
        click_coef_ = tau_coef(0.0012f, sr);
        click_filter_.set(2200.0f, 0.5f, sr);
        drive_ = 1.0f + 6.0f * p.get(kick::Drive);
        drive_norm_ = 1.0f / soft_clip(drive_);
        excitation_frames_ = static_cast<size_t>((5.0f * sweep_time + 0.005f) * sr);
    }

    float calibration() const override { return 0.68f; }

    void process(float *out, size_t frames) override {
        for (size_t i = 0; i < frames; i++) {
            const float increment = tune_ * (1.0f + sweep_ * pitch_env_) * inv_sr_;
            pitch_env_ *= pitch_coef_;
            float body = osc_.next(increment) * amp_;
            amp_ *= amp_coef_;
            body = soft_clip(body * drive_) * drive_norm_;

            const float click = click_filter_.process(click_env_ * (0.4f + noise_.next())).bp_unity;
            click_env_ *= click_coef_;
            out[i] = body + click * click_level_;
        }
    }

  private:
    Noise noise_;
    SineOsc osc_;
    Svf click_filter_;
    float inv_sr_ = 0.0f;
    float tune_ = 0.0f, sweep_ = 0.0f;
    float pitch_env_ = 1.0f, pitch_coef_ = 0.0f;
    float amp_ = 1.0f, amp_coef_ = 0.0f;
    float click_env_ = 1.0f, click_coef_ = 0.0f, click_level_ = 0.0f;
    float drive_ = 1.0f, drive_norm_ = 1.0f;
};

class SnareVoice final : public Voice {
  public:
    SnareVoice(const VoiceParams &p, float accent, uint32_t seed, float sr)
        : noise_(seed ^ 0x534E4152u) {
        inv_sr_ = 1.0f / sr;
        const float tone = p.get(snare::Tone);
        f1_ = p.get(snare::Tune);
        f2_ = f1_ * 1.8f;
        pitch_coef_ = tau_coef(0.004f, sr);
        const float body_decay = p.get(snare::BodyDecay);
        amp1_coef_ = decay_coef(body_decay, sr);
        amp2_coef_ = decay_coef(body_decay * 0.6f, sr);
        level1_ = lerp(1.0f, 0.5f, tone);
        level2_ = lerp(0.35f, 0.8f, tone);
        hp_.set(1000.0f, 0.7f, sr);
        lp_.set(lerp(4000.0f, 11000.0f, tone), 0.7f, sr);
        attack_frames_ = std::max<size_t>(1, static_cast<size_t>(0.0005f * sr));
        noise_coef_ = decay_coef(p.get(snare::NoiseDecay), sr);
        snappy_ = p.get(snare::Snappy) * (0.8f + 0.2f * accent);
        excitation_frames_ = static_cast<size_t>(0.005f * sr);
    }

    float calibration() const override { return 0.5f; }

    void process(float *out, size_t frames) override {
        for (size_t i = 0; i < frames; i++) {
            const float bend = 1.0f + 0.3f * pitch_env_;
            pitch_env_ *= pitch_coef_;
            const float body = osc1_.next(f1_ * bend * inv_sr_) * amp1_ * level1_ +
                               osc2_.next(f2_ * bend * inv_sr_) * amp2_ * level2_;
            amp1_ *= amp1_coef_;
            amp2_ *= amp2_coef_;

            float env;
            if (frame_ < attack_frames_) {
                env = static_cast<float>(frame_) / static_cast<float>(attack_frames_);
            } else {
                env = noise_env_;
                noise_env_ *= noise_coef_;
            }
            frame_++;
            const float noise = lp_.process(hp_.process(noise_.next()).hp).lp;
            out[i] = body * 0.8f + noise * env * snappy_ * 1.2f;
        }
    }

  private:
    Noise noise_;
    SineOsc osc1_, osc2_;
    Svf hp_, lp_;
    float inv_sr_ = 0.0f;
    float f1_ = 0.0f, f2_ = 0.0f;
    float pitch_env_ = 1.0f, pitch_coef_ = 0.0f;
    float amp1_ = 1.0f, amp1_coef_ = 0.0f, amp2_ = 1.0f, amp2_coef_ = 0.0f;
    float level1_ = 0.0f, level2_ = 0.0f;
    size_t frame_ = 0, attack_frames_ = 1;
    float noise_env_ = 1.0f, noise_coef_ = 0.0f, snappy_ = 0.0f;
};

class HatVoice final : public Voice {
  public:
    // The 808's six hat/cymbal oscillators
    static constexpr float kFrequencies[6] = {205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f};

    HatVoice(const VoiceParams &p, float accent, uint32_t seed, float sr)
        : noise_(seed ^ 0x48415453u) {
        const float ratio = semitones_to_ratio(p.get(hat::Tune));
        for (size_t i = 0; i < 6; i++) {
            oscs_[i].set(kFrequencies[i] * ratio, sr, noise_.unit());
        }
        const float tone = std::clamp(p.get(hat::Tone) + 0.1f * (accent - 0.5f), 0.0f, 1.0f);
        const float noise = p.get(hat::Noise);
        // White noise has about 14 dB more energy in the hat's band than the
        // oscillators, so scale it to match them at the same setting
        noise_level_ = 0.2f * noise;
        metal_level_ = (1.0f - 0.6f * noise) / 6.0f;
        bp_.set(lerp(6500.0f, 10500.0f, tone), 1.0f, sr);
        hp_.set(lerp(5000.0f, 8000.0f, tone), 0.7f, sr);
        attack_frames_ = std::max<size_t>(1, static_cast<size_t>(0.0003f * sr));
        decay_coef_ = decay_coef(p.get(hat::Decay), sr);
        excitation_frames_ = attack_frames_;
    }

    float calibration() const override { return 4.5f; }

    void process(float *out, size_t frames) override {
        for (size_t i = 0; i < frames; i++) {
            float metal = 0.0f;
            for (auto &osc : oscs_) {
                metal += osc.next();
            }
            const float source = metal * metal_level_ + noise_.next() * noise_level_;

            float env;
            if (frame_ < attack_frames_) {
                env = static_cast<float>(frame_) / static_cast<float>(attack_frames_);
            } else {
                env = env_;
                env_ *= decay_coef_;
            }
            frame_++;
            out[i] = hp_.process(bp_.process(source).bp_unity * env).hp;
        }
    }

  private:
    Noise noise_;
    SquareOsc oscs_[6];
    Svf bp_, hp_;
    float noise_level_ = 0.0f, metal_level_ = 0.0f;
    size_t frame_ = 0, attack_frames_ = 1;
    float env_ = 1.0f, decay_coef_ = 0.0f;
};

class ClapVoice final : public Voice {
  public:
    ClapVoice(const VoiceParams &p, float /*accent*/, uint32_t seed, float sr)
        : noise_(seed ^ 0x434C4150u) {
        const float tone = p.get(clap::Tone);
        const float spread = p.get(clap::Spread);
        const float width = p.get(clap::Width);
        bursts_ = static_cast<size_t>(std::lround(p.get(clap::Bursts)));
        spread_frames_ = std::max<size_t>(1, static_cast<size_t>(spread * sr));
        burst_coef_ = tau_coef(spread * 0.3f, sr);
        tail_coef_ = decay_coef(p.get(clap::Decay), sr);
        tail_start_ = (bursts_ - 1) * spread_frames_;
        burst_filter_.set(tone, lerp(3.0f, 0.8f, width), sr);
        tail_filter_.set(tone * 0.85f, lerp(1.6f, 0.6f, width), sr);
        excitation_frames_ = tail_start_ + spread_frames_;
    }

    float calibration() const override { return 1.45f; }

    void process(float *out, size_t frames) override {
        for (size_t i = 0; i < frames; i++) {
            if (frame_ % spread_frames_ == 0 && frame_ / spread_frames_ < bursts_) {
                burst_env_ = 1.0f;
            }
            if (frame_ == tail_start_) {
                tail_env_ = 0.55f;
            }
            frame_++;
            const float x = noise_.next();
            out[i] = burst_filter_.process(x).bp_unity * burst_env_ +
                     tail_filter_.process(x).bp_unity * tail_env_;
            burst_env_ *= burst_coef_;
            tail_env_ *= tail_coef_;
        }
    }

  private:
    Noise noise_;
    Svf burst_filter_, tail_filter_;
    size_t bursts_ = 1, spread_frames_ = 1, tail_start_ = 0, frame_ = 0;
    float burst_env_ = 0.0f, burst_coef_ = 0.0f;
    float tail_env_ = 0.0f, tail_coef_ = 0.0f;
};

} // namespace

const ModelInfo &model_info(Model model) {
    const auto index = static_cast<size_t>(model);
    return kModels[index < std::size(kModels) ? index : 0];
}

const ModelInfo *find_model(std::string_view id) {
    for (const auto &info : kModels) {
        if (id == info.id) {
            return &info;
        }
    }
    return nullptr;
}

VoiceParams VoiceParams::defaults(Model model) {
    VoiceParams voice;
    voice.model = model_info(model).model;
    const auto params = voice.info().params;
    for (size_t i = 0; i < params.size(); i++) {
        voice.values[i] = params[i].def;
    }
    return voice;
}

bool VoiceParams::set(size_t index, float value) {
    const auto params = info().params;
    if (index >= params.size() || !std::isfinite(value)) {
        return false;
    }
    values[index] = std::clamp(value, params[index].min, params[index].max);
    return true;
}

bool VoiceParams::set(std::string_view id, float value) {
    const auto params = info().params;
    for (size_t i = 0; i < params.size(); i++) {
        if (id == params[i].id) {
            return set(i, value);
        }
    }
    return false;
}

void VoiceParams::clamp() {
    model = model_info(model).model;
    const auto params = info().params;
    for (size_t i = 0; i < kMaxParams; i++) {
        if (i >= params.size()) {
            values[i] = 0.0f;
        } else if (!std::isfinite(values[i])) {
            values[i] = params[i].def;
        } else {
            values[i] = std::clamp(values[i], params[i].min, params[i].max);
        }
    }
}

std::unique_ptr<Voice> make_voice(const VoiceParams &params, float accent, uint32_t seed,
                                  float sample_rate) {
    VoiceParams p = params;
    p.clamp();
    accent = std::clamp(accent, 0.0f, 1.0f);
    switch (p.model) {
    case Model::Kick:
        return std::make_unique<KickVoice>(p, accent, seed, sample_rate);
    case Model::Snare:
        return std::make_unique<SnareVoice>(p, accent, seed, sample_rate);
    case Model::Hat:
        return std::make_unique<HatVoice>(p, accent, seed, sample_rate);
    case Model::Clap:
        return std::make_unique<ClapVoice>(p, accent, seed, sample_rate);
    case Model::Count:
        break;
    }
    return nullptr;
}

} // namespace drum

#pragma once

#include "drum_synth.hpp"

namespace drum {

/// A model's running state for one render. Output is roughly +-1 at the
/// model's nominal level before the renderer's gain.
class Voice {
  public:
    virtual ~Voice() = default;
    virtual void process(float *out, size_t frames) = 0;
    /// The sound can't end (by going quiet) before this many frames.
    size_t excitation_frames() const { return excitation_frames_; }
    /// Gain that brings the model's default preset to the kit's nominal level.
    virtual float calibration() const = 0;

  protected:
    size_t excitation_frames_ = 0;
};

std::unique_ptr<Voice> make_voice(const VoiceParams &params, float accent, uint32_t seed,
                                  float sample_rate);

} // namespace drum

#pragma once

// sfx::Clip: a recorded sound, mono int16 samples at the rate it was made for,
// that sfx::Synth plays as an effect (`synth.play(clip)`). render_sfxr()
// (sfx/sfxr.hpp) makes them.
//
// The synth plays a clip from its memory, so keep the clip alive and
// unchanged while it can play: until it ends, or until the audio task can't
// call render() any more. To change one that may be playing, remove the tap
// first (`sfx::Tap::remove()` or `viper_audio_tap_remove()`; both wait for
// the audio task), then install it again.

#include <cstdint>
#include <vector>

namespace sfx {

struct Clip {
    std::vector<int16_t> samples; // mono
    uint32_t sample_rate = 0;

    bool empty() const { return samples.empty(); }
    uint32_t length_ms() const {
        return sample_rate ? static_cast<uint32_t>(samples.size() * 1000 / sample_rate) : 0;
    }
};

} // namespace sfx

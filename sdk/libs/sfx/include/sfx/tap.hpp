#pragma once

// sfx::Tap: mixes a built-in app's sound into the audio engine, from install()
// until remove() or the Tap's destructor. ELF apps don't use it; they call
// viper_audio_tap_install() (see sfx/synth.hpp).
//
//     sfx::Synth<4> synth_;
//     sfx::Tap tap_; // after the synth, so it's removed before the synth goes
//
//     void onCreate() { tap_.install(this, synth_); } // starts the synth too
//     uint32_t onLoop(...) { synth_.play(kJump); ... }
//
// Or any function that adds into the buffer:
//     tap_.install(this, [this](int16_t *stereo, size_t frames) { mix(stereo, frames); });
//
// Unlike the rest of sfx, this needs the firmware (System and the audio
// engine), so only built-in apps can include it.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

#include "audio_engine.hpp"
#include "system.hpp"

#include "sfx/synth.hpp"

namespace sfx {

class Tap {
  public:
    using Mix = std::function<void(int16_t *stereo, size_t frames)>;

    Tap() = default;
    Tap(const Tap &) = delete;
    Tap &operator=(const Tap &) = delete;
    ~Tap() { remove(); }

    // The rate the engine plays at, for anything that renders by itself
    static uint32_t sample_rate() { return System::get().audio_sample_rate(); }

    // Start the synth at the engine's rate, with effects at effect_level, and
    // mix it in. `owner` names the tap to the engine; an app passes `this`.
    // The engine removes taps by owner, so give each Tap its own.
    template <int N>
    bool install(espp::BaseComponent *owner, Synth<N> &synth, int effect_level = kEffectLevel) {
        remove();
        synth.start(sample_rate(), effect_level);
        return install(owner, [&synth](int16_t *stereo, size_t frames) {
            synth.render(stereo, static_cast<uint32_t>(frames));
        });
    }

    // Call mix on the audio task with each block, to add into
    bool install(espp::BaseComponent *owner, Mix mix) {
        remove();
        auto engine = System::get().audio_engine();
        if (!engine || !owner) {
            return false;
        }
        if (!engine->install_tap(audio::AudioEngine::AudioNode::INPUT, owner,
                                 [mix = std::move(mix)](int16_t *stereo, size_t frames) {
                                     mix(stereo, frames);
                                     return true;
                                 })) {
            return false;
        }
        owner_ = owner;
        return true;
    }

    // Stop mixing. Once it returns, the audio task won't call in again.
    void remove() {
        if (owner_) {
            if (auto engine = System::get().audio_engine()) {
                engine->remove_tap(audio::AudioEngine::AudioNode::INPUT, owner_);
            }
            owner_ = nullptr;
        }
    }

    bool installed() const { return owner_ != nullptr; }

  private:
    espp::BaseComponent *owner_ = nullptr;
};

} // namespace sfx

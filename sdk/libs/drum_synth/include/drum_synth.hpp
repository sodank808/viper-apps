#pragma once

// Percussion synthesis: renders parameterized drum voices to mono int16 PCM.
//
// Portable C++ (standard library only), so it builds for the device and the
// host simulator. Rendering allocates nothing per sample and runs in blocks,
// so a low-priority task can render a voice a piece at a time while the audio
// engine keeps playing. See docs/plans/drum_machine.md and this component's
// README.md.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace drum {

/// Bump when a change to the renderer (not one model) changes rendered audio.
inline constexpr uint16_t kRendererVersion = 1;

/// A signal structure. A preset picks a model and its parameter values.
enum class Model : uint8_t {
    Kick,  ///< damped sine with pitch sweep, click and drive
    Snare, ///< two tonal modes plus filtered noise
    Hat,   ///< six square oscillators through band-pass and high-pass
    Clap,  ///< band-passed noise in repeated bursts, then a tail
    Count,
};

/// Parameter indices for each model, in VoiceParams::values order.
namespace kick {
enum : uint8_t { Tune, Decay, Sweep, SweepTime, Click, Drive, Count };
}
namespace snare {
enum : uint8_t { Tune, Tone, BodyDecay, NoiseDecay, Snappy, Count };
}
namespace hat {
enum : uint8_t { Tune, Decay, Tone, Noise, Count };
}
namespace clap {
enum : uint8_t { Tone, Spread, Bursts, Decay, Width, Count };
}

enum class Unit : uint8_t {
    Hertz,
    Seconds,
    Semitones,
    Amount, ///< 0..1
    Count,  ///< a whole number (rounded)
};

struct ParamInfo {
    const char *id;   ///< stable name used in preset text, e.g. "decay"
    const char *name; ///< for the UI
    Unit unit;
    float min;
    float max;
    float def;
};

struct ModelInfo {
    Model model;
    const char *id;   ///< stable name used in preset text, e.g. "kick"
    const char *name; ///< for the UI
    uint16_t version; ///< bump when a change alters this model's output
    std::span<const ParamInfo> params;
};

const ModelInfo &model_info(Model model);
/// The model whose id is `id`, or nullptr.
const ModelInfo *find_model(std::string_view id);

inline constexpr size_t kMaxParams = 8;

/// One voice's model and parameter values, in the model's units. Setters clamp
/// to the model's range, so a VoiceParams is always valid to render.
struct VoiceParams {
    Model model = Model::Kick;
    std::array<float, kMaxParams> values{};

    static VoiceParams defaults(Model model);

    const ModelInfo &info() const { return model_info(model); }
    float get(size_t index) const { return index < kMaxParams ? values[index] : 0.0f; }
    /// Sets a parameter, clamped to its range. False if the index is out of
    /// range or the value isn't finite (the old value stays).
    bool set(size_t index, float value);
    /// The same, by parameter id.
    bool set(std::string_view id, float value);
    /// Clamps every value into range and resets non-finite ones to defaults.
    void clamp();

    bool operator==(const VoiceParams &) const = default;
};

/// Preset text: the model id, then `param=value` pairs, e.g.
/// "kick tune=50 decay=0.8". Parameters left out keep their defaults; values
/// are clamped.
std::string format_voice(const VoiceParams &voice);
/// Parses preset text into `out`. On failure, returns false, leaves `out`
/// unchanged and describes the problem in `error` (if given).
bool parse_voice(std::string_view text, VoiceParams &out, std::string *error = nullptr);

struct RenderRequest {
    VoiceParams voice;
    float accent = 1.0f;             ///< 0 (normal hit, 6 dB down) .. 1 (accented)
    uint32_t seed = 1;               ///< noise and oscillator phases; same seed, same sound
    uint32_t sample_rate = 32000;    ///< output rate; 8000..96000
    float gain_db = 0.0f;            ///< kit headroom, applied before the limiter
    float max_seconds = 5.0f;        ///< longest output; 0.01..20
    float tail_threshold_db = -72;   ///< the sound ends once it stays below this
    float tail_hold_seconds = 0.05f; ///< for this long, after the excitation ends
};

enum class RenderStatus : uint8_t {
    Ok,
    InvalidRequest, ///< sample rate, duration or threshold out of range
    NonFinite,      ///< the model produced NaN or infinity (a bug)
};

const char *to_string(RenderStatus status);

struct RenderResult {
    RenderStatus status = RenderStatus::Ok;
    size_t frames = 0;          ///< length of the sound (the quiet tail is trimmed)
    int16_t peak = 0;           ///< largest absolute sample
    bool truncated = false;     ///< hit max_seconds before the tail ended (and faded out)
    size_t limited_samples = 0; ///< samples the limiter had to bring under full scale
};

class Voice; // a model's running state (internal)

/// Renders one voice in pieces. Construct it, size a buffer for capacity()
/// frames, and call process() until done(). Stop calling to cancel.
class Renderer {
  public:
    explicit Renderer(const RenderRequest &request);
    ~Renderer();
    Renderer(const Renderer &) = delete;
    Renderer &operator=(const Renderer &) = delete;

    /// Frames the output can take at most (0 for an invalid request).
    size_t capacity() const { return capacity_; }

    /// Renders up to `max_frames` more frames into `out`, which continues
    /// where the previous call stopped (pass `buffer + frames_rendered()`).
    /// Returns the frames written; 0 once done.
    size_t process(int16_t *out, size_t max_frames);

    bool done() const { return done_; }
    size_t frames_rendered() const { return position_; }
    /// 0..1, a conservative estimate (the sound may end early)
    float progress() const;
    /// Final once done(). frames may be less than frames_rendered(): the
    /// quiet tail that ended the sound is trimmed.
    const RenderResult &result() const { return result_; }

  private:
    void finish(size_t frames);

    RenderRequest request_;
    std::unique_ptr<Voice> voice_;
    RenderResult result_;
    size_t capacity_ = 0;
    size_t position_ = 0;
    size_t excitation_frames_ = 0;
    size_t hold_frames_ = 0;
    size_t fade_frames_ = 0;
    size_t quiet_run_ = 0;
    float gain_ = 1.0f;
    float threshold_ = 0.0f;
    bool done_ = false;
};

/// Renders a whole voice into a new vector sized to the sound.
std::vector<int16_t> render(const RenderRequest &request, RenderResult *result = nullptr);

/// A named voice preset.
struct Preset {
    const char *id;
    const char *name;
    VoiceParams voice;
};

/// The 808-style starter kit: kick, snare, closed hat, open hat, clap.
std::span<const Preset> kit_808();

/// A 16-bit PCM WAV file holding interleaved `samples` (frames * channels).
std::vector<uint8_t> encode_wav(std::span<const int16_t> samples, uint16_t channels,
                                uint32_t sample_rate);

} // namespace drum

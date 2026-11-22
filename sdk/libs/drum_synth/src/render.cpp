// The renderer: runs a voice in blocks, ends it once its tail stays quiet,
// fades it if it hits the length limit, limits peaks, and writes int16.

#include "dsp.hpp"
#include "voice.hpp"

#include <algorithm>
#include <cmath>

namespace drum {

namespace {

constexpr size_t kBlockFrames = 64;
constexpr float kFadeSeconds = 0.005f; // fade-out when max_seconds cuts a tail
constexpr float kLimiterKnee = 0.89f;  // -1 dBFS; the limiter is soft above this

bool valid(const RenderRequest &r) {
    return r.sample_rate >= 8000 && r.sample_rate <= 96000 && std::isfinite(r.accent) &&
           std::isfinite(r.gain_db) && r.gain_db >= -60.0f && r.gain_db <= 24.0f &&
           r.max_seconds >= 0.01f && r.max_seconds <= 20.0f && r.tail_threshold_db >= -120.0f &&
           r.tail_threshold_db <= -20.0f && r.tail_hold_seconds >= 0.0f &&
           r.tail_hold_seconds <= 1.0f && r.voice.model < Model::Count;
}

float limit(float x) {
    const float a = std::fabs(x);
    if (a <= kLimiterKnee) {
        return x;
    }
    const float over = (a - kLimiterKnee) / (1.0f - kLimiterKnee);
    const float y = kLimiterKnee + (1.0f - kLimiterKnee) * dsp::soft_clip(over);
    return x < 0.0f ? -y : y;
}

int16_t to_pcm(float x) {
    const float scaled = std::clamp(x * 32767.0f, -32767.0f, 32767.0f);
    return static_cast<int16_t>(std::lrint(scaled));
}

} // namespace

const char *to_string(RenderStatus status) {
    switch (status) {
    case RenderStatus::Ok:
        return "ok";
    case RenderStatus::InvalidRequest:
        return "invalid request";
    case RenderStatus::NonFinite:
        return "non-finite output";
    }
    return "?";
}

Renderer::Renderer(const RenderRequest &request)
    : request_(request) {
    if (!valid(request_)) {
        result_.status = RenderStatus::InvalidRequest;
        done_ = true;
        return;
    }
    const auto sr = static_cast<float>(request_.sample_rate);
    request_.voice.clamp();
    request_.accent = std::clamp(request_.accent, 0.0f, 1.0f);
    voice_ = make_voice(request_.voice, request_.accent, request_.seed, sr);

    capacity_ = std::max<size_t>(1, static_cast<size_t>(request_.max_seconds * sr));
    excitation_frames_ = voice_->excitation_frames();
    hold_frames_ = std::max<size_t>(1, static_cast<size_t>(request_.tail_hold_seconds * sr));
    fade_frames_ = std::min(capacity_, std::max<size_t>(1, static_cast<size_t>(kFadeSeconds * sr)));
    threshold_ = dsp::db_to_gain(request_.tail_threshold_db);
    // A normal hit is 6 dB below an accented one
    gain_ =
        voice_->calibration() * dsp::db_to_gain(request_.gain_db - 6.0f * (1.0f - request_.accent));
}

Renderer::~Renderer() = default;

float Renderer::progress() const {
    if (done_) {
        return 1.0f;
    }
    return capacity_ ? static_cast<float>(position_) / static_cast<float>(capacity_) : 1.0f;
}

void Renderer::finish(size_t frames) {
    result_.frames = frames;
    done_ = true;
}

size_t Renderer::process(int16_t *out, size_t max_frames) {
    size_t written = 0;
    float block[kBlockFrames];
    while (!done_ && written < max_frames) {
        const size_t n = std::min({kBlockFrames, max_frames - written, capacity_ - position_});
        voice_->process(block, n);
        for (size_t i = 0; i < n; i++) {
            float y = block[i] * gain_;
            if (!std::isfinite(y)) {
                result_.status = RenderStatus::NonFinite;
                finish(position_);
                return written;
            }

            // Tail: once the excitation is over, the sound ends after it has
            // stayed below the threshold for the hold time. The quiet stretch
            // itself is trimmed.
            if (position_ >= excitation_frames_) {
                quiet_run_ = std::fabs(y) < threshold_ ? quiet_run_ + 1 : 0;
            }

            const size_t remaining = capacity_ - position_;
            if (remaining <= fade_frames_) {
                y *= static_cast<float>(remaining - 1) / static_cast<float>(fade_frames_);
            }
            if (std::fabs(y) > 1.0f) {
                result_.limited_samples++;
            }
            const int16_t s = to_pcm(limit(y));
            result_.peak = std::max<int16_t>(result_.peak, static_cast<int16_t>(s < 0 ? -s : s));
            out[written++] = s;
            position_++;

            if (quiet_run_ >= hold_frames_) {
                finish(position_ - quiet_run_);
                return written;
            }
        }
        if (position_ >= capacity_) {
            result_.truncated = true;
            finish(position_);
        }
    }
    return written;
}

std::vector<int16_t> render(const RenderRequest &request, RenderResult *result) {
    Renderer renderer(request);
    std::vector<int16_t> pcm(renderer.capacity());
    while (!renderer.done()) {
        renderer.process(pcm.data() + renderer.frames_rendered(), 4096);
    }
    pcm.resize(renderer.result().frames);
    pcm.shrink_to_fit();
    if (result) {
        *result = renderer.result();
    }
    return pcm;
}

} // namespace drum

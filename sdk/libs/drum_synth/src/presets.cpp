// Preset text (format_voice / parse_voice), the 808-style starter kit, and
// WAV encoding.

#include "drum_synth.hpp"

#include <cstdio>
#include <cstdlib>

namespace drum {

namespace {

VoiceParams make(Model model, std::initializer_list<std::pair<const char *, float>> values) {
    auto voice = VoiceParams::defaults(model);
    for (const auto &[id, value] : values) {
        voice.set(id, value);
    }
    return voice;
}

// Built on first use, so it doesn't depend on static initialization order
std::span<const Preset> kit_808_presets() {
    static const Preset kit[] = {
        {"kick", "Kick",
         make(Model::Kick, {{"tune", 50.0f},
                            {"decay", 0.8f},
                            {"sweep", 14.0f},
                            {"sweep_time", 0.03f},
                            {"click", 0.4f},
                            {"drive", 0.2f}})},
        {"snare", "Snare",
         make(Model::Snare, {{"tune", 185.0f},
                             {"tone", 0.5f},
                             {"body_decay", 0.15f},
                             {"noise_decay", 0.25f},
                             {"snappy", 0.6f}})},
        {"closed_hat", "Closed hat",
         make(Model::Hat, {{"tune", 0.0f}, {"decay", 0.07f}, {"tone", 0.5f}, {"noise", 0.15f}})},
        {"open_hat", "Open hat",
         make(Model::Hat, {{"tune", 0.0f}, {"decay", 0.55f}, {"tone", 0.45f}, {"noise", 0.15f}})},
        {"clap", "Clap",
         make(Model::Clap, {{"tone", 1100.0f},
                            {"spread", 0.011f},
                            {"bursts", 4.0f},
                            {"decay", 0.3f},
                            {"width", 0.5f}})},
    };
    return kit;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Splits off the next whitespace-separated token.
std::string_view next_token(std::string_view &text) {
    size_t start = 0;
    while (start < text.size() && is_space(text[start])) {
        start++;
    }
    size_t end = start;
    while (end < text.size() && !is_space(text[end])) {
        end++;
    }
    const auto token = text.substr(start, end - start);
    text.remove_prefix(end);
    return token;
}

bool fail(std::string *error, std::string message) {
    if (error) {
        *error = std::move(message);
    }
    return false;
}

void put16(std::vector<uint8_t> &v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
}

void put32(std::vector<uint8_t> &v, uint32_t x) {
    put16(v, static_cast<uint16_t>(x));
    put16(v, static_cast<uint16_t>(x >> 16));
}

void put_id(std::vector<uint8_t> &v, const char *id) { v.insert(v.end(), id, id + 4); }

} // namespace

std::string format_voice(const VoiceParams &voice) {
    const auto &info = voice.info();
    std::string text = info.id;
    for (size_t i = 0; i < info.params.size(); i++) {
        char value[32];
        std::snprintf(value, sizeof(value), "%g", static_cast<double>(voice.values[i]));
        text += ' ';
        text += info.params[i].id;
        text += '=';
        text += value;
    }
    return text;
}

bool parse_voice(std::string_view text, VoiceParams &out, std::string *error) {
    const auto model_id = next_token(text);
    const auto *info = find_model(model_id);
    if (!info) {
        return fail(error, "unknown model '" + std::string(model_id) + "'");
    }
    auto voice = VoiceParams::defaults(info->model);
    for (auto token = next_token(text); !token.empty(); token = next_token(text)) {
        const auto eq = token.find('=');
        if (eq == std::string_view::npos || eq == 0) {
            return fail(error, "expected param=value, got '" + std::string(token) + "'");
        }
        const auto id = token.substr(0, eq);
        const std::string value_text(token.substr(eq + 1));
        char *end = nullptr;
        const float value = std::strtof(value_text.c_str(), &end);
        if (value_text.empty() || end != value_text.c_str() + value_text.size()) {
            return fail(error, "bad number in '" + std::string(token) + "'");
        }
        bool known = false;
        for (const auto &param : info->params) {
            known = known || id == param.id;
        }
        if (!known) {
            return fail(error,
                        std::string(info->id) + " has no parameter '" + std::string(id) + "'");
        }
        if (!voice.set(id, value)) {
            return fail(error, "bad value in '" + std::string(token) + "'");
        }
    }
    out = voice;
    return true;
}

std::span<const Preset> kit_808() { return kit_808_presets(); }

std::vector<uint8_t> encode_wav(std::span<const int16_t> samples, uint16_t channels,
                                uint32_t sample_rate) {
    const auto data_bytes = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
    const auto block_align = static_cast<uint16_t>(channels * sizeof(int16_t));
    std::vector<uint8_t> wav;
    wav.reserve(44 + data_bytes);
    put_id(wav, "RIFF");
    put32(wav, 36 + data_bytes);
    put_id(wav, "WAVE");
    put_id(wav, "fmt ");
    put32(wav, 16);
    put16(wav, 1); // PCM
    put16(wav, channels);
    put32(wav, sample_rate);
    put32(wav, sample_rate * block_align);
    put16(wav, block_align);
    put16(wav, 16);
    put_id(wav, "data");
    put32(wav, data_bytes);
    for (int16_t s : samples) {
        put16(wav, static_cast<uint16_t>(s));
    }
    return wav;
}

} // namespace drum

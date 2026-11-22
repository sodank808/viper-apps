// Wolfenstein 3D's sound on Viper: the engine's mixer (SD_Mix() in
// engine/id_sd.c) runs on the engine's thread, each time it hands the turn
// to loop(), and fills a ring a few frames ahead; the audio tap reads the
// ring at the device's rate. Only the ring's two positions cross between the
// threads; the mixer, the OPL and the sounds are the engine thread's.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "viper_app.h"
#include "wolf3d_app.h"

#include "engine/dosbox/dbopl.h"
#include "engine/wolf3d_platform.h"

// The engine's mixer, by its prefixed name (engine/wolf_names.h)
extern "C" void wolf_SD_Mix(int16_t *stereo, int frames);

namespace {

// Frames of stereo at WOLF3D_MIX_RATE: 8192 is 370 ms
constexpr uint32_t kRingFrames = 8192;
// How far ahead the engine mixes: ~70 ms, four frames at 60 fps
constexpr uint32_t kAheadFrames = 1536;

struct Sound {
    int16_t ring[kRingFrames * 2] = {};
    std::atomic<uint32_t> write{0}; // frames written, ever (wraps)
    std::atomic<uint32_t> read{0};  // frames read
    uint32_t step = 0;              // ring frames per output frame, 16.16
    uint32_t phase = 0;             // 16.16, between ring frames
    bool tap = false;
    DBOPL::Chip *opl = nullptr;
};

Sound *s = nullptr;

// The audio task: the ring, at the device's rate, added to the block
void tap(int16_t *samples, uint32_t frames, void *) {
    uint32_t read = s->read.load(std::memory_order_relaxed);
    const uint32_t write = s->write.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < frames; i++) {
        if (write - read < 2) {
            break; // ran dry: the engine is busy (loading a level)
        }
        const int16_t *a = &s->ring[(read % kRingFrames) * 2];
        const int16_t *b = &s->ring[((read + 1) % kRingFrames) * 2];
        const int32_t f = s->phase >> 4; // 12 bits
        for (int c = 0; c < 2; c++) {
            int32_t v = a[c] + (((b[c] - a[c]) * f) >> 12);
            v += samples[i * 2 + c];
            samples[i * 2 + c] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        s->phase += s->step;
        read += s->phase >> 16;
        s->phase &= 0xffff;
    }
    s->read.store(read, std::memory_order_release);
}

} // namespace

void wolf3d_sound_start() {
    s = new Sound;
    s->opl = new DBOPL::Chip;
    s->opl->Setup(WOLF3D_MIX_RATE);
    const uint32_t rate = viper_audio_sample_rate();
    s->step = rate ? static_cast<uint32_t>((uint64_t(WOLF3D_MIX_RATE) << 16) / rate) : 0;
    s->tap = rate && viper_audio_tap_install(tap, nullptr);
}

void wolf3d_sound_stop() {
    if (!s) {
        return;
    }
    if (s->tap) {
        viper_audio_tap_remove();
    }
    delete s->opl;
    delete s;
    s = nullptr;
}

// The engine's thread: mixes up to kAheadFrames ahead of the tap. Without a
// tap the engine still keeps its sound's time (the music, which sounds are
// playing), as if it were heard.
extern "C" void wolf3d_sound_fill(void) {
    if (!s) {
        return;
    }
    const uint32_t write = s->write.load(std::memory_order_relaxed);
    uint32_t read = s->read.load(std::memory_order_acquire);
    if (!s->tap) {
        read = write; // nothing reads: a frame's worth each time
    }
    uint32_t want = kAheadFrames - std::min(kAheadFrames, write - read);
    if (!s->tap) {
        want = WOLF3D_MIX_RATE / 60;
    }
    uint32_t pos = write;
    while (want > 0) {
        const uint32_t at = pos % kRingFrames;
        const uint32_t n = std::min(want, kRingFrames - at);
        wolf_SD_Mix(&s->ring[at * 2], static_cast<int>(n));
        pos += n;
        want -= n;
    }
    s->write.store(pos, std::memory_order_release);
}

extern "C" void wolf3d_opl_write(uint32_t reg, uint8_t value) { s->opl->WriteReg(reg, value); }

extern "C" void wolf3d_opl_generate(int32_t *mono, int frames) {
    s->opl->GenerateBlock2(static_cast<Bitu>(frames), mono);
}

// Doom's sound: the engine's I_*Sound and I_*Song functions, mixed on the
// game's thread into a ring of stereo frames at kRate, which the audio tap
// reads from on the audio task at the device's rate.
//
// Only the ring's two positions cross between the threads (one written by
// each side), so the engine's channels, its music player and the WAD's lumps
// are only ever touched on the game's thread, as everywhere else in the
// engine. The cost is latency: the ring holds kTargetFrames ahead.

#include "doom_port.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

#include "viper_app.h"

extern "C" {
// The engine's headers expect these first
#include "prboom/doomdef.h"
#include "prboom/doomstat.h"
#include "prboom/doomtype.h"

#include "prboom/i_sound.h"
#include "prboom/mus2mid.h"
#include "prboom/opl.h"
#include "prboom/oplplayer.h"
#include "prboom/s_sound.h"
#include "prboom/sounds.h"
#include "prboom/w_wad.h"
#include "prboom/z_zone.h"

// The engine reads these (s_sound.c, m_misc.c)
int snd_card = 1;
int mus_card = 1;
int snd_samplerate = 11025;
}

// z_zone.h turns malloc() and free() into the zone's; this file's own memory
// is the heap's, and what the engine allocated goes back with Z_Free()
#undef malloc
#undef free
#undef realloc
#undef calloc
#undef strdup

namespace {

// Doom's sound effects are 11025 Hz, and the OPL music is rendered at the
// same rate: four times cheaper than the device's rate, which the tap
// stretches it to
constexpr int kRate = 11025;
constexpr int kChannels = 8;
// The ring: a power of two of stereo frames, about 370 ms
constexpr uint32_t kRingFrames = 4096;
// How far ahead of the tap the game keeps it: Doom draws at 35 frames a
// second or fewer, and a slow frame (a level loading) shouldn't run it dry
constexpr uint32_t kTargetFrames = kRate / 8; // 125 ms
// Mixed at a time
constexpr uint32_t kChunkFrames = 256;

// A sound effect lump: a header, then 8-bit unsigned samples
struct SfxLump {
    uint16_t format;
    uint16_t samplerate;
    uint32_t length;
    uint8_t samples[];
};

struct Channel {
    int lump = -1; // locked while it plays
    const SfxLump *sfx = nullptr;
    uint32_t pos = 0;        // 16.16, in the lump's samples
    uint32_t step = 0;       // 16.16 per output frame
    uint32_t end = 0;        // the sample count
    int left = 0, right = 0; // 0..254
    int starttic = 0;
    int sfxid = 0;
};

Channel s_channels[kChannels];
int16_t s_ring[kRingFrames * 2];
std::atomic<uint32_t> s_write{0}; // frames written; the game's
std::atomic<uint32_t> s_read{0};  // frames read; the tap's
// The tap's position between ring frames, 16.16, and its step
uint32_t s_frac = 0;
uint32_t s_step = 0;
std::atomic<bool> s_paused{false};
bool s_started = false;
bool s_music_playing = false;
const music_player_t *s_music = &opl_synth_player;
bool s_music_ready = false;

int16_t s_mix[kChunkFrames * 2];

// The engine keeps songs as int handles, too small for a pointer on the
// simulator's 64-bit host: the player's handles are kept here, the engine
// gets their index + 1
constexpr int kSongs = 4;
const void *s_songs[kSongs];

const void *song_of(int handle) {
    return handle > 0 && handle <= kSongs ? s_songs[handle - 1] : nullptr;
}

void channel_stop(Channel &c) {
    if (c.lump >= 0) {
        W_UnlockLumpNum(c.lump);
    }
    c = Channel{};
}

void set_params(Channel &c, int vol, int sep) {
    // vol is 0..127 (snd_SfxVolume * 8 at most), sep 0..255 from left to
    // right, 128 in the middle
    if (vol < 0)
        vol = 0;
    if (vol > 127)
        vol = 127;
    if (sep < 0)
        sep = 0;
    if (sep > 254)
        sep = 254;
    c.left = ((254 - sep) * vol) / 127;
    c.right = (sep * vol) / 127;
}

void mix(int16_t *out, uint32_t frames) {
    if (s_music_ready && s_music_playing && snd_MusicVolume > 0) {
        s_music->render(out, frames);
    } else {
        memset(out, 0, frames * 2 * sizeof(int16_t));
    }
    for (Channel &c : s_channels) {
        if (!c.sfx) {
            continue;
        }
        int16_t *o = out;
        for (uint32_t i = 0; i < frames; i++) {
            const uint32_t at = c.pos >> 16;
            if (at >= c.end) {
                channel_stop(c);
                break;
            }
            const int s = int(c.sfx->samples[at]) - 128;
            int l = o[0] + s * c.left;
            int r = o[1] + s * c.right;
            o[0] = int16_t(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
            o[1] = int16_t(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
            o += 2;
            c.pos += c.step;
        }
    }
}

// On the audio task: reads the ring at kRate and adds it to the block at the
// device's rate, a straight line between ring frames
void tap(int16_t *samples, uint32_t frames, void *) {
    if (s_paused.load(std::memory_order_relaxed)) {
        return;
    }
    uint32_t read = s_read.load(std::memory_order_relaxed);
    const uint32_t write = s_write.load(std::memory_order_acquire);
    uint32_t frac = s_frac;
    for (uint32_t i = 0; i < frames; i++) {
        // Two frames to blend between, or stop (an underrun is silence)
        if (write - read < 2) {
            break;
        }
        const int16_t *a = &s_ring[(read & (kRingFrames - 1)) * 2];
        const int16_t *b = &s_ring[((read + 1) & (kRingFrames - 1)) * 2];
        const int t = int(frac >> 4); // 12 bits
        for (int ch = 0; ch < 2; ch++) {
            const int v = a[ch] + (((b[ch] - a[ch]) * t) >> 12);
            const int sum = samples[2 * i + ch] + v;
            samples[2 * i + ch] = int16_t(sum < -32768 ? -32768 : sum > 32767 ? 32767 : sum);
        }
        frac += s_step;
        read += frac >> 16;
        frac &= 0xffff;
    }
    s_frac = frac;
    s_read.store(read, std::memory_order_release);
}

} // namespace

bool doom_sound_start() {
    const uint32_t rate = viper_audio_sample_rate();
    if (!rate) {
        return false;
    }
    s_step = uint32_t((uint64_t(kRate) << 16) / rate);
    s_frac = 0;
    s_write.store(0);
    s_read.store(0);
    s_paused.store(false);
    s_started = viper_audio_tap_install(tap, nullptr);
    return s_started;
}

void doom_sound_pump() {
    if (!s_started || s_paused.load(std::memory_order_relaxed)) {
        return;
    }
    const uint32_t read = s_read.load(std::memory_order_acquire);
    uint32_t write = s_write.load(std::memory_order_relaxed);
    if (write - read > kRingFrames) {
        write = read; // never happens; resynchronize anyway
    }
    while (write - read < kTargetFrames) {
        uint32_t n = kChunkFrames;
        const uint32_t at = write & (kRingFrames - 1);
        if (n > kRingFrames - at) {
            n = kRingFrames - at;
        }
        mix(s_mix, n);
        memcpy(&s_ring[at * 2], s_mix, n * 2 * sizeof(int16_t));
        write += n;
        s_write.store(write, std::memory_order_release);
    }
}

void doom_sound_pause(bool paused) {
    s_paused.store(paused);
    if (!paused) {
        // Start the ring over rather than play what was waiting
        s_write.store(s_read.load());
    }
}

void doom_sound_stop() {
    if (s_started) {
        viper_audio_tap_remove();
        s_started = false;
    }
    for (Channel &c : s_channels) {
        c = Channel{}; // the WAD (and its locks) goes with the zone
    }
    if (s_music_ready) {
        s_music->stop();
        for (const void *&song : s_songs) {
            if (song) {
                s_music->unregistersong(song);
                song = nullptr;
            }
        }
        s_music->shutdown();
        s_music_ready = false;
    }
    s_music_playing = false;
}

// ---- the engine's side (i_sound.h) ----------------------------------------------

extern "C" {

void I_InitSound(void) {
    for (Channel &c : s_channels) {
        c = Channel{};
    }
    s_music_ready = s_music->init(kRate) != 0;
    if (s_music_ready) {
        s_music->setvolume(snd_MusicVolume);
    }
}

void I_ShutdownSound(void) { doom_sound_stop(); }

void I_SetChannels(void) {}

int I_StartSound(int sfxid, int channel, int vol, int sep, int pitch, int priority) {
    (void)channel;
    (void)pitch;
    (void)priority;
    if (sfxid <= 0 || sfxid >= NUMSFX || S_sfx[sfxid].lumpnum < 0) {
        return -1;
    }
    // These play once at a time: stop any still going
    if (sfxid == sfx_sawup || sfxid == sfx_sawidl || sfxid == sfx_sawful || sfxid == sfx_sawhit ||
        sfxid == sfx_stnmov || sfxid == sfx_pistol) {
        for (Channel &c : s_channels) {
            if (c.sfx && c.sfxid == sfxid) {
                channel_stop(c);
            }
        }
    }
    // A free channel, or the oldest
    int slot = 0;
    int oldest = gametic;
    for (int i = 0; i < kChannels; i++) {
        if (!s_channels[i].sfx) {
            slot = i;
            break;
        }
        if (s_channels[i].starttic < oldest) {
            slot = i;
            oldest = s_channels[i].starttic;
        }
    }
    Channel &c = s_channels[slot];
    channel_stop(c);
    // The lump stays locked (unpurgeable) only while it plays
    const int lump = S_sfx[sfxid].lumpnum;
    const auto *sfx = static_cast<const SfxLump *>(W_CacheLumpNum(lump));
    const uint32_t lump_len = uint32_t(W_LumpLength(lump));
    if (!sfx || lump_len < 8) {
        if (sfx)
            W_UnlockLumpNum(lump);
        return -1;
    }
    uint32_t len = sfx->length;
    if (len > lump_len - 8) {
        len = lump_len - 8;
    }
    const uint32_t src_rate = sfx->samplerate ? sfx->samplerate : 11025;
    c.lump = lump;
    c.sfx = sfx;
    c.pos = 0;
    c.step = uint32_t((uint64_t(src_rate) << 16) / kRate);
    c.end = len;
    c.starttic = gametic;
    c.sfxid = sfxid;
    set_params(c, vol, sep);
    return slot;
}

void I_StopSound(int handle) {
    if (handle >= 0 && handle < kChannels) {
        channel_stop(s_channels[handle]);
    }
}

bool I_SoundIsPlaying(int handle) {
    return handle >= 0 && handle < kChannels && s_channels[handle].sfx;
}

bool I_AnySoundStillPlaying(void) {
    for (const Channel &c : s_channels) {
        if (c.sfx) {
            return true;
        }
    }
    return false;
}

void I_UpdateSoundParams(int handle, int vol, int sep, int pitch) {
    (void)pitch;
    if (handle >= 0 && handle < kChannels && s_channels[handle].sfx) {
        set_params(s_channels[handle], vol, sep);
    }
}

void I_InitMusic(void) {}
void I_ShutdownMusic(void) {}
void I_UpdateMusic(void) {}

void I_SetMusicVolume(int volume) {
    if (s_music_ready) {
        s_music->setvolume(volume);
    }
}

void I_PauseSong(int handle) {
    (void)handle;
    if (s_music_ready) {
        s_music->pause();
    }
    s_music_playing = false;
}

void I_ResumeSong(int handle) {
    (void)handle;
    if (s_music_ready) {
        s_music->resume();
    }
    s_music_playing = true;
}

int I_RegisterSong(const void *data, size_t len) {
    if (!s_music_ready) {
        return 0;
    }
    // MUS lumps become MIDI; the player copies what it keeps
    uint8_t *mid = nullptr;
    size_t midlen = 0;
    const void *handle;
    if (mus2mid(static_cast<const byte *>(data), len, &mid, &midlen, 64) == 0) {
        handle = s_music->registersong(mid, unsigned(midlen));
    } else {
        handle = s_music->registersong(data, unsigned(len));
    }
    Z_Free(mid); // mus2mid() allocates from the zone
    if (!handle) {
        return 0;
    }
    for (int i = 0; i < kSongs; i++) {
        if (!s_songs[i]) {
            s_songs[i] = handle;
            return i + 1;
        }
    }
    s_music->unregistersong(handle);
    return 0;
}

void I_PlaySong(int handle, int looping) {
    if (s_music_ready && song_of(handle)) {
        s_music->play(song_of(handle), looping);
        s_music_playing = true;
    }
}

void I_StopSong(int handle) {
    (void)handle;
    if (s_music_ready) {
        s_music->stop();
    }
    s_music_playing = false;
}

void I_UnRegisterSong(int handle) {
    if (s_music_ready && song_of(handle)) {
        s_music->unregistersong(song_of(handle));
        s_songs[handle - 1] = nullptr;
    }
}

} // extern "C"

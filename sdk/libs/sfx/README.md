# sfx

Status: implemented; current as of 2026-11-07. `sfx::Synth` with sounds, songs and clips, used by Blocks, Turbo Blast, Pumpkin Smash, Dino and Fruit Merge (ELF apps) and Invaders and Retro SFX (built in, through `sfx::Tap`). sfxr (`sfx/sfxr.hpp`) renders clips for Retro SFX and Escape Vertigo.

A small chiptune synthesizer for games: sound effects, four-track songs and recorded clips, mixed into an audio tap's buffer, and sfxr to make clips. Header-only, with no LVGL, ESP-IDF or `System` calls, so built-in apps, ELF apps and the sim's tests share it. It started as Blocks' synthesizer.

- **Built-in apps:** add `sfx` to `REQUIRES` (the `apps` component has it), and connect the synth with an `sfx::Tap` member.
- **ELF apps:** `set(VIPER_LIBS sfx)` before `project_elf()` (`apps/README.md`, "Game libraries").
- **Tests:** `sim/tests/test_sfx.cpp`, `sim/tests/test_sfxr.cpp`, and Blocks' `apps/blocks/tests/test_blocks_sound.cpp`.

| Header | What |
| --- | --- |
| `sfx/sound.hpp` | `Sound`: up to 6 `Segment`s played one after another, each a `Wave` (12.5%, 25% or 50% pulse, triangle, noise, sine) whose pitch and volume slide in a straight line, and a priority. `seg()` and `sound()` build them as `constexpr` tables; `on_channel()`, `looped()` and `vibrato()` make variants |
| `sfx/song.hpp` | `Song`: four tracks of sixteenth-note steps (pulse lead, pulse arpeggio, triangle bass, drums), written as text (`"A4 . - K"`) and parsed at compile time by `track()` and `song()`; `valid()` for a `static_assert`. A `DrumKit` (`kDrumKit` by default) gives the drums' sounds |
| `sfx/clip.hpp` | `Clip`: mono int16 samples at a sample rate, for the synth to play as recorded |
| `sfx/sfxr.hpp` | sfxr (DrPetter's generator, with jsfxr's presets): `sfxr::Params` by sfxr's own names, `sfxr::preset()` and `sfxr::mutate()` with a seedable `sfxr::Rng`, `sfxr::Generator`, and `render_sfxr(params, rate, seed)` into a `Clip`. Float and heap: render when loading, not on the audio task |
| `sfx/synth.hpp` | `Synth<EffectVoices = 2>`: `start()`, `play(sound, semitones)`, `play(clip, volume)`, `stop(sound)`, `stop(clip)`, `music(&song)`, `set_tempo()`, `set_paused()`, `set_enabled(music, effects)`, and `render()` on the audio task |
| `sfx/tap.hpp` | `Tap`, for built-in apps only: `install(this, synth)` starts the synth at the audio engine's rate and mixes it in, until `remove()` or the destructor. `install(this, function)` does the same for a game's own mixer |

```cpp
constexpr sfx::Sound kJump = sfx::sound(1, sfx::seg(sfx::Wave::Pulse25, 330, 720, 90, 90, 0));
constexpr sfx::Song kTitle = sfx::song(112, sfx::track("E5 . . . G5 . - ."), sfx::track("C4 E4 G4 E4 C4 E4 G4 E4"),
                                       sfx::track("C3 . . . . . - ."), sfx::track("K . H . S . H ."));
static_assert(sfx::valid(kTitle), "the title's tracks differ in length, or one has a bad step");

sfx::Synth<> synth; // a global: the audio task uses it until the tap is removed

void on_create(void *) {
    synth.start(viper_audio_sample_rate()); // before the tap can call render()
    viper_audio_tap_install(
        [](int16_t *s, uint32_t n, void *u) { static_cast<sfx::Synth<> *>(u)->render(s, n); }, &synth);
    synth.music(&kTitle);
}
// loop(): synth.play(kJump); on_destroy(): viper_audio_tap_remove();
```

A built-in app does the same with a `Tap` (`components/apps/README.md`, "Sound Effects"):

```cpp
sfx::Synth<> synth_;
sfx::Tap tap_; // declared after the synth, so it's removed before the synth goes

void MyGame::onCreate() { tap_.install(this, synth_); }
```

## How it works

- **Threads.** The app calls `start()` while no tap is installed, then `play()`, `music()` and the setters; the audio task calls `render()`. They share only 32-bit values and pointers that one side writes and the other reads, plus a 16-entry queue of effects. ESP32-S3 can't do an atomic read-modify-write in PSRAM, where an ELF app's data lives, so nothing here needs one.
- **No heap, no float, no 64-bit division** in the synth: an ELF app can only call what the firmware exports. Sounds and songs are referred to, not copied, so keep them in `const` tables (or anything that outlives the synth).
- **Mixing.** Every 32 samples a control step moves the slides and envelopes and the song along; between them the oscillators run on 32-bit phases. `render()` adds into the buffer (the tap's buffer already holds the rest of the system's sound) and clamps to int16.
- **Voices.** The song has its own four voices. Effects have `EffectVoices`: a new sound takes a free one, or the least important one playing (the oldest of equals) if that's no more important than it; otherwise it's dropped. With every priority 0, the oldest gives way.
- **Channels.** A sound with a channel (`on_channel(sound, n)`, n from 1) replaces whatever is playing on that channel, whatever its priority, the way an arcade board had one circuit for each kind of sound. Invaders gives each of its sounds one: a march note cuts off the last, and the shot starts over when fired again.
- **Loops and vibrato.** A `looped()` sound starts over when it ends, until `stop(sound)` (or switching effects off). `vibrato(sound, hz, percent)` swings its pitch up and down by up to `percent` (a sine, `hz` times a second), moved at each control step; Invaders' mystery ship warbles at 8 Hz by 31%.
- **Clips.** `play(clip, volume)` plays a `Clip` on an effect voice, sample for sample at volume 255, with priority 0 and no channel. The synth reads the clip's memory as it plays, so keep the clip alive and unchanged meanwhile; to replace one that may be playing, remove the tap (which waits for the audio task), change it, and install the tap again, as Retro SFX does. Render clips at the synth's sample rate; the synth doesn't resample.
- **sfxr.** `render_sfxr()` runs sfxr at its own 44.1 kHz with 8x supersampling and resamples in a straight line to the rate asked for, so a sound keeps its pitch and length. It's the same math as sfxr (and as `components/sfx_engine`, which it replaced: sounds without noise come out the same sample for sample); noise comes from `sfxr::Rng`, so a seed makes the same sound every time. It calls only `powf` and `sinf` from libm, which the firmware exports to ELF apps.
- **Levels.** At full volume a lead is 2400, an arpeggio 1100, the bass 3800, a drum 2600, and an effect 4200 (`kEffectLevel`; `start(rate, level)` changes it). Blocks' loudest moment, a song and two effects, peaks under 20000. Turbo Blast and Pumpkin Smash use four effect voices at 5610.
- **Pitch.** `play(sound, semitones)` plays it up to 36 semitones higher (Turbo Blast's chains climb). Pitches stop just below the sample rate; for noise, the "pitch" is how often the noise changes.
- **Tempo.** `kTempoNormal` (256) plays a song at its own bpm; `set_tempo()` takes 128 to 512. A song loops; `music(nullptr)` stops it, and `music()` of any song starts it from the top.

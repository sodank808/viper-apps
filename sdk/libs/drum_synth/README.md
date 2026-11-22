# drum_synth

Status: implemented (milestone 1 of [docs/plans/drum_machine.md](../../docs/plans/drum_machine.md)); current as of 2026-11-05.

Renders parameterized drum voices to mono 16-bit PCM. It's the synthesis half of the planned drum machine: a kit renders when it loads or a sound is edited, and the Drum Machine app's mixer (`apps/drum_machine/`, an ELF app that compiles this library in with `VIPER_LIBS drum_synth`) plays the samples. Standard library only, so the same code builds for the device and the host simulator.

## Models

| Model | Sound | Parameters |
| --- | --- | --- |
| `kick` | Sine with a downward pitch sweep, a filtered-noise click, soft-clip drive | `tune` Hz, `decay` s, `sweep` semitones, `sweep_time` s, `click`, `drive` |
| `snare` | Two sine modes (1 : 1.8) with a short pitch drop, plus high- and low-passed noise | `tune` Hz, `tone`, `body_decay` s, `noise_decay` s, `snappy` |
| `hat` | The 808's six square oscillators (band-limited) and some noise, band-passed, enveloped, high-passed | `tune` semitones, `decay` s, `tone`, `noise` |
| `clap` | Band-passed noise in repeated bursts, then a wider tail | `tone` Hz, `spread` s, `bursts`, `decay` s, `width` |

Decay times are to −60 dB. Unitless parameters run 0..1. Ranges and defaults are in `model_info(model).params`, which a UI can list. `kit_808()` holds the starter kit: kick, snare, closed hat, open hat, clap (both hats use `hat`).

## Use

```cpp
#include "drum_synth.hpp"

drum::RenderRequest req;
req.voice = drum::VoiceParams::defaults(drum::Model::Kick);
req.voice.set("decay", 0.5f);  // clamped to the model's range
req.sample_rate = 32000;

// All at once
drum::RenderResult result;
std::vector<int16_t> pcm = drum::render(req, &result);

// Or in pieces from a background task, with progress and cancellation
drum::Renderer r(req);
int16_t *buf = /* capacity() frames, e.g. in PSRAM */;
while (!r.done() && !cancelled) {
    r.process(buf + r.frames_rendered(), 1024);
    report(r.progress());
}
// r.result().frames is the sound's length; shrink the buffer to it
```

- **Repeatable:** the same parameters, accent, seed and rate give identical samples on one build. Noise and oscillator phases come from the seed, so round-robin variants are just different seeds.
- **Length:** a sound ends once it has stayed below `tail_threshold_db` (−72 dBFS) for `tail_hold_seconds` (50 ms) after its excitation, and the quiet stretch is trimmed. At `max_seconds` (5 s) it's cut with a 5 ms fade and `result.truncated` is set.
- **Level:** each model is calibrated so its defaults peak a few dB under full scale (hats lower). `accent` 0 is 6 dB below 1. `gain_db` is kit headroom. Peaks over −1 dBFS go through a soft limiter, counted in `result.limited_samples`; the tests keep that near zero across every parameter range.
- **Cost:** no allocation while rendering (one small object per `Renderer`), floats, and no transcendental functions per sample. Coefficients follow the sample rate, so lengths and pitches match at any rate.

Preset text is `model param=value ...` (`format_voice`, `parse_voice`); left-out parameters keep their defaults. `encode_wav` writes a 16-bit PCM WAV.

## Versioning

`ModelInfo::version` changes when a model's output for the same parameters changes, and `kRendererVersion` when the renderer's does. Saved kits store parameters, not audio, so versions only matter once rendered audio is cached on disk.

## Sample rate

At 24 kHz nothing above 12 kHz survives, so the hats lose their top octave (their peak is about 4 dB lower than at 48 kHz). The hat oscillators are PolyBLEP band-limited, so they don't alias at any rate. Kick, snare and clap are about the same at every rate. Pick the drum machine's output rate by listening (below).

## Tests and listening

`sim/tests/test_drum_synth.cpp` (`[drum]` in `make sim-test`) checks finite, bounded output across every parameter's range, repeatability, chunked rendering, tuning, sweep, tail timing, truncation, accent, clamping, preset text, and the WAV header.

To listen, write the kit and a four-bar groove at 24, 32 and 48 kHz:

```bash
VIPER_SIM_DRUM_WAVS=/tmp/drums ./build-sim/tests/viper_sim_tests "[drum]"
```

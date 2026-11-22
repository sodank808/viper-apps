# game

Status: implemented; current as of 2026-11-11. `game::Rng` and `game::InputSync` (lockstep for link play; tested in the sim and used by Pong's versus mode, which is the worked example (`components/apps/pong/README.md`), and by NES netplay; played on two units at close range, 2026-11-11, not yet at range); the rest is planned in `docs/plans/game_api_consolidation.md` (Phase 4).

Helpers games share: plain C++, header-only, with no LVGL, ESP-IDF or `System` calls. Built-in apps, ELF apps and the sim's tests all use the same code.

- **Built-in apps:** add `game` to the component's `REQUIRES`.
- **ELF apps:** `set(VIPER_LIBS game)` before `project_elf()` in the app's `CMakeLists.txt` (`apps/README.md`, "Game libraries").
- **Tests:** `sim/tests/test_game_libs.cpp`; `InputSync` in `sim/tests/test_input_sync.cpp` (`[input_sync]`).

## `game::Rng` (`game/rng.hpp`)

A xorshift32 the game seeds once, so the same seed and the same input give the same game. Blocks' and Turbo Blast's replays depend on that.

```cpp
game::Rng rng(viper_random());       // ELF app; built-in: System::get().getRandomInt(...)
int lane = rng.range(0, 3);          // 0..3, both included
uint32_t i = rng.below(7);           // 0..6
float f = rng.unit();                // [0, 1)
if (rng.chance(25)) { /* ... */ }    // 25% of the time
rng.shuffle(cards, 52);
```

- A zero seed is replaced with a fixed one, since xorshift sticks at zero.
- `below()` is a modulo, so slightly uneven for large ranges. It's kept because Blocks' recorded games were made with it.
- Don't seed in a loop from the platform: seed once and draw from the generator.

## `game::InputSync` (`game/input_sync.hpp`)

Deterministic lockstep for link play (design: `docs/design/multiplayer.md`, "InputSync"). Two to four units run copies of the same simulation and exchange only inputs: tick `T` runs on every unit with the same inputs for every player, or doesn't run yet. The NES netplay driver (Phase 4) is meant to run on it too, with "run one frame" as its tick.

It doesn't touch the radio or the clock itself. It talks to the link through a `game::SyncPort` (the session clock, this unit's slot, whether a slot is still in the session, and sending a packet to a slot), and the packets that arrive are handed to `receive()`. Built-in apps use `LinkSyncPort` (`components/system/include/link_sync_port.hpp`), which does both over `System::get().link()`; the tests use one over a `Session` and `Messenger` (`sim/tests/input_sync_test_util.hpp`). Fixed-size buffers, no allocation, never blocks.

### Using it

The game's simulation must be deterministic: a fixed tick, integer or fixed-point state, a `game::Rng` seeded from `SyncStart::seed`, and nothing read from outside the inputs (no clock, no hardware randomness, no floats in the simulation).

```cpp
#include "game/input_sync.hpp"
#include "link_sync_port.hpp"

game::InputSync sync_;
LinkSyncPort port_{System::get().link(), sync_};   // declared after sync_

// The host, once everyone is ready (viper::link::Lobby does this; with it,
// every unit just passes lobby.start_params()->sync to start()):
game::SyncStart start;
start.seed = rng_seed;
start.start_us = link.now_us() + 300'000;           // on the session clock
start.input_delay = game::InputSync::choose_delay(worst_rtt_us, worst_jitter_us);
start.players = 0b11;                               // slots 0 and 1
// ...send start.encode() to every player reliably; each calls:
sync_.start(port_, start);
sim_.reset(start.seed);

uint32_t Pong::onLoop(GamepadState &pad) {
    const game::SyncFrame &f = sync_.advance(pack(pad));   // 2 bytes by default
    for (int i = 0; i < f.ticks; ++i) {
        const game::SyncTick &t = f.tick[i];
        sim_.tick(t.pad(0), t.pad(1));                     // or t.input(slot)
        if (t.hash_due) {
            sync_.report_hash(t.number, sim_.hash());
        }
    }
    if (f.status == game::SyncStatus::Lagging) { /* "Waiting for player" */ }
    if (f.over()) { /* f.end, f.end_slot, or f.desync_tick: end the match */ }
    draw(sim_);
    return f.interval_ms();
}
```

- **Input record:** `input_size` bytes per player per tick (1 to 8, default 2: `advance(uint16_t)` and `SyncTick::pad()`), defined by the game. Pause and other control bits go in it too, so they act on the same tick everywhere.
- **Input delay:** what a player presses while tick `T` is due runs at `T + D`. `choose_delay(rtt, jitter)` is half the round trip plus twice the jitter plus 4 ms, in ticks, clamped to 2..6 (2 for everything Phase 0 measured). Ticks 0 to `D - 1` run with all inputs zero.
- **The schedule:** tick `N` is due at `start_us + N * tick_us` on the session clock, the same moment on every unit. `advance()` runs the due ticks whose inputs are all there, at most two per frame (one extra to catch up), and `interval_ms()` is how long until the next is due, so the frame timer follows the session clock and the units' crystals don't drift apart.
- **Stalls:** a due tick without every input is `Waiting` (`waiting_for` says whose), and `Lagging` once one player has been waited on for 250 ms (`lagging`). While waiting the interval is 4 ms, so the frame comes back soon; it never blocks. After a long stall the host moves the schedule back (`shift_us()`) and the others follow, so nobody runs a long burst of double-speed ticks.
- **Ending:** a player leaving the session (or timing out of it, 3 s), a player's `stop()`, or this unit leaving the session ends the match (`Ended`, with `end` and `end_slot`). Carrying on with a missing player's input held neutral isn't built: the others would first have to agree on the tick its input stops at.
- **Desync:** the game hashes its state after each `hash_due` tick (one in 60). Hashes ride in the packets; a mismatch makes every unit `Desynced`, with the tick (`desync_tick`). Version 1 ends the match there.
- **Counters:** `stats()`: ticks, stall and catch-up frames, schedule shifts, packets and inputs sent and received, redundant inputs, and every packet that was ignored, of another version, or malformed.

### Wire format

Unreliable messages of type `InputSync::kMessageType` (0xE1, reserved for system layers), one per frame to each other player, and only when something changed or a tick has passed. Little-endian: version (1), flags (hash, quit), match id (FNV-1a of the encoded `SyncStart`, so a unit with other parameters or from an earlier match is ignored), the sender's next tick, its ack (the first tick of the addressee's input it lacks), the schedule shift, the first input tick and a count, the inputs, then the latest state hash and its tick. At most 163 bytes. Each packet repeats every input the addressee hasn't acknowledged, at most 2D + 1 of them, so a lost packet is covered by the next. `SyncStart` encodes to 20 bytes, for the lobby's Start message.

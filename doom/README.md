# Doom

Status: runs on a device (5–25 ms of engine time a frame, about 2 s of setup, a clean exit) and in the host simulator (shareware `doom1.wad`). Current as of 2026-11-22.

Doom as an ELF app: [PrBoom](https://prboom.sourceforge.net/), by way of retro-go's prboom-go and esp-box-emu ([esp-cpp/prboom](https://github.com/esp-cpp/prboom), from [esp-box-emu#101](https://github.com/esp-cpp/esp-box-emu/pull/101)), with Viper's platform layer around it. It plays the IWADs PrBoom knows: the shareware `doom1.wad`, `doom.wad` (Doom and Ultimate Doom), `doom2.wad`, `tnt.wad` and `plutonia.wad`.

## Playing

The app is an emulator in the manifest's sense (`"type": "EMULATOR"`, `doom://`, `.wad`): put a WAD in `roms/` on the SD card or fatfs, and the icon opens the ROM picker on it, as Files does. The picker opens on the WAD played last, so A starts it again. Without a file it looks for `doom1.wad`, `doom.wad` or `doom2.wad` in its own folder. The shareware WAD is freely distributable but 4 MB, and this repo doesn't carry it yet; esp-cpp/prboom's `data/` has a copy.

| Button | In the game | In Doom's menus |
| --- | --- | --- |
| D-pad up / down | Move | Choose |
| D-pad left / right | Strafe | Choose, change a setting |
| L / R | Turn | |
| A | Fire | Select |
| B | Use (doors, switches) | Back |
| X | Next weapon | |
| Y, Select | Automap | |
| Start | Menu | Close |

Doom's menus use the arrow keys that also turn, so each button sends its game key or, while a menu is up, its menu key, and lets go of the key its press sent. Autorun is on by default. The app takes B for itself (`viper_app_handle_back(1)`), so it closes from Doom's own Quit Game, or by holding Select as any app does. The settings Doom's options menu changes are kept in `prboom.cfg` with the saves, in the app's data folder (`viper_data_path()`), read at start and saved when the app closes. (esp-box-emu's PrBoom never read or wrote it; `M_LoadDefaults()` now parses it with `fgets()` and `strtol()`, since the firmware doesn't export `fscanf()`.)

It installs to the SD card (`SDCARD_APPS` in the `Makefile`): the app is 640 KB, and the firmware's fatfs is nearly full.

## Layout

- `main/prboom/`: the engine, esp-cpp/prboom's `prboom/` at b18131f6 (2026-11-13), minus the network server (`d_server.c`, which has its own `main()`). Every change is marked `Viper:`.
- `main/doom_app.cpp`: the app (`viper_app_t`), the frame loop, and the engine's video, input, timing and system functions (`I_*`), plus the hooks the haptics come from (`R_Player*`).
- `main/doom_sound.cpp`: sound effects and OPL music.
- `main/doom_memory.cpp`: where the zone's memory comes from (below), and the engine's fixed tables.
- `main/doom_statics.cpp`: the tables esp-box-emu turned from arrays into pointers (its `doom_shared_memory.cpp`), allocated before setup.

## How it runs

- **Setup** (`D_DoomMainSetup()`: the WAD's directory, the texture tables, the translucency map) takes a while on a device, so it runs on a thread of its own while `loop()` shows that it's loading. Nothing draws or plays from that thread.
- **Frames.** Each `loop()` is one turn of `D_DoomLoop()` without its waits: `D_BuildNewTiccmds()` makes the tics the clock says are due (35 a second, each with the input of the moment), they run, and `D_Display()` draws once. A frame with no tic due draws nothing. The screen wipe runs a step a frame (`D_WipeStep()` in `d_main.c`) instead of looping until it's done. Doom takes no input during a wipe, as it never has.
- **Video.** Doom draws its 320x200 8-bit screen into a buffer of the app's, and `I_FinishUpdate()` copies it to an indexed frame (`viper_frame_set_indexed()`) that scan-out stretches to the display, with Doom's palette, which changes as the player is hurt or picks things up.
- **Sound.** The game's thread mixes up to eight sound effects and the OPL music (dbopl, from the WAD's `GENMIDI` instruments) at 11025 Hz into a ring 125 ms ahead; the audio tap reads it at the device's rate. Only the ring's positions cross between the threads, so the channels, the music player and the lumps are touched only by the game's thread. A sound effect's lump is locked only while it plays, so the 522 KB of shareware sounds can be purged like any other cached lump.
- **Errors.** `I_Error()` leaves the engine with `longjmp()` back to `loop()` (or the setup thread) and shows the message; a button closes the app.

## Memory

PSRAM is what's short. On a device with 8 MB of it, the firmware's code takes about 3 MB and its emulators' ROM buffer (`System::romdata()`) 2 MB, which leaves about 1.2 MB free at the home screen. Doom's code and data take about 500 KB of PSRAM when loaded (283 KB of code, 138 KB of read-only data), and its hottest tables 40 KB of internal RAM (`.shared_dram`).

The zone, the engine's heap, needs more than what's left, so it borrows the ROM buffer, which no built-in emulator uses while an ELF app runs: `viper_memory_scratch()`, added to the app API for this, lends it (2 MB) to the app in front. Without it (older firmware) the app takes the biggest block `malloc()` gives, from 3 MB down to 512 KB. The arena is first fit over a free list kept in the block headers (`doom_memory.cpp`).

When the arena is full, `Z_Malloc()` purges cached lumps (textures, sprites, sounds, purged two at a time) and tries again; only when nothing purgeable is left does it take blocks from the heap, and only while more than 256 KB of PSRAM stays free for the rest of the system. So the arena's size decides how often a level rereads the WAD, not whether it runs. In the simulator, the shareware episode uses 709 KB of the arena after setup and fills it during play (then purges), with nothing from the heap.

Two engine bugs that esp-box-emu's pool tolerated mattered here, both in `Z_Close()`: freeing block by block wrote NULL through owner pointers that lived in blocks already freed (corrupting a free list), and the statics that own zone blocks (`P_FindDoomedNum()`'s hash, for one) must be told their blocks are gone, since the simulator starts the engine again in the same program. `Z_Close()` now nulls every owner while every block is still allocated, then forgets the blocks, and `doom_memory_shutdown()` frees the memory.

## Testing

The host simulator is in the Viper firmware repo, with this repo at `apps_public/`. There `make sim-test` builds the app into the simulator, and the all-apps and leak tests open it (without a WAD, that's the ROM picker). The test that plays it, `tests/test_doom_boot.cpp`, needs a WAD:

```bash
VIPER_SIM_DOOM_WAD=/path/to/doom1.wad ./build-sim/tests/viper_sim_app_boot_tests "[doom]"
```

It opens Doom on the WAD, waits for setup, checks that the demos play at 35 tics a second and draw, that Start opens the menu and a new game turns with the D-pad, then closes it and opens it twice more, checking that nothing grows between launches. `VIPER_SIM_DOOM_SHOTS=<dir>` saves the demo and the game. Expect UndefinedBehaviorSanitizer reports from the engine in the simulator's log (signed overflow in dbopl, packed WAD structures read unaligned); they're upstream Doom's, and harmless on the device.

To play it in the simulator, put the WAD in `.viper-sim/sdcard/roms/` and `make sim-run`.

Not yet measured on a device: how long setup takes from a slow SD card, and whether the music's mixing keeps up on the game's thread in the busiest levels.

## License

PrBoom is under the GNU General Public License, version 2 (`main/prboom/COPYING`), and so is this app, which links it. The shareware `doom1.wad` is id Software's, freely distributable unmodified; the other IWADs aren't.

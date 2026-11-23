# NetHack

Status: plays in the host simulator and on a Proto1 with the terminal view (Phase 1 of the firmware repo's `docs/plans/nethack_port.md`); tiles, the first-person view and typing are to come. Current as of 2026-11-22.

[NetHack 5.0.0](https://www.nethack.org/v500/release.html) as an ELF app: the whole game, unchanged but for a few marked lines, with a window port and a platform of its own for Viper, and a terminal view drawn into the frame. It needs firmware that loads apps bigger than the heap (the ELF loader's spare block) and exports what NetHack calls (`sdk/exports/libc.txt`, the double-precision math, stdio reads, time, and `open`/`read`/`write`/`lseek`/`unlink`); older firmware refuses it when it's installed, naming what's missing.

## Playing

| Button | Playing | In a question | In a menu |
| --- | --- | --- | --- |
| D-pad | Move (two at once: diagonally); held, it repeats | Left/Right: choose; a direction, where one is asked | Move; Left/Right a page |
| A | The command list (pick up, eat, read, stairs, pray, save, ...) | Answer; "here" or the list of objects | Choose, or mark (in a list of several) |
| B | Escape | Cancel | Cancel |
| X | Inventory | All objects (`*`) | Done (several) |
| Y | Search | | |
| L / R | Up / down stairs | | Page |
| Start | Extended commands (`#pray`, `#loot`, ...) | | Done (several) |

Holding Select closes the app, as in any app, and saves the game; opening it again restores it. A game that ends (death, quitting, ascending) closes the app.

Typing (naming things, wishes, engraving text) isn't there yet: a prompt for a line of text is cancelled. The on-screen keyboard is Phase 2.

The hero is always "Viper", and its role, race, gender and alignment are chosen with NetHack's own questions (A to let it pick). The options it starts with are in `assets/nethackrc` (no tutorial, no opening story, autopickup of gold, menus rather than typed lists).

## Layout

- `main/nethack/`: NetHack 5.0.0 (tag `NetHack-5.0.0_Released`): `include/`, `src/`, and `sys/share/pmatchregex.c`. Every change is marked `Viper:`:
  - `config.h` includes `port/nhviper.h` first, and leaves out compression, `chdir()` and the crash-recovery level files (`INSURANCE`).
  - `winprocs.h`, `windows.c`, `mdlib.c`, `role.c`, `rip.c`: the `viper` window port, and NetHack's generic role menus and text tombstone for it.
  - `cfgfiles.c`: `sysconf` is found by prefix, as on Windows.
  - `pline.c`: no message handler program (no processes).
  - `include/nhlua.h` is written by hand (upstream's Makefile generates it).
- `main/lua/`: the Lua 5.4.8 NetHack bundles, minus `lua.c` and `luac.c`. `luaconf.h` includes `port/nh_sim.h` (marked `Viper:`).
- `main/port/`: NetHack's window port (`winviper.c`) and platform (`viper_sys.c`: starting a game, the file prefixes, the lock file, the Unix calls NetHack expects), the C interface between them and the app (`nh_port.h`), and the build's settings (`nhviper.h`, `nh_sim.h`, `nh_libc.h`, `nh_stubs.c`, `nh_names.h`).
- `main/nethack_engine.cpp`: the engine's thread, the turn between it and `loop()`, and the game model's side of `nh_port.h`.
- `main/nethack_game.hpp`: the game model (map, messages, status, windows, what the engine waits for). Plain C++.
- `main/nethack_view.cpp`: the terminal view and the menus, text windows and questions drawn over it.
- `main/nethack_app.cpp`: the app (`viper_app_t`), the buttons, the command list.
- `upstream/`: what the game's data is made from (`dat/`, and `util/`'s `makedefs` and `dlb`), not built into the app.
- `assets/`: installed beside `app.elf`: `sysconf`, `nethackrc`, and `nhdat` (built, gitignored).
- `tools/make_nhdat.sh`: builds `makedefs` and `dlb` for the host from the vendored sources and packs `assets/nhdat`. The repo's `make` runs it (`tools/build_assets.sh`); so does the firmware repo's simulator build, for the tests.
- `tools/gen_names.py`: writes `port/nh_names.h` (below).

## How it runs

- **The engine's thread.** NetHack is a program that waits for a key wherever it pleases. It runs on a thread of its own (64 KB of stack on a device), and wherever the window port waits for the player (a key, a yes/no, a menu, a text window, `--More--`) it hands the turn to `loop()` with a request and waits for the answer, through a mutex and a condition variable. Only one side runs at a time, so the model needs no locks: the view reads it only while the engine waits. `loop()` waits up to 100 ms for the engine (5 s in the simulator); a level being made carries on into the next frame.
- **The window port** (`port/winviper.c`) draws nothing: it records what NetHack shows into the game model (the map's glyphs, messages, the status lines from NetHack's own `genl_status_update()`, menus and text windows) and turns NetHack's questions into requests. NetHack's menu identifiers stay on its side; the model sees indexes.
- **Files** have prefixes (`fqn_prefix[]`), never a current directory: the data (`nhdat`, `sysconf`, `nethackrc`) in the app's folder, levels, saves and scores in its data folder (`viper_data_path()`).
- **Leaving.** NetHack's `exit()` (the end of a game, an error) goes back to the top of the engine's thread with `longjmp()` (`nhv_exit()`). Closing the app mid-game answers the engine's next questions with Escape and `S` (and "yes" to "Really save?") until it has saved.
- **Memory.** Every allocation of NetHack's and Lua's goes through `nhv_malloc()` and friends (`port/nh_sim.h`), which keep a list, so the end of a game frees all of it.

## On a device

- The app is a 3.2 MB `app.elf` with 2.0 MB of code, which the loader puts in the firmware's 2 MB spare block (the built-in emulators' ROM buffer), and 0.6 MB of data. Loading it takes about 4 s.
- On a Proto1 the heap peaked at 532 KB in a short game; PSRAM had 1.5 MB free while it ran.
- Saving took 1.6 s (a 54 KB save) and restoring 1.4 s.
- Two link-time workarounds, both in `main/`:
  - Each call site of a function from outside the app gets a PLT entry of its own, and past 254 entries binutils 2.43's Xtensa linker asserts (`elf32-xtensa.c`). The C library functions NetHack calls most go through one local wrapper each (`port/nh_libc.h`), on a device only.
  - GCC's induction variables (`-fivopts`) started a loop's pointer in `glyphs.c` far before its table, which the loader can't relocate; that file is built with `-fno-ivopts`.
- Untested on a device so far: long games (memory over time, bones files, deep levels' Lua), the high score list, the timing of making levels in the Gnomish Mines and Sokoban.

## The simulator

The simulator compiles every ELF app into one program, which brings two things a device doesn't have:

- **Names.** NetHack's and Lua's globals (`level`, `flags`, `panic`, `lua_newstate`, ...) would meet other apps' and the firmware's own Lua. `port/nh_names.h`, included through `port/nh_sim.h` by every NetHack, Lua and port file, gives each the prefix `nh_`. Run `tools/gen_names.py` after adding or removing a global.
- **Globals keep their values** from one game to the next. Built with Clang, NetHack's and Lua's variables are in sections of their own (`port/nh_sim.h`), which the app copies when it first starts a game and puts back before each new one.

Its tests are in `tests/`: `test_nethack_model.cpp` (the model) and `test_nethack_boot.cpp` (a game started, played, saved by closing the app and restored; and three games in a row, freeing everything). `VIPER_SIM_NETHACK_SHOTS=<dir>` saves their screens.

## License

NetHack is under the NetHack General Public License (`main/nethack/LICENSE`): its source comes with it, and changed versions are shared under the same terms. Lua 5.4.8 is under the MIT license (the notice at the end of `main/lua/lua.h`). The app's own code is MIT, like the rest of this repo.

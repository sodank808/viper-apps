# NetHack

Status: plays in the host simulator and on a Proto1 with gamepad controls in four views: terminal, tiles, small tiles and first person (Phases 1 to 4 of the firmware repo's `docs/plans/nethack_port.md`). Current as of 2026-11-23.

[NetHack 5.0.0](https://www.nethack.org/v500/release.html) as an ELF app: the whole game, unchanged but for a few marked lines, with a window port and a platform of its own for Viper, and the map drawn in NetHack's characters or its tiles. It needs firmware that loads apps bigger than the heap (the ELF loader's spare block), exports what NetHack calls (`sdk/exports/libc.txt`, the double-precision math, stdio reads, time, and `open`/`read`/`write`/`lseek`/`unlink`) and has the system keyboard (`viper_text_entry_open()`, API level 4); older firmware refuses it when it's installed, naming what's missing.

## Playing

| Button | Playing | Choosing a position | In a question | In a menu |
| --- | --- | --- | --- | --- |
| D-pad | Move (two at once: diagonally); held, it repeats | Move the cursor | Left/Right: choose; a direction, where one is asked | Move; Left/Right a page |
| A | What's obvious here (the hints line names it): stairs, pick up, open the door next to you, drink from a fountain, sit on a throne; else search | Pick | Answer; "here" or the list of objects | Choose, or mark (in a list of several) |
| B | Escape | Cancel | Cancel (`n` or `q` where Escape isn't an answer) | Cancel |
| X | Inventory: choose an item for what to do with it | Next monster | All objects (`*`) | Done (several) |
| Y | The command list (eat, read, wear, throw, far look, travel, ...), the last used first | Next object | | |
| L / R | Up / down stairs | Stairs | | Page |
| Start | The game menu: the next view, extended commands, options, help, discoveries, the overview, save and quit | Back to the hero | | Done (several) |
| Select | A short press: the next view (terminal, tiles, small tiles, first person) | | | |

In the first-person view the d-pad is the hero's: Up steps forward, Down back, Left and Right turn (a quarter turn, no game turn), Up or Down with Left or Right steps diagonally, and L and R step aside. A direction question is answered the same way, as the hero faces. While a position is chosen it shows the tile view, for the cursor. The view chosen last is the one the app opens in. Holding Select closes the app, as in any app, and saves the game; opening it again restores it. A game that ends (death, quitting, ascending) closes the app.

A prompt for a line of text (naming things, wishes, engraving) opens the system keyboard: A types the key under the cursor, B deletes, Start is done.

The hero is always "Viper", and its role, race, gender and alignment are chosen with NetHack's own questions (A to let it pick). The options it starts with are in `assets/nethackrc` (no tutorial, no opening story, no tips, autopickup of gold, menus rather than typed lists); a restored game keeps the options it was saved with.

## Layout

- `main/nethack/`: NetHack 5.0.0 (tag `NetHack-5.0.0_Released`): `include/`, `src/`, and `sys/share/pmatchregex.c`. Every change is marked `Viper:`:
  - `config.h` includes `port/nhviper.h` first, and leaves out compression, `chdir()` and the crash-recovery level files (`INSURANCE`).
  - `winprocs.h`, `windows.c`, `mdlib.c`, `role.c`, `rip.c`: the `viper` window port, and NetHack's generic role menus and text tombstone for it.
  - `cfgfiles.c`: `sysconf` is found by prefix, as on Windows.
  - `pline.c`: no message handler program (no processes).
  - `getpos.c`: `getpos()` tells the window port while a position is being chosen, so the app shows a cursor.
  - `src/tile.c` is generated (`tools/make_tile_c.sh`): the glyph map with each glyph's tile (`TILES_IN_GLYPHMAP`, set in `port/nhviper.h`).
  - `include/nhlua.h` is written by hand (upstream's Makefile generates it).
- `main/lua/`: the Lua 5.4.8 NetHack bundles, minus `lua.c` and `luac.c`. `luaconf.h` includes `port/nh_sim.h` (marked `Viper:`).
- `main/port/`: NetHack's window port (`winviper.c`) and platform (`viper_sys.c`: starting a game, the file prefixes, the lock file, the Unix calls NetHack expects), the C interface between them and the app (`nh_port.h`), and the build's settings (`nhviper.h`, `nh_sim.h`, `nh_libc.h`, `nh_stubs.c`, `nh_names.h`).
- `main/nethack_engine.cpp`: the engine's thread, the turn between it and `loop()`, and the game model's side of `nh_port.h`.
- `main/nethack_game.hpp`: the game model (map, messages, status, windows, what the engine waits for). Plain C++.
- `main/nethack_view.cpp`: the views (terminal, tiles, small tiles) and the menus, text windows and questions drawn over them.
- `main/nethack_tiles.cpp`: the tiles, read from `tiles.bin` into a cache as the map needs them, also as the raycaster's textures.
- `main/nethack_first_person.cpp`: the first-person view: the map's cells as the shared raycaster's walls, floors and sprites (`VIPER_LIBS raycast`, `components/raycast` in the firmware repo), and the minimap.
- `main/nethack_app.cpp`: the app (`viper_app_t`), the buttons, the context action, the command list and game menu.
- `upstream/`: what the game's data is made from (`dat/`, `util/`'s `makedefs` and `dlb`, and `win/share/`'s tiles and `tilemap.c`), not built into the app.
- `assets/`: installed beside `app.elf`: `sysconf`, `nethackrc`, and `nhdat` and `tiles.bin` (built, gitignored).
- `tools/make_nhdat.sh`: builds `makedefs` and `dlb` for the host from the vendored sources and packs `assets/nhdat`. The repo's `make` runs it (`tools/build_assets.sh`); so does the firmware repo's simulator build, for the tests.
- `tools/make_tiles.py`: packs `assets/tiles.bin` from `upstream/win/share/*.txt`, numbered as `tile.c` numbers them, with a statue (the monster in grays) for each monster. The repo's `make` runs it too.
- `tools/make_tile_c.sh`: builds upstream's `tilemap` for the host and writes `main/nethack/src/tile.c`. Run it after changing the game's monsters, objects or map symbols.
- `tools/gen_names.py`: writes `port/nh_names.h` (below).

## How it runs

- **The engine's thread.** NetHack is a program that waits for a key wherever it pleases. It runs on a thread of its own (64 KB of stack on a device), and wherever the window port waits for the player (a key, a yes/no, a menu, a text window, `--More--`) it hands the turn to `loop()` with a request and waits for the answer, through a mutex and a condition variable. Only one side runs at a time, so the model needs no locks: the view reads it only while the engine waits. `loop()` waits up to 100 ms for the engine (5 s in the simulator); a level being made carries on into the next frame.
- **The context action** comes from NetHack's own state: before each key it reads, the window port says what's under and next to the hero (stairs, objects the hero can reach, a fountain, the closed doors it remembers; `nhv_context()`), and the app picks A's command from that.
- **The window port** (`port/winviper.c`) draws nothing: it records what NetHack shows into the game model (the map's glyphs, messages, the status lines from NetHack's own `genl_status_update()`, menus and text windows) and turns NetHack's questions into requests. NetHack's menu identifiers stay on its side; the model sees indexes.
- **Files** have prefixes (`fqn_prefix[]`), never a current directory: the data (`nhdat`, `sysconf`, `nethackrc`) in the app's folder, levels, saves and scores in its data folder (`viper_data_path()`).
- **Leaving.** NetHack's `exit()` (the end of a game, an error) goes back to the top of the engine's thread with `longjmp()` (`nhv_exit()`). Closing the app mid-game answers the engine's next questions with Escape and `S` (and "yes" to "Really save?") until it has saved.
- **The views** draw the same model. For the first-person view the window port also says what's under each cell's monster or object (NetHack's background glyph, `iflags.use_background_glyph`): wall, door, floor, corridor, feature or liquid, with its tile. The tile views read each cell's tile, which the window port takes from NetHack's glyph map, so objects' shuffled descriptions get the right pictures. `tiles.bin` (590 KB) stays on the card: the tiles the map shows are read into a cache (120 KB), opened the first time a tile view is used. Without `tiles.bin` the app keeps to the terminal view.
- **Memory.** Every allocation of NetHack's and Lua's goes through `nhv_malloc()` and friends (`port/nh_sim.h`), which keep a list, so the end of a game frees all of it.

## On a device

- The app is a 3.2 MB `app.elf` with 2.0 MB of code, which the loader puts in the firmware's 2 MB spare block (the built-in emulators' ROM buffer), and 0.6 MB of data. Loading it takes about 4 s.
- On a Proto1 the heap peaked at 532 KB in a short game; PSRAM had 1.5 MB free while it ran.
- The terminal and tile views draw in 8 to 9 ms a frame; the first frame after switching to a tile view takes 28 ms, reading its tiles from the card. The first-person view takes 36 to 49 ms a frame, drawn only while a step or turn moves the camera.
- Saving took 1.6 s (a 54 KB save) and restoring 1.4 s.
- Two link-time workarounds, both in `main/`:
  - Each call site of a function from outside the app gets a PLT entry of its own, and past 254 entries binutils 2.43's Xtensa linker asserts (`elf32-xtensa.c`). The C library functions NetHack calls most go through one local wrapper each (`port/nh_libc.h`), on a device only.
  - GCC's induction variables (`-fivopts`) started a loop's pointer in `glyphs.c` far before its table, which the loader can't relocate; that file is built with `-fno-ivopts`.
- Untested on a device so far: long games (memory over time, bones files, deep levels' Lua), the high score list, the timing of making levels in the Gnomish Mines and Sokoban.

## The simulator

The simulator compiles every ELF app into one program, which brings two things a device doesn't have:

- **Names.** NetHack's and Lua's globals (`level`, `flags`, `panic`, `lua_newstate`, ...) would meet other apps' and the firmware's own Lua. `port/nh_names.h`, included through `port/nh_sim.h` by every NetHack, Lua and port file, gives each the prefix `nh_`. Run `tools/gen_names.py` after adding or removing a global.
- **Globals keep their values** from one game to the next. Built with Clang, NetHack's and Lua's variables are in sections of their own (`port/nh_sim.h`), which the app copies when it first starts a game and puts back before each new one.

Its tests are in `tests/`: `test_nethack_model.cpp` (the model, how text windows wrap, and reading `tiles.bin`) and `test_nethack_boot.cpp` (a game started, played, saved by closing the app and restored; the views, and a seeded game played twice, switching views (the first-person one included) every turn the second time, which must be the same game; the first-person view's turning and stepping; the buttons: context action, game menu, options, help, item actions, the position cursor; typing an engraving on the keyboard; and three games in a row, freeing everything). `VIPER_SIM_NETHACK_SHOTS=<dir>` saves their screens.

## License

NetHack, its tiles included, is under the NetHack General Public License (`main/nethack/LICENSE`): its source comes with it, and changed versions are shared under the same terms. Lua 5.4.8 is under the MIT license (the notice at the end of `main/lua/lua.h`). The app's own code is MIT, like the rest of this repo.

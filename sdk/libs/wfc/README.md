# wfc: Wave Function Collapse constraint solver

A finite-domain constraint solver over a 2D grid, built for Wave Function
Collapse and tile autotiling. Plain C++20 with no ESP-IDF dependencies, so it
runs (and is tested) in the host simulator. The WFC app
(`apps/wfc`, an ELF app compiling these sources in) shows it off, and the Circuit screensaver
(`apps/screensavers/circuit`, an ELF app: `VIPER_LIBS wfc`) lays out its
boards with it.

## Pieces

- `wfc/rules.hpp`: `Rules`, which tiles may sit next to which in each direction,
  and each tile's weight. Build them pair by pair with `allow()`/`forbid()`, from
  sockets (below), or learn them from an example map with `learn()` (the
  "simple tiled model"). `Domain` is a set of tile ids. `consistent()` checks a
  finished grid against the rules.
- `wfc/sockets.hpp`: `SocketTiles` describes a tileset tile by tile: each edge
  gets a label read clockwise round the tile, and edges fit when one reads as
  the other backwards, so asymmetric edges (a coastline, a chip's border) line
  up the right way round. It can add quarter-turn rotations of a tile.
- `wfc/solver.hpp`: `Solver`.

## Solver

Each cell keeps the set of tiles it could still be, as a bitset.

1. **Constraints** first: `restrict()`, `ban()`, `fix()` limit cells, and
   `restrict_border()` says what lies beyond an edge. Each change propagates
   straight away, so impossible constraints show up as `Status::Failed`.
2. **step()** picks an undecided cell (lowest weighted Shannon entropy by
   default; `Heuristic::MinCount` and `Scanline` are there too), picks a tile
   for it by weight, and **propagates**: each neighbour drops tiles that nothing
   left in the cell allows beside it, and so on outward (arc consistency).
3. On a **contradiction** (some cell has no tiles left) it backtracks: undoes
   the last choice from an undo trail, rules that tile out for that cell, and
   carries on, going further back if needed. After `max_backtracks` it starts
   over from the constraints with fresh random choices, up to `max_restarts`
   times. Running out of choices to undo proves there's no solution, and it
   reports `Failed` without trying further.

`set_cell_weight()` scales tile weights per cell, which steers results without
ruling anything out (the app uses it to follow a height map, and to prefer a
blockout's exact terrain). Results are deterministic for a seed. Entropy is computed in
`float` (the S3 has no double FPU, and ELF apps get `logf`, not `log`, from
the firmware). `periodic`
makes opposite edges neighbours, for maps that tile.

```cpp
wfc::SocketTiles tiles;
tiles.add("board", {"000", "000", "000", "000"}, 4.0f);
tiles.add("wire", {"0T0", "000", "0T0", "000"}, 1.0f, 2);   // and turned
tiles.add("bend", {"0T0", "0T0", "000", "000"}, 1.0f, 4);

wfc::Options options;
options.width = 20;
options.height = 14;
options.seed = 42;
wfc::Solver solver(tiles.rules(), options);
solver.fix(0, 0, 0);                  // board in the corner
while (solver.step() == wfc::Status::Running) {
    // draw solver.tile(x, y) / solver.count(x, y) to animate it
}
```

## Cost

Memory is a few 64-bit words per cell (one word per 64 tiles) plus the undo
trail, which holds the old domain of each cell a choice changed. Propagating
one changed cell costs (tiles left in it) x (words) per side. A 20x14 map of
the app's 73-tile overworld solves in about 2 ms on a desktop.

#!/usr/bin/env python3
"""Packs NetHack's tiles (upstream/win/share/{monsters,objects,other}.txt)
into tiles.bin, which the app installs beside app.elf and its tile views
read (main/nethack_tiles.cpp).

The tiles are numbered as tile.c numbers them (tools/make_tile_c.sh): the
three files' tiles in that order, then a statue for each monster tile, the
monster drawn in grays as NetHack's tiletext.c draws statues. The count is
checked against tile.c's total_tiles_used.

tiles.bin, little-endian:
    char[8]  "NHTILES1"
    u16      tile width, tile height (16, 16)
    u32      tile count
    u16      palette size, then 0
    u16[]    the palette, RGB565; entry 0 is the tiles' background, black
    u8[]     each tile's pixels, a row at a time, as palette indexes

    nethack/tools/make_tiles.py [out_dir]

out_dir defaults to nethack/assets (gitignored).
"""

import pathlib
import re
import struct
import sys

HERE = pathlib.Path(__file__).resolve().parent.parent
SHARE = HERE / "upstream" / "win" / "share"
TILE_C = HERE / "main" / "nethack" / "src" / "tile.c"
FILES = ["monsters.txt", "objects.txt", "other.txt"]
SIZE = 16

# tiletext.c's graymappings[]: for a statue, the colormap entry (by its
# position in the file's colormap: '.', 'A'..'Z', '0') each one becomes
GRAY_MAPPINGS = [0, 1, 17, 18, 19, 20, 27, 22, 23, 24, 25, 26, 21, 15, 13, 14, 14,
                 1, 17, 18, 19, 20, 27, 22, 23, 24, 25, 20]


def read_tiles(path):
    """The file's colormap (letter -> (r, g, b), in file order) and its
    tiles, each SIZE rows of SIZE letters."""
    colormap = {}
    tiles = []
    rows = None
    for line in path.read_text().splitlines():
        line = line.strip()
        m = re.fullmatch(r"(\S) = \(\s*(\d+),\s*(\d+),\s*(\d+)\)", line)
        if m and rows is None:
            colormap[m.group(1)] = tuple(int(m.group(i)) for i in (2, 3, 4))
        elif line == "{":
            rows = []
        elif line == "}":
            if len(rows) != SIZE or any(len(r) != SIZE for r in rows):
                raise SystemExit(f"{path.name}: tile {len(tiles)} isn't {SIZE}x{SIZE}")
            tiles.append(rows)
            rows = None
        elif rows is not None and line:
            rows.append(line)
    return colormap, tiles


def rgb565(rgb):
    r, g, b = rgb
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def total_tiles_used():
    m = re.search(r"int total_tiles_used = (\d+)", TILE_C.read_text())
    if not m:
        raise SystemExit(f"{TILE_C}: no total_tiles_used; run tools/make_tile_c.sh")
    return int(m.group(1))


def main():
    out = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else HERE / "assets"
    palette = [(0, 0, 0)]  # the background ('.'), drawn black
    index_of = {}

    def index(rgb):
        if rgb not in index_of:
            index_of[rgb] = len(palette)
            palette.append(rgb)
        return index_of[rgb]

    pixels = []  # bytes per tile
    monsters = None
    for name in FILES:
        colormap, tiles = read_tiles(SHARE / name)
        letters = list(colormap)
        background = letters[0]
        for rows in tiles:
            pixels.append(bytes(0 if ch == background else index(colormap[ch])
                                for row in rows for ch in row))
        if name == "monsters.txt":
            monsters = (colormap, letters, tiles)

    # The statues: each monster tile in grays, but for the last (the
    # statue patch in tilemap.c makes one less)
    colormap, letters, tiles = monsters
    expected = total_tiles_used()
    for rows in tiles[: expected - len(pixels)]:
        tile = []
        for row in rows:
            for ch in row:
                k = letters.index(ch)
                gray = letters[GRAY_MAPPINGS[k]] if k < len(GRAY_MAPPINGS) else ch
                tile.append(0 if gray == letters[0] else index(colormap[gray]))
        pixels.append(bytes(tile))
    if len(pixels) != expected:
        raise SystemExit(f"{len(pixels)} tiles, but tile.c numbers {expected}")
    if len(palette) > 256:
        raise SystemExit(f"{len(palette)} colors don't fit in a byte")

    out.mkdir(parents=True, exist_ok=True)
    data = bytearray(b"NHTILES1")
    data += struct.pack("<HHIHH", SIZE, SIZE, len(pixels), len(palette), 0)
    data += struct.pack(f"<{len(palette)}H", *(rgb565(c) for c in palette))
    for p in pixels:
        data += p
    (out / "tiles.bin").write_bytes(data)
    print(f"{out / 'tiles.bin'}: {len(pixels)} tiles, {len(palette)} colors, "
          f"{len(data)} bytes")


if __name__ == "__main__":
    main()

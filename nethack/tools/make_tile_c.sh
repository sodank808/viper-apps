#!/bin/sh
# Writes main/nethack/src/tile.c, the glyph map with each glyph's tile
# (TILES_IN_GLYPHMAP), as NetHack's own build does: tilemap, built here for
# the host from the vendored sources, numbers the tiles of
# upstream/win/share/{monsters,objects,other}.txt in that order.
#
#   nethack/tools/make_tile_c.sh
#
# tile.c is committed, without the comment naming each glyph's tile (a third
# of its size); run this after changing the game's monsters, objects or map
# symbols, or the tile files' lists.

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/util" "$work/src"

inc="-I$here/main/nethack/include -I$here/main/port -I$here/main/lua"
# A plain host build: no window port, no renamed names (port/nh_sim.h)
flags="-std=gnu99 -w -DNH_HOST_TOOL"
src=$here/main/nethack/src

cd "$work/util"
$cc $flags $inc -o tilemap "$here/upstream/win/share/tilemap.c" "$src/monst.c" \
    "$src/objects.c" "$src/drawing.c" "$src/hacklib.c" "$src/alloc.c" \
    "$here/upstream/util/panic.c"
./tilemap
sed -e 's|   /\* \[[0-9]*\] .*\*/$||' "$work/src/tile.c" > "$src/tile.c"
echo "$src/tile.c: $(grep -c '^    {' "$src/tile.c") glyphs"

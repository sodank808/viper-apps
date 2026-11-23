#!/bin/sh
# Builds nethack/assets/nhdat, the game's data files (help texts, rumors,
# oracles, the Lua level and dungeon descriptions) packed into one file, as
# NetHack's own build does: makedefs makes the generated texts from
# upstream/dat, and dlb packs them. Both are built here for the host from the
# vendored sources.
#
#   nethack/tools/make_nhdat.sh [out_dir]
#
# out_dir defaults to nethack/assets (gitignored); `make` installs what's
# there beside app.elf. The simulator's build runs it into its build folder.

set -eu

here=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$here/assets}
cc=${CC:-cc}

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/util" "$work/dat" "$work/include" "$out"
cp "$here"/upstream/dat/* "$work/dat/"

inc="-I$here/main/nethack/include -I$here/main/port -I$here/main/lua"
# A plain host build: no window port, no renamed names (port/nh_sim.h)
flags="-std=gnu99 -w -DNH_HOST_TOOL -DDLB"
src=$here/main/nethack/src
util=$here/upstream/util

cd "$work/util"
$cc $flags $inc -o makedefs "$util/makedefs.c" "$src/monst.c" "$src/objects.c" \
    "$src/date.c" "$src/alloc.c" "$util/panic.c" "$src/hacklib.c"
$cc $flags $inc -o dlb "$util/dlb_main.c" "$src/dlb.c" "$src/alloc.c" "$util/panic.c" \
    "$src/hacklib.c"

./makedefs -d # data
./makedefs -r # rumors
./makedefs -h # oracles
./makedefs -2 # engrave
./makedefs -1 # epitaph
./makedefs -3 # bogusmon
./makedefs -v # options (and ../include/date.h, which the game no longer reads)

# The list in sys/unix/Makefile.top's dlb target
cd "$work/dat"
LC_ALL=C
../util/dlb cf nhdat help hh cmdhelp keyhelp history opthelp optmenu usagehlp wizhelp \
    dungeon.lua tribute asmodeus.lua baalz.lua bigrm-*.lua castle.lua fakewiz?.lua \
    juiblex.lua knox.lua medusa-?.lua minend-?.lua minefill.lua minetn-?.lua oracle.lua \
    orcus.lua sanctum.lua soko?-?.lua tower?.lua valley.lua wizard?.lua nhcore.lua nhlib.lua \
    themerms.lua astral.lua air.lua earth.lua fire.lua water.lua hellfill.lua tut-?.lua \
    ???-goal.lua ???-fil?.lua ???-loca.lua ???-strt.lua bogusmon data engrave epitaph \
    oracles options quest.lua rumors
cp nhdat "$out/nhdat"
echo "$out/nhdat: $(wc -c < "$out/nhdat" | tr -d ' ') bytes"

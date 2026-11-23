#!/bin/sh
# Run by the repo's Makefile before NetHack is built: packs its data into
# assets/nhdat (make_nhdat.sh) and its tiles into assets/tiles.bin
# (make_tiles.py), which `make` installs beside app.elf
set -eu
here=$(dirname "$0")
"$here/make_nhdat.sh"
python3 -I "$here/make_tiles.py"

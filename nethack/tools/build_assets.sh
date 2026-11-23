#!/bin/sh
# Run by the repo's Makefile before NetHack is built: packs its data into
# assets/nhdat (make_nhdat.sh), which `make` installs beside app.elf
exec "$(dirname "$0")/make_nhdat.sh"

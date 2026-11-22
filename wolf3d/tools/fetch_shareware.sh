#!/bin/bash
# Downloads the shareware episode of Wolfenstein 3D (v1.4, id Software's
# 1wolf14.zip, freely distributable unmodified), checks it, and puts its data
# files (*.WL1) in wolf3d/assets/ (gitignored), which `make` installs beside
# the app's app.elf, where it looks for them; or in the folder given.
#
#     wolf3d/tools/fetch_shareware.sh [dest]
#
# Needs curl, unzip and 7-Zip (7zz or 7z; `brew install sevenzip`): the data
# is in an ARJ self-extracting archive inside the zip.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
dest="${1:-$here/../assets}"
url="https://www.gamers.org/pub/games/wolf3d/official/1wolf14.zip"
md5_expected="e6d75d885e408003d2f4751d8dfd1cb0"

sevenzip="$(command -v 7zz || command -v 7z || true)"
if [ -z "$sevenzip" ]; then
    echo "Needs 7-Zip (7zz or 7z) to unpack the ARJ archive: brew install sevenzip" >&2
    exit 1
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

echo "Downloading $url"
curl -fsSL -o "$work/1wolf14.zip" "$url"
if command -v md5 >/dev/null; then
    md5_got="$(md5 -q "$work/1wolf14.zip")"
else
    md5_got="$(md5sum "$work/1wolf14.zip" | cut -d' ' -f1)"
fi
if [ "$md5_got" != "$md5_expected" ]; then
    echo "1wolf14.zip's MD5 is $md5_got, not $md5_expected" >&2
    exit 1
fi

mkdir -p "$work/zip" "$work/data"
unzip -q "$work/1wolf14.zip" -d "$work/zip"
cat "$work/zip/WOLF3D1.1" "$work/zip/WOLF3D1.2" "$work/zip/WOLF3D1.3" > "$work/WOLF3D1"
"$sevenzip" x -y -o"$work/data" "$work/WOLF3D1" >/dev/null

mkdir -p "$dest"
cp "$work"/data/*.WL1 "$dest/"
echo "Installed into $dest:"
ls -l "$dest"/*.WL1

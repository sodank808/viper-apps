#!/bin/sh
# Installs a built ELF app project into the fatfs apps folder (the Makefile
# runs it after `idf.py elf`):
#
#     install_app.sh <app dir> <apps folder> <bundle prefix>
#
# A project with one app (project_elf()) goes in <prefix><app>/: its
# build/<app>.app.elf as app.elf, its info.json, icon.png, preview.png and
# screenshot.gif or .png (the launch card's, docs/guides/manifest/
# launch_details.md) if it has them, and the contents of its assets/ folder. A project with several
# (project_elfs(), e.g. screensavers/) installs each folder with an info.json
# the same way, as <prefix><project>.<folder>/.
set -e

app_dir=${1%/}
apps_root=$2
prefix=$3
name=$(basename "$app_dir")

# install_one <built elf> <folder with its info.json and assets> <bundle id>
install_one() {
    if [ ! -f "$1" ]; then
        echo "  Error: $1 not found"
        exit 1
    fi
    dest="$apps_root/$3"
    mkdir -p "$dest"
    cp "$1" "$dest/app.elf"
    for extra in info.json icon.png preview.png screenshot.gif screenshot.png; do
        if [ -f "$2/$extra" ]; then cp "$2/$extra" "$dest/"; fi
    done
    if [ -d "$2/assets" ]; then cp -R "$2/assets/." "$dest/"; fi
    echo "  Installed $3"
}

if grep -q '^project_elfs(' "$app_dir/CMakeLists.txt"; then
    for manifest in "$app_dir"/*/info.json; do
        folder=$(dirname "$manifest")
        sub=$(basename "$folder")
        install_one "$app_dir/build/$sub.app.elf" "$folder" "$prefix$name.$sub"
    done
else
    install_one "$app_dir/build/$name.app.elf" "$app_dir" "$prefix$name"
fi

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
#
# The installed info.json gets "min_api": the app's API level, the oldest
# firmware app API it runs on (check_app_elf.py --api-level). The project's
# own info.json is left as it is.
set -e

# check_app_elf.py: beside this script in the app SDK (sdk/tools/), or in the
# firmware's tree when this is viper's apps/
here=$(cd "$(dirname "$0")" && pwd)
checker="$here/check_app_elf.py"
[ -f "$checker" ] || checker="$here/../components/elf_loader/tool/check_app_elf.py"
python=${PYTHON:-python3}

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
    level=$("$python" "$checker" --api-level "$1")
    if [ -f "$dest/info.json" ]; then
        "$python" - "$dest/info.json" "$level" <<'PY'
import json, sys
path, level = sys.argv[1], int(sys.argv[2])
with open(path) as f:
    manifest = json.load(f)
manifest["min_api"] = level
with open(path, "w") as f:
    json.dump(manifest, f, indent=4)
    f.write("\n")
PY
    fi
    echo "  Installed $3 (API level $level)"
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

# viper-apps

Apps for Viper, a small operating system for ESP32-S3 handheld game devices. Viper loads these at run time as ELF files from the SD card, so they're built and shipped apart from the firmware.

This repo holds the apps whose source is public, starting with ports of GPL-licensed games. Each app is its own ESP-IDF project, built against the Viper app SDK in [`sdk/`](sdk/).

## Building

You need [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/get-started/) at the version in [`sdk/VERSION`](sdk/VERSION) (apps use the firmware's C++ runtime, so the compiler has to match), with its environment loaded (`. $IDF_PATH/export.sh`).

```bash
make
```

```bash
make APP=doom
```

Each app is built with `idf.py elf` and installed into `out/`, as `out/sdcard/elf_apps/app.viper.<app>/` (apps with large assets) or `out/fatfs/apps/app.viper.<app>/`: its `app.elf`, `info.json`, icon and assets. After every link, `sdk/tools/check_app_elf.py` checks that Viper's loader can load the file and that every function it calls is one the firmware exports.

## Installing on a device

Copy each `app.viper.<app>/` folder from `out/` into `apps/` on the device's SD card. Viper adds it to the home screen at the next boot, or right away if the card was shared over USB from the Add App app.

## Writing an app

An app returns a `viper_app_t` from `app_main()` (the `VIPER_APP()` macro): callbacks for create, resume, pause, destroy and a per-frame loop, plus whether it draws with LVGL or straight to the framebuffer. Everything it can call is in [`sdk/include/viper_app.h`](sdk/include/viper_app.h) and LVGL's headers, and the shared game libraries in [`sdk/libs/`](sdk/libs/) (`set(VIPER_LIBS gfx game)` in the project's `CMakeLists.txt`). [`sdk/README.md`](sdk/README.md) shows a project's `CMakeLists.txt`. Apps can be C or C++ (no exceptions or RTTI).

## The SDK and firmware versions

`sdk/` is generated from the Viper firmware repo; don't edit it here. Its version is `<ABI>.<minor>.<patch>`: the major version is the app ABI in `viper_app.h`, and firmware runs apps built for any ABI in its supported range. Functions are only ever added to what the firmware exports ([`sdk/exports.lock`](sdk/exports.lock) lists every one). An app that calls a function newer than the firmware on a device is refused when it's installed, with a message naming the function; update the firmware to run it. SDK releases are tagged `sdk-v<version>`.

## Licenses

The repo, the SDK and the tooling are MIT ([`LICENSE`](LICENSE); LVGL's headers are MIT too, [`sdk/lvgl/LICENCE.txt`](sdk/lvgl/LICENCE.txt)). An app that includes third-party code keeps that code's license in its own folder, and the table below lists it. Each app is a separate program the firmware loads at run time, so an app's license covers that app.

| App | License | Third-party code |
| --- | --- | --- |
| [Doom](doom/) | GPL-2.0 | PrBoom, by way of [esp-cpp/prboom](https://github.com/esp-cpp/prboom) (`doom/main/prboom/COPYING`) |
| [Wolfenstein 3D](wolf3d/) | GPL-2.0 | [Wolf4SDL](https://github.com/KS-Presto/Wolf4SDL), the port of id Software's source, with DOSBox's OPL emulator (`wolf3d/main/engine/license-gpl.txt`, `license-id.txt`) |

Game data (WADs, ROMs) is never committed here except where its license allows redistribution.

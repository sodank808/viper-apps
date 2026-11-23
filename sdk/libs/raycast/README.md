# raycast

Status: implemented; current as of 2026-11-23. Used by Maze3D (`apps/screensavers/maze3d`) and NetHack's first-person view (`apps_public/nethack`).

A Wolfenstein 3D-style raycaster: textured walls on a grid, textured floors and ceilings, and billboard sprites, drawn into an RGB565 buffer. Plain C++, header-only, with no LVGL, ESP-IDF or `System` calls, so built-in apps, ELF apps and the sim's tests use the same code.

- **Built-in apps:** add `raycast` to the component's `REQUIRES`.
- **ELF apps:** `set(VIPER_LIBS raycast)` (with any others) before `project_elf()`.
- **Tests:** `sim/tests/test_raycast.cpp` (`[raycast]`), and Maze3D's (`apps/screensavers/maze3d/tests`, `[maze3d]`), which compare its frames pixel by pixel.

## Use

```cpp
#include "raycast/raycaster.hpp"

// What's where: any class with these, called only for cells on the map
struct Map {
    int width() const;
    int height() const;
    const raycast::Texture *wall(int x, int y) const;    // nullptr: open
    const raycast::Texture *floor(int x, int y) const;   // nullptr: black
    const raycast::Texture *ceiling(int x, int y) const; // nullptr: black
};

raycast::Raycaster raycaster(320, 168);      // the frame's size; fov 66 degrees
raycaster.set_wall_top(color);               // the tops of walls lower than eye level
const raycast::Camera camera{x, y, angle};   // cells; radians, 0 east, pi/2 south
raycaster.render(frame, map, camera, 1.0f, sprites, sprite_count);
```

- **Textures** are square, a power of two on a side (`log2_size`), column-major: texel (u, v) at `texels[(u << log2_size) + v]`. Each cell's wall, floor and ceiling can have its own, of any size: Maze3D uses 64x64 art, NetHack its 16x16 tiles.
- **Sprites** are camera-facing squares (`x`, `y`, `z` for the height of their center, `size`) drawn with a texture in which `raycast::kClear` (magenta, `0xF81F`) texels are see-through.
- **Wall height** (0 to 1) applies to every wall. Below eye level (0.5) the tops show and you see over them, as when Maze3D's walls sink and rise.
- **Cost:** each column keeps up to `kMaxLayers` (10) layers (wall faces, wall tops, sprite slices), nearest first, and the frame is then filled a row at a time, so writes to the frame (in PSRAM on a device) are sequential. `layer_count()` says how many the last frame had. Walls facing north and south are drawn at 3/4 brightness (`shade()`).
- The map is a template parameter, so its calls inline: a map that returns the same texture everywhere (Maze3D's) costs what a raycaster written for one texture does.

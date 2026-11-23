#pragma once

// The first-person view: the map seen from the hero's cell, facing one of
// four directions, drawn by the shared raycaster (components/raycast in the
// firmware repo, VIPER_LIBS raycast). Walls, doors and floors are textured
// with their own tiles, what the hero remembers but doesn't see is dimmed,
// unexplored rock is a stone texture of its own, and monsters, objects and
// features (stairs, altars, fountains, traps) stand on their cells as
// billboards of their tiles. A minimap sits in a corner.
//
// Where the camera is (the hero's cell, or between two while a step or a turn
// is tweened) is the app's; this only draws.

#include <memory>
#include <vector>

#include "gfx/surface.hpp"
#include "nethack_game.hpp"
#include "raycast/raycaster.hpp"

namespace nethack {

class TileSet;

// Facing: 0 north (-y), 1 east, 2 south, 3 west; turning right adds 1
inline float facing_angle(int facing) { return (facing - 1) * 1.5707963f; }

class FirstPerson {
  public:
    // Draws into the rows [top, top + height) of s; the camera in map cells
    void draw(gfx::Surface &s, int top, int height, const Model &m, TileSet &tiles,
              const raycast::Camera &camera, int facing);

    // Sprites drawn in the last frame (for the tests)
    int sprite_count() const { return static_cast<int>(sprites_.size()); }

  private:
    struct Map {
        const raycast::Texture *const *walls;
        const raycast::Texture *const *floors;
        const raycast::Texture *const *ceilings;

        int width() const { return kCols; }
        int height() const { return kRows; }
        const raycast::Texture *wall(int x, int y) const { return walls[y * kCols + x]; }
        const raycast::Texture *floor(int x, int y) const { return floors[y * kCols + x]; }
        const raycast::Texture *ceiling(int x, int y) const { return ceilings[y * kCols + x]; }
    };
    void build(const Model &m, TileSet &tiles, const raycast::Camera &camera);
    void draw_minimap(gfx::Surface &s, int top, int height, const Model &m, int facing);

    std::unique_ptr<raycast::Raycaster> raycaster_;
    // This frame's textures by cell, [y * kCols + x]
    std::vector<const raycast::Texture *> walls_, floors_, ceilings_;
    std::vector<raycast::Sprite> sprites_;
};

} // namespace nethack

#pragma once

// A Wolfenstein 3D-style raycaster: textured walls on a grid, textured floor
// and ceiling, and billboard sprites, drawn into an RGB565 buffer. Plain C++,
// header-only, with no platform calls, so built-in apps, ELF apps
// (VIPER_LIBS raycast) and the sim's tests share it. See ../../README.md.
//
// Walls can be any height from 0 to 1. Below eye level you can see over them,
// so each column keeps a short list of layers, nearest first: the faces and
// tops of the walls along its ray, and sprite slices, each covering a run of
// rows. Pixels are then filled a row at a time, taking each from the nearest
// layer that covers it, else the floor or ceiling. Row order keeps writes to
// the frame (in PSRAM on a device) sequential.
//
// What's where comes from a map the caller supplies, a class with:
//
//     int width() const;                         // cells
//     int height() const;
//     const raycast::Texture *wall(int x, int y) const;    // nullptr: open
//     const raycast::Texture *floor(int x, int y) const;   // nullptr: black
//     const raycast::Texture *ceiling(int x, int y) const; // nullptr: black
//
// called only for cells on the map (0 <= x < width, 0 <= y < height). Off the
// map, rays stop and floor and ceiling are black.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>

namespace raycast {

/// A square texture whose side is a power of two, column-major: texel (u, v)
/// at texels[(u << log2_size) + v]
struct Texture {
    const uint16_t *texels = nullptr;
    uint8_t log2_size = 6; // 64x64

    int size() const { return 1 << log2_size; }
};

/// Texels of this color are transparent in sprites
inline constexpr uint16_t kClear = 0xF81F;

struct Camera {
    float x = 0.0f;     // cells
    float y = 0.0f;     // cells
    float angle = 0.0f; // radians, 0 = east (+x), pi/2 = south (+y)
};

/// A camera-facing square, drawn with `texture` (kClear texels transparent)
struct Sprite {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.5f;    // height of its center (floor 0, ceiling 1)
    float size = 0.5f; // world units on a side
    const Texture *texture = nullptr;
};

class Raycaster {
  public:
    static constexpr int kMaxLayers = 10;
    static constexpr float kEyeHeight = 0.5f;

    Raycaster(int width, int height, float fov_degrees = 66.0f)
        : width_(width)
        , height_(height)
        , plane_scale_(std::tan(fov_degrees * std::numbers::pi_v<float> / 360.0f))
        , focal_(width / 2.0f / plane_scale_)
        , layers_(static_cast<size_t>(kMaxLayers) * width)
        , counts_(width) {}

    /// The tops of walls lower than eye level are drawn flat in this color
    void set_wall_top(uint16_t color) { wall_top_ = color; }

    /// Draw a frame into `out`: width x height RGB565 pixels, row by row.
    /// wall_height (0 to 1) is every wall's height.
    template <typename Map>
    void render(uint16_t *out, const Map &map, const Camera &camera, float wall_height,
                const Sprite *sprites, int sprite_count);

    int width() const { return width_; }
    int height() const { return height_; }
    /// Layers in the last frame, summed over columns (a measure of its cost)
    int layer_count() const { return layer_count_; }

    /// 3/4 brightness: walls facing north and south are drawn this way
    static uint16_t shade(uint16_t c) { return ((c >> 1) & 0x7BEF) + ((c >> 2) & 0x39E7); }

  private:
    struct Layer {
        const uint16_t *column; // texture column, or null for a flat color
        int32_t v_base;         // texel row at screen row y is (v_base + y * v_step) >> 16
        int32_t v_step;
        float depth;
        int16_t top; // rows [top, bottom)
        int16_t bottom;
        uint16_t color; // flat color when column is null
        uint8_t flags;
        uint8_t mask; // the texture's size - 1
    };
    static constexpr uint8_t kShaded = 1;      // darken (walls facing north and south)
    static constexpr uint8_t kTransparent = 2; // skip kClear texels
    static constexpr int32_t kOne = 1 << 16;   // 16.16 fixed point
    static constexpr float kNear = 0.05f;      // closest a wall can be, to keep the math finite

    // The first pixel whose center is at or past coordinate `p`
    static int first_pixel(float p) { return static_cast<int>(std::ceil(p - 0.5f)); }

    template <typename Map>
    void cast_column(int x, const Map &map, const Camera &camera, float dir_x, float dir_y,
                     float wall_height);
    void add_sprite(const Sprite &sprite, const Camera &camera);
    /// Insert into column x's list by depth; drops the farthest if full
    void insert_layer(int x, const Layer &layer);
    Layer *column_layer(int x, int i) { return &layers_[static_cast<size_t>(i) * width_ + x]; }

    int width_;
    int height_;
    float plane_scale_; // tan(fov / 2)
    float focal_;       // pixels per world unit at distance 1
    uint16_t wall_top_ = 0;
    std::vector<Layer> layers_; // kMaxLayers per column, [layer * width + x]
    std::vector<uint8_t> counts_;
    int layer_count_ = 0;
};

inline void Raycaster::insert_layer(int x, const Layer &layer) {
    int n = counts_[x];
    if (n == kMaxLayers) {
        if (column_layer(x, n - 1)->depth <= layer.depth)
            return;
        --n;
    } else {
        counts_[x] = static_cast<uint8_t>(n + 1);
    }
    // Shift farther layers back one
    int i = n;
    while (i > 0 && column_layer(x, i - 1)->depth > layer.depth) {
        *column_layer(x, i) = *column_layer(x, i - 1);
        --i;
    }
    *column_layer(x, i) = layer;
}

template <typename Map>
void Raycaster::cast_column(int x, const Map &map, const Camera &camera, float dir_x, float dir_y,
                            float wall_height) {
    const float horizon = height_ / 2.0f;
    const float cam = 2.0f * (x + 0.5f) / width_ - 1.0f;
    const float ray_x = dir_x - dir_y * plane_scale_ * cam;
    const float ray_y = dir_y + dir_x * plane_scale_ * cam;

    int map_x = static_cast<int>(std::floor(camera.x)),
        map_y = static_cast<int>(std::floor(camera.y));
    const float delta_x = ray_x == 0.0f ? 1e30f : std::fabs(1.0f / ray_x);
    const float delta_y = ray_y == 0.0f ? 1e30f : std::fabs(1.0f / ray_y);
    const int step_x = ray_x < 0 ? -1 : 1, step_y = ray_y < 0 ? -1 : 1;
    float side_x = (ray_x < 0 ? camera.x - map_x : map_x + 1.0f - camera.x) * delta_x;
    float side_y = (ray_y < 0 ? camera.y - map_y : map_y + 1.0f - camera.y) * delta_y;

    // At or above eye level the first wall hides everything behind it
    const bool see_over = wall_height < kEyeHeight;
    bool in_wall = false;
    float wall_in = 0.0f; // distance to the face of the wall we're in
    auto top_row = [&](float distance) {
        return horizon + (kEyeHeight - wall_height) * focal_ / distance;
    };
    auto add_top = [&](float distance_out) {
        // The flat top of a wall, between where the ray enters and leaves it
        Layer top{};
        top.top = static_cast<int16_t>(std::clamp(first_pixel(top_row(distance_out)), 0, height_));
        top.bottom = static_cast<int16_t>(std::clamp(first_pixel(top_row(wall_in)), 0, height_));
        top.depth = wall_in;
        top.color = wall_top_;
        if (top.top < top.bottom)
            insert_layer(x, top);
    };

    while (counts_[x] < kMaxLayers) {
        int side;
        if (side_x < side_y) {
            side_x += delta_x;
            map_x += step_x;
            side = 0;
        } else {
            side_y += delta_y;
            map_y += step_y;
            side = 1;
        }
        const float distance = std::max(side == 0 ? side_x - delta_x : side_y - delta_y, kNear);
        const bool outside =
            map_x < 0 || map_y < 0 || map_x >= map.width() || map_y >= map.height();
        const Texture *wall = outside ? nullptr : map.wall(map_x, map_y);
        if (in_wall && !wall) {
            add_top(distance);
            in_wall = false;
        }
        if (outside)
            break;
        if (!wall || in_wall)
            continue;

        // A wall face. u runs left to right as you look at it.
        const int size = wall->size();
        float hit = side == 0 ? camera.y + distance * ray_y : camera.x + distance * ray_x;
        hit -= std::floor(hit);
        int u = static_cast<int>(hit * size);
        if ((side == 0 && ray_x < 0) || (side == 1 && ray_y > 0))
            u = size - 1 - u;
        const float scale = focal_ / distance;
        const float top = horizon + (kEyeHeight - wall_height) * scale;
        const float bottom = horizon + kEyeHeight * scale;
        Layer face{};
        face.top = static_cast<int16_t>(std::clamp(first_pixel(top), 0, height_));
        face.bottom = static_cast<int16_t>(std::clamp(first_pixel(bottom), 0, height_));
        if (face.top < face.bottom) {
            // The wall rises out of the floor: its top edge is the top of the texture
            const float v_step = size / scale;
            face.column = wall->texels + (u << wall->log2_size);
            face.mask = static_cast<uint8_t>(size - 1);
            face.v_step = static_cast<int32_t>(v_step * kOne);
            face.v_base = static_cast<int32_t>((0.5f - top) * v_step * kOne);
            face.depth = distance;
            face.flags = side == 1 ? kShaded : 0;
            insert_layer(x, face);
        }
        if (!see_over)
            break;
        in_wall = true;
        wall_in = distance;
    }
}

inline void Raycaster::add_sprite(const Sprite &sprite, const Camera &camera) {
    if (!sprite.texture || !sprite.texture->texels)
        return;
    const float dir_x = std::cos(camera.angle), dir_y = std::sin(camera.angle);
    const float rel_x = sprite.x - camera.x, rel_y = sprite.y - camera.y;
    const float depth = rel_x * dir_x + rel_y * dir_y;
    if (depth < 0.2f)
        return;
    const float lateral = -rel_x * dir_y + rel_y * dir_x;
    const float scale = focal_ / depth;
    const float center_x = width_ / 2.0f * (1.0f + lateral / (depth * plane_scale_));
    const float center_y = height_ / 2.0f + (kEyeHeight - sprite.z) * scale;
    const float size = sprite.size * scale;
    const float left = center_x - size / 2, top = center_y - size / 2;

    const int x0 = std::max(first_pixel(left), 0), x1 = std::min(first_pixel(left + size), width_);
    const int y0 = std::max(first_pixel(top), 0), y1 = std::min(first_pixel(top + size), height_);
    if (x0 >= x1 || y0 >= y1)
        return;
    const Texture &t = *sprite.texture;
    const int texture_size = t.size();
    const float texels_per_pixel = texture_size / size;
    Layer slice{};
    slice.top = static_cast<int16_t>(y0);
    slice.bottom = static_cast<int16_t>(y1);
    slice.v_step = static_cast<int32_t>(texels_per_pixel * kOne);
    slice.v_base = static_cast<int32_t>((0.5f - top) * texels_per_pixel * kOne);
    slice.depth = depth;
    slice.flags = kTransparent;
    slice.mask = static_cast<uint8_t>(texture_size - 1);
    for (int x = x0; x < x1; ++x) {
        const int u =
            std::clamp(static_cast<int>((x + 0.5f - left) * texels_per_pixel), 0, texture_size - 1);
        slice.column = t.texels + (u << t.log2_size);
        insert_layer(x, slice);
    }
}

template <typename Map>
void Raycaster::render(uint16_t *out, const Map &map, const Camera &camera, float wall_height,
                       const Sprite *sprites, int sprite_count) {
    const float dir_x = std::cos(camera.angle), dir_y = std::sin(camera.angle);
    std::fill(counts_.begin(), counts_.end(), 0);
    if (wall_height > 0.001f) {
        for (int x = 0; x < width_; ++x)
            cast_column(x, map, camera, dir_x, dir_y, wall_height);
    }
    for (int i = 0; i < sprite_count; ++i)
        add_sprite(sprites[i], camera);
    layer_count_ = 0;
    for (int x = 0; x < width_; ++x)
        layer_count_ += counts_[x];

    const float horizon = height_ / 2.0f;
    const float plane_x = -dir_y * plane_scale_, plane_y = dir_x * plane_scale_;
    const int map_width = map.width(), map_height = map.height();
    const uint32_t map_w = static_cast<uint32_t>(map_width) << 16;
    const uint32_t map_h = static_cast<uint32_t>(map_height) << 16;
    const uint8_t *counts = counts_.data();

    for (int y = 0; y < height_; ++y) {
        // Where this row's floor (or ceiling) is, along the leftmost ray, and
        // how far it moves per pixel
        const bool is_floor = y + 0.5f > horizon;
        const float rise = is_floor ? kEyeHeight : 1.0f - kEyeHeight;
        const float distance = rise * focal_ / std::fabs(y + 0.5f - horizon);
        const float first = 1.0f / width_ - 1.0f;
        int32_t fx = static_cast<int32_t>((camera.x + distance * (dir_x + plane_x * first)) * kOne);
        int32_t fy = static_cast<int32_t>((camera.y + distance * (dir_y + plane_y * first)) * kOne);
        const int32_t step_x = static_cast<int32_t>(distance * plane_x * 2.0f / width_ * kOne);
        const int32_t step_y = static_cast<int32_t>(distance * plane_y * 2.0f / width_ * kOne);

        uint16_t *row = out + static_cast<size_t>(y) * width_;
        for (int x = 0; x < width_; ++x, fx += step_x, fy += step_y) {
            const int n = counts[x];
            int i = 0;
            for (; i < n; ++i) {
                const Layer &layer = layers_[static_cast<size_t>(i) * width_ + x];
                if (y < layer.top || y >= layer.bottom)
                    continue;
                if (!layer.column) {
                    row[x] = layer.color;
                    break;
                }
                const uint16_t t =
                    layer.column[((layer.v_base + y * layer.v_step) >> 16) & layer.mask];
                if ((layer.flags & kTransparent) && t == kClear)
                    continue;
                row[x] = (layer.flags & kShaded) ? shade(t) : t;
                break;
            }
            if (i < n)
                continue;
            if (static_cast<uint32_t>(fx) < map_w && static_cast<uint32_t>(fy) < map_h) {
                const int cx = fx >> 16, cy = fy >> 16;
                const Texture *flat = is_floor ? map.floor(cx, cy) : map.ceiling(cx, cy);
                if (flat) {
                    const int shift = 16 - flat->log2_size;
                    const int mask = flat->size() - 1;
                    row[x] = flat->texels[(((fx >> shift) & mask) << flat->log2_size) |
                                          ((fy >> shift) & mask)];
                } else {
                    row[x] = 0;
                }
            } else {
                row[x] = 0;
            }
        }
    }
}

} // namespace raycast

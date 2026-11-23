// The first-person view (nethack_first_person.hpp).

#include "nethack_first_person.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "nethack_tiles.hpp"

namespace nethack {
namespace {

// Cells farther than this (in either direction) get no sprite
constexpr int kSpriteRange = 10;
// The most sprites drawn, the nearest
constexpr size_t kMaxSprites = 48;

// A 16x16 texture of a color with some noise, column-major: rock for cells
// never seen, the ceiling over the rest
struct Noise {
    std::array<uint16_t, 16 * 16> texels{};
    raycast::Texture texture;
    Noise(int r, int g, int b, int spread, uint32_t seed) {
        uint32_t x = seed;
        for (auto &t : texels) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            const int d = static_cast<int>(x % (2 * spread + 1)) - spread;
            t = gfx::rgb(std::clamp(r + d, 0, 255), std::clamp(g + d, 0, 255),
                         std::clamp(b + d, 0, 255));
        }
        texture = raycast::Texture{texels.data(), 4};
    }
};

const Noise &rock() {
    static const Noise noise(46, 42, 38, 10, 0x2545F491u);
    return noise;
}
const Noise &ceiling() {
    static const Noise noise(22, 22, 30, 4, 0x9E3779B9u);
    return noise;
}

// The minimap's colors: NetHack's CLR_* for what stands on a cell
constexpr gfx::Pixel kColors[16] = {
    gfx::rgb(85, 85, 95),    gfx::rgb(205, 40, 40),   gfx::rgb(40, 170, 40),
    gfx::rgb(170, 110, 30),  gfx::rgb(60, 80, 220),   gfx::rgb(170, 50, 170),
    gfx::rgb(40, 170, 170),  gfx::rgb(190, 190, 190), gfx::rgb(190, 190, 190),
    gfx::rgb(255, 140, 30),  gfx::rgb(90, 255, 90),   gfx::rgb(255, 255, 70),
    gfx::rgb(110, 140, 255), gfx::rgb(255, 100, 255), gfx::rgb(100, 255, 255),
    gfx::rgb(255, 255, 255),
};

gfx::Pixel terrain_color(uint8_t terrain) {
    switch (terrain) {
    case NHV_TERRAIN_WALL:
        return gfx::rgb(150, 150, 160);
    case NHV_TERRAIN_DOOR:
        return gfx::rgb(170, 110, 30);
    case NHV_TERRAIN_FLOOR:
        return gfx::rgb(60, 60, 70);
    case NHV_TERRAIN_CORRIDOR:
        return gfx::rgb(90, 90, 90);
    case NHV_TERRAIN_FEATURE:
        return gfx::rgb(220, 200, 90);
    case NHV_TERRAIN_LIQUID:
        return gfx::rgb(60, 80, 220);
    default:
        return 0;
    }
}

// Something stands on the cell besides its ground: a monster, an object, ...
bool has_thing(const Cell &c) { return c.tile >= 0 && c.tile != c.terrain_tile; }

} // namespace

void FirstPerson::build(const Model &m, TileSet &tiles, const raycast::Camera &camera) {
    const size_t cells = static_cast<size_t>(kCols) * kRows;
    walls_.assign(cells, nullptr);
    floors_.assign(cells, nullptr);
    ceilings_.assign(cells, nullptr);
    sprites_.clear();

    // A floor for the features (stairs, altars) to stand on: a room's, as
    // the level shows one
    int floor_tile = -1;
    for (const Cell &c : m.map) {
        if (c.terrain == NHV_TERRAIN_FLOOR && c.terrain_tile >= 0) {
            floor_tile = c.terrain_tile;
            break;
        }
    }

    for (int y = 0; y < kRows; y++) {
        for (int x = 0; x < kCols; x++) {
            const size_t i = static_cast<size_t>(y) * kCols + x;
            const Cell &c = m.cell(x, y);
            const bool dim = (c.flags & NHV_CELL_REMEMBERED) != 0;
            const auto look = dim ? TileSet::Look::OpaqueDim : TileSet::Look::Opaque;
            switch (x == 0 ? NHV_TERRAIN_UNKNOWN : c.terrain) {
            case NHV_TERRAIN_WALL:
            case NHV_TERRAIN_DOOR: {
                const raycast::Texture *t = tiles.texture(c.terrain_tile, look);
                walls_[i] = t ? t : &rock().texture;
                break;
            }
            case NHV_TERRAIN_FLOOR:
            case NHV_TERRAIN_CORRIDOR:
            case NHV_TERRAIN_LIQUID:
                floors_[i] = tiles.texture(c.terrain_tile, look);
                ceilings_[i] = &ceiling().texture;
                break;
            case NHV_TERRAIN_FEATURE:
                floors_[i] = tiles.texture(floor_tile, look);
                ceilings_[i] = &ceiling().texture;
                break;
            default:
                walls_[i] = &rock().texture;
                break;
            }
        }
    }

    // What stands on the cells near the hero: monsters and objects at eye
    // level, features low on the floor
    struct Candidate {
        int x, y;
        float distance;
    };
    std::vector<Candidate> near;
    const int cx = static_cast<int>(std::floor(camera.x));
    const int cy = static_cast<int>(std::floor(camera.y));
    for (int y = std::max(0, cy - kSpriteRange); y <= std::min(kRows - 1, cy + kSpriteRange); y++) {
        for (int x = std::max(1, cx - kSpriteRange); x <= std::min(kCols - 1, cx + kSpriteRange);
             x++) {
            const Cell &c = m.cell(x, y);
            if (x == m.hero_x && y == m.hero_y) {
                continue;
            }
            if (has_thing(c) || (c.terrain == NHV_TERRAIN_FEATURE && c.terrain_tile >= 0)) {
                const float dx = x + 0.5f - camera.x, dy = y + 0.5f - camera.y;
                near.push_back({x, y, dx * dx + dy * dy});
            }
        }
    }
    std::sort(near.begin(), near.end(),
              [](const Candidate &a, const Candidate &b) { return a.distance < b.distance; });
    if (near.size() > kMaxSprites) {
        near.resize(kMaxSprites);
    }
    for (const Candidate &n : near) {
        const Cell &c = m.cell(n.x, n.y);
        const bool dim = (c.flags & NHV_CELL_REMEMBERED) != 0;
        const auto look = dim ? TileSet::Look::SpriteDim : TileSet::Look::Sprite;
        const bool thing = has_thing(c);
        const raycast::Texture *t = tiles.texture(thing ? c.tile : c.terrain_tile, look);
        if (!t) {
            continue;
        }
        raycast::Sprite sprite;
        sprite.x = n.x + 0.5f;
        sprite.y = n.y + 0.5f;
        sprite.z = thing ? 0.36f : 0.2f;
        sprite.size = thing ? 0.7f : 0.45f;
        sprite.texture = t;
        sprites_.push_back(sprite);
    }
}

void FirstPerson::draw_minimap(gfx::Surface &s, int top, int height, const Model &m, int facing) {
    constexpr int kDot = 2;
    const int w = (kCols - 1) * kDot, h = kRows * kDot;
    if (h + 4 > height || w + 4 > s.w) {
        return;
    }
    const int left = s.w - w - 2, y0 = top + 2;
    s.darken({left - 1, y0 - 1, w + 2, h + 2}, 8);
    for (int y = 0; y < kRows; y++) {
        for (int x = 1; x < kCols; x++) {
            const Cell &c = m.cell(x, y);
            gfx::Pixel color = has_thing(c) ? kColors[c.color & 15] : terrain_color(c.terrain);
            if (!color) {
                continue;
            }
            s.fill({left + (x - 1) * kDot, y0 + y * kDot, kDot, kDot}, color);
        }
    }
    // The hero, and which way it faces
    const int hx = left + (m.hero_x - 1) * kDot, hy = y0 + m.hero_y * kDot;
    s.fill({hx - 1, hy - 1, kDot + 2, kDot + 2}, gfx::rgb(255, 255, 70));
    static constexpr int kDx[4] = {0, 1, 0, -1}, kDy[4] = {-1, 0, 1, 0};
    s.fill({hx + kDx[facing & 3] * 3, hy + kDy[facing & 3] * 3, kDot, kDot},
           gfx::rgb(255, 140, 30));
}

void FirstPerson::draw(gfx::Surface &s, int top, int height, const Model &m, TileSet &tiles,
                       const raycast::Camera &camera, int facing) {
    if (!raycaster_ || raycaster_->width() != s.w || raycaster_->height() != height) {
        raycaster_ = std::make_unique<raycast::Raycaster>(s.w, height);
        raycaster_->set_wall_top(gfx::rgb(60, 60, 70));
    }
    build(m, tiles, camera);
    const Map map{walls_.data(), floors_.data(), ceilings_.data()};
    if (s.stride == s.w) {
        raycaster_->render(s.row(top), map, camera, 1.0f, sprites_.data(),
                           static_cast<int>(sprites_.size()));
    }
    draw_minimap(s, top, height, m, facing);
}

} // namespace nethack

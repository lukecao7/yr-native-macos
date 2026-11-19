// SPDX-License-Identifier: GPL-3.0-only
#include "rts_core/iso.h"
#include "rts_core/worker_pool.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

namespace coastal::rts {
namespace {

constexpr int half_tile_width = 18;
constexpr int half_tile_height = 9;
constexpr int elevation_step = 3;
constexpr int maximum_elevation = 2;
constexpr double minimum_zoom = 0.55;
constexpr double maximum_zoom = 2.0;
constexpr double maximum_pan = 100'000;

struct Offset {
    double x;
    double y;
};

bool valid_map(const Map& map) noexcept {
    return map.width > 0 && map.width <= Map::max_dimension &&
           map.height > 0 && map.height <= Map::max_dimension &&
           map.tiles.size() == std::size_t(map.width) * map.height;
}

bool valid_camera(const Camera& camera) noexcept {
    return std::isfinite(camera.x) && std::abs(camera.x) <= maximum_pan &&
           std::isfinite(camera.y) && std::abs(camera.y) <= maximum_pan &&
           camera.zoom >= minimum_zoom && camera.zoom <= maximum_zoom;
}

Offset map_offset(const Map& map, Cell cell) noexcept {
    return {(cell.x - cell.y) * double(half_tile_width),
            (cell.x + cell.y) * double(half_tile_height) - elevation_step * map.at(cell).height};
}

struct Projection {
    double x;
    double y;
    double zoom;
    int half_width;
    int half_height;

    Projection(const Camera& camera, int width, int height)
        : x(width / 2.0 + camera.x), y(height / 2.0 + camera.y), zoom(camera.zoom),
          half_width(int(std::lround(half_tile_width * zoom))),
          half_height(int(std::lround(half_tile_height * zoom))) {}

    PixelPoint project(const Map& map, Cell cell) const noexcept {
        const auto offset = map_offset(map, cell);
        return {int(std::lround(x + offset.x * zoom)), int(std::lround(y + offset.y * zoom))};
    }

    Offset unproject(double screen_x, double screen_y, int elevation = 0) const noexcept {
        const double difference = (screen_x - x) / (half_tile_width * zoom);
        const double sum = ((screen_y - y) / zoom + elevation * elevation_step) / half_tile_height;
        return {(sum + difference) / 2, (sum - difference) / 2};
    }
};

struct Rect {
    int left;
    int top;
    int right;
    int bottom;
};

bool visible(Rect view, PixelPoint point, int radius) noexcept {
    return point.x >= view.left - radius && point.y >= view.top - radius &&
           point.x < view.right + radius && point.y < view.bottom + radius;
}

using Color = std::array<std::uint8_t, 4>;

constexpr Color color(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return {r, g, b, 255};
}

constexpr Color background = color(20, 33, 43);
constexpr Color shadow = color(27, 37, 35);
constexpr Color outline = color(56, 70, 60);
constexpr Color highlight = color(224, 244, 237);
constexpr Color resource_gold = color(235, 180, 89);

Color faction_color(Side side) {
    return side == Side::Blue ? color(96, 201, 240) : color(236, 146, 103);
}

class Canvas {
public:
    Canvas(std::span<std::uint8_t> pixels, int width, Rect clip)
        : pixels_(pixels), width_(width), clip_(clip) {}

    void rect(Rect box, Color fill) {
        const int left = std::max(box.left, clip_.left);
        const int right = std::min(box.right, clip_.right);
        if (left >= right) return;

        const int bottom = std::min(box.bottom, clip_.bottom);
        for (int y = std::max(box.top, clip_.top); y < bottom; ++y) {
            auto* out = pixels_.data() + (std::size_t(y) * width_ + left) * fill.size();
            for (int x = left; x < right; ++x, out += fill.size()) {
                std::memcpy(out, fill.data(), fill.size());
            }
        }
    }

    void box(Rect box, Color ink) {
        rect({box.left, box.top, box.right, box.top + 1}, ink);
        rect({box.left, box.bottom - 1, box.right, box.bottom}, ink);
        rect({box.left, box.top, box.left + 1, box.bottom}, ink);
        rect({box.right - 1, box.top, box.right, box.bottom}, ink);
    }

    void line(PixelPoint from, PixelPoint to, Color ink) {
        if (std::max(from.x, to.x) < clip_.left || std::min(from.x, to.x) >= clip_.right ||
            std::max(from.y, to.y) < clip_.top || std::min(from.y, to.y) >= clip_.bottom) return;
        const int dx = std::abs(to.x - from.x);
        const int dy = -std::abs(to.y - from.y);
        const int sx = from.x < to.x ? 1 : -1;
        const int sy = from.y < to.y ? 1 : -1;
        int error = dx + dy;
        for (;;) {
            rect({from.x, from.y, from.x + 1, from.y + 1}, ink);
            if (from.x == to.x && from.y == to.y) break;
            const int twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                from.x += sx;
            }
            if (twice <= dx) {
                error += dx;
                from.y += sy;
            }
        }
    }

    void diamond(PixelPoint center, int half_width, int half_height, Color fill) {
        const int top = std::max(-half_height, clip_.top - center.y);
        const int bottom = std::min(half_height, clip_.bottom - center.y - 1);
        for (int y = top; y <= bottom; ++y) {
            const int radius = half_width * (half_height - std::abs(y)) / half_height;
            rect({center.x - radius, center.y + y, center.x + radius + 1, center.y + y + 1}, fill);
        }
    }

    void rhombus_outline(PixelPoint center, int hw, int hh, Color stroke) {
        const PixelPoint top{center.x, center.y - hh};
        const PixelPoint bottom{center.x, center.y + hh};
        const PixelPoint left{center.x - hw, center.y};
        const PixelPoint right{center.x + hw, center.y};
        line(left, top, stroke);
        line(top, right, stroke);
        line(right, bottom, stroke);
        line(bottom, left, stroke);
    }

    void block(PixelPoint foot, int half_width, int half_height, int rise,
               Color roof, Color left_wall, Color right_wall) {
        // A vertical extrusion of a screen-space diamond. Each wall is a set
        // of clipped columns below the front two edges of the top face.
        const PixelPoint top{foot.x, foot.y - rise};
        const int left = std::max(-half_width, clip_.left - top.x);
        const int right = std::min(half_width, clip_.right - top.x - 1);
        for (int x = left; x <= right; ++x) {
            const int edge = top.y + half_height * (half_width - std::abs(x)) / half_width;
            rect({top.x + x, edge, top.x + x + 1, edge + rise + 1},
                 x < 0 ? left_wall : right_wall);
        }
        diamond(top, half_width, half_height, roof);
    }

private:
    std::span<std::uint8_t> pixels_;
    int width_;
    Rect clip_;
};

Color terrain_color(Tile tile, bool alternate = false) {
    switch (tile.terrain) {
        case Terrain::Ground:
            if (tile.height) return alternate ? color(112, 131, 96) : color(121, 139, 103);
            return alternate ? color(89, 110, 83) : color(98, 119, 89);
        case Terrain::Ramp:
            return color(153, 145, 103);
        case Terrain::Water:
            return alternate ? color(42, 94, 103) : color(46, 103, 112);
        case Terrain::Rock:
            return color(104, 108, 103);
        case Terrain::Deposit:
            return tile.resource ? color(141, 119, 75) : color(116, 106, 82);
    }
    return background;
}

int scaled_px(double value, const Projection& projection) {
    return std::max(1, int(std::lround(value * projection.zoom)));
}

void draw_terrain(Canvas& canvas, const Map& map, const Projection& projection, Cell cell,
                  PixelPoint point) {
    const int hw = projection.half_width;
    const int hh = projection.half_height;

    const Tile& tile = map.at(cell);
    const Color top = terrain_color(tile, ((cell.x ^ cell.y) & 1) != 0);
    const auto size = [&](double value) { return scaled_px(value, projection); };
    if (tile.height) {
        const int rise = size(elevation_step * tile.height);
        canvas.block({point.x, point.y + rise}, hw, hh, rise,
                     top, color(75, 91, 68), color(64, 78, 60));
    } else {
        canvas.diamond(point, hw, hh, top);
    }
    const Color edge = tile.terrain == Terrain::Water ? color(35, 78, 86) : outline;
    canvas.line({point.x - hw, point.y}, {point.x, point.y + hh}, edge);
    canvas.line({point.x, point.y + hh}, {point.x + hw, point.y}, edge);

    switch (tile.terrain) {
        case Terrain::Ramp:
            canvas.line({point.x - size(7), point.y}, {point.x, point.y - size(4)}, highlight);
            canvas.line({point.x, point.y - size(4)}, {point.x + size(7), point.y}, highlight);
            break;
        case Terrain::Water:
            if (((cell.x * 7 + cell.y * 11) & 3) == 0) {
                canvas.rect({point.x - size(6), point.y - 1, point.x + size(6), point.y},
                            color(90, 147, 147));
            }
            break;
        case Terrain::Rock:
            canvas.block(point, size(9), size(5), size(8), color(143, 148, 137),
                         color(94, 103, 99), color(79, 91, 89));
            break;
        case Terrain::Deposit:
            if (tile.resource) {
                canvas.block({point.x - size(4), point.y + size(1)}, size(3), size(2), size(5),
                             resource_gold, color(167, 118, 57), color(126, 92, 51));
                canvas.block({point.x + size(4), point.y + size(2)}, size(3), size(2), size(7),
                             color(252, 213, 125), color(186, 134, 76), color(145, 101, 56));
            }
            break;
        case Terrain::Ground:
            break;
    }
}

Rect visible_cells(const Map& map, const Projection& projection, int width, int height) {
    // Expand for the furthest elevated cornerstone before undoing the transform.
    constexpr int margin = 56;
    const auto top_left = projection.unproject(-margin, -margin);
    const auto bottom_right = projection.unproject(width + margin, height + margin, maximum_elevation);
    const auto top_right = projection.unproject(width + margin, -margin);
    const auto bottom_left = projection.unproject(-margin, height + margin, maximum_elevation);
    return {
        std::clamp(int(std::floor(top_left.x)), 0, map.width),
        std::clamp(int(std::floor(top_right.y)), 0, map.height),
        std::clamp(int(std::ceil(bottom_right.x)) + 1, 0, map.width),
        std::clamp(int(std::ceil(bottom_left.y)) + 1, 0, map.height),
    };
}

void draw_entity(Canvas& canvas, const World& world, const Projection& projection,
                 const Entity& entity, PixelPoint point, bool selected) {
    const auto size = [&](double value) { return scaled_px(value, projection); };
    const auto accent = faction_color(entity.side);
    const bool building = is_building(entity.kind);
    const int extent = building ? (entity.kind == UnitKind::Hub ? 14 : 12) :
                       (entity.kind == UnitKind::Sentinel ? 10 : 8);
    const int rise = building ? (entity.kind == UnitKind::Generator ? 15 : 10) : 4;
    const int hw = size(extent);
    const int hh = size(extent * .55);
    canvas.diamond({point.x + size(2), point.y + size(2)}, hw + 1, hh + 1, shadow);
    if (selected) canvas.rhombus_outline(point, hw + size(4), hh + size(4), highlight);
    canvas.block(point, hw, hh, size(rise), color(83, 108, 114), color(58, 77, 83), color(42, 59, 67));

    const PixelPoint roof{point.x, point.y - size(rise)};
    if (building) {
        switch (entity.kind) {
            case UnitKind::Hub:
                canvas.block(roof, size(8), size(4), size(5), accent,
                             color(72, 116, 128), color(63, 94, 109));
                canvas.rect({roof.x - size(1), roof.y - size(9), roof.x + size(1) + 1,
                             roof.y - size(2)}, highlight);
                break;
            case UnitKind::Generator:
                canvas.diamond(roof, size(7), size(4), accent);
                canvas.diamond(roof, size(2), size(2), highlight);
                break;
            case UnitKind::Depot:
                canvas.rect({roof.x - size(6), roof.y - size(2), roof.x + size(7),
                             roof.y + size(3)}, accent);
                break;
            case UnitKind::Workshop:
                canvas.block({roof.x - size(3), roof.y - size(2)}, size(7), size(3), size(3),
                             accent, color(72, 116, 128), color(63, 94, 109));
                break;
            default:
                break;
        }
    } else {
        canvas.diamond(roof, size(entity.kind == UnitKind::Artillery ? 6 : 4), size(3), accent);
        if (entity.kind == UnitKind::Collector && entity.cargo != 0) {
            canvas.rect({roof.x - size(2), roof.y - size(3), roof.x + size(2) + 1, roof.y + 1},
                        resource_gold);
        }
        const int facing_x = std::clamp(entity.facing.x, -1, 1);
        const int facing_y = std::clamp(entity.facing.y, -1, 1);
        canvas.line(roof, {roof.x + size(3) * (facing_x - facing_y),
                           roof.y + size(2) * (facing_x + facing_y)}, highlight);
    }

    const std::uint16_t full_hp = world.rules.spec(entity.kind).hp;
    if (full_hp == 0) return;
    const int width = size(building ? 26 : 15);
    const int height = size(2);
    const int left = point.x - width / 2;
    const int top = point.y - size(rise + (building ? 14 : 8));
    const int filled = int(std::uint32_t(width) * std::min(entity.hp, full_hp) / full_hp);
    canvas.rect({left - 1, top - 1, left + width + 1, top + height + 1}, background);
    canvas.rect({left, top, left + filled, top + height},
                entity.hp * 3 <= full_hp ? color(241, 126, 99) : color(161, 224, 137));
}

std::optional<PixelPoint> projectile_center(const Map& map, const Projection& projection,
                                          const Projectile& projectile) {
    if (projectile.travel_ticks == 0 || !map.contains(projectile.from) || !map.contains(projectile.to)) {
        return std::nullopt;
    }
    const auto from = projection.project(map, projectile.from);
    const auto to = projection.project(map, projectile.to);
    const double progress = 1 - std::min(1.0, double(projectile.remaining) / projectile.travel_ticks);
    const auto blend = [&](int begin, int end) { return int(std::lround(std::lerp(begin, end, progress))); };
    const int arc = int(std::lround((5 + 9 * progress * (1 - progress)) * projection.zoom));
    return PixelPoint{blend(from.x, to.x), blend(from.y, to.y) - arc};
}

struct TerrainSprite {
    Cell cell;
    PixelPoint center;
};

struct EntitySprite {
    const Entity* entity;
    PixelPoint foot;
    bool selected;
};

struct RasterRegion {
    Rect clip{};
    std::vector<TerrainSprite> terrain;
    std::vector<EntitySprite> entities;
    std::vector<PixelPoint> projectiles;
    bool preview = false;
};

void draw_minimap(Canvas& canvas, const World& world, const Projection& projection,
                  int width, int height) {
    const int extent = std::min({164, width / 3, height / 3});
    if (extent < 36) return;

    const int largest_dimension = std::max(world.map.width, world.map.height);
    const int map_width = std::max(1, extent * world.map.width / largest_dimension);
    const int map_height = std::max(1, extent * world.map.height / largest_dimension);
    const int right = width - 12;
    const int bottom = height - 12;
    const int left = right - map_width;
    const int top = bottom - map_height;
    canvas.rect({left - 4, top - 4, right + 4, bottom + 4}, background);
    canvas.box({left - 3, top - 3, right + 3, bottom + 3}, color(104, 135, 143));

    for (int y = 0; y < map_height; ++y) {
        const int row = y * world.map.height / map_height;
        for (int x = 0; x < map_width; ++x) {
            const auto tile = world.map.at({x * world.map.width / map_width, row});
            const int px = left + x;
            const int py = top + y;
            canvas.rect({px, py, px + 1, py + 1}, terrain_color(tile));
        }
    }
    for (const auto& entity : world.entities) {
        if (entity.hp == 0 || !world.map.contains(entity.position)) continue;
        const int x = left + entity.position.x * map_width / world.map.width;
        const int y = top + entity.position.y * map_height / world.map.height;
        const int size = is_building(entity.kind) ? 3 : 2;
        canvas.rect({x, y, std::min(x + size, right), std::min(y + size, bottom)}, faction_color(entity.side));
    }
    const auto center = projection.unproject(width / 2.0, height / 2.0);
    if (center.x >= 0 && center.x < world.map.width && center.y >= 0 && center.y < world.map.height) {
        const int x = left + int(center.x * map_width / world.map.width);
        const int y = top + int(center.y * map_height / world.map.height);
        canvas.box({std::max(left, x - 3), std::max(top, y - 3), std::min(right, x + 4),
                    std::min(bottom, y + 4)}, highlight);
    }
}

void draw_drag(Canvas& canvas, const SelectionBox& drag, int width, int height) {
    Rect box{std::min(drag.start.x, drag.end.x), std::min(drag.start.y, drag.end.y),
             std::max(drag.start.x, drag.end.x), std::max(drag.start.y, drag.end.y)};
    box.left = std::clamp(box.left, 0, width);
    box.right = std::clamp(box.right, 0, width);
    box.top = std::clamp(box.top, 0, height);
    box.bottom = std::clamp(box.bottom, 0, height);
    if (box.left < box.right && box.top < box.bottom) canvas.box(box, highlight);
}

}  // namespace

std::optional<Map> load_map(const std::filesystem::path& path, std::string& error) {
    error.clear();
    std::error_code file_error;
    const auto file_length = std::filesystem::file_size(path, file_error);
    constexpr auto maximum_bytes = Map::max_dimension * (Map::max_dimension + 2) + 128;
    if (file_error || file_length > maximum_bytes) {
        error = "Unable to read a map of the supported size.";
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary);
    Map map;
    std::string magic;
    int version = 0;
    int deposit_amount = 0;
    if (!(input >> magic >> version >> map.width >> map.height >> deposit_amount) ||
        magic != "COASTAL_MAP" || version != 1 ||
        map.width < 1 || map.height < 1 || map.width > Map::max_dimension ||
        map.height > Map::max_dimension || deposit_amount < 0 ||
        deposit_amount > std::numeric_limits<std::uint16_t>::max()) {
        error = "Invalid COASTAL_MAP header.";
        return std::nullopt;
    }

    std::string row;
    if (!std::getline(input, row) || row.find_first_not_of(" \t\r") != std::string::npos) {
        error = "Map rows must start after the header line.";
        return std::nullopt;
    }
    map.tiles.reserve(std::size_t(map.width) * map.height);
    for (int y = 0; y < map.height; ++y) {
        if (!std::getline(input, row)) {
            error = "Missing map row " + std::to_string(y + 1) + ".";
            return std::nullopt;
        }
        if (!row.empty() && row.back() == '\r') row.pop_back();
        if (row.size() != std::size_t(map.width)) {
            error = "Wrong width in map row " + std::to_string(y + 1) + ".";
            return std::nullopt;
        }
        for (int x = 0; x < map.width; ++x) {
            Tile tile;
            switch (row[x]) {
                case '.': break;
                case '/': tile = {Terrain::Ramp, 1, 0}; break;
                case '=': tile.height = 2; break;
                case '~': tile.terrain = Terrain::Water; break;
                case '#': tile.terrain = Terrain::Rock; break;
                case 'o': tile = {Terrain::Deposit, 0, static_cast<std::uint16_t>(deposit_amount)}; break;
                case 'a':
                case 'b': {
                    auto& start = map.starts[row[x] == 'a' ? 0 : 1];
                    if (map.contains(start)) {
                        error = "Duplicate start for a side.";
                        return std::nullopt;
                    }
                    start = {x, y};
                    break;
                }
                default:
                    error = "Unknown terrain in map row " + std::to_string(y + 1) + ".";
                    return std::nullopt;
            }
            map.tiles.push_back(tile);
        }
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad() ||
        !map.contains(map.starts[0]) || !map.contains(map.starts[1])) {
        error = "Expected exactly the declared rows and one start per side.";
        return std::nullopt;
    }
    return map;
}

void Camera::center_on(Cell cell, const Map& map) noexcept {
    if (!valid_map(map) || !map.contains(cell)) return;
    if (!std::isfinite(zoom)) zoom = 1;
    zoom = std::clamp(zoom, minimum_zoom, maximum_zoom);
    const auto offset = map_offset(map, cell);
    x = target_x = -offset.x * zoom;
    y = target_y = -offset.y * zoom;
    target_zoom = zoom;
}

void Camera::pan(double dx, double dy) noexcept {
    if (std::isfinite(dx) && std::isfinite(dy)) {
        target_x = std::clamp(target_x + dx, -maximum_pan, maximum_pan);
        target_y = std::clamp(target_y + dy, -maximum_pan, maximum_pan);
    }
}

void Camera::zoom_by(double scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0 || target_zoom <= 0) return;
    const double next = std::clamp(target_zoom * scale, minimum_zoom, maximum_zoom);
    const double ratio = next / target_zoom;
    target_x = std::clamp(target_x * ratio, -maximum_pan, maximum_pan);
    target_y = std::clamp(target_y * ratio, -maximum_pan, maximum_pan);
    target_zoom = next;
}

void Camera::advance() noexcept {
    const auto ease = [](double current, double target) {
        return std::abs(target - current) < 0.005 ? target : std::lerp(current, target, 0.2);
    };
    x = ease(x, target_x);
    y = ease(y, target_y);
    zoom = ease(zoom, target_zoom);
}

PixelPoint project_cell(const Map& map, const Camera& camera, Cell cell, int width, int height) noexcept {
    if (!valid_map(map) || !map.contains(cell) || !valid_camera(camera) || width <= 0 || height <= 0) return {};
    return Projection(camera, width, height).project(map, cell);
}

std::optional<Cell> pick_cell(const Map& map, const Camera& camera, PixelPoint point,
                              int width, int height) noexcept {
    if (!valid_map(map) || !valid_camera(camera) ||
        point.x < 0 || point.y < 0 || point.x >= width || point.y >= height) return std::nullopt;

    const Projection projection(camera, width, height);
    // A lifted diamond can overlap a tile behind it. Test the same footprints
    // used to draw the tile tops and resolve shared edges in painter order.
    const auto flat = projection.unproject(point.x, point.y);
    const auto lifted = projection.unproject(point.x, point.y, maximum_elevation);
    const auto nearest_x = int(std::floor(flat.x));
    const auto nearest_y = int(std::floor(flat.y));
    std::optional<Cell> front;
    for (int y = nearest_y - 1; y <= int(std::ceil(lifted.y)) + 1; ++y) {
        for (int x = nearest_x - 1; x <= int(std::ceil(lifted.x)) + 1; ++x) {
            const Cell candidate{x, y};
            if (!map.contains(candidate)) continue;
            const auto center = projection.project(map, candidate);
            const int dy = std::abs(point.y - center.y);
            if (dy > projection.half_height ||
                std::abs(point.x - center.x) > projection.half_width *
                                               (projection.half_height - dy) / projection.half_height) continue;
            if (!front || x + y > front->x + front->y ||
                (x + y == front->x + front->y && x > front->x)) front = candidate;
        }
    }
    return front;
}

bool render_isometric(const World& world, const Camera& camera, const ViewOverlay& overlay,
                      std::span<std::uint8_t> rgba, int width, int height, WorkerPool* workers) {
    constexpr int maximum_viewport = 16'384;
    if (!valid_map(world.map) || !valid_camera(camera) || width < 1 || height < 1 ||
        width > maximum_viewport || height > maximum_viewport ||
        std::size_t(width) * height * 4 != rgba.size() || side_index(overlay.side) >= world.sides.size()) {
        return false;
    }

    const Projection projection(camera, width, height);
    const Rect viewport{0, 0, width, height};
    const int chunk = workers && workers->count() > 1 ? 64 : std::max(width, height);
    const int columns = (width + chunk - 1) / chunk;
    const int rows = (height + chunk - 1) / chunk;
    std::vector<RasterRegion> regions(std::size_t(columns) * rows);
    for (int row = 0; row < rows; ++row) {
        for (int col = 0; col < columns; ++col) {
            regions[row * columns + col].clip = {col * chunk, row * chunk,
                                                std::min((col + 1) * chunk, width),
                                                std::min((row + 1) * chunk, height)};
        }
    }
    const auto add_to_regions = [&](Rect bounds, const auto& add) {
        if (regions.size() == 1) {
            add(regions.front());
            return;
        }
        const int left = std::max(0, bounds.left);
        const int right = std::min(width, bounds.right);
        const int top = std::max(0, bounds.top);
        const int bottom = std::min(height, bounds.bottom);
        if (left >= right || top >= bottom) return;
        for (int row = top / chunk; row <= (bottom - 1) / chunk; ++row) {
            for (int col = left / chunk; col <= (right - 1) / chunk; ++col) {
                add(regions[row * columns + col]);
            }
        }
    };

    // Project the visible scene once. Each region receives its own subset in
    // painter order, including any part of a sprite that crosses a tile seam.
    const int hw = projection.half_width;
    const int hh = projection.half_height;
    const int terrain_detail = scaled_px(13, projection) + 1;
    const Rect cells = visible_cells(world.map, projection, width, height);
    for (int sum = cells.left + cells.top; sum < cells.right + cells.bottom - 1; ++sum) {
        const int left = std::max(cells.left, sum - cells.bottom + 1);
        const int right = std::min(cells.right - 1, sum - cells.top);
        for (int x = left; x <= right; ++x) {
            const Cell cell{x, sum - x};
            const auto center = projection.project(world.map, cell);
            if (!visible(viewport, center, hw + 8)) continue;
            const auto& tile = world.map.at(cell);
            const int rise = tile.height ? scaled_px(elevation_step * tile.height, projection) : 0;
            const int top = (tile.terrain == Terrain::Rock || tile.terrain == Terrain::Deposit) ?
                std::max(hh, terrain_detail) : hh;
            add_to_regions({center.x - hw, center.y - top, center.x + hw + 1, center.y + hh + rise + 1},
                           [&](RasterRegion& region) { region.terrain.push_back({cell, center}); });
        }
    }
    PixelPoint preview{};
    const auto tint = overlay.can_build ? color(129, 232, 162) : color(247, 121, 106);
    if (overlay.build_cell && world.map.contains(*overlay.build_cell)) {
        preview = projection.project(world.map, *overlay.build_cell);
        add_to_regions({preview.x - hw, preview.y - hh, preview.x + hw + 1, preview.y + hh + 1},
                       [](RasterRegion& region) { region.preview = true; });
    }

    std::vector<EntitySprite> sprites;
    sprites.reserve(world.entities.size());
    const auto& selected = world.side(overlay.side).selected;
    for (const auto& entity : world.entities) {
        if (!entity.hp || !world.map.contains(entity.position) || entity.kind >= UnitKind::Count) continue;
        const auto foot = projection.project(world.map, entity.position);
        if (visible(viewport, foot, 72)) {
            sprites.push_back({&entity, foot, std::binary_search(selected.begin(), selected.end(), entity.id)});
        }
    }
    std::sort(sprites.begin(), sprites.end(), [](const EntitySprite& a, const EntitySprite& b) {
        if (a.foot.y != b.foot.y) return a.foot.y < b.foot.y;
        return a.entity->id < b.entity->id;
    });
    // Include outlines, roof detail, a dropped shadow, and the health bar.
    const int sprite_side = scaled_px(18, projection) + 3;
    const int sprite_top = scaled_px(30, projection) + 2;
    const int sprite_bottom = scaled_px(12, projection) + 2;
    for (const auto& sprite : sprites) {
        const auto foot = sprite.foot;
        add_to_regions({foot.x - sprite_side, foot.y - sprite_top,
                        foot.x + sprite_side + 1, foot.y + sprite_bottom + 1},
                       [&](RasterRegion& region) { region.entities.push_back(sprite); });
    }
    for (const auto& projectile : world.projectiles) {
        const auto center = projectile_center(world.map, projection, projectile);
        if (center) {
            add_to_regions({center->x - 2, center->y - 2, center->x + 3, center->y + 3},
                           [&](RasterRegion& region) { region.projectiles.push_back(*center); });
        }
    }

    const auto paint = [&](unsigned, std::size_t begin, std::size_t end) {
        for (std::size_t index = begin; index < end; ++index) {
            const auto& region = regions[index];
            Canvas canvas(rgba, width, region.clip);
            canvas.rect(region.clip, background);
            for (const auto& tile : region.terrain) {
                draw_terrain(canvas, world.map, projection, tile.cell, tile.center);
            }
            if (region.preview) canvas.rhombus_outline(preview, hw - 1, hh - 1, tint);
            for (const auto& sprite : region.entities) {
                draw_entity(canvas, world, projection, *sprite.entity, sprite.foot, sprite.selected);
            }
            for (const auto center : region.projectiles) canvas.diamond(center, 2, 2, resource_gold);
        }
    };
    if (workers && regions.size() > 1) workers->run(regions.size(), paint);
    else paint(0, 0, regions.size());

    // Overlay the minimap and drag box after all pixel regions have joined.
    Canvas canvas(rgba, width, viewport);
    draw_minimap(canvas, world, projection, width, height);
    if (overlay.drag) draw_drag(canvas, *overlay.drag, width, height);
    return true;
}

}  // namespace coastal::rts

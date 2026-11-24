// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "world.h"

#include <filesystem>
#include <span>
#include <string>

namespace coastal::rts {

struct PixelPoint {
    int x = 0;
    int y = 0;
};

struct SelectionBox {
    PixelPoint start;
    PixelPoint end;
};

struct Camera {
    double x = 0; // Screen-space displacement of the map origin from the viewport center.
    double y = 0;
    double zoom = 1;
    double target_x = 0;
    double target_y = 0;
    double target_zoom = 1;

    void center_on(Cell cell, const Map& map) noexcept;
    void pan(double dx, double dy) noexcept;
    void zoom_by(double scale) noexcept;
    void advance() noexcept;
};

std::optional<Map> load_map(const std::filesystem::path& path, std::string& error);
PixelPoint project_cell(const Map& map, const Camera& camera, Cell cell, int width, int height) noexcept;
std::optional<Cell> pick_cell(const Map& map, const Camera& camera, PixelPoint point,
                              int width, int height) noexcept;

// Returns the topmost visible body drawn by the isometric renderer at this pixel.
EntityId pick_entity(const World& world, const Camera& camera, PixelPoint point,
                     int width, int height) noexcept;

struct ViewOverlay {
    Side side = Side::Blue;
    std::optional<SelectionBox> drag;
    std::optional<Cell> build_cell;
    bool can_build = false;
};

class WorkerPool;

bool render_isometric(const World& world, const Camera& camera, const ViewOverlay& overlay,
                      std::span<std::uint8_t> rgba, int width, int height, WorkerPool* workers = nullptr);

}  // namespace coastal::rts

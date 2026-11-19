// SPDX-License-Identifier: GPL-3.0-only
#include "rts_3d/scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace coastal::rts::view3d {
namespace {

using Matrix = std::array<double, 16>;
using Homogeneous = std::array<double, 4>;

constexpr double turn = 2 * std::numbers::pi;
constexpr double min_pitch = 0.18;
constexpr double max_pitch = 1.45;
constexpr double min_distance = 6;
constexpr double max_distance = 600;
constexpr double near_clip = 0.25;
constexpr double far_clip = 2048;
constexpr double field_of_view = 52 * std::numbers::pi / 180;
constexpr double epsilon = 1e-10;

Vec3 operator+(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x + rhs.x, lhs.y + rhs.y, lhs.z + rhs.z};
}

Vec3 operator-(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.x - rhs.x, lhs.y - rhs.y, lhs.z - rhs.z};
}

Vec3 operator*(Vec3 vector, double scale) noexcept {
    return {vector.x * scale, vector.y * scale, vector.z * scale};
}

double dot(Vec3 lhs, Vec3 rhs) noexcept {
    return lhs.x * rhs.x + lhs.y * rhs.y + lhs.z * rhs.z;
}

Vec3 cross(Vec3 lhs, Vec3 rhs) noexcept {
    return {lhs.y * rhs.z - lhs.z * rhs.y,
            lhs.z * rhs.x - lhs.x * rhs.z,
            lhs.x * rhs.y - lhs.y * rhs.x};
}

bool finite(Vec3 vector) noexcept {
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

bool valid_map(const Map& map) noexcept {
    return map.width > 0 && map.width <= Map::max_dimension &&
           map.height > 0 && map.height <= Map::max_dimension &&
           map.tiles.size() == std::size_t(map.width) * map.height;
}

template<class Scalar>
Homogeneous transform(const std::array<Scalar, 16>& matrix, Homogeneous vector) noexcept {
    Homogeneous result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            result[row] += matrix[column * 4 + row] * vector[column];
        }
    }
    return result;
}

Matrix compose(const Matrix& lhs, const Matrix& rhs) noexcept {
    Matrix result{};
    for (int column = 0; column < 4; ++column) {
        const auto start = rhs.begin() + column * 4;
        const auto output = transform(lhs, {start[0], start[1], start[2], start[3]});
        std::copy(output.begin(), output.end(), result.begin() + column * 4);
    }
    return result;
}

std::optional<Vec3> unproject(const CameraPose& camera, double x, double y, double z) noexcept {
    const auto point = transform(camera.inverse, {x, y, z, 1});
    if (!std::isfinite(point[3]) || std::abs(point[3]) < epsilon) return std::nullopt;
    const Vec3 position{point[0] / point[3], point[1] / point[3], point[2] / point[3]};
    return finite(position) ? std::optional(position) : std::nullopt;
}

struct Ray {
    Vec3 origin;
    Vec3 direction;
};

bool clip_axis(double origin, double direction, int extent, double& entry, double& exit) noexcept {
    if (std::abs(direction) <= epsilon) return origin >= 0 && origin <= extent;
    const double first = -origin / direction;
    const double last = (extent - origin) / direction;
    entry = std::max(entry, std::min(first, last));
    exit = std::min(exit, std::max(first, last));
    return entry <= exit;
}

struct Axis {
    int cell;
    int step;
    double next;
    double delta;
};

Axis start_axis(double origin, double direction, double entry, int extent) noexcept {
    const double at_entry = std::clamp(origin + direction * entry, 0.0, double(extent));
    const double leading_cell = std::floor(at_entry);
    const int step = (direction > epsilon) - (direction < -epsilon);
    const int cell = std::clamp(int(leading_cell) - (step < 0 && leading_cell == at_entry), 0, extent - 1);
    if (step == 0) {
        const auto infinity = std::numeric_limits<double>::infinity();
        return {cell, step, infinity, infinity};
    }
    const double boundary = cell + (step > 0);
    return {cell, step, std::max(entry, (boundary - origin) / direction), std::abs(1 / direction)};
}

bool hit_triangle(const Ray& ray, Vec3 first, Vec3 second, Vec3 third,
                  double entry, double exit) noexcept {
    const Vec3 edge1 = second - first;
    const Vec3 edge2 = third - first;
    const Vec3 p = cross(ray.direction, edge2);
    const double determinant = dot(edge1, p);
    if (std::abs(determinant) < epsilon) return false;
    const Vec3 relative = ray.origin - first;
    const double u = dot(relative, p) / determinant;
    if (u < -epsilon || u > 1 + epsilon) return false;
    const Vec3 q = cross(relative, edge1);
    const double v = dot(ray.direction, q) / determinant;
    if (v < -epsilon || u + v > 1 + epsilon) return false;
    const double length = dot(edge2, q) / determinant;
    return length >= entry - epsilon && length <= exit + epsilon;
}

bool hit_tile(const Map& map, Cell cell, const Ray& ray, double entry, double exit) noexcept {
    const auto h = tile_heights(map, cell);
    const double x = cell.x;
    const double z = cell.y;
    const std::array<Vec3, 4> vertices{{{x, h[0], z}, {x + 1, h[1], z},
                                        {x, h[2], z + 1}, {x + 1, h[3], z + 1}}};
    return hit_triangle(ray, vertices[0], vertices[2], vertices[1], entry, exit) ||
           hit_triangle(ray, vertices[1], vertices[2], vertices[3], entry, exit);
}

}  // namespace

void OrbitCamera::center_on(const Map& map, Cell cell) noexcept {
    if (valid_map(map) && map.contains(cell)) target = tile_anchor(map, cell);
}

void OrbitCamera::orbit(double dx, double dy) noexcept {
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;
    constexpr double sensitivity = 0.008;
    goal_yaw = std::remainder(goal_yaw - dx * sensitivity, turn);
    goal_pitch = std::clamp(goal_pitch + dy * sensitivity, min_pitch, max_pitch);
}

void OrbitCamera::zoom_by(double scale) noexcept {
    if (!std::isfinite(scale) || scale <= 0) return;
    goal_distance = std::clamp(goal_distance / scale, min_distance, max_distance);
}

void OrbitCamera::advance() noexcept {
    constexpr double easing = 0.22;
    constexpr double settle = 1e-5;
    const double angular_delta = std::remainder(goal_yaw - yaw, turn);
    yaw = std::abs(angular_delta) < settle ? goal_yaw : std::remainder(yaw + easing * angular_delta, turn);
    pitch = std::abs(goal_pitch - pitch) < settle ? goal_pitch : std::lerp(pitch, goal_pitch, easing);
    distance = std::abs(goal_distance - distance) < settle
        ? goal_distance : std::lerp(distance, goal_distance, easing);
}

CameraPose OrbitCamera::pose(int width, int height) const noexcept {
    if (width <= 0 || height <= 0 || !finite(target) ||
        !std::isfinite(yaw) || !std::isfinite(pitch) || !std::isfinite(distance)) return {};
    const double tilt = std::clamp(pitch, min_pitch, max_pitch);
    const double azimuth = std::remainder(yaw, turn);
    const double sin_yaw = std::sin(azimuth);
    const double cos_yaw = std::cos(azimuth);
    const double cos_tilt = std::cos(tilt);
    const Vec3 back{sin_yaw * cos_tilt, std::sin(tilt), cos_yaw * cos_tilt};
    const Vec3 right{cos_yaw, 0, -sin_yaw};
    const Vec3 up = cross(back, right);
    const Vec3 eye = target + back * std::clamp(distance, min_distance, max_distance);

    const Matrix view{right.x, up.x, back.x, 0,
                      right.y, up.y, back.y, 0,
                      right.z, up.z, back.z, 0,
                      -dot(right, eye), -dot(up, eye), -dot(back, eye), 1};
    const Matrix inverse_view{right.x, right.y, right.z, 0,
                              up.x, up.y, up.z, 0,
                              back.x, back.y, back.z, 0,
                              eye.x, eye.y, eye.z, 1};

    const double sy = 1 / std::tan(field_of_view / 2);
    const double sx = sy * height / width;
    const double depth = far_clip / (near_clip - far_clip);
    const double offset = depth * near_clip;
    const Matrix lens{sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, depth, -1, 0, 0, offset, 0};
    const Matrix inverse_lens{1 / sx, 0, 0, 0, 0, 1 / sy, 0, 0, 0, 0, 0, 1 / offset,
                              0, 0, -1, depth / offset};

    CameraPose result{};
    const auto matrix = compose(lens, view);
    std::transform(matrix.begin(), matrix.end(), result.projection.begin(),
                    [](double value) { return static_cast<float>(value); });
    result.inverse = compose(inverse_view, inverse_lens);
    result.right = {float(right.x), float(right.y), float(right.z), 0};
    result.up = {float(up.x), float(up.y), float(up.z), 0};
    result.width = width;
    result.height = height;
    return result;
}

std::optional<PixelPoint> project_point(const CameraPose& camera, Vec3 point) noexcept {
    if (camera.width <= 0 || camera.height <= 0 || !finite(point)) return std::nullopt;
    const auto clip = transform(camera.projection, {point.x, point.y, point.z, 1});
    if (!std::isfinite(clip[3]) || clip[3] <= 0) return std::nullopt;
    const auto x = clip[0] / clip[3];
    const auto y = clip[1] / clip[3];
    const auto z = clip[2] / clip[3];
    if (!finite({x, y, z}) || x < -1 || x >= 1 || y <= -1 || y > 1 || z < 0 || z > 1) {
        return std::nullopt;
    }
    return PixelPoint{std::min(camera.width - 1, int((x + 1) * 0.5 * camera.width)),
                      std::min(camera.height - 1, int((1 - y) * 0.5 * camera.height))};
}

std::optional<Cell> pick_terrain(const Map& map, const CameraPose& camera, PixelPoint pixel) noexcept {
    if (!valid_map(map) || pixel.x < 0 || pixel.y < 0 ||
        pixel.x >= camera.width || pixel.y >= camera.height) return std::nullopt;

    const double x = 2 * (pixel.x + 0.5) / camera.width - 1;
    const double y = 1 - 2 * (pixel.y + 0.5) / camera.height;
    const auto near_point = unproject(camera, x, y, 0);
    const auto far_point = unproject(camera, x, y, 1);
    if (!near_point || !far_point) return std::nullopt;

    const Vec3 line = *far_point - *near_point;
    const double length = std::sqrt(dot(line, line));
    if (!std::isfinite(length) || length < epsilon) return std::nullopt;
    const Ray ray{*near_point, line * (1 / length)};

    double entry = 0;
    double exit = length;
    if (!clip_axis(ray.origin.x, ray.direction.x, map.width, entry, exit) ||
        !clip_axis(ray.origin.z, ray.direction.z, map.height, entry, exit)) return std::nullopt;

    auto horizontal = start_axis(ray.origin.x, ray.direction.x, entry, map.width);
    auto vertical = start_axis(ray.origin.z, ray.direction.z, entry, map.height);
    // Intersect only the cells crossed by the projected ray, in near-to-far
    // order. A pick crosses at most width + height tile intervals.
    for (Cell cell{horizontal.cell, vertical.cell}; map.contains(cell);
         cell = {horizontal.cell, vertical.cell}) {
        const double crossed = std::min(horizontal.next, vertical.next);
        if (hit_tile(map, cell, ray, entry, std::min(crossed, exit))) return cell;
        if (crossed >= exit) break;
        if (horizontal.next == crossed) {
            horizontal.cell += horizontal.step;
            horizontal.next += horizontal.delta;
        }
        if (vertical.next == crossed) {
            vertical.cell += vertical.step;
            vertical.next += vertical.delta;
        }
        entry = crossed;
    }
    return std::nullopt;
}

}  // namespace coastal::rts::view3d

// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/mix_archive.h"
#include "macos_host/session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace coastal::macos_host {
namespace {

using Pixel = std::array<std::uint8_t, 4>;
constexpr int width = 1024;
constexpr int height = 768;
constexpr std::size_t frame_bytes = std::size_t(width) * height * sizeof(Pixel);
constexpr Pixel background{24, 37, 59, 255};
constexpr Pixel alternate_marker{255, 136, 94, 255};
constexpr int initial_marker_size = 64;
constexpr int edge_threshold = 16;
constexpr int edge_speed = 4;

struct Point {
    int x;
    int y;
};

struct Selection {
    Point first;
    Point last;
    Pixel border;
};

bool has_modifier(InputModifiers flags, Modifier modifier) {
    return (flags & static_cast<InputModifiers>(modifier)) != 0;
}

Pixel selection_color(InputModifiers modifiers) {
    if (has_modifier(modifiers, Modifier::Command)) return {245, 155, 255, 255};
    if (has_modifier(modifiers, Modifier::Control)) return {105, 242, 219, 255};
    if (has_modifier(modifiers, Modifier::Shift)) return {255, 218, 102, 255};
    return {218, 237, 255, 255};
}

void paint_rect(std::span<std::uint8_t> pixels, int left, int top, int right, int bottom,
                Pixel color) {
    for (int y = top; y < bottom; ++y) {
        auto* row = pixels.data() + (std::size_t(y) * width + left) * sizeof(Pixel);
        for (int x = left; x < right; ++x) {
            std::memcpy(row, color.data(), sizeof(Pixel));
            row += sizeof(Pixel);
        }
    }
}

void paint_outline(std::span<std::uint8_t> pixels, const Selection& selection) {
    const auto [left, right] = std::minmax(selection.first.x, selection.last.x);
    const auto [top, bottom] = std::minmax(selection.first.y, selection.last.y);
    constexpr int thickness = 2;
    paint_rect(pixels, left, top, right + 1, std::min(top + thickness, bottom + 1), selection.border);
    paint_rect(pixels, left, std::max(top, bottom + 1 - thickness), right + 1, bottom + 1,
               selection.border);
    paint_rect(pixels, left, top, std::min(left + thickness, right + 1), bottom + 1, selection.border);
    paint_rect(pixels, std::max(left, right + 1 - thickness), top, right + 1, bottom + 1,
               selection.border);
}

class FixtureSession final : public Session {
public:
    explicit FixtureSession(Pixel marker) : marker_(marker) {}

    Viewport viewport() const noexcept override { return {width, height}; }
    FrameClock::duration frame_interval() const noexcept override {
        return std::chrono::nanoseconds(16'666'667);
    }

    std::optional<Frame> advance_frame() override {
        auto& surface = surfaces_[index_ % surfaces_.size()];
        if (!surface || !surface.unique()) {
            surface = std::make_shared<std::vector<std::uint8_t>>(frame_bytes);
        }
        auto frame = render(*surface);
        frame.rgba_owner = surface;
        return frame;
    }

    bool supports_shared_frames() const noexcept override { return true; }

    std::optional<Frame> advance_frame_into(std::span<std::uint8_t> target) override {
        if (target.size() != frame_bytes) return std::nullopt;
        return render(target);
    }

    void pointer_event(PointerEvent event) noexcept override {
        using Phase = PointerEvent::Phase;
        if (event.phase == Phase::Leave) {
            hover_.reset();
            return;
        }
        if (!std::isfinite(event.x) || !std::isfinite(event.y)) return;
        const Point point{int(std::clamp(event.x, 0.0, double(width - 1))),
                          int(std::clamp(event.y, 0.0, double(height - 1)))};
        if (event.x >= 0 && event.x < width && event.y >= 0 && event.y < height) {
            hover_ = point;
        } else {
            hover_.reset();
        }
        if (event.button == 1 && event.phase == Phase::Up) {
            cancel_selection();
            pin_marker(point);
            return;
        }
        if (event.button != 0) return;
        switch (event.phase) {
        case Phase::Down:
            drag_origin_ = point;
            selection_.reset();
            pin_marker(point);
            break;
        case Phase::Drag:
            if (drag_origin_) {
                selection_ = Selection{*drag_origin_, point, selection_color(event.modifiers)};
                pin_marker(point);
            }
            break;
        case Phase::Up:
            if (selection_ && drag_origin_) {
                selection_->last = point;
                pin_marker(point);
            }
            drag_origin_.reset();
            break;
        case Phase::Move:
        case Phase::Enter:
        case Phase::Leave:
            break;
        }
    }

    void scroll_event(ScrollEvent event) noexcept override {
        if (!std::isfinite(event.scrolling_delta_x) || !std::isfinite(event.scrolling_delta_y)) return;
        if (has_modifier(event.modifiers, Modifier::Control)) {
            if (event.scrolling_delta_y == 0) return;
            const Point center{origin_.x + marker_size_ / 2, origin_.y + marker_size_ / 2};
            marker_size_ = std::clamp(marker_size_ + (event.scrolling_delta_y > 0 ? 8 : -8), 24, 160);
            pin_marker(center);
            return;
        }
        const double scale = event.precise ? 1.0 : 8.0;
        pan_marker(int(std::lround(std::clamp(event.scrolling_delta_x * scale, -48.0, 48.0))),
                   int(std::lround(std::clamp(event.scrolling_delta_y * scale, -48.0, 48.0))));
    }

    void input_focus_lost() noexcept override {
        hover_.reset();
        drag_origin_.reset();
    }

    void key_event(KeyEvent event) noexcept override {
        if (event.phase != KeyEvent::Phase::Down || event.repeat) return;
        if (event.characters_ignoring_modifiers == " ") alternate_ = !alternate_;
        if (event.characters_ignoring_modifiers == "\x1b") cancel_selection();
    }

private:
    Frame render(std::span<std::uint8_t> pixels) {
        if (!pinned_) {
            origin_.x = int((index_ * 4) % (width - marker_size_ + 1));
        }
        if (hover_ && !drag_origin_) {
            pan_marker(edge_delta(hover_->x, width), edge_delta(hover_->y, height));
        }
        paint_rect(pixels, 0, 0, width, height, background);
        paint_rect(pixels, origin_.x, origin_.y, origin_.x + marker_size_, origin_.y + marker_size_,
                   alternate_ ? alternate_marker : marker_);
        if (selection_) paint_outline(pixels, *selection_);
        return Frame{{index_++, 1, false}, pixels};
    }

    static int edge_delta(int coordinate, int extent) {
        if (coordinate < edge_threshold) return -edge_speed;
        if (coordinate >= extent - edge_threshold) return edge_speed;
        return 0;
    }

    void pin_marker(Point center) noexcept {
        set_origin(center.x - marker_size_ / 2, center.y - marker_size_ / 2);
    }

    void pan_marker(int x, int y) noexcept {
        if (x != 0 || y != 0) set_origin(origin_.x + x, origin_.y + y);
    }

    void set_origin(int x, int y) noexcept {
        origin_ = {std::clamp(x, 0, width - marker_size_), std::clamp(y, 0, height - marker_size_)};
        pinned_ = true;
    }

    void cancel_selection() noexcept {
        drag_origin_.reset();
        selection_.reset();
    }

    Pixel marker_;
    std::array<std::shared_ptr<std::vector<std::uint8_t>>, 2> surfaces_{};
    std::uint64_t index_ = 0;
    bool pinned_ = false;
    bool alternate_ = false;
    int marker_size_ = initial_marker_size;
    Point origin_{0, (height - initial_marker_size) / 2};
    std::optional<Point> hover_;
    std::optional<Point> drag_origin_;
    std::optional<Selection> selection_;
};

}  // namespace

std::unique_ptr<Session> create_session(const LaunchOptions& options, std::string& error) {
    if (options.skirmish_map != "Fixture.map") {
        error = "Pass --skirmish-map Fixture.map for the generated host fixture.";
        return nullptr;
    }
    auto mix = MixArchive::open(options.data_directory / "fixture.mix", {}, error);
    if (!mix) return nullptr;
    std::array<std::uint8_t, 8> data{};
    if (!mix->read(1, 0, data, error)) return nullptr;
    if (!std::equal(data.begin(), data.begin() + 4, "CHFX") || data[7] != 255) {
        error = "Invalid host fixture entry.";
        return nullptr;
    }
    return std::make_unique<FixtureSession>(Pixel{data[4], data[5], data[6], data[7]});
}

}  // namespace coastal::macos_host

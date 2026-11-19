// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/mix_archive.h"
#include "macos_host/session.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace coastal::macos_host {
namespace {

using Pixel = std::array<std::uint8_t, 4>;
constexpr Viewport viewport{1024, 768};
constexpr Pixel background{24, 37, 59, 255};
constexpr int marker_size = 64;

class FixtureSession final : public Session {
public:
    explicit FixtureSession(Pixel marker)
        : marker_(marker), pixels_(std::size_t(viewport().width) * viewport().height, background) {}

    Viewport viewport() const noexcept override { return macos_host::viewport; }
    FrameClock::duration frame_interval() const noexcept override { return std::chrono::nanoseconds(16'666'667); }

    std::optional<Frame> advance_frame() override {
        paint(previous_x_, previous_y_, background);
        const auto span = viewport().width - marker_size;
        previous_x_ = pinned_ ? pointer_x_ : int((index_ * 4) % (span + 1));
        previous_y_ = pinned_ ? pointer_y_ : int((viewport().height - marker_size) / 2);
        paint(previous_x_, previous_y_, marker_);
        return Frame{{index_++, 1, false},
                     {reinterpret_cast<const std::uint8_t*>(pixels_.data()), pixels_.size() * sizeof(Pixel)}};
    }

    void pointer_event(PointerEvent event) noexcept override {
        if (event.button != 0 || event.phase == PointerEvent::Phase::Up ||
            !std::isfinite(event.x) || !std::isfinite(event.y)) return;
        pinned_ = true;
        pointer_x_ = int(std::clamp(event.x - marker_size / 2.0, 0.0,
                                    double(viewport().width - marker_size)));
        pointer_y_ = int(std::clamp(event.y - marker_size / 2.0, 0.0,
                                    double(viewport().height - marker_size)));
    }

private:
    void paint(int x, int y, Pixel pixel) {
        for (int row = 0; row < marker_size; ++row) {
            std::fill_n(pixels_.data() + (y + row) * viewport().width + x, marker_size, pixel);
        }
    }

    Pixel marker_;
    std::vector<Pixel> pixels_;
    std::uint64_t index_ = 0;
    bool pinned_ = false;
    int pointer_x_ = 0, pointer_y_ = 0;
    int previous_x_ = 0, previous_y_ = 0;
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

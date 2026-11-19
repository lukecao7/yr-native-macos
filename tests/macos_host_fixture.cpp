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
    explicit FixtureSession(Pixel marker) : marker_(marker) {}

    Viewport viewport() const noexcept override { return macos_host::viewport; }
    FrameClock::duration frame_interval() const noexcept override { return std::chrono::nanoseconds(16'666'667); }

    std::optional<Frame> advance_frame() override {
        auto& surface = surfaces_[index_ % surfaces_.size()];
        if (!surface || !surface.unique()) {
            surface = std::make_shared<Surface>(std::size_t(viewport().width) * viewport().height);
        }
        paint(*surface, background);
        const auto span = viewport().width - marker_size;
        surface->x = pinned_ ? pointer_x_ : int((index_ * 4) % (span + 1));
        surface->y = pinned_ ? pointer_y_ : int((viewport().height - marker_size) / 2);
        paint(*surface, marker_);
        return Frame{{index_++, 1, false},
                     {reinterpret_cast<const std::uint8_t*>(surface->pixels.data()),
                      surface->pixels.size() * sizeof(Pixel)},
                     surface};
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
    struct Surface {
        explicit Surface(std::size_t size) : pixels(size, background) {}
        std::vector<Pixel> pixels;
        int x = 0, y = 0;
    };

    void paint(Surface& surface, Pixel pixel) {
        for (int row = 0; row < marker_size; ++row) {
            auto* begin = surface.pixels.data() + (surface.y + row) * viewport().width + surface.x;
            std::fill_n(begin, marker_size, pixel);
        }
    }

    Pixel marker_;
    std::array<std::shared_ptr<Surface>, 2> surfaces_{};
    std::uint64_t index_ = 0;
    bool pinned_ = false;
    int pointer_x_ = 0, pointer_y_ = 0;
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

// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "frame_loop.h"
#include "launch_options.h"

#include <memory>

namespace coastal::macos_host {

struct Viewport {
    std::uint32_t width;
    std::uint32_t height;
};

struct Frame {
    FrameCommit commit;
    // Tightly packed, top-left origin, opaque RGBA8. Valid until the next advance.
    std::span<const std::uint8_t> rgba;
};

struct PointerEvent {
    enum class Phase { Down, Drag, Up };
    Phase phase;
    std::uint32_t button;
    double x;
    double y;
};

class Session {
public:
    virtual ~Session() = default;
    virtual Viewport viewport() const noexcept = 0;
    virtual FrameClock::duration frame_interval() const noexcept = 0;
    // Called by the AppKit main thread. Return promptly and report no frame
    // while waiting for work. Each ready frame gets a sequential index.
    virtual std::optional<Frame> advance_frame() = 0;
    virtual void pointer_event(PointerEvent event) noexcept = 0;
};

// Implemented by the runtime linked with the host. Options carry the resolved
// data directory, requested map name and audit mode. Empty means startup failed.
std::unique_ptr<Session> create_session(const LaunchOptions& options, std::string& error);

}  // namespace coastal::macos_host

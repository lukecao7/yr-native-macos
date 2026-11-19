// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "frame_loop.h"
#include "launch_options.h"

#include <memory>
#include <span>
#include <string_view>

namespace coastal::macos_host {

struct Viewport {
    std::uint32_t width;
    std::uint32_t height;
};

struct Frame {
    FrameCommit commit;
    // Tightly packed, top-left origin, opaque RGBA8. Without an owner, valid until the next advance.
    std::span<const std::uint8_t> rgba;
    // An optional owner for the pixel storage. When supplied, the span must stay
    // valid and its bytes unchanged until the last reference is released.
    std::shared_ptr<const void> rgba_owner{};
};

using InputModifiers = std::uint64_t;

// AppKit's device-independent flag values. Events also retain the other bits.
enum class Modifier : InputModifiers {
    CapsLock = 1ULL << 16, Shift = 1ULL << 17, Control = 1ULL << 18,
    Option = 1ULL << 19, Command = 1ULL << 20, Function = 1ULL << 23,
};

struct PointerEvent {
    enum class Phase { Down, Drag, Up, Move, Enter, Leave };
    Phase phase;
    std::uint32_t button;
    double x;
    double y;
    InputModifiers modifiers = 0;
    std::uint32_t click_count = 0;
};

struct ScrollEvent {
    double x;
    double y;
    double delta_x;
    double delta_y;
    double scrolling_delta_x;
    double scrolling_delta_y;
    InputModifiers modifiers;
    std::uint32_t phase;
    std::uint32_t momentum_phase;
    bool precise;
    bool inverted;
};

struct KeyEvent {
    enum class Phase { Down, Up, ModifiersChanged };
    Phase phase;
    std::uint16_t key_code;
    InputModifiers modifiers;
    bool repeat;
    // Borrowed UTF-8 for the duration of key_event().
    std::string_view characters;
    std::string_view characters_ignoring_modifiers;
};

class Session {
public:
    virtual ~Session() = default;
    virtual Viewport viewport() const noexcept = 0;
    virtual FrameClock::duration frame_interval() const noexcept = 0;
    // Called by the AppKit main thread. Return promptly and report no frame
    // while waiting for work. Each ready frame gets a sequential index.
    virtual std::optional<Frame> advance_frame() = 0;
    // Opt-in runtimes render directly into a tightly packed shared RGBA8 frame.
    // The target is borrowed for this call. Return its full span in Frame::rgba
    // and do not retain it. If the target is unavailable, the host skips the tick.
    virtual bool supports_shared_frames() const noexcept { return false; }
    virtual std::optional<Frame> advance_frame_into(std::span<std::uint8_t>) {
        return advance_frame();
    }
    virtual void pointer_event(PointerEvent event) noexcept = 0;
    virtual void scroll_event(ScrollEvent) noexcept {}
    virtual void key_event(KeyEvent) noexcept {}
    virtual void input_focus_lost() noexcept {}
};

// Implemented by the runtime linked with the host. Options carry the resolved
// data directory, requested map name and audit mode. Empty means startup failed.
std::unique_ptr<Session> create_session(const LaunchOptions& options, std::string& error);

}  // namespace coastal::macos_host

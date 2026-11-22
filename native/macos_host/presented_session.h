// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "macos_host/session.h"
#include <memory>

namespace coastal::macos_host {
// Explicit opt-in to a single worker-thread simulation owner. The caller must
// supply a session whose frame/input/scene methods may run on that worker and
// return in finite time; they must not call AppKit or require the main thread.
// viewport(), frame_interval() and terrain_for_metal() are read during setup on
// the calling thread. Terrain must remain immutable; later queries use a cache.
// Call the wrapper and destroy it from its single host thread; destruction joins
// any in-flight frame. Inputs retain their delivery order. advance_frame() and
// scene frames own immutable snapshots; advance_frame_into() borrows its target
// and leaves Frame::rgba_owner empty for a copied 2D frame.
// Ready notifications run outside the publication lock. They are hints only;
// consumers must still poll on their host thread and release frames promptly.
std::unique_ptr<coastal::macos_host::Session> make_presented_session(
    std::unique_ptr<coastal::macos_host::Session>);
}

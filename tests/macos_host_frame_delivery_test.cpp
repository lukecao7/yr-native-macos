// SPDX-License-Identifier: GPL-3.0-only
#include "macos_host/display_confirmation.h"
#include "macos_host/frame_delivery.h"
#include "rts_3d/scene.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace host = coastal::macos_host;
namespace view = coastal::rts::view3d;

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class FakeSession final : public host::Session {
public:
    explicit FakeSession(bool scene_mode) : scene_mode(scene_mode) {}
    host::Viewport viewport() const noexcept override { return {2, 2}; }
    host::FrameClock::duration frame_interval() const noexcept override { return 10ms; }
    bool supports_shared_frames() const noexcept override { return true; }
    void pointer_event(host::PointerEvent) noexcept override {}
    std::optional<host::Frame> advance_frame() override { return advance_frame_into(fallback); }
    std::optional<host::Frame> advance_frame_into(std::span<std::uint8_t> target) override {
        ++advance_calls;
        if (!ready) return std::nullopt;
        const auto index = renders++;
        const auto marker = static_cast<std::uint8_t>(index + 1);
        std::fill(target.begin(), target.end(), marker);
        scene.step = index;
        scene.instances.resize(1);
        scene.instances.front().anchor_yaw.x = static_cast<float>(index + 40);
        std::span<const std::uint8_t> rgba = target;
        if (scene_mode) rgba = {};
        return host::Frame{{index + index_offset, 60 + index, true}, rgba, {}, scene_mode ? &scene : nullptr};
    }

    bool ready = true, scene_mode;
    std::uint64_t renders = 0, advance_calls = 0, index_offset = 0;
    view::SceneFrame scene;
    std::array<std::uint8_t, 16> fallback{};
};

struct FakePresenter {
    std::array<std::uint8_t, 16> pixels{};
    std::vector<host::FramePresentation> outcomes;
    std::vector<std::uint64_t> attempts, submitted;
    std::size_t acquisitions = 0;
    bool scene_mode = false;

    std::span<std::uint8_t> writable_pixels() {
        ++acquisitions;
        std::fill(pixels.begin(), pixels.end(), 0xCC);
        return pixels;
    }
    host::FramePresentation present(const host::Frame& frame) {
        const auto index = frame.commit.index;
        expect(frame.commit.full_original_frame && frame.commit.active_objects == 60 + index,
               "Retry must retain the original frame metadata");
        if (scene_mode) {
            expect(frame.scene && frame.scene->step == index && frame.scene->instances.size() == 1 &&
                       frame.scene->instances.front().anchor_yaw.x == static_cast<float>(index + 40),
                   "A retry must retain valid scene data even after session input changes it");
        } else {
            expect(!frame.scene && frame.rgba.data() == pixels.data() && frame.rgba.size() == pixels.size() &&
                       std::all_of(frame.rgba.begin(), frame.rgba.end(), [marker = static_cast<std::uint8_t>(index + 1)](auto byte) { return byte == marker; }),
                   "A retry must retain the same shared pixel slot and rendered bytes");
        }
        expect(attempts.size() < outcomes.size(), "Unexpected extra presentation attempt");
        const auto result = outcomes[attempts.size()];
        attempts.push_back(index);
        if (result == host::FramePresentation::Presented) submitted.push_back(index);
        return result;
    }
};

struct Fixture {
    host::FrameClock::time_point time{};
    host::FrameLoop loop{10ms, time, [this] { return time; }};
    host::FrameDelivery delivery;
    FakeSession session;
    FakePresenter presenter;

    explicit Fixture(bool scene_mode) : session(scene_mode) { presenter.scene_mode = scene_mode; }
    host::FrameTick tick() {
        const auto result = loop.tick([&] {
            return delivery.step(loop,
                [&] { return session.advance_frame_into(presenter.writable_pixels()); },
                [&](const auto& frame) { return presenter.present(frame); });
        });
        time = loop.next_due();
        return result;
    }
};

void retry_the_same_frame(bool scene_mode) {
    Fixture fixture(scene_mode);
    fixture.presenter.outcomes = {host::FramePresentation::Unavailable, host::FramePresentation::Unavailable,
                                  host::FramePresentation::Presented, host::FramePresentation::Presented};
    expect(fixture.tick() == host::FrameTick::WaitingForFrame, "First unavailable drawable waits");
    // A keyboard load/view action can update the session's borrowed scene while
    // the host waits. The ready frame must remain a snapshot of its own tick.
    fixture.session.scene.step = 999;
    fixture.session.scene.instances.clear();
    expect(fixture.tick() == host::FrameTick::WaitingForFrame, "A second unavailable drawable still waits");
    auto metrics = fixture.loop.metrics();
    expect(fixture.session.renders == 1 && fixture.session.advance_calls == 1 && fixture.presenter.acquisitions == 1,
           "Retries neither rerender nor acquire another shared CPU slot");
    expect(metrics.presented_frames == 0 && metrics.full_original_frames == 0 && !metrics.first_present && !metrics.frame_work,
           "Unavailable attempts cannot fabricate submissions, original frames or presentation timing");
    expect(fixture.tick() == host::FrameTick::Presented, "The same frame commits when its drawable becomes available");
    metrics = fixture.loop.metrics();
    expect(metrics.presented_frames == 1 && metrics.full_original_frames == 1 && metrics.last_original_active_objects == 60,
           "Only the successful retry increments the host counters");
    expect(fixture.session.renders == 1 && fixture.presenter.submitted == std::vector<std::uint64_t>{0},
           "Successful retry submits the already-rendered frame zero once");
    expect(fixture.tick() == host::FrameTick::Presented && fixture.session.renders == 2 && fixture.presenter.acquisitions == 2 &&
               fixture.presenter.attempts == std::vector<std::uint64_t>({0, 0, 0, 1}) &&
               fixture.presenter.submitted == std::vector<std::uint64_t>({0, 1}),
           "The next new frame remains consecutive and reuses pixels only after submission");
}

void confirm_display_before_releasing_first_frame() {
    using Poll = host::DisplayConfirmation::Poll;
    host::FrameClock::time_point time{};
    host::FrameLoop loop{10ms, time, [&] { return time; }};
    host::FrameDelivery delivery;
    std::array<std::uint8_t, 16> pixels{};
    std::vector<std::uint64_t> submissions;
    std::shared_ptr<host::DisplayConfirmation> confirmation;
    std::uint64_t advances = 0;
    bool gpu_retired = false;
    const auto tick = [&] {
        const auto result = loop.tick([&] {
            return delivery.step(loop, [&] {
                const auto index = advances++;
                pixels.fill(static_cast<std::uint8_t>(index + 42));
                return host::Frame{{index, 1, false, index == 0}, pixels};
            }, [&](const auto& frame) {
                const auto index = frame.commit.index;
                expect(frame.rgba.data() == pixels.data() && frame.rgba.front() == index + 42,
                       "Retry holds the same rendered bytes.");
                if (confirmation) {
                    switch (confirmation->poll(index, gpu_retired)) {
                    case Poll::Waiting: return host::FramePresentation::Unavailable;
                    case Poll::WrongFrame: return host::FramePresentation::Invalid;
                    case Poll::Displayed:
                        confirmation.reset();
                        return host::FramePresentation::Presented;
                    case Poll::Retry:
                        confirmation.reset();
                        break;
                    }
                }
                submissions.push_back(index);
                if (!frame.commit.requires_display_confirmation) return host::FramePresentation::Presented;
                confirmation = std::make_shared<host::DisplayConfirmation>(index);
                gpu_retired = false;
                return host::FramePresentation::Unavailable;
            });
        });
        time = loop.next_due();
        return result;
    };
    expect(tick() == host::FrameTick::WaitingForFrame && submissions == std::vector<std::uint64_t>{0},
           "Submission did not confirm the first frame's display.");
    expect(confirmation->poll(1, true) == Poll::WrongFrame,
           "A successor cannot enter the pending frame's confirmation.");
    expect(tick() == host::FrameTick::WaitingForFrame && advances == 1 && submissions.size() == 1,
           "Pending confirmation neither advances nor submits again.");

    const auto skipped = confirmation;
    std::thread([skipped] { skipped->complete(0); }).join();
    expect(tick() == host::FrameTick::WaitingForFrame && submissions.size() == 1,
           "A skipped drawable also needs the GPU slot to retire.");
    gpu_retired = true;
    expect(tick() == host::FrameTick::WaitingForFrame && advances == 1 &&
               submissions == std::vector<std::uint64_t>({0, 0}),
           "A skipped drawable retries the same frame and pixel slot.");
    const auto retried = confirmation;
    skipped->complete(1);
    expect(retried->poll(0, true) == Poll::Waiting && skipped->poll(0, true) == Poll::Retry,
           "Stale and duplicate callbacks cannot accept a different drawable.");
    std::thread([retried] { retried->complete(2); }).join();
    expect(tick() == host::FrameTick::Presented && advances == 1 && submissions.size() == 2,
           "A positive callback accepts the original frame without resubmission.");
    expect(tick() == host::FrameTick::Presented && advances == 2 &&
               submissions == std::vector<std::uint64_t>({0, 0, 1}) &&
               loop.metrics().presented_frames == 2 && loop.metrics().full_original_frames == 0,
           "The next ordinary frame submits immediately and source metrics stay independent.");

    host::DisplayConfirmation invalid_time{0};
    invalid_time.complete(std::numeric_limits<double>::infinity());
    expect(invalid_time.poll(0, true) == Poll::Retry, "Nonfinite times do not confirm display.");
}

void keep_unready_and_invalid_distinct() {
    Fixture waiting(false);
    waiting.session.ready = false;
    expect(waiting.tick() == host::FrameTick::WaitingForFrame && waiting.presenter.attempts.empty(),
           "An unready session never calls the presenter");
    waiting.session.ready = true;
    waiting.presenter.outcomes = {host::FramePresentation::Presented};
    expect(waiting.tick() == host::FrameTick::Presented && waiting.presenter.submitted.front() == 0,
           "Session waiting also preserves the initial frame index");

    Fixture invalid(false);
    invalid.presenter.outcomes = {host::FramePresentation::Unavailable, host::FramePresentation::Invalid};
    expect(invalid.tick() == host::FrameTick::WaitingForFrame, "A pending frame can encounter a later real error");
    bool failed = false;
    try { invalid.tick(); } catch (const std::runtime_error&) { failed = true; }
    expect(failed && invalid.loop.metrics().presented_frames == 0 && invalid.loop.metrics().full_original_frames == 0 &&
               invalid.tick() == host::FrameTick::Stopped && invalid.session.renders == 1 && invalid.presenter.attempts.size() == 2,
           "A real presentation error still stops the loop without submitting or retrying");
    invalid.delivery.clear();

    Fixture sequence(false);
    sequence.session.index_offset = 1;
    expect(sequence.tick() == host::FrameTick::InvalidFrame && sequence.presenter.attempts.empty() &&
               sequence.loop.metrics().presented_frames == 0,
           "A nonconsecutive session commit is rejected before presentation");
}

void release_a_cancelled_owned_frame() {
    host::FrameLoop loop(10ms, host::FrameClock::now());
    host::FrameDelivery delivery;
    std::weak_ptr<std::vector<std::uint8_t>> bytes;
    const auto waiting = delivery.step(loop, [&]() -> std::optional<host::Frame> {
        auto owner = std::make_shared<std::vector<std::uint8_t>>(16, 42);
        bytes = owner;
        return host::Frame{{0, 1, false}, *owner, owner};
    }, [](const auto& frame) {
        expect(frame.rgba.size() == 16 && frame.rgba.front() == 42, "Owned frame bytes are valid at presentation");
        return host::FramePresentation::Unavailable;
    });
    expect(!waiting && !bytes.expired(), "A waiting frame retains its optional pixel-storage owner");
    delivery.clear();
    expect(bytes.expired(), "Cancelling delivery releases the pending frame storage");
}

}  // namespace

int main() {
    try {
        retry_the_same_frame(false);
        retry_the_same_frame(true);
        confirm_display_before_releasing_first_frame();
        keep_unready_and_invalid_distinct();
        release_a_cancelled_owned_frame();
        std::cout << "macos_host_frame_delivery_test: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "macos_host_frame_delivery_test: " << error.what() << '\n';
        return 1;
    }
}

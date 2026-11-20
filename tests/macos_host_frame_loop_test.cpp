#include "macos_host/frame_loop.h"

#include <array>
#include <iostream>
#include <stdexcept>

using namespace std::chrono_literals;
using namespace coastal::macos_host;

namespace {

void expect(bool condition, const char* description) {
    if (!condition) {
        throw std::runtime_error(description);
    }
}

struct Fixture {
    FrameClock::time_point launched{};
    FrameClock::time_point time = launched;
    FrameLoop loop;
    std::uint64_t draws = 0;

    explicit Fixture(std::size_t sample_limit = 4096)
        : loop(10ms, launched, [this] { return time; }, sample_limit) {}

    FrameCommit draw_if_accepted(FrameCommit commit) {
        if (loop.accepts_frame(commit)) {
            ++draws;
        }
        return commit;
    }
};

void continuously_submit_diagnostic_frames() {
    Fixture fixture;
    const auto empty = fixture.loop.metrics();
    expect(empty.presented_frames == 0 && empty.full_original_frames == 0 &&
               !empty.active_objects && !empty.first_present && !empty.frame_work &&
               !empty.presentation_interval,
           "Empty loop has no timing samples");

    for (std::uint64_t index = 0; index < 300; ++index) {
        fixture.time = fixture.launched + 2ms + 10ms * index;
        const auto status = fixture.loop.tick([&] {
            fixture.time += 1ms * (1 + index % 5);
            return fixture.draw_if_accepted({index, 40 + index});
        });
        expect(status == FrameTick::Presented, "All 300 numbered host frames commit");
    }

    const auto metrics = fixture.loop.metrics();
    expect(fixture.draws == 300 && metrics.presented_frames == 300 && metrics.full_original_frames == 0 &&
               metrics.active_objects == 339 && metrics.first_present == 3ms,
           "Fixture commits stay diagnostic and preserve object and first-frame measurements");
    expect(metrics.frame_work && metrics.frame_work->sample_count == 300 &&
               metrics.frame_work->p50 == 3ms && metrics.frame_work->p95 == 5ms &&
               metrics.frame_work->p99 == 5ms,
           "Frame-work percentiles use nearest rank");
    expect(metrics.presentation_interval &&
               metrics.presentation_interval->sample_count == 299 &&
               metrics.presentation_interval->p50 == 11ms &&
               metrics.presentation_interval->p95 == 11ms &&
               metrics.presentation_interval->p99 == 11ms,
           "Presentation percentiles measure elapsed time between commits");
}

void skip_waiting_and_late_slots() {
    Fixture fixture;
    expect(fixture.loop.tick([&]() -> std::optional<FrameCommit> {
               fixture.time += 2ms;
               return std::nullopt;
           }) == FrameTick::WaitingForFrame,
           "An unready renderer consumes a slot without a commit");
    expect(fixture.loop.metrics().presented_frames == 0, "Waiting creates no frame");

    const auto should_not_run = []() -> FrameCommit {
        throw std::runtime_error("An early or stopped dispatch ran a callback");
    };
    expect(fixture.loop.tick(should_not_run) == FrameTick::NotDue,
           "An unready renderer cannot spin within the same slot");

    fixture.time = fixture.launched + 10ms;
    expect(fixture.loop.tick([&] {
               fixture.time += 35ms;
               expect(fixture.loop.tick(should_not_run) == FrameTick::NotDue,
                      "A callback cannot dispatch the same loop recursively");
               return FrameCommit{0, 7};
           }) == FrameTick::Presented,
           "A long callback still reports its single completed frame");
    expect(fixture.loop.tick(should_not_run) == FrameTick::NotDue,
           "Elapsed slots are not replayed after a long callback");
    expect(fixture.loop.next_due() == fixture.launched + 50ms,
           "The host can schedule its timer for the next future slot");
    expect(fixture.loop.metrics().first_present == 45ms,
           "First presentation includes startup and waiting");

    fixture.time = fixture.launched + 49ms;
    expect(fixture.loop.tick(should_not_run) == FrameTick::NotDue,
           "The next deadline remains on the cadence");
    fixture.time += 1ms;
    expect(fixture.loop.tick([] { return FrameCommit{1, 8}; }) == FrameTick::Presented,
           "The next future deadline dispatches a new frame");

    fixture.time = fixture.launched + 900ms;
    expect(fixture.loop.tick([] { return FrameCommit{2, 9}; }) == FrameTick::Presented &&
               fixture.loop.tick(should_not_run) == FrameTick::NotDue,
           "A delayed host callback dispatches at most one frame");

    fixture.loop.stop();
    fixture.loop.stop();
    expect(fixture.loop.tick(should_not_run) == FrameTick::Stopped &&
               fixture.loop.metrics().presented_frames == 3,
           "Stopping is idempotent and leaves prior observations available");
}

void require_valid_frame_sequence() {
    Fixture wrong_first;
    expect(wrong_first.loop.tick([&] { return wrong_first.draw_if_accepted({1, 2}); }) == FrameTick::InvalidFrame &&
               wrong_first.draws == 0 &&
               wrong_first.loop.metrics().presented_frames == 0 &&
               !wrong_first.loop.metrics().first_present,
           "A sequence must start at zero");

    Fixture jumped;
    expect(jumped.loop.tick([&] { return jumped.draw_if_accepted({0, 2}); }) == FrameTick::Presented,
           "Accept the initial state");
    jumped.time += 10ms;
    expect(jumped.loop.tick([&] { return jumped.draw_if_accepted({2, 3}); }) == FrameTick::InvalidFrame &&
               jumped.draws == 1 &&
               jumped.loop.metrics().presented_frames == 1 &&
               jumped.loop.metrics().active_objects == 2 &&
               !jumped.loop.metrics().presentation_interval,
           "Reject a skipped index before drawing or recording its timing or object count");
    expect(jumped.loop.tick([] { return FrameCommit{1, 3}; }) == FrameTick::Stopped,
           "A sequence error stops further dispatch");

    Fixture duplicate;
    expect(duplicate.loop.tick([&] { return duplicate.draw_if_accepted({0, 5}); }) == FrameTick::Presented,
           "Accept frame zero before checking duplication");
    duplicate.time += 10ms;
    expect(duplicate.loop.tick([&] { return duplicate.draw_if_accepted({0, 6}); }) == FrameTick::InvalidFrame &&
               duplicate.draws == 1 &&
               duplicate.loop.metrics().presented_frames == 1,
           "A duplicate index cannot become a second presentation");

    Fixture invalid_clock;
    expect(invalid_clock.loop.tick([&] {
               invalid_clock.time -= 1ns;
               return FrameCommit{0, 1};
           }) == FrameTick::InvalidFrame && invalid_clock.loop.metrics().presented_frames == 0,
           "Reject a regressing injected clock");
}

void count_complete_frames_across_menu_transitions() {
    Fixture fixture;
    // These flags are unit-test inputs for the counting rule.
    expect(fixture.loop.tick([] { return FrameCommit{0, 0, false}; }) == FrameTick::Presented &&
               fixture.loop.metrics().full_original_frames == 0 &&
               !fixture.loop.metrics().last_original_active_objects,
           "The main menu has no original frames or original objects");
    fixture.time += 10ms;
    expect(fixture.loop.tick([] { return FrameCommit{1, 3, true}; }) == FrameTick::Presented &&
               fixture.loop.metrics().full_original_frames == 1,
           "Count a complete match frame after the main menu");
    fixture.time += 10ms;
    expect(fixture.loop.tick([] { return FrameCommit{2, 4, true}; }) == FrameTick::Presented &&
               fixture.loop.metrics().full_original_frames == 2,
           "Count consecutive complete metadata");

    fixture.time += 10ms;
    expect(fixture.loop.tick([] { return FrameCommit{3, 0}; }) == FrameTick::Presented &&
               fixture.loop.metrics().full_original_frames == 2 &&
               fixture.loop.metrics().active_objects == 0 &&
               fixture.loop.metrics().last_original_active_objects == 4,
           "Returning to a menu preserves match totals and distinguishes current object count");
    fixture.time += 10ms;
    expect(fixture.loop.tick([] { return FrameCommit{4, 5, true}; }) == FrameTick::Presented &&
               fixture.loop.metrics().full_original_frames == 3 &&
               fixture.loop.metrics().last_original_active_objects == 5 &&
               fixture.loop.metrics().presented_frames == 5,
           "A restarted match contributes only its own complete frames");

    Fixture invalid;
    expect(invalid.loop.tick([] { return FrameCommit{0, 3, true}; }) == FrameTick::Presented,
           "Start a complete sequence");
    invalid.time += 10ms;
    expect(invalid.loop.tick([] { return FrameCommit{2, 4, true}; }) == FrameTick::InvalidFrame &&
               invalid.loop.metrics().full_original_frames == 0,
           "An index error invalidates the complete sequence");
}

void retain_a_bounded_timing_window() {
    Fixture fixture(3);
    constexpr std::array work = {0ms, 1ms, 3ms, 6ms, 2ms, 7ms};
    for (std::uint64_t index = 0; index < work.size(); ++index) {
        fixture.time = fixture.launched + 10ms * index;
        expect(fixture.loop.tick([&] {
                   fixture.time += work[index];
                   return FrameCommit{index, 0};
               }) == FrameTick::Presented,
               "The sample window can wrap without losing frame counts");
    }
    const auto bounded = fixture.loop.metrics();
    expect(bounded.presented_frames == 6 && bounded.frame_work &&
               bounded.frame_work->sample_count == 3 && bounded.frame_work->p50 == 6ms &&
               bounded.frame_work->p95 == 7ms && bounded.frame_work->p99 == 7ms &&
               bounded.presentation_interval && bounded.presentation_interval->sample_count == 3 &&
               bounded.presentation_interval->p50 == 13ms &&
               bounded.presentation_interval->p95 == 15ms &&
               bounded.presentation_interval->p99 == 15ms,
           "A full timing window retains its newest samples");

    Fixture without_samples(0);
    expect(without_samples.loop.tick([] { return FrameCommit{0, 4}; }) == FrameTick::Presented,
           "A zero-length sample window still commits");
    const auto disabled = without_samples.loop.metrics();
    expect(disabled.presented_frames == 1 && disabled.first_present == 0ns &&
               !disabled.frame_work && !disabled.presentation_interval,
           "A disabled sample window retains start and count metrics");
}

void check_stop_during_dispatch() {
    Fixture stopped;
    expect(stopped.loop.tick([&] {
               stopped.loop.stop();
               return FrameCommit{0, 1};
           }) == FrameTick::Stopped && stopped.loop.metrics().presented_frames == 0,
           "An explicit stop prevents committing a pending callback result");

    Fixture failed;
    bool propagated = false;
    try {
        failed.loop.tick([]() -> std::optional<FrameCommit> {
            throw std::runtime_error("fixture error");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    expect(propagated && failed.loop.tick([] { return FrameCommit{0, 1}; }) == FrameTick::Stopped &&
               failed.loop.metrics().presented_frames == 0,
           "A failed callback surfaces its error and leaves the loop stopped");
}

void check_platform_process_rss() {
#if defined(__APPLE__)
    const auto rss = process_peak_rss_bytes();
    expect(rss && *rss >= 512 * 1024, "Darwin reports the peak RSS in bytes");
#endif
}

}  // namespace

int main() {
    try {
        continuously_submit_diagnostic_frames();
        skip_waiting_and_late_slots();
        require_valid_frame_sequence();
        count_complete_frames_across_menu_transitions();
        retain_a_bounded_timing_window();
        check_stop_during_dispatch();
        check_platform_process_rss();
        std::cout << "macOS host frame-loop tests passed (300 diagnostic frames)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

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

void follow_display_pulses_without_a_second_clock() {
    Fixture fixture;
    fixture.time += 3ms;
    expect(fixture.loop.display_tick(100, [&] {
               fixture.time += 2ms;
               return FrameCommit{0, 12, true};
           }) == FrameTick::Presented,
           "A real display pulse starts a sequential frame");
    fixture.time = fixture.launched + 13ms;
    expect(fixture.time < fixture.loop.next_due(),
           "The independent completion-relative timer would still be early");
    expect(fixture.loop.display_tick(200, [&] {
               fixture.time += 2ms;
               return FrameCommit{1, 13, true};
           }) == FrameTick::Presented,
           "The next display pulse is not gated by an independent timer phase");

    fixture.time = fixture.launched + 23ms;
    expect(fixture.loop.display_tick(300, [&] {
               fixture.time += 35ms;
               return FrameCommit{2, 14, true};
           }) == FrameTick::Presented,
           "A drawable stall still commits exactly its real session frame");
    fixture.time = fixture.launched + 63ms;
    expect(fixture.loop.display_tick(700, [] { return FrameCommit{3, 15, true}; }) == FrameTick::Presented,
           "After a stall the latest display request does not skip a further whole timer slot");
    const auto metrics = fixture.loop.metrics();
    expect(metrics.presented_frames == 4 && metrics.full_original_frames == 4 &&
               metrics.last_original_active_objects == 15 && metrics.first_present == 5ms &&
               metrics.presentation_interval && metrics.presentation_interval->sample_count == 3 &&
               metrics.presentation_interval->p50 == 10ms && metrics.presentation_interval->p95 == 43ms &&
               metrics.frame_work && metrics.frame_work->p95 == 35ms,
           "Display pacing retains actual completion intervals and stalled work, without fabricated frames");
}

void retry_an_unready_display_pulse_without_duplicate_commits() {
    Fixture fixture;
    std::uint64_t calls = 0;
    expect(fixture.loop.display_tick(100, [&]() -> std::optional<FrameCommit> {
               ++calls;
               fixture.time += 1ms;
               return std::nullopt;
           }) == FrameTick::WaitingForFrame,
           "An empty display attempt does not count as a presentation");
    expect(fixture.loop.display_tick(100, [&] {
               ++calls;
               fixture.time += 1ms;
               return FrameCommit{0, 4};
           }) == FrameTick::Presented,
           "A producer that becomes ready in the same refresh can deliver its real frame");
    const auto must_not_run = []() -> FrameCommit {
        throw std::runtime_error("A stale display pulse dispatched a frame");
    };
    expect(fixture.loop.display_tick(100, must_not_run) == FrameTick::NotDue &&
               fixture.loop.display_tick(99, must_not_run) == FrameTick::NotDue && calls == 2 &&
               fixture.loop.metrics().presented_frames == 1 &&
               !fixture.loop.metrics().presentation_interval,
           "A committed or stale refresh cannot repeat a frame or add timing samples");

    expect(fixture.loop.display_tick(200, [&]() -> std::optional<FrameCommit> {
               expect(fixture.loop.display_tick(300, must_not_run) == FrameTick::NotDue,
                      "A display callback cannot reenter with a newer pulse");
               return std::nullopt;
           }) == FrameTick::WaitingForFrame &&
               fixture.loop.display_tick(200, [] { return FrameCommit{1, 5}; }) == FrameTick::Presented,
           "A rejected reentrant pulse does not consume a pending display attempt");
    expect(fixture.loop.display_tick(300, [] { return FrameCommit{3, 6}; }) == FrameTick::InvalidFrame &&
               fixture.loop.metrics().presented_frames == 2,
           "Display pacing retains consecutive-index validation");
    expect(fixture.loop.display_tick(400, must_not_run) == FrameTick::Stopped,
           "A display sequence error stops all later pulses");
}

void preserve_the_session_rate_on_different_displays() {
    const auto count = [](FrameClock::duration refresh, std::size_t pulses) {
        DisplayCadence cadence(16'666'667ns);
        std::size_t accepted = 0;
        for (std::size_t i = 0; i < pulses; ++i) accepted += cadence.accepts_refresh(refresh);
        return accepted;
    };
    expect(count(16'666'667ns, 600) == 600 && count(8'333'333ns, 1200) == 600 &&
               count(6'944'444ns, 1440) == 600 && count(13'333'333ns, 750) == 600,
           "60 Hz sessions keep 60 frames per second on 60, 120, 144 and 75 Hz outputs");
    expect(count(20ms, 500) == 500,
           "An output slower than the session is used on every real refresh");
    expect(count(0ns, 10) == 10,
           "Missing refresh-period metadata never silently halves the callback cadence");

    bool rejected = false;
    try { DisplayCadence cadence(0ns); }
    catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "Display cadence rejects an invalid session interval");
}

void bound_display_retries_and_ignore_late_publishers() {
    Fixture fixture;
    unsigned attempts=0;
    const auto empty=[&]() -> std::optional<FrameCommit> {++attempts;return std::nullopt;};
    const auto must_not_run=[]() -> FrameCommit {throw std::runtime_error("a late or excessive retry ran");};
    expect(fixture.loop.display_tick(100,empty)==FrameTick::WaitingForFrame&&fixture.loop.can_retry_display(100),
           "A display pulse with no frame permits one later ready hint");
    expect(fixture.loop.display_tick(100,empty)==FrameTick::WaitingForFrame&&!fixture.loop.can_retry_display(100)&&
               fixture.loop.display_tick(100,must_not_run)==FrameTick::NotDue&&attempts==2,
           "Even an empty retry exhausts the pulse budget without starting a 1 ms poll chain");
    expect(fixture.loop.display_tick(200,empty)==FrameTick::WaitingForFrame&&
               fixture.loop.display_tick(100,must_not_run)==FrameTick::NotDue&&fixture.loop.can_retry_display(200),
           "A late notification cannot borrow the next pulse's retry budget");
    expect(fixture.loop.display_tick(200,[] {return FrameCommit{0,1200,true};})==FrameTick::Presented&&
               !fixture.loop.can_retry_display(200)&&fixture.loop.display_tick(200,must_not_run)==FrameTick::NotDue&&
               fixture.loop.metrics().presented_frames==1&&fixture.loop.metrics().full_original_frames==1,
           "A ready hint delivers one real frame and cannot repeat its commit");
    fixture.loop.stop();
    expect(!fixture.loop.can_retry_display(300)&&fixture.loop.display_tick(300,must_not_run)==FrameTick::Stopped,
           "Late publisher hints cannot dispatch after shutdown");
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

void measure_elapsed_time_across_late_presentations() {
    Fixture fixture;
    FrameElapsedTime elapsed;
    std::array<std::chrono::nanoseconds,3> intervals{};
    for (const auto started : {0ms, 10ms, 90ms}) {
        fixture.time = fixture.launched + started;
        expect(fixture.loop.tick([&] {
            intervals.at(fixture.draws) = elapsed.sample(fixture.time);
            return fixture.draw_if_accepted({fixture.draws, 1});
        }) == FrameTick::Presented, "Each scheduled or late draw commits once");
    }
    expect(intervals[0] == 0ns && intervals[1] == 10ms && intervals[2] == 80ms,
           "Simulation elapsed time includes skipped display slots");
    elapsed.reset();
    expect(elapsed.sample(fixture.time + 10s) == 0ns &&
           elapsed.sample(fixture.time + 10s + 5ms) == 5ms,
           "Resumed production excludes the time it was suspended");
}

}  // namespace

int main() {
    try {
        continuously_submit_diagnostic_frames();
        skip_waiting_and_late_slots();
        require_valid_frame_sequence();
        follow_display_pulses_without_a_second_clock();
        retry_an_unready_display_pulse_without_duplicate_commits();
        preserve_the_session_rate_on_different_displays();
        bound_display_retries_and_ignore_late_publishers();
        count_complete_frames_across_menu_transitions();
        retain_a_bounded_timing_window();
        check_stop_during_dispatch();
        check_platform_process_rss();
        measure_elapsed_time_across_late_presentations();
        std::cout << "macOS host frame-loop tests passed (300 diagnostic frames)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

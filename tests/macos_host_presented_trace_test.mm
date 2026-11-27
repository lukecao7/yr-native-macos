// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>
#include "macos_host/presented_trace.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace host = coastal::macos_host;
namespace {

void require(bool passed, const char* description) {
    if (!passed) throw std::runtime_error(description);
}

class History {
public:
    void add(double seconds) {
        if (!clock_.record(seconds)) return;
        const auto slot = (clock_.valid_count() - 1) % host::presentation_history_capacity;
        if (slot == slots_.size()) slots_.push_back({seconds, false});
        else slots_[slot] = {seconds, false};
    }

    bool save(const char* path) const {
        auto sorted = slots_;
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& a, const auto& b){return a.time < b.time;});
        return host::write_presented_trace(path, slots_, clock_.summarize_sorted(sorted));
    }
private:
    host::DisplayClock clock_;
    std::vector<host::PresentedFrame> slots_;
};

NSDictionary* read_trace(NSString* path) {
    NSData* data = [NSData dataWithContentsOfFile:path];
    require(data != nil, "Missing trace");
    id object = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
    require([object isKindOfClass:NSDictionary.class], "Trace is not JSON");
    return object;
}

void optional_exit_only(NSString* path) {
    History empty;
    std::string request = path.fileSystemRepresentation;
    require(host::take_exit_trace_path(false, request).empty(), "A live query consumed the exit path");
    require(![NSFileManager.defaultManager fileExistsAtPath:path], "File appeared before exit");
    const auto destination = host::take_exit_trace_path(true, request);
    require(destination == path.fileSystemRepresentation && empty.save(destination.c_str()), "No final trace");
    require(host::take_exit_trace_path(true, request).empty(), "Final path was issued twice");
    std::string opt_out;
    require(host::take_exit_trace_path(true, opt_out).empty(), "Opted-out run produced a trace path");
    NSDictionary* json = read_trace(path);
    require([json[@"positiveCallbacks"] isEqual:@0] && [json[@"callbackOrderSeconds"] isEqual:@[]] &&
            [json[@"truncated"] isEqual:@NO], "Incorrect empty snapshot");
}

void complete_trace(NSString* path) {
    History history;
    for (double value : {0., -1., std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN(), 10., 10., 12., 11., 13.}) {
        history.add(value);
    }
    require(!history.save(nullptr) && !history.save("relative.json"), "Expected an absolute path");
    require(history.save(path.fileSystemRepresentation), "Could not create trace");
    NSDictionary* json = read_trace(path);
    require([json[@"schema"] isEqual:@"coastal-presented-times.v1"] &&
            [json[@"clock"] isEqual:@"CAMetalDrawable.presentedTime"] &&
            [json[@"unit"] isEqual:@"host seconds"], "Wrong trace clock or schema");
    require([json[@"callbackOrderSeconds"] isEqual:@[@10, @10, @12, @11, @13]],
            "Sorted, deduplicated, or invalid callbacks entered the trace");
    require([json[@"positiveCallbacks"] isEqual:@5] && [json[@"invalidCallbacks"] isEqual:@4] &&
            [json[@"outOfOrderCallbacks"] isEqual:@1] && [json[@"adjacentDuplicates"] isEqual:@1] &&
            [json[@"retainedCallbacks"] isEqual:@5] && [json[@"firstRetainedCallbackIndex"] isEqual:@0] &&
            [json[@"truncated"] isEqual:@NO], "Incorrect complete-run counters");
    const auto before = [NSData dataWithContentsOfFile:path];
    require(!history.save(path.fileSystemRepresentation) &&
            [before isEqual:[NSData dataWithContentsOfFile:path]], "An existing trace was overwritten");
}

void drawable_trace(NSString* path) {
    host::DisplayClock display_clock;
    require(display_clock.record(15.), "A synthetic presentation was rejected");
    const std::array<host::PresentedFrame, 1> times{{{15., false}}};
    const std::array<host::DrawableTraceRow, 2> drawables{{
        {3, 22, 10., 10.5, 11., 11.5, 12., 13., 12.125, 12.75, 0., 14., true},
        {3, 23, 14.125, 14.25, 14.375, 14.5, 14.625, 0., 0., 0., 15., 15.25, false},
    }};
    const auto metrics = display_clock.summarize_sorted(times);
    require(!host::write_presented_trace(path.fileSystemRepresentation, times, metrics, drawables, 3),
            "Accepted an inconsistent drawable count");
    require(host::write_presented_trace(path.fileSystemRepresentation, times, metrics, drawables, 2),
            "Could not create drawable trace");
    NSDictionary* json = read_trace(path);
    NSDictionary* trace = json[@"drawables"];
    require([json[@"callbackOrderSeconds"] isEqual:@[@15]], "Drawable rows changed v1 presentation order");
    require([trace[@"requestMode"] isEqual:@"immediate"] && [trace[@"submittedDrawables"] isEqual:@2] &&
            [trace[@"firstRetainedSubmissionIndex"] isEqual:@0], "Wrong drawable trace header");
    require([trace[@"rows"] isEqual:@[
        @[@3, @22, @10, @10.5, @11, @11.5, @12, @13, @12.125, @12.75, @YES, @0, @14],
        @[@3, @23, @14.125, @14.25, @14.375, @14.5, @14.625, @0, @0, @0, @NO, @15, @15.25],
    ]], "Lost a retried drawable or mixed the GPU, request and display timestamps");
}

void bounded_trace(NSString* path) {
    constexpr std::size_t capacity = 32'769;
    constexpr auto valid = capacity*2 + 5;
    History history;
    // One duplicate and one reversed time occur in the retained part of a two-wrap run.
    const auto time_at = [](std::size_t index) {
        if (index == valid-4) --index;
        if (index == valid-2) index -= 2;
        return 100. + static_cast<double>(index) / 512.;
    };
    for (std::size_t i = 0; i < valid; ++i) history.add(time_at(i));
    history.add(-1.);
    require(history.save(path.fileSystemRepresentation), "Could not write wrapped trace");
    NSDictionary* json = read_trace(path);
    require([json[@"capacity"] unsignedLongLongValue] == capacity &&
            [json[@"positiveCallbacks"] unsignedLongLongValue] == valid &&
            [json[@"retainedCallbacks"] unsignedLongLongValue] == capacity &&
            [json[@"firstRetainedCallbackIndex"] unsignedLongLongValue] == valid-capacity &&
            [json[@"invalidCallbacks"] isEqual:@1] && [json[@"outOfOrderCallbacks"] isEqual:@1] &&
            [json[@"adjacentDuplicates"] isEqual:@1] && [json[@"truncated"] isEqual:@YES],
            "Wrapped trace lost its lifetime counters or omitted prefix");
    NSArray* seconds = json[@"callbackOrderSeconds"];
    require(seconds.count == capacity, "Unbounded or incomplete retained sequence");
    for (std::size_t i = 0; i < seconds.count; ++i) {
        require([seconds[i] doubleValue] == time_at(valid-capacity+i), "Incorrect callback order across wrap");
    }
}

} // namespace

int main() {
    @autoreleasepool {
        auto* files = NSFileManager.defaultManager;
        NSString* dir = [files.temporaryDirectory.path stringByAppendingPathComponent:NSUUID.UUID.UUIDString];
        if (![files createDirectoryAtPath:dir withIntermediateDirectories:NO attributes:nil error:nil]) return 1;
        int status = 0;
        try {
            optional_exit_only([dir stringByAppendingPathComponent:@"empty.json"]);
            complete_trace([dir stringByAppendingPathComponent:@"short.json"]);
            bounded_trace([dir stringByAppendingPathComponent:@"bounded.json"]);
            drawable_trace([dir stringByAppendingPathComponent:@"drawables.json"]);
            std::cout << "MAC_HOST_PRESENTED_TRACE_PASS capacity=32769 callbacks=65543 ordered=true"
                         " truncated=true exitOnly=true exclusiveCreate=true\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            status = 1;
        }
        [files removeItemAtPath:dir error:nil];
        return status;
    }
}

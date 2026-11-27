// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>

#include "presented_trace.h"
#include <cmath>
#include <cstring>

namespace coastal::macos_host {

bool write_presented_trace(const char* path, std::span<const PresentedFrame> ring,
                           const DisplayClockMetrics& metrics,
                           std::span<const DrawableTraceRow> drawables,
                           std::uint64_t total_drawables) {
    if (!path || path[0] != '/') return false;
    const auto total = metrics.valid_callbacks;
    if (ring.size() != std::min<std::uint64_t>(total, presentation_history_capacity) ||
        metrics.retained_callbacks != ring.size() ||
        drawables.size() != std::min<std::uint64_t>(total_drawables, presentation_history_capacity)) return false;

    @autoreleasepool {
        const auto omitted = total - ring.size();
        const auto first_slot = omitted ? total % presentation_history_capacity : 0;
        NSMutableArray<NSNumber*>* seconds = [NSMutableArray arrayWithCapacity:ring.size()];
        for (std::size_t i = 0; i < ring.size(); ++i) {
            [seconds addObject:@(ring[(first_slot + i) % ring.size()].time)];
        }
        NSMutableDictionary* report = [@{
            @"schema": @"coastal-presented-times.v1",
            @"clock": @"CAMetalDrawable.presentedTime",
            @"unit": @"host seconds",
            @"pid": @(NSProcessInfo.processInfo.processIdentifier),
            @"capacity": @(presentation_history_capacity),
            @"positiveCallbacks": @(total),
            @"invalidCallbacks": @(metrics.invalid_callbacks),
            @"outOfOrderCallbacks": @(metrics.out_of_order),
            @"adjacentDuplicates": @(metrics.adjacent_duplicates),
            @"retainedCallbacks": @(ring.size()),
            @"firstRetainedCallbackIndex": @(omitted),
            @"truncated": @(omitted != 0),
            @"callbackOrderSeconds": seconds,
        } mutableCopy];
        if (!drawables.empty()) {
            const auto omitted_drawables = total_drawables - drawables.size();
            const auto first_drawable_slot = omitted_drawables ? total_drawables % presentation_history_capacity : 0;
            NSMutableArray<NSArray<NSNumber*>*>* rows = [NSMutableArray arrayWithCapacity:drawables.size()];
            const auto time = [](double value) { return std::isfinite(value) && value > 0 ? value : 0.; };
            for (std::size_t i = 0; i < drawables.size(); ++i) {
                const auto& row = drawables[(first_drawable_slot + i) % drawables.size()];
                [rows addObject:@[@(row.commit), @(row.drawable_id),
                    @(time(row.request_begin)), @(time(row.request_return)),
                    @(time(row.commit_begin)), @(time(row.commit_return)),
                    @(time(row.scheduled_callback)), @(time(row.gpu_callback)),
                    @(time(row.gpu_start)), @(time(row.gpu_end)), @(row.gpu_completed),
                    @(time(row.presented_at)), @(time(row.presented_callback))]];
            }
            report[@"drawables"] = @{
                @"clock": @"Metal host seconds (Mach absolute time)",
                @"requestMode": @"immediate",
                @"submittedDrawables": @(total_drawables),
                @"firstRetainedSubmissionIndex": @(omitted_drawables),
                @"columns": @[@"commit", @"drawableID", @"presentRequestBegin", @"presentRequestReturn",
                    @"commitBegin", @"commitReturn", @"scheduledCallback", @"gpuCallback",
                    @"gpuStart", @"gpuEnd", @"gpuCompleted", @"presentedAt", @"presentedCallback"],
                @"rows": rows,
            };
        }
        NSString* destination = [NSFileManager.defaultManager stringWithFileSystemRepresentation:path
                                                                                           length:std::strlen(path)];
        NSData* bytes = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
        return bytes && [bytes writeToFile:destination options:NSDataWritingWithoutOverwriting error:nil];
    }
}

} // namespace coastal::macos_host

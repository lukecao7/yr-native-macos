// SPDX-License-Identifier: GPL-3.0-only
#import <Foundation/Foundation.h>

#include "presented_trace.h"
#include <cstring>

namespace coastal::macos_host {

bool write_presented_trace(const char* path, std::span<const PresentedFrame> ring,
                           const DisplayClockMetrics& metrics) {
    if (!path || path[0] != '/') return false;
    const auto total = metrics.valid_callbacks;
    if (ring.size() != std::min<std::uint64_t>(total, presentation_history_capacity) ||
        metrics.retained_callbacks != ring.size()) return false;

    @autoreleasepool {
        const auto omitted = total - ring.size();
        const auto first_slot = omitted ? total % presentation_history_capacity : 0;
        NSMutableArray<NSNumber*>* seconds = [NSMutableArray arrayWithCapacity:ring.size()];
        for (std::size_t i = 0; i < ring.size(); ++i) {
            [seconds addObject:@(ring[(first_slot + i) % ring.size()].time)];
        }
        NSDictionary* report = @{
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
        };
        NSString* destination = [NSFileManager.defaultManager stringWithFileSystemRepresentation:path
                                                                                           length:std::strlen(path)];
        NSData* bytes = [NSJSONSerialization dataWithJSONObject:report options:0 error:nil];
        return bytes && [bytes writeToFile:destination options:NSDataWritingWithoutOverwriting error:nil];
    }
}

} // namespace coastal::macos_host

// SPDX-License-Identifier: GPL-3.0-only
#include <cstdio>
#include <cstdlib>
#include <unistd.h>

// Exercise the actual asynchronous helper, not a second implementation.
#define main coastal_host_app_main_for_regression
#include "../native/macos_host/application.mm"
#undef main

int main() {
    alarm(5);
    @autoreleasepool {
        std::weak_ptr<DisplayPulseState> weak;
        {
            auto external = std::make_shared<DisplayPulseState>(
                nullptr, std::chrono::nanoseconds(16'666'667));
            weak = external;
            if (!post_main_signal(external, display_signal)) {
                std::fputs("Main signal was not queued.\n", stderr);
                return 1;
            }
            external.reset();
            // The main queue has not run. Its copied block must own the state;
            // capturing a const shared_ptr reference fails here deterministically.
            if (weak.expired()) {
                std::fputs("Queued main signal did not retain its state.\n", stderr);
                return 1;
            }
        }
        // Serial FIFO puts this after the real helper block. A nil target means
        // no application, window, display link, session or renderer is created.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (!weak.expired()) {
                std::fputs("Completed main signal retained its state.\n", stderr);
                std::exit(1);
            }
            alarm(0);
            std::puts("MAC_HOST_MAIN_SIGNAL_PASS queuedStateOwned=true releasedAfterDispatch=true");
            std::fflush(stdout);
            std::exit(0);
        });
        dispatch_main();
    }
}

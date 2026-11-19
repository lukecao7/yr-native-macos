// SPDX-License-Identifier: GPL-3.0-only
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include "frame_image.h"
#include "input_view.h"
#include "metal_presenter.h"

#include <algorithm>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace host = coastal::macos_host;

namespace {

void write_percentiles(const std::optional<host::Percentiles>& percentiles) {
    if (!percentiles) {
        std::cout << "null";
        return;
    }
    std::cout << "{\"samples\":" << percentiles->sample_count
              << ",\"p50Ns\":" << percentiles->p50.count()
              << ",\"p95Ns\":" << percentiles->p95.count()
              << ",\"p99Ns\":" << percentiles->p99.count() << '}';
}

void write_metrics(const host::FrameMetrics& metrics, host::Viewport viewport = {},
                   const std::optional<host::MetalMetrics>& metal = std::nullopt) {
    const auto peakRss = host::process_peak_rss_bytes();
    std::cout << "MAC_HOST_METRICS {\"presenter\":";
    if (metal) std::cout << "\"metal-shared\"";
    else if (viewport.width) std::cout << "\"core-graphics\"";
    else std::cout << "null";
    std::cout << ",\"viewportWidth\":" << viewport.width
              << ",\"viewportHeight\":" << viewport.height
              << ",\"presentedFrames\":" << (metal ? metal->displayed_frames : metrics.presented_frames)
              << ",\"submittedFrames\":" << metrics.presented_frames
              << ",\"fullOriginalFrames\":" << metrics.full_original_frames
              << ",\"activeObjects\":";
    if (metrics.active_objects) std::cout << *metrics.active_objects;
    else std::cout << "null";
    std::cout << ",\"firstHostFrameNs\":";
    if (metrics.first_present) std::cout << metrics.first_present->count();
    else std::cout << "null";
    std::cout << ",\"firstAppKitDrawNs\":";
    if (!metal && metrics.first_present) std::cout << metrics.first_present->count();
    else std::cout << "null";
    std::cout << ",\"frameWork\":";
    write_percentiles(metrics.frame_work);
    std::cout << ",\"presentationInterval\":";
    write_percentiles(metal ? metal->display_interval : metrics.presentation_interval);
    std::cout << ",\"submissionInterval\":";
    write_percentiles(metrics.presentation_interval);
    std::cout << ",\"peakRssBytes\":";
    if (peakRss) std::cout << *peakRss;
    else std::cout << "null";
    std::cout << '}' << std::endl;
}

int startup_error(const std::string& error) {
    std::cerr << "Coastal Mac Host: " << error << '\n';
    write_metrics({});
    return 2;
}

void install_quit_menu(NSApplication* application) {
    NSMenu* bar = [[NSMenu alloc] init];
    NSMenuItem* entry = [[NSMenuItem alloc] init];
    NSMenu* menu = [[NSMenu alloc] init];
    NSString* name = [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleName"];
    if (!name) name = NSProcessInfo.processInfo.processName;
    NSMenuItem* quit = [menu addItemWithTitle:[@"Quit " stringByAppendingString:name]
                                      action:@selector(terminate:) keyEquivalent:@"q"];
    quit.target = application;
    [bar addItem:entry];
    entry.submenu = menu;
    application.mainMenu = bar;
}

}  // namespace

@interface CoastalFrameView : CoastalInputView
- (std::optional<host::Frame>)advanceFrame;
- (BOOL)presentFrame:(const host::Frame&)frame;
- (std::optional<host::MetalMetrics>)finishPresentation;
@end

@implementation CoastalFrameView {
    host::Session* _session;
    host::Viewport _viewport;
    CoastalMetalPresenter* _metal;
    CGColorSpaceRef _colorSpace;
    CGImageRef _image;
    BOOL _drewImage;
}

- (instancetype)initWithFrame:(NSRect)rect session:(host::Session*)session {
    if ((self = [super initWithFrame:rect session:session])) {
        _session = session;
        _viewport = session->viewport();
        if (session->supports_shared_frames()) {
            _metal = [[CoastalMetalPresenter alloc] initWithViewport:_viewport];
        }
        if (_metal) {
            self.layer = _metal.layer;
            self.wantsLayer = YES;
        } else {
            _colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            if (!_colorSpace) return nil;
        }
    }
    return self;
}

- (void)dealloc {
    if (_image) CGImageRelease(_image);
    if (_colorSpace) CGColorSpaceRelease(_colorSpace);
}

- (BOOL)isOpaque { return YES; }

- (void)viewDidChangeBackingProperties {
    [super viewDidChangeBackingProperties];
    if (_metal) _metal.layer.contentsScale = self.window.backingScaleFactor;
}

- (std::optional<host::Frame>)advanceFrame {
    if (!_metal) return _session->advance_frame();
    const auto pixels = [_metal writablePixels];
    if (pixels.empty()) return std::nullopt;
    return _session->advance_frame_into(pixels);
}

- (BOOL)presentFrame:(const host::Frame&)frame {
    if (_metal) return [_metal presentFrame:frame];
    auto* image = host::create_rgba_frame_image(_viewport, frame, _colorSpace);
    if (!image) return NO;
    if (_image) CGImageRelease(_image);
    _image = image;
    _drewImage = NO;
    self.needsDisplay = YES;
    [self displayIfNeeded];
    return _drewImage;
}

- (std::optional<host::MetalMetrics>)finishPresentation {
    if (!_metal) return std::nullopt;
    [_metal finish];
    return [_metal metrics];
}

- (void)drawRect:(NSRect)dirtyRect {
    if (_metal) return;
    if (!_image) {
        [NSColor.blackColor setFill];
        NSRectFill(dirtyRect);
        return;
    }
    auto* context = NSGraphicsContext.currentContext.CGContext;
    if (!context) return;
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextDrawImage(context, NSRectToCGRect(self.bounds), _image);
    _drewImage = YES;
}

@end

@interface CoastalAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(host::Session*)session launchedAt:(host::FrameClock::time_point)launchedAt;
- (void)start;
- (void)detach;
- (int)status;
- (host::FrameMetrics)metrics;
- (std::optional<host::MetalMetrics>)finishPresentation;
@end

@implementation CoastalAppDelegate {
    host::Session* _session;
    std::unique_ptr<host::FrameLoop> _loop;
    NSWindow* _window;
    CoastalFrameView* _surface;
    NSTimer* _timer;
    BOOL _stopped;
    int _status;
}

- (instancetype)initWithSession:(host::Session*)session launchedAt:(host::FrameClock::time_point)launchedAt {
    if (!(self = [super init])) return nil;
    _session = session;
    _loop = std::make_unique<host::FrameLoop>(session->frame_interval(), launchedAt);
    const auto viewport = session->viewport();
    const CGFloat scale = NSScreen.mainScreen.backingScaleFactor;
    if (scale <= 0) return nil;
    const NSRect rect = NSMakeRect(0, 0, viewport.width / scale, viewport.height / scale);
    _surface = [[CoastalFrameView alloc] initWithFrame:rect session:session];
    if (!_surface) return nil;
    _window = [[NSWindow alloc] initWithContentRect:rect
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                  backing:NSBackingStoreBuffered defer:NO];
    _window.releasedWhenClosed = NO;
    _window.title = NSProcessInfo.processInfo.processName;
    _window.contentView = _surface;
    _window.acceptsMouseMovedEvents = YES;
    _window.delegate = self;
    [_window center];
    return self;
}

- (void)start {
    [_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [self scheduleFrame];
}

- (void)scheduleFrame {
    const auto wait = std::chrono::duration<double>(_loop->next_due() - host::FrameClock::now()).count();
    _timer = [NSTimer timerWithTimeInterval:std::max(0.000001, wait) target:self
                                   selector:@selector(driveFrame:) userInfo:nil repeats:NO];
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)driveFrame:(NSTimer*)timer {
    if (timer != _timer || _stopped) return;
    _timer = nil;
    @autoreleasepool {
        try {
            const auto result = _loop->tick([self]() -> std::optional<host::FrameCommit> {
                if (!_window.visible || _window.miniaturized) return std::nullopt;
                const auto frame = [_surface advanceFrame];
                if (!frame) return std::nullopt;
                if (!_loop->accepts_frame(frame->commit)) return frame->commit;
                if (![_surface presentFrame:*frame]) throw std::runtime_error("Could not present a session frame.");
                return frame->commit;
            });
            if (_stopped) return;
            if (result == host::FrameTick::InvalidFrame) {
                throw std::runtime_error("The session reported a nonconsecutive frame.");
            }
            [self scheduleFrame];
        } catch (const std::exception& error) {
            std::cerr << "Coastal Mac Host: " << error.what() << '\n';
            [self stopWithStatus:1];
        } catch (...) {
            std::cerr << "Coastal Mac Host: session frame failed.\n";
            [self stopWithStatus:1];
        }
    }
}

- (void)stopWithStatus:(int)status {
    if (_stopped) return;
    _stopped = YES;
    _status = status;
    [_timer invalidate];
    _timer = nil;
    _loop->stop();
    [NSApp stop:nil];
    // A stop request made outside event dispatch needs an event to wake -run.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                            modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [NSApp postEvent:wake atStart:NO];
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    [self stopWithStatus:0];
    return NSTerminateCancel;
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    [_window makeFirstResponder:_surface];
}

- (void)windowDidResignKey:(NSNotification*)notification { _session->input_focus_lost(); }

- (void)windowWillClose:(NSNotification*)notification { [self stopWithStatus:0]; }

- (void)detach {
    [_timer invalidate];
    _timer = nil;
    _loop->stop();
    _window.delegate = nil;
    [_window close];
    _window.contentView = nil;
    _surface = nil;
    _window = nil;
    _session = nullptr;
}

- (int)status { return _status; }
- (host::FrameMetrics)metrics { return _loop->metrics(); }
- (std::optional<host::MetalMetrics>)finishPresentation { return [_surface finishPresentation]; }
@end

int main(int argc, char** argv) {
    const auto launchedAt = host::FrameClock::now();
    @autoreleasepool {
        try {
            std::vector<std::string_view> arguments(argv + 1, argv + argc);
            std::string error;
            auto options = host::parse_launch_options(arguments, error);
            if (!options) return startup_error(error);
            if (options->help) {
                std::cout << "Usage: CoastalMacHost [--data-dir DIRECTORY] [--skirmish-map FILE.map] [--strict-audit]\n";
                return 0;
            }

            auto directory = host::resolve_data_directory(options->data_directory, error);
            if (!directory) return error.empty() ? 0 : startup_error(error);
            options->data_directory = std::move(*directory);
            auto session = host::create_session(*options, error);
            if (!session) return startup_error(error.empty() ? "The session runtime could not start." : error);
            const auto viewport = session->viewport();
            constexpr auto maxBytes = std::uint64_t(std::numeric_limits<CFIndex>::max());
            if (viewport.width == 0 || viewport.height == 0 ||
                maxBytes / (std::uint64_t(viewport.width) * 4) < viewport.height ||
                session->frame_interval() <= host::FrameClock::duration::zero()) {
                return startup_error("The session reported an invalid viewport or frame interval.");
            }

            NSApplication* application = NSApp;
            if (!application) {
                application = [NSApplication sharedApplication];
                [application setActivationPolicy:NSApplicationActivationPolicyRegular];
                [application finishLaunching];
            }
            install_quit_menu(application);
            auto* delegate = [[CoastalAppDelegate alloc] initWithSession:session.get() launchedAt:launchedAt];
            if (!delegate) return startup_error("Could not create a Mac window.");
            application.delegate = delegate;
            [delegate start];
            [application run];
            application.delegate = nil;
            const auto metal = [delegate finishPresentation];
            [delegate detach];
            const auto metrics = [delegate metrics];
            session.reset();
            write_metrics(metrics, viewport, metal);
            return [delegate status];
        } catch (const std::exception& error) {
            return startup_error(error.what());
        }
    }
}

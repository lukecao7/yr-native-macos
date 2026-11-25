// SPDX-License-Identifier: GPL-3.0-only
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CVDisplayLink.h>
#import <dispatch/dispatch.h>
#include <mach/mach_time.h>

#include "data_directory.h"
#include "frame_image.h"
#include "initial_window.h"
#include "input_view.h"
#include "metal_presenter.h"

#ifdef COASTAL_FIXTURE_REPLAY
#include "script_replay.h"
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
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
              << ",\"displayed3dFrames\":" << (metal ? metal->displayed_scene_frames : 0)
              << ",\"displayedModeSwitches\":" << (metal ? metal->mode_switches : 0)
              << ",\"maxSwitchIntervalNs\":" << (metal ? metal->max_switch_interval.count() : 0)
              << ",\"fullOriginalFrames\":" << (metal ? metal->displayed_original_frames : metrics.full_original_frames)
              << ",\"submittedFullOriginalFrames\":" << metrics.full_original_frames
              << ",\"lastOriginalActiveObjects\":";
    const auto original_objects = metal ? metal->last_original_active_objects : metrics.last_original_active_objects;
    if (original_objects) std::cout << *original_objects;
    else std::cout << "null";
    std::cout
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
    std::cout << ",\"display3dInterval\":";
    write_percentiles(metal ? metal->scene_interval : std::nullopt);
    std::cout << ",\"drawableAcquire\":";
    write_percentiles(metal ? metal->drawable_acquire : std::nullopt);
    std::cout << ",\"encodeAndSubmit\":";
    write_percentiles(metal ? metal->encode_submit : std::nullopt);
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
- (void)resetFrameElapsed;
- (host::FramePresentation)presentFrame:(const host::Frame&)frame;
- (std::optional<host::MetalMetrics>)finishPresentation;
@end

@implementation CoastalFrameView {
    host::Session* _session;
    host::Viewport _viewport;
    CoastalMetalPresenter* _metal;
    CGColorSpaceRef _colorSpace;
    CGImageRef _image;
    host::FrameElapsedTime _elapsed;
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
            const auto terrain = session->terrain_for_metal();
            if (terrain) session->scene_available([_metal enableScene:terrain] == YES);
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
    if (!_metal) {
        _session->set_frame_elapsed(_elapsed.sample());
        return _session->advance_frame();
    }
    const auto pixels = [_metal writablePixels];
    if (pixels.empty()) return std::nullopt;
    _session->set_frame_elapsed(_elapsed.sample());
    return _session->advance_frame_into(pixels);
}

- (void)resetFrameElapsed {
    _elapsed.reset();
    _session->reset_frame_elapsed();
}

- (host::FramePresentation)presentFrame:(const host::Frame&)frame {
    if (_metal) return [_metal presentFrame:frame];
    auto* image = host::create_rgba_frame_image(_viewport, frame, _colorSpace);
    if (!image) return host::FramePresentation::Invalid;
    if (_image) CGImageRelease(_image);
    _image = image;
    _drewImage = NO;
    self.needsDisplay = YES;
    [self displayIfNeeded];
    return _drewImage ? host::FramePresentation::Presented : host::FramePresentation::Invalid;
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

namespace {

void stop_application(NSApplication* application) {
    [application stop:nil];
    // A stop request outside event dispatch needs an event to wake -run.
    NSEvent* wake = [NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint
                            modifierFlags:0 timestamp:0 windowNumber:0 context:nil subtype:0 data1:0 data2:0];
    [application postEvent:wake atStart:NO];
}

std::unique_ptr<host::Session> open_session(const host::LaunchOptions& options, std::string& error) {
    std::unique_ptr<host::Session> session;
    try {
        session = host::create_session(options, error);
    } catch (const std::filesystem::filesystem_error& failure) {
        error = failure.what();
        return nullptr;
    }
    if (!session) {
        if (error.empty()) error = "The session runtime could not start.";
        return nullptr;
    }
    const auto viewport = session->viewport();
    constexpr auto maxBytes = std::uint64_t(std::numeric_limits<CFIndex>::max());
    if (viewport.width == 0 || viewport.height == 0 ||
        maxBytes / (std::uint64_t(viewport.width) * 4) < viewport.height ||
        session->frame_interval() <= host::FrameClock::duration::zero()) {
        error = "The session reported an invalid viewport or frame interval.";
        return nullptr;
    }
    return session;
}

std::unique_ptr<host::Session> open_launch_session(host::LaunchOptions& options, std::string& error) {
    if (!options.stress || !options.data_directory.empty()) {
        auto directory = host::resolve_data_directory(options.data_directory, error);
        if (!directory) return nullptr;
        options.data_directory = std::move(*directory);
    }
    return open_session(options, error);
}

}  // namespace

@interface CoastalStartupDelegate : NSObject <NSApplicationDelegate>
- (instancetype)initWithAttempt:(std::function<bool()>)attempt;
- (void)scheduleAttempt:(NSTimeInterval)delay;
- (void)startIfActive;
@end

@implementation CoastalStartupDelegate {
    std::function<bool()> _attempt;
    BOOL _starting;
    BOOL _finished;
}

- (instancetype)initWithAttempt:(std::function<bool()>)attempt {
    if ((self = [super init])) _attempt = std::move(attempt);
    return self;
}

- (void)scheduleAttempt:(NSTimeInterval)delay {
    if (_starting || _finished) return;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(startIfActive) object:nil];
    [self performSelector:@selector(startIfActive) withObject:nil afterDelay:delay];
}

- (void)startIfActive {
    const auto action = host::interactive_ui_action(!_starting && !_finished);
    if (action == host::InitialWindowAction::Wait) return;
    if (action == host::InitialWindowAction::Retry) {
        // AppKit can report activation before the workspace changes its frontmost app.
        [self scheduleAttempt:0.05];
        return;
    }
    _starting = YES;
    _finished = _attempt() || _finished;
    _starting = NO;
    if (!_finished) [self scheduleAttempt:0];
}

- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    [self scheduleAttempt:0];
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    [self scheduleAttempt:0];
}

- (BOOL)applicationShouldHandleReopen:(NSApplication*)sender hasVisibleWindows:(BOOL)visible {
    (void)sender;
    (void)visible;
    [self scheduleAttempt:0];
    return NO;
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    _finished = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(startIfActive) object:nil];
    if (sender.modalWindow) [sender abortModal];
    stop_application(sender);
    return NSTerminateCancel;
}

@end

@interface CoastalAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
- (instancetype)initWithSession:(host::Session*)session
                      launchedAt:(host::FrameClock::time_point)launchedAt
                    stressWindow:(BOOL)stressWindow;
- (void)start;
- (void)makeInitialWindowKeyIfActive;
- (void)detach;
- (BOOL)startDisplayLink;
- (void)stopDisplayLink;
- (void)driveDisplayPulse:(std::uint64_t)outputHostTime;
- (void)frameReadyForDisplay:(std::uint64_t)outputHostTime;
- (void)renderFrameForDisplay:(std::uint64_t)outputHostTime;
- (void)writeSchedulingMetrics;
- (int)status;
- (host::FrameMetrics)metrics;
- (std::optional<host::MetalMetrics>)finishPresentation;
@end

namespace {

constexpr unsigned display_signal = 1, frame_ready_signal = 2;

struct DisplayPulseState {
    DisplayPulseState(CoastalAppDelegate* owner, host::FrameClock::duration interval)
        : target(owner), cadence(interval), fallback_period(interval) {}

    __weak CoastalAppDelegate* target;
    host::DisplayCadence cadence;
    const host::FrameClock::duration fallback_period;
    std::atomic<bool> active{true};
    std::atomic<unsigned> pending_signals{0};
    std::atomic<std::uint64_t> output_host_time{0};
    std::atomic<std::uint64_t> callbacks{0}, eligible_pulses{0}, coalesced_pulses{0};
    std::atomic<std::uint64_t> ready_notifications{0}, coalesced_ready{0};
};

// The dispatched block outlives this call: capture an owning value, never a
// reference to the callback's temporary shared_ptr argument.
bool post_main_signal(std::shared_ptr<DisplayPulseState> state,unsigned signal) {
    if (!state->active.load(std::memory_order_acquire)) return false;
    if (state->pending_signals.fetch_or(signal,std::memory_order_acq_rel)) return false;
    dispatch_async(dispatch_get_main_queue(), ^{
        const auto signals=state->pending_signals.exchange(0,std::memory_order_acq_rel);
        if (!state->active.load(std::memory_order_acquire)) return;
        CoastalAppDelegate* target=state->target;
        const auto output=state->output_host_time.load(std::memory_order_acquire);
        // A frame-ready hint never creates a new refresh pulse. If both reasons
        // merged, the real display pulse makes the single first attempt.
        if (signals&display_signal) [target driveDisplayPulse:output];
        else if (signals&frame_ready_signal) [target frameReadyForDisplay:output];
    });
    return true;
}

}  // namespace

@implementation CoastalAppDelegate {
    host::Session* _session;
    std::unique_ptr<host::FrameLoop> _loop;
    host::FrameDelivery _delivery;
    NSWindow* _window;
    CoastalFrameView* _surface;
    NSTimer* _timer;
    CVDisplayLinkRef _displayLink;
    std::shared_ptr<DisplayPulseState> _displayState;
    CGDirectDisplayID _displayID;
    double _hostSecondsPerTick;
    std::uint64_t _retryOutputTime;
    std::uint64_t _currentDisplayOutput;
    std::uint64_t _mainPulses, _readinessRetries, _emptyAttempts;
    std::uint64_t _readyWakeups, _ignoredReadySignals, _fallbackRetries;
    BOOL _usesReadyNotifications;
    BOOL _usedDisplayLink, _timerFallback;
    BOOL _stopped;
    BOOL _initialWindowNeedsKey;
    int _status;
}

- (instancetype)initWithSession:(host::Session*)session
                      launchedAt:(host::FrameClock::time_point)launchedAt
                    stressWindow:(BOOL)stressWindow {
    if (!(self = [super init])) return nil;
    _session = session;
    mach_timebase_info_data_t timebase{};
    if (mach_timebase_info(&timebase) != KERN_SUCCESS || !timebase.denom) return nil;
    _hostSecondsPerTick = double(timebase.numer) / double(timebase.denom) / 1'000'000'000.0;
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
    // The engine restores games through its save files; no AppKit window archive is used.
    _window.restorable = NO;
    _window.title = NSProcessInfo.processInfo.processName;
    _window.contentView = _surface;
    _window.acceptsMouseMovedEvents = YES;
    _window.delegate = self;
    if (stressWindow) {
        const NSRect available = NSScreen.mainScreen.visibleFrame;
        _window.level = NSFloatingWindowLevel;
        _window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                     NSWindowCollectionBehaviorFullScreenAuxiliary;
        [_window setFrameTopLeftPoint:NSMakePoint(NSMaxX(available) - NSWidth(_window.frame) - 16,
                                                  NSMaxY(available) - 16)];
    } else {
        [_window center];
    }
    return self;
}

- (void)start {
    _initialWindowNeedsKey = YES;
    [self makeInitialWindowKeyIfActive];
}

- (void)makeInitialWindowKeyIfActive {
    const auto action = host::interactive_ui_action(_initialWindowNeedsKey && !_stopped);
    if (action == host::InitialWindowAction::Wait) return;

    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:_cmd object:nil];
    if (action == host::InitialWindowAction::Retry) {
        // AppKit can notify us before the workspace changes its frontmost app.
        [self performSelector:_cmd withObject:nil afterDelay:0.05];
        return;
    }
    _initialWindowNeedsKey = NO;
    [_window makeKeyAndOrderFront:nil];
    if (![self startDisplayLink]) {
        _timerFallback = YES;
        [self scheduleFrame];
    }
}

- (void)applicationDidBecomeActive:(NSNotification*)notification {
    (void)notification;
    [self makeInitialWindowKeyIfActive];
}

- (BOOL)startDisplayLink {
    NSNumber* number = _window.screen.deviceDescription[@"NSScreenNumber"];
    _displayID = number ? number.unsignedIntValue : CGMainDisplayID();
    if (CVDisplayLinkCreateWithActiveCGDisplays(&_displayLink) != kCVReturnSuccess) return NO;
    if (CVDisplayLinkSetCurrentCGDisplay(_displayLink, _displayID) != kCVReturnSuccess) {
        [self stopDisplayLink];
        return NO;
    }
    _displayState = std::make_shared<DisplayPulseState>(self, _session->frame_interval());
    const auto state = _displayState;
    const auto result = CVDisplayLinkSetOutputHandler(_displayLink,
        ^CVReturn(CVDisplayLinkRef link, const CVTimeStamp* now, const CVTimeStamp* output,
                  CVOptionFlags flagsIn, CVOptionFlags* flagsOut) {
            (void)link; (void)now; (void)flagsIn; (void)flagsOut;
            if (!state->active.load(std::memory_order_acquire)) return kCVReturnSuccess;
            state->callbacks.fetch_add(1, std::memory_order_relaxed);
            auto refresh = state->fallback_period;
            if ((output->flags & kCVTimeStampVideoRefreshPeriodValid) &&
                output->videoRefreshPeriod > 0 && output->videoTimeScale > 0) {
                const auto seconds = double(output->videoRefreshPeriod) / output->videoTimeScale;
                if (std::isfinite(seconds) && seconds > 0 && seconds <= 1) {
                    refresh = std::chrono::duration_cast<host::FrameClock::duration>(
                        std::chrono::duration<double>(seconds));
                }
            }
            if (!state->cadence.accepts_refresh(refresh)) return kCVReturnSuccess;
            state->eligible_pulses.fetch_add(1, std::memory_order_relaxed);
            // CV hostTime and mach_absolute_time use the same clock. Without
            // a valid future timestamp the attempt simply has no retry budget.
            const auto outputTime = (output->flags & kCVTimeStampHostTimeValid) && output->hostTime
                ? output->hostTime : mach_absolute_time();
            state->output_host_time.store(outputTime, std::memory_order_release);
            if (!post_main_signal(state,display_signal)) {
                state->coalesced_pulses.fetch_add(1, std::memory_order_relaxed);
                return kCVReturnSuccess;
            }
            return kCVReturnSuccess;
        });
    _usesReadyNotifications=_session->supports_frame_ready_notifications();
    if (_usesReadyNotifications) {
        _session->set_frame_ready_handler([weak=std::weak_ptr<DisplayPulseState>(state)] {
            if (const auto signal=weak.lock();signal&&signal->active.load(std::memory_order_acquire)) {
                signal->ready_notifications.fetch_add(1,std::memory_order_relaxed);
                if(!post_main_signal(signal,frame_ready_signal))signal->coalesced_ready.fetch_add(1,std::memory_order_relaxed);
            }
        });
    }
    if (result != kCVReturnSuccess || CVDisplayLinkStart(_displayLink) != kCVReturnSuccess) {
        [self stopDisplayLink];
        return NO;
    }
    _usedDisplayLink = YES;
    return YES;
}

- (void)stopDisplayLink {
    if (_displayState) _displayState->active.store(false, std::memory_order_release);
    if (_session) _session->set_frame_ready_handler({});
    if (_displayLink) {
        CVDisplayLinkStop(_displayLink);
        CVDisplayLinkRelease(_displayLink);
        _displayLink = nullptr;
    }
}

- (void)dealloc {
    [self stopDisplayLink];
}

- (void)driveDisplayPulse:(std::uint64_t)outputHostTime {
    if (_stopped || !_displayLink) return;
    ++_mainPulses;
    if (outputHostTime<=_currentDisplayOutput) return;
    _currentDisplayOutput=outputHostTime;
    [_timer invalidate];
    _timer = nil;
    [self renderFrameForDisplay:outputHostTime];
}

- (void)frameReadyForDisplay:(std::uint64_t)outputHostTime {
    if (_stopped || !_displayLink) return;
    ++_readyWakeups;
    const auto now=mach_absolute_time();
    const auto remaining=outputHostTime>now?(outputHostTime-now)*_hostSecondsPerTick:0;
    if(outputHostTime!=_currentDisplayOutput||remaining<=0.001||!_loop->can_retry_display(outputHostTime)) {
        ++_ignoredReadySignals;
        return;
    }
    ++_readinessRetries;
    [self renderFrameForDisplay:outputHostTime];
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
    if (_displayLink) {
        const auto now=mach_absolute_time();
        if(_retryOutputTime!=_currentDisplayOutput||_retryOutputTime<=now||
           (_retryOutputTime-now)*_hostSecondsPerTick<=0.001||!_loop->can_retry_display(_retryOutputTime))return;
        ++_readinessRetries;++_fallbackRetries;
    }
    [self renderFrameForDisplay:_displayLink ? _retryOutputTime : 0];
}

- (void)renderFrameForDisplay:(std::uint64_t)outputHostTime {
    @autoreleasepool {
        try {
            const auto step = [self]() -> std::optional<host::FrameCommit> {
                if (!_window.visible || _window.miniaturized) return std::nullopt;
                return _delivery.step(*_loop,
                    [self] { return [_surface advanceFrame]; },
                    [self](const host::Frame& frame) { return [_surface presentFrame:frame]; });
            };
            const auto result = outputHostTime ? _loop->display_tick(outputHostTime, step) : _loop->tick(step);
            if (_stopped) return;
            if (result == host::FrameTick::InvalidFrame) {
                throw std::runtime_error("The session reported a nonconsecutive frame.");
            }
            if (result == host::FrameTick::WaitingForFrame) ++_emptyAttempts;
            if (!_displayLink) {
                [self scheduleFrame];
            } else if (!_usesReadyNotifications && result == host::FrameTick::WaitingForFrame &&
                       _loop->can_retry_display(outputHostTime) &&
                       _window.visible && !_window.miniaturized) {
                const auto now = mach_absolute_time();
                const auto remaining = outputHostTime > now ? (outputHostTime - now) * _hostSecondsPerTick : 0;
                // A synchronous Session without notifications gets one bounded
                // retry. FrameLoop consumes the budget even if it is still empty.
                if (remaining > 0.0015) {
                    _retryOutputTime = outputHostTime;
                    _timer = [NSTimer timerWithTimeInterval:std::min(0.001, remaining - 0.001)
                                                    target:self selector:@selector(driveFrame:)
                                                  userInfo:nil repeats:NO];
                    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
                }
            }
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
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(makeInitialWindowKeyIfActive) object:nil];
    [_timer invalidate];
    _timer = nil;
    [self stopDisplayLink];
    _loop->stop();
    stop_application(NSApp);
}

- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
    [self stopWithStatus:0];
    return NSTerminateCancel;
}

- (void)windowDidBecomeKey:(NSNotification*)notification {
    [_window makeFirstResponder:_surface];
}

- (void)windowDidResignKey:(NSNotification*)notification { _session->input_focus_lost(); }

- (void)windowDidDeminiaturize:(NSNotification*)notification { [_surface resetFrameElapsed]; }
- (void)applicationDidUnhide:(NSNotification*)notification { [_surface resetFrameElapsed]; }

- (void)windowWillClose:(NSNotification*)notification { [self stopWithStatus:0]; }

- (void)windowDidChangeScreen:(NSNotification*)notification {
    (void)notification;
    if (!_displayLink || _stopped) return;
    NSNumber* number = _window.screen.deviceDescription[@"NSScreenNumber"];
    const auto display = number ? number.unsignedIntValue : CGMainDisplayID();
    if (display == _displayID) return;
    if (CVDisplayLinkSetCurrentCGDisplay(_displayLink, display) == kCVReturnSuccess) {
        _displayID = display;
    } else {
        [self stopDisplayLink];
        _timerFallback = YES;
        [_timer invalidate];
        [self scheduleFrame];
    }
}

- (void)writeSchedulingMetrics {
    std::cout << "MAC_HOST_SCHEDULER {\"source\":\""
              << (_timerFallback ? "timer-fallback" : (_usedDisplayLink ? "cv-display-link" : "timer"))
              << "\",\"displayId\":" << _displayID
              << ",\"requestedIntervalNs\":" << std::chrono::duration_cast<std::chrono::nanoseconds>(_session->frame_interval()).count()
              << ",\"displayCallbacks\":" << (_displayState ? _displayState->callbacks.load() : 0)
              << ",\"eligiblePulses\":" << (_displayState ? _displayState->eligible_pulses.load() : 0)
              << ",\"coalescedPulses\":" << (_displayState ? _displayState->coalesced_pulses.load() : 0)
              << ",\"mainPulses\":" << _mainPulses
              << ",\"frameReadyNotifications\":" << (_usesReadyNotifications?"true":"false")
              << ",\"readyNotifications\":" << (_displayState ? _displayState->ready_notifications.load() : 0)
              << ",\"coalescedReady\":" << (_displayState ? _displayState->coalesced_ready.load() : 0)
              << ",\"readyWakeups\":" << _readyWakeups
              << ",\"ignoredReadySignals\":" << _ignoredReadySignals
              << ",\"fallbackRetries\":" << _fallbackRetries
              << ",\"readinessRetries\":" << _readinessRetries
              << ",\"emptyAttempts\":" << _emptyAttempts << "}\n";
}

- (void)detach {
    _stopped = YES;
    [NSObject cancelPreviousPerformRequestsWithTarget:self selector:@selector(makeInitialWindowKeyIfActive) object:nil];
    [_timer invalidate];
    _timer = nil;
    [self stopDisplayLink];
    _loop->stop();
    _window.delegate = nil;
    _delivery.clear();
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
#ifdef COASTAL_FIXTURE_REPLAY
            auto options = host::parse_launch_options(arguments, error, host::FixtureReplay::Enabled);
#else
            auto options = host::parse_launch_options(arguments, error);
#endif
            if (!options) return startup_error(error);
            if (options->help) {
                std::cout << "Usage: CoastalMacHost [--data-dir DIRECTORY] [--skirmish-map FILE.map] [--strict-audit] [--threads=1|4|8] [--stress [--cycle-views]] [--adaptive-ai]\n";
#ifdef COASTAL_FIXTURE_REPLAY
                std::cout << "Windowless fixture replay: --script-file FILE|- --script-output NEW_DIRECTORY "
                             "with an explicit --data-dir.\n";
#endif
                return 0;
            }

#ifdef COASTAL_FIXTURE_REPLAY
            if (!options->script_file.empty()) {
                auto directory = host::validate_data_directory(options->data_directory, error);
                if (!directory) return startup_error(error);
                options->data_directory = std::move(*directory);
                auto session = open_session(*options, error);
                if (!session) return startup_error(error);
                if (NSApp) return startup_error("AppKit was initialized before windowless fixture replay.");
                return host::replay_script(*options, *session);
            }
#endif

            std::unique_ptr<host::Session> session;
            if (!options->data_directory.empty() || options->stress) {
                session = open_launch_session(*options, error);
                if (!session) return startup_error(error);
            }

            NSApplication* application = [NSApplication sharedApplication];
            [application setActivationPolicy:NSApplicationActivationPolicyRegular];
            install_quit_menu(application);
            CoastalAppDelegate* delegate = nil;
            id<NSObject> stressActivity = nil;
            int status = 0;
            bool triedSavedDirectory = false;
            bool retrySelection = false;
            const auto attemptStartup = [&]() {
                try {
                    if (!session && !triedSavedDirectory) {
                        triedSavedDirectory = true;
                        auto configured = *options;
                        session = open_launch_session(configured, error);
                        retrySelection = !session && !configured.data_directory.empty();
                        if (retrySelection) startup_error(error);
                    }
                    if (!session) {
                        const auto foreground = [] {
                            return host::interactive_ui_action(true) == host::InitialWindowAction::Show;
                        };
                        if (!foreground()) return false;
                        const auto selection = host::select_data_directory(
                            [&](auto& pickerError) { return host::choose_data_directory(retrySelection, pickerError); },
                            [&](const auto& directory, auto& validationError) {
                                auto configured = *options;
                                configured.data_directory = directory;
                                session = open_launch_session(configured, validationError);
                                return bool(session);
                            }, error);
                        if (selection == host::DirectorySelection::Rejected) {
                            startup_error(error);
                            retrySelection = true;
                            return false;
                        }
                        if (selection != host::DirectorySelection::Accepted) {
                            if (selection == host::DirectorySelection::Unavailable) {
                                if (!foreground()) return false;
                                status = startup_error(error);
                            }
                            stop_application(application);
                            return true;
                        }
                    }
                    delegate = [[CoastalAppDelegate alloc] initWithSession:session.get()
                                                               launchedAt:launchedAt
                                                             stressWindow:options->stress];
                    if (delegate) {
                        application.delegate = delegate;
                        if (options->stress) {
                            stressActivity = [NSProcessInfo.processInfo
                                beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep
                                reason:@"CoastalHost display benchmark"];
                        }
                        [delegate start];
                        return true;
                    }
                    status = startup_error("Could not create a Mac window.");
                } catch (const std::exception& failure) {
                    status = startup_error(failure.what());
                }
                stop_application(application);
                return true;
            };
            __attribute__((objc_precise_lifetime)) CoastalStartupDelegate* startup =
                [[CoastalStartupDelegate alloc] initWithAttempt:attemptStartup];
            application.delegate = startup;
            [startup scheduleAttempt:0];
            [application run];
            if (stressActivity) [NSProcessInfo.processInfo endActivity:stressActivity];
            application.delegate = nil;
            if (!delegate) return status;
            const auto viewport = session->viewport();
            const auto metal = [delegate finishPresentation];
            [delegate writeSchedulingMetrics];
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

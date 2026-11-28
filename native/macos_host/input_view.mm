// SPDX-License-Identifier: GPL-3.0-only
#import "input_view.h"
#include <algorithm>
#include <cmath>

namespace host = coastal::macos_host;

namespace {

std::string_view utf8_view(NSData* bytes) {
    if (!bytes.length) return {};
    return {static_cast<const char*>(bytes.bytes), bytes.length};
}

std::uint32_t button_number(NSEvent* event) {
    switch (event.type) {
    case NSEventTypeLeftMouseDown:
    case NSEventTypeLeftMouseDragged:
    case NSEventTypeLeftMouseUp:
        return 0;
    case NSEventTypeRightMouseDown:
    case NSEventTypeRightMouseDragged:
    case NSEventTypeRightMouseUp:
        return 1;
    default:
        return static_cast<std::uint32_t>(event.buttonNumber);
    }
}

constexpr NSEventMask pointer_motion = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
    NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged;

NSPoint clamp_inside(NSPoint point, NSRect rect) noexcept {
    // Keep the system hotspot clear of title bars, other displays and the
    // system edge. Even at the minimum window size this sits in the scroll band.
    const CGFloat pad = std::min({2.0, NSWidth(rect) / 4, NSHeight(rect) / 4});
    return NSMakePoint(std::clamp(point.x, NSMinX(rect) + pad, NSMaxX(rect) - pad),
                       std::clamp(point.y, NSMinY(rect) + pad, NSMaxY(rect) - pad));
}

NSPoint quartz_point(NSPoint point) noexcept {
    // AppKit's primary screen starts at its lower-left corner. Quartz uses
    // the upper-left corner of that same screen, including across displays.
    const auto primary = NSScreen.screens.firstObject.frame;
    return NSMakePoint(point.x - NSMinX(primary), NSMaxY(primary) - point.y);
}

}  // namespace

@implementation CoastalInputView {
    host::Session* _session;
    NSTrackingArea* _pointerTracking;
    BOOL _softwareCursor;
    BOOL _gameInputSuspended;
    BOOL _confinementActive;
    std::uint64_t _pressedButtons;
    id _localPointerMonitor;
    id _globalPointerMonitor;
}

- (instancetype)initWithFrame:(NSRect)rect session:(host::Session*)session {
    if (!session) return nil;
    if ((self = [super initWithFrame:rect])) {
        _session = session;
    }
    return self;
}

- (void)dealloc {
    if (_localPointerMonitor) [NSEvent removeMonitor:_localPointerMonitor];
    if (_globalPointerMonitor) [NSEvent removeMonitor:_globalPointerMonitor];
}

- (BOOL)acceptsFirstResponder { return YES; }

- (void)setGameInputSuspended:(BOOL)suspended {
    if (_gameInputSuspended == suspended) return;
    _gameInputSuspended = suspended;
    if (suspended) _pressedButtons = 0;
    [self syncFrameCursor];
}

- (BOOL)pointerConfinementActive { return _confinementActive; }

- (NSRect)pointerBoundsOnScreen {
    if (!self.window) return NSZeroRect;
    return [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
}

- (BOOL)moveSystemPointerTo:(NSPoint)point {
    if (!self.window.screen) return NO;
    const auto target = quartz_point(point);
    if (CGWarpMouseCursorPosition(CGPointMake(target.x, target.y)) != kCGErrorSuccess) return NO;
    // Reassert the association after a warp to avoid delaying hardware input,
    // as SDL's Cocoa backend does.
    CGAssociateMouseAndMouseCursorPosition(true);
    return YES;
}

- (void)setPointerConfinementEnabled:(BOOL)enabled {
    if (!enabled) {
        _confinementActive = NO;
        if (_localPointerMonitor) [NSEvent removeMonitor:_localPointerMonitor];
        if (_globalPointerMonitor) [NSEvent removeMonitor:_globalPointerMonitor];
        _localPointerMonitor = nil;
        _globalPointerMonitor = nil;
        return;
    }
    if (_confinementActive || NSEvent.pressedMouseButtons) return;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    _confinementActive = YES;
    const auto pointer = NSEvent.mouseLocation;
    if (!NSPointInRect(pointer, bounds))
        [self moveSystemPointerTo:NSMakePoint(NSMidX(bounds), NSMidY(bounds))];

    // AppKit dispatches motion in the surface normally. Deliver a move in the
    // letterbox once here because the canvas itself cannot forward it.
    __weak CoastalInputView* view = self;
    _localPointerMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:pointer_motion
        handler:^NSEvent*(NSEvent* event) {
            CoastalInputView* owner = view;
            return owner ? [owner handleLocalPointerMotion:event] : event;
        }];
    _globalPointerMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:pointer_motion
        handler:^(NSEvent*) { [view updateConstrainedPointer]; }];
}

- (BOOL)canConstrainPointer {
    return _confinementActive && !_gameInputSuspended && NSApp.isActive &&
        !NSApp.modalWindow && self.window.isKeyWindow;
}

- (NSEvent*)handleLocalPointerMotion:(NSEvent*)event {
    if (![self canConstrainPointer]) return event;
    if (event.window && event.window != self.window) return event;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return event;
    const auto systemPoint = NSEvent.mouseLocation;
    if (!NSPointInRect(systemPoint, bounds))
        [self moveSystemPointerTo:clamp_inside(systemPoint, bounds)];
    if (event.type != NSEventTypeMouseMoved) return event;

    const auto location = [self eventLocationInWindow:event];
    if (event.window == self.window &&
        NSPointInRect([self convertPoint:location fromView:nil], self.bounds)) return event;
    [self forwardPointer:event phase:host::PointerEvent::Phase::Move];
    return nil;
}

- (void)updateConstrainedPointer {
    if (![self canConstrainPointer]) return;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    const auto pointer = NSEvent.mouseLocation;
    if (NSPointInRect(pointer, bounds)) return;
    const auto next = clamp_inside(pointer, bounds);
    if (![self moveSystemPointerTo:next]) return;
    // A Quartz warp does not dispatch a mouse event. Publish the edge explicitly
    // when the original movement went outside the event stream of our window.
    [self sendPointerAtScreenPosition:next];
}

- (void)syncFrameCursor {
    const BOOL next = !_gameInputSuspended && _session && _session->has_software_cursor();
    if (next == _softwareCursor) return;
    _softwareCursor = next;
    [self.window invalidateCursorRectsForView:self];
}

- (void)resetCursorRects {
    [super resetCursorRects];
    _softwareCursor = !_gameInputSuspended && _session && _session->has_software_cursor();
    if (!_softwareCursor) return;
    static NSCursor* transparent;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSImage* blank=[NSImage imageWithSize:NSMakeSize(16,16) flipped:NO drawingHandler:^BOOL(NSRect rect) {
            [[NSColor clearColor] setFill];
            NSRectFillUsingOperation(rect,NSCompositingOperationCopy);
            return YES;
        }];
        transparent=[[NSCursor alloc] initWithImage:blank hotSpot:NSZeroPoint];
    });
    [self addCursorRect:self.visibleRect cursor:transparent];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_pointerTracking) [self removeTrackingArea:_pointerTracking];
    // The window already delivers movement to the first responder. A movement
    // tracking area delivers the same physical event to this view a second time.
    _pointerTracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingInVisibleRect | NSTrackingActiveInKeyWindow |
                NSTrackingMouseEnteredAndExited | NSTrackingEnabledDuringMouseDrag
        owner:self userInfo:nil];
    [self addTrackingArea:_pointerTracking];
}

- (BOOL)viewportPointForLocation:(NSPoint)location point:(NSPoint*)position {
    const NSRect bounds = self.bounds;
    if (NSIsEmptyRect(bounds)) return NO;

    const NSPoint point = [self convertPoint:location fromView:nil];
    const auto viewport = _session->viewport();
    const CGFloat y = self.isFlipped ? point.y - NSMinY(bounds) : NSMaxY(bounds) - point.y;
    *position = NSMakePoint((point.x - NSMinX(bounds)) * viewport.width / NSWidth(bounds),
                            y * viewport.height / NSHeight(bounds));
    return YES;
}

- (NSPoint)eventLocationInWindow:(NSEvent*)event {
    const auto location = event.locationInWindow;
    if (!self.window || event.window == self.window) return location;
    const auto screen = event.window ? [event.window convertPointToScreen:location] : location;
    return [self.window convertPointFromScreen:screen];
}

- (BOOL)viewportPointForEvent:(NSEvent*)event point:(NSPoint*)position {
    auto location = [self eventLocationInWindow:event];
    if (_confinementActive) {
        const auto bounds = [self pointerBoundsOnScreen];
        if (NSIsEmptyRect(bounds)) return NO;
        const auto screen = [self.window convertPointToScreen:location];
        if (!NSPointInRect(screen, bounds))
            location = [self.window convertPointFromScreen:clamp_inside(screen, bounds)];
    }
    return [self viewportPointForLocation:location point:position];
}

- (void)sendPointerAtScreenPosition:(NSPoint)screen {
    NSPoint point;
    if (![self viewportPointForLocation:[self.window convertPointFromScreen:screen] point:&point]) return;
    const auto viewport = _session->viewport();
    if (point.x >= 0 && point.x < viewport.width && point.y >= 0 && point.y < viewport.height)
        _session->pointer_event({host::PointerEvent::Phase::Enter, 0, point.x, point.y, NSEvent.modifierFlags});
}

- (void)refreshPointerPosition {
    if (_gameInputSuspended || !self.window.isKeyWindow || (!_confinementActive && !_session->has_software_cursor())) return;
    const auto position = NSEvent.mouseLocation;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    const auto target = _confinementActive && !NSPointInRect(position, bounds)
        ? clamp_inside(position, bounds) : position;
    [self sendPointerAtScreenPosition:target];
}

- (void)forwardPointer:(NSEvent*)event phase:(host::PointerEvent::Phase)phase {
    if (_gameInputSuspended) return;
    using Phase = host::PointerEvent::Phase;
    if (_confinementActive && phase == Phase::Leave) return;
    NSPoint point;
    if (![self viewportPointForEvent:event point:&point]) return;

    host::PointerEvent input{phase, 0, point.x, point.y, event.modifierFlags};
    if (phase == Phase::Down || phase == Phase::Drag || phase == Phase::Up) {
        input.button = button_number(event);
        if (input.button >= 64) return;
        const auto button = std::uint64_t{1} << input.button;
        if (phase == Phase::Down) _pressedButtons |= button;
        else if (!(_pressedButtons & button)) return;
        if (phase == Phase::Up) _pressedButtons &= ~button;
        input.click_count = static_cast<std::uint32_t>(event.clickCount);
    }
    _session->pointer_event(input);
}

- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self forwardPointer:event phase:host::PointerEvent::Phase::Down];
}
- (void)mouseDragged:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Drag]; }
- (void)mouseUp:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Up]; }
- (void)rightMouseDown:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Down]; }
- (void)rightMouseDragged:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Drag]; }
- (void)rightMouseUp:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Up]; }
- (void)otherMouseDown:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Down]; }
- (void)otherMouseDragged:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Drag]; }
- (void)otherMouseUp:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Up]; }
- (void)mouseMoved:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Move]; }
- (void)mouseEntered:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Enter]; }
- (void)mouseExited:(NSEvent*)event { [self forwardPointer:event phase:host::PointerEvent::Phase::Leave]; }

- (void)scrollWheel:(NSEvent*)event {
    if (_gameInputSuspended) return;
    NSPoint point;
    if (![self viewportPointForEvent:event point:&point]) return;

    _session->scroll_event({
        point.x, point.y, event.deltaX, event.deltaY,
        event.scrollingDeltaX, event.scrollingDeltaY, event.modifierFlags,
        static_cast<std::uint32_t>(event.phase), static_cast<std::uint32_t>(event.momentumPhase),
        event.hasPreciseScrollingDeltas != NO, event.isDirectionInvertedFromDevice != NO,
    });
}

- (void)forwardKey:(NSEvent*)event phase:(host::KeyEvent::Phase)phase {
    if (_gameInputSuspended) return;
    // The C++ callback borrows these bytes, including any embedded NUL.
    NSData* characters __attribute__((objc_precise_lifetime)) =
        [event.characters dataUsingEncoding:NSUTF8StringEncoding];
    NSData* ignoringModifiers __attribute__((objc_precise_lifetime)) =
        [event.charactersIgnoringModifiers dataUsingEncoding:NSUTF8StringEncoding];
    _session->key_event({
        phase, event.keyCode, event.modifierFlags, event.isARepeat != NO,
        utf8_view(characters), utf8_view(ignoringModifiers),
    });
}

- (void)keyDown:(NSEvent*)event { [self forwardKey:event phase:host::KeyEvent::Phase::Down]; }
- (void)keyUp:(NSEvent*)event { [self forwardKey:event phase:host::KeyEvent::Phase::Up]; }

- (void)flagsChanged:(NSEvent*)event {
    if (_gameInputSuspended) return;
    _session->key_event({host::KeyEvent::Phase::ModifiersChanged, event.keyCode,
                        event.modifierFlags, false, {}, {}});
}

@end

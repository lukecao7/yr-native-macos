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

}  // namespace

@implementation CoastalInputView {
    host::Session* _session;
    NSTrackingArea* _pointerTracking;
    BOOL _softwareCursor;
    BOOL _gameInputSuspended;
    BOOL _confinementActive;
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
    [self syncFrameCursor];
}

- (BOOL)pointerConfinementActive { return _confinementActive; }

- (NSRect)pointerBoundsOnScreen {
    return [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
}

- (BOOL)moveSystemPointerTo:(NSPoint)point {
    const auto screen = self.window.screen;
    NSNumber* display = screen.deviceDescription[@"NSScreenNumber"];
    if (!display) return NO;
    const auto quartz = CGDisplayBounds(display.unsignedIntValue);
    const auto cocoa = screen.frame;
    const auto result = CGWarpMouseCursorPosition(CGPointMake(
        quartz.origin.x + point.x - NSMinX(cocoa),
        quartz.origin.y + NSMaxY(cocoa) - point.y));
    return result == kCGErrorSuccess;
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
    const auto position = NSPointInRect(pointer, bounds) ? clamp_inside(pointer, bounds)
                                                        : NSMakePoint(NSMidX(bounds), NSMidY(bounds));
    if (!NSEqualPoints(pointer, position)) [self moveSystemPointerTo:position];

    // AppKit keeps applying the user's tracking speed and acceleration.
    // Observe motion on either side of the boundary without intercepting other apps' events.
    __weak CoastalInputView* view = self;
    _localPointerMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:pointer_motion
        handler:^NSEvent*(NSEvent* event) {
            [view updateConstrainedPointer];
            return event;
        }];
    _globalPointerMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:pointer_motion
        handler:^(NSEvent*) { [view updateConstrainedPointer]; }];
}

- (void)updateConstrainedPointer {
    if (!_confinementActive || _gameInputSuspended || !NSApp.isActive || !self.window.isKeyWindow) return;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    const auto pointer = NSEvent.mouseLocation;
    const auto next = clamp_inside(pointer, bounds);
    if (NSEqualPoints(next, pointer)) return;
    if (![self moveSystemPointerTo:next]) return;
    // A Quartz warp does not dispatch a mouse event. Publish the edge explicitly
    // when the original movement reached the letterbox or another window.
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
    _pointerTracking = [[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingInVisibleRect | NSTrackingActiveInKeyWindow |
                NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                NSTrackingEnabledDuringMouseDrag
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
    [self sendPointerAtScreenPosition:_confinementActive ? clamp_inside(position, bounds) : position];
}

- (void)forwardPointer:(NSEvent*)event phase:(host::PointerEvent::Phase)phase {
    if (_gameInputSuspended) return;
    using Phase = host::PointerEvent::Phase;
    auto location = event.locationInWindow;
    if (_confinementActive) {
        if (phase == Phase::Leave) {
            [self updateConstrainedPointer];
            return;
        }
        const auto bounds = [self pointerBoundsOnScreen];
        if (NSIsEmptyRect(bounds)) return;
        const auto screen = [self.window convertPointToScreen:location];
        location = [self.window convertPointFromScreen:clamp_inside(screen, bounds)];
    }
    NSPoint point;
    if (![self viewportPointForLocation:location point:&point]) return;

    host::PointerEvent input{phase, 0, point.x, point.y, event.modifierFlags};
    if (phase == Phase::Down || phase == Phase::Drag || phase == Phase::Up) {
        input.button = button_number(event);
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
    auto location = event.locationInWindow;
    if (_confinementActive) {
        const auto bounds = [self pointerBoundsOnScreen];
        if (NSIsEmptyRect(bounds)) return;
        const auto screen = [self.window convertPointToScreen:location];
        location = [self.window convertPointFromScreen:clamp_inside(screen, bounds)];
    }
    if (![self viewportPointForLocation:location point:&point]) return;

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

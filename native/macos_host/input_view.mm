// SPDX-License-Identifier: GPL-3.0-only
#import "input_view.h"
#include <algorithm>
#include <cmath>
#include <optional>

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

struct EdgeWarp {
    NSRect bounds;
    host::Viewport viewport;
    NSPoint target;
    NSPoint inward;
    NSTimeInterval last_position_timestamp;
    bool awaiting_inward_move = true;

    bool moved_inward(NSPoint position) const noexcept {
        // Allow for the system cursor's point quantization at the warp target.
        return (position.x - target.x) * inward.x > 1 ||
               (position.y - target.y) * inward.y > 1;
    }
};

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
    BOOL _scrollGestureActive;
    std::uint64_t _pressedButtons;
    std::optional<EdgeWarp> _edgeWarp;
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
    _edgeWarp.reset();
    _scrollGestureActive = NO;
    if (suspended) _pressedButtons = 0;
    [self syncFrameCursor];
}

- (void)viewWillMoveToWindow:(NSWindow*)newWindow {
    if (newWindow != self.window) {
        _edgeWarp.reset();
        _scrollGestureActive = NO;
        _pressedButtons = 0;
    }
    [super viewWillMoveToWindow:newWindow];
}

- (BOOL)pointerConfinementActive { return _confinementActive; }

- (NSRect)pointerBoundsOnScreen {
    if (!self.window) return NSZeroRect;
    return [self.window convertRectToScreen:[self convertRect:self.bounds toView:nil]];
}

- (NSPoint)systemPointerLocation { return NSEvent.mouseLocation; }
- (BOOL)systemPointerButtonDown { return NSEvent.pressedMouseButtons != 0; }
- (NSTimeInterval)pointerUptime { return NSProcessInfo.processInfo.systemUptime; }

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
        _edgeWarp.reset();
        _scrollGestureActive = NO;
        if (_localPointerMonitor) [NSEvent removeMonitor:_localPointerMonitor];
        if (_globalPointerMonitor) [NSEvent removeMonitor:_globalPointerMonitor];
        _localPointerMonitor = nil;
        _globalPointerMonitor = nil;
        return;
    }
    if (_confinementActive || [self systemPointerButtonDown]) return;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    _confinementActive = YES;
    const auto pointer = [self systemPointerLocation];
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
        handler:^(NSEvent*) { [view sampleConstrainedPointerRecovering:NO]; }];
}

- (BOOL)windowOwnsConstrainedPointer {
    return NSApp.isActive && !NSApp.modalWindow && self.window.isKeyWindow;
}

- (BOOL)canConstrainPointer {
    return _confinementActive && !_gameInputSuspended && [self windowOwnsConstrainedPointer];
}

- (BOOL)canRecoverConstrainedPointer {
    return [self canConstrainPointer] && !_pressedButtons && !_scrollGestureActive &&
           ![self systemPointerButtonDown];
}

- (void)validateEdgeWarpInBounds:(NSRect)bounds {
    if (!_edgeWarp) return;
    const auto viewport = _session->viewport();
    if (NSIsEmptyRect(bounds) || ![self canRecoverConstrainedPointer] ||
        !NSEqualRects(bounds, _edgeWarp->bounds) ||
        viewport.width != _edgeWarp->viewport.width || viewport.height != _edgeWarp->viewport.height)
        _edgeWarp.reset();
}

- (BOOL)warpPointerFrom:(NSPoint)origin to:(NSPoint)target inside:(NSRect)bounds {
    _edgeWarp.reset();
    const auto timestamp = [self pointerUptime];
    if (![self moveSystemPointerTo:target]) return NO;
    if ([self canRecoverConstrainedPointer]) {
        const NSPoint inward = NSMakePoint(
            origin.x < NSMinX(bounds) ? 1 : origin.x >= NSMaxX(bounds) ? -1 : 0,
            origin.y < NSMinY(bounds) ? 1 : origin.y >= NSMaxY(bounds) ? -1 : 0);
        _edgeWarp = EdgeWarp{bounds, _session->viewport(), target, inward, timestamp};
    }
    return YES;
}

- (NSEvent*)handleLocalPointerMotion:(NSEvent*)event {
    if (![self canConstrainPointer]) return event;
    if (event.window && event.window != self.window) return event;
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return event;
    [self validateEdgeWarpInBounds:bounds];
    const auto systemPoint = [self systemPointerLocation];
    if (!NSPointInRect(systemPoint, bounds)) {
        const auto target = clamp_inside(systemPoint, bounds);
        if ([self warpPointerFrom:systemPoint to:target inside:bounds] &&
            _edgeWarp && event.type == NSEventTypeMouseMoved) {
            [self sendPointerAtScreenPosition:target phase:host::PointerEvent::Phase::Move
                                   modifiers:event.modifierFlags];
            return nil;
        }
    }
    if (event.type != NSEventTypeMouseMoved) return event;

    const auto location = [self eventLocationInWindow:event];
    if (event.window == self.window &&
        NSPointInRect([self convertPoint:location fromView:nil], self.bounds)) return event;
    [self forwardPointer:event phase:host::PointerEvent::Phase::Move];
    return nil;
}

- (void)updateConstrainedPointer {
    [self sampleConstrainedPointerRecovering:YES];
}

- (void)sampleConstrainedPointerRecovering:(BOOL)recover {
    if (![self canConstrainPointer]) {
        _edgeWarp.reset();
        return;
    }
    const auto bounds = [self pointerBoundsOnScreen];
    [self validateEdgeWarpInBounds:bounds];
    if (NSIsEmptyRect(bounds)) return;
    // The event timestamp and uptime share AppKit's time since system startup.
    const bool awaitingInwardMove = recover && _edgeWarp && _edgeWarp->awaiting_inward_move;
    const auto sampledAt = awaitingInwardMove ? [self pointerUptime] : 0;
    const auto pointer = [self systemPointerLocation];
    if (NSPointInRect(pointer, bounds)) {
        if (awaitingInwardMove && _edgeWarp->moved_inward(pointer)) {
            _edgeWarp->last_position_timestamp = sampledAt;
            _edgeWarp->awaiting_inward_move = false;
            [self sendPointerAtScreenPosition:pointer phase:host::PointerEvent::Phase::Move
                                    modifiers:NSEvent.modifierFlags];
        }
        return;
    }
    const auto next = clamp_inside(pointer, bounds);
    if (![self warpPointerFrom:pointer to:next inside:bounds]) return;
    // A Quartz warp does not dispatch a mouse event. Publish the edge explicitly
    // when the original movement went outside the event stream of our window.
    [self sendPointerAtScreenPosition:next phase:host::PointerEvent::Phase::Enter
                            modifiers:NSEvent.modifierFlags];
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

- (void)sendPointerAtScreenPosition:(NSPoint)screen phase:(host::PointerEvent::Phase)phase
                         modifiers:(NSEventModifierFlags)modifiers {
    NSPoint point;
    if (![self viewportPointForLocation:[self.window convertPointFromScreen:screen] point:&point]) return;
    const auto viewport = _session->viewport();
    if (point.x >= 0 && point.x < viewport.width && point.y >= 0 && point.y < viewport.height)
        _session->pointer_event({phase, 0, point.x, point.y, modifiers});
}

- (void)refreshPointerPosition {
    if (_gameInputSuspended || !self.window.isKeyWindow || (!_confinementActive && !_session->has_software_cursor())) return;
    const auto position = [self systemPointerLocation];
    const auto bounds = [self pointerBoundsOnScreen];
    if (NSIsEmptyRect(bounds)) return;
    const auto target = _confinementActive && !NSPointInRect(position, bounds)
        ? clamp_inside(position, bounds) : position;
    [self sendPointerAtScreenPosition:target phase:host::PointerEvent::Phase::Enter
                            modifiers:NSEvent.modifierFlags];
}

- (BOOL)acceptHoverEventDuringEdgeWarp:(NSEvent*)event phase:(host::PointerEvent::Phase)phase {
    if (!_edgeWarp) return YES;
    [self validateEdgeWarpInBounds:[self pointerBoundsOnScreen]];
    if (!_edgeWarp) return YES;
    if ((event.window && event.window != self.window) ||
        event.timestamp <= _edgeWarp->last_position_timestamp) return NO;

    if (phase == host::PointerEvent::Phase::Enter || _edgeWarp->awaiting_inward_move) {
        const auto screen = [self.window convertPointToScreen:[self eventLocationInWindow:event]];
        if (phase == host::PointerEvent::Phase::Enter) {
            // AppKit can create a tracking event late, with an older location.
            const auto current = [self systemPointerLocation];
            if (!NSPointInRect(current, _edgeWarp->bounds) ||
                std::abs(screen.x - current.x) > 1 || std::abs(screen.y - current.y) > 1) return NO;
        }
        if (NSPointInRect(screen, _edgeWarp->bounds) && _edgeWarp->moved_inward(screen))
            _edgeWarp->awaiting_inward_move = false;
    }
    _edgeWarp->last_position_timestamp = event.timestamp;
    return YES;
}

- (void)forwardPointer:(NSEvent*)event phase:(host::PointerEvent::Phase)phase {
    if (_gameInputSuspended) return;
    using Phase = host::PointerEvent::Phase;
    if (_confinementActive && phase == Phase::Leave) return;
    const bool mouseButton = phase == Phase::Down || phase == Phase::Drag || phase == Phase::Up;
    if (mouseButton) _edgeWarp.reset();
    if ((phase == Phase::Move || phase == Phase::Enter) &&
        ![self acceptHoverEventDuringEdgeWarp:event phase:phase]) return;
    NSPoint point;
    if (![self viewportPointForEvent:event point:&point]) return;

    host::PointerEvent input{phase, 0, point.x, point.y, event.modifierFlags};
    if (mouseButton) {
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
    constexpr auto active = NSEventPhaseMayBegin | NSEventPhaseBegan | NSEventPhaseChanged | NSEventPhaseStationary;
    _edgeWarp.reset();
    _scrollGestureActive = ((event.phase | event.momentumPhase) & active) != 0;
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

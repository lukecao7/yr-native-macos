// SPDX-License-Identifier: GPL-3.0-only
#import "input_view.h"

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

}  // namespace

@implementation CoastalInputView {
    host::Session* _session;
    NSTrackingArea* _pointerTracking;
}

- (instancetype)initWithFrame:(NSRect)rect session:(host::Session*)session {
    if (!session) return nil;
    if ((self = [super initWithFrame:rect])) {
        _session = session;
    }
    return self;
}

- (BOOL)acceptsFirstResponder { return YES; }

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

- (BOOL)viewportPointForEvent:(NSEvent*)event point:(NSPoint*)position {
    const NSRect bounds = self.bounds;
    if (NSIsEmptyRect(bounds)) return NO;

    const NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    const auto viewport = _session->viewport();
    const CGFloat y = self.isFlipped ? point.y - NSMinY(bounds) : NSMaxY(bounds) - point.y;
    *position = NSMakePoint((point.x - NSMinX(bounds)) * viewport.width / NSWidth(bounds),
                            y * viewport.height / NSHeight(bounds));
    return YES;
}

- (void)forwardPointer:(NSEvent*)event phase:(host::PointerEvent::Phase)phase {
    NSPoint point;
    if (![self viewportPointForEvent:event point:&point]) return;

    host::PointerEvent input{phase, 0, point.x, point.y, event.modifierFlags};
    using Phase = host::PointerEvent::Phase;
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
    _session->key_event({host::KeyEvent::Phase::ModifiersChanged, event.keyCode,
                        event.modifierFlags, false, {}, {}});
}

@end

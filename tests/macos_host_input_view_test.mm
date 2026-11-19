// SPDX-License-Identifier: GPL-3.0-only
#import "macos_host/input_view.h"

#import <CoreGraphics/CoreGraphics.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace coastal::macos_host;

namespace {

void expect(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

struct RecordedKey {
    KeyEvent::Phase phase;
    std::uint16_t key_code;
    InputModifiers modifiers;
    bool repeat;
    std::string characters;
    std::string characters_ignoring_modifiers;
};

class RecordingSession final : public Session {
public:
    Viewport viewport() const noexcept override { return {1024, 768}; }
    FrameClock::duration frame_interval() const noexcept override {
        return std::chrono::milliseconds(16);
    }
    std::optional<Frame> advance_frame() override { return std::nullopt; }

    void pointer_event(PointerEvent event) noexcept override { pointers.push_back(event); }
    void scroll_event(ScrollEvent event) noexcept override { scrolls.push_back(event); }
    void key_event(KeyEvent event) noexcept override {
        keys.push_back({event.phase, event.key_code, event.modifiers, event.repeat,
                        std::string(event.characters), std::string(event.characters_ignoring_modifiers)});
    }

    std::vector<PointerEvent> pointers;
    std::vector<ScrollEvent> scrolls;
    std::vector<RecordedKey> keys;
};

NSEvent* mouse(NSEventType type, NSPoint point, NSEventModifierFlags modifiers,
               NSInteger click_count = 1) {
    NSEvent* event = [NSEvent mouseEventWithType:type location:point modifierFlags:modifiers
        timestamp:1 windowNumber:0 context:nil eventNumber:1 clickCount:click_count pressure:1];
    expect(event != nil, "Create a local pointer event");
    return event;
}

NSEvent* crossing(NSEventType type, NSPoint point) {
    NSEvent* event = [NSEvent enterExitEventWithType:type location:point modifierFlags:0
        timestamp:1 windowNumber:0 context:nil eventNumber:1 trackingNumber:1 userData:nullptr];
    expect(event != nil, "Create a local pointer tracking event");
    return event;
}

void expect_pointer(const PointerEvent& event, PointerEvent::Phase phase, std::uint32_t button,
                    double x, double y, InputModifiers modifiers, std::uint32_t click_count) {
    if (event.phase == phase && event.button == button && event.x == x && event.y == y &&
        event.modifiers == modifiers && event.click_count == click_count) return;

    std::cerr << "Pointer event phase/button/x/y/modifiers/clicks: got "
              << static_cast<int>(event.phase) << '/' << event.button << '/'
              << event.x << '/' << event.y << '/' << event.modifiers << '/' << event.click_count
              << ", expected " << static_cast<int>(phase) << '/' << button << '/'
              << x << '/' << y << '/' << modifiers << '/' << click_count << '\n';
    throw std::runtime_error("Pointer event replay differed from the expected viewport event");
}

void replay_pointer(CoastalInputView* view, RecordingSession& session) {
    using Phase = PointerEvent::Phase;
    constexpr auto shift = NSEventModifierFlagShift;
    constexpr auto control = NSEventModifierFlagControl;
    [view mouseDown:mouse(NSEventTypeLeftMouseDown, NSMakePoint(64, 320), shift, 2)];
    [view mouseDragged:mouse(NSEventTypeLeftMouseDragged, NSMakePoint(174, 214), shift)];
    [view mouseUp:mouse(NSEventTypeLeftMouseUp, NSMakePoint(-5, 390), shift)];
    [view rightMouseDown:mouse(NSEventTypeRightMouseDown, NSMakePoint(50, 60), control)];
    [view rightMouseDragged:mouse(NSEventTypeRightMouseDragged, NSMakePoint(59, 61), control)];
    [view rightMouseUp:mouse(NSEventTypeRightMouseUp, NSMakePoint(70, 65), control)];
    [view mouseEntered:crossing(NSEventTypeMouseEntered, NSMakePoint(512, 30))];
    [view mouseMoved:mouse(NSEventTypeMouseMoved, NSMakePoint(511, 30), shift, 0)];
    [view mouseExited:crossing(NSEventTypeMouseExited, NSMakePoint(514, 29))];

    expect(session.pointers.size() == 9, "Forward each mouse replay exactly once");
    expect_pointer(session.pointers[0], Phase::Down, 0, 128, 128, shift, 2);
    expect_pointer(session.pointers[1], Phase::Drag, 0, 348, 340, shift, 1);
    expect_pointer(session.pointers[2], Phase::Up, 0, -10, -12, shift, 1);
    expect_pointer(session.pointers[3], Phase::Down, 1, 100, 648, control, 1);
    expect_pointer(session.pointers[4], Phase::Drag, 1, 118, 646, control, 1);
    expect_pointer(session.pointers[5], Phase::Up, 1, 140, 638, control, 1);
    expect_pointer(session.pointers[6], Phase::Enter, 0, 1024, 708, 0, 0);
    expect_pointer(session.pointers[7], Phase::Move, 0, 1022, 708, shift, 0);
    expect_pointer(session.pointers[8], Phase::Leave, 0, 1028, 710, 0, 0);
}

NSEvent* wheel(CGScrollEventUnit unit, CGScrollPhase phase, CGMomentumScrollPhase momentum) {
    CGEventRef cg_event = CGEventCreateScrollWheelEvent(nullptr, unit, 2, -13, 9);
    expect(cg_event != nullptr, "Create a local scroll event");
    CGEventSetIntegerValueField(cg_event, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(cg_event, kCGScrollWheelEventMomentumPhase, momentum);
    CGEventSetFlags(cg_event, kCGEventFlagMaskControl | kCGEventFlagMaskShift);
    NSEvent* event = [NSEvent eventWithCGEvent:cg_event];
    CFRelease(cg_event);
    expect(event != nil, "Wrap the scroll event for AppKit replay");
    return event;
}

void replay_scroll(CoastalInputView* view, RecordingSession& session) {
    NSEvent* const events[] = {
        wheel(kCGScrollEventUnitLine, kCGScrollPhaseChanged, kCGMomentumScrollPhaseNone),
        wheel(kCGScrollEventUnitPixel, kCGScrollPhaseEnded, kCGMomentumScrollPhaseBegin),
    };
    for (NSEvent* event : events) [view scrollWheel:event];
    expect(session.scrolls.size() == 2, "Forward both scroll events");
    for (std::size_t index = 0; index < session.scrolls.size(); ++index) {
        NSEvent* event = events[index];
        const auto& input = session.scrolls[index];
        const auto position = [view convertPoint:event.locationInWindow fromView:nil];
        expect(input.x == position.x * 2 && input.y == (384 - position.y) * 2 &&
                   input.delta_x == event.deltaX && input.delta_y == event.deltaY &&
                   input.scrolling_delta_x == event.scrollingDeltaX &&
                   input.scrolling_delta_y == event.scrollingDeltaY &&
                   input.modifiers == event.modifierFlags &&
                   input.phase == event.phase && input.momentum_phase == event.momentumPhase &&
                   input.precise == (event.hasPreciseScrollingDeltas != NO) &&
                   input.inverted == (event.isDirectionInvertedFromDevice != NO),
               "Preserve AppKit wheel and trackpad units, phases and modifier data");
    }
    expect(!session.scrolls[0].precise && session.scrolls[1].precise &&
               session.scrolls[1].scrolling_delta_x != 0 && session.scrolls[1].scrolling_delta_y != 0,
           "Exercise both coarse and precise two-axis scrolling");
}

NSEvent* key(NSEventType type, NSString* text, NSString* unmodified,
             NSEventModifierFlags modifiers, BOOL repeat) {
    NSEvent* event = [NSEvent keyEventWithType:type location:NSZeroPoint modifierFlags:modifiers
        timestamp:1 windowNumber:0 context:nil characters:text charactersIgnoringModifiers:unmodified
        isARepeat:repeat keyCode:18];
    expect(event != nil, "Create a local key event");
    return event;
}

void replay_keys(CoastalInputView* view, RecordingSession& session) {
    using Phase = KeyEvent::Phase;
    const auto modifiers = NSEventModifierFlagShift | NSEventModifierFlagControl | NSEventModifierFlagCommand;
    const unichar units[] = {0x00E9, 0, '1'};
    NSString* text = [NSString stringWithCharacters:units length:3];
    [view keyDown:key(NSEventTypeKeyDown, text, @"1", modifiers, YES)];
    [view keyUp:key(NSEventTypeKeyUp, @"1", @"1", NSEventModifierFlagCommand, NO)];

    CGEventRef cg_flags = CGEventCreateKeyboardEvent(nullptr, 56, true);
    expect(cg_flags != nullptr, "Create a local modifier event");
    CGEventSetType(cg_flags, kCGEventFlagsChanged);
    CGEventSetFlags(cg_flags, kCGEventFlagMaskShift);
    NSEvent* flags = [NSEvent eventWithCGEvent:cg_flags];
    CFRelease(cg_flags);
    expect(flags != nil, "Wrap the modifier event for AppKit replay");
    [view flagsChanged:flags];

    expect(session.keys.size() == 3, "Forward press, release and flags changes");
    const auto& down = session.keys[0];
    expect(down.phase == Phase::Down && down.key_code == 18 && down.repeat &&
               down.modifiers == (static_cast<InputModifiers>(Modifier::Shift) |
                                  static_cast<InputModifiers>(Modifier::Control) |
                                  static_cast<InputModifiers>(Modifier::Command)) &&
               down.characters == std::string("\xC3\xA9\0" "1", 4) &&
               down.characters_ignoring_modifiers == "1",
           "Preserve key code, repeat, modifiers and the full UTF-8 payload");
    const auto& up = session.keys[1];
    expect(up.phase == Phase::Up && up.key_code == 18 && !up.repeat &&
               up.modifiers == NSEventModifierFlagCommand && up.characters == "1",
           "Forward the release and its current modifiers");
    const auto& changed = session.keys[2];
    expect(changed.phase == Phase::ModifiersChanged && changed.key_code == 56 &&
               !changed.repeat && changed.modifiers == flags.modifierFlags &&
               changed.characters.empty() && changed.characters_ignoring_modifiers.empty(),
           "Forward modifier transitions without asking AppKit for key text");
}

void verify_tracking(CoastalInputView* view) {
    [view updateTrackingAreas];
    [view updateTrackingAreas];
    const auto needed = NSTrackingInVisibleRect | NSTrackingActiveInKeyWindow |
                        NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
                        NSTrackingEnabledDuringMouseDrag;
    expect(view.acceptsFirstResponder && view.trackingAreas.count == 1 &&
               (view.trackingAreas.firstObject.options & needed) == needed,
           "The view can take key focus and tracks the current visible bounds without duplicate areas");
}

}  // namespace

int main() {
    @autoreleasepool {
        try {
            RecordingSession session;
            CoastalInputView* view = [[CoastalInputView alloc]
                initWithFrame:NSMakeRect(0, 0, 512, 384) session:&session];
            expect(view != nil, "Create an offscreen input view");
            verify_tracking(view);
            replay_pointer(view, session);
            replay_scroll(view, session);
            replay_keys(view, session);
            std::cout << "macOS host input replay tests passed\n";
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}

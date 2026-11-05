/* event_capture_test.mm — P22 事件捕获自检：自造 NSEvent -> yr_event 映射
 *
 * 【SELF_AUTHORED_MOCK】本文件的 CapSink 只记录 yr_event，不含输入桥 / 世界逻辑。
 * 【未与真实世界核心 / 输入桥链接，将在第六批集成】——见 RUN.txt / LIMITATIONS.md。
 *
 * 用 +[NSEvent mouseEventWithType:...] / +[NSEvent keyEventWithType:...] 自造事件，
 * 经 YrEventFromNSEvent（与 NSResponder 各方法共用的 SELF_AUTHORED_MAPPING）映射，
 * 再经 YrShellHandleNSEvent 投递到注入的 SELF_AUTHORED_MOCK sink。
 * 每个用例的 expected 是【手写字面量】，核验器 verify_shell.py 独立重算比对。
 *
 * 产出：event_capture.json
 */
#import <Foundation/Foundation.h>
#include "shell_internal.h"
#include <cstdio>
#include <cstring>
#include <string>

/* ---- SELF_AUTHORED_MOCK：只记录 yr_event 的 sink ---- */
struct CapSink { int count; yr_event last; };
static int CapSinkFn(const yr_event *ev, void *userdata) {
    CapSink *s = static_cast<CapSink *>(userdata);
    if (s && ev) { s->count++; s->last = *ev; }
    return 1;
}

struct CapCase {
    int id;
    const char *nsType;
    NSEventType type;
    double loc_x, loc_y;
    NSEventModifierFlags flags;
    const char *flagJson;       /* JSON 数组内容，如 [\"shift\"] */
    NSInteger click;
    unsigned short keyCode;
    bool isKey;
    yr_event expected;         /* 手写字面量，非运行时推导 */
};

static NSEvent *MakeEvent(const CapCase &c) {
    NSPoint loc = NSMakePoint(c.loc_x, c.loc_y);
    if (c.isKey) {
        return [NSEvent keyEventWithType:c.type
                                location:loc
                           modifierFlags:c.flags
                               timestamp:0.0
                            windowNumber:0
                                 context:nil
                              characters:@" "
               charactersIgnoringModifiers:@" "
                               isARepeat:NO
                                 keyCode:c.keyCode];
    }
    return [NSEvent mouseEventWithType:c.type
                              location:loc
                         modifierFlags:c.flags
                             timestamp:0.0
                          windowNumber:0
                               context:nil
                           eventNumber:1
                            clickCount:c.click
                              pressure:1.0];
}

static int EvEqual(const yr_event &a, const yr_event &b) {
    return a.type == b.type && a.x == b.x && a.y == b.y &&
           a.modifiers == b.modifiers && a.click_count == b.click_count &&
           a.key_code == b.key_code;
}

static void AppendEv(std::string &s, const yr_event &e) {
    char b[256];
    std::snprintf(b, sizeof(b),
        "{\"type\":%d,\"x\":%d,\"y\":%d,\"modifiers\":%d,\"click_count\":%d,\"key_code\":%d}",
        e.type, e.x, e.y, e.modifiers, e.click_count, e.key_code);
    s += b;
}

int main(void) {
    @autoreleasepool {
        const int CELL = 24, VIEW_H = 720;
        const NSEventModifierFlags F_SHIFT   = NSEventModifierFlagShift;
        const NSEventModifierFlags F_CTRL    = NSEventModifierFlagControl;
        const NSEventModifierFlags F_ALT     = NSEventModifierFlagOption;

        /* expected 全部为手写字面量；上方注释给出手算依据。 */
        CapCase cases[] = {
            /* id, nsType, type, loc, flags, flagJson, click, key, isKey, expected */
            {1, "LeftMouseDown", NSEventTypeLeftMouseDown, 240, 480, F_SHIFT, "[\"shift\"]", 2, 0, false,
                {YR_EVT_LEFT_DOWN, 10, 10, YR_MOD_SHIFT, 2, -1}},
            {2, "LeftMouseUp", NSEventTypeLeftMouseUp, 480, 240, 0, "[]", 1, 0, false,
                {YR_EVT_LEFT_UP, 20, 20, 0, 1, -1}},
            {3, "LeftMouseDragged", NSEventTypeLeftMouseDragged, 720, 360, F_CTRL, "[\"control\"]", 1, 0, false,
                {YR_EVT_LEFT_DRAG, 30, 15, YR_MOD_CONTROL, 1, -1}},
            {4, "RightMouseDown", NSEventTypeRightMouseDown, 960, 600, F_ALT, "[\"alt\"]", 1, 0, false,
                {YR_EVT_RIGHT_DOWN, 40, 5, YR_MOD_ALT, 1, -1}},
            {5, "RightMouseUp", NSEventTypeRightMouseUp, 120, 120, F_SHIFT | F_CTRL, "[\"shift\",\"control\"]", 2, 0, false,
                {YR_EVT_RIGHT_UP, 5, 25, YR_MOD_SHIFT | YR_MOD_CONTROL, 2, -1}},
            {6, "KeyDown", NSEventTypeKeyDown, 0, 720, F_SHIFT | F_ALT, "[\"shift\",\"alt\"]", 0, 0, true,
                {YR_EVT_KEY_DOWN, 0, 0, YR_MOD_SHIFT | YR_MOD_ALT, 0, 0}},
            {7, "KeyUp", NSEventTypeKeyUp, 48, 672, 0, "[]", 0, 49, true,
                {YR_EVT_KEY_UP, 2, 2, 0, 0, 49}},
            {8, "LeftMouseDown", NSEventTypeLeftMouseDown, 24, 24, F_SHIFT | F_CTRL | F_ALT,
                "[\"shift\",\"control\",\"alt\"]", 3, 0, false,
                {YR_EVT_LEFT_DOWN, 1, 29, YR_MOD_SHIFT | YR_MOD_CONTROL | YR_MOD_ALT, 3, -1}},
        };
        const int N = (int)(sizeof(cases) / sizeof(cases[0]));

        CapSink sink; sink.count = 0; memset(&sink.last, 0, sizeof(sink.last));
        yr_shell_set_event_sink(CapSinkFn, &sink);

        std::string out;
        out += "{\n";
        out += "  \"component\": \"p22-app-shell\",\n";
        out += "  \"marker\": \"SELF_AUTHORED_MOCK\",\n";
        out += "  \"integration_note\": \"未与真实世界核心 / 输入桥链接，将在第六批集成\",\n";
        out += "  \"method\": \"synthesized NSEvent (headless, no NSApplication)\",\n";
        out += "  \"mapping_marker\": \"SELF_AUTHORED_MAPPING\",\n";
        out += "  \"cell_px\": 24,\n  \"view_h\": 720,\n";
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp), "  \"case_count\": %d,\n", N); out += tmp;
        out += "  \"cases\": [\n";

        int failures = 0, delivered = 0;
        for (int i = 0; i < N; ++i) {
            CapCase &c = cases[i];
            NSEvent *e = MakeEvent(c);
            yr_event actual = YrEventFromNSEvent(e, CELL, VIEW_H);
            int pass = EvEqual(actual, c.expected);

            /* 经 event sink 送出（与 NSResponder 共用 YrShellHandleNSEvent）。 */
            int before = sink.count;
            YrShellHandleNSEvent(e, CELL, VIEW_H);
            int sink_delivered = (sink.count == before + 1);
            int sink_matches = sink_delivered && EvEqual(sink.last, actual);
            if (sink_delivered) delivered++;
            if (!pass || !sink_delivered || !sink_matches) failures++;

            out += "    {\"id\": "; out += std::to_string(c.id);
            out += ", \"nsevent_type\": \""; out += c.nsType; out += "\"";
            std::snprintf(tmp, sizeof(tmp), ", \"loc_x\": %d, \"loc_y\": %d", (int)c.loc_x, (int)c.loc_y);
            out += tmp;
            out += ", \"modifier_flags\": "; out += c.flagJson;
            std::snprintf(tmp, sizeof(tmp), ", \"click_count\": %d, \"key_code\": %d, \"cell_px\": %d, \"view_h\": %d, \"is_key\": %s",
                          (int)c.click, (int)c.keyCode, CELL, VIEW_H, c.isKey ? "true" : "false");
            out += tmp;
            out += ", \"expected\": "; AppendEv(out, c.expected);
            out += ", \"actual\": ";   AppendEv(out, actual);
            out += ", \"pass\": ";     out += pass ? "true" : "false";
            out += ", \"sink_delivered\": "; out += sink_delivered ? "true" : "false";
            out += ", \"sink_matches\": ";   out += sink_matches ? "true" : "false";
            out += "}";
            if (i + 1 < N) out += ",";
            out += "\n";
        }
        out += "  ],\n";
        std::snprintf(tmp, sizeof(tmp), "  \"sink\": {\"marker\": \"SELF_AUTHORED_MOCK\", \"total_delivered\": %d},\n", delivered);
        out += tmp;
        out += "  \"nsresponder_instance_test\": \"NOT_ACHIEVED — 自检经共用的 YrShellHandleNSEvent C 函数验证映射与投递；未实例化 NSView / 未派发真实 responder 消息（headless 安全约束）\",\n";
        std::snprintf(tmp, sizeof(tmp), "  \"selftest_failures\": %d\n", failures);
        out += tmp;
        out += "}\n";

        FILE *f = std::fopen("event_capture.json", "wb");
        if (!f) { std::fprintf(stderr, "cannot write event_capture.json\n"); return 1; }
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        std::printf("EVENT_CAPTURE_CASES=%d SINK_DELIVERED=%d SELFTEST_FAILURES=%d\n",
                    N, delivered, failures);
        return failures == 0 ? 0 : 1;
    }
}

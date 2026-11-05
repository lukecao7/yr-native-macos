/* offscreen_test.mm — P22 自检：注入回调 / 默认 provider / 帧变化证据 / padding / 帧元数据
 *
 * 【SELF_AUTHORED_MOCK】本文件里的 MockFrameProvider 只产生自制图案帧、
 * MockSink 只记录 yr_event，均不含任何世界逻辑 / 输入桥逻辑。
 * 【未与真实世界核心 / 输入桥链接，将在第六批集成】——见 RUN.txt / LIMITATIONS.md。
 *
 * 产出：frame_default.png（未注入时的默认 provider）、
 *       frame_a.png / frame_b.png（注入 SELF_AUTHORED_MOCK 后两个不同状态）、
 *       frame_meta.json（帧关联预留证据：frame_index / provider 调用序号 / mock 占位值）。
 * 帧差异由 verify_shell.py 用纯标准库 PNG 解码器逐像素核验。
 */
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include "shell_internal.h"
#include <cstdio>
#include <cstring>
#include <initializer_list>

/* 真实记录 SELF_AUTHORED_MOCK provider 的调用序号（供 frame_meta.json 取证）。 */
static int g_mock_provider_calls = 0;

/* ---- SELF_AUTHORED_MOCK：自制图案帧 provider（无世界逻辑） ---- */
static int MockFrameProvider(uint8_t *rgba, int width, int height,
                             size_t stride, void *userdata) {
    if (!rgba || width <= 0 || height <= 0 || stride < (size_t)width * 4) return -1;
    ++g_mock_provider_calls;
    int state = userdata ? *static_cast<int *>(userdata) : 0;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = rgba + (size_t)y * stride;
        for (int x = 0; x < width; ++x) {
            int cx = x / 32, cy = y / 32;
            int dx = x - (int)((state * 211u) % (unsigned)width);
            int dy = y - (int)((state * 97u) % (unsigned)height);
            int d = (dx * dx + dy * dy) >> 9;
            uint8_t r = (uint8_t)((x + state * 40 + cx * 7) & 0xff);
            uint8_t g = (uint8_t)((y + d + state * 13) & 0xff);
            uint8_t b = (uint8_t)((d * 3 + cy * 19 + state * 71) & 0xff);
            if (x % 32 == 0 || y % 32 == 0) { r = 0x08; g = 0x2a; b = 0x33; }
            uint8_t *p = row + (size_t)x * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255; /* A=255 */
        }
    }
    return width * height;
}

/* 故意失败；第二种情况在报错前写过部分像素，仍不得导出。 */
struct FailingFrame { bool write_partial; int calls; };
static int FailingFrameProvider(uint8_t *rgba, int, int, size_t, void *userdata) {
    FailingFrame *frame = static_cast<FailingFrame *>(userdata);
    ++frame->calls;
    if (frame->write_partial) {
        rgba[0] = 255; rgba[1] = 0; rgba[2] = 0; rgba[3] = 255;
    }
    return -42;
}

static bool PngDecodesAtSize(NSString *path, int width, int height) {
    CGImageSourceRef source = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:path], nullptr);
    if (!source) return false;
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    bool ok = image && CGImageGetWidth(image) == (size_t)width &&
                      CGImageGetHeight(image) == (size_t)height;
    if (image) CGImageRelease(image);
    CFRelease(source);
    return ok;
}

static int TestExportFailures(void) {
    int failures = 0;
    auto need = [&](const char *name, bool ok, int rc) {
        std::printf("EXPORT_CASE=%s RC=%d OK=%d\n", name, rc, ok);
        if (!ok) ++failures;
    };
    NSFileManager *fm = [NSFileManager defaultManager];
    NSString *dir = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [@"yr-shell-errors-" stringByAppendingString:[[NSProcessInfo processInfo] globallyUniqueString]]];
    if (![fm createDirectoryAtPath:dir withIntermediateDirectories:NO attributes:nil error:nil]) {
        need("setup", false, -1);
        return failures;
    }
    const int width = 32, height = 24;
    int state = 1;
    NSString *existing = [dir stringByAppendingPathComponent:@"existing.png"];
    yr_shell_set_frame_provider(MockFrameProvider, &state);
    int rc = yr_shell_render_offscreen_png([existing fileSystemRepresentation], width, height);
    NSData *original = [NSData dataWithContentsOfFile:existing];
    need("normal_provider", rc == 0 && original.length > 0 &&
         PngDecodesAtSize(existing, width, height), rc);

    uint8_t pixels[width * height * 4];
    uint8_t untouched[sizeof(pixels)];
    std::memset(untouched, 0x5A, sizeof(untouched));
    CGColorSpaceRef colors = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = colors ? CGBitmapContextCreate(
        pixels, width, height, 8, width * 4, colors,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big) : nullptr;
    if (colors) CGColorSpaceRelease(colors);
    need("draw_context_created", context != nullptr, context ? 0 : -1);
    if (context) {
        rc = YRShellDrawFrameIntoContext(context, width, height);
        need("normal_draw", rc == 0, rc);
    }
    rc = YRShellDrawFrameIntoContext(nullptr, width, height);
    need("invalid_draw_context", rc < 0, rc);

    for (bool partial : {false, true}) {
        FailingFrame frame = {partial, 0};
        yr_shell_set_frame_provider(FailingFrameProvider, &frame);
        CGImageRef composed = YRShellComposeFrame(width, height);
        need(partial ? "partial_compose" : "failed_compose",
             !composed && frame.calls == 1, composed ? 0 : -1);
        if (composed) CGImageRelease(composed);
        if (context) {
            std::memcpy(pixels, untouched, sizeof(pixels));
            rc = YRShellDrawFrameIntoContext(context, width, height);
            need(partial ? "partial_draw_unchanged" : "failed_draw_unchanged",
                 rc < 0 && std::memcmp(pixels, untouched, sizeof(pixels)) == 0, rc);
        }

        int callsBeforeExport = frame.calls;
        NSString *fresh = [dir stringByAppendingPathComponent:
            partial ? @"partial.png" : @"failed.png"];
        rc = yr_shell_render_offscreen_png([fresh fileSystemRepresentation], width, height);
        need(partial ? "partial_no_file" : "provider_failure_no_file",
             rc == -7 && frame.calls == callsBeforeExport + 1 &&
             ![fm fileExistsAtPath:fresh], rc);
        rc = yr_shell_render_offscreen_png([existing fileSystemRepresentation], width, height);
        need(partial ? "partial_preserves_existing" : "failure_preserves_existing",
             rc == -7 && frame.calls == callsBeforeExport + 2 &&
             [original isEqualToData:[NSData dataWithContentsOfFile:existing]], rc);
    }
    if (context) CGContextRelease(context);

    yr_shell_set_frame_provider(MockFrameProvider, &state);
    NSString *missing = [dir stringByAppendingPathComponent:@"missing/output.png"];
    rc = yr_shell_render_offscreen_png([missing fileSystemRepresentation], width, height);
    need("write_missing_parent", rc < 0 && ![fm fileExistsAtPath:missing], rc);
    rc = yr_shell_render_offscreen_png([dir fileSystemRepresentation], width, height);
    BOOL isDirectory = NO;
    need("write_directory", rc < 0 && [fm fileExistsAtPath:dir isDirectory:&isDirectory] &&
         isDirectory, rc);
    rc = yr_shell_render_offscreen_png([existing fileSystemRepresentation], width, height);
    need("success_after_failure", rc == 0 && PngDecodesAtSize(existing, width, height), rc);
    yr_shell_set_frame_provider(nullptr, nullptr);
    need("cleanup", [fm removeItemAtPath:dir error:nil], 0);
    return failures;
}

/* ---- SELF_AUTHORED_MOCK：只记录 yr_event 的 sink（无输入桥逻辑） ---- */
struct SinkLog { int count; yr_event last; };
static int MockSink(const yr_event *ev, void *userdata) {
    SinkLog *log = static_cast<SinkLog *>(userdata);
    if (log && ev) { log->count++; log->last = *ev; }
    return 1;
}

/* frame_meta.json：帧关联预留证据。所有 tick/event_seq/entity_ids/order_seq
 * 均显式标注为 SELF_AUTHORED_MOCK 占位值，不是真实世界状态。 */
static int WriteFrameMeta(int fa_seq, int fb_seq) {
    FILE *f = std::fopen("frame_meta.json", "wb");
    if (!f) return 0;
    std::fprintf(f,
        "{\n"
        "  \"component\": \"p22-app-shell\",\n"
        "  \"marker\": \"SELF_AUTHORED_MOCK\",\n"
        "  \"value_kind\": \"MOCK_PLACEHOLDER\",\n"
        "  \"warning\": \"以下 frame_index / tick / event_seq / entity_ids / order_seq 全部是 SELF_AUTHORED_MOCK 占位值，不是真实世界状态。本批没有一次真实运行把事件→实体→订单→tick→显示帧串起来（NOT_ACHIEVED，属第六批 P23 集成）。\",\n"
        "  \"frames\": [\n"
        "    {\n"
        "      \"png\": \"frame_a.png\",\n"
        "      \"frame_index\": 0,\n"
        "      \"provider\": \"SELF_AUTHORED_MOCK (MockFrameProvider)\",\n"
        "      \"provider_call_seq\": %d,\n"
        "      \"provider_state\": 1,\n"
        "      \"tick\": 0,\n"
        "      \"event_seq\": 0,\n"
        "      \"entity_ids\": [],\n"
        "      \"order_seq\": 0,\n"
        "      \"tick_is_mock\": true,\n"
        "      \"event_seq_is_mock\": true,\n"
        "      \"entity_ids_is_mock\": true,\n"
        "      \"order_seq_is_mock\": true\n"
        "    },\n"
        "    {\n"
        "      \"png\": \"frame_b.png\",\n"
        "      \"frame_index\": 1,\n"
        "      \"provider\": \"SELF_AUTHORED_MOCK (MockFrameProvider)\",\n"
        "      \"provider_call_seq\": %d,\n"
        "      \"provider_state\": 2,\n"
        "      \"tick\": 0,\n"
        "      \"event_seq\": 0,\n"
        "      \"entity_ids\": [],\n"
        "      \"order_seq\": 0,\n"
        "      \"tick_is_mock\": true,\n"
        "      \"event_seq_is_mock\": true,\n"
        "      \"entity_ids_is_mock\": true,\n"
        "      \"order_seq_is_mock\": true\n"
        "    }\n"
        "  ],\n"
        "  \"default_provider_frame\": {\n"
        "    \"png\": \"frame_default.png\",\n"
        "    \"provider\": \"YrDefaultFrameProvider (SELF_AUTHORED_ART, 编译期链接)\",\n"
        "    \"frame_index\": -1,\n"
        "    \"note\": \"未注入时默认 provider 的独立探针，不属 frame_a / frame_b 序列\"\n"
        "  },\n"
        "  \"round6_integration\": \"第六批集成器注入真实 yr_world_render / yr_input_dispatch 后，需在同一真实运行内把 事件→实体 ID→订单序号→tick→显示帧 串起来，并以 snapshot diff + 逐像素 diff 成对取证；本批仅为预留字段与 mock 占位值。\"\n"
        "}\n",
        fa_seq, fb_seq);
    std::fclose(f);
    return 1;
}

int main(void) {
    @autoreleasepool {
        const int W = 1280, H = 720;
        int failures = 0;

        /* 1) 未注入：shell 用编译期链接的默认 provider。 */
        YrShellSetDefaultState(7);
        int rcDefault = yr_shell_render_offscreen_png("frame_default.png", W, H);
        std::printf("DEFAULT_PROVIDER_RC=%d (path=frame_default.png)\n", rcDefault);
        if (rcDefault != 0) failures++;

        /* 2) 注入 SELF_AUTHORED_MOCK provider，两个不同状态各出一张 PNG。 */
        static int mock_state = 0;
        yr_shell_set_frame_provider(MockFrameProvider, &mock_state);

        YrShellResetPaddingViolations();
        mock_state = 1;
        int fa_provider_seq = g_mock_provider_calls + 1;
        int rcA = yr_shell_render_offscreen_png("frame_a.png", W, H);
        mock_state = 2;
        int fb_provider_seq = g_mock_provider_calls + 1;
        int rcB = yr_shell_render_offscreen_png("frame_b.png", W, H);
        std::printf("MOCK_PROVIDER_FRAME_A_RC=%d MOCK_PROVIDER_FRAME_B_RC=%d "
                    "MOCK_STATE_A=1 MOCK_STATE_B=2 "
                    "FRAME_A_PROVIDER_CALL_SEQ=%d FRAME_B_PROVIDER_CALL_SEQ=%d\n",
                    rcA, rcB, fa_provider_seq, fb_provider_seq);
        if (rcA != 0 || rcB != 0) failures++;

        /* 2b) 帧关联预留证据（mock 占位值，非真实世界状态）。 */
        int metaOk = WriteFrameMeta(fa_provider_seq, fb_provider_seq);
        std::printf("FRAME_META_WRITTEN=%d\n", metaOk);
        if (metaOk != 1) failures++;

        /* 3) 合成器内部 padding 校验（两次渲染的 padding 不得被覆写）。 */
        int composePadViol = YrShellPaddingViolations();
        std::printf("COMPOSE_PADDING_VIOLATIONS=%d\n", composePadViol);
        if (composePadViol != 0) failures++;

        /* 4) 显式 stride padding 校验（直接喂带 padding 的缓冲给 mock provider）。 */
        int explicitPad = YrShellCheckPadding(MockFrameProvider, &mock_state, W, H);
        std::printf("EXPLICIT_STRIDE_PADDING_OK=%d (stride=%zu, width*4=%d)\n",
                    explicitPad, (size_t)W * 4 + 16, W * 4);
        if (explicitPad != 1) failures++;

        /* 5) 注入 SELF_AUTHORED_MOCK event sink，验证投递链路。 */
        SinkLog log; log.count = 0; memset(&log.last, 0, sizeof(log.last));
        yr_shell_set_event_sink(MockSink, &log);
        yr_event probe;
        memset(&probe, 0, sizeof(probe));
        probe.type = YR_EVT_LEFT_DOWN; probe.x = 11; probe.y = 12;
        probe.modifiers = YR_MOD_SHIFT; probe.click_count = 2; probe.key_code = -1;
        YrShellDeliverEvent(&probe);
        int sinkOk = (log.count == 1 && log.last.type == YR_EVT_LEFT_DOWN &&
                      log.last.x == 11 && log.last.y == 12 &&
                      log.last.modifiers == YR_MOD_SHIFT);
        std::printf("MOCK_SINK_DELIVERED=%d SINK_COUNT=%d\n", sinkOk, log.count);
        if (!sinkOk) failures++;

        failures += TestExportFailures();
        std::printf("SELFTEST_FAILURES=%d\n", failures);
        return failures == 0 ? 0 : 1;
    }
}

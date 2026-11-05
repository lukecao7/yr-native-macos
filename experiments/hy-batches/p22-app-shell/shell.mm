/* shell.mm — P22 AppKit 上屏组件核心（离屏路径 / 事件映射 / 唯一真源合成器）
 *
 * 交付 yr_shell_*（shared/yr_slice.h）：
 *   yr_shell_set_frame_provider / yr_shell_set_event_sink / yr_shell_render_offscreen_png
 *
 * 边界：
 *   - 本文件是「按共享 C ABI 重建 + SELF_AUTHORED_ART 自制图案 + 取证校验 + 独立模拟」，
 *     不是《尤里的复仇》原版 exe 的导出接口，不代表原版操作语义等价。
 *   - 默认 frame provider 是本 shell 编译期链接的自制图案（SELF_AUTHORED_ART）；
 *     本 P22 不链接真实世界核心，第六批集成时由集成器注入 yr_world_render 替换。
 *   - 事件坐标映射属 SELF_AUTHORED_MAPPING。
 *   - 离屏路径（本文件全部函数）不创建 NSWindow / 不激活 / 不运行事件循环 / 不抢焦点。
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <CoreFoundation/CoreFoundation.h>

#include "shell_internal.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstdio>

/* ------------------------------------------------------------------ */
/* 注入回调（未注入时用编译期链接的默认 provider / 空 sink）            */
/* ------------------------------------------------------------------ */
static yr_frame_provider g_frame_provider = nullptr;
static void             *g_frame_userdata = nullptr;
static yr_event_sink     g_event_sink     = nullptr;
static void             *g_event_userdata = nullptr;
static int               g_default_state  = 1;   /* 默认 provider 的状态 */
static int               g_padding_violations = 0;

/* RGBA8888 允许的 stride padding（字节）。provider 不得写这些字节。 */
static const size_t kStridePadding = 16;

void yr_shell_set_frame_provider(yr_frame_provider fn, void *userdata) {
    g_frame_provider = fn;
    g_frame_userdata = userdata;
}

void yr_shell_set_event_sink(yr_event_sink fn, void *userdata) {
    g_event_sink = fn;
    g_event_userdata = userdata;
}

void YrShellSetDefaultState(int state) { g_default_state = state; }
int  YrShellGetDefaultState(void)      { return g_default_state; }

/* ------------------------------------------------------------------ */
/* 默认帧 provider：SELF_AUTHORED_ART 自制图案，按 state 变化           */
/* 只写每行前 width*4 字节，绝不触碰 stride padding。                    */
/* ------------------------------------------------------------------ */
int YrDefaultFrameProvider(uint8_t *rgba, int width, int height,
                           size_t stride, void *userdata) {
    if (!rgba || width <= 0 || height <= 0 || stride < (size_t)width * 4) return -1;
    int state = userdata ? *static_cast<int *>(userdata) : 1;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = rgba + (size_t)y * stride;
        for (int x = 0; x < width; ++x) {
            int cx = x / 24, cy = y / 24;
            int checker = (cx + cy) & 1;
            uint8_t r = (uint8_t)((x * 2 + state * 37) & 0xff);
            uint8_t g = (uint8_t)((y * 3 + state * 11 + (checker ? 64 : 0)) & 0xff);
            uint8_t b = (uint8_t)((cx * 17 + cy * 29 + state * 53) & 0xff);
            if (x % 24 == 0 || y % 24 == 0) { r = 0x10; g = 0x14; b = 0x18; }
            uint8_t *p = row + (size_t)x * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255; /* A=255 全不透明 */
        }
    }
    return width * height;
}

/* ------------------------------------------------------------------ */
/* 唯一真源：合成器 —— 窗口路径与离屏核验路径共用                        */
/* ------------------------------------------------------------------ */
CGImageRef YRShellComposeFrame(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    const size_t stride = (size_t)width * 4 + kStridePadding;
    const size_t bufsz  = stride * (size_t)height;
    std::vector<uint8_t> buf(bufsz, 0xCD); /* padding 哨兵 */

    yr_frame_provider fn = g_frame_provider ? g_frame_provider : YrDefaultFrameProvider;
    void *ud = g_frame_provider ? g_frame_userdata : &g_default_state;
    int r = fn(buf.data(), width, height, stride, ud);
    if (r < 0) return nullptr;

    /* padding 校验：provider 不得覆写每行 [width*4, stride) 区间。 */
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = buf.data() + (size_t)y * stride;
        for (size_t k = (size_t)width * 4; k < stride; ++k) {
            if (row[k] != 0xCD) ++g_padding_violations;
        }
    }

    /* 把带 stride 的 RGBA 缓冲包成 CGImage。
     * kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big => 内存字节序 R,G,B,A；
     * 图案 A=255，premultiplied 与 straight 等价，PNG 往返无颜色漂移。 */
    CFDataRef data = CFDataCreate(nullptr, buf.data(), (CFIndex)bufsz);
    if (!data) return nullptr;
    CGDataProviderRef prov = CGDataProviderCreateWithCFData(data);
    CFRelease(data);
    if (!prov) return nullptr;
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo binfo = kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big;
    CGImageRef img = CGImageCreate((size_t)width, (size_t)height, 8, 32, stride,
                                   cs, binfo, prov, nullptr, false,
                                   kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);
    return img;
}

/* 唯一真源：绘制函数 —— 把一帧画进任意 CGContext。
 * 窗口 drawRect: 与 yr_shell_render_offscreen_png 都只调用本函数。 */
void YRShellDrawFrameIntoContext(CGContextRef ctx, int width, int height) {
    if (!ctx || width <= 0 || height <= 0) return;
    CGImageRef img = YRShellComposeFrame(width, height);
    if (!img) return;
    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)width, (CGFloat)height), img);
    CGImageRelease(img);
}

int  YrShellPaddingViolations(void)      { return g_padding_violations; }
void YrShellResetPaddingViolations(void) { g_padding_violations = 0; }

int YrShellCheckPadding(yr_frame_provider fn, void *userdata, int width, int height) {
    if (!fn || width <= 0 || height <= 0) return 0;
    const size_t stride = (size_t)width * 4 + kStridePadding;
    std::vector<uint8_t> buf(stride * (size_t)height, 0xCD);
    if (fn(buf.data(), width, height, stride, userdata) < 0) return 0;
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = buf.data() + (size_t)y * stride;
        for (size_t k = (size_t)width * 4; k < stride; ++k) {
            if (row[k] != 0xCD) return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* 离屏核验入口：CGBitmapContext 出 PNG，不建窗、不激活、不抢焦点       */
/* ------------------------------------------------------------------ */
int yr_shell_render_offscreen_png(const char *png_path, int width, int height) {
    if (!png_path || width <= 0 || height <= 0) return -1;
    /* 目标上下文：CGBitmapContext（离屏）。使用被最广泛支持的 BGRA 后备格式，
     * 由 CG 负责颜色/字节序转换，最终 PNG 颜色不受影响。 */
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo binfo = kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little;
    CGContextRef bctx = CGBitmapContextCreate(nullptr, (size_t)width, (size_t)height,
                                              8, 0, cs, binfo);
    CGColorSpaceRelease(cs);
    if (!bctx) return -2;

    YRShellDrawFrameIntoContext(bctx, width, height); /* 唯一真源绘制函数 */
    CGContextFlush(bctx);
    CGImageRef outImg = CGBitmapContextCreateImage(bctx);
    CGContextRelease(bctx);
    if (!outImg) return -3;

    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, (const UInt8 *)png_path, (CFIndex)strlen(png_path), false);
    if (!url) { CGImageRelease(outImg); return -4; }
    /* UTI 用字符串常量，避免额外链接 CoreServices / UniformTypeIdentifiers。 */
    CGImageDestinationRef dest = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
    CFRelease(url);
    if (!dest) { CGImageRelease(outImg); return -5; }
    CGImageDestinationAddImage(dest, outImg, nullptr);
    bool ok = CGImageDestinationFinalize(dest);
    CFRelease(dest);
    CGImageRelease(outImg);
    return ok ? 0 : -6;
}

/* ------------------------------------------------------------------ */
/* NSEvent -> yr_event 映射（SELF_AUTHORED_MAPPING）+ 事件投递           */
/* ------------------------------------------------------------------ */
yr_event YrEventFromNSEvent(NSEvent *e, int cell_px, int view_h_px) {
    yr_event ev;
    memset(&ev, 0, sizeof(ev));
    if (!e || cell_px <= 0) return ev;
    NSEventType t = [e type];
    switch (t) {
        case NSEventTypeLeftMouseDown:    ev.type = YR_EVT_LEFT_DOWN;  break;
        case NSEventTypeLeftMouseUp:      ev.type = YR_EVT_LEFT_UP;    break;
        case NSEventTypeLeftMouseDragged: ev.type = YR_EVT_LEFT_DRAG;  break;
        case NSEventTypeRightMouseDown:   ev.type = YR_EVT_RIGHT_DOWN; break;
        case NSEventTypeRightMouseUp:     ev.type = YR_EVT_RIGHT_UP;   break;
        case NSEventTypeKeyDown:          ev.type = YR_EVT_KEY_DOWN;   break;
        case NSEventTypeKeyUp:            ev.type = YR_EVT_KEY_UP;     break;
        default:                          ev.type = 0;                 break;
    }
    NSEventModifierFlags f = [e modifierFlags];
    ev.modifiers = 0;
    if (f & NSEventModifierFlagShift)   ev.modifiers |= YR_MOD_SHIFT;
    if (f & NSEventModifierFlagControl) ev.modifiers |= YR_MOD_CONTROL;
    if (f & NSEventModifierFlagOption)  ev.modifiers |= YR_MOD_ALT;

    /* 窗口坐标（左下原点）-> cell 坐标（左上原点）：x/cell，y 翻转后 /cell。 */
    NSPoint p = [e locationInWindow];
    ev.x = (int)(p.x / cell_px);
    ev.y = (int)(((CGFloat)view_h_px - p.y) / cell_px);
    if (ev.x < 0) ev.x = 0;
    if (ev.y < 0) ev.y = 0;

    bool is_mouse = (ev.type >= YR_EVT_LEFT_DOWN && ev.type <= YR_EVT_RIGHT_UP);
    bool is_key   = (ev.type == YR_EVT_KEY_DOWN || ev.type == YR_EVT_KEY_UP);
    ev.click_count = is_mouse ? (int)[e clickCount] : 0;
    ev.key_code    = is_key ? (int)[e keyCode] : -1;
    return ev;
}

int YrShellDeliverEvent(const yr_event *ev) {
    if (!ev) return 0;
    if (g_event_sink) return g_event_sink(ev, g_event_userdata);
    return 0; /* 未注入时丢弃，不伪造 */
}

int YrShellHandleNSEvent(NSEvent *e, int cell_px, int view_h_px) {
    yr_event ev = YrEventFromNSEvent(e, cell_px, view_h_px);
    return YrShellDeliverEvent(&ev);
}

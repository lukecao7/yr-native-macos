/* window_path.mm — P22 窗口路径（AppKit / NSView / CGContext）
 *
 * 说明（务必连同 LIMITATIONS.md 一起读）：
 *   - 本文件提供真实 NSWindow + NSView(NSResponder) + CGContext 上屏链路，
 *     其 drawRect: 与离屏核验路径【共用同一个】 YRShellDrawFrameIntoContext。
 *   - 各 NSResponder 方法把 NSEvent 交给 YrShellHandleNSEvent（与自检共用）。
 *   - 但按安全约束：窗口只被「构建」，从不运行事件循环、从不激活、从不前置、
 *     从不抢焦点。make check 不调用本路径；真实窗口呈现属 NOT_ACHIEVED，
 *     由用户自行决定何时以 --gui 显式启动并人工核验。
 *   - 本目录一切内容为重建 + 自制图案 + 独立模拟，不链接原版 exe。
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include "shell_internal.h"

static const int kCellPx = 24; /* SELF_AUTHORED_MAPPING: 每 cell 24 像素 */

@interface YRShellView : NSView
@end

@implementation YRShellView

- (BOOL)isFlipped { return NO; } /* 与 CGBitmapContext 一致：左下原点，两路径同一绘制函数 */

- (BOOL)acceptsFirstResponder { return YES; }

/* 唯一真源绘制函数：窗口与离屏 PNG 都调用它。 */
- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    int w = (int)self.bounds.size.width;
    int h = (int)self.bounds.size.height;
    int rc = YRShellDrawFrameIntoContext(ctx, w, h);
    if (rc != 0) NSLog(@"YRShell frame drawing failed rc=%d", rc);
}

/* NSResponder -> yr_event，经 event sink 送出（与自检共用同一函数）。 */
- (void)mouseDown:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)mouseDragged:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)mouseUp:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)rightMouseDown:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)rightMouseUp:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)keyDown:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}
- (void)keyUp:(NSEvent *)e {
    YrShellHandleNSEvent(e, kCellPx, (int)self.bounds.size.height);
}

@end

/* 仅 --gui 显式调用：构建窗口对象，不运行、不激活、不前置、不抢焦点。 */
void YrShellBuildWindow(void) {
    @autoreleasepool {
        (void)[NSApplication sharedApplication];
        NSRect frame = NSMakeRect(0, 0, 1280, 720);
        NSUInteger mask = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                          NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
        NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                   styleMask:mask
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
        [win setTitle:@"YuriSlice (P22 diagnostics)"];
        YRShellView *view = [[YRShellView alloc] initWithFrame:frame];
        [win setContentView:view];
        /* 刻意不运行事件循环、不激活 App、不前置窗口：避免抢焦点。
         * 真实窗口是否可见/像素是否正确：NOT_ACHIEVED（未人工验证）。 */
        (void)win;
    }
}

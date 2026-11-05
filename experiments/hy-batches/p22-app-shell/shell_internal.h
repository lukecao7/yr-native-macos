/* shell_internal.h — P22 shell 内部接口（本目录私有，不进 shared/）
 *
 * 唯一真源：YRShellComposeFrame / YRShellDrawFrameIntoContext 是窗口路径与
 * 离屏核验路径共用的合成器 / 绘制函数。窗口 drawRect: 与离屏 PNG 都只调用它俩。
 *
 * 本文件以及全部实现是「按共享 C ABI 重建 + 自制图案 + 取证校验 + 独立模拟」，
 * 不是《尤里的复仇》原版 exe 的导出接口，不代表原版操作语义等价。
 */
#ifndef SHELL_INTERNAL_H
#define SHELL_INTERNAL_H

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
/* 共享 ABI 头由主对话维护，本目录只读引用（-I../shared），不复制、不改动。 */
#include "yr_slice.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 编译期链接的默认帧 provider：SELF_AUTHORED_ART 自制图案，按 state 变化。
 * 集成后由集成器注入 yr_world_render 取代本默认（本 P22 不链接 P20）。 */
int  YrDefaultFrameProvider(uint8_t *rgba, int width, int height, size_t stride, void *userdata);
void YrShellSetDefaultState(int state);
int  YrShellGetDefaultState(void);

/* ---- 唯一真源：合成器 / 绘制函数 ---- */
/* 取当前帧 provider（未注入则用默认 provider）填带 padding 的 RGBA 缓冲，
 * 校验 padding 未被覆写，再包成 CGImage。窗口与离屏共用。 */
CGImageRef YRShellComposeFrame(int width, int height);
/* 把一帧画进任意 CGContext（NSGraphicsContext 或 CGBitmapContext）。
 * 窗口 drawRect: 与 yr_shell_render_offscreen_png 都调用本函数。
 * 返回 0 成功；-1 参数错误；-2 provider 或图像合成失败，未绘制。 */
int YRShellDrawFrameIntoContext(CGContextRef ctx, int width, int height);

/* stride padding 校验（RGBA8888 允许 stride > width*4，padding 不得被覆写）。 */
int YrShellPaddingViolations(void);
void YrShellResetPaddingViolations(void);
int  YrShellCheckPadding(yr_frame_provider fn, void *userdata, int width, int height);

/* NSEvent -> yr_event 映射（SELF_AUTHORED_MAPPING）+ 事件投递。
 * NSResponder 各方法与自检共用 YrShellHandleNSEvent，避免两套语义。 */
yr_event YrEventFromNSEvent(NSEvent *e, int cell_px, int view_h_px);
int      YrShellDeliverEvent(const yr_event *ev);
int      YrShellHandleNSEvent(NSEvent *e, int cell_px, int view_h_px);

/* 窗口路径：创建 NSApplication / NSWindow / YRView 并接入共享合成器。
 * 仅 --gui 显式调用；不运行事件循环、不激活、不抢焦点。
 * make check 从不调用本函数，真实窗口呈现属 NOT_ACHIEVED。 */
void YrShellBuildWindow(void);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_INTERNAL_H */

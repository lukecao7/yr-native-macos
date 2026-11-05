/* main.mm — YuriSlice.app/Contents/MacOS/YuriSlice 入口
 *
 * 安全默认值：不带参数运行【只打印用法并退出】，绝不弹窗、绝不激活、绝不抢焦点。
 *   --offscreen <png> <w> <h> <state> : 纯离屏 CGBitmapContext 出 PNG（headless）
 *   --gui                             : 仅构建窗口对象（不运行/激活/前置），
 *                                       真实窗口未经人工验证 (NOT_ACHIEVED)
 *
 * 本可执行文件把 libyrshell.a 链进来，故 nm -gU 可见全部 yr_shell_* 符号。
 * 一切内容为重建 + 自制图案 + 独立模拟，不链接原版 exe。
 */
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#include "shell_internal.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void PrintUsage(const char *prog) {
    std::fprintf(stderr,
        "YuriSlice (P22 app-shell, diagnostic build)\n"
        "  %s --offscreen <png> <w> <h> <state>  : headless offscreen PNG\n"
        "  %s --gui                             : build window objects only\n"
        "  (no args)                            : print this usage, exit 0\n"
        "NOTE: default (no args) never launches a GUI / never steals focus.\n",
        prog, prog);
}

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        if (argc >= 2 && std::strcmp(argv[1], "--offscreen") == 0) {
            if (argc != 6) { PrintUsage(argv[0]); return 2; }
            const char *png   = argv[2];
            int w = std::atoi(argv[3]);
            int h = std::atoi(argv[4]);
            int state = std::atoi(argv[5]);
            YrShellSetDefaultState(state);
            int rc = yr_shell_render_offscreen_png(png, w, h);
            if (rc != 0) std::fprintf(stderr, "offscreen render failed rc=%d\n", rc);
            return rc == 0 ? 0 : 1;
        }
        if (argc >= 2 && std::strcmp(argv[1], "--gui") == 0) {
            /* 用户显式动作；make check 从不进入此分支。构建窗口对象但不运行，
             * 不激活、不前置，避免抢焦点。真实窗口呈现未验证 (NOT_ACHIEVED)。 */
            YrShellBuildWindow();
            std::fprintf(stderr,
                "window objects built (no event loop / no activation / no focus steal); "
                "real window NOT manually verified (NOT_ACHIEVED)\n");
            return 0;
        }
        /* 默认 headless，安全。 */
        PrintUsage(argv[0]);
        return 0;
    }
}

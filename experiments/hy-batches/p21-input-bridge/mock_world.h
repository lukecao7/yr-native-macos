/* SELF_AUTHORED_MOCK —— P21 自检专用最小世界核心桩，不属于 shared/yr_slice.h 的共享 ABI。
 *
 * 职责：只记录 yr_input_dispatch 对 yr_world_* 的调用（函数名 + 参数），不做任何游戏逻辑。
 * 边界：本桩「未与真实世界核心（p20-world-core 的 libyrworld.a）链接，将在第六批集成」时被替换。
 *       它是重建 + 独立模拟，不是《尤里的复仇》原版 exe 的导出接口。
 */
#ifndef MOCK_WORLD_H
#define MOCK_WORLD_H

#include <string>
#include <vector>
#include "../shared/yr_slice.h"

struct MockCall {
  std::string func;        /* yr_world_* 函数名 */
  std::vector<int> args;   /* 调用参数（整数坐标/标志） */
  int ret;                /* 桩的确定性返回值 */
};

/* 仅自检用的辅助接口（C++ linkage，不进 libyrinput.a，不进共享 ABI）。 */
yr_world *yr_mock_world_handle(void);            /* 哑 yr_world* 句柄 */
int yr_mock_world_log_size(void);                /* 已记录调用数 */
const MockCall *yr_mock_world_log_at(int i);     /* 第 i 条调用，越界返回 nullptr */
void yr_mock_world_log_clear(void);

#endif /* MOCK_WORLD_H */

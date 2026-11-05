/* SELF_AUTHORED_MOCK —— 只记录 yr_world_* 调用名与参数、不做任何游戏逻辑的最小桩。
 * 未与真实世界核心（p20-world-core / libyrworld.a）链接，将在第六批集成时替换。
 *
 * 说明：三个 yr_world_* 函数均返回确定性常量 3，仅用于让桥接层状态机可重放
 *       （让 RIGHT_DOWN 的「已有选中」查询分支可达）。这不表示任何真实选中/下令结果，
 *       也绝不伪造世界状态。真实选中数 / 单位数由第六批的真实世界核心提供。
 */
#include "mock_world.h"

struct yr_world { int dummy; };   /* 哑定义，桥接层只传指针、不解引用 */
static yr_world g_world;
static std::vector<MockCall> g_log;

extern "C" int yr_world_selected_count(const yr_world *) {
  g_log.push_back(MockCall{"yr_world_selected_count", {}, 3});
  return 3;
}

extern "C" int yr_world_select_box(yr_world *, int x0, int y0, int x1, int y1, int additive) {
  g_log.push_back(MockCall{"yr_world_select_box", {x0, y0, x1, y1, additive}, 3});
  return 3;
}

extern "C" int yr_world_order_move(yr_world *, int x, int y) {
  g_log.push_back(MockCall{"yr_world_order_move", {x, y}, 3});
  return 3;
}

yr_world *yr_mock_world_handle(void) { return &g_world; }
int yr_mock_world_log_size(void) { return static_cast<int>(g_log.size()); }
const MockCall *yr_mock_world_log_at(int i) {
  if (i < 0 || i >= static_cast<int>(g_log.size())) return nullptr;
  return &g_log[i];
}
void yr_mock_world_log_clear(void) { g_log.clear(); }

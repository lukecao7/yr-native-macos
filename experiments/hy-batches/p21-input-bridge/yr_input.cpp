/* p21-input-bridge —— 输入事件 → 世界 API 的翻译层（libyrinput.a）。
 *
 * 本质：按 shared/yr_slice.h 声明重建的自制适配层 + 取证/哈希校验 + 独立模拟，
 *       不是链接《尤里的复仇》原版 exe，不代表原版 Windows 操作语义等价。
 * 解耦：本文件只依赖 shared/yr_slice.h 的 yr_world_* 声明，不依赖 p20-world-core 实现。
 *       自检时 yr_world_* 符号由 SELF_AUTHORED_MOCK（mock_world.cpp）提供；
 *       「未与真实世界核心链接，将在第六批集成」时由真实 libyrworld.a 替换。
 *
 * 语义严格按 yr_slice.h 中 yr_input_dispatch 的注释：
 *   LEFT_DOWN 起框、LEFT_DRAG 更新框、LEFT_UP 提交框选（Shift → additive=1）、
 *   RIGHT_DOWN 若已有选中则对落点下令移动。
 *   未实现 / 证据不足的事件返回 -1，且不得伪造世界状态变化。
 */
#include "../shared/yr_slice.h"
#include <cstdio>
#include <cstring>

struct yr_input_state {
  int dragging;       /* 1 = 已 LEFT_DOWN 起框、尚未 LEFT_UP 提交 */
  int x0, y0, x1, y1;/* 待提交选择框端点（端点包含、自动交由世界核心归一化） */
};

extern "C" yr_input_state *yr_input_state_create(void) {
  yr_input_state *s = new yr_input_state;
  yr_input_state_reset(s);
  return s;
}

extern "C" void yr_input_state_destroy(yr_input_state *s) {
  delete s;
}

extern "C" void yr_input_state_reset(yr_input_state *s) {
  if (!s) return;
  s->dragging = 0;
  s->x0 = s->y0 = s->x1 = s->y1 = 0;
}

/* 供回放与核验：把 state 当前待提交的选择框写成 JSON，未起框时严格写 "{}"，
 * cap 不足返回 -2 且 out 写空字符串（按 yr_slice.h 通用约定）。 */
extern "C" int yr_input_state_json(const yr_input_state *s, char *out, size_t cap) {
  if (!out || cap == 0) return -2;
  out[0] = '\0';
  if (!s) return 0;
  if (!s->dragging) {
    if (cap < 3) return -2;
    std::snprintf(out, cap, "{}");
    return 2;
  }
  char buf[256];
  int n = std::snprintf(buf, sizeof(buf),
      "{\"dragging\":1,\"x0\":%d,\"y0\":%d,\"x1\":%d,\"y1\":%d}",
      s->x0, s->y0, s->x1, s->y1);
  if (n < 0) return -1;
  if (static_cast<size_t>(n) >= cap) return -2;  /* out 保持空字符串 */
  std::memcpy(out, buf, static_cast<size_t>(n) + 1);
  return n;
}

extern "C" int yr_input_dispatch(yr_world *w, const yr_event *ev, yr_input_state *s) {
  if (!w || !ev || !s) return -1;

  switch (ev->type) {
    case YR_EVT_LEFT_DOWN:
      /* 起框：记录框起点，同时把终点初始化为起点（点框）。不产生世界调用。 */
      if (s->dragging) return -1;            /* 重复起框：状态机非法 */
      s->dragging = 1;
      s->x0 = s->x1 = ev->x;
      s->y0 = s->y1 = ev->y;
      return 0;

    case YR_EVT_LEFT_DRAG:
      /* 更新框：只改终点，不提交。不产生世界调用。 */
      if (!s->dragging) return -1;           /* 未起框就拖拽：状态机非法 */
      s->x1 = ev->x;
      s->y1 = ev->y;
      return 0;

    case YR_EVT_LEFT_UP: {
      /* 提交框选：端点取 state 中记录的框，Shift 修饰 → additive=1（并入已有选择）。 */
      if (!s->dragging) return -1;
      // 最后一个 drag 事件可能早于释放位置，以 mouseUp 的实际终点提交。
      s->x1 = ev->x;
      s->y1 = ev->y;
      int additive = (ev->modifiers & YR_MOD_SHIFT) ? 1 : 0;
      yr_world_select_box(w, s->x0, s->y0, s->x1, s->y1, additive);
      s->dragging = 0;
      return 1;                              /* 本次产生 1 次世界 API 调用 */
    }

    case YR_EVT_RIGHT_DOWN: {
      /* 若已有选中则对落点下令移动。
       * 「已有选中」通过 yr_world_selected_count(w) 向世界查询，桥接层不自行记账、
       * 不伪造选中状态；无选中时只查询、不下令（yr_world_order_move 自身亦保证
       * 无选中返回 0 且不以成功计费）。 */
      int n = yr_world_selected_count(w);
      if (n <= 0) return 1;                  /* 仅 1 次查询调用，无下令 */
      yr_world_order_move(w, ev->x, ev->y);
      return 2;                              /* 查询 + 下令，共 2 次调用 */
    }

    case YR_EVT_RIGHT_UP:
      /* yr_slice.h 未定义 RIGHT_UP 语义，证据不足：返回 -1、不产生世界调用。 */
      return -1;

    case YR_EVT_KEY_DOWN:
    case YR_EVT_KEY_UP:
      /* 键盘操作在本桥接层无证据 / 无语义（P18: KeyDown/FlagsChanged 一律 unported）：
       * 返回 -1、不伪造任何世界状态变化。 */
      return -1;

    default:
      /* 未知 / 非法 type：返回 -1、不产生世界调用。 */
      return -1;
  }
}

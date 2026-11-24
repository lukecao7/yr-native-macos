/* p21-input-bridge —— 输入事件 → 世界 API 的翻译层（libyrinput.a）。
 *
 * 本质：按 shared/yr_slice.h 声明重建的自制适配层 + 取证/哈希校验 + 独立模拟，
 *       不是链接《尤里的复仇》原版 exe，不代表原版 Windows 操作语义等价。
 * 解耦：本文件只依赖 shared/yr_slice.h 的 yr_world_* 声明，不依赖 p20-world-core 实现。
 *       接口用例通过 mock_world.cpp 核对调用，world_input_regression 链接 P20 验证实际世界。
 *
 * 左键拖框、点选单位或向选中单位下达移动命令。右击清选，右拖向宿主逐段传递卷屏位移并保留选中。
 */
#include "../shared/yr_slice.h"
#include <cstdio>
#include <cstring>

struct yr_input_state {
  bool right_pressed;
  bool right_dragged;
  int right_x, right_y;
  yr_input_pan_sink pan_sink;
  void *pan_userdata;
  int dragging;       /* 1 = 已 LEFT_DOWN 起框、尚未 LEFT_UP 提交 */
  int x0, y0, x1, y1;/* 待提交选择框端点（端点包含、自动交由世界核心归一化） */
};

extern "C" yr_input_state *yr_input_state_create(void) {
  yr_input_state *s = new yr_input_state{};
  yr_input_state_reset(s);
  return s;
}

extern "C" void yr_input_state_destroy(yr_input_state *s) {
  delete s;
}

extern "C" void yr_input_state_reset(yr_input_state *s) {
  if (!s) return;
  s->dragging = 0;
  s->right_pressed = false;
  s->right_dragged = false;
  s->x0 = s->y0 = s->x1 = s->y1 = 0;
}

extern "C" void yr_input_set_pan_sink(yr_input_state *s, yr_input_pan_sink sink, void *userdata) {
  if (!s) return;
  s->pan_sink = sink;
  s->pan_userdata = userdata;
}

static void update_pan(yr_input_state *s, int x, int y) {
  const int64_t dx = static_cast<int64_t>(x) - s->right_x;
  const int64_t dy = static_cast<int64_t>(y) - s->right_y;
  s->right_x = x;
  s->right_y = y;
  if (!dx && !dy) return;
  s->right_dragged = true;
  if (s->pan_sink) s->pan_sink(dx, dy, s->pan_userdata);
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
      if (s->dragging || s->right_pressed) return -1;            /* 重复起框：状态机非法 */
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
      if (!s->dragging) return -1;
      s->dragging = 0;
      const bool shift = (ev->modifiers & YR_MOD_SHIFT) != 0;
      if (s->x0 != ev->x || s->y0 != ev->y) {
        yr_world_select_box(w, s->x0, s->y0, ev->x, ev->y, shift);
        return 1;
      }
      if (yr_world_select_at(w, ev->x, ev->y, shift)) return 1;
      yr_world_order_move(w, ev->x, ev->y);
      return 2;
    }

    case YR_EVT_RIGHT_DOWN:
      if (s->dragging || s->right_pressed) return -1;
      s->right_pressed = true;
      s->right_dragged = false;
      s->right_x = ev->x;
      s->right_y = ev->y;
      return 0;

    case YR_EVT_RIGHT_DRAG:
      if (!s->right_pressed) return -1;
      s->right_dragged = true;
      update_pan(s, ev->x, ev->y);
      return 0;

    case YR_EVT_RIGHT_UP:
      if (!s->right_pressed) return -1;
      s->right_pressed = false;
      update_pan(s, ev->x, ev->y);
      if (s->right_dragged) return 0;
      yr_world_clear_selection(w);
      return 1;

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

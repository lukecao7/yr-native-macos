// SELF_AUTHORED_MOCK tests only the input adapter, without a game world.
#include "mock_world.h"
#include <cassert>
#include <cstdio>
#include <vector>
#include <utility>
int main() {
  auto* s = yr_input_state_create();
  auto* w = yr_mock_world_handle();
  yr_event e{YR_EVT_LEFT_DOWN, 1, 2, 0, 1, -1};
  assert(yr_input_dispatch(w, &e, s) == 0);
  e = {YR_EVT_LEFT_DRAG, 3, 4, 0, 1, -1};
  assert(yr_input_dispatch(w, &e, s) == 0);
  e = {YR_EVT_LEFT_UP, 8, 9, YR_MOD_SHIFT, 1, -1};
  assert(yr_input_dispatch(w, &e, s) == 1);
  assert(yr_mock_world_log_size() == 1);
  assert(yr_mock_world_log_at(0)->args == std::vector<int>({1, 2, 8, 9, 1}));
  assert(yr_input_dispatch(w, &e, s) == -1);
  assert(yr_mock_world_log_size() == 1);
  auto send = [&](int type, int x, int y, int modifiers = 0) {
    const yr_event event{type, x, y, modifiers, 1, -1};
    return yr_input_dispatch(w, &event, s);
  };
  auto last = [&]() { return yr_mock_world_log_at(yr_mock_world_log_size() - 1); };
  assert(send(YR_EVT_LEFT_DOWN, 5, 5) == 0 && send(YR_EVT_LEFT_UP, 5, 5) == 1);
  assert(last()->func == "yr_world_select_at" && last()->ret == 1);
  for (int expected : {2, 1}) {
    assert(send(YR_EVT_LEFT_DOWN, 7, 7, YR_MOD_SHIFT) == 0);
    assert(send(YR_EVT_LEFT_UP, 7, 7, YR_MOD_SHIFT) == 1);
    assert(send(YR_EVT_LEFT_DOWN, 30, 40) == 0 && send(YR_EVT_LEFT_UP, 30, 40) == 2);
    assert(last()->func == "yr_world_order_move" && last()->ret == expected);
  }
  using Pan = std::pair<int64_t, int64_t>;
  std::vector<Pan> deltas;
  yr_input_set_pan_sink(s, [](int64_t dx, int64_t dy, void* user) {
    static_cast<std::vector<Pan>*>(user)->emplace_back(dx, dy);
  }, &deltas);
  const auto before = yr_mock_world_log_size();
  assert(send(YR_EVT_RIGHT_DRAG, 30, 40) == -1);
  assert(send(YR_EVT_RIGHT_DOWN, 30, 40) == 0);
  assert(send(YR_EVT_RIGHT_DRAG, 34, 43) == 0);
  assert(send(YR_EVT_RIGHT_DRAG, 32, 41) == 0);
  assert(send(YR_EVT_RIGHT_UP, 30, 40) == 0);
  assert((deltas == std::vector<Pan>{{4, 3}, {-2, -2}, {-2, -1}}));
  assert(yr_mock_world_log_size() == before);
  assert(send(YR_EVT_RIGHT_DOWN, 30, 40) == 0 && send(YR_EVT_RIGHT_DRAG, 30, 40) == 0);
  assert(send(YR_EVT_RIGHT_UP, 30, 40) == 0 && yr_mock_world_log_size() == before);
  assert(send(YR_EVT_RIGHT_DOWN, 30, 40) == 0 && send(YR_EVT_RIGHT_UP, 34, 40) == 0);
  assert((deltas.back() == Pan{4, 0}));
  assert(send(YR_EVT_RIGHT_DOWN, 30, 40) == 0 && yr_mock_world_log_size() == before);
  assert(send(YR_EVT_RIGHT_UP, 30, 40) == 1 && last()->func == "yr_world_clear_selection");
  assert(send(YR_EVT_RIGHT_UP, 30, 40) == -1);
  assert(send(YR_EVT_LEFT_DOWN, 30, 40) == 0 && send(YR_EVT_LEFT_UP, 30, 40) == 2);
  assert(last()->func == "yr_world_order_move" && last()->ret == 0);
  yr_input_state_destroy(s);
  std::puts("PASS input regression: marquee endpoint, left click, Shift toggle, movement, right release and pan out and back");
}

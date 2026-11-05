// SELF_AUTHORED_MOCK tests only the input adapter, without a game world.
#include "mock_world.h"
#include <cassert>
#include <cstdio>
#include <vector>
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
  yr_input_state_destroy(s);
  std::puts("PASS input regression: mouse-up endpoint, Shift, unmatched release");
}

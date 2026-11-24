// SPDX-License-Identifier: GPL-3.0-only
// SELF_AUTHORED one-object fixture linking the input bridge and world libraries.
#include "yr_slice.h"
#include "yr_util.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

int main() {
  char path[] = "/tmp/yr-world-input-XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  assert(yr::write_file(path, R"({"width":3,"height":1,"source":"SELF_AUTHORED","tiles":[{"x":0,"y":0,"tile":0},{"x":1,"y":0,"tile":0},{"x":2,"y":0,"tile":0}]})"));
  yr_world* world = nullptr;
  assert(yr_world_create(&world, path, 1, 7) == 0);
  std::remove(path);
  yr_input_state* state = yr_input_state_create();
  const auto snapshot = [&]() {
    char bytes[1024];
    assert(yr_world_snapshot_json(world, bytes, sizeof bytes) > 0);
    return std::string(bytes);
  };
  const auto unit = [&]() { return yr::json_parse(snapshot()).get("units")->arr.front(); };
  const auto send = [&](int type, int x, int y = 0) {
    const yr_event event{type, x, y, 0, 1, -1};
    return yr_input_dispatch(world, &event, state);
  };
  const int start = static_cast<int>(unit().get("x")->num);
  const int target = start == 0 ? 2 : 0;
  assert(send(YR_EVT_LEFT_DOWN, start) == 0 && send(YR_EVT_LEFT_UP, start) == 1);
  assert(yr_world_selected_count(world) == 1);
  assert(send(YR_EVT_LEFT_DOWN, target) == 0 && send(YR_EVT_LEFT_UP, target) == 2);
  assert(unit().get("mission")->str == "MOVE");

  using Delta = std::pair<int64_t, int64_t>;
  std::vector<Delta> deltas;
  yr_input_set_pan_sink(state, [](int64_t dx, int64_t dy, void* data) {
    static_cast<std::vector<Delta>*>(data)->emplace_back(dx, dy);
  }, &deltas);
  const auto ordered = snapshot();
  assert(send(YR_EVT_RIGHT_DOWN, 0) == 0 && send(YR_EVT_RIGHT_DRAG, 3, 1) == 0);
  assert(send(YR_EVT_RIGHT_UP, 0) == 0);
  assert((deltas == std::vector<Delta>{{3, 1}, {-3, -1}}));
  assert(snapshot() == ordered && yr_world_selected_count(world) == 1);
  const int ticks_to_arrival = std::abs(target - start);
  assert(yr_world_step(world, ticks_to_arrival, 1) == ticks_to_arrival);
  assert(unit().get("x")->num == target && unit().get("y")->num == 0);
  assert(unit().get("mission")->str == "GUARD");
  assert(send(YR_EVT_RIGHT_DOWN, target) == 0 && send(YR_EVT_RIGHT_UP, target) == 1);
  assert(yr_world_selected_count(world) == 0 && unit().get("x")->num == target);
  yr_input_state_destroy(state);
  yr_world_destroy(world);
  std::printf("PASS real world: selected first object at (%d,0), reached (%d,0), preserved orders through pan, cleared selection with a click\n", start, target);
}

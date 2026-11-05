// SELF_AUTHORED regression fixtures; no game files or original game rules.
#include "yr_slice.h"
#include "yr_util.h"
#include <cassert>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

int main() {
  const char* path = "regression_grid.json";
  const std::string grid = R"({"width":2,"height":2,"source":"SELF_AUTHORED","tiles":[{"x":0,"y":0,"tile":0},{"x":1,"y":0,"tile":0},{"x":0,"y":1,"tile":0},{"x":1,"y":1,"tile":0}]})";
  assert(yr::write_file(path, grid));
  yr_world* w = nullptr;
  assert(yr_world_create(&w, path, 5, 7) == -4 && !w);
  assert(yr_world_create(&w, path, 4, 7) == 0);
  assert(yr_world_snapshot_json(w, nullptr, 10) == -2);
  std::vector<char> buf(10000);
  for (int threads : {1, 4}) {
    for (int tick = 0; tick < 30; ++tick) {
      assert(yr_world_step(w, 1, threads) == 1);
      assert(yr_world_snapshot_json(w, buf.data(), buf.size()) > 0);
      const auto j = yr::json_parse(buf.data());
      std::set<std::pair<int, int>> cells;
      for (const auto& u : j.get("units")->arr)
        cells.emplace((int)u.get("x")->num, (int)u.get("y")->num);
      assert(cells.size() == 4);
    }
  }
  yr_world_destroy(w);
  assert(yr::write_file(path, R"({"width":2,"height":2,"tiles":[{"x":0,"y":0,"tile":0}]})"));
  assert(yr_world_create(&w, path, 1, 7) == -1 && !w);
  assert(yr::write_file(path, R"({"width":1,"height":1,"tiles":[{"x":0,"y":0,"tile":0},{"x":0,"y":0,"tile":0}]})"));
  assert(yr_world_create(&w, path, 1, 7) == -1 && !w);
  std::remove(path);
  std::puts("PASS world regression: capacity, malformed grids, null output, occupied-cell arbitration (1/4 threads)");
}

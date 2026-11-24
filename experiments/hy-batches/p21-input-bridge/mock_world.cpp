/* SELF_AUTHORED_MOCK 仅记录输入桥的调用。三个固定位置用于区分单位与空地，
 * 桩保留选择标记，不推进游戏逻辑。真实应用使用自己的世界核心。 */
#include "mock_world.h"

#include <algorithm>
#include <array>

struct MockUnit { int x, y; bool selected = false; };
struct yr_world { std::array<MockUnit, 3> units{{{5,5},{7,7},{12,12}}}; };
static yr_world g_world;
static std::vector<MockCall> g_log;

static int selected_count(const yr_world* world) {
  return static_cast<int>(std::count_if(world->units.begin(), world->units.end(),
    [](const auto& unit) { return unit.selected; }));
}

extern "C" int yr_world_select_at(yr_world* world, int x, int y, int toggle) {
  const auto hit = std::find_if(world->units.begin(), world->units.end(), [x, y](const auto& unit) {
    return unit.x == x && unit.y == y;
  });
  const bool found = hit != world->units.end();
  if (found) {
    if (!toggle) for (auto& unit : world->units) unit.selected = false;
    hit->selected = !toggle || !hit->selected;
  }
  g_log.push_back({"yr_world_select_at", {x, y, toggle}, found});
  return found;
}

extern "C" int yr_world_select_box(yr_world* world, int x0, int y0, int x1, int y1, int additive) {
  for (auto& unit : world->units) {
    const bool inside = unit.x >= std::min(x0, x1) && unit.x <= std::max(x0, x1) &&
                        unit.y >= std::min(y0, y1) && unit.y <= std::max(y0, y1);
    unit.selected = inside || (additive && unit.selected);
  }
  const int count = selected_count(world);
  g_log.push_back({"yr_world_select_box", {x0, y0, x1, y1, additive}, count});
  return count;
}

extern "C" int yr_world_clear_selection(yr_world* world) {
  for (auto& unit : world->units) unit.selected = false;
  g_log.push_back({"yr_world_clear_selection", {}, 0});
  return 0;
}

extern "C" int yr_world_order_move(yr_world* world, int x, int y) {
  const int count = selected_count(world);
  g_log.push_back({"yr_world_order_move", {x, y}, count});
  return count;
}

yr_world *yr_mock_world_handle(void) { return &g_world; }
int yr_mock_world_log_size(void) { return static_cast<int>(g_log.size()); }
const MockCall *yr_mock_world_log_at(int i) {
  if (i < 0 || i >= static_cast<int>(g_log.size())) return nullptr;
  return &g_log[i];
}
void yr_mock_world_log_clear(void) { g_log.clear(); }

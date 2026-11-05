/* yr_world.cpp — P20 world-core：按 shared/yr_slice.h 实现全部 yr_world_* C ABI。
 *
 * 本质（务必如实理解）：
 *   - 本地重建 + SELF_AUTHORED 独立模拟，读取真实 ../p5-isomap/tiles.json 建地形；
 *   - 不是《尤里的复仇》原版世界/订单链，不链接任何原版 exe 二进制；
 *   - 本文件为自制诊断模拟，未复用停机工程的游戏世界代码。
 *
 * 确定性策略（1 / 4 线程逐步整数坐标逐字节一致）：
 *   每步 = 阶段A「意图计算」(只读自身状态 + 地图 + tick，写入按 id 索引的独立数组，
 *          4 线程时按 id 连续分段并行写，无竞争) + 阶段B「确定性仲裁」
 *          (单线程按 id 升序提交，claimed 集合按最小 id 优先，退让单位停在旧位置)。
 *   阶段A 是纯函数、阶段B 顺序固定，故线程数不影响结果。
 */
#include "yr_slice.h"
#include "yr_util.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
constexpr int kUnitMax = 4096;

uint64_t mix64(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

int emit(const std::string& s, char* out, size_t cap) {
  if (!out || cap == 0) return -2;
  if (s.size() + 1 > cap) { out[0] = '\0'; return -2; }
  std::memcpy(out, s.c_str(), s.size() + 1);
  return (int)s.size();
}
inline void push_le16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back((uint8_t)(x & 0xff));
  v.push_back((uint8_t)((x >> 8) & 0xff));
}
}  // namespace

struct yr_world {
  int map_w = 0, map_h = 0;
  std::string source;
  uint32_t seed = 0;
  uint64_t tick = 0;
  uint64_t rng_state = 0;
  int unit_count = 0;

  struct Unit {
    int id = 0, x = 0, y = 0, owner = 0;
    std::string mission;
    bool selected = false;
    bool has_target = false;
    int tx = 0, ty = 0;
  };

  std::vector<int> tile;            // 按 cell 索引 y*map_w+x
  std::vector<uint8_t> passable;    // 缓存通行性
  std::vector<Unit> units;          // 按 id 升序（0..unit_count-1）
  std::vector<uint8_t> trace_bytes; // steps*unit_count*2 个 int16 小端
  std::vector<std::string> per_step_hash;
  int trace_steps = 0;
  std::string notice;

  uint64_t next_rand() { rng_state = mix64(rng_state); return rng_state; }
  bool cell_ok(int x, int y) const {
    if (x < 0 || y < 0 || x >= map_w || y >= map_h) return false;
    return passable[(size_t)y * map_w + x] != 0;
  }
};

/* ------------------------------------------------------------------ */
static void do_step(yr_world* w, int threads) {
  const int N = w->unit_count;
  std::vector<int> ix(N), iy(N);  // 阶段A 独立意图数组：按 id 索引

  auto compute = [&](int begin, int end) {
    for (int i = begin; i < end; i++) {
      const yr_world::Unit& u = w->units[i];
      int nx = u.x, ny = u.y;
      if (u.has_target) {
        // 有命令：朝目标按曼哈顿走一格（先 x 后 y），受阻则停。
        int cx = u.x, cy = u.y;
        if (cx != u.tx) { int s = (u.tx > cx) ? 1 : -1; cx += s; }
        else if (cy != u.ty) { int s = (u.ty > cy) ? 1 : -1; cy += s; }
        nx = cx; ny = cy;
        if (!w->cell_ok(nx, ny)) {
          int ax = u.x, ay = u.y;
          if (u.y != u.ty) { int s = (u.ty > u.y) ? 1 : -1; ay = u.y + s; }
          else if (u.x != u.tx) { int s = (u.tx > u.x) ? 1 : -1; ax = u.x + s; }
          if (w->cell_ok(ax, ay)) { nx = ax; ny = ay; }
          else { nx = u.x; ny = u.y; }  // 被阻塞：原地
        }
      } else {
        // 无命令：基于 (seed, id, tick) 的确定性微游走（纯函数，可并行）。
        uint64_t h = mix64((uint64_t)w->seed ^ mix64((uint64_t)(i + 1)) ^
                           mix64((uint64_t)(w->tick + 1)));
        int d = (int)(h & 3);
        int cx = u.x, cy = u.y;
        switch (d) {
          case 0: cx--; break;
          case 1: cx++; break;
          case 2: cy--; break;
          default: cy++; break;
        }
        if (w->cell_ok(cx, cy)) { nx = cx; ny = cy; }
        else { nx = u.x; ny = u.y; }
      }
      ix[i] = nx; iy[i] = ny;
    }
  };

  if (threads == 4) {
    std::thread th[4];
    int chunk = (N + 3) / 4;
    for (int k = 0; k < 4; k++) {
      int a = k * chunk, b = std::min(N, (k + 1) * chunk);
      if (a < b) th[k] = std::thread(compute, a, b);
    }
    for (int k = 0; k < 4; k++) if (th[k].joinable()) th[k].join();
  } else {
    compute(0, N);
  }

  // 保留所有旧占位，再按 id 顺序释放/认领，拒绝挤入尚未离开的单位。
  // 这只是自制诊断规则：不支持交换位置，也不声称原版避让语义。
  std::vector<uint8_t> claimed((size_t)w->map_w * w->map_h, 0);
  for (const auto& u : w->units) claimed[(size_t)u.y * w->map_w + u.x] = 1;
  for (int i = 0; i < N; i++) {
    const size_t old = (size_t)w->units[i].y * w->map_w + w->units[i].x;
    claimed[old] = 0;
    int nx = ix[i], ny = iy[i];
    size_t key = (size_t)ny * w->map_w + nx;
    if (claimed[key]) { nx = w->units[i].x; ny = w->units[i].y; }  // 让位：退让到旧位置
    claimed[(size_t)ny * w->map_w + nx] = 1;
    w->units[i].x = nx; w->units[i].y = ny;
    if (w->units[i].has_target && nx == w->units[i].tx && ny == w->units[i].ty) {
      w->units[i].has_target = false;
      w->units[i].mission = "GUARD";
    }
  }

  // 追加本步全部单位整数坐标（按 id 升序，每单位 x,y 各一个 int16 小端）。
  size_t before = w->trace_bytes.size();
  for (int i = 0; i < N; i++) {
    push_le16(w->trace_bytes, (uint16_t)(int16_t)w->units[i].x);
    push_le16(w->trace_bytes, (uint16_t)(int16_t)w->units[i].y);
  }
  w->per_step_hash.push_back(
      yr::sha256_hex(w->trace_bytes.data() + before, (size_t)N * 4));
  w->trace_steps++;
  w->tick++;
}

/* ------------------------------------------------------------------ */
extern "C" {

int yr_world_create(yr_world** out, const char* tiles_json_path,
                    int unit_count, uint32_t seed) {
  if (!out || !tiles_json_path || unit_count <= 0 || unit_count > kUnitMax) return -4;
  *out = nullptr;
  std::string content;
  if (!yr::read_file(tiles_json_path, content)) return -1;
  yr::JVal root;
  try { root = yr::json_parse(content); }
  catch (...) { return -1; }
  const yr::JVal* jw = root.get("width");
  const yr::JVal* jh = root.get("height");
  const yr::JVal* jt = root.get("tiles");
  const yr::JVal* js = root.get("source");
  if (!jw || !jh || !jt || jw->type != yr::JVal::Num ||
      jh->type != yr::JVal::Num || jt->type != yr::JVal::Arr) return -1;
  int mw = (int)jw->num, mh = (int)jh->num;
  if (mw <= 0 || mh <= 0 || mw > 32767 || mh > 32767 ||
      (int64_t)mw * mh > 10000000) return -1;

  yr_world* w = new yr_world();
  w->map_w = mw; w->map_h = mh; w->seed = seed; w->unit_count = unit_count;
  w->source = (js && js->type == yr::JVal::Str) ? js->str : std::string("unknown");
  w->tile.assign((size_t)mw * mh, 0);
  w->passable.assign((size_t)mw * mh, 0);
  std::vector<uint8_t> seen((size_t)mw * mh, 0);
  for (const auto& c : jt->arr) {
    const yr::JVal* jx = c.get("x");
    const yr::JVal* jy = c.get("y");
    const yr::JVal* jtv = c.get("tile");
    if (!jx || !jy || !jtv || jx->type != yr::JVal::Num ||
        jy->type != yr::JVal::Num || jtv->type != yr::JVal::Num) { delete w; return -1; }
    int x = (int)jx->num, y = (int)jy->num, t = (int)jtv->num;
    const yr::JVal* jlv = c.get("level");
    const yr::JVal* jic = c.get("ice");
    int lv = jlv ? (int)jlv->num : 0;
    int ic = jic ? (int)jic->num : 0;
    if (x < 0 || y < 0 || x >= mw || y >= mh) { delete w; return -1; }
    size_t idx = (size_t)y * mw + x;
    if (seen[idx]) { delete w; return -1; }
    seen[idx] = 1;
    w->tile[idx] = t;
    w->passable[idx] = yr::passable(t, lv, ic) ? 1 : 0;
  }

  std::vector<size_t> free_cells;
  for (size_t k = 0; k < seen.size(); ++k) {
    if (!seen[k]) { delete w; return -1; }
    if (w->passable[k]) free_cells.push_back(k);
  }
  if (free_cells.size() < (size_t)unit_count) { delete w; return -4; }

  w->rng_state = (uint64_t)seed ^ 0x9e3779b97f4a7c15ULL;
  w->units.resize(unit_count);
  for (int i = 0; i < unit_count; i++) {
    size_t pick = (size_t)(w->next_rand() % free_cells.size());
    size_t cell = free_cells[pick];
    free_cells[pick] = free_cells.back();
    free_cells.pop_back();
    int x = (int)(cell % mw), y = (int)(cell / mw);
    yr_world::Unit& u = w->units[i];
    u.id = i; u.x = x; u.y = y; u.owner = i % 8; u.mission = "GUARD";
    u.selected = false; u.has_target = false; u.tx = 0; u.ty = 0;
  }
  w->notice = std::string(
      "SELF_AUTHORED world-core (P20): independent deterministic simulation; map source=") +
      w->source + "; NOT the original Yuri's Revenge world/order chain; " +
      "NOT linked to any original exe binary. See LIMITATIONS.md / RUN.txt.";
  *out = w;
  return 0;
}

void yr_world_destroy(yr_world* w) { delete w; }

int yr_world_unit_count(const yr_world* w) { return w ? w->unit_count : 0; }

int yr_world_map_size(const yr_world* w, int* out_w, int* out_h) {
  if (!w) return -1;
  if (out_w) *out_w = w->map_w;
  if (out_h) *out_h = w->map_h;
  return 0;
}

uint64_t yr_world_tick(const yr_world* w) { return w ? w->tick : 0; }

const char* yr_world_implementation_notice(const yr_world* w) {
  return w ? w->notice.c_str() : "";
}

int yr_world_select_box(yr_world* w, int x0, int y0, int x1, int y1, int additive) {
  if (!w) return -1;
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  if (!additive)
    for (auto& u : w->units) u.selected = false;
  for (auto& u : w->units)
    if (u.x >= x0 && u.x <= x1 && u.y >= y0 && u.y <= y1) u.selected = true;
  int n = 0;
  for (const auto& u : w->units) if (u.selected) n++;
  return n;  // 返回操作后的选中总数
}

int yr_world_selected_count(const yr_world* w) {
  if (!w) return 0;
  int n = 0;
  for (const auto& u : w->units) if (u.selected) n++;
  return n;
}

int yr_world_clear_selection(yr_world* w) {
  if (!w) return 0;
  for (auto& u : w->units) u.selected = false;
  return 0;
}

int yr_world_order_move(yr_world* w, int target_x, int target_y) {
  if (!w) return -1;
  int sel = 0;
  for (const auto& u : w->units) if (u.selected) sel++;
  if (sel == 0) return 0;                                   // 无选中
  if (target_x < 0 || target_y < 0 ||
      target_x >= w->map_w || target_y >= w->map_h) return 0;  // 越界
  if (!w->cell_ok(target_x, target_y)) return 0;           // 不可达 / 不可通行
  for (auto& u : w->units)
    if (u.selected) { u.has_target = true; u.tx = target_x; u.ty = target_y; u.mission = "MOVE"; }
  return sel;
}

int yr_world_step(yr_world* w, int ticks, int threads) {
  if (!w) return -1;
  if (threads != 1 && threads != 4) return -3;  // 只接受 1 或 4
  if (ticks < 0) return -4;
  for (int t = 0; t < ticks; t++) do_step(w, threads);
  return ticks;
}

int yr_world_snapshot_json(const yr_world* w, char* out, size_t cap) {
  if (!w) return -1;
  std::string s = "{\"tick\":" + std::to_string(w->tick) +
      ",\"map\":{\"width\":" + std::to_string(w->map_w) +
      ",\"height\":" + std::to_string(w->map_h) +
      ",\"source\":\"" + yr::json_escape(w->source) + "\"}," +
      "\"selected_count\":" + std::to_string(yr_world_selected_count(w)) +
      ",\"units\":[";
  for (int i = 0; i < w->unit_count; i++) {
    if (i) s += ",";
    const auto& u = w->units[i];
    s += "{\"id\":" + std::to_string(u.id) +
         ",\"x\":" + std::to_string(u.x) +
         ",\"y\":" + std::to_string(u.y) +
         ",\"owner\":" + std::to_string(u.owner) +
         ",\"mission\":\"" + yr::json_escape(u.mission) + "\"}";
  }
  s += "]}";
  return emit(s, out, cap);
}

int yr_world_trace_json(const yr_world* w, char* out, size_t cap) {
  if (!w) return -1;
  std::string s = "{\"unit_count\":" + std::to_string(w->unit_count) +
      ",\"steps\":" + std::to_string(w->trace_steps) +
      ",\"per_step_sha256\":[";
  for (size_t i = 0; i < w->per_step_hash.size(); i++) {
    if (i) s += ",";
    s += "\"" + w->per_step_hash[i] + "\"";
  }
  s += "],\"positions_le_u16_base64\":\"" +
      yr::base64_encode(w->trace_bytes.data(), w->trace_bytes.size()) + "\"}";
  return emit(s, out, cap);
}

void yr_world_reset_trace(yr_world* w) {
  if (!w) return;
  w->trace_bytes.clear();
  w->per_step_hash.clear();
  w->trace_steps = 0;
}

int yr_world_render(const yr_world* w, uint8_t* rgba, int width, int height,
                    size_t stride, const uint8_t* pal768) {
  if (!w || !rgba) return -5;
  if (width <= 0 || height <= 0 || stride < (size_t)width * 4) return -5;
  int scale = width / w->map_w;
  if (scale < 1 || (int64_t)w->map_w * scale != width) return -5;
  if ((int64_t)w->map_h * scale != height) return -5;

  // 真实 P5 地表（SELF_AUTHORED_ART 诊断着色 / 非原版 TMP 地表贴图）。
  for (int cy = 0; cy < w->map_h; cy++) {
    for (int cx = 0; cx < w->map_w; cx++) {
      uint8_t r, g, b;
      yr::tile_color(pal768, w->tile[(size_t)cy * w->map_w + cx], r, g, b);
      for (int py = 0; py < scale; py++) {
        uint8_t* row = rgba + (size_t)(cy * scale + py) * stride;
        for (int px = 0; px < scale; px++) {
          uint8_t* p = row + (size_t)(cx * scale + px) * 4;
          p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
      }
    }
  }

  // 当前单位：cell 块中心 2x2 品红前景（绝不写 stride padding）。
  int mw = scale < 2 ? 1 : 2;
  int off = (scale - mw) / 2;
  int nonbg = 0;
  for (const auto& u : w->units) {
    int bx = u.x * scale, by = u.y * scale;
    for (int dy = 0; dy < mw; dy++) {
      for (int dx = 0; dx < mw; dx++) {
        int px = bx + off + dx, py = by + off + dy;
        if (px >= 0 && px < width && py >= 0 && py < height) {
          uint8_t* p = rgba + (size_t)py * stride + (size_t)px * 4;
          p[0] = 255; p[1] = 0; p[2] = 255; p[3] = 255;
          nonbg++;
        }
      }
    }
  }
  return nonbg;  // 前景像素写入数（= marker_pixels_per_unit * unit_count）
}

}  // extern "C"

/* core_host.cpp — P20 命令行宿主（第六批集成器只链接 libyrworld.a，本宿主仅用于本路取证）。
 * 同 seed、同输入序列（框选→下令→360 步）分别用 1 / 4 线程跑两遍，
 * 产出结构化 JSON / trace / PNG / 帧缓冲 / 守卫结果，供 verify_core.py 独立核验。
 */
#include "yr_slice.h"
#include "yr_util.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

const int UNIT_COUNT = 1200;
const int TICKS = 360;
const int CELL_SCALE = 4;
const int PAD = 16;
const uint32_t SEED = 20261104u;
const char* P5_TILES = "../p5-isomap/tiles.json";   // 只读真实地图
const char* TILES_REF = "tiles_ref.json";            // 本目录只读副本（供独立核验）
const char* PAL = "unittem-ra2.pal";                 // 从 p16 只读拷贝

std::string d2s(double x) { char b[64]; std::snprintf(b, sizeof(b), "%.3f", x); return std::string(b); }

bool load_grid(const std::string& path, int& w, int& h,
               std::vector<int>& tile, std::vector<int>& level, std::vector<int>& ice,
               std::string& source, std::string& theater) {
  std::string s;
  if (!yr::read_file(path, s)) return false;
  yr::JVal root;
  try { root = yr::json_parse(s); } catch (...) { return false; }
  const yr::JVal* jw = root.get("width");
  const yr::JVal* jh = root.get("height");
  const yr::JVal* jt = root.get("tiles");
  if (!jw || !jh || !jt) return false;
  w = (int)jw->num; h = (int)jh->num;
  if (w <= 0 || h <= 0) return false;
  tile.assign((size_t)w * h, 0);
  level.assign((size_t)w * h, 0);
  ice.assign((size_t)w * h, 0);
  for (const auto& c : jt->arr) {
    const yr::JVal* jx = c.get("x"); const yr::JVal* jy = c.get("y");
    const yr::JVal* jtv = c.get("tile");
    if (!jx || !jy || !jtv) continue;
    int x = (int)jx->num, y = (int)jy->num, t = (int)jtv->num;
    if (x < 0 || y < 0 || x >= w || y >= h) continue;
    size_t idx = (size_t)y * w + x;
    tile[idx] = t;
    const yr::JVal* jlv = c.get("level"); const yr::JVal* jic = c.get("ice");
    level[idx] = jlv ? (int)jlv->num : 0;
    ice[idx] = jic ? (int)jic->num : 0;
  }
  const yr::JVal* js = root.get("source");
  const yr::JVal* jth = root.get("theater");
  source = (js && js->type == yr::JVal::Str) ? js->str : std::string("unknown");
  theater = (jth && jth->type == yr::JVal::Str) ? jth->str : std::string("unknown");
  return true;
}

bool pick_cell(int w, int h, const std::vector<int>& tile,
               const std::vector<int>& level, const std::vector<int>& ice,
               bool want_passable, int& ox, int& oy) {
  for (int idx = 0; idx < w * h; idx++) {
    bool p = yr::passable(tile[idx], level[idx], ice[idx]);
    if (p == want_passable) { ox = idx % w; oy = idx / w; return true; }
  }
  return false;
}

typedef int (*JsonFn)(const yr_world*, char*, size_t);
bool dump_json(JsonFn fn, const yr_world* w, const std::string& path) {
  std::vector<char> buf(16u * 1024 * 1024);
  int n = fn(w, buf.data(), buf.size());
  if (n < 0) { std::fprintf(stderr, "dump_json %s returned %d\n", path.c_str(), n); return false; }
  return yr::write_file(path, std::string(buf.data(), (size_t)n));
}

/* 共享产物（仅 1t 那次生成一次） */
bool write_shared(yr_world* w, int mw, int mh,
                  const std::vector<int>& gt, const std::vector<int>& gl, const std::vector<int>& gi,
                  const std::string& src, const std::string& theater,
                  const std::string& p5bytes, int tx, int ty) {
  std::string refbytes, palbytes;
  if (!yr::read_file(TILES_REF, refbytes)) return false;
  if (!yr::read_file(PAL, palbytes)) return false;
  if (palbytes.size() != 768) return false;

  /* ---- cells_baseline.bin + map_digest.json ---- */
  std::vector<uint8_t> cells((size_t)mw * mh * 2);
  for (int k = 0; k < mw * mh; k++) {
    uint16_t v = (uint16_t)(gt[k] & 0xffff);
    cells[2 * k] = (uint8_t)(v & 0xff);
    cells[2 * k + 1] = (uint8_t)((v >> 8) & 0xff);
  }
  int pc = 0;
  for (int k = 0; k < mw * mh; k++) if (yr::passable(gt[k], gl[k], gi[k])) pc++;
  int ic = mw * mh - pc;
  std::vector<int> uniqv(gt.begin(), gt.end());
  std::sort(uniqv.begin(), uniqv.end());
  int uniq = (int)(std::unique(uniqv.begin(), uniqv.end()) - uniqv.begin());
  std::string cells_sha = yr::sha256_hex_s(std::string((const char*)cells.data(), cells.size()));
  std::string p5_sha = yr::sha256_hex_s(p5bytes);
  std::string ref_sha = yr::sha256_hex_s(refbytes);
  std::string pal_sha = yr::sha256_hex_s(palbytes);

  std::string md = std::string("{\n") +
    "  \"tiles_json_path\": \"../p5-isomap/tiles.json\",\n" +
    "  \"p5_tiles_sha256\": \"" + p5_sha + "\",\n" +
    "  \"tiles_ref_path\": \"tiles_ref.json\",\n" +
    "  \"tiles_ref_sha256\": \"" + ref_sha + "\",\n" +
    "  \"pal768_path\": \"unittem-ra2.pal\",\n" +
    "  \"pal768_sha256\": \"" + pal_sha + "\",\n" +
    "  \"source\": \"" + yr::json_escape(src) + "\",\n" +
    "  \"theater\": \"" + yr::json_escape(theater) + "\",\n" +
    "  \"width\": " + std::to_string(mw) + ", \"height\": " + std::to_string(mh) + ",\n" +
    "  \"cell_count\": " + std::to_string(mw * mh) + ",\n" +
    "  \"unique_tiles\": " + std::to_string(uniq) + ",\n" +
    "  \"passable_count\": " + std::to_string(pc) + ",\n" +
    "  \"impassable_count\": " + std::to_string(ic) + ",\n" +
    "  \"passability_rule\": \"SELF_AUTHORED_MAPPING: passable=(level==0 && ice==0) && ((tile*2654435761)%100)<80; diagnostic only, NOT original RA2 terrain\",\n" +
    "  \"cells_sha256\": \"" + cells_sha + "\",\n" +
    "  \"cells_baseline\": \"cells_baseline.bin\",\n" +
    "  \"terrain_rule\": \"SELF_AUTHORED_ART: idx=tile%256; rgb=pal768[3*idx+k]<<2; 诊断着色 / 非原版 TMP 地表贴图\",\n" +
    "  \"implementation_notice\": \"" + yr::json_escape(yr_world_implementation_notice(w)) + "\"\n" +
    "}\n";
  if (!yr::write_file("map_digest.json", md)) return false;
  if (!yr::write_file("cells_baseline.bin", cells.data(), cells.size())) return false;

  /* ---- 帧缓冲（stride padding 必须保留） ---- */
  int width = mw * CELL_SCALE, height = mh * CELL_SCALE;
  size_t stride = (size_t)width * 4 + PAD;
  std::vector<uint8_t> buf((size_t)height * stride);
  for (size_t i = 0; i < buf.size(); i++) buf[i] = (uint8_t)((i * 13 + 7) & 0xff);  // 哨兵
  int nonbg = yr_world_render(w, buf.data(), width, height, stride,
                              (const uint8_t*)palbytes.data());
  if (nonbg < 0) { std::fprintf(stderr, "render returned %d\n", nonbg); return false; }
  std::vector<uint8_t> px((size_t)height * width * 4);
  for (int r = 0; r < height; r++)
    std::memcpy(px.data() + (size_t)r * width * 4,
                buf.data() + (size_t)r * stride, (size_t)width * 4);
  if (!yr::png_write_rgba("frame.png", px.data(), width, height)) return false;
  if (!yr::write_file("frame_rgba_stride.bin", buf.data(), buf.size())) return false;
  int mw2 = CELL_SCALE < 2 ? 1 : 2;
  int mpp = mw2 * mw2;
  std::string fm = std::string("{\n") +
    "  \"width\": " + std::to_string(width) + ", \"height\": " + std::to_string(height) + ",\n" +
    "  \"stride\": " + std::to_string(stride) + ", \"pad_bytes\": " + std::to_string(PAD) + ",\n" +
    "  \"cell_scale\": " + std::to_string(CELL_SCALE) + ",\n" +
    "  \"map_width\": " + std::to_string(mw) + ", \"map_height\": " + std::to_string(mh) + ",\n" +
    "  \"pixels\": " + std::to_string((int64_t)width * height) + ",\n" +
    "  \"sentinel\": \"(i*13+7)&0xff\",\n" +
    "  \"sentinel_note\": \"i = absolute byte index in frame_rgba_stride.bin\",\n" +
    "  \"nonbg_reported\": " + std::to_string(nonbg) + ",\n" +
    "  \"unit_count\": " + std::to_string(UNIT_COUNT) + ",\n" +
    "  \"marker_pixels_per_unit\": " + std::to_string(mpp) + ",\n" +
    "  \"pal768_used\": true,\n" +
    "  \"pal768_sha256\": \"" + pal_sha + "\",\n" +
    "  \"unit_color\": [255,0,255,255],\n" +
    "  \"terrain_rule\": \"SELF_AUTHORED_ART: idx=tile%256; rgb=pal768[3*idx+k]<<2; 诊断着色 / 非原版 TMP 地表贴图\",\n" +
    "  \"art_notice\": \"诊断着色 / 非原版 TMP 地表贴图 (SELF_AUTHORED_ART)\"\n" +
    "}\n";
  if (!yr::write_file("frame_meta.json", fm)) return false;

  /* ---- threads_guard：真实用非法 threads 调 yr_world_step，断言 -3 ---- */
  const int invalid[] = {2, 3, 0, 8, -1};
  std::string tg = "{\n  \"expected\": -3,\n  \"cases\": [\n";
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    int rc = yr_world_step(w, 1, invalid[i]);
    if (rc != -3) {
      std::fprintf(stderr, "threads=%d returned %d, expected -3\n", invalid[i], rc);
      return false;
    }
    tg += "    {\"threads\": " + std::to_string(invalid[i]) +
          ", \"return_code\": " + std::to_string(rc) + "}" +
          (i + 1 < sizeof(invalid) / sizeof(invalid[0]) ? "," : "") + "\n";
  }
  tg += "  ]\n}\n";
  if (!yr::write_file("threads_guard.json", tg)) return false;

  /* ---- order_guard：真实调用无选中/越界/不可通行/合法四种情形 ---- */
  int ix = -1, iy = -1;
  if (!pick_cell(mw, mh, gt, gl, gi, false, ix, iy)) return false;
  yr_world* g = nullptr;
  if (yr_world_create(&g, P5_TILES, UNIT_COUNT, SEED) != 0) return false;
  int a, b, c, d;
  yr_world_clear_selection(g);
  a = yr_world_order_move(g, tx, ty);                              // 无选中
  yr_world_select_box(g, 0, 0, mw - 1, mh - 1, 0);
  b = yr_world_order_move(g, -5, -5);                             // 越界
  yr_world_select_box(g, 0, 0, mw - 1, mh - 1, 0);
  c = yr_world_order_move(g, ix, iy);                            // 不可通行
  yr_world_select_box(g, 0, 0, mw - 1, mh - 1, 0);
  d = yr_world_order_move(g, tx, ty);                            // 合法
  yr_world_destroy(g);
  if (a > 0 || b > 0 || c > 0 || d <= 0) {
    std::fprintf(stderr, "order_guard invalid: a=%d b=%d c=%d d=%d\n", a, b, c, d);
    return false;
  }
  std::string og = std::string("{\n") +
    "  \"no_selection\": " + std::to_string(a) + ",\n" +
    "  \"out_of_bounds\": " + std::to_string(b) + ",\n" +
    "  \"impassable\": " + std::to_string(c) + ",\n" +
    "  \"valid_accepted\": " + std::to_string(d) + ",\n" +
    "  \"target\": [" + std::to_string(tx) + "," + std::to_string(ty) + "],\n" +
    "  \"impassable_cell\": [" + std::to_string(ix) + "," + std::to_string(iy) + "]\n" +
    "}\n";
  if (!yr::write_file("order_guard.json", og)) return false;

  /* ---- init_determinism：两个同 seed 世界初始 snapshot 的哈希相等 ---- */
  yr_world *wa = nullptr, *wb = nullptr;
  if (yr_world_create(&wa, P5_TILES, UNIT_COUNT, SEED) != 0) return false;
  if (yr_world_create(&wb, P5_TILES, UNIT_COUNT, SEED) != 0) return false;
  std::vector<char> b1(4u * 1024 * 1024), b2(4u * 1024 * 1024);
  int n1 = yr_world_snapshot_json(wa, b1.data(), b1.size());
  int n2 = yr_world_snapshot_json(wb, b2.data(), b2.size());
  std::string h1 = yr::sha256_hex_s(std::string(b1.data(), (size_t)(n1 > 0 ? n1 : 0)));
  std::string h2 = yr::sha256_hex_s(std::string(b2.data(), (size_t)(n2 > 0 ? n2 : 0)));
  bool eq = (n1 > 0 && n2 > 0 && h1 == h2);
  yr_world_destroy(wa); yr_world_destroy(wb);
  std::string id = std::string("{\n") +
    "  \"seed\": " + std::to_string(SEED) + ",\n" +
    "  \"hash_a\": \"" + h1 + "\",\n" +
    "  \"hash_b\": \"" + h2 + "\",\n" +
    "  \"equal\": " + (eq ? "true" : "false") + "\n" +
    "}\n";
  if (!yr::write_file("init_determinism.json", id)) return false;
  return true;
}

int do_run(const std::string& tag, int threads, bool no_step = false) {
  // 复制真实 P5 地图到本目录只读参考副本（供独立核验；绝不改动 P5 产物）。
  std::string p5bytes;
  if (!yr::read_file(P5_TILES, p5bytes)) {
    std::fprintf(stderr, "cannot read %s\n", P5_TILES); return 1;
  }
  if (!yr::write_file(TILES_REF, p5bytes)) return 1;

  int mw = 0, mh = 0;
  std::vector<int> gt, gl, gi;
  std::string src, theater;
  if (!load_grid(TILES_REF, mw, mh, gt, gl, gi, src, theater)) return 1;
  int tx = -1, ty = -1;
  if (!pick_cell(mw, mh, gt, gl, gi, true, tx, ty)) return 1;

  yr_world* w = nullptr;
  int rc = yr_world_create(&w, P5_TILES, UNIT_COUNT, SEED);
  if (rc != 0) { std::fprintf(stderr, "yr_world_create rc=%d\n", rc); return 1; }
  yr_world_reset_trace(w);

  // 同输入序列：框选（含 additive）→ 下令 → 360 步。
  int sel1 = yr_world_select_box(w, 10, 8, 100, 55, 0);
  int sel2 = yr_world_select_box(w, 60, 20, 150, 75, 1);
  int ord = yr_world_order_move(w, tx, ty);
  if (ord <= 0) {
    std::fprintf(stderr, "order_move returned %d (expected >0)\n", ord);
    yr_world_destroy(w); return 1;
  }
  auto t0 = std::chrono::steady_clock::now();
  int adv = no_step ? 0 : yr_world_step(w, TICKS, threads);
  auto t1 = std::chrono::steady_clock::now();
  double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  if (adv != (no_step ? 0 : TICKS)) {
    std::fprintf(stderr, "yr_world_step advanced %d (expected %d)\n", adv, TICKS);
    yr_world_destroy(w); return 1;
  }

  if (!dump_json(yr_world_snapshot_json, w, "snapshot_" + tag + ".json")) { yr_world_destroy(w); return 1; }
  if (!dump_json(yr_world_trace_json, w, "trace_" + tag + ".json")) { yr_world_destroy(w); return 1; }
  std::string tm = std::string("{\n") +
    "  \"tag\": \"" + tag + "\",\n" +
    "  \"threads\": " + std::to_string(threads) + ",\n" +
    "  \"unit_count\": " + std::to_string(UNIT_COUNT) + ",\n" +
    "  \"steps\": " + std::to_string(TICKS) + ",\n" +
    "  \"seed\": " + std::to_string(SEED) + ",\n" +
    "  \"select_box_1\": " + std::to_string(sel1) + ",\n" +
    "  \"select_box_2\": " + std::to_string(sel2) + ",\n" +
    "  \"order_accepted\": " + std::to_string(ord) + ",\n" +
    "  \"advanced\": " + std::to_string(adv) + ",\n" +
    "  \"wall_ms\": " + d2s(ms) + "\n" +
    "}\n";
  if (!yr::write_file("timing_" + tag + ".json", tm)) { yr_world_destroy(w); return 1; }

  if (tag == "1t") {
    if (!write_shared(w, mw, mh, gt, gl, gi, src, theater, p5bytes, tx, ty)) {
      yr_world_destroy(w); return 1;
    }
  }
  yr_world_destroy(w);
  return 0;
}

int do_compare() {
  std::string s1, s4;
  if (!yr::read_file("trace_1t.json", s1) || !yr::read_file("trace_4t.json", s4)) return 1;
  yr::JVal j1, j4;
  try { j1 = yr::json_parse(s1); j4 = yr::json_parse(s4); }
  catch (...) { return 1; }
  const yr::JVal* b1 = j1.get("positions_le_u16_base64");
  const yr::JVal* b4 = j4.get("positions_le_u16_base64");
  const yr::JVal* h1 = j1.get("per_step_sha256");
  const yr::JVal* h4 = j4.get("per_step_sha256");
  const yr::JVal* js = j1.get("steps");
  const yr::JVal* jn = j1.get("unit_count");
  if (!b1 || !b4 || !h1 || !h4 || !js || !jn) return 1;
  bool byte_eq = (b1->str == b4->str);
  bool hash_eq = (h1->arr.size() == h4->arr.size());
  if (hash_eq)
    for (size_t i = 0; i < h1->arr.size(); i++)
      if (h1->arr[i].str != h4->arr[i].str) { hash_eq = false; break; }
  bool identical = byte_eq && hash_eq;
  int steps = (int)js->num, N = (int)jn->num;
  std::string c = std::string("{\n") +
    "  \"identical\": " + (identical ? "true" : "false") + ",\n" +
    "  \"positions_byte_equal\": " + (byte_eq ? "true" : "false") + ",\n" +
    "  \"per_step_hashes_equal\": " + (hash_eq ? "true" : "false") + ",\n" +
    "  \"compared_pairs\": " + std::to_string((int64_t)steps * N) + ",\n" +
    "  \"steps\": " + std::to_string(steps) + ",\n" +
    "  \"unit_count\": " + std::to_string(N) + ",\n" +
    "  \"trace_1t_b64_len\": " + std::to_string(b1->str.size()) + ",\n" +
    "  \"trace_4t_b64_len\": " + std::to_string(b4->str.size()) + "\n" +
    "}\n";
  if (!yr::write_file("consistency.json", c)) return 1;
  if (!identical) std::fprintf(stderr, "NOT_ACHIEVED: 1/4 thread trajectories differ\n");
  return identical ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (const char* path = std::getenv("YR_TILES_PATH")) P5_TILES = path;
  if (argc >= 2 && std::string(argv[1]) == "compare") return do_compare();
  if (argc < 3) {
    std::fprintf(stderr, "usage: core_host <1t|4t> <1|4> | compare\n");
    return 2;
  }
  std::string tag = argv[1];
  int threads = std::atoi(argv[2]);
  if ((tag != "1t" && tag != "4t") || (threads != 1 && threads != 4)) {
    std::fprintf(stderr, "bad args: tag=%s threads=%d\n", tag.c_str(), threads);
    return 2;
  }
  if (argc > 4 || (argc == 4 && std::string(argv[3]) != "--no-step")) return 2;
  return do_run(tag, threads, argc == 4);
}

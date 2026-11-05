/* yr_util.h — P20 world-core 内部工具（SELF_AUTHORED）
 * SHA-256 / base64 / PNG(RGBA8, stored-deflate) / 最小 JSON / 文件 IO，
 * 以及 SELF_AUTHORED_MAPPING 通行性规则与 SELF_AUTHORED_ART 诊断着色规则。
 * 本文件全部为本地重建，不涉及任何原版 exe 二进制或原版引擎代码。
 */
#ifndef YR_UTIL_H
#define YR_UTIL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace yr {

/* ---- SHA-256（纯标准库实现，供 C++ 侧取证；核验器用 Python hashlib 独立重算） ---- */
std::string sha256_hex(const uint8_t* data, size_t len);
inline std::string sha256_hex_s(const std::string& s) {
  return sha256_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

/* ---- base64（标准字母表，带 '=' padding） ---- */
std::string base64_encode(const uint8_t* data, size_t len);

/* ---- PNG：RGBA8，紧排像素 width*height*4；stored-deflate，无第三方依赖 ---- */
bool png_write_rgba(const std::string& path, const uint8_t* pixels, int width, int height);

/* ---- 文件 IO（二进制安全） ---- */
bool read_file(const std::string& path, std::string& out);
bool write_file(const std::string& path, const std::string& data);
bool write_file(const std::string& path, const uint8_t* data, size_t len);

/* ---- 最小 JSON 解析器（对象/数组/字符串/数字/bool/null） ---- */
struct JVal {
  enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
  bool b = false;
  long long num = 0;
  std::string str;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;
  const JVal* get(const std::string& k) const {
    for (const auto& kv : obj)
      if (kv.first == k) return &kv.second;
    return nullptr;
  }
};
JVal json_parse(const std::string& s);  // 失败抛 std::runtime_error
std::string json_escape(const std::string& s);

/* ---- SELF_AUTHORED_MAPPING：诊断用通行性规则（非原版 RA2 地形语义） ----
 * 依据：P5 tiles.json 只暴露 tile id，无原版 TMP 的 terrain type / speed 表，
 * 故用 tile id 的确定性散列把 cell 分成「通行/阻塞」两类，仅用于本模拟的
 * 命令可达性判定与逐步仲裁，绝不代表原版地形通行性。规则与依据见 RUN.txt。
 *   passable = (level==0 && ice==0) && ((tile*2654435761) % 100) < 80
 */
inline bool passable(int tile, int level, int ice) {
  if (level != 0 || ice != 0) return false;
  uint64_t h = (uint64_t)(uint32_t)tile * 2654435761ULL;
  return (h % 100) < 80;
}

/* ---- SELF_AUTHORED_ART：诊断着色 / 非原版 TMP 地表贴图 ----
 * idx = tile % 256；rgb = pal768[3*idx + k] << 2（6-bit -> 8-bit）。
 * 单位前景色固定 (255,0,255,255)；因 6-bit<<2 只能取到 4 的倍数（最大 252），
 * 255 不可能出现在地表，故单位像素可被独立区分。pal768==NULL 时用灰度图案。
 */
inline void tile_color(const uint8_t* pal768, int tile,
                       uint8_t& r, uint8_t& g, uint8_t& b) {
  if (!pal768) { r = g = b = (uint8_t)(tile & 0xff); return; }
  int idx = (tile & 0xffff) % 256;
  r = (uint8_t)(pal768[idx * 3 + 0] << 2);
  g = (uint8_t)(pal768[idx * 3 + 1] << 2);
  b = (uint8_t)(pal768[idx * 3 + 2] << 2);
}

}  // namespace yr

#endif /* YR_UTIL_H */

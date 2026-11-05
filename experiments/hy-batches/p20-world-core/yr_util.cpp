/* yr_util.cpp — P20 world-core 内部工具实现（SELF_AUTHORED）。
 * 全部为本地重建：不引用集成源码树任何文件，不链接原版 exe。
 */
#include "yr_util.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace yr {
namespace {

/* ---------------------------- 内部辅助（internal linkage） ---------------------------- */
static const uint32_t kK[64] = {
  0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
  0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
  0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
  0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
  0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
  0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
  0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
  0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++)
      c = (c & 1u) ? ((c >> 1) ^ 0xEDB88320u) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}
uint32_t adler32(const uint8_t* d, size_t n) {
  uint32_t a = 1, b = 0;
  for (size_t i = 0; i < n; i++) {
    a = (a + d[i]) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}
void put32be(std::string& s, uint32_t v) {
  s.push_back((char)((v >> 24) & 0xff));
  s.push_back((char)((v >> 16) & 0xff));
  s.push_back((char)((v >> 8) & 0xff));
  s.push_back((char)(v & 0xff));
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32be(out, (uint32_t)data.size());
  std::string td = std::string(type, 4) + data;
  out += td;
  put32be(out, crc32((const uint8_t*)td.data(), td.size()));
}

/* ---------------------------- 最小 JSON 解析器 ---------------------------- */
struct Parser {
  const std::string& s;
  size_t p = 0;
  explicit Parser(const std::string& str) : s(str) {}
  void ws() { while (p < s.size() && (s[p]==' '||s[p]=='\t'||s[p]=='\n'||s[p]=='\r')) p++; }
  char cur() { return p < s.size() ? s[p] : '\0'; }
  void expect(char c) {
    if (cur() != c) throw std::runtime_error(std::string("json: expected '") + c + "'");
    p++;
  }
  void lit(const char* t) {
    size_t n = std::strlen(t);
    if (p + n > s.size() || s.compare(p, n, t) != 0)
      throw std::runtime_error(std::string("json: bad literal ") + t);
    p += n;
  }
  std::string parse_string() {
    expect('"');
    std::string o;
    while (true) {
      if (p >= s.size()) throw std::runtime_error("json: unterminated string");
      char c = s[p++];
      if (c == '"') break;
      if (c == '\\') {
        if (p >= s.size()) throw std::runtime_error("json: bad escape");
        char e = s[p++];
        switch (e) {
          case '"': o += '"'; break;
          case '\\': o += '\\'; break;
          case '/': o += '/'; break;
          case 'b': o += '\b'; break;
          case 'f': o += '\f'; break;
          case 'n': o += '\n'; break;
          case 'r': o += '\r'; break;
          case 't': o += '\t'; break;
          case 'u': {
            if (p + 4 > s.size()) throw std::runtime_error("json: bad \\u");
            unsigned cp = 0;
            for (int k = 0; k < 4; k++) {
              char h = s[p++]; cp <<= 4;
              if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
              else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
              else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
              else throw std::runtime_error("json: bad \\u hex");
            }
            if (cp < 0x80) o += (char)cp;
            else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
            else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
            break;
          }
          default: throw std::runtime_error("json: unknown escape");
        }
      } else o += c;
    }
    return o;
  }
  JVal value() {
    ws();
    char c = cur();
    if (c == '"') { JVal v; v.type = JVal::Str; v.str = parse_string(); return v; }
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == 't') { lit("true"); JVal v; v.type = JVal::Bool; v.b = true; return v; }
    if (c == 'f') { lit("false"); JVal v; v.type = JVal::Bool; v.b = false; return v; }
    if (c == 'n') { lit("null"); return JVal(); }
    return parse_number();
  }
  JVal parse_number() {
    size_t st = p;
    if (cur() == '-') p++;
    while (p < s.size() && std::isdigit((unsigned char)s[p])) p++;
    if (p < s.size() && s[p] == '.') { p++; while (p < s.size() && std::isdigit((unsigned char)s[p])) p++; }
    if (p < s.size() && (s[p] == 'e' || s[p] == 'E')) {
      p++;
      if (p < s.size() && (s[p] == '+' || s[p] == '-')) p++;
      while (p < s.size() && std::isdigit((unsigned char)s[p])) p++;
    }
    if (p == st) throw std::runtime_error("json: invalid number");
    std::string t = s.substr(st, p - st);
    long long v = 0; size_t q = 0; bool neg = false;
    if (t[0] == '-') { neg = true; q = 1; }
    for (; q < t.size() && std::isdigit((unsigned char)t[q]); q++) v = v * 10 + (t[q] - '0');
    if (neg) v = -v;
    JVal j; j.type = JVal::Num; j.num = v; return j;
  }
  JVal parse_array() {
    expect('[');
    JVal j; j.type = JVal::Arr; ws();
    if (cur() == ']') { p++; return j; }
    while (true) {
      j.arr.push_back(value()); ws();
      char c = cur();
      if (c == ',') { p++; continue; }
      if (c == ']') { p++; break; }
      throw std::runtime_error("json: bad array");
    }
    return j;
  }
  JVal parse_object() {
    expect('{');
    JVal j; j.type = JVal::Obj; ws();
    if (cur() == '}') { p++; return j; }
    while (true) {
      ws();
      std::string k = parse_string();
      ws(); expect(':');
      j.obj.push_back(std::make_pair(k, value()));
      ws();
      char c = cur();
      if (c == ',') { p++; continue; }
      if (c == '}') { p++; break; }
      throw std::runtime_error("json: bad object");
    }
    return j;
  }
};

}  // namespace (anonymous)

/* ---------------------------- 对外 API（external linkage） ---------------------------- */
std::string sha256_hex(const uint8_t* data, size_t len) {
  uint32_t h[8] = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
                   0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
  size_t total = len + 1;
  while (total % 64 != 56) total++;
  total += 8;
  std::vector<uint8_t> msg(total, 0);
  if (len) std::memcpy(msg.data(), data, len);
  msg[len] = 0x80;
  uint64_t bits = (uint64_t)len * 8;
  for (int i = 0; i < 8; i++) msg[total - 1 - i] = (uint8_t)((bits >> (8 * i)) & 0xff);
  for (size_t off = 0; off < total; off += 64) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
      w[i] = ((uint32_t)msg[off + 4*i] << 24) | ((uint32_t)msg[off + 4*i + 1] << 16) |
             ((uint32_t)msg[off + 4*i + 2] << 8) | ((uint32_t)msg[off + 4*i + 3]);
    for (int i = 16; i < 64; i++) {
      uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
      uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
      w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
    for (int i = 0; i < 64; i++) {
      uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
      uint32_t ch = (e & f) ^ ((~e) & g);
      uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
      uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
      uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      uint32_t t2 = S0 + maj;
      hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
  }
  char buf[65];
  for (int i = 0; i < 8; i++) std::snprintf(buf + i*8, 9, "%08x", (unsigned)h[i]);
  buf[64] = '\0';
  return std::string(buf);
}

std::string base64_encode(const uint8_t* d, size_t n) {
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string o;
  o.reserve(((n + 2) / 3) * 4);
  size_t i = 0;
  while (i + 3 <= n) {
    uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i+1] << 8) | d[i+2];
    o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63];
    o += T[(v >> 6) & 63];  o += T[v & 63];
    i += 3;
  }
  if (n - i == 1) {
    uint32_t v = (uint32_t)d[i] << 16;
    o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += '='; o += '=';
  } else if (n - i == 2) {
    uint32_t v = ((uint32_t)d[i] << 16) | ((uint32_t)d[i+1] << 8);
    o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
  }
  return o;
}

bool png_write_rgba(const std::string& path, const uint8_t* pixels, int width, int height) {
  if (!pixels || width <= 0 || height <= 0) return false;
  size_t rowbytes = (size_t)width * 4;
  std::string raw;
  raw.reserve((rowbytes + 1) * (size_t)height);
  for (int y = 0; y < height; y++) {
    raw.push_back('\0');  // filter type 0
    raw.append((const char*)(pixels + (size_t)y * rowbytes), rowbytes);
  }
  std::string z;
  z.push_back((char)0x78);
  z.push_back((char)0x9C);  // zlib: CM=8, FCHECK valid for 0x789C
  const size_t BLK = 65535;
  size_t pos = 0;
  while (pos < raw.size()) {
    size_t remain = raw.size() - pos;
    size_t blk = remain < BLK ? remain : BLK;
    bool last = (pos + blk >= raw.size());
    z.push_back((char)(last ? 1 : 0));  // BFINAL(bit0) + BTYPE=00(stored)
    uint16_t len = (uint16_t)blk;
    uint16_t nlen = (uint16_t)(~(uint16_t)blk);
    z.push_back((char)(len & 0xff));  z.push_back((char)((len >> 8) & 0xff));
    z.push_back((char)(nlen & 0xff)); z.push_back((char)((nlen >> 8) & 0xff));
    z.append(raw, pos, blk);
    pos += blk;
  }
  put32be(z, adler32((const uint8_t*)raw.data(), raw.size()));
  std::string png;
  const char sig[8] = {(char)0x89,'P','N','G',(char)0x0D,(char)0x0A,(char)0x1A,(char)0x0A};
  png.append(sig, 8);
  std::string ihdr;
  put32be(ihdr, (uint32_t)width);
  put32be(ihdr, (uint32_t)height);
  ihdr.push_back((char)8);  // bit depth
  ihdr.push_back((char)6);  // color type RGBA
  ihdr.push_back((char)0);  // compression
  ihdr.push_back((char)0);  // filter
  ihdr.push_back((char)0);  // interlace
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", std::string());
  return write_file(path, png);
}

bool read_file(const std::string& path, std::string& out) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
  std::fclose(f);
  return true;
}
bool write_file(const std::string& path, const std::string& data) {
  return write_file(path, (const uint8_t*)data.data(), data.size());
}
bool write_file(const std::string& path, const uint8_t* data, size_t len) {
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  size_t w = len ? std::fwrite(data, 1, len, f) : 0;
  std::fclose(f);
  return w == len;
}

JVal json_parse(const std::string& s) { Parser p(s); return p.value(); }

std::string json_escape(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 2);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); o += b; }
        else o += (char)c;
    }
  }
  return o;
}

}  // namespace yr

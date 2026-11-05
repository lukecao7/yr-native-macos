/* SELF_AUTHORED —— 极简 JSON 解析器，仅用于 replay_host 读取自制事件序列。
 * 支持 null / bool / 整数 / 字符串 / 数组 / 对象；replay 数据不含浮点（如遇小数按整数截断）。
 * 不依赖任何第三方库（仅 C++ 标准库），满足「不装包」约束。
 */
#ifndef MINI_JSON_HPP
#define MINI_JSON_HPP

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct JVal {
  enum K { NUL, BOOL, NUM, STR, ARR, OBJ } k = NUL;
  long long num = 0;
  bool b = false;
  std::string s;
  std::vector<JVal> a;
  std::map<std::string, JVal> o;

  const JVal &at(const std::string &key) const {
    auto it = o.find(key);
    if (it == o.end()) throw std::runtime_error("missing key: " + key);
    return it->second;
  }
  int as_int() const { return static_cast<int>(num); }
};

class JParser {
  const std::string &src;
  size_t p = 0;

  void ws() {
    while (p < src.size() && std::isspace(static_cast<unsigned char>(src[p]))) ++p;
  }
  void expect(char c) {
    ws();
    if (p >= src.size() || src[p] != c)
      throw std::runtime_error(std::string("expected '") + c + "'");
    ++p;
  }
  std::string parse_str() {
    ws();
    if (p >= src.size() || src[p] != '"') throw std::runtime_error("expected string");
    ++p;
    std::string r;
    while (p < src.size() && src[p] != '"') {
      char c = src[p++];
      if (c == '\\' && p < src.size()) {
        char e = src[p++];
        switch (e) {
          case '"': r += '"'; break;
          case '\\': r += '\\'; break;
          case '/': r += '/'; break;
          case 'n': r += '\n'; break;
          case 't': r += '\t'; break;
          case 'r': r += '\r'; break;
          case 'b': r += '\b'; break;
          case 'f': r += '\f'; break;
          default: throw std::runtime_error("bad escape");
        }
      } else {
        r += c;
      }
    }
    if (p >= src.size()) throw std::runtime_error("unterminated string");
    ++p;  /* closing quote */
    return r;
  }
  JVal parse_value() {
    ws();
    if (p >= src.size()) throw std::runtime_error("unexpected end");
    char c = src[p];
    if (c == '{') return parse_object();
    if (c == '[') return parse_array();
    if (c == '"') { JVal v; v.k = JVal::STR; v.s = parse_str(); return v; }
    if (c == 't' || c == 'f') {
      JVal v; v.k = JVal::BOOL;
      if (src.compare(p, 4, "true") == 0) { v.b = true; p += 4; }
      else { v.b = false; p += 5; }
      return v;
    }
    if (c == 'n') { JVal v; v.k = JVal::NUL; p += 4; return v; }
    /* number：只取整数部分；若有小数则跳过（stoll 会自动在 '.' 处停止）。 */
    JVal v; v.k = JVal::NUM;
    size_t st = p;
    if (p < src.size() && src[p] == '-') ++p;
    while (p < src.size() && std::isdigit(static_cast<unsigned char>(src[p]))) ++p;
    if (p < src.size() && src[p] == '.') {
      ++p;
      while (p < src.size() && std::isdigit(static_cast<unsigned char>(src[p]))) ++p;
    }
    if (p == st) throw std::runtime_error("bad number");
    v.num = std::stoll(src.substr(st, p - st));
    return v;
  }
  JVal parse_array() {
    JVal v; v.k = JVal::ARR;
    expect('[');
    ws();
    if (p < src.size() && src[p] == ']') { ++p; return v; }
    while (true) {
      v.a.push_back(parse_value());
      ws();
      if (p < src.size() && src[p] == ',') { ++p; continue; }
      expect(']');
      break;
    }
    return v;
  }
  JVal parse_object() {
    JVal v; v.k = JVal::OBJ;
    expect('{');
    ws();
    if (p < src.size() && src[p] == '}') { ++p; return v; }
    while (true) {
      std::string key = parse_str();
      expect(':');
      v.o[key] = parse_value();
      ws();
      if (p < src.size() && src[p] == ',') { ++p; continue; }
      expect('}');
      break;
    }
    return v;
  }

public:
  explicit JParser(const std::string &s) : src(s) {}
  JVal parse() {
    JVal v = parse_value();
    ws();
    if (p != src.size()) throw std::runtime_error("trailing content");
    return v;
  }
};

#endif /* MINI_JSON_HPP */

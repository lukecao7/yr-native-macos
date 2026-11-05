/* replay_host —— 逐事件回放自制输入序列，输出 call_log.json。
 *
 * 链路：replay_SELF_AUTHORED.json（SELF_AUTHORED 事件序列）
 *       → yr_input_dispatch（libyrinput.a 的桥接层）
 *       → yr_world_*（由 SELF_AUTHORED_MOCK 提供，仅记录调用名/参数/返回值）
 *       → call_log.json（每次事件的函数名、参数、返回码）
 *
 * 「未与真实世界核心（p20-world-core / libyrworld.a）链接，将在第六批集成」时替换 mock。
 * 用法：./replay_host [--replay <file.json>] [--out <call_log.json>] [--no-dispatch]
 *   --no-dispatch（停止测试，Codex 第 4 次评审增补）：逐事件在 C 层真实跳过
 *   yr_input_dispatch，不产生任何 yr_world_* 调用，输出一个空的 call_log，
 *   用于验证「断开输入后，后续的世界状态变化证据必须按预期停止」。
 */
#include "../shared/yr_slice.h"
#include "mini_json.hpp"
#include "mock_world.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static std::string read_file(const char *path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

static std::string jstr(const std::string &s) {
  std::string r = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') { r += '\\'; r += c; }
    else if (c == '\n') r += "\\n";
    else r += c;
  }
  r += '"';
  return r;
}

static yr_event to_event(const JVal &e) {
  yr_event ev;
  ev.type        = e.at("type").as_int();
  ev.x           = e.at("x").as_int();
  ev.y           = e.at("y").as_int();
  ev.modifiers   = e.at("modifiers").as_int();
  ev.click_count = e.at("click_count").as_int();
  ev.key_code    = e.at("key_code").as_int();
  return ev;
}

int main(int argc, char **argv) {
  std::string replay_path = "replay_SELF_AUTHORED.json";
  std::string out_path    = "call_log.json";
  bool no_dispatch = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--replay" && i + 1 < argc) replay_path = argv[++i];
    else if (a == "--out" && i + 1 < argc) out_path = argv[++i];
    else if (a == "--no-dispatch") no_dispatch = true;
  }

  std::string data = read_file(replay_path.c_str());
  JVal root;
  try {
    root = JParser(data).parse();
  } catch (const std::exception &e) {
    std::fprintf(stderr, "replay_host: JSON parse error: %s\n", e.what());
    return 2;
  }

  yr_world *w = yr_mock_world_handle();
  yr_input_state *s = yr_input_state_create();
  yr_mock_world_log_clear();

  const JVal &seqs = root.at("sequences");
  int total_events = 0, total_calls = 0;
  std::vector<std::string> seq_blocks;

  for (size_t si = 0; si < seqs.a.size(); ++si) {
    const JVal &seq = seqs.a[si];
    std::string seq_id  = seq.at("seq_id").s;
    std::string support = seq.at("bridge_support").s;
    std::string action_id;
    if (seq.o.count("action_id") && seq.at("action_id").k == JVal::STR)
      action_id = seq.at("action_id").s;

    std::ostringstream sb;
    sb << "    {\"seq_id\": " << jstr(seq_id)
       << ", \"action_id\": " << (action_id.empty() ? "null" : jstr(action_id))
       << ", \"bridge_support\": " << jstr(support)
       << ", \"events\": [\n";

    const JVal &events = seq.at("events");
    std::vector<std::string> ev_blocks;
    for (size_t ei = 0; ei < events.a.size(); ++ei) {
      yr_event ev = to_event(events.a[ei]);
      int before = yr_mock_world_log_size();
      /* 停止测试：--no-dispatch 时真实跳过 yr_input_dispatch，绝不调用任何 yr_world_*。 */
      int r = no_dispatch ? 0 : yr_input_dispatch(w, &ev, s);
      int after = yr_mock_world_log_size();
      ++total_events;

      std::ostringstream eb;
      eb << "      {\"index\": " << static_cast<int>(ei)
         << ", \"event\": {\"type\": " << ev.type
         << ", \"x\": " << ev.x << ", \"y\": " << ev.y
         << ", \"modifiers\": " << ev.modifiers
         << ", \"click_count\": " << ev.click_count
         << ", \"key_code\": " << ev.key_code << "}"
         << ", \"dispatch_return\": " << r
         << ", \"world_calls\": [";
      for (int ci = before; ci < after; ++ci) {
        const MockCall *c = yr_mock_world_log_at(ci);
        ++total_calls;
        if (ci > before) eb << ", ";
        eb << "{\"func\": " << jstr(c->func) << ", \"args\": [";
        for (size_t k = 0; k < c->args.size(); ++k) {
          if (k) eb << ", ";
          eb << c->args[k];
        }
        eb << "], \"ret\": " << c->ret << "}";
      }
      eb << "]}";
      ev_blocks.push_back(eb.str());
    }
    for (size_t i = 0; i < ev_blocks.size(); ++i) {
      sb << ev_blocks[i];
      sb << (i + 1 < ev_blocks.size() ? ",\n" : "\n");
    }
    sb << "    ]}";
    seq_blocks.push_back(sb.str());
  }

  std::ostringstream o;
  o << "{\n"
    << "  \"schema_version\": \"1.0\",\n"
    << "  \"source\": \"SELF_AUTHORED_MOCK\",\n"
    << "  \"mock_notice\": \"yr_world_* 由 SELF_AUTHORED_MOCK 提供；未与真实世界核心链接，将在第六批集成\",\n"
    << "  \"sequences\": [\n";
  for (size_t i = 0; i < seq_blocks.size(); ++i) {
    o << seq_blocks[i];
    o << (i + 1 < seq_blocks.size() ? ",\n" : "\n");
  }
  o << "  ],\n"
    << "  \"summary\": {\"sequence_count\": " << static_cast<int>(seqs.a.size())
    << ", \"event_count\": " << total_events
    << ", \"world_call_count\": " << total_calls << "}\n"
    << "}\n";

  std::ofstream of(out_path.c_str(), std::ios::binary);
  of << o.str();
  of.close();

  if (no_dispatch)
    std::printf("replay_host: --no-dispatch ACTIVE: yr_input_dispatch SKIPPED for all events (no world calls)\n");

  /* 证明 yr_input_state_json 可独立读回（不影响 call_log.json 的结构）。 */
  char state_buf[256];
  int state_len = yr_input_state_json(s, state_buf, sizeof(state_buf));
  std::printf("replay_host: final_state_json=%s (len=%d)\n", state_buf, state_len);
  std::printf("replay_host: sequences=%d events=%d world_calls=%d -> %s\n",
              static_cast<int>(seqs.a.size()), total_events, total_calls, out_path.c_str());

  yr_input_state_destroy(s);
  return 0;
}

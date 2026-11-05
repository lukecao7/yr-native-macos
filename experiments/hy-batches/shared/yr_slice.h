/* shared/yr_slice.h — 第五批组件共享 C ABI（主对话定义，各子对话只读引用）
 *
 * 目的：让 世界核心 / 输入桥 / AppKit 上屏 三个组件可以并行开发、互不改对方文件，
 *       并在第六批由集成器链接成「同一个可执行、同一个 App」。
 *
 * 重要边界：本 ABI 以及基于它的实现是本地重建 + 自制适配层，
 *           不是《尤里的复仇》原版 exe 的导出接口，不代表原版操作语义等价。
 *           任何使用方都必须在 LIMITATIONS.md / RUN.txt 里如实写明这一点。
 *
 * 约定（ABI 语义，第五批各组件必须一致）：
 *   - 所有坐标都是地图 cell 整数坐标，左上角为 (0,0)，x 向右、y 向下；**单位 = cell，不是像素**。
 *   - 时间单位 = **逻辑 tick**，不是墙钟毫秒；FPS / 帧耗时只能在宿主主循环里另行测量。
 *   - 所有整数为宿主本机 64 位小端；JSON 用 UTF-8，不含 NaN/Infinity。
 *   - 所有返回 int 的函数：>=0 表示成功（部分函数返回具体计数），<0 表示失败。
 *   - 失败码：-1 通用失败 / -2 out 缓冲不足 / -3 threads 非 1 且非 4 /
 *     -4 参数越界 / -5 帧缓冲或尺寸不合法。
 *   - 所有写 out/cap 的函数：返回实际写入字节数（不含末尾 '\0'），
 *     cap 不足返回 -2，并在 out 写空字符串。
 *   - 帧缓冲为 RGBA8888，stride 单位为字节，允许 stride > width*4（padding 必须保留、不得覆写）。
 *     **缓冲区归属调用方**：`yr_world_render` / frame_provider 只在调用期间使用指针，
 *     不持有、不释放、不缓存；调用返回后调用方可立即复用或释放。
 *   - 线程语义：`yr_world_step` 只接受 1 或 4；4 线程必须产出与 1 线程逐字节一致的整数坐标，
 *     否则视为实现失败（不得靠改 seed 或删数据凑一致）。
 *   - 回调生命周期：`yr_shell_set_*` 注入的函数指针与 userdata 由注入方持有并保证在
 *     绘制期间有效；shell 不复制、不释放。
 *   - 未实现或证据不足的操作必须显式失败，**不得伪造世界状态变化**。
 */
#ifndef YR_SLICE_H
#define YR_SLICE_H

#include <stddef.h>
#include <stdint.h>

/* ABI 版本：集成器必须校验三组件记录的版本号一致。 */
#define YR_SLICE_ABI_VERSION 1
#define YR_SLICE_ABI_VERSION_STRING "1"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 世界核心（p20-world-core 实现） ---------------- */

typedef struct yr_world yr_world;

/* 从 P5 真实 tiles.json 建世界。tiles_json_path 必须指向
 * ../p5-isomap/tiles.json（或其只读副本）；unit_count 上限 4096；seed 决定初始布局。
 * 返回 0 成功；-1 文件/JSON 解析失败；-4 参数越界。 */
int  yr_world_create(yr_world **out, const char *tiles_json_path,
                     int unit_count, uint32_t seed);
void yr_world_destroy(yr_world *w);

/* 世界静态信息 */
int  yr_world_unit_count(const yr_world *w);
int  yr_world_map_size(const yr_world *w, int *out_w, int *out_h);
uint64_t yr_world_tick(const yr_world *w);
const char *yr_world_implementation_notice(const yr_world *w);

/* 选择：框选 [x0,x1]×[y0,y1]（端点包含，自动归一化），
 * additive=1 时并入已有选择，=0 时先清空。返回选中单位数。 */
int  yr_world_select_box(yr_world *w, int x0, int y0, int x1, int y1, int additive);
int  yr_world_selected_count(const yr_world *w);
int  yr_world_clear_selection(yr_world *w);

/* 对当前选中单位下达移动命令，返回接受命令的单位数；
 * 目标不可达 / 越界 / 无选中返回 0 或负值，并不得以"成功"计费。 */
int  yr_world_order_move(yr_world *w, int target_x, int target_y);

/* 推进 ticks 个逻辑步。每步都必须把全部单位的整数坐标追加到内部 trace，
 * 供 1/4 线程逐步一致性校验。返回实际推进步数。 */
int  yr_world_step(yr_world *w, int ticks, int threads);

/* 结构化快照 JSON。字段固定：
 * {"tick":u64,"map":{"width":int,"height":int,"source":str},
 *  "selected_count":int,
 *  "units":[{"id":int,"x":int,"y":int,"owner":int,"mission":str}]}
 * 单位数组必须按 id 升序，且长度 == unit_count。 */
int  yr_world_snapshot_json(const yr_world *w, char *out, size_t cap);

/* 逐步轨迹 JSON。字段固定：
 * {"unit_count":int,"steps":int,
 *  "per_step_sha256":[str,...],            // 每步全部整数坐标的 SHA-256（hex）
 *  "positions_le_u16_base64":str}         // steps*unit_count*2 个 int16 小端坐标拼接后 base64
 * steps 为自 reset 以来 yr_world_step 推进的总步数。 */
int  yr_world_trace_json(const yr_world *w, char *out, size_t cap);
void yr_world_reset_trace(yr_world *w);

/* 把「真实地图地表 + 当前单位」画进 RGBA8888 帧缓冲。
 * pal768 为 768 字节 6-bit 调色板（通道需 <<2）；传 NULL 时使用内置自制灰度图案。
 * 返回非背景像素数；-5 表示缓冲/尺寸不合法。
 * 地表图案若非原版 TMP，实现方必须标 SELF_AUTHORED_ART。 */
int  yr_world_render(const yr_world *w, uint8_t *rgba, int width, int height,
                     size_t stride, const uint8_t *pal768);

/* ---------------- 输入桥（p21-input-bridge 实现） ---------------- */

enum {
  YR_EVT_LEFT_DOWN = 1,
  YR_EVT_LEFT_UP = 2,
  YR_EVT_LEFT_DRAG = 3,
  YR_EVT_RIGHT_DOWN = 4,
  YR_EVT_RIGHT_UP = 5,
  YR_EVT_KEY_DOWN = 6,
  YR_EVT_KEY_UP = 7
};

enum { YR_MOD_SHIFT = 1, YR_MOD_CONTROL = 2, YR_MOD_ALT = 4 };

typedef struct {
  int type;          /* YR_EVT_* */
  int x, y;          /* cell 坐标 */
  int modifiers;     /* YR_MOD_* 按位或 */
  int click_count;
  int key_code;      /* 无按键时 -1 */
} yr_event;

typedef struct yr_input_state yr_input_state;

yr_input_state *yr_input_state_create(void);
void yr_input_state_destroy(yr_input_state *s);
void yr_input_state_reset(yr_input_state *s);

/* 消费一个事件，按需调用 yr_world_select_box / yr_world_order_move 等。
 * 返回本次产生的世界 API 调用次数；非法事件返回 -1。
 * 语义：LEFT_DOWN 起框、LEFT_DRAG 更新框、LEFT_UP 提交框选（Shift 加选）、
 *       RIGHT_DOWN 若已有选中则对落点下令移动。
 * 未实现/证据不足的操作必须返回 -1 且不得伪造世界状态变化。 */
int yr_input_dispatch(yr_world *w, const yr_event *ev, yr_input_state *s);

/* 供回放与核验：把 state 当前待提交的选择框写成 JSON，未起框时写 "{}"。 */
int yr_input_state_json(const yr_input_state *s, char *out, size_t cap);

/* ---------------- AppKit 上屏（p22-app-shell 实现） ---------------- */

/* 由 App shell 导出，供集成器注入"取下一帧"回调（可选）。
 * 未注入时 shell 使用自己编译期链接的 yr_world_render。 */
typedef int (*yr_frame_provider)(uint8_t *rgba, int width, int height,
                                 size_t stride, void *userdata);
void yr_shell_set_frame_provider(yr_frame_provider fn, void *userdata);

/* 由 App shell 导出，供集成器注入"输入事件已捕获"回调，返回是否消费。 */
typedef int (*yr_event_sink)(const yr_event *ev, void *userdata);
void yr_shell_set_event_sink(yr_event_sink fn, void *userdata);

/* 离屏核验入口：不创建窗口、不抢焦点、不激活 App，
 * 走与窗口完全相同的合成路径，产出一张 PNG。返回 0 成功。 */
int yr_shell_render_offscreen_png(const char *png_path, int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* YR_SLICE_H */

# YR Native for macOS

EA has not endorsed and does not support this product.

我们想让玩家在 Mac 上按熟悉的 Windows 操作和规则玩《命令与征服：红色警戒 2 — 尤里的复仇》，也想让上千个单位同时作战时依然流畅。欢迎做过即时战略游戏、熟悉 macOS，或愿意写下第一个回归用例的朋友一起参与。

想了解当前卡在哪里、下一步怎样验收，或挑一个适合自己的贡献，请看[开发路线图与共创任务](ROADMAP.md)。每天收工时更新的合并记录、验证结果和接手点见[每日开发记录](PROGRESS.md)。

## 历史移植目标

| 目标 | 我们想做到什么 | 当前优先级 |
| --- | --- | --- |
| 1. 原版操作与规则 | 在 Mac 原生 App 中完成对局。单位选择、下令、移动、交战和存读档的行为对齐 Windows 原版。 | **一期** |
| 2. 多核千单位 | 利用多核让上千个作战单位持续流畅运行，同时保持与串行计算一致的游戏结果。 | **一期** |
| 3. 自适应 AI | 让电脑对手依据游戏规则和战况调整策略，让进攻、防守和兵力调度更灵活。 | 后续 |
| 4. 3D | 为游戏加入三维画面。 | 后续 |

2026 年 11 月 19 日起，维护者先推进 CoastalHost 通用即时战略引擎。它使用 C++、AppKit 和 Metal，自制测试数据随模块提供。各阶段的目标和实际验收范围见[开发路线图](ROADMAP.md#coastalhost-通用即时战略引擎)。

## 这次开放了什么

当前公开项目自有的 C++ 网格导航、模拟耗时统计，以及配套的独立测试。代码来自 CoastalCommand 工程，保留原有命名空间及 GPL-3.0-only 许可。它可以单独构建，无需游戏安装、游戏素材或图形引擎。

- `native/navigation/flow_cache.*` 按目标格缓存距离场。多个单位要去同一个地点时，可以共用一次计算。地图变化后的局部修复和以多线程准备距离场也在这个模块里。
- `native/navigation/reachability.*` 判断两个格子是否连通，可以检查摆放障碍会不会切断已有通路。`nearest_walkable.*` 寻找最近的可通行格。
- `native/profiling/` 分阶段记录模拟耗时。每个工作线程分别写入计数，全部结束后再汇总。macOS 上还可以读取本进程的线程 CPU 时间。障碍网格探针可以记录网格何时改变。
- `tests/` 有六组回归测试，覆盖距离场缓存、局部修复、网格连通、最近可通行格、线程统计和障碍采样。原版寻路对齐和整场游戏性能另行验收。

## 从源码开始

需要支持 C++20 的编译器和 Make。在 macOS 上可以使用已安装的 Xcode Command Line Tools。在仓库根目录执行以下命令可生成静态库 `build/libnavigation.a`。

```sh
make
```

要运行六组独立测试，可以执行 `make test`。只检查所有源码和测试能否编译，可执行 `make check`。

导航库默认启用 `-O2`，保留严格浮点计算选项；诊断时可用 `make navigation_flags=-O0` 覆盖。切换选项后删除 `build/*.o` 与 `build/libnavigation.a` 再重建。距离场准备按请求最多使用 8 个计算线程，各线程依次领取尚未构建的目标，完成后仍按稳定目标顺序发布；并发度由调用方传入的线程数决定。测试逐项比较 1、4、8 线程的完整距离数组。

每个拓扑版本还缓存合法的八邻边，避免各个冷目标重复检查边界、斜向转角与互相允许的连接。可执行 `make navigation-bench`，用固定种子的自制 160×168 网格构建 32 个冷目标；基准分别报告配置、距离场准备和合计耗时，并独立比较全部 860,160 个距离值及 256 条完整路线。同一台 Apple M2 的一次三轮对照中，同为 `-O2` 的合计中位耗时从 19.15 ms 降至 15.32 ms，约少 20%，已计入邻边缓存初始化。此结果仅覆盖该合成场景；目标少而拓扑频繁变化时，全图预计算也有成本。修改后的 C++ 对象布局需要将库和使用者一起完整重编。

11 月 5 日另归档了 Hy 第一至第五批中可独立公开的资源解码器、原生显示适配和诊断组件，
附自制数据测试与收尾修复，见 [Hy 原型批次](experiments/hy-batches/README.md)。第五批按部分交付关闭，
第六批未启动；这些组件尚未集成为可玩的游戏，也不证明原版规则对齐或多核加速。

## macOS 独立宿主测试

在 Mac 上运行 `make macos-host-test` 可构建本机签名的宿主，检查启动参数、目录设置、只读 MIX 档案、帧调度、像素呈现及输入回放。目录测试使用自制资料和独立偏好验证首次选择、错误重选和无参数重启。宿主支持 `--data-dir`、`--skirmish-map` 和 `--strict-audit`。省略 `--data-dir` 时，宿主优先打开上次成功载入的资料目录。首次启动或资料读入失败时，可以在系统选择器中选目录，游戏读入成功后保存选择，取消则退出。显式参数只对当次运行生效。当前宿主的通用运行时读取下文的自制规则和地图。

以下命令用自制档案启动测试窗口。左键拖动绘制选框，Shift、Ctrl、Cmd 会改变边框颜色。右键清除选框并移动色块，空格切换色块颜色，滚轮平移，Ctrl 加滚轮缩放，指针靠近窗口边缘时平移。按 Cmd+Q 或关闭窗口退出。

通过 Finder 双击或点击 Dock 图标激活宿主后，macOS 确认它位于前台才会显示目录选择器或主窗口。以后台方式打开时，宿主等到主动激活后再检查已保存的目录。

支持共享帧缓冲的运行时可直接向 Metal 共享内存绘制。宿主在 Metal 无法初始化时回退到 Core Graphics。退出时终端会记录显示帧数、提交帧数、帧间隔分位数和峰值内存。Metal 显示帧间隔来自系统的 drawable 呈现回调。 共享像素槽先完成 CPU 绘图，再在编码屏幕绘制前获取显示缓冲。显示缓冲暂时不可用时，保留同一待呈现帧并重试，不额外推进对局；`drawableAcquire` 和 `encodeAndSubmit` 分别记录缓冲等待与编码提交耗时，便于分清游戏计算与显示等待。 `fullOriginalFrames` 也只累计系统实际显示、且由运行时标为完整原版画面的帧；菜单和暂停画面不计入，换局不清零。`submittedFullOriginalFrames` 单独记录提交数量。`activeObjects` 是当前画面的对象数，`lastOriginalActiveObjects` 保留最后一帧原版画面的对象数，返回大厅后可据此区分两种口径。

```sh
make macos-host-fixture
fixture_dir=$(mktemp -d)
python3 tests/macos_host_fixture_data.py "$fixture_dir"
./build/CoastalHostFixture.app/Contents/MacOS/CoastalMacHost \
  --data-dir "$fixture_dir" --skirmish-map Fixture.map
```

### 无窗口回放自制测试会话

`make macos-host-script-replay-test` 使用同一套自制测试数据，核对脚本文件和标准输入得到的帧序号与 PNG。回放只在 `CoastalHostFixture.app` 可用。宿主直接向测试会话传递输入，在指定帧写出光栅图，整个过程无需打开窗口或移动桌面指针。保留上文生成的 `fixture_dir`，为每次回放指定一个尚未创建的输出子目录。

```sh
./build/CoastalHostFixture.app/Contents/MacOS/CoastalMacHost \
  --data-dir "$fixture_dir" --skirmish-map Fixture.map \
  --script-file - --script-output "$fixture_dir/replay" <<'JSONL'
{"id":1,"op":"frame","count":1,"capture":"before.png"}
{"id":2,"op":"pointer","phase":"down","x":420,"y":280}
{"id":3,"op":"pointer","phase":"up","x":420,"y":280}
{"id":4,"op":"scroll","x":420,"y":280,"deltaY":-4.5,"modifiers":262144,"precise":true}
{"id":5,"op":"frame","count":1,"capture":"after.png"}
{"id":6,"op":"quit"}
JSONL
```

也可以将命令写到文件，并把 `--script-file -` 换成 `--script-file commands.jsonl`。每条命令返回一行 `COASTAL_SCRIPT` 和对应的 JSON。`frame` 每次推进 1 至 600 帧，`capture` 写出最近一帧，`state` 返回视口大小与当前帧号。PNG 路径相对于新输出目录，已有文件会保留。

`scroll` 需要视口坐标 `x` 和 `y`。`deltaX`、`deltaY` 默认为零，`scrollingDeltaX`、`scrollingDeltaY` 默认沿用对应的原始增量。可选字段 `modifiers`、`phase`、`momentumPhase` 默认为零，`precise`、`inverted` 默认为 `false`。上例用 Ctrl 加滚轮缩小测试色块。滚动命令直接传给当前会话，帧号在下一条 `frame` 命令执行时推进。

## 运行自制等距对局

宿主按屏幕的 `CVDisplayLink` 刷新脉冲呈现，异步会话在新帧就绪时发通知。每个脉冲最多提交一帧，空帧最多补试一次；缺帧仍反映在真实 drawable 呈现间隔中。组件保留两个就绪帧、最多四份不可变快照，并通过背压等待，不跳过模拟帧。Mac 上的生产线程使用标准 `USER_INITIATED` 优先级。

线程安全的 C++ 会话可显式调用 `make_presented_session()` 使用独立生产线程。初始化时仍在宿主线程读取视口、帧间隔和不可变地形；帧推进及输入随后由单一工作线程执行，不能同步等待 AppKit 主线程。就绪回调必须短小、不阻塞，宿主通过弱状态撤销关闭后的通知。复制到宿主外部缓冲的 2D 帧不声明自己拥有该缓冲，独立像素与 3D 场景则保留快照所有权。`make macos-host-test` 包含通知、关闭、异常、缓冲生命周期和刷新节拍回归。

`make rts-test` 会验证地图、绘制、采集、交火、存档和回放，并用脚本完整跑完一局 1v1。用同一份地图和规则可打开 Mac 窗口。

```sh
make macos-host
match_dir=$(mktemp -d)
python3 tests/rts_fixture_data.py "$match_dir"
./build/CoastalHost.app/Contents/MacOS/CoastalMacHost \
  --data-dir "$match_dir" --skirmish-map Basin.map --strict-audit
```

你操控蓝方，铜方按固定脚本生产并进攻。档案保存在数据目录的 `CoastalHost.snapshot`。右键可退出建造预览，靠近窗口边缘、用方向键或滚轮可移动相机，Ctrl 加滚轮缩放。

| 操作 | 键鼠 |
| --- | --- |
| 部署基地、选择建造位置 | D 部署。G 发电站、M 矿场、W 工坊，按键后左击地格放置。 |
| 生产与集结 | C 采集车、Q 轻型机动单位、E 重甲单位、R 远程单位。P 暂停或继续，U 取消队尾并退款，T 后左击设集结点。 |
| 选择与编队 | 点选或左键拖框，Shift 追加。Ctrl+1–9 保存编队，1–9 调用。 |
| 下令 | 右键移动，Shift+右键边走边攻击，H 驻守，空格定位所选单位或基地。 |
| 存读档 | F5 保存，F9 读取。 |

自制 `COASTAL_MAP` 文本地图使用 `.` 平地、`/` 坡道、`=` 高地、`~` 水面、`#` 障碍、`o` 矿点、`a` 和 `b` 两方出生格。`coastal_rules.tsv` 保存造价、生命、速度、视野、伤害和生产参数。

## 多核与千单位验证

一般对局可用 `--threads=1`、`--threads=4` 或 `--threads=8` 选择计算线程数。默认用单线程。模拟先根据同一时点并行计算，再按单位 ID 顺序处理移动冲突、攻击和随机数。等距画面按 64×64 像素分块，同一地图和相机在各线程配置下得到一致的像素。

```sh
make -j4 CXXFLAGS='-std=c++20 -pthread -O2' rts-stress-bench macos-host
./build/rts_core/stress_bench --threads=1,4,8 --steps=3600
./build/CoastalHost.app/Contents/MacOS/CoastalMacHost --stress --threads=8 --strict-audit
```

压力场景使用 512×512 自制岛屿和 1,200 个单位。每步有 600 个单位移动、另外 600 个单位实际开火。基准逐组计时，每 60 步比较完整存档和两方视野。压力窗口位于屏幕角落并保持可见，便于换到其他应用时继续记录实际显示帧间隔与进程峰值内存。距离数组默认共享 256 MiB 总预算，包含驻留、并行构建和修复所需的距离数组。满额后目标引用仍然有效，冷路径可以按需查询准确距离。

### 工作线程派发基准

线程池为每个持久工作线程保存独立的待办任务与条件变量，只唤醒本轮参与的线程。调用线程仍执行 lane0，并等待所有参与线程完成后返回；异常继续在全部任务汇合后按 lane 顺序报告。`rts_worker_pool_test` 增加了参与数反复变化、多线程同时抛错及随后复用的回归。

```sh
make -j4 build/rts_core/worker_pool_test rts-worker-dispatch-bench
./build/rts_core/worker_pool_test
./build/rts_core/worker_pool_dispatch_bench --rounds=2000 --label=mailbox
```

独立基准不读取游戏文件。它在1、4、8个配置线程下，分别派发2、8、1,200、12,000项固定整数工作；每组先预热100轮，再记录指定轮数。2项工作会让较大的线程池只启用部分线程。每种工作量的校验和必须与单线程一致，否则程序失败退出。JSON 行报告实际参与线程数、调用到汇合返回的总耗时、回调进入时间差，以及最后一个回调结束到返回的耗时；计时包含回调自身的两次时钟读取，关闭了线程池逐任务用量采样。

比较改动前后时，在两个隔离构建中使用同一份基准源码、相同编译器、优化选项、轮数和硬件，并交替执行多轮。输出保留系统版本、架构、编译器及计时前后各线程的身份与 QoS 读取结果。基准不设置调度策略；QoS 是端点观测，不证明整段运行的策略或核心放置，平台不支持时会明确报告。请保留完整输出和退出码，再用上面的完整压力场景检查整体影响；此微基准不代表整场模拟或显示帧率的提升。性能对照应按上述条件在目标设备复测。

## 导入地图的相邻通路

导入器可在 `Map.travel_edges` 提供每格一个字节，按 NW、N、NE、W、E、SW、S、SE 表示允许通过的相邻边。两端都允许才能通行；对角移动还要求两侧地格与四条直边可通行，避免从边角穿过障碍。数组为空时沿用原有平地通路。

导航距离场与单位实际移动使用同一组约束。只修改边也会使旧距离场失效。带边约束的地图使用 V3 存档保存这些数据，原有 V1、V2 存档仍可读取，未使用边约束时的写出格式保持不变。`make rts-test` 会验证单端关闭、绕行、边变更、读档续跑，以及 1／4／8 个实际参与线程的逐帧一致性。

## 外部动态障碍层

接入方可在导航 owner 线程调用 `Navigator::set_obstacle_mask`，传入与地图地格数量相同的字节数组；非零值表示额外阻挡。导航器会复制数组，调用方随后可以修改或释放原数组。传入空数组会清除这一层，地图自身的水面、岩石、建筑锚点和相邻通路约束仍然生效。

setter 先保存数据；下一次 `World::tick` 会更新距离场。手动调用 `next` 前须先调用 `prepare(world)`，尺寸错误也在 `prepare` 时报告。掩码由接入方保存，读档后应重新传入。若移动目标仅被这一层阻挡，单位会接近并等待；解除阻挡后才能进入目标格并结束移动命令。

## 动态移动段与外部已准备步

接入方可在导航 owner 线程调用 `Navigator::set_transient_segments`，传入仍在插值的单格移动段及其 owner，并配合 `set_transient_occupancy` 提供实际车体、起点尾部与下一格占用。每轮替换完整快照，移动结束后传入空数组或剩余段；在 `next()` 或工作线程读取前调用 `prepare(world)`。段端点须在地图内且相邻。相反对角线的内部交点会阻挡其他 owner；动态侧格本身保留绕行语义，地形、建筑、障碍层和导入边的静态转角约束继续生效。

外部朝向控制器若已选定本轮实际准备的下一步，可调用 `set_prepared_move_steps` 传入 `PreparedMoveStep{owner, next}`，再调用 `prepare(world)`。该 owner 只能提交这一步；`next=std::nullopt` 表示本轮等待，未列出的 actor 保留正常候选。每轮重新提供或清空该快照，所有 setter 和 `prepare` 都须避开工作线程读取。导航查询、worker 规划和稳定提交共享这些约束，提交阶段还会阻止同一轮新接受的对角线相交。上述信息均由接入方从当前运动状态推导，不写入对局存档。新增成员改变 `Navigator` 对象布局，库与调用方须一起完整重编。

`make rts-test` 包含自制的 `mobile_segments_test`，覆盖活动段等待与释放、owner／无效输入、外部已准备步、反向 ID、存读档续跑，以及 1／4／8 线程的逐步完整快照比较。这些用例验证通用单格移动机制；真实载具的插值、朝向和画面表现需由接入方另行验证，任意交通的活性也需单独验证。

## 闭合移动目标环与 prepare 快照

当同一方多个仍在执行普通 Move 的单位，分别把相邻另一单位的实际车体格作为目标，可能形成闭合依赖环。导航器在正常下降方向全部不可用时，只允许这个真实环中 ID 最小的成员尝试安全的等距离侧步；原目标格保留，空出原格后其他成员继续各自命令。实际车体索引由 `prepare(world)` 构建，`prepared_body_owner(cell)` 只返回唯一的实际 owner。开放链、共享的已停稳目标、仅有来源尾格、敌方成员、仍有 `move_wait` 或活动段的节点，都不触发这条路径。静态转角、已准备步、占格和段交叉约束仍参与候选过滤；没有安全出口时保留原命令等待。

接入方仍在 owner 线程逐轮调用 `set_transient_occupancy`、`set_transient_segments` 和 `prepare(world)`。`transient_owner_moving(owner)` 表示最后一次成功 prepare 中显式归属于该 owner 的活动段，包括直向和斜向段；未知 owner 不归给无关单位。setter 暂存新列表或清空请求，prepare 才发布 owner 快照，非法段被拒绝时原 owner 快照保留。所有更新须避开工作线程读取。上述索引和侧步选择都是派生结果，不增加存档字段；读档后应从恢复的状态重新准备，库和调用方也须完整重编以更新对象布局。

`make rts-test` 增加自制的 `goal_cycle_test`，覆盖 2／4 节点、反向 ID、精确到达原目标、300 步稳定性、存读档与 1／4／8 线程完整快照，以及活动段、转向等待、重复／未知 owner、暂存／发布／拒绝边界。这项修复只覆盖上述可解的闭合依赖环，不承诺任意交通都能自行解开；真实载具和画面表现仍由接入方验证。

## 视野受限的自适应对手

在自制对局命令中加 `--adaptive-ai`，铜方会依据自己的资源、已探索地形和侦察到的敌情调整生产、扩张、侧翼与集火。加 `--strict-audit` 可在终端查看已观察兵种、效用分值和生产偏好的变化。存档一并保存侦察记忆和当前集火目标。

```sh
make rts-ai-bench
./build/rts_core/ai_bench tests/fixtures/coastal_rules.tsv
```

基准用同样的起始资金与数值表，在三张自制地图上各跑十局，轮换自适应方的出生侧。每局输出种子、策略日志、结算和胜负。

## 在同一局切换 Metal 3D 视图

支持 Metal 的 Mac 窗口内按 **V** 切换等距视图和 3D 视图。3D 中用中键拖动或方向键调整角度，滚轮缩放，空格定位所选单位或基地。左键点选、拖框与右键下令仍使用对局的逻辑地格。地形、单位和水面由自制地图与几何数据生成。

可在 1,200 单位压力场景自动切换两次并记录实际显示间隔。窗口开始使用等距图，第 151 帧进入 3D，第 931 帧切回等距图。

```sh
make macos-host
./build/CoastalHost.app/Contents/MacOS/CoastalMacHost --stress --threads=8 --strict-audit --cycle-views
```

## 一起开发

动手前先查已有 Issue 和进行中的 PR，在原讨论里说明谁负责哪一部分。一个 PR 解决一个能独立验证的问题，相关测试随改动一起提交。性能改动请附地图大小、线程数、硬件及串行对照。[认领任务和拆分 PR 的方法](CONTRIBUTING.md)

可以先写一个有两条通路的例子，封住其中一条，比较局部修复与完整重算的距离场，再检查不同线程数的结果是否相同。这样的贡献不需要游戏文件，一份能稳定复现差异的用例就有价值。

本仓库提供兼容性研究中的独立代码。游戏本体、原版资源和第三方商标归各自权利人所有。请使用自制网格和有权公开的文件贡献代码与测试。代码许可见 [LICENSE](LICENSE)。

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

11 月 5 日另归档了 Hy 第一至第五批中可独立公开的资源解码器、原生显示适配和诊断组件，
附自制数据测试与收尾修复，见 [Hy 原型批次](experiments/hy-batches/README.md)。第五批按部分交付关闭，
第六批未启动；这些组件尚未集成为可玩的游戏，也不证明原版规则对齐或多核加速。

## macOS 独立宿主测试

在 Mac 上运行 `make macos-host-test` 可构建本机签名的宿主，检查启动参数、目录设置、只读 MIX 档案、帧调度、像素呈现及输入回放。目录测试用两个进程验证首次选择持久化和无目录参数重启。宿主支持 `--data-dir`、`--skirmish-map` 和 `--strict-audit`。省略 `--data-dir` 时，宿主优先打开上次通过系统选择器选定的目录。首次启动或目录失效时会打开选择器。显式参数只对当次运行生效。当前宿主的通用运行时读取下文的自制规则和地图。

以下命令用自制档案启动测试窗口。左键拖动绘制选框，Shift、Ctrl、Cmd 会改变边框颜色。右键清除选框并移动色块，空格切换色块颜色，滚轮平移，Ctrl 加滚轮缩放，指针靠近窗口边缘时平移。按 Cmd+Q 或关闭窗口退出。

支持共享帧缓冲的运行时可直接向 Metal 共享内存绘制。宿主在 Metal 无法初始化时回退到 Core Graphics。退出时终端会记录显示帧数、提交帧数、帧间隔分位数和峰值内存。Metal 显示帧间隔来自系统的 drawable 呈现回调。

```sh
make macos-host-fixture
fixture_dir=$(mktemp -d)
python3 tests/macos_host_fixture_data.py "$fixture_dir"
./build/CoastalHostFixture.app/Contents/MacOS/CoastalMacHost \
  --data-dir "$fixture_dir" --skirmish-map Fixture.map
```

## 运行自制等距对局

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

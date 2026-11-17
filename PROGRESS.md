# 每日开发记录

按北京时间记录实际工作的日期，最近的在前。收工时由维护者核对当天可公开的代码、测试和受影响的说明。每条日结列出已合并的 PR、验证结果，以及还有谁在做什么。跨日任务留在对应的 Issue 或草稿 PR，方便下一位贡献者接手。[开发路线图](ROADMAP.md) · [认领任务](CONTRIBUTING.md)

## 2026-11-18

**首次矿石增长写入** Apple Silicon 无窗口实测在原版 MonsterM 和默认规则下完成双方开局及预置对象加载，再调用原版增长逻辑。首类队列从 462 项降至 461 项，实际队首指向原图 (10,74) 的同一个地格。原版八位加法将矿量从 6 写成 7，上限处理再写回 7，两次写入均由原地格回读确认。同一场景随机数游标从 81 走到 82，帧号保持 0。运行约 327 秒，在后续绘制矩形查询处结束，原生局部量、运行时和进程清理通过。剩余增长批次、矿石蔓延和完整对局帧仍待验证。 [矿物增长 PR #89](https://github.com/lukecao7/yr-native-macos/pull/89)

**原版矿石增长调用返回** Apple Silicon 无窗口实测保留原版 MonsterM、默认规则、双方开局和全部预置对象。原版从 462 项增长堆中按规则和当次随机数选出 23 格，逐格完成增长写回、本体及原版空阴影矩形、刷新区域登记和调用返回。其中 6 格由 10 增至 11、达到上限，17 格重新排队；增长堆最终为 456 项，记录游标从 462 增至 479，随机数游标从 81 到 99。刷新向量实际扩容至 30 项，登记 23 个区域，增长堆顺序、成员标志和全部区域覆盖均核对通过。随后四种矿石的计时按当前场景倍率 1，分别写回规则中的 2,200、10,000、2,200、2,200 帧间隔，其余三类空队列按原路径返回。原版矿石增长函数已执行返回，诊断在该返回点结束，帧号仍为 0。矿石蔓延调用、屏幕刷新和完整对局帧仍待验证。原生运行、局部量及进程清理通过。 [完整增长调用 PR #93](https://github.com/lukecao7/yr-native-macos/pull/93) · [前置首个迭代 PR #92](https://github.com/lukecao7/yr-native-macos/pull/92)

**原版矿石扩散选格** 矿石增长与步兵通用 AI 合并后的同一份构建通过两项无窗口回归，关键读回与此前已验证版本一致。后续扩散候选在原版 MonsterM 和默认规则下，从实际增长返回后的同一张地图继续执行扩散。原版从 708 项扩散堆中弹出首格 (10,74)，本次随机批次为 9 格；当前只执行到首格的选格阶段。补齐十二类地形的原版 Buildable 规则读取后，八次相邻地格查询和扩散资格判断均返回，其中两格合格。原版随机选出起始方向，按顺序重新查询五次，选中 (9,74) 的实际地格，随机数游标由 99 走到 101。运行在新矿石写入入口结束，尚未创建新矿石，扩散批次和扩散函数均未返回，完整对局帧仍为 0。原生运行和进程清理通过。后续由维护者 `@lukecao7` 在 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) 接续新矿石写入、窗口复验与完整对局帧。

**首名中立步兵的区域警戒** Apple Silicon 无窗口实测保留原版 MonsterM、默认规则、双方开局部队及全部预置对象。按原版顺序完成十二棵矿树更新后，首名中立步兵读取同一地格、被动索敌许可、45 帧的搜索计时和实际行走对象。它当时未移动，任务目标为空。副武器和 Warhead 从同一份默认规则与地图读入后，区域警戒返回 39 帧的下次更新间隔。后续同图复验将该间隔写回同一实体的原版任务计时，任务函数返回通用 AI 的后续入口，写回前后随机数游标一致。后续对象和完整对局帧仍待验证。运行正常退出，原生运行时和进程已回收。

**同一步兵的通用 AI 接续** 无窗口实测从同一名中立步兵的实际任务返回继续执行。原版索敌计时当前剩余 45 帧，随后从同一类型读到自愈已开启。修复周期经原规则读取、浮点乘法和取整得到 14 帧；当前帧余数为 0。原版比较确认该实体生命值与地图覆盖后的上限均为 1,000，返回本次不回血，生命值保持原值。同图复验完成隐形条件及两次普通等级比较，原版隐形状态函数返回。再次读取同一实体的当前阵营和位置后，原版查询从同一个地格读回该阵营的视野位。目标为空，原动画计时器按原分支处理后，程序进入小地图更新。后续通用 AI 和完整对局帧仍待验证。原生运行、局部量及进程清理通过。 [通用 AI 接续 PR #90](https://github.com/lukecao7/yr-native-macos/pull/90)

**原版 Scene 绘制表面** 独立带窗口实测接通真实窗口参数、原版表面构造、软件清屏和静态绘制，沿用同一块 RGB565 画布。系统锁屏后，该轮等待窗口首帧呈现，随后结束并清理进程。本轮的路线像素和鼠标操作待桌面恢复后复验。

**接续** 主线维护者 `@lukecao7` 继续接入通用 AI 后续更新、正式帧内输入和整张地图的对局循环。普通启动仍需要与固定版本匹配的启动器数据。千单位多核对局仍待验证。 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) · [本轮记录 PR #87](https://github.com/lukecao7/yr-native-macos/pull/87) · [任务计时 PR #88](https://github.com/lukecao7/yr-native-macos/pull/88)

## 2026-11-17

**原图灰熊首动** 同一张 MonsterM 使用默认规则，带窗口诊断完成原版小地图初始化。两组系统鼠标事件先选中本方一辆灰熊，再向北侧相邻格发送 Move。原版消费一条指令，随后同一辆坦克的 Y 坐标减少了 26 个游戏坐标单位，X 和高度保持原值。同一个窗口沿用开局相机重绘，车身从窗口坐标 (514,588) 移到 (517,586)。回读的 1024×768 视口与实际绘制缓冲逐像素一致。车辆更新在位移之后停住，完整对局帧仍待实测。窗口、进程和系统登记已清理。

**车辆更新与目的地路线** 晚间同场复验让同一灰熊的完整原版车辆更新返回，随后回读两遍混合物件绘制和同一窗口中的实际车身位移。原版目的地路线读取同一目标地格及当前计时，在同一视野算出两端位置。实际路线像素和完整对局帧仍待验证。同图寻路区域初始化从一次实测的约 232 秒降到约 32 秒，三档区域计数保持一致。 [PR #86](https://github.com/lukecao7/yr-native-macos/pull/86)

**接续** 主线维护者 `@lukecao7` 负责普通启动和当前地图的完整对局帧。[Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10)

## 2026-11-16

本轮合入了 MonsterM 的雷达、首条步兵记录、原图规则覆盖、实际玩家阵营、首个步兵的构造与入场，以及阵营和完整地形同场的路线图。 [PR #37](https://github.com/lukecao7/yr-native-macos/pull/37) · [PR #38](https://github.com/lukecao7/yr-native-macos/pull/38) · [PR #39](https://github.com/lukecao7/yr-native-macos/pull/39) · [PR #40](https://github.com/lukecao7/yr-native-macos/pull/40) · [PR #41](https://github.com/lukecao7/yr-native-macos/pull/41) · [PR #42](https://github.com/lukecao7/yr-native-macos/pull/42) · [PR #43](https://github.com/lukecao7/yr-native-macos/pull/43) · [PR #44](https://github.com/lukecao7/yr-native-macos/pull/44)

带窗口的 Apple Silicon 运行从原图建立了实际雷达表面，遍历 11,398 个地格并计算 9,642 格颜色。独立无窗口运行完成首个预置步兵的原版入场，核实了同一实体的原图坐标、子格占用、绘制图层、逻辑登记和周围八格计数。最后一次无窗口运行在玩家、一个所选 AI 和中立阵营都存在时，复验了 1,290 个覆盖物、11,398 格属性、298 个地形对象及两组矿石队列。同次完成首个步兵的原版入场和绘制排序，地面层共 299 个对象，步兵顺序与原版对照一致。运行后受管进程全部清理。

**预置步兵与触发条目** 后续无窗口实测在 11,398 格的同一 MonsterM 上放置了全部 9 个原图预置步兵和 298 个地形对象。读取原图 11 个标记、11 个触发器、11 个事件和 51 项动作。同一运行核对了地图规则覆盖和矿石队列，正常退出并清理。触发器在完整对局中的行为仍待验证。 [PR #46](https://github.com/lukecao7/yr-native-macos/pull/46) · [PR #47](https://github.com/lukecao7/yr-native-macos/pull/47) · [PR #48](https://github.com/lukecao7/yr-native-macos/pull/48) · [PR #49](https://github.com/lukecao7/yr-native-macos/pull/49)

**遭遇战读图和开局** 无窗口实测登记一名玩家、一个所选 AI、Neutral 和 Special，加载九种原版游戏模式及十四个国家。Battle 设置成为当前模式，原版选项函数读完本机配置并正常返回。另一次运行回读开局单位数 10、资金 10,000 和科技等级 10，把同一玩家登记到本局。 [PR #50](https://github.com/lukecao7/yr-native-macos/pull/50) · [PR #51](https://github.com/lukecao7/yr-native-macos/pull/51) · [PR #52](https://github.com/lukecao7/yr-native-macos/pull/52) · [PR #53](https://github.com/lukecao7/yr-native-macos/pull/53) · [PR #54](https://github.com/lukecao7/yr-native-macos/pull/54) · [PR #55](https://github.com/lukecao7/yr-native-macos/pull/55) · [PR #56](https://github.com/lukecao7/yr-native-macos/pull/56) · [PR #57](https://github.com/lukecao7/yr-native-macos/pull/57) · [PR #58](https://github.com/lukecao7/yr-native-macos/pull/58)

**起点与灰熊坦克** 使用地图选择器的受控场景和明确的阵营、颜色选项。在原图的八个开局坐标中，真人按原版抽取 (128,79)，AI 从余下位置择远到 (26,77)。实测把坐标写入各自阵营。后续使用实际原图和同一规则读入 80 种载具及 65 种步兵，按开局条件选出灰熊坦克和一项步兵候选，计算开局预算 4,500。读入原版车体、炮塔和炮管后，实测由原版工厂创建本方灰熊，读回生命值 300、造价 700，并正常退出。后续一次运行同时复验全部覆盖物、地格属性、298 个地形对象及矿石队列，补齐同层地形读取后，最后一轮在原图让同一辆灰熊经原版入场落到 (128,79)，登记到地面绘制层，从开局预算扣减 700，并设为原版 Guard。整轮约 8 分 51 秒，正常退出及清理。此轮基地车和可见游戏帧仍待。 [PR #59](https://github.com/lukecao7/yr-native-macos/pull/59) · [PR #60](https://github.com/lukecao7/yr-native-macos/pull/60) · [PR #61](https://github.com/lukecao7/yr-native-macos/pull/61) · [PR #62](https://github.com/lukecao7/yr-native-macos/pull/62) · [PR #63](https://github.com/lukecao7/yr-native-macos/pull/63) · [PR #64](https://github.com/lukecao7/yr-native-macos/pull/64) · [PR #65](https://github.com/lukecao7/yr-native-macos/pull/65)

**开局部队和原版相机** Apple Silicon 带窗口测试在同一张原版 MonsterM 使用默认 Battle 规则和受控阵营及颜色。真人与 AI 各生成一辆基地车、五辆灰熊和五名 GI，开局预算各从 4,500 扣到 0，九名原图预置步兵仍在图上。相机读取真人十一件开局单位的位置，在绑定真实 Mac 窗口的同一个 1024×768 绘制表面画出 951 格地表。后续同场测试遍历地面层 329 个对象，绘出当前视野的 21 棵静态树，颜色和深度各有 12,447 个像素发生变化。原版树木阴影分支全部返回，阴影像素变化为零。同一窗口表面绘出首辆开局灰熊的车体、炮塔、炮管和阴影，测得 964 个颜色像素变化。绘制对象是原图 (132,83) 的同一辆坦克。该轮结束时，原版运行时和 Mac 进程均已清理。 [PR #67](https://github.com/lukecao7/yr-native-macos/pull/67) · [PR #68](https://github.com/lukecao7/yr-native-macos/pull/68) · [PR #69](https://github.com/lukecao7/yr-native-macos/pull/69) · [PR #70](https://github.com/lukecao7/yr-native-macos/pull/70) · [PR #72](https://github.com/lukecao7/yr-native-macos/pull/72) · [PR #73](https://github.com/lukecao7/yr-native-macos/pull/73) · [PR #74](https://github.com/lukecao7/yr-native-macos/pull/74) · [PR #75](https://github.com/lukecao7/yr-native-macos/pull/75)

**坦克命中表与步兵动画** 同一窗口诊断将首辆灰熊登记到原版鼠标命中表，读回窗口坐标 (514,588)。首名 GI 从实际动作序列和朝向得到原版 GI.SHP 的帧索引 7，配色来自本方阵营及同一窗口的颜色转换。同一 GI 的绘制入口接到原版载入状态读取。该轮随后接续了步兵像素绘制和原生鼠标点选。 [PR #76](https://github.com/lukecao7/yr-native-macos/pull/76) · [PR #77](https://github.com/lukecao7/yr-native-macos/pull/77)

**原图第二遍地格** 在一次独立无窗口运行中，原版流程完成两遍各 11,398 格，将 1,273 个含矿地格的总价值 429,175 写回同一张地图。独立读取原图重算得到同一结果。加载进度、信标和完整读图返回待接续。 [PR #71](https://github.com/lukecao7/yr-native-macos/pull/71)

**原版开局画面和点选** Apple Silicon 上的同一次带窗口诊断跑完原版两遍混合物件层、效果和战术绘制尾段，并解锁原来那块绘制表面。恢复原版树木默认耐久后，视野里的 21 棵静态树绘出树冠和阴影。1024×768 的窗口视口和原版绘制缓冲逐像素一致。向窗口发出一次原生鼠标按下和抬起，原版选择逻辑正常返回，选择表只有画面里的同一辆本方灰熊。运行正常退出，窗口、进程和系统登记清空。 [PR #79](https://github.com/lukecao7/yr-native-macos/pull/79) · [PR #80](https://github.com/lukecao7/yr-native-macos/pull/80) · [PR #81](https://github.com/lukecao7/yr-native-macos/pull/81) · [PR #82](https://github.com/lukecao7/yr-native-macos/pull/82) · [PR #83](https://github.com/lukecao7/yr-native-macos/pull/83)

**原版寻路和开局逻辑** 在同一次带窗口开局完成两遍各 11,398 格更新及原版三档通行区域计算。原版选择流程随后通过系统鼠标选中画面里的灰熊。原版通行判断用实际选中的坦克和同一张地图的寻路表比较，确认它所在格与北侧相邻格同属一个普通地面通行区。这次未投递地面 Move。另一次同图测试按原版顺序完成 12 棵矿树的逻辑更新。首名预置步兵保持原任务，通用 AI 读回它的军衔和所在物理格，确认当时未隐藏。随后复验完成同一对象的计时累加和对象 AI，进入原版区域警戒任务。完整对局逻辑帧待接续。

**原图移动指令** 带窗口诊断在失焦后恢复前台。同一个 Mac 窗口接收两组系统鼠标事件，先选中本方灰熊，再向北侧相邻格发出 Move。原版消费了发给同一辆坦克的指令。首次车辆更新停在缺失的小地图绘图表面，原坐标保持不变。

**接续** 主线维护者 `@lukecao7` 继续接入普通启动和原版对局帧，验证默认规则下的 Move。正常启动仍缺与固定游戏版本匹配的启动器数据。验收需要在同一个正常启动的 Mac App 原版 MonsterM 对局中完成真实点选、原版 Move 消费、同一辆坦克的实际位移及下一幅画面。千单位的多核对局仍待验证。 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10)

## 2026-11-15

**MonsterM 覆盖物** Apple Silicon 的一次无窗口实测使用默认规则，按原图顺序处理了全部 1,290 个覆盖物条目、10 种类型，经过矿石、宝石、岩石和坡道分支。地图指定的帧数据写回并逐格读回了 11,398 个原地格。随后释放了 1,290 个临时对象，待删除队列和活跃对象表都已清空。测试用时约 5 分 35 秒，进程正常退出。 [PR #33](https://github.com/lukecao7/yr-native-macos/pull/33)

**全图地格属性** Apple Silicon 的一次无窗口测试复验全部覆盖物后，沿原版地图游标完成 11,398 个原图地格的属性计算。每一格都确认了原地格的身份，游标走到末尾。应用用时约 8 分 12 秒，运行时、加载文件和受管进程均已回收。 [PR #34](https://github.com/lukecao7/yr-native-macos/pull/34)

**原图地形对象** Apple Silicon 的一次无窗口实测从默认规则建立了 78 个地形对象类型，读取原图用到的五种温带素材，按原图顺序放置了全部 298 个地形对象。逐个回读中心地格、八个邻格与绘制图层，其中 12 棵矿石树进入原版逻辑更新表。随后在同一张地图完成矿脉初始化。运行约 8 分 26 秒，进程正常退出。 [PR #35](https://github.com/lukecao7/yr-native-macos/pull/35)

**矿石队列** Apple Silicon 的无窗口实测再次完成全图地形对象放置，随后按原版顺序初始化四个默认矿种的生长和蔓延队列。读回 462 个生长候选格和 708 个蔓延候选格，与单独用原图和默认规则核对的数量一致。运行约 15 分 13 秒，进程正常退出。 [PR #36](https://github.com/lukecao7/yr-native-macos/pull/36)

**接续** 维护者继续接入原版加载进度、雷达和对局初始化。普通启动仍需与所用版本匹配的启动器数据。玩家真实鼠标选中并移动单位，仍待在普通启动的同一次 MonsterM 对局里验收。 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10)

## 2026-11-11

**普通启动** 在 macOS 的正常前台启动测试中，App 读到实际鼠标指标，完成版本检查并创建原版窗口。原版窗口处理了 macOS 发出的首次激活通知。未取得启动器数据时，原版等待在 60.004 秒后进入失败分支，Mac App 自行退出，进程和系统登记已清空。正常启动读入原版地图还缺与所用版本匹配的启动器数据。

**地图地形** 受控 Mac App 在无界面和带窗口两种运行中均读到原版 MonsterM 并通过摘要比较。两次都建立了 11,398 个引擎地格和四组寻路数组，再将 16 个压缩块解开的地形记录写回同一批地格。带窗口的一轮约 14 秒，停在原版加载进度调用前，未显示游戏画面。文件和窗口均已关闭，进程与系统登记已清空。普通启动中的地图加载和对局初始化还在接入。

**温带地形类型** Apple Silicon 的无窗口测试在原版 MonsterM 的同一地格表上完成 82 套温带地形加载，建立 838 个主地形条目和 220 个变体。原版流程完成地形索引写入并关闭四个加载文件，同一次运行随后写回 16 个压缩块中的 11,398 个地格。应用停在原版加载进度调用前，正常退出。

**地形素材与地图边界** 随后的无窗口测试在同一张 MonsterM 地格表上继续运行原版逻辑，核对 11,398 个地格使用的地形，按需读入 136 份地形素材，算出地图边界并释放加载缓冲。应用约 64 秒后正常退出。测试直接调用了读图后半段，普通启动中的加载进度和对局入口仍待验证。

**原版地图地格查询** Apple Silicon 的无窗口测试把 MonsterM 已加载的 11,398 个地格接入 Mac 版地图查询。查询原版 1 号起点及相邻格，均返回刚加载的原地格和对应绘制数据。测试约 65 秒后正常退出。下一步是加载原版覆盖物、计算地格属性并让玩家单位入场。

**地格属性的输入** 同一次 Apple Silicon 无窗口测试从原版 MonsterM 解出覆盖物类型和帧两张表，每张 262,144 字节，与独立解码结果一致。起点两格的覆盖物条目为空，并从同一份原地形素材读到平地和 60×30 的图块尺寸。另一个原版坡地读到坡度 3 和 60×45 的尺寸。应用约 65 秒后正常退出。覆盖物对象放置、整图地格属性写回及玩家单位入场仍待验证。

**原版起点的属性写回** 无窗口测试在同一次 MonsterM 运行中加载默认规则的 250 项覆盖物类型，并用地图编号回读到首个类型对象。原版地格属性逻辑在 1 号起点和相邻格完成，地形、坡度和地格分类写回原来的地格，缓存保留原高度。覆盖物对象放置和整图属性写回仍待验证。

**原版航点** 同一次无窗口测试用原版航点读取逻辑写回场景的 702 个位置。其中 37 个原图航点都在同一批原地格上写入标记。起点地格属性和首个覆盖物文件复验通过，测试约 68 秒后正常退出。

**首个覆盖物的素材** 同一 Mac 测试沿原图编号和默认 Art 选择温带图像文件。实际读取的 4,336 字节与本机原版档案成员一致。文件头记录 24 帧，地图指定的第 6 帧边界核对通过。文件完成清理，起点两格属性复验通过。原版类型的图像绑定和覆盖物放置仍待接入。

**有限场景移动** 在同一个 Mac 测试窗口，真实鼠标事件选中一辆坦克，原版处理了发给它的一条 Move。坦克的世界 X 坐标从 32896 增至 32922，紧邻的一帧经原版绘制显示同一辆坦克向右移 3 像素、向下移 1 像素。先关闭移动提示动画验证，再用未改动的原版规则复验。第二轮的两张实际画面都绘制了原版移动提示，间隔约 53 秒。两轮均调整了有限场景的坦克入场朝向，均在后续模拟停止后清空进程与系统登记。

**默认规则的绘图设备** 在隔离实机测试中，原版主表面的创建和描述读取均已返回。读回的表面为 800×600、16 位 RGB565，每行 1,600 字节。随后用原版动画调色板完成一次动画转换表构造，写入 13,568 个表项。移动提示动画的普通帧已在有限场景的实机窗口显示，压缩帧通过了独立绘制核对。

**同一签名构建的两项实测** 同一份 Apple Silicon 构建先在无窗口测试中完成原版 MonsterM 起点两格的属性写回，约 66 秒后正常退出。随后用同一构建运行默认规则的有限场景，使用朝东的坦克入场参数。真实鼠标选中坦克并点击地面后，原版消费一条 Move，同一辆坦克的世界 X 坐标连续从 32896 变为 32922、32949。相邻画面显示了两次位移，间隔约 39 秒和 7.5 秒。四张画面的像素与前次测试一致。有限场景按预定四帧正常退出，窗口、进程组和系统登记均已回收。普通启动后的 MonsterM 对局仍在接入。

**本轮收尾** 实机验证已结束。维护者在 Demo 任务中跟踪配套启动数据、原版地图覆盖物放置、玩家单位入场和第一条移动命令。[Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) · [PR #18](https://github.com/lukecao7/yr-native-macos/pull/18) · [PR #19](https://github.com/lukecao7/yr-native-macos/pull/19) · [PR #20](https://github.com/lukecao7/yr-native-macos/pull/20) · [PR #21](https://github.com/lukecao7/yr-native-macos/pull/21) · [PR #22](https://github.com/lukecao7/yr-native-macos/pull/22) · [PR #23](https://github.com/lukecao7/yr-native-macos/pull/23) · [PR #24](https://github.com/lukecao7/yr-native-macos/pull/24) · [PR #25](https://github.com/lukecao7/yr-native-macos/pull/25) · [PR #26](https://github.com/lukecao7/yr-native-macos/pull/26) · [PR #27](https://github.com/lukecao7/yr-native-macos/pull/27) · [PR #28](https://github.com/lukecao7/yr-native-macos/pull/28) · [PR #29](https://github.com/lukecao7/yr-native-macos/pull/29) · [PR #30](https://github.com/lukecao7/yr-native-macos/pull/30) · [PR #31](https://github.com/lukecao7/yr-native-macos/pull/31) · [PR #32](https://github.com/lukecao7/yr-native-macos/pull/32)

## 2026-11-10

**地图解码。** [PR #9](https://github.com/lukecao7/yr-native-macos/pull/9) 支持完整地形记录后的四字节零结束标记。自制回归通过，覆盖非法结束标记、越界、重复坐标和缺格。独立解码器另在本地温带地图上核对了 11,398 个地形格。

**Mac 真机测试。** [PR #11](https://github.com/lukecao7/yr-native-macos/pull/11) 记录了启动时的资源校验栈溢出和修复。修复版通过了资源、语言、输入和控件版本检查。原版 800×600 普通窗口实际显示并返回，记录见 [PR #12](https://github.com/lukecao7/yr-native-macos/pull/12)。测试让 App 离开再回到前台，原版收到真实激活事件，继续运行到启动器的命名事件握手。应用随后按正常的 Mac 退出流程关闭，窗口、进程组和系统登记已清空。

**启动器回包。** [PR #13](https://github.com/lukecao7/yr-native-macos/pull/13) 记录同一 Mac App 在真机完成事件打开、发信和关闭，实际回读到本次请求的服务回复，并在退出时释放模拟对象。

**原版窗口自行激活。** 受控小窗候选从启动到回包全程无需切换到其他应用。原版窗口向系统发起激活，收到实际通知，并完成重绘回调。程序在等待原版共享映射的位置按计划退出，窗口、进程和系统登记已清空。这轮实测约 24 秒。[PR #14](https://github.com/lukecao7/yr-native-macos/pull/14)

**原版资源读回。** Mac 独立测试从两层游戏资源包读回 2,718 字节主题配置。另一项测试从选中的本机目录读回两份原版动画调色板，每份 768 字节。三份数据的摘要与原件一致。调色板的热读返回同一缓存引用，节点和文件均完成清理。主题配置与动画调色板在正常场景中的接入仍待验证。[PR #15](https://github.com/lukecao7/yr-native-macos/pull/15)

**有限场景首帧。** Apple Silicon 真机完成三层区域计数和寻路数组初始化，两辆坦克经原版入场与绘制调用出现在同一测试窗口。启动约 6 分 56 秒后保存 1024×768 视口，测试按计划正常退出，系统登记已清空。本轮验收范围为有限测试场景的首帧。选中、移动和正常启动后载入原版地图仍在 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) 跟踪。 [PR #16](https://github.com/lukecao7/yr-native-macos/pull/16)

**有限场景原生输入。** 在 Apple Silicon 真机上，同一个 Mac App 使用原版规则运行有限测试场景，通过原生鼠标事件选中一辆坦克。随后点击地面，游戏逻辑从队列取出并处理了一条发给这辆坦克的移动命令。下一帧在绘制移动提示动画时停在 `0x4AEE5A`。命令处理前后及停下时，坦克的世界坐标一致。后续继续验证实际位移和画面更新。 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) · [PR #17](https://github.com/lukecao7/yr-native-macos/pull/17)

**主线接续。** 维护者在 [Demo 任务 #10](https://github.com/lukecao7/yr-native-macos/issues/10) 继续验证与原版安装匹配的共享数据，并把实际地图接入引擎。第二个资源包的本机打开和地图地形包读取候选已通过编译，仍待 App 运行验证。本机已核对原图的一处出生候选和相邻目标格，后续在同一 App 的实际地图中验证落位、选中和移动。

## 2026-11-05

**今天交付了什么。** [PR #6](https://github.com/lukecao7/yr-native-macos/pull/6) 归档 Hy 原型批次中可独立公开的资源解码、离屏显示和诊断组件。修复对象生成与移动占位重叠、框选释放终点、干净构建依赖，以及总核验中的字节数、哈希与负面测试误判。源码、来源清单与运行方法见 [原型批次目录](experiments/hy-batches/README.md)。

**验证到哪一步。** 公开包 7 组自制数据／离屏测试、错误判定回归和仓库原有 6 组测试均通过。自制世界的 1200 个对象在 360 步单／4 线程对照中共有 432,000 对坐标一致，占位与逐步哈希检查通过。本地第五批汇总 83/85；地图入世界证据和实体注册交付两项未补齐，按未完成保留。4 线程没有加速保证，没有执行完整对局或长期 App 性能验收。

**后续独立维护。** [PR #7](https://github.com/lukecao7/yr-native-macos/pull/7) 修复 P22 的 provider 失败仍导出全透明 PNG 并返回成功；[PR #8](https://github.com/lukecao7/yr-native-macos/pull/8) 修复同类检查中发现的 P3 短写误报。P22 合成失败返回 `-7`；P22、P3 均先编码再检查原子写入，写入失败保留原文件。回归先在旧实现上复现失败，再验证修复：P22 干净构建、provider 与短写回归、92 项原有核验及 6 项篡改检测通过；P3 干净构建、11 项写入检查及 128000 像素固定值对照通过。仅运行自制输入的离屏进程，真实窗口、内存分配失败和断电未验证。本次独立维护已完成，未恢复主线自动开发或第六批。

**本批关闭。** 第一至第四批保留局部交付，第五批按部分交付关闭。第六批尚未开发，本次不接续；主线自动开发仍暂停，没有新认领的开发任务。原版素材、提取物、私有记录和发布权未确认的还原代码仍只保留在本地。

## 2026-11-02

**今天合入了什么。** 网格导航模块、四组网格回归测试和独立构建说明已公开。另一个 PR 加入了按线程统计模拟耗时的代码和两组测试。公开的 11 份模块源码与集成版本逐字节一致，六组测试都已提供源码，本次发布未重新运行。[导航 PR #1](https://github.com/lukecao7/yr-native-macos/pull/1) · [耗时统计 PR #2](https://github.com/lukecao7/yr-native-macos/pull/2)

**移植主线验证到哪一步。** 地图加载器已接入 `MISSIONMD.INI` 文件可用性判断。7 个受影响的 C++ 编译单元通过，171 个工程对象完成链接。如果资源包未命中，仍缺少该临时文件的磁盘读取凭据。真实文件读取、完整对局和同一 Mac App 的千单位性能仍待验证。[路线图 PR #3](https://github.com/lukecao7/yr-native-macos/pull/3)

**交接给谁。** 本轮自动开发已暂停，当前没有工程工作线在执行。恢复后由主线从临时文件的读取权限接续。社区贡献可先查开放的 Issue 和 PR，在原任务下认领具体范围。[协作规则 PR #4](https://github.com/lukecao7/yr-native-macos/pull/4) · [开放的 Issue](https://github.com/lukecao7/yr-native-macos/issues) · [开放的 PR](https://github.com/lukecao7/yr-native-macos/pulls)

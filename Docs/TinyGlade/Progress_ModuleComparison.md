# Tiny Glade 模块对照与进度（合卷）

自监督循环的**状态文件**（卷零）与**模块对照文档**（卷一～卷五）的合卷。
每一轮迭代开始时读卷零、结束时更新卷零 —— 没有它，循环会失忆重做。

设计裁决在 [`Plan_House.md`](Plan_House.md)，逆向证据在
[`Review_ReverseComparison.md`](Review_ReverseComparison.md)，全部文档的地图在 [`index.md`](index.md)。

> **阅读范围（2026-09-30 清理）**：卷零的模块表维护当前状态；已推翻的实施步骤已收敛为裁决摘要。
> 卷一～卷五保留当时的逆向证据和对照分析，其中日期、行号、测试数量及行动项属于历史快照。
> 历史章节中的「未拍板 / 未实现」不能直接当作当前待办；现行裁决看 `Plan_House.md` 与各专题文档。

| 卷 | 内容 | 合并前的文件名 |
| --- | --- | --- |
| [卷零](#vol-0) | 完成进度与循环协议 | `TinyGlade_模块对照进度.md` |
| [卷一](#vol-1) | 拉尺寸（D5）与接缝角柱（D7）对照 | `TinyGlade_拉尺寸与接缝对照.md` |
| [卷二](#vol-2) | 窗户（D8）与装饰／藤蔓（D12/D13）对照 | `TinyGlade_窗户与装饰对照.md` |
| [卷三](#vol-3) | 楼梯模块对照 | `TinyGlade_楼梯模块对照.md` |
| [卷四](#vol-4) | 渲染与光照对照 | `TinyGlade_渲染光照对照.md` |
| [卷五](#vol-5) | 砖构件对照：门框 / 转角 / 垛口 / 接缝 / 上沿 | *（本轮新增，无前身）* |

---

<a id="vol-0"></a>

## 卷零 · 完成进度与循环协议

> 原文件 `TinyGlade_模块对照进度.md`，原标题「Tiny Glade 模块对照与完成进度」。

自监督循环的**状态文件**。每一轮迭代开始时读它、结束时更新它 —— 没有它，循环会失忆重做。

设计裁决在 [`Plan_House.md`](Plan_House.md)，这里只记**对照结论**与**完成进度**。

### 循环协议

每轮：

1. 读本文件，挑一个 `状态 = 待办` 且优先级最高的模块。
2. 派一个 subagent 做**对照研究**（TG 怎么做 / 本项目怎么做 / 差在哪 / 取舍建议），产出或更新该模块的对照文档。
3. 研究结论若指向可落地的改动，**再派一个 agent 实施**，或自己实施。
4. **验收门（每轮必须全绿才算这一轮完成）**：全量构建 + 单测 + 演示回归。任一红灯就先修，不推进新模块。
5. 更新本文件的状态与结论摘要。

**不做的事**：不推翻已记录的用户裁决（发现该翻时，先写进「待用户拍板」一节，不擅自改）。

### 证据来源（按可信度）

| 来源 | 路径 | 说明 |
| --- | --- | --- |
| PDB 符号 | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt` | 97033 条。**最硬的证据** —— 系统的参数签名直接说明它读什么写什么 |
| 逆向分析 | `D:/MyProject/Tiny Glade/MESH_GENERATION_ANALYSIS.md` | 461 行，自带 `【确凿】/【推测】/【待确认】` 标注，沿用这套 |
| 两轮对照评审 | [`Review_ReverseComparison.md`](Review_ReverseComparison.md)（同目录，两轮已合卷） | 含**被否条目**与「看似该抄其实不该抄」清单，别重复推翻 |
| 提取资产 | `Content/HouseTest/TinyGladeAsset/`（`/PCGPlugins/HouseTest/TinyGladeAsset`） | 网格/贴图/材质实例。尺寸一律实测，不猜 |
| 反编译着色器 | `D:/MyProject/Tiny Glade/tmp/shaders` | GLSL 实证 |
| **exe 反汇编** | `tiny-glade.exe` + `tiny_glade.pdb`（同目录） | **2026-09-06 新开**。比 PDB 签名更硬：签名只说“读什么写什么”，反汇编说**做了什么**。用法见下 |
| 模块对照文档 | **本文卷一～卷四**（四份对照已并入本文，调研面已闭合） | 本循环自产。结论前置 + A1..An 行动项，**已被本文引用的部分都经驱动方复核** |
| Houdini 原型 | `D:/MyProject/Houdini/TinyGlade/TinyGlade.hip` | 用户的意图原型，对照写法见 `Ground_Shaper.md`（同目录） |

解 PDB 系统签名的用法（已验证）：找到含系统名的 `bevy_ecs::label::impl$0::as_any<...FunctionSystem<...>>` 行，
`sed -n '<行号>p' pdb_symbols.txt | tr ',' '\n' | grep -oE "(EventReader|EventWriter|Res|ResMut|Query)<[a-zA-Z_:]+"`。

**反汇编的用法（2026-09-06 打通，一趟 15 秒）**：这台机器**没装** Ghidra / IDA / radare2，但 MSVC 的
`dumpbin` 就够 —— PDB 就在 exe 旁边，它能解 Rust 的 mangled 名、标出**函数边界**与**调用目标**：

```powershell
& "<VS>/VC/Tools/MSVC/<ver>/bin/Hostx64/x64/dumpbin.exe" /DISASM:NOBYTES "<TG>/tiny-glade.exe" | python grab_funcs.py > out.asm
```

`grab_funcs.py` 是个**流式**过滤器（十几行）：顶格且以 `:` 结尾的行是函数标签，按关键词只留命中的函数体。
58 MB 的 exe 反汇编出来上 GB，流式处理**不落全量磁盘** —— 实测 15 秒、输出 400 KB、一次捞 8 个函数。

⚠️ 两条读法上的坑：
- 浮点常量是 `__real@<hex>` 的**单精度**，且 **TG 的单位是米**：
  `struct.unpack('<f', struct.pack('<I', 0x3fb70a3f))` → 1.43，正是 rank-2 窗宽 143.5 cm。
  不换算的话会把 0.9 当成 90 而不是 90 cm，整条推理跟着歪。
- 函数可能被**内联**掉（`mirror_holes_as_needed` 就搜不到）。搜不到 ≠ 不存在。

### 模块状态

全文唯一的模块状态表（2026-09-29 由原卷零两张表与 `index.md`「完成进度速览」合并；落地位置 2026-09-18 按源码核对过）。逐模块的来龙去脉在下文按日期排的各小节里。

| 模块 | 状态 | 落地位置 | TG 对照结论 |
| --- | --- | --- | --- |
| D1 地面 / D2 顶点色笔刷 / D3 直推通知 | 已落地；地面材质已接（顶点色 R 混草地 / 土路） | `CSGroundActor.{h,cpp}` | — |
| D4 房屋 + 屋面 | 屋面已落地：四坡 + 瓦（2026-08-31，双坡实体板整套删除），自动档的脊向由长轴导出、平局归 X（用户已裁掉裁决四，`RidgeSwitchRatio` 滞回随之删除）。2026-10-02 脊长改为可调参数（`RoofRidgeLength01`，脊端抓手），等于房长时双坡 + 三角形山墙，见 [`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md#脊长可调与双坡山墙2026-10-02)。墙体按 2026-09-05 两层裁决（砖层 + 灰泥层，见「挖洞策略改成 TG 的真两层」）；2026-09-22 起房子墙体迁到 `CSWall`，见 [`Plan_Wall.md`](Plan_Wall.md) 第 4 步 | `CSHouseActor.{h,cpp}`、`CSHouseRoof.{h,cpp}`、`CSHouseTile.{h,cpp,usf}`、`CSHouseRidgeHandleActor.{h,cpp}` | TG 的脊长是存盘参数 `ridge_length_01`（长宽比的连续函数只是新建时的默认值），恰好为 1 即山墙；没有「翻轴」事件，换脊向是显式操作。[附录 F](RE_RoofRidgeGable.md) |
| D4 屋面收尾（瓦厚 / 密度 / 脊瓦 / 尖顶） | 2026-08-31 落地：瓦厚 6 cm、排距定档 827 片 / 栋、五条脊线盖脊瓦、脊端点立尖顶 | `CSHouseTile.cpp`、`ACSHouseActor::RebuildRoofFinials` | — |
| D4 屋顶露台 / 高度框 / 屋脊收口 / 瓦片 VSM | 2026-09-20 落地 | 见 [`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md) | — |
| D4 房体底面 | 2026-09-17 落地：墙内皮围出的那块在房底封上，朝下单面，开关 `bBottomFace` | `CSHouseActor.cpp`（`CSHouse_BuildBodySoup`） | — |
| D5 拉尺寸 | 2026-08-31 机制层 + 2026-09-05 交互层，整条已通：单边推拉纯函数 + `PushEdge` + `EnterResizeMode` 生成的抓手 actor（标准 gizmo 拖，失选自动退出）。尺寸禁带随四坡屋顶同日删除 | `CSHouseResize.h`、`CSHouseResizeHandleActor.{h,cpp}`、`CSHouseResizeSelectionWatcher.{h,cpp}` | 不做自定义 EdMode / HitProxy（用户裁决） |
| D6 门洞 | 已落地（逐像素 clip + 门框砖）；2026-09-04 重做触发与宽度口径：门宽 = 路在墙上截出的弦长，过宽切成拱廊 | `CSHouseDoorRuns.h`、`CSHouseFrame.{h,cpp}`、`CSHouseFrame.usf` | TG 的门拱也用逐像素 clip，但同时靠 `flags&32` 把洞缘砖**贴合**过去；2026-09-05 起照抄那套四级。触发规则的分歧已于 09-04 证伪（TG 的拱就是道路驱动），见「待用户拍板」 |
| D6 门扇 | 2026-09-04 新增：一洞一个静态网格组件，按洞宽缩放 | `CSHouseActor::RebuildDoorLeaves`、`Scripts/TinyGladeSetupDoorLeaf.py` | — |
| D7 接缝（形状相交） | 2026-08-31 纯函数接缝砖；2026-09-16 改为接触记录：两端共持、谁动谁发、松手提交、一条缝只砌一次、柱逐接触出（不跨接触合并）+ TG 两条过滤；复制出的房子 `HouseId` 查重。横缝只判定不出产物（09-17 的承托梁已被用户判「TG 中没有这个结构」退回） | `CSHouseContact.{h,cpp}`、`CSHouseSeam.h` | TG 在形状相交处**先开洞再砌缝砖**，全库无角柱 / 墙裙 / 脊瓦预制件，全是一个 `brick` |
| D7 转角角石 / 包边石（A8） / 墙-顶收边 | 2026-08-31 落地：四角竖直砖柱（与接缝柱共用 `CSHouseFrame::AppendColumn`）；墙顶压顶 + 墙脚勒脚（共用 `AppendFlatRun`，勒脚按洞切段）；檐口 / 山墙 / 屋脊三处收边 | `CSHouseQuoin.h`、`CSHouseTrim.h` | — |
| D8 窗 | 2026-08-31 整个模块合上；2026-09-06 交互层重定向（附属物自带 mesh、锚点是权威、窗不出框砖）；2026-09-16 窗贴墙脚变门。转角窗 2026-09-05 永久否决 | `CSHouseProfile.h`、`CSHouseFeatureMarker.{h,cpp}`；逐条见 [`House_Window.md`](House_Window.md) | TG 的窗不是 clip，是 CPU 裁砖 + 预制窗框盖缝；两层之后本项目也走 CPU 裁砖，且多一级洞缘砖双向贴合 |
| D9 承重柱 + 塑形物 | 链 A + 链 B 全部落地（裙边噪声、二次抬升、披挂岩壳、石头隆起六参数 —— 同日晚裁决默认归中性、TG 基准偏移 + 坡度比例起伏）。岩壳材质裁决六已执行（`M_TG_Texture` 翻 `MSM_DefaultLit`） | `CSGroundShaperActor.cpp`、`CSGroundRockShell.{cpp,usf}` | 图案 2026-09-30 起改程序化胞腔（`RockShellCellSize` 193.6 cm = 0.35 × TG 间距、M = 16、稀疏槽位池，[`RockShell_Sparse.md`](RockShell_Sparse.md)）；平面长度随胞腔缩、竖向（厚度 / 基准偏移 / 起伏）是世界 cm；`rocky_terrain.y/.z` 与水体仍缺 |
| D10 subsystem | 2026-09-16 晚删除：名单 = `UCSHouseLibrary::GetHouses`（`TActorRange`），快扫由根组件 `TransformUpdated` 取代，找宿主 / 放窗搬到 `UCSHouseLibrary` | `CSHouseLibrary.{h,cpp}` | — |
| D11 Spline 块排布 | 已落地，被石阶与门框复用 | `SolveBlockLayout` | — |
| D12 摆件 | 锚点层 2026-08-31 落地（五家锚点）；复杂度场那一半按 C2 有意未做 | `CSHouseDecor.{h,cpp,usf}`、`CSGroundDecor.{h,cpp}` | TG 是七家锚点生产者 + 候选点烘在资产里；「复杂度场」在 TG 无对位物 |
| D13 藤蔓 | 第一档已落地（枝 319 / 叶 216）；花 / 三季 / 上屋顶 / wall_jump 未做 | `CSHouseVine.{h,cpp,usf}` | — |
| D14 渲染与光照 | VSM 已关 + 观感一轮已落地（天光真因、下半球色、曝光钉死、石材、埋深闭式解）；C-R1 已拍板（clip 是正式路线、代理不许有真洞） | `Scripts/TinyGladeSetupLighting.py` | TG 是完整的现代延迟渲染器（2×uint32 G-buffer、三级联 PCSS、半分辨 GTAO、ReSTIR GI + L1 探针、Tony McMapface），观感来自算法不是资产 |
| 墙面材质（灰泥剥落 + 凸出砖） | 2026-09-03 建图 / 09-04 引擎验收；补了 `PeelBias`（顶替 TG 缺失的覆盖度项）与四个默认值改档 | `Scripts/TinyGladeMakeWallMaterials.py`、`M_TinyGladeWall` | TG `_nani_plaster` 的移植，两处有意不同构 |
| 地形石阶 S1 + S2 + S3 | 已落地：marching squares 等值线 + 定容 + `InterlockedAdd`；S3 = 旧路（`BuildStepPlan` / `CSGroundSteps.usf`）已删，`RDG_SmoothSpline` / `SolveBlockLayout` 另有消费者故保留，`CSGroundStairs.usf` 里剩一个无人调用的 `StairInstanceRandom` | `CSGroundStairs.{h,cpp,usf}` | — |
| 玩家绘制楼梯（TG §4.1） | 2026-09-16 MVP；2026-09-20 栏杆 / 木梯 / 侧面拱墙 / 露台接合 / 宽度把手 / 样条实时重建。挂墙托架、穿墙洞、分叉图未做 | `CSStairs.{h,cpp}`、`CSStairsActor.{h,cpp}`；见 [`Stairs_PlayerStairs.md`](Stairs_PlayerStairs.md)、[附录 D](RE_PlayerStairs.md) | 门前踏步（§4.0）挂在窗变成的门上 |

验收门历史：2026-08-31 晚跑过两轮 —— 岩壳体积那一轮单测 103 绿 0 红、回归 `passed=225 failed=0`；D8 收口那一轮单测 105 绿 0 红、回归 `passed=231 failed=0` / `REGRESS OK`；自动化测试文件数 61 → 92，回归 demo 段增到 11 个（`demo_house_window` / `demo_house_seam` / `demo_house_resize` / `demo_house_decor` / `demo_skirt_decor`）。

### 已定裁决

本表所列事项均已有结论，不再作为等待拍板的阻塞项。仍未实施的工作按模块状态表与专题文档跟进。

| 编号 | 一句话 | 卡住什么 | 状态 |
| --- | --- | --- | --- |
| **岩壳稀疏化（大世界）** | 图案只有一张不可平铺的 TG 原件（×0.35 只盖 47.8 m 见方），每次整张重披挂；稀疏方案把图案换成程序化胞腔、只给裙边环带分配槽位 | D9 链 B 的图案来源（改裁决一后半句「静态资产 / TG 原件」）、观感默认档、容量策略、P3 页大小 | ✅ **2026-09-30 用户裁决：程序化、M = 16**；其余三条按推荐落地（V = 0、拖动期不扩容、MaxCells 8192、P3 未做），P1 已落地，见 [`RockShell_Sparse.md`](RockShell_Sparse.md#裁决2026-09-30) |
| **两层挖洞 vs 裁决三** | 砖层在洞处**不摆砖**，算不算「真几何洞」 | 砖层的实现口径 | ✅ **2026-09-05 当日澄清**：用户裁决**在材质中挖洞、以及删除 instance，都不算修改了几何**。裁决三禁的是「切 / 砌出带洞的网格」（布尔、剖面扫出的洞壁）——渲染层 discard 与不发实例都在允许范围内，**两层方案完整符合裁决三**。只有「几何上永远是实心盒」这句措辞不再准确 |
| **门洞触发规则** | TG 的拱由道路驱动，拱宽取路在墙上截出的段长 | D6 的语义 | ✅ **不必再拍板**（触发规则本来就同构）；改为「宽度/位置的口径与 TG 不同」，见「门洞：TG 确实是道路驱动」一节 |
| **岩壳换 Houdini 新配方** | 用户已在 Houdini 重构链 B（配方存档于 `Ground_Shaper.md` 重构节） | D9 链 B 的图案与 kernel | ✅ **已裁决（2026-08-31 稍晚）：保留 UE 大体方案**（"算法上比较干净"）。只取一个点子落地：裙圈倾斜（`RockShellSkirtTilt`，见下节）。Houdini 配方留档不移植 |
| **`LiftHeight` 语义** | 二次抬升让台顶 = `LiftHeight × 1.021`，属性名不再等于台顶高 | D9 的参数口径 | ✅ **裁决七**：选甲 —— 照原型留 2% 溢出，代码零改动 |
| **门框设施冲突** | 石阶 S3 要删的 `EnsureCapacity`/`RDG_SmoothSpline`，正是门框砖的地基 | 石阶 S3、门框重构 | ✅ **裁决一**：选乙 —— 删旧路，门框改 100% GPU 解析推导 |
| **C-D7-1 + C-D7-2** | 相交处开不开洞、接缝是 actor 还是纯函数 | D7、多栋房叠放 | ✅ **裁决二**：只产生接缝砖，其余完全独立；actor 形态否决 |
| **（新）真几何洞** | 墙到底挖不挖真洞 | D6/D7/D8/D14 全线 | ✅ **裁决三**：**避免所有真几何洞**，一律渲染层挖 |
| **C-D5-1** | 四坡屋顶脊向由长轴派生，脊长在正方形处收为 0 | D5 拉尺寸的观感 | ✅ 2026-08-31 已取代旧裁决四；离散脊向、滞回与尺寸禁带已删除。2026-10-02 起只是**自动档**的行为，见下一行 |
| **脊长可调 / 双坡山墙** | 脊长是玩家可拖的参数；等于房长时四坡变双坡、端墙变三角形；照 TG 反汇编 | D4 屋面与墙顶 | ✅ **2026-10-02 用户裁决并落地**：`RoofRidgeLength01`（< 0 = 自动档，旧存档不变）、`RoofRidgeAxis`、`GableRoofStyle`；[附录 F](RE_RoofRidgeGable.md) |
| **C2** | D12 的「复杂度场」在 TG 无对位物 | D12 的措辞 | ✅ **裁决五**：用户"随便" ⇒ 保留自有场，只订正措辞 |
| **C-R1** | 计划 D14 正文写的是已被你推翻的「真几何洞」路线 | D14 的 GI 代理 | ✅ 随裁决三关闭：正文已订正；**代理不许有真洞** |
| **岩壳材质 unlit** | `MI_rocky_terrain` 挂在 `MSM_Unlit` 的 `M_TG_Texture` 下 | D9 观感 | ✅ **裁决六，已执行**：`M_TG_Texture` 本体翻成 DefaultLit |

#### 2026-08-30 第二批裁决（用户原话逐条落成规格）

> 用户原话：「路和石阶需要把地基的门框砖删了。两栋房交汇时只产生接缝砖，其它任何内容都是独立的。
> 避免所有真几何洞，使用顶点色或者类似门洞的方式在渲染层面挖洞。房屋尺寸更换时有最小距离，
> 避免产生翻轴现象。C2 随便，我不是那么在乎。材质不要使用 unlit —— 我在最后会把所有内容保存为
> StaticMesh，当前阶段我只需要法线和颜色贴图对就行。」

**裁决一（门框设施冲突 → 选乙，已完成）**：门框改为 GPU 解析推导；旧塑形物石阶路与 `legacy_shaper_steps()` 夹具随后删除。地形石阶由地面统一生成，见计划 D9 的 S1–S3。

**裁决二（接缝）**：两栋房的产物保持独立，接缝不建独立 actor。2026-09-16 后以 `FCSHouseContact` 接触记录表达两端关系，谁动谁发、拖动期冻结、松手提交；旧的“零共享状态”说法已不适用。横缝不生成承托梁，现行裁决与派生规则见计划 D7。

**裁决三（全局，最硬的一条）**：**避免所有真几何洞。** 所有洞 —— 门拱、窗、楼梯穿墙、形状相交处 ——
一律用**顶点色**或**门洞那套逐像素 clip 场**在渲染层挖；洞内不发砖实例也符合 09-05 的澄清，禁止的是布尔或剖面重切出带洞网格。

- 这把 `CSHouseActor.cpp:44-45` 那句「洞由材质逐像素 discard 切出来，几何上不挖」从"暂时安全的偏差"
  **升格为正式架构不变量**，对所有新模块生效。
- **代价明写**（计划 D14 那张表逐行核对过 UE 5.7.4 源码，不是猜的）：网格距离场、软件 Lumen、
  Lumen 硬件光追**都把 masked 洞当实心墙** ⇒ **拱门在 GI 里永远没有洞**，光不会从拱洞洒进室内、
  DFAO 把拱周围当实墙压暗。用户已知情接受（与 2026-08-29「gpumesh 进不了 lumen 那就无视它」同向）。
- ⇒ **C-R1 随之关闭**：D14 正文改成 clip 路线，且**删掉「代理另建带真洞低模」那条出路** —— 它本身就是真几何洞。

**裁决四的替代结论（2026-08-31）**：采用四坡屋顶，`FCSRoofDesc::bRidgeAlongX()` 从长轴派生脊向，正方形处脊长连续收为 0。
离散脊向、`RidgeSwitchRatio` 与尺寸禁带已删除；只保留与翻轴无关的 `MinFootprint` 硬下界。
验证关注跨越正方形时的连续性，见 `House.TilePyramid` 与 `demo_house_resize()`；不再要求旧的翻转计数单测。
2026-10-02 起这是**自动档**（`RoofRidgeLength01 < 0`）的结论：脊长另有显式档，脊向存成 `RoofRidgeAxis`、只由用户换，仍然没有自动翻轴。

**裁决五（C2）**：用户「随便，我不是那么在乎」⇒ **保留复杂度场这个自有设计**，只把计划 D12 里
"依据 TG"的措辞订正成"本项目自有；TG 用的是七家锚点 + 烘在资产里的候选点"。锚点叠加案不做。
⚠️ 但楼梯 A6 的 `SuppressedDerived`（TG 的 `DeletedAutoClutter` + 钉住机制）**仍是独立一条**，
不随 C2 关闭 —— 做 D12 或楼梯时再一起定。

**裁决六（材质，全项目纪律 + 终局约束）**：**任何交付路径上的材质都不许是 `MSM_Unlit`。**

- ✅ **立即执行面已完成（2026-08-30 同日）**，但**做法与本条原先写的不同** —— 用户追加指令
  「帮我把材质都改为默认光照」，所以**没有另建 lit 母材质**，而是直接把 `M_TG_Texture` 本体
  翻成 `MSM_DefaultLit`。「`M_TG_Texture` 从此只许 gallery 用」那句**随之作废**：它现在是 lit 的，
  岩壳继续用 `MI_rocky_terrain` 即可，459 个 MI 一次性全部受光。
  - ⚠️ **只翻 shading model 会让整库变纯黑且不报错**：贴图原本接在 `EmissiveColor` 上。
    实际改动是四步 —— 贴图（参数名 `Tex`）搬到 `BaseColor`、`EmissiveColor` 钉成 0 常数
    （5.7.4 的 Python API **没有** `disconnect_material_property`，接 0 是等效且显式的做法）、
    `Roughness` 接 0.85 常数（不接则引擎默认 0.5，看着发亮像塑料）、新增法线参数
    `NormalTexture`（默认 `default_normal`，**先验过它确实是 `TC_NORMALMAP` 才接** ——
    默认值错的话 459 个 MI 会一起拿到错法线，比不接法线糟得多）。
  - 全仓扫过 49 张母材质，TG 交付路径上 unlit 的**只有这一张**。另外四张 unlit 都是别的子系统的
    调试/可视化材质，**有意不动**：`M_SimpleBrush`（笔刷叠加）、`M_Bound`（布尔包围盒）、
    `M_TransformTest`、`M_Color`（space colonization 调试色）—— 可视化材质受光反而是错的。
  - 藤蔓那两个本来就合规（`M_TinyGladeIvyBranch/Leaf` 是 `MSM_TWO_SIDED_FOLIAGE`）。
- **当前阶段验收面收窄**：只要求**法线正确 + 颜色贴图正确**，粗糙度 / 金属度 / AO / 次表面一律不追。
  ⇒ 「无窗/玻璃母材质」这条缺口按同一口径补：法线 + 颜色够用，`is_glass` 顶点流暂不消费。
- **新增终局约束：用户最后会把所有内容保存为 StaticMesh。** 影响三条：
  ① 每一类 GPU 生成物（房体、门框砖、石阶、岩壳、藤）都必须有一条走得通的 `SaveToStaticMesh` 出口 ——
  别再写出「只有 gpumesh 代理能画」的路；
  ② 顶点色通道字典与多组 UV 必须随网格一起保住（烘完 **clip 洞仍靠材质 discard，几何仍是实心** ——
  这是裁决三的直接后果，材质必须随资产一起带走）；
  ③ 材质必须是能直接挂在 StaticMesh 上的普通材质，**不能依赖只有 gpumesh 代理才提供的逐图元数据**
  （D14 通道一那 36 float `CustomPrimitiveData` 属于此列 —— 用它做纯外观量可以，但别让几何正确性依赖它）。
- ⚠️ **顺带**：这条也给「离屏 SceneCapture 画不出 GPU 实例」松了绑 —— 终局资产是 StaticMesh，
  那条 bug 只影响开发期的视觉验证，不影响交付物。**但它仍然没修**，开发期验证照旧只能靠 readback 断言。


#### ❌ 已推翻：「门洞的触发规则与 TG 不同」（2026-08-30 的结论，2026-09-04 证伪）

**用户实测先给的信号**：在 TG 里画路穿墙，*路只擦过墙一点点时，拱非常窄*。这与"TG 的拱与道路无关"
直接冲突，回头复核 PDB —— **旧结论错了，而且错在方法上**。

**旧结论错在哪（两处，都值得记）**：

1. **引错了系统。** 旧结论把 `construct_gates` 当成"产门洞的"，但符号表里它整族都是**木门扇**：
   `country_core::components::WoodenGate`、`construct_gates::construct_wooden_gate_doors`、
   `DoorMeshInProgress::add_quad`、`door_mesh` / `door_mesh_gap`、`is_sewer_gate`、
   `Gates::clear`。它是往已经存在的洞里安一扇木门，不是开洞的那一步。
2. **只看了系统自己的参数表，没看上游谁写了它的输入。** 拱那个系统的签名确实一个 path 都没有
   （见下），但它读的 `PublicWalls` / `WallPathSegmentationMasksMinusStairs` 是被上游一个
   **读路径栅格的系统**写进去的。"签名里没有 ⇒ 与它无关"这一步推理不成立。

**真链路**（PDB 里带完整泛型的 `IntoScheduleConfigs::into_configs<void (*)(...)>` 实例化，
参数表是逐字的，不是推测）：

```text
① (PathRastersResources, ResMut<PublicWalls>, EventReader<OnWallChanged>, Res<UiSignifierStream>)
   ⇒ 唯一读路径栅格又写 PublicWalls 的墙系统。按名字与排除法即
     `system_wall_constructor::segmentation::sample_wall_path_intersections`
     ——「采样墙 × 路径的交集」，把路在这面墙上截出的区间写回 PublicWalls。
② calculate_wall_path_segmentation
   (OnWallChanged, ResMut<PublicWalls>, ResMut<WallPathSegmentationMasks>, Res<UiSignifierStream>)
   ⇒ 把①的区间变成沿墙的一维 mask；`score_split_candidate` / `add_splits_in_empty_range`
     / `process_segment` 在 mask 上**打分求解**分割点。
③ split_wall_path_segment_into_platform_stairs ⇒ WallPathSegmentationMasksMinusStairs（挖掉楼梯占的区间）
④ 拱系统 (OnWallChanged, CreateRoofCmd, SwitchRoofTypeCmd, ResMut<PublicWalls>,
   ResMut<WallPathSegmentationMasksMinusStairs>, Res<AssetSsboLibrary>, Res<UiSignifierStream>,
   EventWriter<ConstructGateCmd>, ResMut<ArchSegments>, ResMut<BuildingHadArchesLastFrame>,
   EventWriter<RecalculateRoofCmd>, Query<&Roof>)
   ⇒ 段 → ArchSegments，并发 ConstructGateCmd 去安木门扇。
⑤ construct_arches → create_stone_arch_profile + ArchFunction::remap_t + ArchWalker::walk 摆拱圈石。
```

**最硬的一条**：`ArchSegment` 的构造函数签名是 **`ArchSegment (*)(WallPathSegment)`** ——
拱是由**墙路径的一段**直接变成的。而 `WallPathSegment` 自带 `length_ws` / `length_ws_2d`。
⇒ **拱宽 = 那一段的世界长度**，也就是路在墙上截出来的弦长。路只擦过一点点 ⇒ 段很短 ⇒ 拱很窄。
用户的实测与这条逐字对得上。

**所以待拍板的那条问题作废了**：「画路穿墙 → 自动开拱」**本来就是 TG 的规则**，不是本项目的自有发明。
剩下的分歧不在"要不要道路驱动"，而在**口径**：

| | 本项目 | TG |
| --- | --- | --- |
| 路的连续值 | `Road >= DoorOnWeight` **当场二值化**（`CSHouseActor.cpp:441`），只留"几个采样点算路" | 路径栅格是抗锯齿覆盖率（`zeno` 光栅化），交集区间是连续量 |
| 分段 | 与路无关的**等分**：`N = round((边长−2×60)/150)`，`Pitch = 可用长/N` | 由路×墙的交集 mask **打分求解**分割点 |
| 拱宽 | `(Pitch − PierWidth) × 离地收窄`，与路无关，量化到 2 cm | **= 段长 = 路的弦长**，连续 |
| 拱位 | 槽心 `S0 + Pitch/2` | 段心 = 交集区间中心 |
| 拱高 | 恒 = 半宽（正半圆） | `ArchType` + `InnerWalls::update_arch_height` + `is_default_arch_height`：独立状态，可非默认 |
| 段的类型 | 恒为拱 | `ArchSegments` / **`LintelSegments`** 两种 |
| 门扇 | **没有这个概念**（洞里只有门框砖） | `construct_gates` 按 `segment_length` **现搭** quad 网格，随洞宽连续变宽；只有把手是预制件 |

**木门扇也走同一条宽度源（用户 2026-09-04 补充实测：门洞与木门**都**随画路由窄变宽）**：
`construct_gates` 里有 `segment_length`，`MaterialStats` 有 `add_gate_length` 与
`set_gates_segments_count` —— 门按**段**计数、按**长度**计量。而门扇本身是**现搭的**，不是缩放预制件：
`DoorMeshInProgress::add_quad` + `door_mesh` / `door_mesh_gap` 逐 quad 拼出来。
资产侧对得上：`extracted/meshes/wooden_gate/` 里**只有** `door_handle_circle.glb` 与 `ladder.glb`，
**没有门扇网格**（板材是 `wooden_plank*.glb`，下水道栅是 `sewage_gate_bar.glb`）。
⇒ 洞与门扇共用同一个 `WallPathSegment` 的长度，两者都随路连续变宽，且拖动期每帧重建。

本项目已于 2026-09-04 加入门扇，后续换成 TG 真门并接入洞表；现状见下文「D6 重做」与「D6 续」。TG 的板条组装证据保留用于对照，不再表述为本项目“没有门扇”。

⚠️ **这一节推翻的只是「与道路无关」。** 下面这些旧证据仍然成立、别一起扔掉：
`BuildingHadArchesLastFrame` 确实是跨「围合体→建筑」的滞回；TG 的 "wall path" 确实指墙自己画出的曲线
（`WallPathSegment`）而不是小路 —— 只不过**切段的依据里包含了小路**；地形石阶那条
`rocky>0.2 && smoothstep(0.075,0.125,path)>0.99 && water<0.5` 也照旧。

**方法论教训（比结论本身值钱）**：Bevy 的 system 签名只说明**直接**依赖。任何"X 与 Y 无关"的结论，
必须再查一遍"谁写了 X 读的那几个 Resource"。这次是靠 PDB 里的
`into_configs<void (*)(...)>` 泛型实例化拿到逐字参数表反查出来的 —— 那些行**比裸符号名信息量高一个量级**，
以后先 grep 它们。

#### 已关闭的早期冲突

- **门洞与道路**：TG 的 `WallPathSegmentationMasksMinusStairs` 来自路径栅格；仅看直接 system 参数曾得出错误结论，前文已保留完整证据。旧的三选一方案撤销。
- **门框与旧石阶设施**：门框已迁到解析 GPU 路，塑形物的 `BuildStepPlan` / `RebuildSteps` 及旧测试夹具已删除，不再等待裁决。
- **C2 复杂度场**：保留为本项目设计，尚未实施；TG 的七家锚点与资产候选点是独立证据。删除派生物 / 钉住机制仍按 D12 与楼梯专题跟进。
- **C-R1 光照代理**：沿用渲染层 clip，禁止另建带真洞的代理；具体支持与限制以 D14 和 [`GpuMeshRayTracing.md`](../GpuMeshRayTracing.md) 为准。旧的“等待订正文档”不是待办。

### C1 已拍板：窗户谓词降维成一维 S 区间（2026-08-30）— 正文已并入 [House_Window.md](House_Window.md#c1-已拍板窗户谓词降维成一维-s-区间2026-08-30)

### 楼梯与接缝的现行入口

- 玩家绘制楼梯已由 `ACSStairsActor` 实现，包含宽度把手、栏杆、拱墙与露台接合，见 [`Stairs_PlayerStairs.md`](Stairs_PlayerStairs.md)；TG 的四套楼梯模型与证据保留在 [卷三](#vol-3) 和 [`RE_PlayerStairs.md`](RE_PlayerStairs.md)。
- 地形石阶使用 `CSGroundStairs.usf` 的 marching squares，旧 `BuildStepPlan` 不再是可复用模板。
- 屋脊连续性已于 2026-08-31 定案，见上方裁决四的替代结论。
- 接缝采用 `FCSHouseContact` 接触记录，由两端共持、松手提交；横缝只判定，不生成已被用户否决的承托梁。现行行为以计划 D7 为准。

### 石阶模块收尾（2026-08-30）

#### 旧石阶路已删除

塑形物的 `RebuildSteps` / `BuildStepPlan`、闭式环半径与 `legacy_shaper_steps()` 夹具均已随 S3 删除。
现在由地面持有唯一的 GPU marching-squares 石阶路；不再通过清空旧调色板关闭第二套绘制。

#### TG 的 15% 小石子已移植

资产 `stairs_pebble` **本来就在**（S2 那轮报"未移植"时不知道）。
随机源与 S2 **同一套格身份哈希**，只换盐（89 抽签 / 97 弦上落点 / 101 缩放 / 103 PerInstanceRandom），
**没有碰 `InterlockedAdd` 的槽位**。实测 `stairs=164 pebbles=25 (15.2%) matched=25 size=[30.2, 51.6] cm`。

**一处刻意偏离 TG（已写进注释）**：石子的 Z 在落点**重新求一次解析场**，而不是照抄石阶的 `Level` ——
弦是等值线的**割线**，弦中段真实地面低几厘米，照抄会让**孤立的小球浮起来**（实测 2.72 cm）。
石阶本身是块状看不出来，单独一颗球就露馅。

判据做成同机位对照组（`StairPebbleChance` 0.15 → 0，别的一个字节不动）：0.851% 像素变化。

#### 裸 flush 纳入计数：31 处，无断言变红

统一改走 `UCSMesh::CountedBlockingFlush()`，**只加计数不改行为**（它们大多是离线/编辑器工具路径，
本来就该阻塞）。七条零阻塞断言仍全是 0。

#### 三条订正（其中两条是驱动方记错的）

1. **`CSGpuMeshComponent.cpp` 的 `-SingleFile` 本来就是坏的** ——
   `GetDefaultSurfaceMaterial` 里 `MD_Surface` 缺 include，与 flush 改动无关，**unity 构建一直掩着**。
   ⚠️ 那个头在 `Runtime/Engine/Public/MaterialDomain.h`，**不在** `Materials/` 下。
2. **「剩余不计数裸 flush」清单不完整**：`PCGEditorProcess` 模块还有 5 处
   （`ComputeShaderLandscape.cpp` ×3、`ComputeShaderLandscapeRoad.cpp`、`CSAssetProcess.cpp`；
   另有测试目录 2 处）。驱动方已复现。该模块的 `Build.cs` 已依赖 `ComputeShaderGenerator`，
   `CountedBlockingFlush` 够得着。**未动**。
3. **旧路调色板实际存在 `BP_GroundShaper` 的 CDO 上**，不在关卡实例上 ⇒
   坑表里"空值被烘进 .umap"那条**并不能完整解释**当时的状态。

### D13 藤蔓已落地（2026-08-30）

四面墙矩形 → CPU 规划藤的折线（起点抖动、逐段游走、撞墙角与撞洞**镜像倾角折返**、越往上越细）
→ 一趟 GPU dispatch 打包实例行 → 两个 `UCSGpuInstancedMeshComponent`（演示房子：枝 319 / 叶 216）。
新增 `Public/CSHouseVine.h`、`Private/CSHouseVine.cpp`、`Shaders/Private/CSHouseVine.usf`、
`Tests/CSHouseVineTests.cpp`、`Scripts/TinyGladeMakeIvyMaterials.py`、`Scripts/TinyGladeShotVine.py`。

逐实例随机是 `(墙号, 藤号, 段号, 种子)` 的身份哈希，且 kernel **不用 `InterlockedAdd`**
（记录由 CPU 排定 ⇒ 线程 i 写第 i 行，槽位恒等于身份）。

#### `ivy_branch` 无法线无 UV：在导入期补齐，不在材质侧绕

`CSHouseVine::BuildBaseMesh` 读 LOD0 顶点，长度轴换到 +Z（实测枝 = +Z、叶 = +Y），
**法线用相邻面法线累加** —— `ivy_branch` 是 12 顶点 / 6 三角、**顶点不共享**，
所以累加出来正好是棱柱的逐面平法线，**是正确答案不是近似**；
UV 用**绕长度轴的柱面展开**（平面投影会被非均匀缩放拉成条）。

#### 🆕 又一条"静默换材质"的坑

母材质必须勾 **`bUsedWithInstancedStaticMeshes`**，否则引擎**静默换成默认材质**，
症状与"没绑材质"**逐像素相同**。现成的 `MI_ivy_*` 全挂在 `M_TG_Texture` 下而它**没勾**。

⇒ 对照文档说"缺藤蔓母材质"，实际缺口是**两条独立的**：`M_TG_Texture` 既是 `MSM_UNLIT`，
**也**没勾这个标志 —— **后者才是致命的那条**。已新建 `M_TinyGladeIvyBranch` / `M_TinyGladeIvyLeaf`，
未动 `M_TG_Texture`。

`IsVineDrawable(OutReason)` 因此比岩壳那个**多一环**：把"母材质勾没勾"也纳入执行面。

#### 未做（如实列出）

花（`ivy_flower`）、三季叶、跨檐上屋顶、TG 的 `check_for_wall_jump`、柱子/地形上的藤。
另：**藤避让墙洞会让拱附近明显变稀**（六个拱全开时枝 319 → 158；第一版"撞洞就整根停"更狠，132），
窄墩上基本长不出藤。

### 出图管线：五个脚本统一修对（2026-08-30）

坑 ⑧ + ⑨ **一起**加进全部五个脚本（`always_persist_rendering_state = True` +
导出前**一帧一次**、共 32 次 `capture_scene()` 预热）。五个脚本每张图的精确 `(0,0,0)` 现在都是
**0.000%**，且每个脚本都新增了 `zero > 0.5%` 的门守住这条。

**阈值标定口径**：先跑一遍量出真值，再取**实测值的一半**
（物理含义 = "被测对象在屏幕上的覆盖面至少还剩一半"），**不是**取一个能绿的小数。
六个脚本各做过一次**故意破坏世界侧**（不是改阈值）的实验，六个全部报红 —— 门是活的。

#### 两条顺带纠正

1. 驱动方任务书里说"这五个脚本各自带着有阈值的像素断言"—— **不成立**，只有
   `TinyGladeShotRockShell.py` 有。另外三个只导图零断言、`BrickSeam` 只断言"存在块数跳变"。
   已按各自判读目的补齐同形状的门。
2. **`TinyGladeShotLighting.py` 的 `house3q_csmonly` 自 VSM 裁决落地起就是空操作** ——
   ini 里已经是 `Enable=0`，"临时关 VSM"什么都不改（实测与 `house3q` 只差 0.6%）。
   已改成 `shadowfloor_noshadow`（临时 `r.ShadowQuality 0`，信噪比 9.6% vs 0.0%）。

#### 一条被主动降级的门

裙边噪声的 `pair3q` 改成**只出图、不设门**：故意破坏实验（两组几何完全相同）下
junction 0.2% / overhead 0.0%，而 pair3q 仍有 **15.5%** —— 那是**捕获自身的不对称**
（同一台相机第二次拍时已带着上一轮的 Lumen 历史），基线离信号（25.5~28.7%）太近，
留着只会是一条分辨不出信号的门。

### 披挂岩壳（链 B）已落地（2026-08-30）

图案用 **TG 原件**（`extracted/meshes/rocky_terrain_shell.glb`，49,598 三角 / 609 胞腔），
**原生 ×65、单张、不缩放不平铺**：tile 136.5 m ≥ 地面 128 m，一张正好盖住。
tile 实测**不是周期的**，平铺会露缝，所以"半尺度换 3 m 胞腔"那条路走不通。

#### ⚠️ 烘焙件的通道语义有五条与文档不符（离线核对器实测，驱动方独立复现过）

核对器：`Scripts/VerifyRockShellGlb.py`（无第三方依赖）。8 项计数与文档表**逐项吻合**，但：

1. **`dir_to_centroid` 指向质心**（`dot` 中位 +0.9996、100% 为正），与契约写的
   `normalize(@P − center)` **符号相反** —— 照抄会让 `CellJitter` 正值变成收缩。
2. **`cell_bby` 不是逐胞腔量，它就是 `bIsTopRim`**（顶圈恒 1、底圈恒 0，零例外；
   609 个胞腔里只有 80 个取值唯一）。⇒ 计划的 `CellRelief` 在原件里**没有数据源**，
   实现改为从 `CellId` 自己哈希。
   附带：`is_top`（TEXCOORD_2.y）是**逐三角**标记，33,020 个顶点上与环不符，不能当 `bIsTopRim`。
3. **`LipOffset` 实测 19.5 cm**，不是裁决四引的 21.4。
4. **绕序实测全部朝 +Y**，文档写的 −Y 描述的是翻转**之前**的状态。
5. **相邻胞腔一个顶点都不共享**（盖是内缩孤岛 86.60%、裙精确填缝 13.40%，合计 100.0000%）。
   "共享角不裂开"要重读成"**角点钉死的是缝宽**"。

#### 实现侧的关键决定

- **"沉下去不是隐藏"验得比要求的更严**：不只量下沉量，还断言**画路前后活三角数逐一相等**
  （路不改高度场 ⇒ 坡度 mask 不变 ⇒ 一个都不许少）。实测路下平均 −107 cm、最深 −162，
  路外漂移 0.00，擦掉路逐位复位。
  踩到的坑：下沉沿**地形法线**，沉下去的顶点 XY 也跑了最多 ~128 cm，
  用"顶点当前 XY 上的路权重"分类会误判成"路外却动了 80 cm"，改成按路的几何分类。
- **计划没写的一条因果链**：胞腔尺寸 → 羽化宽度 → 坡度 → 台高。
  5.53 m 胞腔要求 `Falloff≥800`，而 `max|∇h| = Lift×1.5/Falloff`，
  `Lift=300/Falloff=800` 只有 0.5625 ⇒ **整个土台都在 0.75 阈下、一块碎石都不长**。
  演示塑形物台高因此 300 → **700**。
- `IsRockShellDrawable(OutReason)` 逐环检查渲染侧（组件存在/已注册/可见/网格已绑/流已分配/
  包围盒有效/**且解析得到非空材质**）+ 像素判据（只切 `bRockShell` 拍两张，54% 采样像素变了）。
  这是石阶那个坑的正面执行面。

#### ❌ 曾被记为"岩壳板缝纯黑空洞"——**这条判断是错的**（2026-08-30 已查清）

驱动方原先写的是「14.6% 精确 (0,0,0) 全部落在岩壳板缝里 ⇒ 精确零不是光照问题 ⇒ 是几何/材质」。
**前半段的症状描述对，由此推出的结论错。** 实测：

- 用 `SCS_BASE_COLOR` / `SCS_NORMAL` 直接读 G-buffer（不过光照）：缝里的 base color 是
  **草绿 `(22,34,12)`**，且与"整条岩壳关掉"的对照图**逐像素 100% 相同** ⇒ **缝里画的是地面**。
- 把 `bRockShell` 整条关掉，同机位精确零从 17.60% **涨到 41.56%** ⇒
  **岩壳是遮住黑的那一方**，缝只是地面还露着的那 13.4%。
- 裙三角活得好好的（readback 1112 个活三角），**几何上没有洞**。

真因见下一节。⚠️ 这是本循环第七条被实测推翻的推断，而且**是驱动方自己下的**。

#### 🐛 真因：出图管线连踩两个坑，方向相反

坑 ⑧（D14 那轮修的）`always_persist_rendering_state=True` 让离屏 capture **第一次真的有了
ViewState**，于是坑 ⑤ 翻回来的 Lumen 也**第一次真的跑了起来**；
而 **Lumen 的最终聚集靠帧间历史**，`capture_scene()` 只抓一帧 ⇒ **间接光恒为零** ⇒
整张图里凡是没被太阳直射的 lit 表面全部落在精确 `(0,0,0)`。

三组对照坐实：同机位单帧 **17.23%** / 连拍 198 帧 **0.00%** / 有 ViewState 但 GI 不翻 Lumen **0.00%**。
驱动方当时给的"最强线索"（更早那张图缝里是绿的）也正好在这里对上 ——
`TinyGladeShotRockShell.py` **没有**设那一行，Lumen 压根没跑。

**修法**：每个机位导出前先 `capture_scene()` 预热 **32** 次（标定过：1 次 19.25% → 8 次 0.007% →
**16 次起 0.000%**，亮度均值 32 次时已在 198 帧收敛值的 1% 以内）。
**没改任何 C++/shader，没填缝** —— 缝里现在透出的就是草地。

判据已做成能报红的门：`Scripts/TinyGladeShotSofteningStats.py` 的 `zero%` 列 + `ZERO_FAIL = 0.5%`，
逐机位打 `[PASS]/[FAIL]` 并以退出码报红。
同机位前后：backlit **14.598% → 0.000%**、shadow 6.581% → 0.002%、tread 0.479% → 0.002%、
wide 5.994% → 0.000%；backlit 最暗 20% 平均饱和度 0.181 → **0.874**。

#### ⚠️ 连带作废：D14 那张表的「精确 (0,0,0) 占比」一列不再可比

那一列是**透过坏掉的捕获**量出来的。改进本身是真的（`dark_chroma`、分位数都在动），
但那一列**同时在量捕获的 bug**；管线修好后两侧都归零。历史值留档但不要拿来对比。

#### ✅ 已拍板（2026-08-30 裁决六）：岩壳材质改 lit

⚠️ 本小节标题原为「⛳ 新增待拍板：岩壳材质是 unlit」，用户已裁决 —— **改 lit，且升格为全项目纪律**
（任何交付路径上的材质都不许是 `MSM_Unlit`，见「第二批裁决」裁决六）。下面是原始记录。

`Scripts/TinyGladeSetupRockShell.py` 把 `RockShellMaterial` 接到 `MI_rocky_terrain`，
而它的母材质 `M_TG_Texture` 是 **`MSM_Unlit`**（gallery 用的看图材质）。
⇒ **岩壳完全不受光、不响应阴影**。这既是"壳自己不黑"的原因，也是一条文档没记的内容缺陷。
Lumen 收敛之后，平板与真正受光的草地并排会显得**贴片感明显**。
~~换成 lit 材质需要拍板，未擅自动。~~ ✅ **已拍板并已执行（2026-08-30）**：直接把 `M_TG_Texture`
本体翻成 `MSM_DefaultLit`（贴图搬 BaseColor + Emissive 钉 0 + Roughness 0.85 + 新增 `NormalTexture`
参数）。**不另建母材质**，岩壳照旧用 `MI_rocky_terrain`。当前阶段只要求法线 + 颜色贴图正确。

#### ⚠️ 另外五个出图脚本仍踩着坑 ⑧

`TinyGladeShotRockShell / SkirtNoise / RoadMaterial / Lighting / BrickSeam` 都**没有**
`always_persist_rendering_state` ⇒ 它们的曝光一条都不生效。
**谁按坑 ⑧ 补上那一行，就会立刻继承坑 ⑨（Lumen 单帧恒零）**，必须同时加预热。
这五个脚本各自带着有阈值的像素断言，未擅自改。

### D14 观感：VSM 关掉之后的一轮（2026-08-30）

#### 用户裁决：VSM 关掉

`Config/DefaultEngine.ini:52` = 0。代价：阴影改由 CSM 出，远处分辨率不如 VSM、没有 Nanite
优化路径；本工程网格本就不走 Nanite、场景不大，实际损失小。换来两条 gpumesh 路的阴影从零到全有。

#### 三条"柔和来源"的 UE 对位 —— 驱动方给的两个候选都被推翻

1. **AO 底 `mix(0.6,1.0,ao)`**：`r.AmbientOcclusion.Intensity` 在 5.7 **全树 0 命中**；
   `AmbientOcclusionIntensity` 的语义是 "affects the **non direct** lighting"（`Scene.h:2118`），
   而 TG 夹的是**直接光**；且走 Lumen 屏幕探针时 `DiffuseIndirectComposite.usf:381-386`
   被 `#if` 整个跳过 ⇒ **主路径上是空操作**。
   真正同义的是 **`LumenSkylightLeaking`**（引擎原话 `Scene.h:1752`:
   "keep indoor areas from going fully black"）：0.12 → 0.20 + `LeakingDistance` 1000 → 50。
2. **彩色 L1**：UE 没有"逐 RGB 通道各一条方向"。替代是天光**下半球纯色 = 地面草色**。
   ⚠️ 顺带查出**背光纯黑的最后一半根因**：`LowerHemisphereColor` 原本是 `(0,0,0)` ⇒
   所有朝下/侧下方向的环境光**精确为零**。指纹：改前最暗 20% 像素平均饱和度 **0.000**。
3. **Tony McMapface：UE 5.7 没有对位物，没硬凑**（`AgX`/`Tony`/`McMapface` 零命中；
   `r.TonemapperOutputDevice`/`r.TonemapperFilm` 不存在）。只做了标注为"部分对位"的近似。
   要真上只能烘 LUT 资产。

#### 石阶埋深：闭式解，不是调参

`StairEmbed = StairBlockSize.X / 2`（10 → 30）。摆放规则是"块**心**落等值点、块底钉该层高度"，
下坡底棱悬空 `f = (X/2 − e)·坡度`；取 `e = X/2` 时 **`f ≡ 0` 与坡度无关**，等值线正好成为踏步鼻。
之前两个值都是**坡度相关**的，所以剖面一改就重新出错：规格的 25 在旧坡度 1.125 下把踏步全埋掉，
S1 改的 10 在新坡度 1.342 下给出 **f = +26.8 cm** —— 每块前缘吊在半空，这就是"像石墙"的由来。
实测 `float_under_toe +26.83 → +0.00`。

**一条埋深修不了、必须记住的**：踏步高/踏面 = **坡度本身**（1.342），`StepHeight` 只能同时缩放两者。
想要"能走"的比例只能改地形剖面或让路径斜切上坡 —— 那是塑形物/路径的事。

#### 量化改善（同机位、同总亮度）

| 判据 | backlit | tread | wide |
| --- | --- | --- | --- |
| 精确 (0,0,0) 占比 | 29.28% → **14.60%** | 3.88% → **0.48%** | 10.87% → **5.99%** |
| 最暗 20% 平均饱和度 | 0.000 → **0.181** | — | 0.307 → **0.521** |

#### 只报告、未做的一条

**`r.Shadow.FilterMethod=1` 才是 TG 三级联 PCSS 的精确对位物，一行 ini。**
VSM 关掉后默认是 0 = Uniform PCF，`LightSourceAngle=1.75` 今天**买不到任何东西**
（只被 PCSS 分支读）。但这是与 VSM 同一档的全工程渲染决策，且引擎自标 experimental，未擅自开。

### 地形隆起链 A 补齐：裙边噪声 + 二次抬升（2026-08-30）

#### 噪声选型：为什么不能用内置 `noise()`

CPU 镜像与 GPU 必须**逐位一致**，而 HLSL 内置 `noise()` 没有 CPU 对位物，
`sin/cos/pow/exp` 两边实现也不同。改用自写的**整数哈希 value noise + 3 倍频 fbm**
（gain 0.55 / lacunarity 1.9 / 每层旋转 36.87°，常数照 TG 实测），全程只用
`uint32` 环绕乘法·异或·移位 + float 加减乘 + `sqrt` —— 这几样在 C++ 与 HLSL 里是同一套语义。
二次抬升的 `pow(S, 1.5)` 因此写成 **`S·sqrt(S)`**。

CPU 孪生 `Public/CSGroundShaperField.h` 与 GPU 权威 `Shaders/Private/CSGroundShaperField.ush`
逐行对照，且**喂进去的是同一份打包参数**（`SampleShapeHeight` 现在也先打包再求值）。

**断言里最值钱的一处细节**：`GroundShaper.CpuGpuFieldParity` 先用新增的 `GetGpuDisplaceCount()`
确认这一趟**真的走了 GPU 位移 pass** —— 否则 `RefreshHeightsInRegion` 会退回镜像上传，
断言就变成"CPU 和 CPU 比"、**静默通过**。4225 个顶点实测最大差 **1.07e-4 cm**（约 1 ulp）。

#### 折痕断言：噪声必须加在 `max` 之前

`GroundShaper.CreaseIsBroken` 沿折痕取 51 个采样，量两件事：
- **折痕线的横向游走**：噪声关时**恒等于 0**（折痕是精确的中垂线）；开时 σ = **12.11 cm**、峰值 36.40 cm。
- **横向二阶差分的变异系数**：**0.087 → 0.424**（均值 41.99 → 26.06 cm，51 个采样里 14 个掉到一半以下）。

#### ⚠️ 裁决六的前提自带一条边界（原型公式的性质，不是实现取舍）

权重 `(1 − dist)` 按剖面高度把噪声关小 ⇒ **两座重叠越深、折痕处噪声权重越低**。
半径 300 / 羽化 400 相距 **800**（就是石阶 S1 用例那个场景）时接合处 `S≈0.84`，
噪声只剩 16% 权重，实测折痕**纹丝不动**（游走 σ 仅 4 cm）；相距 1200 时 `S≈0.16` 才有上面那组数。

**结论：裙边相接没问题，盘几乎压在一起的两座仍会留干净折痕。** 披挂岩壳排期时按这个权重算 ——
指望裙边噪声藏住所有折痕是不成立的。

#### 噪声域改成本座局部坐标（与原型不同，有依据）

原型用世界 P。改的理由：同一份世界噪声在**对称的两座之间会原样抵消**
（两边 `S` 与 `turb` 都相等），折痕纹丝不动 —— 实测过。局部域还让图案钉在塑形物上，
拖动时不"沸腾"。

#### ✅ 已拍板（2026-08-30 裁决七）：`LiftHeight` 选甲 —— 照原型留 2% 溢出

二次抬升按原型口径**照抬台顶**、没有归一化 ⇒ 台顶 = `LiftHeight × 1.021`
（默认 300 → **306.3**），`LiftHeight` **不再等于台顶高度**。
演示回归里 5 条拿绝对台高当期望值的断言已改成"和 `SampleHeight` 比"或写成表达式（更好的不变量）。

- ✅ **甲（现状，照原型）—— 用户已选**：保留 2% 溢出，`LiftHeight` 是"抬升幅度"不是"台顶高"。
- ~~乙（驱动方倾向）：归一化，让 `LiftHeight` 保住"就是台顶高"的语义。~~ **已否决。**

**落地含义：代码一行都不用改**，现状即是甲。要做的只有把口径钉死、别让它再被当成 bug 修回去：

- `LiftHeight` 的**契约是"抬升幅度"**，台顶 = `LiftHeight × (1 + SecondaryLiftScale)`（默认 300 → 306.3）。
  这条已经写在 [`Ground_Shaper.md`](Ground_Shaper.md) :135-137，属性注释里也要有同一句。
- **任何断言都不许拿绝对台高当期望值** —— 演示回归那 5 条已经改成"和 `SampleHeight` 比"或写成表达式，
  那才是更好的不变量，别回退成写死的数。
- 想让 `LiftHeight` 严格等于台顶高，把 `SecondaryLiftScale` 设成 0（这是配置，不是改语义）。
- ⚠️ **别再把它当 bug 报**：驱动方原先倾向乙的理由是"用户可见属性的 2% 静默漂移是脚枪"，
  用户已知情并选择照原型。下次看到 300 → 306.3 不是回归。

#### 旧路的退化（已处理，未删）

`HasAnalyticProfile()`（噪声或二次抬升开着即为假）时退回既有的"验证 + 割线 + 二分"逐方向数值求交。
实测噪声关 42 级 / 噪声开 40 级 —— 差的 2 级是噪声让高度沿半径**不再单调**、个别方向括号取不到跨越，
那一格诚实判为"不属于本座"。

**文档那句「裙边噪声是一次架构选择」确认过期**：`AnalyticRingRadius` 当时只剩旧路在用（S3 已把它随旧路一起删掉，全仓 0 命中），
代价已被 S1 的 marching squares 付掉。

#### 默认值依据

`SkirtNoiseAmount = 0.5`（以台高为单位）是折痕验收能量到的**最小档** ——
0.35 只到游走 σ≈10 cm、变异系数 0.34，肉眼仍读得出直线。
`SkirtNoiseWavelength = 300` 与地面 `CellSize`(50) 的比是 6，再细地面网格采不住
（石阶读解析场会先于地面显出细节，看着像"石阶和地面对不上"）。

### 石阶 S1 已落地（2026-08-30，驱动方独立复核过）

整条逻辑的图见 [`Ground_Stairs_Logic.svg`](Ground_Stairs_Logic.svg)（管线 / 单格八步 / 一格的几何 / 鞍点消歧 / 弦长驱动的长度 / 随机源）。

**只加不删**：`CSGroundStairs.usf` / `CSGroundStairs.{h,cpp}` / `Tests/CSGroundStairsTests.cpp` 全新，
旧的 `CSShaperSteps` + `BuildStepPlan` + `RDG_SmoothSpline` 原样保留（S1 落地时如此；S3 随后删掉了旧路，`RDG_SmoothSpline` 因另有消费者留着）。

**唯一动到的既有文件是 `CSMeshOps.usf`，而且动法是对的**：高度场抽进新共享头
`CSGroundShaperField.ush`，`GroundShaperHeightAt(uint,uint)` 退化成一行包装、数学逐字未变。
石阶扫描要在**任意世界 XY** 求同一个场，两边各抄一份迟早分叉 —— 分叉的症状是"石阶浮在坡面
上方几厘米"这种不报错、只在裙边中段可见的错位。

**核心验收项（接合处不断裂）的验法**：两座相距 800 的土台裙边相融（接合处高 253 cm，
既非 0 也非 300），路**沿接合线**画、垂直于土台轴 —— 这样路穿过的每条等值线都属于融合体、
不属于任何一座的星形环。断言：石阶落在接合线上、腰部等值线在轴两侧**都**穿路（38/38 对称）、
层集**连续无缺口**（9/9）。缺一层正是旧路在那里的失效模式。

**驱动方独立复核**：构建 Succeeded、单测 **46/46**（零 Fail，当时的基线）、回归 **REGRESS OK**。

#### 四条与规格草稿的偏离（agent 自报，未擅自改裁决）

1. **Embed 符号**：草稿写 `In = -normalize(GroundGradient(Mid))`（下坡），但 `StepEmbed` 的文档是
   "沿半径向内推进，让踏面扎进坡里" —— 向内是**上坡**。已按上坡实现（负值走下坡），带注释。
2. **块的 +Z 用世界上方**，不用地形法线 —— 对齐 TG「旋转基 = 等值线方向 × Y-up」与旧路的
   `StepPlaneNormal`。踏面跟着坡倾会读成披挂岩壳而不是台阶。地形法线仍算（中心差分，因为
   max 合成的场恰好在我们要修的接缝处不可导），但只用来找下坡方向。
3. **鞍点发两段**而不是草稿的单个 `SolveCrossing` 结果 —— 只发一段会静默丢一级台阶。
4. ⚠️ **规格里的默认值按原样不可用**：可见踏步高 ≈ `BlockSize.Z − Embed × 坡度`，而默认剖面
   坡度峰值 1.125，继承来的 `StepEmbed = 25` 把 30 cm 踏步整个埋掉（实测裙边中段只露 3 cm）。
   已改成 Embed 10 / BlockSize (60, 100, 45)，关系写进属性注释。

#### S2（逐实例抖动）已落地（2026-08-30）

##### 🐛 顺带订正 S1 的一个错误：随机种子取了槽位

S1 写 packed 行 `.w`（顶点工厂读的 `PerInstanceRandom`）时用的是 **`InterlockedAdd` 的槽位**。
槽位由线程组完成顺序决定、**每 dab 重扫一次就重掷** ⇒ 材质拿它做颜色/磨损变化的话，
**画一笔路全场变色**。而且这一条**不会有任何断言报红** —— 它只在接了材质变化之后才显形。
S2 一并订正成格身份。

##### 随机源：格身份哈希，不是位置也不是槽位

种子 = 格坐标 + 层号 + 段号 + 用户种子。TG 用的是 `uint((Tx+Ty+Tz)*100)`
（`_rocky_terrain_stairs_stairs.cs:504` 与对应 VS `:105`）—— 实例**平移量之和量化到 cm**，
同样与槽位无关，但那是**位置派生**。改用格身份的理由：拖塑形物时高度场在动、等值线跟着滑，
位置派生的种子会让石阶在**整个拖动过程**不停重掷；格身份只在"这一格这一层还长不长石阶"
真的变化时才变。顺带好处：**格身份在 CPU 侧可精确复算，所以"抖动只由格身份决定"是可断言的**
（新测 `GroundStairs.JitterIsCellDeterministic`，实测 `stairs=140 matched=140`）。

量值：长度轴胀大 ×1.06 + **单侧** 0–10%，进深/高 ±12%，偏航 ±6°（只抖 yaw —— 踏面必须水平）。

##### 负缝：门框砖那套常数胀大在石阶上**行不通**

门框的槽距由 `SolveBlockLayout` 定成近恒定；**石阶的槽距是几何决定的**，弦长在
`[格距, √2×格距]` 上变。新对照断言实测：S1 的定长块在 **45° 等值线上露 41.42 cm 正缝**。

TG 的答案是把长度轴直接写成 **`distance(A, B)`**（同文件 `:509`）—— 相邻两块因此
**精确共用穿越点**、与拐弯角度无关。已照抄并叠一个常数胀大。

**抖动确实会抵消负缝，两条**：① 长度轴对称抖会让相邻两块同时缩（所以做成**单侧只增**）；
② 偏航把投影缩到 `cos δ`。充要条件是 **`Bloat·cos(yaw) > 1`**，实测 1.0542，
每个接头仍互穿 2.71 cm。**两条都写成了断言**（`GroundStairs.StepOverlap`）。

零阻塞未退化：抖动全在原有那一趟 dispatch 里，保守包围盒的最坏缩放**只由配置算**
（不读一个实例），所以 `bNeedHandover` 不会因抖动成立。回归新增
`a 12-dab stroke with GPU stairs and S2 jitter on blocks the game thread zero times flushes=0`。

##### S2 规格里没做的两条（只报告）

1. **VS 侧倒角噪声**（照 TG 的 `bevel_offset`）：TG 读的是**逐顶点** `bevel_offset` 顶点流，
   本项目的网格与 VF 都没有这条流，加它要动共享顶点工厂或改走材质 WPO，超出「逐实例抖动」。
2. TG 的石阶 CS 还有 **15% 概率额外发一颗 pebble**（均匀缩放 `mix(0.2,0.4)`、落在弦上随机点，
   `:511-547`），未移植。

#### 顺带测出来的一件事：旧路才是 flush 的来源

新的 flush 门一开始报「12 dab 一笔 = 6 次阻塞 flush」，追下去是**旧路**：它的容量按
"这次排了多少条记录"反推，所以画路时一 dab 长一次、每次都重跑阻塞的 `SetInstanceSourceGPU`。
S1 的定容整轮回归只付**一次**交接、笔画期间零阻塞。门现在关掉旧路以测 S1，理由写在注释里。

### 墙-顶三处收边已封（2026-08-30）

#### ⚠️ 先订正一条：檐口**不是**拓扑洞

**本文卷一 · 拉尺寸与接缝对照** §4.3（以及本文此前转述的「从室内看得见天」）**说法不准**。
实测：`CSHouseRoof_EvalZAcross(HalfSpan) ≡ EaveZ` **按构造**成立，墙顶面与屋面底沿墙外棱
**相切** —— 任何直线射线都跨不过那条切线，**射线判据在改之前也全绿**。

真正的破绽是这条封口**宽度为零**：屋面底那张大四边形从墙顶外棱内部横切过去（T 型接缝），
缝里没有任何实体，封口靠两张面在一条线上恰好相等，而顶点是 **float32 世界坐标**。
是**精度**破绽不是拓扑洞。修法不变，但**断言形态必须换**。

#### 断言：落在体积上，不落在射线上

新测 `PCGPlugins.ComputeShaderGenerator.House.EaveSealed` 把房体三角汤当成**凸块的并集**，
用竖直射线上 `Σ sign(N·up) = 包住该点的块数` 的判据（对凸块精确，背靠背重复面自动抵消）：

- ⓐ 在按 footprint **反推**的周界带上（**不照抄生成器的四段分法** —— 否则生成器漏哪条边、
  断言就跟着漏），要求「墙顶 → 屋面底之间深度 ≥ 1」；
- ⓑ 屋面底之上半个咬入量处要求深度 ≥ 2（证明是**互穿**不是相切）；
- ⓒ 沿脊探针要求深度恒 = 1（收口不互穿）；
- ⓓ 屋面覆盖范围内任何顶点不得戳出屋面板上表面。

七组参数覆盖两个脊向 / 250–1000 footprint / 15°–70° 坡度 / 0–90 外挑 / 2–20 板厚 / 12–40 墙厚。
**已验证会报红**：换回改前几何后，演示尺寸报 **120 个开口样本、最深 14.45 cm**
（= 24·tan35°·0.86），陡坡那组 59.58 cm。

为让断言拿得到三角汤，`RebuildBodyMesh` 的几何段被**原样抽出**成
`CSHouse_BuildBodySoup(FCSHouseBodyDesc&, FCSGpuMeshCPUData&)`（纯搬运，让不起 world 的
纯 CPU 用例也能跑）。

#### 余量只有一条方程

`CSHouseRoof_SoffitTopZ = 屋面底 + 咬入量`，檐口封口楔形与山墙顶轮廓都取它 —— 符合 D4
「屋顶高度只能从求值器取」的纪律。
咬入量 = **竖直板厚（`Thickness/cos(pitch)`）的 1/4**，不是硬编 cm：唯一硬上界是"别从屋面板
上表面钻出去"，那个上界就是竖直板厚，按比例取则 `RoofThickness`/`RoofPitch` 怎么调都不越界
（板厚下限 2 cm ⇒ 最坏咬入 0.5 cm，仍比 float32 世界坐标在 1 km 处的 ulp ≈0.008 cm 大两个数量级），
留 3/4 给将来铺瓦铺梁再穿插。咬入量在 footprint 边界**归零、向内一个墙厚爬满** ——
`RoofOverhang = 0` 时屋面板檐口端面正好落在墙外表面所在平面上，那里给正咬入会造出一对
共面重叠的可见面。

**屋脊不用余量，用零余量精确对切**：坡板改成沿坡向 + **竖直**挤出（截面成平行四边形、脊边竖直，
垂直坡面的板厚仍是 `RoofThickness`），挤出量直接用 `Slope` 本身 ⇒ 两块板的跨度分量是
`EaveOut + (−EaveOut)`，**逐位精确**的 0。给脊任何过冲都会让尖端重新戳出对方顶面。

#### 🐛 顺带揪出一个既有 bug（新断言先报的红）

`RidgeToLocal` 在脊沿 Y 时交换 X/Y，**那是一次镜像** —— 脊向坐标系里右手的一组基映射到局部
就成了左手。旧写法 `NRoof = cross(UDir, SlopeDir)` 在镜像下指向**下方**，于是
**脊沿 Y 的房子屋面板整块挂在屋面底面以下**（顶面恰好落在墙顶那条线上，从外面看几乎没区别，
实际是板扎进阁楼、山墙斜边与屋面**顶**面共面）。演示房子一直是脊沿 X，所以从没人撞上。
已按映射手性修正（`Handed` 由实际基的叉积取符号，不是按脊向硬编）。

#### 未擅自处理的三条

1. 楼梯文档 A6 建议「沿脊铺一条 brick 实例带做脊瓦」，与本轮「不引入预制件、要收口」的约束冲突 ——
   按任务书做了纯几何收口，**没碰 A6 那条建议**。
2. A6 还建议给 `ECSHousePart` 加 `Eave`，但 5 号**已预留给 D7 角柱**；本轮不许预判 C-D7-1/2，
   封口件走 `Wall`（它就是被斜切的墙顶），**没占号**。
3. `CSHouseRoof_EvalZAcross` 的注释自称"屋面**外**表面"，但生成器一直拿它当**板底**用
   （新代码沿用同一口径）。措辞该订正，未动。

### 最值得直接抄的一条：TG 的砖是**故意胀大、互相穿插**的

⚠️ **数值以驱动方复读 GLSL 的结果为准** —— subagent 首报的「90% ×1.045、10% ×0.95、
竖直 ×0.95」**是错的**，实际着色器里没有任何 0.95。

`_wall_wall_brick_lod0...vs_main.glsl:160-175`，逐实例缩放乘在单位立方体局部坐标上：

- `flags & 4` **未置位 ⇒ 缩放恒为 (1,1,1)**，根本不胀 —— 胀大是**有门控的**，不是所有砖。
- `flags & 4` 置位时分两支：
  - `flags & 1` 置位 ⇒ **三轴一律 ×1.045**（均匀胀 4.5%）。
  - `flags & 1` 未置位 ⇒ **水平两轴 ×1.1、竖直 ×1.0**，且有 **10% 概率整块不胀**（×1.0）。
    竖直不胀是对的：砖是一层层码上去的，竖直方向也胀会让层高漂移、层间可见重叠。

**这才是"逐帧重排砖却完全看不出跳变"的物理原因 —— 砖缝本来就是负的。**
砖数一变，穿插量微调而已；正缝则会在块数跳变的那一帧露出一条亮缝。

本项目 `SolveBlockLayout` 的语义与 TG 同型（变数量、近定距），**此前缺这一步**：
门框砖原本用 `FrameBrickGap = 1.5f` —— **正缝**。

#### ✅ 已落地（2026-08-30）

⚠️ 订正本节原先写的「动 `SolveBlockLayout` / `FrameBrickGap` 的约 10 行」——**负缝不属于排布层**。
把胀大系数折进 `PaletteLengths` 或折成负 gap 会连**砖数和位置**一起变，那就不是 TG 的"胀大"了；
TG 是把逐实例缩放放在 **VS 里**、排布一无所知。`SolveBlockLayout` 的 `Max(InGap, 0)` 保留，
并在 `CSSplineBlockActor.cpp` 加注释说明负缝为什么**不**在那里做。

映射（本项目没有 TG 那套 flags，简化成一条规则）：**只胀沿曲线的长度轴**。
新增 `ACSHouseActor::FrameBrickBloat`（默认 **1.10**，对应 TG `flags&4` 置位、`flags&1` 未置位
那一支的 ×1.1）乘在 `EnsureFrameComponent` 的 `BlockSize.Y` 上；`BuildFramePlan` 的排布仍按
**未胀大**的 `FrameBrickLength` 定位。`FrameBrickGap` 默认 1.5 → **0**，净缝 = 26×(1−1.1) = **−2.6 cm**。
+Z 钉死在墙厚（封 clip 断口）、+X 是拱缘可见带宽 —— 两者都没有邻居，胀了只会漂移一个被刻意
标定的尺寸，正是 TG 让竖直轴 ×1.0 的同一条理由，故都留 1.0。
TG 那 **10%「整块不胀」没抄**（一条十几块砖的拱缘上会随机露两条可见缝），理由写在字段注释里。

**「不露缝」的证明在数值断言上，不在图上**（当时的对比截图中，
上下两排的差别肉眼**读不出**"断开 vs 连续"）。新测
`PCGPlugins.ComputeShaderGenerator.House.FrameBrickOverlap` 直接读 **CDO 出厂值**，
弧长按 0.1 cm 步长从 200 扫到 1200（10001 样本、穿过 **38 次块数跳变**），
断言每次砖缝都是负的 —— 最差 −2.45、最好 −2.76 cm，即穿插量在所有跳变上只摆动
0.31 cm（砖长的 1.2%）。同一扫描用改动前参数跑对照组，断言其砖缝**恒为正**（+1.40…+1.60），
谁把默认值改回去测试立刻红。

副作用（未动断言）：演示房子砖数 114 → 120（槽距 27.5 → 26），回归的 `60 <= bricks <= 200` 仍成立。

### D14 阴影：假设被实测推翻，真凶是 VSM（2026-08-30）

⚠️ **本节此前写的推断（「`bDoOverrideArgs` 不成立 ⇒ 开 `CastShadow` 就有影子」）已被对照实验否定。**
**本文卷四 · 渲染与光照对照** §7.4 表第二行与 §8.1 的 S-a 同样作废。

两处组件构造开 `CastShadow = true` 后同机位出图：

| 路径 | CSM（`r.Shadow.Virtual.Enable 0`） | VSM（项目级 =1，即当前默认） |
| --- | --- | --- |
| 实例化 `UCSGpuInstancedMeshComponent`（门框砖 114 块） | **有影子** | **无** |
| 非实例化 `UCSMeshRenderComponent`（房体/地面/柱） | **有影子**，自阴影与屋檐投墙都对 | **无** |

对照组：同图里一个普通引擎 Cube 影子正常 ⇒ 捕获路与灯都没问题。
VSM 的三个子开关 `r.Shadow.Virtual.{Cache,NonNanite.Batch,NonNanite.UseHZB}` 逐一排除
（三张对照图 md5 完全相同）。

**机制（源码阅读，标推测）**：VSM 的非 Nanite 光栅整条走 GPU-Scene 实例剔除
（`VirtualShadowMapArray.cpp` 的 `AddRasterPass` → `FParallelMeshDrawCommandPass::Draw`
带 `InstanceCullingDrawParams`；`InstanceCullingContext.cpp:1509/1590` 只为带
`HasPrimitiveIdStreamIndex` 的命令追加实例），而两条 gpumesh 路都是 dynamic relevance +
`FDynamicPrimitiveUniformBuffer`，**GPU-Scene 里没有实例可剔**。

处置：两处保留 `CastShadow = true`（CSM 下确凿正确，VSM 下逐像素比过、一个影子像素都画不出来），
并在 `CSGpuInstancedMeshVertexFactory::InitRHI` 加了
`checkf(GetPrimitiveIdStreamIndex(...) == INDEX_NONE)` —— 这个索引一旦变成 ≥0，
**手取实例与阴影会同时无声失效**。

#### ⛳ 需要你拍板：要不要关掉 VSM

`Config/DefaultEngine.ini:52` 的 `r.Shadow.Virtual.Enable=1` 改成 `0` 就有影子了，**一行**。
但那是**全工程的渲染决策**，agent 没有擅自改。当时的同机位验证中，VSM 无影子，CSM 影子完整；后续屋瓦的 Nanite / VSM 修复见 [屋顶文档](Roof_TerraceAndRidge.md#瓦片-vsm)。

### D14 后处理：已落地，但天花板卡在阴影上

新脚本 `Scripts/TinyGladeSetupLighting.py`（自诊断，日志断言 `LIGHTING OK`）给两张关卡各加
`TG_PostProcess`（Unbound）+ `TG_HeightFog`，太阳 `LightSourceAngle` 0.5357° → 1.75°。
每个参数都注了对位 TG 哪一条：
② 抬 AO 底 → `AmbientOcclusionIntensity=0.4`（下限钉 0.6）+ `Power=1.0` + `LumenSkylightLeaking=0.12`；
③ 间接光染色 → `IndirectLightingColor=(1.0,0.97,0.86)` + `Intensity=1.2` + `LumenDiffuseColorBoost=1.15`；
④ `ExpandGamut=0`。没堆 bloom/色差/暗角，反而把出厂那圈暗角关成 0。
改前/改后 73.6% 像素有可见变化。**但②「抬 AO 底」在一张一个影子都没有的图上几乎无从下手** —— B 的天花板卡在 A。

#### 地面的亮白网格线：真因是**选错了贴图**（2026-08-30 已修）

⚠️ 驱动方原先的猜测「寻址模式被导成 Clamp」**已被两条独立证据证伪**：引擎内实测
`addr=(TA_WRAP, TA_WRAP)`；且 Clamp 在数学上根本产生不了重复网格（真 Clamp 会把 5 m 以外
整片拉成纯白）。同一条猜测里的「或贴图边缘有亮边像素」才是对的那半句。
另一个候选「地面块之间的缝」也排除了：两张关卡各**只有一个** ground actor，(0,0,0)、64×64×50 cm。

**真因**：`grass_patch_summer` **不是地面贴图**，是 TG 草叶**卡片**的贴图（`_generate_grass`
拿它做 billboard）—— RGB 只是两段竖直渐变、形状全在 alpha 里，而它 **alpha=0 的空白边框
RGB 是纯白 255**（上 12 texel、左 6 + 右 4 texel）。地面材质只接 RGB 不看 alpha，
于是每个平铺边界糊出一条白线。

**铁证是线宽不等**：贴图两轴白边 12 / 10 texel，出图实测 13.45 / 11.53 texel
（各 +1.5 texel 的双线性扩散）—— **两轴不等宽这一点只可能来自那张贴图**。
线距实测 500.1 cm，正是 `UVWorldPeriod`。

改用 `dirtpath_grass`（512² 不透明、无白边，本来就是无缝图，且正是 `dirtpath_1` 在 TG 里的搭档）。
同机位复拍：俯视图亮线检出从 6 竖 + 4 横降到 **0**，地面高通对比度 std 22.97 → 1.92。

**顺带**：换图后草地才第一次显出真草叶纹理（之前是两段渐变），观感提升比"消掉白线"还大。

### 顺带修好的一个坏脚本

`Scripts/TinyGladeMakeWallMaterials.py` **今天是坏的**：它引用了 `RevealMaterial`，
而这个属性随"洞口内壁"作废时已从 C++ 删掉，**UE Python 对不存在的属性是抛异常不是静默忽略**
⇒ 脚本跑到那两行就挂，CDO 与关卡实例的材质一个都赋不上。已清理。
⚠️ 但材质资产 `M_TinyGladeReveal` **不能删** —— 它现在是 `PillarMaterial`，关卡还引用着。
**（2026-09-11 更新：柱子早已由 `TinyGladeSetupPillar.py` 改挂 `M_TinyGladeBrick`，它已无任何引用，
已删除；`TinyGladeMakeWallMaterials.py` 同步不再建它、也不再写 `PillarMaterial`。）**

**机制已核实，结论待实测。** 引擎侧真正会走岔的是
`MeshPassProcessor.cpp:1304` 的 `bDoOverrideArgs`，它要求 `PrimitiveIdStreamIndex >= 0`；
而本项目的实例化 VF **明确声明自己不带** `SupportsPrimitiveIdStream`
（`CSGpuInstancedMeshVertexFactory.h:19` 与 `.cpp:27,102` 三处注释都写了这是有意为之）。
既然那条 override 路径根本不成立，**石阶/点刷这条路开 `CastShadow = true` 有可能直接就有影子**。
1 行改动，值得先验 —— 但这是**推断，不是实测**，别当结论用。

同一轮还查到：两张演示关卡**零 `PostProcessVolume`、零 `ExponentialHeightFog`**，跑的全是
UE 出厂默认。TG 的"柔和"有三个来源，前两条是反编译 GLSL 实证：① 三级联 PCSS；
② **AO 被硬夹在 `mix(0.6, 1.0, ao)` —— 阴影区永远压不黑**；③ 彩色 L1 间接光被
`indirect_light_tint` 上色、饱和度 ×1.17、增益 ×1.2。补一个调好参的 PPV 是性价比最高的一步。

中期主线推荐**隐藏的低模真洞光照代理**（`SetVisibility(false)` + `bCastHiddenShadow` +
DF/RT/Lumen 三个 flag）：一次投入同时解决阴影、距离场、光追、Lumen 卡片，**一行 gpumesh 代码
都不用改**，而且正是 TG 自己的做法（prefab 的 `rt` 段粗代理）。它也正好是 C-R1 的解药。

⚠️ 另一条待实测的修正：「gpumesh 进不了 Lumen」这句只对了一半 —— Lumen 的 screen traces
默认开（`LumenScreenProbeTracing.cpp:14-20`）且只读深度 + HZB + 上一帧 SceneColor，**不看
DF/RT 表示**，所以房子在屏幕内时已经在给 Lumen 贡献遮挡与弹射；真正缺的是离屏与远场
（症状是随镜头突变）。

### 转角角石已落地（2026-08-31）—— 合卷卷一 A7 / 卷五 A11

D7 的两半至此都合上：**形状相交**那半（接缝砖）2026-08-31 上午落地，**墙自身转角**这半本轮落地。

新增 `Public/CSHouseQuoin.h`（纯 header，与 `CSHouseSeam.h` 同形态），`ACSHouseActor` 侧新增
`bQuoinEnabled` / `QuoinInset` 两个属性、`GetQuoinBrickCount()` / `GetQuoinColumnCount()` 两个访问器、
`BuildQuoinBricks()` 一个方法。演示房子实测 **4 根柱 / 48 块砖**（`quoin=48`），
总砖数在无路时 48、六拱时 144。

#### 判据不是"不穿模"，是"那条棱被遮住了"

⚠️ 这一条差点写错。四面墙是**精确 butt joint**（合卷卷一 §4.1 已经量过：零重叠、零缝隙；这是 08-31 的状态，2026-09-13 起转角改为斜接），
所以"角石不能穿模"是一条**恒真**断言，写了也永远绿。真正的破绽是外角那个立面由三块 quad 拼成
（0 号墙端盖 / 1 号墙外面 / 2 号墙端盖），而 `AddQuad` 的 UV 从 quad 局部 (0,0) 起算
⇒ 那两条一个墙厚宽的端盖是**各自独立的 UV 岛、横轴还沿墙厚方向**，砖纹到角就断。

单测 `House.QuoinCoversOuterEdge` 因此测的是：砖的横截面在**两个相邻墙面各自的外法线**上
都伸出墙外表面（余量 > 0.5 cm）。只伸出一个方向 = 只遮住半条棱。四组用例覆盖轴对齐 / yaw 37° /
正方形带偏移 / 带内缩，逐角逐面共 32 条。

#### 与接缝柱共用发射器（A11 的可执行版本）

新抽 `CSHouseFrame::AppendColumn(点, 朝外, 底, 顶, 随机数基, 参数, 元素表, 游标)`：
接缝柱与角石都走它，`CSHouseSeam::BuildCornerElements` 从 40 行缩到 10 行。
理由是 TG 侧的实证（合卷卷五 §1、§3.3）：那边转角、墙裙、缝砖、雉堞、承重柱**全是同一个
`brick` × 逐实例非均匀缩放**，138 个网格里砖只有一个，而每砖 96 字节的记录**没有 mesh 索引字段**。

单测 `House.QuoinSharesTheColumnEmitter` 拿同一组输入喂两条路，逐字段比 `FElement`
（砖数 / 槽位 / Pitch / HalfLen / LayoutScale / Path.TopZ / 三根轴）。谁将来复制一份出去，它就红。

#### 🐛 那条单测第一次跑就抓到一个真缺陷

`Seed ^ (uint32(Index) * K)` 在 **`Index == 0` 时是恒等映射** ⇒ 任意两个家族只要 seed 撞上，
各自的 0 号柱就共享同一个逐实例随机数。接缝砖那条从落地起就是这么写的。

**症状要等到有人给砖材质接上 `PerInstanceRandom` 色差那天才显形**，在那之前砖数 / 位置 /
三角形数所有几何断言全绿 —— 与 S1 那个"随机种子取了槽位"是同一类不会报红的缺陷。

修法：新增 `CSHouseFrame::ColumnRandomBase(Seed, FamilySalt, Index)` =
`HashCombine(HashCombine(Seed, FamilySalt), Index*2654435761 + 0x9E3779B9)`，
家族盐登记在 `CSHouseFrame::EColumnFamily`（`Seam` / `Quoin`）。**接缝砖的随机数值因此变了**，
但没有任何断言钉过它的具体值（只钉过"两房算出同一个"与"槽位移动时不变"），两条都仍然成立。

#### ⚠️ 顺带修回一个被我自己弄失效的守门人

角石**恒出砖** ⇒ `RebuildFrame` 的 `BrickCount == 0` 分支在演示关卡里**不再自然可达**，
而那条分支正是"撤实例源之前先把 GPU counter 清零"的所在（不清就会把陈旧计数器连同 buffer
一起交回给组件，画面上砖原样立着而四条数值断言全绿 —— 2026-08-31 出图抓到过现场）。

原来守它的断言是 `擦掉路 ⇒ gpu == 0`，现在 `gpu == 48`。**如果只把期望值改成 48 就完事，
那个守门人就在这一轮悄悄失效了。** 回归里因此显式补了一段：关掉 `bQuoinEnabled` + 无路
⇒ 断言 `bricks == 0` 且 `gpu == 0`，再打开确认角石回来。

#### 本轮未做（如实列出）

- **`CornerMargin` 四角禁区不等宽**（合卷卷一 §4.1 第 4 条）没顺手改。它作用在 `F.Len` 上，
  而 1/3 号墙的 `F.Len` 已经是 `Footprint.Y − 2T` ⇒ ±X 墙实际保留 `T + 60`、±Y 墙保留 `60`。
  改它会移动 `SplitEdgeIntoSlots` 的可用区间 ⇒ **门的位置与数量会变** ⇒ 演示回归里一批断言
  跟着动。那是另一件事，别和角石混在一轮里。
- **墙裙（A8）** 与**垛口**：`AppendColumn` 已经是它们现成的发射器（各自加一个家族盐即可），
  但两者的驱动曲线不是竖直线段（墙裙沿墙脚、垛口沿墙顶），要先决定"沿边铺"那条路怎么走。
- **角石的交错包角**：2026-09-12 已按 A7 的参考图与二进制复核修正。奇偶控制相邻墙的方向，随机数改变长边；旧版等截面斜柱和仅改 `BlockScale.x` 的建议不再适用。

#### 验收门（本轮）

```text
构建   Result: Succeeded（UETest574_2Editor Win64 Development，-NoUba -MaxParallelActions=3）
单测   Success=98 Fail=0（新增 4 条：QuoinCoversOuterEdge / QuoinSharesTheColumnEmitter
                          / QuoinRandomIgnoresSlot / QuoinTruncatesNeverGrows）
回归   passed=208 failed=0 / REGRESS OK（新增 8 条断言）
零阻塞 八条 flushes=0 全部照绿（角石只是同一趟 dispatch 里多几条路，没有新的同步点）
```


### 包边石已落地（2026-08-31）—— 合卷卷一 A8 / 卷五 A11

计划 D11 实拍图注里那三样（「墙顶城齿、转角角石、勒脚石排 —— 同一种『离散块沿线累积』」）
至此齐了：角石上一轮，**墙顶压顶石 + 墙脚勒脚石**本轮。

新增 `Public/CSHouseTrim.h`；`ACSHouseActor` 侧新增 `bTrimEnabled` / `bTrimTop` / `bTrimBase` /
`TrimTopOffset` / `TrimBaseOffset` / `TrimOpeningClearance` 六个属性、三个访问器、
`BuildTrimBricks()` 一个方法。演示房子实测：**无门时 148 块（顶 4 段 + 底 4 段），
六拱时 114 块（底带被切成 6 段）**。

#### 水平砖路是白拿的：`FPath` 早就有 `EMidKind::Flat`

包边不需要新的路型 —— `Flat` 中段（矩形洞的平顶用的那个）就是水平走向的砖路：
`EvalPath` 给 `OutSZ = (LeftS + T, TopZ)`、切向 `(1,0)`。所以只加了一个发射器
`CSHouseFrame::AppendFlatRun`，与角石的 `AppendColumn` 并列，两者共用同一个 `SolveRun`、
同一套负缝、同一份容量、同一个随机数基。

⚠️ **砖的三轴在水平路上换了位置**：kernel 对切向 `(1,0)` 算出 `OutwardSZ = (0,1)` ⇒
`AxisX`（`FrameBrickDepth`）指向**竖直向上**。也就是**包边这一课的高度是 `FrameBrickDepth`**、
进深是墙厚。四家共用一个组件、一份 `BlockSize`（TG 全库也只有一块 `brick`），
所以想让包边比拱缘更厚，只能连拱缘一起改。

#### ⚠️ 必须避开洞，而且这件事没有任何数值断言看得见

墙脚那条带的 Z 就是门洞的底（门 `Z0 = 0`）。不切的话勒脚石会从门口正中一路铺过去 ——
**砖数、零阻塞、三角形数、GPU 计数器全都照绿**，只有出图看得见。这正是本模块唯一的难点。

切法：把边的 S 区间减去"与本带 Z 区间相交"的洞的 S 区间。**判据带 Z**，所以高窗不切勒脚、
落地门不切压顶。单测 `House.TrimAvoidsOpenings` 把四件事钉住：
两扇落地门把 0 号墙的底带切成 3 段、高窗不切、把窗抬到墙顶则顶带被切成 2 段、
重叠的洞不产生负长度或错序的段。

回归里的可断言证据是**段数**：`get_trim_base_run_count()` 在有门时 6、擦掉路后回到 4，
且合回去之后砖更多（148 > 114）。段数变化只可能来自"切了"。

#### 顺带把随机数基改名

`ColumnRandomBase` → `PathRandomBase`，`EColumnFamily` → `EPathFamily`（新增 `TrimTop` /
`TrimBase` 两个盐）。名字里的 "Column" 在水平砖路上是错的，趁只有两个调用点改掉。
上一轮那条"`Index == 0` 时 XOR 是恒等映射"的修法一字未动。

#### 本轮未做（如实列出）

- **包边不外挑**。四家共用一份 `BlockSize` ⇒ 包边的横跨墙厚方向恒等于墙厚，与墙面齐平，
  没有传统压顶石那种外伸。合卷卷五 §4.2 的实测支持这个取舍：TG 上沿的"更厚更亮"来自
  水平向胀大 + 顶面受光 + 随机层高，**不是**来自更宽的一课。要真外挑得给包边单开一个组件。
- **城齿（垛口）**：包边是它的地基（同一条水平路），但垛口要按砖序号奇偶抬高一格，
  且那是墙**顶**的形态选择（TG 侧是 `FlatRoofRimStyle` 这个屋顶样式枚举，见卷五 §4.3）。
  等要做"平屋顶"时一起。
- **`CornerMargin` 四角禁区不等宽**（卷一 §4.1 第 4 条）仍未改 —— 改它会移动门的位置与数量。

#### 验收门（本轮）

```text
构建   Result: Succeeded
单测   Success=100 Fail=0（新增 2 条：TrimTilesThePerimeter / TrimAvoidsOpenings）
回归   passed=214 failed=0 / REGRESS OK（本轮新增 6 条断言）
零阻塞 八条 flushes=0 全部照绿
容量   258/512，无截断告警
```


### 🐛 岩壳加载后长错地方：`!bChanged` 早退门吞掉了三条派生链（2026-08-31 已修）

用户在 `L_TerrainOpsDemo` 里肉眼发现的：碎石贴在地面矩形的一个角上，而土台上一块都没有。

#### 根因：加载时序 + 一个只考虑了镜像的早退门

`ACSGroundActor::RefreshHeightsInRegion` 末尾本来就串着三条派生链的重建（石阶 / 岩壳 / 裙边摆件），
但函数中段有一道幂等短路：

```cpp
if (!bChanged) return;   // 加载后重导出结果与序列化值一致时不重建、不标脏
```

加载时序是**塑形物先向地面注册、后落自己的变换** ⇒ 那一瞬间打包出来的
`GetHeightFieldParams().Profile.xy` 还是 `(0, 0)` ⇒ 岩壳已经按"塑形物在原点"披挂过一趟。
等变换落定、`RebuildHeightsFromShapers()` 再进来时，**镜像是序列化的、本来就对** ⇒
`bChanged == false` ⇒ 直接 return，**尾部那三条派生链重建永远到不了** ⇒ 壳永久停在陈旧位移上。

这条早退门违反的正是计划 D3 自己的纪律：「消费者无条件重求值、靠幂等哈希兜底」。
三条链各自都有幂等守卫（`RockShellBuiltHash` / `SkirtDecorHash`；石阶是定容单 dispatch、
每笔落笔本来就在跑），真没变时放它们过去是**纯空转**。

#### 修法

`if (!bChanged)` 分支里补上 `RebuildStairs(); RebuildRockShell(); RebuildSkirtDecor();` 再 return。

#### 实测（`Scripts/TinyGladeProbeShellPlacement.py`，本轮新增的探针）

塑形物在 (6400, 6400)、台顶 714.70、`Radius+Falloff = 1400`：

| 时刻 | 活三角 | 质心 | 落在触及范围内 |
| --- | --- | --- | --- |
| **修前** 加载后 | 293 | (577, 671) | **0 / 293** |
| **修前** `RebuildHeightsFromShapers()` 后 | 293 | (577, 671) | 0 / 293 |
| **修前** 显式 `RebuildRockShell()` 后 | 1112 | (6387, 6388) | 1111 / 1112 |
| **修后** 加载后（三个阶段读数全相同） | **1112** | **(6387, 6388)** | **1111 / 1112** |

最刺眼的一条现象：修前那 293 个活三角**带着土台的 Z**（区间 [−1.8, 713.2]，台顶 714.7）
却挂在原点附近的平地上 —— 它们是"塑形物还在原点"那一趟的遗物。

#### ⚠️ 为什么演示回归一直全绿（这条比修法本身更值得记）

两层原因叠在一起：

1. **`demo_rock_shell` 在量岩壳之前先画了路。** `PaintRevision` 在 `RockShellInputHash` 里
   （壳在路上是连续下沉，画一笔就得重披挂），落笔走的是**另一条**重建路径 —— 于是它在测之前
   就把陈旧态顺手治好了。
2. **那三条断言（skirt / plateau / flat）是相对塑形物自身位置分类的**，塑形物在哪它们都绿。
   它们能证明"壳贴着裙边"，**证明不了"壳在该在的地方"**。

⇒ 新断言 `straight after load the shell already hugs the shaper (no rebuild needed)` 因此必须
排在 `demo_rock_shell` 的**所有重建之前**（`set_editor_property` / `rebuild_terrain` /
`reset_paint` 每一句都会把它治好），判据是**活三角质心到塑形物中心的距离 ≤ Radius + Falloff**。

#### 这一类缺陷的通用形状（第八条被实测推翻的推断）

"派生链的输入变了，但触发重建的那个判据只看了另一样东西"。前面已经栽过同型的：
门框砖的陈旧 GPU counter、S1 的随机种子取槽位、`bDoOverrideArgs` 那次。
共同点都是**几何断言全绿、只有出图或专门的位置判据看得见**。

#### 验收门（本轮）

```text
构建   Result: Succeeded
单测   Success=95 Fail=0
回归   passed=207 failed=0 / REGRESS OK
探针   PROBE OK（TinyGladeProbeShellPlacement.py，5/5）
```

⚠️ 本轮验收门是**等另一个会话的重构（`FCSRoofDesc` 脊向改为由长轴导出）落地之后**才跑的 ——
中途有一段时间模块只有测试侧编译不过（114 个错误全在三个测试文件），那不是本轮改动引入的。


### 四坡屋顶 + 屋面瓦（2026-08-31，另一会话的重构 + 本轮资产收尾）

当前屋面由 `CSHouseTile` 生成，脊向从长轴派生，正方形退化成金字塔顶。屋面瓦资产和排布已于
2026-09-29 按用户确认的 TG 实拍重做，当前值和验收集中在
[`Roof_TerraceAndRidge.md`](Roof_TerraceAndRidge.md#屋面瓦资产按-tg-真值重做2026-09-29)。

- `RoofTileMesh` 使用 `/PCGPlugins/HouseTest/SM_TinyGladeRoofTile`；新烘焙为 45 × 64 × 10 cm，默认排距 / 列距为 45 / 45 cm。
- 水平屋脊改为分段平躺厚板；09-12 的 3 cm 厚薄瓦、16.9 / 27.75 cm 排距和旧盖脊说明已退出当前配置。
- `M_TinyGladeRoof` 消费 `roof_colors_layer00`、法线与破损图集，保留受光和逐瓦色差。
- 09-12 的材质移植依据仍保留：[shader 摘录](evidence/roof-shader-20260912.txt)、[原始顶点](evidence/roof_tile-source.json)。`roof_tile_lod1` / `roof_tile_backface` 不是随机瓦型；5×5 图集与顶面法线标记不可按包围盒猜测。
- 重建入口为 [TinyGladeSetupRoofTiles.py](../../Scripts/TinyGladeSetupRoofTiles.py)，调用瓦片与材质构建脚本，并更新 CDO 与实例。

保留两条验证经验：既查产量也查 `get_roof_tile_undrawable_reason()`；UE Python 的向量属性可能是活引用，做还原快照应保存分量值。

### 屋面收尾一轮：瓦密度定档 + 脊瓦 + 尖顶（2026-08-31，用户逐条指定）

旧瓦密度、厚度和盖脊排布已被 2026-09-29 方案取代，不再维护重复参数表。
房子墙顶不生成样条墙式压顶；平屋顶露台构件归屋顶，见 [`Plan_Wall.md`](Plan_Wall.md)。
瓦片验证仍需覆盖右手坐标基；坡面测试和脊瓦测试应分别构造夹具，避免把合法的脊瓦计入坡面断言。

以下保留尖顶的逆向依据和实现注意点。演示资产是否启用以屋顶专题与配置脚本为准，历史测试数字不是当前基线。

#### ④ 尖顶：脊端点那两处三面交汇（`RoofFinialMesh` / `Scale` / `Sink`）

先把 TG 挖清楚了再写（PDB + 反编译 VS，本轮新查）：

| 证据 | 内容 |
|---|---|
| `system_roof::visual::place_spires::place_roof_spires` | 产出系统，本轮才发现（此前"`system_roof::visual` 七个函数"那次盘点漏了它） |
| `Query<(&Roof, &RoofAnimation), With<RoofSpire>>` + 一次 `filter_map` | **一座屋顶最多一根**，且有屋顶拿不到（平顶 / 山墙那一档） |
| `RoofSpire { vec4 position; float roof_animation_t; float roof_profile; float radius; }` | **没有任何旋转** —— 尖顶恒竖直、绕自身轴对称，摆位只是一个点 |
| `_instanced_roof_spire` 的 VS | 只对 `roof_profile_mult > -0.001` 的顶点（底裙那 504/828 个）横向放大 `radius·2 · lerp(.6,1,mult) · lerp(.4,1.5,1-profile)`，竖直分量一动不动，最后整体 **×0.5** |
| `roof_spire.json` | 828 顶点，属性只有 `Vertex_Position / Vertex_Normal / roof_profile_mult`，**没有 UV** |

于是：「屋顶越大裙摆越张、杆子不变粗」是 TG 的做法，本项目的 `RoofFinialScale` **默认 0.5 就是
那个硬编码常数**。

⚠️ **本项目摆的是"每个屋脊端点一根"，与 TG 的"每屋顶一根"有意分叉**（`roof_tip_offset_xz` 给的
是一个点，他们尖顶那一档本来就是锥/金字塔）。我们的四坡脊是一条**线段**，两端各有一处
「两条角斜脊 + 一条屋脊」三面交汇的破口，用户指的正是那里。正方形时脊长为 0、两端重合 ⇒
自动退化成金字塔尖上的一根，**不需要为金字塔写特例**（断言在回归里）。

实现上刻意**没有**再复制一套 palette / 容量 / 交接机器 —— 一两根而已，走普通
`UStaticMeshComponent`。三处值得记：

- **竖直轴从包围盒判**（最长的一轴 = 朝上）。源 JSON 是 y-up 的，本仓库导入后已转成 Z-up
  （实测 220.6 × 220.6 × 303.8 cm，判定 `up axis=2`）。自动判轴让**两种口径都不必手工转资产**；
  代价是"矮胖的尖顶"会判错轴。
- **枢轴直接放在脊端点上**：TG 那张的枢轴恰在裙摆与杆子的交界（包围盒 `z ∈ [−85.8, +218.0]`），
  裙摆天然垂到瓦面以下把破口盖住（0.5 缩放下垂 42.9 cm）。换枢轴在底面的资产用 `RoofFinialSink` 压。
- **必须用三平面材质**：`roof_spire` 没有 UV，`MI_roof_tile_normal` 这种跟 UV 走的会采到垃圾。
  接的是石阶那轮建的 `M_TinyGladeStone`。

⚠️ 哈希短路里**组件数也要进条件**：蓝图重跑构造脚本会销毁 transient 组件而形态哈希一个字没变，
只比哈希的话尖顶会在重跑构造脚本后**永久消失**。

⚠️ 这是 `ACSHouseActor` 身上**唯一**的 `UStaticMeshComponent`，而 `CSVineScatter` 的
`CollectSurfaceTriangles`（`CSVineScatter.cpp:38`）恰好按这个类型遍历 —— 旧的 `VineScatter`
三个入口对房子此前**恒返回空三角集**，现在会变成"只有尖顶那点面"。房子自己的藤蔓走另一条
通路，不受影响；但把房子喂给旧入口的话，藤蔓会全爬在尖顶上。

#### 验收

建库无错；单测 **103 / 0**（新增 `House.TileRidgeCaps`：坡面瓦 478 + 脊瓦 55，金字塔 56）；
回归 **224 / 0**（新增尖顶 7 条：可画性 / 矩形两根 / 正方形退化成一根 / 回矩形复原 /
撤资产组件销毁 / 原因串说得出断在哪一环 / 接回来复原）。演示关卡实测每栋 827 片瓦 + 2 根尖顶。

### 岩壳「石头隆起」六参数已落地（2026-08-31，用户规格）

用户看图提的两条：**包边缝隙过多、包边太小（没有体积）**。追下来是两件独立的事，外加一次
对 TG 位移公式的反编译 —— 后者把我自己前一版的修法直接否掉了。

#### 先算的那笔账：缝隙不是图案漏的

图案本身零重叠零空隙（盖 86.60% + 裙 13.40% = 100.0000%）。缝来自**整个胞腔被判死**：

```text
裙边环   宽 8.0 m   面积 503 m²   周长 63 m
胞腔     5.53 m（原生）           面积 30.6 m²
环里放得下 16.4 个   实测活了 13.7 个     胞腔/环宽 = 0.69
```

**整圈只排得下一层胞腔**，坡度 mask 再啃掉边缘的半个 ⇒ 画面上就是"几块大板 + 大缝"。
TG 的 5.53 m 是给**真悬崖**用的，套到 8 m 裙边的小土台上本来就不成立。

⇒ 用户裁决走 **C：`RockShellPatternScale` 默认 1.0 → 0.35**（胞腔 1.94 m，能排四层）。
**"与 TG 同绝对密度"这个锚点被有意放弃**，`CSGroundActor.h` 的字段注释已改写记录这条。

⚠️ **连带后果必须记住**：缩放缩的是**整张 tile**，0.35 之后 tile 只有 **47.8 m** < 128 m 的地面，
**覆盖区外静默无壳**，而平铺补不上（tile 不是周期的）。`RebuildRockShell` 因此新增一条
逐塑形物的覆盖区检查，超出会打 Warning。

#### 🔍 反编译 TG 的位移 kernel，推翻了我自己的第一版修法

我最初把"没体积"归因于 `Relief × Rock` 被 mask 吃掉，加了个 `ReliefFloor` 下限（默认 0.5）。
用户随后提出一个更具体的假设，去 `_rocky_terrain_displace_rocky_terrain.cs` 逐行核，**假设成立**：

```glsl
// _748 = 地形法线   _741 = |∇h|   _752 = rocky_terrain.x（岩石 mask）   _727 = 130/dims（格距 m）
_761 = 贴地位置
     + N * mix(-0.3, 0.1*mix(0,3,rand(cell)), cell_bby) * mask * (1-road) - N*road   // ← 我们已有的全部
_1030 = _761
      + N * ( mix(-0.2, 1 - smoothstep(4, 7, |∇h|), noise01)     // 噪声决定这一点凸出还是缩进
              * smoothstep(0, 0.3, rocky.x) * |∇h| ) * 0.3       // ★ 突起量 ∝ 坡度
      + N * rocky_terrain.y;                                      // ★ 我们从没读过的第二个通道
```

**`_761` 恰好就是我们原有实现的全部** —— TG 的 `Relief × mask` 与我们写的一模一样，我们抄对了。
体积来自另外两项：**突起量正比于地形陡峭度**，以及 `rocky_terrain.y` 那份偏移。
`1 − smoothstep(4,7,|∇h|)` 还说明极陡崖面上突起反而收回去，只有中等坡度最鼓。

⇒ **`ReliefFloor` 默认改回 0（TG 口径）**，退回纯调参用途；体积的职责交给下面这一组。
（抬它的副作用是实测过的：0.5 会让 `RockShell.DrapesOnSlopes` 的最大垂直距离从 66 涨到 83.9 cm，
那条容差是按"厚度被 mask 压着"标定的。想再抬就得连容差一起重新标定。）

#### 六个参数（全部蓝图可调，`CS Ground|Rock Shell|Rise`）

| 属性 | 规格条目 | 默认 | 作用 |
| --- | --- | --- | --- |
| `RockShellCellExpand` | ② ⑤ | 12 cm | 每片沿 **−DirToCentroid**（朝外）扩张，闭合片间缝。带到质心的半径淡入，否则内部点被推过质心翻转三角 |
| `RockShellRiseMultiplier` | ③ | 1.25 | **石头高 = 地形高 × 本值**。1.0 = 贴地（旧行为） |
| `RockShellRiseNoiseAmount` | ④ | 25 cm | 隆起噪波幅度，按隆起量淡入（平地不加，否则整片地起毛刺） |
| `RockShellRiseNoiseWavelength` | ④ | 400 cm | 噪波波长 |
| `RockShellRiseExtend` | ⑥ | 150 cm | **台顶外扩**：把每座塑形物的 `Radius` 临时加上本值再求值 ⇒ 隆起的衰减比地面**晚**这么远。加 `Radius` 而不是缩 `Falloff` —— 后者会压陡裙边，连带改掉坡度 mask 与石阶等值线 |
| `RockShellEdgeCeiling` | ⑧ | 0.9 | 末端边缘的隆起 / 地面高度 上限，**必须 < 1**（=1 会让最外圈与地面共面 ⇒ 一圈 z-fighting，内部再夹到 0.999） |

新增共享求值口 `GroundShaperHeightAtXYExpanded(P, RadiusBonus)`（`CSGroundShaperField.ush`）。
⚠️ 它**没有 CPU 孪生** —— CPU 侧没有消费者（镜像、拾取、落座读的都是真地面高度）。真要加孪生
就得连 `GroundShaper.CpuGpuFieldParity` 一起扩。

#### 🐛 ⑧ 的门写错了两次，两次都被断言抓住

「末端边缘」这个门比想象中难写，两版都报红：

| 版本 | 权重 | 症状 |
| --- | --- | --- |
| v1 | `1 − RockMask` | 坡度 mask 在 band **两端**都掉到 0，内侧那端是台子肩部、地面还有六七百 cm 高 ⇒ 把石头压进台子里，`DrapesOnSlopes` 66 → 68.7 cm |
| v2 | `smoothstep(0, CellRelief, RiseH − GroundH)` | 外扩把**整条剖面**往外推 ⇒ 整条裙边上两者都差一大截 ⇒ 夹紧全程生效，内圈被压到地面以下 **−62.3 cm** |
| **v3** | `(1 − RockMask) × (1 − smoothstep(0.15, 0.5, GroundH / PeakHeight))` | ✅ 两端都要求"band 正在终止"**且**"地面已经落回基底"，只有外侧那端同时成立 |

真正区分「内侧肩部」与「末端」的只有**绝对高度**。峰值取现成的 `MaxAbsHeight`
（`RefreshHeightsInRegion` 里按 `PeakHeight()` 维护的那份台账），不为它新开调参量、也不重扫一遍。

#### 断言

单测 `RockShell.DrapesOnSlopes` 拆成两条职责：

- **披挂契约在「隆起中性」下量**（`RiseMultiplier=1` / `RiseExtend=0` / `RiseNoise=0` /
  `CellExpand=0` / `PatternScale=1` / `ReliefFloor=0`，测完还原）。⚠️ 这不是"调到能过"：
  那条容差是在原生密度 + 厚度被 mask 压着的前提下标定的，把这些量归中性才是让它继续测它该测的东西。
- **隆起单独断言**：同一批裙边顶点，倍数 1.0 vs 2.0 的 Z 均值必须抬高 > 20 cm。
  倍数写死 2.0 而不是读属性 —— 读属性的话，谁把默认调回 1.0 这条就变成恒真的空判据。

演示回归新增三条（`demo_rock_shell`）：

```text
[PASS] the rise really lifts the shell above the ground on the inner skirt   meanInner=299.6 cm n=3283
[PASS] the outer edge tucks back into the ground (it never floats)           worstOuter=15.9 budget=41.0 meanOuter=-2.8 n=69
[PASS] the inner skirt really does stand higher than the outer edge          299.6 vs -2.8
```

⚠️ 外缘那条量的是「顶点 Z − 该点地面高度」，而顶点 Z 里除了隆起还含**壳自身厚度**
（`CellRelief` ± 与表面噪波）。所以口径是"高出去的部分不许超过壳自身的厚度"，
**不能**写成"必须为负"。

#### 未做（如实列出）

- **`rocky_terrain.y` 那份偏移没实现**，显隐 mask 也仍然只用 `.x`（TG 用的是 `x + y`）。
  用户的规格用「高度倍数」取代了 TG 的「坡度正比」，这一项因此暂时没有落点。
- ~~**TG 的 `1 − smoothstep(4,7,|∇h|)`（陡坡收回突起）没有对位物**~~
  —— **2026-08-31 稍晚已补**，连同整条「幅度 ∝ 坡度」一起，见下一节。

### 岩壳体积再补两层：TG `:563` 基准偏移 + 表面起伏改坡度比例（2026-08-31）

上一节结尾那两条「未做」是拿反编译逐行核出来的，但当时只落了**用户规格的六个参数**，
TG 自己那两层体积一层都没抄。这一轮把其中两层补上 —— 都与 ③ 的隆起倍数**正交**，
没有改动任何已记录的裁决。

#### 为什么是这两层：TG 的体积从来不是"按台子多高放大"

| | Tiny Glade | 本项目 ③（用户规格） |
| --- | --- | --- |
| 驱动量 | **坡度** `\|∇h\|` | **地形绝对高度** `H` |
| 上界 | 有界：基准 `mix(−0.4, +0.2)` + 厚度 0.3 m + 起伏(∝坡度) ≈ **0.9 m 封顶** | 无界：`H × 1.25`，700 cm 台肩抬 175 cm |

③ 在**缓坡小土台**上几乎给不出体积（台低 ⇒ 倍数乘出来的差值也小），而那正是本项目的常见配置。
补上的两层都是**有界常量级**的，缓坡上照给 —— 两者互补，不是二选一。

#### ① 基准偏移（`RockShellBaseLift` / `RockShellBaseSink`，20 / 40 cm）

TG `_rocky_terrain_displace_rocky_terrain.cs:563`：

```glsl
_718.y = H + mix(-0.4, 0.2, clamp((mask + rocky_terrain.z) - 10.0 * path, 0.0, 1.0)) - <水体两项>;
```

沿**世界 Y**（我们是世界 Z）。mask 满 ⇒ 整张壳浮起 +0.2 m；mask 空或路上 ⇒ 沉 −0.4 m。
我们落成 `Rise += lerp(-BaseSink, +BaseLift, saturate(Rock - Road))` ——
`Road` 已经乘过 `RoadFade`（= TG 的 10×），所以 `Rock − Road` 就是那条 clamp 的逐项对位物。
`rocky_terrain.z`（笔画自带的岩石度）本项目没有那条通道，是这一层唯一缺的输入。

⚠️ **加在 ⑧ 的末端夹紧之后**：末端 `Rock → 0` 时本项恰好取满 `−BaseSink`，与 ⑧ 同向往地里扎，
放后面不会把刚夹好的末端又顶回去。实测外缘均高从 **−2.8 cm 变成 −42.0 cm** —— 原来是"刚好蹭着
地面"，现在是真的扎进去。

#### ② 表面起伏改成「幅度 ∝ 坡度、且偏正」（`RockShellNoiseAmount` 语义变更）

TG `:721`：

```glsl
N * mix(-0.2, 1.0 - smoothstep(4.0, 7.0, S), n01) * smoothstep(0.0, 0.3, rocky.x) * S * 0.3
```

旧写法是常幅对称的 `Turbulence × NoiseAmount`（默认 6 cm）。三处差都是承重的：

| | 旧 | 新（TG 口径） |
| --- | --- | --- |
| 幅度 | 常数 6 cm | `NoiseAmount × 坡度`，默认 30 cm = TG 的 0.3 m |
| 对称性 | 对称 ⇒ **净体积为零** | `mix(-0.2, Upper, n01)` 偏正，凹陷最多占两成 |
| 陡崖 | 无 | `1 − smoothstep(4,7,S)` 把极陡面（>76°）的突起收回去 |

⚠️ **`RockShellNoiseAmount` 的语义变了**：现在读作「坡度 = 1 时的满幅」。演示土台最陡 1.3125
⇒ 实际值域约 **−7.9 .. +39 cm**。凡是拿它当"最大起伏"的地方都要乘坡度，本轮已同步三处：
`CSGroundRockShellTests.cpp` 的 `DrapeTolerance` 与 `SinkLimit`（新增
`CSRockShellTest_MaxSlope = Lift × 1.5 / Falloff`）、`TinyGladeDemoRegression.py` 的 `lift_budget`。

#### 断言

- 单测新增 ⓒ''「基准偏移把壳浮起来了」：`BaseLift` 0 → 100 cm，同一批裙边顶点均高
  **316.8 → 383.1 cm**（+66.3）。写死 100 而不是读属性，同 ⓒ' 的理由 —— 读属性的话谁把默认
  调成 0 这条就变成恒真的空判据。
- `DrapesOnSlopes` 的中性块**同时归零 `BaseLift` / `BaseSink`**：它和「石头隆起」那一组同类，
  都是**有意**把壳推离坡面的。最大垂直距离 66 → **75.6 cm**（容差 99.4 = 30 + 30×1.3125 + 30）。
- 回归 `demo_rock_shell` 新增一条**与调参量无关**的门：外圈**平均**必须在地面以下。
  上一条的上界跟着 `NoiseAmount` 一起涨，涨到某个值之后它就不再是个门；这条钉的是符号，不是量。

#### 仍未做

- **`rocky_terrain.y` / `.z` 两条通道**：TG 的显隐判据是 `smoothstep(0.1, 0.2, x + y) + water > 0
  || z > 0`，基准偏移的参数也含 `.z`；我们只有坡度那一路（`.x` 的对位物）。这两条是**笔画自带的
  岩石度**（`rasterize_terrain_stroke.cs:1442` 逐笔取 max 写出来的），要补就得先有"笔画带属性"
  这个概念，不是改 kernel 能解决的。
- **水体三项**（显隐、`−smoothstep(0.5,0.6,water.y)×0.2` 下沉、水线附近的低空侵蚀噪声）：
  本项目没有水系统。
- **逐胞腔径向胀缩的方向与随机性**：TG 是 `dir × 0.001 × mix(-3, 8, rand)`（−19.5..+52 cm、
  无淡入、**偏向收缩**），我们的 `CellJitter` 默认 0（关着），取而代之的是 TG 没有的
  `CellExpand = 12 cm 恒定朝外`（那是 Houdini 原型 `attribwrangle3/offset` 的口径）。
- **`PatternScale = 0.35` 把烘死的 `LipOffset` 一起缩了**：19.5 → 6.8 cm，而裙边竖直高度
  = 局部坡度 × LipOffset ⇒ 同坡度下我们的裙墙只有 TG 的 35%。这是缩放裁决的下游后果，
  之前没有单独记过。
- **坡度判据比 TG 钝一倍**：TG 的梯度是 heightmap 上 2 texel ≈ 0.51 m 的中心差分
  （`_terrain_derivative.cs:17` 的 `_59 = 130/dims × 2`），我们是 `2 × CellSize = 1.0 m`。

### 石墙冠缺陷：「石头隆起」默认归中性（2026-08-31 晚，用户看图裁决）

用户贴 `L_TerrainOpsDemo` 截图：壳读成一圈**独立火山口壁** —— 顶圈比台顶高一大截、内壁带
阴影、外立面近乎垂直，台顶陷在墙圈里。指令是「对比原版 TG 进行修改」。

#### 根因是量纲，不是哪一项参数

TG 的壳**从不离开地形**：全部位移有界（基准 ±0.2/−0.4 m + 厚度 0.3 m + 起伏 ∝ 坡度，
合计封顶约 0.9 m）。「石头隆起」组的 `RiseMultiplier` 却是随台高线性长的**无界**量 ——
演示档 `Lift=700` 下 1.25 倍在台肩处就是 +175 cm；`RiseExtend=150` 再让满高外推 1.5 m
（外立面变垂直）；叠上基准 / 浮高 / 隆起噪波合计约 **2 m 石墙冠**。

⇒ **三个默认归中性**：`RiseMultiplier` 1.25→1.0、`RiseExtend` 150→0、`RiseNoiseAmount` 25→0
（TG 没有沿世界 Z 的噪波，它的表面细节全在 ∝ 坡度那条起伏里）。体积由 TG 口径的三层承担
（`BaseLift`/`BaseSink`、`CellRelief`、坡度比例起伏）；**四个参数保留为风格化旋钮**，拧它们
就是有意离开 TG 形态。关卡实例无序列化覆盖（查过，没有脚本钉这几个值），改默认即生效。

#### 缺陷为什么逃过了全部断言 —— 补了两条反火山口

单测在**中性档**量披挂、ⓒ' 只量倍数**差值**；**默认档的绝对形态从来没人量过**。补上：

- 单测 ⓒ'''：恢复默认后，最高活顶点相对**台顶** ≤ `BaseLift + CellRelief + NoiseAmount×最大坡度
  + 40`。实测 **−37.3 cm**（预算 129.4）—— 火山口态约 +200，一眼红。
- 回归 `demo_rock_shell`：内圈最高顶点相对**局部地面** ≤ `BaseLift + (CellRelief +
  NoiseAmount×坡度) × √(1+坡度²) + 40`。实测 161.0 ≤ 174.5。
  原来那条「内圈均值 > 0」只查符号，删了（倍数差值仍由单测 ⓒ' 钉着）。

⚠️ **√(1+坡度²) 那一项是踩出来的**：厚度与起伏沿**地形法线**位移，而判据量的是「顶点 Z −
顶点自己 XY 处的地面」—— 法线位移把顶点水平推向下坡侧，脚下地面跟着降，竖直偏差 =
法线位移 × √(1+g²)，52° 坡上是 **1.65 倍放大**（直觉里的 ×N.z 衰减方向是反的）。
第一版预算没乘它，在**正确的**壳上误报 161 > 129。单测 ⓒ 除以 √(1+g²) 折垂距是同一件事的对偶。

验收门（本轮）：构建 Succeeded、单测 105/0、回归 `passed=231 failed=0 / REGRESS OK`。

#### 出图复核 + 两条追加实测（同日稍晚）

`TinyGladeShotRockShell.py` 全机位重拍：近景/俯视/画路三张都已是 TG 形态（石头贴台披挂、
台顶留草、路上壳沉两侧完好）。像素判据 A = 67.9%（阈 29%）、C = 5.3%（阈 2.75%），
三次独立启动逐位相同。

- **裂缝限额单开**：近景 `wall_shell_on` 有 0.772% 像素精确 (0,0,0) —— 全在胞腔裂缝槽底
  （相邻胞腔浮高差把缝拉成全遮蔽深槽，间接光真值为零；其余五张 ≤ 0.077% ⇒ 预热是好的）。
  坑 ⑨ 的 0.5% 阈把「预热失效」与「几何真黑」混在一起了，给这一张单开 `CREVICE_ZERO_FAIL
  = 2%`（预热失效历史上是两位数百分比，2% 对它仍一眼红）。想消裂缝黑调 `RockShellCellExpand`，
  那是观感决策，不由出图阈值倒逼。
- **垂直拖动的显隐换批已实测对齐 TG**（用户提问驱动）：塑形物 `PostEditMove` 每帧
  `RebuildTerrain`，`ComputeTopHeight = max(0, (Z − 地面Z) + LiftHeight)` 把 actor Z 折进台高，
  岩壳哈希含 `Top.X` ⇒ 拖一帧披挂一帧。逐 50 cm 下压实测：7922 → 7706 → … → 4354（宏观
  坡度恰到阈 0.75）→ 1176 → **0**（台高 200），每档掉几百上千个三角 = **整批胞腔一起 NaN**，
  不是压扁渐隐；阈下那条尾巴（台高 400→200）是裙边噪声局部超阈的散石在分批走 —— 正是 TG
  「不是形变，是换了一批在演」的机制。拉回原位逐位复原 7922（哈希幂等）。
  探针脚本：scratchpad `probe_vertical.py`（一次性，未入库）。

### 裙圈倾斜 `RockShellSkirtTilt` 已落地（2026-08-31 稍晚，用户方案）

用户贴 TG 实拍近景确认机制：**TG 的腔壳互相插着** —— 裙墙滑到邻块盖底下，缝是被压住的
阴影线，不存在透底间隙。落地方案照抄这个机制、不动 UE 大体架构：

- kernel（`CSGroundRockShell.usf`）：**只把底圈**再沿 −DirToCentroid 朝外推
  `RockShellSkirtTilt`（图案空间 cm，默认 20，世界 = ×PatternScale）。顶圈与盖一动不动 ⇒
  缝的顶宽、披挂契约都不变，改的只是裙墙**倾角** —— 竖直裸墙变外撇斜壁，相邻胞腔的底圈
  各自越过 Voronoi 边界钻到对方裙下，两片斜壁在 V 槽中段**互相交叉**。
- 不乘半径淡入：底圈天然在胞腔边界上（|P − 质心| ≈ 胞腔半径），没有"内部点被推过质心"的风险。
- 与 `CellExpand` 的分工：那个整片平移（含盖）收缝的**顶宽**；本值只动底圈，封的是**槽底视线**。

实测：出图近景纯黑像素 **0.772% → 0.000%**（斜壁交叉后没有视线能进槽底），壳开关差异
67.9% → 70.8%（斜壁多占了点剪影）。消裂缝黑的正式旋钮从此是它，出图脚本注释已同步。

#### 🐛 顺带修的一条：演示关卡被"上下拖动实验"存进了下压状态

回归三条地形断言突然红（台顶 393.8 vs 期望 803.2 ≈ **恰好一半**）—— 不是高度场坏了：
`Shaper_Mound` 被用户在编辑器里垂直拖到 **z = −401** 后连关卡一起存盘（`LiftHeight` 也被
改到 ≈787），`ComputeTopHeight = (Z − 地面Z) + Lift` ⇒ 台顶剩一半。而基准态恢复工具
`TinyGladeResizeDemos.py` 的 `move()` **有意保留 Z** ⇒ 恢复不了这种状态。已给该脚本的
塑形物段加**垂直归位**（Z 拉回地面 Z，带日志），跑一遍即复位（`z -401 -> 0`）。

验收门：构建 Succeeded、单测 105/0、回归 `passed=231 failed=0 / REGRESS OK`、出图 VERDICT OK。

### 边缘磕碰带 `RockShellChipAmount` 已落地（2026-08-31 稍晚，用户诉求）

用户贴 TG 实拍问"岩石边缘的小磕碰是材质还是模型"。答案是**模型**：TG 逐顶点 FBM 的高频段
打在边缘折线顶点上（角点钉死），剪影处读成豁口、面上读成细碎小凹凸；材质只贡献石纹条纹。
但我们的 `RockShellNoiseAmount` 出不来这个效果 —— 那条是 TG `:721` 的大起伏（波长 1.5 m、
幅度 ∝ 坡度、三倍频摊薄高频），调短波长会整张脸一起碎。

⇒ 先落了一版**独立高频带**（40 cm Turbulence，满脸小凹凸）；**2026-09-01 反编译读全
`displace:618` 后换成 TG 正版机制 —— mask 域扭曲**：

```glsl
_818 = (rest.xz × 0.5) + vec2(rockMask × 20.0);   // 空间底 ~130 m ≈ 常数；变化全来自 20×mask
for (; _822 < 1; ) { ... }                         // 单倍频 value noise（倍频数就此确证：1）
```

mask 饱和的带内偏移恒为 20 ⇒ 噪声局部常数 ⇒ **面干净**；带边缘 mask 爬坡的一两米里域坐标
扫过 ~14 格 ⇒ 相邻顶点互不相关 ⇒ **磕碰自动集中在边缘**；沿边缘的锯齿来自 mask 自身微起伏
（TG 是逐笔 FBM 侵蚀，我们由裙边噪声提供）。UE 落地：
`CSGSF_ValueNoise(XY × ChipFreq + Rock × 20)`，`ChipAmount` 默认 10 cm、
`ChipWavelength` 改为**空间底**语义（默认 300 cm，只兜 mask 平坦的走廊）。

⚠️ **订正昨天的说法**：「不能只在边缘、否则撕缝」只对**按顶点旗标**的门成立。TG 用
**连续场做域扭曲** —— mask 是 XY 的连续函数，同 XY 双胞胎拿同一个偏移，边缘集中与
不撕缝两头都占。光栅侧同时验了：PS 只有三平面 albedo，**无法线贴图** ⇒ 磕碰 100% 是几何。

四处预算随之 += Chip 项（Contract 的 OwnRelief、DrapeTolerance、CrownBudget、SinkLimit；
回归的 lift_budget / crown_budget 同步）。出图 `overhead` 因盖缘高低错落多出 7 个槽底真黑
采样点（0.54%），并入 `CREVICE_ZERO_FAIL` 同限。

验收门：构建 Succeeded、单测 105/0（一次 CEF `BUseSupportedRHIRenderer` 启动崩溃与改动
无关，重试即绿）、回归 231/0、出图 VERDICT OK。

### D8 收口：`ACSWindowMarker` 已落地（2026-08-31）—— 对照文档 A6 — 正文已并入 [House_Window.md](House_Window.md#d8-收口acswindowmarker-已落地2026-08-31-对照文档-a6)


### 墙面「灰泥剥落 + 凸出砖」：09-03 建的图，09-04 第一次在引擎里看

材质本体（`M_TinyGladeWall` 的重建，TG `_nani_plaster` 的移植）是 2026-09-03 落的，逐条口径
在 `Scripts/TinyGladeMakeWallMaterials.py` 文件头。**但那一轮的收尾只到 numpy 逐像素仿真为止**，
默认值全是对着没有光照、没有 tonemap、没有天光的离线图调出来的，
材质从没在引擎里渲染过一次。本节记的是 09-04 补上的那一步和它翻出来的东西。

#### 仿真与引擎差了多少：三条默认值全错，而且错的方向是**反的**

第一次进引擎（`Saved/TinyGladeShots/walltex_v2_*`，同机位两趟：`MI_TinyGladeWall` 原样
vs 同一张母材质但 `PeelAmount=-5`/`ProtrudeLevel=5` 的纯灰泥 MID）：

| 判据 | 实测 | 读法 |
| --- | --- | --- |
| 两趟差异像素率 | 墙脚 66.0% / 半墙高 43.3% / 正对面 48.1% | 剥落与凸出**确实走到了像素**（不是"材质挂上去了"），但**六成的墙脚是砖** |
| 差异像素在 peel 那趟的均色 | `(0.107, 0.095, 0.006)` | R≈G≫B，暖调、非中性灰 ⇒ 是砖色卡，不是引擎默认材质（那是 R≈G≈B） |
| 纯灰泥那趟的全图标准差 | 0.058 ~ 0.156 | 不是纯色 ⇒ 灰泥贴图真的采到了 |

三条判据全绿，**机制没问题**；出问题的是默认值，而且是仿真原理上预告不了的三条：

- **剥落覆盖了大半面墙，与计划 §327「墙面本身是灰泥，砖只出现在三处」正好相反。**
  真因不是参数调过头，是**移植时漏了一项**：TG 的灰泥是一张**只铺在部分墙面上的独立网格**，
  侵蚀场里的 `cov`（覆盖度）在每块灰泥边缘自然衰减 ⇒ "哪儿是灰泥"由网格决定，噪声只负责啃边。
  本项目墙板本体就是灰泥、铺满整面墙，`cov ≡ 1`，于是噪声一个人说了算。
  修法是加一项 `PeelBias`（默认 −0.30，本项目独有、TG 无对位物）把整条门槛抬起来，
  顶替那份缺失的覆盖度；`PeelAmount` 保持 TG 的 0..1 语义不动。
- **`GrooveDarken = 0.45` 把砖缝压成纯黑沟**，整面墙读成鳞片。参照图
  `img/tiny-glade-ref-corner-pillar.jpg` 里的缝是细暗线。改 0.75。
- **`PlasterTiling = 0.2` ⇒ 世界周期 10 m**，一面 6 m 的墙只看得到一整块渐变 —— 灰泥贴图
  等于没采。改 0.5。
- **`ProtrudeLevel = 0.88` 读成一地碎屑**而不是"偶尔几块石头顶出来"。三档同机位对照
  （0.83 / 0.88 / 0.93）选定 **0.93**。

**改后同判据复测**（`walltex_v3_*`，同机位、同两趟）：差异像素率
**墙脚 50.6% → 半墙高 6.5%**（改前 66.0% → 43.3%），正对面 48.1% → 18.9%、转角 32.5% → 5.2%。
"墙脚破得多、越往上越完整"这条高度渐变**现在才读得出来**；改前那组数字之间几乎没有落差，
说明高度项被噪声整个淹掉了。母材质从 49 节点 / PS 363 变成 50 节点 / PS 364（`PeelBias` 一项）。

#### 🐛 `plaster_colors_layer00` 不是"那张灰泥贴图"，是**配色档 0 号**

这条值得单记：`plaster_colors_layer00..07` 与 `brick_colors_layer00..04` 是 TG 让玩家**逐房选**
的配色档（P00 三文鱼粉 / P01 赭黄 / P02 灰蓝 / P03 炭灰 / P04 淡灰绿奶白 / P05 鼠尾草绿 /
P06 浅褐 / P07 藕粉；B00 米黄带红纹 / B01 淡奶黄 / B02 墨橄榄 / B03 橄榄黄 / B04 灰藕）。
09-03 挑的"layer00"只是**下标 0**。在本关卡的光照下（天光下半球是草地绿
`(0.10, 0.14, 0.055)`，D14 有意为之）P00 读成暗红褐、砖读成橄榄鳞片。
默认值改成参照图里那一档：**灰泥 P04 + 砖 B01**。⚠️ 这是默认值不是结论 —— 换档只要在
`MI_TinyGladeWall` 上改一个贴图参数；逐房差异将来应走顶点色 `Color.A` 色号 + 1D 调色板
（见「逐房差异」那条 finding），不是给每栋房造 MID。

同时核掉一条名字很像的假线索：`bricks_beige/dark/grey/light/yellow`、
`plaster_peel_low/medium/high`、`nani_solid_albedo_layer00..08`、`wall` **全是 32×32 的 UI 图标**，
不是可用的表面贴图。别再去翻第二遍。

#### 出图管线上新踩的两条（与既有那张坑表并列）

- **`always_persist_rendering_state` 不开 ⇒ 离屏捕获没有 ViewState ⇒ 关卡 PPV 里钉死的曝光
  一条都不生效。** v1 那次五张图全是一片近黑（全图均色 0.003~0.03），差异率被压到 0.00% ——
  症状长得像"改动没生效"，其实是**根本没曝光**。这条 `TinyGladeShotSkirt.py` 的坑 ⑦ 早就记过，
  但新脚本抄的是 `TinyGladeShotWallRoofSeams.py`（那张表里没有），于是重踩一次。
- **`-ExecCmds="py <脚本>"` 用 PowerShell `Start-Process -ArgumentList @(...)` 传会掉引号**，
  UE 只收到 `-ExecCmds=py`，日志里是一行光秃秃的 `Cmd: py`，脚本从没跑过、编辑器也不会退。
  要把整条命令行拼成**单个字符串**再传。

#### 本轮产出

| 文件 | 作用 |
| --- | --- |
| `Scripts/TinyGladeShotWallTexture.py` | peel / flat 两趟同机位 + 三条逐像素判据（五机位） |
| `Scripts/TinyGladeShotWallVariants.py` | 默认值候选扫描：一次启动拍完 N 组参数（`VARIANTS` 表每轮要改） |
| `Scripts/TinyGladeAnalyzeWallShots.py` | 上面那两趟 PNG 的判据（**编辑器外**跑，要全分辨率，采样格会漏掉凸出砖） |
| `Scripts/TinyGladeMakeWallMaterials.py` | 加 `PeelBias`；四个默认值改档；`prune_orphan_overrides` |

顺带清掉一条：`MI_TinyGladeWall` 上还挂着 `BrickSeed = stone_floor_2_seed`，来自被废弃的
「用 seed 图逐格选砖」那版。**孤儿覆盖在 MI 面板上看不见**（面板由母材质的参数表驱动），
只能靠脚本自己删 —— `build_wall_instance` 现在每次都按"本次建图登记过的参数名"过一遍。

#### 未做（如实列出）

- **`L_HouseGroundDemo` 里三个 house actor 的 `WallMaterial` 指的是母材质 `M_TinyGladeWall`，
  不是 `MI_TinyGladeWall`**（关卡存盘时间早于 09-03 那次建图，而建图脚本有意不存关卡）。
  今天两者渲染逐像素相同（MI 只覆盖贴图、且与母材质默认值一致），所以不是画面缺陷，
  但"手感参数在 MI 上调"这句话对这张关卡不成立。修法是打开关卡重跑
  `TinyGladeMakeWallMaterials.py` 再存盘 —— 本轮没做，因为后台进程存关卡会和用户开着的
  编辑器抢同一个包。
- **凸出砖仍然是"石头的顶盖"而不是整块石头**。`stone_floor_2` 的 seed 分格与 height 的石块
  对不齐（09-03 已实测否掉逐格选砖），纯高度阈值切出来的必然是穹顶的帽子。0.93 只是把数量
  压到读得过去，形状问题没解。
- **AO 一张没有；`UseVertexCoverage` 恒 0**（顶点色 A 还是保留位）。TG 的 `cov` 项因此永远走
  不到，`PeelBias` 是它的替身。哪天 A 通道被分配出去，这两项要一起重看。


### D6 重做：门宽 = 路在墙上截出的弦长；新增门扇（2026-09-04）

上一节把「TG 的门与道路无关」证伪之后，本节是照着 TG 的口径把 D6 改过来的实施记录。
判据脚本 `Scripts/TinyGladeShotDoorWidth.py`（画不同宽度的路、量门宽），纯函数层
`Public/CSHouseDoorRuns.h`，单测 `PCGPlugins.ComputeShaderGenerator.House.DoorRuns`。

#### 改了什么

| | 旧（等分槽 + 覆盖率二值投票） | 新（道路区间制） |
| --- | --- | --- |
| 路的连续值 | `Road >= DoorOnWeight` 当场二值化，只剩"几个采样点算路" | 沿边一次采样出**连续剖面**，端点在跨阈的相邻两点之间**线性求根** |
| 门宽 | `(槽宽 − 墩宽) × 离地收窄`，**与路无关** | **= 区间长度**，路多宽门多宽 |
| 门心 | 槽心（恒定） | 区间中心，跟着路走 |
| 过宽的路 | 只能是一个顶满槽的拱 | 切成一排拱、相邻留 `PierWidth`（`DoorMaxWidth` 定档） |
| 滞回 | 覆盖率双阈，key 里带段数 N | **区间交叠继承**，没有编号 |
| 离地收窄 | 唯一的宽度源 | 降级成一个可关的乘数（平地上恒 1） |

#### 实测（`TinyGladeShotDoorWidth.py`，同一面墙逐档加宽笔刷）

```text
笔刷半径      60    90   120   150   180   220
这面墙的拱数   1     1     2     2     2     2
总开口宽 cm   66   150   152   212   212   212
路平移 60 cm ⇒ 门心移动 70 cm（旧口径恒为 0）
```

「路只擦过一点点 ⇒ 门很窄」这条用户实测**复现了**：最窄档 66 cm，最宽档 212 cm。
`DoorMaxWidth = 160` 之后 r120 起裂成两拱 —— 这就是用户说的「路过宽时变成连续的石拱门」，
比例参照 `img/tiny-glade-ref-arcade-piers.jpg` / `tiny-glade-ref-twin-arch-pier.jpg`（两拱夹一道窄墩）。

⚠️ **`DoorMaxWidth` 的默认值必须比一面墙的可用跨度小，否则这条路永远走不到。**
260 那一版比演示房 400 深那面墙的可用长 232 还大，宽路只得到一个顶满整墙的巨拱，
拱廊那档从没出现过 —— 参数存在不等于路径被走过。

#### 新增：门扇（`DoorLeafMesh` 一族）

TG 侧：`construct_gates` 按 `segment_length` **现搭** quad 网格（`DoorMeshInProgress::add_quad`
+ `door_mesh` / `door_mesh_gap`），资产里只有把手（`wooden_gate/door_handle_circle.glb`），
**没有门扇网格**。本项目**有意不同构**：用现成的 `door`（TG `decorators/door.glb`，实测
120 × 75 × 250 cm、带 COLOR_0 无 UV）按洞宽缩放，纵横比拉过头时只缩不拉
（`DoorLeafMaxStretch`）。竖直轴与宽度轴从包围盒自判，换 y-up 资产也立得起来。
门扇高默认齐**起拱线**（`DoorLeafRise = 0`）—— 矩形门扇顶到半圆拱顶必然切角。

#### 🐛 一次真崩：门扇批量注册 → Nanite 材质审计 → 渲染 flush → 断言

`DoorMaxWidth` 调小、同一帧要建 2 个以上门扇的那一版当场崩：

```text
FStaticMeshComponentBulkReregisterContext::~ → FScene::BatchAddPrimitives
  → ShouldCreateNaniteProxy → AuditMaterials → CheckMaterialUsage(MATUSAGE_Nanite)
  → 材质缺这一位 ⇒ SetMaterialUsage 重编译 ⇒ FlushRenderingCommands()
  → 抽干任务队列，把在途的 UCSMesh::EditMeshAsync 完成回调拉进来
  → UCSMeshRenderComponent::HandleMeshChanged → MarkActorComponentForNeededEndOfFrameUpdate
  → Assertion failed: !bPostTickComponentUpdate
```

**一个门扇时不崩**（走不到批量注册那条路），所以它是"数量一多才现形"的那类。
修法两道：C++ 给门扇组件 `bDisallowNanite = true`（二十几个顶点的板子，Nanite 零收益，
直接砍掉整条审计分支）；建材质的脚本里把 `used_with_nanite` 预先勾上。
**这与"母材质没勾 `bUsedWithInstancedStaticMeshes` 会被静默换成默认材质"是同一族坑：
材质用途标记要在资产上预先备好，不能等运行时注册时现补。**

#### 🐛 另外三条静默失效（全部由判据抓到，都不报错）

- **`str(枚举).endswith("DOOR")` 匹配不上**：UE Python 的 `str()` 给的是
  `"CSOpeningType.DOOR: 0"`（带值），不是 `"CSOpeningType.DOOR"`。症状是"一档都没量到"，
  而门明明在（`GetDoorLeafCount()` 同时报 1~3）。**枚举一律用对象比较。**
- **判据在空数据下真空通过**：第一版跑完 `widths=[]` 照样打 `OK`。补了一条 ⓪：
  量到的档数不足或一档都没开门，直接红。这正是坑表里"故意破坏世界侧的对照"要防的假绿。
- **单拱宽当判据 → 切拱廊时假红**：路一宽过 `DoorMaxWidth`，单拱宽必然下降。
  判据要量**总开口宽**（这面墙上所有拱之和）。
- 出图机位两次拍到隔壁 `House_Pillar` 的墙（与卷零坑表「山墙机位拍进了隔壁房子的白墙」同型）。
  现在脚本开拍前把别的 house actor 临时藏掉。

#### 旧路已删干净

`SplitEdgeIntoSlots`（等分槽）与 `DoorPitchTarget` 全仓 0 命中，实现、声明、单测
`FCSHouseEdgeSplitTest` 一并删除（资产侧核过：BP 与关卡都没有覆盖过 `DoorPitchTarget`）。

#### 未做（如实列出）

- **拱形仍是半径 = 半宽的正半圆**，拱高与宽度**没有解耦**。TG 是
  `create_stone_arch_profile` + `ArchFunction::remap_t` 的剖面曲线，逐砖带 3 点拱高
  （`global_arch_height_vals`）由 PS 逐像素切；参考图 `tiny-glade-ref-twin-arch-pier.jpg`
  里的拱明显**扁于半圆**（跨约 200、起拱以上只升约 80）。要改得同时动
  `CSHouse_ClipKeeps` 与材质里那段 HLSL（两处必须同式，不一致不报错）。
- **段的类型只有拱，没有楣**（TG 有 `LintelSegments`）。
- **门扇仍是缩放预制件**，不是按洞宽现搭；宽度差得多时板条比例会被拉伸。
- **`DoorLeafRise` 默认 0**，拱顶那半圆是空的（读作气窗）。要顶满得先有拱形门扇网格。

### D6 续：拱高解耦 / 换回 TG 真门 / 门扇填满拱 / 四边接成环（2026-09-04 下午）

同日上午把门宽改成"路在墙上截出的弦长"之后，用户看图给了四条反馈，逐条落地。判据仍是
`Scripts/TinyGladeShotDoorWidth.py`（新增判据 ⑥）。

#### ① 拱高与洞宽解耦（`DoorMaxArchRise`，默认 70 cm）

用户："门上升过高"。半圆下**拱高恒等于半宽**，洞一宽拱就顶到很高。改成**椭圆拱**：
半轴 = (半宽, Rise)，`FCSWallOpening::ArchRise` 为 0 时退回半圆、逐位不变。

**改动小到出乎意料**，因为归一化在裁剪场里：`q = ((S−Cs)/HW, (Z−拱脚)/Rise)`，
`dot(q,q) < 1` 从正圆变椭圆 ⇒ **材质里那段 HLSL 一个字都不用改**（它只看已归一化的 q）。
"两处必须同式"那条老风险这次没咬到 —— 值得记：**把形状参数塞进归一化，判据就不必跟着改**。

新单测 `House.ArchRise`：`ArchRise=0` 逐位退回半圆、剖面顶点不许落进裁剪曲线里
（门框砖沿剖面走、洞由裁剪场切，两者不同式**没有任何报错**）、扁拱确实更早收拢、Rise 夹在洞高内。

面板上因此有两个正交旋钮：`DoorHeight` 管整个洞多高，`DoorMaxArchRise` 管拱那一段升多高。

#### ② 🐛 门扇用错了资产：`decorators/door.glb` 是交互代理盒

用户："门没有使用 TG 的模型"。核过：`door.glb` 只有 **36 顶点、120×250×75、没有 UV** ——
它是**交互/碰撞代理**，同族的 `*_collision` / `*_interaction` / `*_outline` /
`decorator_test_door` 一样。TG 的真门是 **`balcony_door_rank1/2/3`**
（源码路径 `decorator_visual/cottage_balcony_door.rs`；120 / 150 / 180 cm 宽 × 262.5 高，
带 COLOR_0 **和** UV），而且**按尺寸分档**（`DecoratorSubtype` 的 rank）。

⇒ `DoorLeafMesh` 改成 `DoorLeafMeshes` 数组：按每张网格自己的包围盒宽度排序，挑
"native 宽度不超过洞宽的最大一档"，余下差值才交给缩放 ⇒ 拉伸量天然最小。**这就是 TG 的做法。**

⚠️ 顺带解释了另一个假象：之前门扇渲染成**纯黑**，我一度当成材质问题。真因是代理盒的顶点色是 0，
`VertexColor × Tint` 自然全黑 —— **"用错资产"的症状**。换成真门之后顶点色接回去就对了。

#### ③ 门扇不是拱形网格，是**比洞更大的矩形**被墙切出剪影

用户贴了实拍 `img/tiny-glade-ref-door-in-arch.png` 并问："它看上去像圆弧形，但你放的门不是，
这是否意味着它实际上超出了拱形石墙的距离"。**是的**，三条证据：

- 门的轮廓**严丝合缝贴着拱圈石内缘**，连侧边都没有缝；
- 木板条是**竖直**的，各自终止在拱线上；
- 拱圈石在门上投了一道软阴影 ⇒ 门是**凹进去**的。

而 TG 的拱形状随洞宽 / 拱高连续变化，**一张固定网格不可能每次都对上那条曲线**
⇒ 门扇不可能是建成拱顶的。它是一块比洞更大的矩形，退在墙面之后，由墙自己的 `OpacityMask`
切出拱形剪影。⇒ **我们不需要拱形门扇网格。**

落地：`DoorLeafWidthRatio` 0.94 → **1.0**，`DoorLeafRise` 0 → **1.05**（故意越过拱顶）。
早先"门顶停在起拱线、拱顶那半圆空着"是反的，画面上是"方门塞在拱下面"。

⚠️ 同时补了一条：**门扇厚度必须留在墙里**。宽门那档 `ScaleWidth > 1`，
`balcony_door_rank*` 原生 31.6 cm 厚，乘上去比 24 cm 的墙还厚 ⇒ 会从两面探出去，
剪影那套就失效了。现在按墙厚夹住（两侧各留 2 cm）。

#### ④ 四条边接成一条闭合周界 ⇒ 转角能开门

用户："根据 4 条边组合一个环形边肯定不是难事，转角处的设计参考 TG 就行"，并给了实拍
`img/tiny-glade-ref-corner-arch-passage.png` —— **两道拱共用一根角柱，而且没有木门**。

这与卷零上一节查到的 TG 模型一致：`Rectangle2d` 是**闭合周界参数 u**
（`perimeter` / `get_pos_at_u` / `get_edge_id_at_u` / **`circular_slice`** 环形切片 /
`get_normal_at_u_non_normalized_corners` 转角给未归一化角平分线）。

落地：`FCSDoorRunParams::bClosed` + `ComputeDoors` 沿**整圈周界**采样求解，
段再按边切回去；跨角的段被切成两片，两片都打 `CSHouse_StyleCornerDoor`：

- 角柱（`CSHouseQuoin`）**留着当中墩** —— 它不是障碍，实拍里它就是那根柱；
- 转角段**不装门扇**（实拍里转角是敞开的过道）；
- 过宽切分放在**按边切完之后**做，一排拱因此不会骑在转角上。

⚠️ `CornerMargin` 从门洞求解里**退场**（还被窗的谓词用着，不是死代码）：护角本来就是
"四条独立边"模型的产物，环起来之后路自己决定开到哪儿。副作用是可用跨度变大 ——
实测同一栋房的总开口宽上限从 212 涨到 312。

⚠️ 滞回记忆改存**环参数**（`FCSDoorRunMemory::EdgeIndex` 恒 −1）。旧的逐边记忆读进来会全部
失配 ⇒ 升级那一帧滞回失效一次，与"拉尺寸跨 round 边界"是同型但只发生一次，可接受。

#### 实测

```text
笔刷半径      60    90   120   150   180   220
这面墙拱数     1     1     2     2     2     3
总开口宽 cm   66   150   152   224   272   312     （护角退场前上限是 212）
斜穿转角     edges=[0,1] 两条边各开洞（0 号边还被切成两拱），门扇数 = 0
```

#### ⑤ 拱间墩改成小石柱：柱础 + 柱身 + 柱头（2026-09-04 傍晚）

用户："两道门出现时中间会出现一个小型石柱"，并再次给出实拍 `img/tiny-glade-ref-twin-arch-pier.jpg`。
当时的工程截图差在两处：我们的墩是**一摞同样的砖**，没有柱头 / 柱础；
两道拱圈之间还夹着一块灰泥楔子，TG 那边两道拱圈直接收在柱头上。

落地（`CSHouseFrame.{h,cpp}` + `CSHouseFrame.usf`）：

- `FElement::CrossScale`：逐路**横截面缩放**（进深轴 + 墙法线轴同乘，长度轴不动），走 kernel
  里原本空着的 `R5.w`。旧数据那一格是 0，kernel 按 1 处理，逐位兼容。
- 墩那条竖直砖路拆成三段同一个 S：柱础（单砖，`CrossScale = PierCapitalScale`）、柱身（原来那条
  均匀砖路，缩短两个帽子高）、柱头（单砖）。柱头高夹在墩高的 40% 内，墩很矮时两块帽子不能把柱身挤没。
- 只有门框那条路填 `CapitalHeight`；接缝柱与角石沿用默认 0 —— 角石通高到檐口，柱头放在檐下没有意义。
- 新单测 `House.FramePierCapital`：五条砖路 / 础与头各一块放大砖 / 三段首尾相接**同一列**
  （`FramePierSingleColumn` 的不变量不许破）/ 荒谬的柱头高被夹住 / 关掉逐位退回三条。

#### ⑥ 拱廊不装木门（2026-09-04 傍晚，用户实测）

"两道门并排时只有拱没有木门" —— 木门只装在**单拱**上，一排连续拱是敞开的过道。我只在转角段
做了这条（`CSHouse_StyleCornerDoor`），拱廊漏了。补 `CSHouse_StyleArcade`：`ComputeDoors` 里一条路
被 `CSHouse_SplitRun` 切成多拱时每个子拱都打这一位；门扇看 `CSHouse_StyleNoLeafMask`
（转角 | 拱廊）任一位就不装。判据脚本加 ⑦：单拱档恰好 1 扇门，多拱档 0 扇。

#### ⑦ 砖构件外凸出墙面（2026-09-04 傍晚，用户："很多结构离墙太近了"）

用户："应该是它们的中心在墙 mesh 上，而不是它们的最远端。" 根子在 `EnsureFrameComponent`：
砖的穿墙厚度 = `FrameBrickThickness`，为 0 时取**墙厚**，而砖路走墙厚正中 ⇒ 砖的外表面与
墙面**共面**，拱圈石 / 墩 / 勒脚全贴在灰泥里一点不凸。角石反倒是对的（`QuoinInset = 0`：
一半在实体里、一半探出去）。TG 的砖本来就是墙本身、灰泥盖在外面，拱圈石天然凸出灰泥面并在
门上投影（实拍 `img/tiny-glade-ref-door-in-arch.png` 里那道软阴影）。

落地：`FrameBrickProtrude`（默认 6 cm）。自动档厚度 = 墙厚 + 2×它，**中心仍在墙厚正中**
⇒ 两面各凸一截，"砖封住洞口断口两侧"那条既有约束不破。门框砖 / 勒脚 / 角石共用一份
`BlockSize`，三家一起凸；显式给了 `FrameBrickThickness` 就照给的用。进了门框哈希，改参数会重建。
门扇仍退在墙厚正中（厚度夹在墙内），于是比拱圈石前脸低 ~8 cm —— 正是"门凹在拱里"的关系。

⚠️ 两道拱圈之间那块灰泥楔子只被柱头**盖住了下半**：它的成因是两个独立的洞各自起拱，
拱圈从各自的洞缘出发。要像 TG 那样两道拱圈**收在一点**，得让相邻两拱共享起拱点 —— 那是
"跨角连续拱曲线"同一族的活，未做。

#### 🐛 转角标记被 `ResolvePierSpans` 擦掉（判据 ⑥ 抓到）

第一次跑判据 ⑥：两条边确实都开了洞，但**门扇照装**。真因是 `ResolvePierSpans()` 开头那句

```cpp
for (FCSWallOpening& Opening : CurrentOpenings) Opening.StyleFlags = 0;
```

它在 `ComputeDoors` **之后**跑，整份清零把 `CSHouse_StyleCornerDoor` 一起擦了。
改成只清墩那两位（`&= ~(PierBefore | PierAfter)`）。

**这一族坑的形状**：`StyleFlags` 是多个生产者共用的位域，而清零写成了"整份"。
再往里加位的人都会重踩 —— 所以清零那一行现在带着注释写死了"只清自己那两位"。

单测 53 条全绿（新增 `House.ArchRise`）。


### TG 的转角处理：四条互不相干的链（2026-09-04 调研）

用户问的"TG 对转角怎么处理"。PDB 里能分出**四条**，本项目只对上了前两条。

#### 前提：TG 的墙是**一条闭合周界曲线**，本项目是四条刚性边

`utils::geometry::rectangle::Rectangle2d` 暴露的是一整套**周界参数 u** 的接口：
`perimeter` / `get_pos_at_u` / `get_edge_id_at_u` / `get_closest_u_from_pos` /
`as_curve2` / `as_curve3`，以及 **`circular_slice` / `circular_slice_segment_points`**
—— 周界切片是**环形（可绕回）**的。

⇒ **在 TG 里一段墙路径可以跨过转角**，拱因此也可以跨过转角。
配套的是 `get_normal_at_u_non_normalized_corners`：转角处的法线取两条边法线的**未归一化和**
（= 角平分线且更长），这正是沿周界外扩/挤出时做斜接（miter）需要的量。

本项目是 `CSHouse_GetEdge` 给出的**四条独立刚性边**，每条边自己的 `S` 从 0 起算，
门还被 `CornerMargin` 挡在离角 60 cm 之外 ⇒ **洞永远不可能跨角**。
这也是卷一 §4.1 那条「墙角的问题不是穿模而是 UV 岛断裂」的同一个根：
四条边各自的 UV 从 quad 局部 (0,0) 起算，纹理到角就断。

#### 四条链

| # | TG 侧 | 干什么 | 本项目 |
| --- | --- | --- | --- |
| 1 | `system_wall_constructor::utils::wall_corners::add_wall_corners` | 墙**自身**四角的砖 | ✅ `CSHouseQuoin.h`（四角竖直砖柱，与接缝柱共用 `AppendColumn`） |
| 2 | `system_clutter::inter_shape_stitches::detect_intra_shape_corners` → `IntraShapeCorners` + `ShapeIntersectionHole`（**写 `WallHoles`**）→ `stitch_bricks::{InterShapeBrickStitches, StitchBrick}` | 两个**形状相交**处：先开洞，再砌缝砖 | ✅ `CSHouseSeam.h`（洞走 clip 不挖真几何，砖是纯函数） |
| 3 | `WallCornerAttachment::{from_rectangle_corner_id, to_rectangle_corner_id, is_attached}`、`DecoratorSubtype::can_be_on_corner`、`should_offer_snap_to_corner` | 装饰件**吸附到转角**（按 corner id 记账） | ❌ 无。D12 摆件只有五家锚点，没有"转角"这一家 |
| 4 | `system_decorator::decorator_visual::cottage_corner_window::generate_cottage_corner_windows` | **转角窗**（骑在角上的窗） | ❌ 无，且**永久不做**（2026-09-05 用户裁决）。D8 的窗只认单条边，距角过紧直接不生成 |

另有 `stairs_railings_assemble::add_corner_stone_railing_piece`（楼梯栏杆的转角件），本项目楼梯没有栏杆。

⇒ **想抄第 3/4 条，前置是「墙路径的周界参数化」那一层**（同卷四 §4.2 被否时列的前置②
「墙高沿路径的曲线」是同一层设施）。在四条刚性边的模型下，转角窗与转角吸附都无处落脚。


### 挖洞策略改成 TG 的真两层：砖层 + 灰泥层（2026-09-05 用户裁决）

**用户裁决：抄 TG 的真两层。** 先前的「整块实心墙板 + 一个逐像素 clip 场」作废，
计划正文已改写（`Plan_House.md` D4「墙的两层结构：砖层 + 灰泥层」；
结论节 `:10`、D4 生成 bullet、D8「洞的实现」三处已标记被取代）。

#### 触发这次裁决的调研：驱动方逐行读了 `_nani_plaster` 与 `_wall_wall_brick_lod0`

用户问「TG 是不是墙灰用顶点色挖除、露出来的部分用 instance 砖填满」。答案是**一半对，
而且对的那一半推翻了本文卷二 §1.2 原来的一句结论**（该处已就地订正）：

- ✅ **灰泥确实是逐顶点标量 + PS discard 挖掉的，而且这是 TG 唯一可能的做法。**
  `_nani_plaster...vs_main L129-146` 把 `gl_VertexIndex` 拆成 `(row = idx>>16, col = idx&0xFFFF)`，
  偏移 `48 * (col + row * row_stride)` —— **规则栅格，没有索引缓冲、没有可跳过的四边形**。
  PS 第一句 `if (clamp(in_var_C6,0,1) < 0.1) discard;`，`C6` 是从灰泥自己的顶点缓冲读的逐顶点标量。
- ❌ **砖不是"填"进去的，砖是恒在的底层。** `system_wall_constructor` 沿墙曲线累积 `Vec<MyBrick>`
  （一栋小屋数千块），灰泥是盖在砖墙外面的另一张网格。剥落/洞让灰泥消失，砖自然露出来。
  ⇒ TG 是**「砖墙是底，灰泥是面漆」**，不是「灰泥是墙，挖开补砖」。
- 🔑 **砖层也有逐像素裁剪** —— 这条决定了两层方案**不必丢弃现有设施**：
  `_wall_wall_brick_lod0...ps_main L145-172` 的两处 `discard` 是同一判据的正反两支
  （`普通砖 && world_y < 拱高 → discard` / `拱圈石(flags&8) && world_y > 拱高 → discard`），
  `拱高` 由每砖 `global_arch_height_vals` 三点插值。**`FCSOpeningClipField` 的对位物就在这里**：
  两层之后它从「墙板」搬到「每块砖」。而且本项目的场是二维、上下都有界 ⇒ 矩形窗与圆窗仍能精确裁，
  TG 那条一维下界做不到。
- 🔑 **TG 的洞几何就是二维 AABB，没有斜切轴**【确凿，PDB 泛型实参 `pdb_symbols.txt:86452`】：
  `generate_plaster_mesh_and_entities<Map<Filter<Iter<tuple$<utils::geometry::aabb::Aabb2,
  HoleType, HoleOrigin> > > > >`。坐实了逆向报告 L465 那条推断的前半、**证伪了后半** ——
  TG 侧没有 `AxisUS` / `Skew` 的对位物。

#### 落到本项目

| 层 | 内容 | 洞怎么消失 |
| --- | --- | --- |
| A · 砖层 | `brick` 实例**铺满整墙** | **三级**：删实例 → 把剩下的砖挪到贴紧洞缘 → 逐像素裁兜底（见下） |
| B · 灰泥层 | 规则四边形栅格，逐顶点覆盖度 | 洞内顶点覆盖度写 0（边缘衰减）⇒ PS `cov < 阈值 → discard` |

#### 订正（同日稍晚，用户提问触发）：TG 的洞缘不是"全靠逐像素裁"

用户回忆「TG 会删掉一部分砖的 instance，并且移动剩下砖的顶点让它们贴紧窗户」。**核实成立**，
而且比本节初稿写的"逐砖 clip 细裁"更准 —— 逐像素裁只是第四级兜底：

四级（① 删实例 / ② 水平贴合 / ③ 垂直贴合 `flags&32` / ④ 逐像素兜底）的表格、TG 实证与「两条边界」只在计划 [D4「墙的两层结构」](Plan_House.md#墙的两层结构砖层--灰泥层挖洞策略2026-09-05-用户裁决) 维护；④ 已于 2026-09-06 作废。这一轮的额外收获是坐实了 `[分析]` §1.7 对 `flags&32`（拱压扁 + 免拱裁剪）的语义推测 —— 它就是③。

**不变**：`FCSWallOpening` 与 `CSHouse_ComputeClipField` 仍是唯一真源；谓词 `CSHouse_QueryOpening`
**零改动**；C1（一维 S 区间、永久放弃门上开窗）与同日的「不做转角窗」继续有效。
⚠️ C1 选甲的**理由**（面板只带一个 clip 场）在两层之后不再成立 —— 但 C1 是用户裁决不是权宜之计，
**不据此翻案**。

**退役**：`AddPanel` 单游标扫掠、窗台实心盒（窗台就是洞下方照常摆的砖）、
`M_TinyGladeWall` 的 `PeelBias`（2026-09-04 为顶替缺失的覆盖度而加，真覆盖度来了它就没用了）。
`CSHouseFrame` 的门框砖 / 拱圈石**并入砖层**，不再是贴在实心盒外面的装饰。

**最大风险**：拖拽期每帧重建从「千级三角一次上传」涨到「千级实例 + 一张栅格网格」。
零阻塞纪律与哈希短路必须原样守住。

**分期**：P1 砖层（**含①②③④四级**；验收：洞缘与改动前**逐像素相同** + 砖数在预算内 + 零阻塞断言不退化）→
P2 灰泥层（验收：剥落沿高度的渐变读得出来，复用 `TinyGladeAnalyzeWallShots.py` 三条判据）→
P3 `above_to_below` / `below_to_above` 过渡与「哪儿铺灰泥」的片边界。


### 附属物持有 mesh、锚点是权威（2026-09-06 用户裁决）—— D8 交互层重定向

**起因**：09-05 晚两轮追问「改房子尺寸时窗标记会怎样」与「TG 怎么解决」，追出来的东西直接触发了这次裁决。

#### 现状（源码追出来的）

- 标记**从不 attach**（`Host` 只是弱指针；对比拉尺寸抓手是 `AttachToActor` 的），**从不在房子变化时重解析**
  （`ResolveHostAndRegister` 全仓调用点只有标记自己的生命周期钩子和单测）。
- 诉求存墙局部 `(EdgeIndex, CenterS, SillZ)`、标记存世界变换，两边分家。`PushEdge(e)` 挪 e 侧两个角 = 第 e 与
  第 (e+1)%4 条边的 S 原点 ⇒ **推第 e 条边，e 与 e+1 上的窗错位 Δ，另外两条完全正确**（四条边逐一推算验证：
  e 上的窗随墙平移、e+1 上的窗沿没动的墙滑 Δ）。移动 / 旋转整栋房子 ⇒ 四面墙全错。改 `WallHeight` ⇒ `AboveEave` 静默拒。
- **回执过期**：`bCausesCut` / `LastReject` 只在 `ResolveHostAndRegister` 里写，房子拒了标记还说"切了"。
- **自愈方向反了**：`MarkerWindows` transient ⇒ 重开关卡按标记的陈旧世界位置重射线 ⇒ **洞跑去追标记**，用户看到窗又挪一次。
- 计划 D8「吸附回位」整节零实现：`LastAcceptedWorld` / `SnappedWorld` / `SetActorTransform` 一个都搜不到。

#### TG 怎么做（全部 `[PDB]` 实证）

- **变换是每帧派生的**：`resources::walls::cached_decorator_transforms::CachedDecoratorTransforms::{insert, position,
  try_position, transform, try_transform, swap_buffers_and_clear}` + `Local<CachedDecoratorTransformStorage>` 双缓冲；
  产出系统（`pdb_symbols.txt:9033`）读 `PublicWalls` + `TerrainHeightsData` + `DecoratorStorage` 写缓存。**没有可以过期的东西。**
- **锚点是序列化权威**：`resources::walls::wall_attachment::{WallAttachmentAnchor, WallAttachmentSide, WallCornerAttachment}`
  带 serde；`WallCornerAttachment::{from_rectangle_corner_id, to_rectangle_corner_id, is_attached}` ⇒ 至少角挂载用的是
  **拓扑角编号**而不是坐标。
- **每种改法一个系统**：`decorator_on_rectangle_edited`（= 我们的 `PushEdge`）/ `decorator_on_move_shape` /
  `decorator_on_wall_height_changed` / `decorator_on_freehand_wall_edited` / `move_decorators_following_anchors` /
  `cull_oob_decorators` / 事件 `ReanchorDecorators`。
- **裁决每帧重判**：`validate_blueprints` → `DecoratorBlueprints`，配 `clear_blueprints` / `copy_blueprints_to_prev` /
  `iter_maybe_modified` + `PrevDecoratorBlueprints`。被拒的蓝图**不实例化** ⇒ 画面消失、存储保留。
- **窗的 mesh 归 decorator**：`instantiate_blueprints` 出 `window_cottage_*` 实例，墙只 `trim_rows`。

#### 用户裁决（三条，已写入计划 D8）

1. **附属物自己持有 mesh**（预制 StaticMesh），房子只"适配"——只挖洞，不再砌窗框砖 / 窗台盒。
   理由：布尔退场后"cutter 必须在房子网格里"这个约束没了。**比原口径更贴近 TG。**
2. **位置存储与 TG 一致**：锚点 `FCSWallAnchor` 是唯一序列化权威，actor 变换派生；每次重建后房子把标记搬到
   锚点所在处（`SnapToAnchor`）。拖 gizmo 反向：世界 → 射线 → 锚点 → 派生回变换（吸附）。
3. **选中即可拖**：`UStaticMeshComponent` 自带编辑器 hit-proxy，点得到。

#### 随之而来（驱动方推导，已写入计划 D8，可否决）

- 锚点用**角相对** `(EdgeIndex, CornerSide, DistFromCorner, SillZ)` 而非绝对 `CenterS`（理由见计划 D8「锚点」）。
  TG 直墙锚点字段 PDB 拿不到 ⇒ **卷二 U4 重新有下游**，但不阻塞——按性质对齐即可。
- 标记 **`AttachToActor`** + 写相对变换 ⇒ 整栋移动 / 旋转 / 落座由场景图带走，只有 footprint / 墙高变化要通知。
- **被拒 ⇒ 隐藏 mesh，标记留着**（对位 `validate_blueprints` 不实例化）。
- **两层 P1 收窄**：④ 作废（见下），洞缘全靠 ①②③ 的几何贴合。
- `CSHouseFrame` 的窗台砖分支（`bSill`）随之**退役**；门樘 / 拱圈石不受影响。**2026-09-10 已删**：
  `FPath::bSill` / `SillLen`、`CSHouse_SillMinZ` 与 usf 第四段一并移除，窗周围不走砖头补全。
- **卷二 A9 玻璃母材质提前变成必需**：窗有 mesh 就得有材质，`M_TG_Glass` 是 unlit 且只许 gallery 用（裁决六）。
- 补 `PostEditUndo` ⇒ 顺手关掉 09-05 列的"移动-撤销后登记不刷新"疑点。
- 卷二 W2「窗框资产引用放标记 actor 上、由房子另开实例通路」→ 后半作废：**标记自己就是实例通路**。

#### 未决 / 待实测

- 编辑器里拖动**父** actor 时，attach 的子 actor 是否也收到 `PostEditMove` —— 决定"房子被拖"会不会被误当"标记被拖"。
  两种结果都无害（射线命中同一处），但要知道。

#### 已落地并**实跑过**（2026-09-06）：构建 Succeeded、单测 **110 绿 0 红**、回归 **passed=228 failed=3**（剩下三条是门/拱段的既存基线红，见下）

| 件 | 位置 |
| --- | --- |
| `FCSWallAnchor` + `CSHouse_MakeWallAnchor` / `CSHouse_AnchorS` / `CSHouse_AnchorToLocal` | `CSHouseProfile.h`（纯函数，无 world） |
| `ACSHouseActor::AnchorToWorld` | 锚点 → 世界，`BlueprintPure`；构建空间的合成只此一处 |
| 标记持 `MeshComponent` + `Anchor` / `LastAcceptedAnchor` + `SnapToAnchor` / `ApplyHostVerdict` / `RegisterAnchor` | `CSHouseFeatureMarker.{h,cpp}` |
| `AttachToActor` 到宿主 | 整栋移动 / 旋转 / 落座由场景图带走，零同步代码 |
| 加载按锚点复原（宿主由 attach 关系给出），**不重新射线** | `PostRegisterAllComponents` |
| `PostEditUndo` | 撤销后按锚点重登记 + 吸附 |
| 裁决回推 + 吸附 | `FCSMarkerWindow::Marker` 反引 + `CurrentFeatureVerdicts` + `NotifyMarkersRebuilt()` |
| 尺寸从网格包围盒取 | `ACSWindowMarker::GetDemandSize`（`bAutoSizeFromMesh`，没挂网格退回手填值） |
| **默认窗框资产已接** | `decorators_window_cottage_1x1`（78 × 17 × 160、32 三角）+ 网格组件 −90° yaw（资产进深在 Y、本类约定 +X 是墙内法线）；材质随资产 `MI_window_colors_layer00`（`M_TG_Texture` 实例、DefaultLit + Opaque）**零工作量** |
| 新单测 `House.WallAnchor` | 近角选择 / 往返 / **拉尺寸后世界位置不动**（含"绝对弧长会滑 100"的反证）/ 夹取 / 不动点 |
| `bAnchorPlaced` / `bHasResolvedOnce` 两位 | 挡 spawn 期误吸附，见下面那个坑 |

⚠️ **`House.WindowMarker` 有一条判据被翻面**：原来断言"推到墙角 ⇒ 窗洞掉到 0"，
现在松手被拒会**弹回最后一个被答应的位置**（计划 D8「回位规则」= TG `DecoratorBackup`），
所以窗洞留着。"被拒的诉求不撤登记"那一条没变，改由新加的"从未被接受过的标记"那一段来钉。
同段还新增：吸附到墙面、attach 到宿主、`PushEdge` 之后标记跟着锚点走。

⚠️ **窗框砖 / 窗台盒本轮没退役**（计划 D8 的落地出入表已记）：要与接上预制窗框资产同一轮做，
否则窗只剩一个光洞。

#### P1 起步：砖层的①②已落地（2026-09-06，默认关）

**关键复用**：砖层 = **一摞包边带**。`CSHouseTrim::BuildBand` 本来就是"沿四条边铺一行砖、按洞把
行切断"，从房底摞到檐口就是整面砖墙 —— 洞缘四级里的①②因此**一行新算法都没写**：

| 级 | 谁做的 | 说明 |
| --- | --- | --- |
| ① 删实例 | `CSHouseTrim::SplitEdge` | 边的 S 区间减去洞在这一行上的**剪影**（见下）。判据带 Z ⇒ 高窗不切勒脚、落地门不切压顶 |
| ② 水平贴合 | `CSHouseFrame::AppendFlatRun` 的 `SolveRun` | 解一个铺装缩放让整数块砖**正好填满**切出来的段 ⇒ 洞两侧那列砖天然抵住洞缘 |
| ③ 垂直贴合 | **未做**，在 GPU 侧 | 只服务门拱（窗的洞缘由附属物的预制框盖住） |
| ④ 逐像素兜底 | **未做**，在 GPU 侧 | |

落地件：`Public/CSHouseBrickWall.h`（`FCourses` / `PlanCourses` / `EstimateBricks` / `BuildWall`，
纯函数无 world）+ `ACSHouseActor::BuildBrickWallBricks` 家族 + `EPathFamily::BrickWall` 盐
（**它后面 65536 个值都归它** —— 盐 = `BrickWall + 层号`，好让每层随机序列不同）+
新单测 `House.BrickWall`（分层铺满不重不漏 / 上界成立 / 洞处无砖 / 洞缘贴合 / 容量截断）。

⚠️ **`bBrickWallEnabled` 默认关**，且开了也**不是**终局形态：两层是"砖底 + 灰泥面"，而灰泥那半
还没有独立网格（仍是墙板 + 材质假面），现在开 = 在墙板外面再糊一层砖，只用来对观感与量预算。
计划 D4 的验收门要求"洞缘与改动前**逐像素相同**"，那本来就得两条路并存才比得了 ——
**这个开关是验收门的一部分，不是临时脚手架**。

#### ①从「包围盒」升级成「剪影」——拱形自己从砖里长出来（2026-09-06）

初版的①按洞的**包围盒**裁行，于是拱洞被切成一个**矩形缺口**：拱顶两侧本该有砖的地方全空了。
判据换成"这一行上洞**真正**有多宽"：

- `CSHouse_OpeningHalfWidthAtZ(Opening, Z)` —— `CSHouse_ClipKeeps` 的逆解。拱脚以下满宽、
  以上按椭圆 `HW·√(1−q_y²)` 收窄、洞顶归零；矩形与圆各自的闭式解同在一处。
- `CSHouse_OpeningSpanForBand(Opening, LowZ, HighZ, …)` —— 取带内**最宽**的高度（三个采样点：
  两端 + `RefZ`，覆盖"单调"与"中间最宽"两类形状）。少取一点就会在洞里留下半块砖。
- `CSHouseTrim::BlockedSpan` 换成调它，`SplitEdge` 随之改。

⚠️ **两处边界，各自都会静默出错**：
- 拱的 clip 场在**拱脚以下无下界**（那是有意的，窗台那截由实心盒承担）。只问 clip 场的话，
  一扇 `Z0 = 90` 的拱窗会一路裁到地面 —— 画面上只是"墙脚少了一片砖"，没有断言会红。
  所以先用洞自己的 `[Z0, Z1]` 卡一道。
- 判据取 **`<=` 不是 `<`**：`|q| == 1` 恰好在洞缘上（矩形洞的 `Z == Z0` 就是）。闭区间把洞算大
  一丝丝 ⇒ **多删一块砖而不是留一块砖在洞里**，朝安全的方向错。

**对原有两条包边带逐位无影响**（实测回归一条不变）：勒脚落在拱脚以下 ⇒ 剪影就是满宽；
压顶够不着洞 ⇒ 照旧不挡。演示房砖层 **885 → 906 块**，多出来的正是拱肩上那些砖。

单测 `House.BrickWall` 新增一段拱洞：半宽随高度的四个特征点、抬起的拱窗不裁到自己窗台以下、
以及**逐层被挡跨度单调不增且真的收窄过**（只断言"不变宽"是句空话 —— 矩形缺口也满足）。

#### ③ 已落地、④ 作废：洞缘全靠**几何贴合**（2026-09-06 用户裁决）

**用户裁决原话口径："砖头适配窗子直接改缩放就行，不需要 clip。"** ⇒ **④ 逐像素裁整个不做**，
③ 对窗与门拱都开。这条同时消掉了④的**结构性阻塞**：逐像素裁要把 clip 场喂进材质，而材质只看得到
`PerInstanceRandom` 一个标量、那一格已被 `CullBelowZ` 的负值哨兵占着 —— TG 是靠自定义顶点工厂
（96 B/实例）解决的，我们要走这条就得扩 `UCSGpuInstancedMeshComponent` 的实例布局。**现在不必了。**

**③ 怎么做的**：①按剪影裁行时切口取"带内最宽的那个高度"，拱圈往上收的那一头于是留了条缝。
把端头那块砖**剪切**成上宽下窄 —— 顶边朝洞多推 `Shear`、底边不动（底边正落在切口上，动了就把砖
推进洞里）：

```hlsl
const float3 AlongS = -normalize(ScaledY);   // 平顶段 +Y 沿 −S（见 CSHouseFrame.usf 文件头）
ScaledX     += AlongS * Shear;               // 顶 +Shear/2、底 −Shear/2
LocalOrigin += AlongS * (Shear * 0.5f);      // 再推回来 ⇒ 底不动、顶 +Shear
```

**逐实例格式一个字节都没加** —— packed 行的前三行本来就是三个任意向量，倾斜高度轴本身就是一次
剪切。剪切量走逐路常量 **R6 的两个空位**（`ShearAtS0` / `ShearAtS1`），CPU 侧由
`CSHouse_OpeningTopShear` 从剪影算出（= 带内最宽半宽 − 带顶半宽）。剔除球同步涨 `abs(Shear)`，
少涨就会在视锥边缘闪掉且只在斜看时出现。

⚠️ 两处如实的限制（都写在代码注释里）：
- **只处理往上收窄的洞**（拱；矩形恒 0）。往上变宽的（正圆下半）不剪切，保持①的保守阶梯 ——
  剪切是**平行四边形**，一个自由度救不了上下两头都要动的梯形，硬凑会把砖推进洞里。
- **一条路只有一块砖、两端又都挨着洞时只应用较大的那个**，同样理由。

⚠️ **残留（已知、有意接受）**：`FrameBrickBloat = 1.1` 让砖比排布槽长 10%，端头那块因此仍会
**戳进洞里约 1.3 cm**。窗那边由附属物的预制框翻边盖住，门拱那边由门框砖盖住 —— 这正是④本来
要清的东西，裁决之后交给盖缝件。

⚠️ **P1 验收门随之改口径**：原写"洞缘与改动前**逐像素相同**"，那是以④存在为前提的。
④ 作废后洞缘由几何决定，两条路本来就不可能逐像素相同 —— 验收改成"砖不越过洞缘（bloat 那 1.3 cm
除外）+ 砖数在预算内 + 零阻塞不退化"。

#### ✅ 容量已解（2026-09-06 用户裁决：上限拉到 65536）

单测实测：6 × 4 m、檐高 3 m、26 cm 砖 / 20 cm 层高 ⇒ **砖层上界 1110 块**
（周长 1904 / 26 = 74 列 × 15 层）。而 `FrameReserveCapacity` 默认 **512**，还是
**门框 / 接缝 / 角石 / 包边 / 砖层五家共用**的那一份 —— 直接开就是一面砌到一半的墙。

**修法：不让用户手算。** 新增 `ACSHouseActor::EffectiveFrameCapacity()`，七处调用点全部改走它：

```cpp
const int32 Authored = FMath::Max(FrameReserveCapacity, 64);
if (!bBrickWallEnabled) return FMath::Min(Authored, 65536);
// 砖层的量级是 footprint 的函数 ⇒ 自己按上界加够，authored 那份原样留给其余四家
return FMath::Clamp(Authored + GetBrickWallBrickBudget(), 64, 65536);
```

演示房实测（`House_Road` 600 × 400 × 300）：

| | 容量 | 砖总数 | 其中砖层 | 层数 |
| --- | --- | --- | --- | --- |
| 砖层关（基线） | 512 | 163 | 0 | 0 |
| 砖层开 | **1622**（512 + 1110） | 1048 | **885** | 15 |

885 < 1110 是对的：洞吃掉一些、不足半块砖的碎段直接不出。**没有截断。**

⚠️ 两条一起记住：
- **只截断不扩容**。`FrameReserveCapacity` 原注释末句"Growth still happens…"**是错的**，已订正 ——
  `AppendFlatRun` 里那句 `Count = Min(Count, MaxBricks - Cursor)` 就是全部。
- **容量是注册期一次付清的**，所以**把房子拉大到超出注册时算出的量，砖层会被截断而不是扩容**。
  新增的两条 Warning（单家 / 五家合计）是唯一提示；`RebuildFrame` 那条会把砖层的数一并报出来。

#### 窗退出框砖产线，门留着（2026-09-06） — 正文已并入 [House_Window.md](House_Window.md#窗退出框砖产线门留着2026-09-06)

#### 三件网格 + 蓝图分层已落地（2026-09-06 用户裁决） — 正文已并入 [House_Window.md](House_Window.md#三件网格--蓝图分层已落地2026-09-06-用户裁决)

#### 🐛 spawn 期的误吸附会**自我固化**（本轮被 demo 回归抓住，已修）

第一次跑回归，`demo_house_window` 的标记那一段四条全红：`markers=0`、`reject=SILL_TOO_LOW`、
`host=House_Pillar` —— 标记咬上了 **11 m 外的另一栋房**。逐步探针（位置 / 朝向 / 宿主 / attach
逐步打印）读出来的因果链是**两个坑叠在一起**：

1. **`GEditor->AddActor` 先落位、后应用旋转**，而它在 spawn 之后立刻补的那次
   `PostEditMove(bFinished=true)`（基类纪律 ③ 把它降级成非最终裁决，但**照样会去打射线**）
   正落在两者之间 —— 那一刻朝向还是默认的 **+X**，射线于是打中背后的 `House_Pillar`
   （边 3、S=176、局部 Z=0 ⇒ `SILL_TOO_LOW`，逐个数都对得上）。
   **这一条是既有行为、本身无害**：下一次解析朝向就对了，会自己纠正。
2. **致命的是本轮新增的吸附**：`PostRegisterAllComponents` 会跑**不止一次**，第二次跑到时锚点
   已被①填上、attach 也挂上了 ⇒ 「锚点有效且已 attach ⇒ 这是从存档复原」这个判据当场失灵，
   标记被 `SnapToAnchor` **搬到那面错墙上**（实测 `(1500,1600,150)` → `(1895,1600,205)`）。
   从此每次重解析都从错墙出发、再次确认错墙 —— **错误自我固化**。

**修法（用户裁决 2026-09-06：不许加「审批完 actor」这种状态位）**：不打补丁挡症状，直接修根因 ——
`PostRegisterAllComponents` **只服务「从存档加载」**（判据 `HasAnyFlags(RF_WasLoaded)`），
spawn 出来的一概不碰；spawn 的正确入口是 `PostEditMove`，那时变换已经应用完。
一度加过的 `bAnchorPlaced` / `bHasResolvedOnce` 两位标志**已全部删除**。

**用户给的模型（照此实现）**：*窗子 actor 是拖上去的，**只有这段期间需要打射线**；一旦 attach
到宿主，它就切换成「按锚点记录位置」的模式。* 射线因此只出现在 `OnHandleDrag`（拖拽事件的
唯一执行面），**加载 / 房子重建 / 撤销三条路一律只读锚点，不打射线**。

⚠️ **教训（推广到所有会回写 transform 的抓手）**：程序回写 actor 变换**只在最终裁决时做**。
中途回写会把一次本来能自愈的误判变成不可逆的锁定 —— 因为下一次判据的输入正是你刚写进去的那个错值。

⚠️ **合成事件本身也已根除（同日稍晚）**：一度以为"让基类忽略那次合成事件会动到拉尺寸抓手"，
**核实是错的** —— `ACSHouseResizeHandleActor` 是 `UCLASS(NotBlueprintable, NotPlaceable)`，
且只由房子在 `EnterResizeMode` 里用 `World->SpawnActor` 生成（`CSHouseActor.cpp`），
**从来收不到 `AddActor` 的合成事件**；纪律 ③ 实际上只有窗标记会走到。
于是基类 `PostEditMove` 直接从"降级"改成"**跳过**"：

```cpp
const bool bSynthetic = bFinished && !bHasBeenPlaced;
bHasBeenPlaced = true;
if (bSynthetic) return;        // 旋转还没应用，拿它解析会咬错宿主
HandleDrag(bFinished);
```

跳过是安全的：真正的放置一定还会来事件 —— 拖入视口时后续的悬停移动会发 `PostEditMove(false)`，
无头脚本则显式调 `HandleDrag`。探针实测：`SPAWN` 那一行现在是 `host=None attach=None edge=-1`，
误 latch 彻底消失；`AFTER-RESOLVE` 仍是 House_Road / cut=True / `markers=1 windows=2`。

#### 回归的既存基线红（**不是**本轮造成的）

`passed=228 failed=3`，三条全在门/拱段，与标记这条路无关：
`road across the house opens 6 arches doors=2` / `frame brick count is in the right ballpark bricks=144` /
`adjacent arches leave pier spans piers=0`。⚠️ 与记忆里那条"两条失败 + 提前中止"的旧基线**对不上**
（现在是三条、且不再提前中止）——本轮没有去查它们的成因，只确认了修改前后这三条一模一样。
- 预制框翻边宽度是否盖得住④逐像素切口的锯齿 —— TG 的 `window_cottage_1x1` 是 78×17×160 的框板，对 26 cm 砖应当够，P1 出图验。


### 验收门

```bash
# 构建（编辑器必须没在运行，Live Coding 会挡住链接）
"D:/UE-SourceCode-5.7.4/Engine/Build/BatchFiles/Build.bat" UETest574Editor Win64 Development \
  -Project="D:/MyProject/UnrealProject/UETest574/UETest574.uproject" -WaitMutex

# 单测：2026-08-31 岩壳体积那一轮实测 Result={Success} × 103、Result={Fail…} × 0
"D:/UE-SourceCode-5.7.4/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <uproject> \
  -ExecCmds="Automation RunTests PCGPlugins; Quit" -unattended -nopause -nosplash -abslog=<独立日志>

# 演示回归（S1 加了 demo_gpu_stairs 一节）：同轮实测 passed=225 failed=0、REGRESS OK
# ⚠️ -ExecutePythonScript 的相对路径是相对 **Engine/Binaries/Win64** 解析的，不是项目目录 ——
#    写相对路径会得到 "Could not load Python file"（实测）。给绝对路径。
"D:/UE-SourceCode-5.7.4/Engine/Binaries/Win64/UnrealEditor-Cmd.exe" <uproject> \
  -ExecutePythonScript="D:/MyProject/UnrealProject/UETest574/Plugins/PCGPlugins/Scripts/TinyGladeDemoRegression.py" \
  -unattended -nopause -nosplash -abslog=<独立日志>
```

⚠️ 上面两条的 **`-abslog=<路径>` 实测不写文件**（`-nosplash -unattended` 下试过绝对路径、
目录已存在）。要拿到日志就 `-stdout -FullStdOutLogOutput` 再把 stdout 重定向到文件。

按**日志断言**判定（`REGRESS OK` / `Result={Success}` 计数），**不信退出码**。

### 已知潜伏问题（不紧急，但别忘）

`ACSHouseActor::BuildFramePlan` 用 `GetBuildTransform()`（**只取 yaw**）把控制点烘成世界坐标，
却用 `GetActorTransform().ToInverseMatrixWithScale()`（**完整变换**）映射回组件空间。
今天无害——演示房子没有 pitch/roll、缩放为 1；**一旦有房子被旋转或缩放就会错位**。

#### ✅ 拉尺寸把「零阻塞」纪律打穿 —— 已修（2026-08-30）

**零阻塞纪律的三条断言，没有一条改过 `FootprintSize`** —— 驱动方核过，
`Scripts/TinyGladeDemoRegression.py` 的三处 `get_blocking_flush_count()` 断言分别测
「10 次平移重求值」「12 dab 一笔」「12 dab 一笔 + GPU 石阶」（`:126,144,361`）。
纯平移确实 0 flush。**但拉尺寸从没被测过，而它恰好是最糟的那条路。**

最狠的一个 flush 源是**门框砖 handover，且是上一轮引入的**：
`CSHouseActor.cpp:909-911` 把 `LocalBounds` 直接从 `FootprintSize` / `WallHeight` 算出来，
而 `bNeedHandover` 的包围盒阈值是 **1 cm** ⇒ **拖动中每一帧都重走
`SetInstanceSourceGPU`**，其内部有 `SetStreamLayoutSync` / `ResizeStreamsSync` /
`EnsureCapacitySync` + 无条件 `EditMeshSync`，最多 **4 次同步 flush/帧**。

另外两处：房体与柱的 `EnsureCapacitySync` **完全无预留**；
`CSGroundShaperSteps.cpp:84` 有一个**不计数的裸 `FlushRenderingCommands()`**
（计数器只在 `CSMesh.cpp:24` 自增），所以断言看不见它。

##### 硬证据（先让断言报红再修）

新断言第一次跑就红：**连续 12 帧改 `FootprintSize`，有门那栋 38 次阻塞 flush、带柱那栋 36 次**
（`passed=49 failed=2 / REGRESS FAILED`）。同一轮里旧的三条断言仍是 0 ——
证明它们确实测不到拉尺寸这条路。

##### ⚠️ 根因与本节原先的预测**不同**（三处订正）

1. **代价是 3 次/帧而不是「最多 4 次」，构成也不同**：实际是
   `ReleaseSync` + `AllocateSync` + `EditMeshSync`；`SetStreamLayoutSync` / `ResizeStreamsSync`
   **一次都没刷新**（它们会早退）。
2. **真正的放大器本节完全没提**：`set_editor_property` → `PostEditChangeProperty` →
   `RerunConstructionScripts` 会**卸载全部组件**，于是 `EnsureFrameComponent` 在**未注册状态下**
   调 `SetInstanceSourceGPU`，撞进 `RebuildGpuMesh` 的 `!IsRegistered()` 分支 ——
   **那个分支的注释说自己在"省一次渲染往返"，实际却调了 `ReleaseGpuMesh()`**（本身就是一次阻塞刷新），
   还把常驻缓冲整套扔掉，逼得重注册时 `AllocateSync` + `EditMeshSync` 从头再来。
3. **不是容量问题**：`FrameReserveCapacity=512` + `ReserveCapacity` 早就在且工作正常，
   失稳的只有**包围盒**。本节原先写的「门框砖 handover 是上一轮引入的、问题在容量」这半句是错的。
   另外「柱的 `EnsureCapacitySync` 无预留」属实，但柱网格顶点数在柱数不变时本来就是常数 ——
   **它那 0 次是运气不是设计**（已补预留）。

##### 修法

① 新增 `UCSMesh::CountedBlockingFlush()` 作为**阻塞刷新的唯一计数入口**，
`CSGroundShaperSteps.cpp` / `CSGroundStairs.cpp` 的裸 flush 全部改走它。
② 未注册期只置 `bGpuMeshDirty`，`OnRegister` 在建代理**之前**补重建。
③ 交接包围盒改成「留 25% 余量 → 对齐 200 cm → 只涨不缩、全量重建时收紧」（`CSHouse_QuantizeUp`），
房体与柱的 `EnsureCapacitySync` 改用 `CSHouse_ReserveCount`（×1.5 对齐 4096）。

画面正确性双证：几何断言 `dragged=208 rebuilt=208` 三角形、`bricks=152/152 openings=8/8`；
拖动预览与完整重建的同机位逐像素差异只有 131/2,520,000 个通道样本超过 8
（0.005%，光照收敛噪声，无几何差异）。

##### 剩余阻塞源（诚实列出，别当成"已经全清了"）

- 仍是**不计数**的裸 flush（都不在这四条断言覆盖的房子/地面交互路径上，但计数器看不见它们）：
  `ComputeShaderBasicFunction.cpp`(10)、`MeshGeneratorLandscapeCapture.cpp`(5)、
  `ComputeShaderMeshGenerator.cpp`(4)、`ComputeShaderMeshBoolean.cpp`(3)、
  `ComputeShaderMeshFill.cpp`(2)、`CSGpuMeshComponent.cpp`(2)、
  `StaticMeshRenderDataPointSampler.cpp`(2)、`ComputeShaderCliffGenerate.cpp`(1)、
  `CSPointBrushActor.cpp`(1)、`CSMeshOps.cpp`(1，`ComputeWorldBoundsSync`)。
- **零只在预留额度之内成立**：一段把房子涨大超过约 25% 会越过一次包围盒台阶；
  顶点数超过 1.5×/4096 台阶付一次 `EnsureCapacitySync`；砖数超过 `FrameReserveCapacity=512`
  付一次 `CSShaperSteps::EnsureCapacity`。都是**一次性、非逐帧**。
- 房体/柱容量现在**只涨不缩**（没人对它们调 `ShrinkCapacitySync`）。
- `RebuildGpuMesh` 真跑起来时尾部那次 `EditMeshSync` 仍是**无条件**的 ——
  要消掉它需要记一份「上传签名」，超出「容量与包围盒稳态化」的范围，本轮没做。

#### 离屏 SceneCapture 画不出 GPU 实例（真因**仍然开放**）

⚠️ **表述已收窄（2026-08-30 第二轮）**：**不是"实例路画不出来"** ——
同一个组件类在 `L_HouseGroundDemo` 里的 **114 块门框砖，四个机位全画对了**（见 `lit_*` 那组图），
只有 `L_TerrainOpsDemo` 的 216 级石阶复现。所以问题在那一关/那个 actor，不在实例路本身。

原症状：离屏 `SceneCapture2D` 无论相机指向哪里，都只画出固定世界位置的约 15 个块，
而 readback 证明 216 个实例存在且原点正常。已排除：视锥剔除、LOD、容量、抓拍节奏、移动/驾驶视口、`bGpuFrustumCulling`、`bGpuLODSelection`（开/关出图逐字节相同）。

⚠️ **不要采信"`bCulledThisFamily` 跨族 sticky"这个归因** —— 驱动方核过，该标志在
`PreRenderViewFamily_RenderThread`（`CSGpuInstancedMeshSceneProxy.cpp:174-177`）里是**每族重置**的，
与该说法矛盾。可见实例缓冲确实是**逐 proxy 而非逐视图**的（`:189-191` 注释原文），
方向大概率对，但具体走岔在哪还没定位。

##### 嫌疑清单（每条都标注了是"已排除"还是"没查过"）

**已被实测/源码推翻，别再走**：
- 「`bCulledThisFamily` 跨族 sticky」—— 该标志在 `PreRenderViewFamily_RenderThread` 里**每族重置**。
- 「`FCSGpuInstancedCullViewExtension` 没 override `IsActiveThisFrame`」——
  **基类默认实现就是 `return true`**（`SceneViewExtension.h:256`），SceneCapture 自己会
  `GatherActiveExtensions`（`SceneCaptureRendering.cpp:885`）、每个 renderer 都调 Pre* 回调
  （`SceneRendering.cpp:3999-4013`）⇒ **补一个返回 true 的 override 是字面意义上的空操作**。
- 「逐 proxy 不逐 view ⇒ 跨族盖写」在 5.7.4 不成立：`SceneRenderBuilder.cpp:872/915`
  **每 renderer 一张独立 RDG 图**。
- 「中段改容量 ⇒ 画侧拿陈旧裸指针」—— 实测推翻（`Scripts/TinyGladeShotStairsProbe.py`：
  全程只分配一次，症状一模一样）。
- 视锥剔除、LOD、容量、抓拍节奏、移动/驾驶视口、`bGpuFrustumCulling`、`bGpuLODSelection`
  （开/关出图逐字节相同）。

**目前最硬的事实**：indirect args 的 `InstanceCount = 216`、且随剔除开关在 **2/216/0** 之间跳，
**而出图逐像素不动** ⇒ **画出来的那批不是这趟 compaction 的产物**。

**下一步的决定性实验**（回读 VisibleOrigins + 打 `View.bIsSceneCapture`）已写进
`Scripts/TinyGladeShotStairs.py` 文件头。

影响面：这条不修，以后所有视觉验证都只能靠 readback 断言。

#### D13 的既有藤蔓管线接不上房子（硬阻塞）

`VineScatter::CollectSurfaceTriangles`（`Private/CSVineScatter.cpp:38`）只遍历
`UStaticMeshComponent`，而房子挂在 `UCSMeshRenderComponent` 上 —— 三个 `Scatter*` 入口对房子
**静默返回空三角集**，不报错。D13 必须另写「墙矩形 → 点集 → 填 ISM」的通路，不能指望复用。

#### 提取资产：网格几乎不缺，缺的是材质

`Content/HouseTest/TinyGladeAsset/Meshes/` 实有 **474 个** StaticMesh。窗户全家桶（cottage/gothic × rank1-3）、
`setdressing_window_sill/lintel`、58 个 clutter、`ivy_branch/leaf/flower` 全在。

只缺 7 个，且全是**导入器会丢的退化网格**：6 个 `window_cottage_*_flowerbed_locations`
（零三角形点集，坐标已实测并记在 **本文卷二 · 窗户与装饰对照**，硬编即可）+
`outline_window_cottage_corner_2x1`。

真缺口是三条**材质**：① 无窗/玻璃母材质（`*_glass` 上的 `is_glass` 顶点流无人消费）；
② 无藤蔓母材质（`ivy_branch` 只有 `Vertex_Position`，**无法线无 UV**，必须配专用着色器）；
③ 474 个网格里 **473 个是孤儿**，全仓唯一活引用是 `Scripts/TinyGladeSetupFrame.py:12` 的 `brick`。

### 踩过的坑（别再踩）

| 坑 | 症状 | 正确做法 |
| --- | --- | --- |
| **离屏 SceneCapture 被引擎写死关掉 Lumen** | PPV 里所有 Lumen 参数在出图里**一条都不生效**，改了等于没改，容易误判成"参数没调对" | `SceneCaptureRendering.cpp` 的 `SetupViewFamilyForSceneCapture` 在 blend 完世界 PPV **之后**直接写 `DynamicGlobalIlluminationMethod = None`。必须用捕获组件自己的 `post_process_settings` 覆盖回来 |
| **UE Python 对不存在的属性抛异常** | 脚本从那一行起**全部没执行**，但前面的日志看着一切正常 | 删 C++ 属性时全仓 grep `Scripts/`；`set_editor_property` 前若不确定就 try |
| `ExponentialHeightFogComponent` 的属性名 | `volumetric_fog` 写不进去 | 实际叫 `enable_volumetric_fog` |
| **脚本会把值烘进 BP CDO 与关卡实例** | 只改 C++ 默认值、或只改脚本，**画面都不变**，很容易误判成"改动没生效" | `TinyGladeSetupFrame.py` / `TinyGladeMakeGroundMaterial.py` 改完**必须重跑** |
| **UE Python 枚举比较** | `str(枚举).endswith("X")` **恒假**、`int(枚举)` 直接 TypeError —— 分支静默走错 | 用 `unreal.CSRidgeAxis.X` 直接比对象 |
| **出图脚本准备阶段抛异常 ⇒ 编辑器永不退出** | 任务像挂死，实际是 `-ExecCmds` 那条 Quit 再也执行不到 | 准备阶段整个包在 try 里，异常也要走到 Quit |
| **墙面近纯白 + 自动曝光** | 两张室内图糊成全白，看着像"渲染坏了" | 曝光偏置**逐张**给，别指望一个全局值 |
| **离屏 SceneCapture 默认没有 ViewState** | **PPV 里的曝光一条都不生效** —— 实测把 EV100 改 2.83 倍，五张图每个分位数**逐位不变**。不先修这条，任何曝光标定都是假的 | `always_persist_rendering_state = True`。与"SceneCapture 被写死关 Lumen"同症状、**不同根因** |
| **SkyLight 是 `SLS_CAPTURED_SCENE` 但 `real_time_capture=False`** | 静态立方图从没捕获过 ⇒ **是一张黑图**、环境光贡献恒为零，而 Details 面板里它看着一切正常（可见、强度 1.0、affects world） | 这个工程地形与房子一直在变，`real_time_capture=True` 是唯一说得通的模式。对照组：`L_HouseGroundDemo` 那盏本来就是 True，同样场景就不黑 |
| **`LowerHemisphereColor = (0,0,0)`** | 所有朝下/侧下方向的环境光**精确为零**。指纹是最暗 20% 像素平均饱和度 **0.000** | 设成地面主色（草绿）。`bLowerHemisphereIsBlack` 字面是"下半球用纯色"，不是"必须黑" |
| **两个脚本写同一个属性 ⇒ 画面取决于谁后跑** | 两次都打"成功"，结果却不同。已在石阶材质上踩过一次 | 每个属性只由一个脚本负责；跨脚本的顺序依赖要在文件头写明 |
| **改世界让测试通过（反面版本）** | 驱动方清空 `StepMeshes` 想关掉旧路石阶的重复绘制，回归当场红 `a road over the mound grows steps`——那条断言直接拿关卡里这个 shaper 当被测对象。且脚本已把空值**存进 .umap**，而 umap 不在 git 跟踪内，退脚本没用，只能从 BP CDO 读回来 | 想让演示关卡只画一套石阶，得先让那条断言**自带**调色板（测试自备夹具）。在此之前两套叠着是**已知且有意保留**的状态 |
| **离屏 capture 有了 ViewState 之后，Lumen 单帧间接光恒为零** | 凡是没被太阳直射的 lit 表面**精确 (0,0,0)**，看着像"几何有洞/材质坏了"。**坑 ⑧ 与本条方向相反**：修了 ⑧ 才会触发 ⑨ | 导出前 `capture_scene()` 预热 ≥16 次（本项目取 32）。Lumen 的最终聚集靠**帧间历史**，一帧抓不出来 |
| **母材质没勾 `bUsedWithInstancedStaticMeshes`** | 引擎**静默换成默认材质**，症状与"没绑材质"**逐像素相同**。`M_TG_Texture`（gallery 看图材质）就没勾 | 走实例路的材质必须勾。把它纳入 `IsXxxDrawable` 的执行面，别只查"材质非空" |
| **注释可能在撒谎，别只读注释** | `RebuildGpuMesh` 的 `!IsRegistered()` 分支注释写着"省一次渲染往返"，**实际调了 `ReleaseGpuMesh()`** —— 每帧白付 3 次阻塞刷新，而且与容量/包围盒/实例数全都无关 | 追性能问题时**读代码不读注释**；改属性 ⇒ `RerunConstructionScripts` ⇒ **卸载全部组件**，这条链是很多"莫名其妙每帧重建"的根 |
| **`-ExecutePythonScript` 没有 tick 去泵 GameThread AsyncTask** | 刚拖完读到的三角形数**落后一代**，看着像"改动没生效" | 异步编辑收尾（`BodySlot.Pending` → `OnBodyEditComplete`）需要 tick；脚本里要显式 settle。编辑器里有 tick 故非缺陷。**旧代码每帧的阻塞刷新无意中替它做了同步 —— 消掉阻塞才暴露出来** |
| **拿 TG 贴图前先确认它是干什么用的** | `grass_patch_summer` 是草叶**卡片**贴图，形状全在 alpha 里、RGB 空白边是纯白 —— 当地面贴图用会在每个平铺边界糊出白线 | 提取资产按**用途**核对，不按名字猜；地面用 `dirtpath_grass` 这类不透明无缝图 |
| `unreal.Rotator(a,b,c)` 是 `(roll, pitch, yaw)` | 相机朝正上方看天，出图一张纯渐变，像"渲染没出来" | 一律用关键字参数。判别：拍一张俯视，地平线**竖着**就是 roll 被当成了 pitch |
| `-ExecutePythonScript` 跑完即退编辑器 | tick 回调永远不触发 | 需要 tick 的脚本用真编辑器 + `-ExecCmds="py <脚本>"` |
| `create_render_target2d` 默认浮点 | 导出的 `.png` 是 HDR 内容，看图工具打不开 | 显式传 `RTF_RGBA8` |
| 编辑器视口截图 | 本工程是 4 分屏，`HighResShot` 抓的是空面板 | 用离屏 `SceneCapture2D` |
| `-nullrhi` | `AddSetCountersPass` 的 `TShaderMapRef` 断言失败 | 演示回归/出图必须真 RHI；纯 CPU 单测才可 `-nullrhi` |
| 日志被并发实例覆盖 | 找不到自己的输出 | 一律 `-abslog=<独立文件>` |
| unity 构建掩盖缺失 include | 全量过、`-SingleFile` 挂 | 新 TU 的 file-local helper 加模块前缀；改完用 `-SingleFile` 验 |
| CDO 默认值不传播到已存在实例 | 脚本设了参数却"没生效" | 实例上必须再写一份 |
| 笔刷是 3D 球 | 坡面上整段落空，像"路没画上" | 脚本落笔必须自己 `SampleHeight` 贴地 |
| **派生链的重建判据只看了"另一样东西"** | 岩壳/石阶/摆件停在陈旧位移上：碎石长在地图另一头，而 `SampleHeight` 正常、演示回归全绿（回归在量之前先画路，把它治好了；三条断言又都是相对塑形物自身位置分类的） | `RefreshHeightsInRegion` 的 `!bChanged` 早退门必须放三条派生链过去 —— 它们各有幂等守卫，空转不要钱。加载时序是「塑形物先注册、后落变换」，别假设注册那一刻拿到的是最终变换 |
| **弧长比例 ≠ 样条参数** | 块沿曲线**疏密不均**：密处互相穿插只露一条薄片（看着像"朝向错了/有透明"），疏处留大洞 | `ScatterGroundStepsCS` 把 `Rec.x` 当**均匀 B 样条参数**用，只有控制点等弧长时才等价于弧长比例。地形石阶的等角圆环天生满足，门框的「门樘→拱→门樘」不满足（实测控制点间距比 **16.9:1**，17/19 块砖挤在拱上、间距 10.9 cm 而砖长 26 cm）。**喂控制点前必须先重采样成等弧长**（`CSShaperSteps::ResampleUniform`），契约已写进 `FCurve` |

---

<a id="vol-1"></a>

## 卷一 · 拉尺寸（D5）与接缝角柱（D7）对照

> 原文件 `TinyGlade_拉尺寸与接缝对照.md`，原标题「Tiny Glade 拉尺寸（D5）与接缝角柱（D7）对照」。

> ⚠️ **时效（2026-09-18 补注）**：本卷写于 2026-08-30 / 31，文中「本项目今天」的实现描述停在那一刻。之后变了的三件：`UCSHouseSubsystem`（0.25 s 快扫 / 花名册 / `MarkHouseDirty`）已于 09-16 删除，名单改为 `UCSHouseLibrary::GetHouses`、移动由根组件 `TransformUpdated` 标脏；接缝从「两栋房各画一份的纯函数」改为两端共持的接触记录 `FCSHouseContact`（一条缝只砌一次、柱逐接触出）；footprint 自 09-13 起是闭合凸折线，抓手按边数生成。现行设计以计划 D5 / D7 为准，本卷的 TG 侧对照结论不受影响。

本文回答四件事：**TG 怎么拉尺寸**、**TG 怎么收口（转角 / 墙-地 / 墙-顶 / 房-房）**、
**本项目今天差在哪**、**接进来要动哪些设施**。

设计裁决在 [`Plan_House.md`](Plan_House.md) 的 D5（`:241-264`）与 D7（`:332-416`），
本文**不复述**，只写复核结论与新证据。体例对齐
[本文卷三 · 楼梯模块对照](#vol-3) 与
[本文卷二 · 窗户与装饰对照](#vol-2)。

### 证据标注约定

| 标注 | 含义 |
| --- | --- |
| 【确凿】 | PDB 符号 / 资产字节 / 反编译 GLSL / 本仓源码直接给出，不含推理 |
| 【推测】 | 由确凿证据推理得出，推理链在正文写明 |
| 【待确认】 | 证据不足以判定，正文写明「需要什么证据才能确证」 |
| `[PDB]` | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt`（97033 条），行号即该文件行号 |
| `[PATH]` | `D:/MyProject/Tiny Glade/tmp/pdb_paths.txt` |
| `[GLSL]` | `D:/MyProject/Tiny Glade/tmp/shaders/` 反编译产物 |
| `[资产]` | `D:/MyProject/Tiny Glade/assets/` 实测 |
| `[UE资产]` | `Content/HouseTest/TinyGladeAsset/` 实测 |
| `[代码]` | 本仓 `Source/ComputeShaderGenerator/` |
| `[分析]` | `MESH_GENERATION_ANALYSIS.md` |

---

### 结论速览

**D5**

1. TG 的抓手**不是 handle actor**，是「**拖墙本体**（改尺寸）+ **拖角**（旋转）+ **中心 gizmo**（移动）」，
   命中体是解析 trigger volume，视觉是 signifier。计划裁决的「handle actor + 标准 gizmo」形态里那条
   *「不加记账量墙会走 2 倍」* 的父子回路缺陷，在 TG 形态下**根本不存在**。
2. **拖动期零节流、零特例**——全库 97033 条符号里找不到任何 gameplay 节流设施；砖每帧重排。
   代价被三件事吃掉：只重建脏墙、常驻稀疏实例缓冲增量写（**零设备同步**）、一个 unit cube 撑全部砖。
3. **墙加长 = 加砖**，不是拉伸现有砖。而且**砖被有意胀大 4.5% 互相穿插**（GLSL 实证），
   这是"逐帧重排砖却看不出跳变"的根本原因，也是本项目最值得直接抄的一条。
4. **洞是绝对位置**（`WallSpaceCoord` + 可钉在矩形角上），装不下的洞被**归档**而不是删除，拖回来当场复现。
5. **没有"加一层楼"这种跳变**——TG 无楼层概念。屋顶更是**连续**的：脊长是矩形长宽比的函数
   （`ridge_length_01_from_rectangle_ratio`），**根本不存在"脊向翻转"这个事件**。⚠️ 与计划 D4 的
   离散翻轴 + 1.15 滞回冲突，见 [C-D5-1](#c-d5-1)。
6. **本项目零阻塞纪律在拉尺寸上会当场破**：纯平移是 0 flush，**拉尺寸不是**——四个来源，
   最严重的一个（门框砖 handover）**逐帧触发、最多 4 次同步 flush**。见 §三。

**D7**

7. TG 有**两套完全不同**的机制：freehand 墙端点相接 → **拓扑合并成一面墙**（无接缝构件）；
   shape 围合体互相压上 → **开洞 + 缝砖**。后者才是 D7 的对位物。
8. `detect_intra_shape_corners` 在相交处**先开洞**（`ResMut<WallHoles>`），计划 D7 只写了立柱遮挡、
   没有开洞这一步。见 [C-D7-1](#c-d7-1)。
9. 缝砖是**两级纯函数**：`IntraShapeCorners → InterShapeBrickStitches`（**只有两个参数**）
   → `spawn_stitch_bricks`。比计划 D7 的 actor + 弱引用 + `EnsureRefreshed(Gen)` + 邻近合并
   简单一个数量级。见 [C-D7-2](#c-d7-2)。
10. **TG 没有任何角柱 / 护角 / 墙裙 / 脊瓦 / 收边预制件**（`assets/meshes` 138 个 + `nani_meshes.ron`
    181 个名字，全部 grep 过）。转角、墙裙、缝、雉堞**全是同一个 `brick` × 每实例非均匀缩放**。
11. 本项目今天的墙角是**精确对接**（不穿插、不留缝），破绽是 **UV 岛断裂 + 硬棱**，不是穿模。
    真正难看的三处在**墙-顶**：檐口内侧一条 17 cm 贯通开口、山墙斜边零余量共面（发丝缝）、
    屋脊两板互穿露出 7 cm 交叉尖，**且完全没有收边件**。

---

### 一、D5：TG 怎么拉尺寸

#### 1.1 抓手形态：四种亲和，各配一个 trigger volume + 一个 signifier【确凿】

TG 的房子是一个 **shape**（`mode_manager::WallShape`：rectangle / circle / freehand），
编辑亲和分四种，`[PATH]` 目录结构逐条对得上：

| 亲和 | 系统 `[PATH]` | 命中体 | 视觉件 `[资产]` |
| --- | --- | --- | --- |
| **改尺寸** | `wall/shapes/rectangle/ui_edit_rectangle_dims.rs` | `WallTriggerVolume`（**墙本体**） | `dashed_lines.glb`（尺寸线） |
| **旋转** | `.../ui_edit_rectangle_orientation.rs` | `box_trigger_volume` / `skewed_box_trigger_vol` + `ClosestCornerCache` | `corner_arrow.glb` / `corner_arrow_outline.glb` |
| **移动** | `systems/ui_move_shapes.rs` | `move_gizmo_and_trigger_vol` + `MoveShapeCache` | `move_shape.glb` / `move_shape_outline.glb` |
| **抬高 / 墙高** | `systems/ui_raise_shape.rs`、`wall/adjust_wall_height/` | — | `flat_arrow.glb`、`WallHeightSignifier` |

`ui_edit_rectangle_dims` 的完整签名（`[PDB]` 行 8930）：

```text
Res   <AppMode>
Res   <UiSignifierStream>
Res   <UiState>
Res   <CameraCursorRay>                       ← 光标射线
Res   <PublicWalls> · Res<WallEntities>
Query <&WallTriggerVolume>                    ← 命中的是墙自己的粗四边形壳
EventReader<MainCameraMoved>                  ← 相机一动就重解析悬停
ResMut<UiEditRectangleActiveWidgetResource>   ← 当前激活的是哪个 widget
EventWriter<DoUiEditRectangleDims>
```

`WallTriggerVolume` 的 API（`[PDB]`）：`recompute` / `recompute_quads` / `get_tris` / `default_tris` /
`get_collision_geo` / **`get_position_from_mesh_uv`** —— 射线命中后直接返回墙面 UV，
再映射回 wall space，因此系统知道你抓的是**哪条边、边上哪个位置**。
`Rectangle2d` 侧配套的解析原语（`[PDB]`，`utils/src/geometry/rectangle.rs`）：

```text
expand_along_edge · with_expanded_size · get_closest_edge_id · get_edge_id_at_u
get_edge_pts · get_edge_normal · get_corner_normal · get_edge_dirs_from_corner_id
get_pos_at_u · get_point_u · get_closest_u_from_pos · edge_length · perimeter
```

**`expand_along_edge` 这个符号直接坐实了「单边推拉」的语义**——与计划 D5 的
*「房屋单边推拉该墙（对侧不动、中心随动）」* 逐字对上。

signifier 有 `SignifierMinimumLifespan`（最短存活）防止悬停边界上的闪烁，
`ShapeSignifierLerp` / `ShapeVal`（`wall/shapes/utils/shape_signifier_lerp.rs`）做数值平滑——
**平滑只加在 UI 提示上，不加在几何上**。

#### 1.2 拖动期：每帧重建，全库零节流【确凿】

完整链路，每一环都有 `[PDB]` 签名：

```text
ui_edit_rectangle_dims (行 8930)   悬停/命中 → DoUiEditRectangleDims
        ↓
do_ui_edit_rectangle_dims (行 8845)
    ResMut<WipRectangle> · ResMut<DecoratorArchivist> · Res<PublicWalls>
    EventWriter<EditRectangleCmd>          ← 逐帧
    EventWriter<FinishEditRectangleCmd>    ← 松手
    Res<GladeBorder> + EventWriter<DisplayOutOfBorder / FeedbackInputOutOfBorder>
    EventWriter<HideTreesinAreaCmd> · EventWriter<ShowPinnedDecoClutter>
        ↓
edit_rectangle (行 9161)
    EventReader<EditRectangleCmd> · ResMut<InnerWalls> · 两个音频 EventWriter · Res<Time>
    ——— 注意：没有 History
        ↓
publish_public_wall_state → OnWallChanged → construct_walls → 重排砖
    → RecalculatePlasterCmd / RecalculateHalfTimberMeshCmd / UpdateRtWorldEvent

松手：finish_edit_rectangle (行 8841/9536)
    EventReader<FinishEditRectangleCmd> · ResMut<History>   ← 撤销只在这里录
    · ResMut<DecoratorStorage> · ResMut<DecoratorArchivist>
    · EventWriter<OnRectangleEdited> · EventWriter<CmdCheckForSheepInsideBuilding>
        ↓
decorator_on_rectangle_edited (行 8842/9537)
    Res<InnerWalls> · EventReader<OnRectangleEdited> · ResMut<DecoratorStorage/Archivist>
```

**「全库零节流」是否定式结论，边界写清楚**：在 97033 条符号里 grep
`throttle|debounce|rate_limit|coalesc|budget`，只命中 `notify::debounce`（文件监视热重载，
`configwatch`/`meshwatch` 用）与 `PhysicalDeviceMemoryBudgetPropertiesEXT`（Vulkan 内存预算），
**没有一条与几何重建有关**。这与 `[分析]` §1.1 的
*「画线进行中与抬笔定稿走完全相同的重建管线」* 是同一条纪律的另一个侧面。

每帧重建付得起，靠三件事，都是【确凿】：

1. **只重建脏墙**：`detect_intra_shape_corners::WallAndRoofChanges::dirty_walls`（`[PDB]`）
   把 `OnWallDeleted` / `OnWallChanged` / `RecalculateRoofCmd` 三条事件流 chain 成一个
   `HashSet<WallId>`，下游只处理集合里的墙。
2. **GPU 上传是常驻缓冲的增量写，零设备同步**：`SparseInstanceBuffer::{gpu_flush, hide, show,
   take_out_waste}` + `AllocationHandle` / `ChunkId` + `SparseInstanceBufferWriter::{clear, push}`
   + `DirtyIndexRange`（`[PDB]`，`country-core/src/render/`）。重建 = `clear` → `push × N` →
   `gpu_flush`；砖数变少时旧槽变成 waste，由 `take_out_waste` 摊还回收，**不在拖动那一帧扩容**。
3. **一个 unit cube 撑全部砖**：`brick.json` 实测 **600 顶点 / 300 三角 / ±0.5 单位立方**，
   带逐顶点 `is_bevel` 属性（`[资产]`）；砖尺寸完全由 `Affine3Packed transform` 的非均匀缩放给
   （`[分析]` §1.4）。

**节流只有两处，都不是几何**：History 只在 `finish_edit_rectangle` 写；
`OnRectangleEdited` 只在松手发（decorator 的最终重锚定结算）。

#### 1.3 砖：加砖，不是拉伸；而且砖被有意胀大 4.5%【确凿】

布局工具链全在 `wall-constructor/src/utils/`（`[PATH]` + `[PDB]`）：

```text
split_into_random_rows → split_horizontally → perturb_row_ends
    → trim_rows(按洞裁) → resolve_hole_overlap
    → wall_corners::add_wall_corners → wall_skirts::add_skirts
```

全部沿**弧长**工作（每砖带 `wallspace_x_range` = 沿墙参数区间，`[分析]` §1.4），
墙变长 ⇒ 行更长 ⇒ `split_horizontally` 切出更多砖。**砖长不随房子缩放**。

**⚠️ 最值得抄的一条，GLSL 实证**
（`[GLSL]` `_wall_wall_brick_lod0.raster...vs_main.glsl:152-165`）：

```glsl
bool _293 = (_274 & 4) != 0;                       // flags bit2 = 普通墙砖
if (_293) {
    /* hash(seed) → [0,1) */
    float _312 = (hash > 0.9) ? 1.0 : 1.1;
    _316 = mix(vec3(_312, _312, 1.0), vec3(1.05), bvec3((_274 & 1) != 0));
}
vec4 _330 = vec4(vec4(_291 * (_316 * 0.95), 1.0) * _259, 1.0);
```

局部轴由同函数上方的拱高插值 `_290.z = _264.z * mix(arch.x, arch.y, 0.5 - _264.x)` 定死：
**x = 沿墙、y = 穿墙厚、z = 竖直**。所以：

- **沿墙 + 穿墙**：90% 的砖 ×1.045，10% ×0.95；
- **竖直**：一律 ×0.95。

也就是说 **九成的砖沿墙方向互相吃进去 4.5%、并且鼓出墙面 4.5%**，而竖直方向统一缩 5% 让灰缝露出来。
**这就是「逐帧重排砖却完全看不出跳变」的物理原因**——砖缝本来就是负的，少一块多一块、
缝挪半厘米都不可能露出黑缝。本项目今天的 `SolveBlockLayout` **没有这一步**（见 [A5](#a5)）。

【待确认】墙变长时**既有砖是否原地不动**。`utils::rng::{init_rng, init_rng_u64}` 说明种子是显式给的，
但从符号无法判定种子是否与「沿墙的绝对弧长」绑定。要确证需要**逐帧录屏比对砖缝位置**
（拖一条边，看远端的砖缝有没有跟着走）。这条不影响本文任何行动项——本项目的门框砖用
`FrameSeed` + `FRandomStream`，是否稳定由自己决定。

#### 1.4 洞：绝对位置，且装不下就归档【确凿】

- **锚在 wall space，不是比例**：`WallSpaceCoord::{from_curve_internal_coord, from_world_space,
  to_world_space, get_pos_xz, get_tangent, with_y, with_y_offset}` + `WallSpace::space_length`
  + `WallSpaceRegion`（`[PDB]`，`utils/src/wall_space.rs`）。decorator 侧是
  `walls::decorator::{WallAttachment, RoofAttachment, StairAttachment, TerrainAttachment}` +
  `wall_attachment::{WallAttachmentAnchor, WallAttachmentSide, WallCornerAttachment}`。
- **可以直接钉在矩形的某个角上**：`WallCornerAttachment::{from_rectangle_corner_id,
  to_rectangle_corner_id, is_attached}` + `DecoratorSubtype::can_be_on_corner`，
  配一整套绕角窗资产 `window_cottage_corner_{1x1,2x1,3x1}`（`[资产]`）。
- **装不下 = 归档，不是删除**：`DecoratorArchivist::{archive_current_decorators_in_storage(s),
  archive_storage_decorators, archived_walls, extract_and_restore_to_storage, extract_diff,
  extract_before_after_impl}` + 系统 `restore_archived_decorators`（`[PDB]`/`[PATH]`）。
  而且 `ResMut<DecoratorArchivist>` 出现在**逐帧的** `do_ui_edit_rectangle_dims`（行 8845）里 ——
  **拖小的过程中窗当场消失、拖回来当场复现**，不用等松手。

对位物：本项目 D8 的特征标记（`ACSHouseFeatureMarker`）天然就有这个语义——标记 actor 常驻，
`QueryFeaturePlacement` 判不可行就不出几何，拖回来再出。**窗户这边零缺口**。
道路推导的门那边没有，因为门本来就是重算出来的（见 1.5 末段）。

#### 1.5 离散跳变

**没有楼层。** 全库零 storey / floor-count 符号；一个 shape = 一圈墙 + 可选一个屋顶。

**屋顶不是离散翻面，是连续脊长**【确凿，⚠️ 与计划冲突】。`[PDB]` 给出：

```text
roof_shape::ridge_length_01_from_rectangle_ratio    ← 脊长 = 矩形长宽比的函数
roof_shape::inverse_ridge_01_param
roof_shape::roof_height_ws_from_wall_shape
roof_ridge::RoofRidgeDims::{calc, from, get_inner, to_world_space}
roof_ridge::calc_roof_tip_center_ls
```

矩形越接近正方形，`ridge_length_01` 越趋近 0，gable **连续退化成 pyramid/hip**——
正方形处两条轴对称，**「脊朝哪」这个问题根本不出现**，也就不需要滞回。
手动覆盖是另开的 UI（`ui_edit_roof_ridge_dir` / `_length` / `_tip`，资产
`corner_arrow_switch_ridge_dir.glb`）。这把 `[分析]` §3.1 里
*「脊长=0 近似 hip【推测】」* 升级为【确凿】。

**真正的离散跳变在 decorator 一侧**：`DecoratorRank`（`window_cottage_1x1 / 2x1 / 3x1`、
`balcony_door_rank1/2/3`）与 `DecoratorSubtype`（`can_be_on_corner`、dormer / half-dormer /
full-dormer、`is_half_dormer`、`can_become_full_chimney`）。墙变长变高，窗**换档**而不是被拉伸；
`derive_decorator_subtypes`（行 8831）每帧重导出。跳变靠**动画**掩盖——
`autoclutter_animation`、`roof_animation`、以及 `roof_tiles_gravity` 用 `wall_height_delta`
让瓦片按重力落到新高度（`[分析]` §3.2）。

**边界**：`GladeBorder` + `DisplayOutOfBorder` + `FeedbackInputOutOfBorder` 在**逐帧**的编辑系统里 ——
拖出边界不是硬 clamp，是给反馈。顺带每帧发 `HideTreesinAreaCmd`（新 footprint 盖住的树当场隐藏）。

---

### 二、D7：TG 怎么收口

#### 2.1 两套机制别混【确凿】

| 情形 | 机制 | `[PATH]` |
| --- | --- | --- |
| freehand 墙**端点**相接 | 吸附 → **拓扑合并成一面墙**，无任何接缝构件 | `wall/snapping/{snapping_to_ends, snapping_to_segments, telegraph_walls_are_merged}`、`wall/{merge_walls, wall_splitter}` |
| **shape 围合体互相压上** | **开洞 + 缝砖** | `clutter/src/inter_shape_stitches/{detect_intra_shape_corners, stitch_bricks}` |
| **屋顶互相压上** | **开洞 + 雉堞砖** | `clutter/src/inter_roof_merlons/{detect_inter_roof_merlons, construct_inter_roof_merlons}` |

**只有第二行是 D7 的对位物**（计划 D7 的触发条件就是"两房 footprint box 真正相交"）。

#### 2.2 `detect_intra_shape_corners`：先开洞，再出交点表【确凿】

完整签名（`[PDB]` 行 8854，原文逐字）：

```text
Res   <PublicWalls>
Query <&Roof>
Res   <TerrainHeightsData<TerrainHeightSsbo>>
       WallAndRoofChanges                    // = EventReader<OnWallDeleted|OnWallChanged|RecalculateRoofCmd> + dirty_walls()
EventReader<ClearCanvasCmd>
ResMut<WallHoles>                            // ← ① 在相交处开洞
Local <Vec<ShapeIntersectionHole>>           // ← 系统私有的上一帧洞表，用于差分
ResMut<IntraShapeCorners>                    // ← ② 交点表
ResMut<ShapeIntersections>                   // ← ③ 相交关系（下游 RecalculateRectangleSupports… 也读它）
Res   <UiSignifierStream>
```

内部函数：`intersect_shapes<F>`（泛型，merlons 复用同一个）、`add_hole_at_shape_intersection`、
`ShapeIntersectionHole`、`Corner`。

三条与计划不同的观察：

- **TG 先开洞**。两个围合体压在一起时，穿插的那段墙被 `WallHoles` 裁掉（`HoleType` / `HoleOrigin`
  记来源），两个房间内部连通。计划 D7（`:332-416`）只写了"在交点立一根角柱遮住墙体穿插处"，
  **没有开洞这一步** —— 见 [C-D7-1](#c-d7-1)。
- **触发是 push 的脏墙集合，不是每帧全量 OBB 扫描**。
- **增量判据放在系统的 `Local` 里**，不是放在一个 actor 上。这是计划 D7 `InputSignature`
  两级判断的对位物，但载体形态不同。

**2026-09-15 反汇编补证**（[`evidence/inter-shape-stitches-20260915.asm`](evidence/inter-shape-stitches-20260915.asm)，dumpbin + PDB；此前「先开洞」只到符号名）：

- `add_hole_at_shape_intersection(wall, [P0, P1], [y0, y1]) -> Option<hole>` 共 99 条指令：两个交点按中间高度 `(y0 + y1) / 2` 经 `WallSpaceCoord::from_world_space` 转进墙空间；`hole.center_s = project_clamp(s0 + ds / 2)`、`hole.width = ds`（`ComparativeSpace::subtract`，闭合墙绕回一整圈）、`hole.height = y1 − y0`；`ds ≤ 0` 返回 `None`（niche 值 5）。
- 主函数在「沿墙循环相邻两个交点」的循环里调它（`Cycle<Iter<(InteractionMode, Vec2)>>`），只对模式位标成「这一段在对方形状内」的相邻交点对调；`[y0, y1]` 是两面墙高度区间的交（先过 `Range<f32>::intersects`，某种 `Roof::ty` 下 `max_y` 先减 0.67 m）。攒下的 `Vec<ShapeIntersectionHole>`（40 B/条：墙 id + 洞 + `HoleType` 字节）每帧逐条 `WallHoles::add` 回去（`WallHoles` 每帧清空）。
- ⇒ 洞 = **本墙被对方 footprint 盖住的整段 × 两墙高度区间的交**，与本项目 `CutOnEdge` 输出的 `FCSWallCut`（`MinS..MaxS` × `BottomZ..TopZ`）逐项同义。**从室外看不到它**——整段都在对方房子里面；高过对方檐口的那截墙照旧保留。「两个房间内部连通」只对重叠高度成立。

#### 2.3 缝砖：两级纯函数【确凿】

```text
行 9091:  ResMut<InterShapeBrickStitches>  ←  Res<IntraShapeCorners>
          （只有两个参数：交点表 → 缝砖描述。零地形、零历史、零撤销、零 UI）

行 8966:  Res<InterShapeBrickStitches> · Res<AssetSsboLibrary> → ResMut<StitchBrickAlloc>
          （spawn_stitch_bricks；砖记录类型 StitchBrick）
```

屋顶那套同构，且**共用同一个 `StitchBrickAlloc`**（行 8879 的签名里
`ResMut<StitchBrickAlloc>` 与 `Res<InterRoofMerlons>` / `ResMut<InterRoofMerlonBrickCount>`
同时出现）。雉堞砖走 `flags` bit2（取 `roof_color_ids` 而不是砖色，`[分析]` §1.7）。

**这是「D7 是纯函数」最硬的实证**，而且比计划 D7 那套
"中立 actor + 两端弱引用 + 自 tick + `EnsureRefreshed(Gen)` + 邻近合并按 key 定序"
**简单一个数量级** —— 见 [C-D7-2](#c-d7-2)。

#### 2.4 转角 / 墙裙 / 墙-顶：三个函数，零预制件【确凿】

- **墙自身转角**：`utils::wall_corners::add_wall_corners`（`[PDB]` 行 86507）——
  在切层、切砖、裁洞**之后**往 `Vec<MyBrick>` 里**追加转角砖**。
- **墙裙**：`utils::wall_skirts::add_skirts`（行 86510）—— 墙底与地面之间那一圈。
- **墙-顶**：`system_roof::visual::generate_roof_stone_floor_and_roof_bottom` 出 roof bottom，
  走 nani `_nani_roof_floor`，实例参数带 `is_roof_bottom`，**Y 量化
  `floor(y*512)/512 + fract(wall_id*1.618)/512` 专门防 z-fighting**（`[分析]` §3.3）。
  另有 `GableRoofStyle::{is_overhang, is_stepped}` —— `is_stepped` = **阶梯山墙**
  （山墙沿坡按砖阶梯收口），`RoofShapeParams::eave_length_ws` = 檐口出挑长度。
- **灰泥**是独立网格贴在墙的某一侧（`plaster_systems::{create_plaster, generate_plaster_mesh,
  split_wall_into_sides, mirror_holes_as_needed}` + `WallSide`），**不是用来盖缝的**。

#### 2.5 资产层的硬结论：TG 没有任何收边预制件【确凿，实测】

`assets/meshes` 实有 **138** 个网格（`extracted/meshes` 同数），`nani_meshes.ron` 里 **181** 个名字。
对 `corner|stitch|skirt|pillar|column|merlon|quoin|plinth|coping|capping|trim|base|cornice|ledge`
全部 grep 过，结果：

| 命中 | 实际是什么 |
| --- | --- |
| `corner_arrow` / `_outline` / `_switch_ridge_dir` | **UI gizmo**，不是几何 |
| `flag_corner_beam` / `_no_connection` | 旗帜的转角挂杆 |
| `window_cottage_corner_{1x1,2x1,3x1}` 及其 `_glass/_sill/_outline` | **绕角窗**，是洞不是盖 |
| `garden_stone` / `water_stones` | 花园 / 水边石 |
| `brick` | **唯一的砖**，600 顶点 / 300 三角 / ±0.5 单位立方 / 带 `is_bevel` |

**没有 pillar / column / merlon / quoin / plinth / coping / capping / trim / stitch / pier 网格。**
转角、墙裙、缝砖、雉堞、烟囱、承重柱（`construct_elevation_supports/util_pillar_construction::
construct_brick_columns`、`util_cross_brick_pillar::construct_crossbrick_pillar`）
**全部是同一个 `brick` × 每实例非均匀缩放**。

---

### 三、本项目侧（D5）：零阻塞风险点

#### 3.1 交互设施：零

~~全 `Source/` 树 0 命中：`ResizeHandle` / `CSHouseResizeHandleActor` / `OnResizeModeChanged` /
`SelectionWatcher` / `EnterResizeMode` / `ExitResizeMode`。计划 D5 是**纯白纸**。~~

> ✅ **本节 2026-09-05 整条作废：D5 三层全部落地。** 机制层（`CSHouseResize.h` + `PushEdge`）
> 2026-08-31，交互层（`ACSHouseResizeHandleActor` + `EnterResizeMode` / `ExitResizeMode` /
> `OnResizeModeChanged` + `FCSHouseResizeSelectionWatcher`）2026-09-05。
> `NotifyHandleDragged` 仍是 0 命中，但那个名字**从未存在过** —— 机制入口叫 `PushEdge`。
>
> **仍然成立的是 `HHitProxy` / `UGizmo` / `InteractiveGizmo` 三个 0 命中**，而且这是用户裁决的
> 结果而非空白：抓手是真 actor，点选 / 框选 / 多视口全部白拿编辑器原生能力，不写一行 gizmo 代码。
> 零阻塞纪律也已在这条路上验过（`push_edge` 拖 12 帧 flush = 0）。

已有的边角料：`FCSBrushEdModeBase`（`PCGEditorProcess/Private/CSBrushEdModeBase.h:55`，
三个笔刷子类，且 `:71-74` **主动关掉 transform widget**）；
`FSelectedActorViewportOverlayBase`（`SelectedActorViewportOverlayBase.h:14`，
唯一子类按 `ViewEdit` 详情分类过滤，`ACSHouseActor` 没有这个分类，今天挑不中）。
`PostEditMove` 全插件只有 4 个 override，房子是其中之一（`CSHouseActor.cpp:1173`）。

#### 3.2 现有唤醒链

```text
PostEditMove(bFinished)      → bForceFullRebuild = true(仅 bFinished) → ReevaluateSite()   [CSHouseActor.cpp:1173-1181]
OnConstruction               → ReevaluateSite()                                            [:1123-1127]
PostEditChangeProperty       → 只 BindHouseMaterials()；重建靠 AActor 基类重跑构造脚本间接来 [:1158-1171]
UCSHouseSubsystem::Tick      → 每 ScanInterval=0.25 s 比 GetTrackingHash()                 [CSHouseSubsystem.cpp:57-101]
                                 （含 FootprintSize.X/Y 与 WallHeight，所以脚本改尺寸能唤醒）
```

`UCSHouseSubsystem::MarkHouseDirty`（`CSHouseSubsystem.h:38`）**已声明但零调用者** ——
拉尺寸正好是它的第一个客户（拖动期 push 标脏，绕开 0.25 s 的扫描间隔）。

#### 3.3 两级哈希：短路点在昂贵计算**之后**（有意为之）

`ReevaluateSite()`（`CSHouseActor.cpp:461-523`）**没有任何提前 return**，顺序是：

```text
:463  重入守卫 bInReevaluate
:466  ResolveGroundAndSubscribe()
:469  ComputeSeatZ()          ← footprint 全域 (NX+1)*(NY+1) 次 SampleHeight   [:224-233]
:479  脊向滞回
:481  ComputePlacementHash()  ← Loc.XYZ(q=1,1,0.5) + Yaw(q=0.1)                [:399-404]
:489  ComputeDoors()          ← 无条件跑，返回 body 形状哈希                    [:238-336]
:490  if (bForceFullRebuild || BodyHash != BodyShapeHash || !GetTinyGladeMesh()) RebuildBodyMesh()
:496  else if (PlacementHash != BodyPlacementHash) ApplyBodyPlacement()
:505  ComputePillars()        ← 无条件跑
:507  同型守卫 → RebuildPillarMesh() / ApplyPillarPlacement()
:519  if (bForceFullRebuild) FrameDescHash = 0;  :520 RebuildFrame()
```

`:484-488` 的注释写明这是有意的（门集合本来就是世界摆位的函数）。快路径
`ApplyBodyPlacement` / `ApplyPillarPlacement`（`:736-761` / `:845-862`）走
`EditMeshAsync` + `UCSMeshOps::AddTransformPasses`，**0 flush**。

⚠️ 顺带一条**计划书内部的过期**（无需拍板，做 D5 时顺手改）：D4 的 `:194` 仍写
*「`UCSMeshOps::TransformMesh` 同 pass 改 Positions 与 Tangents……」*，而
`Source/` 里**没有 `TransformMesh` 这个函数**；`:1270` 那张表其实已经记了迁移
（*「拆出 `AddTransformPasses`，摆位也走 `EditMeshAsync`」*），只是 `:194` 没跟着改。

#### 3.4 ⚠️ 拉尺寸会当场破掉「零阻塞」纪律

`GetBlockingFlushCount()` 的计数器在 `CSMesh.cpp:20`，唯一自增点是 `CSMesh_CountedFlush()`
（`:22-26`，`CSMesh.cpp` 里 12 个 `FlushRenderingCommands` 站点全部走它），访问器在 `:978`。

**它有断言，但三条断言一条都没测拉尺寸**（`Scripts/TinyGladeDemoRegression.py`）：

| 行 | 断言 | 测的是什么 |
| --- | --- | --- |
| `:126-136` | *10 translated reevaluations block the game thread zero times* | **纯平移**，而且 `:128` 的注释明写"沿路走（路是南北向的），门集合因此不该翻转" |
| `:144-152` | *a 12-dab stroke blocks the game thread zero times* | 落笔 |
| `:361-368` | *a 12-dab stroke with GPU stairs on ...* | 落笔 + GPU 石阶 |

三条都**没有改过 `FootprintSize`**。所以「房子重建零阻塞」这条结论今天只在
**摆位不变形状 / 地面落笔**两个场景下被证过，**拉尺寸从未被测量**。

**纯平移一栋已建好的房子确实是 0 flush。拉尺寸不是**，四个来源：

| # | 位置 | 触发条件 | 代价 |
| --- | --- | --- | --- |
| **R1** | `Body->EnsureCapacitySync`（`CSHouseActor.cpp:680`）、`PillarMesh->EnsureCapacitySync`（`:811`） | 房体/柱三角数随 footprint 增长（面板数、门槽 N、柱数都随之变）。**没有任何预留** | 每次超容量 **1 次计数 flush** |
| **R2** | **门框砖 handover**（`:913-928`） | `bNeedHandover` 的条件之一是 `FrameHandedLocalBounds` 与新 bounds 差 **> 1 cm**，而 `LocalBounds`（`:909-911`）**直接由 `FootprintSize` 与 `WallHeight` 算出** ⇒ **拉尺寸每超 1 cm 就重 handover 一次** | `SetInstanceSourceGPU` → `RebuildGpuMesh` 里有 `SetStreamLayoutSync`(`CSGpuInstancedMeshComponent.cpp:581`) / `ResizeStreamsSync`(`:609`) / `EnsureCapacitySync`(`:622`) + **无条件 `EditMeshSync`**(`:639`) ⇒ **最多 4 次计数 flush，逐帧发生** |
| **R3** | `CSShaperSteps::EnsureCapacity`（`:1092`） | 门槽 N 增加使砖数超 `FrameReserveCapacity`（默认 512） | `CSShaperSteps_GrowTo` → `CSGroundShaperSteps.cpp:84` 的**裸 `FlushRenderingCommands()`，不计数** |
| **R4** | 异步被"非在途原因"拒绝的同步兜底（`:704-712` / `:826-830`） | `EditMeshAsync` 返回 false 且不是在途 | 各 2–4 次计数 flush |

**R2 是最大的单点风险**：它不是"偶尔"，是**逐帧**。拖 1 m 的墙，按 60 fps、
每帧位移远超 1 cm ⇒ 每一帧 1–4 次 `FlushRenderingCommands`，正是计划
`:209-229` 那节要消灭的东西。

⚠️ **R1+R2 这个失败模式在本项目已经被实测到过一次**，就写在回归脚本的注释里
（`TinyGladeDemoRegression.py:350-357`）：

> *「⚠️ **必须先把旧路关掉再测**（实测：不关的话这条断言报 6 次刷新）。旧路的容量是按
> "本次排了多少条记录"算的，画路时逐笔变大 ⇒ 逐笔 `EnsureCapacity` 扩容 + 重走一次
> 阻塞的 `SetInstanceSourceGPU`。」*

**这就是 R1+R2 的同一个病，只不过换成了地形石阶。** 它当时的解法（S1：换成固定容量）
正是 [A3](#a3) 要对房体 / 柱 / 门框砖做的事——**已有先例、已知有效、已知不改就是 6 次/笔**。

另加两条纯 CPU 的逐帧代价（不是 flush，但同样落在拖动那一帧）：

- `ComputeSeatZ()`（`:224-233`）按 footprint 全域格点采 `(NX+1)*(NY+1)` 次 `SampleHeight`，
  **房子拉得越大越慢**（随面积增长）。
- `ComputeDoors()`（`:238-336`）每子段 `Steps = Pitch/DoorSampleStep + 1` 次双探测
  `SampleRoadWeight` + 一次 `SampleHeight`；N 随墙长增长。

#### 3.5 ⚠️ 观感风险：N 跨界那一帧滞回集体失效

`CSHouseActor.cpp:281`：

```cpp
const uint32 Key = (uint32(Edge) << 24) | (uint32(N) << 16) | uint32(Slot);
const bool bWasOpen = DoorSlotOpen.FindRef(Key);
bool bOpen = Coverage >= (bWasOpen ? SlotOffCoverage : SlotOnCoverage);
```

**`N` 在 key 里。** 拉尺寸让 `N = clamp(round(可用长 / DoorPitchTarget), 1, ∞)`
（`SplitEdgeIntoSlots`，`:366-379`）跨过整数边界的那一帧，该边**全部 key 失配**
⇒ `bWasOpen` 一律 false ⇒ 所有拱按**严阈** `SlotOnCoverage = 0.6` 重判（而不是宽阈 0.4）
⇒ **覆盖率落在 [0.4, 0.6) 的拱在那一帧集体关闭**，拖回去又开。
`DoorSlotOpen = MoveTemp(NewOpen)`（`:310`）还会把旧 N 的条目整批丢掉，所以来回拖每次都重演。

`CSHouseActor.h:478` 的注释写的是 *「N 进 key，墙长变化重排时不误继承」* ——
**这是有意的**（避免把 slot 5 的记忆错给重排后的 slot 5）。但它的代价
——滞回恰恰在最需要它的那一刻失效——没有被记下来。计划 `:292` 只写了
*「拖拽中拱重排、松手即稳」*，没提这一条。

---

### 四、本项目侧（D7）：肉眼可见的破绽

#### 4.1 墙角：精确对接，问题不是穿模而是 UV 岛断裂

> ⚠️ 2026-09-13 起转角改为斜接（`CSHouse_GetEdge` 每条边从外角点起、跑满外皮长），下面的 butt joint 实体范围与 UV 岛分析是 08-31 的状态。

`CSHouse_GetEdge`（`CSHouseActor.cpp:132-154`）——0/2 号墙跑满 `Footprint.X`、**独占四个角**；
1/3 号墙两端各内缩 `T`（`Len = Footprint.Y - 2T`）。四面墙的实体范围：

```text
Edge 0: X∈[-HX,+HX]      Y∈[-HY, -HY+T]
Edge 1: X∈[HX-T, HX]     Y∈[-HY+T, HY-T]
Edge 2: X∈[-HX,+HX]      Y∈[HY-T, HY]
Edge 3: X∈[-HX+T, -HX]   Y∈[-HY+T, HY-T]
```

**零重叠、零缝隙、精确 butt joint。** 破绽四条：

1. **外角 UV 岛断裂（主要）**。X = +HX 那个外表面由三块 quad 拼成：0 号墙的 +X 端盖
   （Y ∈ [−HY, −HY+T]）、1 号墙的外面、2 号墙的端盖。`AddQuad`（`:92-99`）的 UV
   **从 quad 局部 (0,0) 起算**，所以那两条 `T` 宽（默认 24 cm）的端盖是**各自独立的 UV 岛，
   横轴还是沿墙厚方向**。砖纹/灰泥纹**到角就断**。这正是角柱要盖的东西。
2. **硬着色 90° 棱**。`AddTri`（`:71`）逐面法线、三角汤（每次调用新开 3 个顶点），无平滑组。
3. **埋在实心里的重复面**。每角一对背靠背 quad（4 三角）被埋在角的实心体里，
   四角共 16 个三角白花；**不 z-fight**（背靠背且在实体内部）。同样的模式在每个面板边界上重演。
4. `CornerMargin`（默认 **60 cm**，`CSHouseActor.h:145-146`）**不产生任何几何**，
   只是给洞用的逻辑禁区（`SplitEdgeIntoSlots` `:366-379` 与 `QueryFeaturePlacement` `:352`）。
   而且它作用在 `F.Len` 上，1/3 号墙的 `F.Len` 已经是 `Footprint.Y − 2T`，
   所以 ±X 墙实际从真角起算保留 `T + 60`，±Y 墙保留 `60` —— **四个角的禁区不等宽**。

#### 4.2 墙-地：完全没有收口

`skirt | plinth | foundation | footing | base trim` 全 `Source/` **0 命中**
（唯一的 `Skirt` 是 `CSGroundShaperActor.cpp:119-122` 的径向衰减变量，无关）。

墙底是 **Z = 0 一刀平切**；房底 = `ComputeSeatZ()`（`:208-236`）= `max(footprint 全域地面高) + HeightOffset`。
坡地上房子踩最高点、其余地方**悬空**，唯一的桥接是离散方柱
（`PillarSize = 30`、`PillarSpacing = 250`、`PillarEmbed = 5`，`CSHouseActor.h:268-281`），
**柱与柱之间是通的**——从侧面能看到房子底下的天。TG 对位物就是 `wall_skirts::add_skirts`。

#### 4.3 墙-顶：三处刀口相切，零收边件

`fascia | verge | bargeboard | soffit | cornice | coping` 全 `Source/` **0 命中**。

| 位置 | 现状（`CSHouseActor.cpp:601-633`、`CSHouseRoof.h:66,94-97`） | 症状 |
| --- | --- | --- |
| **檐口** | `Z(HalfSpan) = EaveZ = WallHeight`，屋面板底面**只擦过墙外顶棱一条线**；往内屋面抬升 | 墙顶面（Z=H）与屋面底之间留楔形空隙，外侧 0、内侧 `T·tan(35°) ≈ 24×0.70 ≈ **17 cm**。**从室内看是贯通两面檐墙的一条开口**，无物封闭 |
| **山墙斜边** | 山墙棱柱的斜边与屋面底**恰好共面**（零余量） | 任何浮点/阴影偏置都会露出**发丝缝** |
| **屋脊** | `SlopeLen = \|Slope\| + RoofThickness*0.5` 故意过冲，两板互穿 | 各自尖端露出对方顶面约 `RoofThickness·sin(35°) ≈ 12×0.57 ≈ **7 cm**`，形成交叉小尖。**没有脊瓦** |
| **山墙端出挑** | `LAtot = LA + 2*RoofOverhang` | 自由边无任何收边 |

TG 对位物：`generate_roof_stone_floor_and_roof_bottom` + `_nani_roof_floor` 的
`is_roof_bottom` + Y 量化防 z-fight；`GableRoofStyle::is_stepped` 的阶梯山墙。

#### 4.4 接缝 / 角柱代码：0 命中

`Seam` / `ACSHouseSeamActor` / `CornerPillar` / `Stitch` / `Merlon` / `Quoin` / `Buttress` /
`Cornice` / `Coping` 全 `Source/` 0 命中。`Public/` 下只有
`CSHouseActor.h` / `CSHouseProfile.h` / `CSHouseRoof.h` / `CSHouseSubsystem.h`，
**计划 `:77` 列的 `CSHouseSeamActor.h` 不存在**。

`Pier` 存在但是**另一回事**——同一面墙上两拱之间的墩（`PierWidth = 40.0f`，
`CSHouseActor.h:140-142`，`CSHouse_OpeningCell` 消费），不是转角。

#### 4.5 材质槽与资产

- **房体只有 2 个槽**：`constexpr int32 SlotWall = 0, SlotRoof = 1;`（`CSHouseActor.cpp:543-545`，
  硬编码、无 enum），**山墙也走 SlotWall**。加第 3 槽只需给
  `ACSTinyGlade::BindTinyGladeMaterials`（`CSTinyGlade.cpp:34-45`）传第三个元素，
  分段排序 pass 按 `Materials.Num()` 自动跟。
- **部件身份走顶点色 R**：`ECSHousePart { Wall=0, Roof=1, Gable=2, Frame=3, Pillar=4 }`
  （`CSHouseActor.h:21-30`；通道字典在 `:48-67`，自称"全项目唯一仲裁点"）。
  **角柱应取 5**。
- **砖资产已在**：`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick`（100³ 中心立方，`[UE资产]`），
  已经是 `FrameBrickMesh` 的默认目标；`EnsureFrameComponent`（`:889-898`）已从它的 bounds
  推 `BlockSize`。**角柱 / 裙砖零新资产。**
- **`Content/HouseTest/TinyGladeAsset/` 里同样没有任何角柱 / 护角 / 墙裙 / 脊瓦 / 收边预制件**
  （474 个网格 grep 过 `pillar|column|quoin|plinth|skirt|trim|coping|capping|merlon|stitch|pier|post|block|base`，
  全部 0；`corner` 的 24 个命中全是 UI 箭头 / 旗杆 / 绕角窗）。与 TG 一致 —— **本来就该用砖砌**。
  勉强可用的线性件：`roof_support_beam`、`wooden_plank_straight`；capping 形状最近的是
  `setdressing_window_sill` / `setdressing_window_lintel`。
- **`PanelCell` 至今全代码库零实现**（复核确认；计划 `:160`、`:322` 已记）。

---

### 五、行动清单

排序按「先把纪律钉死 → 再补最难看的 → 最后做新功能」。工作量按"改动行数 + 需要新写的测试"估。

<a id="a1"></a>
#### A1 —— 补一条**拉尺寸**的零阻塞断言，**动 D5 之前必须做**

**工作量：小（约 20 行，加进现有回归脚本）** · **风险：低**

现有三条 flush 断言只覆盖平移与落笔（§3.4）。
照 `TinyGladeDemoRegression.py:126-136` 的形态加第四条：建一栋**有门**的房 → 记基线 →
连续 30 次 `set_editor_property("FootprintSize", ...)` 每次 +5 cm（模拟拖动）+
`reevaluate_site()` → 断言 `flush_delta == 0`。

**这条断言现在一定是红的**（R1/R2/R3 三条随便哪条都会让它红），红着也要先加——
它是 [A2](#a2) / [A3](#a3) 的唯一验收判据。注意照抄 `:120` 那条纪律：
**必须在有门的非空状态下测**，否则"就算摆位路径整个坏掉也照样绿"。

顺带把 `CSGroundShaperSteps.cpp:84` 的裸 `FlushRenderingCommands()` 换成计数版
（或给 `CSShaperSteps` 导出自己的计数器），否则 R3 那条**永远测不到**。

<a id="a2"></a>
#### A2 —— 修 R2：门框砖 handover 别拿 footprint 当 bounds

**工作量：小（约 15 行）** · **风险：低** · **收益：最大**

`CSHouseActor.cpp:909-911` 把 `LocalBounds` 直接从 `FootprintSize`/`WallHeight` 算，
于是拉尺寸每超 1 cm 就重走一次阻塞 handover（最多 4 次 flush，**逐帧**）。
两个方向任选：① bounds 只在 `PostEditMove(bFinished)` 或 `bForceFullRebuild` 时更新，
拖动期沿用旧 bounds（略大的包围盒只影响剔除，不影响正确性）；
② 一次性把 bounds 放大到 `FootprintSize` 的若干倍并量化（比如按 100 cm 台阶取整），
拖动中跨不过台阶就不 handover。**②更省事且与 `DoorWidthQuantum` 同型**。

<a id="a3"></a>
#### A3 —— 修 R1/R3：房体 / 柱 / 砖三处都要预留容量

**工作量：中（约 60 行）** · **风险：低**

计划 `:224` 已定纪律 *「容量则预留足量、只在真正不够时接受一次同步扩容」*，
但今天房体与柱**一点预留都没有**。做法：`SubmitBodyMesh` 之前按
`FootprintSize` 的**量化上界**（比如向上取到 100 cm 的整数倍）估三角数，
一次 `EnsureCapacitySync` 到位；拖动中跨不过量化台阶就不再扩容。
`FrameReserveCapacity`（默认 512）同理，按 `SplitEdgeIntoSlots` 在量化上界下的 N 估砖数。

<a id="a4"></a>
#### A4 —— D5 抓手形态：先拍板再动工

**工作量：中～大（取决于形态，见 [C-D5-2](#c-d5-2)）** · **风险：中**

无论选哪种形态，这几件事是共同的、可以先做：

- `ACSHouseActor` 加纯函数 `CSHouse_ApplyEdgePush(FVector2D& InOutSize, FVector& InOutCenter,
  int32 EdgeIndex, float Yaw, float Offset, float MinFootprint)` + 计划 `:257` 那两条单测
  （连续 10 次 `Offset=0` 不变量；单次 `Offset=Δ` 后对侧墙世界位置不变、被推墙恰好移动 Δ）。
  **纯 CPU、零 GPU、零编辑器依赖**，先落地先受益。
- 拖动期调 `UCSHouseSubsystem::MarkHouseDirty`（今天零调用者）而不是等 0.25 s 扫描。
- 松手 `PostEditMove(bFinished)` 已经会置 `bForceFullRebuild`，浮点累积对齐已有。

**TG 形态额外需要**：一条 `PickHouseEdge(Ray) → (EdgeIndex, S, Z)` 的解析求交
（`CSHouse_GetEdge` 已给出四面墙的框架，射线 vs 4 个 OBB，几十行），
**不需要 trigger volume 网格**——本项目的墙本来就是解析盒。

<a id="a5"></a>
#### A5 —— 抄砖胀大：`SolveBlockLayout` 的产物加一个 `OverlapScale`

**工作量：小（约 10 行 + 材质侧 0 行）** · **风险：低** · **收益：高**

TG 的 `flags&4` 分支给砖沿墙 / 穿墙 ×1.045、竖直 ×0.95（§1.3）。
本项目 `SolveBlockLayout`（`CSSplineBlockActor.cpp:162-204`）语义与 TG 的
`split_horizontally` 完全同型（**变数量、近定距**：贪心铺到超长，再在"留最后一块压缩"
与"丢最后一块拉伸"之间取 `|log(scale)|` 小的），**唯独没有这一步胀大**。
门框砖今天靠 `FrameBrickGap = 1.5` 留正缝，缝是正的 ⇒ 块数变化时会露。
在 `CSShaperSteps::Scatter` 的记录里给每块加一个按 `seed` 哈希的 (1.045 / 0.95) 因子即可，
**GPU kernel 里 `Rec.y` 已经是 lengthScale，改法是乘一个哈希项**。

<a id="a6"></a>
#### A6 —— 墙-顶三处收口（**观感收益最大，且与 D5/D7 都不冲突**）

**工作量：中（约 120 行，纯 `RebuildBodyMesh`）** · **风险：低**

三处各自独立、都能单做：

1. **檐口封口板**：在墙顶面（Z=H）与屋面底之间，沿两面檐墙各补一条楔形封板
   （外侧高 0、内侧高 `T·tan(pitch)`）。把 `ECSHousePart` 加 `Eave`，走 SlotWall。
   **这条修掉的是"从室内看得见天"的功能性破绽，不只是观感。**
2. **山墙斜边留余量**：把山墙棱柱的斜边沿屋面法线**内缩** 0.5 cm，
   消掉零余量共面导致的发丝缝（与 TG 的 `floor(y*512)/512 + fract(wall_id*1.618)/512`
   同一思路，但本项目不需要 per-wall 抖动，一个常数就够）。
3. **脊瓦**：沿脊铺一条 `brick` 实例带（复用 `CSShaperSteps::Scatter`，
   一条直线曲线、零新资产），盖住两板互穿露出的 7 cm 交叉尖。

<a id="a7"></a>
#### A7 —— 转角护角砖（D7 的"墙自身转角"部分，**不依赖任何接缝设施**）

##### 2026-09-12：按参考图修正包角

![TG 参考：浅凸于灰泥墙面的交错包角石](img/tiny-glade-ref-corner-quoin.png)

图片由用户提供，原样保存。可见的角石沿两面墙展开，外棱连续、表面浅凸于灰泥；相邻层交换长短边，砖长和层高有少量变化。旧模型把砖心放在外角、整块砖朝向角平分线，只缩小位移仍会留下斜着突出的砖柱。

**重新反汇编后的订正。** 本次直接读取 `tiny-glade.exe` 的 `add_wall_corners`（`0x141215540–0x1412172A6`），得到以下证据：

| 指令位置 | 计算或数据流 | 对应含义 |
| --- | --- | --- |
| `0x141215E72–0x141215E8E` | `0.92 + 0.16 × (2r − 1)`，随后送入矩形构造的宽度参数 | 随机改变长边，不能解释成整块砖的对角线位移 |
| `0x141215DD6`、`0x141215EBB`、`0x141215F0E` | 从 `[rbp+0x190]` 读取层号，递增保存，再 `test sil,1` | 这里确实检查层号奇偶；旧代码“所有 test 都是 Rust bool”的注释有误 |
| `0x141216973–0x1412169A0` | 同一个层号的奇偶分支，选择之前算出的两条相邻边方向 | 长边逐层切换墙面方向 |
| `0x141215F26–0x141215F38`、`0x141216BB3` | 半宽减 `0.28 + 0.0385`，另一水平轴厚度 `0.637` | 宽度变化同时补偿中心；`0.637/2 = 0.28+0.0385`，两面外棱保持锚定 |
| `0x141215C41`、`0x141215C93–0x141215CBD` | 标称高度 `0.69`；`random_splits` 输入 `0.21 / 高度` | 随机分层，端点保持固定 |

[保留的反汇编片段及原始二进制 SHA-256](evidence/quoin-native-20260912.asm)可用于复核。上表是本次二进制复核结论。矩形外角锚定到本项目 footprint，以及按现有层高缩放，是 UE 侧的适配。TG 函数还存在屋顶/墙型条件下的 `0.76`、`0.56` 宽度分支；本次没有把尚未对齐的墙型枚举擅自映射到本项目。

**当前实现。** 共用 `brick` 网格、`AppendColumn`、`SolveRun` 和 GPU 实例组件。`CSHouseQuoinLayout.ush` 让 C++ 回归与实际 shader 共用包角变换；层号采用砖路内序号，前方增加门框槽位不会翻转交错方向。宽度随机只移动埋入墙内的中心，外侧两面的位置保持不变。

源尺寸按 `FrameBrickLength / 69` 缩放到本项目。当前 `FrameBrickLength=26 cm` 时，短边约 `24.0 cm`，长边 `34.67 ± 6.03 cm`，两面各浅凸约 `1.45 cm`。`QuoinJitter=16`、`QuoinSplitJitter=21` 保留为 TG 基准厘米，实际分别换算为 `6.03 cm`、`7.91 cm`；`QuoinInset` 则仍是 UE 世界厘米。`Inset=0` 表示正常包角，正值进一步压入墙面。实例层高沿用现有砖路和长度膨胀系数。

角石分支使用砖路记录原有的空格存比例；无剪切的竖直角石在 `R6.yz` 存基础网格 X/Z 尺寸倒数，普通门框仍在这里存端头剪切。总 stride 保持 8 个 float4，逐实例容量与洞口剔除高度保持原契约。宽度随机、分层随机和种子均纳入重新散布的哈希。

**专项渲染核对（1600 × 1000）。** 下图为修正后的贴墙包角。两栋房子的转角、门框和底部支撑已分别检查。

![沿两面墙交错的包角石](img/quoin-reference-20260912-after.png)

房屋整体最新结果见 [屋顶文档](Roof_TerraceAndRidge.md#截图)。

验收：按 Rider 工作区配置推断使用 `UETest574Editor / Win64 / Development`，常规模块编译通过；重新启动编辑器后，关卡 24 个 actor 全部恢复、两栋房子的角石数分别为 72 / 68。角石覆盖、比例、稳定随机数、容量截断、门框重叠、拱间墩与支撑接触共 9 项回归全部通过。以上修正后截图来自常规编译后的重新启动，并已核对文档中的图文显示。

下面保留 2026-08-31 的落地记录；几何朝向和随机含义以本次订正为准。


> ✅ **已落地（2026-08-31）** —— `CSHouseQuoin.h`，见卷零「转角角石已落地」。落地时抓到并修掉了
> 一个真缺陷（`Seed ^ (Index*K)` 在 `Index==0` 时是恒等映射）。
>
> ⚠️ **本条的措辞已被[卷五 A11](#vol-5) 订正**（2026-08-31）：转角砖不只是「和门框砖共用一路」，它和垛口、勒脚、门框墩在 TG 里**共用同一个排布求解器** —— 落地时并进 `SolveBlockLayout`，别另起一套。

**工作量：中（约 100 行，复用 `BuildFramePlan` 形态）** · **风险：低**

对位 TG 的 `wall_corners::add_wall_corners`。在四个角各立一根**叠砖角柱**，
从 Z=0 砌到 `WallHeight`（或到起拱线），截面略大于 `WallThickness` 以盖住
§4.1 的 UV 岛断裂与硬棱。

- **资产零新增**：`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick`，
  链路 `CSShaperSteps::{FCurve, ReserveCapacity, EnsureCapacity, Scatter}` +
  `UCSGpuInstancedMeshComponent` —— **与门框砖同一条**（`RebuildFrame` / `BuildFramePlan`
  `CSHouseActor.cpp:1066-1105` / `:935-1064` 是可逐行照抄的模板）。
- **曲线是一条竖直线段**，天然等弧长 ⇒ **不踩 `ResampleUniform` 那个坑**
  （门框的 U 形路径踩过，见进度文件「踩过的坑」末条）。
- **块列从柱底锚定**（计划 `:322` 的纪律），避免房子拉高时整柱重排。
- `CornerMargin` 的四角禁区不等宽（§4.1 第 4 条）
  顺手订正：改成从真角起算。
- ⚠️ **受 [C-D7-2](#c-d7-2) 与「门框砖建在将被删除的设施上」两条待拍板影响**——
  如果门框改走 100% GPU 解析推导，角柱应该一起改，别先写一套 CPU 规划链。

<a id="a8"></a>
#### A8 —— 墙裙（`add_skirts` 的对位物）

> ✅ **已落地（2026-08-31）** —— `CSHouseTrim.h`，做成了**两条带**（墙顶压顶 + 墙脚勒脚），
> 见卷零「包边石已落地」。勒脚按洞切段，判据是段数不是砖数。

**工作量：中（约 80 行）** · **风险：中（与 D9 承重柱的归属要划清）**

房子踩 footprint 最高点、其余悬空（§4.2），柱与柱之间通天。
沿四面墙外周铺一圈**贴地砖带**：每块砖的底 = 该处 `SampleHeight`、顶 = 房底 Z，
高度为 0 处不生成。与 D9 承重柱**不共享数据、不共享组件**（同 D6 拱间墩 vs 承重柱的纪律），
但视觉语言相同（都是叠砖）。

⚠️ 这条会让 `ComputeDoors` 的 `GapMax`（离地收窄）与视觉产生歧义——
裙砖填上以后门看起来贴地了，宽度却还在按落差收窄。**做之前先想清楚裙砖算不算"地"。**

<a id="a9"></a>
#### A9 —— D7 接缝：先做"检测 + 开洞"，角柱后置

**工作量：大** · **风险：高（形态待拍板）**

TG 的顺序是**先开洞、再缝砖**（§2.2），
计划 D7 只有后半段。而且 TG 的实现是两个各自两三个参数的纯函数系统，
计划 D7 是一整套 actor + 弱引用 + 自 tick + `EnsureRefreshed(Gen)` + 邻近合并。
**两条冲突都未拍板（[C-D7-1](#c-d7-1) / [C-D7-2](#c-d7-2)），不要在拍板前动工。**

可以先做的无争议部分：把「两个 OBB 求轮廓交点 + 按重叠区质心极角排序」抽成
`CSHouseLogicTests` 里的纯函数（计划 `:352` 已经写清算法，16 次线段求交），
它在任何形态下都要用。

<a id="a10"></a>
#### A10 —— 修 3.5：N 跨界时的滞回失效（**可能不修，先记着**）

**工作量：小（约 10 行）** · **风险：低**

如果拍板认为"拉尺寸时拱集体开关一下"可以接受，就只在
`CSHouseActor.h:478` 的注释里补一句代价，**不改代码**。
如果要修：在 key 失配时用**沿墙的绝对位置**回落查一次旧表
（旧 N 的哪个 slot 覆盖了新 slot 的中点），把那个 slot 的开关状态继承过来。
这会让 `DoorSlotOpen` 需要多存一帧旧表（`Local<Vec<...>>` 的对位物）。

---

### 六、历史冲突与裁决记录

本节保留原对照发现；门洞触发、门框设施、C1、C2、C-R1 与连续脊长均已有后续裁决，当前结论见卷零。

<a id="c-d5-1"></a>
#### C-D5-1：连续脊长（已关闭）

TG 的 `roof_shape::ridge_length_01_from_rectangle_ratio`（PDB）说明脊长随矩形长宽比连续变化。
2026-08-31 用户采用这一侧：本项目改为四坡屋顶和派生脊向，正方形处脊长收为 0。
离散翻轴、滞回、尺寸禁带与原三选一方案已删除，不再等待拍板。

<a id="c-d5-2"></a>
#### C-D5-2：D5 抓手形态——handle actor vs 拖墙本体

计划 `:241-264` 裁决的是「生成挂在房子下的临时 handle actor + 标准 transform gizmo」。
TG 是「**拖墙本体** + 解析 trigger volume + 尺寸线 signifier」（§1.1）。

**这不是"计划错了"**，两条路都成立。但有一条客观事实值得放进决策：
计划 `:248-259` 用了 12 行 + 一个递推推导 + 两条单测来处理
*「不加记账量的话，墙的位移是鼠标位移的 2 倍」* 这个父子回路缺陷——
**该缺陷的成因是 handle 作为子 actor 会被父级移动带走**，在 TG 形态下
（没有 handle actor、没有父子关系）**根本不存在**。

另一侧的账：TG 形态需要自己实现悬停高亮 + 拖拽状态机（TG 有
`UiEditRectangleActiveWidgetResource` / `EditingHighlight` / `SignifierMinimumLifespan`
三件套），而 handle actor 形态白嫖编辑器原生 gizmo 与选择系统。
本项目 gpumesh 全线 `NoCollision`，两条路**都**必须自己写解析求交
（`CSHouse_GetEdge` 已给出四面墙框架，射线 vs 4 个 OBB）。

**未拍板前不动 D5 主体**；[A4](#a4) 里那条 `CSHouse_ApplyEdgePush` 纯函数两条路都要用，可以先做。

<a id="c-d7-1"></a>
#### C-D7-1：相交处开洞的证据与裁决

TG 的 `detect_intra_shape_corners` 读取 / 写入 `WallHoles`，并调用 `add_hole_at_shape_intersection`（PDB 行 8854）；缝砖在洞缘生成。
本项目禁止重切真几何洞，采用渲染层 clip；旧的“只立柱 / 真开洞 / 深侵入才开洞”三选一方案撤销，当前规则见计划 D7。

<a id="c-d7-2"></a>
#### C-D7-2：接缝不建独立 actor（已关闭）

TG 的证据链是 `detect_intra_shape_corners` → `IntraShapeCorners` → `InterShapeBrickStitches` → `spawn_stitch_bricks`，实例写入共享的 `StitchBrickAlloc`。
本项目已否决 `ACSHouseSeamActor` 方案；`UCSHouseSubsystem` 也已删除，不能再按旧建议把接缝网格挂到它上面。
当前是两端共持的接触记录与各房子的派生几何，世界查询由 `UCSHouseLibrary` 提供。

---

### 七、明确不该抄

| TG 的做法 | 为什么不抄 |
| --- | --- |
| `WallTriggerVolume` 的四边形壳 + `get_position_from_mesh_uv` | TG 的墙是任意曲线，需要一层代理网格才能做射线拾取。本项目的墙是**解析矩形盒**，`CSHouse_GetEdge` 直接给出四个 OBB，射线求交几十行，**不需要任何代理网格** |
| `DecoratorArchivist` 的归档 / 恢复 | 本项目 D8 的特征标记 actor 常驻，`QueryFeaturePlacement` 判不可行就不出几何——**归档语义天然就有**，不需要另建一套 |
| `merge_walls` / `wall_splitter` / `snapping_*` | 那是 freehand 曲线墙的拓扑合并。本项目的房子是参数化矩形，没有"两面墙首尾相接"这个概念 |
| `SparseInstanceBuffer::take_out_waste` | 本项目 `UCSGpuInstancedMeshComponent` 走 `SetInstanceSourceGPU`（GPU 产实例、零回读），容量策略应该是"预留 + 量化"（[A3](#a3)），不是"回收 waste" |
| TG 的 `Local<Vec<ShapeIntersectionHole>>` 差分 | 本项目已有两级哈希守卫（形状 / 摆位），形态更强也更省内存 |

### 八、待确认

| 问题 | 需要什么证据 |
| --- | --- |
| 墙变长时**既有砖是否原地不动** | 逐帧录屏：拖一条边，看远端砖缝有没有跟着走。PDB 只能看到 `init_rng/init_rng_u64`，判不了种子是否绑绝对弧长 |
| TG 的"接缝空间过小则不生成缝砖"是否存在阈值 | 计划 `:378` 的 `SeamMinExposure = 40 cm` 是本项目自订。TG 侧未找到对应符号，可能在 `stitch_bricks` 的常量里（PDB 不含常量） |
| `wall_corners::add_wall_corners` 到底加几块砖、加在哪 | 只有函数名和它在工具链里的位置（在裁洞之后、加裙之前）。要确证需要反编译该函数 |
| `is_stepped` 阶梯山墙的台阶模数 | 同上 |
| 本项目 `ComputeSeatZ` 在大 footprint 下的实测耗时 | 需要跑一次 profile；本文只从代码结构（`(NX+1)*(NY+1)` 次双线性）指出它随面积增长 |

---

<a id="vol-2"></a>

## 卷二 · 窗户（D8）与装饰／藤蔓（D12/D13）对照

> 原文件 `TinyGlade_窗户与装饰对照.md`，原标题「Tiny Glade 窗户（D8）与装饰／藤蔓（D12/D13）对照」。

本文只回答四件事：**TG 的窗户走的是不是门那套机制**、**窗户由什么触发**、
**本项目现有的洞格式与排布逻辑差在哪**、**TG 的装饰物与藤蔓分别绑在什么载体上、密度谁定**。

设计裁决在 [`Plan_House.md`](Plan_House.md) 的 D6 / D8 / D12 / D13，本文**不复述**它，
只写对照结论与新证据；发现冲突一律写进第八节「待用户拍板」，**不擅自改裁决**。
体例与详略口径对齐 [本文卷三 · 楼梯模块对照](#vol-3)。

### 证据标注约定

沿用 `MESH_GENERATION_ANALYSIS.md` 的三档可信度，并给每条标出**证据种类**：

| 标注 | 含义 |
| --- | --- |
| 【确凿】 | PDB 符号 / 反汇编 GLSL / 资产字节 / 本仓源码直接给出，不含推理 |
| 【推测】 | 由确凿证据推理得出，推理链在正文写明 |
| 【待确认】 | 现有证据不足以判定，正文写明「需要什么证据才能确证」 |
| `[PDB]` | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt`（97032 行）；系统签名按状态文件记的解码口径取 |
| `[PATH]` | `D:/MyProject/Tiny Glade/tmp/pdb_paths.txt` 里的 `crates/**/*.rs` 源码树 |
| `[GLSL]` | `D:/MyProject/Tiny Glade/tmp/shaders/`（272 个反编译着色器） |
| `[资产]` | `D:/MyProject/Tiny Glade/assets/meshes/**.json` 逐顶点实测（TG 单位为米） |
| `[UE资产]` | `D:/MyProject/UnrealProject/UETest574/Content/HouseTest/TinyGladeAsset/` 实际文件清点 |
| `[代码]` | 本仓 `Source/ComputeShaderGenerator/` |
| `[分析]` | `D:/MyProject/Tiny Glade/MESH_GENERATION_ANALYSIS.md` |

⚠️ 一处易误采：`country_core::resources::window::*` 与 `WindowSize` / `OsWindowCmd` 是**操作系统窗口**，
与建筑窗户无关。本文引用的窗户符号一律在 `system_decorator::` 或 `walls::decorator*` 命名空间下。

轴向换算（沿用楼梯对照第七节已验证的口径）：TG 是 glTF Y-up，UE 导入换轴 + ×100 ⇒
**UE (X, Y, Z) = TG (X, Z, Y) × 100 cm**。

---

### 一、窗户不是门那套机制 —— 两边都不是，而且本项目其实更强

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#一窗户不是门那套机制--两边都不是而且本项目其实更强)**（2026-09-06 文档重组）。含 1.1 / 1.2 / 1.2b / 1.2c / 1.3 / 1.4 六个子节。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。
> 别处引用的「卷二 §1.2」「卷二 §1.3」「§1.4」落点都在那篇里。

---

### 二、窗户的触发规则：**玩家手放**，不是墙面剩余空间自动填充【确凿】

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#二窗户的触发规则玩家手放不是墙面剩余空间自动填充确凿)**（2026-09-06 文档重组）。含 2.1 / 2.2 / 2.3 / 2.4 / 2.5 五个子节。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。

---

### 三、`FCSWallOpening` 够不够表达窗户：够挖洞，不够摆框

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#三fcswallopening-够不够表达窗户够挖洞不够摆框)**（2026-09-06 文档重组）。含 W1–W6 那张表（包括已作废的 ~~W4 跨边转角窗~~）与「不缺的两样」。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。
> 别处引用的「卷二 W2」「卷二 W4」落点都在那篇里。

---

### 四、`CSHouse_OpeningCell` / `SolveBlockLayout` 要怎么给窗户让位

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#四cshouse_openingcell--solveblocklayout-要怎么给窗户让位)**（2026-09-06 文档重组）。含 4.1 / 4.2（P1–P4）/ 4.3（标题带删除线的作废整节，保留）。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。

---

### 五、装饰物（D12）：TG 是**锚点驱动的 region**，不是复杂度场

#### 5.1 载体：不绑地面，绑**已经存在的建筑特征**【确凿】

`system_clutter::autoclutter` 是一个三段状态机 `[PATH]`：

```text
populate_autoclutter_regions  →  process_autoclutter_candidates  →  manage_autoclutter_entities
```

`AutoClutterState::{add_region, mark_dirty, remove_stale_regions,
flush_candidates_pending_removal, add_region_to_lazy_grid, remove_region_from_lazy_grid}`
+ `LazyGridClutterCell` + `AutoClutterRegion` + `AutoClutterCandidate` + `ClutterRegionId` `[PDB]`。

**region 的生产者共七家**，每一家都以某个已有物件为锚 `[PDB]` 系统签名：

| `[PDB]` 行 | 生产者【推测，按参数唯一匹配】 | 锚在什么上 | 读什么 |
| --- | --- | --- | --- |
| L8835 | `add_autoclutter_around_windows` | **已放置的窗 decorator** | `CachedDecoratorTransforms` · `DecoratorStorage` · `PublicWalls` |
| L9086 | `add_autoclutter_on_windows` | 同上，且**写** `WindowAutoClutterCandidates` | `ClutterMeshes` |
| L9088 | `add_autoclutter_around_gates` | `Query<&WoodenGate>` | — |
| L9200 | `add_birdnests` | `BirdNestCandidateLocations`（碰撞系统产） + `Query<&Roof>` | `BirdNestLazySubscriber` |
| L9084 | `add_lantern_water_setdresssing` | 灯笼 decorator + `WaterRaster` | `DecoratorStorage` |
| L9085 | `add_tree_vegetation_setdressing` | `Query<&TreeComponent>` + `WaterRaster` | — |
| L8877 | `add_preplaced_autoclutter` | 存档/开局预置 | `PreplacedAutoClutter` + `NewSessionStartedCmd` |

⇒ **一件也不是「在地面上找一块空地摆东西」。** 全部是「某个东西已经在那儿了，围着它摆」。

#### 5.2 密度与位置：**烘在资产里的候选点** + 有序解算 + 一次 GPU 回读【确凿】

三条各自独立：

1. **位置烘在资产里**。`window_cottage_*_flowerbed_locations.json` 是**零三角形的纯点集**
   （1/2/3 个顶点带法线）`[资产]`。`add_autoclutter_on_windows` 写
   `WindowAutoClutterCandidates`（`WindowAutoClutterCandidate` 单数型也在 `[PDB]`）
   ⇒ **花箱挂在哪由美术在窗户模型里点出来，不是程序算的。**
   实测坐标（UE cm，窗户局部空间）：

   | 装饰件 | 候选点 |
   | --- | --- |
   | `window_cottage_1x1` | `(0, 0, −70)` |
   | `window_cottage_2x1` | `(−32.75, 0, −70)` · `(32.75, 1, −70)` |
   | `window_cottage_3x1` | `(−68.47, 19.66, −70)` · `(68.47, 19.66, −70)` · `(0, 38.07, −70)` |
   | `window_cottage_corner_1x1` | `(0, 21, −65)` |
   | `window_cottage_corner_2x1` | `(±34.19, 32.43, −65)` |
   | `window_cottage_corner_3x1` | `(±66.69, 22.41, −65)` · `(0, 47.94, −65)` |

   （这与 `[一轮] L320` 已经记过的结论一致，本文只是补上实测坐标。）
2. **解算显式有序**：`process_autoclutter_candidates::OrderedCandidate` `[PDB]`
   ⇒ TG 自己也把候选排了序才逐个放。**与本项目 D12「层内处理顺序钉死为格坐标字典序」是同一条纪律，
   而且是被验证过的，不是发明出来的。**
   `[PDB]` L8822 的读集是 `ClutterMeshes · RaycastWorld · TerrainHeightsData · ActiveSession`
   ⇒ 重叠测试走物理查询、落高查地形。
3. **只有一处 GPU 回读**：`read_bush_positions_from_gpu` → `BushGrid` / `BushGridCell` /
   `bushes_near_point` `[PDB]` L9090。灌木是花园 compute 在 GPU 直接生成的（`[分析]` §6.2），
   杂物要避开它们，所以把位置**读回 CPU** 建成空间格。
   ⇒ **本项目 D12「场存 GPU + 异步回读 + CPU 放置」的分工在 TG 侧有先例。**
4. **失效不是 dirty box，是 mask 订阅**：`populate_autoclutter_regions::autoclutter_subscribe_to_tabloids`
   + `AutoClutterDirtySubscriptions`，`[PDB]` L9089 对 `BushGrid · RaycastWorld · TerrainHeightsData
   · GardenRaster · PathRaster · WaterRaster` 全部取 `ResMut`（轮询变更）
   ⇒ 就是 `[分析]` §6.5 那张 mask 总线。

#### 5.3 用户可删、可提升为己有：TG 的 `SuppressedDerived` 实证【确凿】

- `ui_clutter::DeletedAutoClutter` `[PDB]`，被 `manage_autoclutter_entities`（L9087）与
  `add_birdnests`（L9200）**当输入读**。
- `ui_convert_autoclutter_to_decoclutter` / `DecoClutterLock` / `ShowPinnedDecoClutter` /
  `decoclutter_show_pins` `[PDB]` ⇒ 玩家动过的自动杂物会被**提升成玩家所有**并钉住，
  不再随重生成 churn。
- `clutter_meshes::{ClutterMarkedForDeletion, DecoratorAutoClutterChildren,
  DisplayChildAutoClutterInfo}` `[PDB]` ⇒ 装饰物与它的子杂物有显式父子表。

⇒ **这与楼梯对照 A6 的 `SuppressedDerived`（`StairsRemovedSupports`）是同一个形状的第二个实例。**
本项目 D12「边界翻转 churn……不可接受再给已存在实例加保留加成」那条风险，
TG 的答案是**钉住 + 抑制集**，不是迟滞。

#### 5.4 与本项目 D12 的分歧（**冲突，写进第八节**）

| | Tiny Glade | 本项目 D12 计划 |
| --- | --- | --- |
| 位置怎么来 | 锚点（窗/门/树/水/屋顶）+ **资产里烘的候选点** | **复杂度场 `RT_DecorField`**（GPU 全网格 dispatch）+ tile-argmax |
| 密度怎么定 | 锚点数量决定（有几扇窗就有几个花箱位） | 场的 Z/W 通道 + 阈值 + tile 尺寸 |
| 朝向 | 由锚点自带（窗的法线） | 场的 XY 通道（最近墙段外法线的距离加权混合） |
| 排除 | mask 订阅 + 物理重叠 + `DeletedAutoClutter` | `RT_DecorMask` 双版（清晰/膨胀）+ CPU 谓词 |
| 用户否决 | `DeletedAutoClutter` + 提升为 decoclutter | 未设计 |

两套都自洽，**但它们回答的不是同一个问题**：TG 的 autoclutter 是「给已有物件配套」，
本项目的 D12 是「在房子周围晕开一圈复杂度」。**后者 TG 里没有对位物** ——
TG 里房子周围的东西要么是玩家自己放的 decoclutter，要么是 `add_preplaced_autoclutter` 的开局预置。

⇒ 本项目 D12 的复杂度场是**自有设计**，不是抄 TG。这一点计划里没写清楚
（D12 一节通篇没提 TG，但整份计划的开篇口径是「实现 Tiny Glade 式」）。
**建议订正措辞，不建议改做法** —— 复杂度场能做到 TG 做不到的「房子一动，摊子跟着挪」，
而 TG 的锚点法则能做到复杂度场做不到的「花箱精确挂在窗台上」。**两者可以叠加**（见第九节 A7）。

---

### 六、藤蔓（D13）：TG 是**玩家画的 + CPU 逐帧增量生长**

#### 6.1 触发：光标 + 输入动作，不是自动【确凿】

`[PDB]` L9009（按 `IvySpawnNoise` 唯一匹配）= **`ivy_spawner`**：

```text
读: CursorTerrainRaycast · ActionSetStates · GladeSettings · IvySpawnNoise
    WallSpatialHash · PublicWalls
写: IvyStorage
```

**`ActionSetStates` = 输入动作状态、`CursorTerrainRaycast` = 光标打在地形上的点**
⇒ **藤蔓由玩家用笔刷点/画出来**，`IvySpawnNoise` 只是给起点加抖动。

`[PDB]` L9010 = **`ivy_grower`**：

```text
读: ActionSetStates · GameTime · WallSpatialHash · PublicWalls · PrevWallHoles
    Local<IvyDirectionProposer> · RemoveIvyCmd(事件) · UiSignifierStream
写: IvyStorage;  发 IvySegmentDoneGrowingMsg
```

两条读法：
① **`GameTime` 在读集里 ⇒ 逐帧增量生长**，与本项目「解一次、成品一次到位」是两种东西；
② **`PrevWallHoles` 在读集里 ⇒ 藤蔓生长时避让墙洞**【确凿】——
`[分析]` §6.4 记了 `intersect_ivy_growth_w_wall_segment` 与 `check_for_wall_jump`，
**漏了「藤蔓读洞表」这一条**。

`[PDB]` L9149 = **`ivy_leaf_spawner`**：`IvySegmentDoneGrowingMsg` → `IvyLeafRng` + `IvySpawnNoise`
+ `GladeSettings` → `IvyLeafStorage`。**叶密度来自设置项 + RNG，不是场。**

`[PDB]` L8839 / L9534 = **`remove_ivy_under_windows`**（`[PATH]`
`crates/systems/decorator/src/remove_ivy_under_windows.rs`）：

```text
读: DecoratorOpEvent(事件) · DecoratorStorage · TerrainHeightsData
发: RemoveIvyCmd
```

⇒ **放/移一扇窗 → 事件 → 命令式修剪那一片藤**，不是空间 mask 排除。

#### 6.2 是不是一维沿线散布 —— **是，而且比本项目的还简单**【确凿】

`[GLSL] _ivy_instanced_ivy_branch.raster...vs_main.glsl` 的实例结构：

```glsl
struct IvySegment {
    vec3  start_pos;    float start_thickness;
    vec3  end_pos;      float end_thickness;
    vec3  wall_normal;  float arch_height;
};   // 12 float = 48 字节
```

- **一段折线 = 一个实例**。VS 把 12 顶点三棱柱按 `distance(start, end) * 1.1` 沿自身长度轴拉伸（L221）。
- `wall_normal` **只**用来搭截面朝向基，**不做沿法线的离墙位移**（`[分析]` §6.4 已确认，本文复核成立）。
- 跨小径不是几何拱起：`path_mask > 0.1` 时把低于包络的段/叶实例整个写 **NaN 剔除**（L111/L172）。
- 地形高度直接在 VS 里采 `mix(-2.5, 24.0, heightmap)`（L204）。

`ivy_branch.json` 实测：**12 顶点 / 6 三角、只有 `Vertex_Position` 一个流**，
TG 包围盒 X[−0.5, 1.0] Y[0, 1.0] Z[−0.866, 0.866] `[资产]` ——
这是一个**外接单位圆的正三角形截面、沿 +Y 长 1.0 的开口管**，
和 `brick` 一样是**字典 mesh**（非均匀缩放本身就是尺寸）。

#### 6.3 能不能复用本项目「GPU 沿曲线摆块」那套设施 —— **能，但有两处不合缝**

`CSShaperSteps` 的记录格式是 `Rec = (alpha, lengthScale, riseZ, _)`，
**`lengthScale` 就是逐记录的长度缩放** `[代码] CSGroundShaperSteps.h` ⇒
「一段一实例、每段长度不同」这件事**格式上天然支持**。

两处不合缝：

| # | 不合缝 | 根因 | 代价 |
| --- | --- | --- | --- |
| V1 | **`SolveBlockLayout` 把弧长切成整块** | 它是给「固定长度的砖」用的一维打包器（楼梯对照 `[二轮] L1071` 已定「原地不动」）。藤蔓的段长由**生长**决定，不是打包出来的 | 藤蔓**不该走** `SolveBlockLayout`；直接自己填 `RecordsByMesh`（`alpha` = 段中点参数、`lengthScale` = 段长/基础长）即可，`Scatter` 那一层照用 |
| V2 | **`ivy_branch` 的长度轴是 UE +Z，不是 +Y** | kernel 轴约定是「X 面内径向 / Y 沿曲线 / Z 平面法线」（`CSGroundShaperSteps.h` 头注释）；`ivy_branch` 的长度在 TG 的 +Y ⇒ 换轴后落在 **UE +Z** `[资产]` | 要么导入期加一次 −90° 旋转，要么在 `BlockSize` 之外加一个基础网格旋转。**【推测】**：这是按 TG→UE 换轴规则推的，确证方式 = 在编辑器里读 `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_branch` 的实际包围盒 |

#### 6.4 但本项目 D13 已经裁决**不接**这条路【确凿】

计划 D13 明写「接现有藤蔓管线」（空间殖民 SC 求解 → 生成**网格**），
并明确「**不建议**为此做真·逐帧模拟：那要引入持久藤状态 + 墙面约束 + 跨墙跳跃 + 修剪四套东西」。
本文的新证据**支持这条裁决**：上面 6.1 列的正是那四套（`IvyStorage` 持久 + `ivy_grower` 墙面约束
+ `check_for_wall_jump` + `ivy_pruner`），一个不少。

⇒ **TG 的藤蔓做法在本项目里属「明确不该抄」**，理由与楼梯对照第六节同源
（TG 是 Bevy + 自研渲染器，没有引擎可用；本项目有）。
6.2/6.3 那套「一段一实例」只在**将来真要换渲染宿主**时才有价值，现在记账即可。

#### 6.5 D13 今天的真缺口：**现有 vine 散点根本看不见房子**【确凿】

`VineScatter::CollectSurfaceTriangles` `[代码] CSVineScatter.cpp:38-45`：

```cpp
TArray<UStaticMeshComponent*> MeshComponents;
SurfaceActor->GetComponents<UStaticMeshComponent>(MeshComponents);
```

**只走 `UStaticMeshComponent`**。而 `ACSHouseActor` 的房体挂在
`UCSMeshRenderComponent` + transient `UCSMesh`（GPU 常驻流）上，**不是** `UStaticMeshComponent`
⇒ `ScatterTargetsFromSurfaceActor()` 对房子会返回**空三角集**，静默产出零个 target
（函数自己会打一条 `No wall triangles on %s` 的 Warning，`:341-345`）。

这不是 bug —— 计划 D13 本来就写的是「逐墙面矩形拒绝采样」（用房子的参数化墙矩形，不读三角形）。
但它意味着：**现有 `VineScatter` 三个入口
（`ScatterTargetsFromSurfaceActor` / `ScatterSourcesAtGroundContact` / `ScatterVineInputs`）
对 D13 一个都不能直接用**，D13 要新写一条「房子参数 → 点集 → 填 ISM」的通路。
好消息是最后一段（填 ISM）`VineScatter::FillInstances` `:302` 已经有了，只是 `static` 在 .cpp 里、
`AVineContainer` 也没有「直接喂一组点」的公开入口 —— 得开一个。

**D12/D13 的代码现状：零。** 全仓 grep
`NotifyEditCommitted` / `MarkDecorDirty` / `RT_Decor` / `ACSGroundDecorItem` / `VineTargetDensity`
= **0 命中** `[代码]`。

---

### 七、资产盘点（`[UE资产]` 实际清点 + `[资产]` 逐顶点实测）

#### 7.1 布局与总量（先纠正一个常见误读）

`Content/HouseTest/TinyGladeAsset/Meshes/` 有 135 个**顶层条目**，但其中两个是**容器目录**：

| 范围 | 数量 |
| --- | --- |
| `Meshes/` 顶层条目 | 135 |
| `Meshes/clutter/*` | **58** |
| `Meshes/decorators/*` | **221** |
| **`Meshes/` 下 StaticMesh 总数** | **474** |

统一路径形态（474/474 成立）：

```
Content/HouseTest/TinyGladeAsset/Meshes/<name>/StaticMeshes/<name>.uasset
Content/HouseTest/TinyGladeAsset/Meshes/{clutter|decorators}/<name>/StaticMeshes/<name>.uasset
```

UE 对象路径例：`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/window_cottage_1x1`。
`Meshes/` 下**没有任何 `Materials/` / `Textures/` 子目录**。
四处多嵌一层的例外：`sheep_animation/{1..30,delighted}`、`terrain_rt/{chunk_0..31}`、
`wooden_gate/{door_handle_circle, ladder}`，以及上面那两个容器。

#### 7.2 窗户可用资产：**基本齐全**

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#72-窗户可用资产基本齐全)**（2026-09-06 文档重组）。含前缀清点表、cottage 全套件清单与「重名陷阱」。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。

#### 7.3 装饰／杂物：**58 个 clutter 一个不缺**

`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/clutter/<n>/StaticMeshes/<n>`，与源 `assets/meshes/clutter/` 一一对应：

```
anvil barrel barrel_w_candles barrel_w_food_basket basket basket_w_food bench bench_short
bench_w_basket birdhouse birdnest box_w_stuff bucket cart cart2 cart_w_pumpkins
cart_w_pumpkins_regular crate_fish crates_w_flowers crates_w_pumpkins crates_w_pumpkins_regular
crates_w_tools door_bell door_krans door_krans_autumn fabric_rolls firewood flowerbox haystack
ladder lilypad market_stall_bread pitchfork plant_pot_v1 plant_pot_v2 potato_sack pumpkin pumpkin2
pumpkin_w_face rack_brooms rake_01 reed road_sign short_bench shovel stall_bread stall_veggies
sunflower watering_can wheel window_flower_bed_1..6 window_flowerpot window_shutter
```

D12 计划里点名的「箱子/水果摊」在这里分别是
`box_w_stuff` / `crates_w_*` 与 `stall_veggies` / `stall_bread` / `market_stall_bread` / `cart_w_pumpkins`。
`window_flower_bed_1..6` + `window_flowerpot` + `window_shutter` 正是窗户 autoclutter 那一组。

`decorators/setdressing_*`（14）另含 `_flower_pot{,_2,_purple,_red}` / `_flower_pot_hanging_1/2` /
`_flower_bed` / `_cloth` / `_clothline_*` 五件。

顶层植被/道具（全在，路径 `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/<n>/StaticMeshes/<n>`）：
`garden_stone flowery_lavender lavender lowpoly_flower plant_leafy plant_thistle bush bush_body
bush_flowers umbrella tree_log tree_stump path_pebble water_stones reed reed_autumn
garden_flower_01_lavender garden_flower_02 meadow_lowpoly_flowers clover clover_flowers
lilypad lilypad_flower fallen_leaves fallen_petals fallen_tree olden_glade_rock`。

#### 7.4 藤蔓：只有 3 个，且都是字典 mesh

| UE 路径 | 顶点/三角 | UE 尺寸 (cm) | 顶点流 |
| --- | --- | --- | --- |
| `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_branch` | 12 / 6 | 150 × 173.2 × 100（长度轴 = **+Z**） | **只有** `Vertex_Position` |
| `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_leaf` | 9 / 8 | 66.7 × 70.0 × 21.5（长度轴 = +Y） | Pos, Normal, UV |
| `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_flower` | 30 / 42 | 76.0 × 78.0 × 38.4 | Pos, Normal, UV |

`ivy_branch` 没有法线也没有 UV ⇒ 它**只能配一个自己算法线/UV 的着色器**，
直接扔给通用材质会全黑。这与 TG 的用法一致（VS 里现搭截面基）。

#### 7.5 缺失清单：**只缺 7 个，且全是退化网格**

| 组 | 缺 | 名字 |
| --- | --- | --- |
| `window_cottage_*` | 6 | `window_cottage_{1x1,2x1,3x1,corner_1x1,corner_2x1,corner_3x1}_flowerbed_locations` |
| `outline_*` | 1 | `outline_window_cottage_corner_2x1` |

原因：6 个 `_flowerbed_locations` 都是**零三角形的纯点集**（1/2/3 个顶点），
StaticMesh 导入器会静默丢弃；`outline_window_cottage_corner_2x1` 是 7 顶点 / 2 三角的退化条带。
**它们不需要重新提取** —— 坐标已经在第 5.2 节的表里，硬编成常量即可。

`clutter/` **零缺失**。源顶层另有 8 个未导入（`*_particle_spawner` / `*_particles`，
Niagara 式发射点云），与本文三个议题无关。

#### 7.6 真正的缺口是**材质**，不是网格

`/PCGPlugins/HouseTest/TinyGladeAsset/Materials/` 只有 8 个母材质：
`M_TG_Bark · M_TG_Canopy · M_TG_CanopyInner · M_TG_CanopyOpaque · M_TG_Floor ·
M_TG_LeafCards · M_TG_Texture · M_TG_VertexColor`。

- **没有窗/玻璃母材质** —— `*_glass` 网格上的 `is_glass` 顶点流没有任何着色器消费。
- **没有藤蔓母材质** —— 但贴图与实例是齐的：
  `MI_ivy_branch_color · MI_ivy_flower_color · MI_{summer,autumn,winter}_ivy_leaf_color · MI_leaf_alpha`。
- 窗户侧的实例齐全：`MI_window_0 · MI_window_glass · MI_window_sill_normal ·
  MI_window_colors_layer00..08`（9 个）+ `color_icon/windows/MI_*`（10 个配色）。

`/PCGPlugins/HouseTest/TinyGladeAsset/Materials/` 459 个，与 `/PCGPlugins/HouseTest/TinyGladeAsset/Textures/` 459 个一一对应。
唯一的地图是 `/PCGPlugins/HouseTest/TinyGladeAsset/Maps/TinyGladeGallery`。

#### 7.7 引用现状：474 个网格里 **473 个是孤儿**

全仓 grep（`.cpp .h .usf .ush .py .md .json .ini`）唯一活的资产引用是
`Scripts/TinyGladeSetupFrame.py:12` 的
`BRICK = "/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/brick"`；
`CSHouseActor.h:223` 是同一路径的注释。
其余命中全是逆向文档里的中文叙述（描述 TG 的 Rust 模块，不是 UE 资产）。

⇒ **窗户/装饰/藤蔓三条线都是干净的白纸开局**：网格与贴图在手，母材质与代码引用一个都没有。

---

### 八、待用户拍板（发现的冲突，**本文不改裁决**）

#### ~~C1~~ 已拍板（2026-08-30：**甲**）—— 谓词降维成一维 S 区间

**用户裁决：永久放弃"门上开窗"。** `CSHouse_OpeningsOverlap` 改比同边一维 S 区间（准确说是两洞的
**面板格**：半宽 + 半个墩），`Z` 不再进判据，与 `RebuildBodyMesh` 的单游标扫掠同维 —— 那条
`continue` 对过了谓词的洞已不可达，唯一真源纪律收口。本文原倾向的乙（面板垂直细分，即 A7）**作废**。
落地细节见状态文件「C1 已拍板」一节。

#### C2 D12 的「复杂度场」在 TG 里没有对位物

第 5.4 节的表。计划开篇写「实现 Tiny Glade 式高交互房屋」，D12 一节的复杂度场
**是自有设计**（TG 是锚点驱动的 region + 资产里烘的候选点）。
三个选项：① 保留自有设计，只订正「依据 TG」的措辞；
② 改成 TG 原版锚点法（推翻整节 `RT_DecorField` + tile-argmax）；
③ **叠加**（场负责「房子周围晕开一圈」的摆件，锚点负责「花箱挂窗台」这类精确附着）。
**倾向③**，因为两者能力互不覆盖。**未拍板前不动 D12。**

#### C3 D12 缺「用户否决派生物」的口子，而 TG 有两个实证

`DeletedAutoClutter` + `ui_convert_autoclutter_to_decoclutter` 钉住（第 5.3 节），
加上楼梯对照 A6 已经确证的 `StairsRemovedSupports` —— **同一形状的第二个实例**。
D12 现在只写了「边界翻转 churn……不可接受再给已存在实例加保留加成（迟滞）」。
⚠️ 与楼梯 A6 一样，它触碰「派生物纯函数、不序列化」这条反复裁决过的原则。**一并拍板更省事。**

#### C4 D8 计划里「逐像素 clip 是 Tiny Glade 的做法」这句对窗户不成立

> **本节正文已迁出到 [`House_Window.md`](House_Window.md#c4-d8-计划里逐像素-clip-是-tiny-glade-的做法这句对窗户不成立)**（2026-09-06 文档重组）。
> 搬过去的是**正文**，证据等级标注【确凿】/【推测】、表格、代码块、删除线作废段一字未改。

#### C5 `Tag` 字节已被门占满

`Tag` 现在写门的 `Slot & 0xFF`，进顶点色 G 通道。窗要区分「门/窗/被拒」需要抢同一字节。
属 D14 的通道字典问题，**动工前必须先定字典**，否则会出现「悬停高亮点到窗上高亮了门」。

---

### 九、待确认

| # | 待确认项 | 需要什么证据才能确证 |
| --- | --- | --- |
| U1 | `generate_cottage_wall_windows` / `generate_gothic_wall_windows` 确实调 `WallHoles::add` | 反编译 `[PDB]` L9100 那个系统的函数体，找调用边。目前只有「同一系统同时握洞表与窗候选表」这条间接证据 |
| U2 | TG 是否真的没有「剩余空间自动填窗」 | 反编译 `validate_blueprints` / `instantiate_blueprints` 函数体。PDB 只能证明没有这样一个**系统** |
| U3 | `HoleType` / `HoleOrigin` 的枚举变体（窗是不是一个独立 HoleType） | PDB 只有类型名。需反编译或找 `.ron` 序列化样本（与楼梯对照 U3 同一条） |
| U4 | `WallAttachment` / `WindowDecoratorInfo` 的字段构成 | 09-05 曾因 W4 作废而关闭，**09-06 重新有下游**：锚点「与 TG 一致」。PDB 只给类型名 `WallAttachmentAnchor` / `WallAttachmentSide` / `WallCornerAttachment{rectangle_corner_id}`；字段名拿不到，只能按性质对齐（权威 / 派生 / 拓扑）。**不阻塞** |
| U5 | `ivy_branch` 在 UE 里的实际长度轴 | 编辑器里读 `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/ivy_branch` 的包围盒。本文的 +Z 结论是按换轴规则推的 |
| U6 | 第 4.3 节「框砖对窗零改动」 | 手工塞一个 `Type=Window, Shape=Rect, Z0=90, Z1=250` 的洞跑一次，看墙板 + 上下两条砖带是否都出、矩形窗的两点剖面会不会把门樘顶角抹圆 |
| U7 | `GladeSettings` 里到底有没有藤蔓密度项 | `assets/glade/*/settings.json` 只有 `{"butterflies": true}`，说明 `GladeSettings` 不是主题文件而是运行时设置。需从 exe 字符串或反编译取 |
| U8 | 顶层 `window_cottage_1x1`（85 cm 深）在 TG 里的用途 | 它不在 `decorators/` 里、带 `Vertex_Color` 不带 UV ⇒ 疑似另一条管线（toolbar 预览？`decorator_test`？）。需在 exe 字符串里找引用点 |

---

### 十、可行动清单（排序）

排序依据同楼梯对照：**先做不触碰任何既有裁决的**，再做只触碰一条的，最后是需要先推翻裁决的。

| 序 | 事项 | 预估改动范围 | 触碰的既有裁决 |
| --- | --- | --- | --- |
| ~~**A1**~~ | ~~订正两处「依据 TG」的措辞~~ **①已被 2026-09-05 的两层重写吸收**（计划不再宣称「clip 是 TG 的做法」，且已如实记下 TG 的四级洞缘）；②D12 复杂度场标注为自有设计**仍待做** | 文档一句话 | 无 |
| **A2** | **`QueryFeaturePlacement` 返回 `FCSFeaturePlacement`**（`bAccepted` + `Reason` + `SnappedWorld`）。计划已给结构体，只是没落地；P2 让拒绝原因从锦上添花变必需品 | `CSHouseActor.h/.cpp` ~40 行 + 单测；调用点只有 `ComputeDoors:316` 一处 | 无（纯加法，计划已裁决过形态） |
| **A3** | **`CSHouse_OpeningCell` 按 `Type` 分流墩宽**（门 `PierWidth`、窗 `csh.WindowPierWidth` 默认 0）+ `csh.WindowMinSillZ` 下限守卫 | `CSHouseProfile.h` ~10 行 + 两处调用点同步 + 单测 | 无（W3/W6/P3） |
| **A4** | **`CurrentOpenings.Sort` 末位加 `SourceId`**，让同 `(EdgeIndex, CenterS)` 的两个洞有全序 —— 幂等短路的正确性条件 | 1 行 + 一条单测 | 无（P4；与楼梯对照第二节「pull 不 push」同一条纪律） |
| ~~**A5**~~ | ~~窗洞冒烟测~~ **已做**（`House.WindowPredicateMatchesGeometry` / `House.FrameSkipsWindows`（原 `FrameWindowSill`）+ `demo_house_window`）。⚠️ 判据里的「墙板 + 窗台盒」两项会随 2026-09-05 的两层重做失效，届时改判「砖层洞缘贴合 + 灰泥覆盖度」 | — | — |
| **A6** | **`ACSHouseFeatureMarker` + `ACSWindowMarker`**（D8 主体）。TG 侧形态已逐条对上（第 2.2 节），照计划实现即可；`DecoratorBackup` 印证了「弹回最后被答应位置」的做法 | 新 actor ~350 行 + 编辑器 tick 那几个坑（计划已列全） | 无（**C1 已于 2026-08-30 拍板选甲**，前置裁决已解除） |
| ~~**A7**~~ | ~~**面板垂直细分**（C1 的乙案）：同一 S 区间上下两块面板各带一个 clip 场~~ **作废** —— C1 于 2026-08-30 拍板选甲（谓词降维），不做垂直细分 | — | — |
| ~~**A8**~~ | ~~矩形窗的框不走曲线铺砖，改摆四条直边~~ **已做**（`CSHouseFrame.cpp:119` 的 `Rect` 分支），**且已被 2026-09-05 两层裁决吸收**：框砖并入砖层，矩形洞的四条直边就是砖层里被贴合过的那几列砖 | — | — |
| ~~**A9**~~ | ~~窗/玻璃母材质~~ **作废**（2026-09-06 用户裁决：**窗不需要玻璃材质，TG 里它也不是透明的**）。实测四个候选 `MI_cottage_window` / `MI_window` / `MI_window_sill_normal` / `MI_window_glass` **全是** `M_TG_Texture` 的实例、`MSM_DefaultLit` + `BLEND_Opaque` ⇒ 窗框资产自带的 `MI_window_colors_layer00` 直接就是受光不透明的，**材质零工作量** | — | — |
| **A10** | **硬编 `_flowerbed_locations` 坐标表**（6 组，第 5.2 节已给全）。它们导不进 StaticMesh，但 D12 的「花箱挂窗台」要用 | 一张 C++ 常量表 ~20 行 | 无，**依赖 C2 拍板**（叠加案才需要） |
| **A11** | **D12 的锚点层**（C2 的③叠加案）：在复杂度场之外加一条「已放窗 → 花箱候选点」的通路，照 `add_autoclutter_on_windows` 的形态 | 依赖 D12 主体先落地 | **触碰 C2**，必须先拍板 |
| **A12** | **`SuppressedDerived` 抑制口**（C3）。TG 侧现在有**两个**实证（`StairsRemovedSupports` + `DeletedAutoClutter`），比楼梯对照 A6 写时又强了一档 | 形状与代价见楼梯对照 A6 | **触碰**「派生物纯函数、不序列化」。**与楼梯 A6 是同一次裁决，建议合并** |
| **A13** | **D13 的房子→点集通路**：新写「墙矩形拒绝采样 → 填 `GrowTarget` ISM」，并给 `AVineContainer` 开一个「直接喂点集」的公开入口 | ~150 行；`VineScatter::FillInstances` 已有，只是 `static` | 无。**但必须知道现有 `ScatterTargetsFromSurfaceActor` 对房子返回空**（第 6.5 节），别照它抄 |
| ~~**A14**~~ | ~~**转角窗 W4**~~ **作废**（2026-09-05 用户裁决，见 W4 行与计划 D8） | — | — |

**A1–A5 之间没有相互依赖之外的前置，五项合起来就是「让窗户能被砌出来」的最小闭环，且不需要任何新裁决。**
A6 起要么依赖一次拍板（A7 / A11 / A12），要么依赖别的模块先动工（A9 / A10 / A13）。

---

<a id="vol-3"></a>

## 卷三 · 楼梯模块对照

> 原文件 `TinyGlade_楼梯模块对照.md`，原标题「Tiny Glade 楼梯模块对照：范围声明的复核与第一步的可行性」。

本文只回答四件事：**TG 的楼梯到底有几套**、**本项目有没有对位物**、
**计划把楼梯划在 D1–D13 之外的理由今天是否还成立**、**真要动工第一步能用现有设施走多远**。

设计裁决在 [`Plan_House.md` 的「开放问题 · 楼梯的范围声明」](Plan_House.md)，
本文**不复述**它，只写复核结论与新证据。前两轮评审已经得出的结论
（[`Review_ReverseComparison.md`](Review_ReverseComparison.md) 卷一 / 卷二 的 H3 / M15 / M16 / L6 / L7 / L8 与被否条目）
同样不复述，只在结论被**推翻、加强或降级**时点名引用行号。

风格与详略口径对齐 [`Ground_Shaper.md`](Ground_Shaper.md)。

### 证据标注约定

沿用 `MESH_GENERATION_ANALYSIS.md` 的三档可信度，并给每条标出**证据种类**：

| 标注 | 含义 |
| --- | --- |
| 【确凿】 | PDB 符号 / 资产字节 / 本仓源码直接给出，不含推理 |
| 【推测】 | 由确凿证据推理得出，推理链在正文写明 |
| 【待确认】 | 现有证据不足以判定，正文写明「需要什么证据才能确证」 |
| `[PDB]` | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt`（97033 条） |
| `[PATH]` | `D:/MyProject/Tiny Glade/tmp/pdb_paths.txt` |
| `[资产]` | `D:/MyProject/Tiny Glade/assets/meshes/*.json` 实测 |
| `[代码]` | 本仓 `Source/ComputeShaderGenerator/` |
| `[分析]` | `MESH_GENERATION_ANALYSIS.md` |
| `[一轮]`/`[二轮]` | 两轮对比逆向报告，带行号 |

⚠️ 一处易误采：`[一轮]` L101 / L109 / L126 的「台阶」指**量化台阶**
（`DoorWidthQuantum` 2 cm、8-bit 顶点色 0.9 cm），与楼梯无关。

---

### 一、TG 的楼梯有几套：是**四**套，不是三套

`[分析]` §4 标题写的是「三套并存」。PDB 里能查到**第四套**，它既不进图状态、
也不依赖墙路径 —— 而且是四套里唯一一套在本项目**今天就有落点**的。

| # | 名字 | 触发源 | 读什么 | 产出什么 | 状态形态 |
| --- | --- | --- | --- | --- | --- |
| §4.1 | 玩家绘制 `playermade` | 玩家画线（`InstaCreateStairPoint`） | `StairsState` 图 + 地形 + 墙 | 踏步砖 + 地板网格块 + 拱/托架/柱 + 栏杆 + 梯子 + 碰撞 + 墙洞 | **持久可撤销图** |
| §4.2 | 平台楼梯 `platform` | `OnWallChanged` / 屋顶命令 | 墙路径分段 + 墙高曲线 | 墙顶步道内的台阶段 + 墙洞 | 无（墙的派生物） |
| §4.3 | 岩地台阶 `rocky_terrain` | 每帧 GPU | 高度场 + rocky + path + water | `Stair{Affine3Packed}` 实例 + pebble | 无（场的纯函数） |
| **§4.0** | **门前踏步 `door_stairs`** | 墙/门变化 | **只有 `PublicWalls` + 资产库** | 门下的砖石踏步 | **无（墙的纯函数）** |

#### §4.0 门前踏步：新发现的第四套【确凿】

`[PATH]` `crates/systems/decorator/src/decorator_visual/door_stairs_assemble_bricks.rs`；
`[PDB]` `door_stairs_assemble_bricks::{DoorStairsAssemblyParams, construct_door_stairs, door_stairs_assemble_bricks}`。

系统签名（`[PDB]` L8843，**全部四个参数**）：

```text
Res   <public_walls::PublicWalls>
Res   <ssbo_library::AssetSsboLibrary>
ResMut<deco_differential_state::WallAttachedDecoTracker<door_stairs_assemble_bricks::DoorStairsAssemblyParams>>
Res   <signifier_create::UiSignifierStream>
```

**这条签名是本文最有价值的一条证据**，因为它把「TG 的楼梯都很贵」这个印象证伪了：

- **没有 `StairsState`**、没有 `StairsStructuralGraph`、没有 `StairsRemovedSupports`、
  没有 `RaycastWorld` —— 它与 §4.1 的整套图/撤销/物理设施**零耦合**。
- **没有 `TerrainHeightsData`**。踏步高度只从墙自身推出来（墙底与门槛的高差），
  不查地形。这与本项目「房底 Z = footprint 全域地面高度的 max + `HeightOffset`」
  的落座规则天然合拍 —— 门槛与地面的高差**本来就是一个已知量**。
- 状态载体是 `WallAttachedDecoTracker<T>` —— 一个泛型**差分跟踪器**
  （`[PATH]` `decorator_visual/deco_differential_state.rs`），
  即「按参数比上一帧、只重建变了的那部分」。这就是 TG 自己的哈希守卫，
  与本项目 `BodyDescHash` / `FrameDescHash` 同形。

**结论**：TG 里存在一套「墙的纯函数、零图状态、零撤销、零地形查询」的楼梯。
`[二轮]` L1083 把 TG 楼梯归纳为三套时漏掉了它，因而**低估了「最小可行第一步」的可达性**。

#### §4.1 的真实体量：比 `[分析]` §4.1 记的还大【确凿】

`[PDB]` 给出的 `stairs_assemble` 完整函数集里，有五个 `[分析]` 未记、且**改变工作量判断**的：

| 符号 `[PDB]` | 说明 |
| --- | --- |
| `stairs_assemble::playermade_stairs_interpolate_and_compare` | 装配参数的**插值 + 比对**（TG 自己的幂等短路） |
| `stairs_assemble::playermade_stairs_assembly_propagate_dirty_flags` | 脏标记**跨节点传播**（改一段影响邻段） |
| `stairs_assemble::stairs_subscribe_lazy` / `StairsLazySubscriber` | **惰性订阅**：不是每帧全量重算 |
| `stairs_assemble::structural_graph_subscribe` / `StructuralGraphSubscription` | 结构图的订阅通道 |
| `stairs_assemble::split_segment_into_supported_and_unsupported_subsegments` | 一段楼梯要先切成「有支撑 / 无支撑」两类子段 |

再加 `stairs_state::StairsState::compute_checksum`、`stairs::invalidate::invalidate_dirty_stair_artifacts`、
`history/replay_stairs::replay_stairs` + `stairs_history_edit::{StairsHistoryEdit, StairsReplayCmd}` `[PATH]`。

**新结论【推测】**：TG 的楼梯撤销**不是逆操作，是快照 + diff + 重放**
（`StairsSnapshot` / `StairsHistoryDiff` / `restore_snapshot` / `apply_diff` / `reduce_diff` + `StairsReplayCmd`）。
推理链：这些符号同时存在且分属 `resources/stairs/stairs_state.rs` 与 `systems/history/replay_stairs.rs`，
而 `StairsState` 只有 `add_node/add_edge/remove_node/disconnect_edge` 四个正向变更入口、
**没有任何 `undo_*` 反向入口**。
要确证需要反编译 `replay_stairs` 的函数体，PDB 只给符号名。

#### `StairsRemovedSupports`：把 `[二轮]` L1106 的【我的推断】升级为【确凿】

`[二轮]` L1106 说「"支撑是用户可删的对象"是【我的推断】，报告没说是谁删、状态存哪」。
PDB 给出了两条直接证据：

1. 资源存在：`country_core::resources::stairs::stairs_removed_supports::StairsRemovedSupports` `[PDB]`，
   独立文件 `resources/stairs/stairs_removed_supports.rs` `[PATH]`。
2. **谁写它**：`[PDB]` L8840 的系统签名里同时出现
   `StairsRemovedSupports`、`HandleStairAttachedDecoratorsOnStairPointDeletion`、
   `AnimateWallBulldozeBuffer`、`DeleteDecoratorCmd`、`History`、`OutlineStairsCmd`
   —— 这是**推土机/橡皮擦**系统。
3. **谁读它**：`[PDB]` L8716（`playermade_stairs_assemble`，16 个参数）
   把 `StairsRemovedSupports` 与 `StairsState` 一起当**输入**读进去。

**这正好是 `[二轮]` L1112 提出的 `SuppressedDerived` 形状，而且 TG 已经证明它可行**：
把抑制集做成装配的**输入**，纯函数性质不受影响。TG 没有为此放弃派生物纯函数。

`playermade_stairs_assemble` 完整签名（`[PDB]` L8716，去重后）：

```text
读:  StairsState  StairsRemovedSupports  PublicWalls  TerrainHeightsData
     AssetSsboLibrary  GraphicsDeviceResource  UiSignifierStream
写:  StairsVisualState  StairsArchSupportRequests  StairsBracketsRequests
     StairColliderRegistry  PlayermadeStairsAssemblyRequests
     DecoratorStorage  RaycastWorld  RtWorld
```

注意「写」那一列的三个 `*Requests` —— §4.1 **不直接砌砖**，
它只往 `stairs_walls::{StairsArchSupportRequests, StairsBracketsRequests, StairsPillarProposals}` 里
投诉求，由 `system_wall_constructor::construct_playermade_stairs::*` 另行消费 `[PDB]`。
`StairsPillarProposals::add` + `pillars_process_proposals::process_playermade_stair_pillars_proposals` `[PDB]`
说明柱子那一路还是**提案 → 裁决**两段式，不是直接生成。

---

### 二、墙洞总线：TG 对 M15 的答案【确凿】

`[二轮]` M15（L356-373）提出需要 `ICSWallOpeningProvider`，但当时不知道 TG 那边长什么样
（L360 只说「三家进同一张 `WallHoles` 表」）。PDB 把这张表的**完整形态**给出来了：

```text
wall_holes::WallHoles::{add, extend, clear_for_next_frame}
wall_holes::WallHoleStorage::{new_in_progress, new_finalized, as_wip_mut,
                              finalize, as_finalized, clone_and_reopen}
wall_holes::InProgressHoleStorage::{add, extend, holes}
wall_holes::FinalizedHoleStorage::inner
wall_holes::FinalizedWallHoles::{holes_no_duplicates, iter_holes_with_padding, wall_length}
wall_holes::{HoleType, HoleOrigin, HoleChecksum, PrevWallHoles}
```

读法：

- **两相状态机**：`InProgress`（累积）→ `finalize()` → `Finalized`（只读），
  `clone_and_reopen` 用于「已定稿但要再加一个」。类型系统保证「定稿后不能再写」。
- **每帧清空**：`clear_for_next_frame` ⇒ 洞表是**派生物、不持久**。
  与本项目 `CurrentOpenings` 是 `UPROPERTY(Transient)` `[代码]` 同一形态。
- **`HoleOrigin`** 记录「谁要的这个洞」；**`HoleChecksum` + `PrevWallHoles`** 就是哈希守卫 + 上一帧比对。
- **`iter_holes_with_padding`** ⇒ 洞自带净空（本项目 `OpeningClearance` 的对位物）。
- **`holes_no_duplicates`** ⇒ 允许多个生产者投出重复洞，去重在消费端。

**生产者不止三家，是六家。** 把 `[PDB]` 里所有含 `wall_holes::WallHoles` 的系统签名解出来：

| `[PDB]` 行 | 该系统的全部 ECS 参数（去重、去命名空间） | 判读 |
| --- | --- | --- |
| L8819 | `StairsVisualState` `WallHoles` | **§4.1 的 `playermade_stairs_add_wall_holes`**【推测】 |
| L9046 | `PrevWallHoles` `WallHoles` | 定稿/比对系统【推测】 |
| L9047 | `WallHoles` `PlatformStairsStorage` `PublicWalls` `TerrainHeightsData` `RaycastWorld` `DecoratorStorage` `AssetSsboLibrary` `UiSignifierStream` | **§4.2 也开洞**【推测】 |
| L9100 | `WallHoles` `WallAttachedDecoTracker` `DecoratorBlueprints` `DecoratorPhysicsColliders` `WindowAutoClutterCandidates` … | 门/窗装饰物 |
| L8851 / L8854 / L8856 | `WallHoles` + 半木/山墙角/雉堞 | 三家只**读**洞的消费者 |
| L9341 | `WallHoles` + `RecalculateWallBricks` `WallEntities` `RtWorld` … | **砌砖主消费者** |

两条从这张表读出来的**新结论**：

1. **§4.1 的开洞系统只有两个依赖**（`StairsVisualState` + `WallHoles`）。
   它不查 `PublicWalls`。⇒ 楼梯与墙的**锚定关系在装配参数阶段就已解完**
   （`stairs_assembly_params::{StairWallAnchor, StairNodeAnchor::try_as_wall, StairWallAnchor::wall_normal}` `[PDB]`），
   这个系统只负责**发布**。**开洞本身极其便宜，贵的是它上游的锚定求解。**
   这直接影响计划里「穿墙开洞」这一步的成本估计 —— 前置是 M16（扫掠查询），不是 M15。
2. **§4.2 平台楼梯也进洞表**（L9047 同时握 `WallHoles` 与 `PlatformStairsStorage`）。
   `[分析]` §4.2 只写了「墙顶步道跨高差自动分段」，没提开洞。【推测】，
   要确证需反编译该系统函数体，或找到 `PlatformStairsStorage` 与 `WallHoles::add` 的调用边。

#### 但这条**不能照抄**：push 模型在 UE 里会退化成不确定序

TG 的六家生产者写同一张 `ResMut<WallHoles>`，顺序由 Bevy 的 `.before()/.after()` 调度图保证。
本项目**没有调度器**，`CurrentOpenings` 由 `ReevaluateSite()` 内部从零重算。
把 push 模型搬过来，「谁先 add」将取决于 actor 的 tick / 注册顺序 —— 同一场景两次加载可能给出
不同的 `SourceId` 排序，进而给出不同的 `BodyDescHash`，**幂等短路会失效**。

⇒ M15 必须是 **pull**（房子向注册表要），不是 push（注入方往房子里写）。
这与 `[二轮]` L367 已给出的 `ICSWallOpeningProvider{ CollectOpenings(...) }` 形状一致，
本文只是补上「为什么不能是 push」的**根据**。同理，`[二轮]` L1313 / L1510 已否掉的
静态多播 `OnAnyHouseGeomChanged`（Bevy 事件总线的项目等价物）在这里**再次被同一条理由否掉**。

---

### 三、逐套的对位物盘点

| TG | 本项目对位物 | 差在哪 | 取舍建议 |
| --- | --- | --- | --- |
| §4.3 岩地台阶 | **地面的石阶** `ACSGroundActor::RebuildStairs`（`CSGroundStairs.usf`，GPU marching squares；塑形物自持的 `BuildStepPlan` + `CSGroundSteps.usf` 旧路已于 08-30 删除） | 等高线求解方式（CPU 闭式 vs GPU marching squares）；已由 D9「石阶改造」裁决对齐 | 已在计划内，本文不重复。骨架对照见 [`Ground_Shaper.md`](Ground_Shaper.md) 的「石阶：骨架几乎一样，两处关键分歧」 |
| §4.2 平台楼梯 | **零** | 缺**两个**模型概念（见下节） | 维持排除。且**不作为入口**（`[二轮]` L1393③ 已定死） |
| §4.1 玩家绘制 | **零** | 图状态 + 撤销 + gizmo 交互层 | 维持排除；但第一步（自由路径踏步）可与图状态解耦 |
| **§4.0 门前踏步** | **零，但落点已经全部就位** | 只缺「谁来摆这些砖」这一段 | **建议新增为第一步的备选入口**，见第五节 |

对位关系的一句话总结：**本项目今天做完的是 TG 四套里最难的那套的 GPU 半边，
而最简单的那套（§4.0）一行代码都没有。**

---

### 四、范围声明的两条理由，今天是否还成立

#### 理由一：§4.2「在当前房屋模型里没有落点」—— **成立，而且比计划写的更强**

计划写的是「要有 §4.2 先得有『墙路径』这个概念」。PDB 显示前置是**两个**，不是一个：

| 前置 | TG 侧证据 `[PDB]` | 本项目现状 `[代码]` |
| --- | --- | --- |
| ① 墙**路径**的一维参数化与分段 | `segmentation::calculate_wall_path_segmentation::{calc_path_wall_segmentation, process_segment, score_split_candidate, add_splits_in_empty_range}`；资源 `WallPathSegmentationMasks` | 墙是刚性矩形四条边，`EdgeIndex` + `CenterS`，无分段、无打分 |
| ② 墙**高**沿路径的**曲线** | `adjust_wall_height::ui_adjust_wall_height_mode::{adjust_wall_height_curve, clamp_wall_height, get_closest_point_u_to_cursor}` | `WallHeight` 是**单个标量** |

②是新证据：TG 的墙高是**沿墙参数 u 的曲线**，玩家可以按位置拖高拖低。
没有它，「墙顶跨高差」这个前提在本项目里**根本不可能出现** —— 墙顶恒水平。

§4.2 的落地形态也由 PDB 给死了：`PlatformStairSegmentationRange` +
`WallPathSegmentationMasksMinusStairs` `[PDB]` ⇒ 平台楼梯是**墙顶分段里被挖掉的一个区间**，
其余步道按「减去楼梯」后的 mask 正常分段。这不是「在墙顶加个楼梯」，
是**在一维分段求解里插一类特殊段**。

⇒ **范围外的理由不但成立，而且工作量被低估了。**
`[二轮]` R4（L1298）否掉的「给 `ACSSplineBlockActor` 加 `bStepMode`、60 行、零前置」，
在本文的新证据下**继续被否**，且多一条理由：`SplineBlockActor` 连墙高曲线的输入都没有。

#### 理由二：§4.1 的门槛是「状态形态」—— **成立，但需要补一条**

计划说门槛是「持久可撤销的节点共享图 vs 每 actor 一份参数、派生物全 transient、零撤销」。
这条**完全成立**，`[二轮]` L1089 已给出仓内实证（全 `Source/` 树只有 2 处 `FScopedTransaction`，
且都在 `EditorShortcuts` 模块）。

**要补的一条：门槛不止状态形态，还有交互层。** `[PDB]` 里 §4.1 的 UI 面：

```text
ui_focus_stairs::{ui_focus_stairs, HeightAllGizmoCache, MoveAllGizmoCache, WidthAllGizmoCache}
ui_stairs_viz::{visualize_ui_stairs, insert_point_ui, StairGizmosCache, WidthPointGizmo}
ui_stairs_misc::{continue_stairs, stairs_reset_for_new_frame}
ui_change_stair_height::ui_change_stair_height
ui_autoclutter::InstaCreateStairPoint
hint_stairs::{hint_stairs, StairsHintConditions}
+ 6 个 onboarding 提示资源（NotifyHintSystem{StairsPlaced, HoveringOverStairs,
  CanContinueStairs, StartedStairsContinue, StairsCanDetachFromWall, StairsDetachedFromWall}）
```

四个 UI 模块 + 三套 gizmo 缓存 + 六条引导提示。
而本项目是「零 HitProxy / 零 Gizmo / 零 InputBehavior」（`[二轮]` L1611，「画线成墙」被否的理由之一）。
⇒ **§4.1 的完整形态需要先推翻一条已裁决的「明确不做」，不只是加一个可撤销图。**

#### 那么 `FCSWallOpening` + `QueryFeaturePlacement` 改变结论了吗？—— **没有，但把边界挪了一格**

- `FCSWallOpening` 的 `Z0/Z1/AxisUS/Skew` 已经落地 `[代码]`，
  且 `CSHouse_SampleOpeningProfile` 已经在按 `Skew` 做倾斜顶（`Sample.ZHigh = ZHigh + Tilt`）`[代码]`。
  ⇒ **H3 已完成**，「洞的数据模型容纳不了楼梯」这条**不再是缺口**。
- 但 H3 只解决了「洞长什么样」。楼梯穿墙还差**两条前置**，两条都还是零：
  - **M15 注入通路**：全仓 grep `OpeningProvider` / `WallOpeningProvider` = **0 命中** `[代码]`。
  - **M16 扫掠查询** `SweptPrismVsWalls`：同样为零。
- `QueryFeaturePlacement` 是**谓词**（能不能放），不是**通路**（怎么进来）。
  `[二轮]` L364 已经指出它对楼梯「没有语义」——「拒绝」的后果是**楼梯插进实心墙**，
  而不是像窗户那样「不生成」。这条结论**未被本轮任何新证据推翻**。

⇒ **结论不变**：§4.1/§4.2 仍在范围外。
**但计划里排的三步顺序，第一步的门槛比原先估计的低**（见下节），
而第二步（穿墙开洞）的**贵处被定位得更准了** —— 贵在 M16 的锚定求解，不在开洞本身。

---

### 五、最小可行第一步：现有设施能走多远

> ⚠️ **2026-09-16 已按 [附录 D](RE_PlayerStairs.md) 落地，本节的两处口径被订正**（附录 D §9.0）：
> ① 踏步不是「按高差 / 18 cm 分级」—— 那是用户 Godot 原型的做法，TG 是 3D 弧长按 50 cm 重采样；
> ② `BuildFramePlan` 那条样条 + 逐砖记录的路已删，MVP 走 CPU 实例路（`ACSStairsActor` + `CSStairs.h`）。下面保留作当时的推理记录。

计划给的顺序是「沿自由路径的踏步 + 底下砖石支撑（不穿墙、不进图状态）→ 穿墙开洞 → 图状态」。
`[二轮]` L1094 把起点指定为「已跑通的 `CSGroundShaperActor` 分层铺装链（`.cpp:286-341`）」。

#### ⚠️ 这条起点指定已经过期，必须改

`[二轮]` L1094 写于 2026-08-29。**2026-08-30 用户裁决「石阶改造：100% GPU 决策 + 零回读」**，
计划 :628-632 的分期 S3 明写要**删掉** `BuildStepPlan`（181 行）、`EnsureCapacity`、
`RDG_SmoothSpline` 那条路，并放弃 `SolveBlockLayout` 变长铺装与 B 样条平滑。

⇒ **把自由路径踏步建在 `BuildStepPlan` 上，等于复活一条已排期删除的链。**

**正确的模板是 `ACSHouseActor::BuildFramePlan`（D6 门拱砖框），不是 `BuildStepPlan`。**
理由：它是同一套设施的**另一个消费者**，而且**不在 D9-S3 的删除范围内**。
它的形态逐条对得上楼梯的需要 `[代码] CSHouseActor.cpp:935-1090`：

| `BuildFramePlan` 的做法 | 楼梯要的东西 | 对得上吗 |
| --- | --- | --- |
| 每条曲线一份 `FScatterParams`，各带自己的 `PlaneNormal` / `CentreWorld` | 楼梯段的平面法线 = 世界上 | ✅ |
| 单一 palette 条目 + `SolveBlockLayout` 沿弧长铺 | 踏步沿路径铺 | ✅ |
| `Scatter(One, Buffers, Params[i], bClearCounter = (i == 0))` 多曲线累加 | 多段楼梯累进同一批 buffer | ✅ |
| `BlockSize = (Depth, Length, Thickness) / MeshSize`（单位立方体字典 mesh） | 踏步尺寸靠非均匀缩放 | ✅ |
| `ReserveCapacity` 在注册期一次付清，交互期零 flush | 拖路径时不能卡 | ✅ |
| `FrameDescHash` 幂等短路 | 声明式重求值 | ✅ |

#### GPU kernel 需要改几行？—— 对称块的 MVP 是**零行**【推测，推理链在下】

`CSGroundSteps.usf` 的记录是 `(alpha, lengthScale, riseZ, _)`：

- **`Rec.z` 就是楼梯要的抬升通道**。kernel 里 `Position += Up * (Rec.z + StepZOffset)`，
  `Up` = `StepPlaneNormal`。楼梯的平面法线是世界上 ⇒ 把第 k 级填 `Rec.z = k * StepRise`，
  平面内的路径曲线**直接变成阶梯**。CPU 侧现在填的是 `PaletteRise[Pick]`（半个身位抬升）`[代码]`，
  换成累加量不需要动 kernel。
- **`Rec.w` 空着**，是「每级块高不同」（Godot 原型那种全高楔块自撑）的天然去处。
- **基向量翻转不成问题**：kernel 用 `Position − StepCentreWorld` 的面内分量强制 `+X` 朝外。
  自由路径没有「中心」，这条判据对开放路径是病态的 —— **但块在 X 上对称时翻转不可见**。
  TG 的 `stairs_step` 恰好是**居中的单位立方体**（见下节实测）⇒ MVP 用它，翻转无害。

**推理链**：以上四条都是对 `Shaders/Private/CSGroundSteps.usf` 与
`Private/CSHouseActor.cpp:935-1090` 的直读，未做任何行为实测。
标【推测】而非【确凿】，因为**没有跑过**；确证方式 = 拿 `stairs_step` 做一次三级直梯的冒烟测。

#### 真缺口：四条，都不在 GPU 侧

| # | 缺口 | 为什么现有设施顶不上 | 规模 |
| --- | --- | --- | --- |
| G1 | **(run, rise) 耦合求解** | `SolveBlockLayout` 只解**一维弧长**（`[二轮]` L1071「原地不动，已是共享的一维打包器且已单测」）。楼梯要求级数在**水平进深与竖直升高上同时成立**，且踏高落在舒适带内 | 新增一个纯函数 + 单测，~60 行 |
| G2 | **块高逐级可变** | `FPaletteBuffers::BlockSize` 是**每 palette 一份**，不是每记录一份。全高楔块自撑要求每级块高不同 | kernel 加一个 `Rec.w` 读取 + CPU 侧填值，~10 行；**但它触碰「两个消费者共用这一支」的既有裁决** |
| G3 | **路径的宿主 actor** | `ACSSplineBlockActor` 有样条 + palette + `RebuildBlocks`，但它产出的是 **CPU 烘焙的常驻 mesh**（`UploadTinyGladeSnapshot`），不是 GPU 实例；且不读地面 `[代码]`。`[二轮]` R4 已否掉「给它加 `bStepMode`」 | 新 actor，参照 `ACSGroundShaperActor` 的地面订阅 + `ACSHouseActor` 的 palette/buffer 三件套，~250 行 |
| G4 | **踏步下不长草** | TG 有 `_stairs_foliage_exclusion.raster`，把楼梯顶视网格烙进植被排除 mask，**只写离地 < 1.75 m 的像素**（桥拱下仍长草）`[分析]` §4.3-7 | 挂到计划 :1080 已规划的 `RT_DecorMask` 上，D12 动工后才有意义 |

G1 的规格**已经有现成参考**：用户自己的 Godot 原型
`D:/MyProject/Tiny Glade/godot-tiny-glade/stairs/stairs_builder.gd` 已经把它写出来了 —— 
`n = max(1, round(|rise| / 0.18))`、`run = length / n`、`run < 0.12` 则拒绝、
每级块从公共基准面拉到踏面顶（全高楔块，自带支撑）。
这份原型**不进图状态、不穿墙、不查碰撞**，正是计划第一步要的形态。

⇒ **能走多远：一个「沿样条的直/曲踏步 + 全高楔块自撑」的可玩切片，
GPU 侧几乎零改动，CPU 侧一个新 actor 加一个可单测的纯函数。**
走不到的是：穿墙（缺 M15/M16）、拱/托架/柱（缺 `FCSSnapshotWriter` 提 Public，即 L6）、
栏杆、梯子、图状态与邻接对齐。

---

### 六、明确不该抄

`[二轮]` L673 已经给出根依据：「TG 从零自研这一套，是因为它是 Bevy 纯 ECS + 自研 Vulkan 渲染器
……**没有引擎可用**。本项目没有这个约束 —— 这是『不该抄』的全部依据。」
下表只列**楼梯特有**的、且此前两轮未点名的条目。

| TG 楼梯做法 | 依赖它的什么架构 | 在本项目会怎样冲突 | 裁决 |
| --- | --- | --- | --- |
| 六家生产者 push 进 `ResMut<WallHoles>` | Bevy 系统调度图的显式偏序 | UE 无调度器 ⇒ 洞的顺序取决于 actor tick/注册序 ⇒ `BodyDescHash` 不稳定 ⇒ **幂等短路失效** | **不抄**。M15 走 pull |
| `StairsLazySubscriber` / `StructuralGraphSubscription` 惰性订阅 | ECS 资源级变更检测 | 与「消费者无条件重求值 + 幂等哈希短路」（`[一轮]` L276）正面冲突：订阅是**推**，本项目是**拉** | **不抄**。哈希守卫已经承担同一职责 |
| `playermade_stairs_assembly_propagate_dirty_flags` 脏标记跨节点传播 | 节点共享图 | 本项目每 actor 一份参数，没有「邻段」这个概念可以传 | **不抄**。等图状态真做时再评 |
| `StairsSnapshot` / `StairsHistoryDiff` / `replay_stairs` 重放式撤销 | 持久图 + 历史栈 | 本项目零撤销（`[一轮]` L372「不要拿撤销当卖点」，L125 既定裁决） | **不抄**。图状态是第三步，且需先解决零撤销 |
| `gen_stair_floors` → `StairsFloorMeshChunk` → `nani_update_stairs_floor` / `_nani_stairs_floor.raster` | **nani 的 `NaniTrimeshChunk`（primitive 内部 chunk 剔除表）** | `[二轮]` L1226/L1408/L1612 已**明确否掉**这套机制（UE 的 bounds/视锥/HZB/VSM 页/Lumen card/距离场全部以 primitive 为键）。而 `_nani_stairs_floor` 正是这套机制**仅有的两个使用者之一**（L1230） | **不抄**。楼梯地板走普通 primitive |
| `iter_shape_overlaps`（parry）算 `calculate_distortion_from_structural_graph` | 物理库直接参与 mesh 生成 | `[二轮]` L394「明确不引入 Chaos / `UBodySetup` / 任何物理库」；L1613 同 | **不抄**。形变属第三步 |
| 四个 UI 模块 + 三套 gizmo 缓存 + 六条 onboarding 提示 | HitProxy / Gizmo / InputBehavior 全家桶 | `[二轮]` L1611「零 HitProxy/Gizmo/InputBehavior + 零撤销」 | **不抄**。第一步用样条 actor 的原生编辑即可 |
| `magic_support::playermade_stair_floating_bricks`（删支撑后的魔法悬浮砖） | `StairsRemovedSupports` 抑制集 | **这条反而该抄**，见下 | **可抄，但排在 L8 之后** |

#### 唯一一条建议**反过来**的

`StairsRemovedSupports` 那条**不在「不该抄」里**。第二节已证明它就是 `[二轮]` L1112 提议的
`SuppressedDerived` 形状，且 TG 用「把抑制集当装配的输入」保住了纯函数性质。
`[二轮]` L1115 的警告仍然成立：**别把「用户否决派生物」推迟到楼梯支撑落地时再想。**
但现在多了一条正面证据 —— 这个形状是被验证过的，不是发明出来的。

⚠️ 仍然要付的代价（`[二轮]` L1113 已指出，本文不推翻）：本项目加一个**序列化的** `TSet`
是**新引入的先例**；`DoorSlotOpen` 连 `UPROPERTY` 都不是，只证明「跨重求值的非序列化记忆可以存在」。

---

### 七、资产实测

从**原始资产 JSON**（`D:/MyProject/Tiny Glade/assets/meshes/`，TG 单位为米）逐顶点算包围盒，
这比 UE 导入后的读数更硬 —— 它不含导入期的轴向转换与缩放。

| 资产 | 顶点属性 | X (m) | Y (m) | Z (m) | 尺寸 (cm) | 中心 |
| --- | --- | --- | --- | --- | --- | --- |
| `stair_step` | `Vertex_Position, Vertex_Normal, **Vertex_Color**` | [−0.3116, +0.3255] | [−0.1772, +0.1764] | **[−0.0143, +1.3171]** | 63.71 × 35.36 × 133.13 | (+0.007, −0.000, **+0.651**) |
| `stairs_step` | `Vertex_Position, Vertex_Normal, **bevel_offset**` | [−0.5, +0.5] | [−0.5, +0.5] | [−0.5, +0.5] | **100 × 100 × 100** | (0, 0, 0) |
| `stairs_pebble` | `Vertex_Position, Vertex_Normal, **bevel_offset**` | [−0.5966, +0.7276] | [−0.6925, +0.5847] | [−0.6849, +0.6670] | 132.42 × 127.72 × 135.19 | (+0.066, −0.054, −0.009) |
| （参照）`brick` | `Vertex_Position, Vertex_Normal, **is_bevel**` | [−0.5, +0.5] | [−0.5, +0.5] | [−0.5, +0.5] | 100 × 100 × 100 | (0, 0, 0) |

**顶点属性集就是归属判据【确凿】**：

- `stairs_step` / `stairs_pebble` 带 `bevel_offset` ⇒ **§4.3 岩地台阶**那一套
  （`[分析]` §4.3-4 明写「VS 内 FBM 噪声驱动 `bevel_offset` 逐级随机圆凿 + 磨损凹槽」，
  §4.3-5「15% 概率撒 pebble」）。
- `stair_step` 带 `Vertex_Color`、**不带** `bevel_offset` ⇒ **§4.1 玩家绘制**那一套
  （`construct_stair_steps`）。

**三条落地含义**：

1. **`stairs_step` 是居中单位立方体，和 `brick` 逐字节同尺寸** ⇒ 它是**字典 mesh**，
   非均匀缩放本身就是台阶尺寸。这正是 `FPaletteBuffers::BlockSize` 的语义
   （`CSGroundShaperSteps.h` 头注释已经写了这条口径）`[代码]`。
   ⇒ 第五节的 MVP 用它，`BlockSize = (踏面进深, 踏步宽, 踏板厚)`，**无需新建资产**。
2. **`stair_step` 的 Z 是单边的**（−0.014 → +1.317，中心偏到 +0.651），
   不是居中盒 ⇒ 它的**原点在踏步的一端**，暗示 §4.1 的踏步是从某个锚点（墙侧/栏杆侧）
   向外摆的。**【待确认】**：要确证需读 `construct_stair_steps` 的变换构造，PDB 只给符号名。
3. UE 侧三张网格**都已导入**（`Content/HouseTest/TinyGladeAsset/Meshes/{stair_step, stairs_step, stairs_pebble}/StaticMeshes/`）。
   全仓 grep（`*.cpp *.h *.md *.py *.json *.txt`）对这三个名字**零命中** ⇒ C++ / 文档侧无引用。
   **【待确认】**：是否被某个蓝图默认值引用未查，需要资产引用查看器才能确证。

#### 轴向：UE 导入把 Y/Z 换了，且换对了

用户记录的 `stair_step` = 63.71 × 133.13 × 35.36 cm，与上表的 (X, **Z**, **Y**) 顺序一致
⇒ 导入按 glTF Y-up → UE Z-up 做了轴交换，缩放 100（m→cm）。落到 UE 局部轴：

| 语义 | TG 源轴 | UE 局部轴 | 值 (cm) |
| --- | --- | --- | --- |
| 踏面进深（面内径向） | X | **+X** | 63.71 |
| 踏步长度（沿曲线） | Z | **+Y** | 133.13 |
| 踏板厚度（平面法线） | Y | **+Z** | 35.36 |

这与 `BuildStepPlan` 取包围盒 **Y** 当长度、`CSGroundSteps.usf` 的
「+X 面内径向 / +Y 沿曲线 / +Z 平面法线」口径**完全一致** `[代码]`。
⇒ `stair_step` 可以**直接**当 palette 条目喂进现有链，不需要改轴。
（`[二轮]` L1619「石阶长度轴统一」是**明确不做**项，本文不触碰它。）

---

### 八、待确认

| # | 待确认项 | 需要什么证据才能确证 |
| --- | --- | --- |
| U1 | §4.2 平台楼梯是否真的产墙洞 | 反编译 `[PDB]` L9047 那个系统的函数体，找 `WallHoles::add` 的调用边；或在游戏里造一段跨高差墙顶步道看墙上有没有洞 |
| U2 | `[PDB]` L8819（`StairsVisualState` + `WallHoles`）确为 `playermade_stairs_add_wall_holes` | 符号名不进 `FunctionSystem` 签名，只能靠「两个参数恰好匹配」推。要确证需按地址把 `stairs_assemble::playermade_stairs_add_wall_holes` 与该 `SystemTypeSet` 对上 |
| U3 | `HoleType` / `HoleOrigin` 的枚举变体 | PDB 只有类型名，无变体符号。需反编译或找 `.ron` 序列化样本 |
| U4 | §4.1 撤销是重放式而非逆操作式 | 反编译 `replay_stairs::replay_stairs` 与 `StairsState::apply_diff` |
| U5 | `stair_step` 原点偏在一端的用意 | 读 `construct_stair_steps` 的变换构造 |
| U6 | 三张楼梯网格是否被蓝图默认值引用 | UE 资产引用查看器（`Reference Viewer`） |
| U7 | 第五节「kernel 零改动」 | 拿 `stairs_step` 跑一次三级直梯冒烟测（`Rec.z = k * StepRise`） |
| U8 | TG 的踏高/踏深实际值 | `stairs_step` 是字典 mesh，真实尺寸只活在 GPU 里。需从 `_rocky_terrain_stairs_stairs.cs` 反编译取常数，或从 §4.1 的 `compute_stair_assembly_params` 取。Godot 原型用的 0.18 m 是用户自己定的，不是 TG 实测值 |

---

### 九、可行动清单（排序）

排序依据：**先做不触碰任何既有裁决的**，再做只触碰一条的，最后是需要先推翻裁决的。
「触碰的裁决」一列指向计划或两轮报告里已经拍板的条目 —— 动它之前需要重新裁决。

| 序 | 事项 | 预估改动范围 | 触碰的既有裁决 |
| --- | --- | --- | --- |
| **A1** | **把「起点是 `BuildStepPlan` 分层铺装链」改成「起点是 `BuildFramePlan`」**。`[二轮]` L1094 的指定写于 D9 GPU 改造裁决之前，S3 要删的正是那条链 | 文档一句话（改计划「开放问题 · 楼梯的范围声明」的末句） | 无 —— 这是**消除**与 D9 :628-632 的冲突，不是新增裁决 |
| **A2** | **补记 §4.0 门前踏步为第四套**，并在范围声明里说明它与 §4.1/§4.2 不同：零图状态、零地形查询、纯墙函数 | 文档，`MESH_GENERATION_ANALYSIS.md` §4 加一节 + 计划范围声明加一句 | 无（纯补充。但**动了别人的文档**，需用户点头） |
| **A3** | **(run, rise) 耦合求解纯函数 + 单测**。规格照 `stairs_builder.gd`：`n = round(\|rise\|/TargetRise)`、`run = length/n`、`run` 过小则拒。放 `CSHouseLogicTests.cpp` 邻域 | ~60 行 + 单测 | 无。符合计划「判定纯函数全部单测」纪律（`CSHouseActor.h:349-353`） |
| **A4** | **自由路径踏步切片**：新 actor（样条 + 地面订阅 + palette 三件套），照 `BuildFramePlan` 的形态，用 `stairs_step` 当字典 mesh，`Rec.z = k * StepRise` | ~250 行新代码，GPU 侧零改动。验收 = 三级直梯 + 一段曲梯，拖样条时零 flush | 无 —— `SolveBlockLayout` 与 `CSShaperSteps::Scatter` 保持原地不动（`[二轮]` L1071）。**不碰** `ACSSplineBlockActor`（`[二轮]` R4 已否 `bStepMode`） |
| **A5** | **全高楔块自撑**：kernel 读 `Rec.w` 当每级块高缩放 | usf ~10 行 + CPU 填值 | **触碰**「`CSGroundSteps.usf` 两个消费者共用这一支」（该文件头注释的显式裁决）。第三个消费者进来时要重新确认「写成一支」仍然划算 |
| **A6** | **`SuppressedDerived` 抑制口**（`[二轮]` L8）。本文已把 TG 侧从【推断】升级为【确凿】，形状可照 `StairsRemovedSupports`：抑制集当装配**输入** | 形状与代价见 `[二轮]` L1112；必须并进两份 desc 哈希 | **触碰**「派生物纯函数、不序列化」（计划反复裁决）。TG 证明「抑制集当输入」不破坏纯函数性，但**序列化的 `TSet` 在本项目仍是新先例**（`[二轮]` L1113） |
| **A7** | **M15 `ICSWallOpeningProvider`**（pull 形态）。本文补上了「为什么不能是 push」的根据 | `[二轮]` L367 已给形状，与 `RegisterShaper`/`UnregisterShaper` 同形同生命周期 | **触碰**「房子是唯一裁决者」这条不变量 —— 注入方只提诉求，房子仍可拒；但 `[二轮]` L364 指出「拒绝」对楼梯没有语义，仲裁规则要另定（L369 已给：楼梯洞按 `SourceId` 排序无条件进表，冲突门槽在 `ComputeDoors` 里被抑制） |
| **A8** | **M16 `SweptPrismVsWalls`**。本文新证据：TG 侧开洞系统本身只有两个依赖，**贵的是它上游的锚定求解**（`StairWallAnchor` / `StairNodeAnchor::try_as_wall`） | `[二轮]` L389-396：~150-200 行纯 CPU、无 GPU、无回读 | **触碰**「明确不引入 Chaos / `UBodySetup` / 任何物理库」（`[二轮]` L394 / L1613）—— 解析写法即可，**不要**因为 TG 用了 parry 就引物理库 |
| **A9** | **楼梯写植被排除 mask**（G4）。TG 只写离地 < 1.75 m 的像素，桥拱下仍长草 | 挂 `RT_DecorMask`（计划 :1080 已规划） | 无新裁决，但**依赖 D12 动工** |
| **A10** | **图状态与邻接对齐**（`find_best_neighbor_alignments` / `maintain_stair_graph`） | 最贵 | **触碰「零撤销」这条根裁决**，且需要先推翻「零 HitProxy/Gizmo/InputBehavior」（`[二轮]` L1611）。计划已把它排在最后，本文**不建议提前** |

**A1–A4 之间没有相互依赖之外的前置，四项合起来就是计划所说的第一步，且不需要任何新裁决。**
A5 起每一项都要先过一次裁决，不应捆进第一步一起做。

---

<a id="vol-4"></a>

## 卷四 · 渲染与光照对照

> 原文件 `TinyGlade_渲染光照对照.md`，原标题「Tiny Glade 渲染与光照对照：三条通道、阴影与 GI 代理」。

本文只回答五件事：**TG 的画面是怎么算出来的**、**那种柔和观感的真实来源是什么**、
**TG 侧有没有「三条通道」这个东西**、**本项目今天差在哪**、
**在「GPU 生成网格进不了 Lumen」这个约束下阴影与间接光实际可行的方案有哪些**。

设计裁决在 [`Plan_House.md` 的 D14](Plan_House.md)，本文**不复述**它，
只写复核结论与新证据；结论被推翻、加强或降级时点名引用行号。

风格与详略口径对齐 [本文卷三 · 楼梯模块对照](#vol-3) 与
[本文卷二 · 窗户与装饰对照](#vol-2)。

---

### 结论前置

1. **TG 不是「简单光照 + 好美术」。** 它是一套完整的现代延迟渲染器：2×uint32 的 G-buffer、
   三级联 **PCSS 软阴影**、半分辨 **GTAO**、**ReSTIR GI over 软件 BVH**（半分辨 L1 探针，
   每个颜色通道各带一条方向）、split-sum 间接高光、clustered 点光、TAA + 自研超分、
   12 个 shader 的景深、以及 **Tony McMapface** 显示变换。【确凿：`compiled-shaders/` 404 个
   编译产物 + 反编译 GLSL】

2. **「柔和」的最大三个单一来源，按贡献排序（前两条是实证，第三条是推断）**：
   ① **PCSS**——penumbra 随遮挡体距离放大（5 抽 blocker search + 10 抽 PCF）；
   ② **AO 被硬夹在 `mix(0.6, 1.0, ao)`**——阴影区永远压不黑；
   ③ 彩色 L1 间接光被 `indirect_light_tint` 上色、饱和度 ×1.17、增益 ×1.2。
   **美术资产与配色（`ColorLibrary` 的 37 个 `vec4`）是参数，不是效果**——同一批 albedo
   丢进 UE 默认管线不会得到同一张图。

3. **TG 侧不存在「三条通道」这个分法。** 它只有**两条**：一条逐实例 SSBO 结构体
   （UE 的通道一与通道三在 TG 是同一样东西），加 41 种自定义语义顶点属性。
   三分法是 UE 的接口限制，不是 TG 的设计。

4. **本项目今天的最大缺口不是「没接三条通道」，是「一点影子都没有 + 一个后处理体积都没有」。**
   两张演示关卡各有一盏平行光 + SkyLight + SkyAtmosphere，**零 PostProcessVolume、
   零 ExponentialHeightFog**；而 `CastShadow = false` 写死在两个组件的构造里。

5. **「进不了 Lumen」这句话被漏掉了一半**：Lumen 的 **screen traces 默认开着
   （`r.Lumen.ScreenProbeGather.ScreenTraces` 默认 1）且只读深度 + 上一帧 SceneColor**，
   不看距离场/光追表示。所以房子**在屏幕内**已经在给 Lumen 贡献遮挡与弹射；
   真正缺的是**离屏与远场**（无 mesh cards、无 surface cache）。这改变了推荐方案。

6. **⚠️ 与计划书的最重要冲突**：D14 写「本计划已选的『参数化生成真几何洞』路线自动满足
   GI 代理硬约束，代价为零」。**今天上线的墙不是真几何洞**——`CSHouseActor.cpp:44-45`
   注释原文「洞由材质逐像素 discard 切出来，**几何上不挖**（Tiny Glade 原版做法）」，
   `AddPanel` 铺的是从 `Z0` 到墙顶的一整个实心盒。**约束今天已经不满足**，
   不是「引入 GI 代理那一刻才变错」。详见 [九](#九与计划书--状态文件冲突之处待拍板)。

---

### 证据标注约定

沿用 `MESH_GENERATION_ANALYSIS.md` 的三档可信度，并给每条标出**证据种类**：

| 标注 | 含义 |
| --- | --- |
| 【确凿】 | 反编译 GLSL / PDB 符号 / 本仓源码 / 引擎源码直接给出，不含推理 |
| 【推测】 | 由确凿证据推理得出，推理链在正文写明 |
| 【待确认】 | 现有证据不足以判定，正文写明「需要什么证据才能确证」 |
| `[GLSL]` | `D:/MyProject/Tiny Glade/tmp/shaders/*.glsl`（272 个，本轮另补 40 个，见下） |
| `[BIN]` | `D:/MyProject/Tiny Glade/compiled-shaders/*.bin`（404 个，**文件名本身就是 pass 清单**） |
| `[代码]` | 本仓 `Source/` / `Shaders/` / `Scripts/` |
| `[资产]` | `Content/` 下 `.uasset` 的字节实测（名字表 + AssetRegistry tag 块） |
| `[引擎]` | `D:/UE-SourceCode-5.7.4/Engine/Source/` 逐行核对 |
| `[分析]` | `MESH_GENERATION_ANALYSIS.md` |
| `[计划]` | `Plan_House.md` |

**本轮新增的反编译产物**：原 `tmp/shaders` 的 272 个文件里**没有任何一个延迟光照 / GI / 后处理
shader**（那次只按 `wall/roof/nani_/terrain/...` 等几何相关模式筛选）。本轮用同一条
`spirv-cross` 管线补出 40 个，落在 scratchpad（**没有写进 TG 目录，也没有动本仓任何文件**）：

```
C:/Users/19223/AppData/Local/Temp/claude/D--MyProject-UnrealProject-UETest574/
  2a8ccb52-9e09-4660-81f6-554223ea8aed/scratchpad/tgshaders/
```

含 `_deferred_{solid,grass,plant,canopy}_light_sun_sky`、`_final_post`、`_taa`、
`_ssgi_*`、`_gi_*`、`_apply_screen_shadows`、`_apply_volumetrics`、`_lut_brdf_fg`、
`_mei_shadow`、`_shadow_mask_mean`、`_skydome`、`_blur_pyramid_*`、`_rdi_temporal`、
`_light_binning_*`。**本文所有 `[GLSL]` 引用里带 `deferred`/`gi_`/`ssgi`/`final_post`
的都在这个目录，不在 TG 的 `tmp/shaders` 里。**

⚠️ **scratchpad 会随会话消失。** 重新生成的办法：`tmp/decompile_shaders.py` 里的
`BASE` 写的是 `D:\MyWork\Tiny Glade`（已失效，实际是 `D:\MyProject\Tiny Glade`），
改对之后按模式跑即可，`spirv-cross.exe` 在
`C:/Program Files/RenderDoc/plugins/spirv/`：

```bash
python "D:/MyProject/Tiny Glade/tmp/decompile_shaders.py" \
    _deferred_ _final_post _taa. _ssgi_ _gi_ _apply_ _lut_brdf \
    _mei_shadow _shadow_mask _skydome _blur_pyramid _rdi_temporal _light_binning
```

（`.bin` 的布局：扫 SPIR-V magic `0x07230203`，magic 前一个 u32 是 word count，
再往前是 `u32 len + entry name`。）

---

### 一、TG 的渲染管线：一套完整的现代延迟渲染器

#### 1.1 pass 全景【确凿，`[BIN]` 文件名】

404 个编译产物按前缀分组，能直接读出整条帧结构：

| 阶段 | 产物 | 说明 |
| --- | --- | --- |
| G-buffer 光栅 | `_nani_*`（24 个 raster）、`_wall_wall_brick_lod{0,1}`、`_rocky_terrain_*`、`_solid_vertex_color*`、`_grass*`、`_clearing_*`、`_ivy_*` | 每类构件一条专用 PS，**没有 uber 材质**（`_nani_uber` 只是 nani 的 fallback subset） |
| 剔除 | `_nani_cull_and_bucket*`（9 个）、`_wall_generate_draw_lists`、`_generate_hzb` | 两阶段遮挡剔除，见 `[分析]` §1.5 |
| 阴影 | `_mei_shadow`、`_apply_screen_shadows`、`_shadow_mask_mean` | 阴影图光栅 → 全屏 PCSS 解析 → **全屏均值归约** |
| AO | `_ssgi_ssgi`、`_ssgi_temporal_filter`、`_ssgi_upsample`、`_extract_half_res_depth_normals` | 半分辨 GTAO 风格，输出 **`r16f` 单标量** |
| GI | `_gi_trace_gi_candidates`、`_gi_update_gi_radiance`、`_gi_eval_screen_radiance`、`_gi_reproject_screen_radiance`、`_gi_blur_screen_radiance`、`_rdi_temporal` | **ReSTIR GI + ReSTIR DI** |
| 光照 | `_deferred_{solid,grass,plant,canopy}_light_sun_sky`、`_light_binning_*`（5 个） | 四条按材质类特化的延迟光照 + clustered 点光分箱 |
| 水/冰 | `_water_deferred_rt`、`_water_ice_{deferred_rt,refl_march,refl_resolve,refr_resolve}` | 水面单独走光追 |
| 大气 | `_skydome`、`_cloud`、`_star`、`_apply_volumetrics` | |
| 后处理 | `_taa`、`_fxaa`、`_tgsr`（4 个 permutation）、`_tgsr_reproject`、`_tgsr_blur_lum`、`_fsr_init_reactive`、`_fsr_copy_history`、`_blur_pyramid_{blur,rev_blur}`、`_dof_*`（**12 个**）、`_final_post`、`_resolve_to_swapchain`、`_blit_*` | `tgsr` = 自研超分（Tiny Glade Super Resolution），与 FSR reactive mask 混用 |
| 风格化滤镜 | `_painterly_{acquire,convolve,generate_strokes,raster_strokes,debug}_*`、`_vermeer_{acquire,convolve,generate_strokes,raster_strokes,combine}_*` | **两套笔触渲染**（照片模式），与主管线并列 |
| 工具 | `_lut_brdf_fg`、`_lut_water_noise`、`_normal_debug`、`_primary_rt_debug`、`_shader_refactoring.{capture,compare}` | `_lut_brdf_fg` = split-sum 的 BRDF 环境项 LUT ⇒ **有真 GGX 间接高光** |

⚠️ **易误读**：`_painterly_*` / `_vermeer_*` 不参与常规画面，它们是照片模式的笔触滤镜
（另有 `_session_selection_thumbnail_painterly`）。**别把 TG 的观感归因给它们。**

#### 1.2 G-buffer 只有两个 uint32【确凿，`[GLSL]` 编解码两侧都在】

写侧（`_wall_wall_brick_lod0...b903e43f...ps_main` 末尾）与读侧
（`_deferred_solid_light_sun_sky...ps_main:212-222`）严丝合缝：

```glsl
// 写（wall_brick PS）
out_var_SV_TARGET0.x = pack8(N*0.5+0.5)         | (0xCC << 24);   // 0xCC = 204 → 粗糙度 0.8
out_var_SV_TARGET0.y = pack8(sqrt(saturate(alb))) | (flags << 24);
// 读（deferred_solid PS）
float rough = float((g.x >> 24) & 255) / 255.0;
vec3  N     = normalize(unpack8(g.x).xyz * 2.0 - 1.0);
vec3  alb   = unpack8(g.y).xyz;
uint  flags = g.y >> 24;
```

| 事实 | 意义 |
| --- | --- |
| **没有 metallic 通道**；直接高光的 F0 恒为 `vec3(0.04)`（`deferred_solid:463`） | TG 全世界都是绝缘体，**一件金属都没有** |
| 粗糙度只有 8 bit，且墙砖是常数 `204/255 = 0.8` | 材质变化全靠 albedo，不靠 BRDF |
| albedo 走 `sqrt` 编码 | 8 bit 下的廉价感知量化 |
| flags 8 bit 控制**逐像素开关哪些光照项** | 见 §3 的 flags 表 |
| **没有 velocity 通道**（另有 `_nani_velocity_only` 单独一条 pass） | |

#### 1.3 逐材质类的延迟光照【确凿】

`_deferred_{solid,grass,plant,canopy}_light_sun_sky` 是**四条独立的 PS**，不是一条 uber
的四个分支——`solid` 2055 行、`canopy` 1700+ 行，光照公式各不相同：

| 变体 | 直接漫反射 | 备注 |
| --- | --- | --- |
| `solid`（墙/屋顶/地面/石头） | **Burley 漫反射**（见 §3.1） | 唯一带 GGX 高光的一条 |
| `grass` | `smoothstep(0.0, 0.5, NdotL)`，**无高光** | 刻意软化的包裹项 |
| `plant` | 双向 wrap + 一条背散射项 | 次表面近似 |
| `canopy` | wrap + **Henyey-Greenstein 相函数** `0.159/(π·g²)` | 树冠透光 |

---

### 二、四类表面的 PS 主干

#### 2.1 墙砖 `_wall_wall_brick_lod0`【确凿】

PS 只做三件事：**逐像素拱洞裁剪 → 查色 → 写 G-buffer**。没有一行光照。

```glsl
// 拱洞：in_var_C7 = 该像素列的拱高（VS 分段插值），flags&8 = 拱圈石反向保留
if (!(flags&8) && C7 > -2.5 && P.y < C7) discard;
else if ( (flags&8) &&           P.y > C7) discard;
```

- **色号 → `brick_colors` 纹理数组**：13 个色号（0..12）映射到 5 层图集的 5 个 UV 区，
  `wall_bricks`/`wall_moss` 两个 `ColorLibrary` 色再按 `pow(c, 1.3)` / `×0.8` 调过。
- **两次 wang-hash 逐砖抖色**：`flags&512` 时按 `source_id ^ 8902390` 哈希在色号色与图集色
  之间 mix 0..0.5；`flags&256` 时再按 `^239085` 往 `wall_1` mix 0..0.2/0.4。
  **同一块砖每帧结果一致，砖与砖之间不一致**——手作感的来源之一。
- **冬季铺雪**：`glade_theme_id == 4` 时用 simplex 噪声 + `N.y` 加权混白。
- **编辑高亮**：`highlight_mode` 混 `(0.7, 0.21, 0)` 橙、橡皮擦按 `brush_position` 距离测试。
  **零几何、零重建**（`[分析]` §1.5 的七槽分桶）。

**阴影 PS 变体（`f0adff76827e73ff`，全文 25 行）只保留同两条 discard**——
**阴影通路认同一个洞**。这与 `[计划]` D14 表里 UE 侧「阴影深度认 mask」那一行是同一个事实，
两边一致，`[计划]` 这条无需修正。

#### 2.2 灰泥 `_nani_plaster`【确凿】

- `if (in_var_C6 < 0.1) discard;`——灰泥覆盖度低于 10% 直接丢，**灰泥的边缘是逐像素的**。
- 三层 domain-warp 的 wang-hash value noise，晶格随 `wall_length` 哈希偏移，
  外加 `blend_uv_seam` 的 `smoothstep` 缝合项——**墙首尾接缝处噪声相位对齐**。
- 逐实例数据来自 `nani_instance_data._m0[in_var_C0]`（含 `wall_id` / `wall_length` /
  `blend_uv_seam`）。

#### 2.3 屋瓦 `_nani_instanced_roof`【确凿】

三条与本项目直接相关：

1. **逐实例裁剪平面**：`if (dot(vec4(worldPos, 1.0), in_var_C7) < 0.0) discard;`
   —— `[分析]` §3.2 的 `clipping_plane_` 实证。
2. **老虎窗逐像素挖洞**：一张 15×15 的空间网格（130 m 铺满，`(P.xz + 65) × 0.11538`），
   每格最多 128 个 `DormerHole`（有向盒 + 一条 `k/a` 的软化曲线），
   逐像素遍历本格的洞列表做 discard。**TG 在屋面上开洞用的是逐像素，不是几何。**
3. **法线来自屏幕空间导数**：`normalize(cross(dFdx(P), -dFdy(P)))`
   —— **完全不读顶点法线**。低模干净的平面着色由此而来，
   顺带解释了 `[分析]` §9.1 里为什么那么多 mesh 只有 `Vertex_Position`。

#### 2.4 地面：两条路【确凿】

**(a) 可见地形** `_rocky_terrain_rocky_terrain` PS：从 `vertices/triangles` SSBO 里
按重心坐标反查三角形与**邻接三角形**（`triangles[i].neighbours[]`），算到共享边的距离，
用 `is_top` 判断是台面还是崖壁——**岩壁与草地的过渡是逐像素在拓扑上算的**，不是贴图。

**(b) 地形光照贴图** `_terrain_lighting_texture.cs`：把地面的**整套光照烘进一张
`r11f_g11f_b10f` 贴图**（8×8 线程组，采样 130 m 高度场）：

```glsl
vec3 sun = sun_color * (1 - shadow) * smoothstep(0.0, 0.5, NdotL) * NdotL * albedo / π;
vec3 rim = sun * pow(1 - saturate(N.y), 5) * pow(1 - saturate(sunDir.y), 3);   // 低阳掠射增益
vec3 sky = sky_high_color * albedo * (N.y * 0.5 + 0.5);                        // 半球环境光
img_output = sun + rim + sky;
```

这张贴图后来被 `_gi_trace_gi_candidates` 读走（binding 17）——**GI 光线打到地形时不重算
光照，直接查这张表**。这是 TG「廉价 GI」里真正廉价的那一半。

⚠️ **这条对本项目没有对位物也不该有**：UE 有实时阴影与 Lumen，不需要烘地形光照贴图。
它的价值在于说明 TG 的地面光照公式（上面三行）——那三行是可以照抄进材质的。

#### 2.5 植被【确凿】

草/花/树冠的 alpha 测试全在 PS 里 discard（`_clearing_tree_trunk` 的阴影变体全文 5 行，
只有 `if (in_var_C4 < 0.3) discard;`）。光照见 §1.3 的四变体表。

---

### 三、光照模型逐项拆解

以下全部来自 `_deferred_solid_light_sun_sky.raster.hlsl_b903e43ffb3da915.ps_main.glsl`
（2055 行，全部内联进 `main`）。绑定表就是这条 pass 的输入清单：

```glsl
binding 14 utexture2D gbuffer_tex          binding 27 texture2D  indirect_radiance_tex
binding 15 texture2D  depth_tex            binding 28 utexture2D rdi_reservoir_tex
binding 16 texture2D  half_depth_tex       binding 29 texture2D  brdf_fg_lut
binding 18 texture2D  blue_noise_texture   binding 26 SSBO       lantern_buf
binding 19 texture2D  ssgi_tex             binding 36/37 SSBO    light_indirection_grid / grid_light_indices
binding 20 texture2D  screen_shadows_tex   binding 24 UBO        spatial_resolve_offsets[512]
```

#### 3.1 直接光：Burley 漫反射 + GGX 高光【确凿，`:463-466`】

```glsl
// 漫反射：Disney/Burley，含 retro-reflection
diff = (1 + fd*(1-NdotL)^5) * (1 + fd*(1-NdotV)^5) / π;
// 高光：Schlick F(F0=0.04) × Smith height-correlated V × GGX D
spec = mix(0.04, 1.0, (1-VdotH)^5) * V_SmithGGX * D_GGX / (4 NdotV NdotL);
sun  = sun_color * (albedo * diff + spec) * NdotL;
```

**Burley 而不是 Lambert 是「柔和」的一条硬来源**：掠射角下亮度不塌，明暗终结线更宽更软。
UE 的 `MSM_DefaultLit` 走的是 **Lambert**；要复刻这一条需要在材质里手写
（或用 `Substrate` 的 rough diffuse）。

#### 3.2 阴影：三级联 PCSS + 屏幕空间接触阴影【确凿】

`_apply_screen_shadows.cs` 与 `_terrain_lighting_texture.cs` 里是**同一段代码**：

| 步骤 | 实证 |
| --- | --- |
| 级联选择 | `view_constants[2 + c]`，`c ∈ {0,1,2}`；scale 10 / 5 / 5×(2/7) ⇒ 覆盖半径比 1 : 2 : 7，与 `[分析]` §1.5 的「`[0,1]/[1,2]/[2,7]`」一致 |
| 法线偏移 bias | 沿法线推 `0.0667 × saturate(1 − NdotL)`，**在阴影视图空间里推** |
| 斜率 bias | `max(0.3/scale × (1 − \|NdotL\|), 0.025)`，深度上再减 `bias/1024` |
| **blocker search** | 5 抽，黄金角 `2.39996` 螺旋，半径 `0.2 × scale` 纹素；求命中遮挡体的**平均深度** |
| **penumbra 宽度** | `w = (z − z_blocker) × 17.07 × scale × pow(10, smoothstep(0,0.25,fog.w)) / z_blocker`，再软钳位 `w / pow((w/(scale−1))^5 + 1, 0.2) + 1` |
| **PCF** | 10 抽同螺旋，`sampler2DArrayShadow` 硬件比较，半径 = penumbra；带**感受野斜率补偿** `min(0, dot(dz/dxy, offset))` |
| 时域抖动 | 螺旋起相 `((x*11 + y*7) + frame*5) % 64 × φ`，半径抖动用 `frame % 64` 偏移的 IGN——**靠 TAA 收敛** |

**penumbra 随遮挡体距离放大** 就是 PCSS 的定义；`pow(10, smoothstep(0,0.25,fog_thickness))`
这一项让**雾越厚阴影越糊**，是纯艺术项。

同一个 CS 还做一次工作组归约（`thread_group_shadow_mask_sum`）→ `_shadow_mask_mean.cs`
→ 一个 float，**喂给后处理的自动曝光**（见 §4）。

**接触阴影**是另一条：deferred PS 里 `flags bit0` 命中时，在裁剪空间沿光方向做一次
屏幕空间光线步进（`:236-330`，`0.3 × clip_to_view[3].z` 步长，HZB 式加速），
结果 `_660` 与 cascade 结果**相乘**：

```glsl
if (flags & 4) sun *= (1 - contactShadow) * (1 - screen_shadows_tex.x);
```

#### 3.3 AO：半分辨 GTAO，且**被夹住**【确凿】

`_ssgi_ssgi.cs`：Hilbert 曲线索引 + R2 黄金比噪声 + `frame % 64` 时域轮换，
输出 **`r16f` 单标量**（不是彩色 GI，名字有误导性）。deferred 侧：

```glsl
float ao = clamp(0.1 + ssgi_tex.x, 0.0, 1.0);     // 先加 0.1 底
if (flags & 32) sun *= mix(0.6, 1.0, ao);          // 直接光最多被压到 0.6
float aoSpec = (flags & 2) ? mix(ao, 1.0, 0.3) : 1.0;   // 高光最多被压到 0.7
```

**AO 永远压不黑**。这是「柔和」的第二条硬来源，也是最容易照抄的一条
（UE 侧对应 `r.AmbientOcclusion.Intensity` / PPV 的 `AmbientOcclusionIntensity`，
或 Lumen 的 `r.Lumen.ScreenProbeGather.ShortRangeAO.*`）。

deferred PS 里还**另外**跑一次 2 抽余弦半球的屏幕空间可见性步进（`:590-700`），
与 `ssgi_tex` 叠加——同一件事算两遍，分辨率不同。

#### 3.4 间接漫反射：ReSTIR GI over 软件 BVH，落成半分辨 L1 探针【确凿】

```
_gi_trace_gi_candidates.cs   → 对 PackedBlBvhNode + PackedTriangle SSBO 做软件 BVH 遍历
   （另读 terrain_lighting_texture / shadow_map / taa_history_tex / reprojection_tex
     —— 屏幕内命中直接取上一帧颜色，屏幕外才吃 BVH）
_gi_update_gi_radiance.cs    → ReSTIR 时域 reservoir 更新（rg32ui）
_gi_eval_screen_radiance.cs  → reservoir → 半分辨 rgba16f
_gi_blur_screen_radiance.cs  → 双边模糊（法线 + 深度权重）
```

⚠️ **这就是 `[分析]` §9.3 说的那套**：prefab 的 `rt` 段是**粗代理**
（`mesh:Aabb` + 三季 albedo + `semantic:Canopy`），BVH 由 obvhs 在 CPU worker 上建好
再 SSBO 上传。**TG 的 GI 几何是另建的一套低模，不是渲染网格。**
这一点对本项目的 GI 代理设计是最直接的先例。

deferred 侧的解码（`:524-575`、`:1334-1424`）：

```glsl
// indirect_radiance_tex 横向切成 3 张子图 —— 每个颜色通道一份 (L0, L1.xyz)
for (i in 0..2) sh[i] += texelFetch(indirect_radiance_tex, uv + (i*W/3, 0)) * w;
// 双边空间解析：蓝噪声选 3 个偏移（不够再取 3 个），权重 exp2(-80·|1 - z_half/z|)
vec3 L0 = vec3(sh[0].x, sh[1].x, sh[2].x);
// 每通道各自的主方向
vec3 dR = sh[0].yzw / sh[0].x,  dG = ...,  dB = ...;
// 艺术上色：tint 归一化后按亮度回填，再提饱和 1.17、增益 1.2
vec3 t  = max(0, 1 - indirect_light_tint * 0.92);
vec3 GI = max(0, mix(normalize_lum(t) * lum(L0), L0, 1.17) * 1.2);
// 辐照度重建：SH L0/L1 常数 0.886227(=√π/2) 与 1.023328，外加一条沿主方向重建的 L2 带谐
irr = 0.886227*GI + 1.023328*(N·d 三项) + GI * (0.08·a + 0.6·a²) * 0.25 * 0.315392*(3(N·d)²−1);
```

**三条值得抄的设计**：
1. **每个 RGB 通道各带一条方向**（不是共享一条）——墙的背光面会带上地面草色的偏移方向，
   这正是 TG 阴影区「有颜色」而不是「灰」的机械原因。
2. `indirect_light_tint` 是**艺术家旋钮**，直接改弹射光的色相与强度。
3. 半分辨 + 蓝噪声选点 + 深度双边上采样——GI 的分辨率成本只有 1/4。

#### 3.5 间接高光：同一份 L1 探针 + split-sum LUT【确凿，`:780-790, 1334-1424`】

```glsl
vec4 fg = texture(brdf_fg_lut, vec2(NdotV, roughness));
vec3 F  = 0.04 * fg.x + fg.y;
vec3 ms = mix(F/(fg.x+fg.y), 1.0, 0.4) * (1 - fg.x - fg.y);   // 多次散射能量补偿
vec3 Fms = F * (1 + ms/(1-ms));
// 反射方向 = 按粗糙度在 N 与 reflect(V,N) 之间插值，然后查**同一份 L1 探针**
vec3 R = normalize(mix(N, reflect(V, N), r*(sqrt(r)+r2)));
```

**没有 cubemap、没有 reflection probe、没有 SSR**（水面除外）。间接高光就是把 L1 探针
往反射方向再取一次值。**这就是「廉价 GI」的全部**。

#### 3.6 点光：clustered + ReSTIR DI【确凿】

`lantern_buf` + `light_indirection_grid` + `grid_light_indices` 是标准 clustered 分箱；
`rdi_reservoir_tex` + `spatial_resolve_offsets[512]` 是 ReSTIR DI 的空间解析
（半分辨 reservoir，`(_317/2) + offsets[...]`），每盏被选中的灯还各跑一次屏幕空间遮挡步进。
灯的颜色是**四个硬编码常量**（`:1247-1281`，四种灯笼色），乘 `ev_mult`。

#### 3.7 flags 位表（GLSL 实证，语义列为推测）

| 位 | 实证效果 | 语义推测 |
| --- | --- | --- |
| 0 | 跑逐像素接触阴影步进 | 需要接触阴影的实体 |
| 1 | 高光乘 `mix(ao, 1, 0.3)` | 参与 AO 的高光 |
| 2 | 乘 `(1−contact)·(1−cascade)` | 接收阳光阴影 |
| 4 | 走「远景/编辑高亮」分支（`\|x\|>65 \|\| \|z\|>65` 判定） | 出界回退 |
| 5 | 直接光乘 `mix(0.6, 1, ao)` | 参与 AO 的漫反射 |
| 6 | 跳过高亮 | 非编辑对象 |

#### 3.8 天空【确凿，`_skydome`】

```glsl
sky = mix(sky_low_color * 1.5, sky_high_color,
          pow(saturate(1 - pow(1 - saturate(-dir.y + 0.2), 14)), 0.65));
sky = mix(lum(sky), sky, 1.25);                     // 饱和度 ×1.25
```
加三层 inverse-square 的太阳晕（`30/…`、`5/(0.5+25θ)²`、`0.8/(1+5θ)²`）与一张月亮贴图。
**两色解析梯度，没有大气散射积分。** `sky_low/high_color` 由 CPU 按 `time_of_day`
在 `ColorLibrary` 的 `sky_low_day/dusk`、`sky_high_day/dusk` 之间插值【推测：只有 day/dusk
两组常量 + 一个 `time_of_day`，插值发生在 CPU】。

---

### 四、后处理链【确凿，`_final_post...ps_main`，逐行】

按执行顺序：

| # | 操作 | 实证 |
| --- | --- | --- |
| 1 | 镜头畸变 + 色散 | `distortion_amount` / `ca_amount`，逐通道偏移采样 |
| 2 | **Bloom** | `blur_pyramid_tex` 单次采样 mip `log2(0.05 × max(W,H))`，×0.12×0.25 = **3%** —— 极宽、极淡 |
| 3 | **阴影均值自动曝光** | `× mix(1.0, 0.57, smoothstep(0.3, 1.0, shadow_mask_mean[0]))` —— **画面里阴影越多整体越暗**。数据来自 §3.2 的工作组归约 |
| 4 | 暗角 | `× exp(−vignette_amount × (3.5·r²)²)` |
| 5 | **预 tonemap gamma** | `pow(x, 1.1 + gamma_shift) × 1.6` —— 压暗部再整体提亮，抬对比 |
| 6 | **Tony McMapface** | `LUT3D(x/(x+1) × 0.979167 + 0.010417)` —— Tomasz Stachowiak（kajiya 作者）的显示变换，**保色相、高光去饱和向白** |
| 7 | 饱和度 / 对比度 grading | `mix(lum, c, grading_saturation)`；`c × pow(lum, grading_contrast)/lum` |
| 8 | 用户 grading LUT + 可选 ICC LUT | `grading_lut_tex` / `icc_lut` |
| 9 | 锐化 | `sharpen_amount` |
| 10 | 蓝噪声抖动到 8 bit | 三角分布 dither，`±1/255` |

**⑤+⑥ 是那种「奶油感」的直接来源**：`pow(1.1)` 让暗部下沉、`×1.6` 再抬回，
然后 Tony McMapface 把过曝区平滑地推向白而不是推向色相偏移。
`_apply_volumetrics.cs` 是另一条（体积雾），在 `final_post` 之前。

---

### 五、「柔和观感」的真实来源

**必须分清看到的与推断的。** 下表的「证据」列是 GLSL 实证，「贡献」列是推断。

| # | 机制 | 证据【确凿】 | 贡献估计【推测】 | 推理链 |
| --- | --- | --- | --- | --- |
| 1 | **PCSS 软阴影** | §3.2 全部 | **最大** | 阴影边缘是画面里对比度最高的边界；把它从硬边换成随距离放大的软边，是唯一能同时软化所有构件轮廓的改动 |
| 2 | **AO 下限 0.6 / 0.7** | §3.3 | 大 | 决定阴影区的**绝对亮度下限**；TG 的截图里从来没有近黑的区域 |
| 3 | **彩色 L1 间接光 + tint + 饱和 1.17 + 增益 1.2** | §3.4 | 大 | 决定阴影区的**色相**；每通道独立方向使背光面吃到地面草色，画面读起来「有空气」而不是「有灰」 |
| 4 | **Tony McMapface + `pow(1.1)×1.6`** | §4 ⑤⑥ | 中 | 决定高光的收束方式；LUT 的设计目标就是高光去饱和 |
| 5 | **Burley 漫反射** | §3.1 | 中 | 明暗终结线更宽；对圆柱形构件（柱、树干、瓦垄）尤其明显 |
| 6 | **面法线来自 dFdx/dFdy** | §2.3 | 中 | 低模不做平滑法线 ⇒ 每个面一个色阶，边界干净不出摩尔纹 |
| 7 | 宽而淡的 bloom（3%）+ 12 shader 的景深 | §4 ②、`[BIN]` | 中 | 景深把远景整片糊掉，是 TG 截图辨识度最高的特征之一 |
| 8 | **美术资产与配色** | `ColorLibrary` 37 个 `vec4` | **不是主因** | 它是这套管线的**输入参数**。把同一批 albedo 丢进 UE 默认管线（硬阴影 + 无 AO 下限 + Lambert + ACES）不会得到同一张图 |
| 9 | 阴影均值自动曝光 | §4 ③ | 小但独特 | 太阳被云/山遮住时整体压暗 0.57——「天色变了」的感觉 |

**结论（推断，但推理链完整）**：观感 ≈ **60% 光照算法 + 25% 后处理 + 15% 美术资产**。
对本项目最有性价比的三条依次是 **①软阴影 → ②AO 下限 → ④后处理体积**，
而这三条**都不需要动 GPU 网格管线一行代码**（见 §8）。

⚠️ **这里最容易犯的错**：把 TG 的观感归因给「顶点色 + 低模 + 好配色」。
`[分析]` 结语第 1 条把整套风格化归结为「字典 mesh × 实例参数 SSBO + seed 驱动的 VS 变形」——
那条讲的是**几何**的风格化，**不是着色的**。着色这一半在 `[分析]` 里根本没有覆盖。

---

### 六、TG 侧对应「三条通道」的是什么

`[计划]` D14 把 UE 侧分成三条：① `CustomPrimitiveData`（逐图元 36 float）、
② 逐顶点语义（Color 的 32 bit）、③ per-instance custom data。**TG 侧只有两条。**

| UE 侧通道 | TG 侧对位物 | 实证 |
| --- | --- | --- |
| **通道一**（逐图元 36 float） | `nani_instance_data._m0[in_var_C0]` / `instanced_data._m0[in_var_C8]` —— **一个逐实例结构体 SSBO**（`wall_id` / `wall_length` / `blend_uv_seam` / `roof_id` / `is_main_roof_body` / `seed` / `animation_t` …） | `_nani_plaster:6-12`、`_nani_instanced_roof:210-236`；`[分析]` §3.2 给了 `RoofShingle` 的完整字段表 |
| **通道三**（per-instance custom data） | **同一个 SSBO 的另一个字段**（`clipping_plane_` = half4 世界平面） | `_nani_instanced_roof` PS 的 `dot(vec4(P,1), in_var_C7) < 0 → discard` |
| **通道二**（逐顶点语义） | **41 种自定义语义顶点属性 / 全库 47 种组合** | `[分析]` §9.1；`is_bevel`/`prim_center`/`appear_pos`/`age`/`wind_bend`/`rest_pos`/`roof_profile_mult`/`autumn_color` |
| （UE 无对位）| 全局 `ColorLibrary` UBO（37 个 `vec4`）+ `FrameConstants` UBO | 每个 PS 都绑；**改色不重建 mesh** 靠的是 `WallColorIds` SSBO + shader 查表（`[分析]` §8.3） |

**结论**：UE 的三分法是**接口限制**产生的，不是设计。TG 的等价物是「一个任意宽度的
逐实例结构体 SSBO」；UE 把它拆成「36 float 逐图元」+「N float 逐实例」两个受限窗口。

**由此推出一条对本项目有用的判断**：`[计划]` 把通道一与通道三当成两件事分别排期是对的
（UE 侧确实是两套接口），但**语义上它们应该共用一份布局声明**——
今天钉在 `[计划]:1129` 的那 12 个 float 的注释块，将来接通道三时会需要一份平行的
per-instance 布局声明，两份必须放在同一个头文件里，否则「哪个下标归谁」会重演
`[计划]` D14 自己吐槽过的「全项目没有任何一处文件仲裁哪个通道归谁」。

---

### 七、本项目现状

#### 7.1 材质清单【确凿，`[资产]` + `[代码]`】

**真正参与房子/地面渲染的只有 5 个**，全在
`Plugins/PCGPlugins/Content/HouseTest/`（无 `Materials/` 子目录）：

| 资产 | BlendMode | ShadingModel | 关键属性 | 图 |
| --- | --- | --- | --- | --- |
| `M_TinyGladeWall` | **`BLEND_Masked`** | `MSM_DefaultLit` | `TwoSided` 缺省(false)、`OpacityMaskClipValue` 缺省(0.3333)、`bCastDynamicShadowAsMasked` 缺省(false) | ⚠️ **2026-09-03 重建，下面这一行是新的**：洞那条链没变（UV1 → Custom `TinyGladeArchClip` → `MP_OPACITY_MASK`，VertexColor.B → `Shape`），但 BaseColor / Normal / Roughness 从常数换成了**灰泥剥落**（`_nani_plaster` 的移植）：世界投影 UV → 6 张贴图 + Custom `TGPlasterPeel`（3 倍频世界 3D value noise 的侵蚀场 → `peelM` / `band` / 卷边法线）+ `TGBrickHeight` / `TGWallAlbedo` / `TGWallNormal`。49 个节点、PS 363 指令、7 采样器。参数全在 `MI_TinyGladeWall` 上（房子挂的应是它）。逐条口径见 `Scripts/TinyGladeMakeWallMaterials.py` 文件头 |
| `M_TinyGladeReveal` | `BLEND_Opaque` | `MSM_DefaultLit` | — | 常数 `(0.34,0.30,0.26)` + Roughness `0.9` |
| `M_TinyGladeRoof` | `BLEND_Opaque` | `MSM_DefaultLit` | — | 常数 `(0.30,0.16,0.13)` + Roughness `0.9` |
| `M_TinyGladeBrick` | `BLEND_Opaque` | `MSM_DefaultLit` | **`bUsedWithInstancedStaticMeshes = true`**（全仓唯一） | 常数 `(0.46,0.42,0.37)` + Roughness `0.92` |
| `M_TinyGladeGround` | `BLEND_Opaque` | `MSM_DefaultLit` | — | 2026-09-12：`dirtpath_grass` / `dirtpath_2` / `dirtpath_1` 三图按道路 R 和 `dirtpath_heightmap` 高度竞争混合；Roughness `0.95`。采样倍率、反编译与参考图见 [Ground_Material.md](Ground_Material.md) |

**五个材质全部 `HasPerInstanceCustomData = False`**（AssetRegistry tag 实测）。
另有一个材质实例 `MI_TinyGladeWall`（父级即 `M_TinyGladeWall`），墙面全部贴图与手感参数的调节面在它上面。

材质的**可读源头是三个 Python 脚本**（材质图本身在 `.uasset` 里不可读）：
`Scripts/TinyGladeMakeWallMaterials.py`（Wall/Reveal/Roof）、
`Scripts/TinyGladeMakeGroundMaterial.py`（Ground）、
`Scripts/TinyGladeSetupFrame.py:29`（Brick，`used_with_instanced_static_meshes = True`）。

**另一套 8 个 `M_TG_*`**（`Content/HouseTest/TinyGladeAsset/Materials/`：`M_TG_Bark`/`Canopy`/`CanopyInner`/
`CanopyOpaque`/`Floor`/`LeafCards`/`Texture`/`VertexColor`）是**提取资产的看图材质**，
`Content/HouseTest/TinyGladeAsset/MaterialInstances/` 下 176 个 `MI_*` 全部以 `M_TG_Texture`
（`MSM_Unlit` / `BLEND_Opaque`）为父。**全仓没有任何 C++ 或 Python 引用它们**——
它们不是房子的候选母材质。其中 `M_TG_Canopy` / `M_TG_LeafCards` 是
`MSM_TwoSidedFoliage` + `BLEND_Masked`，将来接 D13 藤蔓时可以当模板。

**当前光照行为**（由上表直接推出）：
- 全部 `MSM_DefaultLit` ⇒ **Lambert 漫反射 + GGX 高光**（UE 默认），没有 Burley。
- ~~全部无法线贴图、无 AO 贴图、粗糙度是常数 ⇒ 变化全靠顶点色与两张地面贴图。~~
  ⚠️ **这一条被 2026-09-03 的墙面重建推翻了一半**：`M_TinyGladeWall` 现在有法线贴图
  （砖 `stone_floor_2_normal` + 剥落边的卷边扰动），粗糙度也不是常数（灰泥 0.85 / 砖 0.92
  按 `BrickMask` 插值）。**仍然成立的是**：AO 贴图一张都没有；其余四个材质照旧。
  「与 TG 同构」那半句也随之收窄 —— TG 的墙确实是常数粗糙度、无法线贴图，本项目在墙上
  **有意不同构**（TG 的砖是真几何、法线来自几何；本项目的墙板是实心盒，砖只存在于材质里，
  不给法线就完全读不出体积）。理由与两处不同构见 `TinyGladeMakeWallMaterials.py` 文件头。
- `M_TinyGladeWall` 是 Masked ⇒ 阴影通路会为它编译真 PS（`[引擎]`
  `ShadowDepthRendering.cpp` 的 `!bWritesEveryPixelShadowPass` 分支），**洞在阴影里是洞**——
  这条 `[计划]` D14 的表写得对。

#### 7.2 关卡里的光照：两盏灯，零后处理【确凿，`.umap` 字节实测】

| 关卡 | 光照 actor |
| --- | --- |
| `L_HouseGroundDemo.umap` | `DirectionalLight_2`、`TG_Sky`(SkyAtmosphere)、`TG_SkyLight`(SkyLight) |
| `L_TerrainOpsDemo.umap` | `DemoSun`(DirectionalLight)、`DemoAtmosphere`(SkyAtmosphere)、`DemoSky`(SkyLight) |

**两张关卡都没有 `PostProcessVolume`，没有 `ExponentialHeightFog`，没有 `VolumetricCloud`。**
出图脚本 `TinyGladeShotArches.py:57-63` 另外**临时**塞一盏 `intensity=6.0`、
`pitch=-38°/yaw=150°` 的太阳 + `real_time_capture` 的 SkyLight（因为它自己建的关卡没有灯）。

**这意味着今天所有截图跑的都是 UE 的出厂默认**：默认色调映射曲线、默认自动曝光、
默认 bloom、默认 Lumen 设置、零 grading。**§5 里排第 4 的那一整块（后处理）在本项目是空白。**

项目级设置（`Config/DefaultEngine.ini:49-69`）：
```ini
r.DynamicGlobalIlluminationMethod=1     ; Lumen
r.ReflectionMethod=1                    ; Lumen reflections
r.Shadow.Virtual.Enable=1               ; VSM
r.GenerateMeshDistanceFields=True
r.RayTracing=True
r.RayTracing.RayTracingProxies.ProjectEnabled=True   ; ← UE 自带的「光追代理」机制已开
r.AllowStaticLighting=False
r.Substrate=True
r.DefaultFeature.LocalExposure.{Highlight,Shadow}ContrastScale=0.8
```

#### 7.3 三条通道逐条现状

| 通道 | 现状 | 实证 |
| --- | --- | --- |
| **一：`CustomPrimitiveData`** | **完全没接。** `SetCustomPrimitiveData` / `GetCustomPrimitiveData` 全仓 **0 命中**；两处 `Set(...)` 的最后一个实参都是 `nullptr` | `CSGpuMeshSceneProxy.cpp:120-121`、`CSGpuInstancedMeshSceneProxy.cpp:661-662`（⚠️ `[二轮]` L273 记的 `:121` / `:655` 已过期） |
| **二：逐顶点 Color** | **已经在用，而且字典已经上线。** 房体 `R = 构件色号 / G = 洞 Tag / B = 洞形状 id（255 = 无洞）/ A = 0`；`M_TinyGladeWall` 正在消费 B | `CSHouseActor.cpp:27-31`（`CSHouse_Semantic`）、`:64-66`（`SetPanel` 写 B）、`:84`（`AddTri` 逐顶点写）；地面 `R = 道路权重`（`CSGroundActor.h:37`） |
| **二半：UV1** | **已经在用。** 房体 `NumTexCoordChannels = 2`，UV1 = 解析裁剪场 `q` | `CSHouseActor.cpp:83`、`:638`、`:659`（`EnsureTexCoordSets(Body, 2)`） |
| **三：per-instance custom data** | **零宽。** `InstanceCustomDataBuffer` 是占位、`NumCustomDataFloats = 0` | `CSGpuInstancedMeshVertexFactory.cpp:47-48` |

**⚠️ 两条 `[计划]` D14 的描述已经过期**（详见 §9）：
「房体的 32 bit 今天全空（四通道恒 `(1,1,1,1)`）」——**不成立**；
「扩一条 UV1 流的路在 proxy 绑定处有静默地雷 / 要改 5 个 C++ 处 + 7 个 `.usf`」——
**多组 UV 的设施已经建好并在跑**（`CSGpuMeshSceneProxy.cpp:300-312` 逐组挂 stream component、
正确设 `Data.NumTexCoords`；`CSGpuMeshTypes.cpp:44` 的 `ElementsPerUnit = 2 × clamp(NumTexCoordSets, 1, 4)`）。

#### 7.4 为什么一点影子都没有：根因落到引擎的一行【确凿，`[引擎]` 逐行核对】

组件构造里写死：
- `CSMeshRenderComponent.cpp:84` `CastShadow = false;`（房子/地面/道路）
- `CSGpuInstancedMeshComponent.cpp:65` `CastShadow = false;`（石阶/点刷/将来的 decor）

两处注释给的理由都是「GPU-Scene instance culling overrides the custom indirect args in the
Virtual Shadow Map pass」。**这个理由的精确机制是引擎的这一行**：

```cpp
// D:/UE-SourceCode-5.7.4/Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1304
const bool bDoOverrideArgs = SceneArgs.IndirectArgsBuffer != nullptr
                          && MeshDrawCommand.PrimitiveIdStreamIndex >= 0;
...
// :1334-1335 / :1351-1352
    bDoOverrideArgs ? SceneArgs.IndirectArgsBuffer : MeshDrawCommand.IndirectArgs.Buffer,
    bDoOverrideArgs ? SceneArgs.IndirectArgsByteOffset : MeshDrawCommand.IndirectArgs.Offset
```

三个事实把链条闭合：

1. `MeshPassProcessor.inl:128` —— `PrimitiveIdStreamIndex = VertexFactory->GetPrimitiveIdStreamIndex(...)`，
   顶点工厂类型**不声明 `SupportsPrimitiveIdStream` 时恒为 −1**。
2. `VirtualShadowMapArray.cpp:3727 / 4174` —— VSM pass **确实**设了
   `InstanceCullingDrawParams.DrawIndirectArgsBuffer = CullingResult.DrawIndirectArgsRDG`。
3. `LocalVertexFactory.cpp:584` —— 引擎的 `FLocalVertexFactory` **声明**了
   `EVertexFactoryFlags::SupportsPrimitiveIdStream`。

**由此得到本文最有操作价值的一条推论【推测，推理链如上，但三条前提全是确凿】**：

| 路径 | 用的 VF | `PrimitiveIdStreamIndex` | `bDoOverrideArgs` | 结论 |
| --- | --- | --- | --- | --- |
| `FCSGpuMeshSceneProxy`（房子/地面/道路） | **引擎的 `FLocalVertexFactory`**（`CSGpuMeshSceneProxy.h:175/184`，全仓无第二个 `IMPLEMENT_VERTEX_FACTORY_TYPE`） | ≥ 0 | **true** | 自定义 indirect args 在 VSM 里被覆盖 ⇒ **确实画 0 个三角形**。注释是对的 |
| `FCSGpuInstancedMeshSceneProxy`（石阶/点刷） | **`FCSGpuInstancedMeshVertexFactory`**，`IMPLEMENT_VERTEX_FACTORY_TYPE` 里**没有** `SupportsPrimitiveIdStream`（`CSGpuInstancedMeshVertexFactory.cpp:105-111`，`.h:19-20` 明写「declares itself WITHOUT SupportsPrimitiveIdStream」） | **−1** | **false** | **自定义 args 不会被覆盖 ⇒ 这条路本来就能投影**。它的 `CastShadow = false` 很可能是**照抄非实例化路的过度保守** |

⚠️ **这与 `[计划]` D14「实例化那条路不受 args 覆盖之累、打开 `CastShadow` 就有影子」的判断
一致**，但**与 `CSGpuInstancedMeshComponent.cpp:63-64` 的注释「same limitation as the other
GPU meshes」冲突**。计划书对、注释错的可能性更大（三条引擎前提都是确凿的），
但**必须实测**：把 `CastShadow` 设 true 跑一次 `TinyGladeShotStairs.py`，
看石阶有没有影子。这是**整个 D14 里代价最小、收益最直接的一次实验**。

另外两笔账（与 `[计划]` 一致，本文只补实证）：
- `bBatchCastShadow` 声明在 `CSGpuMeshSceneProxy.h:199` 默认 `true`，**全仓没有任何一处给它赋值**，
  三处读（`CSGpuMeshSceneProxy.cpp:70`、`CSMeshRenderComponent.cpp:63`、
  `CSGpuInstancedMeshSceneProxy.cpp:675`）零处写。组件级的 `CastShadow = false`
  在上游就掐断了 `bShadowRelevance`，它根本没机会起作用。
- `SubmitGpuBufferDraw` 里 `BatchElement.FirstIndex = 0` 写死
  （`CSGpuMeshSceneProxy.cpp:104`）——多 section 的 direct draw 确实要改这个 public static
  函数的签名，`[计划]` 这条成立。

#### 7.5 GI / 距离场 / 光追现状，以及「进不了 Lumen」被漏掉的一半

**现状【确凿】**：`CSGpuMeshSceneProxy.cpp:26` `bSupportsDistanceFieldRepresentation = false;`，
所有派生代理继承；全仓 `GetDynamicRayTracingInstances` / `bAffectDistanceFieldLighting` /
`bAffectDynamicIndirectLighting` **0 命中**。所以：

| 通路 | 房子在不在 | 依据 |
| --- | --- | --- |
| 网格距离场 / DFAO / DF 阴影 | ❌ | `bSupportsDistanceFieldRepresentation = false` |
| 硬件光追（BLAS/TLAS） | ❌ | 无 `GetDynamicRayTracingInstances` |
| Lumen mesh cards / surface cache | ❌ | 无卡、无 DF |
| **Lumen screen traces** | ✅ **在** | `[引擎]` `LumenScreenProbeTracing.cpp:14-20`：`GLumenScreenProbeGatherScreenTraces = 1`（默认开），描述原文 *"trace against the screen before falling back to other tracing methods"*；屏幕追踪读的是深度 + HZB + 上一帧 SceneColor，**不看 DF/RT 表示** |
| VSM 阴影 | ❌ | §7.4 |
| base pass / depth prepass | ✅ | 主 pass 有像素 |

**推论【推测，依据上表】**：所谓「房子进不了 Lumen」的实际症状**不是「完全没有弹射光」**，
而是：
- 房子**在屏幕内**时，它既遮挡屏幕探针的追踪、也把自己的颜色弹射给周围——**这部分今天就有**；
- 房子**移出屏幕、被遮挡、或探针要往远场追**时，Lumen 回退到 SDF / 卡片，
  而那里没有房子 ⇒ **弹射光会随镜头移动突变**，且房子在远处对天光的遮挡完全缺失。

这条对方案选择很关键：**如果只是想要「墙的背光面不是死灰」，今天的 Lumen 屏幕追踪
配上一个像样的后处理体积就能给到大半**；只有想要稳定的、不随镜头跳变的间接光时，
才必须建 GI 代理。**未在本工程实测，标为待确认**——验证方法见 §10 A2。

---

### 八、约束下的阴影与间接光方案

约束重述：① GPU 生成网格进不了 Lumen 的世界空间表示（用户裁决「无视它」）；
② 逐像素 clip 的洞不进距离场、不进硬件 RT、不进软件 Lumen；
③ 不推翻已记录的裁决。

#### 8.1 阴影：五条路

| 案 | 做法 | 性能代价 | 维护代价 | 与 GPU 网格管线的兼容性 |
| --- | --- | --- | --- | --- |
| **S-a** | **实例化路直接开 `CastShadow = true`** | 一条额外 VSM draw/LOD | ~1 行 + 一次冒烟测 | **零冲突**。剔除只跑主视锥 ⇒ 视锥外实例不投影，需要时再补一套「阴影用全量 args」 |
| **S-b** | 非实例化路加 `bDirectDraw`（`[计划]` 已选） | 无（直接 draw 与 indirect 同量） | **大**：要在房体路径上破例维护 CPU 侧 `FirstTriangle/TriangleCount` 镜像（`CSMeshRenderComponent.cpp:55-57` 明写 deliberately never read）；要改 `SubmitGpuBufferDraw` 的 public static 签名加 `FirstIndex`，半径波及所有 gpumesh 派生代理 | 兼容，但改动面最大 |
| **S-c** | 非实例化路换一个**不声明 `SupportsPrimitiveIdStream`** 的 `FLocalVertexFactory` 子类 | 一整套新 shader permutation 的编译时间与 DDC | 中：项目**已有这条路的先例**（`FCSGpuInstancedMeshVertexFactory` 就是这么干的，且在跑） | 兼容。但 `[计划]` 已**明确否决**这条 ⇒ **要先重新裁决** |
| **S-d** | **隐藏的光照代理**：低模真几何洞 → `UStaticMeshComponent`，`SetVisibility(false)` + `bCastHiddenShadow = true` | 一次网格构建（重建时），渲染期一条正常静态网格 draw | 中：多一条「低模同步于高模」的不变量 | **完全正交**，一行 gpumesh 代码都不改；**顺带同时解决 DF / RT / Lumen** |
| **S-e** | 烘一张地面光照贴图（TG `_terrain_lighting_texture` 那条） | 一次 CS | 高（要自己实现 PCSS） | 只覆盖地面，房子仍无影 —— **不推荐**，UE 有实时阴影，抄这条是倒退 |

#### 8.2 间接光：四条路

| 案 | 做法 | 代价 | 说明 |
| --- | --- | --- | --- |
| **G-a** | 现状：SkyLight（`real_time_capture`）+ Lumen 屏幕追踪 | 零 | 已经在跑。缺的是**后处理体积把它调出来**，以及影子把它衬托出来 |
| **G-b** | 加 `PostProcessVolume`：Lumen 参数 + `IndirectLightingColor/Intensity` + AO 下限 + 曝光 + 色调曲线 | **零代码**，纯资产 | `IndirectLightingColor` 就是 TG `indirect_light_tint` 的直接对位物 |
| **G-c** | **光照代理**（同 S-d 那份隐藏组件）打开 `bAffectDistanceFieldLighting` / `bVisibleInRayTracing` / `bAffectDynamicIndirectLighting` | 一次网格构建 | **唯一能给到「稳定、不随镜头跳变」间接光的方案**；且是 **TG 自己的做法**（`[分析]` §9.3：prefab 的 `rt` 段是粗代理 `mesh:Aabb`，另建 BVH） |
| **G-d** | 自己实现 `GetDynamicRayTracingInstances`，直接拿常驻流建 BLAS | 大：要维护 BLAS 生命周期、要处理每帧几何变动 | 与 clip 的洞正面冲突（BLAS 是几何，clip 不生效）⇒ **除非先把洞做成真几何，否则不能做** |

#### 8.3 推荐路线

**推荐：S-a + G-b 先做（今天就能做，零风险），S-d/G-c 作为主线（同一份代理同时解决阴影与 GI）。
S-b/S-c 押后到 S-d 被证明不够为止。**

理由：

1. **S-a + G-b 合起来是「零代码 + 一行」的组合，却能拿到 §5 表里排 1、2、4 三条的大部分。**
   今天连后处理体积都没有，讨论 Burley 与 L1 探针是本末倒置。
2. **S-d/G-c 是同一份东西**：一份「低模 + 真几何洞」的代理网格，挂在一个不可见的
   `UStaticMeshComponent` 上，同时喂给 VSM、距离场、硬件光追、Lumen 卡片。
   一次投入解决四个通路，**而且是 TG 自己的做法**（§7.5 表 + `[分析]` §9.3）。
3. **代理的生产几乎是免费的**：房体本来就是 CPU 构建的（`FCSHouseMeshWriter`），
   同一个构建器降一档细分、把洞做成真几何（洞的形状只有 Rect/Circle/Arch 三种原型，
   剖面已经写在 `CSHouseProfile.h`），产出一份 200~500 三角的代理——
   **这正是 `[计划]` D8「参数化生成真几何洞」路线本来要做的事，只不过先做在代理上、
   不动渲染网格**，因此**不需要推翻任何裁决**。
4. **代理天然满足 D14 的硬约束**：「光照代理表示必须保留真几何洞口；逐像素 clip 只允许存在于
   raster 表示」——代理是几何洞，raster 是 clip，两边各就各位。
   而**今天的组合正是这条约束禁止的那个**（见 §9 第 1 条）。
5. **S-b 的账太贵**：它要求在一条明写「deliberately never read」的路径上破例维护 CPU 镜像，
   并改一个 public static 函数的签名。S-d 一行 gpumesh 代码都不改。
6. **S-c 是最省事的技术方案，但被裁决挡着**。本文只报告：`[计划]` 否决它的两条理由
   （「强依赖一个默认为 1 且被 Epic 明写 deprecated 的 CVar」「新增一整套 shader permutation」）
   里，**第二条的成本已经付过了**（instanced VF 就是这么注册的，permutation 已经在编译），
   第一条我在 `[引擎]` 里**没有找到**对应的 CVar——`LocalVertexFactory.cpp:311` 只按
   `VertexFactoryType->SupportsPrimitiveIdStream() && UseGPUScene(...)` 决定
   `VF_SUPPORTS_PRIMITIVE_SCENE_DATA` 这个 define，没有第三个开关。
   **这条待用户复核**，见 §9。

**已知代价（必须一起接受）**：
- `SaveToStaticMesh` 是 `#if WITH_EDITOR`（`CSMeshRenderComponent.h:90` / `.cpp:116`）⇒
  代理的**落盘**路线是编辑器限定。运行期要走 `UStaticMesh::BuildFromMeshDescriptions`
  或 `UDynamicMeshComponent`（后者**不产距离场**，只有阴影与光追）。本项目是编辑器工具，
  这条今天不构成阻塞，但要写进注释。
- 多一条不变量：「代理与渲染网格来自同一份 desc」。建议把代理构建挂在
  `RebuildBodyMesh` 的同一个 `BodyDescHash` 守卫下——**代理是几何，进哈希是对的**，
  与 D14「纯外观量绝不进 desc 哈希」的纪律不冲突。

---

### 九、与计划书 / 状态文件冲突之处（待拍板）

**不擅自改裁决。** 以下五条按严重度排序。

#### C-R1：D14 的 GI 代理硬约束**今天已经不满足**（最严重）

`[计划]` D14 写：

> **本计划已选的"参数化生成真几何洞"路线自动满足这条约束，代价为零** …
> 三段判定：**今天完全安全**（gpumesh 三条都不进，这个分界线碰不到）…
> **只在引入 GI 代理那一刻变错**

**但今天上线的墙不是真几何洞。** `CSHouseActor.cpp:44-45` 注释原文：

> 当前面板所属墙面的框架 + 洞的裁剪场。AddTri 据此为每个顶点算 UV1 = q ——
> **洞由材质逐像素 discard 切出来，几何上不挖**（Tiny Glade 原版做法，见 CSHouseProfile.h）。

`AddPanel`（`:563-574`）铺的是从 `Z0` 到墙顶的一整个 `AddBox`，洞位置一个三角形都没删。
`M_TinyGladeWall` 是 `BLEND_Masked` + `TinyGladeArchClip` 的 OpacityMask
（`TinyGladeMakeWallMaterials.py:45/65`）。**本文卷零**的模块表里
D6 那行写的也是「**逐像素 clip**（TG 原版做法）+ 门框砖」——**状态文件与代码一致，
只有 `[计划]` D14 那一段与它们不一致。**

后果：
- 「今天完全安全」**成立**（三条通路都没进）；
- 「本计划已选的路线自动满足约束，代价为零」**不成立**——那条路线还没上线；
- 「只在引入 GI 代理那一刻变错」**成立且迫在眉睫**：任何从常驻流直接建的代理都是**实心墙**。

三个选项：
① 订正 D14 的措辞（「已选路线」→「计划中的路线，D6 尚未采用」），并把「代理必须另建真洞几何」
   写成显式代价而不是「代价为零」；
② 把 D6 改成真几何洞（推翻 `CSHouseActor.cpp:44-45` 的现状与那条用户裁决）；
③ **接受 §8.3 的推荐**：渲染保持 clip，代理另建真洞——约束原文本来就是这么写的
   （「逐像素 clip 只允许存在于 raster 表示」），只是 D14 误以为两者会自动一致。

**未拍板前不要动 D6，也不要建 GI 代理。**

#### C-R2：D14「纪律」那一半其实没做完

状态文件记 D14「只完成『纪律』那一半」。实测：**那一半也漏了两个属性。**

`ACSHouseActor::BindHouseMaterials`（`CSHouseActor.cpp:1111-1121`）只重绑
`WallMaterial` / `RoofMaterial` / `PillarMaterial` 三个。但：
- `RevealMaterial` 由 `TinyGladeMakeWallMaterials.py:101/111` 写入，
- `FrameMaterial` 声明在 `CSHouseActor.h:229`，

**两个都不在重绑列表里** ⇒ 在细节面板里改它们，仍然是 D14 开篇描述的那个
「静默无效、必须手点 `RebuildHouse()`」的老毛病。

另有一处同形的漏洞：`UCSMesh::Materials` 是 public `UPROPERTY`
（`CSMesh.h:396`），**直接给元素赋值不触发任何广播**（`CSMeshRenderComponent.cpp:269-272`
已经把这个洞写在注释里了）。

⚠️ 这不是新裁决，是把已裁决的东西补完 ⇒ **可以直接做**（见 A6）。

#### C-R3：D14 通道二的字典建议与**已上线的字典冲突**

`[计划]` D14 建议钉死：

> `R = 色号 / G = SourceId 低 8 位（悬停高亮用）/ B、A 保留`

**已上线的字典**（`CSHouseActor.cpp:27-31`、`:64-66`）：

```
R = 构件色号 (ECSHousePart)     G = 洞 Tag     B = 洞形状 id（255 = 无洞）     A = 0
```

**B 已经被占，而且正在被 `M_TinyGladeWall` 消费**（`vcol` "B" → Custom 节点 `Shape`）。
计划书写的「B、A 保留」不成立。而且 `[计划]` 同段还写「房体的 32 bit 今天**全空**
（四通道恒 `(1,1,1,1)`）」——这条也不成立，四个通道里三个有活数据。

⚠️ **同一处过期的还有 `[二轮]` L440**（「房体 Color 是全 1 死值 / 32 bit 全空」）。
凡是建立在「房体顶点色没人用」之上的结论都要重查。

选项：① 把 D14 的建议字典订正成现状 + 把「SourceId 低 8 位」挪到 A；
② 重排字典（要同步改材质脚本 + `CSHouse_ClipKeeps` 的两处翻译）。

#### C-R4：D14 关于 UV1 的告警已过期

`[计划]` D14 写：

> 扩一条 UV1 流的路**在 proxy 绑定处有静默地雷**：`CSGpuMeshSceneProxy.cpp:303` 的
> `Data.TextureCoordinatesSRV = S.SRV` 在流循环里，后一条 TexCoord 流会直接覆盖前一条；
> TexCoord 描述符写死 `ElementsPerUnit = 2` … 真要 UV1 得改 5 个 C++ 处 + 7 个 `.usf` 的 stride

**已经建好并在跑**：`CSGpuMeshTypes.cpp:44` 是
`D.ElementsPerUnit = 2 * FMath::Clamp(Options.NumTexCoordSets, 1u, 4u)`（不是写死 2）；
`CSGpuMeshSceneProxy.cpp:300-312` 按组数逐个挂 stream component（偏移 8×组号）、
只设一次 SRV、正确抬 `Data.NumTexCoords`；房体 `EnsureTexCoordSets(Body, 2)`
（`CSHouseActor.cpp:659`）已经在用第二组。
**本文卷零**的 C1 丙案说这是「上一轮刚建好并测过的设施」——
**状态文件是对的，`[计划]` D14 这一段是它建好之前写的。** 建议订正措辞。

#### C-R5：`[计划]` 否决「非 GPU-Scene 顶点工厂」的第一条理由，我没有在引擎里找到对应物

`[计划]` D14 阴影一节写：

> **明确不做**"自定义非 GPU-Scene 顶点工厂"那条：它强依赖一个默认为 1 且被 Epic 明写
> deprecated 的 CVar，且新增一整套 shader permutation。

第二条理由成立但成本已付（`FCSGpuInstancedMeshVertexFactory` 就是这么注册的，
permutation 已经在编译）。第一条理由我在 `[引擎]` 里**找不到对应的 CVar**：
`LocalVertexFactory.cpp:311` 只按 `VertexFactoryType->SupportsPrimitiveIdStream() &&
UseGPUScene(Platform, ...)` 决定 `VF_SUPPORTS_PRIMITIVE_SCENE_DATA`，没有第三个开关；
`ValidateCompiledResult`（`:326-336`）只是在**声明了**该 flag 时禁止绑 Primitive UB，
反向没有约束。

**我可能没找到 `[计划]` 作者当时指的那个 CVar**（比如 UE 更早版本的
`r.SupportPrimitiveUniformBuffer`），所以**这条标为【待确认】，不构成推翻建议**。
只是：既然 S-c 是技术上最省事的一条，值得让作者点名那个 CVar 再复核一次。

#### 另外两条纯订正（不涉及裁决）

- `[二轮]` L273 记的两处 `nullptr` 行号 `CSGpuMeshSceneProxy.cpp:121` /
  `CSGpuInstancedMeshSceneProxy.cpp:655` 已过期，现值 `:120-121` / `:661-662`。
- `CSGpuInstancedMeshComponent.cpp:63-64` 的注释「same limitation as the other GPU meshes」
  与 `[计划]` D14 的判断相反，且引擎源码支持 `[计划]`（§7.4）。**注释多半是错的，
  但要实测才能定。**

---

### 十、可行动清单（排序）

排序依据：**先做不触碰任何既有裁决的**，再做只触碰一条的，最后是需要先推翻裁决的。

| 序 | 事项 | 预估改动范围 | 触碰的既有裁决 |
| --- | --- | --- | --- |
| **A1** | **实例化路开 `CastShadow` 冒烟测**。`CSGpuInstancedMeshComponent.cpp:65` 改 `true`，跑 `TinyGladeShotStairs.py` 看石阶有没有影子。引擎侧三条前提（`MeshPassProcessor.cpp:1304`、`MeshPassProcessor.inl:128`、该 VF 不声明 `SupportsPrimitiveIdStream`）都指向「本来就该有影子」 | **1 行 + 一次出图**。若成立，顺手把 `.cpp:63-64` 的注释改对 | 无 —— `[计划]` D14 本来就是这么判断的，本条只是去执行 |
| **A2** | **给两张演示关卡加 `PostProcessVolume`（Unbound）+ `ExponentialHeightFog`**，并把太阳/天光的参数定下来。顺带在同一张图上验证 §7.5 的推论（把房子推出屏幕，看地面上的弹射光变不变）。**这是「看起来像不像 TG」性价比最高的一步，而且零代码** | 纯资产 + `TinyGladeShotArches.py` 里那段临时补光挪成关卡里的常驻配置 | 无 |
| **A3** | **PPV 里按 §3/§4 的实证值调参**：`AmbientOcclusionIntensity` 使最暗只到 0.6（对位 §3.3）；`IndirectLightingColor/Intensity`（对位 `indirect_light_tint` + ×1.2）；`r.Shadow.Virtual.SMRT.*` 提软阴影质量（对位 §3.2 的 PCSS）；`BloomIntensity` 压到 ~3% 且半径拉大（对位 §4②）；`Film`/`ToneCurveAmount`/`ExpandGamut` 往 Tony McMapface 的方向调（对位 §4⑥）；`DepthOfField` 开起来 | 纯参数 + 一份「每个值对位 TG 哪一行 GLSL」的注释表 | 无 |
| **A4** | **通道一接上**：两处 `Set(..., nullptr)` → `GetCustomPrimitiveData()`（`CSGpuMeshSceneProxy.cpp:121`、`CSGpuInstancedMeshSceneProxy.cpp:662`），加 `ACSTinyGlade::PushPrimitiveParams()`，布局照 `[计划]:1129` 钉死。**首个消费者建议是 `[0..4]`（房子原点 + yaw）——它解的是「物体空间在材质里根本无法表达」这个今天无解的问题**（`CSMeshRenderComponent.cpp:76-80`） | ~60 行 + 材质侧一个 `CustomPrimitiveData` 节点 | 无（`[计划]` 已规划） |
| **A5** | **通道三接上**：`CSGpuInstancedMeshVertexFactory.cpp:47-48` 两行 + aux slot + SRV + `CSGpuInstancedMesh.usf` 的按可见槽拷贝，共 5 处约 40 行。首个消费者 = D9 石阶 | ~40 行 | 无（`[计划]` 已规划） |
| **A6** | **补完 D14 的「纪律」那一半**：`BindHouseMaterials` 加 `RevealMaterial` / `FrameMaterial`（见 C-R2） | ~4 行 | 无 —— 是把已裁决的事补完 |
| **A7** | **订正 `[计划]` D14 的四处过期措辞**（C-R1① / C-R3 / C-R4 / `[二轮]` L273、L440 的行号与结论） | 文档 | 无，但**动了状态文件与计划书，需驱动方点头** |
| **A8** | **光照代理（S-d + G-c）**：低模真洞代理 → 隐藏 `UStaticMeshComponent`（`SetVisibility(false)` + `bCastHiddenShadow` + `bAffectDistanceFieldLighting` + `bVisibleInRayTracing` + `bAffectDynamicIndirectLighting`）；gpumesh 侧保持 `CastShadow = false` 避免双重投影；代理构建挂同一个 `BodyDescHash` | ~250 行（复用 `FCSHouseMeshWriter` + `CSHouseProfile.h` 的三种剖面）+ 一条「代理同步于本体」的不变量 + 单测 | **必须先过 C-R1 的拍板** |
| **A9** | **Burley 漫反射 / 掠射增益的材质侧近似**（对位 §3.1、§2.4b 的 `rim` 项）。`r.Substrate=True` 已开，可走 Substrate 的 rough diffuse；或在 Custom 节点里手写 | 材质图，~30 节点 | 无。但**排在 A1-A3 之后**：没有影子时它看不出来 |
| **A10** | **非实例化路的影子**（S-b 或 S-c）。只有在 A1 证明实例化路可行、且 A8 的代理仍不够时才做 | S-b：改 public static 签名 + CPU 三角计数镜像，半径大；S-c：一个 VF 子类 | S-b 触碰「`FirstTriangle/TriangleCount` deliberately never read」；S-c 触碰 `[计划]` D14 的明确否决（且见 C-R5） |

**A1 + A2 + A3 合起来是「一行代码 + 两份资产」，却覆盖了 §5 归因表里排 1、2、4 三条的大部分。
建议作为 D14 的第一步，且它们之间没有相互依赖。**
A8 起每一项都要先过一次裁决，不应捆进第一步。

---

### 十一、待确认

| # | 待确认的事 | 需要什么证据 |
| --- | --- | --- |
| 1 | 实例化路开 `CastShadow` 到底有没有影子（§7.4 表的第二行） | 跑 A1，一张出图 |
| 2 | Lumen 屏幕追踪对 gpumesh 是否真的生效（§7.5） | A2 顺带：把房子移出屏幕，看它在地面上的彩色弹射是否消失；或 `r.Lumen.ScreenProbeGather.ScreenTraces 0/1` 对比两张图 |
| 3 | `view_constants[1]` 是什么视图（`[0]` 主相机、`[2..4]` 三级联已确认） | 找到写 `view_constants` 的 CPU 侧符号；`[PDB]` 搜 `camera_idx` 的写入方 |
| 4 | TG 阴影图的分辨率（从 `0.000833333 = 1/1200` 的纹素偏移**推测**为 2400²/层） | `[PDB]` 里阴影图的创建点 |
| 5 | `sky_low/high_color` 的插值是否在 CPU（本文标为【推测】） | `[PDB]` 搜 `sky_low_day` / `time_of_day` 的读方 |
| 6 | `[计划]` D14 说的那个「默认为 1 且被 Epic 明写 deprecated 的 CVar」是哪一个（C-R5） | 由 `[计划]` 作者点名；或在 `[引擎]` 里全文搜 `PrimitiveUniformBuffer` 相关 CVar |
| 7 | `UDynamicMeshComponent` 能否作为距离场代理（本文按「不能，只有阴影与光追」处理） | `[引擎]` `FBaseDynamicMeshSceneProxy` 的 `bSupportsDistanceFieldRepresentation` |

---

### 附：本文没有覆盖的

- **TG 的水/冰渲染**（`_water_*` 12 个 shader，含独立的光追反射/折射）——本项目无水面需求。
- **`_tgsr` 超分的算法**（4 个 permutation，与 FSR reactive mask 混用）——UE 有 TSR，不需要抄。
- **`_painterly_*` / `_vermeer_*` 笔触渲染**——照片模式滤镜，与常规观感无关。
- **风格化 VS 变形**（`[分析]` §1.6 的六步）——那是几何模块的事，`[计划]` D14 末节
  「世界空间 value-noise」已经给了正确的限定（TG 是逐砖刚体平移，连续网格拿不到砖级不连续感）。

---

<a id="vol-5"></a>

## 卷五 · 砖构件对照：门框 / 转角 / 垛口 / 接缝 / 上沿

> 2026-08-31 新增。起因是一个直觉判断：「门框砖、房屋转角砖、房顶砖看上去都是 SplineMesh 排列了各种不同的子砖」。
> 本卷逐条核这个判断 —— 结论是**排布抽象成立、"SplineMesh" 只在一处成立、"各种不同的子砖"不成立**。

### 证据标注约定（本卷）

| 标注 | 含义 |
| --- | --- |
| 【确凿】 | PDB 符号 / 源文件树 / 反编译 GLSL / 资产实测直接给出，不含推理 |
| 【推测】 | 由确凿证据推理得出，推理链在正文写明 |
| 【待确认】 | 证据不足以判定，正文写明「需要什么证据才能确证」 |
| `[PDB]` | `D:/MyProject/Tiny Glade/tmp/pdb_symbols.txt` |
| `[PATH]` | `D:/MyProject/Tiny Glade/tmp/pdb_paths.txt` —— **本卷的主力证据**，源文件树能证「某个东西不存在」 |
| `[GLSL]` | `D:/MyProject/Tiny Glade/tmp/shaders/` 反编译产物，逐行 |
| `[资产]` | `D:/MyProject/Tiny Glade/assets/`（`meshes/` 实测 + `nani_meshes.ron` 逐条） |
| `[分析]` | `MESH_GENERATION_ANALYSIS.md` |
| `[计划]` | [`Plan_House.md`](Plan_House.md) |

⚠️ 一处易误采：`inter_roof_merlons` 是**两个屋顶互相压上**时长出来的雉堞，**不是**单个平屋顶边缘那圈垛口。两者都叫 merlon，机制完全不同。

### 结论速览

| 构件 | 排布归属 | 记录类型 | 实例流 | 绘制管线 | 网格 |
| --- | --- | --- | --- | --- | --- |
| 墙身砖 / 门樘 / 拱圈石 | `system_wall_constructor`（那面墙） | `MyBrick` + `flags` | `InstancedWallData` 96 B | 墙砖**专用**（两阶段遮挡剔除 + 7 槽） | `brick` |
| 转角砖 / 墙裙 / 承重柱 | 同上，**同一个 `Vec<MyBrick>`** | `MyBrick` | 同上 | 同上 | `brick` |
| 平屋顶边缘垛口 | 墙的顶层砖【推测，见 §4.3】 | `MyBrick` | 同上 | 同上 | `brick` |
| 接缝砖 / 屋顶间雉堞 | `system_clutter`（不属于任何墙） | `StitchBrick` | `StitchBrickAlloc` | 【待确认】 | `brick` |
| 门/窗拱缘「帽子」件 | `system_decorator` | `GothicWindowBricksInstanceData` | nani | nani raster | 9 个预制件 |
| 平屋顶顶面石板 | `system_roof::visual` | `StoneFloorInstanceData` | nani | `_nani_roof_floor` | **运行时生成** |
| 屋顶瓦片 | `system_roof::visual` | `RoofShingle` | `RoofTileSsbo` | nani external | `roof_tile`×3 |

**三句话**：

1. **前五行是同一个抽象** —— 沿一条路径累积刚体块，全部是同一个 `brick` × 逐实例非均匀缩放。
2. **"各种不同的子砖"不成立** —— 多样性来自逐实例参数，不是 palette（§1）。
3. **"SplineMesh" 只有拱缘帽子件成立** —— 那里是真的把一条烘好的多砖网格沿骨点弯过去（§2.2）。

---

### 一、资产层的地基：全库只有一个 `brick`

两条硬约束，本卷后面所有结论都挂在它们上面：

- **`assets/meshes` 实有 138 个网格，砖只有一个**：`brick.glb` / `brick.json`（600 顶点 / 300 三角 / ±0.5 单位盒 /
  属性 `Vertex_Position`、`Vertex_Normal`、`is_bevel`，384 个倒角顶点）【确凿，`[资产]` 实测】。
  另有 `garden_stone` / `water_stones`，是花园与水边石，不是砖。
- **`InstancedWallData` 96 字节里没有 mesh 索引字段**【确凿，`[GLSL]` `_wall_generate_draw_lists...cs_pass1` L66-80】：

  ```glsl
  struct InstancedWallData {
      Affine3Packed transform;      // float[12]，3x4 仿射 —— 非均匀缩放**就是**砖尺寸
      vec4 sin_deform_y;            // 砖四角 Y 位移（CPU BrickRowSine 烘焙，VS 双线性插值）
      vec3 global_arch_height_vals; // 拱曲线在砖左/中/右的世界高度
      uint seed;                    // 低 2 位 = 90° 旋转档；参与 UV/颜色哈希
      uint source_id;               // 通常 = WallId
      vec2 wallspace_x_range;       // 砖两端沿墙的参数坐标区间
      uint flags;                   // 位表见下
  };                                // ← 没有 mesh_id / part_index：想摆"不同的子砖"在数据结构上就不可能
  ```

⇒ **墙这条路上不存在「多种子砖」。** 差异全部来自逐实例参数。

#### 1.1 多样性的四个来源【确凿，`[GLSL]` `_wall_wall_brick_lod0...vs_main`】

| 来源 | 效果 |
| --- | --- |
| 逐实例非均匀缩放 | 砖尺寸 = transform 的列长。同一个单位盒能当薄砖、能当角石、能当柱段 |
| `seed & 3` × 1.57 rad 绕 Y | 4 种外观白嫖，零成本 |
| 世界空间 value-noise | 世界坐标 ×0.4（2.5 m 晶格）8 角哈希三线性插值 → XZ ±3 cm。**相邻砖共享同一个场 ⇒ 墙面「歪而不散」** |
| `flags` 若干位 | 改行为，见下表 |

#### 1.2 `flags` 位表【`[GLSL]` 实证，语义列为推测；`[分析]` §1.7】

| 位 | GLSL 实证效果 | 语义推测 |
| --- | --- | --- |
| 1 | 三轴一律 ×1.045（均匀胀） | 特殊砖 |
| 2 | 查 `roof_color_ids[]` 而不是 `wall_color_ids[]` | 随屋顶配色的砖（雉堞 / 烟囱） |
| 4 | 启用随机胀大 | 普通墙砖 |
| 8 | PS **反向**裁剪（保留拱线以下） | 拱圈石 |
| 16 | 减雪 | 檐下砖 |
| 32 | 局部 z 按拱高压扁 + 三平面 UV + 免拱裁剪 | 拱内楣 / 门框构件 |
| 64 | 免编辑高亮 | 非编辑对象 |
| 128 | 强制 LOD0 | 形变敏感大件 |

胀大那一支（`flags & 4` 门控、×1.045 / 水平 ×1.1 竖直 ×1.0、10% 不胀）的口径以驱动方复读 GLSL（`[GLSL]` L160-175）为准，逐条见卷零「最值得直接抄的一条：TG 的砖是故意胀大、互相穿插的」，与 `[分析]` §1.6 的表述略有出入。

**竖直不胀是有意的**：砖一层层码上去，竖直也胀会让层高漂移、层间可见重叠。本项目的 `FrameBrickBloat = 1.10`
就是抄的这一支（见**本文卷零**「TG 的砖是故意胀大、互相穿插的」）。

---

### 二、"沿线摆块"成立，但它不是 SplineMesh

#### 2.1 计划书里已经有这句话

`[计划]` D11（`ACSSplineBlockActor`）的实拍参考图注原文：

> 参考实拍（红框：墙顶城齿、转角角石、勒脚石排 —— **同一种「离散块沿线累积」**）
> 类似 SplineMesh 但**不是弯曲变形** —— 块是刚体，沿样条切向逐个排列。

配图 [`img/tiny-glade-ref-crenellation-trim.jpg`](img/tiny-glade-ref-crenellation-trim.jpg)。
**这条抽象是对的**，而且 `SolveBlockLayout` 已经是它在本项目侧的落地。

#### 2.2 唯一真正的 SplineMesh：门/窗拱缘的「帽子」件【确凿】

`GothicWindowBricks` subset（`[资产]` `nani_meshes.ron`，9 个网格）：

```text
decorators/arrow_slit_{1x2,1x3}_hat
decorators/balcony_door_gothic_rank{1,2,3}_hat
decorators/window_gothic_{2x1,3x1}_hat
decorators/window_gothic_{2x1,3x1}_hat_full
```

`[GLSL]` `_nani_gothic_window_bricks.raster...vs_main`：

```glsl
struct InstanceData {
    Affine3Packed xform;
    int seed; int _wall_id;
    uint wallspace_x_range_packed; uint _is_wooden;
    vec3 bone_0_pos; uint bone_0_normal_packed;   // ← 三个骨点
    vec3 bone_1_pos; uint bone_1_normal_packed;
    vec3 bone_2_pos; uint bone_2_normal_packed;
};
// 顶点输入（L103-108）：Position / Normal / UV / is_bevel / brick_id / bbx_x
// bbx_x = 该顶点沿件长度轴的归一化位置 0..1；brick_id = 该顶点属于第几块砖

if (bbx_x < 0.5)  pos = mix(bone_0, bone_1,  bbx_x * 2.0);          // L125-129
else              pos = mix(bone_1, bone_2, (bbx_x - 0.5) * 2.0);   // L136-138
seed_for_brick = seed + brick_id;                                    // L319 逐砖随机数
```

⇒ **一整条拱缘是一个烘好的多砖网格，被沿 3 控制点、2 段的折线弯过去；每块砖靠顶点属性 `brick_id` 保留自己的身份。**
这就是 SplineMesh。

**一个有意思的例外**：`window_gothic_1x1_hat` 与 `1x1_hat_full` 反而在**不弯**的 `SolidVertexColorBricks` subset 里，
只有 2x1 / 3x1 才进弯曲那条路。【推测】跨度太小，弯了看不出来，省三组骨点上传。

对照组：`_wall_wall_brick_lod0...vs_main` 的顶点输入**只有** `Vertex_Position` / `Vertex_Normal` / `is_bevel`，
`bone_` / `bbx_x` / `brick_id` **零命中**【确凿】。

#### 2.3 `SolidVertexColorBricks` 是预制装饰件，不是砖【确凿】

12 个网格，全是 `decorators/arrow_slit_*` 与 `decorators/window_gothic_*`（窗体 / 箭孔 / 老虎窗 / 1x1 帽子）。
ron 里显式写着 `remove: ["bbx_x", "brick_id", "is_bevel"]` —— **源网格带这三个属性但这条路不用**，
即：不弯、不逐砖抖动。它们是「按尺寸档位（1x1 / 2x1 / 3x1）分的整件窗套」，不是砖的调色板。

#### 2.4 墙砖为什么"看起来"也像 SplineMesh

VS 里三个小量形变把刚体盒串成了连续带【确凿，`[GLSL]`】：

- `sin_deform_y`：砖四角 Y 位移（CPU `BrickRowSine` 烘焙，VS 双线性插值）⇒ 整排砖连续波浪；
- `flags & 32`：局部 z 按拱高压扁 ⇒ 砖贴着拱曲线走；
- 共享的世界空间噪声 ⇒ 相邻砖一起歪。

**视觉上是一条弯带，数据上是一堆刚性单位盒。**

---

### 三、逐族拆解

#### 3.1 墙身 / 门樘 / 拱圈石：同一个 `Vec<MyBrick>`

门框砖不是「另一族构件」，是同一批砖上打了位：`flags & 8`（拱圈石，PS 反向裁剪）与
`flags & 32`（拱内楣 / 门框，沿拱压扁 + 三平面 UV）。

**拱洞的形状由 GPU 逐像素切出**【确凿，`[分析]` §1.6】：CPU 只让跨拱砖携带 3 点拱高
（`global_arch_height_vals`），VS 分段插值成该像素列的拱高传给 PS，gbuffer PS `if (world_y < 拱高) discard;`。
depth-only / shadow PS 变体只保留同样的裁剪 ⇒ **阴影里也有拱洞**。

#### 3.2 拱圈石有独立的「轮廓 + 行走」设施【确凿，`[PDB]`】

```text
construct_arches::profile::create_stone_arch_profile(_impl)   # 出石拱剖面
construct_arches::profile::ArchFunction::remap_t              # 沿拱重映射参数 t
arch_walker::ArchWalker::{new, walk}                          # 沿剖面走位摆砖
```

**这是全 crate 唯一一处真的用「轮廓函数 + 沿线行走」摆砖的地方**，也正是那条直觉最贴的落点。
本项目 `CSHouseFrame` 的「一线程一砖 + 从拱参数解析推导位置与朝向」是同型的另一种实现（GPU 侧完成）。

#### 3.3 转角 / 墙裙 / 承重柱：同一条路，只是追加时机不同【确凿】

- **墙自身转角**：`utils::wall_corners::add_wall_corners` —— 在切层、切砖、裁洞**之后**往 `Vec<MyBrick>` 追加。
- **墙裙**：`utils::wall_skirts::add_skirts` —— 墙底与地面之间那一圈。
- **承重柱**：`construct_elevation_supports/`，**12 个源文件**（`[PATH]`）：

  ```text
  circle_{bottom_layer, categorize, pillars, turret}.rs
  rectangle_{bottom_layer, pillars, large_brackets, small_brackets}.rs
  find_overlap_segments.rs · maintain_stone_column_colliders.rs
  util_pillar_construction.rs · util_cross_brick_pillar.rs
  ```

  圆形与矩形各一套、外加托架与十字砖柱两种形态 —— 但产物仍是 `brick`。

#### 3.4 接缝砖 / 屋顶间雉堞：共用一个分配器【确凿】

```text
[PDB] 行 8854  detect_intra_shape_corners : ... ResMut<WallHoles> · ResMut<IntraShapeCorners> ...
[PDB] 行 9091  stitch_bricks              : Res<IntraShapeCorners> → ResMut<InterShapeBrickStitches>
[PDB] 行 8966  spawn_stitch_bricks        : Res<InterShapeBrickStitches> · Res<AssetSsboLibrary> → ResMut<StitchBrickAlloc>
[PDB] 行 8879  construct_inter_roof_merlons: Res<InterRoofMerlons> · ResMut<InterRoofMerlonBrickCount> · ResMut<StitchBrickAlloc>
```

屋顶那套与形状接缝那套**共用同一个 `StitchBrickAlloc`**；雉堞砖走 `flags` bit2（取 `roof_color_ids`）。
记录类型是 `StitchBrick`，不是 `MyBrick` —— 它不属于任何一面墙。

【待确认】`StitchBrick` 最终走的是墙砖专用管线还是 nani。`SolidVertexColorBricks` subset 装的是预制装饰件
（见 §2.3），所以缝砖多半不在那里，但没有决定性证据。

#### 3.5 屋顶瓦片：唯一一个不属于这个抽象的【确凿】

`system_roof::visual::assemble_roof_tiles` → `SparseInstanceBuffer<RoofTileSsbo>`，实例是 `RoofShingle`
（`base_position` / 逐瓦 `clipping_plane_` / 四元数 / `animation_t` / `roof_id` / `velocity_rotation`）。
**不沿曲线摆，是在屋面这个二维参数面上铺。** 独有：铺瓦生长动画、檐口起翘、逐瓦裁剪平面 discard、
5×5 瓦图集、`_roof_tiles_gravity.cs`（墙升降时的瓦片重力与翻转角）、depth-only PS 里的 `DormerHole` 逐像素开天窗。

⇒ **想做屋顶瓦，别指望复用门框砖那条路。TG 自己就没复用。**

---

### 四、"上沿石头"不存在

#### 4.1 源文件树证明了它不存在【确凿，`[PATH]`】

`wall-constructor` crate 共 **53 个 `.rs`**，`utils/` 下只有 8 个：

```text
src/utils/{half_timber, resolve_hole_overlap, sine_wave, split_into_random_rows,
           stairs_signifiers, trim_rows, wall_corners, wall_skirts}.rs
```

**没有 coping / capping / parapet / crenellation / merlon / top_row 任何一个文件。**
加上 §1 的两条（全库一个砖网格、实例记录无 mesh 索引）⇒ **上沿在数据上不可能是另一种石头。**

#### 4.2 那它为什么看起来不一样：三条叠加

| 现象 | 机制 |
| --- | --- |
| 轮廓最显眼 | `flags & 4` 的胀大是**水平 ×1.1、竖直 ×1.0**，还有 10% 概率整块不胀。墙身里的胀大被邻居挡住看不见，顶排那圈直接压在天空上 |
| 更亮更"厚" | 砖是 ±0.5 单位盒。墙身每块只露一个侧面，**顶排露侧面 + 顶面两个面**，而顶面是唯一被太阳直射的 |
| 层高本来就不齐 | `split_into_random_rows` 给的是**随机层高**。顶层砖高与下面几层不同不是特意做大，是每层都在随机 |

再叠 `sine_wave::BrickRowSine`（整排 Y 起伏）与 `utils::perturb_row_ends`（扰动行端），
顶边与墙端就成了参差的废墟感。

#### 4.3 平屋顶那圈垛口：是墙的顶层砖【推测，理由链完整】

- `FlatRoofRimStyle` 只是**屋顶的一个样式枚举**（`SetFlatRoofRimStyleCmd` + UI `FlatRoofRimTypeToggle`）【确凿】；
- `system_roof::visual` 全部只有 7 个函数 —— `assemble_roof_tiles` / `generate_roof_stone_floor_and_roof_bottom` /
  `place_spires` / `place_support_beams` / `roof_animation` / `roof_snow` / `switch_roof_type`
  —— **没有任何 rim / merlon 产出函数**【确凿】；
- 而 `construct_walls` 的签名里恰好读 `Query<&Roof>`【确凿，`[分析]` §1.3】。

⇒ 垛口由**墙**产出，与转角砖同一个 `Vec<MyBrick>`。具体在 `WallConstructor::from_curve` 里哪一步交替留空，
PDB 查不到（私有 / 内联，符号表只暴露 `WallConstructor` 与 `impl`）。

#### 4.4 平屋顶顶面石板：nani，不是砖【确凿】

`system_roof::visual::generate_roof_stone_floor_and_roof_bottom` → `StoneFloorInstanceData` → `_nani_roof_floor`。
实例参数 `{position, basis0_xz, scale, shape, double_sided, is_roof_bottom, material_id, pattern_id}`；
网格由 `startup::load_mesh_atlases::generate_roof_floor_meshes` **运行时生成**，不在 `nani_meshes.ron` 里。
Y 量化 `floor(y*512)/512 + fract(wall_id*1.618)/512` **专门防 z-fighting**。

#### 4.5 「只是删砖吗」——TG 里没有"删砖"这个操作【确凿】

`OnWallChanged` → `construct_walls` → **整墙全量重排**（`[分析]` §1.1：画线进行中与抬笔定稿走完全相同的重建管线，
砖 SSBO 无任何预览专用变体）。砖在不在，是这一次排布函数的**输出**，不是对上一次结果的编辑
—— 所以"删除"与"重新放置"都把它当成了两步，实际只有一步。

TG 确实有一条裁砖通路 `utils::trim_rows`（`RowTrimmerSink` / `TrimmedRow`），但它的输入是 `WallHoles`,
是**按洞裁**（门 `construct_gates` / 楼梯穿墙 `playermade_stairs_add_wall_holes` / 形状相交
`add_hole_at_shape_intersection`），**不是按顶边裁**。

#### 4.6 拱上方那一段是另一回事【确凿】

`init_wall_height_and_arch_segment_height` —— 系统名字就写着：**墙高与「拱段高」是分别初始化的**。
拱那一段的墙被单独抬高，所以拱顶上方那几块砖比两侧顶排高。不是顶排的一部分。

---

### 五、待确认（本卷新增三条）

| # | 问题 | 需要什么证据才能确证 |
| --- | --- | --- |
| V5-1 | 顶排是否有**专门的排布分支**（例如「最后一层强制用更大的砖长」） | 反编译 `WallConstructor::from_curve`；或游戏内对照实验 —— 连续调墙高，看顶排砖尺寸是否与其它层一样随机跳。**顶排恒定而其它层随机 ⇒ 有特判** |
| V5-2 | `StitchBrick` 走墙砖专用管线还是 nani | 找 `spawn_stitch_bricks` 里 spawn 的组件类型，或在 `AssetSsboLibrary` 里定位它注册的 buffer |
| V5-3 | 垛口在 `from_curve` 里的具体交替机制 | 同 V5-1，需要反编译 |

---

### 六、对本项目的行动项

| # | 行动 | 依据 |
| --- | --- | --- |
| **A11** ✅ | **（转角角石部分已于 2026-08-31 落地）城齿 / 转角角石 / 勒脚 / 檐口不该各写一套** —— 全部并入 `SolveBlockLayout`，只换驱动曲线。这**修正了本文卷一 A7**「转角护角砖不依赖任何接缝设施」的措辞：它不只是「和门框砖共用一路」，而是和垛口、勒脚、门框墩共用**同一个求解器** | §2.1、§3.3、`[计划]` D11 |
| **A12** | 拱缘装饰带若要做，**照 hat 件走真 SplineMesh**（一条烘好的多砖网格 + 3 控制点弯 + 顶点 `brick_id`），别逐砖实例 —— 比实例便宜，且跨度变化时天然不露缝。TG 自己就在这儿破了「全部刚体实例」的例 | §2.2 |
| **A13** | **屋顶瓦不要复用 `CSHouseFrame`。** 它在 TG 是独立实例格式 + 独立 VS + 独立 compute。倒是「屋脊 / 垛口用砖收口」可以直接吃现成的砖路 | §3.5 |
| **A14** | 若要复刻上沿的观感，**别去建"上沿石头"这个构件** —— 按 §4.2 的三条（水平向胀大、顶面受光、随机层高）调参即可 | §4.1、§4.2 |

---

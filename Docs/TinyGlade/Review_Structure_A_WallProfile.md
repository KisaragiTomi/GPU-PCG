# 深挖 C：墙体 / 剖面契约 / 矩形假设

> 子代理 A 的完整报告（2026-09-07，只读审查），补完主审查中断的「深挖 C」。主审查抽查了 A1 / A2 / A3 / A5 / A6 / A7 / A9 的证据后，把结论并入 [`Review_Structure.md`](Review_Structure.md) 的大问题 3、4 与「已知但不算结构问题」；本文保留 16 行洞曲线消费者对照表、`CLIP_HLSL` 逐语句对照与矩形假设清单作参考。行号按报告生成时刻。

- 审查日期：2026-09-07。只读；行号为读取时刻，基线 `CSHouseActor.cpp` 4541 行、`CSHouseProfile.h` 1019 行、`CSHouseLogicTests.cpp` 4559 行。
- 范围：`Plugins/PCGPlugins/Source/ComputeShaderGenerator/{Public,Private}/CSHouse*`、`Shaders/Private/CSHouseFrame.usf`、`Scripts/TinyGladeMakeWallMaterials.py`、`Review_Structure.md` 的大问题 3 / 4 与「已知但不算结构问题」。
- 标注：**已核实** = 读到源码原文并附引文；**推测** = 由已核实事实推演、未运行任何用例。
- 六条已裁决事项（真几何洞禁、门上开窗弃、转角窗弃、砖层+灰泥层两层、窗附属物自带网格且窗不出框砖、材质禁 unlit）本文只作为前提引用，不再当建议提出。

## 结论摘要

1. **拱曲线在消费者之间有三种定义**（高，已核实）：灰泥裁剪场是椭圆（竖轴 = `Rise()`），门框砖路是**正圆**（`Radius = HW`，`FPath` 根本没有竖向半径），`CSHouse_PierSpanBetween` 的墩顶用 `Z1 − HalfWidth()` 而 `ResolvePierSpans` 的转角墩用 `Z1 − Rise()`。2026-09-04「拱高与洞宽解耦」只改了裁剪场与哈希，没改门框砖与同边墩。默认参数下任何宽于 140 cm 的门（`DoorMaxWidth = 160`）都会露出来：拱圈砖顶端比洞缘高最多 10 cm、同边墩顶比拱脚低最多 10 cm。所有相关单测夹具 `Width ≤ 140` 或 `ArchRise = 0`，恰好落在两者相等的那一点上。
2. **`Circle` 洞的竖向范围有两套口径**（中，已核实）：裁剪场与门框砖路用半宽当竖向半径（`Height` 被无视），谓词、砖层裁行、面板分割、藤蔓下界、装饰包围盒全用 `[Z0, Z1]`。`FCSHouseWindow::Shape` 的注释「`Rect`/`Circle` 上下都有界」只对 `Width == Height` 成立。
3. **`CLIP_HLSL` 与 `CSHouse_ClipKeeps` 判据本体逐语句一致**（已核实，文档结论成立）；唯一差异是 HLSL 多一项「只缩不放」的洞缘噪声 `t = 1 − Amp·v`（CPU 侧无对应，有意）。没有任何自动对照测试。
4. **谓词与几何同维成立**（已核实，`House.WindowPredicateMatchesGeometry` 钉住）；`OnPierSpan` 按高阈保守判是**单向**误差（多拒不漏放）；`LintelBand`/`CornerMargin` 只是谓词余量、几何里没有对应构件。
5. **路径依赖有一条具体的 Undo 不闭合序列**（中，已核实机制 + 推测画面）：`PierSpanIsPier`/`DoorRunMemory` 是 `NonTransactional`，`PostEditUndo → ReevaluateSite` 拿**未回滚**的记忆重算 ⇒ 跨度落在 (60, 75) 时 Ctrl+Z 后的墙与操作前不同。`DoorRunMemory` 存绝对环参数，推 0 号边会让 1–3 号边的记忆整体错位。
6. **接缝三函数全是矩形专用**（已核实）：`Intersects` = 只取两根轴的 SAT，`CutOnEdge` = 对轴对齐矩形的 Liang–Barsky，`BuildCorners` = 4×4 线段求交。A/B 顺序对称靠 `Canonical()`，粗筛口径与 `GetTrackingHash` 同一函数；一次重求值 `GatherSeamNeighbours` 调 2 次、每个邻居 `Intersects` 算 3 次、`BuildCorners` 算 2 次。
7. **「47 行、15 个文件」在当前工作区不可复现**（已核实）：同一 grep 模式今为 35 行 / 13 文件（不含测试）或 58 / 17（含测试）；该模式本身漏掉 `Side < 4`、`Corner < 4`、`case 0..3`、`HX/HY`，按更宽模式人工剔误报后约 94 行 / 14 个非测试文件。三类改动规模见「矩形假设清单」。
8. **「三重叠」推翻**（已核实）：窗不出框砖 ⇒ 窗台砖不存在；砖层开着时砖两面各外凸 6 cm、层层相接，把**整块**灰泥板包在里面而不只是窗下。头注释「③ 未做」已过时：③ 以端头砖剪切落地（`CSHouse_OpeningTopShear` + usf），④ 未做；砖实例记录没有 clip 参数、砖材质没有洞判据、默认容量 512 < 一栋房 1000+ 块。
9. `CSHouse_SillMinZ` 自称「一处砌盒、一处砌砖共用同一个数」在代码里不成立：`CSHouse_BuildBodySoup` 用字面量 `0.5f`，门框路对窗已被掐掉。`CSHouse_SampleOpeningProfile` 没有产线消费者，`CSHouseProfile.h` 文件头描述的「同一条剖面供三处使用」是已退役架构。

## 发现

### A1. 拱曲线三种定义并存：门框砖走正圆、墩顶用半宽、灰泥走椭圆（严重度：高；已核实）

- 证据（真源，椭圆）：`Public/CSHouseProfile.h:329-331` `CSHouse_ComputeClipField`
  ```cpp
  const float Rise = Opening.Rise();
  Field.RefZ = Opening.Z1 - Rise;  // 拱脚
  Field.InvScaleZ = 1.0f / Rise;
  ```
  `Public/CSHouseProfile.h:144-149` `FCSWallOpening::Rise()`：`ArchRise > 0 ? ArchRise : HalfWidth()`，再夹到洞高。
- 证据（产线给的值）：`Private/CSHouseActor.cpp:669-671` `ComputeDoors`
  ```cpp
  float Rise = (DoorMaxArchRise > UE_KINDA_SMALL_NUMBER)
      ? FMath::Min(DoorMaxArchRise, Width * 0.5f)
      : Width * 0.5f;
  ```
  `Public/CSHouseActor.h:663` `DoorMaxArchRise = 70.0f`、`:615` `DoorMaxWidth = 160.0f` ⇒ 宽 ∈ (140, 160] 的门 `Rise = 70 < HW ∈ (70, 80]`。
- 证据（消费者一：门框砖是正圆）：`Private/CSHouseFrame.cpp:137-152` `MakeOpeningPath` Arch 分支
  ```cpp
  OutPath.Radius = HW;
  // ⚠️ 起拱线要夹到洞底以上：clip 场的 `RefZ = Z1 − 半宽` 没有这个夹 ……
  OutPath.TopZ = FMath::Max(Field.RefZ, Opening.Z0);
  OutPath.MidKind = EMidKind::Arc;  OutPath.MidSweep = PI;
  ```
  `Public/CSHouseFrame.h:124-176` `FPath` 只有 `Radius` 一个半径；`:199-205` `EvalPath` 圆弧分支 `OutSZ = (CenterS − Radius·cosθ, TopZ + Radius·sinθ)`；`Shaders/Private/CSHouseFrame.usf:109` `FrameEvalPath` 逐字相同。⇒ 砖路是圆心在 `Z1 − Rise`、半径 `HW` 的**正半圆**，拱顶在 `Z1 − Rise + HW`；灰泥洞顶在 `Z1`。`CSHouseFrame.h:44` 文件头仍写着「`1/InvScaleZ` → 矩形洞的半高（拱与圆恒等于半宽，所以那两种不需要它）」——这是 09-04 之前的事实。
- 证据（消费者二：同边墩顶用半宽）：`Public/CSHouseProfile.h:541` `CSHouse_PierSpanBetween`
  ```cpp
  OutTopZ = FMath::Max(FMath::Min(Left.Z1 - Left.HalfWidth(), Right.Z1 - Right.HalfWidth()), 0.0f);
  ```
  它同时喂三处：`Private/CSHouseActor.cpp:1423-1428` `CSHouse_BuildBodySoup` 的 `SpanZ0 → CSHouse_PierClipField`（灰泥裁到哪）、`Private/CSHouseFrame.cpp:165-182` `MakePierPath`（墩柱砌到哪）、`Public/CSHouseProfile.h:997` 谓词（只用 `Span`）。
- 证据（消费者三：转角墩用 `Rise`，作者知道这件事）：`Private/CSHouseActor.cpp:840-842` `ResolvePierSpans`
  ```cpp
  // 墩顶 = 两条起拱线的较低者（与 `CSHouse_PierSpanBetween` 同一个取法）。起拱线用
  // `Z1 − Rise()`：2026-09-04 拱高解耦之后它才是墙上裁剪场真正的那条线，半宽只在正半圆时相等。
  CornerPierTopZ[Corner] = FMath::Max(FMath::Min(Far->Z1 - Far->Rise(), Near->Z1 - Near->Rise()), 0.0f);
  ```
  注释声称「同一个取法」，但 `PierSpanBetween` 用的是 `HalfWidth()`——同一栋房里转角墩与同边墩的墩顶口径不同。
- 证据（其它消费者都用 `Rise`）：门扇 `Private/CSHouseActor.cpp:3791` `SpringZ = Max(Door.Z1 - Door.Rise(), 0)`；砖层裁行 `Public/CSHouseProfile.h:373-390` `CSHouse_OpeningHalfWidthAtZ` 经 `ComputeClipField`；哈希 `Private/CSHouseActor.cpp:743` 含 `ArchRise` 与 `Rise()`。
- 证据（测试盲区）：`Tests/CSHouseLogicTests.cpp:1225-1234` `CSHouseTest_DemoArch` 固定 `Width = 140`、无 `ArchRise`；`:1986` PierPlaster `Left.Width = 120`；`:525-605` ArchRise 测试只比 `ClipKeeps` 与 `SampleOpeningProfile`；`:2512` FrameWindowSill 自己也用 `Door.Z1 - Door.HalfWidth()` 算樘高。grep `ArchRise` 在测试里只出现在 513-536 与 3643-3668，没有一条把扁拱送进 `MakeOpeningPath` / `PierSpanBetween`。
- 后果 / 触发与症状：地面上画一条宽 150–160 cm 的路开一道门（或一条宽路被 `CSHouse_SplitRun` 切成两道 160 的拱廊）。① 拱圈砖沿正半圆铺，拱顶砖心比灰泥洞顶高 `HW − 70`（最多 10 cm），砖的进深 20 + 外凸 6 勉强遮住，但两肩处砖与洞缘明显脱开、洞缘露出灰泥断口；② 拱廊时同边墩柱只砌到 `Z1 − HW`（85），拱圈从 `Z1 − Rise`（95）起，中间 10 cm 砖链断开；③ 跨度那块面板的墩裁剪场只裁到 85，85–95 之间留一条 10 cm 高、跨度宽的灰泥横带悬在墩顶上；④ 同一栋房若同时有转角墩（用 `Rise`）与同边墩（用 `HW`），两种墩顶高度相差同样 10 cm。
- 建议：把「起拱线」与「拱曲线」收成 `FCSWallOpening` 上的两个成员（`SpringZ()`、以及椭圆半轴），`PierSpanBetween` 改读 `Rise()`；`FPath` 增加竖向半径（或存 `InvScaleZ`）并让 `EvalPath` / `FrameEvalPath` 走椭圆参数化 `Z = TopZ + RiseZ·sinθ`（弧长积分要换成数值或按角均分——TG 的 `ArchFunction::remap_t` 本来就是按角度重映射）；给 `House.ArchRise` 补一组 `Width = 160, ArchRise = 70` 的夹具，同时过 `MakeOpeningPath`、`PierSpanBetween`、`ClipKeeps` 三条。

### A2. `Circle` 的竖向范围：裁剪场 / 门框砖用半宽，谓词 / 砖层 / 面板 / 藤蔓 / 装饰用 `[Z0, Z1]`（严重度：中；已核实）

- 证据：`Public/CSHouseProfile.h:320-323` `CSHouse_ComputeClipField` `case Circle: RefZ = (Z0+Z1)/2; InvScaleZ = 1/HW; // 正圆：竖直尺度服从洞宽`；`:37-41` 枚举注释「Width 同时决定直径，Z0/Z1 被居中的圆覆盖」。
- 证据（用 `[Z0,Z1]` 的一侧）：`:371` `CSHouse_OpeningHalfWidthAtZ` `if (Z < Opening.Z0 || Z > Opening.Z1) return 0.0f;`；`:405-407` `CSHouse_OpeningSpanForBand` 先取 `[LowZ,HighZ] ∩ [Z0,Z1]`；`:972-974` `CSHouse_QueryOpening` 的 `SillTooLow` / `AboveEave` 只看 `Z0`/`Z1`；`Private/CSHouseActor.cpp:1428` 洞面板从 `O.Z0` 起砌、`:1356` `Z0 > 0.5f` 时下面是无 clip 的实心窗台盒；`Private/CSHouseVine.cpp:96` `if (Z < O.Z0 - C) continue;`；`Private/CSHouseDecor.cpp:69` 包围盒 `[Z0−C, Z1+C]`。
- 证据（入口开放）：`Public/CSHouseActor.h:158-162` `FCSHouseWindow::Shape` 注释「`Rect`/`Circle` 上下都有界」；`Public/CSHouseFeatureMarker.h:415` `ACSWindowMarker::Shape` 同样可选 `Circle`，`Width`/`Height` 独立可填（`:405-409`）。
- 后果 / 触发与症状：把一扇 `Windows` 表里的窗改成 `Circle`、`Width = 150, SillZ = 90, Height = 110`（圆心 145、半径 75 ⇒ 圆占 [70, 220]）。灰泥按圆裁：面板从 90 起，圆的下弧被窗台盒顶面截平成一条直线；洞顶越过 `Z1 = 200` 到 220，那 20 cm 灰泥没了、砖层（开着时）在 200–220 的行照旧砌着 ⇒ 洞顶露出一排悬砖；谓词按 `Z1 = 200` 判 `AboveEave`，实际洞顶 220。圆形窗的预制框（78 × 160 的方框）本来也盖不住圆洞，所以这条不只是理论。
- 建议：让 `Circle` 的 `Z0/Z1` 成为派生量（`Z0 = Zc − HW, Z1 = Zc + HW`）在 `BuildWindowOpenings` 里就规整掉，或在 `CSHouse_QueryOpening` 加一条 `Degenerate`/新理由拒掉 `Width ≠ Height` 的圆；测试 `House.ClipField` 的圆例（`:958-972`）用的正是 `Width == Height`，补一个不等的。

### A3. 迟回记忆不随 Undo 回滚，`PostEditUndo` 收敛到的是操作**后**的状态；环参数记忆推边后整体错位（严重度：中；机制已核实，画面为推测）

- 证据：`Public/CSHouseActor.h:2203-2204` `UPROPERTY(NonTransactional) TArray<FCSDoorRunMemory> DoorRunMemory;`、`:2213-2214` `UPROPERTY(NonTransactional) TMap<uint32, bool> PierSpanIsPier;`、`:2189-2190` `UPROPERTY(Transient) TArray<FCSWallOpening> CurrentOpenings;`。`Private/CSHouseActor.cpp:4532-4540` `PostEditUndo` 只调 `ReevaluateSite()`。
- 证据（迟回本体）：`Public/CSHouseProfile.h:508-514` `CSHouse_SpanIsPier`：`bWasPier ? (Span < High) : (Span <= MaxWidth)`，默认 `MaxWidth = 60`、`RestoreWidth = 75`（`CSHouseActor.h:568, 576`）。`Private/CSHouseActor.cpp:791-793` `ResolvePierSpans` 以 `(边, 该边洞数, 序号)` 为键读 `PierSpanIsPier`。
- 操作序列（同一世界状态两种房子）：① 新放一栋房，路让同一面墙上出现两道拱、跨度 70 ⇒ `70 > 60` 不是墩：起拱线以下有灰泥、两拱各出门樘、装门扇。② 拖抓手把这面墙拉短使跨度变成 55 ⇒ 墩：灰泥裁掉、门樘收、墩柱、`CSHouse_StyleNoLeafMask` 让门扇消失。③ Ctrl+Z。`FootprintSize` 回到跨度 70，但 `PierSpanIsPier` 是 `NonTransactional` 仍记着「墩」，`70 < 75` ⇒ 仍是墩。画面与 ① 不同：灰泥没了、门扇没了。再 Ctrl+Y / Ctrl+Z 也回不去，只能改参数越过 75。
- 证据（环参数错位）：`Private/CSHouseActor.cpp:590` 记忆存的是 `Ring.S0/S1`（`EdgeIndex = -1`），`:516-523` `EdgeStart[Edge]` 是四条边长的前缀和；`Public/CSHouseDoorRuns.h:247-255` 继承判据是 `Run.Overlaps(Old)` ⇒ 门槛从 `MinWidth = 40` 降到 `KeepWidth = 32`。推 0 号边 Δ 后 1–3 号边的环参数整体平移 Δ，旧记忆区间落到别的物理位置上。
- 推测后果：一次大幅属性改动（`FootprintSize.X` 600 → 800）后，某条 2 号边上宽 35 的路（本来 < 40 开不了门）恰好被平移过来的旧区间盖住 ⇒ `35 ≥ 32` 放行 ⇒ 开出一道门；下一帧它自己进了记忆 ⇒ 门**持久存在**。文档写「只发生一次」只对连续拖动成立。
- 建议（不违反裁决）：记忆键改成与边相对的量（`EdgeIndex + 边内 S`，或按 `SourceId`），并在 `PostEditUndo` 里把两张记忆清空再重算——Undo 的语义本来就是回到用户看到过的那个状态，不是回到迟回轨迹上的某一点。

### A4. 接缝：三函数矩形专用；对称性与口径正确；同一事实一次重求值算两到三遍（严重度：中；已核实）

- 矩形专用证据：`Public/CSHouseSeam.h:141-148` `FootprintCorners(const FHouse&, FVector2D Out[4])` 直接写 `(±HX, ±HY)`；`:173-204` `Intersects` 分离轴只取 `EdgeOutward(A, 0..1)` 与 `EdgeOutward(B, 0..1)` 四根（注释「2D 下矩形的边法线就是它的轴」）——凸多边形要全部边法线，凹多边形要先分解；`:287-322` `CutOnEdge` 把墙段端点变到 `Other` 局部后对 `±HX/±HY` 四个半平面做 Liang–Barsky（`:299-313`），输出**一个**区间 `FCSWallCut`——凹邻居会切出多段；`:233-278` `BuildCorners` 4×4 求交、`(I+1)&3`。`Reach()`（`:81`）= 半对角线。
- 对称性：`:100-105` `Canonical` 按 GUID 定序，`BuildCorners` 内部调用它；`Intersects` 的 `Separated` 对 P/Q 对称；`OverlapZ` 对称。`Tests/CSHouseLogicTests.cpp:2728-2735` 钉住 `Intersects(A,B) == Intersects(B,A)` 与 `BuildCorners` 两序逐位相同。`CutOnEdge(Self, Other, Edge)` 本质是「我的墙被对方盖住的段」，两栋房各切各的墙，不存在对调问题。**已核实对称。**
- 口径一致：`Private/CSHouseActor.cpp:897-923` `GetTrackingHash` 与 `:2267-2300` `ComputeSeamCuts`、`:2302-2336` `BuildSeamBricks` 都调同一个 `GatherSeamNeighbours`（`:2239-2265`，`WithinReach` + 自身 `bSeamEnabled` + `HouseId` 有效）。哈希收邻居的 `Center/BaseZ/Yaw/Footprint/WallHeight`，不收邻居 `WallThickness`——`CutOnEdge` 只读 `Other.Footprint`、`EdgeOutward` 只用方向，邻居墙厚确实不影响接缝，**无缺口**。
- 调用次数：一次 `ReevaluateSite`（`:997` `ComputeSeamCuts`、`:1032 → RebuildFrame → 2127 BuildSeamBricks`）`GatherSeamNeighbours` 2 次，每次 `GetTrackedHouses` 重新排序（`Private/CSHouseSubsystem.cpp:57-70`）；每个邻居 `Intersects` 3 次（`:2280` 显式一次 + 两次 `BuildCorners` 内部）、`BuildCorners` 2 次、`CutOnEdge` 4 次。子系统重求值后再调一次 `GetTrackingHash`（`CSHouseSubsystem.cpp:177`），0.25 s 快扫每栋房又一次（`:149-158`）。
- 建议：把 `ComputeSeamCuts` 的产物（`Corners` 表）留给 `BuildSeamBricks` 用；折线升级时 `BuildCorners` 可机械推广，`Intersects`/`CutOnEdge` 需重写（凸：Cyrus–Beck；凹：`FCSWallCut` 改成区间表——`CSHouse_BuildBodySoup:1329-1340` 已经会并重叠段，接得上）。

### A5. 矩形假设：文档计数不可复现，且其 grep 模式系统性漏计（严重度：中；已核实）

- 证据：文档 `Review_Structure.md:47, 200-206` 写「47 行、15 个文件」，模式 `grep "Edge < 4\|\[4\]\|& 3\|% 4"`。本次同模式复现：`Source/ComputeShaderGenerator/{Public,Private}/CSHouse*` 不含测试 **35 行 / 13 文件**，含 `Tests/CSHouse*` **58 / 17**，整个模块含测试 65 / 22。逐文件：Actor.cpp 11（文档 16）、Seam.h 5（6）、Profile.h 5（4）、Roof.h 2（4）、Decor.cpp 2（3）、Tile.cpp **0**（3，现用 `Side < 4`/`Corner < 4`）、Quoin.h 1（2）、BrickWall.h 2（1）、Frame.cpp **0**（1，边号循环已搬到 `BuildFrameArches`）。
- 证据（模式漏计）：`Private/CSHouseTile.cpp:166, 381` `for (int32 Side = 0; Side < 4; ++Side)`、`:278` `Corner < 4`；`Private/CSHouseDecor.cpp:228` `Side < 4`；`Public/CSHouseQuoin.h:65-74` `switch (Index & 3)` 的 `case 0..3`；`Public/CSHouseResize.h:34-43`；`Public/CSHouseProfile.h:826, 967` `EdgeIndex <= 3` / `> 3`；`Public/CSHouseActor.h:137` `ClampMax = "3"`。按扩展模式扫描后人工剔除误报（`ClampMax = "30.0"`、`T*T` 之类）约 **94 行 / 14 个非测试文件**；测试夹具另有 `CSHouseDecorTests.cpp:35-44`、`CSHouseVineTests.cpp:459-466` 各自手抄了四边框架表。
- 后果：数字本身不重要，重要的是「量化表」被当成进度尺（计划书 5 处 → 47 处）；用一个漏计的模式衡量会低估折线升级的规模，尤其漏掉屋面 / 瓦 / 装饰那一整族。
- 建议：以「矩形假设清单」一节的三分类为准；若要保留计数，把模式写进文档并把 `Side`/`Corner`/`case`/`HX,HY` 收进去。

### A6. 砖层与面板：三重叠推翻；砖层开着时整块灰泥板被砖包住；③ 已做、④ 未做；两层落地差距（严重度：中；已核实）

- 证据（窗台砖不存在）：`Private/CSHouseFrame.cpp:252-263` `BuildEdgeElements`
  ```cpp
  if (Opening.Type != ECSOpeningType::Window && MakeOpeningPath(Opening, Path))
  ```
  注释：「**窗不出框砖**（2026-09-06 …）只掐产线这一路，`MakeOpeningPath` 本身一个字不动 … 单测 `House.FrameWindowSill` 直接调它」。门恒 `Z0 = 0` ⇒ `bSill = false`（`:160`）。⇒ 产线上没有任何洞会砌窗台砖。`Scripts/TinyGladeDemoRegression.py:1648` 也断言 "windows grow no frame bricks at all"。
- 证据（砖层几何相对灰泥）：`Public/CSHouseTrim.h:136-165` `BuildTrimElements` 砖路原点 `Mid = F.Start + F.In·T/2`（墙厚正中）；`Public/CSHouseActor.h:756-774` `FrameBrickDepth = 20`（平顶段的高度轴，见 `CSHouseFrame.h:526-531` 注释）、`FrameBrickThickness = 0` = 用墙厚、`FrameBrickProtrude = 6`「两面各凸这么多」；`Public/CSHouseBrickWall.h:63-73` `PlanCourses` 层高 = `WallHeight / Count`，`CSHouseActor.h:970` `BrickWallCourseHeight = 20` ⇒ 300 高分 15 层、每层 20、砖高 20 ⇒ 层层相接（测试 `:3449-3455` 钉「不重不漏」）。⇒ 开砖层后砖体是 `T + 12` 厚的连续壳，灰泥板（厚 `T`）整个在壳内，只在洞缘的阶梯缝里露出 6 cm 深的一圈。
- 证据（窗下方实际是双重叠）：`Private/CSHouseActor.cpp:1356-1360` `AddPanel` 的 `if (Z0 > 0.5f)` 实心窗台盒；`Public/CSHouseProfile.h:405-407` `SpanForBand` 在 `Z < Z0` 的带返回 false ⇒ 砖层在窗下整行照砌。所以是「窗台盒（灰泥）+ 砖层砖」，且这只是全墙双重叠的一部分。
- 证据（③ 已做，④ 未做）：`Public/CSHouseBrickWall.h:28-30`「③ … 与 ④ 都在 GPU 侧，不在本文件」；`Public/CSHouseProfile.h:424-442` `CSHouse_OpeningTopShear`「砖层洞缘四级的③要它」；`Public/CSHouseTrim.h:118-120` `SplitEdge` 把 `Shear` 写进 `FRun::ShearS0/S1`；`Shaders/Private/CSHouseFrame.usf:224-245`「洞缘四级之③：端头那块砖剪切成上宽下窄」。④（普通砖逐像素裁）：砖材质只有 `OpacityMask = saturate(PerInstanceRandom + 1)` 的剔除哨兵（usf `:256-259` 注释），`TinyGladeMakeWallMaterials.py` 只给墙材质接 `CLIP_HLSL`（`:398-406`）。③ 的已知限制：只处理往上收窄（`Profile.h:430-431`），单砖两头挨洞取较大者（usf `:235-238`）；砖顶边仍是直线，不是 TG `flags & 32` 的逐顶点曲线缩放——弦弓高 ≈ 1 cm（R = 80、砖长 26），今天被灰泥缩洞噪声盖住。
- 证据（容量）：`Public/CSHouseActor.h:832` `FrameReserveCapacity = 512`；`Tests/CSHouseLogicTests.cpp:3505-3510`「一栋 6×4 m、檐高 3 m 的房子要 1000+ 块砖 … 砖层一开就会被截断」；`Private/CSHouseActor.cpp:2142-2148` `RebuildFrame` 撞上限只打 Warning、砖层排最后先被截。
- 两层落地最小差距清单（按依赖顺序）：
  1. 统一起拱线 / 拱曲线（A1）——clip 场搬到砖上之前，砖与灰泥必须先认同一条曲线。
  2. 砖实例记录没有 clip 参数：`CSHouseFrame.usf:252-273` 五行 `float4`（三行基、原点+随机数、剔除球），`.w` 通道被引擎占用或为 0。需要第六行或独立 buffer，并改 `CSGpuInstancedMesh.usf` 的行宽。
  3. 砖材质接同一份判据（`CLIP_HLSL` 的第三个副本），并决定砖是否也吃缩洞噪声（TG：砖被洞切、灰泥是盖层）。
  4. 灰泥层零代码：`grep Coverage` 只命中材质 `PEEL_HLSL` 的 `Coverage` 输入（脚本 `:472`）；「规则栅格 + 逐顶点覆盖度」的网格、覆盖度来源、与砖层的次序都不存在。
  5. 窗台盒与 `CSHouse_SillMinZ`（A7）在砖层承担墙体后失去意义，需要明确退役。
  6. 容量 512 → ≥ 每栋 1400（`EstimateBricks`），或砖层单独一个组件 / 容量。
  7. 一条 CPU / 灰泥 HLSL / 砖 HLSL 三方采样点对照测试（今天连两方的都没有）。
- 建议：两层顺序上先做 2–3（砖上带 clip）再做 4；在此之前把 `bBrickWallEnabled` 的语义写成「预览层」，避免验收拿它与面板路比像素。

### A7. 「唯一真源」在三处只是注释：`CSHouse_SillMinZ`、`CSHouse_SampleOpeningProfile`、`CSHouseProfile.h` 文件头（严重度：低；已核实）

- 证据：`Public/CSHouseProfile.h:46-54` `CSHouse_SillMinZ = 0.5f`，注释「`CSHouse_BuildBodySoup` 在 Z0 以下砌一块无 clip 的实心盒 … `CSHouseFrame::MakeOpeningPath` 沿 Z0 铺一圈砖 … 两处必须共用同一个数」。实际：`Private/CSHouseActor.cpp:1356` `if (Z0 > 0.5f)` 是字面量（同函数 `:1353` `H - Z0 < 0.5f`、`SB - SA < 0.5f` 也是）；`Private/CSHouseFrame.cpp:160` 用了常量，但如 A6 所述这条路对窗已掐、对门恒 false。
- 证据：`Public/CSHouseProfile.h:14-18` 文件头「同一条剖面同时供三处使用 —— ① 墙板砌洞 ② 洞口内壁扫掠 ③ 谓词」。① 现在是 `ComputeClipField`，② 已由用户裁决取消（`CSHouseActor.cpp:1434-1438`「洞口的厚度不产扫掠面」），③ 只看 `Width/Z0/Z1`。`CSHouse_SampleOpeningProfile`（`:191-268`）在 `Source/` 产线代码里零调用，只有 `Tests` 的 `ProfileSample` / `ArchRise` 用它。
- 后果：下一个改 `AddPanel` 阈值的人不会知道要改常量；读文件头的人会以为剖面折线还在砌墙。
- 建议：`AddPanel` 改读 `CSHouse_SillMinZ`；文件头改成描述 clip 场；`SampleOpeningProfile` 要么标成「测试用参考实现」，要么删除并把 `ArchRise` 测试改成拿 `HalfWidthAtZ` 反查。

### A8. 谓词 / 几何契约：同维成立，误差单向；两个余量没有几何对应物（严重度：低；已核实）

- 同维：`Public/CSHouseProfile.h:642-650` `CSHouse_OpeningsOverlap` 比面板格（`CSHouse_OpeningCell`，`:444-449`）的一维 S 区间；`Private/CSHouseActor.cpp:1393-1413` `CSHouse_BuildBodySoup` 用同一个 `OpeningCell` 定面板，`CellMin = Clamp(CellMin, Cursor, F.Len)` 只在格相交时收缩，`continue` 只在 `CellMax − CellMin < VisibleWidth − 1` 时触发。谓词放行的窗，其格与已有格按 `Clearance` 不相交、`S0 ≥ CornerMargin`、`S1 ≤ Len − CornerMargin` ⇒ 永远砌得出。`Tests:2193-2489` 扫描两边 × 两形 × 五尺寸钉住「放行 ⇒ 恰好 +36 顶点」。门不过谓词，但门与门之间由 `CSHouse_SplitRun` 留 `PierWidth`，量化最多推 2 cm，远小于 `PierWidth + 1` 的容忍。
- 单向误差：`:985-998` `OnPierSpan` 用 `PierRestoreWidth = max(Restore, Max)`（`CSHouseActor.cpp:481-482`）。跨度 ∈ (60, 75] 且当前不是墩（新房）时，一扇 10 cm 宽的窗（格 30 + 两侧 10）放得下却被拒；反向（放行却是墩）不存在，因为迟回上界正是同一个数。文档「默认参数下不可达」对 `SplitRun` 产出的拱廊成立（跨度 = 20），对两条独立的路开出的相邻拱不成立。
- 无几何对应：`LintelBand`（`AboveEave`）与 `CornerMargin`（`NearCorner`）在 `BuildBodySoup` 里没有任何构件（面板从 `Z0` 砌到 `H`，端盖到 `Len`）；「墙顶连续砖带」实际是 `bTrimTop`（默认 `false`，`CSHouseActor.h:914`）。它们是纯谓词余量，可接受，但拒绝理由的文案（`:1014`「吃掉了墙顶的连续砖带」）描述了不存在的东西。
- 建议：`OnPierSpan` 的文案改成「可能成为墩的跨度」；`AboveEave` 文案去掉「砖带」。

### A9. `CLIP_HLSL` 与 `CSHouse_ClipKeeps`：判据一致，差异只有单向缩洞噪声；无自动对照（严重度：低；已核实）

逐语句对照见「洞曲线消费者对照表」后的附表。结论：三种形状的比较、`p.y <= 0` 的拱脚分支、`Shape·255 + 0.5` 量化、B = 255 哨兵、无 epsilon 均一致；HLSL 多出 `t ∈ [1 − Amp, 1]`（默认 `HoleEdgeNoise = 0.045`，脚本 `:395`）把阈值往洞内推 ≤ 4.5 %（160 宽的门樘 ≤ 3.6 cm，110 高矩形窗的顶 / 底 ≤ 2.5 cm）。CPU 侧所有消费者用标称曲线；砖层裁行另加 `Clearance = 3`、门框砖靠进深 20 + 外凸 6 盖住，藤蔓再让 12，所以缩洞方向今天没有穿帮。风险点：噪声按 `|N|` 选世界投影轴，面板**底面**（法线 ±Z）与正面选到不同的两轴 ⇒ 拱洞里那块底面在两樘脚各留一条 ≤ `Amp·HW` 的舌头（门在地面看不见；拱窗有预制框盖住；`Windows` 表的裸洞看得见）。

## 洞曲线消费者对照表

| # | 消费者 | 位置 | 判据 `<`/`<=` | 拱脚以下下界 | `ArchRise` / 起拱线 | Clearance 方式 | Arch / Rect / Circle |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 面板裁剪场（UV1 = q，B = 形状 id） | `CSHouseActor.cpp:61-94` `FCSHouseMeshWriter::SetPanel/ClipUV`，`Profile.h:300-340` | 由材质判 | **无**（面板底 = `Z0`，下面是实心窗台盒 `:1356`） | 椭圆，`RefZ = Z1 − Rise()`（真源） | 无；墩侧 `PierBite = 0.1`（`:1401-1403`） | 三者各自归一化；Circle 竖向用 `HW` |
| 2 | 材质 `CLIP_HLSL` | `TinyGladeMakeWallMaterials.py:90-132` | `<` / `< t·t`，`t = 1 − Amp·v` | 无（同 1） | 只看 q，自动跟随 | **缩洞**（不外扩） | 三者 + `shape > 2` 保留 |
| 3 | CPU `CSHouse_ClipKeeps` | `Profile.h:342-353` | `<`（边界 = 墙） | 无 | 同 1 | 无 | 三者；`default → Arch` |
| 4 | 砖层 / 包边裁行 `CSHouse_OpeningHalfWidthAtZ` → `SpanForBand` → `Trim::SplitEdge` | `Profile.h:368-421`，`Trim.h:100-128` | **`<=`**（有意，`:365-367, 381-390`） | **有**：`Z ∉ [Z0, Z1] → 0` | 椭圆（经 `ComputeClipField`） | 剪影 S 区间 ± `Clearance`（砖层 3 / 包边 6），Z 不胀 | 三者；Circle 被 `[Z0,Z1]` 截（A2）；带内取最宽 + 端砖剪切 ③ |
| 5 | 门框砖 `CSHouseFrame::MakeOpeningPath` / `EvalPath` / usf | `Frame.cpp:108-163`，`Frame.h:177-231`，usf `:77-140` | 骑在零等值线上 | 有：`BaseZ = Z0`，`TopZ = max(RefZ, Z0)` | 起拱线 = `Z1 − Rise()`，但曲线 = **正圆 R = HW**（A1） | 无；`FrameBrickBloat = 1.1` 负缝 | 三者（Circle = 2π 环 R = HW 于 `RefZ`；Rect = 平顶）；产线只对门（Arch）出砖 |
| 6 | 同边墩 `CSHouse_PierSpanBetween` → 墩裁剪场 / `MakePierPath` | `Profile.h:527-543`，`Actor.cpp:1423-1428`，`Frame.cpp:165-182` | `Span < 0 → false`（相切 = 跨度 0 仍可为墩） | 墩场对称矩形 `[−Top, Top]` ⇒ 墙内无下界 | 墩顶 = `Z1 − HalfWidth()`（A1） | `CSHouse_PierCutMargin = 1` 外扩 | 只认两侧 `Arch` 且 `Z0 ≈ 0` |
| 7 | 转角墩 `ResolvePierSpans` → `CornerPierTopZ` | `Actor.cpp:813-846` | 2 cm 顶端容差 | — | 墩顶 = `Z1 − Rise()` | 跨度 = 墙厚 | 只认 `Arch` 落地 |
| 8 | 藤避洞 `CSHouseVine::IsInsideOpening` | `Vine.cpp:89-108` | `ClipKeeps`（`<`） | **自补**：`Z < Z0 − C → 外` | 胀洞 `Fat`：`Width + 2C, Z0 − C, Z1 + C`，`Rise(Fat)` = `ArchRise`（不变）或 `HW + C` ⇒ 起拱线上移 `C`；注释「恰好是等距外偏移」只对 `ArchRise = 0` 成立（多让，方向安全） | 胀洞后算场 | 三者 |
| 9 | 装饰锚点 `CSHouseDecor_InsideHole` | `Decor.cpp:63-72` | 闭区间 | 有 | 不看形状 | 包围盒 ± `C`（S、Z 都胀，`WallFootHoleClearance = 130`） | 形状无关（包围盒） |
| 10 | 谓词 `CSHouse_QueryOpening` | `Profile.h:964-1003` | `NearCorner: S0 < Margin`；`AboveEave: >`；`OnPierSpan` 严格 | 只看 `Z0`/`Z1` | 不看；`OnPierSpan` 只用跨度 | 格 ± `OpeningClearance = 10`（一维 S） | 形状无关（Circle 真实范围被忽略，A2） |
| 11 | 门扇 `RebuildDoorLeaves` | `Actor.cpp:3788-3793` | — | 门恒 0 | `SpringZ = Z1 − Rise()`，宽 = `Width·Ratio` | — | 只对门 |
| 12 | 窗台 `CSHouse_SillMinZ` | `Profile.h:54`，`Actor.cpp:1356`（字面量）、`Frame.cpp:160` | `Z0 > 0.5` | — | — | — | 面板路对三形状；门框路已死（A7） |
| 13 | 砖层整体 `CSHouseBrickWall::BuildWall` | `BrickWall.h:105-129` | 同 4 | 同 4（窗下整行照砌） | 同 4 | 同 4 | 同 4 |
| 14 | 接缝裁剪 `CSHouse_SeamClipField` | `Profile.h:620-626`，`Seam.h:287-322` | Rect `<` | `BottomZ ≤ 0` 时对称矩形 | — | `CSHouse_SeamCutMargin = 1` | 纯矩形，不是洞 |
| 15 | 剖面折线 `CSHouse_SampleOpeningProfile` | `Profile.h:191-268` | 外接多边形 | `SpringZ = max(Z1 − Rise, Z0)` | 椭圆 | — | 三者；**产线零消费者** |
| 16 | 形状哈希 | `Actor.cpp:735-750` | — | — | `ArchRise` 与 `Rise()` 都进 | — | — |

### `CLIP_HLSL`（`py:90-132`）↔ `CSHouse_ClipKeeps`（`Profile.h:342-353`）+ `FCSOpeningClipField::Eval`（`:292-296`）+ `SetPanel`（`Actor.cpp:86-94`）逐语句对照

| 语句 | HLSL | C++ | 一致？ |
| --- | --- | --- | --- |
| 形状 id 解码 | `int shape = (int)(Shape*255 + 0.5)` | `Semantic.Z = (bValid ? uint8(Shape) : 255) / 255` | 一致（0/1/2/255 往返无损） |
| 无洞哨兵 | `if (shape > 2) return 1` | `if (!bValid) return true`；`Eval` 另返回 `(8, 8)` | 一致（HLSL 靠 B，C++ 靠 `bValid`；`(8,8)` 在任何判据下都在洞外，双保险） |
| 非法 id | `> 2` 保留 | `default:` 按 Arch | 不一致但不可达（枚举只有 3 值） |
| 阈值 | `t = 1 − Amp·v/norm`，`v` 两个八度值噪声 | 恒 1 | **有意差异**：HLSL 只缩不放 |
| Rect | `max(|p.x|,|p.y|) < t` ⇒ 洞 | `!(max(|x|,|y|) < 1)` ⇒ 墙 | 一致（`t = 1` 时） |
| Circle | `dot(p,p) < t·t` | `!(x²+y² < 1)` | 一致 |
| Arch | `(p.y <= 0) ? |p.x| < t : dot(p,p) < t·t` | `!(Y <= 0 ? |X| < 1 : X²+Y² < 1)` | 一致，含 `<=` 的拱脚分支 |
| epsilon | 无 | 无（`PierBite` 在几何侧处理 `x·(1/x)` 舍入） | 一致 |
| 输入来源 | `Q ← TexCoord[1]`，`Shape ← VertexColor.B`（脚本 `:391-404`） | `TexCoordChannels[1] = ClipUV(P)`（`:110`），`Colors = Semantic`（`:111`） | 一致；q 是 (S, Z) 的仿射函数，透视校正插值精确 |
| 对照测试 | 无 | `House.ClipField` 只比 C++ 与剖面折线 | **无 CPU↔GPU 对照** |

## 矩形假设清单

分类：**机械** = 查表 / 换循环上界 / 数组变 `TArray`，语义不变；**天然** = 以「一条线段 + (S, Z)」为输入的纯函数，只要有段框架提供者就不用改；**重写** = 依赖 90° 直角、轴对齐、四角、对侧墙等矩形性质。

| 文件 | 函数 / 位置 | 假设 | 类别 | 规模 |
| --- | --- | --- | --- | --- |
| `Public/CSHouseProfile.h` | `CSHouse_GetEdge` `:672-686` | `switch (EdgeIndex & 3)`、`±HX/±HY`、**东西两面缩短 `2T`**（butt joint） | 查表部分机械；`2T` 约定是**重写**——非 90° 角要换斜接 / 顶点接头，所有默认这个约定的地方（Quoin 注释 `:12`、HeightHandle `:73-76`、Decor/Vine 测试夹具）跟着变 | 1 函数 15 行 + 约定消费者 6 处 |
| 同上 | `CSHouse_RayHitWall :723`、`CSHouse_NearestWall :762` | `Edge < 4` | 机械（外表面判 `dot(Dir,N) < 0` 对折线段仍成立；凹多边形 `NearestWall` 的「背面」判据要改成点在多边形内外） | 2 函数 |
| 同上 | `IsValidAnchor :826`、`MakeWallAnchor :851`、`QueryOpening :967`、`FeatureRejectText :1011` | `<= 3` / `& 3` / 文案 `0..3` | 机械 | 4 处 |
| 同上 | `QueryOpening` `CornerMargin :971` | 护角按边端 S 量 | 语义**重定义**（锐角 / 钝角的护角宽度不同） | 1 处 |
| 同上 | clip 场、`ClipKeeps`、`HalfWidthAtZ`、`SpanForBand`、`TopShear`、`OpeningCell`、`OpeningsOverlap`、`PierSpanBetween`、`SpanIsPier`、`RectCutField`、`PierClipField`、`SeamClipField`、`AnchorS`、`AnchorToLocal` | 全在单边 (S, Z) | 天然 | 0 |
| `Public/CSHouseSeam.h` | `FootprintCorners :141-148`、`Reach :81` | `[4]`、半对角线 | 机械（顶点表 / 包围半径） | 2 |
| 同上 | `Intersects :173-204` | 2+2 根分离轴 | **重写**（凸：全部边法线；凹：分解或多边形裁剪） | 1 函数 30 行 |
| 同上 | `BuildCorners :233-278` | 4×4、`(I+1)&3`、「2 或 4 个交点」 | 机械（N×M） | 1 函数 |
| 同上 | `CutOnEdge :287-322` | Liang–Barsky 对 `±HX/±HY`，单区间输出 | **重写**（凸：Cyrus–Beck；凹：多区间，`FCSWallCut` 变表） | 1 函数 40 行 + `FCSWallCut` |
| `Public/CSHouseQuoin.h` | `CornerSign :65-74`、`BuildQuoins :84-114` | 四角、`Diag = 1/√2`「四角都是直角」、`Footprint < 2T` 退化判 | **重写**（角平分线随内角变；凹角不出角石；内缩量 `T/(2·sin(θ/2))`） | 2 函数 50 行 |
| `Public/CSHouseResize.h` | `EdgeDrivesX :28-31`、`EdgeOuterLocal :34-43`、`ApplyEdgePush :59-72` | 边号 ↔ X/Y 维、「对侧不动、中心随动」 | **重写**（折线推一条边 = 沿法线平移该段并与两邻段重新求交；没有「对侧」） | 3 函数 40 行 |
| `Public/CSHouseRoof.h` | `bRidgeAlongX :89`、`Along/SpanLength :92-95`、`InsetDistance :134-138`、`RidgeZ :148-151`、`EvalNormal :166-186`（`D[4]`/`Fall[4]`）、`IsUnderRoof :190-195` | 矩形直骨架 = `min(HX−|x|, HY−|y|)` | 凸多边形：`InsetDistance` 改成「到各边支撑线距离的 min」即可（机械）；脊 / 角脊 / 脊高 / 法线的并列判定要从高度场重新推；凹多边形：**重写**（真直骨架） | 6 函数 60 行 |
| `Private/CSHouseTile.cpp` | `CSHouseTile_EdgeSpan :112-117`、`BuildPlan` `Side < 4 :166`、角斜脊 `Corner < 4 :278`、`:381` | 轴对齐边、`±1` 符号表选角 | **重写**（逐坡面按屋面多边形铺瓦、角脊由骨架给） | 3 段 120 行 |
| `Private/CSHouseDecor.cpp` | 檐口锚点 `Side < 4 :228`、`MaxRecordsBound :340-362` 周长 `2(X+Y)` | 同上 | 檐口锚**重写**（随屋面）；周长机械 | 2 段 30 行 |
| `Public/CSHouseTrim.h :176`、`Public/CSHouseBrickWall.h :86` | `Edge < 4` | 循环上界 | 机械；`SplitEdge`/`BuildTrimElements`/`BuildWall` 天然 | 2 处 |
| `Public/CSHouseActor.h` | `FCSHouseWindow::EdgeIndex ClampMax = "3" :137`、`CornerPierTopZ[4] :2221`、`FootprintSize`（FVector2D） | — | 机械（`TArray`、顶点表进哈希） | 3 处 + 8 处哈希 |
| `Private/CSHouseActor.cpp` | `ComputeSeatZ :281-292` | footprint AABB 网格取 max | 机械（包围盒）；凹多边形要加内点测试 | 1 |
| 同上 | `ComputeDoors :516-541, 594` | `Frames[4]`、`EdgeStart[4]`、`RingToEdge`、三副本切边 | 机械——**已经是闭合周界求解**，是最接近折线的一段 | 1 函数 40 行 |
| 同上 | `ResolvePierSpans` 跨角配对 `:813-846` | `Corner < 4`、`(Corner+1)&3`、「跨度 = 墙厚」 | 配对机械；「转角就是一个墩、跨度 = 墙厚」是 90° butt joint 的推论，非直角要**重定义** | 1 段 35 行 |
| 同上 | `ComputePillars :937-943` | `Corners[4]`、`% 4` | 机械 | 1 |
| 同上 | `EnterResizeMode :1199`、`SnapResizeHandles` | 4 个锥子 + 1 个框 | **重写**（N 个边抓手或顶点抓手） | 1 函数 |
| 同上 | `CSHouse_BuildBodySoup :1320` | `Edge < 4`；每块面板 `AddBox(… In·T …)` 直挤 | 循环机械；转角靠 `2T` butt joint，非直角面板端面要斜接 ⇒ **重写**端面 | 1 函数 + 端面处理 |
| 同上 | `BuildFrameArches :2060-2078`、`BuildVineStrips :2726`、`BuildDecorSite`、`RebuildDoorLeaves`、`Reanchor…` | 逐边取 `GetEdge` | 天然（循环上界机械） | 0 |
| 同上 | `BuildCornerPierBricks :2368`、`BuildQuoinBricks :2415`、`ComputeSeamCuts :2281` | `Corner < 4`、`Edge < 4` | 机械，但依赖 Quoin / Seam 的重写 | 3 处 |
| 同上 | `Reach = max(X,Y)·0.6` `:1647, 2004, 2919, 4026`、`:3412` | 包围半径 | 机械 | 5 处 |
| `Private/CSHouseHeightHandleActor.{h,cpp}` | `BarComponents[4]`、`UpdateFrameGeometry :66-100` | 四根轴对齐条子、角上加减一个厚度 | **重写**（折线轮廓网格） | 1 类 40 行 |
| `Private/CSHouseResizeHandleActor.cpp :33` | `EdgeIndex & 3` | — | 随 Resize 重写 | 1 |
| `Private/CSHouseVine.cpp :470, 572` | `T·T`、`case 0/1` 轴置换 | 误报（多项式 / 法线轴选择） | 无关 | 0 |
| `Private/CSHouseFrame.cpp` | — | 现无边号假设（文档表里的 1 处已搬走） | — | 0 |
| `Tests/CSHouseDecorTests.cpp :35-44`、`CSHouseVineTests.cpp :459-466`、`CSHouseLogicTests.cpp` 28 处 | 手抄四边框架表 | 测试夹具 | 随 `GetEdge` 改 | 3 文件 |

规模估计（已核实的函数清单，行数为量级）：

- **机械**：约 22 处 / 60 行，半天。
- **天然可推广**：clip / 谓词 / 门框 / 包边 / 砖层 / 藤条 / 摆件 / 门扇 / 锚点，0 行（前提是先有「段框架提供者」替换 `CSHouse_GetEdge`）。
- **必须重写**（5 组，约 23 个函数 / 700–800 行）：① `GetEdge` 的 `2T` 约定 + `BuildBodySoup` 端面（80 行，但它是一切的前置）；② 接缝 `Intersects`/`CutOnEdge`（+ `FCSWallCut` 多区间，120 行）；③ 角石 / 转角墩 / 跨角配对的「角」（120 行）；④ 屋面 + 瓦 + 檐口锚点（300 行，最大单项）；⑤ 拉尺寸语义 + 两族抓手（150 行）。

## 既有文档结论校核

| 条目（`Review_Structure.md`） | 结论 | 证据 |
| --- | --- | --- |
| 大问题 3：「47 行、15 个文件」（`:47, :202`） | **有误 / 不可复现** | 同模式今为 35/13（不含测试）、58/17（含）；模式漏 `Side < 4`（Tile 166/381、Decor 228）、`Corner < 4`、`case`、`HX/HY`（A5） |
| 「`FCSHouseWindow::EdgeIndex` 的 `ClampMax = 3`」 | 正确 | `CSHouseActor.h:137` |
| 「`CornerPierTopZ[4]`」 | 正确 | `CSHouseActor.h:2221` |
| 「`CSHouse_GetEdge` 按 0..3 switch」 | 正确 | `Profile.h:676-682` |
| 「四个拉尺寸抓手 + 一个高度框」 | 正确 | `Actor.cpp:1199`；`HeightHandleActor.h:102` |
| 表：`CSHouseSeam.h` 6 处，「推测：相交判定按矩形 OBB 写」 | 推测**升级为已核实**：`Intersects` 只取两根轴的 SAT、`CutOnEdge` 是对 AABB 的 Liang–Barsky | `Seam.h:173-204, 287-322`（A4） |
| 表：`CSHouseFrame.cpp` 1 处「门框边号」 | **已过时** | 当前 `CSHouseFrame.cpp` 无边号；循环在 `BuildFrameArches`（`Actor.cpp:2060-2078`） |
| 表：`CSHouseTile.cpp` 3 处 | 当前同模式 0 处（用 `Side`） | `Tile.cpp:166, 278, 381` |
| 「`ComputeDoors` 已经把四条边接成闭合周界求解」 | 正确 | `Actor.cpp:516-541` |
| 「若升折线…要重写的：屋面、角石与转角墩、接缝、四个抓手」 | 基本正确；补充：接缝的 `BuildCorners` 可机械推广，只有 `Intersects`/`CutOnEdge` 重写；屋面对凸多边形 `InsetDistance` 可机械改成到边距的 min；额外要重写的：`GetEdge` 的 `2T` 约定与 `BuildBodySoup` 端面、瓦片 / 檐口锚点、`ResolvePierSpans` 的「跨度 = 墙厚」 | 见清单 |
| 大问题 4 表：面板路「实际在画的路」 | 正确 | `Actor.cpp:1306-1455` |
| 表：砖层「①删实例 ②水平贴合已做，③逐顶点垂直贴合未做」 | **部分有误**：③ 已以端头砖剪切落地（`OpeningTopShear` + `FRun::Shear` + usf 224-245），未做的是 TG 式逐顶点曲线缩放；④ 未做 | A6 |
| 表：灰泥层「零代码；`PeelBias` 仍在材质里顶替覆盖度」 | 正确 | 脚本 `:146-230` `PEEL_HLSL`，源码无 Coverage |
| 表：「④ 逐像素兜底 2026-09-06 裁决作废，但面板路完全靠它」 | 正确 | 面板路 = clip 场逐像素 |
| 「同一条洞曲线的消费者：面板裁剪场 / `CLIP_HLSL` / `HalfWidthAtZ` / `CSHouseFrame` / 藤 / 谓词 / 门扇」 | 正确但**不全**：漏 `PierSpanBetween`（自带一套起拱线）、`ResolvePierSpans` 转角、`CSHouseDecor_InsideHole`（包围盒）、接缝 / 墩矩形场、`SampleOpeningProfile`（死） | 对照表 |
| 「`CSHouse_OpeningHalfWidthAtZ`…有意用 `<=` 而 `ClipKeeps` 用 `<`」 | 正确 | `Profile.h:365-367, 381-390` |
| 「`CSHouse_SillMinZ` 同时约束砌窗台盒与砌窗台砖」 | **有误**：`BuildBodySoup` 用字面量 `0.5f`；窗台砖那条路对窗已掐、对门恒 false | `Actor.cpp:1353, 1356`；`Frame.cpp:252-263, 160`（A7） |
| 「砖层开着时窗下方是窗台盒 + 窗台砖 + 砖层砖三重叠（推测）」 | **推翻**：窗台砖不存在；实际为窗台盒 + 砖层砖，且砖层把整块灰泥板包住 | A6 |
| 「本次逐字比对 `CLIP_HLSL` 与 `CSHouse_ClipKeeps`…当前一致」 | 正确（判据本体）；补充：HLSL 多单向缩洞噪声，且无自动对照 | A9 |
| 「改洞的任何语义要动三到四处」 | 正确且偏少：起拱线今天已经分叉成三种（A1） | A1 |
| 「验收门又要求两条路逐像素相同」 | 无法核实（未读验收文档） | — |
| 已知：「窗有两个来源…回归脚本与 `TinyGladeShotWindow.py` 各用哪一种，未核」 | 已核：两者都用 `Windows` 属性表，都不碰 `MarkerWindows`；标记那一半只有 C++ 测试 `House.WindowMarker` / `WindowBrushPlacement` 覆盖。另：`GetWindowUndrawableReason` 的「Windows 列表是空的（窗只从这份显式列表来）」对纯标记房子是错的 | `ShotWindow.py:146, 422`；`DemoRegression.py:1615-1650`；`Actor.cpp:2651` |
| 已知：「`DoorRunMemory` / `PierSpanIsPier` …Undo 后 `CurrentOpenings` 会恢复、两张记忆不会，靠 `PostEditUndo` 重算收敛」 | **部分有误**：收敛到的是操作后的迟回状态，不是操作前的画面（A3 给出序列）；`CurrentOpenings` 是 `Transient`，是否随事务恢复无关紧要，因为 `ReevaluateSite` 会整表重算 | `CSHouseActor.h:2189-2214`；`Actor.cpp:4532-4540` |
| 已知：「房体材质槽 1 四坡改瓦后零三角」 | 正确 | `Actor.cpp:1310-1313` `SlotWall = 0` 唯一使用 |
| 已知：「34 条逻辑测试钉的是纯函数」 | 计数有误：`CSHouseLogicTests.cpp` 35 条；其中 4 条建世界、spawn actor（`WindowMarker :3852`、`WindowBrushPlacement :4038`、`ResizeHandle :4245`、`HeightHandle :4436`），并非纯函数测试 | grep `RunTest` / `CreateNewMap` |

## 未覆盖项

- 未运行任何用例、未出图：A1 / A2 / A3 的画面症状是从代码推演的，数值（10 cm、20 cm）按默认参数手算。
- `CSGpuInstancedMesh.usf` 的 packed 行布局只从 `CSHouseFrame.usf` 注释推断，未读原文；A6 第 2 条「`.w` 被引擎占用」以该注释为据。
- 材质资产本体（`M_TinyGladeWall` / `M_TinyGladeBrick` 的 .uasset）未读，`CLIP_HLSL` 对照的是生成脚本；若资产被手改过，脚本不是真源。
- 验收文档 / 计划书（`Docs/TinyGlade/*`）未读，「验收门要求逐像素相同」与「D4 两层」的原始表述未核。
- `CSHouseVine.cpp` 的跨墙跳转（`NextWall`）对非直角转角的行为未分析；`CSHouseTile.cpp` 只读了边 / 角循环所在段。
- `TinyGladeDemoRegression.py` 是否覆盖 `bBrickWallEnabled` 未查。

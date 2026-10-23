# 地面派生链的默认值：2026-09-06 调参记录

`ACSGroundActor` 的四条派生链（地被 / 石阶 / 石阶材质 / 岩壳）在 2026-09-06 集中改过一轮。
本文只记**改了什么、从什么改到什么、为什么**，以及改动之间的横向影响。

2026-09-12 的地面三层材质修正另见 [Ground_Material.md](Ground_Material.md)，包含草土深色过渡、噪声土斑、反编译依据与用户参考图。

⚠️ **每个参数自己的语义不在这里** —— 权威是源码声明处的注释（`CSGroundActor.h` 的
`FCSGroundCoverSpecies` 与 `CS Ground|Stairs` / `CS Ground|Rock Shell` 两组）。本文不复述，
只在需要读懂表格时给一句话。

## 岩壳：默认值维持原样，实调值只活在关卡里

用户在 `L_TerrainOpsDemo` 上把岩壳调到满意，当天先把这 9 个值提为出厂默认，**随即又还原**
—— 用户看图判定原来那套默认更好。所以：

- **09-06 当天 C++ 默认还原为 TG 口径那一套。** 09-30 后续调整见下文。
- **实调值只留在 `L_TerrainOpsDemo` 的 `Ground_Demo` 实例上**，作为那张演示关卡的覆盖。
- `BP_TinyGladeGround` 在这 9 个上**一个都没覆盖**（它只覆盖 `RockShellMaterial` 与
  已删除的 `RockShellPatternMesh`），所以三层里只有关卡那层有值。

⚠️ **2026-09-30 稀疏化 + 缝隙修正之后**（[RockShell_Sparse.md](RockShell_Sparse.md)「七」）：

- `RockShellPatternScale` 已退出可编辑参数，只保留私有旧资产迁移字段；`PostLoad` 把它换算成 `RockShellCellSize = 旧值 × 553.13`（下表 0.653361 ⇒ 361.4 cm，密度不变）。
- 扩张 / 倾斜 / 胀缩是「TG 口径 cm × PatternScale」（默认档 0.35 倍、本关卡 0.65 倍）；厚度 / 起伏 / 基准偏移仍是世界 cm（当天一度也缩过，露出太少，已撤回）。
- `RockShellChipAmount` 默认 10 → **0**（TG 没有加性磕碰）；`RockShellNoiseWavelength` 默认 150 → **13000**（TG 单倍频，× PatternScale）。
  下表「C++ 默认」一列的 `ChipAmount` 因此已是 0；关卡里 35 的覆盖**仍在**，它是 TG 之外的加项。
- `RockShellCellJitter` 源码默认 0 → **52 原生 cm**，与盖浮高共用随机数，范围为 −19.5..52 cm，再乘胞腔比例与半径淡入。
  同时保留 TG 位移切向量叉积的长度衰减。实施与未解决的盖裙陡沟见 [RockShell_Sparse.md「八」](RockShell_Sparse.md#八盖裙陡沟的补核与实施2026-09-30)。

两套值对照（左列是现行默认，右列是演示关卡里的覆盖）：

| 参数 | C++ 默认（现行） | `L_TerrainOpsDemo` 覆盖 | 差异 |
| --- | --- | --- | --- |
| `RockShellCellSize`（原 `RockShellPatternScale`） | 193.6（0.35） | 361.4（0.653361） | 胞腔 1.9 m → 3.6 m |
| `RockShellReliefFloor` | 0 | 0.132798 | |
| `RockShellCellExpand` | 12 | 0 | 关掉逐胞腔外扩 |
| `RockShellSlopeLo` | 0.75 | 0.288 | ≈ 37° → 16.1° |
| `RockShellSlopeHi` | 1.25 | 0.662244 | ≈ 51° → 33.5° |
| `RockShellRoadSink` | 160 | 285.7728 | |
| `RockShellCellJitter` | 52（09-30 前 0） | 0.326267 | 原生 cm；胀缩与盖浮高共用随机数 |
| `RockShellChipAmount` | 0（09-30 前 10） | 35 | TG 没有这一项 |
| `RockShellChipWavelength` | 300 | 120 | |

⚠️ **右列的权威是 `.umap`，不是本表**。记在这里只是因为二进制资产里的值没法直接读，
真要用请以关卡为准。字面量是能逐位往返回同一个 `float32` 的最短小数，不是四舍五入。

⚠️ **这一列不要"顺手"提为默认**。真要再提一次，先算 `FCSRockShellContractTest` 的三条不变量
（它是**对着 CDO** 断言的，而这 9 个值里有 4 个在判据里）：

```text
SlopeHi > SlopeLo
RoadSink > CellRelief + NoiseAmount + ChipAmount + BaseLift
1 / RoadFade < StairRoadThreshold
```

实调那套当时是算过的，三条都成立（`ChipAmount` 10 → 35 把第二条右边抬了 25，仍有余量）。
以后动 `Chip*` / `CellRelief` / `NoiseAmount` / `BaseLift` 任意一个，都要重算第二条。

## 地被：草与花

| 参数 | 旧 | 新 | 理由 |
| --- | --- | --- | --- |
| `FCSGroundCoverSpecies::MaxSlopeDegrees` | 55° | **30°** | 55° 让草长到 `tan 55° = 1.43` 的坡上，比岩壳铺满的坡（`SlopeHi = 1.25`）还陡，草直接从石头缝里长出来 |
| `FCSGroundCoverSpecies::bCastShadow` | （新增） | **false** | 用户裁决：草和花不投影。一株一 instance、满密度 50 株/m²，投影是这条路上最贵的一项 |

⚠️ **改 C++ 默认对现有内容是死的**：`BP_TinyGladeGround` 的 `Grass` 结构与 C++ CDO 不同
（`Mesh` 填了 `SM_TG_GrassBlade`），UE 的 delta 序列化把**整个结构**原样写进了资产，包括那个 55。
所以 `MaxSlopeDegrees` 是**两层都改了**才生效的。`bCastShadow` 是新增成员，资产里没有它的标签，
加载时吃构造函数默认值，不需要改资产。

⚠️ 坡度门控是**硬阈值**（`CSGroundCover.usf` 的「4) 坡度门控」直接 `return`），而它上面那条遮罩
门控是**概率拒绝**。阈值挂在 55° 时坡本身少见，那条直边基本看不到；降到 30° 之后它落进常见坡度区。
真被看出来了就把坡度门控也改成概率拒绝，与遮罩共用同一套格身份哈希（边界才不会重扫时闪烁）。

⚠️ 30° 这个值是**按 C++ 默认那套岩壳软阈**（`0.75~1.25` ≈ 37°~51°）定的：草在石头露头之前收干净。
`L_TerrainOpsDemo` 用的是自己那套覆盖（`0.288~0.662` ≈ 16°~33.5°），**壳比草先出现** ——
那张关卡里 16°~30° 这一段草和石头是叠着的。这是关卡侧的取舍，不是默认值的问题。

## 裙边摆件：整条默认关

| 参数 | 旧 | 新 |
| --- | --- | --- |
| `ACSGroundActor::bSkirtDecorEnabled` | true | **false** |

2026-09-06 用户裁决：**摆件只该长在房子周围，不该跟地形隆起有任何关系**。

摆件的锚点层本来就是**五家分产**（`CSHouseDecor.h` 的 `EFamily`）：

| 家族 | 生产者 | 载体 | 现状 |
| --- | --- | --- | --- |
| 门 / 拱两侧 | `CSHouseDecor::BuildAnchors` | `ACSHouseActor` | 在跑 |
| 墙脚 | 同上 | `ACSHouseActor` | 在跑 |
| 檐口 / 屋脊 | 同上 | `ACSHouseActor` | 在跑 |
| 窗 | 同上 | `ACSHouseActor` | **未实现**（窗主体 `ACSWindowMarker` 还没落地，留了空分支） |
| **塑形物裙边** | `CSGroundDecor::BuildSkirtAnchors` | `ACSGroundActor` | **本次关掉** |

所以这条裁决落成一个 bool 就够了：`bSkirtDecorEnabled = false` **只关第五家**，房子那三家在另一个
actor 上、有自己的 `ACSHouseActor::bDecorEnabled`（默认开，`BP_TinyGladeHouse` 里三张网格表与
`DecorMaterial` 都配好了），一点不受影响。

⚠️ **代码没有删**。`CSGroundDecor.h` 那一整套（含它那篇「为什么归地面不归塑形物」的归属论证）
原样留着，网格表与材质也仍旧配在 `BP_TinyGladeGround` 上 —— 这是一个勾就能拿回来的开关。
要真删得连 `EFamily::Skirt`、`FParams` 里那四个 `Skirt*` 旋钮、以及 `ACSGroundActor` 侧的
组件与缓冲一起拆，那是另一件事，**未裁决**。

⚠️ 关卡与 BP 都**没有覆盖**这个 bool，所以改 C++ 默认直接生效 —— 但**运行中的编辑器不会热换模块**，
得重启（或 Live Coding）才看得到。

## 石阶：两条新门控

| 参数 | 默认 | 作用 |
| --- | --- | --- |
| `StairMinMoundHeight` | **60** cm | 塑形物峰高低于它就整座不出台阶；0 = 关掉 |
| `bStairDropTopStep` | **true** | 掐掉每座最顶上那一级（台顶那圈） |

60 = 2 × `StairStepHeight`：一级 30 cm 的落差是个路沿不是楼梯，摆一排石块反而像路障。

两条都**必须按整座塑形物的峰高判，不能按本格高度**：本格高度在高包的裙边同样很小，按它判会把
最低那几级也删掉，而那几级正是人要踩的。峰高 = `LiftHeight × (1 + 二次抬升系数)`，即
`GroundShaperEvalOne` 在台顶（`S → 1`）的取值，**不是裸的 `LiftHeight`**。

落地在两个新的场函数（`CSGroundShaperField.ush`）：

| 函数 | 给谁 | 返回 |
| --- | --- | --- |
| `GroundShaperHeightAtXYTallOnly(P, MinPeak)` | `StairMinMoundHeight` | 只把峰高 ≥ `MinPeak` 的塑形物算进来的高度场 |
| `GroundShaperPeakAtXY(P)` | `bStairDropTopStep` | 本点由哪座说了算，那一座的峰高 |

⚠️ 两个都**只许当门控用**，别拿返回值当地面高度：矮座与高座重叠处它们给的是高座那一份，比真地面低，
拿去解等值线或算块底 Z 会让石阶陷进土里。底下的几何仍旧读完整的 `GroundShaperHeightAtXY`。

⚠️ 顶层号用 `ceil` 不能用 `floor`：跨过的层是 `L·Step < Peak`，峰高正好是层高整数倍时
（例如 120 / 30）`L = 4` 那层因为 `H > Level` 取不到等号本来就不出等值线，真正的顶层是 3，
`floor` 会少掐一级。上限因此压到 `ceil(Peak/Step) − 2`。

## 石阶材质：改走 TG 的 `rocky_terrain`

石阶原本挂 `MI_TinyGladeStone`（常数暖灰 + `stone_floor` 的法线/粗糙度三平面，**没有 albedo 贴图**），
画面上读成塑料灰块、和周围岩壳不是一种石头。**这是配错了贴图，不是灯光或法线的问题。**

出处 `_rocky_terrain_stairs_stairs_indirect.raster.hlsl_*.ps_main.glsl`（104 行，逐行读过）：
TG 的石阶归在 **`rocky_terrain`** 那一族，采的是**和岩壳同一张 `rocky_terrain_texture`**。

| 层 | 旧 | 新 |
| --- | --- | --- |
| `StairMaterial` | `MI_TinyGladeStone` | `MI_TinyGladeStairStep`（`AlbedoMult = 1.2`） |
| `StairPebbleMaterial` | `M_TinyGladeStone` | `MI_TinyGladeStairPebble`（`AlbedoMult = 0.72 = 1.2 × 0.6`） |

母材质 `M_TinyGladeStairs` 由 `Scripts/TinyGladeMakeStairsMaterial.py` 建（幂等，可重跑），
四条常数照抄 TG：周期 1250 cm（TG 的 `× 0.08` 米制口径）、三平面权重 `abs(N)` **一次方不锐化**、
逐实例相位、`× 1.2` / pebble `× 0.6`。⚠️ 周期比岩壳那张细一倍多（岩壳 PS 是 `× 0.0333` = 30 m）
—— TG 有意让小构件平铺更密，**别去对齐岩壳**。

⚠️ 逐实例相位读的是 `PerInstanceRandom + VertexColor.A` 而不是裸的 `PerInstanceRandom`：烘成
StaticMesh 之后没有实例了，`PerInstanceRandom` 恒为 0，整片石阶会塌成同一个相位。两条路互斥
地为零，相加即可（同 `M_TinyGladeStone` 的裁决六 ③）。

⚠️ `M_TinyGladeStone` 本身没动 —— 屋顶尖顶（`TinyGladeSetupRoofTiles.py`）还在用它。

## 地被的近 / 远两档（2026-09-22）

用户："让远处使用组合草。近处使用单根草。并且远处只有法线运动没有 WPO。" + "草的数量限制上升到 100w 根"。

改之前：草**一档**，`MaxInstances`（100 万）摊在整张 512 m 见方的地面上 —— 一格一株，
`MakeGridForDensity` 被格数上限逼着让密度退让，50 株/m² 一路退到 **4 株/m²**。近看稀得见土，
而远处每一株仍然在跑 WPO。

改之后（`FCSGroundCoverLOD`，挂在 `FCSGroundCoverSpecies::LOD` 上；草默认开、花默认关）：

| | 近处 | 远处 |
| --- | --- | --- |
| 撒哪儿 | 相机周围一块**窗口**（默认 ±70 m）里满密度 50 株/m² | 整张地面，1.5 簇/m² |
| 网格 | `SM_TG_GrassBlade`（单根草，10 三角） | `SM_TG_GrassClump`（一簇 18 片叶，18 三角） |
| 材质 | `MI_TG_Grass`（WPO 风） | `MI_TG_GrassFar`：**没有 WPO**，同一份风场折进**法线** |
| 画多远 | 0 → `NearDistance`（默认 60 m） | `NearDistance` → ∞ |
| 数量（512 m 地面实测） | 991² = **982081 株**（窗口满格） | **39.4 万簇** |

三条要点：

1. **窗口跟相机，但只按"格"挪**。窗口半宽 = `NearDistance + RescatterDistance`，格数因此恒定 ⇒
   容量恒定 ⇒ 相机挪动时只是**重撒一趟**（一张 RDG 图、不回读），永远不会走那条阻塞的
   `SetInstanceSourceGPU` 交接。相机水平走出 `RescatterDistance`（默认 10 m）才搬一次。
2. **随机源是整张网格里的格号**（`CoverCellOffset` 在 kernel 里加回去），不是窗口内下标 ——
   否则窗口一挪，同一格的草会换一茬，整片草地在相机移动时"翻滚"。
3. **交界是一条过渡带不是一道圈**。两个组件共用 `FadeDistance`（默认 15 m）：近处组件把
   `InstanceEndCullDistance` 按逐实例随机数往里收、远处组件把 `InstanceStartCullDistance` 按
   **同一条公式**往里收，于是带内距离 d 处近处留下 `(B−d)/Fade`、远处留下 `1−(B−d)/Fade`，两者互补。

⚠️ 相机位置取的是 `UWorld::ViewLocationsRenderedLastFrame` —— **只有透视编辑器视口与游戏视口
往里填**（`EditorEngine.cpp:2631` / `GameViewportClient.cpp:1814`），SceneCapture 不填，所以 CSSW
那些常驻捕获不会把窗口拽走。⚠️ 取的是**离当前窗口最近的那一项，不是 `[0]`**：同一个世界开着多个
透视视口时表里有好几项、次序随绘制次序变，取 `[0]` 会让窗口在两个机位之间来回跳，每跳一次就是整窗
重撒一趟（逐帧卡死且不报错）。无头 / 出图脚本没有视口，要用
`SetGroundCoverViewOverride()` 把窗口钉到机位上，否则窗口停在地面正中（这张地面 512 m 见方、
房子在角上，镜头里会一根近处草都没有）。

⚠️ `MaxInstances` 的语义变了：开着两档时它是**窗口的预算**（窗口装不下就收窗口，不再让密度退让）。
100 万的上限对应窗口边长 1023 格；50 株/m² 时 60 m 的画距用掉 991² 格，正好吃满。

远处那两样资产由 `Scripts/TinyGladeMakeGrassClump.py` 造：网格是现摆的 18 片叶，材质是
**`M_TG_Grass` 的副本**再改两处（删掉喂 WPO 的那个节点、把同一个 `TG Wind Bend` 乘上
`NormalWindStrength` 加进法线）。为什么是复制而不是新写一份：过渡带里两档**同框**，albedo /
粗糙度 / sheen 差一点点，交界就会读成一条色带；`MI_TG_GrassFar` 同理是 `MI_TG_Grass` 的副本、
只换父材质，手调过的风参数原样跟过去。

## 待办

- 地被：坡度门控要不要从硬阈值改成概率拒绝（与遮罩门控同一套写法）。
- 石阶材质：粗糙度取的是常数 0.85 —— TG 这条 pass 的 G-buffer 只打包 albedo + normal
  （`out uvec2`），粗糙度不在里面，没有可抄的值。当前阶段材质验收面只有「法线正确 + 颜色贴图
  正确」两条，粗糙度不追。

# Tiny Glade 窗户（D8）合卷：计划 · TG 对照 · 时间线

本文把**窗户**这一条线的内容集中到一处，来源是两篇既有文档，正文逐字搬过来、原处只留存根：

| 来源 | 搬来的是哪几节 | 落在本文哪一部分 |
| --- | --- | --- |
| [`Plan_House.md`](Plan_House.md) | **D8 整章**（含锚点 / 三件网格 / 蓝图分层 / 窗不出框砖 / 落地出入 / 洞的记录形式 / 拖拽握手七个子节） | 一、计划与口径 |
| [`Progress_ModuleComparison.md`](Progress_ModuleComparison.md) 卷二 | **§一 §二 §三 §四 §7.2 C4** | 二、TG 那边到底怎么做的 |
| 同上，卷零 | **C1（08-30）/ D8 收口（08-31）/ 窗退出框砖产线（09-06）/ 三件网格 + 蓝图分层（09-06）** | 三、时间线 |

原处的存根：计划书保留 `## D8 特征标记：ACSHouseFeatureMarker（窗户等）` 这个标题 + 一段结论摘要；
合卷卷二每节留一条指向本文的存根；合卷卷零每条留一行**带原日期**的存根，时间线的顺序与日期没有断。

**本文的小节导航**

- 计划：[D8 特征标记](#d8-特征标记acshousefeaturemarker窗户等) · [锚点 `FCSWallAnchor`](#锚点-fcswallanchor2026-09-06) · [三个具名网格组件](#三个具名网格组件只有一件定洞2026-09-06-用户裁决) · [蓝图分层](#蓝图分层总蓝图调参子蓝图换网格) · [窗不出框砖，门照旧出](#窗不出框砖门照旧出2026-09-06-用户裁决的直接后果) · [落地与本节的三处出入](#落地与本节的三处出入2026-09-06-实现时改的) · [洞的记录形式](#洞的记录形式剖面--摆位不存任何切出几何也不切) · [拖拽握手与吸附回位](#拖拽握手与吸附回位零回读)
- TG 对照：[一 窗户不是门那套机制](#一窗户不是门那套机制--两边都不是而且本项目其实更强) · [二 触发规则](#二窗户的触发规则玩家手放不是墙面剩余空间自动填充确凿) · [三 `FCSWallOpening` 够不够](#三fcswallopening-够不够表达窗户够挖洞不够摆框) · [四 怎么让位](#四cshouse_openingcell--solveblocklayout-要怎么给窗户让位) · [7.2 可用资产](#72-窗户可用资产基本齐全) · [C4 措辞订正](#c4-d8-计划里逐像素-clip-是-tiny-glade-的做法这句对窗户不成立)
- 时间线：[C1（08-30）](#c1-已拍板窗户谓词降维成一维-s-区间2026-08-30) · [D8 收口（08-31）](#d8-收口acswindowmarker-已落地2026-08-31-对照文档-a6) · [窗退出框砖产线（09-06）](#窗退出框砖产线门留着2026-09-06) · [三件网格 + 蓝图分层（09-06）](#三件网格--蓝图分层已落地2026-09-06-用户裁决) · [窗贴墙脚变门（09-16）](#窗贴墙脚变门2026-09-16tg-snap_balcony_door)

⚠️ **证据等级标注沿用来源文档的口径**：【确凿】= PDB 符号 / 反汇编 GLSL / 资产字节 / 本仓源码直接给出，不含推理；
【推测】= 由确凿证据推理得出，推理链在正文写明；【待确认】= 证据不足，正文写明需要什么才能确证。
`[PDB]` / `[GLSL]` / `[资产]` / `[UE资产]` / `[代码]` / `[分析]` 的具体路径见合卷卷二的「证据标注约定」，本文不复述。
⚠️ 一处易误采（原样保留）：`country_core::resources::window::*` 与 `WindowSize` / `OsWindowCmd` 是**操作系统窗口**，与建筑窗户无关。

---

## 现状速查表

窗**现在**是什么形态，逐条给日期与「哪里能验证」。这张表是本文新写的索引，正文出处见每行末尾。

| # | 现在的形态 | 日期 | 哪里能验证 |
| --- | --- | --- | --- |
| 1 | **附属物自带 mesh，房子只挖洞**：窗 = 附属 actor 自带的预制 StaticMesh，房子不砌窗框砖 / 窗台盒 | 2026-09-06 | 单测 `House.WindowMarker`；`CSHouseFeatureMarker.{h,cpp}` 的 `MeshComponent`；默认资产 `decorators_window_cottage_1x1`（78 × 17 × 160、32 三角） |
| 2 | **锚点是权威**：`FCSWallAnchor(EdgeIndex, CornerSide, DistFromCorner, SillZ)` 是唯一序列化权威，actor 变换派生 | 2026-09-06 | 单测 `House.WallAnchor`：近角选择 / 往返 / **拉尺寸后世界位置不动**（含「绝对弧长会滑 100」的反证）/ 夹取 / 不动点 |
| 3 | **三个具名组件只有 `OpeningMesh` 定洞**，`LintelMesh` / `SillMesh` 不承担任何机制；组件恒在、网格可空 | 2026-09-06 | 单测 `House.WindowMarker` 新增段：给 `LintelMesh` / `SillMesh` 挂**故意超大**的假件（引擎 100³ Cube）后 `GetDemandSize` 与窗洞数**逐位不变**；探针 `probe_window_bps.py` |
| 4 | **窗不出框砖，门照旧出**（`BuildEdgeElements` 里的 `Type != Window`）；窗周围不走任何砖头补全，砖路恒为三段（窗台底边第四段已删） | 2026-09-06 / 09-10 | 单测 `House.FrameSkipsWindows`（原 `FrameWindowSill`；同一个洞只换 `Type` 送两遍）+ `House.WindowPredicateMatchesGeometry`（`WindowBricks==0` 且 `DoorPaths>0`）；回归 `demo_house_window` `bricks=130 (was 130)` |
| 5 | **转角窗不做**：窗恒锚单条边，距角过紧**直接不生成**，不吸附 / 不改形 / 不越角 / 不缩窄 | 2026-09-05 | 谓词 `NearCorner`（`S0() < CornerMargin \|\| S1() > Len − CornerMargin`）；TG 的 `generate_cottage_corner_windows` 一族永久退出范围 |
| 6 | **不需要玻璃材质**（TG 里窗也不是透明的） | 2026-09-06 | 资产自带 `MI_window_colors_layer00` 实测 = `M_TG_Texture` 的实例、`MSM_DefaultLit` + `BLEND_Opaque`；四个候选 `MI_*` 实测同结论 ⇒ 卷二 ~~A9~~ 作废 |
| 7 | **蓝图分层**：总蓝图调参、子蓝图换网格，`bAutoSizeFromMesh` 默认开 ⇒ 换档不动一行 C++ | 2026-09-06 | `/PCGPlugins/HouseTest/` 的 `BP_TinyGladeWindow` / `BP_Window_Cottage_1x1` / `BP_Window_Gothic_1x1`；实测 cottage 洞 78 × 160、gothic 洞 **69.4 × 203.6**（帽子 94.2 宽**没有**污染洞） |
| 8 | **谓词 = 同边一维 S 区间**，`Z` 不参与 ⇒ 永久放弃「门上开窗」 | 2026-08-30 | 单测 `House.OpeningOverlap`：同 S 高窗**必冲突**、「把窗抬到 1000 cm 仍冲突」、「两洞让开 20 cm 但两格相交 ⇒ 仍冲突」 |
| 9 | **宿主解析走解析射线 / 就近墙面**，不走引擎 trace（gpumesh 全线 `NoCollision`） | 2026-08-31 | 单测 `House.WallPick`：命中边号 / S / Z / 距离逐项对；**背面射线不命中**；墙顶以上不命中；就近版的夹取与半径 |
| 10 | **被拒 ⇒ 藏网格件、标记与锚点留着**；`PickSprite` 始终可见（藏了才点得中） | 2026-09-06 | `SetMeshPiecesVisible` **按类型**藏、不走根组件传播；对位 TG 的 `validate_blueprints` 不实例化被拒蓝图 |
| 11 | **程序回写 actor 变换只在最终裁决时做**（加载 / 房子重建 / 撤销三条路只读锚点，不打射线）。⚠️ 「不回写」≠「不解析」—— spawn 那一次仍要**降级解析**，见下面第 12 行 | 2026-09-06 | `OnHandleDrag` 里 `SnapToAnchor()` 只在 `bFinal` 分支；回归 `demo_house_window` 的拖动段 `worst drift=0.000 cm over 12 frames` |
| 12 | **笔刷是创建窗户的唯一入口**：进笔刷 → 在墙上点一下 → 立刻退出。`ACSHouseFeatureMarker` 已改 `NotPlaceable`，拖进视口那条路连同它的三道闸一起退役 | 2026-09-06 | 回归 `demo_house_window`：`one brush click on a wall cuts exactly one window` / `a click that misses every wall creates nothing at all` / `swapping the brush class swaps the hole it cuts`；单测 `House.WindowBrushPlacement`（`AdoptAnchor` 三份坐标互证 + 退路可 spawn + 抽象类被挡）与 `PCGEditorProcess.House.WindowBrushEdMode` / `.WindowBrushModeActivation` / `.WindowBrushBlueprints`（EdMode 那一层，2026-09-06 收尾补的） |
| 13 | **窗的可画性不挂在门的砖上**：砖组件只在"还有人用"时查健康，砖层开着却零砖才算失败 | 2026-09-06 | 回归 `demo_house_window`：关掉门/角石/包边后 `bricks=0 windows=3 why=<ok>` |
| 14 | **窗贴墙脚变门**：同一个标记，形态存锚点 `FCSWallAnchor::bDoorForm`；窗底低于 `max(DoorSnapHeight, WindowMinSillZ)` 变门、门底升过它变回窗；门形态放过窗台下限、`Type` 仍是 `Window`（不出门框砖 / 门扇），自带门铃 / 花环、门前踏步与栏杆 | 2026-09-16 | 单测 `House.DoorFormRule` / `House.WindowBecomesDoor` / `Stairs.DoorSteps`；TG 证据 [附录 E](RE_WindowToDoor.md) |

⚠️ 第 11 条的教训要推广到**所有会回写 transform 的抓手**：中途回写会把一次本来能自愈的误判变成不可逆的锁定 ——
因为下一次判据的输入正是你刚写进去的那个错值。逐条见合卷卷零「🐛 spawn 期的误吸附会**自我固化**」（该节留在原处，主题是 spawn 时序不是窗）。

---

## 一、计划与口径（原 计划书 D8 整章）

> 以下正文原样来自 [`Plan_House.md`](Plan_House.md) 的 `## D8 特征标记：ACSHouseFeatureMarker（窗户等）` 整章
> （原 608–854 行）。**只改了标题层级**（`##` → `###`，两个 `###` 子节 → `####`），正文一字未动。

### D8 特征标记：`ACSHouseFeatureMarker`（窗户等）

~~用户裁决：窗户这类物体**本身不带任何可视网格组件**——它 attach 到房子上，作用只是通知房子"这里有一扇窗（或别的什么）"；**房子根据自身状况决定要不要生成**。可见几何（窗框、窗洞）全部由宿主房在自己的网格里产出，标记只是一份诉求声明。~~ **← 2026-09-06 被下面的裁决取代，留档。**

⚠️ **2026-09-06 用户裁决：附属物自己持有 mesh，房子只负责"适配"它。** 上一段「本身不带任何可视网格组件、
可见几何全归宿主房」是布尔时代的产物——cutter 必须在房子的网格操作里，窗框才被迫并进房体。布尔早已退场，
这个理由随之消失。新口径三条：

1. **窗 = 附属 actor 自带的预制网格**（TG 的 `window_cottage_*` / `window_gothic_*` 那套已提取为 StaticMesh），
   `UStaticMeshComponent` 挂在标记上 ⇒ 在视口里**点得到、选得中、能用 gizmo 拖**（用户点名要的那条）。
   房子**不再**为窗产出任何可见几何——不砌窗框砖、不砌窗台盒；它只做一件事：**按窗的诉求在两层墙上挖洞**
   （砖层删实例 + 逐像素裁，灰泥层覆盖度）。洞缘的观感由预制框的翻边盖住，与 TG 完全同构
   （状态文件卷二 §1.2「TG 的窗 = CPU 裁砖出一个粗洞 + 一块预制框盖住洞缘」）。
2. **位置的存储方式与 TG 一致：锚点是唯一权威，actor 变换是派生量。** 标记序列化的是墙局部锚点
   `FCSWallAnchor`，**不是**世界变换；每次宿主重建之后，房子从锚点反算标记该在的位置并写回
   （TG 的 `CachedDecoratorTransforms` + `move_decorators_following_anchors`）。拖 gizmo 是**反向**：
   世界位置 → 解析射线 → 新锚点 → 再由锚点派生回变换（这一步就是"吸附"）。
3. **被拒 ⇒ 隐藏 mesh，标记留着。** TG 的 `validate_blueprints` 不实例化被拒的蓝图：画面上装饰物消失、
   存储里还在。本项目对位：`bCausesCut == false` 时 `SetVisibility(false)`，actor 与锚点原样保留，
   墙一变回来它就重新出现。不这么做会出现"一块窗框贴在没有洞的实墙上"。

**这条反而比原口径更贴近 TG**：TG 的窗从来就是 decorator 自己的 mesh，墙只裁砖。

#### 锚点 `FCSWallAnchor`（2026-09-06）

| 字段 | 含义 | 为什么这么定 |
| --- | --- | --- |
| `EdgeIndex` | 挂在哪条边 | 沿用 |
| `CornerSide` | 从这条边的**起点角**还是**终点角**量起 | 见下「为什么不是绝对弧长」 |
| `DistFromCorner` | 到那个角的沿墙距离 cm | 同上 |
| `SillZ` | 洞底离墙基的高度 cm | 沿用 |

**为什么不是今天的绝对弧长 `CenterS`**：`CenterS` 的原点是 `CSHouse_GetEdge` 的 `F.Start`，而 `PushEdge(e)`
会把 e 那一侧的两个角挪 Δ——恰好是第 e 与第 (e+1)%4 条边的 S 原点。结果是「推第 e 条边，e+1 那面墙没动、
上面的窗却沿墙滑了 Δ」（2026-09-05 四条边逐一推算验证）。改成「离最近的角多远」之后：窗离哪个角近就跟哪个角——
被推那一侧的窗随墙走、另一侧的窗原地不动，**行为可预期，且与 TG `WallCornerAttachment::rectangle_corner_id`
同一思路**。⚠️ TG 直墙锚点 `WallAttachmentAnchor` 的字段构成 PDB 拿不到（状态文件卷二 U4 因此**重新有了下游**）；
归一化 t（`find_closest_norm_curve_t`）是另一个候选，本计划取角相对，理由是它在缩墙时不会把窗推进 `CornerMargin`。

**派生变换** `ACSHouseActor::AnchorToRelative(const FCSWallAnchor&)`（纯函数，进 `CSHouseLogicTests`）：
`S = CornerSide ? Len − Dist : Dist`；位置 = `F.Start + U·S − In·(T/2 + 嵌墙深)`；高度 = `SillZ + Height/2`
（与 `MakeDemand` 的"命中点是窗心"同口径）；朝向 = `−In`（预制框正面朝外）。

**attach**：标记 `AttachToActor(宿主, KeepWorld)` 之后写**相对**变换 ⇒ 整栋房子移动 / 旋转 / 落座抬升由场景图
带着走，一行代码都不用；只有 footprint / 墙高变化才需要房子主动通知标记重算。换宿主 = 重新 attach。
这正是本节原文写的 `AttachToActor`——实现一直没做，今天补上。

**通知**：`ACSHouseActor` 重建完成后遍历 `MarkerWindows` 对应的标记（表里加一个 `TWeakObjectPtr` 反引），
逐个 `SnapToAnchor()`；正在被 gizmo 拖动的那个**不写**（纪律同 D5 `SnapResizeHandles(Except)`）。

**加载**：`PostRegisterAllComponents` 改为**按存储的锚点**登记 + 派生变换，**不再**从 actor 变换反推射线——
否则就是 2026-09-05 分析出的那条"重开关卡窗又挪一次位"。旧存档里没有锚点的标记走一次射线回填（迁移）。

**撤销**：锚点改动前 `Modify()`；override `PostEditUndo` → `SnapToAnchor()` + 重新登记。

**洞的尺寸**：`Width` / `Height` 默认从预制网格的包围盒在墙面上的投影取，可覆盖——"房子适配 mesh"的字面含义。

#### 三个具名网格组件：只有一件定洞（2026-09-06 用户裁决）

附属物的可见几何拆成**三件**，`ACSWindowMarker` 起具名而不是泛型 —— 这正是 TG 用 `setdressing_`
前缀在做的事：**前缀就是角色标记**。

| 组件 | 角色 | TG 对位物 | 摆位 |
| --- | --- | --- | --- |
| `OpeningMesh` | **唯一决定洞的** | `window_cottage_1x1` / `window_gothic_1x1` | +90° yaw |
| `LintelMesh` | 盖顶，房子一无所知 | `setdressing_window_lintel` / `window_gothic_*_hat` | +90° yaw，cottage Z +87.5、gothic 0 |
| `SillMesh` | 窗台，同上 | `setdressing_window_sill` | +90° yaw，**零偏移**（资产已预摆） |

**拆分的唯一目的是标出「谁定洞」。** 可点击性与组件个数无关（那是 actor 层面的事），所以另外两件
不承担任何机制：`GetDemandSize` 只读 `OpeningMesh`，其余一个字不看。

⚠️ **搞错了不报错**：把过梁并进定洞件，包围盒会从 78×160 涨到 125×182，墙上真开出一个比窗大一圈的
洞，而过梁正好把它盖住 —— 谓词、砖数、三角数全绿，只有从侧面或洞的内壁才看得见。

**组件恒在、网格可空**：空槽就是"这一件不存在"，**不另设开关**（判据只留一个真源）。
`OpeningMesh` 也空时退回手填的 `Width` / `Height`。

**朝向订正**：三件都带 **+90° yaw**。资产的 **+X 是宽、+Y 朝外**，而本节约定 +X 是墙的**内**法线。
"+Y 朝外"是量出来的：`setdressing_window_sill` 的 origin `(0, +28, −76)`、Y 向跨 `[+9, +47]`，
整块压在 +Y 一侧（窗台往外挑才排水，花箱也挂在外面）。⚠️ 一度写成 −90°，把资产正面转进了墙里 ——
**单独一块对称的 17 cm 板看不出来**，加上窗台/过梁才暴露。

**被拒时藏全部网格件**（按 `UStaticMeshComponent` 类型遍历，不走根组件传播），对位 TG 的
`validate_blueprints` 不实例化被拒蓝图。⚠️ 藏了就点不中了，所以另配一个 `bIsEditorOnly` 的
`PickSprite`（`UBillboardComponent`）**始终可见** —— 按类型藏而不是传播藏，正是为了不把它一起藏掉，
顺带让子蓝图里新加的网格件自动跟着藏。

#### 蓝图分层：总蓝图调参，子蓝图换网格

⚠️ `ACSWindowMarker` 必须显式 `UCLASS(Blueprintable)`：两个基类都是 `NotBlueprintable`，
**这个说明符会被继承** —— 不打开的话编辑器里根本创建不出蓝图，而且不报错，只是右键菜单里没有。

已落地（`/PCGPlugins/HouseTest/`）：

- **`BP_TinyGladeWindow`**（总蓝图）：探针长度 / 吸附半径 / `WallStandoff` / `bAutoSizeFromMesh`
  / 手填宽高与洞形。子蓝图全部继承。
- **`BP_Window_Cottage_1x1`**：cottage 三件，过梁 Z +87.5。
- **`BP_Window_Gothic_1x1`**：`window_gothic_1x1` + `window_gothic_1x1_hat`（origin 已预摆 ⇒ **Z 0**），无窗台。

因为 `bAutoSizeFromMesh` 默认开着，**洞自动跟着那一档的 `OpeningMesh` 走，C++ 一行不用改**。
实测：cottage 洞 78 × 160、gothic 洞 **69.4 × 203.6** —— gothic 的帽子 94.2 宽却**没有**污染洞，
正是上面那条纪律要守的东西。

**默认资产（2026-09-06 已接）**：`decorators_window_cottage_1x1`（实测 **78 × 17 × 160 cm**、32 三角），
经 `ConstructorHelpers::FObjectFinderOptional` 挂在 `ACSWindowMarker` 的 `MeshComponent` 上。
- 不用 `window_cottage_1x1`：那是**整套窗**、进深 **85 cm**，塞不进 24 cm 的墙；这一块才是文档里
  那块"覆在裁出来的洞口上"的薄框板。
- **材质什么都不用做**：资产自带 `MI_window_colors_layer00`，实测是 `M_TG_Texture` 的实例、
  `MSM_DefaultLit` + `BLEND_Opaque`。**窗不需要玻璃材质 —— TG 里它也不是透明的**
  （用户裁决 2026-09-06，卷二 A9 的"玻璃"那一半随之作废）。
- ⚠️ **资产的进深在自己的 Y 轴上**，而本节约定 **+X 是墙的内法线** ⇒ 网格组件带 **+90° 相对 yaw**。
  少了它，`GetDemandSize` 会把 17 cm 的进深当成窗宽，洞窄成一条缝而谓词一声不吭。
- `FObjectFinderOptional` 而不是 `FObjectFinder`：找不到只是没有默认网格，**不会把 CDO 构造失败
  带崩整个模块** —— 本仓库 Content 处于 LFS 混合状态，资产缺失要当常态处理。

#### 窗不出框砖，门照旧出（2026-09-06 用户裁决的直接后果）

「附属物持有 mesh」拍板之后，窗的洞缘由它自己带的 `OpeningMesh` 盖住，房子再沿洞缘砌一圈就是
**双份几何**。所以 `CSHouseFrame::BuildEdgeElements` 产线里加了一句 `Type != Window`。

**门走的是反方向，而且这条不对称是 TG 的**，不是我们省事：

| | 窗 | 门 |
| --- | --- | --- |
| 谁生成 | 用户拖上去的**附属物**（`ACSWindowMarker`） | **房子自己**（`CSHouseActor.cpp`，由道路推导出的拱） |
| 有没有 marker actor | 有 | **没有**（连类都没有） |
| 谁盖洞缘 | 它自带的预制框 | **没人** ⇒ 只能是砖 |
| 框砖 | **不出** | **照旧出**（那一圈砖就是门框） |

TG 资产表是判据，一条都不用推：`setdressing_` 前缀下与洞有关的只有 **`setdressing_window_lintel`**
和 **`setdressing_window_sill`** 两件，**`setdressing_door_*` 一件都没有**。门那边只有 `door`（门扇）、
`door_handle_circle` / `door_bell` / `door_krans`（五金与花环）、`balcony_door_rank*`（那是阳台那一档，
自带 `_rails`）、`trap_door_*`（屋顶舱盖）—— **全是挂件，没有一件是门框**。我们这边同构：
`CSHouseDecor.cpp` 里门只拿到 `add_autoclutter_around_gates` 那一套（门侧花盆 + 引道摆件），没有框。

⚠️ **别顺手把门也退了**：门一退，洞缘就是砖层裁出来的生断口，谁也不盖。

⚠️ **已知代价（接受）**：属性面板 `Windows` 那一份没有标记、没有网格 ⇒ 从此是**裸洞**。那条路在文档里
一直写着是"授权 / 测试用的便利入口"，真正的来源是标记。

窗周围**不走 TG 的砖头补全**（2026-09-10 用户裁决）：`MakeOpeningPath` 的第四段（窗台底边）连同
`FPath::bSill`、`CSHouse_SillMinZ` 与 `CSHouseFrame.usf` 里那一段已整条删除，砖路恒为三段。判据：单测
`House.FrameSkipsWindows`（原 `FrameWindowSill`）把同一个洞**只换 `Type`** 送两遍，门那一遍照铺满、
窗那一遍**恰好零块零条路**；扫描测试 `House.WindowPredicate…` 同样一次验两头（窗为零、门非零）——
只验前一半的话，把整条产线掐死也能全绿。

#### 落地与本节的三处出入（2026-09-06 实现时改的）

| 计划原文 | 实际 | 为什么 |
| --- | --- | --- |
| 位置 = `F.Start + U·S − In·(T/2 + 嵌墙深)` | `F.Start + U·S − In·WallStandoff`，**`Standoff ≥ 0`，原点在外皮外侧** | 原式把原点推到墙里（或外皮上），派生变换就**不再是宿主解析的不动点**：射线版 `Dist <= 0` 挡掉、就近版判成背面 `continue`，两条通路一起失效 ⇒ "窗吸附一次之后再也解析不到宿主"，而无宿主会自毁，全程不报红。单测 `House.WallAnchor` 的最后一段专门钉这个不动点 |
| `ACSHouseActor::AnchorToRelative` 出**相对**变换 | `ACSHouseActor::AnchorToWorld(Anchor, HalfHeight, Standoff)` 出**世界**变换 | 合成要用 `GetBuildTransform()`（只取 yaw），而它是 private，且"构建空间"是房子的私事——烘常驻流 / `RayHitWall` / 这里三处必须同一个变换，各拼一遍就会出现"房子一转窗贴到旁边去"。纯函数那半仍在 `CSHouseProfile.h`（`CSHouse_AnchorToLocal`），单测直接调它 |
| 窗框砖 / 窗台盒随本条**退役** | **仍照旧砌，且改挂到两层砖墙 P1 之后** | ⚠️ 2026-09-06 订正：原以为「与接资产同一轮」就能退，**是错的**。墙板是 `AddPanel(CellMin, CellMax, Z0, clip)` 从 `Z0` 起砌的，窗台盒填的是 `0..Z0` 那一段 —— 现在删掉它，窗台以下会**一直通到地面**。预制窗框只盖洞缘、盖不住那块。要等砖层能把 `0..Z0` 填上（P1）才谈得上退役 |

**裁决回执怎么回推**：`ACSHouseActor` 在**落位循环里**把每个洞的裁决记进 `CurrentFeatureVerdicts`
（key = `SourceId`），`ReevaluateSite` 末尾的 `NotifyMarkersRebuilt()` 逐个推回标记。
⚠️ **别改成"事后再问一遍谓词"**：那一刻 `CurrentOpenings` 里已经有它自己了，等于拿"已经放进去"的
表去判自己。

⚠️ **2026-09-05 用户裁决：不做转角窗。** 窗恒锚在**单条边**上（`EdgeIndex` + `CenterS`），
**距转角过紧的窗直接不生成** —— 谓词的 `NearCorner`（`S0() < CornerMargin || S1() > Len − CornerMargin`）
就是最终答案：不吸附、不改形、不越角、不缩窄。TG 的 `window_cottage_corner_*` 那一族
（`generate_cottage_corner_windows`）**永久退出范围**，卷二的 W4 与 A14 随之作废。

**Why**：四条刚性边的 footprint 模型下，"骑在墙角上的窗"没有落脚点 —— 它要么逼出多边形
footprint、要么逼出跨边的 clip 场，两者都是比转角窗本身大一个数量级的改造。
⚠️ 这条**只约束窗**：D6 的**门**跨转角（`CSHouse_StyleCornerDoor`、转角段的墩）不受影响。

- **`ACSHouseFeatureMarker`（隐形基类，纯 `AActor`）**：只有一个 `USceneComponent` 根 + 编辑器线框/图标示意（`bIsEditorOnlyActor` 视觉，运行时零渲染），不继承 `ACSTinyGlade`（无 csmesh）。携带：特征类型（`Window` / 后续 `Door` / `Chimney` / `Balcony`…）、**原型形状 id + 宽高**（不携带任何几何资产）。`ACSWindowMarker` = 首个子类。
- **自动 attach，找不到宿主即自毁**（用户裁决，取代早先"无宿主时自由摆放不挖洞"）：放入场景后自行解析宿主——**射线检测**命中房子则 `AttachToActor` 挂上去并吸附到命中墙面；**检测不到房子 → `Destroy()` 自删**。
  - **射线走解析求交，不是引擎 trace**：房子的 gpumesh 全线 `NoCollision`（`CSGpuMeshComponent.cpp:14`），引擎 line trace 打不到——必须调 subsystem/登记表的 `PickHouse(Ray)`（房子是参数化 OBB，D4 已定的拾取方式）。这条不写清楚，实现时用 `LineTraceSingle` 会得到"永远检测不到、窗户一放就没"的结果。
  - 探针：从标记位置沿自身朝向（面向墙的方向）发长 `csh.HostProbeDistance` 的射线；未命中再退一步做 `csh.WindowSnapDist` 内的就近墙面查询（贴着墙但朝向没摆正时不至于误删）。两者都空才自毁。
  - **时机分两层**（用户裁决）：**拖拽期间逐 tick 实时解析并通知宿主**（洞要跟手）；**自毁只在松手时判定**——否则窗户从房 A 拖向房 B 的途中会半路消失。详见下条。
  - **换宿主**：解析到与当前不同的房子 → 先向旧宿主注销 opening、detach，再 attach/登记新宿主——两边各触发一次 `ReevaluateSite`。
  - **宿主被删**：标记随之自毁（"无宿主的标记不存在"是不变量）；编辑器里这是一次可撤销的 transaction，Undo 能把房子和窗户一起找回来。
- **拖拽期间开 tick 实时通知，松手关 tick**（用户裁决）：
  - **开**：拖入场景 / 开始拖动时（首次 `PostEditMove(bFinished=false)`，或拖入生成的预览 actor 在 `PostRegisterAllComponents` 里判定处于拖拽态）→ `SetActorTickEnabled(true)`。
  - **每 tick 做两件事**：① 解析宿主（`PickHouse(Ray)` 解析求交）；② 把当前世界摆位实时通知该房 → 房子跑 `QueryFeaturePlacement` 谓词 + 接受则按新 openings 表重生成两层墙（一次 CPU 生成 + 一次异步上传，速率由在途拒绝自动限制）。**本 tick 找不到宿主只是不通知，不自毁**。
  - **关**：`PostEditMove(bFinished=true)` → `SetActorTickEnabled(false)`，随后做最终裁决：解析到宿主 → attach + 登记 opening；解析不到 → `Destroy()`。
  - ⚠️ **编辑器 tick 的老坑**（与接缝 actor 同一个）：actor 默认在编辑器 world 不 tick，必须 `PrimaryActorTick.bStartWithTickEnabled` 配合 override `ShouldTickIfViewportsOnly() → true`，否则整条实时通知在编辑器里静默失效。
  - **兜底**：tick 开启后若连续 `csh.MarkerDragIdleSeconds`（默认 2 s）既无位移也没收到结束事件 → 自动关 tick 并按当前位置做一次最终裁决，防止漏掉 `bFinished` 导致永久 tick。
  - **与"松手才生成"不冲突**：两者消费的东西不同——**洞（房体网格）要跟手**，所以标记拖拽期实时通知；**decor / 热力图 / 藤蔓**只在 `NotifyEditCommitted` 后跑（D12/D13）。标记松手时既触发自己的最终裁决，也汇入 `NotifyEditCommitted`。
- **注册即诉求**：解析到宿主后登记的内容是"诉求"不是"结果"：特征类型 + **锚点** + 参数（宽 / 高默认取自预制网格包围盒，2026-09-06）。`EndPlay` / detach / 删除 → 注销。
- **房子裁决（核心）**：宿主 `ReevaluateSite()` 遍历标记，逐条判定**可行性**，只有通过的才进 openings / 生成几何：
  - 落点不在任何一面墙的有效矩形内（挂太高超过檐口、落在屋顶/山墙上、悬在墙外）→ 不生成；
  - 距墙角 < `csh.FeatureCornerMargin` 或与门拱子段冲突（门优先）→ 不生成；
  - 与已通过的标记重叠 → 后者让位（按 `SourceId` 稳定排序，不看注册先后，避免重启后结果翻转）；
  - 通过 → 吸附到墙面（对齐法线、贴外皮）→ 登记 opening（`Type=Window`，`SourceId=标记 GUID`，带 `Z0/Z1` 窗台与窗顶）→ 砖层按该洞删实例、把洞缘那几块砖贴合过去，灰泥层把洞内顶点覆盖度写 0（见 D4「墙的两层结构」）；窗框砖并入砖层，不另开通路。
  - 裁决结果回写标记（`bCausesCut` + 拒绝原因），编辑器里以线框颜色区分"已生成 / 被拒（附原因）"——否则用户只会看到"放了个东西但什么都没发生"。`bCausesCut` 的语义就是"房子确实为它切了洞"（用户指定的那个 bool）。
- **洞的实现**：⚠️ **2026-09-05 改为两层**（见 D4「墙的两层结构」）——窗与门走**同一条**通路：砖层按洞粗裁 + 逐砖 `FCSOpeningClipField` 细裁洞缘，灰泥层按逐顶点覆盖度 discard。窗比门多出来的只有"洞底离地"，在两层模型里它**不再需要任何分支**：洞下方照常摆砖就是窗台。重叠仍在登记前被谓词判掉，不需要布尔去"求并"。
- 移动标记 → 重判：从"通过"变"被拒"则墙面合拢（按新的 openings 表重生成两层墙），反之亦然——纯状态谓词，无增量补丁。
- 好处：~~标记零几何、零材质~~（2026-09-06 起标记持有自己的预制网格）删了仍不留痕——它不在房子的任何网格里留下东西；"无宿主标记"这一整类状态被自毁规则消灭，房子的 openings 表里不会留下悬空诉求；**房子是所有"洞"的唯一产出者，附属物是所有"物"的唯一产出者**（各自的材质槽、生成链、哈希守卫各在一处）；将来加"烟囱/雨棚/花箱"只是新增一个标记子类 + 一个预制网格，房子侧要么零改动（不开洞的）、要么复用同一条开洞通路，不动通知与生命周期。

#### 洞的记录形式：剖面 + 摆位（不存任何切出几何，也不切）

**门是固定的扇形/拱形，窗同理——形状是有限集合，根本不需要保存切出来的形状**（用户裁决，推翻此前的"CPU 侧切出碎片"方案）。CPU 只存原型 id + 一组摆位参数；判重叠时把原型的已知剖面按同样参数变换比较；生成时把剖面 / clip 场喂给砖层与灰泥层（2026-09-05 起，见 D4「墙的两层结构」）。

2026-08-29 的追加裁决把这条推到了终点：**既然剖面已经在手上，就不需要先用它生成 cutter 再去减了**——直接拿剖面喂几何（2026-08-29 当时是砌墙板，08-30 起是逐像素判据，09-05 起是砖层贴合 + 灰泥覆盖度；**「剖面是唯一真源」这一条从未变过**）。于是原方案的负担在"不存几何"之外又消掉了一层：

| 原方案的负担 | 现在 |
| --- | --- |
| 布尔弃片捕获（`MB_SRC_KEEP` compact 到 retained buffer） | 不需要 |
| 每次提交一次回读刷新碎片记录 | **回读彻底归零**（不只是热路径零，是全程零） |
| `FCSOpeningCutRecord` 分桶、世界空间三角、失效重捕获 | 不需要 |
| cutter 资产必须开 `bAllowCPUAccess` | 不需要（不读任何网格顶点） |
| 拖拽开始建"除自己外"的冻结集 | 不需要（openings 表本身就是记录，随时可查） |
| 三角对三角距离 + AABB 粗筛的精度分档 | 退化为矩形比较，几十次浮点 |
| cutter 原型**资产**（每形状一份静态网格） | 不需要——只要一条手写 2D 剖面（C++ 或 `UDataAsset`） |
| `ApplyMeshBoolean` 的 mesh 操作数入口（引擎侧待补） | 不再是前置，开放问题关闭 |
| 布尔链的 TDR 三道闸 / `CachedOthersMesh` 单刀增量 / `csh.LiveCutHz` | 全部删除 |

**判重叠 = 沿边一维区间比较**（**用户裁决 2026-08-30，C1 选甲**）：同一 `EdgeIndex` 上，两个洞的**面板格**（`CSHouse_OpeningCell`：`[CenterS ± (Width/2 + PierWidth/2)]`）按 `csh.OpeningClearance` 膨胀后是否相交。**`Z` 不参与**——`Z0/Z1` 只描述洞自身，不再用来判冲突，代价是**永久放弃"门上开窗"**（TG 里这种堆叠也确实罕见）。一度写成 `(S, Z)` 二维矩形，但**当时**墙板生成是沿 S 的单游标扫掠、每块面板只带一个 clip 场，二维谓词会放行"谓词说能放、几何砌不出"的堆叠，直接违反下面那条唯一真源纪律。比**格**而不是比洞，是因为格才是扫掠真正消费的那个区间。⚠️ **2026-09-05 改两层之后这条几何理由不再成立**（每块砖各自带 clip），但 C1 是用户裁决不是权宜之计，**谓词维持一维，门上开窗仍然不做**。不同 `EdgeIndex` 只需在转角附近做一次角余量检查。

**"重复挖洞"问题自然消失**（用户点明的收益）：洞是**声明式的参数记录**，重叠在进 openings 表之前就被判掉——同一处不会被登记两次。门拱本就锚在互不相交的子段上，天然无重叠；窗由谓词挡在门口。

**剖面求值器必须是唯一真源**：`CSHouse_ArchProfile()` / `CSHouse_ComputeClipField()` 一个纯函数，**四处**共用——① 砖层删实例的高度阈值、② 洞缘砖的**贴合**（拿洞缘高度去缩放砖的局部 z / s）、③ 普通砖与灰泥层的**逐像素判据**、④ `QueryFeaturePlacement` 的重叠谓词。（原文写的是「①砌洞 ②洞口内壁扫掠 ③谓词」——②自 2026-08-30 裁决三起就不存在了，本条一并补上这处陈旧。）**四处各写一份**是这条路线最容易出的错（谓词说能放、几何做出来却穿帮；或者贴合过的砖与逐像素判据对不齐，在洞缘留一条亮缝）。进 `CSHouseLogicTests`。

**代价与边界**：形状受限于剖面集合。将来要真正任意的窗形，就加一条剖面——扩展成本是"多一条 2D 剖面 + 一个枚举值"，不是"回到碎片方案"，也不再是"多一份美术资产"。**形状扩展的成本形态因此变了**：从"美术加资产"变成"程序加剖面函数"，若团队更希望美术自助，这是净损失——缓解是把剖面做成 `UDataAsset`（一串 2D 点 + 圆弧段），美术仍可自助。

#### 拖拽握手与吸附回位（零回读）

用户要的交互：拖动期间逐帧把候选位置报给房子问"这里能开窗吗"，房子答应才让标记记住这个位置；与已切过的洞太近/相交则**什么都不做、不回话**；松开鼠标后标记弹回最后一个被答应的位置。

**成本担忧的答复：这条握手一次 GPU 回读都不需要，也不需要每帧全量重建网格。** 前提是守住一条纪律——

> **可行性完全由 CPU 描述符裁决，GPU 只执行、绝不被查询。** 判"能不能开"读的是房子参数、边缘线段、openings 列表（全是 CPU 侧结构）与地面镜像（本就是权威、从不回读）；**绝不允许**出现"先生成一次看看成不成 / 读回网格量一量 / 问 GPU 洞在哪"这类实现。几何生成是裁决的下游，不是裁决的依据。

去掉布尔后这条纪律**没有放松，反而更硬**：`QueryFeaturePlacement` 从"布尔的上游裁决"升级为**唯一真源**——它和砖层贴合、逐像素判据用的是同一个 `CSHouse_ArchProfile()`，谓词说能放就一定做得出，做不出的一定被谓词挡住。

```cpp
// 房子侧：纯 CPU 谓词，无 RDG、无 flush、无 *Sync 调用，微秒级
struct FCSFeaturePlacement
{
    bool       bAccepted = false;
    ECSFeatureReject Reason = None;  // NotOnWall / AboveEave / NearCorner / OverlapsOpening / ConflictsArch
    FTransform SnappedWorld;         // 吸附到墙面后的规范摆位，仅 bAccepted 时有效
};
FCSFeaturePlacement QueryFeaturePlacement(const ACSHouseFeatureMarker& Marker, const FTransform& Candidate) const;
```

- **每帧只做谓词**：世界位置 → 墙局部（几次点积）→ 墙矩形内 + 檐口/护角余量 → 候选洞的 `(S, Z)` 矩形 vs 同边已有 openings 的矩形按 `OpeningClearance` 比较（上节；**排除标记自己的那个洞**，否则微移即自撞）。几十次浮点，没有 RDG pass、没有渲染 flush、没有 `*Sync`、**没有回读**。
- **接受了才发 GPU 重建**（用户裁决的实时反馈）：谓词通过 → 更新 `LastAcceptedAnchor`（2026-09-06 起权威是锚点不是世界变换）→ 按新 openings 表重生成一次两层墙；被拒的帧什么都不做（GPU 完全空闲）。**不再需要增量路径**——去掉布尔后一次重建就是"一次 CPU 生成 + 一次上传"，`CachedOthersMesh` 单刀增量、`UCSMeshPool` 借还暂存网格、`csh.LiveCutHz` 固定节流三样一并删除。限速交给 `EditMeshAsync` 的在途拒绝 + pending 合并（速率自动等于 GPU 实际完成速率）；保留"连续两帧位移小于阈值跳过"作为输入去抖。
- **代价核对**：拖拽期间每帧 = 一次 CPU 矩形谓词 + 至多一次两层墙重生成；**整个交互的回读次数 = 0，异步化后 flush 次数也 = 0**。
- **回位规则**：拖动中程序**不回写**标记位置（与 gizmo 抢写会抖——与 D5 拉尺寸 handle 同一条教训）；松手时若当前位置被拒，锚点回退到 `LastAcceptedAnchor`、再由锚点派生变换 ⇒ 标记弹回**并吸附到墙面**（2026-09-06 起这是 TG `DecoratorBackup` 的逐字对位）；从未被接受过则保持游离且**隐藏 mesh**（`bCausesCut=false`）。⚠️ 2026-09-05 核对：本条当时**零实现**，随 2026-09-06 的锚点重定向一起补。
- **握手语义分两层**，别混：*可行*（每帧谓词，便宜，决定标记能不能停在这）与 *已实现*（提交后房子真的切了洞，置 `bCausesCut`）。用户描述的"告诉附加物体你可以存在在这里"是前者，`bCausesCut` 记的是后者。
- 运行时（PIE）拖拽走同一套谓词，只是位置来源换成解析射线。

**顺带避开的两个真回读**（都在 `UCSMeshOps` 里，拖拽热路径一律禁用）：`ComputeWorldBoundsSync`（GPU 停顿读六个 uint）与 `GetTriangleCountSync` / `GetCountsSync`。房子的 bounds 在 CPU 侧从参数算得出，无需问 GPU。

---

## 二、TG 那边到底怎么做的（原 合卷卷二）

> 以下六节原样来自 [`Progress_ModuleComparison.md`](Progress_ModuleComparison.md) 卷二 · 窗户（D8）与装饰／藤蔓（D12/D13）对照，
> 分别是 §一 / §二 / §三 / §四 / §7.2 / C4（原 3583–3810、3814–3920、3924–3945、3949–4005、4246–4287、4393–4397 行）。
> §7.2 与 C4 由 `####` 提升为 `###` 以对齐本文层级，**其余一字未动**，包括所有【确凿】/【推测】标注、表格、代码块与删除线作废段。

### 一、窗户不是门那套机制 —— 两边都不是，而且本项目其实更强

这是本文最重要的一条：**TG 的窗洞与门拱走的是两条完全不同的路，而本项目今天已经落地的
逐像素 clip 场比 TG 的强一档，窗户不需要任何新机制**。

#### 1.1 TG 墙砖的逐像素裁剪只有**一个下界**，表达不了窗【确凿】

`[GLSL] _wall_wall_brick_lod0.raster...b903e43f.ps_main.glsl` 全文只有**两处** `discard`（L157 / L172），
且是同一个判据的正反两支：

```glsl
if (拱圈石标志 && world_y < 拱高) discard;   // 普通墙砖：拱线以下丢掉 → 挖出拱洞
else if (反向标志 && world_y > 拱高) discard; // flags&8 拱圈石：拱线以上丢掉
```

`world_y` 是世界高度、`拱高` 是 VS 从每砖 `vec3 global_arch_height_vals` 三点插值出来的**一维曲线**
（`[分析]` §1.4 / §1.6 已给出结构）。**这是一条只有下界的高度阈值** —— 它能挖出「从地面到拱顶」
的门，**在结构上不可能挖出「有窗台又有窗楣」的窗**（那需要同时给上下两条边界）。

⇒ **「TG 的窗户也是 analytic clip」这个假设可以直接排除。**

#### 1.2 TG 的窗洞是 **CPU 裁砖**，洞缘由预制窗框盖住【确凿 + 局部推测】

窗洞进的是同一张 `WallHoles` 表，但消费方式不同：

- `[PDB]` L9100（**`decorator_visual::maintain_visual_entities`**，按参数匹配【推测】，
  但它是全仓唯一同时握 `WallHoles` 与 `WindowAutoClutterCandidates` 的系统）：

  ```text
  写: DecoratorVisualState · WallHoles · RaycastWorld · DecoratorPhysicsColliders
      · WindowAutoClutterCandidates · WallAttachedDecoTracker<ChimneyAssemblyParams>
      · WallAttachedDecoTracker<DoorStairsAssemblyParams>
  读: DecoratorBlueprints · PrevDecoratorBlueprints · AssetMesh/Shader/TextureLibrary
  听: OnWallDeleted
  ```

  ⇒ **装饰物装配系统确实写洞表**。这把 `[分析]` §1.3 里「窗 decorator 产洞为【合理推测】」
  提升到【推测（强）】—— 仍差一条「`generate_cottage_wall_windows` 调用了 `WallHoles::add`」的调用边，
  PDB 只给符号名。
- 洞表的消费者是 CPU 排砖：`[PDB]` `utils::trim_rows::{RowTrimmerSink, TrimmedRow, above_to_below,
  below_to_above}` + `utils::resolve_hole_overlap` + `WallConstructor::from_curve::normalize_holes`。
  `above_to_below` / `below_to_above` 这对名字说明**修剪器要处理「砖排从洞上方走到洞下方」的过渡**
  ⇒ 洞有上下两条边界【推测，推理链即这两个函数名 + 窗必须有窗楣这一常识】。
- 灰泥墙也吃洞：`plaster_systems::mirror_holes_as_needed` `[PDB]`。
  ⚠️ **订正（2026-09-05，驱动方逐行读 GLSL + PDB 泛型实参）**：原文这里写"两处 `discard` 都是剥落
  alpha ⇒ 灰泥的洞是真几何"，**两句都错**。实读 `_nani_plaster...ps_main` 的两处 `discard` 是
  **两套完全不同的判据**：

  ```glsl
  float cov = clamp(in_var_C6, 0.0, 1.0);          // C6 = 逐顶点标量，VS 从灰泥自己的顶点缓冲读
  if (cov < 0.1) discard;                          // ① 覆盖度硬切 —— 洞与灰泥片边界走这条
  ...
  float h = smoothstep(0.0, 4.0, in_var_C3.y);     // 沿墙高度（0..4 m）
  if (noise * mix(2.0, 2.8, peel_strength) > h * 2.0) discard;   // ② 才是剥落
  ```

  **灰泥不可能有真几何洞**，因为它是**不可剔除的规则栅格**：VS 把
  `gl_VertexIndex` 拆成 `(row = idx >> 16, col = idx & 0xFFFF)`，字节偏移 `48 * (col + row * row_stride)`，
  `col > quad_count` 时输出 NaN 退化 `[GLSL] _nani_plaster...vs_main L129-146`。**没有索引缓冲、
  没有可跳过的四边形** ⇒ 想在灰泥上开洞，逐顶点覆盖度是**唯一**可用的通道【推测（强）：
  机制唯一性 + `cov` 在灰泥片边缘自然衰减这一实测】。
  `peel_strength` 是**逐墙**参数（`InnerWallState::PlasterPeelAmount`，玩家可调），不是逐顶点。
- **TG 的洞几何就是二维 AABB，没有斜切轴**【确凿，PDB 泛型实参】：
  `generate_plaster_mesh_and_entities<Map<Filter<Iter<tuple$<utils::geometry::aabb::Aabb2,
  HoleType, HoleOrigin> > > > >`（`pdb_symbols.txt:86452`）—— 灰泥网格生成函数**直接吃洞的迭代器**，
  而洞的元组第一项是 `Aabb2`。这把逆向报告 L465「洞需要二维矩形 + 斜切轴是**我的推断**」的前半坐实、
  **后半证伪**：TG 侧没有 `AxisUS`/`Skew` 的对位物。
- 洞缘的观感由**预制窗框网格**兜底，不是靠切得准：`window_cottage_1x1` 是 78×17×160 cm 的
  一块薄框板，覆在裁出来的洞口上 `[资产]`。

⇒ **TG 的窗 = CPU 裁砖出一个粗洞 + 一块预制框盖住洞缘。**

#### 1.2b 反汇编实证：洞就是一个 `Aabb2`，盖顶件完全在洞之外【确凿，2026-09-06】

上面 §1.2 的结论原本挂在符号名与资产尺寸上。2026-09-06 用 `dumpbin` 反汇编（方法见卷零「证据来源」）
把它坐实到指令级，同时**改正了一处**。

**① 洞的存储：20 字节，没有形状字段。** `InProgressHoleStorage::add` 的实际写入：

```asm
movups  xmm0, [rdx]                      ; 16 字节 = Aabb2(min.x, min.y, max.x, max.y)
movups  [rax+rcx*4], xmm0
mov     byte ptr [rax+rcx*4+10h], bl     ; +0x10 = HoleType，1 字节
```

配 `sort4_stable<tuple$<Aabb2, HoleType, HoleOrigin>>` ⇒ 记录是 **`Aabb2` + 两个字节**。
**没有任何地方能放「这个洞是尖拱」这种形状参数。**

**② 窗的视觉生成器一个洞都不写。** `generate_cottage_wall_windows`（2638 行）的全部语义调用是
`add_lintels` / `flowerbed_xforms` / `DecoratorBlueprint::set_colliders` / `DecoratorBlueprints::insert` /
`WallSpace::project_clamp` / `Rectangle3d::extent` —— **`WallHoles` 一次都没出现**。洞由
`maintain_visual_entities` 那条链从 blueprint 写（同命名空间下有 `clear_out_wall_holes`）。

**③ 订正：`iter_holes_with_padding` 的 padding 不是把洞放大。** 反汇编读出来是**接缝绕回**：

```asm
xmm7 = [rcx+18h]                              ; wall_length
if (padding > hole.min.x)              -> 额外推一份 hole + wall_length
else if (hole.max.x + padding > wall_length)  -> 额外推一份 hole - wall_length
```

靠近 S=0 的洞会在 S=wall_length 处**再出现一份**，好让扫到环形墙接缝的消费方也看得见。
它还会跳过 `HoleOrigin != 0` 的洞（`cmp byte ptr [r13+11h], 0; jne skip`）⇒ 消费方按**来源**分流。

**④ cottage 过梁：一个资产 + 条件缩放。** `add_lintels` 里：

```asm
xmm11 = 0.82  或  [rdi+1Ch]
xmm7 /= 0.9                  ; 0.9 m 像是过梁的基准跨度
test  r9b, 1
jne   -> mulss xmm11, xmm7   ; 旗标置位才缩放
```

常量全是米：1.43（= rank-2 窗宽 143.5 cm）、0.9、0.82、0.64（约等于过梁进深 65 cm）、0.081。
⇒ **rank 之间靠缩放适配，不是摆多块。** ⚠️ `xmm11` 具体是哪个字段没坐实，这条标【推测】。

#### 1.2c 尖拱窗看着不像 AABB —— 但形状全在预制件上【确凿，实测尺寸】

用户提出的反例（gothic 尖拱窗）看上去洞跟着尖拱走。实测资产之后结论相反：

| 资产 | 宽 | 深 | 高 | origin.z | 占的 z 区间 |
| --- | --- | --- | --- | --- | --- |
| `window_gothic_1x1`（框体） | 69.4 | 20.7 | 203.6 | +6.5 | [−95.3, **108.3**] |
| `window_gothic_1x1_hat`（尖拱带） | **94.2** | 68.1 | 93.0 | +79.8 | [33.3, **126.3**] |
| `window_gothic_1x1_glass` | 35.3 | **0** | 160.6 | 0 | 平面片 |

**帽子比框体每侧宽 12.4 cm、顶部高出 18 cm** ⇒ 它把矩形洞的上边缘与**两个上角**完全盖住，还富余。
2x1 同型（帽子 149.2 vs 框体 117.2，每侧 16 cm）。

⇒ **洞是矩形，尖拱的形状 100% 来自预制件**（帽子由 `get_gothic_hat_bones` 的 3 个骨点掰弯贴合曲墙）。
与 cottage 同一个套路，只差多档的实现：**cottage 一个 lintel 资产 + 缩放；gothic 每档一个 hat 资产**。

⚠️ 即便退一步假设 `HoleType` 能编码「半圆拱」，**尖拱的尖锐度也塞不进 1 个字节 + 一个 AABB**。
门拱那条曲线是靠每砖的 `global_arch_height_vals` 在**着色器**里裁的 —— 那是墙分段驱动的**门**专用管线，
窗（decorator）不走它。

**对本项目**：我们的①按**剪影**裁行（`CSHouse_OpeningHalfWidthAtZ`）比 TG 的 AABB 裁排**强一档** ——
砖能直接跟着拱圈收。真要做尖拱窗，两条路都开着：要么用剪影让砖自己贴合，要么照 TG 摆一块盖顶件。

#### 1.3 窗楣／窗台：cottage 是预制件，gothic 是**整条拱带被三骨点掰弯**【确凿】

这条直接回答「窗台/窗楣是不是也和门框一样用砖块沿曲线摆」——**都不是**。

| 样式 | TG 的做法 | 证据 |
| --- | --- | --- |
| cottage 窗楣 | `cottage_wall_window::add_lintels` 摆**预制过梁件** `setdressing_window_lintel`（125×65×15 cm） | `[PDB]` + `[资产]` |
| cottage 窗台 | 预制 `setdressing_window_sill`（78.1×38×12 cm）；转角窗自带 `window_cottage_corner_1x1_sill` | `[资产]` |
| gothic 窗楣 | **一整块作者建好的拱带 `window_gothic_*_hat`**，用 3 个骨点在 VS 里掰弯贴合墙 | `[GLSL]` + `[资产]` |

gothic 那条值得展开，因为它最容易被误读成「沿曲线摆砖」：

`[GLSL] _nani_gothic_window_bricks.raster...vs_main.glsl` 的实例结构是

```glsl
struct InstanceData {
    Affine3Packed xform;  int seed;  int _wall_id;
    uint wallspace_x_range_packed;   uint _is_wooden;
    vec3 bone_0_pos; uint bone_0_normal_packed;   // 左
    vec3 bone_1_pos; uint bone_1_normal_packed;   // 中
    vec3 bone_2_pos; uint bone_2_normal_packed;   // 右
};
```

顶点流是 `Vertex_Position / Normal / UV / is_bevel / brick_id / bbx_x`。VS 拿逐顶点的
`bbx_x ∈ [0,1]`（顶点在拱带包围盒里的归一化横坐标）做二段线性蒙皮：`bbx_x < 0.5` 在
bone0→bone1 之间插值、`≥ 0.5` 在 bone1→bone2 之间插值，法线同插值后用来搭截面朝向基。

关键实测：`window_gothic_1x1_hat.json` **236 个三角形、`brick_id` 全为 0、`bbx_x` 满量程 0..1** `[资产]`
⇒ **整条拱带是一个 nani 实例，不是 N 块砖**。3 个骨点存在的唯一理由是 **TG 的墙是曲线**，
一整块直的拱带要能贴到弯墙上。

- 三个 hat 的实测尺寸（UE cm，宽×深×高）：`gothic_1x1_hat` 94.2×68.1×93.0、
  `gothic_3x1_hat` 192.0×68.1×177.3、`gothic_1x1_hat_full` 95.1×68.1×221.6 `[资产]`。
- 全部 11 个带 `bbx_x`/`brick_id` 的资产都是 `*_hat` `[资产]`。

**⇒ `flags&32`（拱压扁 + 三平面 UV + 免拱裁剪）对窗户不成立【确凿】。**
`flags` 是**墙砖专用管线**的位域（`[分析]` §1.7），而
`GothicWindowBricksInstanceData` **根本没有 flags 字段**，走的是另一套 nani subset
（`_nani_gothic_window_bricks.raster`，`[分析]` §1.4 已点名「窗框砖不走此管线」）。
把 `flags&32` 的语义外推到窗户是一次误读。

#### 1.4 本项目的裁剪场比 TG 强一档 —— ~~窗户零新机制~~【证据确凿，但**结论已被 2026-09-05 两层裁决取代**】

> ⚠️ 本节的**证据**（clip 场是二维、Rect/Circle 上下都有界）全部仍然成立，而且正是两层方案第③级
> （洞缘砖贴合）能同时贴合上下两缘的依据。**作废的只有结论**：窗户不再是「零新机制」——
> 砖层铺满与洞缘四级都是新代码。见「挖洞策略改成 TG 的真两层」一节。

`FCSOpeningClipField` `[代码] CSHouseProfile.h:224-297` 存的是**二维** `q = ((S−Cs)·invHW, (Z−RefZ)·invSZ)`，
三种形状各自封闭判据：

```text
Arch   q.y ≤ 0 ? |q.x| < 1 : dot(q,q) < 1
Rect   max(|q.x|, |q.y|) < 1
Circle dot(q,q) < 1
```

`Rect` 与 `Circle` **上下都有界** ⇒ **矩形窗与圆窗今天就能被逐像素切出来，一行 shader 都不用加**。
`Arch` 在拱脚以下无下界是**故意的**（`CSHouseProfile.h:212-215` 已写明理由：窗台那一截由
`RebuildBodyMesh` 生成的实心盒承担，判据因此只要两个 float）。

而 `RebuildBodyMesh` `[代码] CSHouseActor.cpp:562-577` 的 `AddPanel(SA, SB, Z0, Field, Tag)` **已经**
在 `Z0 > 0.5` 时另砌一块实心窗台盒：

```cpp
Writer.AddBox(Start + U*SA + Up*Z0, U*(SB-SA), In*T, Up*(H - Z0), SlotWall);   // 带 clip 的洞板
if (Z0 > 0.5f) { /* 无 clip */ Writer.AddBox(Start + U*SA, U*(SB-SA), In*T, Up*Z0, SlotWall); }  // 窗台
```

`BuildFramePlan` `[代码] CSHouseActor.cpp:1050-1058` 也**已经**有窗台砖分支：

```cpp
bool bAnySill = false;
for (const FCSOpeningProfileSample& S : Samples) bAnySill |= S.ZLow > 1.0f;
if (bAnySill) { /* 沿下边界再铺一条砖 */ EmitCurve(Path, CentreLocal, -In, Salt | 0x10000); }
```

⇒ **「窗户复用已有的 opening + per-pixel clip 设施」这条计划口径完全成立，
而且底层三处（clip 场 / 墙板 / 门框砖）全都预留好了窗的分支，一行都不用改。**

⚠️ **2026-09-05 订正**：上面这句写于「墙板 = 实心盒」的年代。两层裁决之后「墙板」这一层没有了，
窗台盒被砖取代、门框砖并入砖层 —— **`FCSOpeningClipField` 仍然一行不用改**（它照旧是唯一真源，
只是多了一个消费方：拿洞缘高度去缩放砖的局部坐标），但「一行都不用改」对**墙板与门框砖**不再成立。
缺的全部在**上层**（谁来提诉求、拿什么形状、怎么让位），见第三、第四节。

一条口径订正提给计划：D8 那一节写「D8 窗户沿用同一形态」并把逐像素 clip 描述成
「Tiny Glade 的做法……作为可选优化留在 D14」`[计划:10]`。事实是：**TG 的门拱确实用逐像素 clip，
但 TG 的窗户不用**；本项目让窗户也走 clip 是**自有改进**，比 TG 更省几何、洞缘精度更高。
措辞值得订正，做法不必改（同门拱那条「不是依据 TG」的订正）。

### 二、窗户的触发规则：**玩家手放**，不是墙面剩余空间自动填充【确凿】

这条与门那条（拱由墙自身折线分段驱动、与道路无关）对照着看很重要：**TG 的门与窗触发方式完全不同**。
门是墙的派生物；窗是**玩家显式放置的、可序列化、可撤销的实体**。

#### 2.1 创建链全部在 UI 系统里【确凿】

`[PDB]` L9344 = `ui_place_decorator`（按 `PlaceDecoratorInteractionState` 唯一匹配）：

```text
读:  RaycastWorld · CursorPositionSS · Modifiers · UiState · AppMode · Time
     PublicWalls · Query<&Roof> · TerrainHeightsData · GladeBorder · WallColorIds
     DeferredDecoratorOpEvents · CachedDecoratorTransforms · DecoratorAffordanceDispatcher
写:  DecoratorStorage · DecoratorArchivist · DecoratorIdGen · DecoratorBackup
     StairsState · StairsRemovedSupports · MoveDecoratorDeltaPositions
     DeferredDecoratorEditHistory · PlaceDecoratorInteractionState
听:  EvInitPlaceDecoratorInteractionState · InstaCreateDeco · InstaCreateStairPoint
```

配套 `[PATH]`：
`ui_systems/{ui_place_decorator, ui_move_decorator, calculate_decorator_dst,
calculate_decorator_grab_offset, decorator_interaction_intent, ui_decorator_affordance_dispatch}.rs`。

**决定性的一条**：`calculate_decorator_dst::convert_raycast_hit_to_decorator_dst` `[PDB]` ——
**放置目标由一次光标射线命中转换成 `DecoratorDst`**，而 `DecoratorDst` 是
`{ WallAttachment | RoofAttachment | StairAttachment | TerrainAttachment }` 四选一 `[PDB]`。

⇒ 与本项目 D8「射线检测命中房子 → attach → 吸附到命中墙面」**逐条同构**。

#### 2.2 拖拽链：TG 的形态与 D8 的裁决逐条对得上【确凿】

`[PDB]` L8712 = `ui_move_decorator`（按 `CacheDecoclutterRotationOnGrab` + `MoveDecoratorDeltaPositions` 匹配）：

| D8 的裁决 `[计划]` | TG 的对位物 `[PDB]` | 对得上吗 |
| --- | --- | --- |
| 拖拽期间逐 tick 解析宿主 | `CameraCursorRay` + `CursorPositionSS` + `RaycastWorld` 每帧 | ✅ |
| 房子是参数化 OBB，用解析求交不用引擎 trace | `Query<&WallTriggerVolume>` / `Query<&RoofTriggerVolume>`；`WallTriggerVolume::{recompute, recompute_quads, get_position_from_mesh_uv}` | ✅ 形态同构（TG 是每墙一个 quad 触发体） |
| 松手时被拒则弹回 `LastAcceptedWorld` | **`ResMut<backup::DecoratorBackup>`** —— 拖拽前先备份 | ✅ **同名同义** |
| 换宿主：旧宿主注销 + 新宿主登记 | `EventWriter<ReanchorDecorators>` + `move_decorators_following_anchors` `[PATH]` | ✅ |
| 越界不生成 | `GladeBorder` + `DisplayOutOfBorder` / `FeedbackInputOutOfBorder` | ✅ |
| 编辑器里要区分「已生成 / 被拒（附原因）」 | `NotifyHintSystemWindow`（onboarding 提示）+ `decorator_cursor_icon` / `show_decorator_icon` | 部分（TG 用光标图标 + 引导提示，不是线框变色） |

#### 2.3 「房子裁决」的对位物：`validate_blueprints`【推测（强）】

`[PDB]` L8815：

```text
读: DecoratorStorage · PublicWalls · TerrainHeightsData · WaterRaster · ActiveSession
写: DecoratorBlueprints
```

一个「拿墙 + 地形 + 水面复核已存储的装饰物、产出（或不产出）蓝图」的系统 ——
`[PATH]` 里正好有 `blueprint::validate_blueprints` 且 `[PDB]` 有
`validate_blueprints::closure$0::closure$0`。**这就是本项目 `QueryFeaturePlacement` 的对位物**：
存储层保留玩家的诉求，蓝图层每帧重新裁决要不要出。

配套三条【确凿】：

- `blueprint::{clear_blueprints, copy_blueprints_to_prev, iter_maybe_modified}` + `PrevDecoratorBlueprints`
  ⇒ 蓝图是**每帧重建的派生物 + 上帧差分**，与本项目「声明式重求值 + 哈希短路」同形。
- `DecoratorStorage` 带 serde `serialize/deserialize` `[PDB]`
  ⇒ **诉求持久化，派生物不持久化** —— 与本项目「标记 actor 持参数、openings 表 `Transient`」逐条同构。
- `cull_oob_decorators` / `decorator_on_{wall_height_changed, rectangle_edited, move_shape,
  freehand_wall_edited}` `[PATH]` ⇒ 墙一变就重判，墙没了就剔掉，与 D8「宿主被删 → 标记自毁」同形。

#### 2.4 唯一的「自动」成分：rank + 合并/拆分，**不是**填满剩余空间【确凿】

- `DecoratorRank` + `DecoratorType::max_rank` `[PDB]`；资产实测 rank ∈ {1,2,3}：

  | 资产 | 宽 (UE cm) | 深 | 高 | 局部 Z 范围 |
  | --- | --- | --- | --- | --- |
  | `window_cottage_1x1` | 78.0 | 17.0 | 160.0 | [−80, +80] |
  | `window_cottage_2x1` | 143.5 | 18.0 | 160.0 | [−80, +80] |
  | `window_cottage_3x1` | 213.0 | 53.8 | 170.0 | [−85, +85] |
  | `window_gothic_1x1` | 69.4 | 20.7 | 203.6 | [−95.3, +108.3] |
  | `window_gothic_2x1` | 117.2 | 20.7 | 248.1 | [−95.3, +152.8] |
  | `window_gothic_3x1` | 151.9 | 20.7 | 295.4 | [−95.3, +200.1] |
  | `window_cottage_corner_1x1` | 113.9 | 29.6 | 160.0 | [−80, +80] |
  | `arrow_slit_1x1` | 50.0 | 56.0 | 50.0 | [−25, +25] |
  | （参照）`door` | 120.0 | 75.0 | 250.0 | [−125, +125] |

  两条读法：① cottage 的**窗台高固定**（三个 rank 的 Z 都居中，rank 只加宽）；
  ② gothic 的**下沿固定在 −95.3、上沿随 rank 长高**（尖拱越宽越高）——
  这正是「rank 越大拱越高」的作者制表现，不是程序算的。
- `merge_proposal_wall_decorators::{find_best_wall_decorator_merge_position,
  propose_wall_decorator_merge_position_inner}` `[PDB]` ⇒ 拖一扇窗靠近另一扇时，
  **提议一个合并位置**（两扇 1x1 合成一扇 2x1）。
- `ui_systems::handle_splitting_wall_decorators::{display_decorator_wall_parts_and_get_hovered_part_id,
  handle_moving_out_wall_decorator_part}` `[PDB]` ⇒ 从合并体里**拖一块出来**再拆开。
- `onboarding/hint_unlink_windows.rs` `[PATH]` ⇒ 游戏专门教这个操作。

⇒ 这是**在玩家手放的基础上做吸附与合并**，不是「墙剩下多少就填多少」。

#### 2.5 全仓找不到任何「剩余空间自动填窗」的系统【确凿（否定式，含边界）】

把 `[PDB]` 里所有含 `DecoratorStorage` / `DecoratorBlueprints` 的系统签名解出来，
产出装饰物的只有四类：① UI 放置/移动；② `validate_blueprints` 复核；③ 墙变化时的重锚/剔除；
④ `add_preplaced_autoclutter`（读 `PreplacedAutoClutter`，只在 `NewSessionStartedCmd` /
`SessionLoadedCmd` 触发，是**开局预置存档**，不是运行时填充）。

**没有任何系统读墙长度或 `WallPathSegmentationMasks` 去分配窗位。**
（对比：门那条链是 `construct_gates` ← `ArchSegments` / `LintelSegments` /
`WallPathSegmentationMasksMinusStairs`，见状态文件的门洞小节 —— 窗这条链**一个 segmentation 都不读**。）

⚠️ 否定式结论的边界：PDB 只能证明「没有这样一个 **Bevy 系统**」。若 TG 把它写成
被别的系统内联调用的自由函数，符号名里也不会出现「auto place window」这类词。
要彻底确证需要反编译 `validate_blueprints` 与 `instantiate_blueprints` 的函数体。

### 三、`FCSWallOpening` 够不够表达窗户：够挖洞，不够摆框

`[代码] CSHouseProfile.h:56-120` 的现状字段：
`Type / Shape / EdgeIndex / CenterS / Width / Z0 / Z1 / AxisUS / Skew / SourceId / Tag`。

**挖洞这一半已经够了**：`ECSOpeningType::Window` 与 `ECSOpeningShape::{Arch, Rect, Circle}` 都在，
`Z0` 表达窗台、`Z1` 表达窗顶，`CSHouse_ComputeClipField` 三种形状全覆盖。

**缺的是「洞之外」的六件事**（按落地代价排序）：

| # | 缺什么 | 为什么现有字段顶不上 | 建议形态 |
| --- | --- | --- | --- |
| W1 | **裁决回执** | 计划 D8 要求把结果回写标记（`bCausesCut` + 拒绝原因），`QueryFeaturePlacement` 现在**只返回 bool** `[代码] CSHouseActor.h:330` | 改签名返回 `FCSFeaturePlacement{ bAccepted, Reason, SnappedWorld }`（计划已给结构体，只是没落地）。**纯加法，无裁决冲突** |
| W2 | **窗框资产引用** | 洞只描述空气，窗扇/玻璃/框是实体。TG 一扇窗 = 一组 mesh（主体 + `_glass` + `_collision` + `_interaction` + `_outline` + `_flowerbed_locations`）`[资产]` | 不进 `FCSWallOpening`（它是纯几何契约）；放标记 actor 上 —— ✅ **2026-09-06 裁决：标记自己持有 `UStaticMeshComponent`**，~~由房子另开实例通路~~ 作废，房子只挖洞 |
| W3 | **样式/rank 枚举** | TG 有 cottage/gothic × rank1..3 × 转角/老虎窗四个维度；本项目 `Shape` 只有三种**纯几何**原型 | `ECSOpeningShape` 加 `PointedArch`（尖拱，gothic 的洞形），样式与 rank 留在标记 actor 上不进洞 |
| ~~W4~~ | ~~**跨边转角窗**~~ **作废** —— 2026-09-05 用户裁决**不做转角窗**：窗恒锚单条边，距角过紧**直接不生成**（谓词 `NearCorner` 即最终答案，不吸附不改形不越角） | — | — |
| W5 | **`Tag` 已被门占满** | `Tag` 现在写的是门的 `Slot & 0xFF` `[代码] CSHouseActor.cpp:301`，进顶点色 G 通道做悬停高亮；窗要区分「门/窗/被拒」得抢同一个字节 | D14 的通道字典问题，先记账 |
| W6 | **`Z0` 没有下限守卫** | `QueryFeaturePlacement` 只判 `Z0 < 0` `[代码] CSHouseActor.cpp:353`；窗台压在地面上（`Z0` 极小）时几何合法但观感荒唐 | 加 `csh.WindowMinSillZ`；**纯加法** |

⚠️ **不缺**的两样，别顺手加：`AxisUS` 与 `Skew` 对普通窗恒为 `(0,1)` 与 `0`
（`CSHouseProfile.h:57-64` 的注释已写明它们是为楼梯与转角洞预留的），
窗户**不要**去用它们，否则 W4 真做时语义会打架。

### 四、`CSHouse_OpeningCell` / `SolveBlockLayout` 要怎么给窗户让位

#### 4.1 现状排布逻辑：一块面板一个 clip 场，洞按 `Cursor` 单调推进【确凿】

`RebuildBodyMesh` `[代码] CSHouseActor.cpp:576-593` 的循环：

```cpp
float Cursor = 0;
for (const FCSWallOpening& O : Openings)   // 已按 CenterS 排序
{
    CSHouse_OpeningCell(O, PierWidth, CellMin, CellMax);        // 半宽 + 半个墩
    CellMin = FMath::Clamp(CellMin, Cursor, F.Len);
    CellMax = FMath::Clamp(CellMax, CellMin, F.Len);
    if (CellMax - CellMin < O.Width) continue;                  // 装不下 → 这个洞被丢弃
    AddPanel(Cursor, CellMin, 0.0f, {}, 0);                     // 实心段
    AddPanel(CellMin, CellMax, O.Z0, ComputeClipField(O), O.Tag);
    Cursor = CellMax;
}
AddPanel(Cursor, F.Len, 0.0f, {}, 0);
```

三条硬约束由此而来：

1. **一块面板只能带一个 clip 场** ⇒ 同一段 S 区间上不能有两个洞。
2. **`Cursor` 单调** ⇒ 洞必须沿 S 无重叠且有序。
3. **`CSHouse_OpeningCell` 恒占 `HalfWidth + PierWidth/2`**，与洞的 Z 无关
   `[代码] CSHouseProfile.h:300-305`。

#### 4.2 让位的四个真问题

| # | 问题 | 症状 | 建议改法 | 触碰的裁决 |
| --- | --- | --- | --- | --- |
| P1 | ~~**高窗与低门在同一 S 上会互相吃掉面板**~~ —— **已收口（2026-08-30）** | 曾经：`QueryFeaturePlacement` 判**二维** `(S,Z)`（`CSHouse_OpeningsOverlap`），而 `RebuildBodyMesh` 的 cell 是**一维 S 区间**，第二个洞会因 `CellMax - CellMin < Width` 被 `continue` 静默丢弃 | **用户裁决 C1 选甲**：判据降成同边一维 S 区间（比的是**面板格**，`Z` 不参与），谓词与扫掠同维，那条 `continue` 对过了谓词的洞已不可达。代价是永久放弃"门上开窗" | 已按 D8「谓词是唯一真源」纪律收口 |
| P2 | **窗被门整条边挤掉** | `ComputeDoors` `[代码] :312-317` 是「门先全部落位 → 窗逐条过谓词」。一面墙被道路点亮成连拱时，`SplitEdgeIntoSlots` 会把整条边切满，窗**永远放不进去** | 这是 D6「门拱优先于特征标记」的**预期行为**，不是 bug。但用户会看到「窗放上去就消失」，W1 的拒绝原因回执因此从「锦上添花」升级成**必需品** | 无（W1 是加法） |
| P3 | **`PierWidth` 对窗过宽** | 门要留砖墩（40 cm），窗之间不需要 —— 两扇窗按 `PierWidth` 各让 20 cm，一面 4 m 的墙最多摆 3 扇 78 cm 的窗 | `CSHouse_OpeningCell` 加一个按 `Type` 分流的墩宽（门 `PierWidth`、窗 `csh.WindowPierWidth` 默认 0）。**注意它是 `inline` 头函数、被墙板与谓词两处调** —— 改签名要同步 | 无 |
| P4 | **窗的 `SourceId` 排序与门的 `Tag` 冲突** | `CurrentOpenings.Sort` 按 `(EdgeIndex, CenterS)` `[代码] :318`，`SourceId` 只在谓词里用来「自己不与自己冲突」。窗是 GUID、门是 `(边,子段)`——两扇窗 `CenterS` 相同时排序不稳定 | 排序键末位加 `SourceId`（GUID 有全序）。**这是幂等短路的正确性条件**，与楼梯对照第二节「pull 不能 push」是同一条纪律 | 无 |

#### 4.3 ~~`SolveBlockLayout` / `BuildFramePlan` **不需要为窗改一行**~~【推测，已于 2026-08-31 验证成立；**2026-09-05 两层裁决后整节作废**】

> 本节的判断在「实心盒 + 贴脸门框砖」的模型下**是对的**（`demo_house_window` 实跑确认窗的两条砖带都出）。
> 两层之后 `BuildFramePlan` 不再是「给洞缘贴一圈装饰」而是砖层的一部分，本节的问题本身消失。

`BuildFramePlan` 对每个洞发两条曲线（上边界+门樘、下边界仅当 `Z0>0`）
`[代码] CSHouseActor.cpp:1032-1058`，两条都走 `ResampleUniform` + `SolveBlockLayout` + `Scatter`。
窗只是「`Z0 > 0` 的 Arch/Rect」，两条曲线自动都成立。

**推理链**：直读 `BuildFramePlan` 的 `for (const FCSWallOpening& O : CurrentOpenings)` 循环，
它不看 `O.Type`；`bAnySill` 分支恰好就是窗台。标【推测】而非【确凿】是因为**没有跑过**：
确证方式 = 手工往 `CurrentOpenings` 塞一个 `Type=Window, Shape=Rect, Z0=90, Z1=250` 的洞，
看墙板与两条砖带是否都出。

⚠️ 一条要提前想清楚的：`Rect` 洞的上边界折线只有**两个样本**
（`CSHouse_SampleOpeningProfile` 的 `Rect` 分支只 `Emit` 两次，`CSHouseProfile.h:161-165`），
而 `EmitCurve` 要求 `Even.Num() >= 3` 且两端各外延一格 —— **矩形窗的框砖会走进
`ResampleUniform(2 点, N)` 这条从没被走过的路**。`ResampleUniform` 对两点输入是安全的
（线性插值），但 B 样条把一条直线的两端各抹掉一截、门樘顶角会被抹圆。
**矩形窗的框建议不走曲线铺砖，直接摆四条直边**（三个 quad 的事），别硬套拱的那套。

### 7.2 窗户可用资产：**基本齐全**

`/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/decorators/` 下按前缀（UE 数 / 源数）：

| 组 | UE | 源 | 缺 |
| --- | --- | --- | --- |
| `window_cottage_*` | 49 | 55 | 6 |
| `window_gothic_*` | 42 | 42 | 0 |
| `arrow_slit_*` | 18 | 18 | 0 |
| `balcony_door_*` + `gothic_balcony_door_*` | 34 | 34 | 0 |
| `setdressing_*` | 14 | 14 | 0 |
| `lantern_*` / `flag_*` / `trap_door_*` / `chimney_*` / `vent_pipe*` / `door` | 50 | 50 | 0 |
| `outline_*` | 14 | 15 | 1 |
| **合计** | **221** | **228** | **7** |

一扇 cottage 窗的完整件（全部已导入，路径前缀 `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/decorators/<n>/StaticMeshes/<n>`）：

```
window_cottage_1x1                       78 × 17 × 160 cm   96v/32t   Pos,Normal,Color,UV,tangent,bitangent
window_cottage_1x1_glass                 78 × 1.7 × 160     1056v/352t  + is_glass 流
window_cottage_1x1_collision             78 × 8.5 × 160     8v/12t    Pos
window_cottage_1x1_interaction / _collision_outline / _corner
window_cottage_1x1_dormer{,_frame,_glass} / _full_dormer{,_corner,_collision_outline}
window_cottage_1x1_halfdormer_{collision,frame}
window_cottage_1x1_glass_broken_0/1/2
setdressing_window_sill                  78.1 × 38 × 12
setdressing_window_lintel                125 × 65 × 15
```

`2x1` / `3x1` 同族齐全；`window_cottage_corner_1x1/2x1/3x1{,_collision,_glass,_interaction}` 齐全
（`corner_1x1` 另有 `_sill`）；`window_gothic_1x1/2x1/3x1` 的 14 件套（含 `_hat` / `_hat_full`）**全齐**；
`arrow_slit_1x1/1x2/1x3` 全齐。

⚠️ **重名陷阱**：`window_cottage_1x1` 在**两个路径下各有一份，且是两个不同的网格** `[资产]`：

| UE 路径 | 源 JSON | 顶点/三角 | 尺寸 | 顶点流 |
| --- | --- | --- | --- | --- |
| `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/window_cottage_1x1/…` | `meshes/window_cottage_1x1.json` | 1704 / 568 | 78 × **85** × 163.8 | Pos, Normal, **Color** |
| `/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/decorators/window_cottage_1x1/…` | `meshes/decorators/window_cottage_1x1.json` | 96 / 32 | 78 × **17** × 160 | Pos, Normal, Color, UV, tangent, bitangent |

顶层那个是**装配好的整窗**（框+洞口内壁+窗台，85 cm 深）；`decorators/` 那个是 TG 运行时用的
**17 cm 薄框板**，靠 `_glass` / `_sill` / `_lintel` 拼出整体。**选哪个要有意为之。**

### C4 D8 计划里「逐像素 clip 是 Tiny Glade 的做法」这句对窗户不成立

第 1.4 节。TG 的门拱用逐像素 clip、**窗户不用**（用 CPU 裁砖 + 预制框）。
本项目让窗户也走 clip 是**更强的自有做法**。措辞订正，做法不改。
（与状态文件里门洞那条「不是依据 TG」的订正同型 —— 建议一起改，避免第三次踩同一个坑。）

---

## 三、时间线（原 合卷卷零，按日期排）

> 四条进度日志原样来自 [`Progress_ModuleComparison.md`](Progress_ModuleComparison.md)（原 395–424、1724–1775、2405–2424、2426–2461 行）。
> 后两条由 `####` 提升为 `###`，其余一字未动。原处各留了一行带日期的存根，卷零的时间顺序没有断。

### C1 已拍板：窗户谓词降维成一维 S 区间（2026-08-30）

**用户裁决：永久放弃"门上开窗"。** 三条出路里选**甲** —— 谓词从 `(S, Z)` 二维矩形降成同边一维
S 区间，与 `CSHouse_BuildBodySoup` 的单游标扫掠同维；乙（面板垂直细分）与丙（一块面板带多个
clip 场）都不做。**D8 由此解卡。**

原矛盾（2026-08-30 发现，两边各自都对）：

- `QueryFeaturePlacement` 判的是二维 `(S, Z)` 矩形 —— 谓词**允许**高窗压在低门上方。
- 而铺墙板是**单游标 `Cursor` 沿 S 的单调扫掠**，每块面板只带**一个** `ClipField`。S 上重叠的
  第二个洞轻则被前一块无 clip 的面板咬掉半边，重则因 `CellMax - CellMin < O.Width` 被**静默丢弃**。

谓词说"能放"、几何却砌不出来，违反 D8「谓词是唯一真源」那条纪律。**降维就是让谓词说的话几何一定做得到。**

**落地（同日）**：

- `CSHouse_OpeningsOverlap`（`CSHouseProfile.h`）签名加 `PierWidth`，比的是两洞的**面板格**
  （`CSHouse_OpeningCell`：半宽 + 半个墩）按 `OpeningClearance` 膨胀后是否相交，`Z` 不再进判据。
- **比格不比洞**是关键：格才是扫掠真正消费的那个区间 —— 两格互不相交 ⇒ 游标永远顶不到洞，
  那条 `continue` 对"过了谓词的洞"变成不可达路径。墩宽也因此只有 `CSHouse_OpeningCell` 一处真源，
  将来做窗户对照 A3（按洞型分流墩宽）时谓词自动跟随。
- `CSHouse_OpeningBounds`（二维包围盒）随之删除，无其它调用方。
- 单测 `House.OpeningOverlap` 反转：原来断言"同 S 高窗放行"，现在断言**必冲突**；另加两条 ——
  "把窗抬到 1000 cm 仍冲突"（防二维判据被悄悄加回来）、"两洞让开 20 cm 但两格相交 ⇒ 仍冲突"。
- `Z0/Z1` **保留**：窗台高、窗顶高照旧表达，`FCSOpeningClipField` 仍是二维逐像素判据。
  放弃的只是**同边 S 重叠的洞堆叠**这一种摆法。

**代价（明确接受）**：门上方开窗、以及任何同边 S 上重叠的洞，永久不支持。TG 里这种堆叠也确实罕见。
窗户与装饰对照的 **A7（面板垂直细分）随之作废**；**A6（`ACSHouseFeatureMarker` + `ACSWindowMarker`）
不再有前置裁决**。

### D8 收口：`ACSWindowMarker` 已落地（2026-08-31）—— 对照文档 A6

D8 剩下的最后一件事。谓词（`CSHouse_QueryOpening`）、clip 场、窗台盒、窗台砖早在 08-31 上午就
全在了，缺的只是**交互 actor** 这一半：把"我在世界的哪里"翻成"这面墙上的哪一扇窗"。

#### 落地了什么

| 件 | 位置 | 说明 |
| --- | --- | --- |
| 射线 / 就近打墙 | `CSHouseProfile.h` 的 `CSHouse_RayHitWall` / `CSHouse_NearestWall` | **纯函数**，无 world 依赖。`S`/`Z` 直接就是 `CenterS`/窗台高的同一口径 |
| 找宿主 | `UCSHouseLibrary::PickHouse` / `PickHouseNear`（09-16 起，原在 `UCSHouseSubsystem`） | 遍历 world 里全部房子（`TActorRange`，GUID 升序）取最近，世界→局部那一步归房子（见下） |
| 房子侧入口 | `ACSHouseActor::RayHitWall` / `NearestWall` | 只做一件别处做不了的事：用 **`GetBuildTransform`**（烘常驻流的那个只取 yaw 的变换）解世界坐标 |
| 登记表 | `ACSHouseActor::MarkerWindows` + `Register/UnregisterFeatureMarker` | **transient，故意不序列化**；`BuildWindowOpenings` 把它与属性面板那份 `Windows` 一起喂给同一条谓词 |
| 标记 actor | `CSHouseFeatureMarker.{h,cpp}`：`ACSHouseFeatureMarker`（抽象基类）+ `ACSWindowMarker` | 零可视几何、零材质，`MakeDemand` 是子类唯一要实现的东西 |

身份口径：属性面板那份的 `SourceId` 从**槽位**派生（没有别的稳定身份），标记那份直接用
**标记自己的 GUID**（计划 D8 明写）。用槽位的话，删掉中间一个标记会把后面每一扇窗的身份平移
一格 ⇒ 谓词的"自己不与自己冲突"错位、全序翻转，而且不会有任何断言报红。

#### 🐛 两个坑，都是**编辑器 world 特有**、都被断言抓住

两个都属于同一类：**PIE 里一切正常，编辑器里静默失效**，而本项目的创作全在编辑器 world 里。

| # | 症状 | 真因 | 修法 |
| --- | --- | --- | --- |
| 1 | 在编辑器里删掉窗标记，**墙上的洞还在** | 编辑器 world 没有 begun play ⇒ `World->DestroyActor()` 只发 `Destroyed()`，`EndPlay` 一次都不来。注销只挂在 `EndPlay` 上就漏了这条唯一实际走到的路 | `Destroyed()` 与 `EndPlay()` **两处都挂**（`DetachFromHost` 本身幂等） |
| 2 | 脚本 spawn 出来的标记：`root_component` 是 `None`、`get_actor_location()` 恒为零，日志里只有一行 `RegisterComponentWithWorld: ... IsValid() == false. Aborting.` | `GEditor->AddActor`（拖入视口 / `spawn_actor_from_class` / Python 都走它）在 spawn 之后**立刻**补一次 `PostEditMove(bFinished=true)`。当成"用户松手"⇒ 标记在原点判无宿主 ⇒ `Destroy()` ⇒ 组件变 garbage，而 actor 引用还在 | 加 `bPlaced` 位：spawn 之后的第一次 `bFinished=true` 降级成非最终裁决。**这正是计划 D8 早写过的"拖入生成的 actor 处于拖拽态"** |

⚠️ 坑 2 的诊断值得单独记：`spawned=True` 而 `root_component=None` 这个组合几乎只可能是
"actor 在 spawn 流程中途被自己 `Destroy()` 了"。CDO 上的 root 是好的（探针验过），
所以问题必然在实例的生命周期而不是构造。

#### 断言

- 单测 `House.WallPick`（纯函数）：命中边号 / S / Z / 距离逐项对；**背面射线不命中**
  （否则在屋里挥鼠标会把窗贴到对面墙上）；墙顶以上不命中；够不着不命中；就近版的夹取与半径。
- 单测 `House.WindowMarker`（要 world）：解析宿主 → 登记 → 出洞 → **推到墙角被拒但仍留在登记表里**
  （计划 D8：被拒的诉求留着，撤登记的话"从被占住的墙拖到隔壁墙"会先把它删掉再也回不来）
  → 换宿主（旧的放手、新的接手）→ 删除即注销 → 远离一切房子时确实自毁。
- 回归 `demo_house_window` 末尾新增一段：**真演示房 + 真道路**下，标记那份诉求与属性面板那份
  `Windows` 并存（`windows=2 (list alone: 1)`），以及编辑器 world 里删标记洞合拢 —— 坑 1 在这一层
  再钉一道。⚠️ 基线必须在 spawn **之前**取：标记在 `PostRegisterAllComponents` 里就登记了一次。

#### 未做（如实列出）

- **拖拽期的实时通知没有无头覆盖**。`Tick` + `PostEditMove` 那条路只能靠手动在编辑器里验；
  单测与回归走的都是 `ResolveHostAndRegister()` 这个**唯一执行面**（编辑器事件只是它的触发器）。
- **`csh.*` 那几个 CVar 没做**：`HostProbeDistance` / `SnapDistance` / `DragIdleSeconds`
  落成了逐 actor 的 UPROPERTY 而不是 console 变量。计划写的是 CVar，但逐 actor 可调更符合
  本项目其余部分的口径，未擅自改计划正文。
- **编辑器里的线框回执没做**：`bCausesCut` / `LastReject` 已经算出来并暴露给蓝图/脚本了，
  但"用线框颜色区分已生成 / 被拒"那一步需要 `UActorComponentVisualizer`，不在本轮范围。

### 窗退出框砖产线，门留着（2026-09-06）

窗的洞缘改由附属物自带的预制框盖 ⇒ `BuildEdgeElements` 加一句 `Type != Window`。
**门是反方向**：门由房子自己生成、没有 marker actor、没有任何附属物替它盖洞缘，
**那一圈砖就是门框**，退了就露生断口。

**这条不对称是 TG 的**【确凿，资产表】，资产清单见上文 [「窗不出框砖，门照旧出」](#窗不出框砖门照旧出2026-09-06-用户裁决的直接后果)。

| 判据 | 位置 | 实测 |
| --- | --- | --- |
| 窗零块、门照铺 | `House.FrameSkipsWindows`（原 `FrameWindowSill`；同一个洞只换 `Type` 送两遍） | 绿 |
| 扫描一次验两头 | `House.WindowPredicateMatchesGeometry`（`WindowBricks==0` 且 `DoorPaths>0`） | 绿 |
| 开关窗砖数不变 | `demo_house_window` | `bricks=130 (was 130)` |
| 门那边没被误伤 | 同一次回归 | `open arches grow frame bricks bricks=152`、`seam=24` |

**验收**：House 单测 **58 / 0**；回归 **224 PASS / 7 FAIL**，与本轮改动前**逐条相同**
（2 条门/拱既存红 + 5 条 mound/skirt/vine 属并行线）。

### 三件网格 + 蓝图分层已落地（2026-09-06 用户裁决）

**裁决**：附属物的可见几何拆成三个**具名**组件，**只有 `OpeningMesh` 决定洞**；另加一个总蓝图调参、
子蓝图换网格的层级。拆分的**唯一目的**是标出"谁定洞" —— 可点击性与组件个数无关（actor 层面的事），
所以 `LintelMesh` / `SillMesh` 不承担任何机制。逐条口径见计划 D8「三个具名网格组件」与「蓝图分层」。

| 件 | 位置 |
| --- | --- |
| `OpeningMesh` / `LintelMesh` / `SillMesh` | `CSHouseFeatureMarker.{h,cpp}`；`GetDemandSize` **只读第一件** |
| `PickSprite`（`bIsEditorOnly` 的 `UBillboardComponent`） | 被拒时三件网格全藏 ⇒ 视口点不中；它始终可见兜住这一条 |
| `SetMeshPiecesVisible` | **按类型**藏，不走根组件传播（传播会把 `PickSprite` 一起藏掉） |
| `UCLASS(Blueprintable)` | 两个基类都是 `NotBlueprintable` 且**会继承** —— 不显式打开就建不出蓝图，且不报错 |
| `BP_TinyGladeWindow` + `BP_Window_Cottage_1x1` + `BP_Window_Gothic_1x1` | `/PCGPlugins/HouseTest/` |

**实测**（探针 `probe_window_bps.py`）：cottage 洞 78 × 160、gothic 洞 **69.4 × 203.6**，两者
`cut=True`、`WallStandoff` 继承自总蓝图。**gothic 的帽子 94.2 宽却没有污染洞** —— 正是那条
"搞错了不报错"的纪律要守的东西。

⚠️ **朝向订正**：三件都改成 **+90° yaw**（原来是 −90°，把资产正面转进了墙里）。判据是量出来的：
`setdressing_window_sill` 的 origin `(0, +28, −76)` 整块压在 +Y 一侧 ⇒ **资产 +Y 朝外**。
`GetDemandSize` 量的是 |Y|/|Z| 跨度，两个符号都给 78×160 ⇒ **洞不受影响，只有朝向变**，
所以单独一块对称的 17 cm 板一直看不出来。

**新增断言（锁住“搞错了不报错”那条纪律）**：`House.WindowMarker` 里加了一段 —— 给 `LintelMesh` /
`SillMesh` 挂上**故意超大**的假件（引擎 100³ Cube）之后，`GetDemandSize` 与房子的窗洞数**必须逐位不变**；
再清空 `OpeningMesh` 验"组件恒在、网格可空"退回手填宽高。这条以前完全没有自动化守着 ——
而它恰恰是画面上看不出来的那一类。三个网格组件因此从 `protected` 挪到 `public`
（它们本来就是 `BlueprintReadOnly` 对外公开的，藏 C++ 侧没有意义）。

⚠️ **验收现状（如实）**：本轮我这边 7 条单测全绿
（`BrickWall` / `WallAnchor` / `WindowMarker` / `WallPick` / `OpeningOverlap` / `FrameWindowSill` /
`WindowPredicateMatchesGeometry`）。但同一次跑出现了**不属于本轮**的红：单测
`GpuInstancedMesh.Streams`（aux stream 7→8）、`Vine.StrandMatchesRecords`、`Vine.TubePath`，
回归也从 3 红涨到 7（新增的全是 mound / skirt / vine 计数）。工作区里 `CSGpuInstancedMeshVertexFactory`
/ `CSGpuInstancedMeshSceneProxy` / `CSHouseVine` / `CSGroundRockShell` 都有**并行的未提交改动**，
那批红属于那条线，本轮没有去动它。

---

### 🐛 视口拖放不出窗 —— 纪律 ③ 从「降级」误改成「跳过」（2026-09-06 当日引入、当日修复）

**症状**：把窗蓝图拖到房子上，一扇窗都不出。

**成因**：视口拖放走的是 `GEditor->AddActor` —— 与 `spawn_actor_from_class` **同一条路**。它 spawn 完
只补一次 `PostEditMove(bFinished=true)`，**之后不再来任何事件**（悬停期那个是另一个会被销毁的预览
actor，落点就是终点）。当天早些时候为修「标记咬上 11 m 外另一栋房」，把基类纪律 ③ 从**降级**改成了
**跳过**那一次事件 ⇒ 标记永远不解析。

无头复现（`probe_drop.py`，spawn 完**什么都不调**）：

```
CSWindowMarker (C++)     spawn 后立刻:   windows=0 markers=0 host=None
CSWindowMarker (C++)     显式 resolve 后: windows=1 markers=1 host=House_Road
BP_Window_Cottage_1x1    spawn 后立刻:   windows=0 markers=0 host=None
```

逻辑一直是好的，**只是没人触发它**。

**改法要两半，缺一不可** —— 这是排查过程里最值得记的一点：先只改了前一半，探针立刻打脸。

**前一半：改回降级。** `HandleDrag(bSynthetic ? false : bFinished)`。安全性不靠「跳过」，靠的是非最终
裁决这一路**不自毁**（纪律 ④）、**也不回写变换**（`SnapToAnchor` 只在 `bFinal` 分支）。

只改这一半的实测 —— **解析了，但咬错了房**：

```
CSWindowMarker (C++)     spawn 后立刻: windows=0 markers=0 host=House_Pillar cut=False reject=SILL_TOO_LOW
```

**后一半：放置那一次不打射线，只吃位置。** 因为这一刻**朝向确实还没应用** —— 但成因跟我原先写的
不是一回事，得分清两条路：

| 路径 | 旋转什么时候应用 | 结论 |
| --- | --- | --- |
| `UEditorEngine::AddActor`（`EditorEngine.cpp`） | `World->SpawnActor(Class, &Location, &Rotation, SpawnInfo)` —— **一起传进去** | 到 `PostEditMove` 时朝向已对 |
| `EditorActorSubsystem` 的 `SpawnActor`（Python `spawn_actor_from_class` 走这条） | **先放置**、回调之后才 `SetActorLocationAndRotation(Location, Rotation, ...)` | 回调那一刻 forward 还是默认 **+X** |

所以「`AddActor` 先落位后应用旋转」这句**对 `AddActor` 本身是错的、对 Python 那条路是对的**，我当初
把两条混成了一条。而 `PostRegisterAllComponents` 是第三个成因（它在 `SpawnActor` **内部**就跑完了），
由 `RF_WasLoaded` 闸门单独挡着 —— **三件事，三道闸**。

既然朝向在某些路径上不可信，放置那一次就**别信它**：落点**位置**在那一刻已经是最终值，直接走就近墙面
那一路（`PickHouseNear`，只吃位置）。判据挂在基类的 `IsPlacementResolve()` 上，用 `TGuardValue` 括住
那一次 `HandleDrag` —— ⚠️ 它是**调用期上下文**，不是「审批过了」那种常驻状态位：出了这次调用就回到
false，没有任何东西会读到陈旧值。

修完实测：

```
CSWindowMarker (C++)     spawn 后立刻: windows=1 markers=1 host=House_Road cut=True
BP_Window_Cottage_1x1    spawn 后立刻: windows=1 markers=1 host=House_Road cut=True
```

⚠️ **为什么整套回归当时全绿**：标记那一段紧跟着显式调了 `resolve_host_and_register()`，把 bug
完全遮住了。已补两条**什么都不调**的断言（`dropping a marker into the level cuts its window with no
further calls` / `...and deleting it closes that hole again`），脚本里写着「**别顺手往中间加任何一句**」
—— 那正是它们唯一的价值。

### 砖层扛洞缘：端到端覆盖与预算上界（2026-09-06）

窗退掉框砖之后，洞缘归**砖层**（`bBrickWallEnabled`）或归标记自带的 `OpeningMesh`。而
`bBrickWallEnabled` 在此之前**全仓没有任何一条断言打开过** —— `CSHouseBrickWall.h` 的纯函数有单测，
actor 那条路（`BuildBrickWallBricks`：容量、逐层哈希、`CurrentOpenings` 喂进去裁砖）是盲区。

| 判据 | 实测 |
| --- | --- |
| 砖层真的铺出层与砖 | `courses=18 bricks=1332 budget=1332` |
| 洞真的裁砖（洞缘①） | 三扇窗 `removed 77`（面积当量上界 160） |
| 逐位幂等 | 两次都是 `bricks=1255 courses=18` |
| 总数在硬上限内 | `total=1462 < 65536` |
| 关回去归零 | `bricks=0` |

⚠️ **这个"上界"只剩 7 块余量**。`EstimateBricks` 是 `ceil(周长 / 砖长) × 层数`，除的是**砖长**而不是
砖距（`Length + Gap`），而 `FrameBrickGap` 默认 0 ⇒ 两者相等，**一点余量都没有**（实测 budget 1332 =
实际 1332，逐块相等）。洞把一条整路裂成两段之后两段各取整一次，理论上能比原来还多 —— 只要洞比半个
砖距还窄，省下的长度补不回那次取整。`House.BrickWall` 因此加了一轮扫描（8 档洞宽 × {1,3} 个洞）：

```
budget sweep: worst 1103 bricks (1 hole of 10 cm) vs budget 1110, no-hole 1110
```

最坏正是预测的那一档（10 cm 洞 < 26 cm 砖距）。**今天不溢出，但溢出的后果不是报错**，是
`Params.MaxBricks` 当场静默截断：砖层铺一半，而砖数、三角数、零阻塞每一条断言照绿。

### 可画性判据不许挂在**门**的砖上（2026-09-06）

`GetWindowUndrawableReason()` 里那半段还在要求**排出过框砖**。窗已经一块都不出了，所以那道判据现在
量的是**门**，两个方向都坏、且都不报错：

- 只有窗、没有门的房子 → 误判成「不可画」；
- 演示关卡里门恰好在场 → **因为错的理由通过**，比不查还坏。

改成：窗不再以「框砖存在」为前提；砖组件只要**还有人在用**（门框 / 接缝 / 角石 / 包边 / 砖层）就照查
健康（材质没勾 `bUsedWithInstancedStaticMeshes` 那类静默坑一条不减）；砖层开着却零砖才算失败。钉它的
断言故意把门 / 角石 / 包边全关掉，让"框砖计数为零"真的发生：

```
a house with windows but not one single brick still draws its windows   bricks=0 windows=3 why=<ok>
...and that case is real, not vacuous (the brick component really is empty)   bricks=0
```

### 拖动期通知补上无头覆盖（2026-09-06）

原有那条 12 帧断言拖的是**属性面板那份窗**，它没有 actor、没有 transform 要写。标记完全不同：
footprint 一变墙面就挪，而标记 attach 在房子**根**上 —— 根没动，所以它不会自己跟过去。每帧把它拉
回去的是 `NotifyMarkersRebuilt()` 里那句 `SnapToAnchor()`，也就是「锚点是权威、变换是派生量」的字面
执行。

```
flushes=0 · worst drift=0.000 cm over 12 frames · hops=0
```

三件在单测里够不着（单测只推一次边）：每帧写 transform **不产生阻塞刷新**；标记**逐帧**贴在锚点派生
位上，而不是「拖完了才对得上」（中途分家在画面上就是洞与窗框错开、松手瞬间又跳回去）；拖动不把窗甩
到别的边（`EdgeIndex` 恒定）。

---

### 点击加窗：笔刷模式落地（2026-09-06 用户裁决）

**裁决**：「在 UE 编辑模式下实现有点难，我决定使用笔刷模式，点击后创建窗户，然后立即退出笔刷模式，
这样可以不用再管射线之类的繁琐内容了，直接是几何体记录坐标。」并追加两条：**笔刷是唯一入口**
（拖进视口那条删掉）、**创建之后保留 gizmo 拖动**调位置。

#### 为什么拖放那条路从原理上就不稳

它要靠 actor **自己的 forward** 去解析宿主，而**朝向什么时候被应用，在两条 spawn 路径上不一样**：

| 路径 | 旋转什么时候应用 | 回调里的 forward |
| --- | --- | --- |
| `UEditorEngine::AddActor` | `World->SpawnActor(Class, &Location, &Rotation, SpawnInfo)` —— 一起传进去 | 已经对了 |
| `EditorActorSubsystem` 的 spawn（Python `spawn_actor_from_class` 走这条） | **先放置**、回调之后才 `SetActorLocationAndRotation` | 还是默认 **+X** |

第二种下实测**咬上 11 m 外的另一栋房**（`House_Pillar`）并判 `SillTooLow` —— 画面上就是"窗拖上去了但没有洞"。

视口点击给的是**相机射线 + 精确命中点**，与 actor 自身朝向无关，所以这条路从根上没有那个问题。
标记出生就带着 `Host + Anchor`，正是用户说的"直接是几何体记录坐标"。

#### 落地

| 件 | 位置 | 角色 |
| --- | --- | --- |
| `UCSHouseLibrary::PlaceMarkerAlongRay`（09-16 起，原在 `UCSHouseSubsystem`） | `CSHouseLibrary.{h,cpp}` | **唯一执行面**。吃一条世界射线，命中墙就生成标记并直接交给它宿主与锚点；**打空什么都不生成** |
| `ACSHouseFeatureMarker::AdoptAnchor` | `CSHouseFeatureMarker.{h,cpp}` | 直接认下宿主与锚点，一条射线都不打；无条件吸附（点击就是放置，没有"拖到一半"） |
| `ACSHouseActor::StartWindowBrush()` + `OnWindowBrushRequest` + `WindowBrushClass` | `CSHouseActor.{h,cpp}` | `CallInEditor` 按钮 → 广播 → 编辑器模块应答（接线抄地面的 `StartVertexColorPaint`） |
| `FCSWindowBrushEdMode` | `PCGEditorProcess/Private/CSWindowBrushEdMode.{h,cpp}` | `FCSBrushEdModeBase` 的第四个叶子 |
| ~~`UCSHouseSubsystem::GetHouseSubsystem(WorldContext)`~~（09-16 随 subsystem 删除：`PlaceMarkerAlongRay` 现在是静态 `BlueprintCallable`，Python 直接 `unreal.CSHouseLibrary.place_marker_along_ray(house, ...)`，WorldContext 参数显式传） | — | 历史理由：⚠️ **脚本侧够不着 world subsystem**（UE Python 没有 `SubsystemBlueprintLibrary`），没有这个口，点击加窗就只能靠鼠标点、等于没有自动化判据 |

**EdMode 只是触发器**（与抓手族纪律 ⑤ 同型）：无头测试直接调 `PlaceMarkerAlongRay`，不需要视口、不需要 Slate。
⚠️ 但「只是触发器」**不等于「不用测」** —— 触发器自己也有三条错了不报红的接线（解析求交 / 外法线朝向 /
不做圆盘散布）。2026-09-06 当日晚些补上了 EdMode 那一层的覆盖，见下面「[收尾：把 EdMode 那一层也钉住](#收尾把-edmode-那一层也钉住2026-09-06-当日晚些)」。

叶子里换掉的只有三件，其余（stroke 生命周期、笔刷球、Esc 取消）全归基类：

- **光标求交走解析式**：房子的 gpumesh 全线 `NoCollision`，引擎 trace 一栋都打不到 ⇒ `TraceCandidatePoint` 覆盖成 `PickHouse`（与地面笔刷同型）。世界点与外法线走**公开的** `AnchorToWorld`（它返回的变换 X 轴就是内法线）—— 不自己拼 `GetBuildTransform()`，那是 private，而且"构建空间只取 yaw"这条口径必须只有一处实现。
- **不做圆盘散布**：`SamplePendingPoints` 空实现（基类头文件明写支持）。一次点击 = 一扇窗，落在光标命中点上。
- **落笔即退出**：`bExitAfterCommit = true` —— ⚠️ 这个字段**基类本来就有**，不用新加。

⚠️ `CommitSamples` 里用**命中点反推的短射线**（`命中点 + 外法线×50`，方向 `−外法线`），不复用相机射线：
这样放置由**命中**决定，掠射角点击也落在同一处。

#### 拖放那条路退役掉了什么

- `ACSHouseFeatureMarker` 加 `UCLASS(..., NotPlaceable)`。⚠️ 这个说明符**会被继承** ⇒ 子蓝图一并不可放置（正是想要的）；而 `ACSWindowMarker` 上的 `UCLASS(Blueprintable)` **必须保留**（`NotBlueprintable` 同样会继承，不显式打开就建不出蓝图且不报错）。
- 基类的 `IsPlacementResolve()` / `bPlacementResolve` / `TGuardValue` 全部删除 —— 能走到那次合成事件的子类已经一个都没有了。
- ⚠️ **纪律 ③ 的「降级」保留**：它挡的是另一半（把合成事件当松手会让抓手自毁）。别再改回「跳过」。
- `OnHandleDrag` 的射线恢复**无条件**打。它现在只服务 **gizmo 拖动** —— 那时 actor 早已构造完整，朝向可信。

#### 验收（实测）

| 断言 | 结果 |
| --- | --- |
| `one brush click on a wall cuts exactly one window` | `windows=1 markers=1 host=House_Road cut=True` |
| `a click that misses every wall creates nothing at all` | `returned=None counts (1,1) -> (1,1)` |
| `swapping the brush class swaps the hole it cuts` | `gothic=(69.4 x 203.6) cottage=(78.0 x 160.0)` |
| `deleting a brush-placed marker closes its hole again` | `windows=0 markers=0` |

House 单测 **58 / 0**；回归 **241 PASS / 8 FAIL**（8 条红：2 条门/拱既存基线 + 6 条 mound/skirt/vine 属并行工作流）。

⚠️ **还没人工验过的**：视口里真正用鼠标点的那一下。无头这边测的是执行面与触发时序，不是鼠标。

#### 收尾：把 EdMode 那一层也钉住（2026-09-06 当日晚些）

主体验收完之后剩下的口子很具体：**测的全是执行面 `PlaceMarkerAlongRay`，EdMode 那一层一行断言都没有**。
补了四个用例。

| 用例 | 钉住了什么 |
| --- | --- |
| `PCGPlugins.ComputeShaderGenerator.House.WindowBrushPlacement` | **`AdoptAnchor` 的三份坐标互相印证**：标记身上的 `Anchor`、房子登记表里那个洞的 `CenterS`、标记的世界位置 —— 三者任意一对写岔，画面上都是"窗贴在离你瞄的地方几十厘米开外"而窗数/砖数/三角数全绿。外加幂等（同一锚点收两次不双份）、`PlaceMarkerAlongRay` 的退路可 spawn、抽象类被挡、打空零生成、洞底 = 命中 Z − 半窗高 |
| `PCGPlugins.PCGEditorProcess.House.WindowBrushEdMode` | **叶子契约**：`GetBrushSettings`（`bExitAfterCommit` / 单点采样 / 不重复做背面剔除）、`TraceCandidatePoint`（命中点落在墙的**外表面**、法线**与射线相向**即外法线且单位长、打空 false、没目标 false）、`SamplePendingPoints` 确是空实现（连调三次仍只出一扇窗）、`CommitSamples`（落在瞄的弧长与那面墙上）、`ClearBrushTarget`、**花名册全扫**（目标是 A、瞄 B 就落在 B 上，目标房一扇不出） |
| `PCGPlugins.PCGEditorProcess.House.WindowBrushModeActivation` | **按钮 → 模块 → 模式管理器**这条真链：注册进 `FEditorModeRegistry`、模块订阅了 `OnWindowBrushRequest`、`StartWindowBrush()` 真的把模式拉起来**并交了目标**、Esc 退得掉、换一栋房再按一次能重进并换目标 |
| `PCGPlugins.PCGEditorProcess.House.WindowBrushBlueprints` | `ACSWindowMarker` 仍**可派生蓝图**（`FKismetEditorUtilities::CanCreateBlueprintOfClass`，反例：抽象基类不行，所以不是空判据）；`BP_Window_Cottage_1x1` / `BP_Window_Gothic_1x1` load 得动、父类链对、生成类**不抽象**、`NotPlaceable` 确实继承下来了 |

**目标是否交到手上**这条在真实例上够不着 `GetBrushTargetActor()`（protected），用的是**间接判据**：
`FCSBrushEdModeBase::InputKey` 的第一行就是 `if (!GetBrushTargetActor()) return false;`
⇒ Esc 的**返回值**就是"目标设上了没有"。没目标时它返回 `false`（反例也钉了）。

⚠️ **做不到的那一条，如实记下来。** 无头下**无法把"落笔"从鼠标事件驱动一遍**：
`FCSBrushEdModeBase::InputKey` 的 **LMB-按下**分支无条件解引用 `Viewport->KeyState(...)` 与
`ViewportClient->GetCurrentWidgetAxis()`，而无头造不出真的 `FEditorViewportClient`（它要一个 Slate
视口 widget），传 nullptr 会当场崩并把整套测试一起带走；`BeginStroke` / `CommitStroke` 又都是基类 private。
于是 `bExitAfterCommit` 拆成两半分别钉：叶子确实**要求**了退出（settings 断言），模式确实**退得掉**
（Esc 走的正是同一个 `ExitTemporaryMode()`）。**没有覆盖到的是基类里那一行
`if (bExitAfterCommit) ExitTemporaryMode();` 的字面连接** —— 这里不拿一条假装测了的断言把它盖住。

#### 空 `WindowBrushClass` 的退路：`ACSWindowMarker` **不是**抽象类

疑问是「退路是 `ACSWindowMarker::StaticClass()`，可父类挂着 `Abstract`，`SpawnActor` 会不会静静失败」。
**答案：不会，退路是好的。** `CLASS_Abstract` **不在 `CLASS_Inherit` 里**（`ObjectMacros.h`），
所以 `Abstract` **不**沿继承链传染；而同一行上的 `NotPlaceable` **在** `CLASS_Inherit` 里，所以它传染 ——
两条合起来正好解释了 `UCLASS(Abstract, NotBlueprintable, NotPlaceable)` 为什么给出想要的组合
（子类能实例化、不能拖进视口）。`House.WindowBrushPlacement` 把这三件事逐条钉死，别再凭"父类是
`Abstract`"去改退路。

即便如此仍加了一道闸（`PlaceMarkerAlongRay` 开头）：`WindowBrushClass` 是个 `TSubclassOf`，
用户或脚本完全可以把它指到一个抽象**蓝图**上，而 `SpawnActor` 对抽象类只会静静返回 nullptr ——
症状就是"点了一下什么都没发生"。现在挡在最前面并打
`UE_LOG(LogTinyGladeHouseLibrary, Warning, ...)`，日志里直说是**哪个字段**填错了、该指向什么。
单测用 `AddExpectedErrorPlain` 要求这句警告**恰好出现一次** —— 删掉它或改了措辞都会报红。

⚠️ 顺手抓到并修掉一个**与本任务无关的既存缺陷**：`CSHouseLogicTests.cpp` 用了
`LoadObject<UStaticMesh>` 却没 include `Engine/StaticMesh.h`。全量 unity 构建一路绿灯，
UBT `-SingleFile` 一过就是 `error C2027: 使用了未定义类型"UStaticMesh"`。本仓库的老毛病（见 MEMORY）：
**凡碰过的 TU 都该单独过一遍 `-SingleFile`**，否则 Live Coding 会在别人手上炸。

### 窗贴墙脚变门（2026-09-16，TG `snap_balcony_door`）

用户："在 TG 中窗子在接近底边时会变成门"。TG 侧的逐条证据与常量见 [附录 E](RE_WindowToDoor.md)，
门前踏步的砌法见 [附录 D §6](RE_PlayerStairs.md)。

**门是窗的派生形态，不是另一个类型。** TG 存档里没有「门」：`CottageWindow` / `GothicWindow` 的锚点上
`is_bottom_door` 被拖拽交互写上之后，每帧派生的子类型从 `*WallWindow` 变成 `*BalconyDoor`，rank 原样保留。
本项目照抄这个形态 —— 同一个 `ACSWindowMarker`、同一个 `MarkerId`，形态存 `FCSWallAnchor::bDoorForm`
（撤销、拉尺寸时的重新表达、松手回退全都自动带着它走），诉求与洞带 `bDoorForm`，`Type` 仍是 `Window`。

| 项 | TG（确凿） | 本项目 | 为什么不同 |
| --- | --- | --- | --- |
| 判据 | 窗碰撞盒底边低于墙脚，阈值 0 | `CSHouse_ResolveDoorForm`，阈值 `max(DoorSnapHeight = 0, WindowMinSillZ)` | 本项目把窗台低于 `WindowMinSillZ` 的窗拒掉；不取 max，拖下去窗先消失、再往下才变门 |
| 迟滞 | 鼠标模式没有：进出门同一条判据 | 门形态按**门**的半高判 | TG 的门心在门高/2，按窗判立刻回窗；本项目改一下属性就重新解析，照抄就是一碰参数门变回窗 |
| 洞底 | 墙脚 + 5 cm | 墙脚（`SillZ = 0`） | 与道路门同口径；5 cm 窄缝下的砖会被整块删掉 |
| 洞宽 | 碰撞网格包围盒（rank 1 = 114.8） | 渲染网格宽 − 2 × `DoorHoleInset`（2.6） | 没导入 `_collision` 网格 |
| 洞高 | 碰撞盒高；门顶 ≥ 墙顶即剔除 | 门矮于墙时洞顶夹到 `WallHeight − LintelBand`；门高于墙照旧 `AboveEave` | 默认房 300 − 40 = 260 < 262.5，不夹一扇门都放不下 |
| 件 | 门扇（含框）、gothic 帽；不出过梁 / 窗台 / 花槽 | `DoorMesh` / `DoorHatMesh`；窗那一组（含子蓝图里没打 `DoorForm` 标签的件）门形态下全藏 | — |
| 挂件 | `hash(seed) & 3`：门铃 / 花环各 25% | `DoorBellMesh` / `DoorKransMesh`，按 `MarkerId` 的哈希选 | — |
| 门前踏步 | 门槛贴地 ≤ 20 cm 且门外 106 cm 下沉 10–150 cm | `CSStairs::BuildDoorSteps` → `DoorStepBricks` | — |
| 栏杆 | 墙脚高出地面 15 cm 且没出踏步 | `CSStairs::ShouldAddDoorRails` → `DoorRailsMesh` | — |

⚠️ 道路门的两样配套件**不给**门形态，且这是按 `Type` 分流自动得到的：门框砖（`CSHouseFrame` 的 `Type != Window`）、
门扇（`RebuildDoorLeaves` 的 `Type == Door`）。附属物自带门框与门扇，房子再补一份就是双份几何。
门侧摆件同理走附属物自己的门铃 / 花环，**不**进道路门那一家（`add_autoclutter_around_gates`）。

验证：

- `House.DoorFormRule`：判据的两个不动点、进出门的迟滞、谓词只放过窗台下限（地面以下与过梁带照拒）。
- `House.WindowBecomesDoor`：拖下去变门（洞底 0、洞宽、洞高夹子、吸附到门心、网格件换组）→ 原地重解析不翻 →
  墙矮过门被拒、恢复后回来 → 抬上去变回窗 → 笔刷点在墙脚直接落成门。
- `Stairs.DoorSteps`：门前踏步的判据三关、金字塔砌法（层 / 排 / 进深 / 横向铺满）与栏杆判据。

**没做**：

- gothic 那一档没配门形态：`BP_Window_Gothic_1x1` 没换 `DoorMesh`，目前变出来的是 cottage 门；TG 的 gothic 门洞是
  按 rank 叠三块矩形的阶梯状尖拱（附录 E 结论 5），本项目仍是一个矩形。
- TG 另两路吸附：窗对准别的墙的平顶变阳台门、窗下 50 cm 内有玩家楼梯变楼梯门 —— 本项目没有平顶墙，楼梯 actor 也不挂墙，暂无落点。
- 拖拽途中变了门，门网格按 actor 中心摆、要松手吸附后才落到墙脚（中途回写变换违反「吸附只在最终裁决时做」）。

## 四、已作废但保留的结论（索引）

⚠️ **这些段落带删除线 `~~...~~`，是故意留着的 —— 保留是为了记住「为什么被推翻」，别当成噪音删掉。**
下表只是索引，正文在上面各节的原位。

| 作废的结论 | 在哪一节 | 什么时候、被什么推翻 |
| --- | --- | --- |
| ~~窗户这类物体**本身不带任何可视网格组件**，可见几何全归宿主房~~ | [D8 特征标记](#d8-特征标记acshousefeaturemarker窗户等) 开头 | 2026-09-06 用户裁决「附属物自己持有 mesh」。**理由**：旧口径是布尔时代的产物（cutter 必须在房子的网格操作里），布尔退场后这个约束消失 |
| ~~标记零几何、零材质~~ | [D8 特征标记](#d8-特征标记acshousefeaturemarker窗户等) 末尾「好处」那条 | 同上，2026-09-06 起标记持有自己的预制网格；「删了不留痕」那一半仍然成立 |
| ~~窗框砖 / 窗台盒随本条**退役**~~ | [落地与本节的三处出入](#落地与本节的三处出入2026-09-06-实现时改的) | 2026-09-06 订正：原以为「与接资产同一轮」就能退是**错的** —— 窗台盒填的是 `0..Z0` 那一段，现在删掉窗台以下会一直通到地面。见下面「未完成事项」 |
| ~~窗户零新机制~~（本项目的裁剪场比 TG 强一档） | [§1.4](#一窗户不是门那套机制--两边都不是而且本项目其实更强) 标题 | 2026-09-05 两层裁决。**作废的只有结论**：证据（clip 场是二维、`Rect`/`Circle` 上下都有界）全部仍然成立，而且正是第③级洞缘砖贴合能同时贴合上下两缘的依据 |
| ~~`SolveBlockLayout` / `BuildFramePlan` **不需要为窗改一行**~~ | [§4.3](#四cshouse_openingcell--solveblocklayout-要怎么给窗户让位) 整节 | 【推测】已于 2026-08-31 验证成立；**2026-09-05 两层裁决后整节作废** —— 两层之后 `BuildFramePlan` 不再是「给洞缘贴一圈装饰」而是砖层的一部分，问题本身消失 |
| ~~W4 跨边转角窗~~ | [§三](#三fcswallopening-够不够表达窗户够挖洞不够摆框) 的表 | 2026-09-05 用户裁决**不做转角窗**（谓词 `NearCorner` 即最终答案）；卷二可行动清单的 ~~A14~~ 随之作废（那一行留在原文档） |
| ~~由房子另开实例通路~~（W2 的后半） | [§三](#三fcswallopening-够不够表达窗户够挖洞不够摆框) 的 W2 行 | 2026-09-06 裁决：标记自己持有 `UStaticMeshComponent`，**标记自己就是实例通路** |

⚠️ 另有三条作废项**不在本文**、留在原文档：计划书结论节的 ~~本项目不做预制框 ⇒ ③对窗同样要开~~（2026-09-06 订正）、
卷二可行动清单的 ~~A7 面板垂直细分~~ / ~~A9 窗/玻璃母材质~~ / ~~A14 转角窗 W4~~。它们所在的段落主题不是窗，按重组规则原地不动。

---

## 五、未完成事项（从原文如实摘出，不新增）

**卡住的**

- ⚠️ **窗框砖 / 窗台盒退不了役，卡在砖层能不能把 `0..Z0` 填上（P1）**。墙板是 `AddPanel(CellMin, CellMax, Z0, clip)`
  从 `Z0` 起砌的，窗台盒填的是 `0..Z0` 那一段；现在删掉它，**窗台以下会一直通到地面**，预制窗框只盖洞缘、盖不住那块。
  出处：[落地与本节的三处出入](#落地与本节的三处出入2026-09-06-实现时改的) 第三行。

**已知并接受的代价**

- ⚠️ **属性面板 `Windows` 那一份从此是「裸洞」**：没有标记、没有网格。那条路在文档里一直写着是「授权 / 测试用的便利入口」，
  真正的来源是标记。出处：[窗不出框砖，门照旧出](#窗不出框砖门照旧出2026-09-06-用户裁决的直接后果)。
- **`csh.*` 那几个 CVar 没做**（**有意偏离计划**）：`HostProbeDistance` / `SnapDistance` / `DragIdleSeconds`
  落成了逐 actor 的 `UPROPERTY` 而不是 console 变量。计划写的是 CVar，但逐 actor 可调更符合本项目其余部分的口径，
  当时**未擅自改计划正文**。出处：[D8 收口 · 未做](#d8-收口acswindowmarker-已落地2026-08-31-对照文档-a6)。

**没有自动化覆盖的**

- ~~**拖拽期的实时通知没有无头覆盖**~~ **2026-09-06 补上**（见时间线「拖动期通知补上无头覆盖」与
  「视口拖放不出窗」两节）：前者钉住"每帧写 transform 零阻塞、标记逐帧贴在锚点派生位上"，
  后者钉住"spawn 完什么都不调也必须出窗"。⚠️ 仍然**没有**覆盖的是真正的 gizmo 拖拽本身
  （需要 Slate），无头这边测的是它的执行面 `HandleDrag` 与触发时序，不是鼠标。
  出处：[D8 收口 · 未做](#d8-收口acswindowmarker-已落地2026-08-31-对照文档-a6)。
- ~~**窗笔刷 EdMode 那一层零覆盖**~~ **2026-09-06 当日晚些补上**（见
  [收尾：把 EdMode 那一层也钉住](#收尾把-edmode-那一层也钉住2026-09-06-当日晚些)）：叶子契约、
  按钮 → 激活 → 目标 → 退出这条链、子蓝图那一族的健康度都有断言了。
  ⚠️ 仍然**没有**覆盖的是**鼠标驱动的那一次落笔**：`FCSBrushEdModeBase::InputKey` 的 LMB-按下分支
  无条件解引用 `Viewport` / `ViewportClient`，无头造不出真的 `FEditorViewportClient`，传 nullptr 会崩；
  `BeginStroke` / `CommitStroke` 又是基类 private。于是基类里
  `if (bExitAfterCommit) ExitTemporaryMode();` 那一行的**字面连接**没有断言 ——
  两端各自都钉住了（叶子确实要求退出、Esc 走同一个 `ExitTemporaryMode()` 确实退得掉），中间那一步没有。
- **编辑器里的线框回执没做**：`bCausesCut` / `LastReject` 已经算出来并暴露给蓝图/脚本了，
  但「用线框颜色区分已生成 / 被拒」那一步需要 `UActorComponentVisualizer`，不在当时那一轮的范围。
  出处同上。

**「洞之外」还缺的（卷二 §三 W 系列，逐条形态见该节的表）**

- **W1 裁决回执**：`QueryFeaturePlacement` 现在只返回 bool，要改成 `FCSFeaturePlacement{ bAccepted, Reason, SnappedWorld }`。
  ⚠️ 因为 P2（窗被门整条边挤掉是**预期行为**），这条从「锦上添花」升级成**必需品**。
- **W3 样式 / rank 枚举**：`ECSOpeningShape` 加 `PointedArch`（尖拱，gothic 的洞形），样式与 rank 留在标记 actor 上不进洞。
- **W5 `Tag` 已被门占满**：`Tag` 现在写门的 `Slot & 0xFF` 进顶点色 G 通道，窗要区分「门/窗/被拒」得抢同一个字节 —— 属 D14 的通道字典问题。
- **W6 `Z0` 没有下限守卫**：加 `csh.WindowMinSillZ`。
- **P3 `PierWidth` 对窗过宽**：`CSHouse_OpeningCell` 加一个按 `Type` 分流的墩宽（门 `PierWidth`、窗 `csh.WindowPierWidth` 默认 0）。
  ⚠️ 它是 `inline` 头函数、被墙板与谓词两处调，改签名要同步。
- **P4 窗的 `SourceId` 排序**：`CurrentOpenings.Sort` 末位加 `SourceId`（GUID 有全序）—— 这是**幂等短路的正确性条件**。

**仍在原文档、本文只交叉引用的**

- 卷二「十、可行动清单」的 **A2 / A3 / A4 / A10 / A11**（A10/A11 属 D12 花箱那条线，依赖 C2 拍板）。
- 卷二「九、待确认」的 **U1 / U2 / U3 / U4 / U6 / U8**。其中 **U4**（`WallAttachment` / `WindowDecoratorInfo` 字段构成）
  于 09-05 曾因 W4 作废而关闭、**09-06 重新有下游**（锚点「与 TG 一致」），PDB 只给类型名，只能按性质对齐，**不阻塞**。
- 卷零「回归的既存基线红」里的一条：**预制框翻边宽度是否盖得住④逐像素切口的锯齿** —— TG 的 `window_cottage_1x1`
  是 78×17×160 的框板，对 26 cm 砖应当够，P1 出图验。（④ 已于 2026-09-06 作废，这条随之只剩观感复核的意义。）

---

## 六、本文**没有**收录的（主题不是窗，原处一字未动）

窗只是这些段落里的一个从句，按重组规则原地不动，本文只在此交叉引用：

| 原文位置 | 为什么不搬 |
| --- | --- |
| 计划书 `## 结论` 的开洞策略 / 公共基类 `ACSTinyGlade` / v1 通知那几条 bullet | 主题是全局架构，只提了一句「房/窗自身变动」「窗户增删移」 |
| 计划书 D4「墙的两层结构：砖层 + 灰泥层」与「洞缘的四级处理」 | 主题是墙的两层与洞缘四级，门窗共用；窗只是其中一个消费者 |
| 计划书 D4「`Z0/Z1/AxisUS/Skew` 为什么现在就要」 | 主题是洞字段要容纳楼梯 / 窗台 / 转角洞三类，窗是三分之一 |
| 计划书 D5「公共基类 `ACSHouseHandleActor`」 | 主题是抓手族的公共基类，窗标记只是同族举例 |
| 计划书 D6「拱间墩与转角墩」「让位规则」 | 主题是门拱；「门拱优先于特征标记」只是一句 |
| 计划书 D7「一对房子可能有多处接缝」里的 `InputSignature` | 主题是接缝签名，「开了扇窗签名不变」是举例 |
| 合卷卷零「门框砖建在了将被删除的设施上」（2026-08-30） | 主题是**门**框砖与石阶设施冲突 |
| 合卷卷零「附属物持有 mesh、锚点是权威」一节的**砖层 P1 部分**（`P1 起步` / `①升级成剪影` / `③已落地④作废` / `容量已解`） | 主题是两层墙的砖层落地，门窗共用 |
| 合卷卷零「🐛 spawn 期的误吸附会**自我固化**」 | 主题是 `GEditor->AddActor` 的 spawn 时序与「回写 transform 只在最终裁决时做」这条通用纪律 |
| 合卷卷零「🐛 转角标记被 `ResolvePierSpans` 擦掉」（2026-09-04） | ⚠️ **这条不是窗**：说的是 `StyleFlags` 整份清零把 `CSHouse_StyleCornerDoor`（转角**门**）擦了，属 D6 门的时间线 |
| 合卷卷一 A2「修 R2：门框砖 handover」 | 主题是**门**框砖的 handover 与包围盒 |
| 合卷卷二 §五 装饰物（D12）/ §六 藤蔓（D13）/ §7.1 / §7.3–7.7 / §八 C2 C3 C5 / §九 / §十 | 主题分别是摆件、藤蔓、资产总量与材质缺口、其它待拍板项；窗只是锚点举例 |
| 合卷卷五 §2.2「唯一真正的 SplineMesh：门/窗拱缘的『帽子』件」 | 主题是 SplineMesh 与 `GothicWindowBricks` subset 的归属 |

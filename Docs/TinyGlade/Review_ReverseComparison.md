# Tiny Glade 对比逆向报告（合卷）

两轮多 agent 对比逆向评审的原始报告。**含「被否条目」与「看似该抄其实不该抄」的清单 ——
翻已否结论前先读这两节，别重复推翻。**

结论已并入 [`Plan_House.md`](Plan_House.md)；落地进度在
[`Progress_ModuleComparison.md`](Progress_ModuleComparison.md)。

> **合卷说明（2026-08-31）**：由两份独立报告合并而成，正文一字未改，只把每份的 H1 换成卷标题、
> 其余标题整体降一级。第二轮原文的 `# HIGH / # MEDIUM / # LOW / # 被否条目 / # 合成 agent 存活部分`
> 因此变成 `## …`，仍是卷二内部的一级分组。

| 卷 | 内容 | 合并前的文件名 |
| --- | --- | --- |
| [卷一](#round-1) | 第一轮：门洞机制与改进清单 | `TinyGlade_对比逆向报告_第一轮.md` |
| [卷二](#round-2) | 第二轮：未覆盖维度 | `TinyGlade_对比逆向报告_第二轮.md` |

---

<a id="round-1"></a>

## 卷一 · 第一轮：门洞机制与改进清单

> 原文件 `TinyGlade_对比逆向报告_第一轮.md`，原标题「Tiny Glade 门洞机制 → 本项目的差异与改进清单」。

> 结论均带 `文件:行号` 或计划小节号。证据分级沿用：**【逆向报告确凿】**（原文标注 SYM/GLSL/双零命中）、**【逆向报告推测】**、**【我的推断】**。
> 计划行号全部按 `Plan_House.md` 实际文件核对过（队友摘要里 D4 之后的行号系统性偏 5–7 行，本报告已更正）。

---

### 一、先回答你的问题

**你的直觉对了一半：门洞确实"根本不涉及切割"，但它不是贴图。**

Tiny Glade 的拱门洞是**解析式逐像素裁剪**（analytic per-pixel clipping），完整链路是：

1. **CPU 整砖级剔除**：`trim_rows` 按 `FinalizedWallHoles::iter_holes_with_padding`（注意 **with_padding**，是保守外扩的洞）直接**不发射**洞正中间那些砖实例（报告 L60-72【确凿 SYM】）。这一层没有几何运算，只是数组少几项。
2. **跨界砖携带 3 个 float**：骑在拱曲线上的那圈砖，在 96 字节实例记录里填 `vec3 global_arch_height_vals`（拱曲线在砖左/中/右的世界高度，报告 L84【确凿，GLSL 直接给出】）。这 12 字节在结构里**本来就存在**（与 `uint seed` 凑满一个 16B slot），支持拱洞的边际空间成本为零。
3. **VS 分段插值成 varying**，**PS 一条判据 discard**。报告 L115 原文【确凿】：

```glsl
// gbuffer PS
if (world_y < 拱高) discard;      // flags&8 反向保留拱线以下 —— 拱圈石
```

配合 flags 位表（报告 L126/L128，该表标题 L119 自陈"GLSL 实证，语义列为推测"，即**效果确凿、语义推测**）：

| flags | 效果【确凿】 | 语义【推测】 |
|---|---|---|
| 8 | PS 反向裁剪 | 拱圈石（保留拱线**以下**） |
| 32 | 拱压扁 + 三平面 UV + **免拱裁剪** | 拱内楣/门框构件 |

于是一个带边框的拱洞只靠 2 个 bit 分工就切出来了，零几何运算。

**判定"不是贴图"的两条硬证据**：
- 判据表达式里**没有 `texture()` / `sampler` / `alpha`**，拱高是一个 interpolant，没有对应的纹理资源绑定。
- **depth-only/shadow PS 变体（`f0adff76`，全文 37 行）保留同一条 discard**（报告 L115【确凿】），而同一族的 depth 版 VS 是被**主动裁剪过**的（L168 明写"剔除了颜色 ID SSBO 与逐顶点颜色噪声"）——它在砍一切非必要计算，却保留了拱高插值。37 行、无 SV_TARGET 的 PS 里塞不下"纹理绑定 + UV 变换 + 采样 + alpha 阈值"。

**反例提醒**（免得把整个引擎的风格判错）：Tiny Glade **确实用 alpha 贴图 discard**，但用在**树冠**上（报告 L283 `canopy_alpha` 贴图 discard + 蓝噪声锥形消隐【确凿】）。正确的分界是：**有机形状用贴图 discard，建筑形状一律解析 discard**（拱洞 / 瓦片切边 / 天窗开洞，报告 L460 结语第 3 条）。分界的原因见下面的"新引入的问题"一节——建筑形状的判据必须能被 CPU 精确复算。

**代码库层面的补刀**：报告 L397【确凿，Cargo.lock + PDB 双零命中】"明确不存在：lyon、earcut(r)、meshopt、mikktspace、delaunator、poisson、geo、splines、noise"，L390 spade/rstar 只是 parry3d 的传递依赖、零直接实例化。**Tiny Glade 的代码库里根本没装做布尔/三角化的能力**——不是"选择不用"。

#### 与当前方案的根本分歧点

一句话：**Tiny Glade 让 CPU 只提供解析参数、洞形在像素阶段成立；本项目的计划要求洞是"实体几何上真实存在的缺口"。**

计划 L10（用户裁决）原文：

> **开洞用 MeshBoolean，洞本身是参数化的**……墙体生成为**闭合实体**（布尔减出的洞自带侧壁面）

这条"闭合实体"要求就是分歧的支点。而计划全文对"逐像素 / masked / clip / 裁剪平面"**零覆盖**——检索命中的全是同形异义（L119 顶点色通道门、L539/L549/L555 接缝角柱的几何裁剪协商、L662 装饰热力 RT）。**这条思路在计划里不是"考虑后否决"，是从未出现过。**

而**当前落地走的是第三条路**：既不是布尔也不是像素裁剪，是 CPU 参数化直接砌出拱洞——`Private/CSHouseActor.cpp:352` 注释"拱洞在生成时参数化切出"，`:18` `CSHouse_ArchSegments = 12`，`:369-411` 逐段手排（外脸 2 三角 + 内脸 2 三角 + 拱腹 quad + 顶带 quad = 8 三角/段，96 三角/门）。计划 L754 承认了这条偏差，原因是"`ApplyMeshBoolean` 是场景提取制……没有'常驻房体 − 原型 cutter'的网格操作数入口"。

---

### 二、门洞专节

#### 2.0 现在就在生效的一个显示 bug（不管走哪条路都该立刻修）

`Private/CSHouseActor.cpp:401-402` 原文：

```cpp
Writer.AddTri(PrevInner, TopBi, Inner, SlotWall, { 0, 0 }, { 0, 0 }, { 0, 0 });
Writer.AddTri(PrevInner, TopAi, TopBi, SlotWall, { 0, 0 }, { 0, 0 }, { 0, 0 });
```

拱上墙**内脸**全部 6 个顶点的 UV 写成 `(0,0)` —— 退化 UV 三角。同一循环里外脸（`:391-398`）和拱腹（`:404` 走 `AddQuad`）的 UV 都是正常的。

- 症状：从屋里看拱正上方那块墙是一片纯色，且与相邻实心墙 `AddBox` 的内脸接不上。
- 切线基**不受影响**（`AddTri` `:30` 的 tangent 由 `(B−A)` 位置算，不读 UV），所以只坏纹理不坏光照——正因如此容易被当成材质问题查半天。
- 修法：照外脸口径（`:392-398`）补真实 UV，注意内脸绕序是反的、UV 要对应到正确顶点。
- 顺手补一条 automation 断言（`CSHouseLogicTests`，计划 L749 已规划）：房体快照里不允许出现三顶点 UV 全等的三角形。

另两条同一片代码里的浪费（非 bug）：
- **顶带逐段发射**：`:406` 在循环里逐段 `AddQuad`，12 段 = 24 三角，可合成 1 quad = 2 三角，每门白费 22 个。合并前提我复核过：`SRel = CenterS − R·cos(A)`，A 从 0 到 π 单调，顶带是单调平面带，合并合法；且 cos 在两端变化慢，A≈0 与 A≈π 处现在是极窄细条，合并顺带治掉细缝。
- **分段数写死**：12 段半圆弦高 = `R·(1−cos(π/24)) = 0.008555·R`，按 D6 默认（拱宽 = 150−40 = 110 cm，R = 55）是 **0.47 cm** 折角，房子变大即线性放大。若确定不走 clip 才值得做自适应，公式是 `N = Clamp(CeilToInt(PI / (2 * Acos(1 - Tol/R))), 6, 48)`（**注意分母那个 2**：Tol=0.2、R=55 时正确值 19 段，漏掉 2 会给出 37 段）。

#### 2.1 三条路线对照

| | A. 现状：参数化砌拱带 | B. 计划：MeshBoolean 减 cutter | C. TG 式：解析逐像素 clip |
|---|---|---|---|
| 洞形精度 | 12 段折线，0.47 cm 折角随 R 放大 | 三角化洞缘，取决于 cutter 网格 | **解析精确曲线，无限分辨率** |
| 每门几何 | 96 三角手排（`:369-411`，43 行最易写错绕序/UV 的代码） | cutter 网格 + 布尔产物 | 0（面板整块） |
| 门宽变化的重建 | 全量快照 → `EnsureCapacitySync` + `EditMeshSync` + `BuildMaterialSections`（再一次 `EditMeshSync`）≈ **3 次 GT 停顿** | 同上再加 N 刀布尔 | 单条 TexCoord 流上传 = **1 次 `EditMeshSync`** |
| 洞口内壁 | **有**（`:404` 拱腹带 + 相邻 `AddBox` 端盖白送） | **有**（计划 L10 "闭合实体自带侧壁面"） | **没有，必须补几何** |
| 今天可用性 | 已落地 | **不可用**：`ApplyMeshBoolean` 只接受场景提取（`CSBoxSceneCollection.h:22-26` 只认 `UStaticMeshComponent` + landscape），且 `ECSMeshBooleanOp` 没有减法（`ComputeShaderMeshBoolean.h:13-23` 只有 ArrangementOnly/Union/KeepInside/KeepOutside） | 材质路径本来就开放（`CSMeshRenderComponent.h:43-44` 是普通 `UMaterialInterface*`，无强制 usage） |
| CPU 能否看见洞 | 能（但没人用） | 能（但计划 L418 明令禁止查询） | 不能 |
| 计划一致性 | 已记为有意偏差（L754） | 用户裁决 L10 | **推翻 L10 的"闭合实体"前提** |

**这里有一条最该说、而队友和计划都没说的话**：L10 否掉旧方案（按格跳面）的**理由**是"贴不住拱/圆的剪影，洞缘会漏缝"。**clip 路线在这一点上严格优于布尔**——解析判据比布尔的三角化洞缘还准。也就是说 clip 不但没触犯这条裁决的理由，反而是它的最优解。推翻 L10 时该摆的是这个论证，不是"布尔太贵"。

#### 2.2 推荐

**推荐 C（clip）+ 必配的洞口内壁组件，但作为一份提案交给你拍板，因为它推翻 L10 这条用户裁决。**

理由三条：

1. **报告 §7.3 列出的 TG 方案三个成立前提，本项目全中**：
   - 判据能写成解析式 —— 计划 L406 自己就写了"拱是下身矩形 + 顶半圆，**剖面解析可写，不必采样**"；
   - **不需要 CPU 拿到切完的真实几何** —— 这是本项目最强的一张牌：`Private/CSMeshRenderComponent.cpp:74` `SetCollisionEnabled(NoCollision)`，`Public/CSTinyGlade.h:23-24` 类注释"gpumesh 全线 NoCollision——派生类的拾取一律解析实现"，计划 L761"本计划不给它加 `UBodySetup`"，L418 的最强纪律"**布尔是裁决的下游，不是裁决的依据**"本身就宣告 CPU 从不查询切除结果。报告 §7.3 把这一条列为 TG 方案最要命的前提，本项目**恰好天然满足**；
   - 洞口内壁 —— 唯一真缺口，见 2.4。
2. **布尔路线今天连操作数入口都不存在**（L754 实证），而这条裁决的代价核算（TDR 三道闸、`CachedOthersMesh` 单刀增量、`LiveCutHz`）全是纸面估算，**一次都没在本项目里验证过**。
3. 参数化砌拱带对"矩形房 + 半圆拱"其实够用，但它把**几何精度变成了连续量 `WidthScale` 的函数**：门宽每跨一个 `DoorWidthQuantum`（2 cm，`:233`）台阶就要走一遍全量快照三次 flush。clip 把它降到"单流上传一次 flush"，并且拓扑只依赖 N、不依赖点亮集与每个拱多宽。

#### 2.3 落地步骤（改哪些文件）

**载荷通道用 UV0，不要动 UV1，也不用顶点色。**

⚠️ **本条已过期（2026-09-04 复核）**：多 UV 通路已打通——口径统一成**单条交错流**（`CSGpuMeshTypes.cpp:44` `ElementsPerUnit = 2 * Clamp(NumTexCoordSets, 1u, 4u)`、`CSGpuMeshSceneProxy.cpp` 逐组挂 stream component 且 SRV 只设一次），上限 8 组。下文保留作当时的评审记录。


- **UV1 是结构性不可用**（我复核属实）：`Private/CSGpuMeshSceneProxy.cpp:303` `Data.TextureCoordinatesSRV = S.SRV;` 在 `:289` 的 switch 循环里，后一条 TexCoord 流会直接覆盖前一条；`Private/CSGpuMeshTypes.cpp:37-50` TexCoord 描述符写死 `ElementsPerUnit = 2`；引擎 `LocalVertexFactory.ush` 的 `VertexFetch_TexCoordBuffer[NumFetchTexCoords*VertexId + Index]` 是**交错取数**，两条独立 buffer 物理上表达不了。真要 UV1 得改 5 个 C++ 处 + 7 个 .usf 的 stride，不划算。
- **征用 UV0 的损失为零**：拱上墙内脸 UV 本来就是全 `(0,0)`（见 2.0），墙纹理改世界三平面即可。
- **顶点色是可行的备选但精度不够**：`Private/CSGpuMeshTypes.cpp:51-63` 是 BGRA8，`Private/CSHouseActor.cpp:41` 四通道恒 `(1,1,1,1)` 完全空闲。用 `q/16+0.5` 这类线性映射插值仍是仿射的（**不会**产生伪洞），问题纯是精度：8 bit 在 110 cm 拱上是约 0.9 cm 的洞缘量化台阶，贴脸可见。

**六步：**

1. **`RebuildBodyMesh`（`Private/CSHouseActor.cpp:363-412`）改成按 D6 子段等分发射 N 个闭合 `AddBox`**，N/Pitch 从 `ComputeDoors` 外抛（新成员 `TArray<FCSHouseEdgeSlots>`）。删掉 `:369-411` 的拱扇循环，**保留 `:404` 的拱腹带**（搬进第 4 步的 Reveal 网格）。
   ⚠️ **陷阱**：`CSHouse_GetEdge` 把边 1/3 缩短了 2T（`:99-105`），护角区 `[0, CornerMargin]` 与 `[Len−CornerMargin, Len]` 现在不属于任何子段，**要各补一块面板，否则墙上开天窗**。
2. **`AddTri/AddQuad/AddBox`（`:28-65`）加 `FCSArchField{CenterS, HalfWidth, SpringZ}` 参数**，`AddTri` 把 UV 改写成
   `q = ((s(v) − CenterS)/HalfWidth, (z(v) − SpringZ)/HalfWidth)`。
   q 是 (s,z) 的仿射函数，透视校正插值精确还原（我复核过）。无洞面板写哨兵 `q = (8, 8)`。
3. **新建 `M_TinyGladeWall`（Masked）**，OpacityMask 判据：
   `保留 = !(q.y <= 0 ? abs(q.x) < 1 : dot(q, q) < 1)`
   按代码参数验算的三个边界：顶面 `q.y = (H − SpringZ)/HalfWidth ≥ (LintelBand + HalfWidth)/HalfWidth > 1` 恒保留；底面 `q.y < 0 且 |q.x| < 1` 恒被弃（门下地板本就该开）；面板端盖 `|q.x| = Pitch/(Pitch − PierWidth) > 1` 恒保留（**墩不被误切**）。
4. **新建 `RevealMeshComponent`** 承载洞口内壁，见 2.4。
5. **`ComputeDoors` 的哈希拆两层**：`TopoHash`（含每边 N）与 `ShapeHash`（每门 CenterS/Width/Height）。`ReevaluateSite`（`:312-317`）改三分支：TopoHash 变 → `RebuildBodyMesh()`；只 ShapeHash 变 → `UpdateArchField()`。
6. **新增 `UCSMeshOps::UploadTexCoords(UCSMesh*, const TArray<FVector2f>&)`**：照 `Private/CSMeshOps.cpp:1135-1141` 的 Upload lambda，只走 `Context.TexCoords()`，跳过 `EnsureCapacitySync` / `BuildMaterialSections` / `InvalidateSections`。
   **proxy 不重建这一条成立，但依据是 `Private/CSMeshRenderComponent.cpp:280` 的实际判据** `AllocationGeneration != BoundAllocationGeneration || BatchMaterials != BoundBatchMaterials`（不是 `:249-259` 的解释性注释）——不重新分配、材质表不变即不重建。

**`DoorWidthQuantum` 必须保留。** 它不是"被布尔架构逼出来的妥协"，是这条链上唯一的节流器：删掉它，触发频率从"跨 2 cm 台阶"变成"地面每采样一次就变"，即使单次代价降到 1 次 flush，总 GT 停顿也会**上升**。要真连续，前置是计划 L194 的整栋 `EditMeshAsync`，那条不能顺手降级。
另：计划 L222 的"拱会呼吸"指的是**段长随墙长/N 浮动导致拱宽微伸缩**（拓扑量），与 L235 的量化无关，别把两件事接在一起。还有一处代码相对计划的未记录收紧：L235 只要求"宽度量化后**再进哈希**"，而 `:233` 把量化施加到**几何宽度本身**。

#### 2.4 clip 路线唯一的结构性缺口：洞口内壁必须补几何

`discard` 只丢像素、不生成表面。改成"子段面板 + clip"后，判据 `q(s,z)` 沿厚度方向恒定，会在洞的范围内**同时**弃掉外脸、内脸、底面，中间不剩任何表面 → 从洞口一眼看穿墙。

我验证过一个可能的"自动补救"并否掉了它：相邻面板的端盖确实在 `s = S_k` 处成对存在，但拱边界在 `s = CenterS ± HalfWidth`，而 `HalfWidth = (Pitch − PierWidth)/2 < Pitch/2`，落在面板**内部**，那里没有任何既有面。

**TG 是怎么兜的**：它有真实的拱/楣几何。报告 L23 模块图 `system_wall_constructor（铺砖/**拱/楣**/栅栏/柱/灰泥/楼梯支撑）`、L450 CPU↔GPU 边界总表【确凿】"CPU 生成布局/实例：墙砖、**拱/楣**/栅栏/门、柱/支撑…"——拱与楣是与墙砖并列的真实实例，靠 `flags&32`（VS 按拱高压扁贴合曲线 + **免拱裁剪**）遮住裁剪断口。

**补法**：

1. 保留 `:404` 的拱腹带 + 补两侧竖直门樘（z=0 → SpringZ，宽=墙厚），搬进新的 `RevealMeshComponent` + transient `RevealMesh`。
   **先例可直接照抄**：`Private/CSHouseActor.cpp:121` `PillarMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>` + `:489-493` 内联的"NewObject → SetMaterial → CopyFromMeshSnapshot → SetGpuMesh"四步（`Public/CSTinyGlade.h:53-54` 明写基类上传管道只服务主网格，柱子是自己内联的，Reveal 照做，不必等 D9 的参数化改造）。
2. `RevealDescHash` = 每门 (Edge, CenterS, Width, Height)。每门约 28 三角，纯 CPU + 一次 `CopyFromMeshSnapshot`，**不碰房体网格**。这与计划 **L448**（柱子不并进房体网格）的论证逐字同构。
3. **接缝纪律**：clip 判据是精确圆，拱腹带是 N 段折线。折线内接会漏最多一个弦高的缝，必须按**外接**多边形建（半径 × `1/cos(π/2N)`），宁可微覆盖不可漏缝。
4. 内壁法线朝洞内，Opaque 单面。**不要**为省这步开双面材质：UE 双面 Masked 会让洞口露出墙板背面（法线朝内、无光照信息）成黑洞，且 clip 判据与朝向无关、救不回来。
5. 这个组件同时是计划开放问题 **L787**（"门洞装饰升级：Tiny Glade 式砖拱/门框砖沿洞缘排布"）的天然宿主——对应 TG 的 `flags&32` 构件族。

#### 2.5 会连锁消失的工作项（我逐条核销过）

**真消失：**

| 消失的东西 | 计划位置 |
|---|---|
| 布尔链本身 | L10 / L182 |
| `csh.LiveCutHz` 节流 | L202 / L436 |
| `CachedOthersMesh` 单刀增量 + `UCSMeshPool` 借暂存网格 | L433-435 |
| 布尔三道闸（哈希不变不重切 / 拖拽降级免切 / 失败保基体） | L769 |
| `ClassifyFragmentsCS` 的 TDR 风险 | L769 |
| 开放问题 **L789**（cutter 操作数供给） | 关闭 |
| 每门 96 三角 + `:369-411` 43 行手排代码 + 2.0 的三条缺陷 | 代码 |

**不消失（别在提案里吹）：**

- `BuildMaterialSections`（拓扑变化时仍要）；
- `CopyFromMeshSnapshot` 的同步 flush（L765 本来就把它单列为一条与布尔无关的风险）；
- **整栋 `EditMeshAsync` 改造**（P2 门 L740 保留，只是理由从"N+2 次 flush"缩成"2 次 flush + 拖拽期全量上传"）。`Public/CSMeshOps.h:27-29` 明写"Everything here is synchronous… a render flush is what makes it safe"，这是既定契约不是疏忽；
- **P6 的其余七项验收门**（L744：自动 attach/自毁、编辑器 world 下确实在 tick、从房 A 拖到房 B 中途不消失、宿主被删随之消失、拒绝原因可见、松手弹回、零阻塞回读）。布尔操作数只是其中一项的下游，**"删掉布尔就解锁 P6"不成立**；
- Reveal 侧门宽变化仍走全量快照 → `EnsureCapacitySync` + `EditMeshSync`，且多一个 scene proxy 与一次 `RecreateRenderState_Concurrent`（`Private/CSMeshRenderComponent.cpp:107`）。**净收益仍为正**（房体那条链是大头），但验收门里别写"门宽变化零重建"。

**地位上升：** 计划 L418 的 CPU 谓词 `QueryFeaturePlacement` 从"布尔的上游裁决"变成**唯一真源**，必须与 PS 的 clip 表达式**同式**。建议把剖面写成一个 `CSHouse_ArchProfile` 纯函数，墙板写 q、拱腹建带、L406 的重叠谓词三处共用，并进 `CSHouseLogicTests`（L749）。

**要改的文档位置**：结论 L10 重写；D4 L182 去布尔段；D8-B L391-410 把"cutter = 原型静态网格按参数缩放摆位"改成"洞 = 解析剖面参数，进 UV0 场"；D8-C L432-437 删；P6 门 L744 的"每帧至多一刀"改写；开放问题 L789 关闭。**P2 门 L740 与 D4 子节 L187-202 保留不动。**

#### 2.6 新引入的问题

1. **形状扩展的成本形态变了**：从 L785 的"多一份原型资产 + 一条解析 2D 剖面"变成"多一条材质分支 + 一条 C++ 剖面函数"。团队若更愿意让美术加资产而非让程序改材质图，这是净损失，**需要你拍板**。缓解：剖面做成 MaterialFunction + `ECSOpeningShape` 静态开关。
2. **材质图成了正确性关键路径**：材质坏了 = 洞不见了，没有编译期检查。项目里**一处 Masked 先例都没有**（`Plugins/PCGPlugins/Shaders/` 与 `Source/` 全树 `discard` / `clip(` 零命中）。缓解：automation 断言 CPU 侧 `CSHouse_ArchProfile()` 的形状（材质那半边测不了）。
3. **洞口内壁组件是必配负债**，不能只删不补（2.4）。内壁与墙板之间会有一圈深度极近的表面，将来若拿回阴影要注意 shadow bias。
4. **Masked 的 early-Z 代价**：`r.EarlyZPassOnlyMaterialMasking` 默认 **0**、`r.EarlyZPass` 默认 **3**（引擎 `Renderer/Private/RendererScene.cpp:128-145`，实测），所以 mask 表达式在 depth prepass 与 base pass 各求值一次；整墙改 Masked 会让**没有洞的绝大多数面板一起付账**。**但先测再说**，别预先建结构：把 `WallMaterial` 换成 OpacityMask 恒 1 的 Masked（零代码），贴脸镜头下 `stat GPU` 对比；代价显著时才加 slot 3 只给点亮子段的面板（`AddQuad/AddBox` 已透传 Slot，`:47/:56`，几行的事）。**不要**为此建 `LitSetHash`/`ArchShapeHash` 三层哈希——proxy 重建的真实判据是材质列表（`Private/CSMeshRenderComponent.cpp:280`）而不是 section 表，只要每面墙恒有至少一个 slot-0 和一个 slot-3 三角，点亮集变化根本不触发重建。
5. **碰撞里没有洞** —— 但这与布尔路线**同样不存在**（`Private/CSMeshRenderComponent.cpp:74` + L761 是既定架构），不是本方案引入的债。
6. **"退而求其次用贴图"差在哪**（你点名的问题，逐条核过）：(a) 分辨率写死，512² 摊到 110×220 cm 拱上是 2.1 mm/texel，解析式无限分辨率；(b) 门宽是连续量，贴图要么每宽度一张要么 UV 拉伸把圆拱拉成椭圆；(c) 采样成本 ≠ 0，解析判据是 3 条 ALU；(d) **CPU 端复算不了** —— L418 的谓词必须与 PS 剪影同式。这第 (d) 条正是 TG 树冠用贴图（L283）而建筑形状一律解析（L460）的分界线。

---

### 三、其余改进项（按 severity）

#### HIGH

##### H1. 屋面缺一个共享的 profile 求值器

- **TG**：屋面是被瓦/梁/尖顶/雪/老虎窗共同引用的**单一求值器**。报告 L203【确凿】支撑梁 VS"沿 `circle_normal` 按 `roof_profile` 平移，并施加与瓦片**完全相同**的屋面凹陷噪声——梁跟着屋面一起歪"；L205【确凿】雪 mesh"加与瓦片一致的抖动噪声保证贴合"。L182【确凿 SYM】另给出 `RoofRidgeDims` 参数化脊向/脊长/高度 + `profile_curve_ws`。
- **现状**：屋面方程散在 `Private/CSHouseActor.cpp:422-437` 的函数内局部量里（TanP / RidgeH / EaveOut / EaveZ 全是 const 局部），屋面是两块实体板 `:437`，山墙是独立三角棱柱 `:448`；脊向由 `:417` `const bool bLongX = FootprintSize.X >= FootprintSize.Y` **隐式导出**；计划全文 grep "脊/ridge" **零命中**。
- **差距**：等到铺瓦、铺梁、挂老虎窗、封博风板时，每个子系统会各自再推一遍屋面高度/法线，而 TG 明确警告过梁必须复刻**完全相同**的屋面变形才不脱开。第二条：脊向是导出量，D5 单边推拉（L210"对侧不动、中心随动"）一旦让 X 穿过 Y，`:417` 翻转、屋顶瞬间转 90°，且 `FootprintSize` 在 `BodyDescHash` 里（`:249`）必然整栋重建——拖动中用户看到屋顶"啪"地翻过去。
- **建议**：① **必做且零风险**：新增 `Public/CSHouseRoof.h`，把 `:417-449` 的方程抽成纯函数 `CSHouseRoof_EvalZ/EvalNormal/IsUnderRoof`，`RebuildBodyMesh` 的两块坡板改调它，铺瓦/铺梁/D8 谓词（L382"落屋顶 → 不生成"）全部复用。② `FCSRoofDesc{ ERidgeAxis RidgeAxis; float Pitch; Overhang; Thickness; }`，RidgeAxis 显式 UPROPERTY（构造时按长轴初始化），`:417` 改读它，3 行。③ 可选、需你点头：加 RidgeLength / ProfileCurvature + 变高墙板（**"RidgeLength>0 落出 hip"继承报告的【推测】，别当既定事实**）。
- **代价**：① 是纯重构，三角数与像素完全不变，约 2 小时。③ 才有真代价：变高墙板必须改走 `AddTri` 手填 `V = 世界Z/UVScale`（`AddQuad` 的 UV 从 quad 局部 (0,0) 起算，`:50-51`），否则山墙纹理随高度拉伸；RidgeLength 进哈希后 D5 拖 handle 会整栋重建，需照 `DoorWidthQuantum` 量化。

##### H2. per-instance custom data 不是结构性缺失，补 5 处约 40 行即可解锁逐瓦裁剪平面

（合并了"墙体"与"屋顶"两个维度的同一条）

- **TG**：报告 L188-197【确凿，shader 内一致定义】`RoofShingle` 含 `uvec2 clipping_plane_` = half4 世界平面（8 字节），PS 判据 `if (dot(vec4(p,1), plane) < 0) discard;` ——"瓦片在屋顶边缘被平面裁掉，**不换网格**实现精确切边"。L460 把它与拱洞、天窗并列为同一条方法论。
- **现状与勘察材料的纠正**：勘察材料称"两道门都关着、PerInstanceCustomData 恒返回 0、只能 fork LocalVertexFactory.ush"——**这条结论我逐行核实后推翻**。引擎 `Shaders/Private/MaterialTemplate.ush:1499`（PS）与 `:1529`（VS）在 `VF_USE_PRIMITIVE_SCENE_DATA` 分支之外确有 `#elif USE_INSTANCING && USES_PER_INSTANCE_CUSTOM_DATA` 直读 `InstanceVF.InstanceCustomDataBuffer`；`LocalVertexFactoryCommon.ush:7` 的 `NEEDS_PER_INSTANCE_PARAMS` 含 `(!VF_USE_PRIMITIVE_SCENE_DATA && USES_PER_INSTANCE_CUSTOM_DATA)`；本项目 VF 恰好 `USE_INSTANCING=1`（`Private/CSGpuInstancedMeshVertexFactory.cpp:28`）且不注册 `SupportsPrimitiveIdStream`，条件**自动成立**；`LocalVertexFactory.ush:1006` 会填好索引。副作用也查了：`InstancedVFLooseParameters` 已绑（VF `.cpp:95`），代理填的 `InstancingFadeOutParams = (BIG, 0, 1, 1)`（`Private/CSGpuInstancedMeshSceneProxy.cpp:284`）代入得 x=1、y=1，不会触发"y==0 就关 WPO"。
  **真实缺口只有两行**：`Private/CSGpuInstancedMeshVertexFactory.cpp:47-48`（实测原文）：
  ```cpp
  UniformParameters.InstanceCustomDataBuffer = InstanceOriginSRV; // unused; must be non-null
  UniformParameters.NumCustomDataFloats = 0;
  ```
- **建议（5 处）**：① `Private/CSGpuInstancedMeshSceneProxy.h:32-41` 的 `ECSGpuInstancedAuxSlot` 尾部加 `SourceCustomData/VisibleCustomData`；② `.cpp:319-361` 各加一次 `AddAux(..., sizeof(float), **PF_R32_FLOAT**, ...)`（引擎声明是 `SHADER_PARAMETER_SRV(Buffer<float>, ...)`，`Engine/Classes/Engine/InstancedStaticMesh.h:46`，必须 R32_FLOAT 不是 float4）；注意 `:350-352` 在 `bExternalPackedSource` 时源侧只留 1 元素占位，所以**源侧必须走生产者缓冲，只有可见侧由 mesh 分配**；③ `OnStreamsAllocated`（`.cpp:363-392`）多取一个 SRV，`SetInstanceStreams`（VF `.h:52`）加第四参；④ VF `.cpp:47-48` 填真值 + `NumCustomDataFloats = 4`；⑤ `Shaders/Private/CSGpuInstancedMesh.usf:222-226` 的压缩写入后按同一个 `Dst` 补拷贝。
  **索引口径已核对**：`BatchElement.UserIndex = int32(Lod * Layout.InstanceCapacity)`（proxy `.cpp:681`）就是 shader 的 `InstanceOffset`，而 `Dst = Lod * MaxInstancesPerLod + Slot`（usf `:220`）且 `MaxInstancesPerLod = Layout.InstanceCapacity`（proxy `.cpp:569`），严格对齐 —— **CustomData 必须按可见槽写，不是按源实例写**。
  验收：材质放 PerInstanceCustomData 节点接 BaseColor，用 `ACSGroundShaperActor` 石阶冒烟测（它是这条组件的现役用户）。
- **然后瓦材质设 Masked**，OpacityMask = `dot(float4(AbsoluteWorldPosition,1), PerInstanceCustomData0..3) > 0`。16 字节/瓦 + 一次点积，换来山墙边缘任意角度精确切边、不需要任何"半瓦/角瓦"美术变体。
- **必须同时接受的边界**：平面裁剪只对**薄片**成立。对闭合实体（屋顶板 `:437`、墙盒 `:367`、D7 角柱）裁剪会露出没有 cap 的内壁，所以它**不能**用来解"屋顶与墙相交"，也**不能**解"接缝角柱穿插"——L556/L562 已由你裁决用邻近合并解决并明写"比裁剪简单得多"，本条不触碰。铺瓦时山墙斜边**必须**配封边构件（博风板），否则会直接看到瓦的断面（TG 就是靠 flags&8/32 的构件遮住断口的）。
- **代价**：每可见实例 +16 B（可见槽从 80 B → 112 B，且按 `InstanceCapacity × NumLODs` 分配）；`NEEDS_PER_INSTANCE_PARAMS` 打开后多一个 COLOR1 插值器。**硬限制**：这条通路只在 `USE_INSTANCING` 分支成立，将来若给这个 VF 加回 `SupportsPrimitiveIdStream` 改走 GPU-Scene，必须整条换成 `LoadInstancePayloadDataElement`（`MaterialTemplate.ush:1486-1498`），两条路不能混用。
  **划掉一条**：所谓"复用 `VisibleLightmap` 白嫖零显存"——那条流的 SRV 已绑给 `VertexFetch_InstanceLightmapBuffer`，且一个槽只出一个 SRV，省不下工作量只换来语义脏。

##### H3. `DoorSlotOpen` 是既不序列化也不受事务保护的路径依赖状态

- **现状**：`Public/CSHouseActor.h:237`（实测原文）`TMap<uint32, bool> DoorSlotOpen;` —— **裸 C++ 成员，无 UPROPERTY**。`Private/CSHouseActor.cpp:228` `const bool bWasOpen = DoorSlotOpen.FindRef(Key);` → `:229` 据此在 `SlotOn`(0.6)/`SlotOff`(0.4) 之间选阈值 → `:244` 写回。
- **差距**：滞回让 `ReevaluateSite()` 成为路径依赖函数，而承载路径的表冷加载时为空。① 冷加载入口 `:507`/`:513`，此时所有子段一律按 0.6 判定——**存盘时覆盖率在 [0.4, 0.6) 的已开拱，重开关卡直接消失**。这正面撞计划 L9 的公理"只读推导、不带历史不带副作用"。② `RebuildHouse()`（`:333-336`）显式 `DoorSlotOpen.Empty()`，所以"手动强刷"与"自然重求值"在同一世界状态下产出不同几何，而它是 `CallInEditor` 的通用刷新入口。③ 现有断言抓不住：L753 的冷加载 VERIFY 只验"加载后再复评不变"（第二次有了表就自洽了），验不到"加载后 == 存盘前"。
- **建议**：`Public/CSHouseActor.h:237` 改成 `UPROPERTY(NonTransactional) TMap<uint32, bool> DoorSlotOpen;`。**必须带 `NonTransactional`** —— 普通 `UPROPERTY()` 会被事务缓冲整份捕获，于是一次无关的 details 改参 + Ctrl+Z 就会把滞回表回滚到旧代（与 H4 是同一个坑，两条用同一条纪律）。改文档 L9："不带历史"改成 `F(世界状态, 上一次的开关表)`，并加纪律"凡双阈滞回，开关表必须与宿主同生命周期序列化，且必须 `NonTransactional`"。
- **这不是孤例**：L271 墩样式双阈（60/75）、L501 接触双阈（`LinkMinOverlap` / `SeamMinExposure` 40-30）是同型滞回，而 L358 裁决接缝是 transient 自管理 actor、L470 把 `ContactStates` 放在 subsystem 上 —— **同一个坑还会再踩两到三次**，`ContactStates` 因此应挂在房子上而非 transient 的 seam actor 上。
- **代价**：近乎零（每房 4 边 × ≤32 槽的 bool，N 已 clamp 到 32，`:206`）。端到端断言很便宜：把路画到覆盖率 ~0.5、存盘、重开、断言开拱数不变。注意 L239 说的"纯函数"今天还不是纯函数（`ComputeDoors()` 是读 `Ground` 的成员函数，且 `Private/Tests/CSHouseLogicTests.cpp` **尚不存在**），要写纯函数测试得先把 `分割`/`点亮` 抽成 free function。

##### H4. `Mirror` 是事务型 UPROPERTY 且无 `PostEditUndo`

- **现状**：`Public/CSGroundActor.h:241-242` 是裸 `UPROPERTY() FCSGroundMirror Mirror;`，内含 `:33 Heights` / `:34 Colors`；`Private/CSGroundActor.cpp:217-233` 的 `ApplyPaintStroke` 就地改镜像；整个文件零 `Modify()`，整个 `Source/` 零 `PostEditUndo`（唯一 `NonTransactional` 命中在 `Public/ComputeShaderShallowWater.h:148`）。
- **差距**：危险序列——先改一次属性（事务记录"无路"的镜像）→ 再画路（不进事务）→ Ctrl+Z 撤那次属性改动 → **镜像被回滚成"无路"**。而没有 `PostEditUndo`，`RebuildGroundMesh` 不跑：屏幕上路还在、`SampleRoadWeight` 说没有，房子下一次重求值时拱突然全关（门拱确实读它，`Private/CSHouseActor.cpp:219-221`）。这命中风险节自列的"镜像/GPU 双写漂移"，但成因是它没考虑到的那一种——**语义分叉来自事务系统**。附带成本：257² 每次事务捕获约 528 KB，1025² 约 8 MB。
- **建议**：① `Public/CSGroundActor.h:241` 改 `UPROPERTY(NonTransactional)`。这一行同时消掉误抹与缓冲膨胀，且**与 L125"笔刷不撤销"的用户裁决完全同向** —— 它把口头约定变成类型级保证。② 加 `PostEditUndo() override { Super::PostEditUndo(); EnsureMirrorInitialized(); RebuildGroundMesh(); }`（`RebuildGroundMesh` 末尾已 Broadcast，`.cpp:180`）。同一个 override 也该加到 `ACSHouseActor`（调 `ReevaluateSite()`，因为 `BodyDescHash` 含量化世界变换，`:246-250`）。
  依据：`AActor::PostEditUndo` 在 **`Engine/Source/Runtime/Engine/Private/ActorEditor.cpp:803-822`**（注意不在 UnrealEd 下），函数体只做 `InternalPostEditUndo` + 一条 `UpdateAllPrimitiveSceneInfos`，**不调 `PostEditMove`、不保证跑构造脚本** —— 所以重建逻辑必须放在 override 里。
- **代价**：十几行，零风险。`NonTransactional` 不影响存盘。

---

#### MEDIUM

##### M1. 改色现状比"会触发重建"更糟：细节面板换材质**静默无效**

TG 侧 L344【确凿】：`system_color` 直接写 `WallColorIds/GateColorIds` SSBO，shader 按 `source_id` 查表，**改色不重建 mesh**。

本项目：材质**只**在 `ACSTinyGlade::UploadTinyGladeSnapshot` 里绑（`Private/CSTinyGlade.cpp:27-28`），唯一调用点是 `RebuildBodyMesh`（`Private/CSHouseActor.cpp:456`，实测确认），而它只在 `BodyHash != BodyDescHash` 时调（`:313`），`BodyDescHash` 的输入（`:248-252`）**不含任何材质**。于是：改 `WallMaterial` → `PostEditChangeProperty` → `RerunConstructionScripts` → `OnConstruction`（`:501-504`）→ `ReevaluateSite` → 哈希不变 → 跳过 → **材质从未被重新绑定**，画面零变化，必须手点 `RebuildHouse()`。柱子同病（`:490`）。

讽刺的是底座早就具备 TG 那条能力：`UCSMesh::SetMaterial`（`Private/CSMesh.cpp:844-851`）→ `NotifyMaterialsChanged`（`:853-858`，注释明写"Deliberately does not touch Generation"）→ `HandleMeshChanged`（`Private/CSMeshRenderComponent.cpp:242-270`）只重解 batch 材质、一个三角形都不重传。

**建议（约 10 行，零风险）**：把材质绑定提成 `ACSHouseActor::ApplyMaterials()`，在 `ReevaluateSite` 开头无条件调一次（`UCSMesh::SetMaterial` 自带同值早退，`.cpp:848`，重复调零成本）。顺手按 L182 把槽表定死并传满（`BuildMaterialSections` 明确"including slots no triangle uses: their arg set draws zero indices"，`Public/CSMeshOps.h:399-402`）。

##### M2. 阴影：房体组件 `CastShadow = false` 写死，整栋房子（含屋顶、柱、地面）一点影子都没有

（合并三条重复项）

- **TG**：L115【确凿】depth/shadow PS 37 行保留同一条拱裁剪 discard；L197【确凿】瓦片的 DormerHole 天窗开洞**只存在于 depth-only PS**（144 行，遍历 SSBO）；L157【确凿】阴影侧 roof_tile 恒 LOD1、按 4 个视图各生成 draw list。TG 整套像素级精修建立在"阴影 pass 存在且判据一致"之上。
- **现状**：`Private/CSMeshRenderComponent.cpp:82-84`（实测原文）：
  ```
  // GPU-Scene instance culling overrides the custom indirect args in the Virtual Shadow Map
  // pass, so indirect-drawn meshes cannot cast VSM shadows (same limitation as road/display).
  CastShadow = false;
  ```
  `Private/CSGpuInstancedMeshComponent.cpp:65` 同。计划全文对阴影**零覆盖**（grep 只命中 L594 的 `C4458 shadow` 变量遮蔽）。
- **两层结论**：① **"Masked 洞的阴影一致性"在本项目是伪命题** —— 一栋不投影的房子，洞在阴影里当然不会不一致。别把 TG 的 L115 误当成本项目的技术风险。② 反过来，Tiny Glade 那种低角度阳光下没有影子是致命的（屋檐无投影、悬空/落座/承重柱读不出来，P7 验收 L745 的"抬房生柱""塑形物隆起 Δ → 屋顶升高 Δ"几乎不可判读），而根因是 indirect draw，**与开洞方案完全正交**。
- **建议**：房体是最有资格退回非 indirect 直接 draw 的网格——`Public/CSGpuMeshSceneProxy.h:113-118` 的契约已写了两条分支，`.cpp:107-116` 都实现了。给 `UCSMeshRenderComponent` 加 `bDirectDraw`（默认关），为真时跳过 `.cpp:321-324` 的无条件 `IndirectArgsBuffer` 绑定；`RebuildBodyMesh` 按材质槽排序三角后在 CPU 侧算出 per-section `(FirstIndex, NumPrimitives)`（`S.TriangleMaterialSlots` 是 CPU 权威，`:43`）；解开 `CastShadow = false` 后先在 `r.Shadow.Virtual.Enable 0` 下验证，再单独复核 VSM。
- **两条队友漏掉的账**（我实测补上）：① 全代码库 `DrawDesc.NumPrimitives` **从未被赋过值**（只有 `Public/CSGpuMeshSceneProxy.h:118` 的声明、`.cpp:82` 的形参、`.cpp:69` 与 `Private/CSMeshRenderComponent.cpp:62` 的读取），恒为 0 —— **这是死代码不是可用退路**。② `FirstIndex` 同样没通：`SubmitGpuBufferDraw` 的参数表里根本没有它，`.cpp:104` 写死 `BatchElement.FirstIndex = 0`，**多 section 的 direct draw 必须改这个 public static 函数的签名**，改动半径波及所有 gpumesh 派生代理。
- **另一条必须一起接受的**：`Private/CSMeshRenderComponent.cpp:55-57` 明写 section 的 `FirstTriangle/TriangleCount` "deliberately never read"，而 `Public/CSMesh.h:56-62` 自陈它们在 CPU 侧的正常态是 `INDEX_NONE`（"which is the normal case"）。要影子就得在房体这条路径上破例维护一份 CPU 侧镜像，重新引入 `CSMesh.h` 明确规避掉的失效模式。
- **顺序**：排在 clip 改造之后（拓扑稳下来簿记才好写），且**先做 30 分钟真机实测**（`:82-84` 那条注释写下时的 GPU-Scene 行为在 5.7 可能已变；它只否定了 indirect，没否定 direct draw），**不要先写进验收门再动工**。

##### M3. 石阶散布在编辑热路径上做无条件 `FlushRenderingCommands`

- **链路**：`Private/CSGroundActor.cpp:217` → `:233` Broadcast → `Private/CSGroundShaperActor.cpp:97-100` → `:436 RebuildSteps` → `:471 Scatter` → **`Private/CSGroundShaperSteps.cpp:210 FlushRenderingCommands();`**（队友摘要写的 :173 是错的）。
- **根因是纯技术性的**：`Work` 在栈上（`:91`）、渲染命令按引用捕获它（`:94`），所以函数返回前必须阻塞。而稳态下 `:87-89` 的 `FMath::Max(Want, Palettes[Index].Capacity)` 保证不分配，`:212` 的 `MoveTemp` 回写是彻底的 no-op。
- **触发条件比想象窄**：`RebuildSteps:448-450` 的哈希短路在 `Scatter` **之前**，所以只在"本座的层/弧段/铺装记录真变"的 tick 才 flush；但拖动一个塑形物时 `:576-581 → RebuildTerrain:380-395` 每帧广播，足迹相交者会每帧付。
- **建议**：拆成 `EnsureCapacity`（只在真需要扩容时 ENQUEUE + flush，`AllocatePooledBuffer` 必须在渲染线程）+ `Scatter`（**按值捕获** `TArray<FPaletteBuffers>`，`TRefCountPtr` 拷贝即加引用，录完 pass 直接 return）。**顺序不能颠倒**：不先拆 `EnsureCapacity` 就去掉 flush，GT 侧的 `Palettes` 永远学不到新分配的 buffer，会从"多一次停顿"退化成"画不出石阶 + 显存 churn"。同步改 `Public/CSGroundShaperSteps.h:50` 的契约注释（"返回时缓冲区可用"正是 flush 在维持的语义）。
- **论证口径纠正**：这**不**违反计划 L687 那条铁律（它在 L685-688 的"在途请求纪律"小节内，作用域是 D12 状态机），而且 L765 明确接受了一次同步 flush。正确的说法是"这是一次不必要的、可零代价去掉的停顿"。约 40 行，一个 TU。

##### M4. D3"无效重算被哈希短路成零成本"对塑形物不成立——短路点在昂贵计算之后

- `Private/CSGroundShaperActor.cpp:97-100` 把 `ChangedBounds` 参数名直接注释掉、无条件 `RebuildSteps()`；哈希短路在 `:448-450`，**位于 `BuildStepPlan`（`:445`）之后**。
- 默认参数下 `BuildStepPlan` 规模（`:200-206`；`LiftHeight=300`/`StepHeight=30`/`StepAngleStepDeg=3`）：9 层 × 120 方向 ≈ **1080 次 `SampleRoadWeight`**（`:263`）；非孤立时再加验证采样 + 割线两端 + `CSShaper_RefineIters = 6` 次二分（`:230-256`），逼近 1 万次。**队友漏掉的更脏的一笔**：`IsFootprintIsolated()`（`:158-169`）在 `:206` 每次被调，内部是一次完整的 `TActorIterator` 世界遍历 → O(N²) actor 遍历。
- **两条修法，推荐 B（零裁决冲突）**：
  - **A**：`:97` 恢复 `ChangedBounds`，`Changed.ExpandBy(CellSize)`（**必要**：`SampleRoadWeight` 是双线性的）后与足迹相交才继续。3~5 行。**但这是在消费侧引入区域过滤，L8 是"不做区域过滤"的用户裁决**，属局部开口。
  - **B**：把短路点提前——在 `:441` 之前先算**输入哈希**（本座参数 + actor 变换 + 地面一个单调递增 `GroundRevision`），不变就 return；顺手缓存 `IsFootprintIsolated()` 结果。约 30 行。语义完全是"消费者无条件重求值 + 幂等哈希短路"，**一个字都没改 D3 裁决**，只是把哈希从"输出哈希"换成"输入 + 输出"两级 —— 这与 D7 L330-332 已裁决的 `InputSignature`/`SeamsHash` 两级判断逐字同构。
- 建议把 L140 的措辞改准："消费者可无条件重求值，但**短路必须发生在昂贵计算之前**"。两条都**不引入 CSSceneDirty3D**（正是 D3 想推迟的）。

##### M5. 相位：D7 留下的二选一里，"tick group 分两相"在 UE 中不成立

计划 L348 把"tick group 分两相"与"pull 式 `EnsureRefreshed(帧戳)`"并列未定。**tick group 那条不成立**，三条：① 同一 TickGroup 内 actor 无顺序保证，要有序必须 `AddTickPrerequisiteActor`，而 **房子根本不 tick**（`Private/CSTinyGlade.cpp:12` 基类构造 `PrimaryActorTick.bCanEverTick = false`；落地是 delegate 同步回调 `Private/CSHouseActor.cpp:146-150`），两者不在同一调度域；② 编辑器 world 里 actor 默认不 tick（计划 L377 自己标了这个坑），而这套系统的主场景恰恰是编辑器；③ P5 验收门（L743）全是几何断言，测不到相位。

**建议定死为 pull 式**：`ACSTinyGlade::EnsureRefreshed(uint64 Gen)`（原语已齐：`ReevaluateSite` 幂等 + 双 desc 哈希短路 `:313-327` + `bInReevaluate` 重入保护 `:297-298`）。seam 的 `RefreshFromOperands()` 第一件事就是对两端房子各调一次，落座必然先于交点表求解——L496 第 6 步的相位约束从"调度顺序"降级成"函数调用顺序"。

**两个必须写进实现的点**：① **代数不要用 `GFrameCounter`**（队友说它"编辑器非 tick 帧不前进"是错的，编辑器每帧都在推进它）；真正的问题是同一帧内的多次外部变更（逐笔 Broadcast 就是）会被误判为已刷新。用"变更代数"计数器，**且与 D12 L685 的输入代数戳共用一个**。② `bInReevaluate` 今天是"重入就 return"，pull 模型下这会让一次本该刷新的调用静默变 no-op 而 `LastRefreshedGen` 却已写 —— 重入被拒时**不写** `LastRefreshedGen`，或直接 `ensure()` 报出来（L520 的不变量本来就要写成 `check`）。

##### M6. "拖拽中降级为免切/松手补切"制造进行中 ≠ 定稿；且两个限速器并存

- **TG 结构上禁止这件事**：报告 L54【确凿】"画线进行中与抬笔定稿走**完全相同的重建管线**"，`InProgressWall` 只是资源壳、**砖 SSBO 无任何'预览专用'变体**。（注意：报告**没有**说 TG 全程没有节流，那是【我的推断】，别当确凿用。）
- 计划 L183/L769 的三道闸之一是"拖拽中可降级为免切/松手补切"——这是全计划里唯一会造成肉眼可见"两种房子"的条款，且今天没有成本依据（当前根本没跑布尔）。**建议删，或降级成默认关的应急阀**；P6 验收门 L744 的"每帧至多一刀"换成 TG 的判据："拖拽中任意一帧暂停，画面与松手后逐三角相同"。
- **两个限速器冗余**：`csh.LiveCutHz` = 30 是固定魔数，而 `EditMeshAsync` 的在途拒绝 + pending 合并（`Public/CSMesh.h:504-506` 原文"Refused, and OnComplete never runs, when an async edit is already in flight"，L196-200 + L740 已定机制）给出的速率**自动等于 GPU 实际完成速率**，不需要调参，也不会像固定节流那样在最后一个 tick 落进窗口时吞掉末帧。建议删 `LiveCutHz`，保留"连续两帧位移小于阈值跳过"（那是输入去抖不是限速）。
- **L235 的量化理由错位**：它写成"防布尔重切"，但病因是 `CopyFromMeshSnapshot` 的同步 flush（`Private/CSTinyGlade.cpp:30`，L765 自陈），不是布尔。改成"防拱宽视觉抖动"，并与 L233"连续量无需滞回"接上（现在两处理由脱节）。
- **明确不动**：`CachedOthersMesh` 单刀增量（L433-434）保留 —— 只要 L10 还没被正式推翻，它是死代码但不是错设计。

##### M7. D12 提交链第二次回读取的是 CPU 自己刚上传的数据；且三处文档自相矛盾

- 热力图完全由 CPU 从"已放 bound 列表"上传上去画的（L704"diff 应用完成后，CPU 上传已放 bound 列表 → GPU pass 全量重画"），它唯一的 CPU 消费者是 D13 L719 的藤蔓起点吸附（L711 走异步回读）。**这一级回读是把自己刚写出去的数据读回来**，而且位于 L729 总延迟 6-10 帧里靠后那段。
- **建议**：`RT_DecorHeat` 降级为**单向写**，藤蔓 source 改查"已放植被实例列表"的空间哈希（L636-643 的 diff 本来就在维护这份列表，稳定 id = hash(class, 格坐标)；`FCSSpatialHash2D` 模块内已有、D10 L479 已在用）。答得还更准（吸到真实实例而非"W 超阈的格"）。连带删掉三级状态机的第 ③ 级与那一路的代数戳，总延迟降到约 4-6 帧。**不触碰 L662"场存 GPU + 异步回读"的用户裁决**——被删的只是热力图这一半的回程。
- **三处矛盾必修**（比性能问题更急）：L599"整条链纯 CPU……零回读天然成立" vs L662-671"场存 GPU + 异步回读（用户裁决）" vs L746 的 P8 验收门写的"**CPU 场** + 上传贴图"。三处描述的是三套不同数据流，实现时按哪条写都能找到依据。建议保留 L662 为正文，改 L599 与 L746，并补断言"提交链的回读次数恰为 1"。
- 场是否也搬回 CPU：**不在本轮推翻**。L617 说的是"上 GPU 能省掉写这个优化"，不是"CPU 算不动"；要重开该在 P8 动工前先量一次（50 栋 × 257² 合成场景）。

##### M8. 排除规则有两份实现；`RT_DecorHeat` 通道定义三项自相矛盾

- **TG**：L292-302 的 7 张顶视 mask 表，写入者与消费者分列；L299 + L316 坡度由 `_terrain_derivative.cs` **一次算出**（`rocky_terrain.x = smoothstep(0.75, 1.25, |∇h|)`），不是每个消费者各自算；L262 `buildings_mask` 是**软调制**（"墙边草压低、内院留 40% 矮草，非硬排除"）；L297/L272 它还是**模糊 + 非模糊双版** —— 这正是"用纹理代替逐候选包围盒测试"能成立的原因。
- **现状**：场 pass 已把排除烘进 `RT_DecorField`（L612 门前净空清零、footprint 内清零；L613 道路清零），**然后 CPU 谓词又判一次**（L647"房 footprint 内 / 道路上 / 门前净空 / 陡坡 → 拒"）；坡度只在 CPU 侧存在。同一条规则两份实现 = 半径改一处忘一处，症状是"门口偶尔冒出一个箱子"而两份代码对着各自的定义都是正确的。
- **`RT_DecorHeat` 三项矛盾**：L671"RG16F：R 物体 / G 植被、主存储在 GPU" vs L700-707"与期望场并列的第二块 **CPU float4 数组**、XY 保留 / Z 物体 / W 植被"。格式、通道号、存储位置全对不上；L783 的"XY 保留通道用途"只在 float4 那版里讲得通。
- **建议**：① **硬排除单开一张 `RT_DecorMask`**（R8G8，257² 约 66 KB），不要挤占用户新增的热力语义 —— 因为热力写在放置**之后**（L708"全量重画"）、硬排除必须在放置**之前**就绪，同一张 RT 两阶段写容易互相冲掉。② 每个硬排除通道存**清晰版 + 膨胀版**（照 TG 的 `buildings_mask` 双版），膨胀半径取 palette 里最大 `PlacementBound` 外接半径 —— **没有膨胀就不能删 CPU 谓词**：候选是一个格，但放置物带 bound（L626-629），中心在排除区外、盒子压在房子上的候选会被放行。③ 坡度移进场 pass 的同一个全网格 dispatch。④ CPU 侧只保留两条真正顺序依赖的谓词：跨层 bound 相交（L645）与同类间距球（L646）。⑤ 结掉 L671 vs L700-707，保留用户定义。
- **代价**：物体层仍在 CPU（用户裁决），它要读膨胀 mask ⇒ 提交链回读从 1 张变 2 张（仍在 L683-689 允许的边界内，但代数戳要覆盖两张）；P8 断言口径从"谓词拒了"改成"mask 为 0 所以候选没产生"；膨胀是保守的，会让所有摆件离墙更远，若观感不可接受就退回"膨胀版只用于植被"的混合方案。

##### M9. 植被降级路径的目标写错了：计划写 ISM，本插件已跑通的是 `UCSGpuInstancedMeshComponent`

- **先纠正一处流传的错误结论**（勘察材料与 D9 进度注记都错了）：`Public/CSGroundShaperActor.h:213` 是 `TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> StepComponents`，该文件零 `UInstancedStaticMeshComponent`；**计划 D9 正文 L447 本来就写对了**（"石阶因为要 GPU 实例化 + 剔除 + LOD，走自己的 `UCSGpuInstancedMeshComponent`"），L452-454 连"CPU 不算一个变换、不回读一个字节"都写了。写错的只有 L755 的一行进度流水注记。
- **真实缺口**：L715/L782 把植被的逃生口指向**引擎 ISM**。ISM 的 `PerInstanceSMData` 同样是 CPU 侧序列化数组，等于把 actor 的 GT 成本换成数组的 GT 成本；而本插件已经有一条**实例根本不经过 CPU** 的路（`Private/CSGroundShaperActor.cpp:482-513` 证明"容量/包围盒不变时重散布连一次阻塞交接都不需要"）。
- **建议**：改两行文档（L715/L782 的目标改成 `UCSGpuInstancedMeshComponent`；L755 订正）；严格执行 L715 自己划的界"**父类字段不变，只换渲染宿主**"——`ACSGroundDecorItem` 保留为配置载体（`Channel == Vegetation` 的类其 CDO 读成一份 palette 条目，运行期不 spawn 实例 actor；物体层继续 spawn 真 actor）。散布 kernel 照 `Public/CSGroundShaperSteps.h:14-56` 的三件套复刻，用 `Shaders/Private/CSGroundSteps.usf:81` 同一条 `InterlockedAdd`。间距靠构造（格边长 = `SameClassRadius`，每格至多一个候选）而非逐候选比较。
- ⚠️ **kernel 的输入不要去读地面 `UCSMesh` 的常驻流**：那些流的访问必须经 `EditMeshSync`（阻塞）或 `EditMeshAsync`（在途会被拒），从第三方 pass 直接绑 SRV 不是既有形态。把需要的一切放进场纹理。
- **必须拍板的代价**：`Private/CSGpuInstancedMeshComponent.cpp:65` `CastShadow = false` ⇒ 植被无影子，而物体层 actor 有影子，**两层观感不一致**。（缓解证据：D9 石阶已在这个约束下验收通过。）另：无 per-instance custom data（除非先做 H2）、植被不再能逐株选中/删除、P8 验收门 L746 的两条断言要改探针。

##### M10. 窗台/门边定点杂物：D12 结构上够不到的一整类内容

- **TG**：L340【确凿】autoclutter 含 `add_autoclutter_around_{gates,windows}` / `add_birdnests`；L336【确凿】每个装饰件是**一组 mesh**，含 `_flowerbed_locations` —— "哪里可以摆花盆"是**烘在美术资产里的命名子网格**，不是运行时算的。
- **现状**：D12 的放置域**只有地面 2D 格**（L608 同格、L643 候选是 tile/格、L649 落高采 `Heights`）；D8 标记本体无网格（L364 用户裁决），产物 `FCSWallOpening` 里没有任何"洞周围哪里能摆东西"的字段。全文 `窗台|门楣|灯笼|鸟巢|socket|锚点` **零命中**。
- **建议**：① **锚点定义与 cutter 资产解耦** —— 每种 `ECSOpeningShape` 配一张小锚点表（`UDataAsset` 或纯 C++ 常量），条目 `{FName Slot, FVector2f NormalizedPos, FVector2f NormalOffset}`，位置用**洞局部归一化坐标**。这样对当前参数化砌拱和将来的 cutter 路线**都成立**。⚠️ 不要照队友原案把锚点挂在 `UStaticMeshSocket` 上：**cutter 原型资产今天一份都不存在**（L754 + `Public/CSHouseActor.h:35-36` 逐字印证"布尔的'纯 mesh 操作数'入口尚不存在"，L789 未结案），那是让未落地特性依赖已知受阻的特性。socket 降为将来的可选覆盖。② `ReevaluateSite()` 多产一份 `TArray<FCSDecorAnchor>`（复用 `FCSHouseDoor` 已有的墙参数空间变换，`Public/CSHouseActor.h:16-27`）。③ 房上加 `AnchorClutterComponent`，形态照 D9 承重柱（L447），**不并进房体网格**。④ 与 D12 是单向边：只把地面投影写进 M8 的物体硬排除通道，不参与 D12 的场/候选/谓词，不成环。⑤ **锚点位置必须量化**（门宽随地形连续收窄，L225-235），否则地形每抖一下就重建。
- **做不了的那一半**：`add_birdnests` 依赖 parry 查询世界搜任意候选点（L354），本项目明确没有碰撞基建（L184/L368），也不该为它引入。

##### M11. 若走砖：必须直接喂 GPU 实例源，且先做 LOD

三条绑在一起，只有决定墙体砖化时才成立：

- **喂法**：绝不能经 `PerInstanceTransforms` —— `Public/CSGpuInstancedMeshComponent.h:355-357` 是 `UPROPERTY() TArray<FTransform>`（96 B/实例进关卡包），`:204-208` 自陈"Every one of these repacks the whole instance array and re-uploads it, which now includes a blocking render flush"，`Private/CSGpuInstancedMeshComponent.cpp:425-475` 每次 mutator 全量 Morton 排序。正确形态是 `SetInstanceSourceGPU`（`.h:250-252`，C++-only 非 UFUNCTION，单测必须写成 C++），照 `CSShaperSteps` 新建 `CSHouseBricks`。
- **几何账**：20×10 cm 砖铺满默认房（`FootprintSize=(600,400)`、`WallHeight=300`）约 3000 块 × TG 的 300 tri/砖 = **90 万三角**，比现在的 472 tri（4 面墙 48 + 屋顶 24 + 山墙 16 + 四门 4×96）高约三个数量级。而 TG 用来抵消的三件套本项目**只有一件**：`Shaders/Private/CSGpuInstancedMesh.usf` 只有视锥 + 距离 + 按屏幕尺寸选 LOD（`:186-226`），**全文件与 proxy 对 HZB/Occlusion 零命中**，没有遮挡剔除、没有 impostor。墙是两面对贴的，背面那一半砖每帧照画。
- **顺序**：① **先做 LOD，这是零代码的** —— 给砖资产手工做 LOD1（8 tri 倒角盒）/LOD2（12 tri 单盒），`SetBaseMesh` 通路直接吃（`Private/CSGpuInstancedMeshComponent.cpp:174-192` 读 `LODResources` 与 `ScreenSize`），并把 LOD 数压到 2-3 省可见缓冲（按 `InstanceCapacity × NumLODs` 分配，proxy `.cpp:327`）。② 遮挡问题**不要纸面选**：队友提的"改走引擎 ISM + Nanite"必须降格为待验证选项（它与本条自身的反序列化论证冲突，且放弃了 D9 已跑通、被你裁决为"必须在 GPU 线程中进行"的零回读形态）。自研的替代是在 `RunCulling` 里加一趟 HZB pass。两条都要真机测过再选。

##### M12. 世界空间 value-noise：只有"整墙低频起伏"这一半能白嫖

- TG VS 第 3 步（报告 L107【确凿】）：世界坐标 ×0.4（2.5 m 晶格）8 角点哈希三线性插值 → XZ ±3 cm，"相邻砖共享噪声场，墙面歪而不散"。
- **需要限定**：TG 是在每块砖原点求值后**刚体平移整块砖**，产出砖间离散错位；同一个场逐顶点作用在连续墙面上只得到**平滑曲面扭曲**，砖级不连续感（这条技巧真正的卖点）拿不到。2.5 m 晶格 × ±3 cm 在 6 m 墙上仍留下一段可用的手作感低频起伏，值得做，但**别在文档里宣称是"TG 第 3 步的等价物"**。第 1/4 步（seed 旋转、四角起伏）绑死"砖是独立刚体"，整块网格做不了。
- **一条非显然的性质，值得记进计划**：`AddTri` 每个三角形都新建三个顶点（`Private/CSHouseActor.cpp:30-43`），整份网格是**无共享顶点的三角汤**；同一世界位置的多份副本在世界空间噪声下算出的偏移逐位相同，所以逐顶点位移不会撕缝。将来若"优化"成索引化共享顶点，只要位移仍是世界坐标的纯函数就还安全，但一旦改成依赖顶点法线/切线的位移（同一位置在不同面上法线不同）会立刻炸开。
- **实现要点**：`MF_TinyGladeWallJitter` 接 `Absolute World Position` × 0.004（UE 是 cm、Z-up）；**槽 0 与槽 1 必须挂同一函数同一幅度**（山墙棱柱走槽 0 `:448`、屋顶坡板走槽 1 `:437`，是互插实体不是共享顶点，`RoofThickness = 12` 远大于 3 cm 不会露缝，但幅度不一致会让屋顶与山墙相对滑动），柱子第三种材质同理。**不要**把同一函数接到地面网格（257²~1025² 顶点，且顶点色 R 已被道路权重占用）。房子移动时 `TranslateMesh` 改写世界坐标 ⇒ 噪声重掷，与 TG 同构，属预期，要写进计划免得被当成抖动缺陷。

##### M13. 石阶等值线的星形假设：现在不迁移 marching squares，把接缝钉死

- `AnalyticRingRadius`（`Public/CSGroundShaperActor.h:186-189`）沿"本座中心出发的射线"参数化，只对**关于该中心星形**的等值线成立。非孤立时退回割线 + 6 次二分（`Private/CSGroundShaperActor.cpp:230-256`），且括号两端不跨越该层高度时 `bUsable = false`（`:246`）→ 该方向被丢弃，症状是**路穿过两座相接土台时接合处的石阶弧段断掉**。任意 mesh 形状塑形物（L461/L783）动工时这个假设会**彻底失效**——这是那条开放问题的一个隐藏前置。
- **判断：现在不迁移。** 散布侧已是 GPU（M3 修完即零阻塞），CPU 只解 1080 个采样点；迁移唯一收益是"任意等值线形状"，为此付一整个 marching-squares kernel（TG 那份 163 KB / 3427 行，成本主要在 16 case 全展开与 5/10 鞍点）不划算。
- **要做的四件小事**：① 把"不迁移"的理由写进 D9；② 接缝已现成——`BuildStepPlan` → `TArray<CSShaperSteps::FCurve>`（`Public/CSGroundShaperSteps.h:17-22`，纯 POD）是一条纯数据契约，将来换 marching squares 只需替换一个函数，写进文件头注释与 D9；③ `:206` isolated 为假时加一条 Warning 说明弧段可能断裂是已知降级；**修 L451 的"也不需要二分"**（代码在非孤立情形确实有二分回退，计划漏记）；④ **重叠双份石阶的去重先不做** —— 先在 `L_TerrainOpsDemo` 摆两座等高相接的塑形物、画路穿过接合处**实测**是断裂、双份还是兼有（队友原案用 GUID 排序仲裁，而 GUID 在 PIE/复制下的稳定性本身是 L569 的待实测项，等于用未验证机制修未验证问题）。
- **迁移真正的隐藏成本**（若将来做）：等值线要按弧长重参数化才能喂 `RDG_SmoothSpline`，而 marching squares 出来的是**无序线段集合**。TG 不需要连线（逐边独立发实例、靠位置哈希稳定），我们因为要复用 `SolveBlockLayout` 的整体缩放铺装**必须**连线。所以迁移时应连同"是否还要 SolveBlockLayout 式铺装"一起重估，那是需要你裁决的取舍。

##### M14. D13"vine 解算是否确定性"可以现在结案

用源码直接答，不必等落地。**精确结论：几何形态确定，输出缓冲里线的排列顺序不确定。**

- 分叉归属用 `InterlockedMax(RW_ProposalOwners[NearAttractor], P + 1u)`（`Shaders/Private/SpaceColonizationQueue.usf:380`），紧邻注释写明这是为了复现"CPU 顺序提交时最高提议者胜出"——**与 GPU 执行顺序无关**；配套 reset 在 `:208-209`。
- `RW_ClaimCounts` 的 `InterlockedAdd`（`:270`）只是计数。
- 下游位移噪声按**世界位置**哈希（`Shaders/Private/VVVoxel.usf:1476-1478`），不按索引。
- 唯一非确定处是 `InterlockedAdd(RW_LineCounter[0], 1u, LineIndex)`（`:573`），而 `LineIndex` 只流向 `RW_LineLength` / `RW_NodeLineIndex` / `LineOffset` / `SegBase`（`:578-579, :645-651`）——**纯 buffer 布局，没有 shader 拿它当种子**。

**两条推论**：① L730 后半段的顾虑可删、L783 的开放问题可关，哈希短路从"承重设计"降级为"纯省算力"；② **不要对输出网格做内容哈希**（那会每次都不同），desc 哈希只能建在 source/target 输入上——L728 本来就是这么写的，正好自洽。
**两条残余非确定性必须写进结论，别宣称"完全确定"**：`:574` `if (LineIndex >= TargetCount) return;` 容量溢出时哪几条线被丢取决于原子顺序；上游 source/target 的**输入顺序**必须由 D13 保证确定。

**顺带一个便宜的观感升级**：`VineUVScan*` 三个 pass 已在 GPU 上算出逐点沿曲线的累计弧长并写进 `TexCoord.y`（`VVVoxel.usf:1753-1765`，且是分段扫描、每条线各自从 0 起算 `:1740-1749`）。加一个 `GrowthFront` 标量 + 材质 `clip(V − GrowthFront)`，**任何 compute pass 都不重跑**就能做出生长动画。落地前确认两件事：① `V` 的方向（反了就从梢往根长）；② SC 的线是从 tip 回溯到 root 的完整链（`:565-572`），多条线共享主干 ⇒ 主干段有重复几何，clip 边界处有 z-fighting 风险，要实测。**这不是 TG 的增量生长**（TG 是 CPU 逐帧模拟 + 持久 `VecDeque<IvyPoint>`，报告 L287-290），分支形态从第 0 帧就定死，用户中途改墙时是整条重解后重新揭示一遍。**不建议**为此做真·逐帧模拟：那要引入持久藤状态 + 墙面约束 + 跨墙跳跃 + 修剪四套东西，与"声明式重求值 + 哈希守卫"架构和 L727 的用户裁决正面冲突。

---

#### LOW

- **L1. 墩/角柱/承重柱的归属矛盾待你裁决**：D6-B 的 L258 写墩归"**房体网格**（布尔的自然剩余）"、L260 写"不共享数据、不共享组件、不合并"（该节 L241 标"用户配图裁决"、L249 标"用户订正"），但同一节 L267 又写"柱进 `PillarMeshComponent` —— 与 D9 同一组件、同一材质、同一叠砖样式"。**L260 与 L267 直接互斥**，落地时无法确定墩到底进哪。另两条：D6-B 缺"块列从锚点起算"的纪律（L294/L542 已有先例，墩跨度是连续量，`floor(跨度/PanelCell)` 会在跨过整数倍时让整墩块面重排）；承重柱至今是光板方盒（`Private/CSHouseActor.cpp:465-495`），`PanelCell` **全代码库零实现**。
  ⚠️ 队友原案"把三者统一到一个砖柱组件"我否掉了：与 L260 的用户订正正面冲突，且漏掉一个几何问题——墩是仍留在房体网格里的残料墙体，在其上叠砖柱是两份互穿几何，真做必须让 `AddSegment`（`:365-369`）跳过该跨度，从而推翻 L266"零额外布尔/自然剩余"的语言。
- **L2. 洞口/窗框的悬停高亮走 TG 的 `source_id` 廉价等价物**：TG 用 `source_id` 的 bit30 一个位 + shader 分支（报告 L85-87【确凿】），成本为零。**不要**走"换网格/加描边组件"——几何变了就要重跑 `RebuildBodyMesh`（`BodyDescHash` 会变），等于每次悬停整栋重建。建议把 opening 的 SourceId 低 8 位写进 `S.Colors` 的 G 通道（`:41` 今天恒 1），材质里 `abs(VertexColor.g*255 − HighlightId) < 0.5` 混高亮色，HighlightId 是 MID 标量。命中测试继续解析（openings 表本身就是 `(EdgeIndex, CenterS, Width, Height)`）。
  顶点色通道要与 M1 的色号方案统一分配，建议钉死 **R = 色号 / G = SourceId 低 8 位 / B、A 保留**，写进类注释。
  （队友说的"槽表是跨关卡稳定 id、插槽会整体错位"是**假的**：材质是三个命名 UPROPERTY，`Public/CSHouseActor.h:84/:87/:90`，槽号由 `:455` 现场组装，加 GlassMaterial 只是多 append 一项，已配好的关卡一个都不用动。)
- **L3. 顶点色笔刷是 3D 球，与消费侧全 2D 不对称**：`Shaders/Private/CSMeshOps.usf:376` `length(P − BrushCenter)`、镜像孪生 `Private/CSGroundActor.cpp:257` 同。TG 全部 mask 是顶视 2D（报告 L44【确凿，多 shader 一致】）。**但队友列的三条后果里两条站不住**：EdMode 落点走 `TraceCandidatePoint → RaycastGround`（`CSGroundPaintEdMode.cpp:58`），笔刷中心逐 tick 贴地，"台顶刷不上""编辑顺序有意义"都不成立；而"坡上路自动变细"恰恰是 TG **主动要**的效果（L246 的 `_paths_directional_contract.cs` 沿梯度收窄），我们只是关不掉也调不了。真正剩下的价值是：**它把 L4 的笔画栈钉死在 3D 折线上**。建议加一个投影模式参数（默认保持 3D），**只在同时做 L4 时才做**。注意 `:256` 的注释是**双写 parity 纪律**（"别退化成 2D，那会与 kernel 分叉"）而不是设计辩护，两侧一起改不违反它；改动面比想的大——`CSBrushEdModeBase.cpp:201-202` 的球形预览与 `:377-389` 的球内 3D 采样是基类代码，被三个 EdMode 共用。
- **L4. 道路顶点色是全项目唯一"没有源"的权威数据**：`Heights` 侧已是笔画栈模型（`Private/CSGroundActor.cpp:458-525` 从塑形物列表绝对式重算 + `:491` 幂等短路，与 TG 的 per-texel 重放同构），`Colors` 侧无源——`ApplyPaintStroke` 直接写结果、笔画不留痕。后果：改分辨率即丢（`:88-94` 在 `NumCells/CellSize` 变时 `Colors.Init(BaseColor)`，Heights 会从塑形物长回来、Colors 不会）；L777 的"通道争用"开放问题在 TG 那边的答案不是加通道映射表而是**独立层**（L241 的 4 个 `WorldRaster` + `erase_other_path_variants`）。
  **建议只做诊断与记录，不急着实现**：真做笔画栈有两条队友低估的成本——① 点必须存 3 个分量（只要笔刷判据仍是三维距离，丢 Z 重放不出原样，与 L3 绑定）；② **必须逐 tick 采样点重放，不能重采样折线**（`Add` 累加与 `Erase` 乘 `1−Amount` 都不幂等，同路径按不同步长采样结果不同）。且**不要拿"撤销"当卖点**——L125 是"笔刷家族无 Undo"的既定裁决，笔画栈的真实收益是"改分辨率不丢/可换通道语义/可迁移"。
- **L5. D13 target 种子**：队友指控的"改密度即整面重掷"**不成立**（`FRandomStream` + 逐点拒绝采样、N 只作循环上界时序列是**前缀稳定**的：N 增大只在尾部追加、减小只截断，已有点逐位不变；N 没有进种子）。剩下两条：① **前缀稳定不是自动的，是实现约束** —— 若把 N 提前用于预分配、或把间距写成"抽到冲突就整批重抽"、或把 openings 剪影剔除后**补齐到 N 个**，L725 自己承诺的"bound 不变则逐位复现"立刻失效；② **D12 L654（位置哈希、不用序贯流）与 D13 L725 是两条相反的稳定性纪律，计划没说为什么** —— 照 D12 去"修"D13 会直接破坏 L723 的用户裁决。**最该做的是补一段文档说明这个区别，成本为零。** 可选的代码级改法（与裁决完全兼容）：种子构成一字不改，但用它去**旋转一个无状态低差异序列**（R2，正是 TG L261 用的）而不是驱动序贯流。

---

### 四、不建议改的（看似该抄、其实不该抄）

1. **拱洞的 PS discard 本身 —— 如果你决定要碰撞。** 报告 §7.3 把"不需要 CPU 拿到切完的真实几何"列为 TG 方案成立的最要命前提，TG 能满足是因为碰撞（parry 查询世界 L352）与光追代理（L378）都是**另建的**。本项目今天恰好满足（全线 NoCollision 是既定架构），但这条前提一旦改变，clip 路线立刻塌。**不过要说清：布尔路线在这一点上一样不解决问题**（L761 明写不加 `UBodySetup`），这不是 clip 引入的债。
2. **`DormerHole` 那种 PS 循环遍历全局 SSBO**（报告 L197，144 行 depth PS）。UE 的 deferred + Nanite 环境下成本模型完全不同，材质编辑器表达"遍历结构化缓冲"很别扭。老虎窗开洞在 UE 里应该走几何或独立组件。
3. **平面裁剪用来解"屋顶与墙相交"或"接缝角柱穿插"**。它只对**薄片**成立；对闭合实体会露出没有 cap 的内壁。而且 L556/L562 已由你裁决用邻近合并解决并明写"比裁剪简单得多""不需要谁缩掉谁半个厚度的对称协商"。
4. **删掉 `DoorWidthQuantum` 让门宽"真正连续"**。它是这条链上唯一的节流器，删掉会让 flush 频率**上升**（详见 2.3）。计划 L222 的"拱会呼吸"说的是段长随 N 浮动，与它无关。
5. **把 `EditMeshAsync` 整栋异步化从 P2 阻塞验收门（L740）降级为可选优化**。N 刀布尔归零后仍剩两次 `EditMeshSync` + `EnsureCapacitySync` + 首次 `EnsureIndirectDrawCapacitySync`（`Private/CSMesh.cpp:1357-1380`），且 L765 本来就把它单列为与布尔无关的风险。降级一条你拍板过的阶段门本身需要你同意。
6. **"删掉布尔就解锁 P6 窗户"**。P6 验收门 L744 列了七八项独立前置，布尔操作数只是其中一项的下游。
7. **为 Masked early-Z 预先建"槽位分裂 + 三层哈希"**。proxy 重建的真实判据是材质列表（`Private/CSMeshRenderComponent.cpp:280`）不是 section 表，队友设计的 `LitSetHash`/`ArchShapeHash` 多半在解决一个不存在的问题。**先测**（换一个 OpacityMask 恒 1 的 Masked 材质，零代码），代价显著时再加 slot 3，那是几行的事。
8. **把砖改走引擎 `UInstancedStaticMeshComponent` + Nanite**。它与"避免派生数据进存档"的论证自相矛盾（`PerInstanceSMData` 同样是序列化数组），且放弃了 D9 已跑通、你裁决为"必须在 GPU 线程中进行"的零回读散布形态。降格为待真机验证的选项。
9. **把植被拆出 `ACSGroundDecorItem` 父类**。L600 是"放置物是 actor"的用户裁决，L715 自己划的界是"父类字段不变，**只换渲染宿主**"。批准的是换宿主不是拆父类。
10. **把 D12 的场翻转成 CPU 权威**。L662 标了用户裁决，且 L617 说的是"上 GPU 能省掉写局部累加这个优化"而不是"CPU 算不动"。要重开该先量数据。
11. **删掉 `CachedOthersMesh` 单刀增量与 `UCSMeshPool` 借还**。只要 L10 还没被正式推翻，它是死代码但不是错设计；L754 记录的是"算子当前没有操作数入口"这一实现受阻，不是裁决被撤销。
12. **把 target 改存墙参数空间、种子里去掉面长宽**。直接推翻 L723/L725 的用户裁决——种子不含长宽后 bound 变化时点只拉伸不重掷，正是你明确不要的语义。
13. **为洞口/窗框做"任意表面搜候选点"的杂物放置**（TG 的 `add_birdnests` 那一半）。依赖 parry 查询世界，本项目没有碰撞基建，也不该为它引入。
14. **现在迁移 marching squares**（见 M13）。
15. **为藤蔓做真·逐帧增量生长**（见 M14）。

---

### 五、如果只做三件事

**1. 修 `Private/CSHouseActor.cpp:401-402` 的内脸 UV bug + 给 `DoorSlotOpen` 与 `Mirror` 各加一个 `UPROPERTY(NonTransactional)` 和 `PostEditUndo`。**
三条都是当下就在生效的正确性缺陷，合计不到 30 行，零架构风险：一个是"屋里看拱上方一片纯色"的显示 bug，两个是"存盘/撤销后世界状态静默改变"的数据 bug（`Public/CSHouseActor.h:237`、`Public/CSGroundActor.h:241`）。顺手补两条 automation 断言（不允许退化 UV 三角；存盘-重开后开拱数不变）。

**2. 抽 `CSHouseRoof_EvalZ` 与 `CSHouse_ArchProfile` 两个纯函数，并把材质绑定从哈希守卫里提出来（`ApplyMaterials()`）。**
零行为变更、零渲染风险，但它们是后面一切的地基：屋面求值器是铺瓦/铺梁/D8 谓词的共同引用点（TG L203/L205 明确警告过不共享会脱开），拱剖面是 clip 判据 / 拱腹几何 / L406 重叠谓词的同式保证，而 `ApplyMaterials` 修的是"细节面板换材质完全不生效"这个当下缺陷。做完这一步，第 3 件事才有安全的落点。

**3. 把 clip 路线打包成提案交给用户拍板（#1 墙板 q 场 + #2 `RevealMeshComponent` + #3 推翻 L10），三条必须一起上。**
摆三样东西：① L10 否掉旧方案的**理由**（"贴不住剪影、洞缘漏缝"）在 clip 上严格优于布尔；② 报告 §7.3 的三个前提本项目全中，尤其"CPU 不需要看见切完的几何"是既定架构（`Private/CSMeshRenderComponent.cpp:74` + L761 + L418）；③ 代价与新债一条不漏——洞口内壁必须补几何、形状扩展从"加资产"变成"改材质 + 加 C++ 剖面"、材质图成正确性关键路径且项目零 Masked 先例、`DoorWidthQuantum` 必须保留、P2 门 L740 与 P6 其余七项**不会**因此消失。缺任何一条，这就是个残缺方案。

阴影（M2）排在这三件之后：它与开洞方案完全正交，且动的是 `SubmitGpuBufferDraw` 这个 public static 的签名 + `CSMesh` 的 section 语义，改动半径最大，值得先花 30 分钟真机实测 VSM 对 direct draw 的行为再排期。

---

<a id="round-2"></a>

## 卷二 · 第二轮：未覆盖维度

> 原文件 `TinyGlade_对比逆向报告_第二轮.md`，原标题「Tiny Glade 对比逆向报告 · 第二轮（未覆盖维度）」。

> 8 个维度 × (对比 agent + 对抗核验 agent)，18 agents。合成 agent 的输出在传输时被截断，本文由各核验 agent 的结构化返回直接重建，末尾附合成 agent 存活的部分（GI 压力测试 / 不建议改 / 三档归类）。

### 存活改进项索引

- **楼梯系统_当前方案的完整空白（对抗性核验后）** — confirmed 6 / rejected 5
- **选中高亮_编辑反馈_换色换主题的零重建原则** — confirmed 5 / rejected 4
- **碰撞与空间查询基建** — confirmed 5 / rejected 7
- **交互形态：画线成墙 vs 放房子拉尺寸** — confirmed 6 / rejected 4
- **规模上限与性能预算的量化对照** — confirmed 6 / rejected 5
- **nani架构_GPU驱动实例渲染层** — confirmed 5 / rejected 4
- **资产格式与顶点语义_渲染风格的物质基础** — confirmed 6 / rejected 3
- **光追GI代理_光照与阴影的一致性** — confirmed 5 / rejected 3

---

## HIGH

### 洞的记录形式只有"沿边一维区间 + 从墙基起算的高"，装不下"底在半空 / 顶随坡倾斜 / 切轴非墙法线"——楼梯穿墙与窗台高度共用这一个判决点

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §1.3（标题即【确凿 + 局部推测】，461 行文件的第 70-74 行）：`playermade_stairs_add_wall_holes`（楼梯穿墙）与 `construct_gates`（门/拱）、`add_hole_at_shape_intersection`（形状相交）三家生产者进**同一张** `WallHoles` 表（`resources/walls/wall_holes.rs`：`WallHoles::{add, clear_for_next_frame}`、`WallHoleStorage::{new_in_progress, finalize, clone_and_reopen}`、`FinalizedWallHoles`、`HoleChecksum/HoleType/HoleOrigin`，全部 [SYM] 标注），下游消费者三家：`trim_rows` 裁砖（§1.2 第 62 行）、plaster 的 `mirror_holes_as_needed`、`half_timbering`。**报告没有给出 TG hole struct 的字段表**——"洞需要二维矩形 + 斜切轴"是【我的推断】，由"玩家自由绘制的楼梯穿墙处标高/倾角由楼梯几何决定"推出，不是报告结论。

**当前方案怎么做**：**原条目的行号引错了**：`FCSWallOpening` 不在 D8（:391-410 是叙述段），而在 **D4 的代码块 Plan_House.md:158-170** ——`{ECSOpeningType Type; ECSOpeningShape Shape; int32 EdgeIndex; float CenterS; float Width; float Height; FGuid SourceId;}`，其中 :166 逐字写着「洞高（**从墙基算**）」，:169 写着「cutter = 原型静态网格按 (Width, Height, 墙厚+两侧出头) 缩放后摆到 (EdgeIndex, CenterS) 处」——**摆位参数里没有任何竖向自由度**。判重叠在 :406 与 :431：同 `EdgeIndex` 上 `[CenterS ± Width/2]` 区间比较；cutter「沿墙法向切透」在 :387。已落地实现更窄：`FCSHouseDoor{EdgeIndex, CenterS, Width, Height}`（Public/CSHouseActor.h:17-25）；墙体按 `AddSegment(Cursor, S0); Cursor = S1;` 发实心盒（Private/CSHouseActor.cpp:364-373，lambda 在 :364-368），门跨内从 z=0 到拱曲线**一概不发几何**（拱上墙带在 :375-410），洞恒定坐在墙基上。

**差距**：三条硬假设里，**两条确凿成立、一条要修正**：①【成立】洞底恒 = 墙基——`Height` 从墙基算（计划 :166），摆位参数无竖向项（:169），代码里门跨下方零几何（CSHouseActor.cpp:369-373）。②【需修正】原条目说"洞在墙空间是一维区间、连 Height 方向都不比"——**过强**：计划 :406 明写「需要更严时**再比 Height 方向**（拱是下身矩形 + 顶半圆，剖面解析可写）」。真正的缺口不是"不比高"，而是**没有洞底这个自由度**，所以"比高"只能比"谁更高"，比不出"高窗在门上方、两者不冲突"。③【成立】cutter 轴 = 墙法线（:387）。
楼梯穿墙三条全破：底在半空（= 该处踏步标高）、顶随坡度倾斜、切轴 = 楼梯行进方向（与墙法线成任意角，穿厚墙时内外两面洞位不同）。**同一条假设让 D8 的窗台高度也表达不出来**：窗的竖向位置只活在标记的世界 transform 里，一进 openings 表就丢（:169 的摆位参数不含它），而 :431 的每帧谓词只比同边 S 区间，门上方的高窗会与门判成重叠。这不是"将来的楼梯问题"，是 D8/P6 落地当天的问题。

**建议**：在 `FCSWallOpening` 上加**洞底 + 切轴**两组自由度（改计划 :158-170 的结构定义，落地时进 `Public/CSHouseTypes.h`，见计划 :74 的文件清单；今天的 `FCSHouseDoor` 是它的退化子集）：
```cpp
float Z0 = 0.0f;                 // 墙空间洞底（门 = 0，行为逐字不变；窗 = 窗台高）
float Z1 = 0.0f;                 // 洞顶（= 今天的 Height，语义从"高度"改成"顶标高"）
FVector2f AxisUS = {0.0f, 1.0f}; // 切轴在 (U, In) 平面内；(0,1) = 沿墙法线，今天的行为
float Skew = 0.0f;               // Z1 沿 S 的线性斜率——楼梯洞的倾斜顶
```
**不动 `CenterS/Width`**（原条目改成 `S0/S1` 是无谓的重命名，且会让 :406/:431 已写死的"`[CenterS ± Width/2]` 膨胀比较"整段作废）。消费侧改动量：`RebuildBodyMesh`（CSHouseActor.cpp:369-373）把门跨从"整跨不发"改成"门跨下方补一条 z ∈ [0, Z0) 的裙墙盒"——一次 `Writer.AddBox`；`AxisUS != (0,1)` 时拱腹侧带从 `In*T` 换成沿 `AxisUS` 展开的斜棱柱，仍走同一个 `AddQuad`（:404）。`ComputeDoors`（CSHouseActor.cpp:186-254）填新字段时门恒 `Z0=0, Skew=0, AxisUS=(0,1)`。
**注意与 :182 的布尔路线的关系**：计划的正式路线是 openings 逐个 `ApplyMeshBoolean` 减 cutter（:182），当前 `RebuildBodyMesh` 是有意偏差的参数化生成（:754 ①）。加 `Z0/Skew/AxisUS` 对布尔路线**同样必要且更省**——cutter 原型摆位多三个参数而已，不新增原型。

**代价**：结构加 4 个字段 + `ComputeDoors` 多填常量 + `RebuildBodyMesh` 加一段裙墙，约 80 行；无新 UE 设施、无 GPU 改动、无回读。**必须同步把新字段并进 `BodyDescHash`**（CSHouseActor.cpp:247-252 的两段 `H.Append`，门那一行在 :251-252），否则改窗台高度不触发重建——这是本仓已有前科的坑（材质指针不在哈希里，改材质不重建）。时机是全部代价：D8/P6 动工前改是纯加字段；等窗和楼梯都写完再改要动布尔 cutter 摆位 + 谓词 + 一次序列化迁移。

**证据**：TG 侧：§1.3 三家生产者与洞表 API 为【逆向报告确凿】(第 72 行 [SYM])；**TG hole struct 字段表报告未给**，"需要二维矩形 + 斜切轴"是【我的推断】。项目侧全部【代码事实】，行号已逐条复核并修正（原条目把 D4 的结构定义误引成 D8 的 :391-410）。**与第一轮维度①（门洞与开洞机制）的边界**：①比的是"逐像素 discard vs MeshBoolean"即切法，本条比的是**记录 schema 的自由度**，两者不重叠；本条不对切法表态。

---

### 外观量骑在几何重建入口上：地面改底色 = 一次纯空转的 13 万三角全量重建 + 全场唤醒；房屋改材质则永远绑不上

*维度：选中高亮_编辑反馈_换色换主题的零重建原则*

**Tiny Glade 怎么做**：§8.3【确凿】`system_color::set_wall_color_id / write_roof_color_id_to_gpu` 直接写 `WallColorIds/GateColorIds` SSBO，报告原文即「改色不重建 mesh」，shader 按 `source_id` 查表。§1.6【确凿】砖 VS 按 `flags&2` 决定查 `roof_color_ids[]` 还是 `wall_color_ids[]`——颜色是绘制期一次查表。
【核验修正】原稿说颜色「与砖实例数组完全解耦」是过头的：§1.4 的 `source_id` 注释里写明「bit31 置位时低 4 位内嵌色号」，色号索引本身就在 96 字节实例里。准确表述是：**色表写入不重建 mesh，但色号的载体仍是逐实例字段**。

**当前方案怎么做**：计划全文 grep「材质/颜色变更代价」= 0，无用户裁决可推翻。代码侧（全部逐行复核）：
- 地面 `Private/CSGroundActor.cpp:528-541`：`PostEditChangeProperty` 把 `GroundMaterial`(:537) 与 `BaseColor`(:538) 和 `NumCellsX/Y`、`CellSize`、`UVWorldPeriod` 并列进同一个 `bShapeProperty`，命中即 `RebuildGroundMesh()`(:540)。`RebuildGroundMesh` 在 :169-181 = `BuildSnapshotFromMirror`(:107-167，全量 CPU 重建 257²=66049 顶点/393216 索引) + `UploadTinyGladeSnapshot`→`CopyFromMeshSnapshot`(阻塞 flush) + `OnGroundChanged.Broadcast`(:180，唤醒全部房屋 `ReevaluateSite`)。
- **`BaseColor` 这一路是纯空转**：镜像 `Colors` 只在 `EnsureMirrorInitialized` 的尺寸变更分支(:93)与 `ResetPaint`(:197) 里重新盖章；尺寸没变时 :78-86 提前 return，镜像原封不动；`BuildSnapshotFromMirror` 读的是镜像(:145-146)。所以改 `BaseColor` **画面 0 变化**，代价却是整条重建链。
- 房屋：材质绑定唯一发生在 `Private/CSTinyGlade.cpp:27-28`（`SetMaterial(slot,·)` + `MeshMaterial=`），只被 `RebuildBodyMesh()`(`CSHouseActor.cpp:456`) 调用，而 `RebuildBodyMesh` 受 `BodyDescHash` 守卫(:312-318)，哈希输入在 `ComputeDoors()` 的 :245-253，**无任何材质/颜色项**。`ACSHouseActor` 无 `PostEditChangeProperty`(grep 0)，只有 `OnConstruction`(:501-505)→`ReevaluateSite`。→ 改 `WallMaterial`/`RoofMaterial`(`Public/CSHouseActor.h:84,87`) 构造脚本重跑但哈希不变 → 不重建 → 材质永远不生效，必须手点 `RebuildHouse()`(:331-338) 清哈希。柱子 :490-491 同受柱哈希守卫。
- 补充事实：`ResolveBatchMaterials`(`CSMeshRenderComponent.cpp:169-205`) 在 `NumSections>0` 时**根本不读 `MeshMaterial`**(:178-183 是 0 section 的分支)。房体经 `BuildMaterialSections`(:461) 有 2 个 section，所以只改 `MeshMaterial` 对房子无效，必须走 `UCSMesh::SetMaterial`。

**差距**：TG 把颜色放在绘制期查表，与几何链零耦合；本项目把两个纯外观量各自焊死在几何链的两端，且**方向相反**：地面「不该重建却重建（还是次空操作）」，房屋「该重绑却不重绑」。这不是性能优化题，是两个正确性缺陷。等真有了材质资产、用户开始调底色/换墙面，地面每拖一下滑块 = 一次全量重建 + 全场房屋唤醒。

**建议**：三步，都是既有正规通路，不新增机制：
1. **地面 `GroundMaterial` 摘出 `bShapeProperty`**（删 `CSGroundActor.cpp:537` 那一行），改调 `TinyGladeMesh->SetMaterial(0, GroundMaterial)` + `TinyGladeMeshComponent->MeshMaterial = GroundMaterial`。`UCSMesh::SetMaterial`(`Private/CSMesh.cpp:844-851`) 会 `NotifyMaterialsChanged` 广播 → `UCSMeshRenderComponent::HandleMeshChanged`(:242-297) 重算 `BatchMaterials` 后 `RecreateRenderState_Concurrent`(:288)。该函数自己的注释(:293-296)写明「在这个 leaf 里重建就是重绑——无分配、无 compute」。
2. **地面 `BaseColor` 也摘出**（删 :538），且**不要**像原稿那样新造 `RestampBaseColor`：重新盖章会抹掉已画的道路权重，语义上不该由属性编辑触发；「按底色重铺」的显式动作 `ResetPaint()` 已经是 CallInEditor 按钮（`Public/CSGroundActor.h:173-174`，实现 `.cpp:192-199`）。正确做法是把 `BaseColor` 的注释(`CSGroundActor.h:95-97`)补一句「只在镜像重置时生效，改它不刷新画面，要重铺按 ResetPaint」，然后让它什么都不触发。
3. **`ACSHouseActor` 补一个 `PostEditChangeProperty`**：`WallMaterial/RoofMaterial` → 在 `ACSTinyGlade` 上把 `UploadTinyGladeSnapshot` 的 :27-28 抽成 `protected void BindTinyGladeMaterials(const TArray<TObjectPtr<UMaterialInterface>>&)`，上传路径与材质变更路径共用它；`PillarMaterial` → 把 `CSHouseActor.cpp:490-491` 那两行同样抽出来单调。几何参数才走 `ReevaluateSite()`。

**代价**：约 30 行，无新 shader、无新流、无新资产。三条 UE 侧代价：① `SetMaterial` 触发一次 proxy 重建（重绑级，无分配、无 compute），改材质是低频操作可接受，但**绝不能**把这条路用在悬停/高亮那种每帧路径上；② 抽出 `BindTinyGladeMaterials` 后要注意 `UploadTinyGladeSnapshot` 每次重建都会重写 `Materials[0]`——任何存进该槽的 MID 会被下一次 `RebuildBodyMesh` 覆盖（见 findings 末条）；③ 房体是 2 section，改 `MeshMaterial` 无效，必须走 `SetMaterial(slot,·)`，否则症状是「改了没反应」而无任何报错。

**证据**：TG 侧【逆向报告确凿】(§8.3/§1.6)，但已剔除原稿「与实例数组完全解耦」的过度表述（§1.4 明写色号内嵌在 source_id 低 4 位）。项目侧 100% 代码逐行直证，无推断。

---

### footprint 只有 FVector2D，五处硬编码的 4；但结论应为「不做画线、把 footprint 升级为闭合折线」——且 TG 本身就有形状放置模式，这条比原稿更站得住

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.1：交互链 `ui_draw_wall` → `CurveNursery`(PointInNursery) → `RecordMouseInputIntoWall` → `draw_wall` → `finish_wall`，墙权威数据是 `utils::curve::Curve`（30+ 方法含 try_resample/smooth_weighted/project_point）；§1.2 `WallConstructor::from_curve` 只吃曲线。

【核验修正一，原稿漏掉的关键反证】TG 并不是「只能画线」：§8.5【确凿】把「墙体形状放置吸附（`shapes::shape_placement`）」列为 collision 查询世界的首条服务；§10.2【确凿】`systems/wall/` 下 `shapes` 与 `snapping`、`merge_walls`、`wall_splitter` 并列；§3.1【确凿】屋顶装配只有 `assemble_rectangular_roof`（矩形→gable）与 `assemble_circular_roof`（圆→锥）两条；§8.3【确凿】`construct_timberframe_circle/rectangle`。**矩形/圆形状放置在 TG 里是一等公民**——「放一个形状再调」并不反 TG。

【核验修正二】原稿「房子是曲线闭合的副产物」在报告里无原文支撑，属【你的推断】且与 §3.1（屋顶是独立编辑对象、独立 crate `system_roof`、独立 `CreateRoofCmd`）相抵触，已删。

【逆向报告确凿】§3.1 PDB grep 'skeleton' 零命中；§9.5 lyon/geo/splines/delaunator/earcut 在 Cargo.lock + PDB 双零命中。

**当前方案怎么做**：计划 `Plan_House.md:221`（核验无误）：「门的逻辑只认**边缘线段**（P0/P1/外法线），不认『矩形四面墙』——矩形房 = 4 条底边线段；将来多边形 footprint、接缝角柱的接触段都是线段，复用同一函数」。

代码侧该抽象确实不存在（全部行号已逐条核对）：`Private/CSHouseActor.cpp:97-109` `CSHouse_GetEdge(int32, const FVector2D&, float)` 是矩形专用 4-case switch，东西两面靠 `F.Len = Footprint.Y - 2*T`（:104/:106）硬编码消转角重叠；`ComputeDoors` 的 `for (int32 Edge = 0; Edge < 4; ++Edge)`（:200）；`RebuildBodyMesh` 同（:353）；`ComputePillars` 的 `Corners[4]` + `(E+1)%4`（:270-278）；`ComputeSeatZ` 是 NX×NY 矩形网格双循环（:170-181）；屋顶经 `bLongX` 二选一（:417）。footprint 唯一表示是 `Public/CSHouseActor.h:57` 的 `FVector2D FootprintSize`。

【核验修正三】原稿把这写成「计划在骗人」，过火。计划 :226 的进度注记明确写了本次只是「房-地交互切片」，并逐条列了相对 P2/P3 的两处有意偏差；:221 是 D6 的**规格陈述**，不是「已实现」的声称。降级为「规格与实现之间的一笔未偿技术债」。

**差距**：落差不在「计划撒谎」，在于**五处独立的 `4` 分布在五个不同函数里，没有任何一处是单点**。任何多边形化都要同时改 5 处 + 换一份数据表示；在此之前，计划 :791（原稿误引为 :788）「接缝角柱上过路开拱：接触段本身就是一条边缘线段，D6 的分割/点亮纯函数直接复用」在今天的代码上做不到——`CSHouse_GetEdge` 的入参就是 `FVector2D`，接触段根本喂不进去。D7 一旦动工必然长出第二套边表示。

更重要的是**方向判断**：本轮维度问的是「要不要改画线」。核验后结论比原稿更强——TG 自己就有 rect/circle 形状放置 + snapping（§8.5/§10.2/§3.1/§8.3 全为【确凿】），所以「放房子拉尺寸」不是对 TG 的妥协版，而是 TG 两种建造模式中的一种；而画线在本项目的实现成本是断崖式的（见 proposal ③）。真正该补的是 footprint 拓扑，不是输入方式。

**建议**：**判断：不把「画线成墙」做成主交互；把 footprint 从 `FVector2D` 升级为闭合折线，交互仍是 gizmo + handle。**

三条理由：① TG 的曲线权威性带来的观感（任意 L 形/圆形院落）由「多边形 footprint + 顶点 handle」即可给，与 D5 已裁决的『handle actor + 标准 gizmo』完全兼容——D5（:204-215）裁的是拖拽方式，从未裁定 handle 只能是四个面 handle；② TG 的屋顶本身也只有 rect/circle 两条装配路径（§3.1【确凿】），当前双坡屋顶与 TG 的 rectangular roof 同一水平，曲线化在 TG 也不是全链条的；③ 画线的项目侧成本：`PCGEditorProcess` 零 `HHitProxy`/零 `UInteractiveGizmo`/零 `UInputBehavior`（grep 实证），唯一交互底座 `FCSBrushEdModeBase` 还主动关掉 transform widget（`Private/CSBrushEdModeBase.h:71-74`，四个 override 全返回 false/None，已核对原文）；而 TG 的画线背后有 history 重放撤销（§10.3 第 3 条【确凿】）兜底，本项目 TinyGlade 相关模块零 `FScopedTransaction`（**修正原稿**：全插件并非零命中，`EditorShortcuts/Private/EditorShortcutSubsystem.cpp:171-172` 有两处，但与本链路无关），画错一笔无法回退，体验会**低于**当前的拉尺寸。

改造（可独立于任何曲线化先做，作为 D6/D7 的前置）：
1. 新增 `USTRUCT FCSHouseFootprint { TArray<FVector2D> LocalLoop; }`（CCW 闭环、局部系），构造器 `MakeRect(FVector2D)`；`FootprintSize`（`CSHouseActor.h:57`）保留为 UPROPERTY 但语义降级成「矩形快捷方式」，`OnConstruction` 里同步进 LocalLoop——这样 D5 的单边推拉与现有蓝图/关卡资产（`/PCGPlugins/HouseTest/BP_TinyGladeHouse`）一行不用改。
2. `CSHouse_GetEdge` → `CSHouse_BuildEdges(const TArray<FVector2D>& Loop, float T, TArray<FCSHouseEdgeFrame>& Out)`：逐边给 P0/P1/U/In（由环绕序定）/Len；转角内缩从写死的 `-2*T`（:104/:106）一般化成斜接量 `T / tan(θ/2)`，矩形代入即得原值，写成回归断言。
3. `ComputeDoors`(:200) / `RebuildBodyMesh`(:353) / `ComputePillars`(:272) 的 `4` 换成 `Edges.Num()`；`Corners[4]`(:270) 换成 `Loop`。
4. `ComputeSeatZ`(:170-181) 的矩形网格换成「环 AABB 网格 + `UE::Geometry::TPolygon2<double>::Contains`」——`Engine/Source/Runtime/GeometryCore/Public/Polygon2.h:351`（已核对引擎源码），`ComputeShaderGenerator.Build.cs` 的 PrivateDependencyModuleNames 已列 `GeometryCore`（已核对）。
5. 凹多边形（L 形）的双坡屋顶是这条路上唯一的硬骨头。**TG 的答案是【你的推断】**——报告未直说「一栋建筑挂 1..N 个屋顶」，但 §3.1 屋顶是独立编辑对象 + §3.3【确凿】存在 `inter_roof_merlons`（屋顶间雉堞）合起来强烈暗示这一形态。按此办：一个 footprint 挂 1..N 个独立矩形屋顶，**不做直骨架**（§3.1 已确凿 TG 没有）。

**代价**：约 250 行 C++，全部在 `CSHouseActor.{h,cpp}` 内；纯函数部分（BuildEdges、斜接内缩）直接进计划已规划的 `CSHouseLogicTests`。UE 侧无新依赖（`GeometryCore` 是 `Engine/Source/Runtime` 下的引擎模块，无插件 target 限制，见 Finding「曲线工具」）。

风险两处：① 常驻流是世界空间，`RebuildBodyMesh` 每一处 `Writer.AddBox/AddQuad/AddPrismTri` 都依赖 `U×In=+Z` 的口径（`CSHouseActor.cpp:87-94` 注释「已验」；绕序细节见 :67-84 的 AddPrismTri 注释），凹角处环绕序判错会让整面墙法线朝内，而 `bVerifyUsedMaterials=false` 下是**静默不画**，必须配一条「所有面法线与包围盒中心夹角 > 0」的单测；② D5「单边推拉」在多边形上无定义（推哪条边、相邻边怎么跟随），动工前须冻结「矩形用面 handle、多边形用顶点 handle」这条产品裁决。

**不推翻任何用户裁决**：D5 裁的是「用标准 gizmo 拖 handle actor」，从未裁定 footprint 必须是矩形。

**证据**：【逆向报告确凿】§1.1 / §1.2 / §3.1 / §8.3 / §8.5 / §9.5 / §10.2 / §10.3；【逆向报告推测→已剥离】「房子是曲线闭合的副产物」无原文支撑，已删；「一栋建筑挂 1..N 屋顶」标为【你的推断】。【代码事实（已逐行核对）】CSHouseActor.cpp:97-109/:104/:106/:170-181/:200/:270-278/:353/:417、CSHouseActor.h:57、CSBrushEdModeBase.h:71-74、ComputeShaderGenerator.Build.cs；【引擎源码已核对】Polygon2.h:351。【计划】:221 / :226 / :204-215 / :791（原稿误引 :788，已改）

---

### D5 handle 挂在房子下 + 单边推拉改中心 = 墙走位是鼠标位移的 2×（递推式已给出）；但原稿「松手前持续蠕动几十帧」不成立，已剥离

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.1：TG 里被拖的对象就是曲线采样点本身（`CurveNursery` 的 `PointInNursery`），拖点 = 直接改权威数据，不存在「手柄是被编辑物的子级」这种回路。§3.1【确凿】TG 唯一的 handle 类交互是屋脊（`ui_edit_roof_ridge_dir/_length/_tip`），而屋顶是**独立编辑对象**、不是墙的子级。

即：TG 从未把「手柄」做成「被编辑物的子 actor」，因此从没遇到过本条的父子回路。这个对照是成立的。

**当前方案怎么做**：[`Plan_House.md` D5](Plan_House.md#d5-拉尺寸实体-handle-actor--标准-gizmo用户裁决) 三条规定（下称 **规定①②③**；原文此处引的 `:208/:210/:211` 是旧行号，D5 已移位，改用小节锚点）：①「生成后 `AttachToActor` 挂在房子下，房子整体移动/旋转时跟着走」；②「handle 的 `PostEditMove` 把自身当前位置投影到所属墙的外法线轴，算出推拉量后直调宿主房 **`PushEdge(EdgeIndex, Offset, bFinished)`**……房屋**单边推拉该墙（对侧不动、中心随动**——Tiny Glade 行为，clamp `MinFootprint`，默认 200 cm）」；③「拖拽期间（`bFinished=false`）程序**不回写** handle 位置」。

⚠️ **原文这里写的 `NotifyHandleDragged(WallIndex, Offset)` 这个名字从未存在**：2026-08-31 机制入口是以 `ACSHouseActor::PushEdge(EdgeIndex, Offset, bFinished)` 落地的（`WallIndex` 在那边叫 `EdgeIndex`）。**D5 交互层已于 2026-09-05 落地**（`ACSHouseResizeHandleActor` / `EnterResizeMode` / `OnResizeModeChanged` / `FCSHouseResizeSelectionWatcher`），本条的 2× 缺陷**按下面的修法 (A) 记账量法实装并单测钉死**（`House.ResizeHandle` 第 ②③ 段：单次拖 1 m 走 1 m，连续 10 步每步恰好 25 cm）。所以它确实是"动工前免费修掉"的 —— 一次都没在画面上发生过。

**差距**：【你的推断，递推已闭合；需一次 10 行实验确认 gizmo 增量语义】

「对侧不动、中心随动」= `FootprintSize` += Offset 且 actor 中心沿该墙外法线移动 Offset/2。handle 是子级，父级移 Offset/2 会把 handle 世界位置一并带走 Offset/2；而 handle 的「规范位置」（墙外皮中心，:208）随该墙移动 Offset。设第 n 次 `PostEditMove` 开始时残差 `e_n = handle位置 − 规范位置`，本次鼠标增量 δ：

  Offset = e_n + δ；规范位置 +Offset；handle 因父级 +Offset/2
  e_{n+1} = e_n + δ + Offset/2 − Offset = (e_n + δ)/2

稳态 `e* = δ`，于是**每次事件墙实际走 Offset → 2δ：墙的位移是鼠标位移的 2 倍**。这条成立。

**【核验：原稿两处必须剥离】**
① 「松开鼠标前墙会持续蠕动几十帧」**不成立**。δ=0 时 `e_{n+1}=e_n/2` 确实是几何衰减，但鼠标不动就没有 gizmo 输入事件，`PostEditMove` 根本不会再触发——不会有自发蠕动。残差在 `bFinished=true` 时被 :211 的「统一摆回规范位置」清掉。
② 由 ① 派生的「拱阵会在鼠标已停下之后仍重排若干帧」随之作废。拖拽**过程中**因墙长跨过 `N = round(Usable/DoorPitchTarget)`（`CSHouseActor.cpp:206`）取整边界而重排，是预期行为（计划 :234 已列为已知项），不是本条的症状。

剩下的真实症状：拖 1 m，墙走 2 m；且拖拽期每次事件都触发一次全量 `RebuildBodyMesh`（`CSHouseActor.cpp:343`）。

**建议**：**修法 (A)：记账量法（推荐，~15 行，与规定③逐字兼容）**
`ACSHouseResizeHandleActor` 上加 transient `FVector LastConsumedWorld`。`PostEditMove` 里 `Offset = FVector::DotProduct(GetActorLocation() - LastConsumedWorld, OutNormalWorld)`；调完 `PushEdge`（此时房子已改完 `FootprintSize` 与中心，attach 已把 handle 拖走）**之后**立刻 `LastConsumedWorld = GetActorLocation()`。代入上面的递推：Offset ≡ δ，1× 行为，且**只更新一个记账变量、不写 handle transform**，与规定③「拖拽期不回写 handle 位置」完全一致。

⚠️ **累加器要记 `PushEdge` 的返回值，不是喂进去的 `Offset`**：它返回经 `MinFootprint` 修正后**实际**生效的位移。顶在下限上时记成请求值，残差会在"墙拖不动"的那段时间里一路累积，松手瞬间房子跳一大截。`CSHouseResize.h:57` 与单测第 ③ 例已钉死这条。

**修法 (B)：去掉父子关系 —— 【核验后需加限定，原稿说它「不推翻规定③」是错的】**
原稿写「handle 不 attach，由房子在 `PostEditMove` 与 `ReevaluateSite` 末尾统一摆放全部 handle」。但 handle 拖拽本身会经 `PushEdge` 走进 `ReevaluateSite`，于是房子会在拖拽中写正在被拖的那个 handle 的位置——**直接违反规定③**。若要用 (B)，必须限定为「摆放除当前被拖那一个之外的全部 handle」，并在房子侧记录 `ActiveHandle`。这使 (B) 的复杂度反超 (A)。**建议直接选 (A)。**

**配套单测（无论选哪条）**：✅ **已落地（2026-08-31）** —— 纯函数 `CSHouse_ApplyEdgePush(FVector2D& InOutSize, FVector& InOutCenter, int32 EdgeIndex, float Yaw, float Offset, float MinFootprint)` 在 `CSHouseResize.h`，单测 `FCSHouseEdgePushTest`（`House.EdgePush`）三例：①「连续 10 次 Offset=0 不改变任何量」；②「单次 Offset=Δ 后对侧墙世界位置不变、被推墙恰好移动 Δ」，**四条边 × yaw=37° 各验一遍**（中心随动是世界方向的量，只在 yaw=0 下验是最常见的漏法）——这条正好把 2× 放大钉死；③「推穿 `MinFootprint` 时返回实际生效位移而非请求值」。无头回归侧另有 `demo_house_resize()`：60 步 × 5 cm 断言 `jumps == 0`。

**代价**：(A) ~15 行 + 一条单测；是 D5 动工前的设计修正，零返工。

不修的代价：P4 验收门 :742「拖 handle 实时改尺寸、松手 handle 回位」会以「拖 1 m 墙走 2 m」的形式失败，而症状（拖拽期高频重建、门拱随墙长重排）与计划风险节 :235「门洞抖动」、:237「每帧重建的 flush 代价」高度相似，极易误诊到别处。

**UE 侧限制**：`PostEditMove` 是 `WITH_EDITOR` 的；PIE 游玩外壳（开放问题 :779）需自写同样带累加器的拖拽路径——与上一条 Finding 的累加器纪律是同一条，写在同一处头注释即可。

**不推翻任何用户裁决**：D5 裁的是「实体 handle actor + 标准 gizmo」和「拖拽期不回写 handle 位置」，(A) 两条都守住。

**证据**：【逆向报告确凿】§1.1 / §3.1（TG 无 handle 间接层）；【你的推断】父子回路递推 `e_{n+1}=(e_n+δ)/2` 完整，需一次实验确认 UE gizmo 的增量语义；【已剥离】「松手前持续蠕动几十帧」与派生的门拱抖动——无输入事件即无 PostEditMove。【代码事实】CSHouseActor.cpp:206/:227/:308/:343；【计划】:208 / :210 / :211 / :234 / :235 / :237 / :742 / :779

---

### [CONFIRMED，理由更强] 交互期同步预算：每次 EditMeshSync = 一次 FlushRenderingCommands；画一笔 1 次、每栋房重建 2 次，TG 同等操作 0 次——且计划 :767 已明令禁止，当前笔刷实现违反了它

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 的 CPU→GPU 上行全部是持久 SSBO 的增量子区间写，无设备同步点：墙砖 `SparseInstanceBuffer<BrickTransformSsbo>::gpu_flush`（§1.4）、nani `nani_flush_instance_updates`（§2.1）、笔刷栅格 DirtyTiles → `copy_to_staging` → `upload_raster_staging_to_texture` compute（§5.1）。§1.1【确凿】明写「画线进行中与抬笔定稿走完全相同的重建管线」——每帧全量重排整面墙仍零同步，因为上传是 buffer 子区间写。每砖 96 B（§1.4【确凿】），砖 draw list 桶步长 2^21（§1.5【确凿】）。

（原稿的「单墙约 480 砖 ≈ 46 KB / 144k 三角」是【我的推断】的量级估计，报告无此数，仅作数量级参照，已降权。）

**当前方案怎么做**：`UCSMesh::EditMeshSync` = `ENQUEUE_RENDER_COMMAND` + **`FlushRenderingCommands()`**（CSMesh.cpp:948-957，flush 在 :957）。全部 `UCSMeshOps` 算子经它。实测计数：
- `ACSGroundActor::ApplyPaintStroke`（CSGroundActor.cpp:217-233）每次鼠标移动**无条件**调 `PaintVertexColorsSphere` = 1 次 flush；该算子 `ThreadCount = VertexCapacity`、dispatch 全网格（CSMeshOps.cpp:1379/1401/1404-1406）。
- `ACSHouseActor::RebuildBodyMesh` = `UploadTinyGladeSnapshot`→`CopyFromMeshSnapshot`（CSMeshOps.cpp:1129 EnsureCapacitySync + :1130 EditMeshSync）+ `BuildMaterialSections`（CSMeshOps.cpp:1488 + :1498）= **稳态 2 次 flush**（`EnsureIndirectDrawCapacitySync` 在 CSMesh.cpp:1357 够用即早退，不额外 flush；`EnsureCapacitySync` 在 CSMesh.cpp:1107 同理）。`RebuildPillarMesh`（CSHouseActor.cpp:492）再 1 次。

**原稿一处需订正**：并非「计划里没有给笔刷路径定过同步预算」。计划 :767 已把纪律写死——「**交互热路径（拖标记、拖 handle、画笔刷、拖拽期间的实时布尔）：一次回读都不许有，禁止任何 `*Sync` 算子**……每次 `*Sync` 是一次 GT 停顿」；:765 也已点名 `CopyFromMeshSnapshot` 是同步 flush。所以这不是「计划的空白」，是**当前代码对已定纪律的不合规**，性质更重。计划缺的只是一条可测的计数口径。

**差距**：开销不在 GPU 算力：1024² 地面全网格球刷也只有 16,417 个 wave；房体快照 736 三角 × 112 B ≈ 82 KB。100% 的代价是 flush 次数。

最坏情形：20 栋房的村庄上画一笔穿过的路，某个鼠标 tick 里门集合翻转 → 1（笔刷）+ 20×2（房体）+ 20×1（柱）= **61 次 FlushRenderingCommands 在一个游戏线程 tick 内**，每次都要等渲染线程排空。稳态（哈希短路）也仍有每帧 1 次笔刷 flush——它不是 60 个 16 ms 停顿，而是**彻底取消 GT/RT 流水并行**，帧率被钉在「GT 时间 + RT 时间」的串行和上。对照 TG：同一操作设备同步 0 次。

同一文件里已有正确形态的先例：`DisplaceGroundShapers`（CSMeshOps.cpp:1414-1459）带 `RegionMin/RegionMax`、`ThreadCount = RegionW*RegionH`（:1425）；紧邻的 `PaintVertexColorsSphere` 是全网格。

**建议**：三步，从便宜到贵：

**(1) 给 `PaintVertexColorsSphere` 加区域参数（约 20 行）。** 照抄同文件 `DisplaceGroundShapers` 的形态：加 `FIntPoint RegionMin/RegionMax` → `FUintVector4 GroundRegion` + 行主序 id 解码（地面顶点布局 `Id = y*NumVertsX + x` 已是约定，CSGroundActor.h:22），`ThreadCount = RegionW*RegionH`。默认 `BrushRadius=300` / `CellSize=50` ⇒ 13×13=169 格点：256² 从 66,049 → 169，1024² 从 1,050,625 → 169。**注意这一步只省 GPU 线程数，不省 flush**——它的真实价值是让 1024² 配置下的 dispatch 不再随地面尺寸线性膨胀。

**(2) 落笔路径去掉 flush（核心）。** `ApplyPaintStroke` 只做：写镜像（本来就是权威，CSGroundActor.h:19-20）+ 把笔刷矩形并进新成员 `StrokeGpuDirtyRect`；新增 `FlushPaintToGpu()`，由 `FCSGroundPaintEdMode` 每帧调一次，内部走 `UCSMesh::EditMeshAsync`（CSMesh.h:511）。被 in-flight 拒时（CSMesh.cpp:976）把脏矩形合并进下一帧——计划 D4 :199 已为房子写好这条 pending 合并纪律，直接复用。落笔路径 flush：每帧 1 → 0。

**(3) 房体：`CopyFromMeshSnapshot` 与 `BuildMaterialSections` 必须进同一个 `EditMeshAsync`。** 计划 P2 :740 已列此项，但**只把上传异步化是 2→1 而不是 2→0**，必须点名后者也进去。**一条原稿漏掉的硬约束**：`BuildMaterialSections` 的 `bSorted` 标志在 lambda 体内置位（CSMeshOps.cpp:1495-1497 注释「Set inside the edit, read after EditMeshSync's flush」），随后 :1567 之后才 `SetSections`。异步化时 lambda 体在渲染线程录图时执行，游戏线程会在它置位前就读到 false ⇒ **`SetSections` 必须搬进 `EditMeshAsync` 的 `OnComplete` 游戏线程尾巴**，否则症状是「房子永远只画一个材质」且无任何报错（`bVerifyUsedMaterials=false`，代理不校验）。

**验收门（建议并入 P1/P2 :739/:740）**：把 :740 现有的「profile 断言无 flush」落成可测口径——一次落笔 + N 栋房复评期间 `FlushRenderingCommands` 调用数 = 0。

**代价**：中。(1) 局部，风险只有 kernel 里 id 映射写错（症状是画不上，不崩）。(2)(3) 要处理 `EditMeshAsync` 的三条契约（计划 :198-200 已写全）：EditFunc owned 必须 `MoveTemp`；在途时 `EditMeshSync` 会阻塞到两者都跑完，故 `EnsureCapacitySync` 须预留足量、`ComputeWorldBoundsSync` 一律禁用（`CopyFromMeshSnapshot` 在 CSMeshOps.cpp:1126-1127 已用 CPU 全扫算 bounds，本来就不需要它）。(3) 额外多一条 `SetSections` 搬位的正确性风险。副作用：GPU 顶点色比镜像滞后 ≤1 帧（纯视觉，所有查询走镜像）。不推翻任何用户裁决——这是对计划 :767 已定纪律的合规修复。

**证据**：【代码事实】CSMesh.cpp:948-957（flush 定义）、:976（异步在途拒绝）、:1107/:1357（两处容量早退）；CSMeshOps.cpp:1379/1401/1404-1406（全网格球刷）、:1129-1130（上传 edit）、:1488/:1498（分段 edit）、:1495-1497 + :1567（bSorted 与 SetSections 的同步依赖）、:1414-1459（已有的区域 dispatch 先例）；CSHouseActor.cpp:456-461/:492；CSGroundActor.cpp:217-233。【计划】:765、**:767（交互热路径禁 `*Sync` 的已定纪律）**、:740（P2 验收门）、:198-200（异步三约束）。【我的推断（由代码直算）】房屋三角数按 CSHouseActor.cpp:57-84 逐面直算（AddBox=12 三角，AddPrismTri=2+3×2=8 三角，每拱 12 段 ×(4 三角 + 2 quad)=96 三角）：无门房 4×12+2×12+2×8=88 三角，6 拱房 88+6×96+2×36=736 三角；常驻 112 B/三角（3 顶点 ×32 B + 3 索引 ×4 B + matid 4 B）。【逆向报告确凿】§1.1、§1.4、§1.5、§2.1、§5.1。

---

### [WEAKENED，结论保留、论据订正] 世界变换被写进房体哈希 → 落座每次都全量重建；计划 :181 的增量平移路径因此永不可达，而 TransformMesh 早已存在

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 的砖实例携带 `Affine3Packed transform`（§1.4），**变换本身就是实例数据**，改姿态 = 改 48 字节，不触碰任何网格；整条渲染层没有「世界空间常驻顶点」这个概念。TG 没有「移动一栋房子」这个操作，最近语义是墙曲线变 → `construct_walls` 重排该墙砖（§1.3 系统签名【确凿】；「全量重排该墙砖」报告自标为【待确认】）。

**当前方案怎么做**：计划 D4 :181 写明正确做法：「仅 Z 变而房体 desc 未变时走 `TranslateMesh(ΔZ)`——一个位置 pass，不重建、不跑布尔」。

实现堵死了它：`ComputeDoors()` 的 desc 哈希把**量化世界变换与几何参数、门集合拌在同一个数组**（CSHouseActor.cpp:247-250，前 4 项是 `CSHouse_Q(Loc.X,1)/Loc.Y/Loc.Z,0.5/Yaw,0.1`）；:246 与头文件 CSHouseActor.h:239 的注释都已承认「房子一动必然重建」。`ReevaluateSite`（:312-317）只有二值判断 `BodyHash != BodyDescHash` → `RebuildBodyMesh()` 全量。**落座（:303-309）本身就改 Z**，容差 0.5 cm、哈希量化也是 0.5 cm，所以「地形抬 1 cm → 落座 → 全量重建」是常态路径。`ComputePillars` 同病（:291 把世界变换 append 进柱哈希）。

**差距**：`UCSMeshOps::TransformMesh`（CSMeshOps.h:329，实现 CSMeshOps.cpp:1276-1305）**已存在且比计划要求的更强**：一个 compute pass 同时改 Positions 与 Tangents（`SrcNormalToWorld` 用逆转置，:1281）并把 `Resident.WorldBounds` 变换过去（:1300-1301）；`TranslateMesh` 只是它的薄包装（:1307-1310）。平移与 yaw 旋转都已支持。

量化差（默认 600×400 房、6 拱共 736 三角 / 2,208 顶点）：现状 = 重跑全部几何写入 + 再打包一遍 2,208 顶点 + `GraphBuilder.Alloc` 第三份复制（CSMeshOps.cpp:1140-1142）+ ~82 KB 上传 + 材质计数排序三趟 dispatch + **2 次 flush**（柱也变则 3）；增量 = CPU 0、上传 0、1 趟 VertexCapacity 的位置+切线 pass、1 次 flush（异步化后 0）。30 Hz 拖动一栋房：60–90 次 flush/秒 → 30 次轻 pass。

**原稿的论据必须订正（这一条是它最脆的地方）**：原稿称「ShapeHash 的四项本来就是局部量，天然与世界变换无关」——**不成立**。门的存亡由 `Ground->SampleRoadWeight(世界 XY)`（CSHouseActor.cpp:219-221）决定，门宽由 `GapMax = Loc.Z − SampleHeight(世界 XY)`（:223）经 `WidthScale`（:232）连续决定。所以门集合是世界摆位的函数，**水平移动越过路面时门必然翻转、ShapeHash 必然变、增量路径用不上**。拆哈希真正成立的理由是另一条：门集合每次复评都从世界采样**重算**并已作为派生值进了哈希，额外再哈一遍原始变换是**冗余**；冗余去掉后，「变换变而门不变」的情形（纯 Z 落座、地形均匀区域内的小幅平移、`DoorWidthQuantum=2 cm` 量化吸收的抖动）才落到增量路径。收益因此从原稿暗示的「所有移动」收窄到「落座 + 门不翻转的移动」——但落座恰是 D9 塑形物场景下的**常态**（计划 :755 的 `L_TerrainOpsDemo` 抬台 300→400 就是这条）。

**建议**：把 `ComputeDoors` 的单一返回值拆成两个哈希，`ReevaluateSite` 分三路（约 30 行）：
1. **`ShapeHash`** = 几何参数（Footprint/WallHeight/WallThickness/RoofPitch/RoofOverhang/RoofThickness）+ `CurrentDoors` 的 `(EdgeIndex, CenterS, Width, Height)`。**注释里必须写明它不是「局部量」而是「已把世界采样吸收进去的派生量」**，防止后人误以为可以跳过算门。
2. **`PlacementHash`** = CSHouseActor.cpp:248 现在的前 4 项。
3. 新成员 `FTransform BodyBuiltAtTransform`（照抄 `ACSGroundActor::MeshBuiltAtLocation`，CSGroundActor.h:245 + CSGroundActor.cpp:508/:554-562 的用法）。

分派：`ShapeHash` 变 → 全量（现状不动）；仅 `PlacementHash` 变 → `TransformMesh(TinyGladeMesh, NewWorld * BodyBuiltAtTransform.Inverse())`，同一刀作用于 `PillarMesh`，更新 `BodyBuiltAtTransform`；都不变 → 短路。

**顺序纪律（写进代码注释）**：必须**先照常算完门、再比 `ShapeHash`**，绝不能用「位置没变就跳过算门」省事——理由见 gap，门本就是世界采样的函数；而算门很便宜（约 340 次镜像双线性，见反向盘点条）。柱哈希（:291）同拆。

**验收门（建议并入 P4 :742）**：拖动房子 100 帧，断言 `RebuildBodyMesh` 调用次数 = 0（除非门集合真翻转）。

**代价**：小（一个函数拆返回值 + 一个分支 + 一个成员）。三处要核对：① `TransformMesh` 的 dispatch 跨度是 `VertexCapacity` 而非活跃顶点数（CSMeshOps.cpp:1282），房子容量≈活跃量，无影响但要知道；② `EnsureCapacitySync` 只涨不缩（CSMesh.cpp:1107-1113），增量搬运不会造成容量漂移；③ 浮点累积：连续 N 次增量变换会攒误差，需在 `PostEditMove(bFinished=true)` 做一次全量重建对齐——`ACSGroundActor::PostEditMove`（CSGroundActor.cpp:543-565）已经是这个模式（注释：「拖动期间增量平移，松手后全量重建对齐，顺带把累计浮点误差清零」），照抄即可。不推翻任何裁决：计划 :181 本来就要这条路，本条只是说明它为什么不可达，并把范围从「仅 Z」扩到「平移+yaw」、把收益范围如实收窄。

**证据**：【代码事实】CSHouseActor.cpp:247-250（哈希拌入世界变换）、:246 与 CSHouseActor.h:239（作者已知的注释）、:303-317（落座改 Z 后立刻比哈希）、:219-223（门依赖世界采样——**推翻原稿「局部量」论据的直证**）、:232（宽度随 GapMax 连续变）、:291（柱同病）；CSMeshOps.cpp:1276-1310（TransformMesh 已支持位置+切线+bounds）、:1140-1142（第三份复制）；CSGroundActor.cpp:543-565（增量平移 + 松手重建的既有模式）。【计划】:181。【逆向报告确凿】§1.4；§1.3 的「全量重排」报告自标【待确认】，原稿引用时未标注，已订正。

---

### 方向裁决闸：自定义实例路径的唯一立论前提（"GPU Scene 只能由 CPU 填"）在 5.6+ 已不成立——且引擎里有三个可抄的参考实现

*维度：nani架构_GPU驱动实例渲染层*

**Tiny Glade 怎么做**：§2.1/§2.2【确凿】：nani 自建 GpuWorld（`[RwLock<Option<GpuWorldSubset>>;27]` + HashSet 脏跟踪 + 恰好 20 个 `nani_update_*` + `nani_flush_instance_updates`）、两阶段 HZB 遮挡剔除、1024 桶深度计数排序 + 32 实例合并窗口。§1.6【确凿】砖 LOD0 = 600 顶点/300 三角。§2.4【推测，基于确凿证据推理】：nani 与 Nanite 的"本质区别"是它只有整实例粒度——无 cluster DAG、无连续 LOD、无可见性缓冲、无软光栅，报告把这条明确标为推测。
【我的推断】报告并没有写"nani 是被迫自建的"，那是我从 rhapsody 是自研 Vulkan 渲染器这一事实推出的；同样，"§2.4 的区别清单在 UE 语境下不成立"也是我的推断：UE 侧 depth prepass 顶掉了深度桶排序买到的 early-Z，`FInstanceCullingMergedContext`（Renderer/Private/InstanceCulling/InstanceCullingMergedContext.h:9）顶掉了 32 实例合并窗口买到的命令数。

**当前方案怎么做**：立论写在一处：`Source/ComputeShaderGenerator/Private/CSGpuInstancedMeshVertexFactory.h:18-19` —— "...which is always on SM5 — and GPU Scene instance data can only be filled from the CPU"。据此 `.cpp:28` 强制 `USE_INSTANCING`、`.cpp:102-108` 注册时不带 `SupportsPrimitiveIdStream`，代理走 dynamic relevance。连带代价全部记在案且都已核对：`CSGpuInstancedMeshComponent.cpp:65-66` `CastShadow=false; bUseAsOccluder=false`；`CSGpuMeshSceneProxy.cpp:26` `bSupportsDistanceFieldRepresentation=false`；`CSGpuInstancedMeshComponent.h:154` "no per-instance collision, no ray tracing, no static lighting, no per-instance custom data"。计划全文（含 D9/D12/D13）确实没有讨论过这条路线的方向问题。

**差距**：那句立论在 UE 5.7.4 是过时的，且**比原稿说的更可行**——原稿说"公开但无参考调用者"，这一条经复核为假：
- API：`Engine/Public/SceneInterface.h:560` `virtual void UpdatePrimitiveInstancesFromCompute(FPrimitiveSceneDesc*, FGPUSceneWriteDelegate&&)`；`Engine/Public/MeshBatch.h:129-130` `DataWriterGPU/DataWriterGPUPass`；`Renderer/Public/GPUSceneWriter.h` 全 Public。
- **三个现成调用者**（复核推翻原稿）：`Plugins/FX/NiagaraNanite/.../NiagaraStaticMeshComponent.h:15-59` 是一个 `UNiagaraStaticMeshComponent : UStaticMeshComponent`，暴露 `UpdateInstanceGPU(NumRequiredInstances, TFunction<void(FRDGBuilder&, const FGPUSceneWriteDelegateParams&)>)`，实现在 `.cpp:210-241`——几乎是本议题的模板类；`Plugins/PCG/.../PCGInstanceDataInterface.cpp:423` 是完整范例（含容量检查 `GetInstanceSceneDataOffset()==-1` 重试、`RegisterExternalBuffer` 接 GPU 产物、custom float data）；`Plugins/FX/Niagara/.../NiagaraRendererMeshes.cpp:1163-1193` 走 `FMeshBatchDynamicPrimitiveData` 单帧形态。
- **"计数只有 GPU 知道"不是障碍**：`Plugins/PCG/Shaders/Private/PCGSceneWriter.usf:36-39,63-68` 的做法是 CPU 只给容量，shader 读 GPU 上的 `InWriteCounters`，尾部实例写 `WriteInstancePrimitiveIdAndFlags(..., INSTANCE_SCENE_DATA_FLAG_HIDDEN)` 剔掉。本组件的容量本来就是 CPU 已知的棘轮（`CSGpuInstancedMeshComponent.cpp:482`）。
买到 GPU Scene 之后白拿的正是逐条放弃掉的：逐实例前帧 HZB 遮挡剔除（`InstanceCullingContext.cpp:34-38` `r.InstanceCulling.OcclusionCull`，默认 0、ECVF_Preview）、`InstanceCullingOcclusionQuery.cpp` 的逐实例软遮挡查询、VSM 阴影、Lumen/距离场/光追、per-instance custom data、hitproxy 与选中。

**建议**：**只做步骤 1，它是一道闸；本轮第 2、3、5 条全部以它的结论为前置。**
步骤 1（1–2 天 spike）：照 `UNiagaraStaticMeshComponent`（`NiagaraNanite/Private/Renderer/NiagaraStaticMeshComponent.{h,cpp}`）新建 `UCSGpuSceneInstancedMeshComponent : UStaticMeshComponent`，抄它的 `UpdateInstanceGPU` 三段式（容量不足 → `RecreateRenderState_Concurrent` → `Scene->UpdatePrimitiveInstancesFromCompute`）。散布产物一行不改：`CSShaperSteps::FPaletteBuffers::{PackedInstances, Counter}`（`CSGroundShaperSteps.h:26-28`）用 `RegisterExternalBuffer` 接进委托，委托里跑一个照 `PCGSceneWriter.usf` 改写的 CS，把 5×float4 的行翻成 `InitializeInstanceSceneDataWS` 的入参，尾部 `INSTANCE_SCENE_DATA_FLAG_HIDDEN`。接入点选 `ACSGroundShaperActor::EnsureStepComponents`（`CSGroundShaperActor.cpp:398-419`）+ 交源处（`:504-513`），加一个 `bUseGpuScenePath` 开关并存两条路。
验收三条：① 石阶画出来；② `r.InstanceCulling.OcclusionCull 1` 后被土台挡住的石阶被剔；③ 组件 `CastShadow` 打开后有 VSM 阴影。任一条不过就退回现路线，并把结论改写进 `CSGpuInstancedMeshVertexFactory.h:18`（把"can only be filled from the CPU"改成"5.6 起可以，我们试过，卡在 X"）。
**同时写死"不做"清单**（这四项在 UE 里没有正收益）：① nani 的 1024 桶深度计数排序——UE 有完整 depth prepass；② 32 实例合并窗口——对应物是 `FInstanceCullingMergedContext`，不归我们管；③ 自建两阶段 HZB——定价见代价栏；④ NaniTrimeshChunk 指针网格块（见本轮第 4 条）。

**代价**：spike 1–2 天。**与用户裁决不冲突**：计划 `Plan_House.md:452`「石阶散布全在 GPU 线程上（用户裁决"必须在 gpu 线程中进行"）」在这条路上完全保留——`FGPUSceneWriteDelegate` 就是在渲染线程的 RDG 图里跑，散布 CS 一行不动，换的只是目的地缓冲。
三条真实限制（原稿的"无参考调用者"风险已被证伪，代之以这三条）：
(a) **基础网格必须是 `UStaticMesh` 资产**。GPU Scene 路径没有"运行时生成的基础网格"入口，除非先烘一份资产（阻塞回读，撞计划 `:690-698` 的零阻塞回读纪律）。所幸今天两个真实消费者都用资产基础网格——石阶 `SetBaseMesh(StepMeshes[Index])`（`CSGroundShaperActor.cpp:419`）、点刷同理——`SetBaseMeshFromGpuData`（`CSGpuInstancedMeshComponent.h:199`）全仓库**零调用者**（已 grep 核实），所以这条限制今天不咬人。
(b) `r.InstanceCulling.OcclusionCull` 默认 0 且 `ECVF_Preview`，开了要自己压回归。
(c) 字段语义（`PackedInstanceSceneDataFlags` / `InstanceSceneDataOffset` / `NumCustomDataFloats`）只能照 `PCGSceneWriter.usf` 与 `GPUScene/GPUSceneWriter.ush` 反推，无独立文档。
**遮挡剔除的定价（本条给的是量级估算，非实测）**：当前只有视锥 + 距离，零遮挡（`Shaders/Private/CSGpuInstancedMesh.usf:76,91-97`，全文无 HZB/深度采样），且距离剔除**默认关**（`CSGpuInstancedMeshComponent.h:268` `InstanceEndCullDistance = 0.0f`）。但**本计划的房子不是砖实例**（D4/D6 是闭合实体 + 布尔开洞，千级三角一整块），今天的实例负载只有石阶（十级实例）与点刷（百到千级），decor 按 `:600` 用户裁决是 actor 不是实例。总实例三角在 1e5 量级 ⇒ 遮挡剔除能省 **<0.1 ms**，**单看今天的收益不足以支撑 spike**；支撑它的是阴影 + Lumen + custom data 这三项一起。重估阈值：`:787` 开放问题「门洞装饰升级：Tiny Glade 式砖拱/门框砖沿洞缘排布」落地，或 `:782` 开放问题「decor 按 class 降级为 ISM」落地后实例数上万——而那时正确做法仍是打开 `r.InstanceCulling.OcclusionCull`，不是自己写 HZB。

**证据**：【逆向报告确凿】§2.1/§2.2 的 GpuWorld/27 subset/两阶段 HZB/1024 桶/32 窗口；§1.6 砖 300 三角。【逆向报告推测】§2.4 与 Nanite 的区别（原稿标注正确，无"把推测当确凿"）。【我的推断】"nani 被迫自建"、"该区别在 UE 下不成立"、遮挡剔除的 ms 级定价。引擎侧与项目侧所有 API/行号均已回原文件逐行核对；原稿"引擎里没有任何一个参考调用者"经复核为**假**，已按事实改写。

---

### 逐图元 36 float 参数通道（CustomPrimitiveData）已全链路接好，两个自定义 proxy 只差把 nullptr 换成指针；且因绝对变换，材质今天连"物体空间"都拿不到

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§1.4【逆向报告确凿，GLSL 直接给出】每砖 96 字节 InstancedWallData 带 seed / flags / global_arch_height_vals / wallspace_x_range；§1.6【确凿】VS 据此做 seed&3 的 90° 随机旋转、flags&4 随机胀缩、世界一致性 value-noise、拱压扁、墙空间 UV，颜色 id 按 flags&2 查 wall_color_ids[]/roof_color_ids[] SSBO。§3.2【确凿】屋瓦逐实例带 clipping_plane/scale_t/rotation_quat/animation_t/seed。§8.3【确凿】"改色不重建 mesh"：system_color 直接写 WallColorIds SSBO，shader 按 source_id 查表。结语第 1 条把它总结为"字典 mesh × 实例参数 SSBO"——**逐对象参数是与顶点属性并列的第二条通道，且承担了大部分风格化**。

**当前方案怎么做**：计划里没有"逐房屋渲染参数"这个概念：房屋可调项全是 UPROPERTY → 进 desc 哈希（`CSHouseActor.cpp:248-252` 房体、`:291` 柱）→ 改了就重建几何。渲染侧 `FCSGpuMeshSceneProxy::SubmitGpuBufferDraw` 在 `CSGpuMeshSceneProxy.cpp:120-121` 调 `FDynamicPrimitiveUniformBuffer::Set(..., ReceivesDecals(), false, false, nullptr)`——最后那个实参正是 `const FCustomPrimitiveData*`（引擎签名 `SceneManagement.h:1613-1622`）；实例化 proxy 同样传 nullptr（`CSGpuInstancedMeshSceneProxy.cpp:654-655`）。另：`UCSMeshRenderComponent` 构造里 `SetUsingAbsoluteLocation/Rotation/Scale(true)`（`CSMeshRenderComponent.cpp:77-79`，注释写明"makes local space equal world space"），常驻流是世界空间 ⇒ LocalToWorld 恒等。

**差距**：(a) 36 个 float 的逐图元通道（`FCustomPrimitiveData::NumCustomPrimitiveDataFloat4s = 9`，`SceneTypes.h:38-39`）在这两个 proxy 上**已经全链路接好，只差把 nullptr 换成 `GetCustomPrimitiveData()`**。我逐环节核对了 UE 5.7.4 源码：proxy 成员由基类构造从组件拷入（`PrimitiveSceneProxy.cpp:293`，访问器 `PrimitiveSceneProxy.h:1208`）；`SetCustomPrimitiveDataFloat` → `SetPrimitiveData`（带 memcmp 短路，`PrimitiveComponent.cpp:2556-2562`）→ `UpdateCustomPrimitiveData`（`PrimitiveComponent.cpp:2568-2576`）→ `FScene::UpdateCustomPrimitiveData`（`RendererScene.cpp:1938-1951`）→ 渲染线程 `PrimitiveSceneProxy->CustomPrimitiveData = ...`（`RendererScene.cpp:6411`），**不重建 proxy、不动一根顶点**；`Set()` 把它交给 `FPrimitiveUniformShaderParametersBuilder::CustomPrimitiveData`（`PrimitiveUniformShaderParametersBuilder.h:250-272`）；`FMeshElementCollector::AddMesh` 对有 primitive-id stream 的 VF 走 `AddMeshBatchToGPUScene`（`SceneManagement.cpp:604-614`），该函数**把整份 `FPrimitiveUniformShaderParameters` reinterpret 后灌进 GPU Scene 动态图元**（`SceneRendering.cpp:5654-5668`），而 `CustomPrimitiveData[9]` 就是这个结构的成员（`SceneData.ush:75/134`，GPU Scene 取数 `:487`）。`FCSGpuMeshSceneProxy` 用的正是 `FLocalVertexFactory`（`CSGpuMeshSceneProxy.h:5/175/184`，有 id stream，走这条）；实例化那条 VF 无 id stream 且 `PrimID_ForceZero`（`CSGpuInstancedMeshSceneProxy.cpp:690`），shader 直接读 Primitive uniform buffer——**两条路都通**。这直接推翻队友勘察 §2.2 的"CustomPrimitiveData 这条路本来就不通"。(b) 因为绝对变换，材质里 `LocalPosition` 等于世界位置、`ObjectOrientation` 恒等："沿墙面横向的砖列相位""从房底到檐口的竖直渐变""按房子朝向的三平面投影"今天在材质里**无法表达**——而这正是 (a) 该装的东西。

**建议**：1）`CSGpuMeshSceneProxy.cpp:121` 与 `CSGpuInstancedMeshSceneProxy.cpp:655` 把末尾的 `nullptr` 换成 `SceneProxy.GetCustomPrimitiveData()` / `GetCustomPrimitiveData()`。两行。
2）在 `ACSTinyGlade` 上加一个 protected 的 `PushPrimitiveParams()`，与 `UploadTinyGladeSnapshot`（`CSTinyGlade.h:56`）并列，并在头文件里钉死前 12 个 float 的布局：[0..2] 房子原点世界坐标、[3..4] yaw 的 (cos,sin)、[5..6] FootprintSize、[7] WallHeight、[8] hash(HouseGuid) 归一化 seed、[9] 季节/主题 t、[10] 选中/高亮强度、[11] 年久/湿度。`ACSHouseActor::ReevaluateSite()` 末尾无条件调它，且**不进 BodyDescHash / PillarDescHash**（`CSHouseActor.cpp:248-252`、`:291`），所以改它不触发任何重建。
3）材质侧用 Scalar/Vector Parameter 勾 "Use Custom Primitive Data"（或 CustomPrimitiveData 节点取下标）。拿到 [0..4] 就能在材质里恢复物体空间：`LocalP = Rz(-yaw) · (AbsoluteWorldPosition − Origin)`，砖列相位、竖直渐变、墙面三平面全部解锁，**不需要 MID、不需要逐房材质实例、不影响 section 表**。
4）验证成本极低：`L_HouseGroundDemo` 里给房屋组件推一个 float，材质里取出来乘进 BaseColor，看颜色跟不跟随——不跟随说明 GPU Scene 动态图元这条没走通，再退回查 primitive id stream。（顺带一提：队友 §3.3 记的"房子改材质无效 / 地面改材质重建 13 万三角"属变更传播议题，是第一轮维度⑤的地界，这里只做交叉引用，不重复给方案。）

**代价**：两行改动 + 一个基类方法 + 一份材质约定。四条必须写进注释的限制：① 36 float 是硬上限（`SceneTypes.h:38-39`）；② 这条通道**只在 UE 运行期存在、不进存盘 StaticMesh**——`SaveToStaticMesh` 烘出去的资产不带它，静态化的房子会退回白模；③ `SetCustomPrimitiveDataFloat` 每次走一次 render command，但引擎自带 memcmp 短路（`PrimitiveComponent.cpp:2556-2562`），照它的语义只推变了的下标，别每 tick 全量重推；拖拽期每帧刷十几个 float 比重建网格便宜 4~5 个数量级；④ 实例化那条 VF 是 `PrimID_ForceZero`，所以逐图元参数对一个 component 内的**所有实例是同一份**——想要逐实例变量仍然只有 `[3].w` 的 random 一个 float（`CSGpuInstancedMesh.usf` 打包约定）。风险低：不动几何、不动哈希、不动 section 表，做错了最坏是材质读到 0。**不推翻任何用户裁决**——计划全文没有关于逐图元渲染参数的裁决。

**证据**：TG 侧【逆向报告确凿】（§1.4/§1.6/§3.2/§8.3）；"链路已通、只差 nullptr"由我逐环节核对 UE 5.7.4 源码确认（PrimitiveSceneProxy.cpp:293 / PrimitiveSceneProxy.h:1208 / PrimitiveComponent.cpp:2556-2576 / RendererScene.cpp:1938-1951+6411 / SceneManagement.cpp:604-614 / SceneRendering.cpp:5654-5668 / PrimitiveUniformShaderParametersBuilder.h:250-272 / SceneData.ush:75+134+487），链路属【代码事实】，"接上就能在材质里取到"为【我的推断】（未在编辑器实测）。

---

### 房子/地面在 VSM 里画 0 个三角形——阻塞点是「custom args 被 instance culling 覆盖成 NumPrimitives×3=0」，而主 pass 恰好绕过了它

*维度：光追GI代理_光照与阴影的一致性*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.5：墙砖 draw 命令共 **7 槽**，槽 0-3 = 主视图 LOD0/LOD1×普通/高亮，**槽 4-6 = 三个阴影级联**；阴影槽「不受主视锥/可见掩码过滤」，按砖在太阳视图 `view_constants[2]` sample 空间的 AABB 与级联窗口 `[0,1]/[1,2]/[2,7]±r` 重叠**无条件写入**，单阶段无遮挡剔除；pass2 的 reset 只清主视图 4 槽（`gl_GlobalInvocationID.x < 4u`），阴影槽保留。§0 的分层数据流图另有【确凿】表述：`vkCmdDrawIndexedIndirect(gbuffer 变体 + depth-only/shadow 变体两套 PS)`——TG 的间接绘制**本来就同时供给主 pass 与阴影 pass**。结语第 2 条【确凿】：两阶段 HZB 剔除是全局协议，墙砖与 nani 共享 3 级联分桶。

**当前方案怎么做**：`Private/CSMeshRenderComponent.cpp:84` 硬写 `CastShadow = false;`，注释（:82-83）理由「GPU-Scene instance culling overrides the custom indirect args in the Virtual Shadow Map pass, so indirect-drawn meshes cannot cast VSM shadows」；`Private/CSGpuInstancedMeshComponent.cpp:62-66` 抄了同一句。计划 `Plan_House.md` 全文对阴影/Lumen/距离场/光追**零字**（grep `阴影|shadow|lumen|距离场|光追` 只命中 :594 一个 C4458 变量改名注记）。项目 `Config/DefaultEngine.ini:52` `r.Shadow.Virtual.Enable=1`。

**差距**：注释把结论写对了一半、机制写错了，导致这条路被当死路封存。**核验后的完整机制链（引擎源码直证，逐行核过）**：
1. `MeshPassProcessor.cpp:1304` `bDoOverrideArgs = SceneArgs.IndirectArgsBuffer != nullptr && MeshDrawCommand.PrimitiveIdStreamIndex >= 0`；命中后 `SubmitDrawEnd` 走 `:1332-1335` 的 `DrawIndexedPrimitiveIndirect(..., SceneArgs.IndirectArgsBuffer, ...)`，**本代理挂在 BatchElement 上的 custom args 被整个换掉**。
2. 替换用的 args 由 `InstanceCullingContext.cpp:250` `AllocateIndirectArgs` 造：`:256` `NumVerticesOrIndices = NumPrimitives * 3U`。本代理 `NumPrimitives` 恒为 0（`Private/CSGpuMeshSceneProxy.cpp:111`）→ IndexCountPerInstance = 0 → **画空，不是画错**。
3. **原报告缺的那块拼图（我补的核验）**：为什么主 pass 今天正常？因为 `SceneArgs.IndirectArgsBuffer` 是否非空取决于 `InstanceCullingContext.cpp:1514` 的 `bUseIndirectDraw = bFetchInstanceCountFromScene || bAlwaysUseIndirectDraws || bForceInstanceCulling || (NumRuns>0 || NumInstances>1)`，而 `:1495` `bAlwaysUseIndirectDraws = (SingleInstanceProcessingMode != EBatchProcessingMode::UnCulled)`。主视图单视图 → UnCulled → 单实例命令 `bUseIndirect=0` → SceneArgs 无 args → `bDoOverrideArgs=false` → 落到 `:1331` 的 else 分支用 `MeshDrawCommand.IndirectArgs.Buffer`，**custom args 存活**。VSM 是多视图（clipmap/page views），`:1457-1461` 把 `SingleInstanceProcessingMode` 改成 `Generic` → `bAlwaysUseIndirectDraws=true` → 覆盖生效 → 画 0。**这才是「主 pass 正常、只有阴影全黑」的精确解释，也说明这不是一条不可解的架构死路。**
4. 路由侧：`ShadowDepthRendering.cpp:2145` 用 `MeshBatch.VertexFactory->SupportsGPUScene(FeatureLevel) ? EShadowMeshSelection::VSM : EShadowMeshSelection::SM` 二选一。房屋/地面走基类 `Private/CSGpuMeshSceneProxy.cpp:185-187` 返回的**裸 `FLocalVertexFactory`**（引擎注册带 `SupportsPrimitiveIdStream`）→ SupportsGPUScene = true → 只能进 VSM 掩码的 shadow info → 必然踩上第 2 条。
观感后果：房子/柱子/地面不投任何阴影，屋檐下无暗部——正是 TG 画面里最显眼的那层。

**建议**：**推荐 S2（唯一推荐）；S1 降级为不推荐的备选。**

**S2：让 house/ground 家族用 CPU 已知计数直绘，从而被 VSM 原生接受。**
① 前提已具备且已核实：`UCSMeshOps::CopyFromMeshSnapshot`（`Private/CSMeshOps.cpp:1036`）末尾调 `AddSetCountersPass`（`:520`），后者 `:543` 调 `Context.SetKnownCounts(VertexCount, IndexCount)` → `FCSMeshResident::KnownVertexCount/KnownIndexCount`（`Public/CSMesh.h:118-119`）对整个 `ACSTinyGlade` 家族都是 CPU 精确值，永不为 INDEX_NONE（只有 `ComputeShaderMeshBoolean.cpp:2200`、`CSMeshOps.cpp:1002`、`CSPointArrowMesh.cpp:203`、`GeometryEditorActor.cpp:2905` 四处会 `InvalidateKnownCounts`，房屋/地面路径都不经过）。
② 在 `UCSMeshRenderComponent` 加默认关闭的 `bDrawWithKnownCounts`，由 `ACSTinyGlade::UploadTinyGladeSnapshot`（`Private/CSTinyGlade.cpp:21-33`）打开；藤蔓/布尔等「只有 GPU 知道计数」的叶子保持关闭。
③ **改动比原方案小**：`SubmitGpuBufferDraw` 已经天然支持这条路——`Private/CSGpuMeshSceneProxy.cpp:107-116` 就是 `if (IndirectArgsBuffer) {NumPrimitives=0} else {NumPrimitives=NumPrimitives}`。所以**不需要给 SubmitGpuBufferDraw 加任何分支**，只需在 `FCSMeshRenderSceneProxy::GetDynamicMeshElements`（`Private/CSMeshRenderComponent.cpp:44-65`）传 `IndirectArgsBuffer=nullptr` + `NumPrimitives = KnownIndexCount/3` + `MaxVertexIndex = KnownVertexCount-1`。
④ 多 section 房体要每批次的 `FirstIndex/NumPrimitives`。`FCSMeshSection::FirstTriangle/TriangleCount` 正常是 INDEX_NONE（`Public/CSMesh.h:59,62`），但房体每槽三角数在 CPU 侧本来就知道——`RebuildBodyMesh` 自己写的 `SlotWall=0/SlotRoof=1`（`Private/CSHouseActor.cpp:349`）；`BuildMaterialSections` 是按槽升序的计数排序，section i 的 FirstTriangle = CPU 侧槽计数前缀和，**零回读可推**。给 `BuildMaterialSections` 加一个可选「CPU 已知每槽三角数」入参填进 section 表即可。
⑤ **必须**补失效触发（见「重建链里没有光照代理更新这一环」那条）：`HandleMeshChanged`（`Private/CSMeshRenderComponent.cpp:242`）今天只比 `AllocationGeneration` 与 `BatchMaterials`（:280），容量够用时重建房体不换 AllocationGeneration，会留下过期 NumPrimitives。把 `KnownIndexCount`（和 section span）并进那个比较。
⑥ 组件侧删 `Private/CSMeshRenderComponent.cpp:84` 的 `CastShadow=false`；`bBatchCastShadow` 默认已是 true（`Public/CSGpuMeshSceneProxy.h:199`）。
**验证**：`L_HouseGroundDemo` 放一盏方向光，目视柱子在地面上的投影 + `stat gpu` 看 ShadowDepth。**S2 不需要动任何 cvar**——这是它相对 S1 的决定性优势。

**S1（不推荐，见 cost）：给 `FCSMeshRenderSceneProxy` override `CreateVertexFactory()`（`Public/CSGpuMeshSceneProxy.h:175` 已是虚函数）返回一个不带 `SupportsPrimitiveIdStream` 的 VF**，照抄 `Private/CSGpuInstancedMeshVertexFactory.cpp:102-112` 的注册方式，批次同时设 `BatchElement.PrimitiveIdMode = PrimID_ForceZero`（坑已由实例化叶子踩过并写注释，`Private/CSGpuInstancedMeshSceneProxy.cpp:684-690`）。

**代价**：S2：改动集中在 `CSMeshRenderComponent.cpp` 的绘制与失效判定、`CSMeshOps::BuildMaterialSections` 加一个可选入参、`CSTinyGlade.cpp` 打开开关——100 行以内，**不动着色器、不新增 shader permutation、不新增也不改任何 cvar**。风险是「计数进了 CPU 侧」违反 `Public/CSMesh.h:56-58` 立的规矩（「draw 永远从 arg set 取计数，否则一张活过 args 的表还会继续画」），所以第 ⑤ 步是**必须**的，漏了症状是画出多余/缺失三角且无日志。
S1 的代价被原方案严重低估，因此降级：(a) 必须把 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps` 设为 0——**默认是 1**（`ShadowSetup.cpp:344-346`），且 Epic 在 cvar 说明里明写 `This variable is deprecated and may be removed in a future version`（`:349`），关掉等于给每盏有 VSM 的灯额外跑常规 CSM 深度 pass，是**项目全局**的性能与版本风险；(b) 新增 VF 类型 = 所有用在 gpumesh 上的材质多一整套 shader permutation，而项目 `Config/DefaultEngine.ini` 开着 `r.Substrate=True`，permutation 基数本来就高，「材质槽全空所以代价近乎为零」的说法只在今天成立、上真材质就不成立；(c) 对房屋/地面它被 S2 严格支配。S1 唯一的独有收益是「藤蔓/道路等只有 GPU 知道计数的叶子也能投影」——那是另一个议题的诉求，不应绑进本条。
**不推翻任何用户裁决**：计划从头到尾没有讨论过阴影。

**证据**：【逆向报告确凿】§1.5、§0 数据流图（depth-only/shadow 变体）、结语第 2 条。【引擎源码直证，本轮逐行核过】`D:/UnrealEngine-5.7.4-release/Engine/Source/Runtime/Renderer/Private/MeshPassProcessor.cpp:1304,1316-1338`、`InstanceCulling/InstanceCullingContext.cpp:250,256,1457-1461,1495,1514`、`ShadowDepthRendering.cpp:2145`、`ShadowSetup.cpp:344-349,2106,2324,4313,4383,5611`、`ShadowRendering.h:105-112`。【项目代码直证】`Private/CSMeshRenderComponent.cpp:44-65,82-84,242,280,292-294`、`Private/CSGpuMeshSceneProxy.cpp:26,107-116,111,133-134,185-187`、`Private/CSGpuInstancedMeshVertexFactory.cpp:102-112`、`Private/CSMeshOps.cpp:520,543,1036`、`Public/CSMesh.h:56-58,59,62,118-119`、`Private/CSHouseActor.cpp:349`、`Private/CSTinyGlade.cpp:21-33`、`Config/DefaultEngine.ini:52`。【我的推断】S2 里「VSM 拿到正确 args 后房子就能投影」逻辑闭环但未实测，须按验证步骤跑一次。

---

### gpumesh 对 Lumen / 距离场 / 光追场景全线隐形——但「实现光追实例」比原方案多两道门槛，且拿不到表面缓存

*维度：光追GI代理_光照与阴影的一致性*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 每类几何都显式维护一份光追代理：prefab 的 `rt` 段 = `mesh:Aabb + 三季 albedo + semantic:Canopy`，配 `RtWorld / RtBvhSsbo / GpuRtTriangle`，BVH 由 obvhs（h3r2tic fork）在 **CPU worker** 构建 CwBvh 后 SSBO 上传、shader 端遍历（§9.3，另 §9.5 第三方库表列 obvhs 用途 = 「RtWorld 的 CwBvh 构建」）；`construct_walls` 的系统签名写 `RtWorld` 并发 `UpdateRtWorldEvent`（§1.3）；楼梯装配 `gen_stairs_collider_and_rt_geo` 一个函数同时产碰撞体与光追几何（§4.1）。§0【确凿】：渲染器 rhapsody 为 kajiya 血统。

**当前方案怎么做**：`Private/CSGpuMeshSceneProxy.cpp:26` `bSupportsDistanceFieldRepresentation = false;`（基类构造，所有 gpumesh 无条件继承）；全插件无 `GetDynamicRayTracingInstances` / `HasRayTracingRepresentation` / `IsRayTracingRelevant` 覆写（grep 0）；`GetViewRelevance` 里 `bDynamicRelevance=true; bStaticRelevance=false`（`:133-134`）。项目 `Config/DefaultEngine.ini:49-59` 开着 `r.ReflectionMethod=1`、`r.GenerateMeshDistanceFields=True`、`r.DynamicGlobalIlluminationMethod=1`（Lumen）、`r.RayTracing=True`，另有 `r.AllowStaticLighting=False`（全动态光照，没有烘焙兜底）与 `r.Substrate=True`。计划对这些一字未提。

**差距**：引擎判定（本轮核过）：`Engine/Private/PrimitiveSceneProxy.cpp` 的 `UpdateVisibleInLumenScene()` 里 `bVisibleInLumenScene = bAffectsLumen && bCanBeTraced`；HWRT 分支要 `IsRayTracingAllowed() && HasRayTracingRepresentation()`，软件分支要 `SupportsDistanceFieldRepresentation() || SupportsHeightfieldRepresentation()`——**两条都不成立**，于是 `Renderer/Private/Lumen/LumenScene.cpp:714` 的 `if (Proxy->IsVisibleInLumenScene())` 永假，图元根本不进 `PendingAddOperations`。表面缓存那条路更是结构性关闭：card capture 注册为 `EMeshPassFlags::CachedMeshCommands`（`LumenSceneCardCapture.cpp:629`）且只遍历 `PrimitiveSceneInfo->StaticMeshRelevances`（`:836,856`），纯动态相关性的代理不可能被捕获。
**严重性被地面放大**：`ACSGroundActor` 也是 gpumesh，`L_HouseGroundDemo` 里三个 actor 对 Lumen 全部隐形，Lumen 光线几乎必然打到天空；叠加 `r.AllowStaticLighting=False`，**连一份烘焙兜底都没有**。实际观感 = 直接光 + 天光常量 + 屏幕空间追踪（gpumesh 在 GBuffer 里，屏幕追踪是唯一命中）：没有墙面反弹到地面的暖色、没有屋檐下的间接暗部、离屏即失、Lumen 反射里房子是个洞。

**建议**：**A→B 分阶段，B 的门槛比原方案多两道；C/D 明确否掉。**

**A（立刻做，代价最低）：先把阴影拿回来**（见上一条 S2）。TG 观感里「屋檐下的软阴影」这一半来自阴影图而非 GI，不需要任何几何代理。做完再评估 GI 还差多少。

**B（GI 主路）：给 `FCSGpuMeshSceneProxy` 实现光追实例——但必须覆写三个虚函数，不是两个。**
① **原方案漏了 `IsRayTracingRelevant()`**：`Engine/Public/PrimitiveSceneProxy.h:456` 默认 **false**，不覆写则代理根本不进光追场景，`HasRayTracingRepresentation()`（`:460`）写了也白写。三件套是 `IsRayTracingRelevant()→true`、`IsRayTracingStaticRelevant()→false`、`HasRayTracingRepresentation()→true`，再实现 `GetDynamicRayTracingInstances(FRayTracingInstanceCollector&)`（`:463`）。
② BLAS 从现有常驻流的位置/索引缓冲建。可行前提已核实：`FCSMeshResident::KnownVertexCount/KnownIndexCount`（`Public/CSMesh.h:118-119`）对整个 `ACSTinyGlade` 家族是 CPU 精确值（由 `Private/CSMeshOps.cpp:543` 填），BLAS 需要的图元数当场就有，**零回读**；位置流的收尾访问态是 `VertexOrIndexBuffer | SRVMask`（`CSGpuMeshTypes.cpp` 的 `FinalAccessForRole`），可直接当 RT geometry 顶点缓冲。
③ **必须下调对 B 收益的预期（原方案说「至少先拿到遮挡」，这句要改）**：即使 B 落地，`GetMeshCardRepresentation()` 仍返回 nullptr（基类默认，`PrimitiveSceneProxy.h` 内），Lumen 会建出 `bValidMeshCards=true` 的 primitive group（`LumenScene.cpp:1217`），但 `LumenMeshCards.cpp:771,888` 拿不到 `FCardRepresentationData` 就建不出任何 card——**HWRT 默认 LightingMode 是 SurfaceCache，命中房子时读的是一块空表面缓存**。所以 B 的实际结果不是「干净的遮挡」，而是「房子从漏光变成可能黑块」，除非同时把 `r.Lumen.HardwareRayTracing.LightingMode` 调到 hit-lighting（很贵）。**B 的定位应改写为：先让房子在 GI 里「存在」，代价是要为它的辐射度另找出口。**
④ 硬边界必须写进文档：`r.Lumen.HardwareRayTracing` 默认 1（`Lumen/LumenHardwareRayTracingCommon.cpp:18-20`），但一旦用户关掉它或机器不支持，软件 Lumen 只认距离场，自定义代理没有距离场资产可给 → **B 在软件 Lumen 上完全不生效**。

**C（不推荐）：烘一份低模 StaticMesh 喂 Lumen/距离场。** 唯一能同时拿到 mesh card + 距离场的路，`UCSMeshRenderComponent::SaveToStaticMesh`（`Public/CSMeshRenderComponent.h:85-87`）现成，但它是编辑器阻塞回读（`ReadbackMeshSync` 会 `FlushRenderingCommands` + GPU 停顿，`Public/CSGpuMeshComponent.h:28-33`），与计划 `Plan_House.md` 风险节「交互热路径一次回读都不许有、禁止任何 `*Sync` 算子」正面冲突。只有做成「松手后台异步烘 + 代数戳」才合规——即计划给提交链开的那条口子（同节「必须异步、且不在交互热路径」）。不作为 v1 路线。

**D（明确否掉）：改用 ISM+StaticMesh 走原生路径。** 整套 `UCSMesh` 常驻流/算子/section 体系就是为「几何只在 GPU、CPU 不回读」建的；为 GI 换回原生渲染路径等于推翻计划第 12 行「公共基类 `ACSTinyGlade`」（用户裁决，已落地）与整条架构。

**代价**：A：见上一条，约 100 行。
B：新增 BLAS 生命周期（建/更新/释放）挂在代理上。显存 = 每三角数十字节；房屋千级三角可忽略，**地面默认 256²=131,072 三角要算，上限 1024²=2,097,152 三角时 BLAS 会很贵，必须按 `NumCellsX/Y` 设闸**。`UCSMeshOps::DisplaceGroundShapers`（`Public/CSMeshOps.h:378`，实现 `Private/CSMeshOps.cpp:1414`）每笔都会动地面几何 → 必须 refit 而非 full rebuild，否则拖塑形物直接掉帧。加上 ③ 的表面缓存空洞，B 单独做完可能观感变差而非变好——**建议 B 与「材质自发光/AO 伪造」或 C 的异步烘焙一起排期，不要单独上线**。
C：与零回读纪律冲突，且带来 masked/几何一致性问题（见下一条）。
**不推翻任何用户裁决**——计划没有关于光照的裁决，这是一片空白。

**证据**：【逆向报告确凿】§9.3、§9.5、§1.3、§4.1、§0。【引擎源码直证，本轮逐行核过】`Engine/Private/PrimitiveSceneProxy.cpp` `UpdateVisibleInLumenScene()`、`Engine/Public/PrimitiveSceneProxy.h:456,460,463` 与 `GetMeshCardRepresentation()` 默认 nullptr、`Renderer/Private/Lumen/LumenScene.cpp:714,1217`、`Lumen/LumenMeshCards.cpp:771,888`、`Lumen/LumenSceneCardCapture.cpp:629,836,856`、`Lumen/LumenHardwareRayTracingCommon.cpp:18-20`。【项目代码直证】`Private/CSGpuMeshSceneProxy.cpp:26,133-134`、`Public/CSMesh.h:118-119`、`Private/CSMeshOps.cpp:543,1414`、`Public/CSMeshOps.h:378`、`Public/CSGpuMeshComponent.h:28-33`、`Public/CSMeshRenderComponent.h:85-87`、`Config/DefaultEngine.ini:49-59`。【我的推断】「B 落地后命中点读空表面缓存」由 card 建立链推出，未实测；「实际观感 = 只剩天光 + 屏幕追踪」同理。

---

## MEDIUM

### openings 表没有第三方注入通路——楼梯不是"标记"，房子对它没有裁决权

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §1.3【确凿】：洞的生产者至少三家，且分属不同系统（gates 在 `system_wall_constructor`、楼梯在 `system_decorator::decorator_visual::stairs` 的 `stairs_assemble`、形状相交在 shapes 侧）；协调靠**表本身**而非生产者互相协商：`WallHoles::add` + `clear_for_next_frame`（每帧清空重收集）→ `finalize` → `FinalizedWallHoles`。归一化在两处独立存在：`WallConstructor::from_curve` 内的 `normalize_holes`（§1.2 第 58 行）与 utils 的 `resolve_hole_overlap`（§1.2 第 62 行，报告的中文注解逐字是「消解洞重叠」）。**"三家互不相识"与"resolve = 归并而非拒绝"都是【我的推断】**：报告只给了符号名与"消解"二字，没给实现；判脏链（`HoleChecksum` + `PrevWallHoles` → `RecalculateWallBricks`）报告自己标了【推测】。

**当前方案怎么做**：现状 `CurrentDoors` 是 `ACSHouseActor` 的私有成员（Public/CSHouseActor.h:234，`UPROPERTY(Transient)`），在 `ComputeDoors()` 内生成（Private/CSHouseActor.cpp:186-254）、在 `RebuildBodyMesh()` 内消费（:360），**外部零写入口**（整个 Public/CSHouseActor.h 无任何 Register/Add 类 API）。计划 D8 的第二个生产者是特征标记，走"标记 attach 到宿主房 → 宿主 `QueryFeaturePlacement` 逐条裁决 → 通过才进 openings"（:381-389, :428），重叠**一律拒绝**：:387「重叠在登记前就被谓词判掉，不依赖布尔去『求并』」、:408「同一处不会被登记两次，也就不会被切两次」——:408 明标为「**用户点明的收益**」。

**差距**："无重叠"是被**安排**出来的：门锚在等分互斥子段上（CSHouseActor.cpp:206-210 的 `N`/`Pitch` 等分），窗被"门优先"谓词挡掉（计划 :383）。楼梯不吃这套——它在世界空间里走，撞到哪就该开哪，而"拒绝"的后果不是"少一扇窗"这种良性 no-op，是**楼梯插进实心墙**。另有两处结构性缺口：① 一段楼梯可能同时穿过两栋房，D8 的"attach 单一宿主 + 无宿主自毁"模型（:376）表达不了；② D6/D8 的洞是房子自己推导或自己裁决的，楼梯的洞是外来物强加的——`QueryFeaturePlacement` 的返回值（:422-428 的 `bAccepted`/`Reason`/`SnappedWorld`）在这里没有语义：楼梯不会因为房子说"不行"就改道。

**建议**：**只做加法，不碰用户裁决**：
1. 提一个最薄的注入接口 `ICSWallOpeningProvider{ void CollectOpenings(const ACSHouseActor&, TArray<FCSWallOpening>&) const; }`，外部 actor 在 `PostRegisterAllComponents` 向房子登记、`EndPlay` 注销——与 `ACSGroundShaperActor` 向地面 `RegisterShaper`/`UnregisterShaper`（Public/CSGroundActor.h:147-148，实现 Private/CSGroundActor.cpp:424-441）**同形、同生命周期纪律**，是本仓已验证过的通路形状。
2. `ReevaluateSite()`（CSHouseActor.cpp:295-329）第 ② 步的插入点在 :312：把 `ComputeDoors()` 一步出哈希，改成 `clear → 推门 → 拉外部生产者 → 哈希`。外部生产者在回调里**禁止**反向调 `ReevaluateSite`——现成的 `bInReevaluate` 守卫（:297-298）已经拦得住，但要写进接口注释。
3. **"相交取并"这条从建议降级为提醒**：它直接改写计划 :387/:408 那条被标为「用户点明的收益」的语义（同处不会被登记两次 → 不会被切两次），我没有推翻它的理由。真正需要的是**给非标记类生产者一条不同的仲裁分支**：楼梯登记的洞按 `SourceId` 稳定排序后**无条件进表**，与它冲突的门槽在 `ComputeDoors` 里被抑制（门本来就是可关的），而不是反过来拒绝楼梯。这样门/窗侧的"重叠即拒绝"一字不改。

**代价**：接口 + 登记表 + 主环插入点，约 80-120 行；`Openings` 全表进 `BodyDescHash`（取代现在只哈希 `CurrentDoors` 的 CSHouseActor.cpp:251-252）。无 GPU、无回读。风险：外部生产者读房子参数、房子读生产者几何，双向依赖必须靠 `bInReevaluate` + "生产者只读不写"约束切断，与 D9 塑形物"只读地面不写"（计划 :460）是同一条纪律。

**证据**：TG 侧：三家生产者与 `WallHoles` API 为【逆向报告确凿】；"互不相识"与"resolve=归并"为【我的推断】（报告未给实现，其判脏链自标【推测】）。项目侧全部【代码事实】。**已按核验降级**：原条目的 `NormalizeOpenings` 取并方案与计划 :408 的用户裁决正面冲突，理由不足以推翻，改为提醒 + 一条不冲突的替代仲裁。

---

### "楼梯扫掠体与哪几面墙相交、进出参数各是多少"这条查询没有任何对应物——它是上面新字段的唯一来源

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §4.1【确凿】：楼梯装配用 parry 的 `iter_shape_overlaps` 算 `calculate_distortion_from_structural_graph`——物理库直接参与 mesh 生成 [SYM]。§8.5【确凿，审查修正版】：`systems::collision::world::{ColliderRegistry, ColliderDiff, RaycastWorld}` 基于 rapier3d 0.22（PounceLight fork）+ parry3d 0.17 的**查询管线**（报告明写"未确认动力学模拟被使用"），同时喂墙体形状放置吸附（`shapes::shape_placement`）、mesh-mesh 相交折线（`intersect_meshes::find_mesh_mesh_intersections`）、鸭子寻路、窗台杂物候选点与楼梯装配形变；空间索引是 `WallSpatialHash / wall_roof_hashmap`。

**当前方案怎么做**：**原条目"项目侧对应设施为零"说得太满，要分三档**：① `PickHouse(Ray)` 确为空白——计划 :184、:368 两次点名依赖，全 `Source/` 树 grep `PickHouse` **零命中**；`UCSHouseSubsystem` 全树唯一命中是 Public/CSTinyGlade.h:21 的一句注释；:754 记着「`UCSHouseSubsystem` 未建」。② **broadphase 计划里已经有了**：计划 :486 逐字写着 `FCSSpatialHash2D Broadphase;   // footprint AABB，格边 ≈ 房屋典型尺寸`，:489-499 给出了完整的每帧顺序（含"接触重算必须在落座之后"这条相位约束）。③ D7 接触判定的规格在 :280-290（`FCSHouseSeam{Point, BisectDir, ExposureA/B, Top, Bottom}`，触发 = 两房 footprint OBB 真正相交 + Z 区间相交，:292 标为**用户裁决**），未实现但**已定死算法归属**（:292「检测与生命周期全部归 `UCSHouseSubsystem` 仲裁」）。gpumesh 全线 `NoCollision`（Private/CSGpuMeshComponent.cpp:14），拾取一律解析（Public/CSTinyGlade.h:23-24 写进基类注释）。

**差距**：楼梯需要的不是"射线打中哪栋房"，而是**"这段楼梯的扫掠体与哪些墙相交、沿墙的进入/离开参数和标高各是多少"**——那正是上面 finding 里 `(EdgeIndex, S0..S1, Z0..Z1, AxisUS, Skew)` 的唯一来源。`RayVsHouse` 给不出它（射线只给一个点），`BoxVsHouse`（D7 那种存在性判定，:496「subsystem 只判**存在性**」）也给不出它——D7 只要知道"该不该有这面墙"，楼梯要的是**参数化的相交区间**。这条在项目侧确实是零，且它不是 `PickHouse` 或 D7 SAT 的退化用例，反过来才对：它是最强的那个，另两个是它的退化。

**建议**：落一个纯解析函数，**挂进计划已定的 `UCSHouseSubsystem`（:463）而不是新起注册表**：
```cpp
struct FCSWallHit { int32 EdgeIndex; float S0, S1, Z0, Z1; FVector2f AxisUS; float Skew; };
int32 SweptPrismVsWalls(const ACSHouseActor&, const FVector& A, const FVector& B,
                        FVector2f HalfSection, TArray<FCSWallHit>& Out);
```
实现直接复用 `CSHouse_GetEdge`（Private/CSHouseActor.cpp:97-109）给出的每墙 `(Start, U, In, Len)` 框架——把它从匿名 namespace 提到 `Public/CSHouseTypes.h`（与上一条 finding 的写手提取同批做，一次改动）；每面墙 = 一块 slab（沿 U 长 `Len`、沿 In 厚 `T`、沿 Z 高 `WallHeight`），楼梯段 = 一条有截面的斜棱柱，求交是 slab-vs-段的区间裁剪，`Z0/Z1/Skew` 从裁剪端点的标高直接读出。粗筛**用计划已有的 `FCSSpatialHash2D Broadphase`（:486）**，不另起线性遍历。
**明确不引入 Chaos / `UBodySetup` / 任何物理库**：TG 用 parry 是因为 rapier 已在依赖里（§9.5）且它的墙是玩家自由画的曲线；项目侧房子是参数化 OBB、墙是四块 slab，解析写法二百行就够，引物理体还要给 gpumesh 造 body setup，正面违反 Public/CSTinyGlade.h:23-24 写进基类注释的"不给网格加碰撞体"。

**代价**：约 150-200 行纯 CPU、无 GPU、无回读、可进计划 :749 已规划的 `CSHouseLogicTests.cpp`。前置：`UCSHouseSubsystem` 要先建（已在计划 :740 的 P2 验收门里）。**不承诺"三处合一"**——`PickHouse` 与 D7 存在性判定各有自己的规格（:184 / :280-292），合并与否是 P2/P5 动工时的实现选择，不该在这里预先裁决。

**证据**：TG 侧【逆向报告确凿】（§4.1 `iter_shape_overlaps` 参与 mesh 生成；§8.5 查询基建服务面与 `WallSpatialHash`）。项目侧【代码事实】。**原条目已按核验削弱两处**：其"项目侧无任何空间结构、建议线性遍历、别上 SpatialHash"与计划 :486 的 `FCSSpatialHash2D` 直接冲突，删除；其"PickHouse / D7 SAT 是同一查询的退化，三处各写一份"是对 :292/:496 的误读（D7 只判存在性），改为只保留扫掠体查询这一条独有缺口。

---

### 选中/悬停反馈全计划零设计；核验推翻了原稿的两条路——真正最省的是引擎原生 EditorSelection 通道（可能今天就已经生效），而「多发一组 FMeshBatch」只有 section 粒度、做不到「这面墙」

*维度：选中高亮_编辑反馈_换色换主题的零重建原则*

**Tiny Glade 怎么做**：§1.5【确凿】墙砖 7 个 draw 槽里 2 个是高亮批（槽 0-3 = 主视图 LOD0/LOD1 × 普通/高亮），push constant bit31 使桶号 +2 —— 零额外几何、零重排。§1.4【确凿，GLSL 结构体直给】`source_id` bit30 = 悬停高亮位（§4.1 的 `frame_constants.highlighted_transient_stair_edge_id` 与之互证）。§1.6【确凿】橡皮擦按 `brush_position` 距离测试逐砖亮橙、`highlight_mode` 混橙/灰。§1.7【GLSL 实证，语义列为推测】flags bit64 = 免编辑高亮。
**关键点**：TG 的高亮有两个粒度——**整批**（draw 槽）与**逐实例**（source_id bit30 / flags bit64）。原稿只对位了前者，而「这面墙」需要的是后者。

**当前方案怎么做**：`Plan_House.md` grep `高亮|悬停|描边|highlight|hover|outline` = **0 命中**（实测）。唯一沾边的是 D8 `:386`「编辑器里以线框颜色区分已生成/被拒」，但标记是无网格 actor(`:389`「标记零几何、零材质」)，那是 debug draw，不在房体/地面上。代码侧同样零：gpumesh 路径无一处 MID（`UMaterialInstanceDynamic` 只在浅水 `ComputeShaderShallowWater.cpp:984-1030`、笔刷球 `CSBrushEdModeBase.cpp:171` 与单测 `Tests/CSGpuMeshObjectTests.cpp:725`）。
**核验新增的三条关键事实（原稿全部漏了）**：
1. **房屋/地面用的是引擎原装 `FLocalVertexFactory`**（`Private/CSGpuMeshSceneProxy.cpp:185-187` `CreateVertexFactory` 直接 `MakeUnique<FLocalVertexFactory>`；只有实例化路径在 `CSGpuInstancedMeshSceneProxy.cpp:267` 覆写成自定义 VF）。所以「自定义 VF 编译不出 pass shader」这个通常的拦路虎在这条链上**不存在**。
2. **引擎的编辑器选中描边对 dynamic 元素只看 `bDrawRelevance`**：`Renderer/Private/SceneVisibility.cpp:2558-2563` —— `if (ViewRelevance.bDrawRelevance) { PassMask.Set(EMeshPass::EditorSelection); }`。`bEditorStaticSelectionRelevance`(:1837) 只管 static 路径，本代理 `bStaticRelevance=false`(`CSGpuMeshSceneProxy.cpp:134`) 走不到那里。而门槛条件（`SceneHitProxyRendering.cpp:965-990`）是 `MeshBatch.bUseForMaterial && MeshBatch.bUseSelectionOutline && (WantsEditorEffects() || (WantsSelectionOutline() && (IsSelected()||IsHovered())))` —— 三者在本代理上**默认全部满足**（`Engine/Public/MeshBatch.h:530,538` 默认 true；`Engine/Private/PrimitiveSceneProxy.cpp:380` `bWantsSelectionOutline(true)`）。
3. **section 粒度 ≠ 部件粒度**：`FCSMeshRenderSceneProxy::GetDynamicMeshElements`(`CSMeshRenderComponent.cpp:59-65`) 每个 arg set 一个 batch，而 arg set == 材质槽。房体只有 2 槽（`CSHouseActor.cpp:350` `SlotWall=0, SlotRoof=1`），**四面墙共用槽 0**。所以「再发一组 batch」最细只能高亮「全部墙」或「屋顶」，做不到「这一面墙」。

**差距**：TG 把高亮拆成「整批换槽」+「逐实例位」两级；本项目一级都没有，而且原稿给的两条路各有致命短板：Custom Depth 需要自写后处理描边材质（引擎原生选中描边根本不走 CustomDepth），多发 batch 只到 section 粒度。等 D5 拉尺寸 handle、D6 门洞、D8 窗标记落地，用户在视口里将看不出「我正在拉哪面墙」「这扇窗被拒是撞上了哪个拱」。与第一轮六维度无重叠。

**建议**：按「先测再写」的顺序，三级：
**(A) 整栋级——0 行代码，先做一次 5 分钟实测。** 在 `L_HouseGroundDemo` 里直接点选 `BP_TinyGladeHouse_C_0`，看视口有没有橙色描边。依据上面第 2 条，链路默认全通，唯一未知是 `GetHitProxyPassShaders`(`SceneHitProxyRendering.cpp:847-868`) 能否为当前材质取到 shader —— `FHitProxyVS::ShouldCompilePermutation`(:62-71) 只为 **special engine material / masked / two-sided / WPO** 材质编译，而现在两个 `*Material` 槽全空、跑的是 `GetDefaultSurfaceMaterial()`=WorldGridMaterial（是 special engine material）→ **大概率今天就已经有描边**。若有：结论是「等自己的墙材质做出来后，把它设成 masked 或 two-sided，描边就白拿」，一行代码都不用写。第二个未知是本代理批次是 `NumPrimitives=0 + IndirectArgsBuffer`(`CSGpuMeshSceneProxy.cpp:107-112`)、且 `bVerifyUsedMaterials=false`(:25)（失败是静默不画而非报错）——实测即答。
**(B) 若 (A) 不通，才考虑 Custom Depth。** `Result.bRenderCustomDepth = ShouldRenderCustomDepth();` 已接线在 `CSGpuMeshSceneProxy.cpp:137`，`SetRenderCustomDepth/SetCustomDepthStencilValue` 从 `UPrimitiveComponent` 白拿（`Public/CSGpuMeshComponent.h:17`）。但要自写描边后处理材质——项目里有 `Content/BasicFunction/Material/M_PP_*.uasset` 两份后处理材质可照抄。这条比 (A) 贵一份资产。
**(C) 逐部件级（这面墙/这个拱/这根墩）——正解不是多发 batch，是逐顶点位 + 一个颜色流 pass。** 载体见下一条 finding 的 Color.A 打包；写入需要一个新算子 `UCSMeshOps::SetVertexColorsRange(Target, FirstVertex, Count, Color, ChannelMask)`——照 `SetVertexColors`(`Private/CSMeshOps.cpp:1349-1371`) 抄，一个 compute pass 只动颜色流，**不调 `InvalidateSections`、不动计数/section/bounds**（`Public/CSMeshOps.h:352-353` 明写「Touches only the colour stream」）。房体是三角汤（下条），CPU 侧知道每面墙对应哪段顶点区间，直接算得出 `FirstVertex/Count`。
**明确排除**：① 把高亮三角挪进新材质槽再跑 `BuildMaterialSections` —— 那是一次 GPU 计数排序 + 可能的 arg buffer 重分配 + 缓冲换身份 + proxy 重绑（`Public/CSMeshOps.h:404-411` 自述），每次悬停跑一遍；② 换 `UCSMesh::Materials[slot]` —— 每次触发 `RecreateRenderState_Concurrent`（`CSMeshRenderComponent.cpp:288`）；③ 引擎 `OverlayMaterial` —— 声明在 `Engine/Classes/Components/MeshComponent.h:44` 且只由 StaticMesh/Skeletal 代理实现，本组件链是 `UPrimitiveComponent`(`CSGpuMeshComponent.h:17`)，即使改继承也拿不到。

**代价**：(A) 0 行，一次实测。(B) 一份后处理材质 + 组件上两次 setter。(C) 一个新算子约 40 行（`SetVertexColors` 是现成模板）+ 材质里一个位测试节点 + CPU 侧记录每部件顶点区间（在 `FCSHouseMeshWriter` 里顺手记，`CSHouseActor.cpp:22-45`）。三者都不动 `UCSMesh` 布局、不进任何 desc 哈希。UE 侧限制两条：**(A) 依赖材质是 masked/two-sided/默认材质**，纯 opaque 自定义墙材质会让 `FHitProxyVS` 不编译；**(C) 的位测试要求顶点色不被插值污染**——房体三角汤成立，地面网格不成立。

**证据**：TG 侧【逆向报告确凿】(§1.4/§1.5/§1.6/§4.1)。项目侧与引擎侧全部文件:行号直证（含 `SceneVisibility.cpp:2558-2563`、`SceneHitProxyRendering.cpp:62-71/847-868/965-990`、`MeshBatch.h:530,538`、`PrimitiveSceneProxy.cpp:380`）。「(A) 今天就已生效」属【我的推断】，必须实测；原稿的「多发 batch 可做逐部件高亮」经核验为**错误**，已替换。

---

### 能被材质读到的逐顶点自定义语义只有 Color 那 32 bit——房体的 32 bit 全空且是三角汤（打包免费），地面的 R 已被占且插值会毁掉位域；通道字典要在 P2 房体格式冻结前定

*维度：选中高亮_编辑反馈_换色换主题的零重建原则*

**Tiny Glade 怎么做**：§1.4【确凿，GLSL 结构体直给】一个 `uint source_id` 同时编码 WallId / bit31 置位时低 4 位内嵌色号 / bit4-19 瞬态楼梯边 id / bit30 悬停高亮；§1.7【GLSL 实证，语义列为推测】另一个 `uint flags` 承担 8 位表（特殊砖 / 用屋顶配色 / 随机胀缩 / 拱圈石反向裁剪 / 减雪 / 拱压扁 / 免高亮 / 强制 LOD0）。两个 uint 承担全部逐实例语义，96 字节/砖里只占 8 字节。注意 TG 的载体是**逐实例**（每砖一份，无插值问题），本项目对位物是**逐顶点**（有插值问题）——这是两边最本质的差别。

**当前方案怎么做**：计划里没有「逐顶点/逐三角自定义语义」的设计，只有 D2 的「顶点色 = 道路权重」。**修正原稿**：计划的开放问题 `:777` 已经写了「道路语义通道固定 R 是否够用：多种地表绘制（草/石/路）会争通道，届时引入通道→语义映射表」——**地面侧的通道争抢计划已经识别并给了对策**，不能算缺口。本条真正未被覆盖的是**房体侧**与**总预算的硬边界**：
- 能被材质读到的逐顶点自定义量**只有 Color 一条**：`CSGpuMesh.Colors` = `PF_R8G8B8A8`/`VET_Color`(`Private/CSGpuMeshTypes.cpp:50-62`)，代理绑成 `Data.ColorComponent`+`ColorIndexMask=~0u`(`Private/CSGpuMeshSceneProxy.cpp:313-316`)。
- `CSGpuMesh.MaterialIds` 是逐三角 uint32，但 **`VfType = VET_None`**(`CSGpuMeshTypes.cpp:84`) → 材质完全看不到它，它只服务 `BuildMaterialSections` 排序与存盘。**不要误当 source_id 用。**
- UV 只有一组（TexCoord slot 0），且**同时被绑成 lightmap 坐标**(`CSGpuMeshSceneProxy.cpp:300-311`)。
- `CustomPrimitiveData` 结构性不通：代理每帧用 `FDynamicPrimitiveUniformBuffer`(`CSGpuMeshSceneProxy.cpp:118-122`)，不进 GPU Scene。
- 地面 Color.R 已被道路权重占(`Public/CSGroundActor.h:34`)；**房体 Color 是全 1 死值**(`Private/CSHouseActor.cpp:41` `S.Colors.Add(FVector4f(1,1,1,1))`)——32 bit 全空。

**差距**：TG 有 64 bit 逐实例语义预算；本项目有 32 bit 逐顶点预算，地面已花掉 8 bit 且计划已知会更紧。房体侧接下来「高亮位 / 色号 / 部件类型 / 主题量」会同时来抢这 4 字节，而房体网格格式一旦在 P2 定型，加流的代价从「改一个 struct」变成「所有已存在的 UCSMesh 重传」。

**建议**：1. **只给房体写一份通道字典，现在就写进 `CSHouseActor.h` 头注释**（地面那份按计划 `:777` 的「通道→语义映射表」另行处理，不要合并）。建议：`A` 通道当 8 bit 打包 —— 低 4 位 = 色号（16 套调色板，对位 TG「bit31 置位时低 4 位内嵌色号」），bit4-6 = 部件类型（墙/拱圈/山墙/墩，对位 TG 的 flags 表），bit7 = 高亮/悬停位（即上一条 finding (C) 的载体）；`RGB` 留给 AO/做旧/雪量这类连续量。
2. **8-bit 整数还原是可证的，不是猜的**：GPU 侧打包公式是 `uint4 I = uint4(round(saturate(RGBA) * 255.0))`(`Shaders/Private/CSMeshOps.usf:85-89` `PackColorBGRA`)——**纯线性量化、无 sRGB 编码**；VF 侧 `PF_R8G8B8A8` UNORM 读回即 n/255。所以材质里 `round(A*255)` 逐位还原成立。
3. **成立的前提是「不插值」，这条必须写死在字典注释里**：`FCSHouseMeshWriter::AddTri`(`Private/CSHouseActor.cpp:27-44`) 每个三角推 3 个独立顶点、不共享索引 —— 房体是**三角汤**，三顶点同值 → 光栅化后整个三角常量，位测试安全，且写色号/部件类型**不需要任何顶点拆分**，在 `AddTri` 的 `Slot` 旁多一个 `PackedId` 参数即可。**地面网格是共享顶点的规则网格，任何位域会在相邻顶点间被线性插值成垃圾**——地面只能放连续量（道路权重就是），这是 `:777` 那张映射表必须遵守的硬约束。
4. **8 bit 不够时的逃生口及其两个坑**：`ECSGpuStreamRole::AuxVertex` 是官方扩展槽(`Public/CSGpuMeshTypes.h:63`)，但 ① 标准集已占 AuxVertex slot 0（材质 id，`CSMesh.h:76`）；② aux 流一律 `VET_None`，材质读不到。要让材质读到必须新增一条 `TexCoord` slot 1(`VET_Float2`)，而在已分配网格上加流**必须**走 `UCSMesh::SetStreamLayoutSync`(`Public/CSMesh.h:573`)——重分配并拷贝每一条流、`AllocationGeneration` 前移、section 表被丢弃、proxy 重绑（同文件 :582 注释）。一次性成本，但要在格式冻结前决定。

**代价**：方案 1-3 近乎零成本：`AddTri` 多一个参数 + 材质里几个节点。方案 4 是一次全流重分配 + 全网格拷贝（房体千级三角 = 微秒级；地面 66049 顶点约 4.2 MB 一次重传，要放加载期）。副作用一条：A 通道被占后，`SaveToStaticMesh` 导出的资产 alpha 是 id 而非透明度，落盘路径的注释要跟着改。

**证据**：TG 侧【逆向报告确凿】(§1.4/§1.7，§1.7 语义列自标推测)。项目侧全部文件:行号直证。原稿把「8 bit 还原」标为【我的推断】——核验后**升级为代码直证**（`CSMeshOps.usf:85-89` 无 sRGB）。「逐顶点插值会毁位域」是【我的推断】，但属光栅化基本事实。

---

### 房屋几何没有对外访问面：footprint→周界、yaw→世界轴已在同一文件里重复推导 5 份（原稿说 4 份，漏了柱）

*维度：碰撞与空间查询基建*

**Tiny Glade 怎么做**：§8.5【逆向报告确凿】ColliderRegistry / RaycastWorld 是一份共享查询世界，墙体放置吸附 shapes::shape_placement、鸭子寻路、鸟巢/窗台候选点、mesh-mesh 交线 find_mesh_mesh_intersections、楼梯装配形变全向它提问；索引 WallSpatialHash / wall_roof_hashmap。§8.1【确凿】装饰件资产自带 _collision / _interaction / _flowerbed_locations 网格。§4.1【确凿】gen_stairs_collider_and_rt_geo 把 collider 当装配的一等产物与可视几何并列生成。→ TG 的查询表示与渲染网格分开、显式建造、显式登记。

**当前方案怎么做**：【核验修正 1：计划没有漏，是显式排期了】计划 Plan_House.md:74 的模块布局里已写 `Public/CSHouseTypes.h  # FCSHouseParams / FCSWallOpening / FCSHouseContact`（且规划在新 Runtime 模块 Source/CSHouse 下，而落地代码放进了 ComputeShaderGenerator）；进度注记明写「UCSHouseSubsystem 未建：本切片只有地面一个上游…快扫等 D7/D8 需要跨房关系时再上（D10）」。PickHouse(Ray) 见 :183 / :368。

【代码事实】几何描述确实全私有：D:/MyProject/UnrealProject/UETest574_2/Plugins/PCGPlugins/Source/ComputeShaderGenerator/Private/CSHouseActor.cpp:13 起的匿名 namespace 里放着 FCSHouseEdgeFrame 与 CSHouse_GetEdge（:97，函数体到 :110）；CurrentDoors 是 Public/CSHouseActor.h:234 的 private transient；对外只有 GetOpenDoorCount()（:192）。重复推导实测 5 处：yaw→世界轴四份 Private/CSHouseActor.cpp:163-165 / 192-194 / 263-265 加两处 FTransform(FRotator(0,Yaw,0),Loc)（:347、:477）；此外 footprint→周界另有独立第二实现——ComputePillars 不走 CSHouse_GetEdge，自己用 Corners[4] 内缩半墙厚重算了一遍（:270-278）。UCSHouseSubsystem 全插件仅 Public/CSTinyGlade.h:21 一句注释。

**差距**：【我的推断，已按核验降级】这不是「计划漏了」，是「已排期的债在落地代码里先开始累积」。证据：一个纵切片（D4+D6+D9）就长出 5 份 yaw 推导 + 2 套周界推导；而 D7 要轮廓交点与 exposure（:507）、D8 要墙有效矩形扣墩跨度（:272）与 PickHouse（:368）、D12 要墙段与门洞净空每次提交打包上传（:614）、D13 要 openings 剪影（:722 附近），问的是同一个「这栋房子的墙段和开口在哪」。

【核验修正 2：原稿「D6 线段抽象没有兑现」过强，这半句删】门的判定逻辑确实只认线段——ComputeDoors 全程只消费 F.Start/F.U/F.In/F.Len（Private/CSHouseActor.cpp:202-224），没有一处认「矩形四面墙」。硬编码的只是线段的提供方（CSHouse_GetEdge 的 EdgeIndex & 3）与三处 for(Edge<4)。计划 :221 的承诺在消费侧已兑现，只差一个多边形 footprint 的 provider。

**建议**：把几何描述提出来，落点就是计划 :74 已规划的 CSHouseTypes.h（当前无 CSHouse 模块，先放 ComputeShaderGenerator/Public，注释里记「模块拆分时随 D4 一起搬」）：
1. struct FCSPlanarFrame { FVector2D Origin, AxX, AxY; static FCSPlanarFrame FromActor(const AActor&); FVector2D ToWorld(FVector2D) const; } —— 一次替换 Private/CSHouseActor.cpp:163-165 / 192-194 / 263-265 三处，另两处 FTransform 由它导出。
2. struct FCSWallSegment { FVector2D P0,P1,OutNormal; float BaseZ,TopZ,Thickness; int32 EdgeIndex; }。
3. 把 CSHouse_GetEdge 升为自由函数 CSHouseGeom::BuildWalls(frame, footprint, thickness, baseZ, topZ, TArray<FCSWallSegment>&) —— 这才是 D6 :221 缺的那个 provider；ComputePillars 的 Corners[4]（:270-278）改成读同一份墙段，两套周界合一。
4. FCSHouseDoor（Public/CSHouseActor.h:17-25）升格为 FCSOpeningRect + ECSOpeningShape（D8 :159-168 的原型 id 语义），窗共用同一通路——注意计划 :74 给它起的名字是 FCSWallOpening，沿用计划的命名。
5. ACSHouseActor 加 const FCSHouseGeom& GetGeom() const（Frame / Walls / Openings / BaseZ,EaveZ,RidgeZ / WorldBounds / Generation），在 ReevaluateSite() 尾部（Private/CSHouseActor.cpp:327 之后）刷新。WorldBounds 从参数算（RidgeH/EaveOut/LAtot 已在 :423-426），不问 GPU——守住计划 :442 与风险节「房子的 bounds 在 CPU 侧从参数算得出」。

**代价**：一个新头 + 一次文件内重构，零行为变化：BodyDescHash 的输入量（Private/CSHouseActor.cpp:247-251 的两段 H.Append）逐字不动，现有无头断言（DEMO OK / 冷加载 VERIFY、House_Road 南北墙各 3 拱、House_Pillar 6 柱 z=150）应逐位复现。约半天。UE 侧零新依赖。风险：FCSHouseGeom 不要 UPROPERTY 化（派生态，序列化了会与 Transient 的 CurrentDoors 定位冲突）；ComputePillars 是 const 函数，改读 GetGeom 时注意 geom 的刷新点必须在它之前。

**证据**：TG：§8.5 / §8.1 / §4.1 均为【逆向报告确凿】，原文核对无误。项目【代码事实】：Public/CSTinyGlade.h:21、Public/CSHouseActor.h:17-25 / 192 / 234、Private/CSHouseActor.cpp:13 / 97-110 / 163-165 / 192-194 / 202-224 / 247-251 / 263-265 / 270-278 / 327 / 347 / 423-426 / 477；计划 Plan_House.md:74 / 183 / 221 / 272 / 368 / 442 / 507 / 614 / 722 及进度注记。「5 份而非 4 份」与「重复份数会继续涨」为【我的推断】。

---

### 查询入口已经在分叉（藤蔓自己手写了一套地面分派）；真要非解析查询时后端已预先注定是 GeometryCore——但现在不要建树、不要动 OBB 解析拾取

*维度：碰撞与空间查询基建*

**Tiny Glade 怎么做**：§8.5【逆向报告确凿】rapier3d 0.22 fork + parry3d 0.17 只用查询管线（动力学未确认被使用），一套世界同时服务放置吸附、寻路、候选点、mesh-mesh 交线、楼梯形变。§7.3 的 RaycastWorld::raycast_w_terrain / mesh_follow_terrain / constrain_to_be_above_terrain 是【确凿】符号；但「地形高度场与碰撞体藏在同一个 API 后面、调用方不知道自己在问谁」这句是**由命名推出的**——原文只说「CPU 副本供这三个函数消费」，且 §7.3 标题带【末端链路待确认】。→ 这半句应记为【我的推断】，原稿把它当【确凿】了。

**当前方案怎么做**：【代码事实】项目侧两套解析 API：ACSGroundActor::RaycastGround（声明 Public/CSGroundActor.h:203，实现 Private/CSGroundActor.cpp:347，平地平面快路径 :357-366 + 起伏半格 march）与尚未实现的 PickHouse(Ray)。第三条已自发长出：Private/CSVineScatter.cpp:211-245 的 ResolveGroundZ 手写了「先扫 ACSGroundActor 问镜像（:220）、打不到再 LineTraceSingleByChannel（:238）」的分派，注释里解释了原因（gpumesh 全线 NoCollision，Private/CSGpuMeshComponent.cpp:14）。第四条在编辑器侧（FCSGroundPaintEdMode 覆写 TraceCandidatePoint 改调 RaycastGround）。

【核验修正：与成文架构冲突，原稿没提】计划风险节明写「拾取全靠解析：gpumesh 无碰撞是既定架构（CSGpuMeshComponent.cpp:14），本计划不给它加 UBodySetup。地面=高度场求交、房/窗/handle=OBB 求交」；Public/CSTinyGlade.h:24-25 把同一条钉进了基类头注释「派生类的拾取一律解析实现（镜像高度场 / 参数化 OBB）」。

**差距**：两件事要分开，原稿把它们捆成一条了：
(a)【代码事实，成立】门面在分叉：同一个「这条射线/这个点碰到什么」的问题今天有 4 个各自为政的答案，每加一个特性就多一份「我该问谁」。
(b)【我的推断，已降级】「解析法答不了一批问题」在**当前计划范围内不成立**：D4/D8 的拾取是矩形 OBB，解析求交精确、微秒级、可纯函数单测（P5 验收门 :743 明确要求 SAT/交点表单测绿）；mesh-mesh 交线、任意多边形 inside、最近表面点这些确实解析答不了，但计划今天一个都没要——连 D12 的摆件吸附都可以走声明式锚点（见第 5 条），不需要查询世界。所以这条的价值是**后端选型的预先裁决**，不是「现在缺一个查询世界」。

**建议**：两步，第二步不动工：
1.【现在做，小】门面收敛而非新建设施：把 Private/CSVineScatter.cpp:211-245 的手写分派换成调 ACSGroundActor 的既有解析 API（它本来就只用 SampleHeight + GetWorldRect2D，第一段逻辑已经是了；差的是登记表，见第 4 条），新查询一律进同一入口。今天的入口就是 ACSGroundActor 的解析函数族 + 将来的 PickHouse，**不要为此建 subsystem**。
2.【预先裁决，落地时才动工】真出现解析答不了的需求（多边形/L 形 footprint 的 inside 测试、屋顶压墙的交线、最近表面点+法线）时，后端用 GeometryCore，不用 Chaos、不自写八叉树：GeometryCore / DynamicMesh / GeometryAlgorithms 已在 ComputeShaderGenerator.Build.cs 的 PrivateDependencyModuleNames 里，而且 GeometryFramework 在 PublicDependencyModuleNames 且其 Build.cs:48 公开依赖 GeometryCore——**连公共头里出现这些类型都是零构建改动**（这一点原稿没查到，是加强项）。可用件（行号实测）：TMeshAABBTree3::FindNearestHitTriangle(:436/:446)、FindNearestTriangle(:159)/FindNearestPoint(:197)、TestIntersection(:947/:1001/:1030)、FindAllIntersections(:1060) → MeshIntersection::FIntersectionsQueryResult::Segments(:39-44)；内外测试 TFastWindingTree（Spatial/FastWinding.h）；broadphase TPointHashGrid2（PointHashGrid2.h:31 / InsertPointUnsafe:84 / FindNearestInRadius:176），即计划 :486 规划的 FCSSpatialHash2D 不必自写。
3. 明确不用 Chaos：query-only body 也要 UBodySetup + trimesh/convex cook，而房体按 :183 在拖拽期每帧重建，逐帧 cook 正是 D4/D8 花力气避开的 hitch；挂上 body 后房子还会对所有引擎 trace 通道可见，PCGEditorProcess/Private/CSBrushEdModeBase.cpp:31-59 的 FCSBrushGeometryFilter 行为会被动改变。

**代价**：第 1 步几行。第 2 步不动工，只写进 CSHouseTypes.h 的头注释当选型记录（避免将来有人顺手写八叉树或加 UBodySetup）。风险提醒：一旦真建 CPU 代理树，就会出现「树答体积问题、openings 表答开口问题」的双口径（代理不含洞），且与计划 :393-408「洞是参数记录、不存切出几何」的用户裁决必须显式对齐；这也是我不建议提前建它的原因之一。

**证据**：TG：§8.5【确凿】；§7.3 三个函数名【确凿】，但「地形与碰撞体同一 API 门面」为【我的推断】（原稿误标确凿）。项目【代码事实】：Public/CSGroundActor.h:203、Private/CSGroundActor.cpp:347/357-366、Private/CSVineScatter.cpp:211-245（TActorIterator :220、LineTrace :238）、Private/CSGpuMeshComponent.cpp:14、Public/CSTinyGlade.h:24-25、ComputeShaderGenerator.Build.cs（Public: GeometryFramework；Private: GeometryCore/DynamicMesh/GeometryAlgorithms）、GeometryFramework.Build.cs:48。引擎 API 行号取自 D:/UnrealEngine-5.7.4-release/Engine/Source/Runtime/GeometryCore/Public/Spatial/{MeshAABBTree3.h,PointHashGrid2.h}，8 处逐条实测命中。Chaos cook 成本与 trace 通道副作用为【我的推断】。

---

### 没有登记表，落地代码已在用 TActorIterator 顶替；但 O(N²·66049) 的加载复杂度是算错的，真问题是「逐格点遍历全部 shaper 不按足迹剔除」

*维度：碰撞与空间查询基建*

**Tiny Glade 怎么做**：§10.3【逆向报告确凿】tabloid 全库只实例化两种频道，collision::world::ColliderDiff 是其一，订阅者 autoclutter_subscribe_to_tabloids / overhang_subscribe_to_tabloids。§8.5【确凿】被 diff 的是 ColliderRegistry——先有登记表，才有频道；空间索引 WallSpatialHash / wall_roof_hashmap 是登记表之上的加速层。

**当前方案怎么做**：【代码事实，行号逐条复核全中】TActorIterator 四处：Private/CSVineScatter.cpp:220（在候选循环 :425-432 内，候选上限 4096 见 :410）、Private/CSGroundShaperActor.cpp:162（IsFootprintIsolated 全文 :158-169，调用点在 BuildStepPlan :207）、Private/CSGroundActor.cpp:421（ResolveShapers）、Private/CSHouseActor.cpp:133、Private/CSGroundShaperActor.cpp:75。RefreshHeightsInRegion 的逐格点内层循环遍历全部 shapers 且不按足迹剔除：Private/CSGroundActor.cpp:475-490（内层 :481-484），尽管 GetFootprintRect2D() 现成（Private/CSGroundShaperActor.cpp:134-139）。

【核验修正：计划早就得出同一结论】计划 :346（D7 复评第 1 条）已论证「仍需一份全体房屋登记表…可以是 ACSHouseActor 的类静态 TArray<TWeakObjectPtr>，注册/注销在 PostRegisterAllComponents / EndPlay，不需要 subsystem 的 tick 循环」，进度注记也明写 subsystem 是有意延后。所以登记表不是发现，是排期。

**差距**：【核验修正：原稿的加载复杂度算错了，数字必须撤】原稿称「N 个塑形物加载时 O(N²×66049)，N=20 约 2600 万次」——不成立，三条反证：
(a) RegisterShaper 在已登记时**直接 return**（Private/CSGroundActor.cpp:428），而地面 PostRegisterAllComponents 会先跑 ResolveShapers() 批量登记全部 shaper（:409、:417-422）。常见加载序下每个 shaper 后来的 RegisterShaper 是零成本，全图 RebuildHeightsFromShapers（:430→:452-455）根本不触发。
(b) 每个 shaper 自己的 RebuildTerrain 只刷 union(旧足迹,新足迹)（Private/CSGroundShaperActor.cpp:389-393），不是全图。
(c) 即使最坏序（shaper 早于地面注册），代价是 Σ 全图 ≈ N(N+1)/2×66049，N=20 约 1.4e7 而非 2.6e7。头注释 Public/CSGroundActor.h:155-156 还写着作者已自评「256² 顶点 × 塑形物数，纯 CPU 距离场，微秒级」——这是知情取舍。
【剩下的真问题，仍成立】① 内层 shaper 循环无足迹剔除（:481-484），代价随 N 线性摊在每个格点上；② 藤蔓每个候选点一次全世界 actor 迭代（:220 在 :425 循环内），上限 4096 次——虽然命中第一个包含该点的地面就 return，最坏仍是 O(关卡 actor 数)；③ ACSGroundActor 其实**已经持有一份 shaper 登记表**（Public/CSGroundActor.h:251 的 Shapers），但在 private 段，IsFootprintIsolated 拿不到，只好再扫一遍世界。

**建议**：按代价从小到大，只做前两条：
1.【几行，且逐位等价】RefreshHeightsInRegion 内层加足迹剔除：`if (!Shaper->GetFootprintRect2D().IsInside(World)) continue;`（Private/CSGroundActor.cpp:481 前）。安全性可证：SampleShapeHeight 在足迹外返回 0，而 0 是 Max 的单位元，剔除后结果逐位相同——现成的无头断言（计划 :755 记的「台顶 300 / 裙边中点 150 / 影响外 0」「画路穿台 27 级石阶」）可直接复验。
2.【几行】给 Shapers 加一个 const 访问器（或把 IsFootprintIsolated 改成由地面提供），删掉 Private/CSGroundShaperActor.cpp:162 的全世界扫描；藤蔓侧把 Private/CSVineScatter.cpp:220 的迭代提到候选循环外缓存一次（散布前解析一次地面指针即可，不改语义）。
3.【等 D7 动工时一起做】房屋类静态登记表按计划 :346 的形态实现，**必须按 world 分桶**（否则编辑器 world 的房子会被 PIE world 的查询看到），TWeakObjectPtr + 取表时顺手清失效项（Private/CSGroundActor.cpp:427 已有可抄的写法）。

**代价**：第 1、2 条各几行，有现成断言复验，半小时量级。第 3 条随 D7 落地，不单独排期。

【与用户裁决的冲突，已按核验删除原稿的第 5 条提案】原稿建议加静态多播 OnAnyHouseGeomChanged 作为 ColliderDiff 的最小等价物——与计划 :64 的架构表冲突：「房屋网格重建产出 | 无需发布——其他房屋只关心 footprint，经快扫可见」，且 D3 整节是用户裁决（v1 全量直推、不做 dirty）。理由不足以推翻，降级为**提醒**：D12 的 MarkDecorDirty()（:656 附近）与 D13 的提交链已有 NotifyEditCommitted 这条统一松手边界，不需要再开一条房屋变更频道。

【与第一轮维度的重叠，已裁剪】原稿的「RegisterShaper 改成区域刷新」属于第一轮⑤变更传播/重建触发，此处删除，只保留空间剔除那一半。

**证据**：TG：§10.3 / §8.5【逆向报告确凿】。项目【代码事实】：Private/CSVineScatter.cpp:210-245/410/425-432、Private/CSGroundShaperActor.cpp:75/134-139/158-169/207/389-393、Private/CSGroundActor.cpp:409/417-422/427/428/430/452-455/471-472/475-490、Public/CSGroundActor.h:147/155-156/158/167/217/251、Private/CSHouseActor.cpp:133；计划 Plan_House.md:64/130/346/486/656/755 及进度注记。原稿的 2600 万次为【已证伪的推算】，本条给出的 1.4e7 上界为【我的推断】。

---

### 「附着到某扇窗/某道门」这类锚点，房屋侧今天问不出来——但 D12 的场通道语义是用户定义的，只能加一条并行来源，不能改通道

*维度：碰撞与空间查询基建*

**Tiny Glade 怎么做**：§8.2【逆向报告确凿】autoclutter 状态机 populate_autoclutter_regions → process_autoclutter_candidates → manage_autoclutter_entities，且有定点入口 add_autoclutter_around_{gates,windows} / add_birdnests。§8.5【确凿】鸟巢/窗台候选点正由碰撞查询世界供给。§8.1【确凿】装饰件资产自带 _flowerbed_locations 这类语义锚点网格——TG 的摆件放置有一条与复杂度场完全独立的、基于结构锚点的通路。

**当前方案怎么做**：【计划事实】D12 把与门窗的关系编进标量场通道：RT_DecorField 的 Z「沿墙外侧窄带、贴墙峰值随距离衰减；门前净空清零；房 footprint 内清零」、XY「最近墙段外法线的距离加权混合」（Plan_House.md:609-611），候选来自 tile-argmax 与抖动网格伯努利（:641 附近），场经异步回读才到 CPU（:672-681，总延迟 6-10 帧）。

【核验修正：原稿「窗的世界摆位不出现在任何对外结构里」过强】D8 明写「注册即诉求：解析到宿主后登记的内容是特征类型 + 世界摆位 + 参数」，标记本身是场景里 attach 在房子上的 actor，D13 也明确要消费 openings 剪影。所以窗的摆位在计划层面是存在的，缺的是**房屋侧没有一个可枚举的几何/锚点访问面**（即第 1 条），以及 D12 的放置输入被定义成只有场。

**差距**：【我的推断，已降级】「窗台花盆 / 门两侧灯笼」在当前 D12 的**放置输入定义**下无法表达：场是标量的，tile-argmax 只挑复杂度最高的格，落点与某扇具体的门窗之间只有概率相关性，没有约束关系；而这类摆件本不需要任何回读（锚点是纯 CPU 声明式的），却要排在 6-10 帧的异步状态机后面。这是 TG 那条「结构锚点通路」在本项目的空缺——注意这是**输入来源**的空缺，不是「设计错了」。

【与第一轮⑥的边界】放置流程本身（候选/谓词/diff/稳定 id）属于第一轮⑥植被摆件维度，此处不重复；本条只主张一件事：锚点数据该由房屋几何面产出，而这正是本维度（查询/数据可得性）的问题。

**建议**：只加一条并行来源，不碰任何既有通道：
1. 在第 1 条的 FCSHouseGeom 上挂 `TArray<FCSFeatureAnchor>`：`struct FCSFeatureAnchor { FTransform World; ECSAnchorKind Kind; float Width; FVector2D ClearExtent; FGuid SourceId; };`，Kind ∈ {DoorJambL, DoorJambR, DoorClearance, WindowSill, WallCornerOut, EaveEdge}。
2. 产出它的输入在 ComputeDoors() 里**已经全有**（Private/CSHouseActor.cpp:186-254，逐条实测）：F.Start / F.U / F.In 取自 :202，拱心 S0 + Pitch*0.5 见 :240，Width :233、Height :234，世界外法线 OutWorld 已在 :218 算过。门框两侧锚点 = 拱心 ± Width/2 沿 F.U；朝向 = OutWorld。写出来几十行，零额外查询、零回读，**不依赖第 2 条的查询世界**。
3. D12 放置器加一条锚点驱动来源，与场驱动散布并行，共用后半段的同一条谓词（跨层不重叠 / 同类间距球）与同一条 diff 应用；稳定 id 用 hash(class, SourceId, Kind) 而非格坐标——门窗不动它就不动，比场驱动更稳，正合 :650-654 的稳定性纪律。

【已按核验删除的原稿第 ③ 条】原稿建议把「门前净空」从场通道升级为锚点自带矩形。计划 :606 明写「通道语义**按用户定义**」，Z 通道「门前净空清零」是其中一条。理由不足以推翻，降级为**提醒**：锚点里可以携带 ClearExtent 供锚点来源自用，但场通道原样保留，两者并存不互斥。

**代价**：结构体 + ComputeDoors 尾部几十行 + D12 放置器一层循环。依赖第 1 条，不依赖第 2 条。风险两条：① 锚点朝向必须与 D8 标记的墙面吸附共用同一个 FCSPlanarFrame，否则会出现「窗在墙上、花盆嵌在墙里」的错位——这是把 frame 提成公共结构的另一个理由；② 锚点稳定 id 与 D12 的格坐标 hash 同处一个 id 空间，要避免撞号（用 Kind 做前缀位即可）。数量级：每房 ≈ 门数×3 + 窗数×1 + 4，village 量级几百个，CPU 无压力。

**证据**：TG：§8.2 的 add_autoclutter_around_{gates,windows} / add_birdnests、§8.1 的 _flowerbed_locations、§8.5 的候选点来源，均为【逆向报告确凿】，原文核对无误。项目【代码事实】：Private/CSHouseActor.cpp:186-254（:202 / :218 / :233 / :234 / :240 逐行核对命中）；计划 Plan_House.md:606 / 609-611 / 614 / 641 / 650-654 / 672-681 / 722 附近（原稿行号系统性偏 +2，语义无误）。「当前放置输入无法表达锚点式附着」为【我的推断】。

---

### 【降级为提醒】两房相交处的内部墙段可以直接省掉——TG 侧证据只到「按交点切段」，「切完不铺砖」是推断；作为开放问题 :780 的一个更便宜答案提出，不替换 D7 角柱

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.2 末段原文只有一句：「墙路径按与其他墙/楼梯的交点打分切段：`segmentation::calc_path_wall_segmentation`（`score_split_candidate`、`WallPathSegmentationMask`）[SYM]」。§10.2【确凿】`systems/wall/` 下有 `merge_walls`、`wall_splitter`。§4.2【确凿】平台楼梯 `split_wall_path_segment_into_platform_stairs` 复用同一分段器。

**【核验修正，原稿的核心过度声称】** 原稿把「切段后内部段不铺砖 / 拓扑切割而非装饰性遮挡」标成【确凿】——报告里**没有任何一句**说被切出的段会被丢弃或不铺砖。符号名只证明「有分段器」。这属【你的推断】，且强度只有中等：`merge_walls` 与 `WallPathSegmentationMask` 同样可以解释成「合并成一条曲线后重排砖」。

**第二个核验修正**：TG 里根本不存在「两个盒子房互穿」这个情形——墙是曲线、屋顶是独立对象（§3.1），没有 footprint OBB 这个概念。类比本身是跨架构的，不能当直接依据。

**当前方案怎么做**：D7（:277-360）：触发 = 两房 footprint OBB **真正相交**（:280，用户裁决）；产物 = 每交点一根叠砖角柱（:335，用户裁决）；交点级用 `SeamMinExposure` 逐点丢弃（:341）；整对级 `LinkMaxOverlap` 判为「合并意图」→ 整对不生 + 编辑器提示（:540）。开放问题 :780 明确把「是否值得改走整体合并（对两房 footprint 求并、按外轮廓重建成一栋）」挂起。

代码侧 `RebuildBodyMesh`（`CSHouseActor.cpp:353-413`）无条件铺满每条边：门之间的剩余全部由 `AddSegment(Cursor, F.Len)`（:412）补实，没有任何「这段墙不该存在」的通路。

【核验确认】原稿说「分段器已经在跑，只差第二个谓词」属实：`ComputeDoors` :206-241 已经把每条边等分成 `N = FMath::Clamp(FMath::RoundToInt(Usable / DoorPitchTarget), 1, 32)` 个子段并逐子段跑谓词（:222 道路覆盖率 / :232 离地收窄），加第二个谓词确实是同一循环里的事。

**差距**：两栋相交房今天的观感是「两个完整盒子互穿 + 交点插一根柱子」，内部两截墙实体照旧存在。这是真的缺口，但**收益被原稿高估**：即使把内部墙段 Omit 掉，两个屋顶与两组山墙仍各按自己完整 footprint 生成（`CSHouseActor.cpp:415-450`），交界处照样穿帮——「看起来像一栋连贯建筑」需要屋顶也参与，而屋顶的多屋顶化在另一条 Finding 的步骤 2 之后才有可能。所以本条的真实收益是「墙面不再双层互穿 + 两室之间自然形成通道口」，不是「变成一栋房」。

代价评估这一半是对的：计划 :780 把合并想成「2D 布尔求并 + 拓扑重建」，而「逐子段判是否在别人内部」只是在现成循环里加一次点在 OBB 内的测试。

**建议**：**定位：这是对开放问题 :780 的一个低成本候选，不替换 D7 角柱（角柱遮的是外轮廓交点处的穿插，Omit 消的是内部段，两者共存）。是否做由用户裁决。**

1. `FCSHouseDoor`（`CSHouseActor.h:17-25`）升成 `FCSWallSpan { int32 EdgeIndex; float S0, S1; ECSWallSpanKind Kind; }`，Kind ∈ {Solid, Arch, Omit}；`ComputeDoors` 改名 `ComputeWallSpans`，输出覆盖整条边的连续 span 表，而不是「门列表 + 隐式剩余」。
2. 在 `ComputeDoors` 的逐子段循环（:208-241）里，紧挨着道路覆盖率谓词加第二个谓词：子段中点沿 `F.In` 内缩 `WallThickness` 得世界点，逐个测试是否落在其它已注册房屋的 footprint OBB 内（含 yaw，两次点乘）；命中 → `Kind = Omit`，同时跳过门判定（内部墙不开门）。
3. `RebuildBodyMesh`（:353-413）的墙循环改成遍历 span 表：Solid 走现有 `AddSegment`，Arch 走现有拱几何（:369-411），Omit 什么都不生成。屋顶/山墙本条不动（明确遗留）。
4. 登记表用 D7 复评已裁决的形态（:347：`ACSHouseActor` 类静态 `TArray<TWeakObjectPtr>`，注册在 `PostRegisterAllComponents`、注销在 `EndPlay`），村庄量级线性扫；**不需要 subsystem、不需要 Octree**。
5. 哈希：Omit 段进 `BodyDescHash`——在 :251-252 的 door 循环旁加一段 span 循环即可。
6. 单测进 `CSHouseLogicTests`：两个轴对齐矩形 T 形叠合 → 被吃进去的那条边全 Omit、外轮廓边全 Solid。

**代价**：约 120 行，全在 `CSHouseActor.cpp` 内；每子段一次 OBB 点测试（默认 4 边 × 各 ≤32 段 = 128 次点乘/邻居），CPU 微秒级。

风险三条：① 深度重叠时可能把某栋房的全部墙判成 Omit，退化成「只剩屋顶飘着」——必须加下限「至少保留 1 段 Solid，否则整栋判为被吞并、走 `LinkMaxOverlap`(:540) 的既有提示路径」；② `SeamMinExposure`(:341) 的「外露走线长」定义要改成按 Solid span 算，而不是按几何轮廓算；③ **UE 侧限制**：计划 P2 要把房体做成「闭合实体 + MeshBoolean 减 cutter」，Omit 会在壳上留开口。当前实现并不受影响（`RebuildBodyMesh` 是逐段 `AddBox` 的盒汤，本来就不是单一闭合流形），但 P2 走布尔后需要给 Omit 段补两片端封面，否则布尔的内外判定会失效。

**不推翻裁决**：D7 裁的是「每交点一根角柱」和「整对不生 + 提示」，两条都保留。

**证据**：【逆向报告确凿】§1.2 末段（仅证明存在分段器）/ §10.2 / §4.2；【你的推断】「切段后内部段不铺砖」——报告无原文支撑，原稿误标【确凿】，已降级。【代码事实（已核对）】CSHouseActor.cpp:206-241/:251-252/:353-413/:412/:415-450、CSHouseActor.h:17-25。【计划】:280 / :335 / :341 / :347 / :540 / :780

---

### 【已修正核心论据】缺 house-to-house 对齐吸附（TG 的一等公民）；原稿「可用带只有几十厘米」的算法有误，真实可用带约 40–200 cm，但 PostEditMove 累加器纪律必须先写下

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§8.5：`systems::collision::world::{ColliderRegistry, ColliderDiff, RaycastWorld}` 基于 rapier3d 0.22 + parry3d 0.17 的**查询管线**，服务范围首条就是「墙体形状放置吸附（`shapes::shape_placement`）」；空间索引 `WallSpatialHash / wall_roof_hashmap`。§10.2【确凿】`systems/wall/snapping` 与 `merge_walls`/`wall_splitter`/`shapes`/`eraser` 并列为独立源文件。§9.5【确凿】rapier/parry 的用途栏逐字写着「服务墙放置吸附」。

这一半原稿引用准确：**吸附在 TG 里是一等公民，且服务的正是「放一个墙形状」这个动作**——恰好对应本项目的「放一栋房子」。

**当前方案怎么做**：计划全文只有两处「吸附」，都是窗标记对**自己宿主墙面**的吸附：D8 :369（`WindowSnapDist` 就近墙面查询）与 :385/:426（`SnappedWorld` 对齐法线贴外皮）——三处行号已核对无误。**房屋与房屋之间零吸附、房屋对格线零吸附。**

D7 的接触判据是二值的：:280「两房 footprint box **真正相交**才生角柱——不是『靠得近』。所见即所得，没有『多近算近』的调参问题」；P5 验收门 :743 要求「推到叠上才生、拉开即消；靠得极近但没碰上不生；**停在刚接触处不闪烁**」。

**差距**：**【核验：原稿的量化论据错了，必须改】** 原稿称可用带「下沿是硬的零」「让用户手动命中一个零测度边界」——推错了。三条参数的真实约束是：penetration > 0 才成立；交点两侧外露走线长 ≥ `SeamMinExposure`=40 cm（:341）；penetration < `LinkMaxOverlap`=较小房子半宽（:540，默认 600×400 时 = 200 cm）。默认参数下可用带约 **40–200 cm，宽 1.6 m**，gizmo 拖拽轻松命中。P5 :743 的「停在刚接触处不闪烁」是**鲁棒性要求**（临界处别抖），不是要求用户命中边界。原稿把它读成后者，是本轮最严重的一处误读。

**剩下的真缺口**（仍成立、且是本维度的核心）：把两栋房的墙面**对齐齐平**——Tiny Glade 截图里最常见的那种「两栋房贴着长边并排、墙面共线」——今天全靠肉眼推 gizmo，一个像素在常见视距下是几厘米，共线是真正的零测度目标。这不是 D7 判据的问题，是**缺一整层放置吸附**的问题；TG 恰好有（§8.5/§10.2）。

**建议**：**不需要 parry、不需要给 gpumesh 加碰撞**——操作数是参数化 OBB，全在 CPU。

1. 纯函数 `static bool CSHouse_SnapPose(const FVector& RawLoc, float RawYaw, const FVector2D& Size, TArrayView<const FCSHouseObb> Neighbors, float SnapDist, FVector& OutLoc, float& OutYaw)`（进 `CSHouseLogicTests`）。候选：① 邻居 4 边 × 我 4 边，先按法线夹角筛平行/垂直（`|dot| > cos(SnapAngleTol)`），再算沿法线间距 d，`|d| < SnapDist`（默认 25 cm）时沿法线平移使外皮**共线**（这才是要吸的目标，不是「刚接触」——D7 的可用带已经足够宽，不需要吸到它）；② yaw 吸到邻居边角度的 ±90° 整数倍。邻居来自 D7 复评已裁决的类静态登记表（:347）。
2. **【核验修正】不做「无邻居时吸到 50 cm 格距」这一档**——编辑器原生 translate grid snap 已经覆盖，重复实现只会和它打架。
3. **接线点 `ACSHouseActor::PostEditMove(bFinished=false)`，且必须走「原始位姿累加器」**（这条是本 Finding 最耐用的部分，与下一条 Finding 是同一个坑）：编辑器 gizmo 是**增量**施加变换的（`FLevelEditorViewportClient` 每次鼠标移动把 delta 交给 `GEditor->ApplyDeltaToActor`，它不知道有人改过 actor）。若每帧写 `SetActorLocation(Snap(GetActorLocation()))`，下一次鼠标增量会叠在已吸附位置上，表现为「吸住后拖不动」或反复弹跳。正确形态：actor 上加 transient `FVector RawDragLoc, LastAppliedLoc`；`PostEditMove` 里先 `RawDragLoc += GetActorLocation() - LastAppliedLoc`，再 `Snapped = Snap(RawDragLoc)`，`SetActorLocation(Snapped)`，`LastAppliedLoc = Snapped`；`bFinished=true` 时 `RawDragLoc = LastAppliedLoc`。
4. 视觉反馈：命中时画一条对齐参考线（`Public/CSGpuDebugDraw.h` 与笔刷的预览绘制是现成先例）。
5. `SnapDist = 0` 完全关闭。

**UE 侧限制两条**：① `PostEditMove` 是 `WITH_EDITOR` 的，PIE 游玩外壳（开放问题 :779，**原稿误引为 :216/:439/:782**）要另接一条同样走累加器的路径；② 在 gizmo 拖拽中改 actor transform 时，widget 的 pivot 由 `GEditor->SetPivot` 维护，不同步会让 gizmo 图形与 actor 视觉分离——落地时确认一次。

**代价**：约 130 行（求解器 ~80 + PostEditMove 接线 ~40 + 调试线 ~10），零 GPU、零回读。**不碰任何用户裁决**——计划里没有关于房屋间吸附的裁决，这是空白。

唯一真风险：`RawDragLoc` 累加器的语义必须写进头注释并配单测「连续 10 次零增量的 `PostEditMove` 不改变 actor 位置」，否则后人会把它当冗余变量删掉，重现「吸住后拖不动」。

**证据**：【逆向报告确凿】§8.5 / §9.5 / §10.2（TG 有 shape_placement 吸附 + snapping 独立源文件）；【已删除的错误推断】「可用带下沿是硬的零 / 零测度边界」——由 :280/:341/:540 直算得 40–200 cm，原稿算错。【代码事实】计划 :369/:385/:426 是仅有的两处吸附（已核对）；【计划】:280 / :341 / :540 / :743 / :347 / :779

---

### 屋顶脊向是 footprint 的派生量：D5 单边推拉跨过正方形时脊与山墙原地 90° 跳变（十行滞回即可修）

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§3.1：屋顶是**独立编辑对象**（`CreateRoofCmd/EditRoofCmd/SwitchRoofTypeCmd/RecalculateRoofCmd`，独立 crate `system_roof`），形状由 `RoofRidgeDims` 参数化（脊向 `RoofRidgeDirection`、脊长、高度），配三个 UI 手柄 `ui_edit_roof_ridge_dir / _length / _tip`；山墙墙高被屋顶反向联动 `RecalculateGableRoofWallHeights`【确凿】。
【逆向报告推测】「脊长=0 近似 hip」在报告里明确标为【推测】——原稿在 proposal 步骤 2 里拿它当四坡尖顶的依据，此处已标出。

**当前方案怎么做**：计划从头到尾没有讨论过屋顶形状的可编辑性——`FCSHouseParams`（:147-156，原稿写 :150-160 大致对）只有 `RoofPitch` 一项与屋顶有关；落地后加到 `RoofOverhang` / `RoofThickness`（`CSHouseActor.h:69/:73/:77`）。

代码 `CSHouseActor.cpp:415-450` 把脊向钉死为 `const bool bLongX = FootprintSize.X >= FootprintSize.Y;`（:417），全部屋顶几何（两片坡板 :428-438 的 `AddBox`、两端山墙棱柱 :442-449 的 `AddPrismTri`）经 `AB()` lambda（:420）按这个 bool 做坐标交换。行号全部核对无误。

**差距**：**D5 拖尺寸期的可见跳变**：单边推拉把 `FootprintSize.X` 拖过 `FootprintSize.Y` 的瞬间 `bLongX` 翻转，屋脊与两片山墙原地旋转 90°。`FootprintSize` 在 `BodyDescHash` 里量化到 1 cm（`CSHouseActor.cpp:248-250` 的 `H.Append`），跨越点必然触发一次重建，所以是硬跳变不是渐变。默认 600×400、`MinFootprint` 200（:210），跨越点（X=Y=400）落在合法拖拽范围正中——**是必然会撞上的，不是边角情形**。

【核验注记】原稿把「没有脊向参数 ⇒ 做不了四坡、给不了多边形 footprint 确定屋顶」也算进本条。那部分属屋顶形态议题，与第一轮维度③（屋顶瓦片与装饰件）有重叠风险，**本条只保留与本维度（拉尺寸交互连续性）直接相关的那一面：拖拽中的 90° 跳变**；形态扩展降级为提醒。

**建议**：**步骤 1（~10 行，建议直接做，本条的全部主张）**：给 `ACSHouseActor` 加 `float CachedRidgeYaw`，自动脊向改成带滞回——只有 `|FootprintSize.X − FootprintSize.Y| > csh.RidgeFlipHysteresis`（默认 30 cm）时才允许翻转，否则沿用 `CachedRidgeYaw`；把它量化后并进 `BodyDescHash`（在 `CSHouseActor.cpp:248-250` 的 `H.Append` 里加一项）。与 D6 门拱滞回（`SlotOnCoverage`/`SlotOffCoverage`，`CSHouseActor.h:130/:134`）、D6 墩样式双阈（计划 :271 一带）是同一条纪律，不新增概念。

**⚠️ 核验补的一个坑（原稿没写）**：若 `CachedRidgeYaw` 做成 `Transient`，重开关卡后会退回「按长轴自动选」，接近正方形的房子会在**加载时**翻一次脊——症状与 `DoorSlotOpen`（`CSHouseActor.h:237`，非 UPROPERTY 的 TMap）的滞回状态在加载后丢失是同一类。要么把它做成序列化的 UPROPERTY，要么明确接受「重开工程时近正方形房屋的脊向可能翻转」并写进头注释。

**步骤 2/3 降级为提醒，不作为本条建议**：加 `FCSRoofParams { RidgeYaw / RidgeLength / RidgeHeightOffset }` 并把 `bLongX`/`AB()` 换成按 `RidgeYaw` 建局部基——形态上确实对应 TG 的 `ui_edit_roof_ridge_dir/_length`（§3.1【确凿】），但它属屋顶形态议题（疑与第一轮维度③重叠），且四坡尖顶所依据的「脊长=0 ≈ hip」在报告里是【推测】。真要做时须知：要重写 :437 的坡板 `AddBox` 与 :442-449 的山墙 `AddPrismTri`，而 :67-84 的绕序注释（「前脸绕 -Extrude 为 CCW 时，边取反向才让 边×Extrude 朝外」）说明这里的法线口径极易写反，且 `bVerifyUsedMaterials=false` 下写反只会**静默不画**——必须先配一条「所有面法线与包围盒中心夹角 > 0」的单测。

**代价**：步骤 1 十行、零 GPU、零风险，唯一需要拍板的是 `CachedRidgeYaw` 是否序列化。

**不与任何用户裁决冲突**：计划里关于屋顶只有三个数值参数（`RoofPitch`/`RoofOverhang`/`RoofThickness`），无形态裁决；滞回本身是计划已多处采用的既有纪律。

**证据**：【逆向报告确凿】§3.1（屋顶独立对象 + RoofRidgeDims + 三个 UI 手柄 + RecalculateGableRoofWallHeights）；【逆向报告推测】「脊长=0 近似 hip」；【代码事实（已核对）】CSHouseActor.cpp:415-450（:417 bLongX / :420 AB / :437 坡板 / :442-449 山墙）、:248-250 哈希量化、:67-84 绕序注释、CSHouseActor.h:69/:73/:77/:130/:134/:237；【计划】:147-156 / :210

---

### 【论据方向已倒转】TG 的曲线工具不该抄——GeometryCore 里全有且依赖已在位；但 GeometryProcessing 的 Editor-only 限制**今天就已经armed**，不是未来才暴露的坑

*维度：交互形态：画线成墙 vs 放房子拉尺寸*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.1 `utils::curve::Curve` 30+ 方法（`try_resample` / `smooth_weighted` / `project_point`）；§10.1【确凿】`utils` crate 含 `curve/`、`rdp.rs`（RDP 简化）、`wall_space.rs`、`subtractive_1d_boolean.rs`；§9.5【确凿】lyon / splines / geo / delaunator / earcut(r) / meshopt / mikktspace 在 Cargo.lock + PDB **双零命中**，且报告结语明写「挤出、铺砖、屋顶三角化、台阶生成全部自研」。

即：TG 从零自研这一套，是因为它是 Bevy 纯 ECS + 自研 Vulkan 渲染器（§0【确凿】，Cargo.lock 里连 `bevy_render`/`bevy_pbr` 都没有），**没有引擎可用**。本项目没有这个约束——这是「不该抄」的全部依据。

**当前方案怎么做**：计划里没有任何折线/曲线工具的规划条目。D11 直接用引擎 `USplineComponent`（`CSSplineBlockActor.h:89`），D9 石阶用 GPU 侧 `RDG_SmoothSpline`。插件里唯一的 CPU 折线工具是 `UPolyLine::{SmoothLine, ResamppleByCount, ResamppleByLength, ConvertPolyPathToTransforms, CurveU}`，位于 `Source/GeometryScriptExtraEditor/Public/GeometryMathUtils.h:38-58`（原稿写 :44-56，实际类体 :38-58，函数 :44-56，可接受）。全插件 grep `Douglas|Peucker|RDP` **零命中**（已复跑确认）。

**差距**：两个独立问题，第二个的方向必须倒转。

**① 位置错（原稿正确）**：`GeometryScriptExtraEditor` 在 `PCGPlugins.uplugin` 里是 `"Type": "Editor"`（已核对 uplugin :19-24），且 `UPolyLine` 的签名吃 `FGeometryScriptPolyPath`——Runtime 的 `ComputeShaderGenerator` 既不依赖它也用不了，房屋/地面这条链一个字都复用不到。

**② 抄错方向（原稿正确）**：这些东西引擎 **Runtime** 模块 `GeometryCore` 里已经全有，且 `ComputeShaderGenerator.Build.cs` 的 PrivateDependencyModuleNames 已列 `GeometryCore`（已核对），`Private/CSMeshOps.cpp`、`ComputeShaderMeshBoolean.cpp`、`ComputeShaderMeshFill.cpp`、`Tests/CSGpuMeshObjectTests.cpp`、`Public/ComputeShaderGenerateHelper.h` 五个 TU 已在用 `UE::Geometry`。**对照表（引擎源码逐条核对，行号全对）**：`utils/rdp.rs` → `TPolygon2<T>::Simplify(ClusterTol, LineDeviationTol)`（`Engine/Source/Runtime/GeometryCore/Public/Polygon2.h:819`，内部 `SimplifyDouglasPeucker` :776）；`Curve::project_point` → `TPolygon2::DistanceSquared(Q, OutSegIdx, OutSegParam)`（`Polygon2.h:595`）；点在闭合曲线内 → `TPolygon2::Contains(Q)`（`Polygon2.h:351`）；弧长 → `CurveUtil::ArcLength(Vertices, bLoop)`（`GeometryCore/Public/Curve/CurveUtil.h:213`）；闭环三角化 → `CompGeom/PolygonTriangulation.h`。引擎**没有**的只有折线版 resample 与 `smooth_weighted` 两个。

**③ 【核验：原稿这条的因果讲反了，是本轮最需要纠正的一处】**
原稿说「多边形 footprint 相关的一切只准落在 GeometryCore，不要新增对 GeometryAlgorithms 的依赖面——否则这个坑要等到很晚才暴露」。实测：`PCGPlugins.uplugin:47-53` 把 `GeometryProcessing` 插件写成 `"TargetAllowList": ["Editor"]`；而引擎 `Engine/Plugins/Runtime/GeometryProcessing/GeometryProcessing.uplugin` 里的模块只有 `GeometryAlgorithms` / `DynamicMesh` / `MeshFileUtils`（**`GeometryCore` 不在其中——它在 `Engine/Source/Runtime/GeometryCore`，是无插件门禁的引擎 Runtime 模块**）。而 `ComputeShaderGenerator.Build.cs` 的 PrivateDependencyModuleNames **已经无条件列了 `GeometryAlgorithms` 与 `DynamicMesh`**（不在 `if (Target.Type == TargetType.Editor)` 分支里）。**结论：这个 Runtime 模块今天就已经编不进 Game/Client/Server target**，与多边形 footprint 毫无关系。计划里「运行时（PIE 游玩）交互外壳」（:13 / :214 / :440 / 开放问题 :779——**原稿误引为 :216/:439/:782，全错**）在 Editor target 的 PIE 下没问题，真打 Game target 时**现在就断**。

**建议**：1. **不建曲线库、不移植 TG 的 30 个方法。** 上一条 Finding 的 `FCSHouseFootprint` 内部直接持 `UE::Geometry::FPolygon2d`（或按需从 `TArray<FVector2D>` 构造），Contains / DistanceSquared / Simplify / ArcLength 四件事全部委托给它。
2. 只补引擎缺的两个：`ComputeShaderGenerator/Public/CSPolyline.h`（纯 static、无 UObject、可直接进 `CSHouseLogicTests`）放 `ResampleByLength(TArrayView<const FVector2D>, float Step, TArray<FVector2D>&)` 与 `SmoothWeighted(...)`。约 80 行。
3. 把 `UPolyLine`（`GeometryMathUtils.h:44-56`）降级为「编辑器蓝图门面」，实现转调 `CSPolyline`，消掉重复实现。
4. **【纠正后的行动项】** 关于 `GeometryProcessing` 的 Editor 限制，正确做法不是「避免新增依赖面」（面已经铺满了），而是二选一：**(a)** 若确定要支持 Game target，把 `PCGPlugins.uplugin:47-53` 的 `TargetAllowList` 去掉（`GeometryAlgorithms`/`DynamicMesh` 本身在引擎里就是 Runtime 模块，可以进 Game）；**(b)** 若确定只做编辑器工具，就在 `PCGPlugins.uplugin` 里把 `ComputeShaderGenerator` 也加上 `"TargetAllowList": ["Editor"]`，让约束显式、错误提前，并把计划 :779「运行时（PIE 游玩）交互外壳」明确限定为「PIE 而非独立 Game 打包」。**建议先跑一次 `-Target=UETest574_2Game` 的 UBT 打个真实报错出来再定**——这比任何推理都便宜。
5. RDP 唯一真正需要的场合是「将来真做画线」（把鼠标采样点简化成控制点）；现在不做画线，需要时就是一行 `Polygon.Simplify(tol)`。

**代价**：第 1–3 项：新建一个 ~80 行头文件 + 一次 `UPolyLine` 转调改造，零风险、零 GPU。直接下游：多边形 footprint 的 `ComputeSeatZ`（用 `Contains` 替掉 `CSHouseActor.cpp:170-181` 的矩形双循环）、D8 窗标记的就近墙面查询（计划 :369，用 `DistanceSquared`）、房屋吸附求最近边。
第 4 项：一次 UBT 跑测（~几分钟）+ 一行 uplugin 改动，或一次产品边界确认。**不推翻任何用户裁决**——计划里没有关于目标平台的裁决。
不做的代价：点在多边形内 / 最近边这类三行函数会散落三四份，且 `UPolyLine` 这份现成实现继续对 Runtime 不可见；第 4 项不做则「运行时外壳」在真正打包时才炸。

**证据**：【逆向报告确凿】§0 / §1.1 / §9.5 / §10.1；【代码事实（已核对）】GeometryMathUtils.h:38-58、PCGPlugins.uplugin:19-24（GeometryScriptExtraEditor=Editor）与 :47-53（GeometryProcessing TargetAllowList=[Editor]）、ComputeShaderGenerator.Build.cs 无条件私有依赖 GeometryCore/GeometryAlgorithms/DynamicMesh、五个 TU 已用 UE::Geometry、grep Douglas|Peucker|RDP 零命中；【引擎源码（已核对）】GeometryProcessing.uplugin 只含 GeometryAlgorithms/DynamicMesh/MeshFileUtils、GeometryCore 在 Engine/Source/Runtime、Polygon2.h:351/:595/:776/:819、Curve/CurveUtil.h:213；【计划】:13 / :214 / :369 / :440 / :779（原稿误引 :216/:439/:782）

---

### [WEAKENED] 地面容量台账：镜像采样密度与渲染网格密度被 1:1 绑死，1024² 配置瞬时 CPU 峰值 ~222 MB 且面板零提示；TG 用 512² mask + 91 列网格解耦

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 把「数据分辨率」与「渲染网格分辨率」分开：数据侧主 heightmap 与全部 mask 均 512²、覆盖 130 m（§0 全局坐标约定；§7.1 heightmap r16、编码 `(h·20+2.5)/26.5`）⇒ 25.4 cm/texel、512×512×2 B = 512 KB；渲染侧 `_terrain_editing_floor.raster` 的 VS **无顶点输入**，`gl_VertexIndex` 生成 91 列网格、跨度 ±65.72 m，y 直接采 heightmap（§7.2）。地形笔画不是栅格而是参数化笔画栈（每笔 ≤1024 点，§7.1）。

**两处订正原稿的过度陈述**：① 原稿称「顶点缓冲与索引缓冲各 0 字节」——**报告 §7.2 明写「CPU 只在 `startup_terrain::grid_gen_ccw/cw` 生成索引」，索引缓冲是存在的**，零的只有顶点缓冲；② 原稿把「地形 PS 消费 paths_visual 做材质混合」当【确凿】引用，报告 §5.2 对这最后一步自标【推测】。③「8,281 顶点 / 16,200 三角」是由「91 列」外推的【我的推断】，报告只给了列数，未给行数。

**当前方案怎么做**：`ACSGroundActor` 的镜像与渲染网格是同一张格：`NumVertsX = NumCellsX + 1`（CSGroundActor.cpp:73-74），`BuildSnapshotFromMirror` 逐格点写位置/法线/切线/UV/颜色再每格发两个三角（:107-162）。默认 `NumCellsX/Y = 256`、`CellSize = 50`，**ClampMax = 1024**（CSGroundActor.h:76/:80/:85）。

**差距**：**容量台账（CellSize=50 cm，全部由代码布局直算）**

| NumCells | 覆盖 | 顶点 | 三角 | GPU 常驻 | 镜像（进 .umap） | 一次全量重建瞬时 CPU 峰值 |
|---|---|---|---|---|---|---|
| 256（默认） | 128 m | 66,049 | 131,072 | 4.21 MB | 528 KB | ~14 MB |
| 512 | 256 m | 263,169 | 524,288 | 16.8 MB | 2.1 MB | ~58 MB |
| **1024（clamp 上限）** | 512 m | 1,050,625 | 2,097,152 | **67.2 MB** | **8.4 MB** | **~230 MB** |

GPU 常驻按 8 流直算（pos 12 + tangents 8 + uv 8 + color 4 = 32 B/顶点，索引 4 B，matid 4 B/三角）。峰值 = `FCSGpuMeshCPUData` 快照（Positions/Normals/Tangents 各 FVector3f + TexCoords FVector2f + **Colors 是 FVector4f 16 B** = 60 B/顶点，另加 Indices 与 TriangleMaterialSlots；CSGpuMeshTypes.h:129-147）+ `CopyFromMeshSnapshot` 的打包副本 + `GraphBuilder.Alloc` 的第三份复制（CSMeshOps.cpp:1140-1142），三份叠加。

**结构性根因**：道路权重存在**顶点色 R 通道**（CSGroundActor.h:34/:91），所以「路要多细」直接把网格顶点密度钉死。TG 的路是 512² mask（§5.2【确凿】的 mask 加工链），与地形网格密度无关。

**面板零提示**：属性面板给出 ClampMax=1024，而 1024 是会造成 ~230 MB 瞬时峰值 + 秒级 GT 停顿的配置，meta 与文档都没提。

附带一处已存在的浪费：`RebuildGroundMesh` 被 **`GroundMaterial` 与 `BaseColor` 的属性变更触发**（CSGroundActor.cpp:533-540 把它们并进 `bShapeProperty`）——换一个材质指针要走上表整整一行。

**建议**：**(A) 必做，与任何架构裁决无关（半小时）**：
1. 把上表写进 `NumCellsX/Y` 的 meta 注释与 README；在 `EnsureMirrorInitialized`（CSGroundActor.cpp:71-96）里对 `WantVertsX*WantVertsY > 262144`（>512²）打一条 Warning 说明峰值内存与重建耗时。
2. 把 `GroundMaterial` / `BaseColor` 从 `bShapeProperty`（:538-539）里摘出来，改走 `TinyGladeMesh->SetMaterial(0, GroundMaterial)`（CSMesh.h:394，广播触发重绑、不重分配）+ 同步 `MeshMaterial`。换材质从「重建 13 万三角」降到一次重绑。

**(B) 不作为建议，而是把数字补进计划已有的开放问题 :776「地面规模上限与分块」/ :777「道路语义通道固定 R 是否够用」。** 真正的解耦要求把道路载体从顶点色换成 512² 单通道纹理（25 cm/texel、262 KB，与现在 Colors 流同量级但分辨率 4 倍且不再与网格绑定），`CellSize` 随之可放到 1.0–1.5 m（TG 的 1.44 m 是实证值），128 m 覆盖下 GPU 4.21 MB → 0.48 MB、峰值 14 MB → 1.6 MB。**但这会让 D2 顶点色球刷退化成纯装饰色、D2 的 parity 验收判据（计划 :739）失效，并牵动 D6 `SampleRoadWeight`、D9 `StepRoadThreshold`、D12 场的 W 通道路缘加成。** D1/D2 已落地、顶点色笔刷是用户明确要的形态——**理由不足以推翻，只提供定价，裁决权归用户**。

**代价**：(A) 极小。(B) 大且跨切片，本条不建议动工，只把量化表贴进 :776/:777 让裁决有数字可依。

**证据**：【代码事实】CSGroundActor.h:34/:76/:80/:85/:91、cpp:73-74/:107-162（网格布局）、:533-540（材质触发全量重建）；CSGpuMeshTypes.h:129-147（快照 Colors 是 FVector4f）；CSMeshOps.cpp:1140-1142（第三份复制）；CSMesh.h:394（SetMaterial 免重建路）。【我的推断（由布局直算）】整张容量表。【逆向报告确凿】§0（130 m / 512²）、§7.1（heightmap r16、≤1024 点/笔）、§7.2（91 列、±65.72 m、无顶点输入、**索引由 CPU 生成**）。【逆向报告推测】§5.2 末句（地形 PS 消费 paths_visual）。【计划】:739、:776、:777。

---

### [WEAKENED] 藤蔓提交链的线程账：README 明写「渲染线程总工作量不变」这一条成立；但用 305 ms 除以 target 数外推出的单价不成立，需先按 README 指定方法实测

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 藤蔓是 CPU 增量生长模拟 + GPU 小件实例化（§6.4）：`ivy_grower` 逐帧只提议一个方向、段完成时 `ivy_leaf_spawner` 加一片叶；GPU 侧 `IvySegmentSsbo/IvyLeafSsbo/IvyFlowerSsbo` 每段一条紧凑记录，枝干 VS 把 12 顶点三棱柱沿段方向拉伸；跨小径不是几何拱起，而是把低于包络的段/叶实例**置 NaN 剔除**。墙改/删走 `RemoveIvyCmd` → `ivy_pruner` **修剪**。成本是每帧 O(生长了几段)，不存在「一次性重算整棵藤」。（报告对「SSBO 是否每帧上传」自标【待确认】。）

**当前方案怎么做**：计划 D13 :730（**用户指出**）：「异步 + 帧保护是现成的……松手瞬间多栋房一起重算也不会卡帧——**不需要在本计划里再加节流或分帧调度**」。P9 验收门 :747 写的是「松手多房齐算不卡帧（依赖现成异步+帧保护）」。

README `Plugins/PCGPlugins/README.md:162-180` 给的是**两个数**：`GenerateVineGPU.Total` **12.4–14.6 ms**（game）与 `tube buildLeaf(wallclock)` **305–311 ms**（render）；:171 紧接着写「游戏线程卡顿降低约 98%，**渲染线程总工作量不变**」。基准：980 个 `GrowTarget`、`SC.Iteration=55`、`VoxelSize=5`，产物 `Vertices=593880 Indices=3563280`。

**差距**：**站得住的部分（CONFIRMED）**：异步搬走的是 GT 阻塞，不是工作量——README:171 白纸黑字。渲染线程一帧只有一个，那张图落在哪一帧，那一帧就长。所以 :730 的「不会卡帧」在**游戏线程意义上**成立、在**渲染线程意义上未经验证**；:747 的验收门测的是 GT，**测不出 RT 长帧**。这一条是真实的口径缺口。

**必须撤回的部分（原稿最脆的推断）**：
1. **305 ms 不是可用的基数。** README:180 自己警告：「`buildLeaf(wallclock)` 混着『图本身的量』和『渲染线程有多堵』……同进程内出现过 164 ms 与 553 ms 的尖峰；要 GPU 侧真实耗时请用 `stat gpu` 或 Insights 抓 `VineMesh.Build`」。用一个作者标注为不可靠的数当分子是方法错误。
2. **「0.31 ms / target」这个单价不成立。** 基准里 `GenerateVineGPU.PrepareSurfaceVoxelInputs` 10–12 ms 是**表面体素化**，规模由 `VoxelSize=5` 与包围盒决定，与 target 数无关；空间竞争求解由 `SC.Iteration=55` 与体素场规模主导。把总耗时线性摊到 980 个 target 上再外推到 6,000 个、得出「1.9 s」，是把一个多变量成本模型压成了单变量。原稿那张「5 房/20 房 × 30/100/300 target」的表因此**不具备定价效力**，只能当作「需要标定」的提示。
3. 「33 MB / 容器」由 593,880×32 B + 3,563,280×4 B 直算，这条成立，但同样只对该基准参数组成立。

**仍然有效的一条结构约束**：`EditMeshAsync` 在同一个 `UCSMesh` 上在途时直接拒绝（CSMesh.cpp:976），完成回调稳定落在下一帧（README:172）。所以计划开放问题 :783「每房一个 vine 容器 vs 共享容器按房分段」不是纯风格选择——共享容器会把 K 栋房的重算强制排成至少 K 帧的 GT 往返；每房独立容器只是把同样的 RT 工作背靠背排队。两条路的 RT 总量是同一个不变量。

**建议**：**(1) 把 :730 那句结论按线程改写（纯文档，零代码，不动任何裁决）**：把「不会卡帧」明确为「**游戏线程**不会卡帧（README 实测 12.4–14.6 ms）；渲染线程侧的单帧墙钟未测」。

**(2) 把 P9 验收门 :747 换成可测口径。** 现在的「松手多房齐算不卡帧（依赖现成异步+帧保护）」测的是 GT。改成：一次提交后连续 30 帧内**渲染线程/GPU 单帧墙钟 ≤ X ms**，取数**必须**按 README:180 指定的 `stat gpu` / Insights 抓 `VineMesh.Build`，**不要**用 `buildLeaf(wallclock)`。

**(3) 先标定再谈闸。** 在决定要不要分帧调度之前，跑一组标定：固定 `VoxelSize`/`Iteration`，把 target 数取 100 / 300 / 980 / 3000，用 `VineMesh.Build` 的 GPU 耗时拟合出「固定项（体素化）+ 随 target 增长项」的两段模型，把结果贴进开放问题 :783。**在这组数出来之前，不建议改动 :730 的用户裁决。** 若标定显示单次提交的 RT 长帧确实超过可接受阈值，届时再提分帧闸，形态可复用 D12 已定的「分级异步状态机 + 代数戳」（计划 :673-688/:729），且与 :727「只在编辑提交时生成、拖拽期间完全不动」两条裁决完全兼容。

**代价**：(1)(2) 零代码。(3) 是一次半天的测量任务（复现脚本 README:181 已给：`Saved/CodexTests/measure_vine_after_move.py`，注意「连续生成必须隔帧」）。**本条明确不推翻 :730** —— 用户裁决的「不加节流」在 GT 口径下由 README 实测支持；本条只指出验收门测错了线程，并要求先拿到正确口径的数再谈。

**证据**：【实测数据，项目 README】`Plugins/PCGPlugins/README.md:162-180`（GT 12.4–14.6 ms / RT 305–311 ms / 593880 顶点 / 3563280 索引 / **:171「渲染线程总工作量不变」** / :172「完成回调落在下一帧」/ **:180 buildLeaf 抖动警告与正确取数方法**）。【代码事实】CSMesh.cpp:976（在途拒绝）、:1107-1113（容量只涨不缩）。【我的推断（已降权）】33 MB/容器仅对该基准参数成立；原稿的 0.31 ms/target 单价与 5/20 房外推表**已撤回**。【计划】D13 :725/:727/:729/:730/:747/:783。【逆向报告确凿】§6.4（增量生长与修剪、12 顶点三棱柱、NaN 剔除跨路）；报告对 SSBO 上传频率自标【待确认】。

---

### [CONFIRMED + 新增一处过期快路径] 反向盘点：CPU 谓词（~340 次镜像采样/栋/唤醒）根本不是瓶颈，石阶已与 TG 同构；但 RaycastGround 的「平地快路径就是全部」注释已被 D9 作废，起伏地面上每次光标 trace 是 700–2900 次 CPU 双线性

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：三处对照【逆向报告确凿】：
1. **拾取**：`_mouse_terrain_raycast.cs` GPU 内 1000 步 raymarch heightmap，写回命中位置 + 笔画 id → `MouseTerrainRaycastSsbo`（§7.3）。**订正原稿**：TG 并非只有这一条——同节还写明 `_terrain_editing_read_terrain_height.cs` 把双 heightmap 回读成 `TerrainHeightsData`，「CPU 副本供 `RaycastWorld::raycast_w_terrain / mesh_follow_terrain` 消费」；房屋侧另有 rapier3d/parry3d 查询世界（§8.5）。GPU raymarch 的独有价值是它**同时返回命中的笔画 id**，那是纯高度查询给不出的。所以「TG 每次拾取都要 1000 步 GPU raymarch」是误读。
2. **单栋房几何量**：brick LOD0 是 600 顶点/300 三角的字典 mesh（§1.6），一栋小屋数千块砖，另有瓦、梁、尖顶、灰泥 chunk、半木梁与 `assets/meshes/decorators/` 228 个装饰件（§8.1）。
3. **石阶**：岩地台阶 100% GPU（`_rocky_terrain_stairs_stairs.cs`，原始 bin 162,979 字节、反编译 3427 行、17 层等值线、`atomicAdd(instanceCount)` → 间接绘制，CPU 回读**仅为音频**，§4.3）。

**当前方案怎么做**：1. `ACSGroundActor::RaycastGround`（CSGroundActor.h:203，实现 cpp:347-403）：`MaxAbsHeight <= KINDA_SMALL_NUMBER` 时闭式平面求交（:358-366），否则先裁地面 AABB 再按 **半格步长** 定步进 march（:383-401）。房屋拾取按 D4 :184 走参数化 OBB。全插件零 `UBodySetup`（`CSGpuMeshComponent.cpp:14` 全线 NoCollision，计划 :761 记为既定架构）。
2. `RebuildBodyMesh`：无门房 88 三角 / 264 顶点 / 9.6 KB；6 拱房 736 三角 / 2,208 顶点 / 80.5 KB（按 CSHouseActor.cpp:57-84 逐面直算）。
3. D9 石阶走 `UCSGpuInstancedMeshComponent` 的 GPU packed 实例源：**5×float4 = 80 B/实例**，`Counter[0]` 是活跃数、注释明写「the count never round-trips to the CPU」（CSGpuInstancedMeshComponent.h:65-79）。

**差距**：**(a) 单栋房便宜三个数量级，这是预算盈余不是缺陷。** TG 一栋房实例数据在 10⁵ B、绘制三角 10⁵–10⁶；当前 9.6–80.5 KB / 88–736 三角。D6 拱间墩、D7 接缝角柱、D9 承重柱、D11 spline 块四种「叠砖语汇」全部加上砖级细节也远吃不满 TG 单栋房的预算。

**(b) 石阶已达 TG 同构。** 80 B/实例 vs TG 96 B/砖、计数不回 CPU vs TG 的 `atomicAdd` + 仅音频回读——**这条没有差距，不该再出现在任何改进清单里**。

**(c) CPU 谓词的真实上界远高于计划的估计。** 一栋房一次 `ReevaluateSite` 的镜像采样次数（默认 600×400 房、`DoorPitchTarget=150`、`DoorSampleStep=25`、`CornerMargin=60`、`PillarSpacing=250`、`WallThickness=24`）：
- `ComputeSeatZ`（CSHouseActor.cpp:167-181）：Step=50 ⇒ NX=12, NY=8 ⇒ 13×9 = **117** 次 `SampleHeight`；
- `ComputeDoors`（:200-243）：南北边 Len=600、Usable=480 ⇒ N=3、Pitch=160、Steps=7 ⇒ 每槽 8 点 ×(2 次 `SampleRoadWeight` + 1 次 `SampleHeight`)=24，×3 槽 ×2 边 = 144；东西边 Len=352、Usable=232 ⇒ N=2、Pitch=116、Steps=5 ⇒ 每槽 18，×2 槽 ×2 边 = 72 ⇒ **216**；
- `ComputePillars`（:268-278）：HX=288/HY=188 ⇒ 边点数 2+1+2+1 = **6**。
合计 **≈ 339 次镜像双线性 / 栋 / 次唤醒 ≈ 5 μs**【推断，按 15 ns/次】。100 栋 = 0.5 ms；60 Hz 笔刷全员直推 = GT 的 3%。**真实上界在千栋量级，不是计划 :142/:770 写的「几十栋」。**

**(d)【本条新发现】`RaycastGround` 的快路径注释已过期，起伏地面上的光标 trace 是 CPU 热点。** :357 的注释写「平地快路径：**当前没有任何高度写入路径（MaxAbsHeight 恒 0），这条就是全部**」——但 D9 塑形物已落地，计划 :755 明确记「这是 `Mirror.Heights` 的第一条写入路径（此前 `MaxAbsHeight` 恒 0）」。有塑形物时走 march 分支：`Step = CellSize*0.5 = 25 cm`，`MaxDistance` 是地面盒对角线（:383-384）⇒ 256² 地面（128 m）约 **724 步**、1024²（512 m）约 **2,896 步**，每步一次 `SampleHeight` 双线性。这发生在 `FCSGroundPaintEdMode::TraceCandidatePoint` 的每一次光标移动上，同一游戏线程 tick。所以「解析拾取严格更省」只对平地成立；起伏地面上的量级（~10³ 次采样）与 TG 的 1000 步 GPU raymarch **同阶**，只是它在 CPU 上、且不占一帧延迟。

**建议**：**(1) 按实测数字改写三处预算判断（纯文档，但会改变后续所有取舍）。**
- 计划 :142 / :770 的「几十栋房屋量级无压力」→ 改成「CPU 谓词约 340 次镜像采样/栋/唤醒（≈5 μs），千栋量级仍是 GT 的个位数百分比；**直推的真实上界由『门集合翻转时的重建 flush 次数』决定，见 flush 预算条**」。开放问题 :792「直推 → CSSceneDirty3D 的切换阈值」因此要重定义——不是「房屋数到多少」，是「**同一帧内触发重建的房屋数**到多少」。
- 把这个估算式（而非单一数字）写进 `ACSHouseActor` 类注释，改参数时能自己重算。把 `DoorSampleStep` 从 25 调到 5、`PillarSpacing` 从 250 调到 50，采样数涨约 5×（~1,700 次/栋 ≈ 25 μs），结论不变。
- 计划 :761「拾取全靠解析」的措辞保持不变即可（它已如实写成既定架构），但**不要**升级成「比 TG 更省」——见 gap(a) 的订正。

**(2) 修 `RaycastGround` 的过期注释与 march 代价（这是本条唯一的代码动作）。**
- 先改注释：:357 的「当前没有任何高度写入路径」已被 D9 作废，留着会误导后人以为 march 分支是死代码。
- 再降代价：march 起点已裁到地面 AABB（:375-381），但步进上界仍取整个盒对角线（:384）。两条便宜的改法——① 起点/终点同时裁到 `[GroundZ-MaxAbsHeight, GroundZ+MaxAbsHeight]` 这段 Z 板（射线在板外的部分不可能命中），把 2,896 步压到与板厚/入射角相关的量级；② 用 `MaxAbsHeight` 做 sphere-tracing 式的自适应步长（`Delta` 远大于 0 时可安全大步跨）。前者约十行、无语义变化，后者要小心薄台阶的漏检。

**(3) 在计划「风险」节加一张「预算去向表」**：CPU 谓词 <1%、快照打包 <1%、**flush 与渲染线程 >95%**。它是后续每条优化取舍的依据——没有它，容易再写出「用空间哈希优化 O(N²) 配对」（:569）这类优化了 1% 的条目（计划 :347 自己也说「村庄量级线性扫 OBB 完全够，空间哈希是纯优化」）。

**代价**：(1)(3) 文档改写，零代码。(2) 注释一行 + Z 板裁剪约十行；验证用 `L_TerrainOpsDemo`（有塑形物、`MaxAbsHeight=300`）在台上台下各点几笔，断言落笔位置与视觉一致。不推翻任何裁决——本条不动 `csh.LiveCutHz` / `csh.LinkRebuildHz`（原稿提议删掉它们的建议已撤回，理由见 rejected）。

**证据**：【代码事实】CSGroundActor.cpp:347-403（含 **:357 的过期注释** 与 :383-384 的步进上界）、CSGroundActor.h:203；CSGpuInstancedMeshComponent.h:65-79（5 float4/实例、计数不回 CPU）；CSGpuMeshComponent.cpp:14（全线 NoCollision）；CSHouseActor.cpp:167-181/:200-243/:268-278（采样循环），参数默认值 CSHouseActor.h:57/:98/:106/:118/:158。【我的推断（逐参数直算）】339 次采样与 724/2896 步 march。【逆向报告确凿】§7.3（**GPU raymarch 与 CPU 高度副本并存**）、§8.5、§1.6、§8.1、§4.3。【计划】:142、:347、:569、:755、:761、:770、:792。

---

### 缺 mesh atlas：一个 palette 条目一个组件——代价在每帧 RDG pass 数与固定开销，不在 VB/IB 冗余（但 part 索引不能塞进 Transform2.w/Transform3.w）

*维度：nani架构_GPU驱动实例渲染层*

**Tiny Glade 怎么做**：§2.1【确凿】：`assets/nani_meshes.ron` 按 subset 注册网格进 `MeshAtlas`，GPU 侧 `MeshAtlasPartInfo{first_index, index_count, bounding_sphere_radius}` 数组形状 `[subset][64]`——每 subset ≤64 个 mesh part，**共享一条大 VB/IB**。§2.2【确凿】：一次 `cull_and_bucket` 覆盖整个 subset 的全部 part，排序条目 = `inst_idx*64 + part(6bit)`，`generate_draw_lists` 每 32 实例窗口按 part 发射 `DrawIndexedIndirectCommand`（`first_index/index_count ← atlas_part_info`）。即 N 种基础网格 = 1 次剔除 dispatch，不是 N 套管线。

**当前方案怎么做**：组件硬性单网格单材质：`CSGpuInstancedMeshComponent.h:187` 单 `BaseMesh`、`:192` 单 `InstanceMaterial`、`:154` 明写无 per-instance custom data。落地照此展开：`CSGroundShaperActor.h:37` 注释"石阶……走自己的 `UCSGpuInstancedMeshComponent`（**每个 palette 条目一个**）"，实现 `CSGroundShaperActor.cpp:398-419` 按 `StepMeshes.Num()` 多退少补；散布侧 `CSGroundShaperSteps.cpp:128-168` 是逐 (曲线 × palette 条目) 一个 scatter dispatch（**但这是重建时一次性的，不是每帧**）。计划 `:782` 开放问题「decor：植被量级上去后按 class 降级为 ISM」预留了同一个坑。

**差距**：三项固定开销随 palette 条目数线性放大，都不是显存：
① **每帧 RDG pass 数**：`FCSGpuInstancedMeshSceneProxy::RunCulling` 每组件每族录 3–5 个 pass（`CSGpuInstancedMeshSceneProxy.cpp:497` `AddClearUAVPass(InstanceCount)`、`:504` `AddClearUAVPass(LodCounters)`、`:528` PackPoints、`:546` ClusterCull、`:575` InstanceCull、`:591` BuildArgs），每个 dispatch 只有几十个线程，属**启动开销主导**形态：`stat gpu` 看不出单个 pass 贵，只看到 RDG 图变大。nani 对同一件事是一个 dispatch。
② **primitive / VF / PSO 数**：每组件一个 scene proxy、一个 `FCSGpuInstancedMeshVertexFactory`（自带 InstanceVF uniform buffer，`CSGpuInstancedMeshVertexFactory.cpp:43-52`）、一份 PSO 预缓存、一个 `UCSMesh`（8 条标准流 + 7 条 aux 流，`CSGpuInstancedMeshSceneProxy.h:32-41`）。
③ **容量下限的浪费**：`ResolveInstanceCapacity`（`CSGpuInstancedMeshComponent.cpp:482`）有下限棘轮，可见缓冲按 `InstanceCapacity × NumLODs × 80 B` 计（`CSGpuInstancedMeshSceneProxy.cpp:248`，64 MiB 告警）。
**必须承认的量级折扣**（原稿高估了）：石阶今天只有 3 个 palette 条目、十几个实例；空/未分配的组件在 `RunCulling` 开头就早退（`:406-409`），不产生 pass。所以"50 座塑形物 × 3 palette × 4 pass = 600 pass/帧"是**假想负载**，不是现状。
注意**不是** VB/IB 冗余——不同 palette 条目本来就是不同网格，共享 VB/IB 只省绑定不省字节。

**建议**：**前置：本轮第 1 条的 spike 必须先出结论。走 GPU Scene 的话这条整条作废**——UE 侧就是每 mesh 一个 ISM primitive，剔除由 `FInstanceCullingMergedContext` 合并，不需要我们做 atlas。以下只在 spike 失败时动工。
把已有的 `[LOD]` 定址泛化成 `[part][LOD]`，数据结构本来就是 atlas 形状：`CSGpuInstancedMeshComponent.h:16-23` `FCSGpuInstancedLODRange{FirstIndex, NumIndices, BaseVertex, ScreenSize}` + `:28` "All LODs live in one vertex buffer and one index buffer"。
1) `FCSGpuInstancedBaseMesh` 增 `TArray<FCSGpuInstancedPart>`，每 part 持自己的 `LODs[]` 与 `LocalBounds`；`SetBaseMesh` 变 `SetBaseMeshPalette(TArray<UStaticMesh*>)`，逐条目追加进同一份 Positions/Indices 并累加 `BaseVertex/FirstIndex`。
2) **part 索引放在源实例行的第 6 个 float4（新增），不要动 Transform 行的 .w**——理由见代价栏。关键认识：part 索引**根本不需要到达 VS**，它只决定实例落进哪个 compaction 桶，几何范围由该桶的 indirect args（`first_index/index_count`）携带。所以可见缓冲的行保持现状 5 个 float4 不变。
3) compaction 从"固定区段"换成"计数 + 前缀和 + 散射"（否则可见缓冲变成 `capacity × parts × LODs × 80 B`）：CountCS（每 (part,LOD) 一个 atomic）→ ScanCS（parts×LODs ≤ 256 桶，单线程组 Blelloch，可与 BuildArgsCS 合并）→ ScatterCS（复用 `CSGpuInstancedMesh.usf:206-227` 的后半段）。这正是 nani `histogram_prefix_sum` + `fill_sorted_instance_list` 去掉深度桶后的骨架（§2.2 步骤 1）。
4) 绘制侧 `CSGpuInstancedMeshSceneProxy.cpp:659-694` 的 `for (Lod)` 变 `for (part, lod)`；`BatchElement.UserIndex`（`:681` 现为 `Lod * InstanceCapacity`）改成把桶起点写进 indirect args 的 `StartInstanceLocation`。argset 上限 65536（`CSMesh.cpp:27-30`）远够。

**代价**：约 2–3 天（component + proxy + usf 三处，加 palette 打包与前缀和两个单测）。三个风险：
(a) **原稿的"Transform2.w/Transform3.w 是白送的两个 float"是错的，已从提案中删除**：`Engine/Shaders/Private/LocalVertexFactory.ush:1376-1383` 的 `VertexFactoryGetInstanceHitProxyId`（USE_INSTANCING 变体）把 `Transform1.w - 256*Selected`、`Transform2.w`、`Transform3.w` 直接当 hitproxy 的 R/G/B 读；而 `CSGpuInstancedMesh.usf:222-224` 把源行**逐字**拷进可见行，所以源行里的 part 索引会原样进 VS。今天全写 0 ⇒ hitproxy id 为空 ⇒ 编辑器点击穿透，正是两处 shader 注释（`CSGpuInstancedMesh.usf:157`、`CSGroundSteps.usf:93`）在守的不变量；写非 0 会让点石阶选中随机 actor。
(b) `MaxInstancesPerLod` 是可见缓冲的**区段步长**，`CSGpuInstancedMeshComponent.h:112-124` 的注释警告过"用了与缓冲不一致的数字就会把幸存者写出区段外，是设备故障或静默垃圾"——换前缀和后这个不变量的守卫要重写。
(c) `ECSGpuInstancedAuxSlot` 从 16 起编号是踩过坑的（`CSGpuInstancedMeshSceneProxy.h:17-30`，与常驻集 AuxVertex slot 0 材质 id 撞过，症状是静默绑 null），新增槽位必须继续往 23、24 排。
收益只在"palette 条目 ≥3 且组件实例少"时明显；石阶当前 3 条目 × 十几实例，单看它**不值**——值的是 `:782` decor 降级 ISM 与 `:787` 门洞砖拱这两个开放问题。

**证据**：【逆向报告确凿】§2.1 `MeshAtlasPartInfo[subset][64]` 与共享 VB/IB、§2.2 的 `inst_idx*64 + part(6bit)` 与前缀和/散射三步。【我的推断】pass 数与固定开销的量化（假想负载，非实测）、前缀和 compaction 的必要性。项目侧行号全部回原文件核对；原稿的 Transform2.w/3.w 挪用方案经核对 `LocalVertexFactory.ush:1376-1383` 后**判定有害**，已替换。

---

### per-instance custom data 已经接好线却被钉死为 0——nani 全部风格化词汇在 UE 侧的唯一等价接口，且比原估更便宜（fade 风险已被现有代码挡掉）

*维度：nani架构_GPU驱动实例渲染层*

**Tiny Glade 怎么做**：§1.4/§1.7【确凿，GLSL 直接给出】：每砖 96 字节 `InstancedWallData`，flags 8 位各驱动 VS/PS 一种变形（1.05 各向同性缩放、查 roof_color_ids 还是 wall_color_ids、随机胀缩、拱圈石反向裁剪、减雪、拱压扁 + 三平面 UV、免高亮、强制 LOD0），另有 seed（`seed&3` 给 4 种 90° 旋转白嫖）、拱高 3 点、`wallspace_x_range`。§2.1【确凿】：nani 侧同一机制体现为 20 种 `InstanceData`，各带自己的逐实例字段（GothicWindowBricks 带 3 个骨点、Plaster 带 `peel_strength`、Wren 带 `flap_t/wing_fold_t`）。§2.3【确凿】：这些字段全在 VS 里消费。结语第 1 条把这条总结为"零 CPU 成本制造手工歪斜风格"。

**当前方案怎么做**：本组件的逐实例可变量**只有一个 float**：packed 行第 4 行的 `.w` = PerInstanceRandom（`Shaders/Private/CSGpuInstancedMesh.usf:161`、`CSGroundSteps.usf:96`，两处共用同一份哈希）。`CSGpuInstancedMeshComponent.h:154` 把"no per-instance custom data"写成既定能力边界。最要命的一处在 `CSGpuInstancedMeshVertexFactory.cpp:47-48`：`UniformParameters.InstanceCustomDataBuffer = InstanceOriginSRV; // unused; must be non-null` + `NumCustomDataFloats = 0`——通道**已经建好了**，只是喂了占位 SRV 和 0。计划侧没有任何条目讨论过逐实例参数化（D9 石阶只有 `lengthScale`，且烘进了变换矩阵，不是可供材质读的参数）。

**差距**：引擎的 manual-fetch 实例路径**支持** per-instance custom data，本项目误以为不支持。三处已逐行回原文件核对：
① `Engine/Source/Runtime/Engine/Classes/Engine/InstancedStaticMesh.h:42-48`——本插件已在用的 `FInstancedStaticMeshVertexFactoryUniformShaderParameters` 里就带 `SHADER_PARAMETER_SRV(Buffer<float>, InstanceCustomDataBuffer)` 与 `NumCustomDataFloats`；
② `Engine/Shaders/Private/MaterialTemplate.ush:1499-1507`：`#elif USE_INSTANCING && USES_PER_INSTANCE_CUSTOM_DATA` 分支读 `InstanceVF.InstanceCustomDataBuffer[asuint(Parameters.PerInstanceParams.w) * NumCustomDataFloats + FloatIndex]`；
③ `Engine/Shaders/Private/LocalVertexFactoryCommon.ush:7`：`NEEDS_PER_INSTANCE_PARAMS` 含 `(!VF_USE_PRIMITIVE_SCENE_DATA && USES_PER_INSTANCE_CUSTOM_DATA)`，本工厂该宏恒 0，所以材质一用 PerInstanceCustomData 节点，`LocalVertexFactory.ush:1004-1006` 的 `PerInstanceParams.w = asfloat(GetInstanceId(GetInstanceIdFromVF(Input)) + InstanceOffset)` 就自动编进来，索引口径与本工厂已在用的 `InstanceOffset`（`CSGpuInstancedMeshVertexFactory.cpp:82`）完全一致。
**这条能力是白送的，只差把 buffer 填上。**

**建议**：**前置：本轮第 1 条 spike 若通过，这条改成用 GPU Scene 的 `InstanceCustomData` payload**（`PCGInstanceDataInterface.cpp:423` 的 `InstanceCustomFloatDataExported` 是现成范例，`PCGSceneWriter.usf` 里 `Params.NumCustomDataFloats` 直接可用），工作量相近但收益覆盖阴影与 Lumen。spike 失败才走下面的自建路：
1) `ECSGpuInstancedAuxSlot` 加 `VisibleCustomData = 23`（`Buffer<float>`，元素数 = `NumCustomDataFloats × InstanceCapacity × NumLODs`），走 `CSGpuInstancedBuildAuxStreamDescs`（`CSGpuInstancedMeshSceneProxy.h:47-50`）声明——**必须**继续从 16 往上排（理由见同文件 `:17-30`）。源侧同样加 `SourceCustomData`，或把 GPU 生产者的 buffer 用 `RegisterExternalBuffer` 接进来（照 `CSGpuInstancedMeshSceneProxy.cpp` 里 `SourceInstances` 的写法）。
2) 组件加 `UPROPERTY(EditAnywhere) int32 NumCustomDataFloats = 0`（clamp 0..8）+ CPU 侧 `TArray<float> PerInstanceCustomData`，随 `PerInstanceTransforms` 一起过 `RebuildInstancePacking()`（`CSGpuInstancedMeshComponent.h:335`）的 Morton 重排。
3) `InstanceCullCS` 的 compaction 段（`CSGpuInstancedMesh.usf:218-226`）后补 `for (k < NumCustomDataFloats) RWVisCustomData[Dst*N+k] = SrcCustomData[Instance*N+k];`。
4) `FCSGpuInstancedMeshVertexFactory` 加 `SetCustomDataStream(SRV, Count)`，把 `.cpp:47-48` 两行改成真值；`OnStreamsAllocated` 里一并解析新槽并做同样的"解析不到就报错"处理。
5) 材质侧零改动：用标准 PerInstanceCustomData 节点，仍只需勾 Used with Instanced Static Meshes。
首个消费者按计划优先级挑 D9 石阶：散布 shader 已经算出 `Alpha`/`LengthScale`（`CSGroundSteps.usf`），把"第几级/弧长位置/朝向"喂给材质做色相与磨损微差是白拿；再往后是 `:787` 开放问题的门洞砖拱——那个一旦做，flags 位表这类东西就是刚需。

**代价**：约 1 天，4 个文件各几行；显存 `NumCustomDataFloats × 4 B` / 可见槽（对比已有 80 B/槽，4 个 float 只多 20%）。
**原稿的风险 (b) 经复核已被现有代码挡掉，成本应下调**：打开后 `NEEDS_PER_INSTANCE_PARAMS` 由 0 变 1，`LocalVertexFactory.ush:985-1013` 的 USE_INSTANCING 分支会真的执行 `PerInstanceParams.y = InstancingFadeOutParams.z*Selected + .w*(1-Selected)`，y==0 会关掉 WPO 并被 `MaterialTemplate.ush:2830` 当隐藏处理——但 `CSGpuInstancedMeshSceneProxy.cpp:284` 已经填的是 `FVector4f(UE_BIG_NUMBER, 0.0f, 1.0f, 1.0f)`，z=w=1 ⇒ y 恒为 1，实例不会被 fade 掉。仍建议加一条"批次 loose 参数 z/w 必须为 1"的断言，防将来有人改这行。
**唯一剩下的真风险**：Morton 重排（`RebuildInstancePacking`）必须同步搬 custom data，漏了症状是"参数和实例错位"且完全不报错——要写一个单测钉住。
**不与任何用户裁决冲突**：计划全文没有讨论过逐实例参数化。

**证据**：【逆向报告确凿】§1.4 96B/砖、§1.7 flags 位表、§2.1 的 20 种 InstanceData、§2.3 VS 变形、结语第 1 条。【我的推断】"UE 的 manual-fetch 路径支持 custom data"这一条虽是推断，但三处引擎源码（InstancedStaticMesh.h:42-48 / MaterialTemplate.ush:1499-1507 / LocalVertexFactoryCommon.ush:7）与激活链（LocalVertexFactory.ush:985-1013）已逐行核对；fade 风险的证伪依据是 CSGpuInstancedMeshSceneProxy.cpp:284 实读。

---

### 剔除服务是全进程全局且不按 scene 过滤：一个 view extension 对所有注册 proxy 跑 cull，跨 world / 跨视图族空转

*维度：nani架构_GPU驱动实例渲染层*

**Tiny Glade 怎么做**：§2.2【确凿】：nani 的剔除是每 subset 一次 `cull_and_bucket`，输入是该 subset 的 16 字节紧凑剔除记录数组，作用域天然限定在当前 world 与当前视图族；108 条 draw list = 27 subset × 4 视图，视图数量是**枚举出来的**，不是"碰上哪个族就跑哪个"。§1.5【确凿】墙砖专用管线同样是每面墙一个 `(InstancedWall, WallTriggerVolume)` 实体、pass1/pass2 明确对应主视图，阴影另走 3 个级联槽。

**当前方案怎么做**：`CSGpuInstancedMeshSceneProxy.cpp:158-197`：一个静态 `TSet<FCSGpuInstancedMeshSceneProxy*> GetRegisteredProxies()` + 一个进程唯一的 `FCSGpuInstancedCullViewExtension`（`:205-211` `FSceneViewExtensions::NewExtension`）。`PreRenderViewFamily_RenderThread`（`:173-176`）把 `bCulledThisFamily` 复位，`PreRenderView_RenderThread`（`:178-192`）在每族第一个视图上 `for (Proxy : GetRegisteredProxies()) Proxy->RunCulling(GraphBuilder, InView);`。`RunCulling`（`:393-409`）的早退只有四条：DrawDesc 无效、Layout 无效、Resident 未分配、`AllocationGeneration` 不匹配——**没有一条与"这个 proxy 属不属于当前 scene"有关**。头注释 `CSGpuInstancedMeshSceneProxy.h:66-72` 讨论过分屏一致性（"cull runs once per frame for the first view… over-draw rather than under-draw"），但没讨论跨 world / 跨族。

**差距**：**跨 world / 跨族空转**：编辑器 world 与 PIE world 并存、内容浏览器缩略图渲染、以及本插件自己的 `ComputeShaderSceneCapture` 起的 capture 族，每一个族都会把**全进程**所有实例 proxy 的 cull 跑一遍；属于别的 scene 的 proxy 用这个族的视锥算出一份结果，随后它自己的族再算一遍覆盖掉——结果正确（因为它不会在别人的族里被 draw），但工作量是族数倍。与本轮第 2 条叠乘就是 组件数 × 族数 × 4 个 pass，每个 dispatch 只有几十线程，属启动开销主导形态。
这是一个 O(全局 proxy 数 × 族数) 的循环，正确量级应是 O(本族 proxy 数)。
**必须下调的部分**：今天该组件的实例只有石阶（3 个）与点刷（1 个），空组件在 `:406-409` 就早退，所以现状浪费是微不足道的；值得改的理由是**成本近零 + 结构性**（`:782` decor 降级 ISM 后组件数会上量），不是当前性能。

**建议**：只加**一道**过滤，在 `CSGpuInstancedMeshSceneProxy.cpp:191` 的循环里：
`if (&Proxy->GetScene() != InView.Family->Scene) continue;`
（`FPrimitiveSceneProxy::GetScene()` 是 public inline，`Engine/Public/PrimitiveSceneProxy.h:733` 返回 `FSceneInterface&`；`FSceneViewFamily::Scene` 是 `FSceneInterface*`，比指针即可。）这一条是纯净收益，零行为变化。
**原稿的第二道"视锥过滤"必须删除或加条件**——它不安全：`CSGpuInstancedMeshSceneProxy.h:68-72` 明确记录了"一族只在第一个视图上 cull，其余视图沿用主视图的可见集，宁可 over-draw 不可 under-draw"这条不变量。按第一个视图的视锥跳过 cull，在分屏 / 多视图族里会让"视图 1 看不到但视图 2 看得到"的 proxy 拿到上一帧的陈旧 indirect args，把 over-draw 变成 under-draw/错画。要做只能二选一：(i) 仅当 `InView.Family->Views.Num() == 1` 时启用；或 (ii) 对 `Family->Views` 全部视锥取并集测试。收益本就很小，建议**先不做**。
顺手把 `RunCulling` 开头 `DiagnosticState == Pending` 的日志（`:397-400`）挪到过滤之后，否则每族都打一次。
验收：`L_TerrainOpsDemo` 里同时开编辑器视口与 PIE，用 RenderDoc 或 `r.RDG.Debug 1` 数 `CSGpuInstanced.Cull` scope（`:411`）的出现次数——改前 = 族数 × 组件数，改后 = 本族组件数。

**代价**：半小时以内：一行判断 + 一段注释 + 一次目视验收。风险接近零（scene 指针比较无歧义，不改变任何族内行为）。不触碰任何用户裁决。

**证据**：【逆向报告确凿】§2.2 nani 的 subset 级剔除与 108 = 27×4 的枚举式视图划分、§1.5 墙砖每墙一实体。【我的推断】跨 world/跨族空转的后果与量级。项目侧代码路径逐行核对（`CSGpuInstancedMeshSceneProxy.cpp:158-197`、`:393-409`、`:632-694`、`.h:66-72`）；引擎侧 `PrimitiveSceneProxy.h:733` 已核对。原稿的视锥过滤提案经核对头注释 `:68-72` 后判定与既有不变量冲突，已降级。

---

### UCSMesh 逐顶点语义预算实际只有 4 字节 RGBA8 且全白空转；"再加一条 UV 流"这条扩容路在 proxy 绑定处有静默地雷——但回读侧已按另一种口径设计好，两处必须一次对齐

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§9.1【逆向报告确凿，实测解析】mesh JSON 统一 schema，dtype 只有 float×3/2/1 与 int×1 四种，但**属性名是开放的**：41 种自定义语义顶点属性 / 全库 47 种组合，与反编译 VS 的 `in_var_<属性名>` 一一衔接（is_bevel/bevel_offset、prim_center/appear_pos/age、wind_bend/stem/flower_pivot、rest_pos/pet_pos/is_wing/wing_t、roof_profile_mult、autumn_color/flowery_color）。§1.6/§6.3【确凿】这些属性正是倒角、生长、摆动、换季全部程序化风格化的物质基础；§9.2【确凿】nani_meshes.ron 中 8 个 subset 用 `attribs.remove/add` 把同 subset 顶点布局归一化。

**当前方案怎么做**：8 条流是固定集（`CSGpuMeshTypes.cpp:7-118` BuildStandardTriangleStreamDescs；`CSMesh.cpp:98-105` 强制 bMaterialIds/bReadbackColors；`CSMesh.h:78` 注释）。其中**只有 4 条能被材质看见**：Position / TangentBasis / TexCoord0 / Color。AuxVertex 明写"extension slot: per-vertex data with no built-in VF role"（`CSGpuMeshTypes.h:63`），proxy 的绑定 switch 对它是 `default: break; // MeshCounters, AuxVertex: no VF / draw binding`（`CSGpuMeshSceneProxy.cpp:325-326`）——扩展槽只到 compute，到不了材质。实际占用：房屋每顶点写常数白 `S.Colors.Add(FVector4f(1,1,1,1))`（`CSHouseActor.cpp:41`），UV0 是逐面平铺 200 cm（`CSHouseActor.cpp:17` + AddQuad `:48-53`）；地面 R = 道路权重（`CSGroundActor.h:34`），BaseColor 默认 (0,0,0,1)（`CSGroundActor.h:97`）⇒ G/B 空、A 恒 1。CPU 快照支持 4 条 UV（`CSGpuMeshTypes.h:127/136-141`），但上传路径只打包通道 0（`CSMeshOps.cpp:1062/1108`，`TexCoords[Vertex] = Snapshot.TexCoords()[UVIndex]`）。

⚠️ **本条已过期（2026-09-04 复核）**：多 UV 通路已打通，详见 [卷一 2.3](#23-落地步骤改哪些文件) 的同名说明。

**差距**：今天房屋真正可用的逐顶点参数就是 **4 字节 RGBA8**（且全白空转），地面剩 GBA 三通道。TG 那套"每砖 seed / 3 点拱高 / bone_0/1/2_pos 弯曲骨点 / 换季色"里只有 seed 和一两个标量塞得进。**扩容路上有一颗静默地雷，我在引擎源码里确认了它**：再声明一条 `{Role=TexCoord, TexCoordIndex=1}` 的流时，`CSGpuMeshSceneProxy.cpp:303` 的 `Data.TextureCoordinatesSRV = S.SRV` 会被后声明的流整体覆盖（只赋一次单指针），`:305` 把 `NumTexCoords` 抬到 2；而 D3D12/SM5+ 上 `MANUAL_VERTEX_FETCH` 恒被定义为 1（`LocalVertexFactory.cpp:306-308`），uniform buffer 取 `VertexFetch_TexCoordBuffer = GetTextureCoordinatesSRV()` 与 `NumTexCoords`（`LocalVertexFactory.cpp:89/115/120`），shader 按**单条交错缓冲**取数 `Buffer[NumFetchTexCoords * (VertexOffset + VertexId) + i]`（`LocalVertexFactory.ush:734`）。两条独立缓冲 ⇒ 连只用 UV0 的材质都会按 stride 2 读进 UV1 那条缓冲并越界，且 proxy 关了 `bVerifyUsedMaterials`（`CSGpuMeshSceneProxy.cpp:25`），不会有任何报错。**但原结论漏了一件关键事**：回读侧**已经**按"每通道一条独立流"设计好了——`CSMesh.cpp:686-698` 扫所有 `CpuSemantic==TexCoord` 的流取 `max(TexCoordIndex)+1` 当通道数，`:742-746` 按 `Read.Desc.TexCoordIndex` 分发。也就是说：**项目预期的扩容形态是"多流"，而 proxy 绑定实现的是"单交错流"，两处口径本来就不一致**，这才是地雷的真正根因，不是单纯"没人加过 UV1"。

**建议**：**先在一处把两种口径对齐，再谈花预算。**
1）二选一，写进 `CSGpuMeshTypes.h` 的流描述注释里（这是决策点，不是自动跟随原方案）：
   - **(A) 多流口径**（顺着现有回读设计，改动最小）：`CSGpuMeshSceneProxy.cpp:299-311` 的 `case TexCoord` 保留 `Data.TextureCoordinates.Add(...)`，但**不再赋 `TextureCoordinatesSRV`**，改为在 `OnStreamsAllocated` 之前把 N 条 UV 流拷进一条内部交错 SRV 缓冲专供 MVF——代价是一次额外拷贝，回读/存盘/单测一行不动。
   - **(B) 单交错流口径**（改动大但零运行时拷贝）：`CSGpuMeshTypes.cpp:36-49` 的 TexCoord desc 加 `FStandardStreamOptions::NumTexCoordSets`（默认 1），`ElementsPerUnit = 2*Sets`；proxy 里 `for (i<Sets) Data.TextureCoordinates.Add(FVertexStreamComponent(&S.VB, i*8, VertexStride, VET_Float2))`，`TextureCoordinatesSRV` 只赋一次；**同时必须重写 `CSMesh.cpp:686-698` 的通道数推导与 `:742-746` 的解交错**，因为那段现在假定的是多流。
   两条都要照 `CSGpuMeshObjectTests.cpp:1478-1520` 的 aux 流用例补一条"写两套 UV → 回读 → 逐位比对"单测。
2）**先花掉顶点色那 4 字节**（与 1）无依赖，可立刻做）：把 `FCSHouseMeshWriter::AddTri`（`CSHouseActor.cpp:41`）的常数白换成语义写入——R = 构件类型枚举（墙/檐/柱/门框，量化 8 级）、G = 离地归一化高度、B = hash(HouseGuid, 面索引) 的 8-bit seed、A = 风化/苔藓权重。只改一个 `Add` 的实参，不动任何流、不动哈希，立刻让"每面墙不同随机相位"在材质里可算。
3）只有真需要 float 精度的逐顶点向量（拱剖面、弯曲骨点）时才启用 UV1/UV2——那时第 1 步已经把口径统一了。

**代价**：第 2 步是纯收益、一行量级。第 1 步纯改造无功能收益，但它是 UV1 的前置：不做则第一次加 UV1 会撞上无报错的黑洞（症状是"贴图错位/闪烁"，最难往回追）。另一个易漏点：编辑器构建下 `LocalVertexFactory.cpp:494-498` 的 `#if !WITH_EDITOR` 使顶点声明分支**不会**被 MVF 跳过（UE-165187 的遗留），所以编辑器里还会额外多出一个 UV 顶点元素——无害但会让"编辑器好使打包坏"这类误判更难查。显存：每多一套 UV 每顶点 +8 B（地面 256² = 66049 顶点 ≈ +0.5 MB，1024² ≈ +8 MB；房屋千级三角可忽略）。UE 侧硬上限：`FLocalVertexFactory` 的 UV 属性槽只有 `MAX_STATIC_TEXCOORDS/2 = 4` 条（`LocalVertexFactory.cpp:528-548`），加上当时的 `FCSGpuMeshCPUData::MaxTexCoordChannels = 4`（⚠️ **2026-09-04 已抬到 8**；属性槽只有 4 条那一半仍然属实，第 5 组起只走 manual fetch 的 SRV），天花板是 8 套 UV = 16 个 float，离 TG 的 41 种属性仍差一个数量级——这个差距靠加流抹不平，只能靠上一条的逐图元通道 + 把语义压到 8-bit。

**证据**：TG 侧【逆向报告确凿】（§9.1/§1.6/§9.2）；项目侧全为代码事实（含我新核出的 CSMesh.cpp:686-698/742-746 多流回读，原结论未提）；UV1 地雷为【我的推断】，由 UE 5.7.4 引擎源码（LocalVertexFactory.cpp:89/115/120/306-308/494-498/528-548、LocalVertexFactory.ush:734）与插件绑定代码交叉推出，未实测。

---

### "顶点色 + 小查找纹理、不用 PBR 贴图集"这条路线在项目侧是零起点；更急的是顶点色四通道正被先到先得地占用，全项目没有一处文件仲裁哪个通道归谁

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§9.4【逆向报告确凿】389 个 .texture 内容以调色板/渐变查找纹理为主（brick_colors / canopy_alpha / blue_noise），"渲染主要靠顶点色 + 小查找纹理，非传统 PBR 贴图集"是报告原话；§9.1【确凿】运行时吃的 brick.json 是 600 顶点、带 is_bevel、**无 UV**（带 UV 的那份是 Houdini 导出的 glb，运行时不读，Cargo.lock 无 gltf crate）；§1.6【确凿】颜色 id 按 flags&2 查 wall_color_ids[]/roof_color_ids[] SSBO；§8.3【确凿】改色只写 SSBO 不重建 mesh。⇒ 整套外观的物质基础是"顶点语义 + 逐实例 id → 小 LUT"，几乎不需要 UV 展开与贴图预算。

**当前方案怎么做**：`CSHouseActor.h:83-90` 的 WallMaterial/RoofMaterial/PillarMaterial 与 `CSGroundActor.h:91-93` 的 GroundMaterial 全是空 `TObjectPtr`、无 `ConstructorHelpers` 默认值，空槽兜底是 `UMaterial::GetDefaultMaterial(MD_Surface)`（`CSGpuMeshComponent.cpp:49-52`）。`Content/HouseTest/` 实测只有 `BP_GroundShaper` / `BP_TinyGladeGround` / `BP_TinyGladeHouse` / `L_HouseGroundDemo` / `L_TerrainOpsDemo` / `SM_StoneStep_{S,M,L}` ——**零材质资产**，关卡里三个 actor 实际画的是 WorldGridMaterial。地面材质注释写着"顶点色混合：基底与道路层按 R 通道混"（`CSGroundActor.h:91`），计划 `:752` 自己也记了"'顶点色混合'的地面材质资产仍待补"。笔刷侧 `PaintChannelMask` 默认 (1,0,0,0)（`CSGroundActor.h:121`），但 `PaintVertexColorsSphere`（`CSMeshOps.h:356`）允许任何调用方写任意通道。

**差距**：**缺的不是一张贴图，是一份契约。**顶点色四通道是全项目最稀缺的逐顶点资源（见上一条：房屋今天只有这 4 字节），现在的占用方式是先到先得——地面 R 已归道路，其余通道谁先用谁占，没有任何一处文件仲裁。等 D9 要加苔藓/湿度、D12 要加踩踏、D8 要加窗周脏迹时冲突改起来要重建整张地面网格（`RebuildGroundMesh`，`CSGroundActor.cpp:169-181`，131,072 三角）。同时"墙面/屋顶/地面到底长什么样"在项目侧完全没有起点。**修正原结论两处**：① "计划全文没讲材质走向"过头了——计划 `:752` 明确把地面材质资产列为待补项，这是**已知 TODO 而非盲区**，缺的只是通道语义表这一层；② "计划说 3 槽、代码写 2 槽已经对不上"是误判——计划 `:182` 把槽 2 明标"装饰砖（**预留**）"，而需要它的 D7 接缝角柱（计划 `:294` 用槽 2）尚未落地，代码 `CSHouseActor.cpp:350/455` 只声明 2 槽是正确的当前态，不是缺陷。

**建议**：**先立表，再画材质。**
1）在 `Plan_House.md` 新开一节（建议 D14「外观：顶点色通道语义表 + 材质分层」），把两张表钉死并同步写进 `CSGroundActor.h:34` 与 `CSHouseActor.h` 的成员注释旁：地面 Color = R 道路权重（已用）/ G 苔藓浓度 / B 湿度泥泞 / A 踩踏磨损；房屋 Color = R 构件类型枚举 / G 离地归一化高度 / B 逐面 seed / A 风化（与上一条第 2 步同一张表）；逐图元参数表见第 1 条。宁可留空通道，也要一次定够——语义表一旦发布就等于 API。
2）**一个 master material + 若干 MI，而不是每种材料一个 M_**：新建 `M_TinyGladeSurface`，输入只有四样——顶点色四通道、UV0、CustomPrimitiveData（第 1 条接通后）、两张小贴图（`T_PaletteLUT` 256×N，行=材料/季节、列=通道混合结果，对应 TG 的 brick_colors；`T_BlueNoise`）。不做 albedo/normal/roughness 贴图集，法线扰动与粗糙度用 VertexNormalWS + 顶点色驱动的程序噪声。项目里已有可抄的起点：`Content/MeshBoolean/M_VertexColor.uasset` + `M_VertexColor_Inst.uasset`。
3）落地资产到 `/PCGPlugins/HouseTest/Materials/`：`M_TinyGladeSurface` + `MI_Wall / MI_Roof / MI_Stone / MI_Ground`，设成三个 BP 的默认值，这样 `L_HouseGroundDemo` 一打开就不是方格白模。
4）**第三个材质槽等 D7 一起加**，别现在加：`CSHouseActor.cpp:455` 的 `Materials` 数组多一项就多一个空 section（画 0 索引的 no-op draw，语义见 `CSMeshOps.h:399-402`），在 D7/D6 墩样式落地前买不到任何东西。

**代价**：一次美术/材质工作量（几小时到一天），代码侧只有默认材质指针一处小改。**不推翻任何用户裁决**——计划里没有关于材质与顶点色语义的裁决，这是纯空白 + 一个已记录的 TODO。UE 侧要注意：`MaterialRelevance` 在多 batch 路径上取**并集**（`CSMeshRenderComponent.cpp:32-35`，注释解释了不并集会让半透明 section 被过滤掉），所以几个 MI 的 blend mode 要一致，否则会把不透明 section 拖进半透明队列；地面材质要能吃 `VET_Color`（GPU 侧是 RGBA8 `PF_R8G8B8A8`，与 CPU 镜像的 `TArray<FColor>` 和快照的 `TArray<FVector4f>` 三种表示并存，笔刷 CPU 孪生必须逐位复现 8-bit 混合，`CSMeshOps.h:349-351`）。

**证据**：TG 侧【逆向报告确凿】（§9.4/§9.1/§1.6/§8.3）；项目侧全为代码事实（含 Content/HouseTest 与 Content/MeshBoolean 目录实测）；两处修正由回读计划 :182/:294/:752 得出。

---

### 原型资产（cutter / 叠砖块 / 石阶盘 / 摆件盘）没有跨消费者的格式契约，两套 palette 已在用互相矛盾的长度轴与 pivot 约定，且失配是静默的

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§9.1【逆向报告确凿】brick.glb 是 Houdini 源资产遗留（glTF generator = "Houdini GLTF 2.0 Exporter"，312 顶点带 UV），运行时吃的是 brick.json（600 顶点带 is_bevel、无 UV）；Cargo.lock 901 依赖无 gltf crate、exe/pdb 对 glb/gltf 零命中——DCC 格式只活在制作期。**"整条资产链源头是 Houdini"报告自己标的是【合理推测】，不是确凿**。§9.2【确凿】nani_meshes.ron 是启动时加载、可热重载的 atlas 构建清单，其中 8 个 subset 用 `attribs.remove/add` 把同 subset 的顶点布局**归一化**——中间有一步显式的布局归一化。

**当前方案怎么做**：计划把 D6 的拱/矩形/圆 cutter（`:9`「形状是有限的原型集合，每种一份原型资产」、`:159-170`、`:391-410`）、D7 的叠砖块（`:294`）、D9 的石阶盘（`:451`）、D12 的摆件盘全部押在"原型资产"上，但只有开放问题 `:785` 列了"原型形状清单"，没有一处规定原型资产的**格式**。入口共三条、全部只吃 `UStaticMesh` 的 LOD0 渲染缓冲、全部只取 UV0 + 顶点色：`UCSMeshOps::CopyFromStaticMesh`（`CSMeshOps.h:231`，实现 `CSMeshOps.cpp:614-643`）、`CSSplineBlock_ExtractPaletteEntry`（`CSSplineBlockActor.cpp:40`，UV 取 `:79` 的 `GetVertexUV(V,0)`）、`UCSGpuInstancedMeshComponent::RebuildBaseMeshSnapshot`（`CSGpuInstancedMeshComponent.cpp:159` 起，UV/色在 `:211-212`）。`CSStaticMeshAssetSink`（`.h:23-31`）是**出口**不是入口。**约定已经分叉且我逐行核实了**：`ACSSplineBlockActor` 块长取 bounds 的 **X**（`CSSplineBlockActor.cpp:59`）且把重心移到原点、忽略作者 pivot（`:56-58`）；`ACSGroundShaperActor` 石阶长取 bounds 的 **Y**（`CSGroundShaperActor.cpp:181` 注释 + `:188`，头文件 `.h:74` 写明"长度轴 = 局部 +Y"）且**尊重**作者 pivot（`CSGroundSteps.usf:87-88` 直接用曲线位置当 origin）——而两者调用的是同一个 `ACSSplineBlockActor::SolveBlockLayout`（`CSGroundShaperActor.cpp:301`）。

**差距**：同一块从 DCC 导出的石头，放进 SplineBlock 与放进 StepMeshes 会得到两种长度解释、两种落点，而且**失配是静默的**——只表现为"石阶铺出来长度不对"，不报错。轴向、pivot、单位尺度、材质槽数、是否带 LOD、顶点色通道含义、是否必须勾 Allow CPU Access（只在 `CSSplineBlockActor.cpp:36` 的注释里提过一句）——这些约束今天散落在两个头文件的注释里且互相矛盾，没有任何一处能让作者在导出前查到。这是纯资产格式议题，与第一轮六维度（开洞机制 / 墙体表示 / 屋顶瓦片 / 地形笔刷 / 变更传播 / 摆件放置）都不相交。

**建议**：**约定先于工具，且不动已验收的资产。**
1）新增 `PLUGIN/Docs/prototype-asset-contract.md`（或计划里一节），钉死：**pivot = 底面中心**（对齐 TG 砖的"中心在原点的单位盒"，§1.5）、**1 uu = 1 cm**、**材质槽 0 = 主体**、**顶点色按上一条的通道表**、**必须勾 Allow CPU Access**、**LOD 由作者出**（见下一条）。
2）**长度轴不强行统一，改成显式声明**：给每个 palette 消费者加一个 `EAxis::Type LengthAxis` 成员（`ACSSplineBlockActor` 默认 X、`ACSGroundShaperActor` 默认 Y，与今天行为逐位一致），提取处读它而不是硬编码（`CSSplineBlockActor.cpp:59`、`CSGroundShaperActor.cpp:188` 各一行）。理由见代价栏——石阶的 +X 已经被"踏面进深"占了语义（`CSGroundSteps.usf:84-91` 的 LocalX/LocalY 分工），把长度轴搬到 X 只是把矛盾挪个位置。
3）加一个 `CSPrototypeAssetValidator`（放 PCGEditorProcess，或做成 `UEditorValidatorBase`），对被任一 palette 引用的 `UStaticMesh` 校验上述项 **+ "最长边是否就是声明的 LengthAxis"**，不合规在 Message Log 报出来。**这一步比任何 DCC 工具重要**——它把静默失配变成显式报错。
4）（可后置）Houdini 侧做 `tg_prototype_export` HDA：校正 pivot/单位 → 按通道表烘 Cd → polyreduce 出 LOD1/2/3 → FBX 导出到 `/PCGPlugins/HouseTest/Prototypes/`。FBX 是唯一能把"多套 UV + 顶点色 + 多 LOD"一次带进 UStaticMesh 的通道；Houdini 的自定义 point 属性**进不来**，必须先烘进 Cd 与 UV1..3——这正是第 2 条第 1 步要打通的口子。
5）`CSStaticMeshAssetSink` **不改用途**（它是产物落盘口）；要扩入口就扩 `CSSplineBlock_ExtractPaletteEntry`（`CSSplineBlockActor.cpp:79`）与 `RebuildBaseMeshSnapshot`（`CSGpuInstancedMeshComponent.cpp:211`），让它们带上 UV1..3。

**代价**：第 1-3 步半天到一天，且**零破坏性**：LengthAxis 默认值保持现行为，`SM_StoneStep_*` 不用重做，D9 的无头断言（计划 `:755` 的"画路穿台 → 27 级石阶"、"台顶 300 / 裙边中点 150 / 影响外 0"）一条都不用重跑。第 4 步依赖 Houdini 工作量，可后置。风险点：FBX 顶点色在 UE 导入侧可能被当 sRGB 处理或被 `bReplaceVertexColors` 覆盖——注意插件两条提取路径的口径本身就不同（`CSSplineBlockActor.cpp:80-88` 用 `ReinterpretAsLinear()` 按字节直读，`CSGpuInstancedMeshComponent.cpp:212` 用 `ToPackedARGB()`），导出与导入设置必须一起写进契约，否则上一条的通道语义表在过 DCC 时会悄悄变值。另需注意常驻流面法线口径 `cross(B-A, C-A)` 与 UE StaticMesh 差一个负号（`CSMeshBuild.h:26-41`），提取路径靠"交换角点 1/2"处理（`CSSplineBlockActor.cpp:91-101`），这条被单测钉死、**不能"统一掉"**。

**证据**：TG 侧【逆向报告确凿】（§9.1 的 generator 字符串与 brick.json 属性、§9.2 的布局归一化）；"链条源头是 Houdini"在报告里明确标为【合理推测】，此处按推测对待；项目侧全为代码事实，轴向/pivot 分叉是我逐行核对得出。

---

### 光照代理与 raster 表示的一致性约束：masked 洞在 UE 的距离场与 Lumen 追踪里是实心墙

*维度：光追GI代理_光照与阴影的一致性*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 的 raster 侧把形状精修下放到像素级、且**阴影一致性显式保住**：拱洞由跨拱砖携带 3 点拱高、gbuffer PS `if (world_y < 拱高) discard`，**depth-only/shadow PS 变体（f0adff76，全文 37 行）只保留同样的拱裁剪 discard**（§1.6）；瓦片天窗开洞同样在 depth-only PS 里遍历 `DormerHoleSsbo` 逐像素挖（§3.2）。结语第 3 条【确凿】把这条上升为全局设计要点。
【逆向报告推测/工具链确凿】洞口在 CPU 侧被 `trim_rows::{RowTrimmerSink,TrimmedRow}` 按洞修剪砖排——**工具链存在是【确凿】，但「按 `FinalizedWallHoles::iter_holes_with_padding` 裁砖」这一步出现在 §1.2 明确标注为【推测】的排砖顺序链里**（节标题即「【确凿（工具链），排砖顺序为推测】」）。
【我的推断】`construct_walls` 写 `RtWorld`（§1.3【确凿】）吃的是这份已裁洞的砖布局——报告未直接陈述。prefab 的 rt 表示更是另一套粗代理 `mesh:Aabb`（§9.3【确凿】）。

**当前方案怎么做**：当前 `ACSHouseActor::RebuildBodyMesh`（`Private/CSHouseActor.cpp:343`）是**真几何洞**：拱跨按 `CSHouse_ArchSegments` 分段，逐段手排外脸/内脸/拱腹/顶带四组三角（`:371-411`），墙体实心段只铺到拱两侧（`:363-368` 的 `AddSegment`）。计划 D6 的裁决形态同样是真几何——`Plan_House.md` 第 184 行「**开洞用 MeshBoolean，洞本身是参数化的**（用户裁决，推翻早先的网格跳格方案）……门/窗洞统一走 `UCSMeshOps::ApplyMeshBoolean` 减 cutter 体」，进度注记里也写明当前切片「门洞未走布尔……拱洞在 `RebuildBodyMesh` 里参数化生成，效果与 D6 规格一致」。

**差距**：**本条只保留光照维度独有的那个切面：UE 里 masked 材质的洞只在一部分通路里是洞，分界线正好切在这个项目将来最想要的地方。**（开洞机制本身属第一轮维度①，不在此重复。）
- **是洞**：base pass、depth prepass、阴影深度（SM 与 VSM 都会为 `!bWritesEveryPixelShadowPass` 的材质编译真 PS，`Renderer/Private/ShadowDepthRendering.cpp:371,488`）、custom depth。这一半与 TG 的「shadow PS 保留 discard」等价。
- **不是洞（实心）**：① 网格距离场离线生成只按 blend mode 分「半透明 vs 不透明或 masked」，masked 三角**全量计入**、opacity mask 从不求值（`Developer/MeshUtilities/Private/MeshRepresentationCommon.cpp:303-304`）；② Lumen 硬件光追的默认追踪用的是**全局 hit group** `FLumenHardwareRayTracingMaterialHitGroup`（一个 `FGlobalShader`，`Renderer/Private/Lumen/LumenHardwareRayTracingMaterials.cpp:48-80`，仅 2 个 hit group、其 any-hit 是自相交规避而非材质掩码），根本不走材质 any-hit，掩码不生效；③ 软件 Lumen 走 mesh SDF，等同 ①。
结论：per-pixel clip **今天完全安全**（gpumesh 既不进距离场也不进光追也不进 Lumen），**修阴影之后也仍然安全**（阴影通路认 mask）；它**只在引入 GI 代理的那一刻变错**，而且错法致命——拱门在 GI 里被当实心墙，光不会从拱洞洒进室内，拱周围被 DFAO 当实墙压暗。那正是 TG 连拱最标志性的一张图。

**建议**：**零代码，一条写进 `Plan_House.md` D6/D8 与 `Public/CSHouseActor.h` 头注释的纪律：**

> **光照代理表示必须保留真几何洞口；逐像素 clip 只允许存在于 raster 表示。禁止出现「渲染网格是整块墙 + clip 挖洞，而 GI/距离场代理是同一块整墙」的组合。**

落到本项目：
1. `RebuildBodyMesh` 现在的参数化拱几何（`Private/CSHouseActor.cpp:371-411`）就是唯一几何权威，同时供 raster 与将来的光追/烘焙代理。**这一点今天已成立，代价为零**；D6 转布尔真洞后同样成立。
2. 采纳上一条的 B 路（光追实例）时，BLAS 直接从常驻流位置/索引缓冲建——**它就是带真洞的那份几何**，自动满足约束，无额外工作。
3. 采纳 C 路（后台烘 StaticMesh）时，烘的必须是几何洞版本；用 `UCSMeshRenderComponent::SaveToStaticMesh`（`Public/CSMeshRenderComponent.h:85-87`）从同一份常驻流出资产即可，同样自动满足。
4. **不为 GI 单独造一份「简化成整块墙」的低模**——一栋房千级三角，低模化收益本来就不大。

**验收判据（改过：原方案的判据要求先走它自己否掉的 C 路，自相矛盾）**：不必烘资产也能验。在 `L_HouseGroundDemo` 放一个普通 `UStaticMeshComponent` 立方体、挂一张用 opacity mask 挖洞的 Masked 材质，开 `Show > Visualize > Mesh DistanceFields`——洞在可视化里是实心块即证明分界线存在；这条一分钟就能跑，且完全不动房屋代码。

**代价**：约束本身零代码，只是一条文档与头注释纪律。真正代价只在将来采纳 GI 路线时出现：代理必须与 raster 网格共享同一份带洞几何，限制了未来做 GI 低模优化的自由度。
**不推翻用户裁决——恰恰站在 `Plan_House.md:184`「布尔减 cutter 出真洞」这一侧。**

**证据**：【逆向报告确凿】§1.6、§3.2、§9.3、结语第 3 条。【逆向报告推测】§1.2「按 `iter_holes_with_padding` 裁砖」落在标注为【推测】的排砖顺序链内（`trim_rows` 工具链本身为【确凿】）。【我的推断】「RtWorld 吃的是已裁洞的砖布局」由 §1.2+§1.3 组合推出，报告未直接陈述。【引擎源码直证，本轮逐行核过】`Developer/MeshUtilities/Private/MeshRepresentationCommon.cpp:303-304`、`Renderer/Private/Lumen/LumenHardwareRayTracingMaterials.cpp:48-80`、`Renderer/Private/ShadowDepthRendering.cpp:371,488`。【项目代码直证】`Private/CSHouseActor.cpp:343,363-368,371-411`、`Plan_House.md:184` 及进度注记「切片相对 P2/P3 的两处有意偏差 ①」。

---

### 光照派生表示的失效挂点：只有 `HandleMeshChanged` 覆盖全部写入路径，而它的纯内容编辑分支今天连代理都不重建

*维度：光追GI代理_光照与阴影的一致性*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 把「几何变 → 光追世界跟着变」做成同一个系统的写出口：`construct_walls` 的系统签名同时写 `WallHoles / RtWorld / Query<&mut InstancedWall>`，并写出 `EventWriter<RecalculatePlasterCmd / RecalculateHalfTimberMeshCmd / UpdateRtWorldEvent>`（§1.3，签名即证据核心）；楼梯同理，`gen_stairs_collider_and_rt_geo` 一个函数同时产碰撞体与光追几何（§4.1）。§10.3【确凿】另列三条显式增量重建机制。

**当前方案怎么做**：计划重建链是「唤醒 → `ReevaluateSite()` → 哈希比对 → 变了才重建」（`Private/CSHouseActor.cpp:295-329`，哈希短路在 `:312-327`），产物经 `ACSTinyGlade::UploadTinyGladeSnapshot`（`Private/CSTinyGlade.cpp:21-33`）上传。链路里没有任何光照/代理相关的一环（计划全文 grep 0 命中）。

**差距**：**本条只保留光照维度独有的切面：光照派生表示（BLAS / CPU 已知计数直绘的 NumPrimitives / 后台烘焙代理）该挂在哪、以及挂错会怎样静默失效。**（房屋之间的变更传播、幂等哈希、CPU-GPU 边界纪律属第一轮维度⑤，不在此重复；计划 `:186`「重建产出不发布任何通知」讲的是 actor 之间不互相通知，与渲染组件内部的派生表示更新是两件事，不冲突。）
1. 看着最像挂点的 `UploadTinyGladeSnapshot`（`Private/CSTinyGlade.cpp:21-33`）**覆盖不全**：柱子绕开了它——`ACSHouseActor::RebuildPillarMesh`（`Private/CSHouseActor.cpp:465-493`）把同一套步骤内联了一遍（`:486` 注释自陈「基类上传管道只服务主网格……这里内联同一套步骤」）；地面的形状变化更是根本不走快照上传——`UCSMeshOps::DisplaceGroundShapers`（`Public/CSMeshOps.h:378`）是原地改常驻流 Z 与切线的 compute pass。挂在这里，柱子的阴影/BLAS 永不更新（而柱子恰是最需要投影的物件），画塑形物时地面的光照代理会一直停在旧高度。
2. **唯一覆盖全部写入路径的是 `UCSMesh::OnMeshChanged` → `UCSMeshRenderComponent::HandleMeshChanged`（`Private/CSMeshRenderComponent.cpp:233,242`）**：三条路径都走 `EditMeshSync` 并 bump `Generation`——`CopyFromMeshSnapshot`（`Private/CSMeshOps.cpp:1036`，房体与柱子共用）、`DisplaceGroundShapers`（`Private/CSMeshOps.cpp:1414` → `:1433` `EditMeshSync`）。**这也意味着「收掉柱子旁路」对本议题不是必要条件**（原方案把它列为步骤④，实为多余，见 rejected）。
3. **但 `HandleMeshChanged` 的纯内容编辑分支只做 `UpdateBounds()` + `MarkRenderTransformDirty()`（`:292-294`）就返回**——顶点位置变了、没重分配、材质没换的情况下**连场景代理都不重建**。这正是地面区域位移与「容量够用的房体重建」的常见路径。该分支的注释（`:250-252`）明说这是设计意图（「counts live on the GPU，所以只需要新 bounds」）——**这个前提在引入任何 CPU 侧派生光照表示的那一刻失效**。症状是「画完地形，阴影/GI 还停在老形状」，且没有任何日志。

**建议**：**把光照派生表示的失效做成 `FCSMeshResident::Generation` 的订阅者，挂在 `UCSMeshRenderComponent::HandleMeshChanged`，不要挂在 `UploadTinyGladeSnapshot`。**
1. 在 `UCSMeshRenderComponent` 加 `uint32 BoundContentGeneration`（与已有的 `BoundAllocationGeneration`（`Private/CSMeshRenderComponent.cpp:286`）并列），在 `HandleMeshChanged`（`:242`）里比对 `Resident->Generation`。
2. 变化时给场景代理发一条「几何内容已变」的渲染线程命令（`ENQUEUE_RENDER_COMMAND` + 代理指针），代理侧翻译成：有 BLAS 时标记 refit / rebuild；采用 CPU 已知计数直绘时刷新 `DrawDesc.NumPrimitives` 与 section span（即第一条 S2 的第 ⑤ 步）；采用后台烘 StaticMesh 时打一个代数戳、松手按戳判定是否重烘。
3. 幂等性已由上游保住：`ReevaluateSite` 的哈希短路（`Private/CSHouseActor.cpp:312-327`）把无效唤醒吸成零成本，`Generation` 只在真重建时才动，不会出现「每帧重建 BLAS」——这一点比 TG 还稳（TG 的 `construct_walls` 是「墙脏了就全量重排该墙的砖」，§1.3 该句为【待确认】）。

**代价**：约 40-60 行，全在 `CSMeshRenderComponent.{h,cpp}` 与代理侧，不动 `UCSMesh` 的编辑纪律、不引入跨 actor 通知、不引入 subsystem tick。
主要风险：`HandleMeshChanged` 是每次内容编辑都跑的热路径，地面笔刷/塑形物拖拽期间每帧都会走到，新增的渲染线程命令**必须是一个原子标记而不是重建**，否则直接打掉「拖拽 30 Hz」的目标（计划风险节「每帧重建的 flush 代价」已有相关条目）。
**不推翻用户裁决**：计划 `:186`「重建产出不发布任何通知」约束的是 actor 间通知，本条只在渲染组件内部做代理失效。

**证据**：【逆向报告确凿】§1.3（`construct_walls` 签名同时写 `RtWorld` 并发 `UpdateRtWorldEvent`）、§4.1、§10.3。【逆向报告待确认】§1.3「全量重排该墙砖」明确标注为【待确认】。【项目代码直证，本轮逐行核过】`Private/CSMeshRenderComponent.cpp:233,242,250-252,280,286,292-294`、`Private/CSTinyGlade.cpp:21-33`、`Private/CSHouseActor.cpp:295-329,312-327,465-493,486`、`Private/CSMeshOps.cpp:1036,1414,1433`、`Public/CSMeshOps.h:378`、`Public/CSMesh.h:118-119`、`Plan_House.md:186`。【我的推断】「纯内容编辑分支让派生光照表示静默过期」由 `:292-294` 的早返回推出，具体症状未实测。

---

### 实例化路径（石阶/点刷/D12 摆件植被）能投影且不受 args 覆盖之累，但剔除只跑主视锥——缺 TG 的三个阴影级联槽

*维度：光追GI代理_光照与阴影的一致性*

**Tiny Glade 怎么做**：【逆向报告确凿】§1.5：7 个 draw 槽里槽 4-6 是三个阴影级联，写入规则是「**不受主视锥/可见掩码过滤**，按砖在太阳视图（`view_constants[2]`）sample 空间的 AABB 与级联窗口 `[0,1]/[1,2]/[2,7]±r` 的重叠**无条件写入**，单阶段、无遮挡剔除」；pass2 的 reset 只清主视图 4 槽的 instanceCount，阴影槽保留。结语第 2 条【确凿】：nani 通用管线共享同一套 prev/next 可见性 bitmask + 3 级联分桶。

**当前方案怎么做**：`Private/CSGpuInstancedMeshComponent.cpp:64` `CastShadow = false; bUseAsOccluder = false;`，注释（`:62-63`）与 `CSMeshRenderComponent.cpp:82-83` 同一句。剔除在 `FCSGpuInstancedCullViewExtension::PreRenderView_RenderThread`（`Private/CSGpuInstancedMeshSceneProxy.cpp:178-192`）里**每个 view family 只跑一次**（`:189-190` 的 `bCulledThisFamily` 短路），且只针对当前主视图（`:415-431` 用 `View.GetCullingFrustum()`，`:434` 用 `View.ViewMatrices.GetViewOrigin()`），产出按 LOD 分的 DrawIndexedIndirect 参数（`Shaders/Private/CSGpuInstancedMesh.usf:233` `BuildArgsCS`；批次在 `Private/CSGpuInstancedMeshSceneProxy.cpp:670-690`）。计划 D9 石阶已落地、D12 摆件植被、D13 藤蔓点都要落在这条路上。

**差距**：两处偏差：一处是记错的封禁，一处是真缺口。
**(1) 封禁理由不成立（已核实）。** `FCSGpuInstancedMeshVertexFactory` 注册时**故意不带** `SupportsPrimitiveIdStream`（`Private/CSGpuInstancedMeshVertexFactory.cpp:102-112`，注释自陈是为了让 `VF_USE_PRIMITIVE_SCENE_DATA` 编译成 0），因此 `MeshDrawCommand.PrimitiveIdStreamIndex == INDEX_NONE`，`MeshPassProcessor.cpp:1304` 的 `bDoOverrideArgs` 恒 false——**它的间接参数在任何 pass 里都不会被 instance culling 覆盖，包括 VSM**。它今天没影子的真实原因是路由：`ShadowDepthRendering.cpp:2145` 按 `VertexFactory->SupportsGPUScene()` 二选一，非 GPU-Scene 的 VF 只被 `EShadowMeshSelection::SM` 掩码的 shadow info 收；而 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps` 默认 1（`ShadowSetup.cpp:344-346`）时开了 VSM 的灯**不再创建**常规阴影图（`:4313` 的创建门），SM 掩码的 shadow info 也就不存在。（`ShadowSetup.cpp:2106,2324` 显示这类动态相关性图元在**收集**阶段是按 `EShadowMeshSelection::All` 收的，所以卡点确实在 mesh processor 与 shadow info 的存在性，不在收集。）
**(2) 真缺口：即便放开，投出来的阴影也是错的。** 剔除只跑一次、只对主相机视锥，写出的 instanceCount 是「主视图可见集」；阴影深度 pass 复用同一套 args，等于「只有屏幕上看得见的石阶才投影」——镜头外的、被建筑挡住的实例统统没影子，摇镜头时阴影成片闪现/消失。这正是 TG 用槽 4-6 专门解决的问题。

**建议**：**步骤 1（可立即验证）**：删掉 `Private/CSGpuInstancedMeshComponent.cpp:64` 的 `CastShadow = false` 并订正 `:62-63` 那条不准确的注释，在 `L_TerrainOpsDemo` 里开 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps 0` + 一盏方向光，确认 27 级石阶开始在地面上留影。这一步既是修复也是对 (1) 的实测判据。

**步骤 2（照 TG 补阴影专用参数集，等 D12 上量再做；推荐直接用降级方案）**：
- 把 `FCSGpuInstancedGpuLayout` 的间接参数区从「NumLODs 套」扩成「NumLODs × (1 主视图 + N 阴影)」套，`Shaders/Private/CSGpuInstancedMesh.usf:233` 的 `BuildArgsCS` 多写 N 套；`GetDynamicMeshElements`（`Private/CSGpuInstancedMeshSceneProxy.cpp:670-690`）按当前 pass 选对应的 `IndirectArgsOffset`。
- **降级方案（推荐先用这个）**：给阴影套直接用「全实例、不剔除」的一套固定 args——实例数从源计数直接来（`FCSGpuInstanceSourceGPU::Counter`，`Public/CSGpuInstancedMeshComponent.h:72-81`），一个 compute 线程就能写完。石阶（几十级）与 D12 村庄尺度摆件下，阴影 pass 多画几百个实例远比阴影闪烁廉价，且没有正确性风险。
- **完整方案（按级联分桶）建议不做**：UE 侧拿不到 TG 那种统一的 `view_constants[2]` 太阳视图，级联窗口要从 `FProjectedShadowInfo` 里刨，而场景视图扩展看不到那些。

**验收**：摇镜头让部分石阶出屏，地面上的阶梯影子必须保持完整不闪。

**代价**：**步骤 1 的代价被原方案低估，须订正**：改动确实 < 10 行，但不是「零成本」——(a) `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0` 是**项目全局**开关，会给每盏有 VSM 的灯额外跑常规 CSM 深度 pass，且该 cvar 被 Epic 明写 deprecated（`ShadowSetup.cpp:349`），长期有版本风险；(b) 该 VF 注册带 `EVertexFactoryFlags::DoesNotSupportNullPixelShader` 且**不带** `SupportsPositionOnly`（`Private/CSGpuInstancedMeshVertexFactory.cpp:106-112`），所以 `ShadowDepthRendering.cpp:371` 的 position-only 快路径对它不成立、`UseDefaultMaterialForShadowDepth` 的 null-PS 路径也不成立——阴影深度会为它编译一整套 VS+PS permutation，首次运行会触发一轮 shader 编译；叠加项目 `r.Substrate=True`，permutation 基数不低。功能上应当能跑，但「不需要改一行渲染代码、几乎零成本」这句要改成「代码零改动、但要付一个全局 cvar 与一套新 permutation」。
步骤 2 降级方案：一个额外 args 写入 + 绘制时按 pass 选偏移，约 40-60 行 C++ + 十几行 HLSL；显存增加 = 每 LOD 多一套 5 个 uint，可忽略；运行成本 = 阴影 pass 画全部实例而非可见集（不需要额外的可见实例缓冲，因此不触碰 `Private/CSGpuInstancedMeshSceneProxy.cpp:248-254` 的 64 MiB 警告线）。
**不推翻任何用户裁决**——计划对实例化路径的阴影没有裁决。
**与上一条的关系**：若第一条采纳 S2（不需要任何 cvar），本条步骤 1 仍需 `ForceOnlyVirtualShadowMaps=0`——两条路径的阴影修复**并不共享同一笔开销**，这一点原方案说反了。

**证据**：【逆向报告确凿】§1.5、结语第 2 条。【引擎源码直证，本轮逐行核过】`Renderer/Private/MeshPassProcessor.cpp:1304`、`Renderer/Private/ShadowDepthRendering.cpp:371,2145`、`Renderer/Private/ShadowSetup.cpp:344-349,2106,2324,4313`。【项目代码直证】`Private/CSGpuInstancedMeshComponent.cpp:62-66`、`Private/CSGpuInstancedMeshVertexFactory.cpp:102-112`、`Private/CSGpuInstancedMeshSceneProxy.cpp:178-192,248-254,398,415-434,670-690`、`Shaders/Private/CSGpuInstancedMesh.usf:170,186,233`、`Public/CSGpuInstancedMeshComponent.h:72-81`、`Public/CSGpuMeshSceneProxy.h:199`、`Config/DefaultEngine.ini:52`。【我的推断】「删 CastShadow=false + 关 ForceOnlyVirtualShadowMaps 即可出影」逻辑闭环但未实测，步骤 1 本身就是它的判据。

---

## LOW

### 砖石写手 `FCSHouseMeshWriter` 是文件私有的，D7 角柱是第一个真正抄不到它的消费者（原条目的"已写四遍"不成立）

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §4.1【确凿】：楼梯底下的拱/托架/柱**不是楼梯自己写的**，走墙体构造器 `system_wall_constructor::construct_playermade_stairs::{arches_n_walls, brackets, pillars, magic_support::playermade_stair_floating_bricks}`。§1.2 第 58 行【确凿】：该 crate（`crates/systems/wall-constructor/`，53 个 .rs、约 214 条唯一符号 [PATH][SYM]）同时服务墙（§1.2）、平台楼梯（§4.2）与栅栏（§8.4）。上层只出布局意图，砌法统一由这一个 crate 决定。

**当前方案怎么做**：**原条目的三条论据有两条被核倒**：① grep `AddTri|AddQuad|AddBox|MeshWriter` 覆盖 `Source/ComputeShaderGenerator/{Private/*.cpp,Public/*.h}`，除 `CSHouseActor.cpp` 外**零命中**——今天只写了**一遍**，不是四遍。② `ACSSplineBlockActor` 根本不用写手：它走 `CSSplineBlock_ExtractPaletteEntry` 提取 StaticMesh 三角 + `CSSplineBlock_AppendEntryTransformed` 变换 append（Private/CSSplineBlockActor.cpp:274-285），是另一套机制；计划 :593 说的「与 D6 墩、D7 角柱、D9 承重柱并列为第四种**叠块语汇**」讲的是视觉语汇，不是写手代码（原条目引成 :589，且把"语汇"读成"抄写点"）。③ 计划 :595 的绕序矛盾是 `CSMeshOps.usf::UploadStaticMeshIndicesCS` 注释与 `CSMeshBuild.h` 常驻流口径的冲突，**属 StaticMesh 导入路径**，与手写 `AddTri` 无关——原条目拿它当"口径分叉第一次发作"的实证是错的。
成立的部分：`FCSHouseMeshWriter` 确在**匿名 namespace** 内（Private/CSHouseActor.cpp:13-117，struct 本体 23-85，边框注释 :15「Unity/jumbo 构建共享 TU，file-local 一律 CSHouse_ 前缀」），`CSHouse_GetEdge` 同样文件私有（:97-109）。D7 角柱按计划 :294 是「CPU 生成经基类上传管道进自己的 `UCSMesh`」、「instanced 路线暂不用」——**它必须要一个 CPU 写手，而现在拿不到**。

**差距**：缺口从"四份分叉"缩到"D7 动工时第二个消费者拿不到写手"：`ACSHouseSeamActor` 要按 `PanelCell` 模数逐层错缝堆块（计划 :294），几何原语与 `AddBox`/`AddQuad` 逐字相同，但它在另一个 .cpp 里，只能重抄一遍绕序（`cross(B-A,C-A)`，Public/CSMeshBuild.h）、UV 平铺周期（`CSHouse_UVScale = 200`，CSHouseActor.cpp:17）、法线的 `TransformVectorNoScale` 用法（:38-39）。抄错的症状是"某面不显示 / 法线朝内"，定位成本高。TG 的证据说明这不是可选项：楼梯支撑落地时会出现第三个消费者，且按 TG 架构本来就该复用同一套砌法。

**建议**：一个头文件，不新建模块、不动任何已裁决项：把 `FCSHouseMeshWriter` 原样提到 `Public/CSMeshWriter.h`（改名 `FCSSnapshotWriter` + `COMPUTESHADERGENERATOR_API`）——它已经通用，只依赖 `FCSGpuMeshCPUData` + `FTransform`，零房屋语义；`CSHouse_GetEdge`（:97-109）留在原处不动（那是房屋语义）。**"再抽一个 `CSMasonry.h` 放 `AddArchSpan`/`AddStackedColumn`"这一步现在不做**：`AddArchSpan` 只有一个消费者且要按上一条 finding 参数化，D7 角柱的堆块规则也还没写过一次——抽两个都没有的东西是投机。等 D7 角柱写完、楼梯托架成为第二个堆块消费者时再提取，那时形状是已知的。`ACSSplineBlockActor::SolveBlockLayout`（Public/CSSplineBlockActor.h:79-81）原地不动，它已经是共享的一维打包器且已单测（Private/Tests/CSSplineBlockTests.cpp，`CSGroundShaperActor.cpp:301` 是第二个消费者）。

**代价**：纯搬运，约 60 行位移，零行为变更；unity/jumbo 同名符号风险按本仓已定纪律加前缀（CSHouseActor.cpp:15 的注释就是这条纪律的出处）。时机 = D7/P5 动工前，抄之前抽最省。

**证据**：TG 侧【逆向报告确凿】（§4.1 楼梯支撑走 `system_wall_constructor`；§1.2 crate 规模与服务面）。项目侧【代码事实】，**原条目的"已写四遍"与"绕序矛盾为证"两条经 grep 与读原文判否**，故严重度由 medium 降 low、提取范围由"写手 + 砌法库"收缩到"只提写手"。

---

### TG 三套楼梯里项目只做了 §4.3 那一套，§4.1/§4.2 是零——但 §4.2 在当前房屋模型里没有落点，别当成"60 行就能拿下的最小切片"

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §4 标题即「楼梯：**三套并存**的系统」：§4.1 玩家绘制（CPU 图状态 `StairsState{add_node, add_edge, apply_diff, extract_diff_and_make_backup}` + `stairs_assemble` + 穿墙开洞 + parry 形变）【确凿】；§4.2 平台楼梯 `construct_platform_stairs` + `segmentation::split_wall_path_segment_into_platform_stairs`——**墙顶步道跨高差时自动分段成台阶** [SYM]【确凿】；§4.3 岩地台阶 100% GPU 摆放【确凿】。§10.3【确凿】撤销 = `systems/history/replay_*`（报告写的是含 `stairs` 的 glob，不是逐个符号）重放笔划再走同一构建管线。**报告没有说 §4.2 用不用 §4.1 的设施**——原条目的"§4.2 不带任何重设施"是【论据缺失的推断】，且与 §1.2 第 68 行相抵：墙路径的切段 `segmentation::calc_path_wall_segmentation` 打分依据正是「与其他墙/**楼梯**的交点」，说明平台楼梯是嵌在墙路径切段机制里的，不是独立小件。

**当前方案怎么做**：计划全文 grep 「楼梯」= **0**（792 行）。已落地的是 §4.3 那一套：`ACSGroundShaperActor` 的塑形物阶梯（计划 :451-457、:755），代码 Private/CSGroundShaperActor.cpp:181-344 —— **按 `StepHeight` 切层 + 每层求等高线环半径 + 道路过阈弧段 + `SolveBlockLayout` 沿弧长铺不同长度石阶**（:301 调用），每层的石阶 Z **恒为 `LevelZ`**（:322 的控制点第三分量）→ GPU 实例化（每 palette 条目一个 `UCSGpuInstancedMeshComponent`）。房屋侧：墙是四条边、`WallHeight` 恒定（Private/CSHouseActor.cpp:349、355-367），房底落座在单一 Z（`ComputeSeatZ`，:182-183 取 footprint 全域地面最大值）。`ACSSplineBlockActor` 按自身注释（Public/CSSplineBlockActor.h:12-18）与计划 :593 是**纯装饰**的城齿/檐口块排。

**差距**：缺口成立，但形状与原条目描述的不同：
① **§4.2 在当前模型里没有落点**：它的前提是"墙顶步道"+"沿墙有高差"。项目的房屋墙是刚性矩形、墙高常数、整栋座在单一 Z 上（CSHouseActor.cpp:349、182-183），**墙顶本身就是水平的**，没有可分段的高差；`ACSSplineBlockActor` 是装饰块排不是步道。要有 §4.2，先得有"墙路径"这个概念（TG 的 `wall_path_segment`，报告 §10.2 第 426 行），而这是比楼梯本身更大的模型变更。
② **§4.1 的真门槛是状态形态，不是几何**：`StairsState` 是持久、可撤销、节点共享的图，撤销靠 `replay_*` 重放笔划；项目当前是"每 actor 一份参数、派生物全 transient、零撤销"——`ACSHouseActor` 的 `PillarMesh`（Public/CSHouseActor.h:229-230）与 `CurrentDoors`（:233-234）都是 `UPROPERTY(Transient)`，全 `Source/` 树的 `FScopedTransaction` 只有 2 处、且都在与房屋无关的 `EditorShortcuts` 模块（Source/EditorShortcuts/Private/EditorShortcutSubsystem.cpp:171-172，原条目记成"3 处"）。把可撤销图塞进这套架构是另一个量级的工程，不该和"想要台阶"混谈。

**建议**：**把这条当作排期与措辞的纠正，不提新代码**：
1. 在计划里补一句楼梯的**范围声明**——今天做的石阶 = TG §4.3；§4.1/§4.2 明确不在 D1..D13 内。现在计划全文"楼梯"零出现，读者会误以为 D9 石阶就覆盖了楼梯。
2. 若将来真要做，顺序是：**§4.1 的"沿自由路径的踏步 + 底下砖石支撑"**（不穿墙、不进图状态）→ 穿墙开洞（前置 = 本轮 finding 1 的字段 + finding 2 的注入通路 + finding 4 的扫掠查询全部就位）→ 图状态与邻接对齐（`find_best_neighbor_alignments` / `maintain_stair_graph`，最贵、最后做，且需要先解决零撤销）。**§4.2 排在"墙路径模型"之后**，不作为入口。
3. 若要试第一步，起点是**已跑通的 `CSGroundShaperActor` 分层铺装链**（:286-341：切层 → 每层一条曲线 → `SolveBlockLayout` 沿弧长铺 → 每层恒定 `LevelZ`），把"等高线环"换成"用户样条"即可——那已经是竖向量化 + 弧长打包的完整实现，比从 `ACSSplineBlockActor` 改起近得多。

**代价**：本条本身零代码代价（文档一句范围声明）。附带纠正一个会误导排期的成本估计："给 SplineBlock 加 60 行"这个数字不成立，理由见 rejected。

**证据**：TG 侧【逆向报告确凿】（§4 三套并存、§4.1 `StairsState` 与 §4.2 符号、§10.3 history 重放）；"§4.2 不需要 §4.1 设施"为【被核倒的推断】。项目侧全部【代码事实】（`FScopedTransaction` 2 处已 grep 复核）。**与第一轮维度④（地面/笔刷/地形/台阶/小径）的边界**：④覆盖的是 §4.3 那一套地形台阶（已落地的 `CSGroundShaperActor` 石阶）；本条只讲 §4.1/§4.2 这两套**不属于地形**的楼梯，以及它们与地形石阶的排期关系。

---

### 提醒：派生物全部由输入推导且无抑制口——楼梯支撑落地时，TG 的"删掉支撑、留下悬浮砖"这类交互没有落点

*维度：楼梯系统_当前方案的完整空白（对抗性核验后）*

**Tiny Glade 怎么做**：报告 §4.1【确凿】：楼梯下的砖石支撑由 `construct_playermade_stairs::{arches_n_walls, brackets, pillars}` 自动生成，且**专门有一个分支** `magic_support::playermade_stair_floating_bricks`，报告的中文注解是「删支撑后出现『魔法悬浮砖』」。§10.3【确凿】撤销 = 重放笔划再走同一构建管线。**必须注明**："支撑是用户可删的对象、TG 主动放弃物理正确性换交互自由"是【我的推断】——报告只给了符号名与那半句注解，没说是谁删、怎么删、删的状态存在哪。

**当前方案怎么做**：计划的核心承诺是"目标状态 = 当前输入的纯函数"，全篇贯彻：接缝 actor `RF_Transient` 不序列化、加载时重导出（:293）；柱网格 `UPROPERTY(Transient)`（Public/CSHouseActor.h:229-230）；门每次从道路顶点色重算（Private/CSHouseActor.cpp:186-244）；塑形物产物「删塑形物 = 地形塌回 + 产物全消失」（:447，标为用户裁决）。承重柱的生成条件是纯阈值 `Gap > PillarMinGap`（Public/CSHouseActor.h:161-162，实现 Private/CSHouseActor.cpp:284-285），**无任何 per-instance 关闭口**。

**差距**：这套写法的好处（幂等、无历史、可测）计划已讲透，我不推翻。未被讨论的后果是：**用户对派生物没有否决权**。楼梯支撑一旦落地，"删掉这一根托架"下一次 `ReevaluateSite()`（CSHouseActor.cpp:295）就把它生回来，因为"我删过它"不在任何权威状态里。同类的还有"想要一栋不长柱子的悬空房""想让某一处不开门"（今天只能反过来去擦地面的路）。

**建议**：**这是提醒，不是建议改**。若将来确实需要，形状可以做到不碰纯函数性质：加 `UPROPERTY() TSet<uint32> SuppressedDerived;`（序列化，key 复用各派生物已有的稳定 id——门用 `ComputeDoors` 已在用的 `(Edge<<24)|(N<<16)|Slot`，CSHouseActor.cpp:227），在 `ComputeDoors`/`ComputePillars` 末尾各加一句 `Contains → continue`，其余一字不动——**抑制集本身就成了输入的一部分，纯函数性质不受影响**。必须并进两份 desc 哈希（CSHouseActor.cpp:247-252 与 :280-291），否则抑制后不重建。key 必须像滞回 key 一样带上重排身份（`N` 进 key），否则墙长一变抑制会误落到别的槽——CSHouseActor.h:236 的注释已经为滞回记过一次这个教训。
**一处需修正的类比**：原条目说 `DoorSlotOpen` 是"带记忆的旁路先例、只是 transient"——实际它连 `UPROPERTY` 都不是（Public/CSHouseActor.h:237 是裸成员），所以它证明的是"跨重求值的非序列化记忆可以存在"，**不**证明"序列化旁路已被架构接受"。加一个序列化的 `TSet` 是新引入的先例，得当成新决策对待，不能说"照抄已有形态"。

**代价**：约 30 行 + 两处哈希改动 + 一个 `CallInEditor` 入口（不做自定义 HitProxy——计划已裁决不做）。真正的建议只有一句：别把"用户否决派生物"推迟到楼梯支撑落地时再想，那时它会以"托架删不掉"的形式突然变成阻塞项。

**证据**：TG 侧：`magic_support::playermade_stair_floating_bricks` 符号与 §10.3 重放机制为【逆向报告确凿】；"支撑可被用户删除、TG 以此换交互自由"为【我的推断】（报告未给删除主体与状态存放，原条目把它当成 TG 的明确主张，已改正）。项目侧全部【代码事实】。**定位是提醒**："派生物纯函数、不序列化"是反复裁决过的架构主张，本条是加法且默认不落地。

---

### 【降级为提醒】纯外观量不得进 desc 哈希——这条纪律要现在写进 ComputeDoors 注释；至于主题/换季，计划没有、用户没提，MPC 只是届时的形状建议

*维度：选中高亮_编辑反馈_换色换主题的零重建原则*

**Tiny Glade 怎么做**：§9.1【确凿】`autumn_color` / `flowery_color` 是 mesh JSON 的自定义顶点属性，属报告统计的「41 种自定义语义属性 / 全库 47 种组合」之列，标注用途为「主题换色」。**修正原稿**：报告只说它们是换色用的顶点属性，没说「换季只是 VS 选哪一套颜色」——那一步是【我的推断】。§1.6【效果确凿，主题 id 对应关系报告自标推测】冬季铺雪 = `glade_theme_id==4` + simplex 噪声的 shader 分支，砖 `flags&16` 檐下减雪。§3.3【确凿】屋顶雪是 `gen_roof_snow_mesh` **CPU 生成整片壳 mesh（非实例化）**，VS 复刻瓦片同款抖动噪声以保证贴合。
**修正原稿的一处硬伤**：原稿称屋顶雪是「全报告唯一一处 CPU 生成整片壳 mesh」——**报告并没有这个唯一性主张**，且 §1.3 的 plaster 网格、§4.1 的 `gen_stair_floors→StairsFloorMeshChunk`、§8.3 的 `generate_curved_halftimber_meshes` 都是 CPU 生成的网格。删掉「唯一」。

**当前方案怎么做**：`Plan_House.md` grep「主题/季节/四季/theme/season」= 0 命中（实测）；`Source/` 下 `MaterialParameterCollection` = 0 命中；gpumesh 路径零 MID。`Content/HouseTest/` 里只有 3 个蓝图 + 2 张关卡 + 3 块石阶网格，**一份材质资产都没有**（`GroundMaterial`/`WallMaterial`/`RoofMaterial`/`PillarMaterial` 四个槽全空，兜底是 `UCSGpuMeshComponent::GetDefaultSurfaceMaterial()` = WorldGridMaterial）。计划里也没有任何一条纪律拦住「有人顺手把 SnowAmount 塞进房体 desc 哈希」。

**差距**：**真正的缺口只有一条**：房体 desc 哈希（`ComputeDoors()`，`Private/CSHouseActor.cpp:245-253`）现在没有任何注释说明「什么能进、什么不能进」。今天它只吃几何量是对的，但这是巧合不是纪律——一旦有人把颜色/雪量/主题 id 塞进去，拖一下滑块就重跑整条房体生成（未来还有布尔）链，正是 TG §8.3 那条「改色不重建 mesh」要防的事。至于「主题换装」本身：计划没有、用户没提、连第一份材质都还没做——**提议一整套主题系统属于超出计划范围，本条不主张现在做**。

**建议**：**现在只做一件事（一行注释）**：在 `ComputeDoors()` 的哈希段(`CSHouseActor.cpp:244` 那条现有注释旁)与计划 D4 补一条纪律——「desc 哈希只接受**改变三角位置或数量**的量；颜色、材质指针、主题/雪量等纯外观量一律不得进入，它们走材质参数或颜色流」。同一条也适用于 `ComputePillars` 的柱哈希(:317)。
**将来真要做主题时的形状（备案，不是现在的工作项）**：
- 全局主题 → 一份 `UMaterialParameterCollection`（`ThemeId`/`SnowAmount`/各 Tint）。MPC 是材质域的全局 uniform buffer，由 `FMaterialShader::GetShaderBindings` 绑定，**与顶点工厂、GPU Scene 都无关**，因此在这个自定义代理上照样生效（`FDynamicPrimitiveUniformBuffer` 不进 GPU Scene 不影响它）。这是 TG `frame_constants.glade_theme_id` 的对位。**UE 侧限制必须先说清**：MPC 的运行时覆写值**不序列化**——编辑器里改的主题重开关卡就没了，要么改 MPC 资产默认值，要么由一个 actor/subsystem 在加载时重新施加。
- 逐房差异 → 走上一条 finding 的 Color.A 色号 + 材质里查 1D 调色板贴图；**不要**给每个 actor 造 MID 再塞 `UCSMesh::SetMaterial`（每次 `RecreateRenderState_Concurrent`，`CSMeshRenderComponent.cpp:288`）。
- 加法几何（屋顶雪壳、装饰）→ 只有这一类才允许进 desc 哈希。

**代价**：现在做的部分：2 处注释 + 计划里一段话，0 行代码。备案部分不计入本轮。前提提醒：四个材质槽全空、关卡里跑 WorldGridMaterial，任何外观议题在项目侧都是零起点——这也意味着第一份墙材质的 blend mode 选择（opaque / masked）会同时决定上一条 finding 的原生描边能不能白拿，两件事要一起定。

**证据**：TG 侧【逆向报告确凿】(§9.1/§3.3) + 【报告自标推测】(主题 id ↔ 冬季)，并已剔除原稿两处越界表述（「换季只是 VS 选颜色」「全报告唯一的 CPU 壳 mesh」）。项目侧 grep 与文件:行号直证。「MPC 在这个自定义代理上生效」属【我的推断】（MPC 不依赖 VF/GPU Scene），落地一测即知；「MPC 覆写不序列化」是 UE 既有行为。原稿 severity=medium 已下调为 low：计划与用户均未提出主题需求，可执行的只剩那条纪律注释。

---

### 【降级为提醒】D4 的拖拽降级路径会把反馈整个删掉（不是变差，是消失）；补一个 MID 参数染色的替代实现，并注意 RebuildBodyMesh 每次都会覆盖 MID 绑定

*维度：选中高亮_编辑反馈_换色换主题的零重建原则*

**Tiny Glade 怎么做**：§1.6【确凿】橡皮擦逐砖亮橙 = VS/PS 里对 `brush_position` 做距离测试；`highlight_mode` 混橙/灰同理。§1.5【确凿】高亮批只是换个 draw 槽。
【我的推断】由此得出「TG 的编辑期反馈成本恒等于几个 push constant + 一次分支」——报告没有这句话，是从两处机制推的。

**当前方案怎么做**：D4 `:183`：「拖拽期间每帧重建 = 基体快照上传 + 布尔重切链……若布尔链撑不住 30 Hz，降级策略：拖拽中只重建基体不切洞，松手补切」；风险节同款表述「拖拽中可降级为免切/松手补切」。
**修正原稿的三处错误**：
1. 原稿把 D4:183 的**房子拖拽**降级与 D8 `:431-437` 的**标记拖拽**实时反馈混为一谈。D8 是用户裁决的实时反馈，且计划已经给了增量实现（`CachedOthersMesh` 拷贝 + 只减自己这一刀 + `csh.LiveCutHz` 节流），并自算了代价「每帧 = 一次 CPU 区间谓词 + 至多一次单刀布尔；整个交互的回读次数 = 0」——**D8 没有「反馈消失」的降级路径**。本条只对 D4:183 成立。
2. 原稿引用的 `FCSWallOpening` **不存在**（全仓 grep 0）。真实结构是 `FCSHouseDoor{EdgeIndex, CenterS, Width, Height}`(`Public/CSHouseActor.h:17`)。
3. 原稿要推 `ArchRadius` 是冗余的：代码里 `R = D.Width * 0.5f`、`SpringZ = D.Height - D.Width * 0.5f`(`Private/CSHouseActor.cpp:374-375`)，拱半径由宽度导出，3 个 float 就够。
另外计划风险节自己已判定「房屋几千三角问题不大，仍设 30 Hz 节流 + 哈希短路」——**每帧一次 `CopyFromMeshSnapshot` 同步 flush 的代价，计划已经知道并接受了**，不是被忽略的缺口。

**差距**：剩下的独有切面只有一条：用户裁决的是「要实时反馈」这个**目标**，而 D4:183 的降级路径一旦触发，结果是「拖拽中完全看不到洞」——**降级 = 反馈归零**，比 TG 差一个量级。计划为这条降级准备的是「松手补切」，没有准备「降级时用什么代替反馈」。

**建议**：给 D4:183 的降级路径补一个**染色提示**（不是重切、也不是 discard）作为兜底，使降级后仍有反馈：
- 载体：在 `ACSHouseActor` 首次绑墙材质时建**一个** MID（`UMaterialInstanceDynamic::Create` 一次），此后拖拽期只 `SetVectorParameterValue`。MID 参数写入**不触发 proxy 重建**（对比：换 `UCSMesh::Materials[slot]` 每次都 `RecreateRenderState_Concurrent`，`CSMeshRenderComponent.cpp:288`）。
- 数据：候选洞参数本来就在 CPU 手里（`FCSHouseDoor` + `FCSHouseEdgeFrame`，`CSHouseActor.cpp:17`/`:88-107`），打成 2 个 `float4`（墙外法线+d；CenterS/Width/Height/边索引）推给材质。
- 材质：判断像素是否落在候选洞剖面内 → **染色**（半透明提示色叠加）。拱剖面 = 矩形下身 + 半圆，与 CPU 侧 `RebuildBodyMesh`(:374-380) 是同一套闭式解，可逐字对照。
- 成本核对：降级帧从「一次快照重建 + 同步 flush」降到「一次 `SetVectorParameterValue`」= 0 个 RDG pass、0 次 flush、0 次回读，符合计划 `:766-768`「交互热路径一次回读都不许有」。
- 同一套参数也能直接服务 D5 拉尺寸 handle 的「这面墙正在被拉」。

**代价**：C++ 侧约 20 行 + 墙材质里一段剖面解析判断。不新增流、不新增算子、不进任何哈希。**三条必须写进注释的坑**：① `UploadTinyGladeSnapshot`(`Private/CSTinyGlade.cpp:27`) 每次 `RebuildBodyMesh` 都重写 `Materials[0] = WallMaterial`，会**把 MID 覆盖掉**——必须在上传后重新塞回 MID，或把 MID 的建立收进 `BindTinyGladeMaterials`（见第 1 条 finding）；② 只做染色不做 `discard` —— 在 UE 里 `discard` 要求材质 blend mode 改成 Masked，那会连带改变整面墙的 shader 排列、早 Z 行为与 pass 参与情况，为一个降级预览付这个代价不划算（且逐像素挖洞的表示之争属第一轮维度①的题目，本条不碰）；③ 预览只在外表面成立、无侧壁，松手补切后有一次形状跳变，属已知差异。

**证据**：TG 侧【逆向报告确凿】(§1.6/§1.5)，「成本恒等于 push constant + 分支」属【我的推断】。项目侧计划 D4:183 / D8:431-437 / 风险节与代码文件:行号直证；原稿的 `FCSWallOpening`、`ArchRadius`、D4/D8 混淆三处已核实为错并修正。**不推翻任何用户裁决**：D8 的「实时反馈」裁决完整保留，本条只补 D4 降级路径的空缺，severity 由 medium 下调为 low。

---

### 降级为提醒：屋檐/屋脊伸出 footprint，但这不说明接缝判据错了——错的是「屋顶之间」在本计划里根本没有负责人

*维度：碰撞与空间查询基建*

**Tiny Glade 怎么做**：§8.5【逆向报告确凿】空间索引写的是 WallSpatialHash / **wall_roof_hashmap**，屋顶与墙一起进索引。§8.2 / §3.3【确凿】construct_inter_roof_merlons / inter_roof_merlons —— 「两个屋顶之间」在 TG 里是一等的几何关系，且有**专门的产物**（屋顶间雉堞），不是靠墙缝合顺带解决的。§8.2 / §8.5【确凿】墙缝合 inter_shape_stitches::stitch_bricks 依赖通用 mesh-mesh 相交查询 find_mesh_mesh_intersections。

**当前方案怎么做**：【计划事实】D7/D10 用户裁决：触发 = 两房 2D footprint OBB 真正重叠 + Z 区间 [Bottom, Eave] 相交（:291、:503-508），交点 = 逐边对求交 4×4=16 次线段求交（:507），Top = min(两侧檐口)、Bottom = max(两房底 Z, 柱下地面高度)（:286、:507）。角柱的语义原文写得很清楚：「在每个轮廓交点立一根叠砖角柱**遮住墙体穿插处**」。
【代码事实】可见几何确实伸出 footprint：Private/CSHouseActor.cpp:424 EaveOut = LB*0.5 + RoofOverhang、:426 LAtot = LA + 2*RoofOverhang，RoofOverhang 默认 25 cm（Public/CSHouseActor.h:73）；屋脊 RidgeH（:423）高于檐口 EaveZ（:425）。

**差距**：【核验后的正确表述】两栋房 footprint 相距 40 cm（<2×25）时屋檐已互穿 10 cm，肉眼可见——这条几何观察成立。但原稿据此断言「用户裁决没有被正确实现」是错的：角柱的用途是遮**墙体穿插**，而屋檐相交时两面墙并没有接触，此处立一根从地面砌到檐口的叠砖柱，是在两栋没碰上的房子之间凭空竖一根柱子，恰好违反同一句裁决的下半句「靠得再近只要没碰上就不生」。footprint 判据对**角柱**而言是对的。
真正的空白在别处，且 TG 用的是另一套东西：**屋顶与屋顶的相交在本计划里没有任何机制负责**（TG 有 inter_roof_merlons 专门做这件事）。这是一条独立议题，不是接缝触发条件的缺陷。
【与第一轮③的重叠提醒】若第一轮「屋顶瓦片与装饰件」维度已覆盖屋顶交接，以其结论为准，本条只保留「接缝判据无需改动」这个否定性结论。

**建议**：不改任何触发盒，两件小事：
1.【P5 落地时，一行测试】在 D7 的单测里补一条**否定用例**：「两房 footprint 间距 20 cm、屋檐互穿」→ 断言**不生角柱**。把当前的正确行为钉死，防止将来有人（比如照原稿）把 broadphase 外扩成「可见体积盒」而无人察觉。
2.【记为独立议题，不排期】屋顶-屋顶穿插归入「屋顶」维度而非接缝维度；TG 的对应产物是 inter_roof_merlons，需要时是一个新的产物类型（如屋脊交接件），不是给接缝换判据。

**代价**：第 1 条一条单测。第 2 条零成本（只记录）。

【已 REJECT 的原稿两条提案见 rejected 列表】把 broadphase 换成外扩 RoofOverhang 的可见体积盒（与用户裁决及角柱语义正面冲突）、把 4×4 线段求交换成 TMeshAABBTree3::FindAllIntersections（对两个矩形而言解析版精确、微秒级、可纯函数单测，且计划 :743 的 P5 验收门明确要求 SAT/交点表单测绿；换成依赖代理三角网的树是纯粹的复杂度增加）。

**证据**：TG：§8.5 的 wall_roof_hashmap、§8.2 的 stitch_bricks + find_mesh_mesh_intersections、§3.3/§8.2 的 inter_roof_merlons 均为【逆向报告确凿】，原文核对无误。项目【代码事实】：Public/CSHouseActor.h:73、Private/CSHouseActor.cpp:422-426；计划 Plan_House.md:279-291（用户裁决原文）/286/503-508/743（原稿行号系统性偏 1-2 行，语义无误）。「40 cm 间距时屋檐互穿 10 cm」为按默认值直算的【我的推断】；「角柱语义 = 遮墙体穿插，故外扩盒违反裁决」为【我的推断，依据计划 D7 原文】。

---

### [WEAKENED，核心索赔被现有守卫驳回] Mirror.Heights 的冗余序列化是一次有意的取舍；真正剩下的缺陷只有 EnsureMirrorInitialized 里那趟被自己注释否定的全表扫描

*维度：规模上限与性能预算的量化对照*

**Tiny Glade 怎么做**：【逆向报告确凿】TG 地形完全不存栅格：地形是参数化笔画集合（`EditTerrainStrokeCmd` / `SetTerrainStrokePtsCmd`，历史 `TerrainReplayCmd`，§7.1），每次编辑由 `_terrain_editing_rasterize_terrain_stroke.cs` 逐 texel 重放全部笔画重新生成 heightmap。§10.3【确凿】把它讲成通用机制：「history 延迟录制 + 回放……撤销/重做即『重放笔划再走同一构建管线』」。存档体量 = 笔画数 × ≤1024 点。

**当前方案怎么做**：`FCSGroundMirror::Heights` 是 `UPROPERTY() TArray<float>`（CSGroundActor.h:33），随关卡序列化。全插件对它的写入只有两处：`EnsureMirrorInitialized` 的 `Init(0.0f,…)` 重置（CSGroundActor.cpp:92）与 `RefreshHeightsInRegion` 的 `Slot = Height`（:485-487），公式是纯绝对式 `max(0, 全部塑形物 SampleShapeHeight)`（:479-484，注释 :471-472 自述「在区域内重算与全量重算逐位相同」）。计划 :755 确认这是第一条写入路径。所以 Heights 确实是塑形物集合的纯函数。

**差距**：**原稿的三条索赔，两条被代码里的既有守卫驳回：**

**(a) 「冗余序列化」→ 降级为有意取舍。** `RefreshHeightsInRegion` 有幂等短路 `if (!bChanged) return;`（CSGroundActor.cpp:491，注释：「加载后重导出结果与序列化值一致时不重建、不标脏」）。**序列化的 Heights 正是这条短路的比较基准**：冷加载时重算值 == 序列化值 ⇒ 不跑 GPU pass、不 `MarkPackageDirty`、不广播。改成 transient 后 Heights 反序列化为空 ⇒ `bChanged` 必为 true ⇒ **每次加载都会 `MarkPackageDirty()`（:504）把关卡标脏 + 触发一次 `DisplaceGroundShapers`（含一次 flush）**。省 264 KB(@256²)/4.2 MB(@1024²) 的代价是每次开图脏包。净收益为负（1024² 时可再议）。

**(b) 「加载期最坏 O(V·K²/2)」→ 驳回。** `RegisterShaper` 有去重早退 `if (Shapers.Contains(Shaper)) return;`（:428，注释「已登记：区域更新由塑形物自己发，别在这里再跑一次全域」）。而 `ACSGroundActor::PostRegisterAllComponents`（:405-411）先 `ResolveShapers()`，它用 `TActorIterator` 枚举世界里**已存在但尚未注册组件**的塑形物（:417-421），关卡加载时全部 K 个都会被收进来。随后各塑形物的 `PostRegisterAllComponents`→`RebuildTerrain()`（CSGroundShaperActor.cpp:551/:383）走 `ResolveGroundAndRegister`→`RegisterShaper` **全部早退**，不触发 `RebuildHeightsFromShapers()`。二次方路径只在「地面先注册、塑形物之后才被 spawn」（运行时生成 / 流送 / 编辑器逐个拖入）时才可达，且那正是逐个拖入本来就该重算的语义。

**(c) 「EnsureMirrorInitialized 的全表扫描」→ CONFIRMED，是本条唯一站得住的部分。** `RefreshHeightsInRegion` 第一行调 `EnsureMirrorInitialized`（:459），后者在尺寸匹配分支里**无条件全表扫 Heights 求 MaxAbsHeight**（:83-85）。而同一函数在 :493-503 又从塑形物参数重算了一遍 `MaxAbsHeight`，注释还专门写「直接从参数取，不用扫全表（区域更新的意义就在于不碰区域外的格点）」。:83-85 那趟纯属浪费：1024² 地面上每次塑形物拖动帧要遍历 4.2 MB；加载期 K 个塑形物各自的区域刷新叠加成 K×V（K=20、V=1,050,625 ⇒ 2,100 万次浮点比较）。

**建议**：只做一件事：

**把 :493-503 那段抽成私有 `RecomputeMaxAbsHeightFromShapers()`，在 `EnsureMirrorInitialized` 的匹配分支（:79-86）与 `RefreshHeightsInRegion` 两处都调它，删掉 :83-85 的全表循环。** O(V) → O(K)。约十几行，无语义变化（两者结果恒等：Heights 恒 = max(0, 各塑形物台高)）。

**Heights 转 transient 只作为「1024² 配置下的可选项」记进计划开放问题 :776**，并写明门槛：一旦计划 :790「任意 mesh 形状塑形物走 GPU 光栅化 + 区域回读进镜像」落地，Heights 就不再是塑形物的纯函数，transient 化立即失效；届时正确形态是把回读结果存进**每个塑形物自己的足迹缓存**（尺寸 = 该塑形物包围盒 / 格距，比整张地面小两个数量级），维持「地面 Heights = 塑形物纯函数」这条不变量。这条不变量值得写进 `FCSGroundMirror` 的头注释。

**代价**：极小（十几行，无行为变化）。验证：塑形物关卡 `L_TerrainOpsDemo` 冷加载后台顶高度仍为 300（计划 :755 已有现成断言措辞）、拖动塑形物时高度场表现不变。

**证据**：【代码事实】CSGroundActor.h:33/:36；cpp:83-85（冗余全表扫描）、:92 与 :485-487（仅有的两处 Heights 写入）、:471-472（绝对式注释）、**:428（RegisterShaper 去重早退——驳回二次方索赔）**、**:491（幂等短路——驳回冗余序列化索赔）**、:493-503（MaxAbsHeight 的正确来源 + 自我否定的注释）、:405-411/:417-421（加载路径与 TActorIterator）、:504（MarkPackageDirty）；CSGroundShaperActor.cpp:383/:393/:551（塑形物自发的区域刷新）。【计划】:755、:776、:790。【逆向报告确凿】§7.1、§10.3。

---

### 明确否掉反方向：不做 NaniTrimeshChunk 式"primitive 内部 chunk 剔除表"——UE 的剔除与光照全部以 primitive 为键

*维度：nani架构_GPU驱动实例渲染层*

**Tiny Glade 怎么做**：§2.3【确凿】：`_nani_stairs_floor` 与 `_nani_plaster` 用 GPU 指针网格——`NaniTrimeshChunk{uvec2 gpu_address; vertex_index_count; vertex_byte_offset; vec4 bounding_sphere}` 经 `GL_EXT_buffer_reference` 按字节地址读 **u8 局部索引**，每 chunk ≤256 顶点，`gl_VertexIndex ≥ 索引数` 时输出 NaN 退化；CPU 侧 `StairsFloorMeshChunk` / `TrimeshBuilder` 切块上传。即：**每块唯一的程序化面片被伪装成"实例"塞进同一条实例管线**，每块自带包围球所以享受同一套剔除。§2.1 + §9.2【确凿】：5 个运行时程序化 subset（CurvedHalfTimber / Plaster / Smoke / StairsFloor / StoneFloor）与 19 个 ron subset 走同一条剔除/绘制管线。

**当前方案怎么做**：项目是两条独立路线，计划从未评估过统一：(a) `UCSMesh` + `UCSMeshRenderComponent`——运行时生成的唯一几何，一个 primitive 一整块（房子 D4、地面 D1、接缝 D7、spline 块 D11 均如此；地面默认 256²=131,072 三角、上限 1024²，`CSGroundActor.h:77-85`）；(b) `UCSGpuInstancedMeshComponent`——实例。两者之间只有一座半截桥 `SetBaseMeshFromGpuData`（`CSGpuInstancedMeshComponent.h:199`）。计划 `:776` 开放问题直接相关："地面规模上限与分块：单 actor 单网格先行；超大地面是否拆多 tile actor（各自镜像 + 无缝拼接）**待定**"。

**差距**：技术上项目已经具备零件：`UCSMeshOps::BuildMaterialSections` 已经是"一次 GPU 计数排序把同槽三角排连续 + 每槽一套 DrawIndexedIndirect 参数"（`CSMeshOps.h:399-402`），argset 上限 65536（`CSMesh.cpp:27-30`），把 chunk 当"材质槽"再加一个 pass 把视锥外 chunk 的 `IndexCountPerInstance` 写 0，是可行的。
**但不值得**：nani 用指针网格块是在绕开自研 Vulkan 渲染器"一次 draw 只能绑一套 VB/IB"的限制；UE 里同一件事的原生答案是 primitive 或 Nanite cluster，而 UE 的 bounds、视锥剔除、硬件遮挡查询、VSM 页剔除、Lumen card、距离场**全部以 primitive 为键**。把唯一几何藏进一个 primitive 内部的 chunk 表，等于对引擎的每一个剔除与光照系统再隐身一层——那正是本插件已踩到的坑（`CSGpuMeshSceneProxy.cpp:26` `bSupportsDistanceFieldRepresentation=false`、`CSGpuInstancedMeshComponent.cpp:65` `CastShadow=false`）的加强版。

**建议**：**A. 写死不做**，把结论写进 `CSMeshRenderComponent.h` 的类注释与计划开放问题，措辞："唯一几何按 **primitive** 切分，不做 primitive 内部的 chunk 剔除表。"零成本。
**B. 顺手给 `Plan_House.md:776` 的开放问题一个渲染侧答案**（该条本就写着"待定"，不推翻任何裁决）：超大地面拆多 tile **组件/actor**，每片自己的 `UCSMesh` + `UCSMeshRenderComponent` + 自己的 bounds。理由**不是**三角数（256² 的 131k 三角每帧提交约 0.1 ms 量级，不值得动），而是：只有拆成多 primitive，地面才能拿到逐片视锥剔除、逐片硬件遮挡查询、逐片 VSM 页剔除与逐片 Lumen card——单 primitive 时 128 m（上限 512 m）的包围盒对引擎每一个剔除系统都等于"永远可见"。
触发阈值给一个可判定的数：`NumCellsX*NumCellsY > 512*512`（三角数过 50 万）或地面边长超 200 m 时再拆，此前单 primitive 足够。
（提醒：此条与第一轮维度④「地面/笔刷/地形/台阶/小径」有交集，这里只保留"primitive 粒度 = 剔除/光照粒度"这个渲染层切面，地面本身的机制不重复讨论。）

**代价**：A 是零成本决定，收益是省掉一条看着很像 Tiny Glade 但在 UE 里是负收益的仿写路线。B 只在触发阈值到了才动工，届时成本主要在接缝：地面镜像是单一权威数组（`CSGroundActor.h:34` Heights/Colors），拆 primitive 时镜像**不要跟着拆**——只拆渲染宿主，`BuildSnapshotFromMirror` 按片取子矩形并让相邻片共享边界行顶点，法线在边界处从镜像重算而不是从片内差分，否则接缝会有一条可见的着色缝。动工前先写单测（"两片边界顶点位置与法线逐位相同"）。

**证据**：【逆向报告确凿】§2.3 NaniTrimeshChunk 的字段与 u8 索引/NaN 退化机制、§2.1 + §9.2 程序化 subset 运行时生成进 atlas 的 5 个条目清单。【我的推断】"反方向在 UE 里是负收益"的论证（依据是 UE 以 primitive 为剔除/光照键这一事实）与地面拆分的触发阈值。项目侧行号已核对；原稿关于 `BP_GroundShaper` 只引用三个 SM_StoneStep 资产的转述来自队友勘察 §3.1，本轮未反序列化复核，故此结论不依赖它。

---

### 已写好的 GPU LOD 选择今天在空转：默认开着，但 palette 原型全是单 LOD 网格——这是资产侧的空白不是代码侧的（顺带纠正队友勘察把默认值读反了）

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§1.5/§1.6【逆向报告确凿】LOD 切换阈值是投影半径 30 px；LOD1 是无顶点属性输入的 half_cube 替身，VS 从 gl_VertexIndex 位解码 8 角并逐轴朝相机镜像，gbuffer PS 对 ±0.5 slab 加四张倒角平面（3×d=−0.925 棱面、1×d=−1.35 角面）做逐像素光线求交并在 layout(depth_less) 下重写 gl_FragDepth。这套东西成立的前提是**百万级砖实例**且 TG 自控整条 Vulkan 管线。

**当前方案怎么做**：项目里唯一有 LOD 概念的是 `UCSGpuInstancedMeshComponent`：`CS_GPU_INSTANCED_MAX_LODS = 4`（`.h:14`），**`bGpuLODSelection` 默认 `true`**（`.h:274`——队友勘察 §5.4 记成 false，是把注释"Off pins every instance to LOD0"读反了，以本行为准），选择在 GPU 上按屏幕尺寸做（`Shaders/Private/CSGpuInstancedMesh.usf:204-215`），阈值取源 `UStaticMesh` 自己的 LOD 屏幕尺寸 × `LODScreenSizeScale`（`.h:277-279`，读取处 `CSGpuInstancedMeshComponent.cpp:191`），**所有 LOD 共享一条 VB/IB**、用 `FCSGpuInstancedLODRange{FirstIndex, NumIndices, BaseVertex, ScreenSize}` 定址（`.h:17-23`），每 LOD 一套 DrawIndexedIndirect 参数（`usf:231-249`）——结构上就是 TG 的 MeshAtlas。房屋本体不走这条：它是一整块 `UCSMesh`（`CSHouseActor.cpp:455-461`），千级三角、无 LOD、无实例化。

**差距**：`RebuildBaseMeshSnapshot` 只取源网格已有的 LOD（`CSGpuInstancedMeshComponent.cpp:174` 取 `min(LODResources.Num(), 4)`，并在第一个不合格 LOD 于 `:180`/`:184` break），而 `SM_StoneStep_{S,M,L}` 按计划 `:755` 是"暂用引擎 Cube 改 BuildScale3D 变形而来"——引擎 `/Engine/BasicShapes/Cube` 只有单 LOD。于是这套已经写好、默认开着的 GPU LOD 选择今天**一个三角也没省下**。这是资产侧空白，与第一轮的墙体渲染路径维度不同层：那边问"墙该不该是砖实例"，这边问"palette 原型资产该出几级 LOD、阈值放哪"。

**建议**：1）把"每个 palette 原型出 3 级 LOD"写进上一条的原型契约：LOD0 = 完整倒角几何；LOD1 ≈ 40% 三角；LOD2/3 = 8~12 三角的凸壳，**倒角信息烘进顶点法线与顶点色**（由第 3 条的 LUT 材质着色），既不靠几何也不靠逐像素求交。三级共占几百字节，与实例数无关（共享 VB/IB）。
2）LOD 切换阈值**不写进代码**——在 `UStaticMesh` 的 LOD 设置里定，运行期只用 `LODScreenSizeScale`（`.h:277-279`）微调。这与 TG"阈值在资产里、代码只读"是同一形态。
3）先花两分钟验证前提再动工：在 `L_TerrainOpsDemo` 里打印 `StepMeshes[i]->GetRenderData()->LODResources.Num()`（或看资产的 LOD 设置），确认确实是 1——我这条是从计划 `:755` 的"引擎 Cube 变形"推的，没读 uasset。
4）验收：石阶重做后拉远相机，用 `stat rhi` 看三角数随距离真降（GPU LOD 选择在 `InstanceCullCS` 里分桶写 indirect args，`usf:217-226`，三角数会真掉，不只是 draw call 数）。
5）明确**不做**：PS 逐像素光线求交 impostor 及为它引入的 Pixel Depth Offset / 自定义深度写入。理由写进计划风险节免得后来有人照 §1.6 硬抄——但这条属第一轮"墙体几何表示与渲染路径"维度的地界，此处只留一行结论，不展开定价。

**代价**：资产工作量（三块石阶 × 3 级 LOD），代码零改动。收益要等实例数上千才显现。UE 侧硬上限：`CS_GPU_INSTANCED_MAX_LODS = 4` 是 cull shader 里 `float4 LodScreenSizes` 的上限（`usf:44`、`.h:12-14`），源网格多余 LOD 直接被忽略（`.cpp:174`）。另需注意 `RebuildBaseMeshSnapshot` 在第一个不合格 LOD 就 `break`（`:180` 顶点数为 0 或切线数不匹配、`:184` 索引不足 3），所以 LOD 链中间不能有空档或顶点/切线数不匹配的层，否则后面的 LOD 会被**静默丢掉**——这一条也该进原型契约的校验器。

**证据**：项目侧为代码事实（含 bGpuLODSelection 默认值 = true，CSGpuInstancedMeshComponent.h:274，与队友勘察 §5.4 所记相反）；TG 侧【逆向报告确凿】（§1.5/§1.6）；"SM_StoneStep_* 是单 LOD"为【我的推断】，由计划 :755 的资产来源描述推出，**未读 uasset 验证**，故提案里列了验证步骤。

---

### 缺一层"离线烘焙的背景撒布表"这一资产形态：TG 把与玩家无关的植被存成 32 字节 GPU-instance 形状的序列化记录，项目侧没有对应物

*维度：资产格式与顶点语义_渲染风格的物质基础*

**Tiny Glade 怎么做**：§9.3【逆向报告确凿】随机化不在 prefab 内——`assets/glade/<theme>/trees.json` 索引到烘焙实例数组 `{position, scale, rotation_angle, id, pad0, pad1}`，报告原话"32 字节含显式 padding = GPU instance 结构 JSON 化"，对应 `PlacementDataSsbo` / `AssetPlacementDataLibrary`；"周边植被的'随机'是离线撒点烘焙（这也解释了依赖表中没有 poisson crate）"，只有玩家手植树才走运行时随机。§6.3【确凿】树木同理：预置树来自烘焙 placement 表，玩家手植走 `ui_place_tree`。

**当前方案怎么做**：D12（`Plan_House.md:598-720`）只有一层：`ACSGroundDecorItem : AActor`（`:620-640`，**用户裁决**），场在 GPU、放置决策在 CPU（`:662-668` 的分工表也标**用户裁决**），经异步回读取得（`:670`），提交链是分级异步状态机（`:673-689`）。计划里没有任何"与房子无关、关卡自带的背景植被"层——整张地面只会在房子周围晕开一圈装饰，离房子远的地方是空的。项目侧已有的序列化撒布形态只有 `ACSGroundShaperActor` 的石阶（GPU 现算、不落盘）与 `UCSGpuInstancedMeshComponent` 的 CPU 数组路径（`.h:210-239`，非序列化资产）。

**差距**：**只保留资产形态这一个切面**（放置策略、actor-vs-ISM、GPU 撒点均属第一轮"植被/摆件/藤蔓/杂物放置"维度，此处不重复）：TG 有一类"撒布结果本身就是资产"的东西——离线泊松撒点一次、序列化成紧凑记录、运行时只做上传与剔除、永不参与任何场/热力图/diff。项目里没有这个形态：所有实例撒布要么每帧 GPU 现算（石阶），要么是运行期 CPU 决策（D12）。少了它，Tiny Glade 那种"空地本身就有草甸与树"的底子在本方案里无处安放，而把它塞进 D12 会让一堆与房子无关的东西白白走一遍场 → 回读 → 谓词 → diff。

**建议**：新增一个**纯资产形态**，不动 D12 的任何裁决：
1）`UCSGroundBackgroundScatter : UDataAsset`——内容就是 TG 那张表的 UE 版：`TArray<FCSScatterEntry{ FVector3f Pos; float Yaw; float Scale; uint8 PaletteIndex; }>`（16 B/实例），由一次编辑器烘焙（`CallInEditor` 按钮：泊松盘 + `ACSGroundActor::SampleHeight` 贴地）产出并序列化。**这与 D12 的"每次全局重生成"不冲突**：它压根不参与重生成，是关卡的静态底子。
2）宿主直接用现成的 `UCSGpuInstancedMeshComponent` CPU 数组路径（`SetInstances`，`.h:210-239`），每个 palette 条目一个组件，与 `ACSGroundShaperActor` 的石阶完全同构（`CSGroundShaperActor.h:37`）。它不订阅 `OnGroundChanged`、不进 D12 的场/热力图/diff，只在地面高度真变了时重贴地（一次 `SetInstances`）。
3）撒布表进上一条的原型契约：条目引用的 `UStaticMesh` 同样要过 validator（pivot / 单位 / Allow CPU Access / LOD 链）。

**代价**：纯新增：一个 DataAsset + 一个烘焙按钮 + 复用现成组件，约 300 行，零风险，不碰 D12 任何一行。UE 侧限制：`UCSGpuInstancedMeshComponent` 一个组件只有一个 `BaseMesh`、一个材质槽（`.h:187-192`），palette 有几种就要几个组件；材质必须勾 `Used with Instanced Static Meshes`；`SetInstances` 每次走一次阻塞 render flush（`.h:204-207` 明确警告），所以只能在烘焙/贴地这种非交互时刻调；实例数由可见实例缓冲 64 MiB 警告线兜底（`CSGpuInstancedMeshSceneProxy.cpp:248-254`）。**不推翻任何用户裁决**——"放置物是 actor"管的是 D12 那层响应式装饰，这一层是关卡静态资产，两者不重叠。

**证据**：TG 侧【逆向报告确凿】（§9.3/§6.3）；项目侧为代码事实。原结论中"把 D12 植被换成 ISM / 搬到 GPU 源"两条已被我剔除，理由见 rejected。

---

## 被否条目

### 楼梯系统_当前方案的完整空白（对抗性核验后）

- **同一种叠砖语汇"已经/将要写四遍"，`ACSSplineBlockActor` 与 `CSGroundShaperActor` 各抄了一份写手** — grep `AddTri|AddQuad|AddBox|MeshWriter` 覆盖 `Source/ComputeShaderGenerator/{Private/*.cpp,Public/*.h}`，除 `Private/CSHouseActor.cpp` 外零命中。`ACSSplineBlockActor` 用的是 StaticMesh 三角提取 + 变换 append（Private/CSSplineBlockActor.cpp:274-285），`CSGroundShaperActor` 石阶走 GPU 实例化，两者都没有、也不需要写手。计划 :593（原条目引成 :589）说的「第四种叠块语汇」指视觉语汇并列，不是代码抄写点。今天写了一遍，D7 角柱（计划 :294）是第二个消费者——修正后的表述已并入 findings 中该条。
- **计划 :590 记录的"绕序口径矛盾"是手写砖石写手重复的第一次发作，可作实证支持** — 因果归属错误，且行号错（实为 :595）。原文说的是 `CSMeshOps.usf::UploadStaticMeshIndicesCS` 的注释与 `CSMeshBuild.h` 常驻流面法线口径（差一个负号）冲突，发生在 **StaticMesh 导入路径**上，与 `FCSHouseMeshWriter` 的手写 `AddTri` 绕序毫无关系——`ACSSplineBlockActor` 一行手写三角都没有。这条实证不成立，删除后该 finding 的严重度相应由 medium 降为 low。
- **不建议引入 Octree/SpatialHash——房屋量级几十到几百，`TArray<TWeakObjectPtr>` 线性遍历够用** — 与计划直接冲突：Plan_House.md:486 逐字写着 `FCSSpatialHash2D Broadphase;   // footprint AABB，格边 ≈ 房屋典型尺寸`，:489-499 还给出了基于它的每帧接触重算顺序（含"接触重算必须在落座之后"的相位约束）。原条目基于队友报告"全插件无 Octree/SpatialHash"（这是**代码**现状，属实）就断言"项目侧没有、也不该有"，漏读了计划已规划的部分。建议改为直接复用计划已定的 broadphase。
- **平台楼梯的最小可行切片 = 给 `ACSSplineBlockActor` 加 `bStepMode`，三行 Z 量化、约 60 行、零前置** — 三处硬伤：① **朝向没处理**——块的旋转取自 `Spline->GetRotationAtDistanceAlongSpline`（Private/CSSplineBlockActor.cpp:282），带样条 pitch；只量化 `Location.Z` 会得到"层高对了但踏面仍是斜的"，必须同时压平 pitch。② **弧长域自相矛盾**——`SolveBlockLayout` 吃的是 `Spline->GetSplineLength()`（:212，三维弧长），块按 `GetLocationAtDistanceAlongSpline(Distance)`（:281，同样三维弧长）取样；原条目提出"把 TotalLength 换成 XY 投影长度，仍是一个参数"，但取样端不换就直接错位，换则需要一张 XY→3D 距离映射表，不是"一个参数"。③ **落点选错**——`ACSSplineBlockActor` 按自身注释（Public/CSSplineBlockActor.h:12-18）与计划 :593 是纯装饰城齿块排，不是步道。
- **项目侧唯一缺的是"竖向量化"（Z 按 StepHeight 取层、层内平、层间跳）** — 事实错误。`ACSGroundShaperActor` 已经完整实现了它：Private/CSGroundShaperActor.cpp:286-341 按层处理，每层一条等高线曲线，控制点第三分量恒为 `LevelZ`（:322），弧段用 `ACSSplineBlockActor::SolveBlockLayout` 沿弧长铺不同长度石阶（:301），计划 :451 与落地注记 :755 都写明了这条链（无头断言"画路穿台 → 27 级石阶"）。项目侧缺的不是竖向量化，是"沿用户自由路径"的驱动源与"墙顶步道"这个模型本身。

### 选中高亮_编辑反馈_换色换主题的零重建原则

- **「多发一组 FMeshBatch 就能做逐部件（这面墙/这个洞/这根墩）高亮」** — 代码核验推翻：`FCSMeshRenderSceneProxy::GetDynamicMeshElements`(`Private/CSMeshRenderComponent.cpp:59-65`) 的 batch 与 arg set 一一对应，而 arg set == 材质槽（`Public/CSMeshOps.h:399-402` 明写 section i == argset i == material slot i）。房体只有 2 槽（`Private/CSHouseActor.cpp:350` SlotWall=0/SlotRoof=1），四面墙共用槽 0。因此这条路最细只到「全部墙 / 屋顶」，做不到「这一面墙」。要做到部件粒度就得把该部件挪进新槽再跑 `BuildMaterialSections` —— 恰是原稿自己在「明确排除的做法」里排除掉的那条。已在 finding 2 里替换为「原生 EditorSelection 通道（整栋）+ Color 通道位 + 一个颜色流 range 算子（逐部件）」。
- **「用逐像素 discard 做拖拽期门洞预览」** — 两条理由。① 与第一轮维度①（门洞与开洞机制：逐像素 discard vs MeshBoolean）正面重复——「开洞的表示用不用 discard」正是那个维度的题目，本轮不应重复主张。② UE 侧代价被原稿低估：`discard` 要求墙材质 blend mode 改成 Masked，会改变整面墙的 shader 排列、早 Z 与各 pass 参与情况（还会反过来影响 finding 2 里「原生描边能否白拿」的判断），为一个仅在降级路径出现的预览付这个代价不成比例。finding 5 已收窄为「只做染色提示、不改 blend mode」。
- **「地面应新增 RestampBaseColor()（写镜像 Colors + 一次颜色流 pass）」** — 重复造已有功能，且语义有害。按底色重铺镜像的操作已经是 `ACSGroundActor::ResetPaint()`（`Public/CSGroundActor.h:173-174` CallInEditor 按钮，实现 `Private/CSGroundActor.cpp:192-199`）。而把它挂到 `BaseColor` 的属性编辑上会**抹掉用户已画的道路权重**——`BaseColor` 的定义就是「镜像重置时铺的底色」(`CSGroundActor.h:95-97`)，不是实时底色。正确修法只是把 `BaseColor` 从 `bShapeProperty` 里删掉并补一句注释，已并入 finding 1 的 proposal 第 2 步。
- **「TG 的屋顶雪是全报告唯一一处 CPU 生成整片壳 mesh」** — 逆向报告 §3.3 只写了「`gen_roof_snow_mesh` CPU 生成整片壳 mesh（非实例化）」，从未主张唯一性；且报告里另有多处 CPU 生成网格——§1.3 的 plaster 网格生成、§4.1 的 `gen_stair_floors→StairsFloorMeshChunk`、§8.3 的 `generate_curved_halftimber_meshes`。把它当成「全报告唯一」再据此推出「TG 自己给出了裁决线」，是把推论建在不存在的原文上。已在 finding 4 中删去「唯一」并把裁决线明确降为【我的推断】。

### 碰撞与空间查询基建

- **D7 接缝 broadphase 从 footprint 换成「可见体积盒」（XY 外扩 RoofOverhang、Z 上界取 RidgeH）** — 与用户裁决及角柱语义正面冲突，且原稿把它包装成「裁决没有被正确实现」——不成立。计划 D7 原文写明角柱的用途是「在每个轮廓交点立一根叠砖角柱遮住墙体穿插处」，而屋檐相交时两面墙并未接触；按外扩盒生成的柱子 Top=min(檐口)、Bottom=落地，等于在两栋没碰上的房子之间凭空竖一根从地面到檐口的叠砖柱，恰好违反同一条裁决的下半句「靠得再近只要没碰上就不生」（Plan_House.md:291）。屋檐互穿是真实存在的观感问题，但归属是「屋顶之间」（TG 用 inter_roof_merlons 专门处理，§3.3/§8.2），不是接缝触发条件。已改写为 low 级提醒 + 一条否定用例保留在 findings 里。
- **D7 交点表从「4×4 线段求交」换成 TMeshAABBTree3::FindAllIntersections 取 Segments** — 对两个矩形 OBB 而言解析版本精确、微秒级、且是纯函数——计划 :507 明确按这个形态设计，P5 验收门 :743 要求「SAT/交点表/迟滞/合并单测绿」。换成 AABB 树需要先有 CPU 代理三角网（本身是被 REJECT 的另一条），代理还刻意不含洞，会引入「树的口径 vs openings 表的口径」双份真相；换来的好处（多边形/曲边 footprint、屋顶压墙）计划今天一个都没要。纯粹的复杂度增加。
- **为每栋房建 60 三角 FDynamicMesh3 代理 + TMeshAABBTree3，替代 PickHouse 的参数化 OBB 解析求交** — 与两处成文的既定架构冲突，且原稿没有引用它们：计划风险节「拾取全靠解析…地面=高度场求交、房/窗/handle=OBB 求交」，以及 Public/CSTinyGlade.h:24-25 的基类头注释「派生类的拾取一律解析实现（镜像高度场 / 参数化 OBB）」。对矩形房的射线拾取，解析 OBB 求交本就是最优解（零内存、零维护、可单测）；树的价值只在计划当前不需要的查询上（mesh-mesh 交线、任意多边形 inside、最近表面点），而 D12 的摆件吸附经核验可用声明式锚点解决，不需要查询世界。已把其中成立的部分——门面收敛 + 「真需要时后端必须是 GeometryCore 而非 Chaos/自写八叉树」的预先裁决（含 8 处实测引擎行号与「GeometryFramework 公开依赖 GeometryCore，公共头零构建改动」这条加强证据）——降级改写后保留在 findings 里。
- **ACSHouseActor 加静态多播 OnAnyHouseGeomChanged 作为 ColliderDiff 的最小等价物** — 与计划 :64 的架构表冲突：「房屋网格重建产出 | 无需发布——其他房屋只关心 footprint，经快扫可见」，且整个 D3 是用户裁决（v1 全量直推、不引入 dirty）。原稿自称「与 D3 直推裁决完全兼容」，但 D3 裁决的是「地面→房屋」这条通道必然直推，并未授权新开「房屋→下游」的通道；D12/D13 的下游本就统一挂在 NotifyEditCommitted 这条松手边界上（:656 附近、:722 附近），不需要第二条。理由不足以推翻裁决，已在 findings 里降级为提醒。
- **「N=20 个塑形物加载时约 2600 万次 SampleShapeHeight」（O(N²×66049)）** — 算错了，数字必须撤。三条反证：(a) RegisterShaper 在已登记时直接 return（Private/CSGroundActor.cpp:428），而地面 PostRegisterAllComponents 先跑 ResolveShapers() 批量登记全部 shaper（:409、:417-422），常见加载序下全图重建根本不触发；(b) 每个 shaper 自己的 RebuildTerrain 只刷 union(旧足迹,新足迹)（Private/CSGroundShaperActor.cpp:389-393），不是全图；(c) 即使最坏注册序，代价是 Σ ≈ N(N+1)/2×66049 ≈ 1.4e7 而非 2.6e7。Public/CSGroundActor.h:155-156 的头注释显示作者已自评过这条代价。剩余仍成立的部分（内层 shaper 循环无足迹剔除、藤蔓逐候选点全世界迭代）已改写后保留在 findings 里。
- **「D6 承诺的『门只认边缘线段』在代码里没有兑现」** — 过强，与代码不符。ComputeDoors 全程只消费 F.Start / F.U / F.In / F.Len（Private/CSHouseActor.cpp:202-224），没有任何一处认「矩形四面墙」——计划 :221 的承诺在消费侧已经兑现。硬编码的只是线段的提供方 CSHouse_GetEdge（EdgeIndex & 3）与三处 for(Edge<4) 循环。已改写为「缺一个多边形 footprint 的 provider」这一准确表述保留在 findings 第 1 条里。
- **「TG 的地形高度场与碰撞体藏在同一个 API 后面，调用方不需要知道自己在问谁」标为【逆向报告确凿】** — 标注等级错误。MESH_GENERATION_ANALYSIS.md §7.3 原文只写「CPU 副本供 RaycastWorld::raycast_w_terrain / mesh_follow_terrain / constrain_to_be_above_terrain 消费」——三个符号名是【确凿】，但「统一门面、调用方无感」是由函数命名推出的解读，且该小节标题本身带【末端链路待确认】。已在 findings 第 2 条降级为【我的推断】。

### 交互形态：画线成墙 vs 放房子拉尺寸

- **（原 Finding 4 的子论断）D5 handle 父子回路会让墙在鼠标停下后「持续蠕动几十帧」，并连带让 D6 拱阵在鼠标停下后仍重排若干帧** — 递推 e_{n+1}=(e_n+δ)/2 在 δ=0 时确实几何衰减，但它每一步都需要一次 `PostEditMove` 事件来驱动——而 `PostEditMove(bFinished=false)` 只由编辑器 gizmo 的鼠标输入触发，房子自己 `SetActorLocation` 移动父级**不会**回调子 actor 的 `PostEditMove`。鼠标停住 = 无事件 = 无迭代，不存在自发蠕动；残差在 `bFinished=true` 时被计划 :211 的「统一摆回规范位置」清掉。由此派生的「拱阵在鼠标已停下后仍重排」同样不成立（拖拽**过程中**因墙长跨过 `N=round(Usable/DoorPitchTarget)`（CSHouseActor.cpp:206）取整边界而重排是预期行为，计划 :234 已列）。该 Finding 的 2× 放大主张经独立推导后成立并保留，仅剥离这两条派生断言。
- **（原 Finding 3 的核心量化论据）D7 的二值接触判据使可用带只有几十厘米、下沿是硬的零，用户必须手动命中一个零测度边界** — 由计划三条参数直算：penetration > 0 成立、交点两侧外露走线长 ≥ SeamMinExposure=40 cm（:341）、penetration < LinkMaxOverlap=较小房子半宽（:540，默认 600×400 时 200 cm）——实际可用带约 40–200 cm，宽约 1.6 m，gizmo 拖拽轻松命中。P5 验收门 :743 的「停在刚接触处不闪烁」是要求实现别在临界处抖，不是要求用户命中边界，原稿把它读反了。该 Finding 的另一半（项目全无 house-to-house 对齐吸附，而 TG 的 shape_placement 吸附在 §8.5/§10.2 是【确凿】的一等公民）成立并已按修正后的表述保留。
- **（原 Finding 6 的行动项）多边形 footprint 相关的一切只准落在 GeometryCore，不要新增对 GeometryAlgorithms 的依赖面，否则这个坑要等到很晚才暴露** — 因果讲反了。`ComputeShaderGenerator.Build.cs` 的 PrivateDependencyModuleNames **已经无条件**列了 `GeometryAlgorithms` 与 `DynamicMesh`（不在 `if (Target.Type == TargetType.Editor)` 分支内），而 `PCGPlugins.uplugin:47-53` 把 `GeometryProcessing` 限成 `TargetAllowList: ["Editor"]`。也就是说这个 Runtime 模块**今天就已经编不进 Game/Client/Server target**，与多边形 footprint 无关，也不会「很晚才暴露」。克制新依赖面不能改变任何事。正确行动项（跑一次 Game target UBT 确认真实报错，再决定放开 uplugin 的 TargetAllowList 还是把 ComputeShaderGenerator 也标成 Editor-only）已写进保留后的 Finding。
- **（原 Finding 1 的 TG 侧前提）房子是曲线闭合的副产物；TG 的建造交互就是画线** — 逆向报告里没有任何一句支持「房子是曲线闭合的副产物」，且与 §3.1【确凿】（屋顶是独立编辑对象、独立 crate `system_roof`、独立 `CreateRoofCmd`）相抵触。更要紧的是原稿漏掉了方向相反的确凿证据：§8.5【确凿】「墙体形状放置吸附（`shapes::shape_placement`）」、§10.2【确凿】`systems/wall/shapes` 与 `snapping` 并列、§3.1【确凿】屋顶只有 rectangular/circular 两条装配路径、§8.3【确凿】`construct_timberframe_circle/rectangle`——**矩形/圆形状放置在 TG 里同样是一等公民**。剥离该前提后，Finding 1 的结论（不做画线、把 footprint 升级为闭合折线）反而更强，已按修正后的 tg_mechanism 保留。

### 规模上限与性能预算的量化对照

- **把 Mirror.Heights 改成 UPROPERTY(Transient)（原第 4 条 proposal (1)）** — 净收益为负，且会引入新缺陷。CSGroundActor.cpp:491 的 `if (!bChanged) return;` 幂等短路（注释自述「加载后重导出结果与序列化值一致时不重建、不标脏」）正是以序列化的 Heights 为比较基准。改 transient 后冷加载时 Heights 为空 ⇒ bChanged 恒为 true ⇒ 每次开图都会执行 :504 的 `MarkPackageDirty()` 把关卡标脏、并跑一次带 flush 的 `DisplaceGroundShapers`。用「每次加载脏包 + 一次 flush」换 264 KB(@256²) 的存盘体量不划算。1024²（4.2 MB）时可再议，已改为记入计划开放问题 :776 而非建议。
- **加载期最坏 O(V·K²/2)、13.87M 次 SampleShapeHeight、20 次 GPU pass + 20 次广播（原第 4 条 gap (c)）** — 被代码里两道既有守卫驳回。① `RegisterShaper` 有去重早退 `if (Shapers.Contains(Shaper)) return;`（CSGroundActor.cpp:428，注释明写「已登记：区域更新由塑形物自己发，别在这里再跑一次全域」）；② 地面的 `PostRegisterAllComponents`（:405-411）先跑 `ResolveShapers()`，它用 `TActorIterator`（:417-421）枚举世界里已存在但组件尚未注册的塑形物，关卡加载时 K 个全部收进来，随后每个塑形物的 `RebuildTerrain()`（CSGroundShaperActor.cpp:551/:383）走到 `RegisterShaper` 全部早退，不触发全域重导出。加上 :491 的幂等短路，冷加载既不跑 GPU pass 也不广播。二次方路径只在「地面先注册、塑形物之后才 spawn」的运行时/逐个拖入场景可达，而那本就是应当重算的语义。残留的 K×V 成本来自 EnsureMirrorInitialized 的全表扫描，已单独保留为该条唯一有效发现。
- **异步化落地后删掉 csh.LiveCutHz / csh.LinkRebuildHz，换成「每帧至多一次 EditMeshAsync + pending 合并」（原第 6 条 proposal (2)）** — 与计划自己写明的用途相反。计划 :202 明确：「**异步只解决『不阻塞 GT』，不减少 GPU 工作量**；实时重切仍靠 `CachedOthersMesh` 单刀增量 + `csh.LiveCutHz` 节流控制总量」——这个闸保护的是 GPU 工作量，异步化恰恰不减少它，所以「异步化后就可以删」的推理链断在这里。原稿称「它保护的其实是 flush」也不成立：:568 给 `LinkRebuildHz` 的理由已经逐字写着「砖生成 + 上传 + flush」，作者本就知道成本构成。另外两个常量目前都还只是计划里的名字（全插件仅有的 `csh.` CVar 是 `CSGroundShaperSteps.cpp:21` 的 `csh.StepScatterDebug`），删一个尚未存在的东西没有意义。
- **现在就给藤蔓提交链加显式分帧闸 / 写死「一次重算总 target ≤ 300」（原第 5 条 proposal (2) 与量化表）** — 直接推翻计划 :730 的用户裁决（「不需要在本计划里再加节流或分帧调度」），而支撑理由不足以达到「极强」的门槛：唯一的分子 305–311 ms 是 `buildLeaf(wallclock)`，README:180 自己标注它「混着图本身的量和渲染线程有多堵」、同进程内出现过 164 ms 与 553 ms 的尖峰，并指定真实耗时须用 `stat gpu` / Insights 抓 `VineMesh.Build`。且把总耗时线性摊到 980 个 target 上是错误的成本模型——`PrepareSurfaceVoxelInputs`(10–12 ms) 由 `VoxelSize` 与包围盒决定、求解由 `SC.Iteration=55` 主导，都与 target 数无关。据此外推出的「20 房 ×300 target = 1.9 s」不具备定价效力。已降级为：改写 :730 的线程口径 + 把 P9 验收门 :747 换成 RT/GPU 口径 + 先做一组 target 数标定，标定结果再决定要不要提闸。
- **「当前方案的解析拾取比 Tiny Glade 严格更省且零延迟，应写进 README 当优势」（原第 6 条 gap (a)）** — 两侧都有事实错误。TG 侧：报告 §7.3【确凿】写明 TG 同时有 `_terrain_editing_read_terrain_height.cs` 回读出的 CPU 高度副本，供 `RaycastWorld::raycast_w_terrain / mesh_follow_terrain` 消费，GPU 1000 步 raymarch 并非其唯一拾取路径，且它的独有产出是「命中的笔画 id」，纯高度查询给不出。本侧：`RaycastGround` 的平地闭式快路径（CSGroundActor.cpp:358-366）只在 `MaxAbsHeight==0` 时成立，而 D9 塑形物已落地（计划 :755），有起伏时走 :383-401 的定步进 march，步长 `CellSize*0.5`、上界取地面盒对角线 ⇒ 256² 约 724 步、1024² 约 2,896 步 CPU 双线性，与 TG 的 1000 步同阶。该点已改写为第 6 条内的一处实质发现（含过期注释与 Z 板裁剪的修法）。

### nani架构_GPU驱动实例渲染层

- **「`FGPUSceneWriteDelegate` 是公开但无范例的 API，引擎里没有任何一个参考调用者」（原第 1 条的核心风险论据）** — 事实错误，经复核推翻。`D:/UnrealEngine-5.7.4-release/Engine` 下有三个真实调用者：`Plugins/FX/NiagaraNanite/.../NiagaraStaticMeshComponent.cpp:230-241`（一个 `UStaticMeshComponent` 子类，暴露 `UpdateInstanceGPU(NumRequiredInstances, lambda)`，几乎是本议题的模板类）、`Plugins/PCG/.../PCGInstanceDataInterface.cpp:423`（完整范例，含容量就绪检查、`RegisterExternalBuffer` 接 GPU 产物、custom float data，配套 shader `Plugins/PCG/Shaders/Private/PCGSceneWriter.usf`）、`Plugins/FX/Niagara/.../NiagaraRendererMeshes.cpp:1163-1193`（`FMeshBatchDynamicPrimitiveData` 单帧形态）。原稿的 grep 只覆盖了 `Source/Runtime`。这一条不是删掉了事——它把 spike 的风险与成本都显著下调，已改写进第 1 条的 gap 与 cost，并替换成三条真实限制（基础网格必须是 UStaticMesh / OcclusionCull 默认关且 ECVF_Preview / 字段语义无独立文档）。
- **「part 索引落在源实例行的 `Transform2.w` / `Transform3.w`——那是白送的两个 float」（原第 2 条提案步骤 2）** — 有害，必须删除。`Engine/Shaders/Private/LocalVertexFactory.ush:1376-1383` 的 `VertexFactoryGetInstanceHitProxyId`（USE_INSTANCING 变体）把 `Transform1.w - 256*GetInstanceSelected()`、`Transform2.w`、`Transform3.w` 三个 .w 直接当 hitproxy 颜色的 R/G/B 读——原稿只核对了 `GetInstanceTransform`（:426-434）与 `GetInstanceSelected`（:477-480）两处，漏了这一处。而 `Shaders/Private/CSGpuInstancedMesh.usf:222-224` 把源行**逐字**拷进可见行，所以源行里的 part 索引会原样进 VS，编辑器点击石阶会选中随机 actor（今天三个 .w 全写 0 正是 `CSGpuInstancedMesh.usf:157` 与 `CSGroundSteps.usf:93` 两处注释在守的不变量，也与计划 `:13`「不做自定义 HitProxy」的用户裁决一致）。更关键的是这一步**根本不必要**：part 索引只决定 compaction 桶，几何范围由该桶的 indirect args 携带，永远不需要到达 VS。已在第 2 条里替换为"源行加第 6 个 float4，可见行保持 5 个不变"。
- **「正方向：atlas 条目应允许是运行时生成的网格（palette 条目做成 `UStaticMesh*` / `FCSGpuMeshCPUData` 变体）」（原第 5 条 A 部分）** — 无消费者，属凭空立项。`SetBaseMeshFromGpuData`（`CSGpuInstancedMeshComponent.h:199`）与 `bBaseMeshIsExternal`（`.h:362`）经全仓 grep **零调用者、从未置真**；今天两个真实用户都用资产基础网格（石阶 `CSGroundShaperActor.cpp:419` `SetBaseMesh(StepMeshes[Index])`、点刷 `CSPointBrushActor.cpp:277`）。原稿给出的首个受益者"石阶 palette 混入 CPU 参数化生成的踏面"是发明出来的，计划里没有这条需求——D9 的裁决（`:450-452`）是照 Houdini 原型的三块资产石阶。且若第 1 条 spike 通过（走 GPU Scene / ISM 路径），基础网格必须是 `UStaticMesh` 资产，这条整体作废。第 5 条只保留反方向的"不做"结论与地面拆 primitive 的推论。
- **「在 `PreRenderView_RenderThread` 里加视锥过滤：`if (!InView.GetCullingFrustum().IntersectBox(Proxy->GetBounds()...)) continue;`」（原第 4 条提案步骤 2）** — 与既有不变量冲突，且原稿的安全性论证只在单视图族成立。`CSGpuInstancedMeshSceneProxy.h:68-72` 明确记录："The cull runs once per frame for the first view — with split screen every view therefore draws the primary view's visible set, which over-draws rather than under-draws." 按**第一个视图**的视锥跳过 cull，会让"视图 1 看不到、视图 2 看得到"的 proxy 用上一帧的陈旧 indirect args 绘制，把刻意保留的 over-draw 变成 under-draw/错画——这正是该注释在防的方向。收益本身也极小（该组件今天只有 4 个实例）。已在第 4 条里降级为"建议先不做；要做只能限定 `Family->Views.Num()==1` 或对全部视锥取并集"。scene 指针过滤那一道保留，它是纯净收益。

### 资产格式与顶点语义_渲染风格的物质基础

- **把 D12 的植被层渲染宿主从 actor 换成 per-class ISM（原第 4 条提案第 2 步）** — **计划里其实已经写了，被当成缺口。** `Plan_House.md:713-715` 的风险节原话："植被上千时逐 actor 开销可观。v1 靠 diff 把 churn 压成增量；量级真上去再把植被类降级为 per-class ISM（父类字段不变，只换渲染宿主——列开放问题）"。这是已经识别并有意延后的降级路径，不是遗漏。同时它整体落在第一轮维度⑥（植被/摆件/藤蔓/杂物放置）的地界，与本维度（资产格式与顶点语义）无关。
- **把 D12 的植被撒布整层搬到 GPU 源、并在同一 pass 里写掉 RT_DecorHeat 的 W 通道以砍掉一整个 CPU 往返（原第 4 条提案第 3 步）** — **与两条用户裁决正面冲突且理由不足。** 计划 `:602` "放置物是 **actor**（用户裁决）"；`:662-668` 的分工表"候选 + 谓词 + spawn/diff → CPU"同样标注**用户裁决**，给出的理由是"输出物是 actor；且谓词天然顺序依赖（先放的挤掉后放的），GPU 并行化要多趟冲突消解，不划算"——而 `:657` 又把"层内处理顺序钉死为格坐标字典序"作为确定性纪律的支柱。把植被搬上 GPU 等于放弃这条确定性保证，换来的"6-10 帧砍一半"收益原文自己标为【我的推断】未实测，且原文的 cost 栏也承认"这一点必须在实现前当场确认，否则退回第 2 步"——自认为不成立的提案不该以 medium 严重度提出。此外整条也属第一轮维度⑥。
- **把石阶 palette 的长度轴从局部 +Y 统一改成 +X，并重做 SM_StoneStep_{S,M,L}（原第 5 条提案第 1 步）** — **破坏性改动换不到功能收益，理由不足，已在 findings 里降级为"显式声明 LengthAxis + validator"。** 石阶的局部 +X 已被"踏面进深"占了语义（`Shaders/Private/CSGroundSteps.usf:84-91` 的 LocalX 径向 / LocalY 环切向分工，头文件注释 `CSGroundShaperActor.h:74` 与实现 `CSGroundShaperActor.cpp:181/188` 一致），把长度轴搬到 X 只是把冲突挪个位置，并不消除它。代价却是砸掉已落地并有无头断言验收的 D9 成果：按计划 `:755`，`SM_StoneStep_{S,M,L}` 与"画路穿台 → 27 级石阶""台顶 300 / 裙边中点 150 / 影响外 0"等断言都要重做重跑。真正的问题是"失配静默"，用一个 `LengthAxis` 成员 + 校验器（"最长边是否等于声明轴"）就能消除，且零资产返工。

### 光追GI代理_光照与阴影的一致性

- **（原 finding 4 步骤④）收掉 `RebuildPillarMesh` 的上传旁路，让房屋家族只剩一个 GPU 产物出口** — 三重理由删除。① **对本议题不必要**：核实 `Private/CSHouseActor.cpp:490` 的 `UCSMeshOps::CopyFromMeshSnapshot` 同样走 `EditMeshSync` 并 bump `FCSMeshResident::Generation`（`Private/CSMeshOps.cpp:1036`），因此柱子的 `UCSMeshRenderComponent` 照样会收到 `HandleMeshChanged`——只要按修正后的方案挂在 `HandleMeshChanged` 而非 `UploadTinyGladeSnapshot`，柱子一条都跑不掉，收不收旁路对光照代理失效毫无影响。② **与第一轮维度⑤（变更传播/重建触发/CPU-GPU 边界纪律）正面重复**，它是一条纯粹的上传管道架构整理项，不属于光照维度。③ **已是计划的既有后续项**：`Private/CSHouseActor.cpp:486` 注释自陈「参数化改造是计划 D9 的后续项」，本轮无权也无必要为它另立议题。
- **（原 finding 1）S1（自定义非 GPU-Scene 顶点工厂）与 S2 并列为可选路线，且「代价近乎为零」** — 作为并列选项删除，降级为「不推荐的备选」并入 finding 1。理由：① 「材质槽全空所以新增 VF 的 permutation 代价近乎为零」不成立——项目 `Config/DefaultEngine.ini` 开着 `r.Substrate=True`，permutation 基数本就高，且这个理由天然只在「还没上真材质」的窗口期成立，用它论证一条长期架构选择是错的；② S1 强依赖 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0`，而该 cvar 默认为 1 且被 Epic 在说明文本里明写 deprecated（`ShadowSetup.cpp:344-349`），是项目全局的性能与版本风险，而 S2 一个 cvar 都不需要动；③ 对房屋/地面 S1 被 S2 严格支配（S2 的 `KnownVertexCount/KnownIndexCount` 已经现成，`Public/CSMesh.h:118-119`）。S1 唯一的独有收益是让藤蔓/道路那些「只有 GPU 知道计数」的叶子也能投影，那是另一个议题的诉求，不应绑进房屋光照这一条来抬高它的可行性评分。
- **（原 finding 3）验收判据：按 C 路烘出 StaticMesh 后开 `r.DistanceFieldAO 1` + Mesh DistanceFields 可视化看拱洞是否实心** — 判据本身自相矛盾，已在 finding 3 中替换。原判据要求先执行同一份报告明确否掉的 C 路（编辑器阻塞回读烘 StaticMesh，与计划的零回读纪律冲突），成本远高于它要验证的那条一行纪律。替换为：放一个普通 `UStaticMeshComponent` 立方体挂 Masked 挖洞材质，直接开 `Show > Visualize > Mesh DistanceFields` 看洞是否实心——一分钟可跑、完全不动房屋代码，验的是同一条引擎行为（`MeshRepresentationCommon.cpp:303-304`）。


---

## 合成 agent 存活部分

角几何；LOD1 ≈ 40% 三角；LOD2/3 = 8~12 三角凸壳，**倒角信息烘进顶点法线与顶点色**（由 M7 的 LUT 材质着色），三级共占几百字节且与实例数无关（共享 VB/IB）。② LOD 切换阈值**不写进代码**——在 `UStaticMesh` 的 LOD 设置里定，运行期只用 `LODScreenSizeScale`（`.h:277-279`）微调，与 TG「阈值在资产里、代码只读」同形态。③ 动工前先花两分钟验证前提：在 `L_TerrainOpsDemo` 打印 `StepMeshes[i]->GetRenderData()->LODResources.Num()` 确认确实是 1。④ 验收：石阶重做后拉远相机用 `stat rhi` 看三角数真降（GPU LOD 在 `InstanceCullCS` 里分桶写 args，`usf:217-226`，三角数会真掉，不只是 draw call 数）。⑤ **明确不做**：PS 逐像素光线求交 impostor 及为它引入的 Pixel Depth Offset / 自定义深度写入（这条属第一轮「墙体几何表示与渲染路径」的地界，此处只留结论不展开定价）。
**代价**：资产工作量（三块石阶 × 3 级 LOD），代码零改动，收益要等实例数上千才显现。UE 侧硬上限 4 级（`usf:44` 的 `float4 LodScreenSizes`），源网格多余 LOD 直接被忽略。**另需注意** `RebuildBaseMeshSnapshot` 在第一个不合格 LOD 就 `break`（`.cpp:180` 顶点数为 0 或切线数不匹配、`:184` 索引不足 3）⇒ LOD 链中间不能有空档，否则后面的 LOD 被**静默丢掉**——这一条也该进 M8 的校验器。

---

##### L4. 缺一层「离线烘焙的背景撒布表」这一资产形态
**TG**：§9.3【确凿】随机化不在 prefab 内——`assets/glade/<theme>/trees.json` 索引到烘焙实例数组 `{position, scale, rotation_angle, id, pad0, pad1}`，报告原话「32 字节含显式 padding = GPU instance 结构 JSON 化」，对应 `PlacementDataSsbo`；「周边植被的『随机』是离线撒点烘焙（这也解释了依赖表中没有 poisson crate）」，只有玩家手植树才走运行时随机。§6.3【确凿】树木同理。
**当前**：D12（`:598-720`）只有一层：`ACSGroundDecorItem : AActor`（`:620-640`，**用户裁决**），场在 GPU、放置决策在 CPU（`:662-668` 也标用户裁决），经异步回读取得。**计划里没有「与房子无关、关卡自带的背景植被」层**——整张地面只会在房子周围晕开一圈装饰，离房子远的地方是空的。
**差距（只保留资产形态这一个切面；放置策略属第一轮⑥）**：TG 有一类「撒布结果本身就是资产」的东西——离线泊松撒点一次、序列化成紧凑记录、运行时只做上传与剔除、**永不参与任何场/热力图/diff**。项目里没有这个形态：要么每帧 GPU 现算（石阶），要么运行期 CPU 决策（D12）。把它塞进 D12 会让一堆与房子无关的东西白白走一遍场 → 回读 → 谓词 → diff。
**建议（纯资产形态，不动 D12 任何裁决）**：① `UCSGroundBackgroundScatter : UDataAsset`——`TArray<FCSScatterEntry{FVector3f Pos; float Yaw; float Scale; uint8 PaletteIndex;}>`（16 B/实例），由一次编辑器烘焙（`CallInEditor` 按钮：泊松盘 + `SampleHeight` 贴地）产出并序列化。**它压根不参与 D12 的重生成，是关卡的静态底子。** ② 宿主直接用现成的 `UCSGpuInstancedMeshComponent` CPU 数组路径（`SetInstances`，`.h:210-239`），每个 palette 条目一个组件，与石阶完全同构；不订阅 `OnGroundChanged`，只在地面高度真变了时重贴地。③ 撒布表进 M8 的原型契约（同样过 validator）。
**代价**：纯新增，约 300 行，零风险，不碰 D12 一行。UE 限制：一组件一个 `BaseMesh` 一个材质槽；材质必须勾 `Used with Instanced Static Meshes`；`SetInstances` 每次走一次阻塞 render flush（`.h:204-207` 明确警告）⇒ 只能在烘焙/贴地这种非交互时刻调；实例数由 64 MiB 警告线兜底。

---

##### L5. 屋檐/屋脊伸出 footprint，但这**不说明接缝判据错了**——错的是「屋顶之间」没有负责人
**TG**：§8.5【确凿】空间索引写的是 `WallSpatialHash / **wall_roof_hashmap**`；§3.3/§8.2【确凿】`construct_inter_roof_merlons` / `inter_roof_merlons` —— 「两个屋顶之间」在 TG 里是一等的几何关系且有**专门的产物**，不是靠墙缝合顺带解决的。
**当前**：D7/D10 用户裁决：触发 = 两房 2D footprint OBB **真正相交** + Z 区间相交（`:291`/`:503-508`）；角柱语义原文写得很清楚——「在每个轮廓交点立一根叠砖角柱**遮住墙体穿插处**」。代码侧可见几何确实伸出 footprint：`CSHouseActor.cpp:424` `EaveOut = LB*0.5 + RoofOverhang`、`:426` `LAtot = LA + 2*RoofOverhang`，`RoofOverhang` 默认 25 cm（`.h:73`）。
**差距（正确表述）**：两栋房 footprint 相距 40 cm（<2×25）时屋檐已互穿 10 cm，肉眼可见——**这条几何观察成立**。但据此断言「用户裁决没被正确实现」是错的：角柱遮的是**墙体穿插**，而屋檐相交时两面墙并没有接触，此处立一根从地面砌到檐口的叠砖柱，是在两栋没碰上的房子之间凭空竖一根柱子，**恰好违反同一句裁决的下半句「靠得再近只要没碰上就不生」**。footprint 判据对角柱而言是对的。真正的空白在别处：**屋顶与屋顶的相交在本计划里没有任何机制负责**（TG 用 `inter_roof_merlons` 专门做这件事）。
**建议（不改任何触发盒）**：① **P5 落地时补一条否定用例**：「两房 footprint 间距 20 cm、屋檐互穿」→ 断言**不生角柱**。把当前的正确行为钉死，防止将来有人把 broadphase 外扩成「可见体积盒」而无人察觉。② 屋顶-屋顶穿插归入「屋顶」维度而非接缝维度，记为独立议题、不排期；需要时是一个新的产物类型（如屋脊交接件），不是给接缝换判据。**（若第一轮③已覆盖屋顶交接，以其结论为准；本条只保留「接缝判据无需改动」这个否定性结论。）**
**代价**：一条单测 + 一条记录。

---

##### L6. 砖石写手 `FCSHouseMeshWriter` 是文件私有的，D7 角柱是第一个真正抄不到它的消费者
**TG**：§4.1【确凿】楼梯底下的拱/托架/柱**不是楼梯自己写的**，走墙体构造器 `system_wall_constructor::construct_playermade_stairs::{arches_n_walls, brackets, pillars, magic_support::...}`；§1.2【确凿】该 crate（53 个 .rs、约 214 条唯一符号）同时服务墙、平台楼梯与栅栏——**上层只出布局意图，砌法统一由这一个 crate 决定**。
**当前（订正原索赔）**：grep `AddTri|AddQuad|AddBox|MeshWriter` 覆盖 `SRC/{Private/*.cpp,Public/*.h}`，除 `CSHouseActor.cpp` 外**零命中** —— 今天只写了**一遍**，不是四遍。`ACSSplineBlockActor` 根本不用写手（走 `CSSplineBlock_ExtractPaletteEntry` 提取 StaticMesh 三角 + 变换 append，`.cpp:274-285`）。成立的部分：`FCSHouseMeshWriter` 确在**匿名 namespace** 内（`CSHouseActor.cpp:13-117`，struct 本体 23-85，边框注释 :15「Unity/jumbo 构建共享 TU，file-local 一律 `CSHouse_` 前缀」）。
**差距**：D7 角柱按 `:294` 是「CPU 生成经基类上传管道进自己的 `UCSMesh`」——**它必须要一个 CPU 写手，而现在拿不到**。几何原语与 `AddBox`/`AddQuad` 逐字相同，但在另一个 .cpp 里，只能重抄一遍绕序（`cross(B-A,C-A)`）、UV 平铺周期（`CSHouse_UVScale = 200`，:17）、法线的 `TransformVectorNoScale` 用法(:38-39)。抄错的症状是「某面不显示 / 法线朝内」，定位成本高。楼梯支撑落地时会出现第三个消费者。
**建议**：一个头文件，不新建模块：把 `FCSHouseMeshWriter` 原样提到 `SRC/Public/CSMeshWriter.h`（改名 `FCSSnapshotWriter` + `COMPUTESHADERGENERATOR_API`）——它已经通用，只依赖 `FCSGpuMeshCPUData` + `FTransform`，零房屋语义；`CSHouse_GetEdge` 按 H1 另行处理（那是房屋语义）。**「再抽一个 `CSMasonry.h` 放 `AddArchSpan`/`AddStackedColumn`」现在不做**：`AddArchSpan` 只有一个消费者且要按 H3 参数化，D7 的堆块规则还没写过一次——抽两个都没有的东西是投机；等 D7 写完、楼梯托架成为第二个堆块消费者时再提取。`SolveBlockLayout`（`CSSplineBlockActor.h:79-81`）原地不动（已是共享的一维打包器且已单测）。
**代价**：纯搬运约 60 行位移，零行为变更；时机 = D7/P5 动工前，抄之前抽最省。

---

##### L7. TG 三套楼梯里项目只做了 §4.3 那一套；**§4.2 在当前房屋模型里没有落点**
**TG**：§4 标题即「楼梯：**三套并存**的系统」——§4.1 玩家绘制（CPU 图状态 `StairsState{add_node, add_edge, apply_diff, extract_diff_and_make_backup}` + `stairs_assemble` + 穿墙开洞 + parry 形变）【确凿】；§4.2 平台楼梯 `construct_platform_stairs` + `split_wall_path_segment_into_platform_stairs`（**墙顶步道跨高差时自动分段成台阶**）【确凿】；§4.3 岩地台阶 100% GPU【确凿】。§10.3【确凿】撤销 = 重放笔划再走同一构建管线。（「§4.2 不带任何重设施」是**被核倒的推断**——§1.2 明写墙路径切段的打分依据正是「与其他墙/**楼梯**的交点」，说明平台楼梯嵌在墙路径切段机制里。）
**当前**：计划全文 grep「楼梯」= **0**（792 行）。已落地的是 §4.3：`ACSGroundShaperActor` 的塑形物阶梯（`:451-457`/`:755`，代码 `CSGroundShaperActor.cpp:181-344`）。
**差距（形状与直觉不同）**：① **§4.2 没有落点**——它的前提是「墙顶步道 + 沿墙有高差」，而项目的墙是刚性矩形、`WallHeight` 恒定、整栋座在单一 Z（`CSHouseActor.cpp:349`/:182-183）⇒ **墙顶本身就是水平的**；要有 §4.2 先得有「墙路径」这个概念，那是比楼梯本身更大的模型变更。② **§4.1 的真门槛是状态形态，不是几何**——`StairsState` 是持久、可撤销、节点共享的图，而项目当前是「每 actor 一份参数、派生物全 transient、零撤销」（`PillarMesh`/`CurrentDoors` 都是 `UPROPERTY(Transient)`；全树 `FScopedTransaction` 只有 2 处且都在 `EditorShortcuts`）。把可撤销图塞进这套架构是另一个量级的工程。
**建议（排期与措辞的纠正，不提新代码）**：① 计划里补一句楼梯的**范围声明**——今天做的石阶 = TG §4.3；§4.1/§4.2 明确不在 D1..D13 内（现在计划全文「楼梯」零出现，读者会误以为 D9 石阶就覆盖了楼梯）。② 将来的顺序：**§4.1 的「沿自由路径的踏步 + 底下砖石支撑」**（不穿墙、不进图状态）→ 穿墙开洞（前置 = H3 字段 + M15 注入通路 + M16 扫掠查询全部就位）→ 图状态与邻接对齐（最贵、最后做，且需先解决零撤销）。**§4.2 排在「墙路径模型」之后，不作为入口。** ③ 若要试第一步，起点是**已跑通的 `CSGroundShaperActor` 分层铺装链**（`.cpp:286-341`：切层 → 每层一条曲线 → `SolveBlockLayout` 沿弧长铺 → 每层恒定 `LevelZ`），把「等高线环」换成「用户样条」即可——那已经是竖向量化 + 弧长打包的完整实现，比从 `ACSSplineBlockActor` 改起近得多。
**代价**：零代码（文档一句范围声明）。附带纠正一个会误导排期的估计：「给 SplineBlock 加 60 行」不成立（理由见第 4 节）。
**与第一轮④的边界**：④覆盖的是 §4.3 那套地形台阶；本条只讲 §4.1/§4.2 这两套**不属于地形**的楼梯及其排期关系。

---

##### L8.【提醒】派生物全部由输入推导且**无抑制口**
**TG**：§4.1【确凿】楼梯下的砖石支撑自动生成，且**专门有一个分支** `magic_support::playermade_stair_floating_bricks`，报告的中文注解是「删支撑后出现『魔法悬浮砖』」。（**「支撑是用户可删的对象、TG 主动放弃物理正确性换交互自由」是【我的推断】**——报告只给了符号名与那半句注解，没说是谁删、怎么删、删的状态存在哪。）
**当前**：计划核心承诺是「目标状态 = 当前输入的纯函数」，全篇贯彻（接缝 actor `RF_Transient` 不序列化、`:293`；柱网格 Transient；门每次从道路顶点色重算；塑形物「删塑形物 = 地形塌回 + 产物全消失」`:447` 用户裁决）。承重柱的生成条件是纯阈值 `Gap > PillarMinGap`（`.h:161-162`/`.cpp:284-285`），**无任何 per-instance 关闭口**。
**差距**：这套写法的好处（幂等、无历史、可测）计划已讲透，不推翻。未被讨论的后果是：**用户对派生物没有否决权**。楼梯支撑一旦落地，「删掉这一根托架」下一次 `ReevaluateSite()` 就把它生回来，因为「我删过它」不在任何权威状态里。同类还有「想要一栋不长柱子的悬空房」「想让某一处不开门」。
**建议（这是提醒，不是建议改）**：若将来需要，形状可以做到不碰纯函数性质——加 `UPROPERTY() TSet<uint32> SuppressedDerived;`（序列化，key 复用各派生物已有的稳定 id，门用 `ComputeDoors` 已在用的 `(Edge<<24)|(N<<16)|Slot`，`CSHouseActor.cpp:227`），在 `ComputeDoors`/`ComputePillars` 末尾各加一句 `Contains → continue`——**抑制集本身就成了输入的一部分，纯函数性质不受影响**。必须并进两份 desc 哈希（:247-252 与 :280-291）。key 必须像滞回 key 一样带上重排身份（`N` 进 key），否则墙长一变抑制会误落到别的槽（`.h:236` 的注释已为滞回记过一次这个教训）。**一处需修正的类比**：`DoorSlotOpen`（`.h:237`）连 `UPROPERTY` 都不是，所以它证明的是「跨重求值的非序列化记忆可以存在」，**不**证明「序列化旁路已被架构接受」——加一个序列化的 `TSet` 是**新引入的先例**，得当成新决策对待。
**代价**：~30 行 + 两处哈希 + 一个 `CallInEditor` 入口（不做自定义 HitProxy，计划已裁决不做）。真正的建议只有一句：别把「用户否决派生物」推迟到楼梯支撑落地时再想，那时它会以「托架删不掉」的形式突然变成阻塞项。

---

##### L9. 明确否掉反方向：不做 NaniTrimeshChunk 式「primitive 内部 chunk 剔除表」
**TG**：§2.3【确凿】`_nani_stairs_floor` 与 `_nani_plaster` 用 GPU 指针网格——`NaniTrimeshChunk{uvec2 gpu_address; vertex_index_count; vertex_byte_offset; vec4 bounding_sphere}` 经 `GL_EXT_buffer_reference` 按字节地址读 **u8 局部索引**，每 chunk ≤256 顶点，`gl_VertexIndex ≥ 索引数` 时输出 NaN 退化——**每块唯一的程序化面片被伪装成「实例」塞进同一条实例管线**，每块自带包围球所以享受同一套剔除。§2.1+§9.2【确凿】5 个运行时程序化 subset 与 19 个 ron subset 走同一条管线。
**当前**：两条独立路线，计划从未评估过统一：(a) `UCSMesh` + `UCSMeshRenderComponent`（一个 primitive 一整块，房子/地面/接缝/spline 块）；(b) `UCSGpuInstancedMeshComponent`。两者之间只有半截桥 `SetBaseMeshFromGpuData`（`.h:199`，全仓零调用者）。
**差距**：技术上零件都有（`BuildMaterialSections` 已经是「GPU 计数排序 + 每槽一套 DrawIndexedIndirect 参数」，argset 上限 65536，把 chunk 当「材质槽」再加一个 pass 把视锥外 chunk 的 `IndexCountPerInstance` 写 0 是可行的）。**但不值得**：nani 用指针网格是在绕开自研 Vulkan「一次 draw 只能绑一套 VB/IB」的限制；UE 里同一件事的原生答案是 primitive 或 Nanite cluster，而 UE 的 bounds、视锥剔除、硬件遮挡查询、VSM 页剔除、Lumen card、距离场**全部以 primitive 为键**。把唯一几何藏进一个 primitive 内部的 chunk 表，等于对引擎的每一个剔除与光照系统再隐身一层——那正是 H6/H7 已踩到的坑的加强版。
**建议**：**A. 写死不做**，把结论写进 `CSMeshRenderComponent.h` 类注释与计划开放问题，措辞：「唯一几何按 **primitive** 切分，不做 primitive 内部的 chunk 剔除表。」零成本。**B. 顺手给开放问题 `:776` 一个渲染侧答案**（该条本就写着「待定」，不推翻任何裁决）：超大地面拆多 tile **组件/actor**，每片自己的 `UCSMesh` + 组件 + bounds。理由**不是**三角数（131k 三角每帧提交约 0.1 ms 量级），而是：只有拆成多 primitive，地面才能拿到逐片视锥剔除、逐片硬件遮挡查询、逐片 VSM 页剔除与逐片 Lumen card——单 primitive 时 128 m（上限 512 m）的包围盒对引擎每一个剔除系统都等于「永远可见」。触发阈值给一个可判定的数：`NumCellsX*NumCellsY > 512*512` 或地面边长超 200 m。
**代价**：A 零成本。B 只在阈值到了才动工，届时成本主要在接缝：地面镜像是单一权威数组，拆 primitive 时**镜像不要跟着拆**——只拆渲染宿主，`BuildSnapshotFromMirror` 按片取子矩形并让相邻片共享边界行顶点，法线在边界处从镜像重算而不是从片内差分，否则接缝会有一条可见的着色缝。动工前先写单测「两片边界顶点位置与法线逐位相同」。
**（与第一轮④有交集，此处只保留「primitive 粒度 = 剔除/光照粒度」这个渲染层切面。）**

---

##### L10. `EnsureMirrorInitialized` 里那趟**被自己注释否定**的全表扫描
**TG**：§7.1【确凿】TG 地形完全不存栅格——地形是参数化笔画集合，每次编辑由 `_terrain_editing_rasterize_terrain_stroke.cs` 逐 texel 重放全部笔画重新生成 heightmap；§10.3【确凿】「撤销/重做即『重放笔划再走同一构建管线』」。存档体量 = 笔画数 × ≤1024 点。
**当前**：`FCSGroundMirror::Heights` 是 `UPROPERTY() TArray<float>`（`CSGroundActor.h:33`），全插件对它的写入只有两处（`.cpp:92` 的 `Init(0)` 重置与 `:485-487` 的 `Slot = Height`），公式是纯绝对式 `max(0, 全部塑形物 SampleShapeHeight)`（:479-484）⇒ Heights 是塑形物集合的纯函数。
**差距（三条索赔里两条被既有守卫驳回，只剩一条）**：
- 「冗余序列化」**降级为有意取舍**：`:491` 的 `if (!bChanged) return;` 幂等短路（注释「加载后重导出结果与序列化值一致时不重建、不标脏」）**正是以序列化的 Heights 为比较基准**；改 transient 后每次加载都会 `MarkPackageDirty()`(:504) 把关卡标脏 + 触发一次带 flush 的 `DisplaceGroundShapers`。净收益为负。
- 「加载期 O(V·K²/2)」**驳回**：见第 4 节。
- **【唯一成立】`RefreshHeightsInRegion` 第一行调 `EnsureMirrorInitialized`(:459)，后者在尺寸匹配分支里无条件全表扫 Heights 求 `MaxAbsHeight`(:83-85)**；而同一函数在 `:493-503` 又从塑形物参数重算了一遍，注释还专门写「直接从参数取，不用扫全表（区域更新的意义就在于不碰区域外的格点）」。:83-85 那趟纯属浪费：1024² 地面上每次塑形物拖动帧遍历 4.2 MB；加载期 K 个塑形物的区域刷新叠加成 K×V（K=20、V=1,050,625 ⇒ 2,100 万次浮点比较）。
**建议（只做一件事）**：把 `:493-503` 抽成私有 `RecomputeMaxAbsHeightFromShapers()`，在 `EnsureMirrorInitialized` 的匹配分支(:79-86) 与 `RefreshHeightsInRegion` 两处都调它，删掉 `:83-85` 的全表循环。O(V) → O(K)，十几行，**无语义变化**（两者结果恒等）。
**代价**：极小。验证：`L_TerrainOpsDemo` 冷加载后台顶高度仍为 300、拖动塑形物时高度场表现不变。
**顺带记进开放问题 `:776`**：Heights 转 transient 只作为「1024² 配置下的可选项」，并写明门槛——一旦 `:790`「任意 mesh 形状塑形物走 GPU 光栅化 + 区域回读进镜像」落地，Heights 就不再是塑形物的纯函数，transient 化立即失效；届时正确形态是把回读结果存进**每个塑形物自己的足迹缓存**（比整张地面小两个数量级），维持「地面 Heights = 塑形物纯函数」这条不变量。这条不变量值得写进 `FCSGroundMirror` 的头注释。

---

### 3. 对第一轮门洞结论的压力测试：**逐像素 clip 在 UE 的 Lumen / 距离场里会不会让洞消失？**

> 「光追GI代理」维度被要求独立检验这一条。以下结论全部由 UE 5.7.4 引擎源码逐行核对得出。

#### 3.1 TG 侧的做法（对照组）

【逆向报告确凿】TG 把形状精修下放到像素级，**并且显式保住了阴影一致性**：
- §1.6：拱洞由跨拱砖携带 3 点拱高，gbuffer PS `if (world_y < 拱高) discard`，而 **depth-only/shadow PS 变体（f0adff76，全文 37 行）只保留同样的拱裁剪 discard** —— 换句话说，TG 专门为阴影 pass 编了一个只干「挖洞」这一件事的 PS。
- §3.2：瓦片天窗开洞同样在 depth-only PS 里遍历 `DormerHoleSsbo` 逐像素挖。
- 结语第 3 条【确凿】把这条上升为全局设计要点。

同时 TG 的**光追表示是另一套东西**：§9.3【确凿】prefab 的 rt 段是粗代理 `mesh:Aabb`；§1.3【确凿】`construct_walls` 的系统签名写 `RtWorld`。（「`RtWorld` 吃的是那份已按洞裁过的砖布局」是**【我的推断】**，报告未直接陈述；且 §1.2 里「按 `iter_holes_with_padding` 裁砖」这一步落在报告自标为**【推测】**的排砖顺序链内，`trim_rows` 工具链本身是【确凿】。）

**所以 TG 并没有「一份 clip 出来的洞同时服务所有系统」**——它在 raster 侧用 clip，在光追侧另建代理。这一点在下面的结论里很关键。

#### 3.2 UE 里，masked 材质的洞只在**一部分**通路里是洞

分界线正好切在这个项目将来最想要的地方：

**是洞（clip 生效）**：
- base pass；
- depth prepass；
- **阴影深度（SM 与 VSM 都会为 `!bWritesEveryPixelShadowPass` 的材质编译真 PS）** —— `Renderer/Private/ShadowDepthRendering.cpp:371,488`。这一半与 TG「shadow PS 保留 discard」等价；
- custom depth。

**不是洞（当成实心墙）**：
1. **网格距离场的离线生成只按 blend mode 分「半透明 vs 不透明或 masked」，masked 三角全量计入、opacity mask 从不求值** —— `Developer/MeshUtilities/Private/MeshRepresentationCommon.cpp:303-304`。
2. **Lumen 硬件光追的默认追踪用的是全局 hit group** `FLumenHardwareRayTracingMaterialHitGroup`（一个 `FGlobalShader`，`Renderer/Private/Lumen/LumenHardwareRayTracingMaterials.cpp:48-80`，仅 2 个 hit group，其 any-hit 是自相交规避而非材质掩码）—— **根本不走材质 any-hit，掩码不生效**。
3. **软件 Lumen 走 mesh SDF**，等同第 1 条。

#### 3.3 对本项目当下的判定

- **今天完全安全**：gpumesh 既不进距离场（`CSGpuMeshSceneProxy.cpp:26`）、也不进光追（无 `GetDynamicRayTracingInstances`）、也不进 Lumen（H7 已论证），所以「masked 洞在 GI 里是实心」这个分界线**今天根本触碰不到**。
- **修完阴影（H6/M19）之后仍然安全**：阴影通路认 mask（§3.2 上半）。这是一个重要的正面结论——**修阴影不会把 clip 变成陷阱**。
- **只在引入 GI 代理的那一刻变错**，而且错法致命：拱门在 GI 里被当实心墙，光不会从拱洞洒进室内，拱周围被 DFAO 当实墙压暗。**那正是 Tiny Glade 连拱最标志性的一张图。**

#### 3.4 第一轮的门洞建议要不要加条件？——**要，加两条**

第一轮维度①比较的是「逐像素 discard vs MeshBoolean」这个**切法**，本节不对切法本身表态。但无论第一轮的结论倒向哪一边，都必须补上下面两条限定，否则该结论在 GI 落地那天会失效：

**条件 A（硬约束，写进 `Plan_House.md` D6/D8 与 `Public/CSHouseActor.h` 头注释）：**

> **光照代理表示必须保留真几何洞口；逐像素 clip 只允许存在于 raster 表示。禁止出现「渲染网格是整块墙 + clip 挖洞，而 GI / 距离场 / 光追代理是同一块整墙」的组合。**

落到本项目：
1. `RebuildBodyMesh` 现在的参数化拱几何（`CSHouseActor.cpp:371-411`）就是唯一几何权威，同时供 raster 与将来的光追/烘焙代理 —— **这一点今天已成立，代价为零**；D6 转布尔真洞（计划 `:184` 的用户裁决）后同样成立。
2. 采纳 H7 的 B 路（光追实例）时，BLAS 直接从常驻流位置/索引缓冲建 —— **它就是带真洞的那份几何**，自动满足约束，无额外工作。
3. 采纳 C 路（后台烘 StaticMesh）时，烘的必须是几何洞版本；用 `SaveToStaticMesh` 从同一份常驻流出资产即可，同样自动满足。
4. **不为 GI 单独造一份「简化成整块墙」的低模** —— 一栋房千级三角，低模化收益本来就不大。

**条件 B（时效声明）：** 如果最终决定 GI 只走屏幕空间、永不做代理，那么 clip 无风险 —— 但必须把这条写进注释并标明它是**「暂时安全」而不是「永久安全」**，安全性来源是「gpumesh 目前对距离场/光追/Lumen 全部隐形」这个**缺陷**，而不是某个正确的设计保证。一旦有人为了拿阴影/GI 而给代理接线（这正是 H6/H7 在推的方向），安全性当场失效。

**另一条必须同时考虑的连带影响（跨到 M5/M7）**：在 UE 里 `discard` 要求墙材质 blend mode 改成 **Masked**，这会：
- 改变整面墙的 shader 排列、早 Z 行为与各 pass 参与情况（成本方向：负）；
- 但**反过来让 `FHitProxyVS::ShouldCompilePermutation`（`SceneHitProxyRendering.cpp:62-71`）为它编译**，从而白拿引擎原生选中描边（成本方向：正，见 M5(A)）。

所以「第一份墙材质用 opaque 还是 masked」这一个决定同时绑着三件事：clip 能否用、原生描边能否白拿、shader permutation 基数。**建议把它和 M7 的 master material 一起定，不要分头决策。**

**验收判据（一分钟可跑，完全不动房屋代码）**：在 `L_HouseGroundDemo` 放一个普通 `UStaticMeshComponent` 立方体、挂一张用 opacity mask 挖洞的 Masked 材质，开 `Show > Visualize > Mesh DistanceFields` —— 洞在可视化里是实心块即证明分界线存在（对应 `MeshRepresentationCommon.cpp:303-304`）。

---

### 4. 不建议改的（被否条目中有信息量的）

#### 4.1 看似「白送」，其实有害

| 条目 | 为什么不能做 |
|---|---|
| **把 mesh atlas 的 part 索引塞进源实例行的 `Transform2.w` / `Transform3.w`** | `Engine/Shaders/Private/LocalVertexFactory.ush:1376-1383` 的 `VertexFactoryGetInstanceHitProxyId`（USE_INSTANCING 变体）把 `Transform1.w - 256*Selected`、`Transform2.w`、`Transform3.w` 直接当 hitproxy 的 R/G/B 读；而 `CSGpuInstancedMesh.usf:222-224` 把源行**逐字**拷进可见行 ⇒ part 索引会原样进 VS，点石阶会选中随机 actor。今天三个 `.w` 全写 0 正是 `CSGpuInstancedMesh.usf:157` 与 `CSGroundSteps.usf:93` 两处注释在守的不变量。**而且这一步根本不必要**：part 索引只决定 compaction 桶，永远不需要到达 VS。 |
| **在 view extension 里加视锥过滤跳过 cull** | `CSGpuInstancedMeshSceneProxy.h:68-72` 明确记录「一族只在第一个视图上 cull，其余视图沿用主视图可见集，宁可 over-draw 不可 under-draw」。按第一个视图的视锥跳过，会让「视图 1 看不到、视图 2 看得到」的 proxy 用上一帧的陈旧 args，把刻意保留的 over-draw 变成 under-draw —— 正是该注释在防的方向。scene 指针那一道保留（M11）。 |
| **把 `Mirror.Heights` 改成 `Transient`** | `CSGroundActor.cpp:491` 的幂等短路正是以序列化的 Heights 为比较基准。改 transient 后 Heights 反序列化为空 ⇒ `bChanged` 恒 true ⇒ **每次开图 `MarkPackageDirty()` 把关卡标脏 + 跑一次带 flush 的 `DisplaceGroundShapers`**。用「每次加载脏包」换 264 KB(@256²) 不划算。 |
| **「多发一组 `FMeshBatch` 就能做逐部件高亮」** | batch 与 arg set 一一对应，而 arg set == 材质槽（`CSMeshOps.h:399-402`），房体只有 2 槽、四面墙共用槽 0 ⇒ 最细只到「全部墙/屋顶」。要到部件粒度就得把该部件挪进新槽再跑 `BuildMaterialSections` —— 恰是同一份提案自己排除掉的那条。 |

#### 4.2 与用户裁决冲突且理由不足

| 条目 | 冲突点 |
|---|---|
| **D7 broadphase 换成「可见体积盒」（XY 外扩 `RoofOverhang`、Z 取 `RidgeH`）** | 角柱语义是「遮住**墙体穿插**处」，屋檐相交时两墙并未接触；按外扩盒生成的柱子会在两栋没碰上的房子之间凭空竖一根从地面到檐口的叠砖柱，**违反同一条裁决的下半句**（`:291`「靠得再近只要没碰上就不生」）。真问题归属是「屋顶之间」（TG 用 `inter_roof_merlons`），不是接缝触发条件。 |
| **给 openings 加 `NormalizeOpenings` 做「相交取并」** | 直接改写 `:387`/`:408` 那条被标为「**用户点明的收益**」的语义（同处不会被登记两次 → 不会被切两次）。替代方案见 M15 第 ③ 条：给非标记类生产者一条不同的仲裁分支，门/窗侧一字不改。 |
| **加静态多播 `OnAnyHouseGeomChanged` 作为 `ColliderDiff` 的等价物** | 与计划 `:64` 的架构表冲突（「房屋网格重建产出｜无需发布——其他房屋只关心 footprint，经快扫可见」），且整个 D3 是用户裁决。D3 裁的是「地面→房屋」必然直推，并未授权新开「房屋→下游」通道；D12/D13 的下游本就统一挂在 `NotifyEditCommitted` 这条松手边界上。 |
| **把 D12 植被搬到 GPU 源、在同一 pass 里写掉 `RT_DecorHeat` 的 W 通道** | `:602`「放置物是 **actor**（用户裁决）」+ `:662-668` 的分工表同样标用户裁决，理由是「谓词天然顺序依赖（先放的挤掉后放的）」，而 `:657` 把「层内处理顺序钉死为格坐标字典序」作为确定性纪律的支柱。搬 GPU 等于放弃这条确定性，换来的「6-10 帧砍一半」原提案自己标为未实测。 |
| **删掉 `csh.LiveCutHz` / `csh.LinkRebuildHz`** | 计划 `:202` 明确：「**异步只解决『不阻塞 GT』，不减少 GPU 工作量**；实时重切仍靠单刀增量 + `csh.LiveCutHz` 节流控制总量」—— 「异步化后就可以删」的推理链断在这里。`:568` 给 `LinkRebuildHz` 的理由已逐字写着「砖生成 + 上传 + flush」，作者本就知道成本构成。 |
| **现在就给藤蔓加分帧闸 / 写死「一次重算总 target ≤ 300」** | 推翻 `:730` 的用户裁决，而唯一的分子 305–311 ms 被 README `:180` 自己标注为不可靠；把总耗时线性摊到 980 个 target 上也是错误的成本模型（体素化与迭代次数都与 target 数无关）。改为先标定（M21）。 |
| **把石阶 palette 长度轴从 +Y 统一改成 +X 并重做 `SM_StoneStep_*`** | 石阶的 +X 已被「踏面进深」占了语义（`CSGroundSteps.usf:84-91`），搬轴只是把冲突挪个位置；代价是砸掉已落地并有无头断言验收的 D9 成果。正确解是显式声明 `LengthAxis` + validator（M8）。 |
| **给每栋房建 60 三角 `FDynamicMesh3` 代理 + `TMeshAABBTree3` 替代 OBB 解析拾取** | 与计划风险节及 `Public/CSTinyGlade.h:24-25` 的基类头注释「派生类的拾取一律解析实现（镜像高度场 / 参数化 OBB）」冲突。对矩形房，解析 OBB 求交本就是最优（零内存、零维护、可单测）；树的价值只在计划今天不需要的查询上，且会引入「树的口径 vs openings 表的口径」双份真相（代理不含洞）。 |
| **D7 交点表换成 `TMeshAABBTree3::FindAllIntersections`** | 对两个矩形，4×4 线段求交精确、微秒级、纯函数，而 P5 验收门 `:743` 明确要求 SAT/交点表单测绿。换树要先有 CPU 代理网（已被否），纯粹的复杂度增加。 |
| **不建议引入 SpatialHash（「线性遍历够用」）** | 这条**反过来**与计划冲突：`:486` 逐字写着 `FCSSpatialHash2D Broadphase;`，`:489-499` 还给出了基于它的每帧顺序。原判断基于「代码里没有」就断言「也不该有」，漏读了计划已规划的部分。 |

#### 4.3 论据被代码/引擎源码驳回

| 索赔 | 驳回依据 |
|---|---|
| **「`FGPUSceneWriteDelegate` 公开但无参考调用者」** | 引擎里有三个：`NiagaraNanite/.../NiagaraStaticMeshComponent.cpp:230-241`、`PCG/.../PCGInstanceDataInterface.cpp:423`（配套 `PCGSceneWriter.usf`）、`Niagara/.../NiagaraRendererMeshes.cpp:1163-1193`。原 grep 只覆盖了 `Source/Runtime`。**这条不是删掉了事——它把 H8 spike 的风险与成本都显著下调。** |
| **「D5 handle 会在鼠标停下后持续蠕动几十帧」** | 递推 `e_{n+1}=(e_n+δ)/2` 每一步都需要一次 `PostEditMove` 事件驱动，而房子自己 `SetActorLocation` 移动父级**不会**回调子 actor 的 `PostEditMove`。鼠标停住 = 无事件 = 无迭代。由此派生的「拱阵在鼠标停下后仍重排」同样作废。**2× 放大主张独立成立并保留（H2）。** |
| **「D7 可用带只有几十厘米、下沿是硬的零、用户要命中零测度边界」** | 由 `:341` `SeamMinExposure=40` 与 `:540` `LinkMaxOverlap=200` 直算，默认参数下可用带约 **40–200 cm、宽 1.6 m**。`:743` 的「停在刚接触处不闪烁」是**鲁棒性**要求，不是要求用户命中边界。 |
| **「N=20 个塑形物加载时约 2600 万次 `SampleShapeHeight`」** | `RegisterShaper` 有去重早退（`CSGroundActor.cpp:428`），而地面 `PostRegisterAllComponents` 先跑 `ResolveShapers()` 批量登记(:409/:417-422) ⇒ 常见加载序下全图重建根本不触发；每个 shaper 的 `RebuildTerrain` 只刷 union(旧,新) 足迹；即使最坏序也是 ≈1.4e7 而非 2.6e7。**剩下的真问题只有 `:83-85` 的全表扫描（L10）与 `:481-484` 的无足迹剔除（M13）。** |
| **「D6 承诺的『门只认边缘线段』在代码里没有兑现」** | `ComputeDoors` 全程只消费 `F.Start/F.U/F.In/F.Len`（`CSHouseActor.cpp:202-224`），消费侧已兑现；硬编码的只是 provider。 |
| **「同一种叠砖语汇已经写了四遍」** | grep 全模块除 `CSHouseActor.cpp` 外零命中，今天只写了一遍。`ACSSplineBlockActor` 走 StaticMesh 提取 + 变换 append，根本不需要写手。计划 `:593` 的「第四种叠块语汇」指**视觉语汇**并列，不是代码抄写点。 |
| **「计划 `:595` 的绕序矛盾是手写写手重复的第一次发作」** | 那是 `CSMeshOps.usf::UploadStaticMeshIndicesCS` 与常驻流面法线口径的冲突，发生在 **StaticMesh 导入路径**上，与手写 `AddTri` 无关。 |
| **「平台楼梯最小切片 = 给 `ACSSplineBlockActor` 加 `bStepMode`，约 60 行、零前置」** | 三处硬伤：① 块旋转取自 `GetRotationAtDistanceAlongSpline`（`.cpp:282`）带样条 pitch，只量化 Z 会得到「层高对了但踏面仍是斜的」；② `SolveBlockLayout` 吃三维弧长(:212)、取样也按三维弧长(:281)，换成 XY 投影长度而不换取样端会直接错位，换则需要一张映射表；③ `ACSSplineBlockActor` 按 `.h:12-18` 与 `:593` 是纯装饰城齿块排，不是步道。 |
| **「项目侧唯一缺的是竖向量化」** | `ACSGroundShaperActor` 已经完整实现了（`.cpp:286-341`，每层恒定 `LevelZ`，弧段用 `SolveBlockLayout`）。缺的是「沿用户自由路径」的驱动源与「墙顶步道」这个模型本身。 |
| **「解析拾取严格优于 TG 的 GPU raymarch，应写进 README 当优势」** | 两侧都错：TG 侧 §7.3【确凿】还有 `_terrain_editing_read_terrain_height.cs` 回读出的 CPU 高度副本供 `raycast_w_terrain` 消费，GPU raymarch 的独有价值是**返回笔画 id**；本侧的平地闭式快路径只在 `MaxAbsHeight==0` 时成立，D9 落地后走 march，256² 约 724 步、1024² 约 2,896 步 CPU 双线性，与 TG 同阶。**该点已改写为 M22(d) 的实质发现。** |
| **「TG 的屋顶雪是全报告唯一一处 CPU 生成整片壳 mesh」** | 报告从未主张唯一性；§1.3 的 plaster、§4.1 的 `gen_stair_floors`、§8.3 的 `generate_curved_halftimber_meshes` 都是 CPU 生成的。 |
| **「房子是曲线闭合的副产物 / TG 的建造交互就是画线」** | 报告无原文支撑，且与 §3.1【确凿】（屋顶是独立编辑对象 + 独立 crate + 独立 `CreateRoofCmd`）相抵触；反方向的确凿证据（§8.5 `shape_placement`、§10.2 `shapes`+`snapping`、§8.3 `construct_timberframe_circle/rectangle`）被漏掉了。**剥离该前提后 H1 的结论反而更强。** |
| **「TG 的地形高度场与碰撞体藏在同一个 API 门面后」标为【确凿】** | §7.3 原文只写「CPU 副本供这三个函数消费」，三个符号名是【确凿】，「统一门面、调用方无感」是由命名推出的解读，且该节标题带【末端链路待确认】。 |
| **「切段后内部段不铺砖 / 拓扑切割而非装饰性遮挡」标为【确凿】** | §1.2 末段只有一句「墙路径按与其他墙/楼梯的交点打分切段」，**报告里没有任何一句**说被切出的段会被丢弃；`merge_walls` 同样可以解释成「合并成一条曲线后重排砖」。 |

#### 4.4 计划里已经有 / 已排期，不算缺口

- **D12 植被换 per-class ISM**：`:713-715` 的风险节原话「植被上千时逐 actor 开销可观。v1 靠 diff 把 churn 压成增量；量级真上去再把植被类降级为 per-class ISM（父类字段不变，只换渲染宿主——列开放问题）」。
- **`FCSWallOpening` / `CSHouseTypes.h` / `UCSHouseSubsystem` / `FCSSpatialHash2D`**：`:74` 与 `:486` 已规划，进度注记明写 subsystem 是**有意延后**（「快扫等 D7/D8 需要跨房关系时再上（D10）」）。所以 H1/M12/M16 的定位是「已排期的债在落地代码里先开始累积」，不是「计划漏了」。
- **地面材质资产 / 道路通道争抢**：`:752`（「'顶点色混合'的地面材质资产仍待补」）与 `:777`（「道路语义通道固定 R 是否够用……届时引入通道→语义映射表」）都已识别。M6/M7 补的是**房体侧**与**总预算硬边界**。
- **第三个材质槽**：`:182` 明标「装饰砖（**预留**）」，需要它的 D7（`:294`）尚未落地，代码只声明 2 槽是正确的当前态。
- **`SetBaseMeshFromGpuData` / 运行时生成的 atlas 条目**：全仓**零调用者、从未置真**，首个受益者是发明出来的；且若 H8 spike 通过（基础网格必须是 `UStaticMesh` 资产），这条整体作废。

---

### 5. 三档归类

#### 现在就该做

低成本 + 高确定性 + 修的是正确性缺陷或过期事实。**建议按此顺序**（前四条互不依赖，可并行）：

| # | 事项 | 量级 |
|---|---|---|
| 1 | **H4** 地面 `GroundMaterial`/`BaseColor` 摘出 `bShapeProperty`；房屋补 `PostEditChangeProperty` + `BindTinyGladeMaterials` | ~30 行 |
| 2 | **H5** 两处 `nullptr` → `GetCustomPrimitiveData()` + `PushPrimitiveParams()` + 材质约定 | 2 行 + 一个方法 |
| 3 | **M5(A)** 点选房子看有没有原生描边（可能今天就已生效） | 5 分钟实测 |
| 4 | **M4** 跑一次 `-Target=UETest574_2Game` 的 UBT，拿真实报错再定 uplugin 的 `TargetAllowList` | 几分钟 |
| 5 | **H10** 拆 `ShapeHash` / `PlacementHash` + `BodyBuiltAtTransform` + 走 `TransformMesh` | ~30 行 |
| 6 | **H9(1)(2)** `PaintVertexColorsSphere` 加区域参数 + 落笔路径改 `EditMeshAsync` | ~20 行 + 异步改造 |
| 7 | **H6** 阴影 S2（`bDrawWithKnownCounts` + section span + **M18 的失效触发**） | <100 行 |
| 8 | **M11** view extension 加一行 scene 过滤 | 1 行 |
| 9 | **M13(1)(2)** `RefreshHeightsInRegion` 足迹剔除 + `Shapers` const 访问器 + 藤蔓迭代提循环外 | 各几行 |
| 10 | **L10** 抽 `RecomputeMaxAbsHeightFromShapers()`，删 `:83-85` 全表扫描 | 十几行 |
| 11 | **M22(2)** 改 `RaycastGround:357` 的过期注释 + Z 板裁剪 | 一行 + 十行 |
| 12 | **M2** 屋脊滞回 `CachedRidgeYaw`（并拍板是否序列化） | ~10 行 |
| 13 | **M6(1)(2)(3)** 房体通道字典写进头注释 + `AddTri` 花掉那 4 字节 | 一行参数 |
| 14 | **L1** desc 哈希纪律注释（`ComputeDoors` + `ComputePillars` + 计划 D4） | 2 处注释 |
| 15 | **第 3 节的条件 A** 写进计划 D6/D8 与 `CSHouseActor.h` 头注释 | 一段话 |
| 16 | **L7** 计划补楼梯范围声明；**M22(1)(3)** 改写 `:142`/`:770`/`:792` 的预算判断 + 加「预算去向表」 | 文档 |
| 17 | **M21(1)(2)** 把 `:730` 按线程改写、`:747` 换成 RT/GPU 口径 | 文档 |
| 18 | **M20(A)** 地面容量表写进 meta/README + `>512²` 打 Warning | 半小时 |

**动工前必须先改的（时机就是全部代价）**：
- **H2**（D5 记账量法）—— D5 动工前，15 行；
- **H3**（洞的 `Z0/Z1/AxisUS/Skew`）—— D8/P6 动工前，~80 行；
- **H1**（footprint 折线化 + `FCSHouseGeom`）—— D6/D7 的前置，~250 行；
- **L6**（提 `FCSSnapshotWriter` 到 Public）—— D7 角柱动工前，纯搬运；
- **L5**（D7 的否定单测）—— P5 落地时一条测试；
- **M8(1)(2)(3)**（原型资产契约 + `LengthAxis` 显式声明 + validator）—— 在 D6 cutter / D7 叠砖块动工前，半天到一天。

#### 等规模上来再做

有明确触发阈值，阈值没到就不动：

| 事项 | 触发阈值 |
|---|---|
| **H8** GPU Scene spike（`UpdatePrimitiveInstancesFromCompute`） | `:787` 门洞砖拱 或 `:782` decor 降级 ISM 落地、实例数上万。**它是 M9/M10/M19 的前置闸** |
| **M9** mesh atlas（`[part][LOD]` + 前缀和 compaction） | H8 spike 失败 **且** palette 条目 ≥3、组件数上量 |
| **M10** per-instance custom data | H8 结论出来后二选一实现；首个消费者 D9 石阶 |
| **M19(2)** 实例化阴影级联（用降级方案：全实例不剔除的一套 args） | D12 摆件上量 |
| **H7(B)** 光追实例 BLAS | **必须与「材质自发光/AO 伪造」或异步烘焙一起排期**，单独上线可能观感变差；地面 `NumCells > 512` 时必须设闸 |
| **M6(4)** UV1 扩容（先在多流/单交错两种口径里二选一） | 真需要 float 精度的逐顶点向量（拱剖面、弯曲骨点）时；**必须在房体格式 P2 冻结前决定口径** |
| **M7** master material + LUT + 四个 MI | 与「第一份墙材质 blend mode（opaque/masked）」一起定 —— 它同时绑着第 3 节的 clip 与 M5(A) 的描边 |
| **M8(4)** Houdini `tg_prototype_export` HDA | 原型资产真要走 DCC 时 |
| **L3** palette 原型出 3 级 LOD | 实例数上千 |
| **L4** `UCSGroundBackgroundScatter` DataAsset | 要「空地本身就有草甸与树」时 |
| **M14** `FCSFeatureAnchor`（依赖 H1，不依赖 M12） | D12 要做窗台花盆/门侧灯笼时 |
| **M15** `ICSWallOpeningProvider` 注入通路 | 楼梯或任何第三方开洞源动工时 |
| **M16** `SweptPrismVsWalls`（挂进 `UCSHouseSubsystem`，用 `:486` 的 broadphase） | 楼梯穿墙动工时；前置 `UCSHouseSubsystem` 已在 P2 验收门 |
| **M3** 内部墙段 Omit（`FCSWallSpan`） | **由用户裁决**；是开放问题 `:780` 的低成本候选，不替换 D7 角柱 |
| **M20(B)** 道路载体从顶点色换 512² 纹理 | **由用户裁决**；会让 D2 的 parity 验收判据失效，只提供定价 |
| **L9(B)** 地面拆多 tile primitive | `NumCellsX*NumCellsY > 512*512` 或边长 > 200 m |
| **M21(3)** 藤蔓 target 数标定（100/300/980/3000，抓 `VineMesh.Build`） | 决定是否提闸**之前**；半天 |
| **L8** `SuppressedDerived` 抑制口 | 楼梯支撑落地前想清楚，否则会突然变成阻塞项 |
| **M12(2)** GeometryCore 后端（`TMeshAABBTree3`/`TFastWindingTree`/`TPointHashGrid2`） | 真出现解析答不了的查询时；现在只写进选型记录 |

#### 明确不做

- **画线成墙作为主交互**（H1 的方向裁决：TG 自己就有 rect/circle 形状放置 + snapping，而项目侧零 HitProxy/Gizmo/InputBehavior + 零撤销，体验会低于拉尺寸）。
- **NaniTrimeshChunk 式 primitive 内部 chunk 剔除表**（L9A：UE 的剔除与光照全部以 primitive 为键，这是对引擎再隐身一层）。
- **给 gpumesh 加 `UBodySetup` / 引入 Chaos 做查询**（`CSTinyGlade.h:23-24` 已写进基类注释；且房体每帧重建 ⇒ 逐帧 cook）。
- **自写 Octree / SpatialHash / KDTree**（计划 `:486` 已规划 `FCSSpatialHash2D`；真要 broadphase 用 `TPointHashGrid2`）。
- **nani 的 1024 桶深度计数排序、32 实例合并窗口、自建两阶段 HZB**（UE 有 depth prepass 与 `FInstanceCullingMergedContext`，正确做法是打开 `r.InstanceCulling.OcclusionCull`）。
- **PS 逐像素光线求交 impostor 及为它引入的 Pixel Depth Offset / 自定义深度写入**（L3⑤；属第一轮维度①/②的地界，此处只留结论）。
- **H6 的 S1（自定义非 GPU-Scene 顶点工厂）**：强依赖 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0`（默认 1 且被 Epic 明写 deprecated，`ShadowSetup.cpp:349`），且新增一整套 shader permutation（项目 `r.Substrate=True`），对房屋/地面被 S2 严格支配。
- **把 D12 植被搬 GPU / 换 ISM**（用户裁决 + 计划已排期）。
- **石阶长度轴统一**（换成显式 `LengthAxis` 声明 + validator）。
- **D7 broadphase 换可见体积盒 / 交点表换 AABB 树 / 给房子建 `FDynamicMesh3` 代理**（4.2 与 4.3 已列理由）。
- **删 `csh.LiveCutHz` / `csh.LinkRebuildHz`**（它们保护的是 GPU 工作量，异步不减少它）。
- **现在给藤蔓加分帧闸**（先标定，见 M21）。
- **`RestampBaseColor`**（`ResetPaint()` 已是现成的 CallInEditor 按钮，挂到属性编辑上会抹掉已画的道路权重）。
- **用逐像素 discard 做拖拽期门洞预览**（要改 blend mode 为 Masked，代价与第一轮①重复；L2 收窄为只做染色）。

---

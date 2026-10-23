# GPU 基座与打包契约审查

> 子代理 B 的完整报告（2026-09-07，只读审查）。主审查抽查了 B1 / B2 / B6 / B7 / B10 与「中等问题 6」校核的证据后，把结论并入 [`Review_Structure.md`](Review_Structure.md)「GPU 基座与打包契约（深挖 F）」一节；本文保留九条路的布局对照表与阻塞点计数表作参考。行号按报告生成时刻。

- 范围：`Plugins/PCGPlugins/Source/ComputeShaderGenerator/` 的 `CSMesh*` / `CSGpuInstanced*` / `CSGpuMesh*` / `CSGroundShaperSteps` 与九条打包路（`CSHouseFrame` / `Vine` / `Tile` / `Decor` / `Pillar`、`CSGroundStairs` / `Cover` / `RockShell` / `GroundDecor`、`CSGroundShaperField`），宿主侧只读 `Ensure* / Rebuild* / Submit* / On*EditComplete / DebugRead* / Destroyed / EndPlay / BeginDestroy / FlushPaintToGpu`，测试与回归脚本只看阻塞计数与 GPU 计数断言。
- 基准：2026-09-07 工作区（只读，未构建、未运行）。行号按读取时刻记录，函数名为主。
- 引擎行为一律读 `D:\UE-SourceCode-5.7.4` 原文核实；标 **推测** 的是未读到源码或未跑用例的推演。
- 项目已拍板事项（材质禁 unlit / 终局烘 StaticMesh、禁真几何洞、岩壳披挂与倒角裁决）不再作为建议提出。

## 结论摘要

既有审查给这一层的「好」在**机制层**成立：`FCSMeshEditContext` 的登记 + 访问态恢复、`CountedBlockingFlush` 计数、pooled buffer 的引用持有方式，读透之后没有发现线程或生命周期上的真竞争。问题集中在**契约层**——布局靠魔数与注释对齐、容量扩张后不清零且四家早退门看不见容量身份、异步编辑在渲染线程改写游戏线程读的字段、原子槽位的不确定性在超容量与材质排序两处泄漏到产物。按严重度：

| # | 发现 | 严重度 | 标注 |
| --- | --- | --- | --- |
| B1 | `CSShaperSteps_GrowTo` 新 buffer（行 / counter / CustomData）不清零，而 Frame / Vine / Decor / SkirtDecor 四家的早退门不比较容量身份 ⇒ 扩容后交接的是池子上一位租客的字节，且不会重打包（既有 F4 的结构化版，还多了 counter 与 Pillar 的 CustomData 两处） | 高 | 已核实 |
| B2 | `EditMeshAsync` 在渲染线程改写 `Sections` / `WorldBounds` / `KnownCounts`，而 `CSMesh.h` 自述这些字段「flush 之后才可读」——异步路没有 flush，游戏线程的 `ResolveBatchMaterials` / `CalcBounds` / `IsEmpty` 与之无栅栏并发 | 高 | 已核实 |
| B3 | packed 行 stride `5u`、CustomData 步长、`FRAME_PATH_STRIDE`、位标志、BGRA 字节序、`Float4sPerShaper` 全靠两侧各写一份字面量；Frame / Vine 两处 usf 头注释与实际 stride 已经不一致 | 中 | 已核实 |
| B4 | 原子槽位的不确定性有三处未封装：剔除压缩的绘制序逐帧变（互穿砖的共面侧面）、石阶 / 地被超容量时**丢哪些**由原子序决定、材质排序的段内三角序不可复现 ⇒ 同一房体两次 `SaveToStaticMesh` 的三角序不同 | 中 | 已核实 |
| B5 | 交接包围盒：五家里只有 Frame 把构建空间盒映射到组件空间；石阶 / 地被的盒吃 `MaxAbsHeight` 且不量化；`BlockSize` / `BaseSphere` 烘进行里而网格身份不进哈希 | 中 | 已核实 |
| B6 | 阻塞计数的盲区不在本模块（31 处全部经 `CountedBlockingFlush`），而在引擎：首次给缺 usage 的材质建代理时 `CheckMaterialUsage_Concurrent → SetMaterialUsage → FMaterialUpdateContext` 两次裸 `FlushRenderingCommands` + 着色器编译；计数器数的是 flush 次数，不是 GPU idle | 中 | 已核实 |
| B7 | 「必须交回渲染线程释放」是错误的模型：`FRDGPooledBuffer` 是原子引用计数且池子恒持最后一份引用；真正的风险是**复用**（B1）与 **GC 滞留**（销毁组件不撤源 ⇒ 常驻集 + 生产者 buffer 拖到 GC）。纪律本身在 `GrowTo` 里也没守住 | 中 | 已核实 |
| B8 | `SetStreamLayoutSync` 的按字节前缀拷贝对 stride 变化（`EnsureTexCoordSets` 加宽 UV 流）不安全，今天靠每个调用方紧接着整流重传掩盖 | 低 | 已核实 |
| B9 | `SetInstanceSourceGPU` 无条件 `EditMeshSync` 重传基础网格，交接因此永远是一次阻塞——八家 `Handed*` 缓存与「撤源前先清 counter」那道顺序都是为了绕开它 | 低 | 已核实 |
| B10 | 同一段哈希收尾实际有 9 份 + 1 份死代码（既有文档记 6 份） | 低 | 已核实 |

既有文档校核：大问题 1 及深挖 A 补充 1–6 全部正确；中等问题 6 里「Unregister / Reregister 让所有实例组件丢掉实例源、每家再付一次阻塞交接」**有误**（无 `OnUnregister`、实例源是普通成员、回归脚本经 `set_editor_property` 走的正是这条路且断言 0 次 flush）；冗余 15 正确、17 机制正确但计数少了 3 处。

## 发现

### B1. 扩容不清零 + 早退门不看容量身份 ⇒ 交接陈旧 / 垃圾行（严重度：高；已核实）

- 证据：
  - `CSGroundShaperSteps.cpp:11-40` `CSShaperSteps_GrowTo`：三条 `AllocatePooledBuffer(...)`（行 22-28）之后没有任何 `AddClearUAVPass`，`Counter` 与 `CustomData` 同样不清。`RenderGraphResourcePool.cpp:87-108` `TryFindPooledBuffer` 把 `GetRefCount() == 1` 的同尺寸 buffer 直接复用 ⇒ 新 buffer 里是上一位租客（常常就是本家上一代）的字节。
  - Frame 早退门 `CSHouseActor.cpp:2171-2173`：`bBuffersReady = FrameGpuBuffers.Num() == 1 && FrameHandedCapacities.Num() == 1; ... if (NewHash == FrameDescHash && CurrentFrameBrickCount == BrickCount && (BrickCount == 0 || bBuffersReady)) return;` —— 只判「交接过」，不判「交接的是当前容量」。同型：Vine `3068`（`VineHandedCapacities.Num() == Palette_Num`）、Decor `4138`、SkirtDecor `CSGroundActor.cpp:1774`。**唯一**做对的是 RoofTile `3481-3482`：`RoofTileHandedCapacity == RoofTileGpuBuffers[0].Capacity`。
  - `EnsureFrameComponent` 在 `RebuildFrame` 开头就跑（`2119`），`ReserveCapacity` 扩容（`1996`）→ `bNeedHandover`（容量变，`2018-2019`）→ `SetInstanceSourceGPU` 把新 buffer 交给组件 → 回到 `RebuildFrame` 的早退门：哈希未变（`FrameReserveCapacity` 不进哈希，`2109-2113`）⇒ `return`，`Scatter` 不跑。
  - 剔除 kernel 读的是 `min(SrcInstanceCount[0], MaxSourceInstances)`（`CSGpuInstancedMesh.usf:195`），越界安全，但行内容照画。
  - Pillar 把从未被 kernel 写过的 `CustomData` 当实例源交出去：`CSHouseActor.cpp:1665` `Source.CustomData = PillarGpuBuffers[0].CustomData;` 而 `CSHousePillar.usf` 没有 `RWCustomData` 参数 ⇒ `bHasCustomData = 1`（`CSGpuInstancedMeshSceneProxy.cpp:588`）且 `InstanceCullCS` 把未初始化字节抄进 `VisibleCustomData`（`usf:237-244`）。今天柱材质不读 custom data，所以无症状。
  - 对照：`CSGroundStairs::EnsureBuffers`（`CSGroundStairs.cpp:93-138`）与 `CSGroundCover::EnsureBuffers`（`CSGroundCover.cpp:127-151`）同样不清，但 `RebuildStairs` / `RebuildGroundCover` 紧接着无条件 `Scan` / `Scatter`（含 counter 清零），命令 FIFO 保证代理建起来之前已被覆盖——**同一种分配写法，一边安全一边不安全，取决于调用方是否早退**。
- 后果 / 触发：房子有砖时把 `FrameReserveCapacity` 调大（或砖层开着拖尺寸越过 64 对齐台阶）⇒ 画面上是上一代砖的幽灵或纯垃圾行，`GetFrameBrickCount` / 三角数 / 零阻塞全绿；`DebugReadFrameBrickCountGpuSync` 读到的是池子残值（被 `min(Capacity)` 钳过）。
- 建议：`GrowTo` 至少 `AddClearUAVPass(Counter, 0)`（`ZeroCounters` 的图可以直接复用）；早退门统一加 `Handed*Capacities[i] == Buffers[i].Capacity`（RoofTile 那一行）；更根本的是让 `ReserveCapacity` 返回「是否真的换了 buffer」并由调用方据此强制重打包——把「换 buffer 就必须重写」变成类型上的事实，而不是每家自己记。

### B2. 异步编辑在渲染线程改写游戏线程读的字段，没有栅栏（严重度：高；已核实）

- 证据：
  - 契约声明 `CSMesh.h:87-94`（`Sections`）、`115-119`（`KnownVertexCount`）：「Written on the render thread inside an edit, read on the game thread **after that edit's flush**」；`CSMesh.h:323-327` 只对 `BorrowedGraph` 说「nothing fences a render-thread write of them」。
  - 但 `OwnedGraphAsync` 同样在渲染线程写它们：`CSMeshOps.cpp:518-523` `InvalidateSections` → `Context.Resident.Sections.Reset()`；`1216-1217` `AddCopyFromSnapshotPasses` → `AddSetCountersPass` + `Context.Resident.WorldBounds = Payload.WorldBounds`；`1610` `AddMaterialSectionPasses` → `InvalidateSections`。这两条正是房体 / 柱的 `EditMeshAsync` 里录的（`CSHouseActor.cpp:1530-1535`、`1771-1772`）。
  - 守卫只拦 `BorrowedGraph`：`CSMesh.cpp:513-520` `CSMesh_CanPublishCounts` `if (Context.GetKind() != EKind::BorrowedGraph) return true;`。`Sections` / `WorldBounds` 连这道守卫都没有。
  - 游戏线程并发读者：`CSMeshRenderComponent.cpp:189-221` `ResolveBatchMaterials` 先读 `Resident->Sections.Num()` 再按下标取 `Resident->Sections[BatchIndex]`（`GetUsedMaterials` 随编辑器任意时刻调用，`168-176`）；`225-229` `CalcBounds` 读 `Resident->WorldBounds`（48 字节 FBox，非原子）；`833-835` `HasGeneratedGeometry` → `IsEmpty()` 读 `KnownVertexCount`。
  - 异步完成回调只在 `AsyncTask(GameThread)` 里 `++Generation` 与广播（`CSMesh.cpp:1101-1112`），并不把这三样从渲染线程搬回来。
- 后果 / 触发：拖房子期间房体走 `EditMeshAsync`，此时细节面板 / 缩略图 / 材质用量查询调 `GetUsedMaterials`，或任何 `UpdateBounds` 命中 `CalcBounds`：`Sections` 的 `Num` 与 `[i]` 之间被 RT 清零 ⇒ 开发版 `check`、发布版读到旧内存；`WorldBounds` 撕裂读 ⇒ 一帧错误包围盒（剔除闪一下）。窗口小、难复现，但它是设计上承认的不变量被自己的第二条路破坏。
- 建议：`OwnedGraphAsync` 的 context 把三样写进自己的暂存，完成回调（已经在游戏线程）再发布——计数已有 `SetKnownCounts` 的入口，`Sections` / `WorldBounds` 照它做；或者把 `CSMesh_CanPublishCounts` 扩到三样并对 `OwnedGraphAsync` 也拒绝，让写法只剩一种。

### B3. 布局契约靠两侧各写一份字面量，两处注释已与代码不一致（严重度：中；已核实）

- 证据（「同一个数的两份」）：
  - 行 stride `5u`：8 个 usf（`CSGpuInstancedMesh.usf:158,206,227`、`CSHouseFrame.usf:252`、`CSHouseVine.usf:99`、`CSHouseTile.usf:78`、`CSHouseDecor.usf:91`、`CSHousePillar.usf:62`、`CSGroundStairs.usf:385,438`、`CSGroundCover.usf:243`）+ C++ 8 处（`CSGroundShaperSteps.cpp:23`、`CSGroundStairs.cpp:116,124,302`、`CSGroundCover.cpp:140,320`、`CSGpuInstancedMeshComponent.cpp:606,882,899`、`CSGpuInstancedMeshSceneProxy.cpp:357`）。没有共享常量；`CSGpuInstancedMeshComponent.h:88-91` 明说「那个 `* 5u` 的步长散在两个 .usf 与四处 CPU 路径里，动它的代价与风险都不对等」——实际已散到 16 处。
  - `CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS`：`CSGpuInstancedMeshComponent.h:24` 与 `CSGpuInstancedMesh.usf:23` 各 `#define 2`；`CSHouseVine.usf:65-66` 直接写 `Index * 2u`。`CS_GPU_INSTANCED_MAX_LODS`（`.h:14`）↔ `CS_INSTANCED_MAX_LODS`（`usf:20`）。
  - Frame：`CSHouseFrame.cpp:19` `CSHouseFrame_PathStride = 7` ↔ `CSHouseFrame.usf:42` `FRAME_PATH_STRIDE 7`；位标志 `cpp:22-26` ↔ `usf:44-48`；`R4` 的整数字段靠 `reinterpret_cast`（`cpp:82-85`）↔ `asuint`（`usf:155,180,187`）。**注释不一致**：`usf:17` 「逐路常量 = 6 个 float4」、`usf:50` 「6 个 float4 / 砖路」，实际 7（`R6` 在 `usf:175,233-234,259`）。
  - Vine：`CSHouseVine.h:131` 「一条实例记录 = 3 个 float4」、`CSHouseVine.usf:13` 「记录 = 3 个 float4」，实际 4（`cpp:57-66` 平铺 4 行，`usf:33` 声明 4，`usf:56` `Index * 4u`）。
  - BGRA 字节序有 6 份独立实现：`CSMeshOps.usf:109-112` `PackColorBGRA`；`CSGroundStairs.usf:136` / `CSGroundCover.usf:113-121` / `CSGroundRockShell.usf:140` 各自 `>> 16u` 取 R；`CSMesh.cpp:843-848` 回读解包；`CSGpuInstancedMeshComponent.cpp:76-79,567-574` `ClearPackedColorAlpha` / `UnpackColorARGB`。
  - `CSGroundShaperField.h:32` `Float4sPerShaper = 3` ↔ `.ush:156-158,205-209,226-230,243-247` 四处 `i * 3u` 字面量（`.ush:19` 注释承认「步长写死…两边必须同步」）。
- 后果：任何一侧改数都不报错，只错位；两处注释已经先漂了，说明「注释即契约」这条纪律在这一层已失效一次。
- 建议：一份只含 `#define` 的 `CSGpuInstancedLayout.ush`，C++ 侧以普通头 `#include` 同一文件（预处理器常量两边都认）；stride / 步长 / 位标志 / 颜色字节序都从它取，注释里不再写数字。

### B4. 原子槽位的不确定性在三处泄漏到产物（严重度：中；已核实）

- (a) 剔除压缩：`CSGpuInstancedMesh.usf:223-227` `InterlockedAdd(RWLodCounters[Lod], 1u, Slot); Dst = Lod * MaxInstancesPerLod + Slot` ⇒ 可见槽位次序逐帧变。身份是安全的（`PerInstanceRandom` 与 custom data 都按源下标抄进 `Dst`，`usf:231,238-243`），但**绘制次序**不是：门框 / 接缝 / 柱砖靠 `FrameBrickBloat ≥ 1` 互穿（`CSHouseActor.cpp:1984-1987`），相邻砖的顶面 / 墙法线面共面重叠，深度相等处谁赢由绘制序决定 ⇒ 重叠带内逐帧闪烁。**推测**（机制已核实、画面未跑）。
- (b) 超容量截断不确定：`CSGroundStairs.usf:372-374` / `425-426`、`CSGroundCover.usf:234-236` 先 `InterlockedAdd` 后判 `Slot >= Max` 静默丢弃 ⇒ 容量不够时**丢哪一格**由线程组完成顺序决定，「格身份决定一切」的纪律在这个制度下失效；且诊断口径把 counter 钳到容量（`CSGroundStairs.cpp:314-315`、`CSGroundCover.cpp:332-333`、`CSGpuInstancedMeshComponent.cpp:436-437`），超容量本身对所有断言不可见，也没有任何日志。
- (c) 材质排序：`CSMeshOps.usf:483-486` 自述「scatter order inside a run is not reproducible between runs」，`usf:547-548` `InterlockedAdd(RW_SlotCursors[Slot], 1u, DstTri)`。它改写的是常驻索引流（`usf:553-560`），而 `ReadbackMeshSync` → `SaveToStaticMesh`（`CSMeshRenderComponent.cpp:129-152`）原样带出 ⇒ 同一栋房体两次烘焙的三角序不同，资产字节不可复现（项目终局是烘 StaticMesh）。
- 建议：(c) 用「按槽位的前缀和 + 源三角序」做稳定散射，或落盘前按源三角序排一遍；(b) 超容量至少打一条日志，`DebugRead*` 把「counter > 容量」作为独立返回值；(a) 只能靠几何避免共面（互穿改成微小偏移）或接受。

### B5. 包围盒 / 变换口径（严重度：中；已核实）

- 行本身与变换口径无关：所有打包 kernel 用 `WorldToComponent`（组件完整逆变换，`GetComponentTransform().ToInverseMatrixWithScale()`，如 `CSHouseActor.cpp:2153-2155`）把世界记录转进组件空间，绘制再乘组件 LocalToWorld ⇒ 世界位置精确往返，房子带 pitch / roll / scale 时实例仍落在记录的世界位置上。
- 交接盒不是：只有 Frame 做了 `BuildToComponent` 映射（`CSHouseActor.cpp:2013-2014`），Pillar `1649`、Vine `2926-2927`、RoofTile `3415`、Decor `4031` 直接把构建空间（yaw-only）盒当组件空间盒 ⇒ 有 pitch / roll / scale 时剔除盒偏、视锥边缘闪。
- `WorldToComponent` 在打包时烘进行里，过期条件 = 组件变换变而不重打包：房子五家的哈希都含摆位（`ComputePlacementHash()` 或 `CSHouse_HashElementFrames`，`2158`），地面走 `MeshBuiltAtLocation` 触发全量重建（`CSGroundActor.cpp:706-709`）；点云源每帧现算（`CSGpuInstancedMeshSceneProxy.cpp:545`）。这一条今天没有缺口。
- `BaseSphereCentre / Radius / BlockSize` 烘进行里：Frame 每次 `Ensure` 重算（`1973-1988`），但网格身份不进哈希（既有 F2）；Vine 只在 `!bVineBaseMeshReady` 时算 `BlockSize`（`2854-2896`），`VineThickness` 进哈希却改不动它（既有 V5）。石阶 / 地被的盒吃 `MaxAbsHeight` 且不过 `QuantizeUp`（`CSGroundActor.cpp:925-928`、`2152-2155`）⇒ 拖 `LiftHeight` 每帧交接（回归脚本 `1319-1324` 只记录、未处理）。剔除球用「最大轴缩放 × 半径」保守放大，各 kernel 一致。
- 建议：交接盒统一经 `BuildToComponent` 映射；`MaxAbsHeight` 走 `QuantizeUp`；网格身份（指针 + 包围盒量化）进各家哈希。

### B6. 阻塞计数：模块内完备，盲区在引擎调用（严重度：中；已核实）

- 模块内 31 处阻塞点全部经 `UCSMesh::CountedBlockingFlush`（见「阻塞点计数表」），`SubmitAndBlockUntilGPUIdle` 都包在一次计数 flush 里。
- 盲区一：`UCSGpuInstancedMeshComponent::CreateSceneProxy` 调 `InstanceMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes)`（`CSGpuInstancedMeshComponent.cpp:1226`）。引擎 `Material.cpp:1943-1952` 在游戏线程转 `CheckMaterialUsage` → `SetMaterialUsage`，后者对缺 usage 的材质建 `FMaterialUpdateContext(SyncWithRenderingThread)`（`2058-2062`）并 `CacheResourceShadersForRendering(true)`（`2069`）；`MaterialShared.cpp:4824-4826` / `4876-4878` 构造与析构各一次裸 `FlushRenderingCommands()`。只发生在该材质第一次被实例路建代理时，此后 usage 存进资产——但正是「换了一张没勾 flag 的材质拖一下」那一帧，计数器看不见。
- 盲区二（**推测**）：`SaveToStaticMesh` → `UCSGpuMeshComponent::BuildStaticMesh` → `UStaticMesh` 构建（引擎内部 flush），离线路径，仅影响「烘焙期计数」。
- 计数语义：counter 数 flush 次数，`DebugReadInstancesSync` 一次计数里含一次完整 GPU idle（`CSGroundStairs.cpp:311`），与一次纯命令 flush同权重。诊断用途够，做性能预算不够。
- 回归覆盖：13 处 `flushes=0` 覆盖平移 / 落笔 / 拉尺寸（砖 / 柱 / 藤 / 摆件 / 窗 / 接缝）/ 石阶 / 岩壳 / 裙边 / push_edge；**未覆盖**：砖层开着拖尺寸（既有文档 2）、`LiftHeight` 拖动（脚本自述 12 次基线）、Undo、销毁、地被。

### B7. 「交回渲染线程释放」是错误模型；真实风险是复用与 GC 滞留（严重度：中；已核实）

- 引擎事实：`RenderGraphResources.h:1194` `class FRDGPooledBuffer final : public FRefCountBase`，`RefCounting.h:212` `FRefCountBase : TTransactionalAtomicRefCount<uint32>`（原子）；`RenderGraphResourcePool.cpp:195` `AllocatedBuffers.Add(PooledBuffer)` 池子持一份，`226` `bIsUnused = Buffer.GetRefCount() == 1` 且 `kFramesUntilRelease = 30` 才真释放（`214`）。⇒ 游戏线程 `Reset()` 只减计数，析构永远在渲染线程的 `TickPoolElements` 里。
- 代理持自己的引用：`CSGpuMeshSceneProxy.cpp:231-235` 逐流拷贝 `Pooled`；`CSGpuInstancedMeshSceneProxy.h:133-134` 按值持 `GpuSource` / `GpuPointSource`；`RunCulling` 用 `RegisterExternalBuffer` 再加引用（`.cpp:498-517`）。两种拆卸顺序都安全。
- 纪律不自洽：`CSGroundShaperSteps.cpp:38` `Palettes = MoveTemp(Work)` 在游戏线程释放旧 palette；`CSGroundActor.cpp:1994-1997` 注释「`SetNum` 缩短会在游戏线程上直接析构 `TRefCountPtr`，把在途帧正在读的 buffer 抽走」——按引擎代码这不会发生。三份 `ReleaseOnRenderThread`（`CSGroundShaperSteps.cpp:87-103`、`CSGroundStairs.cpp:258-273`、`CSGroundCover.cpp:283-297`）与 `UCSMesh::BeginDestroy`（`CSMesh.cpp:906-922`）在保护一个不存在的竞争。
- 真实后果：(1) 复用——见 B1；(2) 滞留——`UCSGpuInstancedMeshComponent` 没有 `BeginDestroy` / `OnComponentDestroyed`（grep 无覆写），`DecorComponents.Pop()` 直接 `DestroyComponent`（`CSHouseActor.cpp:3936-3937`），房子四家在 `Destroyed / EndPlay / BeginDestroy` 一处不释放（`4474-4499`）⇒ 被销毁组件的 `InstancedGpuMesh`（可见槽区 = 容量 × LOD 数 × 80 B）与 `GpuInstanceSource` 引用拖到 GC 才回池。
- 建议：删掉或改写那些注释（它们会把下一个人引向错误的修法）；`UCSGpuInstancedMeshComponent::OnComponentDestroyed` 里 `ClearInstanceSourceGPU + ReleaseGpuMesh`（`ReleaseGpuMesh` 是 `ReleaseSync`，一次计数 flush，销毁期可接受）。

### B8. 布局重声明的前缀拷贝对 stride 变化不安全（严重度：低；已核实）

- `CSMesh.cpp:219-234` `CSMesh_ReallocateResidentWithDescs` 按 `(Role, TexCoordIndex)` 配对后 `AddCopyBufferPass(Dst, 0, Src, 0, min(OldBytes, NewBytes))`。`EnsureTexCoordSets`（`CSMeshOps.cpp:1158-1194`）把 TexCoord 流从 2 float / 顶点加宽到 4 时，旧交错-1 数据按字节落进交错-2 布局 ⇒ 顶点 1 的 UV0 落到顶点 0 的 UV1 上。
- 今天无症状：`SubmitBodyMesh` `1492` 之后 `1533` 立即整流重传；`BuildTubeIntoMesh`（`GeometryEditorActor.cpp` 4700+ 区段 grep）也是先 `EnsureTexCoordSets` 再异步上传。`CSMesh.h:621-622` 说「copies every stream that survives the change」，应改为「同 stride 才拷，stride 变则清零」。

### B9. 交接永远是一次阻塞，是八家 `Handed*` 与撤源顺序的根因（严重度：低；已核实）

- `SetInstanceSourceGPU`（`CSGpuInstancedMeshComponent.cpp:379-389`）→ `RebuildGpuMesh`：`SetStreamLayoutSync` 同布局早退、`ResizeStreamsSync` 同尺寸早退、`EnsureCapacitySync` 早退，但 `EditMeshSync`（`1082-1142`）**无条件**重传基础网格 + 清 args ⇒ 稳态交接 = 1 次计数 flush；`ClearInstanceSourceGPU` → `ReleaseGpuMesh` → `ReleaseSync` 又 1 次。
- 因此 `RebuildFrame` 的空表分支要先 `Scatter` 清 counter 再撤源（`2180-2202`），`bNeedHandover` 要五条件判（`2018-2025`），`RebuildFrame` 的哈希要特判「全 0 ⇒ 0」免得每轮付 2 次（`2160-2169`）。
- 建议：给基础网格快照一个 generation，`RebuildGpuMesh` 在快照未变且流未重分配时跳过上传（只换 `GpuInstanceSource` 成员 + `RecreateRenderState_Concurrent`）；交接变成零阻塞后，`Handed*` 缓存可以直接比 `GetInstanceSourceGPU()` 的指针（既有文档 A3 的方案）。

### B10. 哈希收尾 9 份 + 1 份死代码（严重度：低；已核实）

- 同一段 `H = ((H >> ((H >> 28) + 4)) ^ H) * 277803737; (H >> 22 ^ H) & 0xFFFFFF / 2^24`：CPU `CSGpuInstancedMeshComponent.cpp:59-65` `InstanceRandom`、`CSHouseVine.cpp:72-76`、`CSHouseDecor.cpp:87-91`；GPU `CSGpuInstancedMesh.usf:126-132`、`CSHouseFrame.usf:64-69` `FrameInstanceRandom`、`CSGroundStairs.usf:88-92`、`CSGroundCover.usf:89-93`、`CSGroundRockShell.usf:83-87`；测试 `CSGroundStairsTests.cpp:82`；死代码 `CSGroundStairs.usf:102-105` `StairInstanceRandom`（注释自述无调用方）。`CSGroundShaperField.h:49` 同名不同算法。CPU ↔ GPU 只有 `CSGroundShaperField` 有 `CpuGpuFieldParity`。

## 九条路的布局对照表

约定：packed 行 = 5 × float4 / 实例，`[0..2]` 实例→组件 3×3 的**行**（`.w` 必须 0，引擎当 hitproxy/selected），`[3]` 组件空间原点 + `.w = PerInstanceRandom`，`[4]` 剔除球心 + 半径。剔除消费者 `CSGpuInstancedMesh.usf:206-232`；CustomData 按 `Index * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS` 由源下标抄到压缩后下标（`238-243`）。

| 路 | CPU 记录（字段序 / 字节） | 上传元素 | kernel 下标 | 行的写法（基 / 缩放） | `.w` 随机 | CustomData 写者 | counter 语义 | 对不上 / 魔数处 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Frame `CSHouseFrame.cpp:61-93` ↔ `.usf:137-275` | `FElement`：`FPath`(8 float + enum + 3 bool) + `FWallFrame`(4×float3) + BrickBegin/Count(int) + Pitch/HalfLen/LayoutScale + RandomBase(uint) + CrossScale + CullBelowZ + ShearAtS0/S1 → 7×float4 / **路**（整数 `reinterpret_cast` ↔ `asuint`） | `StructuredBuffer<float4> FramePaths` | 线程 = 砖号；线性扫 `FramePaths[K*7+4]` 找路（`153-165`） | X = 面内朝外·Depth·CrossScale，Y = −切向·Length·Bloat·LayoutScale，Z = X×Y·Thickness·CrossScale；端头砖剪切改 X 与原点 | `FrameInstanceRandom(RandomBase + i)`；`CullBelowZ` 命中写 −1 哨兵 | 无（Source 不传，`2028-2032`）；GrowTo 仍分配 | 线程 0 写 `min(BrickTotal, Max)` = 活跃数 | stride 7 两份；位标志两份；**注释说 6 行**（`usf:17,50`） |
| Vine `CSHouseVine.cpp:55-67` ↔ `.usf:46-113` | `FRecord`：WorldPos(3)+LengthScale, Dir(3)+Random01, Normal(3)+SizeScale, SpawnTime, ArcLength (+StrandIndex 不上传) → 4×float4 | `StructuredBuffer<float4> VineRecords` | `Index * 4u` | X = N×Dir，Y = Dir×X（= N），Z = Dir；xy·SizeScale，z·LengthScale；`BlockSize` = 截面/尺寸, 1/长度 | 记录自带 Random01 | kernel `usf:65-66` 在退化 return 之前写 [SpawnTime, ArcLength] | 线程 0 写 `min(Count, Max)`；退化记录仍占槽（行不写 ⇒ 池残值行被画）※ | `.h:131` / `usf:13` 「3 个 float4」实际 4；`Index * 2u` 硬编 |
| Tile `CSHouseTile.cpp:56-66` ↔ `.usf:45-92` | `FRecord`：WorldPos+Random01, AxisX+SizeX, AxisY+SizeY, AxisZ+SizeZ → 4×float4 | 同上 | `Index * 4u` | 三轴直接给（右手性 CPU 保证），`BlockSize` = 1/原生尺寸把 cm 变倍率 | 记录自带 | 无 | 线程 0 写 `min` | 无 |
| Decor / GroundDecor `CSHouseDecor.cpp:51-60` ↔ `.usf:46-105`（裙边共用） | `FRecord`：WorldPos+Scale, Facing+Random01, Up+ScaleZ → 3×float4 | 同上 | `Index * 3u` | Z = Up，X = Facing ⊥ Z，Y = Z×X；xy·Scale，z·Scale·ScaleZ；BlockSize (1,1,1) | 记录自带 | 无 | 线程 0 写 `min` | 无 |
| Pillar `CSHousePillar.cpp:121-129` ↔ `.usf:39-76` | `FBrick`：Origin+Random01, AxisX+0, AxisY+0, AxisZ+0 → 4×float4（三格 `.w` 留白） | 同上 | `Index * 4u` | 轴已含尺寸，只做世界→组件，再乘 `BlockSize`（1/网格尺寸） | 记录自带 | **无写者但被交接**（`CSHouseActor.cpp:1665`）⇒ 剔除抄未初始化字节 | 线程 0 写 `min` | B1 |
| Stairs `CSGroundStairs.cpp` ↔ `.usf:203-453` | 无记录：一线程一格，marching squares | `GroundShaperParams`（3×float4/座）+ 地面色流 SRV | `Slot * 5u`（`InterlockedAdd`） | X = 最陡（偏航抖）、Y = 等值线·弦长·Bloat·(1+J)、Z = 世界上；`Rise·ZFactor` | `CSStairs_CellSeed(cell, L, s, 71)` 格身份 | 无 | **槽位分配器**，可超容量；越界静默丢 | 石子第二套 buffer，`PSlot * 5u`；`i*3u` 场步长 |
| Cover `CSGroundCover.cpp` ↔ `.usf:160-259` | 无记录：一线程一格抖动网格 | 同上 | `Slot * 5u` | Z = lerp(世界上, 法线)，偏航 / 倾倒由格身份 | `CSCover_CellSeed(cell, 41)` | 无 | 槽位分配器，可超容量 | 同上 |
| RockShell `CSGroundRockShell.cpp:565-716` ↔ `.usf` | 非实例路：`FPattern` 三角汤 + aux 32..37（RestDir float4/顶点、CellFlags uint、Centroids Fixed、IncidentRange 2/顶点、IncidentTris Fixed、Neighbours 3/三角 int） | `UCSMesh` 常驻流 | 三趟：逐三角 / 逐角 / 逐三角 | 原地改 Positions / Tangents / TexCoords(UV1..6，stride 14 硬编 `usf:609,647-663`) | 无 | 无 | `MeshCounters` 由 `CopyFromMeshSnapshot` 定死；NaN 关三角 | `RockShellTexCoordStride < 14u` 门是唯一自检；CellFlags 位布局两份（`.h:35-40` ↔ `usf:124-127`） |
| ShaperField `.h:32-142` ↔ `.ush:24-161` | 每座 3×float4：Profile / Top / Noise | `StructuredBuffer<float4>` | `i * 3u` 字面量（4 处） | — | — | — | — | `Float4sPerShaper` 只有 CPU 侧有名字；`CpuGpuFieldParity` 守着公式 |

※ Vine / Tile / Decor / Pillar 的退化记录（`usf` 里各自的 `return`）不写行但 counter 已按记录数写死 ⇒ 那一槽是池残值行（B1 同族，概率低）。

## 阻塞点计数表

| 阻塞点 | 文件:行 | 经过 `CountedBlockingFlush`？ | 备注 |
| --- | --- | --- | --- |
| `UCSMesh::EditMeshSync` | `CSMesh.cpp:1029-1037` | 是（1） | 所有同步算子的底 |
| `UCSMesh::AllocateSync` / `EnsureCapacitySync` 扩容 | `1168-1180` / `1210-1215` | 是（各 1） | 同容量早退无 flush |
| `UCSMesh::ShrinkCapacitySync` | `1269-1274`（+`GetCountsSync` `1234`） | 是（1，未知计数时 +2） | |
| `UCSMesh::SetStreamLayoutSync` / `ResizeStreamsSync` / `EnsureIndirectDrawCapacitySync` | `1323-1328` / `1423-1428` / `1477-1482` | 是（各 1，仅真重分配时） | 未分配时纯簿记 |
| `UCSMesh::ReleaseSync` | `955-957` | 是（1） | |
| `CSMeshReadback::ReadUintBufferSync` / `ReadCountersSync` / `ReadbackResidentSync` | `575-606` / `632-662` / `732-883` | 是（2 / 2 / 2+2） | 内含 `SubmitAndBlockUntilGPUIdle` |
| `CSShaperSteps_GrowTo` | `CSGroundShaperSteps.cpp:36` | 是（1） | 注释记录了曾经裸调 |
| `CSGroundStairs::EnsureBuffers` / `DebugReadInstancesSync` | `CSGroundStairs.cpp:134` / `329` | 是 / 是 | 后者含 GPU idle（`311`） |
| `CSGroundCover::EnsureBuffers` / `DebugReadInstancesSync` | `CSGroundCover.cpp:147` / `346` | 是 / 是 | 同上（`329`） |
| `UCSGpuInstancedMeshComponent::SetInstanceSourceGPU` / `ClearInstanceSourceGPU` | `.cpp:379-400` → `RebuildGpuMesh 1082` / `ReleaseGpuMesh 946` | 是（≥1 / 1） | 经 `EditMeshSync` / `ReleaseSync` |
| `DebugReadDrawnInstanceCountSync` / `DebugGetDrawnAssetMismatchSync` / `ReadLiveInstanceRowsSync` / `SaveToStaticMesh` | `429` / `455,529` / `602,610` / `647` | 是（2 / 2+1 / 4 / 4+引擎） | 材质探针那次是直接 `CountedBlockingFlush()`（`529`） |
| `UCSGpuMeshComponent::ReadbackMeshSync` | `CSGpuMeshComponent.cpp:52,66` | 是（2 + 4） | |
| `UCSMeshOps::ComputeWorldBoundsSync` | `CSMeshOps.cpp:1755,1815,1858` | 是（3） | 唯一在 `EditMeshSync` 外自己消费 readback 的算子 |
| `CSRockShell::BuildMesh` | `CSGroundRockShell.cpp:627,679,686` | 是（经 `SetStreamLayoutSync` / `CopyFromMeshSnapshot` / `EditMeshSync`，≥3） | 只在图案 / 矩形 / 缩放变时走（`CSGroundActor.cpp:1215-1222`） |
| `CSRockShell::Displace` / `CSGroundStairs::Scan` / `CSGroundCover::Scatter` / 五家 `Pack` / `Scatter` / `ZeroCounters` / `ReleaseOnRenderThread` | — | **不阻塞**（ENQUEUE 后直接返回） | 交互热路径 |
| `FCSGpuInstancedMeshSceneProxy::RunCulling` 诊断 `Lock` | `CSGpuInstancedMeshSceneProxy.cpp:631-634` | 不阻塞（只在 `IsReady()` 时 Lock） | |
| `CheckMaterialUsage_Concurrent` → `SetMaterialUsage` | `CSGpuInstancedMeshComponent.cpp:1226` → `Material.cpp:2058-2069` → `MaterialShared.cpp:4826,4878` | **否**（引擎裸 flush ×2 + 编译） | 缺 usage 的材质首次建代理；B6 |
| `UStaticMesh` 构建（`SaveToStaticMesh` 尾部） | `CSGpuMeshComponent.cpp:342-343, 393-394` | **否**（推测，引擎内部） | 离线 |
| 测试里的裸 `FlushRenderingCommands` | `CSMeshBooleanParityTests.cpp:137,158,185,205,630` 等 | 否 | 不在回归测量窗口内 |

## 生命周期与线程（问题 2 的路径图）

```
ReserveCapacity(GT) ─┬─ GrowTo: ENQUEUE(alloc on RT) → CountedBlockingFlush → Palettes = MoveTemp(Work)  [旧引用在 GT 释放]
                     └─ 够大：零 enqueue
Ensure*Component(GT) → SetInstanceSourceGPU：组件成员 GpuInstanceSource 拷一份引用 → RebuildGpuMesh(EditMeshSync, 1 flush) → RecreateRenderState_Concurrent
CreateSceneProxy(GT) → 代理 ctor 按值拷 GpuSource + FCSMeshResidentRef → RT：InitGpuGeometry 逐流拷 Pooled；OnStreamsAllocated 记 AllocationGeneration
每帧 RT：PreRenderView → RunCulling：RegisterExternalBuffer(代理自己的引用)；Generation 不符即早退
撤源(GT)：ZeroCounters(ENQUEUE) → ClearInstanceSourceGPU(ReleaseSync, 1 flush) → 组件引用归零；代理引用随 DestroyRenderThreadResources 在 RT 归零
宿主销毁：地面 EndPlay/Destroyed 撤源 + ReleaseOnRenderThread(MoveTemp 进 RT lambda)；房子四家不释放 → 引用随 UObject GC 在 GT 归零
UCSMesh::BeginDestroy：MoveTemp(Resident) 进 RT lambda → ReleaseBuffers（代理仍持自己的流引用，不受影响）
EditMeshAsync：RT lambda 持 ResidentRef + WeakThis；执行后 AsyncTask(GT) 才清 bAsyncEditInFlight
```

- 按值捕获的 `TRefCountPtr<FRDGPooledBuffer>` 在渲染命令 lambda 析构时（渲染线程）释放；`AsyncTask` 里的在游戏线程释放。两者都只是减计数（B7）。
- `IsEditInFlight` 只覆盖 `EditMeshAsync`；其余写路径全是同步且命令 FIFO 排在在途异步之后，正确但会阻塞到两者都完成——调用方里 `SubmitBodyMesh`（`1495`）、`FlushPaintToGpu`（`256`）、`ApplyBodyPlacement`（`1587`）有判，`RefreshHeightsInRegion → DisplaceGroundShapers` 没判（一律付一次计数 flush，回归自述 12/12）。
- `RF_Transient` 网格：`InstancedGpuMesh` / `TinyGladeMesh` / `PillarMesh` / `VineTubeMesh` / `RockShellMesh` 都是 `UPROPERTY(Transient)`（`CSGpuInstancedMeshComponent.h:515-516`、`CSTinyGlade.h:72-73`、`CSHouseActor.h:2158-2159,2182-2183`、`CSGroundActor.h:1506-1507`），GC 可达；`PillarMesh = nullptr` / `VineTubeMesh = nullptr`（`CSHouseActor.cpp:1682,1717,2985`）之前都先 `SetGpuMesh(nullptr)`，在途异步靠 `ResidentRef` 活到执行完。**没有发现**渲染线程仍在读时游戏线程释放的路径；引用泄漏到 GC 的路径见 B7。

## 容量契约（问题 4）

| kernel | 记录数 > Capacity 时 | counter 可能 > Capacity？ | 下游读越界？ | 截断是否出声 |
| --- | --- | --- | --- | --- |
| `ScatterHouseFrameCS` | `Brick >= FrameMaxInstances return`（`usf:146`），CPU 已按 `MaxBricks` 截 | 否（`min`） | 否 | CPU 侧 Warning（`CSHouseActor.cpp:2085-2090, 2142-2148`） |
| `PackHouseVine/Tile/Decor/PillarCS` | `Index >= Max return` | 否（`min`） | 否 | 静默；CPU 计数不截（`3071-3073`、`4147`）⇒ 「GPU 计数 == CPU 记录」断言会红 |
| `ScanGroundStairsCS` / `ScatterGroundCoverCS` | 先加后判静默丢 | **是** | 否（剔除 / 回读都 `min`） | 静默，且 B4(b) 不确定 |
| `InstanceCullCS` | `Slot >= MaxInstancesPerLod return`（`usf:225`） | 是（LodCounters；`BuildArgsCS` 再 `min`，`264`） | 否 | — |
| `PackPointInstancesCS` | `Instance >= MaxSourceInstances return` | — | 否 | — |
| 材质排序 | `DstTri >= TriangleCount return`（`CSMeshOps.usf:552`） | 否 | 否 | — |

- 「新 buffer 带旧内容 / 未初始化」的路径：B1（GrowTo 三条）、`CSMesh_ReallocateResidentWithDescs` 里新增流不清（`CSMesh.cpp:214-217` 只清 IndirectArgs，头注释 `.h:621-622` 承认）、B8 的 stride 错位、退化记录不写行（表※）。`AllocateSync` 清 counter + args（`1172-1178`）、`ResizeStreams` 清零（`289-294`）、`RebuildGpuMesh` 清 args + MaterialIds（`1111,1118`）是做对的三处。
- `ReserveCount` 台阶：Pillar `1641-1642` ✓、Vine `2910-2911` ✓、RoofTile `3400` ✓、Decor `4015` ✓、SkirtDecor `CSGroundActor.cpp:1636-1637` ✓、房体 / 柱网格 `1517,1763` ✓；**Frame 漏**（`1996` 直接喂 `EffectiveFrameCapacity()`，砖层开着时 `2460` 的预算是 footprint 连续函数）——既有文档 A2 正确。石阶容量是配置量（`863`），地被容量 = 格数（`2095`，随地面尺寸变但不在拖动路上）。

## 确定性（问题 5）

| 路 | 槽位来源 | 随机 / 身份来源 | 两次 dispatch 逐位相同的前提 | 破坏处 |
| --- | --- | --- | --- | --- |
| Frame / Vine / Tile / Decor / Pillar | 线程 = 记录序号 | CPU 身份哈希（Frame 是砖全局序号 = 槽位，但槽位由 CPU 排定，确定） | 记录序确定 + uniform 相同 | Frame 六家的**先后**决定槽位（`RebuildFrame 2123-2138` 注释承认开一扇门会推走后面的槽位 ⇒ 门框砖随机数换色）；Vine 的 `EscapeRoot` 侧移改 S 但身份不含位置 ✓ |
| Stairs / Cover | `InterlockedAdd` | 格身份（`CSStairs_CellSeed` / `CSCover_CellSeed`） | 容量够 | 超容量时丢弃集合随原子序变（B4b） |
| 剔除压缩 | `InterlockedAdd` | 随行携带（`.w` / CustomData 按源下标） | — | 绘制序逐帧变（B4a）；`SV_InstanceID` 不是稳定身份，材质只能读 `PerInstanceRandom` / `PerInstanceCustomData` |
| 材质排序 | `InterlockedAdd` 游标 | — | — | 段内三角序不可复现 ⇒ 烘焙资产字节不可复现（B4c） |
| `InstanceRandom(Index)`（CPU 路 / 点云路） | 插入序 / 点序 | 下标哈希 | 点云 append 顺序稳定 | 点云路的 Index 是点序，重排即换色（点刷是唯一消费者，可接受） |

`CpuGpuFieldParity` 守 `CSGroundShaperField`；HLSL `distance()`/`length()` 与 CPU `Sqrt(Dx*Dx+Dy*Dy)` 在 fma 缩并下可能差最后一位（**推测**，测试应有容差）。

## 既有文档结论校核

| 条目 | 结论 | 证据 |
| --- | --- | --- |
| 大问题 1 主体（八份状态机、共同骨架） | 正确 | 五家 `Ensure*`（`1600-1673, 1952-2038, 2761-2968, 3311-3436, 3909-4069`）与地面四家（`745-982, 1150-1286, 1535-1705, 1948-2188`）逐一读过，骨架同型；行数量级一致、未逐行数 |
| 深挖 A 1（三份 buffer 结构 + 组件第四份；三份 Ensure / Release） | 正确 | `CSGroundShaperSteps.h:114`、`CSGroundStairs.h:25`、`CSGroundCover.h:50`、`CSGpuInstancedMeshComponent.h:82`；`ReleaseOnRenderThread` ×3 |
| 深挖 A 2（门框砖漏 `ReserveCount`） | 正确 | `CSHouseActor.cpp:1996` + `2452-2461`；其余六处都过了台阶 |
| 深挖 A 3（`Handed*` 是组件状态副本） | 正确 | `GetInstanceSourceGPU()` 公开（`.h:415`），`FCSGpuInstanceSourceGPU` 自带 `Capacity` / `LocalBounds` |
| 深挖 A 4（释放不对称；原子引用计数、30 帧滞后；不是竞争是滞留） | 正确 | `RefCounting.h:212`、`RenderGraphResourcePool.cpp:195,214,226`；房子 `4474-4499` 无释放；组件无 `BeginDestroy`。补充：连「交回 RT 释放」本身也不是必要的（B7） |
| 深挖 A 5（构造脚本只销毁 SCS/UCS 组件） | 正确 | `ActorConstruction.cpp:167-170` `IsCreatedByConstructionScript()` |
| 深挖 A 6（Pop 不撤源 / 裙边盒永不收紧 / 石阶地被盒吃 `MaxAbsHeight` / 哈希在 Ensure 前 / `SetBaseMeshFromGpuData` 无早退 / 三份契约不同） | 正确 | `3936-3937`；`CSGroundActor.cpp:1667`；`925-928, 2152-2155`；`1292-1296, 2060-2065`；`CSGpuInstancedMeshComponent.cpp:121-197` 无早退；`CSGroundStairs.h:43-44` / `CSHouseVine.cpp:722` / `CSGroundCover.cpp:161-165` |
| 大问题 2 表内 F4（GrowTo 不拷不清 + 哈希不变早退） | 正确，且范围更大 | B1：counter 与 CustomData 也不清；Vine / Decor / SkirtDecor 早退门同病 |
| 中等问题 6「每次属性改动 = 两次全量重求值」 | 正确 | `ActorEditor.cpp:172-174, 240-244`（Unregister → RerunConstructionScripts → Reregister）；`CSHouseActor.cpp:4456-4471`（`OnConstruction` 与 `PostRegisterAllComponents` 各调一次 `ReevaluateSite`） |
| 中等问题 6「Unregister / Reregister 让所有实例组件丢掉实例源、每家再付一次阻塞交接」 | **有误** | 组件无 `OnUnregister` / `DestroyRenderState` 覆写（grep 无结果）；`GpuInstanceSource` 是非反射普通成员（`.h:506`），注销只拆渲染状态；`bNeedHandover` 以 `HasInstanceSourceGPU()` 兜底（`2025`）故不重交接；回归脚本的 `set_editor_property` 经 `PropertyAccessUtil.cpp:770-786` 发 `PostEditChangeProperty`（Python 默认 `NotifyMode::Default`，`PyWrapperObject.cpp:878`）⇒ 12 帧拉尺寸每帧都走这条注销 / 重注册路，而 `flushes=0` 断言成立且曾抓到并修掉 21 次那次（脚本 `1453-1457`）；若每家真的重交接，每帧至少 ≥1 次 flush |
| 中等问题 6 建议「把细节面板改属性的 flush 纳入断言」 | 已被覆盖 | 同上：`set_editor_property` 就是那条路 |
| 冗余代码 15「诊断回读尾巴 Cover ↔ Stairs 逐字相同」 | 正确（行号漂 ±8） | `CSGroundCover.cpp:348-362` ↔ `CSGroundStairs.cpp:331-345` |
| 冗余代码 15「烘焙出口 / 烘焙自检」 | 正确（宿主层，只核了 `SaveInstancedToStaticMeshes` 两份 `BakeOne`：`CSHouseActor.cpp:4385-4394` ↔ `CSGroundActor.cpp:2246-2258` 同形） | |
| 冗余代码 17「一份数学、六处手抄」 | 机制正确，**计数有误** | 实为 9 份 + 1 份死代码（B10）；文档漏了 `CSHouseFrame.usf:64`、`CSGroundCover.usf:89`、`CSGroundRockShell.usf:83`、`CSGpuInstancedMeshComponent.cpp:59` |
| 冗余代码 17「CPU ↔ GPU 只有 ShaperField 有 parity 测试」 | 正确 | `CpuGpuFieldParity` 唯一；`CSGroundStairsTests` 的格身份复算是另一种形态（`206-300`） |
| 「GPU 基座：好」总评 | 机制层成立；契约层见 B1–B5 | — |

## 未覆盖项

- `CSVineTube::BuildTubeIntoMesh`（`GeometryEditorActor.cpp` 4700+）只 grep 了 Sync/Async 调用，未精读；藤管的容量 / 布局契约未核。
- `CSHelper::CreateUploadedStructuredBuffer` 的上传拷贝语义只读了签名（`ComputeShaderGenerateHelper.h:140-146`，`QueueBufferUpload` 默认拷贝）。
- `UStaticMesh` 构建期的引擎 flush（B6 盲区二）未读引擎源码，标推测。
- 剔除压缩导致互穿砖共面闪烁（B4a）未出图验证，标推测。
- `CSGroundShaperField` CPU/GPU fma 差异标推测；`CpuGpuFieldParity` 的容差未读。
- `CSHouseVineTests` / `CSHouseTileTests` / `CSHouseDecorTests` 只 grep 了确定性与 GPU 计数相关行，未逐条读；它们全是 CPU 判据，对本层无新增证据。
- `CSMeshBooleanParityTests` 与 TG 无关，只确认其裸 `FlushRenderingCommands` 不在回归窗口。
- 回归脚本是否当前能跑到各 `flushes=0` 断言（记忆里门 / 拱段是既存红且提前中止）未验证；本文只以脚本注释里记录的历史实测作为佐证。
- `UCSMeshPool` 只通读，未在本层发现问题（`RequestMesh` 走 `EnsureCapacitySync + Reset`，两次计数 flush）。

## 2026-09-10 复核与修复记录

复核基准：2026-09-10 工作区（含未提交的交接收敛 `HandOverInstanceSources`、网格槽 `ACSTinyGlade` 与 Nanite / GPU-Scene 实例路）。逐条状态与本轮改动：

| 条目 | 复核时现状 | 本轮改动 |
| --- | --- | --- |
| B1 扩容不清零、早退门不看容量 | 交接判据已统一按容量比（`HandOverInstanceSources`），但 `GrowTo` 不清零；门框砖 / 藤 / 摆件 / 裙边摆件的早退门仍只看哈希与数量 | `CSShaperSteps_GrowTo` 三条 buffer 分配即清零；`Ensure*` 返回 `EHandoverResult`，`HandedOver` 时 `Rebuild*` 不许早退（门框砖仅在有砖时强制，免得空房每轮再交再撤）；柱子不再交从未写过的 CustomData |
| B2 异步编辑无栅栏 | 分段表已改在完成回调发布；`WorldBounds` 六处、`KnownCounts`、`InvalidateSections` 仍在渲染线程直写 | `FCSMeshEditContext` 新增 `SetWorldBounds` / `GetWorldBounds` / `InvalidateSections`，与 `SetKnownCounts` 一起：同步通道直写、异步通道暂存到完成回调在游戏线程 `ApplyTo`、借图通道拒绝；全部十二处写点改经它 |
| B3 布局魔数 | 未修且变多（Nanite writer 第三份 CUSTOM_DATA_FLOATS，行 stride 约 24 处） | 新增 `Shaders/Private/CSGpuSharedLayout.ush`，C++ 与 .usf 共 include；行 stride / custom data 步长 / LOD 上限 / 门框砖路 stride 与位标志 / 塑形物场步长全部改从它取 |
| B4b 超容量静默丢 | 未修 | 石阶 / 地被 / 组件的诊断回读在 counter > 容量时打 Warning；截断本身不变 |
| B5 包围盒口径 | 只有门框砖过 BuildToComponent；石阶 / 地被的盒含未量化的 `MaxAbsHeight` | 五家交接盒统一经 `CSHouse_BuildBoundsToComponent`；石阶 / 地被的 `MaxAbsHeight` 过 `QuantizeUp` |
| B7 GC 滞留、错误的释放模型 | actor 级已有 `ReleaseTinyGladeGpu`；组件级不释放；三处 `ReleaseOnRenderThread` 与 `CSMesh.h` 的注释仍传"必须在渲染线程释放" | `OnComponentDestroyed` 撤源并释放常驻网格（构造脚本建的组件只撂下引用，避免改属性路径多出 flush）；点刷三处 `ClearInstances` 改 `ClearInstanceSourceGPU`；注释改写为真实模型 |
| B8 前缀拷贝 | 未修 | `CSMesh_ReallocateResidentWithDescs` 只拷贝逐单元步长没变的流，新增流与加宽的流清零 |
| B9 交接必阻塞 | 经典路未修；GPU-Scene 路上已无 | 未动：随实例化叶子的路线拍板一起定 |
| B10 哈希九份 | 未修 | 未动：与 `CSHash.h / .ush` 对一起做 |
| 点刷点源引用泄漏 | 未修 | 已修（见 B7） |
| L0 回读层 | 从未建 | `GpuTriangleUnified_Plan.md` 标为历史文档 |

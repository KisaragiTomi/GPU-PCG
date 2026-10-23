# 藤蔓 GPU 常驻化：现状与剩余计划

记录 `AVineContainer` 生成链路（表面体素 → 空间竞争 → VisVine 网格）从"CPU 回读 + `UDynamicMesh`"收敛到 GPU 常驻的当前状态，以及还没做的部分。

上游的 GPU 三角形统一化背景见 [`GpuTriangleUnified_Plan.md`](GpuTriangleUnified_Plan.md)。

## 现状

### 独立全局距离场吸附入口（2026-10-09）

新增 Blueprint callable `AVineContainer::GenerateVineGPUWithGlobalDistanceField()`。原 `GenerateVineGPU()`
仍按原有参数选择三角形 / 体素模式，默认行为不变；新入口仍遵守 `VV.bSurfaceAdsorption` 总开关。
独立的 `FindNearestVineGlobalDistanceField()` 位于 `Shaders/Private/VineGlobalDistanceFieldProjection.ush`，
原 `FindNearestVoxelSample()` 未改变。四个涉及吸附的 shader 通过独立 permutation 选择新函数。

新入口跳过三角形收集、Nanite 解码、BVH 和表面体素构建。旧表面准备（包括三角剔除参考点）
只在原入口执行；GDF 吸附分支不再调用 `AddVineTriangleSurfacePasses(nullptr)` 创建空三角缓冲，
不接收旧表面三角 / 体素输入，也不启用体素调试出口。源点 / 目标点只读取实例变换。
GDF shader permutation 在预处理阶段排除三角查询代码，编译产物没有三角形 / BVH 资源绑定。
在匹配本世界的真实渲染视图中，
直接采样当帧 GDF 的有效 clipmap/page，用有符号距离和同层梯度迭代投影；不跨帧保存 GDF 纹理。
噪声、初次吸附、最终吸附使用同一 GDF 查询，平滑后再投影一次，再沿法线施加原 `VinesOffset`。
新增参数只影响此入口：最大投影距离默认 200 cm、收敛容差 1 cm、最大迭代次数 8（上限 32）。

SC 与建网格在该视图的 RDG 图里完成，生成独立 GPU 输出缓冲；完成后通过 `EditMeshAsync` 在 GPU
复制到 `VineGeometry`，不回读点 / 网格。该模式不分配 128 MiB 三角池；引擎 GDF 自身的流送 / 纹理
及新增藤蔓输出缓冲属于各自内存开销，不能把它们宣传为受三角池预算约束。

缺页、clipmap 边缘、窄带饱和、无有效梯度、超距离或未收敛的单点返回查询失败，保留该点原位置，
不会偷偷改用三角形或体素。整个 GDF 不可用时等待匹配世界的视图，10 秒未就绪则报告失败并保留旧藤蔓。
`Clean` 会使待处理 GDF 请求失效，完成交接前再次检查请求代号。没有渲染视图的环境不能完成此模式。
GDF 是场景合成距离场，不提供原三角收集路径的 actor tag 筛选；小细节、薄壁和锐角的吸附质量取决于
实际 GDF 分辨率及覆盖范围。它不是精确三角表面的替代保证。

验证：Rider 首个 `Development_Editor / x64` 配置（无明确活动子项标记）的文件级编译及 UHT 通过；
复用 UE 已生成的 GDF 声明、引擎采样辅助函数和当前 pass 源码，DXC 编译新旧分支共 8 个入口通过；
实际投影 HLSL 配解析平面 / 球体采样器，在 D3D11 WARP 上运行的 14 个内外、距离边界、无效样本、
不收敛和视图平移用例通过。后者验证迭代投影数学，不等于真实 GDF atlas 场景验证。
2026-10-15 按同一 Rider 配置完成 Editor 目标构建及 DLL 链接，目标项目 D3D12 / SM6 启动已通过
真实 UE 全局着色器编译和参数绑定，新 GDF 入口可由 Python 反射访问。真实 GDF atlas 场景吸附仍待验证。

> 2026-10-08 三角形吸附试验（2026-10-15 已链接并通过启动时着色器绑定，吸附场景仍待验证）：`FVV::bUseNaniteTriangleSurface`
> 默认开启。Nanite 资产直接按 `NaniteSurfaceCutError` 解码当前驻留层级；普通静态网格 / 实例和地形
> 使用原场景收集规则。几何在 GPU 合并为三角汤、建立 LBVH，噪声阶段、初次吸附及最终投影均查询
> 三角形最近点和几何法线，不再跨面平均体素目标。平滑完成后另做一次三角形重投影。
>
> 此模式绕过表面体素构建；下表的体素流程仍是关闭该选项后的对照路径。`bSurfaceAdsorption=false`
> 仍完整跳过表面准备。默认误差 1 cm；`SurfaceMemoryBudgetMiB` 默认及硬上限均为 128 MiB，
> Nanite、普通网格 / 实例和地形共用。按本批次实际缓冲布局换算三角容量，包含抽取临时缓冲、
> 位置汤和 BVH 构建缓冲；超限拒绝生成，不截断表面。未驻留细节会明确报告 Incomplete。
>
> 当前仅验证几何路线：Nanite 准备沿用同步 `AppendNaniteCuts`，复制出的 pooled position/index
> 引用随请求进入后续异步图，不跨帧保存 Nanite 页地址。它不等于运行时 Actor 方案的异步抽取已经完成。
> 粗截面本身可能偏离显示表面；逐点重投影也不保证相邻线段和整根管截面无穿透。
>
> 已验证：三个修改的 C++ 实现通过 Rider Development Editor / x64 文件级编译；两个新增 shader
> 内核通过 DXC；同一最近点 HLSL 在 D3D11 WARP 执行的 6 个墙面、转角、边界和搜索半径用例通过。
> 编辑器现有抽取接口实测 `SM_NCH_Rock`：5 / 10 / 20 / 50 cm 分别得到
> 1134 / 630 / 378 / 126 三角，均 Complete；1 cm 本次驻留状态是 Incomplete，不能当作 1 cm 精度已达标。
> 当前 `L_TestWorld` 的静态网格没有开启 Nanite，墙角对比会测试普通三角形分支；Nanite 需另用上述资产验证。

### 128 MiB 表面预算与处理顺序（2026-10-08）

预算是**每次生成请求的表面处理新增逻辑 GPU buffer**，不是整个进程显存上限。源网格 render buffers、
引擎 Nanite 流送页、CPU / readback staging、驱动及 RDG 池缓存、SC 生长和藤蔓输出均不计入。
每个缓冲向上按 64 KiB 对齐；同一表面图的缓冲累加，不依赖 RDG 生命周期复用来凑预算。
同步 Nanite 遍历、解码与后续表面图分阶段检查；解码后只保留 position/index，其他流主动延迟释放，不等待 UObject GC。

三角位置是每面 48 B；还需计入 `2N-1` 个 BVH 节点（每节点 32 B）、parent、两份 atomic bounds、
两份按 `nextPowerOfTwo(N)` 分配的排序数组及 dummy。因此 128 MiB 仅算位置汤 + BVH + 构建临时数据，
上限为 **872448 面**，不是按 48 B 相除得到的 2796202 面。当前抽取流程还需要额外缓冲：

- 全 Nanite：按每面最多 3 个解码顶点预留 position/index，上述表面图上限为 **654336 面**。
  层级遍历、cluster 写出计划等另有分配前字节检查，可能更早拒绝；解码后的实际容量还会复检。
- 单个普通网格：含现有收集器属性流与一张材质映射，上限为 **308565 面**。多实例会增加映射缓冲对齐开销。
- 全地形：含初始位置 / 法线上传缓冲，上限为 **256682 面**。
- 混合批次：先扣掉已知普通网格 / 地形收集成本，再换算 Nanite 可用余量；两类不各占一份 128 MiB。

Nanite 计数按层级规模与字节预算分批，随后批量解码。普通网格会逐 request dispatch，并在写出时筛选区域，
但仍按筛选前面数预分配；最终合并 pass 把无效槽位置零，没有缩容或把空闲容量回收给后续 mesh。
所以当前**不是逐 mesh 裁剪、压实入池后再处理下一个**。这里的区域处理是保守剔除，不是将跨边界三角形切成新三角形。

本次验证：UHT 及三个修改的 C++ 文件通过 Rider 首个配置 `Development_Editor / x64` 的文件级编译；
独立执行容量算法，591 组混合容量边界及纯类型边界通过。2026-10-15 已完整链接并在目标项目加载，未进行场景显存峰值测量。

> 2026-09-10 订正：下表「表面体素」「三角形缓存」两行按现状改写；链路现在录在 `AVineContainer::GenerateVineGPU`
> 走的 `UCSMesh::EditMeshAsync` 图里（`FVineMeshSceneProxy` 已不存在，网格归 `VineGeometry` 这个 `UCSMesh` 所有）。

整条链路是**一张 `FRDGBuilder` 图**：表面体素、空间竞争、前缀和、concat、建网格全部记录在同一次 `EditMeshAsync` 里，中间产物是图生命期的瞬态 buffer。

| 阶段 | 实现 | 数据去向 |
| --- | --- | --- |
| 表面体素（位置/法线/目标点场） | `AddCSSurfaceVoxelPasses`，录进生成的同一张 RDG 图 | 图内瞬态，零回读；有效数只在 GPU 计数器里（`PrepareBoxSceneSurfaceVoxelsGPU` 只剩体素调试箭头在用） |
| 三角形缓存 | 已删除（2026-08-12，`fd95aff`） | 体素化直接吃场景三角，不再经缓存 |
| 空间竞争生长 | `AddVineSCPasses`（每源一次） | 瞬态，紧凑计数只写进 GPU buffer |
| 逐源计数前缀和 + concat | `AddVineFusedSCConcatPasses` | 瞬态，合并后的三条 + 总计数 |
| 藤蔓网格 | `AddVineMeshPasses` | 直接写 `VineGeometry`（`UCSMesh`）的常驻流，`VineGpuMesh` 组件每帧绘制 |

游戏线程只做 CPU 侧准备（`AVineContainer::PrepareVineFusedSCInputs`），把结果塞进 `FVineBuildInput::FusedSC` 就返回；解算本身在渲染线程建图时才发生。

`UDynamicMesh` 已从藤蔓表面路径移除；`Save Mesh` 走 `UCSGpuMeshComponent::SaveRenderedMeshToStaticMesh`。

## 仅剩的 CPU 交互

只剩 `Save Mesh` 的一次性回读：固有且按需，StaticMesh 资产必须在 CPU 上构建，不属于卡点。

合图删掉的东西：

- 三处 `FlushRenderingCommands`（SC 两处、concat 一处）；
- SC / concat 的 `ConvertToExternalBuffer`；
- SC 之后的四-uint `LineCounts` 回读；
- `FVineSCGPUBuffers` 结构体和 `AVineContainer::TubeLineGPUBuffers` 字段；
- N 条 render command（N = 源数量）合成 0 条。

## 计数只在 GPU 上意味着什么

`CreateBuffer` 收的是 CPU 元素数，所以**分配尺寸仍由 CPU 决定**：每源按 `min(TargetCount × (SC_MAX_BACKTRACK + 1), VV.MaxVinePointCount / 源数量)` 分配，concat 目标和 `FVineMeshSceneProxy::RegisterStreams()` 的 stream 容量是这些的和。真实点数只有 GPU 知道，因此下游一律"按容量分配、按 GPU 计数派发和绘制"。

支撑这条规则的三处机制：

- **`VineDispatchArgsCS`**（`VVVoxel.usf`）：单线程直接派发，把 `[lineCount, pointCount, segmentCount]` 换算成四组 `DispatchIndirect` 参数，外加 `DrawIndexedIndirect` 参数和 CPU 回读用的 mesh counters。它总会执行，所以零长度藤蔓也会写出一个规规矩矩的零索引 draw，而不是留下上一个 owner 在 pooled buffer 里的残值。

  | 槽位 | 线程数 | 消费者 |
  | --- | --- | --- |
  | 0 | 路径点数 | `ApplyNoise` / `BuildAxes` / `PerlinNoise` / `FinalProject` / `ResampleSurface` / `SmoothPath`×N / `BuildParallelTransportFrame` / `VineUV.Centers` / `VineUV.ScanBlock` / `VineUV.ScanApply` |
  | 1 | 段数 | `VineUV.SegLen` |
  | 2 | `max(顶点数, 段数)` | `BuildVVVoxelCS`（两半用不同索引） |
  | 3 | 顶点数 | `VineUV.Write` |

- **`ConcatPrefixSumCS`**（`SpaceColonizationQueue.usf`）：单线程扫描逐源计数，产出每源 `[pointBase, segBase, clampedPoints, clampedSegs]` 和批次总计数。`FConcatOffsetInt4CS` / `FConcatCopyFloat4CS` 从这个 buffer 读 base 和 count，位置拷贝因此从 `AddCopyBufferPass`（CPU 字节偏移）变成 compute 拷贝。

- **容量只作范围守卫**：`PathPointCount` / `SegmentCount` / `OutputVertexCount` / `OutputIndexCount` 这几个 uniform 现在是分配容量，shader 里只用来夹住索引；真实计数从 `VineMeshCounts` / `VineUV_Counts` 读。轴向 V 的扫描（下一节）三个 kernel 一律按 `VineUV_Counts[1]` 定范围——按容量走会让扫描跑满 26 万槽位。

concat 目标 buffer 在写入前整体清零：group size 向上取整会让最多 63 个线程越过真实计数，`PathPointMeta` 的 `Count` 读到 0 才不会让下游 kernel 拿着任意索引乱跳。

**不要改成"每源固定容量切片"**：那样各源的 segment 在全局不连续，`RWIndirectArgs[0]` 无法用"总段数 × ProfileCount × 6"计算；绕开就得给无效槽位写退化三角形，等于每帧常驻绘制约 150 万个退化三角形（按 `MaxVinePointCount = 262144`、`ProfileCount = 3` 估算）。前缀和是必需项。

## 轴向 V：分段前缀和

基础流路径上的 mesh `UV.y` 由 `VVVoxel.usf` 的 `VineUV*` 一组 kernel 在 GPU 上算出，`BuildVVVoxelCS` 先写的 `0` 被它们覆盖。它算的**不是**点到终点的距离，而是沿线累积的"归一化弧长"，并在每条线的起点清零：

```text
Inc[P]  = (SegLen[P-1] / max(avg(RingCirc[P-1], RingCirc[P]), 1e-4)) * LengthScale
Head[P] = (P == 0) || (SegLen[P-1] < 0)   // SegLen 清成 -1，-1 表示没有段进入 P
CurveV  = Inc 的分段包含式前缀和，在每个 Head 处重启
```

除以平均环周长是关键：V 的单位是"周长"而不是世界单位，贴图沿藤蔓的纵横比才不随粗细变化。`Head` 处 `Inc` 定义为 0，所以段首的 `CurveV` 正好是 0。

因为它是标准的 segmented scan，用的就是两级并行方案，取代了原来 `[numthreads(1,1,1)]` 的串行循环：

| kernel | 派发 | 职责 |
| --- | --- | --- |
| `VineUVScanBlockCS` | indirect，槽位 0 | 块内 LDS 分段 Hillis-Steele，另写出块聚合 `(尾段和, 块内是否含 Head)` |
| `VineUVScanBlockOffsetsCS` | 直接 `1×1×1` | 单块按 tile 循环扫块聚合，产出每块的 exclusive carry-in |
| `VineUVScanApplyCS` | indirect，槽位 0 | 把 carry-in 加回去，跳过块内前缀已经撞上 Head 的点 |

分段合并算子是 `(v,f) × (v',f') -> (f' ? v' : v+v', f|f')`：某个 lane 的窗口一旦包含 Head，它的值就已经定稿，后续步长不能再把更早的 lane 折进来。`VineUVScanBlockCS` 写出的 `VineUV_ScanFlags[P]` 就是"块内 `[0,P]` 是否出现过 Head"，`VineUVScanApplyCS` 直接拿它当"不必加 carry"的判据。

几个容易踩的点：

- **块大小必须等于 `VineMeshGroupSize`**（C++ 通过 `VINE_UV_SCAN_BLOCK` 定义下发）。相等，`VineUVScanBlockCS` 的分块才和槽位 0 的 `DispatchIndirect` 参数一致，不用再开一个 arg 槽位。
- **末块的尾 lane 补 `(0, 0)`**（合并算子的单位元），于是 lane `BLOCK-1` 自然携带最后一个有效 lane 的聚合值；聚合写入必须放在 `P < N` 守卫之外，否则末块根本不写聚合。
- **`VineUVScanBlockOffsetsCS` 单块够用**：它按 tile 循环，一次派发覆盖任意块数（容量 262144 点 = 4096 块 = 64 个 tile）。tile 可能不满，跨 tile 的 carry 要取"最后一个**有效** lane"而不是 lane `BLOCK-1`。
- LDS 用量 `64×4 + 64×4 + 4 = 516` 字节，远低于 Vulkan 保底的 16 KB。

中间 buffer（`VineUV.ScanFlags` / `ScanBlockSum` / `ScanBlockFlag` / `ScanBlockCarry`）由 CPU 按容量派生尺寸（RDG 建图期必须给元素数），但填它们的三个 pass 一律由 GPU 计数定范围，没有任何计数回到 CPU。

## 容量上限

`FVisVineGPUParameter::MaxVinePointCount`（默认 `262144`）是整批藤蔓共用的路径点上限，同时是 GPU buffer 的分配容量。

每源容量取 `min(TargetCount × (SC_MAX_BACKTRACK + 1), MaxVinePointCount / 源数量)`，即理论界与批上限份额中更紧的那个 —— 目标数少时仍按理论界分配，不会因为引入上限反而多占显存。被上限压到时打 Verbose 日志。

代价：`RegisterStreams()` 现在按容量开常驻 stream。极端情况（容量吃满 `262144`、`ProfileCount = 3`）约 44 MB 常驻 + 约 30 MB 图内瞬态。实测场景（36 个 target、单源）容量只有 `3636`，不是问题；真实规模下 `MaxVinePointCount` 需要按场景校准。

## 管径（锥度）：实测链路与"藤蔓变细"的真实来源

管径只由一条公式决定，`BuildVVVoxelCS` 里没有第二个入口：

```text
ring radius = 10 · VV.CircleScale · PathPoints[P].w
PathPoints[P].w = CurveLUT(l / (N-1)) · (TargetScale · StartSourceScale · 分叉锥度)
```

`l` 是点在**本线内**的序号、`N` 是**重采样后**的本线点数（`CurveSpaceColonizationCS` 用 `NewLineOffset` / `NewLineLength`）。锥度在 concat **之前**、每源各自的紧凑布局上求值，所以 concat 的前缀和与 base 语义不参与锥度。

在 `L_TestWorld`（2 源 / 980 target）上做过一次端到端回读核对（临时代码，已删）：

| 量 | 实测 |
| --- | --- |
| GPU 计数 | lines=303 points=12054 segments=11751（容量 197960） |
| `.w` | min 0 / max 1.933 / mean 0.466，其中 303 个 0 恰好等于线数（每条线的 tip） |
| 实测环半径 | min 0 / max 3.866 / mean 0.932 cm |
| 实测半径 ÷ `10·CircleScale·.w` | **1.00000**（11751 个点全部） |

结论：**从 `.w` 到顶点的几何段完全忠实**，`.w` 也没有在 `ConcatCopyFloat4CS` 里被截断或改写。逐线抽样同样对得上，例如 base=15 count=22 的线：`w[mid]` 在 `l=11` 处等于 `CurveLUT(11/21)=0.5238 × 1.749`，`w[last]` 在 `l=21` 处等于 `1.0 × 0.176`（0.176 = 1.749 × `ForkTaperEndScale` 0.1）。**锥度参数 `t` 没有错位。**

真正决定"粗细"的是**曲线本身**：`VV.CurveControl` 在 `L_TestWorld` 的实例上是 `None`，`PrepareVineFusedSCInputs` 于是 `NewObject<UCurveLinearColor>` 兜底。`UCurveLinearColor` 的构造函数（`WITH_EDITOR`）会塞进 black(0)→white(1) 两个 key，因此 `.G` 是一条 **0→1 线性斜坡**：LUT `[0]=0`、`[128]=0.502`、`[255]=1`、mean `0.5`。也就是说，未指定曲线时锥度是"tip 半径 0、根部满值"，**平均管径只有平坦曲线（`.G ≡ 1`）的一半**，且每条线的第一个环是退化的零半径环。这不是 bug，是"没配曲线"的默认外观；想要更粗就给 `VV.CurveControl` 指一条 `.G` 抬高的曲线，或调 `VV.CircleScale`（默认 `0.2`）。

## 风险与未验证项

- **整体外观一致性未确认。** 已实测的是"跑得起来"：在 `Lvl_TopDown` 上用合成的 1 源 / N×N target 网格跑通 `GenerateVines`，整张 `VineMesh.Build` 图在 GPU 上执行完毕，无 TDR、无崩溃、无 shader 参数校验失败。除轴向 V 与上一节的管径外，**没有**与旧路径做过逐顶点或截图比对。
- **SC 输出与合图前的版本逐数对齐**：`L_TestWorld` 上合图后回读到 points=12054 / segments=11751，和合图前那次日志里的 `Vertices=36162 Indices=211518`（= 12054×3 / 11751×3×6）完全一致。这一批改动没有动几何。
- **每源容量被批上限压到时会静默截断。** `PerSourceCapacity = min(TargetCount × 101, MaxVinePointCount / 源数量)` 直接当 `EmitSpaceColonizationLinesCS` 的 `PathPointCapacity`，越界点只是 `break`；而 `LineLength` / `LineOffset` 仍是未截断的值，`CountResample` / `EmitResample` 会读到越界（返回 0）的点，弧长和锥度随之失真。计数只在 GPU 上，所以**没有任何警告**（`MaxVinePointCount` 的属性注释说"会截断并打警告"，实际没有）。实测场景容量 197960 vs 真实 12054，远未触发。
- **轴向 V 已做过几何自洽核对**（不是逐顶点 A/B）：`Save Mesh` 烘出 StaticMesh 后，从索引缓冲重建环拓扑（管状网格每个三角形恰有两点同环、一点在邻环），逐对相邻环核对 `V[b] == V[a] + dist(Ca,Cb)/avg(周长)*LengthScale`，段首允许 `V == 0`。14003 环 / 26105 对相邻环全部通过，最差相对偏差约 `1e-3`，且不随点数增长——这个量级是 StaticMesh 把 UV 存成 half 造成的量化底噪，不是扫描误差。跨块 carry 若丢失会造成整块量级的偏差（相对误差百分之几），核对里没有出现。
- 上述核对的**取样噪声底是 half 精度 UV**，只能验到约三位有效数字。要更严格就得在 GPU 上直接回读 `VineUV.CurveV`（当前没有这条通路）。
- `Save Mesh` 的 local-space 烘焙改用 actor 变换（leaf 被钉在 identity 世界变换上，用其自身组件变换会导致双重偏移），生成的 `AStaticMeshActor` 同样摆在 actor 变换上。该路径在上面的 V 核对里被实际走通了一次（能烘出 StaticMesh 并读回顶点），但摆放位置的正确性仍未核对。
- `FCSSurfaceVoxelGPUBuffers` 的 `VoxelCount`（真实数）与 `VoxelCapacity`（容量）在 `VineLeaf_BuildVineBuildInput` 的两条分支里取值不一致：GPU-voxel 分支把 `VoxelCount` 也设成容量。`BuildVineVoxelHashCS` 已改为从 `Counter[0]` 读真实数并早退（容量 slack 全是同一个零 cell，插进哈希表会让线性探测退化成 O(slack²) 原子操作，实测直接把 GPU 挂死），但其余 kernel 的 `VoxelCount` 仍是容量，只影响回退路径的松紧，未收敛。

## Open Questions

- SC 是 `SC.Iteration` 轮 × 每轮若干 pass，且每源一遍。合图后 N 个源全在一张图里，pass 数是 N × (3 + 5 × Iteration + 10)，建图开销和 RDG Insights 可读性会随源数量变差。源很多时值得把 SC 改成单次 dispatch 内按 source index 分流。
- `MaxVinePointCount` 的默认值 `262144` 未经真实场景校准。它现在直接决定常驻显存，需按实际藤蔓规模调整。
- `AVineContainer::DrawDebugCachedVineSCStagePoints` 已退化成只打一条警告的存根（CPU 侧 SC 阶段点缓存随解算上 GPU 一并移除），只为不破坏已有蓝图绑定而保留。确认无蓝图依赖后可以删除。（同类的 `SpaceColonizationWithScales` 存根已删除。）
- ~~**`BP_VineSource` 编译不过**~~（2026-08-13 已修）：删 `UDynamicMeshComponent` 那一批之后，`GenerateVines` 的返回值从 `UDynamicMesh*` 变成 `bool`，蓝图里 `GenerateVines → ReturnValue` 还接着 `IsValid` 的 `InputObject`，另有 6 处 `Get DynamicMeshComponent` 变量取值失效。已在蓝图里删掉这条早就无产出的支线（旧链路只是把返回的**空** `UDynamicMesh` 塞回 `SetDynamicMesh`，所以断掉本就不影响藤蔓外观），生成结果已人工确认正确。

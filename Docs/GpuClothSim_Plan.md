# GPU 布料模拟：XPBD 常驻算子驱动 UCSMesh 形变

承接 [`GpuMesh_DynamicMeshWrap_Plan.md`](../../../GpuMesh_DynamicMeshWrap_Plan.md)（`UCSMesh` / `FCSMeshEditContext` / `UCSMeshRenderComponent` 对象层已落地）。本文是 GPUMesh 管线内自写布料模拟的设计与落地记录：布料状态常驻 GPU，每帧一组 XPBD compute pass 解算，结果直接写回 `UCSMesh` 的 Position / TangentBasis 流，由现有渲染叶子绘制。

**状态（2026-10-26）**：P0、P1 已落地，另外提前做了 P3 的 tether 与 P4 的约束上色（后者是 P0 验收逼出来的，见 D3）；P2 碰撞升级、P3 自碰撞、P4 其余项未做。「现状盘点」一节的行号是 2026-08-24 的，其余各节按 2026-10-26 的代码写。

姊妹篇：[`GpuRigidSettle_Plan.md`](GpuRigidSettle_Plan.md)——共享粒子基建的批量刚体沉降 / 稳定性校验；多实例批处理与粒子 spatial hash 在彼处先落地。

## 结论

- **可行，定位是「固定拓扑的每帧形变器」，不是通用物理引擎**。运行期只改顶点位置与法线，不改三角数、不改流布局——这正好避开管线里最难的「计数只在 GPU 上」问题：布料的计数在 setup 时就是 CPU 已知的固定值。
- **求解器自写 XPBD，不是「把 Chaos 搬上 GPU」**。Chaos 布料求解器没有 GPU 路径可迁移（见下节），而自写所需的基建（常驻流、借图编辑入口、多 substep 单图先例）项目里全部现成。
- **sim 状态不进 `UCSMesh` 流集合**，由组件 `UCSClothSimComponent` 自持一套 pooled buffer（`FCSClothGpuState`）；每帧通过 `FCSMeshRenderThreadEdit` 借图写回网格流。这是本设计最重要的一条边界，理由见 D1。
- **运行循环零回读、零阻塞**：setup 一次性 `ReadbackMeshSync` 取拓扑、CPU 建约束、上传；之后每帧纯 GPU。`HangingCloth` 用例断言连跑 120 帧 `UCSMesh::GetBlockingFlushCount()` 增量为 0。
- **渲染与落盘免费**：`UCSMeshRenderComponent` 绑同一个 `UCSMesh`，每帧只有 buffer 内容变、身份不变（AllocationGeneration 不动），proxy 零重建；`CopyToStaticMesh` 读的就是被模拟的流，冻结当前姿态成资产是零成本副产品。

## 与 Chaos 的关系

- 引擎事实（5.7.4 源码核实）：`Engine/Plugins/ChaosCloth` 下 `.usf`/`.ush` 数量为 0，求解实现是 3 个 `.ispc`（`ChaosClothingSimulationSolver.ispc` 等）——CPU + SIMD，跑在物理线程。「Chaos 布料上 GPU」没有可迁移的实现，只能自写。
- 结果互通留缝（**不在本计划范围**）：引擎 `SkeletalMeshDeformerHelpers.h:50` 的 `GetClothBuffersForReading` 暴露 Chaos 解算结果的 GPU SRV（sim 粒子位置法线 + 渲染顶点重心映射），DeformerGraph 的 `DataInterfaceCloth.ush` 里有现成插值数学。将来若要「Chaos 解算、GPUMesh 加工」，一个读 SRV 写常驻流的 pass 即可，与本计划互不阻塞。
- `GPUSkeletalTreeComponent`（`GPUSkeletalTreeComponent.h:36`）现在的 CPU 摆动是本功能的前身：布料落地后树叶/垂藤类摆动可换用真模拟。

## 与 Marvelous Designer 的对比

定位完全不同，不构成二选一；三者按场景分工。

| 维度 | Marvelous Designer / CLO | 本计划（GPUMesh XPBD） | Chaos Cloth（参照） |
| --- | --- | --- | --- |
| 定位 | 离线服装设计 DCC：2D 版片 → 缝合 → 成衣模拟 | 引擎内运行时形变器，`UCSMesh` 常驻流上解算 | 引擎内运行时角色服装（骨骼驱动） |
| 运行时机 | 制作期交互模拟；结果烘缓存（Alembic 等）导入引擎回放 | 游戏运行时每帧解算 | 游戏运行时（物理线程 CPU） |
| 求解预算 | 离线级：高密度网格、大迭代数、精确自碰 / 摩擦 / 多层衣料 | 帧预算内 substep × 迭代；自碰 P3 可选 | 帧预算内，粒子数千级 |
| rest 状态语义 | 2D 版片平面态（真衣料的自然静止态） | rest = setup 时的 3D 姿态 | ClothAsset sim mesh |
| 交互性 | 游戏内无 | 风 / 碰撞体运行时全可变；钉点跟随宿主或 AttachActor 运动 | 跟随动画与碰撞体 |
| 产物 | 高保真网格 / 动画缓存资产 | 每帧姿态；`CopyToStaticMesh` 随时冻结 | 蒙皮渲染结果 |

- **分工结论**：过场级高保真布料在 MD 制作、烘 Alembic 走 GeometryCache 回放；游戏内交互性布料（旗、披风、幕布、程序化网格摆动）走本计划；骨骼角色服装走 Chaos。运行时 XPBD 不追平离线质量，也不必。
- **可借鉴点——版片 rest 语义**：衣料的真实静止态是 2D 版片平面，不是 3D 摆放姿态。若网格来自 MD/CLO 导出（UV 即版片坐标），拉伸 / 弯曲约束的 RestLength 可改按 UV 空间计算（开关 `bRestLengthFromUV`），褶皱行为更接近真衣料；程序化网格保持 3D 边长。仍在 Open Questions。
- MD/CLO 自带 GPU 加速模拟选项（知识性说明，未逐版本核实），但那是 DCC 内的制作加速，与引擎运行时无关。

## 现状盘点：可复用设施

2026-08-24 写计划时的盘点（行号是当时的）。

| 设施 | 位置 | 在布料里的角色 |
| --- | --- | --- |
| `FCSMeshRenderThreadEdit` | `CSMesh.h:313` | 每帧写回入口：借 caller 的图、不 flush、析构自动恢复流访问态 |
| `EditMeshSync` | `CSMesh.h:482` | setup 期一次性写入（包围盒、`SetKnownCounts`） |
| `AddVertexWeldPasses` 的 grid hash | `CSGpuTriangleUtilities.h:68` | setup 焊接语义的参照（落地用的是 CPU 版）；P3 自碰撞 spatial hash 的同族前例 |
| `AddTriangleLBVHBuildPasses` / `AddFastWindingMultipolePasses` | `CSGpuTriangleUtilities.h:34` / `:49` | P2 场景三角碰撞：最近点查询 + 内外判定救回 |
| `FGDFSampleService::EnqueueGDFJob` | `GDFSampleService.h:106` | P2 全局距离场碰撞；job 在渲染器图内 base pass 后执行（GDF 参数此刻才就绪，`GDFSampleService.cpp:24`） |
| ShallowWater 求解 tick | `ComputeShaderShallowWater.h:332` | 「GT 收集 → render command → 单图多 substep」的既有形态 |
| `VinePerlinNoise.ush` | `Shaders/Private/` | P1 风场噪声采样 |
| `CSGpuMemoryBudget` | `CSGpuMemoryBudget.h` | sim buffer 分配预检 |
| `CSMeshOps.usf` 的流写入约定 | `Shaders/Private/CSMeshOps.usf:27` | 写回 pass 的 RW 布局直接照抄（打包 8888 切线） |

## 设计

### D1 数据：FCSClothGpuState（组件自持，不进 UCSMesh）

不把 sim 状态做成 `AuxVertex` 流挂进网格，理由三条：

- **粒子数 ≠ 渲染顶点数**：sim 粒子是焊接后的唯一位置，渲染顶点沿 UV 缝有重复；硬塞会污染 readback/save 语义（速度 buffer 不该被 `CopyToStaticMesh` 看见）。
- **算子契约保持干净**：`UCSMeshOps` 任意拓扑算子（布尔、焊接）跑过后布料约束即作废。分离后 `UCSMesh` 仍然「任何算子可跑」，sim 侧靠 Generation 检测失配停摆，而不是让算子迁就布料。
- 有先例：`FCSSurfaceVoxelGPUBuffers`（actor 持 retained pooled 集）就是这个形态。

全部是 `TRefCountPtr<FRDGPooledBuffer>`，定义在 [`CSClothSimRender.h`](../Source/ComputeShaderGenerator/Private/CSClothSimRender.h)；位置一律在 sim 空间（相对 `SimOrigin` = setup 时的宿主位置，大世界坐标下 float 的分辨率不吃进边长）：

```cpp
// 粒子域（N = 焊接后唯一位置数）
Positions          // float4 × N：xyz 当前位置，w = InvMass（0 = 钉住）—— 约束 kernel 每端一次取数
PrevPositions      // float4 × N：上一 substep 末的位置；速度 = (x - xPrev) / dtPrev
RestPositions      // float4 × N：setup 姿态；钉住粒子的运动学目标 = 钉组变换 × rest
PinGroups          // uint   × N：钉组号，CS_CLOTH_FREE_PARTICLE = 自由
NormalAccum        // int    × 3N：法线定点累加器（Finalize 读完即清）
ParticleNormals    // float4 × N：本帧末的粒子法线；风的受力面朝向
RestNormals        // float4 × N：setup 时用同一套法线 pass 算出

// 约束域（setup 时 CPU 一次性构建上传，运行期只读）
Constraints        // uint4  × C：(A, B, asuint(RestLength), Type)，拉伸 / 弯曲 / tether 三类，按颜色连续存放
Lambdas            // float  × C：XPBD 的 λ，每 substep 第一次迭代视为 0
TriangleParticles  // uint   × 3T：法线累加用（退化三角与双面网格的背面已剔除）

// 映射域
CornerToParticle   // uint × V：渲染顶点 → 粒子
RestTangents       // uint × 2V：setup 时网格切线流的原样拷贝（含副切线符号）
Status             // uint × 4：[0] = 写回时遇到的非有限粒子数，非阻塞回读

// 每帧小数组（GT 打包，渲染命令里上传）
PinTransforms      // float4 × 6G：每钉组上一帧 + 本帧的 3x4（sim 空间，作用在 rest 上）
Colliders          // float4 × 4K：每碰撞体上一帧 + 本帧的形状
```

C++ 与 HLSL 共用的布局常量（定点比例、约束类型、碰撞体 / 钉组布局）在 `Shaders/Private/CSClothLayout.ush`。它刻意没有并进 `CSGpuSharedLayout.ush`：那一份被 GPU 实例化的顶点工厂 include，顶点工厂的源码哈希是每一张材质 shader map 的键的一部分，往里加一行全工程材质 shader 重编。

### D2 组件与 tick：UCSClothSimComponent

- **宿主形态**：`TickComponent`（`TG_PostPhysics`，`bTickInEditor`，编辑器视口里也跑）收集 dt / 风 / 碰撞体 / 钉组变换 → 一条 `ENQUEUE_RENDER_COMMAND` → 渲染线程自建一张 `FRDGBuilder` 图跑完全部 substep → 图尾构造 `FCSMeshRenderThreadEdit`（只登记 Position 与 TangentBasis 两条流）跑写回 pass、析构恢复访问态 → 执行图。不用 `EditMeshSync`：它每次 flush，每帧 flush 不可接受。`bSimulate` 关掉后由调用方 `StepSimulation(dt)` 手动推进（测试就走这里）。
- **绑定协议**：`SetTargetMesh(Mesh)` + `SetupFromMeshSync()`（回读 → 焊接 → 建约束 → 上传 → 缓存 Generation / AllocationGeneration）。每帧解算前校验二者；不一致（别人跑了算子 / resize 了）→ 停摆 + log，等待重新 setup。sim 的每帧写回不 bump Generation、不广播事件。写回时撞见非有限粒子同样停摆（D3）。
- **钉组**：组 0 = 宿主 actor；每个带 `AttachActor` 的钉盒按 actor 去重各占一组。钉住粒子的目标 = 「组的 setup→当前」变换 × rest 位置，substep 之间按上一帧 / 本帧插值。钉盒写在宿主的局部空间里，所以拖动宿主时钉边跟着走、布面靠惯性摆。宿主一帧挪动超过 `TeleportDistance` 按瞬移处理：自由粒子连同速度刚体搬过去。
- **包围盒**：setup 时一次性放大为各钉组「钉住部分 + 可达半径」的并（可达半径 = 挂在该组上的自由粒子到钉点的最远测地距离 × TetherScale）。宿主 / AttachActor 挪出已发布的范围时，用 `EditMeshAsync` 非阻塞地重发；完成回调与 Generation 自增在同一跳游戏线程上，回调里把期望的 Generation 跟上，自己的这次编辑不会被当成「网格被改了」。连不到钉点的布（自由落体）没有上限可言，按 rest 尺寸放宽。
- **VSM**：布每帧变形而组件变换不动，VSM 的缓存页按变换失效、发现不了。setup 时把同一宿主上画这张网格的 `UCSMeshRenderComponent` 标 `ShadowCacheInvalidationBehavior = Always`；`ACSClothActor` 的渲染组件构造时就标好。
- **编辑器**：改柔度、风、碰撞体这类每帧上传的属性下一帧生效，布不重置；改 setup 用掉的属性（面密度、钉点、tether、焊接容差）先 `ResetToRestPose` 把网格摆回 rest 再重建。GPU 状态只在组件销毁时放，不在 `OnUnregister`：编辑器里改任何一个属性都会把组件注销再注册一遍。
- **编辑器模式下模拟**：组件 `bTickInEditor`、`bSimulateInEditor` 默认开，不进 PIE 也在解算。编辑器 world 只在有视口开着 Realtime 时按 `LEVELTICK_ViewportsOnly` tick（否则 `LEVELTICK_TimeOnly`，任何 actor / 组件都不 tick）；编辑器窗口退到后台时，偏好设置「Use Less CPU when in Background」（默认开）给所有视口挂上 Realtime 关闭的覆盖，布停在那一帧。所以 `ACSClothActor` 的布在 `OnConstruction` 里当场建，不靠 tick：放下去那一刻就有布，模拟等视口亮起来再走。`GetSimulatedFrameCount()` 不涨就是这个原因。

### D3 求解循环：XPBD substep pass 序

整帧 pass 流与写回边界示意：[`GpuClothSim_PassFlow.svg`](GpuClothSim_PassFlow.svg)。kernel 在 [`CSClothSim.usf`](../Shaders/Private/CSClothSim.usf)。

```text
for s in 0..Substeps:                # small-steps XPBD：substep 数替代迭代数，默认 8
  PredictCS        # 1 thread/粒子：v = (x-xPrev)/dtPrev·damp + (g + wind)·dt；钉住粒子按钉组变换插值
  for i in 0..Iterations:            # 默认 1
    for c in Colors:                 # 拉伸 + 弯曲的颜色在前，tether 的颜色在最后
      SolveColorCS # 1 thread/约束：XPBD 距离约束，直接改写两端位置（同色约束不共享可动粒子）
  CollideCS        # 1 thread/粒子：analytic 球 / 胶囊 / 半空间投影 + 位置级摩擦（有碰撞体时）
AccumulateNormalsCS  # 1 thread/三角：角度加权单位面法线，定点 InterlockedAdd 到三个粒子
FinalizeNormalsCS    # 1 thread/粒子：归一化写 ParticleNormals，清零累加器
SkinBackCS           # 1 thread/渲染顶点：位置写回；rest 切线基按粒子法线 rest→当前的最小旋转转过去
```

- **约束是上色的 Gauss-Seidel**：setup 时贪心上色，同一种颜色里的约束两两不共享可动粒子（钉住的端点只读，共享不算冲突），一次派发内每条约束直接改写两端，没有原子、没有平均。规则网格是 13 种距离约束颜色 + 1 种 tether 颜色。没有用 Jacobi + 定点原子累加：每个粒子的修正要除以挂在它身上的约束数（约 13），拉伸收敛太慢，过不了 P0 的边长误差门（实测最大 12.8%，上色后 3.25%）。
- **三类距离约束一种记录**：拉伸（每条唯一边）、弯曲（流形内边两侧对顶点之间的跨边距离）、tether（每个自由粒子到最近钉点，上限 = 测地距离 × TetherScale，单边，只在拉长时起作用）。拉伸边被压短时换用更软的 `CompressionCompliance`：布受压起皱，不像橡皮一样顶回去。
- **切线基**：每个渲染顶点用自己的 rest 切线基，按所在粒子的法线从 rest 到当前的最小旋转转过去。rest 姿态下严格恒等（`RestPoseIsIdentity` 用例：位置零漂移、切线基在 8 bit 量化以内）；硬边与 UV 缝两侧共享粒子，但各自的 rest 偏差保留下来。
- **NaN 守卫在写回之前**：`SkinBackCS` 遇到非有限粒子不写回那个角点（保留上一帧的值），计数进 `Status`；状态 buffer 非阻塞回读，游戏线程见到非零就停摆。
- 所有 pass 用参数结构体声明资源走 `FComputeShaderUtils::AddPass`；颜色之间的写后读由 RDG 排序并插 UAV 屏障。`r.RDG.ImmediateMode=1` 下全部用例照绿，且结果与默认模式逐位相同。

### D4 碰撞分层

| 层 | 内容 | 阶段 · 状态 |
| --- | --- | --- |
| analytic | 半空间 / 球 / 胶囊（可跟随 actor），GT 每帧上传，substep 间按上一帧 / 本帧形状插值 | P0 · 已落地 |
| 场景三角 LBVH | setup 时对目标 box 提取场景三角 + `AddTriangleLBVHBuildPasses` 建一次并常驻化；粒子最近三角投影 + FastWinding 内外救回 | P2 候选 · 未做 |
| GDF | `EnqueueGDFJob` 内采样全局距离场，距离 + 梯度推出穿透 | P2 候选 · 未做 |
| 自碰撞 | 粒子 spatial hash（weld grid hash 同族），默认关 | P3 · 未做 |

### D5 setup 管线（一次性，可阻塞）

CPU 半程在 [`CSClothSim.cpp`](../Source/ComputeShaderGenerator/Private/CSClothSim.cpp)（`CSClothSetup::BuildFromSnapshot`，无 RHI，测试可直接调）：

1. `ReadbackMeshSync` 一次取快照（逐顶点属性；逐角点快照被拒）。
2. 位置焊接：网格哈希，格宽 = `WeldTolerance`，查 27 邻格 → 粒子表 + `CornerToParticle`。
3. 三角映射到粒子；退化三角与重复三角（同一组粒子，即双面网格的背面）剔除——背面若参与法线累加，正反两面的面法线正好抵消。
4. 质量 = 相邻三角面积的 1/3 × `ArealDensity`；钉点 = 钉盒（宿主局部空间，先到先得）∪ 顶点色遮罩（`PinColorChannel` ≥ 阈值）；不属于任何三角的粒子钉在组 0。
5. 约束：唯一边 → 拉伸，流形内边 → 弯曲（两端都钉死的不出）；多源 Dijkstra 求到最近钉点的测地距离 → tether 与各钉组的可达半径。
6. 上色：拉伸 + 弯曲一起贪心上色，tether 另占一组颜色排在最后；按颜色计数排序，得到每次派发的 `[Offset, Count)`。
7. 过显存预检后，一条渲染命令建全部 pooled buffer 并上传；rest 切线基从网格切线流原样拷贝（CPU 快照解包时丢了副切线符号，所以不从快照来）；rest 粒子法线用运行期同一套法线 pass 算——整数原子累加与顺序无关，所以 rest 姿态下写回严格恒等。
8. `EditMeshSync` 写放大后的包围盒与精确计数（它的 flush 同时是上一步的栅栏）。

## 阶段计划

| 阶段 | 内容 | 验收门 | 状态 |
| --- | --- | --- | --- |
| P0 | 组件 + `CSClothSim.usf`（Predict / 约束 / analytic 碰撞 / 法线 / 写回）+ box 钉点 + 自动化测试 | headless 测试：悬挂布 N 帧后钉点不动、自由端 Z 下降、边长误差 < 5%、无 NaN；编辑器目视 30k 粒子稳定；`r.RDG.ImmediateMode` 全绿 | 2026-10-26 落地；30k 粒子做了无头稳定性用例；编辑器模式下的模拟由 `Scripts/RunClothEditorModeCheck.ps1` 在真编辑器里验过（2026-10-27，见「实施状态」），30k 粒子没在编辑器里看过 |
| P1 | 风场（Perlin）+ 顶点色 mask 钉点 + BP 参数面板（stiffness/damping/substeps） | 目视摆旗；测试补风力方向断言 | 2026-10-26 落地；风力方向有断言，目视摆旗未做 |
| P2 | 碰撞升级：LBVH 或 GDF（先行者见 Open Questions） | 布落到场景网格不穿插（目视 + 粒子-表面距离采样断言） | 未做 |
| P3 | tether + 自碰撞开关 | 布叠布不互穿目视 | tether 已随 P0 落地；自碰撞未做 |
| P4 | graph coloring、substep 自适应 / 距离 LOD、多实例池化 | profile 对比数据 | graph coloring 已随 P0 落地（D3）；其余未做 |

每阶段独立可回退；P0 的测试同时是后续所有阶段的回归基线。

## 实施状态

| 文件 | 内容 |
| --- | --- |
| [`Public/CSClothSim.h`](../Source/ComputeShaderGenerator/Public/CSClothSim.h) / [`Private/CSClothSim.cpp`](../Source/ComputeShaderGenerator/Private/CSClothSim.cpp) | 面板数据类型（钉盒、碰撞体、顶点色通道）；setup 的 CPU 半程；程序化网格布 `CSClothSetup::BuildGridSnapshot` |
| [`Private/CSClothSimRender.h`](../Source/ComputeShaderGenerator/Private/CSClothSimRender.h) / [`.cpp`](../Source/ComputeShaderGenerator/Private/CSClothSimRender.cpp) | `FCSClothGpuState`、setup / 每帧 / 重置的 RDG pass、诊断用的粒子回读 |
| [`Public/CSClothSimComponent.h`](../Source/ComputeShaderGenerator/Public/CSClothSimComponent.h) / [`Private/CSClothSimComponent.cpp`](../Source/ComputeShaderGenerator/Private/CSClothSimComponent.cpp) | `UCSClothSimComponent`：参数面板、BP API、钉组、失配停摆、包围盒发布 |
| [`Public/CSClothActor.h`](../Source/ComputeShaderGenerator/Public/CSClothActor.h) / [`Private/CSClothActor.cpp`](../Source/ComputeShaderGenerator/Private/CSClothActor.cpp) | `ACSClothActor`：源网格或程序化网格布 + 渲染组件 + 布料组件；网格布预设钉边；当前姿态存成 StaticMesh |
| [`Public/CSClothBandSplit.h`](../Source/ComputeShaderGenerator/Public/CSClothBandSplit.h) / [`Private/CSClothBandSplit.cpp`](../Source/ComputeShaderGenerator/Private/CSClothBandSplit.cpp) | 带状分块：按测地距离把网格切成条带，产出焊接键、锚点、旋流方向的逐顶点绑定（[`HoudiniFallSplit_Port.md`](HoudiniFallSplit_Port.md)） |
| [`Public/CSClothSplitActor.h`](../Source/ComputeShaderGenerator/Public/CSClothSplitActor.h) / [`Private/CSClothSplitActor.cpp`](../Source/ComputeShaderGenerator/Private/CSClothSplitActor.cpp) | `ACSClothSplitActor`：骨骼网格角色播动画、切成条带、锚点每帧跟 CPU 蒙皮位置走 |
| [`Shaders/Private/CSClothSim.usf`](../Shaders/Private/CSClothSim.usf)、`CSClothLayout.ush` | 7 个 kernel（含锚点绳 `SolveAnchorsCS`）；C++ / HLSL 共用常量 |
| [`Private/Tests/CSClothSimTests.cpp`](../Source/ComputeShaderGenerator/Private/Tests/CSClothSimTests.cpp) | `PCGPlugins.ComputeShaderGenerator.ClothSim.*` 11 条、`ClothSplit.*` 4 条 |
| [`Scripts/RunClothEditorModeCheck.ps1`](../Scripts/RunClothEditorModeCheck.ps1) + [`ClothEditorModeCheck.py`](../Scripts/ClothEditorModeCheck.py) | 真编辑器里（不进 PIE）验 `ACSClothActor` 放下即有布、只靠编辑器 tick 模拟；跑之前备份、跑完恢复编辑器偏好 |

最小用法：关卡里放一个 `ACSClothActor`（默认 2 m × 2 m、64² 格、−X 边钉住，编辑器视口里就开始摆）；或者在自己的 actor 上给已有的 `UCSMesh` 加一个 `UCSClothSimComponent`，`SetTargetMesh` 后 `SetupFromMeshSync`。

2026-10-26 验收（`Scripts/RunPluginAutomationTests.ps1 -Filter PCGPlugins.ComputeShaderGenerator.ClothSim`，真 RHI；前 10 条在默认模式与 `-CVars "r.RDG.ImmediateMode=1"` 下各跑一遍，均 10/10，之后加的 `VisualCapture` 在默认模式下通过；同日全插件 268 条只有既有的 4 条红）：

| 用例 | 判据 | 实测 |
| --- | --- | --- |
| `SetupTopology` | 焊接、各类约束数、质量、钉点、tether 锚定、上色互不冲突且 tether 排最后、双面去重、逐角点快照被拒 | 4×3 格 85 条约束 12 种颜色 |
| `HangingCloth` | 21² 布（5 cm 格）挂 120 帧：钉点漂移 < 0.01 cm、自由端落下、最大边长误差 < 5%、写回位置 = 粒子位置、法线与 CPU 重算差 < 5°、发布的包围盒装得下全部粒子、零阻塞 | 平均 0.32%、最大 3.25% |
| `RestPoseIsIdentity` | 零重力 10 帧：位置零漂移，切线基在 8 bit 量化以内（斜摆、双面） | 通过 |
| `SphereCollision` | 不钉的布落到 r = 40 的球上：没有粒子进入球 + 厚度，中心搭在球顶 | 通过 |
| `Wind` | 竖帘沿法线吹 +X 500 cm/s：无风时贴在平面里，有风时自由部分往 +X 鼓 > 10 cm | 通过 |
| `VertexColorPins` | R 通道 = 1 的边钉住不动，其余落下 | 通过 |
| `HaltsOnForeignEdit` | setup 后别的算子改网格 → 拒绝步进、报停摆；重新 setup 后恢复 | 通过 |
| `PinsFollowHost` | 平滑拖动 100 cm 后钉点精确跟随；一帧跳 2000 cm 整块搬过去；重置回到宿主现位置上的 rest 姿态 | 通过 |
| `LargeClothStable` | 181² 布（1.1 cm 格，32761 粒子）挂 120 帧：无 NaN、不停摆、平均边长误差 < 5%、最大 < 50% | 平均 3.5%、最大 43.6%；录制到 GPU 空闲 0.6~0.9 ms/帧（四次运行，含 CPU 录制，不是纯 GPU 时间） |
| `ClothActor` | `ACSClothActor` 建网格、绑渲染组件、按预设钉两个角、挂下来 | 通过 |
| `VisualCapture` | 杆上的帘子 + 球上的桌布，真渲染（受光 + 世界法线两张，存 `Saved/ClothSimShots/`）；布在与不在逐像素比，差异 > 5% | 15.6%；形状、褶皱、接影靠看图（影子跟着形变走，不是 rest 的方块） |

2026-10-27 真编辑器验收（`Scripts/RunClothEditorModeCheck.ps1`，Template_Default 关卡，默认构造的 `ACSClothActor`，不进 PIE）：spawn 回来、一个 tick 都没过就已有 4225 个粒子的布；之后第 1 个编辑器 tick 起开始解算，240 个编辑器 tick 解了 240 帧，不停摆；放下时与 4 秒后两张图 68% 的采样像素变了（布垂下来了）；把 actor 拖 300 cm，网格发布的包围盒中心跟着移 247.5 cm；把 `GridResolution` 改成 32² 当场重建为 1089 个粒子。脚本拉起的编辑器拿不到焦点，要靠 runner 用 `-ini:EditorSettings:...:bThrottleCPUWhenNotForeground=False` 关掉后台降频才 tick（见 D2）；跑前后两份偏好文件的 SHA-256 一致。

细布的拉伸误差是收敛问题，不是稳定性问题：181² 布在 tether 1.1 时，substep 8 / 16 / 32 的平均误差是 4.4% / 3.1% / 1.1%，加迭代远不如加 substep；把 tether 收紧到 1.02 平均 2.9%（所以默认 1.02），1.0 反而更差（tether 与边长约束顶牛）。最坏的边在自由侧边甩动的那一段。

## 风险

- **sim 自有 buffer 的访问态**：`FCSMeshRenderThreadEdit` 只恢复网格流；sim 自持的 pooled buffer 只在本插件的 compute pass 之间流转，每帧 `RegisterExternalBuffer` 后交给 RDG 默认收尾（SRVMask），没有图外的消费者。
- **定点累加器溢出**：只剩法线累加（每角贡献 ≤ π），比例与上限写在 `CSClothLayout.ush` 的注释里。约束不走累加器。
- **爆炸 / NaN**：dt 钳到 `MaxDeltaTime`、速度钳到 `MaxSpeed`、单次修正钳到 `MaxCorrection`，写回前的 NaN 守卫 + 停摆（D3）。
- **运动矢量**：顶点工厂没有上一帧位置流，TAA / TSR 看不到形变的速度，摆得快时有拖影。
- **光追 BLAS 停在 rest 姿态**：BLAS 跟着 draw-args 回读重建，布料的逐帧写回不触发它；硬件 Lumen / 光追阴影 / 反射看到的是 rest 形状。
- **自由落体的包围盒**：连不到钉点的布按 rest 尺寸放宽，掉出范围就被剔除；要么加地面碰撞体，要么周期性重新 setup。
- **TDR**：P2 LBVH 每粒子查询在大场景要设深度上限（本机 `MeshBoolean` 有百万级三角 TDR 前科）；粒子数 × 查询代价预算化。
- **一帧延迟只在 GDF 模式**：现在的自建图当帧生效；若 P2 切 GDF 宿主则延迟一帧，文档与参数面板都要明示。
- **派发数**：每帧约 substep × (颜色数 + 2) + 3 个 pass（默认约 130）；单块布不成问题，几十块同屏时 CPU 录制开销会显出来，P4 的多实例批处理在这里。
- **unity/jumbo 同名符号**：新 TU 的 file-local helper 一律 `CSCloth_` 前缀（`CSMB_` 教训）；四个新 `.cpp` 都过了 UBT `-SingleFile`。
- **headless 测试判定看日志不看退出码**（本机无头编辑器退出约 20% 概率异常的既有结论）。

## Open Questions

- P2 先 LBVH 还是先 GDF？GDF 免建免维护、但精度粗、拖一帧延迟、依赖 DF 开启；LBVH 精确自管、但要处理场景变化后的失效重建。
- 多实例（N 面旗）：N 组件 N 张图，还是单图批处理（buffer 拼接 + 每实例偏移）？P4 池化时一并定；批处理形态在沉降姊妹篇先落地，可直接搬。
- 硬边上新出现的折痕：现在法线跟着粒子法线一起转，rest 时的硬边偏差保留，但模拟中折出来的棱不会变硬；需要时再加角度阈值方案。
- 解算结果（位置/速度）是否暴露给 Niagara 做飘落粒子 / 交互反馈？
- `bRestLengthFromUV`：来自 MD 版片语义的 rest 长度开关（见「与 Marvelous Designer 的对比」）——UV 到 cm 的比例从哪来还没定。
- 运动矢量与光追 BLAS（见风险）要不要做：前者要给顶点工厂加上一帧位置流，后者要按帧（或按形变量）触发 BLAS 重建。

# 帧配额调度计划：GPU 生成任务的统一分帧策略

本插件的 GPU 生成作业（地面派生链、房屋重判、藤蔓、体素化、布尔）各自决定"什么时候跑"，没有统一的帧内配额。本文盘点现状、给出一套通用的分帧设计，并列出各功能的接入方式与落地顺序。所有行号为 2026-09-09 现状。

**当前裁定（2026-09-09）**：先落 GPU mesh 的光追 BLAS 与异步回读，分帧调度暂不实施。本文作为后续实施的设计基线保留。

## 结论

- 现有的"异步"只摘掉了游戏线程那道 `FlushRenderingCommands`，渲染线程和 GPU 的每帧总量没有任何上限。GPU 是共享时间线，一帧里塞进去的工作再异步也要画完才能出图。
- 全插件没有帧时间预算。没有毫秒截止、没有每帧上限，也没有任何 GPU 时间信号。`Budget` 一词全指显存（`CSGpuMemoryBudget.h`），新设计用 **帧配额 FrameQuota** 命名以免混淆。
- 一套通用策略要补四件事：三种资源各自配额、一个调度入口、一种声明式的合并策略、一个能跨帧的 `UCSMesh` 编辑原语。
- 落地顺序按风险从低到高：地面家族先接（已异步、已池化），房屋改为只标脏，然后补编辑会话与统一回读泵，最后接 GPU 时间反馈。

## 现状盘点

### 在途策略：同一原语上的四种做法

| 位置 | 策略 | 后果 |
| --- | --- | --- |
| `UCSMesh::EditMeshAsync`，`CSMesh.cpp:1250` | 拒绝第二次请求，`OnComplete` 不触发 | 所有其它策略都建在这个原语上 |
| 房体 / 柱 / 藤蔓管，`CSHouseActor.cpp:1495` `:1754` `:1887` | 最新覆盖：暂存最新快照，完成回调再提交 | 正确，但每处各写一遍 |
| 地面涂抹，`CSGroundActor.cpp:256` | 保序排队：在途时整队留到下一帧 | 收笔时 `:245` 退化为阻塞 flush |
| 藤蔓容器，`GeometryEditorActor.cpp:3542` | 拒绝并丢弃已算完的 CPU 准备工作 | 10 到 12 ms 的准备白做 |
| 房屋子系统，`CSHouseSubsystem.cpp:128` `:161` | 按身份合并进 `TSet`，每 tick 全量清空 | 无每帧上限 |

被拒后的兜底在 `CSGroundActor.cpp:288` 与 `CSHouseActor.cpp:1547` 都是同步重做，也就是一次阻塞 flush。零阻塞纪律的安全网本身就是阻塞，且恰在负载最高时触发。

### "稍后做"的五种载体

| 载体 | 位置 | 取消语义 |
| --- | --- | --- |
| `FTSTicker` 单次 | `GeometryEditorActor.cpp:3268`、`ComputeShaderLandscapeRoad.cpp:271` | 无 |
| `SetTimerForNextTick` | `CSPointBrushActor.cpp:329`、`CSHouseResizeSelectionWatcher.cpp:98` | 句柄 |
| `SetTimer` 去抖 | `ComputeShaderShallowWater.cpp:396` | 清句柄重设 |
| 可 tick 子系统 | `CSHouseSubsystem.cpp:135` | 集合成员 |
| `OnEndFrameRT` 泵 | `CSMesh.cpp:435`、`GDFSampleService.cpp:106` | 自注册自注销 |

### 每帧总量无上限的三处

- 一笔涂抹的那一帧：一次上传（`CSGroundActor.cpp:280`）+ 四条派生链内联（`:298` 到 `:307`）+ 每栋房子在同一调用栈里同步重判（`CSHouseActor.cpp:264`，且忽略 `ChangedBounds`）。
- 藤蔓一次生成：一张图里三百多个 dispatch，SC 迭代循环在 `GeometryEditorActor.cpp:4232`，全部 SC buffer 是图内临时资源（`:4165` 到 `:4174`）。
- 房屋子系统 tick：`DirtyHouses` 一次清空，`ScanInterval = 0.25 s` 只限流检测不限流执行。

### 依赖顺序靠隐式手段维持

- 石阶与地被必须排在涂抹上传之后，靠渲染命令 FIFO（`CSGroundActor.cpp:295` 到 `:307` 的注释）。
- 岩壳必须与地面位移同一趟（`:728` 到 `:730`）。
- `ReevaluateSite` 的 11 步顺序只在注释里（`CSHouseActor.cpp:991` 到 `:1057`）。

### 可复用设施

| 设施 | 位置 | 在调度器里的角色 |
| --- | --- | --- |
| 计数 flush | `CSMesh.cpp:23`，`UCSMesh::CountedBlockingFlush` | 唯一允许的阻塞入口，`flushes=0` 断言的探针 |
| 回读泵 | `CSMesh.cpp:435`、`GDFSampleService.cpp:106` | 统一回读泵的两个现成样本 |
| 代数取消 | `ComputeShaderShallowWater.cpp:278` | 任务取消语义的模板 |
| 浅水按帧步进 | `ComputeShaderShallowWater.cpp:234` `:560` | 已经是"一帧一张图、状态常驻"的工作模板 |
| 显存代价模型 | `CSGpuMemoryBudget.h` `FTriangleSoupCostModel` | GPU 单元代价模型照此形状 |
| 内容哈希门 | `CSHouseActor.cpp:999` 到 `:1053`、`CSGroundActor.cpp:1292` | 幂等过滤，调度器不替代它 |

## 目标设计

### D1 三种资源分开配额

每帧三个额度，任务声明每一步消耗哪一种：

```cpp
struct FCSFrameQuota
{
	double GameThreadMs = 2.0;        // 游戏线程切片上限；编辑器交互下留给生成的份额
	int32  RenderGraphsPerFrame = 2;  // 每帧允许的自持 FRDGBuilder 数；每张图一次 fence 与 RDG 编译
	int64  GpuUnitsPerFrame = 0;      // GPU 工作单元；0 = 不限。单元由作业的代价模型声明
};
```

- GPU 单元第一阶段用声明式代价模型，形式同 `FTriangleSoupCostModel`：每个作业报三角数、格点数或迭代数。
- 第二阶段接反馈：每帧读 `RHIGetGPUFrameCycles()`（`DynamicRHI.h:1301`），超过目标帧时间就把 GPU 额度减半，低于则缓慢加回（AIMD）。
- 引擎自身的同类先例：`r.RayTracing.Geometry.MaxBuiltPrimitivesPerFrame`（默认 0 即不限）、`r.LumenScene.SurfaceCache.CardCapturesPerFrame`，都是按单元计数配额加优先级递增防饿死。

| 配额 | 起始值建议 | 依据 |
| --- | --- | --- |
| 游戏线程 | 2 ms | 编辑器交互下留给生成的份额 |
| 渲染线程 | 每帧至多 2 张自持图 | 每张图一次 fence 和 RDG 编译开销 |
| GPU | 先按单元；藤蔓一帧 8 次 SC 迭代 | 55 次迭代分 7 帧，视觉上仍是"立刻" |

### D2 一个调度器子系统

- 做成 `UTickableWorldSubsystem`，`UCSHouseSubsystem` 已是这个形态。
- 持有按优先级排序的任务表，每帧在配额内推进。
- 所有"稍后做"改为向它登记，取消统一靠代数计数（照 `SolverGeneration`）。
- 调度 tick 在输入处理之后运行；若晚于 EdMode 的 tick，一帧的 dab 留到下一帧，这正是保序排队今天已接受的延迟。

### D3 任务是可恢复的步进对象

```cpp
enum class ECSTaskStep : uint8 { Continue, Done, WaitingGpu, Failed };

struct ICSFrameTask
{
	virtual ECSTaskStep Step(const FCSFrameQuota& Quota) = 0;  // 每帧至多调用一次；自行判断能做多少
	virtual FName GetKey() const = 0;                          // 合并键，见 D4
	virtual TConstArrayView<FName> After() const = 0;          // 依赖边，见 D5
};
```

三类任务：

- CPU 切片任务：按工作项分块。`PrepareSurfaceVoxelInputs`（`GeometryEditorActor.cpp:3612`）遍历组件与三角，天然可切。
- GPU 步进任务：每步录一张有界的图。中间态必须住在池化 buffer 里，浅水的 RT 成员（`ComputeShaderShallowWater.h:149` 到 `:167`）是现成形态。
- 回读等待任务：永不阻塞，只在泵里轮询。回读泵合并现有的三个为一个，`GDFSampleService.cpp:106` 已是通用形态。

### D4 合并策略按任务键声明

只有三种，"拒绝"取消：

| 策略 | 语义 | 适用 |
| --- | --- | --- |
| `LatestWins` | 在途时新请求覆盖待办，完成后再跑一次 | 房体、藤蔓、四条地面链（默认） |
| `AppendQueue` | 保序追加，一次跑一批 | 涂抹 dab |
| `Once` | 已登记则忽略 | 幂等一次性操作 |

- 在途时的新请求变成脏标记，完成回调负责重新登记。兜底 flush 因此没有存在理由。
- 每个 `UCSMesh` 一次一个会话的串行约束保留，下沉为调度器内部的资源锁，调用方看不到拒绝。

### D5 依赖用边表达

- 任务键带 `After` 列表。地面链写成：涂抹上传之后是石阶和地被；位移之后是岩壳与其余三条。
- 房子的 11 步内部顺序保留为一个任务的子步骤，因为其中只有 GPU 提交是贵的。
- `ChangedBounds` 继续携带。`CSSceneDirty3D` 按裁定仍是零代码（`Docs/TinyGlade/Plan_House.md` §D3），区域过滤的接缝留着。

### D6 优先级三档加收尾语义

- 交互档：来自打开的 stroke 或 drag，先拿配额。
- 背景档：加载恢复、视野外的房子。
- 同档按到相机距离与等待帧数排序，等待帧数每帧递增防饿死。
- 拖拽结束不再阻塞：升到最高档并放开配额直到清空。
- 真正的同步只留给测试与烘焙：显式 `Drain()`，内部经过 `UCSMesh::CountedBlockingFlush()`。否则 `flushes=0` 的断言（`CSMesh.h:603` 到 `:618`）看不见回归。

### D7 `UCSMesh` 多帧编辑会话

这是唯一缺的原语。今天一次编辑等于一个 lambda、一张图、一次 Execute（`CSMesh.cpp:1195` `:1240`）。

```cpp
FCSMeshEditSession Session = Mesh->BeginEditSession();   // 持有在途标志，拒绝并发会话
Session.StepEdit([](FCSMeshEditContext& Ctx){ /* 一帧的量 */ });  // 每帧一张图，可多次
Session.End();                                            // access state 恢复、计数发布、OnMeshChanged 只在此处
```

- 消费者排在会话结束之后，靠依赖边而不是 FIFO 运气。
- `CSMeshOps.h:581` 那条"多个算子一次编辑"的规矩不变，它管的是一步之内。

## 各功能接入

| 作业 | 切分轴 | 中间态 | 阶段 |
| --- | --- | --- | --- |
| 地被 `Scatter`、石阶 `Scan`、岩壳 `Displace`、塑形步 | 网格瓦片或物种索引 | 已池化（`CSGroundCover.cpp:153`、`CSGroundStairs.cpp:123`） | 1 |
| 涂抹上传与四条链 | 已按帧成批 | 已池化 | 1 |
| 房屋 `ReevaluateSite` | 每帧 N 栋，近者优先 | 哈希门已在 | 2 |
| 浅水 | 已按帧步进 | 已在 RT 里 | 2，只登记进来共享配额 |
| 表面体素化 | 50 万三角一批（`ComputeShaderMeshGenerator.cpp:2405`） | 池化版已存在（`:2708` 到 `:2829`） | 3 |
| SC 藤蔓 | 迭代序号（`GeometryEditorActor.cpp:4283`） | 需把 State 与 Proposal 池化 | 3 |
| 光追 BLAS 构建 | 引擎自带 CVar | 无 | 3，只计入 GPU 配额 |
| 布尔 | BSP 已按 16384 分批（`ComputeShaderMeshBoolean.cpp:2341`） | 排列工作集需池化，Stage B 缺 `FragmentBase` | 4，烘焙路径不急 |

## 落地顺序

1. 调度器 + 合并 + 优先级 + 游戏线程配额，先接地面家族。用来验证依赖边替代 FIFO，同时删掉收笔阻塞。
2. 房屋改成只标脏，子系统每帧限量执行（`Review_Structure.md` §9 的同一建议）。删掉两处同步兜底。
3. 编辑会话 + 统一回读泵。藤蔓按迭代步进，体素化按批。
4. GPU 时间反馈：每个会话步骤打时间戳查询，接 AIMD 调节器。
5. 收尾债务：布尔的 TDR 切分；`ComputeShaderLandscape.cpp:347` `:453` `:951` 与 `ComputeShaderLandscapeRoad.cpp:360` 没走计数通道的裸 flush。

## 验收

- 脚本化一笔涂抹加 N 栋房子，`UCSMesh::GetBlockingFlushCount()` 增量为零。
- 每帧 GPU 时间不超过目标值，用 `ue-perf-run` 的交错 A/B 测量。
- 最老待办任务的等待帧数有上界。

## Open Questions

- GPU 单元与真实 GPU 毫秒的换算系数按机器不同，AIMD 能否在编辑器场景切换时快速收敛尚未验证。
- 编辑会话跨帧时，`BorrowedGraph` 不发布计数的限制（`CSMesh.h:344` 到 `:350`）是否需要放开。
- `CSGpuInstancedMeshComponent` 的实例化产物仍有每轮 2 次阻塞 handover（`CSHouseActor.cpp:2163`），不在本设计范围内。

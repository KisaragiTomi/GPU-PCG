# CSSceneDirty3D：TG 组件的世界级登记处与三维 Dirty 通道（设计稿）

给 Tiny Glade 那一族 actor（地面 / 房屋 / 墙 / 石阶 / 塑形物 / 样条块）补一层**世界级**（per-`UWorld`）的服务，回答两件事：

1. **世界级的派生数据放在哪**（墙表、建筑 mask 这类由多个 actor 共同写出来、别人都要读的东西）—— 登记处；
2. **哪里变了**（三维、体素语义）—— 脏 tile 通道，消费者按自己的游标拉取，只重算自己范围内变了的那部分。

状态：**设计稿，零代码**。全插件仍无 `UWorldSubsystem`（`UCSHouseSubsystem` 已于 2026-09-16 删除）。

> **2026-09-21 / 09-22 的两次转向**（本版据此重写）：
> - 09-21 用户纠正服务对象：本系统**主要为 TG 组件服务**（`ACSTinyGlade` 派生类之间的跨 actor 空间影响）；**CSSW、道路、藤蔓都不是它的消费者**。上一版围绕这三者排的消费侧与 M1–M4 作废（CSSW 那段的单腿化作为透明物修复已独立落地，挪到附录留档）。上一版"第四条路径：TG 链不接入本系统"方向是反的 —— TG 链今天的直推（`OnGroundChanged` + 根组件 `TransformUpdated`）是**过渡**，本系统是它的去向。
> - 09-22 用户提出"房子变成墙、subsystem 持有各种 buffer"。TG 的真实结构（见 §二）是**登记处 + 通道 + 订阅**三层，buffer 由写它的一方持有、订阅游标归消费者；本版照此分层。墙那条主线见 [`TinyGlade/Plan_Wall.md`](TinyGlade/Plan_Wall.md)。

## 一、背景：今天的 TG 链怎么通知

| 发布 | 消费 | 问题 |
| --- | --- | --- |
| `ACSGroundActor::OnGroundChanged(Ground, ChangedBounds)`（落笔 / 全量重建 / 拖动，5 处广播）+ 类级 `OnAnyGroundRebuilt` | 房子、楼梯、样条墙各自 `HandleGroundChanged` → 标脏，下一帧 Tick 重求值；无效唤醒靠幂等哈希吸收 | 变更盒一直带着但**没人用**：一笔画路会叫醒全场每一栋房子、每一面墙，各自全量重求值一遍才发现"与我无关" |
| 各 actor 根组件 `TransformUpdated` | 只叫醒自己 | 搬一栋房子，旁边的灌木、邻居的接缝、楼梯的露台入口靠各自另外的通知（`PublishFootprintToGround`、`OnTerraceChanged`、接触记录） —— 每一对关系一条专线 |
| 房子 `PublishFootprintToGround` → 地面 `BuildingFootprints` | 地面的灌木链 | 地面既当登记处又当消费者；灌木每次**整块地面重撒** |

规模一上去（512 m 演示地面、World Partition 下无界），"全量重求值 + 哈希短路"与"每对关系一条专线"都撑不住。缺的是**跨 actor、带区域过滤**的那一层。

## 二、TG 是怎么做的（2026-09-22 PDB 泛型签名坐实）

TG 没有"一个 dirty 系统攥着所有 buffer"：

| 数据 | 持有者 | 变化怎么传 |
| --- | --- | --- |
| 路（4 种）/ 花园 / 水的笔刷栅格 | 各自一个 `WorldRaster<512>` 资源 | 栅格自带 `DirtyTilePublisher`，对外 `WorldRaster::subscribe()` |
| 地形高度、灌木位置、碰撞世界 | `TerrainHeightsData`、`BushGrid`、`RaycastWorld` | 同样可订阅（碰撞那条是 `ColliderDiff` 频道） |
| 墙 | `InnerWalls`（私有）→ `publish_public_wall_state` → `PublicWalls`（公开快照） | `OnWallChanged` / `OnWallDeleted` 事件 |
| GPU 缓冲 | `AssetSsboLibrary`，按类型登记 | — |
| 建筑 mask 等派生图 | 渲染循环里的 pass | 下游当帧采样 |

订阅游标都在消费者手里：`AutoClutterDirtySubscriptions`（6 个源）、`OverhangSupportDirtySubscriptions`（地形 + 碰撞 —— 墙系统的悬挑立柱就是地形 dirty 的消费者）、`WallLazySubscriber` / `StairsLazySubscriber` / `BirdNestLazySubscriber`。发布 / 订阅用 `tabloid` crate，全库只有两种频道：`DirtyTiles` 与 `ColliderDiff`。

搬到 UE 的差别只有两处：

- TG 的资源是 ECS 世界单例，UE 的对应物是 `UWorldSubsystem`；但 **subsystem 不随关卡存盘、没有撤销、没有细节面板** ⇒ 作者数据（墙形状、地面涂色镜像）必须留在 actor 上，subsystem 里只放**派生**数据与引用。
- TG 是每个 buffer 一个发布者；本系统 08-21 定的是**一条通道、入口不分源头**（§四）。单通道只会多标，按"只许过标"的铁律只影响性能，不推翻那条裁决。

## 三、三层职责

```text
   作者数据（actor，存盘 / 撤销）            世界级 subsystem（UCSSceneDirtySubsystem，不存盘）
   ─────────────────────────────           ───────────────────────────────────────────────
   ACSGroundActor  镜像高度 / 涂色  ──发布──▶ ① 登记处  墙表 · 建筑 mask · 地面镜像的引用 …（每项一个写者）
   ACSHouseActor   footprint / 屋顶 ──发布──▶ ② 通道    MarkDirty*() → 帧内合并 → 批次(Generation) 进环
   ACSWallActor    样条           ──发布──▶ ③ 订阅    消费者自持游标 Pull(LastGeneration)，自己的 Tick 里取
   ACSStairsActor  样条           ◀──读 / 拉──
```

1. **登记处**（对应 `AssetSsboLibrary` + `PublicWalls`）：
   - 放两类东西：多个 actor 共写出来的世界级数据（墙表 `FCSPublicWallState`、建筑 mask）；单一 actor 持有的 buffer 的**引用**（地面的高度与涂色），消费者按类型查，不再 `TActorRange` 扫。
   - 规矩只有一条：**每项只登记一个写者**，别人只读。写者变了先注销再登记。
2. **通道**：入口只收体素（§四）；帧内按位或合并，每帧 flush 成一个带 `Generation` 的批次进环形缓冲。
3. **订阅**：游标归消费者（一个 `uint32 LastConsumedGeneration`，transient）；环溢出 ⇒ 该消费者退化为全量。
4. **不做调度**：subsystem 不替任何 actor 决定何时重建。消费者在自己的 Tick 里拉取 —— 与"房子自己 Tick、本帧标脏下一帧重建"的裁决（2026-09-10 / 09-11）一致；配额调度（[`FrameQuotaScheduler_Plan.md`](FrameQuotaScheduler_Plan.md)）若落地，拉取点登记成它的任务，不另起一套。

⚠️ **登记处会推翻 2026-09-16 的一条裁决**："房屋名单 = `TActorRange`、无登记表"。墙表就是登记表；灌木用的 `BuildingFootprints` 其实已经在地面上开了一张小登记表。TG 的做法也是先有登记表（`PublicWalls`、`ColliderRegistry`）才有频道。**动手前请用户确认。**

## 四、通道：体素、砖块、tile

入口语义不变（08-21 两轮裁决）：**源头只有体素**。发布方只回答"哪些体素变了"，不带来源、通道、事件盒；mesh 的增删移改在发布侧体素化。铁律：**只许过标，不许漏标**。

三级粒度（09-21 新增 tile 那一级，与 08-21 的 4×4×4 打包不冲突 —— 那一级改名"砖块"）：

| 级 | 尺寸 | 用途 |
| --- | --- | --- |
| 体素 voxel | `csd.VoxelSize` 默认 100 cm | 唯一语义原子；key = `FloorToInt(World / VoxelSize)`，世界锚定、无界 |
| 砖块 brick | 4 × 4 × 4 体素 = 400 cm | 传输打包：一条标记 = `(BrickKey, uint64 VoxelMask)`，64 bit 一一对应砖块内 64 个体素（原稿叫 tile） |
| tile | **World Partition 网格的 1/4**：UE 5.7 `RuntimePartitionLHGrid::CellSize` 默认 25600 cm ⇒ 边长 6400 cm（1 个 WP 格 = 4 × 4 个 tile），Z 同尺寸分层 | 订阅与调度的单位：消费者先按 tile 与自己的范围求交（粗筛），命中再展开到砖块；debug RT 按 tile 画格线；环溢出按 tile 计数 |

> ⚠️ tile 尺寸按"WP 格**边长** ÷ 4"理解（64 m）。若本意是"面积 ÷ 4"，就是边长 ÷ 2 = 128 m —— 只改一个常量 `csd.TileSize`，其余不动。**待用户确认。**

- tile 内有 16 × 16 × 16 个砖块。标记按 tile 分组：`(TileKey, TArray<(BrickIndex uint16, VoxelMask uint64)>)`，一个 tile 满载 4096 条，实际稀疏。
- 一笔 20 m 长的路 × 墙高 3 m ≈ 60 个体素列 ≈ 十几个砖块 ≈ 1 个 tile：标记量可以忽略。

```cpp
// 入口：唯一语义是体素。Box / Mesh 入口是体素化便捷封装，进中枢前已变成体素。
void MarkDirtyVoxels(TConstArrayView<FIntVector> VoxelKeys);
void MarkDirtyBox(const FBox& WorldBox);                  // AABB 保守体素化（覆盖全标）
void MarkDirtyPolygon(TConstArrayView<FVector2D> WorldXY, double MinZ, double MaxZ);   // 折线外皮 × Z 段（墙 / 房子用）

struct FCSDirtyBrickMark { uint16 BrickIndex = 0; uint64 VoxelMask = 0; };
struct FCSDirtyTileMarks { FIntVector TileKey; TArray<FCSDirtyBrickMark> Bricks; };
struct FCSDirtyBatch     { TArray<FCSDirtyTileMarks> Tiles; uint32 Generation = 0; };

struct FCSDirtyPullResult
{
	TArray<const FCSDirtyBatch*> Batches;   // Generation > 游标 的批次
	uint32 LatestGeneration = 0;
	bool   bEventsLost = false;             // 环溢出：该消费者全量重建
};
```

- 环容量按砖块标记计：`csd.RingMarkCapacity` 默认 65536 条（≈ 0.7 MB）；超限 ⇒ `bEventsLost`，系统永远只是加速器，语义等价于全量。
- `csd.VoxelSize` / `csd.TileSize` 运行期改动 = 清环 + `Generation` 跳变 ⇒ 所有消费者全量。
- 卸载（WP 流出）不是几何变更，**不标脏**；消费者只信任已加载区域。

### 为什么是三维

TG 组件天然是摞起来的：房子摞房子（D7 横缝）、屋顶露台上接楼梯、墙砌在露台上、塑形物抬起的台地上再放房子。二维 XY 标记会让"改上房的屋顶"去叫醒下房的藤、脚下的灌木；Z 不相交的脏体素可以整块剔除。

## 五、发布侧：TG 组件各自什么时候发什么

| # | 发布者 | 什么时候 | 发什么 | 现状钩子 |
| --- | --- | --- | --- | --- |
| P1 | 地面 | 落笔 / 收笔 / 塑形物重算 / 全量重建 | `OnGroundChanged` 的 `ChangedBounds` → `MarkDirtyBox`（Z 取变更前后高度的并） | 5 处广播已在，接入 = 回调里加一行 |
| P2 | 房子 | 摆位或形状**真的变了**（哈希守卫之后） | 旧外皮 ∪ 新外皮 × [房底, 屋脊] → `MarkDirtyPolygon` | `ReevaluateSite` 末尾（今天 `OnTerraceChanged` 的位置） |
| P3 | 样条墙 | 路径真的变了 | 旧 ∪ 新两条皮线围成的带 × [墙脚, 墙顶 + 垛口] | `ACSWallActor::ReevaluateSite`（`BodyHash` 变了那一支） |
| P4 | 楼梯 / 塑形物 / 样条块 | 同上 | 各自产物的旧 ∪ 新包围 | 各自重建函数的哈希守卫之后 |
| P5 | 手动 | 调试 / 脚本 | 体素或盒 | BlueprintCallable + Python |

- **只在产物真的变了时发**（各家已有的哈希守卫之后）：消费者重算后如果产物没变就不再发，自环一轮即收敛，不需要上一版为道路 / CSSW 设计的 Self-Mask。
- 拖动期间照常发（每帧位或合并，重复标记零成本）；消费者按自己的节奏拉。

## 六、消费侧：TG 组件订阅什么

通用循环（消费者自己的 Tick 里）：

```cpp
const FCSDirtyPullResult R = Subsystem->Pull(LastConsumedGeneration);
if (R.bEventsLost) { FullRebuild(); LastConsumedGeneration = R.LatestGeneration; return; }
for (const FCSDirtyBatch* B : R.Batches)
	for (const FCSDirtyTileMarks& T : B->Tiles)
	{
		if (!TileBox(T.TileKey).Intersect(MyBounds)) continue;           // tile 级粗筛
		for (const FCSDirtyBrickMark& M : T.Bricks) AppendHits(T.TileKey, M, MyBounds, Hits);   // 砖块 / 体素级
	}
LastConsumedGeneration = R.LatestGeneration;
if (Hits.Num()) PartialUpdate(Hits);
```

| 消费者 | 订阅范围 | 命中后做什么 | 替掉今天的 |
| --- | --- | --- | --- |
| **建筑周边灌木**（地面，首个用例） | 地面矩形 | 只重撒命中 tile 里的灌木（按 tile 分块的实例段） | 整块地面重撒；房子直推 `BuildingFootprints` |
| 房子 | 外皮 × [房底 − 余量, 屋脊] | 落座、门洞（路权）、柱、藤的地面空隙 | `OnGroundChanged` 无条件叫醒 |
| 样条墙 | 两条皮线的带 × [墙脚, 墙顶] | 贴地、藤的地面空隙 | 同上 |
| 楼梯 | 样条的包围 | 贴地、露台入口 | `OnGroundChanged` + `OnTerraceChanged` |
| 接缝 / 接触（D7） | 外皮 | 邻居变了才重算接触 | 接触记录的专线通知 |
| 地被 / 石阶 / 岩壳（地面自己的派生链） | 地面矩形 | 按 tile 局部重算 | 各自的全量重建 |

### 首个用例：建筑周边灌木

TG 里这条链是"生产者 ≠ 所有者"的样板：房子（其实是墙）只把形状写进 `GroundedBuildingsSsbo`，渲染循环光栅化成建筑 mask，花园 compute 读 mask 撒灌木 —— 灌木归花园系统，不归房子也不归地面（逆向细节见 memory `tg-walls-publish-dirty-tiles` 与 `TinyGlade/Plan_House.md` 的灌木节）。

落到本系统：

1. 墙表进登记处（[墙化计划第 3 步](TinyGlade/Plan_Wall.md#第-3-步墙表与世界级登记处)）；房子 / 围合的墙发布外皮。建筑 mask（或今天的"逐栋有符号距离"）从墙表派生，写者 = 地面的灌木链自己。
2. 房子摆位变了 ⇒ P2 发旧 ∪ 新外皮 ⇒ 灌木链只重撒命中的 tile。
3. 验收：搬一栋房子，只有它周围一两个 tile 的灌木实例段被重写（回读断言其余 tile 的实例逐位不变）。

## 七、Debug RT：一张图看 dirty（09-21 用户要求）

一张顶视 `UTextureRenderTarget2D`，给编辑器里直接看、也给脚本回读断言：

| 项 | 取值 |
| --- | --- |
| 资产 | `/PCGPlugins/Debug/RT_CSSceneDirty`（RGBA8，512 × 512），`csd.DebugRT 1` 时 subsystem 每帧写；可在内容浏览器双击直接看 |
| 覆盖 | 以相机为中心、对齐到 tile 网格的 512 m 窗口（= 默认演示地面的范围），1 texel = 1 m（一个体素列） |
| R | 最近一次被标脏的新鲜度：刚标 = 1，`csd.DebugTTL`（默认 5 s）内线性褪到 0 |
| G | 还有订阅者没拉走 = 1（pending），全部拉走 = 0 |
| B | 网格线：tile 边界 0.5、WP 格边界 1.0 |
| A | 这一列脏体素的最高层（归一化到窗口 Z 范围）—— 看得出"改的是地面还是屋顶" |

- 视口叠加：`csd.DebugRTOverlay 1` 把它画在视口左下角（Canvas），不需要开资产。
- 三维细节仍用 `csd.Debug`（tile / 砖块盒的 `DrawDebugBox`，颜色按批次年龄）与 `csd.Stats`（HUD：`Generation`、环占用、各订阅者 lag / lost）。
- 录制回放：`csd.Record 1` 写 `Saved/CSSceneDirty/marks_<日期>.jsonl`，`csd.Replay <file>` 回放 —— "这串编辑为什么叫醒了那么多东西"类问题的复现手段。

## 八、里程碑

| 阶段 | 内容 | 退出判据 |
| --- | --- | --- |
| M0 | subsystem 骨架：登记处（空）+ 通道（体素 / 砖块 / tile 三级、环、游标、溢出）+ `MarkDirtyBox` / `MarkDirtyPolygon` + debug RT + `csd.Stats` | 编辑器画路：RT 上那一条变红、几秒后褪掉；`Generation` 单调；溢出语义正确 |
| M1 | 发布点 P1–P3（地面 / 房子 / 样条墙） | 画一笔、搬一栋、拖一面墙，RT 上亮起的恰好是变化区域（回读断言：亮起的 texel ⊆ 旧 ∪ 新包围） |
| M2 | 首个消费者：建筑周边灌木按 tile 局部重撒；墙表进登记处（墙化计划第 3 步同步） | 搬一栋房子只重写周围 tile 的灌木实例段 |
| M3 | 房子 / 样条墙 / 楼梯从直推改拉取（`OnGroundChanged` 退化成 P1 的一个发布点） | 画一笔路，只有路经过的房子重求值（`GetReevaluateCount` 断言）；演示回归 FAIL 集不变 |
| M4 | 接缝 / 接触改订阅；地面派生链（地被 / 石阶 / 岩壳）按 tile 局部重算 | 各链局部结果与全量同帧逐位一致 |
| M5 | 编辑器通用监听（任意 mesh 增删移 → `MarkDirtyMesh`，AABB 档）；大 mesh 精确体素化 | 拖普通 mesh 进 TG 场景，相关 TG 组件一个拉取周期内局部刷新 |

## 九、风险

- **漏标是唯一致命错误**：任何拿不准的都按保守超集标（外皮取凸包、Z 取整列）。
- **登记处的写者纪律**：两个 actor 同时认为自己是某一项的写者 ⇒ 互相覆盖且不报错。登记时查重、冲突即告警。
- **与合批 / 本帧标脏的配合**：发布在哈希守卫之后（下一帧重建那一刻），消费者再下一帧拉 ⇒ 链路每层多一帧延迟（用户 09-11 已接受"延迟一帧视觉无差别"）。
- **撤销**：事务恢复不走重建路径时发布点收不到；保底是各 actor 的 `PostEditUndo` 全量重求值 + 发布。
- **多 world**：subsystem 天然 per-world；编辑器 world 与 PIE world 互不相通。
- **WP 流送**：流入的 actor 首次重求值时全量发布一次；流出不标脏。

## 十、Open Questions

- tile 尺寸"WP 格的 1/4"按边长还是按面积（§四）。
- 登记处与墙表推翻"无登记表"那条裁决（§三）—— 需要用户拍板。
- 建筑 mask 要不要真的光栅化成一张图（TG 做法：130 m 顶视 mask + 模糊 + `.y` 生长动画），还是继续"逐栋有符号距离"。房子上百栋之前后者更便宜也更精确；生长动画需要状态时再改。
- 楼层 Z 的订阅粒度：房子订"外皮 × 整高"，还是按楼层切段。先整高，摞房多了再细分。

---

## 附录：上一版的非 TG 消费者（已退出本系统）

- **CSSW 道具捕获单腿化**（2026-09-20 已实施，作为透明物修复**独立成立**）：`bCaptureWithRenderer`（原 `bCaptureNaniteWithRenderer`，`Config/DefaultPCGPlugins.ini` 配 `PropertyRedirects`）打开时，所有拍得到的道具都走引擎 depth pass（Translucent 不写深度、Masked 逐像素 clip、WPO 与画面一致）；拍不到的（隐藏 / 无代理 / 关了 `bRenderInMainPass`·`bRenderInDepthPass`）逐组件退回三角形路径（判据 `IsCapturableComponent`）。代价：高度图不再逐位确定（LOD / cull distance / WPO 归渲染器）。`CSNaniteHeightCapture` 的 pass 本就按任意 texel 子矩形工作，`CaptureRegion` 不再是目标。
- **道路 / 藤蔓**：自身的全量重建与"改场景后不知道"的正确性问题仍在，但不由本系统解决（09-21 裁决）。上一版为它们设计的 Self-Mask、藤蔓 cell 级缓存（`FCSMeshGeneratorVoxelCacheState`，体素 1:1）、道路只重贴地分路，需要时另立项。

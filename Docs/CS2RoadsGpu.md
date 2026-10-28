# CS2 道路逻辑的 GPU 移植（CSRoadNet）

`CSRoadNet` 模块把 Cities: Skylines II 的道路生成逻辑搬进 UE。它从路网快照（节点、边、曲线、prefab）或关卡里画的样条路出发：CPU 选组合、拼组合网格；GPU 跑 `GeometrySystem` 的几何 job、算组合参数、统一路口接缝、细分节点网格，再把组合网格变形到曲线上，结果直接写进 `UCSMesh` 渲染。标线、胎痕和横道（RoadLab 的贴花）由 CPU 遍历车道图生成条目，GPU 把它们投影或裁剪到刚建好的路面上，写进第二个 `UCSMesh`。它与 [Palmsland 道路关卡](CS2Roads.md) 的静态网格导入相互独立。

原版代码来自 `D:\MyProject\CitiesSkylines-Road2-C#` 的 `Port/Original`（反编译源，逐字节可复现）。C# RoadLab 自己写的部分按 RoadLab 移植，并在下文注明，包括 `RoadWorld.Build` 的流程、`RoadMeshes.Deform`、`RoadJunctionSurface`、`RoadMeshRefinement`、`RoadLanes`、`RoadCrosswalks` 和 `RoadDecals`。

## 用法

- 关卡里放 `ACSRoadNetworkActor`，`NetworkFile` 指向 `cs2-road-network-v1` 快照，例如 `D:\MyProject\CitiesSkylines-Road2-C#\Artifacts\public-saves\palmsland-3\network.json`。
- 要用样条画路，把 `Source` 设为 `Splines`，在 actor 下挂 `ACSRoadSplineActor`，一根样条是一条路，见[样条路网](#样条路网)。`ConvertToSplines` 把快照转成这样的样条路。示例关卡 `/PCGPlugins/CSRoadNet/L_SplineRoadDemo` 由 8 根样条组成，打开后拖样条点即可看到路网实时重建。Palmsland 整张图转好的样条在 `/PCGPlugins/CSRoadNet/L_PalmslandSplines`，见[从快照转换](#从快照转换)。
- 城市局部更新：`bLocalUpdate` 让路网分 tile 重建，见[局部更新](#局部更新)；样条路分大路和小路，见[大路与小路](#大路与小路)；特殊建筑、小路生成和批量补房见[城市布局](#城市布局)。示例关卡 `/PCGPlugins/CSRoadNet/L_CityDemo`。
- 改属性、移动松手、关卡加载或复制 actor 时自动重建，也可以点 `Rebuild`。常驻网格不存盘，加载后重新生成。
- `AssetDirectory` 留空时读 `<CS2ROADLAB 或 D:/MyProject/CitiesSkylines-Road2-C#>/Assets/CS2` 下的 `prefabs.json`、`raw-prefabs.json`、`pieces.bin`。同一目录每个进程只加载一次。
- 材质按 atlas 的 BaseColor 贴图名（目录里的 `material_bindings`）在 `MaterialFolder` 找 `M_CS2_<NN>_<BaseColor>`，这些材质由 `Scripts/ImportCS2Roads.py` 生成。找不到的 atlas 列在 `MissingAtlases`，改用 `FallbackMaterial`。
- `bBuildMarkings`（默认开）在 `MarkingMesh` 组件里生成贴花。贴花材质同样按表面的 BaseColor 找 `M_CS2_*`，找不到的表面列在 `MissingMarkingSurfaces`。`AssetDirectory` 里没有 BaseColor 贴图的表面像 RoadLab 一样跳过。Palmsland 的贴花网格要约 690 MiB 显存，`UCSMesh` 只按空闲显存的 70% 分配；装不下时照样建出路面，`LastError` 写明贴花没建成的原因。
- 属性含义写在 `Source/CSRoadNet/Public/CSRoadNetworkActor.h` 的声明注释里。

### 车流预览与桥墩

主 Actor 的 **CS2 Road → Traffic → Show Traffic** 打开后显示车流；关掉会隐藏并暂停车辆，再打开从原位置继续，不重建道路网格。`Traffic Vehicle Count`、`Traffic Speed`（km/h）、`Traffic Gap`（m）和 `Traffic Seed` 控制数量、速度上限、车距和随机种子。`Reset Traffic` 用同一种子重新布车。`Traffic In Editor` 控制编辑器视口里的动画；编辑器须开启实时刷新。关闭标线仍可显示车流，重建道路后会重新生成车道图与车辆。

车辆使用现有的有向车道曲线与合法路口连接，按 30 Hz 固定步长更新。跟车检查跨越边界的前车与车尾；路口用保守的独占预约避免交叉穿车，出口堵住时在入口等待。带信号灯的节点按进入道路轮换 8 秒绿灯、2 秒清空期。普通车速还受道路 prefab 限速约束，连接段上限 8 m/s。默认车体是实例化的简易车身和车顶，可用 `Traffic Vehicle Mesh` 换成 +X 朝前的车辆网格，并指定 `Traffic Material`。

这是车流可视化预览，尚未移植 CS2 的市民出行、目的地寻路、换道、公交或服务车辆。环岛连接仍受现有 `FRoadLanes` 限制，数量显示在 `Unsupported Traffic Roundabouts`；共享双向车道和专用车辆车道不会生成预览车辆。

**CS2 Road → Bridge → Build Bridge Piers** 为 Elevated 且非 Tunnel 的道路生成柱身、盖梁和埋入地面的基础。`Bridge Pier Spacing`、`Bridge Pier Min Height`、`Bridge Pier Width` 使用米，`Bridge Pier Material` 可覆盖默认材质。支撑沿道路弧长布置，底部优先读取可碰撞静态场景的地面高度（`Trace Bridge Ground`），否则使用快照高度图，无高度图时使用 Actor 局部 Z=0。柱顶连接组合网格的底面；低净空段与路口端部不放柱。

基础会避开下穿道路的完整宽度，并在一个跨度内尝试调整位置；无法放置的数量显示在 `Skipped Bridge Piers`。该计数也包括高度不足的候选位置。生成组件是临时数据，重建会清除旧实例。桥墩目前是可视化构件，不提供碰撞或结构承载计算。

车流与桥墩的核心回归测试直接编译生产头文件：`Scripts/CS2Road/RunTrafficBridgeTests.ps1`（MSVC 14.38 / Windows SDK 10.0.22621，与本机 Rider 构建工具链一致）。编辑器验证脚本为 `Scripts/CS2Road/ValidateTrafficBridge.py`；启动方式写在文件头，结果写入 `Saved/CodexTrafficBridge/editor-validation.json`，测试关卡不保存。仅核心测试或单文件编译通过不代表编辑器验证已通过。

2026-10-27 验证：Rider `Development_Editor / x64` 对应的 Editor 与 ShaderCompileWorker 目标增量构建、链接通过；核心回归通过。独立编辑器的 30 项检查通过，测试路网为 12 条边、46 条可行驶车道/连接、32 辆车、12 个桥墩、0 个跳过支撑。验证覆盖显示开关、暂停/恢复、重复种子、道路重建、标线独立性、静态地面射线、自定义车辆网格切换；4 张截图保存在同一 `Saved/CodexTrafficBridge` 目录并已检查。核心布局性能样例：6,000 条合成道路生成 18,000 个桥墩约 10 ms，不含 UE 射线和渲染开销。编辑器启动仍有其它模块原有的结构体未初始化警告，不影响本次脚本结果。

## 样条路网

`Source` 为 `Splines` 时，路网来自样条，`NetworkFile` 只提供地形；不设文件就以 y = 0 为地面。`CSRoadSplineNetwork.cpp` 把样条转成快照的形式（节点表、按下标引用节点的边），后面的流程与读文件相同。结果与样条收集的先后无关：样条先按自身内容排好再处理，输出的边也按内容（曲线、prefab、升级、高程、有没有 owner）排序。几何 job 遇到平局、累加时都按边的顺序，所以同一组路总是按同一个顺序建。

- 来源：挂在该 actor 下的 `ACSRoadSplineActor`、该 actor 自己的 `USplineComponent`，以及挂在它下面的其它 actor 上的 `USplineComponent`（prefab 用 `DefaultSplinePrefab`）。关卡里只有一个样条路网时，没挂在任何路网下的样条路也归它。
- 每个样条点是一个节点，每段是一条边。`USplineComponent` 的一段是三次 Hermite 曲线，正好等于一条三次 Bezier：`b = P0 + Leave/3`，`c = P1 - Arrive/3`。类型为 `Linear` 或 `Constant` 的点，后面那段按直线处理。
- 编辑样条只设一个标记，路网在下一次 `Tick` 里统一重建一次，所以拖动时每帧都会重建。拖点、增删点、拖动样条 actor 或路网 actor、改属性或逐点数据、撤销，以及代码里改样条（`UpdateSpline`），都算编辑。示例关卡暖重建约 30–50 ms；转换出来的 Palmsland 每次约 1 s，拖动会很卡，开 `bLocalUpdate` 分 tile 后挪一个点约 0.15 s（见[局部更新](#局部更新)）。
- 编辑器只在有实时视口时才 tick actor。开着「Use Less CPU when in Background」（默认开）时，编辑器在后台会关掉所有视口的实时刷新，路网要等切回编辑器才重建。用脚本驱动后台编辑器验证时，加 `"-ini:EditorSettings:[/Script/UnrealEd.EditorPerformanceSettings]:bThrottleCPUWhenNotForeground=False"`，只对这一次会话生效，不会写回设置。

开着 `bAdjustSegments` 的样条按 `SplineSettings` 调整：

| 规则 | 设置（默认） | 做法 |
| --- | --- | --- |
| 共用节点 | `SnapDistance`（1 m） | 距离内的点并成一个节点；路的尽头离另一条路不到这个距离时接上去，形成 T 形路口 |
| 交叉 | `JunctionHeight`（2 m） | 水平面上相交、高差不超过它的两条路在交点切开，接成十字路口，节点取两者高度的中点；高差更大时一条从另一条上方跨过 |
| 合并 | `MinSegmentLength`（8 m） | 两端距离小于它的段并入相邻段，前提是中间那个点只是同一根样条上的普通弯：只连着这根样条的两条边、方向一致、点上没有自定义数据 |
| 细分 | `MaxSegmentLength`（96 m）、`MaxSegmentTurn`（30°） | 过长或转向过大的段按长度等分，每份不短于 `MinSegmentLength` |

合并按两端的直线距离判断，不按弧长：UE 自动切线下，3 m 的一段可能绕出约 15 m 的弧。两端落在同一个节点上、或调整后两端相距不足 0.1 m 的段会被去掉。

关掉 `bAdjustSegments` 的样条（转换出来的样条都是这样）每个点原样成为节点，不切分、不合并，也不与别的路形成路口，只和 1 cm 内的点共用节点。

逐点数据存在 `UCSRoadSplineComponent` 的 `RoadMetadata`（`UCSRoadSplineMetadata`，随加点、删点增删），含义见 `CSRoadSplineActor.h` 的声明注释。没设的值按 RoadWorld 的做法定：节点 prefab 取在此相交的道路里 `m_NodePriority` 最高的；高程从地面量起，节点在它的位置量，边在中点（t = 0.5）量。

`L_Palmsland` 的地形是 RoadLab 导出的显示网格（`App/TerrainRenderer.cs`）：高度图整体压低 0.18 m，只在快照自己的道路下再压到路面以下 0.3 m。CS2 在游戏里同样会在路下压平、裁掉地形（`FlattenTerrain`、`ClipTerrain`）。地面道路的路面比曲线低，所以在没有路的地方贴着高度图画路时，路面会被这张网格盖住，只露出人行道。目前的办法是把样条抬高约 0.4 m，`SplineNetworkShots.py` 就是这样画的；要根治，得按新路压低或挖开地形。

### 从快照转换

`ConvertToSplines` 把 `NetworkFile` 的路网转成挂在该 actor 下的 `ACSRoadSplineActor`，再把 `Source` 改为 `Splines` 并重建，整个操作可以撤销。`ChainNetwork` 把路继续延伸的边串成一根样条：prefab 和升级相同，下一条边从上一条的终点出发，在路口只接转角 30° 以内、最直的那条。

转换保留重建结果所依赖的数据：

- 节点偏移 `NodeOffset`：CS2 的边端可以不在节点中心。Palmsland 里最远的离节点 40 m，中位数 1.78 m。
- 出城连接：GPU 几何 job 会读。
- 有没有 owner（`bNodeOwned`、`bSegmentOwned`）：GPU 几何不压平有 owner 的节点和边端。owner 是谁不保留，样条路网里有 owner 的节点和边共用一个 owner。owner 是谁只在两处起作用：owner 本身是一条边（CS2 的子路网，读快照时也不再支持，遇到会警告），以及只有轨道车道的边在节点上比较顶层 owner。Palmsland 两种都没有。

边的顺序不保留，样条路网的边按内容排序，与快照文件里的顺序不同。几何 job 遇到平局、按边的顺序累加时，结果因此略有出入。Palmsland 转成 3,894 根样条后重建，与读文件时比：5,667 条边、4,576 个节点的数据和全部组合都相同；GPU 几何 132.6 万个值里 52 个偏差超过 1 cm，最大 2.7 cm，没有超过 0.1 m 的。把文件里的边按内容排序、倒序或打乱再建，偏差完全一样，可见这是边序本身带来的。不保留有没有 owner 时，那组 owner 附近有 61 个值偏差超过 0.1 m，最大 0.47 m。打乱样条的先后再建，得到的边和节点逐个相同。转换后暖重建约 1.0–1.1 s，与读文件相当。

Palmsland 已经转好存成关卡 `/PCGPlugins/CSRoadNet/L_PalmslandSplines`（`Scripts/CS2Road/MakePalmslandSplines.py` 生成，29 MB）。一个路网 actor 下挂 3,894 根样条，路的数据都在样条点上；`NetworkFile` 仍指向快照，但只提供地形：路面顶点贴着它，新画的路按它量高程。路网按 256 m 的 tile 局部更新，打开关卡后每帧写 16 个 tile，约 50 帧写完。整张 Palmsland 一次建完要约 1,500 万个路面顶点和 1,000 万个标线顶点，显存紧张时标线会分配失败（10-28 生成关卡时遇到过），所以关卡默认分 tile。重新加载后，样条与现场转换的结果逐个相同；只建路面时整网 14,755,860 个顶点，比读文件少 24 个，来自边序不同。

## 局部更新

`bLocalUpdate` 打开后，路面和标线按 `TileSize` 的方格分 tile 写，每个 tile 一对渲染组件（`RoadTile_*`、`MarkingTile_*`），组件和网格都不存盘。一条路归它曲线中点所在的 tile。流程在 `ACSRoadNetworkActor::BuildTiles` 和 `CSRoadTiles.cpp`：

1. 每条路按输入算一个指纹：prefab、曲线、高程、升级、owner，以及两端节点的数据（prefab、位置、旋转、高程、升级、出城连接、owner）。路和节点都没有编号：快照里的 id 读入时就换成了下标，样条路网里有 owner 的都用同一个 owner，所以一处编辑不会让别处的路换指纹。与上次相比，指纹新增或消失的路就是改动的路。离它们两圈以内的路是受影响的路；删掉的路经由它原来的节点和 owner 算进来。两条路共用一个节点，或者 owner 相同（路自己的或两端节点的 owner），就算相隔一圈。
2. 用受影响的路再加两圈建图（检测子图），跑 GPU 几何并读回。一条路的组合与 GPU 几何只取决于两圈以内的路（`Subgraph.Reach` 抽样确定，`Subgraph.Tiles` 对 Palmsland 全部路逐条核过），所以受影响的路在子图里的结果与整网相同。子图按规格原来的顺序取子集，节点数组整份传入；实体编号只比较是否相等，编号不同不影响结果。
3. 受影响的路各算一个输出指纹：输入指纹，加上读回的边与两端节点几何，以及三个实例的组合。输出变了、新增或删掉的路，在它们节点上相接的路所在的 tile 都要重写。tile 尺寸、左右舵、标线开关、actor 变换或材质设置变了，全部重写，这时子图就是整网。
4. 本批 tile 里的路，加上节点上与它们相接的路（贴花接收面、车道图要用），再加两圈，另建一个绘制子图；检测子图已经包含这些路时直接用它。车道图、细分计数和各 tile 的网格都取自这个子图：脏 tile 的实例合在一张图里算细分计数，之后逐 tile 只为本 tile 的路建组合网格、写路面和标线。节点上相邻路的边和节点实例只当贴花接收面，写在绘制计数之后，不画出来。子图的 GPU 几何留在池化缓冲里（`FPooledGeometry`），细分计数和各 tile 的网格图直接注册它，不再重算，也不再上传它的表。整网重建同样只算一遍。
5. 删掉不再有路的 tile。

各 tile 画出的三角形合起来与整网重建逐个相同（`Tiles.Actor`、`Tiles.Subgraph`）。

车流预览和桥墩要用整网的车道图和几何。只有整网建图时（第一次建，或设置变了）才当场重做。之后的编辑只写 tile，预览保持上次的样子，等编辑停下来，即距上次重建超过 0.3 s 且没有待做的重建，再由 `Tick` 整网建图、跑 GPU 几何、建车道图，重做一次（`RefreshPreview`）。所以拖动期间车流和桥墩不跟着变，松手约 0.3 s 后更新。

地形高度图每次加载只上传一次：`FSavedTerrain::Serial` 每次加载取一个新编号，渲染线程按编号留着上传好的两张图（Palmsland 各 32 MB），后面的图直接注册。

一次重建最多写 `MaxTilesPerBatch` 个 tile（默认 16，0 为不限），批量房子的 tile 另算一份上限。其余的 tile 留到后面的帧，每帧一批，这次编辑弄脏的 tile 排在之前积压的前面，所以拖动时不必等积压写完。积压的 tile 不靠比对输出判断，写完之前一直算脏：它们的路已经记进上次的结果，下次比对会显示没有变化，但它们画的还是旧的。`PendingTiles` 是还没写的 tile 数。分批写完与一次写完逐个相同（`Tiles.Batches`）。

每个 tile 写网格时要与渲染线程同步约 6 次，所以脏 tile 多时比整网重建还慢。`L_CityDemo` 的 79 条路分在 8 个 128 m 的 tile 里（10-27 测，子图之前）：整网暖重建 58 ms，分 tile 从零建 225 ms，没有改动时 24 ms。斜穿全区的大路挪一个点会弄脏全部 8 个 tile，耗时 194 ms；只挪特殊建筑时，路面不重写，25–30 ms。

Palmsland 转成样条（`LocalUpdateTiming.py`，3,894 根样条、5,667 条路）后用 256 m 的 tile，共 771 个路面 tile，另有约 836 个 tile 共 36,615 栋批量房子，每批 16 个。10-28 测了两轮，GPU 上另开着一个闲置的编辑器；第二轮加了让后台编辑器照常 tick 的参数，整体慢约 20%。下表给出两轮的范围：

| 重建 | 耗时 | 说明 |
| --- | --- | --- |
| 整网（不含批量房子） | 1.21–1.48 s | 与上文的 1.1–1.4 s 相当 |
| 分 tile 从零建 | 15–25 s，分 53 帧 | 第一帧整网建图（检测子图就是整网），整网布局批量房子；之后每帧的绘制子图 48–575 条路。第二轮有一帧卡了 2.8 s |
| 分 tile，没有改动 | 58–78 ms | 输入指纹都没变，不建图、不跑 GPU 几何，主要是样条转路网 |
| 挪一个样条点 5 m | 139–157 ms | 重写 2 个路面 tile（25 条路）和 1 个房子 tile。受影响的路 7 条，检测与绘制两个子图共 99 条路。样条转路网 44–50 ms，批量房子 7–9 ms，检测子图 10–15 ms，绘制子图 8–11 ms，细分计数 7–9 ms，2 个 tile 的网格 40–58 ms |
| 编辑停下后重做预览 | 196 ms，一次 | 整网建图、GPU 几何、车道图和桥墩：31,444 条车道，1,570 个桥墩 |

改进前后对比：用子图之前，每次编辑都整网建图、跑 GPU 几何、建车道图并重做预览。GPU 空时挪一个点约 240 ms，其中整网几何与读回 42–43 ms、车道图 38–46 ms、预览 24–32 ms、建图 11–14 ms、几何输入 12–13 ms；没有改动也要 0.20 s，从零建 25–28 s。再往前，整网几何共用之前，从零建要 112 s，挪一个点 0.50–0.57 s；批量房子改成局部重排之前，单点编辑里这一块约 200 ms，积压时每帧也要整网布局一遍。

一次重建的所有 tile 在同一帧里写，而 D3D12 的上传堆滞后 20 帧才回收，RDG 池里的缓冲要闲置 30 帧才释放。最初每个 tile 的图都把整网 GPU 几何重算并重新上传一遍：10-27 用 256 m 的 tile 不分批从零建 Palmsland 时，D3D12 报显存不足而崩溃（当时别的进程也占着 GPU）；分批之后不再崩溃，但 53 帧里提交内存仍涨了约 5.3 GB。共用几何后，同样 53 帧只涨约 2.4 GB。剩下的主要是 tile 网格本身：每个 tile 的路面网格还带着相邻路的边和节点实例当贴花接收面，按整卡显存占用估算，256 m 的 tile 写完后比整网模式多占约 3 GB。

## 大路与小路

`UCSRoadSplineComponent::Tier` 默认为 `Main`，设为 `Alley` 的样条路是小路。建路网前，`CSRoadSplineNetwork.cpp` 的 `ClipAlleys` 把小路被大路覆盖的段裁掉。覆盖范围是大路半宽加小路半宽，再加 `SplineSettings.AlleyClearance`，且两条路高差不超过 `JunctionHeight`；从上方跨过的不算。

- 裁口拉到大路中线，带 `bJoin` 标记，按 T 形路口接上大路，节点度数大于 1 时也照样接。
- 一条小路正好横穿大路时，两个裁口落在同一点，成为十字路口。
- 样条本身不改，大路挪走或删掉后，小路按原样恢复。

小路之间的 T 形路口在外侧会画出圆头。`City.AlleyJunctions` 已确认这些路口在拓扑上已经接上，是否与 CS2 里 `Alley` 的画法一致尚未核对。

## 城市布局

按 `LocalUpdateSystem_Plan.md` 的顺序：先放特殊建筑占出预留位，再一次性生成小路，之后随时放入大路，最后批量补房。几何部分是 `CSCityLayout.h` 里的纯函数（UE 厘米，俯视平面），不依赖 actor。

- **特殊建筑** `ACSCityBuildingActor`：闭合轮廓样条 `UCSCityOutlineComponent` 取凸包，挤出 `Height`；凸包外扩 `Setback` 就是它的预留位。改轮廓、移动、改属性或撤销时重建自身，并通知所属路网。所属路网由 `ACSRoadNetworkActor::NetworkOf` 决定：挂在哪个路网下就归谁，否则归关卡里唯一的样条路网。
- **小路区域** `ACSCityAlleyRegion`：`Generate` 把盒子的俯视矩形当成一块，每次垂直于长边把块切成两半，直到每块都不长于 `BlockSize`。切线落点在块中部抖动（`Jitter`、`TurnJitter`）；离预留位外扩 `Clearance` 后的范围有交叉，或切后有一块窄于 `MinBlockSize`，就换下一个落点，都不行的块保持完整。`bBoundaryAlleys` 时区域四边也是小路，跨过预留位的部分去掉。随机数取自块在切分树里的路径，所以同样的区域、预留位和设置得到同样的小路。生成结果是挂在路网下、`Tier = Alley` 的直线样条路，再次生成会替换上次的结果；生成之后它们与手画的路没有区别。
- **批量房子**（路网的 `bBatchBuildings`、`Lots`）：每根样条的每一段两侧每隔 `Frontage` 设一个候选位，快照路网则沿每条边。候选位的身份由样条（actor 名加组件名的哈希）、段号、槽位和朝向侧决定，进深、高度和颜色都由身份派生。候选位离建成后各条道路（小路已裁切）的路边至少半个 `Setback`，离自己面对的那条路是整个 `Setback`，也不能压到预留位；互相重叠时按身份派生的优先级取舍，只与重叠的候选位比较。所以改动一条路，只影响它附近的房子。房子按路面的 tile 网格分组写网格（`BuildingTile_*`），内容哈希没变的 tile 不重写。
- **只重排脏 tile**：一个地块只取决于自己的锚线、离它或它的竞争者够近的路和预留位，以及竞争者的锚线，再远的输入不会改变它。各类输入能影响到的地块中心范围由 `CSCity::AnchorReach`、`RoadReach`、`ReservedReach` 给出。每次重建给锚线、路和预留位算指纹，与上次相比，新增或消失的输入在影响范围内弄脏 tile，再加上积压的 tile。之后只取影响范围碰到脏 tile 的输入来采样、布局，只写回中心落在脏 tile 里的地块，结果与整网重算逐位相同（`City.LotsWindows`、`City.LocalUpdate`）。布局设置、tile 尺寸、actor 变换或材质变了，就整网重算。路是按内容区分的，长边上一处改动会弄脏它整条的影响范围：样条细分按长度等分，裁切点一动，整段小路的边都会变。

`L_CityDemo` 由 `Scripts/CS2Road/MakeCityDemo.py` 生成：两栋特殊建筑，`Old Town` 区域生成 23 条小路；`Avenue` 大路斜穿其中，裁掉 14 段小路并接上。全图共 79 条路，114 栋批量房子分在 10 个 tile 里。

一批最多重排、写 `MaxTilesPerBatch` 个房子 tile，这次编辑弄脏的优先，其余留在积压里。

## 管线

| 阶段 | 位置 | 原版来源 | 文件 |
| --- | --- | --- | --- |
| prefab 初始化 | CPU | `NetInitializeSystem` 子集（经 RoadLab `RoadAssets`） | `CSRoadAssets.cpp` |
| 建图、组合选择 | CPU | `RoadWorld.Build`、`CompositionSelectSystem` | `CSRoadGraph.cpp` |
| 组合数据、零件、车道、横道 | CPU，按（prefab, 标志）缓存 | `NetCompositionHelpers`、`NetCompositionSystem` | `CSRoadComposition.cpp` |
| 组合网格 | CPU，每个组合一次 | `BatchMeshHelpers.GenerateCompositionMesh` | `CSRoadCompositionMesh.cpp` |
| 几何 9 个 job | GPU，一线程一节点或一条边 | `GeometrySystem` | `CSRoadGeometry.usf`、`CSRoadGpuGeometry.cpp` |
| 组合参数 | GPU，一线程一条路 | `BatchDataHelpers.CalculateEdge/NodeParameters` | `CSRoadMesh.usf` |
| 路口接缝 | GPU，一线程一个节点实例的一侧 | RoadLab `RoadJunctionSurface` | `CSRoadDeform.ush`、`CSRoadMesh.usf` |
| 节点细分 | GPU，一线程一个基础三角形；先计数后写出 | RoadLab `RoadMeshRefinement` | `CSRoadRefine.ush`、`CSRoadMesh.usf` |
| 顶点变形、索引、材质 id | GPU，一个线程组一个实例 | RoadLab `RoadMeshes.Deform`（原版在 HDRP 顶点着色器里） | `CSRoadMesh.usf`、`CSRoadGpuMesh.cpp` |
| 车道图、横道曲线 | CPU，读回 GPU 几何 | RoadLab `RoadLanes`、`RoadCrosswalks` | `CSRoadLanes.cpp`、`CSRoadDecals.cpp` |
| 贴花条目 | CPU，按 `RoadDecals` 的调用顺序 | RoadLab `RoadDecals.Add`、`AddConnection` | `CSRoadDecals.cpp` |
| 贴花接收面、条带、投影网格、横道裁剪 | GPU，一个线程组一个段或一个条目 | RoadLab `RoadRenderer.Add`、`RoadSurfaceProjection` | `CSRoadDecal.usf`、`CSRoadGpuDecals.cpp` |
| 材质分段 | GPU | — | `UCSMeshOps::AddMaterialSectionPasses` |

几何 job 按原版依赖顺序录成一张 RDG：`InitializeNode → CalculateEdge → FlattenNode → FinishEdge → CalculateNode ×2 → CalculateIntersection → CopyNode → UpdateNode`。每条路三个实例（边、起点节点、终点节点），每个实例对应一个组合网格和一组 `CompositionParameters`。

一次重建录四张图，中间有两次 CPU 等待：

1. 几何、组合参数、接缝和细分计数。读回每个实例细分后的顶点数、三角形数，开贴花时连同边与节点几何一起读回，供车道图使用。CPU 据此排布网格并生成贴花条目。
2. 路面网格（细分写出、变形、索引），各节点的贴花接收面，以及横道裁剪的计数。几何不再重算，用第 1 张图留在池化缓冲里的那份（`FPooledGeometry`）。读回裁剪计数后，CPU 排布贴花网格。
3. 贴花网格，读第 2 张图写好的路面顶点流。
4. 路面的材质分段。分段会重排索引缓冲，而贴花按索引位置引用路面三角形，所以排在贴花之后。

## 路口接缝与细分

- 接缝：每个节点实例分左右两侧，左侧用曲线 0/2 和同步点 0/1，右侧用曲线 1/3 和同步点 2/3。`JunctionMatchCS` 一线程一侧，按 `SharedSection` 和 7 点有效性检查找唯一的双向对象，再改写打包参数里的 `curve.c2` 与 `lateral.c2`。对象编码为 `slot*2+side+1`（0 表示没有），双向与否由读取方核对。
- 细分：`RefineCS` 对每个基础三角形做深度优先切分，叶子顺序与 C# 一致。每条边带一个定义方向：基础边取所在材质组里第一个三角形的方向，子边继承，内部边取第一个包含它的子三角形的方向。计数、组内前缀和、写出分三步，深度上限 19 层。

## 贴花

- 条目：`FRoadLanes::Build` 按 .NET 语义（字典插入顺序、稳定排序、`Array.BinarySearch`）建车道图。`FGpuDecalInput::Build` 按 `RoadDecals` 的调用顺序为每个贴花网格出一个条目，分三类：没有接收面的条带（每 1.5 m 一行，每行 2 个顶点），投影到节点表面的网格（每 0.5 m 一行，每行 7 个顶点，用于路口连接线），以及裁剪到表面的横道。
- 接收面：一个节点的接收面是 `RoadRenderer.Add` 放进其 `RoadSurfaceProjection` 的路面三角形，即节点实例的全部三角形加上各条边靠近节点的三角形，按段（一个实例）依插入顺序排列。`ReceiversCS` 在 GPU 上从中筛出朝上（法线 y ≥ 0.7）、投影面积不退化的三角形，记成路面索引流里的位置。它同时给每 64 个接收槽记一个包围盒，供后面的收集整块跳过。
- 投影：`TryProject` 只看点所在 8 m 格子里登记的接收面，高度最近者胜，并列时取先到的。每个条目先把包围盒够得着的接收面按顺序收进 groupshared 候选表（1,536 个，每个记三角形、所在格子和厘米精度的包围盒），顶点只读盒子罩住它的几个候选。表装不下时，投影网格改为每 64 个顶点按自己的包围盒重新收集；横道改为整块（64 个）遍历节点的接收面，按每块扇形三角形中心的包围盒收集候选。必要时分几张表轮流装，结果不变。
- 横道裁剪：脚印的两个三角形按 8 m 格子 x 优先遍历，每个接收面在它的第一个格子里处理。Sutherland–Hodgman 裁剪后，扇形三角形的中心须投影回该接收面本身才保留。第 2 张图只计数，第 3 张图写出。
- 输出：`UCSMesh` 带 4 套 UV，布局同静态导入，供 `M_CS2` 贴花材质读取：UV0 为贴花纹理，UV1 为到两端的距离 `(t·len, (1−t)·len)`，UV2、UV3 为颜色的 (r, g) 和 (b, a)。`MarkingMesh` 不投射阴影，不进光追和距离场。

## 约定

- 结构布局由 `Scripts/CS2Road/generate_types.py` 从 `Port/Original` 生成 `Shaders/Private/CSRoad/CSRoadTypes.ush`，C++（`CSRoadCore.h`）和 HLSL 共用。实体、prefab、组合表都从 1 开始编号，0 代表 `Entity.Null`。
- 计算在 Unity 米坐标（y 向上）下进行，写入 `UCSMesh` 时转换为 UE 坐标：`(x, -z, y) * 100`，再乘 actor 变换。这个转换是正交旋转；但 Unity 正面三角形的 `cross(B-A, C-A)` 与法线同向，UE 的则反向，所以写索引时交换每个三角形的第 2、3 个顶点。UV 保持原值，与静态导入一致。贴花读回路面时做反变换。
- `RoadMesh` 和 `MarkingMesh` 存世界坐标、以绝对变换渲染，所以挂在单独的 `Root` 下，自身变换始终是单位变换。做 actor 的根时，它会带上 actor 的位置，把这个变换再乘一遍，路面就画到别处去了。actor 的变换只在写顶点时乘进去，所以移动 actor 后要重建。
- Unity.Mathematics 语义：`min`/`max`/`clamp` 遇 NaN 的结果和 HLSL 内建函数不同，统一用 `CSRoadMath.ush` 的 `U_` 版本。
- 每个 kernel 的输出变量都标 `precise`。DXC 默认给浮点运算加 `fast` 标志，会重排 `a + 18b - 18c + 6d` 这类抵消式，误差可达几米。`precise` 会沿数据依赖和控制依赖反向传播到整段计算。
- 三角函数用 `U_sin`/`U_cos`/`U_tan`/`U_acos`（Cephes 单精度实现，相对误差 1–2 ulp）。GPU 内建版本只保证绝对误差，小角度时有效位基本丢光；Unity.Mathematics 则是把 double 精度的 `System.Math` 结果转成 float。
- `line`、`triangle`、`point` 是 HLSL 关键字，不能作变量名。Windows 上全局 shader 默认按 HLSL 2021 编译，向量逻辑运算要写成 `and()`、`or()`、`select()`；`?:` 不能作用于结构体，要写成 `if`/`else`。

## 与原版的差异

- 导入的路网没有子对象（NetObject、路口物件）和附属网，因此 `CalculateMiddleRadius`、`GetFixedNodeOffset` 返回 0，`LinkAuxOffsets` 的子网循环为空，`GetStraightNodeLength` 取默认宽度的一半。
- `CalculateNodeGeometry` 第 2 轮读取其他边的节点几何（`CheckNodeCurves`）时，读的是第 2 轮开始前的拷贝。C# 是逐条边顺序执行的，能看到已经重算过的边；这只影响 middle connection，导入数据里没有这种情况。
- `MathUtils.TightBounds`：曲线完全对称（b = c、a = d）时三次项系数恰好为 0，原式变成 0/0，会漏掉曲线中间的凸起。C# 参考靠 1 ulp 的舍入残差碰巧算对。HLSL 在系数为 0 时取原式的极限 `f4 / (2 f3)`。
- `FlattenNodeGeometryJob` 用定长数组暂存节点的边，单个节点最多 16 条边（`GpuMaxNodeEdges`），超出时直接报错。
- 细分的中点不共享，每次切分各自生成，因此顶点比 C# 多（Palmsland 378 万个，C# 新增 201 万个），但位置相同，网格仍然闭合。C# 超过 20 轮会抛异常，GPU 的深度上限是 19 层。
- 车流只有预览（见[车流预览与桥墩](#车流预览与桥墩)），没有市民出行。原版按 `Updated` 标记只重算改动实体的做法没有照搬，局部更新改为按 tile 重写，见[局部更新](#局部更新)。

## 验证

测试都在 `PCGPlugins.CSRoadNet.*`。金标由 `Scripts/CS2Road/GoldenDump` 运行 C# RoadLab 导出到 `Saved/CSRoadGolden`，包括 55 个场景（RoadScenarios × 3 种 prefab，外加左舵十字路口）和 Palmsland 全图（几何、接缝、细分计数）。

| 测试 | 比对内容 | 判定 | 当前结果 |
| --- | --- | --- | --- |
| `Composition.Scenarios`、`Composition.Palmsland` | 组合标志、数据、零件、车道、横道、组合网格顶点和索引 | 逐位 | 0 误差 |
| `Geometry.Scenarios` | Edge、Start/End、Node 几何 | 1 cm 内一致，超出份额 ≤ 0.25%，>10 cm ≤ 0.1% | 0.113%，最大 0.91 m（`elevated-cross`） |
| `Geometry.Palmsland` | 同上，5,667 条路 | 同上 | 0.029%，最大 0.29 m |
| `Junction.Scenarios`、`Junction.Palmsland` | 每个节点实例两侧的接缝对象 | 不同的侧 ≤ 0.2% | 场景 856 侧（452 有对象）、Palmsland 22,668 侧（19,346 有对象），都是 0 不同 |
| `Deform.Scenarios` | 不细分时 64 万个变形顶点的位置、法线、UV | 同几何，>10 cm ≤ 0.25% | 位置 0.229%，法线 0.136%，UV 0.061% |
| `Refine.Scenarios` | 细分三角形数；细分后各角的位置、法线、UV | 三角形数之差 ≤ 0.25%；角点同几何 | 969,565 对 969,567 个三角形；位置 0.030% 超 1 cm |
| `Refine.Palmsland` | 11,334 个节点实例的细分三角形数 | 各实例差的绝对值之和 ≤ 0.25% | 8,259,058 对 8,258,645，差 0.101% |
| `Lanes.Scenarios` | 车道图（1,335 条车道，含 724 条连接线）和 178 处横道 | 逐位 | 0 误差 |
| `Decals.Scenarios` | 2,086 个贴花条目：网格三角形逐个比，裁剪三角形按重心配对，再比顶点属性；另外强制所有条目走装不下时的退路再跑一遍 | 网格布局不同 ≤ 1%，三角形 ≤ 0.5%，顶点同几何；退路逐位一致 | 417,916 个网格三角形 0 不同；56,366 个裁剪三角形 23 个未配对；位置 0.013% 超 1 cm；颜色逐位；退路 0 不同 |
| `Splines.Basics` | 手画小路网：十字、立交、高差 1.5 m 的交叉、T 形、固定样条交叉、吸附、按长度和转角细分、合并、闭环、未知 prefab | 拓扑和统计逐项 | 全部符合 |
| `Splines.Palmsland` | Palmsland 经 `ChainNetwork` 转成样条再建回：边和节点的数据、组合、GPU 几何；样条打乱先后再建 | 数据和组合逐个相同，GPU 几何没有超过 0.1 m 的值；打乱后边和节点逐个相同 | 5,667 条边转成 3,894 根样条（9,561 个点）；52 个值超过 1 cm，最大 2.7 cm |
| `Splines.Actor` | actor 放在离原点几公里处并转 30°，建一次，平移后再建一次：两根样条建成 4 条路；再用代码挪一个样条点，下一次 `Tick` 自己重建 | 两个网格组件保持单位变换；路面包围盒含 4 个路端，中心离路口 < 5 m | 通过 |
| `Splines.Alleys` | 小路与大路：横穿、斜穿、顺着压、立交 | 裁切段数，十字或 T 形接法，裁后的小路不进大路的覆盖范围 | 通过 |
| `Tiles.Actor` | 3 条东西向、4 条南北向的路，整网建与分 tile 建；不改动重建；挪一个点；删一条路 | 路面、标线三角形与整网重建逐个相同；不改动时不重写 tile；改动只重写部分 tile，其余网格代数不变 | 通过 |
| `Tiles.Batches` | 同一路网每批 2 个 tile 逐帧写完；改 tile 尺寸弄脏全部 tile，写了一批后再挪一个点 | 每批不超过 2 个 tile；写完后路面、标线三角形与整网重建逐个相同 | 8 个 tile 分 4 批，通过 |
| `Subgraph.Reach` | Palmsland（不带地形）整网建一次；取 192 条分散的路，各自只用周围 1–4 圈的路建子图 | 这条路的组合，以及边、两端节点和节点处的 GPU 几何，与整网逐位相同 | 1 圈 64/192，2 圈起 192/192 |
| `Subgraph.Tiles` | Palmsland（带地形）按 256 m 分 tile，每个 tile 的路加两圈建子图 | tile 里每条路的输出指纹（边、两端节点几何、三个组合）与整网相同 | 771 个 tile，子图最多 92 条路；5,667 条路 0 不同 |
| `Tiles.Subgraph` | 10 条东西向、10 条南北向的街（其中一条中路，最东一条有 owner），转 7°，96 m 的 tile：在西北角接一段支路；挪最西一条街的北端（整条街都斜了）；删一条街；加一条斜路 | 每次编辑后路面、标线三角形与整网重建逐个相同；只重写部分 tile；接支路时两个子图加起来也比整网少，最东那条有 owner 的街 tile 网格不变；编辑停下后预览按整网重做，车道数与整网相同 | 通过；接支路重写 2 个 tile（共 37 个），子图共 54 条路（整网 221 条） |
| `City.Alleys` | 区域里两块预留位，一块横跨中部 | 小路不进预留位外扩 `Clearance` 的范围、不出区域；同输入同结果；去掉预留位后会穿过它们；换种子结果不同 | 通过 |
| `City.AlleyJunctions` | `L_CityDemo` 区域的小路建成路网 | 没有断头 | 40 个节点、57 条边，0 个断头 |
| `City.Lots` | 两条相交的街和一块预留位；在 150 m 外加一条街，再去掉 | 地块离路边至少半个退让、不压预留位、互不重叠；加街后 50 m 外的地块身份和形状不变，新地块只出现在新街旁；去掉后复原 | 通过 |
| `City.LotsWindows` | 带斜街、宽窄街和预留位的街网，整体布局一次，再对 5 个窗口只取影响范围内的输入布局 | 窗口里每个地块的身份、角点、底高和高度与整体布局相同 | 69 个地块，0 不同 |
| `City.LocalUpdate` | 转过 10° 的分 tile 路网（64 m）：画大路、挪它的一个点、挪特殊建筑、删大路 | 每次编辑后房子有变化，且与从零布局的顶点相同；挪特殊建筑只重排部分 tile | 通过；挪特殊建筑重排 14 个 tile（共 20 个） |
| `City.Actor` | 特殊建筑、小路区域、批量房子和分 tile 路网；不改动重建；在区域一角加大路；挪特殊建筑；换种子后每批 2 个房子 tile 逐帧写完；关掉批量房子 | 小路和批量房子都不进预留位；不改动时不重写房子 tile；加大路只重写附近的房子 tile；分批写完与一次写完的房子顶点相同 | 100 栋房子 8 个 tile，加大路后重写 3 个、5 个不动 |

`Refine.Palmsland` 有 21.6% 的实例三角形数不同，这在噪声范围内：扰动版 C# 自己就有 27% 的实例不同。`Decals.Scenarios` 里 `elevated-cross` 的裁剪三角形只报告不计入预算（1,504 个里 615 个未配对），原因是它的几何本身处在平局上，见下文。

渲染对照：`RoadNetworkShots.py` 在 `L_Palmsland` 里生成整张 Palmsland（5,667 条路、2,115 个组合、1,476 万顶点、1,330 万三角形、48,577 个贴花条目、1,031 万个标线三角形），与静态导入的 `Roads` 加 `Markings` 层在同机位对比。近景（4 处高架和立交，以及 2 个带横道的城市路口）平均像素差 0.03–0.29 级（满量程 255），差超过 16 级的像素不到 0.04%。全景约 4.6 级，差在高架的投影（GPU 网格的影子更长），只比路面时也是这个数。

重建耗时：Palmsland 暖启动整次重建 1.1–1.4 s。其中快照约 140 ms，建图与组合约 25 ms，组合网格约 230 ms，细分计数（含阻塞回读）150–300 ms，车道图与贴花条目约 50 ms；GPU 上路面那张图约 200 ms，贴花约 70 ms。完整编辑器里刚打开 `L_Palmsland` 时，第一次重建要等渲染线程处理完静态导入的几千个 actor，细分计数这一步可达 13 s。`SplineNetworkShots.py` 手画的 5 根样条建成 16 条路，暖重建约 50 ms；第一次还要读 prefab 数据和地形，约 0.5 s。

几何 job 本身对舍入不稳定，GPU 结果不按逐位比对。`GoldenDump --perturb=<seed>` 把输入的 x/z 各挪 ±1 ulp，C# 参考自身就会在场景里移动 2.7 m、在 Palmsland 里移动 7.7 m，超过 1 cm 的值分别占 0.62% 和 0.20%。GPU 与金标的偏差比扰动版 C# 小一个数量级。

完全对称的场景（如 `elevated-cross`）处在平局上，GPU 和 .NET 的取舍不同。把环境变量 `CS2ROADGOLDEN` 指向扰动导出再跑测试，GPU 与扰动版 C# 的结果一致。排查新偏差时，先用这种方法区分是移植错误还是舍入敏感。

离线比对工具：

- `Scripts/CS2Road/diff_gpu_geometry.py`：按字段统计测试写出的 `Saved/CSRoadGpu/<金标目录名>/<id>.bin`（几何）与金标的偏差。变形结果和贴花分别写在同目录的 `<id>.deform.bin`、`<id>.decals.bin`。
- `Scripts/CS2Road/RoadNetworkShots.py`：在 `L_Palmsland` 里生成 GPU 路网，从相同机位分别拍静态导入版和 GPU 版。默认两边都带标线，输出到 `Saved/CS2RoadShots`；设 `CS2ROAD_SHOTS=roads` 只比路面，输出到 `Saved/CS2RoadShots_roads`。要用完整编辑器的 `-ExecCmds="py <脚本>"` 运行，`UnrealEditor-Cmd` 里拍出来是黑的。
- `Scripts/CS2Road/MakeSplineRoadDemo.py`：生成示例关卡 `L_SplineRoadDemo`，用无头编辑器的 `-ExecutePythonScript` 运行。关卡基于引擎的 Basic 模板（太阳、天空、雾），地面比样条低 0.25 m（路面比样条低 0.2 m）；8 根样条包括主路、十字路口、弯道 T 形路口、两端坡道各落在一条街上的 12 m 高架，以及一条带连接路的环路。关卡已存在时脚本会拒绝覆盖。
- `Scripts/CS2Road/SplineNetworkShots.py`：在 `L_Palmsland` 离城 900 m 的空地上用样条画一个小路网（十字、T 形、立交、3 m 短段），路网只借 Palmsland 快照的地形，从 5 个机位拍到 `Saved/CS2RoadSplineShots`。运行方式同上。
- `Scripts/CS2Road/MakeCityDemo.py`：生成示例关卡 `L_CityDemo`，见[城市布局](#城市布局)。运行方式同 `MakeSplineRoadDemo.py`，关卡已存在时拒绝覆盖。
- `Scripts/CS2Road/MakePalmslandSplines.py`：把 Palmsland 快照转成样条，存成 `L_PalmslandSplines`，见[从快照转换](#从快照转换)。`CS2ROAD_NETWORK` 可换快照。运行方式同 `MakeSplineRoadDemo.py`，关卡已存在时拒绝覆盖。
- `Scripts/CS2Road/CityDemoShots.py`：打开 `L_CityDemo`，给整网、分 tile、挪大路、挪特殊建筑计时，再从 4 个机位拍到 `Saved/CS2CityShots`。用完整编辑器运行，不存盘。
- `Scripts/CS2Road/LocalUpdateTiming.py`：把 Palmsland 转成样条，开批量房子，给整网和分 tile 重建计时；从零建时每帧一批，直到没有积压。最后打开路网自己的 `Tick`、等编辑停下，预览整网重做的耗时记在日志里。结果写到 `Saved/CS2LocalUpdateTiming_<tile>_<batch>.json`。`CS2ROAD_TILE` 设 tile 尺寸（米，默认 256），`CS2ROAD_BATCH` 设每批 tile 数（默认 16）。要让帧在批与批之间过去，所以用完整编辑器的 `-ExecCmds="py <脚本>"` 运行，不存盘；再加上文[样条路网](#样条路网)里那个 `-ini` 参数，编辑器在后台时路网才会 tick，最后一步才测得到。

## 重新生成

`Port/Original` 的结构定义变了以后，重新生成共享布局：

```bash
python Plugins/PCGPlugins/Scripts/CS2Road/generate_types.py
```

C# 端更新后重新导出金标。`GoldenDump` 引用 `Port/RoadPort.csproj`，并把 RoadLab 的 `App/RoadDecals.cs` 原样编进来，用 `GodotShim.cs` 补上它用到的 Godot API。RoadLab 目录由 MSBuild 属性 `CS2RoadLab` 指定，游戏目录由 `GameRoot` 指定。先在 `Scripts/CS2Road/GoldenDump` 里执行 `dotnet build -c Release`。参数 `--no-mesh` 跳过组合网格，`--perturb=<seed>` 导出扰动版，后面可以跟场景 id 只导出部分场景：

```bash
Plugins/PCGPlugins/Scripts/CS2Road/GoldenDump/bin/Release/net8.0/GoldenDump.exe Saved/CSRoadGolden
```

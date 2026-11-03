# CS2 道路逻辑的 GPU 移植（CSRoadNet）

`CSRoadNet` 模块把 Cities: Skylines II 的道路生成逻辑搬进 UE。它从关卡里画的样条路出发（CS2 的路网快照先转成样条路）：CPU 选组合、拼组合网格；GPU 跑 `GeometrySystem` 的几何 job、算组合参数、统一路口接缝、细分节点网格，再把组合网格变形到曲线上，结果直接写进 `UCSMesh` 渲染。标线、胎痕和横道（RoadLab 的贴花）由 CPU 遍历车道图生成条目，每个条目画成单位网格的实例：材质按实例数据把它沿条目的曲线摆开，作为 DBuffer 网格贴花画到下面的路面上。分 tile 建时，tile 停手后在后台烘成 Nanite 网格，换下 GPU 网格并释放它的显存，见[烘成 Nanite 与显存预算](#烘成-nanite-与显存预算)。它与 [Palmsland 道路关卡](CS2Roads.md) 的静态网格导入相互独立。

原版代码来自 `D:\MyProject\CitiesSkylines-Road2-C#` 的 `Port/Original`（反编译源，逐字节可复现）。C# RoadLab 自己写的部分按 RoadLab 移植，并在下文注明，包括 `RoadWorld.Build` 的流程、`RoadMeshes.Deform`、`RoadJunctionSurface`、`RoadMeshRefinement`、`RoadLanes`、`RoadCrosswalks` 和 `RoadDecals`。

## 用法

- 关卡里放 `ACSRoadNetworkActor`，在它下面挂 `ACSRoadSplineActor`，一根样条是一条路，见[样条路网](#样条路网)。路网 actor 是 manager：设置、操作和统计在它上面，建出来的东西在它名下一个单独的内容 actor 里，见[Manager 与内容 actor](#manager-与内容-actor)。示例关卡 `/PCGPlugins/CSRoadNet/L_SplineRoadDemo` 由 8 根样条组成，打开后拖样条点即可看到路网实时重建。
- `NetworkFile` 指向 `cs2-road-network-v1` 快照时提供地形；相对路径从插件的 `Data/CS2Road` 算起，例如 Palmsland 的 `Palmsland/network.json`，见[数据](#数据)。转换是函数工具，不在详情面板上：`UCSRoadNetTools::ConvertToSplines(Network)` 把快照里的路转成样条路，`UCSRoadNetTools::ConvertTerrainToLandscape(Network, Material)` 把快照地形转成 Landscape（Python：`unreal.CSRoadNetTools.convert_to_splines(network)`，编辑器工具蓝图同名节点）。路网只从样条建，不直接读快照里的路。Palmsland 整张图转好的样条在 `/PCGPlugins/CSRoadNet/L_PalmslandSplines`，它也是测试高架桥、大路、小路和房子的通用关卡，见[Palmsland 通用测试关卡](#palmsland-通用测试关卡)。
- `bShapeTerrain` 让关卡的 Landscape 跟着地面路走，见[地形 edit layer](#地形-edit-layer)。
- 城市局部更新：`bLocalUpdate` 让路网分 tile 重建，见[局部更新](#局部更新)；样条路分大路和小路，见[大路与小路](#大路与小路)；特殊建筑、小路生成和批量补房见[城市布局](#城市布局)。示例关卡 `/PCGPlugins/CSRoadNet/L_CityDemo`。
- 改属性、移动松手、关卡加载或复制 actor 时自动重建，也可以点 `Rebuild`。常驻网格不存盘，加载后重新生成。
- `AssetDirectory` 留空时读插件 `Data/CS2Road/Prefabs` 下的 `prefabs.json`、`raw-prefabs.json`、`pieces.bin`，见[数据](#数据)。同一目录每个进程只加载一次，日志记下读的是哪个目录（`CS2 road prefabs loaded from`）。
- 材质按 atlas 的 BaseColor 贴图名（目录里的 `material_bindings`）在 `MaterialFolder` 找 `M_CS2_<NN>_<BaseColor>`，这些材质由 `Scripts/ImportCS2Roads.py` 生成。找不到的 atlas 列在 `MissingAtlases`，改用 `FallbackMaterial`。
- `bBuildMarkings`（默认开）把贴花画成实例化的 DBuffer 网格贴花，见[贴花](#贴花)。两张母材质 `M_RoadDecal`、`M_RoadDecalFitted`（`DecalMaterial`、`FittedDecalMaterial`）由 `Scripts/CS2Road/MakeRoadDecalMaterial.py` 生成；actor 为每个贴花表面建一个动态材质实例，贴图和 `Tint` 取自同名的 `M_CS2_*` 贴花材质，找不到的表面列在 `MissingMarkingSurfaces`。没有绑定 BaseColor 贴图的表面像 RoadLab 一样跳过：RoadLab 看贴图文件在不在，导出的 33 张都在，所以这里只看绑定，不读 `.dds`。路面组件都接收贴花（`bReceivesDecals`），GPU 网格和 Nanite 网格一样。Palmsland 的 48,577 个贴花条目画成 204,631 个实例（条带 107,194 个，贴合网格 97,437 个）；原先的标线网格要约 690 MiB。
- 属性含义写在 `Source/CSRoadNet/Public/CSRoadNetworkActor.h` 的声明注释里。

### 车流预览与桥墩

主 Actor 的 **CS2 Road → Traffic → Show Traffic** 打开后显示车流；关掉会隐藏并暂停车辆，再打开从原位置继续，不重建道路网格。`Traffic Vehicle Count`、`Traffic Speed`（km/h）、`Traffic Gap`（m）和 `Traffic Seed` 控制数量、速度上限、车距和随机种子。`Reset Traffic` 用同一种子重新布车。`Traffic In Editor` 控制编辑器视口里的动画；编辑器须开启实时刷新。关闭标线仍可显示车流，重建道路后会重新生成车道图与车辆。

车辆使用现有的有向车道曲线与合法路口连接，按 30 Hz 固定步长更新。跟车检查跨越边界的前车与车尾；路口用保守的独占预约避免交叉穿车，出口堵住时在入口等待。带信号灯的节点按进入道路轮换 8 秒绿灯、2 秒清空期。普通车速还受道路 prefab 限速约束，连接段上限 8 m/s。默认车体是实例化的简易车身和车顶，可用 `Traffic Vehicle Mesh` 换成 +X 朝前的车辆网格，并指定 `Traffic Material`。

这是车流可视化预览，尚未移植 CS2 的市民出行、目的地寻路、换道、公交或服务车辆。环岛连接仍受现有 `FRoadLanes` 限制，数量显示在 `Unsupported Traffic Roundabouts`；共享双向车道和专用车辆车道不会生成预览车辆。

**CS2 Road → Bridge → Build Bridge Piers** 为 Elevated 且非 Tunnel 的道路生成柱身、盖梁和埋入地面的基础。`Bridge Pier Spacing`、`Bridge Pier Min Height`、`Bridge Pier Width` 使用米，`Bridge Pier Material` 可覆盖默认材质。支撑沿道路弧长布置，底部优先读取可碰撞静态场景的地面高度（`Trace Bridge Ground`），否则使用快照高度图，无高度图时使用 Actor 局部 Z=0。柱顶连接组合网格的底面；低净空段与路口端部不放柱。

基础会避开下穿道路的完整宽度，并在一个跨度内尝试调整位置；无法放置的数量显示在 `Skipped Bridge Piers`。该计数也包括高度不足的候选位置。生成组件是临时数据，重建会清除旧实例。桥墩目前是可视化构件，不提供碰撞或结构承载计算。每次生成在日志里记一行桥墩数、高架路数和跳过数。

车流和桥墩画在内容 actor 的三个默认子组件里（`TrafficBodies`、`TrafficCabins`、`BridgePiers`），和内容 actor 一起不存盘。

车流与桥墩的核心回归测试直接编译生产头文件：`Scripts/CS2Road/RunTrafficBridgeTests.ps1`（MSVC 14.38 / Windows SDK 10.0.22621，与本机 Rider 构建工具链一致）。编辑器验证脚本为 `Scripts/CS2Road/ValidateTrafficBridge.py`；启动方式写在文件头，结果写入 `Saved/CodexTrafficBridge/editor-validation.json`，测试关卡不保存。仅核心测试或单文件编译通过不代表编辑器验证已通过。

2026-10-27 验证：Rider `Development_Editor / x64` 对应的 Editor 与 ShaderCompileWorker 目标增量构建、链接通过；核心回归通过。独立编辑器的 30 项检查通过，测试路网为 12 条边、46 条可行驶车道/连接、32 辆车、12 个桥墩、0 个跳过支撑。验证覆盖显示开关、暂停/恢复、重复种子、道路重建、标线独立性、静态地面射线、自定义车辆网格切换；4 张截图保存在同一 `Saved/CodexTrafficBridge` 目录并已检查。核心布局性能样例：6,000 条合成道路生成 18,000 个桥墩约 10 ms，不含 UE 射线和渲染开销。编辑器启动仍有其它模块原有的结构体未初始化警告，不影响本次脚本结果。

### Manager 与内容 actor

路网 actor 只放设置、操作和统计，相当于 manager。它建出来的东西都放在它名下的 `ACSRoadNetworkContent` 里（`CSRoadNetworkContent.h`）：路面（整网的 `RoadMesh`，或各 tile 的 `RoadTile_*`、`NaniteTile_*`）、标线、批量房子、车流和桥墩。内容 actor 在第一次建时生成，摆在路网的位置并挂在它下面：

- 不存盘、不复制（`RF_Transient | RF_DuplicateTransient`），加载或复制路网后重新建。
- 不进 Outliner，也选不中：点路面什么也不选，选路网要在 Outliner 里点。
- 路网隐藏时它一起隐藏（`Tick` 里同步），删掉路网时它一起删掉。
- 脚本用 `GetContent()` 取它（Python `network.get_content()`），例如 `network.get_content().get_editor_property("bridge_piers")`。

这样拆是因为选中卡顿：10-30 之前这些组件都在路网 actor 上，Palmsland 建完有 5,428 个（783 个 Nanite tile、3,993 个实例组件、651 个 GPU 网格组件）。点选路网时详情面板要列出全部组件，选中描边也要画全部组件：点一下约 1.2 s（选择调用 0.8 s，之后三帧各 120–220 ms），选中期间每帧从约 10 ms 涨到 40–50 ms。点一根样条路只要约 40 ms。拆开后路网名下只剩一个根组件，在 Palmsland 建完、空闲时点选路网，选择调用 13–18 ms，之后第一帧 30–45 ms，再往后与不选时相当（11–25 ms，不选时 10–14 ms）；点一根样条路约 36 ms。

10-30 之前存的关卡里有路网的 `RoadMesh` 组件。加载后它成了没有属性引用的组件，`PostRegisterAllComponents` 把路网名下的 GPU 网格组件销毁；这不弄脏关卡，下次存盘就没有了。

## 数据

CSRoadNet 读的 CS2 数据都放在插件的 `Data/CS2Road` 下（`CSRoadDataDir()`），用不着 CitiesSkylines-Road2-C# 工程。这些是从游戏里提取的数据，仓库又是公开的，所以不进 git（`.gitignore`），靠同步盘带到另一台机器。

| 目录 | 内容 | 大小 | 用途 |
| --- | --- | --- | --- |
| `Prefabs` | `prefabs.json`、`raw-prefabs.json`、`pieces.bin` | 118 MB | 建路必需，`AssetDirectory` 留空时读这里 |
| `Palmsland` | 快照 `network.json`、高度图 `terrain-playable.r16` 与 `terrain-world.r16`、来源说明 `provenance.json` | 69 MB | `L_PalmslandSplines` 的地形（`NetworkFile` = `Palmsland/network.json`；其它示例关卡不设快照）、转换工具、Palmsland 测试 |
| `Golden` | C# RoadLab 的金标导出：55 个场景和 Palmsland 全图 | 214 MB | 只有测试用，缺了时金标对照跳过 |

- `NetworkFile` 是相对路径时从 `Data/CS2Road` 算起，绝对路径照旧。
- 环境变量 `CS2ROADLAB` 指向一个 CitiesSkylines-Road2-C# 工程时，`AssetDirectory` 留空的路网和测试改读那里的 `Assets/CS2` 与 `Artifacts/public-saves/palmsland-3`；`CS2ROADGOLDEN` 指定另一份金标。
- 标线只看表面有没有绑定 BaseColor 贴图（见[用法](#用法)），所以不需要 C# 工程里 1.6 GB 的 `.dds`；画面上的贴图是 `Content/CS2Roads` 里导入的 UE 贴图。
- 来源：预制件和快照复制自 CitiesSkylines-Road2-C# 的 `Assets/CS2`、`Artifacts/public-saves/palmsland-3`，金标是它的 C# RoadLab 导出。C# 那边已经定型，这些数据不再更新，金标的导出工具 GoldenDump 和类型生成脚本 `generate_types.py` 11-02 已从插件删除（git 历史里还有），插件里不再有读 C# 源码的东西；转换工具、关卡和脚本只读 `Data/CS2Road`。

### 自定义道路

CS2 的道路里没有 2–3 m 宽的（最窄的 `Alley`、`Gravel Road`、`Pedestrian Street Small` 都是 8 m），新道路用现有道路的段拼出来。定义在 `Data/CustomRoads.json`，进 git：它只引用预制件里的道路和段编号，不含游戏数据。`FAssets::Load` 读预制件时把它并进去，路网 actor 的 prefab 下拉框里就有了。

```jsonc
"Alley 2m": {
  "copy": "Alley",                 // 母路：照抄它的 raw 属性（限速、路口规则、路口时的状态规则）
  "sections": [0, 1, 3, 5, 6],     // 两侧的边（0 m）、两条 1 m 的小巷路肩、中间 0 m 的中线；去掉两条 3 m 车道段
  "underground": [],               // 不要地下管线：组合至少和管线一样宽，母路的管线是 3.5 m
  "zoning": false,                 // CS2 不划区：没有 zone block，几何不按格子对齐
  "plots": {                       // 批量房子照样沿它排
    "setback": 0.5,                // 离路边 0.5 m（不写用路网的 Lots.Setback）
    "frontage": 9                  // 每块地沿路 9 m，含 Gap（不写用 Lots.Frontage，也不会比它宽）
  }
}
```

- 新道路的 path id 比所有 raw id 都大，raw 数据复制母路；`zoning: false` 时把 `m_ZoneBlock` 清零。没写的段列表照抄母路。
- 段也可以借别的路的，并改它的标记：`{ "road": "Alley", "index": 5, "invert": true }`（不写 `road` 就是母路；可改 `invert`、`flip`、`median`）。比如 `Alley Oneway` 的单车道段夹在 `Alley` 的两条 1 m 路肩之间，就是 5 m 宽、接口干净的单行巷子。
- 🛑 两侧要留路肩：路口的接法是靠路肩做的。接带人行道的路时，路肩换成从路面坡上人行道的过渡件（`Alley Shoulder To Sidewalk 1`），街道人行道的圆角就接在它上面。CS2 的每条路边缘和行车道之间都有路肩，没有只剩行车道的。11-02 先做过只留中间 3 m 车道段的 `Alley 3m`，路口没有东西接：接街道时人行道开口两侧露出楔形的地面，T 形接小巷时两个角各伸出一根细刺。CS2 的路肩只有 1 m、2 m、2.5 m，凑不出恰好 3 m 又带路肩的组合，所以改成 2 m；中间那条路肩当中线的 3 m 拼法在路口中间出碎面，不能用。
- `plots` 是插件自己的字段，与 CS2 的划区无关：有它的路就有批量房子的候选位，退让和每块地的宽度用它的（`FAssets::Plots`）。别的房子离这条路的路边留它退让的一半。
- `Alley 2m`：宽 2.0 m，两条路肩铺满，没有车道（只走人），与 `Alley` 同样的铺面。路口和标准小巷一样是干净的圆角。两侧的房子离路边 0.5 m、朝向它，面对面只隔 3 m；每栋沿巷子 7 m 宽（9 m 减去 Gap），比街边的房子窄、多，就是楼与楼之间的巷子。
- `Assets.CustomRoads` 测试核对宽度、不划区、地块退让 0.5 m 和宽度 9 m；在带人行道的路口两条路肩都换成上坡过渡件、在小巷路口仍是平的；借段的写法（翻转、改中线）照字面生效；以及它两端 T 形接到小巷后能正常建出路网和房子。

#### 路口的接法记在哪

`Data/CS2Road/Prefabs` 里的 CS2 数据记了每种路在路口「用什么」，「什么时候用」由代码判：

| 层 | 在哪 | 记了什么 |
| --- | --- | --- |
| 路 | `raw-prefabs.json` 的 `m_NodeStates`、`m_EdgeStates` | 状态规则：在什么条件下，这条路的路口（或路段）带上哪些状态。例如 `Small Road`、`Medium Road` 的路口带 `Sidewalk`、`OppositeSidewalk`、`Pavement`，`Alley` 只带 `Pavement`；`Gravel Road` 按对面是不是铺装路在 `Gravel` 和 `Pavement` 之间选（`m_MatchType`）。另有信号灯、高速规则、路的类型、限速等 |
| 段 | `prefabs.json` 的 `sections` → `pieces` | 每个段在各种状态下用哪个件，`all`、`any`、`none` 三组条件：路口（`Intersection`）、断头（`DeadEnd`）、旁边是人行道（`Sidewalk`）、人行横道、平交道口、中央隔离带开口、高架、隧道和它们的过渡 |
| 件 | `prefabs.json` 的 `pieces` 和 `pieces.bin` | 件的网格、宽度、路口退让（`node_offset`）、路面高度、材质，车道和人行横道 |
| 代码 | `CSRoadGraph.cpp`（`GetNodeStates` 等）、`CSRoadComposition.cpp` | 照搬 CS2 的系统：判断每个路口是交叉还是断头、把相连各路的状态规则合起来定每一侧的状态、定路口形状和转角、在哪放人行横道和信号灯，再按状态挑出每段的件 |

所以自定义道路只要用 CS2 现成的段，路口行为就跟着这些段走；拼法不合 CS2 的设计（比如去掉路肩）时，代码照常判出状态，但段里没有能响应的件。

## 样条路网

路网来自样条，`NetworkFile` 只提供地形；不设文件就以 y = 0 为地面。`CSRoadSplineNetwork.cpp` 把样条转成快照的形式（节点表、按下标引用节点的边），后面的流程与读文件相同。结果与样条收集的先后无关：样条先按自身内容排好再处理，输出的边也按内容（曲线、prefab、升级、高程）排序。几何 job 遇到平局、累加时都按边的顺序，所以同一组路总是按同一个顺序建。

- 来源：挂在该 actor 下的 `ACSRoadSplineActor`、该 actor 自己的 `USplineComponent`，以及挂在它下面的其它 actor 上的 `USplineComponent`（prefab 用 `DefaultSplinePrefab`）。关卡里只有一个样条路网时，没挂在任何路网下的样条路也归它。
- 每个样条点是一个节点，每段是一条边。`USplineComponent` 的一段是三次 Hermite 曲线，正好等于一条三次 Bezier：`b = P0 + Leave/3`，`c = P1 - Arrive/3`。类型为 `Linear` 或 `Constant` 的点，后面那段按直线处理。
- 编辑样条只设一个标记，路网在下一次 `Tick` 里统一重建一次，所以拖动时每帧都会重建。拖点、增删点、拖动样条 actor 或路网 actor、改属性或逐点数据、撤销，以及代码里改样条（`UpdateSpline`），都算编辑。每次编辑都会让样条的 `UCSRoadSplineComponent::Revision` 加一；重建收集样条时，`Revision`、设置、样条和路网的变换都没变的样条直接用上次读出的结果。不经过 `UpdateSpline` 或 `PostEditChangeProperty` 的改动（例如只改点不调 `UpdateSpline`），要等这根样条下一次编辑才会被读到。材质表缓存到资产注册表有资产增、删或改名为止。示例关卡暖重建约 30–50 ms；转换出来的 Palmsland 每次约 1 s，拖动会很卡，开 `bLocalUpdate` 分 tile 后挪一个点约 30 ms，而且 `Tick` 里不等 GPU（见[局部更新](#局部更新)），样条也只转换改动附近的（见[只转换改动附近的样条](#只转换改动附近的样条)）。
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

`L_Palmsland` 原来的地面是 RoadLab 导出的显示网格（`App/TerrainRenderer.cs`）：高度图整体压低 0.18 m，只在快照自己的道路下再压到路面以下 0.3 m。地面道路的路面比曲线低，在没有路的地方贴着高度图画路时，路面会被这张网格盖住，只露出人行道，所以当时要把样条抬高约 0.4 m（`SplineNetworkShots.py` 现在仍这样画）。10-30 起地面改用 Landscape（`ConvertTerrainToLandscape`，见[Palmsland 通用测试关卡](#palmsland-通用测试关卡)），整体比高度图低 0.5 m，贴着高度图画的地面路不必再抬高。那批地形网格（`/PCGPlugins/CS2Roads/Meshes/Terrain`，196 个）和它的材质 `M_CS2_12_Terrain` 已删除，`L_Palmsland` 里只剩路和标线的静态网格，没有地面。CS2 在游戏里会在路下压平、裁掉地形（`FlattenTerrain`、`ClipTerrain`）；这里由路网的地形 edit layer 让 Landscape 跟着地面路走，见[地形 edit layer](#地形-edit-layer)。

### 只转换改动附近的样条

`bIncrementalSplines`（默认开）打开后，每次重建只转换改动附近的样条，其余的边和节点沿用上次转换的结果；转换结果与转换全部样条逐个相同（`Splines.Incremental`）。做法在 `CSRoadSplineNetwork.cpp` 的 `CSRoadSplineNetwork_Around`：

1. 按 `FSplineRoad::Id`（样条组件路径的哈希）对比上次的样条：新增的、删掉的、任何一项不同的算改动。设置和点数都没变的样条，只取变了的点两侧的段。这些段新旧两个位置在水平面上的包围盒就是改动区。
2. 改动横向最远影响到 `Reach`：吸附和接入另一条路（4 倍 `SnapDistance`），小巷让大路（最宽路的整个宽度加 `AlleyClearance`）。沿着路则要算整段：按长度等分的段只要一处变了，切出的每条边都会变，所以每条边记着它所在整段的包围盒。
3. 整段碰到改动区外扩 `Reach` 的边重做，其余照搬。重做的边要连同它碰到的一切一起转换：这些整段（上次的和这次的）以及与它们共节点的边，外扩 `Reach`，碰到的样条全部拿来转换。
4. 两项检查，不过就扩大范围再来一轮：新做的整段必须落在转换范围内，否则扩大转换范围；与重做的边共节点的边（上次的和这次的各一圈）必须在另一边找到逐位相同的（两端节点的数据也算），共用节点的数据也要相同，否则说明改动传得更远，扩大改动区。
5. 照搬的边和重做的边都按内容排序，归并成新的边表；节点按边第一次到达的顺序重新编号。节点没有编号，靠位置的二进制位加上「同一位置上第几个造出来的」区分。

以下情况转换全部样条：prefab 数据、`SplineSettings` 或地形（按 `FSavedTerrain::Serial`）变了，有样条没有 id 或 id 重复，要转换的样条超过一半，8 轮内没有收敛。

转换同时输出差量 `FSplineNetworkCache::Diff`：按内容比，这次多出的边和节点（新下标）、上次有而这次没有的（旧下标），挪动或改了的算一删一增。分 tile 的局部更新和批量房子仍按各自的输入指纹找改动的路；照搬的边逐位不变，指纹也不变，所以它们找出来的正是差量附近的路。车流预览和桥墩还是等编辑停下后整网重做。

`bVerifyIncremental` 是校验开关：每次局部转换后再转换全部样条、逐位比较，不同就在日志里报出第一处差异，并改用全部转换的结果。很慢，只用来查问题。

10-30 在 Palmsland 上测（无头编辑器，`Rebuild()`，挪一根小巷的端点 5 m，来回 4 次）：每次转换 38 根样条、一轮收敛，样条转路网从约 11 ms 降到约 2 ms（找改动 1.2 ms，归并 0.8 ms）。整次编辑从 57–66 ms 降到 43–52 ms；其中每次还顺带写了 2 个远处的积压 tile（那次测试先建的一批只写了 4 个 tile，积压 750 多个），改成编辑引起的重建只写编辑弄脏的 tile 后，每次只重写挪动点所在的 tile 和相邻的一个，29–37 ms，参与建图的路从 159–173 条降到 77 条。这一步里现在最大的是收集样条约 5 ms，主要是拷贝上次读出的样条。

### 从快照转换

`UCSRoadNetTools::ConvertToSplines` 把路网 `NetworkFile` 的路转成挂在它下面的 `ACSRoadSplineActor` 并重建，整个操作可以撤销。`ChainNetwork` 把路继续延伸的边串成一根样条：prefab 和升级相同，下一条边从上一条的终点出发，在路口只接转角 30° 以内、最直的那条。

转换保留重建结果所依赖的数据：

- 节点偏移 `NodeOffset`：CS2 的边端可以不在节点中心。Palmsland 里最远的离节点 40 m，中位数 1.78 m。
- 出城连接：GPU 几何 job 会读。

CS2 的 owner（路或节点属于某个资产、建筑或另一条路）不再保留：10-29 起读快照不读 owner，样条点上也没有 owner 的勾，GPU 几何把所有路都当作不属于任何东西的路。原版里属于资产的路高度两端之间拉直、也不参与节点高度向相连地面路坡度的调整；现在它们和普通地面路一样，节点高度跟着调，路段中间按最大坡度限高。Palmsland 只有一处带 owner：owner 1522156 的 11 条路，一条中型单行道分成高架和地面两股再汇合。与原版相比，超过 0.1 m 的值多了 63 个，最大 0.49 m，在这个资产出口节点接出去的那条普通路的端头；要还原，就调那几根样条的点。

边的顺序不保留，样条路网的边按内容排序，与快照文件里的顺序不同。几何 job 遇到平局、按边的顺序累加时，结果因此略有出入。Palmsland 转成 3,894 根样条后重建，与读文件时比：5,667 条边、4,576 个节点的数据和全部组合都相同；GPU 几何 132.6 万个值里 52 个偏差超过 1 cm，最大 2.7 cm，没有超过 0.1 m 的。把文件里的边按内容排序、倒序或打乱再建，偏差完全一样，可见这是边序本身带来的。打乱样条的先后再建，得到的边和节点逐个相同。转换后暖重建约 1.0–1.1 s，与读文件相当。

Palmsland 已经转好存成关卡 `/PCGPlugins/CSRoadNet/L_PalmslandSplines`（`Scripts/CS2Road/MakePalmslandSplines.py` 生成，29 MB）。一个路网 actor 下挂 3,894 根样条，路的数据都在样条点上；`NetworkFile` 仍指向快照，但只提供地形：路面顶点贴着它，新画的路按它量高程。路网按 256 m 的 tile 局部更新，tile 停手后烘成 Nanite，等着烘的 GPU 网格最多占 200 MiB：打开关卡后约 94 s，771 个 tile 全部换成 Nanite，见[烘成 Nanite 与显存预算](#烘成-nanite-与显存预算)。整张 Palmsland 的路面全用 GPU 网格要约 1.3 GiB（整网模式还要一次分配下 1,500 万个顶点），所以关卡默认分 tile。重新加载后，样条与现场转换的结果逐个相同；只建路面时整网 14,755,860 个顶点，比读文件少 24 个，来自边序不同。

### Palmsland 通用测试关卡

10-30 起 `L_PalmslandSplines` 是测试高架桥、大路、小路和房子构建的通用关卡，由 `Scripts/CS2Road/MakePalmslandTestLevel.py` 在 `MakePalmslandSplines.py` 的结果上补齐（只补缺的，再跑一遍不重复加）：

- 地面：快照地形转成的 Landscape（`UCSRoadNetTools::ConvertTerrainToLandscape`，见下），路网的 `bShapeTerrain` 打开，地面路下的地形由「CS2 Roads」edit layer 拉到路面，见[地形 edit layer](#地形-edit-layer)。RoadLab 的地形网格（`/PCGPlugins/CS2Roads/Meshes/Terrain`）10-30 已删除。
- 高架桥：Palmsland 自带的高架互通和桥，`bBuildBridgePiers` 打开，718 条高架路下 1,565 个桥墩（129 处放不下）。
- 大路与小路：Palmsland 原有的小路（prefab 为 `Small Road*`、`Alley`、`Gravel Road`、`Pedestrian Street` 的 2,114 根样条）在小路层，快照画好的路口保持原样，大路挪动或新画的大路压过它们时照常裁切、接驳。城东北一块 523.3 m 的平地（四周是 Palmsland 的直路）上放了一个小路区域「North-east District」，转 9.08° 对齐四周的路，西边压过一条原有的 Alley。原有小路先划进小路层再生成，所以压在这条 Alley 上的小路在生成时就裁回到它上面、接上它（见[城市布局](#城市布局)），共 187 段。之后加了两条大路：`Avenue`（Medium Road Divided）从北边的路接到城边的路，`Boulevard`（Medium Road）从西边那条 Alley 进到街区中间为止。4,083 根样条转换出 4,967 个节点、6,238 条边，大路裁掉 47 段小路，347 处接驳，整网细分全部收敛。原有小路里有一处被大路盖住、却不在画好的路口上，裁口接到固定的大路上，大路在那里切开（多出 1 条边）。
- 房子：`bBatchBuildings` 打开，全图 28,045 栋；街区里两栋特殊建筑（`Town Hall`、`Tower`）先占出预留位。

关卡 126 MB，多出来的主要是 Landscape。在完整编辑器里打开后约 2 分钟写完 783 个路面 tile 并全部烘成 Nanite。`Scripts/CS2Road/PalmslandTestShots.py` 等它写完，再从 12 个机位（全城、三个高架互通、桥墩近景、两座桥、街区俯视与街景、大路尽头、两片城区）拍到 `Saved/CS2PalmslandShots`。

`UCSRoadNetTools::ConvertTerrainToLandscape(Network, Material)`（Python `unreal.CSRoadNetTools.convert_terrain_to_landscape(network, material)`）把路网 `NetworkFile` 的可玩区高度图转成 Landscape：每个纹素一个顶点（Palmsland 3.5 m），2 × 2 个 127 格的分段一个组件，取图里放得下的整数个组件、两边均匀裁掉（Palmsland 16 × 16 个组件，4,065 × 4,065 个顶点）；高度单位按快照的 `HeightScaleOffset` 换算，逐位等于快照高度，再整体降 0.5 m，让地面路的路面留在它上面。随路网 actor 的变换摆放，材质取参数 `Material`（空时用 `M_SplineRoadDemoGround`）。关卡里已有 Landscape 时拒绝；可以撤销。路网仍按快照量高程，两边高度相同。

## 局部更新

`bLocalUpdate` 打开后，路面和标线按 `TileSize` 的方格分 tile 写。每个 tile 有一个路面组件 `RoadTile_*`、几个标线实例组件 `MarkingTile_*`（每个贴花表面、优先级和画法一个），烘好后再有一个 `NaniteTile_*`；组件和网格都不存盘。一条路归它曲线中点所在的 tile。流程在 `ACSRoadNetworkActor::AdvanceTiles` 和 `CSRoadTiles.cpp`：

1. 每条路按输入算一个指纹：prefab、曲线、高程、升级，以及两端节点的数据（prefab、位置、旋转、高程、升级、出城连接）。路和节点都没有编号：快照里的 id 读入时就换成了下标，所以一处编辑不会让别处的路换指纹。与上次相比，指纹新增或消失的路就是改动的路。离它们两圈以内的路是受影响的路；删掉的路经由它原来的节点算进来。两条路共用一个节点，就算相隔一圈。
2. 用受影响的路再加两圈建图（检测子图），跑 GPU 几何并读回。一条路的组合与 GPU 几何只取决于两圈以内的路（`Subgraph.Reach` 抽样确定，`Subgraph.Tiles` 对 Palmsland 全部路逐条核过），所以受影响的路在子图里的结果与整网相同。子图按规格原来的顺序取子集，节点数组整份传入；实体编号只比较是否相等，编号不同不影响结果。
3. 受影响的路各算一个输出指纹：输入指纹，加上读回的边与两端节点几何，以及三个实例的组合。输出变了、新增或删掉的路，在它们节点上相接的路所在的 tile 都要重写。tile 尺寸、左右舵、标线开关、actor 变换或材质设置变了，全部重写，这时子图就是整网。
4. 本批 tile 里的路，加上节点上与它们相接的路（车道图要用：本 tile 的路口连接线接着它们的车道），再加两圈，另建一个绘制子图；检测子图已经包含这些路时直接用它。车道图、细分计数和各 tile 的网格都取自这个子图：脏 tile 的实例合在一张图里算细分计数，之后逐 tile 只为本 tile 的路建组合网格、写路面，再写标线实例。子图的 GPU 几何留在池化缓冲里（`FPooledGeometry`），细分计数和各 tile 的网格图直接注册它，不再重算，也不再上传它的表。整网重建同样只算一遍。
5. 删掉不再有路的 tile。

各 tile 画出的路面三角形和标线实例合起来与整网重建逐个相同（`Tiles.Actor`、`Tiles.Subgraph`）。

车流预览和桥墩要用整网的车道图和几何。只有整网建图时（第一次建，或设置变了）才当场重做。之后的编辑只写 tile，预览保持上次的样子，等编辑停下来，即距上次重建超过 0.3 s 且没有待做的重建，再由 `Tick` 整网建图、跑 GPU 几何、建车道图，重做一次（`RefreshPreview`）。所以拖动期间车流和桥墩不跟着变，松手约 0.3 s 后更新。

地形高度图每次加载只上传一次：`FSavedTerrain::Serial` 每次加载取一个新编号，渲染线程按编号留着上传好的两张图（Palmsland 各 32 MB），后面的图直接注册。

一次重建最多写 `MaxTilesPerBatch` 个 tile（默认 16，0 为不限），批量房子的 tile 另算一份上限。其余的 tile 留到后面的帧，每帧一批。编辑引起的重建只写编辑弄脏的 tile（这次弄脏的，加上之前的编辑弄脏、还没写到的），不顺带写积压，所以拖动时既不等积压，也不为远处的积压 tile 多花时间；没有新改动的重建先写之前编辑剩下的，再写积压。同一档里离镜头近的 tile 先写（`CSRoad::TileBefore`）：编辑器里取当前透视视口的镜头，游戏里取第一个玩家的镜头，都没有时按行、列。10-30 之前积压一律按行、列写，打开 Palmsland 后要约 7 分钟（编辑器在后台时完全停）才轮到默认镜头那一片，看上去像没生成路面。积压的 tile 不靠比对输出判断，写完之前一直算脏：它们的路已经记进上次的结果，下次比对会显示没有变化，但它们画的还是旧的。`PendingTiles` 是还没写的 tile 数，`EditedTilesRebuilt` 是上次重建写的 tile 里编辑弄脏的那些（编辑引起的重建里等于 `TilesRebuilt`），日志在 tile 少时列出写了哪几个。分批写完与一次写完逐个相同（`Tiles.Batches`）。

### 编辑时不等 GPU

`Tick` 里的重建（编辑之后，或写积压的 tile）不在游戏线程上等 GPU 和渲染线程：

- 一次分 tile 重建是一个任务（`FCSRoadNetworkActor_TileJob`，挂在 actor 的 `TileJob` 上），分检测、选批、写 tile 三段。检测子图和绘制子图的 GPU 往返（几何、细分计数和读回）交给 `CSRoad::FRefineCountsReadback`：排进渲染线程就返回，之后每帧查一次拷贝到没到，到了才往下做。所以一次编辑至少隔两帧才写出来。
- tile 的路面用 `UCSMesh::EditMeshAsync` 写，不等渲染线程。编辑持有它要读的网格输入和贴花输入，跑完后再发布材质分段。一个 tile 上次的写入还没跑完时，这一批先跳过它，留到下一批。
- 任务等 GPU 期间又有编辑，不会打断它：它照常写完，下一帧再用最新的样条重建。

直接调用 `Rebuild()`（详情面板的按钮、Python、测试）仍然一次做完：GPU 往返用 `FRefineCountsReadback::Wait` 等，写 tile 之前先让还在路上的写入跑完，再用 `EditMeshSync` 写。正在等 GPU 的任务由它直接丢掉：任务到写 tile 时才动 tile 和上次的比对结果。测试用 `CSRoadTest::TickBuild` 像编辑器那样逐帧 tick 到任务结束，比较网格之前用 `FinishWrites` 等写入和贴花高度落地。

`Rebuild()` 写每个 tile 时要与渲染线程同步约 6 次，所以脏 tile 多时比整网重建还慢。`L_CityDemo` 的 79 条路分在 8 个 128 m 的 tile 里（10-27 测，子图之前）：整网暖重建 58 ms，分 tile 从零建 225 ms，没有改动时 24 ms。斜穿全区的大路挪一个点会弄脏全部 8 个 tile，耗时 194 ms；只挪特殊建筑时，路面不重写，25–30 ms。

Palmsland 转成样条（`LocalUpdateTiming.py`，3,894 根样条、5,667 条路）后用 256 m 的 tile，共 771 个路面 tile，另有约 836 个 tile 共 36,615 栋批量房子，每批 16 个。10-28 测了两轮，GPU 上另开着一个闲置的编辑器；第二轮加了让后台编辑器照常 tick 的参数，整体慢约 20%。下表给出两轮的范围：

| 重建 | 耗时 | 说明 |
| --- | --- | --- |
| 整网（不含批量房子） | 1.21–1.48 s | 与上文的 1.1–1.4 s 相当 |
| 分 tile 从零建 | 15–25 s，分 53 帧 | 第一帧整网建图（检测子图就是整网），整网布局批量房子；之后每帧的绘制子图 48–575 条路。第二轮有一帧卡了 2.8 s |
| 分 tile，没有改动 | 58–78 ms | 输入指纹都没变，不建图、不跑 GPU 几何，主要是样条转路网 |
| 挪一个样条点 5 m | 139–157 ms | 重写 2 个路面 tile（25 条路）和 1 个房子 tile。受影响的路 7 条，检测与绘制两个子图共 99 条路。样条转路网 44–50 ms，批量房子 7–9 ms，检测子图 10–15 ms，绘制子图 8–11 ms，细分计数 7–9 ms，2 个 tile 的网格 40–58 ms |
| 编辑停下后重做预览 | 196 ms，一次 | 整网建图、GPU 几何、车道图和桥墩：31,444 条车道，1,570 个桥墩 |

10-29 治拖动卡顿后，用无头编辑器在同一场景上测（`Rebuild()`，每批 4 个 tile，积压 745 个 tile；GPU 上另开着别的编辑器，数字随它的负载浮动）：挪一个样条点 5 m 从 104–132 ms 降到 57–66 ms。其中收集样条 5–8 ms（3,893 根直接用上次读出的），样条转路网约 11 ms（原来约 30 ms），材质表 0 ms（原来 9–10 ms），检测子图的 CPU 部分 2–4 ms，等 GPU 21–24 ms，4 个 tile 的网格 10–15 ms（其中网格编辑 7–10 ms）。`Tick` 里的重建不等 GPU 也不等渲染线程，等 GPU 和网格编辑这两块不再占游戏线程；按上面的分项估算，开始的一帧约 25 ms，写 tile 的一帧约 5 ms。完整编辑器里还没有实测。10-30 起样条只转换改动附近的，这一项约 2 ms；编辑引起的重建也不再顺带写积压的 tile（上面的 4 个 tile 里有 2 个是积压），同样的编辑整次 29–37 ms。

改进前后对比：用子图之前，每次编辑都整网建图、跑 GPU 几何、建车道图并重做预览。GPU 空时挪一个点约 240 ms，其中整网几何与读回 42–43 ms、车道图 38–46 ms、预览 24–32 ms、建图 11–14 ms、几何输入 12–13 ms；没有改动也要 0.20 s，从零建 25–28 s。再往前，整网几何共用之前，从零建要 112 s，挪一个点 0.50–0.57 s；批量房子改成局部重排之前，单点编辑里这一块约 200 ms，积压时每帧也要整网布局一遍。

一次重建的所有 tile 在同一帧里写，而 D3D12 的上传堆滞后 20 帧才回收，RDG 池里的缓冲要闲置 30 帧才释放。最初每个 tile 的图都把整网 GPU 几何重算并重新上传一遍：10-27 用 256 m 的 tile 不分批从零建 Palmsland 时，D3D12 报显存不足而崩溃（当时别的进程也占着 GPU）；分批之后不再崩溃，但 53 帧里提交内存仍涨了约 5.3 GB。共用几何后，同样 53 帧只涨约 2.4 GB。那时每个 tile 的路面网格还带着相邻路的边和节点实例当贴花接收面，标线也是 GPU 网格，771 个 tile 写完后路网共占约 2.7 GiB 显存。10-28 起标线改成实例化贴花、tile 烘成 Nanite，见下节。

### 烘成 Nanite 与显存预算

`bBakeNanite`（默认开，只在编辑器里）让每个 tile 的路面在停手后烘成 Nanite 静态网格，换下它的 GPU 网格。GPU 网格只在编辑和加载时临时用来显示，`GpuMeshBudget`（默认 200 MiB）限制它们合计占的显存。流程在 `CSRoadNanite.cpp` 和 `ACSRoadNetworkActor::TickBakes`：

1. tile 写完 0.3 s 内没再改，就开始烘：把 GPU 网格的位置、切线、UV、索引和材质 id 拷回 CPU。拷贝排在渲染线程已有的编辑之后，每帧只查一次到没到，不等 GPU。
2. 拷回后在工作线程里整理成网格描述（以顶点包围盒中心取整厘米为原点），提交给一个临时 `UStaticMesh`。提交用内容哈希当 GUID，所以同样的 tile 下次直接命中 DDC。
3. 交给编辑器的静态网格编译器，在它自己的线程上建 Nanite 数据和回退网格（相对误差 1，约为原三角形的 3%；光追和非 Nanite 视图画它）。位置精度固定为 1/16 cm，相邻 tile 共用的点量化到同一处。
4. 建好后，`NaniteTile_*` 组件换上这个网格，GPU 网格当帧释放；网格描述和源数据也清掉（每个 Palmsland tile 约 9 MB 内存），这个网格以后不会再建。

起烘、交给编译器和换网格这三步每帧合计只给 2 ms，离镜头近的 tile 先轮到（`CSRoad::TileBefore`，与写 tile 的顺序一样）。按住鼠标任何一个键时（拖动样条点或 gizmo、拖细节面板里的滑条、飞镜头）完全不烘：不起烘、不推进、不换网格，松开后再接着来（`Tiles.NaniteHeld`）。看的是 Slate 记下的按键，编辑器失去焦点或弹出模态窗口时 Slate 会清空它，所以不会因为在别的程序里松开而一直不烘。拖动时 tile 每帧都在重写，烘了也白烘，换网格还会顿一下。一个 tile 被重写时，它烘到一半的结果或已烘好的网格作废，等新的 GPU 网格停手后再烘。静态网格构建会去掉两个角重合的三角形，所以 Nanite 的输入三角形略少于 GPU 网格（`Tiles.Nanite`）。

拖得久了，按住期间写出的 GPU 网格会把 `GpuMeshBudget` 占满，之后的 tile 就停下来等，松开鼠标、烘焙腾出地方再写。显存真不够时也一样停：`MinFreeVideoMemory`（默认 512 MiB）是显卡驱动给本进程的预算减去已用量（DXGI 实测，含别的程序占的），每 0.25 s 读一次，再减去这之后写出的 tile。剩余低于它时，新的编辑和积压都不建，路面 tile 和房子 tile 都原样留着、记为积压，`bWaitingForVideoMemory` 置位，`FreeVideoMemoryMiB` 显示剩多少；不报错，也不清 tile 状态，显存回来后照常从离镜头近的写起。烘焙照常进行，因为它会释放显存。`UCSMesh` 自己的预检（申请超过剩余的 70% 就拒绝）也按同样处理：tile 保持原样等着，这块网格的材质也不动。11-02 之前拒绝被当成构建失败，tile 状态整个清空，下次又从零建，接着又被拒。

预算按池子实际分配的大小算：每个 GPU 网格 8 条流各向上取整到 64 KiB（包括写在绘制计数之后、给贴花当接收面的上下文实例），再加光追 BLAS（Palmsland 实测约 31 字节一个三角形）。一批 tile 先按估算挑出放得下的；写每个 tile 之前，再用它的确切顶点数和索引数核一次，放不下时这一批就停在这里，剩下的留到后面。只要还有别的 GPU 网格占着显存，就不会超出预算；没有时总放行一个，避免单个 tile 比预算还大时卡死。只剩积压的 tile 要写时，等烘焙腾出下一个 tile 的位置才重建，而且直接用上次转好的样条路网，不再重新收集和转换 3,894 根样条。

10-28 在 `L_PalmslandSplines` 上实测（完整编辑器，GPU 上另开着一个编辑器）：

| 项目 | 结果 |
| --- | --- |
| 打开关卡到 771 个 tile 全部换成 Nanite | 90–120 s（10-28、10-29 多次运行）；GPU 网格峰值 200.0 MiB；3,735 帧里 41 帧超过 100 ms，最长 663 ms，是第一帧整网检测 |
| 挪一个样条点 5 m | 重建 65–67 ms（重写 2 个 tile）；1.3–1.5 s 后烘完，期间最长帧 25–27 ms |
| 全部烘完后的路网显存 | GPU 网格 0；Nanite 根页约 16 MB（流送页面在原有的 512 MB 池里）；回退网格 55.7 万个顶点、39.8 万个三角形，约 17 MB；光追 BLAS 约 12 MB；198,266 个贴花实例在 GPU-Scene 里约 40 MB（10-29 换成贴合网格后 204,631 个，未单独量）；地形高度图 64 MB 不变 |
| 同一关卡关掉烘焙 | 路面 GPU 网格 1,316 MiB |
| 编辑器内存 | 烘焙期间比加载完关卡时多约 1.7 GB；清掉网格描述之前多约 7 GB |
| 改进前（10-28 上午） | 路网约 2.7 GiB：路面网格 1,089 MiB（其中接收面 435 MiB）、标线网格 689 MiB、64 KB 取整 334 MiB、间接参数与计数 193 MiB、BLAS 397 MiB |

加载时间受烘焙吞吐限制：预算一直占满，烘完一个才放进下一个。上表的长帧都在分 tile 流程本身，即当时检测和绘制子图的阻塞回读，与烘焙无关；10-29 起 `Tick` 里的重建不再等这些回读（见[编辑时不等 GPU](#编辑时不等-gpu)），完整编辑器里还没有重新测。

控制台变量 `CSRoad.BakeLog 1` 打出每帧的游戏线程、渲染线程、RHI 和 GPU 时间，以及烘焙各步超过 2 ms 的耗时。`CSRoad.NaniteFallbackError` 调回退网格的相对误差（默认 1）。调到 0.05 时，回退网格增至原三角形的 22%，顶点和索引缓冲多出约 100 MB。

阴影：虚拟阴影贴图的页按视图缓存，内容是第一次画时显存里已有的 Nanite 簇。tile 刚换上，或某处第一次被拉近看时，显存里只有根页；一个 256 m 的 tile，根簇误差有一两米，路缘被简化成斜坡，高出路面的部分把旁边的路面罩进阴影，表现为顺着人行道向外一两米的大块黑影。细簇流送到位后，这些页也不会重画。引擎的 `r.Nanite.VSMInvalidateOnLODDelta`（实验性，默认关）专门处理这种情况：用比需要更粗的簇画进去的页会被标脏，在流送赶上之前每帧重画。第一个 tile 换成 Nanite 时，`CSRoad::RedrawShadowsOnStreaming` 把它设为 1；ini 或控制台已经设过就不动。

10-29 在同一个路口对照（SceneCapture，FOV 60，烘完后才第一次看那里）：不开时近黑像素 32,669 个；开了只有 5,019 个，与强制整张阴影重画后的画面只差 176 个像素。代价：相机刚到时，前 3 次渲染共多重画约 1,100 个阴影页，之后为 0；稳态下开与不开的阴影页统计相同，GPU 帧时间都在 7.0 ms 左右。同一机位改用 FOV 90 时，视口和 SceneCapture 都没有复现。

## 大路与小路

道路分大路和小路两层（`LocalUpdateSystem_Plan.md`）：小路（小巷也算）事先生成或画好，之后布局不再重新生成；大路随时放入、挪动，它压住哪里的小路，哪里的小路就不出现，大路调整完后小路接进大路。`UCSRoadSplineComponent::Tier` 默认为 `Main`（详情面板显示 Main Road），设为 `Alley`（显示 Small Road）的样条路是小路。建路网前，`CSRoadSplineNetwork.cpp` 的 `ClipAlleys` 把小路被大路覆盖的段裁掉。覆盖范围是大路半宽加小路半宽，再加 `SplineSettings.AlleyClearance`，且两条路高差不超过 `JunctionHeight`；从上方跨过的不算。

- 裁口拉到大路中线，带 `bJoin` 标记，按 T 形路口接上大路，节点度数大于 1 时也照样接。
- 小路几乎顺着大路走时（进出大路的覆盖范围），拉过去的裁口会以 1° 左右的角度贴着大路接上，成为一个细长的楔形路口。这种路口的 GPU 节点几何算不出来，曾经让 GPU 挂起（`DXGI_ERROR_DEVICE_HUNG`，编辑器随之退出）。所以裁口拉到大路上之后，沿裁口的来向往回探两倍覆盖距离：探点仍在大路的覆盖范围里，说明两条路夹角小于 30°，裁口的最后一个控制点就改成从大路的法线方向接入，即垂直接上。顺着大路尽头直着延伸出去的裁口不受影响（`Splines.Alleys` 的「nearly along」一组）。大路与大路之间的小角度相交还没有这样的保护。
- 一条小路正好横穿大路时，两个裁口落在同一点，成为十字路口。
- 裁口不是画出来的点：转换出来的固定样条（`bAdjustSegments` 关）上的裁口，也像开着调整的样条一样吸附到 `SnapDistance` 内的节点、跟着节点走，并且可以接到固定的大路上，大路在接点处切开；两根样条其余的点照旧固定（`Splines.Alleys` 的「moved fixed road」）。10-30 之前两边都固定时裁口接不上，留下一个圆头断头。
- 画好的路口保持原样：小路的节点（样条点加 `NodeOffset`）落在大路的节点上时，这个路口是画出来的，经过它的被覆盖段不裁。共用节点的距离与建路网相同：两根样条都开 `bAdjustSegments` 时是 `SnapDistance`，否则 1 cm；高差不超过 `JunctionHeight`。快照转出来的小路与大路的路口都是这样。大路挪开后，小路的节点不再落在大路的节点上，被覆盖的段照常裁切、接上（`Splines.Alleys` 的「drawn tee」「drawn crossing」「moved road」）。
- 样条本身不改，大路挪走或删掉后，小路按原样恢复。
- 裁切前样条先按内容排序，结果与样条收集的先后无关。小路离两条大路恰好一样近时（比如两条大路镜像对称、在小路尽头相交），裁口接到排在前面的那条。
- 查找：大路逐段采样成走廊，走廊的包围盒外扩（大路半宽、各小路里最远的覆盖距离和 1 m）后分进 32 m 的方格，小路的采样点只查所在方格里的走廊，按原顺序比较，结果与逐条比较逐位相同；离所有走廊都远的小路不采样。Palmsland 2,293 根小路的裁切约 120 ms。

小路之间的 T 形路口在外侧会画出圆头。`City.AlleyJunctions` 已确认这些路口在拓扑上已经接上，是否与 CS2 里 `Alley` 的画法一致尚未核对。

## 地形 edit layer

`bShapeTerrain`（默认关，只在编辑器里）让关卡的 Landscape 跟着地面路走。路网在 Landscape 上建一个自己的 procedural edit layer「CS2 Roads」（`UCSRoadTerrainLayer`，放在其它层上面，`CSRoadTerrainLayer.h`），每次整网建预览（第一次建，以及编辑停下约 0.3 s 后）都把地面路的足迹交给它；足迹或设置变了，就让 Landscape 重新合并。层下面的地形不变，关掉开关或删掉路网，这个层随之删除，地形恢复原样。

- 足迹：整网图里的每条边，跳过组合带 `Elevated` 或 `Tunnel` 的（桥、高架、隧道），取 GPU 几何的路面：边本身的两段（`EdgeGeometry` 的 `m_Start`、`m_End`）和它在两端路口里的那一份（`StartNodes`、`EndNodes` 的左右两半）。每段按左右边线的中线切成约 4 m 一节，高度取中线处的路面高度，平地半宽是两条边线距离的一半加 `TerrainMargin`（默认 100 cm）。路面离快照地面超过 4 m 的节（CS2 没标成高架的匝道）不动地形。各节按包围盒再外扩 `TerrainShoulder` 分进 16 m 方格，上传到 GPU 后常驻，足迹变了才重新上传。第一版用节点到节点的曲线高度，GPU 几何在节点处调过高度、路段中间限过坡，实际路面比曲线低的地方，地形反而盖住了路面。
- 合并（`Shaders/Private/CSRoad/CSRoadTerrain.usf`）：Landscape 每合并一批组件，每个顶点在所在方格里找覆盖它最多的段（一样多取更近的）。半宽内覆盖为 1，往外 `TerrainShoulder`（默认 800 cm）内按 smoothstep 降到 0。目标高度是段上中心线的高度加 `TerrainHeightOffset`（默认 −30 cm），按「覆盖 × `TerrainInfluence`」在原有高度和目标之间插值，高处削低、低处填高。
- 重叠处取最低：顶点落在几段的平地半宽里时（路口的几块、分隔带端头、两条路贴在一起），目标取这些段里最低的路面，地面不会从任何一块路面里顶出来；只在肩部时才取覆盖最多的那段。之前一律取覆盖最多的段，路口中间的分隔带端头会冒出一块块草地。
- 改 `CS2 Road|Terrain` 分类的设置只重做预览，不重建路面。
- 合并后的地形随关卡存盘，游戏里用的是存下的结果。`MakePalmslandTestLevel.py` 只打开开关，层在第一次用编辑器打开关卡时才生成并合并，之后要存一次盘。`L_PalmslandSplines` 10-30 已这样存过：层和合并结果都在关卡里，重新打开后不再生成层，关卡也不变脏。

## 城市布局

按 `LocalUpdateSystem_Plan.md` 的顺序：先放特殊建筑占出预留位，再一次性生成小路，之后随时放入大路，最后批量补房。几何部分是 `CSCityLayout.h` 里的纯函数（UE 厘米，俯视平面），不依赖 actor。

- **特殊建筑** `ACSCityBuildingActor`：闭合轮廓样条 `UCSCityOutlineComponent` 取凸包，挤出 `Height`；凸包外扩 `Setback` 就是它的预留位。改轮廓、移动、改属性或撤销时重建自身，并通知所属路网。所属路网由 `ACSRoadNetworkActor::NetworkOf` 决定：挂在哪个路网下就归谁，否则归关卡里唯一的样条路网。
- **小路区域** `ACSCityAlleyRegion`：`Generate` 把盒子的俯视矩形当成一块，每次垂直于长边把块切成两半，直到每块都不长于 `BlockSize`。切线落点在块中部抖动（`Jitter`、`TurnJitter`）；离预留位外扩 `Clearance` 后的范围有交叉，或切后有一块窄于 `MinBlockSize`，就换下一个落点，都不行的块保持完整。`bBoundaryAlleys` 时区域四边也是小路，跨过预留位的部分去掉。随机数取自块在切分树里的路径，所以同样的区域、预留位和设置得到同样的小路。生成结果是挂在路网下、`Tier = Alley` 的直线样条路，再次生成会替换上次的结果；生成之后它们与手画的路没有区别。
- **生成时让开已有的小路**（`CSCity::CutAlleys`）：区域里已经有小路层的路（其它区域生成的、手画的、快照转来的）时，新小路落在它覆盖范围里的段永久去掉。覆盖范围与路网裁切相同：两边半宽加 `SplineSettings.AlleyClearance`，高差不超过 `JunctionHeight`。裁口拉到那条路的中线上接上，一段横穿时两个裁口落在同一点；夹角小于 30° 的裁口不拉过去，停在覆盖范围边上；短于 `MinBlockSize` 四分之一的残段去掉（`City.AlleyCuts`、`City.RegionCut`）。两边都是一次生成、不再重排的小路，所以这一刀是永久的。大路不在生成时裁，而是在建路网时按当时的位置裁（见[大路与小路](#大路与小路)），大路挪开后小路会回来。10-30 之前生成器不看已有的路：Palmsland 西边那条原有的 Alley 划进小路层后，压在它上面的街区小路没人再裁，一条小路贴着它 5.4 m 平行走了 138 m，几条越过它 4 m 就到头，形成细长路口，节点网格细分不收敛，见[路口接缝与细分](#路口接缝与细分)。
- **批量房子**（路网的 `bBatchBuildings`、`Lots`）：每根样条的每一段两侧每隔 `Frontage` 设一个候选位。候选位的身份由样条（actor 名加组件名的哈希）、段号、槽位和朝向侧决定，进深、高度和颜色都由身份派生。候选位离建成后各条道路（小路已裁切）的路边至少半个 `Setback`，离自己面对的那条路是整个 `Setback`，也不能压到预留位；互相重叠时按身份派生的优先级取舍，只与重叠的候选位比较。所以改动一条路，只影响它附近的房子。房子按路面的 tile 网格分组写网格（`BuildingTile_*`），内容哈希没变的 tile 不重写。和 CS2 一样，只有可以划区的路才有候选位（prefab 带 zone block：小路、中路、大路、小巷、碎石路、步行街；高速和各种桥都没有）。路网有快照地形时，房子立在地面上（四角里最低处再往下 0.5 m），面对的路比那块地高出或低于 3 m 以上的地方（桥、匝道、路堑）不放房子；没有地形时仍按路的高度放。
- **填满街块**（`Lots.bFillBlocks`，默认开；`Lots.FillDepth` 默认 40 m）：街块里的地按「离哪条有地块的路近就归哪条」分（比的是到各条路房子前沿的距离），房子一直往里长到分界线，再退半个 `Gap`，所以对面两排房子之间正好隔一个 `Gap`；分界线是斜的地方（街角、T 形路口），房子背面跟着斜。一条路只在它身旁分地，不分它两端以外：死胡同的尽头之外归附近别的路。前沿起 `MinDepth` 的一半之内归了别的路、被路占着或压到预留位的那一截，候选位从侧面让出来，地块变窄，至少留自己槽宽的 40%。离前沿 `FillDepth` 以内什么都碰不到的候选位（城外一侧）不填满，仍取 `MinDepth`–`MaxDepth` 之间的进深。一块地自己这条路（同一根样条）的其它段不跟它分地：弯道内侧挤到一起的房子按优先级，后者往前收，与先者留一个 `Gap`。填满的房子背面可以是斜的，所以地块是梯形；`FLot::Width`、`Depth` 是贴着前沿的那个矩形，网格房子按它摆。关掉时回到旧行为：进深随机，挡路或与别的房子重叠就整块不要（`City.LotsFill`）。
  - 槽位：填满时每段样条按整数个槽均分，槽宽尽量接近 `Frontage`（落在它的 2/3 到 1.5 倍之间），短于最窄宽度加 `Gap` 的段没有槽；所以拐弯处的短段也有房子。不填满时仍从段起点每 `Frontage` 一个槽。槽的身份仍是样条、段号、槽号和朝向侧，挪动段的一端，这段的槽跟着伸缩。
  - 判分界线用的是建成的路，路上每点带着它来自哪根样条（`FRoadSpec::Spline`，小巷被大路裁开后的几段仍算同一根），不带样条号的路对所有地块都算别的路。
  - 代价是影响范围变大：一个地块最深 `FillDepth`、最宽 1.5 个 `Frontage`，竞争者也一样，`RoadReach` 从约 50 m 涨到约 150 m（中路），一次编辑重排的房子 tile 多几圈，但只重写内容变了的。
  - 还空着的地方：路口四角各有一块边长约 7–11 m 的方地（两条路的退让加上分界线斜过去让出的那一截），巷子 T 形接上街道的两角也一样。地块总是贴着一条路的一段，转不过角；要填这些角，得另做转角地块或在空地里补楼。
- **只重排脏 tile**：一个地块只取决于自己的锚线、离它或它的竞争者够近的路和预留位，以及竞争者的锚线，再远的输入不会改变它。各类输入能影响到的地块中心范围由 `CSCity::AnchorReach`、`RoadReach`、`ReservedReach` 给出。每次重建给锚线、路和预留位算指纹，与上次相比，新增或消失的输入在影响范围内弄脏 tile，再加上积压的 tile。之后只取影响范围碰到脏 tile 的输入来采样、布局，只写回中心落在脏 tile 里的地块，结果与整网重算逐位相同（`City.LotsWindows`、`City.LocalUpdate`）。布局设置、tile 尺寸、actor 变换或材质变了，就整网重算。路是按内容区分的，长边上一处改动会弄脏它整条的影响范围：样条细分按长度等分，裁切点一动，整段小路的边都会变。

`L_CityDemo` 由 `Scripts/CS2Road/MakeCityDemo.py` 生成：两栋特殊建筑，`Old Town` 区域生成 23 条小路；`Avenue` 大路斜穿其中，裁掉 14 段小路并接上。全图共 79 条路，114 栋批量房子分在 10 个 tile 里。

一批最多重排、写 `MaxTilesPerBatch` 个房子 tile，其余留在积压里。与路面 tile 一样，编辑引起的重建只重排编辑弄脏的房子 tile（这次的和之前编辑剩下的），没有新改动时先排之前编辑剩下的，再排积压，同一档里离镜头近的先排。

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
| 贴花接收面、贴合网格的高度 | GPU，一个线程组一个段或一个条目，与路面同一张图 | RoadLab `RoadRenderer.Add`、`RoadSurfaceProjection.TryProject` | `CSRoadDecal.usf`（`ReceiversCS`、`DecalFitCS`）、`CSRoadGpuDecals.cpp` |
| 贴花实例 | CPU：条目切成实例，高度读回后挑列数、打包进实例数据；材质沿曲线摆开 | — | `CSRoadDecalInstances.cpp`、`MakeRoadDecalMaterial.py` |
| 材质分段 | GPU | — | `UCSMeshOps::AddMaterialSectionPasses` |
| Nanite 烘焙 | 回读、工作线程、编辑器的静态网格编译器 | — | `CSRoadNanite.cpp` |

几何 job 按原版依赖顺序录成一张 RDG：`InitializeNode → CalculateEdge → FlattenNode → FinishEdge → CalculateNode ×2 → CalculateIntersection → CopyNode → UpdateNode`。每条路三个实例（边、起点节点、终点节点），每个实例对应一个组合网格和一组 `CompositionParameters`。

一次重建录两张图，中间有一次 CPU 等待：

1. 几何、组合参数、接缝和细分计数。读回每个实例细分后的顶点数、三角形数，连同边与节点几何一起读回，供车道图使用。CPU 据此排布网格，生成贴花条目。
2. 路面网格（细分写出、变形、索引），从中收集贴花接收面、求出路口连接线和横道网格点的高度（`AddDecalFitPasses`），最后是材质分段。几何不再重算，用第 1 张图留在池化缓冲里的那份（`FPooledGeometry`）。高度异步读回，不等 GPU；落地后（通常一两帧）才写贴花组件，在此之前保留上一版贴花。

贴花接收面按索引位置引用路面三角形，而材质分段会重排索引缓冲，所以分段总在贴花之后。

## 路口接缝与细分

- 接缝：每个节点实例分左右两侧，左侧用曲线 0/2 和同步点 0/1，右侧用曲线 1/3 和同步点 2/3。`JunctionMatchCS` 一线程一侧，按 `SharedSection` 和 7 点有效性检查找唯一的双向对象，再改写打包参数里的 `curve.c2` 与 `lateral.c2`。对象编码为 `slot*2+side+1`（0 表示没有），双向与否由读取方核对。
- 细分：`RefineCS` 对每个基础三角形做深度优先切分，叶子顺序与 C# 一致。每条边带一个定义方向：基础边取所在材质组里第一个三角形的方向，子边继承，内部边取第一个包含它的子三角形的方向。计数、组内前缀和、写出分三步，深度上限 19 层。
- 不收敛：变形连续时弦误差随边长平方下降，19 层内必然收敛；到 19 层还要切，说明节点变形在那里有跳变，通常是两条路以很小的角度重叠、或一条路的残段比路口还短，形成细长的退化路口。C# 遇到会抛异常，这里照原样画出这些叶子，日志警告 `node mesh triangles did not converge`，并带上不收敛叶子最多的那个节点的世界坐标（`FGpuRoadMeshInput::MostUnconvergedInstance`）。这类路口也是 GPU 挂起的已知来源，见[大路与小路](#大路与小路)。

## 贴花

条目按 RoadLab 的 `RoadDecals` 生成，画成实例化网格贴花，见本节最后一条。RoadLab 的接收面和投影在 `CSRoadDecal.usf` 里移植，给路口连接线和横道的网格点求高度。金标测试拿实例画出的点直接对照 RoadLab 的贴花网格。

- 条目：`FRoadLanes::Build` 按 .NET 语义（字典插入顺序、稳定排序、`Array.BinarySearch`）建车道图。`FGpuDecalInput::Build` 按 `RoadDecals` 的调用顺序为每个贴花网格出一个条目，分三类：没有接收面的条带（每 1.5 m 一行，每行 2 个顶点），投影到节点表面的网格（每 0.5 m 一行，每行 7 个顶点，用于路口连接线），以及裁剪到表面的横道。`FitLayout` 排贴合网格的高度：投影网格照原样，横道沿脚印每 0.5 m 以内一行、每行 7 个点。路面网格必须写出这些节点上每条路的实例：分 tile 时，tile 把相邻路在这些节点上的实例作为上下文实例写进网格，写在绘制计数之后、不画，烘 Nanite 时也不读（`TileInstances`）。
- 接收面：一个节点的接收面是 `RoadRenderer.Add` 放进其 `RoadSurfaceProjection` 的路面三角形，即节点实例的全部三角形加上各条边靠近节点的三角形，按段（一个实例）依插入顺序排列。`ReceiversCS` 在 GPU 上从中筛出朝上（法线 y ≥ 0.7）、投影面积不退化的三角形，记成路面索引流里的位置。它同时给每 64 个接收槽记一个包围盒，供后面的收集整块跳过。
- 投影：`TryProject` 只看点所在 8 m 格子里登记的接收面，高度最近者胜，并列时取先到的。每个条目先把包围盒够得着的接收面按顺序收进 groupshared 候选表（1,536 个，每个记三角形、所在格子和厘米精度的包围盒），顶点只读盒子罩住它的几个候选。表装不下时，改为每 64 个顶点按自己的包围盒重新收集，必要时分几张表轮流装，结果不变。
- actor 的画法（`CSRoadDecalInstances.cpp`）：条目每 8 行切成一个实例（`DecalStripRows`），每个实例带 33 个 float（`DecalData`）：曲线控制点、本段的 t 区间、宽度、抬高、贴图区域、沿线距离的三次式、两端渐隐的权重与距离。实例的变换把单位网格的包围盒对准本段曲线控制点的包围盒（贴合网格再包住它的高度），剔除照常工作。
  - 条带用单位条带和 `M_RoadDecal`：顶点着色器按 RoadLab 条带的标架放顶点，与 RoadLab 的条带顶点相差不到 0.005 cm。
  - 路口连接线和横道是 RoadLab 投影到表面上的，画成贴合路面的单位网格和 `M_RoadDecalFitted`。`DecalFitCS` 用 RoadLab 的 `TryProject` 把网格点投到接收面上（连接线的网格就是 RoadLab 的投影网格，逐点相同），高度读回后，每个实例多带列数和每个点离曲线的高度（毫米，12 位，两个一个 float）。顶点着色器按曲线标架在平面内摆开网格点，高度取实例里的，竖直抬高 `Lift`。每段 8 行对齐 RoadLab 的行，最后一段多出的行收到曲线终点上。
  - 列数按需取 1、2、3 或 6（`DecalFitColumnChoices`）：取最少的，只要在 RoadLab 每行 7 个点上离路面不超过 0.5 cm（`DecalFitTolerance`）。有点不在路面上时用满 6 列，网格点的"在不在路面上"经插值传到像素，三角形有一角不在就不画，与 RoadLab 丢弃单元格一致。金标场景里 93% 的贴合实例只要 1 列。
  - 两者的 UV、渐隐与 `M_CS2` 贴花材质相同，在 DBuffer 里写颜色和粗糙度；RoadDecals 的渲染优先级给组件的 `TranslucencySortPriority`。实例组件不投影、不进光追和距离场。
  - 10-29 前路口连接线和横道用的是投影盒：单位盒沿曲线立起、上下各伸 0.6 m，像素着色器按场景深度反求路面点。换成贴合网格的原因见下面的对比。

10-29 在 `L_PalmslandSplines` 上用 ProfileGPU 量贴花这一遍（`DeferredDecals`），编辑器视口，每项 6 次取中位数。表中是连接线和横道在只画车道条带（路口近景 0.031–0.036 ms、低角度街景 0.11–0.12 ms、城市总览 0.22–0.23 ms）之上多出的时间：

| 画法 | 每实例 | 路口近景 | 低角度街景 | 城市总览 |
| --- | --- | --- | --- | --- |
| 投影盒 | 136 顶点、68 三角形，像素着色器 532 条指令（读场景深度、牛顿迭代） | +0.007–0.010 ms | +0.35 ms | +0.48 ms |
| 贴合网格，固定 6 列 | 63 顶点、96 三角形，397 条 | +0.008 ms | +0.29 ms | +0.54 ms |
| 贴合网格，按需取列（现行） | 多数 18 顶点、16 三角形 | +0.004–0.009 ms | +0.16 ms | +0.27 ms |

开销基本随三角形数走：总览里多数贴花不到一个像素，固定 6 列时 9.7 万个实例是 940 万个三角形，反比投影盒的 620 万还多。画面上两者几乎一样（路口近景差超过 16 级的像素 0.15%），投影盒在路口与路段交界处的一条细暗线没有了。

## 约定

- 结构布局在 `Shaders/Private/CSRoad/CSRoadTypes.ush`，C++（`CSRoadCore.h`）和 HLSL 共用。它当初由 `generate_types.py` 从 `Port/Original` 生成，C# 端定型后脚本已删除，之后手改。实体、prefab、组合表都从 1 开始编号，0 代表 `Entity.Null`。
- 计算在 Unity 米坐标（y 向上）下进行，写入 `UCSMesh` 时转换为 UE 坐标：`(x, -z, y) * 100`，再乘 actor 变换。这个转换是正交旋转；但 Unity 正面三角形的 `cross(B-A, C-A)` 与法线同向，UE 的则反向，所以写索引时交换每个三角形的第 2、3 个顶点。UV 保持原值，与静态导入一致。贴花读回路面时做反变换。
- `RoadMesh`、各 tile 的路面组件和标线实例组件存世界坐标、以绝对变换渲染，所以挂在单独的 `Root` 下，自身变换始终是单位变换（`NaniteTile_*` 放在烘焙时的原点上）。做 actor 的根时，它会带上 actor 的位置，把这个变换再乘一遍，路面就画到别处去了。actor 的变换只在写顶点时乘进去，所以移动 actor 后要重建。
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

## 问题场景关卡

`Scripts/CS2Road/MakeRoadEdgeCases.py` 生成 `/PCGPlugins/CSRoadNet/L_RoadEdgeCases`：一个分 tile（256 m）、烘 Nanite 的路网 actor，下面挂 49 组容易出问题的样条路，组与组隔开，地上有编号和说明。地面平在 0 m，不设 `NetworkFile`，路的高程就是它的高度。不开引擎时 `python MakeRoadEdgeCases.py --preview <png>` 只画布局图，并检查金标数据是否齐全、各组是否互相重叠。10-29 只写了脚本，还没在引擎里生成过关卡。

| 组 | 场景 | 看什么 |
| --- | --- | --- |
| A 小巷 | A1 被大路横穿；A2 斜穿；A3 大路顺着压；A4 大路从 10 m 上方跨过 | 裁切段数和接法，同 `Splines.Alleys` |
| | A5 两条镜像对称的大路在小巷尽头相交 | 小巷到两条大路恰好一样近（平局），接哪条与样条先后无关 |
| | A6 闭环小巷，起点被大路盖住 | 裁口绕到覆盖的另一侧 |
| | A7 3 × 3 小巷网格被大路斜穿 | 一条大路连切多处，斜着接上 |
| B 样条阈值 | B1 端点相距 0.9 / 1.1 m；B2 交叉处高差 1.9 / 2.1 m；B3 停在离另一条路 0.5 / 1.5 m 处 | `SnapDistance`（1 m）、`JunctionHeight`（2 m）两侧各一个 |
| | B4 自动切线下 3 m 的一段；B5 回头弯；B6 280 m 直路；B7 10° 交叉；B8 一个节点 8 条路 | 细分、合并、急转、窄角路口 |
| | B9 一根样条和它的复制品；B10 环路和接到环上的路；B11 相距 12 m 的两条大路；B12 37% 的坡道 | 重合、闭环、路面互相压住、陡坡 |
| C tile 边界 | C1 路口在 tile 角上，四条路沿 tile 边；C2 一条路的中点在 tile 角上；C3 跨几个 tile 的长弯路，tile 边上一条路横穿；C4 tile 角上的路口上方 12 m 有高架 | 路按中点归 tile；路口贴花要上下文实例；烘成 Nanite 后的接缝与阴影 |
| R RoadLab | R1–R18 金标的 18 个场景（不含左舵十字路口），每条边一根固定样条（`bAdjustSegments` 关）；R19–R26 是 `elevated-cross`、`curved-viaduct`、`highway-merge`、`sloped-junction` 换另外两种 prefab | 金标测试里 GPU 与 C# 有偏差的就是这四个场景 |

## 验证

测试都在 `PCGPlugins.CSRoadNet.*`。金标是 C# RoadLab 的导出，放在插件的 `Data/CS2Road/Golden`（见[数据](#数据)），包括 55 个场景（RoadScenarios × 3 种 prefab，外加左舵十字路口）和 Palmsland 全图（几何、接缝、细分计数）。

| 测试 | 比对内容 | 判定 | 当前结果 |
| --- | --- | --- | --- |
| `Composition.Scenarios`、`Composition.Palmsland` | 组合标志、数据、零件、车道、横道、组合网格顶点和索引 | 逐位 | 0 误差 |
| `Geometry.Scenarios` | Edge、Start/End、Node 几何 | 1 cm 内一致，超出份额 ≤ 0.25%，>10 cm ≤ 0.1% | 0.113%，最大 0.91 m（`elevated-cross`） |
| `Geometry.Palmsland` | 同上，5,667 条路 | 同上 | 0.037%，最大 0.49 m（不支持 owner 后，原版里属于资产的那 11 条路一带的差异；之前 0.029%，最大 0.29 m） |
| `Junction.Scenarios`、`Junction.Palmsland` | 每个节点实例两侧的接缝对象 | 不同的侧 ≤ 0.2% | 场景 856 侧（452 有对象）、Palmsland 22,668 侧（19,346 有对象），都是 0 不同 |
| `Deform.Scenarios` | 不细分时 64 万个变形顶点的位置、法线、UV | 同几何，>10 cm ≤ 0.25% | 位置 0.229%，法线 0.136%，UV 0.061% |
| `Refine.Scenarios` | 细分三角形数；细分后各角的位置、法线、UV | 三角形数之差 ≤ 0.25%；角点同几何 | 969,565 对 969,567 个三角形；位置 0.030% 超 1 cm |
| `Refine.Palmsland` | 11,334 个节点实例的细分三角形数 | 各实例差的绝对值之和 ≤ 0.25% | 8,259,067 对 8,258,645，差 0.101% |
| `Lanes.Scenarios` | 车道图（1,335 条车道，含 724 条连接线）和 178 处横道 | 逐位 | 0 误差 |
| `Decals.Scenarios` | 2,086 个贴花条目画成实例，直接对照 RoadLab 的贴花网格：条带的顶点按 `M_RoadDecal` 的顶点着色器复算；连接线和横道的高度由 `RunDecalFitBlocking` 求，网格点按 `M_RoadDecalFitted` 的顶点着色器复算。连接线对 RoadLab 投影网格的每个点（实例的点，或少列时两点之间插值），画不画逐个三角形比；横道的点对 RoadLab 裁剪出的三角形；另外强制走候选表装不下的分支再求一遍 | 网格布局不同 ≤ 1%；条带位置 0.05 cm 内，UV 与颜色 1e-3 内；连接线平面 0.01 cm 内，画不画不同 ≤ 0.5%；横道的点在裁剪三角形上（压在裁剪边上 1 cm 内也算），不在的 ≤ 0.5%；连接线和横道的高度同几何；实例的高度与 `DecalFitCS` 求出的相差 0.06 cm 内，两点之间 0.56 cm 内；每个点都在其实例的包围盒里；两条分支的高度逐位相同 | 1,020 个块与表面，0 个布局不同；9,566 个实例（贴合网格 1 列 4,012 个、2 列 82、3 列 66、6 列 178）；条带 77,136 个顶点，最差 0.0044 cm；连接线 24.3 万个点，平面 0.0031 cm，34.3 万个三角形全同；横道 3.0 万个点，124 个压在裁剪边上（最多出界 0.047 cm，`TryProject` 的重心余量），0 个更远；高度 27.3 万个值，最大差 0.47 cm；实例对 `DecalFitCS` 0.050 cm，两点之间 0.47 cm；两条分支 0 不同 |
| `Splines.Basics` | 手画小路网：十字、立交、高差 1.5 m 的交叉、T 形、固定样条交叉、吸附、按长度和转角细分、合并、闭环、未知 prefab | 拓扑和统计逐项 | 全部符合 |
| `Splines.Palmsland` | Palmsland 经 `ChainNetwork` 转成样条再建回：边和节点的数据、组合、GPU 几何；样条打乱先后再建 | 数据和组合逐个相同，GPU 几何没有超过 0.1 m 的值；打乱后边和节点逐个相同 | 5,667 条边转成 3,894 根样条（9,561 个点）；52 个值超过 1 cm，最大 2.7 cm |
| `Splines.Actor` | actor 放在离原点几公里处并转 30°，建一次，平移后再建一次：两根样条建成 4 条路；再用代码挪一个样条点，下一次 `Tick` 自己重建 | 两个网格组件保持单位变换；路面包围盒含 4 个路端，中心离路口 < 5 m | 通过 |
| `Splines.Alleys` | 小路与大路：横穿、斜穿、顺着压、立交，两条镜像对称的大路在小路尽头相交（小路离两条恰好一样近），以及小路几乎顺着大路弯过去（两头进覆盖范围、中间离开 20 m）；每组样条按每种先后各建一遍 | 裁切段数，十字或 T 形接法，裁后的小路不进大路的覆盖范围；几乎顺着走时小路与大路在接点的夹角不小于 30°；每种先后建出的边和节点逐个相同 | 通过 |
| `Splines.Incremental` | 手画小城（65 根可调样条：主路网格、接在主路上的街、横穿和顺着压的小巷、闭环、三条路交于一点、固定样条、立交）随机编辑 60 次；Palmsland 转成固定样条随机编辑 25 次。编辑有挪点（也挪到别的样条点上）、增删样条、换 prefab、改逐点数据、切换可调；其中几次开校验 | 带缓存只转换改动附近的结果与转换全部样条逐个相同；差量把上一个路网变成这一个；没有改动、样条换个先后时什么都不转换 | 通过；Palmsland 25 次全部局部转换，平均转 2.0% 的样条、一轮收敛，每次 3.5 ms（全部转换 17.4 ms）；小城 62 次里 39 次局部转换 |
| `Tiles.Actor` | 3 条东西向、4 条南北向的路，整网建与分 tile 建；不改动重建；挪一个点；删一条路 | 路面三角形、标线实例（变换、数据、材质、优先级）与整网重建逐个相同；不改动时不重写 tile；改动只重写部分 tile，其余网格代数不变 | 通过 |
| `Tiles.Batches` | 同一路网每批 2 个 tile 逐帧写完；改 tile 尺寸弄脏全部 tile，写了一批后再挪一个点 | 每批不超过 2 个 tile；挪点那次重建只写这次编辑弄脏的 tile；写完后路面三角形、标线实例与整网重建逐个相同 | 8 个 tile 分 4 批；挪点时积压 16 个，那次只写了它的 2 个；通过 |
| `Tiles.Nanite` | 同一路网分 tile 建，逐帧推进到全部烘成 Nanite；挪一个点；1 MiB 预算下换 tile 尺寸从零建 | 每个 tile 有 Nanite 数据、GPU 网格全部释放；Nanite 的输入三角形在 GPU 三角形减去退化三角形与 GPU 三角形之间；包围盒与 GPU 顶点相差 1 cm 内；标线实例不变；编辑只让重写的 tile 回到 GPU 网格，再烘回去；预算下同时最多 2 个 GPU 网格 | 8 个 tile，68,882 个三角形（GPU 72,552 个，退化 5,688 个）；1 MiB 下同时只有 1 个 |
| `Tiles.NaniteHeld` | 分 tile 建好后，往 Slate 发一个左键按下（不落在任何窗口上），逐帧推进 3 s；再发松开 | 按住期间一个 tile 也不烘；松开后一分钟内全部烘完 | 通过 |
| `Subgraph.Reach` | Palmsland（不带地形）整网建一次；取 192 条分散的路，各自只用周围 1–4 圈的路建子图 | 这条路的组合，以及边、两端节点和节点处的 GPU 几何，与整网逐位相同 | 1 圈 64/192，2 圈起 192/192 |
| `Subgraph.Tiles` | Palmsland（带地形）按 256 m 分 tile，每个 tile 的路加两圈建子图 | tile 里每条路的输出指纹（边、两端节点几何、三个组合）与整网相同 | 771 个 tile，子图最多 92 条路；5,667 条路 0 不同 |
| `Tiles.Subgraph` | 10 条东西向、10 条南北向的街（其中一条中路），转 7°，96 m 的 tile：在西北角接一段支路；挪最西一条街的北端（整条街都斜了）；删一条街；加一条斜路 | 每次编辑后路面三角形、标线实例与整网重建逐个相同；只重写部分 tile；接支路时两个子图加起来也比整网少，最东那条街的 tile 网格不变；编辑停下后预览按整网重做，车道数与整网相同 | 通过；接支路重写 2 个 tile（共 37 个），子图共 54 条路（整网 221 条） |
| `City.Alleys` | 区域里两块预留位，一块横跨中部 | 小路不进预留位外扩 `Clearance` 的范围、不出区域；同输入同结果；去掉预留位后会穿过它们；换种子结果不同 | 通过 |
| `City.AlleyJunctions` | `L_CityDemo` 区域的小路建成路网 | 没有断头 | 40 个节点、57 条边，0 个断头 |
| `City.AlleyCuts` | 一条 8 m 宽的路和一座 10 m 高的桥，各种小路：直穿、6 m 外平行、20 m 外平行、越过 3 m 到头、15° 斜穿、从桥下过、越过后只剩 13 m | 直穿的两段在中线同一点接上；压住的平行段没了，没压住的不变；越过的到头接在中线上；斜穿的停在覆盖范围边上；桥不裁；短于 15 m 的残段去掉 | 通过 |
| `Assets.CustomRoads` | 带上 `Data/CustomRoads.json` 读预制件；按带人行道的路口和小巷路口各组合一次 `Alley 2m`；另用借段写法拼两条路；两条 100 m 的小巷之间用 `Alley 2m` 连一条，再开批量房子 | `Alley 2m` 宽 2 m、不划区，地块退让 0.5 m、宽 9 m，CS2 的道路一条不少；带人行道的路口两条路肩都换成上坡过渡件，小巷路口是平路肩；借来的段按写的翻转、当中线，宽 5 m 和 3 m；路网建成 5 条边，有房子 | 通过 |
| `City.RegionCut` | 小路区域里斜穿一条已有的 Alley；再把它改成大路重新生成 | 小路除了接在它中线上的端头，都不进它的覆盖范围，且有小路接上它；改成大路后生成不裁 | 16 段小路，8 段接上它 |
| `City.Lots` | 两条相交的街和一块预留位；在 150 m 外加一条街，再去掉 | 地块离路边至少半个退让、不压预留位、互不重叠；加街后 50 m 外的地块身份和形状不变，新地块只出现在新街旁；去掉后复原 | 通过 |
| `City.LotsFill` | 四条街围成的 80 m 街块，填满与不填满各布局一次；再加一条南北穿过的 3 m 巷子（地块退让 0.5 m） | 地块离路边至少是那条路退让的一半、彼此至少隔一个 `Gap`、同输入同结果；街块里的地块往中间长（深于 35 m，不填满时最多 25 m），覆盖比不填满多 10 个百分点以上；巷子两侧都有朝它的地块，街边的地块长到与巷子地块的中线 | 填满 53%、不填满 36%；有巷子时 6 块朝巷子 |
| `City.LotsWindows` | 带斜街、宽窄街和预留位的街网，整体布局一次，再对 5 个窗口只取影响范围内的输入布局 | 窗口里每个地块的身份、角点、底高和高度与整体布局相同 | 87 个地块，0 不同（填满） |
| `City.LocalUpdate` | 转过 10° 的分 tile 路网（64 m）：画大路、挪它的一个点、挪特殊建筑、删大路 | 每次编辑后房子有变化，且与从零布局的顶点相同；挪特殊建筑只重排部分 tile | 通过；挪特殊建筑重排 14 个 tile（共 20 个） |
| `City.Actor` | 特殊建筑、小路区域、批量房子和分 tile 路网；不改动重建；在区域一角加大路；挪特殊建筑；换种子后每批 2 个房子 tile 逐帧写完；关掉批量房子 | 小路和批量房子都不进预留位；不改动时不重写房子 tile；加大路只重写附近的房子 tile；分批写完与一次写完的房子顶点相同 | 100 栋房子 8 个 tile，加大路后重写 3 个、5 个不动 |

`Refine.Palmsland` 有 21.6% 的实例三角形数不同，这在噪声范围内：扰动版 C# 自己就有 27% 的实例不同。`Decals.Scenarios` 里 `elevated-cross` 的横道只报告、不计入预算（504 个网格点都落在裁剪三角形上），原因是它的几何本身处在平局上，见下文。

渲染对照：`RoadNetworkShots.py` 在 `L_Palmsland` 里生成整张 Palmsland（5,667 条路、2,115 个组合、1,476 万顶点、1,330 万三角形、48,577 个贴花条目、1,031 万个标线三角形），与静态导入的 `Roads` 加 `Markings` 层在同机位对比。近景（4 处高架和立交，以及 2 个带横道的城市路口）平均像素差 0.03–0.29 级（满量程 255），差超过 16 级的像素不到 0.04%。全景约 4.6 级，差在高架的投影（GPU 网格的影子更长），只比路面时也是这个数。这是 10-28 之前标线还是 GPU 网格时的结果。

改成实例化贴花并烘成 Nanite 后，在 `L_PalmslandSplines` 同机位对比 Nanite 版与 GPU 网格版：路口近景平均像素差 0.82 级，差超过 16 级的像素占 0.29%。路口中间的车辙连接线若直接沿曲线平铺，会有一部分沉到路面下（金标场景最多 27.3 cm）被深度测试挡掉，显出块状明暗；现在网格点都落在路面上。

重建耗时（10-26 直接读快照建路时测的）：Palmsland 暖启动整次重建 1.1–1.4 s。其中读快照约 140 ms，建图与组合约 25 ms，组合网格约 230 ms，细分计数（含阻塞回读）150–300 ms，车道图与贴花条目约 50 ms；GPU 上路面那张图约 200 ms，贴花约 70 ms。完整编辑器里刚打开 `L_Palmsland` 时，第一次重建要等渲染线程处理完静态导入的几千个 actor，细分计数这一步可达 13 s。`SplineNetworkShots.py` 手画的 5 根样条建成 16 条路，暖重建约 50 ms；第一次还要读 prefab 数据和地形，约 0.5 s。

几何 job 本身对舍入不稳定，GPU 结果不按逐位比对。当初导出过一份扰动版，把输入的 x/z 各挪 ±1 ulp，C# 参考自身就会在场景里移动 2.7 m、在 Palmsland 里移动 7.7 m，超过 1 cm 的值分别占 0.62% 和 0.20%。GPU 与金标的偏差比扰动版 C# 小一个数量级。

完全对称的场景（如 `elevated-cross`）处在平局上，GPU 和 .NET 的取舍不同。把环境变量 `CS2ROADGOLDEN` 指向扰动导出再跑测试，GPU 与扰动版 C# 的结果一致。排查新偏差时，先用这种方法区分是移植错误还是舍入敏感。

离线比对工具：

- `Scripts/CS2Road/diff_gpu_geometry.py`：按字段统计测试写出的 `Saved/CSRoadGpu/<金标目录名>/<id>.bin`（几何）与金标的偏差。变形结果写在同目录的 `<id>.deform.bin`。
- `Scripts/CS2Road/RoadNetworkShots.py`：在 `L_Palmsland` 里把快照转成样条路（`ConvertToSplines`）并建成 GPU 路网，从相同机位分别拍静态导入版和 GPU 版。默认两边都带标线，输出到 `Saved/CS2RoadShots`；设 `CS2ROAD_SHOTS=roads` 只比路面，输出到 `Saved/CS2RoadShots_roads`。要用完整编辑器的 `-ExecCmds="py <脚本>"` 运行，`UnrealEditor-Cmd` 里拍出来是黑的。
- `Scripts/CS2Road/MakeSplineRoadDemo.py`：生成示例关卡 `L_SplineRoadDemo`，用无头编辑器的 `-ExecutePythonScript` 运行。关卡基于引擎的 Basic 模板（太阳、天空、雾），地面比样条低 0.25 m（路面比样条低 0.2 m）；8 根样条包括主路、十字路口、弯道 T 形路口、两端坡道各落在一条街上的 12 m 高架，以及一条带连接路的环路。关卡已存在时脚本会拒绝覆盖。
- `Scripts/CS2Road/SplineNetworkShots.py`：在 `L_Palmsland` 离城 900 m 的空地上用样条画一个小路网（十字、T 形、立交、3 m 短段），路网只借 Palmsland 快照的地形，从 5 个机位拍到 `Saved/CS2RoadSplineShots`。运行方式同上。`L_Palmsland` 的地形网格 10-30 已删，画面里没有地面。
- `Scripts/CS2Road/MakeCityDemo.py`：生成示例关卡 `L_CityDemo`，见[城市布局](#城市布局)。运行方式同 `MakeSplineRoadDemo.py`，关卡已存在时拒绝覆盖。
- `Scripts/CS2Road/MakePalmslandSplines.py`：把 Palmsland 快照转成样条，存成 `L_PalmslandSplines`，见[从快照转换](#从快照转换)。`CS2ROAD_NETWORK` 可换快照。运行方式同 `MakeSplineRoadDemo.py`，关卡已存在时拒绝覆盖。
- `Scripts/CS2Road/MakePalmslandTestLevel.py`：把 `L_PalmslandSplines` 补成通用测试关卡（Landscape、桥墩、小路街区和两条大路、特殊建筑、批量房子）并存盘，见[Palmsland 通用测试关卡](#palmsland-通用测试关卡)。只补缺的，可以重复跑。用无头编辑器的 `-ExecutePythonScript` 运行。
- `Scripts/CS2Road/PalmslandTestShots.py`：打开 `L_PalmslandSplines`，等全部 tile 写完并烘成 Nanite，再从 12 个机位拍到 `Saved/CS2PalmslandShots`。运行方式同 `LocalUpdateTiming.py`（完整编辑器加 `-ini` 参数），不存盘。
- `Scripts/CS2Road/MakeRoadEdgeCases.py`：生成问题场景关卡 `L_RoadEdgeCases`，见[问题场景关卡](#问题场景关卡)。运行方式同 `MakeSplineRoadDemo.py`，关卡已存在时拒绝覆盖；金标场景读 `Data/CS2Road/Golden`（或 `CS2ROADGOLDEN`），缺的跳过。
- `Scripts/CS2Road/MakeRoadDecalMaterial.py`：生成贴花母材质 `/PCGPlugins/CSRoadNet/M_RoadDecal` 和 `M_RoadDecalFitted`，用无头编辑器的 `-ExecutePythonScript` 运行，已存在时就地重建。材质着色器按下标读的实例数据布局见 `CSRoadDecalInstances.h`，两边要一起改。
- `Scripts/CS2Road/CityDemoShots.py`：打开 `L_CityDemo`，给整网、分 tile、挪大路、挪特殊建筑计时，再从 4 个机位拍到 `Saved/CS2CityShots`。用完整编辑器运行，不存盘。
- `Scripts/CS2Road/LocalUpdateTiming.py`：把 Palmsland 转成样条，开批量房子，给整网和分 tile 重建计时；从零建时每帧一批，直到没有积压。最后打开路网自己的 `Tick`、等编辑停下，预览整网重做的耗时记在日志里。结果写到 `Saved/CS2LocalUpdateTiming_<tile>_<batch>.json`。`CS2ROAD_TILE` 设 tile 尺寸（米，默认 256），`CS2ROAD_BATCH` 设每批 tile 数（默认 16）。要让帧在批与批之间过去，所以用完整编辑器的 `-ExecCmds="py <脚本>"` 运行，不存盘；再加上文[样条路网](#样条路网)里那个 `-ini` 参数，编辑器在后台时路网才会 tick，最后一步才测得到。

## 重新生成

C# 端已经定型，不再从它重新生成任何东西。共享类型布局 `CSRoadTypes.ush` 当初由 `Scripts/CS2Road/generate_types.py` 从 `Port/Original` 生成，金标由 `Scripts/CS2Road/GoldenDump` 导出；两个工具 11-02 已从插件删除，要找回看 git 历史（`beb14da` 及以前）。之后 `CSRoadTypes.ush` 直接手改，C++ 与 HLSL 共用这一份。

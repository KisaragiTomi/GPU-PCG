# PCGPlugins 结构优化方案

[返回文档索引](index.md)

本文盘点 `PCGPlugins` 插件的仓库、目录与模块结构，给出目标结构和分阶段落地步骤。工程里的其它插件不在范围内。
文中数字是 **2026-10-23 的快照**（文件数、行数、体积、未提交条目都会漂移），动手前按 [附录 A](#附录-a盘点方法与复测) 重测。

与已有文档的分工：

- [项目清理审计记录](project-cleanup-plan.md) 管死代码 / 重复代码的历史候选（`csg-*`、`gse-*` 等编号）。本文只在和结构有关时引用其编号，不重复列条目。
- [Tiny Glade 结构审查](TinyGlade/Review_Structure.md) 管 TG 编排层内部（actor 生命周期、基类上提）。本文只管 TG 在模块里的位置。

本文条目编号：`repo-*` 仓库与同步、`plug-*` 插件描述文件、`src-*` 源码与模块、`shd-*` 着色器、`doc-*` 文档、`scr-*` 脚本、`cnt-*` Content。

## 结论摘要

- **核心问题是 `ComputeShaderGenerator` 一个模块装了全部运行时代码**：229 个文件、约 11.5 万行，占插件 C++ 的 88%，`Public/` 88 个头、`Private/` 98 个文件全部平铺。按 include 关系它已经自然分成 5 层，只有 **7 处反向依赖（12 条 include、6 个文件）**。先切断这 7 处，再按层分子文件夹；**暂不拆模块**。
- **仓库安全排在整理前面**：`PCGPlugins` 是坚果云同步根，`.git` 一起在两台机器间同步；最后一次提交是 2026-09-30，之后积累了 373 条未提交变更。任何搬家式整理都要先收口在途改动，并约定只在一台机器上执行。
- **`.uplugin` 有 3 处无效或缺失的声明**：`BlacklistTargets: ["Shipping"]` 不起作用、`ShaderArchive` 引擎不读、实际依赖的 `ProxyLODPlugin` 没声明。都是零代码改动，适合第一批做。另：`Landmass` 依赖**不能删**——`Content/Landscape/` 下 6 个资产在用（旧清理记录 `critic-3` 的「零引用」结论只查了源码）。
- **Game target 目前链接不过**（静态核对，未实际编译）：`ACSGroundActor` 的地被 / 灌木段整段在 `WITH_EDITOR` 里，却被非编辑器代码调用。要不要支持打包是[待决问题](#七待决问题)，它决定运行时 / 编辑器边界整改的优先级。
- **文档、脚本、DCC 源文件散落**：插件根有 4 份计划 md（3 份未入库）和 3 个 `.hip`；`Docs/` 里混着 `.cpp`、PLY 生成表、`.obj` / `.bgeo.sc`；`Scripts/` 根目录平铺 124 个文件，且脚本之间按同级路径互相加载，搬家前要先解决加载方式。
- **2026-10-23 已执行**：在途改动分文档 / 代码两次提交（`repo-1`）；着色器按功能合并了两组并删掉一个死文件（`shd-1` 的一部分，见 [3.3](#33-shaders)）；Content 目录 / 资产名与反射名里的拼写错误已修正（`cnt-1` 的拼写部分与 `src-8`），旧名都留了 CoreRedirects。其余条目未动。

## 一、现状盘点

### 1.1 仓库与同步

```text
同步     坚果云同步根 = Plugins/PCGPlugins 本身（含 .git）
提交     最后一次 3307d60（2026-09-30）；之后 373 条未提交：修改 131、删除 61、改名 35、未跟踪 146
LFS      2008 个文件，只覆盖 uasset/umap/ubulk/uexp/7z/zip/rar
非 LFS   已入库的 .png 53 个 64.1 MB、.jpg 12 个 4.6 MB、.hip 5 个 6.0 MB、.obj 6 个 1.1 MB，都是普通 blob
.git     lfs 1.47 GB、objects 654 MB（其中 5095 个松散对象 244 MB 未打包）；fsck 无损坏
worktree 登记着另一台机器的 C:/Users/19223/.../scratchpad/pcg-main-wt（prunable）
构建产物 Binaries/、Intermediate/ 是 junction，指向同步根外的 Plugins/Binaries（1.71 GB）、Plugins/Intermediate（2.87 GB）
```

- worktree 那一行说明两台机器写的是**同一份** `.git`。目前没有冲突副本、`git fsck` 干净，但两边同时执行 git 写操作时，`index`、`refs`、pack 都可能被同步盘拆成冲突副本。
- junction 的作用是不让构建产物进同步盘。代价是 `Plugins/` 根多了两个看起来像游离垃圾的目录，容易被清理操作误删。引擎插件扫描（`PluginManager.cpp` 的 `FindPluginsInDirectory`）会遍历它们，但合计只有 44 个目录，启动开销可以忽略。

### 1.2 插件根

```text
PCGPlugins/
├─ PCGPlugins.uplugin
├─ README.md                                         # 82.5 KB；「模块总览」「设计文档」「目录结构」三张表手工维护
├─ InstanceCullingOptimization.md                    # 计划，未入库
├─ NaniteClusterHLOD_Plan.md                         # 计划，已入库
├─ VineAsyncPipeline_Plan.md                         # 计划，未入库
├─ VineRuntimeSkill_Plan.md                          # 计划，未入库
├─ MeshFuse.hip                                      # Houdini 源文件，已入库（非 LFS）
├─ MeshFuse_before_LargeFirst_20260922_145456.hip    # 手工备份，已入库
├─ VoxelTest.hip                                     # Houdini 源文件，已入库
├─ backup/                                           # VoxelTest_bak1..12.hip，已 ignore（坚果云仍同步）
├─ Binaries/ → ..\Binaries                           # junction
├─ Intermediate/ → ..\Intermediate                   # junction
├─ Config/                                           # DefaultPCGPlugins.ini（CoreRedirects）
├─ Content/                                          # 1.15 GB，2059 个文件
├─ Docs/                                             # 205 MB，219 个文件
├─ Scripts/                                          # 根目录 124 个文件 + CSSWCustom/
├─ Shaders/Private/                                  # 51 个 .usf/.ush，平铺
└─ Source/                                           # 4 个模块
```

工程根还有一份本插件的历史设计稿 `GpuMesh_DynamicMeshWrap_Plan.md`（CSMesh 对象化，2026-09-30 已标为历史）：工程根既不在 git 里也不在同步根里，另一台机器看不到它。

### 1.3 Source：四个模块

| 模块 | 类型 · 加载 · 平台 | .h / .cpp | 行数 | 说明 |
| --- | --- | --- | --- | --- |
| `ComputeShaderGenerator` | Runtime · PostConfigInit · Win64 | 102 / 127 | 115,079 | 全部运行时代码；`Public/` 88 个头里 59 个从未被其它模块 include |
| `PCGEditorProcess` | Editor · PostConfigInit · Win64 | 21 / 25 | 8,948 | EdMode、Landscape、视口叠加、HLOD 构建器；含可放置的 `ACSLandscape` 家族与 `URoadMeshComponent` |
| `GeometryScriptExtraEditor` | Editor · Default | 10 / 8 | 5,659 | Geometry Script 函数库 |
| `EditorShortcuts` | Editor · Default · Win64 | 8 / 7 | 1,630 | 独立，不依赖其它三个 |

- 模块之间只有向下的依赖：`GeometryScriptExtraEditor → ComputeShaderGenerator`，`PCGEditorProcess → 前两者`。问题集中在 `ComputeShaderGenerator` 内部。
- 工程的游戏模块 `Source/UETest574_2` 不依赖本插件任何模块；已知的外部使用方只有 Lycoris 工程里的分叉副本。

### 1.4 ComputeShaderGenerator 的实际分层

按文件名归类后统计 `#include`，模块内部已经形成下面的分层。类继承也印证这一点：浅水、藤蔓、布尔以及 `PCGEditorProcess` 的 `ACSLandscapeEditLayerBase` 都派生自 `AComputeShaderMeshGenerator`。

```text
L4  TinyGlade        77 文件 ≈36.4k 行  CSTinyGlade / CSGround* / CSHouse* / CSStairs* / CSWall* / CSSplineBlockActor
L3  功能             34 文件 ≈17.5k 行  Vine 7.1k · MeshBoolean 4.1k · ShallowWater 2.2k · Scatter 2.0k（MeshFill / Cliff / RangeGenerator）
                                         · NaniteCut HLOD 1.4k · PointBrush 0.7k
L2  生成器框架       18 文件 ≈10.0k 行  AComputeShaderMeshGenerator / AMeshGeneratorBrushCache / ACSGenerateCaptureScene
                                         / CSBoxSceneCollection / CSNaniteHeightCapture / CSNaniteCut（截面抽取）…
L1  GPU 网格         37 文件 ≈16.9k 行  UCSMesh / UCSMeshOps / 渲染组件与代理 / StaticMesh 落盘 · GPU 实例化 5.8k
L0  基础             18 文件  ≈4.5k 行  CSHelper（ComputeShaderGenerateHelper.h）/ UComputeShaderBasicFunction
                                         / SparseTileDispatch / 显存预算 / 调试绘制 / PCGPluginDebug
——  测试与基准       43 文件 ≈29.9k 行  Private/Tests、Private/Bench，占模块 26%
```

规则只有一条：**只许向下依赖，L3 各功能之间不互相 include**；模块启动文件 `ComputeShaderGenerator.cpp` 是组装根，不受限。当前违反的 7 处：

| ID | 方向 | 位置 | 依赖的东西 | 建议切法 |
| --- | --- | --- | --- | --- |
| `src-1a` | L0 → L2 | `ComputeShaderGenerateHelper.h` → `ComputeShaderSceneCapture.h` | 场景捕获类型 | CSHelper 里与捕获相关的部分拆到 L2，或改前向声明；基础头不再拖入捕获 |
| `src-1b` | L0 → L1 | `ComputeShaderBasicFunction.cpp` → `CSMesh.h` | `UCSMesh::CountedBlockingFlush` | 「计数的阻塞 flush」是基础设施，下沉到 L0，`UCSMesh` 改为调用它 |
| `src-1c` | L0 → L3 | `ComputeShaderBasicFunction.cpp` → MeshFill / Cliff / RangeGenerator 三个头 | `GenerateMapCliff` 等生成器级 BP 函数 | 移到 Scatter 的函数库；`GenerateMapCliff` 被 `UEW_CliffGenerate` 引用，要配 `FunctionRedirects` |
| `src-1d` | L1 → L2/L3 | `CSMeshOps.h/.cpp` → `ComputeShaderMeshGenerator.h`、`ComputeShaderMeshBoolean.h`、`CSBoxSceneCollection.h` | `MakeGeneratorBoxSceneOptions`、`ApplyMeshBoolean`、`ApplyMeshArrangement` | 场景抽取入口移到 L2、布尔入口移到 L3；按字节搜索 Content 未发现这三个函数的引用 |
| `src-1e` | L1 → L4 | `CSMeshOps.cpp` → `CSGroundShaperField.h` | `DisplaceGroundShapers` 的参数布局 | 它不是 UFUNCTION，直接移到 TinyGlade 侧（如 `UCSGroundOps`），BP 无感 |
| `src-1f` | L3 → L4 | `GeometryEditorActor.cpp` → `CSGroundShaperSteps.h` | `CSShaperSteps::ReserveCount`（容量台阶） | 这是 GPU 缓冲容量策略，下沉到 L1（如 `CSMeshPool.h`） |
| `src-1g` | L3 → L4 | `CSVineScatter.cpp` → `CSGroundActor.h` | `TActorIterator<ACSGroundActor>` 读 CPU 镜像高度 | 在 L2 定义地面高度查询接口，`ACSGroundActor` 实现，藤蔓只认接口 |

切断后，「L4 拿掉 TinyGlade 整层仍能编译」「L1 不认识任何 actor 类型」两条都成立，这是将来拆模块的前提。

### 1.5 Shaders、Content、Docs、Scripts

- **Shaders**：51 个文件平铺在 `Shaders/Private/`。C++ 里有 175 处虚拟路径（33 个文件），着色器之间 40 处 `#include`。新文件用 `CS*` 前缀，老文件是 `BasicFunction.usf`、`General.usf`、`VVVoxel.usf` 这类无前缀名；`General.usf` 没有任何 C++ 入口、只被别的着色器 include，实际是 `.ush`。
- **Content**：目录名有拼写错误（`CustomDrawPrimtive/`、`DrawPrimtive_Orthogonal/`、`Landscape/LanscapeRiver/`、`SpaceColonization/BlurPrinet/`、资产 `BP_CSLandscaoeBrush`；2026-10-23 已改名，见 `cnt-1`），大小写与单复数不一（`BluePrint` / `Material`、`result/` / `AutoResult/`、`Tree_Test_2` / `Tree_Test3`）；正式演示内容挂在名为 `HouseTest/` 的目录下（其中 `TinyGladeAsset/` 191 MB、1535 个文件）；`EditorShortcuts/` 与 `EditorShortcuts_BPFL/` 拆成两处。
- **Docs**：根目录 18 份 md 用了三种命名（kebab-case 5 份、`主题_Plan` / `主题_Framework` 5 份、PascalCase 7 份，另有 `index.md`），`TinyGlade/` 已统一为 `前缀_主题.md`。[`index.md`](index.md) 漏列 8 份根目录文档，README「设计文档」表是第二份清单，两边不一致。非文档文件：`VineCpuCostBench.cpp`、`TinyGlade/lextab.py` / `yacctab.py`（pycparser 的 PLY 生成表）、`TinyGlade/out/` 的 `.obj` / `.bgeo.sc`、`TinyGlade/*.hip`。`TinyGlade/index.md` 还链接了不存在的 `doc/index.md`。
- **Scripts**：根目录 124 个文件（121 个 `.py`），按动词粗分：`TinyGladeShot*` 48、`TinyGladeMake*` 15、`TinyGladeSetup*` 13、`*QA` 9、`NaniteCutHLOD*` 8、RockShell 相关 6、Verify / Probe / Repair 9。一次性迭代如 `TinyGladeShotStairsWhere.py` 到 `…Where7.py` 共 7 版并存。

## 二、问题清单

优先级：**P0** 不处理会丢数据或让后续整理出事；**P1** 影响正确性或维护，改动小；**P2** 结构改善，按计划分批做。

| ID | 优先级 | 问题 | 证据 / 影响 |
| --- | --- | --- | --- |
| `repo-1` | P0 | ✅ 2026-10-23 已提交。373 条未提交变更横跨 Docs / Source / Content / Scripts / Shaders | 搬家会与在途改动缠在一起，git 改名追踪和同步盘都会出乱子 |
| `repo-2` | P0 | `.git` 在两台机器间被同步盘同步 | 另一台机器的 worktree 登记出现在本机仓库；并发 git 写操作有拆出冲突副本的风险 |
| `plug-1` | P1 | 三个 Editor 模块写了 `"BlacklistTargets": ["Shipping"]` | 它是 `TargetDenyList` 的弃用别名，只认 Game / Editor 等目标类型（`ModuleDescriptor.cpp`），`Shipping` 匹配不到任何东西 |
| `plug-2` | P1 | `GeometryScriptExtraEditor` 链接 `ProxyLODMeshReduction`，`.uplugin` 没声明 `ProxyLODPlugin` | 现在能用只因为 `MeshModelingToolset` 恰好把它带了起来 |
| `plug-3` | P1 | `.uplugin` 的 `ShaderArchive` 字段 | 5.7.4 引擎不读（README 已注明），留着误导 |
| `src-1` | P1 | 7 处反向依赖，见 [1.4](#14-computeshadergenerator-的实际分层) | 不先切断，分文件夹只是换个地方平铺，拆模块更无从谈起 |
| `src-2` | P1 | 重名头：`PCGPluginDebug.h` 在 `Public/`、`Private/` 各一份且逐字相同；`Private/GeometryAsync.h` 与 `GeometryScriptExtraEditor/Public/GeometryAsync.h` 只差一行注释，前者无人 include | 同名头让「用的是哪份」取决于搜索顺序；分文件夹后更难发现（对应清理记录 `xmodule-1`、`xmodule-2`） |
| `src-3` | P1 / P2 | Game target 链接不过 | `CSGroundActor.h` 无条件声明 `RebuildGroundCover`、`SetBuildingFootprint`、`RemoveBuildingFootprint`，定义却在 `CSGroundActor.cpp` 的 `#if WITH_EDITOR` 大段里，`CSGroundActor.cpp` 与 `CSHouseActor.cpp` 又在守卫外调用它们（静态核对）。工程 `Binaries/Win64/UETest574_2.exe` 停在 2026-04-29，此后没有成功的游戏构建。是否 P1 取决于[待决问题 1](#七待决问题) |
| `src-4` | P2 | `PCGEditorProcess`（Editor 模块）里有可放置类：`ACSLandscape`、`ACSLandscapeRiver`、`ACSLandscapeRoad`、`ACSLandscapeEditLayerBase`、`URoadMeshComponent` | 打包后这些类不存在，引用它们的关卡会丢 actor；纯编辑器工具则无所谓 |
| `src-5` | P2 | `ComputeShaderGenerator` 的 `Public/`、`Private/` 平铺 | 186 个文件按名字前缀找，域边界只存在于人脑里 |
| `src-6` | P2 | 测试与基准约 2.99 万行编进运行时模块 | `WITH_DEV_AUTOMATION_TESTS` 在 Development 游戏构建里同样为 1 |
| `src-7` | P2 | 三个 Build.cs 各抄一份 `PCGPLUGINS_DEBUG` 块 | `ComputeShaderGenerator` 已用 `PublicDefinitions` 导出给下游，另两份多余（清理记录 `pcgep-15`） |
| `src-8` | P1 | ✅ 2026-10-23 已修。反射名拼写错误：`ACSSHallowWaterSource`、`ReusltMesh`、`ShallowWaterSolverSoucePoint`、`DivdeCount`、`InConectivityClassifiy`、`RandomRoation`（三处）、`GetAllInstancesTransfrom`、`ResamppleByCount/ByLength` | 改名后旧名全在 `DefaultPCGPlugins.ini` 留了重定向；`ReusltMesh` 改成 `ResultMeshComponent` 而不是 `ResultMesh`，因为 `BP_CSSW_Capture` 有同名蓝图函数。`Meshs` 复数写法（`CaptureMeshsInBox` 等约 7 个反射名）未改 |
| `shd-1` | P2 | 部分完成（2026-10-23，见 [3.3](#33-shaders)）。着色器平铺、新老命名混用，`General.usf` 实为 include 文件 | 改路径要同步 175 + 40 处字符串，错一处全局 shader 启动即 fatal |
| `doc-1` | P1 | 计划文档散在插件根（4 份，3 份未入库）与工程根（1 份，无版本控制、不同步） | 未入库的计划随时可能丢；另一台机器看不到工程根那份 |
| `doc-2` | P2 | 文档有两份清单（README 表、`Docs/index.md`），互不一致；根目录三种命名 | 新文档不知道放哪、叫什么 |
| `doc-3` | P2 | `Docs/` 里混着代码、生成物、DCC 源文件 | `lextab.py` / `yacctab.py` 是可再生缓存；`.obj` / `.bgeo.sc` 是 `build_tree.py` 的输出 |
| `doc-4` | P2 | 两份 `TinyGlade.hip` 已分叉 | `D:\MyProject\Houdini\TinyGlade\TinyGlade.hip`（690,937 B，08-31）与 `Docs/TinyGlade/TinyGlade.hip`（767,612 B，09-01）内容不同，不知道哪份是真源 |
| `scr-1` | P2 | `Scripts/` 平铺 124 个文件、无索引；一次性迭代堆积 | 找脚本靠记忆；`StairsWhere` 7 个版本并存 |
| `cnt-1` | P2 | 拼写部分 ✅ 2026-10-23 已修（`CustomDrawPrimitive/DrawPrimitive_Orthogonal`、`LandscapeRiver`、`SpaceColonization/BluePrint`、`BP_CSLandscapeBrush`，无外部引用，重定向器已清）。剩命名不一、`HouseTest` 承载正式演示 | 只影响可读性；批量统一风险高，见 [第六节](#六不建议做的事) |
| `repo-3` | P2 | junction 目标放在 `Plugins/` 根，名字看不出归属 | 容易被当垃圾删；删了要整插件重编 |
| `repo-4` | P2 | 图片、`.hip`、`.obj` 不走 LFS | 每次重出图都在历史里多一份完整 blob，`objects/` 已 654 MB |

## 三、目标结构

### 3.1 插件根

```text
PCGPlugins/
├─ PCGPlugins.uplugin
├─ README.md                    # 入口：安装、模块总览、演示场景；文档清单只链接 Docs/index.md
├─ Config/
├─ Content/
├─ Docs/                        # 所有 md 都在这里，插件根不再放计划文档
├─ Scripts/
├─ Shaders/
├─ Source/
├─ SourceArt/                   # 新增：DCC 源文件（.hip / 原始 .fbx / .glb），走 LFS；或改放外部同步根（待决问题 3）
│  └─ Houdini/                  # MeshFuse.hip、VoxelTest.hip、TinyGlade.hip、Tree.hip 及其生成脚本
├─ Binaries/ → junction
└─ Intermediate/ → junction
```

- `backup/` 这类 Houdini 自动备份不进插件目录：在 Houdini 里把 `HOUDINI_BACKUP_DIR` 指到插件外，否则坚果云会同步全部备份。
- junction 目标可选地改到名字自解释的位置，例如 `Plugins/_PCGPluginsBuild/{Binaries,Intermediate}`（`repo-3`）。

### 3.2 Source：先分层、再分文件夹、暂不拆模块

`ComputeShaderGenerator` 的 `Public/` 与 `Private/` 采用同构子目录，层号见 [1.4](#14-computeshadergenerator-的实际分层)：

```text
Source/ComputeShaderGenerator/
├─ Public/
│  ├─ ComputeShaderGenerator.h  # 模块接口，留在根
│  ├─ Core/                     # L0  CSHelper、BasicFunction、General、DebugParams、SparseTileDispatchHelper、
│  │                            #     CSGpuMemoryBudget、CSGpuDebugDraw、CSBoundsVisual、GDFSampleService、PCGPluginDebug
│  ├─ GpuMesh/                  # L1  CSMesh、CSMeshOps、CSMeshBuild、CSMeshPool、CSMeshRenderComponent、CSMeshVisibilityCull、
│  │                            #     CSGpuMesh*、CSGpuTriangleUtilities、CSStaticMesh*、StaticMeshRenderDataPointSampler
│  ├─ Instanced/                # L1  CSGpuInstancedMeshComponent、CSGpuInstancedNaniteComponent
│  ├─ Generator/                # L2  ComputeShaderMeshGenerator、MeshGeneratorBrushCache、ComputeShaderSceneCapture、
│  │                            #     CSBoxSceneCollection、CSSurfaceVoxelPasses、CSDepthBrushSampleService、CSNaniteCut
│  ├─ Scatter/                  # L3  ComputeShaderMeshFill、ComputeShaderCliffGenerate、CSRangeGeneratorActor
│  ├─ MeshBoolean/              # L3  ComputeShaderMeshBoolean
│  ├─ ShallowWater/             # L3  ComputeShaderShallowWater、ComputeShaderShallowWaterVoxel
│  ├─ Vine/                     # L3  GeometryEditorActor、CSVineTube、GPUSkeletalTree、GPUSkeletalTreeComponent
│  ├─ NaniteCut/                # L3  CSNaniteCutBake、CSNaniteCutHLODActor
│  ├─ Brush/                    # L3  CSPointBrushActor
│  └─ TinyGlade/                # L4  CSTinyGlade、CSSplineBlockActor
│     ├─ Ground/                #     CSGround*
│     ├─ House/                 #     CSHouse*
│     ├─ Stairs/                #     CSStairs*
│     └─ Wall/                  #     CSWall*
└─ Private/                     # 与 Public 同构；Private 独有的文件按同一张归类表放
   ├─ …
   ├─ Tests/                    # 按域分子目录：GpuMesh/、Instanced/、TinyGlade/、MeshBoolean/ …
   └─ Bench/
```

**include 写法二选一**：

| 方案 | 做法 | 改动量 | 取舍 |
| --- | --- | --- | --- |
| A（推荐先做） | Build.cs 把各子目录加进 `PublicIncludePaths` / `PrivateIncludePaths`，`#include "CSHouseActor.h"` 写法不变 | 只改 Build.cs | 零 include 改写；要求模块内头文件名唯一（UHT 的 `.generated.h` 本来就按文件名平铺），先完成 `src-2` |
| B | 改成路径限定 `#include "TinyGlade/House/CSHouseActor.h"`（引擎惯例） | 216 个文件里 679 行 include | 依赖关系在源码里一眼可见；与另一台机器、Lycoris 副本的在途改动冲突面大 |

方案 A 的 Build.cs 片段（示例）：

```csharp
// 子目录只用于归类，不改变 #include 写法；新增子目录要同步加进这里
string[] SourceFolders = { "Core", "GpuMesh", "Instanced", "Generator", "Scatter", "MeshBoolean",
	"ShallowWater", "Vine", "NaniteCut", "Brush", "TinyGlade", "TinyGlade/Ground", "TinyGlade/House",
	"TinyGlade/Stairs", "TinyGlade/Wall" };
foreach (string Folder in SourceFolders)
{
	PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public", Folder));
	PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", Folder));
}
```

**为什么暂不拆模块**：

- 每个搬到新模块的 `UCLASS` / `USTRUCT` / `UENUM` 都会改 `/Script/` 路径，要逐个写 `ClassRedirects` / `StructRedirects` / `EnumRedirects`；2026-08-31 `AVineContainer` 跨模块迁移时 `DefaultPCGPlugins.ini` 已经积累了一整组这类重定向。
- 导出宏要从 `COMPUTESHADERGENERATOR_API` 换成新模块的宏；全局 shader 所在模块必须保持 `PostConfigInit` 加载。
- 收益主要是「编译器强制分层」，而分层用一个 include 检查脚本（附录 A 的规则）就能守住。

重新考虑拆模块的触发条件：Lycoris 只想要 L0–L1 而不想要 TinyGlade；需要打包并把 TG 排除在外；或单 DLL 的链接时间成为瓶颈。届时按层拆：`CSCore`（L0–L1）、`CSGenerators`（L2–L3）、`CSTinyGlade`（L4）。

**测试独立成模块的可行性**：42 个测试文件里只有 2 个 include 了 Private 头（`CSGpuInstancedMeshTests.cpp`、`CSShallowWaterNaniteCaptureTests.cpp`），但跨模块链接要求被测符号导出，命名空间里的自由函数要逐个补 `COMPUTESHADERGENERATOR_API`。先只做「按域分子目录」，独立模块等一次试编译统计出 LNK2019 数量再定。

**新代码的规矩**（现在就可以执行，不依赖搬家）：

- 新头默认放 `Private/`，只有别的模块要 include 时才放 `Public/`。现有 59 个只在本模块用的 Public 头不批量搬（见[第六节](#六不建议做的事)）。
- 领域相关的算子不进 `UCSMeshOps`：它只放与领域无关的网格操作。
- 可放置的 actor / 组件放运行时模块，编辑器逻辑用 `WITH_EDITOR` 包在类内部；或者明确写进类注释「仅编辑器，打包后不存在」。

### 3.3 Shaders

**2026-10-23 已合并**（入口名与 C++ 着色器类都不变，只改了 `IMPLEMENT_GLOBAL_SHADER` 的文件路径）：

| 合并后 | 原文件 | 说明 |
| --- | --- | --- |
| `CSHousePack.usf` | `CSHouseDecor.usf`、`CSHousePillar.usf`、`CSHouseTile.usf`、`CSHouseVine.usf` | 四支按 CPU 记录打包实例行的 kernel，共用 `CSHousePack_Rows`（三行缩放基 + 原点与随机数 + 剔除球）；门框砖是解析推导，仍在 `CSHouseFrame.usf` |
| `CSPackedVertex.ush` | 8 个文件各自的 `PackSnorm8888` / `PackColorBGRA`（含 `CSRockShell_`、`CSNC_`、`MBOut` 前缀版）与 `CSMeshOps.usf` 的两个解包函数 | 常驻顶点流的唯一编码定义，逐字节比对过各副本相同 |
| （删除） | `CSDirectMesh.usf` | 没有任何 shader 类、材质或脚本引用 |

刻意不合并：`CSGpuInstancedNaniteWriter.usf` 必须与 `CSGpuInstancedMesh.usf` 分开（`GPUSceneWriter.ush` 把 SceneData 切到 RW 读法）；`VineGlobalDistanceFieldProjection.ush` / `VineTriangleProjection.ush` 被 `VVVoxel.usf` 按 permutation 二选一 include；`CSGroundMaskWarp.ush` 被地面材质 `M_TinyGladeGround` 的自定义节点直接 include。

后续分目录时与 Source 同构（`Core/`、`GpuMesh/`、`Instanced/`、`Generator/`、`Scatter/`、`MeshBoolean/`、`ShallowWater/`、`Vine/`、`NaniteCut/`、`TinyGlade/`），虚拟路径随之变成 `/Plugin/PCGPlugins/Shaders/Private/TinyGlade/CSHousePack.usf`。约定：

- `.usf` 只放有入口的文件，只被 include 的一律 `.ush`。第一个要改的是 `General.usf` → `General.ush`。
- 新文件统一 `CS` 前缀；老文件名不为改名而改名，随搬家一起处理。
- 生成的文件（如 `CSGpuInstancedIndexedVertexFactory.ush`，文件头已标「生成的文件，不要手改」）和生成脚本的路径一起改。

着色器搬家必须与 C++ 里的虚拟路径在**同一个提交**里改完，并跑一次冷启动看全局 shader 编译错误与 `could not be bound` 为零。

### 3.4 Docs

```text
Docs/
├─ index.md                     # 唯一的文档清单；README 只链接这里
├─ Plan_*.md                    # 计划与设计稿（含未实施的）
├─ Design_*.md                  # 已落地机制的说明
├─ Review_*.md                  # 审计、复核、结构审查（含本插件的清理记录）
├─ Guide_*.md                   # 操作说明、移植指南
├─ History/                     # 已被取代或子系统已删除的文档，只读
├─ img/                         # 根目录文档的配图，与文档同前缀
├─ Result/                      # README 展示图（只放展示图）
├─ InstanceCulling/             # 专题目录：正文 + data/（A/B 原始数据）+ img/
├─ CSSWCustomMaterial/          # 专题目录：README + 校验输出
└─ TinyGlade/                   # 专题目录，沿用其 index.md 的命名规则
```

- 根目录沿用 `TinyGlade/` 已定的 `前缀_主题.md` 规则（英文 PascalCase、不带日期），只是前缀换成上面四个。例：`FrameQuotaScheduler_Plan.md` → `Plan_FrameQuotaScheduler.md`，`cs-scene-dirty-3d-design.md` → `Design_SceneDirty3D.md`，`project-cleanup-plan.md` → `Review_ProjectCleanup.md`。
- 进 `History/`：`ComputeShaderMeshGeneratorVoxelTriangleCache.md`、`ComputeShaderMeshGeneratorTriangleCachePitfalls.md` 及其流程图（子系统 2026-08-12 已删）、`GpuTriangleUnified_Plan.md`（已被 CSMesh 对象层取代）、`VineCpuCostBench.cpp`（CPU 版藤蔓基准）。
- 移出 Docs：`TinyGlade/lextab.py`、`yacctab.py` 删除并加进 `.gitignore`（pycparser 会自动重建）；`build_tree.py`、`measure_cards.py` 与 `.hip` 一起进 `SourceArt/Houdini/`；`TinyGlade/out/` 只保留文档真正引用的渲染图，其余加进 `.gitignore`。
- `motion-matching-plugin-porting-guide.md` 讲的不是本插件，在 `index.md` 里标「非本插件」，或移出 Docs。
- 插件根 4 份计划搬进来并入库；工程根 `GpuMesh_DynamicMeshWrap_Plan.md` 直接进 `History/`，它的链接是相对工程根写的（`Plugins/PCGPlugins/README.md`），要一并改。这几份文件名在 27 个文件里出现 45 次（含 C++ 注释与着色器注释），用脚本统一替换路径。

### 3.5 Scripts

```text
Scripts/
├─ README.md                    # 脚本索引：用途、怎么跑、谁按路径引用它
├─ Regression/                  # TinyGladeDemoRegression.py、RunPluginAutomationTests.ps1、*QA.py、TinyGladeVerify*.py
├─ Build/                       # TinyGladeMake*.py、TinyGladeSetup*.py、*RockShell* 构建、GenerateGpuInstancedVertexFactoryShader.py
├─ Shots/                       # TinyGladeShot*.py、NaniteCutHLOD 出图与视频
├─ Bench/                       # RunGpuInstancedBench.ps1、GpuInstancedBench*.py
├─ Debug/                       # DebugShowNaniteVsGpuMesh.py
├─ Houdini/                     # HoudiniMeshBooleanWeldFailureCases.py
├─ CSSWCustom/                  # 已是独立工具链，保持
└─ _lib/                        # 共享的脚本定位 / 加载工具
```

搬家前必须先处理的按路径依赖：

| 依赖方 | 被依赖的路径 | 处理 |
| --- | --- | --- |
| `Content/EditorShortcuts_BPFL/BPFL_NaniteVisualize.uasset` | `Scripts/DebugShowNaniteVsGpuMesh.py` | 先改资产里的路径再搬，或这一个保持原位 |
| 11 个脚本用 `Path(__file__).with_name(...)`、`runpy.run_path(SCRIPTS / ...)`、`os.path.join(SCRIPTS, ...)` 或同目录 `import` 加载同级脚本（QA → Setup、Repair → Make 等） | 同级文件名 | 先引入 `_lib` 里的按名查找函数，再搬 |
| `BakeRockShellPattern.py`、`VerifyRockShellGlb.py` 的 `DEFAULT_PREVIEW_RELPATH` | `Docs/TinyGlade/RockShell_Pattern*.preview.png` | Docs 改名时同步改常量 |
| `CSGpuInstancedMeshVertexFactory.cpp` 的运行时报错文本、`.ush` 文件头 | `Scripts/GenerateGpuInstancedVertexFactoryShader.py` | 与脚本一起改 |
| 29 处 C++ 源码引用（注释与测试提示文本）及 README | `Scripts/TinyGladeMake*.py` 等 | 脚本统一替换 |

**分两步**：第一步只写 `Scripts/README.md` 索引、把一次性迭代（`StairsWhere2`..`7` 等）删掉或挪进 `_archive/`（它们已入库，git 可找回），根目录保持平铺；第二步在 `_lib` 就位后再分子目录。

### 3.6 Content：只立规矩，不搬旧资产

- 新目录用单数、PascalCase、拼写正确（`Blueprints/`、`Materials/`、`Meshes/`、`Maps/`）；自动落盘产物统一叫 `AutoResult/` 并在 `.gitignore` 里。
- 正式演示和试验件分开：新的演示关卡放 `Demo/<Feature>/`，试验件放 `Sandbox/<Feature>/`。
- 拼写错误的目录与资产已于 2026-10-23 改名（见 `cnt-1`）；其余已有目录（`HouseTest/`、大小写不一的 `BluePrint/` 等）**不为统一风格而改名**，理由见[第六节](#六不建议做的事)。README「可运行的测试场景」继续作为演示内容的权威清单。

## 四、实施步骤

每一阶段一个或几个独立提交；阶段内每一步都可以单独回滚。

### 阶段 0：前置（搬家类的阶段 2–4 之前必须做完；阶段 1 只改描述文件，不受限）

1. 收口在途改动：两台机器上的会话把当前 373 条变更提交（可以分主题多次提交），或者至少打一个快照提交到分支。
2. 约定执行窗口：搬家期间只在一台机器上操作；另一台关编辑器、停掉会话，坚果云同步完成后再开工，开工前查一遍冲突副本（文件名带「冲突」）。
3. 需要回灌 Lycoris 副本的改动先移植完。文件搬家后按路径 diff 会失效，之后只能按类名 / 符号对照。

### 阶段 1：描述文件（零代码改动）

| 步骤 | 改动 | 验证 |
| --- | --- | --- |
| `plug-1` | 删掉三个 Editor 模块的 `BlacklistTargets`（Editor 类型本来就进不了非编辑器目标）；真要按配置排除就改用 `TargetConfigurationDenyList` | 编译通过 |
| `plug-2` | `.uplugin` 的 `Plugins` 加 `{ "Name": "ProxyLODPlugin", "Enabled": true, "TargetAllowList": ["Editor"] }` | 日志 `Mounting Engine plugin ProxyLODPlugin` 仍在 |
| `plug-3` | 删 `ShaderArchive` 字段，同步删 README 里对应的那句说明 | 冷启动全局 shader 正常 |
| `src-7` | `GeometryScriptExtraEditor`、`PCGEditorProcess` 的 `PCGPLUGINS_DEBUG` 块删掉，只留 `ComputeShaderGenerator` 那份 | 编译通过；`PCGPLUGINS_DEBUG=0` 环境变量仍能关掉 |
| — | 保留 `Landmass` 依赖；旧清理记录 `critic-3` 已标为不成立 | — |

### 阶段 2：文档与 DCC 文件归位（不碰代码逻辑）

1. `doc-1`：5 份散落的计划搬进 `Docs/` 并入库，批量替换 45 处引用。
2. `doc-2`：`Docs/index.md` 补齐 8 份漏列文档，README「设计文档」表改为一行链接；根目录文档按 3.4 改名，同一提交里替换所有链接。
3. `doc-3`、`doc-4`：建 `SourceArt/Houdini/`，先确定两份 `TinyGlade.hip` 哪份是真源，再搬 `.hip` 与 Houdini 脚本；删 PLY 生成表；`TinyGlade/out/` 清理。
4. `scr-1` 第一步：写 `Scripts/README.md`，清理一次性迭代。
5. 修 `TinyGlade/index.md` 里指向不存在的 `doc/index.md` 的那行。

验证：用脚本扫一遍所有 md 的相对链接与 C++ / 着色器注释里的路径，失效链接为零。

### 阶段 3：切断反向依赖（代码改动，一条一个提交）

按 `src-1e` → `src-1f` → `src-1b` → `src-1a` → `src-1g` → `src-1d` → `src-1c` 的顺序：前五条只动纯 C++ 符号（`DisplaceGroundShapers`、`ReserveCount`、`CountedBlockingFlush` 都不是 UFUNCTION），BP 无感；`src-1d`、`src-1c` 涉及 UFUNCTION 换类，放最后。`src-2`（删重名头）可以在这一阶段开头顺手做。

每一条的验证：

1. 增量构建（见下方命令）。
2. `Automation RunTests PCGPlugins` 与 TinyGlade 演示回归，失败集合与改动前逐条相同。
3. 涉及 UFUNCTION 的两条：在 `DefaultPCGPlugins.ini` 加 `FunctionRedirects`，打开 `UEW_CliffGenerate` 确认节点不红、重新保存。
4. 跑附录 A 的分层检查，违规数单调下降。

```powershell
# 增量构建；不要加 -Rebuild / Clean（源码引擎下会连引擎一起清）
& "<UE>\Engine\Build\BatchFiles\Build.bat" UETest574_2Editor Win64 Development -Project="<Project>.uproject" -WaitMutex -NoUba -MaxParallelActions=3
```

### 阶段 4：分子文件夹（一次性搬家）

1. 用 `git mv` 搬 `ComputeShaderGenerator` 的文件，保留历史；Build.cs 按 3.2 方案 A 加 include 路径。
2. 测试按域分子目录。
3. 着色器搬家可以同一提交，也可以单独一个提交，但 C++ 虚拟路径必须与 `.usf` / `.ush` 同一提交改完。
4. 删除工程的 `Intermediate/Build/Win64/x64/UETest574_2Editor/Development/Makefile.bin` 强制重建动作图，再增量构建。
5. 冷启动编辑器：全局 shader 编译错误与 `could not be bound` 为零；打开 `L_HouseGroundDemo` 等演示关卡；跑全套测试与回归。
6. 把附录 A 的分层检查落成脚本（如 `Scripts/Regression/CheckSourceLayers.py`），以后每次大改跑一次。

另一台机器在这个提交同步完成之前不要开编辑器，同步完先全量构建一次。

### 阶段 5：按需（有触发条件才做）

- `src-3`、`src-4`：确认要打包后，补 Game target 的编译检查，修 `ACSGroundActor` 的 `WITH_EDITOR` 分段，并把可放置类移到运行时模块。
- `src-6`：测试独立模块（先试编译统计导出缺口）。
- 拆模块（触发条件见 3.2）。
- `repo-3`：junction 目标改到 `Plugins/_PCGPluginsBuild/`。关编辑器 → 移动两个目录 → 用 `mklink /J` 重建 junction → 增量构建确认不触发全量重编。
- `repo-4`：LFS 扩到 `*.png`、`*.jpg`、`*.hip`、`*.fbx`、`*.glb`、`*.obj` 的**新增文件**（不改写历史）。先确认 GitHub LFS 配额。

## 五、风险与约束

| 风险 | 什么时候发生 | 怎么防 |
| --- | --- | --- |
| 同步盘冲突副本被 UBT 编进去 | 两台机器同时改同一文件；搬家后另一台仍按旧路径写 | 阶段 0 的执行窗口；每次开工先查冲突副本 |
| 增量构建假故障 | 搬文件后 obj、动作图与新路径不配套 | 删 `Makefile.bin`；`-NoUba`；必要时 `-MaxParallelActions=1` |
| BP 节点变红 | UFUNCTION 换类 | `FunctionRedirects` + 打开引用资产验证；已知引用只有 `UEW_CliffGenerate` → `GenerateMapCliff` |
| 全局 shader 启动即 fatal | `.usf` 改了路径而 C++ 没同步，或 C++ 改了没重编 | 同一提交改；改完先编译再起编辑器 |
| 资产按路径找脚本失败 | 搬 `DebugShowNaniteVsGpuMesh.py` | 先改 `BPFL_NaniteVisualize` |
| Lycoris 移植变难 | 文件搬家后两边路径不同 | 阶段 0 先移植；之后按类名对照 |
| 未入库文件丢失 | 整理时顺手删了 ignore 或未跟踪的文件 | 只删已入库的文件；未跟踪的先列清单再定 |

## 六、不建议做的事

| 事项 | 为什么不做 |
| --- | --- |
| 插件改名（如改成仓库名 `GPU-PCG`） | 所有 `/PCGPlugins/...` 内容路径都会变；README 已说明仓库名与插件名不同 |
| 模块改名（`ComputeShaderGenerator` 已名不副实） | `/Script/ComputeShaderGenerator.*` 写进了大量资产，改名要全量重定向，收益只有名字 |
| 把 59 个只在本模块用的 Public 头批量挪进 Private | Lycoris 副本等外部使用方可能 include 它们；UHT 不在乎；收益低 |
| 为统一风格批量搬动已有 Content 资产（大小写、单复数、`HouseTest` 改名） | 重定向器与 CDO 存盘的坑多，工程 Content 也引用插件资产；只对新内容立规矩。真正的拼写错误已单独修过（`cnt-1`） |
| 把大 actor 的 UPROPERTY 分组成结构体（`ACSHouseActor` 有 195 个） | 属性路径变化后存盘值没有重定向可接，要写 PostLoad 迁移 |
| 移除 `Landmass` 依赖 | `Content/Landscape/` 下 `BP_CSLandscape`、`BP_CSLRiver`、`L_LandscapeRiver` 等 6 个资产在用 |

## 七、待决问题

1. **是否要支持 Game target / 打包？** 要，则 `src-3`、`src-4` 升为 P1，并加 Game target 编译作为门禁；不要，则在 README 写明「仅编辑器」，这两条降为注释说明。
2. **同步方式**：继续用坚果云同步整个 `PCGPlugins`（含 `.git`），还是改为 git push / pull、坚果云只同步插件外的大文件？前者需要「同一时间只有一台机器执行 git 写操作」的纪律。
3. **DCC 源文件放哪**：插件内 `SourceArt/`（走 LFS、随仓库分发），还是外部 `D:\MyProject\Houdini` 同步根（不进仓库）？
4. **LFS 是否扩到图片与 `.hip`**：取决于 GitHub LFS 配额。

## 附录 A：盘点方法与复测

**分层归类表**（文件按基名匹配，先精确名、后前缀；`Private/Tests`、`Private/Bench` 单独计）：

| 层 | 域 | 文件 |
| --- | --- | --- |
| — | 组装根 | `ComputeShaderGenerator` |
| L0 | Core | `ComputeShaderGenerateHelper`、`ComputeShaderBasicFunction`、`ComputeShaderGeneral`、`ComputeShaderDebugParams`、`SparseTileDispatchHelper`、`CSBoundsVisual`、`CSFixedScreenBillboardComponent`、`GDFSampleService`、`CSGpuMemoryBudget`、`CSGpuSurfaceBudget`、`CSGpuDebugDraw`、`PCGPluginDebug` |
| L1 | GpuMesh | `CSMesh`、`CSMeshOps`、`CSMeshBuild`、`CSMeshPool`、`CSMeshRenderComponent`、`CSMeshRenderSceneProxy`、`CSMeshVisibilityCull`、`CSGpuMeshComponent`、`CSGpuMeshSceneProxy`、`CSGpuMeshTypes`、`CSGpuTriangleUtilities`、`CSStaticMeshAssetSink`、`CSStaticMeshBuildLibrary`、`StaticMeshRenderDataPointSampler`、`CSPointArrowMesh` |
| L1 | Instanced | `CSGpuInstanced*` |
| L2 | Generator | `ComputeShaderMeshGenerator`、`MeshGeneratorInternal`、`MeshGeneratorLandscapeCapture`、`MeshGeneratorBrushCache`、`ComputeShaderSceneCapture`、`CSNaniteHeightCapture`、`CSSurfaceVoxelPasses`、`CSDepthBrushSampleService`、`CSNaniteCut`、`CSBoxSceneCollection`、`CSBoxSceneCollectionImpl` |
| L3 | Scatter | `ComputeShaderMeshFill`、`ComputeShaderCliffGenerate`、`CSRangeGeneratorActor` |
| L3 | MeshBoolean | `ComputeShaderMeshBoolean`、`MeshBooleanRepair` |
| L3 | ShallowWater | `ComputeShaderShallowWater`、`ComputeShaderShallowWaterVoxel`、`CSShallowWaterRestore` |
| L3 | Vine | `GeometryEditorActor`、`CSVine*`、`GPUSkeletalTree*`、`GeometryAsync` |
| L3 | NaniteCut | `CSNaniteCutBake`、`CSNaniteCutHLODActor` |
| L3 | Brush | `CSPointBrushActor` |
| L4 | TinyGlade | 前缀 `CSTinyGlade`、`CSGround`、`CSHouse`、`CSStairs`、`CSWall`、`CSSplineBlock` |

**检查规则**：统计每个文件的 `#include "..."`，按被包含头所在的层判定；目标层号更高、或同为 L3 的不同域，即为违规。组装根与测试不检查。新增文件先补进归类表，归不进去的说明归类表需要更新。

**复测命令**：

```powershell
# 未提交条目数（按状态码分组）
git -C Plugins/PCGPlugins status --porcelain=v1 -uall | ForEach-Object { $_.Substring(0, 2) } | Group-Object

# 本插件与 ProxyLODPlugin 是否挂载（plug-2 改完后对照）
Select-String -Path Saved/Logs/UETest574_2.log -Pattern "Mounting (Project|Engine) plugin (PCGPlugins|ProxyLODPlugin)"

# 坚果云同步根
Get-ChildItem "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Explorer\SyncRootManager"
```

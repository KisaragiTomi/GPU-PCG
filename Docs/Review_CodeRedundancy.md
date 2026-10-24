# PCGPlugins 代码冗余与空指针检查审查

[返回文档索引](index.md)

本文记录 2026-10-24 对 `PCGPlugins` 生产代码的一次审查。范围是 `Source/` 下四个模块，不含 `Tests/`、`Bench/`，共 260 个文件、约 10 万行，基于提交 `2e4c70e`。审查回答三件事：

- 哪些空指针检查可以证明是多余的；
- 哪些代码重复或已经死掉；
- 审查中顺带发现了哪些真实缺陷。

文中所有条目都**还没有改代码**。行号是这个快照的，动手前请按位置重新确认。

## 结论摘要

- 只收 high / medium 置信度，共 338 条：

  | 类别 | 条数 |
  | --- | --- |
  | 多余判空（null-unnecessary） | 98 |
  | 冗余逻辑（redundant-logic） | 85 |
  | 重复（duplicate） | 66 |
  | 死代码（dead-code） | 50 |
  | 空指针风险（null-risk） | 22 |
  | 单行转发（alias） | 12 |
  | 其它缺陷 | 5 |

- **最该先处理的是约 25 处真实缺陷，而不是多余判空**，见[第二节](#二要先修的真实缺陷p0)。其中包括：
  - 一处对象被 GC 回收后仍被使用；
  - RDG lambda 悬空引用；
  - 缓冲大小写反；
  - 几处越界读写和整数除零；
  - 二十多处蓝图可达的空指针解引用；
  - 两处房子逻辑错误。
- 多余判空大多是机械性的，比如 `NewObject` 之后判空、守卫之后重复判空、内部函数的所有调用点都传非空。这类改动风险低，收益主要是可读性，适合修完缺陷后按模块批量清理。
- 值得做的结构性去重见[第三节](#三值得做的结构性去重p1)。最大的几块是：
  - 场景代理里已经没有使用者的「自持 buffer」模式；
  - 藤蔓构建里约 250 行不可达代码；
  - 悬崖与 MeshFill 约 200 行复制；
  - 新旧两条表面体素管线约 200 行复制；
  - 楼梯、墙、房子各写一份的延迟重求值机制。

| 组 | 范围 | 规模 | 条目 | 其中缺陷 / 风险 |
| --- | --- | --- | --- | --- |
| [4.1](#41-房子) 房子 | `CSHouse*` | 35 文件 / 1.9 万行 | 37 | 2 处逻辑缺陷 |
| [4.2](#42-地面--楼梯--墙--样条块) 地面 / 楼梯 / 墙 | `CSTinyGlade*`、`CSGround*`、`CSStairs*`、`CSWall*`、`CSSplineBlock*` | 42 / 1.7 万 | 38 | — |
| [4.3](#43-gpu-网格) GPU 网格 | `CSMesh*`、`CSGpuMesh*`、`CSGpuInstanced*` 等 | 37 / 1.7 万 | 43 | — |
| [4.4](#44-生成器框架与核心) 生成器与核心 | `ComputeShaderMeshGenerator*`、`ComputeShaderBasicFunction` 等 | 36 / 1.3 万 | 80 | 5 处缺陷、5 处风险 |
| [4.5](#45-功能模块) 功能模块 | 藤蔓、Nanite 截面、Mesh Boolean、浅水、MeshFill / 悬崖 | 36 / 1.9 万 | 45 | 1 处越界、2 处风险、2 处疑似缺陷 |
| [4.6](#46-编辑器模块) 编辑器模块 | `PCGEditorProcess`、`GeometryScriptExtraEditor`、`EditorShortcuts` | 74 / 1.5 万 | 95 | 15 处风险、3 处缺陷 |

## 一、审查方法与判定口径

1. **脚本扫描出线索。** 扫了三类：跨文件连续 8 行以上的完全重复块（生产代码 86 对）、在整个 `Source/` 里只出现一两次的函数名、`NewObject` 后紧跟判空这类机械模式。脚本结果只当线索用。
2. **6 个只读子代理逐条核实。** 按功能域分组，读调用点、对照 `D:\UnrealEngine-5.7.4-release` 的引擎源码，只收能用 file:line 给出证据的条目。
3. **人工复核。** 我又亲自复核了其中 30 多条，全部属实，见[第五节](#五复核记录)。

「多余判空」的判定标准是：能证明指针在该处不可能为空。具体包括以下情形：
- 刚由 `NewObject`、`MakeShared`、`MakeUnique` 创建（`NewObject` 先 `check` 再构造，不会返回空）；
- 构造函数里的 `CreateDefaultSubobject`；
- 同一函数里已经被守卫过，之后没有重新赋值；
- `IsValid(X)` 已经包含判空，再写 `X &&` 是重复的；
- `delete nullptr` 本身就是安全的；
- 内部函数的每个调用点都传非空——这种情况必须逐一列出调用点作证据。

不计入多余判空的情形见[第六节](#六按口径不报的检查)。

## 二、要先修的真实缺陷（P0）

「复核」列打 ✔ 的，是我亲自读代码核实过的。

| # | 位置 | 问题 | 后果 | 条目 | 复核 |
| --- | --- | --- | --- | --- | --- |
| 1 | `ComputeShaderGenerator/Public/MeshGeneratorBrushCache.h:16-20` | `AMeshGeneratorBrushCache` 的三张缓存渲染目标是普通 `TObjectPtr` 成员，没有 `UPROPERTY`，模块里也没有 `AddReferencedObjects` | 下一次 GC 就会回收它们，之后继续读写已释放的对象，并交给渲染线程 | 4.4 X01 | ✔ |
| 2 | `ComputeShaderBasicFunction.cpp:354` 等 12 处 | RDG pass 的 lambda 写 `[&PassParameters]`，按引用捕获了局部指针 | `GraphBuilder.Execute()` 执行 lambda 时这个局部变量已经失效，属于未定义行为 | 4.4 X02 | ✔ |
| 3 | `ComputeShaderBasicFunction.cpp:229, 234` | `CreateRWBuffer` 的元素数和每元素字节数两个实参写反 | 结果缓冲只有所需大小的 1/4，3/4 的写入被丢弃 | 4.4 X03 | ✔ |
| 4 | `ComputeShaderMeshFill.cpp:243-257`、`ComputeShaderCliffGenerate.cpp:426-440` | 循环按资产数走，每轮却读 `GenerateDatas[i]`；切片号也用错了 | 资产数多于迭代次数时触发数组越界断言；纹理数组切片写错 | 4.5 U1 | ✔ |
| 5 | `ComputeShaderBasicFunction.cpp:165-171` | `DrawLinearColorsToRenderTarget16` 写越界 | 颜色数多于像素数时堆越界写 | 4.4 X04 | |
| 6 | `ComputeShaderBasicFunction.cpp:702, 720` | `SampleSpline` 采样失败时少 Add 一项 | 后续按下标读时越界 | 4.4 X05 | |
| 7 | `GeometryScriptExtraEditor/Public/GeometryAsync.h:19-22` | 先算 `NumInput / NumThreads`，后判 `NumThreads == 0` | 输入少于每线程处理量时整数除零，直接崩溃 | 4.6 L3 | ✔ |
| 8 | `GeometryGeneral.cpp:762-765` | 循环条件该用 `&&` 却写成 `\|\|`；空网格时先执行 `% 0` | 完成后仍空转；空网格整数除零 | 4.6 L2 | |
| 9 | `GeometryGeneral.cpp:1168, 1175` | UV 层数守卫差一 | 最常见的单层 UV 网格就会解引用空指针 | 4.6 K1 | ✔ |
| 10 | `GeometryGeneral.cpp:1134, 1295, 1357` | 不判颜色层；UV 层号写死为 2 | 普通网格默认没有颜色层，一进来就崩 | 4.6 K2、K3 | |
| 11 | `GeometryMathUtils.cpp:124` 等 6 处 | 直接解引用默认为空的 `PolyPath.Path` | 蓝图引脚取默认值即崩 | 4.6 K4 | |
| 12 | `GeometryGenerate.cpp:416, 436, 467` | `VDBMeshFromActors` 不判没设网格的组件和数组里的 None | 崩溃 | 4.6 K5 | |
| 13 | `PCGEditorProcess/Private/ComputeShaderLandscape.cpp:973-977` | `SimRiver` 不判 `SpawnActor` 的结果 | 类为 None 时崩；传了非 CSSW 子类时触发致命断言 | 4.6 K7 | |
| 14 | `PCGEditorProcess/Private/ComputeShaderLandscape.cpp:442` | 在渲染线程里调用要求游戏线程的 `GameThread_GetRenderTargetResource` | 开发版触发 `check(IsInGameThread())` | 4.6 附带 | |
| 15 | `ComputeShaderBasicFunction.cpp:478, 596, 697, 829-846, 1215` | 五个蓝图可调函数不判参数或 `SpawnActor` 的结果；其中 `GenerateMapCliff` 被 `UEW_CliffGenerate` 使用 | 蓝图传 None 即崩 | 4.4 R01-R05 | |
| 16 | `GeometryGeneral.cpp:1419` 等 | `AddCustomAttribute` 永远返回 null；约 15 个 BlueprintCallable 不判 `TargetMesh` | 蓝图节点串联时崩溃 | 4.6 K8、K9 | |
| 17 | `ComputeShaderMeshFill.cpp:245`、`ComputeShaderCliffGenerate.cpp:178-185` 等 | 资产数组可能有空元素；渲染目标没有先懒创建就被使用 | 渲染线程解引用空指针 | 4.5 K1、K2 | |
| 18 | `PCGEditorProcess` 多处 | `Box` 别名属性、`GetMeshDescription`、`CreateAsset` 的结果、`EditLayer` 等未判空 | 特定条件下崩溃 | 4.6 K10-K15 | |
| 19 | `ComputeShaderGenerator/Private/CSHouseActor.cpp:702-733` | 道路绕过周长起点时，判窗用的开口表是乱序的 | 窗户的取舍可能判错 | 4.1 H02 | ✔ |
| 20 | `CSHouseActor.cpp:3886, 4041` | 尖顶和门扇组件的「失效自愈」实际不生效 | 组件被销毁后补不回来，计数却照记 | 4.1 H01 | |
| 21 | `PCGEditorProcess/Private/CSAssetProcess.cpp:56` | `CaptureMeshHeight` 的出参从来不赋值 | 蓝图拿到的永远是 None，下游静默不工作 | 4.6 K6 | ✔ |
| 22 | `ComputeShaderCliffGenerate.cpp:254-276` | 2×GroupCount.X 趟 GPU pass 的输出被随后的 Blur 整幅覆盖 | 白跑；或者本意的乒乓根本没生效 | 4.5 R1 | |
| 23 | `GeometryEditorActor.cpp:4100` | `SetHiddenInGame(false)` 下一行就被改回 true | 要么是死语句，要么是显示 bug | 4.5 R6 | |
| 24 | `GeometryMathUtils.cpp:134-206` | `ResampleByLength` 的间隔算法疑似错误；`NumIterations<2` 时写到 [-1] | 结果不对；越界写 | 4.6 P6 | |
| 25 | `GeometryGenerate.cpp:823-841, 958-1004` | 在 TaskGraph 工作线程上调用 `NewObject` 和 GeometryScript | 线程不安全 | 4.6 附带 | |

## 三、值得做的结构性去重（P1）

| 主题 | 位置 | 内容 | 条目 |
| --- | --- | --- | --- |
| 场景代理的「自持 buffer」模式已无使用者 | `CSGpuMeshSceneProxy.*` | 两个子类都是 final，都借用常驻集。可以删掉两个纯虚 hook、分配块、owned 尾段、恒为零的字段，以及 5 个零调用函数 | 4.3 G3-01、G3-02 |
| 藤蔓构建里的不可达路径 | `GeometryEditorActor.cpp` | 约 250 行 CPU 体素重打包和 5 个 helper；legacy 输出路径；一个白编译的 shader 排列 | 4.5 D1、D2、D8 |
| 死掉的 GPU 辅助函数 | `ComputeShaderBasicFunction.cpp`、`ComputeShaderMeshGenerator.cpp` | `RasterizeTriangleSoupToHeightmapRDG`、约 130 行的 `UpdateTextureArray`、`DrawCopyTexture`、`BuildTextureArray`、`BlurTextureRDG`，以及它们的 shader | 4.4 D01-D05 |
| 悬崖与 MeshFill 互相复制 | 两个 cpp | 约 200 行，其中包括上面第 4 条的共有越界、白付的阻塞回读和无用 buffer | 4.5 U1、R2、R3 |
| 新旧两条表面体素管线 | `ComputeShaderMeshGenerator.cpp:2378-2573 ↔ 2705-2978` | 约 200 行 buffer 分配与 pass 设置重复。改之前要实测 TDR 和峰值显存 | 4.4 U01 |
| 延迟重求值机制写了三份 | 楼梯、墙、房子 | 上提到 `ACSTinyGlade`，样条观测抽成 `FCSSplineChangeObserver` | 4.2 G2-01、4.1 H24 |
| 抓手 actor 写了三份 | `CSHouse{Resize,Ridge,Height}HandleActor` | 抽一个抽象中间类，Spawn / Get 改成模板 | 4.1 H10、H18 |
| 重复的头文件 | `GeometryAsync.h`、`PCGPluginDebug.h` | CSG 里那份 `GeometryAsync.h` 没人包含；`PCGPluginDebug.h` 的 Private 副本可以删（即[结构方案](Plan_PluginStructure.md)的 `src-2`） | 4.6 D1、4.4 D12 |
| GeometryScriptExtraEditor 内部复制 | `FoliageConverter`、`GeometryGenerate`、`GeometryGeneral` | 同一段前置写了 6 份；收集逻辑 3 份；`WeldVertices` 约 230 行照抄引擎算法 | 4.6 P1-P3、P5、P9 |
| 拖动时反复重算 | 房子框架容量、楼梯把手 | 每帧多次全量重算 | 4.1 H11、4.2 G2-16 |
| 顶点打包函数 | `CSGpuInstancedMeshComponent.cpp`、`CSMeshOps.cpp`、`CSMesh.cpp` | C++ 侧还有多份；shader 侧已在 10-23 合并进 `CSPackedVertex.ush` | 4.3 G3-07 |

## 四、逐组明细

「置信度」列标「已复核」的，是我亲自核实过的。

### 4.1 房子

范围：`CSHouse*`，35 个文件、约 1.9 万行。路径相对 `Source/ComputeShaderGenerator/`。37 条：null-unnecessary 11、dead-code 4、duplicate 12、redundant-logic 8、alias 2。

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| H01 | redundant-logic（缺陷） | `Private/CSHouseActor.cpp:3886, 3895, 4041, 4061` | 尖顶 / 门扇的 `bAllValid` 短路（3859 / 4025）本意是组件被销毁后自愈，但循环只在 `!IsValidIndex` 时新建，失效旧项直接 `continue`；`Discard` 只删下标 ≥ WantCount 的项，计数照样记成 WantCount（3911、4126）——失效组件永远补不回来 | 失效项原位重建并删掉 `continue`；或删掉 `bAllValid` 并改正注释 | medium |
| H02 | redundant-logic（缺陷） | `Private/CSHouseActor.cpp:730-733` | 注释说这次 `Sort` 只是兜底，不成立：环形求解会把 S0<0 的绕回段排最前（`CSHouseDoorRuns.h:140, 236-239`），596-619 先切出 0 号边、再切 N−1 号边，716 行判窗时 `CurrentOpenings` 是乱序的；而 `QueryFeatureReject` 的墩跨度判据要求按 (Edge, CenterS) 升序（`CSHouseProfile.h:1493-1496, 1541-1554`） | 在 702 之后、窗循环之前先排一次序，保留现在这次，改正 730-732 的注释 | medium（已复核） |
| H03 | dead-code | `Public/CSHouseSeam.h:370, 114` | `BuildCornerElements` / `SeamSeed` 只有单测引用；生产路径 `BuildSeamBricks`（`CSHouseActor.cpp:2658-2677`）和 `FCSHouseContact::Seed()` 各自又写了一份，单测测的不是生产路径 | 生产与单测共用一个 helper，或让单测改测生产路径 | medium |
| H04 | duplicate | `Public/CSHouseQuoin.h:98` | `IsQuoinCorner` 生产代码 0 引用（只有 `CSWallUnifyTests.cpp:394`）；`CSWall.cpp:702, 727` 把同一判据内联了两遍（阈值 1e-4 与 1e-3 不一致） | 抽共享判据，三处都调它 | high |
| H05 | duplicate | `Private/CSHouseActor.cpp:3445-3476` | 藤的三条材质判据重写了 `UCSGpuInstancedMeshComponent::GetMaterialUndrawableReason`（其余三类已改用组件版），口径已分叉：只看 `InstanceMaterial`，"还在编译"也报红 | 改调 `Component->GetMaterialUndrawableReason()` | medium |
| H06 | null-unnecessary | `Private/CSHouseActor.cpp:2217` | `!FrameComponent`：2185 的 `EnsureInstancedComponent(this, …)` 保证非空（`CSGroundShaperSteps.cpp:126-135`），2188-2189 已解引用 | 删掉 `\|\| !FrameComponent` | high |
| H07 | null-unnecessary | `Private/CSHouseActor.cpp:2009` | `!PillarBrickComponent`：1985 已 Ensure，1987-1988 已解引用 | 删掉这一项 | high |
| H08 | null-unnecessary | `Private/CSHouseActor.cpp:2360, 2407, 2411, 2416` | `RebuildFrame` 第一句 `EnsureFrameComponent` 已保证组件有效且 `Num()==1`，中间的 `Build*` 不碰组件 | 删除或改 `check` | high |
| H09 | redundant-logic | `CSHouseFrame.cpp:335`、`CSHouseDecor.cpp:426`、`CSHousePillar.cpp:158`、`CSHouseTile.cpp:564`、`CSHouseVine.cpp:852` | 上传 helper 只在元素数为 0 时返回空 SRV（`ComputeShaderGenerateHelper.h:120-124`），这五处之前都挡掉了空表 | 删除或改 `check(RecordRefs.SRV)` | high |
| H10 | duplicate | `CSHouse{Resize,Ridge,Height}HandleActor.cpp` | `ConsumeDragToHost`、`OnDetachFromHost` 和两个记账字段三份逐字相同；箭头组件 + `UpdateArrowPose` 两份，`CSStairsWidthHandleActor` 是第三份 | 抽抽象中间类（三者都是 NotBlueprintable + Transient，改继承不影响资产） | high |
| H11 | redundant-logic | `Private/CSHouseActor.cpp:2874-2902` | 每次 `RebuildFrame` 最多调 9 次 `EffectiveFrameCapacity`，每次全量重算砖层预算和 `BuildGableWalls`，拖动时每帧都走 | 每轮只算一次，用参数传下去 | medium |
| H12 | duplicate | `Private/CSHouseActor.cpp:2259, 2648, 2737, 2787, 2916, 2975` | 六个砖构建函数开头同一段 `FBrickParams` 初始化；种子表达式重复 4 次、Gable 转换重复 3 次 | 抽 `MakeFrameBrickParams` / `GetBrickSeed` / `ToBrickGables` | high |
| H13 | duplicate | `Private/CSHouseDecor.cpp:87` | `CSHouseDecor::Hash01` 与 `CSHouseVine::Hash01`（:113）逐字相同，都是导出 API；柱和瓦已用后者（`CSGpuInstancedMeshComponent.cpp:84` 是第三份） | 删掉 Decor 那份 | high |
| H14 | dead-code | `Private/CSHouseActor.cpp:59` | `CSHouse_WallUV` 0 处调用；1808 行把同一式子内联了一遍 | 删除，或让 1808 改用它 | high |
| H15 | dead-code | `Public/CSHouseProfile.h:1560` | `CSHouse_FeatureRejectText` 全工程 0 调用 | 删除 | high |
| H16 | alias | `Private/CSHouseActor.cpp:79` | `CSHouse_AppendFootprintHash` 只转发 `AppendQuantizedHash`，8 个调用点（744、1116、2487、3348、3747、3852、4013、4364），1620 已直调后者 | 内联后删除 | high |
| H17 | duplicate | Frame / Decor / Pillar / Tile / Vine 的打包函数 | 五份结构相同的 RDG 打包样板（注册外部 buffer → UAV → 空表清零 → 上传 → AddPass → SetBufferAccessFinal） | 抽模板 helper，参数用 lambda 填 | medium |
| H18 | duplicate | `Private/CSHouseActor.cpp:1495-1552；1430, 1743, 1753` | `Spawn{Edge,Height,Ridge}Handle` 三个函数体相同；`Get{Roof,Height,Base}Handle` 只差 Side | `SpawnHandle<T>` + `FindHeightHandle(Side)` | high |
| H19 | duplicate | `Private/CSHouseActor.cpp:3042, 3117, 3369, 4399；3066-3078 ↔ 3144-3156` | 四个 `Is*Drawable` 包装结构相同；接缝与窗的房体 / 墙材质 / Masked 六条检查逐字相同；组件状态检查链 4 份 | 抽三个私有 helper | medium |
| H20 | redundant-logic | `Private/CSHouseActor.cpp:915` | `GetRoofDesc` 的 `bFlat` 与 `IsFlatRoof`（1290-1293）是同一判据写两份 | 共用一个判据函数 | high |
| H21 | null-unnecessary | `Private/CSHouseActor.cpp:2520, 2534, 2559, 2581, 2605, 2638, 2713, 3086` | `Contacts` 是私有非 UPROPERTY 数组，只有 2546、2560 两处写入，写入前都查了 `IsValid`，表里不可能有空 `TSharedPtr` | 删除这些判断（`AuditContacts` 保留） | high |
| H22 | null-unnecessary | `Private/CSHouseActor.cpp:1975；4294, 4301, 4372` | `CSHouse_BuildBoundsToComponent` 的 5 个调用点都在 `Ensure*` / `IsValid` 之后；摆件路上的 nullptr 分支走不到，`Num` 相等比较恒真 | 去掉 `!Component` 与恒真比较 | medium |
| H23 | null-unnecessary | `Private/CSHouseActor.cpp:180` | `ResolveGroundAndSubscribe` 唯一调用点 1029 在 1024 的 `!GetWorld()` 提前返回之后 | 改成 `if (!Ground)` | high |
| H24 | duplicate | `Private/CSHouseActor.cpp:178-244` | 地面订阅的五个函数和三个成员与 `CSWallActor.cpp:214-256` 同一套逻辑（同属 `ACSWallBase`），已出现细微分叉 | 上提到 `ACSWallBase`（注意房子的 Ground 是 EditAnywhere，墙的是 Transient） | medium |
| H25 | duplicate | `Private/CSHouseActor.cpp:4552-4616` | `DebugBakeFrameBricksSync` 与 `CSGroundActor.cpp:3098-3164` 约 40 行同构 | 抽 editor-only 共享 helper | medium |
| H26 | duplicate | `CSHouseDecor.cpp:75；CSHouseVine.cpp:201, 499；377 ↔ 506` | 按 EdgeIndex 找墙带 3 份，(S,Z)→世界坐标 2 份（注释自己写着"逐字同源"） | 提供 `FindStrip` 与 `FWallStrip::ToWorld` | high |
| H27 | null-unnecessary | `Private/CSHouseHandleActor.cpp:27` | `MakeEditorGizmoProp` 的 7 个调用点全在构造函数里，传的都是刚 `CreateDefaultSubobject` 且已解引用的指针；头文件说 GroundShaper 也在用，已过期 | 删除或改 `check` | medium |
| H28 | null-unnecessary | `Private/CSHouseActor.cpp:2557` | `ReceiveContactFrom` 两个调用点（2547、2582）都传 `this` 给另一栋房，`!From`、`From==this` 走不到 | 删掉前两个条件 | medium |
| H29 | null-unnecessary | `Private/CSHouseActor.cpp:442, 456` | `RegisterFeatureMarker` 唯一调用点 `CSHouseFeatureMarker.cpp:337` 传 `this`，默认参数 nullptr 没人用 | 去掉默认参数，改引用 | medium |
| H30 | null-unnecessary | `Private/CSHouseFeatureMarker.cpp:290, 493` | `GetComponents` 不返回空指针（引擎 `Actor.h:3899-3906`）；`IsPieceInCurrentForm` 唯一调用点就是 290 | 删除 | medium |
| H31 | null-unnecessary | `Private/CSHouseLibrary.cpp:20` | `TActorRange` 默认带 SkipPendingKill（`EngineUtils.h:631`），`IsValid(House)` 多余 | 只留 `!IsTemplate()` | medium |
| H32 | redundant-logic | `Private/CSHouseDecor.cpp:407` | `Records.Num()` = Min(ByPalette, Palettes) ≤ `Work.Num()`，`!Work.IsValidIndex` 恒假 | 删掉前一半 | high |
| H33 | redundant-logic | `Private/CSHouseActor.cpp:3396` | 围合墙路径（`CSWall.cpp:772-781`）至少写 2 个采样，`GroundGaps` 不会为空 | 删除或改 `check` | medium |
| H34 | dead-code | `CSHouseRoof.h:359, 409, 451；CSHouseProfile.h:198；CSHouseTrim.h:85；CSHouseQuoin.h:83` | 六个 inline 函数生产代码 0 引用，只有单测在用；`CSHouseActor.cpp:1853` 的注释"全部调它"与事实不符 | 标"仅单测"或挪进测试 helper，改正注释 | medium |
| H35 | alias | `Public/CSHouseFeatureMarker.h:194` | `ResolveHostAndRegister` 只转发 `HandleDrag`；测试 12 处、`TinyGladeDemoRegression.py` 1 处在调 | 暂留，迁走调用方后再删 | medium |
| H36 | redundant-logic | `Private/CSHouseActor.cpp:1010 ↔ 2495` | `Tick` 与 `CanCommitSeams` 是同一个超时表达式 | 抽 `IsGizmoDragStale()` | high |
| H37 | duplicate | `Private/CSHouseActor.cpp:2183-2215` | 砖调色板配置与 `CSWallActor.cpp:529-552` 同构；`BaseSphere` 两行全工程 8 份 | 抽 `FPaletteBuffers::SetBaseBounds` | medium |

C++、Python、资产都没有引用、但按口径不建议直接删的 UFUNCTION：`GetPillarMesh`、`IsInGroundDrag`、`HasHost`、`QueryFeaturePlacement`（对插件与工程 Content 做过二进制名字检索）。

### 4.2 地面 / 楼梯 / 墙 / 样条块

范围：`CSTinyGlade*`、`CSGround*`、`CSStairs*`、`CSWall*`、`CSSplineBlock*`，42 个文件、约 1.7 万行。路径相对 `Source/ComputeShaderGenerator/`。38 条：null-unnecessary 16、dead-code 3、duplicate 11（1 条判定保留）、redundant-logic 8。

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| G2-01 | duplicate | `Private/CSStairsActor.cpp:421-498, 512-515` ↔ `Private/CSWallActor.cpp:127-208`（头 278-297 ↔ 314-335） | 楼梯与墙的"标脏 → 编辑器 ticker → 补票"和样条 / 根组件变换订阅逐行同构；`CSHouseActor.cpp:989-1003` 是第三份延迟补票；EndPlay / Destroyed 也两两相同。三者唯一公共基类是 `ACSTinyGlade`（楼梯不经过 `ACSWallBase`） | 延迟补票上提到 `ACSTinyGlade`（仍由 actor 自己 Tick 兑现）；样条观测抽成非 UObject 的 `FCSSplineChangeObserver` | high |
| G2-02 | duplicate | `Private/CSGroundActor.cpp:570-575, 603-618, 1006-1010, 1051-1060, 3223`（另有 371-384） | 同一条派生链重建序列手写 5 份，611-612 注释"只有一个位置要维护"不成立；补画循环两遍（564-568 ↔ 594-598） | 抽 `RebuildDerivedChains(bIncludeMask)` | high |
| G2-03 | null-unnecessary | `Private/CSWallBase.cpp:74-77` | 71-73 刚对三个组件调过 `EnsureInstancedComponent(this, …)`（无效就 `NewObject`），`NotReady` 分支不可达 | 删除或改 `check` | high（已复核） |
| G2-04 | null-unnecessary | `Private/CSWallBase.cpp:90` | `if (VineLeafSeasonMID)` 紧跟 `UMaterialInstanceDynamic::Create`，引擎实现末尾无条件 `NewObject` 并返回（`MaterialInstanceDynamic.cpp:120-122`） | 去掉 if | high（已复核） |
| G2-05 | null-unnecessary | `Private/CSWallBase.cpp:115` | `if (!MID) return;` 理由同上 | 删掉 | high（已复核） |
| G2-06 | redundant-logic | `Private/CSGroundActor.cpp:3233` | `PostEditUndo` 先调 `EnsureMirrorInitialized` 再调 `RebuildGroundMesh`，后者在 347-348 自己会调，第二次白扫一遍 MaxAbsHeight | 删掉 3233 | high |
| G2-07 | redundant-logic | `Private/CSWallActor.cpp:761-762` | (A∧B)∨¬B ≡ A∨¬B，761 的 `&& bBuffersReady` 多余；`EnsureVineRig` 已做过预留与交接，762 实际不可达。`CSHouseActor.cpp:3354` 同款 | 761 只留前两项，删 762 | high（等价已复核）/ medium |
| G2-08 | duplicate | `Private/CSGroundActor.cpp:2263-2289` ↔ `2727-2750`；`HashFloat` 在 474、1377、2259、2700 与 `CSGroundRockShell.cpp:328` 共 5 份 | 地被与灌木的地面侧输入哈希逐行相同（这些哈希不存盘） | 抽 `CSGround_HashFloat` 与 `HashGroundSurfaceInputs` | high |
| G2-09 | dead-code | `Public/CSWall.h:111-114` | `FCSWallSkins::TurnDegrees` 全插件零调用，非反射 | 删除 | high（已复核） |
| G2-10 | dead-code | `Public/CSStairs.h:149-153` | `CSStairs::FNode` 全插件零引用 | 删除 | high（已复核） |
| G2-11 | dead-code | `Public/CSGroundCollisionComponent.h:48` | `GetCollisionTriangleCount` 注释说给测试用，但 Source / Tests / Scripts 都没调 | 删除或在测试里用上 | high（已复核） |
| G2-12 | duplicate | `Private/CSGroundCover.cpp:444-540` ↔ `Private/CSGroundStairs.cpp:278-358` | 两份 `DebugReadInstancesSync` 的回读 lambda 与收尾几乎逐字相同 | 在 `CSShaperSteps` 抽共享回读 | medium |
| G2-13 | duplicate | `Private/CSGroundActor.cpp:3098-3165` ↔ `Private/CSHouseActor.cpp:4552-4616` | 两个烘焙自检流程同构（同 H25） | `ACSTinyGlade` 放 protected static helper | medium |
| G2-14 | duplicate | `Private/CSStairsActor.cpp:303-354` ↔ `18-47` | 踏步砖上传把 `CSStairsActor_UploadPieces` 重写了一遍 | 砖也走 UploadPieces（合并后复跑 UploadCount 单测） | medium |
| G2-15 | duplicate | `Private/CSStairs.cpp:274-297` ↔ `18-36` | `BuildRunBricks` 重写了 `CSStairs_ResampleRun`；`Min(Seg+1, …)` 也多余 | 改调 `CSStairs_ResampleRun` | medium |
| G2-16 | redundant-logic | `Private/CSStairsResize.cpp:101-106`；`Private/CSStairsWidthHandleActor.cpp:51, 58` | 一次把手归位要算 5 次完整 `BuildRuns`，一次拖动约 12 次；露台连接每次重建算两遍 | 框架只算一次再传下去 | medium |
| G2-17 | redundant-logic | `Private/CSGroundActor.cpp:2057-2060` | `EnsureSkirtDecorComponents` 返回 true 之后，`bBuffersReady` 与容量长度相等两条恒真 | 早退只留哈希比较 | medium |
| G2-18 | redundant-logic | `Private/CSGroundActor.cpp:1984, 2633, 2940` | 三处 `IsValidIndex` 三元恒真：组件数组已调成与缓冲数组同长 | 直接取下标 | high |
| G2-19 | null-unnecessary | `Private/CSTinyGlade.cpp:86` | `EnsureSlotMesh` 的 `Component ? : this` 兜底从未走到，4 个调用点都传非空组件 | 改 `check(Component)` | medium |
| G2-20 | null-unnecessary | `Private/CSTinyGlade.cpp:101` | `!Snapshot.IsValid()`：6 个来源都是 `MakeShared` 或判过非空的 `TakePending`（`!Component` 那一半保留） | 去掉快照那一半 | medium |
| G2-21 | null-unnecessary | `Private/CSWallBase.cpp:301` | `!Path.IsValid()`：只有两处调用，276 传 `MakeShared`，342 传判过的值 | 去掉 Path 那一半 | medium |
| G2-22 | null-unnecessary | `Private/CSWallActor.cpp:510` | 私有 static `WorldBoundsToComponent` 的 `!Component`：两个调用点之前组件都已保证有效 | 去掉 | medium |
| G2-23 | null-unnecessary | `Private/CSGroundShaperSteps.cpp:128` | `EnsureInstancedComponent` 的 `!Owner`：9 个调用点全传 `this` | 改 `check(Owner)` | medium |
| G2-24 | null-unnecessary | `Private/CSGroundActor.cpp:913` | `RegisterShaper` 的 `!Shaper`：唯一调用点传 `this` | 删除或 `check` | medium |
| G2-25 | null-unnecessary | `Private/CSGroundActor.cpp:2660` | `SetBuildingFootprint` 的 `!Key`：调用点传 `this` 或测试里 `NewObject` 出来的键 | 删除或 `check` | medium |
| G2-26 | null-unnecessary | `Private/CSStairsActor.cpp:509` | `HandleTerraceChanged` 的 `House &&`：静态委托，两处 Broadcast 都传 `this` | 去掉 | medium |
| G2-27 | null-unnecessary | `Private/CSWallActor.cpp:254` | `HandleAnyGroundRebuilt` 的 `IsValid(RebuiltGround)`：唯一广播点传 `this` | 直接比较 world | medium |
| G2-28 | null-unnecessary | `Private/CSGroundRockShell.cpp:571` | `BuildMesh` 的 `!ShellMesh`：唯一调用点紧跟 `EnsureSlotMesh` | 去掉或 `check` | medium |
| G2-29 | null-unnecessary | `Private/CSGroundRockShell.cpp:672` | `Displace` 的两个检查：唯一调用点之前都已保证 | 去掉或 `check` | medium |
| G2-30 | null-unnecessary | `Private/CSGroundRoadField.cpp:213` | `AddBlurPassesToTexture` 的 `!Target`：唯一调用传 `RegisterExternalTexture` 的结果（不返回空） | 去掉 | medium |
| G2-31 | null-unnecessary | `Private/CSGroundRoadField.cpp:234` | `UpdateMaskTexture` 的两个检查：唯一调用点 `CSGroundActor.cpp:494` 之前已保证 | 去掉或 `check` | medium |
| G2-32 | redundant-logic | `Public/CSGroundShaperField.h:159, 198, 204, 257, 265` | CPU 侧 `RadiusBonus` 参数从未被传值，只有 GPU 侧用 | 删 CPU 侧参数或注明原因 | medium |
| G2-33 | redundant-logic | `Private/CSGroundShaperSteps.cpp:14-79` | `GrowTo` 恒返回 true，`ReserveCapacity` 的返回值 7 个调用点全丢 | 改为 void | medium |
| G2-34 | redundant-logic | `Public/CSTinyGlade.h:227`；`Private/CSGroundActor.cpp:581`；`Private/CSStairsResize.cpp:80` | `MoveTemp` 之后紧跟的 `Reset()` 无效 | 删掉 | medium |
| G2-35 | duplicate | `Private/CSGroundShaperActor.cpp:94, 108, 166` | 同一个 MaskSdf 判定写 3 遍 | 抽 `HasMaskSdf()` | medium |
| G2-36 | duplicate | `CSGroundShaperSteps.cpp:106-122`、`CSGroundCover.cpp:428-442`、`CSGroundStairs.cpp:261-276` | `ReleaseOnRenderThread` 三份同构 | 模板化 | medium |
| G2-37 | duplicate | `Private/CSGroundActor.cpp:1261-1299` | 石阶 / 石子两对诊断函数只差 `bPebbles` | 抽私有共享实现（价值低） | medium |
| G2-38 | duplicate（保留） | `Private/CSGroundRoadField.cpp:61-72` ↔ `94-105` | 一个写 RWBuffer、一个写 RWTexture2D，RDG 参数结构必须分开声明 | 保留 | high |

其它观察：`CSGroundShaperActor.cpp:61/65/88/205` 判断 Ground 用裸 `!Ground`，其余地方用 `IsValid`——地面被删、GC 之前塑形物仍会调用这个已成垃圾的地面，建议统一成 `IsValid`；`CSGroundActor.h:1444-1446` 说"重复登记会重算"，实现（`.cpp:915`）是直接返回，注释与实现不符。

### 4.3 GPU 网格

范围：`CSMesh*`、`CSGpuMesh*`、`CSGpuInstanced*`、`CSGpuTriangle*`、`CSStaticMesh*`、`StaticMeshRenderDataPointSampler`、`CSPointArrow*`，37 个文件、约 1.7 万行。路径相对 `Source/ComputeShaderGenerator/`。43 条：null-unnecessary 14、dead-code 5（9 个符号）、duplicate 8、redundant-logic 13、alias 3；没有发现真实的空指针风险。

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| G3-01 | redundant-logic | `Private/CSGpuMeshSceneProxy.cpp:341-343, 362-377, 512-520`；`Public/CSGpuMeshSceneProxy.h:212, 217` | "代理自持 buffer"模式已无使用者：子类只有 `FCSMeshRenderSceneProxy` 与 `FCSGpuInstancedMeshSceneProxy`，都是 final，都在构造时以非空 resident 调 `SetExternalStreams`，`RegisterStreams` / `BuildGeometry` 都是空实现。owned 分支、`bAllocateBuffers` 分配块、`GatherDrawArgs` owned 尾段都到不了；`FDrawDesc::NumPrimitives` 无写入恒 0 | 收敛成"只借用常驻集"：删两个纯虚 hook、分配块、owned 尾段与恒零字段，改写类注释 | high（子类已复核） |
| G3-02 | dead-code | `Public/CSGpuMeshSceneProxy.h:130-131, 241, 244`；`.cpp:264-274, 283-287` | 代理版 `AddStandardTriangleStreams` / `GetStreamBuffer` / `GetVertexCapacity` / `GetIndexCapacity` 零调用（同名调用都在 `FCSMeshResident` / `UCSMesh` 上） | 删除（owned 模式残留） | high |
| G3-03 | dead-code | `Public/CSGpuMeshTypes.h:294-304`；`.cpp:165-182` | 导出函数 `SetStandardStreamAccessFinal` 只有声明和定义，其余提及都是"这里不调它"的注释 | 删除，注释改指 `RestoreStreamAccess` | high（已复核） |
| G3-04 | dead-code | `Public/CSMesh.h:188-196`；`Private/CSMesh.cpp:378-389` | `FCSMeshResident::AddStream` 零调用，函数体与 `CSMesh_AppendUniqueStreamDesc`（:143-155）逐字重复 | 删除并更新引用它的注释 | high（已复核） |
| G3-05 | duplicate | `Private/CSMesh.cpp:869-924` | `ReadCountersSync` 就是 `ReadUintBufferSync(…, 2, MeshCounters, …)` 的展开（约 45 行）；"消费回读"在 `CSMeshOps.cpp:1777-1796`、`StaticMeshRenderDataPointSampler.cpp:354-422` 还各有一份 | 改为转调；再抽 `ConsumeReadbackSync` | high |
| G3-06 | duplicate | `Private/StaticMeshRenderDataPointSampler.cpp:93-159` | `ResolveRequest` / `CreateIndexBufferSRV` ≈ `ComputeShaderMeshGenerator.cpp:779-845`；建索引 SRV 在 `CSMeshOps.cpp:662-671` 是第三份 | 抽模块内共享头 `ResolveResidentLOD` + `CreateIndexBufferSRV` | high |
| G3-07 | duplicate | `Private/CSGpuInstancedMeshComponent.cpp:52-59, 1142-1147` | `PackSnorm8888` ≡ `CSMeshOps.cpp:472` 的版本；解包在 `CSMesh.cpp:46` 与本文件各一份（后者不钳 -128） | 在 `CSGpuMeshStreams` 放一组 inline Pack/Unpack | high |
| G3-08 | redundant-logic | `Private/CSGpuMeshSceneProxy.cpp:109`（另有 608、798、865，`CSGpuInstancedMeshSceneProxy.cpp:907`，`CSMeshRenderComponent.cpp:49`） | `bValid := (IndexBuffer != nullptr)`（:467），叶子不写 DrawDesc，`IndexBuffer == nullptr` 多余 | 删 6 处 IndexBuffer 判断 | high |
| G3-09 | null-unnecessary | `Private/CSMesh.cpp:1170, 1230` | `Doomed` 是刚判过有效的 Resident `MoveTemp` 来、按值捕获的，lambda 里 `IsValid()` 恒真 | 去掉判断 | high |
| G3-10 | null-unnecessary | `Private/CSGpuMeshComponent.cpp:339` | 对 `NewObject` 结果判空（`UObjectGlobals.cpp:4964` 先 `check` 再构造，不返回空） | 删掉 | high |
| G3-11 | null-unnecessary | `Private/CSMeshBuild.cpp:194` | `AllocateGpuMesh` 无条件返回 `NewObject` 结果（`CSMeshOps.cpp:557`），头文件"分配失败返回 null"的说法不对 | 删掉并改头注释 | high |
| G3-12 | null-unnecessary | `Private/CSGpuInstancedMeshSceneProxy.cpp:540` | `InstancedVF` 刚 `MakeUnique`，基类在 :461 紧接着无保护解引用 | 删掉 | high |
| G3-13 | null-unnecessary | `Private/CSGpuInstancedMeshSceneProxy.cpp:521, 526, 621` | 成员 `Resident` 只在构造时赋值，唯一构造点前已判有效 | 只删 `IsValid()` 一项 | high |
| G3-14 | redundant-logic | `Private/CSGpuInstancedMeshSceneProxy.cpp:909` | `DrawMaterials.Num() != Sections.Num()` 恒假：构造时一一对应，之后不改 | 删掉 | high |
| G3-15 | null-unnecessary | `Private/CSGpuInstancedNaniteComponent.cpp:219`（`.h:105`） | `Stats` 只在构造时 `MakeShared`，从不重新赋值 | 成员改 `TSharedRef`，删两处判断 | high |
| G3-16 | redundant-logic | `Private/CSGpuMeshComponent.cpp:100`；`CSMeshBuild.cpp:20-23` | `ContainsNaN()` 已覆盖三个分量的 `IsFinite`（`Vector.h:2296`）；两个 helper 互为重复 | 只留 `!ContainsNaN()`，删 helper | high |
| G3-17 | redundant-logic | `Private/CSMeshRenderComponent.cpp:30, 54, 79` | `ResolveBatchMaterials` 不会返回空表或空项，空表回退与默认材质兜底到不了 | 删回退或改 `ensure` | medium |
| G3-18 | null-unnecessary | `Private/StaticMeshRenderDataPointSampler.cpp:114` | `!LODResource` 判的是数组元素地址，不可能为空（`ComputeShaderMeshGenerator.cpp:811` 同款） | 删掉或改引用 | high |
| G3-19 | redundant-logic | `Private/StaticMeshRenderDataPointSampler.cpp:108` | 下标刚钳到 `[0, Num-1]` 且 Num>0，`IsValidIndex` 恒真（:805 同款） | 删掉 | high |
| G3-20 | null-unnecessary | `Private/StaticMeshRenderDataPointSampler.cpp:143` | 局部函数唯一调用点传非空 `&elem`，下一行就解引用 | 删掉，参数改引用 | high |
| G3-21 | null-unnecessary | `Private/StaticMeshRenderDataPointSampler.cpp:358` | 两个 readback 都来自 236-237 的 `new`、按值捕获；这个分支万一走到还会泄漏两个对象 | 删掉 | high（已复核） |
| G3-22 | redundant-logic | `Private/StaticMeshRenderDataPointSampler.cpp:341-346` | lambda 无提前 return，:336 必置真且 :339 flush 已等它跑完，`!bRenderWorkQueued` 分支到不了 | 删分支和变量 | high（已复核） |
| G3-23 | null-unnecessary | `Private/StaticMeshRenderDataPointSampler.cpp:163` | 唯一调用点传 `SetNumZeroed(>0)` 后的 `GetData()`；`Count <= 0` 时循环本来也返回 0 | 删掉 | high |
| G3-24 | duplicate | `Private/StaticMeshRenderDataPointSampler.cpp:371-421` | `delete PointReadback; delete CountReadback;` 在 5 条退出路径各写一遍 | `ON_SCOPE_EXIT` 或 `TUniquePtr` | medium |
| G3-25 | alias | `Private/CSMeshOps.cpp:518` | `UCSMeshOps::InvalidateSections` 纯转发 `Context.InvalidateSections()`，9 个调用点 | 内联（也可为可发现性保留） | medium |
| G3-26 | alias | `Private/ComputeShaderMeshGenerator.cpp:3937-3980` | 三个 `Add*ToRDG` 纯转发，各只在 `ComputeShaderMeshBoolean.cpp:721/731/809` 用一次（同 A01） | 改直调后删除 | medium |
| G3-27 | duplicate | `Private/CSMeshOps.cpp:1175-1181` | `EnsureTexCoordSets` 又拼了一遍 `CSMesh_BuildStandardDescs` 的强制选项，两边漂移会让 `SetStreamLayoutSync` 因重复流拒掉整套布局 | 由 CSMesh 侧导出 `BuildStandardStreamDescs` | medium |
| G3-28 | dead-code | `Private/CSGpuInstancedMeshVertexFactory.h:100` | `GetInstanceCustomDataSRV` 零调用 | 删掉 | high（已复核） |
| G3-29 | redundant-logic | `Private/CSMeshVisibilityCull.cpp:304-305`（另有 `CSGpuInstancedMeshComponent.cpp:889, 1194, 1231, 1256`） | `ReadUintBufferSync` 成功时恰好返回 NumUints 个值且自己拒绝无效 buffer，调用方后置检查多余 | 删掉或在函数头写明契约 | medium |
| G3-30 | redundant-logic | `Private/CSGpuInstancedNaniteComponent.cpp:126, 175` | 唯一上游 `IssueGpuSceneWrite` 已保证槽数 > 0 且 Feed 有效 | 删掉两项 | medium |
| G3-31 | null-unnecessary | `Private/CSGpuInstancedMeshSceneProxy.cpp:441` | `if (P) delete P;` | 去掉 if | high |
| G3-32 | null-unnecessary | `Private/CSGpuInstancedMeshSceneProxy.cpp:851` | 处于 Waiting 状态时 `DiagnosticReadback` 一定非空 | 删掉或 `check` | medium |
| G3-33 | null-unnecessary | `Private/CSPointArrowMesh.cpp:90` | 模块内部函数，两个调用点都先用 `NewObject` 兜底 | 改 `check` / `ensure` | medium |
| G3-34 | null-unnecessary | `Private/CSGpuMeshComponent.cpp:333` | private 函数，3 个上游调用点都传非空 Outer（但 `BuildStaticMesh` 是公开 API） | 判断提到 `BuildStaticMesh` 入口 | medium |
| G3-35 | duplicate | `Private/CSGpuInstancedMeshComponent.cpp:1817-1825` ↔ `CSPointArrowMesh.cpp:136-144` 等 12 处 | 私有 `EditMeshSync` lambda 里对标准流的判空恒不成立（`IsAllocated` + 全掩码登记 + 标准 8 条流必在），`CSMeshVisibilityCull.cpp:377` 那条警告打不出来 | 提升 `CSMeshOps_HasVertexStreams` 为 `FCSMeshEditContext::HasStandardStreams()` + `ensure` | medium |
| G3-36 | duplicate | `Private/CSMesh.cpp:702-710` | `FCSMeshEditContext::Find` 重写了 `FindStreamIndex`（:608）；第三份在 `CSGpuMeshSceneProxy.cpp:276-281` | 改为转调 | medium |
| G3-37 | duplicate | `Private/CSMesh.cpp:173-180` | `CSMesh_RequiredBytesForDescs` 与 `GetRequiredBytes`（:638-646）只差输入容器 | 合并（价值低） | medium |
| G3-38 | redundant-logic | `Private/CSGpuInstancedMeshComponent.cpp:1045` | 与 `ResolveSlotMaterial` 第一行（:1037）是同一个 `InstanceMaterial` 判断 | 删掉 | high |
| G3-39 | redundant-logic | `Public/CSGpuInstancedMeshComponent.h:872` | `RebuildInstanceData(bRebuildGpuMesh = true)` 的 8 个调用点全用默认值 | 删参数 | high |
| G3-40 | redundant-logic | `Private/CSGpuMeshComponent.cpp:108` | `NameSuffix` 参数唯一调用点从未传值 | 删参数 | high |
| G3-41 | redundant-logic | `Private/CSMeshPool.cpp:86` | `MoveTemp` 后的 `ActiveMeshes.Reset()` 无效 | 删掉（价值低） | high |
| G3-42 | dead-code | `Public/CSMesh.h:459-460` | `FCSMeshRenderThreadEdit` 的 `Get()` / `operator*` 零调用，8 个使用点只用 `operator->` | 删掉（价值低） | high |
| G3-43 | alias | `Private/CSGpuMeshComponent.cpp:417-420` | 默认材质包装函数与直调引擎函数两种写法混用 | 统一一种（价值低） | medium |

### 4.4 生成器框架与核心

范围：`ComputeShaderMeshGenerator*`、`MeshGenerator*`、场景捕获、`CSBoxScene*`、显存预算、调试绘制、`ComputeShaderBasicFunction` 等，36 个文件、约 1.3 万行。路径相对 `Source/ComputeShaderGenerator/`（`Public/` 或 `Private/` 省略时见文件名）。约 80 条：真实缺陷 5、空指针风险 5、null-unnecessary 22、dead-code 16、redundant-logic 18、duplicate 11、alias 3。

**真实缺陷与空指针风险**（汇总见第二节）：

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| X01 | 缺陷（GC） | `Public/MeshGeneratorBrushCache.h:16-20` | `AMeshGeneratorBrushCache` 的 `VoxelMetaRT` / `TriangleVertexRT` / `TriangleNormalRT` 是普通 `TObjectPtr` 成员，没有 `UPROPERTY`，模块里也没有 `AddReferencedObjects`；由 `NewObject` 创建（`.cpp:1059-1061`），Outer 不保活，下一次 GC 就会被回收，之后 `IsValidCacheRT`（136-139）读已释放对象，808-810 还把它交给渲染线程。`InstanceBrushMesh`（:28）同类、风险较低 | 加 `UPROPERTY(Transient)` | high（已复核） |
| X02 | 缺陷（悬空引用） | `Private/ComputeShaderBasicFunction.cpp:354`（另有 258、293、301、310、398、462、512、573、660、786、1203） | RDG lambda 写 `[&PassParameters]`，按引用捕获局部指针变量；lambda 在 `GraphBuilder.Execute()` 时才跑，`BlurTexture_Internal` 在 375 行 Execute 之前就已返回，引用指向失效栈 | 改按值捕获 | high（已复核） |
| X03 | 缺陷（缓冲大小） | `Private/ComputeShaderBasicFunction.cpp:229, 234` | `CreateRWBuffer` 签名是 (NumElements, BytesPerElement)（`ComputeShaderGenerateHelper.h:243`），两处实参写反；`ResultBuffer` 只有 4·W·H 字节，按 float4 需要 16·W·H，3/4 的写入被丢弃；231 的 `BytesPerElementFloat4` 没用上 | 改正实参顺序并用上 float4 字节数 | high（已复核） |
| X04 | 缺陷（越界写） | `Private/ComputeShaderBasicFunction.cpp:165-171` | `DrawLinearColorsToRenderTarget16`（UFUNCTION）颜色数多于像素数时写 `Colors16` 越界 | 循环上界改 `TexturePixelCount` | high |
| X05 | 缺陷（越界读） | `Private/ComputeShaderBasicFunction.cpp:702, 720` | `SampleSpline` 采样失败时 `continue` 少 Add 一项，之后按 NumSpline 取 `ResampleTransform[i]` 越界 | 失败也 Add 空项，或按实际数量循环 | high |
| R01 | null-risk | `Private/ComputeShaderBasicFunction.cpp:478-479` | `ExtentMaskFast`（BlueprintCallable，`EUW_Tests` 在用）两个 RT 参数都没判空 | 开头补判空 | high |
| R02 | null-risk | `Private/ComputeShaderBasicFunction.cpp:596, 603` | `CalTreeWindTexture` 只在 593 查了两张 RT，`InDebugView` 直接 `ResizeTarget` | 593 的判断加上 `InDebugView` | high |
| R03 | null-risk | `Private/ComputeShaderBasicFunction.cpp:697, 719` | `SampleSpline` 的蓝图数组元素可能是 None，未判空 | 循环里先判空再 continue（并修 X05） | high |
| R04 | null-risk | `Private/ComputeShaderBasicFunction.cpp:829-846` | `GenerateMapCliff`（`UEW_CliffGenerate` 在用）：类为 None 或生成失败时 `SpawnActor` 返回 nullptr 被直接解引用；`GeneratorClass` 为空时 `Generators[0]` 越界 | 每次 Spawn 后判空，空数组直接返回 | high |
| R05 | null-risk | `Private/ComputeShaderBasicFunction.cpp:1215-1247` | `Test()`（WITH_EDITOR UFUNCTION）不判 `InArray` / `InDebugView`；编辑器非 PIE 下 `GetGameViewport` / `GetFirstPlayerController` 为空必崩；函数体只有一个空 pass | 删除这个测试函数 | high |

**null-unnecessary**：

| ID | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- |
| N01 | `Private/ComputeShaderMeshGenerator.cpp:3289` | 3285 刚 `NewObject<UTextureRenderTarget2D>`，判空走不到 | 删 3289-3292 | high |
| N02 | `Private/MeshGeneratorBrushCache.cpp:217` | 216 刚 `NewObject<UHierarchicalInstancedStaticMeshComponent>` | 删 217-220 | high |
| N03 | `Private/ComputeShaderMeshGenerator.cpp:3332, 3429` | `GetOrCreateGeneratedDataRenderTarget` 唯一的空出口就是 N01 那条走不到的分支，8 处调用方的判空链随之多余 | 随 N01 一起删 | high |
| N04 | `Private/ComputeShaderMeshGenerator.cpp:153` | 匿名命名空间函数，8 处调用传入的 RT 都已检查过、尺寸由 3297 `InitAutoFormat(≥1, ≥1)` 设定 | 删除或 `check` | high |
| N05 | `Private/ComputeShaderMeshGenerator.cpp:2958` | `OutGPUVoxels = &LastSurfaceVoxelGPUBuffers`（2703），成员地址 | 去掉 if | high |
| N06 | `Private/ComputeShaderMeshGenerator.cpp:811` | `&RenderData->LODResources[LODIndex]` 不可能为空（PointSampler 同款） | 去掉，改引用 | high |
| N07 | `Private/ComputeShaderMeshGenerator.cpp:3614` | 3613 为空时赋 `AllocateGpuMesh`，它总是返回 `NewObject` 结果 | 删掉 | high |
| N08 | `Private/ComputeShaderMeshGenerator.cpp:3714` | 同 N07 | 删掉 | high |
| N09 | `Private/ComputeShaderMeshGenerator.cpp:1035` | 唯一调用 1443，1440 已保证非空 | 删除或 `check` | high |
| N10 | `Private/ComputeShaderMeshGenerator.cpp:958` | 唯一调用 1474 在 Resolve 成功之后，LODResource 非空、三角数 > 0 | 只留 `MaterialSlots.Num()==0` | high |
| N11 | `Private/ComputeShaderMeshGenerator.cpp:833` | 两个调用点：1947 下一行无条件解引用；4300 之前 4298 已判 | 删 `!LODResource`，留 RHI 有效性检查 | high |
| N12 | `Private/ComputeShaderMeshGenerator.cpp:4298` | 唯一生产者只在 Resolve 成功后才 Add | 删或 `checkSlow` | medium |
| N13 | `Private/ComputeShaderMeshGenerator.cpp:96-97` | 两个调用点的 readback 都来自 `new`；这条提前返回真触发还会漏 delete | 删除 | high |
| N14 | `Private/ComputeShaderMeshGenerator.cpp:223` | 唯一调用 3640，LevelSet 由 `createLevelSet`（make_shared）创建且已先解引用 | 删除 | high |
| N15 | `Private/ComputeShaderMeshGenerator.cpp:3835` | 参数来自 `GetAttachedActors`，只回调非空 owner | 去掉 `Actor &&`（收益小） | high |
| N16 | `Private/MeshGeneratorBrushCache.cpp:128` | `InitCacheRT` 只在 1063-1065 被调，实参都是 `NewObject` 结果 | 删除 | high |
| N17 | `Private/MeshGeneratorBrushCache.cpp:770` | 唯一调用 336，302 的 `HasValidCacheResources` 已要求三张 RT 非空（真实风险是 X01） | 只留 `DirtyPageData.IsEmpty()` | medium |
| N18 | `Private/MeshGeneratorLandscapeCapture.cpp:105` | `TActorIterator` 默认跳过 `!IsValid` 的 actor（`EngineUtils.h:443/582`） | 直接 Add | high |
| N19 | `Private/ComputeShaderBasicFunction.cpp:939` | 911/914 保证样本与控制点 ≥ 2，buffer 一定建出 | 删 if（收益小） | medium |
| N20 | `Private/ComputeShaderBasicFunction.cpp:178` | 非 UFUNCTION，唯一调用 172 之前 163 已判 | 可删（公开 API，留着无害） | medium |
| N21 | `Private/MeshGeneratorLandscapeCapture.cpp:131, 303`；`Private/ComputeShaderMeshGenerator.cpp:4253, 4291` | 所有调用点传的都是 `RegisterExternalTexture` 的结果（不返回空） | 可删，或作为公开 API 防御保留 | medium |
| N22 | `Private/GDFSampleService.cpp:163` | `JobQueue` 私有，两个入口都拒绝了空 Build | 删除 | medium |

**dead-code**：

| ID | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- |
| D01 | `Private/ComputeShaderMeshGenerator.cpp:4088-4162`（h:602-610） | `RasterizeTriangleSoupToHeightmapRDG` 全工程 0 调用，连带 `FTriangleSoupToHeightmapCS`（465-483）；`MeshGeneratorLandscapeCapture.cpp:8-9` 说它"被复用"已过时 | 连同 shader 类与 usf 入口删除 | high |
| D02 | `Private/ComputeShaderBasicFunction.cpp:1288-1418`（h:189） | `UpdateTextureArray` 唯一引用 `ComputeShaderCliffGenerate.cpp:350` 已被注释掉（约 130 行，是引擎代码的拷贝） | 删除 | high |
| D03 | `Private/ComputeShaderBasicFunction.cpp:1172-1185, 55-112` | `DrawCopyTexture` 只在 1164 有一处注释掉的调用；`FDrawCopyTexturePS` 只有它在用 | 一并删除（含 usf 入口） | high |
| D04 | `Private/ComputeShaderBasicFunction.cpp:1187-1207` | `BuildTextureArray` 0 调用（`CliffGenerate:443`、`MeshFill:260` 各内联了一份） | 删除 | high |
| D05 | `Private/ComputeShaderBasicFunction.cpp:380-403` | `BlurTextureRDG` 0 调用，且与 `BlurTexture_Internal` 重复 | 删除 | high |
| D06 | `Public/ComputeShaderSceneCapture.h:97` | `CreateRenderTarget(float)` 无定义也无调用 | 删声明 | high |
| D07 | `Public/ComputeShaderSceneCapture.h:112-113` | `PreCenter` / `PreExtent` 非 UPROPERTY、0 引用 | 删除 | high |
| D08 | `Private/MeshGeneratorBrushCache.cpp:571-576` | 两参数重载 `BuildActiveCellsFromReferencePoints` 纯转发且 0 调用 | 删除 | high |
| D09 | `Public/ComputeShaderBasicFunction.h:21-24` | `FCSSPlinePointData` 0 引用 | 删除 | high |
| D10 | `Public/CSGpuSurfaceBudget.h:10` | `MaxBytes` 0 引用 | 删除 | high |
| D11 | `Public/ComputeShaderGenerateHelper.h:19-31, 42-43, 60-63` | `CREATE_UAVB/_16/_32`、`CREATE_TEXTURE_UAV_16`、`CREATE_TEXTURE_UAV_32_OUTP` 头文件之外 0 使用 | 删除 | high |
| D12 | `Private/PCGPluginDebug.h` | 与 `Public/` 副本逐字节相同，唯一 includer `GeometryEditorActor.cpp:11`（删后回落到 Public 副本，零行为变化）；`PCG_DEBUG_ONLY` / `PCG_DEBUG_LOG` / `PCG_DEBUG_TIME_SCOPE` 无人用，`#ifndef PCGPLUGINS_DEBUG` 兜底永远不生效 | 删 Private 副本与无用宏 | high |
| D13 | `Private/ComputeShaderMeshGenerator.cpp:3140-3142` | 参数 `DebugName` 被 `(void)` 掉，3 个调用点都不传 | 删参数 | high |
| D14 | `Private/ComputeShaderMeshGenerator.cpp:2207-2237` | `NaniteTriangles` 从未被填充，2214 恒真、2237 恒为 nullptr | 删除，2249 直接传 nullptr | high |
| D15 | `Private/ComputeShaderBasicFunction.cpp:231, 697, 726, 1220-1221, 1260` | 赋值后从不读取的变量与无副作用语句（231 应按 X03 修） | 清理 | high |
| D16 | 反射成员（仅供复核） | C++ 与资产都无引用：`SyncRenderThread`、`CalTreeWindTexture`、`CaptureLandscapeTrianglesGPU`、USTRUCT `FCSMeshData`、`FCSDebug*VoxelDirectionOptions` 的 `Thickness` / `bDrawPoints` / `PointColor` / `PointSize`（只写不读）。必须保留：`SpawnDebugSurfaceTrianglesDynamicMeshActor`（`BP_VineSource`）、`CreateLandscapeMesh`（`EUW_Tests`）、`EVisVineGPUDebugStage` | 先用 Reference Viewer 复核再删 | medium |

**redundant-logic**：

| ID | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- |
| L01 | `Private/ComputeShaderMeshGenerator.cpp:2982-2990` | 两个标志在 lambda 里无条件置真，之后又 flush，分支不可达 | 删整段与两个标志 | high |
| L02 | `Private/ComputeShaderMeshGenerator.cpp:2267`；`MeshGeneratorLandscapeCapture.cpp:562-588` | 同 L01：flush 后 `bRenderWorkQueued` 恒真，else 走不到 | 删标志 | high |
| L03 | `Private/ComputeShaderMeshGenerator.cpp:2464-2466, 2814-2816`；`MeshGeneratorBrushCache.cpp:901-909` | SRV 与 buffer 同时创建，`SRV ? SRV : CreateSRV(...)` 的回退永不执行 | 直接用 SRV | high |
| L04 | `Private/ComputeShaderMeshGenerator.cpp:805` | Clamp 之后再查 `IsValidIndex` | 删除 | high |
| L05 | `Private/ComputeShaderMeshGenerator.cpp:1859-1875 ↔ 1923-1940` | 分支内 `IsEmpty` 不可达，同一份参考点数据上传两次 | 提前 1923-1940，复用 SRV | high |
| L06 | `Private/ComputeShaderMeshGenerator.cpp:1822-1823` | 1820 已清 0，紧接着又清 1，多一个 pass | lambda 加清除值参数 | high |
| L07 | `Private/ComputeShaderMeshGenerator.cpp:1966-1969` | 所有 request 都经过 Init 且三角数 > 0，else 不可达 | 删 else | medium |
| L08 | `Private/ComputeShaderMeshGenerator.cpp:3134-3137` | `VoxelCount` 三元与重设 Size / Origin 都是重复赋值 | 只留 VoxelCount | high |
| L09 | `Private/MeshGeneratorInternal.h:96-102` | `!ContainsNaN() && IsFinite(X)…` 重复（`Vector.h:2296-2299`） | 只留 `!ContainsNaN()` | high |
| L10 | `Public/ComputeShaderGeneral.h:71-75, 138-142, 513` | 无用局部变量；513 重设了 494 已设的 `THREADGROUPSIZE_X` | 用宏，删 513 | high |
| L11 | `Private/ComputeShaderMeshGenerator.cpp:2149` | `HasAnyTriangles` 第一行就判了 `!Impl` | 只留后一项 | high |
| L12 | `Private/MeshGeneratorBrushCache.cpp:840` | 770 已返回过，817 是 MoveTemp 进来的，不可能变空 | 删这一项 | high |
| L13 | `Private/MeshGeneratorLandscapeCapture.cpp:266`；`ComputeShaderShallowWater.cpp:1180`；`PCGEditorProcess/.../ComputeShaderLandscapeRoad.cpp:346` | 两个函数不读成员却不是 static，迫使渲染线程 lambda 捕获 actor 指针 | 改 static | medium |
| L14 | `Private/ComputeShaderMeshGenerator.cpp:3792-3793` | `GetName()` 永不为空 | 直接返回（收益小） | medium |
| L15 | `Private/ComputeShaderMeshGenerator.cpp:704-707` | 唯一调用前 1500 已提前返回 | 删除（收益小） | medium |
| L16 | `Private/GDFSampleService.cpp:225` | 唯一入口 80 已拒绝空请求 | 删除（收益小） | medium |
| L17 | `Private/MeshGeneratorBrushCache.cpp:1015-1023` | 两个分支都执行 `RequestActiveCells.Empty()` | 提到 if 外 | high |
| L18 | `Private/ComputeShaderMeshGenerator.cpp:3551-3556` | 数据在 3066-3078 已清洗过 | 可删（收益小） | medium |

**duplicate / alias**：

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| U01 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:2378-2573 ↔ 2705-2978` | 新旧两条表面体素管线约 200 行 buffer 分配、分批、pass 参数逐字重复（2338-2341 注释自己承认） | 抽 helper 或旧路径改走新接口；改前实测 TDR 与峰值显存 | medium |
| U02 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:2185, 2595, 2649` | 三份游戏线程收集前置，等价于 `CollectBoxSceneTriangles` + `MakeBoxSceneCollectOptions` | 改用现成函数 | medium |
| U03 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:4106-4161 ↔ 4184-4280` | uint 高度图目标与转换 pass 两份 | 删 D01 即消除 | high |
| U04 | duplicate | `Private/MeshGeneratorLandscapeCapture.cpp:226-291 ↔ 340-412` | 多地形捕获循环两份 | 抽 `ForEachLandscapeHeightmap` | medium |
| U05 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:783-819 ↔ StaticMeshRenderDataPointSampler.cpp:95-127` | LOD 解析前置逐行重复（同 G3-06） | `MeshGeneratorInternal.h` 提供 `ResolveRenderLOD` | medium |
| U06 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:1654-1742 ↔ CSGpuMemoryBudget.cpp:240-309` | 显存预检估算与真实提取各写一份场景枚举，2136 要求两者口径一致只能靠手工同步 | 抽共享枚举 | medium |
| U07 | duplicate | `Public/ComputeShaderGeneral.h` 6 处 THREADGROUPSIZE、5 个 `Create*Permutation` | SetDefine 三连与排列工厂重复（参数结构体属于不同 shader，保持独立） | 一个宏 + 一个模板工厂 | medium |
| U08 | duplicate | `Private/ComputeShaderBasicFunction.cpp:334-359 ↔ 380-403` | Blur 的 pass 设置两份 | 删 D05 即消除 | high |
| U09 | duplicate | `Public/ComputeShaderGenerateHelper.h:213-241, 150-172` | `CreateClearedTypedBuffer` 4 个重载、`ConvertToUVATexture*` 3 个版本函数体两两相同 | 模板化 / 委托 | medium |
| U10 | duplicate | `Private/ComputeShaderMeshGenerator.cpp:3294-3298, 147-150 ↔ MeshGeneratorBrushCache.cpp:129-133, 121-124` | RT 初始化 / 释放两个 TU 各一份 | 收进 `MeshGeneratorInternal.h` | medium |
| U11 | duplicate | `Private/GDFSampleService.cpp:250-253 ↔ 374-377` | GDF 参数与采样器设置逐字相同 | 抽小 helper | medium |
| A01 | alias | `Private/ComputeShaderMeshGenerator.cpp:3937-3980` | 三个 static 纯转发到 `CSGpuTriangleUtilities`，各一处调用（同 G3-26；头注释 928-934 说是刻意设计） | 直调，或作为设计选择保留 | medium |
| A02 | alias | `Private/ComputeShaderMeshGenerator.cpp:3693-3696` | 已废弃的 UFUNCTION，`BP_VineSource` 仍在引用 | 等 BP 迁移后再删 | high |
| A03 | alias | `Private/CSFixedScreenBillboardComponent.cpp:65` | `GetAllocatedSize` 遮蔽基类且行为相同 | 删除（收益小） | medium |

### 4.5 功能模块

范围：藤蔓 / 空间殖民、Nanite 截面 HLOD、点笔刷、MeshFill / 悬崖 / 范围生成、Mesh Boolean、浅水，36 个文件、约 1.9 万行。路径相对 `Source/ComputeShaderGenerator/`。45 条：null-unnecessary 11、null-risk 2、dead-code 8、duplicate 6、redundant-logic 17、alias 1。

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| R1 | redundant-logic（疑似缺陷） | `Private/ComputeShaderCliffGenerate.cpp:254-276` | `GenerateTargetHeightCal` 每次跑 2×GroupCount.X 趟 `GenerateTargetHeight`：每趟读不变的 `RDG_TargetHeight`、写 `TmpRDG_TargetHeight`，lambda 只交换 shader 里只有声明的 `RW_TempA/B`；:276 把初值拷回；随后 :284-299 的 Blur 以 `RDG_TargetHeight` 为输入整幅覆盖输出——循环结果从未被读 | 二选一：删掉循环与乒乓（输出不变）；或按原意做真正的乒乓。先在编辑器比对两种输出 | high |
| D1 | dead-code | `Private/GeometryEditorActor.cpp:2739-2852`，另有 1581-1766 的 5 个 helper | `VineLeaf_BuildVineBuildInput` 两个调用点传的 `VoxelData` 都是空对象（:3848、:5008），CPU 体素重打包段、`BuildVineVoxelHashSlots`、`BuildVineTargetBucketBuffers`、`GetVineTargetBucketCell`、`Hash/SameVineVoxelCell` 都不可达；3 个计时出参恒为 nullptr | 删掉 `VoxelData` 参数、CPU 段、5 个 helper 与计时出参（约 250 行），`EnsureVineTargetBucketDummyBuffers` 保留 | high |
| D2 | dead-code | `Private/GeometryEditorActor.cpp:2363-2374、1959-1968、1859-1866` | `FVineMeshPassOutputs` 只在 :3051、:3157 构造，都设 `bBaseStreams = true` 并给了 IndirectArgs / MeshCounters：legacy 输出分支、scratch 兜底、3 个 `Out*UAV` 字段都不可达，`FVVVoxelCS` 的 `FBaseStreams=false` 排列白编译；:1863 的注释与事实相反 | 删开关与 legacy 路径（或在 `ShouldCompilePermutation` 排除 false 排列），修正注释 | high |
| R3 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:269-281`；`ComputeShaderCliffGenerate.cpp:452-464` | 3 个 FindPixel buffer 和 `RW_Deduplication`、`RW_FilterResulteMult` 在 usf 里只有声明；3 个 UAV 都建在同一个 `Tmp_CountBuffer` 上，清零互相覆盖 | 删掉这些 buffer、纹理、清零、绑定与 usf 声明 | high |
| R2 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:464-469`；`ComputeShaderCliffGenerate.cpp:85-90` | `ReadLinearColorPixels` 读出的 `LinearSamples` 没人用，紧接着又 `ReadRenderTargetRaw` 读同一张 RT——每次生成多一次阻塞 GPU 回读 | 删掉 | high |
| U1 | duplicate（含缺陷） | `Private/ComputeShaderCliffGenerate.cpp:35-161、321-607` ↔ `ComputeShaderMeshFill.cpp:143-416、423-543` | 约 200 行几乎逐行相同。两份共有同一个越界：循环到 `MeshDataAssets.Num()`，每轮却读 `GenerateDatas[i]`（只有 NumIteration 个元素，MeshFill :243-257、Cliff :426-440）；而且这里该用的切片号是 `i`，不是 `GenerateDatas[i].SelectIndex` | 先修越界；再抽自由函数到 `Private/CSMeshFillShared.h`（不要插中间 UCLASS，蓝图资产依赖现有层级） | high（越界已复核） |
| D3 | dead-code | `Private/GeometryAsync.h` | CSG 里没人包含它，唯一包含点解析到 GSEE 那份（同 g6 的 D1） | 删掉 CSG 这份 | high |
| R4 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:103-126` | 重叠查询只贡献"带 Tag 的"子集，紧接着遍历全世界的 Tag 又全加一遍（还有重复）；`OverlapOutActorsWithoutTag` 没人用 | 删掉重叠查询；若本意是"盒内且带 Tag"则删第二段，需作者确认 | high |
| U3 | duplicate | `Private/GeometryEditorActor.cpp:114-150、443-465、591-610、674-693`，绑定 2097-2392 | 4 个 VVVoxel shader 声明并逐行绑定同一组 19 个参数；本文件已对 TriangleSurface / GDF 用 `SHADER_PARAMETER_STRUCT_INCLUDE` | 新建 `FVineVoxelLookupParameters` 并 STRUCT_INCLUDE，约省 140 行 | medium |
| U2 | duplicate | `Private/GeometryEditorActor.cpp:1445-1472 ↔ 1826-1852` 等 5 处 | 约 30 个标量在两个 bundle 声明、58 行逐字段拷贝、45 行局部别名、20 多个位置参数里重复 | 抽 POD `FVineMeshScalars`，约省 120 行 | medium |
| K2 | null-risk | `Private/ComputeShaderMeshFill.cpp:245-246、480`；`ComputeShaderCliffGenerate.cpp:109、428-429` | `MeshDataAssets` 是 EditAnywhere 数组，资产字段默认 nullptr，`IsParameterValidMult` 只查长度——空元素或没填高度图就在渲染线程崩 | 逐元素校验 | medium |
| K1 | null-risk | `Private/ComputeShaderCliffGenerate.cpp:178-185` | `GenerateTargetHeightCal` 是 BlueprintCallable，不调 `CheckTexture`，三张 RT 只在那里懒创建也不在校验里 | 开头先调 `CheckTexture()` | medium |
| R6 | redundant-logic（疑似缺陷） | `Private/GeometryEditorActor.cpp:4100` | `TubeVineSource->SetHiddenInGame(false)` 下一行经 :3541 → :3267 被改回 true | 本意隐藏就删这行；本意显示就是 bug，需调换顺序 | high |
| U4 | duplicate | `Private/ComputeShaderMeshBoolean.cpp:1124-1192 ↔ 2095-2144` 等 | Snapshot 与 GpuMesh 两个入口的校验、成本模型、收集选项、溢出判断、capture 填充约 70 行相同 | 抽 3 个 helper | medium |
| D8 | dead-code | `Private/GeometryEditorActor.cpp:2716-2722、2045、2060-2062` | 用 pooled GPUVoxels 构建这条路总被哑缓冲分支或 FusedVoxels 抢先，走不到 | 不打算恢复就删参数、成员与分支 | medium |
| U5 | duplicate | `Private/GeometryEditorActor.cpp:3220-3240 ↔ PCGEditorProcess/Private/ComputeShaderLandscapeRoad.cpp:22-41` | 两个"能否生成"判断逐行相同（同 g6 的 P7） | CSG Public 提供共享函数 | high |
| D6 | dead-code | `Private/ComputeShaderShallowWaterVoxel.cpp:20-23` 及整个类 | `ACSShallowWaterVoxelCapture` 是空壳：`BuildTerrainVoxelGrid` 没人调，buffer 从未分配，类在代码与资产里都没被引用 | 删函数与字段；无计划可删整个类（先确认外部关卡没放置） | high |
| D5 | dead-code | `Public/ComputeShaderMeshFill.h:232-241` | `FCSGenerateParameter::IsValid` / `IsValidMult` 没人调且函数体一字不差，USTRUCT 在代码和资产里零引用 | 至少删两个函数 | high |
| D4 | dead-code | `Private/GeometryEditorActor.cpp:1493、3272` | `NormalizeVineAxisOrFallback`、`RebuildVineDisplayInstances` 只有定义处 | 删除连同悬空注释 | high |
| R5 | redundant-logic | `Private/GeometryEditorActor.cpp:3242-3257、3343-3344` | `ApplyVineReferenceComponentsHiddenInGame` 唯一调用在构造函数，而构造函数 :3322、:3330 已设过；里面的判空不可能成立 | 删函数与两行调用 | high |
| R13 | redundant-logic（性能） | `Private/GPUSkeletalTreeComponent.cpp:158` | 每帧对每根骨骼做一次 `Name.ToString().StartsWith`，结果只取决于骨骼名 | 在 `RebuildBoneHierarchy` 时缓存成 `bBranch` | high |
| N2 | null-unnecessary | `Private/ComputeShaderMeshBoolean.cpp:2413-2415` | :2375 的 `Capture.IsValid()` 已查过同样 9 个 buffer | 删掉 | high |
| N4 | null-unnecessary | `Private/MeshBooleanRepair.cpp:383` | 输入都来自 `RegisterExternalBuffer`，不返回 null | 只留计数判断 | high |
| N1 | null-unnecessary | `Private/ComputeShaderMeshBoolean.cpp:903` | 构造函数里 `CreateDefaultSubobject` 后马上 `if (BoundsCube)`；类无 FObjectInitializer 构造、无 C++ 子类 | 平铺展开 | high（已复核） |
| N3 | null-unnecessary | `Private/ComputeShaderMeshBoolean.cpp:1324、1410` | 回读对象的分配与字节数 > 0 取决于同一个 WeldDistance 条件 | 删掉 | high |
| N5 | null-unnecessary | `Private/CSVineScatter.cpp:42、52、214、304、382` | `SurfaceActor` 恒为 `X ? X : this`；helper 调用点都已判空或已解引用；`GetComponents` 跳过空组件 | 删掉，:382 只留 `!World` | high |
| N6 | null-unnecessary | `Private/GeometryEditorActor.cpp:2958、3128` | `!Target`、`!bValid`、`!bUseGPULines` 在每个调用点之前都已保证 | 删掉；保留 World、IsEditInFlight 与 ensure | high |
| R7 | redundant-logic | `Private/GeometryEditorActor.cpp:3827-3830` | `CurveControl` 懒创建与 :4913 重复，同一条调用链 | 删 3827-3830 | high |
| N10 | null-unnecessary | `Private/GeometryEditorActor.cpp:1502` | 唯一调用点前一行已 `NewObject` | 去掉 `!CurveControl` | high |
| N7 | null-unnecessary | `Private/GeometryEditorActor.cpp:3293、1519、1566` | 调用点传 `this`，`InFoliageType` 已判空 | 只留 `!World` | high |
| N8 | null-unnecessary | `Private/ComputeShaderShallowWater.cpp:357、576、1069` | lambda 实参已判非空；`!World` 之前 `CheckAndCreateTexture` 已检查 `GetWorld()` | 删掉 | high |
| R11 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:145-146`；`ComputeShaderCliffGenerate.cpp:323` | 唯一调用者 `Generate` 已做过同样校验 | 删掉，保留 `GenerateDatas.Num()==0` | high |
| R10 | redundant-logic | `Private/CSRangeGeneratorActor.cpp:114-119` 等 | `Empty()` 之后的分支不可达；死局部变量、重复 `SetupAttachment`、只转发 Super 的 Tick | 删掉（tick 开关保留） | high |
| U6 | duplicate | `Private/ComputeShaderShallowWater.cpp:335-353 ↔ 1050-1062` 等 | 浅水初始化里 4 段小代码各重复 2 到 4 份 | 抽 `EnsureSceneDepthRT` 等 helper | medium |
| R9 | redundant-logic | `Private/CSNaniteCutHLODActor.cpp:94-98、112` | 已按排除标签剔除一遍，`MakeSources` 再判恒假（WP builder 需要那次判断，那里别动） | Gather 不做 Exclude，或传 NAME_None | high |
| R12 | redundant-logic（性能） | `Private/GeometryEditorActor.cpp:4957-4966` | 每个源都重建一遍全部 target 的查找数组再丢掉 | target 数组只算一次 | high |
| D7 | dead-code | `Public/GeometryEditorActor.h:33、39、78、82、84、86、266` | 6 个 UPROPERTY 在 C++ / shader / 脚本里零读取；`InstanceBound` 只写不读 | `InstanceBound` 直接删；其余先确认蓝图不读 | high |
| R8 | redundant-logic | `Private/ComputeShaderMeshBoolean.cpp:1195-1209` 等 | 两个 constexpr 标志恒为 true，相关分支全是常量分支 | 删掉 | high |
| N9 | null-unnecessary | `Private/CSNaniteCutHLODActor.cpp:86、90` | `TActorRange` 与 `GetComponents` 不产出 null | 去掉 | high |
| N11 | null-unnecessary | `Private/CSNaniteCutHLODActor.cpp:169-170、250、252-253、289-291` | 依赖 `MakeSourcesFromComponents` 的一一对应与 Mesh 已判空 | 去掉或改 `check` | medium |
| R14 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:69、75-79` | `Super::OnConstruction` 刚做过同样的事 | 删掉 | high |
| R17 | redundant-logic | `Private/ComputeShaderMeshFill.cpp:149、178-179` 等 | 赋值后从未读取的变量 | 随 U1 一起删 | high |
| R15 | redundant-logic | `Private/GeometryEditorActor.cpp:30-34、87` | include 与 :13-17 重复；`GV_ENABLE_PERF_LOGS` 宏没人用 | 删掉 | high |
| R16 | redundant-logic | `Private/ComputeShaderCliffGenerate.cpp:24-34` | 构造函数与 `OnConstruction` 都只转发 Super | 删声明与定义 | high |
| A1 | alias | `Private/CSNaniteCut.cpp:944-947` | `IsExcludedByTag` 只转发 `ComponentOrOwnerHasTag` | 保留（与 `IsPickedByTag` 成对，是 public API） | medium |

按口径未报：`VineGpuMesh` 判空与 `RebindVineGpuMeshIfMissing`（资产陈旧兜底）；`GeometryEditorActor.cpp:4100` 解引用 `TubeVineSource`、`CSNaniteCutHLODActor.cpp:152` 解引用 `HLODMeshComponent` 都没守卫，与同文件写法不一致，但目前没有已知资产会把它们存成 null。

### 4.6 编辑器模块

范围：`PCGEditorProcess/`、`GeometryScriptExtraEditor/`、`EditorShortcuts/`，74 个文件、约 1.5 万行。路径相对 `Source/`；`GSEE` = `GeometryScriptExtraEditor`，`PEP` = `PCGEditorProcess`，`ES` = `EditorShortcuts`。95 条：null-risk 15、null-unnecessary 24、dead-code 14、duplicate 18、redundant-logic 21、alias 3。

**空指针风险与逻辑缺陷**（汇总见第二节）：

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| K1 | null-risk | `GSEE/Private/GeometryGeneral.cpp:1168, 1175` | `UVAttribTransf` 守卫写 `NumUVLayers() < UVNum`，差一：层数恰好等于 UVNum 时放行，`GetUVLayer(UVNum)` 返回空（`DynamicMeshAttributeSet.h:178-185`），1180 / 1191 解引用；默认 UVNum=1 + 单层 UV 网格就会崩 | 两处改 `<=`，overlay 判空 | high（已复核） |
| K2 | null-risk | `GSEE/Private/GeometryGeneral.cpp:1295, 1357` | `FillUVData` 写死 `GetUVLayer(2)`，UVLayerNum<3 时为空；没有颜色层时 `PrimaryColors()` 为空 | 层号用参数并判空；需要颜色时先 `EnablePrimaryColors` | high |
| K3 | null-risk | `GSEE/Private/GeometryGeneral.cpp:1134` | `ColorAttribTransf` 不判 `PrimaryColors()`，`UDynamicMesh` 默认不开颜色层，普通网格一进来就崩 | 两个 overlay 判空 | high |
| K4 | null-risk | `GSEE/Private/GeometryMathUtils.cpp:124, 131, 168, 175, 211, 227` | `ResampleByCount` / `ByLength` / `ConvertPolyPathToTransforms` / `CurveU` 直接 `*PolyPath.Path`，默认构造的 `Path` 是空 `TSharedPtr`；同文件 `SmoothLine`（:109）判了 | 开头补 `if (!PolyPath.Path.IsValid()) return {};` | high |
| K5 | null-risk | `GSEE/Private/GeometryGenerate.cpp:416, 436, 467` | `VDBMeshFromActors` 遇到没设网格的 SMC / ISM 或数组里的 None actor 都会解引用空；同文件另两个函数都判了 | 补判空（或走 P5 的统一收集） | high |
| K6 | null-risk | `PEP/Private/CSAssetProcess.cpp:56` | `CaptureMeshHeight` 的出参 `OutRenderTarget2D` 从未赋值，蓝图拿到永远是 None，接 `CalculateMeshHeight` 会在 :107 提前返回 | 补 `OutRenderTarget2D = NewRenderTarget2D;` | high（已复核） |
| K7 | null-risk | `PEP/Private/ComputeShaderLandscape.cpp:973-977` | `SimRiver`（UFUNCTION）不判 `SpawnActor` 结果；`ActorClass` 为 None 时返回空，传非 CSSW 子类时 `CastChecked` 直接致命 | 先判类型再判结果 | high |
| K8 | null-risk | `GSEE/Private/GeometryGeneral.cpp:1419` | `AddCustomAttribute` 永远 `return nullptr`，蓝图串到不判空的节点就崩；`LandscapeExtra.cpp:118`、`GeometryGeneral.cpp:194` 失败时也返回空 | 改为返回 TargetMesh | high |
| K9 | null-risk | `GSEE/Private/GeometryGeneral.cpp:54` 等约 15 处 | 一批 BlueprintCallable 不判 `TargetMesh`（54/93/125/191/203/236/876/1129-1130/1165-1166/1283/1376，`GeometryGenerate.cpp:1201`，`LandscapeExtra.cpp:162`，`GeometryVisualization.cpp:21`）；同模块 `VDBExtra.cpp` 判了 | 统一补判空 | medium |
| K10 | null-risk | `PEP/Private/ComputeShaderLandscape.cpp:171, 509, 833, 873, 883, 977` | 序列化的别名 UPROPERTY `Box`（指向 `GeneratorBounds`）5 处判空、6 处不判，与 `BP_VineSource` 那次同类风险 | 改 `GetBox()` 访问器或标 Transient | medium |
| K11 | null-risk | `PEP/Private/CSShallowWaterProcess.cpp:574` | `ResultMeshComponent` 未判空，同文件 :75 / :508 判了 | 补判空 | medium |
| K12 | null-risk | `PEP/Private/CSShallowWaterProcess.cpp:439` | `GetMeshDescription(0)` 没有 LOD0 源模型时可能返回空（`StaticMesh.cpp:6661-6663`） | 判空并打日志 | medium |
| K13 | null-risk | `PEP/Private/CSAssetProcess.cpp:254, 265, 123` | `CreateAsset` 被拒时返回空，:254 / :265 直接解引用；:123 的 RT 资源未判空就进渲染线程 | 判空 | medium |
| K14 | null-risk | `PEP/Private/CSLandscapeEditLayerBase.cpp:202` | 没判 `EditLayer`，同文件其它三处判了 | 统一成 `Layer && Layer->EditLayer` | medium |
| K15 | null-risk | `PEP/Private/CSAssetProcess.cpp:440, 447` | BlueprintCallable 参数没判空，`InDebugView` 为空时在 :330 崩 | 补参数判空 | medium |
| L2 | 缺陷 | `GSEE/Private/GeometryGeneral.cpp:762, 765` | `while (complete==0 \|\| attempt<maxAttempts)` 应为 `&&`（完成后仍空转）；空网格时 :765 先执行 `% 0`，`<1` 保护来不及 | 改 `&&`，对 0 提前返回 | high |
| L3 | 缺陷 | `GSEE/Public/GeometryAsync.h:19, 22` | `NumInput / NumThreads` 写在 `NumThreads==0` 守卫之前，输入少于每线程处理量时整数除零 | 守卫移到除法前 | high（已复核） |
| P6 | 缺陷（疑似） | `GSEE/Private/GeometryMathUtils.cpp:134-206` | `ResampleByCount` 与 `ByLength` 采样循环相同；`ByLength` 用 `Interval = ArcLength / N`，疑似算错；守卫 `<1` 却读 [1]，NumIterations<2 时写 [-1] | 抽 `ResampleUniform` 并修正 | high |

**null-unnecessary**：

| ID | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- |
| N1 | `ES/Private/EditorShortcutInvoke.cpp:129` | `FindCallableFunction` 的 3 个调用点（`Subsystem.cpp:126/209/257`）都已判非空 | 删或 `check` | high |
| N2 | `ES/Private/EditorShortcutInvoke.cpp:102` | `IsBindable` 的调用点传的都非空 | 删掉 | high |
| N3 | `ES/Private/EditorShortcutInvoke.cpp:152` | `CallFunction` 两个调用点都已判过（`GetDefaultObject` 不返回空） | 删掉 | high |
| N4 | `ES/Private/EditorShortcutSubsystem.cpp:304, 297` | 私有 `GetOrCreateInstance` 唯一调用点前已判；结果来自 `IsValid` 筛过的缓存或 `NewObject` | 两处都删 | high |
| N5 | `ES/Private/EditorShortcutSubsystem.cpp:207`；`Invoke.cpp:185` | 选中集来自 `USelection::GetSelectedObjects`，已滤掉空 | :207 改 `!IsValid(Actor)`（防前一次调用销毁 actor），删 :185 | high |
| N6 | `ES/Private/EditorShortcutInputProcessor.cpp:72` 等 8 处 | `GetDefault<>` 没有时会当场创建，不会返回空 | 删判空与兜底值 | medium |
| N7 | `ES/Private/EditorShortcutTagLibrary.cpp:38` | 唯一调用点 :154 之前 :141 已判 | 去掉 `TargetActor ?` | high |
| N8 | `ES/Private/EditorShortcutSetupLibrary.cpp:78` | `CreateNewGraph` 内部 `NewObject` 并已解引用 | 删掉 | high |
| N9 | `GSEE/Private/FoliageConverter.cpp:132` 等 6 处 | `GetAllFoliageTypesForSource` 只收非空项 | 只留 `Num()==0` | high |
| N10 | `GSEE/Private/GeometryGenerate.cpp:587, 1297` | `GetComponents` 只返回非空组件 | 去掉组件判空 | high |
| N11 | `GSEE/Private/GeometryGenerate.cpp:595` | 与 :592 是同一个对象，:587 已判 | 删掉 | high |
| N12 | `GSEE/Private/GeometryGenerate.cpp:153, 178` | 文件内整条调用链传入的都非空 | 两处都删 | high |
| N13 | `GSEE/Private/GeometryGenerate.cpp:1007, 897` | 元素来自 lambda 里的 `NewObject`，`ProcessAsync` 原样收集 | 只留三角数判断 | high |
| N14 | `PEP/Private/CSAssetProcess.cpp:79` | `check(NewRenderTarget2D)` 紧跟 `NewObject` | 删掉 | high（已复核） |
| N15 | `PEP/Private/CSShallowWaterProcess.cpp:87, 452` | 两条分支都已证明非空；`RT_ResultVelHeight ?` 已由提前返回保证 | 删掉 | high |
| N16 | `PEP/Private/CSBrushEdModeBase.cpp:195, 197, 241` | :191 `CreateBrushComponent` 总会赋值；私有 `TraceBrushRay` 重复判 ViewportClient | 删掉 | high |
| N17 | `PEP/Private/ComputeShaderLandscapeRoad.cpp:280, 147` | 私有函数，唯一调用点前已判 | 删或 `check` | high |
| N18 | `PEP/Private/RoadBuilderShaders.cpp:142, 36, 347` | 调用点参数都来自 `RegisterExternalBuffer` 或已判过 | 删掉被调方的重复判断 | high |
| N19 | `PEP/Private/SelectedActorViewportOverlayBase.cpp:687, 688, 241` | 私有函数唯一调用点前已 `IsValid`；`TFieldIterator` 不产出空 | 删掉 | high |
| N20 | `PEP/Private/CSLandscapeEditLayer.cpp:26` | `TActorIterator` 默认跳过 `!IsValid` | 删掉 | medium |
| N21 | `PEP/Private/CSInstanceBrushEdMode.cpp:91` | 私有函数唯一调用点 :44 前 :40 已判 | 把 Target 作参数传进来 | high |
| N22 | `PEP/Private/PCGEditorProcess.cpp:291, 303, 315, 327` | 广播点传的都是 `this` | 去掉或 `check` | medium |
| N23 | `GSEE/Private/GeometryGeneral.cpp:1679`；`PEP/.../ComputeShaderLandscape.cpp:732` | 存活对象的 `GetOutermost()` 不会为空 | 删掉 | medium |
| N24 | `PEP/Private/ComputeShaderLandscape.cpp:694` | `DebugResult` 只在 `R_Debug != nullptr` 时赋值 | 改 `if (DebugResult)` | high |

**dead-code**：

| ID | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- |
| D1 | `ComputeShaderGenerator/Private/GeometryAsync.h` | 整份文件没有任何地方包含（唯一 include `GeometryGenerate.cpp:57` 解析到 GSEE 那份，两份只差一行注释）；`Parallel_For` 两份都 0 调用 | 删 CSG 那份与 `Parallel_For` | high |
| D2 | `GSEE/Public/GeometryGeneral.h:103` | `TreeWindMergeComponents` 空函数体、无调用 | 删除 | high |
| D3 | `GSEE/Public/GeometryGeneral.h:159` | `FindNearestComponents` 模板从未实例化；`FWindTreeReduceData` 随之没用 | 删除 | high |
| D4 | `GSEE/Public/GeometryGeneral.h:154` | `SaveDynamicMeshToStaticMesh(UDynamicMesh*, int32)` 无调用 | 删除 | high |
| D5 | `GSEE/Public/GeometryMathUtils.h:31` | `FindNearPointIteration` 两个重载无调用，删后 `UPointFunction` 成空类 | 删除 | high |
| D6 | `GSEE/Public/GeometryMathUtils.h:127` | `CountIf`、`Reduce_Custom` 两个重载无调用 | 删除 | high |
| D7 | `GSEE/Public/GeometryAttribute.h:64` | 非 inline 的 `GetAttrib` 写在 Public 头里（再被别的 TU 包含就 LNK2005）；`PointInt` 与 6 个属性宏无人用（VV/VP 前缀还写错）；被 `GeometryGeneral.cpp` 包含了两次 | 删除 | high |
| D8 | `PEP/Private/SelectedActorViewportOverlayBase.h:34` | `CreateDetailsCategoryOverlayWidget` 无调用，函数体与 `CreateOverlayWidget` 相同 | 删除 | high |
| D9 | `PEP/Private/RoadBuilderShaders.cpp:11` | `ROAD_SHADER` 宏从没用过 | 删除 | high |
| D10 | `GSEE/Private/GeometryGenerate.cpp:847` | `bool UsePointVDB=false; if (UsePointVDB)`，约 108 行永不执行（含候选重复块 855-890） | 删除 | high |
| D11 | `GSEE/Public/VDBExtra.h:92` | `VDBParticleList::Add` 无调用 | 删除 | medium |
| D12 | `GSEE/Private/FoliageConverter.cpp:370`；`GeometryMathUtils.cpp:309-311` | `AddFoliage`、`Reduction` 是空函数体 UFUNCTION | 先查 BP 引用再删 | medium |
| D13 | `GSEE/Private/GeometryGeneral.cpp:233` 等 | `WindDataForTree` 十几个写了不读的局部变量；出参 `OutHolePositions` 从未写入；`DiscardPoint` 分支恒假 | 清理；出参要么填上要么删掉 | high |
| D14 | `GSEE/Private/GeometryGeneral.cpp:1326-1345` | `FillUVData` 对每个三角形的每个顶点算一次测地路径，结果直接丢掉；外加一批死局部变量 | 删除 | high |

**duplicate / redundant-logic / alias**：

| ID | 类别 | 位置 | 问题与证据 | 建议 | 置信度 |
| --- | --- | --- | --- | --- | --- |
| P1 | duplicate | `GSEE/Private/FoliageConverter.cpp:118, 151, 192, 224, 298, 347` | 同一段前置（GWorld → IFA(create=true) → FoliageTypes[0] → FindInfo）6 份，"取第一个 IFA" 2 份 | 提 `FindFoliageInfoForMesh`；getter 改传 `bCreateIfNone=false` | high |
| P2 | duplicate | `GSEE/Private/GeometryGenerate.cpp:958-1004 ↔ 1037-1074` | `CollectMeshsMultThread` 与 `CollectMeshs` 逐实例流程相同，LOD 拷贝（826 ↔ 1024）也重复 | 提 `CopyStaticMeshLOD` / `BuildCulledInstance` | high |
| P3 | duplicate | `GSEE/Private/GeometryGenerate.cpp:726 ↔ 1098；772 ↔ 1153` | 扫掠搭建与 Editor 选择重复 | 提 `AppendBoundarySweep` | high |
| P4 | duplicate | `GSEE/Private/GeometryGenerate.cpp:516-529 ↔ 675-689` | 地形裙边收尾相同 | 提 `FinishWithLandscapeSkirt` | high |
| P5 | duplicate | `GSEE/Private/GeometryGenerate.cpp:413-488, 576-646, 1286-1326` | 三份收集逻辑已各自走样（见 K5） | 提 `CollectStaticMeshPlacements` | medium |
| P7 | duplicate | `PEP/Private/ComputeShaderLandscapeRoad.cpp:22` | `CSRoad_CanGenerateFor` 与 `GeometryEditorActor.cpp:3220` 的 `VineLeaf_CanGenerateFor` 逐行相同 | 移到 CSG 作公共 helper | high |
| P8 | duplicate | `GSEE/Private/VDBExtra.cpp:64-107` | "粒子 → LevelSet" 与 `ComputeShaderMeshGenerator.cpp:3606` 相同，两份粒子表的非有限值过滤已不一致 | 在 CSG 统一一份 | medium |
| P9 | duplicate | `GSEE/Private/GeometryGeneral.cpp:883` | `WeldVertices` 约 230 行基本照抄引擎 `FMergeCoincidentMeshEdges::Apply`，5.7 已暴露所需选项 | 改用引擎算子并对照结果 | medium |
| P10 | duplicate | `PEP/Private/CSAssetProcess.cpp:112, 282, 440`；`CSShallowWaterProcess.cpp:585, 610` | "关卡路径 Split + Append" 5 份，且 Append 原地改写原字符串；正确实现在 `ComputeShaderMeshGenerator.cpp:3801` | 提 `GetLevelAssetFolder` | medium |
| P11 | duplicate | `GSEE/Private/LandscapeExtra.cpp:79, 172`；`PEP/.../CSLandscapeEditLayerBase.cpp:28` | 找地形 3 份，`ProjectPoint` 逐点调用每次都重新查 | 提 `FindFirstLandscape` 并缓存 | medium |
| P12 | duplicate | `GSEE/Private/LandscapeExtra.cpp:256-276 ↔ 315-339；297-308` | 法线计算与 pow2 循环重复 | 提 `ComputeHeightNormal` | medium |
| P13 | duplicate | `PEP/Private/PCGEditorProcess.cpp:228-323` | 4 份 Start、4 份 Register 与叶子模式样板 | 提 `TCSBrushEdMode<TTarget>` | medium |
| P14 | duplicate | `ES/Private/EditorShortcutsModule.cpp:16` | `AuditBindings` 与 `Subsystem.cpp:92-142` 流程与文案重复 | 提 `CollectStaticProblems` | medium |
| P15 | duplicate | `GSEE/Private/GeometryGenerate.cpp:105 ↔ 338-341` | 同一个表面点数估算公式 | 提 `EstimateSurfacePoints` | medium |
| P16 | duplicate | `PEP/Private/ComputeShaderLandscape.cpp:607-613 ↔ 654-660；867-868 ↔ 797-798` | 参数填写重复 | 提 lambda（收益小） | medium |
| P17 | duplicate | `PEP/Private/PCGEditorProcess.cpp:342-358` | 同一份骨骼列表加了两遍 | 提 `AddBones` lambda | medium |
| P18 | duplicate | `PEP/Private/CSLandscapeEditLayerBase.cpp:201-217 ↔ 251-261` | 写基础层收尾相同且已走样（见 K14） | 提 `WriteHeightToBaseLayer` | medium |
| L1 | redundant-logic | `GSEE/Private/GeometryGeneral.cpp:1098` | `WeldVertices` 的 if / else 两支完全一样；:911 三元两边相等；多个死局部变量 | 合并，或按 P9 整体替换 | high |
| L4 | redundant-logic | `GSEE/Private/GeometryGenerate.cpp:326` | `ShouldSampleBounds` 循环后仍 `return true`，参数 `BBoxVectors` 不起作用 | 先定本意 | high |
| L5 | redundant-logic | `GSEE/Private/GeometryGenerate.cpp:125` | `GridSteps` 至少返回 1，三元多余 | 去掉 | high |
| L6 | redundant-logic | `GSEE/Private/LandscapeExtra.cpp:374-409` | `CreateLandscapeMeshTextureData` 建好平面就丢弃，返回值恒空，`TextureSize` 没用 | 删除或补完实现 | high |
| L7 | redundant-logic | `GSEE/Private/GeometryGeneral.cpp:1394-1398` | 两个读回循环无效果；:1394 用顶点 ID 写三角形属性 | 删除并修正 | high |
| L8 | redundant-logic | `GSEE/Private/GeometryGeneral.cpp:73` | 默认构造的选择集必然为空 | 删掉 | high |
| L9 | redundant-logic | `GSEE/Private/FoliageConverter.cpp:325` | `BaseComponent` 默认就是 nullptr | 直接赋值 | high |
| L10 | redundant-logic | `GSEE/Private/GeometryVisualization.cpp:86, 123-128` | :56 已 `Max(GridSize,2)`；颜色与循环无关且与注释不符 | 去掉三元；颜色按注释实现 | high |
| L11 | redundant-logic | `PEP/Private/CSAssetProcess.cpp:161-177, 275` | 为两个从不读的数组把整个目录的资产都加载一遍；给按值参数赋 null 无意义 | 删除 | high |
| L12 | redundant-logic | `PEP/Private/CSAssetProcess.cpp:357` | 新建路径上 :344 已转过一次 | 只在已有资产分支转 | medium |
| L13 | redundant-logic | `PEP/Private/CSShallowWaterProcess.cpp:486-487, 505-506` | 保存之后又标脏 | 调换顺序或去掉 | medium |
| L14 | redundant-logic | `PEP/Private/ComputeShaderLandscape.cpp:965-970` | 查出来的 Actors 没用，每次覆盖只剩最后一个 | 改 `TActorIterator`，找到即 break | high |
| L15 | redundant-logic | `PEP/Private/ComputeShaderLandscape.cpp:870` | :863 的 `CopyLandscapeData` 里（:284）已读过一次 | 删掉或注明原因 | medium |
| L16 | redundant-logic | `PEP/Private/PCGEditorProcess.cpp:334` | :54 必定加 Root，`IsEmpty` 恒假 | 删掉 | high |
| L17 | redundant-logic | `GSEE/Private/GeometryMathUtils.cpp:53` | :52 已乘过一次 `Frequency`，又乘一次 | 先确认本意（改了输出会变） | medium |
| L18 | redundant-logic | `GSEE/Private/GeometryVisualization.cpp:21` | 只读操作用了 `EditMesh`，多广播一次修改，且忽略 ID 空洞 | 改 `ProcessMesh` | medium |
| L19 | redundant-logic | `GSEE/Private/LandscapeExtra.cpp:222-237, 104` | 死局部变量；重复取 :90 已有的值 | 删掉 | high |
| L20 | redundant-logic | `GSEE/Private/GeometryGenerate.cpp:1186` | 中断器永远返回 false | 换平凡实现 | medium |
| L21 | redundant-logic | `PEP/Private/ViewEditCategoryViewportOverlay.cpp:7` | 唯一调用点已判 `IsValid` | 给基类加默认实现 | medium |
| A1 | alias | `PEP/Private/CSBrushEdModeBase.cpp:108` | `CapturedMouseMove` 与 `MouseMove` 逐行相同 | `return MouseMove(...)` | high |
| A2 | alias | `GSEE/Private/LandscapeExtra.cpp:79, 172` | `GetWorldFromContextObject(GWorld, …)` 等价于 GWorld | 直接用 GWorld | medium |
| A3 | alias | `PEP/Private/GeometryEditorFunction.cpp:17` | 纯转发，注释说明为兼容 BP 保留 | 保留，仅登记 | high |

范围外附带（已核实，与判空 / 冗余无关）：`PEP/.../ComputeShaderLandscape.cpp:442` 在 `ENQUEUE_RENDER_COMMAND` 里调用要求游戏线程的 `GameThread_GetRenderTargetResource`；`CSLandscapeEditLayerBase.cpp:172/190` 读回结果没查大小就用；`GeometryGeneral.cpp:386/515/553` 的 `ComponentDatas[ClassNum]` 可能触发 TMap 的 check；`GeometryGeneral.cpp:1363` 颜色 overlay 用 UV 元素 ID 作下标；`GeometryGenerate.cpp:823-841/958-1004` 在 TaskGraph 工作线程上调用 `NewObject` 和 GeometryScript。

## 五、复核记录

下列条目我亲自读过代码，部分还对照了引擎源码，结论都属实：

- **4.1 房子**：H02。`CSHouse_SolveRoadRuns` 的说明写明，闭环时可能有一条 `S0 < Lo` 的绕回段，见 `CSHouseDoorRuns.h:140`。`QueryFeatureReject` 要求开口表按 (EdgeIndex, CenterS) 升序，见 `CSHouseProfile.h:1493-1496`。
- **4.2 地面 / 楼梯 / 墙**：
  - G2-03：`EnsureInstancedComponent` 要么保留有效组件，要么 `NewObject`。
  - G2-04、G2-05：引擎 `UMaterialInstanceDynamic::Create` 末尾无条件 `NewObject` 并返回。
  - G2-07。
  - G2-09、G2-10、G2-11：全插件 grep 只出现在声明处。
- **4.3 GPU 网格**：
  - G3-01：`FCSGpuMeshSceneProxy` 只有两个子类，都是 `final`。
  - G3-03、G3-04、G3-28：grep 确认。
  - G3-21、G3-22。
- **4.4 生成器与核心**：
  - X01：成员不是 `UPROPERTY`，模块里也没有 `AddReferencedObjects`。
  - X02：`BlurTexture_Internal` 在 `Execute` 之前就返回了。
  - X03：`CreateRWBuffer` 的签名见 `ComputeShaderGenerateHelper.h:243`。
  - 脚本扫出、子代理确认的 N01、N02（`NewObject` 之后紧跟判空）。
- **4.5 功能模块**：
  - U1 的越界：`GenerateDatas` 只有 NumIteration 个元素，循环却走到 `MeshDataAssets.Num()`。
  - R6：只读了 4100-4101 两行，「被改回 true」那一处没有逐行跟。
  - N1：构造函数里 `CreateDefaultSubobject` 之后立即判空。
- **4.6 编辑器模块**：
  - K1：守卫是 `<`，`GetUVLayer(UVNum)` 越界返回空。
  - K6：出参从不赋值。
  - L3：先除后判零。
  - N14：`NewObject` 之后立即 `check`。

其余条目由子代理按同一标准核实（每条都附了 file:line 证据），我没有逐条复核。

## 六、按口径不报的检查

下列判空即使看起来多余，也一律视为必要，不计入：

- **构造函数之外，对 `UPROPERTY` 组件 / 对象指针的判空。** 本项目有真实案例：`BP_VineSource` 的 `VineGpuMesh` 在资产里存成了 null，代码靠 `RebindVineGpuMeshIfMissing` 兜底。
- **`UFUNCTION` / BlueprintCallable 的参数**，以及蓝图、Python 能调到的入口。
- **以下调用的返回值：** `Cast<>`、`FindObject` / `LoadObject`、`TWeakObjectPtr::Get()`、`SpawnActor`、`GetWorld()`（可能跑在 CDO、编辑器或 commandlet 上）、`GEngine` / `GEditor`、`SceneProxy`、渲染资源。
- **GC 或异步边界之后**再访问的对象。
- **导出 API 的防御性检查**：公开接口留着无害，表里只标「可删」。
- **零引用的 `UFUNCTION` / `UPROPERTY`。** 即使 C++ 里零引用也不判为死代码，因为蓝图、Python、资产可能在用；只列出来供用 Reference Viewer 复核。例如 4.1 末尾的 4 个函数、4.4 D16。

## 七、建议的处理顺序

1. **先修[第二节](#二要先修的真实缺陷p0)。** 按模块分批，每批跑 `RunTests PCGPlugins` 和演示回归，基线见测试记录。第 1-4 条会写坏内存或触发断言，优先级最高。
2. **删死代码和死子系统。** 第三节前三项和各组的 dead-code，收益大、风险小。
3. **结构性去重。** 需要先定落点：放基类还是 helper，要不要插中间类（插中间类会影响蓝图资产的类层级）。
4. **多余判空。** 按模块批量清理，机械、低风险；可以改成 `check` 的地方，优先改 `check` 而不是直接删。

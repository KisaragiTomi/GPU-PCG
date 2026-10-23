# 生命周期与持久化审查

> 子代理 C 的完整报告（2026-09-07，只读审查；引擎证据读自 `D:\UnrealEngine-5.7.4-release`——报告原写的 `D:\UE-SourceCode-5.7.4` 是同一份源码的旧路径，本机已不存在）。主审查逐条抽查 C1–C13 的插件侧引文与 C1 / C2 / C3 / C4 / C5 / C6 / C8 的引擎侧引文后，把结论并入 [`Review_Structure.md`](Review_Structure.md)「生命周期与持久化（深挖 G）」一节（26–30）并订正了第 12 条；本文保留状态分类表、六条路径矩阵与钩子对称性矩阵作参考，行号漂移与升级为已核实的推测见末尾「主审查复核记录」。行号按报告生成时刻。

审查对象：`Plugins/PCGPlugins/Source/ComputeShaderGenerator/`（`CSHouseActor` / `CSGroundActor` / `CSGroundShaperActor` / `CSTinyGlade` / `CSHouseFeatureMarker` / 两族抓手 / `CSHouseSubsystem`）与 `Source/PCGEditorProcess/`（`CSBrushEdModeBase` / `CSWindowBrushEdMode` / `CSGroundPaintEdMode` / `CSHouseResizeSelectionWatcher` / `PCGEditorProcess.cpp`）。引擎依据：`D:\UnrealEngine-5.7.4-release`（报告期路径 `D:\UE-SourceCode-5.7.4`，同一份源码）。只读审查；行号为读取时刻（2026-09-07）的行号。

标注约定：**已核实** = 读到源码原文并附 `文件:行号`；**推测** = 从已核实的机制推导、未跑用例；引擎证据用 `Engine/…` 路径。

## 结论摘要

按严重度：C1 拉尺寸 / 调高度不进事务（高）、C2 `RF_WasLoaded` 闸门让 PIE 丢窗（高）、C3 撤销顺序确定但参照系不回滚（高）、C4 撤销三次重求值（中）、C5 复制撞 GUID 与抢登记格（中）、C6 落笔撤销只撤一半（中）、C7 地面订阅以句柄代身份（中）、C8 卸载只来 `BeginDestroy` 与 GPU 释放不对称（中）、C13 笔刷标记首次松手被降级（中）、C9 / C10 / C11 / C12（低—中）。逐条与证据见「发现」；完整摘要见文末「结论摘要（终稿）」。六条生命周期路径里只有「新建」与「`DestroyActor`」有测试，其余零覆盖。

## 状态分类表

分类口径：**权威·序列化**（存盘且应进事务）／**派生·Transient**（可从权威重算）／**持久记忆·NonTransactional**（存盘、路径依赖、刻意不进事务）／**拖动期临时**（一次交互内有效）／**跨 actor 登记·副本**（别人的事实在本对象上的抄本或订阅）。"一致？"判的是说明符是否与分类相符；不一致的在最后一列给出编号，正文「发现」里展开。

### ACSHouseActor（`Public/CSHouseActor.h`）

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `FootprintSize` / `WallHeight` / 其余 EditAnywhere 参数 | 246–1296 | 权威·序列化 | `EditAnywhere, BlueprintReadWrite` | **否**：由 `PushEdge` / `PushHeight` 直写、从不 `Modify()`（C1） |
| `Ground` | 1297 | 跨 actor 登记（强引用，序列化） | `EditAnywhere` `TObjectPtr` | 部分：自动解析结果被固化进关卡；地面被删后仍被当作有效（C7） |
| `WindowBrushClass` / `Windows` | 1539 / 1538 | 权威·序列化 | `EditAnywhere` | 是 |
| `HouseId` | 2451–2452 | 权威·序列化（身份） | `UPROPERTY()` | **否**：缺 `PostDuplicate` / `NonPIEDuplicateTransient`（C5） |
| `DoorRunMemory` / `PierSpanIsPier` | 2203–2204 / 2213–2214 | 持久记忆 | `NonTransactional` | 是（撤销后由三次重求值继承，见校核表） |
| `ResizeHandles` | 1904–1905 | 拖动期临时（编辑设施） | `Transient` `TArray<TObjectPtr>` | 是；抓手 actor 本身 `RF_Transient` 且无 `RF_Transactional`（`CSHouseActor.cpp:1190`） |
| `MarkerWindows`（`FCSMarkerWindow`：`MarkerId` / `Window` / `Anchor` 抄本 / 弱引用） | 1925–1947 | 跨 actor 登记·副本 | 无 UPROPERTY | 部分：加载由标记重登记 ✓；撤销不回滚、`Destroyed`/`EndPlay` 之外无人清（C3 / C6） |
| `MarkerRefFootprint` / `MarkerRefThickness` / `MarkerRefBuild` / `bMarkerRefValid` | 1957–1960 | 派生参照系，但跨帧承担"上一轮权威"的记忆 | 无 UPROPERTY | **否**：撤销把 `FootprintSize` 回滚而参照系不回滚（C3） |
| `CurrentFeatureVerdicts` | 1970 | 派生 | 无 UPROPERTY | 是 |
| `CurrentOpenings` | 2189–2190 | 派生 | `Transient` | 是（事务会捕获它，无害） |
| `BodyShapeHash` / `BodyPlacementHash` / `PillarShapeHash` / `PillarPlacementHash` / `*DescHash` | 2226–2229, 2259, 2311, 2354, 2375, 2378, 2397 | 派生（幂等守卫） | 无 UPROPERTY | 是 |
| `BodyBuiltAtTransform` / `PillarBuiltAtTransform` | 2232–2233 | 派生参照系（GPU 实际所在） | 无 UPROPERTY | 是（松手 `bForceFullRebuild` 对齐） |
| `Frame/Vine/Pillar/RoofTile/Decor GpuBuffers`、`*HandedCapacities`、`*HandedLocalBounds` | 2166–2167, 2253–2257, 2306–2308, 2340, 2388–2394 | 派生 GPU 资源 | 无 UPROPERTY | 部分：只在 Ensure*/Rebuild* 释放，三条销毁路径都不释放（C8，与地面不对称） |
| `Current*Count`、`CornerPierTopZ` | 2221–2275, 2312–2314, 2355, 2376, 2379, 2398–2400 | 派生（诊断面） | 无 UPROPERTY | 是 |
| `FrameComponent` / `Vine*Component` / `RoofTileComponent` / `RoofFinialComponents` / `DoorLeafComponents` / `DecorComponents` | 2249, 2278–2284, 2335, 2368, 2372, 2383 | 派生（`NewObject(..., RF_Transient)`，`CSHouseActor.cpp:1605/1957/2768/3316/3615/3766/3949`） | `Transient` | 是 |
| `PillarMeshComponent` / `VineTubeComponent` | 2154–2155 / 2178–2179 | CDO 子对象 | `VisibleAnywhere` | 是 |
| `PillarMesh` / `VineTubeMesh` / `TinyGladeMesh`（基类 `CSTinyGlade.h:73`） | 2158, 2182 | 派生 GPU 投影 | `Transient` | 是 |
| `VineLeafSeasonMID` / `VineBranchGrowMID` / `VineFlowerGrowMID`；`*MeshBuiltFrom` | 2289–2302；2323–2329, 2346, 2407 | 派生 | `Transient` | 是 |
| `VineStrandHistory` | 2440–2448 | 持久记忆（相位 memo），设计上不跨关卡 | 无 UPROPERTY | 是 |
| `PendingBodySnapshot` / `PendingPillarSnapshot` / `PendingVineTubePath` | 2414–2417 | 拖动期临时（在途合并槽） | 无 UPROPERTY | 是；完成回调持 `WeakThis`（`CSHouseActor.cpp:1523, 1534–1537`） |
| `bForceFullRebuild` / `bInReevaluate` | 2411 / 2455 | 拖动期临时 | 无 UPROPERTY | 是 |
| `GroundChangedHandle` | 2454 | 跨 actor 登记（订阅句柄） | 无 UPROPERTY | **否**：以句柄是否有效代替"订阅的是哪块地面"（C7） |
| `OnResizeModeChanged` / `OnWindowBrushRequest`（静态） | 1388 / 1532 | 进程级委托 | 无 | 部分：跨 world（C11） |

### ACSGroundActor（`Public/CSGroundActor.h`）

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `NumCellsX` / `NumCellsY` / `CellSize` | 234–243 | 权威·序列化（形状配置） | `EditAnywhere, BlueprintReadOnly` | 是；但与 `Mirror` 是两份权威，靠 `EnsureMirrorInitialized` 以"重置"调和（C9 之地面条） |
| `Mirror`（`FCSGroundMirror`：`NumVertsX/Y`、`CellSize`、`Heights`、`Colors`） | 33–47, 1574–1575 | 权威·序列化，刻意豁免事务 | `NonTransactional`；结构体字段 `UPROPERTY()` | 是（与「笔刷无 Undo」裁决一致；后果见校核表） |
| `OnGroundChanged` | 227 | 跨 actor 登记（订阅表） | 无 UPROPERTY | 是（随 actor 消亡；订阅方句柄见 C7） |
| `MeshBuiltAtLocation` | 1578 | 派生参照系（GPU 实际所在） | 无 UPROPERTY | 是：`PostEditUndo`→`RebuildGroundMesh` 重置（`CSGroundActor.cpp:2413–2422, 195`） |
| `MaxAbsHeight` | 1581 | 派生 | 无 UPROPERTY | 是（`EnsureMirrorInitialized` 重算，`CSGroundActor.cpp:96–100`） |
| `PaintRevision` | 1534 | 派生计数（哈希输入） | 无 UPROPERTY | 是 |
| `Shapers` | 1587 | 跨 actor 登记（弱引用表） | 无 UPROPERTY | 是（双向自登记：`ResolveShapers` + `RegisterShaper`） |
| `StairComponent` / `StairPebbleComponent` / `RockShellMesh` / `RockShellComponent` / `RockShellMaterialInstance` / `SkirtDecorComponents` / `CoverComponents` | 1498–1513, 1591, 1620 | 派生 | `Transient` | 是 |
| `StairBuffers` / `SkirtDecorGpuBuffers` / `CoverBuffers`；`Handed*`；`RockShellBuilt*`；`SkirtDecorHash` / `CoverBuiltHash` | 1537, 1594, 1623; 1560–1563, 1601, 1638; 1517–1522; 1605, 1642 | 派生 GPU 资源 / 幂等守卫 | 无 UPROPERTY | 部分：`EndPlay` + `Destroyed` 释放，`BeginDestroy` 不释放（C8） |
| `StrokeDirtyBounds` / `bPaintStrokeOpen` / `PendingPaintDabs` | 1645–1646 / 1653 | 拖动期临时 | 无 UPROPERTY | 部分：脏标记只在 `EndPaintStroke`（C10） |
| `OnGroundPaintEditorRequest`（静态） | 217 | 进程级委托 | 无 | 是 |

### ACSGroundShaperActor（`Public/CSGroundShaperActor.h`）

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `Radius` / `LiftHeight` / `FalloffDistance` / 噪声四项 | 58–115 | 权威·序列化 | `EditAnywhere` | 是 |
| `Ground` | 124 | 跨 actor 登记（强引用，序列化） | `EditAnywhere` | 部分（同房子的 `Ground`，C7） |
| `EditorShapeMesh` / `EditorShapeComponent` / `SpriteComponent` | 128 / 181 / 185 | 编辑器道具 | `EditAnywhere` / `UPROPERTY()` / `UPROPERTY()`（EditorOnlyData） | 是 |
| `LastAppliedFootprint` | 189 | 派生参照系（上次生效足迹） | 无 UPROPERTY | 是：撤销后取 union(旧, 新) 恰好覆盖（`CSGroundShaperActor.cpp:120–126`） |
| 基类 `TinyGladeMesh` / `TinyGladeMeshComponent` | `CSTinyGlade.h:69, 73` | 未使用 | `VisibleAnywhere` / `Transient` | 是（死重，既有文档 7 已记） |

### ACSHouseFeatureMarker / ACSHouseHandleActor（`Public/CSHouseFeatureMarker.h`、`Public/CSHouseHandleActor.h`）

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `Anchor` | 346 | 权威·序列化，进事务（写前 `Modify()`：`.cpp:204, 386`） | `VisibleAnywhere` | 是 |
| `LastAcceptedAnchor` | 354 | 权威·序列化（回位记忆） | `VisibleAnywhere` | 是（写在同一次 `Modify()` 之后） |
| `MarkerId` | 320 | 权威·序列化（身份） | `VisibleAnywhere` | **否**：构造期生成、复制沿用（C5） |
| `bCausesCut` / `LastReject` | 323 / 326 | 派生回执 | `VisibleAnywhere, Transient` | 是 |
| `Host`（基类） | `CSHouseHandleActor.h:150` | 跨 actor 登记副本（弱引用） | `Transient` | 是：权威是 attach 父（`.cpp:94–104`）；但恢复路径被 `RF_WasLoaded` 闸住（C2） |
| `bHasBeenPlaced`（基类） | `CSHouseHandleActor.h:159` | spawn 期临时 | 无 UPROPERTY | **否**：只有 `AddActor` 路径会置位；笔刷生成的标记第一次松手被降级（C13） |
| `bDestroyWhenHostless`（基类） | `CSHouseHandleActor.h:86` | 权威 | `EditAnywhere` | 是 |
| `IdleSeconds` / `LastTickLocation` | 366–367 | 拖动期临时 | 无 UPROPERTY | 是 |
| `PickSprite` | 338 | 编辑器道具 | `UPROPERTY()`（EditorOnlyData） | 是 |
| `OpeningMesh` / `LintelMesh` / `SillMesh` / `GlassMesh` | 236–261 | CDO 子对象；可见性是派生回执 | `VisibleAnywhere` | 是 |

### 抓手两族（`CSHouseResizeHandleActor.h`、`CSHouseHeightHandleActor.h`）

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `EdgeIndex` | Resize:127 | 拖动期临时 | `Transient` | 是（actor 本身 `RF_Transient`） |
| `LastConsumedWorld` | Resize:134 / Height:95 | 拖动期临时（记账量） | `Transient` | 是 |
| `LastAppliedOffset` | Resize:137 / Height:98 | 拖动期临时 | 无 UPROPERTY | 是 |
| `ArrowComponent` / `BarComponents[4]` | Resize:141 / Height:102 | 编辑器道具 | `Transient` | 是 |
| `HandleOffset` / `HandleHeightFraction` / `FrameScale` / `FrameThickness` | Resize:108–112 / Height:74–78 | 观感参数 | `EditAnywhere` | 是（actor 不存盘，等于每次进模式都回默认） |

### UCSHouseSubsystem（`Public/CSHouseSubsystem.h`）

> 2026-09-16 晚：本类已整个删除（名单 = `UCSHouseLibrary::GetHouses`，快扫由根组件 `TransformUpdated` 取代，找宿主 / 放窗搬到 `UCSHouseLibrary`，见计划 D10 状态）。以下为删除前的审查记录。

| 状态 | 行 | 分类 | UPROPERTY 说明符 | 一致？ |
| --- | --- | --- | --- | --- |
| `Tracked`（`FGuid` → 弱引用 + `TrackingHash`） | 126–133 | 跨 actor 登记 | 无 UPROPERTY | 部分：`Tick` 惰性清理失效项 ✓（`.cpp:139–143`）；键是 `HouseId`，复制撞键（C5） |
| `DirtyHouses` | 134 | 跨 actor 登记（待重求值集） | 无 UPROPERTY | 是 |
| `ScanAccumulator` / `ScanWakeCount` | 136–137 | 运行时计数 | 无 UPROPERTY | 是 |
| `ScanInterval` | 114 | 配置 | `EditAnywhere, BlueprintReadWrite` | 无意义（子系统没有细节面板）；低 |

## 发现

### C1. 拉尺寸 / 调高度整条链不进事务：Ctrl+Z 只回滚它的副作用（严重度：高；已核实）

- 证据：
  - `CSHouseActor.cpp:1110–1139` `PushEdge`：`FootprintSize = NewSize; SetActorLocation(NewCentre);` —— 全函数无 `Modify()`；`PushHeight`（`1141–1169`）同。全文件 `grep Modify(` 只命中注释 `1186`。
  - `CSHouseActor.cpp:1186–1190` `EnterResizeMode`：`SpawnParams.ObjectFlags = RF_Transient;` 注释写「真正该被撤销的是 FootprintSize，而它由 PushEdge 直接写」—— 但没有任何一处把它写进事务。
  - 引擎：`Engine/Source/Runtime/CoreUObject/Private/UObject/UObjectGlobals.cpp:3361–3372` `SaveToTransactionBuffer`：`if ( GUndo && bIsTransactional && bIsNotScriptPackage)`，`bIsTransactional = Object->HasAnyFlags(RF_Transactional)` —— 抓手没有 `RF_Transactional`，gizmo 对它的 `Modify()` 是空操作。
  - 引擎：`Engine/Source/Editor/UnrealEd/Private/Elements/Actor/ActorElementEditorViewportInteractionCustomization.cpp:17–38` `GizmoManipulationStarted`：只对被拖的 actor 及其 **attached 子级**（`GetAttachedActors(..., bRecursivelyIncludeAttachedActors=true)`）调 `Modify()`；宿主房是父级，不在其中。`LevelEditorViewport.cpp:3735–3765` 在拖动期开着 "Move Elements" 事务。
  - `CSHouseActor.cpp:301–343` `ReanchorMarkersToPreserveWorld` → `M->Reanchor(A)`；`CSHouseFeatureMarker.h:141–145` `Reanchor` 调 `Modify()`。标记由 `CSHouseSubsystem.cpp:213–216` `World->SpawnActor` 默认参数生成（`RF_Transactional`）⇒ **标记进了这笔事务，房子没有**。
- 触发与症状：进拉尺寸模式，用 gizmo 推一面墙（墙上有窗），Ctrl+Z。房子尺寸与位置纹丝不动；标记的 `Anchor` 被回滚到推墙前的 `DistFromCorner` ⇒ 标记 `PostEditUndo`（`CSHouseFeatureMarker.cpp:175–197`）按旧锚点在新墙上重登记 + `SnapToAnchor` ⇒ 洞与窗框一起沿墙滑动（`bFromEndCorner` 一侧的窗滑 Δ=新长−旧长）。再按一次 Ctrl+Z 撤的是上一件不相干的操作。调高度同理（只是没有标记副作用，表现为"Ctrl+Z 无反应且吃掉一步撤销"）。`ReevaluateSite` 落座的 `SetActorLocation`（`966–979`）同样不进事务（它会被地面广播自愈，可接受）。
- 建议：`PushEdge` / `PushHeight` 写权威量之前 `Modify()`（`FTransaction::SaveObject` 按对象去重，`EditorTransaction.cpp:701–720`，同一事务内只序列化一次）；或抓手生成时保留 `RF_Transactional` 并由抓手 `OnHandleDrag` 里 `H->Modify()`。两者都要配合 C3 的 `PreEditUndo`。

### C2. `RF_WasLoaded` 被当作「从盘来」闸门：本会话新放的窗在 PIE 里不登记（严重度：高；已核实）

- 证据：
  - `CSHouseFeatureMarker.cpp:75–117` `PostRegisterAllComponents`：`if (!HasAnyFlags(RF_WasLoaded)) return;` 之后才走「attach 父 → `Host` → `RegisterAnchor` → `SnapToAnchor`」。
  - 引擎：`World.cpp:4395–4413` `GetDuplicatedWorldForPIE` → `StaticDuplicateObjectEx`（`PPF_DuplicateForPIE`）；`UObjectGlobals.cpp:3057` 默认 `FlagMask = RF_AllFlags & ~(RF_MarkAsRootSet|RF_MarkAsNative|RF_HasExternalPackage)`，`3178` `Params.SetFlags = ApplyFlags | SourceObject->GetMaskedFlags(FlagMask)` ⇒ PIE 副本**只有源对象有 `RF_WasLoaded` 时才有**；`UObjectGlobals.cpp:1989` 该标志只在链接器加载时置位。
  - `FDuplicateDataWriter` 是持久化写入器（`Engine/Source/Runtime/CoreUObject/Private/Serialization/DuplicateDataWriter.cpp:40` `SetIsPersistent(true)`）+ `Property.cpp:1046`（Transient 在持久化时跳过）⇒ `Host`（Transient）、`bCausesCut` 等在 PIE 副本里全是默认值；房子的 `MarkerWindows` 非 UPROPERTY，PIE 副本为空。标记没有 `BeginPlay` 覆写，`Tick` 默认关（`.cpp:26`），没有第二条登记路。
- 触发与症状：笔刷放一扇窗（未保存重开关卡），直接 Play。PIE 世界里的标记 `PostRegisterAllComponents` 在闸门处早退 ⇒ 房子没有这扇窗的诉求 ⇒ 墙上无洞；标记的四件网格默认可见、attach 关系已复制 ⇒ **窗框贴在实墙上**。存盘并 `load_map` 之后再 Play 则正常（编辑器对象有了 `RF_WasLoaded`）——症状时有时无，取决于是否重开过关卡。
- 建议：闸门改判状态而不是来历：`if (!Anchor.IsValidAnchor()) return;` + `Cast<ACSHouseActor>(GetAttachParentActor())`。注释里担心的「`AddActor` 期用默认 +X 解析」在标记 `NotPlaceable`、只经 `PlaceMarkerAlongRay` 生成之后已不存在（`AdoptAnchor` 在 spawn 之后才写锚点，届时 `Anchor` 无效、闸门自然不放行）。

### C3. 撤销顺序是确定的；失同步的根源是非事务参照系 `MarkerRef*`，缺 `PreEditUndo`（严重度：高；已核实）

- 证据（引擎 `Engine/Source/Editor/UnrealEd/Private/EditorTransaction.cpp`）：
  - `816–820`：`Start/End` 由 `Inc` 决定；`Editor/UnrealEd/Classes/Editor/Transactor.h:304` `int32 Inc = -1;` ⇒ **撤销从最后一条记录往前走**，`Apply` 末尾 `1008 Inc *= -1` ⇒ 重做正向。
  - `701–720` `SaveObject`：同一对象在同一事务只在**第一次** `Modify()` 时 `Records.Add` ⇒ 记录序 = 首次 `Modify()` 序。
  - `848–878`：第一轮循环对每条记录 `PreEditUndo()` 并按记录序把对象插入 `ChangedObjects`（`TMap`，保持插入序）；`886–899`：**先把所有记录 `Restore`/`Load`**；`903–908` `KeyStableSort`（组件排到 actor 前，稳定）；`935–951`：然后才逐个 `PostEditUndo`。
  - 结论：actor 的 `PostEditUndo` 顺序 = 撤销时**首次 `Modify()` 的逆序**、重做时正序；且调用任何一个 `PostEditUndo` 时，所有对象的 UPROPERTY 都已恢复完毕。
  - 插件：细节面板改 `FootprintSize` ⇒ 房子先 `Modify()`（属性编辑的 `PreEditChange`）→ `PostEditChangeProperty` → 构造脚本 → `ReevaluateSite` → `ReanchorMarkersToPreserveWorld`（`CSHouseActor.cpp:301–343`）→ 标记 `Reanchor` `Modify()`。记录序 [房, 标记] ⇒ 撤销时**标记的 `PostEditUndo` 先跑**。
  - `CSHouseActor.h:1957–1960` `MarkerRefFootprint/MarkerRefThickness/MarkerRefBuild/bMarkerRefValid` 非 UPROPERTY，不随事务回滚。标记 `PostEditUndo`（`CSHouseFeatureMarker.cpp:175–197`）→ `RegisterAnchor` → `RegisterFeatureMarker` → 房子 `ReevaluateSite`：此刻 `FootprintSize` 已恢复为旧值，`MarkerRefFootprint` 仍是新值 ⇒ `bGeomChanged` 为真（`305–307`）⇒ 把**已恢复的旧锚点**当作「新墙上的锚点」再重表达一次（`312–336`）⇒ 锚点偏移；随后房子自己的 `PostEditUndo` 再跑三次重求值（C4）也不会纠正，因为参照系此时已"有效"。
- 触发与症状：细节面板把 `FootprintSize.X` 600→1000（墙上有窗），Ctrl+Z。尺寸回到 600，窗沿墙滑到错误位置（既有文档 12 预测的症状成立，但成因不是「先后不定」，而是「先后确定 + 参照系不回滚」）。
- 建议：房子覆写 `PreEditUndo()`（引擎在 `Restore` 之前对每条记录调它，`EditorTransaction.cpp:857–861`）置 `bMarkerRefValid = false`，让撤销后的第一次重求值只重建基线（`301–310` 路径）而不重表达。配合 C1 让房子本身也进事务。

### C4. 撤销 = 房子三次全量重求值、地面两次整张上传 + 两次全场广播、塑形物三次区域刷新；细节面板改属性 = 两次（严重度：中；已核实）

- 证据（引擎）：
  - `Engine/Source/Runtime/CoreUObject/Private/UObject/Obj.cpp:847–853` `UObject::PostEditUndo()` → `PostEditChange()`；`Runtime/Engine/Private/ActorEditor.cpp:803–822` `AActor::PostEditUndo` → `InternalPostEditUndo` → `Super::PostEditUndo()`。
  - `ActorEditor.cpp:152–243` `AActor::PostEditChangeProperty`：`bReregisterComponents` 为真时，有事务注解走 `UnregisterAllComponents → 逐个 RegisterComponent → RerunConstructionScripts → ReregisterAllComponents() 或 PostRegisterAllComponents()`（`181–237`），无注解走 `UnregisterAllComponents(); RerunConstructionScripts(); ReregisterAllComponents();`（`241–243`）。两条分支都跑构造脚本并再登记一次组件。
  - `Runtime/Engine/Private/ActorConstruction.cpp:252–300` `RerunConstructionScripts`：`bAllowReconstruction` 只被「正在构造 / 父蓝图正在编译 / 正在销毁」挡，**原生类照样放行**；`1005` `ExecuteConstruction` 末尾 `OnConstruction(Transform)`。
  - `ActorEditor.cpp:269–296` `AActor::PostEditMove`：`bFinished || bRunConstructionScriptOnDrag || Blueprint->bRunConstructionScriptOnDrag` 时 `RerunConstructionScripts()`。
- 插件侧计数：
  - 房子：`CSHouseActor.cpp:4456–4460` `OnConstruction` → `ReevaluateSite`；`4462–4472` `PostRegisterAllComponents` → `ReevaluateSite` + `RegisterHouse`；`4532–4540` `PostEditUndo` → `ReevaluateSite`。撤销 = 3 次；细节面板 = 2 次；gizmo 松手 = 2 次（`4518–4530`）。
  - 地面：`CSGroundActor.cpp:2388–2411` `PostEditChangeProperty` 收到引擎的**空属性事件**（`PostEditChange()` 传 `nullptr`）⇒ `PropertyName == NAME_None` ⇒ 落到 else 分支：石阶 / 岩壳 / 裙边 / 地被四链各跑一次；`536–542` `PostRegisterAllComponents` → `ResolveShapers` + `RebuildGroundMesh`（整张上传 + `OnGroundChanged` 广播 + 四链）；`2413–2422` `PostEditUndo` → `EnsureMirrorInitialized` + `RebuildGroundMesh` 又一次。每次广播都让**每栋房子**全量重求值（`CSHouseActor.cpp:261–265`）。
  - 塑形物：`CSGroundShaperActor.cpp:178–182` `PostEditChangeProperty` → `RebuildTerrain`；`151–155` `OnConstruction` → `RebuildTerrain`；`157–162` `PostRegisterAllComponents` → `RebuildTerrain`。撤销一次移动 = 3 次区域刷新（每次高度真变才广播，所以通常只 1 次广播）。
  - 标记：空属性事件走 `CSHouseFeatureMarker.cpp:238–250` `RefreshPieceLayout(); HandleDrag(false);`，见 C9。
- 触发与症状：任何一次 Ctrl+Z / Ctrl+Y；地面的一次撤销在多房关卡里是「N 栋房 × 2 次全量重求值」。
- 建议：`PostEditUndo` 覆写去掉自己那次（引擎那条链已经跑了两次）；地面的 `PostEditChangeProperty` 对 `PropertyName == NAME_None` 直接返回（那是撤销 / `PostEditChange()`，随后 `PostEditUndo` 会全量对齐）。

### C5. 复制 / 粘贴 / PIE 沿用 `HouseId` / `MarkerId`；引擎对 `ActorGuid` 的处理就是先例；复制标记会抢走原标记的登记格（严重度：中；已核实）

- 证据：
  - `CSHouseActor.h:2451–2452` `UPROPERTY() FGuid HouseId`，只在 `CSHouseActor.cpp:4467` 「无效才生成」；`CSHouseFeatureMarker.h:320` `UPROPERTY(VisibleAnywhere) FGuid MarkerId`，`CSHouseFeatureMarker.cpp:66` 构造期 `NewGuid()`。范围内无任何 `PostDuplicate` / `Serialize` / `PostEditImport` 覆写（grep 全部 .h/.cpp 只命中 `BeginDestroy`）。
  - 引擎：`Property.cpp:1039–1060` `ShouldSerializeValue`：只有 `DuplicateTransient` / `NonPIEDuplicateTransient` 在复制时跳过；`1135–1149` `ShouldPort`（文本粘贴）同理 ⇒ 两条复制路（`EditorActor.cpp:616` `DuplicateActors` 二进制、`267` `edactPasteSelected` 文本）都原样复制两个 GUID。
  - 引擎先例：`Runtime/Engine/Private/Actor.cpp:1015–1033` `AActor::Serialize`：`(PortFlags & (PPF_Duplicate|PPF_DuplicateForPIE)) == PPF_Duplicate` 时 `ActorGuid = FGuid::NewGuid()`，PIE 复制则保留 —— 正是既有文档 13 提议的语义。
  - 复制标记的具体链：粘贴 / Ctrl+D 后引擎调 `PostEditMove(true)`（`EditorActor.cpp:444`）→ `CSHouseHandleActor.cpp:89–133`：新副本 `bHasBeenPlaced == false` ⇒ 降级成 `HandleDrag(false)` → `OnHandleDrag` 射线解析 → `RegisterFeatureMarker(MarkerId, …, this)`（`CSHouseActor.cpp:434–447`）命中**原标记那一格**，把 `Marker` 反引换成副本 ⇒ 原标记从此不再被 `NotifyMarkersRebuilt` 回推裁决与吸附；删掉副本 → `UnregisterFeatureMarker` 把这一格连同原标记的洞一起删掉。
- 触发与症状：Alt 拖复制一扇窗：只出一个洞、两个框；删副本，原窗的洞消失、框还在。复制房子：`Tracked.FindOrAdd`（`CSHouseSubsystem.cpp:35–47`）覆盖登记、`UnregisterHouse`（`49–55`）互删（既有文档 13 已述）。
- 建议：两个字段加 `NonPIEDuplicateTransient`（PIE 走 `PPF_DuplicateForPIE` 仍复制），房子沿用「无效才生成」，标记把生成从构造函数挪到 `PostRegisterAllComponents`（`RF_WasLoaded` 与否都要判无效再生成）。

### C6. 笔刷落笔的撤销只撤一半：引擎把 spawn 记成 DeadToAlive，撤销只标 garbage、不走 `Destroyed` / `EndPlay`（严重度：中；已核实）

- 证据：
  - `CSWindowBrushEdMode.cpp:118–127` `CommitSamples`：`FScopedTransaction` 包住 `PlaceMarkerAlongRay`；`CSHouseSubsystem.cpp:213–216` `SpawnActor`；`CSHouseFeatureMarker.cpp:199–222` `AdoptAnchor` 里 `Modify()`。
  - 引擎：`UObjectGlobals.cpp:4974–4987`：事务中构造的 `RF_Transactional` 对象「`MarkAsGarbage(); SaveToTransactionBuffer(); ClearGarbage();`」⇒ 记录的初态是「死」；`LevelActor.cpp:734–737` `SpawnActor` 里 `if (GUndo) ModifyLevel(LevelToSpawnIn)`；撤销时 `EditorTransaction.cpp:871–876` 按 `DeadToAlive` 对它 `MarkAsGarbage()`，关卡 `Actors` 数组随记录恢复；`Level.cpp:2756–2766` `ULevel::PostEditUndo` 只把**非 garbage** 的 actor 补回数组。整条链上没有 `UWorld::DestroyActor`（`LevelActor.cpp:838–925`），所以 `Destroyed()` 与 `EndPlay` 都不来。
  - 插件：`OnDetachFromHost`（`CSHouseFeatureMarker.cpp:119–132`）只挂在 `Destroyed` / `EndPlay`（`CSHouseHandleActor.cpp:75–87`）⇒ `UnregisterFeatureMarker` 不会被调。更糟的是被标成 garbage 的标记仍在 `ChangedObjects` 里，`PostEditUndo`（`175–197`）照跑：恢复到构造时态的 `Anchor` 无效、`Host` 空 ⇒ 落到 `HandleDrag(false)` ⇒ 用 spawn 时的变换（`FTransform(RayOrigin)`，墙外 50 cm、朝向 +X）再射线 / 就近解析 ⇒ **一个 garbage actor 重新登记进房子**并 `AttachToActor`。`NotifyMarkersRebuilt`（`CSHouseActor.cpp:1072–1079`）在那一轮**末尾**才按 `IsStale()` 清掉它，而洞已按含它的表切好。
- 触发与症状：点一扇窗，Ctrl+Z。窗框消失（actor 成 garbage、下一次 GC 回收），墙上的洞留到下一次任何原因的重求值才合拢。Ctrl+Y 后正常（`ClearGarbage` + 恢复终态 + `PostEditUndo` 重登记）。
- 建议：房子侧不要只靠标记的销毁钩子：`NotifyMarkersRebuilt` 的 stale 清理挪到 `BuildWindowOpenings` 之前（或 `ReevaluateSite` 开头）；标记 `PostEditUndo` 在 `!IsValid(this)` 时先 `DetachFromHost()` 再返回。

### C7. 地面订阅以 `FDelegateHandle` 是否有效为准、不以地面身份为准：地面删除重建后房子永久失联（严重度：中；已核实）

- 证据：`CSHouseActor.cpp:244–253` `ResolveGroundAndSubscribe`：`if (!Ground && GetWorld()) {找第一个}`、`if (GroundChangedHandle.IsValid()) return;`。`Ground` 是强引用 `TObjectPtr`（`.h:1297`），被删的地面在 GC 之前非空 ⇒ `!Ground` 为假、继续拿它 `SampleHeight`（`271–299`）；GC 后引用被清空 ⇒ 下一次重求值找到新地面并赋给 `Ground`，但 `GroundChangedHandle` 仍"有效"（它属于已死的委托）⇒ **不再订阅**。`ACSGroundActor::Destroyed`（`CSGroundActor.cpp:553–578`）不广播、不通知订阅者。`UnsubscribeGround`（`255–259`）只在房子 `EndPlay` / `BeginDestroy` 调。
- 触发与症状：删掉地面，新建一块地面（或从别处复制一块）。房子落座到新地面（重求值时 `SampleHeight` 走新地面），但此后画路、拖塑形物都不再唤醒这栋房子（只有 0.25 s 快扫比对房子自身变换，`CSHouseSubsystem.cpp:147–160`），直到重开关卡。撤销删除（同一对象 `ClearGarbage` 回来）不受影响 —— 委托是 C++ 成员，从未被销毁。
- 建议：订阅状态记「订阅的是哪块地面」（`TWeakObjectPtr<ACSGroundActor> SubscribedGround`），`ResolveGroundAndSubscribe` 比较 `SubscribedGround != Ground` 时换订；`Ground` 判有效用 `IsValid(Ground)` 而不是非空。

### C8. 编辑器 world 的卸载路径只来 `BeginDestroy`：「两处解绑」实际需要三处；GPU 缓冲释放纪律在房 / 地面、三条路径之间不对称（严重度：中；已核实 + 部分推测）

- 证据（引擎）：
  - `Runtime/Engine/Private/Actor.cpp:3194–3230` `RouteEndPlay` 整体被 `bActorInitialized` 闸住；`4403–4480` `PostActorConstruction` 只在 `World->AreActorsInitialized()` 时调 `PostInitializeComponents`（置位处 `6544–6550`）⇒ 编辑器 world 的 actor 永远 `bActorInitialized == false` ⇒ `EndPlay` 永不来（证实代码注释）。`Level.cpp:3910–3918` 流送卸载的 `RouteEndPlay(RemovedFromWorld)` 同样被闸住。
  - 编辑器换图：`EditorEngine.cpp:1599 / 1610 / 4416` `CleanupWorld` → `World.cpp:2680–2723` `DestroyWorld → MarkObjectsPendingKill → MarkAsGarbage` → GC ⇒ 只有 `BeginDestroy`。`World.cpp:6374` 子系统 `Deinitialize` 在 `CleanupWorldInternal` 里、GC 之前。
  - PIE 结束才是 `UWorld::EndPlay`（`World.cpp:6080–6097`）→ `RouteEndPlay`。
- 插件侧后果：
  - 三条销毁路径的覆盖：房子 `Destroyed`（`4474–4481`）只退拉尺寸模式；`EndPlay`（`4483–4493`）退模式 + 退订 + 注销；`BeginDestroy`（`4495–4499`）只退订。地面 `EndPlay`（`544–551`）与 `Destroyed`（`553–578`）都把 pooled buffer 交回渲染线程释放，`BeginDestroy` 没有；房子所有 `*GpuBuffers`（`.h:2166, 2253, 2306, 2340, 2388`）**三条路都不释放**（`grep ReleaseOnRenderThread` 只在 Ensure*/Rebuild* 路径命中：`1615, 1966, 2838, 3325`）。地面 `EndPlay` 的注释自己写着「在游戏线程上直接丢引用会把在途帧正在读的 buffer 抽走」—— 房子的缓冲在编辑器删房 / 换图时正是这么死的（**推测**：取决于 `FPaletteBuffers` 的析构语义，未读该类型）。
  - 编辑器模块：`FCSHouseResizeSelectionWatcher::ActiveHouses` 只在 `UpdateSelectionBinding`（`CSHouseResizeSelectionWatcher.cpp:63–70`）惰性清理失效弱引用 ⇒ 处于拉尺寸模式时换图，`USelection` 绑定保持到下一次任何房子进 / 退模式；`EvaluateSelection` 对死弱引用空转（无害，但「集合空 ⇒ 解绑」的不变量被打破）。EdMode：`FEditorModeTools::OnWorldCleanup`（`EditorModeManager.cpp:772–779`）只退出**待停用**的模式，`MapChangeNotify`（`1242`）只通知 ⇒ 窗笔刷 / 地面画笔模式跨换图仍活着，目标弱引用变空后各回调早退（**推测**：未核实 legacy `FEdMode` 在换图时是否另有停用路径）。
- 建议：把「解绑」收敛成一个幂等的 `Teardown()`，在 `Destroyed` / `EndPlay` / `BeginDestroy` 三处都调（`BeginDestroy` 里对已 garbage 的对方跳过）；房子的 GPU 缓冲释放照地面那样进 `Teardown()`。

### C9. `PostEditChangeProperty` 分不清「用户改属性」与引擎的空 `PostEditChange()`：撤销路径上标记重新射线覆盖了事务恢复的锚点（严重度：中；已核实）

- 证据：引擎撤销链 `Obj.cpp:847–853` → `PostEditChange()` → `PostEditChangeProperty(FPropertyChangedEvent(nullptr))`。标记 `CSHouseFeatureMarker.cpp:238–250`：无条件 `RefreshPieceLayout(); HandleDrag(false);`；`OnHandleDrag`（`329–410`）从**当前变换**射线解析、`Modify()`、`Anchor = CSHouse_MakeWallAnchor(Hit, …)`。这正是类头（`CSHouseFeatureMarker.h:36–46`）自己列为禁令的「房子变了 → 重新射线 = 让洞去追标记」。随后 `PostEditUndo`（`175–197`）再用被覆盖的锚点 `RegisterAnchor` + `SnapToAnchor`。
- 触发与症状：任何含标记的撤销 / 重做。多数情况下变换与锚点同时恢复，射线结果≈恢复的锚点，差异只有量化与 `WallStandoff` 引入的漂移；但当变换记录与锚点记录来自不同时刻（C1 的拉尺寸撤销：变换未记录、锚点记录了）就会把错误固化 —— 与 C1、C3 叠加。地面同类：空属性事件落进 else 分支跑四条链（C4）。
- 建议：三个 `PostEditChangeProperty` 覆写开头加 `if (PropertyChangedEvent.Property == nullptr) return;`（撤销由各自 `PostEditUndo` 负责；`PostEditChange()` 的其它调用方也不该被当成用户编辑）。

### C10. 画笔括号与脏标记：只有 `EndPaintStroke` 标脏，EdMode 之外的调用保存不到；`PendingPaintDabs` 无收尾者（严重度：低；已核实）

- 证据：`CSGroundActor.cpp:310–346` `ApplyPaintStroke`：`if (!bPaintStrokeOpen) BeginPaintStroke();` 自开括号、写镜像、排队、广播，**不标脏**；`239–249` `EndPaintStroke` 才 `FlushPaintToGpu(true)` + `MarkPackageDirty()`。EdMode 三处收尾（`CSGroundPaintEdMode.cpp:9–14 SetTargetActor`、`44–48 CommitSamples`、`50–55 ClearBrushTarget`）都调 `EndPaintStroke`，回归脚本也成对调（`TinyGladeDemoRegression.py:71–78, 423–432`）；但 `ApplyPaintStroke` 是 `BlueprintCallable`，蓝图 / 其它脚本只调它就得到「镜像改了、包不脏、保存被跳过」。actor 销毁 / 换图时 `PendingPaintDabs`（`.h:1653`）随 actor 消失（无害）；PIE 进入时画笔模式是否被停用未核实（**推测**）。
- 建议：`ApplyPaintStroke` 里对首笔 `MarkPackageDirty()`（或 `Modify(false)`），括号只管 flush。

### C11. 静态多播委托是进程级：PIE world 的房子会把 PIE actor 塞进编辑器的失选监听（严重度：低；已核实机制 / 症状推测）

- 证据：`CSHouseActor.h:1388 / 1532`、`CSGroundActor.h:217` 三个 `static` 委托；`EnterResizeMode` 是 `BlueprintCallable`（`.h:1391–1409`），PIE 里可调；`CSHouseResizeSelectionWatcher.cpp:47–55` `HandleResizeModeChanged` 不判 world 类型就 `ActiveHouses.Add(House)` 并绑 `USelection`；`EvaluateSelection`（`90–115`）拿**编辑器**选中集判 PIE 房子 ⇒ 下一次点选即对 PIE 房子 `ExitResizeMode`。模块生命周期本身对称：`PCGEditorProcess.cpp:156–166 AddRaw/Start`、`189–199 Stop/RemoveAll`；`Stop()` 清定时器前判 `GEditor`（`Watcher.cpp:26–29`）。Live Coding 不卸载模块、静态委托与 `this` 都不重建（**推测**）。
- 建议：`HandleResizeModeChanged` / `StartWindowBrush` / `StartGroundPaint` 入口处 `if (!World || !World->IsEditorWorld()) return;`。

### C12. 加载顺序不保证：三条登记链各有自登记，但「先来的一方」在对方未就绪时就跑了派生链（严重度：低；已核实 + 部分推测）

- 证据：`Level.cpp:1936–1939` `IncrementalRegisterComponents` 按 `Actors` 数组序调 `RegisterAllComponents` ⇒ `PostRegisterAllComponents` 顺序 = 存盘序。
  - 标记先于房子：标记 `PostRegisterAllComponents`（`.cpp:75–117`，`RF_WasLoaded` 放行）→ `RegisterAnchor(*Attached)` → 房子 `RegisterFeatureMarker` → `ReevaluateSite`（`966`：只判 `IsTemplate()||!GetWorld()`）⇒ 房子在**自己的组件注册之前**被全量重求值（`HouseId` 尚未生成、未入花名册、`TinyGladeMeshComponent` 未注册；`EnsureFrameComponent` 等对新组件直接 `RegisterComponent()`，`1957–1959`）。随后房子自己的 `PostRegisterAllComponents` 再来一次（哈希短路）。能工作靠的是 `RegisterComponent` 对「宿主尚未注册」的宽容（**推测**）。
  - 塑形物先于地面：`CSGroundShaperActor.cpp:157–162` → `RebuildTerrain` → `Ground->RegisterShaper`（`CSGroundActor.cpp:587–599`）→ `RebuildHeightsFromShapers` + `RebuildSkirtDecor`，此时地面 `TinyGladeMesh` 为空（Transient，`RebuildGroundMesh` 才建）；`RefreshHeightsInRegion` 的 `!bChanged` 分支仍跑石阶 / 岩壳（`CSGroundActor.cpp:640–700` 区段）。各 Ensure* 有空指针早退，所以只是空转。
  - 房子先于地面：`ResolveGroundAndSubscribe` 用 `TActorIterator` 找到未注册的地面并读其已反序列化的 `Mirror` ⇒ 正确；随后地面 `RebuildGroundMesh` 广播再唤醒一次。
- 建议：`ReevaluateSite` 开头加 `if (!HasActorRegisteredAllComponents()) return;`，让「先来的一方」把工作留给自己的 `PostRegisterAllComponents`；标记侧 `RegisterAnchor` 只登记不触发重求值（房子注册时会自己算）。

### C13. 纪律③（spawn 后假松手降级）在拖放入口退役后成了反向 bug：笔刷生成的标记第一次真松手被降级（严重度：中；已核实逻辑 / 症状推测）

- 证据：`CSHouseHandleActor.cpp:89–133` `PostEditMove`：`bSynthetic = bFinished && !bHasBeenPlaced; bHasBeenPlaced = true; HandleDrag(bSynthetic ? false : bFinished);`。`bHasBeenPlaced` 只在这里置位。笔刷生成走 `CSHouseSubsystem.cpp:213–216` 直接 `World->SpawnActor`，**不经过** `UEditorEngine::AddActor`（`EditorEngine.cpp:5144–5156` 才有那次合成 `PostEditMove(true)`）⇒ 标记出生后 `bHasBeenPlaced == false`。用户第一次用 gizmo 拖它并松手：`CSHouseFeatureMarker.cpp:154–173` 先 `SetActorTickEnabled(false)`，再 `Super::PostEditMove(true)` ⇒ 判为 synthetic ⇒ `HandleDrag(false)`：不做最终裁决（`329–410` 的 `bFinal` 分支：不写 `LastAcceptedAnchor`、被拒不回位、不 `SnapToAnchor`、不自毁）。tick 已关，`Tick` 的 `DragIdleSeconds` 兜底（`134–152`）也不会补。
- 触发与症状：笔刷放窗后第一次拖动松手：窗框停在鼠标松开处（不吸附回锚点派生位，可能半嵌在墙里），拖到不可放位置也不弹回；第二次拖动起一切正常。
- 建议：`PlaceMarkerAlongRay` / `AdoptAnchor` 完成时置 `bHasBeenPlaced = true`（提供一个 `MarkPlaced()`），或干脆删除降级逻辑 —— 注释自己承认「能走到这条合成事件的子类已经一个都没有了」。

## 六条路径矩阵

| 路径 | 失同步的状态 | 应负责的钩子 | 现状 | 测试覆盖 |
| --- | --- | --- | --- | --- |
| ① 新建（`SpawnActor` / `AddActor` / Python） | 房子：`HouseId` 在 `PostRegisterAllComponents` 生成 ✓；`AddActor` 的合成 `PostEditMove(true)` 让房子 spawn 即 3 次重求值（PostRegister + OnConstruction + override）。标记：只经 `PlaceMarkerAlongRay`，`bHasBeenPlaced` 永不置位（C13）。抓手：房子 `SpawnActor(RF_Transient)`，attach → `InitializeHandle`（`1191–1224`）✓。塑形物：spawn = 3 次 `RebuildTerrain` | `PostRegisterAllComponents`（自登记）+ `PostEditMove(true)`（`AddActor` 合成） | 各类都自登记 ✓；降级逻辑错位（C13） | 单测经 `World->SpawnActor`（无合成事件）：`House.WindowMarker`、`House.WindowBrushPlacement`、`House.ResizeHandle`、`House.HeightHandle`；回归经 `spawn_actor_from_class` / `spawn_actor_from_object` 生房（`TinyGladeDemoRegression.py:588, 753, 1991`）、`place_marker_along_ray` 生标记（`1785–1907`）。`AddActor` 路径对标记已无入口 |
| ② 存盘后重开（反序列化顺序不保证） | 房子：`DoorRunMemory` / `PierSpanIsPier` 恢复 ✓、`MarkerWindows` 由标记重登记 ✓、`Ground` 强引用恢复 ✓。标记：`Anchor` ✓、`Host` 由 attach 父反推 ✓（`RF_WasLoaded` 放行）。地面：`Mirror` ✓、`MeshBuiltAtLocation` 由 `RebuildGroundMesh` 重设 ✓。抓手：不存盘（`Level.cpp:706` 过滤 `RF_Transient`），`ResizeHandles` Transient 为空 ⇒ 模式静默消失，`OnResizeModeChanged(false)` 不广播（监听器惰性清理）。顺序：标记先于房子 / 塑形物先于地面时先来者在对方未就绪时跑派生链（C12）；加载期探针裁决对空 `CurrentOpenings` 判 —— 既有文档 11 的一个生命周期实例（**推测**） | `PostRegisterAllComponents`（三方都有）；标记侧用它代替 `PostLoad` | 功能上收敛；多付 1–2 次重求值 | 回归只 `load_map` 演示关卡（`86, 531, 664, …`），**从不保存本会话放的标记再重开**；单测无。`RF_WasLoaded` 路径是否被任何演示关卡里预存的标记覆盖：未知 |
| ③ 复制粘贴 / Alt 拖 / PIE 复制 | `HouseId` / `MarkerId` 撞键（C5）；复制标记抢走原标记登记格（C5）；Transient 属性与非 UPROPERTY 成员在副本里为默认值（写入器持久化，`DuplicateDataWriter.cpp:40`）⇒ 派生物由 `PostRegisterAllComponents` + `PostEditMove(true)` 重建 ✓；`DoorRunMemory` 等 NonTransactional 照抄 ✓ | `PostDuplicate(EDuplicateMode)` 或 `Serialize` 里按 `PPF_Duplicate` 重掷（引擎 `ActorGuid` 先例） | 缺失 | 无 |
| ④ Undo / Redo | 拉尺寸 / 调高度不进事务，只回滚标记锚点（C1）；细节面板改尺寸撤销后参照系陈旧 ⇒ 窗滑（C3）；每次撤销房子 3 次重求值、地面 2 次上传 + 2 次广播（C4）；落笔撤销留幽灵登记、洞多留一轮（C6）；标记在撤销链上重新射线覆盖锚点（C9）；`Mirror` 不回滚、撤销格数改动会把画重置（校核表）。删房撤销：抓手不回来 ✓（非事务）、标记的 `Host` 弱引用复活 ✓、`MarkerWindows` 因从未注销而"碰巧"完整（**推测**：未核实删父 actor 时引擎是否 detach 子级）。删地面撤销：委托列表是 C++ 成员、未销毁 ⇒ 广播照旧 ✓；`Handed*` 与已释放的 GPU 缓冲错配由 `HasInstanceSourceGPU()` 兜底（`CSGroundActor.cpp:940, 967, 1679, 2171`；房子 `1659, 2025, 2941, 3421, 4044`）✓ | `Modify()` 于权威写入前；`PreEditUndo` 清参照系；`PostEditUndo` 只补事务不认识的登记 | C1/C3/C6/C9 缺失；C4 过量 | **无**：单测与回归都没有事务 / 撤销（grep `undo|transaction` 全空） |
| ⑤ PIE 进入 / 退出 | 抓手被 `ULevel::Serialize` 过滤、`ResizeHandles` Transient 不复制 ✓；两个 world 各一份子系统（`CSHouseSubsystem.cpp:15–21` 放行 Editor + PIE + Game），PIE 房子在 `PostRegisterAllComponents` 登记进 PIE 子系统 ✓；**本会话新放的标记在 PIE 里不登记**（C2）；静态委托跨 world（C11）；PIE 结束走 `UWorld::EndPlay → RouteEndPlay`，各 `EndPlay` 覆写正常解绑 ✓ | 标记 `PostRegisterAllComponents` 的闸门应判状态 | C2 缺失 | **无**（回归不进 PIE，grep `play_in_editor|editor_play` 全空） |
| ⑥ 关卡卸载 / 流送 / GC / 编辑器关闭 | 编辑器 world 只来 `BeginDestroy`（C8）：房子退订 ✓，其余类无 `BeginDestroy`；GPU 缓冲在房子三条路都不释放、地面 `BeginDestroy` 不释放（C8，部分推测）；子系统 `Deinitialize` 在 `CleanupWorld` 里、GC 之前（`World.cpp:6374`）✓；模块 `ShutdownModule` `RemoveAll` / `Stop()` 对称 ✓；EdMode 与失选监听跨换图存活但只持弱引用（C8）；World Partition 编辑器卸载具体路径未核实（**推测**：若走 `DestroyActor` 则等价于删除；若只 GC 则等价于换图） | `BeginDestroy` 应是三处解绑之一 | 只有房子做了一半 | 无 |

## 钩子对称性矩阵

触发次数按「一次用户操作对该 actor」计；括号为引擎依据。

| 钩子 | 编辑器 world | PIE | 打包运行时 | 蓝图子类的差异 | 插件覆写 |
| --- | --- | --- | --- | --- | --- |
| `OnConstruction` | spawn 1×；`PostEditChangeProperty` 1×（`ActorEditor.cpp:227/242` → `ActorConstruction.cpp:1005`）；`PostEditMove(true)` 1×（`ActorEditor.cpp:274–296`）；撤销 / 重做 1×（经 `Obj.cpp:851` → `PostEditChangeProperty`）；粘贴 / 复制 1×（`EditorActor.cpp:444`）。编辑器加载是否另跑一次取决于 `UpdateLevelComponents(bRerunConstructionScripts)`（**未核实**） | 复制世界不重跑（**推测**） | 0×（**推测**） | `bRunConstructionScriptOnDrag` 为真时**每个拖动帧** 1×（`ActorEditor.cpp:274`） | 房子 → `ReevaluateSite`（`4456`）；塑形物 → `RebuildTerrain`（`151`）；地面 / 标记 / 抓手不覆写 |
| `PostRegisterAllComponents` | spawn 1×（`SpawnActor` 内）；加载 1×（`Level.cpp:1936–1939`）；`PostEditChangeProperty` 1×（`ActorEditor.cpp:231/236/243`）；撤销 1×（同链）；`PostEditMove(true)` 0×（已注册的 actor 不再登记，`ActorConstruction.cpp:905–908`） | 世界初始化 1× | 加载 1× | 同 | 房子（`4462`）、地面（`536`）、塑形物（`157`）、标记（`75`，`RF_WasLoaded` 闸）；抓手刻意不覆写（`CSHouseResizeHandleActor.h:114–117`） |
| `PostLoad` | 1× | 复制世界 `bSkipPostLoad`（**推测**） | 1× | 同 | 无覆写；标记用 `PostRegisterAllComponents` + `RF_WasLoaded` 代替 |
| `PostEditMove(false)` | gizmo 每帧（`ActorElementEditorViewportInteractionCustomization.cpp:193`） | 0× | 0× | 同 | 房子（增量摆位）、地面（`TranslateMesh` + 广播）、塑形物（`RebuildTerrain`）、标记（开 tick）、抓手基类（`HandleDrag(false)`） |
| `PostEditMove(true)` | gizmo 松手 1×（`…Customization.cpp:237`）；`AddActor` 后合成 1×（`EditorEngine.cpp:5156`）；粘贴 / 复制 1×（`EditorActor.cpp:444`）；撤销 **0×**（`ActorEditor.cpp:803–822` 无此调用） | 0× | 0× | 同 | 房子（`bForceFullRebuild` + 重求值 + 归位抓手）、地面（全量重建）、塑形物、标记 / 抓手（降级判定，C13） |
| `PostEditChangeProperty` | 细节面板 1×（属性非空）；Python `set_editor_property` 1×；撤销 / 重做 1×（**属性为空**）；`PostEditChange()` 的任何调用方 1×（属性为空） | 0× | 0× | 同 | 房子只重绑材质（`4502–4516`，其余交给引擎链的两次重求值）；地面按属性名分流，**空属性落进 else 四链**（`2388–2411`）；塑形物无条件 `RebuildTerrain`；标记 `RefreshPieceLayout + HandleDrag(false)`（C9） |
| `PostEditUndo` | 每个被记录对象 1×，顺序 = 首次 `Modify()` 逆序、组件优先（`EditorTransaction.cpp:816–951`） | 0× | 0× | 有 `FActorTransactionAnnotation` 时走 `PostEditUndo(注解)` 分支（`ActorEditor.cpp:825–833`），仍重跑构造脚本 | 房子（`4532`）、地面（`2413`）、标记（`175`）；塑形物 / 抓手不覆写（塑形物靠空属性事件的 `RebuildTerrain` 收敛） |
| `Destroyed` | `DestroyActor` 1×（`LevelActor.cpp:925`）；换图 / GC / 撤销 spawn / 流送卸载 **0×** | `DestroyActor` 1× | 同 PIE | 同 | 房子（退模式）、地面（释放 GPU）、塑形物（注销 + 刷地面）、抓手基类（`DetachFromHost`） |
| `EndPlay` | **0×**（`Actor.cpp:3196` `bActorInitialized` 恒假） | `DestroyActor` 1×（经 `Destroyed → RouteEndPlay`）；PIE 结束 1×（`World.cpp:6094–6097`） | 同 PIE + 关卡移除（`Level.cpp:3918`） | 同 | 房子（退模式 + 退订 + 注销）、地面（释放 GPU）、塑形物（注销、不刷地面）、抓手基类 |
| `BeginDestroy` | 换图 / GC / 删除后的 GC 各 1× | 1× | 1× | 同 | 只有房子（退订，`4495`） |

对称性结论：
- 「`Destroyed` vs `EndPlay`」两处都做的只有抓手基类与地面（地面两处释放 GPU、但语义不同的部分——退订、注销——房子放在 `EndPlay`，编辑器里靠子系统 `Tick` 的弱引用清理与 `BeginDestroy` 补偿）。塑形物 `Destroyed` 刷地面而 `EndPlay` 不刷（PIE 结束时地面同时消亡，合理）。
- 「`OnConstruction` vs `PostRegisterAllComponents`」：房子两处都 `ReevaluateSite`，所以引擎每一次「重跑构造脚本 + 再登记」都是两次；塑形物同型（两处都 `RebuildTerrain`）；地面只挂 `PostRegisterAllComponents`，所以细节面板改地面属性 = `PostEditChangeProperty` 分流 1 次 + `PostRegisterAllComponents` 全量重建 1 次 —— 与既有文档 6 的「两次」结论对地面也成立，只是第一次不是全量。
- 「`PostEditMove` vs `PostEditChangeProperty` vs `PostEditUndo`」：撤销**不经过** `PostEditMove` 但**经过** `PostEditChangeProperty`（空属性）—— 三个类的 `PostEditUndo` 覆写注释都只写了前半句（`CSHouseActor.cpp:4534–4536`、`CSGroundActor.cpp:2416–2420`），后半句没人处理（C4 / C9）。

## 既有文档结论校核

对象：`Plugins/PCGPlugins/Docs/TinyGlade/Review_Structure.md`。「中等问题 6」里「Unregister / Reregister 让实例组件丢掉实例源」那半句已由另一名审查员判定有误，此处不再核。

| 条目 | 结论 | 证据 |
| --- | --- | --- |
| 中等问题 5「变换契约靠约定不靠代码」 | **正确** | `CSHouseActor.cpp:892–895` `GetBuildTransform` 只取 yaw + 位置；`4518–4530` `PostEditMove`、`4502–4516` `PostEditChangeProperty` 不钳任何分量；`1990–2018` `EnsureFrameComponent` 注释自认「只有在房子没有 pitch/roll/缩放时才重合」。生命周期补充：撤销 / 粘贴同样能把 pitch / roll / 缩放带回来，没有任何钩子把它们钉回去 |
| 中等问题 6「每次属性改动 = 两次全量重求值」（只核这半） | **正确，且撤销是三次** | 引擎 `ActorEditor.cpp:152–243`（`UnregisterAllComponents → RerunConstructionScripts → ReregisterAllComponents / PostRegisterAllComponents`）+ `ActorConstruction.cpp:252–300`（原生类放行）+ `1005`（`OnConstruction`）；插件 `4456–4472`。「gizmo 松手帧同样两次」：`ActorEditor.cpp:274–296` + 插件 `4518–4530` ✓。「抓手每个拖动事件也是两次」：`PushEdge` `1128–1137`（`MarkHouseDirty` + `ReevaluateSite`）+ `CSHouseSubsystem.cpp:135–178`（下一 tick 再 `ReevaluateSite`）✓。「蓝图子类拖动中每帧还多一次 `OnConstruction`」：`ActorEditor.cpp:274` `Blueprint->bRunConstructionScriptOnDrag` ✓（该标志的默认值未读，**推测**为真）。扩展：撤销 = `OnConstruction` + `PostRegisterAllComponents` + `PostEditUndo` 三次（C4）；塑形物细节面板改属性 = `PostEditChangeProperty` + `OnConstruction` + `PostRegisterAllComponents` 三次 `RebuildTerrain`；地面 = 分流一次 + `PostRegisterAllComponents` 全量一次；标记 = `PostEditChangeProperty` 的 `HandleDrag(false)` 一次登记 + 引擎链两次房子重求值 |
| 深挖 D 第 9 条「唤醒协议：四类客户端、三种策略、十一个同步入口」 | **正确** | 十一个入口逐一对上：`261–265`（`HandleGroundChanged`→264）、`427–459`→458、`461–466`→465、`1110–1139`→1137、`1141–1169`→1161、`1096–1107`→1103、`4456`→4459、`4462`→4470、`4518`→4525、`4532`→4539、`CSHouseSubsystem.cpp:135–178`→172。「合并队列只有抓手在用」：`MarkHouseDirty` 的调用点只有 `PushEdge` / `PushHeight` ✓。「拖动期 `Window == Demand` 永远不成立」：`RegisterFeatureMarker` `444` 比较含 `CenterS` 的整个结构，`MakeDemand` 从锚点现算 ✓。生命周期补充：撤销 / 重做是第十二类入口，而且经由**每个**被记录对象（标记、地面、塑形物）的 `PostEditUndo` 扇出到房子 |
| 深挖 D 第 11 条「探针裁决盖掉了最终裁决」 | **正确（机制）** | `CSHouseFeatureMarker.cpp:301–327` `RegisterAnchor`：`QueryFeatureReject` → `RegisterFeatureMarker`（内部同步 `ReevaluateSite` → `NotifyMarkersRebuilt` → `ApplyHostVerdict(最终)`）→ `ApplyHostVerdict(探针)` 覆盖 ✓。生命周期补充（**推测**）：加载路径 `PostRegisterAllComponents` → `RegisterAnchor` 在房子自己重求值之前跑，探针对着空的 `CurrentOpenings`（Transient）判 ⇒ 被门拱挤掉的窗在重开关卡后先显示框体、洞不出，直到下一次重求值 |
| 深挖 D 第 12 条「同一个事实存两到三份」表格 | **正确** | 每一行都对上：标记 `Anchor`（`.h:346`，`.cpp:204/386 Modify`）；房子 `FCSMarkerWindow::Anchor`（`.h:1925–1946`，写于 `CSHouseActor.cpp:438–439, 451`、`334`）；`Window.CenterS` 参与 `444` 的相等比较；`Host`（`CSHouseHandleActor.h:150` Transient）；attach 父反推（`CSHouseFeatureMarker.cpp:94–104`）；`MarkerRef*` 非 UPROPERTY（`.h:1957–1960`） |
| 深挖 D 第 12 条「撤销后房子与标记 `PostEditUndo` 先后不定」 | **有误**（顺序是确定的；预测的症状成立但成因不同） | `EditorTransaction.cpp:816–820, 848–878, 886–899, 903–908, 935–951` + `Transactor.h:304 Inc = -1` + `701–720 SaveObject` 去重：撤销时 `PostEditUndo` 顺序 = 首次 `Modify()` 的逆序（组件先于 actor），且所有记录先全部 `Restore` 再逐个通知。细节面板改尺寸：房子先记、标记后记 ⇒ 标记先 `PostEditUndo`；症状「窗沿墙滑」的成因是 `MarkerRef*` 不回滚（C3）。gizmo 拉尺寸：房子根本不在事务里（C1），只有标记 `PostEditUndo` |
| 深挖 D 第 13 条「身份 GUID 随 actor 复制而撞键」 | **正确**，并有引擎先例 | `Property.cpp:1039–1060 / 1135–1149`：两条复制路都抄 GUID；`Actor.cpp:1015–1033`：引擎对 `ActorGuid` 在 `PPF_Duplicate` 时 `NewGuid()`、PIE 保留 —— 与文档提议的 `NonPIEDuplicateTransient` 语义一致。补充同类：复制标记会抢走原标记登记格（C5）。文档「修法」可行：`NonPIEDuplicateTransient` 在文本粘贴路也生效（`ShouldPort` 对 `!(PPF_DuplicateForPIE)` 跳过） |
| 已知但不算结构问题「`DoorRunMemory` / `PierSpanIsPier`…Undo 后 Transient 的 `CurrentOpenings` 会恢复、两张记忆不会，靠 `PostEditUndo` 重算收敛」 | **部分正确** | Transient 进事务：`Property.cpp:1046` 只在 `Ar.IsPersistent()` 时跳过，事务写入器未置持久化（`TransactionCommon.cpp` grep `SetIsPersistent` 无命中）✓；NonTransactional 不进事务：`Property.cpp:1039` SkipFlags ✓。但「收敛」≠「回到撤销前」：`PostEditUndo` 的三次重求值都继承撤销期间写进记忆的状态（例如改宽度时开出的拱、其宽度落在保活区间内的，撤销后仍开着）—— 与文档自己在 `DoorRunMemory` 注释里描述的路径依赖一致，属已接受的设计；补充 `Mirror` 的对应条：撤销一次 `NumCellsX` / `CellSize` 改动，`PostEditUndo` 的 `EnsureMirrorInitialized`（`CSGroundActor.cpp:85–111`）判错配 ⇒ 镜像重置为平地 + 底色、`PaintRevision++` ⇒ 改动之后画的路全丢（改动当时已丢过一次），且不 `MarkPackageDirty` |
| 已知但不算结构问题「编排层没有单测…只靠需要真 RHI 的 Python 回归」 | **正确，且生命周期维度完全空白** | `Scripts/TinyGladeDemoRegression.py`：只有 `load_map`、`spawn_actor_from_class` / `spawn_actor_from_object`、`destroy_actor`、`place_marker_along_ray`；无 `save_current_level`（其它 7 个 `Setup*` 脚本的 `save_current_level` 是搭场景，不是判据）、无事务 / 撤销、无 PIE、无复制。`CSHouseLogicTests.cpp` 的 `House.WindowMarker`（`3847`）/ `WindowBrushPlacement`（`4033`）/ `ResizeHandle`（`4240`，含删房带走抓手）/ `HeightHandle`（`4431`）与 `PCGEditorProcess` 的两条编辑器测试（`ResizeWatcherSelection`、`WindowBrushEdMode` / `ModeActivation` / `Blueprints`）都只走「spawn → 操作 → `DestroyActor`」 |

## 未覆盖项

- 未跑任何用例；所有「症状」都是从已核实的调用链推出的，标为「推测」的条目尤其需要一次实测。
- `FPaletteBuffers` / `FStairBuffers` / `FCoverBuffers` 的析构语义（C8 的「游戏线程丢引用」是否真的发生）未读 `CSShaperSteps.h` / `CSGroundStairs.h` / `CSGroundCover.h`。**已由主文 25 回答**：`FRDGPooledBuffer` 原子引用计数、池子恒持最后一份，游戏线程丢引用不是竞争，风险是显存滞留到 GC。
- `UEditorActorSubsystem::SpawnActorFromClass` 的内部（`EditorActorSubsystem.cpp:536` 转 `InternalActorUtilitiesSubsystemLibrary::SpawnActor`）未读到落位与设朝向的先后；插件注释「先放置、回调之后才 `SetActorLocationAndRotation`」未核实。
- `ULevel::UpdateLevelComponents(bRerunConstructionScripts)` 在编辑器加载时是否重跑原生 actor 的构造脚本（影响加载期重求值次数）未核实。
- PIE 复制世界时是否跳过 `PostLoad` / 构造脚本（`bSkipPostLoad`）未核实。
- World Partition 编辑器侧卸载 actor 的具体调用（`DestroyActor` 还是仅 GC）未在 `Runtime/Engine/Private/WorldPartition` 找到对应路径；`OnWorldCleanup` 之外 legacy `FEdMode` 是否在换图 / PIE 开始时被停用未核实。
- `UBlueprint::bRunConstructionScriptOnDrag` 的默认值未读。**已核实**：默认 `true`（`Blueprint.cpp:361`）。
- `AActor::Modify` 是否连带 `RootComponent->Modify()`、删父 actor 时引擎是否 detach 子级（影响「删房撤销」一行的推测）未核实。**已核实**：`AActor::Modify` 连带 `RootComponent->Modify()`（`Actor.cpp:2211–2215`）；`UWorld::DestroyActor` 对 attach 子级 `DetachAllSceneComponents(FDetachmentTransformRules::KeepWorldTransform)`（`LevelActor.cpp:927–945`），该规则 `bCallModify = true`（`EngineTypes.cpp:22`），`DetachFromComponent` 对非 `RF_Transient` 组件 `Modify()`（`SceneComponent.cpp:2669`）⇒ 删房撤销会恢复标记的 attach。
- `FActorTransactionAnnotation::CreateIfRequired` 对纯原生房子返回空还是注解（两条分支都重跑构造脚本，结论不受影响，但组件重登记的细节不同）未细读。
- 校核范围只覆盖任务点名的条目；`Review_Structure.md` 大问题 1–4、深挖 8 / 10 / 14 与冗余代码部分未核。

## 主审查复核记录（2026-09-07）

- 抽查范围与结果：C1–C13 的插件侧引文逐条对上；引擎侧抽查 `SaveToTransactionBuffer`（`UObjectGlobals.cpp:3369–3372`）、gizmo `Modify` 范围（`ActorElementEditorViewportInteractionCustomization.cpp:29–35`）、`FTransaction::Apply`（`EditorTransaction.cpp:848–957`；`Transactor.h:304` `Inc = -1`）、`UObject::PostEditUndo`（`Obj.cpp:847–851`）、`AActor::PostEditChangeProperty` 的重跑门（`ActorEditor.cpp:172`：`bReregisterComponents` 只排除 `ActorLabel`；`ReregisterComponentsWhenModified` `:335–344` 对编辑器 world 恒真）、`StaticDuplicateObjectEx` 标志传播（`UObjectGlobals.cpp:3178`）、PIE 复制参数（`World.cpp:4406–4407`）、`ActorGuid` 重掷（`Actor.cpp:1030–1032`）、`Property.cpp:1039–1058, 1161`、`DuplicateDataWriter.cpp:40`、DeadToAlive（`UObjectGlobals.cpp:4984–4986`）、`ULevel::PostEditUndo`（`Level.cpp:2758–2767`）、`RouteEndPlay` 闸（`Actor.cpp:3196, 6550`）、`AddActor`（`EditorEngine.cpp:5143–5156`）、粘贴（`EditorActor.cpp:444`），全部成立。
- 行号漂移：复核当天 `CSGroundActor.cpp` 又改了地被一族，`CoverInputHash` 之后整体约 +74 行——地面 `PostEditChangeProperty` 今 `2462–2485`（else 分支 `2484`）、`PostEditUndo` `2487–2496`；塑形物四个钩子今 `157 / 163 / 184 / 190`；子系统 `SpawnActor` 今 `219–221`；房子 `ReleaseOnRenderThread` 命中 5 处（多出摆件 `3961`）。结论不变。
- 推测升级为已核实：`UBlueprint::bRunConstructionScriptOnDrag` 默认 `true`（`Blueprint.cpp:361`）；`AActor::Modify` 连带 `RootComponent->Modify()`（`Actor.cpp:2211–2215`），所以 C1 的修法一次 `Modify()` 即同时覆盖尺寸与落座位置；`UWorld::DestroyActor` 对 attach 子级 `DetachAllSceneComponents(KeepWorldTransform)`（`LevelActor.cpp:927–945`），`KeepWorldTransform` 的 `bCallModify = true`（`EngineTypes.cpp:22`），`DetachFromComponent` 对非 `RF_Transient` 组件 `Modify()`（`SceneComponent.cpp:2669`）⇒ 「删房撤销」一行的推测成立，标记的 attach 随撤销恢复；`RF_WasLoaded` 的对象级置位在 `LinkerLoad.cpp:5544–5587`（本文引的 `UObjectGlobals.cpp:1989` 是包级）。
- C8 的「游戏线程直接丢引用会抽走在途 buffer」按主文 25 不成立：`FRDGPooledBuffer` 原子引用计数、池子恒持最后一份、30 帧滞后释放，风险是显存滞留到 GC 而非竞争；C8 的结论（三条路不对称、房子三条路都不释放）不受影响。
- 补一条本文未列的旁证：标记 `PostRegisterAllComponents` 闸门注释给的理由「`AddActor` 先落位后应用旋转」（`CSHouseFeatureMarker.cpp:86–94`）已被 `CSHouseHandleActor.cpp:96–101` 的注释与 `EditorEngine.cpp:5143–5144`（`Rotation` 随 `SpawnActor` 一起传入）否定，C2 的「闸门改判状态」因此没有反证。

## 结论摘要（终稿）

1. **拉尺寸 / 调高度不进事务**（C1，高）：`PushEdge` / `PushHeight` 从不 `Modify()`，抓手无 `RF_Transactional`，gizmo 只 `Modify` 被拖的抓手；Ctrl+Z 只回滚标记锚点，窗沿墙滑而房子不动。
2. **`RF_WasLoaded` 闸门让 PIE 里本会话新放的窗不登记**（C2，高）：PIE 副本沿用源对象的标志，新放的标记没有它，`PostRegisterAllComponents` 早退 ⇒ 无洞、框贴实墙；存盘重开后又正常。
3. **撤销顺序确定、参照系不回滚**（C3，高）：引擎先恢复全部记录再按首次 `Modify` 逆序通知；`MarkerRef*` 非 UPROPERTY，第一次通知时用旧参照系重表达已恢复的锚点 ⇒ 窗滑；缺 `PreEditUndo`。既有文档 12 的「先后不定」有误。
4. **撤销 = 房子 3 次全量重求值、地面 2 次上传 + 2 次全场广播**（C4，中）：`UObject::PostEditUndo → PostEditChange → PostEditChangeProperty(空)` 重跑构造脚本 + 再登记，插件 `PostEditUndo` 再加一次；既有文档 6 的「两次」正确并扩展。
5. **复制沿用 GUID、复制标记抢走原登记格**（C5，中）：引擎对 `ActorGuid` 在 `PPF_Duplicate` 重掷是现成先例。
6. **笔刷落笔撤销只撤一半**（C6，中）：spawn 记为 DeadToAlive，撤销只标 garbage、不走 `Destroyed`/`EndPlay`，标记的 `PostEditUndo` 还会以 garbage 身份重新登记；洞多留一轮。
7. **地面订阅以句柄有效性代替地面身份**（C7，中）：删地面再建，房子永久失联；`Ground` 强引用在 GC 前被当作有效。
8. **编辑器 world 的卸载只来 `BeginDestroy`**（C8，中）：「两处解绑」实际三处；房子 GPU 缓冲三条路都不释放，与地面不对称。
9. 其余：`PostEditChangeProperty` 分不清空事件与用户编辑（C9）；画笔脏标记只在 `EndPaintStroke`（C10）；静态委托跨 world（C11）；加载顺序下先来者提前跑派生链（C12）；纪律③在拖放退役后让笔刷标记第一次松手被降级（C13）。
10. 生命周期六条路径中，测试与回归只覆盖①（spawn）与⑥的「`DestroyActor`」一角；存读、复制、撤销、PIE、卸载均为零覆盖。

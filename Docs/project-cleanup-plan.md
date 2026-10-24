# UETest574_2 项目清理审计记录

[返回文档索引](index.md)

本文件保留早期跨模块审计的候选项编号与风险线索，**不是当前待办清单**。
2026-09-30 清理了旧源码规模、删除收益估算、临时会话 / WIP 清单、已不存在目录的回收指令和父仓库假设。
原审计的行号、调用计数与 Content 引用结论均属于当时快照，不能据此直接删除现有代码。

## 当前核对结果

- 工程根目录当前没有 Git 仓库；`Plugins/PCGPlugins` 是独立仓库。旧的「父子仓库双重跟踪」结论不适用。
- direct / road / vine 已接入 `UCSMesh`，旧场景代理上提和 vine 迁移待办以 [当前说明](gpu-triangle-buffer-unification.md) 为准。
- `ACSLandscapeLayer`、`ACSRuntimeLandscapeLayer` / Manager、`LandscapeLayer.usf` 已删除；临时图层已合并到 `ACSLandscape`，见 [Landscape 框架](CSLandscapeLayer_Framework.md)。
- 旧文档列出的 `VineCPUTail`、`bVisVineGPUUseCPUForPostPasses`、`r.Vine.SC.FusedPath`、`r.Vine.SC.ForceMode` 与 `bUseGPUMode` 在当前插件源码中无匹配；已移除围绕这些旧入口的删除 / 回退决策表。
- `Plugins/Temp`、`Plugins/AssetHandlerPlugin.7z`、`PCGPlugins/Binaries_deadjunction` 均已不存在；不再列为可回收空间。

## 后续代码清理的依据

逐条按符号重查调用点、配置、反射 API 和资产引用。C++ 零调用不等于 Blueprint 零使用；
序列化字段和 CoreRedirects 要结合旧资产核实。工作区已有大量未提交改动，不能把它们当作冗余。
下列保留条目均为**历史候选**；状态和行数需重新测量。

## 一、冗余内容清理（redundant）

### 1.1 CPU→GPU 迁移遗留的死代码（旗舰项，最大收益）

原审计在 CPU→GPU 迁移后列出的候选如下；当前实现经历过拆分和迁移，需要按符号重新确认是否仍存在及是否有消费者。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `gse-1` | `GeometryEditorActor.cpp:1478-1820` | `PrepareVVLinesProjected` + 其专属助手簇（7 个 static 函数） | ~470 | low |
| `gse-3` | `GeometryEditorActor` 头/实现 | 死的 per-point axis 管线（`PointAxes`/`TubeLinePointAxes`，端到端恒为空） | ~30 | medium |
| `gse-7` | `GeometryEditorActor.cpp:5760+` | `constexpr false` 的 SC step-log 基础设施（if-constexpr 岛屿散布在活函数中，需外科式移除） | ~450 | low |
| `gse-8` | `GeometryEditorActor.cpp:1039,5072` | `LogVineSCStageTargetTransformMatch` 无条件热路径调试扫描 O(points×targets) | ~30 | low |
| `gse-25` | `GeometryEditorActor.cpp:7987-8033` | SC prep parity 校验块（默认配置下不可达） | ~40 | low |
| `csg-1` | `ComputeShaderMeshGenerator.cpp` 匿名命名空间 | 死 readback/batch 助手（`AppendTriangleMeshData` 等，surface-voxel 内联后失效） | ~645 | low |
| `hierarchy-21` | `GeometryEditorActor.cpp` | SC step-log / parity-debug 助手套件（与 `gse-7` 重叠，合并处理） | (含于上) | low |

**原审计判为「产出但无消费」的缓存候选**（当前消费者需重查）：

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `csg-3` | `ComputeShaderMeshGenerator` | 脏体素三角缓存子系统：`GenerateVines` 刷新它，vine 路径又显式丢弃其 handle | ~900 | medium |
| `csg-2` | `ComputeShaderMeshGenerator` | generated-data 纹理缓存：每次 readback 产出，无任何消费者 | ~400 | medium |
| `hierarchy-13` | `ACSGenerateCaptureScene` 两兄弟 `Generate()` | 计算后被丢弃的 GPU readback | ~20 | low |

### 1.2 整体功能 / 类的历史候选

已执行删除的六项不再重复列为待办；`ACSShallowWaterVoxelCapture` 曾按用户要求回退保留，不在删除名单中。

| ID | 位置 | 原审计内容 | 原估计行数 | 风险 | 复核状态 |
| --- | --- | --- | --- | --- | --- |
| `mcp-1` | `NetworkDebugRoutes.cpp` | 网络调试追踪器：`RecordRequest`/`CompleteRequest` 零调用，端点恒返回空，`OnTick` 是 no-op | ~120 | low | 待处理 |
| `game-1` | `UETest574_2Character/GameMode/PlayerController` | TopDown 基类三件套，Content 与代码零引用（BP 直接挂引擎类） | ~366 | low |
| `aitool-1..6` | `NKSRGrid.h/.cpp`, `NKSRSvh` | NKSR 参考实现中从未接入推理路径的 API（`SampleBezier`/`SplatTrilinear`/`Coarsened`/`Dual`/`EvaluateVoxelStatus`/`FNKSRLayerField`） | ~230 | low |

### 1.3 死 BlueprintCallable API 与未用 UPROPERTY

> 删除前逐一确认 Content 引用；原审计已经排除被蓝图引用的入口，不能只凭 C++ 调用计数判断。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `csg-11` | `AComputeShaderMeshGenerator` | 无引用的 landscape-triangle BP 包装 API | ~430 | medium |
| `csg-10` | `UComputeShaderBasicFunction` | 死成员与其孤立 shader 类 | ~480 | medium |
| `pcgep-4..9` | `CSAssetProcess` / `CSShallowWaterProcess` | 死 BP 包装、死 `MaterialTest`、死调试导出/dump | ~360 | low |
| `pcgep-10..12` | `ACSLandscapeRiver` / `ACSLandscape` | 中性化的 `SimRiver` no-op、只写成员、死 Debug UPROPERTY | ~50 | mixed（含 high） |
| `mcp-2,3,19` | `AnimBP` / `RealtimeMesh` / `Fab` 路由 | fake-success 桩（返回 success 却什么都不做）、`WITH_REALTIMEMESH` 从未定义、死 store | ~70 | mixed |
| `gse-10..20,26,27` | `GeometryEditorActor` / `GeometryMathUtils` / `VDBExtra` / `LandscapeExtra` | 未用 SC 属性字段、死 3-arg 重载、`UGeneralMath` 空桩库、死 VDB `VDBMeshFromActors` 家族、`CreateLandscapeMeshTextureData` 恒返回空、死 legacy 重载、6 个内容无引用的 BP 函数 | ~800 | mixed |
| `gse-9` | `FVV` 5 个未用序列化 UPROPERTY（`bUseGPUMode` 等） | 需先在 BP 编辑器确认无 Make/Break FVV 引脚并重存 `BP_VineSource` | ~10 | **high** |
| `aitool-9,10,11` | `NKSRKernelEvaluation` / `UNKSRReconstructLibrary` | 无消费者的 gradient 管线、无调用的 `GetAIToolPluginDir`、只写成员 | ~60 | mixed |
| `game-4..12` | `Source/UETest574_2` 各处 | 死配置节、未用 UPROPERTY、只写成员、不存在类的前向声明、自我覆盖的重复调用、7 个类无 Tick 却开着 Tick | ~50 | mixed |
| `hierarchy-11,12,15,19` | 跨 vine/geometry | `EnqueueGDFJob` 零生产者、`FVV::bUseGPUMode` 未读、死/桩调试 UFUNCTION、死顶点缓冲声明 | ~240 | mixed |

### 1.4 死 Shader kernel 与 permutation（减少编译量）

以下是旧派发器与 permutation 的历史候选；Shader 目录映射不等于入口被使用，需重新检查注册、编译条件与派发点。

| ID | 位置 | 内容 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `shaders-3` | `BasicFunction.usf` | `CalculateGradient` 入口编译但从不派发（拖带 `ComputeGradientT` 等） | ~100 | low |
| `shaders-7` | `General.usf` | 死助手簇（`FillSharedGroup`/`Random2D/3D/4D`/`SampleBilinear` 等） | ~190 | low |
| `shaders-15` | `MeshFill.usf` | 15 个 permutation 中 4 个死（3 个空 `#elif` 体） | ~110 | medium |
| `shaders-16` | `MeshFill.usf` | `FMESHFILL_TEST` 调试 kernel + BP `CurvaturePostTest` 调试残留 | ~85 | **high** |
| `shaders-13` | `ShallowWater.usf` | `SW_SmoothHeight` 全程派发却是空 kernel（"reserved" no-op，每帧水面 bake 都在跑） | ~80 | medium |
| `shaders-1,2,4,5,6,10,14,17,18,19,22` | 多个 `.usf` | 死 kernel（`CS_Promote`/`BlurPixelOrig`）、被 Fast 版取代的旧版、死 permutation、死打包助手、死邻域采样等 | ~350 | low |
| `csg-5,6` | `FMeshFillMult` / `FShallowWaterSim` | 死 permutation 模式（`FMeshFillMult` 16 中 5 个从不派发，但 128 个排列全在编译）、未接线的 shader 参数 | ~140 | medium |

### 1.5 注释掉的大段代码

| ID | 位置 | 行数 | 风险 |
| --- | --- | --- | --- |
| `gse-16` | `GeometryGeneral.cpp` 等（含 `return;` 后 141 行死块引用已不存在的 `SelectionMechanic`） | ~500 | low |
| `hierarchy-14` | `GeometryGeneral.cpp` 大段注释块（与 `gse-16` 部分重叠） | ~358 | low |
| `pcgep-13` `csg-18` `hierarchy-17` | `ComputeShaderLandscape.cpp` / 零字节 README / 恒假死分支 | ~90 | low |

## 二、多余的兼容性内容清理（compat）

### 2.1 旧 CPU 兼容路径

原表中的 vine CPU 后处理、非 fused 上传和 CPU 空间殖民开关已无法在当前源码定位，旧行数与删除顺序撤销。
后续若发现新的 fallback，应重新核对其入口与资产契约。

### 2.2 其它兼容分支的历史候选

| ID | 位置 | 内容 | 风险 |
| --- | --- | --- | --- |
| `mcp-16,17` | `AnimBP` 路由 | legacy 别名 op + UE 5.7 改名 workaround shim；硬编码 `WITH_PCG/WITH_GAMEPLAY_ABILITIES/WITH_NIAGARA=1` 使 501 fallback 分支不可达 | low |
| `mcp-3` | `RealtimeMesh` 端点 | `WITH_REALTIMEMESH` 从未定义 | low |
| `game-4,5` | `DefaultGame.ini` / `Build.cs` | 指向不存在属性的死配置节、仅为模板代码存在的依赖与 include 路径 | low |
| `pcgep-15` | `PCGPLUGINS_DEBUG` Build.cs 块 | ✅ 2026-10-24 已删：`GeometryScriptExtraEditor`、`PCGEditorProcess` 两份删掉，只留 `ComputeShaderGenerator` 那份（见 [结构优化方案](Plan_PluginStructure.md) `src-7`） | low |
| `shaders-2` | `Connectivity.usf` | `CPF_Findislands` 被 `CPF_FindislandsFast` 取代 + 44 行注释实验 | low |
| `shaders-17` | `SampleSpline.usf` | `SS_RASTERIZESPLINE` permutation 从不派发且函数体 100% 注释 | low |

## 三、抽取子类公用内容到父类（lift-to-parent）

| ID | 兄弟类 | 共用内容 → 目标父类 | 行数 | 风险 |
| --- | --- | --- | --- | --- |
| `hierarchy-6` | `ACSCliffGenerateCapture` / `ACSFillTarget` | 重复成员、`IsParameterValidMult` 与捕获逻辑 → 共同基类 | ~120 | medium |
| `game-13` | 4 个 `ACharacter` 变体子类 | 近乎相同的 `CharacterMovement` 平面约束设置 → 变体基类 | ~20 | low |
| `gse-28` | `FWindTreeReduceData` / `FWindTreeCombineLeafData` | 验证后实为**死成员清理**而非上提：删基类 `ClassNum` + `FDynamicMeshComponentData::IsValid` + 注释的 `FComponentMergeOptions`，保留 `ParentClass` | ~20 | low |


## 四、跨模块 / 模块内重复 → 提取共享（duplicate）

| ID | 重复内容 | 副本位置 | 处理 | 净减行 |
| --- | --- | --- | --- | --- |
| `xmodule-1` `csg-12` `hierarchy-16` | `GeometryAsync.h` 逐字节重复 | `ComputeShaderGenerator`（死副本）+ `GeometryScriptExtraEditor` | 删死副本，单一来源 | ~71 |
| `xmodule-2` | `PCGPluginDebug.h` 逐字节重复 | 同上 | 删死副本 | ~46 |
| `xmodule-3` `gse-21` `gse-27` | OpenVDB 粒子→level-set→mesh 管线 | `ComputeShaderGenerator` ⟷ `GeometryScriptExtraEditor`（`VDBParticleList` vs `FCSGeneratorVDBParticleList`） | 上提到 `ComputeShaderGenerator`（已是 Public 依赖） | ~150–200 |
| `xmodule-4` | `DynamicMesh→UStaticMesh` 保存管线 | 三个 PCGPlugins 模块各一份 | 提取到共享助手 | ~110 |
| `xmodule-5` | 同步 GPU buffer-readback 样板 | `ComputeShaderGenerator` 内复制 ~9 次 | 提取 `CSHelper` 风格模板 | ~250 |
| `mcp-10,11,12,13,18,20` | graph-by-name 查找 ×6、transform-JSON ×3、actor 查找 ×6、vector→JSON ×10+、capabilities 手抄注册表、resolve-actor+component 样板 | `MCPUnreal` 多文件 | 全部收拢进 `MCPUnrealUtils.h` | ~400 |
| `mcp-4,5,6` | 测试模块把派发表复制为平行副本（已漂移，接受不存在的 op、漏掉真实 op） | `MCPUnrealTests` | 让测试引用生产派发表，删 `ParseJsonBody_BROKEN` | ~300 |
| `gse-22,23,29` | foliage 实例变换助手、GPU 体素上传块 ×3、foliage↔transform 薄包装 | `GeometryEditorActor.cpp` static ⟷ `UFoliageConverter` | 合并为带 `UWorld*` 的重载 | ~40 |
| `shaders-8,9,18,20,21` | bilinear 采样器、2D value-noise、`ClosestPointOnTriangle`、体素空间哈希、`HashUint` | 多个 `.usf` 各自实现，绕过 `PCGMathCommon.ush` | 收拢进 `PCGMathCommon.ush` | ~100 |
| `hierarchy-18` `pcgep-18` | `IsFiniteVector` ×5、"派生关卡文件夹路径"片段 ×7 | 跨文件 | 提取谓词/助手 | ~50 |
| `aitool-7,8` | 三入口 glue（sync BP 库 / async BP 节点 / commandlet），含仅为绕 unity-build ODR 而改名的同一函数对；二次 Bezier 基实现两遍 | `AITool` | 提取共享 glue | ~80 |
| `csg-13,15` | `AGPUSkeletalTree` 死骨架副本、static-mesh render-data resolve + index-SRV 创建 | `ComputeShaderGenerator` | 合并/删死副本 | ~175 |

## 五、仓库级卫生（hygiene · 非代码）

### 5.1 目录与仓库

旧的回收清单与双重跟踪假设已清除，见顶部核对结果。`mcp-unreal/` 仍是独立目录；
是否保留及与 `Plugins/MCPUnreal` 的关系应单独核实，旧的「克隆可直接删」结论不作为操作依据。

### 5.2 配置与资产的历史候选

| ID | 问题 | 动作 |
| --- | --- | --- |
| `hygiene-14` | `T_Smoke005.uasset` 同名两副本（内容不同）：`Content/Effects/Fx/0_Resources/Tex/Smoke/` 与 `Content/MaterialLibrary/Texture/CommonNoise/` | 编辑器内确认引用后合并，走重定向器（uasset 移动需 redirect 处理） |
| `hygiene-11` | `Config/DefaultEditor.ini` `SimpleMap` 指向已删关卡 `/Game/TopDown/Maps/TopDownExampleMap` | 更新为现有关卡 |
| `hygiene-12` | `Config/DefaultEngine.ini` 重复/矛盾条目（`RecastNavMesh` 节重复） | 去重 |
| `critic-3` | `PCGPlugins.uplugin` 强启用 `Landmass` 但代码零引用 | ⚠️ 2026-10-23 复核不成立：`Content/Landscape/` 下 6 个资产引用 Landmass，**保留依赖**（见 [结构优化方案](Plan_PluginStructure.md)） |
| `critic-4` | `PCGPlugins.uplugin` 源码插件却标 `"Installed": true`（应为 false，其他源码插件均 false） | 2026-10-23 复核：当前已是 `false`，无需处理 |

### 5.3 根目录杂物与 Build.cs 依赖的历史候选

| ID | 问题 | 动作 |
| --- | --- | --- |
| `hygiene-6,7,8` | `docs/`（仅生成的 bleve 搜索索引二进制）、`ShaderTest/`（无引用的调试草稿）、`Tools/`（6 个跟踪文件混 14 个草稿脚本/日志） | 分别评估：`docs/`、`ShaderTest/` 移出跟踪；`Tools/` 分离跟踪与草稿 |
| `shaders-25` | `Tools/_vine_diff.txt`（65KB 陈旧 `VVVoxel.usf` scratch diff） | 删除 |
| `critic-2` | `ComputeShaderGenerator.Build.cs`：未用 `Foliage` 依赖 + `CoreUObject`/`Engine` 在 Public 与 Private 重复 | 精简依赖 |
| `pcgep-16` | `AssetHandlerPlugin`：9 个未用模块依赖、1 个未用插件依赖 | 精简（注：`AssetHandlerPlugin` 本身**非死插件**，被 `EUW_AssetDebug` 引用且 `EnabledByDefault=true`） |
| `pcgep-20` `gse-30` | `PCGEditorProcess.Build.cs` `Core` ×3、`Blutility` ×2；`GeometryScriptExtraEditor.Build.cs` 4 个模块各列两遍 | 去重 |
| `mcp-21` | `MCPUnrealTests` 模块每次编辑器启动都 `PostEngineInit` 加载（无 target 限定） | 限定为测试/开发构建加载 |

## 六、继续审计时的顺序

1. 在目标模块当前工作区重新定位符号，确认旧问题是否已经消失或改变形态。
2. 对仍成立的候选记录现有调用者、资产消费者与范围；重新估计收益。
3. 只为确认后的改动选择合适的验证，不复用旧报告的编译 / 测试通过记录。

## 附录 A：负向 / 需保留发现

- `pcgep`：`AssetHandlerPlugin` **非死插件**，与 `PCGEditorProcess` 无功能重叠（视口 clay-material override vs PCG/landscape 工具），被 `EUW_AssetDebug.uasset` 引用且默认启用。
- `aitool`：commandlet 的 `.npy` dump 被 `Plugins/AITool/Tools/compare_golden.py` 消费（golden 测试），非死代码。
- `game`：游戏模块与插件源码**无跨模块重复**（grep 命中均为 UHT 中间产物）。
- `WeldDynamicMesh`、`ColorAttribTransf`、`UVAttribTransf`、`WindDataForTree` 等反射入口曾有 Content 消费者；更名或删除前重新核实资产引用。

## 附录 B：编号说明

原报告给出的 journal 路径含省略号，无法作为可定位证据，已移除。保留编号便于与旧提交或完整审计记录对照。

发现按维度编号：`csg-*`（ComputeShaderGenerator）、`gse-*`（GeometryScriptExtraEditor）、`pcgep-*`（PCGEditorProcess+AssetHandler）、`mcp-*`（MCPUnreal）、`aitool-*`（AITool）、`game-*`（Source）、`shaders-*`（Shaders）、`hierarchy-*`（类继承/上提）、`xmodule-*`（跨模块重复）、`hygiene-*` + `critic-*`（仓库卫生）。

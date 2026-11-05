# PCGPlugins 文档

本目录存放 PCGPlugins 的设计、实现说明与历史审计。插件入口见 [README](../README.md)；Tiny Glade 专题的命名与分类见其独立索引。

**这是唯一的文档清单**：README「设计文档」一节只链接这里；插件根目录的几份计划稿也登记在这里。新增、合并、删除文档时只改这一处。状态写在每条后面的括号里。

## 审查与整理

- [代码冗余与空指针检查审查](Review_CodeRedundancy.md)（2026-10-24 审查 338 条；10-24 修了 P0 前 4 条，11-05 删了死代码 / 转发 / 可删副本 41 条，第八节记录被快照提交带回的脏页三角缓存子系统及其删除、并收了 2026-08 旧审计的遗留候选）
- [PCGPlugins 结构优化方案](Plan_PluginStructure.md)（2026-10-23 盘点：模块分层与反向依赖、目录 / 文档 / 脚本归位、分阶段步骤；阶段 1 已做，搬家类阶段未动）

## GPU 网格与渲染

- [GPU 三角形 Buffer 统一现状](gpu-triangle-buffer-unification.md)（`UCSMesh` 对象层的数据流、流与计数契约；「设计沿革」收了 2026-07 第二轮方案的去向与仍成立的约束）
- [GPU 网格光追与 Lumen surface cache](GpuMeshRayTracing.md)（GPU 常驻网格的光追 BLAS 接入，2026-09-09 落地）
- [Instance 剔除与实例 ID 稳定性优化方案](../InstanceCullingOptimization.md)（插件根；第一至第三阶段已落地、第四阶段做了一项，进度与开关见其 §11）
  - [2026-10-02 性能与正确性基线](InstanceCulling/Baseline_20261002.md)
  - [可见源行号布局：实现、验证与 A/B 结果](InstanceCulling/VisibleSourceIndex_20261002.md)
  - [减少更新、重复写入与每组件固定成本：实现与结果](InstanceCulling/Step3_20261002.md)
- [Nanite 截面 HLOD](../NaniteClusterHLOD_Plan.md)（插件根；`UCSNaniteCutOps` 截面算子 + `UCSNaniteCutHLODBuilder`：2026-09-22 落地，测试 5/5、试验关卡截图验收，未在 WP 关卡实跑）
- [帧配额调度计划](FrameQuotaScheduler_Plan.md)（GPU 生成作业的统一分帧调度；设计基线，未实施）

## 模拟

- [GPU 布料模拟](GpuClothSim_Plan.md)（XPBD 常驻算子逐帧驱动 `UCSMesh` 形变：P0 / P1 已于 2026-10-26 落地，含设计、实测数据与未做的阶段；pass 流程图 [GpuClothSim_PassFlow.svg](GpuClothSim_PassFlow.svg)）
  - [AnimationFallSplit 移植：角色碎成布条](HoudiniFallSplit_Port.md)（Houdini 分块与 Vellum 绳的逐节点整理，带状分块 + 锚点 + 旋流场的 UE 实现与差异）
- [GPU 批量刚体沉降](GpuRigidSettle_Plan.md)（粒子簇形状匹配，布料计划的姊妹篇；计划，未实施）
- 浅水模拟：生命周期图 [shallow-water-lifecycle.svg](shallow-water-lifecycle.svg)、体素地形构建流程图 [ShallowWater_VoxelGridBuild_Flow.svg](ShallowWater_VoxelGridBuild_Flow.svg)（`ACSShallowWaterVoxelCapture` 仍只有骨架）；水面材质的 Custom 节点副本与对比见 [CSSWCustomMaterial](CSSWCustomMaterial/README.md)

## 藤蔓

- [藤蔓 GPU 常驻化：现状与剩余计划](VineGpuResidency.md)（2026-08-14；现状表 2026-09-10 订正过两行）
- [藤蔓生成异步改造方案](../VineAsyncPipeline_Plan.md)（插件根；2026-10-08，待定；流程图 [当前](VineAsync_CurrentFlow.svg) / [目标](VineAsync_TargetFlow.svg)）
- [独立藤蔓运行时 Actor：Nanite 表面生成方案](../VineRuntimeSkill_Plan.md)（插件根；2026-10-08 方案草案，未实现）
- [藤蔓 CPU 版单次成本基准](VineCpuCostBench.cpp)（数据见 README 藤蔓测试场景「CPU 版本耗时预估」）

## 地形、道路与城市

- [Landscape 图层编辑框架](CSLandscapeLayer_Framework.md)（当前图层继承、Merge 与烘焙入口；2026-09-30 按源码清理过）
- [Palmsland 道路关卡](CS2Roads.md)（CS2 Road Lab 场景的静态网格导入）
- [CS2 道路逻辑的 GPU 移植](CS2RoadsGpu.md)（`CSRoadNet` 模块：快照或样条输入，CPU 组合与车道图 + GPU 几何、参数、路口接缝、细分、变形与标线贴花，直写 `UCSMesh`；验证方法与精度结论）
- [道路、建筑与属性场局部更新系统](../LocalUpdateSystem_Plan.md)（插件根；第一版已在 `CSRoadNet` 落地，其余为设计提案；编号示意图 [LocalUpdateSystem_Ids.svg](../LocalUpdateSystem_Ids.svg)）
- [通用地面与房屋生态衔接方案](../GroundJoin_Plan.md)（插件根；2026-11-04 设计提案，未实现）
- [夜之城房屋群落、通用分件与 GPU 立面生成](../NightCity_BuildingKit.md)（插件根；群落与分件调研、`UCSCityFacadeKit` GPU 立面、沿街楼带 `ACSCityStreetActor`（单独摆放或挂路下接管一段路，A–D 群落预设）；2026-11-05 已实现，CSRoadNet 44/44，未提交）
- [夜之城建筑的程序化生成与分块](NightCity_ProceduralBuildings_Partitioning.md)（2026-11-04 MCP 实测：模块复用、建筑语法、空间输出与 World Partition 分块；含证据和实施顺序，尚未实施）
- [夜之城附属物：放置、跨楼互斥与连接](NightCity_Attachments_And_Links.md)（2026-11-05 设计补充：空调与招牌安装语义、实体与净空裁决、复用 Tiny Glade 关系协议；尚未实施）
- [夜之城附属物：模块锚点与生成后 GPU 搜索评估](NightCity_Attachment_Placement_Evaluation.md)（2026-11-05 数据审计：模块复用与变体、模板及 GPU 求解接口、网络输入输出；附统计脚本与结果，求解器尚未实施）
- [可放置区域网格与源 Mesh 的资产关系](NightCity_Placement_Surface_Assets.md)（2026-11-05：Profile 和源默认关联已实现；Kit 覆盖、GPU 输出与自动失效仍为设计）
- [夜之城常用模型：第一批可放置区域](NightCity_Placement_Batch01.md)（2026-11-05 已处理 24 个模型：22 个有区域、2 个待复核；71 个区域 / 352 个三角形，含绑定、预览关卡、重载验证及重建脚本）
- [CSSceneDirty3D：场景级三维 Dirty 区域系统](cs-scene-dirty-3d-design.md)（当前范围与设计见正文）

## 生成器与网格工具

- [VDB 网格：由 Actor 点生成](VDBMeshFromActorPoints.md)、[由表面体素生成](VDBMeshFromSurfaceVoxels.md)（GPU 加速方案，2026-07-23 写成；之后 `*ToDynamicMesh` 系列已改为 `*ToGpuMesh`）
- [Actor 自持的实例笔刷](InstanceBrushPaintDesign.md)（2026-07-23 设计稿）
- [MeshBoolean 焊接流程的失败情况](meshboolean-weld-failure-cases.md)（Houdini 复现截图，构建脚本 [HoudiniMeshBooleanWeldFailureCases.py](../Scripts/HoudiniMeshBooleanWeldFailureCases.py)）
- [Voxel 激活与三角形缓存设计](ComputeShaderMeshGeneratorVoxelTriangleCache.md)（⚠️ 历史：所述子系统 2026-11-05 已删，设计与踩坑记录保留）

## Tiny Glade

- [Tiny Glade 复刻：文档索引](TinyGlade/index.md)（`Plugins/PCGPlugins/Docs/TinyGlade/`：gpumesh 地面 + 顶点色笔刷 + 直推式房屋重求值，v1 不用 CSSceneDirty3D。设计裁决在 [Plan_House.md](TinyGlade/Plan_House.md)，进度在 [模块对照与进度合卷](TinyGlade/Progress_ModuleComparison.md)，编排层结构审查在 [Review_Structure.md](TinyGlade/Review_Structure.md)）

## 非本插件

- [Motion Matching 与动画预测插件移植说明](motion-matching-plugin-porting-guide.md)（讲的是工程里的 GASP 移植，不是本插件的功能）

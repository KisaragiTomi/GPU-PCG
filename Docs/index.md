# PCGPlugins 文档

本目录存放 PCGPlugins 的设计、实现说明与历史审计。插件入口见 [README](../README.md)；Tiny Glade 专题的命名与分类见其独立索引。

## 文档索引

- [项目清理审计记录](project-cleanup-plan.md)（历史候选项；执行前需按当前代码重新核实）
- [PCGPlugins 结构优化方案](Plan_PluginStructure.md)（2026-10-23 盘点：模块分层与反向依赖、目录 / 文档 / 脚本归位、分阶段步骤）
- [代码冗余与空指针检查审查](Review_CodeRedundancy.md)（2026-10-24：338 条，其中约 25 处真实缺陷要先修；多余判空 98 条）
- [GPU 三角形 Buffer 统一现状](gpu-triangle-buffer-unification.md)
- [GPU 三角形统一设计沿革](GpuTriangleUnified_Plan.md)（已被 CSMesh 对象层取代的方案）
- [GPU 网格光追与 Lumen surface cache](GpuMeshRayTracing.md)
- [GPU 布料模拟](GpuClothSim_Plan.md)（XPBD 常驻算子逐帧驱动 `UCSMesh` 形变：P0 / P1 已于 2026-10-26 落地，含设计、实测数据与未做的阶段；pass 流程图 [GpuClothSim_PassFlow.svg](GpuClothSim_PassFlow.svg)）
- [Instance 剔除与实例 ID 稳定性优化方案](../InstanceCullingOptimization.md)（第一至第三阶段已落地、第四阶段做了一项，进度与开关见其 §11）
  - [2026-10-02 性能与正确性基线](InstanceCulling/Baseline_20261002.md)
  - [可见源行号布局：实现、验证与 A/B 结果](InstanceCulling/VisibleSourceIndex_20261002.md)
  - [减少更新、重复写入与每组件固定成本：实现与结果](InstanceCulling/Step3_20261002.md)
- [Landscape 图层编辑框架](CSLandscapeLayer_Framework.md)
- [Palmsland 道路关卡](CS2Roads.md)（CS2 Road Lab 场景的静态网格导入）
- [CS2 道路逻辑的 GPU 移植](CS2RoadsGpu.md)（`CSRoadNet` 模块：快照或样条输入，CPU 组合与车道图 + GPU 几何、参数、路口接缝、细分、变形与标线贴花，直写 `UCSMesh`；验证方法与精度结论）
- [帧配额调度计划](FrameQuotaScheduler_Plan.md)（未实施）
- [Motion Matching 与动画预测插件移植说明](motion-matching-plugin-porting-guide.md)
- [CSSceneDirty3D：场景级三维 Dirty 区域系统](cs-scene-dirty-3d-design.md)（当前范围与设计见正文）
- [MeshBoolean 焊接流程的失败情况](meshboolean-weld-failure-cases.md)（Houdini 复现截图，构建脚本 [HoudiniMeshBooleanWeldFailureCases.py](../Scripts/HoudiniMeshBooleanWeldFailureCases.py)）
- [Tiny Glade 复刻：文档索引](TinyGlade/index.md)（`Plugins/PCGPlugins/Docs/TinyGlade/`：gpumesh 地面 + 顶点色笔刷 + 直推式房屋重求值，v1 不用 CSSceneDirty3D。设计裁决在 [Plan_House.md](TinyGlade/Plan_House.md)，进度在 [模块对照与进度合卷](TinyGlade/Progress_ModuleComparison.md)）

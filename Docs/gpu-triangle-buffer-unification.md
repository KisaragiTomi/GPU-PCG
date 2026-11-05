# GPU 三角形 Buffer 统一现状

[返回文档索引](index.md)

2026-09-30 按当前源码清理。direct mesh、road、vine 均已接入 `UCSMesh` 对象层；早期的「road 缺 readback」「vine 待叶子化」已完成，不再列为待办。

## 当前数据流

生成算子写入 `UCSMesh` 持有的 `FCSMeshResident`；`UCSMeshRenderComponent` 绑定常驻流负责显示。
保存时，`UCSMeshRenderComponent::SaveToStaticMesh` 调用 `UCSMeshOps::CopyToStaticMesh`，通过
`CSMeshReadback::ReadbackResidentSync` 取得 `FCSGpuMeshCPUData`，再构建静态网格。
回读以网格对象为源，不依赖组件是否可见或已注册，也不要求 scene proxy 存活。

- **meshgenerator**：`AComputeShaderMeshGenerator::SaveDirectGPUMeshToStaticMesh` 使用共享保存入口；生产者持有的 `DirectGpuMesh` 可通过 `SourceMeshOverride` 显式传入。
- **road**：`URoadMeshComponent` 派生自 `UCSMeshRenderComponent`，保存走基类入口。
- **vine**：`AVineContainer` 位于 `ComputeShaderGenerator/GeometryEditorActor.{h,cpp}`；`VineGeometry` 常驻 GPU，`SaveStaticmesh()` 通过 `VineGpuMesh->SaveToStaticMesh` 烘焙，保留结果资产命名和挂接流程。

源码入口：[`CSMesh.h`](../Source/ComputeShaderGenerator/Public/CSMesh.h)、
[`CSMesh.cpp`](../Source/ComputeShaderGenerator/Private/CSMesh.cpp)、
[`CSMeshRenderComponent.cpp`](../Source/ComputeShaderGenerator/Private/CSMeshRenderComponent.cpp)、
[`RoadMeshComponent.h`](../Source/PCGEditorProcess/Public/RoadMeshComponent.h)。

## 流与计数契约

- 流仍由 `FCSGpuStreamDesc` 描述；标准渲染流、辅助流及回读语义定义在 [`CSGpuMeshTypes.h`](../Source/ComputeShaderGenerator/Public/CSGpuMeshTypes.h)。增加非回读辅助流与增加 CPU 回读语义是两种改动，后者还需接通快照解码和保存消费端。
- `DrawIndexedIndirect` 的首个 uint 是索引数，不能当作顶点数。triangle soup 可有 `V == I`，indexed mesh 必须分别保留顶点数与索引数。
- 当前 CPU 快照支持顶点色、最多 8 组 UV、逐三角材质槽与材质表；旧的「只存白色、单 UV、单材质槽」限制已不适用。实际保留哪些属性取决于生产者注册和写入的流。
- 常驻网格按世界空间呈现。存盘要显式传入 `BakeSpace`，烘回结果 actor 的局部空间；不能直接使用恒为单位变换的渲染组件变换。
- 同步回读和存盘是阻塞操作，属于离线烘焙路径，不应放进拖动或每帧生成链。

## 验证入口

已有 `CSDirectMeshSaveTests.cpp`、`RoadMeshSaveTests.cpp`、`CSGpuMeshObjectTests.cpp`，以及岩壳的顶点色烘焙用例。
测试位置和运行要求见 [README「自动化测试」](../README.md#自动化测试)。本次仅核对文档与源码，未运行引擎测试。

## 设计沿革

最初统一的是 proxy 基类的分配、计数与保存；此后所有权上移到 `UCSMesh`。现行对象层说明见
[README「GPU 网格对象 · CSMesh」](../README.md)。

2026-07-24 的第二轮方案（原独立文档 `GpuTriangleUnified_Plan.md`，2026-11-05 并入本节）计划新建
`CSGpuReadback.h`、`FCSGpuReadbackRequest`、`AddCopies` 与独立的 DynamicMesh sink，都没有按此实现，
回读和保存改由 `UCSMesh` 对象层接管。各项的去向：

- **描述符驱动储存**：保留。`FCSGpuStreamDesc` 描述流；`FCSMeshResident` 持有常驻资源。
- **统一回读**：由 `CSMeshReadback::ReadbackResidentSync` 实现，作用于常驻集；计划里的 L0 回读层没有建。
- **统一保存**：`UCSMeshOps::CopyToStaticMesh` 与 `UCSMeshRenderComponent::SaveToStaticMesh` 为现行入口。
- **vine 迁移**：GPU 常驻化与旧 CPU 回读生成路的删除已完成；原 M1–M3 不再是待执行计划。

那一轮留下、现在仍然成立的设计约束：

1. 常驻流、单个 RDG 图中的瞬态 buffer、actor 持有的 pooled buffer 有不同生命周期，不能仅为统一接口强行互换所有权。
2. 索引数与顶点数分开；已知计数、容量上界和 GPU 实际计数应分别处理。
3. 回读完成后恢复各流的正确访问状态，尤其是 Index 与顶点流。
4. 异步化必须重新处理资源生命周期；不能照搬依赖游戏线程阻塞才安全的 proxy 裸指针捕获。
5. parity 测试保留独立参考来源，避免生成端和对照端同时依赖同一套错误的解码数学。

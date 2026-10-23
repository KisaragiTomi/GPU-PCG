# GPU 三角形统一设计沿革

[返回文档索引](index.md) · [当前实现](gpu-triangle-buffer-unification.md)

本文件记录 2026-07-24 的第二轮方案。原计划中的 `CSGpuReadback.h`、`FCSGpuReadbackRequest`、
`AddCopies` 与独立 DynamicMesh sink 未按此实现；后续由 `UCSMesh` 对象层接管回读和保存。
2026-09-30 清理了已失效的伪代码、迁移步骤、临时冻结规则和旧限制。

## 方案的去向

- **描述符驱动储存**：保留。`FCSGpuStreamDesc` 描述流；`FCSMeshResident` 持有常驻资源。
- **统一回读**：由 `CSMeshReadback::ReadbackResidentSync` 实现，作用于常驻集；无需新建计划里的 L0 接口。
- **统一保存**：`UCSMeshOps::CopyToStaticMesh` 与 `UCSMeshRenderComponent::SaveToStaticMesh` 为现行入口。
- **vine 迁移**：GPU 常驻化与旧 CPU 回读生成路的删除已完成；原 M1–M3 不再是待执行计划。
- **CPU 快照**：`FCSGpuMeshCPUData` 已支持颜色、多组 UV 与材质槽；不再沿用「白色、单 UV、单材质」假设。

## 保留的设计约束

1. 常驻流、单个 RDG 图中的瞬态 buffer、actor 持有的 pooled buffer 有不同生命周期，不能仅为统一接口强行互换所有权。
2. 索引数与顶点数分开；已知计数、容量上界和 GPU 实际计数应分别处理。
3. 回读完成后恢复各流的正确访问状态，尤其是 Index 与顶点流。
4. 异步化必须重新处理资源生命周期；不能照搬依赖游戏线程阻塞才安全的 proxy 裸指针捕获。
5. parity 测试保留独立参考来源，避免生成端和对照端同时依赖同一套错误的解码数学。

当前对象、算子、渲染组件和池的使用方式见 [README](../README.md)。

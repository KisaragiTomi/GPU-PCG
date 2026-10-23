## 跑测：smoke · NVIDIA GeForce RTX 4080 591.86 · D3D12 · 1920x1080 · 引擎 5.7.4-0+UE5 Development · 2026-10-02T23:18:55.321Z · 每机位 60 帧

关键 CVar：`r.Nanite=1`，`r.Shadow.Virtual.Enable=1`，`r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0`，`r.InstanceCulling.OcclusionCull=0`，`r.DynamicGlobalIlluminationMethod=1`，`r.ReflectionMethod=1`，`r.AntiAliasingMethod=4`，`r.ScreenPercentage=100`，`r.RayTracing=1`，`r.Substrate=1`，`r.VSync=0`，`t.MaxFPS=0`，`r.GPUCsvStatsEnabled=1`，`r.Shadow.CSM.MaxCascades=10`

### 场景

| 场景 | 路 | 源 | 活跃 | 容量 | 组件 | 顶点 / 三角 | LOD / 段 | Nanite | 常驻流 MiB | 源缓冲 MiB | 备注 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

### 逐机位（中位数，ms）

| 场景 | 机位 | 可见 (逐 LOD) | Frame | GT | RT | GPU | 剔除 | 写入 | 预通道 | 基通道 | 阴影 | Nanite | 场景更新 | 建图(CPU) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |

### CPU 实例数组的增删（游戏线程上一次调用，ms）

| 路 | 实例数 | 操作 | 次数 | 平均 | 最大 | 阻塞刷新 / 次 |
| --- | --- | --- | --- | --- | --- | --- |
| classic | 10000 | SetInstances | 3 | 1.48 | 2.74 | 1.33 |
| classic | 10000 | AddInstance | 8 | 0.84 | 1.27 | 1.00 |
| classic | 10000 | UpdateInstanceTransform | 8 | 0.71 | 0.87 | 1.00 |
| classic | 10000 | UpdateInstanceTransform_batched8 | 1 | 0.71 | 0.71 | 1.00 |
| classic | 10000 | RemoveInstance_middle | 8 | 0.85 | 1.26 | 1.00 |
| classic | 10000 | RemoveInstance_tail | 8 | 0.81 | 1.27 | 1.00 |
| classic | 10000 | AddInstances_batch8 | 1 | 0.68 | 0.68 | 1.00 |
| classic | 10000 | AddInstance_scope8 | 1 | 0.84 | 0.84 | 1.00 |
| classic | 10000 | MixedOps_scope24 | 1 | 0.77 | 0.77 | 1.00 |
| classic | 10000 | RemoveInstances_8 | 1 | 0.75 | 0.75 | 1.00 |
| classic | 100000 | SetInstances | 3 | 9.59 | 11.30 | 1.33 |
| classic | 100000 | AddInstance | 8 | 7.85 | 11.39 | 1.00 |
| classic | 100000 | UpdateInstanceTransform | 8 | 7.24 | 7.49 | 1.00 |
| classic | 100000 | UpdateInstanceTransform_batched8 | 1 | 7.32 | 7.32 | 1.00 |
| classic | 100000 | RemoveInstance_middle | 8 | 7.20 | 7.33 | 1.00 |
| classic | 100000 | RemoveInstance_tail | 8 | 7.32 | 7.59 | 1.00 |
| classic | 100000 | AddInstances_batch8 | 1 | 7.33 | 7.33 | 1.00 |
| classic | 100000 | AddInstance_scope8 | 1 | 7.06 | 7.06 | 1.00 |
| classic | 100000 | MixedOps_scope24 | 1 | 7.43 | 7.43 | 1.00 |
| classic | 100000 | RemoveInstances_8 | 1 | 7.50 | 7.50 | 1.00 |
| nanite | 10000 | SetInstances | 3 | 0.60 | 0.65 | 0.00 |
| nanite | 10000 | AddInstance | 8 | 0.49 | 0.66 | 0.00 |
| nanite | 10000 | UpdateInstanceTransform | 8 | 0.46 | 0.47 | 0.00 |
| nanite | 10000 | UpdateInstanceTransform_batched8 | 1 | 0.45 | 0.45 | 0.00 |
| nanite | 10000 | RemoveInstance_middle | 8 | 0.45 | 0.47 | 0.00 |
| nanite | 10000 | RemoveInstance_tail | 8 | 0.45 | 0.47 | 0.00 |
| nanite | 10000 | AddInstances_batch8 | 1 | 0.45 | 0.45 | 0.00 |
| nanite | 10000 | AddInstance_scope8 | 1 | 0.45 | 0.45 | 0.00 |
| nanite | 10000 | MixedOps_scope24 | 1 | 0.45 | 0.45 | 0.00 |
| nanite | 10000 | RemoveInstances_8 | 1 | 0.45 | 0.45 | 0.00 |
| nanite | 100000 | SetInstances | 3 | 7.08 | 7.81 | 0.00 |
| nanite | 100000 | AddInstance | 8 | 6.22 | 9.94 | 0.00 |
| nanite | 100000 | UpdateInstanceTransform | 8 | 5.67 | 6.01 | 0.00 |
| nanite | 100000 | UpdateInstanceTransform_batched8 | 1 | 5.60 | 5.60 | 0.00 |
| nanite | 100000 | RemoveInstance_middle | 8 | 5.46 | 5.51 | 0.00 |
| nanite | 100000 | RemoveInstance_tail | 8 | 5.53 | 5.73 | 0.00 |
| nanite | 100000 | AddInstances_batch8 | 1 | 5.54 | 5.54 | 0.00 |
| nanite | 100000 | AddInstance_scope8 | 1 | 5.50 | 5.50 | 0.00 |
| nanite | 100000 | MixedOps_scope24 | 1 | 5.46 | 5.46 | 0.00 |
| nanite | 100000 | RemoveInstances_8 | 1 | 5.48 | 5.48 | 0.00 |


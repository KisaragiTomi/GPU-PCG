## 交错 A/B：map · NVIDIA GeForce RTX 4080 591.86 · D3D12 · 1920x1080 · 引擎 5.7.4-0+UE5 Development · 2026-10-03T00:15:48.857Z · 每机位 200 帧

- `before`：r.CSGpuInstanced.VisibleSourceIndex 0; r.CSGpuInstanced.LeanCullSetup 0; r.CSGpuInstanced.Nanite.SkipUnchangedWrites 0
- `after`：r.CSGpuInstanced.VisibleSourceIndex 1; r.CSGpuInstanced.LeanCullSetup 3; r.CSGpuInstanced.Nanite.SkipUnchangedWrites 1

每格 = 变体 − `before` 的配对差的平均（ms）；括号里是各对里与平均差同号的对数。

### 常驻流（MiB）

| 场景 | 容量 | LOD | `before` | `after` |
| --- | --- | --- | --- | --- |
| map_L_HouseGroundDemo | 1976320 | 1 | 40.0 | 2.5 (-94%) |

### `after` − `before`

| 场景 | 机位 | 对数 | Frame | RT | GPU | 剔除 | 写入 | 预通道 | 基通道 | 阴影 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| map_L_HouseGroundDemo | top | 3 | -0.343 -4% (3/3) | -0.343 -4% (3/3) | -0.328 -4% (3/3) | -0.021 -8% (3/3) | -0.283 -83% (3/3) | +0.000 +0% (3/3) | -0.001 -0% (2/3) | -0.015 -1% (3/3) |
| map_L_HouseGroundDemo | edge | 3 | -0.326 -3% (3/3) | -0.327 -3% (3/3) | -0.319 -3% (3/3) | -0.012 -5% (3/3) | -0.290 -83% (3/3) | +0.000 +0% (3/3) | +0.000 +0% (2/3) | +0.004 +0% (2/3) |
| map_L_HouseGroundDemo | center | 3 | -0.300 -3% (3/3) | -0.296 -3% (3/3) | -0.304 -3% (3/3) | -0.002 -1% (3/3) | -0.313 -84% (3/3) | -0.002 -1% (3/3) | +0.001 +0% (3/3) | +0.025 +1% (3/3) |
| map_L_HouseGroundDemo | away | 3 | -0.284 -4% (3/3) | -0.288 -4% (3/3) | -0.314 -5% (3/3) | +0.001 +0% (1/3) | -0.281 -83% (3/3) | -0.001 -4% (3/3) | +0.000 +0% (2/3) | +0.002 +0% (3/3) |
| map_L_HouseGroundDemo | orbit | 3 | -0.052 -0% (3/3) | -0.081 -1% (3/3) | -0.029 -0% (3/3) | -0.009 -4% (3/3) | -0.073 -21% (3/3) | +0.000 +0% (2/3) | -0.001 -0% (3/3) | +0.005 +0% (2/3) |
| map_L_HouseGroundDemo | look | 3 | -0.304 -3% (3/3) | -0.301 -3% (3/3) | -0.320 -3% (3/3) | -0.008 -4% (3/3) | -0.280 -83% (3/3) | +0.001 +0% (3/3) | +0.003 +0% (3/3) | -0.031 -1% (3/3) |


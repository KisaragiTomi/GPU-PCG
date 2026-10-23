# VDBMeshFromSurfaceVoxels GPU 流程文档

> 当前 GPU 网格输出接口见 [README](../README.md)；本文保留算法说明。

`VDBMeshFromSurfaceVoxels` 的目标是从 `In_Actors` 与 `ValidPositions` 生成贴近真实表面的网格。新的设计不再以 CPU 复制 StaticMesh、合并 `FDynamicMesh3`、构建 `TMeshAABBTree3`、逐体素最近三角形查询为主流程，也不再把最终结果停留在 voxel 点云；而是让 GPU Compute Shader **直接绑定并读取 StaticMesh 已上传到显存/RHI 的顶点与索引 buffer**，把输入的参考点集合加入 CS，在 GPU 端完成：

```text
ReferencePoints -> 采样三角形集法线/最近表面 -> 直接生成三角形 -> 朝向校验/翻转/剔除
```

这里的“直接读取显存中的 Mesh 信息”指的是：

```text
Shader 读取 StaticMesh RenderData 的 PositionBuffer / IndexBuffer SRV
```

不是：

```text
CPU 直接拿指针读取 VRAM
```

CPU 如果需要结果，只读取 GPU 处理后的 compact 三角形顶点、索引与法线，不读取完整 Mesh 顶点/索引，也不读取中间 voxel。

对应函数声明：

```cpp
static UDynamicMesh* VDBMeshFromSurfaceVoxels(
    TArray<AActor*> In_Actors,
    TArray<FVector> ValidPositions,
    float VoxelSize = 10,
    float SurfaceDistance = 0,
    float PointRadiusMult = 2,
    bool bProjectToSurface = true,
    float InclusionDistance = 50.0f);
```

流程图：暂不生成，当前文档只维护 Markdown 设计说明。

## 1. GPU-only 目标

主目标：

```text
GameThread 只收集 Actor / Component / Instance 快照
RenderThread / RDG 绑定 StaticMesh RenderData 的 Position/Index SRV
Compute Shader 直接从显存/RHI buffer 读取三角形
Compute Shader 让参考点采样三角形集的 closest point / normal
Compute Shader 基于参考点邻域直接生成三角形
Compute Shader 剔除距离参考点过远的三角形
Compute Shader 检查三角形朝向是否接近周围参考点方向
GPU 端 compact triangles / vertices / normals
只 readback compact 后的最终三角形数据
CPU 只负责创建并填充 UDynamicMesh
```

明确不再作为主路径的内容：

- 不再 `CopyMeshFromStaticMesh` 到 `UDynamicMesh`。
- 不再为每个实例复制 `FDynamicMesh3`。
- 不再把候选网格合并成 `CombinedMesh`。
- 不再构建 CPU `TMeshAABBTree3`。
- 不再 CPU 遍历 `NX * NY * NZ` 体素并逐点查最近三角形。
- 不再执行 `T * P` 的三角形对 `ValidPositions` 线性距离检查。
- 不把显存中的 Mesh 全量 readback 到 CPU。
- 不再把 `SurfaceVoxels` 作为最终输出。
- 不再依赖 `ParticlesToVDBMeshUniform` 作为主流程 Mesh 化阶段。

## 2. 直接读取显存 Mesh 的可行路径

Unreal 中不能让 CPU 直接像访问普通内存一样读取 VRAM；但是可以在 RenderThread / RDG pass 中把 StaticMesh 的 GPU buffer 作为 SRV 绑定给 Compute Shader。这样 shader 就能直接读取 Mesh 顶点和索引：

```cpp
FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];

FRHIShaderResourceView* PositionBufferSRV =
    LOD.VertexBuffers.PositionVertexBuffer.GetSRV();

const FBufferRHIRef& IndexBufferRHI = LOD.IndexBuffer.GetRHI();
FShaderResourceViewRHIRef IndexBufferSRV =
    RHICmdList.CreateShaderResourceView(
        IndexBufferRHI,
        FRHIViewDesc::CreateBufferSRV()
            .SetType(FRHIViewDesc::EBufferType::Typed)
            .SetFormat(LOD.IndexBuffer.Is32Bit() ? PF_R32_UINT : PF_R16_UINT));
```

对应 shader 输入：

```hlsl
Buffer<uint>  IndexBuffer;
Buffer<float> PositionBuffer;

float3 ReadPosition(uint VertexIndex)
{
    uint Offset = VertexIndex * PositionStrideFloat;
    return float3(
        PositionBuffer[Offset + 0],
        PositionBuffer[Offset + 1],
        PositionBuffer[Offset + 2]);
}
```

之后每个 compute thread 可以读取三角形：

```hlsl
uint IndexBase = TriIndex * 3u;
uint I0 = IndexBuffer[IndexBase + 0u];
uint I1 = IndexBuffer[IndexBase + 1u];
uint I2 = IndexBuffer[IndexBase + 2u];

float3 P0 = ReadPosition(I0);
float3 P1 = ReadPosition(I1);
float3 P2 = ReadPosition(I2);
```

项目中已有 `StaticMeshRenderDataPointSampler` 使用了同一类机制：

```text
Source/ComputeShaderGenerator/Private/StaticMeshRenderDataPointSampler.cpp
Shaders/Private/StaticMeshPointSampler.usf
```

`VDBMeshFromSurfaceVoxels` 的 GPU 版本应沿用这个 RenderData SRV 读取方式，但把“点采样”升级为“参考点采样三角形法线 + GPU 直接三角化”。

## 3. 参数语义

| 参数 | GPU 流程中的用途 |
| --- | --- |
| `In_Actors` | GameThread 遍历组件与实例，只提取 `UStaticMesh`、LOD、transform、bounds 等快照数据。 |
| `ValidPositions` | 作为参考点集合上传 GPU。每个参考点会采样附近三角形集的 closest point / normal，并参与后续 GPU 直接三角化。 |
| `VoxelSize` | 不再只表示输出 voxel 尺寸；在直接三角化路径中作为参考点邻域尺度、边长上限、局部搜索半径的基础值。 |
| `SurfaceDistance` | 参考点采样三角形时的最大表面吸附距离；也是输出三角形距离参考点的硬阈值之一；`<= 0` 时使用 `VoxelSize`。 |
| `PointRadiusMult` | 直接三角化路径中不再传入 `ParticlesToVDBMeshUniform`；可复用为参考点影响半径倍数或邻域半径倍数。 |
| `bProjectToSurface` | GPU 目标流程中不再做 CPU 投影；参考点在 CS 中已经采样 closest point，可直接使用 projected point 生成三角形。 |
| `InclusionDistance` | Mesh/instance/triangle 与 reference point grid 的候选范围筛选距离。 |

## 4. 推荐模块结构

建议新增独立 Surface Triangulator，而不是复用只做三角中心采样的 `StaticMeshRenderDataPointSampler`：

```text
Source/ComputeShaderGenerator/Public/StaticMeshReferencePointTriangulator.h
Source/ComputeShaderGenerator/Private/StaticMeshReferencePointTriangulator.cpp
Shaders/Private/StaticMeshReferencePointTriangulator.usf
```

可参考现有 GPU 采样基础：

```text
Source/ComputeShaderGenerator/Public/StaticMeshRenderDataPointSampler.h
Source/ComputeShaderGenerator/Private/StaticMeshRenderDataPointSampler.cpp
Shaders/Private/StaticMeshPointSampler.usf
```

现有 sampler 已经证明了这些能力可用：

- 在 RDG 中绑定 StaticMesh position/index buffer SRV。
- Compute Shader 中读取三角形数据。
- GPU 端 hash compact。
- `FRHIGPUBufferReadback` 读回 compact 结果。

Reference point triangulator 需要在此基础上把“采样三角中心点”升级为：

```text
参考点上传 GPU
参考点采样附近三角形的 closest point / normal
参考点按邻域关系直接组 triangle
输出 compact vertices / indices / normals
```

## 5. GPU 数据流

```text
Blueprint 调用
  -> GameThread: 校验 ValidPositions，计算 Bounds / MaxDist / InclusionDistance
  -> GameThread: 遍历 Actor，收集 StaticMeshComponent / ISM / HISM 快照
  -> GameThread: 为每个 mesh/instance 生成 request，只保存 StaticMesh/LOD/Transform/Bounds
  -> RenderThread/RDG: 解析 UStaticMesh::GetRenderData()
  -> RenderThread/RDG: 绑定 PositionVertexBuffer.GetSRV() 与 IndexBuffer SRV
  -> RDG: 上传 ValidPositions 作为 ReferencePoints
  -> RDG: BuildReferencePointGridCS 构建 reference-point spatial grid
  -> RDG: CandidateCullCS 筛选 component/instance
  -> RDG: ReferencePointSurfaceSampleCS 让参考点采样三角形 closest point / normal
  -> RDG: BuildReferencePointNeighborCS 为参考点寻找局部邻点
  -> RDG: ReferencePointTriangulateCS 直接生成候选 triangles，并剔除距离参考点过远的 triangle
  -> RDG: TriangleOrientationValidateCS 检查距离阈值与朝向是否接近周围参考点方向
  -> RDG: CompactTrianglesCS 输出 compact vertices / indices / normals + counters
  -> GPU readback: 只读回 compact mesh buffers
  -> GameThread: 创建 UDynamicMesh 并填入 vertices / triangles / normals
```

## 6. Compute Pass 设计

### 6.1 BuildReferencePointGridCS

输入：

- `ValidPositions` structured buffer，作为 `ReferencePoints`。
- `BoundsMin / BoundsMax`。
- `ReferenceCellSize`，建议使用 `VoxelSize` 或 `VoxelSize * PointRadiusMult`。

输出：

- `ReferencePointCellSlots`。
- `ReferencePointCellCounts` / `ReferencePointCellRanges`。
- `ReferencePointIndexBuffer`。

作用：把输入参考点集合变成 GPU 空间索引。后续步骤不再把 `ValidPositions` 只当作 bounds 或 voxel 范围，而是每个参考点都参与：

```text
参考点 -> 采样三角形集 closest point / normal
参考点 -> 与周围参考点建立邻接关系
参考点邻域 -> 直接生成 triangle
```

### 6.2 CandidateCullCS

输入：

- Component/instance bounds buffer。
- Instance transform buffer。
- Reference point grid。
- `InclusionDistance`。

输出：

- `CandidateInstanceList`。
- `CandidateInstanceCount`。

作用：每个 instance 一个或多个 thread，判断其 world bounds 是否靠近 reference point grid。命中后 append 到候选列表。这样参考点只会采样附近 mesh 的三角形，不会对全场景所有三角形做查询。

### 6.3 ReferencePointSurfaceSampleCS

核心 pass。这个 pass 把参考点集合也放进 CS。每个参考点在 GPU 中查询候选三角形集，采样最近表面点与法线。

```text
ReferencePoint P
  -> 查询附近 CandidateInstance / CandidateTriangle
  -> 从 Position/Index SRV 读取三角形
  -> local vertices -> world vertices
  -> closest point on triangle
  -> triangle normal / optionally interpolated vertex normal
  -> 选择距离最近或加权最稳定的 normal
  -> 输出 SampledReferencePoint
```

HLSL 伪代码：

```hlsl
[numthreads(64, 1, 1)]
void ReferencePointSurfaceSampleCS(uint3 DTid : SV_DispatchThreadID)
{
    uint pointId = DTid.x;
    if (pointId >= ReferencePointCount)
    {
        return;
    }

    float3 refP = ReferencePoints[pointId].xyz;

    float bestDistSq = MaxSurfaceDistanceSq;
    float3 bestClosest = refP;
    float3 bestNormal = float3(0, 0, 1);
    uint bestTri = INVALID_TRIANGLE;

    CandidateTriangleIterator it = MakeCandidateTriangleIterator(refP, InclusionDistance);
    while (it.HasNext())
    {
        CandidateTriangle tri = it.Next();

        float3 v0, v1, v2;
        LoadWorldTriangle(tri, v0, v1, v2);

        float3 nRaw = cross(v1 - v0, v2 - v0);
        float nLenSq = dot(nRaw, nRaw);
        if (nLenSq <= 1e-12f)
        {
            continue;
        }
        float3 triNormal = nRaw * rsqrt(nLenSq);

        float3 closest = ClosestPointOnTriangle(refP, v0, v1, v2);
        float distSq = dot(refP - closest, refP - closest);
        if (distSq < bestDistSq)
        {
            bestDistSq = distSq;
            bestClosest = closest;
            bestNormal = triNormal;
            bestTri = tri.GlobalTriangleId;
        }
    }

    if (bestTri == INVALID_TRIANGLE)
    {
        SampledPoints[pointId].Flags = 0;
        return;
    }

    // 如果输入只有参考点位置，没有显式方向，则从 refP 指向采样表面点推导参考方向。
    // 方向约定可按项目需求翻转：SurfaceToReference 或 ReferenceToSurface。
    float3 referenceVec = refP - bestClosest;
    float referenceLenSq = dot(referenceVec, referenceVec);
    float3 referenceDir = referenceLenSq > 1e-8f
        ? referenceVec * rsqrt(referenceLenSq)
        : bestNormal;

    SampledPoints[pointId].Position = bestClosest;
    SampledPoints[pointId].Normal = bestNormal;
    SampledPoints[pointId].ReferenceDirection = referenceDir;
    SampledPoints[pointId].DistanceSq = bestDistSq;
    SampledPoints[pointId].SourceTriangleId = bestTri;
    SampledPoints[pointId].Flags = SAMPLE_VALID;
}
```

如果之后需要更平滑的法线，可以在 `bestTri` 附近做加权平均：

```text
normal = normalize(sum(triNormal_i * weight_i))
weight_i = saturate(1 - distSq_i / MaxSurfaceDistanceSq)
```

### 6.4 BuildReferencePointNeighborCS

输入：

- `SampledPoints`。
- Reference point grid。
- `NeighborRadius = VoxelSize * PointRadiusMult`。
- `MaxNeighbors`，建议 8、12 或 16，避免单点邻域过大。

输出：

- `ReferenceNeighborIndices`。
- `ReferenceNeighborCounts`。

作用：为每个有效参考点寻找周围参考点。后续 GPU 直接三角化不从 voxel 生成，而是基于这些参考点邻域连接 triangle。

筛选邻点时建议同时检查：

```text
distance(Pi, Pj) <= NeighborRadius
dot(Ni, Nj) >= NormalConsistencyCos
abs(dot(normalize(Pj - Pi), Ni)) <= TangentPlaneTolerance
```

这样可以避免把两层相近但朝向不同的表面错误连起来。

### 6.5 ReferencePointTriangulateCS

输入：

- `SampledPoints`。
- `ReferenceNeighborIndices / Counts`。
- `MaxEdgeLength`。
- `MaxTriangleReferenceDistance`，建议取 `SurfaceDistance > 0 ? SurfaceDistance : VoxelSize`。
- `NormalConsistencyCos`。

输出：

- `CandidateTriangles`。
- `CandidateTriangleCount`。

作用：在 CS 中把参考点直接转成三角形，不再输出 voxel。这里必须过滤掉距离参考点集合过远的三角形。推荐第一版使用“局部切平面扇形三角化”：

```text
对每个中心参考点 C：
  取 C 的 sampled surface position Pc 和 normal Nc
  根据 Nc 构建 tangent basis T/B
  把邻点投影到 C 的 tangent plane
  按 atan2(dot(Pj-Pc,B), dot(Pj-Pc,T)) 排序
  相邻邻点组成候选 triangle: C, Nj, Nj+1
  只在 C 的 point id 是三点中最小 id 时输出，减少重复 triangle
```

HLSL 伪代码：

```hlsl
[numthreads(64, 1, 1)]
void ReferencePointTriangulateCS(uint3 DTid : SV_DispatchThreadID)
{
    uint centerId = DTid.x;
    if (!IsSampleValid(centerId))
    {
        return;
    }

    SampledPoint c = SampledPoints[centerId];
    NeighborItem neighbors[MAX_NEIGHBORS];
    uint neighborCount = LoadAndProjectNeighbors(centerId, c.Position, c.Normal, neighbors);

    SortNeighborsByAngle(neighbors, neighborCount);

    for (uint i = 0; i + 1 < neighborCount; ++i)
    {
        uint a = neighbors[i].PointId;
        uint b = neighbors[i + 1].PointId;

        if (!IsSampleValid(a) || !IsSampleValid(b))
        {
            continue;
        }

        // 避免重复输出同一片局部三角形。
        if (centerId > min(a, b))
        {
            continue;
        }

        float3 p0 = c.Position;
        float3 p1 = SampledPoints[a].Position;
        float3 p2 = SampledPoints[b].Position;

        if (!PassTriangleShapeTest(p0, p1, p2, MaxEdgeLength))
        {
            continue;
        }

        if (!PassTriangleReferenceDistanceTest(centerId, a, b, MaxTriangleReferenceDistance))
        {
            continue;
        }

        AppendCandidateTriangle(centerId, a, b);
    }
}
```

形状检查建议：

```text
edge length <= MaxEdgeLength
triangle area >= MinTriangleArea
triangle aspect ratio <= MaxAspectRatio
dot(sample normal i, sample normal j) >= NormalConsistencyCos
```

距离参考点检查建议至少包含三层：

```text
1. 三角形三个顶点必须来自有效 sampled reference points，且各自采样距离 <= SurfaceDistance。
2. triangle centroid 到最近参考点 / 最近 sampled point 的距离 <= MaxTriangleReferenceDistance。
3. triangle 三条边的中点到参考点 grid 的最近距离 <= MaxTriangleReferenceDistance，避免跨空洞生成长三角。
```

HLSL 伪代码：

```hlsl
bool PassTriangleReferenceDistanceTest(uint i0, uint i1, uint i2, float MaxRefDist)
{
    SampledPoint s0 = SampledPoints[i0];
    SampledPoint s1 = SampledPoints[i1];
    SampledPoint s2 = SampledPoints[i2];

    float maxRefDistSq = MaxRefDist * MaxRefDist;

    // 每个顶点的表面采样本身不能离原始参考点太远。
    if (s0.DistanceSq > maxRefDistSq || s1.DistanceSq > maxRefDistSq || s2.DistanceSq > maxRefDistSq)
    {
        return false;
    }

    float3 c = (s0.Position + s1.Position + s2.Position) / 3.0f;
    float3 e01 = (s0.Position + s1.Position) * 0.5f;
    float3 e12 = (s1.Position + s2.Position) * 0.5f;
    float3 e20 = (s2.Position + s0.Position) * 0.5f;

    // QueryNearestReferenceDistanceSq 使用 BuildReferencePointGridCS 生成的 reference-point spatial grid。
    if (QueryNearestReferenceDistanceSq(c) > maxRefDistSq)
    {
        return false;
    }
    if (QueryNearestReferenceDistanceSq(e01) > maxRefDistSq ||
        QueryNearestReferenceDistanceSq(e12) > maxRefDistSq ||
        QueryNearestReferenceDistanceSq(e20) > maxRefDistSq)
    {
        return false;
    }

    return true;
}
```

### 6.6 TriangleOrientationValidateCS

输入：

- `CandidateTriangles`。
- `SampledPoints.Position`。
- `SampledPoints.Normal`。
- `SampledPoints.ReferenceDirection`。
- 邻域参考点方向统计。
- `MaxTriangleReferenceDistance`。

输出：

- `ValidatedTriangles`。
- `TriangleFlags`，记录 kept / flipped / rejected / too far from reference。

作用：最后输出的三角形必须检查两件事：

1. 三角形不能距离参考点集合过远。
2. 三角形朝向要和周围参考点方向差不多。

检查逻辑如下：

```text
triNormal = normalize(cross(P1 - P0, P2 - P0))
avgSampleNormal = normalize(N0 + N1 + N2)
avgReferenceDir = normalize(D0 + D1 + D2 + neighbor reference dirs)

如果 triangle centroid / edge midpoint 到 reference point grid 太远:
    剔除 triangle
如果 dot(triNormal, avgReferenceDir) >= OrientationCosThreshold:
    保留 triangle
如果 dot(-triNormal, avgReferenceDir) >= OrientationCosThreshold:
    交换 triangle index 1/2，翻转朝向后保留
否则:
    剔除 triangle 或标记为低置信度
```

HLSL 伪代码：

```hlsl
void ValidateTriangleOrientation(inout uint i0, inout uint i1, inout uint i2, out uint flags)
{
    float3 p0 = SampledPoints[i0].Position;
    float3 p1 = SampledPoints[i1].Position;
    float3 p2 = SampledPoints[i2].Position;

    float3 triNormalRaw = cross(p1 - p0, p2 - p0);
    float area2 = length(triNormalRaw);
    if (area2 <= MinTriangleArea2)
    {
        flags = TRIANGLE_REJECTED;
        return;
    }

    float3 triNormal = triNormalRaw / area2;

    if (!PassTriangleReferenceDistanceTest(i0, i1, i2, MaxTriangleReferenceDistance))
    {
        flags = TRIANGLE_REJECTED_TOO_FAR_FROM_REFERENCE;
        return;
    }

    float3 avgReferenceDir = normalize(
        SampledPoints[i0].ReferenceDirection +
        SampledPoints[i1].ReferenceDirection +
        SampledPoints[i2].ReferenceDirection +
        LoadNeighborReferenceDirectionAverage(i0, i1, i2));

    float d = dot(triNormal, avgReferenceDir);
    if (d >= OrientationCosThreshold)
    {
        flags = TRIANGLE_KEEP;
        return;
    }

    if (-d >= OrientationCosThreshold)
    {
        uint tmp = i1;
        i1 = i2;
        i2 = tmp;
        flags = TRIANGLE_FLIPPED;
        return;
    }

    flags = TRIANGLE_REJECTED;
}
```

如果参考点本身没有显式方向，`ReferenceDirection` 默认由采样结果推导：

```text
ReferenceDirection = normalize(ReferencePoint - ClosestSurfacePoint)
如果长度接近 0，则 fallback 到采样到的三角形 normal。
```

如果后续 API 可以传入方向数组，建议新增：

```cpp
TArray<FVector> ReferenceDirections
```

此时朝向检查优先使用输入方向；缺失时再用 `ReferencePoint - ClosestSurfacePoint` 推导。

### 6.7 CompactTrianglesCS

输入：

- `ValidatedTriangles`。
- `SampledPoints.Position`。
- `SampledPoints.Normal`。
- `TriangleFlags`。

输出：

- `RW_CompactVertices`。
- `RW_CompactIndices`。
- `RW_CompactVertexNormals`。
- `RW_TriangleCount`。
- `RW_VertexCount`。

作用：把通过朝向检查的 triangle 直接输出为最终 mesh buffer。第一版可以允许每个 triangle 输出 3 个独立顶点，避免复杂 GPU 顶点去重；后续再增加 hash vertex dedup。

```text
CompactVertices[vertexId + 0] = P0
CompactVertices[vertexId + 1] = P1
CompactVertices[vertexId + 2] = P2
CompactIndices[triId * 3 + 0] = vertexId + 0
CompactIndices[triId * 3 + 1] = vertexId + 1
CompactIndices[triId * 3 + 2] = vertexId + 2
CompactVertexNormals[...]     = sampled normal 或 triangle normal
```

## 7. GPU 数据结构建议

| Buffer | 类型 | 说明 |
| --- | --- | --- |
| `ReferencePointsBuffer` | `StructuredBuffer<float4>` | 输入参考点，xyz 为位置，w 可存权重/半径/标记。 |
| `ReferenceDirectionsBuffer` | `StructuredBuffer<float4>` | 可选输入参考方向；没有时由 `ReferencePoint - ClosestSurfacePoint` 推导。 |
| `ReferencePointCellSlots` | `RWBuffer<uint>` | reference point hash grid。 |
| `ReferencePointCellRanges` | `RWBuffer<uint2>` | 每个 grid cell 的参考点范围，用于邻域查询。 |
| `ReferencePointIndexBuffer` | `RWBuffer<uint>` | grid 排序/分桶后的参考点索引。 |
| `InstanceRequestBuffer` | `StructuredBuffer` | mesh id、instance id、transform、bounds。 |
| `CandidateInstanceBuffer` | `RWStructuredBuffer<uint>` | 粗筛命中的 instance。 |
| `CandidateTriangleBuffer` | `RWStructuredBuffer` | 可选，存储靠近参考点 grid 的候选三角形范围。 |
| `MeshSectionTable` | `StructuredBuffer` | 每个 StaticMesh/LOD 的 triangle offset / section 信息。 |
| `MeshResourceTable` | CPU/RDG side table | 保存每个 request 对应的 `FStaticMeshLODResources`、Position SRV、Index SRV。 |
| `PositionBufferSRV` | `Buffer<float>` | StaticMesh LOD position vertex buffer，shader 直接读。 |
| `IndexBufferSRV` | `Buffer<uint>` | StaticMesh LOD index buffer，shader 直接读。 |
| `TransformBuffer` | `StructuredBuffer<float4x4>` | local-to-world 矩阵。 |
| `SampledPoints` | `RWStructuredBuffer` | 每个参考点采样后的 surface position、normal、reference direction、source triangle、distance、flags。 |
| `ReferenceNeighborIndices` | `RWBuffer<uint>` | 每个参考点的邻点索引，定长或 prefix-range。 |
| `ReferenceNeighborCounts` | `RWBuffer<uint>` | 每个参考点有效邻点数量。 |
| `CandidateTriangles` | `RWStructuredBuffer<uint3>` | 参考点邻域生成的候选三角形 point ids。 |
| `TriangleFlags` | `RWBuffer<uint>` | kept / flipped / rejected / low confidence。 |
| `MaxTriangleReferenceDistance` | uniform float | 输出三角形允许距离参考点集合的最大距离，通常来自 `SurfaceDistance` 或 `VoxelSize`。 |
| `CompactVertices` | `RWBuffer<float4>` | readback 顶点，xyz 为位置，w 可存 source point id。 |
| `CompactIndices` | `RWBuffer<uint>` | readback 三角形索引。 |
| `CompactVertexNormals` | `RWBuffer<float4>` | readback 顶点法线，xyz 为 normal，w 可存 confidence。 |
| `Counters` | `RWBuffer<uint>` | candidate count、sampled point count、triangle count、vertex count、debug 统计。 |

## 8. Readback 策略

当前同步 Blueprint 返回 `UDynamicMesh*` 会迫使函数等待 GPU 结果。注意：这里 readback 的是 GPU 生成后的 compact mesh buffers，不是原始 Mesh 的 position/index buffer，也不是中间 voxel。为了真正避免编辑器卡顿，建议拆成任务式 API：

```cpp
StartVDBMeshFromSurfaceVoxelsGPU(..., FOnVDBMeshReady Callback)
```

第一阶段可先保持同步兼容，但内部应尽量做到：

1. 先 readback counter。
2. 根据 `VertexCount / TriangleCount` 只读回有效 `CompactVertices / CompactIndices / CompactVertexNormals`。
3. 超过上限时分块 readback、降采样参考点或提高 `NormalConsistencyCos`。
4. 对 1000w 三角以上输入，避免一次性生成/读回百万级三角形。

经验目标：

| 输出规模 | 建议 |
| --- | --- |
| `< 10w triangles` | 可接受，适合交互。 |
| `10w - 50w triangles` | 需要异步 readback，UDynamicMesh 构建可能成为主耗时。 |
| `> 50w triangles` | 应考虑参考点降采样、LOD 或 GPU vertex dedup 后再 readback。 |

## 9. 性能预估：100 个 Actor / 1000w 三角

假设目标 Bounds 不是全场景极大体积，且 GPU 端只读回 compact vertices / indices / normals：

| 阶段 | GPU-only 预估 | 说明 |
| --- | --- | --- |
| GameThread 快照 | `5-50ms` | 取决于 actor/component/instance 数量，不读 mesh 拓扑，不 readback 显存 mesh。 |
| RenderData SRV 绑定 | `<1-10ms` | 解析 LODResource，绑定 position/index SRV；不复制 mesh 数据。 |
| Reference grid 构建 | `<1-5ms` | 取决于 `ValidPositions.Num()`。 |
| CandidateCullCS | `<1-5ms` | instance 数量很大时上升。 |
| ReferencePointSurfaceSampleCS | `10-250ms` | 取决于参考点数量、候选三角数量、空间索引效率。 |
| BuildReferencePointNeighborCS | `1-30ms` | 取决于参考点密度和 `MaxNeighbors`。 |
| ReferencePointTriangulateCS | `1-60ms` | 取决于参考点数量、邻点数、排序、形状检查和 reference distance 查询成本。 |
| TriangleOrientationValidateCS | `1-40ms` | 取决于候选 triangle 数、reference distance 查询和邻域方向统计。 |
| CompactTrianglesCS | `1-30ms` | 取决于最终 kept triangle 数。 |
| GPU readback | `2-80ms+` | 取决于 compact vertices / indices / normals 数量和是否同步等待。 |
| UDynamicMesh 返回 | `10-200ms+` | 取决于最终网格规模。 |

对比旧 CPU 路线，最大收益来自避开：

```text
CopyMeshFromStaticMesh + instance MeshCopy + T*P 裁剪 + CPU AABBTree + NX*NY*NZ 最近三角形查询
```

## 10. 读取显存 Mesh 的限制

| 限制 | 说明 | 建议 |
| --- | --- | --- |
| CPU 不能直接读 VRAM 指针 | CPU 需要结果必须 readback。 | 只 readback compact vertices / indices / normals。 |
| Nanite | 普通 `LODResources` 不一定等于 Nanite 实际渲染 cluster。 | 先读 fallback LOD；必要时用碰撞/深度图 fallback。 |
| WPO / 材质位移 | PositionBuffer 是原始局部顶点，不包含材质 shader 变形。 | 需要时在 compute shader 中复现变形，或走 SceneDepth/自定义 pass。 |
| SkeletalMesh | 动画后顶点来自 skinning / skin cache，不同于 StaticMesh。 | 本函数先限定 StaticMeshComponent / ISM / HISM。 |
| 同步 readback | `FlushRenderingCommands()` / GPU idle 会卡编辑器。 | 改异步任务，counter 与 compact mesh buffers 分阶段 readback。 |

## 11. 参考点方向与三角形朝向

原函数的 `bProjectToSurface` 会把输出网格顶点再次投影到 `CombinedMesh` 最近三角形。新目标里不再需要 CPU 投影：参考点已经在 `ReferencePointSurfaceSampleCS` 中采样了 `ClosestSurfacePoint`，后续 triangle 直接使用这些 sampled surface positions。

关键问题变成：**最终输出 triangle 的 winding/normal 是否和周围参考点方向一致**。

建议约定：

```text
ReferenceDirection 表示期望输出 triangle normal 大致朝向。
若用户没有提供 ReferenceDirection，则使用 normalize(ReferencePoint - ClosestSurfacePoint) 推导。
```

三角形输出前必须做：

```text
dot(TriangleNormal, AverageReferenceDirection) >= OrientationCosThreshold
```

如果相反则翻转 index；如果两边都不接近，则剔除或标记低置信度。

## 12. 推荐迭代顺序

1. 新增 `StaticMeshReferencePointTriangulator` C++/USF 文件。
2. 复用现有 `StaticMeshRenderDataPointSampler` 的 RDG 绑定与 readback 框架。
3. 在 RenderThread/RDG 中解析 `FStaticMeshLODResources`，绑定 `PositionVertexBuffer.GetSRV()`。
4. 为 index buffer 创建 `IndexBufferSRV`，兼容 16/32 bit index。
5. 在 shader 中实现 `ReadPosition()` 与 `LoadWorldTriangle()`。
6. 实现 `BuildReferencePointGridCS`。
7. 实现 `CandidateCullCS`。
8. 实现 `ReferencePointSurfaceSampleCS`，让参考点采样三角形集的 closest point / normal。
9. 实现 `BuildReferencePointNeighborCS`，为参考点构建局部邻域。
10. 实现 `ReferencePointTriangulateCS`，在 CS 中直接从参考点邻域生成 candidate triangles。
11. 在 `ReferencePointTriangulateCS` 中加入 `MaxTriangleReferenceDistance` 检查，避免输出远离参考点集合的三角形。
12. 实现 `TriangleOrientationValidateCS`，再次检查距离阈值，并检查/翻转/剔除朝向不符合周围参考点方向的三角形。
13. 实现 `CompactTrianglesCS`，输出 compact vertices / indices / vertex normals。
14. GameThread 根据 readback mesh buffers 创建并填入 `UDynamicMesh`。
15. 验证 `VoxelSize / SurfaceDistance / PointRadiusMult` 对邻域、三角形密度、距离阈值和朝向一致性的影响。
16. 再改成异步 API，避免同步 `FlushRenderingCommands()` 卡住编辑器。
17. 后续优化 GPU vertex dedup、边界补洞、LOD 降采样。

## 13. 验证指标

建议每次生成记录：

- Actor 数、component 数、instance 数。
- candidate instance 数。
- 输入三角形总数与候选三角形数。
- SRV 绑定成功/失败数。
- 16-bit / 32-bit index buffer 数量。
- reference point 数。
- sampled reference point 数。
- sample miss 数 / 超过 `SurfaceDistance` 数。
- average / max neighbor count。
- candidate triangle count。
- kept / flipped / rejected triangle count。
- rejected too far from reference triangle count。
- triangle centroid / edge midpoint reference distance max。
- orientation dot 平均值 / 最小值。
- invalid normal count。
- readback 字节数。
- compact vertex / index / normal 数量。
- 最终输出三角形数。
- 各 RDG pass GPU 时间。
- GameThread 等待时间。

这些指标可以判断瓶颈是否已经从 CPU Mesh 处理转移到 GPU 参考点采样、GPU 三角化、readback 或 `UDynamicMesh` 构建。

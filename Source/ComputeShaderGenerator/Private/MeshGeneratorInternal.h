#pragma once

// -----------------------------------------------------------------------------
// ComputeShaderGenerator 模块内部共享接口（Private 头，不导出模块 API）。
//
// AComputeShaderMeshGenerator 的场景提取内部管线原先全部以匿名命名空间的形式住在
// ComputeShaderMeshGenerator.cpp 里；拆分 MeshGeneratorBrushCache.cpp /
// MeshGeneratorLandscapeCapture.cpp 之后，这些内部函数需要跨 TU 链接，因此收进
// CSMeshGenInternal 命名空间（具名命名空间同时防 unity build 匿名合并冲突）。
// 四个大函数的定义仍留在 ComputeShaderMeshGenerator.cpp。
// -----------------------------------------------------------------------------

#include "CoreMinimal.h"
#include "ComputeShaderMeshGenerator.h"
#include "StaticMeshResources.h"

class FRDGBuilder;
class FRHICommandListImmediate;
class FTextureRenderTargetResource;
class UMaterialInterface;
class UPrimitiveComponent;

namespace CSMeshGenInternal
{

struct FResolvedStaticMeshTriangleRequest
{
	TRefCountPtr<const FStaticMeshLODResources> LODResource;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FBox3f WorldBounds = FBox3f(EForceInit::ForceInit);
	int32 TriangleCount = 0;
	int32 PositionStrideFloat = 3;
	// 长度 == TriangleCount：每个源三角形的材质 registry id（CS_NO_MATERIAL_ID = 无材质）。
	// 由 section 范围 + Request.MaterialSlots 在 game thread 构建，随 request 上传给 GPU extract。
	TArray<uint32> TriToMaterial;
};

// Nanite 网格的全细节源三角（来自 editor MeshDescription，绕过低模 render fallback）。
// 已在 game thread 变换到世界空间并按 request bounds 粗筛；每三角携带 3 组 UV0 与 1 个材质 registry id
// （与 render 路径同一张材质表），由 AppendSourceTrianglesCS 原子追加进与 render 提取相同的 triangle soup。
// 多个 Nanite request 共用一份实例（扁平拼接），故一次 dispatch 即可追加整场景的全部 Nanite 源三角。
struct FCSNaniteSourceTriangleData
{
	// 每三角 3 个世界空间顶点，扁平：[t0.v0, t0.v1, t0.v2, t1.v0, ...]，w=1。绕序与 render index buffer 一致。
	TArray<FVector4f> Positions;
	// Per-corner source normals transformed to world space, parallel to Positions.
	TArray<FVector4f> Normals;
	TArray<FVector4f> Colors;
	TArray<FVector4f> Tangents;
	TArray<FVector4f> BiTangents;
	// 每三角 3 组 UV0，扁平，与 Positions 平行。
	// 逐角点 UV，按通道交错存放：UVs[Corner * NumUVChannels + Channel]。
	// 用交错而不是 N 条并行数组，是为了让整条链（上传/GPU/回读）只需要多一个通道数标量，
	// 而不是每加一条 UV 就多一套 buffer、SRV、readback。
	TArray<FVector2f> UVs;
	// 源模型实际有几条 UV 就是几条；所有 request 取最大值，缺的通道补 (0,0)。
	int32 NumUVChannels = 1;
	// 每三角 1 个材质 registry id（CS_NO_MATERIAL_ID = 无材质）。
	TArray<uint32> MaterialIds;
	// 逐三角的参照标志（1=参照体）。参照三角照常进 soup/LBVH/winding 场，
	// 但 tri-tri 切分与 BSP 输出都会跳过它们。
	TArray<uint32> ReferenceFlags;
	// 有效三角数（= Positions.Num() / 3 == MaterialIds.Num()）。
	int32 NumTriangles = 0;

	bool IsEmpty() const { return NumTriangles <= 0; }
};

// --- 线性数据纹理尺寸（cache RT 与 generated-data RT 共用同一套公式） ---

inline constexpr int32 CSGeneratorMinTextureDimension = 1;
inline constexpr int32 CSGeneratorDefaultTextureDimension = 1;

inline int32 CeilDivInt64ToInt32(int64 Numerator, int64 Denominator)
{
	if (Denominator <= 0) return 0;
	return int32((Numerator + Denominator - 1) / Denominator);
}

inline FIntPoint GetLinearDataTextureSize(int64 ElementCount, int32 MaxTextureDimension)
{
	const int32 SafeMaxDimension = FMath::Max(CSGeneratorMinTextureDimension, MaxTextureDimension);
	const int64 SafeElementCount = FMath::Max<int64>(1, ElementCount);
	const int32 Width = FMath::Min<int32>(
		SafeMaxDimension,
		FMath::Max<int64>(CSGeneratorDefaultTextureDimension, FMath::Min<int64>(SafeElementCount, SafeMaxDimension)));
	const int32 Height = FMath::Max(CSGeneratorDefaultTextureDimension, CeilDivInt64ToInt32(SafeElementCount, Width));
	return FIntPoint(Width, Height);
}

// 有限性谓词：原 IsFiniteCSGeneratorVector / IsFiniteCSVertex 两份逐字相同实现的合并版。
inline bool IsFiniteVector(const FVector& Vector)
{
	return !Vector.ContainsNaN()
		&& FMath::IsFinite(Vector.X)
		&& FMath::IsFinite(Vector.Y)
		&& FMath::IsFinite(Vector.Z);
}

// VertexCount/IndexCount < 0 表示"用数组长度"；有效计数 = 声明计数与数组实际长度的安全交集。
inline int32 GetEffectiveVertexCount(const FCSTriangleMeshData& TriangleData)
{
	return TriangleData.VertexCount >= 0
		? FMath::Clamp(TriangleData.VertexCount, 0, TriangleData.Vertices.Num())
		: TriangleData.Vertices.Num();
}

inline int32 GetEffectiveIndexCount(const FCSTriangleMeshData& TriangleData)
{
	return TriangleData.IndexCount >= 0
		? FMath::Clamp(TriangleData.IndexCount, 0, TriangleData.Indices.Num())
		: TriangleData.Indices.Num();
}

// 索引 >= 3 时按 indexed mesh 计数，否则按 soup。
inline int32 GetTriangleMeshDataTriangleCount(const FCSTriangleMeshData& TriangleData)
{
	const int32 EffectiveIndexCount = GetEffectiveIndexCount(TriangleData);
	return EffectiveIndexCount >= 3 ? EffectiveIndexCount / 3 : GetEffectiveVertexCount(TriangleData) / 3;
}

// RT 注册进 RDG 的空值安全包装（定义在 ComputeShaderMeshGenerator.cpp；核心与 BrushCache 共用）。
FRDGTextureRef RegisterRenderTargetTexture(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* RenderTargetResource, const TCHAR* DebugName);

// --- 场景提取内部管线（定义在 ComputeShaderMeshGenerator.cpp） ---

// [game thread] 把 request 列表 resolve 成可跨线程携带的 render 资源引用 + 材质注册表。
// 返回值仅统计 render-resolve 出的三角；Nanite 全细节源三角单独累积进 *OutNaniteTriangles。
// OutNaniteRenderComponents 非空时，按 Nanite 画着的组件不 resolve，去重收进它（交给渲染器去拍）；
// bCollectAllRenderComponents 再把范围放宽到所有渲染器拍得到的组件（单腿化，见 CSBoxSceneCollection.h）。
uint64 ResolveStaticMeshTriangleRequests(
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	const AActor* ExcludedActor,
	const TArray<FName>& ExcludedActorTags,
	bool bNaniteOnlyFallbackMesh,
	TArray<FResolvedStaticMeshTriangleRequest>& OutResolvedRequests,
	TArray<TObjectPtr<UMaterialInterface>>* OutMaterialRegistry = nullptr,
	FCSNaniteSourceTriangleData* OutNaniteTriangles = nullptr,
	bool bPreserveSourceMaterialSlots = true,
	TArray<TWeakObjectPtr<UPrimitiveComponent>>* OutNaniteRenderComponents = nullptr,
	bool bCollectAllRenderComponents = false);

// [game thread] 枚举 QueryBox 内的 static mesh component，生成提取 request。
// RequiredActorTags 非空时在枚举阶段就跳过不带任一 tag 的 actor——放到 request 建完再筛，
// 盒内未带 tag 的大型 instanced 组件会先展开成海量 per-instance request（各自拷贝材质槽表）
// 再被整批丢弃，白付一次峰值内存。NAME_None 条目永不匹配（与收集器的既有约定一致）。
void BuildBoxSceneTriangleRequestsInternal(UWorld* World,
	const FBox& QueryBox,
	int32 LODIndex,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests,
	const TArray<FName>& RequiredActorTags = TArray<FName>());

// [game thread] Landscape 高度场 CPU 提取为 triangle soup（世界坐标）。细节见定义处注释。
void BuildBoxSceneLandscapeTrianglesInternal(UWorld* World,
	const FBox& QueryBox,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData,
	const FTransform* WorldToLocalBoxTransform = nullptr,
	const FVector* LocalBoxExtent = nullptr,
	FName RequiredActorTag = NAME_None,
	bool bSortComponentsByDistance = true);

// [render thread] 轻量高度图光栅化：resolved request 的 index/position buffer 直读，逐网格
// dispatch 融合 kernel 打进一张共享 uint 深度图，最后按 min 合并进 OutputHeightmap
//（空 texel 保留输出现值）。全程不建 triangle soup，是 AddResolvedStaticMeshTrianglesToRDGInternal
// 的"只要高度"替代——soup 的顶点/法线/UV/材质 buffer 对深度捕获全是死重。
void RasterizeResolvedMeshesToHeightmapRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FResolvedStaticMeshTriangleRequest>& ResolvedRequests,
	FRDGTextureRef OutputHeightmap,
	const FBox& WorldBounds,
	float CameraHeight);

// [render thread] 核心 RDG 提取管线：resolved request + landscape/initial 三角 + Nanite 源三角
// → 统一 triangle soup buffer 组。
FCSStaticMeshTriangleRDGOutput AddResolvedStaticMeshTrianglesToRDGInternal(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FResolvedStaticMeshTriangleRequest>& ResolvedRequests,
	uint64 TotalTriangleCount,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	const FCSTriangleMeshData* InitialTriangleData,
	const TCHAR* DebugName,
	const FCSNaniteSourceTriangleData* NaniteTriangles = nullptr);

} // namespace CSMeshGenInternal

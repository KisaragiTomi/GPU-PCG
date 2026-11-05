#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "UDynamicMesh.h"
#include "ComputeShaderDebugParams.h"
#include "CSBoxSceneCollection.h"
#include "CSGpuMemoryBudget.h"
#include "CSGpuTriangleUtilities.h"
#include "ComputeShaderMeshGenerator.generated.h"

class AActor;
class ALandscape;
class UCSMesh;
class UCSMeshRenderComponent;
struct FCSGpuDebugPooledSource;
class UHierarchicalInstancedStaticMeshComponent;
class UMaterialInterface;
class AStaticMeshActor;
class UStaticMesh;
class UStaticMeshComponent;

class AComputeShaderMeshGenerator;

DECLARE_MULTICAST_DELEGATE_OneParam(FCSInstanceBrushEditorRequest, AComputeShaderMeshGenerator*);

// Triangle-soup 材质 id 的"无材质"哨兵值（如地形/CPU 三角形）。soup 材质 buffer 会预清成该值，
// GPU extract 只对 static mesh 三角形写入真实 registry id，因此地形/未写入的三角保持无材质。
inline constexpr uint32 CS_NO_MATERIAL_ID = 0xFFFFFFFFu;

// -----------------------------------------------------------------------------
// Core Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType, meta = (DisplayName = "CS Triangle Mesh Data"))
struct COMPUTESHADERGENERATOR_API FCSTriangleMeshData
{
	GENERATED_BODY()
public:
	// GPU readback 后的 compact vertex buffer。xyz 是顶点位置。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Vertices;

	// 有效 vertex 数。小于 0 时使用 Vertices.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VertexCount = -1;

	// 可选 index buffer。每 3 个 index 组成一个 triangle。
	// 如果为空，则 Vertices 会按 triangle soup 解释：0/1/2, 3/4/5, ...
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<int32> Indices;

	// 有效 index 数。小于 0 时使用 Indices.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 IndexCount = -1;

	// 可选 vertex normal。若 bRecomputeNormals 为 true，下游 DynamicMesh 可忽略它并重算法线。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> VertexNormals;
};

USTRUCT(BlueprintType, meta = (DisplayName = "CS Static Mesh Triangle Request"))
struct COMPUTESHADERGENERATOR_API FCSStaticMeshTriangleRequest
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	UStaticMesh* StaticMesh = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 LODIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FTransform LocalToWorld = FTransform::Identity;

	// 可选包围盒。有效时作为粗筛；无效时不按 Bounds 筛选。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FBox WorldBounds = FBox(ForceInit);

	// 生成 Request 的来源 Actor。用于在 RDG 三角形提取阶段排除自身或指定 Tag 的 Actor。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	AActor* SourceActor = nullptr;

	// Optional originating component. Used to preserve component-instance painted vertex colors.
	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh")
	TObjectPtr<UStaticMeshComponent> SourceComponent = nullptr;

	// 来源 component 的材质槽（override-aware，来自 Component->GetMaterial(i)）。
	// FStaticMeshSection.MaterialIndex 索引进本数组，用于把每个三角形映射回其源材质。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<TObjectPtr<UMaterialInterface>> MaterialSlots;

	/**
	 * 参照网格（场景里给 actor 或 component 打 "Ref" 标签）：只提供体积信息，不产出几何。
	 * 用途是拿一个代理体去决定哪些面被埋住，而不在它的表面上切开真实几何。
	 *
	 *   参与：soup、LBVH、fast-winding 场、inside/outside 判定、**射线遮挡**
	 *   不参与：tri-tri 切分（既不被切也不切别人）、BSP 输出
	 *
	 * 射线遮挡这条是有意为之：参照体既然能让 winding 判定认为某处是实心的，就也该挡住
	 * 可见性射线，否则会出现「winding 说被埋住、射线却说看得见」的自相矛盾，被埋的面又被
	 * 救回来。改动点见 MeshBoolean.usf 的 OccluderVerts 注释。
	 *
	 * 注意参照体仍占 soup / LBVH / winding 场的显存，并计入三角容量上限。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	bool bIsReference = false;
};

USTRUCT(BlueprintType, meta = (DisplayName = "CS Surface Voxel Data"))
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelData
{
	GENERATED_BODY()
public:
	// GPU 生成的 surface voxel 中心点。每个 voxel 只表示一个表面面片，不生成封闭 cube。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Positions;

	// 与 Positions 一一对应的表面法线；用于后续生成开放 mesh 的面朝向。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> Normals;

	// 有效 voxel 数。小于 0 时使用 Positions.Num()。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	int32 VoxelCount = -1;

	// 生成 voxel 时使用的 cell size，后续转 mesh 时可作为默认面片大小。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	float VoxelSize = 0.0f;

	// 体素整数网格坐标，与 Positions 一一对应（-1 索引为无效）。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FIntVector> Cells;

	// 面积加权质心（target position），与 Positions 一一对应。用于更精确的表面匹配。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	TArray<FVector> TargetPositions;

	// 体素网格的世界空间原点（与 Cells 坐标系对应）。
	// Cell (cx, cy, cz) 的世界空间中心 = VoxelOrigin + (Cell + 0.5) * VoxelSize。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh")
	FVector VoxelOrigin = FVector::ZeroVector;
};

// FCSBoxScenePreparedData 与它的收集入口已搬到 CSBoxSceneCollection.h：盒内场景提取不需要
// 本 actor，把它留在这个头里就等于要求调用方先有一个 generator 实例。

struct COMPUTESHADERGENERATOR_API FCSStaticMeshTriangleRDGOutput
{
	// Triangle soup: 每个 triangle 占 3 个 float4 vertex，vertex.w = 1。
	FRDGBufferRef TriangleVertices = nullptr;
	FRDGBufferUAVRef TriangleVerticesUAV = nullptr;
	FRDGBufferSRVRef TriangleVerticesSRV = nullptr;

	// 与 TriangleVertices 一一对应；每个 vertex 存 triangle normal，normal.w = 0。
	FRDGBufferRef TriangleNormals = nullptr;
	FRDGBufferUAVRef TriangleNormalsUAV = nullptr;
	FRDGBufferSRVRef TriangleNormalsSRV = nullptr;

	// Counter[0] = 实际写入的 triangle 数；有效 vertex 数 = Counter[0] * 3。
	FRDGBufferRef TriangleCounter = nullptr;
	FRDGBufferUAVRef TriangleCounterUAV = nullptr;
	FRDGBufferSRVRef TriangleCounterSRV = nullptr;

	// 每个 triangle 一个 uint 材质 id（与 TriangleVertices 平行，按 triangle 而非 vertex 索引）。
	// 值为 FCSBoxScenePreparedData 材质表下标；CS_NO_MATERIAL_ID 表示无材质（如地形）。
	// 由 GPU 在与顶点相同的 atomic slot 上写入，故 readback 后可按 soup 三角序号取回材质。
	FRDGBufferRef TriangleMaterialIds = nullptr;
	FRDGBufferUAVRef TriangleMaterialIdsUAV = nullptr;
	FRDGBufferSRVRef TriangleMaterialIdsSRV = nullptr;

	// 逐三角参照标志（1=参照体）。参照三角进 LBVH 与 winding 场，但 tri-tri 与 BSP 跳过。
	FRDGBufferRef TriangleReferenceFlags = nullptr;
	FRDGBufferUAVRef TriangleReferenceFlagsUAV = nullptr;
	FRDGBufferSRVRef TriangleReferenceFlagsSRV = nullptr;

	// 与 TriangleVertices 一一对应（按 vertex 索引，3 per triangle）：每顶点 UV0（float2）。
	// 由 GPU extract 在与顶点相同的 atomic slot 写入；无 UV 的源（地形/未绑定）保持 (0,0)。
	// 按通道交错存放：UV[Corner * NumUVChannels + Channel]。通道数取自源模型，
	// 源只有 1 条 UV 时就是 1，退化成原来的逐角点单 UV 布局。
	FRDGBufferRef TriangleUVs = nullptr;
	FRDGBufferUAVRef TriangleUVsUAV = nullptr;
	FRDGBufferSRVRef TriangleUVsSRV = nullptr;
	int32 NumUVChannels = 1;

	// Per-corner source vertex attributes, parallel to TriangleVertices.
	FRDGBufferRef TriangleColors = nullptr;
	FRDGBufferUAVRef TriangleColorsUAV = nullptr;
	FRDGBufferSRVRef TriangleColorsSRV = nullptr;
	FRDGBufferRef TriangleTangents = nullptr;
	FRDGBufferUAVRef TriangleTangentsUAV = nullptr;
	FRDGBufferSRVRef TriangleTangentsSRV = nullptr;
	FRDGBufferRef TriangleBiTangents = nullptr;
	FRDGBufferUAVRef TriangleBiTangentsUAV = nullptr;
	FRDGBufferSRVRef TriangleBiTangentsSRV = nullptr;

	uint32 MaxTriangles = 0;
	uint32 MaxVertices = 0;

	// 保持外部 RHI SRV 引用直到 GraphBuilder.Execute()，避免 RDG pass 执行前被释放。
	TArray<FShaderResourceViewRHIRef> ReferencedIndexBufferSRVs;
};

// GPU-resident surface voxels retained across the game/render boundary. The surface
// voxelizer (GetBoxSceneFilteredSurfaceVoxels) already builds these on the GPU as
// pooled buffers; retaining them here lets a consumer register them directly
// (RegisterExternalBuffer) instead of the readback -> repack -> re-upload round-trip.
// Layout matches what the vine VVVoxel projection consumes: float4 positions/normals/
// target-positions, int4 cells, uint hash slots. Counts/params come with them so no
// per-consume readback is needed. IsValid() means the buffers are usable.
struct COMPUTESHADERGENERATOR_API FCSSurfaceVoxelGPUBuffers
{
	TRefCountPtr<FRDGPooledBuffer> Positions;       // float4 (xyz center, w=1)
	TRefCountPtr<FRDGPooledBuffer> Normals;         // float4 (xyz normal, w=0), blur-resolved
	TRefCountPtr<FRDGPooledBuffer> TargetPositions; // float4 (xyz clipped centroid, w=1), blur-resolved
	TRefCountPtr<FRDGPooledBuffer> Cells;           // int4 (grid xyz, w=0)
	TRefCountPtr<FRDGPooledBuffer> Counter;         // uint2 ([0]=valid count, [1]=dropped count)
	// Two-buffer cell hash (producer format, StaticMeshPointSampler.usf): HashSlots[slot] =
	// HashCell(cell)+1 (key), HashIndices[slot] = voxelIndex+1. Probe (Hash+Probe*1103515245)%Count.
	TRefCountPtr<FRDGPooledBuffer> HashSlots;        // uint keys
	TRefCountPtr<FRDGPooledBuffer> HashIndices;      // uint voxelIndex+1 parallel to HashSlots
	int32 VoxelCapacity = 0;
	int32 VoxelCount = 0;
	uint32 HashSlotCount = 0;
	float VoxelSize = 0.0f;
	FVector VoxelOrigin = FVector::ZeroVector;
	FBox WorldBounds = FBox(ForceInit);
	bool IsValid() const { return Positions.IsValid() && Normals.IsValid() && TargetPositions.IsValid() && Cells.IsValid() && Counter.IsValid() && VoxelCapacity > 0; }
	void Reset() { *this = FCSSurfaceVoxelGPUBuffers(); }
};

// -----------------------------------------------------------------------------
// Core System - Generated Data Cache
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorTriangleTextureDataHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 VertexCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 TriangleCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 IndexCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	float ReferenceFilterDistance = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FBox SourceWorldBounds = FBox(ForceInit);

	// One RGBA32f texel per triangle-soup vertex. xyz = world position, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT = nullptr;

	// One RGBA32f texel per triangle-soup vertex. xyz = normal, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT = nullptr;

	// Small metadata texture. Pixel 0 = counts/filter, pixels 1-3 = bounds/dimensions.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> TriangleMetaRT = nullptr;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorSurfaceVoxelTextureDataHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	bool bValid = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	int32 VoxelCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	float VoxelSize = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FVector VoxelOrigin = FVector::ZeroVector;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FBox SourceWorldBounds = FBox(ForceInit);

	// One RGBA32f texel per sampled surface voxel. xyz = voxel center, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelPositionRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = blended normal, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelNormalRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = weighted surface target, w = 1.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelTargetRT = nullptr;

	// One RGBA32f texel per sampled surface voxel. xyz = integer voxel cell encoded as floats, w = 0.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelCellRT = nullptr;

	// Small metadata texture. Pixel 0 = counts/size, pixels 1-4 = origin/bounds/dimensions.
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT = nullptr;
};

// -----------------------------------------------------------------------------
// Dirty Cache Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshGeneratorVoxelGridSettings
{
	GENERATED_BODY()

	float VoxelSize = 100.0f;

	float ActivationRadius = 200.0f;

	int32 MaxActiveVoxels = 4096;

	int32 MaxTrianglesPerVoxel = 256;

	int32 LODIndex = 0;

	float BoundsTolerance = 1.0f;

	int32 MaxCacheTextureDimension = 4096;
};

// -----------------------------------------------------------------------------
// Brush Data
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSInstancePaintComponentSlot
{
	GENERATED_BODY()

	TObjectPtr<UStaticMesh> Mesh = nullptr;

	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> Component = nullptr;
};

UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AComputeShaderMeshGenerator : public AActor
{
	GENERATED_BODY()

public:
	/** Creates the generator actor, scene root, bounds component, and GPU render defaults. */
	AComputeShaderMeshGenerator(const FObjectInitializer& ObjectInitializer);

	/** Delegating default constructor so subclasses can keep plain default constructors. */
	AComputeShaderMeshGenerator();

	// -------------------------------------------------------------------------
	// Core System
	// -------------------------------------------------------------------------

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<USceneComponent> SceneRoot;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UBoxComponent> GeneratorBounds;

	/** GPU 内容调试显示：体素方向以箭头网格画出，几何是一个 UCSMesh，绑在这个组件上。
	 *  以绝对（世界原点）变换渲染，世界空间数据 1:1 画出。
	 *
	 *  这里以前是 UCSDisplayComponent —— 一个自持 position-only 顶点工厂、按 PT_LineList /
	 *  PT_PointList 直接 indirect 绘制的专用组件。它和点集箭头是同一件事的两种做法，而箭头那条
	 *  （几何归 UCSMesh 所有）不会在每次渲染状态重建时重跑生成 compute，也能直接存盘，故统一到
	 *  这条。代价是线被膨胀成了有体积的箭头，顶点量高一个数量级；调试规模上不成问题。 */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UCSMeshRenderComponent> DisplayComponent;

	/** DisplayComponent 画的那份调试几何。Transient：GPU 数据不跨关卡加载，这个属性只是拿住对象
	 *  不让 GC 收走，不参与序列化。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> DebugDisplayMesh;

	/**
	 * SubmitBoxSceneTrianglesToRenderPipeline 提取出的常驻场景三角汤。
	 *
	 * 几何归这个网格对象所有，而不是归某个 scene proxy：渲染状态重建只是重新绑定缓冲，
	 * 不会像代理自持缓冲那样把整个场景提取重跑一遍；存盘也不再需要组件正在渲染它。
	 *
	 * Transient：GPU 数据不跨关卡重载存活，这个 UPROPERTY 只是在挡 GC。
	 */
	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UCSMesh> DirectGpuMesh;

	/** 画 DirectGpuMesh 的组件。数据是世界空间，故以绝对（世界原点）变换渲染，
	 *  本 Actor 的变换不会挪动几何。网格带 section 表时逐 section 用真实材质绘制。 */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator")
	TObjectPtr<UCSMeshRenderComponent> DirectMeshRenderComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator")
	FCSMeshGeneratorVoxelGridSettings VoxelGridSettings;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Reference Filter")
	TArray<FVector> ReferencePoints;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Scene Filter")
	TArray<FName> ExcludedActorTags = { TEXT("UA") };

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxTriangles = 20000000;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 MaxVoxels = 2000000;

	// -------------------------------------------------------------------------
	// GPU memory budget
	//
	// MaxTriangles above is a fixed authoring limit; it says nothing about the machine the
	// generator actually runs on. These settings drive the shared pre-flight check in
	// CSGpuMemoryBudget, which sizes the limit from the adapter's live free VRAM instead.
	// -------------------------------------------------------------------------

	/** Runs the shared VRAM pre-flight check before box-scene GPU pipelines start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bCheckGpuMemoryBudget = true;

	/** Fraction of the free VRAM a single operation is allowed to claim. The rest covers RDG
	 *  pooling, fragmentation and driver overhead, so values near 1.0 will crash before the
	 *  check ever fires. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget", meta = (ClampMin = "0.05", ClampMax = "1.0"))
	float GpuMemoryBudgetSafetyRatio = 0.7f;

	/** Asks for confirmation when the estimate exceeds the budget. False aborts (or proceeds,
	 *  per bProceedWhenGpuMemoryBudgetUnattended) without showing a dialog. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bPromptWhenExceedingGpuMemoryBudget = true;

	/** What to do when the budget is exceeded but no dialog can be shown (commandlet, unattended
	 *  run, or a non-game thread caller). Default aborts, so batch jobs fail loudly instead of
	 *  taking the machine down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Mesh Generator|Memory Budget")
	bool bProceedWhenGpuMemoryBudgetUnattended = false;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0.001"))
	float QuadScale = 1.0f;

	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh")
	float NormalOffsetScale = 0.0f;

	// -------------------------------------------------------------------------
	// Surface Voxel Blur — ResinRattan port
	// -------------------------------------------------------------------------

	/** Number of 3D mean-filter iterations applied after voxelization. 0 = disabled. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "0"))
	int32 SurfaceVoxelBlurIterations = 0;

	/** Neighbourhood radius for the 3D mean filter. 1 = 3x3x3 Moore neighbourhood. */
	UPROPERTY(BlueprintReadOnly, Category = "CS Mesh Generator|Mesh", meta = (ClampMin = "1"))
	int32 SurfaceVoxelBlurRadius = 1;

	// -------------------------------------------------------------------------
	// Core System - Generated Data Cache
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle LastTriangleTextureData;

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle LastSurfaceVoxelTextureData;

	// -------------------------------------------------------------------------
	// Debug System
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, BlueprintReadOnly, Category = "CS Mesh Generator|Generated Data|Debug")
	FCSSurfaceVoxelData LastSurfaceVoxelData;

	// GPU-resident mirror of LastSurfaceVoxelData: the same voxel buffers the readback
	// sources, kept pooled so a consumer can register them directly instead of the
	// readback -> repack -> re-upload round-trip. Populated alongside LastSurfaceVoxelData
	// by GetBoxSceneFilteredSurfaceVoxels. Not a UPROPERTY (holds render resources).
	FCSSurfaceVoxelGPUBuffers LastSurfaceVoxelGPUBuffers;

	/** 结果资产名尾部的稳定编号（YYMMDDHHMM）。-1 表示尚未生成，首次保存时由
	 *  EnsureGeneratorTimeCode() 赋值并随 actor 存盘，之后每次运行都复用。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Mesh Generator|Mesh|Debug")
	int64 GeneratorTimeCode = -1;

	// -------------------------------------------------------------------------
	// Core System - Scene Extraction and Mesh Output
	// -------------------------------------------------------------------------

	void BuildBoxSceneTriangleRequests(UWorld* World,
		const FBox& QueryBox,
		TArray<FCSStaticMeshTriangleRequest>& OutRequests);

	/**
	 * Fills the stateless collection's options with this actor's scene-extraction policy.
	 *
	 * The collection itself lives in CSBoxSceneCollection and needs no generator; what is
	 * genuinely the actor's own is which actor to skip (itself — a generator inside its own
	 * query box would extract the geometry it is about to replace), which tags to skip, which
	 * LOD the voxel grid works at, and the authored triangle ceiling. Those four are serialized
	 * UPROPERTYs, so this hands them over in one place instead of letting three call sites drift.
	 *
	 * Reference points are deliberately *not* prefilled: whether a run filters by distance is a
	 * per-call decision, and quietly inheriting the actor's list would re-introduce exactly the
	 * hidden input this split removes. An invalid QueryBox falls back to the generator bounds,
	 * which is the one piece of "where do I look" the actor still legitimately answers.
	 */
	FCSBoxSceneCollectOptions MakeBoxSceneCollectOptions(const FBox& QueryBox = FBox(ForceInit)) const;

	/** [render thread] 消费 CSBoxSceneCollection::CollectBoxSceneTriangles 的预备数据，在 GraphBuilder
	 *  上建出 triangle-soup buffer。只做 RHI/RDG 操作，不触碰 UObject，可安全在
	 *  ENQUEUE_RENDER_COMMAND lambda 内调用。 */
	static FCSStaticMeshTriangleRDGOutput AddPreparedBoxSceneTrianglesToRDG(
		FRDGBuilder& GraphBuilder,
		FRHICommandListImmediate& RHICmdList,
		const FCSBoxScenePreparedData& Prepared,
		const TCHAR* DebugName = TEXT("CS.BoxSceneTriangles"));

	/** Reads back box-scene triangles into a CPU FCSTriangleMeshData by dispatching
	 *  AddBoxSceneTrianglesToRDG on the render thread and copying the GPU triangle-soup buffer back.
	 *  ReferenceFilterDistance <= 0 (or empty ReferencePoints) keeps all triangles in the generator
	 *  bounds; otherwise triangles are GPU-filtered by distance to this actor's ReferencePoints.
	 *  Also refreshes LastTriangleTextureData. Blocks via FlushRenderingCommands. */
	FCSTriangleMeshData GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance = 200.0f);

	/** Converts an ALandscape::RenderHeightmap G16 output into the depth format (CameraHeight - WorldZ)
	 *  and merges it into an existing OutputHeightmap using min (higher terrain wins).
	 *  Runs entirely within the supplied FRDGBuilder; must be called on the render thread. */
	void ConvertLandscapeHeightmapToDepthRDG(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef LandscapeG16Texture,
		FRDGTextureRef OutputHeightmap,
		float CameraHeight,
		float LandscapeScaleZ,
		float LandscapeOriginZ);

	/** Captures the landscape heightmap using GeneratorBounds as the capture area.
	 *  bOutputWorldHeight=true  → RGBA16f with RGB=Normal, A=WorldZ (cm)
	 *  bOutputWorldHeight=false → Depth from CameraHeight (R channel)
	 *  If OutRT is null, auto-creates a temporary RT and draws DrawDebugPoint.
	 *  Iterates ALL ALandscape actors; supports multi-landscape merge and World Partition.
	 *  @param OutRT Output render target (null for debug mode)
	 *  @param bOutputWorldHeight true=Normal+WorldZ, false=Depth */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Heightmap")
	bool CaptureLandscapeHeightmap(UTextureRenderTarget2D* OutRT, bool bOutputWorldHeight = true);

	/** Explicit-parameter overload for CaptureLandscapeHeightmap(Depth mode).
	 *  Iterates ALL ALandscape actors and min-merges (highest terrain wins). */
	bool CaptureLandscapeHeightmapToDepth(
		FVector WorldCenter,
		float CaptureExtent,
		float CameraHeight,
		UTextureRenderTarget2D* OutDepthRT);

	/** Converts an ALandscape::RenderHeightmap G16 output into Normal+Height format
	 *  (RGBA: Normal.XYZ, WorldHeight_cm) via finite-difference normals.
	 *  When bMergeByMaxZ is true, only overwrites texels where the new worldZ exceeds the
	 *  existing .w value — used to composite multiple landscapes (output must be pre-cleared
	 *  with .w = -large for correct results).
	 *  Runs entirely within the supplied FRDGBuilder; must be called on the render thread. */
	static void ConvertLandscapeHeightmapToNormalHeightRDG(
		FRDGBuilder& GraphBuilder,
		FRDGTextureRef LandscapeG16Texture,
		FRDGTextureRef OutputNormalHeight,
		float LandscapeScaleZ,
		float LandscapeOriginZ,
		FVector2f TexelWorldSize,
		bool bMergeByMaxZ = false);

	/** Explicit-parameter overload for CaptureLandscapeHeightmap(WorldHeight mode). */
	bool CaptureLandscapeHeightmapGPU(
		FVector WorldCenter,
		float CaptureExtent,
		UTextureRenderTarget2D* OutNormalHeightRT);

	/** GPU triangle extraction from landscape heightmap.
	 *  Renders the landscape heightmap in GeneratorBounds, then a compute shader converts
	 *  each texel into 2 triangles (6 world-space vertices) in a StructuredBuffer.
	 *  Returns readback vertex data as FCSTriangleMeshData.
	 *  @param TextureSize Resolution of the intermediate heightmap (default 128) */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Heightmap")
	FCSTriangleMeshData CaptureLandscapeTrianglesGPU(int32 TextureSize = 128);

	/** Generic: rasterize an indexed GPU triangle mesh into OutHeightmap via top-down orthographic
	 *  projection (texel.x = CameraHeight - WorldZ). A fused kernel (IndexedMeshToHeightmapCS) reads
	 *  the position/index buffers and rasterizes directly — no intermediate triangle soup. Adds
	 *  passes to GraphBuilder; the caller executes. Must be called on the render thread.
	 *  @param PositionSRV      Buffer<float> SRV, xyz per vertex (stride 3 floats).
	 *  @param IndexSRV         Buffer<uint>  SRV, triangle-list indices.
	 *  @param TriangleCapacity Number of triangles to process (index_count / 3); extra degenerate
	 *                          triangles from unused capacity rasterize to nothing.
	 *  @param LocalToWorld     Transforms the (local-space) positions to world.
	 *  @param OutHeightmap     UAV-capable float heightmap (RDG-registered). */
	void RasterizeIndexedMeshToHeightmapRDG(
		FRDGBuilder& GraphBuilder,
		FRHIShaderResourceView* PositionSRV,
		FRHIShaderResourceView* IndexSRV,
		uint32 TriangleCapacity,
		const FMatrix44f& LocalToWorld,
		FRDGTextureRef OutHeightmap,
		const FBox& WorldBounds,
		float CameraHeight);

	/** Static utility: renders a landscape heightmap via ALandscape::RenderHeightmap (GPU)
	 *  and converts to Normal+Height (RGB=Normal, A=WorldHeight_cm) in the given RT.
	 *  Does NOT require an AComputeShaderMeshGenerator instance.
	 *  @param Landscape The landscape actor to capture
	 *  @param WorldCenter Center of the capture area
	 *  @param WorldExtentXY Half-size of the capture area (only XY used)
	 *  @param OutNormalHeightRT Output render target (RGBA16f/RGBA32f, bCanCreateUAV=true) */
	static bool RenderLandscapeToNormalHeightRT(
		ALandscape* Landscape,
		FVector WorldCenter,
		FVector WorldExtentXY,
		UTextureRenderTarget2D* OutNormalHeightRT);

	/** Converts the latest bounded scene surface voxels into an open quad-strip GPU mesh.
	 *  Returns the mesh only: this actor owns no render component for it, so drawing it is the
	 *  caller's job (UCSMeshRenderComponent::SetGpuMesh). A null TargetMesh allocates one under
	 *  this actor; anything that needs a UDynamicMesh runs the result through
	 *  UCSMeshOps::CopyToDynamicMesh, which is the only conversion out of the GPU pipeline. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UPARAM(DisplayName = "Target") UCSMesh* SurfaceVoxelsToOpenGpuMesh(UCSMesh* TargetMesh,
		float VoxelSize = 10.0f,
		bool bReverseOrientation = false,
		bool bRecomputeNormals = false);

	/** Converts bounded scene surface voxels into a VDB-style meshed surface GPU mesh.
	 *  Returns the mesh only, see SurfaceVoxelsToOpenGpuMesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UPARAM(DisplayName = "Target") UCSMesh* SurfaceVoxelsToVDBGpuMesh(UCSMesh* TargetMesh,
		float VoxelSize = 10.0f,
		float RadiusMult = 2.0f,
		bool bRecomputeNormals = true);

	/** Rasterizes world-space particles into an OpenVDB level set and meshes the isosurface into
	 *  TargetMesh, replacing its contents. Static because no generator state takes part — the
	 *  voxel source (this actor's readback, or a caller's own FCSSurfaceVoxelData) is the only
	 *  difference between the callers, and duplicating the OpenVDB setup per caller is how the
	 *  two copies of it drifted apart in the first place.
	 *
	 *  bRecomputeNormals=false keeps VDB's per-face normals, which needs one vertex per corner;
	 *  true shares vertices and smooths across them. */
	static UCSMesh* VDBParticlesToGpuMesh(UCSMesh* TargetMesh,
		UObject* Outer,
		const TArray<FVector>& WorldPositions,
		float VoxelSize = 10.0f,
		float RadiusMult = 2.0f,
		bool bRecomputeNormals = true);

	/** Voxelizes filtered scene triangles and outputs world-space positions and normals.
	 *  If ReferenceFilterDistance is 0 or ReferencePoints is empty, voxelizes all triangles
	 *  within the box; otherwise only voxelizes triangles near reference points. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	void GetBoxSceneFilteredSurfaceVoxels(float VoxelSize,
		float ReferenceFilterDistance,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals);

	/** Builds and retains bounded scene surface voxels entirely on the GPU. The valid count remains
	 *  in FCSSurfaceVoxelGPUBuffers::Counter and is consumed by downstream compute/indirect draws. */
	bool PrepareBoxSceneSurfaceVoxelsGPU(float VoxelSize, float ReferenceFilterDistance = 0.0f);

	/** 只做体素化的 CPU 侧准备（收集并解析三角形请求、地形三角形、参数），不 dispatch 任何东西。
	 *  产出的 bundle 交给 AddCSSurfaceVoxelPasses 往调用者自己的 RDG 图里记录，于是体素能和下游
	 *  pass 合并进同一张图，省掉 PrepareBoxSceneSurfaceVoxelsGPU 那条路上的独立图与
	 *  FlushRenderingCommands。要 pooled buffer 或 CPU 回读的调用者仍应走旧接口。
	 *  返回 false 表示范围内没有可体素化的几何。 */
	bool PrepareSurfaceVoxelPassInputs(float VoxelSize, float ReferenceFilterDistance, struct FCSSurfaceVoxelPassInputs& OutInputs);

	/** Synchronously voxelizes the bounded scene surface and reads the voxels back to the CPU by
	 *  running the RDG surface-voxel pass on the render thread and blocking via FlushRenderingCommands.
	 *  Keeps all triangles within the generator bounds (no reference-point filtering). Refreshes the
	 *  cached LastSurfaceVoxelData / LastSurfaceVoxelTextureData and returns the sanitized voxel data. */
	FCSSurfaceVoxelData ReadbackBoxSceneSurfaceVoxelsSync(float VoxelSize);

	/** Returns the handle for the most recently stored triangle texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle GetLastTriangleTextureData() const { return LastTriangleTextureData; }

	/** Returns the handle for the most recently stored surface-voxel texture data. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle GetLastSurfaceVoxelTextureData() const { return LastSurfaceVoxelTextureData; }

	/** Rebuilds triangle data for the generator bounds and stores it in transient render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorTriangleTextureDataHandle UpdateBoxSceneTriangleTextureData(float ReferenceFilterDistance = 200.0f);

	/** Rebuilds surface-voxel data for the generator bounds and stores it in transient render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	FCSMeshGeneratorSurfaceVoxelTextureDataHandle UpdateBoxSceneSurfaceVoxelTextureData(float VoxelSize = 10.0f);

	/** Releases transient generated-data render targets and invalidates the cached handles. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data")
	void ClearGeneratedDataTextureCache();

	// -------------------------------------------------------------------------
	// Debug System
	// -------------------------------------------------------------------------

	/** Draws a debug arrow at each surface voxel, oriented along its normal, from the last retained
	 *  GPU surface voxels. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugLastSurfaceVoxelDirections(
		const FCSDebugLastVoxelDirectionOptions& Options = FCSDebugLastVoxelDirectionOptions());

	/** Regenerates bounded scene surface voxels and draws their normals as debug arrows. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugBoxSceneSurfaceVoxelDirections(
		const FCSDebugBoxVoxelDirectionOptions& Options);

	/**
	 * 把一组 GPU 常驻的点画成箭头（沿各自法线朝向），几何写进 DebugDisplayMesh、绑到
	 * DisplayComponent。这是 UCSDisplayComponent::ShowVoxelDirections 的替代：一个箭头同时表达
	 * 位置和方向，所以不再有独立的"点"通道。
	 *
	 * ArrowLength <= 0 时取 Source.ItemSize。Lifetime：<0 常驻，==0 下一帧清除，>0 按秒清除。
	 * 返回"最多会画几个"——真正画几个由 GPU 计数决定。
	 */
	int32 ShowDebugPointArrows(const FCSGpuDebugPooledSource& Source, float ArrowLength,
		FLinearColor ArrowColor, int32 MaxArrows, float Lifetime);

	/** 解绑调试几何并取消待触发的自动清除。以前是 UCSDisplayComponent::ClearDisplay。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Generated Data|Debug")
	void ClearDebugDisplay();

	/** Extracts and draws bounded scene surface triangles through a dedicated GPU component. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly, DisplayName = "Draw GPU Surface Triangles"))
	void DrawDebugBoxSceneSurfaceTrianglesGPU(float LifetimeSeconds = 10.0f);

	/** Deprecated compatibility entry point. This path does not spawn an actor or create a DynamicMesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly, DeprecatedFunction,
		DeprecationMessage = "Use DrawDebugBoxSceneSurfaceTrianglesGPU instead.", DisplayName = "Draw GPU Surface Triangles (Deprecated)"))
	void SpawnDebugSurfaceTrianglesDynamicMeshActor(float LifetimeSeconds = 10.0f);

	/** Clears all GPU-only MeshGenerator debug visualization. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Debug", meta = (DevelopmentOnly))
	void ClearMeshGeneratorGPUDebug();

	// -------------------------------------------------------------------------
	// Core System - Direct GPU Render (no readback, no DynamicMesh)
	// -------------------------------------------------------------------------

	/**
	 * Extracts the bounded scene surface triangles into DirectGpuMesh and draws them through
	 * DirectMeshRenderComponent, with vertex/index data living only on the GPU — no CPU readback and
	 * no UDynamicMesh.
	 *
	 * Replaces whatever the mesh already held (this is a submit, not an append). The extraction fills
	 * the per-triangle material-id stream and the mesh's material table, and the material sort runs
	 * afterwards, so the result draws with its real per-slot materials rather than one material for
	 * the whole scene. Material overrides the render component's own material, which is what the mesh
	 * falls back to while it carries no section table (null keeps the current one).
	 *
	 * MaxDirectTriangles bounds the allocation: it becomes the mesh's triangle ceiling, and the
	 * capacity is sized from it (the actual count is GPU-decided). ReferenceFilterDistance filters by
	 * distance to ReferencePoints when > 0 and ReferencePoints is non-empty.
	 *
	 * Blocking (scene walk, render flush, one counter readback). Returns false when there is no
	 * world/bounds/geometry — and because a submit replaces, a submit that finds nothing leaves the
	 * mesh cleared rather than leaving the previous scene on screen.
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	bool SubmitBoxSceneTrianglesToRenderPipeline(UMaterialInterface* Material = nullptr,
		int32 MaxDirectTriangles = 500000,
		float ReferenceFilterDistance = 0.0f);

	/** Releases DirectGpuMesh and stops drawing it. Unlike the display component's timed clear this
	 *  frees the VRAM: the allocation was sized by MaxDirectTriangles, so keeping it around after the
	 *  geometry is gone is pure waste. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	void ClearDirectGPUMesh();

	/** Saves DirectGpuMesh as a StaticMesh asset, material slots and all. Reads the mesh object back
	 *  directly, so it works whether or not the render component is currently drawing it.
	 *  Editor only; returns null otherwise. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	UStaticMesh* SaveDirectGPUMeshToStaticMesh(
		const FString& AssetPathAndName,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = true,
		bool bConvertToActorLocalSpace = true);

	// -------------------------------------------------------------------------
	// Core System - Result Asset Naming
	// -------------------------------------------------------------------------
	//
	// 结果资产的命名策略集中在这里，来自 CSSW 的烘焙流程：每个 actor 只认一个稳定编号，
	// 名字里不带每次运行的时间戳，因此同一个 actor 反复运行始终写同一个资产 —— 覆盖旧模型，
	// 而不是在 content browser 里堆出一串只差时间戳的副本。

	/** 返回本 actor 的稳定编号，首次调用时按当前时间生成一次并记入 GeneratorTimeCode。
	 *  懒生成而不是在构造函数里生成：构造期赋值会让 CDO 也带上一个编号，与 CDO 同值的实例不会被
	 *  delta 序列化，重新打开关卡后编号就变了 —— 编号一变，重跑就写出新资产而不是覆盖旧的。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	int64 EnsureGeneratorTimeCode();

	/** 结果资产所在文件夹：<关卡所在目录>/<GetResultAssetFolderName()>（关卡 /Game/Maps/L_Foo
	 *  -> /Game/Maps/AutoResult）。关卡没有内容路径（未存盘的 /Temp 地图）时返回空串。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	FString GetResultAssetFolderPath() const;

	/** 结果资产完整路径 <文件夹>/SM_<基名>_<编号><NameSuffix>。同一个 actor 编号恒定，重跑即覆盖。
	 *  只有一个 actor 要同时产出多份互不覆盖的结果时才需要传 NameSuffix。
	 *  关卡没有内容路径时返回空串，调用方应据此退回 transient 或显式路径。 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	virtual FString BuildResultAssetPath(const FString& NameSuffix = TEXT(""));

	/**
	 * 生成一个承载 Mesh 的结果 StaticMeshActor，挂在本 actor 下。
	 *
	 * 生成器把结果存成 StaticMesh 后，通常还要在关卡里放一个挂在自己身上的
	 * StaticMeshActor 来承载它。这套生命周期对所有生成器都一样，故收在基类：
	 * **先清掉上一次生成的结果**，再打标签 / 设网格 / 按世界变换挂接 / 命名 / 标脏。
	 * 清场只认结果标签，用户自己挂上来的 actor 不受影响。
	 *
	 * InActorLabel 为空时用资产名。返回生成的 actor，失败返回 nullptr。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Mesh")
	AStaticMeshActor* SpawnAttachedResultActor(UStaticMesh* Mesh, const FString& InActorLabel);


	// -------------------------------------------------------------------------
	// Result asset naming policy (子类覆写点)
	// -------------------------------------------------------------------------

	/** 结果资产文件夹名，落在关卡同级。 */
	virtual FString GetResultAssetFolderName() const { return TEXT("AutoResult"); }

	/** 结果资产名里 SM_ 之后、编号之前的部分。默认用 actor 名（已是合法的包内对象名）。 */
	virtual FString GetResultAssetBaseName() const;

	/** 结果资产名尾部的稳定编号。子类若已有自己的持久化编号（如 CSSW 的 SWUniqueID），
	 *  覆写此函数即可沿用，已烘好的资产名不会变。 */
	virtual FString GetResultAssetUniqueTag();


	// -------------------------------------------------------------------------
	// Shared GPU triangle-soup algorithms
	// -------------------------------------------------------------------------

	/**
	 * Protected convenience entry point for derived generators that need spatial queries.
	 *
	 * The implementation is delegated to the stateless CSGpuTriangleUtilities module:
	 * inheritance provides discoverability to subclasses, while the actor remains free of
	 * render-resource ownership and Boolean-specific policy.
	 */
	static CSGpuTriangleUtilities::FTriangleLBVH AddTriangleLBVHToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		int32 TriangleCount,
		int32 SortElementCount,
		const FVector3f& AabbMin,
		const FVector3f& InvExtent);

	/**
	 * Builds fast-winding multipoles for a shared LBVH. The base class exposes only
	 * geometric preprocessing; derived classes retain their own iso thresholds and
	 * sampling rules because those values describe algorithm policy.
	 */
	static FRDGBufferRef AddFastWindingToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferSRVRef TriangleSoupSRV,
		const CSGpuTriangleUtilities::FTriangleLBVH& LBVH,
		int32 TriangleCount);

	/**
	 * Finds positional weld representatives on the GPU. Mesh-attribute merging and
	 * topology cleanup remain with the derived producer because different outputs have
	 * different seam, material, and winding requirements.
	 */
	static FRDGBufferRef AddVertexWeldToRDG(
		FRDGBuilder& GraphBuilder,
		FRDGBufferRef OutputTriangleSoup,
		FRDGBufferRef OutputTriangleCounter,
		int32 OutputTriangleCapacity,
		int32 SourceTriangleCapacity,
		const FVector3f& GridOrigin,
		float WeldDistance,
		FRDGBufferSRVRef TriangleFilter = nullptr,
		uint32 TriangleFilterMask = 0u);

	/**
	 * Pre-flight VRAM check for a box-scene pipeline, using this actor's budget settings.
	 *
	 * The cost model is the caller's: only the derived generator knows which buffers its own
	 * pass will allocate and at what multiplier. Everything device-dependent - free VRAM,
	 * the triangle estimate, the confirmation policy - is delegated to CSGpuMemoryBudget so
	 * every generator answers the question the same way.
	 *
	 * Must be called on the game thread, before any extraction work. Returns true to proceed.
	 */
	bool ConfirmGpuMemoryBudgetForBoxScene(
		const TCHAR* OperationName,
		const FBox& QueryBox,
		const CSGpuMemoryBudget::FTriangleSoupCostModel& Cost,
		bool bIncludeLandscape) const;

	// -------------------------------------------------------------------------
	// Core System - Lifecycle
	// -------------------------------------------------------------------------
	/** Shared surface-voxel producer. CPU output/readback is performed only when bReadbackToCPU is true. */
	void BuildBoxSceneFilteredSurfaceVoxels(float VoxelSize,
		float ReferenceFilterDistance,
		TArray<FVector>& OutPositions,
		TArray<FVector>& OutNormals,
		bool bReadbackToCPU);

	/** Releases transient GPU resources when the actor leaves play. */
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Returns the current GeneratorBounds component as a valid world-space box when possible. */
	FBox GetGeneratorBoundsWorldBox() const;
	/** Stores CPU triangle data into generated-data texture targets and updates LastTriangleTextureData. */
	void StoreTriangleTextureData(const FCSTriangleMeshData& TriangleData, float ReferenceFilterDistance, FBox SourceWorldBounds = FBox(ForceInit));
	/** Stores CPU surface-voxel data into generated-data texture targets and updates LastSurfaceVoxelTextureData. */
	void StoreSurfaceVoxelTextureData(const FCSSurfaceVoxelData& SurfaceVoxelData, FVector VoxelOrigin);
	/** Releases triangle generated-data textures and invalidates the triangle data handle. */
	void ClearTriangleTextureData();
	/** Releases surface-voxel generated-data textures and invalidates the surface-voxel data handle. */
	void ClearSurfaceVoxelTextureData();
	/** Gets or allocates a transient generated-data render target with the requested size. */
	UTextureRenderTarget2D* GetOrCreateGeneratedDataRenderTarget(TObjectPtr<UTextureRenderTarget2D>& RenderTarget, const TCHAR* BaseName, int32 Width, int32 Height);

private:
	/** Arms (or, for LifetimeSeconds <= 0, cancels) the auto-clear of DirectGpuMesh. The timer lives
	 *  on the actor because the mesh does — the display component's own timer only ever governed the
	 *  display, and there is no display holding this geometry any more. */
	void ScheduleDirectMeshClear(float LifetimeSeconds);

	FTimerHandle DirectMeshClearTimerHandle;

	/** 两个 DrawDebug*VoxelDirections 的共同体：把 LastSurfaceVoxelGPUBuffers 展开成箭头几何、
	 *  绑到 DisplayComponent 上、按 Options 排自动清除。两个 Options USTRUCT 字段同名同义，故用
	 *  duck-typed 模板；定义在 .cpp 里，只有那两个入口实例化它。 */
	template <typename TOptions>
	int32 DrawVoxelDirectionArrows(const TOptions& Options);

	/** ClearDebugDisplay 的定时器。语义沿用旧显示组件：<0 常驻，==0 下一帧清，>0 按秒清。 */
	void ScheduleDebugDisplayClear(float Lifetime);

	FTimerHandle DebugDisplayClearHandle;
};

#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "RenderGraphResources.h"
#include "RenderResource.h"
#include "RHIDefinitions.h"
#include "UObject/ObjectPtr.h"

class UMaterialInterface;
class FRDGBuilder;

// -----------------------------------------------------------------------------
// Shared GPU-mesh buffer-set types
//
// The GPU-resident mesh base (FCSGpuMeshSceneProxy / UCSGpuMeshComponent) owns a
// *descriptor-driven* buffer set: each vertex/index/indirect/aux buffer is described
// by an FCSGpuStreamDesc, and the base allocates, binds the vertex factory, and reads
// back the set by iterating the descriptors. Adding a new buffer later is one more
// AddStream(...) call in a leaf's RegisterStreams() — the alloc / VF-bind / readback /
// save code never has to change.
// -----------------------------------------------------------------------------

// Thin FRenderResource wrappers that expose a pooled buffer's RHI object as a vertex /
// index buffer for a vertex-factory stream. Shared by every GPU-resident path in the
// plugin: the descriptor-driven mesh base, and the debug geometry built by FCSGpuDebugDraw.
struct FCSPooledVertexBuffer final : public FVertexBuffer
{
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (Pooled.IsValid()) VertexBufferRHI = Pooled->GetRHI();
	}
	virtual void ReleaseRHI() override
	{
		VertexBufferRHI.SafeRelease();
	}
};

struct FCSPooledIndexBuffer final : public FIndexBuffer
{
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		if (Pooled.IsValid()) IndexBufferRHI = Pooled->GetRHI();
	}
	virtual void ReleaseRHI() override
	{
		IndexBufferRHI.SafeRelease();
	}
};

// What a stream is used for. Drives buffer usage flags and the vertex-factory binding.
enum class ECSGpuStreamRole : uint8
{
	Position,      // Buffer<float>,  ElementsPerUnit=3 (xyz)     -> VF PositionComponent
	TangentBasis,  // Buffer<uint>,   ElementsPerUnit=2 (TanX,TanZ) packed 8888 -> VF TangentBasisComponents[0/1]
	TexCoord,      // Buffer<float>,  ElementsPerUnit=2 (uv)       -> VF TextureCoordinates[TexCoordIndex]
	Color,         // Buffer<uint>,   ElementsPerUnit=1 (RGBA8)    -> VF ColorComponent
	Index,         // Buffer<uint>,   ElementsPerUnit=1            -> DrawDesc.IndexBuffer (IndexBuffer usage)
	IndirectArgs,  // Indirect buffer, ElementsPerUnit=5           -> DrawDesc.IndirectArgsBuffer (DrawIndexedIndirect)
	MeshCounters,  // Buffer<uint>,   ElementsPerUnit=2 ([0]=vertexCount, [1]=indexCount) -> readback count carrier
	AuxVertex,     // extension slot: per-vertex data with no built-in VF role
};

/**
 * One DrawIndexedIndirect arg set as the CPU last saw it: the three fields that describe WHICH
 * indices a draw covers, mirrored out of the GPU args buffer. (The other two args are the
 * instance count, which a cull pass owns, and StartInstanceLocation, which is always 0 here.)
 *
 * Exists solely to feed a shadow-depth batch, which cannot read the GPU args — see
 * FCSMeshResident::GetDrawArgs for the full reason. Any other consumer wants the args buffer.
 */
struct FCSGpuDrawArgs
{
	uint32 IndexCount = 0;       // IndexCountPerInstance
	uint32 FirstIndex = 0;       // StartIndexLocation
	int32 BaseVertexIndex = 0;   // BaseVertexLocation

	bool IsDrawable() const { return IndexCount >= 3; }
};

// How a stream's element count scales. PerVertex/PerIndex multiply the base
// VertexCapacity/IndexCapacity; Fixed uses ElementsPerUnit verbatim.
enum class ECSGpuCountSource : uint8
{
	PerVertex,
	PerIndex,
	Fixed,
	PerTriangle,   // IndexCapacity / 3. For per-face data such as the material registry id.
};

// Which FCSGpuMeshCPUData member(s) a readback stream fills. None = not part of the
// CPU mesh data (e.g. Color today, IndirectArgs, MeshCounters).
enum class ECSGpuMeshSemantic : uint8
{
	None,
	Position,      // -> Positions (float3 per vertex)
	TangentBasis,  // -> Normals + Tangents (two packed 8888 per vertex: [0]=TangentX, [1]=TangentZ/normal)
	TexCoord,      // -> TexCoords (float2 per vertex)
	Index,         // -> Indices (uint per index)
	Color,         // -> Colors (RGBA8 per vertex). Vertex colours existed on the GPU and rendered
	               //    fine, but were dropped on save because no stream carried them to the CPU.
	MaterialId,    // -> TriangleMaterialSlots (uint per triangle). Carries the registry id only;
	               //    the readback layer never resolves it to a UMaterialInterface.
};

// One buffer in the GPU-mesh set. Leaves push these in RegisterStreams(); the base
// consumes them for allocation, vertex-factory binding, and readback.
struct FCSGpuStreamDesc
{
	const TCHAR* DebugName = TEXT("CSGpuStream");
	ECSGpuStreamRole Role = ECSGpuStreamRole::AuxVertex;
	uint32 BytesPerElement = sizeof(uint32); // FRDGBufferDesc::CreateBufferDesc element stride
	uint32 ElementsPerUnit = 1;              // elements written per vertex/index (or the whole Fixed count)
	ECSGpuCountSource CountSource = ECSGpuCountSource::PerVertex;
	EPixelFormat SrvFormat = PF_Unknown;     // PF_Unknown => no manual-fetch SRV created
	EVertexElementType VfType = VET_None;    // VET_None => no vertex-factory stream
	uint8 TexCoordIndex = 0;                 // TexCoord role only (0 also drives LightMapCoordinate)
	ECSGpuMeshSemantic CpuSemantic = ECSGpuMeshSemantic::None;
	bool bReadback = false;                  // participates in the CPU mesh readback loop
};

// CPU snapshot produced only when explicitly saving a GPU mesh. Vertex arrays are sized
// to the GPU-decided vertex count; Indices to the index count (they differ for an indexed
// mesh such as a road, and are equal for a triangle soup). Extensible: extra readback
// streams whose CpuSemantic maps to a known member fill it here; unmapped aux streams can
// be surfaced later without breaking this contract.
struct FCSGpuMeshCPUData
{
	// Which space Positions/Normals/Tangents are expressed in. A single "convert to local"
	// bool could not express this: one leaf defaulted it to false because its data was already
	// local, another defaulted it to false because its data was world - opposite reasons, same
	// flag, and no way for the converter to tell them apart.
	enum class ESpace : uint8 { World, ComponentLocal };

	// Whether vertex-instance attributes are indexed by position or by index corner. GPU render
	// components normally use per-position data; boolean output uses per-corner data so welded
	// positions can still retain source UV/normal/colour seams. For a triangle soup both counts
	// are equal, which is exactly when inferring the layout from array sizes is ambiguous.
	enum class EAttrLayout : uint8 { PerVertex, PerCorner };

	/**
	 * UV 通道数上限 = 引擎的 `MAX_STATIC_TEXCOORDS`（`Components.h:22`），已经顶到天花板。
	 *
	 * ⚠️ 抬到 8 时**顶点声明侧跟不上**：`FStaticMeshDataType::TextureCoordinates` 是
	 * `TFixedAllocator<MAX_STATIC_TEXCOORDS / 2>`（= 4 个 stream component，`Components.h:46`），
	 * 第 5 组起只能走 manual fetch 的 SRV 路径。钳位与理由写在 `CSGpuMeshSceneProxy.cpp` 的
	 * `ECSGpuStreamRole::TexCoord` 分支里，本插件只面向 SM5+ ⇒ manual fetch 恒开。
	 */
	static constexpr int32 MaxTexCoordChannels = 8;

	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector3f> Tangents;

	/**
	 * 多条 UV 通道。通道 0 是主 UV（用 TexCoords() 取，与旧代码等价）；lightmap / 遮罩 /
	 * 世界投影等额外通道填 1..NumTexCoordChannels-1。只有前 NumTexCoordChannels 条会被
	 * 装配进 MeshDescription，后面的即使非空也会被忽略。
	 */
	TArray<FVector2f> TexCoordChannels[MaxTexCoordChannels];
	int32 NumTexCoordChannels = 1;

	TArray<FVector2f>& TexCoords() { return TexCoordChannels[0]; }
	const TArray<FVector2f>& TexCoords() const { return TexCoordChannels[0]; }

	TArray<FVector4f> Colors;
	TArray<float> BinormalSigns;
	TArray<uint32> Indices;
	TArray<int32> TriangleMaterialSlots;

	// Material table indexed by TriangleMaterialSlots. Kept with the data so producers do not
	// have to pass a parallel array alongside it to every conversion call. Empty slots are the
	// converter's problem (it substitutes the engine default surface material), not the
	// producer's.
	TArray<TObjectPtr<UMaterialInterface>> Materials;

	ESpace SourceSpace = ESpace::World;
	EAttrLayout AttrLayout = EAttrLayout::PerVertex;

	void Reset()
	{
		Positions.Reset();
		Normals.Reset();
		Tangents.Reset();
		for (TArray<FVector2f>& Channel : TexCoordChannels) Channel.Reset();
		NumTexCoordChannels = 1;
		Colors.Reset();
		BinormalSigns.Reset();
		Indices.Reset();
		TriangleMaterialSlots.Reset();
		Materials.Reset();
		SourceSpace = ESpace::World;
		AttrLayout = EAttrLayout::PerVertex;
	}

	// Vertex-instance attributes may be supplied per position or per index corner. GPU render
	// components normally use per-position data; boolean output uses per-corner data so welded
	// positions can still retain source UV/normal/color seams.
	bool IsValid() const
	{
		auto HasValidAttributeCount = [this](int32 Count)
		{
			return Count == Positions.Num() || Count == Indices.Num();
		};

		// 已声明的每条 UV 通道都必须与主通道等长，否则装配时会逐角点越界。
		const int32 ActiveChannels = FMath::Clamp(NumTexCoordChannels, 1, MaxTexCoordChannels);
		for (int32 Channel = 1; Channel < ActiveChannels; ++Channel)
		{
			if (TexCoordChannels[Channel].Num() != TexCoordChannels[0].Num()) return false;
		}

		return Positions.Num() >= 3
			&& Indices.Num() >= 3
			&& Indices.Num() % 3 == 0
			&& HasValidAttributeCount(Normals.Num())
			&& HasValidAttributeCount(Tangents.Num())
			&& HasValidAttributeCount(TexCoords().Num())
			&& (Colors.IsEmpty() || HasValidAttributeCount(Colors.Num()))
			&& (BinormalSigns.IsEmpty() || HasValidAttributeCount(BinormalSigns.Num()))
			&& (TriangleMaterialSlots.IsEmpty() || TriangleMaterialSlots.Num() == Indices.Num() / 3);
	}
};

// -----------------------------------------------------------------------------
// Stream-set services shared by every producer of this buffer set: the descriptor
// list itself, and the per-role access state a *persistent* buffer must be left in.
//
// Both used to be copy-pasted per producer. The access-state block in particular was
// written out by hand in five places, and getting it wrong is the nastiest failure
// mode in this system: RDG's default epilogue state (SRVMask) is illegal for index /
// indirect usage, so a stream left in it silently stops drawing or reading back after
// whichever pass touched it last — with no error anywhere.
// -----------------------------------------------------------------------------

namespace CSGpuMeshStreams
{
	/** Which extras the standard triangle set carries beyond the seven core streams. */
	struct FStandardStreamOptions
	{
		/** DrawIndexedIndirect arg sets (5 uints each) in the IndirectArgs buffer. */
		uint32 NumIndirectDraws = 1;

		/** Add a per-triangle material-registry id stream (readback semantic MaterialId).
		 *  Off for the proxy-owned leaves: none of them fills such a buffer, and an
		 *  unfilled readback stream would hand the saver garbage material slots. */
		bool bMaterialIds = false;

		/** Carry vertex colours through the CPU readback. Colours have always existed on
		 *  the GPU and rendered fine; only the readback dropped them. */
		bool bReadbackColors = false;

		/**
		 * UV 组数（UV0..UVn-1）。**这是逐 mesh 的变体旋钮,不是全局设置。**
		 *
		 * 为什么必须做成变体：这条 TexCoord 流是全插件共用的（道路 / 藤蔓 / 布尔产物 / 直接
		 * 网格 / 地面都在用），把它一刀切成 4 float 等于让每个 gpumesh 的 UV 显存翻倍 ——
		 * 1024² 地面单这一张就多 8.4 MB，而需要第二组 UV 的只有房子墙。默认 1 时产出的
		 * 描述符与改动前**逐位相同**，所有既有 mesh 和写 TexCoord 的 7 个 .usf 一行都不用动
		 * （它们的 `RW_TexCoords[V]` 对单组 mesh 依然正确）。
		 *
		 * 布局是**交错**的而不是另起一条流，这一条没得选：引擎 LocalVertexFactory.ush:734 的
		 * `VertexFetch_TexCoordBuffer[NumFetchTexCoords * VertexId + CoordIndex]` 是单 buffer
		 * 交错取数，两条独立 buffer 物理上表达不了；而代理里 `TextureCoordinatesSRV` 也只有
		 * 一个槽位，第二条流会直接覆盖第一条。所以第二组 UV 的正确形态是把**同一条流**加宽到
		 * 每顶点 2×N 个 float，SrvFormat 保持 PF_G32R32F（float2 视图，元素数自然翻倍）。
		 */
		uint32 NumTexCoordSets = 1;
	};

	/** Fills OutDescs with the standard triangle set: Position / TangentBasis / TexCoord0 /
	 *  Color / Index / IndirectArgs / MeshCounters, plus whatever Options asks for. */
	COMPUTESHADERGENERATOR_API void BuildStandardTriangleStreamDescs(
		TArray<FCSGpuStreamDesc>& OutDescs, const FStandardStreamOptions& Options = FStandardStreamOptions());

	/** How many units a stream of this CountSource covers. The one place PerTriangle is
	 *  resolved: allocation and readback used to compute it inline and neither handled it,
	 *  so a per-face stream would have been sized as if it were per-vertex. */
	COMPUTESHADERGENERATOR_API uint32 UnitsForCountSource(
		ECSGpuCountSource CountSource, uint32 VertexUnits, uint32 IndexUnits);

	/** The access state a persistent stream of this role must be left in at the end of
	 *  every pass sequence that writes it. Any operator that skips this leaves the mesh
	 *  undrawable / unreadable until something else happens to transition it back. */
	COMPUTESHADERGENERATOR_API ERHIAccess FinalAccessForRole(ECSGpuStreamRole Role);

	/** SetBufferAccessFinal(Buffer, FinalAccessForRole(Role)). Null buffers are ignored so
	 *  callers can pass optional streams straight through. */
	COMPUTESHADERGENERATOR_API void SetStreamAccessFinal(
		FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, ECSGpuStreamRole Role);

}

// -----------------------------------------------------------------------------
// 「GPU 网格快照 -> StaticMesh」的转换/落盘选项。实现是 UCSGpuMeshComponent 的
// 静态成员（原 namespace CSGpuMeshConvert，2026-08 并入基类）。
// -----------------------------------------------------------------------------

/** 属性装配选项。退化面阈值等判据统一在此，避免各路径各自为政。 */
struct FCSGpuMeshConvertOptions
{
	/** 目标 actor/组件变换。bBakeToLocalSpace 为真时把世界空间数据烘到它的局部空间。 */
	FTransform TargetTransform = FTransform::Identity;

	/** 源数据是世界空间、需要烘到 TargetTransform 的局部空间。源数据本就是局部空间时置 false。 */
	bool bBakeToLocalSpace = true;

	// 这里曾有 bRecomputeNormals 与 DegenerateAreaThresholdSq 两个字段，全项目无人读取：
	// 法线一律取自回读快照，退化面判据写死在 BuildGpuMeshDescription 里的 FaceNormal.IsNearlyZero()。
	// 留着只会让人以为它们可调。（BP 面的 FCSMeshToStaticMeshOptions::bRecomputeNormals 是另一个
	// 字段，DynamicMesh 那条出口确实在读，不受影响。）

	/** 材质槽为空时填入引擎默认表面材质，避免输出网格渲染成默认灰且无法在编辑器里区分槽位。 */
	bool bFillEmptySlotsWithDefaultMaterial = true;
};

/** 落盘选项。空 AssetPath 表示用 OwnerActor 所在 level 旁的 result 目录。 */
struct FCSGpuMeshAssetOptions
{
	FString AssetPath;
	bool bTransient = false;
	bool bReplaceExisting = true;
	bool bSaveToDisk = false;

	/**
	 * 产出的 StaticMesh 是否启用 Nanite。布尔结果动辄上百万三角，正是 Nanite 的适用场景：
	 * 开启后由 Nanite 自己做 LOD 与剔除，省掉手工 LOD，渲染开销与三角数基本脱钩。
	 * 代价是构建时会多一步 Nanite 数据生成（大网格上是秒级），且资产体积变大。
	 * 必须在 BuildFromMeshDescriptions 之前设置，否则这次构建不会产出 Nanite 数据。
	 */
	bool bEnableNanite = false;
};

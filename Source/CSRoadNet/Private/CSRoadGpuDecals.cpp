#include "CSRoadGpuDecals.h"

#include "CSRoadGpuShaders.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"

#include <atomic>

static_assert(sizeof(CSRoad::FGpuDecalItem) == 34 * sizeof(uint32), "FGpuDecalItem must match CSRoadDecal.usf FDecalItem");
static_assert(sizeof(CSRoad::FGpuReceiverSegment) == 12 * sizeof(uint32), "FGpuReceiverSegment must match CSRoadDecal.usf FReceiverSegment");
static_assert(sizeof(CSRoad::FGpuDecalStats) <= 4 * sizeof(uint32), "FGpuDecalStats must fit the stats buffer");
static_assert(CSRoad::FGpuDecalInput::ReceiverChunk == uint32(CSRoad::GpuDetail::GroupSize), "CSRoadDecal.usf boxes receiver chunks of THREADGROUP_SIZE");

BEGIN_SHADER_PARAMETER_STRUCT(FCSRoadDecalParameters, )
	SHADER_PARAMETER(uint32, NumSegments)
	SHADER_PARAMETER(uint32, SegmentGroupsX)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FReceiverSegment>, Segments)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMeshInstance>, MeshInstances)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, ReceiverNodes)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ReceiverSlotsRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ReceiverSlots)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, SegmentCountsRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, SegmentCounts)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, ChunkBoxesRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ChunkBoxes)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RoadPositions)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RoadTangents)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RoadIndices)
	SHADER_PARAMETER(FMatrix44f, WorldToLocal)
	SHADER_PARAMETER(uint32, NumItems)
	SHADER_PARAMETER(uint32, ItemGroupsX)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FDecalItem>, Items)
	SHADER_PARAMETER(uint32, NumClipped)
	SHADER_PARAMETER(uint32, ClippedGroupsX)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, ClippedItems)
	SHADER_PARAMETER(uint32, ClipMode)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, ClipCountsRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, StatsRW)
	SHADER_PARAMETER(uint32, ForceFallback)
	SHADER_PARAMETER(uint32, OutputMode)
	SHADER_PARAMETER(FMatrix44f, LocalToWorld)
	SHADER_PARAMETER(uint32, VertexCapacity)
	SHADER_PARAMETER(uint32, IndexCapacity)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_TexCoords)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, DebugVertices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, FitHeightsRW)
END_SHADER_PARAMETER_STRUCT()

#define CSROAD_DECAL_SHADER(ShaderClass, EntryPoint) \
	class ShaderClass : public FGlobalShader \
	{ \
	public: \
		using FParameters = FCSRoadDecalParameters; \
		DECLARE_GLOBAL_SHADER(ShaderClass); \
		SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader); \
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
		{ \
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
		} \
		static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
		{ \
			FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); \
			OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), CSRoad::GpuDetail::GroupSize); \
		} \
	}; \
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSRoadDecal.usf", EntryPoint, SF_Compute)

CSROAD_DECAL_SHADER(FCSRoadReceiversCS, "ReceiversCS");
CSROAD_DECAL_SHADER(FCSRoadClipCS, "ClipCS");
CSROAD_DECAL_SHADER(FCSRoadDecalGridCS, "DecalGridCS");
CSROAD_DECAL_SHADER(FCSRoadDecalFitCS, "DecalFitCS");

namespace CSRoad
{
namespace
{
FIntVector CSRoadGpuDecals_Groups(uint32 Count)
{
	return FIntVector(int32(FMath::Clamp(Count, 1u, 65535u)), int32(FMath::DivideAndRoundUp(FMath::Max(Count, 1u), 65535u)), 1);
}

template<typename ShaderType>
void CSRoadGpuDecals_AddPass(FRDGBuilder& GraphBuilder, const FCSRoadDecalParameters& Common, uint32 Count, FRDGEventName&& Name)
{
	if (Count == 0) return;
	FCSRoadDecalParameters* Parameters = GraphBuilder.AllocParameters<FCSRoadDecalParameters>();
	*Parameters = Common;
	TShaderMapRef<ShaderType> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters, CSRoadGpuDecals_Groups(Count));
}

// A buffer ReceiversCS fills: zeroed instead when there is no segment for it to run over, as the
// later passes still bind it.
template<typename T>
FRDGBufferRef CSRoadGpuDecals_Received(FRDGBuilder& GraphBuilder, const FGpuDecalInput& Decals, uint32 Count, const TCHAR* Name)
{
	if (Decals.Segments.Num() == 0) return GpuDetail::Zeroed<T>(GraphBuilder, Count, Name);
	return GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(T), FMath::Max(1u, Count)), Name);
}

FRDGBufferUAVRef CSRoadGpuDecals_TypedUAV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, EPixelFormat Format, const TCHAR* StandInName)
{
	if (!Buffer) Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), StandInName);
	return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, Format));
}

// What every decal pass binds: the receivers' layout and the road mesh streams.
FCSRoadDecalParameters CSRoadGpuDecals_Common(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road)
{
	FCSRoadDecalParameters Common;
	Common.NumSegments = uint32(Decals.Segments.Num());
	Common.SegmentGroupsX = uint32(CSRoadGpuDecals_Groups(Common.NumSegments).X);
	Common.Segments = GpuDetail::UploadSRV(GraphBuilder, Decals.Segments, TEXT("CSRoad.ReceiverSegments"));
	Common.MeshInstances = GpuDetail::UploadSRV(GraphBuilder, Mesh.Instances, TEXT("CSRoad.DecalMeshInstances"));
	Common.ReceiverNodes = GpuDetail::UploadSRV(GraphBuilder, Decals.ReceiverSegments, TEXT("CSRoad.ReceiverNodes"));
	Common.RoadPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Road.Positions, PF_R32_FLOAT));
	Common.RoadTangents = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Road.Tangents, PF_R32_UINT));
	Common.RoadIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Road.Indices, PF_R32_UINT));
	Common.WorldToLocal = Road.WorldToLocal;
	Common.NumItems = uint32(Decals.Items.Num());
	Common.ItemGroupsX = uint32(CSRoadGpuDecals_Groups(Common.NumItems).X);
	Common.Items = GpuDetail::UploadSRV(GraphBuilder, Decals.Items, TEXT("CSRoad.DecalItems"));
	Common.NumClipped = uint32(Decals.ClippedItems.Num());
	Common.ClippedGroupsX = uint32(CSRoadGpuDecals_Groups(Common.NumClipped).X);
	Common.ClippedItems = GpuDetail::UploadSRV(GraphBuilder, Decals.ClippedItems, TEXT("CSRoad.ClippedItems"));
	Common.ClipMode = 0u;
	Common.ForceFallback = Decals.bForceFallback ? 1u : 0u;
	Common.OutputMode = 0u;
	Common.LocalToWorld = FMatrix44f::Identity;
	Common.VertexCapacity = 0u;
	Common.IndexCapacity = 0u;
	Common.RW_Positions = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_FLOAT, TEXT("CSRoad.NoDecalPositions"));
	Common.RW_Tangents = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_UINT, TEXT("CSRoad.NoDecalTangents"));
	Common.RW_TexCoords = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_FLOAT, TEXT("CSRoad.NoDecalTexCoords"));
	Common.RW_Colors = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_UINT, TEXT("CSRoad.NoDecalColors"));
	Common.RW_Indices = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_UINT, TEXT("CSRoad.NoDecalIndices"));
	Common.RW_MaterialIds = CSRoadGpuDecals_TypedUAV(GraphBuilder, nullptr, PF_R32_UINT, TEXT("CSRoad.NoDecalMaterialIds"));
	Common.DebugVertices = GraphBuilder.CreateUAV(GpuDetail::Zeroed<float>(GraphBuilder, 1, TEXT("CSRoad.NoDecalDebug")));
	Common.FitHeightsRW = GraphBuilder.CreateUAV(GpuDetail::Zeroed<float>(GraphBuilder, 1, TEXT("CSRoad.NoFitHeights")));
	return Common;
}

// The road mesh into transient streams, as the decals read it (the tests' blocking runs).
FGpuRoadStreams CSRoadGpuDecals_RecordRoad(FRDGBuilder& GraphBuilder, const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh)
{
	const FGpuGeometryOutput GeometryOut = AddGeometryPasses(GraphBuilder, Geometry);
	FGpuRoadMeshTarget Target;
	Target.Positions = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), Mesh.NumVertices * 3u), TEXT("CSRoad.TestRoadPositions"));
	Target.Tangents = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Mesh.NumVertices * 2u), TEXT("CSRoad.TestRoadTangents"));
	Target.TexCoords = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), Mesh.NumVertices * 2u), TEXT("CSRoad.TestRoadTexCoords"));
	Target.Colors = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), Mesh.NumVertices), TEXT("CSRoad.TestRoadColors"));
	Target.Indices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, Mesh.NumIndices)), TEXT("CSRoad.TestRoadIndices"));
	Target.MaterialIds = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, Mesh.NumIndices / 3u)), TEXT("CSRoad.TestRoadMaterialIds"));
	Target.VertexCapacity = Mesh.NumVertices;
	Target.IndexCapacity = Mesh.NumIndices;
	AddRoadMeshPasses(GraphBuilder, GeometryOut, Mesh, Target);
	FGpuRoadStreams Road;
	Road.Positions = Target.Positions;
	Road.Tangents = Target.Tangents;
	Road.Indices = Target.Indices;
	return Road;
}
}

FGpuDecalReceivers AddDecalReceiverPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road)
{
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadDecalReceivers");
	FGpuDecalReceivers Out;
	Out.Slots = CSRoadGpuDecals_Received<uint32>(GraphBuilder, Decals, Decals.NumReceiverSlots, TEXT("CSRoad.ReceiverSlots"));
	Out.SegmentCounts = CSRoadGpuDecals_Received<uint32>(GraphBuilder, Decals, uint32(Decals.Segments.Num()), TEXT("CSRoad.SegmentCounts"));
	Out.ChunkBoxes = CSRoadGpuDecals_Received<FVector4f>(GraphBuilder, Decals, Decals.NumReceiverChunks(), TEXT("CSRoad.ReceiverChunkBoxes"));
	Out.ClipCounts = GpuDetail::Zeroed<uint32>(GraphBuilder, uint32(Decals.ClippedItems.Num()), TEXT("CSRoad.ClipCounts"));
	Out.Stats = GpuDetail::Zeroed<uint32>(GraphBuilder, 4, TEXT("CSRoad.DecalStats"));
	FCSRoadDecalParameters Common = CSRoadGpuDecals_Common(GraphBuilder, Mesh, Decals, Road);
	Common.ReceiverSlotsRW = GraphBuilder.CreateUAV(Out.Slots);
	Common.SegmentCountsRW = GraphBuilder.CreateUAV(Out.SegmentCounts);
	Common.ChunkBoxesRW = GraphBuilder.CreateUAV(Out.ChunkBoxes);
	CSRoadGpuDecals_AddPass<FCSRoadReceiversCS>(GraphBuilder, Common, Common.NumSegments, RDG_EVENT_NAME("Receivers"));

	// Fitted crosswalks are not clipped.
	if (Decals.bFitted) return Out;
	FCSRoadDecalParameters Count = CSRoadGpuDecals_Common(GraphBuilder, Mesh, Decals, Road);
	Count.ReceiverSlots = GraphBuilder.CreateSRV(Out.Slots);
	Count.SegmentCounts = GraphBuilder.CreateSRV(Out.SegmentCounts);
	Count.ChunkBoxes = GraphBuilder.CreateSRV(Out.ChunkBoxes);
	Count.ClipCountsRW = GraphBuilder.CreateUAV(Out.ClipCounts);
	Count.StatsRW = GraphBuilder.CreateUAV(Out.Stats);
	CSRoadGpuDecals_AddPass<FCSRoadClipCS>(GraphBuilder, Count, Count.NumClipped, RDG_EVENT_NAME("ClipCount"));
	return Out;
}

FRDGBufferRef AddDecalFitPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road, const FGpuDecalReceivers& Receivers)
{
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadDecalFit");
	FRDGBufferRef Heights = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), FMath::Max(1u, Decals.NumFitHeights)), TEXT("CSRoad.DecalFitHeights"));
	FCSRoadDecalParameters Common = CSRoadGpuDecals_Common(GraphBuilder, Mesh, Decals, Road);
	Common.ReceiverSlots = GraphBuilder.CreateSRV(Receivers.Slots);
	Common.SegmentCounts = GraphBuilder.CreateSRV(Receivers.SegmentCounts);
	Common.ChunkBoxes = GraphBuilder.CreateSRV(Receivers.ChunkBoxes);
	Common.StatsRW = GraphBuilder.CreateUAV(GpuDetail::Zeroed<uint32>(GraphBuilder, 4, TEXT("CSRoad.DecalFitStats")));
	Common.ClipCountsRW = GraphBuilder.CreateUAV(GpuDetail::Zeroed<uint32>(GraphBuilder, 1, TEXT("CSRoad.NoClipCounts")));
	Common.FitHeightsRW = GraphBuilder.CreateUAV(Heights);
	// A heights buffer the pass does not reach in full (no projected or clipped item) is still read back.
	if (Decals.NumFitHeights == 0) AddClearUAVPass(GraphBuilder, Common.FitHeightsRW, 0u);
	CSRoadGpuDecals_AddPass<FCSRoadDecalFitCS>(GraphBuilder, Common, Common.NumItems, RDG_EVENT_NAME("DecalFit"));
	return Heights;
}

bool RunDecalFitBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, TArray<float>& OutHeights, FString& OutError)
{
	if (!Decals.bFitted || Mesh.NeedsRefinementCounts() || Mesh.NumVertices == 0)
	{
		OutError = TEXT("The decals are not laid out for fitting, or the road mesh is empty or has no refinement counts");
		return false;
	}
	OutHeights.SetNumZeroed(int32(FMath::Max(1u, Decals.NumFitHeights)));
	const TArray<TPair<void*, uint32>> Targets = { { OutHeights.GetData(), uint32(OutHeights.Num() * sizeof(float)) } };
	if (!RunGraphBlocking([&](FRDGBuilder& GraphBuilder)
		{
			const FGpuRoadStreams Road = CSRoadGpuDecals_RecordRoad(GraphBuilder, Geometry, Mesh);
			const FGpuDecalReceivers Receivers = AddDecalReceiverPasses(GraphBuilder, Mesh, Decals, Road);
			return TArray<FRDGBufferRef>{ AddDecalFitPasses(GraphBuilder, Mesh, Decals, Road, Receivers) };
		}, Targets))
	{
		OutError = TEXT("The GPU decal fit readback did not complete");
		return false;
	}
	OutHeights.SetNum(int32(Decals.NumFitHeights));
	return true;
}

struct FDecalFitReadback::FShared
{
	FRHIGPUBufferReadback* Readback = nullptr;
	uint32 Bytes = 0;
	TArray<float> Data;
	// 0: on its way, 1: landed, 2: could not be read.
	std::atomic<int32> State { 0 };
	std::atomic<bool> bLooking { false };

	~FShared()
	{
		if (!Readback) return;
		// On the render thread, after any command that still reads it.
		ENQUEUE_RENDER_COMMAND(CSRoadDecalFitRelease)([Readback = Readback](FRHICommandListImmediate&) { delete Readback; });
	}
};

FDecalFitReadback::FDecalFitReadback()
	: Shared(MakeShared<FShared, ESPMode::ThreadSafe>())
{
}

FDecalFitReadback::~FDecalFitReadback() = default;

void FDecalFitReadback::Enqueue(FRDGBuilder& GraphBuilder, FRDGBufferRef Heights, uint32 NumHeights)
{
	Shared->Bytes = FMath::Max(1u, NumHeights) * uint32(sizeof(float));
	Shared->Readback = new FRHIGPUBufferReadback(TEXT("CSRoad.DecalFitReadback"));
	AddEnqueueCopyPass(GraphBuilder, Shared->Readback, Heights, Shared->Bytes);
}

bool FDecalFitReadback::Poll()
{
	if (Shared->State.load(std::memory_order_acquire) != 0) return true;
	if (Shared->bLooking.exchange(true)) return false;
	ENQUEUE_RENDER_COMMAND(CSRoadDecalFitPoll)([State = Shared](FRHICommandListImmediate&)
	{
		State->bLooking.store(false);
		if (State->State.load(std::memory_order_relaxed) != 0) return;
		if (!State->Readback)
		{
			State->State.store(2, std::memory_order_release);
			return;
		}
		if (!State->Readback->IsReady()) return;
		const void* Raw = State->Readback->Lock(State->Bytes);
		if (!Raw)
		{
			State->State.store(2, std::memory_order_release);
			return;
		}
		State->Data.SetNumUninitialized(int32(State->Bytes / sizeof(float)));
		FMemory::Memcpy(State->Data.GetData(), Raw, State->Bytes);
		State->Readback->Unlock();
		State->State.store(1, std::memory_order_release);
	});
	return false;
}

bool FDecalFitReadback::IsFailed() const
{
	return Shared->State.load(std::memory_order_acquire) == 2;
}

const TArray<float>& FDecalFitReadback::Heights() const
{
	return Shared->Data;
}

void FPooledDecalReceivers::Extract(FRDGBuilder& GraphBuilder, const FGpuDecalReceivers& Receivers)
{
	GraphBuilder.QueueBufferExtraction(Receivers.Slots, &Slots);
	GraphBuilder.QueueBufferExtraction(Receivers.SegmentCounts, &SegmentCounts);
	GraphBuilder.QueueBufferExtraction(Receivers.ChunkBoxes, &ChunkBoxes);
}

FGpuDecalReceivers FPooledDecalReceivers::Register(FRDGBuilder& GraphBuilder) const
{
	FGpuDecalReceivers Out;
	Out.Slots = GraphBuilder.RegisterExternalBuffer(Slots);
	Out.SegmentCounts = GraphBuilder.RegisterExternalBuffer(SegmentCounts);
	Out.ChunkBoxes = GraphBuilder.RegisterExternalBuffer(ChunkBoxes);
	return Out;
}

FRDGBufferRef AddDecalMeshPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road,
	const FGpuDecalReceivers& Receivers, const FGpuDecalTarget& Target)
{
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadDecals");
	const bool bDebug = Target.DebugVertices != nullptr;
	FCSRoadDecalParameters Common = CSRoadGpuDecals_Common(GraphBuilder, Mesh, Decals, Road);
	Common.ReceiverSlots = GraphBuilder.CreateSRV(Receivers.Slots);
	Common.SegmentCounts = GraphBuilder.CreateSRV(Receivers.SegmentCounts);
	Common.ChunkBoxes = GraphBuilder.CreateSRV(Receivers.ChunkBoxes);
	FRDGBufferRef Stats = GpuDetail::Zeroed<uint32>(GraphBuilder, 4, TEXT("CSRoad.DecalStats"));
	Common.StatsRW = GraphBuilder.CreateUAV(Stats);
	Common.ClipCountsRW = GraphBuilder.CreateUAV(GpuDetail::Zeroed<uint32>(GraphBuilder, 1, TEXT("CSRoad.NoClipCounts")));
	Common.OutputMode = bDebug ? 1u : 0u;
	Common.LocalToWorld = Target.LocalToWorld;
	Common.VertexCapacity = Target.VertexCapacity;
	Common.IndexCapacity = Target.IndexCapacity;
	Common.RW_Positions = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.Positions, PF_R32_FLOAT, TEXT("CSRoad.NoDecalPositions"));
	Common.RW_Tangents = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.Tangents, PF_R32_UINT, TEXT("CSRoad.NoDecalTangents"));
	Common.RW_TexCoords = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.TexCoords, PF_R32_FLOAT, TEXT("CSRoad.NoDecalTexCoords"));
	Common.RW_Colors = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.Colors, PF_R32_UINT, TEXT("CSRoad.NoDecalColors"));
	Common.RW_Indices = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.Indices, PF_R32_UINT, TEXT("CSRoad.NoDecalIndices"));
	Common.RW_MaterialIds = CSRoadGpuDecals_TypedUAV(GraphBuilder, Target.MaterialIds, PF_R32_UINT, TEXT("CSRoad.NoDecalMaterialIds"));
	if (bDebug) Common.DebugVertices = GraphBuilder.CreateUAV(Target.DebugVertices);
	CSRoadGpuDecals_AddPass<FCSRoadDecalGridCS>(GraphBuilder, Common, Common.NumItems, RDG_EVENT_NAME("DecalGrids"));
	FCSRoadDecalParameters Emit = Common;
	Emit.ClipMode = 1u;
	CSRoadGpuDecals_AddPass<FCSRoadClipCS>(GraphBuilder, Emit, Emit.NumClipped, RDG_EVENT_NAME("DecalClips"));
	return Stats;
}

bool RunDecalsBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, FGpuDecalInput& Decals, TArray<float>& OutVertices, TArray<uint32>& OutIndices,
	FGpuDecalStats& OutStats, FString& OutError)
{
	OutStats = FGpuDecalStats();
	if (Mesh.NeedsRefinementCounts() || Mesh.NumVertices == 0)
	{
		OutError = TEXT("The road mesh is empty or has no refinement counts");
		return false;
	}
	auto RecordRoad = [&Geometry, &Mesh](FRDGBuilder& GraphBuilder) { return CSRoadGpuDecals_RecordRoad(GraphBuilder, Geometry, Mesh); };

	TArray<uint32> ClipCounts;
	ClipCounts.SetNumZeroed(Decals.ClippedItems.Num());
	uint32 Stats[4] = {};
	TArray<TPair<void*, uint32>> Targets = { { Stats, uint32(sizeof(Stats)) } };
	if (ClipCounts.Num() > 0) Targets.Add({ ClipCounts.GetData(), uint32(ClipCounts.Num() * sizeof(uint32)) });
	if (!RunGraphBlocking([&](FRDGBuilder& GraphBuilder)
		{
			const FGpuDecalReceivers Receivers = AddDecalReceiverPasses(GraphBuilder, Mesh, Decals, RecordRoad(GraphBuilder));
			TArray<FRDGBufferRef> Buffers = { Receivers.Stats };
			if (Decals.ClippedItems.Num() > 0) Buffers.Add(Receivers.ClipCounts);
			return Buffers;
		}, Targets))
	{
		OutError = TEXT("The GPU decal count readback did not complete");
		return false;
	}
	if (!Decals.ApplyClipCounts(ClipCounts, OutError)) return false;
	if (Decals.NumVertices == 0)
	{
		OutVertices.Reset();
		OutIndices.Reset();
		return true;
	}

	OutVertices.SetNumZeroed(int32(Decals.NumVertices * 14u));
	OutIndices.SetNumZeroed(int32(Decals.NumIndices));
	const TArray<TPair<void*, uint32>> DecalTargets = { { OutVertices.GetData(), Decals.NumVertices * 14u * uint32(sizeof(float)) },
		{ OutIndices.GetData(), Decals.NumIndices * uint32(sizeof(uint32)) }, { Stats, uint32(sizeof(Stats)) } };
	if (!RunGraphBlocking([&](FRDGBuilder& GraphBuilder)
		{
			const FGpuRoadStreams Road = RecordRoad(GraphBuilder);
			const FGpuDecalReceivers Receivers = AddDecalReceiverPasses(GraphBuilder, Mesh, Decals, Road);
			FGpuDecalTarget Target;
			Target.DebugVertices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Decals.NumVertices * 14u), TEXT("CSRoad.TestDecalVertices"));
			Target.Indices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, Decals.NumIndices)), TEXT("CSRoad.TestDecalIndices"));
			Target.VertexCapacity = Decals.NumVertices;
			Target.IndexCapacity = Decals.NumIndices;
			const FRDGBufferRef DecalStats = AddDecalMeshPasses(GraphBuilder, Mesh, Decals, Road, Receivers, Target);
			return TArray<FRDGBufferRef>{ Target.DebugVertices, Target.Indices, DecalStats };
		}, DecalTargets))
	{
		OutError = TEXT("The GPU decal readback did not complete");
		return false;
	}
	OutStats.InMemoryRuns = Stats[0];
	OutStats.PerRunGrids = Stats[1];
	OutStats.PerChunkClips = Stats[2];
	return true;
}
}

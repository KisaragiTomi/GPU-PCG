#include "CSRoadGpuGeometry.h"

#include "CSRoadNet.h"
#include "CSRoadGpuShaders.h"
#include "CSRoadNetworkFile.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "HAL/PlatformTime.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderResource.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

static_assert(sizeof(CSRoad::NodeGeometry) == 9 * sizeof(float), "NodeGeometry must match the HLSL layout");
static_assert(sizeof(CSRoad::EdgeGeometry) == 58 * sizeof(float), "EdgeGeometry must match the HLSL layout");
static_assert(sizeof(CSRoad::EdgeNodeGeometry) == 79 * sizeof(float), "EdgeNodeGeometry must match the HLSL layout");
static_assert(sizeof(CSRoad::GeometrySystem_IntersectionData) == 36 * sizeof(float), "IntersectionData must match the HLSL layout");
static_assert(sizeof(CSRoad::FGpuRange) == 2 * sizeof(uint32), "FGpuRange is a uint2 in HLSL");


#define CSROAD_GEOMETRY_SHADER(ShaderClass, EntryPoint) \
	class ShaderClass : public FGlobalShader \
	{ \
	public: \
		using FParameters = FCSRoadGeometryParameters; \
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
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSRoadGeometry.usf", EntryPoint, SF_Compute)

CSROAD_GEOMETRY_SHADER(FCSRoadInitializeNodeGeometryCS, "InitializeNodeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadCalculateEdgeGeometryCS, "CalculateEdgeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadFlattenNodeGeometryCS, "FlattenNodeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadFinishEdgeGeometryCS, "FinishEdgeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadCalculateNodeGeometryCS, "CalculateNodeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadCalculateIntersectionGeometryCS, "CalculateIntersectionGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadCopyNodeGeometryCS, "CopyNodeGeometryCS");
CSROAD_GEOMETRY_SHADER(FCSRoadUpdateNodeGeometryCS, "UpdateNodeGeometryCS");

namespace CSRoad
{
namespace
{
template<typename ShaderType>
void CSRoadGpu_AddPass(FRDGBuilder& GraphBuilder, const FCSRoadGeometryParameters& Common, uint32 IterationIndex, int32 NumItems, FRDGEventName&& Name)
{
	if (NumItems <= 0) return;
	FCSRoadGeometryParameters* Parameters = GraphBuilder.AllocParameters<FCSRoadGeometryParameters>();
	*Parameters = Common;
	Parameters->IterationIndex = IterationIndex;
	TShaderMapRef<ShaderType> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters, FComputeShaderUtils::GetGroupCount(NumItems, GpuDetail::GroupSize));
}

template<typename T>
void CSRoadGpu_AppendRange(TArray<FGpuRange>& Ranges, int32 Index, TArray<T>& List, const TArray<T>& Items)
{
	Ranges[Index].Offset = uint32(List.Num());
	Ranges[Index].Count = uint32(Items.Num());
	List.Append(Items);
}

// The terrain's two height maps on the GPU, uploaded once per load (its serial) and kept: every graph
// of every rebuild registers them instead of uploading the maps again (2 x 32 MB on Palmsland). Render
// thread only; let go with the RHI.
class FCSRoadGpu_TerrainCache : public FRenderResource
{
public:
	uint64 Serial = 0;
	TRefCountPtr<FRDGPooledBuffer> Maps[2];

	virtual void ReleaseRHI() override
	{
		Serial = 0;
		Maps[0].SafeRelease();
		Maps[1].SafeRelease();
	}
};
TGlobalResource<FCSRoadGpu_TerrainCache> GCSRoadGpu_TerrainCache;

// A map's samples, two R16 per uint (low half first): the cached upload, or a fresh one (kept when the
// terrain has a serial). Without terrain, one zero.
FRDGBufferSRVRef CSRoadGpu_TerrainMap(FRDGBuilder& GraphBuilder, const FSavedTerrain* Terrain, int32 Map)
{
	if (!Terrain) return GraphBuilder.CreateSRV(GpuDetail::Zeroed<uint32>(GraphBuilder, 1, TEXT("CSRoad.NoTerrain")));
	FCSRoadGpu_TerrainCache& Cache = GCSRoadGpu_TerrainCache;
	if (Terrain->Serial != 0 && Cache.Serial == Terrain->Serial && Cache.Maps[Map].IsValid()) return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Cache.Maps[Map]));
	const TArray<uint16>& Samples = Map == 0 ? Terrain->Playable.Data : Terrain->World.Data;
	const FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(1, (Samples.Num() + 1) / 2)), Map == 0 ? TEXT("CSRoad.TerrainPlayable") : TEXT("CSRoad.TerrainWorld"));
	// The snapshot holds the samples until the graph has run; an odd count leaves the last half unread.
	GraphBuilder.QueueBufferUpload(Buffer, Samples.GetData(), uint64(Samples.Num()) * sizeof(uint16), ERDGInitialDataFlags::NoCopy);
	if (Terrain->Serial != 0)
	{
		if (Cache.Serial != Terrain->Serial)
		{
			Cache.Maps[0].SafeRelease();
			Cache.Maps[1].SafeRelease();
			Cache.Serial = Terrain->Serial;
		}
		GraphBuilder.QueueBufferExtraction(Buffer, &Cache.Maps[Map]);
	}
	return GraphBuilder.CreateSRV(Buffer);
}
}

bool FGpuGeometryInput::Build(const FGraph& Graph, const FAssets& Assets, const FSavedTerrain* Terrain, FString& OutError)
{
	NumEntities = uint32(Graph.NumEntities());
	EntityMask = Graph.Mask;
	EntityPrefab = Graph.PrefabRef;
	NodeData = Graph.NodeData;
	EdgeData = Graph.EdgeData;
	CurveData = Graph.CurveData;
	ElevationData = Graph.ElevationData;
	OwnerData = Graph.OwnerData;
	CompositionData = Graph.CompositionData;
	InitialNodeGeometry = Graph.InitialNodeGeometry;

	ConnectedEdgeRange.SetNum(Graph.NumEntities());
	ConnectedEdgeList.Reset();
	for (int32 Entity = 0; Entity < Graph.NumEntities(); Entity++)
	{
		const TArray<int32>& Edges = Graph.ConnectedEdges[Entity];
		if (Edges.Num() > GpuMaxNodeEdges)
		{
			OutError = FString::Printf(TEXT("Node %d has %d edges; the GPU geometry supports %d"), Entity, Edges.Num(), GpuMaxNodeEdges);
			return false;
		}
		CSRoadGpu_AppendRange(ConnectedEdgeRange, Entity, ConnectedEdgeList, Edges);
	}
	NodeList = Graph.Nodes;
	EdgeList.Reset(Graph.Roads.Num());
	for (const FRoadInstance& Road : Graph.Roads) EdgeList.Add(Road.Entity);

	PrefabGeometry = Graph.PrefabGeometry;
	PrefabPlaceable = Graph.PrefabPlaceable;

	// m_Lane holds a lane prefab id; the shader reads NetLaneData from a dense table (0 = none).
	TMap<int32, int32> LaneIndices;
	LaneDataTable.Reset();
	LaneDataTable.AddDefaulted();
	int32 MissingLanes = 0;
	auto LaneIndex = [&](int32 LanePrefab) -> int32
	{
		if (LanePrefab == Entity_Null) return 0;
		if (const int32* Existing = LaneIndices.Find(LanePrefab)) return *Existing;
		const NetLaneData* Data = Assets.LaneData.Find(LanePrefab);
		if (!Data)
		{
			MissingLanes++;
			return 0;
		}
		const int32 Index = LaneDataTable.Add(*Data);
		LaneIndices.Add(LanePrefab, Index);
		return Index;
	};

	const int32 NumCompositions = Graph.Compositions.Num();
	PrefabComposition.SetNumZeroed(NumCompositions);
	PieceRange.SetNumZeroed(NumCompositions);
	LaneRange.SetNumZeroed(NumCompositions);
	CrosswalkRange.SetNumZeroed(NumCompositions);
	PieceList.Reset();
	LaneList.Reset();
	CrosswalkList.Reset();
	for (int32 Index = 1; Index < NumCompositions; Index++)
	{
		const FComposition& Source = *Graph.Compositions[Index];
		PrefabComposition[Index] = Source.Data;
		CSRoadGpu_AppendRange(PieceRange, Index, PieceList, Source.Pieces);
		TArray<NetCompositionLane> Lanes = Source.Lanes;
		for (NetCompositionLane& Lane : Lanes) Lane.m_Lane = LaneIndex(Lane.m_Lane);
		CSRoadGpu_AppendRange(LaneRange, Index, LaneList, Lanes);
		TArray<NetCompositionCrosswalk> Crosswalks = Source.Crosswalks;
		for (NetCompositionCrosswalk& Crosswalk : Crosswalks) Crosswalk.m_Lane = LaneIndex(Crosswalk.m_Lane);
		CSRoadGpu_AppendRange(CrosswalkRange, Index, CrosswalkList, Crosswalks);
	}
	if (MissingLanes > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%d composition lanes reference a lane prefab without NetLaneData"), MissingLanes);

	bHasTerrain = Terrain && Terrain->IsValid();
	if (bHasTerrain)
	{
		TerrainPlayableOffsetSize = FVector4f(Terrain->Playable.Offset.x, Terrain->Playable.Offset.y, Terrain->Playable.Size.x, Terrain->Playable.Size.y);
		TerrainWorldOffsetSize = FVector4f(Terrain->World.Offset.x, Terrain->World.Offset.y, Terrain->World.Size.x, Terrain->World.Size.y);
		TerrainDims = FIntVector4(Terrain->Playable.Width, Terrain->Playable.Height, Terrain->World.Width, Terrain->World.Height);
		TerrainHeightScaleOffset = FVector2f(Terrain->HeightScaleOffset.x, Terrain->HeightScaleOffset.y);
	}
	TerrainSource = bHasTerrain ? Terrain : nullptr;
	return true;
}

FGpuGeometryOutput AddGeometryPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryInput& In)
{
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadGeometry");
	const uint32 NumEntities = FMath::Max(1u, In.NumEntities);

	FCSRoadGeometryParameters Common;
	Common.NumEntities = In.NumEntities;
	Common.NumNodes = uint32(In.NodeList.Num());
	Common.NumEdges = uint32(In.EdgeList.Num());
	Common.IterationIndex = 0;
	// RoadWorld injects these bounds into every Bounds3 job field (CalculateEdgeGeometryJob.m_TerrainBounds).
	Common.TerrainBoundsMin = FVector3f(-10000.0f, -1000.0f, -10000.0f);
	Common.TerrainBoundsMax = FVector3f(10000.0f, 1000.0f, 10000.0f);
	Common.EntityMask = GpuDetail::UploadSRV(GraphBuilder, In.EntityMask, TEXT("CSRoad.EntityMask"));
	Common.EntityPrefab = GpuDetail::UploadSRV(GraphBuilder, In.EntityPrefab, TEXT("CSRoad.EntityPrefab"));
	Common.NodeData = GpuDetail::UploadSRV(GraphBuilder, In.NodeData, TEXT("CSRoad.NodeData"));
	Common.EdgeData = GpuDetail::UploadSRV(GraphBuilder, In.EdgeData, TEXT("CSRoad.EdgeData"));
	Common.CurveData = GpuDetail::UploadSRV(GraphBuilder, In.CurveData, TEXT("CSRoad.CurveData"));
	Common.ElevationData = GpuDetail::UploadSRV(GraphBuilder, In.ElevationData, TEXT("CSRoad.ElevationData"));
	Common.OwnerData = GpuDetail::UploadSRV(GraphBuilder, In.OwnerData, TEXT("CSRoad.OwnerData"));
	Common.CompositionData = GpuDetail::UploadSRV(GraphBuilder, In.CompositionData, TEXT("CSRoad.CompositionData"));
	Common.ConnectedEdgeRange = GpuDetail::UploadSRV(GraphBuilder, In.ConnectedEdgeRange, TEXT("CSRoad.ConnectedEdgeRange"));
	Common.ConnectedEdgeList = GpuDetail::UploadSRV(GraphBuilder, In.ConnectedEdgeList, TEXT("CSRoad.ConnectedEdgeList"));
	Common.NodeList = GpuDetail::UploadSRV(GraphBuilder, In.NodeList, TEXT("CSRoad.NodeList"));
	Common.EdgeList = GpuDetail::UploadSRV(GraphBuilder, In.EdgeList, TEXT("CSRoad.EdgeList"));
	Common.PrefabGeometryData = GpuDetail::UploadSRV(GraphBuilder, In.PrefabGeometry, TEXT("CSRoad.PrefabGeometry"));
	Common.PrefabPlaceableData = GpuDetail::UploadSRV(GraphBuilder, In.PrefabPlaceable, TEXT("CSRoad.PrefabPlaceable"));
	Common.PrefabCompositionData = GpuDetail::UploadSRV(GraphBuilder, In.PrefabComposition, TEXT("CSRoad.PrefabComposition"));
	Common.CompositionPieceRange = GpuDetail::UploadSRV(GraphBuilder, In.PieceRange, TEXT("CSRoad.PieceRange"));
	Common.CompositionPieceList = GpuDetail::UploadSRV(GraphBuilder, In.PieceList, TEXT("CSRoad.PieceList"));
	Common.CompositionLaneRange = GpuDetail::UploadSRV(GraphBuilder, In.LaneRange, TEXT("CSRoad.LaneRange"));
	Common.CompositionLaneList = GpuDetail::UploadSRV(GraphBuilder, In.LaneList, TEXT("CSRoad.LaneList"));
	Common.CompositionCrosswalkRange = GpuDetail::UploadSRV(GraphBuilder, In.CrosswalkRange, TEXT("CSRoad.CrosswalkRange"));
	Common.CompositionCrosswalkList = GpuDetail::UploadSRV(GraphBuilder, In.CrosswalkList, TEXT("CSRoad.CrosswalkList"));
	Common.LaneDataTable = GpuDetail::UploadSRV(GraphBuilder, In.LaneDataTable, TEXT("CSRoad.LaneDataTable"));

	FRDGBufferRef NodeGeometryBuffer = GpuDetail::Upload(GraphBuilder, In.InitialNodeGeometry, TEXT("CSRoad.NodeGeometry"));
	FRDGBufferRef EdgeGeometryBuffer = GpuDetail::Zeroed<EdgeGeometry>(GraphBuilder, NumEntities, TEXT("CSRoad.EdgeGeometry"));
	FRDGBufferRef StartNodeGeometryBuffer = GpuDetail::Zeroed<EdgeNodeGeometry>(GraphBuilder, NumEntities, TEXT("CSRoad.StartNodeGeometry"));
	FRDGBufferRef EndNodeGeometryBuffer = GpuDetail::Zeroed<EdgeNodeGeometry>(GraphBuilder, NumEntities, TEXT("CSRoad.EndNodeGeometry"));
	FRDGBufferRef PrevStartBuffer = GraphBuilder.CreateBuffer(StartNodeGeometryBuffer->Desc, TEXT("CSRoad.PrevStartNodeGeometry"));
	FRDGBufferRef PrevEndBuffer = GraphBuilder.CreateBuffer(EndNodeGeometryBuffer->Desc, TEXT("CSRoad.PrevEndNodeGeometry"));
	Common.NodeGeometryRW = GraphBuilder.CreateUAV(NodeGeometryBuffer);
	Common.EdgeGeometryRW = GraphBuilder.CreateUAV(EdgeGeometryBuffer);
	Common.StartNodeGeometryRW = GraphBuilder.CreateUAV(StartNodeGeometryBuffer);
	Common.EndNodeGeometryRW = GraphBuilder.CreateUAV(EndNodeGeometryBuffer);
	Common.PrevStartNodeGeometry = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PrevStartBuffer));
	Common.PrevEndNodeGeometry = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(PrevEndBuffer));
	Common.EdgeHeightMap = GraphBuilder.CreateUAV(GpuDetail::Zeroed<FVector4f>(GraphBuilder, NumEntities * 2u, TEXT("CSRoad.EdgeHeightMap")));
	Common.EdgeHeightMapValid = GraphBuilder.CreateUAV(GpuDetail::Zeroed<uint32>(GraphBuilder, NumEntities * 2u, TEXT("CSRoad.EdgeHeightMapValid")));
	Common.IntersectionBuffer = GraphBuilder.CreateUAV(GpuDetail::Zeroed<GeometrySystem_IntersectionData>(GraphBuilder, NumEntities, TEXT("CSRoad.Intersections")));

	Common.bHasTerrain = In.bHasTerrain ? 1u : 0u;
	Common.TerrainPlayableOffsetSize = In.TerrainPlayableOffsetSize;
	Common.TerrainWorldOffsetSize = In.TerrainWorldOffsetSize;
	Common.TerrainDims = In.TerrainDims;
	Common.TerrainHeightScaleOffset = In.TerrainHeightScaleOffset;
	Common.TerrainPlayable = CSRoadGpu_TerrainMap(GraphBuilder, In.TerrainSource, 0);
	Common.TerrainWorld = CSRoadGpu_TerrainMap(GraphBuilder, In.TerrainSource, 1);

	const int32 NumNodes = In.NodeList.Num();
	const int32 NumEdges = In.EdgeList.Num();
	CSRoadGpu_AddPass<FCSRoadInitializeNodeGeometryCS>(GraphBuilder, Common, 0, NumNodes, RDG_EVENT_NAME("InitializeNodeGeometry"));
	CSRoadGpu_AddPass<FCSRoadCalculateEdgeGeometryCS>(GraphBuilder, Common, 0, NumEdges, RDG_EVENT_NAME("CalculateEdgeGeometry"));
	CSRoadGpu_AddPass<FCSRoadFlattenNodeGeometryCS>(GraphBuilder, Common, 0, NumNodes, RDG_EVENT_NAME("FlattenNodeGeometry"));
	CSRoadGpu_AddPass<FCSRoadFinishEdgeGeometryCS>(GraphBuilder, Common, 0, NumEdges, RDG_EVENT_NAME("FinishEdgeGeometry"));
	for (uint32 Iteration = 0; Iteration < 2; Iteration++)
	{
		// Iteration 1 reads the other edges' node geometry (CheckNodeCurves); the copy keeps those reads
		// independent of the order the edges run in. Iteration 0 binds it too, so it is copied as well.
		AddCopyBufferPass(GraphBuilder, PrevStartBuffer, StartNodeGeometryBuffer);
		AddCopyBufferPass(GraphBuilder, PrevEndBuffer, EndNodeGeometryBuffer);
		CSRoadGpu_AddPass<FCSRoadCalculateNodeGeometryCS>(GraphBuilder, Common, Iteration, NumEdges, RDG_EVENT_NAME("CalculateNodeGeometry(%u)", Iteration));
	}
	CSRoadGpu_AddPass<FCSRoadCalculateIntersectionGeometryCS>(GraphBuilder, Common, 0, NumEdges, RDG_EVENT_NAME("CalculateIntersectionGeometry"));
	CSRoadGpu_AddPass<FCSRoadCopyNodeGeometryCS>(GraphBuilder, Common, 0, NumEdges, RDG_EVENT_NAME("CopyNodeGeometry"));
	CSRoadGpu_AddPass<FCSRoadUpdateNodeGeometryCS>(GraphBuilder, Common, 0, NumNodes, RDG_EVENT_NAME("UpdateNodeGeometry"));

	FGpuGeometryOutput Out;
	FCSRoadGeometryParameters* Shared = GraphBuilder.AllocParameters<FCSRoadGeometryParameters>();
	*Shared = Common;
	Out.Parameters = Shared;
	Out.NodeGeometry = NodeGeometryBuffer;
	Out.EdgeGeometry = EdgeGeometryBuffer;
	Out.StartNodeGeometry = StartNodeGeometryBuffer;
	Out.EndNodeGeometry = EndNodeGeometryBuffer;
	return Out;
}

namespace
{
// Visit(member address, is a UAV) for every buffer member of the geometry parameters, in member order.
template<typename FVisit>
void CSRoadGpu_ForEachBuffer(FCSRoadGeometryParameters& Parameters, FVisit&& Visit)
{
	for (const FShaderParametersMetadata::FMember& Member : FCSRoadGeometryParameters::FTypeInfo::GetStructMetadata()->GetMembers())
	{
		const EUniformBufferBaseType Type = Member.GetBaseType();
		if (Type == UBMT_RDG_BUFFER_SRV || Type == UBMT_RDG_BUFFER_UAV) Visit(reinterpret_cast<uint8*>(&Parameters) + Member.GetOffset(), Type == UBMT_RDG_BUFFER_UAV);
	}
}
}

bool FPooledGeometry::IsValid() const
{
	return Parameters.IsValid() && Buffers.Num() > 0 && Buffers.Last().IsValid();
}

void FPooledGeometry::Extract(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry)
{
	Parameters = MakeShared<FCSRoadGeometryParameters>(*Geometry.Parameters);
	// Sized first: each extraction keeps a pointer into the array.
	int32 Count = 0;
	CSRoadGpu_ForEachBuffer(*Parameters, [&Count](uint8*, bool) { Count++; });
	Buffers.Reset();
	Buffers.SetNum(Count);
	int32 Index = 0;
	CSRoadGpu_ForEachBuffer(*Parameters, [&GraphBuilder, this, &Index](uint8* Address, bool bUAV)
	{
		const FRDGBufferRef Buffer = bUAV ? (*reinterpret_cast<FRDGBufferUAVRef*>(Address))->GetParent() : (*reinterpret_cast<FRDGBufferSRVRef*>(Address))->GetParent();
		GraphBuilder.QueueBufferExtraction(Buffer, &Buffers[Index++]);
	});
}

FGpuGeometryOutput FPooledGeometry::Register(FRDGBuilder& GraphBuilder) const
{
	FCSRoadGeometryParameters* Bound = GraphBuilder.AllocParameters<FCSRoadGeometryParameters>();
	*Bound = *Parameters;
	int32 Index = 0;
	CSRoadGpu_ForEachBuffer(*Bound, [&GraphBuilder, this, &Index](uint8* Address, bool bUAV)
	{
		const FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(Buffers[Index++]);
		if (bUAV) *reinterpret_cast<FRDGBufferUAVRef*>(Address) = GraphBuilder.CreateUAV(Buffer);
		else *reinterpret_cast<FRDGBufferSRVRef*>(Address) = GraphBuilder.CreateSRV(Buffer);
	});
	FGpuGeometryOutput Out;
	Out.Parameters = Bound;
	Out.NodeGeometry = Bound->NodeGeometryRW->GetParent();
	Out.EdgeGeometry = Bound->EdgeGeometryRW->GetParent();
	Out.StartNodeGeometry = Bound->StartNodeGeometryRW->GetParent();
	Out.EndNodeGeometry = Bound->EndNodeGeometryRW->GetParent();
	return Out;
}

bool RunGeometryBlocking(const FGpuGeometryInput& Input, FGpuGeometryResult& OutResult, FString& OutError)
{
	const uint32 NumEntities = Input.NumEntities;
	if (NumEntities == 0 || Input.EdgeList.Num() == 0)
	{
		OutError = TEXT("The network has no edges");
		return false;
	}
	OutResult.Nodes.SetNumZeroed(NumEntities);
	OutResult.Edges.SetNumZeroed(NumEntities);
	OutResult.StartNodes.SetNumZeroed(NumEntities);
	OutResult.EndNodes.SetNumZeroed(NumEntities);
	void* Targets[4] = { OutResult.Nodes.GetData(), OutResult.Edges.GetData(), OutResult.StartNodes.GetData(), OutResult.EndNodes.GetData() };
	const uint32 Sizes[4] = { NumEntities * uint32(sizeof(NodeGeometry)), NumEntities * uint32(sizeof(EdgeGeometry)), NumEntities * uint32(sizeof(EdgeNodeGeometry)),
		NumEntities * uint32(sizeof(EdgeNodeGeometry)) };
	bool bRead = false;
	const double StartTime = FPlatformTime::Seconds();
	ENQUEUE_RENDER_COMMAND(CSRoadGeometryBlocking)([&Input, &Targets, &Sizes, &bRead](FRHICommandListImmediate& RHICmdList)
	{
		FRHIGPUBufferReadback* Readbacks[4] = { new FRHIGPUBufferReadback(TEXT("CSRoad.NodeGeometryReadback")), new FRHIGPUBufferReadback(TEXT("CSRoad.EdgeGeometryReadback")),
			new FRHIGPUBufferReadback(TEXT("CSRoad.StartNodeGeometryReadback")), new FRHIGPUBufferReadback(TEXT("CSRoad.EndNodeGeometryReadback")) };
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRoadGeometryBlocking"));
			const FGpuGeometryOutput Out = AddGeometryPasses(GraphBuilder, Input);
			const FRDGBufferRef Buffers[4] = { Out.NodeGeometry, Out.EdgeGeometry, Out.StartNodeGeometry, Out.EndNodeGeometry };
			for (int32 Index = 0; Index < 4; Index++) AddEnqueueCopyPass(GraphBuilder, Readbacks[Index], Buffers[Index], Sizes[Index]);
			GraphBuilder.Execute();
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		bRead = true;
		for (int32 Index = 0; Index < 4; Index++)
		{
			const void* Data = Readbacks[Index]->IsReady() ? Readbacks[Index]->Lock(Sizes[Index]) : nullptr;
			if (Data)
			{
				FMemory::Memcpy(Targets[Index], Data, Sizes[Index]);
				Readbacks[Index]->Unlock();
			}
			else bRead = false;
			delete Readbacks[Index];
		}
	});
	FlushRenderingCommands();
	OutResult.Milliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	if (!bRead)
	{
		OutError = TEXT("The GPU geometry readback did not complete");
		return false;
	}
	return true;
}
}

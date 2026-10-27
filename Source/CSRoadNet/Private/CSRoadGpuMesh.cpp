#include "CSRoadGpuMesh.h"

#include "CSRoadGpuShaders.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

static_assert(sizeof(CSRoad::FCompositionVertex) == 10 * sizeof(uint32), "FCompositionVertex must match the HLSL layout");
static_assert(sizeof(CSRoad::CompositionParameters) == 156 * sizeof(float), "CompositionParameters must match the HLSL layout");
static_assert(sizeof(CSRoad::FGpuMeshRange) == 6 * sizeof(uint32) && sizeof(CSRoad::FGpuMeshInstance) == 9 * sizeof(uint32), "FMeshRange / FMeshInstance must match the HLSL layout");
static_assert(sizeof(CSRoad::FGpuRefineTriangle) == 4 * sizeof(uint32), "FRefineTriangle must match the HLSL layout");

namespace CSRoad
{
// CSRoadDeform.ush FJunctionSide.
struct FGpuJunctionSide
{
	float3 Start;
	float3 End;
	uint32 Flags = 0;
};
static_assert(sizeof(FGpuJunctionSide) == 7 * sizeof(uint32), "FJunctionSide must match the HLSL layout");
}

BEGIN_SHADER_PARAMETER_STRUCT(FCSRoadMeshParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FCSRoadGeometryParameters, Geometry)
	SHADER_PARAMETER(uint32, NumInstances)
	SHADER_PARAMETER(uint32, InstanceGroupsX)
	SHADER_PARAMETER(uint32, OutputMode)
	SHADER_PARAMETER(FMatrix44f, LocalToWorld)
	SHADER_PARAMETER(uint32, VertexCapacity)
	SHADER_PARAMETER(uint32, IndexCapacity)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, DeformParamsRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, DeformParams)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FJunctionSide>, JunctionSidesRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, JunctionPartnersRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, JunctionPartners)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, RoadOfEntity)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FCompositionVertex>, MeshVertices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MeshIndices)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, MeshTriangleMaterials)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMeshRange>, MeshRanges)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FMeshInstance>, MeshInstances)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_TexCoords)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, DebugVertices)
	SHADER_PARAMETER(uint32, NumRefineInstances)
	SHADER_PARAMETER(uint32, RefineGroupsX)
	SHADER_PARAMETER(uint32, RefineMode)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RefineInstances)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FRefineTriangle>, RefineTriangles)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FCompositionVertex>, RefinedVerticesRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RefineCountsRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RefineOffsetsRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, RefineTotalsRW)
END_SHADER_PARAMETER_STRUCT()

#define CSROAD_MESH_SHADER(ShaderClass, EntryPoint) \
	class ShaderClass : public FGlobalShader \
	{ \
	public: \
		using FParameters = FCSRoadMeshParameters; \
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
	IMPLEMENT_GLOBAL_SHADER(ShaderClass, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSRoadMesh.usf", EntryPoint, SF_Compute)

CSROAD_MESH_SHADER(FCSRoadCalculateParametersCS, "CalculateParametersCS");
CSROAD_MESH_SHADER(FCSRoadJunctionMatchCS, "JunctionMatchCS");
CSROAD_MESH_SHADER(FCSRoadRefineCS, "RefineCS");
CSROAD_MESH_SHADER(FCSRoadRefineScanCS, "RefineScanCS");
CSROAD_MESH_SHADER(FCSRoadDeformVerticesCS, "DeformVerticesCS");
CSROAD_MESH_SHADER(FCSRoadWriteIndicesCS, "WriteIndicesCS");

namespace CSRoad
{
namespace
{
template<typename ShaderType>
void CSRoadGpuMesh_AddPass(FRDGBuilder& GraphBuilder, const FCSRoadMeshParameters& Common, FIntVector GroupCount, FRDGEventName&& Name)
{
	FCSRoadMeshParameters* Parameters = GraphBuilder.AllocParameters<FCSRoadMeshParameters>();
	*Parameters = Common;
	TShaderMapRef<ShaderType> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, MoveTemp(Name), Shader, Parameters, GroupCount);
}

// A typed view of a stream, or of a one-element stand-in the passes never write (test output).
FRDGBufferUAVRef CSRoadGpuMesh_StreamUAV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, EPixelFormat Format, const TCHAR* StandInName)
{
	if (!Buffer) Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), StandInName);
	return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, Format));
}

FIntVector CSRoadGpuMesh_Groups(uint32 Count)
{
	return FIntVector(int32(FMath::Clamp(Count, 1u, 65535u)), int32(FMath::DivideAndRoundUp(FMath::Max(Count, 1u), 65535u)), 1);
}

enum class ECSRoadGpuMeshStage
{
	// Parameters, seams and the refinement counts: the first graph of a refined build.
	Counts,
	// Everything, writing the target.
	Build,
};

struct FCSRoadGpuMesh_Outputs
{
	// Seam partner per side (JunctionMatchCS).
	FRDGBufferRef Partners = nullptr;
	// Per refine instance: leaves, midpoints, unconverged (RefineScanCS).
	FRDGBufferRef RefineTotals = nullptr;
};

FCSRoadGpuMesh_Outputs CSRoadGpuMesh_AddPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target,
	ECSRoadGpuMeshStage Stage)
{
	check(Geometry.Parameters);
	check(Stage == ECSRoadGpuMeshStage::Counts || !Mesh.NeedsRefinementCounts());
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadMesh");
	const uint32 NumEdges = Geometry.Parameters->NumEdges;
	const uint32 NumInstances = uint32(Mesh.Instances.Num());
	const uint32 NumRefine = uint32(Mesh.RefineInstances.Num());
	const bool bDebug = Target.DebugVertices != nullptr;
	// CompositionParameters per parameter slot, packed as CSRoadDeform.ush RM_PARAM_STRIDE float4s.
	FRDGBufferRef ParametersBuffer = GpuDetail::Zeroed<FVector4f>(GraphBuilder, NumEdges * 3u * 37u, TEXT("CSRoad.DeformParams"));
	// Two sides per parameter slot; the edge slots stay unused (zero: no partner).
	FRDGBufferRef SidesBuffer = GpuDetail::Zeroed<FGpuJunctionSide>(GraphBuilder, NumEdges * 6u, TEXT("CSRoad.JunctionSides"));
	FCSRoadGpuMesh_Outputs Outputs;
	Outputs.Partners = GpuDetail::Zeroed<uint32>(GraphBuilder, NumEdges * 6u, TEXT("CSRoad.JunctionPartners"));
	Outputs.RefineTotals = GpuDetail::Zeroed<FUintVector4>(GraphBuilder, NumRefine, TEXT("CSRoad.RefineTotals"));
	FRDGBufferRef RefineCounts = GpuDetail::Zeroed<FUintVector4>(GraphBuilder, Mesh.NumRefineItems, TEXT("CSRoad.RefineCounts"));
	FRDGBufferRef RefineOffsets = GpuDetail::Zeroed<FUintVector2>(GraphBuilder, Mesh.NumRefineItems, TEXT("CSRoad.RefineOffsets"));
	FRDGBufferRef RefinedVertices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FCompositionVertex), FMath::Max(1u, Mesh.NumMidpoints)), TEXT("CSRoad.RefinedVertices"));

	FCSRoadMeshParameters Common;
	Common.Geometry = *Geometry.Parameters;
	Common.NumInstances = NumInstances;
	Common.InstanceGroupsX = uint32(CSRoadGpuMesh_Groups(NumInstances).X);
	Common.OutputMode = bDebug ? 1u : 0u;
	Common.LocalToWorld = Target.LocalToWorld;
	Common.VertexCapacity = Target.VertexCapacity;
	Common.IndexCapacity = Target.IndexCapacity;
	Common.DeformParamsRW = GraphBuilder.CreateUAV(ParametersBuffer);
	Common.DeformParams = GraphBuilder.CreateSRV(ParametersBuffer);
	Common.JunctionSidesRW = GraphBuilder.CreateUAV(SidesBuffer);
	Common.JunctionPartnersRW = GraphBuilder.CreateUAV(Outputs.Partners);
	Common.JunctionPartners = GraphBuilder.CreateSRV(Outputs.Partners);
	Common.RoadOfEntity = GpuDetail::UploadSRV(GraphBuilder, Mesh.RoadOfEntity, TEXT("CSRoad.RoadOfEntity"));
	Common.MeshVertices = GpuDetail::UploadSRV(GraphBuilder, Mesh.Vertices, TEXT("CSRoad.MeshVertices"));
	Common.MeshIndices = GpuDetail::UploadSRV(GraphBuilder, Mesh.Indices, TEXT("CSRoad.MeshIndices"));
	Common.MeshTriangleMaterials = GpuDetail::UploadSRV(GraphBuilder, Mesh.TriangleMaterials, TEXT("CSRoad.MeshTriangleMaterials"));
	Common.MeshRanges = GpuDetail::UploadSRV(GraphBuilder, Mesh.Meshes, TEXT("CSRoad.MeshRanges"));
	Common.MeshInstances = GpuDetail::UploadSRV(GraphBuilder, Mesh.Instances, TEXT("CSRoad.MeshInstances"));
	Common.RW_Positions = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.Positions, PF_R32_FLOAT, TEXT("CSRoad.NoPositions"));
	Common.RW_Tangents = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.Tangents, PF_R32_UINT, TEXT("CSRoad.NoTangents"));
	Common.RW_TexCoords = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.TexCoords, PF_R32_FLOAT, TEXT("CSRoad.NoTexCoords"));
	Common.RW_Colors = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.Colors, PF_R32_UINT, TEXT("CSRoad.NoColors"));
	Common.RW_Indices = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.Indices, PF_R32_UINT, TEXT("CSRoad.NoIndices"));
	Common.RW_MaterialIds = CSRoadGpuMesh_StreamUAV(GraphBuilder, Target.MaterialIds, PF_R32_UINT, TEXT("CSRoad.NoMaterialIds"));
	Common.DebugVertices = GraphBuilder.CreateUAV(bDebug ? Target.DebugVertices : GpuDetail::Zeroed<float>(GraphBuilder, 1, TEXT("CSRoad.NoDebugVertices")));
	Common.NumRefineInstances = NumRefine;
	Common.RefineGroupsX = uint32(CSRoadGpuMesh_Groups(NumRefine).X);
	Common.RefineMode = 0u;
	Common.RefineInstances = GpuDetail::UploadSRV(GraphBuilder, Mesh.RefineInstances, TEXT("CSRoad.RefineInstances"));
	Common.RefineTriangles = GpuDetail::UploadSRV(GraphBuilder, Mesh.RefineTriangles, TEXT("CSRoad.RefineTriangles"));
	Common.RefinedVerticesRW = GraphBuilder.CreateUAV(RefinedVertices);
	Common.RefineCountsRW = GraphBuilder.CreateUAV(RefineCounts);
	Common.RefineOffsetsRW = GraphBuilder.CreateUAV(RefineOffsets);
	Common.RefineTotalsRW = GraphBuilder.CreateUAV(Outputs.RefineTotals);

	if (NumEdges == 0) return Outputs;
	CSRoadGpuMesh_AddPass<FCSRoadCalculateParametersCS>(GraphBuilder, Common, FComputeShaderUtils::GetGroupCount(int32(NumEdges), GpuDetail::GroupSize),
		RDG_EVENT_NAME("CalculateParameters"));
	CSRoadGpuMesh_AddPass<FCSRoadJunctionMatchCS>(GraphBuilder, Common, FComputeShaderUtils::GetGroupCount(int32(NumEdges * 4u), GpuDetail::GroupSize),
		RDG_EVENT_NAME("JunctionMatch"));
	if (NumRefine > 0)
	{
		CSRoadGpuMesh_AddPass<FCSRoadRefineCS>(GraphBuilder, Common, CSRoadGpuMesh_Groups(NumRefine), RDG_EVENT_NAME("RefineCount"));
		CSRoadGpuMesh_AddPass<FCSRoadRefineScanCS>(GraphBuilder, Common, CSRoadGpuMesh_Groups(NumRefine), RDG_EVENT_NAME("RefineScan"));
	}
	if (Stage == ECSRoadGpuMeshStage::Counts || NumInstances == 0) return Outputs;
	if (NumRefine > 0)
	{
		FCSRoadMeshParameters Emit = Common;
		Emit.RefineMode = 1u;
		CSRoadGpuMesh_AddPass<FCSRoadRefineCS>(GraphBuilder, Emit, CSRoadGpuMesh_Groups(NumRefine), RDG_EVENT_NAME("RefineEmit"));
	}
	CSRoadGpuMesh_AddPass<FCSRoadDeformVerticesCS>(GraphBuilder, Common, CSRoadGpuMesh_Groups(NumInstances), RDG_EVENT_NAME("DeformVertices"));
	if (!bDebug) CSRoadGpuMesh_AddPass<FCSRoadWriteIndicesCS>(GraphBuilder, Common, CSRoadGpuMesh_Groups(NumInstances), RDG_EVENT_NAME("WriteIndices"));
	return Outputs;
}

}

bool RunGraphBlocking(TFunctionRef<TArray<FRDGBufferRef>(FRDGBuilder&)> Record, const TArray<TPair<void*, uint32>>& Targets)
{
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSRoadGpuMeshBlocking)([&Record, &Targets, &bRead](FRHICommandListImmediate& RHICmdList)
	{
		TArray<TUniquePtr<FRHIGPUBufferReadback>> Readbacks;
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRoadGpuMeshBlocking"));
			const TArray<FRDGBufferRef> Buffers = Record(GraphBuilder);
			check(Buffers.Num() == Targets.Num());
			for (int32 I = 0; I < Buffers.Num(); I++)
			{
				Readbacks.Add(MakeUnique<FRHIGPUBufferReadback>(TEXT("CSRoad.Readback")));
				if (Targets[I].Value > 0) AddEnqueueCopyPass(GraphBuilder, Readbacks.Last().Get(), Buffers[I], Targets[I].Value);
			}
			GraphBuilder.Execute();
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		bRead = true;
		for (int32 I = 0; I < Readbacks.Num(); I++)
		{
			if (Targets[I].Value == 0) continue;
			const void* Data = Readbacks[I]->IsReady() ? Readbacks[I]->Lock(Targets[I].Value) : nullptr;
			if (!Data)
			{
				bRead = false;
				continue;
			}
			FMemory::Memcpy(Targets[I].Key, Data, Targets[I].Value);
			Readbacks[I]->Unlock();
		}
	});
	FlushRenderingCommands();
	return bRead;
}

const FCompositionMesh& FCompositionMeshCache::Get(FAssets& Assets, const FComposition& Composition)
{
	if (const TUniquePtr<FCompositionMesh>* Known = Meshes.Find(&Composition)) return **Known;
	TUniquePtr<FCompositionMesh>& Mesh = Meshes.Add(&Composition, MakeUnique<FCompositionMesh>());
	GenerateCompositionMesh(Assets, Composition, *Mesh);
	return *Mesh;
}

bool FGpuRoadMeshInput::Build(const FGraph& Graph, FAssets& Assets, FString& OutError, bool bRefine)
{
	// Edge, start node and end node of every road, in road order (RoadRenderer.BuildSteps).
	TArray<FGpuInstanceRef> All;
	All.Reserve(Graph.Roads.Num() * 3);
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++) for (int32 Kind = 0; Kind < 3; Kind++) All.Add({ Road, Kind });
	return BuildInstances(Graph, Assets, All, {}, nullptr, OutError, bRefine);
}

bool FGpuRoadMeshInput::BuildSubset(const FGraph& Graph, FAssets& Assets, const TArray<FGpuInstanceRef>& Drawn, const TArray<FGpuInstanceRef>& Context, FCompositionMeshCache* Cache,
	FString& OutError, bool bRefine)
{
	return BuildInstances(Graph, Assets, Drawn, Context, Cache, OutError, bRefine);
}

bool FGpuRoadMeshInput::BuildInstances(const FGraph& Graph, FAssets& Assets, const TArray<FGpuInstanceRef>& Drawn, const TArray<FGpuInstanceRef>& Context, FCompositionMeshCache* Cache,
	FString& OutError, bool bRefine)
{
	Vertices.Reset();
	Indices.Reset();
	TriangleMaterials.Reset();
	MaterialSlots.Reset();
	RefineTriangles.Reset();
	RefineInstances.Reset();
	NumRefineItems = 0;
	bRefinementApplied = false;
	NumMidpoints = 0;
	NumUnconverged = 0;
	Meshes.SetNumZeroed(Graph.Compositions.Num());
	auto CompositionOf = [&Graph](const FGpuInstanceRef& Ref)
	{
		const Composition& Compositions = Graph.CompositionData[Graph.Roads[Ref.Road].Entity];
		return Ref.Kind == 0 ? Compositions.m_Edge : Ref.Kind == 1 ? Compositions.m_StartNode : Compositions.m_EndNode;
	};
	TBitArray<> Used(false, Graph.Compositions.Num());
	for (const TArray<FGpuInstanceRef>* List : { &Drawn, &Context })
	{
		for (const FGpuInstanceRef& Ref : *List)
		{
			if (!Graph.Roads.IsValidIndex(Ref.Road) || Ref.Kind < 0 || Ref.Kind > 2)
			{
				OutError = FString::Printf(TEXT("No mesh instance %d of road %d"), Ref.Kind, Ref.Road);
				return false;
			}
			Used[CompositionOf(Ref)] = true;
		}
	}
	// The compositions the instances draw, in composition order: it numbers the material slots.
	TMap<int32, int32> SlotOfAtlas;
	for (int32 Index = 1; Index < Graph.Compositions.Num(); Index++)
	{
		if (!Used[Index]) continue;
		FCompositionMesh Generated;
		if (!Cache) GenerateCompositionMesh(Assets, *Graph.Compositions[Index], Generated);
		const FCompositionMesh& Mesh = Cache ? Cache->Get(Assets, *Graph.Compositions[Index]) : Generated;
		FGpuMeshRange& Range = Meshes[Index];
		Range.VertexOffset = uint32(Vertices.Num());
		Range.VertexCount = uint32(Mesh.Vertices.Num());
		Range.IndexOffset = uint32(Indices.Num());
		Range.IndexCount = uint32(Mesh.Indices.Num() / 3 * 3);
		Vertices.Append(Mesh.Vertices);
		for (int32 I = 0; I < int32(Range.IndexCount); I++) Indices.Add(uint32(Mesh.Indices[I]));
		const int32 FirstTriangle = TriangleMaterials.AddZeroed(int32(Range.IndexCount / 3));
		const bool bRefined = bRefine && (Graph.Compositions[Index]->Data.m_Flags.m_General & CompositionFlags_General_Node) != 0;
		Range.RefineOffset = uint32(RefineTriangles.Num());
		for (const MeshMaterial& Material : Mesh.Materials)
		{
			int32* Slot = SlotOfAtlas.Find(Material.m_MaterialIndex);
			if (!Slot)
			{
				Slot = &SlotOfAtlas.Add(Material.m_MaterialIndex, MaterialSlots.Num());
				MaterialSlots.Add(Assets.Atlases().IsValidIndex(Material.m_MaterialIndex) ? Assets.Atlases()[Material.m_MaterialIndex] : FString());
			}
			const int32 End = FMath::Min(int32(Range.IndexCount), Material.m_StartIndex + Material.m_IndexCount) / 3;
			for (int32 Triangle = Material.m_StartIndex / 3; Triangle < End; Triangle++) TriangleMaterials[FirstTriangle + Triangle] = uint32(*Slot);
			if (!bRefined) continue;
			// Refine works per material group, triangle by triangle; an edge's defining direction is
			// the one of the first triangle of the group that uses it (the first Midpoint call).
			TMap<uint64, bool> FirstForward;
			for (int32 Triangle = Material.m_StartIndex / 3; Triangle < End; Triangle++)
			{
				FGpuRefineTriangle& Refine = RefineTriangles.AddDefaulted_GetRef();
				const uint32 Corners[3] = { uint32(Mesh.Indices[Triangle * 3]), uint32(Mesh.Indices[Triangle * 3 + 1]), uint32(Mesh.Indices[Triangle * 3 + 2]) };
				Refine.A = Corners[0];
				Refine.B = Corners[1];
				Refine.C = Corners[2];
				Refine.Flags = uint32(*Slot);
				for (int32 Edge = 0; Edge < 3; Edge++)
				{
					const uint32 U = Corners[Edge];
					const uint32 V = Corners[(Edge + 1) % 3];
					const uint64 Key = (uint64(FMath::Min(U, V)) << 32) | uint64(FMath::Max(U, V));
					const bool bForward = U < V;
					const bool* First = FirstForward.Find(Key);
					if (!First) FirstForward.Add(Key, bForward);
					else if (*First != bForward) Refine.Flags |= 1u << (29 + Edge);
				}
			}
		}
		Range.RefineCount = uint32(RefineTriangles.Num()) - Range.RefineOffset;
	}

	RoadOfEntity.Init(-1, Graph.NumEntities());
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++) RoadOfEntity[Graph.Roads[Road].Entity] = Road;

	// Drawn instances first, so the draw counters cover exactly them; the bounds hold only those.
	InstanceOfSlot.Init(INDEX_NONE, Graph.Roads.Num() * 3);
	Instances.Reset(Drawn.Num() + Context.Num());
	FBox Unity(ForceInit);
	float MaxHalfWidth = 0.0f;
	float MinHeight = 0.0f;
	float MaxHeight = 0.0f;
	TBitArray<> Bounded(false, Graph.Roads.Num());
	for (const TArray<FGpuInstanceRef>* List : { &Drawn, &Context })
	{
		const bool bDrawn = List == &Drawn;
		for (const FGpuInstanceRef& Ref : *List)
		{
			if (InstanceOfSlot[Ref.Slot()] != INDEX_NONE)
			{
				OutError = FString::Printf(TEXT("Mesh instance %d of road %d is listed twice"), Ref.Kind, Ref.Road);
				return false;
			}
			const int32 MeshIndex = CompositionOf(Ref);
			FGpuMeshInstance& Instance = Instances.AddDefaulted_GetRef();
			Instance.Mesh = uint32(MeshIndex);
			Instance.Parameters = uint32(Ref.Slot());
			Instance.JunctionSlot = Ref.Kind == 0 ? FGpuMeshInstance::NoJunction : Instance.Parameters;
			if (Meshes[MeshIndex].RefineCount > 0)
			{
				Instance.RefineItemOffset = NumRefineItems;
				NumRefineItems += Meshes[MeshIndex].RefineCount;
				RefineInstances.Add(uint32(Instances.Num() - 1));
			}
			InstanceOfSlot[Ref.Slot()] = Instances.Num() - 1;
			if (!bDrawn) continue;
			const NetCompositionData& Data = Graph.Compositions[MeshIndex]->Data;
			MaxHalfWidth = FMath::Max(MaxHalfWidth, Data.m_Width * 0.5f);
			MinHeight = FMath::Min(MinHeight, Data.m_HeightRange.min);
			MaxHeight = FMath::Max(MaxHeight, Data.m_HeightRange.max);
			if (Bounded[Ref.Road]) continue;
			Bounded[Ref.Road] = true;
			const Bezier4x3& Curve = Graph.CurveData[Graph.Roads[Ref.Road].Entity].m_Bezier;
			for (const float3& P : { Curve.a, Curve.b, Curve.c, Curve.d }) Unity += FVector(P.x, P.y, P.z);
		}
	}
	NumDrawnInstances = uint32(Drawn.Num());
	// Nodes widen past the curves by about a road width; terrain-following vertices may drop to the
	// ground under elevated roads. Refinement only adds points on the same curves.
	Bounds = FBox(ForceInit);
	if (Unity.IsValid)
	{
		const double Margin = double(MaxHalfWidth) * 2.0 + 2.0;
		Unity.Min += FVector(-Margin, double(MinHeight) - 100.0, -Margin);
		Unity.Max += FVector(Margin, double(MaxHeight) + 10.0, Margin);
		Bounds += FVector(Unity.Min.X, -Unity.Min.Z, Unity.Min.Y) * 100.0;
		Bounds += FVector(Unity.Max.X, -Unity.Max.Z, Unity.Max.Y) * 100.0;
	}
	return Layout(OutError);
}

bool FGpuRoadMeshInput::ApplyRefinement(const TArray<FUintVector4>& Totals, FString& OutError)
{
	if (Totals.Num() != RefineInstances.Num())
	{
		OutError = FString::Printf(TEXT("%d refinement totals for %d refined instances"), Totals.Num(), RefineInstances.Num());
		return false;
	}
	uint64 Midpoints = 0;
	NumUnconverged = 0;
	for (int32 I = 0; I < Totals.Num(); I++)
	{
		FGpuMeshInstance& Instance = Instances[RefineInstances[I]];
		Instance.RefinedTriangleCount = Totals[I].X;
		Instance.RefinedVertexCount = Totals[I].Y;
		Instance.RefinedVertexOffset = uint32(Midpoints);
		Midpoints += Totals[I].Y;
		NumUnconverged += Totals[I].Z;
	}
	if (Midpoints > uint64(MAX_int32))
	{
		OutError = FString::Printf(TEXT("Refinement made %llu midpoints"), Midpoints);
		return false;
	}
	NumMidpoints = uint32(Midpoints);
	bRefinementApplied = true;
	return Layout(OutError);
}

bool FGpuRoadMeshInput::ApplyRefinementFrom(const FGpuRoadMeshInput& Counted, FString& OutError)
{
	if (!Counted.bRefinementApplied)
	{
		OutError = TEXT("The counted mesh input has no refinement");
		return false;
	}
	TArray<FUintVector4> Totals;
	Totals.SetNumZeroed(RefineInstances.Num());
	for (int32 I = 0; I < RefineInstances.Num(); I++)
	{
		const int32 Slot = int32(Instances[RefineInstances[I]].Parameters);
		const int32 Source = Counted.InstanceOfSlot.IsValidIndex(Slot) ? Counted.InstanceOfSlot[Slot] : INDEX_NONE;
		if (Source == INDEX_NONE)
		{
			OutError = FString::Printf(TEXT("Mesh instance slot %d was not counted"), Slot);
			return false;
		}
		const FGpuMeshInstance& From = Counted.Instances[Source];
		Totals[I] = FUintVector4(From.RefinedTriangleCount, From.RefinedVertexCount, 0u, 0u);
	}
	return ApplyRefinement(Totals, OutError);
}

bool FGpuRoadMeshInput::Layout(FString& OutError)
{
	uint64 VertexCount = 0;
	uint64 IndexCount = 0;
	uint64 DrawnVertexCount = 0;
	uint64 DrawnIndexCount = 0;
	for (int32 I = 0; I < Instances.Num(); I++)
	{
		if (uint32(I) == NumDrawnInstances)
		{
			DrawnVertexCount = VertexCount;
			DrawnIndexCount = IndexCount;
		}
		FGpuMeshInstance& Instance = Instances[I];
		const FGpuMeshRange& Range = Meshes[Instance.Mesh];
		Instance.VertexOffset = uint32(FMath::Min(VertexCount, uint64(MAX_uint32)));
		Instance.IndexOffset = uint32(FMath::Min(IndexCount, uint64(MAX_uint32)));
		VertexCount += uint64(Range.VertexCount) + Instance.RefinedVertexCount;
		IndexCount += Instance.RefinedTriangleCount > 0 ? uint64(Instance.RefinedTriangleCount) * 3u : uint64(Range.IndexCount);
	}
	if (NumDrawnInstances >= uint32(Instances.Num()))
	{
		DrawnVertexCount = VertexCount;
		DrawnIndexCount = IndexCount;
	}
	if (VertexCount > uint64(MAX_int32) || IndexCount > uint64(MAX_int32))
	{
		OutError = FString::Printf(TEXT("The road mesh needs %llu vertices / %llu indices"), VertexCount, IndexCount);
		return false;
	}
	NumVertices = uint32(VertexCount);
	NumIndices = uint32(IndexCount);
	NumDrawnVertices = uint32(DrawnVertexCount);
	NumDrawnIndices = uint32(DrawnIndexCount);
	return true;
}

void AddRoadMeshPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target)
{
	CSRoadGpuMesh_AddPasses(GraphBuilder, Geometry, Mesh, Target, ECSRoadGpuMeshStage::Build);
}

bool RunRefineCountsBlocking(const FGpuGeometryInput& Geometry, FGpuRoadMeshInput& Mesh, FString& OutError, FGpuGeometryResult* OutGeometry, FPooledGeometry* Shared)
{
	const bool bCounts = Mesh.NeedsRefinementCounts();
	if (!bCounts && !OutGeometry) return true;
	TArray<FUintVector4> Totals;
	Totals.SetNumZeroed(Mesh.RefineInstances.Num());
	TArray<TPair<void*, uint32>> Targets;
	if (bCounts) Targets.Add({ Totals.GetData(), uint32(Totals.Num() * sizeof(FUintVector4)) });
	if (OutGeometry)
	{
		const uint32 NumEntities = Geometry.NumEntities;
		OutGeometry->Nodes.Reset();
		OutGeometry->Edges.SetNumZeroed(NumEntities);
		OutGeometry->StartNodes.SetNumZeroed(NumEntities);
		OutGeometry->EndNodes.SetNumZeroed(NumEntities);
		Targets.Add({ OutGeometry->Edges.GetData(), NumEntities * uint32(sizeof(EdgeGeometry)) });
		Targets.Add({ OutGeometry->StartNodes.GetData(), NumEntities * uint32(sizeof(EdgeNodeGeometry)) });
		Targets.Add({ OutGeometry->EndNodes.GetData(), NumEntities * uint32(sizeof(EdgeNodeGeometry)) });
	}
	const bool bRead = RunGraphBlocking([&Geometry, &Mesh, bCounts, OutGeometry, Shared](FRDGBuilder& GraphBuilder)
	{
		const bool bReuse = Shared && Shared->IsValid();
		const FGpuGeometryOutput GeometryOut = bReuse ? Shared->Register(GraphBuilder) : AddGeometryPasses(GraphBuilder, Geometry);
		if (Shared && !bReuse) Shared->Extract(GraphBuilder, GeometryOut);
		TArray<FRDGBufferRef> Buffers;
		if (bCounts) Buffers.Add(CSRoadGpuMesh_AddPasses(GraphBuilder, GeometryOut, Mesh, FGpuRoadMeshTarget(), ECSRoadGpuMeshStage::Counts).RefineTotals);
		if (OutGeometry) Buffers.Append({ GeometryOut.EdgeGeometry, GeometryOut.StartNodeGeometry, GeometryOut.EndNodeGeometry });
		return Buffers;
	}, Targets);
	if (!bRead)
	{
		OutError = TEXT("The GPU refinement count readback did not complete");
		return false;
	}
	return !bCounts || Mesh.ApplyRefinement(Totals, OutError);
}

bool RunDeformBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<float>& OutVertices, TArray<uint32>* OutPartners,
	TArray<uint32>* OutIndices, FString& OutError)
{
	if (Mesh.NumVertices == 0)
	{
		OutError = TEXT("The road mesh has no vertices");
		return false;
	}
	if (Mesh.NeedsRefinementCounts())
	{
		OutError = TEXT("The road mesh has refined instances but no refinement counts (RunRefineCountsBlocking)");
		return false;
	}
	OutVertices.SetNumZeroed(int32(Mesh.NumVertices * 8u));
	TArray<TPair<void*, uint32>> Targets = { { OutVertices.GetData(), Mesh.NumVertices * 8u * uint32(sizeof(float)) } };
	if (OutPartners)
	{
		OutPartners->SetNumZeroed(Mesh.Instances.Num() * 2);
		Targets.Add({ OutPartners->GetData(), uint32(OutPartners->Num() * sizeof(uint32)) });
	}
	if (OutIndices)
	{
		OutIndices->SetNumZeroed(int32(Mesh.NumIndices));
		Targets.Add({ OutIndices->GetData(), Mesh.NumIndices * uint32(sizeof(uint32)) });
	}
	const bool bRead = RunGraphBlocking([&Geometry, &Mesh, OutPartners, OutIndices](FRDGBuilder& GraphBuilder)
	{
		const FGpuGeometryOutput GeometryOut = AddGeometryPasses(GraphBuilder, Geometry);
		FGpuRoadMeshTarget MeshTarget;
		MeshTarget.DebugVertices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Mesh.NumVertices * 8u), TEXT("CSRoad.DeformedVertices"));
		if (OutIndices)
		{
			MeshTarget.Indices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1u, Mesh.NumIndices)), TEXT("CSRoad.DebugIndices"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshTarget.Indices, PF_R32_UINT)), 0u);
			MeshTarget.IndexCapacity = Mesh.NumIndices;
		}
		const FCSRoadGpuMesh_Outputs Outputs = CSRoadGpuMesh_AddPasses(GraphBuilder, GeometryOut, Mesh, MeshTarget, ECSRoadGpuMeshStage::Build);
		TArray<FRDGBufferRef> Buffers = { MeshTarget.DebugVertices };
		if (OutPartners) Buffers.Add(Outputs.Partners);
		if (OutIndices) Buffers.Add(MeshTarget.Indices);
		return Buffers;
	}, Targets);
	if (!bRead) OutError = TEXT("The GPU deform readback did not complete");
	return bRead;
}

bool RunJunctionBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<uint32>& OutPartners, FString& OutError)
{
	// Parameters and seam matching only: an input without instances skips the rest.
	FGpuRoadMeshInput MatchOnly;
	MatchOnly.RoadOfEntity = Mesh.RoadOfEntity;
	OutPartners.SetNumZeroed(Geometry.EdgeList.Num() * 6);
	if (OutPartners.Num() == 0) return true;
	const TArray<TPair<void*, uint32>> Targets = { { OutPartners.GetData(), uint32(OutPartners.Num() * sizeof(uint32)) } };
	const bool bRead = RunGraphBlocking([&Geometry, &MatchOnly](FRDGBuilder& GraphBuilder)
	{
		const FGpuGeometryOutput GeometryOut = AddGeometryPasses(GraphBuilder, Geometry);
		return TArray<FRDGBufferRef>{ CSRoadGpuMesh_AddPasses(GraphBuilder, GeometryOut, MatchOnly, FGpuRoadMeshTarget(), ECSRoadGpuMeshStage::Build).Partners };
	}, Targets);
	if (!bRead) OutError = TEXT("The GPU junction readback did not complete");
	return bRead;
}
}

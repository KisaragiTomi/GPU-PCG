#include "CSRoadGpuMesh.h"

#include "CSRoadGpuShaders.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RHIGPUReadback.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

static_assert(sizeof(CSRoad::FCompositionVertex) == 10 * sizeof(uint32), "FCompositionVertex must match the HLSL layout");
static_assert(sizeof(CSRoad::CompositionParameters) == 156 * sizeof(float), "CompositionParameters must match the HLSL layout");
static_assert(sizeof(CSRoad::FGpuMeshRange) == 4 * sizeof(uint32) && sizeof(CSRoad::FGpuMeshInstance) == 4 * sizeof(uint32), "Mesh tables are uint4 in HLSL");

BEGIN_SHADER_PARAMETER_STRUCT(FCSRoadMeshParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FCSRoadGeometryParameters, Geometry)
	SHADER_PARAMETER(uint32, NumInstances)
	SHADER_PARAMETER(uint32, InstanceGroupsX)
	SHADER_PARAMETER(uint32, OutputMode)
	SHADER_PARAMETER(FMatrix44f, LocalToWorld)
	SHADER_PARAMETER(uint32, VertexCapacity)
	SHADER_PARAMETER(uint32, IndexCapacity)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<CompositionParameters>, ParametersRW)
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

FRDGBufferRef CSRoadGpuMesh_AddPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target)
{
	check(Geometry.Parameters);
	RDG_EVENT_SCOPE(GraphBuilder, "CSRoadMesh");
	const uint32 NumEdges = Geometry.Parameters->NumEdges;
	const uint32 NumInstances = uint32(Mesh.Instances.Num());
	const bool bDebug = Target.DebugVertices != nullptr;
	FRDGBufferRef ParametersBuffer = GpuDetail::Zeroed<CompositionParameters>(GraphBuilder, NumEdges * 3u, TEXT("CSRoad.CompositionParameters"));

	FCSRoadMeshParameters Common;
	Common.Geometry = *Geometry.Parameters;
	Common.NumInstances = NumInstances;
	Common.InstanceGroupsX = FMath::Clamp(NumInstances, 1u, 65535u);
	Common.OutputMode = bDebug ? 1u : 0u;
	Common.LocalToWorld = Target.LocalToWorld;
	Common.VertexCapacity = Target.VertexCapacity;
	Common.IndexCapacity = Target.IndexCapacity;
	Common.ParametersRW = GraphBuilder.CreateUAV(ParametersBuffer);
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

	if (NumEdges == 0 || NumInstances == 0) return ParametersBuffer;
	CSRoadGpuMesh_AddPass<FCSRoadCalculateParametersCS>(GraphBuilder, Common, FComputeShaderUtils::GetGroupCount(int32(NumEdges), GpuDetail::GroupSize),
		RDG_EVENT_NAME("CalculateParameters"));
	const FIntVector InstanceGroups(int32(Common.InstanceGroupsX), int32(FMath::DivideAndRoundUp(NumInstances, 65535u)), 1);
	CSRoadGpuMesh_AddPass<FCSRoadDeformVerticesCS>(GraphBuilder, Common, InstanceGroups, RDG_EVENT_NAME("DeformVertices"));
	if (!bDebug) CSRoadGpuMesh_AddPass<FCSRoadWriteIndicesCS>(GraphBuilder, Common, InstanceGroups, RDG_EVENT_NAME("WriteIndices"));
	return ParametersBuffer;
}
}

bool FGpuRoadMeshInput::Build(const FGraph& Graph, FAssets& Assets, FString& OutError)
{
	Vertices.Reset();
	Indices.Reset();
	TriangleMaterials.Reset();
	MaterialSlots.Reset();
	Meshes.SetNumZeroed(Graph.Compositions.Num());
	TMap<int32, int32> SlotOfAtlas;
	for (int32 Index = 1; Index < Graph.Compositions.Num(); Index++)
	{
		FCompositionMesh Mesh;
		GenerateCompositionMesh(Assets, *Graph.Compositions[Index], Mesh);
		FGpuMeshRange& Range = Meshes[Index];
		Range.VertexOffset = uint32(Vertices.Num());
		Range.VertexCount = uint32(Mesh.Vertices.Num());
		Range.IndexOffset = uint32(Indices.Num());
		Range.IndexCount = uint32(Mesh.Indices.Num() / 3 * 3);
		Vertices.Append(Mesh.Vertices);
		for (int32 I = 0; I < int32(Range.IndexCount); I++) Indices.Add(uint32(Mesh.Indices[I]));
		const int32 FirstTriangle = TriangleMaterials.AddZeroed(int32(Range.IndexCount / 3));
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
		}
	}

	// Edge, start node and end node of every road, in road order (RoadRenderer.BuildSteps).
	Instances.Reset(Graph.Roads.Num() * 3);
	uint64 VertexCount = 0;
	uint64 IndexCount = 0;
	FBox Unity(ForceInit);
	float MaxHalfWidth = 0.0f;
	float MinHeight = 0.0f;
	float MaxHeight = 0.0f;
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++)
	{
		const int32 Entity = Graph.Roads[Road].Entity;
		const Composition& Compositions = Graph.CompositionData[Entity];
		const int32 MeshOf[3] = { Compositions.m_Edge, Compositions.m_StartNode, Compositions.m_EndNode };
		for (int32 Kind = 0; Kind < 3; Kind++)
		{
			FGpuMeshInstance& Instance = Instances.AddDefaulted_GetRef();
			Instance.Mesh = uint32(MeshOf[Kind]);
			Instance.Parameters = uint32(Road * 3 + Kind);
			Instance.VertexOffset = uint32(VertexCount);
			Instance.IndexOffset = uint32(IndexCount);
			VertexCount += Meshes[MeshOf[Kind]].VertexCount;
			IndexCount += Meshes[MeshOf[Kind]].IndexCount;
			const NetCompositionData& Data = Graph.Compositions[MeshOf[Kind]]->Data;
			MaxHalfWidth = FMath::Max(MaxHalfWidth, Data.m_Width * 0.5f);
			MinHeight = FMath::Min(MinHeight, Data.m_HeightRange.min);
			MaxHeight = FMath::Max(MaxHeight, Data.m_HeightRange.max);
		}
		const Bezier4x3& Curve = Graph.CurveData[Entity].m_Bezier;
		for (const float3& P : { Curve.a, Curve.b, Curve.c, Curve.d }) Unity += FVector(P.x, P.y, P.z);
	}
	if (VertexCount > uint64(MAX_int32) || IndexCount > uint64(MAX_int32))
	{
		OutError = FString::Printf(TEXT("The road mesh needs %llu vertices / %llu indices"), VertexCount, IndexCount);
		return false;
	}
	NumVertices = uint32(VertexCount);
	NumIndices = uint32(IndexCount);
	// Nodes widen past the curves by about a road width; terrain-following vertices may drop to the
	// ground under elevated roads.
	if (Unity.IsValid)
	{
		const double Margin = double(MaxHalfWidth) * 2.0 + 2.0;
		Unity.Min += FVector(-Margin, double(MinHeight) - 100.0, -Margin);
		Unity.Max += FVector(Margin, double(MaxHeight) + 10.0, Margin);
		Bounds = FBox(ForceInit);
		Bounds += FVector(Unity.Min.X, -Unity.Min.Z, Unity.Min.Y) * 100.0;
		Bounds += FVector(Unity.Max.X, -Unity.Max.Z, Unity.Max.Y) * 100.0;
	}
	return true;
}

void AddRoadMeshPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target)
{
	CSRoadGpuMesh_AddPasses(GraphBuilder, Geometry, Mesh, Target);
}

bool RunDeformBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<float>& OutVertices, FString& OutError)
{
	if (Mesh.NumVertices == 0)
	{
		OutError = TEXT("The road mesh has no vertices");
		return false;
	}
	const uint32 Bytes = Mesh.NumVertices * 8u * uint32(sizeof(float));
	OutVertices.SetNumZeroed(int32(Mesh.NumVertices * 8u));
	float* Target = OutVertices.GetData();
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSRoadDeformBlocking)([&Geometry, &Mesh, Target, Bytes, &bRead](FRHICommandListImmediate& RHICmdList)
	{
		FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSRoad.DeformReadback"));
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRoadDeformBlocking"));
			const FGpuGeometryOutput GeometryOut = AddGeometryPasses(GraphBuilder, Geometry);
			FGpuRoadMeshTarget MeshTarget;
			MeshTarget.DebugVertices = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), Mesh.NumVertices * 8u), TEXT("CSRoad.DeformedVertices"));
			CSRoadGpuMesh_AddPasses(GraphBuilder, GeometryOut, Mesh, MeshTarget);
			AddEnqueueCopyPass(GraphBuilder, Readback, MeshTarget.DebugVertices, Bytes);
			GraphBuilder.Execute();
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		const void* Data = Readback->IsReady() ? Readback->Lock(Bytes) : nullptr;
		if (Data)
		{
			FMemory::Memcpy(Target, Data, Bytes);
			Readback->Unlock();
			bRead = true;
		}
		delete Readback;
	});
	FlushRenderingCommands();
	if (!bRead) OutError = TEXT("The GPU deform readback did not complete");
	return bRead;
}
}

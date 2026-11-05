#include "CSGpuMeshTypes.h"

#include "RenderGraphBuilder.h"

namespace CSGpuMeshStreams
{
void BuildStandardTriangleStreamDescs(TArray<FCSGpuStreamDesc>& OutDescs, const FStandardStreamOptions& Options)
{
	OutDescs.Reset();

	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Positions");
		D.Role = ECSGpuStreamRole::Position;
		D.BytesPerElement = sizeof(float);
		D.ElementsPerUnit = 3;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R32_FLOAT;
		D.VfType = VET_Float3;
		D.CpuSemantic = ECSGpuMeshSemantic::Position;
		D.bReadback = true;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Tangents");
		D.Role = ECSGpuStreamRole::TangentBasis;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 2;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R8G8B8A8_SNORM;
		D.VfType = VET_PackedNormal;
		D.CpuSemantic = ECSGpuMeshSemantic::TangentBasis;
		D.bReadback = true;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.TexCoords");
		D.Role = ECSGpuStreamRole::TexCoord;
		D.BytesPerElement = sizeof(float);
		// 交错：每顶点 2×N 个 float。N=1 时与改动前逐位相同。SrvFormat 仍是 float2 视图，
		// 于是 SRV 的元素数天然变成 N×顶点数，正好是引擎交错取数期待的排布。
		D.ElementsPerUnit = 2 * FMath::Clamp(Options.NumTexCoordSets, 1u, uint32(FCSGpuMeshCPUData::MaxTexCoordChannels));
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_G32R32F;
		D.VfType = VET_Float2;
		D.TexCoordIndex = 0;
		D.CpuSemantic = ECSGpuMeshSemantic::TexCoord;
		D.bReadback = true;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Colors");
		D.Role = ECSGpuStreamRole::Color;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 1;
		D.CountSource = ECSGpuCountSource::PerVertex;
		D.SrvFormat = PF_R8G8B8A8;
		D.VfType = VET_Color;
		D.CpuSemantic = Options.bReadbackColors ? ECSGpuMeshSemantic::Color : ECSGpuMeshSemantic::None;
		D.bReadback = Options.bReadbackColors;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.Indices");
		D.Role = ECSGpuStreamRole::Index;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 1;
		D.CountSource = ECSGpuCountSource::PerIndex;
		D.SrvFormat = PF_Unknown;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::Index;
		D.bReadback = true;
		OutDescs.Add(D);
	}
	if (Options.bMaterialIds)
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.MaterialIds");
		D.Role = ECSGpuStreamRole::AuxVertex;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 1;
		D.CountSource = ECSGpuCountSource::PerTriangle;
		D.SrvFormat = PF_R32_UINT;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::MaterialId;
		D.bReadback = true;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.IndirectArgs");
		D.Role = ECSGpuStreamRole::IndirectArgs;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 5 * FMath::Max(Options.NumIndirectDraws, 1u);
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = PF_Unknown;
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::None;
		D.bReadback = false;
		OutDescs.Add(D);
	}
	{
		FCSGpuStreamDesc D;
		D.DebugName = TEXT("CSGpuMesh.MeshCounters");
		D.Role = ECSGpuStreamRole::MeshCounters;
		D.BytesPerElement = sizeof(uint32);
		D.ElementsPerUnit = 2; // [0]=vertexCount, [1]=indexCount
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = PF_Unknown; // operators create the SRV inside their own graph
		D.VfType = VET_None;
		D.CpuSemantic = ECSGpuMeshSemantic::None; // read via the counters readback, not the mesh loop
		D.bReadback = false;
		OutDescs.Add(D);
	}
}

uint32 UnitsForCountSource(ECSGpuCountSource CountSource, uint32 VertexUnits, uint32 IndexUnits)
{
	switch (CountSource)
	{
	case ECSGpuCountSource::PerVertex:   return VertexUnits;
	case ECSGpuCountSource::PerIndex:    return IndexUnits;
	case ECSGpuCountSource::PerTriangle: return IndexUnits / 3u;
	default:                             return 1u; // Fixed: ElementsPerUnit is the whole count
	}
}

ERHIAccess FinalAccessForRole(ECSGpuStreamRole Role)
{
	switch (Role)
	{
	case ECSGpuStreamRole::Index:
		// Drawn from as an index buffer AND read as a plain shader resource by an acceleration
		// structure build: a BLAS build reads its index and vertex inputs in SRV state (the
		// engine's own dynamic-geometry path transitions UAVCompute -> SRVMask right before it
		// builds, RayTracingDynamicGeometryUpdateManager.cpp:938 in 5.7.4) and the RHI does not
		// transition build inputs for you. Without SRVMask here the BLAS FCSGpuMeshSceneProxy
		// builds would read the index buffer in a state it was never put in.
		return ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask;
	case ECSGpuStreamRole::IndirectArgs:
		return ERHIAccess::IndirectArgs;
	case ECSGpuStreamRole::MeshCounters:
		// The counters buffer is the readback carrier; leaving it in CopySrc is what lets
		// ReadbackMeshSync enqueue its copy without an extra transition.
		return ERHIAccess::CopySrc;
	case ECSGpuStreamRole::AuxVertex:
		// No vertex-factory binding, so plain shader access is enough.
		return ERHIAccess::SRVMask;
	default:
		// Vertex streams are both drawn from and manually fetched by the vertex factory.
		return ERHIAccess::VertexOrIndexBuffer | ERHIAccess::SRVMask;
	}
}

void SetStreamAccessFinal(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, ECSGpuStreamRole Role)
{
	if (!Buffer) return;
	GraphBuilder.SetBufferAccessFinal(Buffer, FinalAccessForRole(Role));
}
}

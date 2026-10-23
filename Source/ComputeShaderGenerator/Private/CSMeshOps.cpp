#include "CSMeshOps.h"

#include "CSMesh.h"
#include "CSBoxSceneCollection.h"
#include "CSGpuMeshComponent.h"
#include "CSGpuTriangleUtilities.h"
#include "CSGroundShaperField.h"
#include "ComputeShaderGenerateHelper.h"
#include "ComputeShaderMeshBoolean.h"
#include "ComputeShaderMeshGenerator.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "DynamicMesh/MeshNormals.h"
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "Materials/MaterialInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "RHIResourceUtils.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"
#include "UDynamicMesh.h"
#include "UObject/Package.h"   // GetTransientPackage() 的 UPackage 完整定义；unity 构建下靠邻居 TU 兜住

DEFINE_LOG_CATEGORY_STATIC(LogCSMeshOps, Log, All);

// -----------------------------------------------------------------------------
// Shaders (Shaders/Private/CSMeshOps.usf)
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU, so file-local names in this module carry a CSMeshOps_ prefix.
constexpr uint32 CSMeshOps_GroupSize = 64;

/** Dummy typed buffers for sources a mesh may simply not have (no UVs, no vertex colours).
 *  Binding a null SRV to a parameter the shader actually declares is a crash, so every
 *  optional source gets a stand-in instead of a branch in the C++ (same trick, and the same
 *  PF_G32R32F / PF_A32B32G32R32F formats, as the extraction path's dummy tex-coord buffer). */
class FCSMeshOpsDummyTexCoordBuffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		const FVector2f Zero(0.0f, 0.0f);
		VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(
			RHICmdList, TEXT("CSMeshOps.DummyTexCoord"), EBufferUsageFlags::ShaderResource, MakeArrayView(&Zero, 1));
		ShaderResourceViewRHI = RHICmdList.CreateShaderResourceView(VertexBufferRHI,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_G32R32F));
	}
};
TGlobalResource<FCSMeshOpsDummyTexCoordBuffer> GCSMeshOpsDummyTexCoordBuffer;

class FCSMeshOpsDummyFloat4Buffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		// Two entries: the tangent stream is read as pairs, so a single element would let a
		// stray fetch run past the end even though the shader gates on SrcHasTangents.
		const FVector4f Zero[2] = { FVector4f(0, 0, 1, 1), FVector4f(0, 0, 1, 1) };
		VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(
			RHICmdList, TEXT("CSMeshOps.DummyFloat4"), EBufferUsageFlags::ShaderResource, MakeArrayView(Zero, 2));
		ShaderResourceViewRHI = RHICmdList.CreateShaderResourceView(VertexBufferRHI,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_A32B32G32R32F));
	}
};
TGlobalResource<FCSMeshOpsDummyFloat4Buffer> GCSMeshOpsDummyFloat4Buffer;
}

class FCSMeshOpsSetCountersCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsSetCountersCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsSetCountersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_IndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MeshCounters)
		SHADER_PARAMETER(uint32, DstVertexBase)
		SHADER_PARAMETER(uint32, DstIndexBase)
		SHADER_PARAMETER(uint32, VertexCapacity)
		SHADER_PARAMETER(uint32, IndexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

class FCSMeshOpsUploadVerticesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsUploadVerticesCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsUploadVerticesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<float>, SrcPositionBuffer)
		SHADER_PARAMETER_SRV(Buffer<float4>, SrcTangentBuffer)
		SHADER_PARAMETER_SRV(Buffer<float2>, SrcTexCoordBuffer)
		SHADER_PARAMETER_SRV(Buffer<float4>, SrcColorBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_TexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
		SHADER_PARAMETER(FMatrix44f, SrcLocalToWorld)
		SHADER_PARAMETER(FMatrix44f, SrcNormalToWorld)
		SHADER_PARAMETER(uint32, SrcVertexCount)
		SHADER_PARAMETER(uint32, SrcPositionStrideFloat)
		SHADER_PARAMETER(uint32, SrcNumTexCoords)
		SHADER_PARAMETER(uint32, SrcHasTangents)
		SHADER_PARAMETER(uint32, SrcHasColors)
		SHADER_PARAMETER(uint32, DstVertexBase)
		SHADER_PARAMETER(uint32, VertexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsUploadIndicesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsUploadIndicesCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsUploadIndicesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<uint>, SrcIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcTriToMaterial)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
		SHADER_PARAMETER(uint32, SrcIndexCount)
		SHADER_PARAMETER(uint32, SrcTriangleCount)
		SHADER_PARAMETER(uint32, MaterialIdOffset)
		SHADER_PARAMETER(uint32, DstVertexBase)
		SHADER_PARAMETER(uint32, DstIndexBase)
		SHADER_PARAMETER(uint32, IndexCapacity)
		SHADER_PARAMETER(uint32, bFlipWinding)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsAppendSoupCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsAppendSoupCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsAppendSoupCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcSoupVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcSoupNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, SrcSoupUVs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcSoupMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcSoupCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_TexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
		SHADER_PARAMETER(uint32, SrcTriangleCount)
		SHADER_PARAMETER(uint32, SrcSoupUVChannels)
		SHADER_PARAMETER(uint32, MaterialIdOffset)
		SHADER_PARAMETER(uint32, VertexCapacity)
		SHADER_PARAMETER(uint32, IndexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsAdvanceCountersCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsAdvanceCountersCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsAdvanceCountersCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcSoupCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_IndirectArgs)
		SHADER_PARAMETER(uint32, SrcTriangleCount)
		SHADER_PARAMETER(uint32, VertexCapacity)
		SHADER_PARAMETER(uint32, IndexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

class FCSMeshOpsTransformCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsTransformCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsTransformCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER(FMatrix44f, SrcLocalToWorld)
		SHADER_PARAMETER(FMatrix44f, SrcNormalToWorld)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsFlipWindingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsFlipWindingCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsFlipWindingCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsNegateNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsNegateNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsNegateNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsWeldCounterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsWeldCounterCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsWeldCounterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_WeldTriangleCounter)
		SHADER_PARAMETER(uint32, IndexCapacity)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

class FCSMeshOpsWeldSoupCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsWeldSoupCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsWeldSoupCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, WeldSrcPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldSrcIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector3f>, RW_WeldSoup)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsWeldRemapCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsWeldRemapCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsWeldRemapCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldSrcIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, WeldRepresentatives)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsSetColorsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsSetColorsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsSetColorsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
		SHADER_PARAMETER(FVector4f, ConstantColor)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsPaintColorsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsPaintColorsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsPaintColorsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, BrushSrcPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
		SHADER_PARAMETER(FVector4f, ConstantColor)
		SHADER_PARAMETER(FVector4f, BrushChannelMask)
		SHADER_PARAMETER(FVector3f, BrushCenter)
		SHADER_PARAMETER(float, BrushRadius)
		SHADER_PARAMETER(float, BrushFalloff)
		SHADER_PARAMETER(float, BrushStrength)
		SHADER_PARAMETER(uint32, PaintBlendOp)
		SHADER_PARAMETER(FUintVector4, BrushRegion)     // 规则网格时的格点闭区间 (x0, y0, x1, y1)
		SHADER_PARAMETER(uint32, BrushGridVertsX)       // 0 = 全网格派发（非规则网格）
		SHADER_PARAMETER(uint32, BrushGridVertsY)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsDisplaceGroundCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsDisplaceGroundCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsDisplaceGroundCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundShaperParams)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER(FVector2f, GroundOriginXY)
		SHADER_PARAMETER(float, GroundCellSize)
		SHADER_PARAMETER(float, GroundBaseZ)
		SHADER_PARAMETER(uint32, GroundShaperCount)
		SHADER_PARAMETER(uint32, GroundVertsX)
		SHADER_PARAMETER(uint32, GroundVertsY)
		SHADER_PARAMETER(FUintVector4, GroundRegion)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsMaterialHistogramCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsMaterialHistogramCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsMaterialHistogramCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortSrcMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SlotCounters)
		SHADER_PARAMETER(uint32, NumMaterialSlots)
		SHADER_PARAMETER(uint32, IndexCapacity)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsMaterialScanCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsMaterialScanCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsMaterialScanCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InSlotCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SlotCursors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_IndirectArgs)
		SHADER_PARAMETER(uint32, NumMaterialSlots)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

class FCSMeshOpsMaterialScatterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsMaterialScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsMaterialScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortSrcIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SortSrcMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SlotCursors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
		SHADER_PARAMETER(uint32, NumMaterialSlots)
		SHADER_PARAMETER(uint32, IndexCapacity)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

class FCSMeshOpsClearBoundsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsClearBoundsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsClearBoundsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MeshBounds)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

class FCSMeshOpsReduceBoundsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSMeshOpsReduceBoundsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSMeshOpsReduceBoundsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, InMeshCounters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, BoundsSrcPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MeshBounds)
		SHADER_PARAMETER(uint32, VertexCapacity)
		SHADER_PARAMETER(uint32, ThreadCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsSetCountersCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "SetCountersCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsUploadVerticesCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "UploadStaticMeshVerticesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsUploadIndicesCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "UploadStaticMeshIndicesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsAppendSoupCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "AppendTriangleSoupCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsAdvanceCountersCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "AdvanceCountersBySoupCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsTransformCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "TransformMeshCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsFlipWindingCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "FlipNormalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsNegateNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "NegateNormalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsSetColorsCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "SetVertexColorsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsPaintColorsCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "PaintVertexColorsSphereCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsDisplaceGroundCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "DisplaceGroundShapersCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsWeldCounterCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "BuildWeldCounterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsWeldSoupCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "BuildWeldSoupCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsWeldRemapCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "RemapIndicesByWeldCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsMaterialHistogramCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "HistogramMaterialSlotsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsMaterialScanCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "ScanMaterialSlotsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsMaterialScatterCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "ScatterTrianglesByMaterialCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsClearBoundsCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "ClearMeshBoundsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSMeshOpsReduceBoundsCS, "/Plugin/PCGPlugins/Shaders/Private/CSMeshOps.usf", "ReduceMeshBoundsCS", SF_Compute);

// -----------------------------------------------------------------------------
// Shared pass helpers
// -----------------------------------------------------------------------------

namespace
{
FGlobalShaderMap* CSMeshOps_ShaderMap()
{
	return GetGlobalShaderMap(GMaxRHIFeatureLevel);
}

FRDGBufferUAVRef CSMeshOps_UAV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, EPixelFormat Format)
{
	return Buffer ? GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, Format)) : nullptr;
}

FRDGBufferSRVRef CSMeshOps_SRV(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer, EPixelFormat Format)
{
	return Buffer ? GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buffer, Format)) : nullptr;
}

/** True when the context has every stream the vertex-writing operators need. */
bool CSMeshOps_HasVertexStreams(const FCSMeshEditContext& Context)
{
	return Context.Positions() && Context.Tangents() && Context.TexCoords() && Context.Colors()
		&& Context.Indices() && Context.MaterialIds() && Context.Counters() && Context.IndirectArgs();
}

/** CPU twins of the shader's packers, for the snapshot-upload path (which has no kernel). */
uint32 CSMeshOps_PackSnorm8888(const FVector4f& Value)
{
	auto PackByte = [](float Component)
	{
		const int32 Quantized = FMath::RoundToInt(FMath::Clamp(Component, -1.0f, 1.0f) * 127.0f);
		return uint32(Quantized) & 0xffu;
	};
	return PackByte(Value.X) | (PackByte(Value.Y) << 8) | (PackByte(Value.Z) << 16) | (PackByte(Value.W) << 24);
}

uint32 CSMeshOps_PackColorBGRA(const FVector4f& RGBA)
{
	auto PackByte = [](float Component)
	{
		return uint32(FMath::RoundToInt(FMath::Clamp(Component, 0.0f, 1.0f) * 255.0f)) & 0xffu;
	};
	return PackByte(RGBA.Z) | (PackByte(RGBA.Y) << 8) | (PackByte(RGBA.X) << 16) | (PackByte(RGBA.W) << 24);
}

/** CPU twin of LBVHFloatUnflip (CSGpuTriangleUtilities.ush:325), for decoding the bounds
 *  reduction's ordered uints. Bit-cast through memory rather than a pointer cast: the union /
 *  reinterpret_cast spellings are the ones that turn into a strict-aliasing bug. */
float CSMeshOps_OrderedUintToFloat(uint32 Ordered)
{
	const uint32 Bits = Ordered ^ (((Ordered >> 31) - 1u) | 0x80000000u);
	float Result = 0.0f;
	FMemory::Memcpy(&Result, &Bits, sizeof(float));
	return Result;
}

/** Per-triangle material-registry ids for a LOD, derived from its section table. */
void CSMeshOps_BuildTriangleMaterialIds(const FStaticMeshLODResources& LOD, TArray<uint32>& OutIds)
{
	OutIds.Init(0u, FMath::Max(LOD.GetNumTriangles(), 1));
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		const int32 FirstTriangle = int32(Section.FirstIndex / 3u);
		for (uint32 Offset = 0; Offset < Section.NumTriangles; ++Offset)
		{
			const int32 Triangle = FirstTriangle + int32(Offset);
			if (OutIds.IsValidIndex(Triangle)) OutIds[Triangle] = uint32(FMath::Max(Section.MaterialIndex, 0));
		}
	}
}
}

void UCSMeshOps::InvalidateSections(FCSMeshEditContext& Context)
{
	// The context decides how the drop reaches the game thread: behind the flush of a synchronous
	// edit, from the completion hop of an asynchronous one — never straight from the render thread.
	Context.InvalidateSections();
}

void UCSMeshOps::AddSetCountersPass(FCSMeshEditContext& Context, uint32 VertexCount, uint32 IndexCount)
{
	FRDGBufferRef Args = Context.IndirectArgs();
	FRDGBufferRef Counters = Context.Counters();
	if (!Args || !Counters) return;

	// SetCountersCS writes arg set 0 only. On a sectioned mesh that turns set 0 into a whole-mesh
	// draw while sets 1..N-1 keep describing runs of a triangle layout this operator has just
	// replaced, so the table goes with them.
	InvalidateSections(Context);

	FCSMeshOpsSetCountersCS::FParameters* Params = Context.GraphBuilder.AllocParameters<FCSMeshOpsSetCountersCS::FParameters>();
	Params->RW_IndirectArgs = CSMeshOps_UAV(Context.GraphBuilder, Args, PF_R32_UINT);
	Params->RW_MeshCounters = CSMeshOps_UAV(Context.GraphBuilder, Counters, PF_R32_UINT);
	Params->DstVertexBase = VertexCount;
	Params->DstIndexBase = IndexCount;
	Params->VertexCapacity = Context.Resident.VertexCapacity;
	Params->IndexCapacity = Context.Resident.IndexCapacity;

	TShaderMapRef<FCSMeshOpsSetCountersCS> Shader(CSMeshOps_ShaderMap());
	FComputeShaderUtils::AddPass(Context.GraphBuilder, RDG_EVENT_NAME("CSMeshOps.SetCounters"),
		Shader, Params, FIntVector(1, 1, 1));

	Context.SetKnownCounts(int32(VertexCount), int32(IndexCount));
}

// -----------------------------------------------------------------------------
// Creation
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshOps::AllocateGpuMesh(UObject* Outer, int32 VertexCapacity, int32 IndexCapacity)
{
	UCSMesh* Mesh = NewObject<UCSMesh>(Outer ? Outer : GetTransientPackage());
	Mesh->EnsureCapacitySync(VertexCapacity, IndexCapacity);
	return Mesh;
}

// -----------------------------------------------------------------------------
// CopyFromStaticMesh
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshOps::CopyFromStaticMesh(UCSMesh* Target, UStaticMesh* Source, const FCSMeshFromStaticMeshOptions& Options)
{
	if (!Target || !Source) return Target;

	const FStaticMeshRenderData* RenderData = Source->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyFromStaticMesh: '%s' has no render data."), *Source->GetName());
		return Target;
	}
	const int32 LODIndex = FMath::Clamp(Options.LODIndex, 0, RenderData->LODResources.Num() - 1);
	const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];

	const int32 SourceVertexCount = LOD.GetNumVertices();
	const int32 SourceTriangleCount = LOD.GetNumTriangles();
	if (SourceVertexCount < 3 || SourceTriangleCount < 1)
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyFromStaticMesh: '%s' LOD%d is empty."), *Source->GetName(), LODIndex);
		return Target;
	}
	const int32 SourceIndexCount = SourceTriangleCount * 3;

	Target->EnsureCapacitySync(SourceVertexCount, SourceIndexCount);

	if (Options.bCopyMaterials)
	{
		Target->Materials.Reset();
		for (const FStaticMaterial& StaticMaterial : Source->GetStaticMaterials())
			Target->Materials.Add(StaticMaterial.MaterialInterface);
	}

	TArray<uint32> TriangleMaterialIds;
	CSMeshOps_BuildTriangleMaterialIds(LOD, TriangleMaterialIds);

	const FMatrix44f LocalToWorld(Options.Transform.ToMatrixWithScale());
	// Normals need the inverse transpose; a non-uniform scale otherwise shears them off the
	// surface. UE stores row-vector matrices, so the transpose goes on the inverse.
	const FMatrix44f NormalToWorld(Options.Transform.ToMatrixWithScale().Inverse().GetTransposed());

	// Bounds are known on the CPU here, unlike the GPU-decided paths.
	const FBox SourceBounds = Source->GetBoundingBox().TransformBy(Options.Transform);

	const FStaticMeshLODResources* LODPtr = &LOD;
	const bool bFlipWinding = Options.bFlipWinding;

	Target->EditMeshSync([LODPtr, SourceVertexCount, SourceIndexCount, SourceTriangleCount,
		LocalToWorld, NormalToWorld, bFlipWinding, &TriangleMaterialIds, SourceBounds](FCSMeshEditContext& Context)
	{
		if (!CSMeshOps_HasVertexStreams(Context)) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;

		FRHIShaderResourceView* PositionSRV = LODPtr->VertexBuffers.PositionVertexBuffer.GetSRV();
		FRHIShaderResourceView* TangentSRV = LODPtr->VertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
		FRHIShaderResourceView* TexCoordSRV = LODPtr->VertexBuffers.StaticMeshVertexBuffer.GetTexCoordsSRV();
		FRHIShaderResourceView* ColorSRV = LODPtr->VertexBuffers.ColorVertexBuffer.GetColorComponentsSRV();
		const uint32 NumTexCoords = TexCoordSRV ? LODPtr->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() : 0u;
		if (!PositionSRV)
		{
			UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyFromStaticMesh: source position SRV unavailable."));
			return;
		}

		const bool bHasTangents = TangentSRV != nullptr;
		const bool bHasColors = ColorSRV != nullptr && LODPtr->VertexBuffers.ColorVertexBuffer.GetNumVertices() >= uint32(SourceVertexCount);
		if (!TangentSRV) TangentSRV = GCSMeshOpsDummyFloat4Buffer.ShaderResourceViewRHI.GetReference();
		if (!ColorSRV) ColorSRV = GCSMeshOpsDummyFloat4Buffer.ShaderResourceViewRHI.GetReference();
		if (!TexCoordSRV || NumTexCoords == 0u) TexCoordSRV = GCSMeshOpsDummyTexCoordBuffer.ShaderResourceViewRHI.GetReference();

		{
			FCSMeshOpsUploadVerticesCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsUploadVerticesCS::FParameters>();
			Params->SrcPositionBuffer = PositionSRV;
			Params->SrcTangentBuffer = TangentSRV;
			Params->SrcTexCoordBuffer = TexCoordSRV;
			Params->SrcColorBuffer = ColorSRV;
			Params->RW_Positions = CSMeshOps_UAV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
			Params->RW_Tangents = CSMeshOps_UAV(GraphBuilder, Context.Tangents(), PF_R32_UINT);
			Params->RW_TexCoords = CSMeshOps_UAV(GraphBuilder, Context.TexCoords(), PF_R32_FLOAT);
			Params->RW_Colors = CSMeshOps_UAV(GraphBuilder, Context.Colors(), PF_R32_UINT);
			Params->SrcLocalToWorld = LocalToWorld;
			Params->SrcNormalToWorld = NormalToWorld;
			Params->SrcVertexCount = uint32(SourceVertexCount);
			Params->SrcPositionStrideFloat = FMath::Max(3u, LODPtr->VertexBuffers.PositionVertexBuffer.GetStride() / uint32(sizeof(float)));
			Params->SrcNumTexCoords = NumTexCoords;
			Params->SrcHasTangents = bHasTangents ? 1u : 0u;
			Params->SrcHasColors = bHasColors ? 1u : 0u;
			Params->DstVertexBase = 0;
			Params->VertexCapacity = Context.Resident.VertexCapacity;

			TShaderMapRef<FCSMeshOpsUploadVerticesCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.UploadVertices"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(SourceVertexCount, CSMeshOps_GroupSize));
		}

		{
			// The index buffer's own SRV lives on the LOD; it stays alive because the caller
			// holds the static mesh across this synchronous edit.
			FBufferRHIRef IndexBufferRHI = LODPtr->IndexBuffer.GetRHI();
			if (!IndexBufferRHI.IsValid())
			{
				UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyFromStaticMesh: source index buffer unavailable."));
				return;
			}
			FShaderResourceViewRHIRef IndexSRV = FRHICommandListExecutor::GetImmediateCommandList().CreateShaderResourceView(
				IndexBufferRHI,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed)
					.SetFormat(LODPtr->IndexBuffer.Is32Bit() ? PF_R32_UINT : PF_R16_UINT));
			// SHADER_PARAMETER_SRV does not take a reference, and the graph executes after this
			// lambda returns — the context has to hold the SRV until then.
			Context.KeepAliveResource(IndexSRV);

			FRDGBufferRef TriToMaterial = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(TriangleMaterialIds.Num(), 1)),
				TEXT("CSMeshOps.TriToMaterial"));
			uint32* Upload = GraphBuilder.AllocPODArray<uint32>(FMath::Max(TriangleMaterialIds.Num(), 1));
			FMemory::Memcpy(Upload, TriangleMaterialIds.GetData(), TriangleMaterialIds.Num() * sizeof(uint32));
			GraphBuilder.QueueBufferUpload(TriToMaterial, Upload, TriangleMaterialIds.Num() * sizeof(uint32));

			FCSMeshOpsUploadIndicesCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsUploadIndicesCS::FParameters>();
			Params->SrcIndexBuffer = IndexSRV.GetReference();
			Params->SrcTriToMaterial = CSMeshOps_SRV(GraphBuilder, TriToMaterial, PF_R32_UINT);
			Params->RW_Indices = CSMeshOps_UAV(GraphBuilder, Context.Indices(), PF_R32_UINT);
			Params->RW_MaterialIds = CSMeshOps_UAV(GraphBuilder, Context.MaterialIds(), PF_R32_UINT);
			Params->SrcIndexCount = uint32(SourceIndexCount);
			Params->SrcTriangleCount = uint32(SourceTriangleCount);
			Params->MaterialIdOffset = 0;
			Params->DstVertexBase = 0;
			Params->DstIndexBase = 0;
			Params->IndexCapacity = Context.Resident.IndexCapacity;
			Params->bFlipWinding = bFlipWinding ? 1u : 0u;

			TShaderMapRef<FCSMeshOpsUploadIndicesCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.UploadIndices"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(SourceIndexCount, CSMeshOps_GroupSize));
		}

		AddSetCountersPass(Context, uint32(SourceVertexCount), uint32(SourceIndexCount));
		Context.SetWorldBounds(SourceBounds);
	});

	return Target;
}

// -----------------------------------------------------------------------------
// Sinks
// -----------------------------------------------------------------------------

UStaticMesh* UCSMeshOps::CopyToStaticMesh(
	UCSMesh* Target, UObject* Outer, AActor* OwnerActor, const FCSMeshToStaticMeshOptions& Options)
{
	if (!Target) return nullptr;

	FCSGpuMeshCPUData MeshData;
	if (!Target->ReadbackMeshSync(MeshData))
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyToStaticMesh: readback returned no valid triangles."));
		return nullptr;
	}

	FCSGpuMeshConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = Options.TargetTransform;
	ConvertOptions.bBakeToLocalSpace = Options.bBakeToLocalSpace;
	// 【未实装】Options.bRecomputeNormals（BP 面的 UPROPERTY，保留以免加载旧资产时静默丢值）在
	// StaticMesh 这条出口上没有任何效果：UCSGpuMeshComponent::BuildGpuMeshDescription 一律直接用
	// 快照里的法线。转发目标 FCSGpuMeshConvertOptions::bRecomputeNormals 已经删除——它全模块无人
	// 读取，留着只是让这条赋值看起来像在做事。DynamicMesh 那条出口是真读它的，不受影响。

	FCSGpuMeshAssetOptions AssetOptions;
	AssetOptions.AssetPath = Options.AssetPath.TrimStartAndEnd();
	AssetOptions.bTransient = Options.bTransient;
	AssetOptions.bReplaceExisting = Options.bReplaceExisting;
	AssetOptions.bSaveToDisk = Options.bSaveToDisk;
	AssetOptions.bEnableNanite = Options.bEnableNanite;

	return UCSGpuMeshComponent::BuildStaticMesh(
		Outer ? Outer : GetTransientPackage(), OwnerActor, MeshData,
		TArray<UMaterialInterface*>(), ConvertOptions, AssetOptions);
}

UDynamicMesh* UCSMeshOps::CopyToDynamicMesh(
	UCSMesh* Target, UDynamicMesh* TargetMesh, UObject* Outer, const FCSMeshToDynamicMeshOptions& Options)
{
	if (!Target) return TargetMesh;

	FCSGpuMeshCPUData MeshData;
	if (!Target->ReadbackMeshSync(MeshData))
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] CopyToDynamicMesh: readback returned no valid triangles."));
		return TargetMesh;
	}

	if (!TargetMesh) TargetMesh = NewObject<UDynamicMesh>(Outer ? Outer : GetTransientPackage());

	using namespace UE::Geometry;
	TargetMesh->EditMesh([&MeshData, &Options](FDynamicMesh3& Mesh)
	{
		Mesh.Clear();
		Mesh.EnableAttributes();
		FDynamicMeshAttributeSet* Attributes = Mesh.Attributes();
		if (Options.bTransferMaterialIDs) Attributes->EnableMaterialID();
		FDynamicMeshNormalOverlay* NormalOverlay = Attributes->PrimaryNormals();
		FDynamicMeshUVOverlay* UVOverlay = (Options.bTransferUVs && Attributes->NumUVLayers() > 0) ? Attributes->PrimaryUV() : nullptr;
		if (Options.bTransferColors && !MeshData.Colors.IsEmpty()) Attributes->EnablePrimaryColors();
		FDynamicMeshColorOverlay* ColorOverlay = (Options.bTransferColors && !MeshData.Colors.IsEmpty()) ? Attributes->PrimaryColors() : nullptr;

		const bool bBake = Options.bBakeToLocalSpace;
		const FTransform& Xf = Options.TargetTransform;

		const int32 VertexCount = MeshData.Positions.Num();
		TArray<int32> VertexIDs;
		VertexIDs.SetNumUninitialized(VertexCount);
		for (int32 Index = 0; Index < VertexCount; ++Index)
		{
			const FVector World(MeshData.Positions[Index]);
			const FVector Local = bBake ? Xf.InverseTransformPosition(World) : World;
			VertexIDs[Index] = Mesh.AppendVertex(Local);
		}

		// Attribute arrays may be indexed by position or by index corner (a welded boolean
		// result keeps per-corner seams); the same rule the StaticMesh converter uses.
		const int32 IndexCount = MeshData.Indices.Num();
		auto AttributeIndex = [IndexCount](int32 AttributeCount, int32 PositionIndex, int32 CornerIndex)
		{
			return AttributeCount == IndexCount ? CornerIndex : PositionIndex;
		};

		for (int32 Corner = 0; Corner + 2 < IndexCount; Corner += 3)
		{
			int32 A = int32(MeshData.Indices[Corner + 0]);
			int32 B = int32(MeshData.Indices[Corner + 1]);
			int32 C = int32(MeshData.Indices[Corner + 2]);
			if (!VertexIDs.IsValidIndex(A) || !VertexIDs.IsValidIndex(B) || !VertexIDs.IsValidIndex(C)) continue;
			if (A == B || B == C || A == C) continue;

			if (Options.bSkipDegenerateTriangles)
			{
				const FVector3f P0 = MeshData.Positions[A];
				const FVector3f P1 = MeshData.Positions[B];
				const FVector3f P2 = MeshData.Positions[C];
				if (FVector3f::CrossProduct(P1 - P0, P2 - P0).SizeSquared() <= UE_SMALL_NUMBER) continue;
			}

			int32 CornerA = Corner + 0, CornerB = Corner + 1, CornerC = Corner + 2;
			if (Options.bReverseOrientation)
			{
				Swap(B, C);
				Swap(CornerB, CornerC);
			}

			const int32 MaterialID = MeshData.TriangleMaterialSlots.IsValidIndex(Corner / 3)
				? FMath::Max(MeshData.TriangleMaterialSlots[Corner / 3], 0)
				: 0;

			const int32 TriangleID = Mesh.AppendTriangle(FIndex3i(VertexIDs[A], VertexIDs[B], VertexIDs[C]));
			if (TriangleID < 0) continue; // non-manifold: dropped, as every other sink does

			if (Options.bTransferMaterialIDs && Attributes->HasMaterialID())
				Attributes->GetMaterialID()->SetValue(TriangleID, MaterialID);

			const int32 SourceCorners[3] = { CornerA, CornerB, CornerC };
			const int32 SourceVertices[3] = { A, B, C };

			if (NormalOverlay && !Options.bRecomputeNormals && !MeshData.Normals.IsEmpty())
			{
				FIndex3i Elements;
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 NormalIndex = AttributeIndex(MeshData.Normals.Num(), SourceVertices[k], SourceCorners[k]);
					FVector3f Normal = MeshData.Normals.IsValidIndex(NormalIndex) ? MeshData.Normals[NormalIndex] : FVector3f::UnitZ();
					if (bBake) Normal = FVector3f(Xf.InverseTransformVectorNoScale(FVector(Normal)));
					Normal = Normal.GetSafeNormal();
					if (Options.bReverseOrientation) Normal = -Normal;
					Elements[k] = NormalOverlay->AppendElement(Normal);
				}
				NormalOverlay->SetTriangle(TriangleID, Elements);
			}

			if (UVOverlay)
			{
				FIndex3i Elements;
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 UVIndex = AttributeIndex(MeshData.TexCoords().Num(), SourceVertices[k], SourceCorners[k]);
					const FVector2f UV = MeshData.TexCoords().IsValidIndex(UVIndex) ? MeshData.TexCoords()[UVIndex] : FVector2f::ZeroVector;
					Elements[k] = UVOverlay->AppendElement(UV);
				}
				UVOverlay->SetTriangle(TriangleID, Elements);
			}

			if (ColorOverlay)
			{
				FIndex3i Elements;
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 ColorIndex = AttributeIndex(MeshData.Colors.Num(), SourceVertices[k], SourceCorners[k]);
					const FVector4f Color = MeshData.Colors.IsValidIndex(ColorIndex) ? MeshData.Colors[ColorIndex] : FVector4f(1, 1, 1, 1);
					Elements[k] = ColorOverlay->AppendElement(Color);
				}
				ColorOverlay->SetTriangle(TriangleID, Elements);
			}
		}

		// 刻意不走 CSMeshBuild::ComputeAreaWeightedVertexNormals：sink 不同，口径也不同。
		// FMeshNormals 是面积×角度双重加权、且用 UE 的反向叉积口径（VectorUtil::NormalArea），
		// 服务 FDynamicMesh3；builder 那份是纯面积加权、用常驻流的正向叉积口径。两者合并会让
		// 其中一个 sink 整体翻面，见 CSMeshBuild.h 里对两种口径的说明。
		// 第二个参数是 bUseMeshVertexNormalsIfAvailable，不是"是否面积加权"（很容易看反）：
		// 传 false 表示一律重算，而不是复用 Mesh.GetVertexNormal。
		// 它内部会 ClearElements()，把上面逐角点拆出来的 normal overlay 元素抹平成逐顶点，
		// 硬边随之丢失 —— 这正是"重算法线"这个开关的语义，不是 bug。
		if (Options.bRecomputeNormals || MeshData.Normals.IsEmpty()) FMeshNormals::InitializeOverlayToPerVertexNormals(Attributes->PrimaryNormals(), false);
	});

	return TargetMesh;
}

// -----------------------------------------------------------------------------
// Scene extraction
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshOps::AppendBoxSceneTriangles(
	UCSMesh* Target, UObject* WorldContextObject, const FCSMeshBoxSceneOptions& Options)
{
	if (!Target) return Target;

	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull) : nullptr;
	if (!World) return Target;

	const FBox QueryBox = Options.QueryBox;
	if (!QueryBox.IsValid)
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] AppendBoxSceneTriangles: no valid query box."));
		return Target;
	}

	const int32 MaxTriangles = FMath::Max(Options.MaxTriangles, 1);

	// Game thread: resolve render resources and extract landscape triangles. The collection is
	// stateless (CSBoxSceneCollection), so every input it used to read off a generator actor
	// comes out of Options here.
	FCSBoxSceneCollectOptions CollectOptions;
	CollectOptions.QueryBox = QueryBox;
	CollectOptions.MaxTriangles = MaxTriangles;
	CollectOptions.ReferencePoints = Options.ReferencePoints;
	CollectOptions.ReferenceFilterDistance = Options.ReferenceFilterDistance;
	// 蓝图侧仍是单标签属性；None 表示不过滤，故空标签不能进数组（数组非空即开启过滤）。
	if (!Options.RequiredActorTag.IsNone()) CollectOptions.RequiredActorTags = { Options.RequiredActorTag };
	CollectOptions.ExcludedActor = Options.ExcludedActor;
	CollectOptions.ExcludedActorTags = Options.ExcludedActorTags;
	CollectOptions.LODIndex = FMath::Max(0, Options.LODIndex);
	CollectOptions.bIncludeLandscape = Options.bIncludeLandscape;
	CollectOptions.bPreserveSourceMaterialSlots = Options.bPreserveSourceMaterialSlots;

	const FCSBoxScenePreparedData Prepared = CSBoxSceneCollection::CollectBoxSceneTriangles(World, CollectOptions);
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles())
	{
		UE_LOG(LogCSMeshOps, Log, TEXT("[CSMeshOps] AppendBoxSceneTriangles: no geometry inside the query box."));
		return Target;
	}

	// Append semantics: the destination must fit whatever it already holds plus the soup.
	// Only the GPU knows the current count exactly, so grow by the conservative capacity.
	const int32 ExistingVertices = FMath::Max(Target->GetVertexCapacity(), 0);
	const int32 ExistingIndices = FMath::Max(Target->GetIndexCapacity(), 0);
	const bool bEmpty = Target->IsEmpty();
	const int32 NeededVertices = (bEmpty ? 0 : ExistingVertices) + MaxTriangles * 3;
	const int32 NeededIndices = (bEmpty ? 0 : ExistingIndices) + MaxTriangles * 3;
	Target->EnsureCapacitySync(NeededVertices, NeededIndices);

	// Registry ids are relative to this extraction's material table, so they shift by however
	// many slots the target already has.
	const uint32 MaterialIdOffset = uint32(Target->Materials.Num());
	for (int32 Index = 0; Index < Prepared.GetMaterialRegistryNum(); ++Index)
		Target->Materials.Add(Prepared.GetMaterialByRegistryIndex(Index));

	Target->EditMeshSync([&Prepared, MaxTriangles, MaterialIdOffset, QueryBox](FCSMeshEditContext& Context)
	{
		if (!CSMeshOps_HasVertexStreams(Context)) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

		const FCSStaticMeshTriangleRDGOutput Soup = AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
			GraphBuilder, RHICmdList, Prepared, TEXT("CSMeshOps.BoxSceneSoup"));
		if (!Soup.TriangleVertices || !Soup.TriangleNormals || !Soup.TriangleCounter) return;

		FRDGBufferSRVRef VerticesSRV = Soup.TriangleVerticesSRV ? Soup.TriangleVerticesSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleVertices, PF_A32B32G32R32F));
		FRDGBufferSRVRef NormalsSRV = Soup.TriangleNormalsSRV ? Soup.TriangleNormalsSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleNormals, PF_A32B32G32R32F));
		FRDGBufferSRVRef CounterSRV = Soup.TriangleCounterSRV ? Soup.TriangleCounterSRV
			: GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleCounter, PF_R32_UINT));
		FRDGBufferSRVRef UVsSRV = Soup.TriangleUVsSRV ? Soup.TriangleUVsSRV
			: (Soup.TriangleUVs ? GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleUVs, PF_G32R32F)) : nullptr);
		FRDGBufferSRVRef MaterialIdsSRV = Soup.TriangleMaterialIdsSRV ? Soup.TriangleMaterialIdsSRV
			: (Soup.TriangleMaterialIds ? GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup.TriangleMaterialIds, PF_R32_UINT)) : nullptr);
		if (!UVsSRV || !MaterialIdsSRV) return;

		FRDGBufferSRVRef CountersSRV = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);

		{
			FCSMeshOpsAppendSoupCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsAppendSoupCS::FParameters>();
			Params->SrcSoupVertices = VerticesSRV;
			Params->SrcSoupNormals = NormalsSRV;
			Params->SrcSoupUVs = UVsSRV;
			Params->SrcSoupMaterialIds = MaterialIdsSRV;
			Params->SrcSoupCounter = CounterSRV;
			Params->InMeshCounters = CountersSRV;
			Params->RW_Positions = CSMeshOps_UAV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
			Params->RW_Tangents = CSMeshOps_UAV(GraphBuilder, Context.Tangents(), PF_R32_UINT);
			Params->RW_TexCoords = CSMeshOps_UAV(GraphBuilder, Context.TexCoords(), PF_R32_FLOAT);
			Params->RW_Colors = CSMeshOps_UAV(GraphBuilder, Context.Colors(), PF_R32_UINT);
			Params->RW_Indices = CSMeshOps_UAV(GraphBuilder, Context.Indices(), PF_R32_UINT);
			Params->RW_MaterialIds = CSMeshOps_UAV(GraphBuilder, Context.MaterialIds(), PF_R32_UINT);
			Params->SrcTriangleCount = uint32(MaxTriangles);
			Params->SrcSoupUVChannels = uint32(FMath::Max(Soup.NumUVChannels, 1));
			Params->MaterialIdOffset = MaterialIdOffset;
			Params->VertexCapacity = Context.Resident.VertexCapacity;
			Params->IndexCapacity = Context.Resident.IndexCapacity;

			TShaderMapRef<FCSMeshOpsAppendSoupCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.AppendSoup"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(MaxTriangles * 3, CSMeshOps_GroupSize));
		}
		{
			// AdvanceCountersBySoupCS writes arg set 0 only, so a mesh that had been sectioned
			// would come out of this with set 0 covering everything and the rest stale.
			InvalidateSections(Context);

			FCSMeshOpsAdvanceCountersCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsAdvanceCountersCS::FParameters>();
			Params->SrcSoupCounter = CounterSRV;
			Params->RW_MeshCounters = CSMeshOps_UAV(GraphBuilder, Context.Counters(), PF_R32_UINT);
			Params->RW_IndirectArgs = CSMeshOps_UAV(GraphBuilder, Context.IndirectArgs(), PF_R32_UINT);
			Params->SrcTriangleCount = uint32(MaxTriangles);
			Params->VertexCapacity = Context.Resident.VertexCapacity;
			Params->IndexCapacity = Context.Resident.IndexCapacity;

			TShaderMapRef<FCSMeshOpsAdvanceCountersCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.AdvanceCounters"), Shader, Params,
				FIntVector(1, 1, 1));
		}

		// The extraction decides its own output size, so the game thread must not claim to know it.
		Context.InvalidateKnownCounts();
		// The query box is the only bound available without asking the GPU, and it stands as the
		// fallback if the exact reduction below is off or fails.
		Context.SetWorldBounds(Context.GetWorldBounds() + QueryBox);
	});

	if (Options.bComputeExactBounds) ComputeWorldBoundsSync(Target);

	return Target;
}

FCSMeshBoxSceneOptions UCSMeshOps::MakeGeneratorBoxSceneOptions(
	AComputeShaderMeshGenerator* Generator, float ReferenceFilterDistance, int32 MaxTrianglesOverride)
{
	FCSMeshBoxSceneOptions Options;
	if (!Generator) return Options;

	Options.QueryBox = Generator->GetGeneratorBoundsWorldBox();
	Options.MaxTriangles = FMath::Max(1, MaxTrianglesOverride > 0 ? MaxTrianglesOverride : Generator->MaxTriangles);
	Options.ReferencePoints = Generator->ReferencePoints;
	// 没有参照点时距离过滤没有意义，按 0 传（提取端据此保留盒内全部三角形）。传一个非零距离
	// 而参照点为空，会让提取端把每个三角形都判成"离参照点太远"，结果是一个空网格。
	Options.ReferenceFilterDistance = Generator->ReferencePoints.IsEmpty() ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);
	// 生成器坐在自己的查询盒里，不排除自身就会把马上要被替换掉的几何再抽一遍。
	Options.ExcludedActor = Generator;
	Options.ExcludedActorTags = Generator->ExcludedActorTags;
	Options.LODIndex = Generator->VoxelGridSettings.LODIndex;
	return Options;
}

// -----------------------------------------------------------------------------
// Snapshot upload (the mirror of ReadbackMeshSync)
// -----------------------------------------------------------------------------

bool UCSMeshOps::BuildUploadPayload(const FCSGpuMeshCPUData& Snapshot, FCSMeshUploadPayload& OutPayload,
	int32 NumTexCoordSets)
{
	if (!Snapshot.IsValid()) return false;

	const int32 SourceIndexCount = Snapshot.Indices.Num();
	const int32 SourceTriangleCount = SourceIndexCount / 3;

	// Attributes may be per position or per index corner. The resident streams are strictly
	// per-vertex, so a per-corner snapshot becomes a soup: one vertex per corner, identity
	// index buffer. Nothing is lost (the StaticMesh build re-merges by attribute equality on
	// the way out) but the vertex count grows, which matters for capacity.
	auto IsPerCorner = [&Snapshot, SourceIndexCount](int32 AttributeCount)
	{
		return AttributeCount == SourceIndexCount && SourceIndexCount != Snapshot.Positions.Num();
	};
	const bool bExpandToSoup = Snapshot.AttrLayout == FCSGpuMeshCPUData::EAttrLayout::PerCorner
		|| IsPerCorner(Snapshot.Normals.Num())
		|| IsPerCorner(Snapshot.TexCoords().Num())
		|| IsPerCorner(Snapshot.Colors.Num());

	const int32 VertexCount = bExpandToSoup ? SourceIndexCount : Snapshot.Positions.Num();
	if (VertexCount < 3 || SourceTriangleCount < 1) return false;

	// --- pack the CPU side into the exact stream layouts
	TArray<FVector3f>& Positions = OutPayload.Positions;
	TArray<uint32>& Tangents = OutPayload.Tangents;
	TArray<FVector2f>& TexCoords = OutPayload.TexCoords;
	TArray<uint32>& Colors = OutPayload.Colors;
	TArray<uint32>& Indices = OutPayload.Indices;
	TArray<uint32>& MaterialIds = OutPayload.MaterialIds;
	Positions.SetNumUninitialized(VertexCount);
	Tangents.SetNumUninitialized(VertexCount * 2);
	// 多组 UV 交错：一条流承载 N 组，长度 = 顶点数 × N（N=1 时与单组逐位相同）。
	// 组数由**目标网格**的声明决定，不是快照说了算 —— 流的宽度是网格的属性。
	const int32 NumSets = FMath::Clamp(NumTexCoordSets, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	OutPayload.NumTexCoordSets = NumSets;
	TexCoords.SetNumUninitialized(VertexCount * NumSets);
	Colors.SetNumUninitialized(VertexCount);
	Indices.SetNumUninitialized(SourceIndexCount);
	MaterialIds.SetNumUninitialized(SourceTriangleCount);

	auto AttributeIndex = [SourceIndexCount](int32 AttributeCount, int32 PositionIndex, int32 CornerIndex)
	{
		return AttributeCount == SourceIndexCount ? CornerIndex : PositionIndex;
	};

	for (int32 Vertex = 0; Vertex < VertexCount; ++Vertex)
	{
		const int32 Corner = bExpandToSoup ? Vertex : INDEX_NONE;
		const int32 PositionIndex = bExpandToSoup ? int32(Snapshot.Indices[Vertex]) : Vertex;
		if (!Snapshot.Positions.IsValidIndex(PositionIndex)) return false;

		Positions[Vertex] = Snapshot.Positions[PositionIndex];

		const int32 AttrCorner = bExpandToSoup ? Corner : PositionIndex;
		const int32 NormalIndex = AttributeIndex(Snapshot.Normals.Num(), PositionIndex, AttrCorner);
		const int32 TangentIndex = AttributeIndex(Snapshot.Tangents.Num(), PositionIndex, AttrCorner);
		const int32 UVIndex = AttributeIndex(Snapshot.TexCoords().Num(), PositionIndex, AttrCorner);

		FVector3f Normal = Snapshot.Normals.IsValidIndex(NormalIndex) ? Snapshot.Normals[NormalIndex] : FVector3f::UnitZ();
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
		FVector3f Tangent = Snapshot.Tangents.IsValidIndex(TangentIndex) ? Snapshot.Tangents[TangentIndex] : FVector3f::UnitX();
		Tangent = (Tangent - Normal * FVector3f::DotProduct(Tangent, Normal)).GetSafeNormal();
		if (Tangent.IsNearlyZero())
		{
			const FVector3f Axis = FMath::Abs(Normal.Z) < 0.9f ? FVector3f::UnitZ() : FVector3f::UnitX();
			Tangent = FVector3f::CrossProduct(Axis, Normal).GetSafeNormal();
		}
		float BinormalSign = 1.0f;
		{
			const int32 SignIndex = AttributeIndex(Snapshot.BinormalSigns.Num(), PositionIndex, AttrCorner);
			if (Snapshot.BinormalSigns.IsValidIndex(SignIndex)) BinormalSign = Snapshot.BinormalSigns[SignIndex] < 0.0f ? -1.0f : 1.0f;
		}
		Tangents[Vertex * 2 + 0] = CSMeshOps_PackSnorm8888(FVector4f(Tangent, 0.0f));
		Tangents[Vertex * 2 + 1] = CSMeshOps_PackSnorm8888(FVector4f(Normal, BinormalSign));

		for (int32 Set = 0; Set < NumSets; ++Set)
		{
			const TArray<FVector2f>& Channel = Snapshot.TexCoordChannels[Set];
			const int32 ChannelIndex = AttributeIndex(Channel.Num(), PositionIndex, AttrCorner);
			TexCoords[Vertex * NumSets + Set] = Channel.IsValidIndex(ChannelIndex) ? Channel[ChannelIndex] : FVector2f::ZeroVector;
		}

		const int32 ColorIndex = AttributeIndex(Snapshot.Colors.Num(), PositionIndex, AttrCorner);
		Colors[Vertex] = Snapshot.Colors.IsValidIndex(ColorIndex)
			? CSMeshOps_PackColorBGRA(Snapshot.Colors[ColorIndex])
			: 0xffffffffu;
	}

	for (int32 Corner = 0; Corner < SourceIndexCount; ++Corner)
		Indices[Corner] = bExpandToSoup ? uint32(Corner) : Snapshot.Indices[Corner];

	for (int32 Triangle = 0; Triangle < SourceTriangleCount; ++Triangle)
	{
		MaterialIds[Triangle] = Snapshot.TriangleMaterialSlots.IsValidIndex(Triangle)
			? uint32(FMath::Max(Snapshot.TriangleMaterialSlots[Triangle], 0))
			: 0u;
	}

	OutPayload.WorldBounds = FBox(ForceInit);
	for (const FVector3f& Position : Positions) OutPayload.WorldBounds += FVector(Position);
	OutPayload.VertexCount = VertexCount;
	OutPayload.IndexCount = SourceIndexCount;
	return true;
}

int32 UCSMeshOps::GetTexCoordSets(const UCSMesh* Target)
{
	if (!Target) return 1;
	const FCSMeshResident* Resident = Target->GetResidentPtr();
	if (!Resident) return 1;
	const FCSMeshResident::FStream* Stream = Resident->FindStream(ECSGpuStreamRole::TexCoord, 0);
	if (!Stream) return 1;
	return FMath::Max(int32(Stream->Desc.ElementsPerUnit) / 2, 1);
}

bool UCSMeshOps::EnsureTexCoordSets(UCSMesh* Target, int32 NumTexCoordSets)
{
	if (!Target) return false;
	const int32 Want = FMath::Clamp(NumTexCoordSets, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	if (GetTexCoordSets(Target) == Want) return true;   // 已是这个组数：一点布局工作都不做

	FCSMeshStreamLayout Layout;
	Layout.NumTexCoordSets = uint32(Want);

	if (const FCSMeshResident* Resident = Target->GetResidentPtr())
	{
		// NumIndirectDraws 必须沿用当前值：这条路只想改 UV 组数，顺手把分段用的 arg set 数
		// 打回 1 会让已经建好的材质分段静默失效（症状是网格只画一个材质，且不报任何错）。
		Layout.NumIndirectDraws = FMath::Max(Resident->NumIndirectDraws, 1u);

		// 已声明的非标准流必须原样带过去。重新声明布局是**整套重建**，漏掉谁谁就没了，
		// 而调用方根本不知道那些流是哪个消费者加的（症状是那边报"额外流不在常驻集里"）。
		TArray<FCSGpuStreamDesc> Standard;
		CSGpuMeshStreams::FStandardStreamOptions Options;
		Options.NumIndirectDraws = Layout.NumIndirectDraws;
		Options.bMaterialIds = true;
		Options.bReadbackColors = true;
		Options.NumTexCoordSets = uint32(Want);
		CSGpuMeshStreams::BuildStandardTriangleStreamDescs(Standard, Options);

		for (const FCSMeshResident::FStream& Stream : Resident->Streams)
		{
			const bool bStandard = Standard.ContainsByPredicate([&Stream](const FCSGpuStreamDesc& D)
			{
				// TexCoordIndex 兼任通用槽位号（FindStream / FCSMeshEditContext::Find 都以它为键）。
				return D.Role == Stream.Desc.Role && D.TexCoordIndex == Stream.Desc.TexCoordIndex;
			});
			if (!bStandard) Layout.ExtraStreams.Add(Stream.Desc);
		}
	}
	return Target->SetStreamLayoutSync(Layout);
}

void UCSMeshOps::AddCopyFromSnapshotPasses(FCSMeshEditContext& Context, const FCSMeshUploadPayload& Payload)
{
	if (!CSMeshOps_HasVertexStreams(Context) || !Payload.IsValid()) return;
	FRDGBuilder& GraphBuilder = Context.GraphBuilder;

	auto Upload = [&GraphBuilder](FRDGBufferRef Buffer, const void* Data, uint64 Bytes)
	{
		if (!Buffer || Bytes == 0) return;
		void* Copy = GraphBuilder.Alloc(Bytes, 16);
		FMemory::Memcpy(Copy, Data, Bytes);
		GraphBuilder.QueueBufferUpload(Buffer, Copy, Bytes, ERDGInitialDataFlags::None);
	};

	Upload(Context.Positions(), Payload.Positions.GetData(), Payload.Positions.Num() * sizeof(FVector3f));
	Upload(Context.Tangents(), Payload.Tangents.GetData(), Payload.Tangents.Num() * sizeof(uint32));
	Upload(Context.TexCoords(), Payload.TexCoords.GetData(), Payload.TexCoords.Num() * sizeof(FVector2f));
	Upload(Context.Colors(), Payload.Colors.GetData(), Payload.Colors.Num() * sizeof(uint32));
	Upload(Context.Indices(), Payload.Indices.GetData(), Payload.Indices.Num() * sizeof(uint32));
	Upload(Context.MaterialIds(), Payload.MaterialIds.GetData(), Payload.MaterialIds.Num() * sizeof(uint32));

	AddSetCountersPass(Context, uint32(Payload.VertexCount), uint32(Payload.IndexCount));
	Context.SetWorldBounds(Payload.WorldBounds);
}

bool UCSMeshOps::CopyFromMeshSnapshot(UCSMesh* Target, const FCSGpuMeshCPUData& Snapshot)
{
	if (!Target) return false;

	FCSMeshUploadPayload Payload;
	// 按**目标网格已声明的**组数打包 —— 上传路径不碰布局。
	if (!BuildUploadPayload(Snapshot, Payload, GetTexCoordSets(Target))) return false;

	Target->EnsureCapacitySync(Payload.VertexCount, Payload.IndexCount);
	return Target->EditMeshSync([&Payload](FCSMeshEditContext& Context)
	{
		AddCopyFromSnapshotPasses(Context, Payload);
	});
}

// -----------------------------------------------------------------------------
// Boolean / arrangement
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshOps::ApplyMeshBoolean(
	UCSMesh* Target, AComputeShaderMeshBoolean* Generator, ECSMeshBooleanOp Op, const FCSMeshBooleanOptions& Options)
{
	if (!Target || !Generator) return Target;

	// The GPU path writes the result straight into the resident streams — no snapshot, no
	// re-upload — and with VertexWeldDistance > 0 it also welds and repairs on the GPU.
	if (Generator->RunBooleanToGpuMesh(Op, Options, Target)) return Target;
	UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] ApplyMeshBoolean: the GPU path produced no geometry."));
	return Target;
}

UCSMesh* UCSMeshOps::ApplyMeshArrangement(
	UCSMesh* Target, AComputeShaderMeshBoolean* Generator, const FCSMeshBooleanOptions& Options)
{
	return ApplyMeshBoolean(Target, Generator, ECSMeshBooleanOp::ArrangementOnly, Options);
}

UCSMesh* UCSMeshOps::WeldVertices(UCSMesh* Target, float WeldDistance)
{
	if (!Target || WeldDistance <= UE_SMALL_NUMBER) return Target;

	const int32 IndexCapacity = FMath::Max(Target->GetIndexCapacity(), 3);
	const int32 TriangleCapacity = FMath::Max(IndexCapacity / 3, 1);
	const FBox Bounds = Target->GetWorldBoundsApprox();
	const FVector3f GridOrigin = Bounds.IsValid ? FVector3f(Bounds.Min) : FVector3f::ZeroVector;

	// No InvalidateSections here on purpose: the weld rewrites index *values*, leaving every
	// triangle at the same position in the buffer with the same material id and the same counts.
	// The runs a section table describes therefore still describe the same triangles.
	Target->EditMeshSync([IndexCapacity, TriangleCapacity, GridOrigin, WeldDistance](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Positions() || !Context.Indices() || !Context.Counters()) return;

		FRDGBufferSRVRef CountersSRV = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);

		// The weld facility rewrites nothing itself; it needs a corner-indexed soup view and a
		// triangle counter, and returns a representative corner per corner.
		FRDGBufferRef WeldSoup = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector3f), IndexCapacity), TEXT("CSMeshOps.WeldSoup"));
		FRDGBufferRef WeldCounter = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSMeshOps.WeldCounter"));
		// Remapping in place would race: a corner's representative may itself be rewritten by
		// another thread in the same pass, so the read side works off a pre-weld copy.
		FRDGBufferRef IndexCopy = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), IndexCapacity), TEXT("CSMeshOps.WeldIndexCopy"));
		AddCopyBufferPass(GraphBuilder, IndexCopy, 0, Context.Indices(), 0, uint64(IndexCapacity) * sizeof(uint32));

		{
			FCSMeshOpsWeldCounterCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsWeldCounterCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->RW_WeldTriangleCounter = CSMeshOps_UAV(GraphBuilder, WeldCounter, PF_R32_UINT);
			Params->IndexCapacity = uint32(IndexCapacity);

			TShaderMapRef<FCSMeshOpsWeldCounterCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.WeldCounter"), Shader, Params, FIntVector(1, 1, 1));
		}
		{
			FCSMeshOpsWeldSoupCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsWeldSoupCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->WeldSrcPositions = CSMeshOps_SRV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
			Params->WeldSrcIndices = CSMeshOps_SRV(GraphBuilder, IndexCopy, PF_R32_UINT);
			Params->RW_WeldSoup = GraphBuilder.CreateUAV(WeldSoup);
			Params->ThreadCount = uint32(IndexCapacity);

			TShaderMapRef<FCSMeshOpsWeldSoupCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.WeldSoup"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(IndexCapacity, CSMeshOps_GroupSize));
		}

		FRDGBufferRef Representatives = CSGpuTriangleUtilities::AddVertexWeldPasses(
			GraphBuilder, WeldSoup, WeldCounter, TriangleCapacity, TriangleCapacity, GridOrigin, WeldDistance);

		{
			FCSMeshOpsWeldRemapCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsWeldRemapCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->WeldSrcIndices = CSMeshOps_SRV(GraphBuilder, IndexCopy, PF_R32_UINT);
			Params->WeldRepresentatives = CSMeshOps_SRV(GraphBuilder, Representatives, PF_R32_UINT);
			Params->RW_Indices = CSMeshOps_UAV(GraphBuilder, Context.Indices(), PF_R32_UINT);
			Params->ThreadCount = uint32(IndexCapacity);

			TShaderMapRef<FCSMeshOpsWeldRemapCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.WeldRemap"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(IndexCapacity, CSMeshOps_GroupSize));
		}
	});
	return Target;
}

// -----------------------------------------------------------------------------
// In-place operators
// -----------------------------------------------------------------------------

void UCSMeshOps::AddTransformPasses(FCSMeshEditContext& Context, const FTransform& Transform)
{
	FRDGBuilder& GraphBuilder = Context.GraphBuilder;
	if (!Context.Positions() || !Context.Tangents() || !Context.Counters()) return;

	const uint32 VertexCapacity = Context.Resident.VertexCapacity;
	if (VertexCapacity == 0) return;

	FCSMeshOpsTransformCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsTransformCS::FParameters>();
	Params->InMeshCounters = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
	Params->RW_Positions = CSMeshOps_UAV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
	Params->RW_Tangents = CSMeshOps_UAV(GraphBuilder, Context.Tangents(), PF_R32_UINT);
	Params->SrcLocalToWorld = FMatrix44f(Transform.ToMatrixWithScale());
	Params->SrcNormalToWorld = FMatrix44f(Transform.ToMatrixWithScale().Inverse().GetTransposed());
	Params->ThreadCount = VertexCapacity;

	TShaderMapRef<FCSMeshOpsTransformCS> Shader(CSMeshOps_ShaderMap());
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.Transform"), Shader, Params,
		FComputeShaderUtils::GetGroupCountWrapped(int32(VertexCapacity), CSMeshOps_GroupSize));

	const FBox Bounds = Context.GetWorldBounds();
	if (Bounds.IsValid) Context.SetWorldBounds(Bounds.TransformBy(Transform));
}

UCSMesh* UCSMeshOps::TransformMesh(UCSMesh* Target, const FTransform& Transform)
{
	if (!Target) return Target;
	Target->EditMeshSync([&Transform](FCSMeshEditContext& Context) { AddTransformPasses(Context, Transform); });
	return Target;
}

UCSMesh* UCSMeshOps::TranslateMesh(UCSMesh* Target, FVector Translation)
{
	return TransformMesh(Target, FTransform(Translation));
}

UCSMesh* UCSMeshOps::FlipNormals(UCSMesh* Target)
{
	if (!Target) return Target;

	const uint32 VertexCapacity = uint32(FMath::Max(Target->GetVertexCapacity(), 0));
	const uint32 TriangleCapacity = uint32(FMath::Max(Target->GetIndexCapacity(), 0)) / 3u;

	Target->EditMeshSync([VertexCapacity, TriangleCapacity](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Indices() || !Context.Tangents() || !Context.Counters()) return;
		FRDGBufferSRVRef CountersSRV = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);

		{
			FCSMeshOpsFlipWindingCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsFlipWindingCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->RW_Indices = CSMeshOps_UAV(GraphBuilder, Context.Indices(), PF_R32_UINT);
			Params->ThreadCount = TriangleCapacity;

			TShaderMapRef<FCSMeshOpsFlipWindingCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.FlipWinding"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(int32(TriangleCapacity), CSMeshOps_GroupSize));
		}
		{
			FCSMeshOpsNegateNormalsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsNegateNormalsCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->RW_Tangents = CSMeshOps_UAV(GraphBuilder, Context.Tangents(), PF_R32_UINT);
			Params->ThreadCount = VertexCapacity;

			TShaderMapRef<FCSMeshOpsNegateNormalsCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.NegateNormals"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(int32(VertexCapacity), CSMeshOps_GroupSize));
		}
	});
	return Target;
}

UCSMesh* UCSMeshOps::SetVertexColors(UCSMesh* Target, FLinearColor Color)
{
	if (!Target) return Target;

	const uint32 VertexCapacity = uint32(FMath::Max(Target->GetVertexCapacity(), 0));
	const FVector4f PackedColor(Color.R, Color.G, Color.B, Color.A);

	Target->EditMeshSync([VertexCapacity, PackedColor](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Colors() || !Context.Counters()) return;

		FCSMeshOpsSetColorsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsSetColorsCS::FParameters>();
		Params->InMeshCounters = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
		Params->RW_Colors = CSMeshOps_UAV(GraphBuilder, Context.Colors(), PF_R32_UINT);
		Params->ConstantColor = PackedColor;
		Params->ThreadCount = VertexCapacity;

		TShaderMapRef<FCSMeshOpsSetColorsCS> Shader(CSMeshOps_ShaderMap());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.SetVertexColors"), Shader, Params,
			FComputeShaderUtils::GetGroupCountWrapped(int32(VertexCapacity), CSMeshOps_GroupSize));
	});
	return Target;
}

UCSMesh* UCSMeshOps::PaintVertexColorsSphere(
	UCSMesh* Target, FVector Center, float Radius, float Falloff, float Strength,
	FLinearColor Color, FLinearColor ChannelMask, ECSMeshPaintBlendOp BlendOp)
{
	// 全网格派发 = 区域版退化情形（VertsX/Y = 0），公式与 kernel 完全共用。
	return PaintVertexColorsSphereInRegion(Target, Center, Radius, Falloff, Strength, Color, ChannelMask, BlendOp,
		0, 0, FIntPoint::ZeroValue, FIntPoint::ZeroValue);
}

UCSMesh* UCSMeshOps::PaintVertexColorsSphereInRegion(
	UCSMesh* Target, FVector Center, float Radius, float Falloff, float Strength,
	FLinearColor Color, FLinearColor ChannelMask, ECSMeshPaintBlendOp BlendOp,
	int32 VertsX, int32 VertsY, FIntPoint RegionMin, FIntPoint RegionMax)
{
	if (!Target || Radius <= 0.0f || Strength <= 0.0f) return Target;

	// 区域派发：把 (x, y) 闭区间 clamp 进网格，线程数 = 矩形面积；否则退回全顶点容量。
	const bool bRegional = VertsX >= 2 && VertsY >= 2;
	FIntPoint Min = FIntPoint::ZeroValue;
	FIntPoint Max = FIntPoint::ZeroValue;
	uint32 ThreadCount = uint32(FMath::Max(Target->GetVertexCapacity(), 0));
	if (bRegional)
	{
		Min = FIntPoint(FMath::Clamp(RegionMin.X, 0, VertsX - 1), FMath::Clamp(RegionMin.Y, 0, VertsY - 1));
		Max = FIntPoint(FMath::Clamp(RegionMax.X, Min.X, VertsX - 1), FMath::Clamp(RegionMax.Y, Min.Y, VertsY - 1));
		ThreadCount = uint32(Max.X - Min.X + 1) * uint32(Max.Y - Min.Y + 1);
	}
	if (ThreadCount == 0) return Target;

	FCSMeshPaintSphereDab Dab;
	Dab.Center = Center;
	Dab.Radius = Radius;
	Dab.Falloff = Falloff;
	Dab.Strength = Strength;
	Dab.Color = Color;
	Dab.ChannelMask = ChannelMask;
	Dab.BlendOp = BlendOp;
	Dab.VertsX = bRegional ? VertsX : 0;
	Dab.VertsY = bRegional ? VertsY : 0;
	Dab.RegionMin = Min;
	Dab.RegionMax = Max;

	Target->EditMeshSync([&Dab](FCSMeshEditContext& Context) { AddPaintSpherePasses(Context, Dab); });
	return Target;
}

void UCSMeshOps::AddPaintSpherePasses(FCSMeshEditContext& Context, const FCSMeshPaintSphereDab& Dab)
{
	FRDGBuilder& GraphBuilder = Context.GraphBuilder;
	if (!Context.Positions() || !Context.Colors() || !Context.Counters()) return;
	if (Dab.Radius <= 0.0f || Dab.Strength <= 0.0f) return;

	const bool bRegional = Dab.VertsX >= 2 && Dab.VertsY >= 2;
	uint32 ThreadCount = Context.Resident.VertexCapacity;
	if (bRegional) ThreadCount = uint32(Dab.RegionMax.X - Dab.RegionMin.X + 1) * uint32(Dab.RegionMax.Y - Dab.RegionMin.Y + 1);
	if (ThreadCount == 0) return;

	FCSMeshOpsPaintColorsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsPaintColorsCS::FParameters>();
	Params->InMeshCounters = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
	Params->BrushSrcPositions = CSMeshOps_SRV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
	Params->RW_Colors = CSMeshOps_UAV(GraphBuilder, Context.Colors(), PF_R32_UINT);
	Params->ConstantColor = FVector4f(Dab.Color.R, Dab.Color.G, Dab.Color.B, Dab.Color.A);
	Params->BrushChannelMask = FVector4f(Dab.ChannelMask.R, Dab.ChannelMask.G, Dab.ChannelMask.B, Dab.ChannelMask.A);
	Params->BrushCenter = FVector3f(Dab.Center);   // resident positions are world-space floats
	Params->BrushRadius = Dab.Radius;
	Params->BrushFalloff = FMath::Clamp(Dab.Falloff, 0.0f, 1.0f);
	Params->BrushStrength = FMath::Clamp(Dab.Strength, 0.0f, 1.0f);
	Params->PaintBlendOp = uint32(Dab.BlendOp);
	Params->BrushRegion = FUintVector4(uint32(Dab.RegionMin.X), uint32(Dab.RegionMin.Y), uint32(Dab.RegionMax.X), uint32(Dab.RegionMax.Y));
	Params->BrushGridVertsX = bRegional ? uint32(Dab.VertsX) : 0u;
	Params->BrushGridVertsY = bRegional ? uint32(Dab.VertsY) : 0u;
	Params->ThreadCount = ThreadCount;

	TShaderMapRef<FCSMeshOpsPaintColorsCS> Shader(CSMeshOps_ShaderMap());
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.PaintVertexColorsSphere"), Shader, Params,
		FComputeShaderUtils::GetGroupCountWrapped(int32(ThreadCount), CSMeshOps_GroupSize));
}

UCSMesh* UCSMeshOps::DisplaceGroundShapers(
	UCSMesh* Target, const TArray<FVector4f>& ShaperParams,
	const FVector2f& GridOriginXY, float CellSize, float BaseZ,
	int32 VertsX, int32 VertsY, FIntPoint RegionMin, FIntPoint RegionMax)
{
	if (!Target || VertsX < 2 || VertsY < 2 || CellSize <= 0.0f) return Target;

	// 空 palette 也要跑：塑形物被删掉时正是"全 0 高度场"把隆起抹平的那一趟。
	const int32 ShaperCount = CSGroundShaperField::NumShapers(ShaperParams);   // 前缀里读：池子跟在头后面，不能再按长度除
	TArray<FVector4f> UploadParams = ShaperParams;
	if (UploadParams.IsEmpty()) UploadParams.Add(FVector4f::Zero());   // 结构化 buffer 不能是 0 长度

	const FIntPoint Min(FMath::Clamp(RegionMin.X, 0, VertsX - 1), FMath::Clamp(RegionMin.Y, 0, VertsY - 1));
	const FIntPoint Max(FMath::Clamp(RegionMax.X, Min.X, VertsX - 1), FMath::Clamp(RegionMax.Y, Min.Y, VertsY - 1));
	const uint32 RegionW = uint32(Max.X - Min.X + 1);
	const uint32 RegionH = uint32(Max.Y - Min.Y + 1);
	const uint32 ThreadCount = RegionW * RegionH;
	if (ThreadCount == 0) return Target;

	Target->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Positions() || !Context.Tangents() || !Context.Counters()) return;

		CSHelper::FRDGStructuredBufferRefs ShaperRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
			GraphBuilder, UploadParams, TEXT("CSMeshOps.GroundShaperParams"), false, true);
		if (!ShaperRefs.SRV) return;

		FCSMeshOpsDisplaceGroundCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsDisplaceGroundCS::FParameters>();
		Params->InMeshCounters = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
		Params->GroundShaperParams = ShaperRefs.SRV;
		Params->RW_Positions = CSMeshOps_UAV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
		Params->RW_Tangents = CSMeshOps_UAV(GraphBuilder, Context.Tangents(), PF_R32_UINT);
		Params->GroundOriginXY = GridOriginXY;
		Params->GroundCellSize = CellSize;
		Params->GroundBaseZ = BaseZ;
		Params->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
		Params->GroundVertsX = uint32(VertsX);
		Params->GroundVertsY = uint32(VertsY);
		Params->GroundRegion = FUintVector4(uint32(Min.X), uint32(Min.Y), uint32(Max.X), uint32(Max.Y));
		Params->ThreadCount = ThreadCount;

		TShaderMapRef<FCSMeshOpsDisplaceGroundCS> Shader(CSMeshOps_ShaderMap());
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.DisplaceGroundShapers"), Shader, Params,
			FComputeShaderUtils::GetGroupCountWrapped(int32(ThreadCount), CSMeshOps_GroupSize));

		// 位移只动 Z：包围盒按调用方已知的台高上界加宽即可，不值得为它跑一次 ComputeWorldBoundsSync。
		FBox Bounds = Context.GetWorldBounds();
		if (Bounds.IsValid)
		{
			float MaxTop = 0.0f;
			float MinBottom = 0.0f;   // 坑（负台高）往下撑
			for (int32 Index = 0; Index < ShaperCount; ++Index)
			{
				// 峰值而不是打包头里的台高（那份已除过二次抬升），按它收边会把最顶那圈顶点裁掉。
				const float Peak = CSGroundShaperField::PeakHeight(CSGroundShaperField::GetHeader(ShaperParams, Index).Top);
				MaxTop = FMath::Max(MaxTop, Peak);
				MinBottom = FMath::Min(MinBottom, Peak);
			}
			Bounds.Min.Z = FMath::Min(Bounds.Min.Z, double(BaseZ + MinBottom) - 1.0);
			Bounds.Max.Z = FMath::Max(Bounds.Max.Z, double(BaseZ + MaxTop) + 1.0);
			Context.SetWorldBounds(Bounds);
		}
	});
	return Target;
}

// -----------------------------------------------------------------------------
// Draw batches
// -----------------------------------------------------------------------------

bool UCSMeshOps::AddMaterialSectionPasses(FCSMeshEditContext& Context, int32 NumSlots)
{
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Indices() || !Context.MaterialIds() || !Context.Counters() || !Context.IndirectArgs()) return false;

		const int32 IndexCapacity = int32(FMath::Max(Context.Resident.IndexCapacity, 3u));
		const int32 TriangleCapacity = FMath::Max(IndexCapacity / 3, 1);

		// The old table describes the layout this sort is about to replace. It is dropped here
		// rather than left for SetSections so that a mesh is never observable holding a table and
		// arg sets that disagree, whatever happens between the edit and the publish.
		InvalidateSections(Context);

		// Scattering into the index buffer while reading it would race — one triangle's
		// destination is another's source — so the sort reads pre-sort copies, the same way the
		// weld does with WeldSrcIndices.
		FRDGBufferRef SrcIndices = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), IndexCapacity), TEXT("CSMeshOps.SortSrcIndices"));
		FRDGBufferRef SrcMaterialIds = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), TriangleCapacity), TEXT("CSMeshOps.SortSrcMaterialIds"));
		AddCopyBufferPass(GraphBuilder, SrcIndices, 0, Context.Indices(), 0, uint64(IndexCapacity) * sizeof(uint32));
		AddCopyBufferPass(GraphBuilder, SrcMaterialIds, 0, Context.MaterialIds(), 0, uint64(TriangleCapacity) * sizeof(uint32));

		FRDGBufferRef SlotCounters = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSlots), TEXT("CSMeshOps.SortSlotCounters"));
		FRDGBufferRef SlotCursors = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSlots), TEXT("CSMeshOps.SortSlotCursors"));

		AddClearUAVPass(GraphBuilder, CSMeshOps_UAV(GraphBuilder, SlotCounters, PF_R32_UINT), 0u);
		// Every arg set, not just the ones this sort fills: a mesh sectioned with more slots last
		// time would otherwise keep the tail sets, and they draw from index ranges that no longer
		// hold what they held.
		AddClearUAVPass(GraphBuilder, CSMeshOps_UAV(GraphBuilder, Context.IndirectArgs(), PF_R32_UINT), 0u);

		FRDGBufferSRVRef CountersSRV = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
		FRDGBufferSRVRef SrcIndicesSRV = CSMeshOps_SRV(GraphBuilder, SrcIndices, PF_R32_UINT);
		FRDGBufferSRVRef SrcMaterialIdsSRV = CSMeshOps_SRV(GraphBuilder, SrcMaterialIds, PF_R32_UINT);

		{
			FCSMeshOpsMaterialHistogramCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsMaterialHistogramCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->SortSrcMaterialIds = SrcMaterialIdsSRV;
			Params->RW_SlotCounters = CSMeshOps_UAV(GraphBuilder, SlotCounters, PF_R32_UINT);
			Params->NumMaterialSlots = uint32(NumSlots);
			Params->IndexCapacity = Context.Resident.IndexCapacity;
			Params->ThreadCount = uint32(TriangleCapacity);

			TShaderMapRef<FCSMeshOpsMaterialHistogramCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.MaterialHistogram"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(TriangleCapacity, CSMeshOps_GroupSize));
		}
		{
			FCSMeshOpsMaterialScanCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsMaterialScanCS::FParameters>();
			Params->InSlotCounters = CSMeshOps_SRV(GraphBuilder, SlotCounters, PF_R32_UINT);
			Params->RW_SlotCursors = CSMeshOps_UAV(GraphBuilder, SlotCursors, PF_R32_UINT);
			Params->RW_IndirectArgs = CSMeshOps_UAV(GraphBuilder, Context.IndirectArgs(), PF_R32_UINT);
			Params->NumMaterialSlots = uint32(NumSlots);

			TShaderMapRef<FCSMeshOpsMaterialScanCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.MaterialScan"), Shader, Params,
				FIntVector(1, 1, 1));
		}
		{
			FCSMeshOpsMaterialScatterCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsMaterialScatterCS::FParameters>();
			Params->InMeshCounters = CountersSRV;
			Params->SortSrcIndices = SrcIndicesSRV;
			Params->SortSrcMaterialIds = SrcMaterialIdsSRV;
			Params->RW_SlotCursors = CSMeshOps_UAV(GraphBuilder, SlotCursors, PF_R32_UINT);
			Params->RW_Indices = CSMeshOps_UAV(GraphBuilder, Context.Indices(), PF_R32_UINT);
			Params->RW_MaterialIds = CSMeshOps_UAV(GraphBuilder, Context.MaterialIds(), PF_R32_UINT);
			Params->NumMaterialSlots = uint32(NumSlots);
			Params->IndexCapacity = Context.Resident.IndexCapacity;
			Params->ThreadCount = uint32(TriangleCapacity);

			TShaderMapRef<FCSMeshOpsMaterialScatterCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.MaterialScatter"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(TriangleCapacity, CSMeshOps_GroupSize));
		}

		// The sort moves triangles around but adds and removes none, so the counters still hold.
	}
	return true;
}

void UCSMeshOps::PublishMaterialSections(UCSMesh* Target)
{
	if (!Target) return;

	// A mesh with no material table publishes an empty table rather than a one-entry one: both
	// draw identically (empty means one whole-mesh batch from arg set 0, which is exactly what the
	// single slot's arg set now holds) and the empty one claims no material index that the table
	// cannot resolve.
	TArray<FCSMeshSection> Sections;
	Sections.Reserve(Target->Materials.Num());
	for (int32 Slot = 0; Slot < Target->Materials.Num(); ++Slot)
	{
		// FirstTriangle / TriangleCount stay INDEX_NONE: the run extents live in the arg sets the
		// scan just wrote, and reporting them here would cost a readback for information the draw
		// never consults.
		FCSMeshSection& Section = Sections.AddDefaulted_GetRef();
		Section.MaterialIndex = Slot;
	}
	Target->SetSections(Sections);
}

UCSMesh* UCSMeshOps::BuildMaterialSections(UCSMesh* Target)
{
	if (!Target) return Target;

	// One slot minimum: a mesh with no material table still ends up with arg set 0 covering the
	// whole (single-slot) mesh, which is what an unsectioned mesh draws with anyway.
	const int32 NumSlots = FMath::Max(Target->Materials.Num(), 1);

	// Growing the args buffer changes its identity, which bumps AllocationGeneration and drops
	// the section table — so it has to happen before the args are written, and the table can only
	// be published after. Get the order wrong and the sections vanish with no symptom but a mesh
	// that draws one material.
	if (!Target->EnsureIndirectDrawCapacitySync(NumSlots))
	{
		UE_LOG(LogCSMeshOps, Warning,
			TEXT("[CSMeshOps] BuildMaterialSections: the mesh cannot carry %d indirect arg sets; leaving it unsectioned."), NumSlots);
		return Target;
	}

	// Set inside the edit, read after EditMeshSync's flush. Publishing sections for a sort that
	// never ran would point them at arg sets nothing wrote. (An ASYNC caller cannot read it here —
	// see PublishMaterialSections.)
	bool bSorted = false;
	Target->EditMeshSync([NumSlots, &bSorted](FCSMeshEditContext& Context)
	{
		bSorted = AddMaterialSectionPasses(Context, NumSlots);
	});

	if (bSorted) PublishMaterialSections(Target);
	return Target;
}

// -----------------------------------------------------------------------------
// Bounds
// -----------------------------------------------------------------------------

UCSMesh* UCSMeshOps::ComputeWorldBoundsSync(UCSMesh* Target)
{
	if (!Target) return Target;

	// Six uints: flipped min xyz then flipped max xyz. See the reduction kernel for the encoding.
	constexpr uint32 BoundsUints = 6;
	constexpr uint32 BoundsBytes = BoundsUints * sizeof(uint32);

	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSMeshOps.BoundsReadback"));
	bool bEnqueued = false;

	// Through EditMeshSync even though nothing resident is written: it is the only path that
	// registers the streams into the graph and puts their access states back afterwards, and a
	// read-only pass that leaves the position stream in RDG's epilogue state breaks the draw just
	// as thoroughly as a writing one would.
	Target->EditMeshSync([Readback, BoundsUints, BoundsBytes, &bEnqueued](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		if (!Context.Positions() || !Context.Counters()) return;

		const int32 VertexCapacity = int32(FMath::Max(Context.Resident.VertexCapacity, 3u));

		FRDGBufferRef Bounds = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), BoundsUints), TEXT("CSMeshOps.MeshBounds"));
		FRDGBufferUAVRef BoundsUAV = CSMeshOps_UAV(GraphBuilder, Bounds, PF_R32_UINT);

		{
			FCSMeshOpsClearBoundsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsClearBoundsCS::FParameters>();
			Params->RW_MeshBounds = BoundsUAV;

			TShaderMapRef<FCSMeshOpsClearBoundsCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.ClearBounds"), Shader, Params,
				FIntVector(1, 1, 1));
		}
		{
			FCSMeshOpsReduceBoundsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSMeshOpsReduceBoundsCS::FParameters>();
			Params->InMeshCounters = CSMeshOps_SRV(GraphBuilder, Context.Counters(), PF_R32_UINT);
			Params->BoundsSrcPositions = CSMeshOps_SRV(GraphBuilder, Context.Positions(), PF_R32_FLOAT);
			Params->RW_MeshBounds = BoundsUAV;
			Params->VertexCapacity = Context.Resident.VertexCapacity;
			Params->ThreadCount = uint32(VertexCapacity);

			TShaderMapRef<FCSMeshOpsReduceBoundsCS> Shader(CSMeshOps_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSMeshOps.ReduceBounds"), Shader, Params,
				FComputeShaderUtils::GetGroupCountWrapped(VertexCapacity, CSMeshOps_GroupSize));
		}

		// The buffer is graph-local, so it needs no final access state — it does not outlive the
		// graph. The copy below is the only thing that does.
		AddEnqueueCopyPass(GraphBuilder, Readback, Bounds, BoundsBytes);
		bEnqueued = true;
	});

	uint32 Encoded[BoundsUints] = {};
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSMeshOpsConsumeBounds)(
		[Readback, &Encoded, &bRead, bEnqueued, BoundsBytes](FRHICommandListImmediate& RHICmdList)
		{
			// EditMeshSync's flush only waits for the render thread; the copy itself is still in
			// flight on the GPU, so this is where the stall the operator's name warns about is.
			if (bEnqueued)
			{
				if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
				if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= BoundsBytes)
				{
					if (const uint32* Raw = static_cast<const uint32*>(Readback->Lock(BoundsBytes)))
					{
						FMemory::Memcpy(Encoded, Raw, BoundsBytes);
						Readback->Unlock();
						bRead = true;
					}
				}
			}
			delete Readback;
		});
	UCSMesh::CountedBlockingFlush();

	if (!bEnqueued)
	{
		// The mesh has no allocation, or no position stream to reduce. Not a failure worth a
		// warning — the caller gets the bounds it already had.
		UE_LOG(LogCSMeshOps, Log, TEXT("[CSMeshOps] ComputeWorldBoundsSync: nothing to reduce."));
		return Target;
	}
	if (!bRead)
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] ComputeWorldBoundsSync: the bounds readback did not complete."));
		return Target;
	}

	// Still at the clear value: no vertex reached the reduction, so the mesh is empty (or every
	// position was NaN). Either way the existing bounds are a better answer than an empty box.
	const bool bNothingContributed =
		Encoded[0] == 0xFFFFFFFFu && Encoded[1] == 0xFFFFFFFFu && Encoded[2] == 0xFFFFFFFFu
		&& Encoded[3] == 0u && Encoded[4] == 0u && Encoded[5] == 0u;
	if (bNothingContributed)
	{
		UE_LOG(LogCSMeshOps, Verbose, TEXT("[CSMeshOps] ComputeWorldBoundsSync: no live vertices; keeping the previous bounds."));
		return Target;
	}

	const FVector Min(
		CSMeshOps_OrderedUintToFloat(Encoded[0]),
		CSMeshOps_OrderedUintToFloat(Encoded[1]),
		CSMeshOps_OrderedUintToFloat(Encoded[2]));
	const FVector Max(
		CSMeshOps_OrderedUintToFloat(Encoded[3]),
		CSMeshOps_OrderedUintToFloat(Encoded[4]),
		CSMeshOps_OrderedUintToFloat(Encoded[5]));
	if (Min.ContainsNaN() || Max.ContainsNaN() || !(Min.X <= Max.X && Min.Y <= Max.Y && Min.Z <= Max.Z))
	{
		UE_LOG(LogCSMeshOps, Warning, TEXT("[CSMeshOps] ComputeWorldBoundsSync: the reduction produced an unusable box; keeping the previous bounds."));
		return Target;
	}

	// Through EditMeshSync rather than by writing the resident set directly: the bounds are part
	// of what a render consumer draws with, and this is what broadcasts the change to it.
	const FBox Bounds(Min, Max);
	Target->EditMeshSync([Bounds](FCSMeshEditContext& Context) { Context.SetWorldBounds(Bounds); });

	return Target;
}

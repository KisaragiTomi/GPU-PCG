#include "ComputeShaderMeshGenerator.h"
#include "CSBoxSceneCollectionImpl.h"
#include "CSNaniteHeightCapture.h"
#include "MeshGeneratorBrushCache.h"
#include "MeshGeneratorInternal.h"

#include "ComputeShaderBasicFunction.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderTargetPool.h"
#include "TextureResource.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAttributeSet.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "TimerManager.h"
#include "GameFramework/Actor.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GlobalShader.h"
#include "HAL/IConsoleManager.h"
#include "EngineUtils.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "CSPointArrowMesh.h"
#include "CSGpuMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshBuild.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSSurfaceVoxelPasses.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "MeshDescriptionToDynamicMesh.h"
#if WITH_EDITOR
#include "LandscapeDataAccess.h"
#endif
#include "RawIndexBuffer.h"
#include "RenderGraphResources.h"
#include "RenderResource.h"
#include "RHI.h"
#include "RHIResourceUtils.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshAttributes.h"
#include "Rendering/ColorVertexBuffer.h"
#include "UObject/UObjectIterator.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#endif

#include <openvdb/tools/ParticlesToLevelSet.h>
#include <openvdb/tools/VolumeToMesh.h>

// 跨 TU 的场景提取内部接口（结构体/常量/小 helper 已上移至 MeshGeneratorInternal.h）。
using namespace CSMeshGenInternal;

FRDGTextureRef CSMeshGenInternal::RegisterRenderTargetTexture(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* RenderTargetResource, const TCHAR* DebugName)
{
	if (!RenderTargetResource || !RenderTargetResource->GetRenderTargetTexture())
	{
		return nullptr;
	}
	return RegisterExternalTexture(GraphBuilder, RenderTargetResource->GetRenderTargetTexture(), DebugName);
}

namespace
{

// 同步榨干一组 GPU readback：必要时 SubmitAndBlockUntilGPUIdle 等待就绪、校验尺寸、
// 逐个 Lock/Memcpy/Unlock 到 Spec.Dst，并总是 delete 全部 readback 对象（与原三处手写逻辑一致）。
// 全部拷贝成功才返回 true；告警文案统一为 DebugLabel + 尺寸。
struct FCSMeshGenReadbackSpec
{
	FRHIGPUBufferReadback* Readback = nullptr;
	void* Dst = nullptr;
	uint32 NumBytes = 0;
};

bool CSMeshGen_DrainReadbacks(FRHICommandListImmediate& RHICmdList, TArrayView<const FCSMeshGenReadbackSpec> Specs, const TCHAR* DebugLabel)
{
	for (const FCSMeshGenReadbackSpec& Spec : Specs)
		if (!Spec.Readback) return false;

	auto AllReady = [&Specs]()
	{
		for (const FCSMeshGenReadbackSpec& Spec : Specs)
			if (!Spec.Readback->IsReady()) return false;
		return true;
	};
	auto DeleteAll = [&Specs]()
	{
		for (const FCSMeshGenReadbackSpec& Spec : Specs) delete Spec.Readback;
	};

	if (!AllReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();

	if (!AllReady())
	{
		UE_LOG(LogTemp, Warning, TEXT("[%s] GPU readback was not ready after flush."), DebugLabel);
		DeleteAll();
		return false;
	}

	for (const FCSMeshGenReadbackSpec& Spec : Specs)
	{
		if (Spec.Readback->GetGPUSizeBytes() < Spec.NumBytes)
		{
			UE_LOG(LogTemp, Warning, TEXT("[%s] GPU readback size mismatch: got %llu bytes, need %u."),
				DebugLabel, Spec.Readback->GetGPUSizeBytes(), Spec.NumBytes);
			DeleteAll();
			return false;
		}
	}

	bool bLockedAll = true;
	for (const FCSMeshGenReadbackSpec& Spec : Specs)
	{
		if (const void* Ptr = Spec.Readback->Lock(Spec.NumBytes))
		{
			FMemory::Memcpy(Spec.Dst, Ptr, Spec.NumBytes);
			Spec.Readback->Unlock();
		}
		else
		{
			bLockedAll = false;
		}
	}
	DeleteAll();
	return bLockedAll;
}

void CSMeshGen_ReleaseRT(UTextureRenderTarget2D* RT)
{
	if (RT) RT->ReleaseResource();
}
void UploadLinearColorsToRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor> Colors)
{
	if (!RenderTarget || RenderTarget->SizeX <= 0 || RenderTarget->SizeY <= 0) return;

	const int32 PixelCount = RenderTarget->SizeX * RenderTarget->SizeY;
	if (Colors.Num() < PixelCount) Colors.SetNumZeroed(PixelCount);
	else if (Colors.Num() > PixelCount) Colors.SetNum(PixelCount, EAllowShrinking::No);

	UComputeShaderBasicFunction::DrawLinearColorsToRenderTarget32(RenderTarget, MoveTemp(Colors));
}

class FCSGeneratorVDBParticleList
{
	struct FParticle
	{
		openvdb::Vec3R Position;
		openvdb::Real Radius = 0.0;
	};

public:
	using PosType = openvdb::Vec3R;

	explicit FCSGeneratorVDBParticleList(openvdb::Real InRadiusScale = 1.0)
		: RadiusScale(InRadiusScale)
	{
	}

	void Add(const FVector& Position, float Radius)
	{
		FParticle Particle;
		Particle.Position = openvdb::Vec3R(Position.X, Position.Y, Position.Z);
		Particle.Radius = Radius;
		Particles.push_back(Particle);
	}

	size_t size() const
	{
		return Particles.size();
	}

	void getPos(size_t Index, openvdb::Vec3R& OutPosition) const
	{
		OutPosition = Particles[Index].Position;
	}

	void getPosRad(size_t Index, openvdb::Vec3R& OutPosition, openvdb::Real& OutRadius) const
	{
		OutPosition = Particles[Index].Position;
		OutRadius = RadiusScale * Particles[Index].Radius;
	}

	void getPosRadVel(size_t Index, openvdb::Vec3R& OutPosition, openvdb::Real& OutRadius, openvdb::Vec3R& OutVelocity) const
	{
		getPosRad(Index, OutPosition, OutRadius);
		OutVelocity = openvdb::Vec3R(1.0, 1.0, 1.0);
	}

private:
	openvdb::Real RadiusScale = 1.0;
	std::vector<FParticle> Particles;
};

// Meshes an OpenVDB level set into a shared-vertex triangle list.
//
// Replaces the older FMeshDescription form: the GPU mesh object takes positions + indices, and
// routing them through a MeshDescription and an FDynamicMesh3 first only to read them back out
// again was two conversions that could not add anything — a VDB isosurface carries no UVs,
// colours or material slots, and its per-corner normals are recoverable from the winding.
void ConvertVDBVolumeToTriangles(openvdb::FloatGrid::ConstPtr SDFVolume, TArray<FVector>& OutVertices, TArray<int32>& OutIndices)
{
	OutVertices.Reset();
	OutIndices.Reset();
	if (!SDFVolume) return;

	std::vector<openvdb::Vec3s> Points;
	std::vector<openvdb::Vec3I> Triangles;
	std::vector<openvdb::Vec4I> Quads;
	openvdb::tools::volumeToMesh(*SDFVolume, Points, Triangles, Quads, 0.001, 0.25);

	OutVertices.Reserve(static_cast<int32>(Points.size()));
	for (const openvdb::Vec3s& Point : Points) OutVertices.Add(FVector(Point[0], Point[1], Point[2]));

	const int32 VertexNum = OutVertices.Num();
	auto AppendTriangle = [&OutIndices, VertexNum](int32 Index0, int32 Index1, int32 Index2)
	{
		if (Index0 < 0 || Index1 < 0 || Index2 < 0 || Index0 >= VertexNum || Index1 >= VertexNum || Index2 >= VertexNum) return;
		OutIndices.Add(Index0);
		OutIndices.Add(Index1);
		OutIndices.Add(Index2);
	};

	OutIndices.Reserve(static_cast<int32>(Quads.size()) * 6 + static_cast<int32>(Triangles.size()) * 3);
	for (const openvdb::Vec4I& Quad : Quads)
	{
		AppendTriangle(Quad[0], Quad[1], Quad[2]);
		AppendTriangle(Quad[2], Quad[3], Quad[0]);
	}

	for (const openvdb::Vec3I& Triangle : Triangles) AppendTriangle(Triangle[0], Triangle[1], Triangle[2]);
}
}

// 一个持久的 1 元素、全 0 的 float2 typed 顶点缓冲 + SRV。给不追踪 UV 的 extract 路径
// （如 heightmap）或源 mesh 无 tex-coord SRV 时绑到 SourceTexCoordBuffer，配合 NumTexCoords=0
// 让 shader 不真正读取它。用 PF_G32R32F 以匹配 shader 里 Buffer<float2> 的资源类型。
class FCSDummyTexCoordVertexBuffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override
	{
		const FVector2f Zero(0.0f, 0.0f);
		VertexBufferRHI = UE::RHIResourceUtils::CreateVertexBufferFromArray(
			RHICmdList, TEXT("CSDummyTexCoord"), EBufferUsageFlags::ShaderResource,
			MakeArrayView(&Zero, 1));
		ShaderResourceViewRHI = RHICmdList.CreateShaderResourceView(
			VertexBufferRHI,
			FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_G32R32F));
	}
};
static TGlobalResource<FCSDummyTexCoordVertexBuffer> GCSDummyTexCoordVertexBuffer;

class FExtractStaticMeshTrianglesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FExtractStaticMeshTrianglesCS);
	SHADER_USE_PARAMETER_STRUCT(FExtractStaticMeshTrianglesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<uint>, IndexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, PositionBuffer)
		SHADER_PARAMETER_SRV(Buffer<float2>, SourceTexCoordBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, ReferencePoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TriToMaterial)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutTriangleMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutTriangleReferenceFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float2>, RW_OutTriangleUVs)
		SHADER_PARAMETER(uint32, CSNumUVChannels)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, PositionStrideFloat)
		SHADER_PARAMETER(uint32, ReferenceCount)
		SHADER_PARAMETER(uint32, TriangleCapacity)
		SHADER_PARAMETER(uint32, NumTexCoords)
		SHADER_PARAMETER(uint32, bUseBounds)
		SHADER_PARAMETER(uint32, bUseReferenceFilter)
		SHADER_PARAMETER(float, ReferenceFilterDistanceSq)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FExtractStaticMeshTrianglesCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ExtractStaticMeshTrianglesCS", SF_Compute);


// Nanite 全细节源三角追加：读取 game thread 从 editor MeshDescription 提取、已变换到世界空间的源三角
// （world-space 顶点 + UV0 + 材质 registry id），原子追加进与 FExtractStaticMeshTrianglesCS 完全相同的
// triangle soup（复用同一 RW_TriangleCounter/顶点/法线/材质/UV UAV）。用于替代 Nanite 网格的低模 render fallback。
class FAppendSourceTrianglesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FAppendSourceTrianglesCS);
	SHADER_USE_PARAMETER_STRUCT(FAppendSourceTrianglesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SourceTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SourceTriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float2>, SourceTriangleUVs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SourceTriangleColors)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SourceTriangleTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SourceTriangleBiTangents)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SourceTriangleMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SourceTriangleReferenceFlags)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, ReferencePoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutTriangleMaterialIds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_OutTriangleReferenceFlags)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float2>, RW_OutTriangleUVs)
		SHADER_PARAMETER(uint32, CSNumUVChannels)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleBiTangents)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, TriangleCapacity)
		SHADER_PARAMETER(uint32, ReferenceCount)
		SHADER_PARAMETER(uint32, bUseBounds)
		SHADER_PARAMETER(uint32, bUseReferenceFilter)
		SHADER_PARAMETER(float, ReferenceFilterDistanceSq)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FAppendSourceTrianglesCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "AppendSourceTrianglesCS", SF_Compute);


class FFilterTriangleSoupByReferenceCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFilterTriangleSoupByReferenceCS);
	SHADER_USE_PARAMETER_STRUCT(FFilterTriangleSoupByReferenceCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, ReferencePoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutTriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_TriangleCounter)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, ReferenceCount)
		SHADER_PARAMETER(uint32, TriangleCapacity)
		SHADER_PARAMETER(uint32, bUseReferenceFilter)
		SHADER_PARAMETER(float, ReferenceFilterDistanceSq)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FFilterTriangleSoupByReferenceCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "FilterTriangleSoupByReferenceCS", SF_Compute);

// -----------------------------------------------------------------------------
// Core System - Surface Voxel Shaders
// -----------------------------------------------------------------------------

class FTriangleSurfaceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTriangleSurfaceVoxelsCS);
	SHADER_USE_PARAMETER_STRUCT(FTriangleSurfaceVoxelsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleVertices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, TriangleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceTriangleCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelNormalSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelNormalCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelTargetOffsetSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelTargetWeightSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, RW_OutVoxelCells)
		SHADER_PARAMETER(FVector3f, SurfaceVoxelOrigin)
		SHADER_PARAMETER(float, SurfaceVoxelSize)
		SHADER_PARAMETER(float, SurfaceThickness)
		SHADER_PARAMETER(uint32, SurfaceTriangleCount)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(uint32, SurfaceVoxelHashSlotCount)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FTriangleSurfaceVoxelsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "TriangleSurfaceVoxelsCS", SF_Compute);

class FFinalizeSurfaceVoxelNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FFinalizeSurfaceVoxelNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FFinalizeSurfaceVoxelNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>, SurfaceVoxelNormalSums)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceVoxelNormalCounts)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<int>, RW_SurfaceVoxelTargetOffsetSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelTargetWeightSums)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(float, SurfaceVoxelSize)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FFinalizeSurfaceVoxelNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "FinalizeSurfaceVoxelNormalsCS", SF_Compute);

// Ported from ResinRattan: 3D spatial blur on surface voxel normals and target positions
class FBlurSurfaceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FBlurSurfaceVoxelsCS);
	SHADER_USE_PARAMETER_STRUCT(FBlurSurfaceVoxelsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_OutVoxelTargetPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint4>, RW_OutVoxelCells)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashSlots)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_SurfaceVoxelHashIndices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_BlurredVoxelNormals)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_BlurredVoxelTargetPositions)
		SHADER_PARAMETER(uint32, SurfaceVoxelCapacity)
		SHADER_PARAMETER(uint32, SurfaceVoxelHashSlotCount)
		SHADER_PARAMETER(uint32, SurfaceVoxelBlurRadius)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FBlurSurfaceVoxelsCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "BlurSurfaceVoxelsCS", SF_Compute);
// -----------------------------------------------------------------------------
// Mesh → Heightmap rasterization（uint 高度图 → float 转换）
// -----------------------------------------------------------------------------

class FConvertHeightmapUintToFloatCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FConvertHeightmapUintToFloatCS);
	SHADER_USE_PARAMETER_STRUCT(FConvertHeightmapUintToFloatCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint>, T_HeightmapUint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(FIntPoint, HM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

IMPLEMENT_GLOBAL_SHADER(FConvertHeightmapUintToFloatCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "ConvertHeightmapUintToFloatCS", SF_Compute);

// index/position buffer 直读版光栅化：extract 的三角读取 + soup 光栅化融合成一个 kernel，
// 中间不再落 triangle soup（高度图只用位置，soup 的 normal/UV/材质通道在该路径全是死重）。
class FIndexedMeshToHeightmapCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FIndexedMeshToHeightmapCS);
	SHADER_USE_PARAMETER_STRUCT(FIndexedMeshToHeightmapCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_SRV(Buffer<uint>, IndexBuffer)
		SHADER_PARAMETER_SRV(Buffer<float>, PositionBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RW_HeightmapUint)
		SHADER_PARAMETER(FMatrix44f, LocalToWorld)
		SHADER_PARAMETER(FVector3f, BoundsMin)
		SHADER_PARAMETER(FVector3f, BoundsMax)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, PositionStrideFloat)
		SHADER_PARAMETER(uint32, bUseBounds)
		SHADER_PARAMETER(FVector2f, HM_BoundsMin)
		SHADER_PARAMETER(FVector2f, HM_BoundsInvSize)
		SHADER_PARAMETER(float, HM_CameraHeight)
		SHADER_PARAMETER(FIntPoint, HM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(64)
};

IMPLEMENT_GLOBAL_SHADER(FIndexedMeshToHeightmapCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "IndexedMeshToHeightmapCS", SF_Compute);

// min 合并版 uint→float 转换：跳过无三角覆盖的 texel（哨兵位型是 NaN），已覆盖的与现值取 min，
// 可与 LandscapeG16ToDepthCS 的地形深度按任意顺序叠加。
class FMergeHeightmapUintToFloatMinCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMergeHeightmapUintToFloatMinCS);
	SHADER_USE_PARAMETER_STRUCT(FMergeHeightmapUintToFloatMinCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<uint>, T_HeightmapUint)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(FIntPoint, HM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
};

IMPLEMENT_GLOBAL_SHADER(FMergeHeightmapUintToFloatMinCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "MergeHeightmapUintToFloatMinCS", SF_Compute);

// -----------------------------------------------------------------------------
// Core System - Internal Helpers
// -----------------------------------------------------------------------------

namespace
{
bool IsValidCSTriangleIndex(int32 Index, int32 VertexCount);
bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C);
bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, const TArray<FName>& ExcludedActorTags);

FVector ClosestPointOnTriangleForReferenceFilterCPU(const FVector& P, const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const FVector AP = P - A;
	const double D1 = FVector::DotProduct(AB, AP);
	const double D2 = FVector::DotProduct(AC, AP);
	if (D1 <= 0.0 && D2 <= 0.0)
	{
		return A;
	}

	const FVector BP = P - B;
	const double D3 = FVector::DotProduct(AB, BP);
	const double D4 = FVector::DotProduct(AC, BP);
	if (D3 >= 0.0 && D4 <= D3)
	{
		return B;
	}

	const double VC = D1 * D4 - D3 * D2;
	if (VC <= 0.0 && D1 >= 0.0 && D3 <= 0.0)
	{
		const double V = D1 / (D1 - D3);
		return A + AB * V;
	}

	const FVector CP = P - C;
	const double D5 = FVector::DotProduct(AB, CP);
	const double D6 = FVector::DotProduct(AC, CP);
	if (D6 >= 0.0 && D5 <= D6)
	{
		return C;
	}

	const double VB = D5 * D2 - D1 * D6;
	if (VB <= 0.0 && D2 >= 0.0 && D6 <= 0.0)
	{
		const double W = D2 / (D2 - D6);
		return A + AC * W;
	}

	const double VA = D3 * D6 - D5 * D4;
	if (VA <= 0.0 && (D4 - D3) >= 0.0 && (D5 - D6) >= 0.0)
	{
		const double W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
		return B + (C - B) * W;
	}

	const double Denom = 1.0 / (VA + VB + VC);
	const double V = VB * Denom;
	const double W = VC * Denom;
	return A + AB * V + AC * W;
}

bool PassTriangleReferenceFilterCPU(
	const FVector& A,
	const FVector& B,
	const FVector& C,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance)
{
	if (ReferencePoints.IsEmpty() || ReferenceFilterDistance <= 0.0f)
	{
		return true;
	}

	const double ReferenceFilterDistanceSq = FMath::Square(double(ReferenceFilterDistance));
	double BestDistSq = TNumericLimits<double>::Max();
	for (const FVector& ReferencePoint : ReferencePoints)
	{
		const FVector ClosestPoint = ClosestPointOnTriangleForReferenceFilterCPU(ReferencePoint, A, B, C);
		BestDistSq = FMath::Min(BestDistSq, FVector::DistSquared(ClosestPoint, ReferencePoint));
	}
	return BestDistSq <= ReferenceFilterDistanceSq;
}

FVector GetSafeTriangleNormal(const FVector& P0, const FVector& P1, const FVector& P2, const FVector& FallbackNormal = FVector::UpVector)
{
	const FVector Normal = FVector::CrossProduct(P1 - P0, P2 - P0).GetSafeNormal(UE_SMALL_NUMBER, FallbackNormal);
	return Normal.ContainsNaN() ? FallbackNormal : Normal;
}



bool TryAppendTriangleSoup(FCSTriangleMeshData& OutTriangleData,
	const FVector& P0,
	const FVector& P1,
	const FVector& P2,
	const FVector& Normal,
	int32 MaxTriangles)
{
	if (MaxTriangles > 0 && GetTriangleMeshDataTriangleCount(OutTriangleData) >= MaxTriangles)
	{
		return false;
	}

	if (!IsFiniteVector(P0) || !IsFiniteVector(P1) || !IsFiniteVector(P2) || IsDegenerateCSTriangle(P0, P1, P2))
	{
		return true;
	}

	const FVector SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	OutTriangleData.Vertices.Add(P0);
	OutTriangleData.Vertices.Add(P1);
	OutTriangleData.Vertices.Add(P2);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexNormals.Add(SafeNormal);
	OutTriangleData.VertexCount = OutTriangleData.Vertices.Num();
	OutTriangleData.IndexCount = 0;
	return true;
}

bool TryAppendTriangleSoupOrientedToNormal(FCSTriangleMeshData& OutTriangleData,
	const FVector& P0,
	const FVector& P1,
	const FVector& P2,
	const FVector& DesiredNormal,
	int32 MaxTriangles)
{
	const FVector SafeDesiredNormal = DesiredNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const FVector WindingNormal = GetSafeTriangleNormal(P0, P1, P2, SafeDesiredNormal);
	if (FVector::DotProduct(WindingNormal, SafeDesiredNormal) < 0.0)
	{
		return TryAppendTriangleSoup(OutTriangleData, P0, P2, P1, SafeDesiredNormal, MaxTriangles);
	}

	return TryAppendTriangleSoup(OutTriangleData, P0, P1, P2, SafeDesiredNormal, MaxTriangles);
}

FVector MakeLandscapeNormalFaceUp(const FVector& Normal)
{
	FVector SafeNormal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	if (SafeNormal.Z < 0.0)
	{
		SafeNormal *= -1.0;
	}
	return SafeNormal;
}

// NormalizeTriangleMeshDataWinding 随 GetBoxSceneTrianglesFilteredToDynamicMesh 一起删掉了：
// 它只服务那条"CPU 回读三角形 -> DynamicMesh"的出口。GPU 常驻提取（AppendBoxSceneTriangles）
// 原样保留源网格的绕序，不需要按顶点法线再纠一遍。

bool TriangleIntersectsBox(const FVector& P0, const FVector& P1, const FVector& P2, const FBox& QueryBox)
{
	if (!QueryBox.IsValid)
	{
		return true;
	}

	FBox TriangleBox(ForceInit);
	TriangleBox += P0;
	TriangleBox += P1;
	TriangleBox += P2;
	return TriangleBox.Intersect(QueryBox);
}

uint32 BuildTriangleUploadData(const FCSTriangleMeshData& TriangleData,
	uint32 TriangleCapacity,
	TArray<FVector4f>& OutVertices,
	TArray<FVector4f>& OutNormals)
{
	if (TriangleCapacity == 0)
	{
		return 0;
	}

	const int32 EffectiveVertexCount = GetEffectiveVertexCount(TriangleData);
	const int32 EffectiveIndexCount = GetEffectiveIndexCount(TriangleData);
	const bool bUseIndices = EffectiveIndexCount >= 3;
	const int32 SourceTriangleCount = bUseIndices ? EffectiveIndexCount / 3 : EffectiveVertexCount / 3;
	const int32 UploadTriangleCount = FMath::Min<int32>(SourceTriangleCount, int32(TriangleCapacity));
	if (UploadTriangleCount <= 0)
	{
		return 0;
	}

	OutVertices.Reset(UploadTriangleCount * 3);
	OutNormals.Reset(UploadTriangleCount * 3);
	OutVertices.Reserve(UploadTriangleCount * 3);
	OutNormals.Reserve(UploadTriangleCount * 3);

	const bool bUseVertexNormals = TriangleData.VertexNormals.Num() >= EffectiveVertexCount;
	for (int32 TriangleIndex = 0; TriangleIndex < UploadTriangleCount; ++TriangleIndex)
	{
		int32 I0 = TriangleIndex * 3 + 0;
		int32 I1 = TriangleIndex * 3 + 1;
		int32 I2 = TriangleIndex * 3 + 2;
		if (bUseIndices)
		{
			I0 = TriangleData.Indices[TriangleIndex * 3 + 0];
			I1 = TriangleData.Indices[TriangleIndex * 3 + 1];
			I2 = TriangleData.Indices[TriangleIndex * 3 + 2];
		}

		if (!IsValidCSTriangleIndex(I0, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I1, EffectiveVertexCount)
			|| !IsValidCSTriangleIndex(I2, EffectiveVertexCount))
		{
			continue;
		}

		const FVector& P0 = TriangleData.Vertices[I0];
		const FVector& P1 = TriangleData.Vertices[I1];
		const FVector& P2 = TriangleData.Vertices[I2];
		const FVector Normal = bUseVertexNormals
			? ((TriangleData.VertexNormals[I0] + TriangleData.VertexNormals[I1] + TriangleData.VertexNormals[I2]) / 3.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector)
			: GetSafeTriangleNormal(P0, P1, P2);

		OutVertices.Add(FVector4f(FVector3f(P0), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(P1), 1.0f));
		OutVertices.Add(FVector4f(FVector3f(P2), 1.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
		OutNormals.Add(FVector4f(FVector3f(Normal), 0.0f));
	}

	return uint32(OutVertices.Num() / 3);
}

bool ResolveTriangleRequest(const FCSStaticMeshTriangleRequest& Request,
	FResolvedStaticMeshTriangleRequest& OutResolved,
	bool bNaniteOnlyFallbackMesh = true)
{
	if (!Request.StaticMesh)
	{
		return false;
	}

	FStaticMeshRenderData* RenderData = Request.StaticMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		return false;
	}

	if (bNaniteOnlyFallbackMesh
		&& Request.StaticMesh->HasValidNaniteData()
		&& !Request.StaticMesh->HasNaniteFallbackMesh(GMaxRHIShaderPlatform))
	{
		UE_LOG(LogTemp, Verbose, TEXT("[ResolveTriangleRequest] Skip Nanite mesh without fallback mesh: %s"),
			*Request.StaticMesh->GetPathName());
		return false;
	}

	const int32 LODIndex = FMath::Clamp(Request.LODIndex, 0, RenderData->LODResources.Num() - 1);
	const int32 CurrentFirstLOD = RenderData->GetCurrentFirstLODIdx(Request.StaticMesh->GetMinLODIdx());
	if (LODIndex < CurrentFirstLOD || !RenderData->LODResources.IsValidIndex(LODIndex))
	{
		return false;
	}

	const FStaticMeshLODResources* LODResource = &RenderData->LODResources[LODIndex];
	if (!LODResource || LODResource->GetNumTriangles() <= 0 || LODResource->GetNumVertices() <= 0 || LODResource->BuffersSize <= 0)
	{
		return false;
	}

	if (!LODResource->VertexBuffers.PositionVertexBuffer.GetSRV() || !LODResource->IndexBuffer.GetRHI().IsValid())
	{
		return false;
	}

	OutResolved.LODResource = LODResource;
	OutResolved.LocalToWorld = FMatrix44f(Request.LocalToWorld.ToMatrixWithScale());
	OutResolved.WorldBounds = Request.WorldBounds.IsValid
		? FBox3f(FVector3f(Request.WorldBounds.Min), FVector3f(Request.WorldBounds.Max))
		: FBox3f(EForceInit::ForceInit);
	OutResolved.TriangleCount = LODResource->GetNumTriangles();
	OutResolved.PositionStrideFloat = FMath::Max(3, int32(LODResource->VertexBuffers.PositionVertexBuffer.GetStride() / sizeof(float)));
	return true;
}

FShaderResourceViewRHIRef CreateTriangleIndexBufferSRV(FRHICommandListImmediate& RHICmdList, const FStaticMeshLODResources* LODResource)
{
	if (!LODResource || !LODResource->IndexBuffer.GetRHI().IsValid())
	{
		return nullptr;
	}

	return RHICmdList.CreateShaderResourceView(
		LODResource->IndexBuffer.GetRHI(),
		FRHIViewDesc::CreateBufferSRV()
			.SetType(FRHIViewDesc::EBufferType::Typed)
			.SetFormat(LODResource->IndexBuffer.Is32Bit() ? PF_R32_UINT : PF_R16_UINT));
}

uint32 GetSurfaceVoxelHashSlotCount(int32 VoxelCapacity, int32 RequestedHashSlotCount)
{
	const uint32 SafeCapacity = uint32(FMath::Max(1, VoxelCapacity));
	const uint64 TargetSlots64 = RequestedHashSlotCount > 0
		? uint64(RequestedHashSlotCount)
		: FMath::Max<uint64>(1024ull, uint64(SafeCapacity) * 2ull);
	const uint32 TargetSlots = uint32(FMath::Min<uint64>(FMath::Max<uint64>(TargetSlots64, SafeCapacity), uint64(1u << 30)));

	uint32 SlotCount = 1u;
	while (SlotCount < TargetSlots && SlotCount < (1u << 30))
	{
		SlotCount <<= 1u;
	}
	return SlotCount;
}

bool IsValidCSTriangleIndex(int32 Index, int32 VertexCount)
{
	return Index >= 0 && Index < VertexCount;
}

bool IsFiniteCSVector4(const FVector4f& Vector)
{
	return FMath::IsFinite(Vector.X)
		&& FMath::IsFinite(Vector.Y)
		&& FMath::IsFinite(Vector.Z)
		&& FMath::IsFinite(Vector.W);
}

bool IsDegenerateCSTriangle(const FVector& A, const FVector& B, const FVector& C)
{
	const FVector AB = B - A;
	const FVector AC = C - A;
	const double AreaSq4 = FVector::CrossProduct(AB, AC).SizeSquared();
	return AreaSq4 <= 1.0e-8;
}

bool ShouldExcludeStaticMeshTriangleRequest(const FCSStaticMeshTriangleRequest& Request, const AActor* ExcludedActor, const TArray<FName>& ExcludedActorTags)
{
	const AActor* SourceActor = Request.SourceActor;
	if (!SourceActor)
	{
		return false;
	}

	if (ExcludedActor && SourceActor == ExcludedActor)
	{
		return true;
	}

	for (const FName& Tag : ExcludedActorTags)
	{
		if (!Tag.IsNone() && SourceActor->ActorHasTag(Tag))
		{
			return true;
		}
	}
	return false;
}

// 为一个已 resolve 的 request 构建 per-triangle 材质 id：按 LOD section 范围把三角映射到源材质，
// 材质经 SharedRegistry 去重成 registry id。空 section / 无材质槽时全部记为 CS_NO_MATERIAL_ID。
// Material registry dedupe key. Keying by material pointer alone merges every slot that
// resolves to the same material - and every empty slot, since they all hash as nullptr - so a
// source with five slots collapses to a single output slot. Including the source mesh and slot
// index keeps each source slot distinct, which is what lets an unassigned 5-slot mesh come out
// the other side as five (still empty) slots the user can fill in afterwards. Instances of the
// same mesh share the same keys, so instancing does not multiply slots.
struct FCSMaterialRegistryKey
{
	TObjectPtr<UMaterialInterface> Material = nullptr;
	const UStaticMesh* SourceMesh = nullptr;
	int32 SlotIndex = INDEX_NONE;

	bool operator==(const FCSMaterialRegistryKey& Other) const
	{
		return Material == Other.Material && SourceMesh == Other.SourceMesh && SlotIndex == Other.SlotIndex;
	}
};

FORCEINLINE uint32 GetTypeHash(const FCSMaterialRegistryKey& Key)
{
	return HashCombine(
		HashCombine(GetTypeHash(Key.Material.Get()), GetTypeHash(Key.SourceMesh)),
		::GetTypeHash(Key.SlotIndex));
}

// bPreserveSourceSlots=false reproduces the old pointer-only dedupe, which yields the most
// compact material list when many sources share one material.
FCSMaterialRegistryKey MakeMaterialRegistryKey(
	UMaterialInterface* Material, const UStaticMesh* SourceMesh, int32 SlotIndex, bool bPreserveSourceSlots)
{
	FCSMaterialRegistryKey Key;
	Key.Material = Material;
	if (bPreserveSourceSlots)
	{
		Key.SourceMesh = SourceMesh;
		Key.SlotIndex = SlotIndex;
	}
	return Key;
}

void BuildTriToMaterialForResolvedRequest(
	const FCSStaticMeshTriangleRequest& Request,
	FResolvedStaticMeshTriangleRequest& Resolved,
	TArray<TObjectPtr<UMaterialInterface>>& SharedRegistry,
	TMap<FCSMaterialRegistryKey, int32>& MaterialToRegistry,
	bool bPreserveSourceMaterialSlots)
{
	const int32 TriangleCount = FMath::Max(0, Resolved.TriangleCount);
	Resolved.TriToMaterial.Init(CS_NO_MATERIAL_ID, TriangleCount);

	const FStaticMeshLODResources* LODResource = Resolved.LODResource.GetReference();
	if (!LODResource || TriangleCount == 0 || Request.MaterialSlots.Num() == 0)
	{
		return;
	}

	for (const FStaticMeshSection& Section : LODResource->Sections)
	{
		UMaterialInterface* Material = Request.MaterialSlots.IsValidIndex(Section.MaterialIndex)
			? Request.MaterialSlots[Section.MaterialIndex].Get()
			: nullptr;

		const FCSMaterialRegistryKey Key = MakeMaterialRegistryKey(
			Material, Request.StaticMesh, Section.MaterialIndex, bPreserveSourceMaterialSlots);
		int32 RegistryIndex = INDEX_NONE;
		if (const int32* Found = MaterialToRegistry.Find(Key)) RegistryIndex = *Found;
		else
		{
			RegistryIndex = SharedRegistry.Add(Material);
			MaterialToRegistry.Add(Key, RegistryIndex);
		}

		const int32 FirstTri = FMath::Max(0, int32(Section.FirstIndex / 3u));
		const int32 LastTri = FMath::Min(FirstTri + int32(Section.NumTriangles), TriangleCount);
		for (int32 Tri = FirstTri; Tri < LastTri; ++Tri) Resolved.TriToMaterial[Tri] = uint32(RegistryIndex);
	}
}

// Triangle facing is topology, never shading data. Vertex normals are authored art:
// smoothed shells, foliage cards, baked-from-highpoly props, and hard creases routinely
// produce normals that oppose the geometric face or sit near-perpendicular to it, so a
// per-triangle "normals vote on the winding" rule flips isolated triangles and leaves the
// soup inconsistent with its own connectivity. The only transform that genuinely reverses
// world-space winding is a mirrored (negative-determinant) one, so that is the only case
// the extractor compensates for; everything else keeps the source corner order verbatim.
bool IsMirroredStaticMeshTransform(const FTransform& LocalToWorld)
{
	return LocalToWorld.GetDeterminant() < 0.0;
}

FVector3f TransformStaticMeshSourceNormal(
	const FTransform& LocalToWorld,
	const FVector3f& LocalNormal,
	const FVector3f& FallbackNormal)
{
	const FVector Scale = LocalToWorld.GetScale3D();
	if (FMath::Abs(Scale.X) <= UE_SMALL_NUMBER
		|| FMath::Abs(Scale.Y) <= UE_SMALL_NUMBER
		|| FMath::Abs(Scale.Z) <= UE_SMALL_NUMBER)
	{
		return FallbackNormal;
	}

	const FVector InverseScaledNormal(
		double(LocalNormal.X) / Scale.X,
		double(LocalNormal.Y) / Scale.Y,
		double(LocalNormal.Z) / Scale.Z);
	const FVector WorldNormal = LocalToWorld.TransformVectorNoScale(InverseScaledNormal);
	return FVector3f(WorldNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector(FallbackNormal)));
}

#if WITH_EDITOR
// 从 Nanite 网格的 LOD0 编辑器 MeshDescription 提取全细节源三角（数万~数百万级），绕过低模 render fallback。
// 每三角：3 个顶点位置经 LocalToWorld 变换到世界空间；按 request bounds 在 CPU 粗筛（等价于 render 路径的
// bUseBounds 逐三角剔除，同时减少上传量）；UV0 取 vertex-instance 通道 0；材质经 polygon group 的 slot name 在
// Request.MaterialSlots 里经共享 registry 去重（与 render 路径完全同一张表，保证材质槽统一）。
// 结果扁平追加进 OutTriangles（可跨多个 request 累积）。返回 true 表示成功提取；无可用 MeshDescription
// （cooked / 非 editor 数据）返回 false，调用方据此回退低模 render fallback 路径。
// 绕序说明：MeshDescription 三角绕序与 render index buffer 一致（两者同源于 build 前的源网格绕序），故上传后
// P0/P1/P2 直接交给 AppendSourceTrianglesCS 走与 ExtractStaticMeshTrianglesCS 逐字相同的反绕逻辑，输出朝向一致。
bool ExtractNaniteSourceTrianglesForRequest(
	const FCSStaticMeshTriangleRequest& Request,
	TArray<TObjectPtr<UMaterialInterface>>& SharedRegistry,
	TMap<FCSMaterialRegistryKey, int32>& MaterialToRegistry,
	bool bPreserveSourceMaterialSlots,
	FCSNaniteSourceTriangleData& OutTriangles)
{
	UStaticMesh* StaticMesh = Request.StaticMesh;
	if (!StaticMesh)
	{
		return false;
	}

	const FMeshDescription* MeshDescription = StaticMesh->GetMeshDescription(0);
	if (!MeshDescription)
	{
		return false;
	}

	const int32 SourceTriangleNum = MeshDescription->Triangles().Num();
	if (SourceTriangleNum <= 0)
	{
		return false;
	}

	FStaticMeshConstAttributes Attributes(*MeshDescription);
	TVertexAttributesConstRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceNormals = Attributes.GetVertexInstanceNormals();
	TVertexInstanceAttributesConstRef<FVector3f> VertexInstanceTangents = Attributes.GetVertexInstanceTangents();
	TVertexInstanceAttributesConstRef<float> VertexInstanceBinormalSigns = Attributes.GetVertexInstanceBinormalSigns();
	TVertexInstanceAttributesConstRef<FVector4f> VertexInstanceColors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesConstRef<FVector2f> VertexInstanceUVs = Attributes.GetVertexInstanceUVs();
	if (!VertexPositions.IsValid())
	{
		return false;
	}
	const bool bHasUVs = VertexInstanceUVs.IsValid() && VertexInstanceUVs.GetNumChannels() > 0;
	// 源模型实际有几条 UV。soup 是所有 request 共享的，通道数必须取到目前为止的最大值：
	// 先写进来的单 UV 网格已经按旧步长排好了，遇到更多通道的网格就得把已有数据重排，
	// 否则步长不一致会让整段 UV 错位。
	const int32 SourceUVChannels = bHasUVs
		? FMath::Clamp(VertexInstanceUVs.GetNumChannels(), 1, FCSGpuMeshCPUData::MaxTexCoordChannels)
		: 1;
	if (SourceUVChannels > OutTriangles.NumUVChannels)
	{
		const int32 OldChannels = OutTriangles.NumUVChannels;
		const int32 CornerCount = OldChannels > 0 ? OutTriangles.UVs.Num() / OldChannels : 0;
		TArray<FVector2f> Restrided;
		Restrided.SetNumZeroed(CornerCount * SourceUVChannels);
		for (int32 Corner = 0; Corner < CornerCount; ++Corner)
			for (int32 Channel = 0; Channel < OldChannels; ++Channel)
				Restrided[Corner * SourceUVChannels + Channel] = OutTriangles.UVs[Corner * OldChannels + Channel];
		OutTriangles.UVs = MoveTemp(Restrided);
		OutTriangles.NumUVChannels = SourceUVChannels;
	}
	const bool bHasNormals = VertexInstanceNormals.IsValid();
	const bool bHasTangents = VertexInstanceTangents.IsValid() && VertexInstanceBinormalSigns.IsValid();
	const bool bHasColors = VertexInstanceColors.IsValid();

	struct FComponentColorSample
	{
		FVector3f Position = FVector3f::ZeroVector;
		FVector3f Normal = FVector3f::ZeroVector;
		FVector4f Color = FVector4f(1, 1, 1, 1);
	};
	TArray<FComponentColorSample> ComponentColorSamples;
	TMap<FIntVector, TArray<int32>> ComponentColorCells;
	constexpr float ColorCellScale = 100.0f; // 0.01 cm cells tolerate render-buffer position quantization.
	auto ColorCell = [](const FVector3f& Position)
	{
		return FIntVector(
			FMath::RoundToInt(Position.X * ColorCellScale),
			FMath::RoundToInt(Position.Y * ColorCellScale),
			FMath::RoundToInt(Position.Z * ColorCellScale));
	};
	// Painted vertex colours are stored as sRGB-encoded bytes, but everything downstream - the
	// barycentric interpolation in the boolean rebuild and MeshDescription's VertexInstanceColors -
	// works in linear space (the engine applies ToFColor(true) itself when it builds render data,
	// see StaticMesh.cpp ~8448). Dividing by 255 only rescaled the bytes and left them gamma
	// encoded, so the value was encoded a second time on save and every result came out washed
	// out. FLinearColor(FColor) does the proper sRGB -> linear conversion.
	auto LinearVectorFromSRGBColor = [](const FColor& Color)
	{
		const FLinearColor Linear(Color);
		return FVector4f(Linear.R, Linear.G, Linear.B, Linear.A);
	};
	auto AddColorSample = [&](const FVector3f& Position, const FVector3f& Normal, const FColor& Color)
	{
		const int32 SampleIndex = ComponentColorSamples.Add({
			Position,
			Normal.GetSafeNormal(),
			LinearVectorFromSRGBColor(Color)});
		ComponentColorCells.FindOrAdd(ColorCell(Position)).Add(SampleIndex);
	};

	if (Request.SourceComponent && Request.SourceComponent->LODData.IsValidIndex(0))
	{
		const FStaticMeshComponentLODInfo& ComponentLOD = Request.SourceComponent->LODData[0];
		const FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
		const FStaticMeshLODResources* RenderLOD = RenderData && RenderData->LODResources.IsValidIndex(0)
			? &RenderData->LODResources[0]
			: nullptr;
		if (ComponentLOD.OverrideVertexColors && RenderLOD
			&& ComponentLOD.OverrideVertexColors->GetNumVertices() == RenderLOD->VertexBuffers.PositionVertexBuffer.GetNumVertices())
		{
			const uint32 NumColors = ComponentLOD.OverrideVertexColors->GetNumVertices();
			ComponentColorSamples.Reserve(NumColors);
			for (uint32 VertexIndex = 0; VertexIndex < NumColors; ++VertexIndex)
			{
				AddColorSample(
					RenderLOD->VertexBuffers.PositionVertexBuffer.VertexPosition(VertexIndex),
					FVector3f(RenderLOD->VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(VertexIndex)),
					ComponentLOD.OverrideVertexColors->VertexColor(VertexIndex));
			}
		}
		else
		{
			ComponentColorSamples.Reserve(ComponentLOD.PaintedVertices.Num());
			for (const FPaintedVertex& PaintedVertex : ComponentLOD.PaintedVertices)
				AddColorSample(
					FVector3f(PaintedVertex.Position),
					FVector3f(float(PaintedVertex.Normal.X), float(PaintedVertex.Normal.Y), float(PaintedVertex.Normal.Z)),
					PaintedVertex.Color);
		}
	}

	auto SourceColor = [&](const FVertexInstanceID VertexInstanceID)
	{
		const FVector3f Position = VertexPositions[MeshDescription->GetVertexInstanceVertex(VertexInstanceID)];
		const FVector3f Normal = bHasNormals ? VertexInstanceNormals[VertexInstanceID].GetSafeNormal() : FVector3f::ZeroVector;
		const FIntVector Cell = ColorCell(Position);
		int32 BestSample = INDEX_NONE;
		float BestScore = TNumericLimits<float>::Max();
		for (int32 Z = -1; Z <= 1; ++Z)
		for (int32 Y = -1; Y <= 1; ++Y)
		for (int32 X = -1; X <= 1; ++X)
		{
			const TArray<int32>* Candidates = ComponentColorCells.Find(Cell + FIntVector(X, Y, Z));
			if (!Candidates) continue;
			for (const int32 SampleIndex : *Candidates)
			{
				const FComponentColorSample& Sample = ComponentColorSamples[SampleIndex];
				const float PositionError = FVector3f::DistSquared(Position, Sample.Position);
				if (PositionError > 0.0004f) continue;
				const float NormalError = bHasNormals ? 1.0f - FVector3f::DotProduct(Normal, Sample.Normal) : 0.0f;
				const float Score = PositionError + FMath::Max(0.0f, NormalError) * 0.0001f;
				if (Score < BestScore) { BestScore = Score; BestSample = SampleIndex; }
			}
		}
		if (BestSample != INDEX_NONE) return ComponentColorSamples[BestSample].Color;
		// MeshDescription vertex colours are already linear; the old round trip through
		// ToFColor(true) gamma encoded them and the engine then encoded the result again.
		return bHasColors
			? VertexInstanceColors[VertexInstanceID]
			: FVector4f(1, 1, 1, 1);
	};

	// 按 polygon group 预解析一次 registry id：group 的 slot name 经 Request.MaterialSlots 进共享 registry。
	const int32 PolygonGroupArraySize = MeshDescription->PolygonGroups().GetArraySize();
	TArray<uint32> PolyGroupToRegistry;
	PolyGroupToRegistry.Init(CS_NO_MATERIAL_ID, FMath::Max(1, PolygonGroupArraySize));
	for (const FPolygonGroupID PolygonGroupID : MeshDescription->PolygonGroups().GetElementIDs())
	{
		// Single source of truth: the component's material array (Request.MaterialSlots, filled from
		// UStaticMeshComponent::GetMaterial, so component overrides already win). Polygon group index
		// is the material slot index for StaticMesh mesh descriptions, so no second lookup is needed.
		// The asset-side GetMaterialIndexFromImportedMaterialSlotName remap used to run here as well;
		// it read authority off the asset instead of the component and silently collapsed groups to
		// slot 0 whenever imported slot names were empty or duplicated, which is common for merged
		// and procedurally generated meshes.
		//
		// Keep this comment ASCII. It once held CJK text and the file on disk had no line break
		// between it and the statement below, so the lookup sat inside the comment and every
		// compiler dropped it - Material stayed null for every group and all materials were lost.
		const int32 MaterialIndex = PolygonGroupID.GetValue();
		UMaterialInterface* Material = Request.MaterialSlots.IsValidIndex(MaterialIndex)
			? Request.MaterialSlots[MaterialIndex].Get()
			: nullptr;

		// Key on the source slot so an unassigned multi-slot mesh keeps its slot count instead of
		// having every empty slot dedupe together into one.
		const FCSMaterialRegistryKey Key = MakeMaterialRegistryKey(
			Material, StaticMesh, MaterialIndex, bPreserveSourceMaterialSlots);
		int32 RegistryIndex = INDEX_NONE;
		if (const int32* Found = MaterialToRegistry.Find(Key)) RegistryIndex = *Found;
		else
		{
			RegistryIndex = SharedRegistry.Add(Material);
			MaterialToRegistry.Add(Key, RegistryIndex);
		}
		if (PolyGroupToRegistry.IsValidIndex(PolygonGroupID.GetValue())) PolyGroupToRegistry[PolygonGroupID.GetValue()] = uint32(RegistryIndex);
	}

	const FMatrix44f LocalToWorld(Request.LocalToWorld.ToMatrixWithScale());
	const FBox3f RequestBounds = Request.WorldBounds.IsValid
		? FBox3f(FVector3f(Request.WorldBounds.Min), FVector3f(Request.WorldBounds.Max))
		: FBox3f(EForceInit::ForceInit);
	const bool bUseBounds = (Request.WorldBounds.IsValid != 0);
	const bool bMirroredTransform = IsMirroredStaticMeshTransform(Request.LocalToWorld);
	const float TransformHandedness = bMirroredTransform ? -1.0f : 1.0f;
	// A mirrored instance reverses world-space winding, so swap two emitted corners to keep
	// the soup front face pointing where the engine's reversed culling renders it.
	const int32 SourceCornerOrder[3] = { 0, bMirroredTransform ? 2 : 1, bMirroredTransform ? 1 : 2 };

	OutTriangles.Positions.Reserve(OutTriangles.Positions.Num() + SourceTriangleNum * 3);
	OutTriangles.Normals.Reserve(OutTriangles.Normals.Num() + SourceTriangleNum * 3);
	OutTriangles.Colors.Reserve(OutTriangles.Colors.Num() + SourceTriangleNum * 3);
	OutTriangles.Tangents.Reserve(OutTriangles.Tangents.Num() + SourceTriangleNum * 3);
	OutTriangles.BiTangents.Reserve(OutTriangles.BiTangents.Num() + SourceTriangleNum * 3);
	OutTriangles.UVs.Reserve(OutTriangles.UVs.Num() + SourceTriangleNum * 3);
	OutTriangles.MaterialIds.Reserve(OutTriangles.MaterialIds.Num() + SourceTriangleNum);
	OutTriangles.ReferenceFlags.Reserve(OutTriangles.ReferenceFlags.Num() + SourceTriangleNum);

	for (const FTriangleID TriangleID : MeshDescription->Triangles().GetElementIDs())
	{
		TArrayView<const FVertexInstanceID> TriVertexInstances = MeshDescription->GetTriangleVertexInstances(TriangleID);
		if (TriVertexInstances.Num() < 3)
		{
			continue;
		}

		const FVertexInstanceID VertexInstances[3] =
		{
			TriVertexInstances[0],
			TriVertexInstances[1],
			TriVertexInstances[2]
		};
		FVector3f Positions[3];
		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const FVertexID VertexID = MeshDescription->GetVertexInstanceVertex(VertexInstances[Corner]);
			const FVector4f HomogeneousPosition = LocalToWorld.TransformPosition(VertexPositions[VertexID]);
			Positions[Corner] = FVector3f(
				HomogeneousPosition.X,
				HomogeneousPosition.Y,
				HomogeneousPosition.Z);
		}

		// Front face of the triangle as it is actually emitted below (UE convention:
		// cross(P2 - P0, P1 - P0) over the emitted corner order). Only used as the
		// fallback normal for assets that carry no vertex-instance normals.
		const FVector3f FaceNormal = FVector3f::CrossProduct(
			Positions[SourceCornerOrder[2]] - Positions[0],
			Positions[SourceCornerOrder[1]] - Positions[0])
			.GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
		auto WorldNormal = [&](const FVertexInstanceID VertexInstanceID)
		{
			if (!bHasNormals) return FaceNormal;
			return TransformStaticMeshSourceNormal(
				Request.LocalToWorld,
				VertexInstanceNormals[VertexInstanceID],
				FaceNormal);
		};
		auto WorldTangent = [&](const FVertexInstanceID VertexInstanceID, const FVector3f& WorldN)
		{
			if (!bHasTangents)
			{
				const FVector3f Axis = FMath::Abs(WorldN.Z) < 0.9f ? FVector3f::UnitZ() : FVector3f::UnitX();
				return FVector3f::CrossProduct(Axis, WorldN).GetSafeNormal();
			}
			FVector3f Tangent(Request.LocalToWorld.TransformVector(FVector(VertexInstanceTangents[VertexInstanceID])).GetSafeNormal());
			Tangent = (Tangent - FVector3f::DotProduct(Tangent, WorldN) * WorldN).GetSafeNormal();
			if (!Tangent.IsNearlyZero()) return Tangent;
			const FVector3f Axis = FMath::Abs(WorldN.Z) < 0.9f ? FVector3f::UnitZ() : FVector3f::UnitX();
			return FVector3f::CrossProduct(Axis, WorldN).GetSafeNormal();
		};
		// Normals are copied through as authored. Forcing them into the face hemisphere
		// here would be the same normals-versus-facing coupling in the other direction and
		// would destroy intentionally divergent art normals; the Boolean output stage
		// already aligns its own corner normals to the fragment's geometric face.
		const FVector3f WorldNormals[3] =
		{
			WorldNormal(VertexInstances[0]),
			WorldNormal(VertexInstances[1]),
			WorldNormal(VertexInstances[2])
		};

		if (bUseBounds)
		{
			FBox3f TriBox(EForceInit::ForceInit);
			TriBox += Positions[0];
			TriBox += Positions[1];
			TriBox += Positions[2];
			if (!TriBox.Intersect(RequestBounds))
			{
				continue;
			}
		}

		// 逐角点读出源模型的每一条 UV。源只有 1 条就只读 1 条，多的通道保持 (0,0)。
		FVector2f UVs[3][FCSGpuMeshCPUData::MaxTexCoordChannels];
		for (int32 Corner = 0; Corner < 3; ++Corner)
			for (int32 Channel = 0; Channel < FCSGpuMeshCPUData::MaxTexCoordChannels; ++Channel)
				UVs[Corner][Channel] = FVector2f::ZeroVector;
		if (bHasUVs)
		{
			for (int32 Corner = 0; Corner < 3; ++Corner)
				for (int32 Channel = 0; Channel < SourceUVChannels; ++Channel)
					UVs[Corner][Channel] = VertexInstanceUVs.Get(VertexInstances[Corner], Channel);
		}

		const FPolygonGroupID PolygonGroupID = MeshDescription->GetTrianglePolygonGroup(TriangleID);
		const uint32 MaterialId = PolyGroupToRegistry.IsValidIndex(PolygonGroupID.GetValue())
			? PolyGroupToRegistry[PolygonGroupID.GetValue()]
			: CS_NO_MATERIAL_ID;

		for (int32 UploadCorner = 0; UploadCorner < 3; ++UploadCorner)
		{
			const int32 SourceCorner = SourceCornerOrder[UploadCorner];
			const FVertexInstanceID VertexInstanceID = VertexInstances[SourceCorner];
			const FVector3f N = WorldNormals[SourceCorner];
			const FVector3f T = WorldTangent(VertexInstanceID, N);
			const float Sign = bHasTangents ? VertexInstanceBinormalSigns[VertexInstanceID] : 1.0f;
			const FVector3f B = FVector3f::CrossProduct(N, T).GetSafeNormal() * Sign * TransformHandedness;
			OutTriangles.Positions.Add(FVector4f(Positions[SourceCorner], 1.0f));
			OutTriangles.Normals.Add(FVector4f(N, 0.0f));
			OutTriangles.Tangents.Add(FVector4f(T, 0.0f));
			OutTriangles.BiTangents.Add(FVector4f(B, 0.0f));
			OutTriangles.Colors.Add(SourceColor(VertexInstanceID));
			// UV 按通道交错写入，步长 = soup 的通道数（各 request 取最大值，见 SourceUVChannels）。
			for (int32 Channel = 0; Channel < OutTriangles.NumUVChannels; ++Channel)
				OutTriangles.UVs.Add(UVs[SourceCorner][Channel]);
		}
		OutTriangles.MaterialIds.Add(MaterialId);
		OutTriangles.ReferenceFlags.Add(Request.bIsReference ? 1u : 0u);
		++OutTriangles.NumTriangles;
	}

	if (bMirroredTransform) UE_LOG(LogTemp, Log, TEXT("[SceneTriangles] compensated mirrored-instance winding (negative-determinant transform): %s"), *StaticMesh->GetPathName());
	return true;
}

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSSceneTriangleSourceOrientationAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.MeshBoolean.SourceSoupOrientation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSSceneTriangleSourceOrientationAutomationTest::RunTest(const FString& Parameters)
{
	// Winding must follow the source topology, so vertex normals never enter the decision
	// and only a negative-determinant transform reverses the emitted corner order.
	TestFalse(
		TEXT("A positive-determinant transform keeps the source triangle winding"),
		IsMirroredStaticMeshTransform(
			FTransform(FQuat::Identity, FVector::ZeroVector, FVector(2.0, 3.0, 4.0))));
	TestTrue(
		TEXT("One mirrored axis reverses the emitted winding"),
		IsMirroredStaticMeshTransform(
			FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-2.0, 3.0, 4.0))));
	TestFalse(
		TEXT("Two mirrored axes cancel and keep the source triangle winding"),
		IsMirroredStaticMeshTransform(
			FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-2.0, -3.0, 4.0))));
	TestTrue(
		TEXT("Three mirrored axes reverse the emitted winding"),
		IsMirroredStaticMeshTransform(
			FTransform(FQuat::Identity, FVector::ZeroVector, FVector(-2.0, -3.0, -4.0))));

	const FTransform MirroredTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(-2.0, 3.0, 4.0));
	const FVector3f MirroredNormal = TransformStaticMeshSourceNormal(
		MirroredTransform,
		FVector3f::UnitX(),
		FVector3f::UnitZ());
	TestTrue(
		TEXT("Source normals use inverse-transpose scaling for mirrored components"),
		MirroredNormal.Equals(-FVector3f::UnitX(), UE_KINDA_SMALL_NUMBER));
	return true;
}

#endif
#endif // WITH_EDITOR

} // 匿名空间暂停：以下定义属于 CSMeshGenInternal，需要外部链接（跨 TU 使用，声明见 MeshGeneratorInternal.h）

uint64 CSMeshGenInternal::ResolveStaticMeshTriangleRequests(
	const TArray<FCSStaticMeshTriangleRequest>& Requests,
	const AActor* ExcludedActor,
	const TArray<FName>& ExcludedActorTags,
	bool bNaniteOnlyFallbackMesh,
	TArray<FResolvedStaticMeshTriangleRequest>& OutResolvedRequests,
	TArray<TObjectPtr<UMaterialInterface>>* OutMaterialRegistry,
	FCSNaniteSourceTriangleData* OutNaniteTriangles,
	bool bPreserveSourceMaterialSlots,
	TArray<TWeakObjectPtr<UPrimitiveComponent>>* OutNaniteRenderComponents,
	bool bCollectAllRenderComponents)
{
	OutResolvedRequests.Reset();
	OutResolvedRequests.Reserve(Requests.Num());

	TArray<TObjectPtr<UMaterialInterface>> MaterialRegistry;
	TMap<FCSMaterialRegistryKey, int32> MaterialToRegistry;
	// ISM 每个实例一个 request，而"交不交给渲染器拍"是组件级的（还要做材质审计），按组件只判一次。
	TMap<const UStaticMeshComponent*, bool> RenderByComponent;

	// 返回值仅统计 render-resolve 出的三角（= OutResolvedRequests[i].TriangleCount 之和）。Nanite 全细节源三角
	// 单独累积进 *OutNaniteTriangles（其自带 NumTriangles），容量核算在 AddResolvedStaticMeshTrianglesToRDGInternal
	// 把两者相加完成，避免重复计数。OutNaniteTriangles == nullptr 时行为与改动前逐字一致（Nanite 网格照旧走
	// render fallback 并计入返回值），故所有未接入 Nanite 的既有调用方零回归。
	uint64 TotalTriangleCount = 0;
	for (const FCSStaticMeshTriangleRequest& Request : Requests)
	{
		if (ShouldExcludeStaticMeshTriangleRequest(Request, ExcludedActor, ExcludedActorTags))
		{
			continue;
		}

		// Nanite 网格：优先提取 editor MeshDescription 的全细节源几何，替代低模 render fallback。
		// 仅当调用方要求（OutNaniteTriangles 非空）时启用；提取成功即跳过 render resolve。
		if (OutNaniteTriangles && Request.StaticMesh)
		{
#if WITH_EDITOR
            if (ExtractNaniteSourceTrianglesForRequest(Request, MaterialRegistry, MaterialToRegistry, bPreserveSourceMaterialSlots, *OutNaniteTriangles)) continue;
            UE_LOG(LogTemp, Warning, TEXT("[ResolveStaticMeshTriangleRequests] Nanite mesh '%s' has no usable editor MeshDescription; using render fallback."), *Request.StaticMesh->GetPathName());
#else
            UE_LOG(LogTemp, Warning, TEXT("[ResolveStaticMeshTriangleRequests] Nanite mesh '%s' requires editor MeshDescription; using render fallback."), *Request.StaticMesh->GetPathName());
#endif
		}

		// 交给渲染器去拍的组件不在这里 resolve。默认只分流"按 Nanite 画着的"（这里 resolve 出来的
		// 只会是 fallback 低模）；bCollectAllRenderComponents 时所有拍得到的组件都走渲染器（单腿化：
		// 三角形路径不看材质，透明道具会被当成实心写进深度图）。拍不到的照旧留在这条路上。
		if (OutNaniteRenderComponents && Request.SourceComponent)
		{
			const UStaticMeshComponent* Component = Request.SourceComponent;
			const bool* bCached = RenderByComponent.Find(Component);
			const bool bRenderWithRenderer = bCached ? *bCached : RenderByComponent.Add(Component,
				bCollectAllRenderComponents
					? CSNaniteHeightCapture::IsCapturableComponent(Component)
					: CSNaniteHeightCapture::IsCapturableNaniteComponent(Component));
			if (bRenderWithRenderer)
			{
				OutNaniteRenderComponents->AddUnique(Request.SourceComponent.Get());
				continue;
			}
		}

		FResolvedStaticMeshTriangleRequest Resolved;
		if (!ResolveTriangleRequest(Request, Resolved, bNaniteOnlyFallbackMesh))
		{
			continue;
		}

		BuildTriToMaterialForResolvedRequest(Request, Resolved, MaterialRegistry, MaterialToRegistry, bPreserveSourceMaterialSlots);

		TotalTriangleCount = FMath::Min<uint64>(
			TotalTriangleCount + uint64(FMath::Max(0, Resolved.TriangleCount)),
			uint64(TNumericLimits<uint32>::Max()));
		OutResolvedRequests.Add(MoveTemp(Resolved));
	}

	if (OutMaterialRegistry) *OutMaterialRegistry = MoveTemp(MaterialRegistry);
	return TotalTriangleCount;
}

void CSMeshGenInternal::BuildBoxSceneLandscapeTrianglesInternal(UWorld* World,
	const FBox& QueryBox,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	FCSTriangleMeshData& OutTriangleData,
	const FTransform* WorldToLocalBoxTransform,
	const FVector* LocalBoxExtent,
	FName RequiredActorTag,
	bool bSortComponentsByDistance)
{
	OutTriangleData = FCSTriangleMeshData();

#if WITH_EDITOR
	if (!World || !QueryBox.IsValid || MaxTriangles == 0)
	{
		return;
	}

	const bool bHasOBBFilter = WorldToLocalBoxTransform && LocalBoxExtent;

	struct FLandscapeComponentEntry
	{
		ULandscapeComponent* Component;
		double DistSqToCenter;
	};
	TArray<FLandscapeComponentEntry> CandidateComponents;
	const FVector QueryCenter = QueryBox.GetCenter();

	for (TObjectIterator<ULandscapeComponent> It; It; ++It)
	{
		ULandscapeComponent* LandscapeComponent = *It;
		if (!IsValid(LandscapeComponent)
			|| LandscapeComponent->IsTemplate()
			|| !LandscapeComponent->IsRegistered()
			|| LandscapeComponent->GetWorld() != World)
		{
			continue;
		}

		if (!RequiredActorTag.IsNone())
		{
			ALandscapeProxy* LandscapeProxy = LandscapeComponent->GetLandscapeProxy();
			if (!LandscapeProxy || !LandscapeProxy->ActorHasTag(RequiredActorTag))
			{
				continue;
			}
		}

		if (!LandscapeComponent->Bounds.GetBox().Intersect(QueryBox))
		{
			continue;
		}

		const int32 ComponentSizeQuads = LandscapeComponent->ComponentSizeQuads;
		if (ComponentSizeQuads <= 0)
		{
			continue;
		}

		const double DistSq = FVector::DistSquared(LandscapeComponent->Bounds.Origin, QueryCenter);
		CandidateComponents.Add({ LandscapeComponent, DistSq });
	}

	if (bSortComponentsByDistance)
	{
		CandidateComponents.Sort([](const FLandscapeComponentEntry& A, const FLandscapeComponentEntry& B)
		{
			return A.DistSqToCenter < B.DistSqToCenter;
		});
	}

	for (const FLandscapeComponentEntry& Entry : CandidateComponents)
	{
		FLandscapeComponentDataInterface LandscapeData(Entry.Component, 0, false);
		if (!LandscapeData.GetRawHeightData())
		{
			continue;
		}

		const int32 ComponentSizeQuads = Entry.Component->ComponentSizeQuads;
		for (int32 Y = 0; Y < ComponentSizeQuads; ++Y)
		{
			for (int32 X = 0; X < ComponentSizeQuads; ++X)
			{
				if (MaxTriangles > 0 && GetTriangleMeshDataTriangleCount(OutTriangleData) >= MaxTriangles)
				{
					return;
				}

				FVector P00;
				FVector P10;
				FVector P01;
				FVector P11;
				FVector TangentX;
				FVector TangentY;
				FVector N00;
				FVector N10;
				FVector N01;
				FVector N11;
				LandscapeData.GetWorldPositionTangents(X, Y, P00, TangentX, TangentY, N00);
				LandscapeData.GetWorldPositionTangents(X + 1, Y, P10, TangentX, TangentY, N10);
				LandscapeData.GetWorldPositionTangents(X, Y + 1, P01, TangentX, TangentY, N01);
				LandscapeData.GetWorldPositionTangents(X + 1, Y + 1, P11, TangentX, TangentY, N11);

				auto PassesAllFilters = [&](const FVector& A, const FVector& B, const FVector& C) -> bool
				{
					if (!TriangleIntersectsBox(A, B, C, QueryBox))
					{
						return false;
					}
					if (bHasOBBFilter)
					{
						FBox LocalTriBox(ForceInit);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(A);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(B);
						LocalTriBox += WorldToLocalBoxTransform->TransformPosition(C);
						if (!LocalTriBox.Intersect(FBox(-*LocalBoxExtent, *LocalBoxExtent)))
						{
							return false;
						}
					}
					if (!PassTriangleReferenceFilterCPU(A, B, C, ReferencePoints, ReferenceFilterDistance))
					{
						return false;
					}
					return true;
				};

				if (PassesAllFilters(P00, P10, P11))
				{
					const FVector Tri0Normal = MakeLandscapeNormalFaceUp(N00 + N10 + N11);
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P10, P11, Tri0Normal, MaxTriangles))
					{
						return;
					}
				}

				if (PassesAllFilters(P00, P11, P01))
				{
					const FVector Tri1Normal = MakeLandscapeNormalFaceUp(N00 + N11 + N01);
					if (!TryAppendTriangleSoupOrientedToNormal(OutTriangleData, P00, P11, P01, Tri1Normal, MaxTriangles))
					{
						return;
					}
				}
			}
		}
	}
#else
	(void)World; (void)QueryBox; (void)ReferencePoints; (void)ReferenceFilterDistance;
	(void)MaxTriangles; (void)OutTriangleData; (void)WorldToLocalBoxTransform;
	(void)LocalBoxExtent; (void)RequiredActorTag; (void)bSortComponentsByDistance;
#endif
}

void CSMeshGenInternal::BuildBoxSceneTriangleRequestsInternal(UWorld* World,
	const FBox& QueryBox,
	int32 LODIndex,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests,
	const TArray<FName>& RequiredActorTags)
{
	OutRequests.Reset();
	if (!World || !QueryBox.IsValid)
	{
		return;
	}

	for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
	{
		UStaticMeshComponent* StaticMeshComponent = *It;
		if (!IsValid(StaticMeshComponent)
			|| StaticMeshComponent->IsTemplate()
			|| !StaticMeshComponent->IsRegistered()
			|| StaticMeshComponent->GetWorld() != World)
		{
			continue;
		}

		AActor* SourceActor = StaticMeshComponent->GetOwner();
		UStaticMesh* StaticMesh = StaticMeshComponent->GetStaticMesh();
		if (!StaticMesh)
		{
			continue;
		}

		// 无属主的组件不参与 tag 过滤（与旧 RemoveAll 语义一致：SourceActor 为空时保留）。
		if (!RequiredActorTags.IsEmpty() && SourceActor)
		{
			bool bHasRequiredTag = false;
			for (const FName& Tag : RequiredActorTags)
			{
				if (!Tag.IsNone() && SourceActor->ActorHasTag(Tag))
				{
					bHasRequiredTag = true;
					break;
				}
			}
			if (!bHasRequiredTag) continue;
		}

		// "Ref" 标签把网格标成参照体：进 winding 场但不切分、不输出。
		// actor 与 component 两级都认，方便按整个 actor 或单个组件标注。
		static const FName ReferenceTag(TEXT("Ref"));
		const bool bReferenceMesh =
			(SourceActor && SourceActor->ActorHasTag(ReferenceTag))
			|| StaticMeshComponent->ComponentHasTag(ReferenceTag);

		// override-aware 材质槽：section.MaterialIndex 索引进本数组。game thread 才能安全读组件材质。
		TArray<TObjectPtr<UMaterialInterface>> ComponentMaterialSlots;
		const int32 NumComponentMaterials = StaticMeshComponent->GetNumMaterials();
		ComponentMaterialSlots.Reserve(NumComponentMaterials);
		for (int32 MatIdx = 0; MatIdx < NumComponentMaterials; ++MatIdx) ComponentMaterialSlots.Add(StaticMeshComponent->GetMaterial(MatIdx));

		if (UInstancedStaticMeshComponent* InstancedComponent = Cast<UInstancedStaticMeshComponent>(StaticMeshComponent))
		{
			const FBox LocalMeshBounds = StaticMesh->GetBoundingBox();
			for (int32 InstanceIndex = 0; InstanceIndex < InstancedComponent->GetInstanceCount(); ++InstanceIndex)
			{
				FTransform InstanceTransform = FTransform::Identity;
				InstancedComponent->GetInstanceTransform(InstanceIndex, InstanceTransform, true);

				const FBox InstanceWorldBounds = LocalMeshBounds.TransformBy(InstanceTransform);
				if (!InstanceWorldBounds.Intersect(QueryBox))
				{
					continue;
				}

				FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
				Request.StaticMesh = StaticMesh;
				Request.LODIndex = LODIndex;
				Request.LocalToWorld = InstanceTransform;
				Request.WorldBounds = QueryBox;
				Request.SourceActor = SourceActor;
				Request.SourceComponent = StaticMeshComponent;
				Request.bIsReference = bReferenceMesh;
				Request.MaterialSlots = ComponentMaterialSlots;
			}
			continue;
		}

		const FBox ComponentWorldBounds = StaticMeshComponent->Bounds.GetBox();
		if (!ComponentWorldBounds.Intersect(QueryBox))
		{
			continue;
		}

		FCSStaticMeshTriangleRequest& Request = OutRequests.AddDefaulted_GetRef();
		Request.StaticMesh = StaticMesh;
		Request.LODIndex = LODIndex;
		Request.LocalToWorld = StaticMeshComponent->GetComponentTransform();
		Request.WorldBounds = QueryBox;
		Request.SourceActor = SourceActor;
		Request.SourceComponent = StaticMeshComponent;
		Request.bIsReference = bReferenceMesh;
		Request.MaterialSlots = MoveTemp(ComponentMaterialSlots);
	}
}

// -----------------------------------------------------------------------------
// Core System - Scene Requests
// -----------------------------------------------------------------------------

void AComputeShaderMeshGenerator::BuildBoxSceneTriangleRequests(UWorld* World,
	const FBox& QueryBox,
	TArray<FCSStaticMeshTriangleRequest>& OutRequests)
{
	BuildBoxSceneTriangleRequestsInternal(World, QueryBox, VoxelGridSettings.LODIndex, OutRequests);
}

// -----------------------------------------------------------------------------
// Core System - RDG Extraction
// -----------------------------------------------------------------------------

FCSStaticMeshTriangleRDGOutput CSMeshGenInternal::AddResolvedStaticMeshTrianglesToRDGInternal(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FResolvedStaticMeshTriangleRequest>& ResolvedRequests,
	uint64 TotalTriangleCount,
	const TArray<FVector>& ReferencePoints,
	float ReferenceFilterDistance,
	int32 MaxTriangles,
	const FCSTriangleMeshData* InitialTriangleData,
	const TCHAR* DebugName,
	const FCSNaniteSourceTriangleData* NaniteTriangles)
{
	FCSStaticMeshTriangleRDGOutput Output;

	const uint64 InitialTriangleCount = InitialTriangleData ? uint64(FMath::Max(0, GetTriangleMeshDataTriangleCount(*InitialTriangleData))) : 0ull;
	// Nanite 全细节源三角计入总容量：TotalTriangleCount 只含 render-resolve 三角，Nanite 三角单独在此累加。
	// 保证 soup buffer 尺寸覆盖 render + landscape(initial) + Nanite 三者之和（欠尺寸会裁剪输出）。
	const uint64 NaniteTriangleCount = NaniteTriangles ? uint64(FMath::Max(0, NaniteTriangles->NumTriangles)) : 0ull;
	const uint64 CombinedTriangleCount = FMath::Min<uint64>(TotalTriangleCount + InitialTriangleCount + NaniteTriangleCount, uint64(TNumericLimits<int32>::Max()));
	if (CombinedTriangleCount == 0)
	{
		return Output;
	}

	const int32 CombinedTriangleCountInt = int32(CombinedTriangleCount);
	const uint32 TriangleCapacity = uint32(FMath::Clamp(MaxTriangles > 0 ? MaxTriangles : CombinedTriangleCountInt, 1, CombinedTriangleCountInt));
	const uint32 VertexCapacity = TriangleCapacity * 3u;
	Output.MaxTriangles = TriangleCapacity;
	Output.MaxVertices = VertexCapacity;

	CSHelper::CreateClearedTypedBuffer(GraphBuilder, Output.TriangleVertices, Output.TriangleVerticesUAV, Output.TriangleVerticesSRV, sizeof(FVector4f), VertexCapacity, PF_A32B32G32R32F, TEXT("CS.StaticMeshTriangles.Vertices"), 0.0f);

	CSHelper::CreateClearedTypedBuffer(GraphBuilder, Output.TriangleNormals, Output.TriangleNormalsUAV, Output.TriangleNormalsSRV, sizeof(FVector4f), VertexCapacity, PF_A32B32G32R32F, TEXT("CS.StaticMeshTriangles.Normals"), 0.0f);

	CSHelper::CreateClearedTypedBuffer(GraphBuilder, Output.TriangleCounter, Output.TriangleCounterUAV, Output.TriangleCounterSRV, sizeof(uint32), 1, PF_R32_UINT, TEXT("CS.StaticMeshTriangles.Counter"), 0u);

	// 每 triangle 一个材质 id（与顶点平行，按 triangle 索引）。预清成 CS_NO_MATERIAL_ID。
	// GPU extract 只对 static mesh 三角写真实 registry id，地形/未写入的三角自动保持无材质。
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, Output.TriangleMaterialIds, Output.TriangleMaterialIdsUAV, Output.TriangleMaterialIdsSRV, sizeof(uint32), TriangleCapacity, PF_R32_UINT, TEXT("CS.StaticMeshTriangles.MaterialIds"), CS_NO_MATERIAL_ID);

	// 逐三角参照标志。清成 0：未写入的槽位（含地形）都按普通网格处理，参照体必须显式标注。
	CSHelper::CreateClearedTypedBuffer(GraphBuilder, Output.TriangleReferenceFlags, Output.TriangleReferenceFlagsUAV, Output.TriangleReferenceFlagsSRV, sizeof(uint32), TriangleCapacity, PF_R32_UINT, TEXT("CS.StaticMeshTriangles.ReferenceFlags"), 0u);

	// 每 vertex 一组 UV0（与顶点平行，3 per triangle）。预清成 (0,0)：GPU extract 只对 static mesh
	// 顶点写真实 UV；地形/未写入 UV 的源自动保持 (0,0)。PF_G32R32F 匹配 shader Buffer<float2>。
	// 逐角点、按通道交错：容量 = 顶点容量 * 通道数。通道数由源模型决定（见 NumUVChannels）。
	Output.NumUVChannels = FMath::Clamp(
		NaniteTriangles ? NaniteTriangles->NumUVChannels : 1, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	Output.TriangleUVs = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector2f), VertexCapacity * uint32(Output.NumUVChannels)),
		TEXT("CS.StaticMeshTriangles.UVs"));
	Output.TriangleUVsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.TriangleUVs, PF_G32R32F));
	Output.TriangleUVsSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Output.TriangleUVs, PF_G32R32F));
	AddClearUAVPass(GraphBuilder, Output.TriangleUVsUAV, 0.0f);

	auto CreateFloat4AttributeBuffer = [&GraphBuilder, VertexCapacity](const TCHAR* Name, FRDGBufferRef& Buffer, FRDGBufferUAVRef& UAV, FRDGBufferSRVRef& SRV)
	{
		Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VertexCapacity), Name);
		UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, PF_A32B32G32R32F));
		SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Buffer, PF_A32B32G32R32F));
		AddClearUAVPass(GraphBuilder, UAV, 0.0f);
	};
	CreateFloat4AttributeBuffer(TEXT("CS.StaticMeshTriangles.Colors"), Output.TriangleColors, Output.TriangleColorsUAV, Output.TriangleColorsSRV);
	AddClearUAVPass(GraphBuilder, Output.TriangleColorsUAV, 1.0f);
	CreateFloat4AttributeBuffer(TEXT("CS.StaticMeshTriangles.Tangents"), Output.TriangleTangents, Output.TriangleTangentsUAV, Output.TriangleTangentsSRV);
	CreateFloat4AttributeBuffer(TEXT("CS.StaticMeshTriangles.BiTangents"), Output.TriangleBiTangents, Output.TriangleBiTangentsUAV, Output.TriangleBiTangentsSRV);

	if (InitialTriangleData && InitialTriangleCount > 0)
	{
		TArray<FVector4f> InitialVertices;
		TArray<FVector4f> InitialNormals;
		const uint32 UploadTriangleCount = BuildTriangleUploadData(*InitialTriangleData, TriangleCapacity, InitialVertices, InitialNormals);
		if (UploadTriangleCount > 0)
		{
			const uint32 UploadVertexCount = UploadTriangleCount * 3u;
			FRDGBufferRef InitialVertexBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), UploadVertexCount),
				TEXT("CS.StaticMeshTriangles.InitialVertices"));
			FVector4f* InitialVertexUploadData = GraphBuilder.AllocPODArray<FVector4f>(UploadVertexCount);
			FMemory::Memcpy(InitialVertexUploadData, InitialVertices.GetData(), UploadVertexCount * sizeof(FVector4f));
			GraphBuilder.QueueBufferUpload(InitialVertexBuffer, InitialVertexUploadData, UploadVertexCount * sizeof(FVector4f));

			FRDGBufferRef InitialNormalBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), UploadVertexCount),
				TEXT("CS.StaticMeshTriangles.InitialNormals"));
			FVector4f* InitialNormalUploadData = GraphBuilder.AllocPODArray<FVector4f>(UploadVertexCount);
			FMemory::Memcpy(InitialNormalUploadData, InitialNormals.GetData(), UploadVertexCount * sizeof(FVector4f));
			GraphBuilder.QueueBufferUpload(InitialNormalBuffer, InitialNormalUploadData, UploadVertexCount * sizeof(FVector4f));

			const bool bFilterInitialTriangleSoup = ReferencePoints.Num() > 0 && ReferenceFilterDistance > 0.0f;
			if (bFilterInitialTriangleSoup)
			{
				FRDGBufferRef InitialCounterBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
					TEXT("CS.StaticMeshTriangles.InitialCounter"));
				uint32* InitialCounterUploadData = GraphBuilder.AllocPODArray<uint32>(1);
				*InitialCounterUploadData = UploadTriangleCount;
				GraphBuilder.QueueBufferUpload(InitialCounterBuffer, InitialCounterUploadData, sizeof(uint32));

				TArray<FVector4f> InitialReferencePointData;
				InitialReferencePointData.Reserve(ReferencePoints.Num());
				for (const FVector& ReferencePoint : ReferencePoints)
				{
					InitialReferencePointData.Add(FVector4f(FVector3f(ReferencePoint), 1.0f));
				}
				if (InitialReferencePointData.IsEmpty())
				{
					InitialReferencePointData.Add(FVector4f(0, 0, 0, 0));
				}

				FRDGBufferRef InitialReferencePointBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), InitialReferencePointData.Num()),
					TEXT("CS.StaticMeshTriangles.InitialReferencePoints"));
				FVector4f* InitialReferencePointUploadData = GraphBuilder.AllocPODArray<FVector4f>(InitialReferencePointData.Num());
				FMemory::Memcpy(InitialReferencePointUploadData, InitialReferencePointData.GetData(), InitialReferencePointData.Num() * sizeof(FVector4f));
				GraphBuilder.QueueBufferUpload(InitialReferencePointBuffer, InitialReferencePointUploadData, InitialReferencePointData.Num() * sizeof(FVector4f));

				TShaderMapRef<FFilterTriangleSoupByReferenceCS> FilterInitialShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FFilterTriangleSoupByReferenceCS::FParameters* FilterInitialParameters = GraphBuilder.AllocParameters<FFilterTriangleSoupByReferenceCS::FParameters>();
				FilterInitialParameters->TriangleVertices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialVertexBuffer, PF_A32B32G32R32F));
				FilterInitialParameters->TriangleNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialNormalBuffer, PF_A32B32G32R32F));
				FilterInitialParameters->SurfaceTriangleCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialCounterBuffer, PF_R32_UINT));
				FilterInitialParameters->ReferencePoints = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InitialReferencePointBuffer, PF_A32B32G32R32F));
				FilterInitialParameters->RW_OutTriangleVertices = Output.TriangleVerticesUAV;
				FilterInitialParameters->RW_OutTriangleNormals = Output.TriangleNormalsUAV;
				FilterInitialParameters->RW_TriangleCounter = Output.TriangleCounterUAV;
				FilterInitialParameters->TriangleCount = UploadTriangleCount;
				FilterInitialParameters->ReferenceCount = uint32(ReferencePoints.Num());
				FilterInitialParameters->TriangleCapacity = TriangleCapacity;
				FilterInitialParameters->bUseReferenceFilter = 1u;
				FilterInitialParameters->ReferenceFilterDistanceSq = ReferenceFilterDistance * ReferenceFilterDistance;

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("%s.FilterInitialTriangleSoup", DebugName),
					FilterInitialParameters,
					ERDGPassFlags::Compute,
					[FilterInitialParameters, FilterInitialShader, UploadTriangleCount](FRHIComputeCommandList& InRHICmdList)
					{
						FComputeShaderUtils::Dispatch(
							InRHICmdList,
							FilterInitialShader,
							*FilterInitialParameters,
							FComputeShaderUtils::GetGroupCount(FIntVector(int32(UploadTriangleCount), 1, 1), 64));
					});
			}
			else
			{
				AddCopyBufferPass(GraphBuilder, Output.TriangleVertices, 0, InitialVertexBuffer, 0, UploadVertexCount * sizeof(FVector4f));
				AddCopyBufferPass(GraphBuilder, Output.TriangleNormals, 0, InitialNormalBuffer, 0, UploadVertexCount * sizeof(FVector4f));

				const uint32 CounterValue = UploadTriangleCount;
				FRDGBufferRef InitialCounterBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1),
					TEXT("CS.StaticMeshTriangles.InitialCounter"));
				uint32* InitialCounterUploadData = GraphBuilder.AllocPODArray<uint32>(1);
				*InitialCounterUploadData = CounterValue;
				GraphBuilder.QueueBufferUpload(InitialCounterBuffer, InitialCounterUploadData, sizeof(uint32));
				AddCopyBufferPass(GraphBuilder, Output.TriangleCounter, 0, InitialCounterBuffer, 0, sizeof(uint32));
			}
		}
	}


	TArray<FVector4f> ReferencePointData;
	ReferencePointData.Reserve(ReferencePoints.Num());
	for (const FVector& ReferencePoint : ReferencePoints)
	{
		ReferencePointData.Add(FVector4f(FVector3f(ReferencePoint), 1.0f));
	}
	if (ReferencePointData.IsEmpty())
	{
		ReferencePointData.Add(FVector4f(0, 0, 0, 0));
	}

	FRDGBufferRef ReferencePointBuffer = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), ReferencePointData.Num()),
		TEXT("CS.StaticMeshTriangles.ReferencePoints"));
	FVector4f* ReferencePointUploadData = GraphBuilder.AllocPODArray<FVector4f>(ReferencePointData.Num());
	FMemory::Memcpy(ReferencePointUploadData, ReferencePointData.GetData(), ReferencePointData.Num() * sizeof(FVector4f));
	GraphBuilder.QueueBufferUpload(ReferencePointBuffer, ReferencePointUploadData, ReferencePointData.Num() * sizeof(FVector4f));
	FRDGBufferSRVRef ReferencePointSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ReferencePointBuffer, PF_A32B32G32R32F));

	TShaderMapRef<FExtractStaticMeshTrianglesCS> TriangleShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	Output.ReferencedIndexBufferSRVs.Reserve(ResolvedRequests.Num());
	for (const FResolvedStaticMeshTriangleRequest& Request : ResolvedRequests)
	{
		FShaderResourceViewRHIRef IndexBufferSRV = CreateTriangleIndexBufferSRV(RHICmdList, Request.LODResource.GetReference());
		FRHIShaderResourceView* PositionBufferSRV = Request.LODResource->VertexBuffers.PositionVertexBuffer.GetSRV();
		if (!IndexBufferSRV.IsValid() || !PositionBufferSRV)
		{
			continue;
		}
		Output.ReferencedIndexBufferSRVs.Add(IndexBufferSRV);

		// UV0 直接读源 mesh 的 tex-coord SRV（与 position 读法一致）。无 UV 的 mesh 侧绑 dummy + NumTexCoords=0。
		FRHIShaderResourceView* TexCoordSRV = Request.LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTexCoordsSRV();
		const uint32 RequestNumTexCoords = TexCoordSRV ? Request.LODResource->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() : 0u;
		if (!TexCoordSRV || RequestNumTexCoords == 0u) TexCoordSRV = GCSDummyTexCoordVertexBuffer.ShaderResourceViewRHI.GetReference();

		// 上传本 request 的 per-triangle 材质 id（Buffer<uint>，长度 == 源三角数）。
		// TriToMaterial 未填（其它调用方）时，退化为全 CS_NO_MATERIAL_ID，行为等价于无材质追踪。
		const int32 RequestTriangleCount = FMath::Max(1, Request.TriangleCount);
		FRDGBufferRef TriToMaterialBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), RequestTriangleCount),
			TEXT("CS.StaticMeshTriangles.TriToMaterial"));
		uint32* TriToMaterialUploadData = GraphBuilder.AllocPODArray<uint32>(RequestTriangleCount);
		if (Request.TriToMaterial.Num() == Request.TriangleCount && Request.TriangleCount > 0)
			FMemory::Memcpy(TriToMaterialUploadData, Request.TriToMaterial.GetData(), Request.TriangleCount * sizeof(uint32));
		else
			for (int32 FillIndex = 0; FillIndex < RequestTriangleCount; ++FillIndex) TriToMaterialUploadData[FillIndex] = CS_NO_MATERIAL_ID;
		GraphBuilder.QueueBufferUpload(TriToMaterialBuffer, TriToMaterialUploadData, RequestTriangleCount * sizeof(uint32));

		FExtractStaticMeshTrianglesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FExtractStaticMeshTrianglesCS::FParameters>();
		PassParameters->IndexBuffer = Output.ReferencedIndexBufferSRVs.Last().GetReference();
		PassParameters->PositionBuffer = PositionBufferSRV;
		PassParameters->SourceTexCoordBuffer = TexCoordSRV;
		PassParameters->ReferencePoints = ReferencePointSRV;
		PassParameters->TriToMaterial = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriToMaterialBuffer, PF_R32_UINT));
		PassParameters->RW_OutTriangleVertices = Output.TriangleVerticesUAV;
		PassParameters->RW_OutTriangleNormals = Output.TriangleNormalsUAV;
		PassParameters->RW_TriangleCounter = Output.TriangleCounterUAV;
		PassParameters->RW_OutTriangleMaterialIds = Output.TriangleMaterialIdsUAV;
		PassParameters->RW_OutTriangleReferenceFlags = Output.TriangleReferenceFlagsUAV;
		PassParameters->RW_OutTriangleUVs = Output.TriangleUVsUAV;
		PassParameters->CSNumUVChannels = uint32(Output.NumUVChannels);
		PassParameters->LocalToWorld = Request.LocalToWorld;
		PassParameters->BoundsMin = Request.WorldBounds.IsValid ? Request.WorldBounds.Min : FVector3f(-TNumericLimits<float>::Max());
		PassParameters->BoundsMax = Request.WorldBounds.IsValid ? Request.WorldBounds.Max : FVector3f(TNumericLimits<float>::Max());
		PassParameters->TriangleCount = uint32(Request.TriangleCount);
		PassParameters->PositionStrideFloat = uint32(Request.PositionStrideFloat);
		PassParameters->ReferenceCount = uint32(ReferencePoints.Num());
		PassParameters->TriangleCapacity = TriangleCapacity;
		PassParameters->NumTexCoords = RequestNumTexCoords;
		PassParameters->bUseBounds = Request.WorldBounds.IsValid ? 1u : 0u;
		PassParameters->bUseReferenceFilter = ReferencePoints.Num() > 0 ? 1u : 0u;
		PassParameters->ReferenceFilterDistanceSq = ReferenceFilterDistance > 0.0f
			? ReferenceFilterDistance * ReferenceFilterDistance
			: TNumericLimits<float>::Max();

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("%s.Extract", DebugName),
			PassParameters,
			ERDGPassFlags::Compute,
			[PassParameters, TriangleShader, TriangleCount = Request.TriangleCount, IndexBufferSRV](FRHIComputeCommandList& InRHICmdList)
			{
				(void)IndexBufferSRV;
				// wrapped：TriangleCount 可达数百万（65535*64=4.19M），需要 2D 包裹避免 GroupCount.X 越限 ensure（shader 侧 GetUnWrappedDispatchThreadId 还原）
				FComputeShaderUtils::Dispatch(InRHICmdList, TriangleShader, *PassParameters, FComputeShaderUtils::GetGroupCountWrapped(FMath::Max(1, int32(TriangleCount)), 64));
			});
	}

	// Nanite 全细节源三角：上传已变换到世界空间的源三角（顶点 + UV0 + 材质 id），由 AppendSourceTrianglesCS
	// 原子追加进上面 extract 已写入的同一 soup（共享 RW_TriangleCounter）。位置在 CPU 端已按 request bounds
	// 粗筛，故这里 bUseBounds=0；参考点过滤仍在 GPU 端执行以对齐 render 提取路径。声明在 extract 循环之后，
	// 依赖 RW_TriangleCounter 与 RDG 自动把本 pass 排在所有 extract pass 之后（追加顺序不影响最终 soup 内容）。
	if (NaniteTriangles && NaniteTriangles->NumTriangles > 0)
	{
		const int32 NaniteTriCount = NaniteTriangles->NumTriangles;
		const int32 NaniteVertCount = NaniteTriCount * 3;

		FRDGBufferRef NaniteVertexBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NaniteVertCount),
			TEXT("CS.StaticMeshTriangles.NaniteSrcVertices"));
		FVector4f* NaniteVertexUpload = GraphBuilder.AllocPODArray<FVector4f>(NaniteVertCount);
		FMemory::Memcpy(NaniteVertexUpload, NaniteTriangles->Positions.GetData(), NaniteVertCount * sizeof(FVector4f));
		GraphBuilder.QueueBufferUpload(NaniteVertexBuffer, NaniteVertexUpload, NaniteVertCount * sizeof(FVector4f));

		// UV 按通道交错：每角点 NumUVChannels 个 float2，buffer 与上传量都要乘上通道数。
		const int32 SourceUVChannelCount = FMath::Clamp(
			NaniteTriangles->NumUVChannels, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
		const uint32 NaniteUVElemCount = NaniteVertCount * uint32(SourceUVChannelCount);
		FRDGBufferRef NaniteUVBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(FVector2f), NaniteUVElemCount),
			TEXT("CS.StaticMeshTriangles.NaniteSrcUVs"));
		FVector2f* NaniteUVUpload = GraphBuilder.AllocPODArray<FVector2f>(NaniteUVElemCount);
		FMemory::Memcpy(NaniteUVUpload, NaniteTriangles->UVs.GetData(), NaniteUVElemCount * sizeof(FVector2f));
		GraphBuilder.QueueBufferUpload(NaniteUVBuffer, NaniteUVUpload, NaniteUVElemCount * sizeof(FVector2f));

		FRDGBufferRef NaniteNormalBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NaniteVertCount),
			TEXT("CS.StaticMeshTriangles.NaniteSrcNormals"));
		FVector4f* NaniteNormalUpload = GraphBuilder.AllocPODArray<FVector4f>(NaniteVertCount);
		FMemory::Memcpy(NaniteNormalUpload, NaniteTriangles->Normals.GetData(), NaniteVertCount * sizeof(FVector4f));
		GraphBuilder.QueueBufferUpload(NaniteNormalBuffer, NaniteNormalUpload, NaniteVertCount * sizeof(FVector4f));

		auto UploadFloat4Attribute = [&GraphBuilder, NaniteVertCount](const TCHAR* Name, const TArray<FVector4f>& Values)
		{
			FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NaniteVertCount), Name);
			FVector4f* Upload = GraphBuilder.AllocPODArray<FVector4f>(NaniteVertCount);
			FMemory::Memcpy(Upload, Values.GetData(), NaniteVertCount * sizeof(FVector4f));
			GraphBuilder.QueueBufferUpload(Buffer, Upload, NaniteVertCount * sizeof(FVector4f));
			return Buffer;
		};
		FRDGBufferRef NaniteColorBuffer = UploadFloat4Attribute(TEXT("CS.StaticMeshTriangles.SourceColors"), NaniteTriangles->Colors);
		FRDGBufferRef NaniteTangentBuffer = UploadFloat4Attribute(TEXT("CS.StaticMeshTriangles.SourceTangents"), NaniteTriangles->Tangents);
		FRDGBufferRef NaniteBiTangentBuffer = UploadFloat4Attribute(TEXT("CS.StaticMeshTriangles.SourceBiTangents"), NaniteTriangles->BiTangents);

		FRDGBufferRef NaniteMaterialBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NaniteTriCount),
			TEXT("CS.StaticMeshTriangles.NaniteSrcMaterialIds"));
		uint32* NaniteMaterialUpload = GraphBuilder.AllocPODArray<uint32>(NaniteTriCount);
		FMemory::Memcpy(NaniteMaterialUpload, NaniteTriangles->MaterialIds.GetData(), NaniteTriCount * sizeof(uint32));
		GraphBuilder.QueueBufferUpload(NaniteMaterialBuffer, NaniteMaterialUpload, NaniteTriCount * sizeof(uint32));

		// 参照标志与材质 id 同为 per-triangle uint，走同一套上传。
		FRDGBufferRef NaniteReferenceBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(1, NaniteTriCount)),
			TEXT("CS.StaticMeshTriangles.SourceReferenceFlags"));
		uint32* NaniteReferenceUpload = GraphBuilder.AllocPODArray<uint32>(FMath::Max(1, NaniteTriCount));
		FMemory::Memzero(NaniteReferenceUpload, FMath::Max(1, NaniteTriCount) * sizeof(uint32));
		if (NaniteTriangles->ReferenceFlags.Num() >= NaniteTriCount)
		{
			FMemory::Memcpy(NaniteReferenceUpload, NaniteTriangles->ReferenceFlags.GetData(), NaniteTriCount * sizeof(uint32));
		}
		GraphBuilder.QueueBufferUpload(NaniteReferenceBuffer, NaniteReferenceUpload, FMath::Max(1, NaniteTriCount) * sizeof(uint32));

		TShaderMapRef<FAppendSourceTrianglesCS> AppendShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FAppendSourceTrianglesCS::FParameters* AppendParameters = GraphBuilder.AllocParameters<FAppendSourceTrianglesCS::FParameters>();
		AppendParameters->SourceTriangleVertices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteVertexBuffer, PF_A32B32G32R32F));
		AppendParameters->SourceTriangleNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteNormalBuffer, PF_A32B32G32R32F));
		AppendParameters->SourceTriangleUVs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteUVBuffer, PF_G32R32F));
		AppendParameters->SourceTriangleColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteColorBuffer, PF_A32B32G32R32F));
		AppendParameters->SourceTriangleTangents = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteTangentBuffer, PF_A32B32G32R32F));
		AppendParameters->SourceTriangleBiTangents = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteBiTangentBuffer, PF_A32B32G32R32F));
		AppendParameters->SourceTriangleMaterialIds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteMaterialBuffer, PF_R32_UINT));
		AppendParameters->SourceTriangleReferenceFlags = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NaniteReferenceBuffer, PF_R32_UINT));
		AppendParameters->ReferencePoints = ReferencePointSRV;
		AppendParameters->RW_OutTriangleVertices = Output.TriangleVerticesUAV;
		AppendParameters->RW_OutTriangleNormals = Output.TriangleNormalsUAV;
		AppendParameters->RW_TriangleCounter = Output.TriangleCounterUAV;
		AppendParameters->RW_OutTriangleMaterialIds = Output.TriangleMaterialIdsUAV;
		AppendParameters->RW_OutTriangleReferenceFlags = Output.TriangleReferenceFlagsUAV;
		AppendParameters->RW_OutTriangleUVs = Output.TriangleUVsUAV;
		AppendParameters->CSNumUVChannels = uint32(Output.NumUVChannels);
		AppendParameters->RW_OutTriangleColors = Output.TriangleColorsUAV;
		AppendParameters->RW_OutTriangleTangents = Output.TriangleTangentsUAV;
		AppendParameters->RW_OutTriangleBiTangents = Output.TriangleBiTangentsUAV;
		AppendParameters->BoundsMin = FVector3f(-TNumericLimits<float>::Max());
		AppendParameters->BoundsMax = FVector3f(TNumericLimits<float>::Max());
		AppendParameters->TriangleCount = uint32(NaniteTriCount);
		AppendParameters->TriangleCapacity = TriangleCapacity;
		AppendParameters->ReferenceCount = uint32(ReferencePoints.Num());
		AppendParameters->bUseBounds = 0u; // CPU 端已按 request bounds 粗筛
		AppendParameters->bUseReferenceFilter = ReferencePoints.Num() > 0 ? 1u : 0u;
		AppendParameters->ReferenceFilterDistanceSq = ReferenceFilterDistance > 0.0f
			? ReferenceFilterDistance * ReferenceFilterDistance
			: TNumericLimits<float>::Max();

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("%s.AppendNaniteSource", DebugName),
			AppendParameters,
			ERDGPassFlags::Compute,
			[AppendParameters, AppendShader, NaniteTriCount](FRHIComputeCommandList& InRHICmdList)
			{
				// 与 extract 完全一致的包裹 dispatch：Nanite 源三角可达数百万
				FComputeShaderUtils::Dispatch(InRHICmdList, AppendShader, *AppendParameters, FComputeShaderUtils::GetGroupCountWrapped(FMath::Max(1, NaniteTriCount), 64));
			});
	}

	return Output;
}

// -----------------------------------------------------------------------------
// MakeBoxSceneCollectOptions / AddPreparedBoxSceneTrianglesToRDG
//
// 收集本体（原 PrepareBoxSceneTriangles）已搬去 CSBoxSceneCollection.cpp。留在这里的只有两件
// 与 actor 真正相关的事：把本 actor 的提取策略装进 options，以及 render 线程消费预备数据。
// -----------------------------------------------------------------------------

FCSBoxSceneCollectOptions AComputeShaderMeshGenerator::MakeBoxSceneCollectOptions(const FBox& QueryBox) const
{
	FCSBoxSceneCollectOptions Options;
	Options.QueryBox = QueryBox.IsValid ? QueryBox : GetGeneratorBoundsWorldBox();
	Options.MaxTriangles = FMath::Max(1, MaxTriangles);
	Options.ExcludedActor = this;
	Options.ExcludedActorTags = ExcludedActorTags;
	// 提取用哪级 LOD 必须与显存预检（ConfirmGpuMemoryBudgetForBoxScene）走同一个值，
	// 否则预检按一套三角数放行、提取按另一套分配。
	Options.LODIndex = VoxelGridSettings.LODIndex;
	return Options;
}

FCSStaticMeshTriangleRDGOutput AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const FCSBoxScenePreparedData& Prepared,
	const TCHAR* DebugName)
{
	FCSStaticMeshTriangleRDGOutput Output;
	if (!Prepared.IsValid() || !Prepared.HasAnyTriangles()) return Output;

	const auto& D = *Prepared.Impl;
	const FCSTriangleMeshData* InitialTriangleData =
		GetTriangleMeshDataTriangleCount(D.LandscapeTriangleData) > 0 ? &D.LandscapeTriangleData : nullptr;
	const FCSNaniteSourceTriangleData* NaniteTriangles = !D.NaniteTriangles.IsEmpty() ? &D.NaniteTriangles : nullptr;

	Output = AddResolvedStaticMeshTrianglesToRDGInternal(
		GraphBuilder, RHICmdList, D.ResolvedRequests, D.TotalStaticMeshTriangleCount,
		D.ReferencePoints, D.ReferenceFilterDistance, D.SafeMaxTriangles,
		InitialTriangleData, DebugName, NaniteTriangles);

	return Output;
}

FCSTriangleMeshData AComputeShaderMeshGenerator::GetBoxSceneTrianglesFromGPUFiltered(float ReferenceFilterDistance)
{
	FCSTriangleMeshData ResultTriangleData;
	ClearTriangleTextureData();

	UWorld* World = GetWorld();
	if (!World)
	{
		return ResultTriangleData;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return ResultTriangleData;
	}

	const TArray<FVector> ReferencePointsForRender = ReferencePoints;
	const float SafeFilterDistance = (ReferencePointsForRender.IsEmpty()) ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);
	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);

	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		World,
		QueryBox,
		ReferencePointsForRender,
		SafeFilterDistance,
		SafeMaxTriangles,
		LandscapeTriangleData);

	const bool bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return ResultTriangleData;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	// 与全局收集约定一致：Nanite 用 render fallback（不做 MeshDescription 全精度 CPU 提取）。
	// 全精度是 Mesh Boolean 勾选项专属；这个 legacy 回读/诊断入口跟随默认，不追加 Nanite 源三角。
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		this,
		ExcludedActorTags,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return ResultTriangleData;
	}

	FRHIGPUBufferReadback* VertexReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_VertexReadback"));
	FRHIGPUBufferReadback* NormalReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_NormalReadback"));
	FRHIGPUBufferReadback* CounterReadback = new FRHIGPUBufferReadback(TEXT("BoxSceneTrianglesFiltered_CounterReadback"));
	const uint32 CounterReadbackBytes = sizeof(uint32);
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	int32 VertexCapacity = 0;
	uint32 ActualVertexReadbackBytes = 0;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesFromGPUFilteredGPU)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), TotalStaticMeshTriangleCount, LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 VertexReadback, NormalReadback, CounterReadback, CounterReadbackBytes,
		 ReferencePointsForRender, SafeFilterDistance, SafeMaxTriangles,
		 &bRenderWorkQueued, &bHasGPUOutput, &VertexCapacity, &ActualVertexReadbackBytes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;
			FCSStaticMeshTriangleRDGOutput TriangleOutput = AddResolvedStaticMeshTrianglesToRDGInternal(
				GraphBuilder,
				RHICmdList,
				ResolvedRequests,
				TotalStaticMeshTriangleCount,
				ReferencePointsForRender,
				SafeFilterDistance,
				SafeMaxTriangles,
				InitialTriangleData,
				TEXT("CS.BoxSceneTrianglesFiltered"),
				nullptr);

			if (TriangleOutput.TriangleVertices && TriangleOutput.TriangleNormals && TriangleOutput.TriangleCounter)
			{
				VertexCapacity = int32(TriangleOutput.MaxVertices);
				ActualVertexReadbackBytes = uint32(uint64(TriangleOutput.MaxVertices) * sizeof(FVector4f));
				AddEnqueueCopyPass(GraphBuilder, VertexReadback, TriangleOutput.TriangleVertices, ActualVertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, NormalReadback, TriangleOutput.TriangleNormals, ActualVertexReadbackBytes);
				AddEnqueueCopyPass(GraphBuilder, CounterReadback, TriangleOutput.TriangleCounter, CounterReadbackBytes);
				bHasGPUOutput = true;
			}

			GraphBuilder.Execute();
			bRenderWorkQueued = true;
		});

	UCSMesh::CountedBlockingFlush();

	if (!bRenderWorkQueued || !bHasGPUOutput || VertexCapacity <= 0)
	{
		delete VertexReadback;
		delete NormalReadback;
		delete CounterReadback;
		return ResultTriangleData;
	}

	TArray<FVector4f> VertexData;
	TArray<FVector4f> NormalData;
	VertexData.SetNumZeroed(VertexCapacity);
	NormalData.SetNumZeroed(VertexCapacity);
	uint32 TriangleCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneTrianglesFromGPUFilteredReadback)(
		[VertexReadback, NormalReadback, CounterReadback, ActualVertexReadbackBytes, CounterReadbackBytes,
		 &VertexData, &NormalData, &TriangleCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			const FCSMeshGenReadbackSpec Specs[] = {
				{ VertexReadback, VertexData.GetData(), ActualVertexReadbackBytes },
				{ NormalReadback, NormalData.GetData(), ActualVertexReadbackBytes },
				{ CounterReadback, &TriangleCount, CounterReadbackBytes },
			};
			bReadbackSucceeded = CSMeshGen_DrainReadbacks(RHICmdList, Specs, TEXT("GetBoxSceneTrianglesFromGPUFiltered"));
		});

	UCSMesh::CountedBlockingFlush();

	if (!bReadbackSucceeded)
	{
		return ResultTriangleData;
	}

	const int32 MaxTriangleCapacity = VertexCapacity / 3;
	const int32 SafeTriangleCount = FMath::Clamp<int32>(int32(TriangleCount), 0, MaxTriangleCapacity);
	const int32 EffectiveVertexCount = FMath::Clamp(SafeTriangleCount * 3, 0, VertexData.Num());
	if (EffectiveVertexCount <= 0)
	{
		StoreTriangleTextureData(ResultTriangleData, SafeFilterDistance, QueryBox);
		return ResultTriangleData;
	}

	ResultTriangleData.Vertices.Reserve(EffectiveVertexCount);
	ResultTriangleData.VertexNormals.Reserve(EffectiveVertexCount);
	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector4f& Vertex = VertexData[VertexIndex];
		ResultTriangleData.Vertices.Add(FVector(Vertex.X, Vertex.Y, Vertex.Z));

		const FVector4f& Normal = NormalData[VertexIndex];
		ResultTriangleData.VertexNormals.Add(FVector(Normal.X, Normal.Y, Normal.Z));
	}
	ResultTriangleData.VertexCount = EffectiveVertexCount;
	ResultTriangleData.IndexCount = 0;

	StoreTriangleTextureData(ResultTriangleData, SafeFilterDistance, QueryBox);
	return ResultTriangleData;
}

void AComputeShaderMeshGenerator::GetBoxSceneFilteredSurfaceVoxels(float VoxelSize,
	float ReferenceFilterDistance,
	TArray<FVector>& OutPositions,
	TArray<FVector>& OutNormals)
{
	BuildBoxSceneFilteredSurfaceVoxels(VoxelSize, ReferenceFilterDistance, OutPositions, OutNormals, true);
}

// ===========================================================================
// 表面体素的记录式接口（声明见 Public/CSSurfaceVoxelPasses.h）
//
// 与下面的 PrepareBoxSceneSurfaceVoxelsGPU 走同一套 shader、同一套参数，区别只在于
// 谁拥有那张 RDG 图：旧接口自己开三张图再 Flush，这里只往调用者的图里记录，让体素能和
// 下游 pass 合并进同一张图。两条路径共存，旧接口的行为一个字节都没动。
// ===========================================================================

struct FCSSurfaceVoxelPassInputsImpl
{
	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	FCSTriangleMeshData LandscapeTriangleData;
	TArray<FVector> ReferencePoints;
	uint64 TotalStaticMeshTriangleCount = 0;
	float FilterDistance = 0.0f;
	float VoxelSize = 0.0f;
	FVector VoxelOrigin = FVector::ZeroVector;
	FBox WorldBounds = FBox(ForceInit);
	int32 MaxTriangles = 0;
	int32 MaxVoxels = 0;
	int32 BlurIterations = 0;
	int32 BlurRadius = 1;
	bool bHasLandscapeTriangles = false;
};

FCSSurfaceVoxelPassInputs::FCSSurfaceVoxelPassInputs() : Impl(MakeShared<FCSSurfaceVoxelPassInputsImpl>()) {}
FCSSurfaceVoxelPassInputs::~FCSSurfaceVoxelPassInputs() = default;
FCSSurfaceVoxelPassInputs::FCSSurfaceVoxelPassInputs(FCSSurfaceVoxelPassInputs&& Other) = default;
FCSSurfaceVoxelPassInputs& FCSSurfaceVoxelPassInputs::operator=(FCSSurfaceVoxelPassInputs&& Other) = default;
FCSSurfaceVoxelPassInputs::FCSSurfaceVoxelPassInputs(const FCSSurfaceVoxelPassInputs& Other) = default;
FCSSurfaceVoxelPassInputs& FCSSurfaceVoxelPassInputs::operator=(const FCSSurfaceVoxelPassInputs& Other) = default;

bool FCSSurfaceVoxelPassInputs::IsValid() const
{
	if (!Impl.IsValid() || Impl->VoxelSize <= UE_KINDA_SMALL_NUMBER) return false;
	return Impl->ResolvedRequests.Num() > 0 || Impl->bHasLandscapeTriangles;
}

FVector FCSSurfaceVoxelPassInputs::GetVoxelOrigin() const { return Impl.IsValid() ? Impl->VoxelOrigin : FVector::ZeroVector; }
FBox FCSSurfaceVoxelPassInputs::GetWorldBounds() const { return Impl.IsValid() ? Impl->WorldBounds : FBox(ForceInit); }
float FCSSurfaceVoxelPassInputs::GetVoxelSize() const { return Impl.IsValid() ? Impl->VoxelSize : 0.0f; }
uint32 FCSSurfaceVoxelPassInputs::GetVoxelCapacity() const { return Impl.IsValid() ? uint32(FMath::Max(1, Impl->MaxVoxels)) : 0u; }

bool AddCSSurfaceVoxelPasses(FRDGBuilder& GraphBuilder, const FCSSurfaceVoxelPassInputs& In, FCSSurfaceVoxelPassOutputs& Out)
{
	Out = FCSSurfaceVoxelPassOutputs();
	if (!In.IsValid()) return false;

	const FCSSurfaceVoxelPassInputsImpl& P = *In.Impl;
	const uint32 VoxCap = uint32(FMath::Max(1, P.MaxVoxels));
	const uint32 HashSlotCount = GetSurfaceVoxelHashSlotCount(P.MaxVoxels, 0);
	const float SafeVoxelSize = FMath::Max(P.VoxelSize, UE_KINDA_SMALL_NUMBER);
	const float SafeSurfaceThickness = SafeVoxelSize * 0.5f;

	// 全部是图内 transient buffer。旧路径必须用 AllocatePooledBuffer 是因为结果要跨图活到
	// 下一张图；这里下游就在同一张图里，交给 RDG 自己管生命周期即可。
	auto MakeBuf = [&GraphBuilder](uint32 BytesPerElement, uint32 NumElements, const TCHAR* Name)
	{
		return GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
	};
	FRDGBufferRef Positions = MakeBuf(sizeof(FVector4f), VoxCap, TEXT("CS.SV.Positions"));
	FRDGBufferRef Normals = MakeBuf(sizeof(FVector4f), VoxCap, TEXT("CS.SV.Normals"));
	FRDGBufferRef Counter = MakeBuf(sizeof(uint32), 2, TEXT("CS.SV.Counter"));
	FRDGBufferRef HashSlots = MakeBuf(sizeof(uint32), HashSlotCount, TEXT("CS.SV.HashSlots"));
	FRDGBufferRef HashIndices = MakeBuf(sizeof(uint32), HashSlotCount, TEXT("CS.SV.HashIndices"));
	FRDGBufferRef NormalSums = MakeBuf(sizeof(int32), VoxCap * 4u, TEXT("CS.SV.NormalSums"));
	FRDGBufferRef NormalCounts = MakeBuf(sizeof(uint32), VoxCap, TEXT("CS.SV.NormalCounts"));
	FRDGBufferRef TargetPositions = MakeBuf(sizeof(FVector4f), VoxCap, TEXT("CS.SV.TargetPositions"));
	FRDGBufferRef TargetOffsetSums = MakeBuf(sizeof(int32), VoxCap * 4u, TEXT("CS.SV.TargetOffsetSums"));
	FRDGBufferRef TargetWeightSums = MakeBuf(sizeof(uint32), VoxCap, TEXT("CS.SV.TargetWeightSums"));
	FRDGBufferRef Cells = MakeBuf(sizeof(int32) * 4, VoxCap, TEXT("CS.SV.Cells"));
	FRDGBufferRef BlurredNormals = MakeBuf(sizeof(FVector4f), VoxCap, TEXT("CS.SV.BlurredNormals"));
	FRDGBufferRef BlurredTargets = MakeBuf(sizeof(FVector4f), VoxCap, TEXT("CS.SV.BlurredTargets"));

	// ---- 清零（原 InitGraph）----
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_A32B32G32R32F)), 0.0f);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Normals, PF_A32B32G32R32F)), 0.0f);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counter, PF_R32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashSlots, PF_R32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashIndices, PF_R32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NormalSums, PF_R32_SINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NormalCounts, PF_R32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetPositions, PF_A32B32G32R32F)), 0.0f);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetOffsetSums, PF_R32_SINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetWeightSums, PF_R32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Cells, PF_R32G32B32A32_UINT)), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlurredNormals, PF_A32B32G32R32F)), 0.0f);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlurredTargets, PF_A32B32G32R32F)), 0.0f);

	// ---- 分批体素化（原 BatchGraph×N）----
	// 分批保留：每批的三角形临时 buffer 只被本批的体素化 pass 引用，RDG 的 transient
	// allocator 会在批次间复用同一块显存，所以峰值仍是「一批」而不是「全部」。批次之间
	// 通过写同一组 UAV 形成依赖，RDG 自动串行化，语义与原来的逐图 Execute 一致。
	const int32 BatchTriangleCap = FMath::Min(FMath::Max(1, P.MaxTriangles), 500000);
	const FCSTriangleMeshData* InitialTriangleData = P.bHasLandscapeTriangles ? &P.LandscapeTriangleData : nullptr;

	TArray<TArray<FResolvedStaticMeshTriangleRequest>> Batches;
	{
		TArray<FResolvedStaticMeshTriangleRequest> CurrentBatch;
		int64 CurrentBatchTriCount = 0;
		for (int32 i = 0; i < P.ResolvedRequests.Num(); ++i)
		{
			if (CurrentBatchTriCount + P.ResolvedRequests[i].TriangleCount > int64(BatchTriangleCap) && CurrentBatch.Num() > 0)
			{
				Batches.Add(MoveTemp(CurrentBatch));
				CurrentBatch.Reset();
				CurrentBatchTriCount = 0;
			}
			CurrentBatch.Add(P.ResolvedRequests[i]);
			CurrentBatchTriCount += P.ResolvedRequests[i].TriangleCount;
		}
		if (CurrentBatch.Num() > 0) Batches.Add(MoveTemp(CurrentBatch));
	}
	// 只有地形三角形、没有任何静态网格请求时，仍需要跑一批把地形喂进去。
	if (Batches.Num() == 0 && InitialTriangleData) Batches.AddDefaulted();

	TShaderMapRef<FTriangleSurfaceVoxelsCS> VoxelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	for (int32 BatchIdx = 0; BatchIdx < Batches.Num(); ++BatchIdx)
	{
		const TArray<FResolvedStaticMeshTriangleRequest>& Batch = Batches[BatchIdx];
		uint64 BatchTriCount = 0;
		for (const FResolvedStaticMeshTriangleRequest& Req : Batch) BatchTriCount += Req.TriangleCount;

		const FCSTriangleMeshData* BatchInitData = (BatchIdx == 0) ? InitialTriangleData : nullptr;
		FCSStaticMeshTriangleRDGOutput TriOut = CSMeshGenInternal::AddResolvedStaticMeshTrianglesToRDGInternal(
			GraphBuilder, GraphBuilder.RHICmdList, Batch, BatchTriCount, P.ReferencePoints, P.FilterDistance,
			BatchTriangleCap, BatchInitData, TEXT("CS.FilteredSV.Batch.Tri"), nullptr);
		if (!TriOut.TriangleVertices || !TriOut.TriangleNormals || !TriOut.TriangleCounter || TriOut.MaxTriangles <= 0) continue;

		FRDGBufferSRVRef TriVertsSRV = TriOut.TriangleVerticesSRV ? TriOut.TriangleVerticesSRV : GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleVertices, PF_A32B32G32R32F));
		FRDGBufferSRVRef TriNormsSRV = TriOut.TriangleNormalsSRV ? TriOut.TriangleNormalsSRV : GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleNormals, PF_A32B32G32R32F));
		FRDGBufferSRVRef TriCounterSRV = TriOut.TriangleCounterSRV ? TriOut.TriangleCounterSRV : GraphBuilder.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleCounter, PF_R32_UINT));

		FTriangleSurfaceVoxelsCS::FParameters* VP = GraphBuilder.AllocParameters<FTriangleSurfaceVoxelsCS::FParameters>();
		VP->TriangleVertices = TriVertsSRV;
		VP->TriangleNormals = TriNormsSRV;
		VP->SurfaceTriangleCounter = TriCounterSRV;
		VP->RW_OutVoxelPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_A32B32G32R32F));
		VP->RW_OutVoxelNormals = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Normals, PF_A32B32G32R32F));
		VP->RW_SurfaceVoxelCounter = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counter, PF_R32_UINT));
		VP->RW_SurfaceVoxelHashSlots = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashSlots, PF_R32_UINT));
		VP->RW_SurfaceVoxelHashIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashIndices, PF_R32_UINT));
		VP->RW_SurfaceVoxelNormalSums = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NormalSums, PF_R32_SINT));
		VP->RW_SurfaceVoxelNormalCounts = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NormalCounts, PF_R32_UINT));
		VP->RW_OutVoxelTargetPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetPositions, PF_A32B32G32R32F));
		VP->RW_SurfaceVoxelTargetOffsetSums = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetOffsetSums, PF_R32_SINT));
		VP->RW_SurfaceVoxelTargetWeightSums = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetWeightSums, PF_R32_UINT));
		VP->RW_OutVoxelCells = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Cells, PF_R32G32B32A32_UINT));
		VP->SurfaceVoxelOrigin = FVector3f(P.VoxelOrigin);
		VP->SurfaceVoxelSize = SafeVoxelSize;
		VP->SurfaceThickness = SafeSurfaceThickness;
		VP->SurfaceTriangleCount = TriOut.MaxTriangles;
		VP->SurfaceVoxelCapacity = VoxCap;
		VP->SurfaceVoxelHashSlotCount = HashSlotCount;

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("CS.FusedSV.Batch%d.Voxelize", BatchIdx), VP, ERDGPassFlags::Compute,
			[VP, VoxelShader, TriCap = TriOut.MaxTriangles](FRHIComputeCommandList& Cmd)
			{
				FComputeShaderUtils::Dispatch(Cmd, VoxelShader, *VP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(TriCap), 1, 1), 64));
			});
	}

	// ---- finalize + blur（原 FinalGraph）----
	FRDGBufferUAVRef PositionsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_A32B32G32R32F));
	FRDGBufferUAVRef NormalsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Normals, PF_A32B32G32R32F));
	FRDGBufferUAVRef TargetPositionsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetPositions, PF_A32B32G32R32F));

	TShaderMapRef<FFinalizeSurfaceVoxelNormalsCS> FinalizeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FFinalizeSurfaceVoxelNormalsCS::FParameters* FP = GraphBuilder.AllocParameters<FFinalizeSurfaceVoxelNormalsCS::FParameters>();
	FP->SurfaceVoxelNormalSums = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NormalSums, PF_R32_SINT));
	FP->SurfaceVoxelNormalCounts = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NormalCounts, PF_R32_UINT));
	FP->RW_SurfaceVoxelTargetOffsetSums = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetOffsetSums, PF_R32_SINT));
	FP->RW_SurfaceVoxelTargetWeightSums = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TargetWeightSums, PF_R32_UINT));
	FP->RW_OutVoxelPositions = PositionsUAV;
	FP->RW_OutVoxelNormals = NormalsUAV;
	FP->RW_OutVoxelTargetPositions = TargetPositionsUAV;
	FP->SurfaceVoxelCapacity = VoxCap;
	FP->SurfaceVoxelSize = SafeVoxelSize;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("CS.FusedSV.FinalizeNormals"), FP, ERDGPassFlags::Compute,
		[FP, FinalizeShader, VoxCap](FRHIComputeCommandList& Cmd)
		{
			FComputeShaderUtils::Dispatch(Cmd, FinalizeShader, *FP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
		});

	const uint32 BlurIters = uint32(FMath::Max(0, P.BlurIterations));
	if (BlurIters > 0u)
	{
		FRDGBufferUAVRef BlurNormalsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlurredNormals, PF_A32B32G32R32F));
		FRDGBufferUAVRef BlurTargetsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlurredTargets, PF_A32B32G32R32F));
		FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counter, PF_R32_UINT));
		FRDGBufferUAVRef CellsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Cells, PF_R32G32B32A32_UINT));
		FRDGBufferUAVRef HashSlotsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashSlots, PF_R32_UINT));
		FRDGBufferUAVRef HashIndicesUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(HashIndices, PF_R32_UINT));
		const uint32 BlurRadius = uint32(FMath::Max(1, P.BlurRadius));
		TShaderMapRef<FBlurSurfaceVoxelsCS> BlurShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		for (uint32 Iter = 0u; Iter < BlurIters; ++Iter)
		{
			// 乒乓：偶数轮从原 buffer 读、写进 blur buffer，奇数轮反过来。
			const bool bReadFromOriginal = (Iter % 2u) == 0u;
			FBlurSurfaceVoxelsCS::FParameters* BP = GraphBuilder.AllocParameters<FBlurSurfaceVoxelsCS::FParameters>();
			BP->RW_SurfaceVoxelCounter = CounterUAV;
			BP->RW_OutVoxelNormals = bReadFromOriginal ? NormalsUAV : BlurNormalsUAV;
			BP->RW_BlurredVoxelNormals = bReadFromOriginal ? BlurNormalsUAV : NormalsUAV;
			BP->RW_OutVoxelTargetPositions = bReadFromOriginal ? TargetPositionsUAV : BlurTargetsUAV;
			BP->RW_BlurredVoxelTargetPositions = bReadFromOriginal ? BlurTargetsUAV : TargetPositionsUAV;
			BP->RW_OutVoxelCells = CellsUAV;
			BP->RW_SurfaceVoxelHashSlots = HashSlotsUAV;
			BP->RW_SurfaceVoxelHashIndices = HashIndicesUAV;
			BP->SurfaceVoxelCapacity = VoxCap;
			BP->SurfaceVoxelHashSlotCount = HashSlotCount;
			BP->SurfaceVoxelBlurRadius = BlurRadius;
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CS.FusedSV.Blur%d", Iter), BP, ERDGPassFlags::Compute,
				[BP, BlurShader, VoxCap](FRHIComputeCommandList& Cmd)
				{
					FComputeShaderUtils::Dispatch(Cmd, BlurShader, *BP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
				});
		}
	}

	// blur 跑了奇数轮时，最终数据停在 blur buffer 里 —— 与旧路径同样的判定。
	const bool bFinalDataInBlurBuffers = BlurIters > 0u && (BlurIters & 1u) != 0u;
	Out.Positions = Positions;
	Out.Normals = bFinalDataInBlurBuffers ? BlurredNormals : Normals;
	Out.TargetPositions = bFinalDataInBlurBuffers ? BlurredTargets : TargetPositions;
	Out.Cells = Cells;
	Out.Counter = Counter;
	Out.HashSlots = HashSlots;
	Out.HashIndices = HashIndices;
	Out.VoxelCapacity = VoxCap;
	Out.HashSlotCount = HashSlotCount;
	Out.VoxelSize = SafeVoxelSize;
	Out.VoxelOrigin = P.VoxelOrigin;
	Out.WorldBounds = P.WorldBounds;
	Out.bValid = true;
	return true;
}

bool AComputeShaderMeshGenerator::PrepareSurfaceVoxelPassInputs(float VoxelSize, float ReferenceFilterDistance, FCSSurfaceVoxelPassInputs& OutInputs)
{
	OutInputs = FCSSurfaceVoxelPassInputs();
	FCSSurfaceVoxelPassInputsImpl& P = *OutInputs.Impl;

	UWorld* World = GetWorld();
	if (!World) return false;
	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid) return false;

	P.VoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	P.VoxelOrigin = QueryBox.Min;
	P.WorldBounds = QueryBox;
	P.MaxTriangles = FMath::Max(1, MaxTriangles);
	P.MaxVoxels = FMath::Max(1, MaxVoxels);
	P.BlurIterations = FMath::Max(0, SurfaceVoxelBlurIterations);
	P.BlurRadius = FMath::Max(1, SurfaceVoxelBlurRadius);
	P.ReferencePoints = ReferencePoints;
	P.FilterDistance = P.ReferencePoints.IsEmpty() ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);

	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);
	BuildBoxSceneLandscapeTrianglesInternal(World, QueryBox, P.ReferencePoints, P.FilterDistance, P.MaxTriangles, P.LandscapeTriangleData);
	P.bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(P.LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !P.bHasLandscapeTriangles) return false;

	P.TotalStaticMeshTriangleCount = CSMeshGenInternal::ResolveStaticMeshTriangleRequests(
		Requests, this, ExcludedActorTags, true, P.ResolvedRequests);
	return OutInputs.IsValid();
}

bool AComputeShaderMeshGenerator::PrepareBoxSceneSurfaceVoxelsGPU(float VoxelSize, float ReferenceFilterDistance)
{
	TArray<FVector> UnusedPositions;
	TArray<FVector> UnusedNormals;
	BuildBoxSceneFilteredSurfaceVoxels(VoxelSize, ReferenceFilterDistance, UnusedPositions, UnusedNormals, false);
	return LastSurfaceVoxelGPUBuffers.IsValid();
}

void AComputeShaderMeshGenerator::BuildBoxSceneFilteredSurfaceVoxels(float VoxelSize,
	float ReferenceFilterDistance,
	TArray<FVector>& OutPositions,
	TArray<FVector>& OutNormals,
	bool bReadbackToCPU)
{
	OutPositions.Reset();
	OutNormals.Reset();

	const float SafeVoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	const TArray<FVector> ReferencePointsForRender = ReferencePoints;
	const float SafeFilterDistance = (ReferencePointsForRender.IsEmpty()) ? 0.0f : FMath::Max(0.0f, ReferenceFilterDistance);
	LastSurfaceVoxelData = FCSSurfaceVoxelData();
	LastSurfaceVoxelData.VoxelSize = SafeVoxelSize;
	LastSurfaceVoxelTextureData.bValid = false;
	LastSurfaceVoxelTextureData.VoxelCount = 0;
	LastSurfaceVoxelTextureData.VoxelSize = SafeVoxelSize;
	LastSurfaceVoxelGPUBuffers.Reset();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid)
	{
		return;
	}

	const int32 SafeMaxTriangles = FMath::Max(1, MaxTriangles);
	const int32 SafeMaxVoxels = FMath::Max(1, MaxVoxels);
	const int32 SafeSurfaceVoxelBlurIterations = FMath::Max(0, SurfaceVoxelBlurIterations);
	const int32 SafeSurfaceVoxelBlurRadius = FMath::Max(1, SurfaceVoxelBlurRadius);
	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequests(World, QueryBox, Requests);

	FCSTriangleMeshData LandscapeTriangleData;
	BuildBoxSceneLandscapeTrianglesInternal(
		World,
		QueryBox,
		ReferencePointsForRender,
		SafeFilterDistance,
		SafeMaxTriangles,
		LandscapeTriangleData);

	const bool bHasLandscapeTriangles = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0;
	if (Requests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return;
	}

	TArray<FResolvedStaticMeshTriangleRequest> ResolvedRequests;
	const uint64 TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		Requests,
		this,
		ExcludedActorTags,
		true,
		ResolvedRequests);
	if (ResolvedRequests.IsEmpty() && !bHasLandscapeTriangles)
	{
		return;
	}

	const uint64 MaxVoxelReadbackBytes64 = uint64(SafeMaxVoxels) * sizeof(FVector4f);
	if (bReadbackToCPU && MaxVoxelReadbackBytes64 > uint64(TNumericLimits<uint32>::Max()))
	{
		UE_LOG(LogTemp, Warning, TEXT("[GetBoxSceneFilteredSurfaceVoxels] Readback request too large. MaxVoxels=%d"), SafeMaxVoxels);
		return;
	}

	const uint32 CounterReadbackBytes = sizeof(uint32) * 2;

	FRHIGPUBufferReadback* PositionReadback = bReadbackToCPU ? new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_PositionReadback")) : nullptr;
	FRHIGPUBufferReadback* NormalReadback = bReadbackToCPU ? new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_NormalReadback")) : nullptr;
	FRHIGPUBufferReadback* TargetPositionReadback = bReadbackToCPU ? new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_TargetPositionReadback")) : nullptr;
	FRHIGPUBufferReadback* CellReadback = bReadbackToCPU ? new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_CellReadback")) : nullptr;
	FRHIGPUBufferReadback* CounterReadback = bReadbackToCPU ? new FRHIGPUBufferReadback(TEXT("FilteredSurfaceVoxels_CounterReadback")) : nullptr;
	bool bRenderWorkQueued = false;
	bool bHasGPUOutput = false;
	int32 VoxelCapacity = 0;
	uint32 ActualVoxelReadbackBytes = 0;
	uint32 ActualVoxelCellReadbackBytes = 0;

	// Trip B: retain the GPU voxel buffers so a consumer can register them directly
	// instead of reading them back + re-uploading. The game thread blocks on
	// FlushRenderingCommands below, so storing into this member from the render lambda
	// is race-free (the flush is a barrier). Counts/params are filled after the flush.
	FCSSurfaceVoxelGPUBuffers* OutGPUVoxels = &LastSurfaceVoxelGPUBuffers;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneFilteredSurfaceVoxelsGPU)(
		[ResolvedRequests = MoveTemp(ResolvedRequests), TotalStaticMeshTriangleCount, LandscapeTriangleData = MoveTemp(LandscapeTriangleData),
		 PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, CounterReadbackBytes,
		 SafeMaxTriangles, SafeMaxVoxels, VoxelOrigin = QueryBox.Min, VoxelWorldBounds = QueryBox, SafeVoxelSize, bReadbackToCPU,
		 ReferencePointsForRender, SafeFilterDistance, SafeSurfaceVoxelBlurIterations, SafeSurfaceVoxelBlurRadius, OutGPUVoxels,
		 &bRenderWorkQueued, &bHasGPUOutput, &VoxelCapacity, &ActualVoxelReadbackBytes, &ActualVoxelCellReadbackBytes](FRHICommandListImmediate& RHICmdList)
		{
			const uint32 VoxCap = uint32(FMath::Max(1, SafeMaxVoxels));
			const uint32 HashSlotCount = GetSurfaceVoxelHashSlotCount(SafeMaxVoxels, 0);
			const float SafeSurfaceThickness = SafeVoxelSize * 0.5f;

			TRefCountPtr<FRDGPooledBuffer> VoxelPositionsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.Positions"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.Normals"));
			TRefCountPtr<FRDGPooledBuffer> VoxelCounterBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("CS.SV.Counter"));
			TRefCountPtr<FRDGPooledBuffer> VoxelHashSlotsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HashSlotCount), TEXT("CS.SV.HashSlots"));
			TRefCountPtr<FRDGPooledBuffer> VoxelHashIndicesBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), HashSlotCount), TEXT("CS.SV.HashIndices"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxCap * 4u), TEXT("CS.SV.NormalSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelNormalCountsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxCap), TEXT("CS.SV.NormalCounts"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetPositionsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.TargetPositions"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetOffsetSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32), VoxCap * 4u), TEXT("CS.SV.TargetOffsetSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelTargetWeightSumsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), VoxCap), TEXT("CS.SV.TargetWeightSums"));
			TRefCountPtr<FRDGPooledBuffer> VoxelCellsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32) * 4, VoxCap), TEXT("CS.SV.Cells"));
			TRefCountPtr<FRDGPooledBuffer> BlurredNormalsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.BlurredNormals"));
			TRefCountPtr<FRDGPooledBuffer> BlurredTargetsBuf = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VoxCap), TEXT("CS.SV.BlurredTargets"));

			{
				FRDGBuilder InitGraph(RHICmdList);
				FRDGBufferRef RDG_Positions = InitGraph.RegisterExternalBuffer(VoxelPositionsBuf);
				FRDGBufferRef RDG_Normals = InitGraph.RegisterExternalBuffer(VoxelNormalsBuf);
				FRDGBufferRef RDG_Counter = InitGraph.RegisterExternalBuffer(VoxelCounterBuf);
				FRDGBufferRef RDG_HashSlots = InitGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
				FRDGBufferRef RDG_HashIndices = InitGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
				FRDGBufferRef RDG_NormalSums = InitGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
				FRDGBufferRef RDG_NormalCounts = InitGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
				FRDGBufferRef RDG_TargetPositions = InitGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
				FRDGBufferRef RDG_TargetOffsetSums = InitGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
				FRDGBufferRef RDG_TargetWeightSums = InitGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
				FRDGBufferRef RDG_Cells = InitGraph.RegisterExternalBuffer(VoxelCellsBuf);
				FRDGBufferRef RDG_BlurredNormals = InitGraph.RegisterExternalBuffer(BlurredNormalsBuf);
				FRDGBufferRef RDG_BlurredTargets = InitGraph.RegisterExternalBuffer(BlurredTargetsBuf);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Positions, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Normals, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Counter, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_HashSlots, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_HashIndices, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_NormalSums, PF_R32_SINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_NormalCounts, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetPositions, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetOffsetSums, PF_R32_SINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_TargetWeightSums, PF_R32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_Cells, PF_R32G32B32A32_UINT)), 0u);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_BlurredNormals, PF_A32B32G32R32F)), 0.0f);
				AddClearUAVPass(InitGraph, InitGraph.CreateUAV(FRDGBufferUAVDesc(RDG_BlurredTargets, PF_A32B32G32R32F)), 0.0f);
				InitGraph.Execute();
			}

			const int32 BatchTriangleCap = FMath::Min(SafeMaxTriangles, 500000);
			const FCSTriangleMeshData* InitialTriangleData = GetTriangleMeshDataTriangleCount(LandscapeTriangleData) > 0 ? &LandscapeTriangleData : nullptr;

			TArray<TArray<FResolvedStaticMeshTriangleRequest>> Batches;
			{
				TArray<FResolvedStaticMeshTriangleRequest> CurrentBatch;
				int64 CurrentBatchTriCount = 0;
				for (int32 i = 0; i < ResolvedRequests.Num(); ++i)
				{
					if (CurrentBatchTriCount + ResolvedRequests[i].TriangleCount > int64(BatchTriangleCap) && CurrentBatch.Num() > 0)
					{
						Batches.Add(MoveTemp(CurrentBatch));
						CurrentBatch.Reset();
						CurrentBatchTriCount = 0;
					}
					CurrentBatch.Add(ResolvedRequests[i]);
					CurrentBatchTriCount += ResolvedRequests[i].TriangleCount;
				}
				if (CurrentBatch.Num() > 0)
				{
					Batches.Add(MoveTemp(CurrentBatch));
				}
			}

			TShaderMapRef<FTriangleSurfaceVoxelsCS> VoxelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

			for (int32 BatchIdx = 0; BatchIdx < Batches.Num(); ++BatchIdx)
			{
				const TArray<FResolvedStaticMeshTriangleRequest>& Batch = Batches[BatchIdx];
				uint64 BatchTriCount = 0;
				for (const auto& Req : Batch) BatchTriCount += Req.TriangleCount;

				FRDGBuilder BatchGraph(RHICmdList);

				const FCSTriangleMeshData* BatchInitData = (BatchIdx == 0) ? InitialTriangleData : nullptr;
				FCSStaticMeshTriangleRDGOutput TriOut = AddResolvedStaticMeshTrianglesToRDGInternal(
					BatchGraph, RHICmdList, Batch, BatchTriCount, ReferencePointsForRender, SafeFilterDistance,
					BatchTriangleCap, BatchInitData, TEXT("CS.FilteredSV.Batch.Tri"));

				if (TriOut.TriangleVertices && TriOut.TriangleNormals && TriOut.TriangleCounter && TriOut.MaxTriangles > 0)
				{
					FRDGBufferRef ExtPositions = BatchGraph.RegisterExternalBuffer(VoxelPositionsBuf);
					FRDGBufferRef ExtNormals = BatchGraph.RegisterExternalBuffer(VoxelNormalsBuf);
					FRDGBufferRef ExtCounter = BatchGraph.RegisterExternalBuffer(VoxelCounterBuf);
					FRDGBufferRef ExtHashSlots = BatchGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
					FRDGBufferRef ExtHashIndices = BatchGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
					FRDGBufferRef ExtNormalSums = BatchGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
					FRDGBufferRef ExtNormalCounts = BatchGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
					FRDGBufferRef ExtTargetPositions = BatchGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
					FRDGBufferRef ExtTargetOffsetSums = BatchGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
					FRDGBufferRef ExtTargetWeightSums = BatchGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
					FRDGBufferRef ExtCells = BatchGraph.RegisterExternalBuffer(VoxelCellsBuf);

					FRDGBufferSRVRef TriVertsSRV = TriOut.TriangleVerticesSRV ? TriOut.TriangleVerticesSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleVertices, PF_A32B32G32R32F));
					FRDGBufferSRVRef TriNormsSRV = TriOut.TriangleNormalsSRV ? TriOut.TriangleNormalsSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleNormals, PF_A32B32G32R32F));
					FRDGBufferSRVRef TriCounterSRV = TriOut.TriangleCounterSRV ? TriOut.TriangleCounterSRV : BatchGraph.CreateSRV(FRDGBufferSRVDesc(TriOut.TriangleCounter, PF_R32_UINT));

					FTriangleSurfaceVoxelsCS::FParameters* VP = BatchGraph.AllocParameters<FTriangleSurfaceVoxelsCS::FParameters>();
					VP->TriangleVertices = TriVertsSRV;
					VP->TriangleNormals = TriNormsSRV;
					VP->SurfaceTriangleCounter = TriCounterSRV;
					VP->RW_OutVoxelPositions = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtPositions, PF_A32B32G32R32F));
					VP->RW_OutVoxelNormals = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormals, PF_A32B32G32R32F));
					VP->RW_SurfaceVoxelCounter = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtCounter, PF_R32_UINT));
					VP->RW_SurfaceVoxelHashSlots = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtHashSlots, PF_R32_UINT));
					VP->RW_SurfaceVoxelHashIndices = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtHashIndices, PF_R32_UINT));
					VP->RW_SurfaceVoxelNormalSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormalSums, PF_R32_SINT));
					VP->RW_SurfaceVoxelNormalCounts = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtNormalCounts, PF_R32_UINT));
					VP->RW_OutVoxelTargetPositions = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetPositions, PF_A32B32G32R32F));
					VP->RW_SurfaceVoxelTargetOffsetSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetOffsetSums, PF_R32_SINT));
					VP->RW_SurfaceVoxelTargetWeightSums = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtTargetWeightSums, PF_R32_UINT));
					VP->RW_OutVoxelCells = BatchGraph.CreateUAV(FRDGBufferUAVDesc(ExtCells, PF_R32G32B32A32_UINT));
					VP->SurfaceVoxelOrigin = FVector3f(VoxelOrigin);
					VP->SurfaceVoxelSize = SafeVoxelSize;
					VP->SurfaceThickness = SafeSurfaceThickness;
					VP->SurfaceTriangleCount = TriOut.MaxTriangles;
					VP->SurfaceVoxelCapacity = VoxCap;
					VP->SurfaceVoxelHashSlotCount = HashSlotCount;

					BatchGraph.AddPass(
						RDG_EVENT_NAME("CS.FilteredSV.Batch%d.Voxelize", BatchIdx),
						VP, ERDGPassFlags::Compute,
						[VP, VoxelShader, TriCap = TriOut.MaxTriangles](FRHIComputeCommandList& Cmd)
						{
							FComputeShaderUtils::Dispatch(Cmd, VoxelShader, *VP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(TriCap), 1, 1), 64));
						});
				}
				BatchGraph.Execute();
			}

			{
				FRDGBuilder FinalGraph(RHICmdList);

				FRDGBufferRef FPositions = FinalGraph.RegisterExternalBuffer(VoxelPositionsBuf);
				FRDGBufferRef FNormals = FinalGraph.RegisterExternalBuffer(VoxelNormalsBuf);
				FRDGBufferRef FCounter = FinalGraph.RegisterExternalBuffer(VoxelCounterBuf);
				FRDGBufferRef FNormalSums = FinalGraph.RegisterExternalBuffer(VoxelNormalSumsBuf);
				FRDGBufferRef FNormalCounts = FinalGraph.RegisterExternalBuffer(VoxelNormalCountsBuf);
				FRDGBufferRef FTargetPositions = FinalGraph.RegisterExternalBuffer(VoxelTargetPositionsBuf);
				FRDGBufferRef FTargetOffsetSums = FinalGraph.RegisterExternalBuffer(VoxelTargetOffsetSumsBuf);
				FRDGBufferRef FTargetWeightSums = FinalGraph.RegisterExternalBuffer(VoxelTargetWeightSumsBuf);
				FRDGBufferRef FCells = FinalGraph.RegisterExternalBuffer(VoxelCellsBuf);
				FRDGBufferRef FBlurNormals = FinalGraph.RegisterExternalBuffer(BlurredNormalsBuf);
				FRDGBufferRef FBlurTargets = FinalGraph.RegisterExternalBuffer(BlurredTargetsBuf);

				FRDGBufferUAVRef PositionsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FPositions, PF_A32B32G32R32F));
				FRDGBufferUAVRef NormalsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FNormals, PF_A32B32G32R32F));
				FRDGBufferUAVRef TargetPositionsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetPositions, PF_A32B32G32R32F));
				FRDGBufferSRVRef NormalSumsSRV = FinalGraph.CreateSRV(FRDGBufferSRVDesc(FNormalSums, PF_R32_SINT));
				FRDGBufferSRVRef NormalCountsSRV = FinalGraph.CreateSRV(FRDGBufferSRVDesc(FNormalCounts, PF_R32_UINT));
				FRDGBufferUAVRef TargetOffsetSumsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetOffsetSums, PF_R32_SINT));
				FRDGBufferUAVRef TargetWeightSumsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FTargetWeightSums, PF_R32_UINT));

				TShaderMapRef<FFinalizeSurfaceVoxelNormalsCS> FinalizeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
				FFinalizeSurfaceVoxelNormalsCS::FParameters* FP = FinalGraph.AllocParameters<FFinalizeSurfaceVoxelNormalsCS::FParameters>();
				FP->SurfaceVoxelNormalSums = NormalSumsSRV;
				FP->SurfaceVoxelNormalCounts = NormalCountsSRV;
				FP->RW_SurfaceVoxelTargetOffsetSums = TargetOffsetSumsUAV;
				FP->RW_SurfaceVoxelTargetWeightSums = TargetWeightSumsUAV;
				FP->RW_OutVoxelPositions = PositionsUAV;
				FP->RW_OutVoxelNormals = NormalsUAV;
				FP->RW_OutVoxelTargetPositions = TargetPositionsUAV;
				FP->SurfaceVoxelCapacity = VoxCap;
				FP->SurfaceVoxelSize = SafeVoxelSize;
				FinalGraph.AddPass(
					RDG_EVENT_NAME("CS.FilteredSV.FinalizeNormals"), FP, ERDGPassFlags::Compute,
					[FP, FinalizeShader, VoxCap](FRHIComputeCommandList& Cmd)
					{
						FComputeShaderUtils::Dispatch(Cmd, FinalizeShader, *FP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
					});

				const uint32 BlurIters = uint32(FMath::Max(0, SafeSurfaceVoxelBlurIterations));
				FRDGBufferUAVRef BlurNormalsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FBlurNormals, PF_A32B32G32R32F));
				FRDGBufferUAVRef BlurTargetsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FBlurTargets, PF_A32B32G32R32F));
				FRDGBufferUAVRef CounterUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FCounter, PF_R32_UINT));
				FRDGBufferUAVRef CellsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FCells, PF_R32G32B32A32_UINT));
				FRDGBufferRef FHashSlots = FinalGraph.RegisterExternalBuffer(VoxelHashSlotsBuf);
				FRDGBufferRef FHashIndices = FinalGraph.RegisterExternalBuffer(VoxelHashIndicesBuf);
				FRDGBufferUAVRef HashSlotsUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FHashSlots, PF_R32_UINT));
				FRDGBufferUAVRef HashIndicesUAV = FinalGraph.CreateUAV(FRDGBufferUAVDesc(FHashIndices, PF_R32_UINT));

				if (BlurIters > 0u)
				{
					const uint32 BlurRadius = uint32(FMath::Max(1, SafeSurfaceVoxelBlurRadius));
					int32 CurrentNormalsIdx = 0;
					int32 CurrentTargetsIdx = 0;
					TShaderMapRef<FBlurSurfaceVoxelsCS> BlurShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					for (uint32 Iter = 0u; Iter < BlurIters; ++Iter)
					{
						const bool bReadFromOriginal = (CurrentNormalsIdx == 0);
						FRDGBufferUAVRef SrcN = bReadFromOriginal ? NormalsUAV : BlurNormalsUAV;
						FRDGBufferUAVRef SrcT = bReadFromOriginal ? TargetPositionsUAV : BlurTargetsUAV;
						FRDGBufferUAVRef DstN = bReadFromOriginal ? BlurNormalsUAV : NormalsUAV;
						FRDGBufferUAVRef DstT = bReadFromOriginal ? BlurTargetsUAV : TargetPositionsUAV;
						FBlurSurfaceVoxelsCS::FParameters* BP = FinalGraph.AllocParameters<FBlurSurfaceVoxelsCS::FParameters>();
						BP->RW_SurfaceVoxelCounter = CounterUAV;
					BP->RW_OutVoxelNormals = SrcN;
					BP->RW_BlurredVoxelNormals = DstN;
					BP->RW_OutVoxelTargetPositions = SrcT;
					BP->RW_BlurredVoxelTargetPositions = DstT;
					BP->RW_OutVoxelCells = CellsUAV;
					BP->RW_SurfaceVoxelHashSlots = HashSlotsUAV;
					BP->RW_SurfaceVoxelHashIndices = HashIndicesUAV;
					BP->SurfaceVoxelCapacity = VoxCap;
					BP->SurfaceVoxelHashSlotCount = HashSlotCount;
					BP->SurfaceVoxelBlurRadius = BlurRadius;
						FinalGraph.AddPass(
							RDG_EVENT_NAME("CS.FilteredSV.Blur%d", Iter), BP, ERDGPassFlags::Compute,
							[BP, BlurShader, VoxCap](FRHIComputeCommandList& Cmd)
							{
								FComputeShaderUtils::Dispatch(Cmd, BlurShader, *BP, FComputeShaderUtils::GetGroupCount(FIntVector(int32(VoxCap), 1, 1), 64));
							});
						CurrentNormalsIdx = 1 - CurrentNormalsIdx;
						CurrentTargetsIdx = 1 - CurrentTargetsIdx;
					}
					if (CurrentNormalsIdx != 0) { NormalsUAV = BlurNormalsUAV; }
					if (CurrentTargetsIdx != 0) { TargetPositionsUAV = BlurTargetsUAV; }
				}

				VoxelCapacity = int32(VoxCap);
				ActualVoxelReadbackBytes = uint32(uint64(VoxCap) * sizeof(FVector4f));
				ActualVoxelCellReadbackBytes = uint32(uint64(VoxCap) * sizeof(FIntVector4));
				const bool bFinalDataInBlurBuffers = BlurIters > 0u && (BlurIters & 1u) != 0u;
				FRDGBufferRef FinalNormals = bFinalDataInBlurBuffers ? FBlurNormals : FNormals;
				FRDGBufferRef FinalTargets = bFinalDataInBlurBuffers ? FBlurTargets : FTargetPositions;
				if (bReadbackToCPU)
				{
					AddEnqueueCopyPass(FinalGraph, PositionReadback, FPositions, ActualVoxelReadbackBytes);
					AddEnqueueCopyPass(FinalGraph, NormalReadback, FinalNormals, ActualVoxelReadbackBytes);
					AddEnqueueCopyPass(FinalGraph, TargetPositionReadback, FinalTargets, ActualVoxelReadbackBytes);
					AddEnqueueCopyPass(FinalGraph, CellReadback, FCells, ActualVoxelCellReadbackBytes);
					AddEnqueueCopyPass(FinalGraph, CounterReadback, FCounter, CounterReadbackBytes);
				}
				bHasGPUOutput = true;

				// Trip B: keep the same buffers the readback sources (base pooled buffers,
				// which hold the final blur-resolved data), so a consumer can register them.
				if (OutGPUVoxels)
				{
					OutGPUVoxels->Positions = VoxelPositionsBuf;
					OutGPUVoxels->Normals = bFinalDataInBlurBuffers ? BlurredNormalsBuf : VoxelNormalsBuf;
					OutGPUVoxels->TargetPositions = bFinalDataInBlurBuffers ? BlurredTargetsBuf : VoxelTargetPositionsBuf;
					OutGPUVoxels->Cells = VoxelCellsBuf;
					OutGPUVoxels->Counter = VoxelCounterBuf;
					OutGPUVoxels->HashSlots = VoxelHashSlotsBuf;
					OutGPUVoxels->HashIndices = VoxelHashIndicesBuf;
					OutGPUVoxels->VoxelCapacity = int32(VoxCap);
					OutGPUVoxels->HashSlotCount = HashSlotCount;
					OutGPUVoxels->VoxelSize = SafeVoxelSize;
					OutGPUVoxels->VoxelOrigin = FVector(VoxelOrigin);
					OutGPUVoxels->WorldBounds = VoxelWorldBounds;
				}

				FinalGraph.Execute();
			}

			bRenderWorkQueued = true;
		});

	UCSMesh::CountedBlockingFlush();

	if (!bRenderWorkQueued || !bHasGPUOutput)
	{
		delete PositionReadback;
		delete NormalReadback;
		delete TargetPositionReadback;
		delete CellReadback;
		delete CounterReadback;
		return;
	}

	if (!bReadbackToCPU) return;

	TArray<FVector4f> PositionData;
	TArray<FVector4f> NormalData;
	TArray<FVector4f> TargetPositionData;
	TArray<FIntVector4> CellData;
	PositionData.SetNumZeroed(VoxelCapacity);
	NormalData.SetNumZeroed(VoxelCapacity);
	TargetPositionData.SetNumZeroed(VoxelCapacity);
	CellData.SetNumZeroed(VoxelCapacity);
	uint32 VoxelCount = 0;
	uint32 DroppedVoxelCount = 0;
	bool bReadbackSucceeded = false;

	ENQUEUE_RENDER_COMMAND(GetBoxSceneFilteredSurfaceVoxelsReadback)(
		[PositionReadback, NormalReadback, TargetPositionReadback, CellReadback, CounterReadback, ActualVoxelReadbackBytes, ActualVoxelCellReadbackBytes, CounterReadbackBytes,
		 &PositionData, &NormalData, &TargetPositionData, &CellData, &VoxelCount, &DroppedVoxelCount, &bReadbackSucceeded](FRHICommandListImmediate& RHICmdList)
		{
			uint32 LocalCounters[2] = { 0, 0 };
			const FCSMeshGenReadbackSpec Specs[] = {
				{ PositionReadback, PositionData.GetData(), ActualVoxelReadbackBytes },
				{ NormalReadback, NormalData.GetData(), ActualVoxelReadbackBytes },
				{ TargetPositionReadback, TargetPositionData.GetData(), ActualVoxelReadbackBytes },
				{ CellReadback, CellData.GetData(), ActualVoxelCellReadbackBytes },
				{ CounterReadback, LocalCounters, CounterReadbackBytes },
			};
			bReadbackSucceeded = CSMeshGen_DrainReadbacks(RHICmdList, Specs, TEXT("GetBoxSceneFilteredSurfaceVoxels"));
			VoxelCount = LocalCounters[0];
			DroppedVoxelCount = LocalCounters[1];
		});

	UCSMesh::CountedBlockingFlush();

	if (!bReadbackSucceeded)
	{
		return;
	}

	if (DroppedVoxelCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GetBoxSceneFilteredSurfaceVoxels] Voxel buffer overflow: %u cells dropped (capacity MaxVoxels=%d). Increase MaxVoxels or VoxelSize."),
			DroppedVoxelCount, SafeMaxVoxels);
	}

	const int32 EffectiveVoxelCount = FMath::Clamp<int32>(int32(VoxelCount), 0, PositionData.Num());
	if (EffectiveVoxelCount <= 0)
	{
		return;
	}

	OutPositions.Reserve(EffectiveVoxelCount);
	OutNormals.Reserve(EffectiveVoxelCount);
	FCSSurfaceVoxelData FilteredVoxelData;
	FilteredVoxelData.Positions.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.Normals.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.TargetPositions.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.Cells.Reserve(EffectiveVoxelCount);
	FilteredVoxelData.VoxelSize = SafeVoxelSize;
	FilteredVoxelData.VoxelOrigin = QueryBox.Min;
	int32 InvalidPositionCount = 0;
	int32 InvalidNormalCount = 0;
	int32 InvalidTargetCount = 0;
	int32 InvalidTargetVectorCount = 0;
	int32 InvalidTargetWCount = 0;
	int32 FarTargetCount = 0;
	const double MaxTargetDistanceSq = FMath::Square(double(FMath::Max(SafeVoxelSize * 2.0f, UE_KINDA_SMALL_NUMBER)));
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector4f& Position = PositionData[VoxelIndex];
		const FVector4f& Normal = NormalData[VoxelIndex];
		const FVector4f& TargetPosition = TargetPositionData[VoxelIndex];
		const FIntVector4& Cell = CellData[VoxelIndex];

		if (!IsFiniteCSVector4(Position) || Position.W <= 0.0f)
		{
			++InvalidPositionCount;
			continue;
		}

		const FVector VoxelCenter(Position.X, Position.Y, Position.Z);
		FVector SafeNormal(Normal.X, Normal.Y, Normal.Z);
		if (!IsFiniteCSVector4(Normal) || !SafeNormal.Normalize())
		{
			SafeNormal = FVector::UpVector;
			++InvalidNormalCount;
		}

		FVector SafeTarget(TargetPosition.X, TargetPosition.Y, TargetPosition.Z);
		const bool bFiniteTarget = IsFiniteCSVector4(TargetPosition);
		const bool bPositiveTargetW = TargetPosition.W > 0.0f;
		const bool bTargetWithinVoxel = bFiniteTarget
			&& FVector::DistSquared(SafeTarget, VoxelCenter) <= MaxTargetDistanceSq;
		const bool bValidTarget = bFiniteTarget && bPositiveTargetW && bTargetWithinVoxel;
		if (!bValidTarget)
		{
			if (!bFiniteTarget)
			{
				++InvalidTargetVectorCount;
			}
			else if (!bPositiveTargetW)
			{
				++InvalidTargetWCount;
			}
			else
			{
				++FarTargetCount;
			}
			SafeTarget = VoxelCenter;
			++InvalidTargetCount;
		}

		FilteredVoxelData.Positions.Add(VoxelCenter);
		FilteredVoxelData.Normals.Add(SafeNormal);
		FilteredVoxelData.TargetPositions.Add(SafeTarget);
		FilteredVoxelData.Cells.Add(FIntVector(Cell.X, Cell.Y, Cell.Z));
		OutPositions.Add(VoxelCenter);
		OutNormals.Add(SafeNormal);
	}
	FilteredVoxelData.VoxelCount = FilteredVoxelData.Positions.Num();
	if (InvalidPositionCount > 0 || InvalidNormalCount > 0 || InvalidTargetCount > 0)
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[GetBoxSceneFilteredSurfaceVoxels] Sanitized surface voxel readback. Input=%d Output=%d InvalidPositions=%d InvalidNormals=%d InvalidTargets=%d InvalidTargetVectors=%d InvalidTargetW=%d FarTargets=%d"),
			EffectiveVoxelCount,
			FilteredVoxelData.VoxelCount,
			InvalidPositionCount,
			InvalidNormalCount,
			InvalidTargetCount,
			InvalidTargetVectorCount,
			InvalidTargetWCount,
			FarTargetCount);
	}

	// Update cached data
	LastSurfaceVoxelData = FilteredVoxelData;
	StoreSurfaceVoxelTextureData(LastSurfaceVoxelData, QueryBox.Min);

	// Trip B: finalize the retained GPU voxel buffers with the same counts/params as the
	// CPU cache (the buffers themselves were stored on the render thread above). VoxelCount
	// comes from the tiny counter readback; the big per-voxel arrays no longer need reading
	// back once a consumer registers these buffers directly.
	LastSurfaceVoxelGPUBuffers.VoxelCount = LastSurfaceVoxelData.VoxelCount > 0
		? LastSurfaceVoxelData.VoxelCount : LastSurfaceVoxelData.Positions.Num();
	LastSurfaceVoxelGPUBuffers.VoxelSize = LastSurfaceVoxelData.VoxelSize;
	LastSurfaceVoxelGPUBuffers.VoxelOrigin = LastSurfaceVoxelData.VoxelOrigin;
}

FCSSurfaceVoxelData AComputeShaderMeshGenerator::ReadbackBoxSceneSurfaceVoxelsSync(float VoxelSize)
{
	TArray<FVector> UnusedPositions;
	TArray<FVector> UnusedNormals;
	GetBoxSceneFilteredSurfaceVoxels(VoxelSize, 0.0f, UnusedPositions, UnusedNormals);
	return LastSurfaceVoxelData;
}

// -----------------------------------------------------------------------------
// Core System - Generated Data Cache
// -----------------------------------------------------------------------------

FCSMeshGeneratorTriangleTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneTriangleTextureData(float ReferenceFilterDistance)
{
	GetBoxSceneTrianglesFromGPUFiltered(ReferenceFilterDistance);
	return LastTriangleTextureData;
}

FCSMeshGeneratorSurfaceVoxelTextureDataHandle AComputeShaderMeshGenerator::UpdateBoxSceneSurfaceVoxelTextureData(float VoxelSize)
{
	ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);
	return LastSurfaceVoxelTextureData;
}

void AComputeShaderMeshGenerator::ClearGeneratedDataTextureCache()
{
	ClearTriangleTextureData();
	ClearSurfaceVoxelTextureData();
}

// 两个 DrawDebug*VoxelDirections 入口的共同体：两个 Options USTRUCT（Last/Box）字段同名同义，
// 仅入口守卫不同（缓存有效性 vs 现场准备），故用 duck-typed 模板吃掉公共部分。
//
// Options.bDrawPoints / PointColor 不再有对应物：以前"点 + 方向线"是两次绘制，现在一个箭头
// 同时表达位置和方向（见 CSPointArrowMesh.usf 的文件头）。字段留在 USTRUCT 里不动，免得打断
// 已有蓝图连线，但它们不再影响画面。
int32 AComputeShaderMeshGenerator::ShowDebugPointArrows(
	const FCSGpuDebugPooledSource& Source, float ArrowLength, FLinearColor ArrowColor,
	int32 MaxArrows, float Lifetime)
{
	if (!DisplayComponent || !Source.IsValid())
	{
		ClearDebugDisplay();
		return 0;
	}

	FCSPointArrowBuildParams Params;
	Params.Source = Source;
	Params.MaxArrows = MaxArrows;
	Params.ArrowLength = ArrowLength > 0.0f ? ArrowLength : FMath::Max(Source.ItemSize, UE_KINDA_SMALL_NUMBER);
	Params.ArrowColor = ArrowColor;

	if (!DebugDisplayMesh) DebugDisplayMesh = NewObject<UCSMesh>(this);
	if (!BuildPointArrowGeometryIntoMesh(DebugDisplayMesh, Params))
	{
		ClearDebugDisplay();
		return 0;
	}

	DisplayComponent->SetGpuMesh(DebugDisplayMesh);
	ScheduleDebugDisplayClear(Lifetime);

	// 报告的是"最多会画几个"，与代理时代同口径：真正画几个由 GPU 计数决定，问准它要一次 stall。
	return MaxArrows > 0 ? FMath::Min(MaxArrows, Source.Capacity) : Source.Capacity;
}

template <typename TOptions>
int32 AComputeShaderMeshGenerator::DrawVoxelDirectionArrows(const TOptions& Options)
{
	FCSGpuDebugPooledSource Source;
	Source.Positions = LastSurfaceVoxelGPUBuffers.Positions;
	Source.Normals = LastSurfaceVoxelGPUBuffers.Normals;
	Source.Counter = LastSurfaceVoxelGPUBuffers.Counter;
	Source.Capacity = LastSurfaceVoxelGPUBuffers.VoxelCapacity;
	Source.ItemSize = LastSurfaceVoxelGPUBuffers.VoxelSize;
	Source.WorldBounds = LastSurfaceVoxelGPUBuffers.WorldBounds;

	// (Duration, bPersistentLines) 的语义与改动前逐字一致：常驻 => 不排清除；Duration <= 0 =>
	// 下一帧清除（一帧可视）。
	return ShowDebugPointArrows(Source, Options.DirectionLength, Options.DirectionColor,
		Options.MaxDirectionsToDraw, Options.bPersistentLines ? -1.0f : Options.Duration);
}

void AComputeShaderMeshGenerator::ClearDebugDisplay()
{
	if (UWorld* World = GetWorld()) World->GetTimerManager().ClearTimer(DebugDisplayClearHandle);
	if (DisplayComponent) DisplayComponent->SetGpuMesh(nullptr);
}

void AComputeShaderMeshGenerator::ScheduleDebugDisplayClear(float Lifetime)
{
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().ClearTimer(DebugDisplayClearHandle);
	if (Lifetime < 0.0f) return; // 常驻
	if (Lifetime == 0.0f)
	{
		DebugDisplayClearHandle = World->GetTimerManager().SetTimerForNextTick(
			this, &AComputeShaderMeshGenerator::ClearDebugDisplay);
		return;
	}
	World->GetTimerManager().SetTimer(DebugDisplayClearHandle, this,
		&AComputeShaderMeshGenerator::ClearDebugDisplay, Lifetime, false);
}

int32 AComputeShaderMeshGenerator::DrawDebugLastSurfaceVoxelDirections(
	const FCSDebugLastVoxelDirectionOptions& Options)
{
	if (!DisplayComponent || !LastSurfaceVoxelGPUBuffers.IsValid()) return 0;
	return DrawVoxelDirectionArrows(Options);
}

int32 AComputeShaderMeshGenerator::DrawDebugBoxSceneSurfaceVoxelDirections(
	const FCSDebugBoxVoxelDirectionOptions& Options)
{
	if (!DisplayComponent || !PrepareBoxSceneSurfaceVoxelsGPU(Options.VoxelSize)) return 0;
	return DrawVoxelDirectionArrows(Options);
}

UTextureRenderTarget2D* AComputeShaderMeshGenerator::GetOrCreateGeneratedDataRenderTarget(
	TObjectPtr<UTextureRenderTarget2D>& RenderTarget,
	const TCHAR* BaseName,
	int32 Width,
	int32 Height)
{
	const int32 SafeWidth = FMath::Max(CSGeneratorMinTextureDimension, Width);
	const int32 SafeHeight = FMath::Max(CSGeneratorMinTextureDimension, Height);
	const bool bNeedsCreate = !RenderTarget
		|| RenderTarget->SizeX != SafeWidth
		|| RenderTarget->SizeY != SafeHeight
		|| RenderTarget->RenderTargetFormat != RTF_RGBA32f
		|| !RenderTarget->bCanCreateUAV;

	if (!bNeedsCreate)
	{
		return RenderTarget.Get();
	}

	if (RenderTarget)
	{
		RenderTarget->ReleaseResource();
		RenderTarget = nullptr;
	}

	RenderTarget = NewObject<UTextureRenderTarget2D>(
		this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), BaseName),
		RF_Transient);
	if (!RenderTarget)
	{
		return nullptr;
	}

	RenderTarget->RenderTargetFormat = RTF_RGBA32f;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->bCanCreateUAV = true;
	RenderTarget->InitAutoFormat(SafeWidth, SafeHeight);
	RenderTarget->UpdateResourceImmediate(true);
	return RenderTarget.Get();
}

void AComputeShaderMeshGenerator::StoreTriangleTextureData(const FCSTriangleMeshData& TriangleData, float ReferenceFilterDistance, FBox SourceWorldBounds)
{
	const int32 EffectiveVertexCount = GetEffectiveVertexCount(TriangleData);
	const int32 EffectiveIndexCount = GetEffectiveIndexCount(TriangleData);
	const int32 TriangleCount = EffectiveIndexCount >= 3
		? EffectiveIndexCount / 3
		: EffectiveVertexCount / 3;
	if (EffectiveVertexCount <= 0 || TriangleCount <= 0)
	{
		ClearTriangleTextureData();
		return;
	}

	const int32 MaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);
	const FIntPoint VertexTextureSize = GetLinearDataTextureSize(EffectiveVertexCount, MaxTextureDimension);
	UTextureRenderTarget2D* VertexRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleVertexRT,
		TEXT("CSMeshGenerator_LastTriangleVertexRT"),
		VertexTextureSize.X,
		VertexTextureSize.Y);
	UTextureRenderTarget2D* NormalRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleNormalRT,
		TEXT("CSMeshGenerator_LastTriangleNormalRT"),
		VertexTextureSize.X,
		VertexTextureSize.Y);
	UTextureRenderTarget2D* MetaRT = GetOrCreateGeneratedDataRenderTarget(
		LastTriangleTextureData.TriangleMetaRT,
		TEXT("CSMeshGenerator_LastTriangleMetaRT"),
		8,
		1);
	if (!VertexRT || !NormalRT || !MetaRT)
	{
		ClearTriangleTextureData();
		return;
	}

	const int32 TexturePixelCount = VertexTextureSize.X * VertexTextureSize.Y;
	TArray<FLinearColor> VertexPixels;
	VertexPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> NormalPixels;
	NormalPixels.SetNumZeroed(TexturePixelCount);

	for (int32 VertexIndex = 0; VertexIndex < EffectiveVertexCount; ++VertexIndex)
	{
		const FVector& Vertex = TriangleData.Vertices[VertexIndex];
		VertexPixels[VertexIndex] = FLinearColor(float(Vertex.X), float(Vertex.Y), float(Vertex.Z), 1.0f);

		FVector Normal = TriangleData.VertexNormals.IsValidIndex(VertexIndex)
			? TriangleData.VertexNormals[VertexIndex]
			: FVector::ZeroVector;
		if (!Normal.Normalize())
		{
			const int32 TriVertexBase = (VertexIndex / 3) * 3;
			if (TriangleData.Vertices.IsValidIndex(TriVertexBase + 2))
			{
				Normal = FVector::CrossProduct(
					TriangleData.Vertices[TriVertexBase + 1] - TriangleData.Vertices[TriVertexBase + 0],
					TriangleData.Vertices[TriVertexBase + 2] - TriangleData.Vertices[TriVertexBase + 0]).GetSafeNormal();
			}
		}
		NormalPixels[VertexIndex] = FLinearColor(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f);
	}

	const FBox SourceBounds = SourceWorldBounds.IsValid ? SourceWorldBounds : GetGeneratorBoundsWorldBox();
	TArray<FLinearColor> MetaPixels;
	MetaPixels.SetNumZeroed(8);
	MetaPixels[0] = FLinearColor(float(TriangleCount), float(EffectiveVertexCount), float(EffectiveIndexCount), ReferenceFilterDistance);
	MetaPixels[1] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Min.X), float(SourceBounds.Min.Y), float(SourceBounds.Min.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[2] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Max.X), float(SourceBounds.Max.Y), float(SourceBounds.Max.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[3] = FLinearColor(float(VertexTextureSize.X), float(VertexTextureSize.Y), float(MaxTextureDimension), 0.0f);

	UploadLinearColorsToRenderTarget(VertexRT, MoveTemp(VertexPixels));
	UploadLinearColorsToRenderTarget(NormalRT, MoveTemp(NormalPixels));
	UploadLinearColorsToRenderTarget(MetaRT, MoveTemp(MetaPixels));

	LastTriangleTextureData.bValid = true;
	LastTriangleTextureData.VertexCount = EffectiveVertexCount;
	LastTriangleTextureData.TriangleCount = TriangleCount;
	LastTriangleTextureData.IndexCount = EffectiveIndexCount;
	LastTriangleTextureData.ReferenceFilterDistance = ReferenceFilterDistance;
	LastTriangleTextureData.SourceWorldBounds = SourceBounds;
}

void AComputeShaderMeshGenerator::StoreSurfaceVoxelTextureData(const FCSSurfaceVoxelData& SurfaceVoxelData, FVector VoxelOrigin)
{
	const int32 EffectiveVoxelCount = SurfaceVoxelData.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxelData.VoxelCount, 0, SurfaceVoxelData.Positions.Num())
		: SurfaceVoxelData.Positions.Num();
	if (EffectiveVoxelCount <= 0)
	{
		ClearSurfaceVoxelTextureData();
		LastSurfaceVoxelData.VoxelSize = SurfaceVoxelData.VoxelSize;
		LastSurfaceVoxelTextureData.VoxelSize = SurfaceVoxelData.VoxelSize;
		return;
	}

	const int32 MaxTextureDimension = FMath::Max(CSGeneratorMinTextureDimension, VoxelGridSettings.MaxCacheTextureDimension);
	const FIntPoint VoxelTextureSize = GetLinearDataTextureSize(EffectiveVoxelCount, MaxTextureDimension);
	UTextureRenderTarget2D* PositionRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelPositionRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelPositionRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* NormalRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelNormalRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelNormalRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* TargetRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelTargetRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelTargetRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* CellRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelCellRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelCellRT"),
		VoxelTextureSize.X,
		VoxelTextureSize.Y);
	UTextureRenderTarget2D* MetaRT = GetOrCreateGeneratedDataRenderTarget(
		LastSurfaceVoxelTextureData.VoxelMetaRT,
		TEXT("CSMeshGenerator_LastSurfaceVoxelMetaRT"),
		8,
		1);
	if (!PositionRT || !NormalRT || !TargetRT || !CellRT || !MetaRT)
	{
		ClearSurfaceVoxelTextureData();
		return;
	}

	const int32 TexturePixelCount = VoxelTextureSize.X * VoxelTextureSize.Y;
	TArray<FLinearColor> PositionPixels;
	PositionPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> NormalPixels;
	NormalPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> TargetPixels;
	TargetPixels.SetNumZeroed(TexturePixelCount);
	TArray<FLinearColor> CellPixels;
	CellPixels.SetNumZeroed(TexturePixelCount);
	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector& Position = SurfaceVoxelData.Positions[VoxelIndex];
		PositionPixels[VoxelIndex] = FLinearColor(float(Position.X), float(Position.Y), float(Position.Z), 1.0f);

		FVector Normal = SurfaceVoxelData.Normals.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.Normals[VoxelIndex]
			: FVector::UpVector;
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
		NormalPixels[VoxelIndex] = FLinearColor(float(Normal.X), float(Normal.Y), float(Normal.Z), 0.0f);

		FVector Target = SurfaceVoxelData.TargetPositions.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.TargetPositions[VoxelIndex]
			: Position;
		if (!IsFiniteVector(Target))
		{
			Target = Position;
		}
		TargetPixels[VoxelIndex] = FLinearColor(float(Target.X), float(Target.Y), float(Target.Z), 1.0f);

		FIntVector Cell = SurfaceVoxelData.Cells.IsValidIndex(VoxelIndex)
			? SurfaceVoxelData.Cells[VoxelIndex]
			: FIntVector(
				FMath::FloorToInt((Position.X - VoxelOrigin.X) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)),
				FMath::FloorToInt((Position.Y - VoxelOrigin.Y) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)),
				FMath::FloorToInt((Position.Z - VoxelOrigin.Z) / FMath::Max(SurfaceVoxelData.VoxelSize, UE_KINDA_SMALL_NUMBER)));
		CellPixels[VoxelIndex] = FLinearColor(float(Cell.X), float(Cell.Y), float(Cell.Z), 0.0f);
	}

	const FBox SourceBounds = GetGeneratorBoundsWorldBox();
	TArray<FLinearColor> MetaPixels;
	MetaPixels.SetNumZeroed(8);
	MetaPixels[0] = FLinearColor(float(EffectiveVoxelCount), SurfaceVoxelData.VoxelSize, float(VoxelTextureSize.X), float(VoxelTextureSize.Y));
	MetaPixels[1] = FLinearColor(float(VoxelOrigin.X), float(VoxelOrigin.Y), float(VoxelOrigin.Z), 1.0f);
	MetaPixels[2] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Min.X), float(SourceBounds.Min.Y), float(SourceBounds.Min.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[3] = SourceBounds.IsValid
		? FLinearColor(float(SourceBounds.Max.X), float(SourceBounds.Max.Y), float(SourceBounds.Max.Z), 1.0f)
		: FLinearColor::Black;
	MetaPixels[4] = FLinearColor(float(VoxelTextureSize.X), float(VoxelTextureSize.Y), 1.0f, 0.0f);

	UploadLinearColorsToRenderTarget(PositionRT, MoveTemp(PositionPixels));
	UploadLinearColorsToRenderTarget(NormalRT, MoveTemp(NormalPixels));
	UploadLinearColorsToRenderTarget(TargetRT, MoveTemp(TargetPixels));
	UploadLinearColorsToRenderTarget(CellRT, MoveTemp(CellPixels));
	UploadLinearColorsToRenderTarget(MetaRT, MoveTemp(MetaPixels));

	LastSurfaceVoxelTextureData.bValid = true;
	LastSurfaceVoxelTextureData.VoxelCount = EffectiveVoxelCount;
	LastSurfaceVoxelTextureData.VoxelSize = SurfaceVoxelData.VoxelSize;
	LastSurfaceVoxelTextureData.VoxelOrigin = VoxelOrigin;
	LastSurfaceVoxelTextureData.SourceWorldBounds = SourceBounds;
}

void AComputeShaderMeshGenerator::ClearTriangleTextureData()
{
	CSMeshGen_ReleaseRT(LastTriangleTextureData.TriangleVertexRT);
	CSMeshGen_ReleaseRT(LastTriangleTextureData.TriangleNormalRT);
	CSMeshGen_ReleaseRT(LastTriangleTextureData.TriangleMetaRT);
	LastTriangleTextureData = FCSMeshGeneratorTriangleTextureDataHandle();
}

void AComputeShaderMeshGenerator::ClearSurfaceVoxelTextureData()
{
	CSMeshGen_ReleaseRT(LastSurfaceVoxelTextureData.VoxelPositionRT);
	CSMeshGen_ReleaseRT(LastSurfaceVoxelTextureData.VoxelNormalRT);
	CSMeshGen_ReleaseRT(LastSurfaceVoxelTextureData.VoxelTargetRT);
	CSMeshGen_ReleaseRT(LastSurfaceVoxelTextureData.VoxelCellRT);
	CSMeshGen_ReleaseRT(LastSurfaceVoxelTextureData.VoxelMetaRT);
	LastSurfaceVoxelTextureData = FCSMeshGeneratorSurfaceVoxelTextureDataHandle();
	LastSurfaceVoxelData = FCSSurfaceVoxelData();
}

// -----------------------------------------------------------------------------
// Core System - GPU Mesh Output
//
// 这里的出口一律产出 UCSMesh。要 UDynamicMesh 的调用方走 UCSMeshOps::CopyToDynamicMesh
// —— GPU 管线只留一条出桥，转换的修补（材质 id、顶点色、逐角点属性）才会一次覆盖所有路径，
// 而不是只修到被改的那一条。
// -----------------------------------------------------------------------------

UCSMesh* AComputeShaderMeshGenerator::SurfaceVoxelsToOpenGpuMesh(UCSMesh* TargetMesh,
	float VoxelSize,
	bool bReverseOrientation,
	bool bRecomputeNormals)
{
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);

	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();

	const float BaseVoxelSize = VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize;
	const float SafeVoxelSize = FMath::Max(BaseVoxelSize, UE_KINDA_SMALL_NUMBER);
	const float HalfQuadSize = SafeVoxelSize * FMath::Max(QuadScale, UE_KINDA_SMALL_NUMBER) * 0.5f;

	TArray<FVector> Vertices;
	TArray<FVector> Normals;
	TArray<int32> Indices;
	Vertices.Reserve(FMath::Max(EffectiveVoxelCount, 0) * 4);
	Normals.Reserve(FMath::Max(EffectiveVoxelCount, 0) * 4);
	Indices.Reserve(FMath::Max(EffectiveVoxelCount, 0) * 6);

	for (int32 VoxelIndex = 0; VoxelIndex < EffectiveVoxelCount; ++VoxelIndex)
	{
		const FVector& Position = SurfaceVoxels.Positions[VoxelIndex];
		if (!IsFiniteVector(Position)) continue;

		FVector Normal = SurfaceVoxels.Normals.IsValidIndex(VoxelIndex)
			? SurfaceVoxels.Normals[VoxelIndex]
			: FVector::UpVector;
		if (Normal.ContainsNaN()) continue;
		Normal = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

		const FVector HelperAxis = FMath::Abs(Normal.Z) < 0.99 ? FVector::UpVector : FVector::RightVector;
		const FVector AxisX = FVector::CrossProduct(HelperAxis, Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
		const FVector AxisY = FVector::CrossProduct(Normal, AxisX).GetSafeNormal(UE_SMALL_NUMBER, FVector::RightVector);
		const FVector Center = Position + Normal * (SafeVoxelSize * NormalOffsetScale);
		const FVector DX = AxisX * HalfQuadSize;
		const FVector DY = AxisY * HalfQuadSize;

		const int32 A = Vertices.Num();
		Vertices.Add(Center - DX - DY);
		Vertices.Add(Center + DX - DY);
		Vertices.Add(Center + DX + DY);
		Vertices.Add(Center - DX + DY);
		for (int32 Corner = 0; Corner < 4; ++Corner) Normals.Add(Normal);

		// 两个三角形直接按最终绕序写出，所以下面 builder 的 bReverseOrientation 必须关着：
		// 在那里再翻一次会把这里的翻转抵消掉。
		const int32 Reversed[6] = { A + 0, A + 2, A + 1, A + 0, A + 3, A + 2 };
		const int32 Forward[6] = { A + 0, A + 1, A + 2, A + 0, A + 2, A + 3 };
		Indices.Append(bReverseOrientation ? Reversed : Forward, 6);
	}

	// 不做退化剔除：这里唯一能让四边形零面积的是 QuadScale，那是调用方自己设的，不该被悄悄丢掉。
	return CSMeshBuild::BuildGpuMeshFromCSTriangleData(TargetMesh, this, Vertices, Indices, Normals,
		Vertices.Num(), Indices.Num(), false, false, bRecomputeNormals);
}

UCSMesh* AComputeShaderMeshGenerator::SurfaceVoxelsToVDBGpuMesh(UCSMesh* TargetMesh,
	float VoxelSize,
	float RadiusMult,
	bool bRecomputeNormals)
{
	const FCSSurfaceVoxelData SurfaceVoxels = ReadbackBoxSceneSurfaceVoxelsSync(VoxelSize);

	const int32 EffectiveVoxelCount = SurfaceVoxels.VoxelCount >= 0
		? FMath::Clamp(SurfaceVoxels.VoxelCount, 0, SurfaceVoxels.Positions.Num())
		: SurfaceVoxels.Positions.Num();

	TArray<FVector> Positions;
	Positions.Reserve(FMath::Max(EffectiveVoxelCount, 0));
	for (int32 Index = 0; Index < EffectiveVoxelCount; ++Index) Positions.Add(SurfaceVoxels.Positions[Index]);

	// VoxelSize<=0 表示"沿用体素化时的尺寸"，这个回退只有读回来的数据知道，必须在这里解开，
	// 不能推给通用的粒子入口。
	const float SafeVoxelSize = FMath::Max(VoxelSize > 0.0f ? VoxelSize : SurfaceVoxels.VoxelSize, UE_KINDA_SMALL_NUMBER);
	return VDBParticlesToGpuMesh(TargetMesh, this, Positions, SafeVoxelSize, RadiusMult, bRecomputeNormals);
}

UCSMesh* AComputeShaderMeshGenerator::VDBParticlesToGpuMesh(UCSMesh* TargetMesh,
	UObject* Outer,
	const TArray<FVector>& WorldPositions,
	float VoxelSize,
	float RadiusMult,
	bool bRecomputeNormals)
{
	if (!TargetMesh) TargetMesh = UCSMeshOps::AllocateGpuMesh(Outer, 3, 3);
	if (!TargetMesh) return nullptr;

	const float SafeVoxelSize = FMath::Max(VoxelSize, UE_KINDA_SMALL_NUMBER);
	const float Rmin = 1.5f;
	const float Radius = FMath::Max((Rmin + 0.1f) * SafeVoxelSize * RadiusMult, (Rmin + 0.1f) * SafeVoxelSize);

	FCSGeneratorVDBParticleList Particles;
	for (const FVector& Position : WorldPositions) if (IsFiniteVector(Position)) Particles.Add(Position, Radius);

	if (Particles.size() == 0)
	{
		TargetMesh->Reset();
		return TargetMesh;
	}

	const float Rmax = 100.0f;
	openvdb::FloatGrid::Ptr LevelSet = openvdb::createLevelSet<openvdb::FloatGrid>(SafeVoxelSize, 2.0);
	openvdb::tools::ParticlesToLevelSet<openvdb::FloatGrid> Raster(*LevelSet);
	Raster.setRmax(Rmax);
	Raster.setRmin(Rmin);
	Raster.rasterizeTrails(Particles, 0.75);
	Raster.finalize(true);
	LevelSet->setTransform(openvdb::math::Transform::createLinearTransform(SafeVoxelSize));

	TArray<FVector> Vertices;
	TArray<int32> Indices;
	ConvertVDBVolumeToTriangles(LevelSet, Vertices, Indices);

	// 等值面本身不带法线。共享顶点 + builder 的面积加权累加就是"平滑"的那一档。
	if (bRecomputeNormals)
	{
		return CSMeshBuild::BuildGpuMeshFromCSTriangleData(TargetMesh, Outer, Vertices, Indices, TArray<FVector>(),
			Vertices.Num(), Indices.Num(), false, true, true);
	}

	// 平面着色这一档必须逐角点拆开：一个共享顶点只能带一条法线，想保住 VDB 的逐面法线就得先摊成 soup。
	TArray<FVector> SoupVertices;
	TArray<FVector> SoupNormals;
	SoupVertices.Reserve(Indices.Num());
	SoupNormals.Reserve(Indices.Num());
	for (int32 Corner = 0; Corner + 2 < Indices.Num(); Corner += 3)
	{
		const int32 SourceCorners[3] = { Indices[Corner + 0], Indices[Corner + 1], Indices[Corner + 2] };
		// 口径必须与平滑档（builder 的面积加权累加）同源，否则同一份 VDB 换个开关就整体翻面。
		const FVector FaceNormal = CSMeshBuild::ResidentFaceNormal(
			Vertices[SourceCorners[0]], Vertices[SourceCorners[1]], Vertices[SourceCorners[2]]);

		for (int32 k = 0; k < 3; ++k)
		{
			SoupVertices.Add(Vertices[SourceCorners[k]]);
			SoupNormals.Add(FaceNormal);
		}
	}

	return CSMeshBuild::BuildGpuMeshFromCSTriangleData(TargetMesh, Outer, SoupVertices, TArray<int32>(), SoupNormals,
		SoupVertices.Num(), 0, false, true, false);
}

void AComputeShaderMeshGenerator::DrawDebugBoxSceneSurfaceTrianglesGPU(float LifetimeSeconds)
{
	// 和提交路径同一条管线，只是不做参照点过滤并且到期自动清除。
	//
	// 容量**不**取 actor 的 MaxTriangles：那是抽取的上限（场景走查最多收多少三角），拿它当分配
	// 尺寸意味着永远按最坏情况分配。默认 2000 万三角 ≈ 2.2 GB 显存，过不了 UCSMesh 的预算闸门，
	// 于是一个调试可视化在多数机器上会直接变成"拒绝分配"。老的 proxy 是盲分配才看不出这个问题。
	// 调试画面本来就不需要看完整场景，这里给一个自己的上限，超出部分不画。
	static constexpr int32 DebugDrawTriangleCap = 2000000;
	const int32 DebugTriangles = FMath::Min(MaxTriangles, DebugDrawTriangleCap);
	if (MaxTriangles > DebugDrawTriangleCap)
		UE_LOG(LogTemp, Verbose, TEXT("[CSMeshGen] Debug draw caps extraction at %d of %d triangles."), DebugTriangles, MaxTriangles);

	if (!SubmitBoxSceneTrianglesToRenderPipeline(nullptr, DebugTriangles, 0.0f))
	{
		ClearDirectGPUMesh();
		return;
	}
	ScheduleDirectMeshClear(LifetimeSeconds);
}

void AComputeShaderMeshGenerator::SpawnDebugSurfaceTrianglesDynamicMeshActor(float LifetimeSeconds)
{
	DrawDebugBoxSceneSurfaceTrianglesGPU(LifetimeSeconds);
}

void AComputeShaderMeshGenerator::ClearMeshGeneratorGPUDebug()
{
	ClearDebugDisplay();
	// 三角汤迁到 UCSMesh 之前也归 DisplayComponent 管，清场一并抹掉它 —— 保持这条语义。
	ClearDirectGPUMesh();
}

bool AComputeShaderMeshGenerator::SubmitBoxSceneTrianglesToRenderPipeline(UMaterialInterface* Material, int32 MaxDirectTriangles, float ReferenceFilterDistance)
{
	UWorld* World = GetWorld();
	if (!World || !DirectMeshRenderComponent) return false;

	const FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid) return false;

	if (!DirectGpuMesh) DirectGpuMesh = UCSMeshOps::AllocateGpuMesh(this, 3, 3);
	if (!DirectGpuMesh) return false;

	// 提交是"替换"而不是"追加"：计数与材质表都要先归零。留着计数会让新几何堆在旧几何后面，
	// 留着材质表则会让 id 偏移越推越远、section 数每次提交都翻一倍。Reset 保留已有分配，
	// 所以同样容量的反复提交不会来回重分配。
	DirectGpuMesh->Reset();
	DirectGpuMesh->Materials.Reset();

	// 提取算子不再读 generator，本 actor 的场景过滤策略必须显式交出去，否则会退回算子的中立
	// 默认值（不排除自身、只按默认 tag 过滤），提取范围就变了。这份策略同时是 Blueprint 能拿到
	// 它的唯一途径，所以它住在 UCSMeshOps 而不是这里手写一份。
	// MaxDirectTriangles 仍然是分配上限，只是现在落在网格容量上而不是代理的持久 buffer 上。
	const FCSMeshBoxSceneOptions SceneOptions = UCSMeshOps::MakeGeneratorBoxSceneOptions(
		this, ReferenceFilterDistance, FMath::Max(1, MaxDirectTriangles));
	UCSMeshOps::AppendBoxSceneTriangles(DirectGpuMesh, this, SceneOptions);

	// 提取的输出大小由 GPU 决定，因此"盒内到底有没有几何"只能问 GPU。这比旧路径那个
	// game-thread 预检更准：预检说有，距离过滤之后仍可能一个三角都不剩。
	if (DirectGpuMesh->GetTriangleCountSync() <= 0)
	{
		// 显式记一笔：这里也接住"容量申请被显存预检拒了"的情况，那种失败在下游只表现为
		// 一个空网格，不打日志就没人能从"什么都没画出来"回溯到显存预算上。
		UE_LOG(LogTemp, Warning,
			TEXT("[CSMeshGenerator] SubmitBoxSceneTrianglesToRenderPipeline produced no triangles on %s (empty box, filtered out, or the %d-triangle allocation was refused)."),
			*GetActorNameOrLabel(), SceneOptions.MaxTriangles);
		ClearDirectGPUMesh();
		return false;
	}

	// 提取已经填好逐三角材质 id 和材质表；排一次 section 才能真的按材质槽分批绘制，
	// 否则整片场景只会用一个材质画出来（旧路径唯一能做到的事）。
	UCSMeshOps::BuildMaterialSections(DirectGpuMesh);

	if (Material) DirectMeshRenderComponent->MeshMaterial = Material;
	DirectMeshRenderComponent->SetGpuMesh(DirectGpuMesh);

	// 常驻显示：取消上一次 DrawDebug 排下的定时清除，否则新提交的网格会被旧计时器抹掉。
	ScheduleDirectMeshClear(-1.0f);
	return true;
}

void AComputeShaderMeshGenerator::ClearDirectGPUMesh()
{
	ScheduleDirectMeshClear(-1.0f);
	if (DirectMeshRenderComponent) DirectMeshRenderComponent->SetGpuMesh(nullptr);
	// 只清计数不放显存等于白占：这条路上的容量是按 MaxDirectTriangles 开的，可能是几百 MB。
	if (DirectGpuMesh) DirectGpuMesh->ReleaseSync();
}

void AComputeShaderMeshGenerator::ScheduleDirectMeshClear(float LifetimeSeconds)
{
	UWorld* World = GetWorld();
	if (!World) return;
	World->GetTimerManager().ClearTimer(DirectMeshClearTimerHandle);
	if (LifetimeSeconds <= 0.0f) return; // 常驻，直到调用方自己清除
	World->GetTimerManager().SetTimer(DirectMeshClearTimerHandle, this,
		&AComputeShaderMeshGenerator::ClearDirectGPUMesh, LifetimeSeconds, false);
}

int64 AComputeShaderMeshGenerator::EnsureGeneratorTimeCode()
{
	if (GeneratorTimeCode == -1)
	{
		const FDateTime Now = FDateTime::Now();
		GeneratorTimeCode =
			int64(Now.GetYear() % 100) * 100000000LL +
			int64(Now.GetMonth()) * 1000000LL +
			int64(Now.GetDay()) * 10000LL +
			int64(Now.GetHour()) * 100LL +
			int64(Now.GetMinute());
		// 编号必须随 actor 一起存盘，否则下次打开关卡又会生成一个新编号，重跑就不再覆盖旧资产。
		Modify();
	}
	return GeneratorTimeCode;
}

FString AComputeShaderMeshGenerator::GetResultAssetBaseName() const
{
	const FString ActorName = GetName();
	return ActorName.IsEmpty() ? TEXT("GpuMesh") : ActorName;
}

FString AComputeShaderMeshGenerator::GetResultAssetUniqueTag()
{
	return LexToString(EnsureGeneratorTimeCode());
}

FString AComputeShaderMeshGenerator::GetResultAssetFolderPath() const
{
	const ULevel* ActorLevel = GetLevel();
	const UPackage* LevelPackage = ActorLevel ? ActorLevel->GetPackage() : nullptr;
	const FString LevelPackageName = LevelPackage ? LevelPackage->GetName() : FString();
	if (!FPackageName::IsValidLongPackageName(LevelPackageName)) return FString();

	const FString LevelFolder = FPackageName::GetLongPackagePath(LevelPackageName); // 例如 /Game/Maps
	if (LevelFolder.IsEmpty()) return FString();

	const FString FolderName = GetResultAssetFolderName();
	return FolderName.IsEmpty() ? LevelFolder : LevelFolder / FolderName;
}

FString AComputeShaderMeshGenerator::BuildResultAssetPath(const FString& NameSuffix)
{
	const FString ResultFolderPath = GetResultAssetFolderPath();
	if (ResultFolderPath.IsEmpty()) return FString();

	return FString::Printf(TEXT("%s/SM_%s_%s%s"),
		*ResultFolderPath, *GetResultAssetBaseName(), *GetResultAssetUniqueTag(), *NameSuffix);
}

// -----------------------------------------------------------------------------
// 结果 StaticMeshActor 的生成 / 挂接 / 清理
// -----------------------------------------------------------------------------

namespace
{
// 结果 actor 的识别标签。清场只销毁带此标签的挂接 actor，用户自己挂的不受影响。
const FName CSGeneratedResultActorTag(TEXT("CSGeneratedResultActor"));

bool IsCSGeneratedResultActor(const AActor* Actor)
{
	return Actor && Actor->Tags.Contains(CSGeneratedResultActorTag);
}
}

AStaticMeshActor* AComputeShaderMeshGenerator::SpawnAttachedResultActor(UStaticMesh* Mesh, const FString& InActorLabel)
{
	if (!Mesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSMeshGenerator] SpawnAttachedResultActor skipped: null mesh on %s."), *GetActorNameOrLabel());
		return nullptr;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSMeshGenerator] SpawnAttachedResultActor failed: invalid world on %s."), *GetActorNameOrLabel());
		return nullptr;
	}

	// 先清掉上一次的结果：挂接列表就是账本，按标签认领即可，调用方不必自己记账。
	// 标签是唯一凭据——没有标签的挂接 actor 是用户自己放的，绝不能销毁。
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (!IsCSGeneratedResultActor(AttachedActor)) continue;
		AttachedActor->Modify();
		AttachedActor->Destroy();
	}

	const FString EffectiveLabel = InActorLabel.IsEmpty() ? Mesh->GetName() : InActorLabel;

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	SpawnParams.OverrideLevel = GetLevel();
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
#if WITH_EDITOR
	SpawnParams.InitialActorLabel = EffectiveLabel;
#endif

	// 生成在本 actor 的变换上：资产按 actor 局部空间烘焙，二者对齐后网格才落在原位。
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(), GetActorTransform(), SpawnParams);
	if (!Spawned)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSMeshGenerator] SpawnAttachedResultActor failed: could not spawn actor for %s."), *Mesh->GetPathName());
		return nullptr;
	}

	Spawned->Modify();
	Spawned->Tags.AddUnique(CSGeneratedResultActorTag);
	if (UStaticMeshComponent* StaticMeshComponent = Spawned->GetStaticMeshComponent())
	{
		// 生成的结果要能跟着生成器一起移动，故必须是 Movable。
		StaticMeshComponent->SetMobility(EComponentMobility::Movable);
		StaticMeshComponent->SetStaticMesh(Mesh);
		StaticMeshComponent->UpdateBounds();
		StaticMeshComponent->MarkRenderStateDirty();
	}
	Spawned->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
#if WITH_EDITOR
	Spawned->SetActorLabel(EffectiveLabel);
#endif
	Spawned->MarkPackageDirty();

	MarkPackageDirty();
	return Spawned;
}


UStaticMesh* AComputeShaderMeshGenerator::SaveDirectGPUMeshToStaticMesh(
	const FString& AssetPathAndName,
	bool bReplaceExistingAsset,
	bool bSaveAsset,
	bool bConvertToActorLocalSpace)
{
#if WITH_EDITOR
	if (!DirectGpuMesh || !DirectMeshRenderComponent) return nullptr;

	// 空路径走本 actor 的稳定结果命名（覆盖上一次的结果），而不是让落盘层各自兜底。
	// 仍留空（未存盘的地图）时交给落盘层的 AutoResult 兜底，与改动前一致。
	FString EffectiveAssetPath = AssetPathAndName.TrimStartAndEnd();
	if (EffectiveAssetPath.IsEmpty()) EffectiveAssetPath = BuildResultAssetPath();

	// 落盘本身没有任何"直接网格"特有的东西，走组件那条公用入口。烘焙空间用 actor 变换：
	// 组件以绝对变换渲染，它自己的组件变换恒为单位，按它烘会把几何冻结在世界坐标上。
	//
	// 显式把 DirectGpuMesh 传进去，而不是让组件用自己的绑定：几何归本 actor 所有，组件只是
	// 它的显示端。上面那句 `if (!DirectGpuMesh)` 守卫本来就说明了这一点 —— 不传的话，组件一旦
	// 被解绑（或绑到别的网格上），这里就会静默存出空/错的东西。
	return DirectMeshRenderComponent->SaveToStaticMesh(
		GetActorTransform(), EffectiveAssetPath, bReplaceExistingAsset, bSaveAsset, bConvertToActorLocalSpace,
		/*bEnableNanite*/ false, DirectGpuMesh);
#else
	return nullptr;
#endif
}

// -----------------------------------------------------------------------------
// Core System - Lifecycle
// -----------------------------------------------------------------------------

CSGpuTriangleUtilities::FTriangleLBVH AComputeShaderMeshGenerator::AddTriangleLBVHToRDG(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef TriangleSoupSRV,
	int32 TriangleCount,
	int32 SortElementCount,
	const FVector3f& AabbMin,
	const FVector3f& InvExtent)
{
	return CSGpuTriangleUtilities::AddTriangleLBVHBuildPasses(
		GraphBuilder, TriangleSoupSRV, TriangleCount, SortElementCount, AabbMin, InvExtent);
}

FRDGBufferRef AComputeShaderMeshGenerator::AddFastWindingToRDG(
	FRDGBuilder& GraphBuilder,
	FRDGBufferSRVRef TriangleSoupSRV,
	const CSGpuTriangleUtilities::FTriangleLBVH& LBVH,
	int32 TriangleCount)
{
	return CSGpuTriangleUtilities::AddFastWindingMultipolePasses(
		GraphBuilder, TriangleSoupSRV, LBVH, TriangleCount);
}

FRDGBufferRef AComputeShaderMeshGenerator::AddVertexWeldToRDG(
	FRDGBuilder& GraphBuilder,
	FRDGBufferRef OutputTriangleSoup,
	FRDGBufferRef OutputTriangleCounter,
	int32 OutputTriangleCapacity,
	int32 SourceTriangleCapacity,
	const FVector3f& GridOrigin,
	float WeldDistance,
	FRDGBufferSRVRef TriangleFilter,
	uint32 TriangleFilterMask)
{
	return CSGpuTriangleUtilities::AddVertexWeldPasses(
		GraphBuilder,
		OutputTriangleSoup,
		OutputTriangleCounter,
		OutputTriangleCapacity,
		SourceTriangleCapacity,
		GridOrigin,
		WeldDistance,
		TriangleFilter,
		TriangleFilterMask);
}

bool AComputeShaderMeshGenerator::ConfirmGpuMemoryBudgetForBoxScene(
	const TCHAR* OperationName,
	const FBox& QueryBox,
	const CSGpuMemoryBudget::FTriangleSoupCostModel& Cost,
	bool bIncludeLandscape) const
{
	if (!bCheckGpuMemoryBudget) return true;

	CSGpuMemoryBudget::FBudgetCheckSettings Settings;
	Settings.SafetyRatio = GpuMemoryBudgetSafetyRatio;
	Settings.bPromptOnExceed = bPromptWhenExceedingGpuMemoryBudget;
	Settings.bProceedWhenUnattended = bProceedWhenGpuMemoryBudgetUnattended;

	// The scene estimate walks the same components the extraction would, so it must run with the
	// LOD the requests will actually use.
	const CSGpuMemoryBudget::FBudgetCheckResult Result = CSGpuMemoryBudget::CheckBoxSceneTriangleBudget(
		OperationName, GetWorld(), QueryBox, VoxelGridSettings.LODIndex, bIncludeLandscape, Cost, Settings);
	return Result.ShouldProceed();
}

AComputeShaderMeshGenerator::AComputeShaderMeshGenerator()
	: AComputeShaderMeshGenerator(FObjectInitializer::Get())
{
}

AComputeShaderMeshGenerator::AComputeShaderMeshGenerator(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;

	// GeneratorTimeCode 留在 -1，由 EnsureGeneratorTimeCode() 在第一次保存结果时懒生成：
	// 在构造期赋值会连 CDO 一起写上编号，与 CDO 同值的实例不会被 delta 序列化，重新加载后
	// 编号变成加载时刻的值，资产名跟着变，重跑就不再覆盖旧模型。

	USceneComponent* ExistingRoot = GetRootComponent();
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SceneRoot->bEditableWhenInherited = false;
	SetRootComponent(SceneRoot);
	if (ExistingRoot && ExistingRoot != SceneRoot)
	{
		ExistingRoot->SetupAttachment(SceneRoot);
	}

	GeneratorBounds = CreateDefaultSubobject<UBoxComponent>(TEXT("GeneratorBounds"));
#if WITH_EDITOR
	GeneratorBounds->SetIsVisualizationComponent(true);
#endif
	GeneratorBounds->bEditableWhenInherited = false;
	GeneratorBounds->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	GeneratorBounds->SetCollisionObjectType(ECollisionChannel::ECC_WorldDynamic);
	GeneratorBounds->SetCollisionResponseToAllChannels(ECR_Overlap);
	GeneratorBounds->SetHiddenInGame(true);
	GeneratorBounds->SetBoxExtent(FVector(500.0, 500.0, 500.0));
	GeneratorBounds->SetupAttachment(SceneRoot);

	DisplayComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("DisplayComponent"));
	DisplayComponent->SetupAttachment(SceneRoot);
	// 箭头的颜色写在顶点色里（BuildPointArrowGeometryIntoMesh 打包 ArrowColor），所以默认材质
	// 必须是读顶点色的那个，否则 DirectionColor 静默失效——代理时代颜色是逐次绘制的材质参数，
	// 换成常驻几何后它变成了顶点属性。取不到就退回引擎调试材质（形状仍在，只是没颜色）。
	if (GEngine)
	{
		UMaterialInterface* VertexColorMaterial = GEngine->VertexColorViewModeMaterial_ColorOnly;
		DisplayComponent->MeshMaterial = VertexColorMaterial ? VertexColorMaterial : ToRawPtr(GEngine->DebugMeshMaterial);
	}

	// 常驻场景三角汤的画笔。自身不生成任何东西，只绑定 DirectGpuMesh；没绑网格时不建代理，
	// 所以对没用过提交路径的 actor 是零成本。
	DirectMeshRenderComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("DirectMeshRenderComponent"));
	DirectMeshRenderComponent->SetupAttachment(SceneRoot);
}

void AComputeShaderMeshGenerator::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearMeshGeneratorGPUDebug();
	LastSurfaceVoxelGPUBuffers.Reset();
	ClearGeneratedDataTextureCache();
	Super::EndPlay(EndPlayReason);
}

// -----------------------------------------------------------------------------
// Generator bounds helper（BrushCache 缓存实现已归位 MeshGeneratorBrushCache.cpp）
// -----------------------------------------------------------------------------

FBox AComputeShaderMeshGenerator::GetGeneratorBoundsWorldBox() const
{
	if (!GeneratorBounds)
	{
		return FBox(ForceInit);
	}

	const FBox BoundsBox = GeneratorBounds->Bounds.GetBox();
	if (BoundsBox.IsValid)
	{
		return BoundsBox;
	}

	const FVector SafeExtent = GeneratorBounds->GetScaledBoxExtent().ComponentMax(FVector::ZeroVector);
	return FBox(GeneratorBounds->GetComponentLocation() - SafeExtent, GeneratorBounds->GetComponentLocation() + SafeExtent);
}


namespace
{
// FIndexedMeshToHeightmapCS 的目标封装：一张清成哨兵的 uint 高度图 + 光栅化参数，
// 单网格入口与多网格入口共用（多网格把所有 pass 打进同一张 uint 图后只转换一次）。
struct FIndexedMeshHeightmapTarget
{
	FRDGTextureRef HeightmapUint = nullptr;
	FRDGTextureUAVRef HeightmapUintUAV = nullptr;
	FVector2f BoundsMin = FVector2f::ZeroVector;
	FVector2f BoundsInvSize = FVector2f::ZeroVector;
	float CameraHeight = 0.0f;
	FIntPoint TextureSize = FIntPoint::ZeroValue;
};

FIndexedMeshHeightmapTarget MakeIndexedMeshHeightmapTarget(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef OutputHeightmap,
	const FBox& WorldBounds,
	float CameraHeight)
{
	FIndexedMeshHeightmapTarget Target;
	Target.TextureSize = FIntPoint(OutputHeightmap->Desc.Extent.X, OutputHeightmap->Desc.Extent.Y);
	Target.CameraHeight = CameraHeight;

	const FVector2f BoundsSize(WorldBounds.Max.X - WorldBounds.Min.X, WorldBounds.Max.Y - WorldBounds.Min.Y);
	Target.BoundsMin = FVector2f(WorldBounds.Min.X, WorldBounds.Min.Y);
	Target.BoundsInvSize = FVector2f(
		BoundsSize.X > 0.01f ? 1.0f / BoundsSize.X : 0.0f,
		BoundsSize.Y > 0.01f ? 1.0f / BoundsSize.Y : 0.0f);

	const FRDGTextureDesc UintDesc = FRDGTextureDesc::Create2D(
		Target.TextureSize, PF_R32_UINT, FClearValueBinding::None,
		TexCreate_ShaderResource | TexCreate_UAV);
	Target.HeightmapUint = GraphBuilder.CreateTexture(UintDesc, TEXT("CS.IdxMeshHeightmapUint"));
	Target.HeightmapUintUAV = GraphBuilder.CreateUAV(Target.HeightmapUint);
	AddClearUAVPass(GraphBuilder, Target.HeightmapUintUAV, 0xFFFFFFFFu);
	return Target;
}

// 单网格光栅化 pass：index/position buffer 直读 + 顶点变换 + 俯视光栅化一次完成，
// 不再经过 extract→triangle soup 的中转（那一套 per-mesh 顶点/法线/UV/材质 buffer 全部省掉）。
// IndexBufferSRV 按引用捕获进 pass lambda 保活；PositionBufferSRV 的生命周期由调用方
// 持有的 LODResource 引用保证。
void AddIndexedMeshToHeightmapPass(
	FRDGBuilder& GraphBuilder,
	const FIndexedMeshHeightmapTarget& Target,
	FShaderResourceViewRHIRef IndexBufferSRV,
	FRHIShaderResourceView* PositionBufferSRV,
	const FMatrix44f& LocalToWorld,
	const FBox3f& TriangleCullBounds,
	uint32 TriangleCount,
	uint32 PositionStrideFloat)
{
	TShaderMapRef<FIndexedMeshToHeightmapCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* PassParams = GraphBuilder.AllocParameters<FIndexedMeshToHeightmapCS::FParameters>();
	PassParams->IndexBuffer = IndexBufferSRV.GetReference();
	PassParams->PositionBuffer = PositionBufferSRV;
	PassParams->RW_HeightmapUint = Target.HeightmapUintUAV;
	PassParams->LocalToWorld = LocalToWorld;
	PassParams->BoundsMin = TriangleCullBounds.IsValid ? TriangleCullBounds.Min : FVector3f(-TNumericLimits<float>::Max());
	PassParams->BoundsMax = TriangleCullBounds.IsValid ? TriangleCullBounds.Max : FVector3f(TNumericLimits<float>::Max());
	PassParams->TriangleCount = TriangleCount;
	PassParams->PositionStrideFloat = PositionStrideFloat;
	PassParams->bUseBounds = TriangleCullBounds.IsValid ? 1u : 0u;
	PassParams->HM_BoundsMin = Target.BoundsMin;
	PassParams->HM_BoundsInvSize = Target.BoundsInvSize;
	PassParams->HM_CameraHeight = Target.CameraHeight;
	PassParams->HM_TextureSize = Target.TextureSize;

	GraphBuilder.AddPass(RDG_EVENT_NAME("IndexedMeshToHeightmap"), PassParams, ERDGPassFlags::Compute,
		[PassParams, CS, TriangleCount, IndexBufferSRV](FRHIComputeCommandList& CmdList)
		{
			// wrapped：与 IndexedMeshToHeightmapCS 的 GetUnWrappedDispatchThreadId 配套（大网格三角数可 >4.19M）
			FComputeShaderUtils::Dispatch(CmdList, CS, *PassParams,
				FComputeShaderUtils::GetGroupCountWrapped(FMath::Max(1, int32(TriangleCount)), 64));
		});
}
} // namespace

void AComputeShaderMeshGenerator::RasterizeIndexedMeshToHeightmapRDG(
	FRDGBuilder& GraphBuilder,
	FRHIShaderResourceView* PositionSRV,
	FRHIShaderResourceView* IndexSRV,
	uint32 TriangleCapacity,
	const FMatrix44f& LocalToWorld,
	FRDGTextureRef OutHeightmap,
	const FBox& WorldBounds,
	float CameraHeight)
{
	if (!PositionSRV || !IndexSRV || TriangleCapacity == 0 || !OutHeightmap) return;

	FIndexedMeshHeightmapTarget Target = MakeIndexedMeshHeightmapTarget(GraphBuilder, OutHeightmap, WorldBounds, CameraHeight);

	// Unused indices are zero -> degenerate (0,0,0) triangles that rasterize to nothing.
	AddIndexedMeshToHeightmapPass(GraphBuilder, Target, IndexSRV, PositionSRV,
		LocalToWorld, FBox3f(EForceInit::ForceInit), TriangleCapacity, 3u);

	// 覆盖式 uint→float 转换，保持本入口旧语义（空 texel 也写入，值为哨兵位型）。
	{
		TShaderMapRef<FConvertHeightmapUintToFloatCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* ConvertParams = GraphBuilder.AllocParameters<FConvertHeightmapUintToFloatCS::FParameters>();
		ConvertParams->T_HeightmapUint = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(Target.HeightmapUint));
		ConvertParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutHeightmap);
		ConvertParams->HM_TextureSize = Target.TextureSize;

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(Target.TextureSize.X, 8),
			FMath::DivideAndRoundUp(Target.TextureSize.Y, 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ConvertHeightmapUintToFloat"),
			ERDGPassFlags::Compute,
			CS,
			ConvertParams,
			GroupCount);
	}
}

void CSMeshGenInternal::RasterizeResolvedMeshesToHeightmapRDG(
	FRDGBuilder& GraphBuilder,
	FRHICommandListImmediate& RHICmdList,
	const TArray<FResolvedStaticMeshTriangleRequest>& ResolvedRequests,
	FRDGTextureRef OutputHeightmap,
	const FBox& WorldBounds,
	float CameraHeight)
{
	if (ResolvedRequests.IsEmpty() || !OutputHeightmap) return;

	FIndexedMeshHeightmapTarget Target = MakeIndexedMeshHeightmapTarget(GraphBuilder, OutputHeightmap, WorldBounds, CameraHeight);

	bool bAnyPass = false;
	for (const FResolvedStaticMeshTriangleRequest& Request : ResolvedRequests)
	{
		if (!Request.LODResource || Request.TriangleCount <= 0) continue;

		FShaderResourceViewRHIRef IndexBufferSRV = CreateTriangleIndexBufferSRV(RHICmdList, Request.LODResource.GetReference());
		FRHIShaderResourceView* PositionBufferSRV = Request.LODResource->VertexBuffers.PositionVertexBuffer.GetSRV();
		if (!IndexBufferSRV.IsValid() || !PositionBufferSRV) continue;

		AddIndexedMeshToHeightmapPass(GraphBuilder, Target, IndexBufferSRV, PositionBufferSRV,
			Request.LocalToWorld, Request.WorldBounds, uint32(Request.TriangleCount), uint32(Request.PositionStrideFloat));
		bAnyPass = true;
	}
	if (!bAnyPass) return;

	// min 合并进浮点输出：空 texel 保留输出现值（清屏哨兵/已合并的地形深度不被覆盖）。
	TShaderMapRef<FMergeHeightmapUintToFloatMinCS> MergeCS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* MergeParams = GraphBuilder.AllocParameters<FMergeHeightmapUintToFloatMinCS::FParameters>();
	MergeParams->T_HeightmapUint = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(Target.HeightmapUint));
	MergeParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputHeightmap);
	MergeParams->HM_TextureSize = Target.TextureSize;

	FIntVector GroupCount(
		FMath::DivideAndRoundUp(Target.TextureSize.X, 8),
		FMath::DivideAndRoundUp(Target.TextureSize.Y, 8),
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("MergeHeightmapUintToFloatMin"),
		ERDGPassFlags::Compute,
		MergeCS,
		MergeParams,
		GroupCount);
}

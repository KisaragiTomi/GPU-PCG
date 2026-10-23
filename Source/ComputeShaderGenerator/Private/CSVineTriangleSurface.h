#pragma once

#include "CoreMinimal.h"
#include "CSBoxSceneCollection.h"
#include "CSMesh.h"
#include "ShaderParameterStruct.h"

// The synchronous editor experiment copies Nanite pages into an independently owned mesh.
// Only that resident copy crosses into the asynchronous vine graph, never a Nanite page address.
struct FCSVineTriangleSurfaceInputs
{
	TRefCountPtr<FRDGPooledBuffer> NanitePositions;
	TRefCountPtr<FRDGPooledBuffer> NaniteIndices;
	FCSBoxScenePreparedData OtherGeometry;
	FBox Bounds = FBox(ForceInit);
	uint32 NaniteTriangles = 0;
	uint32 OtherTriangleCapacity = 0;
	uint64 MemoryBudgetBytes = 0;
	uint64 SurfaceBufferBytes = 0;
	float MaxProjectionDistance = 0.0f;
};

BEGIN_SHADER_PARAMETER_STRUCT(FCSVineTriangleParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, VineSurfaceTriangles)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, VineSurfaceBVH)
	SHADER_PARAMETER(uint32, VineSurfaceTriangleCount)
	SHADER_PARAMETER(float, VineSurfaceMaxDistance)
END_SHADER_PARAMETER_STRUCT()

TSharedPtr<FCSVineTriangleSurfaceInputs, ESPMode::ThreadSafe> PrepareVineTriangleSurface(
	UObject* Outer, UWorld* World, const FCSBoxSceneCollectOptions& Options,
	float WorldCutError, int32 MemoryBudgetMiB, float MaxProjectionDistance);

FCSVineTriangleParameters AddVineTriangleSurfacePasses(
	FRDGBuilder& GraphBuilder, const FCSVineTriangleSurfaceInputs* Input);

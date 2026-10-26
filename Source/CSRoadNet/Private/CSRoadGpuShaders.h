// Shader parameter blocks and RDG upload helpers shared by the CSRoad compute passes
// (CSRoadGeometry.usf, CSRoadMesh.usf).
#pragma once

#include "CSRoadCore.h"
#include "RenderGraphBuilder.h"
#include "ShaderParameterStruct.h"

// One block for every geometry kernel (CSRoadGeometryCommon.ush); AddPass drops what a kernel does not use.
BEGIN_SHADER_PARAMETER_STRUCT(FCSRoadGeometryParameters, )
	SHADER_PARAMETER(uint32, NumEntities)
	SHADER_PARAMETER(uint32, NumNodes)
	SHADER_PARAMETER(uint32, NumEdges)
	SHADER_PARAMETER(uint32, IterationIndex)
	SHADER_PARAMETER(FVector3f, TerrainBoundsMin)
	SHADER_PARAMETER(FVector3f, TerrainBoundsMax)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EntityMask)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, EntityPrefab)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Node>, NodeData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Edge>, EdgeData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Curve>, CurveData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Elevation>, ElevationData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Owner>, OwnerData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<Composition>, CompositionData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, ConnectedEdgeRange)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, ConnectedEdgeList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, NodeList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, EdgeList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetGeometryData>, PrefabGeometryData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<PlaceableNetData>, PrefabPlaceableData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetCompositionData>, PrefabCompositionData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CompositionPieceRange)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetCompositionPiece>, CompositionPieceList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CompositionLaneRange)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetCompositionLane>, CompositionLaneList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CompositionCrosswalkRange)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetCompositionCrosswalk>, CompositionCrosswalkList)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<NetLaneData>, LaneDataTable)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<NodeGeometry>, NodeGeometryRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<EdgeGeometry>, EdgeGeometryRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<EdgeNodeGeometry>, StartNodeGeometryRW)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<EdgeNodeGeometry>, EndNodeGeometryRW)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<EdgeNodeGeometry>, PrevStartNodeGeometry)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<EdgeNodeGeometry>, PrevEndNodeGeometry)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, EdgeHeightMap)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, EdgeHeightMapValid)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<GeometrySystem_IntersectionData>, IntersectionBuffer)
	SHADER_PARAMETER(uint32, bHasTerrain)
	SHADER_PARAMETER(FVector4f, TerrainPlayableOffsetSize)
	SHADER_PARAMETER(FVector4f, TerrainWorldOffsetSize)
	SHADER_PARAMETER(FIntVector4, TerrainDims)
	SHADER_PARAMETER(FVector2f, TerrainHeightScaleOffset)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TerrainPlayable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TerrainWorld)
END_SHADER_PARAMETER_STRUCT()

namespace CSRoad::GpuDetail
{
static constexpr int32 GroupSize = 64;

inline void Zero(void* Data, uint32 Size) { FMemory::Memzero(Data, Size); }

// Structured buffer holding a copy of Data (one zeroed element when Data is empty).
template<typename T>
FRDGBufferRef Upload(FRDGBuilder& GraphBuilder, const TArray<T>& Data, const TCHAR* Name)
{
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(T), FMath::Max(1, Data.Num())), Name);
	if (Data.Num() > 0) GraphBuilder.QueueBufferUpload(Buffer, Data.GetData(), uint64(sizeof(T)) * uint64(Data.Num()));
	else GraphBuilder.QueueBufferUpload(Buffer, Zero);
	return Buffer;
}

template<typename T>
FRDGBufferSRVRef UploadSRV(FRDGBuilder& GraphBuilder, const TArray<T>& Data, const TCHAR* Name)
{
	return GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Upload(GraphBuilder, Data, Name)));
}

template<typename T>
FRDGBufferRef Zeroed(FRDGBuilder& GraphBuilder, uint32 Count, const TCHAR* Name)
{
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(T), FMath::Max(1u, Count)), Name);
	GraphBuilder.QueueBufferUpload(Buffer, Zero);
	return Buffer;
}
}

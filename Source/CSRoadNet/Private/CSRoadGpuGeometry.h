// GPU half of a road network build: GeometrySystem's jobs as compute passes
// (Shaders/Private/CSRoad/CSRoadGeometry.usf) over the entity tables of an FGraph.
#pragma once

#include "CSRoadGraph.h"
#include "RenderGraphFwd.h"

class FCSRoadGeometryParameters;

namespace CSRoad
{
struct FSavedTerrain;

// FlattenNodeGeometryJob keeps a node's edges in a fixed array (CSROAD_MAX_NODE_EDGES).
static constexpr int32 GpuMaxNodeEdges = 16;

// (offset, count) into a flat list: the GPU stand-in for a DynamicBuffer.
struct FGpuRange
{
	uint32 Offset = 0;
	uint32 Count = 0;
};

// Structured-buffer images of one FGraph. Entity, prefab and composition tables keep the
// graph's 1-based indexing; lane prefab ids are remapped into the dense LaneDataTable.
struct FGpuGeometryInput
{
	uint32 NumEntities = 0;
	TArray<uint32> EntityMask;
	TArray<int32> EntityPrefab;
	TArray<Node> NodeData;
	TArray<Edge> EdgeData;
	TArray<Curve> CurveData;
	TArray<Elevation> ElevationData;
	TArray<Owner> OwnerData;
	TArray<Composition> CompositionData;
	TArray<NodeGeometry> InitialNodeGeometry;
	TArray<FGpuRange> ConnectedEdgeRange;
	TArray<int32> ConnectedEdgeList;
	TArray<int32> NodeList;
	TArray<int32> EdgeList;

	TArray<NetGeometryData> PrefabGeometry;
	TArray<PlaceableNetData> PrefabPlaceable;
	TArray<NetCompositionData> PrefabComposition;
	TArray<FGpuRange> PieceRange;
	TArray<NetCompositionPiece> PieceList;
	TArray<FGpuRange> LaneRange;
	TArray<NetCompositionLane> LaneList;
	TArray<FGpuRange> CrosswalkRange;
	TArray<NetCompositionCrosswalk> CrosswalkList;
	TArray<NetLaneData> LaneDataTable;

	// TerrainHeightData: two R16 maps, two samples per uint (low half first).
	bool bHasTerrain = false;
	FVector4f TerrainPlayableOffsetSize = FVector4f::Zero();
	FVector4f TerrainWorldOffsetSize = FVector4f::Zero();
	FIntVector4 TerrainDims = FIntVector4(0);
	FVector2f TerrainHeightScaleOffset = FVector2f::ZeroVector;
	TArray<uint32> TerrainPlayable;
	TArray<uint32> TerrainWorld;

	bool Build(const FGraph& Graph, const FAssets& Assets, const FSavedTerrain* Terrain, FString& OutError);
};

struct FGpuGeometryOutput
{
	// The bound tables, graph-allocated: later passes (CSRoadGpuMesh) include them as is.
	const FCSRoadGeometryParameters* Parameters = nullptr;
	FRDGBufferRef NodeGeometry = nullptr;
	FRDGBufferRef EdgeGeometry = nullptr;
	FRDGBufferRef StartNodeGeometry = nullptr;
	FRDGBufferRef EndNodeGeometry = nullptr;
};

// Render thread: adds the GeometrySystem passes in job order. The input is copied into the graph.
FGpuGeometryOutput AddGeometryPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryInput& Input);

// Per-entity results (index = entity id).
struct FGpuGeometryResult
{
	TArray<NodeGeometry> Nodes;
	TArray<EdgeGeometry> Edges;
	TArray<EdgeNodeGeometry> StartNodes;
	TArray<EdgeNodeGeometry> EndNodes;
	double Milliseconds = 0.0;
};

// Game thread: runs the passes and blocks until the four tables are read back.
bool RunGeometryBlocking(const FGpuGeometryInput& Input, FGpuGeometryResult& OutResult, FString& OutError);
}

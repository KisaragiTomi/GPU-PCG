// Local updates of a road network built in square tiles of its ground plane (Unity metres, x/z).
// A road belongs to the tile its curve's middle lies in and draws there: its edge and both node
// meshes, and its decals (a junction connector goes with its source road).
//
// A rebuild finds what to draw again without building the whole graph. A road's compositions and GPU
// geometry depend only on the roads within two rings of it, a ring being a node or an owner in common
// (checked bit for bit on Palmsland: Subgraph.Reach). So a road that came, went or asks for something
// else can change only the output of the roads within two rings of it; those are built again, in a
// sub-graph holding two rings more, and compared with what they came out as last time. Every tile
// holding a road at a node of a road whose output changed is drawn again, from a sub-graph holding its
// roads, the roads at their nodes and two rings more: all of those come out as in a build of the whole
// network.
//
// A build may write only some of the tiles to rebuild (a batch): the others stay pending, rebuilt
// by a later build whatever its comparison finds, since the roads they hold are already committed.
#pragma once

#include "CSRoadGpuMesh.h"

namespace CSRoad
{
// One road as the tiles see it, taken from its spec and nodes alone.
struct FTileRoad
{
	// What was asked for: prefab, curve, data, owner and both nodes' data (a repeat gets its own key).
	uint64 Key = 0;
	// What came out (edge and node geometry, compositions), from the last build that computed it.
	uint64 Output = 0;
	FIntPoint Tile = FIntPoint::ZeroValue;
	// Its nodes, by position.
	uint64 StartNode = 0;
	uint64 EndNode = 0;
	// Its owner and its nodes' owners (0: none): the geometry jobs treat one owner's roads together.
	int32 Owners[3] = { 0, 0, 0 };
};

// A network's roads in spec order, and which of them meet: a node or an owner in common.
class FTileNetwork
{
public:
	TArray<FTileRoad> Roads;

	void Build(const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes, double TileSize);
	// The roads within Rings rings of Roads and of the given nodes and owners (a road at one of those
	// nodes, or with one of those owners, is one ring from them), Roads included; in spec order.
	TArray<int32> Around(const TArray<int32>& From, const TSet<uint64>& Nodes, const TSet<int32>& Owners, int32 Rings) const;

private:
	TMap<uint64, TArray<int32>> AtNode;
	TMap<int32, TArray<int32>> OfOwner;
};

// What a road (its index in Graph) came out as: edge and node geometry, compositions.
uint64 TileOutput(const FGraph& Graph, const FGpuGeometryResult& Geometry, int32 Road, uint64 Key);

class FTileState
{
public:
	// Kept between builds: the meshes of compositions FAssets has made.
	FCompositionMeshCache Compositions;

	// Signature: whatever else the meshes are made from (tile size, transform, materials, settings);
	// when it differs from the last build's, everything is rebuilt.
	bool Matches(uint64 Signature) const { return bValid && LastSignature == Signature; }
	// The roads whose output may differ from the last build's: those within two rings of a road that
	// came, went or asks for something else. In spec order.
	TArray<int32> Affected(const FTileNetwork& Network) const;
	// The tiles to rebuild, each with the roads drawn in it (spec order; an empty list removes the tile):
	// the tiles of the roads at a node of a road whose output changed, came or went, and of the roads
	// that went; then those left pending. Each part by row, then column. Outputs: what the affected
	// roads come out as now. bEverything: every tile, Existing (the tiles drawn now) included.
	TMap<FIntPoint, TArray<int32>> Dirty(const FTileNetwork& Network, const TArray<int32>& Affected, const TArray<uint64>& Outputs, bool bEverything, const TArray<FIntPoint>& Existing) const;
	// The network as built (the affected roads' outputs as given, the others' as before); of the dirty
	// tiles, Written were rebuilt and the others stay pending.
	void Commit(const FTileNetwork& Network, const TArray<int32>& Affected, const TArray<uint64>& Outputs, uint64 Signature, const TMap<FIntPoint, TArray<int32>>& Dirty, const TArray<FIntPoint>& Written);
	// The next build rebuilds everything.
	void Reset();
	int32 NumPending() const { return Pending.Num(); }

private:
	// By key, with their outputs.
	TMap<uint64, FTileRoad> Last;
	TSet<FIntPoint> Pending;
	uint64 LastSignature = 0;
	bool bValid = false;
};

// The mesh instances a tile writes: its roads' edge and node instances, drawn; and, as context for
// the decal receivers, the edge instance and the node instance there of every other road at their
// nodes. RoadOfEntity: FGraph::Roads index per entity (INDEX_NONE for nodes).
void TileInstances(const FGraph& Graph, const TArray<int32>& RoadOfEntity, const TArray<int32>& Roads, TArray<FGpuInstanceRef>& OutDrawn, TArray<FGpuInstanceRef>& OutContext);
}

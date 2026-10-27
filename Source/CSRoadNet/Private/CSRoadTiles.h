// Local updates of a road network built in square tiles of its ground plane (Unity metres, x/z).
// A road belongs to the tile its curve's middle lies in and draws there: its edge and both node
// meshes, and its decals (a junction connector goes with its source road). The graph and the GPU
// geometry still cover the whole network, which is cheap per entity; what a rebuild spends its
// time on (refinement, deformation, decals, the mesh writes) runs only for the tiles that changed.
//
// Which tiles changed comes from the geometry itself: a road whose edge and node geometry or
// compositions differ from the last build's, or a new or removed one, marks its nodes, and every
// tile holding a road at such a node is rebuilt. A road's node meshes, seams and connectors depend
// on the roads at its nodes, so those are the tiles whose output can differ; comparing the GPU
// results bit for bit catches however far a change reached through the geometry jobs.
//
// A build may write only some of the tiles to rebuild (a batch): the others stay pending, rebuilt
// by a later build whatever its comparison finds, since the roads they hold are already committed.
#pragma once

#include "CSRoadGpuMesh.h"

namespace CSRoad
{
// One road of a build, as the tile bookkeeping sees it.
struct FTileRoad
{
	// What was asked for: prefab, curve, data and both nodes' data (a repeat gets its own key).
	uint64 Key = 0;
	// What came out: edge and node geometry, compositions.
	uint64 Output = 0;
	FIntPoint Tile = FIntPoint::ZeroValue;
	// The nodes, by position.
	uint64 StartNode = 0;
	uint64 EndNode = 0;
};

struct FTilePlan
{
	// Per FGraph::Roads index.
	TArray<FTileRoad> Roads;
	// The tiles to rebuild, with the roads drawn in each (FGraph::Roads indices, in road order); an
	// empty list removes the tile. The tiles this plan's comparison found come first, then those left
	// pending by earlier batches; each part by row, then column.
	TMap<FIntPoint, TArray<int32>> Dirty;
};

class FTileState
{
public:
	// Kept between builds: the meshes of compositions FAssets has made.
	FCompositionMeshCache Compositions;

	// Signature: whatever else the meshes are made from (tile size, transform, materials, settings);
	// when it differs from the last build's, every tile is rebuilt, Existing (the tiles drawn now)
	// included.
	FTilePlan Plan(const FGraph& Graph, const FGpuGeometryResult& Geometry, double TileSize, uint64 Signature, const TArray<FIntPoint>& Existing) const;
	// The plan's roads are the network as built; of its tiles, Written were rebuilt and the others
	// stay pending.
	void Commit(const FTilePlan& Plan, uint64 Signature, const TArray<FIntPoint>& Written);
	// The next plan rebuilds everything.
	void Reset();
	int32 NumPending() const { return Pending.Num(); }

private:
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

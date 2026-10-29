// Lane and crosswalk decals on the GPU (Shaders/Private/CSRoad/CSRoadDecal.usf). They sit on the road
// surface, so they come after it: the road mesh's graph also gathers each node's receivers
// (AddDecalReceiverPasses) and finds the heights the junction connectors' and crosswalks' grids land
// on (AddDecalFitPasses), which the network actor draws its decal instances at.
#pragma once

#include "CSRoadDecals.h"

namespace CSRoad
{
// The road mesh as the decal passes read it: its typed streams (world space) in the current graph.
struct FGpuRoadStreams
{
	FRDGBufferRef Positions = nullptr;
	FRDGBufferRef Tangents = nullptr;
	FRDGBufferRef Indices = nullptr;
	// Inverse of the LocalToWorld the road mesh was written with.
	FMatrix44f WorldToLocal = FMatrix44f::Identity;
};

// What the decal passes count (CSRoadDecal.usf StatsRW).
struct FGpuDecalStats
{
	// Runs whose area was too wide to pack their candidates, so they walked the node's receivers.
	uint32 InMemoryRuns = 0;
	// Grids whose candidates did not fit, gathered again per run of vertices.
	uint32 PerRunGrids = 0;
};

struct FGpuDecalReceivers
{
	// Per FGpuDecalInput receiver slot: a road mesh triangle; per segment: how many slots it filled;
	// per chunk of slots: the union of its receivers' boxes.
	FRDGBufferRef Slots = nullptr;
	FRDGBufferRef SegmentCounts = nullptr;
	FRDGBufferRef ChunkBoxes = nullptr;
};

// Render thread, after the road mesh passes of the same graph.
FGpuDecalReceivers AddDecalReceiverPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road);

// Render thread, after the receivers: the heights the projected and clipped items' grids land on
// (FGpuDecalInput::NumFitHeights floats, Unity metres; NaN off the surface). OutStats (optional):
// the FGpuDecalStats buffer, as uint32s.
FRDGBufferRef AddDecalFitPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road, const FGpuDecalReceivers& Receivers,
	FRDGBufferRef* OutStats = nullptr);

// Game thread, tests: the road mesh, then the decals' receivers and heights (AddDecalFitPasses).
// Mesh must have its refinement.
bool RunDecalFitBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, TArray<float>& OutHeights, FString& OutError,
	FGpuDecalStats* OutStats = nullptr);

// A decal input's heights on their way back from the GPU, with the items they are for: the network
// actor draws the connectors and crosswalks once they land, a frame or two after the road.
class FDecalFitReadback
{
public:
	FDecalFitReadback();
	~FDecalFitReadback();

	// The decal input's items and decal surfaces.
	TArray<FGpuDecalItem> Items;
	TArray<FString> MaterialSlots;

	// Render thread, in the graph AddDecalFitPasses recorded Heights in: queues their copy.
	void Enqueue(FRDGBuilder& GraphBuilder, FRDGBufferRef Heights, uint32 NumHeights);
	// Game thread: whether the copy has landed or failed; looks at most once a frame, never waits.
	bool Poll();
	bool IsFailed() const;
	// Once Poll is true and the copy did not fail.
	const TArray<float>& Heights() const;

private:
	struct FShared;
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
};
}

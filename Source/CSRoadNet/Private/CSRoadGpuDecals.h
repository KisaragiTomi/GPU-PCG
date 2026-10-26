// Lane and crosswalk decals on the GPU (Shaders/Private/CSRoad/CSRoadDecal.usf). They sit on the road
// surface, so they come after it: the road mesh's graph also gathers each node's receivers and counts
// the clipped crosswalks (AddDecalReceiverPasses); once those counts are read back and laid out
// (FGpuDecalInput::ApplyClipCounts), a second graph writes the decal mesh (AddDecalMeshPasses).
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
	// Projected grids whose candidates did not fit, gathered again per run of vertices.
	uint32 PerRunGrids = 0;
	// Crosswalks whose candidates did not fit, taking the node's receivers a chunk at a time.
	uint32 PerChunkClips = 0;
};

struct FGpuDecalReceivers
{
	// Per FGpuDecalInput receiver slot: a road mesh triangle; per segment: how many slots it filled;
	// per chunk of slots: the union of its receivers' boxes.
	FRDGBufferRef Slots = nullptr;
	FRDGBufferRef SegmentCounts = nullptr;
	FRDGBufferRef ChunkBoxes = nullptr;
	// Per clipped item: its triangles (FGpuDecalInput::ClippedItems order).
	FRDGBufferRef ClipCounts = nullptr;
	// FGpuDecalStats, as uint32s.
	FRDGBufferRef Stats = nullptr;
};

// Render thread, after the road mesh passes of the same graph.
FGpuDecalReceivers AddDecalReceiverPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road);

// The receivers kept from the road mesh's graph for the decal graph.
struct FPooledDecalReceivers
{
	TRefCountPtr<FRDGPooledBuffer> Slots;
	TRefCountPtr<FRDGPooledBuffer> SegmentCounts;
	TRefCountPtr<FRDGPooledBuffer> ChunkBoxes;

	bool IsValid() const { return Slots.IsValid() && SegmentCounts.IsValid() && ChunkBoxes.IsValid(); }
	// Render thread: extracted when the graph executes.
	void Extract(FRDGBuilder& GraphBuilder, const FGpuDecalReceivers& Receivers);
	// Render thread: the receivers in another graph (without its clip counts and stats).
	FGpuDecalReceivers Register(FRDGBuilder& GraphBuilder) const;
};

// Destination of AddDecalMeshPasses: a UCSMesh with four UV sets, or DebugVertices for the tests.
struct FGpuDecalTarget
{
	FRDGBufferRef Positions = nullptr;
	FRDGBufferRef Tangents = nullptr;
	FRDGBufferRef TexCoords = nullptr;
	FRDGBufferRef Colors = nullptr;
	FRDGBufferRef Indices = nullptr;
	FRDGBufferRef MaterialIds = nullptr;
	uint32 VertexCapacity = 0;
	uint32 IndexCapacity = 0;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	// When set, Unity-space position, normal, uv, uv2 and colour (14 floats per vertex) go here
	// instead of the streams; Indices still receives the triangles.
	FRDGBufferRef DebugVertices = nullptr;
};

// Render thread. Decals must have had ApplyClipCounts; Receivers come from AddDecalReceiverPasses
// over the same road mesh (FPooledDecalReceivers::Register when that ran in another graph).
// Returns the stats buffer (FGpuDecalStats, as uint32s).
FRDGBufferRef AddDecalMeshPasses(FRDGBuilder& GraphBuilder, const FGpuRoadMeshInput& Mesh, const FGpuDecalInput& Decals, const FGpuRoadStreams& Road,
	const FGpuDecalReceivers& Receivers, const FGpuDecalTarget& Target);

// Game thread, tests: road mesh, receivers and clip counts (applied to Decals), then the decals into
// Unity-space vertices (14 floats each) and the decal index buffer. Mesh must have its refinement.
bool RunDecalsBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, FGpuDecalInput& Decals, TArray<float>& OutVertices, TArray<uint32>& OutIndices,
	FGpuDecalStats& OutStats, FString& OutError);
}

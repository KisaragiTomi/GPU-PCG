// RoadLab's lane and crosswalk decals (App/RoadDecals.cs, Port/Runtime/RoadCrosswalks.cs) as GPU
// work: the CPU walks the lane graph, the composition lanes and the crosswalks the way RoadDecals.Add
// and AddConnection do and emits one item per decal mesh; CSRoadDecal.usf projects the grids of the
// junction connectors and crosswalks onto the node surfaces (RoadSurfaceProjection), gathered on the
// GPU from the road mesh. The network actor draws the items as instanced mesh decals
// (CSRoadDecalInstances.h).
#pragma once

#include "CSRoadGpuMesh.h"
#include "CSRoadLanes.h"

namespace CSRoad
{
// RoadCrosswalks.Crossing: the crossing's lane prefab, its path and the part of it that is marked.
struct FCrossing
{
	int32 Lane = 0;
	Bezier4x3 Curve;
	Bezier4x3 MarkingCurve;
};

// RoadCrosswalks.Create(world, road, start).
void CreateCrosswalks(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry, int32 Road, bool bStart, TArray<FCrossing>& Out);

enum class EDecalKind : uint32
{
	// AddLane without a receiver: a two-column strip along the curve.
	Strip = 0,
	// AddLane onto a node surface (junction connectors): a six-column grid, each vertex projected,
	// cells with a vertex off the surface dropped.
	Projected = 1,
	// AddLane(clipToSurface) (crosswalks): RoadLab clips the footprint's two triangles to the surface;
	// here a six-column grid over the footprint, each vertex projected.
	Clipped = 2,
};

// One decal mesh along one curve (CSRoadDecal.usf FDecalItem). Unity space, metres.
struct FGpuDecalItem
{
	Bezier4x3 Curve;
	// BuildCurveMatrix's .w column: the cubic giving the tiled texture distance along the curve.
	float4 Distance;
	// DecalProperties.m_TextureArea: min.xy, max.xy.
	float4 UVArea;
	// Vertex colour r at t = 0 / 1 (endpoint weights) and g, b (connected ends).
	float2 Weights;
	float2 Connected;
	float Length = 0.0f;
	float Width = 0.0f;
	// Height above the surface: .018 + renderer priority * .0003.
	float Lift = 0.0f;
	// Vertex colour a (CurveProperties.m_SmoothingDistance).
	float Smoothing = 0.0f;
	uint32 Kind = 0;
	// Rows of the grid (a crosswalk's made a row per FGpuDecalInput::FitRowLength along it or less).
	uint32 Rows = 0;
	// Projected / Clipped: index into FGpuDecalInput::ReceiverNodes.
	uint32 Receiver = 0;
	uint32 MaterialSlot = 0;
	// Projected / Clipped: its grid's first height (of FGpuDecalInput::NumFitHeights).
	uint32 VertexOffset = 0;
};

// A run of a node's receiver triangles (CSRoadDecal.usf FReceiverSegment): the triangles of one road
// mesh instance RoadRenderer.Add puts into that node's RoadSurfaceProjection, in its order.
struct FGpuReceiverSegment
{
	// FGpuRoadMeshInput::Instances index.
	uint32 Instance = 0;
	// 0: every triangle (a node instance); 1: those with a corner within Radius of Centre (an edge's
	// end next to the node; a road with both ends on the node takes either end's).
	uint32 Filter = 0;
	// First slot in the receiver list, on a chunk (FGpuDecalInput::ReceiverChunk); the instance's
	// triangle count bounds what lands there.
	uint32 Offset = 0;
	uint32 Capacity = 0;
	float3 Centre;
	float Radius2 = 0.0f;
	float3 OtherCentre;
	uint32 bTwoCentres = 0;
};

// A part of the network's decals: the items of some roads (a connector belongs to its source road),
// projected onto receivers gathered from every road at their node as a whole build gathers them. The
// road mesh input must therefore write those roads' instances at the node too (as context ones).
struct FDecalScope
{
	// The whole network's lane graph, built once and shared; Build does not build its own.
	const FRoadLanes* Lanes = nullptr;
	// Per FGraph::Roads index: the road's items are emitted.
	const TBitArray<>* Keep = nullptr;
};

struct FGpuDecalInput
{
	// Receiver slots per chunk box (CSRoadDecal.usf ChunkBoxes): the decal passes' group size.
	static constexpr uint32 ReceiverChunk = 64;
	// A fitted grid's columns (CSRoadDecal.usf RD_FIT_COLUMNS, a projected grid's), and the most a
	// crosswalk's rows lie apart along it (metres).
	static constexpr uint32 FitColumns = 6;
	static constexpr float FitRowLength = 0.5f;

	// Built here unless a scope shares its lane graph.
	FRoadLanes Lanes;
	TArray<FGpuDecalItem> Items;
	// The RoadDecals call each item came from: its road's Add (road index), then each connector's
	// AddConnection (number of roads + connector ordinal). The golden tests compare per block.
	TArray<int32> ItemBlocks;
	int32 NumBlocks = 0;
	// Decal surface of each material slot (FGpuDecalItem::MaterialSlot).
	TArray<FString> MaterialSlots;
	// Nodes decals project onto (entities); per node its segments [first, count) in Segments.
	TArray<int32> ReceiverNodes;
	TArray<FUintVector2> ReceiverSegments;
	TArray<FGpuReceiverSegment> Segments;
	uint32 NumReceiverSlots = 0;
	uint32 NumReceiverChunks() const { return FMath::DivideAndRoundUp(NumReceiverSlots, ReceiverChunk); }
	// The heights the network actor fits its connector and crosswalk instances to (CSRoadDecal.usf
	// DecalFitCS): per projected or clipped item, from its VertexOffset on, Rows + 1 rows of
	// FitColumns + 1.
	uint32 NumFitHeights = 0;
	// Tests: every grid takes the way of a candidate list that does not fit (CSRoadDecal.usf), which
	// must give the same heights.
	bool bForceFallback = false;

	// Mesh: the road mesh input after ApplyRefinement (receiver segments index its instances).
	// AssetDirectory: where RoadDecals.Material looks for a surface's base-colour texture; surfaces
	// without one are skipped as in RoadLab. Scope (optional) limits the items to some roads.
	bool Build(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry, const FGpuRoadMeshInput& Mesh, const FString& AssetDirectory, FString& OutError,
		const FDecalScope* Scope = nullptr);

private:
	bool FitLayout(FString& OutError);
};
}

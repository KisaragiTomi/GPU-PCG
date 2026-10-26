// Road networks drawn as splines (ACSRoadSplineActor, or any USplineComponent): every spline point
// a node, every segment one edge, as a USplineComponent segment is a cubic Hermite curve and so
// exactly one cubic Bezier. Points that meet share a node, roads crossing at about the same height
// or ending on another one form junctions there, and the segments of adjustable splines are split
// where too long or turning too much and merged where too short. The result is the snapshot form
// (nodes with ids, edges referencing them) that FGraph::Build takes.
//
// ChainNetwork goes the other way: a snapshot as splines that build back into the same edges.
#pragma once

#include "CSRoadNetworkFile.h"

namespace CSRoad
{
// What a spline point says about its node and about the segment that starts there. Unset values
// are measured (elevation above the ground) or chosen as RoadWorld does (node prefab).
struct FSplinePointData
{
	// Where the node is, from the point: CS2 roads may end off their node's centre.
	FVector3d NodeOffset = FVector3d::ZeroVector;
	FString NodePrefab;
	bool bNodeElevation = false;
	float2 NodeElevation;
	CompositionFlags NodeUpgrade{};
	// The node is an outside connection: its road runs off the map, with no end there.
	bool bOutside = false;
	// What owns the node, and the segment's edge (a snapshot's building id): the geometry jobs treat
	// the roads of one owner together. 0: nothing.
	int32 NodeOwner = 0;
	int32 SegmentOwner = 0;
	bool bSegmentElevation = false;
	float2 SegmentElevation;
	// Where the segment's edge goes among the network's edges: a converted snapshot keeps its edge
	// order, which the geometry jobs follow where they break ties. Unset: after those, in spline order.
	int32 SegmentOrder = INDEX_NONE;

	bool HasNodeOverrides() const { return !NodeOffset.IsZero() || !NodePrefab.IsEmpty() || bNodeElevation || !IsDefault(NodeUpgrade) || bOutside || NodeOwner != 0; }
};

// One spline in the network's space (Unity metres, y up). Point i's segment runs to point i + 1;
// a closed spline's last one runs back to point 0.
struct FSplineRoad
{
	// For messages.
	FString Name;
	FString Prefab;
	// Every segment's upgrades.
	CompositionFlags Upgrade{};
	// Split and merge its segments, and join roads it crosses or ends on.
	bool bAdjust = true;
	bool bClosed = false;
	TArray<FVector3d> Points;
	// Hermite tangents per unit of the segment parameter, as a USplineComponent keeps them.
	TArray<FVector3d> Arrive;
	TArray<FVector3d> Leave;
	// Per segment: a straight line whatever the tangents.
	TArray<bool> Linear;
	TArray<FSplinePointData> Data;
};

struct FSplineNetworkSettings
{
	// Longer segments are split (0: never).
	double MaxEdgeLength = 96.0;
	// Segments whose heading changes by more (radians) are split (0: never).
	double MaxEdgeTurn = UE_DOUBLE_PI / 6.0;
	// Segments whose ends are closer are merged into their neighbour where the point between them is
	// a plain bend of one spline; splits never go below it.
	double MinEdgeLength = 8.0;
	// Points this close share a node, and a road ending this close to another one joins it. Points
	// of fixed splines only ever share a node within a centimetre.
	double SnapDistance = 1.0;
	// Roads crossing at most this far apart in height meet in a junction; farther, one passes over.
	double JunctionHeight = 2.0;
};

struct FSplineNetworkStats
{
	// Made where splines cross or one ends on another.
	int32 Junctions = 0;
	// Segments added by splitting for length or turn.
	int32 Splits = 0;
	// Segments merged away for being short.
	int32 Merges = 0;
	// Segments shorter than the snap distance, left out.
	int32 Dropped = 0;
};

// Terrain (optional) is the ground elevations are measured from; without it, the plane y = 0.
bool BuildSplineNetwork(FAssets& Assets, const TArray<FSplineRoad>& Splines, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FSavedNode>& OutNodes, TArray<FRoadSpec>& OutRoads, FSplineNetworkStats& OutStats, FString& OutError);

// A snapshot (nodes and the source edges referencing them) as fixed splines. Edges are chained
// where the road goes on: same prefab and upgrades, the next edge starting where the last one
// ended, and at a junction only straight on. Every node's and edge's data is kept, so the splines
// build back into the same edges.
void ChainNetwork(const TArray<FSavedNode>& Nodes, const TArray<FRoadSpec>& Roads, TArray<FSplineRoad>& OutSplines);
}

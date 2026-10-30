// Road networks drawn as splines (ACSRoadSplineActor, or any USplineComponent): every spline point
// a node, every segment one edge, as a USplineComponent segment is a cubic Hermite curve and so
// exactly one cubic Bezier. Points that meet share a node, roads crossing at about the same height
// or ending on another one form junctions there, and the segments of adjustable splines are split
// where too long or turning too much and merged where too short. The result is the snapshot form
// (nodes, and edges naming them by index) that FGraph::Build takes, the edges in an order of their
// own content: the same roads give the same network whatever order the splines come in.
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
	bool bSegmentElevation = false;
	float2 SegmentElevation;
	// An alley end cut under a main road: its node joins the road it lies on, however many roads end
	// there (two, where an alley crossing straight over meets it).
	bool bJoin = false;

	bool HasNodeOverrides() const { return !NodeOffset.IsZero() || !NodePrefab.IsEmpty() || bNodeElevation || !IsDefault(NodeUpgrade) || bOutside; }
};

// One spline in the network's space (Unity metres, y up). Point i's segment runs to point i + 1;
// a closed spline's last one runs back to point 0.
struct FSplineRoad
{
	// For messages.
	FString Name;
	// Tells the spline apart from the network's others for what derives from it (the batch buildings'
	// plots); 0 when unknown.
	uint64 Id = 0;
	FString Prefab;
	// Every segment's upgrades.
	CompositionFlags Upgrade{};
	// Split and merge its segments, and join roads it crosses or ends on.
	bool bAdjust = true;
	// An alley: where a main road (any road that is not an alley) covers it, it is cut away and its
	// ends join that road. The spline itself never changes, so the alley is back once the road goes.
	bool bAlley = false;
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
	// An alley is cut back this far from a main road's edge, past its own half width.
	double AlleyClearance = 1.0;
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
	// Stretches of alley cut away under main roads.
	int32 AlleyCuts = 0;
	// Milliseconds: ordering the splines, alleys giving way, points to nodes, roads ending on roads,
	// crossings, merging and splitting, and the snapshot form.
	double SortMs = 0.0;
	double AlleyMs = 0.0;
	double NodeMs = 0.0;
	double JoinMs = 0.0;
	double CrossMs = 0.0;
	double ShapeMs = 0.0;
	double OutputMs = 0.0;
	// With a cache (BuildSplineNetwork): whether every spline was converted (else only those around the
	// ones that changed, and the counts above are theirs), how many splines changed, how many were
	// converted and in how many rounds; milliseconds finding what changed (with every spline converted
	// after all, trying not to) and putting the edges made again among the others.
	bool bWhole = true;
	int32 Changed = 0;
	int32 Converted = 0;
	int32 Rounds = 0;
	double DiffMs = 0.0;
	double MergeMs = 0.0;
	// With bVerify: values that differ from converting every spline, which is then used instead.
	int32 Mismatches = 0;
};

// What a build changed against the one before it, by content: an edge or node that moved or changed is
// one removed and one added. Indices into this build's output (added) and the last one's (removed), in
// order.
struct FSplineNetworkDiff
{
	// Against the build before; otherwise everything is new.
	bool bValid = false;
	TArray<int32> AddedRoads;
	TArray<int32> RemovedRoads;
	TArray<int32> AddedNodes;
	TArray<int32> RemovedNodes;
};

// What a build leaves for the next one (BuildSplineNetwork with a cache): the splines, told apart by
// FSplineRoad::Id, and the network they made, so that the next build converts only around the splines
// that changed.
class FSplineNetworkCache
{
public:
	FSplineNetworkCache();
	~FSplineNetworkCache();
	void Reset();

	// What the last build changed against the one before it.
	FSplineNetworkDiff Diff;

	struct FState;
	TUniquePtr<FState> State;
};

// Terrain (optional) is the ground elevations are measured from; without it, the plane y = 0.
// Alleys give way first: the stretches a main road covers (at most JunctionHeight apart in height)
// are cut away, and the cut ends are pulled onto the main road, where they join it like any road
// ending on another one; an alley crossing straight over meets it in one crossing.
//
// Cache (optional) holds the last build. When that was of the same prefab data, settings and terrain, and
// every spline has an id of its own, only the splines around those that changed are converted again: the
// edges near the change are made again from them, the others are taken as the last build made them, and
// the network comes out as converting every spline makes it (bVerify converts every spline as well and
// compares; on a difference it logs it and keeps that conversion). The cache then holds this build, and
// its Diff what this build changed.
bool BuildSplineNetwork(FAssets& Assets, const TArray<FSplineRoad>& Splines, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FSavedNode>& OutNodes, TArray<FRoadSpec>& OutRoads, FSplineNetworkStats& OutStats, FString& OutError, FSplineNetworkCache* Cache = nullptr,
	bool bVerify = false);

// A snapshot (nodes and the source edges referencing them) as fixed splines. Edges are chained
// where the road goes on: same prefab and upgrades, the next edge starting where the last one
// ended, and at a junction only straight on. Every node's and edge's data is kept, so the splines build
// back into the same edges, in their content order rather than the snapshot's.
void ChainNetwork(const TArray<FSavedNode>& Nodes, const TArray<FRoadSpec>& Roads, TArray<FSplineRoad>& OutSplines);
}

// The directed lane graph RoadLab's lane decals are drawn along: Port/Runtime RoadLanes.cs (edge
// lanes from the road geometry, connectors at the nodes, the connection counts the decals fade by).
// What only the traffic simulation reads (speed limits, lane changes, signals, curviness) is left out.
#pragma once

#include "CSRoadGpuGeometry.h"

namespace CSRoad
{
struct FRoadLane
{
	// FGraph::Roads index. A connector carries its source lane's road and definition.
	int32 Road = INDEX_NONE;
	int32 StartNode = 0;
	int32 EndNode = 0;
	// Connectors: the node they cross (entity).
	int32 Junction = 0;
	NetCompositionLane Definition{};
	const FComposition* StartComposition = nullptr;
	const FComposition* EndComposition = nullptr;
	// Edge lanes: the start and end segment's curve in travel order; connectors: one curve.
	TArray<Bezier4x3, TInlineAllocator<2>> Curves;
	float Width = 0.0f;
	float Length = 0.0f;
	bool bConnector = false;
	bool bReverse = false;
	// Connectors: the edge lanes they join (Lanes indices).
	int32 From = INDEX_NONE;
	int32 To = INDEX_NONE;
	// Connectors: NodeLane.m_SharedStartCount / m_SharedEndCount. Edge lanes: EdgeLane.m_Connected*.
	int32 SharedStartCount = 0;
	int32 SharedEndCount = 0;
	int32 ConnectedStartCount = 0;
	int32 ConnectedEndCount = 0;
	// RoadLane._distance: cumulative polyline length, 64 steps per curve.
	TArray<float> Distances;

	void Measure();
	// RoadLane.Sample: position and unit tangent at an arc length.
	void Sample(float Distance, float3& OutPosition, float3& OutTangent) const;
};

class FRoadLanes
{
public:
	TArray<FRoadLane> Lanes;
	int32 UnsupportedRoundaboutNodes = 0;

	// Geometry: the road entities' edge and node geometry (index = entity), from the GPU geometry jobs.
	void Build(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry);
};

// RoadLanes.EdgeCurve: a lane's curve over one road segment (borders interpolated, lane height added,
// reversed for an inverted lane).
Bezier4x3 LaneEdgeCurve(const Segment& Segment, const NetCompositionLane& Lane, float Width);
}

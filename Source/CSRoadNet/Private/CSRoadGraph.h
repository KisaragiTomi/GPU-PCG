// CPU side of a road network build: world entity tables (the ECS components the jobs read)
// and composition selection. Port of RoadWorld.Build plus Game.Net.CompositionSelectSystem
// (SelectCompositionJob methods and its Execute dispatch).
#pragma once

#include "CSRoadAssets.h"

namespace CSRoad
{
// Component presence bits per world entity (HasComponent / TryGetComponent).
static const uint32 Component_Node = 1u << 0;
static const uint32 Component_Edge = 1u << 1;
static const uint32 Component_Curve = 1u << 2;
static const uint32 Component_Elevation = 1u << 3;
static const uint32 Component_Upgraded = 1u << 4;
static const uint32 Component_Owner = 1u << 5;
static const uint32 Component_OutsideConnection = 1u << 6;
static const uint32 Component_Composition = 1u << 7;

struct FSavedNode
{
	int32 Id = 0;
	FString Prefab;
	float3 Position;
	float4 Rotation = float4(0.0f, 0.0f, 0.0f, 1.0f);
	bool bHasElevation = false;
	float2 Elevation;
	CompositionFlags Upgrade{};
	bool bOutside = false;
	int32 Owner = 0;
};

// A road edge to build. Without a saved source, nodes are merged by 1 cm position like RoadWorld.Node.
struct FRoadSpec
{
	FString Prefab;
	Bezier4x3 Curve;
	CompositionFlags Upgrade{};
	bool bHasSource = false;
	int32 SourceId = 0;
	int32 SourceStart = INDEX_NONE;
	int32 SourceEnd = INDEX_NONE;
	bool bHasElevation = false;
	float2 Elevation;
	int32 Owner = 0;
};

struct FRoadInstance
{
	int32 Entity = 0;
	int32 SpecIndex = INDEX_NONE;
	FString Prefab;
	const FComposition* EdgeComposition = nullptr;
	const FComposition* StartComposition = nullptr;
	const FComposition* EndComposition = nullptr;
};

class FGraph
{
public:
	bool bLeftHandTraffic = false;

	// World entities, 1-based (0 = Entity.Null). Nodes and edges share the id space as in ECS.
	TArray<uint32> Mask;
	TArray<Node> NodeData;
	TArray<Edge> EdgeData;
	TArray<Curve> CurveData;
	TArray<Elevation> ElevationData;
	TArray<Upgraded> UpgradedData;
	TArray<Owner> OwnerData;
	TArray<Composition> CompositionData;
	TArray<int32> PrefabRef;
	TArray<NodeGeometry> InitialNodeGeometry;
	TArray<TArray<int32>> ConnectedEdges;

	TArray<int32> Nodes;
	TArray<FRoadInstance> Roads;

	// Prefab and composition tables, 1-based: PrefabRef / Composition values index them.
	TArray<FString> PrefabNames;
	TArray<int32> PrefabRoadIds;
	TArray<NetGeometryData> PrefabGeometry;
	TArray<NetData> PrefabNet;
	TArray<RoadData> PrefabRoad;
	TArray<PlaceableNetData> PrefabPlaceable;
	TArray<const FComposition*> Compositions;

	bool Build(FAssets& Assets, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& SavedNodes, FString& OutError);

	bool Has(int32 Entity, uint32 Component) const { return Entity > 0 && Entity < Mask.Num() && (Mask[Entity] & Component) != 0; }
	int32 NumEntities() const { return Mask.Num(); }

private:
	TMap<FIntVector, int32> PositionNodes;
	TMap<int32, int32> SourceEntities;
	TMap<int32, int32> SourceNodeEntities;
	TMap<FString, int32> PrefabIndices;
	TMap<const FComposition*, int32> CompositionIndices;

	int32 NewEntity();
	int32 SourceEntity(int32 Id);
	int32 Prefab(FAssets& Assets, const FString& Name);
	int32 PositionNode(float3 Position, int32 Prefab);
	int32 SourceNode(FAssets& Assets, const FSavedNode& Source);
	int32 CompositionIndex(const FComposition& Composition);
};
}

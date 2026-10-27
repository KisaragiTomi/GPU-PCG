#include "CSRoadTiles.h"

#include "Hash/xxhash.h"

namespace CSRoad
{
namespace
{
struct FCSRoadTiles_Hash
{
	FXxHash64Builder Builder;

	template<typename T>
	void Add(const T& Value) { Builder.Update(&Value, sizeof(T)); }
	void Add(const FString& Text)
	{
		Add(Text.Len());
		Builder.Update(*Text, Text.Len() * sizeof(TCHAR));
	}
	uint64 Get() const { return Builder.Finalize().Hash; }
};

// What the graph holds about a node, as it was asked for.
void CSRoadTiles_AddNode(FCSRoadTiles_Hash& Hash, const FGraph& Graph, int32 Node)
{
	Hash.Add(Graph.NodeData[Node].m_Position);
	Hash.Add(Graph.PrefabNames[Graph.PrefabRef[Node]]);
	const bool bElevation = Graph.Has(Node, Component_Elevation);
	Hash.Add(bElevation);
	if (bElevation) Hash.Add(Graph.ElevationData[Node].m_Elevation);
	const bool bUpgraded = Graph.Has(Node, Component_Upgraded);
	Hash.Add(bUpgraded);
	if (bUpgraded) Hash.Add(Graph.UpgradedData[Node].m_Flags);
	Hash.Add(Graph.Has(Node, Component_OutsideConnection));
	Hash.Add(Graph.Has(Node, Component_Owner));
}

uint64 CSRoadTiles_NodeKey(const FGraph& Graph, int32 Node)
{
	FCSRoadTiles_Hash Hash;
	Hash.Add(Graph.NodeData[Node].m_Position);
	return Hash.Get();
}
}

FTilePlan FTileState::Plan(const FGraph& Graph, const FGpuGeometryResult& Geometry, double TileSize, uint64 Signature) const
{
	FTilePlan Plan;
	Plan.Roads.SetNum(Graph.Roads.Num());
	TMap<uint64, int32> Repeats;
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++)
	{
		const FRoadInstance& Instance = Graph.Roads[Road];
		const int32 Entity = Instance.Entity;
		const Edge& RoadEdge = Graph.EdgeData[Entity];
		FTileRoad& Out = Plan.Roads[Road];

		FCSRoadTiles_Hash Asked;
		Asked.Add(Instance.Prefab);
		Asked.Add(Graph.CurveData[Entity].m_Bezier);
		const bool bElevation = Graph.Has(Entity, Component_Elevation);
		Asked.Add(bElevation);
		if (bElevation) Asked.Add(Graph.ElevationData[Entity].m_Elevation);
		const bool bUpgraded = Graph.Has(Entity, Component_Upgraded);
		Asked.Add(bUpgraded);
		if (bUpgraded) Asked.Add(Graph.UpgradedData[Entity].m_Flags);
		Asked.Add(Graph.Has(Entity, Component_Owner));
		CSRoadTiles_AddNode(Asked, Graph, RoadEdge.m_Start);
		CSRoadTiles_AddNode(Asked, Graph, RoadEdge.m_End);
		Out.Key = Asked.Get();
		// The same road twice (drawn on top of itself) still needs two keys.
		int32& Seen = Repeats.FindOrAdd(Out.Key);
		if (Seen++ > 0)
		{
			FCSRoadTiles_Hash Repeat;
			Repeat.Add(Out.Key);
			Repeat.Add(Seen);
			Out.Key = Repeat.Get();
		}

		FCSRoadTiles_Hash Made;
		Made.Add(Out.Key);
		Made.Add(Geometry.Edges[Entity]);
		Made.Add(Geometry.StartNodes[Entity]);
		Made.Add(Geometry.EndNodes[Entity]);
		Made.Add(Instance.EdgeComposition);
		Made.Add(Instance.StartComposition);
		Made.Add(Instance.EndComposition);
		Out.Output = Made.Get();

		const float3 Middle = MathUtils::Position(Graph.CurveData[Entity].m_Bezier, 0.5f);
		Out.Tile = FIntPoint(FMath::FloorToInt32(double(Middle.x) / TileSize), FMath::FloorToInt32(double(Middle.z) / TileSize));
		Out.StartNode = CSRoadTiles_NodeKey(Graph, RoadEdge.m_Start);
		Out.EndNode = CSRoadTiles_NodeKey(Graph, RoadEdge.m_End);
	}

	// Nodes where a road came out different, appeared or went away.
	const bool bEverything = !bValid || Signature != LastSignature;
	TSet<uint64> Changed;
	TSet<uint64> Present;
	for (const FTileRoad& Road : Plan.Roads)
	{
		Present.Add(Road.Key);
		const FTileRoad* Before = Last.Find(Road.Key);
		if (!bEverything && Before && Before->Output == Road.Output) continue;
		Changed.Add(Road.StartNode);
		Changed.Add(Road.EndNode);
	}
	TSet<FIntPoint> Tiles;
	for (const TPair<uint64, FTileRoad>& Pair : Last)
	{
		if (Present.Contains(Pair.Key)) continue;
		Changed.Add(Pair.Value.StartNode);
		Changed.Add(Pair.Value.EndNode);
		Tiles.Add(Pair.Value.Tile);
	}
	if (bEverything) Tiles.Append(Built);
	for (const FTileRoad& Road : Plan.Roads) if (bEverything || Changed.Contains(Road.StartNode) || Changed.Contains(Road.EndNode)) Tiles.Add(Road.Tile);

	// Every road of a tile to rebuild is drawn again.
	for (const FIntPoint& Tile : Tiles) Plan.Dirty.Add(Tile);
	Plan.Dirty.KeySort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	for (int32 Road = 0; Road < Plan.Roads.Num(); Road++)
	{
		TArray<int32>* Drawn = Plan.Dirty.Find(Plan.Roads[Road].Tile);
		if (!Drawn) continue;
		Drawn->Add(Road);
		Plan.NumDirtyRoads++;
	}
	return Plan;
}

void FTileState::Commit(const FTilePlan& Plan, uint64 Signature)
{
	Last.Reset();
	Built.Reset();
	for (const FTileRoad& Road : Plan.Roads)
	{
		Last.Add(Road.Key, Road);
		Built.Add(Road.Tile);
	}
	LastSignature = Signature;
	bValid = true;
}

void FTileState::Reset()
{
	Last.Reset();
	Built.Reset();
	LastSignature = 0;
	bValid = false;
}

void TileInstances(const FGraph& Graph, const TArray<int32>& RoadOfEntity, const TArray<int32>& Roads, TArray<FGpuInstanceRef>& OutDrawn, TArray<FGpuInstanceRef>& OutContext)
{
	OutDrawn.Reset(Roads.Num() * 3);
	OutContext.Reset();
	TBitArray<> InTile(false, Graph.Roads.Num());
	for (const int32 Road : Roads)
	{
		InTile[Road] = true;
		for (int32 Kind = 0; Kind < 3; Kind++) OutDrawn.Add({ Road, Kind });
	}
	// RoadRenderer fills a node's projection from every road there: the edge's triangles near the
	// node and the node instance's.
	TSet<int32> Slots;
	for (const int32 Road : Roads)
	{
		const Edge& RoadEdge = Graph.EdgeData[Graph.Roads[Road].Entity];
		for (const int32 Node : { RoadEdge.m_Start, RoadEdge.m_End })
		{
			for (const int32 Other : Graph.ConnectedEdges[Node])
			{
				const int32 OtherRoad = RoadOfEntity.IsValidIndex(Other) ? RoadOfEntity[Other] : INDEX_NONE;
				if (OtherRoad == INDEX_NONE || InTile[OtherRoad]) continue;
				const Edge& OtherEdge = Graph.EdgeData[Other];
				Slots.Add(OtherRoad * 3);
				if (OtherEdge.m_Start == Node) Slots.Add(OtherRoad * 3 + 1);
				if (OtherEdge.m_End == Node) Slots.Add(OtherRoad * 3 + 2);
			}
		}
	}
	TArray<int32> Sorted = Slots.Array();
	Sorted.Sort();
	OutContext.Reserve(Sorted.Num());
	for (const int32 Slot : Sorted) OutContext.Add({ Slot / 3, Slot % 3 });
}
}

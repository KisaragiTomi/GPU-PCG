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

// What a saved node asks for.
void CSRoadTiles_AddNode(FCSRoadTiles_Hash& Hash, const FSavedNode& Node)
{
	Hash.Add(Node.Prefab);
	Hash.Add(Node.Position);
	Hash.Add(Node.Rotation);
	Hash.Add(Node.bHasElevation);
	if (Node.bHasElevation) Hash.Add(Node.Elevation);
	Hash.Add(Node.Upgrade);
	Hash.Add(Node.bOutside);
	Hash.Add(Node.Owner);
}

// A node by position: a saved node's as it is; a spec's end without one as FGraph::PositionNode
// merges them, to the centimetre.
uint64 CSRoadTiles_NodeKey(const FSavedNode* Saved, float3 End)
{
	FCSRoadTiles_Hash Hash;
	Hash.Add(Saved != nullptr);
	if (Saved) Hash.Add(Saved->Position);
	else Hash.Add(FIntVector(RoundToInt(End.x * 100.0f), RoundToInt(End.y * 100.0f), RoundToInt(End.z * 100.0f)));
	return Hash.Get();
}
}

void FTileNetwork::Build(const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes, double TileSize)
{
	Roads.Reset();
	AtNode.Reset();
	OfOwner.Reset();
	Roads.SetNum(Specs.Num());
	TMap<uint64, int32> Repeats;
	for (int32 Index = 0; Index < Specs.Num(); Index++)
	{
		const FRoadSpec& Spec = Specs[Index];
		FTileRoad& Out = Roads[Index];
		const bool bSaved = Spec.bHasSource && Nodes.IsValidIndex(Spec.SourceStart) && Nodes.IsValidIndex(Spec.SourceEnd);
		const FSavedNode* Start = bSaved ? &Nodes[Spec.SourceStart] : nullptr;
		const FSavedNode* End = bSaved ? &Nodes[Spec.SourceEnd] : nullptr;

		FCSRoadTiles_Hash Asked;
		Asked.Add(Spec.Prefab);
		Asked.Add(Spec.Curve);
		Asked.Add(Spec.Upgrade);
		Asked.Add(Spec.bHasSource);
		Asked.Add(Spec.bHasElevation);
		if (Spec.bHasElevation) Asked.Add(Spec.Elevation);
		Asked.Add(Spec.Owner);
		for (const FSavedNode* Node : { Start, End }) if (Node) CSRoadTiles_AddNode(Asked, *Node);
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

		const float3 Middle = MathUtils::Position(Spec.Curve, 0.5f);
		Out.Tile = FIntPoint(FMath::FloorToInt32(double(Middle.x) / TileSize), FMath::FloorToInt32(double(Middle.z) / TileSize));
		Out.StartNode = CSRoadTiles_NodeKey(Start, Spec.Curve.a);
		Out.EndNode = CSRoadTiles_NodeKey(End, Spec.Curve.d);
		Out.Owners[0] = Spec.Owner;
		Out.Owners[1] = Start ? Start->Owner : 0;
		Out.Owners[2] = End ? End->Owner : 0;
		AtNode.FindOrAdd(Out.StartNode).Add(Index);
		if (Out.EndNode != Out.StartNode) AtNode.FindOrAdd(Out.EndNode).Add(Index);
		for (const int32 Owner : Out.Owners) if (Owner != 0) OfOwner.FindOrAdd(Owner).AddUnique(Index);
	}
}

TArray<int32> FTileNetwork::Around(const TArray<int32>& From, const TSet<uint64>& Nodes, const TSet<int32>& Owners, int32 Rings) const
{
	TBitArray<> Taken(false, Roads.Num());
	TArray<int32> Frontier;
	for (const int32 Road : From)
	{
		if (Taken[Road]) continue;
		Taken[Road] = true;
		Frontier.Add(Road);
	}
	for (int32 Ring = 0; Ring < Rings; Ring++)
	{
		TArray<int32> Next;
		auto Take = [&Taken, &Next](const TArray<int32>* Others)
		{
			if (!Others) return;
			for (const int32 Other : *Others)
			{
				if (Taken[Other]) continue;
				Taken[Other] = true;
				Next.Add(Other);
			}
		};
		for (const int32 Road : Frontier)
		{
			const FTileRoad& Here = Roads[Road];
			Take(AtNode.Find(Here.StartNode));
			Take(AtNode.Find(Here.EndNode));
			for (const int32 Owner : Here.Owners) if (Owner != 0) Take(OfOwner.Find(Owner));
		}
		// The roads at the given nodes and of the given owners are one ring from them.
		if (Ring == 0)
		{
			for (const uint64 Node : Nodes) Take(AtNode.Find(Node));
			for (const int32 Owner : Owners) Take(OfOwner.Find(Owner));
		}
		Frontier = MoveTemp(Next);
	}
	TArray<int32> Out;
	for (TConstSetBitIterator<> It(Taken); It; ++It) Out.Add(It.GetIndex());
	return Out;
}

uint64 TileOutput(const FGraph& Graph, const FGpuGeometryResult& Geometry, int32 Road, uint64 Key)
{
	const FRoadInstance& Instance = Graph.Roads[Road];
	const int32 Entity = Instance.Entity;
	FCSRoadTiles_Hash Made;
	Made.Add(Key);
	Made.Add(Geometry.Edges[Entity]);
	Made.Add(Geometry.StartNodes[Entity]);
	Made.Add(Geometry.EndNodes[Entity]);
	Made.Add(Instance.EdgeComposition);
	Made.Add(Instance.StartComposition);
	Made.Add(Instance.EndComposition);
	return Made.Get();
}

TArray<int32> FTileState::Affected(const FTileNetwork& Network) const
{
	TArray<int32> Changed;
	TSet<uint64> Present;
	for (int32 Road = 0; Road < Network.Roads.Num(); Road++)
	{
		Present.Add(Network.Roads[Road].Key);
		if (!Last.Contains(Network.Roads[Road].Key)) Changed.Add(Road);
	}
	// A road that went is gone from the network: the roads at its nodes and of its owners are the
	// ones a ring from it.
	TSet<uint64> Nodes;
	TSet<int32> Owners;
	for (const TPair<uint64, FTileRoad>& Pair : Last)
	{
		if (Present.Contains(Pair.Key)) continue;
		Nodes.Add(Pair.Value.StartNode);
		Nodes.Add(Pair.Value.EndNode);
		for (const int32 Owner : Pair.Value.Owners) if (Owner != 0) Owners.Add(Owner);
	}
	return Network.Around(Changed, Nodes, Owners, 2);
}

TMap<FIntPoint, TArray<int32>> FTileState::Dirty(const FTileNetwork& Network, const TArray<int32>& Affected, const TArray<uint64>& Outputs, bool bEverything, const TArray<FIntPoint>& Existing) const
{
	// Nodes where a road came out different, appeared or went away. The roads that are not affected
	// come out as before.
	TSet<uint64> Changed;
	for (int32 I = 0; I < Affected.Num(); I++)
	{
		const FTileRoad& Road = Network.Roads[Affected[I]];
		const FTileRoad* Before = Last.Find(Road.Key);
		if (!bEverything && Before && Before->Output == Outputs[I]) continue;
		Changed.Add(Road.StartNode);
		Changed.Add(Road.EndNode);
	}
	TSet<uint64> Present;
	for (const FTileRoad& Road : Network.Roads) Present.Add(Road.Key);
	TSet<FIntPoint> Fresh;
	for (const TPair<uint64, FTileRoad>& Pair : Last)
	{
		if (Present.Contains(Pair.Key)) continue;
		Changed.Add(Pair.Value.StartNode);
		Changed.Add(Pair.Value.EndNode);
		Fresh.Add(Pair.Value.Tile);
	}
	if (bEverything) Fresh.Append(Existing);
	for (const FTileRoad& Road : Network.Roads) if (bEverything || Changed.Contains(Road.StartNode) || Changed.Contains(Road.EndNode)) Fresh.Add(Road.Tile);

	// Every road of a tile to rebuild is drawn again. What this edit changed goes before what earlier
	// batches left, so a drag keeps up however much is pending.
	TMap<FIntPoint, TArray<int32>> Out;
	for (const FIntPoint& Tile : Fresh) Out.Add(Tile);
	for (const FIntPoint& Tile : Pending) Out.Add(Tile);
	Out.KeySort([&Fresh](const FIntPoint& A, const FIntPoint& B)
	{
		const bool bFreshA = Fresh.Contains(A);
		if (bFreshA != Fresh.Contains(B)) return bFreshA;
		return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
	});
	for (int32 Road = 0; Road < Network.Roads.Num(); Road++) if (TArray<int32>* Drawn = Out.Find(Network.Roads[Road].Tile)) Drawn->Add(Road);
	return Out;
}

void FTileState::Commit(const FTileNetwork& Network, const TArray<int32>& Affected, const TArray<uint64>& Outputs, uint64 Signature, const TMap<FIntPoint, TArray<int32>>& Dirty, const TArray<FIntPoint>& Written)
{
	TMap<uint64, FTileRoad> Next;
	Next.Reserve(Network.Roads.Num());
	for (const FTileRoad& Road : Network.Roads)
	{
		FTileRoad& Kept = Next.Add(Road.Key, Road);
		if (const FTileRoad* Before = Last.Find(Road.Key)) Kept.Output = Before->Output;
	}
	for (int32 I = 0; I < Affected.Num(); I++) Next[Network.Roads[Affected[I]].Key].Output = Outputs[I];
	Last = MoveTemp(Next);
	// The tiles not written keep whatever they drew before: rebuilt by a later batch even though the
	// roads they hold now compare equal.
	Pending.Reset();
	for (const TPair<FIntPoint, TArray<int32>>& Pair : Dirty) Pending.Add(Pair.Key);
	for (const FIntPoint& Tile : Written) Pending.Remove(Tile);
	LastSignature = Signature;
	bValid = true;
}

void FTileState::Reset()
{
	Last.Reset();
	Pending.Reset();
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

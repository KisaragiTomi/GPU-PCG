#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadGpuGeometry.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadTestUtils.h"
#include "CSRoadTiles.h"
#include "Misc/Paths.h"

using namespace CSRoad;

// How far a road's compositions and GPU geometry reach through the graph. Palmsland's roads are built
// again from sub-graphs, a sampled road with every road within H rings of it (sharing a node, or an
// owner: the geometry jobs treat one owner's roads together), the snapshot's order kept, and compared
// with the whole network built at once (without terrain, an input of each road alone). For each H it
// logs how many sampled roads come out bit for bit the same: the three compositions, the edge
// geometry, both edge-node geometries and the node geometry at both ends. Fails if a road is still off
// at the largest H.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSubgraphReachTest, "PCGPlugins.CSRoadNet.Subgraph.Reach",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSubgraphReachTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	const FString Path = FPaths::Combine(CSRoadTest::LabRoot(), TEXT("Artifacts/public-saves/palmsland-3/network.json"));
	if (!Lib.bLoaded || !FPaths::FileExists(Path))
	{
		AddWarning(TEXT("Palmsland network missing (skipped)"));
		return true;
	}
	FSavedNetwork Network;
	FString Error;
	if (!Network.Load(Path, &Lib.Assets, false, Error))
	{
		AddError(Error);
		return false;
	}
	const TArray<FRoadSpec>& Specs = Network.Roads;

	struct FBuilt
	{
		FGraph Graph;
		FGpuGeometryResult Geometry;
		// Graph.Roads index per snapshot road (INDEX_NONE: not in this build).
		TArray<int32> RoadOfSpec;
	};
	auto Build = [&](const TArray<int32>& Taken, FBuilt& Out)
	{
		TArray<FRoadSpec> Part;
		for (const int32 Spec : Taken) Part.Add(Specs[Spec]);
		Out.Graph.bLeftHandTraffic = Network.bLeftHandTraffic;
		FGpuGeometryInput Input;
		if (!Out.Graph.Build(Lib.Assets, Part, Network.Nodes, Error) || !Input.Build(Out.Graph, Lib.Assets, nullptr, Error) || !RunGeometryBlocking(Input, Out.Geometry, Error)) return false;
		Out.RoadOfSpec.Init(INDEX_NONE, Specs.Num());
		for (int32 Road = 0; Road < Out.Graph.Roads.Num(); Road++) Out.RoadOfSpec[Taken[Out.Graph.Roads[Road].SpecIndex]] = Road;
		return true;
	};
	auto Bytes = [](const auto& A, const auto& B) { return FMemory::Memcmp(&A, &B, sizeof(A)) == 0; };
	auto Same = [&Bytes](const FBuilt& A, const FBuilt& B, int32 Spec)
	{
		const int32 RoadA = A.RoadOfSpec[Spec];
		const int32 RoadB = B.RoadOfSpec[Spec];
		if (RoadA == INDEX_NONE || RoadB == INDEX_NONE) return false;
		const FRoadInstance& InA = A.Graph.Roads[RoadA];
		const FRoadInstance& InB = B.Graph.Roads[RoadB];
		if (InA.EdgeComposition != InB.EdgeComposition || InA.StartComposition != InB.StartComposition || InA.EndComposition != InB.EndComposition) return false;
		const int32 EntityA = InA.Entity;
		const int32 EntityB = InB.Entity;
		const Edge& EdgeA = A.Graph.EdgeData[EntityA];
		const Edge& EdgeB = B.Graph.EdgeData[EntityB];
		return Bytes(A.Geometry.Edges[EntityA], B.Geometry.Edges[EntityB]) && Bytes(A.Geometry.StartNodes[EntityA], B.Geometry.StartNodes[EntityB])
			&& Bytes(A.Geometry.EndNodes[EntityA], B.Geometry.EndNodes[EntityB]) && Bytes(A.Geometry.Nodes[EdgeA.m_Start], B.Geometry.Nodes[EdgeB.m_Start])
			&& Bytes(A.Geometry.Nodes[EdgeA.m_End], B.Geometry.Nodes[EdgeB.m_End]);
	};

	TArray<int32> Everything;
	for (int32 Spec = 0; Spec < Specs.Num(); Spec++) Everything.Add(Spec);
	FBuilt Whole;
	if (!TestTrue(TEXT("the whole network builds"), Build(Everything, Whole))) return false;

	// Neighbours: roads sharing a node, and roads sharing an owner (a road's, or its nodes').
	TArray<TArray<int32>> AtNode;
	AtNode.SetNum(Network.Nodes.Num());
	TMap<int32, TArray<int32>> OfOwner;
	for (int32 Spec = 0; Spec < Specs.Num(); Spec++)
	{
		const FRoadSpec& Road = Specs[Spec];
		for (const int32 Node : { Road.SourceStart, Road.SourceEnd })
		{
			AtNode[Node].Add(Spec);
			if (Network.Nodes[Node].Owner != 0) OfOwner.FindOrAdd(Network.Nodes[Node].Owner).AddUnique(Spec);
		}
		if (Road.Owner != 0) OfOwner.FindOrAdd(Road.Owner).AddUnique(Spec);
	}
	auto Within = [&](int32 Seed, int32 Rings)
	{
		TSet<int32> Taken = { Seed };
		TArray<int32> Frontier = { Seed };
		for (int32 Ring = 0; Ring < Rings; Ring++)
		{
			TArray<int32> Next;
			auto Reach = [&](const TArray<int32>& Others)
			{
				for (const int32 Other : Others)
				{
					bool bTaken = false;
					Taken.Add(Other, &bTaken);
					if (!bTaken) Next.Add(Other);
				}
			};
			for (const int32 Spec : Frontier)
			{
				for (const int32 Node : { Specs[Spec].SourceStart, Specs[Spec].SourceEnd })
				{
					Reach(AtNode[Node]);
					if (Network.Nodes[Node].Owner != 0) Reach(OfOwner.FindChecked(Network.Nodes[Node].Owner));
				}
				if (Specs[Spec].Owner != 0) Reach(OfOwner.FindChecked(Specs[Spec].Owner));
			}
			Frontier = MoveTemp(Next);
		}
		TArray<int32> Out = Taken.Array();
		Out.Sort();
		return Out;
	};

	constexpr int32 MaxRings = 4;
	constexpr int32 Samples = 192;
	TArray<int32> ExactAt;
	ExactAt.Init(0, MaxRings + 1);
	int32 Needed = 0;
	int32 Missed = 0;
	for (int32 Sample = 0; Sample < Samples; Sample++)
	{
		const int32 Seed = int32(int64(Sample) * Specs.Num() / Samples);
		int32 First = INDEX_NONE;
		for (int32 Rings = 1; Rings <= MaxRings; Rings++)
		{
			FBuilt Part;
			if (!TestTrue(FString::Printf(TEXT("road %d, %d rings: builds"), Seed, Rings), Build(Within(Seed, Rings), Part))) return false;
			const bool bSame = Same(Whole, Part, Seed);
			if (bSame) ExactAt[Rings]++;
			if (bSame && First == INDEX_NONE) First = Rings;
			if (!bSame) First = INDEX_NONE;
		}
		if (First == INDEX_NONE) Missed++;
		else Needed = FMath::Max(Needed, First);
	}
	for (int32 Rings = 1; Rings <= MaxRings; Rings++) AddInfo(FString::Printf(TEXT("%d rings: %d of %d sampled roads exact"), Rings, ExactAt[Rings], Samples));
	AddInfo(FString::Printf(TEXT("rings needed (from then on exact): %d; never exact within %d: %d"), Needed, MaxRings, Missed));
	TestEqual(TEXT("sampled roads still off at the largest sub-graph"), Missed, 0);
	return true;
}

// Every road of Palmsland, its terrain included, built again in the sub-graph a tiled rebuild draws a
// 256 m tile from: the tile's roads and two rings more (FTileNetwork::Around). Each comes out as in the
// whole network (TileOutput). A road that does not is built again alone with more and more rings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSubgraphTilesTest, "PCGPlugins.CSRoadNet.Subgraph.Tiles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSubgraphTilesTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	const FString Path = FPaths::Combine(CSRoadTest::LabRoot(), TEXT("Artifacts/public-saves/palmsland-3/network.json"));
	if (!Lib.bLoaded || !FPaths::FileExists(Path))
	{
		AddWarning(TEXT("Palmsland network missing (skipped)"));
		return true;
	}
	FSavedNetwork Network;
	FString Error;
	if (!Network.Load(Path, &Lib.Assets, true, Error))
	{
		AddError(Error);
		return false;
	}
	const TArray<FRoadSpec>& Specs = Network.Roads;
	const FSavedTerrain* Terrain = Network.Terrain.IsValid() ? &Network.Terrain : nullptr;
	FTileNetwork Tiles;
	Tiles.Build(Specs, Network.Nodes, 256.0);

	// What each snapshot road comes out as (0: not in this build).
	auto Build = [&](const TArray<int32>& Taken, TArray<uint64>& Out)
	{
		TArray<FRoadSpec> Part;
		for (const int32 Spec : Taken) Part.Add(Specs[Spec]);
		FGraph Graph;
		Graph.bLeftHandTraffic = Network.bLeftHandTraffic;
		FGpuGeometryInput Input;
		FGpuGeometryResult Geometry;
		if (!Graph.Build(Lib.Assets, Part, Network.Nodes, Error) || !Input.Build(Graph, Lib.Assets, Terrain, Error) || !RunGeometryBlocking(Input, Geometry, Error)) return false;
		Out.Init(0, Specs.Num());
		for (int32 Road = 0; Road < Graph.Roads.Num(); Road++)
		{
			const int32 Spec = Taken[Graph.Roads[Road].SpecIndex];
			Out[Spec] = TileOutput(Graph, Geometry, Road, Tiles.Roads[Spec].Key);
		}
		return true;
	};
	TArray<int32> Everything;
	for (int32 Spec = 0; Spec < Specs.Num(); Spec++) Everything.Add(Spec);
	TArray<uint64> Whole;
	if (!TestTrue(TEXT("the whole network builds"), Build(Everything, Whole))) return false;

	TMap<FIntPoint, TArray<int32>> ByTile;
	for (int32 Spec = 0; Spec < Specs.Num(); Spec++) ByTile.FindOrAdd(Tiles.Roads[Spec].Tile).Add(Spec);
	TArray<int32> Off;
	int32 Largest = 0;
	for (const TPair<FIntPoint, TArray<int32>>& Pair : ByTile)
	{
		const TArray<int32> Taken = Tiles.Around(Pair.Value, {}, {}, 2);
		Largest = FMath::Max(Largest, Taken.Num());
		TArray<uint64> Part;
		if (!TestTrue(FString::Printf(TEXT("tile %d,%d builds"), Pair.Key.X, Pair.Key.Y), Build(Taken, Part))) return false;
		for (const int32 Spec : Pair.Value) if (Part[Spec] != Whole[Spec]) Off.Add(Spec);
	}
	AddInfo(FString::Printf(TEXT("%d tiles, sub-graphs of up to %d roads: %d of %d roads off"), ByTile.Num(), Largest, Off.Num(), Specs.Num()));
	for (int32 I = 0; I < FMath::Min(Off.Num(), 12); I++)
	{
		const int32 Spec = Off[I];
		int32 Exact = INDEX_NONE;
		for (int32 Rings = 1; Rings <= 8 && Exact == INDEX_NONE; Rings++)
		{
			TArray<uint64> Part;
			if (!Build(Tiles.Around({ Spec }, {}, {}, Rings), Part)) return false;
			if (Part[Spec] == Whole[Spec]) Exact = Rings;
		}
		const FRoadSpec& Road = Specs[Spec];
		AddInfo(FString::Printf(TEXT("road %d (tile %d,%d; owner %d; nodes %d, %d with owners %d, %d): exact alone from %d rings"), Spec, Tiles.Roads[Spec].Tile.X, Tiles.Roads[Spec].Tile.Y,
			Road.Owner, Road.SourceStart, Road.SourceEnd, Network.Nodes[Road.SourceStart].Owner, Network.Nodes[Road.SourceEnd].Owner, Exact));
	}
	TestEqual(TEXT("roads off in their tile's sub-graph"), Off.Num(), 0);
	return true;
}

#endif

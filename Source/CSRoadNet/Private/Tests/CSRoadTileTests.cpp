#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadTestUtils.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
// The drawn triangles of a mesh, each as its three corners (0.1 mm) and its material, counted.
void CSRoadTileTests_Triangles(const UCSMesh* Mesh, TMap<FString, int32>& Out, int32& OutCount)
{
	FCSGpuMeshCPUData Data;
	if (!Mesh || !Mesh->ReadbackMeshSync(Data)) return;
	for (int32 Triangle = 0; Triangle + 2 < Data.Indices.Num() / 3 * 3; Triangle += 3)
	{
		TArray<FString, TInlineAllocator<3>> Corners;
		for (int32 Corner = 0; Corner < 3; Corner++)
		{
			const int32 Index = int32(Data.Indices[Triangle + Corner]);
			if (!Data.Positions.IsValidIndex(Index))
			{
				Corners.Add(TEXT("none"));
				continue;
			}
			const FVector3f& P = Data.Positions[Index];
			Corners.Add(P.ContainsNaN() ? FString(TEXT("nan")) : FString::Printf(TEXT("%lld,%lld,%lld"), FMath::RoundToInt64(P.X * 10.0f), FMath::RoundToInt64(P.Y * 10.0f), FMath::RoundToInt64(P.Z * 10.0f)));
		}
		// The winding stays; only where the corner list starts differs.
		int32 First = 0;
		for (int32 Corner = 1; Corner < 3; Corner++) if (Corners[Corner] < Corners[First]) First = Corner;
		const int32 Slot = Data.TriangleMaterialSlots.IsValidIndex(Triangle / 3) ? Data.TriangleMaterialSlots[Triangle / 3] : 0;
		const UMaterialInterface* Material = Mesh->Materials.IsValidIndex(Slot) ? Mesh->Materials[Slot].Get() : nullptr;
		Out.FindOrAdd(FString::Printf(TEXT("%s|%s|%s|%s"), *Corners[First], *Corners[(First + 1) % 3], *Corners[(First + 2) % 3], Material ? *Material->GetPathName() : TEXT("none")))++;
		OutCount++;
	}
}

struct FCSRoadTileTests_Output
{
	TMap<FString, int32> Roads;
	TMap<FString, int32> Markings;
	int32 RoadTriangles = 0;
	int32 MarkingTriangles = 0;
};

// What the network draws: its whole-build meshes, or every tile's.
FCSRoadTileTests_Output CSRoadTileTests_Read(ACSRoadNetworkActor* Network)
{
	FCSRoadTileTests_Output Out;
	TArray<UCSMeshRenderComponent*> Components;
	Network->GetComponents<UCSMeshRenderComponent>(Components);
	for (const UCSMeshRenderComponent* Component : Components)
	{
		if (!Component->IsVisible() || !Component->GetGpuMesh() || Component->GetName().StartsWith(TEXT("BuildingTile"))) continue;
		const bool bMarkings = Component == Network->MarkingMesh || Component->GetName().StartsWith(TEXT("MarkingTile"));
		CSRoadTileTests_Triangles(Component->GetGpuMesh(), bMarkings ? Out.Markings : Out.Roads, bMarkings ? Out.MarkingTriangles : Out.RoadTriangles);
	}
	return Out;
}

// Differing triangles of two counted sets, the first few logged.
int32 CSRoadTileTests_Differences(FAutomationTestBase& Test, const FString& Label, const TMap<FString, int32>& Mine, const TMap<FString, int32>& Theirs)
{
	int32 Differences = 0;
	auto Count = [&](const TMap<FString, int32>& A, const TMap<FString, int32>& B, const TCHAR* Side)
	{
		for (const TPair<FString, int32>& Pair : A)
		{
			const int32 Other = B.FindRef(Pair.Key);
			if (Pair.Value <= Other) continue;
			if (Differences < 3) Test.AddInfo(FString::Printf(TEXT("%s: %d more of %s %s"), *Label, Pair.Value - Other, *Pair.Key, Side));
			Differences += Pair.Value - Other;
		}
	};
	Count(Mine, Theirs, TEXT("in tiles"));
	Count(Theirs, Mine, TEXT("in the whole build"));
	return Differences;
}

ACSRoadSplineActor* CSRoadTileTests_Road(UWorld* World, ACSRoadNetworkActor* Network, const TArray<FVector>& Points, const FString& Prefab)
{
	ACSRoadSplineActor* Road = World->SpawnActor<ACSRoadSplineActor>();
	Road->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Road->Spline->Prefab = Prefab;
	Road->Spline->SetSplinePoints(Points, ESplineCoordinateSpace::Local);
	return Road;
}

// Three streets east to west crossed by four north to south, one of them a medium road (cm).
TArray<ACSRoadSplineActor*> CSRoadTileTests_Grid(UWorld* World, ACSRoadNetworkActor* Network)
{
	TArray<ACSRoadSplineActor*> Roads;
	for (const double Y : { -9000.0, 0.0, 9000.0 }) Roads.Add(CSRoadTileTests_Road(World, Network, { FVector(-21000.0, Y, 0.0), FVector(21000.0, Y, 0.0) }, TEXT("Small Road")));
	for (const double X : { -18000.0, -6000.0, 6000.0, 18000.0 }) Roads.Add(CSRoadTileTests_Road(World, Network, { FVector(X, -16000.0, 0.0), FVector(X, 0.0, 0.0), FVector(X + 1500.0, 16000.0, 0.0) }, X == 6000.0 ? TEXT("Medium Road") : TEXT("Small Road")));
	return Roads;
}

// The tiles draw exactly the triangles of the whole build, roads and decals alike.
void CSRoadTileTests_Compare(FAutomationTestBase& Test, const FString& Case, const FCSRoadTileTests_Output& Tiled, const FCSRoadTileTests_Output& Whole)
{
	Test.TestTrue(Case + TEXT(": the whole build has roads"), Whole.RoadTriangles > 0);
	Test.TestTrue(Case + TEXT(": the whole build has decals"), Whole.MarkingTriangles > 0);
	Test.TestEqual(Case + TEXT(": road triangles"), Tiled.RoadTriangles, Whole.RoadTriangles);
	Test.TestEqual(Case + TEXT(": decal triangles"), Tiled.MarkingTriangles, Whole.MarkingTriangles);
	Test.TestEqual(Case + TEXT(": road triangles that differ"), CSRoadTileTests_Differences(Test, Case + TEXT(" roads"), Tiled.Roads, Whole.Roads), 0);
	Test.TestEqual(Case + TEXT(": decal triangles that differ"), CSRoadTileTests_Differences(Test, Case + TEXT(" decals"), Tiled.Markings, Whole.Markings), 0);
}
}

// A network of crossing streets drawn whole and in tiles: the tiles together draw exactly the
// triangles of the whole build, roads and decals alike; an edit rebuilds only the tiles at the roads
// it changed and leaves the meshes of the others alone, and what that leaves equals a whole build of
// the edited network; a removed road takes its tile's triangles with it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadTilesActorTest, "PCGPlugins.CSRoadNet.Tiles.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRoadTilesActorTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>(FVector(40000.0, -20000.0, 0.0), FRotator(0.0, 15.0, 0.0));
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->Source = ECSRoadNetworkSource::Splines;
	Network->TileSize = 128.0f;
	// Every tile in one rebuild (Tiles.Batches covers the batches).
	Network->MaxTilesPerBatch = 0;
	const TArray<ACSRoadSplineActor*> Roads = CSRoadTileTests_Grid(World, Network);

	auto Build = [&](bool bTiles)
	{
		Network->bLocalUpdate = bTiles;
		Network->Rebuild();
		TestEqual(FString::Printf(TEXT("%s build: error"), bTiles ? TEXT("tiled") : TEXT("whole")), Network->LastError, FString());
		return CSRoadTileTests_Read(Network);
	};
	auto Compare = [this](const FString& Case, const FCSRoadTileTests_Output& Tiled, const FCSRoadTileTests_Output& Whole) { CSRoadTileTests_Compare(*this, Case, Tiled, Whole); };
	auto Generations = [&]()
	{
		TMap<FString, uint32> Out;
		TArray<UCSMeshRenderComponent*> Components;
		Network->GetComponents<UCSMeshRenderComponent>(Components);
		for (const UCSMeshRenderComponent* Component : Components) if (Component->GetName().StartsWith(TEXT("RoadTile")) && Component->GetGpuMesh()) Out.Add(Component->GetName(), Component->GetGpuMesh()->GetGeneration());
		return Out;
	};

	// Drawn whole, then in tiles.
	const FCSRoadTileTests_Output Whole = Build(false);
	const FCSRoadTileTests_Output Tiled = Build(true);
	TestTrue(TEXT("several tiles"), Network->NumTiles >= 6);
	TestEqual(TEXT("the first tiled build writes every tile"), Network->TilesRebuilt, Network->NumTiles);
	TestEqual(TEXT("the first tiled build draws every road"), Network->RoadsRebuilt, Network->NumRoads);
	Compare(TEXT("placed"), Tiled, Whole);
	AddInfo(FString::Printf(TEXT("%d roads in %d tiles: %d road and %d decal triangles"), Network->NumRoads, Network->NumTiles, Tiled.RoadTriangles, Tiled.MarkingTriangles));

	// Nothing changed: nothing rebuilt.
	const TMap<FString, uint32> Before = Generations();
	Network->Rebuild();
	TestEqual(TEXT("unchanged: tiles rebuilt"), Network->TilesRebuilt, 0);
	TestTrue(TEXT("unchanged: meshes untouched"), Generations().OrderIndependentCompareEqual(Before));

	// The north end of the easternmost street moves: the tiles around it rebuild, the others keep their meshes.
	ACSRoadSplineActor* East = Roads.Last();
	East->Spline->SetLocationAtSplinePoint(2, FVector(18000.0 + 4000.0, 15000.0, 0.0), ESplineCoordinateSpace::Local);
	Network->Tick(0.0f);
	TestEqual(TEXT("edited: error"), Network->LastError, FString());
	const int32 Rebuilt = Network->TilesRebuilt;
	TestTrue(TEXT("edited: some tiles rebuilt"), Rebuilt > 0);
	TestTrue(TEXT("edited: not every tile rebuilt"), Rebuilt < Network->NumTiles);
	const TMap<FString, uint32> After = Generations();
	int32 Changed = 0;
	for (const TPair<FString, uint32>& Pair : Before) Changed += After.FindRef(Pair.Key) != Pair.Value ? 1 : 0;
	TestTrue(TEXT("edited: some meshes changed"), Changed > 0);
	TestTrue(TEXT("edited: only rebuilt tiles changed their meshes"), Changed <= Rebuilt);
	AddInfo(FString::Printf(TEXT("edit: %d of %d tiles rebuilt (%d roads)"), Rebuilt, Network->NumTiles, Network->RoadsRebuilt));
	const FCSRoadTileTests_Output Edited = CSRoadTileTests_Read(Network);
	Compare(TEXT("edited"), Edited, Build(false));

	// A street removed: its tiles lose its triangles, what is left equals a whole build without it.
	Build(true);
	Roads[0]->Destroy();
	Network->Tick(0.0f);
	TestEqual(TEXT("removed: error"), Network->LastError, FString());
	TestTrue(TEXT("removed: not every tile rebuilt"), Network->TilesRebuilt < Network->NumTiles);
	const FCSRoadTileTests_Output Removed = CSRoadTileTests_Read(Network);
	Compare(TEXT("removed"), Removed, Build(false));
	return true;
}

// A network far wider than a sub-graph's reach: ten streets east to west crossed by ten north to
// south, one a medium road, the easternmost owned by something. Edits drawn in tiles from sub-graphs
// come out as a whole build draws them: a short spur added at the north-west corner (sub-graphs well
// short of the network, and the new node leaves the owned street's tiles alone), the westernmost
// street's north end moved (the whole street tilts), a street removed, a diagonal road added. Once
// edits settle, the traffic preview is made again for the whole network.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadTilesSubgraphTest, "PCGPlugins.CSRoadNet.Tiles.Subgraph",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRoadTilesSubgraphTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>(FVector(5000.0, -8000.0, 0.0), FRotator(0.0, 7.0, 0.0));
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->Source = ECSRoadNetworkSource::Splines;
	Network->TileSize = 96.0f;
	Network->MaxTilesPerBatch = 0;
	TArray<ACSRoadSplineActor*> EastWest;
	TArray<ACSRoadSplineActor*> NorthSouth;
	for (int32 I = 0; I < 10; I++)
	{
		const double Y = -27000.0 + I * 6000.0;
		EastWest.Add(CSRoadTileTests_Road(World, Network, { FVector(-30000.0, Y, 0.0), FVector(30000.0, Y, 0.0) }, I == 4 ? TEXT("Medium Road") : TEXT("Small Road")));
	}
	for (int32 I = 0; I < 10; I++)
	{
		const double X = -27000.0 + I * 6000.0;
		NorthSouth.Add(CSRoadTileTests_Road(World, Network, { FVector(X, -30000.0, 0.0), FVector(X, 30000.0, 0.0) }, TEXT("Small Road")));
	}
	// At x = 270 m, the column of tiles x = 2.
	NorthSouth[9]->Spline->RoadMetadata->Points[0].SegmentOwner = 77;

	auto Build = [&](bool bTiles)
	{
		Network->bLocalUpdate = bTiles;
		Network->Rebuild();
		TestEqual(FString::Printf(TEXT("%s build: error"), bTiles ? TEXT("tiled") : TEXT("whole")), Network->LastError, FString());
		return CSRoadTileTests_Read(Network);
	};
	// The mesh generations of a column of road tiles.
	auto Column = [&](int32 X)
	{
		TMap<FString, uint32> Out;
		TArray<UCSMeshRenderComponent*> Components;
		Network->GetComponents<UCSMeshRenderComponent>(Components);
		const FString Prefix = FString::Printf(TEXT("RoadTile_%d_"), X);
		for (const UCSMeshRenderComponent* Component : Components) if (Component->GetName().StartsWith(Prefix) && Component->GetGpuMesh()) Out.Add(Component->GetName(), Component->GetGpuMesh()->GetGeneration());
		return Out;
	};
	const FCSRoadTileTests_Output Whole = Build(false);
	const FCSRoadTileTests_Output Tiled = Build(true);
	TestEqual(TEXT("first tiled build: every road graphed"), Network->RoadsGraphed, Network->NumRoads);
	CSRoadTileTests_Compare(*this, TEXT("placed"), Tiled, Whole);
	AddInfo(FString::Printf(TEXT("%d roads in %d tiles"), Network->NumRoads, Network->NumTiles));

	// Each edit drawn in tiles, then the whole network built again to compare, then back to tiles.
	// bSmall: the edit keeps to the north-west corner.
	auto Edit = [&](const FString& Case, TFunctionRef<void()> Change, bool bSmall)
	{
		const TMap<FString, uint32> Owned = Column(2);
		Change();
		Network->Tick(0.0f);
		TestEqual(Case + TEXT(": error"), Network->LastError, FString());
		const int32 Graphed = Network->RoadsGraphed;
		const int32 Roads = Network->NumRoads;
		TestTrue(Case + TEXT(": some tiles rebuilt"), Network->TilesRebuilt > 0 && Network->TilesRebuilt < Network->NumTiles);
		if (bSmall)
		{
			TestTrue(Case + TEXT(": sub-graphs together short of the network"), Graphed < Roads);
			TestTrue(Case + TEXT(": the owned street's tiles"), Owned.Num() > 0);
			TestTrue(Case + TEXT(": the owned street's tiles keep their meshes"), Column(2).OrderIndependentCompareEqual(Owned));
		}
		AddInfo(FString::Printf(TEXT("%s: %d of %d tiles rebuilt, %d roads graphed of %d"), *Case, Network->TilesRebuilt, Network->NumTiles, Graphed, Roads));
		const FCSRoadTileTests_Output Edited = CSRoadTileTests_Read(Network);
		// The traffic preview, stale while edits go on, made again once they settle.
		FPlatformProcess::Sleep(0.35f);
		Network->Tick(0.0f);
		const int32 Lanes = Network->NumTrafficLanes;
		CSRoadTileTests_Compare(*this, Case, Edited, Build(false));
		TestEqual(Case + TEXT(": traffic lanes once edits settle"), Lanes, Network->NumTrafficLanes);
		Build(true);
	};
	Edit(TEXT("spur added"), [&]() { CSRoadTileTests_Road(World, Network, { FVector(-27000.0, 30000.0, 0.0), FVector(-27000.0, 34000.0, 0.0) }, TEXT("Small Road")); }, true);
	Edit(TEXT("north end moved"), [&]() { NorthSouth[0]->Spline->SetLocationAtSplinePoint(1, FVector(-27000.0 + 2000.0, 30000.0, 0.0), ESplineCoordinateSpace::Local); }, false);
	Edit(TEXT("street removed"), [&]() { EastWest[8]->Destroy(); }, false);
	Edit(TEXT("diagonal added"), [&]() { CSRoadTileTests_Road(World, Network, { FVector(9000.0, -30000.0, 0.0), FVector(30000.0, -9000.0, 0.0) }, TEXT("Small Road")); }, false);
	return true;
}

// The same network two tiles a batch: a rebuild writes two tiles and leaves the rest pending, the
// next ticks write them, and then the tiles draw what a whole build draws. A new tile size dirties
// every tile; a road moved one batch into that backlog still comes out as a whole build draws it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadTilesBatchesTest, "PCGPlugins.CSRoadNet.Tiles.Batches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRoadTilesBatchesTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>(FVector(-30000.0, 12000.0, 0.0), FRotator(0.0, -20.0, 0.0));
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->Source = ECSRoadNetworkSource::Splines;
	Network->TileSize = 128.0f;
	Network->MaxTilesPerBatch = 2;
	const TArray<ACSRoadSplineActor*> Roads = CSRoadTileTests_Grid(World, Network);

	auto Whole = [&]()
	{
		Network->bLocalUpdate = false;
		Network->Rebuild();
		TestEqual(TEXT("whole build: error"), Network->LastError, FString());
		return CSRoadTileTests_Read(Network);
	};
	// Ticks until nothing is pending, as the editor does; returns the number of batches.
	auto Finish = [&](const FString& Case)
	{
		int32 Batches = 1;
		int32 Largest = Network->TilesRebuilt;
		for (; Network->PendingTiles > 0 && Batches < 100; Batches++)
		{
			Network->Tick(0.0f);
			Largest = FMath::Max(Largest, Network->TilesRebuilt);
		}
		TestEqual(Case + TEXT(": error"), Network->LastError, FString());
		TestEqual(Case + TEXT(": tiles left pending"), Network->PendingTiles, 0);
		TestTrue(Case + TEXT(": two tiles a batch at most"), Largest <= 2);
		return Batches;
	};

	const FCSRoadTileTests_Output Reference = Whole();
	Network->bLocalUpdate = true;
	Network->Rebuild();
	TestEqual(TEXT("first batch: tiles written"), Network->TilesRebuilt, 2);
	TestTrue(TEXT("first batch: tiles left pending"), Network->PendingTiles > 0);
	const int32 Batches = Finish(TEXT("placed"));
	TestTrue(TEXT("placed: several batches"), Batches >= 3);
	AddInfo(FString::Printf(TEXT("%d tiles in %d batches"), Network->NumTiles, Batches));
	CSRoadTileTests_Compare(*this, TEXT("placed"), CSRoadTileTests_Read(Network), Reference);

	// A new tile size: the old tiles go and the new ones come, a batch at a time; one batch in, the
	// easternmost street's north end moves.
	Network->TileSize = 96.0f;
	Network->Rebuild();
	TestTrue(TEXT("resized: tiles left pending"), Network->PendingTiles > 0);
	Roads.Last()->Spline->SetLocationAtSplinePoint(2, FVector(18000.0 + 4000.0, 15000.0, 0.0), ESplineCoordinateSpace::Local);
	Finish(TEXT("resized and edited"));
	const FCSRoadTileTests_Output Edited = CSRoadTileTests_Read(Network);
	CSRoadTileTests_Compare(*this, TEXT("resized and edited"), Edited, Whole());
	return true;
}

#endif

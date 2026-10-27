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
	// Three streets east to west crossed by four north to south, one of them a medium road (cm).
	TArray<ACSRoadSplineActor*> Roads;
	for (const double Y : { -9000.0, 0.0, 9000.0 }) Roads.Add(CSRoadTileTests_Road(World, Network, { FVector(-21000.0, Y, 0.0), FVector(21000.0, Y, 0.0) }, TEXT("Small Road")));
	for (const double X : { -18000.0, -6000.0, 6000.0, 18000.0 })
	{
		Roads.Add(CSRoadTileTests_Road(World, Network, { FVector(X, -16000.0, 0.0), FVector(X, 0.0, 0.0), FVector(X + 1500.0, 16000.0, 0.0) }, X == 6000.0 ? TEXT("Medium Road") : TEXT("Small Road")));
	}

	auto Build = [&](bool bTiles)
	{
		Network->bLocalUpdate = bTiles;
		Network->Rebuild();
		TestEqual(FString::Printf(TEXT("%s build: error"), bTiles ? TEXT("tiled") : TEXT("whole")), Network->LastError, FString());
		return CSRoadTileTests_Read(Network);
	};
	auto Compare = [&](const FString& Case, const FCSRoadTileTests_Output& Tiled, const FCSRoadTileTests_Output& Whole)
	{
		TestTrue(Case + TEXT(": the whole build has roads"), Whole.RoadTriangles > 0);
		TestTrue(Case + TEXT(": the whole build has decals"), Whole.MarkingTriangles > 0);
		TestEqual(Case + TEXT(": road triangles"), Tiled.RoadTriangles, Whole.RoadTriangles);
		TestEqual(Case + TEXT(": decal triangles"), Tiled.MarkingTriangles, Whole.MarkingTriangles);
		TestEqual(Case + TEXT(": road triangles that differ"), CSRoadTileTests_Differences(*this, Case + TEXT(" roads"), Tiled.Roads, Whole.Roads), 0);
		TestEqual(Case + TEXT(": decal triangles that differ"), CSRoadTileTests_Differences(*this, Case + TEXT(" decals"), Tiled.Markings, Whole.Markings), 0);
	};
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

#endif

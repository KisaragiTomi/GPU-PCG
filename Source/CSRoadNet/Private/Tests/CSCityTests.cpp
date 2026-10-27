#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityAlleyRegion.h"
#include "CSCityBuildingActor.h"
#include "CSCityLayout.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTestUtils.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
double CSCityTests_Distance(const CSCity::FPolygon& Polygon, const FVector2D& P)
{
	if (CSCity::Contains(Polygon, P)) return 0.0;
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Polygon.Num(); I++) Best = FMath::Min(Best, FVector2D::Distance(P, FMath::ClosestPointOnSegment2D(P, Polygon[I], Polygon[(I + 1) % Polygon.Num()])));
	return Best;
}

CSCity::FPolygon CSCityTests_Box(const FVector2D& Centre, const FVector2D& Extent, double Degrees = 0.0)
{
	TArray<FVector2D> Corners;
	for (const FVector2D& Sign : { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) }) Corners.Add(Centre + (Extent * Sign).GetRotated(Degrees));
	return CSCity::ConvexHull(Corners);
}

// A straight street (cm), sampled every 8 m.
CSCity::FRoadLine CSCityTests_Street(uint64 Id, const FVector& A, const FVector& B, double HalfWidth)
{
	CSCity::FRoadLine Line;
	Line.Id = Id;
	Line.HalfWidth = HalfWidth;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector::Distance(A, B) / 800.0));
	for (int32 S = 0; S <= Steps; S++) Line.Points.Add(FMath::Lerp(A, B, double(S) / Steps));
	return Line;
}

bool CSCityTests_SameLots(const TArray<CSCity::FLot>& A, const TArray<CSCity::FLot>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 I = 0; I < A.Num(); I++) if (A[I].Id != B[I].Id || A[I].Corners != B[I].Corners || A[I].Base != B[I].Base || A[I].Height != B[I].Height) return false;
	return true;
}

// The world positions the network's batch buildings are drawn with.
TArray<FVector3f> CSCityTests_BuildingVertices(ACSRoadNetworkActor* Network)
{
	TArray<FVector3f> Out;
	TArray<UCSMeshRenderComponent*> Components;
	Network->GetComponents<UCSMeshRenderComponent>(Components);
	for (const UCSMeshRenderComponent* Component : Components)
	{
		FCSGpuMeshCPUData Data;
		if (!Component->GetName().StartsWith(TEXT("BuildingTile")) || !Component->GetGpuMesh() || !Component->GetGpuMesh()->ReadbackMeshSync(Data)) continue;
		Out.Append(Data.Positions);
	}
	return Out;
}

TMap<FString, uint32> CSCityTests_BuildingGenerations(ACSRoadNetworkActor* Network)
{
	TMap<FString, uint32> Out;
	TArray<UCSMeshRenderComponent*> Components;
	Network->GetComponents<UCSMeshRenderComponent>(Components);
	for (const UCSMeshRenderComponent* Component : Components) if (Component->GetName().StartsWith(TEXT("BuildingTile")) && Component->GetGpuMesh()) Out.Add(Component->GetName(), Component->GetGpuMesh()->GetGeneration());
	return Out;
}
}

// A region with two reserved plots, one of them across its middle: the alleys keep out of both (by
// the clearance), stay in the region and come out the same for the same inputs; without the plots
// they would have run through them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAlleysTest, "PCGPlugins.CSRoadNet.City.Alleys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAlleysTest::RunTest(const FString& Parameters)
{
	// 300 x 200 m turned by 10 degrees (cm).
	const CSCity::FPolygon Region = CSCityTests_Box(FVector2D(1000.0, -500.0), FVector2D(15000.0, 10000.0), 10.0);
	const TArray<CSCity::FPolygon> Reserved = { CSCityTests_Box(FVector2D(1000.0, -500.0), FVector2D(5000.0, 1500.0), 10.0), CSCityTests_Box(FVector2D(9000.0, 5000.0), FVector2D(1500.0, 1200.0), 35.0) };
	CSCity::FAlleyLayoutSettings Settings;
	const TArray<CSCity::FAlley> Alleys = CSCity::LayoutAlleys(Region, Reserved, Settings);
	TestTrue(TEXT("alleys laid out"), Alleys.Num() >= 6);

	TArray<CSCity::FPolygon> Kept;
	for (const CSCity::FPolygon& Plot : Reserved) Kept.Add(CSCity::Grow(Plot, Settings.Clearance - 1.0));
	auto Crossing = [&Kept](const TArray<CSCity::FAlley>& Laid)
	{
		int32 Count = 0;
		for (const CSCity::FAlley& Alley : Laid) for (const CSCity::FPolygon& Plot : Kept) Count += CSCity::SegmentHits(Plot, Alley.A, Alley.B) ? 1 : 0;
		return Count;
	};
	TestEqual(TEXT("alleys within the clearance of a reserved plot"), Crossing(Alleys), 0);
	const CSCity::FPolygon Edge = CSCity::Grow(Region, 1.0);
	int32 Outside = 0;
	for (const CSCity::FAlley& Alley : Alleys) Outside += (CSCity::Contains(Edge, Alley.A) ? 0 : 1) + (CSCity::Contains(Edge, Alley.B) ? 0 : 1);
	TestEqual(TEXT("alley ends outside the region"), Outside, 0);

	auto Same = [](const TArray<CSCity::FAlley>& A, const TArray<CSCity::FAlley>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 I = 0; I < A.Num(); I++) if (A[I].A != B[I].A || A[I].B != B[I].B) return false;
		return true;
	};
	TestTrue(TEXT("the same inputs give the same alleys"), Same(Alleys, CSCity::LayoutAlleys(Region, Reserved, Settings)));
	TestTrue(TEXT("without the plots some alley runs through them"), Crossing(CSCity::LayoutAlleys(Region, {}, Settings)) > 0);
	CSCity::FAlleyLayoutSettings Other = Settings;
	Other.Seed = 2;
	TestFalse(TEXT("another seed lays them out differently"), Same(Alleys, CSCity::LayoutAlleys(Region, Reserved, Other)));
	return true;
}

// The alleys of L_CityDemo's region as spline roads: every alley ends on another one (the boundary
// alleys close the region), so the network they make has no dead end.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAlleyJunctionsTest, "PCGPlugins.CSRoadNet.City.AlleyJunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAlleyJunctionsTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const CSCity::FPolygon Region = CSCityTests_Box(FVector2D::ZeroVector, FVector2D(15000.0, 10000.0));
	const TArray<CSCity::FPolygon> Reserved = {
		CSCity::Grow(CSCityTests_Box(FVector2D(-6000.0, 3500.0), FVector2D(2000.0, 1400.0)), 600.0),
		CSCity::Grow(CSCityTests_Box(FVector2D(8000.0, -4000.0), FVector2D(900.0, 900.0), 30.0), 500.0) };
	const TArray<CSCity::FAlley> Alleys = CSCity::LayoutAlleys(Region, Reserved, CSCity::FAlleyLayoutSettings());
	TArray<CSRoad::FSplineRoad> Splines;
	for (const CSCity::FAlley& Alley : Alleys)
	{
		// UE centimetres to the network's Unity metres, as ACSRoadNetworkActor gathers them.
		const FVector3d A(Alley.A.X / 100.0, 0.0, -Alley.A.Y / 100.0);
		const FVector3d B(Alley.B.X / 100.0, 0.0, -Alley.B.Y / 100.0);
		CSRoad::FSplineRoad& Road = Splines.AddDefaulted_GetRef();
		Road.Name = FString::Printf(TEXT("alley %d"), Splines.Num() - 1);
		Road.Prefab = TEXT("Alley");
		Road.bAlley = true;
		Road.Points = { A, B };
		Road.Arrive = { B - A, B - A };
		Road.Leave = { B - A, B - A };
		Road.Linear = { true, true };
	}
	TArray<CSRoad::FSavedNode> Nodes;
	TArray<CSRoad::FRoadSpec> Roads;
	CSRoad::FSplineNetworkStats Stats;
	FString Error;
	if (!TestTrue(TEXT("the alleys build into a network"), CSRoad::BuildSplineNetwork(Lib.Assets, Splines, CSRoad::FSplineNetworkSettings(), nullptr, Nodes, Roads, Stats, Error))) return false;
	TArray<int32> Degree;
	Degree.Init(0, Nodes.Num());
	for (const CSRoad::FRoadSpec& Road : Roads)
	{
		Degree[Road.SourceStart]++;
		Degree[Road.SourceEnd]++;
	}
	int32 DeadEnds = 0;
	for (int32 N = 0; N < Nodes.Num(); N++)
	{
		if (Degree[N] != 1) continue;
		if (DeadEnds++ < 6) AddInfo(FString::Printf(TEXT("dead end at (%.2f, %.2f) cm"), Nodes[N].Position.x * 100.0, -Nodes[N].Position.z * 100.0));
	}
	AddInfo(FString::Printf(TEXT("%d alleys: %d nodes, %d edges, %d junctions joined"), Alleys.Num(), Nodes.Num(), Roads.Num(), Stats.Junctions));
	TestEqual(TEXT("dead ends"), DeadEnds, 0);
	return true;
}

// Plots along two crossing streets: clear of the streets (by half the setback), of a reserved plot
// and of each other, and the same for the same inputs. A street added 150 m away takes plots along itself but leaves every
// plot more than 50 m from it as it was; taken away again, the first layout is back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLotsTest, "PCGPlugins.CSRoadNet.City.Lots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityLotsTest::RunTest(const FString& Parameters)
{
	const TArray<CSCity::FRoadLine> Streets = {
		CSCityTests_Street(1, FVector(-20000.0, 0.0, 0.0), FVector(20000.0, 0.0, 0.0), 400.0),
		CSCityTests_Street(2, FVector(0.0, -20000.0, 0.0), FVector(500.0, 20000.0, 0.0), 600.0) };
	const TArray<CSCity::FPolygon> Reserved = { CSCityTests_Box(FVector2D(6000.0, 2500.0), FVector2D(1500.0, 1500.0), 20.0) };
	CSCity::FLotSettings Settings;
	const TArray<CSCity::FLot> Lots = CSCity::LayoutLots(Streets, Streets, Reserved, Settings);
	TestTrue(TEXT("plots taken"), Lots.Num() >= 40);

	int32 OnStreet = 0;
	int32 OnReserved = 0;
	int32 Overlapping = 0;
	for (int32 I = 0; I < Lots.Num(); I++)
	{
		for (const CSCity::FRoadLine& Street : Streets) for (const FVector& P : Street.Points) OnStreet += CSCityTests_Distance(Lots[I].Corners, FVector2D(P)) < Street.HalfWidth + Settings.Setback * 0.5 - 1.0 ? 1 : 0;
		OnReserved += CSCity::Overlap(Lots[I].Corners, Reserved[0]) ? 1 : 0;
		for (int32 J = I + 1; J < Lots.Num(); J++) Overlapping += CSCity::Overlap(Lots[I].Corners, Lots[J].Corners) ? 1 : 0;
	}
	TestEqual(TEXT("plots within half the setback of a street"), OnStreet, 0);
	TestEqual(TEXT("plots on the reserved plot"), OnReserved, 0);
	TestEqual(TEXT("plots overlapping each other"), Overlapping, 0);
	TestTrue(TEXT("the same inputs give the same plots"), CSCityTests_SameLots(Lots, CSCity::LayoutLots(Streets, Streets, Reserved, Settings)));

	TArray<CSCity::FRoadLine> More = Streets;
	More.Add(CSCityTests_Street(3, FVector(-20000.0, 15000.0, 0.0), FVector(20000.0, 15000.0, 0.0), 600.0));
	const TArray<CSCity::FLot> After = CSCity::LayoutLots(More, More, Reserved, Settings);
	TMap<uint64, const CSCity::FLot*> ById;
	for (const CSCity::FLot& Lot : After) ById.Add(Lot.Id, &Lot);
	auto Near = [](const CSCity::FLot& Lot)
	{
		for (const FVector2D& Corner : Lot.Corners) if (FMath::Abs(Corner.Y - 15000.0) < 5000.0) return true;
		return false;
	};
	int32 Kept = 0;
	int32 Changed = 0;
	for (const CSCity::FLot& Lot : Lots)
	{
		if (Near(Lot)) continue;
		const CSCity::FLot* Same = ById.FindRef(Lot.Id);
		if (Same && Same->Corners == Lot.Corners) Kept++;
		else Changed++;
	}
	TSet<uint64> Before;
	for (const CSCity::FLot& Lot : Lots) Before.Add(Lot.Id);
	int32 Appeared = 0;
	int32 AlongNew = 0;
	for (const CSCity::FLot& Lot : After)
	{
		if (Before.Contains(Lot.Id)) continue;
		if (Near(Lot)) AlongNew++;
		else Appeared++;
	}
	TestTrue(TEXT("street added: plots away from it kept"), Kept > 20);
	TestEqual(TEXT("street added: plots away from it changed or gone"), Changed, 0);
	TestEqual(TEXT("street added: plots away from it appeared"), Appeared, 0);
	TestTrue(TEXT("street added: plots along it"), AlongNew > 10);
	TestTrue(TEXT("street taken away: the first plots are back"), CSCityTests_SameLots(Lots, CSCity::LayoutLots(Streets, Streets, Reserved, Settings)));
	return true;
}

// The city on a network built in tiles: a special building placed first, alleys laid out around it,
// batch buildings along every road. The alleys and the batch buildings keep out of the special
// building's plot; an unchanged rebuild writes no building tile; a main road drawn over a corner
// rewrites the building tiles there and leaves the others alone; the special building moved, the
// batch buildings make way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityActorTest, "PCGPlugins.CSRoadNet.City.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityActorTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->Source = ECSRoadNetworkSource::Splines;
	Network->bLocalUpdate = true;
	Network->TileSize = 128.0f;
	Network->bBatchBuildings = true;
	// Every tile in one rebuild: the counts below are per rebuild.
	Network->MaxTilesPerBatch = 0;

	ACSCityBuildingActor* Special = World->SpawnActor<ACSCityBuildingActor>(FVector(-3000.0, 2000.0, 0.0), FRotator(0.0, 20.0, 0.0));
	ACSCityAlleyRegion* Region = World->SpawnActor<ACSCityAlleyRegion>();
	if (!TestNotNull(TEXT("special building"), Special) || !TestNotNull(TEXT("alley region"), Region)) return false;
	TestTrue(TEXT("the special building belongs to the network"), ACSRoadNetworkActor::NetworkOf(Special) == Network);
	Region->Bounds->SetBoxExtent(FVector(12000.0, 8000.0, 500.0));
	Region->Generate();
	TestTrue(TEXT("alleys laid out"), Region->Generated.Num() >= 4);
	const CSCity::FPolygon AlleyKept = CSCity::Grow(Special->Reservation(), double(Region->Clearance) * 100.0 - 1.0);
	int32 Crossing = 0;
	for (const ACSRoadSplineActor* Alley : Region->Generated)
	{
		if (!Alley) continue;
		const FVector2D A(Alley->Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
		const FVector2D B(Alley->Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World));
		Crossing += CSCity::SegmentHits(AlleyKept, A, B) ? 1 : 0;
	}
	TestEqual(TEXT("alleys through the special building's plot"), Crossing, 0);

	// No batch building corner inside the special building's plot.
	auto InPlot = [&]()
	{
		const CSCity::FPolygon Plot = CSCity::Grow(Special->Reservation(), -1.0);
		int32 Count = 0;
		for (const FVector3f& P : CSCityTests_BuildingVertices(Network)) Count += CSCity::Contains(Plot, FVector2D(P.X, P.Y)) ? 1 : 0;
		return Count;
	};
	Network->Rebuild();
	TestEqual(TEXT("built: error"), Network->LastError, FString());
	TestTrue(TEXT("built: roads"), Network->NumRoads > 0);
	TestTrue(TEXT("built: batch buildings"), Network->NumBuildings > 0);
	TestTrue(TEXT("built: the special building drawn"), Special->Mesh && Special->Mesh->GetGpuMesh() != nullptr);
	TestEqual(TEXT("built: batch buildings in the special building's plot"), InPlot(), 0);
	const TMap<FString, uint32> Before = CSCityTests_BuildingGenerations(Network);
	TestTrue(TEXT("built: several building tiles"), Before.Num() >= 3);
	AddInfo(FString::Printf(TEXT("%d alleys, %d roads in %d tiles, %d batch buildings in %d tiles"), Region->Generated.Num(), Network->NumRoads, Network->NumTiles, Network->NumBuildings, Before.Num()));

	Network->Rebuild();
	TestEqual(TEXT("unchanged: building tiles rebuilt"), Network->BuildingTilesRebuilt, 0);
	TestTrue(TEXT("unchanged: building meshes untouched"), CSCityTests_BuildingGenerations(Network).OrderIndependentCompareEqual(Before));

	// A main road over the north-west corner of the region.
	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-16000.0, 3000.0, 0.0), FVector(-3000.0, 11000.0, 0.0) }, ESplineCoordinateSpace::Local);
	const int32 BuildingsBefore = Network->NumBuildings;
	Network->Tick(0.0f);
	TestEqual(TEXT("main road: error"), Network->LastError, FString());
	TestTrue(TEXT("main road: some building tiles rebuilt"), Network->BuildingTilesRebuilt > 0);
	const TMap<FString, uint32> After = CSCityTests_BuildingGenerations(Network);
	int32 Unchanged = 0;
	for (const TPair<FString, uint32>& Pair : Before) Unchanged += After.FindRef(Pair.Key) == Pair.Value ? 1 : 0;
	TestTrue(TEXT("main road: building tiles away from it untouched"), Unchanged >= 2);
	AddInfo(FString::Printf(TEXT("main road: %d -> %d batch buildings, %d building tiles rebuilt, %d of %d untouched"), BuildingsBefore, Network->NumBuildings, Network->BuildingTilesRebuilt, Unchanged, Before.Num()));

	// The special building moved east: the batch buildings make way.
	Special->SetActorLocation(FVector(6000.0, -3000.0, 0.0));
	Special->Rebuild();
	Network->Tick(0.0f);
	TestEqual(TEXT("moved: error"), Network->LastError, FString());
	TestTrue(TEXT("moved: some building tiles rebuilt"), Network->BuildingTilesRebuilt > 0);
	TestEqual(TEXT("moved: batch buildings in the special building's plot"), InPlot(), 0);

	// Two building tiles a batch: a new seed changes every building, the next ticks write the rest,
	// and they end as one rebuild with no limit draws them.
	auto Sorted = [&]()
	{
		TArray<FVector3f> Vertices = CSCityTests_BuildingVertices(Network);
		Vertices.Sort([](const FVector3f& A, const FVector3f& B) { return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z; });
		return Vertices;
	};
	Network->MaxTilesPerBatch = 2;
	Network->Lots.Seed = 7;
	Network->Rebuild();
	TestTrue(TEXT("batched: building tiles left pending"), Network->PendingTiles > 0);
	for (int32 Tick = 0; Tick < 50 && Network->PendingTiles > 0; Tick++) Network->Tick(0.0f);
	TestEqual(TEXT("batched: tiles left pending"), Network->PendingTiles, 0);
	const TArray<FVector3f> Batched = Sorted();
	Network->MaxTilesPerBatch = 0;
	Network->bBatchBuildings = false;
	Network->Rebuild();
	Network->bBatchBuildings = true;
	Network->Rebuild();
	TestTrue(TEXT("batched: the same buildings as one rebuild"), Batched.Num() > 0 && Batched == Sorted());

	// Batch buildings switched off: their tiles go.
	Network->bBatchBuildings = false;
	Network->Rebuild();
	TestEqual(TEXT("switched off: building tiles"), CSCityTests_BuildingGenerations(Network).Num(), 0);
	TestEqual(TEXT("switched off: batch buildings"), Network->NumBuildings, 0);
	return true;
}

#endif

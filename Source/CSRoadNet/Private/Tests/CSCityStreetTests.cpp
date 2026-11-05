#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityBuildingActor.h"
#include "CSCityLayout.h"
#include "CSCityStreetActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadMeshWrite.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadTestUtils.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
// How many modules the street draws (its writes landed first).
int32 CSCityStreetTests_Modules(const ACSCityStreetActor* Street)
{
	CSRoad::LandMeshWrites();
	int32 Count = 0;
	for (const UCSGpuInstancedMeshComponent* Component : Street->Facade) if (Component && Component->HasInstanceSourceGPU()) Count += Component->DebugReadDrawnInstanceCountSync();
	return Count;
}

// The storeys of each built plot, from the Plots list ("R3: 6 storeys, ...").
TArray<int32> CSCityStreetTests_Storeys(const ACSCityStreetActor* Street)
{
	TArray<int32> Out;
	for (const FString& Line : Street->Plots)
	{
		FString Name;
		FString Rest;
		if (!Line.Split(TEXT(": "), &Name, &Rest) || !Rest.Contains(TEXT(" storeys"))) continue;
		Out.Add(FCString::Atoi(*Rest));
	}
	return Out;
}

int32 CSCityStreetTests_Overlaps(const TArray<CSCity::FPolygon>& Plots)
{
	int32 Count = 0;
	for (int32 A = 0; A < Plots.Num(); A++) for (int32 B = A + 1; B < Plots.Num(); B++) Count += CSCity::Overlap(Plots[A], Plots[B]) ? 1 : 0;
	return Count;
}
}

// A street on its own (no road network): a fabric's plots on one side of its spline, kept apart, their storeys within
// the fabric's range; plots set by hand; a special building dropped on it taking its place; nothing written again when
// nothing changed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityStreetTest, "PCGPlugins.CSRoadNet.City.Street",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityStreetTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSCityStreetActor* Street = World->SpawnActor<ACSCityStreetActor>();
	if (!TestNotNull(TEXT("street"), Street)) return false;
	TestNull(TEXT("no network"), ACSRoadNetworkActor::NetworkOf(Street));
	Street->ApplyFabric(ECSCityFabric::StreetWall);
	// Every plot built, so that the overrides below are the only ones open.
	Street->Vacancy = 0.0f;
	Street->Sides = ECSCityStreetSides::Right;
	Street->StreetWidth = 10.0f;
	Street->bOnGround = false;
	Street->Street->SetSplinePoints({ FVector(0.0, 0.0, 0.0), FVector(12000.0, 0.0, 0.0) }, ESplineCoordinateSpace::World);
	Street->Rebuild();

	const TArray<CSCity::FPolygon> Plots = Street->PlotCorners();
	TestTrue(TEXT("plots"), Plots.Num() >= 6);
	TestTrue(TEXT("buildings"), Street->NumBuildings >= 5);
	// A street along +X has its right side towards +Y; the plots start the street's half width and the setback out.
	int32 Wrong = 0;
	for (const CSCity::FPolygon& Plot : Plots) for (const FVector2D& Corner : Plot) Wrong += Corner.Y < 500.0 + Street->Setback * 100.0 - 1.0 ? 1 : 0;
	TestEqual(TEXT("plot corners off the right side"), Wrong, 0);
	TestEqual(TEXT("plots overlapping"), CSCityStreetTests_Overlaps(Plots), 0);
	const TArray<int32> Storeys = CSCityStreetTests_Storeys(Street);
	TestEqual(TEXT("storeys listed for every building"), Storeys.Num(), Street->NumBuildings);
	for (const int32 Count : Storeys) TestTrue(TEXT("storeys within the fabric's range"), Count >= Street->MinStoreys && Count <= Street->MaxStoreys);
	TestTrue(TEXT("modules drawn"), CSCityStreetTests_Modules(Street) > 0);
	TestFalse(TEXT("unchanged: nothing written"), Street->UpdateFromNetwork({}, 0.0));

	// R0 left open, R1 twelve storeys.
	const int32 Before = Street->NumBuildings;
	FCSCityStreetLot& Open = Street->Overrides.AddDefaulted_GetRef();
	Open.Slot = 0;
	Open.bEmpty = true;
	FCSCityStreetLot& Tall = Street->Overrides.AddDefaulted_GetRef();
	Tall.Slot = 1;
	Tall.Storeys = 12;
	Street->Rebuild();
	TestEqual(TEXT("overrides: a building fewer"), Street->NumBuildings, Before - 1);
	TestTrue(TEXT("overrides: R0 open"), Street->Plots.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("R0: open")); }));
	TestTrue(TEXT("overrides: R1 twelve storeys"), Street->Plots.ContainsByPredicate([](const FString& Line) { return Line.StartsWith(TEXT("R1: 12 storeys")); }));
	Street->Overrides.Reset();

	// A special building in the middle of the street: the plots make way.
	ACSCityBuildingActor* Special = World->SpawnActor<ACSCityBuildingActor>(FVector(6000.0, 2000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("special building"), Special)) return false;
	Street->Rebuild();
	const CSCity::FPolygon Kept = CSCity::Grow(Special->Reservation(), -1.0);
	int32 Inside = 0;
	for (const CSCity::FPolygon& Plot : Street->PlotCorners()) Inside += CSCity::Overlap(Plot, Kept) ? 1 : 0;
	TestEqual(TEXT("special building: plots in its plot"), Inside, 0);
	TestTrue(TEXT("special building: still plots on either side"), Street->PlotCorners().Num() >= 4);

	// A taller fabric: taller buildings.
	auto Median = [](TArray<int32> Values) { Values.Sort(); return Values.Num() > 0 ? Values[Values.Num() / 2] : 0; };
	const int32 WallMedian = Median(CSCityStreetTests_Storeys(Street));
	Street->ApplyFabric(ECSCityFabric::HighCore);
	Street->Rebuild();
	const int32 CoreMedian = Median(CSCityStreetTests_Storeys(Street));
	TestTrue(TEXT("high-rise core taller than the street wall"), CoreMedian > WallMedian);
	TestTrue(TEXT("high-rise core: modules drawn"), CSCityStreetTests_Modules(Street) > 0);
	AddInfo(FString::Printf(TEXT("street wall median %d storeys, high-rise core %d; %d buildings"), WallMedian, CoreMedian, Street->NumBuildings));
	return true;
}

// A street hung under a road of a network, lining a stretch of it: its plots keep clear of the road and of a road
// crossing it, the network's batch buildings keep out of its strip and still line the rest of the road, and its centre
// line is no road.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityStreetOnRoadTest, "PCGPlugins.CSRoadNet.City.StreetOnRoad",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityStreetOnRoadTest::RunTest(const FString& Parameters)
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
	Network->bLocalUpdate = true;
	Network->TileSize = 128.0f;
	Network->bBatchBuildings = true;
	Network->MaxTilesPerBatch = 0;

	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-10000.0, 0.0, 0.0), FVector(10000.0, 0.0, 0.0) }, ESplineCoordinateSpace::Local);
	Network->Rebuild();
	TestEqual(TEXT("road: error"), Network->LastError, FString());
	const int32 BatchAlone = Network->NumBuildings;
	TestTrue(TEXT("road: batch buildings"), BatchAlone > 0);

	// The street lines the road from 50 m to 150 m, both sides.
	ACSCityStreetActor* Street = World->SpawnActor<ACSCityStreetActor>();
	if (!TestNotNull(TEXT("street"), Street)) return false;
	Street->AttachToActor(Main, FAttachmentTransformRules::KeepRelativeTransform);
	Street->ApplyFabric(ECSCityFabric::MidMixed);
	Street->Vacancy = 0.0f;
	Street->bOnGround = false;
	Street->FollowFrom = 50.0f;
	Street->FollowTo = 150.0f;
	TestTrue(TEXT("street follows the road"), Street->FollowedRoad() == Main);
	TestTrue(TEXT("street belongs to the network"), ACSRoadNetworkActor::NetworkOf(Street) == Network);
	Street->Rebuild();
	Network->Rebuild();
	TestEqual(TEXT("street: error"), Network->LastError, FString());
	TestEqual(TEXT("street: one spline road, the street's is none"), Network->NumSplines, 1);
	TestEqual(TEXT("street: one street"), Network->NumStreets, 1);
	TestTrue(TEXT("street: buildings"), Street->NumBuildings >= 6);
	TestTrue(TEXT("street: status"), Street->Status.StartsWith(TEXT("Follows")));
	TestTrue(TEXT("street: modules drawn"), CSCityStreetTests_Modules(Street) > 0);
	TestTrue(TEXT("street: batch buildings still line the rest of the road"), Network->NumBuildings > 0 && Network->NumBuildings < BatchAlone);

	const double HalfWidth = ACSRoadNetworkActor::RoadWidth(Network, Main->Spline->Prefab) * 0.5;
	TestTrue(TEXT("road width known"), HalfWidth > 0.0);
	auto Check = [&](const TCHAR* Label, double CrossX, double CrossHalfWidth)
	{
		int32 OnRoad = 0;
		int32 OffStretch = 0;
		for (const CSCity::FPolygon& Plot : Street->PlotCorners())
		{
			for (const FVector2D& Corner : Plot)
			{
				OnRoad += FMath::Abs(Corner.Y) < HalfWidth - 1.0 ? 1 : 0;
				OnRoad += CrossHalfWidth > 0.0 && FMath::Abs(Corner.X - CrossX) < CrossHalfWidth - 1.0 ? 1 : 0;
				OffStretch += Corner.X < -5000.0 - Street->Frontage * 100.0 || Corner.X > 5000.0 + Street->Frontage * 100.0 ? 1 : 0;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: plot corners on a road"), Label), OnRoad, 0);
		TestEqual(FString::Printf(TEXT("%s: plot corners off the stretch"), Label), OffStretch, 0);
		TestEqual(FString::Printf(TEXT("%s: plots overlapping"), Label), CSCityStreetTests_Overlaps(Street->PlotCorners()), 0);
		// No batch building in the street's ground.
		TArray<CSCity::FPolygon> Ground;
		for (const CSCity::FPolygon& Piece : Street->Reservations()) Ground.Add(CSCity::Grow(Piece, -1.0));
		int32 Inside = 0;
		for (const FVector3f& P : CSRoadTest::BuildingCorners(Network))
		{
			for (const CSCity::FPolygon& Piece : Ground)
			{
				if (!CSCity::Contains(Piece, FVector2D(P.X, P.Y))) continue;
				Inside++;
				break;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: batch building corners in the street's ground"), Label), Inside, 0);
	};
	Check(TEXT("street"), 0.0, 0.0);

	// A small road across the middle of the stretch: the plots make way for it.
	ACSRoadSplineActor* Cross = World->SpawnActor<ACSRoadSplineActor>();
	Cross->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Cross->Spline->Prefab = TEXT("Small Road");
	Cross->Spline->SetSplinePoints({ FVector(0.0, -8000.0, 0.0), FVector(0.0, 8000.0, 0.0) }, ESplineCoordinateSpace::Local);
	const int32 BuildingsBefore = Street->NumBuildings;
	Network->Rebuild();
	TestEqual(TEXT("crossing: error"), Network->LastError, FString());
	TestEqual(TEXT("crossing: street rebuilt"), Network->StreetsRebuilt, 1);
	Check(TEXT("crossing"), 0.0, ACSRoadNetworkActor::RoadWidth(Network, Cross->Spline->Prefab) * 0.5);

	// Nothing changed: the street is not written again.
	Network->Rebuild();
	TestEqual(TEXT("unchanged: streets rebuilt"), Network->StreetsRebuilt, 0);
	AddInfo(FString::Printf(TEXT("%d batch buildings alone, %d with the street; street %d buildings, %d across the small road"), BatchAlone, Network->NumBuildings, BuildingsBefore, Street->NumBuildings));
	return true;
}

#endif

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityBuildingActor.h"
#include "CSCityGarage.h"
#include "CSCityLayout.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTestUtils.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
// A straight road of the network's space (CS2's metres: x right, z forward).
CSRoad::FSplineRoad CSCityGarageTests_Road(const TCHAR* Name, const TCHAR* Prefab, const FVector3d& A, const FVector3d& B)
{
	CSRoad::FSplineRoad Road;
	Road.Name = Name;
	Road.Prefab = Prefab;
	Road.Points = { A, B };
	Road.Arrive = { B - A, B - A };
	Road.Leave = { B - A, B - A };
	Road.Linear = { true, true };
	Road.Data.SetNum(2);
	return Road;
}

// A box in the network's ground plane (x, z).
TArray<FVector2d> CSCityGarageTests_Box(const FVector2d& Centre, const FVector2d& Half)
{
	return { Centre + FVector2d(-Half.X, -Half.Y), Centre + FVector2d(Half.X, -Half.Y), Centre + FVector2d(Half.X, Half.Y), Centre + FVector2d(-Half.X, Half.Y) };
}

// The nodes of a network with Degree edges within Within (m) of P in the ground plane.
int32 CSCityGarageTests_NodesNear(const TArray<CSRoad::FSavedNode>& Nodes, const TArray<CSRoad::FRoadSpec>& Roads, const FVector2d& P, double Within, int32 Degree)
{
	TArray<int32> Edges;
	Edges.Init(0, Nodes.Num());
	for (const CSRoad::FRoadSpec& Road : Roads)
	{
		Edges[Road.SourceStart]++;
		Edges[Road.SourceEnd]++;
	}
	int32 Found = 0;
	for (int32 N = 0; N < Nodes.Num(); N++) Found += Edges[N] == Degree && FVector2d::Distance(FVector2d(Nodes[N].Position.x, Nodes[N].Position.z), P) <= Within ? 1 : 0;
	return Found;
}

FVector CSCityGarageTests_ToUnreal(const FVector3d& V)
{
	return FVector(V.X, -V.Z, V.Y) * 100.0;
}
}

// A garage 33 m north of a 200 m road, its door facing it: a driveway from the door straight to the road's centre line,
// leaving square and meeting square, which the network joins in a T junction there (and leaves a dead end at the door).
// Facing away, or reaching 30 m, it joins nothing; a building beside the way blocks it; a door 9 m from the centre line
// opens onto the road (no driveway); of two roads in front it takes the nearer.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityGarageTest, "PCGPlugins.CSRoadNet.City.Garage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityGarageTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const CSRoad::FSplineRoad Main = CSCityGarageTests_Road(TEXT("Main"), TEXT("Medium Road"), FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0));
	TArray<CSCity::FObstacle> Obstacles;
	Obstacles.Add({ TEXT("House"), CSCityGarageTests_Box(FVector2d(0.0, 40.0), FVector2d(10.0, 7.0)) });
	CSCity::FGarage Garage;
	Garage.Name = TEXT("House driveway");
	Garage.Id = 77;
	Garage.Prefab = TEXT("Alley");
	Garage.Door = FVector3d(5.0, 0.0, 33.0);
	Garage.Facing = FVector2d(0.0, -1.0);
	Garage.Own = 0;

	CSRoad::FSplineRoad Driveway;
	FString Why;
	if (!TestTrue(TEXT("facing the road: joined"), CSCity::Driveway(Garage, { Main }, Obstacles, Lib.Assets, Driveway, Why))) return false;
	AddInfo(Why);
	TestTrue(TEXT("facing the road: says which"), Why.StartsWith(TEXT("joins Main")));
	if (!TestEqual(TEXT("facing the road: driveway points"), Driveway.Points.Num(), 2)) return false;
	TestTrue(TEXT("driveway: from the door"), Driveway.Points[0].Equals(Garage.Door, 1e-6));
	TestTrue(TEXT("driveway: to the foot on the centre line"), Driveway.Points[1].Equals(FVector3d(5.0, 0.0, 0.0), 1e-3));
	TestTrue(TEXT("driveway: an alley without plots"), Driveway.bAlley && !Driveway.bPlots && Driveway.Id == 77 && Driveway.Prefab == TEXT("Alley"));
	TestTrue(TEXT("driveway: leaves the door square"), FVector3d(Driveway.Leave[0]).GetSafeNormal().Equals(FVector3d(0.0, 0.0, -1.0), 1e-6));
	TestTrue(TEXT("driveway: meets the road square"), FVector3d(Driveway.Arrive[1]).GetSafeNormal().Equals(FVector3d(0.0, 0.0, -1.0), 1e-3));

	TArray<CSRoad::FSavedNode> Nodes;
	TArray<CSRoad::FRoadSpec> Roads;
	CSRoad::FSplineNetworkStats Stats;
	FString Error;
	// Segments not split for length: the edges counted are the junction's.
	CSRoad::FSplineNetworkSettings Settings;
	Settings.MaxEdgeLength = 0.0;
	if (!TestTrue(TEXT("road and driveway build into a network"), CSRoad::BuildSplineNetwork(Lib.Assets, { Main, Driveway }, Settings, nullptr, Nodes, Roads, Stats, Error))) return false;
	TestEqual(TEXT("network: edges (the road split by the junction, and the driveway)"), Roads.Num(), 3);
	TestEqual(TEXT("network: T junction on the centre line at the foot"), CSCityGarageTests_NodesNear(Nodes, Roads, FVector2d(5.0, 0.0), 0.5, 3), 1);
	TestEqual(TEXT("network: dead end at the door"), CSCityGarageTests_NodesNear(Nodes, Roads, FVector2d(5.0, 33.0), 0.5, 1), 1);

	CSCity::FGarage Away = Garage;
	Away.Facing = FVector2d(0.0, 1.0);
	TestFalse(TEXT("facing away: not joined"), CSCity::Driveway(Away, { Main }, Obstacles, Lib.Assets, Driveway, Why));
	TestEqual(TEXT("facing away: why"), Why, FString(TEXT("no main road within 50 m in front of the garage")));
	CSCity::FGarage Short = Garage;
	Short.Reach = 30.0;
	TestFalse(TEXT("reaching 30 m: not joined"), CSCity::Driveway(Short, { Main }, Obstacles, Lib.Assets, Driveway, Why));
	TestEqual(TEXT("reaching 30 m: why"), Why, FString(TEXT("no main road within 30 m in front of the garage")));

	TArray<CSCity::FObstacle> Shed = Obstacles;
	Shed.Add({ TEXT("Shed"), CSCityGarageTests_Box(FVector2d(8.0, 18.0), FVector2d(2.0, 2.0)) });
	TestFalse(TEXT("a shed beside the way, within half the driveway's width: blocked"), CSCity::Driveway(Garage, { Main }, Shed, Lib.Assets, Driveway, Why));
	TestEqual(TEXT("blocked: why"), Why, FString(TEXT("the way to Main runs through Shed")));
	Shed.Last().Outline = CSCityGarageTests_Box(FVector2d(15.0, 18.0), FVector2d(2.0, 2.0));
	TestTrue(TEXT("the shed farther off: joined"), CSCity::Driveway(Garage, { Main }, Shed, Lib.Assets, Driveway, Why) && Driveway.Points.Num() == 2);

	CSCity::FGarage Near = Garage;
	Near.Door = FVector3d(5.0, 0.0, 9.0);
	TArray<CSCity::FObstacle> NearHouse;
	NearHouse.Add({ TEXT("House"), CSCityGarageTests_Box(FVector2d(0.0, 16.0), FVector2d(10.0, 7.0)) });
	TestTrue(TEXT("9 m from the centre line: joined"), CSCity::Driveway(Near, { Main }, NearHouse, Lib.Assets, Driveway, Why));
	TestEqual(TEXT("9 m from the centre line: no driveway"), Driveway.Points.Num(), 0);
	TestTrue(TEXT("9 m from the centre line: opens onto the road"), Why.StartsWith(TEXT("opens onto Main")));

	// A second road 25 m south of the first, and one behind the house: the first is nearer in front.
	const CSRoad::FSplineRoad South = CSCityGarageTests_Road(TEXT("South"), TEXT("Medium Road"), FVector3d(-100.0, 0.0, -25.0), FVector3d(100.0, 0.0, -25.0));
	const CSRoad::FSplineRoad North = CSCityGarageTests_Road(TEXT("North"), TEXT("Medium Road"), FVector3d(-100.0, 0.0, 60.0), FVector3d(100.0, 0.0, 60.0));
	TestTrue(TEXT("three roads: joined"), CSCity::Driveway(Garage, { South, North, Main }, Obstacles, Lib.Assets, Driveway, Why) && Why.StartsWith(TEXT("joins Main")));
	return true;
}

// A city building with a garage 33 m from a main road, on a network built in tiles: the network joins it with a driveway
// (a T junction on the road, the main road split there) and says so; batch buildings keep off the driveway and stand along
// the road but not along it. The building moved, the junction follows; the garage turned to face away, the driveway goes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityGarageNetworkTest, "PCGPlugins.CSRoadNet.City.GarageNetwork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityGarageNetworkTest::RunTest(const FString& Parameters)
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
	Network->bBakeNanite = false;
	Network->MaxTilesPerBatch = 0;
	// Segments not split for length: the roads counted are the junction's.
	Network->SplineSettings.MaxSegmentLength = 0.0f;
	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	if (!TestNotNull(TEXT("main road"), Main)) return false;
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-10000.0, 0.0, 0.0), FVector(10000.0, 0.0, 0.0) }, ESplineCoordinateSpace::Local);
	// 20 x 14 m, its south side 33 m north of the road (y < 0 is north here: CS2's z is -y), the garage in its middle.
	ACSCityBuildingActor* House = World->SpawnActor<ACSCityBuildingActor>(FVector(2000.0, -4000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("house"), House)) return false;
	House->bHasGarage = true;
	House->GarageLocation = FVector(0.0, 700.0, 0.0);
	House->Rebuild();

	// The driveway's centre line, world space, and the T junctions at about a point.
	auto Driveway = [&]()
	{
		TArray<FVector2D> Out;
		const TArray<CSRoad::FSplineRoad>* Splines = Network->SplinesForTest();
		if (!Splines || Splines->Num() < 2 || Splines->Last().bPlots) return Out;
		const CSRoad::FSplineRoad& Road = Splines->Last();
		for (int32 K = 0; K <= 64; K++)
		{
			const double T = K / 64.0;
			const double U = 1.0 - T;
			const FVector3d A = Road.Points[0];
			const FVector3d D = Road.Points[1];
			const FVector3d B = A + Road.Leave[0] / 3.0;
			const FVector3d C = D - Road.Arrive[1] / 3.0;
			Out.Add(FVector2D(CSCityGarageTests_ToUnreal(A * (U * U * U) + B * (3.0 * U * U * T) + C * (3.0 * U * T * T) + D * (T * T * T))));
		}
		return Out;
	};
	auto JunctionsAt = [&](const FVector2D& World)
	{
		const CSRoad::FSavedNetwork* Built = Network->RoadsForTest();
		return Built ? CSCityGarageTests_NodesNear(Built->Nodes, Built->Roads, FVector2d(World.X / 100.0, -World.Y / 100.0), 0.5, 3) : -1;
	};
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("built: error"), Network->LastError, FString());
	TestEqual(TEXT("built: garages"), Network->NumGarages, 1);
	TestEqual(TEXT("built: joined"), Network->NumJoinedGarages, 1);
	AddInfo(House->GarageStatus);
	TestTrue(TEXT("built: the house says which road"), House->GarageStatus.StartsWith(FString::Printf(TEXT("joins %s"), *Main->GetActorNameOrLabel())));
	TestEqual(TEXT("built: roads (the main road split, and the driveway)"), Network->NumRoads, 3);
	TestEqual(TEXT("built: T junction below the garage"), JunctionsAt(FVector2D(2000.0, 0.0)), 1);
	const TArray<FVector2D> Way = Driveway();
	if (!TestEqual(TEXT("built: driveway samples"), Way.Num(), 65)) return false;
	TestTrue(TEXT("built: driveway from the door"), FVector2D::Distance(Way[0], FVector2D(2000.0, -3300.0)) < 1.0);

	// Batch buildings: along the road, none on the driveway (within its half width and a metre), none along it.
	Network->bBatchBuildings = true;
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("buildings: error"), Network->LastError, FString());
	const int32 WithDriveway = Network->NumBuildings;
	TestTrue(TEXT("buildings: some"), WithDriveway > 0);
	const double Keep = ACSRoadNetworkActor::RoadWidth(Network, TEXT("Alley")) * 0.5 + 100.0;
	int32 OnWay = 0;
	for (const FVector3f& P : CSRoadTest::BuildingCorners(Network))
	{
		for (int32 K = 0; K + 1 < Way.Num(); K++)
		{
			if (FVector2D::Distance(FVector2D(P.X, P.Y), FMath::ClosestPointOnSegment2D(FVector2D(P.X, P.Y), Way[K], Way[K + 1])) >= Keep) continue;
			OnWay++;
			break;
		}
	}
	TestEqual(TEXT("buildings: corners on the driveway"), OnWay, 0);
	House->bHasGarage = false;
	House->Rebuild();
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("no garage: roads"), Network->NumRoads, 1);
	AddInfo(FString::Printf(TEXT("batch buildings: %d with the driveway, %d without"), WithDriveway, Network->NumBuildings));
	TestTrue(TEXT("buildings: the driveway takes plots away and brings none"), WithDriveway <= Network->NumBuildings);
	House->bHasGarage = true;

	// Moved 30 m east: the junction goes with it.
	House->SetActorLocation(FVector(5000.0, -4000.0, 0.0));
	House->Rebuild();
	CSRoadTest::TickBuild(Network);
	Network->FinishWrites();
	TestEqual(TEXT("moved: error"), Network->LastError, FString());
	TestEqual(TEXT("moved: T junction below the garage"), JunctionsAt(FVector2D(5000.0, 0.0)), 1);
	TestEqual(TEXT("moved: none where it was"), JunctionsAt(FVector2D(2000.0, 0.0)), 0);
	TestEqual(TEXT("moved: roads"), Network->NumRoads, 3);

	// The garage on the far side, facing away from the road.
	House->GarageLocation = FVector(0.0, -700.0, 0.0);
	House->Rebuild();
	CSRoadTest::TickBuild(Network);
	Network->FinishWrites();
	TestEqual(TEXT("facing away: joined"), Network->NumJoinedGarages, 0);
	TestEqual(TEXT("facing away: why"), House->GarageStatus, FString(TEXT("no main road within 50 m in front of the garage")));
	TestEqual(TEXT("facing away: roads"), Network->NumRoads, 1);
	return true;
}

#endif

#include "CSCityAlleyRegion.h"

#include "CSCityBuildingActor.h"
#include "CSCityLayout.h"
#include "CSCityStreetActor.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

ACSCityAlleyRegion::ACSCityAlleyRegion()
{
	PrimaryActorTick.bCanEverTick = false;
	Bounds = CreateDefaultSubobject<UBoxComponent>(TEXT("Bounds"));
	RootComponent = Bounds;
	Bounds->SetBoxExtent(FVector(20000.0, 15000.0, 500.0));
	Bounds->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Bounds->ShapeColor = FColor(80, 200, 255);
}

TArray<FString> ACSCityAlleyRegion::GetRoadPrefabOptions() const
{
	return ACSRoadNetworkActor::RoadPrefabNames(ACSRoadNetworkActor::NetworkOf(this));
}

void ACSCityAlleyRegion::Generate()
{
	UWorld* World = GetWorld();
	ACSRoadNetworkActor* Network = ACSRoadNetworkActor::NetworkOf(this);
	if (!World || !Network)
	{
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: no road network building from splines to lay alleys out for"), *GetName());
		return;
	}
#if WITH_EDITOR
	const FScopedTransaction Transaction(NSLOCTEXT("CSRoadNet", "GenerateAlleys", "Generate Alleys"));
#endif
	Modify();
	for (ACSRoadSplineActor* Old : Generated) if (IsValid(Old)) Old->Destroy();
	Generated.Reset();

	// The box's rectangle from above, and the plots reserved in it.
	const FTransform Placement = Bounds->GetComponentTransform();
	const FVector Extent = Bounds->GetUnscaledBoxExtent();
	TArray<FVector2D> Corners;
	for (const FVector2D& Sign : { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) }) Corners.Add(FVector2D(Placement.TransformPosition(FVector(Extent.X * Sign.X, Extent.Y * Sign.Y, 0.0))));
	const CSCity::FPolygon Region = CSCity::ConvexHull(Corners);
	TArray<CSCity::FPolygon> Reserved;
	for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
	{
		if (Building->GetLevel() != GetLevel()) continue;
		const CSCity::FPolygon Plot = Building->Reservation();
		if (Plot.Num() >= 3 && CSCity::Overlap(Plot, Region)) Reserved.Add(Plot);
	}
	// And the ground the city streets keep.
	for (ACSCityStreetActor* Street : TActorRange<ACSCityStreetActor>(World))
	{
		if (Street->GetLevel() != GetLevel()) continue;
		for (const CSCity::FPolygon& Kept : Street->Reservations()) if (Kept.Num() >= 3 && CSCity::Overlap(Kept, Region)) Reserved.Add(Kept);
	}
	CSCity::FAlleyLayoutSettings Settings;
	Settings.BlockSize = double(BlockSize) * 100.0;
	Settings.MinBlockSize = double(MinBlockSize) * 100.0;
	Settings.Jitter = Jitter;
	Settings.TurnJitter = TurnJitter;
	Settings.Clearance = double(Clearance) * 100.0;
	Settings.bBoundary = bBoundaryAlleys;
	Settings.Seed = Seed;

	// The small roads already there: an alley laid out under one is cut back to it for good and joins it,
	// both being small roads laid out once. Main roads cut the alleys only while they lie over them
	// (CSRoadSplineNetwork), so that moving one away gives the alleys back.
	const double Height = GetActorLocation().Z;
	const double Reach = ACSRoadNetworkActor::RoadWidth(Network, AlleyPrefab) * 0.5 + double(Network->SplineSettings.AlleyClearance) * 100.0;
	FBox2D Area(ForceInit);
	for (const FVector2D& Corner : Region) Area += Corner;
	Area = Area.ExpandBy(10000.0);
	TArray<CSCity::FRoadLine> SmallRoads;
	for (const ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World))
	{
		const UCSRoadSplineComponent* Spline = Road->Spline;
		if (!Spline || Spline->Tier != ECSRoadTier::Alley || Road->GetLevel() != GetLevel()) continue;
		FBox2D Box(ForceInit);
		for (int32 Point = 0; Point < Spline->GetNumberOfSplinePoints(); Point++) Box += FVector2D(Spline->GetLocationAtSplinePoint(Point, ESplineCoordinateSpace::World));
		const double Width = ACSRoadNetworkActor::RoadWidth(Network, Spline->Prefab);
		if (!Box.bIsValid || !Box.Intersect(Area) || Width <= 0.0 || ACSRoadNetworkActor::NetworkOf(Road) != Network) continue;
		CSCity::FRoadLine& Line = SmallRoads.AddDefaulted_GetRef();
		Line.HalfWidth = Width * 0.5;
		const double Length = Spline->GetSplineLength();
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32(Length / 200.0), 1, 4096);
		for (int32 Step = 0; Step <= Steps; Step++) Line.Points.Add(Spline->GetLocationAtDistanceAlongSpline(Length * Step / Steps, ESplineCoordinateSpace::World));
	}
	const TArray<CSCity::FAlley> Alleys = CSCity::CutAlleys(CSCity::LayoutAlleys(Region, Reserved, Settings), SmallRoads, Reach, Height, double(Network->SplineSettings.JunctionHeight) * 100.0, Settings.MinBlockSize * 0.25);

	// Each alley a straight spline road under the network, at the region's height.
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetLevel();
	for (int32 I = 0; I < Alleys.Num(); I++)
	{
		const FVector A(Alleys[I].A, Height);
		const FVector B(Alleys[I].B, Height);
		ACSRoadSplineActor* Road = World->SpawnActor<ACSRoadSplineActor>(ACSRoadSplineActor::StaticClass(), FTransform(A), Parameters);
		if (!Road || !Road->Spline) continue;
		Road->AttachToActor(Network, FAttachmentTransformRules::KeepWorldTransform);
		UCSRoadSplineComponent* Spline = Road->Spline;
		Spline->Prefab = AlleyPrefab;
		Spline->Tier = ECSRoadTier::Alley;
		Spline->ClearSplinePoints(false);
		Spline->AddSplinePoint(A, ESplineCoordinateSpace::World, false);
		Spline->AddSplinePoint(B, ESplineCoordinateSpace::World, false);
		for (int32 Point = 0; Point < 2; Point++) Spline->SetSplinePointType(Point, ESplinePointType::Linear, false);
		Spline->UpdateSpline();
#if WITH_EDITOR
		Road->SetActorLabel(FString::Printf(TEXT("%s_Alley%02d"), *GetActorLabel(), I));
#endif
		Generated.Add(Road);
	}
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d alleys around %d reserved plots"), *GetName(), Generated.Num(), Reserved.Num());
	Network->RequestRebuild();
}

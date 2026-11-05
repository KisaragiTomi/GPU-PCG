#include "CSCityBuildingActor.h"

#include "CSCityFacade.h"
#include "CSCityFacadeKit.h"
#include "CSCityLayout.h"
#include "CSCityStreetActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadNetworkActor.h"
#include "Hash/xxhash.h"

UCSCityOutlineComponent::UCSCityOutlineComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A 20 x 14 m rectangle to start from.
	ClearSplinePoints(false);
	for (const FVector& Corner : { FVector(-1000.0, -700.0, 0.0), FVector(1000.0, -700.0, 0.0), FVector(1000.0, 700.0, 0.0), FVector(-1000.0, 700.0, 0.0) })
	{
		AddSplinePoint(Corner, ESplineCoordinateSpace::Local, false);
		SetSplinePointType(GetNumberOfSplinePoints() - 1, ESplinePointType::Linear, false);
	}
	SetClosedLoop(true, false);
	UpdateSpline();
}

void UCSCityOutlineComponent::UpdateSpline()
{
	Super::UpdateSpline();
	if (IsTemplate()) return;
	if (ACSCityBuildingActor* Building = Cast<ACSCityBuildingActor>(GetOwner())) Building->Rebuild();
}

ACSCityBuildingActor::ACSCityBuildingActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Outline = CreateDefaultSubobject<UCSCityOutlineComponent>(TEXT("Outline"));
	RootComponent = Outline;
}

TArray<FVector2D> ACSCityBuildingActor::Footprint() const
{
	TArray<FVector2D> Points;
	if (!Outline) return Points;
	for (int32 I = 0; I < Outline->GetNumberOfSplinePoints(); I++) Points.Add(FVector2D(Outline->GetLocationAtSplinePoint(I, ESplineCoordinateSpace::World)));
	return CSCity::ConvexHull(Points);
}

TArray<FVector2D> ACSCityBuildingActor::Reservation() const
{
	return CSCity::Grow(Footprint(), double(Setback) * 100.0);
}

bool ACSCityBuildingActor::GarageDoor(FVector& OutDoor, FVector& OutFacing) const
{
	const TArray<FVector2D> Hull = Footprint();
	if (!bHasGarage || Hull.Num() < 3) return false;
	FVector2D Centre = FVector2D::ZeroVector;
	for (const FVector2D& Corner : Hull) Centre += Corner / Hull.Num();
	const FVector2D Mark(GetActorTransform().TransformPosition(GarageLocation));
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Hull.Num(); I++)
	{
		const FVector2D& A = Hull[I];
		const FVector2D& B = Hull[(I + 1) % Hull.Num()];
		const FVector2D Door = FMath::ClosestPointOnSegment2D(Mark, A, B);
		const double Distance = FVector2D::Distance(Mark, Door);
		FVector2D Out = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
		if (Distance >= Best || Out.IsNearlyZero()) continue;
		if (FVector2D::DotProduct(Out, Door - Centre) < 0.0) Out = -Out;
		Best = Distance;
		OutDoor = FVector(Door, GetActorLocation().Z);
		OutFacing = FVector(Out, 0.0);
	}
	return Best < TNumericLimits<double>::Max();
}

TArray<FString> ACSCityBuildingActor::GetRoadPrefabOptions() const
{
	return ACSRoadNetworkActor::RoadPrefabNames(ACSRoadNetworkActor::NetworkOf(this));
}

void ACSCityBuildingActor::Rebuild()
{
	if (!Outline || IsTemplate() || !GetWorld()) return;
	UCSCityFacadeKit* Used = Kit ? Kit.Get() : UCSCityFacadeKit::Default();
	if (!Used)
	{
		CSCity::ReleaseFacade(Facade, FacadeGpu);
		NotifyNetwork();
		return;
	}
	CSCity::FFacadeBuilding Building;
	Building.Block = CSCity::HullBlock(Footprint(), GetActorLocation().Z, double(Height) * 100.0);
	// Its windows drawn from its actor name, the same in every session.
	const FString Identity = GetPathName(GetOuter());
	Building.Seed = uint32(FXxHash64::HashBuffer(*Identity, Identity.Len() * sizeof(TCHAR)).Hash);
	Building.Shape = MassShape;
	Building.Stack = MassStack;
	// The door on the side facing the garage, where the garage is along it; else in the middle of the front.
	FVector Door;
	FVector Facing;
	if (GarageDoor(Door, Facing))
	{
		CSCity::FBlock& Block = Building.Block;
		const FVector2D Front = Block.Front.GetSafeNormal();
		// The sides' outward directions: the front, the building's +X, the back, its -X.
		const FVector2D Out[4] = { Front, FVector2D(Front.Y, -Front.X), -Front, FVector2D(-Front.Y, Front.X) };
		int32 Best = 0;
		for (int32 Side = 1; Side < 4; Side++) if (FVector2D::DotProduct(Out[Side], FVector2D(Facing)) > FVector2D::DotProduct(Out[Best], FVector2D(Facing))) Best = Side;
		if (Best % 2 == 1) Swap(Block.Width, Block.Depth);
		Block.Front = Out[Best];
		const FVector2D Along(Block.Front.Y, -Block.Front.X);
		Building.DoorAt = Block.Width > 0.0 ? FMath::Clamp((FVector2D(Door - Block.Base).Dot(Along) + 0.5 * Block.Width) / Block.Width, 0.0, 1.0) : 0.5;
	}
	CSCity::WriteFacade(*this, Outline, TEXT("Facade"), Building.Block.Base, *Used, MakeArrayView(&Building, 1), Facade, FacadeGpu, /*bFollowParent*/ true);
	NotifyNetwork();
}

void ACSCityBuildingActor::NotifyNetwork() const
{
	if (ACSRoadNetworkActor* Network = ACSRoadNetworkActor::NetworkOf(this)) Network->RequestRebuild();
	// With no network the streets make way themselves.
	else ACSCityStreetActor::RelayoutStreets(GetLevel());
}

void ACSCityBuildingActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Rebuild();
}

void ACSCityBuildingActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The modules are made at build time and never saved: a loaded or copied building writes them.
	if (!IsTemplate() && GetWorld() && Facade.Num() == 0) Rebuild();
}

void ACSCityBuildingActor::Destroyed()
{
	CSCity::ReleaseFacade(Facade, FacadeGpu);
	NotifyNetwork();
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSCityBuildingActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	Rebuild();
}

void ACSCityBuildingActor::PostEditUndo()
{
	Super::PostEditUndo();
	Rebuild();
}
#endif

#include "CSCityBuildingActor.h"

#include "CSCityLayout.h"
#include "CSRoadNetworkActor.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"

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
	Blocks = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Blocks"));
	Blocks->SetupAttachment(Outline);
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
	if (!Blocks || IsTemplate() || !GetWorld()) return;
	UStaticMesh* Piece = Module ? Module.Get() : DefaultModule();
	if (Blocks->GetStaticMesh() != Piece) Blocks->SetStaticMesh(Piece);
	TArray<FTransform> Pieces;
	if (Piece)
	{
		const FBox Bounds = Piece->GetBoundingBox();
		CSCity::AssembleBlock(CSCity::HullBlock(Footprint(), GetActorLocation().Z, double(Height) * 100.0), Bounds, double(StoreyHeight) * 100.0, Pieces);
		// The garage door: a slab 3 m wide and 2.6 m high, 10 cm into the wall and 20 cm out of it.
		FVector Door;
		FVector Facing;
		if (GarageDoor(Door, Facing))
		{
			CSCity::FBlock Slab;
			Slab.Base = Door + Facing * 5.0;
			Slab.Front = FVector2D(Facing);
			Slab.Width = 300.0;
			Slab.Depth = 30.0;
			Slab.Height = 260.0;
			Pieces.Add(CSCity::BlockPiece(Slab, Bounds));
		}
	}
	Blocks->ClearInstances();
	Blocks->AddInstances(Pieces, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ true);
	NotifyNetwork();
}

UStaticMesh* ACSCityBuildingActor::DefaultModule()
{
	return LoadObject<UStaticMesh>(nullptr, TEXT("/PCGPlugins/CSRoadNet/Houses/SM_CityBlock.SM_CityBlock"));
}

void ACSCityBuildingActor::NotifyNetwork() const
{
	if (ACSRoadNetworkActor* Network = ACSRoadNetworkActor::NetworkOf(this)) Network->RequestRebuild();
}

void ACSCityBuildingActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Rebuild();
}

void ACSCityBuildingActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// A building saved before it had copies (or with the module gone) puts them where they go.
	if (!IsTemplate() && GetWorld() && Blocks && Blocks->GetInstanceCount() == 0) Rebuild();
}

void ACSCityBuildingActor::Destroyed()
{
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

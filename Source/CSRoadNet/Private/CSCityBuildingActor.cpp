#include "CSCityBuildingActor.h"

#include "CSCityLayout.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadNetworkActor.h"
#include "Materials/MaterialInterface.h"

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
	// World-space streams drawn with an absolute transform: never the root (it would apply the
	// actor's placement twice).
	Mesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Outline);
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
	if (!Mesh || IsTemplate() || !GetWorld()) return;
	FCSGpuMeshCPUData Data;
	CSCity::AppendPrism(Data, Footprint(), GetActorLocation().Z, double(Height) * 100.0, Color);
	UMaterialInterface* Surface = Material ? Material.Get() : DefaultMaterial();
	if (!Prism || Prism->GetOuter() != Mesh) Prism = NewObject<UCSMesh>(Mesh);
	Prism->Materials = { Surface };
	Prism->NotifyMaterialsChanged();
	Mesh->MeshMaterial = Surface;
	if (Data.Indices.Num() >= 3 && UCSMeshOps::CopyFromMeshSnapshot(Prism, Data)) Mesh->SetGpuMesh(Prism);
	else Mesh->SetGpuMesh(nullptr);
	NotifyNetwork();
}

UMaterialInterface* ACSCityBuildingActor::DefaultMaterial()
{
	return LoadObject<UMaterialInterface>(nullptr, TEXT("/PCGPlugins/MeshBoolean/M_VertexColor.M_VertexColor"));
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
	// The prism is transient: a loaded or duplicated building has none until it builds.
	if (!IsTemplate() && GetWorld() && Mesh && !Mesh->HasGeneratedGeometry()) Rebuild();
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

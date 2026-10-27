#include "CSRoadSplineActor.h"

#include "CSRoadNetworkActor.h"

void UCSRoadSplineMetadata::InsertPoint(int32 Index, float t, bool bClosedLoop)
{
	Modify();
	// The new point starts the second half of the segment it splits, so it takes that segment's data.
	FCSRoadSplinePoint Point;
	const int32 Previous = Index > 0 ? Index - 1 : (bClosedLoop ? Points.Num() - 1 : INDEX_NONE);
	if (Points.IsValidIndex(Previous))
	{
		Point.bSegmentElevation = Points[Previous].bSegmentElevation;
		Point.SegmentElevation = Points[Previous].SegmentElevation;
		Point.SegmentOrder = Points[Previous].SegmentOrder;
		Point.SegmentOwner = Points[Previous].SegmentOwner;
	}
	Points.Insert(Point, FMath::Clamp(Index, 0, Points.Num()));
}

void UCSRoadSplineMetadata::UpdatePoint(int32 Index, float t, bool bClosedLoop)
{
}

void UCSRoadSplineMetadata::AddPoint(float InputKey)
{
	Modify();
	Points.AddDefaulted();
}

void UCSRoadSplineMetadata::RemovePoint(int32 Index)
{
	if (!Points.IsValidIndex(Index)) return;
	Modify();
	Points.RemoveAt(Index);
}

void UCSRoadSplineMetadata::DuplicatePoint(int32 Index)
{
	if (!Points.IsValidIndex(Index)) return;
	Modify();
	Points.Insert(FCSRoadSplinePoint(Points[Index]), Index);
}

void UCSRoadSplineMetadata::CopyPoint(const USplineMetadata* FromSplineMetadata, int32 FromIndex, int32 ToIndex)
{
	const UCSRoadSplineMetadata* From = Cast<UCSRoadSplineMetadata>(FromSplineMetadata);
	if (!From || !From->Points.IsValidIndex(FromIndex) || !Points.IsValidIndex(ToIndex)) return;
	Modify();
	Points[ToIndex] = From->Points[FromIndex];
}

void UCSRoadSplineMetadata::Reset(int32 NumPoints)
{
	Modify();
	Points.Reset(NumPoints);
}

void UCSRoadSplineMetadata::Fixup(int32 NumPoints, USplineComponent* SplineComp)
{
	if (Points.Num() != NumPoints) Points.SetNum(NumPoints);
}

#if WITH_EDITOR
void UCSRoadSplineMetadata::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (const UCSRoadSplineComponent* Spline = Cast<UCSRoadSplineComponent>(GetOuter())) Spline->NotifyNetwork();
}

void UCSRoadSplineMetadata::PostEditUndo()
{
	Super::PostEditUndo();
	if (const UCSRoadSplineComponent* Spline = Cast<UCSRoadSplineComponent>(GetOuter())) Spline->NotifyNetwork();
}
#endif

UCSRoadSplineComponent::UCSRoadSplineComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	RoadMetadata = CreateDefaultSubobject<UCSRoadSplineMetadata>(TEXT("RoadMetadata"));
}

USplineMetadata* UCSRoadSplineComponent::GetSplinePointsMetadata()
{
	return RoadMetadata;
}

const USplineMetadata* UCSRoadSplineComponent::GetSplinePointsMetadata() const
{
	return RoadMetadata;
}

void UCSRoadSplineComponent::UpdateSpline()
{
	Super::UpdateSpline();
	if (!IsTemplate()) NotifyNetwork();
}

void UCSRoadSplineComponent::PostLoad()
{
	Super::PostLoad();
	if (RoadMetadata) RoadMetadata->Fixup(GetNumberOfSplinePoints(), this);
}

#if WITH_EDITOR
void UCSRoadSplineComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (RoadMetadata) RoadMetadata->Fixup(GetNumberOfSplinePoints(), this);
	NotifyNetwork();
}

void UCSRoadSplineComponent::PostEditUndo()
{
	Super::PostEditUndo();
	NotifyNetwork();
}
#endif

ACSRoadNetworkActor* UCSRoadSplineComponent::FindNetwork() const
{
	return ACSRoadNetworkActor::NetworkOf(GetOwner());
}

void UCSRoadSplineComponent::NotifyNetwork() const
{
	if (ACSRoadNetworkActor* Network = FindNetwork()) Network->RequestRebuild();
}

TArray<FString> UCSRoadSplineComponent::GetRoadPrefabOptions() const
{
	return ACSRoadNetworkActor::RoadPrefabNames(FindNetwork());
}

ACSRoadSplineActor::ACSRoadSplineActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Spline = CreateDefaultSubobject<UCSRoadSplineComponent>(TEXT("Spline"));
	RootComponent = Spline;
	// A spline component starts 1 m long; a new road starts 40 m straight ahead.
	Spline->SetLocationAtSplinePoint(1, FVector(4000.0, 0.0, 0.0), ESplineCoordinateSpace::Local);
}

void ACSRoadSplineActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (Spline) Spline->NotifyNetwork();
}

void ACSRoadSplineActor::Destroyed()
{
	if (Spline) Spline->NotifyNetwork();
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSRoadSplineActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// Every step of a drag: the network rebuilds once per tick, so the roads follow the gizmo.
	if (Spline) Spline->NotifyNetwork();
}

void ACSRoadSplineActor::PostEditUndo()
{
	Super::PostEditUndo();
	if (Spline) Spline->NotifyNetwork();
}
#endif

// Roads drawn as splines for an ACSRoadNetworkActor building from splines: one ACSRoadSplineActor
// per road. Its UCSRoadSplineComponent holds the road's prefab and, per point, what the point says
// about its node and the segment to the next point (UCSRoadSplineMetadata, kept in step as points
// are added or removed). CSRoadSplineNetwork turns them into the network's nodes and edges.
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "CSRoadSplineActor.generated.h"

class ACSRoadNetworkActor;

UENUM()
enum class ECSRoadTier : uint8
{
	/** A main road: alleys under it give way. */
	Main,
	/** An alley (laid out beforehand): where a main road covers it, it is cut away and its ends join that road. The
	 *  spline stays as drawn, so the alley comes back when the road goes. */
	Alley,
};

// Unset values are measured (elevation above the ground) or chosen by the network (node prefab).
USTRUCT()
struct CSROADNET_API FCSRoadSplinePoint
{
	GENERATED_BODY()

	/** Where the node is, from the point (spline space): CS2 roads may end off their node's centre. */
	UPROPERTY(EditAnywhere, Category = "Node")
	FVector NodeOffset = FVector::ZeroVector;

	/** The node's prefab; empty: the highest node priority of the roads meeting there. */
	UPROPERTY(EditAnywhere, Category = "Node")
	FString NodePrefab;

	UPROPERTY(EditAnywhere, Category = "Node", meta = (InlineEditConditionToggle))
	bool bNodeElevation = false;

	/** Height above the ground at the node, left and right (m); unset: measured. */
	UPROPERTY(EditAnywhere, Category = "Node", meta = (EditCondition = "bNodeElevation"))
	FVector2D NodeElevation = FVector2D::ZeroVector;

	/** The node's upgrades: CompositionFlags general, left, right. */
	UPROPERTY(EditAnywhere, Category = "Node")
	FIntVector NodeUpgrade = FIntVector::ZeroValue;

	/** The node is an outside connection: its road runs off the map, with no end there. */
	UPROPERTY(EditAnywhere, Category = "Node")
	bool bOutsideConnection = false;

	/** Something owns the node (a converted snapshot's building): the geometry does not flatten it. */
	UPROPERTY(EditAnywhere, Category = "Node", AdvancedDisplay)
	bool bNodeOwned = false;

	UPROPERTY(EditAnywhere, Category = "Segment", meta = (InlineEditConditionToggle))
	bool bSegmentElevation = false;

	/** Height above the ground of the segment to the next point, left and right (m); unset: measured at its middle. */
	UPROPERTY(EditAnywhere, Category = "Segment", meta = (EditCondition = "bSegmentElevation"))
	FVector2D SegmentElevation = FVector2D::ZeroVector;

	/** Something owns the segment's edge (a converted snapshot's building): the geometry does not flatten its ends. */
	UPROPERTY(EditAnywhere, Category = "Segment", AdvancedDisplay)
	bool bSegmentOwned = false;
};

UCLASS()
class CSROADNET_API UCSRoadSplineMetadata : public USplineMetadata
{
	GENERATED_BODY()

public:
	/** One per spline point. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	TArray<FCSRoadSplinePoint> Points;

	virtual void InsertPoint(int32 Index, float t, bool bClosedLoop) override;
	virtual void UpdatePoint(int32 Index, float t, bool bClosedLoop) override;
	virtual void AddPoint(float InputKey) override;
	virtual void RemovePoint(int32 Index) override;
	virtual void DuplicatePoint(int32 Index) override;
	virtual void CopyPoint(const USplineMetadata* FromSplineMetadata, int32 FromIndex, int32 ToIndex) override;
	virtual void Reset(int32 NumPoints) override;
	virtual void Fixup(int32 NumPoints, USplineComponent* SplineComp) override;
#if WITH_EDITOR
	// Edited in the details panel, the points change without their spline: the network is told directly.
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif
};

/** A road of an ACSRoadNetworkActor building from splines: every point a node, every segment an edge. */
UCLASS(ClassGroup = (CS2Road), meta = (BlueprintSpawnableComponent))
class CSROADNET_API UCSRoadSplineComponent : public USplineComponent
{
	GENERATED_BODY()

public:
	UCSRoadSplineComponent(const FObjectInitializer& ObjectInitializer);

	/** The road prefab of every segment. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (GetOptions = "GetRoadPrefabOptions"))
	FString Prefab = TEXT("Small Road");

	/** A main road, or an alley that gives way to main roads. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	ECSRoadTier Tier = ECSRoadTier::Main;

	/** Split segments that are too long or turn too much, merge short ones, and join the roads this one crosses or
	 *  ends on. Off keeps every point a node exactly as drawn (a converted snapshot). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	bool bAdjustSegments = true;

	/** The upgrades of every segment (CompositionFlags general, left, right): trees, sound barriers, ... */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", AdvancedDisplay)
	FIntVector Upgrade = FIntVector::ZeroValue;

	/** Per point: node and segment data. */
	UPROPERTY(EditAnywhere, Instanced, Export, Category = "CS2 Road", AdvancedDisplay)
	TObjectPtr<UCSRoadSplineMetadata> RoadMetadata;

	virtual USplineMetadata* GetSplinePointsMetadata() override;
	virtual const USplineMetadata* GetSplinePointsMetadata() const override;
	// Every change of the curve, from the editor's spline tools (each step of a drag) or from code.
	virtual void UpdateSpline() override;
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif

	// The network building this spline: the network actor owning it or one it hangs under, else the
	// level's only network building from splines.
	ACSRoadNetworkActor* FindNetwork() const;
	// Has that network rebuild on its next tick.
	void NotifyNetwork() const;

	UFUNCTION()
	TArray<FString> GetRoadPrefabOptions() const;
};

/** One road of an ACSRoadNetworkActor building from splines. */
UCLASS()
class CSROADNET_API ACSRoadSplineActor : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadSplineActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS2 Road")
	TObjectPtr<UCSRoadSplineComponent> Spline;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif
};

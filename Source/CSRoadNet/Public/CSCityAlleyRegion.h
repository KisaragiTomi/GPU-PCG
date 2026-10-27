// Where a city's alleys are laid out beforehand: Generate splits the region (its box, seen from above)
// into blocks by straight alleys, keeping out of the plots ACSCityBuildingActors reserve there, and
// leaves them as ordinary spline roads of tier Alley under the road network. From then on they are
// drawn roads like any other; main roads drawn over them later cut them back (CSRoadSplineNetwork).
// Generating again replaces the alleys the region made before.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSCityAlleyRegion.generated.h"

class ACSRoadSplineActor;
class UBoxComponent;

UCLASS()
class CSROADNET_API ACSCityAlleyRegion : public AActor
{
	GENERATED_BODY()

public:
	ACSCityAlleyRegion();

	/** The region: the box's rectangle seen from above. The alleys lie at its height. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "City Alleys")
	TObjectPtr<UBoxComponent> Bounds;

	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (GetOptions = "GetRoadPrefabOptions"))
	FString AlleyPrefab = TEXT("Alley");

	/** Blocks are split until no side is longer. */
	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (ClampMin = "10", Units = "m"))
	float BlockSize = 90.0f;

	/** No split leaves a block narrower. */
	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (ClampMin = "5", Units = "m"))
	float MinBlockSize = 35.0f;

	/** How far from the middle of a block its split may fall, as a share of the block. */
	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (ClampMin = "0", ClampMax = "0.4"))
	float Jitter = 0.2f;

	/** How far a split may turn from square across the block. */
	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (ClampMin = "0", ClampMax = "30", Units = "deg"))
	float TurnJitter = 8.0f;

	/** An alley's centre line keeps this far out of a reserved plot (which already holds the building's setback). */
	UPROPERTY(EditAnywhere, Category = "City Alleys", meta = (ClampMin = "0", Units = "m"))
	float Clearance = 6.0f;

	/** Alleys round the region's edge too. */
	UPROPERTY(EditAnywhere, Category = "City Alleys")
	bool bBoundaryAlleys = true;

	UPROPERTY(EditAnywhere, Category = "City Alleys")
	int32 Seed = 1;

	/** The alleys the last Generate made; generating again replaces them. */
	UPROPERTY(VisibleAnywhere, Category = "City Alleys", AdvancedDisplay)
	TArray<TObjectPtr<ACSRoadSplineActor>> Generated;

	/** Lays the region's alleys out (again), keeping out of the reserved plots. Undoable. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "City Alleys")
	void Generate();

	UFUNCTION()
	TArray<FString> GetRoadPrefabOptions() const;
};

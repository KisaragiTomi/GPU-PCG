// A building placed by hand in the city layout: copies of a module on the smallest box around its outline's convex hull,
// as the batch buildings are put together (CSCity::AssembleBlock), drawn as instances; it makes no mesh of its own. Its
// footprint grown by Setback is the plot it reserves: alleys laid out afterwards
// (ACSCityAlleyRegion) keep out of it, and a road network's batch buildings leave it free. A garage on it the network
// joins to the nearest main road with a driveway it derives on every build (CSCityGarage).
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "CSCityBuildingActor.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;

/** The outline of an ACSCityBuildingActor: a closed loop; only its points' x and y count. */
UCLASS(ClassGroup = (CS2Road))
class CSROADNET_API UCSCityOutlineComponent : public USplineComponent
{
	GENERATED_BODY()

public:
	UCSCityOutlineComponent(const FObjectInitializer& ObjectInitializer);

	// Every change of the curve, from the editor's spline tools (each step of a drag) or from code.
	virtual void UpdateSpline() override;
};

UCLASS()
class CSROADNET_API ACSCityBuildingActor : public AActor
{
	GENERATED_BODY()

public:
	ACSCityBuildingActor();

	/** The footprint is the convex hull of these points. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "City Building")
	TObjectPtr<UCSCityOutlineComponent> Outline;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building", meta = (ClampMin = "1", Units = "m"))
	float Height = 15.0f;

	/** How far around the footprint its plot reaches: alleys and batch buildings stay out of it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building", meta = (ClampMin = "0", Units = "m"))
	float Setback = 4.0f;

	/** What it is put together from, as ACSRoadNetworkActor::BuildingModule. Empty: DefaultModule. */
	UPROPERTY(EditAnywhere, Category = "City Building")
	TObjectPtr<UStaticMesh> Module;

	/** The storeys Module's copies stack in come nearest this height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building", meta = (ClampMin = "0.5", Units = "m"))
	float StoreyHeight = 3.0f;

	/** A garage: the road network joins it to the nearest main road in front of it (within GarageReach) with a driveway of
	 *  GarageRoad, which batch buildings keep clear of but do not line. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage")
	bool bHasGarage = false;

	/** Where the garage is, relative to the building (the diamond in the viewport while it is selected): its door is the
	 *  nearest point of the footprint's sides, facing out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (MakeEditWidget, EditCondition = "bHasGarage"))
	FVector GarageLocation = FVector(0.0, -700.0, 0.0);

	/** The driveway's road. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (GetOptions = "GetRoadPrefabOptions", EditCondition = "bHasGarage"))
	FString GarageRoad = TEXT("Alley");

	/** How far from the door a main road may be for the garage to join it: a driveway no longer than that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (ClampMin = "1", Units = "m", EditCondition = "bHasGarage"))
	float GarageReach = 50.0f;

	/** What the road network made of the garage when it last built: the road it joins, or why it joins none. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Building|Garage")
	FString GarageStatus;

	/** The module's copies (and the garage door's). */
	UPROPERTY(VisibleAnywhere, Category = "City Building")
	TObjectPtr<UInstancedStaticMeshComponent> Blocks;

	// World space (cm), counter-clockwise from above.
	TArray<FVector2D> Footprint() const;
	// The footprint grown by Setback.
	TArray<FVector2D> Reservation() const;
	// The garage door (world space, on the footprint's side at the building's base) and the level way out of it; false
	// without a garage or a footprint.
	bool GarageDoor(FVector& OutDoor, FVector& OutFacing) const;

	// Puts the copies where they go again, and tells the road network its plot may have moved.
	void Rebuild();

	// What buildings are put together from when nothing else is set: /PCGPlugins/CSRoadNet/Houses/SM_CityBlock, a 1 m box
	// with Nanite on (Scripts/CS2Road/MakeCityBlock.py makes it).
	static UStaticMesh* DefaultModule();

	UFUNCTION()
	TArray<FString> GetRoadPrefabOptions() const;

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

private:
	void NotifyNetwork() const;
};

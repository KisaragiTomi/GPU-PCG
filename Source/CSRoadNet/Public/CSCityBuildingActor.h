// A building placed by hand in the city layout: a facade kit's modules round the smallest box about its outline's convex
// hull, put together on the GPU as the batch buildings are (CSCityFacade.h), its door on the side its garage is on; it
// makes no mesh of its own. Its footprint grown by Setback is the plot it reserves: alleys laid out afterwards
// (ACSCityAlleyRegion) keep out of it, and a road network's batch buildings leave it free. A garage on it the network
// joins to the nearest main road (or, none in reach, the nearest small road leading to one) with a driveway it derives on
// every build (CSCityGarage).
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "CSCityFacadeKit.h"
#include "CSCityBuildingActor.generated.h"

class UCSCityFacadeKit;
class UCSGpuInstancedMeshComponent;
namespace CSCity
{
class FFacadeGpu;
}

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

	/** What it is put together from, as ACSRoadNetworkActor::BuildingKit (Height rounded to its storeys). Empty:
	 *  UCSCityFacadeKit::Default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building")
	TObjectPtr<UCSCityFacadeKit> Kit;

	/** Its footprint on the kit's bay grid. Auto: drawn from the kit's shape weights and the building's name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building")
	ECSCityMassShape MassShape = ECSCityMassShape::Auto;

	/** How its upper storeys sit on the lower ones. Auto: drawn from the kit's chances and the building's name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building")
	ECSCityMassStack MassStack = ECSCityMassStack::Auto;

	/** A garage: the road network joins it to the nearest main road in front of it (within GarageReach) with a driveway of
	 *  GarageRoad, which batch buildings keep clear of but do not line. With no main road there, to the nearest small road
	 *  in front of it, which counts as joined when it leads on to a main road. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage")
	bool bHasGarage = false;

	/** Where the garage is, relative to the building (the diamond in the viewport while it is selected): its door is the
	 *  nearest point of the footprint's sides, facing out. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (MakeEditWidget, EditCondition = "bHasGarage"))
	FVector GarageLocation = FVector(0.0, -700.0, 0.0);

	/** The driveway's road. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (GetOptions = "GetRoadPrefabOptions", EditCondition = "bHasGarage"))
	FString GarageRoad = TEXT("Alley");

	/** How far from the door a road may be for the garage to join it: a driveway no longer than that. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building|Garage", meta = (ClampMin = "1", Units = "m", EditCondition = "bHasGarage"))
	float GarageReach = 50.0f;

	/** What the road network made of the garage when it last built: the road it joins (a small road: and the main road it
	 *  leads to), or why it joins none. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Building|Garage")
	FString GarageStatus;

	/** The kit's modules, a component per slot (null where the kit has none), made when it builds. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Building")
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> Facade;

	// World space (cm), counter-clockwise from above.
	TArray<FVector2D> Footprint() const;
	// The footprint grown by Setback.
	TArray<FVector2D> Reservation() const;
	// The garage door (world space, on the footprint's side at the building's base) and the level way out of it; false
	// without a garage or a footprint.
	bool GarageDoor(FVector& OutDoor, FVector& OutFacing) const;

	// Writes its modules again (drawn a frame or two later, no wait for the render thread), and tells the road network its
	// plot may have moved.
	void Rebuild();

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
	TSharedPtr<CSCity::FFacadeGpu> FacadeGpu;

	void NotifyNetwork() const;
};

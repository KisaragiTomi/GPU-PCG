// A building placed by hand in the city layout, for now the convex hull of its outline pushed up to a
// height. Its footprint grown by Setback is the plot it reserves: alleys laid out afterwards
// (ACSCityAlleyRegion) keep out of it, and a road network's batch buildings leave it free.
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "CSCityBuildingActor.generated.h"

class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;

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

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Building")
	FLinearColor Color = FLinearColor(0.78f, 0.55f, 0.42f);

	/** Empty: /PCGPlugins/MeshBoolean/M_VertexColor. */
	UPROPERTY(EditAnywhere, Category = "City Building")
	TObjectPtr<UMaterialInterface> Material;

	UPROPERTY(VisibleAnywhere, Category = "City Building")
	TObjectPtr<UCSMeshRenderComponent> Mesh;

	// World space (cm), counter-clockwise from above.
	TArray<FVector2D> Footprint() const;
	// The footprint grown by Setback.
	TArray<FVector2D> Reservation() const;

	// Builds the prism again and tells the road network its plot may have moved.
	void Rebuild();

	// What buildings with no material of their own are drawn with: vertex colours.
	static UMaterialInterface* DefaultMaterial();

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

private:
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> Prism;

	void NotifyNetwork() const;
};

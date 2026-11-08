// What the city's ground is put together from (GroundJoin_Plan.md): slabs that fill the blocks between the roads, walls where the
// ground drops too steeply for a slab to follow, and what stands in the ground left free between the buildings: planters, or cart
// shops where the city's field (ACSCityFieldActor) says it is a night market. A compute pass decides all of it and writes GPU instances
// (CSCityGround.h), so the ground makes no mesh of its own; give the meshes Nanite.
#pragma once

#include "CoreMinimal.h"
#include "CSCityFacadeKit.h"
#include "Engine/DataAsset.h"
#include "CSCityGroundKit.generated.h"

UCLASS(BlueprintType)
class CSROADNET_API UCSCityGroundKit : public UDataAsset
{
	GENERATED_BODY()

public:
	/** A slab of ground: stretched over its cell (12, 6 or 3 m, as flat as it can be), its top on the ground, turned to the slope, its
	 *  thickness kept: the sides hide the little steps between neighbours. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground")
	FCSCityFacadeModule Slab;

	/** Where a slab's edge stands high over its neighbour: its +Y side out, its top at the edge, stretched along it and down. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground")
	FCSCityFacadeModule Wall;

	/** In the ground left free, its long side (+X) along the nearest road, standing on the ground at its box's bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps")
	FCSCityFacadeModule Planter;

	/** Another, drawn half the time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps")
	FCSCityFacadeModule Planter2;

	/** In a night market: its front (+Y) towards the nearest road, standing on the ground at its box's bottom. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps")
	FCSCityFacadeModule CartShop;

	/** Another, drawn half the time. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps")
	FCSCityFacadeModule CartShop2;

	/** How far from a road the ground reaches: a block closed in by roads is filled by anything over half its width. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "m"))
	float Reach = 60.0f;

	/** Next to a road the ground is this far under the road's edge, so that the road's mesh covers the seam. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "cm"))
	float EdgeSink = 2.5f;

	/** How far from a road's edge the ground stays at that height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "m"))
	float RoadMargin = 1.0f;

	/** How far beyond that it takes to come back to what the rest of the ground wants. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "m"))
	float RoadShoulder = 8.0f;

	/** How far from a plot it takes to come back from the plot's own level. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "m"))
	float PlotBlend = 4.0f;

	/** Steeper than this a slab stays level, and walls take up the difference. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", ClampMax = "1"))
	float MaxSlope = 0.25f;

	/** The least step between neighbours a wall goes on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Ground", meta = (ClampMin = "0", Units = "cm"))
	float WallMinStep = 40.0f;

	/** How far from any building ground must be to count as free: a gap between buildings narrower than twice this has nothing in it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", Units = "m"))
	float BuildingClearance = 3.0f;

	/** How far from a road's edge. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", Units = "m"))
	float RoadClearance = 2.0f;

	/** Free ground this steep has nothing on it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", ClampMax = "1"))
	float MaxPropSlope = 0.15f;

	/** How likely each 6 m square of free ground has a planter, where the field says nothing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", ClampMax = "1"))
	float AutoPlanterChance = 0.25f;

	/** Where the field says planters. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", ClampMax = "1"))
	float PlanterChance = 0.6f;

	/** Where the field says night market. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gaps", meta = (ClampMin = "0", ClampMax = "1"))
	float CartChance = 0.8f;

	/** /PCGPlugins/CSRoadNet/Houses/NCSimple/DA_NCSimple_Ground (Scripts/NightCity/MakeNCSimpleModules.py). */
	static UCSCityGroundKit* Default();
};

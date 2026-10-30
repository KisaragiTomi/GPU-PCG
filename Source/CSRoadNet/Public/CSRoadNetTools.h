// One-off conversions on a road network, for scripts and editor utility blueprints rather than the
// network's details panel: a road snapshot's roads into spline roads, its terrain into a Landscape.
// Python: unreal.CSRoadNetTools.convert_to_splines(network) and the like.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSRoadNetTools.generated.h"

class ACSRoadNetworkActor;
class UMaterialInterface;

UCLASS()
class CSROADNET_API UCSRoadNetTools : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/** Network's NetworkFile roads as ACSRoadSplineActors under it, one per run of edges where the road goes on, with
	 *  every node's and edge's data, so they build into the same roads; then builds from them. Refuses when spline
	 *  roads hang under the network already. Undoable; editor only. False with the reason in Network's LastError. */
	UFUNCTION(BlueprintCallable, Category = "CS2 Road|Tools")
	static bool ConvertToSplines(ACSRoadNetworkActor* Network);

	/** Network's NetworkFile terrain (its playable map) as a Landscape under the roads: a vertex per heightmap texel, as
	 *  many whole components (2 x 2 sections of 127 quads) as the map holds, cropped evenly, half a metre below the
	 *  heightmap so the ground roads' asphalt stays above it. Material: empty, M_SplineRoadDemoGround (a flat green).
	 *  The roads still measure their elevation from the snapshot, which holds the same heights. Refuses when the level
	 *  has a landscape. Undoable; editor only. The landscape made, or null with the reason in Network's LastError. */
	UFUNCTION(BlueprintCallable, Category = "CS2 Road|Tools")
	static AActor* ConvertTerrainToLandscape(ACSRoadNetworkActor* Network, UMaterialInterface* Material = nullptr);
};

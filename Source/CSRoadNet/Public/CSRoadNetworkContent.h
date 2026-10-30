// What an ACSRoadNetworkActor builds, held apart from it: the road mesh or tiles and their Nanite bakes, the
// markings, the batch buildings, the traffic and the bridge piers. A network the size of Palmsland makes
// thousands of components; on the network itself, selecting it in the editor built a details tree of them
// all and drew every one of them with the selection outline (about a second, then four times the frame time
// while selected). The network keeps the settings and the operations, the manager's part.
//
// The network spawns its content when it first builds, at its own placement and attached to it. The content
// is never saved, copied, listed in the outliner or selected (clicking a road selects nothing); it goes with
// the network.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSRoadNetworkContent.generated.h"

class UCSMeshRenderComponent;
class UInstancedStaticMeshComponent;

UCLASS(NotPlaceable, Transient)
class CSROADNET_API ACSRoadNetworkContent : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadNetworkContent();

	/** The whole network's road mesh, when it is not built in tiles. */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UCSMeshRenderComponent> RoadMesh;

	/** The decals of a whole build, one component per material slot and priority (tiles have their own). */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> MarkingDecals;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UInstancedStaticMeshComponent> TrafficBodies;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UInstancedStaticMeshComponent> TrafficCabins;

	/** Columns, caps and footings, generated as one instanced component. */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UInstancedStaticMeshComponent> BridgePiers;

#if WITH_EDITOR
	virtual bool IsSelectable() const override { return false; }
	virtual bool IsListedInSceneOutliner() const override { return false; }
#endif
};

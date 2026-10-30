#include "CSRoadNetworkContent.h"

#include "CSMeshRenderComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"

ACSRoadNetworkContent::ACSRoadNetworkContent()
{
	PrimaryActorTick.bCanEverTick = false;
	// The meshes are world-space and draw with an absolute transform, so they hang under a root of
	// their own: as the root, a mesh takes the actor's placement and so applies it twice.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	RoadMesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("RoadMesh"));
	RoadMesh->SetupAttachment(RootComponent);
	// The decals are DBuffer mesh decals: they show only on receivers.
	RoadMesh->bReceivesDecals = true;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	TrafficBodies = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TrafficBodies"));
	TrafficCabins = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TrafficCabins"));
	BridgePiers = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BridgePiers"));
	for (UInstancedStaticMeshComponent* Component : {TrafficBodies.Get(), TrafficCabins.Get(), BridgePiers.Get()})
	{
		Component->SetupAttachment(RootComponent);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube.Object);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
	}
	TrafficBodies->SetVisibility(false);
	TrafficCabins->SetVisibility(false);
}

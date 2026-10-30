#include "CSRoadNetTools.h"

#include "CSRoadNetworkActor.h"

bool UCSRoadNetTools::ConvertToSplines(ACSRoadNetworkActor* Network)
{
	return Network && Network->ConvertToSplines();
}

AActor* UCSRoadNetTools::ConvertTerrainToLandscape(ACSRoadNetworkActor* Network, UMaterialInterface* Material)
{
	return Network ? Network->ConvertTerrainToLandscape(Material) : nullptr;
}

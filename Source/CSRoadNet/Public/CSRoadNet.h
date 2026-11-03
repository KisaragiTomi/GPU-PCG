#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCSRoadNet, Log, All);

// The CS2 data the road network reads, kept in the plugin so that it needs no CitiesSkylines-Road2-C# checkout:
// <PCGPlugins>/Data/CS2Road with Prefabs (prefabs.json, raw-prefabs.json, pieces.bin), Palmsland (a snapshot and its
// heightmaps) and Golden (the tests' RoadLab export). Extracted from the game, so not in git (Docs/CS2RoadsGpu.md, "数据").
CSROADNET_API FString CSRoadDataDir();

class FCSRoadNetModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

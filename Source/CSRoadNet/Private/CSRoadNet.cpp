#include "CSRoadNet.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY(LogCSRoadNet);

FString CSRoadDataDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PCGPlugins"));
	const FString Base = Plugin.IsValid() ? Plugin->GetBaseDir() : FPaths::Combine(FPaths::ProjectPluginsDir(), TEXT("PCGPlugins"));
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(Base, TEXT("Data"), TEXT("CS2Road")));
}

void FCSRoadNetModule::StartupModule()
{
	// Shaders live under /Plugin/PCGPlugins/Shaders/Private/CSRoad; ComputeShaderGenerator
	// (a dependency, loaded first) registers that mapping.
}

void FCSRoadNetModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FCSRoadNetModule, CSRoadNet)

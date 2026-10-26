#include "CSRoadNet.h"

DEFINE_LOG_CATEGORY(LogCSRoadNet);

void FCSRoadNetModule::StartupModule()
{
	// Shaders live under /Plugin/PCGPlugins/Shaders/Private/CSRoad; ComputeShaderGenerator
	// (a dependency, loaded first) registers that mapping.
}

void FCSRoadNetModule::ShutdownModule()
{
}

IMPLEMENT_MODULE(FCSRoadNetModule, CSRoadNet)

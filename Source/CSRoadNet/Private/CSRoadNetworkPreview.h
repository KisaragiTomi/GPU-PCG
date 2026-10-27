#pragma once

#include "CSRoadLanes.h"
#include "CSRoadTrafficSimulation.h"

struct FCSRoadNetworkPreview
{
	CSRoad::FRoadLanes Lanes;
	CSRoadTraffic::FSimulation Simulation;
	int32 Count = INDEX_NONE;
	int32 Seed = 0;
	float Speed = 0;
	float Gap = 0;
};

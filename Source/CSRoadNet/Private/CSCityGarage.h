// A special building's garage joined to the road network (ACSCityBuildingActor's garage): the network builds a driveway
// from the garage door into the nearest main road in front of it, as an alley with no plots along it, which it then cuts
// at the road's edge and joins to the road as it does alleys. With no main road in reach, into the nearest small road
// instead, which it joins in a T junction (a road ending on another); the network then checks that small road leads on
// to a main road. Derived on every build, so it follows the building, the garage and the roads; nothing of it is saved.
#pragma once

#include "CoreMinimal.h"
#include "CSRoadSplineNetwork.h"

namespace CSRoad
{
class FAssets;
}

namespace CSCity
{
// A footprint the driveway may not run through, in the network's ground plane (CS2's x and z, metres).
struct FObstacle
{
	FString Name;
	TArray<FVector2d> Outline;     // convex
};

struct FGarage
{
	FString Name;                  // the driveway's, for messages
	uint64 Id = 0;                 // the driveway spline's
	FString Prefab;                // the driveway's road
	FVector3d Door;                // network space (CS2's: metres, y up), on the footprint's side at its base
	FVector2d Facing;              // out of the footprint, in the ground plane (x, z)
	double Reach = 50.0;           // how far from the door the road may be (m)
	double Clearance = 1.0;        // FSplineNetworkSettings::AlleyClearance (m)
	int32 Own = INDEX_NONE;        // its own footprint among the obstacles
};

// The driveway: an alley spline from the door into the main road (a spline of Splines that is no alley) nearest the door
// within reach and 60 degrees of where it faces, or with none there the small road (an alley) nearest it so, leaving the
// door square and meeting the road square. A door too close to the road for a driveway opens onto it: true with Out empty
// (no points). False with why when there is no such road, or when the way runs through a footprint (its own, or within
// half the driveway's width of another); OutWhy names the road otherwise, and OutTarget (if given) points to it in Splines.
bool Driveway(const FGarage& Garage, const TArray<CSRoad::FSplineRoad>& Splines, const TArray<FObstacle>& Obstacles, CSRoad::FAssets& Assets,
	CSRoad::FSplineRoad& Out, FString& OutWhy, const CSRoad::FSplineRoad** OutTarget = nullptr);
}

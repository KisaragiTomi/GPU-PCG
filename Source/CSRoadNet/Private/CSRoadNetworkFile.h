// Reader for the road snapshots written by CitiesSkylines-Road2-C# (format cs2-road-network-v1,
// e.g. Artifacts/public-saves/palmsland-3/network.json). Port of SavedNetwork / SavedTerrain.
#pragma once

#include "CSRoadGraph.h"

namespace CSRoad
{
// Two R16 heightmaps with the original half-texel offset and bilinear conversion.
struct FSavedTerrain
{
	struct FMap
	{
		int32 Width = 0;
		int32 Height = 0;
		TArray<uint16> Data;
		float2 Offset;
		float2 Size;
	};
	FMap Playable;
	FMap World;
	float2 HeightScaleOffset;

	bool IsValid() const { return Playable.Width > 1 && World.Width > 1; }
	float Sample(float3 Point) const;
};

struct FNetworkView
{
	FString Name;
	float3 Position;
	float Distance = 0.0f;
};

struct FSavedNetwork
{
	TArray<FSavedNode> Nodes;
	TArray<FRoadSpec> Roads;
	TArray<FNetworkView> Views;
	FSavedTerrain Terrain;
	bool bLeftHandTraffic = false;

	// Validates like SavedNetwork.Load: format tag, unique edge ids, known prefabs, finite curves.
	bool Load(const FString& Path, const FAssets* Assets, bool bLoadTerrain, FString& OutError);
};
}

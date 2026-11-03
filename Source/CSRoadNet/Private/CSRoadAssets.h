// Road prefab data in the shape the composition code consumes. Port of RoadAssets.cs from
// CitiesSkylines-Road2-C# (NetInitializeSystem subset), with three original-game details
// that port leaves out: CompositionState.BlockZone, PlacementFlags.LinkAuxOffsets and the
// per-composition Hidden/terrain state are applied here.
#pragma once

#include "CSRoadComposition.h"
#include "Dom/JsonObject.h"

namespace CSRoad
{
struct FPieceMesh
{
	TArray<float3> Vertices;
	TArray<float3> Normals;
	TArray<float4> Tangents;
	TArray<float2> UV0s;
	TArray<int32> Indices;
	TArray<MeshMaterial> Materials;
};

// RoadComposition record: one (road prefab, composition flags) result.
struct FComposition
{
	int32 Prefab = 0;
	NetCompositionData Data{};
	TArray<NetCompositionPiece> Pieces;
	TArray<NetCompositionLane> Lanes;
	TArray<NetCompositionCarriageway> Carriageways;
	TArray<NetCompositionCrosswalk> Crosswalks;
};

// A decal mesh of a lane prefab (a NetLanePrefab.m_Meshes entry whose render asset has an active
// DecalProperties): what RoadDecals.AddLane reads from it.
struct FLaneDecalMesh
{
	// The NetLaneMeshInfo requirements RoadDecals filters on.
	bool bRequireEditor = false;
	bool bRequireLeftHandTraffic = false;
	bool bRequireRightHandTraffic = false;
	bool bRequireLevelCrossing = false;
	bool bRequireTrackCrossing = false;
	// Render asset bounds size; x is the decal's width across the curve, z its tile length.
	float3 Size;
	// DecalProperties.m_TextureArea.
	float2 UVMin;
	float2 UVMax;
	int32 RendererPriority = 0;
	// CurveProperties (0 without one).
	int32 TilingCount = 0;
	float SmoothingDistance = 0.0f;
	// The render asset's first surface: the decal material.
	FString Surface;
};

class FAssets
{
public:
	// The byte arrays must outlive this object (they are owned by UCSRoadPrefabLibrary). CustomRoadsJson (the plugin's
	// Data/CustomRoads.json, optional): roads made from others, each {"copy": road, "sections": [indices into its
	// sections, or {"road": another road, "index": one of its, "invert"/"flip"/"median": set}], "underground": [the same
	// for its pipes, which keep a composition at least as wide], "zoning": false to give it no zone block, "plots":
	// {"setback": m, "frontage": m} to give it the batch buildings' plots anyway}, joining the roads as copies of their base
	// with only the sections listed (all of a list not given). A road's shoulders are what its junctions are made of
	// (a shoulder becomes a ramp up to a sidewalk there): keep one either side.
	bool Load(const TArray<uint8>& PrefabsJson, const TArray<uint8>& RawPrefabsJson, const TArray64<uint8>* PiecesBin, FString& OutError,
		const TArray<uint8>* CustomRoadsJson = nullptr);

	const TArray<FString>& RoadNames() const { return Names; }
	// A custom road's "plots": how far from its edge the batch buildings along it stand and how much of it each takes (m;
	// negative: the network's). Null for a road without: plots go by its zoning.
	struct FPlots
	{
		double Setback = -1.0;
		double Frontage = -1.0;
	};
	const FPlots* Plots(const FString& Name) const { return PlotSettings.Find(Name); }
	bool HasRoad(const FString& Name) const { return RoadIds.Contains(Name); }
	int32 RoadId(const FString& Name) const { return RoadIds.FindChecked(Name); }
	const FString& RoadName(int32 Id) const { return IdNames.FindChecked(Id); }

	// Compose(name, flags, resolveStates) with the (name, flags) cache of RoadAssets.Compose.
	const FComposition& Compose(const FString& Name, CompositionFlags Flags, bool bResolveStates = true);
	NetGeometryData Geometry(const FString& Name);
	NetData Network(const FString& Name);
	RoadData Road(const FString& Name);
	PlaceableNetData Placeable(const FString& Name);
	const TArray<NetGeometryEdgeState>& EdgeStates(int32 Road) const { return EdgeStateData.FindChecked(Road); }
	const TArray<NetGeometryNodeState>& NodeStates(int32 Road) const { return NodeStateData.FindChecked(Road); }

	// Piece meshes from pieces.bin, decoded on first use. Null when the piece has no geometry.
	const FPieceMesh* PieceMesh(int32 Piece);
	const TArray<FString>& Atlases() const { return AtlasNames; }
	// RoadRenderer.Material: the base-colour texture an atlas is drawn with (catalog material_bindings).
	FString AtlasBaseColor(const FString& Atlas) const;
	// RoadDecals.Material: material_bindings[Surface].BaseColor, empty without one (no fallback name).
	FString SurfaceBaseColor(const FString& Surface) const;
	// The decal meshes of a lane prefab in m_Meshes order; meshes without a render asset or decal
	// properties are left out, as AddLane skips them. Cached per prefab.
	const TArray<FLaneDecalMesh>& LaneDecalMeshes(int32 LanePrefab) const;

	TLookup<NetPieceData> PieceData;
	TLookup<NetVertexMatchData> VertexMatches;
	TLookup<NetLaneData> LaneData;
	TLookup<NetCrosswalkData> CrosswalkData;
	TBufferLookup<NetPieceLane> PieceLanes;
	TBufferLookup<NetSubSection> SubSections;
	TBufferLookup<NetSectionPiece> SectionPieces;

private:
	TSharedPtr<FJsonObject> Catalog;
	TSharedPtr<FJsonObject> Raw;
	const TArray64<uint8>* Pieces = nullptr;
	TMap<FString, TPair<int64, FIntVector>> MeshEntries;
	TMap<int32, TSharedPtr<FPieceMesh>> MeshCache;
	TArray<FString> AtlasNames;
	TArray<FString> Names;
	TMap<FString, FPlots> PlotSettings;
	TMap<FString, int32> RoadIds;
	TMap<int32, FString> IdNames;
	TArray<FString> RequirementNames;
	TBufferLookup<NetGeometryEdgeState> EdgeStateData;
	TBufferLookup<NetGeometryNodeState> NodeStateData;
	TMap<FString, TArray<NetGeometrySection>> Sections;
	TMap<TPair<FString, CompositionFlags>, TSharedPtr<FComposition>> CompositionCache;
	TMap<FString, NetGeometryData> GeometryCache;
	mutable TMap<int32, TArray<FLaneDecalMesh>> LaneDecalCache;

	TSharedPtr<FJsonObject> RawObject(int32 Id) const;
	TArray<TSharedPtr<FJsonObject>> Components(const TSharedPtr<FJsonObject>& Prefab) const;
	TSharedPtr<FJsonObject> RoadRaw(const FString& Name) const;
	void Requirements(const TSharedPtr<FJsonObject>& Item, const TCHAR* Key, CompositionFlags& OutFlags, uint& OutSections) const;
	void AddSections(const TArray<TSharedPtr<FJsonValue>>& Source, TArray<NetGeometrySection>& Target, uint Flags) const;
	uint InitializePieceFlags(int32 Piece) const;
	NetLaneData InitializeLane(const TSharedPtr<FJsonObject>& Prefab) const;
};
}

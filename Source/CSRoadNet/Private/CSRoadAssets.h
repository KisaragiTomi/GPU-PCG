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

class FAssets
{
public:
	// The byte arrays must outlive this object (they are owned by UCSRoadPrefabLibrary).
	bool Load(const TArray<uint8>& PrefabsJson, const TArray<uint8>& RawPrefabsJson, const TArray64<uint8>* PiecesBin, FString& OutError);

	const TArray<FString>& RoadNames() const { return Names; }
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
	TMap<FString, int32> RoadIds;
	TMap<int32, FString> IdNames;
	TArray<FString> RequirementNames;
	TBufferLookup<NetGeometryEdgeState> EdgeStateData;
	TBufferLookup<NetGeometryNodeState> NodeStateData;
	TMap<FString, TArray<NetGeometrySection>> Sections;
	TMap<TPair<FString, CompositionFlags>, TSharedPtr<FComposition>> CompositionCache;
	TMap<FString, NetGeometryData> GeometryCache;

	TSharedPtr<FJsonObject> RawObject(int32 Id) const;
	TArray<TSharedPtr<FJsonObject>> Components(const TSharedPtr<FJsonObject>& Prefab) const;
	TSharedPtr<FJsonObject> RoadRaw(const FString& Name) const;
	void Requirements(const TSharedPtr<FJsonObject>& Item, const TCHAR* Key, CompositionFlags& OutFlags, uint& OutSections) const;
	void AddSections(const TArray<TSharedPtr<FJsonValue>>& Source, TArray<NetGeometrySection>& Target, uint Flags) const;
	uint InitializePieceFlags(int32 Piece) const;
	NetLaneData InitializeLane(const TSharedPtr<FJsonObject>& Prefab) const;
};
}

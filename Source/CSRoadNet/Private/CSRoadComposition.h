// Port of Game.Prefabs.NetCompositionHelpers and NetCompositionSystem.AddCompositionCrosswalks
// (Cities: Skylines II 1.6.2f1, decompiled; Port/Original in CitiesSkylines-Road2-C#).
#pragma once

#include "CSRoadCore.h"

namespace CSRoad
{
// ComponentLookup / BufferLookup stand-ins keyed by entity id (0 = Entity.Null).
template<typename T> using TLookup = TMap<int32, T>;
template<typename T> using TBufferLookup = TMap<int32, TArray<T>>;

namespace NetCompositionHelpers
{
void GetRequirementFlags(const TArray<uint>& Requirements, CompositionFlags& OutCompositionFlags, uint& OutSectionFlags);
void GetRequirementFlags(uint Requirement, CompositionFlags& CompositionFlagsRef, uint& SectionFlags);
CompositionFlags InvertCompositionFlags(CompositionFlags Flags);
uint InvertSectionFlags(uint Flags);
bool TestSectionFlags(const NetGeometrySection& Section, CompositionFlags Flags);
bool TestSubSectionFlags(const NetSubSection& SubSection, CompositionFlags Flags, uint SectionFlags);
bool TestPieceFlags(const NetSectionPiece& Piece, CompositionFlags Flags, uint SectionFlags);
bool TestPieceFlags2(const NetSectionPiece& Piece, CompositionFlags Flags, uint SectionFlags);
bool TestEdgeFlags(const NetGeometryEdgeState& EdgeState, CompositionFlags Flags);
bool TestEdgeFlags(const NetGeometryNodeState& NodeState, CompositionFlags Flags);
bool TestEdgeMatch(const NetGeometryNodeState& NodeState, bool2 Match);
void GetCompositionPieces(TArray<NetCompositionPiece>& ResultBuffer, const TArray<NetGeometrySection>& GeometrySections, CompositionFlags Flags,
	const TBufferLookup<NetSubSection>& SubSectionData, const TBufferLookup<NetSectionPiece>& SectionPieceData);
void CalculateCompositionData(NetCompositionData& CompositionData, TArray<NetCompositionPiece>& Pieces,
	const TLookup<NetPieceData>& PieceData, const TLookup<NetVertexMatchData>& VertexMatchData);
// Carriageways may be null (the original passes an uncreated buffer from InitializeNetDefaultsJob).
void AddCompositionLanes(int32 Entity, NetCompositionData& CompositionData, TArray<NetCompositionPiece>& Pieces, TArray<NetCompositionLane>& NetLanes,
	TArray<NetCompositionCarriageway>* Carriageways, const TLookup<NetLaneData>& LaneData, const TBufferLookup<NetPieceLane>& PieceLanes);
float2 CalculateRoundaboutSize(const NetCompositionData& CompositionData, const TArray<NetCompositionPiece>& Pieces);
CompositionFlags GetElevationFlags(const Elevation& StartElevation, const Elevation& MiddleElevation, const Elevation& EndElevation, const NetGeometryData& PrefabGeometryData);
}

namespace NetCompositionSystem
{
void AddCompositionCrosswalks(const NetCompositionData& CompositionData, const TArray<NetCompositionPiece>& Pieces, TArray<NetCompositionCrosswalk>& Crosswalks,
	const TLookup<NetCrosswalkData>& CrosswalkData, const TLookup<NetLaneData>& LaneData, const TBufferLookup<NetPieceLane>& PieceLanes);
}
}

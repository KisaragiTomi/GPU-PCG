#include "CSRoadComposition.h"

namespace CSRoad
{
namespace NetCompositionHelpers
{
namespace
{
struct FTempLaneData
{
	int32 m_GroupIndex = 0;
};

struct FTempLaneGroup
{
	int32 m_Prefab = 0;
	float3 m_Position;
	uint m_Flags = 0;
	int32 m_LaneCount = 0;
	int32 m_FullLaneCount = 0;
	int32 m_CarriagewayIndex = 0;

	bool IsCompatible(const FTempLaneGroup& Other) const
	{
		const uint Mask = LaneFlags_Invert | LaneFlags_Road;
		return ((m_Flags & Mask) == (Other.m_Flags & Mask)) & ((m_Flags & LaneFlags_Road) != 0);
	}
};

int32 FindClosestLane(const TArray<NetCompositionLane>& Lanes, int32 StartIndex, float3 Position, uint FlagMask, uint Flags)
{
	int32 Num = StartIndex - 1;
	int32 Num2 = StartIndex + 1;
	while (true)
	{
		if (Num >= 0 && Num2 < Lanes.Num())
		{
			const NetCompositionLane& Lane = Lanes[Num];
			const NetCompositionLane& Lane2 = Lanes[Num2];
			if (math::lengthsq(Lane.m_Position - Position) <= math::lengthsq(Lane2.m_Position - Position))
			{
				if ((Lane.m_Flags & FlagMask) == Flags) return Num;
				Num--;
			}
			else
			{
				if ((Lane2.m_Flags & FlagMask) == Flags) return Num2;
				Num2++;
			}
		}
		else if (Num >= 0)
		{
			if ((Lanes[Num].m_Flags & FlagMask) == Flags) return Num;
			Num--;
		}
		else
		{
			if (Num2 >= Lanes.Num()) break;
			if ((Lanes[Num2].m_Flags & FlagMask) == Flags) return Num2;
			Num2++;
		}
	}
	return -1;
}

void CalculateCompositionPieceOffsets(NetCompositionData& CompositionData, TArray<NetCompositionPiece>& Pieces, const TLookup<NetPieceData>& NetPieceDataLookup)
{
	CompositionData.m_Width = 0.0f;
	CompositionData.m_MiddleOffset = 0.0f;
	CompositionData.m_WidthOffset = 0.0f;
	CompositionData.m_NodeOffset = 0.0f;
	CompositionData.m_HeightRange = Bounds1(FLT_MAX, -FLT_MAX);
	CompositionData.m_SurfaceHeight = Bounds1(FLT_MAX, -FLT_MAX);
	const bool bTest = (CompositionData.m_Flags.m_General & CompositionFlags_General_Invert) != 0;
	float Num = 0.0f;
	float Num2 = 0.0f;
	float Num3 = 0.0f;
	float Num4 = 0.0f;
	float Num5 = 0.0f;
	float Num6 = 0.0f;
	float Num7 = 0.0f;
	bool bFlag = false;
	bool bFlag2 = false;
	bool bFlag3 = false;
	bool bFlag4 = false;
	int32 Num8 = 0;
	while (Num8 < Pieces.Num())
	{
		NetCompositionPiece Value = Pieces[Num8];
		const bool bFlag5 = (Value.m_SectionFlags & NetSectionFlags_Underground) != 0;
		const bool bFlag6 = (Value.m_SectionFlags & NetSectionFlags_Overhead) != 0;
		const bool bTest2 = (Value.m_SectionFlags & NetSectionFlags_Invert) != 0;
		const bool bTest3 = (Value.m_SectionFlags & NetSectionFlags_FlipMesh) != 0;
		const bool2 Test4((Value.m_SectionFlags & NetSectionFlags_Left) != 0, (Value.m_SectionFlags & NetSectionFlags_Right) != 0);
		const NetPieceData& PieceData2 = NetPieceDataLookup.FindChecked(Value.m_Piece);
		float Num9 = PieceData2.m_Width;
		if (!bFlag5 || (CompositionData.m_Flags.m_General & CompositionFlags_General_Elevated) == 0) CompositionData.m_HeightRange |= Value.m_Offset.y + PieceData2.m_HeightRange;
		if (!bFlag5 && !bFlag6 && (Value.m_PieceFlags & NetPieceFlags_PreserveShape) == 0)
		{
			if ((Value.m_PieceFlags & NetPieceFlags_Side) != 0)
			{
				float4 FalseValue = math::select(PieceData2.m_SurfaceHeights, PieceData2.m_SurfaceHeights.yxwz(), bTest2);
				FalseValue = math::select(FalseValue, FalseValue.zwxy(), bTest3);
				CompositionData.m_EdgeHeights = math::select(CompositionData.m_EdgeHeights, FalseValue, Test4.xyxy());
				CompositionData.m_SideConnectionOffset = math::select(CompositionData.m_SideConnectionOffset, float2(PieceData2.m_SideConnectionOffset), Test4);
			}
			if ((Value.m_PieceFlags & NetPieceFlags_Surface) != 0)
			{
				CompositionData.m_SurfaceHeight.min = math::min(CompositionData.m_SurfaceHeight.min, Value.m_Offset.y + math::cmin(PieceData2.m_SurfaceHeights));
				CompositionData.m_SurfaceHeight.max = math::max(CompositionData.m_SurfaceHeight.max, Value.m_Offset.y + math::cmax(PieceData2.m_SurfaceHeights));
				bFlag = true;
			}
		}
		CompositionData.m_WidthOffset = math::max(CompositionData.m_WidthOffset, PieceData2.m_WidthOffset);
		CompositionData.m_NodeOffset = math::max(CompositionData.m_NodeOffset, PieceData2.m_NodeOffset);
		Value.m_Size.x = PieceData2.m_Width;
		Value.m_Size.y = PieceData2.m_HeightRange.max - PieceData2.m_HeightRange.min;
		Value.m_Size.z = PieceData2.m_Length;
		int32 I;
		for (I = Num8 + 1; I < Pieces.Num(); I++)
		{
			NetCompositionPiece Value2 = Pieces[I];
			if (Value2.m_SectionIndex != Value.m_SectionIndex) break;
			const NetPieceData& PieceData3 = NetPieceDataLookup.FindChecked(Value2.m_Piece);
			Num9 = math::max(Num9, PieceData3.m_Width);
			if (!bFlag5 || (CompositionData.m_Flags.m_General & CompositionFlags_General_Elevated) == 0) CompositionData.m_HeightRange |= Value2.m_Offset.y + PieceData3.m_HeightRange;
			if (!bFlag5 && !bFlag6 && (Value2.m_PieceFlags & NetPieceFlags_PreserveShape) == 0)
			{
				if ((Value2.m_PieceFlags & NetPieceFlags_Side) != 0)
				{
					float4 FalseValue2 = math::select(PieceData3.m_SurfaceHeights, PieceData3.m_SurfaceHeights.yxwz(), bTest2);
					FalseValue2 = math::select(FalseValue2, FalseValue2.zwxy(), bTest3);
					CompositionData.m_EdgeHeights = math::select(CompositionData.m_EdgeHeights, FalseValue2, Test4.xyxy());
				}
				if ((Value2.m_PieceFlags & NetPieceFlags_Surface) != 0)
				{
					CompositionData.m_SurfaceHeight.min = math::min(CompositionData.m_SurfaceHeight.min, Value2.m_Offset.y + math::cmin(PieceData3.m_SurfaceHeights));
					CompositionData.m_SurfaceHeight.max = math::max(CompositionData.m_SurfaceHeight.max, Value2.m_Offset.y + math::cmax(PieceData3.m_SurfaceHeights));
					bFlag = true;
				}
			}
			CompositionData.m_WidthOffset = math::max(CompositionData.m_WidthOffset, PieceData3.m_WidthOffset);
			CompositionData.m_NodeOffset = math::max(CompositionData.m_NodeOffset, PieceData3.m_NodeOffset);
			Value2.m_Size.x = PieceData3.m_Width;
			Value2.m_Size.y = PieceData3.m_HeightRange.max - PieceData3.m_HeightRange.min;
			Value2.m_Size.z = PieceData3.m_Length;
			Pieces[I] = Value2;
		}
		const float X = Value.m_Offset.x;
		if (bFlag5)
		{
			Value.m_Offset.x += Num + Num9 * 0.5f;
			Num += Num9;
			if ((Value.m_SectionFlags & (NetSectionFlags_Median | NetSectionFlags_AlignCenter)) == NetSectionFlags_Median)
			{
				Num2 = Value.m_Offset.x - math::select(X * 2.0f, 0.0f, bTest);
				Num3 = X;
				bFlag3 = true;
			}
			else if ((Value.m_SectionFlags & (NetSectionFlags_Right | NetSectionFlags_AlignCenter)) == NetSectionFlags_Right && !bFlag3)
			{
				Num2 = Value.m_Offset.x - Value.m_Size.x * 0.5f - math::select(X * 2.0f, 0.0f, bTest);
				Num3 = X;
				bFlag3 = true;
			}
		}
		else if (bFlag6)
		{
			Value.m_Offset.x += Num4 + Num9 * 0.5f;
			Num4 += Num9;
			if ((Value.m_SectionFlags & (NetSectionFlags_Median | NetSectionFlags_AlignCenter)) == NetSectionFlags_Median)
			{
				Num5 = Value.m_Offset.x - math::select(X * 2.0f, 0.0f, bTest);
				Num6 = X;
				bFlag4 = true;
			}
			else if ((Value.m_SectionFlags & (NetSectionFlags_Right | NetSectionFlags_AlignCenter)) == NetSectionFlags_Right && !bFlag4)
			{
				Num5 = Value.m_Offset.x - Value.m_Size.x * 0.5f - math::select(X * 2.0f, 0.0f, bTest);
				Num6 = X;
				bFlag4 = true;
			}
		}
		else
		{
			Value.m_Offset.x += CompositionData.m_Width + Num9 * 0.5f;
			CompositionData.m_Width += Num9;
			if ((Value.m_SectionFlags & (NetSectionFlags_Median | NetSectionFlags_AlignCenter)) == NetSectionFlags_Median)
			{
				CompositionData.m_MiddleOffset = Value.m_Offset.x - math::select(X * 2.0f, 0.0f, bTest);
				Num7 = X;
				bFlag2 = true;
			}
			else if ((Value.m_SectionFlags & (NetSectionFlags_Right | NetSectionFlags_AlignCenter)) == NetSectionFlags_Right && !bFlag2)
			{
				CompositionData.m_MiddleOffset = Value.m_Offset.x - Value.m_Size.x * 0.5f - math::select(X * 2.0f, 0.0f, bTest);
				Num7 = X;
				bFlag2 = true;
			}
		}
		Pieces[Num8] = Value;
		for (int32 J = Num8 + 1; J < I; J++)
		{
			NetCompositionPiece Value3 = Pieces[J];
			Value3.m_Offset.x = Value.m_Offset.x;
			Pieces[J] = Value3;
		}
		if ((Value.m_PieceFlags & NetPieceFlags_Side) != 0 && I > Num8 + 1 && (Value.m_SectionFlags & (NetSectionFlags_Left | NetSectionFlags_Right)) != 0)
		{
			const bool bTest5 = (Value.m_SectionFlags & NetSectionFlags_Right) != 0;
			for (int32 K = Num8; K < I; K++)
			{
				NetCompositionPiece Value4 = Pieces[K];
				const float Num10 = (NetPieceDataLookup.FindChecked(Value4.m_Piece).m_Width - Num9) * 0.5f;
				Value4.m_Offset.x += math::select(-Num10, Num10, bTest5);
				Pieces[K] = Value4;
			}
		}
		Num8 = I;
	}
	if (bFlag3) Num2 -= Num * 0.5f;
	if (bFlag4) Num5 -= Num4 * 0.5f;
	if (bFlag2) CompositionData.m_MiddleOffset -= CompositionData.m_Width * 0.5f;
	if ((CompositionData.m_Flags.m_General & (CompositionFlags_General_DeadEnd | CompositionFlags_General_LevelCrossing)) == CompositionFlags_General_LevelCrossing
		|| (CompositionData.m_Flags.m_General & (CompositionFlags_General_DeadEnd | CompositionFlags_General_Intersection | CompositionFlags_General_Crosswalk)) == CompositionFlags_General_Crosswalk
		|| (CompositionData.m_Flags.m_Right & CompositionFlags_Side_AbruptEnd) != 0)
	{
		CompositionData.m_State |= CompositionState_BlockUTurn;
	}
	uint CompositionStateMask = 0;
	for (int32 L = 0; L < Pieces.Num(); L++)
	{
		NetCompositionPiece Value5 = Pieces[L];
		const bool bFlag7 = (Value5.m_SectionFlags & NetSectionFlags_Underground) != 0;
		const bool bFlag8 = (Value5.m_SectionFlags & NetSectionFlags_Overhead) != 0;
		if ((Value5.m_PieceFlags & (NetPieceFlags_PreserveShape | NetPieceFlags_BlockTraffic)) == NetPieceFlags_BlockTraffic) CompositionData.m_State |= CompositionState_BlockUTurn;
		if ((Value5.m_PieceFlags & NetPieceFlags_LowerBottomToTerrain) != 0) CompositionData.m_State |= CompositionState_LowerToTerrain;
		if ((Value5.m_PieceFlags & NetPieceFlags_RaiseTopToTerrain) != 0) CompositionData.m_State |= CompositionState_RaiseToTerrain;
		if ((Value5.m_SectionFlags & NetSectionFlags_HalfLength) != 0) CompositionData.m_State |= CompositionState_HalfLength;
		if ((Value5.m_SectionFlags & NetSectionFlags_Hidden) != 0) CompositionData.m_State |= CompositionState_Hidden;
		else if (!bFlag7 && !bFlag8) CompositionStateMask |= CompositionState_Hidden;
		if (bFlag7)
		{
			Value5.m_Offset.x -= Num * 0.5f + Num3;
			Value5.m_Offset.x += CompositionData.m_MiddleOffset - Num2;
		}
		else if (bFlag8)
		{
			Value5.m_Offset.x -= Num4 * 0.5f + Num6;
			Value5.m_Offset.x += CompositionData.m_MiddleOffset - Num5;
		}
		else Value5.m_Offset.x -= CompositionData.m_Width * 0.5f + Num7;
		Pieces[L] = Value5;
	}
	CompositionData.m_State &= ~CompositionStateMask;
	CompositionData.m_Width = math::max(CompositionData.m_Width, math::max(Num, Num4));
	if (CompositionData.m_HeightRange.min > CompositionData.m_HeightRange.max) CompositionData.m_HeightRange = Bounds1();
	if (bFlag)
	{
		CompositionData.m_State |= CompositionState_HasSurface;
		if ((CompositionData.m_Flags.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) == 0) CompositionData.m_State |= CompositionState_ExclusiveGround;
	}
	else
	{
		const float Num11 = MathUtils::Center(CompositionData.m_HeightRange);
		CompositionData.m_SurfaceHeight = Bounds1(Num11, Num11);
	}
}

void CalculateSyncVertexOffsets(NetCompositionData& CompositionData, const TArray<NetCompositionPiece>& Pieces, const TLookup<NetVertexMatchData>& VertexMatchData)
{
	float4 Left(0.0f, 0.0f, 0.0f, 1.0f);
	float4 Right(0.0f, 1.0f, 1.0f, 1.0f);
	const float MiddleOffset = CompositionData.m_MiddleOffset;
	const float Num = CompositionData.m_Width * 0.5f + MiddleOffset;
	const float Num2 = CompositionData.m_Width * 0.5f - MiddleOffset;
	bool bFlag = false;
	float2 F;
	for (int32 I = 0; I < Pieces.Num(); I++)
	{
		const NetCompositionPiece& Piece = Pieces[I];
		if ((Piece.m_SectionFlags & (NetSectionFlags_Underground | NetSectionFlags_Overhead)) != 0) continue;
		if ((Piece.m_SectionFlags & NetSectionFlags_Median) != 0)
		{
			if (const NetVertexMatchData* Match = VertexMatchData.Find(Piece.m_Piece))
			{
				if (!math::any(math::isnan(float2(Match->m_Offsets.x, Match->m_Offsets.y))))
				{
					F.x = Match->m_Offsets.x;
					F.y = math::select(Match->m_Offsets.z, Match->m_Offsets.y, math::isnan(Match->m_Offsets.z));
					if ((Piece.m_SectionFlags & NetSectionFlags_Invert) != 0) F = -F.yx();
					F += Piece.m_Offset.x;
					if (Num > 0.0f) Left.w = (F.x - MiddleOffset) / Num + 1.0f;
					if (Num2 > 0.0f) Right.x = (F.y - MiddleOffset) / Num2;
					bFlag = true;
				}
			}
			if (!bFlag)
			{
				float2 F2 = Piece.m_Offset.x;
				F2.x -= Piece.m_Size.x * 0.5f;
				F2.y += Piece.m_Size.x * 0.5f;
				if (Num > 0.0f) Left.w = (F2.x - MiddleOffset) / Num + 1.0f;
				if (Num2 > 0.0f) Right.x = (F2.y - MiddleOffset) / Num2;
			}
		}
		else
		{
			const NetVertexMatchData* Match = VertexMatchData.Find(Piece.m_Piece);
			if (!Match) continue;
			if (math::isnan(Match->m_Offsets.x)) continue;
			float Num3 = Match->m_Offsets.x;
			for (int32 J = 0; J < 3; J++)
			{
				if ((Piece.m_SectionFlags & NetSectionFlags_Invert) != 0) Num3 = -Num3;
				Num3 += Piece.m_Offset.x;
				if ((Piece.m_SectionFlags & NetSectionFlags_Right) != 0)
				{
					Num3 = (Num3 - MiddleOffset) / Num2;
					if (Right.z != 1.0f) Right.w = Num3;
					else if (Right.y != 1.0f) Right.z = Num3;
					else Right.y = Num3;
				}
				else
				{
					Num3 = (Num3 - MiddleOffset) / Num + 1.0f;
					if (Left.y != 0.0f) Left.x = Num3;
					else if (Left.z != 0.0f) Left.y = Num3;
					else Left.z = Num3;
				}
				if (J == 0)
				{
					if (math::isnan(Match->m_Offsets.y)) break;
					Num3 = Match->m_Offsets.y;
				}
				else
				{
					if (math::isnan(Match->m_Offsets.z)) break;
					Num3 = Match->m_Offsets.z;
				}
			}
		}
	}
	if (Left.x > Left.y) Left.set_xy(Left.yx());
	if (Right.z > Right.w) Right.set_zw(Right.wz());
	if (Left.y > Left.z) Left.set_yz(Left.zy());
	if (Right.y > Right.z) Right.set_yz(Right.zy());
	if (Left.x > Left.y) Left.set_xy(Left.yx());
	if (Right.z > Right.w) Right.set_zw(Right.wz());
	if (Left.z <= Left.x) Left.z = math::lerp(Left.x, Left.w, 2.0f / 3.0f);
	if (Right.y >= Right.w) Right.y = math::lerp(Right.w, Right.x, 2.0f / 3.0f);
	if (Left.y <= Left.x) Left.y = math::lerp(Left.x, Left.z, 0.5f);
	if (Right.z >= Right.w) Right.z = math::lerp(Right.w, Right.y, 0.5f);
	if (Left.y < Left.x + 1E-05f) Left.y = Left.x;
	if (Left.w < Left.z + 1E-05f) Left.z = Left.w;
	if (Right.y < Right.x + 1E-05f) Right.y = Right.x;
	if (Right.w < Right.z + 1E-05f) Right.z = Right.w;
	CompositionData.m_SyncVertexOffsetsLeft = Left;
	CompositionData.m_SyncVertexOffsetsRight = Right;
}
}

void GetRequirementFlags(const TArray<uint>& Requirements, CompositionFlags& OutCompositionFlags, uint& OutSectionFlags)
{
	OutCompositionFlags = CompositionFlags{};
	OutSectionFlags = 0;
	for (uint Requirement : Requirements) GetRequirementFlags(Requirement, OutCompositionFlags, OutSectionFlags);
}

void GetRequirementFlags(uint Requirement, CompositionFlags& F, uint& SectionFlags)
{
	switch (Requirement)
	{
	case NetPieceRequirements_Node: F.m_General |= CompositionFlags_General_Node; break;
	case NetPieceRequirements_Intersection: F.m_General |= CompositionFlags_General_Intersection; break;
	case NetPieceRequirements_DeadEnd: F.m_General |= CompositionFlags_General_DeadEnd; break;
	case NetPieceRequirements_Crosswalk: F.m_General |= CompositionFlags_General_Crosswalk; break;
	case NetPieceRequirements_BusStop: F.m_Right |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_Median: SectionFlags |= NetSectionFlags_Median; break;
	case NetPieceRequirements_TrainStop: F.m_Right |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_OppositeTrainStop: F.m_Left |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_Inverted: SectionFlags |= NetSectionFlags_Invert; break;
	case NetPieceRequirements_TaxiStand: F.m_Right |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_LevelCrossing: F.m_General |= CompositionFlags_General_LevelCrossing; break;
	case NetPieceRequirements_Elevated: F.m_General |= CompositionFlags_General_Elevated; break;
	case NetPieceRequirements_Tunnel: F.m_General |= CompositionFlags_General_Tunnel; break;
	case NetPieceRequirements_Raised: F.m_Right |= CompositionFlags_Side_Raised; break;
	case NetPieceRequirements_Lowered: F.m_Right |= CompositionFlags_Side_Lowered; break;
	case NetPieceRequirements_LowTransition: F.m_Right |= CompositionFlags_Side_LowTransition; break;
	case NetPieceRequirements_HighTransition: F.m_Right |= CompositionFlags_Side_HighTransition; break;
	case NetPieceRequirements_WideMedian: F.m_General |= CompositionFlags_General_WideMedian; break;
	case NetPieceRequirements_TramTrack: F.m_Right |= CompositionFlags_Side_PrimaryTrack; break;
	case NetPieceRequirements_TramStop: F.m_Right |= CompositionFlags_Side_SecondaryStop; break;
	case NetPieceRequirements_OppositeTramTrack: F.m_Left |= CompositionFlags_Side_PrimaryTrack; break;
	case NetPieceRequirements_OppositeTramStop: F.m_Left |= CompositionFlags_Side_SecondaryStop; break;
	case NetPieceRequirements_MedianBreak: F.m_General |= CompositionFlags_General_MedianBreak; break;
	case NetPieceRequirements_ShipStop: F.m_Right |= CompositionFlags_Side_TertiaryStop; break;
	case NetPieceRequirements_Sidewalk: F.m_Right |= CompositionFlags_Side_Sidewalk; break;
	case NetPieceRequirements_Edge: F.m_General |= CompositionFlags_General_Edge; break;
	case NetPieceRequirements_SubwayStop: F.m_Right |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_OppositeSubwayStop: F.m_Left |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_MiddlePlatform: F.m_General |= CompositionFlags_General_MiddlePlatform; break;
	case NetPieceRequirements_Underground: SectionFlags |= NetSectionFlags_Underground; break;
	case NetPieceRequirements_Roundabout: F.m_General |= CompositionFlags_General_Roundabout; break;
	case NetPieceRequirements_OppositeSidewalk: F.m_Left |= CompositionFlags_Side_Sidewalk; break;
	case NetPieceRequirements_SoundBarrier: F.m_Right |= CompositionFlags_Side_SoundBarrier; break;
	case NetPieceRequirements_Overhead: SectionFlags |= NetSectionFlags_Overhead; break;
	case NetPieceRequirements_TrafficLights: F.m_General |= CompositionFlags_General_TrafficLights; break;
	case NetPieceRequirements_PublicTransportLane: F.m_Right |= CompositionFlags_Side_PrimaryLane; break;
	case NetPieceRequirements_OppositePublicTransportLane: F.m_Left |= CompositionFlags_Side_PrimaryLane; break;
	case NetPieceRequirements_Spillway: F.m_General |= CompositionFlags_General_Spillway; break;
	case NetPieceRequirements_MiddleGrass: F.m_General |= CompositionFlags_General_PrimaryMiddleBeautification; break;
	case NetPieceRequirements_MiddleTrees: F.m_General |= CompositionFlags_General_SecondaryMiddleBeautification; break;
	case NetPieceRequirements_WideSidewalk: F.m_Right |= CompositionFlags_Side_WideSidewalk; break;
	case NetPieceRequirements_SideGrass: F.m_Right |= CompositionFlags_Side_PrimaryBeautification; break;
	case NetPieceRequirements_SideTrees: F.m_Right |= CompositionFlags_Side_SecondaryBeautification; break;
	case NetPieceRequirements_OppositeGrass: F.m_Left |= CompositionFlags_Side_PrimaryBeautification; break;
	case NetPieceRequirements_OppositeTrees: F.m_Left |= CompositionFlags_Side_SecondaryBeautification; break;
	case NetPieceRequirements_Opening: F.m_General |= CompositionFlags_General_Opening; break;
	case NetPieceRequirements_Front: F.m_General |= CompositionFlags_General_Front; break;
	case NetPieceRequirements_Back: F.m_General |= CompositionFlags_General_Back; break;
	case NetPieceRequirements_Flipped: SectionFlags |= NetSectionFlags_FlipMesh; break;
	case NetPieceRequirements_RemoveTrafficLights: F.m_General |= CompositionFlags_General_RemoveTrafficLights; break;
	case NetPieceRequirements_AllWayStop: F.m_General |= CompositionFlags_General_AllWayStop; break;
	case NetPieceRequirements_Pavement: F.m_General |= CompositionFlags_General_Pavement; break;
	case NetPieceRequirements_Gravel: F.m_General |= CompositionFlags_General_Gravel; break;
	case NetPieceRequirements_Tiles: F.m_General |= CompositionFlags_General_Tiles; break;
	case NetPieceRequirements_ForbidLeftTurn: F.m_Right |= CompositionFlags_Side_ForbidLeftTurn; break;
	case NetPieceRequirements_ForbidRightTurn: F.m_Right |= CompositionFlags_Side_ForbidRightTurn; break;
	case NetPieceRequirements_OppositeWideSidewalk: F.m_Left |= CompositionFlags_Side_WideSidewalk; break;
	case NetPieceRequirements_OppositeForbidLeftTurn: F.m_Left |= CompositionFlags_Side_ForbidLeftTurn; break;
	case NetPieceRequirements_OppositeForbidRightTurn: F.m_Left |= CompositionFlags_Side_ForbidRightTurn; break;
	case NetPieceRequirements_OppositeSoundBarrier: F.m_Left |= CompositionFlags_Side_SoundBarrier; break;
	case NetPieceRequirements_SidePlatform: F.m_Right |= CompositionFlags_Side_Sidewalk; break;
	case NetPieceRequirements_AddCrosswalk: F.m_Right |= CompositionFlags_Side_AddCrosswalk; break;
	case NetPieceRequirements_RemoveCrosswalk: F.m_Right |= CompositionFlags_Side_RemoveCrosswalk; break;
	case NetPieceRequirements_Lighting: F.m_General |= CompositionFlags_General_Lighting; break;
	case NetPieceRequirements_OppositeBusStop: F.m_Left |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_OppositeTaxiStand: F.m_Left |= CompositionFlags_Side_PrimaryStop; break;
	case NetPieceRequirements_OppositeRaised: F.m_Left |= CompositionFlags_Side_Raised; break;
	case NetPieceRequirements_OppositeLowered: F.m_Left |= CompositionFlags_Side_Lowered; break;
	case NetPieceRequirements_OppositeLowTransition: F.m_Left |= CompositionFlags_Side_LowTransition; break;
	case NetPieceRequirements_OppositeHighTransition: F.m_Left |= CompositionFlags_Side_HighTransition; break;
	case NetPieceRequirements_OppositeShipStop: F.m_Left |= CompositionFlags_Side_TertiaryStop; break;
	case NetPieceRequirements_OppositePlatform: F.m_Left |= CompositionFlags_Side_Sidewalk; break;
	case NetPieceRequirements_OppositeAddCrosswalk: F.m_Left |= CompositionFlags_Side_AddCrosswalk; break;
	case NetPieceRequirements_OppositeRemoveCrosswalk: F.m_Left |= CompositionFlags_Side_RemoveCrosswalk; break;
	case NetPieceRequirements_Inside: F.m_General |= CompositionFlags_General_Inside; break;
	case NetPieceRequirements_ForbidStraight: F.m_Right |= CompositionFlags_Side_ForbidStraight; break;
	case NetPieceRequirements_OppositeForbidStraight: F.m_Left |= CompositionFlags_Side_ForbidStraight; break;
	case NetPieceRequirements_Hidden: SectionFlags |= NetSectionFlags_Hidden; break;
	case NetPieceRequirements_ParkingSpaces: F.m_Right |= CompositionFlags_Side_ParkingSpaces; break;
	case NetPieceRequirements_OppositeParkingSpaces: F.m_Left |= CompositionFlags_Side_ParkingSpaces; break;
	case NetPieceRequirements_FixedNodeSize: F.m_General |= CompositionFlags_General_FixedNodeSize; break;
	case NetPieceRequirements_HalfLength: SectionFlags |= NetSectionFlags_HalfLength; break;
	case NetPieceRequirements_AbruptEnd: F.m_Right |= CompositionFlags_Side_AbruptEnd; break;
	case NetPieceRequirements_OppositeAbruptEnd: F.m_Left |= CompositionFlags_Side_AbruptEnd; break;
	case NetPieceRequirements_AttachmentTrack: F.m_Right |= CompositionFlags_Side_SecondaryTrack; break;
	case NetPieceRequirements_EnterGate: F.m_Right |= CompositionFlags_Side_Gate; break;
	case NetPieceRequirements_ExitGate: F.m_Left |= CompositionFlags_Side_Gate; break;
	case NetPieceRequirements_StyleBreak: F.m_General |= CompositionFlags_General_StyleBreak; break;
	case NetPieceRequirements_FerryStop: F.m_Right |= CompositionFlags_Side_TertiaryStop; break;
	case NetPieceRequirements_StraightNodeEnd: F.m_General |= CompositionFlags_General_StraightNodeEnd; break;
	case NetPieceRequirements_BicycleLane: F.m_Right |= CompositionFlags_Side_SecondaryLane; break;
	case NetPieceRequirements_OppositeBicycleLane: F.m_Left |= CompositionFlags_Side_SecondaryLane; break;
	case NetPieceRequirements_ForbidBicycles: F.m_Right |= CompositionFlags_Side_ForbidSecondary; break;
	case NetPieceRequirements_OppositeForbidBicycles: F.m_Left |= CompositionFlags_Side_ForbidSecondary; break;
	case NetPieceRequirements_ToggleZones: F.m_Right |= CompositionFlags_Side_ZonesDisabled; break;
	case NetPieceRequirements_Fence: F.m_Right |= CompositionFlags_Side_Fence; break;
	case NetPieceRequirements_OppositeFence: F.m_Left |= CompositionFlags_Side_Fence; break;
	default: break;
	}
}

CompositionFlags InvertCompositionFlags(CompositionFlags Flags)
{
	return MakeFlags(Flags.m_General, Flags.m_Right, Flags.m_Left);
}

uint InvertSectionFlags(uint Flags)
{
	return Flags;
}

bool TestSectionFlags(const NetGeometrySection& Section, CompositionFlags Flags)
{
	if (((Section.m_CompositionAll | Section.m_CompositionNone) & Flags) != Section.m_CompositionAll) return false;
	if (IsDefault(Section.m_CompositionAny)) return true;
	return !IsDefault(Section.m_CompositionAny & Flags);
}

bool TestSubSectionFlags(const NetSubSection& SubSection, CompositionFlags Flags, uint SectionFlags)
{
	if ((SectionFlags & NetSectionFlags_Median) == 0) Flags.m_General &= ~CompositionFlags_General_MedianBreak;
	if (((SubSection.m_CompositionAll | SubSection.m_CompositionNone) & Flags) != SubSection.m_CompositionAll) return false;
	if (((SubSection.m_SectionAll | SubSection.m_SectionNone) & SectionFlags) != SubSection.m_SectionAll) return false;
	if (IsDefault(SubSection.m_CompositionAny) && SubSection.m_SectionAny == 0) return true;
	if (IsDefault(SubSection.m_CompositionAny & Flags)) return (SubSection.m_SectionAny & SectionFlags) != 0;
	return true;
}

bool TestPieceFlags(const NetSectionPiece& Piece, CompositionFlags Flags, uint SectionFlags)
{
	if ((SectionFlags & NetSectionFlags_Median) == 0) Flags.m_General &= ~CompositionFlags_General_MedianBreak;
	if (((Piece.m_CompositionAll | Piece.m_CompositionNone) & Flags) != Piece.m_CompositionAll) return false;
	if (((Piece.m_SectionAll | Piece.m_SectionNone) & SectionFlags) != Piece.m_SectionAll) return false;
	if (IsDefault(Piece.m_CompositionAny) && Piece.m_SectionAny == 0) return true;
	if (IsDefault(Piece.m_CompositionAny & Flags)) return (Piece.m_SectionAny & SectionFlags) != 0;
	return true;
}

bool TestPieceFlags2(const NetSectionPiece& Piece, CompositionFlags Flags, uint SectionFlags)
{
	if ((Flags.m_General & CompositionFlags_General_Roundabout) != 0 && (Piece.m_Flags & NetPieceFlags_Side) != 0)
	{
		CompositionFlags Flags2 = Flags;
		if ((Flags2.m_General & CompositionFlags_General_Elevated) != 0)
		{
			if ((Flags2.m_Left & CompositionFlags_Side_HighTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Elevated;
				Flags2.m_Left &= ~CompositionFlags_Side_HighTransition;
			}
			else if ((Flags2.m_Left & CompositionFlags_Side_LowTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Elevated;
				Flags2.m_Left &= ~CompositionFlags_Side_LowTransition;
				Flags2.m_Left |= CompositionFlags_Side_Raised;
			}
			if ((Flags2.m_Right & CompositionFlags_Side_HighTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Elevated;
				Flags2.m_Right &= ~CompositionFlags_Side_HighTransition;
			}
			else if ((Flags2.m_Right & CompositionFlags_Side_LowTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Elevated;
				Flags2.m_Right &= ~CompositionFlags_Side_LowTransition;
				Flags2.m_Right |= CompositionFlags_Side_Raised;
			}
		}
		else if ((Flags2.m_General & CompositionFlags_General_Tunnel) != 0)
		{
			if ((Flags2.m_Left & CompositionFlags_Side_HighTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Tunnel;
				Flags2.m_Left &= ~CompositionFlags_Side_HighTransition;
			}
			else if ((Flags2.m_Left & CompositionFlags_Side_LowTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Tunnel;
				Flags2.m_Left &= ~CompositionFlags_Side_LowTransition;
				Flags2.m_Left |= CompositionFlags_Side_Lowered;
			}
			if ((Flags2.m_Right & CompositionFlags_Side_HighTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Tunnel;
				Flags2.m_Right &= ~CompositionFlags_Side_HighTransition;
			}
			else if ((Flags2.m_Right & CompositionFlags_Side_LowTransition) != 0)
			{
				Flags2.m_General &= ~CompositionFlags_General_Tunnel;
				Flags2.m_Right &= ~CompositionFlags_Side_LowTransition;
				Flags2.m_Right |= CompositionFlags_Side_Lowered;
			}
		}
		else
		{
			if ((Flags2.m_Left & CompositionFlags_Side_LowTransition) != 0)
			{
				if ((Flags2.m_Left & CompositionFlags_Side_Raised) != 0) Flags2.m_Left &= ~(CompositionFlags_Side_Raised | CompositionFlags_Side_LowTransition);
				else if ((Flags2.m_Left & CompositionFlags_Side_Lowered) != 0) Flags2.m_Left &= ~(CompositionFlags_Side_Lowered | CompositionFlags_Side_LowTransition);
				else if ((Flags2.m_Left & CompositionFlags_Side_SoundBarrier) != 0) Flags2.m_Left &= ~(CompositionFlags_Side_LowTransition | CompositionFlags_Side_SoundBarrier);
				else if ((Flags2.m_Left & CompositionFlags_Side_Fence) != 0) Flags2.m_Left &= ~(CompositionFlags_Side_LowTransition | CompositionFlags_Side_Fence);
			}
			if ((Flags2.m_Right & CompositionFlags_Side_LowTransition) != 0)
			{
				if ((Flags2.m_Right & CompositionFlags_Side_Raised) != 0) Flags2.m_Right &= ~(CompositionFlags_Side_Raised | CompositionFlags_Side_LowTransition);
				else if ((Flags2.m_Right & CompositionFlags_Side_Lowered) != 0) Flags2.m_Right &= ~(CompositionFlags_Side_Lowered | CompositionFlags_Side_LowTransition);
				else if ((Flags2.m_Right & CompositionFlags_Side_SoundBarrier) != 0) Flags2.m_Right &= ~(CompositionFlags_Side_LowTransition | CompositionFlags_Side_SoundBarrier);
				else if ((Flags2.m_Right & CompositionFlags_Side_Fence) != 0) Flags2.m_Right &= ~(CompositionFlags_Side_LowTransition | CompositionFlags_Side_Fence);
			}
		}
		if (Flags != Flags2) return TestPieceFlags(Piece, Flags2, SectionFlags);
	}
	return false;
}

bool TestEdgeFlags(const NetGeometryEdgeState& EdgeState, CompositionFlags Flags)
{
	if (((EdgeState.m_CompositionAll | EdgeState.m_CompositionNone) & Flags) != EdgeState.m_CompositionAll) return false;
	if (IsDefault(EdgeState.m_CompositionAny)) return true;
	return !IsDefault(EdgeState.m_CompositionAny & Flags);
}

bool TestEdgeFlags(const NetGeometryNodeState& NodeState, CompositionFlags Flags)
{
	if (((NodeState.m_CompositionAll | NodeState.m_CompositionNone) & Flags) != NodeState.m_CompositionAll) return false;
	if (IsDefault(NodeState.m_CompositionAny)) return true;
	return !IsDefault(NodeState.m_CompositionAny & Flags);
}

bool TestEdgeMatch(const NetGeometryNodeState& NodeState, bool2 Match)
{
	switch (NodeState.m_MatchType)
	{
	case NetEdgeMatchType_Both: return math::all(Match);
	case NetEdgeMatchType_Any: return math::any(Match);
	case NetEdgeMatchType_Exclusive: return Match.x != Match.y;
	default: return false;
	}
}

void GetCompositionPieces(TArray<NetCompositionPiece>& ResultBuffer, const TArray<NetGeometrySection>& GeometrySections, CompositionFlags Flags,
	const TBufferLookup<NetSubSection>& SubSectionData, const TBufferLookup<NetSectionPiece>& SectionPieceData)
{
	int32 Num = 0;
	int32 Num2 = 0;
	CompositionFlags InvertedFlags = InvertCompositionFlags(Flags);
	InvertedFlags.m_Left = (InvertedFlags.m_Left & 0xFFFFBFFFu) | (Flags.m_Left & CompositionFlags_Side_AbruptEnd);
	InvertedFlags.m_Right = (InvertedFlags.m_Right & 0xFFFFBFFFu) | (Flags.m_Right & CompositionFlags_Side_AbruptEnd);
	for (int32 I = 0; I < GeometrySections.Num(); I++)
	{
		NetGeometrySection Section;
		if ((Flags.m_General & CompositionFlags_General_Invert) != 0)
		{
			Section = GeometrySections[GeometrySections.Num() - 1 - I];
			Section.m_Flags ^= NetSectionFlags_Invert | NetSectionFlags_FlipLanes;
			if ((Section.m_Flags & NetSectionFlags_Left) != 0)
			{
				Section.m_Flags &= ~NetSectionFlags_Left;
				Section.m_Flags |= NetSectionFlags_Right;
			}
			else if ((Section.m_Flags & NetSectionFlags_Right) != 0)
			{
				Section.m_Flags &= ~NetSectionFlags_Right;
				Section.m_Flags |= NetSectionFlags_Left;
			}
		}
		else Section = GeometrySections[I];
		if ((Flags.m_General & CompositionFlags_General_Flip) != 0) Section.m_Flags ^= NetSectionFlags_FlipLanes;
		const CompositionFlags SectionCompositionFlags = (Section.m_Flags & NetSectionFlags_Invert) != 0 ? InvertedFlags : Flags;
		uint HiddenPieceFlags = 0;
		if ((Section.m_Flags & NetSectionFlags_HiddenSurface) != 0) HiddenPieceFlags |= NetPieceFlags_Surface;
		if ((Section.m_Flags & NetSectionFlags_HiddenBottom) != 0) HiddenPieceFlags |= NetPieceFlags_Bottom;
		if ((Section.m_Flags & NetSectionFlags_HiddenTop) != 0) HiddenPieceFlags |= NetPieceFlags_Top;
		if ((Section.m_Flags & NetSectionFlags_HiddenSide) != 0) HiddenPieceFlags |= NetPieceFlags_Side;
		if (!TestSectionFlags(Section, SectionCompositionFlags)) continue;
		const uint SectionFlags = (Section.m_Flags & NetSectionFlags_Invert) != 0 ? InvertSectionFlags(Section.m_Flags) : Section.m_Flags;
		// Follow the first matching sub-section chain (the decompiled goto loop).
		while (true)
		{
			const TArray<NetSubSection>& SubSections = SubSectionData.FindChecked(Section.m_Section);
			int32 Found = INDEX_NONE;
			for (int32 J = 0; J < SubSections.Num(); J++)
			{
				if (TestSubSectionFlags(SubSections[J], SectionCompositionFlags, SectionFlags))
				{
					Found = J;
					break;
				}
			}
			if (Found == INDEX_NONE) break;
			Section.m_Section = SubSections[Found].m_SubSection;
		}
		const TArray<NetSectionPiece>& SectionPieces = SectionPieceData.FindChecked(Section.m_Section);
		for (int32 K = 0; K < SectionPieces.Num(); K++)
		{
			const NetSectionPiece& Piece = SectionPieces[K];
			uint PieceFlags = Piece.m_Flags;
			if (!TestPieceFlags(Piece, SectionCompositionFlags, SectionFlags))
			{
				if (!TestPieceFlags2(Piece, SectionCompositionFlags, SectionFlags)) continue;
				PieceFlags |= NetPieceFlags_SkipBottomHalf;
			}
			NetCompositionPiece Value{};
			Value.m_Piece = Piece.m_Piece;
			Value.m_SectionFlags = Section.m_Flags;
			Value.m_PieceFlags = PieceFlags;
			Value.m_SectionIndex = Num;
			Value.m_Offset = Section.m_Offset + Piece.m_Offset;
			if ((HiddenPieceFlags & PieceFlags) != 0) Value.m_SectionFlags |= NetSectionFlags_Hidden;
			ResultBuffer.Add(Value);
			Num2++;
		}
		if (Num2 != 0)
		{
			Num++;
			Num2 = 0;
		}
	}
}

void CalculateCompositionData(NetCompositionData& CompositionData, TArray<NetCompositionPiece>& Pieces,
	const TLookup<NetPieceData>& PieceData, const TLookup<NetVertexMatchData>& VertexMatchData)
{
	CalculateCompositionPieceOffsets(CompositionData, Pieces, PieceData);
	CalculateSyncVertexOffsets(CompositionData, Pieces, VertexMatchData);
}

void AddCompositionLanes(int32 Entity, NetCompositionData& CompositionData, TArray<NetCompositionPiece>& Pieces, TArray<NetCompositionLane>& NetLanes,
	TArray<NetCompositionCarriageway>* Carriageways, const TLookup<NetLaneData>& LaneDataLookup, const TBufferLookup<NetPieceLane>& PieceLanes)
{
	int32 Num = 0;
	int32 Num2 = 0;
	int32 Num3 = 0;
	bool bFlag = true;
	Bounds3 BoundsTrack(float3(FLT_MAX), float3(-FLT_MAX));
	Bounds3 BoundsRoad(float3(FLT_MAX), float3(-FLT_MAX));
	Bounds3 BoundsBicycle(float3(FLT_MAX), float3(-FLT_MAX));
	bool2 bTrack;
	bool2 bRoad;
	bool2 bBicycle;
	NetCompositionCarriageway Elem{};
	TArray<FTempLaneData> TempLanes;
	TempLanes.Reserve(256);
	TArray<FTempLaneGroup> TempGroups;
	TempGroups.Reserve(32);

	auto FinishCarriageway = [&]()
	{
		if (!math::any(bRoad))
		{
			bRoad = bBicycle;
			BoundsRoad = BoundsBicycle;
		}
		if (math::all(bRoad)) bFlag = false;
		if (math::any(bTrack | bRoad) && Carriageways)
		{
			Bounds3 Bounds4 = BoundsTrack | BoundsRoad;
			bool2 X = bTrack | bRoad;
			if (math::any(bTrack != bRoad))
			{
				if (math::all(bRoad) & math::any(bTrack))
				{
					Bounds4 = BoundsTrack;
					X = bTrack;
				}
				else if (math::any(bRoad))
				{
					Bounds4 = BoundsRoad;
					X = bRoad;
				}
			}
			Elem.m_Position = MathUtils::Center(Bounds4);
			Elem.m_Width = MathUtils::Size(Bounds4).x;
			if (math::all(X))
			{
				Elem.m_Flags &= ~LaneFlags_Invert;
				Elem.m_Flags |= LaneFlags_Twoway;
			}
			else if (X.x) Elem.m_Flags &= ~(LaneFlags_Invert | LaneFlags_Twoway);
			else
			{
				Elem.m_Flags &= ~LaneFlags_Twoway;
				Elem.m_Flags |= LaneFlags_Invert;
			}
			Carriageways->Add(Elem);
		}
	};

	for (int32 I = 0; I < Pieces.Num(); I++)
	{
		const NetCompositionPiece& Piece = Pieces[I];
		if ((Piece.m_PieceFlags & NetPieceFlags_BlockTraffic) != 0 && Num3 != 0)
		{
			Num2++;
			Num3 = 0;
			FinishCarriageway();
			BoundsTrack = Bounds3(float3(FLT_MAX), float3(-FLT_MAX));
			BoundsRoad = Bounds3(float3(FLT_MAX), float3(-FLT_MAX));
			BoundsBicycle = Bounds3(float3(FLT_MAX), float3(-FLT_MAX));
			bTrack = bool2();
			bRoad = bool2();
			bBicycle = bool2();
			Elem = NetCompositionCarriageway{};
		}
		const TArray<NetPieceLane>* Lanes = PieceLanes.Find(Piece.m_Piece);
		if (!Lanes) continue;
		const bool bFlag2 = (Piece.m_SectionFlags & NetSectionFlags_Invert) != 0;
		const bool bFlag3 = (Piece.m_SectionFlags & NetSectionFlags_FlipLanes) != 0;
		for (int32 J = 0; J < Lanes->Num(); J++)
		{
			NetPieceLane PieceLane = (*Lanes)[bFlag2 ? Lanes->Num() - J - 1 : J];
			NetLaneData LaneData2 = LaneDataLookup.FindChecked(PieceLane.m_Lane);
			FTempLaneData Value;
			FTempLaneGroup Value2;
			LaneData2.m_Flags |= PieceLane.m_ExtraFlags;
			if (bFlag2) PieceLane.m_Position.x = -PieceLane.m_Position.x;
			if ((LaneData2.m_Flags & LaneFlags_Twoway) != 0)
			{
				bTrack |= bool2((LaneData2.m_Flags & LaneFlags_Track) != 0);
				bRoad |= bool2((LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == LaneFlags_Road);
				bBicycle |= bool2((LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == (LaneFlags_Road | LaneFlags_BicyclesOnly));
			}
			else if (bFlag2 != bFlag3)
			{
				LaneData2.m_Flags |= LaneFlags_Invert;
				bTrack.y |= (LaneData2.m_Flags & LaneFlags_Track) != 0;
				bRoad.y |= (LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == LaneFlags_Road;
				bBicycle.y |= (LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == (LaneFlags_Road | LaneFlags_BicyclesOnly);
			}
			else
			{
				bTrack.x |= (LaneData2.m_Flags & LaneFlags_Track) != 0;
				bRoad.x |= (LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == LaneFlags_Road;
				bBicycle.x |= (LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == (LaneFlags_Road | LaneFlags_BicyclesOnly);
			}
			const float3 F = Piece.m_Offset + PieceLane.m_Position;
			Value2.m_Flags = LaneData2.m_Flags;
			const float3 HalfWidth(LaneData2.m_Width * 0.5f, 0.0f, 0.0f);
			if ((LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == LaneFlags_Road)
			{
				BoundsRoad |= Bounds3(F - HalfWidth, F + HalfWidth);
				Elem.m_Flags |= LaneData2.m_Flags;
			}
			if ((LaneData2.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == (LaneFlags_Road | LaneFlags_BicyclesOnly))
			{
				BoundsBicycle |= Bounds3(F - HalfWidth, F + HalfWidth);
				Elem.m_Flags |= LaneData2.m_Flags;
			}
			if ((LaneData2.m_Flags & LaneFlags_Track) != 0)
			{
				BoundsTrack |= Bounds3(F - HalfWidth, F + HalfWidth);
				Elem.m_Flags |= LaneData2.m_Flags;
			}
			if (Num3 != 0)
			{
				FTempLaneGroup TempLaneGroup = TempGroups.Last();
				if (Value2.IsCompatible(TempLaneGroup))
				{
					Value.m_GroupIndex = TempGroups.Num() - 1;
					TempLanes.Add(Value);
					if ((TempLaneGroup.m_Flags & (LaneFlags_DisconnectedStart | LaneFlags_DisconnectedEnd | LaneFlags_BicyclesOnly)) != 0)
					{
						TempLaneGroup.m_Prefab = PieceLane.m_Lane;
						TempLaneGroup.m_Flags = Value2.m_Flags & (TempLaneGroup.m_Flags | LaneFlags_Track);
						TempLaneGroup.m_Position = F;
						TempLaneGroup.m_FullLaneCount = 1;
					}
					else
					{
						TempLaneGroup.m_Flags &= Value2.m_Flags | LaneFlags_Track;
						if ((Value2.m_Flags & (LaneFlags_DisconnectedStart | LaneFlags_DisconnectedEnd | LaneFlags_BicyclesOnly)) == 0)
						{
							TempLaneGroup.m_Position += F;
							TempLaneGroup.m_FullLaneCount++;
						}
					}
					TempLaneGroup.m_LaneCount++;
					TempGroups.Last() = TempLaneGroup;
					continue;
				}
			}
			Value.m_GroupIndex = Num++;
			TempLanes.Add(Value);
			Value2.m_Prefab = PieceLane.m_Lane;
			Value2.m_Position = F;
			Value2.m_LaneCount = 1;
			Value2.m_FullLaneCount = 1;
			Value2.m_CarriagewayIndex = Num2;
			TempGroups.Add(Value2);
			Num3++;
		}
	}
	if (Num3 != 0) FinishCarriageway();
	if (bFlag) CompositionData.m_State |= CompositionState_SeparatedCarriageways;
	int32 Num4 = 0;
	int32 Num5 = -1;
	int32 Num6 = 0;
	for (int32 K = 0; K < Pieces.Num(); K++)
	{
		NetCompositionPiece Piece2 = Pieces[K];
		const TArray<NetPieceLane>* Lanes2 = PieceLanes.Find(Piece2.m_Piece);
		if (!Lanes2) continue;
		const bool bFlag4 = (Piece2.m_SectionFlags & NetSectionFlags_Invert) != 0;
		const bool bFlag5 = (Piece2.m_SectionFlags & NetSectionFlags_FlipLanes) != 0;
		for (int32 L = 0; L < Lanes2->Num(); L++)
		{
			NetPieceLane PieceLane2 = (*Lanes2)[bFlag4 ? Lanes2->Num() - L - 1 : L];
			NetLaneData LaneData3 = LaneDataLookup.FindChecked(PieceLane2.m_Lane);
			const FTempLaneData& TempLaneData = TempLanes[Num4];
			const FTempLaneGroup& TempLaneGroup2 = TempGroups[TempLaneData.m_GroupIndex];
			LaneData3.m_Flags |= PieceLane2.m_ExtraFlags;
			if (bFlag4) PieceLane2.m_Position.x = -PieceLane2.m_Position.x;
			if (bFlag4 != bFlag5) LaneData3.m_Flags |= LaneFlags_Invert;
			NetCompositionLane Value3{};
			Value3.m_Lane = PieceLane2.m_Lane;
			Value3.m_Position = Piece2.m_Offset + PieceLane2.m_Position;
			Value3.m_Carriageway = static_cast<uint8>(TempLaneGroup2.m_CarriagewayIndex);
			Value3.m_Group = static_cast<uint8>(TempLaneData.m_GroupIndex);
			Value3.m_Index = static_cast<uint8>(Num4);
			Value3.m_Flags = LaneData3.m_Flags;
			if (TempLaneGroup2.m_LaneCount > 1) Value3.m_Flags |= LaneFlags_Slave;
			if (TempLaneData.m_GroupIndex != Num5)
			{
				Num5 = TempLaneData.m_GroupIndex;
				Value3.m_Flags |= (bFlag4 != bFlag5) ? LaneFlags_RightLimit : LaneFlags_LeftLimit;
				Num6 = 0;
			}
			if (++Num6 == TempLaneGroup2.m_LaneCount) Value3.m_Flags |= (bFlag4 != bFlag5) ? LaneFlags_LeftLimit : LaneFlags_RightLimit;
			NetLanes.Add(Value3);
			if ((LaneData3.m_Flags & LaneFlags_Road) != 0) Piece2.m_PieceFlags |= (Value3.m_Flags & LaneFlags_BicyclesOnly) != 0 ? NetPieceFlags_HasBicycleLanes : NetPieceFlags_HasRoadLanes;
			Num4++;
		}
		Pieces[K] = Piece2;
	}
	int2 Forward = 0;
	int2 Backward = 0;
	for (int32 M = 0; M < NetLanes.Num(); M++)
	{
		NetCompositionLane Value5 = NetLanes[M];
		if ((Value5.m_Flags & LaneFlags_Parking) != 0)
		{
			const int32 Num7 = FindClosestLane(NetLanes, M, Value5.m_Position, LaneFlags_Road | LaneFlags_BicyclesOnly, LaneFlags_Road);
			if (Num7 != -1)
			{
				NetCompositionLane Value6 = NetLanes[Num7];
				if (((Value5.m_Flags ^ Value6.m_Flags) & LaneFlags_Invert) != 0) Value5.m_Flags ^= LaneFlags_Invert;
				const uint ParkingSide = ((Num7 < M) != ((Value6.m_Flags & LaneFlags_Invert) != 0)) ? LaneFlags_ParkingRight : LaneFlags_ParkingLeft;
				Value5.m_Flags |= ParkingSide;
				NetLanes[M] = Value5;
				if ((Value5.m_Flags & LaneFlags_Virtual) == 0)
				{
					Value6.m_Flags |= ParkingSide;
					NetLanes[Num7] = Value6;
				}
			}
		}
		if ((Value5.m_Flags & (LaneFlags_Road | LaneFlags_BicyclesOnly)) == LaneFlags_Road)
		{
			if ((Value5.m_Flags & LaneFlags_Invert) != 0)
			{
				CompositionData.m_State |= CompositionState_HasBackwardRoadLanes;
				Backward.x++;
			}
			else
			{
				CompositionData.m_State |= CompositionState_HasForwardRoadLanes;
				Forward.x++;
			}
		}
		if ((Value5.m_Flags & LaneFlags_Track) != 0)
		{
			if ((Value5.m_Flags & LaneFlags_Invert) != 0)
			{
				CompositionData.m_State |= CompositionState_HasBackwardTrackLanes;
				Backward.y++;
			}
			else
			{
				CompositionData.m_State |= CompositionState_HasForwardTrackLanes;
				Forward.y++;
			}
		}
		if ((Value5.m_Flags & LaneFlags_Pedestrian) != 0) CompositionData.m_State |= CompositionState_HasPedestrianLanes;
	}
	if (math::any(Forward != Backward)) CompositionData.m_State |= CompositionState_Asymmetric;
	if (math::any(Forward > int2(1)) | math::any(Backward > int2(1))) CompositionData.m_State |= CompositionState_Multilane;
	for (int32 N = 0; N < TempGroups.Num(); N++)
	{
		const FTempLaneGroup& Group = TempGroups[N];
		if (Group.m_LaneCount > 1)
		{
			NetCompositionLane Value7{};
			Value7.m_Lane = Group.m_Prefab;
			Value7.m_Position = Group.m_Position / static_cast<float>(Group.m_FullLaneCount);
			Value7.m_Carriageway = static_cast<uint8>(Group.m_CarriagewayIndex);
			Value7.m_Group = static_cast<uint8>(N);
			Value7.m_Index = static_cast<uint8>(Num4);
			Value7.m_Flags = Group.m_Flags | LaneFlags_Master;
			NetLanes.Add(Value7);
			Num4++;
		}
	}
	if (Num4 >= 256) UE_LOG(LogTemp, Error, TEXT("CSRoad: too many lanes in composition of prefab %d"), Entity);
}

float2 CalculateRoundaboutSize(const NetCompositionData& CompositionData, const TArray<NetCompositionPiece>& Pieces)
{
	float2 F = 0.0f;
	float2 F2 = FLT_MAX;
	float4 F3 = 0.0f;
	float2 Y = 0.0f;
	float2 F4 = CompositionData.m_Width * 0.5f;
	int2 RoadRange(Pieces.Num(), -1);
	int2 BicycleRange(Pieces.Num(), -1);
	for (int32 I = 0; I < Pieces.Num(); I++)
	{
		const NetCompositionPiece& Piece = Pieces[I];
		if ((Piece.m_PieceFlags & NetPieceFlags_HasRoadLanes) != 0)
		{
			RoadRange.x = math::min(RoadRange.x, I);
			RoadRange.y = I;
		}
		else if ((Piece.m_PieceFlags & NetPieceFlags_HasBicycleLanes) != 0)
		{
			BicycleRange.x = math::min(BicycleRange.x, I);
			BicycleRange.y = I;
		}
	}
	uint LaneFlag = NetPieceFlags_HasRoadLanes;
	if (RoadRange.y < RoadRange.x && BicycleRange.y >= BicycleRange.x)
	{
		RoadRange = BicycleRange;
		LaneFlag = NetPieceFlags_HasBicycleLanes;
	}
	for (int32 J = RoadRange.x; J <= RoadRange.y; J++)
	{
		const NetCompositionPiece& Piece2 = Pieces[J];
		if (J == RoadRange.x || (Piece2.m_PieceFlags & (LaneFlag | NetPieceFlags_BlockTraffic)) == NetPieceFlags_BlockTraffic)
		{
			F4.y = CompositionData.m_Width * 0.5f;
			float Num = 0.0f;
			for (int32 K = J + 1; K < RoadRange.y; K++)
			{
				const NetCompositionPiece& Piece3 = Pieces[K];
				if ((Piece3.m_PieceFlags & LaneFlag) == 0)
				{
					if ((Piece3.m_PieceFlags & NetPieceFlags_BlockTraffic) != 0)
					{
						F4.y = Piece3.m_Offset.x - Piece3.m_Size.x * 0.5f;
						break;
					}
					Num += Piece3.m_Size.x;
				}
			}
			F4.y -= Num;
		}
		if ((Piece2.m_PieceFlags & LaneFlag) != 0)
		{
			const float Y2 = (Piece2.m_SectionFlags & NetSectionFlags_Invert) == 0
				? F4.y + Piece2.m_Size.x * 0.5f - Piece2.m_Offset.x
				: F4.x + Piece2.m_Size.x * 0.5f + Piece2.m_Offset.x;
			if (((Piece2.m_SectionFlags & NetSectionFlags_Invert) != 0) != ((Piece2.m_SectionFlags & NetSectionFlags_FlipLanes) != 0))
			{
				F.x = math::max(F.x, Y2);
				F2.y = math::min(F2.y, Y2);
				Y.x += Piece2.m_Size.x;
				F3.w = math::max(F3.w, Piece2.m_Size.x);
			}
			else
			{
				F.y = math::max(F.y, Y2);
				F2.x = math::min(F2.x, Y2);
				Y.y += Piece2.m_Size.x;
				F3.z = math::max(F3.z, Piece2.m_Size.x);
			}
		}
		else if ((Piece2.m_PieceFlags & NetPieceFlags_BlockTraffic) != 0)
		{
			F4.x = Piece2.m_Size.x * 0.5f - Piece2.m_Offset.x;
			F3.set_xy(math::max(F3.xy(), Y));
			Y = 0.0f;
		}
		else
		{
			F4.x -= Piece2.m_Size.x;
			F4.y += Piece2.m_Size.x;
		}
	}
	F3.set_xy(math::max(F3.xy(), Y));
	float2 F5 = math::select(F, math::max(F2, F), F2 < float2(FLT_MAX));
	F5 = math::select(F5, float2(CompositionData.m_Width * 0.5f), F5 == float2(0.0f));
	F5 += math::max(F3.xy(), F3.zw()) / 3.0f;
	if (LaneFlag == NetPieceFlags_HasRoadLanes && BicycleRange.y >= BicycleRange.x) F5 += 1.5f;
	return F5;
}

CompositionFlags GetElevationFlags(const Elevation& StartElevation, const Elevation& MiddleElevation, const Elevation& EndElevation, const NetGeometryData& PrefabGeometryData)
{
	CompositionFlags Result{};
	const float2 F = math::max(float2(math::max(math::cmin(StartElevation.m_Elevation), math::cmin(EndElevation.m_Elevation))), MiddleElevation.m_Elevation);
	const float3 X(math::cmin(StartElevation.m_Elevation), math::cmin(EndElevation.m_Elevation), math::cmin(MiddleElevation.m_Elevation));
	const float3 X2(math::cmax(StartElevation.m_Elevation), math::cmax(EndElevation.m_Elevation), math::cmax(MiddleElevation.m_Elevation));
	const float2 F2 = math::min(float2(math::min(math::cmax(StartElevation.m_Elevation), math::cmax(EndElevation.m_Elevation))), MiddleElevation.m_Elevation);
	if (math::all(F >= float2(PrefabGeometryData.m_ElevationLimit * 2.0f)) || (PrefabGeometryData.m_Flags & GeometryFlags_RequireElevated) != 0)
	{
		if ((PrefabGeometryData.m_Flags & GeometryFlags_ElevatedIsRaised) != 0)
		{
			Result.m_Left |= CompositionFlags_Side_Raised;
			Result.m_Right |= CompositionFlags_Side_Raised;
		}
		else Result.m_General |= CompositionFlags_General_Elevated;
	}
	else if (math::cmax(X) <= PrefabGeometryData.m_ElevationLimit * -2.0f && math::cmin(X2) <= PrefabGeometryData.m_ElevationLimit * -3.0f) Result.m_General |= CompositionFlags_General_Tunnel;
	else
	{
		if (F.x >= PrefabGeometryData.m_ElevationLimit)
		{
			if ((PrefabGeometryData.m_Flags & GeometryFlags_RaisedIsElevated) != 0) Result.m_General |= CompositionFlags_General_Elevated;
			else Result.m_Left |= CompositionFlags_Side_Raised;
		}
		else if (F2.x <= -PrefabGeometryData.m_ElevationLimit)
		{
			if ((PrefabGeometryData.m_Flags & GeometryFlags_LoweredIsTunnel) != 0) Result.m_General |= CompositionFlags_General_Tunnel;
			else Result.m_Left |= CompositionFlags_Side_Lowered;
		}
		if (F.y >= PrefabGeometryData.m_ElevationLimit)
		{
			if ((PrefabGeometryData.m_Flags & GeometryFlags_RaisedIsElevated) != 0) Result.m_General |= CompositionFlags_General_Elevated;
			else Result.m_Right |= CompositionFlags_Side_Raised;
		}
		else if (F2.y <= -PrefabGeometryData.m_ElevationLimit)
		{
			if ((PrefabGeometryData.m_Flags & GeometryFlags_LoweredIsTunnel) != 0) Result.m_General |= CompositionFlags_General_Tunnel;
			else Result.m_Right |= CompositionFlags_Side_Lowered;
		}
	}
	return Result;
}
}

namespace NetCompositionSystem
{
void AddCompositionCrosswalks(const NetCompositionData& CompositionData, const TArray<NetCompositionPiece>& Pieces, TArray<NetCompositionCrosswalk>& Crosswalks,
	const TLookup<NetCrosswalkData>& CrosswalkDataLookup, const TLookup<NetLaneData>& LaneDataLookup, const TBufferLookup<NetPieceLane>& PieceLanes)
{
	NetCrosswalkData Current{};
	bool bFlag = false;
	bool bFlag2 = false;
	bool bFlag3 = false;
	bool bFlag4 = false;
	for (int32 I = 0; I < Pieces.Num(); I++)
	{
		const NetCompositionPiece& Piece = Pieces[I];
		if ((Piece.m_PieceFlags & NetPieceFlags_Surface) == 0) continue;
		if (const NetCrosswalkData* Found = CrosswalkDataLookup.Find(Piece.m_Piece))
		{
			NetCrosswalkData Crosswalk2 = *Found;
			if (bFlag3)
			{
				if (!bFlag)
				{
					const NetLaneData& LaneData = LaneDataLookup.FindChecked(Current.m_Lane);
					NetCompositionCrosswalk Elem{};
					Elem.m_Lane = Current.m_Lane;
					Elem.m_Start = Current.m_Start;
					Elem.m_End = Current.m_End;
					Elem.m_Flags = LaneData.m_Flags;
					if (bFlag4) Elem.m_Flags |= LaneFlags_CrossRoad;
					Crosswalks.Add(Elem);
				}
				Current = NetCrosswalkData{};
				bFlag = bFlag2;
				bFlag2 = false;
				bFlag3 = false;
				bFlag4 = false;
			}
			if (!bFlag4)
			{
				if (const TArray<NetPieceLane>* Lanes = PieceLanes.Find(Piece.m_Piece))
				{
					for (const NetPieceLane& PieceLane : *Lanes)
					{
						if (PieceLane.m_Position.x >= Crosswalk2.m_Start.x && PieceLane.m_Position.x <= Crosswalk2.m_End.x && (LaneDataLookup.FindChecked(PieceLane.m_Lane).m_Flags & LaneFlags_Road) != 0)
						{
							bFlag4 = true;
							break;
						}
					}
				}
			}
			if ((Piece.m_SectionFlags & NetSectionFlags_Invert) != 0)
			{
				const float X = Crosswalk2.m_Start.x;
				Crosswalk2.m_Start.x = -Crosswalk2.m_End.x;
				Crosswalk2.m_End.x = -X;
			}
			if (Current.m_Lane == Entity_Null)
			{
				Current.m_Lane = Crosswalk2.m_Lane;
				Current.m_Start = Piece.m_Offset + Crosswalk2.m_Start;
				Current.m_End = Piece.m_Offset + Crosswalk2.m_End;
			}
			else Current.m_End = Piece.m_Offset + Crosswalk2.m_End;
			continue;
		}
		if (Piece.m_Size.x > 0.0f)
		{
			bFlag2 = false;
			if (Current.m_Lane != Entity_Null) bFlag3 = true;
		}
		if ((Piece.m_PieceFlags & NetPieceFlags_BlockCrosswalk) != 0)
		{
			bFlag = true;
			bFlag2 = true;
		}
	}
	if (Current.m_Lane != Entity_Null && !bFlag)
	{
		const NetLaneData& LaneData2 = LaneDataLookup.FindChecked(Current.m_Lane);
		NetCompositionCrosswalk Elem2{};
		Elem2.m_Lane = Current.m_Lane;
		Elem2.m_Start = Current.m_Start;
		Elem2.m_End = Current.m_End;
		Elem2.m_Flags = LaneData2.m_Flags;
		if (bFlag4) Elem2.m_Flags |= LaneFlags_CrossRoad;
		Crosswalks.Add(Elem2);
	}
}
}
}

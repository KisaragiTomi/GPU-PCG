#include "CSRoadGraph.h"

namespace CSRoad
{
namespace
{
struct FEdgeIteratorValue
{
	int32 m_Edge = 0;
	bool m_End = false;
	bool m_Middle = false;
};

// EdgeIterator for permanent nodes (no Temp/Hidden entities exist in a built network).
struct FEdgeIterator
{
	const FGraph& G;
	const TArray<int32>& Buffer;
	int32 Node = 0;
	int32 Iterator = 0;
	bool bMiddles = false;

	FEdgeIterator(const FGraph& InGraph, int32 InNode, bool bIncludeMiddleConnections = false)
		: G(InGraph), Buffer(InGraph.ConnectedEdges[InNode]), Node(InNode), bMiddles(bIncludeMiddleConnections)
	{
	}

	bool GetNext(FEdgeIteratorValue& Value)
	{
		while (Iterator < Buffer.Num())
		{
			Value.m_Edge = Buffer[Iterator++];
			const Edge& E = G.EdgeData[Value.m_Edge];
			Value.m_End = E.m_End == Node;
			if (Value.m_End || E.m_Start == Node)
			{
				Value.m_Middle = false;
				return true;
			}
			if (bMiddles)
			{
				Value.m_Middle = true;
				return true;
			}
		}
		Value = FEdgeIteratorValue();
		return false;
	}
};

// Port of CompositionSelectSystem.SelectCompositionJob's helper methods.
struct FCompositionSelect
{
	const FGraph& G;
	FAssets& A;
	bool m_LeftHandTraffic = false;

	FCompositionSelect(const FGraph& InGraph, FAssets& InAssets) : G(InGraph), A(InAssets), m_LeftHandTraffic(InGraph.bLeftHandTraffic) {}

	const NetGeometryData& PrefabGeometry(int32 Entity) const { return G.PrefabGeometry[G.PrefabRef[Entity]]; }

	// Sub-objects (net objects, attached props) are not part of an imported network, so the
	// lookups of the original always miss and both overloads return default flags.
	CompositionFlags GetSubObjectFlags(int32 /*Entity*/, const Curve& /*Curve*/, const NetGeometryData& /*Geometry*/) const { return CompositionFlags{}; }
	CompositionFlags GetSubObjectFlags(int32 /*Entity*/, const NetData& /*Net*/) const { return CompositionFlags{}; }

	bool TryGetElevation(int32 Entity, Elevation& Out) const
	{
		Out = Elevation{};
		if (!G.Has(Entity, Component_Elevation)) return false;
		Out = G.ElevationData[Entity];
		return true;
	}

	CompositionFlags GetElevationFlags(int32 EdgeEntity, int32 StartNode, int32 EndNode, const NetGeometryData& PrefabGeometryData) const
	{
		Elevation ComponentData;
		Elevation ComponentData2;
		TryGetElevation(StartNode, ComponentData);
		TryGetElevation(EndNode, ComponentData2);
		Elevation ComponentData3;
		if (EdgeEntity == Entity_Null) ComponentData3 = ComponentData;
		else TryGetElevation(EdgeEntity, ComponentData3);
		return NetCompositionHelpers::GetElevationFlags(ComponentData, ComponentData3, ComponentData2, PrefabGeometryData);
	}

	CompositionFlags GetHandednessFlags(const NetGeometryData& PrefabGeometryData) const
	{
		CompositionFlags Result{};
		if (((PrefabGeometryData.m_Flags & GeometryFlags_IsLefthanded) != 0) != m_LeftHandTraffic)
		{
			if ((PrefabGeometryData.m_Flags & GeometryFlags_InvertCompositionHandedness) != 0) Result.m_General |= CompositionFlags_General_Invert;
			if ((PrefabGeometryData.m_Flags & GeometryFlags_FlipCompositionHandedness) != 0) Result.m_General |= CompositionFlags_General_Flip;
		}
		return Result;
	}

	// Prefab arguments are prefab table indices (PrefabRef values) throughout.
	CompositionFlags GetEdgeStates(int32 Prefab, CompositionFlags EdgeFlags) const
	{
		CompositionFlags Result{};
		for (const NetGeometryEdgeState& EdgeState : A.EdgeStates(G.PrefabRoadIds[Prefab]))
		{
			if (NetCompositionHelpers::TestEdgeFlags(EdgeState, EdgeFlags)) Result |= EdgeState.m_State;
		}
		return Result;
	}

	CompositionFlags GetNodeStates(const TArray<NetGeometryNodeState>& NodeStates, CompositionFlags EdgeFlags1, CompositionFlags EdgeFlags2, bool bIsLeft, bool bIsRight) const
	{
		CompositionFlags Result{};
		for (const NetGeometryNodeState& Original : NodeStates)
		{
			NetGeometryNodeState NodeState = Original;
			CompositionFlags Flags = EdgeFlags1;
			CompositionFlags Flags2 = EdgeFlags2;
			if (!bIsLeft)
			{
				NodeState.m_CompositionNone.m_Left = 0;
				NodeState.m_State.m_Left = 0;
				Flags2.m_Right = 0;
				if (NodeState.m_MatchType == NetEdgeMatchType_Exclusive) Flags.m_Left = 0;
			}
			if (!bIsRight)
			{
				NodeState.m_CompositionNone.m_Right = 0;
				NodeState.m_State.m_Right = 0;
				Flags2.m_Left = 0;
				if (NodeState.m_MatchType == NetEdgeMatchType_Exclusive) Flags.m_Right = 0;
			}
			const bool2 Match(NetCompositionHelpers::TestEdgeFlags(Original, Flags), NetCompositionHelpers::TestEdgeFlags(NodeState, Flags2));
			if (NetCompositionHelpers::TestEdgeMatch(NodeState, Match)) Result |= NodeState.m_State;
		}
		return Result;
	}

	CompositionFlags GetEdgeFlags(int32 EdgeEntity, int32 Prefab, const NetGeometryData& PrefabGeometryData, CompositionFlags UpgradeFlags, CompositionFlags SubObjectFlags,
		CompositionFlags ElevationFlags, CompositionFlags& ObsoleteEdgeFlags) const
	{
		CompositionFlags Flags = UpgradeFlags | SubObjectFlags | ElevationFlags;
		ObsoleteEdgeFlags = CompositionFlags{};
		Flags.m_General |= CompositionFlags_General_Edge;
		Flags |= GetHandednessFlags(PrefabGeometryData);
		Flags |= GetEdgeStates(Prefab, Flags);
		const bool bNum = (Flags.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) != 0;
		bool bFlag = (Flags.m_Left & (CompositionFlags_Side_Raised | CompositionFlags_Side_Lowered)) != 0;
		bool bFlag2 = (Flags.m_Right & (CompositionFlags_Side_Raised | CompositionFlags_Side_Lowered)) != 0;
		if ((PrefabGeometryData.m_Flags & (GeometryFlags_RequireElevated | GeometryFlags_ElevatedIsRaised)) == GeometryFlags_ElevatedIsRaised)
		{
			bFlag = false;
			bFlag2 = false;
		}
		if (bNum)
		{
			const uint General = UpgradeFlags.m_General & (CompositionFlags_General_PrimaryMiddleBeautification | CompositionFlags_General_SecondaryMiddleBeautification);
			Flags.m_General &= ~General;
			ObsoleteEdgeFlags.m_General |= General;
		}
		const uint SideMask = CompositionFlags_Side_PrimaryBeautification | CompositionFlags_Side_SecondaryBeautification | CompositionFlags_Side_WideSidewalk | CompositionFlags_Side_SoundBarrier | CompositionFlags_Side_ZonesDisabled;
		if (bNum || bFlag)
		{
			const uint Side = UpgradeFlags.m_Left & SideMask;
			Flags.m_Left &= ~Side;
			ObsoleteEdgeFlags.m_Left |= Side;
		}
		if (bNum || bFlag2)
		{
			const uint Side2 = UpgradeFlags.m_Right & SideMask;
			Flags.m_Right &= ~Side2;
			ObsoleteEdgeFlags.m_Right |= Side2;
		}
		return Flags;
	}

	CompositionFlags GetEdgeFlags(int32 EdgeEntity, bool bInvert, int32 Prefab, const NetGeometryData& PrefabGeometryData, CompositionFlags ElevationFlags) const
	{
		const Curve& EdgeCurve = G.CurveData[EdgeEntity];
		CompositionFlags Flags{};
		const CompositionFlags SubObjectFlags = GetSubObjectFlags(EdgeEntity, EdgeCurve, PrefabGeometryData);
		if (G.Has(EdgeEntity, Component_Upgraded)) Flags = G.UpgradedData[EdgeEntity].m_Flags;
		// Fixed elements (fixed bridge spans) are not part of an imported network.
		CompositionFlags Flags2 = Flags | SubObjectFlags | ElevationFlags;
		Flags2.m_General |= CompositionFlags_General_Edge;
		Flags2 |= GetHandednessFlags(PrefabGeometryData);
		Flags2 |= GetEdgeStates(Prefab, Flags2);
		if (bInvert) Flags2 = NetCompositionHelpers::InvertCompositionFlags(Flags2);
		return Flags2;
	}

	void FindFriendEdges(int32 EdgeEntity, int32 NodeEntity, uint MergeLayers, const Curve& EdgeCurve, int32& LeftEdge, int32& RightEdge) const
	{
		float2 F = 0.0f;
		int32 Num = 0;
		LeftEdge = EdgeEntity;
		RightEdge = EdgeEntity;
		FEdgeIterator Iterator(G, NodeEntity);
		FEdgeIteratorValue Value;
		while (Iterator.GetNext(Value))
		{
			if (Value.m_Edge == EdgeEntity) continue;
			if ((PrefabGeometry(Value.m_Edge).m_MergeLayers & MergeLayers) != Layer_None)
			{
				const Curve& Curve2 = G.CurveData[Value.m_Edge];
				F += math::select(Curve2.m_Bezier.a.xz(), Curve2.m_Bezier.d.xz(), Value.m_End);
				Num++;
				LeftEdge = Value.m_Edge;
				RightEdge = Value.m_Edge;
			}
		}
		if (Num <= 1) return;
		F /= static_cast<float>(Num);
		const float2 F2 = math::normalizesafe(MathUtils::Position(EdgeCurve.m_Bezier, 0.5f).xz() - F);
		const float2 X = MathUtils::Right(F2);
		float Num2 = -2.0f;
		float Num3 = 2.0f;
		FEdgeIterator Iterator2(G, NodeEntity);
		FEdgeIteratorValue Value2;
		while (Iterator2.GetNext(Value2))
		{
			if (Value2.m_Edge == EdgeEntity) continue;
			if ((PrefabGeometry(Value2.m_Edge).m_MergeLayers & MergeLayers) != Layer_None)
			{
				Curve Curve3 = G.CurveData[Value2.m_Edge];
				if (Value2.m_End) Curve3.m_Bezier = MathUtils::Invert(Curve3.m_Bezier);
				const float2 Y = math::normalizesafe(MathUtils::Position(Curve3.m_Bezier, 0.5f).xz() - F);
				float Num4;
				if (math::dot(F2, Y) < 0.0f) Num4 = math::dot(X, Y) * 0.5f;
				else
				{
					const float Num5 = math::dot(X, Y);
					Num4 = math::select(-1.0f, 1.0f, Num5 >= 0.0f) - Num5 * 0.5f;
				}
				if (Num4 > Num2)
				{
					Num2 = Num4;
					LeftEdge = Value2.m_Edge;
				}
				if (Num4 < Num3)
				{
					Num3 = Num4;
					RightEdge = Value2.m_Edge;
				}
			}
		}
	}

	static bool LevelCrossingLayers(uint Layers)
	{
		return ((Layers & (Layer_Road | Layer_Pathway | Layer_MarkerPathway | Layer_PublicTransportRoad)) != Layer_None && (Layers & (Layer_TrainTrack | Layer_SubwayTrack)) != Layer_None)
			|| ((Layers & (Layer_Road | Layer_TrainTrack | Layer_Pathway | Layer_TramTrack | Layer_SubwayTrack | Layer_MarkerPathway | Layer_PublicTransportRoad)) != Layer_None && (Layers & Layer_Waterway) != Layer_None);
	}

	CompositionFlags GetNodeFlags(int32 EdgeEntity, int32 NodeEntity, int32 OwnerEntity, int32 Prefab, const NetData& PrefabNetData, const NetGeometryData& PrefabGeometryData,
		CompositionFlags EdgeFlags, Curve EdgeCurve, bool bIsStart, CompositionFlags& ObsoleteNodeFlags, CompositionFlags& ObsoleteEdgeFlags) const
	{
		const CompositionFlags OriginalEdgeFlags = EdgeFlags;
		EdgeFlags.m_General &= CompositionFlags_General_Lighting | CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel | CompositionFlags_General_MiddlePlatform | CompositionFlags_General_WideMedian | CompositionFlags_General_PrimaryMiddleBeautification | CompositionFlags_General_SecondaryMiddleBeautification;
		const uint SideKeep = CompositionFlags_Side_Raised | CompositionFlags_Side_Lowered | CompositionFlags_Side_PrimaryTrack | CompositionFlags_Side_SecondaryTrack | CompositionFlags_Side_TertiaryTrack | CompositionFlags_Side_QuaternaryTrack | CompositionFlags_Side_PrimaryStop | CompositionFlags_Side_SecondaryStop | CompositionFlags_Side_TertiaryStop | CompositionFlags_Side_PrimaryBeautification | CompositionFlags_Side_SecondaryBeautification | CompositionFlags_Side_AbruptEnd | CompositionFlags_Side_Gate | CompositionFlags_Side_WideSidewalk | CompositionFlags_Side_ParkingSpaces | CompositionFlags_Side_SoundBarrier | CompositionFlags_Side_SecondaryLane | CompositionFlags_Side_ForbidLeftTurn | CompositionFlags_Side_ForbidRightTurn | CompositionFlags_Side_ForbidStraight | CompositionFlags_Side_ForbidSecondary | CompositionFlags_Side_ZonesDisabled;
		EdgeFlags.m_Left &= SideKeep;
		EdgeFlags.m_Right &= SideKeep;
		CompositionFlags Handedness = GetHandednessFlags(PrefabGeometryData);
		ObsoleteNodeFlags = CompositionFlags{};
		Handedness.m_General |= CompositionFlags_General_Node;
		if (bIsStart)
		{
			Handedness.m_General ^= CompositionFlags_General_Invert;
			EdgeFlags = NetCompositionHelpers::InvertCompositionFlags(EdgeFlags);
		}
		Handedness |= GetSubObjectFlags(NodeEntity, PrefabNetData);
		Handedness |= EdgeFlags;
		if (G.Has(OwnerEntity, Component_Edge))
		{
			const Edge& OwnerEdge = G.EdgeData[OwnerEntity];
			const Curve& Curve2 = G.CurveData[OwnerEntity];
			const NetData& PrefabNetData2 = G.PrefabNet[G.PrefabRef[OwnerEntity]];
			const NetGeometryData& OwnerGeometry = PrefabGeometry(OwnerEntity);
			const float3 X = bIsStart ? EdgeCurve.m_Bezier.a : EdgeCurve.m_Bezier.d;
			const float Num = math::distancesq(X, Curve2.m_Bezier.a);
			const float Num2 = math::distancesq(X, Curve2.m_Bezier.d);
			const int32 OwnerNode = Num <= Num2 ? OwnerEdge.m_Start : OwnerEdge.m_End;
			Handedness |= GetSubObjectFlags(OwnerNode, PrefabNetData2) & MakeFlags(CompositionFlags_General_FixedNodeSize, 0, 0);
			FEdgeIterator Iterator(G, OwnerNode);
			FEdgeIteratorValue Value;
			while (Iterator.GetNext(Value))
			{
				const NetGeometryData& Geometry2 = PrefabGeometry(Value.m_Edge);
				if ((Geometry2.m_MergeLayers & OwnerGeometry.m_MergeLayers) == 0 && LevelCrossingLayers(Geometry2.m_MergeLayers | OwnerGeometry.m_MergeLayers)) Handedness.m_General |= CompositionFlags_General_LevelCrossing;
			}
		}
		if ((PrefabGeometryData.m_Flags & GeometryFlags_SupportRoundabout) == 0) Handedness.m_General &= ~CompositionFlags_General_Roundabout;
		int32 Num3 = 0;
		int3 Lanes = 0;
		bool bFlag = true;
		bool bFlag2 = false;
		bool bFlag3 = false;
		bool bFlag4 = false;
		bool bFlag5 = false;
		bool bFlag6 = false;
		bool bFlag7 = false;
		bool bFlag8 = false;
		const TArray<NetGeometryNodeState>& NodeStates = A.NodeStates(G.PrefabRoadIds[Prefab]);
		if (!bIsStart) EdgeCurve.m_Bezier = MathUtils::Invert(EdgeCurve.m_Bezier);
		int32 LeftEdge = 0;
		int32 RightEdge = 0;
		FindFriendEdges(EdgeEntity, NodeEntity, PrefabGeometryData.m_MergeLayers, EdgeCurve, LeftEdge, RightEdge);
		const bool bIncludeMiddleConnections = (PrefabGeometryData.m_MergeLayers & (Layer_Pathway | Layer_MarkerPathway)) != 0;
		FEdgeIterator Iterator2(G, NodeEntity, bIncludeMiddleConnections);
		FEdgeIteratorValue Value2;
		while (Iterator2.GetNext(Value2))
		{
			const int32 Prefab3 = G.PrefabRef[Value2.m_Edge];
			const NetGeometryData& Geometry2 = G.PrefabGeometry[Prefab3];
			const TArray<NetGeometryNodeState>& NodeStates2 = A.NodeStates(G.PrefabRoadIds[Prefab3]);
			// Every imported net is a road prefab, so RoadData is always present.
			{
				const RoadData& Road = G.PrefabRoad[Prefab3];
				bFlag3 |= (Road.m_Flags & RoadFlags_PreferTrafficLights) != 0;
				if ((Road.m_Flags & (RoadFlags_DefaultIsForward | RoadFlags_DefaultIsBackward)) != 0) Lanes += math::select(int3(0, 1, 1), int3(1, 0, 1), Value2.m_End == ((Road.m_Flags & RoadFlags_DefaultIsForward) != 0));
				else ++Lanes;
			}
			if ((Geometry2.m_MergeLayers & PrefabGeometryData.m_MergeLayers) == 0)
			{
				const uint Layers2 = Geometry2.m_MergeLayers | PrefabGeometryData.m_MergeLayers;
				if (LevelCrossingLayers(Layers2)) Handedness.m_General |= CompositionFlags_General_LevelCrossing;
				if ((Layers2 & Layer_Road) != Layer_None && (Layers2 & (Layer_Pathway | Layer_MarkerPathway)) != Layer_None) bFlag2 = true;
				continue;
			}
			if (Value2.m_Middle) continue;
			const bool bFlag9 = Value2.m_Edge == LeftEdge;
			const bool bFlag10 = Value2.m_Edge == RightEdge;
			const Edge& Edge2 = G.EdgeData[Value2.m_Edge];
			const CompositionFlags ElevationFlags = GetElevationFlags(Value2.m_Edge, Edge2.m_Start, Edge2.m_End, Geometry2);
			CompositionFlags Flags = ElevationFlags;
			if (Value2.m_End) Flags = NetCompositionHelpers::InvertCompositionFlags(Flags);
			const CompositionFlags EdgeFlags2 = GetEdgeFlags(Value2.m_Edge, Value2.m_End, Prefab3, Geometry2, ElevationFlags);
			if ((EdgeFlags.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) != 0
				|| (EdgeFlags.m_Left & (CompositionFlags_Side_Raised | CompositionFlags_Side_Lowered | CompositionFlags_Side_SoundBarrier)) != 0
				|| (EdgeFlags.m_Right & (CompositionFlags_Side_Raised | CompositionFlags_Side_Lowered | CompositionFlags_Side_SoundBarrier)) != 0)
			{
				if ((EdgeFlags.m_General & CompositionFlags_General_Elevated) != 0)
				{
					if ((Flags.m_General & CompositionFlags_General_Elevated) == 0)
					{
						if (bFlag9) Handedness.m_Left |= (Flags.m_Left & CompositionFlags_Side_Raised) != 0 ? CompositionFlags_Side_LowTransition : CompositionFlags_Side_HighTransition;
						if (bFlag10) Handedness.m_Right |= (Flags.m_Right & CompositionFlags_Side_Raised) != 0 ? CompositionFlags_Side_LowTransition : CompositionFlags_Side_HighTransition;
					}
				}
				else if ((EdgeFlags.m_General & CompositionFlags_General_Tunnel) != 0)
				{
					if ((Flags.m_General & CompositionFlags_General_Tunnel) == 0)
					{
						if (bFlag9) Handedness.m_Left |= (Flags.m_Left & CompositionFlags_Side_Lowered) != 0 ? CompositionFlags_Side_LowTransition : CompositionFlags_Side_HighTransition;
						if (bFlag10) Handedness.m_Right |= (Flags.m_Right & CompositionFlags_Side_Lowered) != 0 ? CompositionFlags_Side_LowTransition : CompositionFlags_Side_HighTransition;
					}
				}
				else
				{
					if (bFlag9)
					{
						if ((EdgeFlags.m_Left & CompositionFlags_Side_Raised) != 0)
						{
							if ((Flags.m_Left & CompositionFlags_Side_Raised) == 0 && (Flags.m_General & CompositionFlags_General_Elevated) == 0) Handedness.m_Left |= CompositionFlags_Side_LowTransition;
						}
						else if ((EdgeFlags.m_Left & CompositionFlags_Side_Lowered) != 0)
						{
							if ((Flags.m_Left & CompositionFlags_Side_Lowered) == 0 && (Flags.m_General & CompositionFlags_General_Tunnel) == 0) Handedness.m_Left |= CompositionFlags_Side_LowTransition;
						}
						else if ((EdgeFlags.m_Left & CompositionFlags_Side_SoundBarrier) != 0 && (EdgeFlags2.m_Left & CompositionFlags_Side_SoundBarrier) == 0) Handedness.m_Left |= CompositionFlags_Side_LowTransition;
					}
					if (bFlag10)
					{
						if ((EdgeFlags.m_Right & CompositionFlags_Side_Raised) != 0)
						{
							if ((Flags.m_Right & CompositionFlags_Side_Raised) == 0 && (Flags.m_General & CompositionFlags_General_Elevated) == 0) Handedness.m_Right |= CompositionFlags_Side_LowTransition;
						}
						else if ((EdgeFlags.m_Right & CompositionFlags_Side_Lowered) != 0)
						{
							if ((Flags.m_Right & CompositionFlags_Side_Lowered) == 0 && (Flags.m_General & CompositionFlags_General_Tunnel) == 0) Handedness.m_Right |= CompositionFlags_Side_LowTransition;
						}
						else if ((EdgeFlags.m_Right & CompositionFlags_Side_SoundBarrier) != 0 && (EdgeFlags2.m_Right & CompositionFlags_Side_SoundBarrier) == 0) Handedness.m_Right |= CompositionFlags_Side_LowTransition;
					}
				}
			}
			if ((EdgeFlags2.m_Left & CompositionFlags_Side_RemoveCrosswalk) != 0) bFlag7 = true;
			else
			{
				if (((EdgeFlags2.m_Left | EdgeFlags2.m_Right) & CompositionFlags_Side_Sidewalk) != 0 && (EdgeFlags2.m_General & CompositionFlags_General_MiddlePlatform) != 0) bFlag5 = true;
				if ((EdgeFlags2.m_Left & CompositionFlags_Side_AddCrosswalk) != 0)
				{
					bFlag5 = true;
					bFlag8 = true;
				}
				bFlag6 = true;
			}
			if (((EdgeFlags2.m_Left | EdgeFlags2.m_Right) & CompositionFlags_Side_ParkingSpaces) != 0) bFlag4 = true;
			if (PrefabGeometryData.m_StyleType != Geometry2.m_StyleType) Handedness.m_General |= CompositionFlags_General_StyleBreak;
			if (NodeStates.Num() != 0) Handedness |= GetNodeStates(NodeStates, OriginalEdgeFlags, EdgeFlags2, bFlag9, bFlag10);
			if (NodeStates2.Num() != 0) Handedness |= NetCompositionHelpers::InvertCompositionFlags(GetNodeStates(NodeStates2, EdgeFlags2, OriginalEdgeFlags, bFlag10, bFlag9));
			if (Value2.m_Edge != EdgeEntity)
			{
				if (Prefab3 != Prefab) bFlag = false;
				Num3++;
			}
		}
		if ((Handedness.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) != 0)
		{
			const bool bNum4 = (Handedness.m_Left & (CompositionFlags_Side_LowTransition | CompositionFlags_Side_HighTransition)) != 0;
			const bool bFlag11 = (Handedness.m_Right & (CompositionFlags_Side_LowTransition | CompositionFlags_Side_HighTransition)) != 0;
			if (bNum4 && !bFlag11) Handedness.m_Right |= CompositionFlags_Side_LowTransition;
			if (!bNum4 && bFlag11) Handedness.m_Left |= CompositionFlags_Side_LowTransition;
		}
		if (bFlag2 && (PrefabGeometryData.m_MergeLayers & (Layer_Pathway | Layer_MarkerPathway)) != Layer_None) return Handedness;
		bFlag = bFlag && Num3 == 1;
		bFlag2 = bFlag2 && Num3 >= 1;
		bFlag6 = bFlag6 && bFlag2;
		if (Num3 >= 2)
		{
			Handedness.m_General |= CompositionFlags_General_Intersection;
			if ((Handedness.m_Left & Handedness.m_Right & CompositionFlags_Side_Sidewalk) != 0 || bFlag2) Handedness.m_General |= CompositionFlags_General_Crosswalk;
		}
		else if (Num3 == 1)
		{
			if (bFlag)
			{
				if (bFlag4 && ((OriginalEdgeFlags.m_Left | OriginalEdgeFlags.m_Right) & CompositionFlags_Side_ParkingSpaces) != 0 && (Handedness.m_General & CompositionFlags_General_LevelCrossing) == 0) Handedness.m_General |= CompositionFlags_General_Crosswalk;
			}
			else Handedness.m_General |= CompositionFlags_General_Intersection;
			if (bFlag2) Handedness.m_General |= CompositionFlags_General_Crosswalk;
		}
		else if (Num3 == 0)
		{
			Handedness.m_General |= CompositionFlags_General_DeadEnd;
			bFlag5 = false;
			if ((Handedness.m_General & CompositionFlags_General_Invert) != 0) ObsoleteEdgeFlags.m_Left |= OriginalEdgeFlags.m_Left & (CompositionFlags_Side_AddCrosswalk | CompositionFlags_Side_RemoveCrosswalk);
			else ObsoleteEdgeFlags.m_Right |= OriginalEdgeFlags.m_Right & (CompositionFlags_Side_AddCrosswalk | CompositionFlags_Side_RemoveCrosswalk);
		}
		const uint CrosswalkFlags = CompositionFlags_Side_AddCrosswalk | CompositionFlags_Side_RemoveCrosswalk;
		if (Num3 != 0)
		{
			if (((OriginalEdgeFlags.m_Left | OriginalEdgeFlags.m_Right) & CompositionFlags_Side_Sidewalk) != 0 && (OriginalEdgeFlags.m_General & CompositionFlags_General_MiddlePlatform) != 0) Handedness.m_General |= CompositionFlags_General_Crosswalk;
			if ((Handedness.m_General & (CompositionFlags_General_Intersection | CompositionFlags_General_MedianBreak)) == 0)
			{
				if ((Handedness.m_General & CompositionFlags_General_Crosswalk) != 0)
				{
					if ((Handedness.m_General & CompositionFlags_General_Invert) != 0)
					{
						if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_AddCrosswalk) != 0 || bFlag8) ObsoleteEdgeFlags.m_Left |= OriginalEdgeFlags.m_Left & CrosswalkFlags;
						else if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_RemoveCrosswalk) != 0 || bFlag7)
						{
							Handedness.m_General &= ~CompositionFlags_General_Crosswalk;
							bFlag6 = false;
						}
					}
					else if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_AddCrosswalk) != 0 || bFlag8) ObsoleteEdgeFlags.m_Right |= OriginalEdgeFlags.m_Right & CrosswalkFlags;
					else if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_RemoveCrosswalk) != 0 || bFlag7)
					{
						Handedness.m_General &= ~CompositionFlags_General_Crosswalk;
						bFlag6 = false;
					}
				}
				else if ((Handedness.m_General & CompositionFlags_General_Invert) != 0)
				{
					if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_RemoveCrosswalk) != 0 || bFlag7) ObsoleteEdgeFlags.m_Left |= OriginalEdgeFlags.m_Left & CrosswalkFlags;
					else if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_AddCrosswalk) != 0 || bFlag8) Handedness.m_General |= CompositionFlags_General_Crosswalk;
				}
				else if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_RemoveCrosswalk) != 0 || bFlag7) ObsoleteEdgeFlags.m_Right |= OriginalEdgeFlags.m_Right & CrosswalkFlags;
				else if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_AddCrosswalk) != 0 || bFlag8) Handedness.m_General |= CompositionFlags_General_Crosswalk;
			}
			else if ((Handedness.m_General & CompositionFlags_General_Crosswalk) != 0)
			{
				if ((Handedness.m_General & CompositionFlags_General_Invert) != 0)
				{
					if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_AddCrosswalk) != 0) ObsoleteEdgeFlags.m_Left |= CompositionFlags_Side_AddCrosswalk;
					if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_RemoveCrosswalk) != 0) Handedness.m_General &= ~CompositionFlags_General_Crosswalk;
				}
				else
				{
					if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_AddCrosswalk) != 0) ObsoleteEdgeFlags.m_Right |= CompositionFlags_Side_AddCrosswalk;
					if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_RemoveCrosswalk) != 0) Handedness.m_General &= ~CompositionFlags_General_Crosswalk;
				}
			}
			else if ((Handedness.m_General & CompositionFlags_General_Invert) != 0)
			{
				if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_RemoveCrosswalk) != 0) ObsoleteEdgeFlags.m_Left |= CompositionFlags_Side_RemoveCrosswalk;
				if ((OriginalEdgeFlags.m_Left & CompositionFlags_Side_AddCrosswalk) != 0) Handedness.m_General |= CompositionFlags_General_Crosswalk;
			}
			else
			{
				if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_RemoveCrosswalk) != 0) ObsoleteEdgeFlags.m_Right |= CompositionFlags_Side_RemoveCrosswalk;
				if ((OriginalEdgeFlags.m_Right & CompositionFlags_Side_AddCrosswalk) != 0) Handedness.m_General |= CompositionFlags_General_Crosswalk;
			}
			if ((Handedness.m_General & CompositionFlags_General_MedianBreak) != 0 && ((Handedness.m_General & CompositionFlags_General_Crosswalk) != 0 || bFlag5)) Handedness.m_General |= CompositionFlags_General_Intersection;
		}
		CompositionFlags NodeUpgrade{};
		if (G.Has(NodeEntity, Component_Upgraded))
		{
			Upgraded Upgrade = G.UpgradedData[NodeEntity];
			NodeUpgrade = Upgrade.m_Flags;
			Upgrade.m_Flags.m_General &= ~CompositionFlags_General_RemoveTrafficLights;
			Handedness |= Upgrade.m_Flags;
		}
		if ((Handedness.m_General & (CompositionFlags_General_Roundabout | CompositionFlags_General_LevelCrossing)) == 0
			&& ((Handedness.m_General & (CompositionFlags_General_Intersection | CompositionFlags_General_Crosswalk)) != 0 || bFlag2 || bFlag5))
		{
			if (bFlag3 && math::all(Lanes >= int3(1, 1, 2)) && (math::all(Lanes >= int3(2, 1, 3)) || (Handedness.m_General & CompositionFlags_General_Crosswalk) != 0 || bFlag5 || bFlag6))
			{
				if ((NodeUpgrade.m_General & (CompositionFlags_General_RemoveTrafficLights | CompositionFlags_General_AllWayStop)) != 0) Handedness.m_General &= ~CompositionFlags_General_TrafficLights;
				else Handedness.m_General |= CompositionFlags_General_TrafficLights;
				ObsoleteNodeFlags.m_General |= NodeUpgrade.m_General & CompositionFlags_General_TrafficLights;
			}
			else ObsoleteNodeFlags.m_General |= NodeUpgrade.m_General & CompositionFlags_General_RemoveTrafficLights;
		}
		else
		{
			Handedness.m_General &= ~(CompositionFlags_General_TrafficLights | CompositionFlags_General_AllWayStop);
			ObsoleteNodeFlags.m_General |= NodeUpgrade.m_General & (CompositionFlags_General_TrafficLights | CompositionFlags_General_RemoveTrafficLights | CompositionFlags_General_AllWayStop);
		}
		if ((Handedness.m_General & (CompositionFlags_General_Intersection | CompositionFlags_General_Roundabout)) != CompositionFlags_General_Intersection)
		{
			const uint Forbid = CompositionFlags_Side_ForbidLeftTurn | CompositionFlags_Side_ForbidRightTurn | CompositionFlags_Side_ForbidStraight;
			Handedness.m_Left &= ~Forbid;
			Handedness.m_Right &= ~Forbid;
			if ((Handedness.m_General & CompositionFlags_General_Invert) != 0) ObsoleteEdgeFlags.m_Left |= OriginalEdgeFlags.m_Left & Forbid;
			else ObsoleteEdgeFlags.m_Right |= OriginalEdgeFlags.m_Right & Forbid;
		}
		return Handedness;
	}
};
}

int32 FGraph::NewEntity()
{
	const int32 Id = Mask.Num();
	Mask.Add(0);
	NodeData.AddDefaulted();
	EdgeData.AddDefaulted();
	CurveData.AddDefaulted();
	ElevationData.AddDefaulted();
	UpgradedData.AddDefaulted();
	OwnerData.AddDefaulted();
	CompositionData.AddDefaulted();
	PrefabRef.Add(0);
	InitialNodeGeometry.AddDefaulted();
	ConnectedEdges.AddDefaulted();
	return Id;
}

int32 FGraph::OwnerEntity(int32 Owner)
{
	if (const int32* Existing = OwnerEntities.Find(Owner)) return *Existing;
	const int32 Entity = NewEntity();
	OwnerEntities.Add(Owner, Entity);
	return Entity;
}

int32 FGraph::Prefab(FAssets& Assets, const FString& Name)
{
	if (const int32* Existing = PrefabIndices.Find(Name)) return *Existing;
	const int32 Index = PrefabNames.Add(Name);
	PrefabRoadIds.Add(Assets.RoadId(Name));
	PrefabGeometry.Add(Assets.Geometry(Name));
	PrefabNet.Add(Assets.Network(Name));
	PrefabRoad.Add(Assets.Road(Name));
	PrefabPlaceable.Add(Assets.Placeable(Name));
	PrefabIndices.Add(Name, Index);
	return Index;
}

int32 FGraph::PositionNode(float3 Position, int32 PrefabIndex)
{
	// (int)MathF.Round: half to even.
	const FIntVector Key(RoundToInt(Position.x * 100.0f), RoundToInt(Position.y * 100.0f), RoundToInt(Position.z * 100.0f));
	if (const int32* Existing = PositionNodes.Find(Key))
	{
		if (PrefabNet[PrefabIndex].m_NodePriority > PrefabNet[PrefabRef[*Existing]].m_NodePriority) PrefabRef[*Existing] = PrefabIndex;
		return *Existing;
	}
	const int32 Id = NewEntity();
	PositionNodes.Add(Key, Id);
	Nodes.Add(Id);
	Mask[Id] |= Component_Node | Component_Elevation;
	NodeData[Id].m_Position = Position;
	NodeData[Id].m_Rotation = float4(0.0f, 0.0f, 0.0f, 1.0f);
	InitialNodeGeometry[Id].m_Position = Position.y;
	ElevationData[Id].m_Elevation = float2(Position.y);
	PrefabRef[Id] = PrefabIndex;
	return Id;
}

int32 FGraph::SavedNode(FAssets& Assets, const TArray<FSavedNode>& SavedNodes, int32 Index)
{
	if (SavedNodeEntities[Index] != 0) return SavedNodeEntities[Index];
	const FSavedNode& Source = SavedNodes[Index];
	const int32 PrefabIndex = Prefab(Assets, Source.Prefab);
	const int32 Id = NewEntity();
	SavedNodeEntities[Index] = Id;
	Nodes.Add(Id);
	Mask[Id] |= Component_Node;
	NodeData[Id].m_Position = Source.Position;
	NodeData[Id].m_Rotation = Source.Rotation;
	InitialNodeGeometry[Id].m_Position = Source.Position.y;
	if (Source.bHasElevation)
	{
		Mask[Id] |= Component_Elevation;
		ElevationData[Id].m_Elevation = Source.Elevation;
	}
	if (!IsDefault(Source.Upgrade))
	{
		Mask[Id] |= Component_Upgraded;
		UpgradedData[Id].m_Flags = Source.Upgrade;
	}
	if (Source.bOutside) Mask[Id] |= Component_OutsideConnection;
	if (Source.Owner != 0)
	{
		const int32 Owner = OwnerEntity(Source.Owner);
		Mask[Id] |= Component_Owner;
		OwnerData[Id].m_Owner = Owner;
	}
	PrefabRef[Id] = PrefabIndex;
	return Id;
}

int32 FGraph::CompositionIndex(const FComposition& Composition)
{
	if (const int32* Existing = CompositionIndices.Find(&Composition)) return *Existing;
	const int32 Index = Compositions.Add(&Composition);
	CompositionIndices.Add(&Composition, Index);
	return Index;
}

bool FGraph::Build(FAssets& Assets, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& SavedNodes, FString& OutError)
{
	if (Roads.Num() != 0)
	{
		OutError = TEXT("Create a fresh graph for a rebuild.");
		return false;
	}
	// Index 0 of every table is the null entity / prefab / composition.
	NewEntity();
	PrefabNames.AddDefaulted();
	PrefabRoadIds.Add(0);
	PrefabGeometry.AddDefaulted();
	PrefabNet.AddDefaulted();
	PrefabRoad.AddDefaulted();
	PrefabPlaceable.AddDefaulted();
	Compositions.Add(nullptr);
	SavedNodeEntities.Init(0, SavedNodes.Num());

	struct FPending
	{
		int32 Id;
		int32 Spec;
		int32 Start;
		int32 End;
	};
	TArray<FPending> Pending;
	for (int32 SpecIndex = 0; SpecIndex < Specs.Num(); SpecIndex++)
	{
		const FRoadSpec& Spec = Specs[SpecIndex];
		if (!Assets.HasRoad(Spec.Prefab))
		{
			OutError = FString::Printf(TEXT("Unknown road prefab '%s'"), *Spec.Prefab);
			return false;
		}
		if (math::distance(Spec.Curve.a, Spec.Curve.d) < 0.1f)
		{
			OutError = TEXT("Road endpoints must differ.");
			return false;
		}
		if (Spec.bHasSource && (!SavedNodes.IsValidIndex(Spec.SourceStart) || !SavedNodes.IsValidIndex(Spec.SourceEnd)))
		{
			OutError = FString::Printf(TEXT("Road %d ends on a node that is not saved."), SpecIndex);
			return false;
		}
		const int32 PrefabIndex = Prefab(Assets, Spec.Prefab);
		const int32 Start = Spec.bHasSource ? SavedNode(Assets, SavedNodes, Spec.SourceStart) : PositionNode(Spec.Curve.a, PrefabIndex);
		const int32 End = Spec.bHasSource ? SavedNode(Assets, SavedNodes, Spec.SourceEnd) : PositionNode(Spec.Curve.d, PrefabIndex);
		const int32 Id = NewEntity();
		Mask[Id] |= Component_Edge | Component_Curve;
		EdgeData[Id].m_Start = Start;
		EdgeData[Id].m_End = End;
		PrefabRef[Id] = PrefabIndex;
		CurveData[Id].m_Bezier = Spec.Curve;
		CurveData[Id].m_Length = MathUtils::Length(Spec.Curve);
		if (!Spec.bHasSource)
		{
			Mask[Id] |= Component_Elevation;
			ElevationData[Id].m_Elevation = float2((Spec.Curve.a.y + Spec.Curve.d.y) * 0.5f);
		}
		else if (Spec.bHasElevation)
		{
			Mask[Id] |= Component_Elevation;
			ElevationData[Id].m_Elevation = Spec.Elevation;
		}
		if (!IsDefault(Spec.Upgrade))
		{
			Mask[Id] |= Component_Upgraded;
			UpgradedData[Id].m_Flags = Spec.Upgrade;
		}
		if (Spec.bHasSource && Spec.Owner != 0)
		{
			const int32 Owner = OwnerEntity(Spec.Owner);
			Mask[Id] |= Component_Owner;
			OwnerData[Id].m_Owner = Owner;
		}
		ConnectedEdges[Start].Add(Id);
		ConnectedEdges[End].Add(Id);
		Pending.Add({ Id, SpecIndex, Start, End });
	}

	// SelectCompositionJob.Execute for every edge, then the composition lookup/creation.
	FCompositionSelect Selector(*this, Assets);
	for (const FPending& Item : Pending)
	{
		const FRoadSpec& Spec = Specs[Item.Spec];
		const int32 PrefabIndex = PrefabRef[Item.Id];
		const NetGeometryData& Geometry = PrefabGeometry[PrefabIndex];
		const NetData& Net = PrefabNet[PrefabIndex];
		const Curve& EdgeCurve = CurveData[Item.Id];
		const int32 OwnerEntity = Has(Item.Id, Component_Owner) ? OwnerData[Item.Id].m_Owner : Entity_Null;
		const CompositionFlags SubObjectFlags = Selector.GetSubObjectFlags(Item.Id, EdgeCurve, Geometry);
		const CompositionFlags ElevationFlags = Selector.GetElevationFlags(Item.Id, Item.Start, Item.End, Geometry);
		const CompositionFlags UpgradeFlags = Has(Item.Id, Component_Upgraded) ? UpgradedData[Item.Id].m_Flags : CompositionFlags{};
		CompositionFlags ObsoleteEdge;
		CompositionFlags ObsoleteStart;
		CompositionFlags ObsoleteEnd;
		CompositionFlags EdgeFlags = Selector.GetEdgeFlags(Item.Id, PrefabIndex, Geometry, UpgradeFlags, SubObjectFlags, ElevationFlags, ObsoleteEdge);
		const CompositionFlags StartFlags = Selector.GetNodeFlags(Item.Id, Item.Start, OwnerEntity, PrefabIndex, Net, Geometry, EdgeFlags, EdgeCurve, true, ObsoleteStart, ObsoleteEdge);
		const CompositionFlags EndFlags = Selector.GetNodeFlags(Item.Id, Item.End, OwnerEntity, PrefabIndex, Net, Geometry, EdgeFlags, EdgeCurve, false, ObsoleteEnd, ObsoleteEdge);
		// Original Execute: FixedNodeSize from either node also applies to the edge composition.
		EdgeFlags.m_General |= (StartFlags.m_General | EndFlags.m_General) & CompositionFlags_General_FixedNodeSize;
		const FComposition& EdgeComposition = Assets.Compose(Spec.Prefab, EdgeFlags, false);
		const FComposition& StartComposition = Assets.Compose(Spec.Prefab, StartFlags, false);
		const FComposition& EndComposition = Assets.Compose(Spec.Prefab, EndFlags, false);
		Mask[Item.Id] |= Component_Composition;
		CompositionData[Item.Id].m_Edge = CompositionIndex(EdgeComposition);
		CompositionData[Item.Id].m_StartNode = CompositionIndex(StartComposition);
		CompositionData[Item.Id].m_EndNode = CompositionIndex(EndComposition);
		FRoadInstance Road;
		Road.Entity = Item.Id;
		Road.SpecIndex = Item.Spec;
		Road.Prefab = Spec.Prefab;
		Road.EdgeComposition = &EdgeComposition;
		Road.StartComposition = &StartComposition;
		Road.EndComposition = &EndComposition;
		Roads.Add(Road);
	}
	return true;
}
}

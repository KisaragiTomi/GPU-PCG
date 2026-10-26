#include "CSRoadLanes.h"

#include "Algo/StableSort.h"
#include "CSRoadCurves.h"

namespace CSRoad
{
namespace
{
constexpr int32 CSRoadLanes_Steps = 64;

// .NET Array.BinarySearch: an index holding Value, else the complement of the first larger element.
int32 CSRoadLanes_BinarySearch(const TArray<float>& Values, float Value)
{
	int32 Lo = 0;
	int32 Hi = Values.Num() - 1;
	while (Lo <= Hi)
	{
		const int32 I = Lo + ((Hi - Lo) >> 1);
		if (Values[I] == Value) return I;
		if (Values[I] < Value) Lo = I + 1;
		else Hi = I - 1;
	}
	return ~Lo;
}

// RoadLanes.TurnRestrictions: node orientation and lane direction decide which side's restrictions apply.
uint CSRoadLanes_TurnRestrictions(const NetCompositionData& Composition, const NetCompositionLane& Lane)
{
	return ((Composition.m_Flags.m_General & CompositionFlags_General_Invert) != 0) != ((Lane.m_Flags & LaneFlags_Invert) != 0)
		? Composition.m_Flags.m_Left : Composition.m_Flags.m_Right;
}

// Enumerable.GroupBy(lane => lane.Road): groups in order of first appearance, lanes in order.
TArray<TArray<int32>> CSRoadLanes_GroupByRoad(const TArray<FRoadLane>& Lanes, const TArray<int32>& Members)
{
	TArray<TArray<int32>> Groups;
	TMap<int32, int32> GroupOfRoad;
	for (int32 Lane : Members)
	{
		const int32* Group = GroupOfRoad.Find(Lanes[Lane].Road);
		if (!Group) Group = &GroupOfRoad.Add(Lanes[Lane].Road, Groups.AddDefaulted());
		Groups[*Group].Add(Lane);
	}
	return Groups;
}
}

Bezier4x3 LaneEdgeCurve(const Segment& InSegment, const NetCompositionLane& Lane, float Width)
{
	Bezier4x3 Curve = MathUtils::Lerp(InSegment.m_Left, InSegment.m_Right, Lane.m_Position.x / math::max(1.0f, Width) + 0.5f);
	Curve.a.y += Lane.m_Position.y;
	Curve.b.y += Lane.m_Position.y;
	Curve.c.y += Lane.m_Position.y;
	Curve.d.y += Lane.m_Position.y;
	return (Lane.m_Flags & LaneFlags_Invert) != 0 ? MathUtils::Invert(Curve) : Curve;
}

void FRoadLane::Measure()
{
	Distances.SetNumZeroed(Curves.Num() * CSRoadLanes_Steps + 1);
	float3 Previous = Curves[0].a;
	for (int32 I = 1; I < Distances.Num(); I++)
	{
		const int32 Part = (I - 1) / CSRoadLanes_Steps;
		const float T = static_cast<float>(I - Part * CSRoadLanes_Steps) / static_cast<float>(CSRoadLanes_Steps);
		const float3 P = MathUtils::Position(Curves[Part], T);
		Distances[I] = Distances[I - 1] + math::distance(Previous, P);
		Previous = P;
	}
	Length = Distances.Last();
}

void FRoadLane::Sample(float Distance, float3& OutPosition, float3& OutTangent) const
{
	Distance = math::clamp(Distance, 0.0f, Length);
	int32 I = CSRoadLanes_BinarySearch(Distances, Distance);
	if (I < 0) I = ~I;
	I = FMath::Clamp(I, 1, Distances.Num() - 1);
	const int32 Part = (I - 1) / CSRoadLanes_Steps;
	const float F = (Distance - Distances[I - 1]) / math::max(1e-6f, Distances[I] - Distances[I - 1]);
	const float T = (static_cast<float>(I - 1 - Part * CSRoadLanes_Steps) + F) / static_cast<float>(CSRoadLanes_Steps);
	OutPosition = MathUtils::Position(Curves[Part], T);
	OutTangent = math::normalizesafe(MathUtils::Tangent(Curves[Part], T), float3(0.0f, 0.0f, 1.0f));
}

void FRoadLanes::Build(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry)
{
	Lanes.Reset();
	UnsupportedRoundaboutNodes = 0;
	auto Add = [this](FRoadLane& Lane)
	{
		Lane.Measure();
		return Lanes.Add(Lane);
	};

	// Passenger-car lanes of every road, in road and definition order; a two-way lane twice.
	const uint Selected = LaneFlags_Road | LaneFlags_Master | LaneFlags_Parking | LaneFlags_Utility | LaneFlags_BicyclesOnly | LaneFlags_Virtual;
	for (int32 RoadIndex = 0; RoadIndex < Graph.Roads.Num(); RoadIndex++)
	{
		const FRoadInstance& Road = Graph.Roads[RoadIndex];
		const EdgeGeometry& RoadGeometry = Geometry.Edges[Road.Entity];
		const Edge& RoadEdge = Graph.EdgeData[Road.Entity];
		for (const NetCompositionLane& Definition : Road.EdgeComposition->Lanes)
		{
			if ((Definition.m_Flags & Selected) != LaneFlags_Road) continue;
			const bool bInverted = (Definition.m_Flags & LaneFlags_Invert) != 0;
			const Bezier4x3 Start = LaneEdgeCurve(RoadGeometry.m_Start, Definition, Road.EdgeComposition->Data.m_Width);
			const Bezier4x3 End = LaneEdgeCurve(RoadGeometry.m_End, Definition, Road.EdgeComposition->Data.m_Width);
			FRoadLane Lane;
			Lane.Road = RoadIndex;
			Lane.Definition = Definition;
			Lane.bReverse = bInverted;
			Lane.StartNode = bInverted ? RoadEdge.m_End : RoadEdge.m_Start;
			Lane.EndNode = bInverted ? RoadEdge.m_Start : RoadEdge.m_End;
			Lane.StartComposition = bInverted ? Road.EndComposition : Road.StartComposition;
			Lane.EndComposition = bInverted ? Road.StartComposition : Road.EndComposition;
			Lane.Curves = bInverted ? TArray<Bezier4x3, TInlineAllocator<2>>{ End, Start } : TArray<Bezier4x3, TInlineAllocator<2>>{ Start, End };
			Lane.Width = Assets.LaneData.FindChecked(Definition.m_Lane).m_Width;
			Add(Lane);
			if ((Definition.m_Flags & LaneFlags_Twoway) == 0) continue;
			FRoadLane Twin = Lane;
			Twin.bReverse = !bInverted;
			Twin.StartNode = Lane.EndNode;
			Twin.EndNode = Lane.StartNode;
			Twin.StartComposition = Lane.EndComposition;
			Twin.EndComposition = Lane.StartComposition;
			Twin.Curves = { MathUtils::Invert(Lane.Curves[1]), MathUtils::Invert(Lane.Curves[0]) };
			Add(Twin);
		}
	}

	// Connectors from every lane ending at a node to the lanes of the other roads starting there.
	const int32 NumEdgeLanes = Lanes.Num();
	TArray<int32> IncomingNodes;
	TMap<int32, TArray<int32>> Incoming;
	TMap<int32, TArray<int32>> Outgoing;
	for (int32 I = 0; I < NumEdgeLanes; I++)
	{
		TArray<int32>* Sources = Incoming.Find(Lanes[I].EndNode);
		if (!Sources)
		{
			IncomingNodes.Add(Lanes[I].EndNode);
			Sources = &Incoming.Add(Lanes[I].EndNode);
		}
		Sources->Add(I);
		Outgoing.FindOrAdd(Lanes[I].StartNode).Add(I);
	}
	auto LateralOrder = [this](int32 Lane) { return Lanes[Lane].Definition.m_Position.x * (Lanes[Lane].bReverse ? -1.0f : 1.0f); };
	for (int32 Node : IncomingNodes)
	{
		const TArray<int32> Sources = Incoming[Node];
		const TArray<int32>* Targets = Outgoing.Find(Node);
		if (!Targets) continue;
		// A roundabout needs the original circulation algorithm.
		if (Sources.ContainsByPredicate([this](int32 Lane) { return (Lanes[Lane].EndComposition->Data.m_Flags.m_General & CompositionFlags_General_Roundabout) != 0; }))
		{
			UnsupportedRoundaboutNodes++;
			continue;
		}
		const TArray<TArray<int32>> SourceGroups = CSRoadLanes_GroupByRoad(Lanes, Sources);
		const TArray<TArray<int32>> TargetGroups = CSRoadLanes_GroupByRoad(Lanes, *Targets);
		for (const TArray<int32>& SourceGroup : SourceGroups)
		{
			for (const TArray<int32>& TargetGroup : TargetGroups)
			{
				if (Lanes[SourceGroup[0]].Road == Lanes[TargetGroup[0]].Road) continue;
				TArray<int32> From = SourceGroup;
				TArray<int32> To = TargetGroup;
				Algo::StableSortBy(From, LateralOrder);
				Algo::StableSortBy(To, LateralOrder);
				float3 APosition, ATangent, BPosition, BTangent;
				Lanes[From[0]].Sample(Lanes[From[0]].Length, APosition, ATangent);
				Lanes[To[0]].Sample(0.0f, BPosition, BTangent);
				bool bRight = false;
				bool bGentle = false;
				bool bUTurn = false;
				const bool bTurn = NetUtils::IsTurn(APosition.xz(), math::normalizesafe(ATangent.xz()), BPosition.xz(), -math::normalizesafe(BTangent.xz()), bRight, bGentle, bUTurn);
				if (bUTurn) continue;
				const uint Side = CSRoadLanes_TurnRestrictions(Lanes[From[0]].EndComposition->Data, Lanes[From[0]].Definition);
				if ((!bTurn && (Side & CompositionFlags_Side_ForbidStraight) != 0) || (bTurn && bRight && (Side & CompositionFlags_Side_ForbidRightTurn) != 0)
					|| (bTurn && !bRight && (Side & CompositionFlags_Side_ForbidLeftTurn) != 0))
				{
					continue;
				}
				// A sharp turn joins the outermost lanes; otherwise a monotone lane-rank mapping.
				TArray<FIntPoint> Pairs;
				if (bTurn && !bGentle) Pairs.Add(bRight ? FIntPoint(From.Num() - 1, To.Num() - 1) : FIntPoint(0, 0));
				else
				{
					const int32 N = FMath::Max(From.Num(), To.Num());
					for (int32 I = 0; I < N; I++) Pairs.AddUnique(FIntPoint(FMath::Min(From.Num() - 1, I * From.Num() / N), FMath::Min(To.Num() - 1, I * To.Num() / N)));
				}
				for (const FIntPoint& Pair : Pairs)
				{
					const int32 Source = From[Pair.X];
					const int32 Target = To[Pair.Y];
					if ((Lanes[Source].Definition.m_Flags & LaneFlags_DisconnectedEnd) != 0 || (Lanes[Target].Definition.m_Flags & LaneFlags_DisconnectedStart) != 0) continue;
					float3 P, PTangent, Q, QTangent;
					Lanes[Source].Sample(Lanes[Source].Length, P, PTangent);
					Lanes[Target].Sample(0.0f, Q, QTangent);
					Bezier4x3 Curve = math::distance(P, Q) >= 0.1f ? NetUtils::FitCurve(P, PTangent, QTangent, Q) : NetUtils::StraightCurve(P, Q);
					LaneSystem::ModifyCurveHeight(Curve, P.y - Lanes[Source].Definition.m_Position.y, Q.y - Lanes[Target].Definition.m_Position.y,
						Lanes[Source].EndComposition->Data, Lanes[Target].StartComposition->Data);
					FRoadLane Connector;
					Connector.Road = Lanes[Source].Road;
					Connector.Junction = Node;
					Connector.StartNode = Node;
					Connector.EndNode = Node;
					Connector.Definition = Lanes[Source].Definition;
					Connector.From = Source;
					Connector.To = Target;
					Connector.bConnector = true;
					Connector.Curves = { Curve };
					Connector.Width = math::min(Lanes[Source].Width, Lanes[Target].Width);
					Add(Connector);
				}
			}
		}
	}

	TMap<int32, int32> Starts;
	TMap<int32, int32> Ends;
	for (const FRoadLane& Lane : Lanes)
	{
		if (!Lane.bConnector) continue;
		Starts.FindOrAdd(Lane.From)++;
		Ends.FindOrAdd(Lane.To)++;
	}
	for (int32 I = 0; I < Lanes.Num(); I++)
	{
		FRoadLane& Lane = Lanes[I];
		if (Lane.bConnector)
		{
			Lane.SharedStartCount = FMath::Min(254, Starts[Lane.From] - 1);
			Lane.SharedEndCount = FMath::Min(254, Ends[Lane.To] - 1);
		}
		else
		{
			Lane.ConnectedStartCount = FMath::Min(255, Ends.FindRef(I));
			Lane.ConnectedEndCount = FMath::Min(255, Starts.FindRef(I));
		}
	}
}
}

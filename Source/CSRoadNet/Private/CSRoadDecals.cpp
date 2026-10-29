#include "CSRoadDecals.h"

#include "Algo/StableSort.h"
#include "CSRoadCurves.h"
#include "Misc/Paths.h"

namespace CSRoad
{
namespace
{
// RoadCrosswalks.CutFractions: how much of each end a crossing loses to the angle it meets the kerb at.
float2 CSRoadDecals_CutFractions(const Bezier4x3& Curve, float3 StartTangent, float3 EndTangent, float2 Widths)
{
	const float Length = math::distance(Curve.a, Curve.d);
	if (Length < 0.1f) return float2(0.0f);
	const float2 Direction = math::normalizesafe(Curve.d.xz() - Curve.a.xz());
	const float2 Dots = math::saturate(math::abs(float2(math::dot(StartTangent.xz(), Direction), math::dot(EndTangent.xz(), Direction))));
	// System.MathF.PI * .5f.
	const float HalfPi = 3.14159265358979f * 0.5f;
	const float2 Distance = float2(math::tan_d(HalfPi - math::acos_d(Dots.x)), math::tan_d(HalfPi - math::acos_d(Dots.y))) * Widths * 0.5f;
	return math::select(math::saturate(Distance / Length), float2(0.0f), bool2(Distance.x < 0.01f, Distance.y < 0.01f));
}

// RoadCrosswalks.Create(composition, geometry): placement for crossings away from an ordinary
// intersection (mid-block, side paths) and on roundabouts.
void CSRoadDecals_LegacyCrosswalks(const FComposition& Composition, const EdgeNodeGeometry& Geometry, TArray<FCrossing>& Out)
{
	if ((Composition.Data.m_Flags.m_General & CompositionFlags_General_Crosswalk) == 0) return;
	for (const NetCompositionCrosswalk& C : Composition.Crosswalks)
	{
		const float X = C.m_Start.x / math::max(1.0f, Composition.Data.m_Width) + 0.5f;
		const float Y = C.m_End.x / math::max(1.0f, Composition.Data.m_Width) + 0.5f;
		float3 A = math::lerp(Geometry.m_Left.m_Left.a, Geometry.m_Right.m_Right.a, X);
		float3 B = math::lerp(Geometry.m_Left.m_Left.a, Geometry.m_Right.m_Right.a, Y);
		const float3 Forward = math::normalizesafe(MathUtils::StartTangent(Geometry.m_Left.m_Left));
		A += Forward * C.m_Start.z;
		B += Forward * C.m_End.z;
		A.y += C.m_Start.y;
		B.y += C.m_End.y;
		const Bezier4x3 Curve = NetUtils::StraightCurve(A, B);
		Out.Add({ C.m_Lane, Curve, Curve });
	}
}
}

void CreateCrosswalks(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry, int32 RoadIndex, bool bStart, TArray<FCrossing>& Out)
{
	const FRoadInstance& Road = Graph.Roads[RoadIndex];
	const FComposition& Composition = bStart ? *Road.StartComposition : *Road.EndComposition;
	if ((Composition.Data.m_Flags.m_General & CompositionFlags_General_Crosswalk) == 0 || Composition.Crosswalks.Num() == 0) return;
	const EdgeNodeGeometry& NodeGeometry = bStart ? Geometry.StartNodes[Road.Entity] : Geometry.EndNodes[Road.Entity];
	// Mid-block / side-path and roundabout placement need the node-wide pedestrian graph.
	if ((Composition.Data.m_Flags.m_General & CompositionFlags_General_Intersection) == 0 || NodeGeometry.m_MiddleRadius > 0.0f)
	{
		CSRoadDecals_LegacyCrosswalks(Composition, NodeGeometry, Out);
		return;
	}
	// LaneSystem.GetNodeConnectPositions: where the road's pedestrian lanes meet the node, ordered
	// across the road; they are matched to the crossing slots before the ends are offset.
	const Segment& RoadSegment = bStart ? Geometry.Edges[Road.Entity].m_Start : Geometry.Edges[Road.Entity].m_End;
	struct FConnection
	{
		float Order;
		float3 Position;
		float3 Tangent;
	};
	TArray<FConnection> Connections;
	for (const NetCompositionLane& Lane : Road.EdgeComposition->Lanes)
	{
		if ((Lane.m_Flags & (LaneFlags_Pedestrian | LaneFlags_OnWater)) != LaneFlags_Pedestrian) continue;
		const bool bExit = (!bStart) == ((Lane.m_Flags & LaneFlags_Invert) == 0);
		if ((Lane.m_Flags & (bExit ? LaneFlags_DisconnectedEnd : LaneFlags_DisconnectedStart)) != 0) continue;
		const float T = Lane.m_Position.x / math::max(1.0f, Road.EdgeComposition->Data.m_Width) + 0.5f;
		Bezier4x3 Curve = MathUtils::Lerp(RoadSegment.m_Left, RoadSegment.m_Right, T);
		if (!bStart) Curve = MathUtils::Invert(Curve);
		float3 Tangent = -MathUtils::StartTangent(Curve);
		const float PlanarLength = math::length(Tangent.xz());
		if (PlanarLength < 1e-6f) continue;
		Tangent /= PlanarLength;
		Tangent.y = math::clamp(Tangent.y, -1.0f, 1.0f);
		float3 Position = Curve.a;
		Position.y += Lane.m_Position.y;
		Connections.Add({ bStart ? -Lane.m_Position.x : Lane.m_Position.x, Position, Tangent });
	}
	if (Connections.Num() == 0) return;
	// List.Sort sorts this few elements by insertion, which keeps ties in order.
	Algo::StableSortBy(Connections, [](const FConnection& C) { return C.Order; });
	const TArray<NetCompositionCrosswalk>& Crosswalks = Composition.Crosswalks;
	const Bezier4x3& Left = NodeGeometry.m_Left.m_Left;
	const Bezier4x3& Right = NodeGeometry.m_Right.m_Right;
	auto Position = [&Left, &Right, &Composition](float3 Offset)
	{
		float3 P = math::lerp(Left.a, Right.a, Offset.x / math::max(1.0f, Composition.Data.m_Width) + 0.5f);
		P.y += Offset.y;
		return P;
	};
	const int32 NumSlots = Crosswalks.Num() + 1;
	TArray<float3> Slots;
	TArray<float3> Tangents;
	TArray<bool> Matched;
	Slots.SetNum(NumSlots);
	Tangents.Init(Connections[0].Tangent, NumSlots);
	Matched.Init(false, NumSlots);
	for (int32 I = 0; I < NumSlots; I++)
	{
		Slots[I] = Position(I == 0 ? Crosswalks[0].m_Start : I == NumSlots - 1 ? Crosswalks.Last().m_End : math::lerp(Crosswalks[I - 1].m_End, Crosswalks[I].m_Start, 0.5f));
	}
	for (const FConnection& Connection : Connections)
	{
		int32 Best = -1;
		float Distance = FLT_MAX;
		for (int32 I = 0; I < NumSlots; I++)
		{
			if (Matched[I] || !(math::distancesq(Connection.Position, Slots[I]) < Distance)) continue;
			Distance = math::distancesq(Connection.Position, Slots[I]);
			Best = I;
		}
		if (Best < 0) continue;
		Matched[Best] = true;
		Tangents[Best] = Connection.Tangent;
	}
	for (int32 I = 0; I < Crosswalks.Num(); I++)
	{
		const NetCompositionCrosswalk& C = Crosswalks[I];
		const float3 A = Position(C.m_Start) + Tangents[I] * C.m_Start.z;
		const float3 B = Position(C.m_End) + Tangents[I + 1] * C.m_End.z;
		const Bezier4x3 Curve = NetUtils::StraightCurve(A, B);
		const float2 Cut = CSRoadDecals_CutFractions(Curve, Tangents[I], Tangents[I + 1], float2(Assets.LaneData.FindChecked(C.m_Lane).m_Width));
		Out.Add({ C.m_Lane, Curve, MathUtils::Cut(Curve, Bounds1(Cut.x, math::max(Cut.x, 1.0f - Cut.y))) });
	}
}

bool FGpuDecalInput::Build(const FGraph& Graph, const FAssets& Assets, const FGpuGeometryResult& Geometry, const FGpuRoadMeshInput& Mesh, const FString& AssetDirectory, FString& OutError,
	const FDecalScope* Scope)
{
	if (!Scope || !Scope->Lanes) Lanes.Build(Graph, Assets, Geometry);
	const FRoadLanes& Graphed = Scope && Scope->Lanes ? *Scope->Lanes : Lanes;
	auto Kept = [Scope](int32 Road) { return !Scope || !Scope->Keep || (*Scope->Keep)[Road]; };
	Items.Reset();
	ItemBlocks.Reset();
	NumBlocks = 0;
	MaterialSlots.Reset();
	ReceiverNodes.Reset();
	ReceiverSegments.Reset();
	Segments.Reset();
	NumReceiverSlots = 0;

	// Roads touching each node, in road order: the order RoadRenderer fills the node's projection.
	TMap<int32, TArray<int32>> RoadsAtNode;
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++)
	{
		const Edge& RoadEdge = Graph.EdgeData[Graph.Roads[Road].Entity];
		RoadsAtNode.FindOrAdd(RoadEdge.m_Start).Add(Road);
		if (RoadEdge.m_End != RoadEdge.m_Start) RoadsAtNode.FindOrAdd(RoadEdge.m_End).Add(Road);
	}
	auto Triangles = [&Mesh](uint32 Instance)
	{
		const FGpuMeshInstance& Item = Mesh.Instances[Instance];
		return Item.RefinedTriangleCount > 0 ? Item.RefinedTriangleCount : Mesh.Meshes[Item.Mesh].IndexCount / 3u;
	};
	// Segments point at the mesh input's instances by parameter slot (road * 3 + kind); a scoped build
	// must have written every one its nodes need.
	int32 MissingSlot = INDEX_NONE;
	auto AddSegment = [this, &Triangles, &Mesh, &MissingSlot](int32 Slot) -> FGpuReceiverSegment&
	{
		const int32 Instance = Mesh.InstanceOfSlot.IsValidIndex(Slot) ? Mesh.InstanceOfSlot[Slot] : INDEX_NONE;
		if (Instance == INDEX_NONE && MissingSlot == INDEX_NONE) MissingSlot = Slot;
		FGpuReceiverSegment& Segment = Segments.AddDefaulted_GetRef();
		Segment.Instance = Instance == INDEX_NONE ? 0u : uint32(Instance);
		Segment.Offset = Align(NumReceiverSlots, ReceiverChunk);
		Segment.Capacity = Instance == INDEX_NONE ? 0u : Triangles(uint32(Instance));
		NumReceiverSlots = Segment.Offset + Segment.Capacity;
		return Segment;
	};
	// RoadRenderer.BuildSteps: per road the edge's triangles near the node, then the node instance's.
	TMap<int32, uint32> ReceiverOfNode;
	auto Receiver = [&](int32 Node) -> uint32
	{
		if (const uint32* Known = ReceiverOfNode.Find(Node)) return *Known;
		const uint32 Index = uint32(ReceiverNodes.Add(Node));
		ReceiverOfNode.Add(Node, Index);
		const uint32 First = uint32(Segments.Num());
		for (int32 Road : RoadsAtNode.FindRef(Node))
		{
			const int32 Entity = Graph.Roads[Road].Entity;
			const Edge& RoadEdge = Graph.EdgeData[Entity];
			const EdgeGeometry& RoadGeometry = Geometry.Edges[Entity];
			const float3 StartCentre = (RoadGeometry.m_Start.m_Left.a + RoadGeometry.m_Start.m_Right.a) * 0.5f;
			const float3 EndCentre = (RoadGeometry.m_End.m_Left.d + RoadGeometry.m_End.m_Right.d) * 0.5f;
			const float Radius = Graph.Roads[Road].EdgeComposition->Data.m_Width * 2.0f;
			const bool bStartHere = RoadEdge.m_Start == Node;
			const bool bEndHere = RoadEdge.m_End == Node;
			FGpuReceiverSegment& Near = AddSegment(Road * 3);
			Near.Filter = 1;
			Near.Centre = bStartHere ? StartCentre : EndCentre;
			Near.OtherCentre = EndCentre;
			Near.bTwoCentres = bStartHere && bEndHere ? 1u : 0u;
			Near.Radius2 = Radius * Radius;
			if (bStartHere) AddSegment(Road * 3 + 1);
			if (bEndHere) AddSegment(Road * 3 + 2);
		}
		ReceiverSegments.Add(FUintVector2(First, uint32(Segments.Num()) - First));
		return Index;
	};

	// RoadDecals.Material: a surface draws only if its base-colour texture exists.
	TMap<FString, int32> SlotOfSurface;
	auto MaterialSlot = [&](const FString& Surface)
	{
		if (const int32* Known = SlotOfSurface.Find(Surface)) return *Known;
		const FString BaseColor = Assets.SurfaceBaseColor(Surface);
		const bool bTexture = !BaseColor.IsEmpty() && FPaths::FileExists(FPaths::Combine(AssetDirectory, BaseColor + TEXT(".dds")));
		const int32 Slot = bTexture ? MaterialSlots.Add(Surface) : INDEX_NONE;
		SlotOfSurface.Add(Surface, Slot);
		return Slot;
	};

	// RoadDecals.AddLane: one item per decal mesh of the lane prefab. Node: the receiver for projected
	// and clipped decals, INDEX_NONE for a strip.
	int32 Block = 0;
	auto AddLane = [&](int32 LanePrefab, const Bezier4x3& Curve, int32 Node, float2 Weights, float2 Connected, bool bClip)
	{
		const TArray<FLaneDecalMesh>& DecalMeshes = Assets.LaneDecalMeshes(LanePrefab);
		const float Length = MathUtils::Length(Curve);
		if (DecalMeshes.Num() == 0 || Length < 0.05f) return;
		for (const FLaneDecalMesh& DecalMesh : DecalMeshes)
		{
			if (DecalMesh.bRequireEditor || (DecalMesh.bRequireLeftHandTraffic && !Graph.bLeftHandTraffic) || (DecalMesh.bRequireRightHandTraffic && Graph.bLeftHandTraffic)
				|| DecalMesh.bRequireLevelCrossing || DecalMesh.bRequireTrackCrossing)
			{
				continue;
			}
			const int32 Slot = MaterialSlot(DecalMesh.Surface);
			if (Slot == INDEX_NONE) continue;
			const Float4x4 Matrix = BatchDataHelpers::BuildCurveMatrix(Curve, Length, float4(DecalMesh.Size, 0.0f), DecalMesh.TilingCount);
			FGpuDecalItem& Item = Items.AddDefaulted_GetRef();
			Item.Curve = Curve;
			Item.Distance = float4(Matrix.c0.w, Matrix.c1.w, Matrix.c2.w, Matrix.c3.w);
			Item.UVArea = float4(DecalMesh.UVMin, DecalMesh.UVMax);
			Item.Weights = Weights;
			Item.Connected = Connected;
			Item.Length = Length;
			Item.Width = DecalMesh.Size.x;
			Item.Lift = 0.018f + static_cast<float>(DecalMesh.RendererPriority) * 0.0003f;
			Item.Smoothing = DecalMesh.SmoothingDistance;
			Item.Kind = uint32(bClip ? EDecalKind::Clipped : Node == INDEX_NONE ? EDecalKind::Strip : EDecalKind::Projected);
			Item.Rows = bClip ? 0u : uint32(FMath::Clamp(static_cast<int32>(std::ceil(static_cast<double>(Length / (Node == INDEX_NONE ? 1.5f : 0.5f)))), 2, 400));
			Item.Receiver = Node == INDEX_NONE ? 0u : Receiver(Node);
			Item.MaterialSlot = uint32(Slot);
			ItemBlocks.Add(Block);
		}
	};

	// Edge lanes of the graph by road, one per lane definition (a two-way lane's reverse is skipped).
	TArray<TArray<int32>> EdgeLanesOfRoad;
	EdgeLanesOfRoad.SetNum(Graph.Roads.Num());
	for (int32 I = 0; I < Graphed.Lanes.Num(); I++)
	{
		const FRoadLane& Lane = Graphed.Lanes[I];
		if (Lane.bConnector) continue;
		TArray<int32>& OfRoad = EdgeLanesOfRoad[Lane.Road];
		if (!OfRoad.ContainsByPredicate([&Graphed, &Lane](int32 Other) { return Graphed.Lanes[Other].Definition.m_Index == Lane.Definition.m_Index; })) OfRoad.Add(I);
	}
	TArray<FCrossing> Crossings;
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++)
	{
		Block = Road;
		if (!Kept(Road)) continue;
		const FRoadInstance& Instance = Graph.Roads[Road];
		const Edge& RoadEdge = Graph.EdgeData[Instance.Entity];
		for (int32 LaneIndex : EdgeLanesOfRoad[Road])
		{
			const FRoadLane& Lane = Graphed.Lanes[LaneIndex];
			const float2 Ends = BatchDataHelpers::ConnectedEnds(Lane.ConnectedStartCount, Lane.ConnectedEndCount);
			for (int32 I = 0; I < Lane.Curves.Num(); I++) AddLane(Lane.Definition.m_Lane, Lane.Curves[I], INDEX_NONE, float2(1.0f), float2(I == 0 ? Ends.x : 1.0f, I == Lane.Curves.Num() - 1 ? Ends.y : 1.0f), false);
		}
		// The composition's other lanes (pedestrian, bicycle, tram, ...); road lanes come from the graph.
		const EdgeGeometry& RoadGeometry = Geometry.Edges[Instance.Entity];
		for (const NetCompositionLane& Lane : Instance.EdgeComposition->Lanes)
		{
			if ((Lane.m_Flags & (LaneFlags_Master | LaneFlags_Utility | LaneFlags_Parking)) != 0) continue;
			if ((Lane.m_Flags & LaneFlags_Road) != 0 && (Lane.m_Flags & LaneFlags_BicyclesOnly) == 0) continue;
			for (const Segment* RoadSegment : { &RoadGeometry.m_Start, &RoadGeometry.m_End }) AddLane(Lane.m_Lane, LaneEdgeCurve(*RoadSegment, Lane, Instance.EdgeComposition->Data.m_Width), INDEX_NONE, float2(1.0f), float2(0.0f), false);
		}
		for (const bool bStart : { true, false })
		{
			Crossings.Reset();
			CreateCrosswalks(Graph, Assets, Geometry, Road, bStart, Crossings);
			// BatchDataSystem cuts the path before calculating the tile count.
			for (const FCrossing& Crossing : Crossings)
			{
				if (math::distance(Crossing.MarkingCurve.a, Crossing.MarkingCurve.d) > 0.1f) AddLane(Crossing.Lane, Crossing.MarkingCurve, bStart ? RoadEdge.m_Start : RoadEdge.m_End, float2(1.0f), float2(0.0f), true);
			}
		}
	}
	// RoadDecals.AddConnection: connectors fade by the number of lanes sharing each end.
	Block = Graph.Roads.Num();
	for (const FRoadLane& Lane : Graphed.Lanes)
	{
		if (!Lane.bConnector) continue;
		if (Kept(Lane.Road)) AddLane(Lane.Definition.m_Lane, Lane.Curves[0], Lane.Junction, BatchDataHelpers::SharedWeights(Lane.SharedStartCount, Lane.SharedEndCount), float2(1.0f), false);
		Block++;
	}
	NumBlocks = Block;
	if (MissingSlot != INDEX_NONE)
	{
		OutError = FString::Printf(TEXT("The road mesh input does not write mesh instance %d of road %d, which a decal receiver needs"), MissingSlot % 3, MissingSlot / 3);
		return false;
	}
	return FitLayout(OutError);
}

bool FGpuDecalInput::FitLayout(FString& OutError)
{
	uint64 Heights = 0;
	for (FGpuDecalItem& Item : Items)
	{
		Item.VertexOffset = 0;
		if (EDecalKind(Item.Kind) == EDecalKind::Strip) continue;
		if (EDecalKind(Item.Kind) == EDecalKind::Clipped)
		{
			const double Along = math::distance(Item.Curve.a, Item.Curve.d);
			Item.Rows = uint32(FMath::Clamp(static_cast<int32>(std::ceil(Along / double(FitRowLength))), 1, 400));
		}
		Item.VertexOffset = uint32(FMath::Min(Heights, uint64(MAX_uint32)));
		Heights += (uint64(Item.Rows) + 1) * (FitColumns + 1);
	}
	if (Heights > uint64(MAX_int32))
	{
		OutError = FString::Printf(TEXT("The decals' fitted grids need %llu heights"), Heights);
		return false;
	}
	NumFitHeights = uint32(Heights);
	return true;
}
}

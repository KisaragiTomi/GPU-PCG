#include "CSRoadNetworkActor.h"

#include "CSRoadBridgePlacement.h"
#include "CSRoadCurves.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadNetworkPreview.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/RotationMatrix.h"

namespace
{
FVector CSRoadPreview_Position(CSRoad::float3 P) { return FVector(P.x, -P.z, P.y) * 100.0; }
FVector CSRoadPreview_Position(CSRoadBridge::FPoint P) { return FVector(P.X, -P.Z, P.Y) * 100.0; }

void CSRoadPreview_Instances(UInstancedStaticMeshComponent* Component, const TArray<FTransform>& Transforms)
{
	if (!Component) return;
	if (Component->GetInstanceCount() != Transforms.Num())
	{
		Component->ClearInstances();
		if (!Transforms.IsEmpty()) Component->AddInstances(Transforms, false, false, false);
	}
	else if (!Transforms.IsEmpty()) Component->BatchUpdateInstancesTransforms(0, Transforms, false, true, true);
}
}

void ACSRoadNetworkActor::ClearPreview()
{
	Preview.Reset();
	NumTrafficVehicles = NumTrafficLanes = UnsupportedTrafficRoundabouts = 0;
	NumBridgePiers = SkippedBridgePiers = 0;
	for (UInstancedStaticMeshComponent* Component : {TrafficBodies.Get(), TrafficCabins.Get(), BridgePiers.Get()}) if (Component) Component->ClearInstances();
}

void ACSRoadNetworkActor::SetShowTraffic(bool bShow)
{
	bShowTraffic = bShow;
	if (TrafficBodies) TrafficBodies->SetVisibility(bShow);
	if (TrafficCabins) TrafficCabins->SetVisibility(bShow && !TrafficVehicleMesh);
	if (bShow && Preview && Preview->Count == INDEX_NONE) ResetTraffic();
}

void ACSRoadNetworkActor::ResetTraffic()
{
	if (!Preview) return;
	Preview->Count = TrafficVehicleCount;
	Preview->Seed = TrafficSeed;
	Preview->Speed = TrafficSpeed;
	Preview->Gap = TrafficGap;
	Preview->Simulation.MaxSpeed = TrafficSpeed / 3.6f;
	Preview->Simulation.Gap = TrafficGap;
	Preview->Simulation.bSignals = bTrafficSignals;
	Preview->Simulation.Reset(TrafficVehicleCount, static_cast<uint32>(TrafficSeed));
	if (TrafficBodies)
	{
		TrafficBodies->SetStaticMesh(TrafficVehicleMesh ? TrafficVehicleMesh.Get() : LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		TrafficBodies->SetMaterial(0, TrafficMaterial);
	}
	if (TrafficCabins) TrafficCabins->SetMaterial(0, TrafficMaterial);
	SetShowTraffic(bShowTraffic);
	RenderTraffic();
}

void ACSRoadNetworkActor::TickPreview(float DeltaSeconds)
{
	// Also honor direct Blueprint/C++ property writes, which do not send editor notifications.
	if (TrafficBodies && TrafficBodies->IsVisible() != bShowTraffic) SetShowTraffic(bShowTraffic);
	if (!Preview || !bShowTraffic) return;
	if (Preview->Count != TrafficVehicleCount || Preview->Seed != TrafficSeed || Preview->Speed != TrafficSpeed || Preview->Gap != TrafficGap) ResetTraffic();
	Preview->Simulation.bSignals = bTrafficSignals;
	if (!GetWorld() || (!GetWorld()->IsGameWorld() && !bTrafficInEditor)) return;
	Preview->Simulation.Advance(DeltaSeconds);
	RenderTraffic();
}

void ACSRoadNetworkActor::RenderTraffic()
{
	if (!Preview || !TrafficBodies || !TrafficCabins) return;
	TArray<FTransform> Bodies, Cabins;
	Bodies.Reserve(static_cast<int32>(Preview->Simulation.Cars.size()));
	Cabins.Reserve(Bodies.Max());
	const FBox Bounds = TrafficVehicleMesh ? TrafficVehicleMesh->GetBoundingBox() : FBox(FVector(-50), FVector(50));
	const FVector MeshSize = Bounds.GetSize().ComponentMax(FVector(1));
	const FVector Size = TrafficVehicleMesh ? FVector(420, 180, 150) : FVector(420, 180, 90);
	const FVector Scale = Size / MeshSize;
	for (const CSRoadTraffic::FCar& Car : Preview->Simulation.Cars)
	{
		if (!Preview->Lanes.Lanes.IsValidIndex(Car.Lane)) continue;
		CSRoad::float3 Position, Tangent;
		Preview->Lanes.Lanes[Car.Lane].Sample(Car.Distance, Position, Tangent);
		const FVector P = CSRoadPreview_Position(Position);
		const FVector Forward = FVector(Tangent.x, -Tangent.z, Tangent.y).GetSafeNormal();
		const FQuat Rotation = FRotationMatrix::MakeFromXZ(Forward, FVector::UpVector).ToQuat();
		const FVector Center = FVector(0, 0, Size.Z * 0.5 + 10) - Bounds.GetCenter() * Scale;
		Bodies.Emplace(Rotation, P + Rotation.RotateVector(Center), Scale);
		if (!TrafficVehicleMesh) Cabins.Emplace(Rotation, P + Rotation.RotateVector(FVector(-25, 0, 120)), FVector(2.1, 1.5, 0.6));
	}
	CSRoadPreview_Instances(TrafficBodies, Bodies);
	CSRoadPreview_Instances(TrafficCabins, Cabins);
	NumTrafficVehicles = Bodies.Num();
}

void ACSRoadNetworkActor::BuildPreview(const CSRoad::FGraph& Graph, const CSRoad::FRoadLanes& Lanes, const CSRoad::FGpuGeometryResult& Geometry, const CSRoad::FSavedTerrain* Terrain)
{
	using namespace CSRoad;
	Preview = MakeUnique<FCSRoadNetworkPreview>();
	Preview->Lanes = Lanes;
	UnsupportedTrafficRoundabouts = Lanes.UnsupportedRoundaboutNodes;
	Preview->Simulation.Lanes.resize(Lanes.Lanes.Num());
	TMap<int32, TArray<int32>> SignalApproaches;
	for (const FRoadLane& Lane : Lanes.Lanes)
	{
		if (!Lane.bConnector || !Lanes.Lanes.IsValidIndex(Lane.From)) continue;
		const FRoadLane& From = Lanes.Lanes[Lane.From];
		if (From.EndComposition && (From.EndComposition->Data.m_Flags.m_General & CompositionFlags_General_TrafficLights)) SignalApproaches.FindOrAdd(Lane.Junction).AddUnique(From.Road);
	}
	NumTrafficLanes = 0;
	for (int32 I = 0; I < Lanes.Lanes.Num(); ++I)
	{
		const FRoadLane& Lane = Lanes.Lanes[I];
		CSRoadTraffic::FLane& Out = Preview->Simulation.Lanes[I];
		Out.Length = Lane.Length;
		Out.bConnector = Lane.bConnector;
		Out.Junction = Lane.Junction;
		// Shared two-way/service lanes need a different reservation model; do not seed opposing cars.
		Out.bDriveable = Lane.Length > 0.01f && (Lane.Definition.m_Flags & (LaneFlags_PublicOnly | LaneFlags_Twoway)) == 0;
		if (Graph.Roads.IsValidIndex(Lane.Road)) Out.SpeedLimit = FMath::Max(1.0f, Graph.PrefabRoad[Graph.PrefabRef[Graph.Roads[Lane.Road].Entity]].m_SpeedLimit);
		if (Lane.bConnector)
		{
			Out.SpeedLimit = FMath::Min(Out.SpeedLimit, 8.0f);
			if (Lanes.Lanes.IsValidIndex(Lane.From) && Lanes.Lanes.IsValidIndex(Lane.To))
			{
				Preview->Simulation.Lanes[Lane.From].Next.push_back(I);
				Out.Next.push_back(Lane.To);
			}
			if (const TArray<int32>* Approaches = SignalApproaches.Find(Lane.Junction))
			{
				Out.SignalGroup = Approaches->Find(Lane.Road);
				Out.SignalGroups = Approaches->Num();
			}
		}
		if (Out.bDriveable) ++NumTrafficLanes;
		// Composition pointers belong to the build; sampling needs only curves and distances.
		Preview->Lanes.Lanes[I].StartComposition = nullptr;
		Preview->Lanes.Lanes[I].EndComposition = nullptr;
	}
	if (bShowTraffic) ResetTraffic();
	if (!bBuildBridgePiers || !BridgePiers) return;

	std::vector<CSRoadBridge::FDeck> Decks;
	Decks.reserve(Graph.Roads.Num());
	for (const FRoadInstance& Road : Graph.Roads)
	{
		CSRoadBridge::FDeck Deck;
		const NetCompositionData& Composition = Road.EdgeComposition->Data;
		Deck.Width = Composition.m_Width;
		Deck.Bottom = FMath::Min(-0.35f, Composition.m_HeightRange.min);
		Deck.bElevated = (Composition.m_Flags.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) == CompositionFlags_General_Elevated;
		const EdgeGeometry& Edge = Geometry.Edges[Road.Entity];
		for (const Segment* Part : {&Edge.m_Start, &Edge.m_End})
		{
			const Bezier4x3 Center = MathUtils::Lerp(Part->m_Left, Part->m_Right, 0.5f);
			for (int32 I = 0; I <= 64; ++I)
			{
				const float3 Point = MathUtils::Position(Center, I / 64.0f);
				Deck.Points.push_back({Point.x, Point.y, Point.z});
			}
		}
		Decks.push_back(MoveTemp(Deck));
	}
	const FTransform ActorTransform = GetActorTransform();
	auto GroundAt = [&](CSRoadBridge::FPoint Point)
	{
		float Ground = Terrain ? Terrain->Sample(float3(Point.X, Point.Y, Point.Z)) : 0;
		if (!bTraceBridgeGround || !GetWorld()) return Ground;
		const FVector Start = ActorTransform.TransformPosition(CSRoadPreview_Position(Point) - FVector(0, 0, 50));
		const FVector End = ActorTransform.TransformPosition(FVector(Point.X * 100, -Point.Z * 100, Ground * 100 - 10000));
		FCollisionQueryParams Query(SCENE_QUERY_STAT(CSRoadBridgeGround), true, this);
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Query)) Ground = float(ActorTransform.InverseTransformPosition(Hit.ImpactPoint).Z * 0.01);
		return Ground;
	};
	CSRoadBridge::FSettings Settings;
	Settings.Spacing = BridgePierSpacing;
	Settings.MinHeight = BridgePierMinHeight;
	const float ColumnWidth = FMath::Clamp(BridgePierWidth, 0.5f, 5.0f);
	Settings.Footprint = ColumnWidth * 2;
	int Skipped = 0;
	const std::vector<CSRoadBridge::FPier> Piers = CSRoadBridge::Build(Decks, Settings, GroundAt, Skipped);
	TArray<FTransform> Instances;
	for (const CSRoadBridge::FPier& Pier : Piers)
	{
		const FVector Top = CSRoadPreview_Position(Pier.Top);
		const float Height = Pier.Top.Y - Pier.Ground;
		const FQuat Rotation = FRotator(0, FMath::RadiansToDegrees(FMath::Atan2(-Pier.Direction.Z, Pier.Direction.X)), 0).Quaternion();
		const float CapHeight = 0.7f;
		const float ColumnHeight = Height - CapHeight;
		Instances.Emplace(Rotation, FVector(Top.X, Top.Y, (Pier.Ground + ColumnHeight * 0.5f) * 100), FVector(ColumnWidth, ColumnWidth, ColumnHeight));
		Instances.Emplace(Rotation, Top - FVector(0, 0, CapHeight * 50), FVector(ColumnWidth * 1.4f, FMath::Max(ColumnWidth, Pier.Width * 0.8f), CapHeight));
		// The footing is embedded 0.35 m, with 0.15 m visible above the sampled ground.
		Instances.Emplace(Rotation, FVector(Top.X, Top.Y, Pier.Ground * 100 - 10), FVector(Settings.Footprint, Settings.Footprint, 0.5));
	}
	BridgePiers->SetMaterial(0, BridgePierMaterial);
	CSRoadPreview_Instances(BridgePiers, Instances);
	NumBridgePiers = static_cast<int32>(Piers.size());
	SkippedBridgePiers = Skipped;
}

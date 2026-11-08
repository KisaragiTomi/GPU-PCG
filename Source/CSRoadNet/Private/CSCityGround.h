// The city's ground on the GPU, a tile at a time (Shaders/Private/CSRoad/CSCityGround.usf). The CPU gives a tile's road segments and
// plots; the pass works out the ground's height (the roads, the plots' levels and the field's heights), fills the tile with slabs, puts
// walls where the ground drops, finds the ground left free between the buildings and puts planters or cart shops on it as the field
// says. One pass counts, the counts come back without waiting, another writes: as the facades do (CSCityGpuWrite.h).
#pragma once

#include "CoreMinimal.h"
#include "CSCityFacade.h"
#include "CSCityGroundKit.h"

namespace CSCity
{
enum class EGroundSlot : uint8 { Slab, Wall, Planter, Planter2, CartShop, CartShop2 };
inline constexpr int32 GroundSlots = 6;

// What the pass needs of a kit (cm).
struct FGroundKitShape
{
	bool bHas[GroundSlots] = {};
	FBox Bounds[GroundSlots];
	int32 Turns[GroundSlots] = {};
	double Reach = 6000.0;
	double EdgeSink = 2.5;
	double RoadMargin = 100.0;
	double RoadShoulder = 800.0;
	double PlotBlend = 400.0;
	double MaxSlope = 0.25;
	double WallMinStep = 40.0;
	double BuildingClearance = 300.0;
	double RoadClearance = 200.0;
	double MaxPropSlope = 0.15;
	double AutoPlanterChance = 0.25;
	double PlanterChance = 0.6;
	double CartChance = 0.8;

	FGroundKitShape() { for (FBox& Box : Bounds) Box = FBox(ForceInit); }
};
FGroundKitShape GroundKitShape(const UCSCityGroundKit& Kit);
uint64 GroundKitHash(const UCSCityGroundKit& Kit);

// The ground's tiles are squares of the world a whole number of its 12 m cells wide, so that its slabs line up from one tile to the next however
// the network lies: the nearest such size to a network's tiles.
inline double GroundTileCm(double NetworkTileCm) { return FMath::Max(FMath::RoundToDouble(NetworkTileCm / 1200.0), 1.0) * 1200.0; }

// A road's curve as the network keeps it (its own space), and what tells it from any other: tiles it bears on are only written again if that changes.
struct FGroundCurve
{
	uint64 Key = 0;
	FVector3d Curve[4];
	double HalfWidth = 0.0;
};

// A straight piece of road: world cm, along its middle.
struct FGroundRoad
{
	FVector A = FVector::ZeroVector;
	FVector B = FVector::ZeroVector;
	double HalfWidth = 0.0;
};

// What stands on a rectangle of ground. With bLevel the ground under it follows it: Base - (Drop + DropX * x + DropY * y), x and y in its
// frame from its middle (FLot's); without, it only keeps planters and carts away.
struct FGroundLot
{
	FVector2D Centre = FVector2D::ZeroVector;
	FVector2D Front = FVector2D::UnitY();
	double HalfWidth = 0.0;
	double HalfDepth = 0.0;
	double Base = 0.0;
	double Drop = 0.0;
	double DropX = 0.0;
	double DropY = 0.0;
	bool bLevel = true;
};
FGroundLot GroundLot(const FLot& Lot, bool bLevel);

// A tile of ground: a square of the world (cm) and what bears on it.
struct FGroundTile
{
	FVector2D Min = FVector2D::ZeroVector;
	double Size = 0.0;
	TArray<FGroundRoad> Roads;
	TArray<FGroundLot> Lots;
};

// Writes a tile with Kit: when the counts are back and the instances written, a component per slot that has any, made in Owner and attached
// to Parent as Name. The field is the one in Owner's world. Until then what was last written stays drawn.
void WriteGround(AActor& Owner, USceneComponent* Parent, const FString& Name, const UCSCityGroundKit& Kit, const FGroundTile& Tile, FFacadeComponents Components, TSharedPtr<FFacadeGpu>& Gpu);

// What a tile comes to, the passes run and waited for: only for tests. Each instance's box in the world (cm).
struct FGroundLayout
{
	uint32 Counts[GroundSlots] = {};
	TArray<FBox> Boxes[GroundSlots];
	uint32 Slabs() const { return Counts[int32(EGroundSlot::Slab)]; }
	uint32 Walls() const { return Counts[int32(EGroundSlot::Wall)]; }
	uint32 Planters() const { return Counts[int32(EGroundSlot::Planter)] + Counts[int32(EGroundSlot::Planter2)]; }
	uint32 Carts() const { return Counts[int32(EGroundSlot::CartShop)] + Counts[int32(EGroundSlot::CartShop2)]; }
};
FGroundLayout LayoutGround(const FGroundKitShape& Kit, const FGroundTile& Tile, const TSharedPtr<FFacadeFieldGpu>& Field = nullptr);
}

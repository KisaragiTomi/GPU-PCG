// The city layout around a road network, in the ground plane (UE centimetres, x/y, z up): the alleys
// laid out beforehand (a region split into blocks, keeping out of reserved plots) and the plots of
// the batch buildings along the roads. Plain geometry with no engine state: ACSCityAlleyRegion,
// ACSCityBuildingActor and ACSRoadNetworkActor feed it.
#pragma once

#include "CoreMinimal.h"

struct FCSGpuMeshCPUData;

namespace CSCity
{
// Convex polygons run counter-clockwise seen from above.
using FPolygon = TArray<FVector2D>;

// Counter-clockwise; fewer than three distinct points give fewer.
FPolygon ConvexHull(TArray<FVector2D> Points);
// A convex polygon with its edges pushed out by Distance (mitred corners).
FPolygon Grow(const FPolygon& Polygon, double Distance);
bool Contains(const FPolygon& Polygon, const FVector2D& Point);
// Two convex polygons share area (separating axes).
bool Overlap(const FPolygon& A, const FPolygon& B);
// Segment AB enters the convex polygon.
bool SegmentHits(const FPolygon& Polygon, const FVector2D& A, const FVector2D& B);
// The parts of segment AB outside the convex polygon, in order from A.
TArray<TPair<FVector2D, FVector2D>> SegmentOutside(const FPolygon& Polygon, const FVector2D& A, const FVector2D& B);

struct FAlleyLayoutSettings
{
	// Blocks are split until no side is longer.
	double BlockSize = 9000.0;
	// No split leaves a block narrower than this.
	double MinBlockSize = 3500.0;
	// How far from the middle a split may fall (share of the block), and how far it may turn (degrees).
	double Jitter = 0.2;
	double TurnJitter = 8.0;
	// Alleys keep this far out of reserved plots: half their width and some.
	double Clearance = 600.0;
	// Alleys round the region's edge too (cut where they would cross a reserved plot).
	bool bBoundary = true;
	int32 Seed = 1;
};

struct FAlley
{
	FVector2D A;
	FVector2D B;
};

// The convex region cut into blocks by straight alleys: each block is split across its longer side
// until short enough; a split that would cross a reserved plot is tried at other places, and a block
// no split fits stays whole. The same region, plots and settings give the same alleys.
TArray<FAlley> LayoutAlleys(const FPolygon& Region, const TArray<FPolygon>& Reserved, const FAlleyLayoutSettings& Settings);

struct FLotSettings
{
	// Along the road per plot, of which Gap stays between neighbours.
	double Frontage = 1600.0;
	double Gap = 200.0;
	// From the road's edge to the plot; plots keep half of it from every other road.
	double Setback = 300.0;
	double MinDepth = 1000.0;
	double MaxDepth = 1800.0;
	double MinHeight = 600.0;
	double MaxHeight = 2400.0;
	int32 Seed = 1;
};

// A road's centre line, sampled, with its half width (world cm).
struct FRoadLine
{
	// Plot ids derive from it: stable for the line's source (a spline's segment).
	uint64 Id = 0;
	TArray<FVector> Points;
	double HalfWidth = 0.0;
};

struct FLot
{
	uint64 Id = 0;
	// Counter-clockwise, standing on Base.
	FPolygon Corners;
	double Base = 0.0;
	double Height = 0.0;
	FVector2D Centre = FVector2D::ZeroVector;
	FLinearColor Color = FLinearColor::White;
};

// Plots on both sides of each anchor line, one every Frontage along it. A plot is built when it is
// clear of every road (Roads: as built, alleys already cut back under main roads; by half the setback
// from its edge) and every reserved plot, and no clear plot it overlaps comes first (a priority drawn from its id). A plot's id, size
// and place depend only on its anchor line, side and slot, so an edit elsewhere leaves it as it was.
TArray<FLot> LayoutLots(const TArray<FRoadLine>& Anchors, const TArray<FRoadLine>& Roads, const TArray<FPolygon>& Reserved, const FLotSettings& Settings);

// How far past an input's bounds the centres of the plots it can change lie (cm). A plot depends on its
// anchor line, on the roads and reserved plots near it or near a plot it overlaps (a rival), and on
// those rivals' anchor lines; nothing farther. So LayoutLots run on the inputs whose bounds lie within
// reach of a window gives every plot centred in the window exactly as a run on all of them does.
// LotRadius: the plots' largest half diagonal.
double LotRadius(const FLotSettings& Settings);
double AnchorReach(const FLotSettings& Settings, double HalfWidth);
double RoadReach(const FLotSettings& Settings, double HalfWidth);
double ReservedReach(const FLotSettings& Settings);

// The batch buildings between two layouts, so that the second lays out only the tiles near what
// changed: what every input could change last time (tiles, inclusive, by the input's fingerprint),
// and the tiles laid out but not written yet.
struct FLotState
{
	uint64 Signature = 0;
	bool bValid = false;
	TMap<uint64, FIntRect> Reach;
	TSet<FIntPoint> Pending;

	void Reset()
	{
		Signature = 0;
		bValid = false;
		Reach.Reset();
		Pending.Reset();
	}
};

// A convex footprint pushed up from Base by Height into Out (world space, one material slot): the
// sides and the top, flat shaded, coloured Color; no floor.
void AppendPrism(FCSGpuMeshCPUData& Out, const FPolygon& Footprint, double Base, double Height, const FLinearColor& Color);
}

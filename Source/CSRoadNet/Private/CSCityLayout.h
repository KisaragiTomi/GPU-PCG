// The city layout around a road network, in the ground plane (UE centimetres, x/y, z up): the alleys
// laid out beforehand (a region split into blocks, keeping out of reserved plots) and the plots of
// the batch buildings along the roads. Plain geometry with no engine state: ACSCityAlleyRegion,
// ACSCityBuildingActor and ACSRoadNetworkActor feed it.
#pragma once

#include "CoreMinimal.h"

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
	// From the road's edge to the plot, for roads that set none (FRoadLine::Setback).
	double Setback = 300.0;
	double MinDepth = 1000.0;
	double MaxDepth = 1800.0;
	double MinHeight = 600.0;
	double MaxHeight = 2400.0;
	// Plots in a block the roads close in run back to whatever stops them within FillDepth of their front (halfway to
	// the plots of another road, a road's clearance, a reserved plot), a Gap short, their back slanted to fit; plots
	// with nothing that near take MinDepth..MaxDepth. Near a corner or a road joining, a plot gives up the side of its
	// front that is not its own. A filled plot shallower than half MinDepth on a side, or narrower than 40% of its
	// slot, is left out. Without it, plots take MinDepth..MaxDepth and give way whole.
	bool bFill = true;
	double FillDepth = 4000.0;
	int32 Seed = 1;
};

// A road's centre line, sampled, with its half width (world cm).
struct FRoadLine
{
	// Plot ids derive from it: stable for the line's source (a spline's segment).
	uint64 Id = 0;
	// The road it is part of (a spline): a plot never makes way for its own road's other parts. 0: Id.
	uint64 Group = 0;
	TArray<FVector> Points;
	double HalfWidth = 0.0;
	// From its edge to the plots along it; negative: FLotSettings::Setback. Every plot keeps half of it from the road.
	double Setback = -1.0;
	// Along it per plot, Gap included; negative: FLotSettings::Frontage, which it never exceeds.
	double Frontage = -1.0;
	// It has plots along it: with FLotSettings::bFill, the plots of other roads stop halfway to them.
	bool bZoned = true;
};

// The alleys with the stretches under the roads taken out: where an alley comes within a road's half
// width plus Reach of its centre line, the road at most HeightTolerance off Height there. A cut end goes
// onto the centre line of the road that cut it (both ends of a stretch crossing a road straight over at
// one point), unless that would meet the road flatter than 30 degrees: then it stops where the cover
// starts. Pieces shorter than MinLength are dropped.
TArray<FAlley> CutAlleys(const TArray<FAlley>& Alleys, const TArray<FRoadLine>& Roads, double Reach, double Height, double HeightTolerance, double MinLength);

struct FLot
{
	uint64 Id = 0;
	// Counter-clockwise, standing on Base: a rectangle, or with FLotSettings::bFill a trapezoid whose back is slanted.
	FPolygon Corners;
	double Base = 0.0;
	double Height = 0.0;
	// The middle of the Width by Depth rectangle on its front, which lies within Corners.
	FVector2D Centre = FVector2D::ZeroVector;
	// Towards the road it faces; its size along that road (the stretch of its slot it takes) and away from it (the
	// shallower side of a slanted plot).
	FVector2D Front = FVector2D::UnitX();
	double Width = 0.0;
	double Depth = 0.0;
};

// Plots on both sides of each anchor line, one every Frontage along it, standing the anchor's setback from its edge.
// A plot stays clear of every road (Roads: as built, alleys already cut back under main roads; by half the road's
// setback from its edge; one whose front is not is left out) and of every reserved plot; with bFill it runs back to
// whatever stops it (FLotSettings::bFill), and gives way to a reserved plot or to a plot that comes first (a priority
// drawn from its id) by coming forward a Gap short of it; without, it gives way by being left out. A plot's id and
// place depend only on its anchor line, side and slot, so an edit elsewhere leaves it as it was.
// Place decides whether a plot can stand where it lies (and may set its base): one that cannot is no
// candidate at all.
TArray<FLot> LayoutLots(const TArray<FRoadLine>& Anchors, const TArray<FRoadLine>& Roads, const TArray<FPolygon>& Reserved, const FLotSettings& Settings,
	TFunctionRef<bool(FLot&)> Place = [](FLot&) { return true; });

// How far past an input's bounds the centres of the plots it can change lie (cm). Every plot lies in its strip,
// Frontage - Gap wide, from its front back by the deepest a plot can be; LotStrip is the strip's diagonal. A plot
// depends on its anchor line, on the roads whose half width, setback and that depth reach its strip, on the reserved
// plots near it, and on the plots it gives way to (their strips within a few Gaps of its own) with what those depend
// on; nothing farther. So LayoutLots run on the inputs whose bounds lie within reach of a window gives every plot
// centred in the window exactly as a run on all of them does. Setback: the line's own, negative for the settings'.
double LotStrip(const FLotSettings& Settings);
double AnchorReach(const FLotSettings& Settings, double HalfWidth, double Setback = -1.0);
double RoadReach(const FLotSettings& Settings, double HalfWidth, double Setback = -1.0);
double ReservedReach(const FLotSettings& Settings);

// The batch buildings between two layouts, so that the second lays out only the tiles near what
// changed: what every input could change last time (tiles, inclusive, by the input's fingerprint),
// and the tiles laid out but not written yet, of them those an edit dirtied (they go first).
struct FLotState
{
	uint64 Signature = 0;
	bool bValid = false;
	TMap<uint64, FIntRect> Reach;
	TSet<FIntPoint> Pending;
	TSet<FIntPoint> Edited;

	void Reset()
	{
		Signature = 0;
		bValid = false;
		Reach.Reset();
		Pending.Reset();
		Edited.Reset();
	}
};

// A box standing on the ground (world cm): the middle of its bottom, which way its front faces, and its size along the
// front (Width), back from it (Depth) and up (Height).
struct FBlock
{
	FVector Base = FVector::ZeroVector;
	FVector2D Front = FVector2D::UnitX();
	double Width = 0.0;
	double Depth = 0.0;
	double Height = 0.0;
};

// A plot's building as a box: the Width by Depth rectangle on its front, from its base up by its height.
FBlock LotBlock(const FLot& Lot);
// The smallest box around a convex footprint with a side along one of the footprint's, from Base up by Height; its front
// faces out of that side. Fewer than three corners: an empty box.
FBlock HullBlock(const FPolygon& Footprint, double Base, double Height);

// A module stretched to fill Block, as an instance's transform. Bounds is the module's local box; its +Y side is its
// front and +X runs along it (as BuildingMeshFrontYaw's default).
FTransform BlockPiece(const FBlock& Block, const FBox& Bounds);

// A building put together from copies of a module (Bounds as BlockPiece's), appended to Out: the rule for which copies
// make a building and where. For now one copy per storey, stretched to the block's footprint: as many storeys as come
// nearest StoreyHeight (one at least), sharing Height evenly. An empty block: none.
void AssembleBlock(const FBlock& Block, const FBox& Bounds, double StoreyHeight, TArray<FTransform>& Out);

// Which of Count meshes stands on a plot: drawn from its id, so an edit elsewhere leaves it as it was.
int32 LotMesh(const FLot& Lot, int32 Seed, int32 Count);

// A mesh on a plot as an instance's transform, the plot's Height aside: turned so that its front (Bounds is its
// local box; it faces FrontYaw degrees from +X towards +Y) faces the plot's road, shrunk as a whole until it
// fits the plot (never grown), its front on the plot's near edge, its bottom on Base. It stays within the plot.
FTransform LotInstance(const FLot& Lot, const FBox& Bounds, double FrontYaw);
}

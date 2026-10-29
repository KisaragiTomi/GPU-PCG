// RoadLab's decal items drawn as DBuffer mesh decals, each one or more instances of a unit mesh that a
// material lays along the item's curve from the instance's custom data. A strip (a lane along an
// edge) is a unit strip (DecalStripRows rows, two columns) that M_RoadDecal lays out as
// CSRoadDecal.usf lays a strip's vertices. A junction connector and a crosswalk, which RoadLab
// projects onto the surface, are a unit grid (DecalStripRows rows) that M_RoadDecalFitted lays on the
// surface: its points are among those of RoadLab's projected grid (a crosswalk's span its clipped
// footprint), as few columns as stay on the surface between them, and DecalFitCS's heights, carried
// in the instance, put each on the receiver RoadLab's TryProject picks; a triangle with a corner off
// the surface is not drawn, as RoadLab drops it. The DBuffer pass puts both on whatever receives
// decals there. Scripts/CS2Road/MakeRoadDecalMaterial.py makes the materials; their shader code reads
// the data below by index.
#pragma once

#include "CSRoadDecals.h"

class UObject;
class UStaticMesh;

namespace CSRoad
{
// Rows of the unit meshes: an item of N rows takes ceil(N / DecalStripRows) instances.
constexpr int32 DecalStripRows = 8;
constexpr int32 DecalFitColumns = int32(FGpuDecalInput::FitColumns);
// The columns a fitted grid draws: the fewest of these whose grid stays within DecalFitTolerance (cm)
// of the surface at RoadLab's points between its own (divisors of DecalFitColumns, so its points are
// among RoadLab's); all of them where a point is off the surface, as RoadLab drops cells by them.
constexpr int32 DecalFitColumnChoices[] = { 1, 2, 3, DecalFitColumns };
constexpr double DecalFitTolerance = 0.5;

// The per-instance floats, world space and centimetres from the instance's origin (its box's
// minimum corner) unless noted.
namespace DecalData
{
constexpr int32 A = 0;           // the curve's control points, 3 floats each
constexpr int32 B = 3;
constexpr int32 C = 6;
constexpr int32 D = 9;
constexpr int32 Scale = 12;      // the instance's scale: its box's size (the unit mesh's local space is that box)
constexpr int32 T0 = 15;         // the stretch of the curve this instance draws (a fitted grid's T1 may pass 1)
constexpr int32 T1 = 16;
constexpr int32 Width = 17;
constexpr int32 Lift = 18;       // a strip: along its normal; fitted: straight up off the surface
constexpr int32 Straight = 19;   // 1: a crosswalk's footprint
constexpr int32 UMin = 20;       // UV0 across: DecalProperties.m_TextureArea x
constexpr int32 UMax = 21;
constexpr int32 VMin = 22;       // UV0 along at t = 0
constexpr int32 VCurve = 23;     // 4: the tiled texture distance's cubic, times the area's height
constexpr int32 Length = 27;     // metres, as the fades' distances to the ends
constexpr int32 Weights = 28;    // 2: vertex colour r at t = 0 / 1
constexpr int32 Connected = 30;  // 2: g and b, ends that do not fade
constexpr int32 Smoothing = 32;  // metres
constexpr int32 Count = 33;
// A fitted grid's columns (DecalFitColumnChoices), then per point (row-major, columns + 1 a row) its
// height above the curve point it lies across from, in millimetres plus FitHeightZero (FitHeightOff:
// off the surface), 12 bits; two a float as the integer first + second * 4096.
constexpr int32 Columns = 33;
constexpr int32 Heights = 34;
constexpr int32 FittedCount(int32 GridColumns) { return Heights + ((DecalStripRows + 1) * (GridColumns + 1) + 1) / 2; }
constexpr int32 FitHeightZero = 2048;
constexpr int32 FitHeightOff = 4095;
}

// The instances of one material slot and renderer priority (the decals of a higher one draw over),
// strips or fitted grids of some columns.
struct FDecalInstances
{
	int32 MaterialSlot = 0;
	int32 Priority = 0;
	bool bFitted = false;
	int32 Columns = 1;
	// Per instance: DecalData::Count floats, a fitted one DecalData::FittedCount(Columns).
	int32 Floats = DecalData::Count;
	TArray<FTransform> Transforms;
	TArray<float> Data;
};

// The items (the network's space, Unity metres) as instances in world space; batches in order of
// first appearance, instances in item order. The projected and clipped items take their heights from
// FitHeights (a fitted input's, AddDecalFitPasses); without, they are left out.
void BuildDecalInstances(const FGpuDecalInput& Decals, const FTransform& NetworkToWorld, TArray<FDecalInstances>& Out, const TArray<float>* FitHeights = nullptr);

// The unit meshes, transient in Outer: DecalStripRows rows of Columns + 1 points (a strip's two).
// Local x along (0..1), y across (0..1); UV0 (across, along); z is y, so their bounds are the unit box.
UStaticMesh* MakeDecalStripMesh(UObject* Outer);
UStaticMesh* MakeDecalGridMesh(UObject* Outer, int32 Columns);
}

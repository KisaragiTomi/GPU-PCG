// Buildings put together from a facade kit (UCSCityFacadeKit) on the GPU. The CPU cuts each building's box into a grid
// of bays, draws its massing (which bays each storey keeps, MassFacade) and walks the outlines into runs: a row of
// facade cells on one side over a range of storeys, a corner column, a row of parapet, a rectangle of roof tiles
// (LayoutFacade). A compute pass (Shaders/Private/CSRoad/CSCityFacade.usf) runs a thread per module, finds its run,
// picks the module's slot and writes its instance into that slot's GPU instance source, which a
// UCSGpuInstancedMeshComponent per slot draws. Nothing waits for the render thread: the buffers are kept and grown on
// the render thread, and the components take the new ones once the write has landed (CSRoadMeshWrite), the last ones
// drawn until then.
#pragma once

#include "CoreMinimal.h"
#include "CSCityFacadeKit.h"
#include "CSCityLayout.h"

class AActor;
class UCSGpuInstancedMeshComponent;
class USceneComponent;

namespace CSCity
{
enum class EFacadeSlot : uint8 { Wall, Window, Door, Corner, Parapet, Roof, CornerInner, GroundWall, GroundWindow };
inline constexpr int32 FacadeSlots = 9;

// What the layout needs of a kit (cm): each slot's module box once turned, and the sizes and odds the cells take.
struct FFacadeKitShape
{
	bool bHas[FacadeSlots] = {};
	FBox Bounds[FacadeSlots];
	int32 Turns[FacadeSlots] = {};
	double Bay = 300.0;
	double Storey = 400.0;
	// The ground storey's height (0: Storey).
	double Ground = 0.0;
	double WindowChance = 0.6;
	double GroundWindowChance = 0.7;
	int32 ColumnGroup = 1;
	// The corner module's width along the sides (0: no corners), and the parapet's height (0: no parapet).
	double Corner = 0.0;
	double Parapet = 0.0;
	// Footprint weights in ECSCityMassShape's order from Box; stacking odds. A shape put together by hand is a box.
	float ShapeWeights[5] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float SetbackChance = 0.0f;
	float PodiumChance = 0.0f;
	int32 MinSetbackStoreys = 4;

	double GroundHeight() const { return Ground > 0.0 ? Ground : Storey; }
	// The bottom of storey S above the building's base.
	double StoreyBase(int32 S) const { return S <= 0 ? 0.0 : GroundHeight() + double(S - 1) * Storey; }
};
FFacadeKitShape KitShape(const UCSCityFacadeKit& Kit);
// Equal hashes, the same modules in the same places.
uint64 KitHash(const UCSCityFacadeKit& Kit);

// A building to put together: its box (Height rounded to whole storeys), its seed (what its massing, windows and walls
// are drawn from), where along its front the door goes (0..1 along the front's +X, from its -X corner; negative:
// none), and its massing (Auto: drawn from the kit).
struct FFacadeBuilding
{
	FBlock Block;
	uint32 Seed = 0;
	double DoorAt = 0.5;
	ECSCityMassShape Shape = ECSCityMassShape::Auto;
	ECSCityMassStack Stack = ECSCityMassStack::Auto;
};
// A plot's building: its box and its seed (from its id).
FFacadeBuilding LotBuilding(const FLot& Lot);

// The building's frame: X along its front (FBlock::Front turned clockwise), Y out of its front, from the box's middle.
// The bay grid is GridX by GridY cells, cell (I, J) at X from -Width / 2 + I * CellX, Y from -Depth / 2 + J * CellY;
// row GridY - 1 is the front. A tier is a run of storeys standing on the same cells; each tier keeps a subset of the
// one below.
struct FFacadeTier
{
	int32 First = 0;
	int32 End = 0;
	TArray<uint8> Cells;   // GridX * GridY, row by row from the back
};

struct FFacadeMassing
{
	int32 GridX = 0;
	int32 GridY = 0;
	int32 Storeys = 0;
	ECSCityMassShape Shape = ECSCityMassShape::Box;
	ECSCityMassStack Stack = ECSCityMassStack::None;
	TArray<FFacadeTier> Tiers;

	bool At(int32 Tier, int32 I, int32 J) const { return Tiers.IsValidIndex(Tier) && I >= 0 && J >= 0 && I < GridX && J < GridY && Tiers[Tier].Cells[J * GridX + I] != 0; }
};
// The building's grid and the cells each tier keeps (an empty box: no tiers).
FFacadeMassing MassFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building);

// A run the compute pass expands, a thread per module. Kinds: facade cells along a side over storeys [First, End); a
// corner column at a run's start vertex (outer or inner) over the same storeys, with one more piece at the parapet's
// height when the corner stops there; parapet cells along a side at storey End's base; a rectangle of roof tiles there.
// Sides as in the shader: 0 the front (out +Y, along +X), 1 the right (out +X, along -Y), 2 the back, 3 the left. Along
// and Out are the side frame's (Along runs with the side, Out is the outline's distance out from the middle).
enum class EFacadeRun : uint8 { Facade, Corner, CornerInner, Parapet, Roof };
struct FFacadeRun
{
	EFacadeRun Kind = EFacadeRun::Facade;
	uint8 Side = 0;
	bool bTrimStart = false;     // the first cell gives the corner piece its width
	bool bTrimEnd = false;       // the last cell does (the next side's corner)
	bool bParapetPiece = false;  // corners: one more piece at the parapet's height
	int32 Count = 0;             // cells along (roof: tiles along X)
	int32 Rows = 1;              // roof: tiles along Y
	int32 First = 0;             // storeys [First, End); parapets and roofs stand at End's base
	int32 End = 0;
	int32 Column = 0;            // the first cell's column on the grid (I on sides 0 and 2, J on 1 and 3)
	int32 ColumnStep = 1;
	int32 DoorCell = INDEX_NONE; // facade runs: the cell with the door, on the ground storey
	double Along = 0.0;          // roof: the rectangle's min X
	double Out = 0.0;            // roof: its min Y
	double Cell = 0.0;           // cell length along (roof: CellX)
	double CellY = 0.0;          // roof: CellY
	uint32 Threads() const;
};

// One building's runs and how many modules each slot gets at most (walls and windows share the facade's cells; the
// ground storey's may go to either pair).
struct FFacadeLayout
{
	FFacadeMassing Massing;
	double CellX = 0.0;
	double CellY = 0.0;
	TArray<FFacadeRun> Runs;
	uint32 FacadeCells = 0;
	uint32 GroundCells = 0;
	uint32 Doors = 0;
	uint32 Corners = 0;
	uint32 InnerCorners = 0;
	uint32 Parapets = 0;
	uint32 Roofs = 0;
	// The front cell with the door; INDEX_NONE: none.
	int32 DoorCell = INDEX_NONE;
	uint32 Threads() const;
};
FFacadeLayout LayoutFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building);

// The GPU side of one owner's facade (a building tile, a city building): each slot's instance buffers, which only the
// render thread touches.
class FFacadeGpu;

// Writes Buildings with Kit into Components: one per slot with a mesh, made in Owner and attached to Parent as Name,
// placed at Origin unturned (world; the instances are written relative to it), the others destroyed. The modules land a
// frame or two later, and those of the last write stay drawn until then. bFollowParent: the components move with Parent
// until the next write (a building being dragged); else they stay where they are put.
void WriteFacade(AActor& Owner, USceneComponent* Parent, const FString& Name, const FVector& Origin, const UCSCityFacadeKit& Kit, TConstArrayView<FFacadeBuilding> Buildings,
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu, bool bFollowParent = false);
// Components destroyed and the buffers handed back.
void ReleaseFacade(TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu);
}

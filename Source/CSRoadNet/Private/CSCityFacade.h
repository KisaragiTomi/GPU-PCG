// Buildings put together from a facade kit (UCSCityFacadeKit) on the GPU, as the roads are. The CPU only says what stands where:
// each building's box, seed and whatever is asked of it by hand. Compute passes (Shaders/Private/CSRoad/CSCityFacade.usf) do the
// rest: draw its massing (which bays of its grid each storey keeps), read the city's attribute field (ACSCityFieldActor: which
// faces stand out, which corner style) and walk the outlines, a thread per bay, into modules. As with the roads' refinement, one
// pass counts what each slot gets, the counts come back without waiting, and another writes that many into each slot's GPU
// instance source, which a UCSGpuInstancedMeshComponent per slot draws. A slot with nothing in it has no buffers and no component.
// Nothing waits for the render thread or the GPU: what was drawn stays drawn until the new write has landed (CSRoadMeshWrite).
#pragma once

#include "CoreMinimal.h"
#include "CSCityFacadeKit.h"
#include "CSCityLayout.h"

class AActor;
class UCSGpuInstancedMeshComponent;
class USceneComponent;

namespace CSCity
{
enum class EFacadeSlot : uint8 { Wall, Window, Door, Corner, Parapet, Roof, CornerInner, GroundWall, GroundWindow, RoofCorner, ParapetCorner, Soffit, Column, Plinth, PlinthCorner, Platform, Stair };
// The slots above, then four for each corner style after the kit's own: its corner, roof, parapet and plinth pieces.
inline constexpr int32 FacadeBaseSlots = 17;
inline constexpr int32 FacadeCornerStyles = 4;
inline constexpr int32 FacadeSlots = FacadeBaseSlots + FacadeCornerStyles * 4;
enum class EFacadeCornerPiece : uint8 { Corner, Roof, Parapet, Plinth };
// The slot of a corner style's piece (ECSCityCornerStyle; Auto is the kit's own).
int32 FacadeCornerSlot(ECSCityCornerStyle Style, EFacadeCornerPiece Piece);

// What the passes need of a kit (cm): each slot's module box once turned, and the sizes and odds the cells take.
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
	// The corner module's width along the sides (0: no corners) and depth into the building, and the parapet's height (0:
	// no parapet).
	double Corner = 0.0;
	double CornerDepth = 0.0;
	double Parapet = 0.0;
	// The same for each corner style after the kit's own.
	double StyleCorner[FacadeCornerStyles] = {};
	double StyleDepth[FacadeCornerStyles] = {};
	// Footprint weights in ECSCityMassShape's order from Box; stacking odds. A shape put together by hand is a box.
	float ShapeWeights[5] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	float SetbackChance = 0.0f;
	float PodiumChance = 0.0f;
	int32 MinSetbackStoreys = 4;
	// Overhangs: their odds and depth in bays, the columns' spacing in bays and inset.
	float OverhangChance = 0.0f;
	int32 OverhangBays = 1;
	int32 ColumnEvery = 2;
	double ColumnInset = 0.0;
	// Under the base: how far the plinth and columns go on under the ground (0 without a plinth), how far a platform
	// reaches, the drop under which there is none, the bays between its flights of steps.
	double PlinthSink = 0.0;
	double PlatformDepth = 150.0;
	double PlatformMinDrop = 15.0;
	int32 StairEvery = 4;

	FFacadeKitShape() { for (FBox& Box : Bounds) Box = FBox(ForceInit); }
	double GroundHeight() const { return Ground > 0.0 ? Ground : Storey; }
	// The bottom of storey S above the building's base.
	double StoreyBase(int32 S) const { return S <= 0 ? 0.0 : GroundHeight() + double(S - 1) * Storey; }
};
FFacadeKitShape KitShape(const UCSCityFacadeKit& Kit);
// Equal hashes, the same modules in the same places.
uint64 KitHash(const UCSCityFacadeKit& Kit);

// A building to put together: its box (Height rounded to whole storeys), its seed (what its massing, windows and walls
// are drawn from), where along its front the door goes (0..1 along the front's +X, from its -X corner; negative:
// none), and what is asked of its massing (Auto: drawn on the GPU from the kit, an overhang's sides from the seed).
struct FFacadeBuilding
{
	FBlock Block;
	uint32 Seed = 0;
	double DoorAt = 0.5;
	ECSCityMassShape Shape = ECSCityMassShape::Auto;
	ECSCityMassStack Stack = ECSCityMassStack::Auto;
	ECSCityMassOverhang Overhang = ECSCityMassOverhang::Auto;
	// Its stacking was set by hand: the field does not change it. Otherwise the field's wanted faces stand out whatever was
	// drawn or set here, and those it will not have do not.
	bool bLockMassing = false;
	// Auto: as the field has it, else the kit's own.
	ECSCityCornerStyle CornerStyle = ECSCityCornerStyle::Auto;
	// The ground under its base, as FLot's Drop, and the room in front of it to the road (0: no platform).
	double Drop = 0.0;
	double DropX = 0.0;
	double DropY = 0.0;
	double FrontRoom = 0.0;
};
// A plot's building: its box, its seed (from its id) and the ground under it.
FFacadeBuilding LotBuilding(const FLot& Lot);

// A building's massing as the GPU drew it, read back. The building's frame: X along its front (FBlock::Front turned clockwise), Y
// out of its front, from the box's middle. The bay grid is GridX by GridY cells, cell (I, J) at X from -Width / 2 + I * CellX, Y
// from -Depth / 2 + J * CellY; row GridY - 1 is the front. A tier is a run of storeys standing on the same cells; each tier keeps a
// subset of the one below, except that an overhang's upper tier stands on more cells than its base.
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
	// Which faces stand out past the base: 1 front, 2 right (+X), 4 back, 8 left. How many bays by (0: none).
	uint32 OverhangFaces = 0;
	int32 OverhangDepth = 0;
	TArray<FFacadeTier> Tiers;

	bool At(int32 Tier, int32 I, int32 J) const { return Tiers.IsValidIndex(Tier) && I >= 0 && J >= 0 && I < GridX && J < GridY && Tiers[Tier].Cells[J * GridX + I] != 0; }
};

// The GPU copy of a field (ACSCityFieldActor makes it): only the render thread looks inside.
class FFacadeFieldGpu;
// A field's page table and pages (FCSCityFieldPage's cells, 4 to the uint; heights the same, empty with none), the table's first
// page and size, uploaded.
TSharedPtr<FFacadeFieldGpu> MakeFacadeField(TArray<uint32> Table, TArray<uint32> Pages, TArray<uint32> Heights, const FIntPoint& FirstPage, const FIntPoint& TableSize);

// What one building comes to, the passes run and waited for: only for tests (and whatever else may wait).
struct FFacadeLayout
{
	FFacadeMassing Massing;
	uint32 Counts[FacadeSlots] = {};
	// Each module's box in the building's frame (from the middle of its box, up from its base), slot by slot.
	TArray<FBox> Boxes[FacadeSlots];
	// The same counts by what they are for.
	uint32 FacadeCells = 0;
	uint32 GroundCells = 0;
	uint32 Doors = 0;
	uint32 Corners = 0;
	uint32 InnerCorners = 0;
	uint32 Parapets = 0;
	uint32 Roofs = 0;
	uint32 RoofCorners = 0;
	uint32 ParapetCorners = 0;
	uint32 Soffits = 0;
	uint32 Columns = 0;
	uint32 Plinths = 0;
	uint32 PlinthCorners = 0;
	uint32 Platforms = 0;
	uint32 Stairs = 0;
	// How far the platform reaches out from the outline (0: none), and the front cell with the door (INDEX_NONE: none).
	double PlatformDepth = 0.0;
	int32 DoorCell = INDEX_NONE;
	uint32 Modules() const;
};
FFacadeLayout LayoutFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building, const TSharedPtr<FFacadeFieldGpu>& Field = nullptr);
FFacadeMassing MassFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building, const TSharedPtr<FFacadeFieldGpu>& Field = nullptr);

// The GPU side of one owner's facade (a building tile, a city building): each slot's instance buffers, which only the
// render thread touches.
class FFacadeGpu;

// Where a write's components live: looked up when it lands, which may be never (null: the owner is gone).
using FFacadeComponents = TFunction<TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>*()>;

// Writes Buildings with Kit: when the counts are back and the modules written, a component per slot that has any, made in Owner and
// attached to Parent as Name, placed at Origin unturned (world; the instances are written relative to it), the others destroyed.
// Until then those of the last write stay drawn; a later write supersedes one still on its way. The field is the one in Owner's
// world. bFollowParent: the components move with Parent until the next write (a building being dragged); else they stay where they
// are put. OnMassing: each building's massing once landed, in Buildings' order.
void WriteFacade(AActor& Owner, USceneComponent* Parent, const FString& Name, const FVector& Origin, const UCSCityFacadeKit& Kit, TConstArrayView<FFacadeBuilding> Buildings,
	FFacadeComponents Components, TSharedPtr<FFacadeGpu>& Gpu, bool bFollowParent = false, TFunction<void(TConstArrayView<FFacadeMassing>)> OnMassing = nullptr);
// Whether a write to Gpu is still on its way.
bool FacadeWritePending(const TSharedPtr<FFacadeGpu>& Gpu);
// Components destroyed and the buffers handed back; a write on its way comes to nothing.
void ReleaseFacade(TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu);
}

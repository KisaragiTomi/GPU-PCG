// What the city's buildings are put together from: a facade kit after Night City's modular buildings
// (NightCity_BuildingKit.md). A building's box is cut into a grid of bays about BayWidth square; its massing (a
// rectangle, an L, a U, a T or a ring, with setbacks or a tower on a podium) keeps some of the bays on each storey.
// Walls and windows fill the bays round each storey StoreyHeight high (the ground storey may take its own modules and
// height), a door goes in the front, corner pieces run up the outer corners and inner pieces up the inner ones, a
// parapet goes round every roof and roof tiles cover them. Every module is fitted into its cell by its bounds box:
// stretched along the facade and up, its own thickness kept, its +Y side out (towards the street) and +X along the
// facade once its QuarterTurns are applied; its pivot can be anywhere. The modules are drawn as GPU instances that a
// compute pass writes (CSCityFacade.h), so a building makes no mesh of its own; give the meshes Nanite, or each
// component uploads a copy of its mesh and waits for the render thread to.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "CSCityFacadeKit.generated.h"

class UStaticMesh;

/** One kind of module. */
USTRUCT(BlueprintType)
struct CSROADNET_API FCSCityFacadeModule
{
	GENERATED_BODY()

	/** Empty: the slot is left out (a window slot without a mesh takes walls, and the other way round). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	TObjectPtr<UStaticMesh> Mesh;

	/** Quarter turns about Z, counter-clockwise from above, that bring the mesh's outside to +Y. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade", meta = (ClampMin = "0", ClampMax = "3"))
	int32 QuarterTurns = 0;
};

/** A building's footprint on its bay grid. The front row of bays (the street side) is always kept. */
UENUM(BlueprintType)
enum class ECSCityMassShape : uint8
{
	/** Drawn from the kit's shape weights and the building's seed. */
	Auto,
	/** Every bay. */
	Box,
	/** A back corner cut away. */
	L,
	/** A notch cut into the back between two wings. */
	U,
	/** Both back corners cut away. */
	T,
	/** A courtyard in the middle, open to the sky. */
	Courtyard,
};

/** How a building's upper storeys sit on its lower ones. */
UENUM(BlueprintType)
enum class ECSCityMassStack : uint8
{
	/** Drawn from the kit's chances and the building's seed. */
	Auto,
	/** Every storey on the whole footprint. */
	None,
	/** The upper storeys step back from the street (and on tall buildings once more), leaving roof terraces. */
	Setback,
	/** A low podium on the whole footprint with a narrower tower on it. */
	PodiumTower,
};

UCLASS(BlueprintType)
class CSROADNET_API UCSCityFacadeKit : public UDataAsset
{
	GENERATED_BODY()

public:
	/** A bay of plain wall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Wall;

	/** A bay with a window: whole columns of bays are windows, WindowChance of them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Window;

	/** The ground storey's bay on the front where the door is. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Door;

	/** Up each outer corner, a storey at a time (and the parapet's height on top); as wide as the bays start in from the corner. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Corner;

	/** Round every roof, a bay at a time, kept at its own height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Parapet;

	/** Tiles over every roof and terrace, a bay square, their tops at the storey's top. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule Roof;

	/** Up each inner corner (an L's, a U's), in the open square outside both walls, its box's corner on the corner;
	 *  kept at its own width and depth. Empty: the walls just meet. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade")
	FCSCityFacadeModule CornerInner;

	/** The ground storey's plain bays (Night City's gf walls). Empty: the ground storey takes walls and windows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade|Ground")
	FCSCityFacadeModule GroundWall;

	/** The ground storey's glazed bays, shop fronts, GroundWindowChance of them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade|Ground")
	FCSCityFacadeModule GroundWindow;

	/** The bays of each side come nearest this width (the modules stretch to fit); a building's footprint is a grid of them. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade", meta = (ClampMin = "50", Units = "cm"))
	float BayWidth = 300.0f;

	/** A storey's height: a building's height is rounded to whole storeys. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade", meta = (ClampMin = "100", Units = "cm"))
	float StoreyHeight = 400.0f;

	/** The ground storey's height (Night City's gf modules are 450 cm). 0: StoreyHeight. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade|Ground", meta = (ClampMin = "0", Units = "cm"))
	float GroundStoreyHeight = 0.0f;

	/** How likely a column of bays is windows rather than walls, drawn from the building's seed, the side and the
	 *  column; a column is the same all the way up (as 84% of Night City's are) and lines up across setbacks. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade", meta = (ClampMin = "0", ClampMax = "1"))
	float WindowChance = 0.6f;

	/** Columns drawn together: 2 makes pairs of window columns and pairs of wall columns. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade", meta = (ClampMin = "1", ClampMax = "8"))
	int32 ColumnGroup = 1;

	/** How likely a ground-storey bay is a shop front (GroundWindow) rather than GroundWall. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Facade|Ground", meta = (ClampMin = "0", ClampMax = "1"))
	float GroundWindowChance = 0.7f;

	/** How often each footprint is drawn (Night City's masses: 58% rectangles, 15% L, 10% U/T/Z, 1.5% courtyards). A
	 *  shape the grid is too small for falls back to a box. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0"))
	float BoxWeight = 0.58f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0"))
	float LWeight = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0"))
	float UWeight = 0.06f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0"))
	float TWeight = 0.04f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0"))
	float CourtyardWeight = 0.02f;

	/** How likely a building of MinSetbackStoreys or more steps back (59% of Night City's masses have a taller one
	 *  rising at their edge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0", ClampMax = "1"))
	float SetbackChance = 0.35f;

	/** How likely a building of six storeys or more is a tower on a podium (drawn before the setback). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "0", ClampMax = "1"))
	float PodiumChance = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Massing", meta = (ClampMin = "2"))
	int32 MinSetbackStoreys = 4;

	// The slots in CSCity::EFacadeSlot's order.
	const FCSCityFacadeModule& Module(int32 Slot) const;

	// /PCGPlugins/CSRoadNet/Houses/Facade/DA_FacadeKit_Simple: boxes put together (Scripts/CS2Road/MakeFacadeKit.py).
	static UCSCityFacadeKit* Default();
};

// A street front: buildings along a spline, put together from a facade kit on the GPU as the batch buildings are
// (CSCityFacade.h). Its plots are laid out as the road network's batch plots are (CSCity::LayoutLots): on one side of
// the spline or both, one every Frontage, Setback from the street's edge; their heights are drawn from a storey
// distribution, and a fabric preset fills in the plots and heights of one of Night City's fabrics
// (NightCity_BuildingKit.md section 2). Edits stay local: a plot can be emptied or given its storeys and massing
// (Overrides), a special building (ACSCityBuildingActor) dropped on the street takes its place, and the spline's
// points' scale (Z) scales the storeys near them.
//
// It stands on its own: a level with no road network is enough to try a kit or a fabric. Hung under a road (an
// ACSRoadSplineActor) with bFollowRoad it lines a stretch of that road instead of its own spline. Belonging to a road
// network (NetworkOf), it keeps clear of the network's roads as built, and reserves its strip of ground: the network's
// batch plots and the alleys keep out of it, so the stretch it lines is its own.
#pragma once

#include "CoreMinimal.h"
#include "Components/SplineComponent.h"
#include "GameFramework/Actor.h"
#include "CSCityFacadeKit.h"
#include "CSCityStreetActor.generated.h"

class ACSRoadSplineActor;
class UCSCityFacadeKit;
class UCSGpuInstancedMeshComponent;
namespace CSCity
{
class FFacadeGpu;
struct FRoadLine;
}
struct FCSCityStreetActor_Layout;

/** Which sides of the street have buildings, looking along the spline. */
UENUM(BlueprintType)
enum class ECSCityStreetSides : uint8
{
	Both,
	Right,
	Left,
};

UENUM(BlueprintType)
enum class ECSCityStreetSide : uint8
{
	Right,
	Left,
};

/** Night City's fabrics (NightCity_BuildingKit.md section 2): choosing one fills in the plots and heights below. */
UENUM(BlueprintType)
enum class ECSCityFabric : uint8
{
	/** The values as set. */
	Custom,
	/** A: low-rise scatter at the city's edge, 2-3 storeys, wide gaps (19% of the ground built on). */
	LowScatter UMETA(DisplayName = "A Low-rise Scatter"),
	/** B: low and mid-rise mixed, 4 storeys (9 one in ten), plots left open (36%). */
	MidMixed UMETA(DisplayName = "B Mid-rise Mixed"),
	/** C: a continuous mid-rise street wall, 6 storeys, now and then a tall one (67%). */
	StreetWall UMETA(DisplayName = "C Mid-rise Street Wall"),
	/** D: the high-rise core, towers on podiums, 22 storeys (45 one in ten), hardly a gap (81%). */
	HighCore UMETA(DisplayName = "D High-rise Core"),
};

/** One plot set by hand: which one (the Plots list names them), and what stands on it. */
USTRUCT(BlueprintType)
struct CSROADNET_API FCSCityStreetLot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot")
	ECSCityStreetSide Side = ECSCityStreetSide::Right;

	/** Its slot along the street, from 0 at the spline's start. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot", meta = (ClampMin = "0"))
	int32 Slot = 0;

	/** Nothing built there: the plot stays open (and still reserved). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot")
	bool bEmpty = false;

	/** 0: drawn as the others are. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot", meta = (ClampMin = "0", EditCondition = "!bEmpty"))
	int32 Storeys = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot", meta = (EditCondition = "!bEmpty"))
	ECSCityMassShape MassShape = ECSCityMassShape::Auto;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Plot", meta = (EditCondition = "!bEmpty"))
	ECSCityMassStack MassStack = ECSCityMassStack::Auto;
};

/** The street of an ACSCityStreetActor: only its points' x and y count for the plots; their scale's Z scales the storeys
 *  near them. */
UCLASS(ClassGroup = (CS2Road))
class CSROADNET_API UCSCityStreetSplineComponent : public USplineComponent
{
	GENERATED_BODY()

public:
	UCSCityStreetSplineComponent(const FObjectInitializer& ObjectInitializer);

	// Every change of the curve, from the editor's spline tools (each step of a drag) or from code.
	virtual void UpdateSpline() override;
};

UCLASS()
class CSROADNET_API ACSCityStreetActor : public AActor
{
	GENERATED_BODY()

public:
	ACSCityStreetActor();
	virtual ~ACSCityStreetActor() override;
	// Declared here so UHT does not define it in the .gen.cpp, where Layout's type is incomplete (as ACSRoadNetworkActor's).
	ACSCityStreetActor(FVTableHelper& Helper);

	/** The street's centre line (unless it follows a road). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "City Street")
	TObjectPtr<UCSCityStreetSplineComponent> Street;

	/** Hung under a road (an ACSRoadSplineActor): line that road from FollowFrom to FollowTo, as wide as it is, instead of
	 *  the own spline. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	bool bFollowRoad = true;

	/** Where along the road the street starts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street", meta = (ClampMin = "0", Units = "m", EditCondition = "bFollowRoad"))
	float FollowFrom = 0.0f;

	/** Where along the road it ends; 0: at the road's end. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street", meta = (ClampMin = "0", Units = "m", EditCondition = "bFollowRoad"))
	float FollowTo = 0.0f;

	/** The street's width about its own spline (the plots stand Setback from its edge). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street", meta = (ClampMin = "0", Units = "m"))
	float StreetWidth = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	ECSCityStreetSides Sides = ECSCityStreetSides::Both;

	/** Fills in the plots and heights below as one of Night City's fabrics; any of them can be changed afterwards. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	ECSCityFabric Fabric = ECSCityFabric::Custom;

	/** Along the street per plot, of which Gap stays between neighbours (0: a continuous street wall). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "4", Units = "m"))
	float Frontage = 16.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "0", Units = "m"))
	float Gap = 2.0f;

	/** From the street's edge to the plots. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "0", Units = "m"))
	float Setback = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "2", Units = "m"))
	float MinDepth = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "2", Units = "m"))
	float MaxDepth = 18.0f;

	/** How many plots are left open (drawn from each plot's id): with the gaps, how much of the street is built on. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Plots", meta = (ClampMin = "0", ClampMax = "1"))
	float Vacancy = 0.0f;

	/** Half the buildings have fewer storeys than this... */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Height", meta = (ClampMin = "1"))
	float MedianStoreys = 4.0f;

	/** ...and nine in ten fewer than this (a log-normal spread between them, as Night City's roofs have). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Height", meta = (ClampMin = "1"))
	float P90Storeys = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Height", meta = (ClampMin = "1"))
	int32 MinStoreys = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Height", meta = (ClampMin = "1"))
	int32 MaxStoreys = 60;

	/** The chances below instead of the kit's: whether a building is a tower on a podium (six storeys or more), else
	 *  steps back (four or more). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Massing")
	bool bOverrideStacking = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Massing", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bOverrideStacking"))
	float PodiumChance = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street|Massing", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bOverrideStacking"))
	float SetbackChance = 0.35f;

	/** What the plots' depths and heights and the buildings' massing and windows are drawn from. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	int32 Seed = 1;

	/** What the buildings are put together from (heights in whole storeys of it). Empty: UCSCityFacadeKit::Default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	TObjectPtr<UCSCityFacadeKit> Kit;

	/** Each building stands on whatever the ground below its plot is (traced, half a metre into it below its lowest
	 *  corner); else on the street's height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	bool bOnGround = true;

	/** Plots set by hand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Street")
	TArray<FCSCityStreetLot> Overrides;

	/** Every plot as last laid out: side and slot (what Overrides name), storeys, size and massing. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Street")
	TArray<FString> Plots;

	UPROPERTY(VisibleAnywhere, Transient, Category = "City Street")
	int32 NumBuildings = 0;

	/** What it lines: the own spline, or the road it follows (or why it cannot). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Street")
	FString Status;

	/** The kit's modules, a component per slot (null where the kit has none), made when it builds. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "City Street")
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> Facade;

	/** Hangs it under the road nearest its spline's start, following it. */
	UFUNCTION(CallInEditor, Category = "City Street")
	void FollowNearestRoad();

	// The road it follows: its attach parent, when bFollowRoad.
	ACSRoadSplineActor* FollowedRoad() const;
	// The ground it keeps (world space, counter-clockwise convex pieces): its strip along each side it builds on, from
	// the street's edge back as deep as a plot may be, and every plot.
	const TArray<TArray<FVector2D>>& Reservations() const;
	// The plots as last laid out (world space, counter-clockwise), built on or not.
	TArray<TArray<FVector2D>> PlotCorners() const;

	// Lays the plots out again and, when they or what stands on them changed, writes the modules (drawn a frame or two
	// later, no wait for the render thread); then has the network (or, with none, the streets after it) follow.
	void Rebuild();
	// The network's part (NetworkOf): its roads as built (world space) near the street, and the width of the road it
	// follows (0: none). Lays the plots out with them; false when nothing changed. The network's batch plots come after.
	bool UpdateFromNetwork(const TArray<CSCity::FRoadLine>& Roads, double FollowedWidth);
	// The street's centre line as laid out from (world space, points about 2 m apart), along the followed road's
	// stretch or the own spline.
	TArray<FVector> CentreLine() const;
	// The streets of the level laid out again in name order (given After, only those after it): with no road network,
	// what a special building's change, or a street's, asks for.
	static void RelayoutStreets(const ULevel* Level, const ACSCityStreetActor* After = nullptr);

	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

	// The values a fabric fills in (Custom: none).
	void ApplyFabric(ECSCityFabric Preset);

private:
	TSharedPtr<CSCity::FFacadeGpu> FacadeGpu;
	TUniquePtr<FCSCityStreetActor_Layout> Layout;

	// Lays out and writes; false when nothing changed.
	bool Relayout();
};

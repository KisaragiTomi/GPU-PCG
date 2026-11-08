// The city's attribute field: one byte for every 4 m cell of the world (LocalUpdateSystem_Plan.md, 6.4), saying what stands there,
// and where anything has been raised or lowered, a second byte of height. Sparse: only pages with something painted exist, 64 m
// square, 256 bytes a layer; on the GPU a page table over the painted pages and the pages, which the facade and ground passes read
// as they put things together (Shaders/Private/CSRoad/CSCityFacade.usf, CSCityGround.usf). Each sample looks at several cells and
// goes by their average: a face stands out or not by the cells along it, a corner or a gap takes the style on at least half of the
// four cells round it, the ground's height is interpolated between them.
//
// A cell: 2 bits of overhang (ECSCityOverhangField), 3 of corner style (ECSCityCornerStyle), 2 of gap style (ECSCityGapStyle), 1
// spare. A height: half metres up or down, the ground only (never what stands on a plot). What is set on a building or a plot by
// hand wins over the field, and the field over what the seed draws. One a level; a second is not looked at.
#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "CSCityFacadeKit.h"
#include "GameFramework/Actor.h"
#include "CSCityFieldActor.generated.h"

class UBoxComponent;
class ACSCityFieldActor;

namespace CSCity
{
class FFacadeFieldGpu;
}

/** What a cell says of the faces over it. */
UENUM(BlueprintType)
enum class ECSCityOverhangField : uint8
{
	/** Up to the building's seed. */
	Unset,
	/** A face over these cells stays flush. */
	Never,
	/** A face over these cells stands out past the base. */
	Always,
};

/** What goes in the ground left free between the buildings. */
UENUM(BlueprintType)
enum class ECSCityGapStyle : uint8
{
	/** The ground kit's chance of a planter. */
	Auto,
	Planter,
	/** Cart shops, facing the nearest road. */
	NightMarket,
	/** Nothing: left clear. */
	Empty,
};

/** Which layer a brush paints, and which the map of it shows. */
UENUM(BlueprintType)
enum class ECSCityFieldChannel : uint8
{
	Overhang,
	CornerStyle,
	GapStyle,
	Elevation,
};

USTRUCT()
struct FCSCityFieldPage
{
	GENERATED_BODY()

	/** 16 x 16 cells, row by row. */
	UPROPERTY()
	TArray<uint8> Cells;

	/** The same cells' heights in half metres; empty when none is raised or lowered. */
	UPROPERTY()
	TArray<int8> Heights;
};

/** The map of the field over the world: a flat tile of colour for each stretch of painted cells, over everything, while the field is
 *  selected. Only for looking at: nothing reads it. */
UCLASS(ClassGroup = (CS2Road), hidecategories = (Object, LOD, Physics, Collision, Rendering, Cooking))
class CSROADNET_API UCSCityFieldMapComponent : public UPrimitiveComponent
{
	GENERATED_BODY()

public:
	UCSCityFieldMapComponent(const FObjectInitializer& ObjectInitializer);

	/** Show it when the field is not selected too. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field")
	bool bAlwaysShow = false;

	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FCSCityFieldPaintRequest, ACSCityFieldActor*);

/** Paints what the buildings and the ground over it should do. Choose a channel and what to paint, press Start Paint and drag in the
 *  viewport (the buildings follow when the mouse comes up, Esc to leave), or put the box where you want it and press Paint Box. */
UCLASS(hidecategories = (Input, Physics, Collision, HLOD, Replication, Cooking, Networking))
class CSROADNET_API ACSCityFieldActor : public AActor
{
	GENERATED_BODY()

public:
	ACSCityFieldActor();

	/** The box Paint Box fills. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "City Field")
	TObjectPtr<UBoxComponent> Brush;

	/** The map of the field. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "City Field")
	TObjectPtr<UCSCityFieldMapComponent> Map;

	/** What the brush paints, and what the map shows. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field")
	ECSCityFieldChannel BrushChannel = ECSCityFieldChannel::GapStyle;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field", meta = (EditCondition = "BrushChannel == ECSCityFieldChannel::Overhang", EditConditionHides))
	ECSCityOverhangField BrushOverhang = ECSCityOverhangField::Always;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field", meta = (EditCondition = "BrushChannel == ECSCityFieldChannel::CornerStyle", EditConditionHides))
	ECSCityCornerStyle BrushCorner = ECSCityCornerStyle::Round;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field", meta = (EditCondition = "BrushChannel == ECSCityFieldChannel::GapStyle", EditConditionHides))
	ECSCityGapStyle BrushGap = ECSCityGapStyle::NightMarket;

	/** How far the ground is raised (lowered if negative), to the half metre. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field", meta = (ClampMin = "-60", ClampMax = "60", Units = "m", EditCondition = "BrushChannel == ECSCityFieldChannel::Elevation", EditConditionHides))
	float BrushElevation = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "City Field", meta = (ClampMin = "2", Units = "m"))
	float BrushRadius = 12.0f;

	/** Pages with something painted, 64 m square. */
	UPROPERTY(VisibleAnywhere, Category = "City Field")
	int32 NumPages = 0;

	/** Paint in the viewport with the channel chosen; Esc to leave. Painting Unset, Auto or a height of 0 takes it off again. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "City Field")
	void StartPaint();

	/** What the channel chosen says, over the cells of the box. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "City Field")
	void PaintBox();

	/** Every channel off the cells of the box. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "City Field")
	void EraseBox();

	/** Nothing painted anywhere. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "City Field")
	void ClearField();

	/** Over the cells of a world box (cm, x and y). */
	UFUNCTION(BlueprintCallable, Category = "City Field")
	void SetOverhangInBox(FVector2D Min, FVector2D Max, ECSCityOverhangField Value);

	UFUNCTION(BlueprintCallable, Category = "City Field")
	void SetCornerStyleInBox(FVector2D Min, FVector2D Max, ECSCityCornerStyle Style);

	UFUNCTION(BlueprintCallable, Category = "City Field")
	void SetGapStyleInBox(FVector2D Min, FVector2D Max, ECSCityGapStyle Style);

	/** Raises the ground over them by Metres (lowers if negative), to the half metre. */
	UFUNCTION(BlueprintCallable, Category = "City Field")
	void SetElevationInBox(FVector2D Min, FVector2D Max, float Metres);

	/** A dab of the brush at a world point: the channel chosen, over the cells within BrushRadius. The map shows it now; whatever stands
	 *  there is told when EndPaintStroke is called. */
	UFUNCTION(BlueprintCallable, Category = "City Field")
	void PaintDab(FVector2D World);

	/** The stroke is done: whatever stands where it went is written again. */
	UFUNCTION(BlueprintCallable, Category = "City Field")
	void EndPaintStroke();

	UFUNCTION(BlueprintPure, Category = "City Field")
	ECSCityOverhangField OverhangAt(FVector2D World) const;

	UFUNCTION(BlueprintPure, Category = "City Field")
	ECSCityCornerStyle CornerStyleAt(FVector2D World) const;

	UFUNCTION(BlueprintPure, Category = "City Field")
	ECSCityGapStyle GapStyleAt(FVector2D World) const;

	/** How far the ground is raised at the cell under a point, in metres. */
	UFUNCTION(BlueprintPure, Category = "City Field")
	float ElevationAt(FVector2D World) const;

	/** The bytes it takes on the GPU: the page table and the pages. */
	UFUNCTION(BlueprintPure, Category = "City Field")
	int32 GpuBytes() const;

	/** How many flat tiles the map shows now. */
	UFUNCTION(BlueprintPure, Category = "City Field")
	int32 MapTiles() const;

	// The field in World; null when it has none.
	static ACSCityFieldActor* Find(const UWorld* World);
	// Equal hashes, the same cells in the pages over a world box: what a writer compares, so that it writes again when they change. It
	// never decides anything from them: that is the GPU's.
	uint64 HashBox(const FBox2D& Box) const;
	// The field's GPU copy, up to date (null with nothing painted): uploaded when it has changed, what was uploaded before left to
	// whatever is still reading it.
	TSharedPtr<CSCity::FFacadeFieldGpu> GpuField() const;
	// How far any height goes from 0 (cm).
	double MaxElevation() const;

	// The map's flat tiles: a rectangle of cells (world cm) and its colour.
	struct FMapTile
	{
		FBox2D Box;
		FColor Color;
	};
	TArray<FMapTile> BuildMap() const;

	// The editor's brush (PCGEditorProcess) listens.
	static FCSCityFieldPaintRequest OnPaintRequest;

	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif

private:
	UPROPERTY()
	TMap<FIntPoint, FCSCityFieldPage> Pages;

	uint32 Revision = 1;
	mutable uint32 GpuRevision = 0;
	mutable TSharedPtr<CSCity::FFacadeFieldGpu> Gpu;
	FBox2D StrokeBox = FBox2D(ForceInit);

	void Fill(const FBox2D& Box, uint8 Mask, uint8 Value);
	void FillHeights(const FBox2D& Box, int8 Value);
	void PaintCell(int32 X, int32 Y);
	void DropEmptyPages();
	FBox2D BrushBox() const;
	FBox2D PaintedBox() const;
	uint8 ValueAt(const FVector2D& World) const;
	// Whatever stands over a box of cells is told, and the map drawn again.
	void Changed(const FBox2D& Box);
	void RedrawMap();
};

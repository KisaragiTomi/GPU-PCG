#include "CSCityFieldActor.h"

#include "Components/BoxComponent.h"
#include "CSCityBuildingActor.h"
#include "CSCityFacade.h"
#include "CSCityGpuWrite.h"
#include "CSCityStreetActor.h"
#include "CSRoadNetworkActor.h"
#include "DynamicMeshBuilder.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Hash/xxhash.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Materials/MaterialRenderProxy.h"
#include "PrimitiveSceneProxy.h"
#include "SceneManagement.h"
#include "SceneView.h"

FCSCityFieldPaintRequest ACSCityFieldActor::OnPaintRequest;

namespace
{
constexpr double CSCityField_CellCm = 400.0;
constexpr double CSCityField_PageCm = 6400.0;
constexpr int32 CSCityField_PageCells = 16;
constexpr int32 CSCityField_PageBytes = CSCityField_PageCells * CSCityField_PageCells;
constexpr uint8 CSCityField_OverhangMask = 3;
constexpr uint8 CSCityField_CornerMask = 7 << 2;
constexpr uint8 CSCityField_GapMask = 3 << 5;
constexpr double CSCityField_HeightStep = 50.0;

int32 CSCityField_Cell(double World)
{
	return FMath::FloorToInt32(World / CSCityField_CellCm);
}

// The cell's page and its place in the page.
FIntPoint CSCityField_Page(int32 CellX, int32 CellY)
{
	return FIntPoint(CellX >> 4, CellY >> 4);
}

int32 CSCityField_Index(int32 CellX, int32 CellY)
{
	return (CellY & 15) * CSCityField_PageCells + (CellX & 15);
}

// What each channel's values look like on the map.
FColor CSCityField_Color(ECSCityFieldChannel Channel, uint8 Cell, int8 Height)
{
	const uint8 Alpha = 110;
	switch (Channel)
	{
	case ECSCityFieldChannel::Overhang:
		if ((Cell & CSCityField_OverhangMask) == uint8(ECSCityOverhangField::Always)) return FColor(255, 150, 30, Alpha);
		if ((Cell & CSCityField_OverhangMask) == uint8(ECSCityOverhangField::Never)) return FColor(40, 120, 255, Alpha);
		break;
	case ECSCityFieldChannel::CornerStyle:
		switch (ECSCityCornerStyle((Cell & CSCityField_CornerMask) >> 2))
		{
		case ECSCityCornerStyle::Square: return FColor(200, 200, 200, Alpha);
		case ECSCityCornerStyle::Chamfer: return FColor(40, 220, 230, Alpha);
		case ECSCityCornerStyle::Round: return FColor(240, 60, 220, Alpha);
		case ECSCityCornerStyle::RoundLarge: return FColor(130, 60, 240, Alpha);
		default: break;
		}
		break;
	case ECSCityFieldChannel::GapStyle:
		switch (ECSCityGapStyle((Cell & CSCityField_GapMask) >> 5))
		{
		case ECSCityGapStyle::Planter: return FColor(50, 220, 70, Alpha);
		case ECSCityGapStyle::NightMarket: return FColor(255, 60, 60, Alpha);
		case ECSCityGapStyle::Empty: return FColor(90, 90, 90, Alpha);
		default: break;
		}
		break;
	case ECSCityFieldChannel::Elevation:
		if (Height > 0) return FColor(uint8(FMath::Min(120 + int32(Height) * 6, 255)), 90, 40, Alpha);
		if (Height < 0) return FColor(40, 90, uint8(FMath::Min(120 - int32(Height) * 6, 255)), Alpha);
		break;
	}
	return FColor(0, 0, 0, 0);
}

// The map: its flat tiles drawn over everything, a mesh a colour.
class FCSCityFieldMapProxy final : public FPrimitiveSceneProxy
{
public:
	FCSCityFieldMapProxy(const UCSCityFieldMapComponent* Component, TArray<ACSCityFieldActor::FMapTile> InTiles, double InZ)
		: FPrimitiveSceneProxy(Component), Tiles(MoveTemp(InTiles)), Z(InZ), bAlways(Component->bAlwaysShow)
	{
		bWillEverBeLit = false;
		Tiles.Sort([](const ACSCityFieldActor::FMapTile& A, const ACSCityFieldActor::FMapTile& B) { return A.Color.DWColor() < B.Color.DWColor(); });
	}

	virtual SIZE_T GetTypeHash() const override
	{
		static size_t UniquePointer;
		return reinterpret_cast<size_t>(&UniquePointer);
	}

	virtual uint32 GetMemoryFootprint() const override { return sizeof(*this) + GetAllocatedSize(); }
	uint32 GetAllocatedSize() const { return FPrimitiveSceneProxy::GetAllocatedSize() + uint32(Tiles.GetAllocatedSize()); }

	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override
	{
		FPrimitiveViewRelevance Result;
		const bool bShown = IsShown(View) && (bAlways || IsSelected() || IsIndividuallySelected());
		Result.bDrawRelevance = bShown;
		Result.bDynamicRelevance = true;
		Result.bShadowRelevance = false;
		Result.bEditorPrimitiveRelevance = UseEditorCompositing(View);
		Result.bSeparateTranslucency = bShown;
		Result.bNormalTranslucency = bShown;
		return Result;
	}

	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const override
	{
		if (!GEngine || !GEngine->DebugMeshMaterial) return;
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			if (!(VisibilityMap & (1 << ViewIndex))) continue;
			for (int32 First = 0; First < Tiles.Num();)
			{
				int32 End = First;
				while (End < Tiles.Num() && Tiles[End].Color == Tiles[First].Color) End++;
				FDynamicMeshBuilder Builder(Views[ViewIndex]->GetFeatureLevel());
				for (int32 I = First; I < End; I++)
				{
					const FBox2D& Box = Tiles[I].Box;
					const int32 V = (I - First) * 4;
					const FVector3f Tangent(1.0f, 0.0f, 0.0f);
					const FVector3f Bitangent(0.0f, 1.0f, 0.0f);
					const FVector3f Normal(0.0f, 0.0f, 1.0f);
					Builder.AddVertex(FVector3f(FVector(Box.Min.X, Box.Min.Y, Z)), FVector2f(0.0f, 0.0f), Tangent, Bitangent, Normal, Tiles[I].Color);
					Builder.AddVertex(FVector3f(FVector(Box.Max.X, Box.Min.Y, Z)), FVector2f(1.0f, 0.0f), Tangent, Bitangent, Normal, Tiles[I].Color);
					Builder.AddVertex(FVector3f(FVector(Box.Max.X, Box.Max.Y, Z)), FVector2f(1.0f, 1.0f), Tangent, Bitangent, Normal, Tiles[I].Color);
					Builder.AddVertex(FVector3f(FVector(Box.Min.X, Box.Max.Y, Z)), FVector2f(0.0f, 1.0f), Tangent, Bitangent, Normal, Tiles[I].Color);
					Builder.AddTriangle(V, V + 2, V + 1);
					Builder.AddTriangle(V, V + 3, V + 2);
				}
				FColoredMaterialRenderProxy* Colored = &Collector.AllocateOneFrameResource<FColoredMaterialRenderProxy>(GEngine->DebugMeshMaterial->GetRenderProxy(), Tiles[First].Color);
				Builder.GetMesh(FMatrix::Identity, Colored, SDPG_Foreground, /*bDisableBackfaceCulling*/ true, /*bReceivesDecals*/ false, ViewIndex, Collector);
				First = End;
			}
		}
	}

private:
	TArray<ACSCityFieldActor::FMapTile> Tiles;
	double Z;
	bool bAlways;
};
}

UCSCityFieldMapComponent::UCSCityFieldMapComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bIsEditorOnly = true;
	bHiddenInGame = true;
	CastShadow = false;
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
}

FPrimitiveSceneProxy* UCSCityFieldMapComponent::CreateSceneProxy()
{
	const ACSCityFieldActor* Field = Cast<ACSCityFieldActor>(GetOwner());
	if (!Field) return nullptr;
	TArray<ACSCityFieldActor::FMapTile> Tiles = Field->BuildMap();
	if (Tiles.Num() == 0) return nullptr;
	return new FCSCityFieldMapProxy(this, MoveTemp(Tiles), GetComponentLocation().Z);
}

FBoxSphereBounds UCSCityFieldMapComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	const ACSCityFieldActor* Field = Cast<ACSCityFieldActor>(GetOwner());
	FBox2D Painted(ForceInit);
	if (Field) for (const ACSCityFieldActor::FMapTile& Tile : Field->BuildMap()) Painted += Tile.Box;
	const double Z = LocalToWorld.GetLocation().Z;
	if (!Painted.bIsValid) return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector(100.0), 100.0);
	return FBoxSphereBounds(FBox(FVector(Painted.Min, Z - 100.0), FVector(Painted.Max, Z + 100.0)));
}

ACSCityFieldActor::ACSCityFieldActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Brush = CreateDefaultSubobject<UBoxComponent>(TEXT("Brush"));
	Brush->SetBoxExtent(FVector(2000.0, 2000.0, 1000.0));
	Brush->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Brush->SetHiddenInGame(true);
	RootComponent = Brush;
	Map = CreateDefaultSubobject<UCSCityFieldMapComponent>(TEXT("Map"));
	Map->SetupAttachment(Brush);
	Map->SetUsingAbsoluteRotation(true);
	Map->SetUsingAbsoluteScale(true);
}

ACSCityFieldActor* ACSCityFieldActor::Find(const UWorld* World)
{
	if (!World) return nullptr;
	for (TActorIterator<ACSCityFieldActor> It(World); It; ++It) if (IsValid(*It)) return *It;
	return nullptr;
}

void ACSCityFieldActor::Fill(const FBox2D& Box, uint8 Mask, uint8 Value)
{
	if (!Box.bIsValid) return;
	const int32 X0 = CSCityField_Cell(Box.Min.X);
	const int32 X1 = CSCityField_Cell(Box.Max.X);
	const int32 Y0 = CSCityField_Cell(Box.Min.Y);
	const int32 Y1 = CSCityField_Cell(Box.Max.Y);
	Modify();
	for (int32 Y = Y0; Y <= Y1; Y++)
	{
		for (int32 X = X0; X <= X1; X++)
		{
			const FIntPoint Key = CSCityField_Page(X, Y);
			FCSCityFieldPage* Page = Pages.Find(Key);
			if (!Page)
			{
				// Nothing there, and nothing to write: no page.
				if (Value == 0) continue;
				Page = &Pages.Add(Key);
				Page->Cells.SetNumZeroed(CSCityField_PageBytes);
			}
			uint8& Cell = Page->Cells[CSCityField_Index(X, Y)];
			Cell = (Cell & ~Mask) | (Value & Mask);
		}
	}
	DropEmptyPages();
	Changed(Box);
}

void ACSCityFieldActor::FillHeights(const FBox2D& Box, int8 Value)
{
	if (!Box.bIsValid) return;
	const int32 X0 = CSCityField_Cell(Box.Min.X);
	const int32 X1 = CSCityField_Cell(Box.Max.X);
	const int32 Y0 = CSCityField_Cell(Box.Min.Y);
	const int32 Y1 = CSCityField_Cell(Box.Max.Y);
	Modify();
	for (int32 Y = Y0; Y <= Y1; Y++)
	{
		for (int32 X = X0; X <= X1; X++)
		{
			const FIntPoint Key = CSCityField_Page(X, Y);
			FCSCityFieldPage* Page = Pages.Find(Key);
			if (!Page)
			{
				if (Value == 0) continue;
				Page = &Pages.Add(Key);
				Page->Cells.SetNumZeroed(CSCityField_PageBytes);
			}
			if (Page->Heights.Num() == 0)
			{
				if (Value == 0) continue;
				Page->Heights.SetNumZeroed(CSCityField_PageBytes);
			}
			Page->Heights[CSCityField_Index(X, Y)] = Value;
		}
	}
	DropEmptyPages();
	Changed(Box);
}

void ACSCityFieldActor::DropEmptyPages()
{
	// A layer or a page left with nothing goes.
	for (auto It = Pages.CreateIterator(); It; ++It)
	{
		bool bHeights = false;
		for (const int8 Height : It.Value().Heights)
		{
			bHeights = Height != 0;
			if (bHeights) break;
		}
		if (!bHeights) It.Value().Heights.Reset();
		bool bAny = bHeights;
		for (int32 Cell = 0; Cell < It.Value().Cells.Num() && !bAny; Cell++) bAny = It.Value().Cells[Cell] != 0;
		if (!bAny) It.RemoveCurrent();
	}
	NumPages = Pages.Num();
}

void ACSCityFieldActor::SetOverhangInBox(FVector2D Min, FVector2D Max, ECSCityOverhangField Value)
{
	Fill(FBox2D(Min, Max), CSCityField_OverhangMask, uint8(Value));
}

void ACSCityFieldActor::SetCornerStyleInBox(FVector2D Min, FVector2D Max, ECSCityCornerStyle Style)
{
	Fill(FBox2D(Min, Max), CSCityField_CornerMask, uint8(uint8(Style) << 2));
}

void ACSCityFieldActor::SetGapStyleInBox(FVector2D Min, FVector2D Max, ECSCityGapStyle Style)
{
	Fill(FBox2D(Min, Max), CSCityField_GapMask, uint8(uint8(Style) << 5));
}

void ACSCityFieldActor::SetElevationInBox(FVector2D Min, FVector2D Max, float Metres)
{
	FillHeights(FBox2D(Min, Max), int8(FMath::Clamp(FMath::RoundToInt32(Metres * 100.0f / CSCityField_HeightStep), -127, 127)));
}

void ACSCityFieldActor::StartPaint()
{
	OnPaintRequest.Broadcast(this);
}

void ACSCityFieldActor::PaintBox()
{
	const FBox2D Box = BrushBox();
	switch (BrushChannel)
	{
	case ECSCityFieldChannel::Overhang: SetOverhangInBox(Box.Min, Box.Max, BrushOverhang); break;
	case ECSCityFieldChannel::CornerStyle: SetCornerStyleInBox(Box.Min, Box.Max, BrushCorner); break;
	case ECSCityFieldChannel::GapStyle: SetGapStyleInBox(Box.Min, Box.Max, BrushGap); break;
	case ECSCityFieldChannel::Elevation: SetElevationInBox(Box.Min, Box.Max, BrushElevation); break;
	}
}

void ACSCityFieldActor::EraseBox()
{
	const FBox2D Box = BrushBox();
	Fill(Box, 0xFF, 0);
	FillHeights(Box, 0);
}

void ACSCityFieldActor::ClearField()
{
	Modify();
	const FBox2D Box = PaintedBox();
	Pages.Reset();
	NumPages = 0;
	if (Box.bIsValid) Changed(Box);
	else RedrawMap();
}

void ACSCityFieldActor::PaintCell(int32 X, int32 Y)
{
	const FIntPoint Key = CSCityField_Page(X, Y);
	const int32 Index = CSCityField_Index(X, Y);
	uint8 Mask = 0;
	uint8 Value = 0;
	int8 Height = 0;
	switch (BrushChannel)
	{
	case ECSCityFieldChannel::Overhang: Mask = CSCityField_OverhangMask; Value = uint8(BrushOverhang); break;
	case ECSCityFieldChannel::CornerStyle: Mask = CSCityField_CornerMask; Value = uint8(uint8(BrushCorner) << 2); break;
	case ECSCityFieldChannel::GapStyle: Mask = CSCityField_GapMask; Value = uint8(uint8(BrushGap) << 5); break;
	case ECSCityFieldChannel::Elevation: Height = int8(FMath::Clamp(FMath::RoundToInt32(BrushElevation * 100.0f / CSCityField_HeightStep), -127, 127)); break;
	}
	FCSCityFieldPage* Page = Pages.Find(Key);
	if (!Page)
	{
		if (Value == 0 && Height == 0) return;
		Page = &Pages.Add(Key);
		Page->Cells.SetNumZeroed(CSCityField_PageBytes);
	}
	if (Mask != 0)
	{
		uint8& Cell = Page->Cells[Index];
		Cell = (Cell & ~Mask) | Value;
		return;
	}
	if (Page->Heights.Num() == 0)
	{
		if (Height == 0) return;
		Page->Heights.SetNumZeroed(CSCityField_PageBytes);
	}
	Page->Heights[Index] = Height;
}

void ACSCityFieldActor::PaintDab(FVector2D World)
{
	const double Radius = FMath::Max(double(BrushRadius), 2.0) * 100.0;
	const FBox2D Box(World - FVector2D(Radius), World + FVector2D(Radius));
	for (int32 Y = CSCityField_Cell(Box.Min.Y); Y <= CSCityField_Cell(Box.Max.Y); Y++)
	{
		for (int32 X = CSCityField_Cell(Box.Min.X); X <= CSCityField_Cell(Box.Max.X); X++)
		{
			const FVector2D Middle((X + 0.5) * CSCityField_CellCm, (Y + 0.5) * CSCityField_CellCm);
			if (FVector2D::DistSquared(Middle, World) <= Radius * Radius) PaintCell(X, Y);
		}
	}
	StrokeBox += Box;
	NumPages = Pages.Num();
	// Only the map: the buildings wait for the stroke's end.
	RedrawMap();
}

void ACSCityFieldActor::EndPaintStroke()
{
	if (!StrokeBox.bIsValid) return;
	const FBox2D Box = StrokeBox;
	StrokeBox = FBox2D(ForceInit);
	DropEmptyPages();
	Changed(Box);
}

int32 ACSCityFieldActor::GpuBytes() const
{
	if (Pages.Num() == 0) return 0;
	FIntPoint Min(MAX_int32, MAX_int32);
	FIntPoint Max(MIN_int32, MIN_int32);
	bool bHeights = false;
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages)
	{
		Min = Min.ComponentMin(Pair.Key);
		Max = Max.ComponentMax(Pair.Key);
		bHeights |= Pair.Value.Heights.Num() > 0;
	}
	return (Max.X - Min.X + 1) * (Max.Y - Min.Y + 1) * int32(sizeof(uint32)) + Pages.Num() * CSCityField_PageBytes * (bHeights ? 2 : 1);
}

int32 ACSCityFieldActor::MapTiles() const
{
	return BuildMap().Num();
}

TArray<ACSCityFieldActor::FMapTile> ACSCityFieldActor::BuildMap() const
{
	TArray<FMapTile> Out;
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages)
	{
		const FCSCityFieldPage& Page = Pair.Value;
		const FVector2D Origin(Pair.Key.X * CSCityField_PageCm, Pair.Key.Y * CSCityField_PageCm);
		for (int32 Y = 0; Y < CSCityField_PageCells; Y++)
		{
			// Each row's cells of one colour, as one tile.
			for (int32 X = 0; X < CSCityField_PageCells;)
			{
				const int32 Index = Y * CSCityField_PageCells + X;
				const FColor Color = CSCityField_Color(BrushChannel, Page.Cells[Index], Page.Heights.Num() > 0 ? Page.Heights[Index] : int8(0));
				int32 End = X + 1;
				while (End < CSCityField_PageCells)
				{
					const int32 Next = Y * CSCityField_PageCells + End;
					if (CSCityField_Color(BrushChannel, Page.Cells[Next], Page.Heights.Num() > 0 ? Page.Heights[Next] : int8(0)) != Color) break;
					End++;
				}
				if (Color.A > 0)
				{
					FMapTile& Tile = Out.AddDefaulted_GetRef();
					Tile.Box = FBox2D(Origin + FVector2D(X, Y) * CSCityField_CellCm, Origin + FVector2D(End, Y + 1) * CSCityField_CellCm);
					Tile.Color = Color;
				}
				X = End;
			}
		}
	}
	return Out;
}

FBox2D ACSCityFieldActor::BrushBox() const
{
	const FBox Box = Brush->CalcBounds(Brush->GetComponentTransform()).GetBox();
	return FBox2D(FVector2D(Box.Min), FVector2D(Box.Max));
}

uint8 ACSCityFieldActor::ValueAt(const FVector2D& World) const
{
	const int32 X = CSCityField_Cell(World.X);
	const int32 Y = CSCityField_Cell(World.Y);
	const FCSCityFieldPage* Page = Pages.Find(CSCityField_Page(X, Y));
	return Page ? Page->Cells[CSCityField_Index(X, Y)] : 0;
}

ECSCityOverhangField ACSCityFieldActor::OverhangAt(FVector2D World) const
{
	return ECSCityOverhangField(ValueAt(World) & CSCityField_OverhangMask);
}

ECSCityCornerStyle ACSCityFieldActor::CornerStyleAt(FVector2D World) const
{
	return ECSCityCornerStyle((ValueAt(World) & CSCityField_CornerMask) >> 2);
}

ECSCityGapStyle ACSCityFieldActor::GapStyleAt(FVector2D World) const
{
	return ECSCityGapStyle((ValueAt(World) & CSCityField_GapMask) >> 5);
}

float ACSCityFieldActor::ElevationAt(FVector2D World) const
{
	const int32 X = CSCityField_Cell(World.X);
	const int32 Y = CSCityField_Cell(World.Y);
	const FCSCityFieldPage* Page = Pages.Find(CSCityField_Page(X, Y));
	return Page && Page->Heights.Num() > 0 ? float(Page->Heights[CSCityField_Index(X, Y)]) * float(CSCityField_HeightStep) * 0.01f : 0.0f;
}

double ACSCityFieldActor::MaxElevation() const
{
	int32 Most = 0;
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages) for (const int8 Height : Pair.Value.Heights) Most = FMath::Max(Most, FMath::Abs(int32(Height)));
	return Most * CSCityField_HeightStep;
}

uint64 ACSCityFieldActor::HashBox(const FBox2D& Box) const
{
	if (Pages.Num() == 0 || !Box.bIsValid) return 0;
	const FIntPoint Min = CSCityField_Page(CSCityField_Cell(Box.Min.X), CSCityField_Cell(Box.Min.Y));
	const FIntPoint Max = CSCityField_Page(CSCityField_Cell(Box.Max.X), CSCityField_Cell(Box.Max.Y));
	FXxHash64Builder Hash;
	// Nothing painted over it is 0 whatever is painted elsewhere: a first dab somewhere says nothing to everything else.
	bool bAny = false;
	for (int32 Y = Min.Y; Y <= Max.Y; Y++)
	{
		for (int32 X = Min.X; X <= Max.X; X++)
		{
			const FCSCityFieldPage* Page = Pages.Find(FIntPoint(X, Y));
			if (!Page) continue;
			bAny = true;
			const FIntPoint Key(X, Y);
			Hash.Update(&Key, sizeof(Key));
			Hash.Update(Page->Cells.GetData(), Page->Cells.Num());
			if (Page->Heights.Num() > 0) Hash.Update(Page->Heights.GetData(), Page->Heights.Num());
		}
	}
	return bAny ? Hash.Finalize().Hash : 0;
}

FBox2D ACSCityFieldActor::PaintedBox() const
{
	FBox2D Box(ForceInit);
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages)
	{
		const FVector2D Min(Pair.Key.X * CSCityField_PageCm, Pair.Key.Y * CSCityField_PageCm);
		Box += Min;
		Box += Min + FVector2D(CSCityField_PageCm);
	}
	return Box;
}

TSharedPtr<CSCity::FFacadeFieldGpu> ACSCityFieldActor::GpuField() const
{
	if (Pages.Num() == 0) return nullptr;
	if (Gpu && GpuRevision == Revision) return Gpu;
	FIntPoint Min(MAX_int32, MAX_int32);
	FIntPoint Max(MIN_int32, MIN_int32);
	bool bHeights = false;
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages)
	{
		Min = Min.ComponentMin(Pair.Key);
		Max = Max.ComponentMax(Pair.Key);
		bHeights |= Pair.Value.Heights.Num() > 0;
	}
	const FIntPoint Size = Max - Min + FIntPoint(1, 1);
	TArray<uint32> Table;
	Table.SetNumZeroed(Size.X * Size.Y);
	const int32 Words = Pages.Num() * CSCityField_PageBytes / int32(sizeof(uint32));
	TArray<uint32> Packed;
	Packed.SetNumZeroed(Words);
	TArray<uint32> Heights;
	if (bHeights) Heights.SetNumZeroed(Words);
	int32 Index = 0;
	for (const TPair<FIntPoint, FCSCityFieldPage>& Pair : Pages)
	{
		Table[(Pair.Key.Y - Min.Y) * Size.X + (Pair.Key.X - Min.X)] = uint32(Index + 1);
		for (int32 Cell = 0; Cell < CSCityField_PageBytes; Cell++)
		{
			const int32 Byte = Index * CSCityField_PageBytes + Cell;
			Packed[Byte >> 2] |= uint32(Pair.Value.Cells[Cell]) << ((Byte & 3) * 8);
			if (Pair.Value.Heights.Num() > 0) Heights[Byte >> 2] |= uint32(uint8(Pair.Value.Heights[Cell])) << ((Byte & 3) * 8);
		}
		Index++;
	}
	Gpu = CSCity::MakeFacadeField(MoveTemp(Table), MoveTemp(Packed), MoveTemp(Heights), Min, Size);
	GpuRevision = Revision;
	return Gpu;
}

void ACSCityFieldActor::RedrawMap()
{
	if (!Map) return;
	Map->MarkRenderStateDirty();
	Map->UpdateBounds();
}

void ACSCityFieldActor::Changed(const FBox2D& Box)
{
	Revision++;
	RedrawMap();
	UWorld* World = GetWorld();
	if (!World) return;
	// Whatever may look at those cells: a building's corners read the cells round them, so a cell further on all round.
	const FBox2D Reach = Box.ExpandBy(CSCityField_CellCm);
	for (ACSRoadNetworkActor* Network : TActorRange<ACSRoadNetworkActor>(World)) Network->FieldChanged(Reach);
	for (ACSCityStreetActor* Street : TActorRange<ACSCityStreetActor>(World)) if (!ACSRoadNetworkActor::NetworkOf(Street)) Street->FieldChanged();
	for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
	{
		const TArray<FVector2D> Footprint = Building->Footprint();
		FBox2D Around(ForceInit);
		for (const FVector2D& Corner : Footprint) Around += Corner;
		if (Around.bIsValid && Around.Intersect(Reach)) Building->WriteModules();
	}
}

void ACSCityFieldActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	NumPages = Pages.Num();
	// What loaded before this did not see it.
	if (!IsTemplate() && Pages.Num() > 0) Changed(PaintedBox());
}

void ACSCityFieldActor::Destroyed()
{
	const FBox2D Box = PaintedBox();
	Super::Destroyed();
	if (Box.bIsValid) Changed(Box);
}

#if WITH_EDITOR
void ACSCityFieldActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	// The map shows the channel the brush paints.
	RedrawMap();
}

void ACSCityFieldActor::PostEditUndo()
{
	Super::PostEditUndo();
	NumPages = Pages.Num();
	const FBox2D Box = PaintedBox();
	// Whatever the undo took away is somewhere the buildings were written from: let them all look.
	Changed(Box.bIsValid ? Box.ExpandBy(1e7) : FBox2D(FVector2D(-1e7), FVector2D(1e7)));
}
#endif

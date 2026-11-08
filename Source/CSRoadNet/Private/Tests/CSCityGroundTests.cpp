#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityFieldActor.h"
#include "CSCityGround.h"
#include "CSCityGroundKit.h"
#include "CSCityLayout.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadMeshWrite.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadTestUtils.h"
#include "Components/SplineComponent.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

namespace
{
using namespace CSCity;

constexpr double CSCityGroundTests_RoadY = 6000.0;
constexpr double CSCityGroundTests_HalfWidth = 500.0;

// The kit's boxes without any mesh: the passes only need its shape.
FGroundKitShape CSCityGroundTests_Kit()
{
	FGroundKitShape Kit;
	const FBox Boxes[GroundSlots] = {
		FBox(FVector(0.0, 0.0, -50.0), FVector(300.0, 300.0, 0.0)),
		FBox(FVector(0.0, -64.0, 0.0), FVector(300.0, 0.0, 100.0)),
		FBox(FVector(0.0, 0.0, 0.0), FVector(300.0, 150.0, 100.0)),
		FBox(FVector(0.0, 0.0, 0.0), FVector(200.0, 200.0, 500.0)),
		FBox(FVector(0.0, 0.0, 0.0), FVector(240.0, 150.0, 260.0)),
		FBox(FVector(0.0, 0.0, 0.0), FVector(300.0, 150.0, 280.0)),
	};
	for (int32 Slot = 0; Slot < GroundSlots; Slot++)
	{
		Kit.bHas[Slot] = true;
		Kit.Bounds[Slot] = Boxes[Slot];
	}
	return Kit;
}

// A 120 m square at the origin with a road along X through its middle, rising Slope per cm.
FGroundTile CSCityGroundTests_Tile(double Slope = 0.0)
{
	FGroundTile Tile;
	Tile.Min = FVector2D::ZeroVector;
	Tile.Size = 12000.0;
	for (double X = -6000.0; X < 18000.0; X += 800.0)
	{
		FGroundRoad& Piece = Tile.Roads.AddDefaulted_GetRef();
		Piece.A = FVector(X, CSCityGroundTests_RoadY, X * Slope);
		Piece.B = FVector(X + 800.0, CSCityGroundTests_RoadY, (X + 800.0) * Slope);
		Piece.HalfWidth = CSCityGroundTests_HalfWidth;
	}
	return Tile;
}

FGroundLot CSCityGroundTests_Lot(double MinX, double MaxX, double MinY, double MaxY, double Base = 0.0, bool bLevel = true)
{
	FGroundLot Lot;
	Lot.Centre = FVector2D((MinX + MaxX) * 0.5, (MinY + MaxY) * 0.5);
	Lot.Front = FVector2D(0.0, -1.0);
	Lot.HalfWidth = (MaxX - MinX) * 0.5;
	Lot.HalfDepth = (MaxY - MinY) * 0.5;
	Lot.Base = Base;
	Lot.bLevel = bLevel;
	return Lot;
}

// The highest slab over a point (cm), or none.
TOptional<double> CSCityGroundTests_Top(const FGroundLayout& Layout, const FVector2D& P)
{
	TOptional<double> Top;
	for (const FBox& Box : Layout.Boxes[int32(EGroundSlot::Slab)])
	{
		if (P.X < Box.Min.X || P.X > Box.Max.X || P.Y < Box.Min.Y || P.Y > Box.Max.Y) continue;
		Top = FMath::Max(Top.Get(TNumericLimits<double>::Lowest()), Box.Max.Z);
	}
	return Top;
}

TArray<FBox> CSCityGroundTests_Props(const FGroundLayout& Layout)
{
	TArray<FBox> Out;
	for (const EGroundSlot Slot : { EGroundSlot::Planter, EGroundSlot::Planter2, EGroundSlot::CartShop, EGroundSlot::CartShop2 }) Out.Append(Layout.Boxes[int32(Slot)]);
	return Out;
}
}

// The ground on its own, from the passes: slabs fill a block without a hole, a slab follows a slope and stops where a road covers it; too steep and it
// stays level on walls; a plot takes the ground to its own level; what is free between buildings wide enough has something on it, a gap too narrow nothing.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityGroundTest, "PCGPlugins.CSRoadNet.City.Ground",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityGroundTest::RunTest(const FString& Parameters)
{
	const FGroundKitShape Kit = CSCityGroundTests_Kit();
	auto RoadDist = [](const FVector2D& P) { return FMath::Abs(P.Y - CSCityGroundTests_RoadY) - CSCityGroundTests_HalfWidth; };

	// Flat: everything off the road covered, a few big slabs and small ones by the road, nothing to wall.
	{
		FGroundKitShape NoProps = Kit;
		NoProps.AutoPlanterChance = 0.0;
		const FGroundLayout Flat = LayoutGround(NoProps, CSCityGroundTests_Tile());
		TestTrue(TEXT("flat: slabs"), Flat.Slabs() > 0);
		TestEqual(TEXT("flat: the boxes come back"), Flat.Boxes[int32(EGroundSlot::Slab)].Num(), int32(Flat.Slabs()));
		TestEqual(TEXT("flat: no walls"), Flat.Walls(), 0u);
		TestEqual(TEXT("flat: no planters at no chance"), Flat.Planters(), 0u);
		int32 Holes = 0;
		int32 High = 0;
		for (double Y = 75.0; Y < 12000.0; Y += 150.0)
		{
			for (double X = 75.0; X < 12000.0; X += 150.0)
			{
				const FVector2D P(X, Y);
				if (RoadDist(P) < 10.0) continue;
				const TOptional<double> Top = CSCityGroundTests_Top(Flat, P);
				Holes += Top.IsSet() ? 0 : 1;
				High += Top.IsSet() && FMath::Abs(Top.GetValue()) > 3.0 ? 1 : 0;
			}
		}
		TestEqual(TEXT("flat: holes off the road"), Holes, 0);
		TestEqual(TEXT("flat: tops off the road's level"), High, 0);
		int32 Big = 0;
		int32 Small = 0;
		int32 Under = 0;
		for (const FBox& Box : Flat.Boxes[int32(EGroundSlot::Slab)])
		{
			const double Size = Box.GetSize().X;
			Big += Size > 1100.0 ? 1 : 0;
			Small += Size < 400.0 ? 1 : 0;
			Under += Box.Min.Y > CSCityGroundTests_RoadY - CSCityGroundTests_HalfWidth && Box.Max.Y < CSCityGroundTests_RoadY + CSCityGroundTests_HalfWidth ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("flat: %u slabs, %d of 12 m, %d of 3 m"), Flat.Slabs(), Big, Small));
		TestTrue(TEXT("flat: 12 m slabs far from the road"), Big >= 20);
		TestTrue(TEXT("flat: 3 m slabs at the road's edge"), Small >= 40);
		TestTrue(TEXT("flat: far fewer than 3 m slabs everywhere"), Flat.Slabs() < 800u);
		TestEqual(TEXT("flat: slabs wholly under the road"), Under, 0);

		// Past its reach there is none.
		FGroundKitShape Short = NoProps;
		Short.Reach = 2000.0;
		const FGroundLayout Near = LayoutGround(Short, CSCityGroundTests_Tile());
		int32 Beyond = 0;
		for (const FBox& Box : Near.Boxes[int32(EGroundSlot::Slab)]) Beyond += FMath::Min(RoadDist(FVector2D(Box.Min)), RoadDist(FVector2D(Box.Max))) > 2000.0 + 700.0 ? 1 : 0;
		TestTrue(TEXT("short reach: fewer"), Near.Slabs() < Flat.Slabs());
		TestEqual(TEXT("short reach: slabs out of it"), Beyond, 0);
	}

	// A slope: by the road the ground is under its edge, wherever along it; slabs are turned, no walls.
	{
		const double Slope = 0.05;
		FGroundKitShape NoProps = Kit;
		NoProps.AutoPlanterChance = 0.0;
		const FGroundLayout Gentle = LayoutGround(NoProps, CSCityGroundTests_Tile(Slope));
		TestEqual(TEXT("gentle: no walls"), Gentle.Walls(), 0u);
		for (const double X : { 1500.0, 6000.0, 10500.0 })
		{
			const TOptional<double> Top = CSCityGroundTests_Top(Gentle, FVector2D(X, CSCityGroundTests_RoadY - CSCityGroundTests_HalfWidth - 60.0));
			if (TestTrue(TEXT("gentle: ground by the road"), Top.IsSet())) TestTrue(FString::Printf(TEXT("gentle: at %.0f, %.1f by a road at %.1f"), X, Top.GetValue(), X * Slope), FMath::Abs(Top.GetValue() - X * Slope) < 30.0);
		}
		int32 Turned = 0;
		for (const FBox& Box : Gentle.Boxes[int32(EGroundSlot::Slab)]) Turned += Box.GetSize().Z > 55.0 ? 1 : 0;
		TestEqual(TEXT("gentle: every slab turned to the slope"), Turned, int32(Gentle.Slabs()));

		const FGroundLayout Steep = LayoutGround(NoProps, CSCityGroundTests_Tile(0.4));
		TestTrue(TEXT("steep: walls"), Steep.Walls() > 0u);
		int32 Level = 0;
		for (const FBox& Box : Steep.Boxes[int32(EGroundSlot::Slab)]) Level += FMath::IsNearlyEqual(Box.GetSize().Z, 50.0, 0.5) ? 1 : 0;
		TestTrue(TEXT("steep: level slabs"), Level > 0);
		int32 Hanging = 0;
		for (const FBox& Box : Steep.Boxes[int32(EGroundSlot::Wall)]) Hanging += Box.GetSize().Z < 40.0 ? 1 : 0;
		TestEqual(TEXT("steep: walls shorter than the least step"), Hanging, 0);
	}

	// A plot's level: the ground under it at its height, whatever the road says; back within its blend.
	{
		FGroundTile Tile = CSCityGroundTests_Tile();
		Tile.Lots.Add(CSCityGroundTests_Lot(3000.0, 9000.0, 8000.0, 11000.0, 150.0));
		FGroundKitShape NoProps = Kit;
		NoProps.AutoPlanterChance = 0.0;
		const FGroundLayout Raised = LayoutGround(NoProps, Tile);
		const TOptional<double> On = CSCityGroundTests_Top(Raised, FVector2D(6000.0, 9500.0));
		if (TestTrue(TEXT("plot: ground under it"), On.IsSet())) TestTrue(FString::Printf(TEXT("plot: at its level, %.1f"), On.GetValue()), FMath::IsNearlyEqual(On.GetValue(), 150.0, 3.0));
		const TOptional<double> Away = CSCityGroundTests_Top(Raised, FVector2D(600.0, 9500.0));
		if (TestTrue(TEXT("plot: ground far from it"), Away.IsSet())) TestTrue(FString::Printf(TEXT("plot: back to the road's, %.1f"), Away.GetValue()), FMath::Abs(Away.GetValue()) < 3.0);
	}

	// Free ground: planters in a gap wide enough, well off the buildings and the road, on the ground, along the road.
	{
		FGroundKitShape Always = Kit;
		Always.AutoPlanterChance = 1.0;
		FGroundTile Wide = CSCityGroundTests_Tile();
		Wide.Lots.Add(CSCityGroundTests_Lot(0.0, 3000.0, 7000.0, 9000.0));
		Wide.Lots.Add(CSCityGroundTests_Lot(9000.0, 12000.0, 7000.0, 9000.0));
		const FGroundLayout Out = LayoutGround(Always, Wide);
		TestEqual(TEXT("free: no carts where nothing asks for them"), Out.Carts(), 0u);
		const TArray<FBox> Props = CSCityGroundTests_Props(Out);
		int32 InGap = 0;
		int32 OnBuildings = 0;
		int32 OnRoad = 0;
		int32 Floating = 0;
		for (const FBox& Box : Props)
		{
			const FVector2D Middle(Box.GetCenter());
			const bool bBand = Middle.Y > 7000.0 && Middle.Y < 9000.0;
			InGap += bBand && Middle.X > 3000.0 && Middle.X < 9000.0 ? 1 : 0;
			OnBuildings += Middle.Y > 7000.0 - 299.0 && Middle.Y < 9000.0 + 299.0 && (Middle.X < 3000.0 + 299.0 || Middle.X > 9000.0 - 299.0) ? 1 : 0;
			OnRoad += RoadDist(Middle) < 199.0 ? 1 : 0;
			Floating += FMath::Abs(Box.Min.Z) > 3.0 ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("free: %d props, %d in the 60 m gap"), Props.Num(), InGap));
		TestTrue(TEXT("free: props in the wide gap"), InGap >= 4);
		TestEqual(TEXT("free: within the clearance of a building"), OnBuildings, 0);
		TestEqual(TEXT("free: within the clearance of the road"), OnRoad, 0);
		TestEqual(TEXT("free: off the ground"), Floating, 0);

		// Buildings 2 m apart: the same band has nothing in it.
		FGroundTile Narrow = CSCityGroundTests_Tile();
		for (double X = 0.0; X < 12000.0; X += 3200.0) Narrow.Lots.Add(CSCityGroundTests_Lot(X, X + 3000.0, 7000.0, 9000.0));
		int32 Between = 0;
		for (const FBox& Box : CSCityGroundTests_Props(LayoutGround(Always, Narrow))) Between += Box.GetCenter().Y > 7000.0 && Box.GetCenter().Y < 9000.0 ? 1 : 0;
		TestEqual(TEXT("free: a 2 m gap has nothing"), Between, 0);
	}

	// The same place, the same random, whichever tile size it is written in.
	{
		FGroundKitShape Always = Kit;
		Always.AutoPlanterChance = 0.5;
		const FGroundLayout Whole = LayoutGround(Always, CSCityGroundTests_Tile());
		FGroundTile Half = CSCityGroundTests_Tile();
		Half.Size = 6000.0;
		const FGroundLayout Part = LayoutGround(Always, Half);
		int32 Found = 0;
		const TArray<FBox> WholeProps = CSCityGroundTests_Props(Whole);
		const TArray<FBox> PartProps = CSCityGroundTests_Props(Part);
		for (const FBox& Box : PartProps) for (const FBox& Other : WholeProps) Found += Box.GetCenter().Equals(Other.GetCenter(), 0.5) ? 1 : 0;
		TestEqual(TEXT("written in half a tile: the same props"), Found, PartProps.Num());
		TestTrue(TEXT("written in half a tile: some"), PartProps.Num() > 0);
	}
	return true;
}

// What the field says: planters, a night market's cart shops (their long side along the road), nothing; half of each with a vote at the seam; the
// ground raised with walls round it, and gone again when it is cleared.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityGroundFieldTest, "PCGPlugins.CSRoadNet.City.GroundField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityGroundFieldTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSCityFieldActor* Field = World->SpawnActor<ACSCityFieldActor>();
	if (!TestNotNull(TEXT("field"), Field)) return false;

	FGroundKitShape Kit = CSCityGroundTests_Kit();
	Kit.AutoPlanterChance = 0.0;
	Kit.PlanterChance = 1.0;
	Kit.CartChance = 1.0;
	const FGroundTile Tile = CSCityGroundTests_Tile();
	const FVector2D Min(-400.0, -400.0);
	const FVector2D Max(12400.0, 12400.0);

	const FGroundLayout Plain = LayoutGround(Kit, Tile, Field->GpuField());
	TestEqual(TEXT("nothing painted, no chance: nothing stands"), Plain.Planters() + Plain.Carts(), 0u);

	Field->SetGapStyleInBox(Min, Max, ECSCityGapStyle::Planter);
	const FGroundLayout Planters = LayoutGround(Kit, Tile, Field->GpuField());
	TestTrue(TEXT("planter style: planters"), Planters.Planters() > 0u);
	TestEqual(TEXT("planter style: no carts"), Planters.Carts(), 0u);
	TestTrue(TEXT("planter style: both kinds"), Planters.Counts[int32(EGroundSlot::Planter)] > 0u && Planters.Counts[int32(EGroundSlot::Planter2)] > 0u);
	TestEqual(TEXT("the slabs do not care"), Planters.Slabs(), Plain.Slabs());

	Field->SetGapStyleInBox(Min, Max, ECSCityGapStyle::NightMarket);
	const FGroundLayout Market = LayoutGround(Kit, Tile, Field->GpuField());
	TestTrue(TEXT("market style: carts"), Market.Carts() > 0u);
	TestEqual(TEXT("market style: no planters"), Market.Planters(), 0u);
	TestTrue(TEXT("market style: both kinds"), Market.Counts[int32(EGroundSlot::CartShop)] > 0u && Market.Counts[int32(EGroundSlot::CartShop2)] > 0u);
	int32 Across = 0;
	for (const EGroundSlot Slot : { EGroundSlot::CartShop, EGroundSlot::CartShop2 }) for (const FBox& Box : Market.Boxes[int32(Slot)]) Across += Box.GetSize().X < Box.GetSize().Y ? 1 : 0;
	TestEqual(TEXT("market style: carts across a straight road"), Across, 0);
	TestEqual(TEXT("as many places whichever it is"), Market.Carts(), Planters.Planters());

	Field->SetGapStyleInBox(Min, Max, ECSCityGapStyle::Empty);
	const FGroundLayout Clear = LayoutGround(Kit, Tile, Field->GpuField());
	TestEqual(TEXT("empty style: nothing stands"), Clear.Planters() + Clear.Carts(), 0u);

	// Half and half along X: each on its own side, within the four cells a point votes among.
	Field->SetGapStyleInBox(Min, FVector2D(6000.0, Max.Y), ECSCityGapStyle::NightMarket);
	Field->SetGapStyleInBox(FVector2D(6000.0, Min.Y), Max, ECSCityGapStyle::Planter);
	const FGroundLayout Both = LayoutGround(Kit, Tile, Field->GpuField());
	TestTrue(TEXT("half and half: some of each"), Both.Carts() > 0u && Both.Planters() > 0u);
	int32 Wrong = 0;
	for (const EGroundSlot Slot : { EGroundSlot::CartShop, EGroundSlot::CartShop2 }) for (const FBox& Box : Both.Boxes[int32(Slot)]) Wrong += Box.GetCenter().X > 6000.0 + 400.0 ? 1 : 0;
	for (const EGroundSlot Slot : { EGroundSlot::Planter, EGroundSlot::Planter2 }) for (const FBox& Box : Both.Boxes[int32(Slot)]) Wrong += Box.GetCenter().X < 6000.0 - 400.0 ? 1 : 0;
	TestEqual(TEXT("half and half: on the wrong side"), Wrong, 0);
	Field->ClearField();

	// The ground raised 3 m, well away from the road: at that height in the middle, walls round it. Down to nothing when cleared.
	const FVector2D RaisedMin(3200.0, 8400.0);
	const FVector2D RaisedMax(8800.0, 11600.0);
	FGroundKitShape NoProps = Kit;
	NoProps.PlanterChance = 0.0;
	const FGroundLayout Before = LayoutGround(NoProps, Tile, Field->GpuField());
	TestEqual(TEXT("not raised: no walls"), Before.Walls(), 0u);
	Field->SetElevationInBox(RaisedMin, RaisedMax, 3.0f);
	TestEqual(TEXT("raised: the field says so"), Field->ElevationAt(FVector2D(6000.0, 10000.0)), 3.0f);
	const FGroundLayout Raised = LayoutGround(NoProps, Tile, Field->GpuField());
	const TOptional<double> Middle = CSCityGroundTests_Top(Raised, FVector2D(6000.0, 10000.0));
	if (TestTrue(TEXT("raised: ground"), Middle.IsSet())) TestTrue(FString::Printf(TEXT("raised: %.1f in the middle"), Middle.GetValue()), FMath::IsNearlyEqual(Middle.GetValue(), 300.0, 5.0));
	TestTrue(TEXT("raised: walls"), Raised.Walls() > 0u);
	const TOptional<double> ByRoad = CSCityGroundTests_Top(Raised, FVector2D(6000.0, CSCityGroundTests_RoadY + CSCityGroundTests_HalfWidth + 60.0));
	if (TestTrue(TEXT("raised: ground by the road"), ByRoad.IsSet())) TestTrue(FString::Printf(TEXT("raised: by the road still its level, %.1f"), ByRoad.GetValue()), FMath::Abs(ByRoad.GetValue()) < 3.0);
	Field->ClearField();
	TestEqual(TEXT("cleared: no walls"), LayoutGround(NoProps, Tile, Field->GpuField()).Walls(), 0u);
	TestEqual(TEXT("cleared: no field"), Field->GpuBytes(), 0);
	return true;
}

// The brush's side of the field: a dab paints the channel chosen within its radius and draws it on the map at once; each channel comes back as painted;
// painting Auto takes it off.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFieldBrushTest, "PCGPlugins.CSRoadNet.City.FieldBrush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFieldBrushTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSCityFieldActor* Field = World->SpawnActor<ACSCityFieldActor>();
	if (!TestNotNull(TEXT("field"), Field)) return false;
	TestEqual(TEXT("nothing painted: an empty map"), Field->MapTiles(), 0);

	const FVector2D At(3200.0, 3200.0);
	Field->BrushRadius = 12.0f;
	Field->BrushChannel = ECSCityFieldChannel::GapStyle;
	Field->BrushGap = ECSCityGapStyle::NightMarket;
	Field->PaintDab(At);
	TestEqual(TEXT("dab: the middle"), Field->GapStyleAt(At), ECSCityGapStyle::NightMarket);
	TestEqual(TEXT("dab: within the radius"), Field->GapStyleAt(At + FVector2D(1000.0, 0.0)), ECSCityGapStyle::NightMarket);
	TestEqual(TEXT("dab: out of it"), Field->GapStyleAt(At + FVector2D(1600.0, 0.0)), ECSCityGapStyle::Auto);
	TestEqual(TEXT("dab: round, not its corner"), Field->GapStyleAt(At + FVector2D(1100.0, 1100.0)), ECSCityGapStyle::Auto);
	const int32 Tiles = Field->MapTiles();
	TestTrue(TEXT("dab: on the map at once"), Tiles > 0);
	Field->EndPaintStroke();
	TestTrue(TEXT("dab: on the GPU"), Field->GpuBytes() > 0);

	// A dab of another channel leaves the first alone, and its own map shows it.
	Field->BrushChannel = ECSCityFieldChannel::CornerStyle;
	Field->BrushCorner = ECSCityCornerStyle::Round;
	TestEqual(TEXT("corner channel: nothing on its map yet"), Field->MapTiles(), 0);
	Field->PaintDab(At);
	TestEqual(TEXT("corner channel: painted"), Field->CornerStyleAt(At), ECSCityCornerStyle::Round);
	TestEqual(TEXT("corner channel: the gap style left alone"), Field->GapStyleAt(At), ECSCityGapStyle::NightMarket);
	TestTrue(TEXT("corner channel: on its map"), Field->MapTiles() > 0);
	Field->BrushChannel = ECSCityFieldChannel::Overhang;
	Field->BrushOverhang = ECSCityOverhangField::Never;
	Field->PaintDab(At);
	TestEqual(TEXT("overhang channel: painted"), Field->OverhangAt(At), ECSCityOverhangField::Never);
	Field->BrushChannel = ECSCityFieldChannel::Elevation;
	Field->BrushElevation = -2.5f;
	Field->PaintDab(At);
	TestEqual(TEXT("elevation channel: painted"), Field->ElevationAt(At), -2.5f);
	TestEqual(TEXT("elevation channel: goes as low as it says"), Field->MaxElevation(), 250.0);
	Field->EndPaintStroke();

	// Painting the off value takes it off, and an empty page goes.
	Field->BrushElevation = 0.0f;
	Field->PaintDab(At);
	Field->BrushChannel = ECSCityFieldChannel::Overhang;
	Field->BrushOverhang = ECSCityOverhangField::Unset;
	Field->PaintDab(At);
	Field->BrushChannel = ECSCityFieldChannel::CornerStyle;
	Field->BrushCorner = ECSCityCornerStyle::Auto;
	Field->PaintDab(At);
	Field->BrushChannel = ECSCityFieldChannel::GapStyle;
	Field->BrushGap = ECSCityGapStyle::Auto;
	Field->PaintDab(At);
	Field->EndPaintStroke();
	TestEqual(TEXT("off again: a clear map"), Field->MapTiles(), 0);
	TestEqual(TEXT("off again: no pages"), Field->NumPages, 0);
	return true;
}

// On a network: ground tiles over a road's surroundings, drawn; an unchanged rebuild writes none; painting writes only those it covers; vacant plots leave
// fewer buildings; off, it goes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityGroundNetworkTest, "PCGPlugins.CSRoadNet.City.GroundNetwork",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityGroundNetworkTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UCSCityGroundKit* Kit = UCSCityGroundKit::Default();
	if (!Kit)
	{
		AddWarning(TEXT("no ground kit: run Scripts/NightCity/MakeNCSimpleModules.py (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network"), Network)) return false;
	Network->bLocalUpdate = true;
	Network->TileSize = 64.0f;
	Network->bBatchBuildings = true;
	Network->bBuildGround = true;
	Network->MaxTilesPerBatch = 0;
	Network->MinFreeVideoMemory = 0;
	Network->Lots.Seed = 3;

	ACSRoadSplineActor* Road = World->SpawnActor<ACSRoadSplineActor>();
	if (!TestNotNull(TEXT("road"), Road)) return false;
	Road->AttachToActor(Network, FAttachmentTransformRules::KeepWorldTransform);
	Road->Spline->Prefab = TEXT("Medium Road");
	Road->Spline->SetSplinePoints({ FVector(0.0, 0.0, 0.0), FVector(12000.0, 0.0, 600.0) }, ESplineCoordinateSpace::World);
	Road->Spline->SetSplinePointType(0, ESplinePointType::Linear, false);
	Road->Spline->SetSplinePointType(1, ESplinePointType::Linear);

	auto DrawnSlabs = [&]()
	{
		CSRoad::LandMeshWrites();
		int32 Count = 0;
		for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network)) if (Component->GetName().StartsWith(TEXT("GroundTile")) && Component->HasInstanceSourceGPU() && Component->BaseMesh == Kit->Slab.Mesh) Count += Component->DebugReadDrawnInstanceCountSync();
		return Count;
	};

	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("built: error"), Network->LastError, FString());
	const int32 Tiles = Network->NumGroundTiles;
	TestTrue(TEXT("built: ground tiles"), Tiles >= 4);
	TestEqual(TEXT("built: every one written"), Network->GroundTilesRebuilt, Tiles);
	const int32 Slabs = DrawnSlabs();
	AddInfo(FString::Printf(TEXT("%d ground tiles, %d slabs, %d buildings"), Tiles, Slabs, Network->NumBuildings));
	TestTrue(TEXT("built: slabs drawn"), Slabs > 100);
	const int32 Everyone = Network->NumBuildings;
	TestTrue(TEXT("built: buildings"), Everyone > 0);

	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("again: nothing written"), Network->GroundTilesRebuilt, 0);

	// Painted over part of it, and the ground over that written again, not all of it.
	ACSCityFieldActor* Field = World->SpawnActor<ACSCityFieldActor>();
	if (!TestNotNull(TEXT("field"), Field)) return false;
	Network->Rebuild();
	Network->FinishWrites();
	Field->SetGapStyleInBox(FVector2D(1000.0, 1500.0), FVector2D(1800.0, 2300.0), ECSCityGapStyle::NightMarket);
	Network->Rebuild();
	Network->FinishWrites();
	const int32 Painted = Network->GroundTilesRebuilt;
	AddInfo(FString::Printf(TEXT("painted a 8 m square: %d of %d ground tiles written"), Painted, Tiles));
	TestTrue(TEXT("painted: written again"), Painted > 0);
	TestTrue(TEXT("painted: only those near it"), Painted < Tiles);

	// Plots left empty.
	Network->Lots.Vacancy = 0.5f;
	Network->Rebuild();
	Network->FinishWrites();
	TestTrue(FString::Printf(TEXT("vacant: fewer buildings, %d of %d"), Network->NumBuildings, Everyone), Network->NumBuildings < Everyone && Network->NumBuildings > 0);

	Network->bBuildGround = false;
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("off: no ground tiles"), Network->NumGroundTiles, 0);
	CSRoad::LandMeshWrites();
	int32 Left = 0;
	for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network)) Left += Component->GetName().StartsWith(TEXT("GroundTile")) ? 1 : 0;
	TestEqual(TEXT("off: no components"), Left, 0);
	return true;
}

#endif

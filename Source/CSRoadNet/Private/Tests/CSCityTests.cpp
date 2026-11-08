#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityAlleyRegion.h"
#include "CSCityBuildingActor.h"
#include "CSCityFacade.h"
#include "CSCityFacadeKit.h"
#include "CSCityFieldActor.h"
#include "CSCityStreetActor.h"
#include "CSCityLayout.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedNaniteComponent.h"
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadMeshWrite.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTestUtils.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace
{
double CSCityTests_Distance(const CSCity::FPolygon& Polygon, const FVector2D& P)
{
	if (CSCity::Contains(Polygon, P)) return 0.0;
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Polygon.Num(); I++) Best = FMath::Min(Best, FVector2D::Distance(P, FMath::ClosestPointOnSegment2D(P, Polygon[I], Polygon[(I + 1) % Polygon.Num()])));
	return Best;
}

CSCity::FPolygon CSCityTests_Box(const FVector2D& Centre, const FVector2D& Extent, double Degrees = 0.0)
{
	TArray<FVector2D> Corners;
	for (const FVector2D& Sign : { FVector2D(-1.0, -1.0), FVector2D(1.0, -1.0), FVector2D(1.0, 1.0), FVector2D(-1.0, 1.0) }) Corners.Add(Centre + (Extent * Sign).GetRotated(Degrees));
	return CSCity::ConvexHull(Corners);
}

// A straight street (cm), sampled every 8 m.
CSCity::FRoadLine CSCityTests_Street(uint64 Id, const FVector& A, const FVector& B, double HalfWidth)
{
	CSCity::FRoadLine Line;
	Line.Id = Id;
	Line.HalfWidth = HalfWidth;
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector::Distance(A, B) / 800.0));
	for (int32 S = 0; S <= Steps; S++) Line.Points.Add(FMath::Lerp(A, B, double(S) / Steps));
	return Line;
}

bool CSCityTests_SameLots(const TArray<CSCity::FLot>& A, const TArray<CSCity::FLot>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 I = 0; I < A.Num(); I++) if (A[I].Id != B[I].Id || A[I].Corners != B[I].Corners || A[I].Base != B[I].Base || A[I].Height != B[I].Height) return false;
	return true;
}

// Each building tile (its components' name without the number), with what changes when its instances are written: a
// component made anew, or its instances packed again.
TMap<FString, uint32> CSCityTests_BuildingSerials(ACSRoadNetworkActor* Network)
{
	// Writes still on their way count once landed.
	CSRoad::LandMeshWrites();
	TMap<FString, uint32> Out;
	for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network))
	{
		if (!Component->GetName().StartsWith(TEXT("BuildingTile"))) continue;
		uint32& Serial = Out.FindOrAdd(Component->GetFName().GetPlainNameString());
		Serial = HashCombine(Serial, HashCombine(GetTypeHash(Component), uint32(Component->GetInstancePackSerial())));
	}
	return Out;
}

// GPU meshes drawn for buildings: none, the buildings are instances only.
int32 CSCityTests_BuildingMeshes(ACSRoadNetworkActor* Network)
{
	int32 Count = 0;
	for (const UCSMeshRenderComponent* Component : CSRoadTest::Built<UCSMeshRenderComponent>(Network)) Count += Component->GetName().StartsWith(TEXT("BuildingTile")) ? 1 : 0;
	// The classic path's copy of the base mesh (a mesh without Nanite): not on the Nanite path.
	for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network)) Count += Component->GetName().StartsWith(TEXT("BuildingTile")) && Component->GetGpuMesh() && Component->GetGpuMesh()->GetResidentPtr() && Component->GetGpuMesh()->GetResidentPtr()->IsAllocated() ? 1 : 0;
	return Count;
}

// The engine's cube with Nanite on, built here and now; a copy per name, so that each is a mesh of its own. Make
// it after the test's CreateNewMap: the map's garbage collection takes it otherwise.
UStaticMesh* CSCityTests_NaniteCube(const TCHAR* Name)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!Cube) return nullptr;
	UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), Name));
	FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
	Settings.bEnabled = true;
	Mesh->SetNaniteSettings(Settings);
	// Given OutErrors the build does not go async: the render data is there when this returns.
	TArray<FText> Errors;
	Mesh->Build(/*bInSilent*/ true, &Errors);
	return Mesh;
}

// The instances drawn in place of the network's buildings: their component per instance, in world space.
struct FCSCityTests_Instance
{
	UCSGpuInstancedMeshComponent* Component = nullptr;
	FTransform Transform;
};

TArray<FCSCityTests_Instance> CSCityTests_Instances(ACSRoadNetworkActor* Network)
{
	TArray<FCSCityTests_Instance> Out;
	for (UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network))
	{
		for (int32 I = 0; I < Component->GetInstanceCount(); I++)
		{
			FCSCityTests_Instance& Instance = Out.AddDefaulted_GetRef();
			Instance.Component = Component;
			Component->GetInstanceTransform(I, Instance.Transform, /*bWorldSpace*/ true);
		}
	}
	return Out;
}

// The eight corners of Bounds under Transform.
TArray<FVector> CSCityTests_Corners(const FBox& Bounds, const FTransform& Transform)
{
	TArray<FVector> Corners;
	for (const FVector& Point : { Bounds.Min, FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Min.Z), FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Min.Z), FVector(Bounds.Max.X, Bounds.Max.Y, Bounds.Min.Z),
		FVector(Bounds.Min.X, Bounds.Min.Y, Bounds.Max.Z), FVector(Bounds.Max.X, Bounds.Min.Y, Bounds.Max.Z), FVector(Bounds.Min.X, Bounds.Max.Y, Bounds.Max.Z), Bounds.Max })
	{
		Corners.Add(Transform.TransformPosition(Point));
	}
	return Corners;
}
}

// A mesh on a plot: its box stays inside the plot and stands on its base; its front faces the plot's road and lies on
// the near edge; it shrinks to fit, as a whole, and never grows; a pivot off the box's middle, as on the
// real houses, and any front direction change none of that. Which mesh a plot takes depends on its id and the seed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLotInstanceTest, "PCGPlugins.CSRoadNet.City.LotInstance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityLotInstanceTest::RunTest(const FString& Parameters)
{
	CSCity::FLotSettings Settings;
	const TArray<CSCity::FRoadLine> Streets = { CSCityTests_Street(1, FVector(0.0, 0.0, 300.0), FVector(16000.0, 0.0, 300.0), 400.0), CSCityTests_Street(2, FVector(30000.0, 0.0, 300.0), FVector(42000.0, 9000.0, 300.0), 800.0) };
	const TArray<CSCity::FLot> Lots = CSCity::LayoutLots(Streets, Streets, {}, Settings);
	if (!TestTrue(TEXT("plots laid out"), Lots.Num() >= 8)) return false;

	struct FCase
	{
		const TCHAR* Name;
		FBox Bounds;
		double FrontYaw;
		bool bShrinks;
	};
	const FCase Cases[] =
	{
		{ TEXT("fits, front +Y"), FBox(FVector(-600.0, -400.0, 0.0), FVector(600.0, 400.0, 1500.0)), 90.0, false },
		{ TEXT("fits, front -X"), FBox(FVector(-400.0, -600.0, 0.0), FVector(400.0, 600.0, 1500.0)), 180.0, false },
		{ TEXT("pivot off the middle, below the bottom"), FBox(FVector(-12.0, -1200.0, 34.0), FVector(1188.0, -400.0, 1534.0)), 90.0, false },
		{ TEXT("too wide"), FBox(FVector(-2000.0, -500.0, 0.0), FVector(2000.0, 500.0, 2000.0)), 90.0, true },
		{ TEXT("too deep"), FBox(FVector(-500.0, -4000.0, 0.0), FVector(500.0, 4000.0, 2000.0)), 90.0, true },
		{ TEXT("front turned 30 degrees"), FBox(FVector(-300.0, -300.0, 0.0), FVector(300.0, 300.0, 900.0)), 30.0, false },
	};
	for (const FCase& Case : Cases)
	{
		int32 Outside = 0;
		int32 BaseOff = 0;
		int32 FrontOff = 0;
		int32 NearOff = 0;
		int32 SizeOff = 0;
		for (const CSCity::FLot& Lot : Lots)
		{
			const FTransform Transform = CSCity::LotInstance(Lot, Case.Bounds, Case.FrontYaw);
			const FVector Scale = Transform.GetScale3D();
			const CSCity::FPolygon Kept = CSCity::Grow(Lot.Corners, 1.0);
			double Bottom = TNumericLimits<double>::Max();
			double Nearest = TNumericLimits<double>::Lowest();
			for (const FVector& Corner : CSCityTests_Corners(Case.Bounds, Transform))
			{
				Outside += CSCity::Contains(Kept, FVector2D(Corner)) ? 0 : 1;
				Bottom = FMath::Min(Bottom, Corner.Z);
				Nearest = FMath::Max(Nearest, (FVector2D(Corner) - Lot.Centre).Dot(Lot.Front));
			}
			BaseOff += FMath::IsNearlyEqual(Bottom, Lot.Base, 0.01) ? 0 : 1;
			NearOff += FMath::IsNearlyEqual(Nearest, Lot.Depth * 0.5, 0.01) ? 0 : 1;
			const FVector Front = Transform.TransformVectorNoScale(FVector(FVector2D(1.0, 0.0).GetRotated(Case.FrontYaw), 0.0));
			FrontOff += FVector2D(Front).Equals(Lot.Front, 1e-6) ? 0 : 1;
			const bool bAsOne = FMath::IsNearlyEqual(Scale.X, Scale.Y, 1e-9) && FMath::IsNearlyEqual(Scale.X, Scale.Z, 1e-9);
			SizeOff += bAsOne && Scale.X <= 1.0 && (Case.bShrinks ? Scale.X < 1.0 : FMath::IsNearlyEqual(Scale.X, 1.0, 1e-9)) ? 0 : 1;
		}
		TestEqual(FString::Printf(TEXT("%s: corners outside the plot"), Case.Name), Outside, 0);
		TestEqual(FString::Printf(TEXT("%s: not standing on the base"), Case.Name), BaseOff, 0);
		TestEqual(FString::Printf(TEXT("%s: front not on the near edge"), Case.Name), NearOff, 0);
		TestEqual(FString::Printf(TEXT("%s: not facing the road"), Case.Name), FrontOff, 0);
		TestEqual(FString::Printf(TEXT("%s: wrong size"), Case.Name), SizeOff, 0);
	}

	// The near edge is the side towards the plot's own road.
	for (const CSCity::FLot& Lot : Lots)
	{
		const double ToNear = CSCityTests_Distance(Lot.Corners, Lot.Centre + Lot.Front * (Lot.Depth * 0.5));
		TestTrue(TEXT("the near edge's middle lies on the plot"), ToNear < 0.01);
	}

	TSet<int32> Used;
	for (uint64 Id = 1; Id <= 200; Id++)
	{
		CSCity::FLot Lot;
		Lot.Id = Id * 0x9E3779B97F4A7C15ull;
		const int32 Mesh = CSCity::LotMesh(Lot, 1, 6);
		Used.Add(Mesh);
		TestEqual(TEXT("the same plot takes the same mesh"), CSCity::LotMesh(Lot, 1, 6), Mesh);
		TestTrue(TEXT("a mesh of those there are"), Mesh >= 0 && Mesh < 6);
	}
	TestEqual(TEXT("every mesh is taken"), Used.Num(), 6);
	TestEqual(TEXT("no meshes, none taken"), CSCity::LotMesh(CSCity::FLot(), 1, 0), int32(INDEX_NONE));
	return true;
}

namespace
{
// A tier's footprint counted as the layout should see it: cells, outline edges (one per cell side facing out), outer
// corners (a diagonal touch counts two) and inner corners.
struct FCSCityTestsOutline
{
	int32 Cells = 0;
	int32 Edges = 0;
	int32 Outer = 0;
	int32 Inner = 0;
};

FCSCityTestsOutline CSCityTests_Outline(const CSCity::FFacadeMassing& M, int32 Tier)
{
	FCSCityTestsOutline Out;
	for (int32 J = 0; J < M.GridY; J++)
	{
		for (int32 I = 0; I < M.GridX; I++)
		{
			if (!M.At(Tier, I, J)) continue;
			Out.Cells++;
			Out.Edges += int32(!M.At(Tier, I + 1, J)) + int32(!M.At(Tier, I - 1, J)) + int32(!M.At(Tier, I, J + 1)) + int32(!M.At(Tier, I, J - 1));
		}
	}
	for (int32 VY = 0; VY <= M.GridY; VY++)
	{
		for (int32 VX = 0; VX <= M.GridX; VX++)
		{
			const bool A = M.At(Tier, VX - 1, VY - 1);
			const bool B = M.At(Tier, VX, VY - 1);
			const bool C = M.At(Tier, VX - 1, VY);
			const bool D = M.At(Tier, VX, VY);
			const int32 N = int32(A) + int32(B) + int32(C) + int32(D);
			if (N == 1) Out.Outer++;
			else if (N == 2 && A == D) Out.Outer += 2;
			else if (N == 3) Out.Inner++;
		}
	}
	return Out;
}

CSCity::FFacadeKitShape CSCityTests_Kit()
{
	CSCity::FFacadeKitShape Kit;
	// The plain box's slots; round corners and overhangs have tests of their own.
	for (int32 Slot = 0; Slot <= int32(CSCity::EFacadeSlot::GroundWindow); Slot++) Kit.bHas[Slot] = true;
	Kit.Bay = 300.0;
	Kit.Storey = 400.0;
	Kit.Parapet = 110.0;
	using CSCity::EFacadeSlot;
	for (const EFacadeSlot Slot : { EFacadeSlot::Wall, EFacadeSlot::Window, EFacadeSlot::Door, EFacadeSlot::GroundWall, EFacadeSlot::GroundWindow }) Kit.Bounds[int32(Slot)] = FBox(FVector(0.0, -50.0, 0.0), FVector(300.0, 0.0, 400.0));
	Kit.Bounds[int32(EFacadeSlot::Parapet)] = FBox(FVector(0.0, -40.0, 0.0), FVector(300.0, 0.0, 110.0));
	Kit.Bounds[int32(EFacadeSlot::Roof)] = FBox(FVector(0.0, 0.0, -20.0), FVector(300.0, 300.0, 0.0));
	Kit.Bounds[int32(EFacadeSlot::CornerInner)] = FBox(FVector(0.0, 0.0, 0.0), FVector(100.0, 100.0, 400.0));
	Kit.Corner = 40.0;
	Kit.CornerDepth = 40.0;
	Kit.Bounds[int32(EFacadeSlot::Corner)] = FBox(FVector(0.0, -40.0, 0.0), FVector(40.0, 0.0, 400.0));
	return Kit;
}

// The kit's corner: its piece's size and the numbers that go with it.
void CSCityTests_SetCorner(CSCity::FFacadeKitShape& Kit, double Size)
{
	Kit.Corner = Size;
	Kit.CornerDepth = Size;
	Kit.Bounds[int32(CSCity::EFacadeSlot::Corner)] = FBox(FVector(0.0, -Size, 0.0), FVector(Size, 0.0, 400.0));
}

// Every wall, window and door cell's box.
TArray<FBox> CSCityTests_Cells(const CSCity::FFacadeLayout& Layout)
{
	using CSCity::EFacadeSlot;
	TArray<FBox> Out;
	for (const EFacadeSlot Slot : { EFacadeSlot::Wall, EFacadeSlot::Window, EFacadeSlot::Door, EFacadeSlot::GroundWall, EFacadeSlot::GroundWindow }) Out.Append(Layout.Boxes[int32(Slot)]);
	return Out;
}

// Those on the front (outside on Y = Depth / 2, their thickness along Y) and on the +X side.
TArray<FBox> CSCityTests_OnFront(const TArray<FBox>& Boxes, double HalfDepth)
{
	return Boxes.FilterByPredicate([HalfDepth](const FBox& Box) { return FMath::IsNearlyEqual(Box.Max.Y, HalfDepth, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 50.0, 0.1); });
}

TArray<FBox> CSCityTests_OnRight(const TArray<FBox>& Boxes, double HalfWidth)
{
	return Boxes.FilterByPredicate([HalfWidth](const FBox& Box) { return FMath::IsNearlyEqual(Box.Max.X, HalfWidth, 0.1) && FMath::IsNearlyEqual(Box.GetSize().X, 50.0, 0.1); });
}
}

// A box building's cells from a facade kit: whole storeys nearest its height (one at least), a grid of bays nearest the
// bay width, the end bays giving the corner pieces their width, the door in the front bay asked for, the corners a storey
// at a time and once more for the parapet, a parapet bay over each top bay and a roof tile over each bay; a slot the kit
// leaves empty takes none of it. A ground storey of its own height lifts the storeys above. The smallest box around a hull
// holds it and is the hull for a rectangle.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadeLayoutTest, "PCGPlugins.CSRoadNet.City.FacadeLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFacadeLayoutTest::RunTest(const FString& Parameters)
{
	const CSCity::FFacadeKitShape Kit = CSCityTests_Kit();
	CSCity::FFacadeBuilding Building;
	Building.Block.Width = 1240.0;
	Building.Block.Depth = 680.0;
	Building.Block.Height = 1300.0;
	Building.Shape = ECSCityMassShape::Box;
	Building.Stack = ECSCityMassStack::None;
	const CSCity::FFacadeLayout Full = CSCity::LayoutFacade(Kit, Building);
	TestEqual(TEXT("storeys: nearest the height"), Full.Massing.Storeys, 3);
	TestEqual(TEXT("front bays: nearest 3 m"), Full.Massing.GridX, 4);
	TestEqual(TEXT("side bays"), Full.Massing.GridY, 2);
	TestTrue(TEXT("a box, one tier"), Full.Massing.Shape == ECSCityMassShape::Box && Full.Massing.Stack == ECSCityMassStack::None && Full.Massing.Tiers.Num() == 1);
	TestEqual(TEXT("facade cells"), Full.FacadeCells, 3u * 12u);
	TestEqual(TEXT("ground cells"), Full.GroundCells, 12u);
	TestEqual(TEXT("corners: a storey at a time and the parapet's"), Full.Corners, 4u * 4u);
	TestEqual(TEXT("no inner corners"), Full.InnerCorners, 0u);
	TestEqual(TEXT("parapet bays"), Full.Parapets, 12u);
	TestEqual(TEXT("roof tiles"), Full.Roofs, 8u);
	TestEqual(TEXT("door: the front's middle bay"), Full.DoorCell, 2);
	TestEqual(TEXT("modules"), Full.Modules(), 36u + 16u + 12u + 8u);
	// The end bays give the corner piece its width; the others are a bay each.
	const TArray<FBox> FullCells = CSCityTests_Cells(Full);
	const TArray<FBox> FullFront = CSCityTests_OnFront(FullCells, 340.0);
	TestEqual(TEXT("front: the bays of 3 storeys"), FullFront.Num(), 12);
	int32 Trimmed = 0;
	for (const FBox& Box : FullFront)
	{
		const double Length = Box.GetSize().X;
		TestTrue(TEXT("front: a bay, or an end bay short of the corner"), FMath::IsNearlyEqual(Length, 310.0, 0.1) || FMath::IsNearlyEqual(Length, 270.0, 0.1));
		Trimmed += FMath::IsNearlyEqual(Length, 270.0, 0.1) ? 1 : 0;
		TestTrue(TEXT("front: inside the corners"), Box.Min.X >= -620.0 + 40.0 - 0.1 && Box.Max.X <= 620.0 - 40.0 + 0.1);
	}
	TestEqual(TEXT("front: two end bays a storey"), Trimmed, 6);
	for (const FBox& Box : CSCityTests_OnRight(FullCells, 620.0)) TestTrue(TEXT("side: its end bays short of the corners"), FMath::IsNearlyEqual(Box.GetSize().Y, 300.0, 0.1));

	CSCity::FFacadeBuilding NoDoor = Building;
	NoDoor.DoorAt = -1.0;
	TestTrue(TEXT("no door asked for: none"), CSCity::LayoutFacade(Kit, NoDoor).DoorCell == INDEX_NONE && CSCity::LayoutFacade(Kit, NoDoor).Doors == 0u);
	NoDoor.DoorAt = 0.0;
	TestEqual(TEXT("door at the start: the first bay"), CSCity::LayoutFacade(Kit, NoDoor).DoorCell, 0);

	CSCity::FFacadeKitShape Bare = Kit;
	Bare.bHas[int32(CSCity::EFacadeSlot::Corner)] = false;
	Bare.bHas[int32(CSCity::EFacadeSlot::Parapet)] = false;
	Bare.Corner = 0.0;
	Bare.Parapet = 0.0;
	const CSCity::FFacadeLayout NoCorners = CSCity::LayoutFacade(Bare, Building);
	TestTrue(TEXT("no corners: the bays run corner to corner"), NoCorners.FacadeCells == 36u && NoCorners.Corners == 0u && NoCorners.Parapets == 0u);
	for (const FBox& Box : CSCityTests_OnFront(CSCityTests_Cells(NoCorners), 340.0)) TestTrue(TEXT("no corners: nothing trimmed"), FMath::IsNearlyEqual(Box.GetSize().X, 310.0, 0.1));
	CSCity::FFacadeKitShape NoParapet = Kit;
	NoParapet.bHas[int32(CSCity::EFacadeSlot::Parapet)] = false;
	TestEqual(TEXT("no parapet: corners a storey at a time only"), CSCity::LayoutFacade(NoParapet, Building).Corners, 12u);

	CSCity::FFacadeBuilding Low = Building;
	Low.Block.Height = 100.0;
	TestEqual(TEXT("lower than a storey: one"), CSCity::LayoutFacade(Kit, Low).Massing.Storeys, 1);
	CSCity::FFacadeBuilding Narrow = Building;
	Narrow.Block.Depth = 60.0;
	TestEqual(TEXT("a side narrower than its corners: only the front and back have cells"), CSCity::LayoutFacade(Kit, Narrow).FacadeCells, 3u * 8u);
	TestEqual(TEXT("an empty box: nothing"), CSCity::LayoutFacade(Kit, CSCity::FFacadeBuilding()).Modules(), 0u);

	CSCity::FFacadeKitShape Tall = Kit;
	Tall.Ground = 450.0;
	TestTrue(TEXT("ground storey 450: the storeys above start on it"), Tall.StoreyBase(0) == 0.0 && Tall.StoreyBase(1) == 450.0 && Tall.StoreyBase(3) == 1250.0);
	TestTrue(TEXT("no ground height: the storey's"), Kit.GroundHeight() == 400.0 && Kit.StoreyBase(2) == 800.0);

	// The smallest box around a turned rectangle is the rectangle; around a triangle it holds every corner.
	const CSCity::FPolygon Rectangle = CSCityTests_Box(FVector2D(500.0, -300.0), FVector2D(1000.0, 700.0), 20.0);
	const CSCity::FBlock Box = CSCity::HullBlock(Rectangle, 50.0, 900.0);
	TestTrue(TEXT("rectangle: its size"), FMath::IsNearlyEqual(FMath::Min(Box.Width, Box.Depth), 1400.0, 0.01) && FMath::IsNearlyEqual(FMath::Max(Box.Width, Box.Depth), 2000.0, 0.01));
	TestTrue(TEXT("rectangle: its middle"), Box.Base.Equals(FVector(500.0, -300.0, 50.0), 0.01));
	const CSCity::FPolygon Triangle = CSCity::ConvexHull({ FVector2D(0.0, 0.0), FVector2D(3000.0, 200.0), FVector2D(800.0, 1900.0) });
	const CSCity::FBlock Around = CSCity::HullBlock(Triangle, 0.0, 600.0);
	const FVector2D Front = Around.Front.GetSafeNormal();
	const FVector2D Along(Front.Y, -Front.X);
	int32 Out = 0;
	for (const FVector2D& Corner : Triangle)
	{
		const FVector2D Local = Corner - FVector2D(Around.Base);
		Out += FMath::Abs(Local.Dot(Along)) <= Around.Width * 0.5 + 0.01 && FMath::Abs(Local.Dot(Front)) <= Around.Depth * 0.5 + 0.01 ? 0 : 1;
	}
	TestEqual(TEXT("triangle: corners outside the box"), Out, 0);
	return true;
}

// Massing on the bay grid. Each footprint keeps the front row whole and has its own corners (an L one inner corner, a U
// or T two, a courtyard four round its hole) and the layout's cells follow the outline: facade cells a storey at a time
// on every outline edge, outer and inner corner pieces a storey at a time and once more for the parapet, a parapet on
// every edge and a roof tile on every cell. Setbacks and towers keep fewer cells higher up, every tier inside the one
// below, the storeys shared out without gaps; every cell is roofed once, at the top of the highest tier on it. A grid
// too small for a shape gets a box; Auto draws every shape the kit weighs, the same each time for one seed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadeMassingTest, "PCGPlugins.CSRoadNet.City.FacadeMassing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFacadeMassingTest::RunTest(const FString& Parameters)
{
	CSCity::FFacadeKitShape Kit = CSCityTests_Kit();
	CSCity::FFacadeBuilding Building;
	Building.Block.Width = 1800.0;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 2400.0;
	Building.Stack = ECSCityMassStack::None;
	struct FWant
	{
		ECSCityMassShape Shape;
		int32 Outer;
		int32 Inner;
	};
	const FWant Wants[] = { { ECSCityMassShape::Box, 4, 0 }, { ECSCityMassShape::L, 5, 1 }, { ECSCityMassShape::U, 6, 2 }, { ECSCityMassShape::T, 6, 2 }, { ECSCityMassShape::Courtyard, 4, 4 } };
	for (const FWant& Want : Wants)
	{
		const FString Name = StaticEnum<ECSCityMassShape>()->GetNameStringByValue(int64(Want.Shape));
		int32 Wrong = 0;
		for (uint32 Seed = 1; Seed <= 24; Seed++)
		{
			Building.Shape = Want.Shape;
			Building.Seed = Seed * 0x9E3779B1u;
			const CSCity::FFacadeLayout Layout = CSCity::LayoutFacade(Kit, Building);
			const CSCity::FFacadeMassing& M = Layout.Massing;
			if (M.Shape != Want.Shape || M.Tiers.Num() != 1 || M.GridX != 6 || M.GridY != 4 || M.Storeys != 6)
			{
				Wrong++;
				continue;
			}
			const FCSCityTestsOutline O = CSCityTests_Outline(M, 0);
			bool bFront = true;
			for (int32 I = 0; I < M.GridX; I++) bFront &= M.At(0, I, M.GridY - 1);
			const uint32 S = uint32(M.Storeys);
			const bool bRight = O.Outer == Want.Outer && O.Inner == Want.Inner && bFront && (Want.Shape == ECSCityMassShape::Box) == (O.Cells == 24)
				&& Layout.FacadeCells == uint32(O.Edges) * S && Layout.Corners == uint32(O.Outer) * (S + 1) && Layout.InnerCorners == uint32(O.Inner) * (S + 1)
				&& Layout.Parapets == uint32(O.Edges) && Layout.Roofs == uint32(O.Cells) && Layout.Doors == 1u && Layout.GroundCells == uint32(O.Edges);
			if (!bRight)
			{
				Wrong++;
				AddInfo(FString::Printf(TEXT("%s seed %u: cells %d edges %d outer %d inner %d front %d; layout facade %u corners %u inner %u parapets %u roofs %u"), *Name, Seed, O.Cells, O.Edges, O.Outer, O.Inner, int32(bFront), Layout.FacadeCells, Layout.Corners, Layout.InnerCorners, Layout.Parapets, Layout.Roofs));
			}
		}
		TestEqual(FString::Printf(TEXT("%s: footprints and cells as its outline says"), *Name), Wrong, 0);
	}

	// Setbacks and towers.
	Building.Block.Depth = 1500.0;
	Building.Block.Height = 6000.0;
	Building.Shape = ECSCityMassShape::Box;
	for (const ECSCityMassStack Stack : { ECSCityMassStack::Setback, ECSCityMassStack::PodiumTower })
	{
		const FString Name = StaticEnum<ECSCityMassStack>()->GetNameStringByValue(int64(Stack));
		int32 Wrong = 0;
		int32 ThreeTiers = 0;
		for (uint32 Seed = 1; Seed <= 24; Seed++)
		{
			Building.Stack = Stack;
			Building.Seed = Seed * 0x85EBCA6Bu;
			const CSCity::FFacadeLayout Layout = CSCity::LayoutFacade(Kit, Building);
			const CSCity::FFacadeMassing& M = Layout.Massing;
			bool bRight = M.Stack == Stack && M.Tiers.Num() >= 2 && M.Tiers[0].First == 0 && M.Tiers.Last().End == M.Storeys;
			uint32 Facade = 0;
			for (int32 T = 0; T < M.Tiers.Num() && bRight; T++)
			{
				const FCSCityTestsOutline O = CSCityTests_Outline(M, T);
				Facade += uint32(O.Edges) * uint32(M.Tiers[T].End - M.Tiers[T].First);
				bRight &= O.Cells > 0 && M.Tiers[T].End > M.Tiers[T].First;
				if (T == 0) continue;
				bRight &= M.Tiers[T].First == M.Tiers[T - 1].End && O.Cells < CSCityTests_Outline(M, T - 1).Cells;
				for (int32 J = 0; J < M.GridY; J++) for (int32 I = 0; I < M.GridX; I++) bRight &= !M.At(T, I, J) || M.At(T - 1, I, J);
				// A setback leaves the front row; a tower is a rectangle off the podium's edge.
				if (Stack == ECSCityMassStack::Setback) for (int32 I = 0; I < M.GridX; I++) bRight &= !M.At(T, I, M.GridY - 1);
				else bRight &= O.Outer == 4 && O.Inner == 0 && M.Tiers[0].End <= 3;
			}
			bRight &= Layout.FacadeCells == Facade && Layout.Roofs == uint32(CSCityTests_Outline(M, 0).Cells);
			ThreeTiers += M.Tiers.Num() == 3 ? 1 : 0;
			if (!bRight)
			{
				Wrong++;
				AddInfo(FString::Printf(TEXT("%s seed %u: %d tiers, stack %d, facade %u (want %u), roofs %u"), *Name, Seed, M.Tiers.Num(), int32(M.Stack), Layout.FacadeCells, Facade, Layout.Roofs));
			}
		}
		TestEqual(FString::Printf(TEXT("%s: tiers inside each other, storeys shared, every cell roofed once"), *Name), Wrong, 0);
		if (Stack == ECSCityMassStack::Setback) TestTrue(TEXT("Setback: tall ones sometimes step twice"), ThreeTiers > 0);
	}

	// Too small for the shape asked: a box.
	CSCity::FFacadeBuilding Small;
	Small.Block.Width = 300.0;
	Small.Block.Depth = 900.0;
	Small.Block.Height = 800.0;
	Small.Stack = ECSCityMassStack::None;
	for (const ECSCityMassShape Shape : { ECSCityMassShape::L, ECSCityMassShape::U, ECSCityMassShape::T, ECSCityMassShape::Courtyard })
	{
		Small.Shape = Shape;
		TestTrue(TEXT("one bay wide: a box"), CSCity::MassFacade(Kit, Small).Shape == ECSCityMassShape::Box);
	}
	Small.Block.Width = 600.0;
	Small.Block.Depth = 600.0;
	Small.Shape = ECSCityMassShape::Courtyard;
	TestTrue(TEXT("2 by 2: no room for a courtyard"), CSCity::MassFacade(Kit, Small).Shape == ECSCityMassShape::Box);
	Small.Shape = ECSCityMassShape::L;
	TestTrue(TEXT("2 by 2: an L fits"), CSCity::MassFacade(Kit, Small).Shape == ECSCityMassShape::L);

	// Auto: every weighed shape drawn somewhere, the same for the same seed.
	for (float& Weight : Kit.ShapeWeights) Weight = 1.0f;
	Kit.SetbackChance = 0.5f;
	Kit.PodiumChance = 0.3f;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 2800.0;
	Building.Shape = ECSCityMassShape::Auto;
	Building.Stack = ECSCityMassStack::Auto;
	TSet<int32> Shapes;
	TSet<int32> Stacks;
	int32 Changed = 0;
	for (uint32 Seed = 1; Seed <= 200; Seed++)
	{
		Building.Seed = Seed * 0xC2B2AE35u;
		const CSCity::FFacadeMassing A = CSCity::MassFacade(Kit, Building);
		const CSCity::FFacadeMassing B = CSCity::MassFacade(Kit, Building);
		Shapes.Add(int32(A.Shape));
		Stacks.Add(int32(A.Stack));
		bool bSame = A.Shape == B.Shape && A.Stack == B.Stack && A.Tiers.Num() == B.Tiers.Num();
		for (int32 T = 0; T < A.Tiers.Num() && bSame; T++) bSame = A.Tiers[T].Cells == B.Tiers[T].Cells && A.Tiers[T].First == B.Tiers[T].First;
		Changed += bSame ? 0 : 1;
	}
	TestEqual(TEXT("auto: every shape"), Shapes.Num(), 5);
	TestEqual(TEXT("auto: every stacking"), Stacks.Num(), 3);
	TestEqual(TEXT("auto: one seed, one massing"), Changed, 0);
	AddInfo(FString::Printf(TEXT("auto over 200 seeds: %d shapes, %d stackings"), Shapes.Num(), Stacks.Num()));
	return true;
}

// Outer corners that take whole bays (round or cut off): the wall gives up those bays, the piece is stretched to them along
// both sides, the roof tiles leave its square to the corner's roof piece, also where a whole side is corners (a drum) and
// on a narrow wall that two corners share. A piece narrower than the bays stays a pilaster, and with a roof piece the tiles
// fill the rest of its bays exactly.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadeRoundCornersTest, "PCGPlugins.CSRoadNet.City.FacadeRoundCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFacadeRoundCornersTest::RunTest(const FString& Parameters)
{
	using namespace CSCity;
	FFacadeKitShape Kit = CSCityTests_Kit();
	Kit.bHas[int32(EFacadeSlot::RoofCorner)] = true;
	Kit.Bounds[int32(EFacadeSlot::RoofCorner)] = FBox(FVector(0.0, -300.0, -20.0), FVector(300.0, 0.0, 0.0));
	CSCityTests_SetCorner(Kit, 300.0);
	FFacadeBuilding Building;
	Building.Block.Width = 1800.0;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 1200.0;
	Building.Shape = ECSCityMassShape::Box;
	Building.Stack = ECSCityMassStack::None;

	// One bay: 6 x 4 bays, 3 storeys. The walls lose a bay at each end: (4 + 2) x 2 = 12 bays a storey.
	const FFacadeLayout One = LayoutFacade(Kit, Building);
	TestEqual(TEXT("one bay: facade cells"), One.FacadeCells, 3u * 12u);
	TestEqual(TEXT("one bay: corners a storey at a time and the parapet's"), One.Corners, 4u * 4u);
	TestEqual(TEXT("one bay: parapet bays"), One.Parapets, 12u);
	TestEqual(TEXT("one bay: a roof piece a corner"), One.RoofCorners, 4u);
	TestEqual(TEXT("one bay: roof tiles leave the corners"), One.Roofs, 24u - 4u);
	const TArray<FBox> OneFront = CSCityTests_OnFront(CSCityTests_Cells(One), 600.0);
	TestEqual(TEXT("one bay: the front's bays"), OneFront.Num(), 3 * 4);
	for (const FBox& Box : OneFront) TestTrue(TEXT("one bay: nothing trimmed, the wall starts a bay in"), FMath::IsNearlyEqual(Box.GetSize().X, 300.0, 0.1) && Box.Min.X >= -600.0 - 0.1 && Box.Max.X <= 600.0 + 0.1);
	for (const FBox& Box : One.Boxes[int32(EFacadeSlot::Corner)]) TestTrue(TEXT("one bay: the piece reaches a bay each way"), FMath::IsNearlyEqual(Box.GetSize().X, 300.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 300.0, 0.1));
	TestTrue(TEXT("one bay: the door in a bay the corners left"), One.Doors == 1u && One.DoorCell >= 1 && One.DoorCell <= 4);

	// A bay's width that is not the corner's: the piece is stretched to the bay, differently along each side.
	Building.Block.Width = 1900.0;
	const FFacadeLayout Stretched = LayoutFacade(Kit, Building);
	const double CellX = 1900.0 / 6.0;
	for (const FBox& Box : Stretched.Boxes[int32(EFacadeSlot::Corner)]) TestTrue(TEXT("stretched: to the bays"), FMath::IsNearlyEqual(Box.GetSize().X, CellX, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 300.0, 0.1));
	Building.Block.Width = 1800.0;

	// Two bays' radius: (2 + 0) x 2 = 4 bays a storey; the side walls are all corner.
	FFacadeKitShape Big = Kit;
	CSCityTests_SetCorner(Big, 600.0);
	const FFacadeLayout Two = LayoutFacade(Big, Building);
	TestEqual(TEXT("two bays: facade cells"), Two.FacadeCells, 3u * 4u);
	TestEqual(TEXT("two bays: roof tiles leave 2 x 2 bays a corner"), Two.Roofs, 24u - 16u);
	TestEqual(TEXT("two bays: a roof piece a corner"), Two.RoofCorners, 4u);

	// A drum: 12 x 12 m with the 6 m radius. No walls at all, four corners, no tiles.
	FFacadeBuilding Drum = Building;
	Drum.Block.Width = 1200.0;
	Drum.Block.Depth = 1200.0;
	const FFacadeLayout Round = LayoutFacade(Big, Drum);
	TestEqual(TEXT("drum: no walls"), Round.FacadeCells, 0u);
	TestEqual(TEXT("drum: no parapet bays"), Round.Parapets, 0u);
	TestEqual(TEXT("drum: four quarters"), Round.RoofCorners, 4u);
	TestEqual(TEXT("drum: no tiles"), Round.Roofs, 0u);
	TestEqual(TEXT("drum: corners a storey at a time and the parapet's"), Round.Corners, 4u * 4u);

	// A wall too short for its two corners: they share it.
	FFacadeBuilding Thin = Building;
	Thin.Block.Depth = 300.0;
	const FFacadeLayout Slim = LayoutFacade(Kit, Thin);
	for (const FBox& Box : Slim.Boxes[int32(EFacadeSlot::Corner)]) TestTrue(TEXT("shared wall: half of it each"), FMath::IsNearlyEqual(Box.GetSize().Y, 150.0, 0.1));
	TestEqual(TEXT("shared wall: only the long sides have bays"), Slim.FacadeCells, 3u * 8u);

	// A pilaster with a roof piece: in a 300 cm bay a 100 cm piece keeps its size, and tiles fill the rest of the bay.
	FFacadeKitShape Thinner = Kit;
	CSCityTests_SetCorner(Thinner, 100.0);
	const FFacadeLayout Pilaster = LayoutFacade(Thinner, Building);
	TestEqual(TEXT("pilaster: the walls keep their bays"), Pilaster.FacadeCells, 3u * 20u);
	double Area = 0.0;
	for (const FBox& Box : Pilaster.Boxes[int32(EFacadeSlot::Roof)]) Area += Box.GetSize().X * Box.GetSize().Y;
	for (const FBox& Box : Pilaster.Boxes[int32(EFacadeSlot::RoofCorner)]) Area += Box.GetSize().X * Box.GetSize().Y;
	TestTrue(TEXT("pilaster: tiles and pieces cover the roof once"), FMath::IsNearlyEqual(Area, 1800.0 * 1200.0, 1.0));

	// Without a roof piece the tiles cover the corners, as they always have.
	FFacadeKitShape NoPiece = Kit;
	NoPiece.bHas[int32(EFacadeSlot::RoofCorner)] = false;
	const FFacadeLayout Tiled = LayoutFacade(NoPiece, Building);
	TestTrue(TEXT("no roof piece: tiles everywhere"), Tiled.Roofs == 24u && Tiled.RoofCorners == 0u);
	return true;
}

// The upper storeys standing out past a smaller base. Each overhang's base is the footprint less whole rows of bays on its
// sides; the door moves to the base's front. A flat one has a soffit tile under every bay past the base and columns along
// its outer edges (not along the flush walls), at its corners and every ColumnEvery bays, once each; the base's corners
// have no parapet piece.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadeOverhangTest, "PCGPlugins.CSRoadNet.City.FacadeOverhang",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFacadeOverhangTest::RunTest(const FString& Parameters)
{
	using namespace CSCity;
	FFacadeKitShape Kit = CSCityTests_Kit();
	Kit.bHas[int32(EFacadeSlot::Soffit)] = true;
	Kit.bHas[int32(EFacadeSlot::Column)] = true;
	Kit.Bounds[int32(EFacadeSlot::Column)] = FBox(FVector(0.0, -80.0, 0.0), FVector(80.0, 0.0, 400.0));
	Kit.ColumnEvery = 2;
	FFacadeBuilding Building;
	Building.Seed = 7;
	Building.Block.Width = 1800.0;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 2400.0;
	Building.Shape = ECSCityMassShape::Box;
	Building.Stack = ECSCityMassStack::Overhang;

	auto Check = [&](ECSCityMassOverhang Where, int32 BaseCells, uint32 PastCells, uint32 Columns)
	{
		const FString Name = StaticEnum<ECSCityMassOverhang>()->GetNameStringByValue(int64(Where));
		Building.Overhang = Where;
		const FFacadeLayout Layout = LayoutFacade(Kit, Building);
		const FFacadeMassing& M = Layout.Massing;
		TestTrue(Name + TEXT(": an overhang of two tiers"), M.Stack == ECSCityMassStack::Overhang && M.Tiers.Num() == 2 && M.OverhangDepth == 1);
		if (M.Tiers.Num() != 2) return;
		TestEqual(Name + TEXT(": base cells"), CSCityTests_Outline(M, 0).Cells, BaseCells);
		TestEqual(Name + TEXT(": upper cells, the whole footprint"), CSCityTests_Outline(M, 1).Cells, 24);
		TestTrue(Name + TEXT(": the base 1 or 2 storeys"), M.Tiers[0].End >= 1 && M.Tiers[0].End <= 2 && M.Tiers[1].First == M.Tiers[0].End && M.Tiers[1].End == M.Storeys);
		TestEqual(Name + TEXT(": soffit tiles"), Layout.Soffits, PastCells);
		TestEqual(Name + TEXT(": columns"), Layout.Columns, Columns);
		TestEqual(Name + TEXT(": roof tiles only on top"), Layout.Roofs, 24u);
		TestEqual(Name + TEXT(": one door"), Layout.Doors, 1u);
		// The base's outline and the upper one's, at their storeys.
		const FCSCityTestsOutline Lower = CSCityTests_Outline(M, 0);
		const uint32 BaseStoreys = uint32(M.Tiers[0].End);
		TestEqual(Name + TEXT(": facade cells"), Layout.FacadeCells, uint32(Lower.Edges) * BaseStoreys + 20u * (uint32(M.Storeys) - BaseStoreys));
		TestEqual(Name + TEXT(": parapet bays, none on the base"), Layout.Parapets, 20u);
		TestEqual(Name + TEXT(": corners, the base's without a parapet piece"), Layout.Corners, uint32(Lower.Outer) * BaseStoreys + 4u * (uint32(M.Storeys) - BaseStoreys + 1u));
		// Every column stands under the upper outline, none inside the base.
		for (const FBox& Box : Layout.Boxes[int32(EFacadeSlot::Column)])
		{
			TestTrue(Name + TEXT(": a column from the ground to the soffit"), FMath::IsNearlyZero(Box.Min.Z, 0.1) && FMath::IsNearlyEqual(Box.Max.Z, Kit.StoreyBase(M.Tiers[1].First), 0.1));
			TestTrue(Name + TEXT(": a column on the edge"), FMath::IsNearlyEqual(Box.Max.Y, 600.0, 0.1) || FMath::IsNearlyEqual(Box.Min.Y, -600.0, 0.1) || FMath::IsNearlyEqual(Box.Max.X, 900.0, 0.1) || FMath::IsNearlyEqual(Box.Min.X, -900.0, 0.1));
		}
		// The door is on the base's front row.
		const int32 BaseFront = Where == ECSCityMassOverhang::Sides ? 3 : 2;
		TestTrue(Name + TEXT(": a door cell"), Layout.DoorCell != INDEX_NONE && M.At(0, Layout.DoorCell, BaseFront));
		for (const FBox& Box : Layout.Boxes[int32(EFacadeSlot::Door)]) TestTrue(Name + TEXT(": the door on the base, at the ground"), FMath::IsNearlyZero(Box.Min.Z, 0.1) && FMath::IsNearlyEqual(Box.Max.Y, -600.0 + (BaseFront + 1) * 300.0, 0.1));
	};
	// 6 x 4 bays, a column every two along the edges that face out, one at each vertex: the front's 4 (0, 2, 4, 6); each
	// side's 3 (0, 2, 4), sharing their corners with the front or back when those stand out too.
	Check(ECSCityMassOverhang::Front, 18, 6u, 4u);
	Check(ECSCityMassOverhang::Sides, 16, 8u, 6u);
	Check(ECSCityMassOverhang::FrontAndSides, 12, 12u, 8u);
	Check(ECSCityMassOverhang::AllRound, 8, 16u, 10u);

	// The grid too small for the overhang asked: the sides that do not fit stay flush.
	FFacadeBuilding Small = Building;
	Small.Block.Width = 600.0;
	Small.Block.Depth = 300.0;
	Small.Overhang = ECSCityMassOverhang::AllRound;
	TestTrue(TEXT("no room: no overhang"), MassFacade(Kit, Small).Stack == ECSCityMassStack::None);
	Small.Block.Depth = 600.0;
	Small.Overhang = ECSCityMassOverhang::Front;
	TestTrue(TEXT("room at the front: an overhang"), MassFacade(Kit, Small).Stack == ECSCityMassStack::Overhang);

	// Auto: a kit with the odds draws overhangs; without, its buildings stay as they were.
	FFacadeKitShape Odds = Kit;
	Odds.OverhangChance = 0.5f;
	Building.Stack = ECSCityMassStack::Auto;
	Building.Overhang = ECSCityMassOverhang::Auto;
	TSet<int32> Wheres;
	int32 Moved = 0;
	for (uint32 Seed = 1; Seed <= 200; Seed++)
	{
		Building.Seed = Seed * 0x27D4EB2Fu;
		const FFacadeMassing A = MassFacade(Odds, Building);
		if (A.OverhangDepth > 0) Wheres.Add(int32(A.OverhangFaces));
		FFacadeKitShape None = Kit;
		None.OverhangChance = 0.0f;
		Moved += MassFacade(None, Building).OverhangDepth;
	}
	TestEqual(TEXT("auto: every side drawn"), Wheres.Num(), 4);
	TestEqual(TEXT("auto: no odds, no overhang"), Moved, 0);
	return true;
}

// Under the base. Plots on a sloping street know the road's height at the ends of their fronts, and a seated plot stands at
// the higher end with the ground falling away from it. With a plinth a building has a cell under every wall and a piece under
// every corner, whatever the ground; where the road falls away along its front and there is room, a platform as wide as the
// building, its edges plinth cells too, and flights of steps from the door every StairEvery bays where the drop asks, as many
// one after another as there is room for. Without a plinth, none of it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadePlinthTest, "PCGPlugins.CSRoadNet.City.FacadePlinth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFacadePlinthTest::RunTest(const FString& Parameters)
{
	using namespace CSCity;
	// A street rising 5 cm a metre along +X, plots on both sides.
	FRoadLine Street = CSCityTests_Street(11, FVector(0.0, 0.0, 0.0), FVector(20000.0, 0.0, 1000.0), 500.0);
	FLotSettings Settings;
	Settings.Frontage = 1800.0;
	Settings.Gap = 0.0;
	Settings.bFill = false;
	Settings.Setback = 300.0;
	const TArray<FLot> Lots = LayoutLots({ Street }, {}, {}, Settings);
	TestTrue(TEXT("sloping street: plots"), Lots.Num() >= 16);
	int32 WrongHeights = 0;
	int32 WrongSeats = 0;
	for (const FLot& Plain : Lots)
	{
		const FVector2D Front = Plain.Front.GetSafeNormal();
		const FVector2D HalfX = FVector2D(Front.Y, -Front.X) * (Plain.Width * 0.5);
		const FVector2D Near = Plain.Centre + Front * (Plain.Depth * 0.5);
		// The road's height there is 5 cm a metre of world X, on both sides of the street.
		const double WantStart = (Near - HalfX).X * 0.05;
		const double WantEnd = (Near + HalfX).X * 0.05;
		WrongHeights += FMath::IsNearlyEqual(Plain.FrontHeightStart, WantStart, 1.0) && FMath::IsNearlyEqual(Plain.FrontHeightEnd, WantEnd, 1.0) && FMath::IsNearlyEqual(Plain.FrontRoom, 300.0) ? 0 : 1;
		FLot Lot = Plain;
		const double Top = FMath::Max(Lot.FrontHeightStart, Lot.FrontHeightEnd);
		const FLot& Seated = Lot;
		SeatLot(Lot, Top, [&Seated](const FVector2D& P) { return LotRoadHeight(Seated, P); });
		// At the high end the ground is at the base; at the low end a frontage's rise (90 cm) under it.
		const double DropStart = Lot.Drop - Lot.DropX * Lot.Width * 0.5;
		const double DropEnd = Lot.Drop + Lot.DropX * Lot.Width * 0.5;
		WrongSeats += Lot.Base == Top && FMath::IsNearlyZero(FMath::Min(DropStart, DropEnd), 0.01) && FMath::IsNearlyEqual(FMath::Max(DropStart, DropEnd), 90.0, 1.0) && FMath::IsNearlyZero(Lot.DropY, 1e-6) ? 0 : 1;
	}
	TestEqual(TEXT("sloping street: the road's height at each end of the front"), WrongHeights, 0);
	TestEqual(TEXT("sloping street: seated at the high end, the ground falling away"), WrongSeats, 0);

	FFacadeKitShape Kit = CSCityTests_Kit();
	Kit.bHas[int32(EFacadeSlot::Plinth)] = true;
	Kit.bHas[int32(EFacadeSlot::Platform)] = true;
	Kit.bHas[int32(EFacadeSlot::Stair)] = true;
	Kit.Bounds[int32(EFacadeSlot::Plinth)] = FBox(FVector(0.0, -50.0, 0.0), FVector(300.0, 0.0, 100.0));
	Kit.Bounds[int32(EFacadeSlot::Stair)] = FBox(FVector(0.0, 0.0, -75.0), FVector(300.0, 150.0, 0.0));
	Kit.Bounds[int32(EFacadeSlot::Platform)] = FBox(FVector(0.0, 0.0, -15.0), FVector(300.0, 300.0, 0.0));
	Kit.PlinthSink = 50.0;
	Kit.PlatformDepth = 150.0;
	Kit.PlatformMinDrop = 15.0;
	Kit.StairEvery = 2;
	FFacadeBuilding Building;
	Building.Block.Width = 1800.0;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 1200.0;
	Building.Shape = ECSCityMassShape::Box;
	Building.Stack = ECSCityMassStack::None;
	Building.FrontRoom = 300.0;

	// Flat: only the plinth, on the outline, and a piece under each corner (Corner stretched, with no PlinthCorner).
	const FFacadeLayout Flat = LayoutFacade(Kit, Building);
	TestEqual(TEXT("flat: plinth cells"), Flat.Plinths, 20u);
	TestEqual(TEXT("flat: corners and those under them"), Flat.Corners, 4u * 4u + 4u);
	TestTrue(TEXT("flat: no platform, no steps"), Flat.Platforms == 0u && Flat.Stairs == 0u && Flat.PlatformDepth == 0.0);
	FFacadeKitShape NoPlinth = Kit;
	NoPlinth.bHas[int32(EFacadeSlot::Plinth)] = false;
	Building.Drop = 60.0;
	Building.DropX = 0.04;
	const FFacadeLayout Bare = LayoutFacade(NoPlinth, Building);
	TestTrue(TEXT("no plinth: nothing under the base"), Bare.Plinths == 0u && Bare.Platforms == 0u && Bare.Stairs == 0u && Bare.Corners == 16u);

	// The ground falling 24 to 96 cm under the base along the front: a platform; door in bay 3, flights at bays 1, 3 and 5.
	const FFacadeLayout Sloped = LayoutFacade(Kit, Building);
	TestEqual(TEXT("slope: a platform tile a bay"), Sloped.Platforms, 6u);
	TestEqual(TEXT("slope: plinth cells, the platform's edges too"), Sloped.Plinths, 20u + 6u + 2u);
	TestEqual(TEXT("slope: flights of steps, room for one each"), Sloped.Stairs, 3u);
	TestTrue(TEXT("slope: the platform takes what room the steps leave"), FMath::IsNearlyEqual(Sloped.PlatformDepth, 150.0, 0.1));
	double FloorLeft = TNumericLimits<double>::Max();
	for (const FBox& Box : Sloped.Boxes[int32(EFacadeSlot::Platform)])
	{
		// Stopping at the 50 cm edge cells round it, on every side but the building's.
		FloorLeft = FMath::Min(FloorLeft, Box.Min.X);
		TestTrue(TEXT("slope: the platform from the front out, inside its edges"), FMath::IsNearlyEqual(Box.Min.Y, 600.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 100.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().X, 1700.0 / 6.0, 0.1) && FMath::IsNearlyZero(Box.Max.Z, 0.1));
	}
	TestTrue(TEXT("slope: the platform inside its side edges"), FMath::IsNearlyEqual(FloorLeft, -850.0, 0.1));
	for (const FBox& Box : Sloped.Boxes[int32(EFacadeSlot::Stair)])
	{
		// Their own rise where one flight reaches the ground, steeper at the deepest (90 cm), with room for only one.
		const double Rise = FMath::Max(75.0, 60.0 + 0.04 * Box.GetCenter().X);
		TestTrue(TEXT("slope: steps from the platform's edge, down to the ground"), FMath::IsNearlyEqual(Box.Min.Y, 750.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 150.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Z, Rise, 0.1) && FMath::IsNearlyZero(Box.Max.Z, 0.1));
	}

	// More room: the deepest drop (90 cm at bay 5) takes two flights.
	Building.FrontRoom = 450.0;
	TestEqual(TEXT("room for two: flights"), LayoutFacade(Kit, Building).Stairs, 4u);
	// Too little room for steps: the platform alone. None at all: nothing in front.
	Building.FrontRoom = 100.0;
	const FFacadeLayout Tight = LayoutFacade(Kit, Building);
	TestTrue(TEXT("no room for steps: a platform alone"), Tight.Platforms == 6u && Tight.Stairs == 0u && FMath::IsNearlyEqual(Tight.PlatformDepth, 100.0, 0.1));
	Building.FrontRoom = 0.0;
	TestEqual(TEXT("no room: no platform"), LayoutFacade(Kit, Building).Platforms, 0u);
	// A slope too small to matter, and steps only where the drop asks.
	Building.FrontRoom = 300.0;
	Building.Drop = 5.0;
	Building.DropX = 0.0;
	TestEqual(TEXT("next to no drop: no platform"), LayoutFacade(Kit, Building).Platforms, 0u);
	Building.Drop = 15.0;
	Building.DropX = 0.04;
	// At the flights' bays the drop is -3, 21 and 45 cm.
	TestEqual(TEXT("steps only where the drop asks"), LayoutFacade(Kit, Building).Stairs, 2u);
	// Over an overhang's recessed base the platform starts at the base's front.
	Building.Stack = ECSCityMassStack::Overhang;
	Building.Overhang = ECSCityMassOverhang::Front;
	Building.Seed = 3;
	Building.Drop = 60.0;
	for (const FBox& Box : LayoutFacade(Kit, Building).Boxes[int32(EFacadeSlot::Platform)]) TestTrue(TEXT("overhang: the platform from the base's front"), FMath::IsNearlyEqual(Box.Min.Y, 300.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 400.0, 0.1));
	// Behind a corner piece taking a bay, the floor and the platform's sides go back as far, so no ground shows behind it.
	Building.Stack = ECSCityMassStack::None;
	FFacadeKitShape Round = Kit;
	CSCityTests_SetCorner(Round, 300.0);
	const FFacadeLayout Rounded = LayoutFacade(Round, Building);
	for (const FBox& Box : Rounded.Boxes[int32(EFacadeSlot::Platform)]) TestTrue(TEXT("round corners: the floor goes back a bay"), FMath::IsNearlyEqual(Box.Min.Y, 300.0, 0.1) && FMath::IsNearlyEqual(Box.GetSize().Y, 400.0, 0.1));
	int32 Sides = 0;
	for (const FBox& Box : Rounded.Boxes[int32(EFacadeSlot::Plinth)]) Sides += FMath::IsNearlyEqual(Box.GetSize().Y, 450.0, 0.1) ? 1 : 0;
	TestEqual(TEXT("round corners: the platform's sides go back as far"), Sides, 2);
	return true;
}

// The city's field, read on the GPU as each building is put together. A face stands out when the cells along it say so on average
// (half of it or more), each face on its own; a face it will not have stays flush, whatever was drawn; massing set by hand is left
// alone. A corner takes the style on at least half of the cells round it, or the kit's own if the kit has no such piece, and the
// walls beside it give it their bays; a style set on the building wins. Painting tells whatever stands there, and only that is
// written again. Nothing painted takes nothing on the GPU, and a page only its 256 bytes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityFacadeFieldTest, "PCGPlugins.CSRoadNet.City.FacadeField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityFacadeFieldTest::RunTest(const FString& Parameters)
{
	using namespace CSCity;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSCityFieldActor* Field = World->SpawnActor<ACSCityFieldActor>();
	if (!TestNotNull(TEXT("field"), Field)) return false;
	TestTrue(TEXT("nothing painted: nothing on the GPU"), Field->GpuBytes() == 0 && !Field->GpuField().IsValid());

	FFacadeKitShape Kit = CSCityTests_Kit();
	Kit.bHas[int32(EFacadeSlot::Soffit)] = true;
	Kit.Bounds[int32(EFacadeSlot::Soffit)] = FBox(FVector(0.0, 0.0, -25.0), FVector(300.0, 300.0, 0.0));
	// 18 x 12 m, front +Y, six storeys, in the middle of a field cell's page.
	FFacadeBuilding Building;
	Building.Block.Base = FVector(3200.0, 3200.0, 0.0);
	Building.Block.Front = FVector2D(0.0, 1.0);
	Building.Block.Width = 1800.0;
	Building.Block.Depth = 1200.0;
	Building.Block.Height = 2400.0;
	Building.Seed = 11;
	Building.Shape = ECSCityMassShape::Box;
	Building.Stack = ECSCityMassStack::None;
	const FVector2D Middle(Building.Block.Base);

	auto Faces = [&](const FFacadeBuilding& Asked) { return MassFacade(Kit, Asked, Field->GpuField()).OverhangFaces; };
	auto Paint = [&](double X0, double Y0, double X1, double Y1, ECSCityOverhangField Value) { Field->SetOverhangInBox(Middle + FVector2D(X0, Y0), Middle + FVector2D(X1, Y1), Value); };
	TestEqual(TEXT("nothing painted: drawn as it was"), Faces(Building), 0u);

	// Each face on its own.
	Paint(-900.0, 500.0, 900.0, 700.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("over the front: the front"), Faces(Building), 1u);
	TestTrue(TEXT("a page or two on the GPU"), Field->GpuBytes() > 0 && Field->GpuBytes() <= 16 * 256 + 16 * 4);
	Field->ClearField();
	TestEqual(TEXT("cleared: none"), Faces(Building), 0u);
	Paint(800.0, -500.0, 1000.0, 500.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("over the +X side: that one"), Faces(Building), 2u);
	Field->ClearField();
	Paint(-900.0, -700.0, 900.0, -500.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("over the back: the back"), Faces(Building), 4u);
	Field->ClearField();
	Paint(-1000.0, -500.0, -800.0, 500.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("over the -X side: that one"), Faces(Building), 8u);
	Field->ClearField();

	// The average along a face: a quarter of it is not enough, three quarters are.
	Paint(-900.0, 500.0, -500.0, 700.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("a quarter of the front: no"), Faces(Building), 0u);
	Field->ClearField();
	Paint(-900.0, 500.0, 500.0, 700.0, ECSCityOverhangField::Always);
	TestEqual(TEXT("three quarters of the front: yes"), Faces(Building), 1u);
	Field->ClearField();

	// What it will not have, and what was set by hand.
	FFacadeBuilding Drawn = Building;
	Drawn.Stack = ECSCityMassStack::Overhang;
	Drawn.Overhang = ECSCityMassOverhang::FrontAndSides;
	TestEqual(TEXT("drawn front and sides"), Faces(Drawn), 11u);
	Paint(-900.0, 500.0, 900.0, 700.0, ECSCityOverhangField::Never);
	TestEqual(TEXT("never over the front: the sides only"), Faces(Drawn), 10u);
	Paint(800.0, -700.0, 1000.0, 700.0, ECSCityOverhangField::Never);
	Paint(-1000.0, -700.0, -800.0, 700.0, ECSCityOverhangField::Never);
	TestTrue(TEXT("never all round: no overhang"), MassFacade(Kit, Drawn, Field->GpuField()).Stack == ECSCityMassStack::None);
	FFacadeBuilding Locked = Drawn;
	Locked.bLockMassing = true;
	TestEqual(TEXT("set by hand: as set"), Faces(Locked), 11u);
	Field->ClearField();
	Paint(-900.0, 500.0, 900.0, 700.0, ECSCityOverhangField::Always);
	FFacadeBuilding Flat = Building;
	Flat.bLockMassing = true;
	TestEqual(TEXT("set flat by hand: flat"), Faces(Flat), 0u);
	// The modules follow: a soffit tile a bay along the front, the base a row back.
	const FFacadeLayout Out = LayoutFacade(Kit, Building, Field->GpuField());
	TestEqual(TEXT("the front out: soffit tiles"), Out.Soffits, 6u);
	TestTrue(TEXT("the front out: two tiers"), Out.Massing.Tiers.Num() == 2 && Out.Massing.OverhangDepth == 1 && !Out.Massing.At(0, 3, 3) && Out.Massing.At(0, 3, 2) && Out.Massing.At(1, 3, 3));
	Field->ClearField();

	// Corner styles: a kit with a round one a bay wide.
	const int32 RoundSlot = FacadeCornerSlot(ECSCityCornerStyle::Round, EFacadeCornerPiece::Corner);
	Kit.bHas[RoundSlot] = true;
	Kit.Bounds[RoundSlot] = FBox(FVector(0.0, -300.0, 0.0), FVector(300.0, 0.0, 400.0));
	Kit.StyleCorner[int32(ECSCityCornerStyle::Round) - 1] = 300.0;
	Kit.StyleDepth[int32(ECSCityCornerStyle::Round) - 1] = 300.0;
	const uint32 Storeys = 6u;
	const FFacadeLayout Plain = LayoutFacade(Kit, Building, Field->GpuField());
	TestEqual(TEXT("no style painted: the kit's corners"), Plain.Corners, 4u * (Storeys + 1u));
	TestEqual(TEXT("no style painted: no round ones"), Plain.Counts[RoundSlot], 0u);
	TestEqual(TEXT("no style painted: every bay a wall"), Plain.FacadeCells, 20u * Storeys);
	// Round over the front -X corner only: its two walls give up a bay each.
	Field->SetCornerStyleInBox(Middle + FVector2D(-1100.0, 400.0), Middle + FVector2D(-700.0, 800.0), ECSCityCornerStyle::Round);
	const FFacadeLayout OneRound = LayoutFacade(Kit, Building, Field->GpuField());
	TestEqual(TEXT("one corner painted: one round column"), OneRound.Counts[RoundSlot], Storeys + 1u);
	TestEqual(TEXT("one corner painted: three of the kit's"), OneRound.Corners, 3u * (Storeys + 1u));
	TestEqual(TEXT("one corner painted: its walls give up a bay each"), OneRound.FacadeCells, 18u * Storeys);
	for (const FBox& Box : OneRound.Boxes[RoundSlot]) TestTrue(TEXT("one corner painted: on that corner"), FMath::IsNearlyEqual(Box.Min.X, -900.0, 0.1) && FMath::IsNearlyEqual(Box.Max.Y, 600.0, 0.1));
	// A style the kit has no piece for: its own.
	Field->SetCornerStyleInBox(Middle + FVector2D(-1100.0, 400.0), Middle + FVector2D(-700.0, 800.0), ECSCityCornerStyle::Chamfer);
	TestEqual(TEXT("a style it does not have: the kit's corners"), LayoutFacade(Kit, Building, Field->GpuField()).Corners, 4u * (Storeys + 1u));
	// Set on the building: all round, whatever is painted.
	FFacadeBuilding AllRound = Building;
	AllRound.CornerStyle = ECSCityCornerStyle::Round;
	TestEqual(TEXT("set on the building: all four round"), LayoutFacade(Kit, AllRound, Field->GpuField()).Counts[RoundSlot], 4u * (Storeys + 1u));
	Field->ClearField();

	// A street on it: painting all of it brings overhangs, and nothing else is written again.
	ACSCityStreetActor* Street = World->SpawnActor<ACSCityStreetActor>();
	if (!TestNotNull(TEXT("street"), Street)) return false;
	Street->ApplyFabric(ECSCityFabric::StreetWall);
	Street->Vacancy = 0.0f;
	Street->OverhangChance = 0.0f;
	Street->SetbackChance = 0.0f;
	Street->PodiumChance = 0.0f;
	Street->bOnGround = false;
	Street->Sides = ECSCityStreetSides::Right;
	Street->StreetWidth = 10.0f;
	Street->Street->SetSplinePoints({ FVector(0.0, 0.0, 0.0), FVector(12000.0, 0.0, 0.0) }, ESplineCoordinateSpace::World);
	Street->Rebuild();
	CSRoad::LandMeshWrites();
	auto Lines = [&Street](const TCHAR* What) { int32 Count = 0; for (const FString& Line : Street->Plots) Count += Line.Contains(What) ? 1 : 0; return Count; };
	TestTrue(TEXT("street: buildings"), Street->NumBuildings >= 5);
	TestEqual(TEXT("street: no overhangs drawn"), Lines(TEXT("overhang")), 0);
	TestFalse(TEXT("street: unchanged, nothing written"), Street->UpdateFromNetwork({}, 0.0));
	Field->SetOverhangInBox(FVector2D(-2000.0, -5000.0), FVector2D(14000.0, 8000.0), ECSCityOverhangField::Always);
	CSRoad::LandMeshWrites();
	TestEqual(TEXT("street: painted over, all of them stand out"), Lines(TEXT("overhang")), Street->NumBuildings);
	Field->ClearField();
	CSRoad::LandMeshWrites();
	TestEqual(TEXT("street: cleared, none again"), Lines(TEXT("overhang")), 0);
	return true;
}

// Plots laid out along two streets and written as a build writes them, with no prefab data, so it runs anywhere: two
// Nanite meshes draw them as GPU instances through Nanite, one component per mesh, every instance within its plot.
// Fewer plots rewrite the components in place; a mesh no plot takes loses its component; no meshes, the tile goes
// to the default facade kit (a component per slot, the modules written on the GPU: as many in each slot as the layout
// has cells for, every one within its plot), and back. No mesh is ever made for the buildings.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityInstancedTileTest, "PCGPlugins.CSRoadNet.City.InstancedTile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityInstancedTileTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	UStaticMesh* First = CSCityTests_NaniteCube(TEXT("SM_CSCityTestHouseA"));
	UStaticMesh* Second = CSCityTests_NaniteCube(TEXT("SM_CSCityTestHouseB"));
	if (!TestNotNull(TEXT("first mesh"), First) || !TestNotNull(TEXT("second mesh"), Second)) return false;
	if (!TestTrue(TEXT("the meshes have Nanite data"), First->HasValidNaniteData() && Second->HasValidNaniteData())) return false;

	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->bBuildOnLoad = false;
	Network->BuildingMeshes = { First, Second };

	CSCity::FLotSettings Settings;
	const TArray<CSCity::FRoadLine> Streets = { CSCityTests_Street(1, FVector(0.0, 0.0, 0.0), FVector(32000.0, 0.0, 0.0), 400.0), CSCityTests_Street(2, FVector(0.0, 9000.0, 0.0), FVector(32000.0, 9000.0, 0.0), 400.0) };
	const TArray<CSCity::FLot> Lots = CSCity::LayoutLots(Streets, Streets, {}, Settings);
	if (!TestTrue(TEXT("plots laid out"), Lots.Num() >= 16)) return false;
	const FIntPoint Tile(0, 0);
	Network->WriteBuildingTileForTest(Tile, Lots);

	TArray<UCSGpuInstancedMeshComponent*> Components = CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network);
	TestEqual(TEXT("written: a component per mesh"), Components.Num(), 2);
	const TArray<FCSCityTests_Instance> Instances = CSCityTests_Instances(Network);
	TestEqual(TEXT("written: an instance per plot"), Instances.Num(), Lots.Num());
	FlushRenderingCommands();
	for (UCSGpuInstancedMeshComponent* Component : Components)
	{
		TestTrue(TEXT("written: drawn by Nanite"), Component->IsNaniteRenderPath());
		UCSGpuInstancedNaniteComponent* Nanite = Component->GetNaniteComponent();
		if (!TestNotNull(TEXT("written: the Nanite stand-in"), Nanite)) continue;
		TestEqual(TEXT("written: GPU-Scene slots"), Nanite->GetGpuSceneSlotCount(), uint32(Component->GetInstanceCount()));
		const FString Mismatch = Component->DebugGetDrawnAssetMismatchSync();
		TestTrue(FString::Printf(TEXT("written: draws what it should (%s)"), *Mismatch), Mismatch.IsEmpty());
		TestNull(TEXT("written: no proxy of its own, the stand-in draws"), Component->GetSceneProxy());
	}
	TestEqual(TEXT("written: no building meshes"), CSCityTests_BuildingMeshes(Network), 0);

	// Every plot holds exactly one instance, whole.
	TArray<int32> Holding;
	Holding.SetNumZeroed(Lots.Num());
	int32 Outside = 0;
	for (const FCSCityTests_Instance& Instance : Instances)
	{
		const FVector Middle = Instance.Component->BaseMesh->GetBoundingBox().GetCenter();
		const FVector2D At(Instance.Transform.TransformPosition(Middle));
		const int32 Plot = Lots.IndexOfByPredicate([&At](const CSCity::FLot& Lot) { return CSCity::Contains(Lot.Corners, At); });
		if (Plot == INDEX_NONE)
		{
			Outside++;
			continue;
		}
		Holding[Plot]++;
		const CSCity::FPolygon Kept = CSCity::Grow(Lots[Plot].Corners, 1.0);
		for (const FVector& Corner : CSCityTests_Corners(Instance.Component->BaseMesh->GetBoundingBox(), Instance.Transform)) Outside += CSCity::Contains(Kept, FVector2D(Corner)) ? 0 : 1;
	}
	TestEqual(TEXT("written: instances outside their plots"), Outside, 0);
	int32 NotOne = 0;
	for (const int32 Count : Holding) NotOne += Count == 1 ? 0 : 1;
	TestEqual(TEXT("written: plots without exactly one instance"), NotOne, 0);

	// Fewer plots: the same components, written again.
	const TArray<CSCity::FLot> Half(Lots.GetData(), Lots.Num() / 2);
	Network->WriteBuildingTileForTest(Tile, Half);
	TArray<UCSGpuInstancedMeshComponent*> Again = CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network);
	TestEqual(TEXT("half: instances"), CSCityTests_Instances(Network).Num(), Half.Num());
	TestTrue(TEXT("half: the same components"), Again.Num() == 2 && Again.Contains(Components[0]) && Again.Contains(Components[1]));

	// Plots all taking one mesh: the other's component goes.
	TArray<CSCity::FLot> OneMesh;
	for (const CSCity::FLot& Lot : Lots) if (CSCity::LotMesh(Lot, Network->Lots.Seed, 2) == 0) OneMesh.Add(Lot);
	if (TestTrue(TEXT("plots on the first mesh"), OneMesh.Num() > 0))
	{
		Network->WriteBuildingTileForTest(Tile, OneMesh);
		Again = CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network);
		TestEqual(TEXT("one mesh: one component"), Again.Num(), 1);
		if (Again.Num() == 1) TestTrue(TEXT("one mesh: the first"), Again[0]->BaseMesh == First);
		TestEqual(TEXT("one mesh: instances"), CSCityTests_Instances(Network).Num(), OneMesh.Num());
	}

	// No meshes: the default kit's modules, written on the GPU, a component per slot, and back.
	UCSCityFacadeKit* Kit = UCSCityFacadeKit::Default();
	if (!TestNotNull(TEXT("the default kit (Scripts/CS2Road/MakeFacadeKit.py)"), Kit)) return false;
	const CSCity::FFacadeKitShape Shape = CSCity::KitShape(*Kit);
	Network->BuildingMeshes.Reset();
	Network->WriteBuildingTileForTest(Tile, Lots);
	CSRoad::LandMeshWrites();
	Again = CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network);
	int32 Slots = 0;
	for (const bool bHas : Shape.bHas) Slots += bHas ? 1 : 0;
	TestTrue(TEXT("kit: a component per slot with anything in it"), Again.Num() > 0 && Again.Num() <= Slots);
	// What each slot must hold: the layout's cells (each plot's massing drawn from its seed, as the network draws it), the
	// windows and walls sharing the facade's but for the doors.
	uint32 Want[CSCity::FacadeSlots] = {};
	TSet<int32> Massings;
	for (const CSCity::FLot& Lot : Lots)
	{
		const CSCity::FFacadeLayout Layout = CSCity::LayoutFacade(Shape, CSCity::LotBuilding(Lot));
		Want[int32(CSCity::EFacadeSlot::Wall)] += Layout.FacadeCells - Layout.Doors;
		Want[int32(CSCity::EFacadeSlot::Door)] += Layout.Doors;
		Want[int32(CSCity::EFacadeSlot::Corner)] += Layout.Corners;
		Want[int32(CSCity::EFacadeSlot::CornerInner)] += Layout.InnerCorners;
		Want[int32(CSCity::EFacadeSlot::Parapet)] += Layout.Parapets;
		Want[int32(CSCity::EFacadeSlot::Roof)] += Layout.Roofs;
		Massings.Add(int32(Layout.Massing.Shape) * 8 + int32(Layout.Massing.Stack));
	}
	AddInfo(FString::Printf(TEXT("kit: %d plots in %d kinds of massing"), Lots.Num(), Massings.Num()));
	int32 Got[CSCity::FacadeSlots] = {};
	int32 OutsidePlots = 0;
	for (UCSGpuInstancedMeshComponent* Component : Again)
	{
		TestTrue(TEXT("kit: written on the GPU"), Component->HasInstanceSourceGPU());
		TestTrue(TEXT("kit: drawn by Nanite"), Component->IsNaniteRenderPath());
		int32 Slot = 0;
		while (Slot < CSCity::FacadeSlots && Kit->Module(Slot).Mesh != Component->BaseMesh) Slot++;
		if (!TestTrue(TEXT("kit: a slot's mesh"), Slot < CSCity::FacadeSlots)) continue;
		Got[Slot] = Component->DebugReadDrawnInstanceCountSync();
		// Every module within the plot it stands on.
		const TArray<FVector3f> Corners = CSRoadTest::InstanceCorners(Component);
		for (int32 First8 = 0; First8 + 8 <= Corners.Num(); First8 += 8)
		{
			FVector2D Middle = FVector2D::ZeroVector;
			for (int32 K = 0; K < 8; K++) Middle += FVector2D(Corners[First8 + K].X, Corners[First8 + K].Y) / 8.0;
			const int32 Plot = Lots.IndexOfByPredicate([&Middle](const CSCity::FLot& Lot) { return CSCity::Contains(CSCity::Grow(Lot.Corners, 1.0), Middle); });
			if (Plot == INDEX_NONE)
			{
				OutsidePlots++;
				continue;
			}
			const CSCity::FPolygon Kept = CSCity::Grow(Lots[Plot].Corners, 1.0);
			for (int32 K = 0; K < 8; K++) OutsidePlots += CSCity::Contains(Kept, FVector2D(Corners[First8 + K].X, Corners[First8 + K].Y)) ? 0 : 1;
		}
	}
	const int32 Cells = Got[int32(CSCity::EFacadeSlot::Wall)] + Got[int32(CSCity::EFacadeSlot::Window)] + Got[int32(CSCity::EFacadeSlot::GroundWall)] + Got[int32(CSCity::EFacadeSlot::GroundWindow)];
	TestEqual(TEXT("kit: walls and windows"), Cells, int32(Want[int32(CSCity::EFacadeSlot::Wall)]));
	TestTrue(TEXT("kit: some of each"), Got[int32(CSCity::EFacadeSlot::Wall)] > 0 && Got[int32(CSCity::EFacadeSlot::Window)] > 0);
	for (const CSCity::EFacadeSlot Slot : { CSCity::EFacadeSlot::Door, CSCity::EFacadeSlot::Corner, CSCity::EFacadeSlot::CornerInner, CSCity::EFacadeSlot::Parapet, CSCity::EFacadeSlot::Roof })
	{
		if (Shape.bHas[int32(Slot)]) TestEqual(FString::Printf(TEXT("kit: slot %d"), int32(Slot)), Got[int32(Slot)], int32(Want[int32(Slot)]));
	}
	TestEqual(TEXT("kit: modules outside their plots"), OutsidePlots, 0);
	TestEqual(TEXT("kit: no building meshes"), CSCityTests_BuildingMeshes(Network), 0);
	Network->BuildingMeshes = { First };
	Network->WriteBuildingTileForTest(Tile, Lots);
	Again = CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network);
	TestTrue(TEXT("meshes again: the kit's components gone"), Again.Num() == 1 && Again[0]->BaseMesh == First);
	TestEqual(TEXT("meshes again: every plot on the one mesh"), CSCityTests_Instances(Network).Num(), Lots.Num());
	return true;
}

// The whole of it: a network of alleys and batch buildings drawn as Nanite instances. A main road across some alleys
// rewrites the building tiles near it only and takes houses from under it; moved back and gone, every house
// is where it was. Needs the CS2 prefab data.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityInstancedBuildingsTest, "PCGPlugins.CSRoadNet.City.InstancedBuildings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityInstancedBuildingsTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	UStaticMesh* First = CSCityTests_NaniteCube(TEXT("SM_CSCityTestHouseA"));
	UStaticMesh* Second = CSCityTests_NaniteCube(TEXT("SM_CSCityTestHouseB"));
	if (!TestNotNull(TEXT("first mesh"), First) || !TestNotNull(TEXT("second mesh"), Second)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->bLocalUpdate = true;
	Network->bBakeNanite = false;
	Network->TileSize = 128.0f;
	Network->MaxTilesPerBatch = 0;
	Network->bBatchBuildings = true;
	Network->BuildingMeshes = { First, Second };
	ACSCityAlleyRegion* Region = World->SpawnActor<ACSCityAlleyRegion>();
	if (!TestNotNull(TEXT("alley region"), Region)) return false;
	Region->Bounds->SetBoxExtent(FVector(12000.0, 8000.0, 500.0));
	Region->Generate();

	// Every instance's place, sorted: what a layout comes out as, whichever component draws it.
	auto Places = [&]()
	{
		TArray<FVector> Out;
		for (const FCSCityTests_Instance& Instance : CSCityTests_Instances(Network)) Out.Add(Instance.Transform.GetTranslation());
		Out.Sort([](const FVector& A, const FVector& B) { return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z; });
		return Out;
	};
	auto Serials = [&]()
	{
		TMap<const UCSGpuInstancedMeshComponent*, int32> Out;
		for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network)) Out.Add(Component, Component->GetInstancePackSerial());
		return Out;
	};

	Network->Rebuild();
	TestEqual(TEXT("built: error"), Network->LastError, FString());
	TestTrue(TEXT("built: batch buildings"), Network->NumBuildings > 0);
	TestEqual(TEXT("built: no building meshes"), CSCityTests_BuildingMeshes(Network), 0);
	const TArray<FVector> Built = Places();
	TestEqual(TEXT("built: an instance per building"), Built.Num(), Network->NumBuildings);
	for (const UCSGpuInstancedMeshComponent* Component : CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network)) TestTrue(TEXT("built: drawn by Nanite"), Component->IsNaniteRenderPath());
	const TMap<const UCSGpuInstancedMeshComponent*, int32> Before = Serials();
	AddInfo(FString::Printf(TEXT("%d roads, %d batch buildings as instances in %d components"), Network->NumRoads, Network->NumBuildings, Before.Num()));

	Network->Rebuild();
	TestEqual(TEXT("unchanged: building tiles rebuilt"), Network->BuildingTilesRebuilt, 0);
	TestTrue(TEXT("unchanged: instances not written"), Serials().OrderIndependentCompareEqual(Before));

	// A main road over the north-west corner of the region.
	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-16000.0, 3000.0, 0.0), FVector(-3000.0, 11000.0, 0.0) }, ESplineCoordinateSpace::Local);
	CSRoadTest::TickBuild(Network);
	TestEqual(TEXT("main road: error"), Network->LastError, FString());
	TestTrue(TEXT("main road: some building tiles rebuilt"), Network->BuildingTilesRebuilt > 0);
	const TArray<FVector> Crossed = Places();
	TestEqual(TEXT("main road: an instance per building"), Crossed.Num(), Network->NumBuildings);
	TestTrue(TEXT("main road: the houses changed"), Crossed != Built);
	int32 Untouched = 0;
	const TMap<const UCSGpuInstancedMeshComponent*, int32> After = Serials();
	for (const TPair<const UCSGpuInstancedMeshComponent*, int32>& Pair : Before) Untouched += After.Find(Pair.Key) && After[Pair.Key] == Pair.Value ? 1 : 0;
	TestTrue(TEXT("main road: components away from it not written"), Untouched > 0);
	AddInfo(FString::Printf(TEXT("main road: %d batch buildings, %d building tiles rebuilt, %d of %d components untouched"), Network->NumBuildings, Network->BuildingTilesRebuilt, Untouched, Before.Num()));

	// Gone: every house back where it was.
	Main->Destroy();
	CSRoadTest::TickBuild(Network);
	TestEqual(TEXT("gone: error"), Network->LastError, FString());
	TestTrue(TEXT("gone: every house where it was"), Places() == Built);

	// Switched off: the instances go.
	Network->bBatchBuildings = false;
	Network->Rebuild();
	TestEqual(TEXT("switched off: instances"), CSRoadTest::Built<UCSGpuInstancedMeshComponent>(Network).Num(), 0);
	TestEqual(TEXT("switched off: batch buildings"), Network->NumBuildings, 0);
	return true;
}

// A region with two reserved plots, one of them across its middle: the alleys keep out of both (by
// the clearance), stay in the region and come out the same for the same inputs; without the plots
// they would have run through them.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAlleysTest, "PCGPlugins.CSRoadNet.City.Alleys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAlleysTest::RunTest(const FString& Parameters)
{
	// 300 x 200 m turned by 10 degrees (cm).
	const CSCity::FPolygon Region = CSCityTests_Box(FVector2D(1000.0, -500.0), FVector2D(15000.0, 10000.0), 10.0);
	const TArray<CSCity::FPolygon> Reserved = { CSCityTests_Box(FVector2D(1000.0, -500.0), FVector2D(5000.0, 1500.0), 10.0), CSCityTests_Box(FVector2D(9000.0, 5000.0), FVector2D(1500.0, 1200.0), 35.0) };
	CSCity::FAlleyLayoutSettings Settings;
	const TArray<CSCity::FAlley> Alleys = CSCity::LayoutAlleys(Region, Reserved, Settings);
	TestTrue(TEXT("alleys laid out"), Alleys.Num() >= 6);

	TArray<CSCity::FPolygon> Kept;
	for (const CSCity::FPolygon& Plot : Reserved) Kept.Add(CSCity::Grow(Plot, Settings.Clearance - 1.0));
	auto Crossing = [&Kept](const TArray<CSCity::FAlley>& Laid)
	{
		int32 Count = 0;
		for (const CSCity::FAlley& Alley : Laid) for (const CSCity::FPolygon& Plot : Kept) Count += CSCity::SegmentHits(Plot, Alley.A, Alley.B) ? 1 : 0;
		return Count;
	};
	TestEqual(TEXT("alleys within the clearance of a reserved plot"), Crossing(Alleys), 0);
	const CSCity::FPolygon Edge = CSCity::Grow(Region, 1.0);
	int32 Outside = 0;
	for (const CSCity::FAlley& Alley : Alleys) Outside += (CSCity::Contains(Edge, Alley.A) ? 0 : 1) + (CSCity::Contains(Edge, Alley.B) ? 0 : 1);
	TestEqual(TEXT("alley ends outside the region"), Outside, 0);

	auto Same = [](const TArray<CSCity::FAlley>& A, const TArray<CSCity::FAlley>& B)
	{
		if (A.Num() != B.Num()) return false;
		for (int32 I = 0; I < A.Num(); I++) if (A[I].A != B[I].A || A[I].B != B[I].B) return false;
		return true;
	};
	TestTrue(TEXT("the same inputs give the same alleys"), Same(Alleys, CSCity::LayoutAlleys(Region, Reserved, Settings)));
	TestTrue(TEXT("without the plots some alley runs through them"), Crossing(CSCity::LayoutAlleys(Region, {}, Settings)) > 0);
	CSCity::FAlleyLayoutSettings Other = Settings;
	Other.Seed = 2;
	TestFalse(TEXT("another seed lays them out differently"), Same(Alleys, CSCity::LayoutAlleys(Region, Reserved, Other)));
	return true;
}

// The alleys of L_CityDemo's region as spline roads: every alley ends on another one (the boundary
// alleys close the region), so the network they make has no dead end.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAlleyJunctionsTest, "PCGPlugins.CSRoadNet.City.AlleyJunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAlleyJunctionsTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const CSCity::FPolygon Region = CSCityTests_Box(FVector2D::ZeroVector, FVector2D(15000.0, 10000.0));
	const TArray<CSCity::FPolygon> Reserved = {
		CSCity::Grow(CSCityTests_Box(FVector2D(-6000.0, 3500.0), FVector2D(2000.0, 1400.0)), 600.0),
		CSCity::Grow(CSCityTests_Box(FVector2D(8000.0, -4000.0), FVector2D(900.0, 900.0), 30.0), 500.0) };
	const TArray<CSCity::FAlley> Alleys = CSCity::LayoutAlleys(Region, Reserved, CSCity::FAlleyLayoutSettings());
	TArray<CSRoad::FSplineRoad> Splines;
	for (const CSCity::FAlley& Alley : Alleys)
	{
		// UE centimetres to the network's Unity metres, as ACSRoadNetworkActor gathers them.
		const FVector3d A(Alley.A.X / 100.0, 0.0, -Alley.A.Y / 100.0);
		const FVector3d B(Alley.B.X / 100.0, 0.0, -Alley.B.Y / 100.0);
		CSRoad::FSplineRoad& Road = Splines.AddDefaulted_GetRef();
		Road.Name = FString::Printf(TEXT("alley %d"), Splines.Num() - 1);
		Road.Prefab = TEXT("Alley");
		Road.bAlley = true;
		Road.Points = { A, B };
		Road.Arrive = { B - A, B - A };
		Road.Leave = { B - A, B - A };
		Road.Linear = { true, true };
	}
	TArray<CSRoad::FSavedNode> Nodes;
	TArray<CSRoad::FRoadSpec> Roads;
	CSRoad::FSplineNetworkStats Stats;
	FString Error;
	if (!TestTrue(TEXT("the alleys build into a network"), CSRoad::BuildSplineNetwork(Lib.Assets, Splines, CSRoad::FSplineNetworkSettings(), nullptr, Nodes, Roads, Stats, Error))) return false;
	TArray<int32> Degree;
	Degree.Init(0, Nodes.Num());
	for (const CSRoad::FRoadSpec& Road : Roads)
	{
		Degree[Road.SourceStart]++;
		Degree[Road.SourceEnd]++;
	}
	int32 DeadEnds = 0;
	for (int32 N = 0; N < Nodes.Num(); N++)
	{
		if (Degree[N] != 1) continue;
		if (DeadEnds++ < 6) AddInfo(FString::Printf(TEXT("dead end at (%.2f, %.2f) cm"), Nodes[N].Position.x * 100.0, -Nodes[N].Position.z * 100.0));
	}
	AddInfo(FString::Printf(TEXT("%d alleys: %d nodes, %d edges, %d junctions joined"), Alleys.Num(), Nodes.Num(), Roads.Num(), Stats.Junctions));
	TestEqual(TEXT("dead ends"), DeadEnds, 0);
	return true;
}

// Alleys cut back to a road along x (8 m wide, reach 5 m: covered within 9 m of its centre line) and one
// on a bridge 10 m up: one crossing straight over meets it at one point from both sides; one along it
// within reach is gone, one out of reach is kept; one ending past it ends on it; one crossing at 15
// degrees stops where the cover starts; the bridge cuts nothing; a piece shorter than 15 m is dropped.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityAlleyCutsTest, "PCGPlugins.CSRoadNet.City.AlleyCuts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityAlleyCutsTest::RunTest(const FString& Parameters)
{
	CSCity::FRoadLine Road;
	Road.HalfWidth = 400.0;
	Road.Points = { FVector(-10000.0, 0.0, 0.0), FVector(0.0, 0.0, 0.0), FVector(10000.0, 0.0, 0.0) };
	CSCity::FRoadLine Bridge;
	Bridge.HalfWidth = 400.0;
	Bridge.Points = { FVector(6000.0, -10000.0, 1000.0), FVector(6000.0, 10000.0, 1000.0) };
	auto Cut = [&](const FVector2D& A, const FVector2D& B) { return CSCity::CutAlleys({ { A, B } }, { Road, Bridge }, 500.0, 0.0, 200.0, 1500.0); };
	auto Near = [](const FVector2D& P, const FVector2D& Q) { return FVector2D::Distance(P, Q) < 1.0; };

	const TArray<CSCity::FAlley> Crossing = Cut(FVector2D(0.0, -5000.0), FVector2D(0.0, 5000.0));
	TestTrue(TEXT("crossing straight over: two pieces meeting on the centre line"), Crossing.Num() == 2 && Near(Crossing[0].B, FVector2D::ZeroVector) && Near(Crossing[1].A, FVector2D::ZeroVector));
	TestEqual(TEXT("along it within reach: pieces"), Cut(FVector2D(-5000.0, 600.0), FVector2D(5000.0, 600.0)).Num(), 0);
	const TArray<CSCity::FAlley> Clear = Cut(FVector2D(-5000.0, 2000.0), FVector2D(5000.0, 2000.0));
	TestTrue(TEXT("along it out of reach: kept as it is"), Clear.Num() == 1 && Near(Clear[0].A, FVector2D(-5000.0, 2000.0)) && Near(Clear[0].B, FVector2D(5000.0, 2000.0)));
	const TArray<CSCity::FAlley> Past = Cut(FVector2D(3000.0, -5000.0), FVector2D(3000.0, 300.0));
	TestTrue(TEXT("ending past it: ends on the centre line"), Past.Num() == 1 && Near(Past[0].B, FVector2D(3000.0, 0.0)));
	const double C = FMath::Cos(FMath::DegreesToRadians(15.0)) * 6000.0;
	const double S = FMath::Sin(FMath::DegreesToRadians(15.0)) * 6000.0;
	const TArray<CSCity::FAlley> Flat = Cut(FVector2D(-2000.0 - C, -S), FVector2D(-2000.0 + C, S));
	TestTrue(TEXT("crossing at 15 degrees: two pieces stopping at the cover"), Flat.Num() == 2 && FMath::Abs(Flat[0].B.Y + 900.0) < 1.0 && FMath::Abs(Flat[1].A.Y - 900.0) < 1.0);
	TestEqual(TEXT("under the bridge: pieces"), Cut(FVector2D(4000.0, 3000.0), FVector2D(8000.0, 3000.0)).Num(), 1);
	const TArray<CSCity::FAlley> Short = Cut(FVector2D(-9000.0, 1300.0), FVector2D(-9000.0, -5000.0));
	TestTrue(TEXT("a short piece dropped"), Short.Num() == 1 && Near(Short[0].A, FVector2D(-9000.0, 0.0)) && Near(Short[0].B, FVector2D(-9000.0, -5000.0)));
	return true;
}

// A region laid out over a small road already there: no alley runs under the road (within both half widths
// and the clearance of its centre line) but where its end joins it on the centre line, and some do. The
// road a main road instead, the alleys are laid out over it: the network cuts them while it lies there.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityRegionCutTest, "PCGPlugins.CSRoadNet.City.RegionCut",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityRegionCutTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	ACSRoadSplineActor* Small = World->SpawnActor<ACSRoadSplineActor>();
	ACSCityAlleyRegion* Region = World->SpawnActor<ACSCityAlleyRegion>();
	if (!TestNotNull(TEXT("network actor"), Network) || !TestNotNull(TEXT("small road"), Small) || !TestNotNull(TEXT("alley region"), Region)) return false;
	Small->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Small->Spline->Prefab = TEXT("Alley");
	Small->Spline->Tier = ECSRoadTier::Alley;
	const FVector2D From(-3000.0, -12000.0);
	const FVector2D To(-2000.0, 12000.0);
	Small->Spline->SetSplinePoints({ FVector(From, 0.0), FVector(To, 0.0) }, ESplineCoordinateSpace::Local);
	Region->Bounds->SetBoxExtent(FVector(12000.0, 8000.0, 500.0));
	const double Limit = ACSRoadNetworkActor::RoadWidth(Network, TEXT("Alley")) + double(Network->SplineSettings.AlleyClearance) * 100.0;
	if (!TestTrue(TEXT("alley width"), Limit > 100.0)) return false;

	// Alley samples under the road away from an end joining it, and the alleys joining it.
	auto Count = [&](int32& OutJoined)
	{
		int32 Under = 0;
		OutJoined = 0;
		for (const ACSRoadSplineActor* Alley : Region->Generated)
		{
			if (!Alley) continue;
			const FVector2D A(Alley->Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
			const FVector2D B(Alley->Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World));
			TArray<FVector2D> Joined;
			for (const FVector2D& End : { A, B }) if (FVector2D::Distance(End, FMath::ClosestPointOnSegment2D(End, From, To)) < 1.0) Joined.Add(End);
			OutJoined += Joined.Num() > 0 ? 1 : 0;
			for (int32 K = 0; K <= 100; K++)
			{
				const FVector2D P = FMath::Lerp(A, B, K / 100.0);
				if (FVector2D::Distance(P, FMath::ClosestPointOnSegment2D(P, From, To)) > Limit - 1.0) continue;
				Under += Joined.ContainsByPredicate([&](const FVector2D& End) { return FVector2D::Distance(P, End) < Limit * 2.0 + 100.0; }) ? 0 : 1;
			}
		}
		return Under;
	};
	Region->Generate();
	int32 Joined = 0;
	TestEqual(TEXT("small road: alley samples under it"), Count(Joined), 0);
	TestTrue(TEXT("small road: alleys joining it"), Joined >= 2);
	AddInfo(FString::Printf(TEXT("small road: %d alleys, %d joining it"), Region->Generated.Num(), Joined));
	Small->Spline->Tier = ECSRoadTier::Main;
	Region->Generate();
	TestTrue(TEXT("main road: alleys laid out under it"), Count(Joined) > 0);
	return true;
}

// Plots along two crossing streets: clear of the streets (by half the setback), of a reserved plot
// and of each other, and the same for the same inputs. A street added 150 m away takes plots along itself but leaves every
// plot more than 50 m from it as it was; taken away again, the first layout is back.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLotsTest, "PCGPlugins.CSRoadNet.City.Lots",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityLotsTest::RunTest(const FString& Parameters)
{
	const TArray<CSCity::FRoadLine> Streets = {
		CSCityTests_Street(1, FVector(-20000.0, 0.0, 0.0), FVector(20000.0, 0.0, 0.0), 400.0),
		CSCityTests_Street(2, FVector(0.0, -20000.0, 0.0), FVector(500.0, 20000.0, 0.0), 600.0) };
	const TArray<CSCity::FPolygon> Reserved = { CSCityTests_Box(FVector2D(6000.0, 2500.0), FVector2D(1500.0, 1500.0), 20.0) };
	CSCity::FLotSettings Settings;
	const TArray<CSCity::FLot> Lots = CSCity::LayoutLots(Streets, Streets, Reserved, Settings);
	TestTrue(TEXT("plots taken"), Lots.Num() >= 40);

	int32 OnStreet = 0;
	int32 OnReserved = 0;
	int32 Overlapping = 0;
	for (int32 I = 0; I < Lots.Num(); I++)
	{
		for (const CSCity::FRoadLine& Street : Streets) for (const FVector& P : Street.Points) OnStreet += CSCityTests_Distance(Lots[I].Corners, FVector2D(P)) < Street.HalfWidth + Settings.Setback * 0.5 - 1.0 ? 1 : 0;
		OnReserved += CSCity::Overlap(Lots[I].Corners, Reserved[0]) ? 1 : 0;
		for (int32 J = I + 1; J < Lots.Num(); J++) Overlapping += CSCity::Overlap(Lots[I].Corners, Lots[J].Corners) ? 1 : 0;
	}
	TestEqual(TEXT("plots within half the setback of a street"), OnStreet, 0);
	TestEqual(TEXT("plots on the reserved plot"), OnReserved, 0);
	TestEqual(TEXT("plots overlapping each other"), Overlapping, 0);
	TestTrue(TEXT("the same inputs give the same plots"), CSCityTests_SameLots(Lots, CSCity::LayoutLots(Streets, Streets, Reserved, Settings)));

	TArray<CSCity::FRoadLine> More = Streets;
	More.Add(CSCityTests_Street(3, FVector(-20000.0, 15000.0, 0.0), FVector(20000.0, 15000.0, 0.0), 600.0));
	const TArray<CSCity::FLot> After = CSCity::LayoutLots(More, More, Reserved, Settings);
	TMap<uint64, const CSCity::FLot*> ById;
	for (const CSCity::FLot& Lot : After) ById.Add(Lot.Id, &Lot);
	auto Near = [](const CSCity::FLot& Lot)
	{
		for (const FVector2D& Corner : Lot.Corners) if (FMath::Abs(Corner.Y - 15000.0) < 5000.0) return true;
		return false;
	};
	int32 Kept = 0;
	int32 Changed = 0;
	for (const CSCity::FLot& Lot : Lots)
	{
		if (Near(Lot)) continue;
		const CSCity::FLot* Same = ById.FindRef(Lot.Id);
		if (Same && Same->Corners == Lot.Corners) Kept++;
		else Changed++;
	}
	TSet<uint64> Before;
	for (const CSCity::FLot& Lot : Lots) Before.Add(Lot.Id);
	int32 Appeared = 0;
	int32 AlongNew = 0;
	for (const CSCity::FLot& Lot : After)
	{
		if (Before.Contains(Lot.Id)) continue;
		if (Near(Lot)) AlongNew++;
		else Appeared++;
	}
	TestTrue(TEXT("street added: plots away from it kept"), Kept > 20);
	TestEqual(TEXT("street added: plots away from it changed or gone"), Changed, 0);
	TestEqual(TEXT("street added: plots away from it appeared"), Appeared, 0);
	TestTrue(TEXT("street added: plots along it"), AlongNew > 10);
	TestTrue(TEXT("street taken away: the first plots are back"), CSCityTests_SameLots(Lots, CSCity::LayoutLots(Streets, Streets, Reserved, Settings)));
	return true;
}

// An 80 m block closed in by four streets. Filled, its plots run back halfway across the block and cover far more
// of it than plots of their own depth. Cut north to south by a 3 m lane whose plots stand half a metre from it, it
// has plots along both sides of the lane facing it, and the street plots run back halfway to the lane's. Either way
// every plot keeps half a road's setback from every road and a Gap from every other plot, and the same inputs give
// the same plots.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLotsFillTest, "PCGPlugins.CSRoadNet.City.LotsFill",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityLotsFillTest::RunTest(const FString& Parameters)
{
	TArray<CSCity::FRoadLine> Streets = {
		CSCityTests_Street(1, FVector(-4000.0, 0.0, 0.0), FVector(12000.0, 0.0, 0.0), 400.0),
		CSCityTests_Street(2, FVector(-4000.0, 8000.0, 0.0), FVector(12000.0, 8000.0, 0.0), 400.0),
		CSCityTests_Street(3, FVector(0.0, -4000.0, 0.0), FVector(0.0, 12000.0, 0.0), 400.0),
		CSCityTests_Street(4, FVector(8000.0, -4000.0, 0.0), FVector(8000.0, 12000.0, 0.0), 400.0) };
	TArray<CSCity::FRoadLine> WithLane = Streets;
	WithLane.Add(CSCityTests_Street(5, FVector(4000.0, 0.0, 0.0), FVector(4000.0, 8000.0, 0.0), 150.0));
	WithLane.Last().Setback = 50.0;
	CSCity::FLotSettings Settings;
	const FBox2D Block(FVector2D(0.0, 0.0), FVector2D(8000.0, 8000.0));

	auto Distance = [](const CSCity::FPolygon& A, const CSCity::FPolygon& B)
	{
		if (CSCity::Overlap(A, B)) return 0.0;
		double Best = TNumericLimits<double>::Max();
		for (const CSCity::FPolygon* From : { &A, &B }) for (const FVector2D& P : *From) Best = FMath::Min(Best, CSCityTests_Distance(From == &A ? B : A, P));
		return Best;
	};
	auto Area = [](const CSCity::FPolygon& Polygon)
	{
		double Twice = 0.0;
		for (int32 I = 0; I < Polygon.Num(); I++) Twice += Polygon[I].X * Polygon[(I + 1) % Polygon.Num()].Y - Polygon[(I + 1) % Polygon.Num()].X * Polygon[I].Y;
		return Twice * 0.5;
	};
	auto Covered = [&Block, &Area](const TArray<CSCity::FLot>& Lots)
	{
		double Sum = 0.0;
		for (const CSCity::FLot& Lot : Lots) if (Block.IsInside(Lot.Centre)) Sum += Area(Lot.Corners);
		return Sum / Block.GetArea();
	};
	// Clear of the roads and of each other.
	auto Spaced = [&](const FString& Case, const TArray<CSCity::FRoadLine>& Roads, const TArray<CSCity::FLot>& Lots)
	{
		int32 OnRoad = 0;
		int32 TooNear = 0;
		for (int32 I = 0; I < Lots.Num(); I++)
		{
			for (const CSCity::FRoadLine& Road : Roads)
			{
				const double Keep = Road.HalfWidth + (Road.Setback >= 0.0 ? Road.Setback : Settings.Setback) * 0.5 - 1.0;
				for (const FVector& P : Road.Points) OnRoad += CSCityTests_Distance(Lots[I].Corners, FVector2D(P)) < Keep ? 1 : 0;
			}
			for (int32 J = I + 1; J < Lots.Num(); J++) TooNear += Distance(Lots[I].Corners, Lots[J].Corners) < Settings.Gap - 1.0 ? 1 : 0;
		}
		TestEqual(Case + TEXT(": plots within half a road's setback of it"), OnRoad, 0);
		TestEqual(Case + TEXT(": plots nearer each other than a Gap"), TooNear, 0);
	};

	// The block alone.
	const TArray<CSCity::FLot> Filled = CSCity::LayoutLots(Streets, Streets, {}, Settings);
	TestTrue(TEXT("block: the same inputs give the same plots"), CSCityTests_SameLots(Filled, CSCity::LayoutLots(Streets, Streets, {}, Settings)));
	Spaced(TEXT("block"), Streets, Filled);
	double Middle = 0.0;
	for (const CSCity::FLot& Lot : Filled) if (Block.IsInside(Lot.Centre) && FMath::Abs(Lot.Front.Y + 1.0) < 1e-6) for (const FVector2D& Corner : Lot.Corners) Middle = FMath::Max(Middle, Corner.Y);
	// Towards the middle of the block, where the four streets' ground meets (40 m), half a Gap short of the diagonals;
	// unfilled plots reach 25 m at most.
	TestTrue(FString::Printf(TEXT("block: south street plots run back near the middle (to %.0f cm)"), Middle), Middle > 3500.0 && Middle <= 3901.0);
	Settings.bFill = false;
	const TArray<CSCity::FLot> Unfilled = CSCity::LayoutLots(Streets, Streets, {}, Settings);
	Settings.bFill = true;
	AddInfo(FString::Printf(TEXT("block: %.0f%% covered filled, %.0f%% not"), Covered(Filled) * 100.0, Covered(Unfilled) * 100.0));
	TestTrue(TEXT("block: filled plots cover far more of it"), Covered(Filled) > Covered(Unfilled) + 0.1);

	// The block with the lane.
	const TArray<CSCity::FLot> Lots = CSCity::LayoutLots(WithLane, WithLane, {}, Settings);
	Spaced(TEXT("lane"), WithLane, Lots);
	int32 AlongLane = 0;
	double Deepest = 0.0;
	for (const CSCity::FLot& Lot : Lots)
	{
		if (!Block.IsInside(Lot.Centre)) continue;
		// A lane plot: its front on the lane, half a metre from its edge, facing it.
		const FVector2D FrontMiddle = Lot.Centre + Lot.Front * (Lot.Depth * 0.5);
		if (FMath::Abs(FMath::Abs(FrontMiddle.X - 4000.0) - 200.0) < 1.0 && FMath::Abs(Lot.Front.Y) < 1e-6) AlongLane++;
		// How far a west street plot (west of the lane, facing west) runs back towards the lane.
		if (Lot.Centre.X < 4000.0 && FMath::Abs(Lot.Front.X + 1.0) < 1e-6) for (const FVector2D& Corner : Lot.Corners) Deepest = FMath::Max(Deepest, Corner.X);
	}
	AddInfo(FString::Printf(TEXT("lane: %d plots along it, %.0f%% of the block covered"), AlongLane, Covered(Lots) * 100.0));
	TestTrue(TEXT("lane: plots along both sides of it"), AlongLane >= 6);
	// Halfway between the west street's plot fronts (7 m) and the lane's (38 m), half a Gap short.
	TestTrue(FString::Printf(TEXT("lane: west street plots run back halfway to the lane's (to %.0f cm)"), Deepest), Deepest > 2100.0 && Deepest <= 2151.0);
	return true;
}

// A street grid with a diagonal, wide and narrow streets and reserved plots, laid out whole, then only
// from the inputs within reach (CSCity::AnchorReach and the like) of each of several windows: every plot
// centred in a window comes out the same, id, corners, base and height.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLotsWindowsTest, "PCGPlugins.CSRoadNet.City.LotsWindows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityLotsWindowsTest::RunTest(const FString& Parameters)
{
	TArray<CSCity::FRoadLine> Streets;
	for (int32 I = 0; I < 5; I++) Streets.Add(CSCityTests_Street(10 + I, FVector(-30000.0, -24000.0 + I * 12000.0, 0.0), FVector(30000.0, -24000.0 + I * 12000.0 + 800.0 * I, 0.0), I == 2 ? 700.0 : 400.0));
	for (int32 I = 0; I < 5; I++) Streets.Add(CSCityTests_Street(20 + I, FVector(-24000.0 + I * 12000.0, -30000.0, 0.0), FVector(-24000.0 + I * 12000.0 - 600.0 * I, 30000.0, 0.0), 400.0));
	Streets.Add(CSCityTests_Street(30, FVector(-30000.0, -30000.0, 0.0), FVector(30000.0, 26000.0, 0.0), 600.0));
	const TArray<CSCity::FPolygon> Reserved = {
		CSCityTests_Box(FVector2D(6000.0, 6000.0), FVector2D(2500.0, 1500.0), 15.0),
		CSCityTests_Box(FVector2D(-13000.0, -5000.0), FVector2D(1800.0, 1800.0), 40.0),
		CSCityTests_Box(FVector2D(18000.0, -17000.0), FVector2D(3000.0, 1200.0)) };
	CSCity::FLotSettings Settings;
	const TArray<CSCity::FLot> Whole = CSCity::LayoutLots(Streets, Streets, Reserved, Settings);
	TestTrue(TEXT("plots laid out"), Whole.Num() > 200);

	auto Bounds = [](const CSCity::FRoadLine& Line)
	{
		FBox2D Box(ForceInit);
		for (const FVector& P : Line.Points) Box += FVector2D(P);
		return Box;
	};
	auto Same = [](const CSCity::FLot& A, const CSCity::FLot& B) { return A.Id == B.Id && A.Corners == B.Corners && A.Base == B.Base && A.Height == B.Height; };
	const FBox2D Windows[] = {
		FBox2D(FVector2D(-6000.0, -6000.0), FVector2D(6000.0, 6000.0)),
		FBox2D(FVector2D(2000.0, 3000.0), FVector2D(9000.0, 9000.0)),
		FBox2D(FVector2D(-30000.0, -30000.0), FVector2D(-18000.0, -20000.0)),
		FBox2D(FVector2D(11000.0, -23000.0), FVector2D(25000.0, -11000.0)),
		FBox2D(FVector2D(-1000.0, -26000.0), FVector2D(1000.0, 26000.0)) };
	for (int32 W = 0; W < UE_ARRAY_COUNT(Windows); W++)
	{
		const FBox2D& Window = Windows[W];
		TArray<CSCity::FRoadLine> Anchors;
		TArray<CSCity::FRoadLine> Roads;
		TArray<CSCity::FPolygon> Plots;
		for (const CSCity::FRoadLine& Street : Streets)
		{
			if (Bounds(Street).ExpandBy(CSCity::AnchorReach(Settings, Street.HalfWidth)).Intersect(Window)) Anchors.Add(Street);
			if (Bounds(Street).ExpandBy(CSCity::RoadReach(Settings, Street.HalfWidth)).Intersect(Window)) Roads.Add(Street);
		}
		for (const CSCity::FPolygon& Plot : Reserved) if (FBox2D(Plot).ExpandBy(CSCity::ReservedReach(Settings)).Intersect(Window)) Plots.Add(Plot);
		const TArray<CSCity::FLot> Local = CSCity::LayoutLots(Anchors, Roads, Plots, Settings);
		TMap<uint64, const CSCity::FLot*> LocalById;
		for (const CSCity::FLot& Lot : Local) if (Window.IsInside(Lot.Centre)) LocalById.Add(Lot.Id, &Lot);
		int32 Inside = 0;
		int32 Different = 0;
		for (const CSCity::FLot& Lot : Whole)
		{
			if (!Window.IsInside(Lot.Centre)) continue;
			Inside++;
			const CSCity::FLot* Mine = LocalById.FindRef(Lot.Id);
			Different += Mine && Same(*Mine, Lot) ? 0 : 1;
		}
		const FString Case = FString::Printf(TEXT("window %d"), W);
		TestTrue(Case + TEXT(": plots in it"), Inside > 0);
		TestEqual(Case + TEXT(": plots the window lays out differently"), Different, 0);
		TestEqual(Case + TEXT(": plots only the window lays out"), LocalById.Num(), Inside);
		AddInfo(FString::Printf(TEXT("%s: %d plots from %d of %d streets and %d of %d reserved plots"), *Case, Inside, Roads.Num(), Streets.Num(), Plots.Num(), Reserved.Num()));
	}
	return true;
}

// The city on a network built in tiles: a special building placed first, alleys laid out around it,
// batch buildings along every road. The alleys and the batch buildings keep out of the special
// building's plot; an unchanged rebuild writes no building tile; a main road drawn over a corner
// rewrites the building tiles there and leaves the others alone; the special building moved, the
// batch buildings make way.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityActorTest, "PCGPlugins.CSRoadNet.City.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityActorTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->bLocalUpdate = true;
	Network->TileSize = 128.0f;
	Network->bBatchBuildings = true;
	// Every tile in one rebuild: the counts below are per rebuild, whatever else is using the video memory.
	Network->MaxTilesPerBatch = 0;
	Network->MinFreeVideoMemory = 0;

	ACSCityBuildingActor* Special = World->SpawnActor<ACSCityBuildingActor>(FVector(-3000.0, 2000.0, 0.0), FRotator(0.0, 20.0, 0.0));
	ACSCityAlleyRegion* Region = World->SpawnActor<ACSCityAlleyRegion>();
	if (!TestNotNull(TEXT("special building"), Special) || !TestNotNull(TEXT("alley region"), Region)) return false;
	TestTrue(TEXT("the special building belongs to the network"), ACSRoadNetworkActor::NetworkOf(Special) == Network);
	Region->Bounds->SetBoxExtent(FVector(12000.0, 8000.0, 500.0));
	Region->Generate();
	TestTrue(TEXT("alleys laid out"), Region->Generated.Num() >= 4);
	const CSCity::FPolygon AlleyKept = CSCity::Grow(Special->Reservation(), double(Region->Clearance) * 100.0 - 1.0);
	int32 Crossing = 0;
	for (const ACSRoadSplineActor* Alley : Region->Generated)
	{
		if (!Alley) continue;
		const FVector2D A(Alley->Spline->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World));
		const FVector2D B(Alley->Spline->GetLocationAtSplinePoint(1, ESplineCoordinateSpace::World));
		Crossing += CSCity::SegmentHits(AlleyKept, A, B) ? 1 : 0;
	}
	TestEqual(TEXT("alleys through the special building's plot"), Crossing, 0);

	// No batch building corner inside the special building's plot.
	auto InPlot = [&]()
	{
		const CSCity::FPolygon Plot = CSCity::Grow(Special->Reservation(), -1.0);
		int32 Count = 0;
		for (const FVector3f& P : CSRoadTest::BuildingCorners(Network)) Count += CSCity::Contains(Plot, FVector2D(P.X, P.Y)) ? 1 : 0;
		return Count;
	};
	Network->Rebuild();
	TestEqual(TEXT("built: error"), Network->LastError, FString());
	TestTrue(TEXT("built: roads"), Network->NumRoads > 0);
	TestTrue(TEXT("built: batch buildings"), Network->NumBuildings > 0);
	CSRoad::LandMeshWrites();
	int32 SpecialModules = 0;
	for (const UCSGpuInstancedMeshComponent* Component : Special->Facade) if (Component && Component->HasInstanceSourceGPU()) SpecialModules += Component->DebugReadDrawnInstanceCountSync();
	TestTrue(TEXT("built: the special building's modules"), SpecialModules > 0);
	TestEqual(TEXT("built: no building meshes"), CSCityTests_BuildingMeshes(Network), 0);
	TestEqual(TEXT("built: batch buildings in the special building's plot"), InPlot(), 0);
	const TMap<FString, uint32> Before = CSCityTests_BuildingSerials(Network);
	TestTrue(TEXT("built: several building tiles"), Before.Num() >= 3);
	AddInfo(FString::Printf(TEXT("%d alleys, %d roads in %d tiles, %d batch buildings in %d tiles"), Region->Generated.Num(), Network->NumRoads, Network->NumTiles, Network->NumBuildings, Before.Num()));

	Network->Rebuild();
	TestEqual(TEXT("unchanged: building tiles rebuilt"), Network->BuildingTilesRebuilt, 0);
	TestTrue(TEXT("unchanged: building meshes untouched"), CSCityTests_BuildingSerials(Network).OrderIndependentCompareEqual(Before));

	// A main road over the north-west corner of the region.
	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-16000.0, 3000.0, 0.0), FVector(-3000.0, 11000.0, 0.0) }, ESplineCoordinateSpace::Local);
	const int32 BuildingsBefore = Network->NumBuildings;
	CSRoadTest::TickBuild(Network);
	TestEqual(TEXT("main road: error"), Network->LastError, FString());
	TestTrue(TEXT("main road: some building tiles rebuilt"), Network->BuildingTilesRebuilt > 0);
	const TMap<FString, uint32> After = CSCityTests_BuildingSerials(Network);
	int32 Unchanged = 0;
	for (const TPair<FString, uint32>& Pair : Before) Unchanged += After.FindRef(Pair.Key) == Pair.Value ? 1 : 0;
	TestTrue(TEXT("main road: building tiles away from it untouched"), Unchanged >= 2);
	AddInfo(FString::Printf(TEXT("main road: %d -> %d batch buildings, %d building tiles rebuilt, %d of %d untouched"), BuildingsBefore, Network->NumBuildings, Network->BuildingTilesRebuilt, Unchanged, Before.Num()));

	// The special building moved east: the batch buildings make way.
	Special->SetActorLocation(FVector(6000.0, -3000.0, 0.0));
	Special->Rebuild();
	CSRoadTest::TickBuild(Network);
	TestEqual(TEXT("moved: error"), Network->LastError, FString());
	TestTrue(TEXT("moved: some building tiles rebuilt"), Network->BuildingTilesRebuilt > 0);
	TestEqual(TEXT("moved: batch buildings in the special building's plot"), InPlot(), 0);

	// Two building tiles a batch: a new seed changes every building, the next ticks write the rest,
	// and they end as one rebuild with no limit draws them.
	auto Sorted = [&]()
	{
		TArray<FVector3f> Vertices = CSRoadTest::BuildingCorners(Network);
		Vertices.Sort([](const FVector3f& A, const FVector3f& B) { return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z; });
		return Vertices;
	};
	Network->MaxTilesPerBatch = 2;
	Network->Lots.Seed = 7;
	Network->Rebuild();
	TestTrue(TEXT("batched: building tiles left pending"), Network->PendingTiles > 0);
	for (int32 Tick = 0; Tick < 50 && Network->PendingTiles > 0; Tick++) CSRoadTest::TickBuild(Network);
	TestEqual(TEXT("batched: tiles left pending"), Network->PendingTiles, 0);
	const TArray<FVector3f> Batched = Sorted();
	Network->MaxTilesPerBatch = 0;
	Network->bBatchBuildings = false;
	Network->Rebuild();
	Network->bBatchBuildings = true;
	Network->Rebuild();
	TestTrue(TEXT("batched: the same buildings as one rebuild"), Batched.Num() > 0 && Batched == Sorted());

	// Batch buildings switched off: their tiles go.
	Network->bBatchBuildings = false;
	Network->Rebuild();
	TestEqual(TEXT("switched off: building tiles"), CSCityTests_BuildingSerials(Network).Num(), 0);
	TestEqual(TEXT("switched off: batch buildings"), Network->NumBuildings, 0);
	return true;
}

// The batch buildings laid out again only near what an edit changed, on a turned network in small
// tiles: a main road drawn over the alleys, one of its points moved, the special building moved, the
// main road deleted. After each edit the buildings change and equal those of a layout from scratch;
// moving the special building writes only some of the building tiles.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSCityLocalUpdateTest, "PCGPlugins.CSRoadNet.City.LocalUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSCityLocalUpdateTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>(FVector(2000.0, -1500.0, 0.0), FRotator(0.0, 10.0, 0.0));
	Network->MinFreeVideoMemory = 0;
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->bLocalUpdate = true;
	Network->TileSize = 64.0f;
	Network->MaxTilesPerBatch = 0;
	Network->bBatchBuildings = true;
	ACSCityBuildingActor* Special = World->SpawnActor<ACSCityBuildingActor>(FVector(-3000.0, 2000.0, 0.0), FRotator(0.0, 20.0, 0.0));
	ACSCityAlleyRegion* Region = World->SpawnActor<ACSCityAlleyRegion>();
	if (!TestNotNull(TEXT("special building"), Special) || !TestNotNull(TEXT("alley region"), Region)) return false;
	Region->Bounds->SetBoxExtent(FVector(12000.0, 8000.0, 500.0));
	Region->Generate();
	Network->Rebuild();
	TestEqual(TEXT("built: error"), Network->LastError, FString());

	auto Sorted = [&]()
	{
		TArray<FVector3f> Vertices = CSRoadTest::BuildingCorners(Network);
		Vertices.Sort([](const FVector3f& A, const FVector3f& B) { return A.X != B.X ? A.X < B.X : A.Y != B.Y ? A.Y < B.Y : A.Z < B.Z; });
		return Vertices;
	};
	// Every building laid out from scratch: what the local layout must come out as.
	auto Scratch = [&]()
	{
		Network->bBatchBuildings = false;
		Network->Rebuild();
		Network->bBatchBuildings = true;
		Network->Rebuild();
		return Sorted();
	};
	TArray<FVector3f> Current = Sorted();
	TestTrue(TEXT("built: batch buildings"), Current.Num() > 0);
	auto Check = [&](const FString& Case)
	{
		CSRoadTest::TickBuild(Network);
		TestEqual(Case + TEXT(": error"), Network->LastError, FString());
		const int32 LaidOut = Network->BuildingTilesLaidOut;
		const int32 Written = Network->BuildingTilesRebuilt;
		const int32 Tiles = CSCityTests_BuildingSerials(Network).Num();
		const TArray<FVector3f> Local = Sorted();
		TestTrue(Case + TEXT(": the buildings changed"), Local != Current);
		TestTrue(Case + TEXT(": tiles laid out"), LaidOut > 0);
		TestTrue(Case + TEXT(": the same buildings as a layout from scratch"), Local == Scratch());
		AddInfo(FString::Printf(TEXT("%s: %d tiles laid out, %d of %d building tiles written"), *Case, LaidOut, Written, Tiles));
		Current = Local;
		return Written < Tiles;
	};

	ACSRoadSplineActor* Main = World->SpawnActor<ACSRoadSplineActor>();
	Main->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
	Main->Spline->Prefab = TEXT("Medium Road");
	Main->Spline->SetSplinePoints({ FVector(-16000.0, 3000.0, 0.0), FVector(-9000.0, 7000.0, 0.0), FVector(-3000.0, 11000.0, 0.0) }, ESplineCoordinateSpace::Local);
	Check(TEXT("main road drawn"));
	Main->Spline->SetLocationAtSplinePoint(1, FVector(-9500.0, 5500.0, 0.0), ESplineCoordinateSpace::Local);
	Check(TEXT("main road point moved"));
	Special->SetActorLocation(FVector(6000.0, -3000.0, 0.0));
	Special->Rebuild();
	TestTrue(TEXT("special building moved: only some building tiles written"), Check(TEXT("special building moved")));
	Main->Destroy();
	Check(TEXT("main road deleted"));
	return true;
}

#endif

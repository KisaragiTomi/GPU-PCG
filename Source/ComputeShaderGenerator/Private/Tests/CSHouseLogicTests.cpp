#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"       // House.BaseHandle —— 落座要一块地
#include "CSGroundShaperSteps.h"
#include "CSHouseActor.h"
#include "CSHouseDoorRuns.h"
#include "CSHouseProfile.h"
#include "CSHouseQuoin.h"
#include "CSHouseQuoinLayout.ush"
#include "CSHousePillar.h"
#include "CSHouseTrim.h"
#include "CSHouseBrickWall.h"
#include "CSHouseResize.h"
#include "CSHouseRoof.h"
#include "CSHouseSeam.h"
#include "CSHouseContact.h"   // D7 接触记录
#include "Algo/Reverse.h"
#include "CSHouseFeatureMarker.h"
#include "CSHouseHeightHandleActor.h"
#include "CSHouseResizeHandleActor.h"
#include "CSHouseLibrary.h"
#include "CSSplineBlockActor.h"
#include "Components/StaticMeshComponent.h"   // House.BaseHandle 数框的四根条子
#include "Engine/StaticMesh.h"   // House.WindowMarker 里 LoadObject<UStaticMesh> 要完整类型
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"
#include "Math/NumericLimits.h"
#include "Math/RandomStream.h"
#include "UObject/Class.h"   // HasAnyClassFlags / GetBoolMetaDataHierarchical（unity 构建会替你藏起来）
#include "UObject/UnrealType.h"   // FindFProperty —— House.ContactStackedPostsPerPair 给私有的 HouseId 定序
#include "Editor.h"                              // GEditor —— House.DuplicateGetsOwnId 走编辑器复制
#include "Subsystems/EditorActorSubsystem.h"     // DuplicateActor = Ctrl+D / Alt 拖同一条 T3D 路
#include "HAL/PlatformProcess.h"                 // FPlatformProcess::Sleep —— House.ContactDragTimeoutCommits

// -----------------------------------------------------------------------------
// TinyGladeHouse 的判定纯函数用例：不碰 RHI / world，只钉数学 ——
// 屋面求值器（四坡，唯一真源，瓦/梁/落窗谓词都调它）、边缘分割、离地收窄。
//
// 计划纪律（Plan_House.md 阶段计划）：门洞区间、接触段、柱布点、openings 排布
// 这类判定全部做成无 GPU 依赖的纯函数 + automation 测试。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSHouseTest_ prefix
// （与 CSHouseActor.cpp 内的 CSHouse_ 前缀必须不同，否则 unity blob 里同名符号打架）。

FCSRoofDesc CSHouseTest_MakeRoof(double SizeX, double SizeY)
{
	FCSRoofDesc Desc;
	Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(SizeX, SizeY));
	Desc.EaveZ = 300.0f;
	Desc.Pitch = 35.0f;
	Desc.Overhang = 25.0f;
	return Desc;
}
}

// -----------------------------------------------------------------------------
// 屋面求值器（四坡）：高度场 = 内距 × 坡度，脊长与角斜脊都是它的推论
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofEvalTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofEval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofEvalTest::RunTest(const FString& Parameters)
{
	// 600 × 400 ⇒ 脊沿长轴 X，跨度是 Y（半跨 200）。
	const FCSRoofDesc Roof = CSHouseTest_MakeRoof(600.0, 400.0);
	const float TanP = Roof.TanPitch();

	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Roof.Footprint, Roof.Overhang, Skeleton);
	TestTrue(TEXT("The ridge follows the long axis"),
		FMath::IsNearlyZero(Skeleton.TopA.Y, 1.0e-6) && FMath::IsNearlyZero(Skeleton.TopB.Y, 1.0e-6) && Skeleton.TopB.X > Skeleton.TopA.X);
	TestTrue(TEXT("The highest inset is half the 400 side"), FMath::IsNearlyEqual(Roof.MaxInset(), 200.0, 1.0e-6));
	// 等坡度四坡的推论：两端的坡面各吃掉半跨，脊线只剩 |X − Y|。**不是**长边的长度。
	TestTrue(TEXT("The ridge segment is the aspect difference"),
		FMath::IsNearlyEqual(FVector2D::Distance(Skeleton.TopA, Skeleton.TopB), 200.0, 1.0e-4));

	// 屋脊高只由**短边**决定：脊线上的内距恰好是半跨。
	TestTrue(TEXT("Ridge height"), FMath::IsNearlyEqual(CSHouseRoof_RidgeZ(Roof), 300.0f + TanP * 200.0f, 1.0e-3f));

	// **四条** footprint 边上屋面都恰好落在墙顶 —— 双坡时只有两条成立（另两条是山墙）。
	TestTrue(TEXT("Roof meets the wall top on +Y"), FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 200.0)), 300.0f, 1.0e-3f));
	TestTrue(TEXT("Roof meets the wall top on -Y"), FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, -200.0)), 300.0f, 1.0e-3f));
	TestTrue(TEXT("Roof meets the wall top on +X"), FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(300.0, 0.0)), 300.0f, 1.0e-3f));
	TestTrue(TEXT("Roof meets the wall top on -X"), FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(-300.0, 0.0)), 300.0f, 1.0e-3f));

	TestTrue(TEXT("Symmetric across the ridge"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, -137.0)), CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 137.0)), 1.0e-3f));

	// 檐口外沿继续往下走 overhang × tan。
	TestTrue(TEXT("Eave outer height"),
		FMath::IsNearlyEqual(CSHouseRoof_EaveOuterZ(Roof), 300.0f - TanP * 25.0f, 1.0e-3f));

	// 脊线**段内**是平的；出了脊端点（|沿脊| > 100）就跟着两端的坡面往下走 ——
	// 这一条正是四坡与双坡的分界，双坡在整条长轴上都是平的。
	TestTrue(TEXT("Flat along the ridge segment"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(60.0, 0.0)), CSHouseRoof_RidgeZ(Roof), 1.0e-3f));
	TestTrue(TEXT("Past the ridge end the roof falls away"),
		CSHouseRoof_EvalZ(Roof, FVector2D(250.0, 0.0)) < CSHouseRoof_RidgeZ(Roof) - 1.0f);

	// 角斜脊落在 45° 对角线上：300 − |x| = 200 − |y| ⇒ (200, 100) 的内距是 100。
	TestTrue(TEXT("The corner hip line is the 45 degree diagonal"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(200.0, 100.0)), 300.0f + TanP * 100.0f, 1.0e-3f));

	// 正方形连续退化成金字塔：脊长 0，尖点在中心。
	const FCSRoofDesc Square = CSHouseTest_MakeRoof(400.0, 400.0);
	FCSRoofSkeleton SquareSkeleton;
	CSHouseRoof_BuildSkeleton(Square.Footprint, Square.Overhang, SquareSkeleton);
	TestTrue(TEXT("A square roof degenerates to a pyramid"),
		FVector2D::Distance(SquareSkeleton.TopA, SquareSkeleton.TopB) < 1.0e-4 && SquareSkeleton.TopA.Equals(FVector2D::ZeroVector, 1.0e-4));
	TestTrue(TEXT("The pyramid apex is at the centre"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Square, FVector2D(0.0, 0.0)), 300.0f + Square.TanPitch() * 200.0f, 1.0e-3f));

	// 转 90°：脊向跟着长轴走，短边没变 ⇒ 脊高不变，且高度场整体就是转置。
	const FCSRoofDesc RoofY = CSHouseTest_MakeRoof(400.0, 600.0);
	FCSRoofSkeleton SkeletonY;
	CSHouseRoof_BuildSkeleton(RoofY.Footprint, RoofY.Overhang, SkeletonY);
	TestTrue(TEXT("The ridge follows the long axis after the swap"),
		FMath::IsNearlyZero(SkeletonY.TopA.X, 1.0e-6) && FMath::IsNearlyZero(SkeletonY.TopB.X, 1.0e-6) && SkeletonY.TopB.Y > SkeletonY.TopA.Y);
	TestTrue(TEXT("Ridge height is unchanged by the swap"),
		FMath::IsNearlyEqual(CSHouseRoof_RidgeZ(RoofY), CSHouseRoof_RidgeZ(Roof), 1.0e-3f));
	TestTrue(TEXT("The height field is the transpose"),
		FMath::IsNearlyEqual(CSHouseRoof_EvalZ(RoofY, FVector2D(90.0, -280.0)), CSHouseRoof_EvalZ(Roof, FVector2D(-280.0, 90.0)), 1.0e-3f));

	return true;
}

// -----------------------------------------------------------------------------
// 屋面法线与覆盖谓词：四个坡面 + 四条角斜脊 + 脊线 + 金字塔尖
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofNormalTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofNormal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofNormalTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTest_MakeRoof(600.0, 400.0);

	// 跨脊那两个坡面（长边侧）。
	const FVector NPos = CSHouseRoof_EvalNormal(Roof, FVector2D(0.0, 120.0));
	const FVector NNeg = CSHouseRoof_EvalNormal(Roof, FVector2D(0.0, -120.0));

	TestTrue(TEXT("Normals are unit length"), FMath::IsNearlyEqual(NPos.Size(), 1.0, 1.0e-4));
	TestTrue(TEXT("Normal points up"), NPos.Z > 0.0);
	TestTrue(TEXT("Normal leans outward on the +span side"), NPos.Y > 0.0);
	TestTrue(TEXT("The two slopes mirror each other"), FMath::IsNearlyEqual(NPos.Y, -NNeg.Y, 1.0e-4));
	TestTrue(TEXT("Normal is flat along the ridge axis"), FMath::IsNearlyZero(NPos.X, 1.0e-4));

	// 法线与屋面斜率一致：坡面切向 · 法线 = 0。切向沿 +跨度 是 (0, 1, -tan)。
	const FVector Tangent = FVector(0.0, 1.0, -Roof.TanPitch()).GetSafeNormal();
	TestTrue(TEXT("Normal is perpendicular to the slope"), FMath::IsNearlyZero(FVector::DotProduct(Tangent, NPos), 1.0e-4));

	// **两端那两个坡面是四坡独有的**（双坡时那儿是山墙，法线是水平的）。
	const FVector NEnd = CSHouseRoof_EvalNormal(Roof, FVector2D(280.0, 0.0));
	TestTrue(TEXT("The end slope leans along the ridge axis"), NEnd.X > 0.0 && NEnd.Z > 0.0);
	TestTrue(TEXT("The end slope has no cross-ridge lean"), FMath::IsNearlyZero(NEnd.Y, 1.0e-4));

	// 脊线上两坡对冲 ⇒ 正上。
	TestTrue(TEXT("On the ridge the two slopes cancel to straight up"),
		CSHouseRoof_EvalNormal(Roof, FVector2D(50.0, 0.0)).Equals(FVector::UpVector, 1.0e-4));

	// 角斜脊上一个 X 面与一个 Y 面并列 ⇒ 两个水平分量相等（法线落在对角竖直面里）。
	const FVector NHip = CSHouseRoof_EvalNormal(Roof, FVector2D(200.0, 100.0));
	TestTrue(TEXT("The corner hip normal splits the two faces evenly"),
		NHip.X > 0.0 && FMath::IsNearlyEqual(NHip.X, NHip.Y, 1.0e-4));

	// 金字塔尖：四面对冲 ⇒ 正上。
	const FCSRoofDesc Square = CSHouseTest_MakeRoof(400.0, 400.0);
	TestTrue(TEXT("The pyramid apex normal is straight up"),
		CSHouseRoof_EvalNormal(Square, FVector2D(0.0, 0.0)).Equals(FVector::UpVector, 1.0e-4));

	// 覆盖谓词：四面外挑都算（半跨 200 + 25，长轴半长 300 + 25）。
	TestTrue(TEXT("Inside the footprint is under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 0.0)));
	TestTrue(TEXT("The overhang counts as under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 220.0)));
	TestFalse(TEXT("Past the overhang is not under the roof"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 240.0)));
	TestTrue(TEXT("Under the end overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(320.0, 0.0)));
	TestFalse(TEXT("Past the end overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(340.0, 0.0)));

	return true;
}

// -----------------------------------------------------------------------------
// 屋面折线化（footprint 3e）：凸折线的直骨架 = 等坡度四坡屋面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofPolylineSkeletonTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofPolylineSkeleton",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofPolylineSkeletonTest::RunTest(const FString& Parameters)
{
	auto MakeRoof = [](const TArray<FVector2D>& Verts)
	{
		FCSRoofDesc Desc;
		Desc.Footprint.Verts = Verts;
		Desc.EaveZ = 300.0f;
		Desc.Pitch = 35.0f;
		Desc.Overhang = 25.0f;
		return Desc;
	};

	// ---- ① 矩形：骨架恰好是「四条角斜脊（按角号）+ 一条脊」，端点与老公式一致 ----
	{
		const FCSRoofDesc Roof = CSHouseTest_MakeRoof(600.0, 400.0);
		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof.Footprint, Roof.Overhang, S);
		TestEqual(TEXT("rect: four hips"), S.NumHips, 4);
		TestEqual(TEXT("rect: four hips plus one ridge"), S.Arcs.Num(), 5);
		if (S.Arcs.Num() == 5)
		{
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				const FVector2D Sign = CSHouseQuoin::CornerSign(Corner);
				const FCSRoofSkeletonArc& Hip = S.Arcs[Corner];
				TestTrue(FString::Printf(TEXT("rect: hip %d starts on the eave outer corner"), Corner),
					Hip.A.Equals(FVector2D(Sign.X * 325.0, Sign.Y * 225.0), 1.0e-6) && FMath::IsNearlyEqual(Hip.InsetA, -25.0, 1.0e-9));
				TestTrue(FString::Printf(TEXT("rect: hip %d ends on its ridge end"), Corner),
					Hip.B.Equals(FVector2D(Sign.X * 100.0, 0.0), 1.0e-6) && FMath::IsNearlyEqual(Hip.InsetB, 200.0, 1.0e-9));
			}
			const FCSRoofSkeletonArc& Ridge = S.Arcs[4];
			TestTrue(TEXT("rect: the ridge runs -100 -> +100 along X at the top"),
				Ridge.A.Equals(FVector2D(-100.0, 0.0), 1.0e-6) && Ridge.B.Equals(FVector2D(100.0, 0.0), 1.0e-6)
				&& FMath::IsNearlyEqual(Ridge.InsetA, 200.0, 1.0e-9));
		}
		TestTrue(TEXT("rect: max inset is exactly half the short side"), S.MaxInset == 200.0);
	}

	// ---- ② 正六边形：收成一个尖，六条角斜脊都汇到中心 ----
	{
		TArray<FVector2D> Hex;
		for (int32 i = 0; i < 6; ++i)
		{
			const double A = UE_DOUBLE_TWO_PI * double(i) / 6.0;
			Hex.Add(FVector2D(300.0 * FMath::Cos(A), 300.0 * FMath::Sin(A)));
		}
		const FCSRoofDesc Roof = MakeRoof(Hex);
		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof.Footprint, Roof.Overhang, S);
		const double Inradius = 300.0 * FMath::Cos(UE_DOUBLE_PI / 6.0);
		TestTrue(FString::Printf(TEXT("hexagon: max inset is the inradius (%.4f)"), S.MaxInset), FMath::IsNearlyEqual(S.MaxInset, Inradius, 1.0e-6));
		TestTrue(TEXT("hexagon: the top is a point at the centre"),
			S.TopA.Equals(FVector2D::ZeroVector, 1.0e-6) && S.TopB.Equals(FVector2D::ZeroVector, 1.0e-6));
		TestEqual(TEXT("hexagon: six hips"), S.NumHips, 6);
		for (int32 Corner = 0; Corner < S.NumHips && Corner < S.Arcs.Num(); ++Corner)
		{
			const FCSRoofSkeletonArc& Hip = S.Arcs[Corner];
			const FVector2D Vertex = Hex[(Corner + 1) % 6];
			TestTrue(FString::Printf(TEXT("hexagon: hip %d ends at the apex"), Corner), Hip.B.Equals(FVector2D::ZeroVector, 1.0e-6));
			TestTrue(FString::Printf(TEXT("hexagon: hip %d starts on the eave corner past its vertex"), Corner),
				Hip.A.Equals(Vertex.GetSafeNormal() * (300.0 + 25.0 / FMath::Cos(UE_DOUBLE_PI / 6.0)), 1.0e-6));
		}
		TestTrue(TEXT("hexagon: ridge height uses the inradius"),
			FMath::IsNearlyEqual(CSHouseRoof_RidgeZ(Roof), 300.0f + Roof.TanPitch() * float(Inradius), 1.0e-3f));
		TestTrue(TEXT("hexagon: vertices sit on the wall top"),
			FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, Hex[2]), 300.0f, 1.0e-3f));
		TestTrue(TEXT("hexagon: just inside the eave outline is under the roof"),
			CSHouseRoof_IsUnderRoof(Roof, FVector2D(Inradius + 24.0, 0.0).GetRotated(30.0)));
		TestFalse(TEXT("hexagon: just outside the eave outline is not"),
			CSHouseRoof_IsUnderRoof(Roof, FVector2D(Inradius + 26.0, 0.0).GetRotated(30.0)));
	}

	// ---- ③ 不规则凸五边形：骨架的每条弧上，两侧坡面都取到最小内距，且内距沿弧线性变化 ----
	{
		const FCSRoofDesc Roof = MakeRoof({ FVector2D(-320, -180), FVector2D(260, -220), FVector2D(380, 60),
			FVector2D(40, 290), FVector2D(-290, 150) });
		TestTrue(TEXT("pentagon fixture is strictly convex CCW"), Roof.Footprint.IsStrictlyConvexCCW());
		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof.Footprint, Roof.Overhang, S);
		TestEqual(TEXT("pentagon: five hips"), S.NumHips, 5);
		TestTrue(FString::Printf(TEXT("pentagon: a convex skeleton has 2N-3 = 7 arcs or fewer after dropping zero-length ones (%d)"), S.Arcs.Num()),
			S.Arcs.Num() >= 5 && S.Arcs.Num() <= 8);

		int32 OffArc = 0, WrongFaces = 0;
		for (const FCSRoofSkeletonArc& Arc : S.Arcs)
		{
			for (int32 K = 0; K <= 8; ++K)
			{
				const double Alpha = double(K) / 8.0;
				const FVector2D P = FMath::Lerp(Arc.A, Arc.B, Alpha);
				const double Want = FMath::Lerp(Arc.InsetA, Arc.InsetB, Alpha);
				if (!FMath::IsNearlyEqual(Roof.InsetDistance(P), Want, 1.0e-6)) ++OffArc;
				const FCSHouseEdgeFrame FA = CSHouse_GetEdge(Arc.FaceA, Roof.Footprint, 0.0f);
				const FCSHouseEdgeFrame FB = CSHouse_GetEdge(Arc.FaceB, Roof.Footprint, 0.0f);
				if (!FMath::IsNearlyEqual(FVector2D::DotProduct(P - FA.Start, FA.In), Want, 1.0e-6)
					|| !FMath::IsNearlyEqual(FVector2D::DotProduct(P - FB.Start, FB.In), Want, 1.0e-6)) ++WrongFaces;
			}
		}
		TestEqual(TEXT("pentagon: every arc point has the arc's inset"), OffArc, 0);
		TestEqual(TEXT("pentagon: both faces of every arc attain that inset"), WrongFaces, 0);

		// 最高处：网格上找不到比 MaxInset 更高的点，而且离它不远。
		double GridMax = -1.0e9;
		for (int32 X = -400; X <= 400; X += 4)
		for (int32 Y = -300; Y <= 300; Y += 4)
		{
			GridMax = FMath::Max(GridMax, Roof.InsetDistance(FVector2D(X, Y)));
		}
		TestTrue(FString::Printf(TEXT("pentagon: max inset bounds the height field (grid %.3f vs %.3f)"), GridMax, S.MaxInset),
			GridMax <= S.MaxInset + 1.0e-6 && GridMax >= S.MaxInset - 4.0);
		TestTrue(TEXT("pentagon: the top sits at the max inset"),
			FMath::IsNearlyEqual(Roof.InsetDistance(S.TopA), S.MaxInset, 1.0e-6)
			&& FMath::IsNearlyEqual(Roof.InsetDistance(S.TopB), S.MaxInset, 1.0e-6));
	}

	// ---- ④ 退化 ----
	{
		FCSRoofSkeleton S;
		FCSHouseFootprint Two;
		Two.Verts = { FVector2D(0, 0), FVector2D(100, 0) };
		CSHouseRoof_BuildSkeleton(Two, 25.0, S);
		TestTrue(TEXT("an invalid footprint yields an empty skeleton"), S.Arcs.IsEmpty() && S.MaxInset == 0.0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// 拉尺寸（D5）：单边推拉的记账
// -----------------------------------------------------------------------------

namespace
{
/** 第 EdgeIndex 面墙外皮中心的世界位置。**推拉的两条不变量都只能靠它验**。 */
FVector CSHouseTest_WallCentre(const FVector2D& Size, const FVector& Centre, int32 EdgeIndex, float Yaw)
{
	const double Dim = CSHouseResize_EdgeDrivesX(EdgeIndex) ? Size.X : Size.Y;
	return Centre + CSHouseResize_EdgeOuterWorld(EdgeIndex, Yaw) * (Dim * 0.5);
}

/**
 * 第 EdgeIndex 面墙推的是 footprint 的哪一维（的当前值）。
 *
 * 存在的理由：边号到轴的映射是 `EdgeIndex & 1`，**边 1 / 边 3 推 X，边 0 / 边 2 推 Y** ——
 * 直觉上"东墙"该配 Y，写断言时极易反手写成 `.Y`，而那条轴恒不动 ⇒ 断言读到 0，
 * 与"推拉整个没生效"逐字相同。
 */
double CSHouseTest_PushedDim(const ACSHouseActor* House, int32 EdgeIndex)
{
	return CSHouseResize_EdgeDrivesX(EdgeIndex) ? House->FootprintSize.X : House->FootprintSize.Y;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseEdgePushTest,
	"PCGPlugins.ComputeShaderGenerator.House.EdgePush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseEdgePushTest::RunTest(const FString& Parameters)
{
	// ① 幂等：连续 10 次 Offset=0 不许改动任何量。
	// 计划 D5 的第一条配套单测。它抓的是"每次事件都白走一遍状态机"这类漂移 ——
	// gizmo 在没动的那些帧照样发 PostEditMove，漂一点点就是拖动期的持续抖动。
	{
		FVector2D Size(600.0, 400.0);
		FVector Centre(1000.0, 2000.0, 50.0);
		for (int32 i = 0; i < 10; ++i)
		{
			const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 1, 30.0f, 0.0f, 200.0f);
			TestEqual(TEXT("A zero push applies zero"), Applied, 0.0f);
		}
		TestTrue(TEXT("Ten zero pushes leave the size untouched"), Size == FVector2D(600.0, 400.0));
		TestTrue(TEXT("Ten zero pushes leave the centre untouched"), Centre == FVector(1000.0, 2000.0, 50.0));
	}

	// ② 对侧墙不动、被推墙恰好走 Δ ——「拖 1 m 墙走 2 m」那个父子回路缺陷的钉子。
	// 四条边 × 带 yaw 各验一遍：中心随动是**世界**方向的量，只在 yaw=0 下对是最常见的漏法。
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		constexpr float Yaw = 37.0f;
		constexpr double Delta = 123.0;
		FVector2D Size(600.0, 400.0);
		FVector Centre(1000.0, 2000.0, 50.0);
		const FVector PushedBefore = CSHouseTest_WallCentre(Size, Centre, Edge, Yaw);
		const FVector OppositeBefore = CSHouseTest_WallCentre(Size, Centre, Edge + 2, Yaw);

		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, Edge, Yaw, float(Delta), 200.0f);
		TestEqual(FString::Printf(TEXT("Edge %d applies the whole offset"), Edge), double(Applied), Delta, 1.0e-3);

		const FVector PushedAfter = CSHouseTest_WallCentre(Size, Centre, Edge, Yaw);
		const FVector OppositeAfter = CSHouseTest_WallCentre(Size, Centre, Edge + 2, Yaw);
		TestTrue(FString::Printf(TEXT("Edge %d: the opposite wall does not move"), Edge),
			OppositeAfter.Equals(OppositeBefore, 1.0e-3));
		TestTrue(FString::Printf(TEXT("Edge %d: the pushed wall moves exactly the offset"), Edge),
			PushedAfter.Equals(PushedBefore + CSHouseResize_EdgeOuterWorld(Edge, Yaw) * Delta, 1.0e-3));
	}

	// ③ MinFootprint 是硬下界，且返回的是**实际**位移（不是请求值）。
	// 记账量法必须拿返回值累加：记成请求值的话，顶在下限上的那段时间里残差会一路攒着，
	// 松手瞬间房子跳一大截 —— 症状与"拖动漂移"一模一样，很难归因。
	{
		FVector2D Size(600.0, 400.0);
		FVector Centre = FVector::ZeroVector;
		const float Applied = CSHouse_ApplyEdgePush(Size, Centre, 0, 0.0f, -1000.0f, 200.0f);
		TestEqual(TEXT("Shrinking past the floor applies only what was possible"), double(Applied), -200.0, 1.0e-3);
		TestEqual(TEXT("The floor holds"), Size.Y, 200.0, 1.0e-3);
	}

	return true;
}

namespace
{
/** 正 N 边形（逆时针，首顶点在 +X 轴上）。 */
TArray<FVector2D> CSHouseTest_RegularPolygon(int32 N, double Radius)
{
	TArray<FVector2D> Verts;
	for (int32 i = 0; i < N; ++i)
	{
		const double A = UE_DOUBLE_TWO_PI * double(i) / double(N);
		Verts.Add(FVector2D(Radius * FMath::Cos(A), Radius * FMath::Sin(A)));
	}
	return Verts;
}

/** 第 Edge 条边所在直线的世界表示：(世界外法线, 直线沿外法线离世界原点多远)。 */
void CSHouseTest_WorldEdgeLine(const FCSHouseFootprint& Local, const FVector& Centre, float Yaw, int32 Edge,
	FVector2D& OutNormal, double& OutOffset)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Local, 0.0f);
	const FRotator R(0.0, double(Yaw), 0.0);
	const FVector N = R.RotateVector(FVector(-F.In.X, -F.In.Y, 0.0));
	const FVector P = Centre + R.RotateVector(FVector(F.Start.X, F.Start.Y, 0.0));
	OutNormal = FVector2D(N.X, N.Y);
	OutOffset = FVector2D::DotProduct(FVector2D(P.X, P.Y), OutNormal);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseEdgePushPolylineTest,
	"PCGPlugins.ComputeShaderGenerator.House.EdgePushPolyline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseEdgePushPolylineTest::RunTest(const FString& Parameters)
{
	// footprint 折线化 3f：推一条边 = 这条边沿外法线平移、两端沿相邻边滑、包围盒回到房心。
	// 不变量从「对侧墙不动」推广成「**除被推的那条以外每条边的世界直线都不动**」。

	// ---- ① 矩形上与矩形版一致（矩形房子仍走矩形版，这条钉住两者没有分叉）----
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		for (double Delta : { 123.0, -50.0, -1000.0 })
		{
			constexpr float Yaw = 37.0f;
			FVector2D Size(600.0, 400.0);
			FVector RectCentre(1000.0, 2000.0, 50.0);
			const float RectApplied = CSHouse_ApplyEdgePush(Size, RectCentre, Edge, Yaw, float(Delta), 200.0f);

			FCSHouseFootprint Poly = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
			FVector PolyCentre(1000.0, 2000.0, 50.0);
			const float PolyApplied = CSHouse_ApplyEdgePushPolyline(Poly, PolyCentre, Edge, Yaw, float(Delta), 200.0f);

			TestEqual(FString::Printf(TEXT("rect edge %d delta %.0f: same applied offset"), Edge, Delta),
				double(PolyApplied), double(RectApplied), 1.0e-3);
			TestTrue(FString::Printf(TEXT("rect edge %d delta %.0f: same size"), Edge, Delta),
				Poly.GetBounds().GetSize().Equals(Size, 1.0e-3));
			TestTrue(FString::Printf(TEXT("rect edge %d delta %.0f: same centre"), Edge, Delta),
				PolyCentre.Equals(RectCentre, 1.0e-3));
			TestTrue(FString::Printf(TEXT("rect edge %d delta %.0f: still the centred rectangle"), Edge, Delta),
				Poly.EqualsApprox(FCSHouseFootprint::MakeRect(Size), 1.0e-3));
		}
	}

	// ---- ② 六边形：被推的边走 Δ，其余五条边的世界直线一条都不动 ----
	{
		constexpr float Yaw = 23.0f;
		constexpr double Delta = 60.0;
		FCSHouseFootprint Hex;
		Hex.Verts = CSHouseTest_RegularPolygon(6, 300.0);
		FVector Centre(500.0, -200.0, 10.0);

		TArray<FVector2D> NormalsBefore;
		TArray<double> OffsetsBefore;
		for (int32 Edge = 0; Edge < 6; ++Edge)
		{
			FVector2D N; double O = 0.0;
			CSHouseTest_WorldEdgeLine(Hex, Centre, Yaw, Edge, N, O);
			NormalsBefore.Add(N);
			OffsetsBefore.Add(O);
		}

		const float Applied = CSHouse_ApplyEdgePushPolyline(Hex, Centre, 2, Yaw, float(Delta), 200.0f);
		TestEqual(TEXT("hexagon: the push applies in full"), double(Applied), Delta, 1.0e-4);
		for (int32 Edge = 0; Edge < 6; ++Edge)
		{
			FVector2D N; double O = 0.0;
			CSHouseTest_WorldEdgeLine(Hex, Centre, Yaw, Edge, N, O);
			TestTrue(FString::Printf(TEXT("hexagon: edge %d keeps its direction"), Edge), N.Equals(NormalsBefore[Edge], 1.0e-9));
			const double Want = OffsetsBefore[Edge] + (Edge == 2 ? Delta : 0.0);
			TestTrue(FString::Printf(TEXT("hexagon: edge %d line %s (%.6f vs %.6f)"), Edge,
				Edge == 2 ? TEXT("moves by the push") : TEXT("stays put"), O, Want), FMath::IsNearlyEqual(O, Want, 1.0e-6));
		}
		TestTrue(TEXT("hexagon: still strictly convex"), Hex.IsStrictlyConvexCCW());
		TestTrue(TEXT("hexagon: bounding box recentred on the local origin"),
			Hex.GetBounds().GetCenter().Equals(FVector2D::ZeroVector, 1.0e-9));
	}

	// ---- ③ 下限：宽度（MinFootprint）、相邻边推穿、被推的边推没 ----
	{
		const double Sin60 = FMath::Sin(UE_DOUBLE_PI / 3.0);
		const double Width = 2.0 * 300.0 * FMath::Cos(UE_DOUBLE_PI / 6.0);

		// 宽度先卡住：MinFootprint 450 < 宽度 519.6，而相邻边要到 −258.9 才被推穿。
		{
			FCSHouseFootprint Hex;
			Hex.Verts = CSHouseTest_RegularPolygon(6, 300.0);
			FVector Centre = FVector::ZeroVector;
			const float Applied = CSHouse_ApplyEdgePushPolyline(Hex, Centre, 0, 0.0f, -1000.0f, 450.0f);
			TestEqual(TEXT("hexagon: the width floor holds"), double(Applied), 450.0 - Width, 1.0e-3);
		}
		// 相邻边先卡住：MinFootprint 200 太松，往里推到两条相邻边只剩 1 cm 为止。
		{
			FCSHouseFootprint Hex;
			Hex.Verts = CSHouseTest_RegularPolygon(6, 300.0);
			FVector Centre = FVector::ZeroVector;
			const float Applied = CSHouse_ApplyEdgePushPolyline(Hex, Centre, 0, 0.0f, -1000.0f, 200.0f);
			TestEqual(TEXT("hexagon: neighbours are not pushed through"), double(Applied), (1.0 - 300.0) * Sin60, 1.0e-3);
			TestEqual(TEXT("hexagon: the previous edge keeps 1 cm"), double(CSHouse_GetEdge(5, Hex, 0.0f).Len), 1.0, 1.0e-3);
			TestEqual(TEXT("hexagon: the next edge keeps 1 cm"), double(CSHouse_GetEdge(1, Hex, 0.0f).Len), 1.0, 1.0e-3);
			TestTrue(TEXT("hexagon: pushed to the neighbour limit it is still convex"), Hex.IsStrictlyConvexCCW());
		}
		// 往外推：60° 转角上被推的边两头各缩 d·cot60°，推到只剩 1 cm 为止。
		{
			FCSHouseFootprint Hex;
			Hex.Verts = CSHouseTest_RegularPolygon(6, 300.0);
			FVector Centre = FVector::ZeroVector;
			const float Applied = CSHouse_ApplyEdgePushPolyline(Hex, Centre, 0, 0.0f, 1000.0f, 200.0f);
			const double Cot60 = 1.0 / FMath::Tan(UE_DOUBLE_PI / 3.0);
			TestEqual(TEXT("hexagon: the pushed edge is not pushed out of existence"), double(Applied), 299.0 / (2.0 * Cot60), 1.0e-3);
			TestEqual(TEXT("hexagon: the pushed edge keeps 1 cm"), double(CSHouse_GetEdge(0, Hex, 0.0f).Len), 1.0, 1.0e-3);
		}
		// 幂等：Offset = 0 一位都不许动。
		{
			FCSHouseFootprint Hex;
			Hex.Verts = CSHouseTest_RegularPolygon(6, 300.0);
			const FCSHouseFootprint Before = Hex;
			FVector Centre(1.0, 2.0, 3.0);
			TestEqual(TEXT("hexagon: a zero push applies zero"), CSHouse_ApplyEdgePushPolyline(Hex, Centre, 3, 11.0f, 0.0f, 200.0f), 0.0f);
			TestTrue(TEXT("hexagon: a zero push leaves the polyline bit for bit"), Hex.EqualsApprox(Before, 0.0));
			TestTrue(TEXT("hexagon: a zero push leaves the centre bit for bit"), Centre == FVector(1.0, 2.0, 3.0));
		}
	}

	// ---- ④ 形状 × 尺寸：任意坐标系、顺时针也行；凹的取凸包（2026-09-14 裁决），退化的退回矩形 ----
	//      凸包的逐条判据（共线、乱序、重复点、起点口径）在 `House.FootprintConvexHull`。
	{
		const FVector2D Size(600.0, 519.6152422706632);
		TArray<FVector2D> Shape = CSHouseTest_RegularPolygon(6, 3.0);
		for (FVector2D& V : Shape) V += FVector2D(7.0, -3.0);                       // 任意坐标系
		TArray<FVector2D> Clockwise;
		for (int32 i = 0; i < Shape.Num(); ++i) Clockwise.Add(Shape[(Shape.Num() - i) % Shape.Num()]);

		const FCSHouseFootprint FromCCW = FCSHouseFootprint::FromShape(Shape, Size);
		const FCSHouseFootprint FromCW = FCSHouseFootprint::FromShape(Clockwise, Size);
		TestEqual(TEXT("FromShape keeps the vertex count"), FromCCW.NumEdges(), 6);
		TestTrue(TEXT("FromShape stretches the bounding box to the size"), FromCCW.GetBounds().GetSize().Equals(Size, 1.0e-6));
		TestTrue(TEXT("FromShape centres the bounding box on the origin"), FromCCW.GetBounds().GetCenter().Equals(FVector2D::ZeroVector, 1.0e-6));
		FCSHouseFootprint Expected;
		Expected.Verts = CSHouseTest_RegularPolygon(6, 300.0);
		TestTrue(TEXT("FromShape reproduces the hexagon of that size"), FromCCW.EqualsApprox(Expected, 1.0e-6));
		TestTrue(TEXT("a clockwise shape comes out counter-clockwise with its first vertex kept"),
			FromCW.EqualsApprox(FromCCW, 1.0e-9));

		TArray<FVector2D> LShape = { FVector2D(0, 0), FVector2D(2, 0), FVector2D(2, 1), FVector2D(1, 1), FVector2D(1, 2), FVector2D(0, 2) };
		const FCSHouseFootprint LHull = FCSHouseFootprint::FromShape(LShape, Size);
		TestEqual(TEXT("a concave shape is used as its convex hull (the inner corner is dropped)"), LHull.NumEdges(), 5);
		TestTrue(TEXT("and the hull is strictly convex and counter-clockwise"), LHull.IsStrictlyConvexCCW());
		TestTrue(TEXT("an empty shape is the rectangle bit for bit"),
			FCSHouseFootprint::FromShape(TArray<FVector2D>(), Size).EqualsApprox(FCSHouseFootprint::MakeRect(Size), 0.0));
	}
	return true;
}

// -----------------------------------------------------------------------------
// footprint 形状取凸包（2026-09-14 用户裁决："让折线取最后的凸包就行了"，凹 footprint 不做）
//
// 此前非严格凸的形状一律**静默**退回矩形：用户在细节面板里加一个凹的点，整栋房子当场变回方盒子，
// 而日志、断言都不出声。现在按凸包使用（`FCSHouseFootprint::FromShape`），处置方式经 `ECSFootprintShapeFix`
// 报给调用方（房子据此在重求值里打一次 Warning）。这一条钉凸包本身：顶点、起点口径、绕向、各种坏输入。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFootprintConvexHullTest,
	"PCGPlugins.ComputeShaderGenerator.House.FootprintConvexHull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFootprintConvexHullTest::RunTest(const FString& Parameters)
{
	const FVector2D Size(600.0, 400.0);
	auto Shaped = [&Size](TConstArrayView<FVector2D> Shape, ECSFootprintShapeFix& OutFix)
	{
		OutFix = ECSFootprintShapeFix::Rect;
		return FCSHouseFootprint::FromShape(Shape, Size, &OutFix);
	};
	auto SameVerts = [](const FCSHouseFootprint& FP, const TArray<FVector2D>& Want, double Tolerance)
	{
		if (FP.Verts.Num() != Want.Num()) return false;
		for (int32 Index = 0; Index < Want.Num(); ++Index)
		{
			if (!FP.Verts[Index].Equals(Want[Index], Tolerance)) return false;
		}
		return true;
	};

	// ① 凹（L 形）：内角那一点被包进去丢掉，6 → 5；从原下标 0 起、逆时针。
	//    包围盒 [0,2]² 居中拉到 600×400 ⇒ x' = (x − 1)·300、y' = (y − 1)·200。
	{
		const TArray<FVector2D> L = { FVector2D(0, 0), FVector2D(2, 0), FVector2D(2, 1), FVector2D(1, 1), FVector2D(1, 2), FVector2D(0, 2) };
		ECSFootprintShapeFix Fix;
		const FCSHouseFootprint FP = Shaped(L, Fix);
		TestTrue(TEXT("concave: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("concave: six input points give a five-edge footprint"), FP.NumEdges(), 5);
		TestTrue(TEXT("concave: the hull vertices, from input vertex 0, counter-clockwise"),
			SameVerts(FP, { FVector2D(-300, -200), FVector2D(300, -200), FVector2D(300, 0), FVector2D(0, 200), FVector2D(-300, 200) }, 1.0e-9));
		TestTrue(TEXT("concave: the hull is strictly convex and counter-clockwise"), FP.IsStrictlyConvexCCW());
	}

	// ② 共线点：矩形四条边各插一个中点（8 点）⇒ 4 条边，而且逐位就是那个矩形（起点 = 原下标 0 的角）。
	{
		const TArray<FVector2D> Mid = { FVector2D(0, 0), FVector2D(1, 0), FVector2D(2, 0), FVector2D(2, 1),
			FVector2D(2, 2), FVector2D(1, 2), FVector2D(0, 2), FVector2D(0, 1) };
		ECSFootprintShapeFix Fix;
		const FCSHouseFootprint FP = Shaped(Mid, Fix);
		TestTrue(TEXT("collinear: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("collinear: eight input points give a four-edge footprint"), FP.NumEdges(), 4);
		TestTrue(TEXT("collinear: the result is the rectangle of that size"), FP.EqualsApprox(FCSHouseFootprint::MakeRect(Size), 1.0e-9));
	}

	// ③ **近**共线（转角正弦低于 `IsStrictlyConvexCCW` 的 1e-4）同样摘掉 —— 否则凸包过不了凸性判据又被打回矩形。
	{
		const TArray<FVector2D> Bump = { FVector2D(0, 0), FVector2D(1, -1.0e-6), FVector2D(2, 0), FVector2D(2, 2), FVector2D(0, 2) };
		ECSFootprintShapeFix Fix;
		const FCSHouseFootprint FP = Shaped(Bump, Fix);
		TestTrue(TEXT("nearly collinear: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("nearly collinear: the hair-thin bump is dropped"), FP.NumEdges(), 4);
		TestTrue(TEXT("nearly collinear: still strictly convex"), FP.IsStrictlyConvexCCW());
	}

	// ④ 乱序（自交的"领结"）：正六边形的 6 个顶点打乱 ⇒ 还原成逆时针六边形，首顶点仍是原下标 0 那个。
	{
		const TArray<FVector2D> Hex = CSHouseTest_RegularPolygon(6, 1.0);
		const TArray<FVector2D> Shuffled = { Hex[0], Hex[3], Hex[1], Hex[5], Hex[2], Hex[4] };
		const FVector2D HexSize(600.0, 519.6152422706632);
		ECSFootprintShapeFix Fix = ECSFootprintShapeFix::Rect;
		const FCSHouseFootprint FP = FCSHouseFootprint::FromShape(Shuffled, HexSize, &Fix);
		TestTrue(TEXT("shuffled: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("shuffled: six points give a six-edge footprint"), FP.NumEdges(), 6);
		FCSHouseFootprint Expected;
		Expected.Verts = CSHouseTest_RegularPolygon(6, 300.0);
		TestTrue(TEXT("shuffled: the hexagon comes back in counter-clockwise order from input vertex 0"), FP.EqualsApprox(Expected, 1.0e-6));
	}

	// ⑤ 重复点：三角形的一个角写了两遍 ⇒ 3 条边（边长为零的那条不许留下来）。
	{
		const TArray<FVector2D> Dup = { FVector2D(0, 0), FVector2D(2, 0), FVector2D(2, 0), FVector2D(1, 2) };
		ECSFootprintShapeFix Fix;
		const FCSHouseFootprint FP = Shaped(Dup, Fix);
		TestTrue(TEXT("duplicate: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("duplicate: a triangle with a repeated corner has three edges"), FP.NumEdges(), 3);
		TestTrue(TEXT("duplicate: strictly convex"), FP.IsStrictlyConvexCCW());
	}

	// ⑥ 内点：凸五边形里多给一个中心点 ⇒ 5 条边，中心点不出现。
	{
		TArray<FVector2D> Penta = CSHouseTest_RegularPolygon(5, 1.0);
		Penta.Insert(FVector2D(0.1, -0.05), 2);
		ECSFootprintShapeFix Fix;
		const FCSHouseFootprint FP = Shaped(Penta, Fix);
		TestTrue(TEXT("interior point: reported as hulled"), Fix == ECSFootprintShapeFix::Hulled);
		TestEqual(TEXT("interior point: the pentagon keeps five edges"), FP.NumEdges(), 5);
	}

	// ⑦ **严格凸的输入不算"修过"**：逆时针原样、顺时针翻过来（首顶点不动），两者逐位相同，都报 Kept。
	//    这一条守的是"已有的异形存档一个 bit 都不许变"：快路必须在凸包之前。
	{
		const TArray<FVector2D> Hex = CSHouseTest_RegularPolygon(6, 3.0);
		TArray<FVector2D> Clockwise;
		for (int32 Index = 0; Index < Hex.Num(); ++Index) Clockwise.Add(Hex[(Hex.Num() - Index) % Hex.Num()]);
		ECSFootprintShapeFix FixCCW = ECSFootprintShapeFix::Rect;
		ECSFootprintShapeFix FixCW = ECSFootprintShapeFix::Rect;
		const FCSHouseFootprint FromCCW = FCSHouseFootprint::FromShape(Hex, Size, &FixCCW);
		const FCSHouseFootprint FromCW = FCSHouseFootprint::FromShape(Clockwise, Size, &FixCW);
		TestTrue(TEXT("convex CCW: kept"), FixCCW == ECSFootprintShapeFix::Kept);
		TestTrue(TEXT("convex CW: kept (a flip is not a fix)"), FixCW == ECSFootprintShapeFix::Kept);
		TestTrue(TEXT("convex CW and CCW come out bit for bit the same"), FromCW.EqualsApprox(FromCCW, 0.0));
		TestTrue(TEXT("the hull of an already convex shape is that shape (same start, same order)"),
			SameVerts(FromCCW, FCSHouseFootprint::ConvexHullCCW(FromCCW.Verts), 0.0));
	}

	// ⑧ 退化兜底：凸包不足 3 个顶点 / 包围盒零宽 ⇒ 矩形（Degenerate）；少于 3 个点 ⇒ 矩形（Rect）。
	{
		const TArray<FVector2D> Line = { FVector2D(0, 0), FVector2D(1, 1), FVector2D(2, 2), FVector2D(3, 3) };
		const TArray<FVector2D> Flat = { FVector2D(0, 0), FVector2D(1, 0), FVector2D(2, 0) };
		const TArray<FVector2D> Two = { FVector2D(0, 0), FVector2D(1, 1) };
		ECSFootprintShapeFix Fix;
		TestTrue(TEXT("all collinear (tilted): the rectangle"), Shaped(Line, Fix).EqualsApprox(FCSHouseFootprint::MakeRect(Size), 0.0));
		TestTrue(TEXT("all collinear (tilted): reported as degenerate"), Fix == ECSFootprintShapeFix::Degenerate);
		TestTrue(TEXT("zero-height bounding box: the rectangle"), Shaped(Flat, Fix).EqualsApprox(FCSHouseFootprint::MakeRect(Size), 0.0));
		TestTrue(TEXT("zero-height bounding box: reported as degenerate"), Fix == ECSFootprintShapeFix::Degenerate);
		TestTrue(TEXT("two points: the rectangle"), Shaped(Two, Fix).EqualsApprox(FCSHouseFootprint::MakeRect(Size), 0.0));
		TestTrue(TEXT("two points: reported as plain rect, not a fix"), Fix == ECSFootprintShapeFix::Rect);
	}
	return true;
}

// -----------------------------------------------------------------------------
// footprint 哈希的粒度（2026-09-14）：矩形上必须还是折线化之前的 1 cm 尺寸粒度
//
// 折线化把各家哈希里的 `Q(FootprintSize.X, 1)` 换成了"逐顶点量化到 1 cm"，而矩形顶点是 ±Size/2 ⇒
// 尺寸粒度悄悄粗到了 2 cm：拖尺寸 1 cm 可能一次重建都不触发，哈希短路把一次真实改动吞掉。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFootprintHashGranularityTest,
	"PCGPlugins.ComputeShaderGenerator.House.FootprintHashGranularity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFootprintHashGranularityTest::RunTest(const FString& Parameters)
{
	auto HashOf = [](const FCSHouseFootprint& Footprint)
	{
		TArray<int32> H;
		Footprint.AppendQuantizedHash(H);
		return H;
	};
	auto Rect = [](double X, double Y) { return FCSHouseFootprint::MakeRect(FVector2D(X, Y)); };

	const TArray<int32> Base = HashOf(Rect(600.0, 400.0));

	// ① 相差 1 cm 的尺寸都分得开（X 与 Y 两个方向）。⚠️ **基准必须带小数**：顶点量到 1 cm 时，
	//    整数尺寸恰好落在四舍五入的奇点上（±300.5 一边进一边不进），整数对照一个都不红 —— 带 0.25 的
	//    偏移，1 cm 顶点量子会有一半（10 / 20）合并，而折线化之前的 `Q(FootprintSize, 1)` 一个都不合并。
	int32 MergedX = 0;
	int32 MergedY = 0;
	for (int32 Cm = 0; Cm < 20; ++Cm)
	{
		if (HashOf(Rect(600.25 + Cm, 400.0)) == HashOf(Rect(601.25 + Cm, 400.0))) ++MergedX;
		if (HashOf(Rect(600.0, 400.25 + Cm)) == HashOf(Rect(600.0, 401.25 + Cm))) ++MergedY;
	}
	TestEqual(TEXT("every centimetre of length is a different footprint"), MergedX, 0);
	TestEqual(TEXT("every centimetre of width is a different footprint"), MergedY, 0);
	// 最直白的反例：差 1.6 cm。1 cm 顶点量子下两者同一个哈希，旧口径与新口径都分得开。
	TestTrue(TEXT("599.2 and 600.8 are different footprints"), HashOf(Rect(599.2, 400.0)) != HashOf(Rect(600.8, 400.0)));

	// ② 亚厘米的抖动照旧被吸收（同旧口径 `Q(600.4, 1) == Q(600, 1)`）。
	TestTrue(TEXT("+0.4 cm is absorbed"), HashOf(Rect(600.4, 400.0)) == Base);
	TestTrue(TEXT("-0.4 cm is absorbed"), HashOf(Rect(599.6, 400.0)) == Base);

	// ③ 顶点数照旧进哈希：同一个矩形多一个共线顶点，边的划分已经换了一套。
	FCSHouseFootprint WithMid = Rect(600.0, 400.0);
	WithMid.Verts.Insert(FVector2D(0.0, -200.0), 1);
	TestTrue(TEXT("an extra vertex changes the hash even when every coordinate is shared"), HashOf(WithMid) != Base);
	return true;
}

// -----------------------------------------------------------------------------
// 边缘线段分割：等分、护角、最小宽度早退
// -----------------------------------------------------------------------------
// ⓘ `FCSHouseEdgeSplitTest`（等分槽）已随 `SplitEdgeIntoSlots` 于 2026-09-04 一并删除：
//    门不再按等分槽开，改成"路在墙上截出的区间"。接替它的是下面的 `DoorRuns`。
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// 门洞区间求解（2026-09-04 重做）：门宽 = 路在墙上截出的弦长
//
// 这一族用例钉的正是旧口径做不到的四件事：亚采样端点、连续性、区间滞回、过宽切分。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDoorRunsTest,
	"PCGPlugins.ComputeShaderGenerator.House.DoorRuns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/** 造一条沿边剖面：[Lo, Hi] 等距 Steps+1 点，路是中心 Center、半宽 HalfW 的梯形（边沿线性过渡）。 */
void CSHouseTest_MakeRoadProfile(float Lo, float Hi, int32 Steps, float Center, float HalfW,
	float Feather, TArray<float>& Out, float& OutStep)
{
	OutStep = (Hi - Lo) / Steps;
	Out.Reset();
	for (int32 K = 0; K <= Steps; ++K)
	{
		const float S = Lo + OutStep * K;
		const float D = FMath::Abs(S - Center);
		// 梯形：|D| ≤ HalfW 处为 1，HalfW..HalfW+Feather 线性降到 0。
		const float W = (D <= HalfW) ? 1.0f : FMath::Max(0.0f, 1.0f - (D - HalfW) / FMath::Max(Feather, 1.0e-3f));
		Out.Add(W);
	}
}

FCSDoorRunParams CSHouseTest_MakeRunParams(float Hi)
{
	FCSDoorRunParams P;
	P.OnWeight = 0.5f;
	P.MinWidth = 40.0f;
	P.KeepWidth = 32.0f;
	P.MaxWidth = 260.0f;
	P.PierWidth = 20.0f;
	P.Hi = Hi;
	return P;
}
}   // namespace

bool FCSHouseDoorRunsTest::RunTest(const FString& Parameters)
{
	constexpr float Lo = 60.0f, Hi = 540.0f;   // 600 长墙、护角 60
	const FCSDoorRunParams P = CSHouseTest_MakeRunParams(Hi);
	TArray<float> Weights;
	TArray<FCSDoorRun> Runs;
	float Step = 0;

	// ---- ① 没有路 ⇒ 一个洞都不开 ----
	{
		Weights.Init(0.0f, 21);
		CSHouse_SolveRoadRuns(Weights, Lo, (Hi - Lo) / 20.0f, P, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("No road yields no runs"), Runs.Num(), 0);
	}

	// ---- ② 一条居中的路 ⇒ 一个洞，宽度 = 路在阈值 0.5 处的弦长 ----
	// 梯形半宽 60、过渡 20 ⇒ 权重降到 0.5 的位置在 60 + 10 = 70 ⇒ 弦长 140。
	{
		CSHouseTest_MakeRoadProfile(Lo, Hi, 24, 300.0f, 60.0f, 20.0f, Weights, Step);
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("One road yields one run"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			TestTrue(TEXT("Run is centred on the road"), FMath::IsNearlyEqual(Runs[0].Center(), 300.0f, 1.0f));
			TestTrue(TEXT("Run width is the road's chord at the threshold"),
				FMath::IsNearlyEqual(Runs[0].Width(), 140.0f, 2.0f));
		}
	}

	// ---- ③ **亚采样端点**：宽度不是采样步长的整数倍 ----
	// 这一条是重做的全部意义。旧口径按"几个采样点过阈"定宽，宽度只能是 Step 的整数倍，
	// 画路时门一格一格地跳；这里端点在跨阈的两点之间线性求根，宽度是路宽的连续函数。
	{
		CSHouseTest_MakeRoadProfile(Lo, Hi, 24, 300.0f, 53.0f, 17.0f, Weights, Step);
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("Sub-sample profile still yields one run"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			const float Ratio = Runs[0].Width() / Step;
			TestTrue(TEXT("Run width is NOT quantised to the sample step"),
				FMath::Abs(Ratio - FMath::RoundToFloat(Ratio)) > 0.05f);
		}
	}

	// ---- ④ **连续性**：路一点点变宽，洞就一点点变宽（没有台阶）----
	{
		float Previous = 0.0f;
		float MaxJump = 0.0f;
		for (int32 K = 0; K <= 60; ++K)
		{
			const float HalfW = 30.0f + K * 1.0f;   // 每步只宽 1 cm
			CSHouseTest_MakeRoadProfile(Lo, Hi, 24, 300.0f, HalfW, 20.0f, Weights, Step);
			CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
			if (Runs.Num() != 1) continue;
			const float Width = Runs[0].Width();
			TestTrue(TEXT("Run width never shrinks as the road widens"), Width >= Previous - 1.0e-3f);
			if (Previous > 0.0f) MaxJump = FMath::Max(MaxJump, Width - Previous);
			Previous = Width;
		}
		// 每步路宽 +2（两侧各 +1）⇒ 洞宽的单步增量必须同量级，绝不能出现"一跳一个采样格"。
		TestTrue(TEXT("Width grows smoothly, never by a whole sample step"), MaxJump < Step * 0.6f);
	}

	// ---- ⑤ 滞回：窄到 MinWidth 以下但仍 ≥ KeepWidth 的洞，**只有上一帧开着**才留 ----
	{
		CSHouseTest_MakeRoadProfile(Lo, Hi, 24, 300.0f, 8.0f, 20.0f, Weights, Step);   // 弦长 ≈ 36
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("A sub-minimum road does not open a new door"), Runs.Num(), 0);

		const FCSDoorRun Prev[] = { FCSDoorRun{ 280.0f, 320.0f } };
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, Prev, Runs);
		TestEqual(TEXT("The same road keeps an already-open door alive"), Runs.Num(), 1);

		// 上一帧的区间在别处 ⇒ 不交叠 ⇒ 不继承（滞回不能跨洞传染）。
		const FCSDoorRun Elsewhere[] = { FCSDoorRun{ 80.0f, 140.0f } };
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, Elsewhere, Runs);
		TestEqual(TEXT("Hysteresis does not leak across non-overlapping runs"), Runs.Num(), 0);
	}

	// ---- ⑥ 过宽的路切成一排拱，相邻之间留墩 ----
	{
		CSHouseTest_MakeRoadProfile(Lo, Hi, 48, 300.0f, 220.0f, 10.0f, Weights, Step);   // 弦长 ≈ 450 > MaxWidth
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		TestTrue(TEXT("An over-wide road splits into several arches"), Runs.Num() >= 2);
		for (int32 Index = 0; Index + 1 < Runs.Num(); ++Index)
		{
			TestTrue(TEXT("Adjacent arches keep a pier between them"),
				FMath::IsNearlyEqual(Runs[Index + 1].S0 - Runs[Index].S1, P.PierWidth, 0.5f));
			TestTrue(TEXT("Split arches respect the max width"), Runs[Index].Width() <= P.MaxWidth + 0.5f);
		}
	}

	// ---- ⑦ 路跑出可用区间 ⇒ 端点夹在护角上，不越界 ----
	{
		CSHouseTest_MakeRoadProfile(Lo, Hi, 24, Lo - 30.0f, 90.0f, 10.0f, Weights, Step);
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		for (const FCSDoorRun& Run : Runs)
		{
			TestTrue(TEXT("Run stays inside the usable span"), Run.S0 >= Lo - 1.0e-3f && Run.S1 <= Hi + 1.0e-3f);
		}
	}

	// ---- ⑧ 两条分开的路 ⇒ 两个洞（旧口径下这依赖两条路恰好落在不同的槽里）----
	{
		Weights.Reset();
		Step = (Hi - Lo) / 48.0f;
		for (int32 K = 0; K <= 48; ++K)
		{
			const float S = Lo + Step * K;
			const bool bRoadA = FMath::Abs(S - 180.0f) <= 45.0f;
			const bool bRoadB = FMath::Abs(S - 420.0f) <= 45.0f;
			Weights.Add((bRoadA || bRoadB) ? 1.0f : 0.0f);
		}
		CSHouse_SolveRoadRuns(Weights, Lo, Step, P, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("Two separate roads yield two doors"), Runs.Num(), 2);
	}

	// -------------------------------------------------------------------------
	// 以下四条走**闭环**（`bClosed`）—— 四条边接成周界之后门洞真正跑的就是这一支，
	// 而 2026-09-05 之前它一条断言都没有（转角的洞全靠它）。
	// -------------------------------------------------------------------------

	constexpr float Period = 800.0f;
	constexpr int32 RingN = 80;
	const float RingStep = Period / RingN;   // 10 cm
	// `Weights` 覆盖 [0, Period)，末点**不重复**首点，这是闭环模式的口径。
	auto MakeRing = [&](auto&& IsRoad)
	{
		Weights.Reset();
		for (int32 K = 0; K < RingN; ++K) Weights.Add(IsRoad(RingStep * K) ? 1.0f : 0.0f);
	};
	FCSDoorRunParams Ring = CSHouseTest_MakeRunParams(Period);
	Ring.bClosed = true;

	// ---- ⑨ 压在环原点上的路得到**一条**跨原点的段，不是被数组首尾切成两条 ----
	{
		MakeRing([](float S) { return S <= 60.0f || S >= 740.0f; });
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("A road across the ring origin yields one run, not two"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			// 绕回的段用**不取模**的 S 表示（调用方按边切时靠这一点拆成两片）。
			TestTrue(TEXT("The wrapping run keeps un-modded S"), Runs[0].S1 > Period);
		}
	}

	// ---- ⑩ 碎环段并掉：两个洞之间只剩一小段墙 ⇒ 并成一条（要点 ④）----
	{
		MakeRing([](float S) { return (S >= 200.0f && S <= 260.0f) || (S >= 280.0f && S <= 340.0f); });

		Ring.MinWallSegment = 0.0f;
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("Without the filter a wall sliver keeps the two runs apart"), Runs.Num(), 2);

		Ring.MinWallSegment = 30.0f;
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("A sub-minimum wall sliver merges the two runs"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			TestTrue(TEXT("The merged run spans both roads"),
				Runs[0].S0 < 200.0f && Runs[0].S1 > 340.0f);
		}
	}

	// ---- ⑪ 绕回那一段也要能并（首段与末段之间跨过 Hi 的空隙）----
	{
		MakeRing([](float S) { return (S >= 10.0f && S <= 90.0f) || (S >= 700.0f && S <= 780.0f); });

		Ring.MinWallSegment = 0.0f;
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("Without the filter the two runs straddle the origin separately"), Runs.Num(), 2);

		Ring.MinWallSegment = 40.0f;
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("The gap across the ring origin merges too"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			TestTrue(TEXT("The merged wrapping run is expressed with S0 < Lo"), Runs[0].S0 < 0.0f);
			TestTrue(TEXT("The merged wrapping run never exceeds one period"),
				Runs[0].Width() <= Period + 1.0e-3f);
		}
	}

	// ---- ⑫ 拱廊的墩不受它影响：环上根本看不到那些空隙（`MaxWidth = 0` 时才是产线口径）----
	{
		MakeRing([](float S) { return S >= 150.0f && S <= 600.0f; });
		Ring.MinWallSegment = 60.0f;   // 远大于 PierWidth = 20
		Ring.MaxWidth = 0.0f;          // 产线在环上不切，切分放到按边切完之后
		CSHouse_SolveRoadRuns(Weights, 0.0f, RingStep, Ring, TArrayView<const FCSDoorRun>(), Runs);
		TestEqual(TEXT("One wide road stays one ring run"), Runs.Num(), 1);
		if (Runs.Num() == 1)
		{
			// 切分由调用方按边切完之后再做，墩宽因此永远不会被 MinWallSegment 并掉。
			TArray<FCSDoorRun> Split;
			CSHouse_SplitRun(Runs[0], 160.0f, 20.0f, 32.0f, Split);
			TestTrue(TEXT("The downstream split still leaves piers between the arches"), Split.Num() >= 3);
			for (int32 Index = 0; Index + 1 < Split.Num(); ++Index)
			{
				TestTrue(TEXT("Split arches keep the pier gap"),
					FMath::IsNearlyEqual(Split[Index + 1].S0 - Split[Index].S1, 20.0f, 0.5f));
			}
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// 扁拱（`ArchRise` ≠ 半宽）：剖面与裁剪场必须描述**同一条**曲线
//
// 这一条是整个 D6 里最容易静默错的地方：门框砖沿 `CSHouse_SampleOpeningProfile` 摆，
// 墙面的洞由 `CSHouse_ClipKeeps`（以及材质里它的逐字翻译）切。两者一旦不同式，
// 症状是"砖沿着一条弧走、洞却是另一条弧"，**没有任何断言或报错**会响。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseArchRiseTest,
	"PCGPlugins.ComputeShaderGenerator.House.ArchRise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseArchRiseTest::RunTest(const FString& Parameters)
{
	auto MakeArch = [](float Width, float Rise)
	{
		FCSWallOpening O;
		O.Type = ECSOpeningType::Door;
		O.Shape = ECSOpeningShape::Arch;
		O.CenterS = 300.0f;
		O.Width = Width;
		O.Z0 = 0.0f;
		O.Z1 = 220.0f;
		O.ArchRise = Rise;
		return O;
	};

	// ---- ① ArchRise = 0 ⇒ 逐位退回正半圆（旧行为不许被这次改动动到）----
	{
		const FCSWallOpening O = MakeArch(200.0f, 0.0f);
		TestTrue(TEXT("Rise defaults to the half width"), FMath::IsNearlyEqual(O.Rise(), 100.0f, 1.0e-3f));
		const FCSOpeningClipField F = CSHouse_ComputeClipField(O);
		TestTrue(TEXT("A default arch is still a true semicircle"),
			FMath::IsNearlyEqual(F.InvScaleZ, 1.0f / 100.0f, 1.0e-5f));
	}

	// ---- ② 扁拱：剖面的每个顶点都落在裁剪场的边界上 ----
	for (const float Rise : { 40.0f, 70.0f, 100.0f, 160.0f })
	{
		const FCSWallOpening O = MakeArch(240.0f, Rise);
		const FCSOpeningClipField F = CSHouse_ComputeClipField(O);
		TArray<FCSOpeningProfileSample> Samples;
		CSHouse_SampleOpeningProfile(O, 1.0f, Samples);
		TestTrue(TEXT("A flattened arch still yields a profile"), Samples.Num() >= 6);

		float WorstOutside = 0.0f;
		for (const FCSOpeningProfileSample& Sample : Samples)
		{
			// 剖面顶点是**外接**多边形（弦高补偿），所以它恒在解析曲线之外或之上；
			// 判据是"不许落到曲线里面"，而不是"恰好在曲线上"。
			const FVector2f Q = F.Eval(Sample.S, Sample.ZHigh);
			const float Radial = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y);
			if (Q.Y > 0.0f) WorstOutside = FMath::Max(WorstOutside, 1.0f - Radial);
		}
		TestTrue(FString::Printf(TEXT("Rise %.0f: no profile vertex falls inside the clip curve"), Rise),
			WorstOutside < 1.0e-3f);
	}

	// ---- ③ 扁拱的洞**确实更矮**：拱脚以上正中那一列，半圆保留不了的高度扁拱要保留 ----
	{
		const FCSWallOpening Flat = MakeArch(240.0f, 50.0f);
		const FCSWallOpening Round = MakeArch(240.0f, 0.0f);       // Rise = 120
		const FCSOpeningClipField FF = CSHouse_ComputeClipField(Flat);
		const FCSOpeningClipField RF = CSHouse_ComputeClipField(Round);

		// 洞心正上方 Z = 200：半圆（拱脚 100、顶 220）还在洞里；扁拱（拱脚 170、顶 220）也在洞里。
		TestFalse(TEXT("Round arch keeps the crown open at Z=200"), CSHouse_ClipKeeps(RF, RF.Eval(300.0f, 200.0f)));
		TestFalse(TEXT("Flat arch keeps the crown open at Z=200"), CSHouse_ClipKeeps(FF, FF.Eval(300.0f, 200.0f)));

		// 洞**边缘**附近 S = 300 + 110（离中心 110，半宽 120）、Z = 200：
		// 半圆在这个高度还没收拢（弧很陡）⇒ 仍是洞；扁拱早就收到 Z=220 附近了 ⇒ 已是墙。
		TestTrue(TEXT("Flat arch has already closed near the springing edge"),
			CSHouse_ClipKeeps(FF, FF.Eval(410.0f, 200.0f)));

		// 两者的拱脚高不同 —— 这正是"扁"的定义。
		TestTrue(TEXT("Flat arch springs higher than the round one"), FF.RefZ > RF.RefZ + 50.0f);
	}

	// ---- ④ Rise 被夹在洞高之内：给一个荒谬的大值也不许把拱脚推到洞底以下 ----
	{
		const FCSWallOpening O = MakeArch(200.0f, 5000.0f);
		TestTrue(TEXT("Rise is clamped to the opening height"), O.Rise() <= 220.0f + 1.0e-3f);
		const FCSOpeningClipField F = CSHouse_ComputeClipField(O);
		TestTrue(TEXT("Clamped rise keeps the springing at or above the sill"), F.RefZ >= -1.0e-3f);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 离地收窄：连续、单调、两端夹紧
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDoorWidthScaleTest,
	"PCGPlugins.ComputeShaderGenerator.House.DoorWidthScale",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDoorWidthScaleTest::RunTest(const FString& Parameters)
{
	constexpr float Full = 30.0f, Zero = 120.0f;

	TestTrue(TEXT("Flush with the ground is full width"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(0.0f, Full, Zero), 1.0f));
	TestTrue(TEXT("At the full-width threshold it is still full"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(Full, Full, Zero), 1.0f));
	TestTrue(TEXT("At the zero threshold it is gone"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(Zero, Full, Zero), 0.0f));
	TestTrue(TEXT("Past the zero threshold it stays gone"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(500.0f, Full, Zero), 0.0f));
	TestTrue(TEXT("Halfway is half width"), FMath::IsNearlyEqual(ACSHouseActor::ComputeDoorWidthScale(75.0f, Full, Zero), 0.5f, 1.0e-4f));

	// 单调不增 —— 抬得越高门只会越窄，不会反弹（"没有二值跳变"的形式化）。
	float Previous = 1.0f;
	for (int32 Step = 0; Step <= 100; ++Step)
	{
		const float Gap = Step * 2.0f;
		const float Scale = ACSHouseActor::ComputeDoorWidthScale(Gap, Full, Zero);
		TestTrue(TEXT("Width scale never increases with the gap"), Scale <= Previous + 1.0e-6f);
		TestTrue(TEXT("Width scale stays in [0, 1]"), Scale >= 0.0f && Scale <= 1.0f);
		Previous = Scale;
	}

	// 参数被填反 / 相等时不除零，退化成硬阈。
	TestTrue(TEXT("Degenerate thresholds do not divide by zero"),
		FMath::IsFinite(ACSHouseActor::ComputeDoorWidthScale(50.0f, 100.0f, 100.0f)));

	return true;
}

// -----------------------------------------------------------------------------
// 洞的剖面：分段数自适应
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseProfileSegmentsTest,
	"PCGPlugins.ComputeShaderGenerator.House.ProfileSegments",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseProfileSegmentsTest::RunTest(const FString& Parameters)
{
	// 计划里算过的那个例子：Tol = 0.2、R = 55 → 19 段。
	// 公式漏掉分母那个 2 会给出 37 —— 这条断言就是钉死它的。
	TestEqual(TEXT("Tol 0.2 at R 55 gives 19 segments"), CSHouse_ProfileSegments(55.0f, 0.2f), 19);

	// 半径越大越需要更多段（同一绝对弦高容差下），且始终夹在 [6, 48]。
	int32 Previous = 0;
	for (int32 Step = 1; Step <= 40; ++Step)
	{
		const float R = Step * 20.0f;
		const int32 N = CSHouse_ProfileSegments(R, 0.2f);
		TestTrue(TEXT("Segment count is monotonic in radius"), N >= Previous);
		TestTrue(TEXT("Segment count stays within the clamp"), N >= 6 && N <= 48);
		Previous = N;
	}

	// 实际弦高确实落在容差内（除非撞上 48 段的上限）—— 这才是段数存在的理由。
	for (const float R : { 30.0f, 55.0f, 120.0f })
	{
		const int32 N = CSHouse_ProfileSegments(R, 0.2f);
		if (N >= 48) continue;
		const float Sagitta = R * (1.0f - FMath::Cos(PI / (2.0f * N)));
		TestTrue(TEXT("Actual chord sagitta is within tolerance"), Sagitta <= 0.2f + 1.0e-4f);
	}

	// 退化输入不炸。
	TestTrue(TEXT("Zero radius is clamped"), CSHouse_ProfileSegments(0.0f, 0.2f) >= 6);
	TestTrue(TEXT("Zero tolerance is clamped"), CSHouse_ProfileSegments(55.0f, 0.0f) <= 48);

	return true;
}

// -----------------------------------------------------------------------------
// 洞的剖面：采样形状
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseProfileSampleTest,
	"PCGPlugins.ComputeShaderGenerator.House.ProfileSample",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseProfileSampleTest::RunTest(const FString& Parameters)
{
	TArray<FCSOpeningProfileSample> Samples;

	// ---- 拱：矩形下身 + 半圆顶 ----
	{
		FCSWallOpening Arch;
		Arch.Shape = ECSOpeningShape::Arch;
		Arch.CenterS = 300.0f;
		Arch.Width = 110.0f;
		Arch.Z0 = 0.0f;
		Arch.Z1 = 220.0f;
		CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);

		TestTrue(TEXT("Arch yields samples"), Samples.Num() >= 2);

		// S 严格单调递增 —— 墙板砌筑与内壁扫掠都依赖这条（否则条带会自交）。
		for (int32 K = 1; K < Samples.Num(); ++K)
		{
			TestTrue(TEXT("Profile S is strictly increasing"), Samples[K].S > Samples[K - 1].S);
		}

		// 洞底恒为 Z0（矩形下身），洞顶在中点最高。
		for (const FCSOpeningProfileSample& Sample : Samples)
		{
			TestTrue(TEXT("Arch floor stays at Z0"), FMath::IsNearlyEqual(Sample.ZLow, 0.0f));
			TestTrue(TEXT("Arch never exceeds its top"), Sample.ZHigh <= Arch.Z1 + 1.0f);
		}
		const int32 Mid = Samples.Num() / 2;
		TestTrue(TEXT("Arch peaks at the centre"), FMath::IsNearlyEqual(Samples[Mid].ZHigh, Arch.Z1, 1.0f));

		// **外接而非内接**：折线端点比标称半宽略宽（1/cos(π/2N)），微覆盖不漏缝。
		const float Span = Samples.Last().S - Samples[0].S;
		TestTrue(TEXT("Circumscribed profile is slightly wider than nominal"), Span > Arch.Width);
		TestTrue(TEXT("...but only slightly"), Span < Arch.Width * 1.02f);
	}

	// ---- 矩形：两个样本，上下边界都是常数 ----
	{
		FCSWallOpening Rect;
		Rect.Shape = ECSOpeningShape::Rect;
		Rect.CenterS = 200.0f;
		Rect.Width = 90.0f;
		Rect.Z0 = 110.0f;   // 窗台高
		Rect.Z1 = 210.0f;
		CSHouse_SampleOpeningProfile(Rect, 0.2f, Samples);

		TestEqual(TEXT("Rect needs only two samples"), Samples.Num(), 2);
		TestTrue(TEXT("Rect spans exactly its nominal width"), FMath::IsNearlyEqual(Samples.Last().S - Samples[0].S, 90.0f, 1.0e-3f));
		TestTrue(TEXT("Rect sill is flat"), FMath::IsNearlyEqual(Samples[0].ZLow, 110.0f) && FMath::IsNearlyEqual(Samples[1].ZLow, 110.0f));
		TestTrue(TEXT("Rect head is flat"), FMath::IsNearlyEqual(Samples[0].ZHigh, 210.0f) && FMath::IsNearlyEqual(Samples[1].ZHigh, 210.0f));
	}

	// ---- 圆：两端闭合（ZLow == ZHigh），中点最高最低 ----
	{
		FCSWallOpening Circle;
		Circle.Shape = ECSOpeningShape::Circle;
		Circle.CenterS = 250.0f;
		Circle.Width = 80.0f;
		Circle.Z0 = 160.0f;
		Circle.Z1 = 240.0f;
		CSHouse_SampleOpeningProfile(Circle, 0.2f, Samples);

		TestTrue(TEXT("Circle yields samples"), Samples.Num() >= 4);
		TestTrue(TEXT("Circle closes on the left"), FMath::IsNearlyEqual(Samples[0].ZLow, Samples[0].ZHigh, 1.0e-3f));
		TestTrue(TEXT("Circle closes on the right"), FMath::IsNearlyEqual(Samples.Last().ZLow, Samples.Last().ZHigh, 1.0e-3f));
		const int32 Mid = Samples.Num() / 2;
		TestTrue(TEXT("Circle is widest at the centre"), Samples[Mid].ZHigh - Samples[Mid].ZLow > 70.0f);
	}

	// ---- Skew：洞顶沿 S 线性倾斜（楼梯口），洞底不动 ----
	{
		FCSWallOpening Stair;
		Stair.Shape = ECSOpeningShape::Rect;
		Stair.Type = ECSOpeningType::Stair;
		Stair.CenterS = 300.0f;
		Stair.Width = 100.0f;
		Stair.Z0 = 50.0f;
		Stair.Z1 = 200.0f;
		Stair.Skew = 0.5f;
		CSHouse_SampleOpeningProfile(Stair, 0.2f, Samples);

		TestEqual(TEXT("Skewed rect still has two samples"), Samples.Num(), 2);
		TestTrue(TEXT("Skew tilts the head down on the near side"), FMath::IsNearlyEqual(Samples[0].ZHigh, 200.0f - 25.0f, 1.0e-3f));
		TestTrue(TEXT("Skew tilts the head up on the far side"), FMath::IsNearlyEqual(Samples[1].ZHigh, 200.0f + 25.0f, 1.0e-3f));
		TestTrue(TEXT("Skew leaves the sill alone"), FMath::IsNearlyEqual(Samples[0].ZLow, Samples[1].ZLow));
	}

	// 无效洞（零宽 / 上下颠倒）产出空表，调用方据此跳过。
	{
		FCSWallOpening Bad;
		Bad.Width = 0.0f;
		CSHouse_SampleOpeningProfile(Bad, 0.2f, Samples);
		TestEqual(TEXT("A zero-width opening yields nothing"), Samples.Num(), 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 洞的重叠谓词：同边**一维 S 区间**，比的是面板格，Z 不参与
//
// 用户裁决 2026-08-30（C1 选甲）：永久放弃"门上开窗"。谓词必须与 CSHouse_BuildBodySoup 的
// 单游标扫掠同维 —— 二维判据会放行几何砌不出来的堆叠，那正是 D8「谓词是唯一真源」要防的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseOpeningOverlapTest,
	"PCGPlugins.ComputeShaderGenerator.House.OpeningOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseOpeningOverlapTest::RunTest(const FString& Parameters)
{
	// 门与窗各自的面板格 = 半宽 + 半个墩（CSHouse_OpeningCell），墩宽取项目默认。
	constexpr float Pier = 40.0f;
	constexpr float Clear = 10.0f;

	FCSWallOpening Door;
	Door.Type = ECSOpeningType::Door;
	Door.EdgeIndex = 1;
	Door.CenterS = 300.0f;
	Door.Width = 110.0f;
	Door.Z0 = 0.0f;
	Door.Z1 = 220.0f;

	// 同一 S 位置的高窗：**冲突**。这正是裁决放弃掉的那件事 —— 墙板是沿 S 的单游标扫掠，
	// 门上方那扇窗砌不出来，所以谓词必须当场拒绝，而不是放行到几何里静默消失。
	FCSWallOpening HighWindow;
	HighWindow.Type = ECSOpeningType::Window;
	HighWindow.EdgeIndex = 1;
	HighWindow.CenterS = 300.0f;
	HighWindow.Width = 60.0f;
	HighWindow.Z0 = 250.0f;
	HighWindow.Z1 = 300.0f;
	TestTrue(TEXT("A high window conflicts with a low door at the same S"),
		CSHouse_OpeningsOverlap(Door, HighWindow, Pier, Clear));

	// Z 一律不参与：把窗抬到天上仍然冲突。这条是回归守卫 —— 二维判据被悄悄加回来就会变红。
	FCSWallOpening WayUp = HighWindow;
	WayUp.Z0 = 1000.0f;
	WayUp.Z1 = 1060.0f;
	TestTrue(TEXT("Z plays no part in the predicate"),
		CSHouse_OpeningsOverlap(Door, WayUp, Pier, Clear));

	// 两洞本身让开了（净空 20 > Clearance），但两**格**仍相交 —— 扫掠会把窗的左半边留给
	// 前一块无 clip 的面板，所以照样判冲突。比格不比洞的理由就在这一条。
	FCSWallOpening CellClash = HighWindow;
	CellClash.CenterS = 300.0f + 110.0f * 0.5f + 60.0f * 0.5f + 20.0f;
	TestTrue(TEXT("Openings that clear each other but whose panel cells overlap still conflict"),
		CSHouse_OpeningsOverlap(Door, CellClash, Pier, Clear));

	// 让开一个整墩再加净距才放行。
	constexpr float CellTouch = 110.0f * 0.5f + 60.0f * 0.5f + Pier;   // 两格刚好相接的中心距
	FCSWallOpening Beside = HighWindow;
	Beside.CenterS = 300.0f + CellTouch + 15.0f;   // 格间净距 15 > Clearance
	TestFalse(TEXT("A full pier plus the clearance band apart is fine"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear));
	Beside.CenterS = 300.0f + CellTouch + 5.0f;    // 格间净距 5 < Clearance
	TestTrue(TEXT("Inside the clearance band conflicts"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear));

	// 不同边永远不冲突（洞只认自己那条边缘线段）。
	FCSWallOpening OtherEdge = HighWindow;
	OtherEdge.EdgeIndex = 2;
	TestFalse(TEXT("Different edges never conflict"),
		CSHouse_OpeningsOverlap(Door, OtherEdge, Pier, Clear));

	// 对称性：谓词不能因为参数顺序而改口。
	TestEqual(TEXT("Overlap is symmetric"),
		CSHouse_OpeningsOverlap(Door, Beside, Pier, Clear), CSHouse_OpeningsOverlap(Beside, Door, Pier, Clear));

	return true;
}

// -----------------------------------------------------------------------------
// 解析裁剪场：判据与剖面必须描述同一条曲线
//
// 这是 clip 路线的正确性支点。剖面（CSHouse_SampleOpeningProfile）供洞口内壁扫掠，
// 判据（CSHouse_ClipKeeps）供材质逐像素切洞 —— 两者若不同式，就会出现"内壁贴在这里、
// 洞却切在那里"的穿帮，且不报任何错。这里用剖面的采样点去打判据的边界来钉死它。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseClipFieldTest,
	"PCGPlugins.ComputeShaderGenerator.House.ClipField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseClipFieldTest::RunTest(const FString& Parameters)
{
	// ---- 拱：矩形下身 + 半圆顶 ----
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = 300.0f;
	Arch.Width = 110.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;

	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Arch);
	TestTrue(TEXT("Arch yields a valid clip field"), Field.bValid);

	const float HW = Arch.HalfWidth();
	const float SpringZ = Arch.Z1 - HW;

	// 洞正中偏下：一定在洞内（被 discard）。
	TestFalse(TEXT("The middle of the doorway is clipped away"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, SpringZ * 0.5f)));
	// 拱顶正下方一点：仍在洞内。
	TestFalse(TEXT("Just under the crown is clipped away"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, Arch.Z1 - 2.0f)));
	// 拱顶之上：保留。
	TestTrue(TEXT("Above the crown is kept"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS, Arch.Z1 + 2.0f)));
	// 洞外侧向：保留 —— 这条就是"墩不被误切"。
	TestTrue(TEXT("Beyond the jamb is kept"),
		CSHouse_ClipKeeps(Field, Field.Eval(Arch.CenterS + HW + 2.0f, SpringZ * 0.5f)));

	// 面板端盖必须落在洞外，否则墩会被切掉。|q.x| = (HW + PierWidth/2)/HW > 1。
	{
		float CellMin = 0, CellMax = 0;
		CSHouse_OpeningCell(Arch, 40.0f, CellMin, CellMax);
		TestTrue(TEXT("The panel is wider than the opening"), CellMax - CellMin > Arch.Width);
		for (const float Z : { 5.0f, SpringZ * 0.5f, SpringZ, Arch.Z1 - 1.0f })
		{
			TestTrue(TEXT("The panel's left end cap survives the clip"), CSHouse_ClipKeeps(Field, Field.Eval(CellMin, Z)));
			TestTrue(TEXT("The panel's right end cap survives the clip"), CSHouse_ClipKeeps(Field, Field.Eval(CellMax, Z)));
		}
	}

	// **判据边界 == 剖面曲线**：剖面上边界的采样点应当恰好落在判据的临界面上。
	// 剖面按外接建（比精确曲线略外扩 1/cos(π/2N)），所以它们全都刚好在"洞外"一侧 ——
	// 这正是想要的方向：内壁微覆盖，绝不漏缝。
	{
		TArray<FCSOpeningProfileSample> Samples;
		CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);
		TestTrue(TEXT("Arch profile yields samples"), Samples.Num() >= 2);

		float WorstOutside = 0.0f;
		for (const FCSOpeningProfileSample& Sample : Samples)
		{
			const FVector2f Q = Field.Eval(Sample.S, Sample.ZHigh);
			TestTrue(TEXT("Every profile crown point is on the kept side"), CSHouse_ClipKeeps(Field, Q));
			// 半圆段上 |q| 应当≈1（外接的那点余量之内）。
			if (Sample.ZHigh > SpringZ + 0.5f)
			{
				const float R = FMath::Sqrt(Q.X * Q.X + Q.Y * Q.Y);
				WorstOutside = FMath::Max(WorstOutside, R - 1.0f);
				TestTrue(TEXT("Crown points sit on the analytic circle"), FMath::Abs(R - 1.0f) < 0.02f);
			}
		}
		TestTrue(TEXT("The profile circumscribes rather than inscribes"), WorstOutside >= 0.0f);
	}

	// ---- 矩形 ----
	FCSWallOpening Rect;
	Rect.Shape = ECSOpeningShape::Rect;
	Rect.CenterS = 200.0f;
	Rect.Width = 90.0f;
	Rect.Z0 = 110.0f;
	Rect.Z1 = 210.0f;
	const FCSOpeningClipField RectField = CSHouse_ComputeClipField(Rect);
	TestFalse(TEXT("The middle of the window is clipped away"),
		CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 160.0f)));
	TestTrue(TEXT("Below the sill is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 105.0f)));
	TestTrue(TEXT("Above the head is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(200.0f, 215.0f)));
	TestTrue(TEXT("Beside the reveal is kept"), CSHouse_ClipKeeps(RectField, RectField.Eval(250.0f, 160.0f)));

	// ---- 圆 ----
	FCSWallOpening Circle;
	Circle.Shape = ECSOpeningShape::Circle;
	Circle.CenterS = 250.0f;
	Circle.Width = 80.0f;
	Circle.Z0 = 160.0f;
	Circle.Z1 = 240.0f;
	const FCSOpeningClipField CircleField = CSHouse_ComputeClipField(Circle);
	TestFalse(TEXT("The middle of the oculus is clipped away"),
		CSHouse_ClipKeeps(CircleField, CircleField.Eval(250.0f, 200.0f)));
	// 圆的四个方向都在洞外一点点即保留。
	for (const FVector2D& Dir : { FVector2D(1, 0), FVector2D(-1, 0), FVector2D(0, 1), FVector2D(0, -1) })
	{
		const float S = 250.0f + float(Dir.X) * 42.0f;
		const float Z = 200.0f + float(Dir.Y) * 42.0f;
		TestTrue(TEXT("Just outside the oculus is kept"), CSHouse_ClipKeeps(CircleField, CircleField.Eval(S, Z)));
	}

	// ---- 无洞面板：哨兵 (8, 8) 在任何判据下都保留 ----
	{
		const FCSOpeningClipField Empty;
		TestFalse(TEXT("An empty field is not valid"), Empty.bValid);
		const FVector2f Q = Empty.Eval(123.0f, 45.0f);
		TestTrue(TEXT("The sentinel is the documented (8, 8)"), FMath::IsNearlyEqual(Q.X, 8.0f) && FMath::IsNearlyEqual(Q.Y, 8.0f));
		TestTrue(TEXT("A panel with no opening is kept everywhere"), CSHouse_ClipKeeps(Empty, Q));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 门框曲线的控制点必须等弧长
//
// 散布 kernel 把记录里的 alpha 当**样条参数**用，而 BuildFramePlan 排砖时算的是**弧长
// 占比** —— 两者只有在控制点等距时才等价。原始的 U 形洞缘折线（门樘一整段 160cm、
// 拱缘每段 9.4cm）疏密比 16.9:1，砖会全挤到拱上互相穿模成薄鳍片。这条用例直接用真实
// 的拱剖面走一遍 BuildFramePlan 的组路逻辑，把"重采样后必须等距"钉死。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameCurveUniformTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameCurveUniform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameCurveUniformTest::RunTest(const FString& Parameters)
{
	// 演示关卡里那扇门：600 长墙、3 个拱位 ⇒ 宽 120、高 220 ⇒ 拱脚 160。
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = 140.0f;
	Arch.Width = 120.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;

	TArray<FCSOpeningProfileSample> Samples;
	CSHouse_SampleOpeningProfile(Arch, 0.2f, Samples);
	TestTrue(TEXT("Arch profile yields samples"), Samples.Num() >= 2);

	// BuildFramePlan 的①号路径：左樘底 → 沿洞顶曲线 → 右樘底。
	TArray<FVector> Path;
	Path.Add(FVector(Samples[0].S, 0.0, Samples[0].ZLow));
	for (const FCSOpeningProfileSample& S : Samples) Path.Add(FVector(S.S, 0.0, S.ZHigh));
	Path.Add(FVector(Samples.Last().S, 0.0, Samples.Last().ZLow));

	auto SegmentLengths = [](const TArray<FVector>& Points)
	{
		TArray<double> Out;
		for (int32 K = 1; K < Points.Num(); ++K) Out.Add(FVector::Dist(Points[K - 1], Points[K]));
		return Out;
	};

	// 先证明"原样喂进去确实是坏的"，否则这条用例可能在两边都通过而钉不住任何东西。
	{
		const TArray<double> Raw = SegmentLengths(Path);
		double RawMin = TNumericLimits<double>::Max(), RawMax = 0.0;
		for (const double L : Raw) { RawMin = FMath::Min(RawMin, L); RawMax = FMath::Max(RawMax, L); }
		TestTrue(TEXT("The raw opening polyline is wildly non-uniform"), RawMax / RawMin > 8.0);
	}

	double RawLength = 0.0;
	for (const double L : SegmentLengths(Path)) RawLength += L;

	const float BrickLength = 26.0f;
	const int32 Segments = FMath::Clamp(
		FMath::CeilToInt(RawLength / FMath::Max(BrickLength * 0.5f, 1.0f)), 4, 128);

	TArray<FVector> Even;
	CSShaperSteps::ResampleUniform(Path, Segments, Even);

	TestEqual(TEXT("Resampling yields SegmentCount + 1 points"), Even.Num(), Segments + 1);
	// 端点必须逐位保留：两端外延一格 + 只取内区间那条纪律全靠它。
	TestTrue(TEXT("The first control point is untouched"), Even[0].Equals(Path[0], 1.0e-6));
	TestTrue(TEXT("The last control point is untouched"), Even.Last().Equals(Path.Last(), 1.0e-6));

	const TArray<double> Lengths = SegmentLengths(Even);
	double MinLen = TNumericLimits<double>::Max(), MaxLen = 0.0, Sum = 0.0;
	for (const double L : Lengths) { MinLen = FMath::Min(MinLen, L); MaxLen = FMath::Max(MaxLen, L); Sum += L; }
	// 弦长只在拐弯处比目标短一丝（折线内接圆弧），1% 已经宽松得能容下任何洞形。
	TestTrue(TEXT("Every control segment is the same length"), MaxLen / MinLen < 1.01);
	TestTrue(TEXT("Resampling preserves the arc length"), FMath::Abs(Sum - RawLength) < RawLength * 0.01);

	// 重采样点必须仍然贴在设计曲线上（拱段在解析圆上、门樘段在竖直线上）。
	const double SpringZ = Arch.Z1 - Arch.HalfWidth();
	const double Circum = Samples.Last().S - Arch.CenterS;   // 外接半径（剖面按外接建，略大于半宽）
	for (const FVector& P : Even)
	{
		if (P.Z > SpringZ + 0.5)
		{
			const double Radius = FMath::Sqrt(FMath::Square(P.X - Arch.CenterS) + FMath::Square(P.Z - SpringZ));
			TestTrue(TEXT("Crown samples stay on the analytic circle"), FMath::Abs(Radius - Circum) < 0.3);
		}
		else
		{
			const double Drift = FMath::Min(FMath::Abs(P.X - Path[0].X), FMath::Abs(P.X - Path.Last().X));
			TestTrue(TEXT("Jamb samples stay on the vertical"), Drift < 0.3);
		}
	}

	// 退化输入：点太少 / 零长度折线时原样返回，调用方的 Num() < 3 早退才有意义。
	{
		TArray<FVector> Degenerate = { FVector(1.0, 2.0, 3.0) };
		TArray<FVector> Result;
		CSShaperSteps::ResampleUniform(Degenerate, 8, Result);
		TestEqual(TEXT("A single point is returned untouched"), Result.Num(), 1);

		TArray<FVector> Coincident = { FVector::ZeroVector, FVector::ZeroVector };
		CSShaperSteps::ResampleUniform(Coincident, 8, Result);
		TestEqual(TEXT("A zero-length polyline is returned untouched"), Result.Num(), 2);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 解析推导（裁决一选乙）的三条断言：闭式砖数、与旧路等价、墩上只有一列砖
// -----------------------------------------------------------------------------

namespace
{
/**
 * 旧路（B 样条 + 逐砖记录）在墙空间摆出来的**砖心**。⚠️ **旧路的产线代码已经删掉**
 * （2026-08-30 裁决一第二步：`ACSHouseActor::BuildFramePlan` / `CSShaperSteps::EnsureCapacity`
 * / `CSShaperSteps::Scatter` / `CSGroundSteps.usf` / `csh.FrameLegacy` 全部不在了），
 * 所以这一份**测试内部的 CPU 镜像**从此就是旧路的唯一存本。它逐字镜像当时的三处：
 *   · 组路 / 重采样 / 排砖：`ACSHouseActor::BuildFramePlan` 里的 `EmitCurve`（已删）
 *   · 均匀三次 B 样条：`General.usf:332-361`（`BSplinePosition` / `BSplineMapGlobalAlpha`，仍在）
 *   · alpha → 采样插值：`CSGroundSteps.usf` 的 I0/I1 线性插值（已删）
 *
 * **它不调用任何产线代码**，这正是删掉旧路之后 `House.FrameAnalyticMatchesLegacy` 仍然成立
 * 的原因 —— 也是这条断言必须活下来的原因：解析推导有没有跑偏，只剩它一个守卫。
 * 唯一还与产线共用的是 `CSHouse_SampleOpeningProfile` / `CSShaperSteps::ResampleUniform` /
 * `ACSSplineBlockActor::SolveBlockLayout` 三个纯函数，它们因此**不许跟着旧路一起删**。
 */
void CSHouseTest_LegacyFrameBricks(const FCSWallOpening& Opening, float ChordTolerance,
	float BrickLength, float BrickGap, int32 SeedSalt, TArray<FVector2D>& OutSZ,
	double& OutControlSpacing)
{
	OutSZ.Reset();
	OutControlSpacing = 0.0;

	TArray<FCSOpeningProfileSample> Samples;
	CSHouse_SampleOpeningProfile(Opening, ChordTolerance, Samples);
	if (Samples.Num() < 2) return;

	// ①号路径：左樘底 → 沿洞顶曲线 → 右樘底。墙空间 (S, Z) 摊成 (X, _, Z)。
	TArray<FVector> Path;
	Path.Add(FVector(Samples[0].S, 0.0, Samples[0].ZLow));
	for (const FCSOpeningProfileSample& S : Samples) Path.Add(FVector(S.S, 0.0, S.ZHigh));
	Path.Add(FVector(Samples.Last().S, 0.0, Samples.Last().ZLow));

	double RawLength = 0.0;
	for (int32 K = 1; K < Path.Num(); ++K) RawLength += FVector::Dist(Path[K - 1], Path[K]);
	if (RawLength < BrickLength * 0.5) return;

	const int32 Segments = FMath::Clamp(
		FMath::CeilToInt(RawLength / FMath::Max(BrickLength * 0.5f, 1.0f)), 4, 128);
	TArray<FVector> Even;
	CSShaperSteps::ResampleUniform(Path, Segments, Even);
	if (Even.Num() < 3) return;

	double ArcLength = 0.0;
	for (int32 K = 1; K < Even.Num(); ++K) ArcLength += FVector::Dist(Even[K - 1], Even[K]);
	if (ArcLength < BrickLength * 0.5) return;
	// 控制点间距 h：均匀三次 B 样条把半径 R 的圆弧内缩 h²/(6R)，等价性判据要的就是这个 h。
	OutControlSpacing = ArcLength / Segments;

	FRandomStream Rand(1 * 7919 + SeedSalt);
	TArray<int32> Sequence;
	const TArray<float> Palette = { FMath::Max(BrickLength, 1.0f) };
	const float LayoutScale = ACSSplineBlockActor::SolveBlockLayout(float(ArcLength), BrickGap, Palette, Rand, Sequence);
	if (Sequence.IsEmpty() || LayoutScale <= 0.0f) return;

	// 两端各外延一格：均匀三次 B 样条在控制点处取 (P₋₁ + 4P₀ + P₁)/6，外延点选成
	// 2·E₀ − E₁ 时这一式恰好还原 E₀ —— 端点因此逐位钉在设计位置上。
	TArray<FVector> Ctrl;
	Ctrl.Reserve(Even.Num() + 2);
	Ctrl.Add(Even[0] + (Even[0] - Even[1]));
	Ctrl.Append(Even);
	Ctrl.Add(Even.Last() + (Even.Last() - Even[Even.Num() - 2]));

	const int32 NumCtrl = Ctrl.Num();
	const double Alpha0 = 1.0 / (NumCtrl - 1);
	const double Alpha1 = double(NumCtrl - 2) / (NumCtrl - 1);
	const int32 SampleCount = FMath::Clamp((NumCtrl - 1) * 8, 16, 512);

	auto BSplineAt = [&Ctrl, NumCtrl](double A)
	{
		const int32 NumSeg = FMath::Max(NumCtrl - 1, 1);
		const double G = FMath::Clamp(A, 0.0, 1.0) * NumSeg;
		const int32 Seg = FMath::Min(int32(FMath::FloorToDouble(G)), NumSeg - 1);
		const double T = G - Seg;
		auto Load = [&Ctrl, NumCtrl](int32 I) { return Ctrl[FMath::Clamp(I, 0, NumCtrl - 1)]; };
		const double T2 = T * T, T3 = T2 * T;
		return (-T3 + 3.0 * T2 - 3.0 * T + 1.0) / 6.0 * Load(Seg - 1)
			+ (3.0 * T3 - 6.0 * T2 + 4.0) / 6.0 * Load(Seg)
			+ (-3.0 * T3 + 3.0 * T2 + 3.0 * T + 1.0) / 6.0 * Load(Seg + 1)
			+ T3 / 6.0 * Load(Seg + 2);
	};

	TArray<FVector> Curve;
	Curve.Reserve(SampleCount);
	for (int32 K = 0; K < SampleCount; ++K) Curve.Add(BSplineAt(double(K) / double(SampleCount - 1)));

	double Cursor = 0.0;
	for (int32 Index = 0; Index < Sequence.Num(); ++Index)
	{
		const double BlockLength = Palette[Sequence[Index]] * LayoutScale;
		const double MidArc = Cursor + BlockLength * 0.5;
		Cursor += BlockLength + BrickGap * LayoutScale;
		const double Frac = FMath::Clamp(MidArc / ArcLength, 0.0, 1.0);
		const double Alpha = Alpha0 + Frac * (Alpha1 - Alpha0);

		// 旧 kernel 的采样插值（已删的 `CSGroundSteps.usf`）：alpha → 均匀采样序号 → 相邻两点线性插值。
		const int32 Last = SampleCount - 1;
		const double F = Alpha * Last;
		const int32 I0 = FMath::Min(int32(F), Last);
		const int32 I1 = FMath::Min(I0 + 1, Last);
		const FVector P = FMath::Lerp(Curve[I0], Curve[I1], FMath::Clamp(F - I0, 0.0, 1.0));
		OutSZ.Add(FVector2D(P.X, P.Z));
	}
}

/** 解析路（新路）在墙空间摆出来的砖心。墙框架取单位框架，比较才不掺进任何世界变换。 */
void CSHouseTest_AnalyticFrameBricks(TArrayView<const FCSWallOpening> Openings,
	float BrickLength, float BrickGap, TArray<CSHouseFrame::FElement>& OutElements, TArray<FVector2D>& OutSZ)
{
	OutElements.Reset();
	OutSZ.Reset();

	CSHouseFrame::FBrickParams Params;
	Params.Length = BrickLength;
	Params.Gap = BrickGap;
	Params.MaxBricks = 4096;

	CSHouseFrame::FWallFrame Frame;   // S 对 +X、Z 对 +Z：墙空间 = 世界空间
	CSHouseFrame::BuildEdgeElements(Frame, Openings, Params, OutElements);

	for (const CSHouseFrame::FElement& E : OutElements)
	{
		for (int32 K = 0; K < E.BrickCount; ++K)
		{
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(E.Path, E.HalfLen + K * E.Pitch, SZ, Tangent);
			OutSZ.Add(FVector2D(SZ.X, SZ.Y));
		}
	}
}

/** 演示关卡那扇门：600 长墙、3 个拱位、墩宽 20 ⇒ 段距 160、拱宽 140、拱高 220 ⇒ 起拱线 150。 */
FCSWallOpening CSHouseTest_DemoArch(float CenterS)
{
	FCSWallOpening Arch;
	Arch.Shape = ECSOpeningShape::Arch;
	Arch.CenterS = CenterS;
	Arch.Width = 140.0f;
	Arch.Z0 = 0.0f;
	Arch.Z1 = 220.0f;
	return Arch;
}
}

// -----------------------------------------------------------------------------
// 闭式砖数必须与 SolveBlockLayout **逐个弧长**一致
//
// 解析推导不再排一张逐砖记录表，砖数只能由闭式给出（`CSHouseFrame::SolveRun`）。它是
// 单条目 palette 下 `SolveBlockLayout` 的同式重写 —— 而"同式"这件事必须被钉住：砖数进
// desc 哈希、也进演示回归的断言，差一块就是画面差一块。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameRunMatchesLayoutTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameRunMatchesLayout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameRunMatchesLayoutTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;

	int32 Mismatch = 0, Jumps = 0, PrevCount = -1;
	double WorstScale = 0.0;
	float WorstArc = 0.0f;
	// 20..1300 cm 覆盖"墩那一小段（150 上下）"到"整条 U 形路（620 上下）"再到放大后的房子。
	// 步长 0.1 cm：砖数的跳变点是连续的实数，粗扫会整段跳过跳变那一帧。
	for (int32 Step = 0; Step <= 12800; ++Step)
	{
		const float Arc = 20.0f + float(Step) * 0.1f;
		for (float GapValue : { 0.0f, 1.5f })
		{
			FRandomStream Rand(1);
			TArray<int32> Sequence;
			const TArray<float> Palette = { BrickLength };
			const float RefScale = ACSSplineBlockActor::SolveBlockLayout(Arc, GapValue, Palette, Rand, Sequence);

			float Scale = 0.0f;
			const int32 Count = CSHouseFrame::SolveRun(Arc, BrickLength, GapValue, Scale);

			if (Count != Sequence.Num() || FMath::Abs(Scale - RefScale) > 1.0e-6f)
			{
				++Mismatch;
				WorstArc = Arc;
			}
			WorstScale = FMath::Max(WorstScale, FMath::Abs(double(Scale) - double(RefScale)));
			if (GapValue == 0.0f)
			{
				if (PrevCount >= 0 && Sequence.Num() != PrevCount) ++Jumps;
				PrevCount = Sequence.Num();
			}
		}
	}

	// 扫描必须真的穿过很多次砖数跳变，否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("Sweep crosses brick-count jumps (%d)"), Jumps), Jumps >= 40);
	TestTrue(FString::Printf(TEXT("The closed form matches SolveBlockLayout at every arc length (%d misses, worst at %.1f cm, worst scale delta %.3g)"),
		Mismatch, WorstArc, WorstScale), Mismatch == 0);

	// 退化输入：零长度 / 零砖长一律给 0 块，调用方的早退才有意义。
	{
		float Scale = 1.0f;
		TestEqual(TEXT("A zero-length run yields no bricks"), CSHouseFrame::SolveRun(0.0f, BrickLength, 0.0f, Scale), 0);
		TestEqual(TEXT("A zero-length brick yields no bricks"), CSHouseFrame::SolveRun(500.0f, 0.0f, 0.0f, Scale), 0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// **迁移的等价性判据**：新旧两条路把砖摆在同一个地方
//
// 「同一个地方」不可能是逐位相等，两条路本来就不是同一条曲线：
//   · 新路骑在 `FCSOpeningClipField` 那条**解析**曲线上 —— 那正是材质切洞用的那条边；
//   · 旧路骑在"外接多边形折线 → 等距重采样 → 均匀三次 B 样条"之后的近似曲线上。
// 所以判据不写成"差得不多"，写成**差异恰好等于那条近似的教科书误差、没有别的成分**：
//   ① 砖数逐块相同 —— 弧长参数化一致，这是"摆在同一批位置上"的骨架；
//   ② 门樘（直段）上的偏差 = **纯横向**的外接半径余量 `R·(sec(π/2N) − 1)`：旧路的洞缘折线
//      是按**外接**多边形建的（`CSHouse_SampleOpeningProfile` 的 `Circum`，"宁可覆盖不可漏"），
//      门樘线因此落在解析半宽之外那么一点；新路骑在 clip 场自己的边上，正好把这一点收回来。
//      竖直分量必须近乎为零 —— 那是"只有这一项、没有竖向漂移"的证据。
//   ③ 拱圈（圆弧）上的偏差 = 均匀三次 B 样条把半径 R 的圆弧按控制点间距 h 内缩的闭式量
//      **h²/(6R)**。⚠️ 早先按"90° 折角被抹圆"写这条判据是**错的**，实测把它证伪了：设计路径
//      在起拱线处切向连续（半圆 θ=0 的切向恰是竖直），根本没有折角，偏差在拱圈上均匀分布。
//      三个尺寸实测 Δ·6R/h² = 1.008 / 0.941 / 1.066，对理论值 1。
// 两条都带 ±25%~35% 的容差钉住：这不是"差不多"，是**把差异逐项归因到已知量** —— 朝向翻转、
// 参数化漂移、圆心取错，任何别的成分都会立刻把比值顶出带外。
// 顺带钉住新路的定位依据本身：砖心必须**恰好落在 clip 场的零等值线上**（|q| = 1）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameAnalyticMatchesLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameAnalyticMatchesLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameAnalyticMatchesLegacyTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;
	const float BrickGap = CDO->FrameBrickGap;
	const float ChordTolerance = CDO->OpeningChordTolerance;

	// 三种尺寸都过一遍：出厂拱、窄拱、放大近一倍的拱。等价性不能只在一个尺寸上成立。
	const float Widths[3] = { 140.0f, 90.0f, 260.0f };
	const float Heights[3] = { 220.0f, 170.0f, 380.0f };

	for (int32 Case = 0; Case < 3; ++Case)
	{
		FCSWallOpening Arch = CSHouseTest_DemoArch(300.0f);
		Arch.Width = Widths[Case];
		Arch.Z1 = Heights[Case];

		TArray<FVector2D> Legacy;
		double ControlSpacing = 0.0;
		CSHouseTest_LegacyFrameBricks(Arch, ChordTolerance, BrickLength, BrickGap, /*SeedSalt*/ 0, Legacy, ControlSpacing);

		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Analytic;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(&Arch, 1), BrickLength, BrickGap, Elements, Analytic);

		TestTrue(FString::Printf(TEXT("Case %d lays bricks at all (%d)"), Case, Legacy.Num()), Legacy.Num() > 8);
		// ① 骨架：同一条洞缘、同一个排布器 ⇒ 砖数必须一块不差。
		TestEqual(FString::Printf(TEXT("Case %d: the two paths lay the same number of bricks"), Case),
			Analytic.Num(), Legacy.Num());
		if (Analytic.Num() != Legacy.Num()) continue;

		const double SpringZ = FMath::Max(Arch.Z1 - Arch.HalfWidth(), Arch.Z0);
		const double Radius = Arch.HalfWidth();
		double WorstJamb = 0.0, BestJamb = TNumericLimits<double>::Max(), WorstJambZ = 0.0, WorstRing = 0.0;
		int32 JambBricks = 0, RingBricks = 0, JambInward = 0;
		for (int32 K = 0; K < Analytic.Num(); ++K)
		{
			const double Delta = FVector2D::Distance(Analytic[K], Legacy[K]);
			// 拱圈 / 门樘的分界就是起拱线。留半块砖的余量，跨在分界上的那块砖算进拱圈
			// （它有一半确实在圆弧上，按直段判会误判成"直段也有误差"）。
			if (Analytic[K].Y > SpringZ + 0.5 * BrickLength) { WorstRing = FMath::Max(WorstRing, Delta); ++RingBricks; }
			else if (Analytic[K].Y < SpringZ - 0.5 * BrickLength)
			{
				WorstJamb = FMath::Max(WorstJamb, Delta);
				BestJamb = FMath::Min(BestJamb, Delta);
				WorstJambZ = FMath::Max(WorstJambZ, FMath::Abs(Analytic[K].Y - Legacy[K].Y));
				// 新路的门樘线应当**更靠近洞心**（把外接余量收回到 clip 场自己的边上）。
				if (FMath::Abs(Analytic[K].X - Arch.CenterS) < FMath::Abs(Legacy[K].X - Arch.CenterS)) ++JambInward;
				++JambBricks;
			}
		}
		// 外接半径余量：`CSHouse_SampleOpeningProfile` 把圆弧按外接多边形建，顶点半径因此是
		// `R / cos(π/2N)`。这是门樘那一项差异的**唯一**来源，所以用同一个 N 的公开求值器算它。
		const int32 ProfileSegments = CSHouse_ProfileSegments(float(Radius), ChordTolerance);
		const double JambOffset = Radius * (1.0 / FMath::Cos(PI / (2.0 * ProfileSegments)) - 1.0);
		// 外接多边形比真圆弧**长**这么多（周长 2N·R·tan(π/2N) 对 π·R）。旧路按它的弧长排砖，
		// 于是每块砖的弧长位置整体漂一点点 —— 门樘上那一点点就表现为竖向偏差，上限就是它。
		const double PolySurplus = 2.0 * ProfileSegments * Radius * FMath::Tan(PI / (2.0 * ProfileSegments)) - PI * Radius;
		// h²/(6R)：均匀三次 B 样条在控制点处取 (P₋₁ + 4P₀ + P₁)/6，圆弧上等距 h 的三点代进去
		// 恰好把半径缩掉这么多。这是这条判据里唯一允许存在的差异来源。
		const double Predicted = ControlSpacing * ControlSpacing / (6.0 * FMath::Max(Radius, 1.0));
		const double Ratio = Predicted > 0.0 ? WorstRing / Predicted : 0.0;
		const double JambRatio = JambOffset > 0.0 ? WorstJamb / JambOffset : 0.0;
		AddInfo(FString::Printf(TEXT("Case %d (W=%.0f H=%.0f): %d bricks (%d ring / %d jamb), h=%.2f cm; ring delta %.3f vs h²/(6R)=%.3f (ratio %.3f); jamb delta %.4f..%.4f vs R(sec(pi/2N)-1)=%.4f (ratio %.3f), |dZ| %.4f"),
			Case, Widths[Case], Heights[Case], Analytic.Num(), RingBricks, JambBricks,
			ControlSpacing, WorstRing, Predicted, Ratio, BestJamb, WorstJamb, JambOffset, JambRatio, WorstJambZ));

		TestTrue(FString::Printf(TEXT("Case %d samples both the ring and the jambs (%d / %d)"), Case, RingBricks, JambBricks),
			RingBricks >= 4 && JambBricks >= 2);
		// ② 门樘那一项差异基本是**横向**的外接半径余量；竖向那一丝由外接多边形的弧长盈余
		//    `2NR·tan(π/2N) − πR` 封顶（实测约 0.23 倍，三个尺寸一致），方向一律朝内。
		TestTrue(FString::Printf(TEXT("Case %d: the jamb's vertical drift stays under the polygon's arc-length surplus (%.4f of %.4f cm)"),
			Case, WorstJambZ, PolySurplus), WorstJambZ < PolySurplus && WorstJambZ < 0.15);
		TestEqual(FString::Printf(TEXT("Case %d: every jamb brick moves inward onto the clip edge"), Case), JambInward, JambBricks);
		TestTrue(FString::Printf(TEXT("Case %d: the jamb difference is exactly the circumscribed-radius margin (ratio %.3f)"), Case, JambRatio),
			JambRatio > 0.9 && JambRatio < 1.35 && BestJamb > 0.75 * JambOffset);
		// ③ 拱圈上的差异**恰好**是那条闭式的 B 样条圆弧内缩，没有别的成分。
		TestTrue(FString::Printf(TEXT("Case %d: the ring difference is exactly the B-spline arc shrink h²/(6R) (ratio %.3f)"), Case, Ratio),
			Ratio > 0.75 && Ratio < 1.25);
		// 兜底的绝对量：无论比值怎么算，偏差都必须远小于砖长（肉眼与出图都分辨不出）。
		TestTrue(FString::Printf(TEXT("Case %d: and it stays far below one brick (%.3f cm of %.1f)"), Case, WorstRing, BrickLength),
			WorstRing < 0.1 * BrickLength);

		// 新路的定位依据：砖心恰好落在 clip 场的零等值线上（拱圈 |q| = 1、门樘 |q.x| = 1）。
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Arch);
		double WorstRidge = 0.0;
		for (const FVector2D& P : Analytic)
		{
			const FVector2f Q = Field.Eval(float(P.X), float(P.Y));
			const double Norm = Q.Y > 0.0f ? FMath::Sqrt(double(Q.X) * Q.X + double(Q.Y) * Q.Y) : FMath::Abs(double(Q.X));
			WorstRidge = FMath::Max(WorstRidge, FMath::Abs(Norm - 1.0));
		}
		TestTrue(FString::Printf(TEXT("Case %d: every analytic brick sits on the clip field's own edge (|q|-1 = %.3g)"), Case, WorstRidge),
			WorstRidge < 1.0e-3);
	}
	return true;
}

// -----------------------------------------------------------------------------
// 拱间墩的小石柱：柱础 + 柱身 + 柱头（2026-09-04）
//
// 实拍 `img/tiny-glade-ref-twin-arch-pier.jpg`：两道拱之间那根墩不是一摞同样的砖，而是
// 柱础 + 柱身 + **更宽的柱头**，两道拱圈收在柱头上。这里把它钉成几何断言：
//   ① 开了柱头之后墩变成三条砖路（础 / 身 / 头），与两侧的拱合计五条；
//   ② 础与头各**一块**砖、横截面放大（`CrossScale`），柱身与两拱的 `CrossScale` 恒 1；
//   ③ 三段首尾相接、同一个 S —— 跨度里仍然只有**一列**（`FramePierSingleColumn` 的不变量不许破）；
//   ④ 柱头高度被夹在墩高的 40% 内：给一个荒谬的大值也不许把柱身挤成负长；
//   ⑤ 关掉（`CapitalHeight = 0`）逐位退回旧拓扑（三条砖路）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFramePierCapitalTest,
	"PCGPlugins.ComputeShaderGenerator.House.FramePierCapital",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFramePierCapitalTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();

	FCSWallOpening A = CSHouseTest_DemoArch(140.0f);
	FCSWallOpening B = CSHouseTest_DemoArch(300.0f);
	A.StyleFlags |= CSHouse_StylePierAfter;
	B.StyleFlags |= CSHouse_StylePierBefore;
	const FCSWallOpening Pair[2] = { A, B };
	const float PierCentre = (A.S1() + B.S0()) * 0.5f;
	float Span = 0.0f, PierTop = 0.0f;
	TestTrue(TEXT("The demo pair leaves a pier span"), CSHouse_PierSpanBetween(A, B, Span, PierTop));

	auto Build = [&](float CapHeight, TArray<CSHouseFrame::FElement>& Out)
	{
		CSHouseFrame::FBrickParams Params;
		Params.Length = CDO->FrameBrickLength;
		Params.Gap = CDO->FrameBrickGap;
		Params.MaxBricks = 4096;
		Params.CapitalScale = 1.5f;
		Params.CapitalHeight = CapHeight;
		CSHouseFrame::FWallFrame Frame;   // 墙空间 = 世界空间
		Out.Reset();
		CSHouseFrame::BuildEdgeElements(Frame, MakeArrayView(Pair, 2), Params, Out);
	};

	// ---- ①②③ 开柱头 ----
	{
		TArray<CSHouseFrame::FElement> Elements;
		Build(14.0f, Elements);
		TestEqual(TEXT("Arch + base + shaft + capital + arch = five brick paths"), Elements.Num(), 5);
		if (Elements.Num() == 5)
		{
			const CSHouseFrame::FElement& Base = Elements[1];
			const CSHouseFrame::FElement& Shaft = Elements[2];
			const CSHouseFrame::FElement& Capital = Elements[3];

			TestTrue(TEXT("Base is a single widened brick on the floor"),
				Base.BrickCount == 1 && FMath::IsNearlyEqual(Base.CrossScale, 1.5f)
				&& FMath::IsNearlyEqual(Base.Path.BaseZ, 0.0f) && FMath::IsNearlyEqual(Base.Path.TopZ, 14.0f)
				&& FMath::IsNearlyEqual(Base.Path.LeftS, PierCentre, 0.01f));
			TestTrue(TEXT("Capital is a single widened brick under the springing"),
				Capital.BrickCount == 1 && FMath::IsNearlyEqual(Capital.CrossScale, 1.5f)
				&& FMath::IsNearlyEqual(Capital.Path.TopZ, PierTop, 0.01f)
				&& FMath::IsNearlyEqual(Capital.Path.BaseZ, PierTop - 14.0f, 0.01f)
				&& FMath::IsNearlyEqual(Capital.Path.LeftS, PierCentre, 0.01f));
			TestTrue(TEXT("Shaft keeps the plain cross-section and fills exactly the gap between them"),
				FMath::IsNearlyEqual(Shaft.CrossScale, 1.0f)
				&& FMath::IsNearlyEqual(Shaft.Path.BaseZ, 14.0f, 0.01f)
				&& FMath::IsNearlyEqual(Shaft.Path.TopZ, PierTop - 14.0f, 0.01f)
				&& FMath::IsNearlyEqual(Shaft.Path.LeftS, PierCentre, 0.01f));
			TestTrue(TEXT("The arches themselves are not widened"),
				FMath::IsNearlyEqual(Elements[0].CrossScale, 1.0f) && FMath::IsNearlyEqual(Elements[4].CrossScale, 1.0f));

			// 单砖的位置 = 它那一小段的中点（HalfLen 就是半段高）。
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(Base.Path, Base.HalfLen, SZ, Tangent);
			TestTrue(TEXT("Base brick sits mid-slab"), FMath::IsNearlyEqual(SZ.Y, 7.0f, 0.01f) && FMath::IsNearlyEqual(SZ.X, PierCentre, 0.01f));
			CSHouseFrame::EvalPath(Capital.Path, Capital.HalfLen, SZ, Tangent);
			TestTrue(TEXT("Capital brick sits mid-slab"), FMath::IsNearlyEqual(SZ.Y, PierTop - 7.0f, 0.01f));

			// ③ 跨度里仍然只有一列：三段砖心的 S 全部相同。
			TArray<double> Columns;
			for (const CSHouseFrame::FElement& E : Elements)
			{
				for (int32 K = 0; K < E.BrickCount; ++K)
				{
					CSHouseFrame::EvalPath(E.Path, E.HalfLen + K * E.Pitch, SZ, Tangent);
					if (SZ.Y > PierTop - 1.0f || SZ.X < A.S1() - 0.5f || SZ.X > B.S0() + 0.5f) continue;
					bool bKnown = false;
					for (const double S : Columns) bKnown |= FMath::Abs(S - SZ.X) < 0.5;
					if (!bKnown) Columns.Add(SZ.X);
				}
			}
			TestEqual(TEXT("Base, shaft and capital still form exactly one column"), Columns.Num(), 1);
		}
	}

	// ---- ④ 夹住：柱头高度荒谬地大，柱身也不许是负长 ----
	{
		TArray<CSHouseFrame::FElement> Elements;
		Build(1000.0f, Elements);
		TestEqual(TEXT("An absurd capital height still yields five paths"), Elements.Num(), 5);
		if (Elements.Num() == 5)
		{
			TestTrue(TEXT("Capital height is clamped so the shaft keeps positive length"),
				Elements[2].Path.TopZ > Elements[2].Path.BaseZ + 1.0f
				&& Elements[1].Path.TopZ <= PierTop * 0.4f + 0.01f);
		}
	}

	// ---- ⑤ 关掉 = 旧拓扑 ----
	{
		TArray<CSHouseFrame::FElement> Elements;
		Build(0.0f, Elements);
		TestEqual(TEXT("With the capital off the pier is one plain path again (three total)"), Elements.Num(), 3);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 拱间墩上只砌**一列**砖
//
// 旧路让相邻两拱各出一条门樘砖脚、各伸进跨度一半 ⇒ 两列砖在墩正中**共面对接**，从地面
// 一直贯着一条竖缝（出图 `pier_after_pier.png` 可见），而 TG 实拍里墩就是一列。
// 这条用例把"一列"钉成几何断言，而不是留给出图去看：
//   ① 跨度里的砖心 S 只有**一个**取值（一列），且恰在墩心；
//   ② 那一列覆盖满整条跨度（`PierWidth ≤ FrameBrickDepth` 那条配平条件的执行面）；
//   ③ 两侧的拱在墩那一侧**不再出门樘**（否则就是三列，比两列更糟）；
//   ④ 对照组：把墩样式位清掉（= 旧拓扑）时，跨度里恰好出现**两**列，且它们在墩正中对接
//      —— 没有这一条，①③ 可能只是在描述一个本来就不会发生的情况。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFramePierSingleColumnTest,
	"PCGPlugins.ComputeShaderGenerator.House.FramePierSingleColumn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFramePierSingleColumnTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;
	const float BrickGap = CDO->FrameBrickGap;
	const float BrickDepth = CDO->FrameBrickDepth;

	// 演示关卡的连拱：段距 160、拱宽 140 ⇒ 相邻两拱之间的跨度恰好 20 = PierWidth。
	FCSWallOpening Left = CSHouseTest_DemoArch(140.0f);
	FCSWallOpening Right = CSHouseTest_DemoArch(300.0f);
	const double SpanMin = Left.S1(), SpanMax = Right.S0();
	const double PierCentre = (SpanMin + SpanMax) * 0.5;
	const double Span = SpanMax - SpanMin;
	TestTrue(FString::Printf(TEXT("The two arches leave a pier span (%.1f cm)"), Span), Span > 1.0);

	const double SpringZ = Left.Z1 - Left.HalfWidth();
	auto ColumnsInSpan = [SpanMin, SpanMax, SpringZ](const TArray<FVector2D>& Bricks)
	{
		// 起拱线**以下**、落在跨度里的砖心（拱圈上的砖不算 —— 它们本来就骑在洞缘上）。
		TArray<double> Columns;
		for (const FVector2D& P : Bricks)
		{
			if (P.Y > SpringZ - 1.0) continue;
			if (P.X < SpanMin - 0.5 || P.X > SpanMax + 0.5) continue;
			bool bKnown = false;
			for (const double S : Columns) bKnown |= FMath::Abs(S - P.X) < 0.5;
			if (!bKnown) Columns.Add(P.X);
		}
		Columns.Sort();
		return Columns;
	};

	// ---- 新拓扑：墩样式位打上 ----
	{
		FCSWallOpening A = Left, B = Right;
		A.StyleFlags |= CSHouse_StylePierAfter;
		B.StyleFlags |= CSHouse_StylePierBefore;
		const FCSWallOpening Pair[2] = { A, B };

		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Bricks;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(Pair, 2), BrickLength, BrickGap, Elements, Bricks);

		// ③ 两拱各自砌一条路，墩单独一条 ⇒ 恰好三条。
		TestEqual(TEXT("Two arches sharing a pier lay three brick paths"), Elements.Num(), 3);
		if (Elements.Num() == 3)
		{
			TestTrue(TEXT("The left arch drops its pier-side jamb"), Elements[0].Path.bLeftJamb && !Elements[0].Path.bRightJamb);
			TestTrue(TEXT("The pier is a bare vertical column at the span centre"),
				Elements[1].Path.MidKind == CSHouseFrame::EMidKind::None
				&& FMath::IsNearlyEqual(double(Elements[1].Path.LeftS), PierCentre, 0.01)
				&& FMath::IsNearlyEqual(double(Elements[1].Path.TopZ), SpringZ, 0.01));
			TestTrue(TEXT("The right arch drops its pier-side jamb"), !Elements[2].Path.bLeftJamb && Elements[2].Path.bRightJamb);
		}

		// ① 一列，且恰在墩心。
		const TArray<double> Columns = ColumnsInSpan(Bricks);
		TestEqual(FString::Printf(TEXT("The pier carries exactly one brick column (%d)"), Columns.Num()), Columns.Num(), 1);
		if (Columns.Num() == 1)
		{
			TestTrue(FString::Printf(TEXT("That column stands on the pier centre (%.2f vs %.2f)"), Columns[0], PierCentre),
				FMath::Abs(Columns[0] - PierCentre) < 0.01);
			// ② 覆盖：一列砖横向占 FrameBrickDepth，要盖满跨度就要求 PierWidth ≤ FrameBrickDepth。
			TestTrue(FString::Printf(TEXT("One column of depth %.1f covers the whole %.1f cm span"), BrickDepth, Span),
				Columns[0] - BrickDepth * 0.5 <= SpanMin + 0.01 && Columns[0] + BrickDepth * 0.5 >= SpanMax - 0.01);
		}
		// 墩必须真的砌到起拱线：矮一截就会在墩顶露出一段被裁空的灰泥。
		double PierTop = 0.0;
		for (const FVector2D& P : Bricks) if (FMath::Abs(P.X - PierCentre) < 0.5) PierTop = FMath::Max(PierTop, P.Y);
		TestTrue(FString::Printf(TEXT("The pier column reaches the springing line (%.1f of %.1f)"), PierTop, SpringZ),
			PierTop > SpringZ - BrickLength);
	}

	// ---- 对照组：旧拓扑（不打墩样式位）⇒ 两列砖在墩正中对接，那就是那条竖缝 ----
	{
		const FCSWallOpening Pair[2] = { Left, Right };
		TArray<CSHouseFrame::FElement> Elements;
		TArray<FVector2D> Bricks;
		CSHouseTest_AnalyticFrameBricks(MakeArrayView(Pair, 2), BrickLength, BrickGap, Elements, Bricks);

		const TArray<double> Columns = ColumnsInSpan(Bricks);
		TestEqual(FString::Printf(TEXT("Without the pier merge the span carries two columns (%d)"), Columns.Num()),
			Columns.Num(), 2);
		if (Columns.Num() == 2)
		{
			// 两列各伸进跨度半个进深 ⇒ 右缘与左缘在墩正中重合，那条竖缝就是这么来的。
			const double SeamLeft = Columns[0] + BrickDepth * 0.5;
			const double SeamRight = Columns[1] - BrickDepth * 0.5;
			TestTrue(FString::Printf(TEXT("The two legacy columns butt exactly at the pier centre (%.2f / %.2f vs %.2f)"),
				SeamLeft, SeamRight, PierCentre),
				FMath::Abs(SeamLeft - PierCentre) < 0.51 && FMath::Abs(SeamRight - PierCentre) < 0.51);
		}
	}
	return true;
}

// -----------------------------------------------------------------------------
// 门框砖的**负缝**：任何弧长、任何块数下相邻砖都必须互相穿插
//
// 这条断言就是"胀大砖"那条改动的全部意义。门框砖的块数随洞口弧长跳变（SolveBlockLayout
// 变数量、近定距），跳变那一帧如果砖缝是正的，整条拱缘会同时露出一条缝 —— 而 TG 之所以
// 逐帧重排砖也看不出跳变，正是因为它的砖缝本来就是负的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameBrickOverlapTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameBrickOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameBrickOverlapTest::RunTest(const FString& Parameters)
{
	// 参数取**出厂默认**（CDO），断言才真的钉住线上的观感：谁把 FrameBrickGap 调回正缝、
	// 或把 FrameBrickBloat 调回 1，这里立刻红，而不是等出图时用肉眼去找那条缝。
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float BrickLength = CDO->FrameBrickLength;

	// 砖缝 = 相邻砖中心距 − 渲染长度。两行公式与产线**逐字对应**，改一处就要改这里：
	//   · 中心距：`CSHouseFrame::SolveRun` 定下的 `Pitch`（= 砖长 × 缩放 + 缝 × 缩放）；
	//   · 渲染长度：EnsureFrameComponent 的 BlockSize.Y = FrameBrickLength × Bloat，
	//     再被 CSHouseFrame.usf 的 `BlockScale.y *= LayoutScale` 乘上铺装缩放。
	auto Sweep = [BrickLength](float GapValue, float BloatValue, int32& OutCountJumps,
		float& OutMinSeam, float& OutMaxSeam)
	{
		OutCountJumps = 0;
		OutMinSeam = TNumericLimits<float>::Max();
		OutMaxSeam = -TNumericLimits<float>::Max();

		const TArray<float> Palette = { BrickLength };
		int32 PrevCount = -1;
		// 200..1200 cm 覆盖门樘 + 拱缘的真实弧长范围（演示房子实测一条曲线 ≈ 500 cm / 19 块）。
		// 步长 0.1 cm：块数跳变点是连续的实数，粗扫会整段跳过跳变那一帧。
		for (int32 Step = 0; Step <= 10000; ++Step)
		{
			const float Arc = 200.0f + float(Step) * 0.1f;
			FRandomStream Rand(1);
			TArray<int32> Sequence;
			const float Scale = ACSSplineBlockActor::SolveBlockLayout(Arc, GapValue, Palette, Rand, Sequence);
			if (Sequence.Num() < 2 || Scale <= 0.0f) continue;
			if (PrevCount >= 0 && Sequence.Num() != PrevCount) ++OutCountJumps;
			PrevCount = Sequence.Num();

			const float Pitch = (BrickLength + GapValue) * Scale;
			const float RenderLength = BrickLength * BloatValue * Scale;
			const float Seam = Pitch - RenderLength;     // > 0 露缝，< 0 穿插
			OutMinSeam = FMath::Min(OutMinSeam, Seam);
			OutMaxSeam = FMath::Max(OutMaxSeam, Seam);
		}
	};

	int32 CountJumps = 0;
	float MinSeam = 0.0f;
	float MaxSeam = 0.0f;
	Sweep(CDO->FrameBrickGap, CDO->FrameBrickBloat, CountJumps, MinSeam, MaxSeam);

	// 扫描必须真的穿过很多次块数跳变，否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("Sweep crosses brick-count jumps (%d)"), CountJumps), CountJumps >= 8);
	// 全部意义所在：块数怎么跳，砖缝都是负的。
	TestTrue(FString::Printf(TEXT("Seam stays negative at every brick count (worst %.3f cm)"), MaxSeam),
		MaxSeam < 0.0f);
	// 而且穿插量始终有厚度 —— 差一丁点就穿插不上的话，浮点/铺装缩放一抖就还是会露缝。
	TestTrue(FString::Printf(TEXT("Interpenetration stays substantial (worst %.3f cm)"), MaxSeam),
		MaxSeam < -0.05f * BrickLength);
	// 反向护栏：穿插量不能大到吃掉整块砖（那就不是砌缝，是砖叠砖）。
	TestTrue(FString::Printf(TEXT("Interpenetration never swallows a whole brick (%.3f cm)"), MinSeam),
		MinSeam > -BrickLength);
	// TG 语义"砖数一变只是穿插量微调"：整个扫描里穿插量的摆幅要远小于砖长。
	TestTrue(FString::Printf(TEXT("Interpenetration only wobbles across jumps (%.3f cm)"), MaxSeam - MinSeam),
		(MaxSeam - MinSeam) < 0.25f * BrickLength);

	// 对照组 = 改动前的出厂参数（gap 1.5、不胀）：同一扫描下砖缝恒为正 —— 那就是拱缘上
	// 一格一格断开的那条缝，也是这条改动存在的理由。留着它，免得有人把默认值改回去。
	int32 LegacyJumps = 0;
	float LegacyMinSeam = 0.0f;
	float LegacyMaxSeam = 0.0f;
	Sweep(1.5f, 1.0f, LegacyJumps, LegacyMinSeam, LegacyMaxSeam);
	TestTrue(FString::Printf(TEXT("Legacy gap 1.5 without bloat always leaves a positive seam (best %.3f cm)"), LegacyMinSeam),
		LegacyMinSeam > 0.0f);
	return true;
}

// -----------------------------------------------------------------------------
// 房体夹具：竖直射线数实体层数 + 一组参数化的房体 desc
//
// 深度判据的用法：房体是**凸块的并集**（面板盒、窗台盒、柱子全是凸的，且每块闭合、法线朝外）。
// 对一个凸块：起点在块内的竖直射线只穿出一次（N·up > 0，记 +1）；起点在块外则一进一出净 0。
// 于是**所有交点上 sign(N·up) 的和 = 包住 P 的块数** —— 不要求 mesh 是流形，也不受"埋在实心里
// 的背靠背重复面"影响（那种一对正好抵消）。竖直射线穿不过竖直面，所以 XY 投影退化的三角直接跳过。
// -----------------------------------------------------------------------------

namespace
{
/** 从 P 朝正上方数"套着几层实体"（判据见本节抬头）。 */
int32 CSHouseTest_SolidDepth(const FCSGpuMeshCPUData& S, const FVector& P)
{
	int32 Depth = 0;
	const int32 NumTris = S.Indices.Num() / 3;
	auto Cross2 = [](const FVector2D& U, const FVector2D& V) { return U.X * V.Y - U.Y * V.X; };
	for (int32 Tri = 0; Tri < NumTris; ++Tri)
	{
		const uint32 I0 = S.Indices[Tri * 3], I1 = S.Indices[Tri * 3 + 1], I2 = S.Indices[Tri * 3 + 2];
		const FVector A(S.Positions[I0]), B(S.Positions[I1]), C(S.Positions[I2]);
		const FVector2D A2(A.X, A.Y), B2(B.X, B.Y), C2(C.X, C.Y);

		const double Area2 = Cross2(B2 - A2, C2 - A2);
		if (FMath::Abs(Area2) < 1.0e-6) continue;   // 竖直面 / 退化三角：与竖直射线无关

		const FVector2D Q(P.X, P.Y);
		const double WA = Cross2(C2 - B2, Q - B2);
		const double WB = Cross2(A2 - C2, Q - C2);
		const double WC = Cross2(B2 - A2, Q - A2);
		if (!((WA >= 0 && WB >= 0 && WC >= 0) || (WA <= 0 && WB <= 0 && WC <= 0))) continue;

		const double HitZ = (WA * A.Z + WB * B.Z + WC * C.Z) / Area2;
		if (HitZ <= P.Z + 1.0e-4) continue;

		const double NormalZ = double(S.Normals[I0].Z);
		if (NormalZ > 0) ++Depth;
		else if (NormalZ < 0) --Depth;
	}
	return Depth;
}

/** 一组房体参数（底面 / 墙高 / 墙厚）。屋面不进房体三角汤，所以坡度与外挑在这里无处可用。 */
struct FCSHouseTestBodyCase
{
	double SizeX, SizeY;
	float WallHeight, WallThickness;
	const TCHAR* What;
};

FCSHouseBodyDesc CSHouseTest_MakeBody(const FCSHouseTestBodyCase& Case)
{
	FCSHouseBodyDesc Body;
	Body.Footprint = FCSHouseFootprint::MakeRect(FVector2D(Case.SizeX, Case.SizeY));
	Body.WallHeight = Case.WallHeight;
	Body.WallThickness = Case.WallThickness;
	return Body;   // World = Identity ⇒ 三角汤就是局部坐标
}
}

// -----------------------------------------------------------------------------
// 拱间墩的判定：双阈迟回 + 「跨度只在两侧都是落地的拱时才成立」
//
// 迟回是**路径依赖**的：同一个跨度宽度，从窄边过来是墩、从宽边过来是墙。这不是可有可无的
// 打磨 —— 跨度是拉尺寸 / 离地收窄的**连续**函数，单阈会让它在阈值附近抖动时整面墙的灰泥
// 忽有忽无，比"选错一种样式"难看得多。所以这里不只测两条阈值，还要真扫一趟带内抖动。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHousePierHysteresisTest,
	"PCGPlugins.ComputeShaderGenerator.House.PierHysteresis",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHousePierHysteresisTest::RunTest(const FString& Parameters)
{
	const FCSHousePierStyle Style;   // 默认 60 / 75，与 ACSHouseActor 的两个 UPROPERTY 同源

	// ---- ① 迟回带之外：结论与历史无关 ----
	TestTrue(TEXT("A narrow span is a pier whichever side it came from"),
		CSHouse_SpanIsPier(Style, 40.0f, false) && CSHouse_SpanIsPier(Style, 40.0f, true));
	TestFalse(TEXT("A wide span is a wall whichever side it came from"),
		CSHouse_SpanIsPier(Style, 90.0f, false) || CSHouse_SpanIsPier(Style, 90.0f, true));

	// ---- ② 迟回带内 (60, 75)：结论**只**由历史决定 ----
	for (const float Span : { 60.5f, 67.0f, 74.5f })
	{
		TestTrue(FString::Printf(TEXT("Span %.1f stays a pier when it already was one"), Span),
			CSHouse_SpanIsPier(Style, Span, true));
		TestFalse(FString::Printf(TEXT("Span %.1f stays a wall when it already was one"), Span),
			CSHouse_SpanIsPier(Style, Span, false));
	}

	// ---- ③ 带内来回抖 20 趟：一次都不许翻 ----
	// 这条就是本轮验收门那句"跨度在 60/75 之间来回时不许反复切换样式"的纯函数版。
	{
		bool bState = CSHouse_SpanIsPier(Style, 40.0f, false);   // 先从窄边进入墩态
		int32 Flips = 0;
		for (int32 Round = 0; Round < 20; ++Round)
		{
			for (const float Span : { 62.0f, 73.0f, 66.5f, 71.0f })
			{
				const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
				if (bNext != bState) ++Flips;
				bState = bNext;
			}
		}
		TestTrue(TEXT("Dithering inside the band never flips the style"), Flips == 0 && bState);
	}

	// ---- ④ 单调一往一返：样式恰好翻两次（进带不翻、出带才翻）----
	{
		// 起点必须**用起点跨度自己定**，不能拿一个凭空的 false 当初值 ——
		// 那会把"第一次求值"也数成一次翻转（实测：数出 3 次）。
		bool bState = CSHouse_SpanIsPier(Style, 30.0f, false);
		int32 Flips = 0;
		for (float Span = 31.0f; Span <= 100.0f; Span += 1.0f)
		{
			const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
			if (bNext != bState) ++Flips;
			bState = bNext;
		}
		for (float Span = 100.0f; Span >= 30.0f; Span -= 1.0f)
		{
			const bool bNext = CSHouse_SpanIsPier(Style, Span, bState);
			if (bNext != bState) ++Flips;
			bState = bNext;
		}
		TestTrue(FString::Printf(TEXT("A full narrow-wide-narrow sweep flips exactly twice (got %d)"), Flips),
			Flips == 2 && bState);
	}

	// ---- ⑤ 退化输入 ----
	{
		FCSHousePierStyle Off;
		Off.bEnabled = false;
		TestFalse(TEXT("Disabled means never a pier"), CSHouse_SpanIsPier(Off, 1.0f, true));

		// 高阈被填得比低阈还小 ⇒ 退化成单阈，绝不能出现"转墩了就再也转不回来"。
		FCSHousePierStyle Inverted;
		Inverted.MaxWidth = 60.0f;
		Inverted.RestoreWidth = 10.0f;
		TestFalse(TEXT("An inverted pair still lets a wide span go back to wall"),
			CSHouse_SpanIsPier(Inverted, 80.0f, true));
	}

	// ---- ⑥ 跨度本身：只认"两侧都是落地的拱"，墩顶取两条起拱线的较低者 ----
	{
		FCSWallOpening Left;
		Left.Shape = ECSOpeningShape::Arch;
		Left.EdgeIndex = 0;
		Left.CenterS = 200.0f;
		Left.Width = 120.0f;
		Left.Z0 = 0.0f;
		Left.Z1 = 220.0f;                       // 起拱线 220 − 60 = 160

		FCSWallOpening Right = Left;
		Right.CenterS = 360.0f;
		Right.Width = 100.0f;
		Right.Z1 = 260.0f;                      // 起拱线 260 − 50 = 210，比左边高

		float Span = 0.0f, TopZ = 0.0f;
		TestTrue(TEXT("Two grounded arches on one edge yield a span"), CSHouse_PierSpanBetween(Left, Right, Span, TopZ));
		TestEqual(TEXT("The span is measured between the openings, not between the panel cells"), Span, 50.0f);
		// 取较高者会在两条起拱线之间留一条横缝：低的那一拱在那里已经把洞开满了，
		// 而墩上的灰泥还没开始砌。
		TestEqual(TEXT("The pier top is the lower of the two springing lines"), TopZ, 160.0f);

		FCSWallOpening OtherEdge = Right;
		OtherEdge.EdgeIndex = 1;
		TestFalse(TEXT("Openings on different edges are not a span"),
			CSHouse_PierSpanBetween(Left, OtherEdge, Span, TopZ));

		FCSWallOpening Window = Right;
		Window.Z0 = 90.0f;                      // 窗：它下面那截墙是窗台，不是墩
		TestFalse(TEXT("A sill-height opening is not a pier side"), CSHouse_PierSpanBetween(Left, Window, Span, TopZ));

		FCSWallOpening RectHole = Right;
		RectHole.Shape = ECSOpeningShape::Rect; // 矩形洞没有起拱线 ⇒ 墩顶无定义
		TestFalse(TEXT("A rectangular opening is not a pier side"), CSHouse_PierSpanBetween(Left, RectHole, Span, TopZ));

		FCSWallOpening Overlapping = Right;
		Overlapping.CenterS = 250.0f;           // 与左洞重叠：谓词本该挡住，这里不许当成负跨度
		TestFalse(TEXT("Overlapping openings are not a span"), CSHouse_PierSpanBetween(Left, Overlapping, Span, TopZ));
	}

	return true;
}

// -----------------------------------------------------------------------------
// 连续拱之间到底还剩不剩灰泥（计划 D6 的实拍裁决，Docs/TinyGlade/img/TG_continuous_arches.png）
//
// 实拍读出来的是：起拱线以下**没有"墙"这个表面**，两侧直接透到背景草地，那一截只有门樘砖
// 自己站着。
//
// ⚠️ 判据必须**两条一起立**，只立一条都会写成空判据或者写反：
//  ① **几何仍然实心**（2026-08-30 裁决三：避免所有真几何洞）—— 面板照砌，一块都不许少。
//     只测"跨度里没实体"的话，等于把裁决三反过来钉死了。
//  ② **像素上被裁掉** —— 拿顶点色 B 通道还原形状 id、拿 UV1 当 q，逐字复刻材质那一侧的判据。
//     只测几何的话，把裁剪场漏掉也照样绿。
//
// ⚠️ 还有一条容易写成恒真：默认参数下拱宽 = 段距 − 墩宽，两个**面板格**首尾相接，格之间那块
// "实心段"宽度本来就是 0 —— 断言"实心段没了"永远成立。真正要抹掉的灰泥是两个格各自伸进跨度的
// **端盖**（各半个墩宽），所以探针必须打在跨度里、贴着洞缘那两侧。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHousePierPlasterTest,
	"PCGPlugins.ComputeShaderGenerator.House.PierPlaster",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/**
 * 逐字复刻材质那一侧：从顶点色 B 还原形状 id（255 = 这块面板没有洞）、拿 UV1 当 q，
 * 判这个顶点会不会活下来。**判据的真源只有一处**（`CSHouse_ClipKeeps`），这里只负责把
 * 常驻流里那两个通道解回它要的输入 —— 与 `FCSHouseMeshWriter::SetPanel` 写进去的口径对称。
 */
bool CSHouseTest_VertexKept(const FCSGpuMeshCPUData& S, int32 Index)
{
	const int32 ShapeId = FMath::RoundToInt(S.Colors[Index].Z * 255.0f);
	if (ShapeId == 255) return true;
	FCSOpeningClipField Field;
	Field.bValid = true;
	Field.Shape = ECSOpeningShape(uint8(ShapeId));
	return CSHouse_ClipKeeps(Field, S.TexCoordChannels[1][Index]);
}
}

bool FCSHousePierPlasterTest::RunTest(const FString& Parameters)
{
	const FCSHouseTestBodyCase Case = { 600, 400, 300.0f, 24.0f, TEXT("piers") };
	const float PierWidth = 40.0f;

	// 边 0 = 南墙：Start = (−HX, −HY)、U = +X、In = +Y、Len = FootprintSize.X（见 CSHouse_GetEdge）。
	// 墙空间 (S, 厚度比 FD, Z) → 局部 (−HX + S, −HY + FD·T, Z)。
	auto Probe = [&Case](float S, double FD, float Z)
	{
		return FVector(-Case.SizeX * 0.5 + S, -Case.SizeY * 0.5 + FD * Case.WallThickness, Z);
	};
	auto LocalToS = [&Case](const FVector3f& P) { return float(P.X + Case.SizeX * 0.5); };

	FCSWallOpening Left;
	Left.Shape = ECSOpeningShape::Arch;
	Left.EdgeIndex = 0;
	Left.CenterS = 200.0f;
	Left.Width = 120.0f;
	Left.Z0 = 0.0f;
	Left.Z1 = 220.0f;
	FCSWallOpening Right = Left;
	Right.CenterS = 360.0f;                       // 跨度 = 300 − 260 = 40，起拱线 160

	float Span = 0.0f, PierTopZ = 0.0f;
	TestTrue(TEXT("The fixture really is a pier span"), CSHouse_PierSpanBetween(Left, Right, Span, PierTopZ));
	TestEqual(TEXT("The fixture span"), Span, PierWidth);
	TestEqual(TEXT("The fixture pier top"), PierTopZ, 160.0f);

	// 探针：跨度**内部**、贴着两侧洞缘，避开 260/280/300 那几个盒边界（竖直射线落在边界上判定是模糊的）。
	const float SpanS[] = { 264.0f, 271.5f, 288.5f, 296.0f };
	const double DepthFrac[] = { 0.19, 0.5, 0.81 };
	const float BelowZ[] = { 4.0f, 47.0f, 112.0f, 155.0f };
	const float AboveZ[] = { 168.0f, 213.0f, 271.0f };

	// 跨度里、起拱线以下的**顶点**（面板盒的下边那圈角点）。两种配置都要看它们，
	// 因为"灰泥没了"与"灰泥还在"的区别整个落在这些顶点的 UV1 上。
	auto CountSpanFloorVerts = [&](const FCSGpuMeshCPUData& S, int32& OutKept)
	{
		int32 Total = 0;
		OutKept = 0;
		for (int32 Index = 0; Index < S.Positions.Num(); ++Index)
		{
			const FVector3f& P = S.Positions[Index];
			if (FMath::Abs(P.Z) > 0.01f) continue;                                    // 只看落地那一圈
			if (P.Y > -Case.SizeY * 0.5 + Case.WallThickness + 0.01) continue;        // 只看南墙那一条带
			const float PS = LocalToS(P);
			if (PS < Left.S1() - 0.5f || PS > Right.S0() + 0.5f) continue;            // 只看跨度
			++Total;
			if (CSHouseTest_VertexKept(S, Index)) ++OutKept;
		}
		return Total;
	};

	// ---- ① 判为墙（不打样式位）：跨度里从地面到墙顶都是实心灰泥 —— 这是**改之前**的观感 ----
	{
		FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
		Body.PierWidth = PierWidth;
		Body.Openings = { Left, Right };
		FCSGpuMeshCPUData S;
		CSHouse_BuildBodySoup(Body, S);

		int32 Missing = 0;
		for (float PS : SpanS) for (double FD : DepthFrac) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) ++Missing;
		}
		// 这条同时是下面那条断言的**对照组**：它红了就说明探针根本没打在墙上，
		// 下面那条"墩跨度被裁掉了"也就成了空判据。
		TestTrue(FString::Printf(TEXT("Without the pier flags the span is solid plaster (%d hollow samples)"), Missing),
			Missing == 0);

		int32 Kept = 0;
		const int32 Total = CountSpanFloorVerts(S, Kept);
		TestTrue(FString::Printf(TEXT("Without the pier flags the span floor is drawn (%d of %d vertices kept)"), Kept, Total),
			Total > 0 && Kept > 0);
	}

	// ---- ② 判为墩 ----
	FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
	Body.PierWidth = PierWidth;
	FCSWallOpening PierLeft = Left, PierRight = Right;
	PierLeft.StyleFlags |= CSHouse_StylePierAfter;
	PierRight.StyleFlags |= CSHouse_StylePierBefore;
	Body.Openings = { PierLeft, PierRight };
	FCSGpuMeshCPUData S;
	CSHouse_BuildBodySoup(Body, S);

	// ①' **几何仍然实心**（裁决三）：墩不是靠"不生成面板"做出来的，跨度里每一点都得被实体包住 ——
	// 起拱线上下都要测，只测上半段等于没测。
	{
		int32 Hollow = 0;
		FVector WorstAt = FVector::ZeroVector;
		for (float PS : SpanS) for (double FD : DepthFrac)
		{
			for (float PZ : BelowZ) if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) { ++Hollow; WorstAt = Probe(PS, FD, PZ); }
			for (float PZ : AboveZ) if (CSHouseTest_SolidDepth(S, Probe(PS, FD, PZ)) < 1) { ++Hollow; WorstAt = Probe(PS, FD, PZ); }
		}
		TestTrue(FString::Printf(TEXT("The pier span is still solid geometry (%d hollow samples, e.g. %s)"),
			Hollow, *WorstAt.ToString()), Hollow == 0);
	}

	// ②' **像素上整片裁掉**：跨度落地那一圈顶点一个都不许活下来。
	{
		int32 Kept = 0;
		const int32 Total = CountSpanFloorVerts(S, Kept);
		TestTrue(FString::Printf(TEXT("The pier span floor is clipped away (%d of %d vertices survive)"), Kept, Total),
			Total > 0 && Kept == 0);
	}

	// 裁剪场本身：起拱线以下裁掉、以上保留，且面板的两片端盖落在洞**内**（与普通面板相反）——
	// 端盖被保住的话，跨度两端会各立起一片贯穿墙厚的灰泥薄片，正好卡在墩的位置上。
	{
		const float SpanMin = Left.S1() - CSHouse_PierCutMargin * 0.1f;
		const float SpanMax = Right.S0() + CSHouse_PierCutMargin * 0.1f;
		const FCSOpeningClipField Cut = CSHouse_PierClipField(SpanMin, SpanMax, PierTopZ);
		TestTrue(TEXT("The pier cut is a valid field"), Cut.bValid);
		for (float PS : SpanS)
		{
			for (float PZ : BelowZ) TestFalse(TEXT("Under the springing line the pier cut discards"), CSHouse_ClipKeeps(Cut, Cut.Eval(PS, PZ)));
			for (float PZ : AboveZ) TestTrue(TEXT("Above the springing line the pier cut keeps"), CSHouse_ClipKeeps(Cut, Cut.Eval(PS, PZ)));
		}
		TestFalse(TEXT("The panel's own end caps fall inside the cut"),
			CSHouse_ClipKeeps(Cut, Cut.Eval(SpanMin, 1.0f)) || CSHouse_ClipKeeps(Cut, Cut.Eval(SpanMax, 1.0f)));
	}

	// **只**抹掉墩那一侧：两拱各自的外侧（没打样式位的那半个墩）一块灰泥都不许少，
	// 否则整面墙会跟着塌成一排孤零零的拱。几何与像素两条一起看。
	{
		int32 MissingOuter = 0, ClippedOuter = 0;
		const FCSOpeningClipField LeftField = CSHouse_ComputeClipField(PierLeft);
		const FCSOpeningClipField RightField = CSHouse_ComputeClipField(PierRight);
		// 落在两拱**外侧**那半个墩里（左 [120,140]、右 [420,440]），不是拱肚子底下 ——
		// 拱肚子那块面板盒无论如何都在（洞是逐像素 clip 的），拿它当探针证明不了任何事。
		for (float PS : { 123.7f, 137.2f }) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, PZ)) < 1) ++MissingOuter;
			if (!CSHouse_ClipKeeps(LeftField, LeftField.Eval(PS, PZ))) ++ClippedOuter;
		}
		for (float PS : { 423.5f, 437.9f }) for (float PZ : BelowZ)
		{
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, PZ)) < 1) ++MissingOuter;
			if (!CSHouse_ClipKeeps(RightField, RightField.Eval(PS, PZ))) ++ClippedOuter;
		}
		TestTrue(FString::Printf(TEXT("The far side of each arch keeps its plaster (%d hollow, %d clipped)"),
			MissingOuter, ClippedOuter), MissingOuter == 0 && ClippedOuter == 0);
	}

	// 墙顶那一整条（灰泥 + 墙顶砖带）必须**没有断口** —— 墩是把墙"裁空"，不是把墙"切断"。
	{
		int32 MissingTop = 0;
		for (int32 K = 0; K < 59; ++K)
		{
			const float PS = 5.3f + K * 10.0f;   // 故意不取整数比例，避开每一处盒边界
			if (CSHouseTest_SolidDepth(S, Probe(PS, 0.5, 285.0f)) < 1) ++MissingTop;
		}
		TestTrue(FString::Printf(TEXT("The wall head runs unbroken across the whole edge (%d hollow samples)"), MissingTop),
			MissingTop == 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 窗（D8）：**谓词说能放的窗，几何一定砌得出**
//
// 这是 C1 选甲（谓词降维成同边一维 S 区间）的全部意义，也是 D8「谓词是唯一真源」那条纪律
// 唯一说得清的执行面。它必须**跨过两侧**：一侧调真正的谓词 `CSHouse_QueryOpening`，另一侧跑
// 真正的产线 `CSHouse_BuildBodySoup` + `CSHouseFrame::BuildEdgeElements`，中间不许有任何
// "测试自己写的镜像"——镜像只会证明两份代码长得像，证明不了谓词与几何同维。
//
// 观察通道用常驻流本来就带着的两条语义（`ACSHouseActor` 的通道字典），不另开后门：
//   · B = 洞形状 id / 255，255 = 这块面板没有洞 ⇒ **带裁剪场的顶点数 / 36 就是洞板数**
//     （一块洞板 = 一次 `AddBox` = 6 面 × 2 三角 × 3 顶点）。谓词放行的洞若被
//     `CellMax - CellMin < O.Width` 那条静默 `continue` 丢掉，这个数就少 36 —— 那正是
//     二维谓词时代"谓词说能放、几何砌不出"的**唯一**症状，画面上与"用户根本没放窗"逐像素相同。
//   · G = 洞的 Tag / 255。窗的 Tag 从 0x80 起编（`ACSHouseActor::BuildWindowOpenings`），
//     与门的子段号不撞，所以能把"这一扇窗的那块洞板"从整栋房子里精确挑出来。
//
// ⚠️ **谓词与几何必须吃同一个 `PierWidth`**：面板格（`CSHouse_OpeningCell`）两边都用它，
// 各填各的会让这条断言在参数不一致时静默变假。产线上两者同出于 `ACSHouseActor::PierWidth`，
// 这里也只写一个常量。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWindowPredicateMatchesGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.House.WindowPredicateMatchesGeometry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

namespace
{
/**
 * 带裁剪场的**墙**面板顶点数。/36 = 洞板数（一块洞板恰好一次 `AddBox`）。
 *
 * ⚠️ **必须先按 R 通道筛出墙**：只按 B 数会把别的构件也算进来（历史上两块实体屋面板就撞过
 * 这一条 —— 它们的 B 恒为 0，按字典读正好是"形状 id = Arch"，一栋空房子能数出两块"洞板"）。
 * 屋面现在改成瓦片实例、根本不进这份三角汤，但这条筛选照留：柱子、包边砖同样不是墙。
 */
int32 CSHouseTest_ClipVertCount(const FCSGpuMeshCPUData& S)
{
	int32 Count = 0;
	for (const FVector4f& C : S.Colors)
	{
		if (FMath::RoundToInt(C.X * 255.0f) != int32(ECSHousePart::Wall)) continue;
		if (FMath::RoundToInt(C.Z * 255.0f) == 255) continue;   // 255 = 这块面板没有洞
		++Count;
	}
	return Count;
}

/** 挑出带某个 Tag 的洞板顶点（窗的 Tag ≥ 0x80，与门的子段号不撞）。 */
int32 CSHouseTest_TaggedClipVerts(const FCSGpuMeshCPUData& S, uint8 Tag, TArray<int32>& OutIndices)
{
	OutIndices.Reset();
	for (int32 Index = 0; Index < S.Colors.Num(); ++Index)
	{
		if (FMath::RoundToInt(S.Colors[Index].X * 255.0f) != int32(ECSHousePart::Wall)) continue;
		if (FMath::RoundToInt(S.Colors[Index].Z * 255.0f) == 255) continue;
		if (FMath::RoundToInt(S.Colors[Index].Y * 255.0f) != int32(Tag)) continue;
		OutIndices.Add(Index);
	}
	return OutIndices.Num();
}
}

bool FCSHouseWindowPredicateMatchesGeometryTest::RunTest(const FString& Parameters)
{
	// 出厂参数（CDO），断言才真的钉住线上那一份口径。
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	const float Pier = CDO->PierWidth;
	const FCSHouseTestBodyCase Case = { 600, 400, 300.0f, 24.0f, TEXT("windows") };
	const FVector2D Foot(Case.SizeX, Case.SizeY);

	// 两扇门：一扇占住南墙（边 0）的右端 —— 窗挤到它跟前就该被判 `OverlapsOpening`（门拱优先）；
	// 另一扇在北墙（边 2），用来验"不同边的洞永远不冲突"这条在扫描里也成立。
	TArray<FCSWallOpening> Doors;
	{
		FCSWallOpening D0 = CSHouseTest_DemoArch(460.0f);
		D0.Type = ECSOpeningType::Door;
		D0.EdgeIndex = 0;
		D0.Tag = 2;
		Doors.Add(D0);
		FCSWallOpening D2 = CSHouseTest_DemoArch(300.0f);
		D2.Type = ECSOpeningType::Door;
		D2.EdgeIndex = 2;
		D2.Tag = 1;
		Doors.Add(D2);
	}

	FCSOpeningSite Site;
	Site.Footprint = FCSHouseFootprint::MakeRect(Foot);
	Site.WallThickness = Case.WallThickness;
	Site.WallHeight = Case.WallHeight;
	Site.LintelBand = CDO->LintelBand;
	Site.CornerMargin = CDO->CornerMargin;
	Site.PierWidth = Pier;
	Site.OpeningClearance = CDO->OpeningClearance;
	Site.MinSillZ = CDO->WindowMinSillZ;
	Site.bPierStyleEnabled = CDO->bPierStyleEnabled;
	Site.PierRestoreWidth = FMath::Max(CDO->PierStyleRestoreWidth, CDO->PierStyleMaxWidth);
	Site.Openings = Doors;

	// 只有门时的洞板数：后面每放行一扇窗，这个数必须**恰好 +36**（多一块洞板，一块不多一块不少）。
	FCSHouseBodyDesc BaseBody = CSHouseTest_MakeBody(Case);
	BaseBody.PierWidth = Pier;
	BaseBody.Openings = Doors;
	FCSGpuMeshCPUData BaseSoup;
	CSHouse_BuildBodySoup(BaseBody, BaseSoup);
	const int32 BaseClipVerts = CSHouseTest_ClipVertCount(BaseSoup);
	TestEqual(TEXT("Two doors lay two clip panels (36 verts each)"), BaseClipVerts, 2 * 36);

	// 五组尺寸：前三组会被放行，后两组**必然**分别撞上窗台下限与过梁带 —— 拒绝理由的直方图
	// 因此不是空的（不然"扫描也拒了很多"那条会退化成只测到护角一种）。
	struct FSize { float Width, Z0, Height; };
	const FSize Sizes[5] = {
		{ 60.0f,  90.0f, 110.0f },   // 小窗
		{ 78.0f,  90.0f, 110.0f },   // TG window_cottage_1x1 的实测宽
		{ 120.0f, 95.0f, 140.0f },   // 宽窗
		{ 70.0f,  20.0f,  90.0f },   // 窗台压在地上 ⇒ SillTooLow
		{ 70.0f, 150.0f, 130.0f },   // 洞顶吃掉过梁带 ⇒ AboveEave
	};
	const ECSOpeningShape Shapes[2] = { ECSOpeningShape::Rect, ECSOpeningShape::Arch };
	// 边 0（南墙，长 600、有门）与边 1（东墙，长 400 − 2T = 352、无门）：后者顺带把
	// `CSHouse_GetEdge` 对短边的缩短口径也带进这条断言。
	const int32 Edges[2] = { 0, 1 };

	int32 Accepted = 0, Rejected = 0, DroppedByGeometry = 0, BadPanel = 0, MissingSill = 0;
	int32 WindowBricks = 0, DoorPaths = 0;
	int32 RejectHistogram[8] = { 0 };
	FString FirstFailure;

	for (const int32 Edge : Edges)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Foot, Case.WallThickness);
		for (const ECSOpeningShape Shape : Shapes)
		{
			for (const FSize& Size : Sizes)
			{
				for (int32 Step = 0; Step <= 75; ++Step)
				{
					FCSWallOpening Window;
					Window.Type = ECSOpeningType::Window;
					Window.Shape = Shape;
					Window.EdgeIndex = Edge;
					Window.CenterS = float(Step) * 8.0f;
					Window.Width = Size.Width;
					Window.Z0 = Size.Z0;
					Window.Z1 = Size.Z0 + Size.Height;
					Window.SourceId = FGuid(0x57494E44u, 0u, 0u, 0u);
					Window.Tag = 0x80;

					const ECSFeatureReject Reason = CSHouse_QueryOpening(Site, Window);
					if (Reason != ECSFeatureReject::None)
					{
						++Rejected;
						++RejectHistogram[FMath::Clamp(int32(Reason), 0, 7)];
						continue;
					}
					++Accepted;

					// ---- 几何那一侧：跑真正的产线，不写镜像 ----
					TArray<FCSWallOpening> All = Doors;
					All.Add(Window);
					All.Sort([](const FCSWallOpening& A, const FCSWallOpening& B)
					{
						return A.EdgeIndex != B.EdgeIndex ? A.EdgeIndex < B.EdgeIndex : A.CenterS < B.CenterS;
					});

					FCSHouseBodyDesc Body = CSHouseTest_MakeBody(Case);
					Body.PierWidth = Pier;
					Body.Openings = All;
					FCSGpuMeshCPUData Soup;
					CSHouse_BuildBodySoup(Body, Soup);

					// ① 洞板数恰好 +36 —— 那条静默 `continue` 对过了谓词的洞不可达，
					//    而且新来的窗也没有把哪扇门的面板挤掉（挤掉的话这个数会不增反平）。
					if (CSHouseTest_ClipVertCount(Soup) != BaseClipVerts + 36)
					{
						++DroppedByGeometry;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f W=%.0f Z=[%.0f,%.0f]"),
								Edge, int32(Shape), Window.CenterS, Window.Width, Window.Z0, Window.Z1);
						}
						continue;
					}

					// ② 这一扇窗那块洞板：恰好 36 个顶点、横向盖住整个洞、竖向从洞底一直到墙顶，
					//    而且**它自己的每个角点都活得下来**（端盖落进洞内的话，两侧的墙会跟着被切）。
					TArray<int32> Verts;
					const int32 TaggedCount = CSHouseTest_TaggedClipVerts(Soup, Window.Tag, Verts);
					float MinS = TNumericLimits<float>::Max(), MaxS = -TNumericLimits<float>::Max();
					float MinZ = TNumericLimits<float>::Max(), MaxZ = -TNumericLimits<float>::Max();
					int32 ClippedCorners = 0;
					for (const int32 Index : Verts)
					{
						const FVector3f& P = Soup.Positions[Index];
						const float S = float((FVector2D(P.X, P.Y) - F.Start) | F.U);
						MinS = FMath::Min(MinS, S);
						MaxS = FMath::Max(MaxS, S);
						MinZ = FMath::Min(MinZ, P.Z);
						MaxZ = FMath::Max(MaxZ, P.Z);
						if (!CSHouseTest_VertexKept(Soup, Index)) ++ClippedCorners;
					}
					const bool bPanelOk = TaggedCount == 36
						&& MinS <= Window.S0() + 0.02f && MaxS >= Window.S1() - 0.02f
						&& FMath::IsNearlyEqual(MinZ, Window.Z0, 0.02f)
						&& FMath::IsNearlyEqual(MaxZ, Case.WallHeight, 0.02f)
						&& ClippedCorners == 0;
					if (!bPanelOk)
					{
						++BadPanel;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(
								TEXT("edge=%d shape=%d S=%.0f W=%.0f：verts=%d S=[%.1f,%.1f] 洞=[%.1f,%.1f] Z=[%.1f,%.1f] 被裁角点=%d"),
								Edge, int32(Shape), Window.CenterS, Window.Width, TaggedCount,
								MinS, MaxS, Window.S0(), Window.S1(), MinZ, MaxZ, ClippedCorners);
						}
						continue;
					}

					// ③ 窗台：洞面板底下那一截必须是**实心**（2026-08-30 裁决三：几何永远实心）。
					const FVector2D Flat = F.Start + F.U * Window.CenterS + F.In * (0.5 * Case.WallThickness);
					if (CSHouseTest_SolidDepth(Soup, FVector(Flat.X, Flat.Y, Window.Z0 * 0.5f)) < 1)
					{
						++MissingSill;
						if (FirstFailure.IsEmpty())
						{
							FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f 的窗台底下是空的"),
								Edge, int32(Shape), Window.CenterS);
						}
						continue;
					}

					// ④ 砖那一侧：**窗一条框砖路都不许有**（2026-09-06 裁决「附属物持有 mesh」的
					// 直接后果：洞缘由窗自带的预制框盖住，房子再砌一圈就是双份几何）。
					//
					// ⚠️ 同一次调用里门/拱**照旧**出它那一圈 —— 门是房子自己生成的（`CSHouseActor.cpp`
					// 里由道路推导），没有任何附属物替它盖洞缘，那一圈砖**就是**门框。所以这里一次验
					// 两头：窗为零、门非零。只验前一半的话，把整条产线掐死也能全绿。
					CSHouseFrame::FBrickParams Params;
					Params.Length = CDO->FrameBrickLength;
					Params.Gap = CDO->FrameBrickGap;
					Params.MaxBricks = 4096;
					CSHouseFrame::FWallFrame Frame;
					TArray<CSHouseFrame::FElement> Elements;
					TArray<FCSWallOpening> EdgeOnly;
					for (const FCSWallOpening& O : All) if (O.EdgeIndex == Edge) EdgeOnly.Add(O);
					CSHouseFrame::BuildEdgeElements(Frame, EdgeOnly, Params, Elements);
					for (const CSHouseFrame::FElement& E : Elements)
					{
						if (E.BrickCount <= 0) continue;
						// 窗那条路的指纹：同一个 S **且从窗台起砌**（`BaseZ` = 洞底）。门与墩都从地面起砌 ——
						// 少了离地这一半，一根正好骑在窗中线上的墩会被误判成"窗又长砖了"。
						if (FMath::IsNearlyEqual(E.Path.CenterS, Window.CenterS, 0.01f)
							&& FMath::IsNearlyEqual(E.Path.BaseZ, Window.Z0, 0.01f))
						{
							++WindowBricks;
							if (FirstFailure.IsEmpty())
							{
								FirstFailure = FString::Printf(TEXT("edge=%d shape=%d S=%.0f 的窗还在长框砖（%d 块）"),
									Edge, int32(Shape), Window.CenterS, E.BrickCount);
							}
						}
						else
						{
							++DoorPaths;
						}
					}
				}
			}
		}
	}

	AddInfo(FString::Printf(TEXT("sweep: %d accepted / %d rejected (corner=%d eave=%d sill=%d overlap=%d)"),
		Accepted, Rejected,
		RejectHistogram[int32(ECSFeatureReject::NearCorner)],
		RejectHistogram[int32(ECSFeatureReject::AboveEave)],
		RejectHistogram[int32(ECSFeatureReject::SillTooLow)],
		RejectHistogram[int32(ECSFeatureReject::OverlapsOpening)]));

	// 非空判据：扫描必须两边都踩到，而且每一条拒绝理由都真的发生过 —— 否则这条测试测的是空气。
	TestTrue(FString::Printf(TEXT("The sweep accepts a lot of windows (%d)"), Accepted), Accepted > 100);
	TestTrue(FString::Printf(TEXT("The sweep also rejects a lot (%d)"), Rejected), Rejected > 100);
	TestTrue(TEXT("Windows near the wall ends are rejected"), RejectHistogram[int32(ECSFeatureReject::NearCorner)] > 0);
	TestTrue(TEXT("Windows overlapping a door's panel cell are rejected (arches win, D6)"),
		RejectHistogram[int32(ECSFeatureReject::OverlapsOpening)] > 0);
	TestTrue(TEXT("Windows sitting on the floor are rejected"), RejectHistogram[int32(ECSFeatureReject::SillTooLow)] > 0);
	TestTrue(TEXT("Windows eating into the wall head are rejected"), RejectHistogram[int32(ECSFeatureReject::AboveEave)] > 0);

	// **本条测试的全部意义。**
	TestEqual(FString::Printf(TEXT("Every accepted window really gets its own clip panel (%s)"),
		FirstFailure.IsEmpty() ? TEXT("nothing dropped") : *FirstFailure), DroppedByGeometry, 0);
	TestEqual(TEXT("...and that panel spans the whole opening with its end caps outside the clip"), BadPanel, 0);
	TestEqual(TEXT("...and the wall under the sill stays solid (裁决三：几何永远实心)"), MissingSill, 0);
	TestEqual(TEXT("...and grows no frame bricks of its own (its prefab frame covers the edge)"), WindowBricks, 0);
	TestTrue(FString::Printf(TEXT("...while the doors sharing those edges still get their brick ring (%d paths)"),
		DoorPaths), DoorPaths > 0);

	// ---- 墩跨度不接受窗（计划 D6）：默认参数下不可达，必须把跨度撑开才测得到 ----
	{
		FCSWallOpening Left = CSHouseTest_DemoArch(160.0f);
		Left.EdgeIndex = 0;
		FCSWallOpening Right = CSHouseTest_DemoArch(440.0f);
		Right.EdgeIndex = 0;
		const FCSWallOpening Pair[2] = { Left, Right };
		const float SpanMin = Left.S1(), SpanMax = Right.S0();
		TestTrue(FString::Printf(TEXT("The fixture leaves a wide span (%.0f cm)"), SpanMax - SpanMin),
			SpanMax - SpanMin > 120.0f);

		FCSOpeningSite Wide = Site;
		Wide.Openings = MakeArrayView(Pair, 2);
		Wide.PierRestoreWidth = 200.0f;     // 反常参数：这么宽也按墩处理，正是它让这条路可达

		FCSWallOpening Window;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.EdgeIndex = 0;
		Window.CenterS = (SpanMin + SpanMax) * 0.5f;
		Window.Width = 60.0f;
		Window.Z0 = 90.0f;
		Window.Z1 = 200.0f;
		Window.SourceId = FGuid(0x57494E44u, 9u, 0u, 0u);
		TestTrue(TEXT("A window in the middle of a pier span is rejected"),
			CSHouse_QueryOpening(Wide, Window) == ECSFeatureReject::OnPierSpan);

		// 对照组一：墩样式关掉 ⇒ 那截墙是普通灰泥，同一扇窗放得下。没有它，上一条可能只是在
		// 描述"那个位置本来就放不下"。
		FCSOpeningSite NoPier = Wide;
		NoPier.bPierStyleEnabled = false;
		TestTrue(TEXT("With the pier style off the very same window is fine"),
			CSHouse_QueryOpening(NoPier, Window) == ECSFeatureReject::None);

		// 对照组二：阈值调回正常 ⇒ 这条跨度宽到永远判不成墩，窗照样放得下
		// （判据认的是"会不会被判成墩"，不是"是不是一段跨度"）。
		FCSOpeningSite NormalThreshold = Wide;
		NormalThreshold.PierRestoreWidth = 75.0f;
		TestTrue(TEXT("A span too wide to ever become a pier accepts the window"),
			CSHouse_QueryOpening(NormalThreshold, Window) == ECSFeatureReject::None);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 门框砖对窗**一块不出**（2026-09-06 裁决「附属物持有 mesh」）
//
// 窗的洞缘由附属物自带的预制框盖住，窗周围不走任何砖头补全 —— 曾经沿洞底补的「窗台底边」
// 第四段已于 2026-09-10 整条删除（本条的前身是 `House.FrameWindowSill`）。三条判据：
//   ① **门一步不动**（回归护栏）：落地拱的弧长仍是 2×樘 + πR。
//   ② **离地洞的砖路仍骑在 clip 场的零等值线上**：`MakeOpeningPath` 对矩形 / 拱照旧出三段路，
//      沿整条路密扫，往面内朝外法线挪 ε 必须落在洞外（保留）、往反方向挪 ε 必须落在洞内
//      （被 discard）。
//   ③ **同一个洞只换 `Type` 送两遍**：按门送进去照铺砖，按窗送进去一块砖、一条空路都不出。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFrameSkipsWindowsTest,
	"PCGPlugins.ComputeShaderGenerator.House.FrameSkipsWindows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFrameSkipsWindowsTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* CDO = GetDefault<ACSHouseActor>();
	CSHouseFrame::FBrickParams Params;
	Params.Length = CDO->FrameBrickLength;
	Params.Gap = CDO->FrameBrickGap;
	Params.MaxBricks = 4096;
	CSHouseFrame::FWallFrame Frame;   // S 对 +X、Z 对 +Z：墙空间 = 世界空间

	// ---- ① 门：弧长 = 两樘 + 半圆 ----
	{
		const FCSWallOpening Door = CSHouseTest_DemoArch(300.0f);
		CSHouseFrame::FPath Path;
		TestTrue(TEXT("A door still yields a path"), CSHouseFrame::MakeOpeningPath(Door, Path));
		const float Jamb = FMath::Max(Door.Z1 - Door.HalfWidth(), Door.Z0) - Door.Z0;
		TestTrue(TEXT("A door's arc length is two jambs plus a half circle"),
			FMath::IsNearlyEqual(Path.TotalLen(), 2.0f * Jamb + PI * Door.HalfWidth(), 0.001f));
	}

	// ---- 三种窗：矩形两档 + 尖顶（拱），窗台都离地 ----
	struct FCase { ECSOpeningShape Shape; float Width, Z0, Z1; const TCHAR* What; };
	const FCase Cases[3] = {
		{ ECSOpeningShape::Rect, 78.0f,  90.0f, 200.0f, TEXT("cottage-sized rect window") },
		{ ECSOpeningShape::Rect, 143.5f, 90.0f, 250.0f, TEXT("2x1 rect window") },
		{ ECSOpeningShape::Arch, 69.4f,  95.0f, 240.0f, TEXT("arched window") },
	};

	for (const FCase& C : Cases)
	{
		FCSWallOpening Window;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = C.Shape;
		Window.EdgeIndex = 0;
		Window.CenterS = 300.0f;
		Window.Width = C.Width;
		Window.Z0 = C.Z0;
		Window.Z1 = C.Z1;

		CSHouseFrame::FPath Path;
		TestTrue(FString::Printf(TEXT("%s yields a path"), C.What), CSHouseFrame::MakeOpeningPath(Window, Path));

		// ---- ② 砖路骑在零等值线上、法线朝外 ----
		const FCSOpeningClipField Field = CSHouse_ComputeClipField(Window);
		constexpr float Eps = 0.75f;      // 远大于 float 噪声，远小于一块砖
		constexpr float Corner = 2.0f;    // 接缝两侧各让开这么多弧长，理由见下
		int32 OutsideBad = 0, InsideBad = 0, Samples = 0;
		const float Total = Path.TotalLen();
		// 矩形洞平顶两端的接缝处切向转 90°，法线在那一点没有定义 —— 在折角上取样等于问"这个角
		// 朝哪边"，两个答案都对。所以把各接缝（0 / L0 / L0+L1 / Total）各让开 2 cm。
		const float Junctions[4] = { 0.0f, Path.LeftLen(), Path.LeftLen() + Path.MidLen(), Total };
		for (int32 K = 0; K <= 400; ++K)
		{
			const float Arc = Total * float(K) / 400.0f;
			bool bAtCorner = false;
			for (const float J : Junctions) bAtCorner |= FMath::Abs(Arc - J) < Corner;
			if (bAtCorner) continue;
			FVector2f SZ, Tangent;
			CSHouseFrame::EvalPath(Path, Arc, SZ, Tangent);
			// 面内朝外法线 = 切向逆时针转 90°（`CSHouseFrame.usf` 里那一行的 CPU 对照）。
			const FVector2f Outward(-Tangent.Y, Tangent.X);
			++Samples;

			// 往洞里挪：必须被裁掉 —— 否则砖是骑在实墙上，洞缘那条断口根本没被盖住。
			if (CSHouse_ClipKeeps(Field, Field.Eval(SZ.X - Outward.X * Eps, SZ.Y - Outward.Y * Eps))) ++InsideBad;
			// 往洞外挪：必须保留。
			if (!CSHouse_ClipKeeps(Field, Field.Eval(SZ.X + Outward.X * Eps, SZ.Y + Outward.Y * Eps))) ++OutsideBad;
		}
		TestTrue(FString::Printf(TEXT("%s: the sweep really walks the path (%d samples)"), C.What, Samples), Samples > 300);
		TestEqual(FString::Printf(TEXT("%s: every path point has the opening on its inward side"), C.What), InsideBad, 0);
		TestEqual(FString::Printf(TEXT("%s: and solid wall on its outward side"), C.What), OutsideBad, 0);

		// ---- ③ 同一个洞只换 `Type`：门照铺，窗零块 ----
		//
		// 对照组先行：几何一模一样、按门送进去 ⇒ 照铺。没有它，下面的"零块"可能只是这个洞本来
		// 就铺不出砖。
		FCSWallOpening AsDoor = Window;
		AsDoor.Type = ECSOpeningType::Door;
		TArray<CSHouseFrame::FElement> Elements;
		const int32 Bricks = CSHouseFrame::BuildEdgeElements(Frame, MakeArrayView(&AsDoor, 1), Params, Elements);
		TestTrue(FString::Printf(TEXT("%s lays bricks when it is a door (%d in %d paths)"), C.What, Bricks, Elements.Num()),
			Bricks > 8 && Elements.Num() == 1);

		// **退役本身的判据**：几何一模一样、只是类型是窗 ⇒ 一块砖都不出。用"恰好为零"而不是
		// "少一些"，是为了让"把窗重新接回框砖"的改动没法悄悄通过。
		TArray<CSHouseFrame::FElement> AsWindow;
		const int32 WindowBricks = CSHouseFrame::BuildEdgeElements(Frame, MakeArrayView(&Window, 1), Params, AsWindow);
		TestEqual(FString::Printf(TEXT("%s lays no frame bricks as a window"), C.What), WindowBricks, 0);
		TestEqual(FString::Printf(TEXT("%s: not even an empty path"), C.What), AsWindow.Num(), 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// D7 接缝（裁决二）—— 纯函数性是**唯一**的硬证据
//
// "零共享状态、零归属、零撤销"这三句在代码里没有可以直接指的东西（没有 actor、没有表、
// 没有生命周期）。能证明它们的只有一条：**同一对房子，交换两者的顺序、或只重建其中一栋，
// 算出来的砖列逐位相同**。逐位不是洁癖 —— 两栋房各画一份重叠的砖，差一个 ulp 就可能闪。
// -----------------------------------------------------------------------------

namespace
{
CSHouseSeam::FHouse CSHouseTest_MakeSeamHouse(uint32 Tag, double X, double Y, float Yaw, double SizeX, double SizeY)
{
	CSHouseSeam::FHouse H;
	// 固定 GUID：规范序的键必须是确定的，随机 GUID 会让"交换顺序"这条断言时灵时不灵。
	H.Id = FGuid(Tag, 0x1111u, 0x2222u, 0x3333u);
	H.Center = FVector2D(X, Y);
	H.Yaw = Yaw;
	H.Footprint = FCSHouseFootprint::MakeRect(FVector2D(SizeX, SizeY));
	H.BaseZ = 0.0f;
	H.WallHeight = 300.0f;
	H.WallThickness = 24.0f;
	return H;
}

/** 砖路元素的**逐位**相等（浮点用 `==`，不是 IsNearlyEqual —— 这条断言的全部意义就在"逐位"）。 */
bool CSHouseTest_ElementBitEqual(const CSHouseFrame::FElement& A, const CSHouseFrame::FElement& B)
{
	auto SameVec = [](const FVector3f& U, const FVector3f& V) { return U.X == V.X && U.Y == V.Y && U.Z == V.Z; };
	return A.Path.BaseZ == B.Path.BaseZ && A.Path.TopZ == B.Path.TopZ
		&& A.Path.LeftS == B.Path.LeftS && A.Path.RightS == B.Path.RightS
		&& A.Path.CenterS == B.Path.CenterS && A.Path.Radius == B.Path.Radius
		&& A.Path.MidSweep == B.Path.MidSweep && A.Path.FlatLen == B.Path.FlatLen
		&& A.Path.MidKind == B.Path.MidKind && A.Path.bLeftJamb == B.Path.bLeftJamb
		&& A.Path.bRightJamb == B.Path.bRightJamb
		&& SameVec(A.Frame.Origin, B.Frame.Origin) && SameVec(A.Frame.AxisU, B.Frame.AxisU)
		&& SameVec(A.Frame.AxisV, B.Frame.AxisV)
		&& A.BrickBegin == B.BrickBegin && A.BrickCount == B.BrickCount
		&& A.Pitch == B.Pitch && A.HalfLen == B.HalfLen && A.LayoutScale == B.LayoutScale
		&& A.RandomBase == B.RandomBase;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamPureTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamIsAPureFunction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamPureTest::RunTest(const FString& Parameters)
{
	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	// 三组：轴对齐十字搭、带 yaw 的斜搭、角对角只咬一个角。
	struct FCase { const TCHAR* What; CSHouseSeam::FHouse A; CSHouseSeam::FHouse B; };
	const FCase Cases[] = {
		{ TEXT("axis-aligned cross"),
			CSHouseTest_MakeSeamHouse(1, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(2, 200.0, 150.0, 0.0f, 500.0, 500.0) },
		{ TEXT("yawed overlap"),
			CSHouseTest_MakeSeamHouse(3, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(4, 250.0, 120.0, 37.0f, 520.0, 380.0) },
		{ TEXT("corner bite"),
			CSHouseTest_MakeSeamHouse(5, 0.0, 0.0, 0.0f, 600.0, 400.0),
			CSHouseTest_MakeSeamHouse(6, 340.0, 240.0, 0.0f, 300.0, 300.0) },
	};

	for (const FCase& C : Cases)
	{
		TestTrue(FString::Printf(TEXT("%s: the two houses really do overlap (otherwise everything below is vacuous)"), C.What),
			CSHouseSeam::Intersects(C.A, C.B));
		TestTrue(FString::Printf(TEXT("%s: overlap is symmetric"), C.What),
			CSHouseSeam::Intersects(C.A, C.B) == CSHouseSeam::Intersects(C.B, C.A));

		// ---- ① 交换顺序 ⇒ 砖列逐位相同 ----
		TArray<CSHouseSeam::FCorner> CornersAB, CornersBA;
		const int32 NAB = CSHouseSeam::BuildCorners(C.A, C.B, CornersAB);
		const int32 NBA = CSHouseSeam::BuildCorners(C.B, C.A, CornersBA);
		// 交点数必是**偶数**且 ≥ 2：两个凸多边形的边界沿轮廓交替进出，进几次就出几次。
		// 轴对齐一般是 2、带 yaw 时 4/6/8 都可能，所以钉的是奇偶性而不是具体的数。
		TestTrue(FString::Printf(TEXT("%s: contour crossings come in pairs (got %d)"), C.What, NAB),
			NAB >= 2 && (NAB % 2) == 0);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the corner count"), C.What), NBA, NAB);

		TArray<CSHouseFrame::FElement> FromA, FromB;
		const int32 BricksA = CSHouseSeam::BuildCornerElements(CornersAB, CSHouseSeam::SeamSeed(C.A, C.B), Params, FromA);
		const int32 BricksB = CSHouseSeam::BuildCornerElements(CornersBA, CSHouseSeam::SeamSeed(C.B, C.A), Params, FromB);
		TestTrue(FString::Printf(TEXT("%s: the seam actually lays bricks (%d)"), C.What, BricksA), BricksA > 0);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the brick count"), C.What), BricksB, BricksA);
		TestEqual(FString::Printf(TEXT("%s: swapping the pair keeps the path count"), C.What), FromB.Num(), FromA.Num());

		int32 Differing = 0;
		for (int32 K = 0; K < FMath::Min(FromA.Num(), FromB.Num()); ++K)
		{
			if (!CSHouseTest_ElementBitEqual(FromA[K], FromB[K])) ++Differing;
		}
		// **这就是"零共享状态、零归属"的硬证据**：两栋房各自从自己的视角算，输出逐位重合。
		TestEqual(FString::Printf(TEXT("%s: the two houses compute the SAME seam bit for bit"), C.What), Differing, 0);

		// ---- ② 只重建其中一栋 ⇒ 它算出来的还是同一条缝（无记忆、无增量）----
		TArray<CSHouseSeam::FCorner> Again;
		TArray<CSHouseFrame::FElement> AgainElements;
		CSHouseSeam::BuildCorners(C.A, C.B, Again);
		CSHouseSeam::BuildCornerElements(Again, CSHouseSeam::SeamSeed(C.A, C.B), Params, AgainElements);
		int32 DriftedOnRebuild = 0;
		for (int32 K = 0; K < FMath::Min(FromA.Num(), AgainElements.Num()); ++K)
		{
			if (!CSHouseTest_ElementBitEqual(FromA[K], AgainElements[K])) ++DriftedOnRebuild;
		}
		TestEqual(FString::Printf(TEXT("%s: rebuilding only one house reproduces the seam bit for bit"), C.What),
			DriftedOnRebuild, 0);

		// ---- ③ 逐实例随机数从接缝身份来，不从槽位来 ----
		//
		// 槽位是"这栋房自己的第几块砖"，两栋房必然不同。把同一条缝接在一堆已有的门框砖后面
		// （模拟"这栋房还有门"），砖序号整体挪位 —— 随机数基必须**一个都不变**。
		TArray<CSHouseFrame::FElement> Shifted;
		CSHouseFrame::FElement Filler;   // 假装前面已经有 37 块门框砖
		Filler.BrickBegin = 0;
		Filler.BrickCount = 37;
		Shifted.Add(Filler);
		CSHouseSeam::BuildCornerElements(CornersAB, CSHouseSeam::SeamSeed(C.A, C.B), Params, Shifted);
		int32 SeedChanged = 0, SlotUnshifted = 0;
		for (int32 K = 0; K + 1 < Shifted.Num(); ++K)
		{
			if (Shifted[K + 1].RandomBase != FromA[K].RandomBase) ++SeedChanged;
			if (Shifted[K + 1].BrickBegin != FromA[K].BrickBegin + 37) ++SlotUnshifted;
		}
		TestEqual(FString::Printf(TEXT("%s: the slots really did shift (otherwise the next check is vacuous)"), C.What),
			SlotUnshifted, 0);
		TestEqual(FString::Printf(TEXT("%s: but the per-instance random seed is derived from the seam, not the slot"), C.What),
			SeedChanged, 0);
	}

	// ---- ④ 没碰上就什么都不出（触发条件是 footprint **真重叠**，不是"靠得近"）----
	const CSHouseSeam::FHouse Far = CSHouseTest_MakeSeamHouse(7, 0.0, 0.0, 0.0f, 600.0, 400.0);
	const CSHouseSeam::FHouse Apart = CSHouseTest_MakeSeamHouse(8, 620.0, 0.0, 0.0f, 600.0, 400.0);
	TArray<CSHouseSeam::FCorner> None;
	TestFalse(TEXT("two houses that merely stand close do not seam"), CSHouseSeam::Intersects(Far, Apart));
	TestEqual(TEXT("no overlap, no corners"), CSHouseSeam::BuildCorners(Far, Apart, None), 0);
	FCSWallCut NoCut;
	int32 CutsWhenApart = 0;
	for (int32 Edge = 0; Edge < 4; ++Edge) if (CSHouseSeam::CutOnEdge(Far, Apart, Edge, NoCut)) ++CutsWhenApart;
	TestEqual(TEXT("no overlap, no wall cut"), CutsWhenApart, 0);

	// 抬高到够不着：footprint 完全重合，但 Z 区间不相交 ⇒ 仍然不是接缝。
	CSHouseSeam::FHouse Above = CSHouseTest_MakeSeamHouse(9, 0.0, 0.0, 0.0f, 600.0, 400.0);
	Above.BaseZ = 400.0f;
	TestFalse(TEXT("a house floating above another one does not seam"), CSHouseSeam::Intersects(Far, Above));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamGeometryTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamCornersAndCuts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamGeometryTest::RunTest(const FString& Parameters)
{
	const CSHouseSeam::FHouse A = CSHouseTest_MakeSeamHouse(11, 0.0, 0.0, 0.0f, 600.0, 400.0);
	const CSHouseSeam::FHouse B = CSHouseTest_MakeSeamHouse(12, 200.0, 150.0, 0.0f, 500.0, 500.0);

	// ---- ① 交点确实同时落在两条轮廓上 ----
	TArray<CSHouseSeam::FCorner> Corners;
	const int32 N = CSHouseSeam::BuildCorners(A, B, Corners);
	TestTrue(TEXT("the pair crosses"), N > 0);
	int32 OffContour = 0, BadBisect = 0;
	for (const CSHouseSeam::FCorner& C : Corners)
	{
		const FVector2D LA = CSHouseSeam::ToLocal(A, C.Point);
		const FVector2D LB = CSHouseSeam::ToLocal(B, C.Point);
		const double AX = FMath::Abs(LA.X) - A.Footprint.GetBounds().Max.X, AY = FMath::Abs(LA.Y) - A.Footprint.GetBounds().Max.Y;
		const double BX = FMath::Abs(LB.X) - B.Footprint.GetBounds().Max.X, BY = FMath::Abs(LB.Y) - B.Footprint.GetBounds().Max.Y;
		// 落在轮廓上 = 至少一个方向恰好贴边，且两个方向都不在框外。
		if (AX > 0.01 || AY > 0.01 || BX > 0.01 || BY > 0.01) ++OffContour;
		if (FMath::Abs(AX) > 0.01 && FMath::Abs(AY) > 0.01) ++OffContour;
		if (FMath::Abs(BX) > 0.01 && FMath::Abs(BY) > 0.01) ++OffContour;
		if (!FMath::IsNearlyEqual(float(C.Outward.Size()), 1.0f, 1.0e-3f)) ++BadBisect;
	}
	TestEqual(TEXT("every seam corner lies on BOTH contours"), OffContour, 0);
	TestEqual(TEXT("every bisector is a unit vector (a zero one would mirror the whole column)"), BadBisect, 0);

	// 角平分朝"两栋房外面"那个象限：沿它走一小步必须同时离开两个 footprint。
	int32 PointingInwards = 0;
	for (const CSHouseSeam::FCorner& C : Corners)
	{
		const FVector2D Probe = C.Point + C.Outward * 20.0;
		const FVector2D PA = CSHouseSeam::ToLocal(A, Probe);
		const FVector2D PB = CSHouseSeam::ToLocal(B, Probe);
		const bool bOutA = FMath::Abs(PA.X) > A.Footprint.GetBounds().Max.X || FMath::Abs(PA.Y) > A.Footprint.GetBounds().Max.Y;
		const bool bOutB = FMath::Abs(PB.X) > B.Footprint.GetBounds().Max.X || FMath::Abs(PB.Y) > B.Footprint.GetBounds().Max.Y;
		if (!bOutA || !bOutB) ++PointingInwards;
	}
	TestEqual(TEXT("the brick depth axis faces the quadrant that is outside both houses"), PointingInwards, 0);

	// ---- ② 裁剪段：落在段内的墙点确实在邻居 footprint 里，段外的确实在外面 ----
	int32 Cuts = 0, MisclassifiedIn = 0, MisclassifiedOut = 0;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		FCSWallCut Cut;
		if (!CSHouseSeam::CutOnEdge(A, B, Edge, Cut)) continue;
		++Cuts;
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, A.Footprint, A.WallThickness);
		auto InsideB = [&](float S)
		{
			const FVector2D L = CSHouseSeam::ToLocal(B, CSHouseSeam::ToWorld(A, F.Start + F.U * S));
			return FMath::Abs(L.X) <= B.Footprint.GetBounds().Max.X + 0.01 && FMath::Abs(L.Y) <= B.Footprint.GetBounds().Max.Y + 0.01;
		};
		for (int32 K = 1; K < 20; ++K)
		{
			if (!InsideB(FMath::Lerp(Cut.MinS, Cut.MaxS, float(K) / 20.0f))) ++MisclassifiedIn;
		}
		// 段外两侧各探一步（探到墙外就不算）。
		if (Cut.MinS > 2.0f && InsideB(Cut.MinS - 2.0f)) ++MisclassifiedOut;
		if (Cut.MaxS < F.Len - 2.0f && InsideB(Cut.MaxS + 2.0f)) ++MisclassifiedOut;
	}
	TestTrue(FString::Printf(TEXT("the overlap cuts at least one wall (%d)"), Cuts), Cuts > 0);
	TestEqual(TEXT("every point inside the cut really is inside the neighbour"), MisclassifiedIn, 0);
	TestEqual(TEXT("and the cut stops where the neighbour stops"), MisclassifiedOut, 0);

	// ---- ③ 洞是 **clip** 出来的，几何仍然实心（裁决三，全局不变量）----
	//
	// 拿房体三角汤直接验：接缝那一段的三角形数**不许减少**（不生成面板就是一个真几何洞），
	// 而裁剪判据必须在那一段上说"丢掉"。两条一起才说得清"洞在渲染层、不在几何里"。
	FCSHouseBodyDesc Desc;
	Desc.Footprint = A.Footprint;
	Desc.WallThickness = A.WallThickness;
	Desc.WallHeight = A.WallHeight;
	Desc.PierWidth = 40.0f;

	FCSGpuMeshCPUData Solid;
	CSHouse_BuildBodySoup(Desc, Solid);

	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		FCSWallCut Cut;
		if (CSHouseSeam::CutOnEdge(A, B, Edge, Cut)) Desc.SeamCuts.Add(Cut);
	}
	TestTrue(TEXT("the seam produced cuts to feed the body"), Desc.SeamCuts.Num() > 0);

	FCSGpuMeshCPUData Cutaway;
	CSHouse_BuildBodySoup(Desc, Cutaway);
	// 面板被切成三块（实心 | 裁掉 | 实心）⇒ 三角形只会变多，绝不会变少。
	TestTrue(FString::Printf(TEXT("the seam never removes geometry: %d tris with cuts vs %d without"),
		Cutaway.Indices.Num() / 3, Solid.Indices.Num() / 3),
		Cutaway.Indices.Num() >= Solid.Indices.Num());

	// 裁剪场本身：段中点被 discard、段外被保留。判据用 `CSHouse_ClipKeeps`（材质那份的 CPU 孪生）。
	int32 KeptInsideCut = 0, DroppedOutsideCut = 0;
	for (const FCSWallCut& Cut : Desc.SeamCuts)
	{
		const FCSOpeningClipField Field = CSHouse_SeamClipField(Cut.MinS, Cut.MaxS, Cut.BottomZ, Cut.TopZ);
		const float MidS = (Cut.MinS + Cut.MaxS) * 0.5f;
		const float MidZ = FMath::Max((FMath::Max(Cut.BottomZ, 0.0f) + Cut.TopZ) * 0.5f, 1.0f);
		if (CSHouse_ClipKeeps(Field, Field.Eval(MidS, MidZ))) ++KeptInsideCut;
		// 段外一步（横向）必须留着 —— 否则接缝会把整面墙吃掉。
		if (!CSHouse_ClipKeeps(Field, Field.Eval(Cut.MinS - 20.0f, MidZ))) ++DroppedOutsideCut;
		if (!CSHouse_ClipKeeps(Field, Field.Eval(Cut.MaxS + 20.0f, MidZ))) ++DroppedOutsideCut;
	}
	TestEqual(TEXT("the wall inside the neighbour is clipped away"), KeptInsideCut, 0);
	TestEqual(TEXT("the wall outside the neighbour survives"), DroppedOutsideCut, 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamPolylineTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamPolylineFootprints",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamPolylineTest::RunTest(const FString& Parameters)
{
	// footprint 折线化 3d：接缝的分离轴、交点、墙段裁剪从「矩形的 2 根轴 / 4×4 / 四条轴对齐边界」
	// 换成「每条边的法线 / N×M / 每条边一个半平面」。判据与 SeamCornersAndCuts 同一套，换到六边形上。
	auto WithPolyline = [](uint32 Tag, double X, double Y, float Yaw, const TArray<FVector2D>& Verts)
	{
		CSHouseSeam::FHouse H = CSHouseTest_MakeSeamHouse(Tag, X, Y, Yaw, 1.0, 1.0);
		H.Footprint.Verts = Verts;
		return H;
	};
	auto Hexagon = [](double R)
	{
		TArray<FVector2D> V;
		for (int32 i = 0; i < 6; ++i)
		{
			const double A = UE_DOUBLE_TWO_PI * double(i) / 6.0;
			V.Add(FVector2D(R * FMath::Cos(A), R * FMath::Sin(A)));
		}
		return V;
	};
	// 世界点是否在某栋房的 footprint 里（含边界容差）。
	auto Inside = [](const CSHouseSeam::FHouse& H, const FVector2D& World, double Tol)
	{
		return H.Footprint.ContainsPoint(CSHouseSeam::ToLocal(H, World), Tol);
	};

	// ---- ① 包围盒相交、斜边之间隔着一条缝 ⇒ 不是接缝（只测两根轴的矩形 SAT 会误报）----
	{
		const CSHouseSeam::FHouse Lower = WithPolyline(21, 0.0, 0.0, 0.0f,
			{ FVector2D(0, 0), FVector2D(300, 0), FVector2D(0, 300) });
		const CSHouseSeam::FHouse Upper = WithPolyline(22, 0.0, 0.0, 0.0f,
			{ FVector2D(310, 310), FVector2D(10, 310), FVector2D(310, 10) });
		TestTrue(TEXT("triangle fixtures are counter-clockwise"),
			Lower.Footprint.GetSignedArea() > 0.0 && Upper.Footprint.GetSignedArea() > 0.0);
		TestFalse(TEXT("two triangles separated along their hypotenuses do not seam"), CSHouseSeam::Intersects(Lower, Upper));
		TestFalse(TEXT("and the test is symmetric"), CSHouseSeam::Intersects(Upper, Lower));
		TArray<CSHouseSeam::FCorner> None;
		TestEqual(TEXT("separated triangles: no corners"), CSHouseSeam::BuildCorners(Lower, Upper, None), 0);
	}

	// ---- ② 六边形 × 带 yaw 的矩形：交点在两条轮廓上、成对、交换顺序逐位相同；裁剪段分类正确 ----
	const CSHouseSeam::FHouse Hex = WithPolyline(23, 0.0, 0.0, 0.0f, Hexagon(300.0));
	const CSHouseSeam::FHouse Box = CSHouseTest_MakeSeamHouse(24, 330.0, 90.0, 23.0f, 360.0, 260.0);
	TestTrue(TEXT("hexagon and yawed box overlap"), CSHouseSeam::Intersects(Hex, Box));

	TArray<CSHouseSeam::FCorner> AB, BA;
	const int32 NAB = CSHouseSeam::BuildCorners(Hex, Box, AB);
	const int32 NBA = CSHouseSeam::BuildCorners(Box, Hex, BA);
	TestTrue(FString::Printf(TEXT("hexagon/box crossings come in pairs (got %d)"), NAB), NAB >= 2 && (NAB % 2) == 0);
	TestEqual(TEXT("swapping the pair keeps the corner count"), NBA, NAB);
	int32 NotBitEqual = 0;
	for (int32 K = 0; K < FMath::Min(NAB, NBA); ++K)
	{
		if (AB[K].Point.X != BA[K].Point.X || AB[K].Point.Y != BA[K].Point.Y
			|| AB[K].Outward.X != BA[K].Outward.X || AB[K].Outward.Y != BA[K].Outward.Y) ++NotBitEqual;
	}
	TestEqual(TEXT("both houses compute the same polyline seam bit for bit"), NotBitEqual, 0);

	int32 OffContour = 0, PointingInwards = 0;
	for (const CSHouseSeam::FCorner& C : AB)
	{
		// 在轮廓上 = 容差内算「在里面」，但往两侧各挪一点就分出里外 —— 用「容差内在、零容差邻域不全在」近似。
		if (!Inside(Hex, C.Point, 0.01) || !Inside(Box, C.Point, 0.01)) ++OffContour;
		const FVector2D Probe = C.Point + C.Outward * 20.0;
		if (Inside(Hex, Probe, 0.0) || Inside(Box, Probe, 0.0)) ++PointingInwards;
	}
	TestEqual(TEXT("every polyline seam corner lies on both contours"), OffContour, 0);
	TestEqual(TEXT("every polyline seam bisector faces outside both houses"), PointingInwards, 0);

	int32 Cuts = 0, MisclassifiedIn = 0, MisclassifiedOut = 0;
	for (int32 Edge = 0; Edge < Hex.Footprint.NumEdges(); ++Edge)
	{
		FCSWallCut Cut;
		if (!CSHouseSeam::CutOnEdge(Hex, Box, Edge, Cut)) continue;
		++Cuts;
		TestEqual(TEXT("the cut names the edge it was asked about"), Cut.EdgeIndex, Edge);
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Hex.Footprint, Hex.WallThickness);
		auto WallPoint = [&](float S) { return CSHouseSeam::ToWorld(Hex, F.Start + F.U * S); };
		for (int32 K = 1; K < 20; ++K)
		{
			if (!Inside(Box, WallPoint(FMath::Lerp(Cut.MinS, Cut.MaxS, float(K) / 20.0f)), 0.01)) ++MisclassifiedIn;
		}
		if (Cut.MinS > 2.0f && Inside(Box, WallPoint(Cut.MinS - 2.0f), 0.0)) ++MisclassifiedOut;
		if (Cut.MaxS < F.Len - 2.0f && Inside(Box, WallPoint(Cut.MaxS + 2.0f), 0.0)) ++MisclassifiedOut;
	}
	TestTrue(FString::Printf(TEXT("the box cuts at least one hexagon wall (%d)"), Cuts), Cuts > 0);
	TestEqual(TEXT("every point inside a hexagon cut really is inside the box"), MisclassifiedIn, 0);
	TestEqual(TEXT("and each hexagon cut stops where the box stops"), MisclassifiedOut, 0);

	// 反过来：矩形的墙被六边形盖住的段（Other 是六边形 ⇒ 走 6 个半平面）。
	int32 BoxCuts = 0, BoxMisIn = 0, BoxMisOut = 0;
	for (int32 Edge = 0; Edge < Box.Footprint.NumEdges(); ++Edge)
	{
		FCSWallCut Cut;
		if (!CSHouseSeam::CutOnEdge(Box, Hex, Edge, Cut)) continue;
		++BoxCuts;
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Box.Footprint, Box.WallThickness);
		auto WallPoint = [&](float S) { return CSHouseSeam::ToWorld(Box, F.Start + F.U * S); };
		for (int32 K = 1; K < 20; ++K)
		{
			if (!Inside(Hex, WallPoint(FMath::Lerp(Cut.MinS, Cut.MaxS, float(K) / 20.0f)), 0.01)) ++BoxMisIn;
		}
		if (Cut.MinS > 2.0f && Inside(Hex, WallPoint(Cut.MinS - 2.0f), 0.0)) ++BoxMisOut;
		if (Cut.MaxS < F.Len - 2.0f && Inside(Hex, WallPoint(Cut.MaxS + 2.0f), 0.0)) ++BoxMisOut;
	}
	TestTrue(FString::Printf(TEXT("the hexagon cuts at least one box wall (%d)"), BoxCuts), BoxCuts > 0);
	TestEqual(TEXT("every point inside a box cut really is inside the hexagon"), BoxMisIn, 0);
	TestEqual(TEXT("and each box cut stops where the hexagon stops"), BoxMisOut, 0);

	// ---- ③ 外接圆粗筛取最远顶点 ----
	TestTrue(TEXT("reach of a hexagon is its circumradius"), FMath::IsNearlyEqual(Hex.Reach(), 300.0f, 1.0e-3f));
	return true;
}

// =============================================================================
// 角石（D7 墙自身转角，合卷卷一 A7 / 卷五 A11）
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseQuoinCoversOuterEdgeTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinCoversOuterEdge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinCoversOuterEdgeTest::RunTest(const FString& Parameters)
{
	// ⚠️ **判据不是"不穿模"** —— 转角是精确斜接（3a-2 之前是直角对接），那条恒真、测了也永远绿。
	// 角石要盖的是外角那条竖直棱上的 UV 岛断裂，所以判据是：砖的横截面必须在**两个相邻墙面**
	// 的外法线方向上都伸出墙外表面。只伸出一个方向 = 只遮住半条棱，另一半照旧断纹。
	struct FCase { const TCHAR* What; FVector2D Footprint; float Yaw; FVector2D Center; float Inset; };
	const FCase Cases[] = {
		{ TEXT("axis-aligned"),     FVector2D(600.0, 400.0),    0.0f, FVector2D(0.0, 0.0),       0.0f },
		{ TEXT("yawed 37 deg"),     FVector2D(600.0, 400.0),   37.0f, FVector2D(1200.0, -800.0), 0.0f },
		{ TEXT("square + offset"),  FVector2D(500.0, 500.0), -113.0f, FVector2D(-300.0, 450.0),  0.0f },
		{ TEXT("slightly inset"),   FVector2D(800.0, 300.0),   90.0f, FVector2D(0.0, 0.0),       1.0f },
	};

	const float T = 24.0f;          // WallThickness
	const float Scale = 26.0f / 69.0f; // Shared shader profile, adapted to the current course height.
	const double Diag = 0.70710678118654752440;

	for (const FCase& C : Cases)
	{
		const FTransform World(FRotator(0.0f, C.Yaw, 0.0f), FVector(C.Center.X, C.Center.Y, 0.0));
		TArray<CSHouseQuoin::FQuoin> Quoins;
		const int32 Made = CSHouseQuoin::BuildQuoins(World, C.Footprint, T, 0.0f, 300.0f, C.Inset, Quoins);

		TestEqual(FString::Printf(TEXT("[%s] a rectangle yields exactly four quoins"), C.What), Made, 4);
		if (Made != 4) continue;

		const double HX = 0.5 * C.Footprint.X;
		const double HY = 0.5 * C.Footprint.Y;

		for (int32 Index = 0; Index < 4; ++Index)
		{
			const CSHouseQuoin::FQuoin& Q = Quoins[Index];
			const FVector2D Sign = CSHouseQuoin::CornerSign(Index);

			// ---- 柱心：Inset = 0 时正落在外角点上 ----
			const FVector Expect = World.TransformPosition(FVector(
				Sign.X * HX - Sign.X * Diag * C.Inset,
				Sign.Y * HY - Sign.Y * Diag * C.Inset, 0.0));
			TestTrue(FString::Printf(TEXT("[%s] quoin %d sits on its footprint corner"), C.What, Index),
				FVector2D(Expect.X, Expect.Y).Equals(Q.Point, 0.01));

			// ---- 朝外方向 = 角平分线（世界空间） ----
			const FVector Bisector = World.TransformVectorNoScale(FVector(Sign.X * Diag, Sign.Y * Diag, 0.0));
			TestTrue(FString::Printf(TEXT("[%s] quoin %d points along the corner bisector"), C.What, Index),
				FVector2D::DotProduct(Q.Outward, FVector2D(Bisector.X, Bisector.Y)) > 0.999);

			// Exercise the actual placement helper compiled by both C++ and HLSL.
			// Each course changes its long edge, but both outer wall planes stay fixed.
			for (bool Odd : { false, true })
			for (float Offset : { -16.0f * Scale, 0.0f, 16.0f * Scale })
			{
				const auto P = CSHouseQuoinLayout::PlaceQuoin(
					FVector3f(Q.Point.X, Q.Point.Y, 0.0f), Q.Outward.X, Q.Outward.Y, Q.HalfTurnCos, Scale, Offset, Odd);
				TestTrue(TEXT("Quoin basis remains right-handed on both course orientations"),
					FVector3f::DotProduct(FVector3f::CrossProduct(P.AxisX, FVector3f(0, 0, -1)), P.AxisZ) > 0.999f);
				TestTrue(TEXT("The long edge stays longer than the short edge at either random extreme"), P.LongSize > P.ShortSize);
				for (int32 Which = 0; Which < 2; ++Which)
				{
					const FVector LocalN = Which == 0 ? FVector(Sign.X, 0, 0) : FVector(0, Sign.Y, 0);
					const FVector3f N(World.TransformVectorNoScale(LocalN));
					const FVector3f FacePoint(World.TransformPosition(FVector(Sign.X * HX, Sign.Y * HY, 0)));
					const float XDot = FVector3f::DotProduct(P.AxisX, N);
					const float ZDot = FVector3f::DotProduct(P.AxisZ, N);
					TestTrue(TEXT("Brick faces align with a wall, never the diagonal bisector"),
						FMath::Abs(XDot) > 0.999f || FMath::Abs(ZDot) > 0.999f);
					const float Outmost = FVector3f::DotProduct(P.Center - FacePoint, N)
						+ 0.5f * (P.LongSize * FMath::Abs(XDot) + P.ShortSize * FMath::Abs(ZDot));
					const float Expected = 3.85f * Scale - C.Inset * float(Diag);
					TestTrue(FString::Printf(TEXT("[%s] corner %d face %d stays shallow and anchored (%.3f cm)"),
						C.What, Index, Which, Outmost), FMath::IsNearlyEqual(Outmost, Expected, 0.002f) && Outmost > 0.5f);
				}
			}
			const auto Even = CSHouseQuoinLayout::PlaceQuoin(FVector3f::ZeroVector, Q.Outward.X, Q.Outward.Y, Q.HalfTurnCos, Scale, 0, false);
			const auto Odd = CSHouseQuoinLayout::PlaceQuoin(FVector3f::ZeroVector, Q.Outward.X, Q.Outward.Y, Q.HalfTurnCos, Scale, 0, true);
			TestTrue(TEXT("Successive courses exchange the long edge between adjacent walls"),
				FMath::Abs(FVector3f::DotProduct(Even.AxisX, Odd.AxisX)) < 0.001f);
		}
	}

	// ---- 退化 footprint 不出柱：任一边窄于两个墙厚时四角互相吃掉 ----
	{
		TArray<CSHouseQuoin::FQuoin> None;
		TestEqual(TEXT("a footprint thinner than two wall thicknesses yields no quoin"),
			CSHouseQuoin::BuildQuoins(FTransform::Identity, FVector2D(600.0, 40.0), 24.0f, 0.0f, 300.0f, 0.0f, None), 0);
		TestEqual(TEXT("a zero-height wall yields no quoin"),
			CSHouseQuoin::BuildQuoins(FTransform::Identity, FVector2D(600.0, 400.0), 24.0f, 0.0f, 0.0f, 0.0f, None), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseQuoinPolylineCornersTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinPolylineCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinPolylineCornersTest::RunTest(const FString& Parameters)
{
	// footprint 折线化 3c：角石从「矩形四角 + shader 写死的 45°」换成「折线的每个合格凸角 + 逐角的半转角」。
	// 判据与 QuoinCoversOuterEdge 同一条 —— 外角那条竖直棱被砖盖住、两面相邻墙都只伸出 3.85 × Scale ——
	// 换到非直角上再钉一遍；外加：凹角 / 锐角不出、CornerIndex 跟着角号走、墩心在两面墙的中线交点上。
	const float T = 24.0f;
	const float Scale = 26.0f / 69.0f;
	const float Protrude = 3.85f * Scale;

	auto RegularPolygon = [](int32 N, double Radius)
	{
		FCSHouseFootprint FP;
		for (int32 i = 0; i < N; ++i)
		{
			const double A = UE_DOUBLE_TWO_PI * double(i) / double(N);
			FP.Verts.Add(FVector2D(Radius * FMath::Cos(A), Radius * FMath::Sin(A)));
		}
		return FP;
	};

	// 每个角：两条相邻墙的外法线（远边 = k 号边、近边 = k+1 号边）。
	auto WallNormals = [&](const FCSHouseFootprint& FP, int32 Corner, FVector3f& OutFar, FVector3f& OutNear)
	{
		const FCSHouseEdgeFrame Far = CSHouse_GetEdge(Corner, FP, T);
		const FCSHouseEdgeFrame Near = CSHouse_GetEdge((Corner + 1) % FP.NumEdges(), FP, T);
		OutFar = FVector3f(float(-Far.In.X), float(-Far.In.Y), 0.0f);
		OutNear = FVector3f(float(-Near.In.X), float(-Near.In.Y), 0.0f);
	};

	// 砖盒底面四个角沿法线 N 最远伸到哪（相对角点）。
	auto Outmost = [](const CSHouseQuoinLayout::FQuoinPlacement& P, const FVector3f& Corner, const FVector3f& N)
	{
		float Best = -FLT_MAX;
		for (float SX : { -0.5f, 0.5f })
		for (float SZ : { -0.5f, 0.5f })
		{
			const FVector3f V = P.Center + P.AxisX * (SX * P.LongSize) + P.AxisZ * (SZ * P.ShortSize);
			Best = FMath::Max(Best, FVector3f::DotProduct(V - Corner, N));
		}
		return Best;
	};

	// ---- ① 正五 / 六 / 八边形：每个角都出，半转角 = π/N，砖盖住外棱且两面只伸出 Protrude ----
	for (int32 N : { 5, 6, 8 })
	{
		const FCSHouseFootprint FP = RegularPolygon(N, 300.0);
		TestTrue(FString::Printf(TEXT("[%d-gon] fixture is strictly convex CCW"), N), FP.IsStrictlyConvexCCW());
		TArray<CSHouseQuoin::FQuoin> Quoins;
		const int32 Made = CSHouseQuoin::BuildQuoins(FTransform::Identity, FP, T, 0.0f, 300.0f, 0.0f, Quoins);
		TestEqual(FString::Printf(TEXT("[%d-gon] every corner gets a quoin"), N), Made, N);
		if (Made != N) continue;

		const float ExpectCos = float(FMath::Cos(UE_DOUBLE_PI / N));
		for (int32 i = 0; i < N; ++i)
		{
			const CSHouseQuoin::FQuoin& Q = Quoins[i];
			const FVector2D V = FP.Verts[(i + 1) % N];
			TestEqual(FString::Printf(TEXT("[%d-gon] quoin %d carries its corner index"), N, i), Q.CornerIndex, i);
			TestTrue(FString::Printf(TEXT("[%d-gon] quoin %d sits on its vertex"), N, i), Q.Point.Equals(V, 1.0e-3));
			TestTrue(FString::Printf(TEXT("[%d-gon] quoin %d points radially out"), N, i),
				FVector2D::DotProduct(Q.Outward, V.GetSafeNormal()) > 0.99999);
			TestTrue(FString::Printf(TEXT("[%d-gon] quoin %d half-turn cosine is cos(pi/N) (%.6f)"), N, i, Q.HalfTurnCos),
				FMath::IsNearlyEqual(Q.HalfTurnCos, ExpectCos, 1.0e-5f));

			FVector3f NFar, NNear;
			WallNormals(FP, i, NFar, NNear);
			const FVector3f Corner(float(Q.Point.X), float(Q.Point.Y), 0.0f);
			for (bool Odd : { false, true })
			for (float Offset : { -16.0f * Scale, 0.0f, 16.0f * Scale })
			{
				const auto P = CSHouseQuoinLayout::PlaceQuoin(Corner, Q.Outward.X, Q.Outward.Y, Q.HalfTurnCos, Scale, Offset, Odd);
				TestTrue(TEXT("Polyline quoin basis stays right-handed"),
					FVector3f::DotProduct(FVector3f::CrossProduct(P.AxisX, FVector3f(0, 0, -1)), P.AxisZ) > 0.999f);
				TestTrue(TEXT("Polyline quoin basis stays orthogonal (a box, not a sheared prism)"),
					FMath::Abs(FVector3f::DotProduct(P.AxisX, P.AxisZ)) < 1.0e-4f);
				// 偶数层方正地贴远边、奇数层贴近边。
				const FVector3f& Aligned = Odd ? NNear : NFar;
				TestTrue(FString::Printf(TEXT("[%d-gon] corner %d course %d is squared to one wall"), N, i, int32(Odd)),
					FMath::Abs(FVector3f::DotProduct(P.AxisZ, Aligned)) > 0.9999f);
				// 两面墙都被砖盖过、且都只伸出 Protrude（转角 ≤ 90° 时最远点恰在外棱锚点上）。
				for (const FVector3f& Wall : { NFar, NNear })
				{
					const float Out = Outmost(P, Corner, Wall);
					TestTrue(FString::Printf(TEXT("[%d-gon] corner %d course %d stays shallow on both walls (%.4f cm)"), N, i, int32(Odd), Out),
						FMath::IsNearlyEqual(Out, Protrude, 0.002f));
				}
				// 外棱（角点那条竖线）落在砖的横截面里。
				const FVector3f D = Corner - P.Center;
				TestTrue(FString::Printf(TEXT("[%d-gon] corner %d course %d covers the outer edge"), N, i, int32(Odd)),
					FMath::Abs(FVector3f::DotProduct(D, P.AxisX)) <= 0.5f * P.LongSize + 1.0e-3f
					&& FMath::Abs(FVector3f::DotProduct(D, P.AxisZ)) <= 0.5f * P.ShortSize + 1.0e-3f);
			}
		}

		// 半转角一路送进 GPU 元素。
		CSHouseFrame::FBrickParams Params;
		Params.Length = 26.0f;
		Params.MaxBricks = 4096;
		TArray<CSHouseFrame::FElement> Elements;
		CSHouseQuoin::BuildQuoinElements(Quoins, 7u, Params, Elements, FVector2f(0.01f, 0.01f));
		TestEqual(FString::Printf(TEXT("[%d-gon] one column path per quoin"), N), Elements.Num(), N);
		for (int32 i = 0; i < Elements.Num() && i < N; ++i)
		{
			TestEqual(FString::Printf(TEXT("[%d-gon] element %d carries the half-turn cosine"), N, i),
				Elements[i].QuoinHalfTurnCos, Quoins[i].HalfTurnCos);
		}
	}

	// ---- ② 直角：半转角 1/√2，且 shader 的「≤ 0 按直角」退路与显式传 1/√2 等价 ----
	{
		TArray<CSHouseQuoin::FQuoin> Quoins;
		CSHouseQuoin::BuildQuoins(FTransform::Identity, FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)), T, 0.0f, 300.0f, 0.0f, Quoins);
		TestEqual(TEXT("rect polyline yields four quoins"), Quoins.Num(), 4);
		for (int32 i = 0; i < Quoins.Num(); ++i)
		{
			const CSHouseQuoin::FQuoin& Q = Quoins[i];
			TestEqual(FString::Printf(TEXT("rect quoin %d carries its corner index"), i), Q.CornerIndex, i);
			TestTrue(FString::Printf(TEXT("rect quoin %d half-turn cosine is 1/sqrt(2)"), i),
				FMath::IsNearlyEqual(Q.HalfTurnCos, UE_INV_SQRT_2, 1.0e-6f));
			for (bool Odd : { false, true })
			{
				const auto Explicit = CSHouseQuoinLayout::PlaceQuoin(FVector3f::ZeroVector, Q.Outward.X, Q.Outward.Y, UE_INV_SQRT_2, Scale, 0.0f, Odd);
				const auto Legacy = CSHouseQuoinLayout::PlaceQuoin(FVector3f::ZeroVector, Q.Outward.X, Q.Outward.Y, 0.0f, Scale, 0.0f, Odd);
				TestTrue(TEXT("HalfTurnCos <= 0 falls back to a right angle"),
					Explicit.Center.Equals(Legacy.Center, 1.0e-4f) && Explicit.AxisX.Equals(Legacy.AxisX, 1.0e-5f)
					&& Explicit.AxisZ.Equals(Legacy.AxisZ, 1.0e-5f));
			}
		}
	}

	// ---- ③ L 形：凹角不出角石，角号跳过它；墩心在两面墙的中线交点上（凸角凹角同一个式子） ----
	{
		FCSHouseFootprint L;
		for (const FVector2D& V : { FVector2D(0, 0), FVector2D(600, 0), FVector2D(600, 300),
			FVector2D(300, 300), FVector2D(300, 600), FVector2D(0, 600) })
		{
			L.Verts.Add(V);
		}
		TestTrue(TEXT("L fixture is counter-clockwise"), L.GetSignedArea() > 0.0);
		TArray<CSHouseQuoin::FQuoin> Quoins;
		const int32 Made = CSHouseQuoin::BuildQuoins(FTransform::Identity, L, T, 0.0f, 300.0f, 0.0f, Quoins);
		TestEqual(TEXT("L: five convex corners, the reflex one is skipped"), Made, 5);
		TArray<int32> Indices;
		for (const CSHouseQuoin::FQuoin& Q : Quoins) Indices.Add(Q.CornerIndex);
		TestTrue(TEXT("L: corner indices skip the reflex corner"), Indices == TArray<int32>({ 0, 1, 3, 4, 5 }));

		const FCSHouseCornerFrame Reflex = CSHouse_GetCorner(2, L);
		TestTrue(TEXT("L: corner 2 is reflex"), Reflex.SinTurn < 0.0 && !Reflex.IsConvex());
		for (int32 Corner = 0; Corner < L.NumEdges(); ++Corner)
		{
			const FVector2D Mid = CSHouse_GetCorner(Corner, L).PointAtDepth(T * 0.5);
			const FCSHouseEdgeFrame Far = CSHouse_GetEdge(Corner, L, T);
			const FCSHouseEdgeFrame Near = CSHouse_GetEdge((Corner + 1) % L.NumEdges(), L, T);
			const double DFar = FVector2D::DotProduct(Mid - Far.Start, Far.In);
			const double DNear = FVector2D::DotProduct(Mid - Near.Start, Near.In);
			TestTrue(FString::Printf(TEXT("L: corner %d mid point is half a wall inside both walls (%.4f, %.4f)"), Corner, DFar, DNear),
				FMath::IsNearlyEqual(DFar, 12.0, 1.0e-6) && FMath::IsNearlyEqual(DNear, 12.0, 1.0e-6));
		}
		// 直角上的中线交点就是老写法「沿平分线内缩 T/√2」。
		const FCSHouseCornerFrame Right = CSHouse_GetCorner(0, L);
		TestTrue(TEXT("L: right-angle mid point equals the legacy T/sqrt(2) inset"),
			Right.PointAtDepth(T * 0.5).Equals(Right.Point - Right.Outward * (T * 0.70710678118654752440), 1.0e-6));
	}

	// ---- ④ 锐角不出：直角三角形只在直角上出一根 ----
	{
		FCSHouseFootprint Tri;
		Tri.Verts = { FVector2D(0, 0), FVector2D(400, 0), FVector2D(0, 400) };
		TArray<CSHouseQuoin::FQuoin> Quoins;
		const int32 Made = CSHouseQuoin::BuildQuoins(FTransform::Identity, Tri, T, 0.0f, 300.0f, 0.0f, Quoins);
		TestEqual(TEXT("right triangle: only the right angle gets a quoin"), Made, 1);
		if (Made == 1)
		{
			TestEqual(TEXT("right triangle: the quoin sits on corner 2 (the origin)"), Quoins[0].CornerIndex, 2);
			TestTrue(TEXT("right triangle: at the origin"), Quoins[0].Point.Equals(FVector2D::ZeroVector, 1.0e-6));
		}
	}

	// ---- ⑤ 退化：边长放不下两头的斜接让出量时一根都不出 ----
	{
		TArray<CSHouseQuoin::FQuoin> None;
		// 正六边形边长 = 半径；两头各让出 T·tan(30°) ≈ 13.86，边长 20 < 27.7。
		TestEqual(TEXT("a hexagon whose sides cannot hold both mitres yields no quoin"),
			CSHouseQuoin::BuildQuoins(FTransform::Identity, RegularPolygon(6, 20.0), T, 0.0f, 300.0f, 0.0f, None), 0);
		FCSHouseFootprint Two;
		Two.Verts = { FVector2D(0, 0), FVector2D(100, 0) };
		TestEqual(TEXT("an invalid footprint yields no quoin"),
			CSHouseQuoin::BuildQuoins(FTransform::Identity, Two, T, 0.0f, 300.0f, 0.0f, None), 0);
		TestEqual(TEXT("an out-of-range corner is not convex"), CSHouse_GetCorner(7, Two).IsConvex(), false);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseQuoinSharesColumnEmitterTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinSharesTheColumnEmitter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinSharesColumnEmitterTest::RunTest(const FString& Parameters)
{
	// 合卷卷五 A11 的可执行版本：角石与接缝柱**必须**走同一个 `CSHouseFrame::AppendColumn`。
	// 谁将来复制一份出去（哪怕只差一个 0.5 的下限），这条就会红。
	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	const FVector2D Point(1234.5, -678.25);
	const FVector2D Outward = FVector2D(0.6, -0.8);   // 已归一
	const float BottomZ = 137.0f;
	const float TopZ = 137.0f + 293.5f;

	TArray<CSHouseSeam::FCorner> Corners;
	CSHouseSeam::FCorner Corner;
	Corner.Point = Point; Corner.Outward = Outward; Corner.BottomZ = BottomZ; Corner.TopZ = TopZ;
	Corners.Add(Corner);

	TArray<CSHouseQuoin::FQuoin> Quoins;
	CSHouseQuoin::FQuoin Quoin;
	Quoin.Point = Point; Quoin.Outward = Outward; Quoin.BottomZ = BottomZ; Quoin.TopZ = TopZ;
	Quoins.Add(Quoin);

	TArray<CSHouseFrame::FElement> FromSeam, FromQuoin;
	const int32 SeamBricks = CSHouseSeam::BuildCornerElements(Corners, 0x1234u, Params, FromSeam);
	const int32 QuoinBricks = CSHouseQuoin::BuildQuoinElements(Quoins, 0x1234u, Params, FromQuoin, FVector2f(0.01f, 0.01f));

	TestEqual(TEXT("both emit the same brick count"), QuoinBricks, SeamBricks);
	TestTrue(TEXT("both emit exactly one path"), FromSeam.Num() == 1 && FromQuoin.Num() == 1);
	if (FromSeam.Num() != 1 || FromQuoin.Num() != 1) return false;

	const CSHouseFrame::FElement& A = FromSeam[0];
	const CSHouseFrame::FElement& B = FromQuoin[0];
	TestEqual(TEXT("same brick count"), B.BrickCount, A.BrickCount);
	TestEqual(TEXT("same slot start"), B.BrickBegin, A.BrickBegin);
	TestEqual(TEXT("same pitch"), B.Pitch, A.Pitch);
	TestEqual(TEXT("same half length"), B.HalfLen, A.HalfLen);
	TestEqual(TEXT("same layout scale"), B.LayoutScale, A.LayoutScale);
	TestEqual(TEXT("same path top"), B.Path.TopZ, A.Path.TopZ);
	TestTrue(TEXT("same origin"), B.Frame.Origin.Equals(A.Frame.Origin, 1e-4f));
	TestTrue(TEXT("same U axis"), B.Frame.AxisU.Equals(A.Frame.AxisU, 1e-6f));
	TestTrue(TEXT("same V axis"), B.Frame.AxisV.Equals(A.Frame.AxisV, 1e-6f));

	// ---- 进深轴朝外，不朝里 ----
	// kernel：路径竖直 ⇒ 切向 (0,1) ⇒ OutwardSZ = (−1,0) ⇒ AxisX = −AxisU = 砖的进深轴。
	// 写成 `AxisU = Outward` 的症状是砖整根插进房间，而**位置断言一条都不会红** —— 所以必须单测。
	const FVector2D DepthAxis(-B.Frame.AxisU.X, -B.Frame.AxisU.Y);
	const double Dot = FVector2D::DotProduct(DepthAxis, Outward);
	TestTrue(FString::Printf(TEXT("the depth axis points outward (dot = %.4f)"), Dot), Dot > 0.999);

	// ---- 随机数基必须不同：两者是不同的东西，共用发射器不等于共用身份 ----
	TestNotEqual(TEXT("seam and quoin derive different per-instance randoms from the same seed"),
		B.RandomBase, A.RandomBase);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseQuoinRandomIgnoresSlotTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinRandomIgnoresSlot",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinRandomIgnoresSlotTest::RunTest(const FString& Parameters)
{
	// 角石排在门框砖与接缝砖之后 ⇒ 每开一扇门它的槽位就整体后移。逐实例随机数**不许**跟着变，
	// 否则将来谁给砖材质接上 PerInstanceRandom 色差，开一扇门就会让四个角整体换色，
	// 而砖数 / 位置 / 三角形数所有几何断言全绿。
	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	TArray<CSHouseQuoin::FQuoin> Quoins;
	CSHouseQuoin::BuildQuoins(FTransform::Identity, FVector2D(600.0, 400.0), 24.0f, 0.0f, 300.0f, 0.0f, Quoins);
	TestEqual(TEXT("fixture yields four quoins"), Quoins.Num(), 4);

	auto RandomsWithPrefix = [&](int32 PrefixBricks, TArray<uint32>& OutRandoms, TArray<int32>& OutSlots)
	{
		TArray<CSHouseFrame::FElement> Elements;
		const int32 Skip = PrefixBricks > 0 ? 1 : 0;
		if (PrefixBricks > 0)
		{
			CSHouseFrame::FElement Filler;      // 假装前面已经排了这么多门框砖
			Filler.BrickBegin = 0;
			Filler.BrickCount = PrefixBricks;
			Elements.Add(Filler);
		}
		CSHouseQuoin::BuildQuoinElements(Quoins, 0xABCDu, Params, Elements, FVector2f(0.01f, 0.01f));
		for (int32 i = Skip; i < Elements.Num(); ++i)
		{
			OutRandoms.Add(Elements[i].RandomBase);
			OutSlots.Add(Elements[i].BrickBegin);
		}
	};

	TArray<uint32> R0, R1;
	TArray<int32> S0, S1;
	RandomsWithPrefix(0, R0, S0);
	RandomsWithPrefix(37, R1, S1);

	TestEqual(TEXT("the same four columns come out either way"), R1.Num(), R0.Num());
	if (R1.Num() != R0.Num()) return false;

	bool bSlotsMoved = false;
	for (int32 i = 0; i < R0.Num(); ++i)
	{
		TestEqual(FString::Printf(TEXT("quoin %d keeps its per-instance random when the slots shift"), i), R1[i], R0[i]);
		bSlotsMoved |= (S1[i] != S0[i]);
	}
	// 夹具自证：槽位**确实**动了，否则上面那条是空判据。
	TestTrue(TEXT("the fixture really does shift the slots"), bSlotsMoved);

	// 四个角互不相同 —— 否则一栋房四个角会长得一模一样。
	TSet<uint32> Distinct(R0);
	TestEqual(TEXT("the four corners get four different randoms"), Distinct.Num(), R0.Num());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseQuoinTruncatesTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinTruncatesNeverGrows",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinTruncatesTest::RunTest(const FString& Parameters)
{
	// 容量是注册期一次付清的常量。撞上就截断、绝不扩容 —— 扩容是阻塞刷新，落在用户恰好
	// 画到的那一笔上（零阻塞纪律）。
	TArray<CSHouseQuoin::FQuoin> Quoins;
	CSHouseQuoin::BuildQuoins(FTransform::Identity, FVector2D(600.0, 400.0), 24.0f, 0.0f, 300.0f, 0.0f, Quoins);

	const int32 Caps[] = { 0, 1, 7, 12, 23, 512 };
	for (int32 Cap : Caps)
	{
		CSHouseFrame::FBrickParams Params;
		Params.Length = 26.0f;
		Params.Gap = 0.0f;
		Params.MaxBricks = Cap;

		TArray<CSHouseFrame::FElement> Elements;
		const int32 Added = CSHouseQuoin::BuildQuoinElements(Quoins, 1u, Params, Elements, FVector2f(0.01f, 0.01f));

		TestTrue(FString::Printf(TEXT("cap %d is never exceeded (added %d)"), Cap, Added), Added <= Cap);
		TestTrue(FString::Printf(TEXT("cap %d never yields a negative count"), Cap), Added >= 0);

		int32 Sum = 0;
		for (const CSHouseFrame::FElement& E : Elements)
		{
			TestTrue(FString::Printf(TEXT("cap %d: every path has a positive brick count"), Cap), E.BrickCount > 0);
			TestEqual(FString::Printf(TEXT("cap %d: slots stay contiguous"), Cap), E.BrickBegin, Sum);
			Sum += E.BrickCount;
		}
		TestEqual(FString::Printf(TEXT("cap %d: the returned count matches the slots"), Cap), Sum, Added);
	}
	return true;
}

// =============================================================================
// 包边石（D7 第三样，合卷卷一 A8 / 卷五 A11）
// =============================================================================

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTrimTilesPerimeterTest,
	"PCGPlugins.ComputeShaderGenerator.House.TrimTilesThePerimeter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTrimTilesPerimeterTest::RunTest(const FString& Parameters)
{
	// 无洞时四条边各出一段，合起来正好铺满中线周界。砖路走墙厚正中，所以每段取**中线处的
	// 斜接区间** `[T/2, L - T/2]`：两条边的砖在角平分线上相遇（2026-09-13 起；之前是 0/2 号墙
	// 跑满、1/3 号墙两端各缩 T 的直角对接，两种分法的总长恰好相等，都是 2X + 2Y - 4T）。
	const FVector2D Footprint(600.0, 400.0);
	const float T = 24.0f;

	CSHouseTrim::FBand Band;
	Band.CenterZ = 300.0f;
	Band.HalfHeight = 10.0f;

	TArray<CSHouseTrim::FRun> Runs;
	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	TArray<CSHouseFrame::FElement> Elements;
	const int32 Bricks = CSHouseTrim::BuildBand(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Band, 6.0f,
		TArrayView<const FCSWallOpening>(), 0x99u, CSHouseFrame::EPathFamily::TrimTop, Params, Runs, Elements);

	TestEqual(TEXT("a hole-free rectangle yields one run per edge"), Runs.Num(), 4);
	TestTrue(TEXT("the band actually produces bricks"), Bricks > 0);

	double Total = 0.0;
	TSet<int32> Edges;
	for (const CSHouseTrim::FRun& R : Runs)
	{
		Total += R.Span();
		Edges.Add(R.EdgeIndex);
		TestTrue(FString::Printf(TEXT("run on edge %d starts at the mid-thickness mitre point (%.3f)"), R.EdgeIndex, R.S0),
			FMath::IsNearlyEqual(R.S0, T * 0.5f, 0.01f));
	}
	TestEqual(TEXT("all four edges are covered"), Edges.Num(), 4);
	// 每条边 L - T（两端各让半个墙厚）=> 2(X - T) + 2(Y - T)。
	const double Expect = 2.0 * (Footprint.X - T) + 2.0 * (Footprint.Y - T);
	TestTrue(FString::Printf(TEXT("the runs tile the perimeter (%.1f vs %.1f)"), Total, Expect),
		FMath::IsNearlyEqual(Total, Expect, 0.01));

	// 每条路都是平顶段、且高度就是带高 —— 写错成竖直段的话砖会整排立起来，而砖数照样对。
	for (const CSHouseFrame::FElement& E : Elements)
	{
		TestTrue(TEXT("every trim path is a flat run"), E.Path.MidKind == CSHouseFrame::EMidKind::Flat);
		TestFalse(TEXT("a trim path has no jambs"), E.Path.bLeftJamb || E.Path.bRightJamb);
		TestEqual(TEXT("the path sits at the band height"), E.Path.TopZ, Band.CenterZ);
		TestTrue(TEXT("the path length is the run span"), E.Path.TotalLen() > 0.0f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTrimAvoidsOpeningsTest,
	"PCGPlugins.ComputeShaderGenerator.House.TrimAvoidsOpenings",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTrimAvoidsOpeningsTest::RunTest(const FString& Parameters)
{
	// ⚠️ 这条是本模块存在的唯一理由：不切洞的话勒脚石会从门口正中一路铺过去，
	// 而砖数 / 零阻塞 / 三角形数**所有断言都不会红**，只有出图看得见。
	const FVector2D Footprint(600.0, 400.0);
	const float T = 24.0f;
	const float WallHeight = 300.0f;

	// 0 号墙上两扇落地门 + 一扇高窗。
	TArray<FCSWallOpening> Openings;
	{
		FCSWallOpening Door;
		Door.EdgeIndex = 0; Door.Z0 = 0.0f; Door.Z1 = 200.0f; Door.Width = 120.0f;
		Door.CenterS = 150.0f; Openings.Add(Door);
		Door.CenterS = 420.0f; Openings.Add(Door);

		FCSWallOpening Window;
		Window.EdgeIndex = 0; Window.Z0 = 160.0f; Window.Z1 = 240.0f; Window.Width = 90.0f;
		Window.CenterS = 290.0f; Openings.Add(Window);
	}
	Openings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B)
	{
		return A.EdgeIndex != B.EdgeIndex ? A.EdgeIndex < B.EdgeIndex : A.CenterS < B.CenterS;
	});

	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 512;

	auto BuildBandRuns = [&](float CenterZ, uint32 Salt, TArray<CSHouseTrim::FRun>& OutRuns)
	{
		CSHouseTrim::FBand Band;
		Band.CenterZ = CenterZ;
		Band.HalfHeight = 10.0f;
		TArray<CSHouseFrame::FElement> Elements;
		CSHouseTrim::BuildBand(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Band, 6.0f,
			MakeArrayView(Openings), 0x77u, Salt, Params, OutRuns, Elements);
		return Band;
	};

	// ---- 墙脚带：门挡路（Z0 = 0），高窗不挡 ⇒ 0 号墙被切成 3 段，其余三边各 1 段 ----
	TArray<CSHouseTrim::FRun> BaseRuns;
	const CSHouseTrim::FBand BaseBand = BuildBandRuns(0.0f, CSHouseFrame::EPathFamily::TrimBase, BaseRuns);

	int32 OnEdge0 = 0;
	for (const CSHouseTrim::FRun& R : BaseRuns) if (R.EdgeIndex == 0) ++OnEdge0;
	TestEqual(TEXT("two ground-level doors cut the base band on edge 0 into three runs"), OnEdge0, 3);
	TestEqual(TEXT("the other three edges stay whole"), BaseRuns.Num() - OnEdge0, 3);

	// 核心：没有任何一段与挡路的洞重叠。
	for (const CSHouseTrim::FRun& R : BaseRuns)
	{
		for (const FCSWallOpening& O : Openings)
		{
			if (O.EdgeIndex != R.EdgeIndex) continue;
			if (!CSHouseTrim::BlocksBand(O, BaseBand)) continue;
			const bool bOverlap = R.S1 > O.S0() && R.S0 < O.S1();
			TestFalse(FString::Printf(
				TEXT("base run [%.1f, %.1f] does not cross the opening at %.1f"), R.S0, R.S1, O.CenterS), bOverlap);
		}
	}

	// ---- 墙顶带：三个洞都够不着 ⇒ 四条边各 1 段，与无洞时一样 ----
	TArray<CSHouseTrim::FRun> TopRuns;
	BuildBandRuns(WallHeight, CSHouseFrame::EPathFamily::TrimTop, TopRuns);
	TestEqual(TEXT("nothing reaches the wall top, so the top band stays whole"), TopRuns.Num(), 4);

	// ---- 判据带 Z，不是只看 S：把窗抬到墙顶，顶带就该被切开 ----
	{
		TArray<FCSWallOpening> HighOnly;
		FCSWallOpening Tall;
		Tall.EdgeIndex = 0; Tall.Z0 = 280.0f; Tall.Z1 = 340.0f; Tall.Width = 90.0f; Tall.CenterS = 290.0f;
		HighOnly.Add(Tall);

		CSHouseTrim::FBand Band;
		Band.CenterZ = WallHeight;
		Band.HalfHeight = 10.0f;
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		CSHouseTrim::BuildBand(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Band, 6.0f,
			MakeArrayView(HighOnly), 0x77u, CSHouseFrame::EPathFamily::TrimTop, Params, Runs, Elements);

		int32 Cut = 0;
		for (const CSHouseTrim::FRun& R : Runs) if (R.EdgeIndex == 0) ++Cut;
		TestEqual(TEXT("an opening that does reach the top cuts the top band"), Cut, 2);
	}

	// ---- 重叠的洞不该产生负长度或错序的段 ----
	{
		TArray<FCSWallOpening> Overlap;
		FCSWallOpening A;
		A.EdgeIndex = 0; A.Z0 = 0.0f; A.Z1 = 200.0f; A.Width = 200.0f; A.CenterS = 200.0f; Overlap.Add(A);
		A.CenterS = 260.0f; Overlap.Add(A);   // 与上一个重叠

		CSHouseTrim::FBand Band;
		Band.CenterZ = 0.0f;
		Band.HalfHeight = 10.0f;
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		CSHouseTrim::BuildBand(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Band, 6.0f,
			MakeArrayView(Overlap), 0x77u, CSHouseFrame::EPathFamily::TrimBase, Params, Runs, Elements);

		for (const CSHouseTrim::FRun& R : Runs)
		{
			TestTrue(FString::Printf(TEXT("run [%.1f, %.1f] has a positive span"), R.S0, R.S1), R.Span() > 0.0f);
		}
		// 同一条边上的段必须严格递增且互不相交。
		float Prev = -1.0f;
		for (const CSHouseTrim::FRun& R : Runs)
		{
			if (R.EdgeIndex != 0) continue;
			TestTrue(TEXT("runs on one edge are ordered and disjoint"), R.S0 >= Prev);
			Prev = R.S1;
		}
	}
	return true;
}


// -----------------------------------------------------------------------------
// D8 特征标记：宿主解析（纯函数）+ 登记 / 裁决 / 换宿主 / 注销（要 world）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWallPickTest,
	"PCGPlugins.ComputeShaderGenerator.House.WallPick",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseWallPickTest::RunTest(const FString& Parameters)
{
	const FVector2D Footprint(600.0, 400.0);
	const float T = 24.0f;
	const float WallHeight = 300.0f;

	// 边 0 的外表面是 y = -HY = -200，外法线 (0,-1)。从外面朝 +Y 打过去必须命中它。
	{
		const FCSWallHit Hit = CSHouse_RayHitWall(
			FVector(0.0, -400.0, 150.0), FVector(0.0, 1.0, 0.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 1000.0f);
		TestTrue(TEXT("ray from outside hits the south wall"), Hit.bHit);
		TestEqual(TEXT("it is edge 0"), Hit.EdgeIndex, 0);
		// 边 0 的 Start = (-HX, -HY)、U = (+1, 0) ⇒ x = 0 处的 S 就是半个 footprint.X。
		TestTrue(FString::Printf(TEXT("S lands at mid-wall (%.1f)"), Hit.S),
			FMath::IsNearlyEqual(Hit.S, float(Footprint.X * 0.5), 0.01f));
		TestTrue(FString::Printf(TEXT("Z is the ray height (%.1f)"), Hit.Z),
			FMath::IsNearlyEqual(Hit.Z, 150.0f, 0.01f));
		TestTrue(FString::Printf(TEXT("distance is the gap to the face (%.1f)"), Hit.Distance),
			FMath::IsNearlyEqual(Hit.Distance, 200.0f, 0.01f));
	}

	// **背面不算命中**：站在房子里往外打，一面墙都不该咬上 —— 否则在屋里挥一下鼠标就会把窗
	// 贴到背面那堵墙上，而画面上看起来只是"窗跑到对面去了"。
	{
		const FCSWallHit Hit = CSHouse_RayHitWall(
			FVector::ZeroVector, FVector(0.0, -1.0, 0.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 1000.0f);
		TestFalse(TEXT("a ray leaving from inside hits nothing"), Hit.bHit);
	}

	// 打在墙顶以上 ⇒ 不命中（Z 越界）。这条守的是"窗贴到屋顶上"。
	{
		const FCSWallHit Hit = CSHouse_RayHitWall(
			FVector(0.0, -400.0, WallHeight + 50.0), FVector(0.0, 1.0, 0.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 1000.0f);
		TestFalse(TEXT("a ray above the eave hits nothing"), Hit.bHit);
	}

	// 够不着 ⇒ 不命中。MaxDistance 就是标记的探针长度，这条守的是"隔着半张地图也能咬上"。
	{
		const FCSWallHit Hit = CSHouse_RayHitWall(
			FVector(0.0, -400.0, 150.0), FVector(0.0, 1.0, 0.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 100.0f);
		TestFalse(TEXT("a ray that falls short hits nothing"), Hit.bHit);
	}

	// 就近版：贴在南墙外一点点、朝向随便，必须找到边 0 并把 S/Z 夹进墙面内。
	{
		const FCSWallHit Hit = CSHouse_NearestWall(
			FVector(0.0, -230.0, 150.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 200.0f);
		TestTrue(TEXT("a point just outside the wall snaps to it"), Hit.bHit);
		TestEqual(TEXT("it is edge 0"), Hit.EdgeIndex, 0);
		TestTrue(FString::Printf(TEXT("perpendicular distance (%.1f)"), Hit.Distance),
			FMath::IsNearlyEqual(Hit.Distance, 30.0f, 0.01f));
	}
	// 太远 ⇒ 不吸附。两条都空才轮到标记自毁，所以这条界限是承重的。
	{
		const FCSWallHit Hit = CSHouse_NearestWall(
			FVector(0.0, -1000.0, 150.0), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 200.0f);
		TestFalse(TEXT("a far point snaps to nothing"), Hit.bHit);
	}
	return true;
}

// -----------------------------------------------------------------------------
// D4 砖层（两层墙之 A，2026-09-06）：一摞包边带。纯函数，无 world
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseBrickWallTest,
	"PCGPlugins.ComputeShaderGenerator.House.BrickWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseBrickWallTest::RunTest(const FString& Parameters)
{
	const FVector2D Footprint(600.0, 400.0);
	const float T = 24.0f;
	const float WallHeight = 300.0f;

	// ---- 分层：铺满 0..檐口，不重不漏 ----
	{
		const CSHouseBrickWall::FCourses C = CSHouseBrickWall::PlanCourses(WallHeight, 20.0f);
		TestEqual(TEXT("300 / 20 gives 15 courses"), C.Count, 15);
		TestTrue(TEXT("courses exactly fill the wall"),
			FMath::IsNearlyEqual(C.Height * float(C.Count), WallHeight, 0.01f));
		TestTrue(TEXT("the first course sits on the floor"),
			FMath::IsNearlyEqual(C.CenterZ(0) - C.HalfHeight(), 0.0f, 0.01f));
		TestTrue(TEXT("the last course tops out at the eave"),
			FMath::IsNearlyEqual(C.CenterZ(C.Count - 1) + C.HalfHeight(), WallHeight, 0.01f));

		// 请求值除不尽时**向上取整**：宁可层高略矮，也不在檐口下留半层。
		const CSHouseBrickWall::FCourses Odd = CSHouseBrickWall::PlanCourses(300.0f, 40.0f);
		TestEqual(TEXT("300 / 40 rounds up to 8 courses"), Odd.Count, 8);
		TestTrue(TEXT("and the courses still fill the wall exactly"),
			FMath::IsNearlyEqual(Odd.Height * 8.0f, 300.0f, 0.01f));

		// 退化输入不许产出一个"看着合理"的方案。
		TestEqual(TEXT("zero wall height gives no courses"),
			CSHouseBrickWall::PlanCourses(0.0f, 20.0f).Count, 0);
		TestEqual(TEXT("zero course height gives no courses"),
			CSHouseBrickWall::PlanCourses(WallHeight, 0.0f).Count, 0);
	}

	CSHouseFrame::FBrickParams Params;
	Params.Length = 26.0f;
	Params.Gap = 0.0f;
	Params.MaxBricks = 4096;

	const CSHouseBrickWall::FCourses Courses = CSHouseBrickWall::PlanCourses(WallHeight, 20.0f);
	auto Noop = [](int32, const CSHouseTrim::FBand&, int32, const TArray<CSHouseTrim::FRun>&) {};

	// ---- 无洞：砖数不超上界，且四条边每层都各出一整段 ----
	int32 NoHoleBricks = 0;
	{
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		int32 CoursesSeen = 0;
		NoHoleBricks = CSHouseBrickWall::BuildWall(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Courses,
			3.0f, TArrayView<const FCSWallOpening>(), 0x1234u, Params, Runs, Elements,
			[&](int32, const CSHouseTrim::FBand&, int32, const TArray<CSHouseTrim::FRun>& R)
			{
				++CoursesSeen;
				TestEqual(TEXT("a course with no holes is four whole runs"), R.Num(), 4);
			});
		TestEqual(TEXT("every course was emitted"), CoursesSeen, Courses.Count);
		TestTrue(TEXT("bricks were emitted at all"), NoHoleBricks > 0);

		const int32 Budget = CSHouseBrickWall::EstimateBricks(FCSHouseFootprint::MakeRect(Footprint), T, Courses, Params.Length);
		TestTrue(FString::Printf(TEXT("the estimate is an upper bound (%d <= %d)"), NoHoleBricks, Budget),
			NoHoleBricks <= Budget);
		// 上界就是"中线周长 / 砖长 × 层数"，别留魔数：这里现算一遍对答案。周长取中线处的斜接
		// 区间（与 `BuildBand` 真正铺砖的区间同一个口径），不是外皮全长 `Len`。
		float Perimeter = 0.0f;
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			float S0 = 0.0f, S1 = 0.0f;
			CSHouse_GetEdge(Edge, Footprint, T).SpanAtDepth(T * 0.5f, T, S0, S1);
			Perimeter += S1 - S0;
		}
		TestEqual(TEXT("the budget is perimeter / brick length x courses"),
			Budget, FMath::CeilToInt(Perimeter / Params.Length) * Courses.Count);

		// **这条才是重点**：一栋 6×4 m、檐高 3 m 的房子要 1000+ 块砖，而砖的常驻容量默认只有
		// 512（`FrameReserveCapacity`，与门框/接缝/角石/包边共用）—— 砖层一开就会被截断。
		// 容量因此是 P1 必须先解决的事，不是收尾时再调的参数。
		TestTrue(FString::Printf(TEXT("one house alone outgrows the default brick capacity (%d)"), Budget),
			Budget > 1000);
	}

	// ---- 有洞：洞挡住的那些层要让开，没挡住的照旧整段 ----
	{
		// 一扇窗：边 0、S ∈ [200, 300]、Z ∈ [90, 200]。
		FCSWallOpening Win;
		Win.Type = ECSOpeningType::Window;
		Win.Shape = ECSOpeningShape::Rect;
		Win.EdgeIndex = 0;
		Win.CenterS = 250.0f;
		Win.Width = 100.0f;
		Win.Z0 = 90.0f;
		Win.Z1 = 200.0f;
		Win.SourceId = FGuid(1, 2, 3, 4);
		const TArray<FCSWallOpening> Openings = { Win };

		const float Clearance = 3.0f;
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		int32 CutCourses = 0;

		const int32 Total = CSHouseBrickWall::BuildWall(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Courses,
			Clearance, MakeArrayView(Openings), 0x1234u, Params, Runs, Elements,
			[&](int32 Index, const CSHouseTrim::FBand& Band, int32, const TArray<CSHouseTrim::FRun>& R)
			{
				const bool bBlocked = CSHouseTrim::BlocksBand(Win, Band);
				if (!bBlocked)
				{
					TestEqual(FString::Printf(TEXT("course %d clears the hole in Z, so it stays whole"), Index),
						R.Num(), 4);
					return;
				}
				++CutCourses;
				// 边 0 被切成两段 ⇒ 这一层总共 5 段。
				TestEqual(FString::Printf(TEXT("course %d is cut into an extra run"), Index), R.Num(), 5);

				for (const CSHouseTrim::FRun& Run : R)
				{
					if (Run.EdgeIndex != 0) continue;
					// ① 删实例：没有任何一段压在洞（含让开量）上。
					const bool bOverlaps = Run.S1 > Win.S0() - Clearance && Run.S0 < Win.S1() + Clearance;
					TestFalse(FString::Printf(TEXT("run [%.1f, %.1f] keeps off the hole"), Run.S0, Run.S1),
						bOverlaps);
				}
				// ② 水平贴合：洞两侧那两段的端点正好落在洞缘 ± 让开量上。
				const CSHouseTrim::FRun* Left = nullptr;
				const CSHouseTrim::FRun* Right = nullptr;
				for (const CSHouseTrim::FRun& Run : R)
				{
					if (Run.EdgeIndex != 0) continue;
					if (Run.S1 <= Win.S0()) Left = &Run;
					else Right = &Run;
				}
				if (TestNotNull(TEXT("there is a run left of the hole"), Left))
				{
					TestTrue(FString::Printf(TEXT("it butts against the hole (%.2f)"), Left->S1),
						FMath::IsNearlyEqual(Left->S1, Win.S0() - Clearance, 0.01f));
				}
				if (TestNotNull(TEXT("there is a run right of the hole"), Right))
				{
					TestTrue(FString::Printf(TEXT("it butts against the hole (%.2f)"), Right->S0),
						FMath::IsNearlyEqual(Right->S0, Win.S1() + Clearance, 0.01f));
				}
			});

		TestTrue(TEXT("some courses really were cut (otherwise the checks above are vacuous)"), CutCourses > 0);
		TestTrue(FString::Printf(TEXT("a hole only ever removes bricks (%d < %d)"), Total, NoHoleBricks),
			Total < NoHoleBricks);
	}

	// ---- 上界对**有洞**也得成立：窄洞会让一段路裂成两段，两段各自向上取整 ----
	//
	// ⚠️ 这不是多余的谨慎。`EstimateBricks` 是 `ceil(周长 / 砖长) x 层数`，除的是**砖长**而不是
	// 砖距（`Length + Gap`），所以 `Gap = 0` 的默认档下它**一点余量都没有**（实测 6x4 m 房
	// budget 1332 = 实际 1332，逐块相等）。而洞把一条整路裂成两段之后，两段各走一次 `SolveRun`
	// 的取整，合起来**可能比原来那一整段还多一块** —— 只要洞比半个砖距还窄，省下的长度补不回
	// 那次取整。真超了的后果不是报错，是 `Params.MaxBricks` 当场**静默截断**：砖层铺一半，
	// 而砖数、三角数、零阻塞每一条断言照绿。
	//
	// 所以这里从**比砖距还窄**（10 cm，砖距 26）一路扫到很宽，单洞与三洞各来一遍。
	{
		const int32 Budget = CSHouseBrickWall::EstimateBricks(FCSHouseFootprint::MakeRect(Footprint), T, Courses, Params.Length);
		const float Widths[8] = { 10.0f, 13.0f, 20.0f, 26.0f, 40.0f, 78.0f, 100.0f, 150.0f };
		int32 Worst = 0;
		float WorstWidth = 0.0f;
		int32 WorstHoles = 0;

		for (const float W : Widths)
		{
			for (int32 HoleCount = 1; HoleCount <= 3; HoleCount += 2)
			{
				TArray<FCSWallOpening> Many;
				for (int32 I = 0; I < HoleCount; ++I)
				{
					FCSWallOpening O;
					O.Type = ECSOpeningType::Window;
					O.Shape = ECSOpeningShape::Rect;
					// 铺到不同的边上，免得三个洞在同一条边上互相重叠成一个大洞（那就测不到裂段了）。
					O.EdgeIndex = I;
					O.CenterS = 150.0f + 60.0f * float(I);
					O.Width = W;
					O.Z0 = 90.0f;
					O.Z1 = 200.0f;
					O.SourceId = FGuid(7, 7, 7, I + 1);
					Many.Add(O);
				}

				TArray<CSHouseTrim::FRun> SweepRuns;
				TArray<CSHouseFrame::FElement> SweepElements;
				const int32 Built = CSHouseBrickWall::BuildWall(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Courses,
					3.0f, MakeArrayView(Many), 0x1234u, Params, SweepRuns, SweepElements, Noop);
				if (Built > Worst)
				{
					Worst = Built;
					WorstWidth = W;
					WorstHoles = HoleCount;
				}
			}
		}

		AddInfo(FString::Printf(TEXT("budget sweep: worst %d bricks (%d hole(s) of %.0f cm) vs budget %d, no-hole %d"),
			Worst, WorstHoles, WorstWidth, Budget, NoHoleBricks));
		TestTrue(FString::Printf(TEXT("no arrangement of holes ever outgrows the no-hole estimate (%d <= %d)"),
			Worst, Budget), Worst <= Budget);
	}

	// ---- 拱洞：砖跟着**剪影**收，不是被切成一个矩形缺口 ----
	//
	// 这条是①从"包围盒"升级成"剪影"的全部理由（2026-09-06）。落地拱 Z ∈ [0, 220]、
	// 拱脚在 220 − 100 = 120：120 以下满宽 200，120 以上按椭圆收窄，到 220 收成 0。
	{
		FCSWallOpening Arch;
		Arch.Type = ECSOpeningType::Door;
		Arch.Shape = ECSOpeningShape::Arch;
		Arch.EdgeIndex = 0;
		Arch.CenterS = 300.0f;
		Arch.Width = 200.0f;
		Arch.Z0 = 0.0f;
		Arch.Z1 = 220.0f;
		Arch.ArchRise = 100.0f;
		Arch.SourceId = FGuid(9, 9, 9, 9);

		// 半宽随高度的形状：拱脚以下满宽、拱脚以上单调收窄、洞顶归零、洞外归零。
		TestTrue(TEXT("below the springing the arch is full width"),
			FMath::IsNearlyEqual(CSHouse_OpeningHalfWidthAtZ(Arch, 50.0f), 100.0f, 0.01f));
		TestTrue(TEXT("at the springing it is still full width"),
			FMath::IsNearlyEqual(CSHouse_OpeningHalfWidthAtZ(Arch, 120.0f), 100.0f, 0.01f));
		const float Mid = CSHouse_OpeningHalfWidthAtZ(Arch, 170.0f);
		TestTrue(FString::Printf(TEXT("halfway up the arch it has narrowed (%.1f)"), Mid),
			Mid > 0.0f && Mid < 100.0f);
		TestTrue(TEXT("at the crown it closes"),
			FMath::IsNearlyEqual(CSHouse_OpeningHalfWidthAtZ(Arch, 220.0f), 0.0f, 0.01f));
		TestTrue(TEXT("above the hole there is nothing to cut"),
			FMath::IsNearlyEqual(CSHouse_OpeningHalfWidthAtZ(Arch, 260.0f), 0.0f, 0.01f));

		// ⚠️ 拱的 clip 场在拱脚以下**无下界**（那是有意的）。抬起来的拱窗必须靠洞自己的 Z0 兜住，
		// 否则它会一路裁到地面 —— 而画面上只是"墙脚少了一片砖"，没有任何断言会红。
		FCSWallOpening HighArch = Arch;
		HighArch.Z0 = 90.0f;
		HighArch.Z1 = 260.0f;
		TestTrue(TEXT("a raised arch does not cut below its own sill"),
			FMath::IsNearlyEqual(CSHouse_OpeningHalfWidthAtZ(HighArch, 50.0f), 0.0f, 0.01f));

		// 逐层：被挡住的 S 跨度必须**随高度单调不增**（拱圈越往上越窄）。
		const TArray<FCSWallOpening> Openings = { Arch };
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		float PrevSpan = TNumericLimits<float>::Max();
		int32 Narrowing = 0;
		CSHouseBrickWall::BuildWall(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Courses, 0.0f,
			MakeArrayView(Openings), 0x1234u, Params, Runs, Elements,
			[&](int32, const CSHouseTrim::FBand& Band, int32, const TArray<CSHouseTrim::FRun>&)
			{
				float B0 = 0.0f, B1 = 0.0f;
				const float Span = CSHouseTrim::BlockedSpan(Arch, Band, B0, B1) ? (B1 - B0) : 0.0f;
				TestTrue(FString::Printf(TEXT("the blocked span never widens going up (%.1f -> %.1f)"),
						PrevSpan, Span),
					Span <= PrevSpan + 0.01f);
				if (Span < PrevSpan - 0.01f) ++Narrowing;
				PrevSpan = Span;
			});
		// 至少收窄过几次 —— 否则上面那条"不变宽"是句空话（矩形缺口也满足它）。
		TestTrue(FString::Printf(TEXT("and it really does narrow (%d steps)"), Narrowing), Narrowing >= 3);
	}

	// ---- 容量是硬上限：撞上就截断，绝不扩容 ----
	{
		CSHouseFrame::FBrickParams Tight = Params;
		Tight.MaxBricks = 100;
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		const int32 Total = CSHouseBrickWall::BuildWall(FTransform::Identity, FCSHouseFootprint::MakeRect(Footprint), T, Courses,
			3.0f, TArrayView<const FCSWallOpening>(), 0x1234u, Tight, Runs, Elements, Noop);
		TestTrue(FString::Printf(TEXT("the wall is truncated at capacity (%d <= 100)"), Total),
			Total <= 100);
	}

	return true;
}


// -----------------------------------------------------------------------------
// D8 锚点（2026-09-06 用户裁决「位置的存储方式与 TG 一致」）：纯函数，无 world
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWallAnchorTest,
	"PCGPlugins.ComputeShaderGenerator.House.WallAnchor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseWallAnchorTest::RunTest(const FString& Parameters)
{
	const FVector2D Footprint(600.0, 400.0);
	const float T = 24.0f;
	const float WallHeight = 300.0f;
	const float WinHeight = 110.0f;

	auto MakeHit = [](int32 Edge, float S, float Z)
	{
		FCSWallHit H;
		H.bHit = true; H.EdgeIndex = Edge; H.S = S; H.Z = Z;
		return H;
	};

	// ---- 锚到**近**的那个角 ----
	{
		// 边 0 长 600。S = 100 靠起点角。
		const FCSWallAnchor A = CSHouse_MakeWallAnchor(MakeHit(0, 100.0f, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 95.0f);
		TestTrue(TEXT("anchor is valid"), A.IsValidAnchor());
		TestFalse(TEXT("S=100 on a 600 wall anchors to the start corner"), A.bFromEndCorner);
		TestTrue(TEXT("and records 100 from it"), FMath::IsNearlyEqual(A.DistFromCorner, 100.0f, 0.01f));

		// S = 500 靠终点角。
		const FCSWallAnchor B = CSHouse_MakeWallAnchor(MakeHit(0, 500.0f, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 95.0f);
		TestTrue(TEXT("S=500 anchors to the end corner"), B.bFromEndCorner);
		TestTrue(TEXT("and records 100 from it"), FMath::IsNearlyEqual(B.DistFromCorner, 100.0f, 0.01f));

		// 正中间归**起点角** —— 判据要给出确定的一侧，否则同一个位置两次算出两个锚点，
		// 而两者在墙不变时给出同一个 S ⇒ 差异要等到第一次拉尺寸才显形。
		const FCSWallAnchor M = CSHouse_MakeWallAnchor(MakeHit(0, 300.0f, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 95.0f);
		TestFalse(TEXT("dead centre is deterministic (start corner)"), M.bFromEndCorner);
	}

	// ---- 往返：墙不变时 锚点 → S 必须还原命中点 ----
	{
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
			const float S = F.Len * 0.3f;
			const FCSWallAnchor A = CSHouse_MakeWallAnchor(MakeHit(Edge, S, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 95.0f);
			TestTrue(FString::Printf(TEXT("edge %d round-trips S=%.1f"), Edge, S),
				FMath::IsNearlyEqual(CSHouse_AnchorS(A, FCSHouseFootprint::MakeRect(Footprint), T), S, 0.01f));
		}
	}

	// ---- **这条是本次改动的理由**：拉尺寸之后，靠"没动的那个角"的窗在世界里纹丝不动 ----
	//
	// `PushEdge(1, +100)` 干两件事：Footprint.X += 100，且 actor 中心沿 +X 移 50
	// （对侧墙在世界里不动）。于是 +X 那一侧的两个角各挪 +100、−X 那一侧的两个角不动。
	//
	// 拿边 2 来测（它从 (HX, HY) 往 −X 走，S 原点正是会动的那个角）：
	// 绝对弧长的老写法下，边 2 上的窗会**整体滑 +100**，而那面墙根本没动 —— 2026-09-05
	// 逐一推算出来的四条边里最难解释的一种。锚到远端角之后它必须不动。
	{
		const FVector2D Before(600.0, 400.0);
		const FVector2D After(700.0, 400.0);
		const double CentreShift = 50.0;   // PushEdge 同时把中心挪半个增量

		auto WorldXOnEdge2 = [&](float S, const FVector2D& FP, double Shift)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(2, FP, T);
			return F.Start.X + F.U.X * double(S) + Shift;   // 边 2 的 U = (−1, 0)
		};

		// 窗在边 2 的 S = 500 处 ⇒ 靠终点角（= 世界里不动的那个角）。
		const float SBefore = 500.0f;
		const FCSWallAnchor A = CSHouse_MakeWallAnchor(MakeHit(2, SBefore, 150.0f), FCSHouseFootprint::MakeRect(Before), T, 95.0f);
		TestTrue(TEXT("it anchored to the corner that will not move"), A.bFromEndCorner);

		const double XBefore = WorldXOnEdge2(SBefore, Before, 0.0);
		const double XAfter = WorldXOnEdge2(CSHouse_AnchorS(A, FCSHouseFootprint::MakeRect(After), T), After, CentreShift);
		TestTrue(FString::Printf(TEXT("the window holds its world position across the resize (%.1f -> %.1f)"),
				XBefore, XAfter),
			FMath::IsNearlyEqual(XBefore, XAfter, 0.01));

		// 反证：同一扇窗按**绝对弧长**记的话会滑整整 100 —— 这就是被修掉的那个 bug。
		const double XAbsolute = WorldXOnEdge2(SBefore, After, CentreShift);
		TestTrue(FString::Printf(TEXT("absolute arc length would have slid it by %.1f"), XAbsolute - XBefore),
			FMath::IsNearlyEqual(XAbsolute - XBefore, 100.0, 0.01));

		// 而靠**会动**的那个角的窗，本来就该随墙走：+X 面从 300 挪到 400。
		const FCSWallAnchor Near = CSHouse_MakeWallAnchor(MakeHit(2, 100.0f, 150.0f), FCSHouseFootprint::MakeRect(Before), T, 95.0f);
		TestFalse(TEXT("a window near the pushed corner anchors to it"), Near.bFromEndCorner);
		TestTrue(TEXT("and travels with that corner"),
			FMath::IsNearlyEqual(
				WorldXOnEdge2(CSHouse_AnchorS(Near, FCSHouseFootprint::MakeRect(After), T), After, CentreShift)
					- WorldXOnEdge2(100.0f, Before, 0.0),
				100.0, 0.01));
	}

	// ---- 墙缩到锚点越界 ⇒ 夹回墙面内（谓词照旧会判 NearCorner，但位置不许飞出去） ----
	{
		const FCSWallAnchor A = CSHouse_MakeWallAnchor(MakeHit(0, 100.0f, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 95.0f);
		const FVector2D Tiny(50.0, 400.0);
		const float S = CSHouse_AnchorS(A, FCSHouseFootprint::MakeRect(Tiny), T);
		TestTrue(FString::Printf(TEXT("S clamps into the shortened wall (%.1f)"), S), S >= 0.0f && S <= 50.0f);
	}

	// ---- **不动点**：派生出来的变换再打一次探针，必须拿回同一个锚点 ----
	//
	// 这条钉的是 `WallStandoff` 那个坑：把原点推进墙里（或正好贴在外皮上而 Standoff = 0）
	// 会让射线的 `Dist <= 0` 与就近版的"背面"判据一起失效 —— 症状是"窗吸附一次之后再也
	// 解析不到宿主"，而无宿主会自毁，全程不报红。
	{
		const float Standoff = 5.0f;
		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, T);
			const float S = F.Len * 0.4f;
			const FCSWallAnchor A = CSHouse_MakeWallAnchor(MakeHit(Edge, S, 150.0f), FCSHouseFootprint::MakeRect(Footprint), T, 150.0f - WinHeight * 0.5f);

			const FTransform Local = CSHouse_AnchorToLocal(A, FCSHouseFootprint::MakeRect(Footprint), T, WinHeight * 0.5f, Standoff);
			const FCSWallHit Back = CSHouse_RayHitWall(
				Local.GetLocation(), Local.GetRotation().GetForwardVector(), FCSHouseFootprint::MakeRect(Footprint), T, WallHeight, 600.0f);

			TestTrue(FString::Printf(TEXT("edge %d: the derived transform still sees its own wall"), Edge), Back.bHit);
			TestEqual(FString::Printf(TEXT("edge %d: same edge"), Edge), Back.EdgeIndex, Edge);
			TestTrue(FString::Printf(TEXT("edge %d: same S (%.2f vs %.2f)"), Edge, Back.S, S),
				FMath::IsNearlyEqual(Back.S, S, 0.01f));

			const FCSWallAnchor Round = CSHouse_MakeWallAnchor(Back, FCSHouseFootprint::MakeRect(Footprint), T, Back.Z - WinHeight * 0.5f);
			TestTrue(FString::Printf(TEXT("edge %d: the anchor is a fixed point of resolve"), Edge), Round == A);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWindowMarkerTest,
	"PCGPlugins.ComputeShaderGenerator.House.WindowMarker",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseWindowMarkerTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	// 道路驱动的门会占掉墙面，本条只测标记这一环 —— 把门那一路摘干净，否则断言会时红时绿
	// 地取决于有没有地面。
	House->Windows.Reset();

	ACSWindowMarker* Marker = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Window marker"), Marker)) return false;
	// ⚠️ **必须关掉自毁**：脚本是"先 spawn 后摆位"，spawn 那一瞬间标记在原点、找不到宿主，
	// 开着它会当场自删 —— 而"标记没了"与"标记判它放不下"在断言里长得一模一样。
	Marker->bDestroyWhenHostless = false;

	// 摆到南墙外 100 cm、朝 +Y（面向墙），高度取半个窗高以上免得一上来就 SillTooLow。
	const double HalfY = House->FootprintSize.Y * 0.5;
	Marker->SetActorLocation(FVector(0.0, -HalfY - 100.0, 150.0));
	Marker->SetActorRotation(FRotator(0.0, 90.0, 0.0));   // +X 指向 +Y

	TestTrue(TEXT("the marker resolves a host"), Marker->ResolveHostAndRegister(true));
	TestEqual(TEXT("the host is that house"), Marker->GetHost(), House);
	TestEqual(TEXT("the house holds exactly one marker demand"), House->GetFeatureMarkerCount(), 1);
	TestEqual(TEXT("the demand was accepted"), Marker->GetLastReject(), ECSFeatureReject::None);
	TestTrue(TEXT("the house really cut a hole for it"), Marker->CausesCut());
	TestEqual(TEXT("and that hole shows up in the window count"), House->GetWindowCount(), 1);

	// 吸附：接受之后标记被搬到墙面上（锚点 → 世界的派生变换），不再停在用户随手放的地方。
	const FVector AcceptedAt = Marker->GetActorLocation();
	TestTrue(TEXT("an accepted marker snaps onto the wall face"),
		FMath::IsNearlyEqual(AcceptedAt.Y, -HalfY - Marker->WallStandoff, 0.5));
	TestTrue(TEXT("and it is attached to the host (that is how it follows the house)"),
		Marker->GetAttachParentActor() == House);

	// ---- 松手时被拒 ⇒ **弹回最后一个被答应的位置**（计划 D8「回位规则」= TG DecoratorBackup）----
	//
	// ⚠️ 2026-09-06 起这条判据翻了个面：以前是"停在被拒处、窗洞掉到 0"，现在回退到
	// `LastAcceptedAnchor` 并吸附回去 —— 所以窗洞**留着**。"被拒的诉求不撤登记"那一条没变，
	// 它由下面那个从未被接受过的标记来钉。
	Marker->SetActorLocation(FVector(-House->FootprintSize.X * 0.5, -HalfY - 100.0, 150.0));
	Marker->ResolveHostAndRegister(true);
	TestEqual(TEXT("the demand is still registered"), House->GetFeatureMarkerCount(), 1);
	TestTrue(TEXT("a rejected release springs back to the last accepted spot"),
		Marker->GetActorLocation().Equals(AcceptedAt, 0.5));
	TestTrue(TEXT("so it still cuts a hole"), Marker->CausesCut());
	TestEqual(TEXT("and the window is still there"), House->GetWindowCount(), 1);

	// ---- 房子拉尺寸 ⇒ 标记按锚点跟着走 ----
	//
	// 2026-09-05 核出的缺陷：标记既不 attach、也没有任何人在房子变化时通知它 ⇒ 洞与标记
	// 从此分家（推第 e 条边，第 e 与 e+1 条边上的窗错位 Δ）。判据取"锚点是权威"的字面含义：
	// **标记的世界位置恒等于锚点派生出来的位置**。
	{
		const int32 EdgeBefore = Marker->GetAnchor().EdgeIndex;
		House->PushEdge(1, 100.0f, true);   // 推 +X 那面墙：footprint → 700×400，中心挪 +50
		TestEqual(TEXT("resizing does not fling the window onto another wall"),
			Marker->GetAnchor().EdgeIndex, EdgeBefore);
		TestTrue(TEXT("and the marker follows its anchor across the resize"),
			Marker->GetActorLocation().Equals(
				House->AnchorToWorld(Marker->GetAnchor(), Marker->GetDemandHalfHeight(),
					Marker->WallStandoff).GetLocation(), 0.5));
		TestTrue(TEXT("the window survives it"), Marker->CausesCut());
	}

	// ---- 盖顶件不许污染洞（三件里只有 OpeningMesh 定洞）----
	//
	// ⚠️ 这条钉的是本类**最容易搞错、而且搞错了一条断言都不会红**的纪律：把过梁/窗台并进定洞件，
	// 洞会从 78×160 涨到框外那一圈，而过梁正好把它盖住 —— 画面上看不出来，只有从侧面或洞的
	// 内壁才露馅。所以这里用一个**故意超大**的假过梁（引擎的 100³ Cube）去撞它。
	{
		float BeforeW = 0.0f, BeforeH = 0.0f;
		Marker->GetDemandSize(BeforeW, BeforeH);
		TestTrue(TEXT("the opening mesh gives a sane size to begin with"), BeforeW > 1.0f && BeforeH > 1.0f);
		const int32 WindowsBefore = House->GetWindowCount();

		UStaticMesh* Fat = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (TestNotNull(TEXT("engine cube (stand-in for an oversized lintel)"), Fat))
		{
			Marker->LintelMesh->SetStaticMesh(Fat);
			Marker->SillMesh->SetStaticMesh(Fat);
			Marker->ResolveHostAndRegister(true);

			float AfterW = 0.0f, AfterH = 0.0f;
			Marker->GetDemandSize(AfterW, AfterH);
			TestTrue(FString::Printf(TEXT("dressing meshes do not widen the hole (%.1f -> %.1f)"), BeforeW, AfterW),
				FMath::IsNearlyEqual(AfterW, BeforeW, 0.01f));
			TestTrue(FString::Printf(TEXT("nor heighten it (%.1f -> %.1f)"), BeforeH, AfterH),
				FMath::IsNearlyEqual(AfterH, BeforeH, 0.01f));
			TestEqual(TEXT("and the house still cuts exactly the same windows"),
				House->GetWindowCount(), WindowsBefore);

			Marker->LintelMesh->SetStaticMesh(nullptr);
			Marker->SillMesh->SetStaticMesh(nullptr);
		}

		// **组件恒在、网格可空**：清掉定洞件 ⇒ 退回手填的 Width / Height，而不是产出零尺寸洞
		// （谓词对零宽洞只会淡淡地说一句 Degenerate，画面上"窗没了"与"窗被门挤掉"长得一样）。
		UStaticMesh* Opening = Marker->OpeningMesh->GetStaticMesh();
		Marker->OpeningMesh->SetStaticMesh(nullptr);
		float BareW = 0.0f, BareH = 0.0f;
		Marker->GetDemandSize(BareW, BareH);
		TestTrue(FString::Printf(TEXT("an empty opening slot falls back to the typed size (%.1f x %.1f)"),
				BareW, BareH),
			FMath::IsNearlyEqual(BareW, Marker->Width, 0.01f)
				&& FMath::IsNearlyEqual(BareH, Marker->Height, 0.01f));
		Marker->OpeningMesh->SetStaticMesh(Opening);
		Marker->ResolveHostAndRegister(true);
	}

	// ---- 从未被接受过的标记：登记留着、不弹回、洞不出 ----
	//
	// 这条钉的是计划 D8 那句"被拒的诉求留在列表里、只是这一轮不出洞" —— 撤登记的话，
	// "从被门拱占住的墙拖到隔壁墙"会先把它删掉再也回不来。
	{
		ACSWindowMarker* NeverOk = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Corner marker"), NeverOk)) return false;
		NeverOk->bDestroyWhenHostless = false;
		NeverOk->SetActorLocation(FVector(-House->FootprintSize.X * 0.5, -HalfY - 100.0, 150.0));
		NeverOk->SetActorRotation(FRotator(0.0, 90.0, 0.0));
		NeverOk->ResolveHostAndRegister(true);

		TestEqual(TEXT("a never-accepted rejection stays registered"), House->GetFeatureMarkerCount(), 2);
		TestEqual(TEXT("and it reports why"), NeverOk->GetLastReject(), ECSFeatureReject::NearCorner);
		TestFalse(TEXT("no hole was cut for it"), NeverOk->CausesCut());
		TestEqual(TEXT("it is counted as a reject, not as a missing demand"), House->GetWindowRejectCount(), 1);
		TestEqual(TEXT("the accepted one is untouched"), House->GetWindowCount(), 1);

		World->DestroyActor(NeverOk);
		TestEqual(TEXT("and removing it leaves the first one alone"), House->GetFeatureMarkerCount(), 1);
	}

	// ---- 换宿主：先向旧的注销，再挂新的 ----
	ACSHouseActor* Other = World->SpawnActor<ACSHouseActor>(
		FVector(0.0, 3000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Second house"), Other)) return false;
	Other->Windows.Reset();

	const double OtherHalfY = Other->FootprintSize.Y * 0.5;
	Marker->SetActorLocation(FVector(0.0, 3000.0 - OtherHalfY - 100.0, 150.0));
	Marker->ResolveHostAndRegister(true);
	TestEqual(TEXT("the marker moved to the second house"), Marker->GetHost(), Other);
	TestEqual(TEXT("the old host let go of it"), House->GetFeatureMarkerCount(), 0);
	TestEqual(TEXT("the new host picked it up"), Other->GetFeatureMarkerCount(), 1);
	TestTrue(TEXT("and it cuts a hole there"), Marker->CausesCut());

	// ---- 删掉标记 ⇒ 宿主那一份跟着消失（"无主的窗"这一类状态被消灭）----
	World->DestroyActor(Marker);
	TestEqual(TEXT("destroying the marker unregisters its demand"), Other->GetFeatureMarkerCount(), 0);
	TestEqual(TEXT("and the hole closes"), Other->GetWindowCount(), 0);

	// ---- 找不到宿主时确实会自毁（这条单独造一个，免得污染上面的断言）----
	ACSWindowMarker* Lonely = World->SpawnActor<ACSWindowMarker>(
		FVector(0.0, -100000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Lonely marker"), Lonely)) return false;
	TestFalse(TEXT("a marker far from any house resolves nothing"), Lonely->ResolveHostAndRegister(true));
	TestFalse(TEXT("and it destroys itself (D8 ruling)"), IsValid(Lonely));

	return true;
}

// -----------------------------------------------------------------------------
// 点击加窗的**落地那一步**（D8 笔刷模式，2026-09-06）：`AdoptAnchor` 与 `PlaceMarkerAlongRay`
//
// `House.WindowMarker` 钉的是**拖 gizmo** 那条路（输入 = actor 自己的变换 →
// `ResolveHostAndRegister`）。这一条钉的是**点击**那条路（输入 = 已经解出来的命中 →
// `AdoptAnchor`），两者的分工写在 `ACSHouseFeatureMarker::AdoptAnchor` 的注释里。
//
// 判据是"锚点是权威"这句话的可执行形式 —— **三份坐标必须逐位互相印证**：
//   ① 标记身上的 `Anchor`（(边号, 离角距离, 洞底高) 这份权威记录）；
//   ② 房子登记表里那个洞的 `CenterS`（弧长，谓词与 clip 场吃的就是它）；
//   ③ 标记的**世界位置**（用户看得见的那一份）。
// 三者写岔任意一对，画面上都是"窗贴在离你瞄的地方几十厘米开外"，而窗数、砖数、三角数
// 全部照绿 —— 所以必须三份对齐着判，只判其中一份等于没判。
//
// 顺带钉死**空 `WindowBrushClass` 的退路到底能不能 spawn**：`ACSHouseFeatureMarker` 是
// `Abstract`，而 `CLASS_Abstract` **不在 `CLASS_Inherit` 里**，所以子类 `ACSWindowMarker`
// 不是抽象类、退路是好的。这句话靠读代码是猜的，靠这里的断言才是知道的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWindowBrushPlacementTest,
	"PCGPlugins.ComputeShaderGenerator.House.WindowBrushPlacement",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseWindowBrushPlacementTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// ---- ① 类说明符：退路能不能实例化（复核项，2026-09-06）----
	//
	// ⚠️ 这三条**必须一起看**。只判"子类不是抽象的"会在有人把 `Abstract` 抄到子类上时报红，
	// 但读断言的人无从知道父类本来就是抽象的、这是不是预期；只判父类则完全没测到退路。
	TestTrue(TEXT("the feature-marker base really is abstract (otherwise the next check is vacuous)"),
		ACSHouseFeatureMarker::StaticClass()->HasAnyClassFlags(CLASS_Abstract));
	TestFalse(TEXT("ACSWindowMarker is NOT abstract, so an empty WindowBrushClass can still spawn"),
		ACSWindowMarker::StaticClass()->HasAnyClassFlags(CLASS_Abstract));
	// `NotPlaceable` 反过来**是**继承的（它在 `CLASS_Inherit` 里）—— 拖放入口已退役，
	// 这一条正是想要的，和上一条一起说明了"哪些说明符会传染"这件事没有被记岔。
	TestTrue(TEXT("but NotPlaceable IS inherited, so it still cannot be dragged into the viewport"),
		ACSWindowMarker::StaticClass()->HasAnyClassFlags(CLASS_NotPlaceable));
	// `Blueprintable` 是 metadata（`IsBlueprintBase`），按继承链查 —— 两个基类都是
	// `NotBlueprintable`，掉了这条就再也建不出 `BP_Window_*` 那一族子蓝图，而且不报错。
	// （`WITH_METADATA` 守卫：`GetBoolMetaDataHierarchical` 只在带 metadata 的构建里存在。）
#if WITH_METADATA
	TestTrue(TEXT("and Blueprintable survived, so BP_Window_* subclasses are still authorable"),
		ACSWindowMarker::StaticClass()->GetBoolMetaDataHierarchical(TEXT("IsBlueprintBase")));
#endif

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	// 与 `House.WindowMarker` 同一条理由：道路驱动的门会占掉墙面，把门那一路摘干净，
	// 否则断言会时红时绿地取决于有没有地面。
	House->Windows.Reset();
	House->ReevaluateSite();

	// 没有登记表（2026-09-16）：名单就是 world 本身，`UCSHouseLibrary::GetHouses` 走 `TActorRange`。
	TestEqual(TEXT("the world enumerates exactly this one house"), UCSHouseLibrary::GetHouseCount(House), 1);

	const FVector2D Footprint = House->FootprintSize;
	const float T = House->WallThickness;

	// -------------------------------------------------------------------------
	// ② AdoptAnchor：给一个**已知**的 FCSWallHit，三份坐标必须互相印证
	// -------------------------------------------------------------------------
	//
	// 取边 1（+X 那面短墙）弧长 120、命中高度 150 —— 斜接口径下边 1 从外角点量起、长 400，
	// 护角 max(60, 让出量 24) = 60 ⇒ 可用区间 [60, 340]，120 稳稳落在里面（直角对接时代是 352 长、
	// [60, 292]，选点理由同 `demo_house_window` 里那三扇窗）。
	{
		ACSWindowMarker* Marker = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Window marker"), Marker)) return false;
		// ⚠️ 必须关自毁：spawn 那一瞬间标记在原点、还没 adopt，开着它会当场自删 ——
		// 而"标记没了"与"标记判它放不下"在断言里长得一模一样。
		Marker->bDestroyWhenHostless = false;

		FCSWallHit Hit;
		Hit.bHit = true;
		Hit.EdgeIndex = 1;
		Hit.S = 120.0f;
		Hit.Z = 150.0f;

		// 命中点的 Z 是窗**心**，锚点存的是**洞底** ⇒ 减半个窗高。口径与 `PlaceMarkerAlongRay`
		// 同源（两处写岔的症状是"窗整体偏高半扇"）。
		const float HalfHeight = Marker->GetDemandHalfHeight();
		TestTrue(TEXT("the marker reports a sane demand height to begin with"), HalfHeight > 1.0f);
		const float SillZ = FMath::Max(0.0f, Hit.Z - HalfHeight);
		const FCSWallAnchor Want = CSHouse_MakeWallAnchor(Hit, FCSHouseFootprint::MakeRect(Footprint), T, SillZ);
		if (!TestTrue(TEXT("the hand-built anchor is valid"), Want.IsValidAnchor())) return false;

		Marker->AdoptAnchor(House, Want);

		// —— 权威那一份：锚点被**原样**收下（AdoptAnchor 不打射线、不重新解析）——
		TestTrue(TEXT("AdoptAnchor takes the anchor verbatim (it resolves nothing of its own)"),
			Marker->GetAnchor() == Want);
		TestEqual(TEXT("the marker adopted that house as its host"), Marker->GetHost(), House);
		TestTrue(TEXT("and attached to it, so the house carries it around"),
			Marker->GetAttachParentActor() == House);
		TestEqual(TEXT("the house holds exactly one marker demand"), House->GetFeatureMarkerCount(), 1);
		TestEqual(TEXT("the demand was accepted"), Marker->GetLastReject(), ECSFeatureReject::None);
		TestTrue(TEXT("so the house really cut a hole for it"), Marker->CausesCut());
		TestEqual(TEXT("and it shows up in the window count"), House->GetWindowCount(), 1);

		// —— ②：洞的 `CenterS` 与锚点同源 ——
		//
		// ⚠️ 判据写成"等于 `CSHouse_AnchorS(锚点)`"而**不是**"等于 120"：`CenterS` 是 footprint
		// 的函数（`S = bFromEndCorner ? Len − Dist : Dist`），硬编 120 只在这一个尺寸下成立，
		// 房子一改尺寸就变成一条骗人的绿灯。顺便钉一次它此刻确实**就是** 120，
		// 免得两边一起写错还互相印证。
		const float WantS = CSHouse_AnchorS(Want, FCSHouseFootprint::MakeRect(Footprint), T);
		TestTrue(FString::Printf(TEXT("the anchor really points at the spot we aimed at (S=%.2f)"), WantS),
			FMath::IsNearlyEqual(WantS, 120.0f, 0.01f));

		const TArray<FCSWallOpening> Openings = House->GetCurrentOpenings();
		const FCSWallOpening* Cut = Openings.FindByPredicate(
			[](const FCSWallOpening& O) { return O.Type == ECSOpeningType::Window; });
		if (!TestNotNull(TEXT("the openings table carries that window"), Cut)) return false;
		TestEqual(TEXT("the hole sits on the edge the anchor names"), Cut->EdgeIndex, Want.EdgeIndex);
		TestTrue(FString::Printf(TEXT("the hole's CenterS is the anchor's arc length (%.2f vs %.2f)"),
				Cut->CenterS, WantS),
			FMath::IsNearlyEqual(Cut->CenterS, WantS, 0.01f));
		TestTrue(FString::Printf(TEXT("and its floor is the anchor's SillZ (%.2f vs %.2f)"),
				Cut->Z0, Want.SillZ),
			FMath::IsNearlyEqual(Cut->Z0, Want.SillZ, 0.01f));

		// —— ③：世界位置也是同一个锚点派生出来的（`AdoptAnchor` 无条件吸附）——
		//
		// ⚠️ 这条钉的是「无条件吸附」那句纪律：不摆的话标记会留在 spawn 时那个临时位姿
		// （相机跟前 / 原点），用户在墙上根本找不到它，而上面每一条断言照绿。
		const FVector Want3 =
			House->AnchorToWorld(Want, HalfHeight, Marker->WallStandoff).GetLocation();
		TestTrue(FString::Printf(TEXT("the marker snapped onto the anchor's world spot (%s vs %s)"),
				*Marker->GetActorLocation().ToCompactString(), *Want3.ToCompactString()),
			Marker->GetActorLocation().Equals(Want3, 0.5));
		TestTrue(TEXT("which is nowhere near where it was spawned (so the snap is a real move)"),
			!Marker->GetActorLocation().Equals(FVector::ZeroVector, 1.0));

		// —— 幂等：同一个锚点再收一次，什么都不许变（重复点击 / 重复加载都会走到）——
		Marker->AdoptAnchor(House, Want);
		TestEqual(TEXT("adopting the same anchor twice does not double-register"),
			House->GetFeatureMarkerCount(), 1);
		TestEqual(TEXT("nor double-cut"), House->GetWindowCount(), 1);

		World->DestroyActor(Marker);
		House->ReevaluateSite();
		TestEqual(TEXT("destroying it closes the hole again"), House->GetWindowCount(), 0);
	}

	// -------------------------------------------------------------------------
	// ③ PlaceMarkerAlongRay：唯一执行面。退路可 spawn、抽象类被挡住且不静默
	// -------------------------------------------------------------------------
	{
		// 从 −Y 那面长墙外往墙里打。用命中点反推的短射线与 `FCSWindowBrushEdMode::CommitSamples`
		// 同形（恒垂直于墙 ⇒ `CSHouse_RayHitWall` 的"只认外表面"判据必然成立）。
		const double HalfY = Footprint.Y * 0.5;
		const FVector Origin(0.0, -HalfY - 120.0, 150.0);
		const FVector Dir(0.0, 1.0, 0.0);

		// —— 空 `WindowBrushClass` 的退路：`ACSWindowMarker` 本身 ——
		//
		// ⚠️ 这一条是上面那个 `CLASS_Abstract` 断言的**执行面对照**：类标志说"能 spawn"，
		// 这里证明它**真的**被 spawn 出来并落到了墙上。只判标志不判这条的话，
		// 将来谁在 `PlaceMarkerAlongRay` 里加一道把退路挡掉的闸，标志断言照样绿。
		ACSHouseFeatureMarker* Fallback = UCSHouseLibrary::PlaceMarkerAlongRay(World,
			ACSWindowMarker::StaticClass(), Origin, Dir, 400.0f);
		if (!TestNotNull(TEXT("the C++ fallback class (empty WindowBrushClass) really spawns"), Fallback))
		{
			return false;
		}
		TestEqual(TEXT("and it lands on that house"), Fallback->GetHost(), House);
		TestTrue(TEXT("cutting one hole"), Fallback->CausesCut());
		TestEqual(TEXT("exactly one"), House->GetWindowCount(), 1);

		// —— 抽象类：什么都不生成，而且**出声**（2026-09-06 新增的闸）——
		//
		// ⚠️ 只判返回值为空是不够的：`SpawnActor` 失败之后如果还留下了半个登记，
		// "生成了但没登记"会静静地漏过去。所以连计数一起判。
		const int32 WindowsBefore = House->GetWindowCount();
		const int32 MarkersBefore = House->GetFeatureMarkerCount();
		// ⚠️ **这条同时是"它真的出声了"的断言**：`AddExpectedError...` 要求这句警告恰好出现
		// 一次，一次都不出（有人把 `UE_LOG` 删了 / 改了措辞）就报红。用 `...Plain` 而不是
		// `AddExpectedError`：后者的 `IsRegex` 默认是 **true**，模式里将来混进一个元字符就会
		// 静默变成另一条正则。
		AddExpectedErrorPlain(TEXT("is abstract and cannot be spawned"),
			EAutomationExpectedErrorFlags::Contains, 1);
		ACSHouseFeatureMarker* Abstract = UCSHouseLibrary::PlaceMarkerAlongRay(World,
			ACSHouseFeatureMarker::StaticClass(), Origin, Dir, 400.0f);
		TestNull(TEXT("an abstract marker class places nothing"), Abstract);
		TestEqual(TEXT("and leaves the window count alone"), House->GetWindowCount(), WindowsBefore);
		TestEqual(TEXT("and the demand list alone"), House->GetFeatureMarkerCount(), MarkersBefore);

		// —— 打空：同样什么都不生成（没有"游离标记"这种状态）——
		ACSHouseFeatureMarker* Miss = UCSHouseLibrary::PlaceMarkerAlongRay(World,
			ACSWindowMarker::StaticClass(), FVector(0.0, 0.0, 100000.0), FVector::UpVector, 400.0f);
		TestNull(TEXT("a ray that misses every wall places nothing"), Miss);
		TestEqual(TEXT("and changes nothing"), House->GetWindowCount(), WindowsBefore);

		// —— 执行面与 `AdoptAnchor` 是**同一条**口径：命中的 Z 是窗心，锚点是洞底 ——
		//
		// ⚠️ 钉这条是因为两处各自减了一次半窗高，写岔了就是"窗整体偏高半扇"，
		// 而它在画面上只有贴着檐口时才露馅（那时会莫名判 `AboveEave`）。
		const FCSWallAnchor Got = Fallback->GetAnchor();
		TestTrue(FString::Printf(TEXT("the placed anchor's sill is the hit Z minus half the window (%.2f)"),
				Got.SillZ),
			FMath::IsNearlyEqual(Got.SillZ, 150.0f - Fallback->GetDemandHalfHeight(), 0.5f));
		TestTrue(TEXT("and the marker sits exactly where that anchor says it should"),
			Fallback->GetActorLocation().Equals(
				House->AnchorToWorld(Got, Fallback->GetDemandHalfHeight(),
					Fallback->WallStandoff).GetLocation(), 0.5));

		World->DestroyActor(Fallback);
		House->ReevaluateSite();
		TestEqual(TEXT("and deleting it closes the hole"), House->GetWindowCount(), 0);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 窗贴墙脚变门（2026-09-16，TG `snap_balcony_door`，附录 E）
//
// 纯函数那条钉判据本身：两个形态各自是解析的不动点、进出门之间有迟滞、门形态只放过窗台下限
// 不放过过梁带。actor 那条钉接线：拖下去变门、原地重解析不翻、抬上去变回窗、笔刷点在墙脚直接出门、
// 墙矮过门就被拒 —— 每一步都顺带核对洞与网格件的显隐，因为"洞对了、网格没换"不会有别的断言报红。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDoorFormRuleTest,
	"PCGPlugins.ComputeShaderGenerator.House.DoorFormRule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDoorFormRuleTest::RunTest(const FString& Parameters)
{
	// TG cottage 1x1：窗碰撞盒 160 高，门 262.5 高（附录 E §3.5）。
	const float WinH = 160.0f;
	const float DoorH = 262.5f;

	// 两档阈值：TG 原值 0，与本项目默认生效的 max(0, WindowMinSillZ = 40)。
	for (const float Snap : { 0.0f, 40.0f })
	{
		const FString Tag = FString::Printf(TEXT("[snap=%.0f]"), Snap);
		TestFalse(Tag + TEXT(" a window whose bottom clears the threshold stays a window"),
			CSHouse_ResolveDoorForm(Snap + WinH * 0.5f + 1.0f, WinH, DoorH, Snap, false));
		TestTrue(Tag + TEXT(" a window whose bottom dips below the threshold becomes a door"),
			CSHouse_ResolveDoorForm(Snap + WinH * 0.5f - 1.0f, WinH, DoorH, Snap, false));

		// 不动点：吸附之后再解析一次必须拿回同一个形态，否则松手即翻、改个属性也翻。
		TestTrue(Tag + TEXT(" a snapped door (centre = door height / 2) is a fixed point"),
			CSHouse_ResolveDoorForm(DoorH * 0.5f, WinH, DoorH, Snap, true));
		TestFalse(Tag + TEXT(" a window sitting exactly on the threshold is a fixed point"),
			CSHouse_ResolveDoorForm(Snap + WinH * 0.5f, WinH, DoorH, Snap, false));

		// 迟滞：刚变成门的那个高度不会把门翻回窗；门底抬过阈值才回窗。
		TestTrue(Tag + TEXT(" the height that turned the window into a door keeps it a door"),
			CSHouse_ResolveDoorForm(Snap + WinH * 0.5f - 1.0f, WinH, DoorH, Snap, true));
		TestFalse(Tag + TEXT(" lifting the door until its bottom clears the threshold turns it back"),
			CSHouse_ResolveDoorForm(FMath::Max(Snap, 1.0f) + DoorH * 0.5f + 1.0f, WinH, DoorH, Snap, true));
	}
	TestFalse(TEXT("no door mesh ⇒ never a door"), CSHouse_ResolveDoorForm(0.0f, WinH, 0.0f, 40.0f, false));

	// 谓词：同一个落地洞，窗判 SillTooLow、门形态放行；门形态**不**放过地面以下与过梁带。
	FCSOpeningSite Site;   // 600 × 400、墙高 300、过梁带 40、护角 60
	Site.MinSillZ = 40.0f;
	FCSWallOpening Hole;
	Hole.Type = ECSOpeningType::Window;
	Hole.Shape = ECSOpeningShape::Rect;
	Hole.EdgeIndex = 0;
	Hole.CenterS = 300.0f;
	Hole.Width = 114.8f;
	Hole.Z0 = 0.0f;
	Hole.Z1 = Site.WallHeight - Site.LintelBand;
	TestEqual(TEXT("a floor-level window is still SillTooLow"), CSHouse_QueryOpening(Site, Hole), ECSFeatureReject::SillTooLow);
	Hole.bDoorForm = true;
	TestEqual(TEXT("the same hole in door form is accepted"), CSHouse_QueryOpening(Site, Hole), ECSFeatureReject::None);
	FCSWallOpening Sunk = Hole;
	Sunk.Z0 = -1.0f;
	TestEqual(TEXT("door form does not excuse a hole below the ground"), CSHouse_QueryOpening(Site, Sunk), ECSFeatureReject::SillTooLow);
	FCSWallOpening Tall = Hole;
	Tall.Z1 = DoorH;
	TestEqual(TEXT("nor one that eats the lintel band"), CSHouse_QueryOpening(Site, Tall), ECSFeatureReject::AboveEave);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseWindowBecomesDoorTest,
	"PCGPlugins.ComputeShaderGenerator.House.WindowBecomesDoor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseWindowBecomesDoorTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();

	ACSWindowMarker* Marker = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Window marker"), Marker)) return false;
	Marker->bDestroyWhenHostless = false;   // 理由同 House.WindowMarker：先 spawn 后摆位
	if (!TestTrue(TEXT("the TG door asset is loaded (otherwise every check below is vacuous)"), Marker->CanBecomeDoor())) return false;

	float WinW = 0.0f, WinH = 0.0f, DoorW = 0.0f, DoorH = 0.0f;
	Marker->GetFormSize(false, WinW, WinH);
	Marker->GetFormSize(true, DoorW, DoorH);
	TestTrue(FString::Printf(TEXT("the door is taller than the window (%.1f vs %.1f)"), DoorH, WinH), DoorH > WinH);
	{
		const UStaticMesh* DoorAsset = Marker->DoorMesh->GetStaticMesh();
		const float FrameWidth = float(DoorAsset->GetBounds().TransformBy(Marker->DoorMesh->GetRelativeTransform()).BoxExtent.Y) * 2.0f;
		TestTrue(FString::Printf(TEXT("the door hole is narrower than its frame by the inset (%.1f vs %.1f)"), DoorW, FrameWidth),
			FMath::IsNearlyEqual(DoorW, FrameWidth - 2.0f * Marker->DoorHoleInset, 0.05f));
	}

	const double HalfY = House->FootprintSize.Y * 0.5;
	const float Threshold = FMath::Max(Marker->DoorSnapHeight, House->WindowMinSillZ);
	auto ReleaseAt = [&](double CentreZ)
	{
		Marker->SetActorLocation(FVector(0.0, -HalfY - 100.0, CentreZ));
		Marker->SetActorRotation(FRotator(0.0, 90.0, 0.0));   // +X 指向墙
		return Marker->ResolveHostAndRegister(true);
	};
	auto FindCut = [&](const FGuid& Id, FCSWallOpening& Out)
	{
		for (const FCSWallOpening& O : House->GetCurrentOpenings())
		{
			if (O.SourceId == Id) { Out = O; return true; }
		}
		return false;
	};
	auto WindowPiecesShown = [&]() { return Marker->OpeningMesh->GetVisibleFlag() && Marker->SillMesh->GetVisibleFlag(); };
	auto DoorPiecesShown = [&]() { return Marker->DoorMesh->GetVisibleFlag(); };

	// ① 高于阈值：窗。
	TestTrue(TEXT("① resolves a host"), ReleaseAt(Threshold + WinH * 0.5f + 30.0f));
	TestFalse(TEXT("① well above the threshold it is a window"), Marker->IsDoorForm());
	TestTrue(TEXT("① and it cuts"), Marker->CausesCut());
	TestTrue(TEXT("① window pieces shown, door pieces hidden"), WindowPiecesShown() && !DoorPiecesShown());

	// ② 往下拖到窗底低于阈值：门。
	TestTrue(TEXT("② resolves a host"), ReleaseAt(Threshold + WinH * 0.5f - 10.0f));
	TestTrue(TEXT("② dipping the window's bottom below the threshold makes it a door"), Marker->IsDoorForm());
	TestTrue(TEXT("② the anchor carries the form"), Marker->GetAnchor().bDoorForm);
	TestEqual(TEXT("② and its floor snapped to the wall foot"), Marker->GetAnchor().SillZ, 0.0f);
	TestEqual(TEXT("② the house accepts it"), Marker->GetLastReject(), ECSFeatureReject::None);
	TestTrue(TEXT("② and cuts a hole for it"), Marker->CausesCut());
	{
		FCSWallOpening Cut;
		if (TestTrue(TEXT("② the hole is registered under the marker's id"), FindCut(Marker->GetMarkerId(), Cut)))
		{
			TestEqual(TEXT("② it is still a marker hole (no road-door frame bricks / leaves)"), Cut.Type, ECSOpeningType::Window);
			TestTrue(TEXT("② flagged as door form"), Cut.bDoorForm);
			TestEqual(TEXT("② starting at the wall foot"), Cut.Z0, 0.0f);
			TestTrue(FString::Printf(TEXT("② as wide as the door hole (%.1f vs %.1f)"), Cut.Width, DoorW), FMath::IsNearlyEqual(Cut.Width, DoorW, 0.05f));
			const float Usable = House->WallHeight - House->LintelBand;
			TestTrue(FString::Printf(TEXT("② as tall as the door, clamped under the lintel band (%.1f)"), Cut.Z1),
				FMath::IsNearlyEqual(Cut.Z1, FMath::Min(DoorH, Usable), 0.05f));
		}
	}
	TestTrue(FString::Printf(TEXT("② the marker snapped to the door's centre (z=%.2f)"), Marker->GetActorLocation().Z),
		FMath::IsNearlyEqual(Marker->GetActorLocation().Z, double(DoorH) * 0.5, 0.5));
	TestTrue(TEXT("② door pieces shown, window pieces hidden"), DoorPiecesShown() && !WindowPiecesShown());
	TestEqual(TEXT("② the marker door gets no road-door leaf"), House->GetDoorLeafCount(), 0);

	// ③ 不动点：从吸附位原地再解析，形态与锚点都不变。
	{
		const FCSWallAnchor Before = Marker->GetAnchor();
		Marker->ResolveHostAndRegister(true);
		TestTrue(TEXT("③ re-resolving from the snapped spot keeps the door"), Marker->IsDoorForm());
		TestTrue(TEXT("③ and the anchor does not move"), Marker->GetAnchor() == Before);
	}

	// ④ 墙矮过门（TG 剔除）：拒；墙恢复：回来。
	{
		const float SavedHeight = House->WallHeight;
		House->WallHeight = DoorH - 10.0f;
		House->RebuildHouse();
		TestFalse(TEXT("④ a wall lower than the door rejects it"), Marker->CausesCut());
		TestFalse(TEXT("④ and hides its pieces"), DoorPiecesShown());
		House->WallHeight = SavedHeight;
		House->RebuildHouse();
		TestTrue(TEXT("④ restoring the wall brings the door back"), Marker->CausesCut());
	}

	// ⑤ 抬上去：门底升过阈值就变回窗，窗台落在合理高度。
	TestTrue(TEXT("⑤ resolves a host"), ReleaseAt(FMath::Max(Threshold, 1.0f) + DoorH * 0.5f + 5.0f));
	TestFalse(TEXT("⑤ lifting the door clear of the threshold makes it a window again"), Marker->IsDoorForm());
	TestTrue(FString::Printf(TEXT("⑤ with a sill above the threshold (%.1f)"), Marker->GetAnchor().SillZ), Marker->GetAnchor().SillZ >= Threshold);
	TestTrue(TEXT("⑤ accepted"), Marker->CausesCut());
	TestTrue(TEXT("⑤ window pieces back, door pieces gone"), WindowPiecesShown() && !DoorPiecesShown());

	// ⑥ 笔刷点在墙脚附近：直接落成门（与拖拽同一个口径）。
	{
		ACSHouseFeatureMarker* Placed = UCSHouseLibrary::PlaceMarkerAlongRay(House, ACSWindowMarker::StaticClass(),
			FVector(160.0, -HalfY - 300.0, 40.0), FVector(0.0, 1.0, 0.0), 1000.0f);
		const ACSWindowMarker* PlacedWindow = Cast<ACSWindowMarker>(Placed);
		if (TestNotNull(TEXT("⑥ the brush placed a window marker"), PlacedWindow))
		{
			TestTrue(TEXT("⑥ clicked at the wall foot it lands as a door"), PlacedWindow->IsDoorForm());
			TestTrue(TEXT("⑥ and cuts"), PlacedWindow->CausesCut());
			World->DestroyActor(Placed);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// 拉尺寸抓手（D5 交互层）：父子回路的 2x 缺陷、下限记账、模式生命周期
//
// 这一条要 world —— 抓手是真 actor、attach 在房子下，而"父级移动 Applied/2 会把抓手一起
// 带走"正是缺陷的成因，纯函数层复现不出来。`House.EdgePush` 钉的是推拉本身的数学，
// 这一条钉的是**抓手到房子这段接线**。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeHandleTest,
	"PCGPlugins.ComputeShaderGenerator.House.ResizeHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 带 yaw：中心随动是**世界**方向的量，yaw=0 下测不出把局部量当世界量用的错误
	// （`House.EdgePush` 的第 ② 例同一条理由）。
	constexpr float Yaw = 37.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1000.0, 2000.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->MinFootprint = 200.0f;

	// ---- ① 进入模式：四面墙各一个，各自落在自己那面墙外 ----
	House->EnterResizeMode();
	TestTrue(TEXT("Entering resize mode reports the mode is on"), House->IsInResizeMode());

	// 九个抓手：四个水平锥子 + 檐口框 + 房底框 + 屋顶框 + 脊两端的脊长箭头（2026-10-02）。
	TestEqual(TEXT("Nine handles: four cones, the eave / base / roof frames and two ridge arrows"), House->GetResizeHandles().Num(), 9);
	TestNotNull(TEXT("The eave frame is there"), House->GetHeightHandle());
	TestNotNull(TEXT("The base frame is there"), House->GetBaseHandle());
	TestTrue(TEXT("The eave frame and the base frame are two different actors"), House->GetHeightHandle() != House->GetBaseHandle());

	TArray<ACSHouseResizeHandleActor*> Handles = House->GetEdgeHandles();
	if (!TestEqual(TEXT("One cone per wall"), Handles.Num(), 4)) return false;

	for (const ACSHouseResizeHandleActor* Handle : Handles)
	{
		if (!TestNotNull(TEXT("Handle"), Handle)) return false;
		TestTrue(TEXT("The handle knows its host"), Handle->GetHost() == House);
		// 锥子在墙外皮再往外 HandleOffset 处：投影到外法线上应当恰好比墙中心多出这么多。
		const FVector Outer = Handle->GetOuterNormalWorld();
		const FVector WallCentre = CSHouseTest_WallCentre(
			House->FootprintSize, House->GetActorLocation(), Handle->GetEdgeIndex(), Yaw);
		const double Out = FVector::DotProduct(Handle->GetActorLocation() - WallCentre, Outer);
		TestEqual(
			FString::Printf(TEXT("Handle %d sits HandleOffset outside its wall"), Handle->GetEdgeIndex()),
			Out, double(Handle->HandleOffset), 1.0e-2);
	}

	// ---- 高度框：停在檐口正上方，框的大小是 footprint 的 FrameScale 倍 ----
	ACSHouseHeightHandleActor* Height = House->GetHeightHandle();
	if (!TestNotNull(TEXT("Height handle"), Height)) return false;
	{
		// 规范位置：房心正上方、檐口高度 —— 框同时是"墙有多高"的读数。
		const FVector LocalPos = House->GetActorTransform().InverseTransformPosition(Height->GetActorLocation());
		TestTrue(TEXT("The height frame sits over the house centre"),
			FMath::IsNearlyZero(LocalPos.X, 1.0e-2) && FMath::IsNearlyZero(LocalPos.Y, 1.0e-2));
		TestEqual(TEXT("The height frame sits at the eave"), LocalPos.Z, double(House->WallHeight), 1.0e-2);
	}

	// 边号必须四条齐全，重号的话有一面墙推不动而另一面被推两次。
	{
		TSet<int32> Edges;
		for (const ACSHouseResizeHandleActor* Handle : Handles) Edges.Add(Handle->GetEdgeIndex());
		TestEqual(TEXT("The four handles cover four distinct edges"), Edges.Num(), 4);
	}

	// 幂等：再进一次不许生出第二组（详情面板上的按钮会被连点）。
	House->EnterResizeMode();
	TestEqual(TEXT("Re-entering resize mode does not spawn a second set"), House->GetResizeHandles().Num(), 9);

	// ---- ② 拖 1 m 墙恰好走 1 m：父子回路那个 2x 缺陷的钉子 ----
	ACSHouseResizeHandleActor* East = nullptr;
	for (ACSHouseResizeHandleActor* Handle : House->GetEdgeHandles())
	{
		if (Handle->GetEdgeIndex() == 1) { East = Handle; break; }
	}
	if (!TestNotNull(TEXT("East handle"), East)) return false;

	{
		constexpr double Delta = 100.0;
		const FVector Outer = East->GetOuterNormalWorld();
		const FVector PushedBefore = CSHouseTest_WallCentre(House->FootprintSize, House->GetActorLocation(), 1, Yaw);
		const FVector OppositeBefore = CSHouseTest_WallCentre(House->FootprintSize, House->GetActorLocation(), 3, Yaw);

		// 模拟 gizmo：把抓手挪 Delta，然后走它自己的执行面。
		East->SetActorLocation(East->GetActorLocation() + Outer * Delta);
		const float Applied = East->ConsumeDragToHost(false);

		TestEqual(TEXT("A one metre drag applies one metre"), double(Applied), Delta, 1.0e-2);
		const FVector PushedAfter = CSHouseTest_WallCentre(House->FootprintSize, House->GetActorLocation(), 1, Yaw);
		const FVector OppositeAfter = CSHouseTest_WallCentre(House->FootprintSize, House->GetActorLocation(), 3, Yaw);
		TestTrue(TEXT("The pushed wall moves exactly the drag"),
			PushedAfter.Equals(PushedBefore + Outer * Delta, 1.0e-2));
		TestTrue(TEXT("The opposite wall does not move"), OppositeAfter.Equals(OppositeBefore, 1.0e-2));
	}

	// ---- ③ 连续 10 步：单步可能蒙对，2x 回路是**稳态**行为，必须连拖才显形 ----
	{
		constexpr double Step = 25.0;
		constexpr int32 Steps = 10;
		// ⚠️ 边 1 驱动的是 **X**（`CSHouseResize_EdgeDrivesX(1) == true`）。写死成 .Y 的话
		// 断言量的是一条根本没被推的轴 —— 恒为 0，而"推拉整个没生效"也是 0，两者读不开。
		const double SizeBefore = CSHouseTest_PushedDim(House, 1);
		int32 Jumps = 0;
		for (int32 i = 0; i < Steps; ++i)
		{
			const FVector Outer = East->GetOuterNormalWorld();
			East->SetActorLocation(East->GetActorLocation() + Outer * Step);
			const float Applied = East->ConsumeDragToHost(false);
			if (FMath::Abs(double(Applied) - Step) > 1.0e-2) ++Jumps;
		}
		TestEqual(TEXT("Every drag step applies exactly the offset"), Jumps, 0);
		TestEqual(TEXT("Ten steps grow the footprint by ten steps"),
			CSHouseTest_PushedDim(House, 1) - SizeBefore, Steps * Step, 1.0e-2);
	}

	// ---- ④ 顶在 MinFootprint 上：返回实际生效量，且**继续内拖不攒残差** ----
	{
		const FVector Outer = East->GetOuterNormalWorld();
		const double SizeBefore = CSHouseTest_PushedDim(House, 1);
		East->SetActorLocation(East->GetActorLocation() - Outer * 10000.0);
		const float Applied = East->ConsumeDragToHost(false);

		TestEqual(TEXT("The floor holds"), CSHouseTest_PushedDim(House, 1), double(House->MinFootprint), 1.0e-2);
		TestEqual(TEXT("Shrinking past the floor applies only what was possible"),
			double(Applied), double(House->MinFootprint) - SizeBefore, 1.0e-2);

		// 再往里拖一大截：一步都不许再生效。记账记成请求值的话这里会返回 0 但残差照攒，
		// 下面那次外拖就会把攒下的量一次性放出来。
		East->SetActorLocation(East->GetActorLocation() - Outer * 5000.0);
		TestEqual(TEXT("Dragging further past the floor applies nothing"),
			double(East->ConsumeDragToHost(false)), 0.0, 1.0e-2);

		// 往外拖 50：必须**恰好**长 50，不许把刚才那 15000 的残差一起吐出来。
		East->SetActorLocation(East->GetActorLocation() + Outer * 50.0);
		TestEqual(TEXT("Pulling back out applies exactly the drag, not the swallowed residue"),
			double(East->ConsumeDragToHost(false)), 50.0, 1.0e-2);
	}

	// ---- ⑤ 每次推拉后框都保持闭合（**不只是松手**，2026-09-06 纪律 ②）----
	// 只在松手时回位的话，被拖的那根会以每次事件 0.4δ 的速度跑到光标前面，画面上框会裂开。
	{
		// 先拖一把（不松手），四根都必须已经在框上。
		const FVector Outer = East->GetOuterNormalWorld();
		East->SetActorLocation(East->GetActorLocation() + Outer * 60.0);
		East->ConsumeDragToHost(false);
		for (const ACSHouseResizeHandleActor* Handle : House->GetEdgeHandles())
		{
			TestTrue(
				FString::Printf(TEXT("Handle %d stays on the frame mid-drag"), Handle->GetEdgeIndex()),
				Handle->GetActorLocation().Equals(Handle->ComputeCanonicalWorldLocation(), 1.0e-2));
		}

		// 侧向 / 竖向偏一截：松手后必须被清掉（那两个分量本来就不参与推拉）。
		East->SetActorLocation(East->GetActorLocation() + FVector(0.0, 0.0, 137.0));
		East->ConsumeDragToHost(true);
		for (const ACSHouseResizeHandleActor* Handle : House->GetEdgeHandles())
		{
			TestTrue(
				FString::Printf(TEXT("Handle %d is back on its canonical spot after release"), Handle->GetEdgeIndex()),
				Handle->GetActorLocation().Equals(Handle->ComputeCanonicalWorldLocation(), 1.0e-2));
		}
	}

	// ---- ⑥ 退出模式：抓手全销毁 ----
	House->ExitResizeMode();
	TestFalse(TEXT("Leaving resize mode reports the mode is off"), House->IsInResizeMode());
	TestEqual(TEXT("Leaving resize mode destroys every handle"), House->GetResizeHandles().Num(), 0);
	House->ExitResizeMode();   // 幂等：不在模式里再调一次不许崩

	// ---- ⑦ 删房子：抓手跟着走（编辑器 world 只发 Destroyed，不发 EndPlay）----
	{
		House->EnterResizeMode();
		TArray<TWeakObjectPtr<ACSHouseHandleActor>> Weak;
		for (ACSHouseHandleActor* Handle : House->GetResizeHandles()) Weak.Add(Handle);
		if (!TestEqual(TEXT("Handles for the destroy pass"), Weak.Num(), 9)) return false;

		World->DestroyActor(House);
		for (const TWeakObjectPtr<ACSHouseHandleActor>& Handle : Weak)
		{
			TestFalse(TEXT("Destroying the house takes its handles with it"),
				Handle.IsValid() && !Handle->IsActorBeingDestroyed());
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// 推墙抓手画的是 TG 原版箭头（`flat_arrow` + 描边）：躺平、头朝房外、gizmo 在箭头正中
// -----------------------------------------------------------------------------
//
// 2026-09-22 起 EditHouse 的墙面标识换成 TG 矩形房子拉尺寸画的那一对（`rectangle/ui_modes/edit_dims.rs`）。
// 几何是 TG 源 json 原样换轴烘的（`Scripts/TinyGladeMakeHandleArrows.py`），这里钉的是**摆法**：
// 网格头指局部 +Y、+Z 是厚度，摆错的症状是"箭头竖着插在墙外"或"指向墙里"，而别的单测全绿。
// 六边形 + 带 yaw：外法线不沿轴，yaw=0 的矩形测不出把局部量当世界量用的错误。

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeHandleTGArrowTest,
	"PCGPlugins.ComputeShaderGenerator.House.ResizeHandleTGArrow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeHandleTGArrowTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Arrow = LoadObject<UStaticMesh>(nullptr, TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/flat_arrow.flat_arrow"));
	UStaticMesh* Outline = LoadObject<UStaticMesh>(nullptr, TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/flat_arrow_outline.flat_arrow_outline"));
	if (!TestNotNull(TEXT("TG arrow asset (run Scripts/TinyGladeMakeHandleArrows.py)"), Arrow)) return false;
	if (!TestNotNull(TEXT("TG arrow outline asset"), Outline)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr float Yaw = 37.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1000.0, 2000.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintShape = CSHouseTest_RegularPolygon(6, 1.0);
	House->FootprintSize = FVector2D(600.0, 519.6152422706632);
	House->ReevaluateSite();
	House->EnterResizeMode();

	const TArray<ACSHouseResizeHandleActor*> Handles = House->GetEdgeHandles();
	if (!TestEqual(TEXT("one arrow per hexagon edge"), Handles.Num(), 6)) return false;

	auto CheckArrow = [this](ACSHouseResizeHandleActor* Handle, const TCHAR* Stage)
	{
		UStaticMeshComponent* Body = nullptr;
		UStaticMeshComponent* Rim = nullptr;
		TArray<UStaticMeshComponent*> Components;
		Handle->GetComponents(Components);
		for (UStaticMeshComponent* Component : Components)
		{
			if (Component->GetFName() == TEXT("Arrow")) Body = Component;
			else if (Component->GetFName() == TEXT("ArrowOutline")) Rim = Component;
		}
		const int32 Edge = Handle->GetEdgeIndex();
		if (!TestNotNull(*FString::Printf(TEXT("%s edge %d: arrow body"), Stage, Edge), Body)) return;
		if (!TestNotNull(*FString::Printf(TEXT("%s edge %d: arrow outline"), Stage, Edge), Rim)) return;
		TestEqual(*FString::Printf(TEXT("%s edge %d: the body is TG's flat_arrow"), Stage, Edge),
			Body->GetStaticMesh()->GetPathName(), FString(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/flat_arrow.flat_arrow")));
		TestEqual(*FString::Printf(TEXT("%s edge %d: the rim is TG's flat_arrow_outline"), Stage, Edge),
			Rim->GetStaticMesh()->GetPathName(), FString(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/flat_arrow_outline.flat_arrow_outline")));

		const FTransform& X = Body->GetComponentTransform();
		const FVector Outer = Handle->GetOuterNormalWorld();
		TestTrue(*FString::Printf(TEXT("%s edge %d: the head (mesh +Y) points out of the house"), Stage, Edge),
			X.GetUnitAxis(EAxis::Y).Equals(Outer, 1.0e-4));
		TestTrue(*FString::Printf(TEXT("%s edge %d: the arrow lies flat (mesh +Z is up)"), Stage, Edge),
			X.GetUnitAxis(EAxis::Z).Equals(FVector::UpVector, 1.0e-4));
		TestTrue(*FString::Printf(TEXT("%s edge %d: scaled by ArrowScale"), Stage, Edge),
			X.GetScale3D().Equals(FVector(Handle->ArrowScale), 1.0e-4));
		// gizmo 就在 actor 原点 ⇒ 箭头的包围盒中心必须落在那儿。
		TestTrue(*FString::Printf(TEXT("%s edge %d: the gizmo sits in the middle of the arrow"), Stage, Edge),
			Body->Bounds.Origin.Equals(Handle->GetActorLocation(), 0.05));
		// 箭尾 → 箭头沿外法线恰好一只箭头长（TG 原大 100 cm）。
		const double Reach = FVector::DotProduct(X.TransformPosition(FVector(0.0, 100.0, 0.0)) - X.GetLocation(), Outer);
		TestEqual(*FString::Printf(TEXT("%s edge %d: tail to head is 100 cm x ArrowScale"), Stage, Edge),
			Reach, 100.0 * Handle->ArrowScale, 1.0e-2);
		TestTrue(*FString::Printf(TEXT("%s edge %d: the rim rides the body"), Stage, Edge),
			Rim->GetComponentTransform().Equals(X, 1.0e-4));
	};

	for (ACSHouseResizeHandleActor* Handle : Handles) CheckArrow(Handle, TEXT("hexagon"));

	// 推一面墙之后全体重摆，箭头照样对：推拉改的是 footprint 与房心，attach 只保相对位置。
	ACSHouseResizeHandleActor* Pushed = Handles[0];
	Pushed->SetActorLocation(Pushed->GetActorLocation() + Pushed->GetOuterNormalWorld() * 50.0);
	Pushed->ConsumeDragToHost(true);
	for (ACSHouseResizeHandleActor* Handle : House->GetEdgeHandles()) CheckArrow(Handle, TEXT("after a push"));

	// 缩放是纯观感量，下一次归位生效，gizmo 仍在箭头正中。
	Pushed->ArrowScale = 2.0f;
	Pushed->SnapToCanonical();
	CheckArrow(Pushed, TEXT("ArrowScale 2"));

	House->ExitResizeMode();
	return true;
}

// -----------------------------------------------------------------------------
// 异形房子的拉尺寸抓手（footprint 折线化 3f）：一条边一个锥子，推一条边其余墙不动
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeHandlePolylineTest,
	"PCGPlugins.ComputeShaderGenerator.House.ResizeHandlePolyline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeHandlePolylineTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr float Yaw = 37.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1000.0, 2000.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintShape = CSHouseTest_RegularPolygon(6, 1.0);
	House->FootprintSize = FVector2D(600.0, 519.6152422706632);
	House->MinFootprint = 200.0f;
	House->ReevaluateSite();

	const FCSHouseFootprint Before = House->GetFootprint();
	if (!TestEqual(TEXT("the shaped house really is a hexagon"), Before.NumEdges(), 6)) return false;

	House->EnterResizeMode();
	TestEqual(TEXT("eleven handles: six cones, the eave / base / roof frames and two ridge arrows"), House->GetResizeHandles().Num(), 11);
	TArray<ACSHouseResizeHandleActor*> Handles = House->GetEdgeHandles();
	if (!TestEqual(TEXT("one cone per hexagon edge"), Handles.Num(), 6)) return false;

	TSet<int32> Edges;
	ACSHouseResizeHandleActor* Pushed = nullptr;
	for (ACSHouseResizeHandleActor* Handle : Handles)
	{
		if (!TestNotNull(TEXT("Handle"), Handle)) return false;
		Edges.Add(Handle->GetEdgeIndex());
		if (Handle->GetEdgeIndex() == 4) Pushed = Handle;

		// 锥子在自己那面墙外皮中点再往外 HandleOffset。
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Handle->GetEdgeIndex(), Before, House->WallThickness);
		const FVector2D MidLocal = F.Start + F.U * (F.Len * 0.5f);
		const FVector WallMid = House->GetActorTransform().TransformPosition(FVector(MidLocal.X, MidLocal.Y, 0.0));
		const FVector Outer = Handle->GetOuterNormalWorld();
		TestTrue(FString::Printf(TEXT("cone %d points along its wall's outer normal"), Handle->GetEdgeIndex()),
			Outer.Equals(FRotator(0.0, Yaw, 0.0).RotateVector(FVector(-F.In.X, -F.In.Y, 0.0)), 1.0e-6));
		TestEqual(FString::Printf(TEXT("cone %d sits HandleOffset outside its wall"), Handle->GetEdgeIndex()),
			FVector::DotProduct(Handle->GetActorLocation() - WallMid, Outer), double(Handle->HandleOffset), 1.0e-2);
	}
	TestEqual(TEXT("the six cones cover six distinct edges"), Edges.Num(), 6);
	if (!TestNotNull(TEXT("cone on edge 4"), Pushed)) return false;

	TArray<FVector2D> NormalsBefore;
	TArray<double> OffsetsBefore;
	for (int32 Edge = 0; Edge < 6; ++Edge)
	{
		FVector2D N; double O = 0.0;
		CSHouseTest_WorldEdgeLine(Before, House->GetActorLocation(), Yaw, Edge, N, O);
		NormalsBefore.Add(N);
		OffsetsBefore.Add(O);
	}

	constexpr double Delta = 60.0;
	Pushed->SetActorLocation(Pushed->GetActorLocation() + Pushed->GetOuterNormalWorld() * Delta);
	const float Applied = Pushed->ConsumeDragToHost(false);
	TestEqual(TEXT("a 60 cm drag applies 60 cm"), double(Applied), Delta, 1.0e-2);
	TestEqual(TEXT("the pushed house keeps its six-vertex shape"), House->FootprintShape.Num(), 6);

	const FCSHouseFootprint After = House->GetFootprint();
	TestEqual(TEXT("the pushed footprint still has six edges"), After.NumEdges(), 6);
	for (int32 Edge = 0; Edge < FMath::Min(6, After.NumEdges()); ++Edge)
	{
		FVector2D N; double O = 0.0;
		CSHouseTest_WorldEdgeLine(After, House->GetActorLocation(), Yaw, Edge, N, O);
		const double Want = OffsetsBefore[Edge] + (Edge == 4 ? Delta : 0.0);
		TestTrue(FString::Printf(TEXT("world wall %d %s (%.4f vs %.4f)"), Edge,
			Edge == 4 ? TEXT("moves by the drag") : TEXT("stays put"), O, Want),
			N.Equals(NormalsBefore[Edge], 1.0e-6) && FMath::IsNearlyEqual(O, Want, 1.0e-2));
	}
	for (const ACSHouseResizeHandleActor* Handle : House->GetEdgeHandles())
	{
		TestTrue(FString::Printf(TEXT("cone %d is back on its canonical spot"), Handle->GetEdgeIndex()),
			Handle->GetActorLocation().Equals(Handle->ComputeCanonicalWorldLocation(), 1.0e-2));
	}

	House->ExitResizeMode();
	TestEqual(TEXT("leaving resize mode destroys every handle"), House->GetResizeHandles().Num(), 0);
	World->DestroyActor(House);
	return true;
}

// -----------------------------------------------------------------------------
// 拉尺寸模式里改边数：锥子跟着边数走（2026-09-14 用户裁决："折线有多少折手柄就有多少个"）
//
// 此前锥子只在 `EnterResizeMode` 那一刻按边数生成一次：模式开着时把矩形改成六边形，锥子还是四个，
// 五、六号边根本拖不动；反过来从六边形改回矩形，多出来的两个锥子认着不存在的边号悬在原地。
// 边数按**凸包之后**算（凹形输入按凸包的边数）。三个高度框不受影响、不许被重建。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeHandlesFollowEdgeCountTest,
	"PCGPlugins.ComputeShaderGenerator.House.ResizeHandlesFollowEdgeCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeHandlesFollowEdgeCountTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr float Yaw = 37.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1000.0, 2000.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->MinFootprint = 200.0f;
	House->ReevaluateSite();

	// 一个阶段的完整判据：边数、锥子数、N + 5、边号铺满 0..N-1、每个锥子都在自己那面墙外的规范位置上。
	auto CheckCones = [this, House](const TCHAR* Stage, int32 WantEdges)
	{
		const FCSHouseFootprint Footprint = House->GetFootprint();
		if (!TestEqual(FString::Printf(TEXT("%s: the footprint has %d edges"), Stage, WantEdges), Footprint.NumEdges(), WantEdges)) return false;
		const TArray<ACSHouseResizeHandleActor*> Cones = House->GetEdgeHandles();
		if (!TestEqual(FString::Printf(TEXT("%s: one cone per edge"), Stage), Cones.Num(), WantEdges)) return false;
		TestEqual(FString::Printf(TEXT("%s: N + 5 handles in all"), Stage), House->GetResizeHandles().Num(), WantEdges + 5);
		TestTrue(FString::Printf(TEXT("%s: still in resize mode"), Stage), House->IsInResizeMode());

		TSet<int32> Edges;
		for (const ACSHouseResizeHandleActor* Cone : Cones)
		{
			if (!TestNotNull(FString::Printf(TEXT("%s: cone"), Stage), Cone)) return false;
			Edges.Add(Cone->GetEdgeIndex());
			TestTrue(FString::Printf(TEXT("%s: cone %d knows its host"), Stage, Cone->GetEdgeIndex()), Cone->GetHost() == House);
			TestTrue(FString::Printf(TEXT("%s: cone %d sits on its canonical spot"), Stage, Cone->GetEdgeIndex()),
				Cone->GetActorLocation().Equals(Cone->ComputeCanonicalWorldLocation(), 1.0e-2));
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Cone->GetEdgeIndex(), Footprint, House->WallThickness);
			TestTrue(FString::Printf(TEXT("%s: cone %d points along a real wall"), Stage, Cone->GetEdgeIndex()), F.Len > 0.0f);
		}
		bool bCovered = Edges.Num() == WantEdges;
		for (int32 Edge = 0; Edge < WantEdges; ++Edge) bCovered &= Edges.Contains(Edge);
		return TestTrue(FString::Printf(TEXT("%s: the cones cover edges 0..%d exactly once"), Stage, WantEdges - 1), bCovered);
	};

	House->EnterResizeMode();
	if (!CheckCones(TEXT("rectangle"), 4)) return false;
	const ACSHouseHeightHandleActor* Eave = House->GetHeightHandle();
	const ACSHouseHeightHandleActor* Base = House->GetBaseHandle();
	if (!TestNotNull(TEXT("eave frame"), Eave) || !TestNotNull(TEXT("base frame"), Base)) return false;
	const TArray<ACSHouseResizeHandleActor*> RectCones = House->GetEdgeHandles();

	// ① 模式开着把矩形改成六边形（细节面板改属性 = PostEditChange → 构造脚本 → 重求值）⇒ 六个锥子。
	House->FootprintShape = CSHouseTest_RegularPolygon(6, 1.0);
	House->FootprintSize = FVector2D(600.0, 519.6152422706632);
	House->PostEditChange();
	if (!CheckCones(TEXT("hexagon"), 6)) return false;
	{
		// 复用而不是整组重建：原来那四个都还在（被选中的锥子因此不会因为改形状而被销毁、把模式退掉）。
		int32 Reused = 0;
		const TArray<ACSHouseResizeHandleActor*> Now = House->GetEdgeHandles();
		for (ACSHouseResizeHandleActor* Cone : RectCones) if (IsValid(Cone) && Now.Contains(Cone)) ++Reused;
		TestEqual(TEXT("growing the edge count keeps every existing cone"), Reused, 4);
		TestTrue(TEXT("the two frames are untouched by a shape change"), House->GetHeightHandle() == Eave && House->GetBaseHandle() == Base);
	}

	// ② 凹形（L）⇒ 按**凸包**的 5 条边；属性本身不改写。
	House->FootprintShape = { FVector2D(0, 0), FVector2D(2, 0), FVector2D(2, 1), FVector2D(1, 1), FVector2D(1, 2), FVector2D(0, 2) };
	House->FootprintSize = FVector2D(600.0, 600.0);
	House->PostEditChange();
	if (!CheckCones(TEXT("concave L used as its hull"), 5)) return false;
	TestEqual(TEXT("the concave shape property is not rewritten to its hull"), House->FootprintShape.Num(), 6);

	// ③ 形状清空回到矩形 ⇒ 四个；多出来的那个被销毁（边号越界的锥子不许留着）。
	{
		TArray<TWeakObjectPtr<ACSHouseResizeHandleActor>> Before;
		for (ACSHouseResizeHandleActor* Cone : House->GetEdgeHandles()) Before.Add(Cone);
		House->FootprintShape.Reset();
		House->FootprintSize = FVector2D(600.0, 400.0);
		House->PostEditChange();
		if (!CheckCones(TEXT("back to the rectangle"), 4)) return false;
		int32 Gone = 0;
		for (const TWeakObjectPtr<ACSHouseResizeHandleActor>& Cone : Before) if (!Cone.IsValid() || Cone->IsActorBeingDestroyed()) ++Gone;
		TestEqual(TEXT("shrinking from five to four edges destroys exactly one cone"), Gone, 1);
	}

	// ④ 直接走重求值（撤销也落到这里：PostEditUndo → PostEditChange / ReevaluateSite）同样对齐。
	House->FootprintShape = CSHouseTest_RegularPolygon(5, 1.0);
	House->ReevaluateSite();
	if (!CheckCones(TEXT("pentagon via a bare reevaluate"), 5)) return false;

	// ⑤ 推一条边不改边数 ⇒ 锥子一个都不许换（稳态零动作；被拖的那个在拖动途中绝不能被销毁）。
	{
		const TArray<ACSHouseResizeHandleActor*> Stable = House->GetEdgeHandles();
		ACSHouseResizeHandleActor* Pushed = nullptr;
		for (ACSHouseResizeHandleActor* Cone : Stable) if (Cone->GetEdgeIndex() == 2) Pushed = Cone;
		if (!TestNotNull(TEXT("cone on edge 2"), Pushed)) return false;
		Pushed->SetActorLocation(Pushed->GetActorLocation() + Pushed->GetOuterNormalWorld() * 30.0);
		TestEqual(TEXT("a 30 cm push on the pentagon applies 30 cm"), double(Pushed->ConsumeDragToHost(false)), 30.0, 1.0e-2);
		const TArray<ACSHouseResizeHandleActor*> After = House->GetEdgeHandles();
		bool bSame = After.Num() == Stable.Num();
		for (ACSHouseResizeHandleActor* Cone : Stable) bSame &= After.Contains(Cone);
		TestTrue(TEXT("pushing an edge keeps the very same cones"), bSame);
		CheckCones(TEXT("pentagon after a push"), 5);
	}

	// ⑥ 不在模式里改形状不许生出抓手。
	House->ExitResizeMode();
	House->FootprintShape = CSHouseTest_RegularPolygon(6, 1.0);
	House->ReevaluateSite();
	TestEqual(TEXT("outside resize mode a shape change spawns no handles"), House->GetResizeHandles().Num(), 0);

	World->DestroyActor(House);
	return true;
}

// -----------------------------------------------------------------------------
// 异形房子的接缝（footprint 折线化 3d 的 actor 接线）：4 号及以后的边也要被裁
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamPolylineActorTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamPolylineActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamPolylineActorTest::RunTest(const FString& Parameters)
{
	// 3d 只改了纯函数、actor 那一侧的逐边循环当时还写着 `Edge < 4`：纯函数单测全绿，
	// 六边形房子的 4、5 号边却永远不被接缝裁剪。这条从 actor 读回裁剪段数，专钉那一层接线。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 六边形（半径 300，0 号顶点在 +X）：4 号边是底边 y = −259.8、x ∈ [−150, 150]。
	ACSHouseActor* Hex = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	// 矩形邻居只咬住底边中段：x ∈ [−120, 120]、y ∈ [−430, −230]，碰不到 3、5 号斜边。
	ACSHouseActor* Box = World->SpawnActor<ACSHouseActor>(FVector(0.0, -330.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Hexagon house"), Hex) || !TestNotNull(TEXT("Box house"), Box)) return false;
	for (ACSHouseActor* House : { Hex, Box })
	{
		House->Windows.Reset();
		House->bSeamEnabled = true;
	}
	Hex->FootprintShape = CSHouseTest_RegularPolygon(6, 1.0);
	Hex->FootprintSize = FVector2D(600.0, 519.6152422706632);
	Box->FootprintSize = FVector2D(240.0, 200.0);
	Hex->ReevaluateSite();
	Box->ReevaluateSite();
	Hex->ReevaluateSite();

	if (!TestTrue(TEXT("both houses have an identity"), Hex->GetHouseId().IsValid() && Box->GetHouseId().IsValid())) return false;
	TestEqual(TEXT("the hexagon is cut on exactly one wall (its edge 4)"), Hex->GetSeamCutCount(), 1);
	TestTrue(FString::Printf(TEXT("the box is cut where it enters the hexagon (%d)"), Box->GetSeamCutCount()),
		Box->GetSeamCutCount() >= 1);
	TestTrue(TEXT("and the two contours cross"), Hex->GetSeamCornerCount() >= 2);

	World->DestroyActor(Box);
	World->DestroyActor(Hex);
	return true;
}

// -----------------------------------------------------------------------------
// 高度抓手（D5 的第二个自由度）：上下拖那个"窗框"改墙高
//
// 与 `House.ResizeHandle` 分开：那条钉的是**水平**推拉那套父子回路的数学，这条钉的是
// 竖直这一路 —— 框的几何、只吃 Z 分量、MinWallHeight 下限，以及"改高度之后四个水平锥子
// 也要跟着抬"这条跨抓手的联动。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseHeightHandleTest,
	"PCGPlugins.ComputeShaderGenerator.House.HeightHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseHeightHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr float Yaw = 37.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(1000.0, 2000.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->WallHeight = 300.0f;
	House->MinWallHeight = 200.0f;

	House->EnterResizeMode();
	ACSHouseHeightHandleActor* Frame = House->GetHeightHandle();
	if (!TestNotNull(TEXT("Height handle"), Frame)) return false;

	// ---- ① 框的几何：四条边、大小 = footprint × FrameScale、首尾相接 ----
	{
		TArray<USceneComponent*> Bars;
		Frame->GetComponents<USceneComponent>(Bars);
		int32 MeshBars = 0;
		for (const USceneComponent* C : Bars)
		{
			if (C->IsA<UStaticMeshComponent>()) ++MeshBars;
		}
		TestEqual(TEXT("The frame is made of four bars"), MeshBars, 4);
	}

	// 框的四条边各自离房心多远：应当恰好是 footprint/2 × FrameScale。
	// 用世界量反算，顺带把"房子带 yaw 时框也跟着转"一起验掉。
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FVector2D OuterLocal = CSHouseResize_EdgeOuterLocal(Edge);
		const double Dim = CSHouseResize_EdgeDrivesX(Edge) ? House->FootprintSize.X : House->FootprintSize.Y;
		const double Expected = Dim * 0.5 * double(Frame->FrameScale);

		const FVector BarLocal(OuterLocal.X * Expected, OuterLocal.Y * Expected, 0.0);
		const FVector BarWorld = Frame->GetActorTransform().TransformPosition(BarLocal);
		const FVector Outer = CSHouseResize_EdgeOuterWorld(Edge, Yaw);
		const double Out = FVector::DotProduct(BarWorld - House->GetActorLocation(), Outer);
		TestEqual(
			FString::Printf(TEXT("Frame edge %d sits at footprint/2 x FrameScale"), Edge),
			Out, Expected, 1.0e-2);
	}

	// ---- ② 上下拖 = 改墙高，且 1:1 ----
	{
		constexpr double Rise = 80.0;
		const float Before = House->WallHeight;
		Frame->SetActorLocation(Frame->GetActorLocation() + FVector(0.0, 0.0, Rise));
		const float Applied = Frame->ConsumeDragToHost(false);

		TestEqual(TEXT("Dragging up 80 raises the wall by 80"), double(Applied), Rise, 1.0e-2);
		TestEqual(TEXT("The wall height follows the drag"), double(House->WallHeight), double(Before) + Rise, 1.0e-2);
		// 框回到新的檐口上 —— 它同时是墙高的读数。
		const double LocalZ = House->GetActorTransform().InverseTransformPosition(Frame->GetActorLocation()).Z;
		TestEqual(TEXT("The frame rides back up to the new eave"), LocalZ, double(House->WallHeight), 1.0e-2);
	}

	// ---- ③ 水平分量不改高度（把框拖歪不该有任何效果）----
	{
		const float Before = House->WallHeight;
		Frame->SetActorLocation(Frame->GetActorLocation() + FVector(250.0, -180.0, 0.0));
		const float Applied = Frame->ConsumeDragToHost(false);
		TestEqual(TEXT("A purely horizontal drag applies nothing"), double(Applied), 0.0, 1.0e-2);
		TestEqual(TEXT("A purely horizontal drag leaves the wall height alone"),
			double(House->WallHeight), double(Before), 1.0e-2);
	}

	// ---- ④ 连续 10 步都恰好走一步：记账量法在竖直这一路同样成立 ----
	{
		constexpr double Step = 12.0;
		constexpr int32 Steps = 10;
		const double Before = House->WallHeight;
		int32 Jumps = 0;
		for (int32 i = 0; i < Steps; ++i)
		{
			Frame->SetActorLocation(Frame->GetActorLocation() + FVector(0.0, 0.0, Step));
			if (FMath::Abs(double(Frame->ConsumeDragToHost(false)) - Step) > 1.0e-2) ++Jumps;
		}
		TestEqual(TEXT("Every vertical step applies exactly the offset"), Jumps, 0);
		TestEqual(TEXT("Ten steps raise the wall by ten steps"),
			double(House->WallHeight) - Before, Steps * Step, 1.0e-2);
	}

	// ---- ⑤ MinWallHeight 是硬下界，且不攒残差 ----
	{
		const double Before = House->WallHeight;
		Frame->SetActorLocation(Frame->GetActorLocation() - FVector(0.0, 0.0, 10000.0));
		const float Applied = Frame->ConsumeDragToHost(false);
		TestEqual(TEXT("The height floor holds"), double(House->WallHeight), double(House->MinWallHeight), 1.0e-2);
		TestEqual(TEXT("Squashing past the floor applies only what was possible"),
			double(Applied), double(House->MinWallHeight) - Before, 1.0e-2);

		// 再往下拖一大截：一步都不许再生效。
		Frame->SetActorLocation(Frame->GetActorLocation() - FVector(0.0, 0.0, 5000.0));
		TestEqual(TEXT("Dragging further past the floor applies nothing"),
			double(Frame->ConsumeDragToHost(false)), 0.0, 1.0e-2);

		// 往上拖 40：必须**恰好**长 40，不许把刚才那 15000 的残差一起吐出来。
		Frame->SetActorLocation(Frame->GetActorLocation() + FVector(0.0, 0.0, 40.0));
		TestEqual(TEXT("Pulling back up applies exactly the drag, not the swallowed residue"),
			double(Frame->ConsumeDragToHost(false)), 40.0, 1.0e-2);
	}

	// ---- ⑥ 跨抓手联动：改墙高之后四个水平锥子也要跟着抬 ----
	// 锥子挂在 `WallHeight × HandleHeightFraction` 上。不联动的话它们会留在旧高度，
	// 画面上是"房子长高了，四个锥子还在半腰"。
	{
		Frame->SetActorLocation(Frame->GetActorLocation() + FVector(0.0, 0.0, 150.0));
		Frame->ConsumeDragToHost(true);

		for (const ACSHouseResizeHandleActor* Cone : House->GetEdgeHandles())
		{
			const double LocalZ = House->GetActorTransform().InverseTransformPosition(Cone->GetActorLocation()).Z;
			TestEqual(
				FString::Printf(TEXT("Cone %d rides the new wall height"), Cone->GetEdgeIndex()),
				LocalZ, double(House->WallHeight * Cone->HandleHeightFraction), 1.0e-2);
		}
	}

	return true;
}

// -----------------------------------------------------------------------------
// 房底框（2026-09-14 用户裁决「底动顶不动」）：上下拖房底 = HeightOffset 与 WallHeight 反向同改
//
// 与 `House.HeightHandle` 的区别只在一件事上，而那件事正是这条用例存在的理由：**房底框会动 actor 变换**。
// 改 HeightOffset ⇒ 重求值落座 `SetActorLocation` ⇒ attach 在房子下的框被父级一起带走 —— 拉尺寸锥子那条
// 2× 回路的竖直版。所以逐步钉"拖 δ 房底恰好走 δ"，以及檐口世界高度全程不动。
//
// 要一块地：落座公式 `房底 Z = max(地面) + HeightOffset` 没有地面时不起作用（房子原地不动），
// 那样下面每一条"檐口不动"都是恒真的。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseBaseHandleTest,
	"PCGPlugins.ComputeShaderGenerator.House.BaseHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseBaseHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 平地。岩壳 / 石阶 / 地被关掉：本条只要一个高度场，多几条 GPU 路只会多耗时与噪声。
	ACSGroundActor* Ground = World->SpawnActorDeferred<ACSGroundActor>(ACSGroundActor::StaticClass(), FTransform::Identity);
	if (!TestNotNull(TEXT("Ground"), Ground)) return false;
	Ground->bRockShell = false;
	Ground->StairMesh = nullptr;
	Ground->bGroundCoverEnabled = false;
	Ground->FinishSpawning(FTransform::Identity);

	constexpr float Yaw = 37.0f;
	const FVector Centre(800.0, 800.0, 0.0);
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(Centre, FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->Ground = Ground;
	House->FootprintSize = FVector2D(600.0, 400.0);
	// 墙高 400、下限 200：②③ 一共抬 100 之后，④ 顶墙高下限时还剩 100 的余量 —— 否则 ④ 是在已经顶住的
	// 状态下测"只给可能的那一截"，生效量恒为 0，那条断言就成了空话。
	House->WallHeight = 400.0f;
	House->MinWallHeight = 200.0f;
	House->HeightOffset = 0.0f;
	House->ReevaluateSite();

	auto BaseZ = [House]() { return House->GetActorLocation().Z; };
	auto EaveZ = [House]() { return House->GetActorLocation().Z + double(House->WallHeight); };
	const double GroundZ = Ground->SampleHeight(FVector2D(Centre.X, Centre.Y));
	if (!TestEqual(TEXT("the house starts seated on the ground"), BaseZ(), GroundZ, 1.0e-2)) return false;
	const double Eave0 = EaveZ();

	House->EnterResizeMode();
	ACSHouseHeightHandleActor* Base = House->GetBaseHandle();
	ACSHouseHeightHandleActor* Top = House->GetHeightHandle();
	if (!TestNotNull(TEXT("base frame"), Base) || !TestNotNull(TEXT("eave frame"), Top)) return false;
	TestTrue(TEXT("the base frame knows which end it is"), Base->GetSide() == ECSHouseHeightHandleSide::Base);
	TestTrue(TEXT("the eave frame knows which end it is"), Top->GetSide() == ECSHouseHeightHandleSide::Eave);

	// ---- ① 规范位置：房心正下方的房底（局部 Z = 0）；四根条子与檐口框同一副框 ----
	{
		const FVector Local = House->GetActorTransform().InverseTransformPosition(Base->GetActorLocation());
		TestTrue(TEXT("the base frame sits under the house centre"),
			FMath::IsNearlyZero(Local.X, 1.0e-2) && FMath::IsNearlyZero(Local.Y, 1.0e-2));
		TestEqual(TEXT("the base frame sits at the base"), Local.Z, 0.0, 1.0e-2);
		TArray<UStaticMeshComponent*> Bars;
		Base->GetComponents<UStaticMeshComponent>(Bars);
		TestEqual(TEXT("the base frame is made of four bars too"), Bars.Num(), 4);
	}
	const FVector TopWorld0 = Top->GetActorLocation();

	// ---- ② 上拖 50：墙矮 50、房子离地高 50、檐口与檐口框的世界位置不动，一次到位 ----
	{
		Base->SetActorLocation(Base->GetActorLocation() + FVector(0.0, 0.0, 50.0));
		const float Applied = Base->ConsumeDragToHost(false);
		TestEqual(TEXT("dragging the base up 50 applies 50"), double(Applied), 50.0, 1.0e-2);
		TestEqual(TEXT("HeightOffset follows the drag"), double(House->HeightOffset), 50.0, 1.0e-2);
		TestEqual(TEXT("the wall gets exactly that much shorter"), double(House->WallHeight), 350.0, 1.0e-2);
		TestEqual(TEXT("the house lifts off the ground by the drag"), BaseZ(), GroundZ + 50.0, 1.0e-2);
		TestEqual(TEXT("the eave stays put in the world"), EaveZ(), Eave0, 1.0e-2);
		TestTrue(TEXT("the eave frame stays put in the world"), Top->GetActorLocation().Equals(TopWorld0, 1.0e-2));
		TestTrue(TEXT("the base frame is back on its canonical spot"),
			Base->GetActorLocation().Equals(Base->ComputeCanonicalWorldLocation(), 1.0e-2));
	}

	// ---- ③ 连续 10 步 +5：每步恰好 5、房底共走 50 —— 父级带着框走的那一截不许被算成用户拖的 ----
	{
		const double Before = BaseZ();
		int32 Jumps = 0;
		for (int32 Step = 0; Step < 10; ++Step)
		{
			Base->SetActorLocation(Base->GetActorLocation() + FVector(0.0, 0.0, 5.0));
			if (FMath::Abs(double(Base->ConsumeDragToHost(false)) - 5.0) > 1.0e-2) ++Jumps;
		}
		TestEqual(TEXT("every base step applies exactly the drag"), Jumps, 0);
		TestEqual(TEXT("ten steps lift the base by ten steps, not twice that"), BaseZ() - Before, 50.0, 1.0e-2);
		TestEqual(TEXT("and the eave still has not moved"), EaveZ(), Eave0, 1.0e-2);
		// 锥子挂在 `WallHeight × HandleHeightFraction` 上：墙变矮、房底抬高之后它们也要跟着重摆。
		for (const ACSHouseResizeHandleActor* Cone : House->GetEdgeHandles())
		{
			const double LocalZ = House->GetActorTransform().InverseTransformPosition(Cone->GetActorLocation()).Z;
			TestEqual(FString::Printf(TEXT("cone %d rides the new wall"), Cone->GetEdgeIndex()),
				LocalZ, double(House->WallHeight * Cone->HandleHeightFraction), 1.0e-2);
		}
	}

	// ---- ④ 往上顶在 MinWallHeight：只给可能的那一截，再往上一步都不给，拉回来恰好走拉回的量 ----
	{
		const double WallBefore = House->WallHeight;
		if (!TestTrue(TEXT("the clamp step starts with headroom above the wall floor"), WallBefore > double(House->MinWallHeight) + 1.0)) return false;
		Base->SetActorLocation(Base->GetActorLocation() + FVector(0.0, 0.0, 10000.0));
		const float Applied = Base->ConsumeDragToHost(false);
		TestEqual(TEXT("the wall floor holds"), double(House->WallHeight), double(House->MinWallHeight), 1.0e-2);
		TestEqual(TEXT("raising past the wall floor applies only what was possible"),
			double(Applied), WallBefore - double(House->MinWallHeight), 1.0e-2);
		Base->SetActorLocation(Base->GetActorLocation() + FVector(0.0, 0.0, 5000.0));
		TestEqual(TEXT("raising further past the floor applies nothing"), double(Base->ConsumeDragToHost(false)), 0.0, 1.0e-2);
		Base->SetActorLocation(Base->GetActorLocation() - FVector(0.0, 0.0, 20.0));
		TestEqual(TEXT("lowering back applies exactly the drag, not the swallowed residue"),
			double(Base->ConsumeDragToHost(false)), -20.0, 1.0e-2);
		TestEqual(TEXT("the eave has not moved through the clamp"), EaveZ(), Eave0, 1.0e-2);
	}

	// ---- ⑤ 往下顶在贴地（HeightOffset = 0）：墙变高、房底落回地面，再往下一步都不给 ----
	{
		const double OffsetBefore = House->HeightOffset;
		Base->SetActorLocation(Base->GetActorLocation() - FVector(0.0, 0.0, 10000.0));
		const float Applied = Base->ConsumeDragToHost(true);   // 顺带走一次松手
		TestEqual(TEXT("lowering past the ground applies only what was possible"), double(Applied), -OffsetBefore, 1.0e-2);
		TestEqual(TEXT("the base comes to rest at HeightOffset 0"), double(House->HeightOffset), 0.0, 1.0e-2);
		TestEqual(TEXT("the house is seated on the ground again"), BaseZ(), GroundZ, 1.0e-2);
		TestEqual(TEXT("the wall took the whole difference"), EaveZ(), Eave0, 1.0e-2);
		Base->SetActorLocation(Base->GetActorLocation() - FVector(0.0, 0.0, 5000.0));
		TestEqual(TEXT("lowering further past the ground applies nothing"), double(Base->ConsumeDragToHost(false)), 0.0, 1.0e-2);
		Base->SetActorLocation(Base->GetActorLocation() + FVector(0.0, 0.0, 30.0));
		TestEqual(TEXT("raising again applies exactly the drag"), double(Base->ConsumeDragToHost(false)), 30.0, 1.0e-2);
	}

	// ---- ⑥ 旧存档里 HeightOffset 已经是负数：以当前值为界，**不许一抓就跳到 0** ----
	House->ExitResizeMode();
	House->HeightOffset = -30.0f;
	House->WallHeight = 300.0f;
	House->ReevaluateSite();
	{
		const double Eave = EaveZ();
		TestEqual(TEXT("legacy negative offset: a zero push applies zero"), double(House->PushBase(0.0f)), 0.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: grabbing does not jump it to zero"), double(House->HeightOffset), -30.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: pushing further down is refused"), double(House->PushBase(-10.0f)), 0.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: and leaves it where it was"), double(House->HeightOffset), -30.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: pushing up applies"), double(House->PushBase(10.0f)), 10.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: HeightOffset -30 -> -20"), double(House->HeightOffset), -20.0, 1.0e-4);
		TestEqual(TEXT("legacy negative offset: the eave does not move"), EaveZ(), Eave, 1.0e-2);
	}

	// ---- ⑦ 已经矮于 MinWallHeight 的旧存档：往上一步都不给（不许再矮），往下照常 ----
	House->HeightOffset = 40.0f;
	House->WallHeight = 150.0f;
	House->ReevaluateSite();
	TestEqual(TEXT("a wall already under the floor cannot be made shorter"), double(House->PushBase(5.0f)), 0.0, 1.0e-4);
	TestEqual(TEXT("but it can be made taller"), double(House->PushBase(-5.0f)), -5.0, 1.0e-4);

	World->DestroyActor(House);
	World->DestroyActor(Ground);
	return true;
}


// -----------------------------------------------------------------------------
// 合批（2026-09-10）：一帧里动 N 个窗标记 ⇒ 房子只重求值一次，与 N 无关
//
// 合批之前，每个标记的每一次 `RegisterFeatureMarker` 都同步走一趟完整 `ReevaluateSite`：
// 算门（约 340 次镜像双线性）、接缝、砖、藤、瓦、尖顶、门扇、摆件各跑一遍，收尾的
// `NotifyMarkersRebuilt` 还要对**每个**标记吸附一次 ⇒ 一帧 N 次重建 + N² 次吸附。
// 而前 N−1 次的结果全被后一次盖掉：第 k 次跑的时候，第 k+1..N 个标记这一帧的新诉求还没写进来。
//
// ⚠️ 判据只能是**次数**。合批坏掉的症状是"画面全对，就是卡" —— 几何有别的用例守着，
// 而次数没人数的话，这条纪律迟早被某个"顺手改成直接调 ReevaluateSite"的补丁悄悄拆掉。
// 第二条断言（诉求一个都没丢）钉的是另一半：合批不许把最后那次唤醒吞掉。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseMarkerDragBatchTest,
	"PCGPlugins.ComputeShaderGenerator.House.MarkerDragBatchesRebuilds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseMarkerDragBatchTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	// 同 `House.WindowMarker`：把道路驱动的门摘干净，本条只测标记这一环。
	House->Windows.Reset();

	// 三扇窗摆在南墙上，彼此隔开、离转角也远（`NearCorner` 会把它们判掉，那样测的就不是合批了）。
	constexpr int32 MarkerCount = 3;
	const double HalfY = House->FootprintSize.Y * 0.5;
	const double SpanX = House->FootprintSize.X;

	TArray<ACSWindowMarker*> Markers;
	for (int32 Index = 0; Index < MarkerCount; ++Index)
	{
		ACSWindowMarker* One = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (!TestNotNull(TEXT("Window marker"), One)) return false;
		// 脚本是"先 spawn 后摆位"，spawn 那一瞬间在原点、找不到宿主 —— 开着自毁会当场自删。
		One->bDestroyWhenHostless = false;
		const double X = SpanX * (0.25 + 0.25 * double(Index)) - SpanX * 0.5;
		One->SetActorLocation(FVector(X, -HalfY - 100.0, 150.0));
		One->SetActorRotation(FRotator(0.0, 90.0, 0.0));   // +X 指向 +Y = 面向南墙
		One->ResolveHostAndRegister(true);
		Markers.Add(One);
	}
	if (!TestEqual(TEXT("all three windows landed"), House->GetWindowCount(), MarkerCount)) return false;

	// ---- 这里开始就是"同一帧里拖着三个窗走"----
	//
	// 单测整段跑在同一个 `GFrameCounter` 里，所以这正是 gizmo 多选拖动那一帧的形状：
	// 三个标记各自 tick、各自 `RegisterFeatureMarker`。
	// 移动前的洞心弧长：下面要拿它证明这一轮的诉求**真的变了** —— 诉求没变的话
	// `RegisterFeatureMarker` 自己就早退了，那样测出来的"0 次重建"是假绿。
	auto FirstWindowS = [House]() -> float
	{
		for (const FCSWallOpening& O : House->GetCurrentOpenings())
		{
			if (O.Type == ECSOpeningType::Window) return O.CenterS;
		}
		return -1.0f;
	};
	const float BeforeS = FirstWindowS();

	const int64 Before = House->GetReevaluateCount();
	for (int32 Index = 0; Index < MarkerCount; ++Index)
	{
		// 真的挪一段（20 cm）：诉求没变的话 `RegisterFeatureMarker` 自己就早退了，
		// 那样测出来的 0 次是假的。
		Markers[Index]->SetActorLocation(Markers[Index]->GetActorLocation() + FVector(20.0, 0.0, 0.0));
		Markers[Index]->ResolveHostAndRegister(true);
	}
	const int64 AfterMoves = House->GetReevaluateCount();

	// ① 通知本身**一次都不重建**：只标脏。单测整段在同一帧里，房子的 Tick 插不进来，
	//    所以这里必须是 0，不是"≤ 1"。
	TestEqual(
		FString::Printf(TEXT("moving %d markers only marks the house dirty (rebuilds=%lld)"),
			MarkerCount, AfterMoves - Before),
		AfterMoves - Before, int64(0));

	// ② 兑现点之一：房子自己的 Tick。手推一帧编辑器 world（`LEVELTICK_ViewportsOnly` 正是编辑器的
	//    tick 类型）。这条钉的是"编辑器里没人读也会更新"：构造里没开 `bCanEverTick`、没 override
	//    `ShouldTickIfViewportsOnly`、或 Tick 里忘了兑现，编辑器画面都会停在旧洞上 —— 而下面每一条
	//    "读"的断言照样绿，因为读会补票。
	//
	//    ⚠️ 每次 World->Tick 前都要推进 GFrameCounter：引擎一帧里同一个 tick 函数只执行一次
	//    （`TickVisitedGFrameCounter`），单测整段又在同一帧里。不推的话第二次 World->Tick 根本不会
	//    执行房子的 tick —— 下面"之后不再重建"那条就是假绿，房子忘了关 tick 也照样过。
	TestTrue(TEXT("marking dirty switched the house's tick on"), House->IsActorTickEnabled());
	++GFrameCounter;
	World->Tick(LEVELTICK_ViewportsOnly, 1.0f / 30.0f);
	const int64 AfterTick = House->GetReevaluateCount();
	TestEqual(TEXT("the house's own tick pays the debt exactly once"), AfterTick - AfterMoves, int64(1));
	TestFalse(TEXT("and then switches its tick off (an idle house costs nothing per frame)"), House->IsActorTickEnabled());

	++GFrameCounter;
	World->Tick(LEVELTICK_ViewportsOnly, 1.0f / 30.0f);
	TestEqual(TEXT("a following frame does not rebuild again"), House->GetReevaluateCount() - AfterTick, int64(0));

	// ③ 结果对：三个诉求一个没丢，洞心跟着挪了。已经兑现过，读不许再补一次。
	TestEqual(TEXT("not one demand was dropped"), House->GetWindowCount(), MarkerCount);
	const float AfterS = FirstWindowS();
	TestTrue(
		FString::Printf(TEXT("the batched rebuild really applied the move (S %.2f -> %.2f)"), BeforeS, AfterS),
		BeforeS >= 0.0f && AfterS >= 0.0f && !FMath::IsNearlyEqual(BeforeS, AfterS, 1.0f));
	TestEqual(TEXT("reads after the tick cost nothing"), House->GetReevaluateCount() - AfterTick, int64(0));

	// ④ 兑现点之二：没等到 Tick 就读 —— 读前补票，恰好一次。
	for (int32 Index = 0; Index < MarkerCount; ++Index)
	{
		Markers[Index]->SetActorLocation(Markers[Index]->GetActorLocation() + FVector(20.0, 0.0, 0.0));
		Markers[Index]->ResolveHostAndRegister(true);
	}
	const int64 BeforeRead = House->GetReevaluateCount();
	const int32 WindowsAfterRead = House->GetWindowCount();
	TestEqual(TEXT("a read before the tick pays exactly one rebuild"),
		House->GetReevaluateCount() - BeforeRead, int64(1));
	TestEqual(TEXT("and sees every demand"), WindowsAfterRead, MarkerCount);

	for (ACSWindowMarker* One : Markers) World->DestroyActor(One);
	return true;
}

// -----------------------------------------------------------------------------
// footprint 折线化（3a-2）：转角斜接 —— 面板沿角平分线相接，凸凹两种角都成立
// -----------------------------------------------------------------------------

namespace
{
/**
 * 斜接之前 `CSHouse_GetEdge` 的**原样冻结副本**（直角对接：偶数边吃下转角方块、奇数边两端各缩
 * `T`）。只服务一条关系断言：新旧两套框架描述的是**同一面外墙**，差别只在转角方块归谁 ——
 * 这正是 `FCSWallAnchor::SConvention` 那条「奇数边距离 + T」换算的依据。
 */
FCSHouseEdgeFrame CSHouseTest_ButtJointGetEdge(int32 EdgeIndex, const FVector2D& Footprint, float T)
{
	const double HX = Footprint.X * 0.5, HY = Footprint.Y * 0.5;
	FCSHouseEdgeFrame F;
	switch (EdgeIndex & 3)
	{
	case 0: F.Start = { -HX, -HY };     F.U = { 1, 0 };  F.In = { 0, 1 };  F.Len = float(Footprint.X); break;
	case 1: F.Start = { HX, -HY + T };  F.U = { 0, 1 };  F.In = { -1, 0 }; F.Len = float(Footprint.Y) - 2 * T; break;
	case 2: F.Start = { HX, HY };       F.U = { -1, 0 }; F.In = { 0, -1 }; F.Len = float(Footprint.X); break;
	default:F.Start = { -HX, HY - T };  F.U = { 0, -1 }; F.In = { 1, 0 };  F.Len = float(Footprint.Y) - 2 * T; break;
	}
	return F;
}

/** 逐位相等：墙长、起点、让出量都进哈希，1 ULP 的漂移就会让幂等短路失效。 */
bool CSHouseTest_FramesIdentical(const FCSHouseEdgeFrame& A, const FCSHouseEdgeFrame& B)
{
	return A.Start.X == B.Start.X && A.Start.Y == B.Start.Y
		&& A.U.X == B.U.X && A.U.Y == B.U.Y
		&& A.In.X == B.In.X && A.In.Y == B.In.Y
		&& A.Len == B.Len && A.InsetStart == B.InsetStart && A.InsetEnd == B.InsetEnd;
}

/** 内皮上的端点（深度 T 处的斜接点）。 */
FVector2D CSHouseTest_InnerStart(const FCSHouseEdgeFrame& F, float T)
{
	return F.Start + F.U * double(F.InsetStart) + F.In * double(T);
}
FVector2D CSHouseTest_InnerEnd(const FCSHouseEdgeFrame& F, float T)
{
	return F.Start + F.U * double(F.Len - F.InsetEnd) + F.In * double(T);
}

/** 简单多边形的有向面积（逆时针为正）。 */
double CSHouseTest_PolygonArea(const TArray<FVector2D>& P)
{
	double Twice = 0.0;
	for (int32 i = 0; i < P.Num(); ++i)
	{
		const FVector2D& A = P[i];
		const FVector2D& B = P[(i + 1) % P.Num()];
		Twice += A.X * B.Y - A.Y * B.X;
	}
	return Twice * 0.5;
}

/**
 * 三角汤的体积（散度定理：Σ p0·(p1×p2) / 6）。每块面板都是闭合的六面体，所以对整锅汤求和
 * 就是所有面板体积之和 —— 面板之间有任何重叠，这个数就会比实心环大；有缝就会小。
 */
double CSHouseTest_SoupVolume(const FCSGpuMeshCPUData& Soup)
{
	double Six = 0.0;
	for (int32 i = 0; i + 2 < Soup.Indices.Num(); i += 3)
	{
		const FVector P0(Soup.Positions[Soup.Indices[i]]);
		const FVector P1(Soup.Positions[Soup.Indices[i + 1]]);
		const FVector P2(Soup.Positions[Soup.Indices[i + 2]]);
		Six += FVector::DotProduct(P0, FVector::CrossProduct(P1, P2));
	}
	return FMath::Abs(Six) / 6.0;
}
}   // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseFootprintMitreCornersTest,
	"PCGPlugins.ComputeShaderGenerator.House.FootprintMitreCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseFootprintMitreCornersTest::RunTest(const FString& Parameters)
{
	// ---- ① 矩形：外角点起、外皮全长、两端各让一个墙厚；矩形重载与折线重载逐位相同 ----
	const FVector2D Sizes[] = { { 600.0, 400.0 }, { 400.0, 600.0 }, { 500.0, 500.0 }, { 1000.0, 210.0 }, { 233.7, 417.3 } };
	const float Thicknesses[] = { 24.0f, 1.0f, 60.0f, 12.5f };
	for (const FVector2D& Size : Sizes)
	{
		for (const float T : Thicknesses)
		{
			const FCSHouseFootprint FP = FCSHouseFootprint::MakeRect(Size);
			for (int32 Edge = 0; Edge < 4; ++Edge)
			{
				const FCSHouseEdgeFrame Rect = CSHouse_GetEdge(Edge, Size, T);
				const FCSHouseEdgeFrame Poly = CSHouse_GetEdge(Edge, FP, T);
				const FString Tag = FString::Printf(TEXT("size %s, T %.2f, edge %d"), *Size.ToString(), T, Edge);

				TestTrue(*FString::Printf(TEXT("Rect and polyline overloads are bit-identical (%s)"), *Tag),
					CSHouseTest_FramesIdentical(Rect, Poly));
				// 直角的半角公式 sin/(1+cos) = 1/1 —— 让出量必须**恰好**是 T，不是约等于。
				TestTrue(*FString::Printf(TEXT("A right-angle corner insets exactly one wall thickness (%s)"), *Tag),
					Rect.InsetStart == T && Rect.InsetEnd == T);
				TestTrue(*FString::Printf(TEXT("Start is the outer corner (%s)"), *Tag),
					Rect.Start.Equals(FP.Verts[Edge], 0.0));
				TestTrue(*FString::Printf(TEXT("In is the left-hand perpendicular of U (%s)"), *Tag),
					Rect.In.X == -Rect.U.Y && Rect.In.Y == Rect.U.X);

				// 新旧口径是同一面外墙：偶数边完全一致，奇数边旧框架 = 新框架两端各缩 T。
				const FCSHouseEdgeFrame Butt = CSHouseTest_ButtJointGetEdge(Edge, Size, T);
				const bool bOdd = (Edge & 1) != 0;
				const FVector2D ExpectStart = bOdd ? (Rect.Start + Rect.U * double(T)) : Rect.Start;
				const float ExpectLen = bOdd ? (Rect.Len - 2.0f * T) : Rect.Len;
				TestTrue(*FString::Printf(TEXT("The butt-joint frame is the mitre frame minus the ceded corner (%s)"), *Tag),
					Butt.Start.Equals(ExpectStart, 1.0e-6) && FMath::IsNearlyEqual(Butt.Len, ExpectLen, 1.0e-3f));
			}
		}
	}

	// ---- ② 斜接闭合：每个角上，前一条边的内皮终点 == 后一条边的内皮起点 ----
	//
	// 这是「面板不重叠、不留缝」的几何本体。凸角与凹角走同一个公式，所以拿一个 L 形（含一个凹角）
	// 和一个正五边形（非直角凸角）一起验。
	auto CheckClosure = [this](const FCSHouseFootprint& FP, float T, const TCHAR* Name)
	{
		const int32 N = FP.NumEdges();
		for (int32 Edge = 0; Edge < N; ++Edge)
		{
			const FCSHouseEdgeFrame A = CSHouse_GetEdge(Edge, FP, T);
			const FCSHouseEdgeFrame B = CSHouse_GetEdge((Edge + 1) % N, FP, T);
			const FVector2D EndA = CSHouseTest_InnerEnd(A, T);
			const FVector2D StartB = CSHouseTest_InnerStart(B, T);
			TestTrue(*FString::Printf(TEXT("%s: the inner faces of edges %d and %d meet on the bisector (%s vs %s)"),
					Name, Edge, (Edge + 1) % N, *EndA.ToString(), *StartB.ToString()),
				EndA.Equals(StartB, 1.0e-4));
		}
	};

	const float T = 24.0f;
	CheckClosure(FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)), T, TEXT("rectangle"));

	FCSHouseFootprint Penta;
	for (int32 i = 0; i < 5; ++i)
	{
		const double Angle = 2.0 * UE_DOUBLE_PI * double(i) / 5.0;
		Penta.Verts.Add(FVector2D(300.0 * FMath::Cos(Angle), 300.0 * FMath::Sin(Angle)));
	}
	CheckClosure(Penta, T, TEXT("pentagon"));
	// 正五边形每个角左转 72° ⇒ 让出量 T·tan(36°)。
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(0, Penta, T);
		const float Want = T * FMath::Tan(FMath::DegreesToRadians(36.0f));
		TestTrue(FString::Printf(TEXT("A regular pentagon cedes T*tan(36 deg) at each corner (%.4f vs %.4f)"), F.InsetStart, Want),
			FMath::IsNearlyEqual(F.InsetStart, Want, 1.0e-3f) && FMath::IsNearlyEqual(F.InsetEnd, Want, 1.0e-3f));
	}

	// L 形（逆时针）：(0,0) → (600,0) → (600,300) → (300,300) → (300,600) → (0,600)。
	// 顶点 3 = (300,300) 处是**右转**（凹角），让出量必须为负，内皮向外伸出去补那块三角。
	FCSHouseFootprint L;
	L.Verts = { { 0.0, 0.0 }, { 600.0, 0.0 }, { 600.0, 300.0 }, { 300.0, 300.0 }, { 300.0, 600.0 }, { 0.0, 600.0 } };
	CheckClosure(L, T, TEXT("L-shape"));
	{
		// 边 2 走进凹角（终点是顶点 3），边 3 走出凹角（起点是顶点 3）。
		const FCSHouseEdgeFrame In = CSHouse_GetEdge(2, L, T);
		const FCSHouseEdgeFrame Out = CSHouse_GetEdge(3, L, T);
		TestTrue(FString::Printf(TEXT("A reflex corner cedes a negative amount on the incoming edge (%.3f)"), In.InsetEnd),
			FMath::IsNearlyEqual(In.InsetEnd, -T, 1.0e-3f));
		TestTrue(FString::Printf(TEXT("and on the outgoing edge (%.3f)"), Out.InsetStart),
			FMath::IsNearlyEqual(Out.InsetStart, -T, 1.0e-3f));
		// 深度 T 处的实体区间因此比外皮还长。
		float S0 = 0.0f, S1 = 0.0f;
		Out.SpanAtDepth(T, T, S0, S1);
		TestTrue(TEXT("At the inner face the reflex-side span reaches past the outer corner"), S0 < 0.0f);
	}

	// ---- ③ 实心体守恒：无洞时三角汤的体积 = (外轮廓面积 − 内轮廓面积) × 墙高 ----
	//
	// 面板有任何重叠，体积就会偏大；有缝就会偏小。直角对接时代同样满足这条（偶数边吃下转角、
	// 奇数边让开），所以它钉的不是「换了约定」，而是「换完之后仍然是那一个实心环」。
	auto CheckVolume = [this](const FCSHouseFootprint& FP, float T, float H, const TCHAR* Name)
	{
		FCSHouseBodyDesc Desc;
		Desc.Footprint = FP;
		Desc.WallThickness = T;
		Desc.WallHeight = H;
		FCSGpuMeshCPUData Soup;
		CSHouse_BuildBodySoup(Desc, Soup);

		TArray<FVector2D> Inner;
		for (int32 Edge = 0; Edge < FP.NumEdges(); ++Edge)
		{
			Inner.Add(CSHouseTest_InnerStart(CSHouse_GetEdge(Edge, FP, T), T));
		}
		const double Want = (CSHouseTest_PolygonArea(FP.Verts) - CSHouseTest_PolygonArea(Inner)) * double(H);
		const double Got = CSHouseTest_SoupVolume(Soup);
		TestTrue(FString::Printf(TEXT("%s: the wall panels fill exactly one solid ring (volume %.1f vs %.1f)"), Name, Got, Want),
			FMath::IsNearlyEqual(Got, Want, Want * 1.0e-6 + 1.0));
	};
	CheckVolume(FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)), T, 300.0f, TEXT("rectangle"));
	CheckVolume(Penta, T, 300.0f, TEXT("pentagon"));
	CheckVolume(L, T, 300.0f, TEXT("L-shape"));

	// ---- ④ 旧锚点：口径 0 在奇数边上从缩进 T 的那一点量起 ⇒ 解析时补一个 T ----
	{
		const FCSHouseFootprint FP = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
		FCSWallAnchor Legacy;
		Legacy.EdgeIndex = 1;
		Legacy.DistFromCorner = 100.0f;
		Legacy.SConvention = 0;
		FCSWallAnchor Mitre = Legacy;
		Mitre.SConvention = 1;
		TestTrue(TEXT("A legacy odd-edge anchor resolves one wall thickness further from the corner"),
			FMath::IsNearlyEqual(CSHouse_AnchorS(Legacy, FP, T), 100.0f + T, 1.0e-3f));
		TestTrue(TEXT("A mitre anchor resolves verbatim"),
			FMath::IsNearlyEqual(CSHouse_AnchorS(Mitre, FP, T), 100.0f, 1.0e-3f));

		// 终点角那一侧同理：旧框架里 S = (L − 2T) − d，折到外角点量起就是 L − (d + T)。
		Legacy.bFromEndCorner = true;
		TestTrue(TEXT("The same holds when anchored to the end corner"),
			FMath::IsNearlyEqual(CSHouse_AnchorS(Legacy, FP, T), 400.0f - (100.0f + T), 1.0e-3f));

		// 偶数边在旧口径下本来就从外角点量起，不换算。
		FCSWallAnchor EvenLegacy;
		EvenLegacy.EdgeIndex = 0;
		EvenLegacy.DistFromCorner = 100.0f;
		TestTrue(TEXT("A legacy even-edge anchor is unchanged"),
			FMath::IsNearlyEqual(CSHouse_AnchorS(EvenLegacy, FP, T), 100.0f, 1.0e-3f));

		// 新造的锚点一律写斜接口径 —— 否则它会被上面那条换算再挪一次。
		FCSWallHit Hit;
		Hit.bHit = true;
		Hit.EdgeIndex = 1;
		Hit.S = 120.0f;
		const FCSWallAnchor Made = CSHouse_MakeWallAnchor(Hit, FP, T, 90.0f);
		TestEqual(TEXT("MakeWallAnchor writes the mitre convention"), int32(Made.SConvention), 1);
		TestTrue(TEXT("and round-trips its own S"), FMath::IsNearlyEqual(CSHouse_AnchorS(Made, FP, T), 120.0f, 1.0e-3f));
	}

	// ---- ⑤ 退化输入不炸 ----
	{
		FCSHouseFootprint Degenerate;
		Degenerate.Verts.Add(FVector2D::ZeroVector);
		Degenerate.Verts.Add(FVector2D(100.0, 0.0));
		TestFalse(TEXT("Two vertices is not a valid footprint"), Degenerate.IsValidFootprint());
		TestTrue(TEXT("Degenerate footprint yields a zero-length frame"), CSHouse_GetEdge(0, Degenerate, T).Len == 0.0f);
		TestTrue(TEXT("Out-of-range edge yields a zero-length frame"),
			CSHouse_GetEdge(9, FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)), T).Len == 0.0f);
	}

	return true;
}

// -----------------------------------------------------------------------------
// footprint 折线化（3a-2）的存量锚点：旧口径的窗经过拉尺寸之后仍守恒世界位置，而且不会越拉越偏
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseLegacyAnchorResizeTest,
	"PCGPlugins.ComputeShaderGenerator.House.LegacyAnchorSurvivesResize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseLegacyAnchorResizeTest::RunTest(const FString& Parameters)
{
	// `ReanchorMarkersToPreserveWorld` 在斜接框架上重新量出距离，却曾把锚点的口径字段原样留着：
	// 旧锚点（`SConvention = 0`）于是被 `CSHouse_AnchorS` 在奇数边上**再补一个 T** —— 拉一次尺寸，
	// 东西墙上的窗就沿墙滑一个墙厚，拖边的每一帧再滑一个。`House.FootprintMitreCorners` ④ 只验了
	// "旧锚点读得对"，没验"旧锚点被重新表达之后读得对"；重新表达只发生在房子的重求值里，所以这条要 world。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	// 与 `House.WindowMarker` 同一条理由：把道路驱动的门那一路摘干净，只测锚点这一环。
	House->Windows.Reset();
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->ReevaluateSite();
	const float T = House->WallThickness;

	ACSWindowMarker* Marker = World->SpawnActor<ACSWindowMarker>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Window marker"), Marker)) return false;
	// 必须关自毁：spawn 那一瞬间标记在原点、还没 adopt（理由同 `House.WindowBrushPlacement`）。
	Marker->bDestroyWhenHostless = false;

	// 东墙（边 1，奇数边）上一个**旧口径**锚点：从起点角缩进 T 的那一点量起 100 ⇒ 斜接口径下 S = 100 + T。
	// 起点角是东南角 —— 下面推的是南墙，这个角会动，所以锚点距离必须被重新表达。
	const float HalfHeight = Marker->GetDemandHalfHeight();
	FCSWallAnchor Legacy;
	Legacy.EdgeIndex = 1;
	Legacy.bFromEndCorner = false;
	Legacy.DistFromCorner = 100.0f;
	Legacy.SillZ = FMath::Max(0.0f, 150.0f - HalfHeight);
	Legacy.SConvention = 0;
	Marker->AdoptAnchor(House, Legacy);
	TestEqual(TEXT("the legacy window is accepted"), Marker->GetLastReject(), ECSFeatureReject::None);
	if (Marker->GetLastReject() != ECSFeatureReject::None) return false;
	TestTrue(TEXT("a legacy odd-edge anchor reads back one wall thickness past its old origin"),
		FMath::IsNearlyEqual(CSHouse_AnchorS(Marker->GetAnchor(), House->GetFootprint(), T), 100.0f + T, 0.01f));
	const FVector Before = House->AnchorToWorld(Marker->GetAnchor(), HalfHeight, Marker->WallStandoff).GetLocation();

	auto WindowCentreS = [House]()
	{
		const TArray<FCSWallOpening> Openings = House->GetCurrentOpenings();
		const FCSWallOpening* Window = Openings.FindByPredicate(
			[](const FCSWallOpening& O) { return O.Type == ECSOpeningType::Window; });
		return Window ? Window->CenterS : -1.0f;
	};

	// 连推三次（有进有出）：修之前第一次就偏一个 T，之后每次再叠一个 T。
	int32 Step = 0;
	for (const float Push : { 50.0f, -30.0f, 20.0f })
	{
		++Step;
		House->PushEdge(0, Push, true);
		const FCSWallAnchor Now = Marker->GetAnchor();
		const FVector After = House->AnchorToWorld(Now, HalfHeight, Marker->WallStandoff).GetLocation();
		TestEqual(FString::Printf(TEXT("push %d: the re-expressed anchor is written in the mitre convention"), Step),
			int32(Now.SConvention), 1);
		TestTrue(FString::Printf(TEXT("push %d: the window keeps its world position (%s vs %s)"), Step,
				*After.ToCompactString(), *Before.ToCompactString()),
			After.Equals(Before, 0.05));
		TestTrue(FString::Printf(TEXT("push %d: the marker sits on its anchor"), Step),
			Marker->GetActorLocation().Equals(After, 0.5));
		const float WantS = CSHouse_AnchorS(Now, House->GetFootprint(), T);
		TestTrue(FString::Printf(TEXT("push %d: the hole follows the same anchor (%.2f vs %.2f)"), Step, WindowCentreS(), WantS),
			FMath::IsNearlyEqual(WindowCentreS(), WantS, 0.01f));
		TestTrue(FString::Printf(TEXT("push %d: and it is still cut"), Step), Marker->CausesCut());
	}

	World->DestroyActor(Marker);
	World->DestroyActor(House);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSHouseQuoinProfileTest,
	"PCGPlugins.ComputeShaderGenerator.House.QuoinProfile",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseQuoinProfileTest::RunTest(const FString& Parameters)
{
	CSHouseFrame::FBrickParams Params;
	Params.Jitter = 16.0f;
	Params.SplitJitter = 21.0f;
	TArray<CSHouseQuoin::FQuoin> Quoins;
	CSHouseQuoin::BuildQuoins(FTransform::Identity, FVector2D(600, 400), 24, 0, 471, 0, Quoins);
	Quoins[0].CullBelowZ = 180.0f;
	TArray<CSHouseFrame::FElement> Elements;
	CSHouseQuoin::BuildQuoinElements(Quoins, 123, Params, Elements, FVector2f(0.01f, 0.01f));
	TestEqual(TEXT("Four corners use four ordinary column paths"), Elements.Num(), 4);
	if (Elements.Num() != 4) return false;
	const auto& E = Elements[0];
	TestTrue(TEXT("Native dimensions scale with course height"), FMath::IsNearlyEqual(E.QuoinScale, 26.0f / 69.0f));
	TestTrue(TEXT("Width jitter is scaled, not a world-space diagonal shift"), FMath::IsNearlyEqual(E.Jitter, 16.0f * 26.0f / 69.0f));
	TestTrue(TEXT("Split jitter uses the same native scale"), FMath::IsNearlyEqual(E.SplitJitter, 21.0f * 26.0f / 69.0f));
	TestEqual(TEXT("Corner openings retain their cull height"), E.CullBelowZ, 180.0f);
	TestEqual(TEXT("Generic frame paths do not acquire a quoin profile"), CSHouseFrame::FElement().QuoinScale, 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSHouseMasonryContactTest,
	"PCGPlugins.ComputeShaderGenerator.House.MasonryContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseMasonryContactTest::RunTest(const FString& Parameters)
{
	// Exercise short supports, non-integral lengths, rotated placement and
	// legacy serialized jitter. Every course must bear on a full shaft section;
	// caps must grow towards the building and the top must touch its underside.
	for (float Length : { 11.0f, 37.0f, 150.0f, 237.0f })
	{
		CSHousePillar::FParams Params;
		Params.YawJitter = 0.14f;
		Params.SizeJitter = 0.10f;
		const FTransform World(FRotator(0, 37, 0), FVector(1200, -600, 300));
		TArray<CSHousePillar::FBrick> Bricks;
		CSHousePillar::BuildBricks({ FVector(100, 80, 0) }, { Length }, World, Params, Bricks);
		TestTrue(TEXT("A usable gap produces a support"), !Bricks.IsEmpty());
		float LastBottom = 300.0f;
		for (const auto& B : Bricks)
		{
			const float X = B.AxisX.Size(), Y = B.AxisY.Size();
			TestTrue(TEXT("Masonry retains a square bearing section"), FMath::IsNearlyEqual(X,Y,0.001f));
			TestTrue(TEXT("No course narrows below 95 percent of the shaft"), X >= Params.BrickWidth * 0.95f - 0.001f);
			TestTrue(TEXT("Adjacent horizontal bearing planes meet"), FMath::IsNearlyEqual(B.Origin.Z+B.AxisZ.Z*0.5f,LastBottom,0.001f));
			LastBottom = B.Origin.Z-B.AxisZ.Z*0.5f;
		}
		TestTrue(TEXT("Support reaches the requested embedded bottom"), FMath::IsNearlyEqual(LastBottom,300.0f-Length,0.001f));
	}
	return true;
}


// =============================================================================
// D7 接触记录（2026-09-16）—— 共享记录 / 写入口验 / 谁动了谁发 / 松手提交 / 一条缝只砌一次
//
// 纯函数那半（交点 / 裁剪 / 逐位相同）仍由 House.Seam* 三条钉着；这一组钉的是**协议**：
// 两端持同一个对象、只有 Owner 砌砖、拖拽帧不发、松手才发、删除时对端清空、两栋同帧动也收敛。
// =============================================================================

namespace
{
struct FCSContactPair
{
	ACSHouseActor* A = nullptr;
	ACSHouseActor* B = nullptr;
};

/** 两栋真相交的房（600×400 与 500×500 偏移 (250,150)，同回归脚本），都能出砖（引擎 cube 当砖）。 */
FCSContactPair CSHouseTest_SpawnContactPair(UWorld* World, const FVector& AtA, const FVector& AtB, bool bSpawnBFirst = false)
{
	UStaticMesh* Brick = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	auto Spawn = [&](const FVector& At, const FVector2D& Size) -> ACSHouseActor*
	{
		ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(At, FRotator::ZeroRotator);
		if (!House) return nullptr;
		House->Windows.Reset();
		House->bSeamEnabled = true;
		House->bFrameEnabled = true;
		House->FrameBrickMesh = Brick;
		House->FootprintSize = Size;
		House->ReevaluateSite();
		return House;
	};
	FCSContactPair Pair;
	if (bSpawnBFirst)
	{
		Pair.B = Spawn(AtB, FVector2D(500.0, 500.0));
		Pair.A = Spawn(AtA, FVector2D(600.0, 400.0));
	}
	else
	{
		Pair.A = Spawn(AtA, FVector2D(600.0, 400.0));
		Pair.B = Spawn(AtB, FVector2D(500.0, 500.0));
	}
	return Pair;
}

const FCSHouseContact* CSHouseTest_FirstContact(const ACSHouseActor* House)
{
	const TArray<TSharedPtr<FCSHouseContact>>& Contacts = House->DebugGetContacts();
	return Contacts.Num() > 0 ? Contacts[0].Get() : nullptr;
}

bool CSHouseTest_SameCuts(const TArray<FCSWallCut>& L, const TArray<FCSWallCut>& R)
{
	if (L.Num() != R.Num()) return false;
	for (int32 I = 0; I < L.Num(); ++I)
	{
		if (L[I].EdgeIndex != R[I].EdgeIndex || L[I].MinS != R[I].MinS || L[I].MaxS != R[I].MaxS
			|| L[I].BottomZ != R[I].BottomZ || L[I].TopZ != R[I].TopZ) return false;
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactSingleOwnerTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactSingleOwner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactSingleOwnerTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;

	TestEqual(TEXT("A holds one contact"), P.A->GetContactCount(), 1);
	TestEqual(TEXT("B holds one contact"), P.B->GetContactCount(), 1);
	const FCSHouseContact* CA = CSHouseTest_FirstContact(P.A);
	const FCSHouseContact* CB = CSHouseTest_FirstContact(P.B);
	TestTrue(TEXT("both ends hold the same object"), CA != nullptr && CA == CB);
	if (CA) TestTrue(TEXT("it is a vertical seam"), CA->Kind == ECSHouseContactKind::Seam);
	TestTrue(TEXT("A is cut where it pokes into B"), P.A->GetSeamCutCount() > 0);
	TestTrue(TEXT("B is cut where it pokes into A"), P.B->GetSeamCutCount() > 0);
	TestTrue(TEXT("both report the same crossings"),
		P.A->GetSeamCornerCount() > 0 && P.A->GetSeamCornerCount() == P.B->GetSeamCornerCount());

	// 一条缝只砌一次：规范序槽 0（GUID 小者）出砖，另一端一块都不砌。
	const bool bAOwns = CSHouseSeam::IdLess(P.A->GetHouseId(), P.B->GetHouseId());
	ACSHouseActor* Owner = bAOwns ? P.A : P.B;
	ACSHouseActor* Guest = bAOwns ? P.B : P.A;
	TestTrue(FString::Printf(TEXT("the GUID-smaller house builds the posts (posts=%d bricks=%d)"), Owner->GetSeamOwnedPostCount(), Owner->GetSeamBrickCount()),
		Owner->GetSeamOwnedPostCount() > 0 && Owner->GetSeamBrickCount() > 0);
	TestEqual(TEXT("the other house builds no posts"), Guest->GetSeamOwnedPostCount(), 0);
	TestEqual(TEXT("and has no seam bricks"), Guest->GetSeamBrickCount(), 0);
	if (CA) TestTrue(TEXT("Owner() agrees with the GUID order"), CA->Owner() == Owner);
	TestEqual(TEXT("A audits clean"), P.A->AuditContacts().Num(), 0);
	TestEqual(TEXT("B audits clean"), P.B->AuditContacts().Num(), 0);

	// 只重建其中一栋：输入没变 ⇒ 不重发，两边一个数都不动，对象还是同一个。
	P.B->ReevaluateSite();
	TestTrue(TEXT("rebuilding one house alone keeps the shared object"), CSHouseTest_FirstContact(P.A) == CA && CSHouseTest_FirstContact(P.B) == CA);

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactScriptedMoveTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactScriptedMoveCommits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactScriptedMoveTest::RunTest(const FString& Parameters)
{
	// 脚本直设 transform 没有"松手"：根组件 TransformUpdated 标脏，下一次重求值就是提交，对端收到**新对象**。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;

	const FCSHouseContact* Before = CSHouseTest_FirstContact(P.B);
	const TArray<FCSWallCut> BCutsBefore = P.B->DebugGetSeamCuts();
	if (!TestNotNull(TEXT("a contact exists before the move"), Before)) return false;

	P.A->SetActorLocation(FVector(40.0, 0.0, 0.0));
	TestTrue(TEXT("SetActorLocation marks A pending through TransformUpdated"), P.A->IsReevaluatePending());
	TestEqual(TEXT("A commits on its next evaluation and still holds one contact"), P.A->GetContactCount(), 1);
	TestTrue(TEXT("B was written a new object"), CSHouseTest_FirstContact(P.B) != Before && P.B->GetContactCount() == 1);
	TestTrue(TEXT("both ends hold the new object"), CSHouseTest_FirstContact(P.A) == CSHouseTest_FirstContact(P.B));
	TestFalse(TEXT("B's cuts moved with A"), CSHouseTest_SameCuts(BCutsBefore, P.B->DebugGetSeamCuts()));
	TestEqual(TEXT("A audits clean"), P.A->AuditContacts().Num(), 0);
	TestEqual(TEXT("B audits clean"), P.B->AuditContacts().Num(), 0);

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactCommitOnlyTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactCommitOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactCommitOnlyTest::RunTest(const FString& Parameters)
{
	// 拖拽帧（bFinished=false）接缝一概不动：自己的裁剪冻结、对端不被写；松手那一次才提交。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;

	P.B->GetContactCount();   // 补票，让 B 不再欠账 —— 下面要断言"拖动期间 B 没被写"
	const FCSHouseContact* P0 = CSHouseTest_FirstContact(P.B);
	const TArray<FCSWallCut> ACutsBefore = P.A->DebugGetSeamCuts();
	if (!TestNotNull(TEXT("a contact exists before the drag"), P0)) return false;

	for (int32 Frame = 0; Frame < 3; ++Frame) P.A->PushEdge(1, 10.0f, /*bFinished=*/false);   // 东墙外推，拖动中
	TestTrue(TEXT("A is in gizmo drag"), P.A->IsInGizmoDrag());
	TestTrue(TEXT("A's own seam is frozen while dragging"), CSHouseTest_SameCuts(ACutsBefore, P.A->DebugGetSeamCuts()));
	TestTrue(TEXT("B is not written during the drag"), CSHouseTest_FirstContact(P.B) == P0 && !P.B->IsReevaluatePending());
	TestEqual(TEXT("the audit exempts the drag"), P.A->AuditContacts().Num(), 0);

	P.A->PushEdge(1, 0.0f, /*bFinished=*/true);
	TestFalse(TEXT("release ends the drag"), P.A->IsInGizmoDrag());
	TestTrue(TEXT("release commits: B holds a new object"), CSHouseTest_FirstContact(P.B) != P0 && P.B->GetContactCount() == 1);
	TestTrue(TEXT("both ends hold the new object"), CSHouseTest_FirstContact(P.A) == CSHouseTest_FirstContact(P.B));
	TestFalse(TEXT("A's seam follows the new footprint after release"), CSHouseTest_SameCuts(ACutsBefore, P.A->DebugGetSeamCuts()));
	TestEqual(TEXT("A audits clean"), P.A->AuditContacts().Num(), 0);
	TestEqual(TEXT("B audits clean"), P.B->AuditContacts().Num(), 0);

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactDetachOnDestroyTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactDetachOnDestroy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactDetachOnDestroyTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;
	TestEqual(TEXT("B starts with a contact"), P.B->GetContactCount(), 1);

	World->DestroyActor(P.A);
	TestEqual(TEXT("B drops the contact when A is destroyed"), P.B->GetContactCount(), 0);
	TestEqual(TEXT("B has no cuts left"), P.B->GetSeamCutCount(), 0);
	TestEqual(TEXT("B has no seam bricks left"), P.B->GetSeamBrickCount(), 0);
	TestEqual(TEXT("B audits clean"), P.B->AuditContacts().Num(), 0);

	World->DestroyActor(P.B);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactSpawnOrderTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactSpawnOrderInvariant",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactSpawnOrderTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	auto Measure = [&](bool bSpawnBFirst, int32& OutCutsA, int32& OutCutsB, int32& OutOwnerPosts, int32& OutOwnerBricks) -> bool
	{
		const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0), bSpawnBFirst);
		if (!P.A || !P.B) return false;
		OutCutsA = P.A->GetSeamCutCount();
		OutCutsB = P.B->GetSeamCutCount();
		ACSHouseActor* Owner = CSHouseSeam::IdLess(P.A->GetHouseId(), P.B->GetHouseId()) ? P.A : P.B;
		OutOwnerPosts = Owner->GetSeamOwnedPostCount();
		OutOwnerBricks = Owner->GetSeamBrickCount();
		const bool bClean = P.A->AuditContacts().Num() == 0 && P.B->AuditContacts().Num() == 0
			&& P.A->GetContactCount() == 1 && P.B->GetContactCount() == 1;
		World->DestroyActor(P.B);
		World->DestroyActor(P.A);
		return bClean;
	};
	int32 CutsA1 = 0, CutsB1 = 0, Posts1 = 0, Bricks1 = 0, CutsA2 = 0, CutsB2 = 0, Posts2 = 0, Bricks2 = 0;
	TestTrue(TEXT("A then B: one clean contact"), Measure(false, CutsA1, CutsB1, Posts1, Bricks1));
	TestTrue(TEXT("B then A: one clean contact"), Measure(true, CutsA2, CutsB2, Posts2, Bricks2));
	TestEqual(TEXT("A's cuts do not depend on spawn order"), CutsA2, CutsA1);
	TestEqual(TEXT("B's cuts do not depend on spawn order"), CutsB2, CutsB1);
	TestEqual(TEXT("the owner's posts do not depend on spawn order"), Posts2, Posts1);
	TestEqual(TEXT("the owner's bricks do not depend on spawn order"), Bricks2, Bricks1);
	TestTrue(TEXT("and there are posts at all"), Posts1 > 0 && Bricks1 > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactMultiMoveTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactMultiMoveConverges",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactMultiMoveTest::RunTest(const FString& Parameters)
{
	// 两栋同帧动（多选拖动松手）：互发一轮就停，不重复、不互相叫醒不停。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;

	P.A->SetActorLocation(FVector(30.0, 0.0, 0.0));
	P.B->SetActorLocation(FVector(280.0, 160.0, 0.0));
	P.A->GetContactCount();
	P.B->GetContactCount();
	P.A->GetContactCount();
	P.B->GetContactCount();
	TestEqual(TEXT("A holds one contact"), P.A->GetContactCount(), 1);
	TestEqual(TEXT("B holds one contact"), P.B->GetContactCount(), 1);
	TestTrue(TEXT("both ends hold the same object"),
		CSHouseTest_FirstContact(P.A) != nullptr && CSHouseTest_FirstContact(P.A) == CSHouseTest_FirstContact(P.B));
	TestEqual(TEXT("A audits clean"), P.A->AuditContacts().Num(), 0);
	TestEqual(TEXT("B audits clean"), P.B->AuditContacts().Num(), 0);

	// 收敛：之后再补票谁都不再重求值。
	const int64 CountA = P.A->GetReevaluateCount();
	const int64 CountB = P.B->GetReevaluateCount();
	P.A->GetContactCount();
	P.B->GetContactCount();
	P.A->GetContactCount();
	TestEqual(TEXT("A does not re-evaluate again (no ping-pong)"), P.A->GetReevaluateCount(), CountA);
	TestEqual(TEXT("B does not re-evaluate again (no ping-pong)"), P.B->GetReevaluateCount(), CountB);

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseSeamPostMergeWithinContactTest,
	"PCGPlugins.ComputeShaderGenerator.House.SeamPostMergeWithinContact",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseSeamPostMergeWithinContactTest::RunTest(const FString& Parameters)
{
	// 同一条接触内：XY 近的交点并成一根（质心、Z 取并集），远的各自一根，与输入顺序无关。
	// 「跨接触绝不合并」是接触层面的事（2026-09-16 晚用户裁决），由 `House.ContactStackedPostsPerPair` 在真房子上钉。
	auto Post = [](double X, double Y, float Bottom, float Top)
	{
		CSHouseSeam::FPost P;
		P.Point = FVector2D(X, Y); P.BottomZ = Bottom; P.TopZ = Top;
		return P;
	};
	const TArray<CSHouseSeam::FPost> In = {
		Post(0.0, 0.0, 0.0f, 300.0f),      // 簇 1
		Post(100.0, 0.0, 0.0f, 300.0f),    // 簇 2
		Post(10.0, 0.0, 20.0f, 320.0f),    // 簇 1（将来逐点底高不同时也取并集）
	};
	TArray<CSHouseSeam::FPost> Out;
	const int32 N = CSHouseSeam::MergePosts(In, 30.0f, Out);
	TestEqual(TEXT("two posts come out"), N, 2);
	if (N != 2) return false;
	TestTrue(TEXT("the merged post sits at the cluster centroid"), Out[0].Point.Equals(FVector2D(5.0, 0.0), 1.0e-6));
	TestTrue(TEXT("and spans the union of its members' Z"), Out[0].BottomZ == 0.0f && Out[0].TopZ == 320.0f);
	TestTrue(TEXT("the far post stays on its own"), Out[1].Point.Equals(FVector2D(100.0, 0.0), 1.0e-6));

	// 顺序无关：倒着喂，并出同一个集合（输出顺序按最小输入下标，比较前先排一下）。
	TArray<CSHouseSeam::FPost> Reversed = In;
	Algo::Reverse(Reversed);
	TArray<CSHouseSeam::FPost> Out2;
	CSHouseSeam::MergePosts(Reversed, 30.0f, Out2);
	auto Key = [](const CSHouseSeam::FPost& P) { return P.Point.X * 1.0e6 + P.Point.Y * 1.0e3 + double(P.BottomZ); };
	Out.Sort([&](const CSHouseSeam::FPost& L, const CSHouseSeam::FPost& R) { return Key(L) < Key(R); });
	Out2.Sort([&](const CSHouseSeam::FPost& L, const CSHouseSeam::FPost& R) { return Key(L) < Key(R); });
	bool bSame = Out.Num() == Out2.Num();
	for (int32 I = 0; bSame && I < Out.Num(); ++I)
	{
		bSame = Out[I].Point.Equals(Out2[I].Point, 1.0e-6) && Out[I].BottomZ == Out2[I].BottomZ && Out[I].TopZ == Out2[I].TopZ;
	}
	TestTrue(TEXT("feeding the posts in reverse order merges to the same set"), bSame);

	// BuildPosts 的门槛：同回归脚本那一对（600x400 与 500x500 偏 (250,150)），两个交点、两墙正交、离顶点都远。
	CSHouseSeam::FHouse A, B;
	A.Id = FGuid(1u, 0u, 0u, 0u);
	A.Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
	B.Id = FGuid(2u, 0u, 0u, 0u);
	B.Center = FVector2D(250.0, 150.0);
	B.Footprint = FCSHouseFootprint::MakeRect(FVector2D(500.0, 500.0));
	TArray<CSHouseSeam::FCorner> Corners;
	CSHouseSeam::BuildCorners(A, B, Corners);
	TestEqual(TEXT("the pair crosses at two points"), Corners.Num(), 2);
	const CSHouseSeam::FPostParams Params;
	TArray<CSHouseSeam::FPost> Posts;
	TestEqual(TEXT("default thresholds keep both posts"), CSHouseSeam::BuildPosts(A, B, Corners, Params, Posts), 2);
	CSHouseSeam::FPostParams Far = Params;
	Far.MergeDistance = 100000.0f;
	TestEqual(TEXT("a huge merge distance folds one contact's posts into one"), CSHouseSeam::BuildPosts(A, B, Corners, Far, Posts), 1);
	CSHouseSeam::FPostParams Tall = Params;
	Tall.MinHeight = 301.0f;
	TestEqual(TEXT("posts shorter than MinHeight are dropped"), CSHouseSeam::BuildPosts(A, B, Corners, Tall, Posts), 0);
	CSHouseSeam::FPostParams Clear = Params;
	Clear.VertexClearance = 100000.0f;
	TestEqual(TEXT("posts near a footprint vertex are dropped"), CSHouseSeam::BuildPosts(A, B, Corners, Clear, Posts), 0);
	CSHouseSeam::FPostParams Parallel = Params;
	Parallel.ParallelDot = -1.0f;
	TestEqual(TEXT("posts whose walls count as parallel are dropped"), CSHouseSeam::BuildPosts(A, B, Corners, Parallel, Posts), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactBearingTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactBearingNoPosts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactBearingTest::RunTest(const FString& Parameters)
{
	// 上房嵌进下房几十厘米（容差内）⇒ 横缝：不立矮墩子、不 clip；嵌得比容差深 ⇒ 竖缝。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	// 下房墙高 300；上房底 270（嵌 30 cm，BearingTolerance 默认 100）。没有地面 ⇒ 落座不改 Z。
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(200.0, 0.0, 270.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;

	TestEqual(TEXT("a bearing contact exists on both ends"), P.A->GetContactCount() + P.B->GetContactCount(), 2);
	const FCSHouseContact* C = CSHouseTest_FirstContact(P.A);
	if (TestNotNull(TEXT("A holds it"), C)) TestTrue(TEXT("it is classified as bearing"), C->Kind == ECSHouseContactKind::Bearing);
	TestEqual(TEXT("no vertical-seam cuts on A"), P.A->GetSeamCutCount(), 0);
	TestEqual(TEXT("no vertical-seam cuts on B"), P.B->GetSeamCutCount(), 0);
	TestEqual(TEXT("no stub posts on A"), P.A->GetSeamOwnedPostCount(), 0);
	TestEqual(TEXT("no stub posts on B"), P.B->GetSeamOwnedPostCount(), 0);
	TestEqual(TEXT("A audits clean"), P.A->AuditContacts().Num(), 0);

	// 压下去 150 cm（> 容差）：这才是两栋房穿插，走竖缝。
	P.B->SetActorLocation(FVector(200.0, 0.0, 150.0));
	P.B->GetContactCount();
	const FCSHouseContact* C2 = CSHouseTest_FirstContact(P.A);
	if (TestNotNull(TEXT("A still holds a contact"), C2)) TestTrue(TEXT("deeper overlap is a vertical seam"), C2->Kind == ECSHouseContactKind::Seam);
	TestTrue(TEXT("and it cuts"), P.A->GetSeamCutCount() > 0 && P.B->GetSeamCutCount() > 0);

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactStackedPostsTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactStackedPostsPerPair",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactStackedPostsTest::RunTest(const FString& Parameters)
{
	// 用户裁决 2026-09-16 晚「数量上必须要对应」：A 在下、B 摞在 A 上（房底 = A 檐口 ⇒ 横缝）、C 从地面横穿两栋。
	// C 手里 A–C 与 B–C 两组交点 XY 相同、Z 相接，仍是**两组**柱，各归各的出砖方，永不跨接触合并。
	// 早先跨接触聚簇时，合并柱的归属取决于每栋房各自看得到哪些接触，GUID 六种序里四种少砌一截 —— 所以六种都跑。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	UStaticMesh* Brick = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	const FStructProperty* IdProperty = FindFProperty<FStructProperty>(ACSHouseActor::StaticClass(), TEXT("HouseId"));
	if (!TestNotNull(TEXT("HouseId is reflected"), IdProperty)) return false;

	// 先在远处生成（互不接触）并改好 GUID，再挪进位：改 GUID 的那一刻手里没有任何接触，不会留下旧序的记录。
	auto Spawn = [&](double FarX, const FVector2D& Size, float Height, const FGuid& Id) -> ACSHouseActor*
	{
		ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(FarX, 0.0, 0.0), FRotator::ZeroRotator);
		if (!House) return nullptr;
		House->Windows.Reset();
		House->bSeamEnabled = true;
		House->bFrameEnabled = true;
		House->FrameBrickMesh = Brick;
		House->FootprintSize = Size;
		House->WallHeight = Height;
		*IdProperty->ContainerPtrToValuePtr<FGuid>(House) = Id;
		House->ReevaluateSite();
		return House;
	};
	// 挪进位之后一路兑现：谁动了谁发，收到的被标脏，读 getter 补票，直到没人欠账。
	auto Settle = [](const TArray<ACSHouseActor*>& Houses)
	{
		for (int32 Round = 0; Round < 8; ++Round)
		{
			bool bPending = false;
			for (const ACSHouseActor* House : Houses) bPending |= House->IsReevaluatePending();
			if (!bPending) return true;
			for (const ACSHouseActor* House : Houses) House->GetContactCount();
		}
		return false;
	};
	auto CountKind = [](const ACSHouseActor* House, ECSHouseContactKind Kind)
	{
		int32 N = 0;
		for (const TSharedPtr<FCSHouseContact>& Contact : House->DebugGetContacts())
		{
			if (Contact.IsValid() && Contact->Kind == Kind) ++N;
		}
		return N;
	};

	const FGuid Ids[3] = { FGuid(1u, 0u, 0u, 0u), FGuid(2u, 0u, 0u, 0u), FGuid(3u, 0u, 0u, 0u) };
	const int32 Orders[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 0, 2 }, { 1, 2, 0 }, { 2, 0, 1 }, { 2, 1, 0 } };
	for (const auto& Order : Orders)
	{
		const FString Tag = FString::Printf(TEXT("[GUID rank A=%d B=%d C=%d] "), Order[0], Order[1], Order[2]);
		// A 600x400 墙高 300；B 同轮廓摞在 A 上（房底 300）；C 200x800 墙高 700 从地面起。
		// 交点：C 的两条长墙 x = ±100 × A / B 的两条长墙 y = ±200 ⇒ 每对 4 个，离任何顶点都 ≥ 200 cm。
		ACSHouseActor* A = Spawn(20000.0, FVector2D(600.0, 400.0), 300.0f, Ids[Order[0]]);
		ACSHouseActor* B = Spawn(40000.0, FVector2D(600.0, 400.0), 300.0f, Ids[Order[1]]);
		ACSHouseActor* C = Spawn(60000.0, FVector2D(200.0, 800.0), 700.0f, Ids[Order[2]]);
		if (!TestTrue(Tag + TEXT("three houses spawned"), A && B && C)) return false;
		A->SetActorLocation(FVector(0.0, 0.0, 0.0));
		B->SetActorLocation(FVector(0.0, 0.0, 300.0));
		C->SetActorLocation(FVector(0.0, 0.0, 0.0));
		TestTrue(Tag + TEXT("the three houses settle"), Settle({ A, B, C }));

		TestEqual(Tag + TEXT("C holds two vertical seams (A-C and B-C)"), CountKind(C, ECSHouseContactKind::Seam), 2);
		TestEqual(Tag + TEXT("A holds one vertical seam"), CountKind(A, ECSHouseContactKind::Seam), 1);
		TestEqual(Tag + TEXT("B holds one vertical seam"), CountKind(B, ECSHouseContactKind::Seam), 1);
		TestEqual(Tag + TEXT("A and B rest on each other (one bearing each)"),
			CountKind(A, ECSHouseContactKind::Bearing) + CountKind(B, ECSHouseContactKind::Bearing), 2);

		// 逐条竖缝：4 根柱、Z 只是自己那一对的区间（没有 0-600 的合并柱），记到它的出砖方名下。
		TMap<const ACSHouseActor*, int32> Expected;
		int32 Total = 0;
		for (const TSharedPtr<FCSHouseContact>& Contact : C->DebugGetContacts())
		{
			if (!Contact.IsValid() || Contact->Kind != ECSHouseContactKind::Seam) continue;
			const FCSHouseSeamContact& Seam = static_cast<const FCSHouseSeamContact&>(*Contact);
			const ACSHouseActor* Owner = Seam.Owner();
			if (!TestNotNull(Tag + TEXT("each seam has an owner"), Owner)) continue;
			const bool bLower = Seam.Involves(A);
			const float WantBottom = bLower ? 0.0f : 300.0f;
			const float WantTop = bLower ? 300.0f : 600.0f;
			TArray<CSHouseSeam::FPost> Posts;
			Seam.BuildPosts(Owner->MakeSeamPostParams(), Posts);
			TestEqual(Tag + (bLower ? TEXT("A-C has four posts") : TEXT("B-C has four posts")), Posts.Num(), 4);
			for (const CSHouseSeam::FPost& Post : Posts)
			{
				TestTrue(Tag + TEXT("each post spans only its own pair's Z"),
					FMath::IsNearlyEqual(Post.BottomZ, WantBottom, 0.01f) && FMath::IsNearlyEqual(Post.TopZ, WantTop, 0.01f));
			}
			Expected.FindOrAdd(Owner) += Posts.Num();
			Total += Posts.Num();
		}
		TestEqual(Tag + TEXT("two seams x four posts = eight"), Total, 8);
		for (const ACSHouseActor* House : { A, B, C })
		{
			TestEqual(Tag + FString::Printf(TEXT("%s builds exactly the posts of the seams it owns"), *House->GetName()),
				House->GetSeamOwnedPostCount(), Expected.FindRef(House));
			TestEqual(Tag + FString::Printf(TEXT("%s audits clean"), *House->GetName()), House->AuditContacts().Num(), 0);
		}
		TestEqual(Tag + TEXT("the three houses together build all eight"),
			A->GetSeamOwnedPostCount() + B->GetSeamOwnedPostCount() + C->GetSeamOwnedPostCount(), 8);

		World->DestroyActor(C);
		World->DestroyActor(B);
		World->DestroyActor(A);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactClassifySymmetricTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactClassifySymmetric",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactClassifySymmetricTest::RunTest(const FString& Parameters)
{
	// 同一对房子的接触种类不许取决于谁提交（谁当 `Classify` 的第一个参数）。早先有两处会翻：
	// ① 房底相同时 `>=` 让 A 当上房 —— B 矮墙 + 大容差时，A 提交判横缝、B 提交判竖缝；
	// ② 容差只读 A 的 —— 两栋设得不同时，结论随谁最后动翻转。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;
	auto KindOf = [](const TSharedPtr<FCSHouseContact>& Contact) { return Contact.IsValid() ? int32(Contact->Kind) : -1; };

	// ① 同房底；B 墙高 120、两栋容差都 150 ⇒「A 底 0 与 B 檐口 120 差 120 ≤ 150」是早先 A 视角的横缝。
	P.A->BearingTolerance = 150.0f;
	P.B->BearingTolerance = 150.0f;
	P.B->WallHeight = 120.0f;
	const int32 AB1 = KindOf(FCSHouseContact::Classify(P.A, P.B));
	const int32 BA1 = KindOf(FCSHouseContact::Classify(P.B, P.A));
	TestEqual(TEXT("same base: the kind does not depend on who classifies"), AB1, BA1);
	TestEqual(TEXT("same base means nobody sits on anybody: a vertical seam"), AB1, int32(ECSHouseContactKind::Seam));

	// ② B 摞在 A 上、嵌进 20 cm；A 容差 100、B 容差 0 ⇒ 取较大者，两个方向都是横缝。
	P.B->WallHeight = 300.0f;
	P.A->BearingTolerance = 100.0f;
	P.B->BearingTolerance = 0.0f;
	P.B->SetActorLocation(FVector(250.0, 150.0, 280.0));
	const int32 AB2 = KindOf(FCSHouseContact::Classify(P.A, P.B));
	const int32 BA2 = KindOf(FCSHouseContact::Classify(P.B, P.A));
	TestEqual(TEXT("different tolerances: the kind does not depend on who classifies"), AB2, BA2);
	TestEqual(TEXT("the larger tolerance wins: bearing"), AB2, int32(ECSHouseContactKind::Bearing));

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseDuplicateGetsOwnIdTest,
	"PCGPlugins.ComputeShaderGenerator.House.DuplicateGetsOwnId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseDuplicateGetsOwnIdTest::RunTest(const FString& Parameters)
{
	// 结构审查 13（2026-09-16 晚编辑器实测）：复制出的房子与原件同 GUID ⇒ 规范序打平、`Classify` 判成同一栋，
	// 两栋永远不出缝。① 编辑器复制（Ctrl+C/V、Ctrl+D、Alt 拖同一条 T3D 路）给副本新 GUID，并与原件正常出缝；
	// ② 已经撞上的（此前存盘的旧副本）注册时查重、换一个确定性的。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(20000.0, 0.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;
	UEditorActorSubsystem* EditorActors = GEditor ? GEditor->GetEditorSubsystem<UEditorActorSubsystem>() : nullptr;
	if (!TestNotNull(TEXT("EditorActorSubsystem"), EditorActors)) return false;
	const FStructProperty* IdProperty = FindFProperty<FStructProperty>(ACSHouseActor::StaticClass(), TEXT("HouseId"));
	if (!TestNotNull(TEXT("HouseId is reflected"), IdProperty)) return false;
	TestTrue(TEXT("HouseId is skipped by non-PIE text export (so Ctrl+C text carries no HouseId)"),
		IdProperty->HasAnyPropertyFlags(CPF_NonPIEDuplicateTransient));

	// ① 与原件重叠地复制一份（600x400 偏 (250,150)，同回归脚本那一对的摆法）。
	const FGuid AId = P.A->GetHouseId();
	ACSHouseActor* Copy = Cast<ACSHouseActor>(EditorActors->DuplicateActor(P.A, World, FVector(250.0, 150.0, 0.0)));
	if (!TestNotNull(TEXT("the editor duplicates the house"), Copy)) return false;
	TestTrue(TEXT("the copy gets a valid id of its own"), Copy->GetHouseId().IsValid() && Copy->GetHouseId() != AId);
	TestTrue(TEXT("and the original keeps its id"), P.A->GetHouseId() == AId);
	for (int32 Round = 0; Round < 4; ++Round)
	{
		P.A->GetContactCount();
		Copy->GetContactCount();
	}
	TestEqual(TEXT("so the copy seams with the original (original end)"), P.A->GetContactCount(), 1);
	TestEqual(TEXT("so the copy seams with the original (copy end)"), Copy->GetContactCount(), 1);
	TestTrue(TEXT("and exactly one of them builds the posts"),
		(P.A->GetSeamOwnedPostCount() > 0) != (Copy->GetSeamOwnedPostCount() > 0));

	// ② 模拟旧副本：把 B 的 GUID 写成 A 的（反射写私有属性），重新注册 ⇒ B 换一个，A 不动；同一栋再撞一次得到同一个值。
	*IdProperty->ContainerPtrToValuePtr<FGuid>(P.B) = AId;
	P.B->ReregisterAllComponents();
	const FGuid Reassigned = P.B->GetHouseId();
	TestTrue(TEXT("a house registering with a taken id takes a new one"), Reassigned.IsValid() && Reassigned != AId);
	TestTrue(TEXT("while the house that already had it keeps it"), P.A->GetHouseId() == AId);
	*IdProperty->ContainerPtrToValuePtr<FGuid>(P.B) = AId;
	P.B->ReregisterAllComponents();
	TestTrue(TEXT("the replacement is deterministic for the same actor (reloads do not reshuffle its bricks)"), P.B->GetHouseId() == Reassigned);

	World->DestroyActor(Copy);
	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseContactDragTimeoutTest,
	"PCGPlugins.ComputeShaderGenerator.House.ContactDragTimeoutCommits",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseContactDragTimeoutTest::RunTest(const FString& Parameters)
{
	// 松手事件没来（抓手收到的第一个 bFinished 被降级成拖动帧、Esc 取消、选中不拖）时，拖动兜底要**自己**兑现。
	// 2026-09-16 晚编辑器实测：标志卡在 true，而只在下次重求值里查超时 —— 没人再叫醒这栋房，拖完一直不出缝。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	const FCSContactPair P = CSHouseTest_SpawnContactPair(World, FVector::ZeroVector, FVector(250.0, 150.0, 0.0));
	if (!TestNotNull(TEXT("A"), P.A) || !TestNotNull(TEXT("B"), P.B)) return false;
	const FCSHouseContact* Before = CSHouseTest_FirstContact(P.B);
	if (!TestNotNull(TEXT("B holds the contact before the drag"), Before)) return false;

	P.A->GizmoDragIdleSeconds = 0.1f;
	P.A->PushEdge(1, 40.0f, false);   // 一帧拖动，之后再没有任何事件
	TestTrue(TEXT("A is frozen mid-drag"), P.A->IsInGizmoDrag());
	TestTrue(TEXT("B is not written during the drag"), CSHouseTest_FirstContact(P.B) == Before);
	TestTrue(TEXT("the dragged house keeps its tick on to time the drag out"), P.A->IsActorTickEnabled());

	FPlatformProcess::Sleep(0.25f);
	++GFrameCounter;
	World->Tick(LEVELTICK_ViewportsOnly, 1.0f / 30.0f);
	TestFalse(TEXT("the stuck drag flag times out on the house's own tick"), P.A->IsInGizmoDrag());
	const FCSHouseContact* After = CSHouseTest_FirstContact(P.B);
	TestTrue(TEXT("and the seam is re-sent without any release event"), After != nullptr && After != Before);
	TestTrue(TEXT("to both ends"), CSHouseTest_FirstContact(P.A) == After);

	++GFrameCounter;
	World->Tick(LEVELTICK_ViewportsOnly, 1.0f / 30.0f);
	TestFalse(TEXT("after that the house goes idle again (tick off)"), P.A->IsActorTickEnabled());

	World->DestroyActor(P.B);
	World->DestroyActor(P.A);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseBodyBottomFaceTest,
	"PCGPlugins.ComputeShaderGenerator.House.BodyBottomFace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseBodyBottomFaceTest::RunTest(const FString& Parameters)
{
	// 房体底面（2026-09-17）：墙内皮围出的那块在房底高度封上，朝下的单面；墙板自己的底不重复铺。
	FCSHouseBodyDesc Desc;
	Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
	Desc.WallThickness = 24.0f;
	Desc.WallHeight = 300.0f;

	Desc.bBottomFace = false;
	FCSGpuMeshCPUData Open;
	CSHouse_BuildBodySoup(Desc, Open);
	Desc.bBottomFace = true;
	FCSGpuMeshCPUData Closed;
	CSHouse_BuildBodySoup(Desc, Closed);

	const int32 OpenTris = Open.Indices.Num() / 3, ClosedTris = Closed.Indices.Num() / 3;
	TestEqual(TEXT("a rectangle's bottom face is exactly two extra triangles"), ClosedTris - OpenTris, 2);
	if (ClosedTris - OpenTris != 2) return false;
	// 追加写 ⇒ 底面是最后两个三角（墙在前、底面在屋顶注释之前、之后再无几何）。
	double Area = 0.0;
	int32 BadNormal = 0, BadHeight = 0, OutsideInner = 0;
	const double HX = 300.0 - 24.0, HY = 200.0 - 24.0;
	for (int32 Tri = OpenTris; Tri < ClosedTris; ++Tri)
	{
		const FVector P[3] = { FVector(Closed.Positions[Closed.Indices[Tri * 3]]), FVector(Closed.Positions[Closed.Indices[Tri * 3 + 1]]),
			FVector(Closed.Positions[Closed.Indices[Tri * 3 + 2]]) };
		for (int32 V = 0; V < 3; ++V)
		{
			if (!FMath::IsNearlyZero(P[V].Z, 1.0e-3)) ++BadHeight;
			if (FMath::Abs(P[V].X) > HX + 1.0e-3 || FMath::Abs(P[V].Y) > HY + 1.0e-3) ++OutsideInner;
			if (Closed.Normals[Closed.Indices[Tri * 3 + V]].Z > -0.999f) ++BadNormal;
		}
		Area += 0.5 * FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Size();
	}
	TestEqual(TEXT("the bottom face lies on the house base"), BadHeight, 0);
	TestEqual(TEXT("and faces down"), BadNormal, 0);
	TestEqual(TEXT("and stays inside the inner wall skin (the wall slabs close their own bottoms)"), OutsideInner, 0);
	TestTrue(FString::Printf(TEXT("and covers the whole inner rectangle (%.1f cm^2)"), Area),
		FMath::IsNearlyEqual(Area, 4.0 * HX * HY, 1.0));

	// 墙厚超过房子一半：内皮翻过来，屋里没有空腔，不铺。
	Desc.WallThickness = 250.0f;
	Desc.bBottomFace = false;
	FCSGpuMeshCPUData ThickOpen;
	CSHouse_BuildBodySoup(Desc, ThickOpen);
	Desc.bBottomFace = true;
	FCSGpuMeshCPUData ThickClosed;
	CSHouse_BuildBodySoup(Desc, ThickClosed);
	TestEqual(TEXT("no bottom face when the walls leave no inside"), ThickClosed.Indices.Num(), ThickOpen.Indices.Num());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSHouseActor.h"
#include "CSHouseBrickWall.h"
#include "CSHouseFrame.h"
#include "CSHouseHeightHandleActor.h"
#include "CSHouseRidgeHandleActor.h"
#include "CSHouseRoof.h"
#include "CSHouseTile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Math/NumericLimits.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 脊长可调 / 双坡 + 山墙（2026-10-02，照 TG 反汇编，见 Docs/TinyGlade/RE_RoofRidgeGable.md）。
//
// 分四层钉：① TG 的参数换算（脊长 ↔ ridge_length_01、默认值、山墙悬挑量）；② 带逐边坡度倍率的求值器与骨架
// （等坡度时逐位退回原样）；③ 瓦在调过脊长的四坡 / 双坡上仍然贴着屋面、铺满、装得进容量；④ 山墙进房体三角汤、
// 脊长箭头拖 δ 脊端走 δ、拖到头就是山墙；⑥ 阶梯山墙上沿那一圈露砖（灰泥只做到屋面线）。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseRidgeTest_ 前缀。

FCSRoofDesc CSHouseRidgeTest_MakeRoof(double SizeX, double SizeY)
{
	FCSRoofDesc Desc;
	Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(SizeX, SizeY));
	Desc.EaveZ = 300.0f;
	Desc.Pitch = 35.0f;
	Desc.Overhang = 25.0f;
	return Desc;
}

/** 抖动全关、脊瓦关的瓦参数（同 `CSHouseTileTests.cpp` 的夹具：几何断言必须在确定的排布上做）。 */
CSHouseTile::FParams CSHouseRidgeTest_MakeTileParams()
{
	CSHouseTile::FParams Params;
	Params.RowPitch = 26.0f;
	Params.ColumnPitch = 30.0f;
	Params.RowOverlap = 1.6f;
	Params.ColumnOverlap = 1.06f;
	Params.StandOff = 0.0f;
	Params.ScaleJitter = 0.0f;
	Params.YawJitter = 0.0f;
	Params.LiftJitter = 0.0f;
	Params.Seed = 7;
	Params.RidgeCapScale = 0.0f;
	Params.Axes.UpSlope = 0;
	Params.Axes.AlongRow = 1;
	Params.Axes.Normal = 2;
	Params.Axes.NormalSign = 1.0f;
	Params.Axes.NativeSize = FVector3f(20.0f, 24.0f, 3.0f);
	return Params;
}

/** 屋面高度场的**独立**实现（不走求值器的任何一行）：逐条坡面写平面方程取最小。测试拿它当对照。 */
double CSHouseRidgeTest_BruteZ(const FCSRoofDesc& Roof, const FVector2D& P)
{
	const double TanP = FMath::Tan(FMath::DegreesToRadians(double(Roof.Pitch)));
	const double EaveOuterZ = double(Roof.EaveZ) - TanP * double(Roof.Overhang);
	double Min = TNumericLimits<double>::Max();
	for (int32 Edge = 0; Edge < Roof.Footprint.NumEdges(); ++Edge)
	{
		const double W = Roof.EdgeSteep.IsValidIndex(Edge) ? double(Roof.EdgeSteep[Edge]) : 1.0;
		if (W <= 0.0) continue;
		const FVector2D A = Roof.Footprint.Verts[Edge];
		const FVector2D B = Roof.Footprint.Verts[(Edge + 1) % Roof.Footprint.NumEdges()];
		const FVector2D U = (B - A).GetSafeNormal();
		const FVector2D In(-U.Y, U.X);
		// 这个坡面：从檐口外沿（内距 −Overhang，高 EaveOuterZ）起，每往里 1 cm 升 W·tan。
		Min = FMath::Min(Min, EaveOuterZ + W * TanP * (FVector2D::DotProduct(P - A, In) + double(Roof.Overhang)));
	}
	return Min;
}
}

// -----------------------------------------------------------------------------
// ① TG 的参数换算
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofRidgeParamsTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofRidgeParams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofRidgeParamsTest::RunTest(const FString& Parameters)
{
	// `RoofRidgeDims::calc`：脊长 = (1 − r)·0.1 m + r·顺脊边长。
	TestEqual(TEXT("r = 0 is the 10 cm stub"), CSHouseRoof_RidgeLengthFrom01(0.0, 600.0), 10.0);
	TestEqual(TEXT("r = 1 is the full house length"), CSHouseRoof_RidgeLengthFrom01(1.0, 600.0), 600.0);
	TestEqual(TEXT("r = 0.5 is halfway between 10 cm and the house length"), CSHouseRoof_RidgeLengthFrom01(0.5, 600.0), 305.0);

	// `inverse_ridge_01_param`：反函数，钳在 [0, 1]。**钳到 1 的那一下必须是恰好的 1.0** —— 山墙判的是相等。
	TestEqual(TEXT("the inverse recovers r"), CSHouseRoof_Ridge01FromLength(305.0, 600.0), 0.5);
	TestTrue(TEXT("dragging past the house end clamps to exactly 1"), CSHouseRoof_Ridge01FromLength(735.0, 600.0) == 1.0);
	TestTrue(TEXT("dragging to exactly the house length is exactly 1"), CSHouseRoof_Ridge01FromLength(600.0, 600.0) == 1.0);
	TestTrue(TEXT("dragging below the stub clamps to exactly 0"), CSHouseRoof_Ridge01FromLength(-50.0, 600.0) == 0.0);
	TestEqual(TEXT("a house no longer than the stub has no ridge parameter"), CSHouseRoof_Ridge01FromLength(5.0, 10.0), 0.0);
	for (const double R : { 0.0, 0.137, 0.5, 0.93, 1.0 })
	{
		TestTrue(FString::Printf(TEXT("round trip at r = %.3f"), R),
			FMath::IsNearlyEqual(CSHouseRoof_Ridge01FromLength(CSHouseRoof_RidgeLengthFrom01(R, 842.0), 842.0), R, 1.0e-12));
	}

	// `ridge_length_01_from_rectangle_ratio`，手算对照（公式见函数注释）：
	//   2 × 2 m：s = 0，(1 + 0 − 1.5) / 1.5 < 0 ⇒ 0（小正方形是个尖）
	//   10 × 10 m：s = 1，(1 + 1.5 − 1.5) / 1.5 = 2/3 ⇒ smoothstep = 20/27
	//   6 × 4 m：t = 375/525 ⇒ s = 0.801749，v = (1.5 + 1.5·s − 1.5) / 1.5 = s ⇒ smoothstep(s) = 0.897674
	//   11.34 × 3.57 m：长宽比 3.18，v > 1 ⇒ 恰好 1（TG 里这种房子一建出来就是双坡）
	TestEqual(TEXT("small square: a point"), CSHouseRoof_DefaultRidgeLength01(200.0, 200.0), 0.0f);
	TestTrue(TEXT("big square: 20/27"), FMath::IsNearlyEqual(CSHouseRoof_DefaultRidgeLength01(1000.0, 1000.0), 20.0f / 27.0f, 1.0e-5f));
	TestTrue(FString::Printf(TEXT("6 x 4 m: 0.8977 (got %.6f)"), CSHouseRoof_DefaultRidgeLength01(600.0, 400.0)),
		FMath::IsNearlyEqual(CSHouseRoof_DefaultRidgeLength01(600.0, 400.0), 0.897674f, 1.0e-4f));
	TestTrue(TEXT("long house: exactly 1 (a gable from the start)"), CSHouseRoof_DefaultRidgeLength01(1134.0, 357.0) == 1.0f);
	TestEqual(TEXT("the default is symmetric in the two sides"),
		CSHouseRoof_DefaultRidgeLength01(400.0, 600.0), CSHouseRoof_DefaultRidgeLength01(600.0, 400.0));
	TestEqual(TEXT("a degenerate rectangle has no ridge"), CSHouseRoof_DefaultRidgeLength01(0.0, 400.0), 0.0f);

	// 山墙悬挑量（`assemble_rectangular_roof`）：悬挑式 0.265 m + eave，阶梯式缩进 0.265 m；
	// eave = 2·(Overhang − 14 cm)，外挑不足 14 cm 时按 0。
	TestEqual(TEXT("overhanging gable at the default 25 cm eave"), CSHouseRoof_GableOverhang(ECSGableRoofStyle::Overhang, 25.0f), 48.5f);
	TestEqual(TEXT("overhanging gable with no eave"), CSHouseRoof_GableOverhang(ECSGableRoofStyle::Overhang, 10.0f), 26.5f);
	TestEqual(TEXT("stepped gable is inset regardless of the eave"), CSHouseRoof_GableOverhang(ECSGableRoofStyle::Stepped, 25.0f), -26.5f);
	// 枚举值与 TG 的 serde 变体表同序（存盘里的 `gable_style` 字节：0 = Stepped，1 = Overhang）。
	TestTrue(TEXT("the gable style values match TG's"), uint8(ECSGableRoofStyle::Stepped) == 0 && uint8(ECSGableRoofStyle::Overhang) == 1);

	// 脊参数只对矩形生效：朝向任意的矩形算，平行四边形与多边形不算。
	TestTrue(TEXT("MakeRect is a rectangle"), CSHouseRoof_IsRectangle(FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0))));
	FCSHouseFootprint Rotated;
	for (const FVector2D& V : FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0)).Verts) Rotated.Verts.Add(V.GetRotated(33.0));
	TestTrue(TEXT("a rotated rectangle is a rectangle"), CSHouseRoof_IsRectangle(Rotated));
	FCSHouseFootprint Skewed;
	Skewed.Verts = { FVector2D(-300, -200), FVector2D(300, -200), FVector2D(380, 200), FVector2D(-220, 200) };
	TestFalse(TEXT("a parallelogram is not"), CSHouseRoof_IsRectangle(Skewed));
	FCSHouseFootprint Triangle;
	Triangle.Verts = { FVector2D(-300, -200), FVector2D(300, -200), FVector2D(0, 250) };
	TestFalse(TEXT("a triangle is not"), CSHouseRoof_IsRectangle(Triangle));
	return true;
}

// -----------------------------------------------------------------------------
// ② 求值器与骨架：等坡度逐位不变；调过脊长的四坡；金字塔；双坡 + 山墙；脊向
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRoofRidgeEvalTest,
	"PCGPlugins.ComputeShaderGenerator.House.RoofRidgeEval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRoofRidgeEvalTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Plain = CSHouseRidgeTest_MakeRoof(600.0, 400.0);
	const float TanP = Plain.TanPitch();

	// 网格上逐点对照独立实现；顺带返回最大误差。
	auto WorstAgainstBrute = [](const FCSRoofDesc& Roof)
	{
		double Worst = 0.0;
		for (int32 X = -340; X <= 340; X += 17)
		for (int32 Y = -230; Y <= 230; Y += 13)
		{
			const FVector2D P(X, Y);
			Worst = FMath::Max(Worst, FMath::Abs(double(CSHouseRoof_EvalZ(Roof, P)) - CSHouseRidgeTest_BruteZ(Roof, P)));
		}
		return Worst;
	};

	// ---- (a) 倍率全 1 = 空表 = 旧签名：骨架与高度场**逐位**相同 ----
	{
		FCSRoofDesc Ones = Plain;
		Ones.EdgeSteep.Init(1.0f, 4);
		FCSRoofSkeleton Legacy, FromDesc, FromOnes;
		CSHouseRoof_BuildSkeleton(Plain.Footprint, double(Plain.Overhang), Legacy);
		CSHouseRoof_BuildSkeleton(Plain, FromDesc);
		CSHouseRoof_BuildSkeleton(Ones, FromOnes);
		bool bSame = Legacy.Arcs.Num() == FromDesc.Arcs.Num() && Legacy.Arcs.Num() == FromOnes.Arcs.Num()
			&& Legacy.MaxInset == FromDesc.MaxInset && Legacy.MaxInset == FromOnes.MaxInset
			&& Legacy.TopA == FromOnes.TopA && Legacy.TopB == FromOnes.TopB;
		for (int32 Index = 0; bSame && Index < Legacy.Arcs.Num(); ++Index)
		{
			for (const FCSRoofSkeleton* Other : { &FromDesc, &FromOnes })
			{
				const FCSRoofSkeletonArc& L = Legacy.Arcs[Index];
				const FCSRoofSkeletonArc& O = Other->Arcs[Index];
				bSame &= L.A == O.A && L.B == O.B && L.InsetA == O.InsetA && L.InsetB == O.InsetB
					&& L.FaceA == O.FaceA && L.FaceB == O.FaceB && !O.bVerge;
			}
		}
		TestTrue(TEXT("(a) all-ones multipliers reproduce the equal-pitch skeleton bit for bit"), bSame);

		int32 Differ = 0;
		for (int32 X = -340; X <= 340; X += 17)
		for (int32 Y = -230; Y <= 230; Y += 13)
		{
			const FVector2D P(X, Y);
			if (CSHouseRoof_EvalZ(Ones, P) != CSHouseRoof_EvalZ(Plain, P)) ++Differ;
			if (CSHouseRoof_EvalNormal(Ones, P) != CSHouseRoof_EvalNormal(Plain, P)) ++Differ;
			if (CSHouseRoof_IsUnderRoof(Ones, P) != CSHouseRoof_IsUnderRoof(Plain, P)) ++Differ;
		}
		TestEqual(TEXT("(a) ...and the same height field, normals and coverage bit for bit"), Differ, 0);

		double A0 = 0, A1 = 0, B0 = 0, B1 = 0;
		const bool bA = CSHouseRoof_FaceSpanAtInset(Plain.Footprint, 0, 73.0, A0, A1);
		const bool bB = CSHouseRoof_FaceSpanAtInset(Ones, 0, 73.0, B0, B1);
		TestTrue(TEXT("(a) ...and the same face rows bit for bit"), bA && bB && A0 == B0 && A1 == B1);
	}

	// ---- (b) 脊长恰好取等坡度的那个值（200）：倍率算出来就是 1 ----
	{
		FCSRoofDesc Roof = Plain;
		const float R = float(CSHouseRoof_Ridge01FromLength(200.0, 600.0));
		TestTrue(TEXT("(b) a rectangle takes the ridge"), CSHouseRoof_ApplyRidge(Roof, R, 0, 48.5f));
		TestEqual(TEXT("(b) four multipliers"), Roof.EdgeSteep.Num(), 4);
		TestTrue(TEXT("(b) the side slopes keep multiplier 1"), Roof.EdgeSteep.Num() == 4 && Roof.EdgeSteep[0] == 1.0f && Roof.EdgeSteep[2] == 1.0f);
		TestTrue(TEXT("(b) the equal-pitch ridge length gives end multipliers of 1"),
			Roof.EdgeSteep.Num() == 4 && FMath::IsNearlyEqual(Roof.EdgeSteep[1], 1.0f, 1.0e-4f) && FMath::IsNearlyEqual(Roof.EdgeSteep[3], 1.0f, 1.0e-4f));
		TestTrue(TEXT("(b) the height field matches the equal-pitch roof"), WorstAgainstBrute(Roof) < 1.0e-2);
	}

	// ---- (c) r = 0.5：脊 305 cm，两端坡面比两侧陡 ----
	{
		FCSRoofDesc Roof = Plain;
		CSHouseRoof_ApplyRidge(Roof, 0.5f, 0, 48.5f);
		const double Ridge = 305.0;
		// 倍率 = (半跨 + 外挑) / (半长 + 外挑 − 脊长 / 2) = 225 / 172.5。
		TestTrue(TEXT("(c) end multiplier is the run ratio"),
			Roof.EdgeSteep.Num() == 4 && FMath::IsNearlyEqual(Roof.EdgeSteep[1], float(225.0 / 172.5), 1.0e-5f) && Roof.EdgeSteep[3] == Roof.EdgeSteep[1]);
		TestFalse(TEXT("(c) not a gable"), Roof.HasGable());

		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof, S);
		TestTrue(FString::Printf(TEXT("(c) the top ridge is the requested length (%.4f)"), FVector2D::Distance(S.TopA, S.TopB)),
			S.TopA.Equals(FVector2D(-Ridge * 0.5, 0.0), 1.0e-3) && S.TopB.Equals(FVector2D(Ridge * 0.5, 0.0), 1.0e-3));
		// 脊高只由主坡面定：与脊长无关。
		TestTrue(TEXT("(c) the ridge height does not depend on the ridge length"),
			FMath::IsNearlyEqual(S.MaxInset, 200.0, 1.0e-6) && FMath::IsNearlyEqual(CSHouseRoof_RidgeZ(Roof), 300.0f + TanP * 200.0f, 1.0e-3f));
		TestEqual(TEXT("(c) four hips plus one ridge"), S.Arcs.Num(), 5);
		for (int32 Corner = 0; Corner < S.NumHips && Corner < S.Arcs.Num(); ++Corner)
		{
			const FCSRoofSkeletonArc& Hip = S.Arcs[Corner];
			// 四条檐口外沿同高 ⇒ 角斜脊仍然从檐口外角（±325, ±225）起。
			TestTrue(FString::Printf(TEXT("(c) hip %d starts on the eave outer corner"), Corner),
				FMath::IsNearlyEqual(FMath::Abs(Hip.A.X), 325.0, 1.0e-6) && FMath::IsNearlyEqual(FMath::Abs(Hip.A.Y), 225.0, 1.0e-6)
				&& FMath::IsNearlyEqual(Hip.InsetA, -25.0, 1.0e-9));
			TestTrue(FString::Printf(TEXT("(c) hip %d ends on its ridge end"), Corner),
				FMath::IsNearlyEqual(FMath::Abs(Hip.B.X), Ridge * 0.5, 1.0e-3) && FMath::IsNearlyZero(Hip.B.Y, 1.0e-6)
				&& FMath::IsNearlyEqual(Hip.InsetB, 200.0, 1.0e-6) && !Hip.bVerge);
		}
		// 骨架的每条弧上两侧坡面等高，且就是屋面在那里的高度。
		int32 OffArc = 0;
		for (const FCSRoofSkeletonArc& Arc : S.Arcs)
		{
			const FCSHouseEdgeFrame FA = CSHouse_GetEdge(Arc.FaceA, Roof.Footprint, 0.0f);
			const FCSHouseEdgeFrame FB = CSHouse_GetEdge(Arc.FaceB, Roof.Footprint, 0.0f);
			for (int32 K = 0; K <= 8; ++K)
			{
				const double Alpha = double(K) / 8.0;
				const FVector2D P = FMath::Lerp(Arc.A, Arc.B, Alpha);
				const double Want = FMath::Lerp(Arc.InsetA, Arc.InsetB, Alpha);
				if (!FMath::IsNearlyEqual(Roof.InsetDistance(P), Want, 1.0e-6)
					|| !FMath::IsNearlyEqual(Roof.EquivalentInset(Arc.FaceA, FVector2D::DotProduct(P - FA.Start, FA.In)), Want, 1.0e-6)
					|| !FMath::IsNearlyEqual(Roof.EquivalentInset(Arc.FaceB, FVector2D::DotProduct(P - FB.Start, FB.In)), Want, 1.0e-6)) ++OffArc;
			}
		}
		TestEqual(TEXT("(c) both faces of every arc meet at the arc's height"), OffArc, 0);

		TestTrue(FString::Printf(TEXT("(c) the height field matches the independent plane model (worst %.5f)"), WorstAgainstBrute(Roof)),
			WorstAgainstBrute(Roof) < 1.0e-2);
		// 四条檐口外沿同高（放样面的下口是平的）—— 两端坡面再陡，檐口也不会掉下去。
		const float EaveOuterZ = CSHouseRoof_EaveOuterZ(Roof);
		TestTrue(TEXT("(c) the end eave sits at the same height as the side eave"),
			FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(325.0, 0.0)), EaveOuterZ, 1.0e-3f)
			&& FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(0.0, -225.0)), EaveOuterZ, 1.0e-3f));
		TestTrue(TEXT("(c) flat along the ridge, falling away past its end"),
			FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(140.0, 0.0)), CSHouseRoof_RidgeZ(Roof), 1.0e-3f)
			&& CSHouseRoof_EvalZ(Roof, FVector2D(200.0, 0.0)) < CSHouseRoof_RidgeZ(Roof) - 1.0f);

		// 端坡面的法线比主坡面更"躺"向外：坡度 = atan(倍率 · tan)。
		const FVector NEnd = CSHouseRoof_EvalNormal(Roof, FVector2D(290.0, 0.0));
		const double WantTan = double(Roof.EdgeSteep[1]) * double(TanP);
		TestTrue(TEXT("(c) the end face normal carries the steeper pitch"),
			FMath::IsNearlyZero(NEnd.Y, 1.0e-6) && NEnd.X > 0.0 && FMath::IsNearlyEqual(NEnd.X / NEnd.Z, WantTan, 1.0e-4));
		const FVector NSide = CSHouseRoof_EvalNormal(Roof, FVector2D(0.0, -120.0));
		TestTrue(TEXT("(c) the side face normal keeps the nominal pitch"),
			FMath::IsNearlyZero(NSide.X, 1.0e-6) && FMath::IsNearlyEqual(-NSide.Y / NSide.Z, double(TanP), 1.0e-4));

		// 坡面排区间：主坡面在脊高处剩下的就是脊；端坡面收成脊端那个点。
		double T0 = 0, T1 = 0;
		TestTrue(TEXT("(c) the side face narrows to the ridge at the top"),
			CSHouseRoof_FaceSpanAtInset(Roof, 0, 199.99, T0, T1)
			&& FMath::IsNearlyEqual(T1 - T0, Ridge, 0.1) && FMath::IsNearlyEqual((T0 + T1) * 0.5, 300.0, 1.0e-3));
		TestTrue(TEXT("(c) the end face spans the whole eave at its outer edge"),
			CSHouseRoof_FaceSpanAtInset(Roof, 1, -25.0, T0, T1) && FMath::IsNearlyEqual(T0, -25.0, 1.0e-6) && FMath::IsNearlyEqual(T1, 425.0, 1.0e-6));
	}

	// ---- (d) r = 0：一个尖，位置在中心，四条角斜脊都汇过去 ----
	{
		FCSRoofDesc Roof = Plain;
		CSHouseRoof_ApplyRidge(Roof, 0.0f, 0, 48.5f);
		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof, S);
		TestTrue(FString::Printf(TEXT("(d) the 10 cm stub is a point (%.5f)"), FVector2D::Distance(S.TopA, S.TopB)),
			FVector2D::Distance(S.TopA, S.TopB) < 1.0e-3 && S.TopA.Equals(FVector2D::ZeroVector, 1.0e-3));
		TestTrue(TEXT("(d) the apex is as high as any ridge would be"), FMath::IsNearlyEqual(S.MaxInset, 200.0, 1.0e-3));
		// 两端坡面比两侧**缓**（进深 325 > 225）：倍率 < 1。
		TestTrue(TEXT("(d) the end faces are shallower than the side faces"), Roof.EdgeSteep.Num() == 4 && Roof.EdgeSteep[1] < 1.0f && Roof.EdgeSteep[1] > 0.0f);
		TestTrue(TEXT("(d) the height field matches the independent plane model"), WorstAgainstBrute(Roof) < 1.0e-2);
	}

	// ---- (e) r = 1：双坡 + 山墙 ----
	{
		FCSRoofDesc Roof = Plain;
		const float Gable = CSHouseRoof_GableOverhang(ECSGableRoofStyle::Overhang, Roof.Overhang);   // 48.5
		CSHouseRoof_ApplyRidge(Roof, 1.0f, 0, Gable);
		TestTrue(TEXT("(e) the two ends are gables, the two sides are slopes"),
			Roof.EdgeSteep.Num() == 4 && Roof.EdgeSteep[0] == 1.0f && Roof.EdgeSteep[2] == 1.0f && Roof.IsGable(1) && Roof.IsGable(3)
			&& !Roof.IsGable(0) && !Roof.IsGable(2) && Roof.HasGable());
		TestEqual(TEXT("(e) the gable overhang is carried on the desc"), Roof.GableOverhang, Gable);

		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof, S);
		TestTrue(TEXT("(e) the ridge height is exactly half the span"), S.MaxInset == 200.0);
		// 脊一直通到山墙外：两端各伸出 48.5。
		TestTrue(FString::Printf(TEXT("(e) the ridge runs the whole roof, gable overhang included (%.3f .. %.3f)"), S.TopA.X, S.TopB.X),
			S.TopA.Equals(FVector2D(-300.0 - Gable, 0.0), 1.0e-6) && S.TopB.Equals(FVector2D(300.0 + Gable, 0.0), 1.0e-6));
		TestEqual(TEXT("(e) four verges plus one ridge"), S.Arcs.Num(), 5);
		int32 Verges = 0;
		for (int32 Corner = 0; Corner < S.NumHips && Corner < S.Arcs.Num(); ++Corner)
		{
			const FCSRoofSkeletonArc& Arc = S.Arcs[Corner];
			if (Arc.bVerge) ++Verges;
			// 山墙侧斜边：从檐口外角沿山墙面（x = ±348.5）爬到脊端。
			TestTrue(FString::Printf(TEXT("(e) verge %d climbs the gable plane"), Corner),
				FMath::IsNearlyEqual(FMath::Abs(Arc.A.X), 300.0 + Gable, 1.0e-6) && FMath::IsNearlyEqual(FMath::Abs(Arc.A.Y), 225.0, 1.0e-6)
				&& FMath::IsNearlyEqual(FMath::Abs(Arc.B.X), 300.0 + Gable, 1.0e-6) && FMath::IsNearlyZero(Arc.B.Y, 1.0e-6)
				&& FMath::IsNearlyEqual(Arc.InsetA, -25.0, 1.0e-9) && FMath::IsNearlyEqual(Arc.InsetB, 200.0, 1.0e-9));
		}
		TestEqual(TEXT("(e) all four corner arcs are verges, the ridge is not"), Verges, 4);
		TestFalse(TEXT("(e) the ridge arc is a real ridge"), S.Arcs.Num() == 5 && S.Arcs[4].bVerge);

		// 高度场沿脊向处处一样：只有两个坡面。
		TestTrue(TEXT("(e) the roof is a straight extrusion along the ridge"),
			CSHouseRoof_EvalZ(Roof, FVector2D(-340.0, 77.0)) == CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 77.0))
			&& CSHouseRoof_EvalZ(Roof, FVector2D(340.0, 77.0)) == CSHouseRoof_EvalZ(Roof, FVector2D(0.0, 77.0))
			&& FMath::IsNearlyEqual(CSHouseRoof_EvalZ(Roof, FVector2D(340.0, 0.0)), CSHouseRoof_RidgeZ(Roof), 1.0e-3f));
		TestTrue(TEXT("(e) the height field matches the independent plane model"), WorstAgainstBrute(Roof) < 1.0e-2);

		// 覆盖：山墙那一头算到墙外 48.5，两侧照旧算到檐口外沿 25。
		TestTrue(TEXT("(e) under the gable overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(347.0, 0.0)));
		TestFalse(TEXT("(e) past the gable overhang"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(350.0, 0.0)));
		TestTrue(TEXT("(e) under the side eave"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 224.0)));
		TestFalse(TEXT("(e) past the side eave"), CSHouseRoof_IsUnderRoof(Roof, FVector2D(0.0, 226.0)));

		// 排：两个主坡面每一排都一样长，一直通到山墙外；山墙边没有坡面。
		double T0 = 0, T1 = 0, U0 = 0, U1 = 0;
		TestTrue(TEXT("(e) a side row runs past both gables"),
			CSHouseRoof_FaceSpanAtInset(Roof, 0, -25.0, T0, T1) && FMath::IsNearlyEqual(T0, -double(Gable), 1.0e-6) && FMath::IsNearlyEqual(T1, 600.0 + double(Gable), 1.0e-6));
		TestTrue(TEXT("(e) rows do not narrow toward the ridge"),
			CSHouseRoof_FaceSpanAtInset(Roof, 2, 180.0, U0, U1) && FMath::IsNearlyEqual(U1 - U0, T1 - T0, 1.0e-6));
		TestFalse(TEXT("(e) a gable edge has no slope rows"), CSHouseRoof_FaceSpanAtInset(Roof, 1, 0.0, T0, T1));
		TestFalse(TEXT("(e) no rows above the ridge"), CSHouseRoof_FaceSpanAtInset(Roof, 0, 201.0, T0, T1));

		// 法线：山墙附近仍然是主坡面的法线（那里没有第二个坡面来平均）。
		const FVector N = CSHouseRoof_EvalNormal(Roof, FVector2D(330.0, -100.0));
		TestTrue(TEXT("(e) the normal near the gable is the side slope's"),
			FMath::IsNearlyZero(N.X, 1.0e-6) && N.Y < 0.0 && FMath::IsNearlyEqual(-N.Y / N.Z, double(TanP), 1.0e-4));

		// 山墙的上沿 = 屋面沿墙线的剖面：等腰三角形，顶点在脊正下方。
		TArray<FVector2D> Profile;
		TestTrue(TEXT("(e) a gable edge has a wall profile"), CSHouseRoof_BuildGableProfile(Roof, 1, Profile));
		TestTrue(TEXT("(e) the gable is an isosceles triangle up to the ridge"),
			Profile.Num() == 3 && Profile[0].Equals(FVector2D(0.0, 0.0), 1.0e-6) && Profile[1].Equals(FVector2D(200.0, 200.0), 1.0e-6)
			&& Profile[2].Equals(FVector2D(400.0, 0.0), 1.0e-6));
		TestFalse(TEXT("(e) a slope edge has no gable profile"), CSHouseRoof_BuildGableProfile(Roof, 0, Profile));
		TestTrue(TEXT("(e) the opposite gable mirrors it"),
			CSHouseRoof_BuildGableProfile(Roof, 3, Profile) && Profile.Num() == 3 && Profile[1].Equals(FVector2D(200.0, 200.0), 1.0e-6));

		// 阶梯式：屋面缩在山墙里面 26.5。
		FCSRoofDesc Stepped = Plain;
		CSHouseRoof_ApplyRidge(Stepped, 1.0f, 0, CSHouseRoof_GableOverhang(ECSGableRoofStyle::Stepped, Stepped.Overhang));
		FCSRoofSkeleton SteppedSkeleton;
		CSHouseRoof_BuildSkeleton(Stepped, SteppedSkeleton);
		TestTrue(TEXT("(e) a stepped gable keeps the roof inside the gable walls"),
			SteppedSkeleton.TopB.Equals(FVector2D(273.5, 0.0), 1.0e-6) && !CSHouseRoof_IsUnderRoof(Stepped, FVector2D(280.0, 0.0))
			&& CSHouseRoof_IsUnderRoof(Stepped, FVector2D(270.0, 0.0)));
	}

	// ---- (f) 脊向：脊沿短轴（局部 Y）—— 脊高改由长边的一半定 ----
	{
		FCSRoofDesc Roof = Plain;
		CSHouseRoof_ApplyRidge(Roof, 1.0f, 1, 48.5f);
		TestTrue(TEXT("(f) the gables move to the long sides"), Roof.IsGable(0) && Roof.IsGable(2) && !Roof.IsGable(1) && !Roof.IsGable(3));
		FCSRoofSkeleton S;
		CSHouseRoof_BuildSkeleton(Roof, S);
		TestTrue(TEXT("(f) the ridge runs along Y"),
			S.TopA.Equals(FVector2D(0.0, -248.5), 1.0e-6) && S.TopB.Equals(FVector2D(0.0, 248.5), 1.0e-6));
		TestTrue(TEXT("(f) the ridge height comes from half the long side"), S.MaxInset == 300.0);
		TArray<FVector2D> Profile;
		TestTrue(TEXT("(f) the gable now spans the long side"),
			CSHouseRoof_BuildGableProfile(Roof, 0, Profile) && Profile.Num() == 3 && Profile[1].Equals(FVector2D(300.0, 300.0), 1.0e-6));

		FCSRoofDesc Hip = Plain;
		CSHouseRoof_ApplyRidge(Hip, 0.5f, 1, 48.5f);
		CSHouseRoof_BuildSkeleton(Hip, S);
		// 顺脊边长 400 ⇒ 脊长 205。
		TestTrue(FString::Printf(TEXT("(f) a hip along Y has the requested ridge (%.4f)"), FVector2D::Distance(S.TopA, S.TopB)),
			S.TopA.Equals(FVector2D(0.0, -102.5), 1.0e-3) && S.TopB.Equals(FVector2D(0.0, 102.5), 1.0e-3) && FMath::IsNearlyEqual(S.MaxInset, 300.0, 1.0e-6));
		TestTrue(TEXT("(f) ...and still matches the plane model"), WorstAgainstBrute(Hip) < 1.0e-2);
	}

	// ---- (g) 不是矩形：脊参数不生效，倍率清空 ----
	{
		FCSRoofDesc Roof = Plain;
		Roof.EdgeSteep.Init(2.0f, 4);
		Roof.GableOverhang = 12.0f;
		Roof.Footprint.Verts = { FVector2D(-320, -180), FVector2D(260, -220), FVector2D(380, 60), FVector2D(40, 290), FVector2D(-290, 150) };
		TestFalse(TEXT("(g) a pentagon does not take a ridge parameter"), CSHouseRoof_ApplyRidge(Roof, 1.0f, 0, 48.5f));
		TestTrue(TEXT("(g) ...and is left as an equal-pitch roof"), Roof.EdgeSteep.IsEmpty() && Roof.GableOverhang == 0.0f && !Roof.HasGable());
	}

	// ---- (h) 零外挑、差一点到 1：端坡面进深趋零，按山墙收，不出发散的倍率 ----
	{
		FCSRoofDesc Roof = Plain;
		Roof.Overhang = 0.0f;
		CSHouseRoof_ApplyRidge(Roof, 0.9995f, 0, 0.0f);
		TestTrue(TEXT("(h) a vanishing end run collapses into a gable instead of a huge multiplier"), Roof.IsGable(1) && Roof.IsGable(3));
		FCSRoofDesc Fine = Plain;
		Fine.Overhang = 0.0f;
		CSHouseRoof_ApplyRidge(Fine, 0.98f, 0, 0.0f);
		TestTrue(TEXT("(h) a short but real end run stays a hip"), !Fine.HasGable() && Fine.EdgeSteep[1] > 1.0f && FMath::IsFinite(Fine.EdgeSteep[1]));
	}
	return true;
}

// -----------------------------------------------------------------------------
// ③ 瓦：调过脊长的四坡 / 双坡上仍然贴着屋面、铺到檐口与脊、装得进容量
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileRidgeLengthTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileRidgeLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileRidgeLengthTest::RunTest(const FString& Parameters)
{
	const CSHouseTile::FParams Params = CSHouseRidgeTest_MakeTileParams();

	// ---- 倍率全 1 与空表：瓦逐位相同（等坡度的排布一个 bit 都不许变） ----
	{
		const FCSRoofDesc Plain = CSHouseRidgeTest_MakeRoof(600.0, 400.0);
		FCSRoofDesc Ones = Plain;
		Ones.EdgeSteep.Init(1.0f, 4);
		CSHouseTile::FParams WithCaps = Params;
		WithCaps.RidgeCapScale = 1.15f;
		WithCaps.LengthJitter = 0.2f;
		TArray<CSHouseTile::FRecord> A, B;
		CSHouseTile::BuildPlan(Plain, FTransform::Identity, WithCaps, A);
		CSHouseTile::BuildPlan(Ones, FTransform::Identity, WithCaps, B);
		int32 Differ = A.Num() == B.Num() ? 0 : 1;
		for (int32 Index = 0; Index < FMath::Min(A.Num(), B.Num()); ++Index)
		{
			const CSHouseTile::FRecord& L = A[Index];
			const CSHouseTile::FRecord& R = B[Index];
			if (L.WorldPos != R.WorldPos || L.Random01 != R.Random01 || L.AxisX != R.AxisX || L.AxisY != R.AxisY || L.AxisZ != R.AxisZ
				|| L.SizeX != R.SizeX || L.SizeY != R.SizeY || L.SizeZ != R.SizeZ || L.ClipPlane != R.ClipPlane) ++Differ;
		}
		TestEqual(FString::Printf(TEXT("all-ones multipliers tile exactly like the equal-pitch roof (%d tiles)"), A.Num()), Differ, 0);
		TestEqual(TEXT("...with the same capacity bound"), CSHouseTile::MaxTilesBound(Ones, WithCaps), CSHouseTile::MaxTilesBound(Plain, WithCaps));
	}

	struct FCase { double SizeX, SizeY; float Ridge01; int32 Axis; const TCHAR* What; };
	const FCase Cases[] = {
		{  600, 400, 0.0f,  0, TEXT("6x4 pyramid") },
		{  600, 400, 0.25f, 0, TEXT("6x4 short ridge") },
		{  600, 400, 0.5f,  0, TEXT("6x4 half ridge") },
		{  600, 400, 0.93f, 0, TEXT("6x4 nearly a gable (steep ends)") },
		{  600, 400, 1.0f,  0, TEXT("6x4 gable") },
		{  600, 400, 0.6f,  1, TEXT("6x4 ridge across the short axis") },
		{  600, 400, 1.0f,  1, TEXT("6x4 gable across the short axis") },
		{ 1134, 357, 1.0f,  0, TEXT("long gable") },
		{  400, 400, 1.0f,  0, TEXT("square gable") },
	};
	for (const FCase& Case : Cases)
	{
		FCSRoofDesc Roof = CSHouseRidgeTest_MakeRoof(Case.SizeX, Case.SizeY);
		CSHouseRoof_ApplyRidge(Roof, Case.Ridge01, Case.Axis, CSHouseRoof_GableOverhang(ECSGableRoofStyle::Overhang, Roof.Overhang));

		TArray<CSHouseTile::FRecord> Tiles;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
		TestTrue(FString::Printf(TEXT("[%s] the roof gets tiled (%d)"), Case.What, Tiles.Num()), Tiles.Num() > 40);

		int32 OffSurface = 0, Outside = 0, BadNormal = 0, BadBasis = 0;
		float Lowest = TNumericLimits<float>::Max(), Highest = -TNumericLimits<float>::Max();
		int32 PerFace[4] = { 0, 0, 0, 0 };
		for (const CSHouseTile::FRecord& R : Tiles)
		{
			const FVector2D XY(R.WorldPos.X, R.WorldPos.Y);
			if (!FMath::IsNearlyEqual(float(R.WorldPos.Z), CSHouseRoof_EvalZ(Roof, XY), 0.02f)) ++OffSurface;
			if (!CSHouseRoof_IsUnderRoof(Roof, XY)) ++Outside;
			const FVector AxisX(R.AxisX), AxisY(R.AxisY), AxisZ(R.AxisZ);
			if (AxisZ.Z <= 0.0 || FVector::DotProduct(AxisZ, CSHouseRoof_EvalNormal(Roof, XY)) < 0.70) ++BadNormal;
			if (FMath::Abs(FVector::DotProduct(AxisX, AxisY)) > 1.0e-3 || FMath::Abs(FVector::DotProduct(AxisY, AxisZ)) > 1.0e-3
				|| FMath::Abs(FVector::DotProduct(AxisX, AxisZ)) > 1.0e-3
				|| FVector::DotProduct(FVector::CrossProduct(AxisX, AxisY), AxisZ) < 0.9) ++BadBasis;
			Lowest = FMath::Min(Lowest, float(R.WorldPos.Z));
			Highest = FMath::Max(Highest, float(R.WorldPos.Z));

			// 属于哪个坡面：法线的水平分量指向哪条边的外侧。
			int32 Face = INDEX_NONE;
			double Best = 0.5;
			for (int32 Edge = 0; Edge < 4; ++Edge)
			{
				const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Roof.Footprint, 0.0f);
				const double Along = FVector2D::DotProduct(FVector2D(AxisZ.X, AxisZ.Y).GetSafeNormal(), -F.In);
				if (Along > Best) { Best = Along; Face = Edge; }
			}
			if (Face != INDEX_NONE) ++PerFace[Face];
		}
		TestEqual(FString::Printf(TEXT("[%s] every tile sits on the roof surface"), Case.What), OffSurface, 0);
		TestEqual(FString::Printf(TEXT("[%s] no tile lands outside the roof outline"), Case.What), Outside, 0);
		TestEqual(FString::Printf(TEXT("[%s] every tile faces its own slope"), Case.What), BadNormal, 0);
		TestEqual(FString::Printf(TEXT("[%s] every tile basis is orthonormal and right-handed"), Case.What), BadBasis, 0);

		// 最低一排在檐口外挑段上，最高一排贴着脊。
		const float RidgeZ = CSHouseRoof_RidgeZ(Roof);
		TestTrue(FString::Printf(TEXT("[%s] the lowest course sits on the overhang (%.1f)"), Case.What, Lowest),
			Lowest < Roof.EaveZ && Lowest >= CSHouseRoof_EaveOuterZ(Roof) - 0.01f);
		TestTrue(FString::Printf(TEXT("[%s] the top course reaches the ridge (%.1f vs %.1f)"), Case.What, Highest, RidgeZ),
			Highest > RidgeZ - 26.0f && Highest <= RidgeZ + 0.01f);

		for (int32 Edge = 0; Edge < 4; ++Edge)
		{
			if (Roof.IsGable(Edge))
			{
				TestEqual(FString::Printf(TEXT("[%s] gable edge %d carries no tiles"), Case.What, Edge), PerFace[Edge], 0);
			}
			else
			{
				TestTrue(FString::Printf(TEXT("[%s] slope %d is tiled (%d)"), Case.What, Edge, PerFace[Edge]), PerFace[Edge] > 5);
			}
		}
		TestEqual(FString::Printf(TEXT("[%s] opposite slopes carry the same number of tiles"), Case.What), PerFace[0], PerFace[2]);
		TestEqual(FString::Printf(TEXT("[%s] opposite ends carry the same number of tiles"), Case.What), PerFace[1], PerFace[3]);

		// 容量：真排的不超过上界；上界不离谱。带脊瓦再对一次（山墙侧斜边不出斜脊板，但容量照样要装得下）。
		const int32 Bound = CSHouseTile::MaxTilesBound(Roof, Params);
		TestTrue(FString::Printf(TEXT("[%s] the tiles fit the bound (%d <= %d)"), Case.What, Tiles.Num(), Bound), Tiles.Num() <= Bound);
		TestTrue(FString::Printf(TEXT("[%s] the bound is not absurdly high (%d <= 4 x %d)"), Case.What, Bound, Tiles.Num()),
			Bound <= FMath::Max(Tiles.Num(), 1) * 4);
		CSHouseTile::FParams WithCaps = Params;
		WithCaps.RidgeCapScale = 1.15f;
		TArray<CSHouseTile::FRecord> Capped;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, WithCaps, Capped);
		TestTrue(FString::Printf(TEXT("[%s] with ridge caps the plan still fits its bound (%d <= %d)"), Case.What, Capped.Num(),
			CSHouseTile::MaxTilesBound(Roof, WithCaps)), Capped.Num() <= CSHouseTile::MaxTilesBound(Roof, WithCaps));
	}

	// ---- 双坡的细节：排一样长、通到山墙外；脊瓦只有水平那一条 ----
	{
		FCSRoofDesc Roof = CSHouseRidgeTest_MakeRoof(600.0, 400.0);
		const float Gable = CSHouseRoof_GableOverhang(ECSGableRoofStyle::Overhang, Roof.Overhang);
		CSHouseRoof_ApplyRidge(Roof, 1.0f, 0, Gable);

		TArray<CSHouseTile::FRecord> Tiles;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
		// 每一排的列数相同（坡面是矩形）：按排高分组数瓦，偶数排 N 片、奇数排错半片 N + 1 片。
		TMap<int32, int32> PerCourse;
		double MinX = 1.0e9, MaxX = -1.0e9;
		for (const CSHouseTile::FRecord& R : Tiles)
		{
			if (R.WorldPos.Y < 0.0f) ++PerCourse.FindOrAdd(FMath::RoundToInt(R.WorldPos.Z));
			MinX = FMath::Min(MinX, double(R.WorldPos.X));
			MaxX = FMath::Max(MaxX, double(R.WorldPos.X));
		}
		int32 Fewest = MAX_int32, Most = 0;
		for (const TPair<int32, int32>& Course : PerCourse)
		{
			Fewest = FMath::Min(Fewest, Course.Value);
			Most = FMath::Max(Most, Course.Value);
		}
		TestTrue(FString::Printf(TEXT("gable: every course has the same number of tiles, staggered by one (%d..%d)"), Fewest, Most),
			PerCourse.Num() > 3 && Most - Fewest == 1);
		TestTrue(FString::Printf(TEXT("gable: the courses run out over the gable walls (%.1f .. %.1f)"), MinX, MaxX),
			MinX < -300.0 && MaxX > 300.0 && MinX >= -300.0 - double(Gable) && MaxX <= 300.0 + double(Gable));

		// 瓦的裁剪面：离山墙近的瓦切在山墙外那条线上（世界 X = ±348.5 的竖直面）。
		// 每片瓦只带一张裁剪面、取最近的那条坡面边界 —— 贴着脊的那几片离脊更近，切的是脊线，不在这条断言里。
		int32 VergeClipped = 0, WrongPlane = 0;
		for (const CSHouseTile::FRecord& R : Tiles)
		{
			if (FMath::Abs(R.WorldPos.X) < 300.0f) continue;
			if (FMath::Abs(R.WorldPos.Y) <= 300.0f + Gable - FMath::Abs(R.WorldPos.X)) continue;
			if (R.ClipPlane.X <= 0.0f) { ++WrongPlane; continue; }
			const float Angle = R.ClipPlane.X - 4.0f;
			const FVector2D N(FMath::Cos(Angle), FMath::Sin(Angle));
			// 保留 N·P + D ≤ 0：山墙外那条线上恰好为 0。
			const double AtVerge = N.X * (R.WorldPos.X > 0.0f ? 300.0 + double(Gable) : -300.0 - double(Gable)) + N.Y * double(R.WorldPos.Y) + double(R.ClipPlane.Y);
			if (FMath::Abs(N.Y) < 1.0e-3 && FMath::Abs(AtVerge) < 0.05) ++VergeClipped; else ++WrongPlane;
		}
		TestTrue(FString::Printf(TEXT("gable: tiles over the gable overhang are clipped on the verge line (%d, %d wrong)"), VergeClipped, WrongPlane),
			VergeClipped > 0 && WrongPlane == 0);

		CSHouseTile::FParams WithCaps = Params;
		WithCaps.RidgeCapScale = 1.15f;
		TArray<CSHouseTile::FRecord> Capped;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, WithCaps, Capped);
		int32 Level = 0;
		double CapMinX = 1.0e9, CapMaxX = -1.0e9;
		// 封边板：TG 的四条角线盖板在山墙时退化成屋面两端的斜边，同一块窄板顺着它铺。四条边各一串。
		int32 Verge[4] = { 0, 0, 0, 0 };
		int32 OffVerge = 0, NotOnSlope = 0;
		float VergeLow = TNumericLimits<float>::Max(), VergeHigh = -TNumericLimits<float>::Max();
		const float SinP = Roof.SinPitch(), CosP = Roof.CosPitch();
		for (int32 Index = Tiles.Num(); Index < Capped.Num(); ++Index)
		{
			const CSHouseTile::FRecord& R = Capped[Index];
			// 恒等排列：局部 X = 板的长轴。水平脊的板长轴是水平的；封边板的长轴顺着坡往上爬。
			if (FMath::Abs(R.AxisX.Z) <= 1.0e-3f)
			{
				++Level;
				CapMinX = FMath::Min(CapMinX, double(R.WorldPos.X) - double(R.SizeX) * 0.5);
				CapMaxX = FMath::Max(CapMaxX, double(R.WorldPos.X) + double(R.SizeX) * 0.5);
				continue;
			}
			++Verge[(R.WorldPos.X > 0.0f ? 0 : 1) + (R.WorldPos.Y > 0.0f ? 0 : 2)];
			// 板心在山墙外那条线的正上方（板只沿自己坡面的法线抬起，法线没有沿脊分量）。
			if (!FMath::IsNearlyEqual(FMath::Abs(R.WorldPos.X), 300.0f + Gable, 0.05f)) ++OffVerge;
			// 板躺在它那个坡面上：法线 = 坡面法线（不是两面平均），长轴垂直于脊。
			const float Side = R.WorldPos.Y > 0.0f ? 1.0f : -1.0f;
			if (!FVector3f(R.AxisZ).Equals(FVector3f(0.0f, Side * SinP, CosP), 1.0e-3f) || FMath::Abs(R.AxisX.X) > 1.0e-3f) ++NotOnSlope;
			VergeLow = FMath::Min(VergeLow, R.WorldPos.Z);
			VergeHigh = FMath::Max(VergeHigh, R.WorldPos.Z);
		}
		TestTrue(FString::Printf(TEXT("gable: each of the four verges carries a run of edge boards (%d / %d / %d / %d)"),
			Verge[0], Verge[1], Verge[2], Verge[3]),
			Verge[0] >= 3 && Verge[0] == Verge[1] && Verge[0] == Verge[2] && Verge[0] == Verge[3]);
		TestEqual(TEXT("gable: the edge boards sit on the verge line"), OffVerge, 0);
		TestEqual(TEXT("gable: the edge boards lie on their own slope"), NotOnSlope, 0);
		TestTrue(FString::Printf(TEXT("gable: the edge boards climb from the eave to the ridge (%.1f .. %.1f)"), VergeLow, VergeHigh),
			VergeLow < Roof.EaveZ + 30.0f && VergeHigh > CSHouseRoof_RidgeZ(Roof) - 45.0f);
		TestTrue(FString::Printf(TEXT("gable: the ridge caps cover the whole ridge (%d caps, %.1f .. %.1f)"), Level, CapMinX, CapMaxX),
			Level >= 8 && CapMinX <= -300.0 - double(Gable) + 1.0 && CapMaxX >= 300.0 + double(Gable) - 1.0);

		// 调过脊长的四坡：四条角上的弧还是斜脊盖板，骑在两坡的交线上（法线是两面的平均，带沿脊分量）。
		FCSRoofDesc Hip = CSHouseRidgeTest_MakeRoof(600.0, 400.0);
		CSHouseRoof_ApplyRidge(Hip, 0.5f, 0, Gable);
		TArray<CSHouseTile::FRecord> HipTiles, HipCapped;
		CSHouseTile::BuildPlan(Hip, FTransform::Identity, Params, HipTiles);
		CSHouseTile::BuildPlan(Hip, FTransform::Identity, WithCaps, HipCapped);
		int32 HipCaps = 0, FlatNormal = 0;
		for (int32 Index = HipTiles.Num(); Index < HipCapped.Num(); ++Index)
		{
			const CSHouseTile::FRecord& R = HipCapped[Index];
			if (FMath::Abs(R.AxisX.Z) <= 1.0e-3f) continue;
			++HipCaps;
			if (FMath::Abs(R.AxisZ.X) < 0.05f || FMath::Abs(R.AxisZ.Y) < 0.05f) ++FlatNormal;
		}
		TestTrue(FString::Printf(TEXT("hip with a longer ridge: hip caps straddle both faces (%d caps, %d not)"), HipCaps, FlatNormal),
			HipCaps >= 8 && FlatNormal == 0);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ④ 山墙进房体三角汤
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseGableWallSoupTest,
	"PCGPlugins.ComputeShaderGenerator.House.GableWallSoup",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseGableWallSoupTest::RunTest(const FString& Parameters)
{
	FCSHouseBodyDesc Desc;   // 600 × 400，墙厚 24，墙高 300
	FCSGpuMeshCPUData Plain;
	CSHouse_BuildBodySoup(Desc, Plain);

	// 两面山墙（东 1 / 西 3），上沿是到 140 cm 的等腰三角形。
	const double Rise = 140.0;
	for (const int32 Edge : { 1, 3 })
	{
		FCSHouseGableWall Gable;
		Gable.EdgeIndex = Edge;
		Gable.Profile = { FVector2D(0.0, 0.0), FVector2D(200.0, Rise), FVector2D(400.0, 0.0) };
		Desc.Gables.Add(Gable);
	}
	FCSGpuMeshCPUData Gabled;
	CSHouse_BuildBodySoup(Desc, Gabled);

	// 墙板一个三角形都不动（山墙是追加的）；每面山墙两段，每段 外皮 1 + 内皮 1 + 上沿 2 = 4 个三角形。
	const int32 Before = Plain.Indices.Num();
	TestEqual(TEXT("two gables add sixteen triangles"), Gabled.Indices.Num() - Before, 16 * 3);
	bool bPrefixSame = Gabled.Positions.Num() >= Plain.Positions.Num();
	for (int32 Index = 0; bPrefixSame && Index < Plain.Positions.Num(); ++Index) bPrefixSame &= Gabled.Positions[Index] == Plain.Positions[Index];
	TestTrue(TEXT("the wall panels below are untouched"), bPrefixSame);

	const float H = Desc.WallHeight, T = Desc.WallThickness;
	int32 OffWall = 0, AboveProfile = 0, WrongPart = 0, WithHole = 0;
	bool bApexEast = false, bApexWest = false;
	for (int32 Index = Plain.Positions.Num(); Index < Gabled.Positions.Num(); ++Index)
	{
		const FVector3f& P = Gabled.Positions[Index];
		// 都落在东 / 西墙的墙厚之内。
		const bool bEast = P.X >= 300.0f - T - 0.01f && P.X <= 300.01f;
		const bool bWest = P.X <= -300.0f + T + 0.01f && P.X >= -300.01f;
		if (!bEast && !bWest) ++OffWall;
		// 高度：墙顶之上、三角形上沿之下（上沿 = Rise·(1 − |y| / 200)）。
		const float Top = H + float(Rise) * (1.0f - FMath::Abs(P.Y) / 200.0f);
		if (P.Z < H - 0.01f || P.Z > Top + 0.01f) ++AboveProfile;
		if (FMath::IsNearlyEqual(P.Z, H + float(Rise), 0.01f) && FMath::IsNearlyZero(P.Y, 0.01f))
		{
			bApexEast |= bEast;
			bApexWest |= bWest;
		}
		// 通道字典：山墙是墙（R = 0）、没有洞（B = 255，UV1 = 哨兵）。
		const FVector4f& C = Gabled.Colors[Index];
		if (C.X != 0.0f) ++WrongPart;
		if (C.Z != 1.0f || Gabled.TexCoordChannels[1][Index].X < 4.0f) ++WithHole;
	}
	TestEqual(TEXT("every gable vertex sits inside the gable wall's thickness"), OffWall, 0);
	TestEqual(TEXT("every gable vertex sits between the wall top and the triangle"), AboveProfile, 0);
	TestTrue(TEXT("both gables reach the apex"), bApexEast && bApexWest);
	TestEqual(TEXT("the gable is wall, in the channel dictionary"), WrongPart, 0);
	TestEqual(TEXT("the gable carries no opening"), WithHole, 0);

	// 朝向：外皮朝房外、内皮朝房内、上沿朝上。法线是世界空间（World = Identity）。
	int32 Outer = 0, Inner = 0, Up = 0, Other = 0;
	for (int32 Index = Plain.Positions.Num(); Index < Gabled.Positions.Num(); Index += 3)
	{
		const FVector3f& N = Gabled.Normals[Index];
		const FVector3f& P = Gabled.Positions[Index];
		const float Side = P.X > 0.0f ? 1.0f : -1.0f;
		if (N.X * Side > 0.99f) ++Outer;
		else if (N.X * Side < -0.99f) ++Inner;
		else if (N.Z > 0.5f && FMath::IsNearlyZero(N.X, 1.0e-4f)) ++Up;
		else ++Other;
	}
	TestTrue(FString::Printf(TEXT("faces: 4 outer, 4 inner, 8 top (got %d / %d / %d, %d other)"), Outer, Inner, Up, Other),
		Outer == 4 && Inner == 4 && Up == 8 && Other == 0);

	// 贴图坐标接着下面的墙板往上铺：外皮上 UV0 = (沿边弧长, 高度) / 平铺周期。
	int32 BadUV = 0;
	for (int32 Index = Plain.Positions.Num(); Index < Gabled.Positions.Num(); ++Index)
	{
		const FVector3f& N = Gabled.Normals[Index];
		const FVector3f& P = Gabled.Positions[Index];
		if (N.X < 0.99f || P.X < 299.9f) continue;   // 只看东墙外皮：1 号边从 (300, −200) 走到 (300, 200)
		const FVector2f Want((P.Y + 200.0f) / 200.0f, P.Z / 200.0f);
		if (!Gabled.TexCoords()[Index].Equals(Want, 1.0e-4f)) ++BadUV;
	}
	TestEqual(TEXT("the gable's outer face continues the wall's (S, Z) texture coordinates"), BadUV, 0);

	// ---- 阶梯山墙：上沿整体抬高，两头不落到 0 ⇒ 每段外皮 / 内皮各两个三角形，两头各多一个端面 ----
	{
		FCSHouseBodyDesc SteppedDesc;
		FCSHouseGableWall Gable;
		Gable.EdgeIndex = 1;
		Gable.Profile = { FVector2D(0.0, 58.0), FVector2D(200.0, 198.0), FVector2D(400.0, 58.0) };
		SteppedDesc.Gables.Add(Gable);
		FCSGpuMeshCPUData Stepped;
		CSHouse_BuildBodySoup(SteppedDesc, Stepped);
		// 两段 ×（外皮 2 + 内皮 2 + 上沿 2）+ 两个端面 × 2 = 16 个三角形。
		TestEqual(TEXT("stepped: a raised gable adds sixteen triangles"), Stepped.Indices.Num() - Before, 16 * 3);

		int32 EndSouth = 0, EndNorth = 0, Wrong = 0;
		float Lowest = TNumericLimits<float>::Max(), Highest = -TNumericLimits<float>::Max();
		for (int32 Index = Plain.Positions.Num(); Index < Stepped.Positions.Num(); Index += 3)
		{
			const FVector3f& N = Stepped.Normals[Index];
			// 1 号边沿 +Y 走：起点端面朝 −Y，终点端面朝 +Y。
			if (N.Y < -0.99f) ++EndSouth;
			else if (N.Y > 0.99f) ++EndNorth;
			else if (FMath::Abs(N.X) < 0.99f && N.Z < 0.5f) ++Wrong;
		}
		for (int32 Index = Plain.Positions.Num(); Index < Stepped.Positions.Num(); ++Index)
		{
			Lowest = FMath::Min(Lowest, Stepped.Positions[Index].Z);
			Highest = FMath::Max(Highest, Stepped.Positions[Index].Z);
		}
		TestTrue(FString::Printf(TEXT("stepped: both raised ends are closed, facing out along the wall (%d / %d, %d stray)"), EndSouth, EndNorth, Wrong),
			EndSouth == 2 && EndNorth == 2 && Wrong == 0);
		TestTrue(TEXT("stepped: the gable spans from the wall top to the raised apex"),
			FMath::IsNearlyEqual(Lowest, H, 0.01f) && FMath::IsNearlyEqual(Highest, H + 198.0f, 0.01f));
	}

	// ---- 砖层在山墙上的那几层：只铺在墙顶线不低于行中线的那一段（TG `trim_rows`） ----
	{
		const TArray<FVector2D> Triangle = { FVector2D(0.0, 0.0), FVector2D(200.0, 140.0), FVector2D(400.0, 0.0) };
		float S0 = 0.0f, S1 = 0.0f;
		// 行中线高 35：三角形在 S = 50 与 350 处穿过它；吸附到 0.2 块砖（26 cm 砖 ⇒ 5.2 cm）。
		TestTrue(TEXT("bricks: a low course spans most of the gable"), CSHouseBrickWall::GableCourseSpan(Triangle, 400.0f, 35.0f, 26.0f, S0, S1));
		TestTrue(FString::Printf(TEXT("bricks: the course ends where the slope crosses its mid line, snapped (%.2f .. %.2f)"), S0, S1),
			FMath::IsNearlyEqual(S0, 52.0f, 0.01f) && FMath::IsNearlyEqual(S1, 348.4f, 0.01f));
		// 越往上越短：台阶。
		float T0 = 0.0f, T1 = 0.0f;
		TestTrue(TEXT("bricks: a higher course still exists"), CSHouseBrickWall::GableCourseSpan(Triangle, 400.0f, 105.0f, 26.0f, T0, T1));
		TestTrue(TEXT("bricks: higher courses are shorter and nest inside the lower ones"), T0 > S0 && T1 < S1 && T1 - T0 < S1 - S0);
		// 尖上不足一块砖的那一段加宽成一块砖，不丢。
		float P0 = 0.0f, P1 = 0.0f;
		TestTrue(TEXT("bricks: the tip course is kept"), CSHouseBrickWall::GableCourseSpan(Triangle, 400.0f, 136.0f, 26.0f, P0, P1));
		TestTrue(FString::Printf(TEXT("bricks: ...widened to one whole brick around the apex (%.2f .. %.2f)"), P0, P1),
			FMath::IsNearlyEqual(P1 - P0, 26.0f, 0.01f) && FMath::IsNearlyEqual((P0 + P1) * 0.5f, 200.0f, 2.7f));
		TestFalse(TEXT("bricks: no course above the gable"), CSHouseBrickWall::GableCourseSpan(Triangle, 400.0f, 141.0f, 26.0f, P0, P1));
		// 阶梯山墙两头是抬着的：低于两头高度的行铺满整条边，行端顶在边两头、不吸附。
		const TArray<FVector2D> Raised = { FVector2D(0.0, 58.0), FVector2D(200.0, 198.0), FVector2D(400.0, 58.0) };
		TestTrue(TEXT("bricks: below the raised ends a course runs the whole edge"),
			CSHouseBrickWall::GableCourseSpan(Raised, 400.0f, 30.0f, 26.0f, S0, S1) && S0 == 0.0f && S1 == 400.0f);

		// 整摞：接着墙体的层号往上，逐层变短；总数不超过预算。
		const CSHouseBrickWall::FCourses Courses = CSHouseBrickWall::PlanCourses(300.0f, 20.0f);
		const CSHouseBrickWall::FGable Gables[] = { { 1, Triangle }, { 3, Triangle } };
		CSHouseFrame::FBrickParams BrickParams;
		BrickParams.Length = 26.0f;
		BrickParams.MaxBricks = 4096;
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		TArray<float> Widths;
		int32 FirstCourse = INDEX_NONE, LastCourse = INDEX_NONE, RunsPerCourseWrong = 0;
		const int32 Bricks = CSHouseBrickWall::BuildGableCourses(FTransform::Identity, Desc.Footprint, Desc.WallThickness, Courses,
			Gables, 1234u, BrickParams, Runs, Elements,
			[&](int32 CourseIndex, const CSHouseTrim::FBand& Band, int32, const TArray<CSHouseTrim::FRun>& CourseRuns)
			{
				if (FirstCourse == INDEX_NONE) FirstCourse = CourseIndex;
				LastCourse = CourseIndex;
				if (CourseRuns.Num() != 2 || CourseRuns[0].EdgeIndex != 1 || CourseRuns[1].EdgeIndex != 3
					|| CourseRuns[0].Span() != CourseRuns[1].Span() || Band.CenterZ < 300.0f) ++RunsPerCourseWrong;
				else Widths.Add(CourseRuns[0].Span());
			});
		TestEqual(TEXT("bricks: gable courses continue the wall's course numbering"), FirstCourse, Courses.Count);
		TestEqual(TEXT("bricks: seven 20 cm courses fit under a 140 cm apex"), LastCourse - FirstCourse + 1, 7);
		TestEqual(TEXT("bricks: every gable course carries one run per gable, mirrored"), RunsPerCourseWrong, 0);
		bool bShrinks = Widths.Num() == 7;
		for (int32 Index = 1; Index < Widths.Num(); ++Index) bShrinks &= Widths[Index] < Widths[Index - 1];
		TestTrue(TEXT("bricks: each course is shorter than the one below (a staircase)"), bShrinks);
		const int32 Budget = CSHouseBrickWall::EstimateGableBricks(Desc.Footprint, Desc.WallThickness, Gables, Courses, 26.0f);
		TestTrue(FString::Printf(TEXT("bricks: the gable bricks fit their budget (%d <= %d)"), Bricks, Budget), Bricks > 20 && Bricks <= Budget);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 脊长箭头：拖 δ 脊端走 δ；拖到头是山墙；往回拖又是四坡
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseRidgeHandleTest,
	"PCGPlugins.ComputeShaderGenerator.House.RidgeHandle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseRidgeHandleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	constexpr float Yaw = 30.0f;
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector(700.0, -400.0, 0.0), FRotator(0.0f, Yaw, 0.0f));
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->ReevaluateSite();

	// 旧存档 / 新放的房子：自动 = 等坡度四坡，脊长是长边 − 短边。
	TestTrue(TEXT("a fresh house has an automatic ridge"), House->RoofRidgeLength01 < 0.0f);
	TestTrue(TEXT("...of the equal-pitch length"), FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 200.0f, 1.0e-3f));
	TestFalse(TEXT("...and is not a gable"), House->IsGableRoof());
	TestTrue(TEXT("...with no multipliers on its roof desc"), House->GetRoofDesc().EdgeSteep.IsEmpty());

	const float WallHeight = House->WallHeight, Pitch = House->RoofPitch, Rise = House->GetRoofRise();
	const FVector2D Size = House->FootprintSize;

	House->EnterResizeMode();
	ACSHouseRidgeHandleActor* End0 = House->GetRidgeHandle(0);
	ACSHouseRidgeHandleActor* End1 = House->GetRidgeHandle(1);
	if (!TestNotNull(TEXT("Resize mode creates the ridge handle at end 0"), End0)) return false;
	if (!TestNotNull(TEXT("Resize mode creates the ridge handle at end 1"), End1)) return false;
	TestTrue(TEXT("the two ridge handles are different actors"), End0 != End1);

	// 规范位置：脊端点沿脊向再往外 HandleOffset，屋脊之上 HandleLift。房子转过 30°，脊向跟着转。
	const FVector RidgeDir = FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector::ForwardVector);
	auto WantLocation = [&](const ACSHouseRidgeHandleActor* Handle, double RidgeLength)
	{
		const double Along = (RidgeLength * 0.5 + double(Handle->HandleOffset)) * (Handle->GetEnd() == 0 ? -1.0 : 1.0);
		const double Z = double(House->WallHeight + House->RoofHeightOffset + House->GetRoofRise() + Handle->HandleLift);
		return House->GetActorTransform().TransformPosition(FVector(Along, 0.0, Z));
	};
	TestTrue(TEXT("end 1 sits past the +ridge end"), End1->GetActorLocation().Equals(WantLocation(End1, 200.0), 0.01));
	TestTrue(TEXT("end 0 sits past the -ridge end"), End0->GetActorLocation().Equals(WantLocation(End0, 200.0), 0.01));
	TestTrue(TEXT("end 1 points along the ridge"), End1->GetOutwardWorld().Equals(RidgeDir, 1.0e-4));
	TestTrue(TEXT("end 0 points the other way"), End0->GetOutwardWorld().Equals(-RidgeDir, 1.0e-4));

	// ---- 拖 50 cm：本端走 50，脊居中 ⇒ 脊长 + 100；拖歪的分量不算 ----
	const FVector Before1 = End1->GetActorLocation();
	const FVector Before0 = End0->GetActorLocation();
	End1->AddActorWorldOffset(RidgeDir * 50.0 + FVector(0.0, 0.0, 37.0) + FVector::CrossProduct(RidgeDir, FVector::UpVector) * 21.0);
	const float Applied = End1->ConsumeDragToHost(false);
	TestTrue(FString::Printf(TEXT("dragging one end 50 cm lengthens the ridge by 100 (%.3f)"), Applied), FMath::IsNearlyEqual(Applied, 100.0f, 0.05f));
	TestTrue(TEXT("the ridge is now 300 long"), FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 300.0f, 0.05f));
	TestTrue(TEXT("the first real drag pins the ridge as an explicit parameter"), House->RoofRidgeLength01 >= 0.0f);
	TestEqual(TEXT("...along the long axis it already had"), House->RoofRidgeAxis, ECSRoofRidgeAxis::X);
	TestTrue(TEXT("the dragged handle ends up exactly 50 cm further along the ridge"), End1->GetActorLocation().Equals(Before1 + RidgeDir * 50.0, 0.05));
	TestTrue(TEXT("the other handle moves 50 cm the other way"), End0->GetActorLocation().Equals(Before0 - RidgeDir * 50.0, 0.05));
	TestFalse(TEXT("still a hip roof"), House->IsGableRoof());
	TestTrue(TEXT("the end faces are now steeper than the sides"),
		House->GetRoofDesc().EdgeSteep.Num() == 4 && House->GetRoofDesc().EdgeSteep[1] > 1.0f);

	// ---- 拖过房子的头：钳在恰好的 1 ⇒ 双坡 + 山墙 ----
	End0->AddActorWorldOffset(-RidgeDir * 1000.0);
	const float ToGable = End0->ConsumeDragToHost(false);
	TestTrue(FString::Printf(TEXT("dragging past the house end only lengthens the ridge to the house length (%.3f)"), ToGable),
		FMath::IsNearlyEqual(ToGable, 300.0f, 0.05f));
	TestTrue(TEXT("the parameter clamps to exactly 1"), House->RoofRidgeLength01 == 1.0f);
	TestTrue(TEXT("the roof is now a gable roof"), House->IsGableRoof());
	TestTrue(TEXT("the ridge is as long as the house"), FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 600.0f, 1.0e-3f));
	const FCSRoofDesc GableRoof = House->GetRoofDesc();
	TestTrue(TEXT("the two end edges are gables"), GableRoof.IsGable(1) && GableRoof.IsGable(3) && !GableRoof.IsGable(0) && !GableRoof.IsGable(2));
	// 默认是 TG 给砖石墙的阶梯山墙：屋面每头缩进山墙 26.5 cm。
	TestEqual(TEXT("a masonry house gets TG's stepped gable by default"), House->GableRoofStyle, ECSGableRoofStyle::Stepped);
	TestEqual(TEXT("the roof stops inside the gable walls by the TG amount"), GableRoof.GableOverhang, -26.5f);
	TestTrue(TEXT("the handles sit past the gable ends"), End0->GetActorLocation().Equals(WantLocation(End0, 600.0), 0.05)
		&& End1->GetActorLocation().Equals(WantLocation(End1, 600.0), 0.05));
	// 悬挑式是另一种：屋面每头伸出山墙 26.5 cm + 檐口出挑（外挑 25 ⇒ 48.5）。
	House->GableRoofStyle = ECSGableRoofStyle::Overhang;
	House->ReevaluateSite();
	TestEqual(TEXT("the overhanging style sails past the gables instead"), House->GetRoofDesc().GableOverhang, 48.5f);
	House->GableRoofStyle = ECSGableRoofStyle::Stepped;
	House->ReevaluateSite();

	// 顶在 1 上再往外拖：一步都不走，也不攒残差。
	End0->AddActorWorldOffset(-RidgeDir * 80.0);
	TestTrue(TEXT("pushing further out applies nothing"), FMath::IsNearlyZero(End0->ConsumeDragToHost(false)));
	TestTrue(TEXT("...and the handle snaps back"), End0->GetActorLocation().Equals(WantLocation(End0, 600.0), 0.05));

	// 别的量都不动：墙高、坡度、起伏、尺寸。
	TestEqual(TEXT("the wall height is untouched"), House->WallHeight, WallHeight);
	TestEqual(TEXT("the pitch is untouched"), House->RoofPitch, Pitch);
	TestTrue(TEXT("the roof rise is untouched"), FMath::IsNearlyEqual(House->GetRoofRise(), Rise, 1.0e-3f));
	TestEqual(TEXT("the footprint is untouched"), House->FootprintSize, Size);

	// ---- 往回拖 60 cm：脊长 − 120，又是四坡 ----
	End1->AddActorWorldOffset(-RidgeDir * 60.0);
	const float Back = End1->ConsumeDragToHost(true);
	TestTrue(FString::Printf(TEXT("dragging back in shortens the ridge (%.3f)"), Back), FMath::IsNearlyEqual(Back, -120.0f, 0.05f));
	TestFalse(TEXT("no longer a gable"), House->IsGableRoof());
	TestTrue(TEXT("the ridge is 480 long"), FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 480.0f, 0.05f));

	// ---- 存的是比例：拉房子尺寸，山墙还是山墙 ----
	House->RoofRidgeLength01 = 1.0f;
	House->PushEdge(1, 150.0f, true);
	TestTrue(TEXT("a gable stays a gable when the house is stretched"), House->IsGableRoof()
		&& FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 750.0f, 0.05f));
	House->PushEdge(1, -150.0f, true);

	// ---- 换脊向：脊转 90°，脊高改由长边的一半定 ----
	House->SwitchRoofRidgeDirection();
	TestEqual(TEXT("the ridge now runs along Y"), House->RoofRidgeAxis, ECSRoofRidgeAxis::Y);
	TestTrue(TEXT("still a gable — the parameter is a ratio"), House->IsGableRoof() && FMath::IsNearlyEqual(House->GetRoofRidgeLength(), 400.0f, 1.0e-3f));
	TestTrue(TEXT("the rise now comes from half the long side"), FMath::IsNearlyEqual(House->GetRoofRise(), Rise * 1.5f, 0.01f));
	const FVector AcrossDir = FRotator(0.0f, Yaw, 0.0f).RotateVector(FVector::RightVector);
	TestTrue(TEXT("the handles follow the ridge to its new direction"), End1->GetOutwardWorld().Equals(AcrossDir, 1.0e-4));
	House->SwitchRoofRidgeDirection();
	TestEqual(TEXT("switching twice comes back"), House->RoofRidgeAxis, ECSRoofRidgeAxis::X);

	// ---- TG 的默认值：6 × 4 m 是 0.8977（差一点到山墙），沿长边 ----
	House->ResetRoofRidgeToTinyGladeDefault();
	TestTrue(TEXT("TG default ridge parameter for a 6 x 4 m house"), FMath::IsNearlyEqual(House->RoofRidgeLength01, 0.897674f, 1.0e-4f));
	TestEqual(TEXT("TG default ridge runs along the long side"), House->RoofRidgeAxis, ECSRoofRidgeAxis::X);

	// ---- 露台没有脊：拖箭头什么都不发生 ----
	House->RoofPitch = 0.0f;
	House->ReevaluateSite();
	TestTrue(TEXT("a zero-pitch roof is a terrace"), House->IsFlatRoof());
	const float Kept = House->RoofRidgeLength01;
	TestTrue(TEXT("pushing the ridge on a terrace applies nothing"), FMath::IsNearlyZero(House->PushRidgeLength(100.0f, true)));
	TestEqual(TEXT("...and leaves the parameter alone"), House->RoofRidgeLength01, Kept);
	TestFalse(TEXT("a terrace is never a gable"), House->IsGableRoof());
	TestTrue(TEXT("the terrace desc carries no multipliers"), House->GetRoofDesc().EdgeSteep.IsEmpty());

	House->ExitResizeMode();
	House->Destroy();
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 阶梯山墙的露砖带：灰泥墙板只做到屋面线，上面 46 cm 由砖逐层收成台阶
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseGableCopingTest,
	"PCGPlugins.ComputeShaderGenerator.House.GableCoping",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseGableCopingTest::RunTest(const FString& Parameters)
{
	// ---- 纯函数：阶梯山墙的墙顶线 = 屋面线 + 46（两头 46、尖 186），坡度 tan = 0.7 ----
	const FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
	const float Thickness = 24.0f, Band = CSHouseRoof_SteppedGableRise;
	const TArray<FVector2D> TopLine = { FVector2D(0.0, 46.0), FVector2D(200.0, 186.0), FVector2D(400.0, 46.0) };
	auto TopAt = [](double S) { return 46.0 + 0.7 * (200.0 - FMath::Abs(S - 200.0)); };
	const CSHouseBrickWall::FCourses Courses = CSHouseBrickWall::PlanCourses(300.0f, 20.0f);
	const CSHouseBrickWall::FGable Gables[] = { { 1, TopLine }, { 3, TopLine } };
	CSHouseFrame::FBrickParams BrickParams;
	BrickParams.Length = 26.0f;
	BrickParams.MaxBricks = 4096;

	struct FCourse { int32 Index; float MidRise; float HalfHeight; TArray<CSHouseTrim::FRun> Runs; int32 Bricks; };
	auto Collect = [&](float ExposedBand, uint32 Salt, TArray<FCourse>& Out, TArray<CSHouseFrame::FElement>& Elements)
	{
		TArray<CSHouseTrim::FRun> Runs;
		return CSHouseBrickWall::BuildGableCourses(FTransform::Identity, Footprint, Thickness, Courses, Gables, 1234u, BrickParams,
			Runs, Elements,
			[&Out](int32 CourseIndex, const CSHouseTrim::FBand& B, int32 Added, const TArray<CSHouseTrim::FRun>& CourseRuns)
			{
				Out.Add({ CourseIndex, B.CenterZ - 300.0f, B.HalfHeight, CourseRuns, Added });
			}, ExposedBand, Salt);
	};
	TArray<FCourse> Full, Coping;
	TArray<CSHouseFrame::FElement> FullElements, CopingElements;
	const int32 FullBricks = Collect(0.0f, CSHouseFrame::EPathFamily::BrickWall, Full, FullElements);
	const int32 CopingBricks = Collect(Band, CSHouseFrame::EPathFamily::GableCoping, Coping, CopingElements);

	// 186 cm 高的墙顶线、20 cm 一层 ⇒ 中线在 10..170 的九层；两种口径层数相同（露砖带只是每层里挖掉中间）。
	TestEqual(TEXT("nine courses fit under the stepped top line"), Full.Num(), 9);
	TestEqual(TEXT("the coping walks the same courses"), Coping.Num(), Full.Num());
	TestTrue(FString::Printf(TEXT("the coping is a strict subset of the full gable (%d < %d bricks)"), CopingBricks, FullBricks),
		CopingBricks > 20 && CopingBricks < FullBricks);

	int32 WrongRuns = 0, Buried = 0, MissingEdge = 0, OutOfSpan = 0, SplitCourses = 0, WholeCourses = 0;
	float PreviousStart = -1.0f;
	bool bStairs = true;
	for (int32 Index = 0; Index < FMath::Min(Full.Num(), Coping.Num()); ++Index)
	{
		const FCourse& C = Coping[Index];
		const double Top = double(C.MidRise) + double(C.HalfHeight);
		for (const int32 Edge : { 1, 3 })
		{
			TArray<CSHouseTrim::FRun> Mine, Whole;
			for (const CSHouseTrim::FRun& R : C.Runs) if (R.EdgeIndex == Edge) Mine.Add(R);
			for (const CSHouseTrim::FRun& R : Full[Index].Runs) if (R.EdgeIndex == Edge) Whole.Add(R);
			if (Whole.Num() != 1 || Mine.Num() < 1 || Mine.Num() > 2) { ++WrongRuns; continue; }
			// 两头不动：露砖带的外端就是整行的两端（台阶的形状不许变）。
			if (Mine[0].S0 != Whole[0].S0 || Mine.Last().S1 != Whole[0].S1) ++MissingEdge;
			for (const CSHouseTrim::FRun& R : Mine) if (R.S0 < Whole[0].S0 || R.S1 > Whole[0].S1 || R.Span() < 13.0f) ++OutOfSpan;
			if (Mine.Num() == 2)
			{
				// 中间挖掉的那一段**整块埋在灰泥下**：灰泥线（墙顶线 − 46）在那里不低于这一层的顶。
				const double Gap0 = Mine[0].S1, Gap1 = Mine[1].S0;
				for (const double S : { Gap0 + 0.01, (Gap0 + Gap1) * 0.5, Gap1 - 0.01 })
				{
					if (TopAt(S) - double(Band) < Top - 0.02) ++Buried;
				}
				// ……而且没有多挖：再往外一点灰泥线就低于层顶了（除非那一截是被补到一块砖的）。
				if (Mine[0].Span() > 26.01f && TopAt(Gap0 - 0.5) - double(Band) >= Top) ++Buried;
				if (Mine[1].Span() > 26.01f && TopAt(Gap1 + 0.5) - double(Band) >= Top) ++Buried;
			}
			if (Edge == 1)
			{
				if (Mine.Num() == 2) ++SplitCourses;
				else ++WholeCourses;
				bStairs &= Mine[0].S0 >= PreviousStart;
				PreviousStart = Mine[0].S0;
			}
		}
	}
	TestEqual(TEXT("every course carries one or two runs per gable"), WrongRuns, 0);
	TestEqual(TEXT("the coping keeps the outer ends of every course"), MissingEdge, 0);
	TestEqual(TEXT("coping runs stay inside the course and can hold a brick"), OutOfSpan, 0);
	TestEqual(TEXT("only bricks wholly under the plaster are dropped"), Buried, 0);
	TestTrue(TEXT("courses step inward as they climb"), bStairs);
	// 中线 10..110 的六层两头各一截；130 / 150 / 170 三层灰泥线够不着层顶，整行都露着（山墙尖）。
	TestTrue(FString::Printf(TEXT("six split courses under three whole ones at the apex (%d / %d)"), SplitCourses, WholeCourses),
		SplitCourses == 6 && WholeCourses == 3);

	// 家族盐不同：同一层、同一段，露砖带与砖层的逐实例随机数不许撞。
	bool bSaltDiffers = FullElements.Num() > 0 && CopingElements.Num() > 0;
	for (const CSHouseFrame::FElement& A : CopingElements)
	{
		for (const CSHouseFrame::FElement& B : FullElements) bSaltDiffers &= A.RandomBase != B.RandomBase;
	}
	TestTrue(TEXT("coping bricks draw their random numbers from their own family"), bSaltDiffers);

	// 陡屋面：每层露出来的那一截不足一块砖 ⇒ 补到一块，不留空层。
	{
		const TArray<FVector2D> Steep = { FVector2D(0.0, 46.0), FVector2D(200.0, 646.0), FVector2D(400.0, 46.0) };   // tan = 3
		const CSHouseBrickWall::FGable SteepGables[] = { { 1, Steep } };
		TArray<CSHouseTrim::FRun> Runs;
		TArray<CSHouseFrame::FElement> Elements;
		int32 Thin = 0, Seen = 0;
		CSHouseBrickWall::BuildGableCourses(FTransform::Identity, Footprint, Thickness, Courses, SteepGables, 1234u, BrickParams,
			Runs, Elements,
			[&](int32, const CSHouseTrim::FBand&, int32 Added, const TArray<CSHouseTrim::FRun>& CourseRuns)
			{
				++Seen;
				if (Added <= 0) ++Thin;
				for (const CSHouseTrim::FRun& R : CourseRuns) if (R.Span() < 25.99f) ++Thin;
			}, Band, CSHouseFrame::EPathFamily::GableCoping);
		TestTrue(FString::Printf(TEXT("steep gables still get at least a brick per run (%d courses, %d thin)"), Seen, Thin), Seen > 20 && Thin == 0);
	}

	// ---- 房体三角汤：墙板只做到「墙顶线 − PlasterDrop」 ----
	{
		FCSHouseBodyDesc Desc;   // 600 × 400，墙高 300
		FCSGpuMeshCPUData Plain;
		CSHouse_BuildBodySoup(Desc, Plain);
		FCSHouseGableWall Gable;
		Gable.EdgeIndex = 1;
		Gable.Profile = TopLine;
		Gable.PlasterDrop = Band;
		Desc.Gables.Add(Gable);
		FCSGpuMeshCPUData Dropped;
		CSHouse_BuildBodySoup(Desc, Dropped);
		float Highest = -TNumericLimits<float>::Max();
		int32 AboveRoofLine = 0;
		for (int32 Index = Plain.Positions.Num(); Index < Dropped.Positions.Num(); ++Index)
		{
			const FVector3f& P = Dropped.Positions[Index];
			Highest = FMath::Max(Highest, P.Z);
			if (P.Z > 300.0f + float(TopAt(double(P.Y) + 200.0)) - Band + 0.01f) ++AboveRoofLine;
		}
		// 两头落回墙顶（46 − 46 = 0）⇒ 没有端面：两段 ×（外皮 1 + 内皮 1 + 上沿 2）= 8 个三角形。
		TestEqual(TEXT("plaster: the slab stops at the roof line, with no raised ends"), Dropped.Indices.Num() - Plain.Indices.Num(), 8 * 3);
		TestTrue(TEXT("plaster: its apex is 46 cm under the wall top line"), FMath::IsNearlyEqual(Highest, 300.0f + 186.0f - Band, 0.01f));
		TestEqual(TEXT("plaster: nothing pokes above the roof line"), AboveRoofLine, 0);
	}

	// ---- 整栋房：有砖可出时才把墙板降下来；四种配置各归各 ----
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	House->Windows.Reset();
	House->FootprintSize = FVector2D(600.0, 400.0);
	House->RoofRidgeLength01 = 1.0f;
	House->RoofRidgeAxis = ECSRoofRidgeAxis::X;
	House->ReevaluateSite();
	// 没配砖网格：没有砖可出 ⇒ 墙板自己升到墙顶线。
	TestTrue(TEXT("a gable roof"), House->IsGableRoof());
	TestFalse(TEXT("no brick mesh: the plaster slab keeps the whole stepped rise"), House->HasSteppedGableBricks());
	TestEqual(TEXT("no brick mesh: no coping bricks"), House->GetTrimGableBrickCount(), 0);

	House->bFrameEnabled = true;
	House->FrameBrickMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	House->ReevaluateSite();
	const int32 CopingCount = House->GetTrimGableBrickCount();
	TestTrue(TEXT("with bricks the top band is laid in brick"), House->HasSteppedGableBricks());
	TestTrue(FString::Printf(TEXT("two stepped gables get coping bricks (%d)"), CopingCount), CopingCount > 40);
	TestTrue(TEXT("coping bricks are part of the trim count"), House->GetTrimBrickCount() >= CopingCount);
	TestTrue(TEXT("...and of the shared brick component"), House->GetFrameBrickCount() >= House->GetTrimBrickCount());

	// 悬挑山墙：墙顶藏在瓦底下，没有露砖。
	House->GableRoofStyle = ECSGableRoofStyle::Overhang;
	House->ReevaluateSite();
	TestEqual(TEXT("an overhanging gable has no coping"), House->GetTrimGableBrickCount(), 0);
	TestFalse(TEXT("...and keeps its slab at the roof line by itself"), House->HasSteppedGableBricks());
	House->GableRoofStyle = ECSGableRoofStyle::Stepped;

	// 关掉开关：回到直斜边的墙板。
	House->bTrimGable = false;
	House->ReevaluateSite();
	TestEqual(TEXT("switched off: no coping"), House->GetTrimGableBrickCount(), 0);
	TestFalse(TEXT("switched off: the slab rises to the top line again"), House->HasSteppedGableBricks());
	House->bTrimGable = true;

	// 四坡没有山墙。
	House->RoofRidgeLength01 = 0.6f;
	House->ReevaluateSite();
	TestEqual(TEXT("a hip roof has no coping"), House->GetTrimGableBrickCount(), 0);
	House->RoofRidgeLength01 = 1.0f;

	// 开了砖层：整面山墙由砖层砌，露砖带不重复出；墙板照样只做到屋面线。
	House->bBrickWallEnabled = true;
	House->ReevaluateSite();
	TestEqual(TEXT("with the brick layer on the coping stands down"), House->GetTrimGableBrickCount(), 0);
	TestTrue(TEXT("...the brick layer lays the gable instead"), House->GetBrickWallBrickCount() > CopingCount);
	TestTrue(TEXT("...and the slab still stops at the roof line"), House->HasSteppedGableBricks());

	House->Destroy();
	return true;
}

#endif

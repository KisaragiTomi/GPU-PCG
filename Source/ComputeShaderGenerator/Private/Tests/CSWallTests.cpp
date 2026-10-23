#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Components/SplineComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSGroundShaperActor.h"
#include "CSHouseProfile.h"
#include "CSWall.h"
#include "CSWallActor.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 样条墙（「房子变成墙」第 1 步，计划 `Docs/TinyGlade/Plan_Wall.md`）
//
// 纯函数那一层逐条钉口径：斜接皮线、墙体闭合且朝外、墙顶砖的奇偶与断点、角石落位、藤的两面墙；
// actor 那一层走真路径：地面 + 土包 + 默认 L 形样条，断言贴地、GPU 回读的砖数 / 叶数、幂等、改样条重建。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSWallTest_ 前缀。

/** L 形：(0,0) → (600,0) → (600,450)，平地 0、墙顶 150，三个点都是控制点（与 actor 的默认样条同形）。 */
FCSWallPath CSWallTest_LPath(double Top = 150.0)
{
	FCSWallPath Path;
	Path.Points = { FVector2D(0.0, 0.0), FVector2D(600.0, 0.0), FVector2D(600.0, 450.0) };
	Path.BaseZ = { 0.0, 0.0, 0.0 };
	Path.TopZ = { Top, Top, Top };
	Path.Kinks = { 1, 1, 1 };
	return Path;
}

/** 逆时针正方形，边长 400，四角都是控制点。 */
FCSWallPath CSWallTest_Square()
{
	FCSWallPath Path;
	Path.Points = { FVector2D(0.0, 0.0), FVector2D(400.0, 0.0), FVector2D(400.0, 400.0), FVector2D(0.0, 400.0) };
	Path.BaseZ = { 0.0, 0.0, 0.0, 0.0 };
	Path.TopZ = { 150.0, 150.0, 150.0, 150.0 };
	Path.Kinks = { 1, 1, 1, 1 };
	Path.bClosed = true;
	return Path;
}

/** 四分之一圆弧（半径 500，20 段），只有两端是控制点 —— 一条"弯墙"，中间没有拐角。 */
FCSWallPath CSWallTest_Arc()
{
	FCSWallPath Path;
	const int32 Steps = 20;
	for (int32 K = 0; K <= Steps; ++K)
	{
		const double A = double(UE_HALF_PI) * K / Steps;
		Path.Points.Add(FVector2D(500.0 * FMath::Cos(A), 500.0 * FMath::Sin(A)));
		Path.BaseZ.Add(0.0);
		Path.TopZ.Add(150.0);
		Path.Kinks.Add(K == 0 || K == Steps ? 1 : 0);
	}
	return Path;
}

/**
 * 三角汤按**引擎绕序**读出来的外法线面积向量之和与体积（散度定理）。
 * 索引存的是 (A, C, B)：外法线 = −cross(P[i1]−P[i0], P[i2]−P[i0])。闭合且朝向一致 ⇒ 面积向量和 ≈ 0。
 */
void CSWallTest_SurfaceIntegrals(const FCSGpuMeshCPUData& S, FVector& OutAreaSum, double& OutVolume)
{
	OutAreaSum = FVector::ZeroVector;
	OutVolume = 0.0;
	for (int32 T = 0; T + 2 < S.Indices.Num(); T += 3)
	{
		const FVector A(S.Positions[S.Indices[T]]);
		const FVector B(S.Positions[S.Indices[T + 1]]);
		const FVector C(S.Positions[S.Indices[T + 2]]);
		const FVector AreaVector = -FVector::CrossProduct(B - A, C - A) * 0.5;
		OutAreaSum += AreaVector;
		OutVolume += FVector::DotProduct((A + B + C) / 3.0, AreaVector) / 3.0;
	}
}

/**
 * 点到墙体水平截面（两条皮线围成的带）的**有符号**距离：墙外为正、墙里为负。
 * 开口墙的带 = 右皮 + 倒序的左皮（首尾两条端面边自动闭合）；闭合墙是外皮环减内皮环。
 */
double CSWallTest_BandDistance(const FCSWallSkins& Skins, bool bClosed, const FVector2D& P)
{
	auto EdgeDistance = [](const TArray<FVector2D>& Ring, const FVector2D& Q)
	{
		double Best = TNumericLimits<double>::Max();
		for (int32 I = 0; I < Ring.Num(); ++I)
		{
			const FVector2D A = Ring[I];
			const FVector2D E = Ring[(I + 1) % Ring.Num()] - A;
			const double T = FMath::Clamp(FVector2D::DotProduct(Q - A, E) / FMath::Max(E.SizeSquared(), 1.0e-9), 0.0, 1.0);
			Best = FMath::Min(Best, FVector2D::Distance(Q, A + E * T));
		}
		return Best;
	};
	auto Inside = [](const TArray<FVector2D>& Ring, const FVector2D& Q)
	{
		bool bIn = false;
		for (int32 I = 0, J = Ring.Num() - 1; I < Ring.Num(); J = I++)
		{
			if (((Ring[I].Y > Q.Y) != (Ring[J].Y > Q.Y)) && (Q.X < (Ring[J].X - Ring[I].X) * (Q.Y - Ring[I].Y) / (Ring[J].Y - Ring[I].Y) + Ring[I].X)) bIn = !bIn;
		}
		return bIn;
	};
	if (bClosed)
	{
		const double D = FMath::Min(EdgeDistance(Skins.Right, P), EdgeDistance(Skins.Left, P));
		const bool bInBand = Inside(Skins.Right, P) != Inside(Skins.Left, P);
		return bInBand ? -D : D;
	}
	TArray<FVector2D> Ring = Skins.Right;
	for (int32 I = Skins.Left.Num() - 1; I >= 0; --I) Ring.Add(Skins.Left[I]);
	const double D = EdgeDistance(Ring, P);
	return Inside(Ring, P) ? -D : D;
}

bool CSWallTest_AllBricksAreSingle(const TArray<CSHouseFrame::FElement>& Elements)
{
	for (const CSHouseFrame::FElement& E : Elements) if (E.BrickCount != 1) return false;
	return true;
}

/** 独立墙规矩（样条墙）的角石：墙端 + 按夹角判的拐角、凸侧出。 */
int32 CSWallTest_Quoins(const FCSWallPath& Path, const FCSWallSkins& Skins, bool bEnds, bool bCorners, float CornerTurnDegrees,
	TArray<CSHouseQuoin::FQuoin>& Out)
{
	CSWall::FQuoinParams Params;
	Params.Kind = ECSWallKind::Freestanding;
	Params.bEnds = bEnds;
	Params.bCorners = bCorners;
	Params.CornerTurnDegrees = CornerTurnDegrees;
	return CSWall::BuildQuoins(Path, Skins, Params, Out);
}

/** 独立墙规矩（样条墙）的藤墙面：两面折线带，开口墙在墙端绕过端面。 */
int32 CSWallTest_VineStrips(const FCSWallPath& Path, const FCSWallSkins& Skins, float BaseLift, float VineHeight,
	float GroundSampleSpacing, float CornerTurnDegrees, CSWall::FGroundSampler Ground, TArray<CSHouseVine::FWallStrip>& Out)
{
	CSWall::FVineStripParams Params;
	Params.Kind = ECSWallKind::Freestanding;
	Params.BaseLift = BaseLift;
	Params.VineHeight = VineHeight;
	Params.GroundSampleSpacing = GroundSampleSpacing;
	Params.CornerTurnDegrees = CornerTurnDegrees;
	return CSWall::BuildVineStrips(Path, Skins, Params, Ground, Out);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallSkinsMitreTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.SkinsMitre",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallSkinsMitreTest::RunTest(const FString& Parameters)
{
	// ① 直墙：两条皮线各偏半个墙厚，右手是 −Y。
	{
		FCSWallPath Path;
		Path.Points = { FVector2D(0.0, 0.0), FVector2D(600.0, 0.0) };
		Path.BaseZ = { 0.0, 0.0 };
		Path.TopZ = { 150.0, 150.0 };
		FCSWallSkins Skins;
		if (!TestTrue(TEXT("直墙建得出皮线"), CSWall::BuildSkins(Path, 40.0f, Skins))) return false;
		TestTrue(TEXT("直墙右皮 = (x, −20)"), Skins.Right[0].Equals(FVector2D(0.0, -20.0), 1.0e-6) && Skins.Right[1].Equals(FVector2D(600.0, -20.0), 1.0e-6));
		TestTrue(TEXT("直墙左皮 = (x, +20)"), Skins.Left[0].Equals(FVector2D(0.0, 20.0), 1.0e-6) && Skins.Left[1].Equals(FVector2D(600.0, 20.0), 1.0e-6));
		TestEqual(TEXT("直墙全长"), Skins.Length, 600.0, 1.0e-6);
		TestEqual(TEXT("端点没有转角"), Skins.HalfTurnCos[0], 1.0, 1.0e-9);
	}

	// ② L 形左转 90°：外角（右皮）沿平分线伸出 20·√2，内角（左皮）缩进同样多 —— 两条皮线仍与中线平行。
	{
		FCSWallSkins Skins;
		if (!TestTrue(TEXT("L 形建得出皮线"), CSWall::BuildSkins(CSWallTest_LPath(), 40.0f, Skins))) return false;
		TestTrue(TEXT("外角 = (620, −20)"), Skins.Right[1].Equals(FVector2D(620.0, -20.0), 1.0e-6));
		TestTrue(TEXT("内角 = (580, 20)"), Skins.Left[1].Equals(FVector2D(580.0, 20.0), 1.0e-6));
		TestTrue(TEXT("左转的转角正弦为正"), Skins.TurnSin[1] > 0.99);
		TestEqual(TEXT("直角的 cos(转角/2) = 1/√2"), Skins.HalfTurnCos[1], double(UE_INV_SQRT_2), 1.0e-6);
		TestEqual(TEXT("中线全长 600 + 450"), Skins.Length, 1050.0, 1.0e-6);
	}

	// ③ 逆时针正方形：右皮在外（内部在左手），四个外角都伸出 20·√2。
	{
		FCSWallSkins Skins;
		if (!TestTrue(TEXT("闭合正方形建得出皮线"), CSWall::BuildSkins(CSWallTest_Square(), 40.0f, Skins))) return false;
		TestTrue(TEXT("(0,0) 的外角 = (−20, −20)"), Skins.Right[0].Equals(FVector2D(-20.0, -20.0), 1.0e-6));
		TestTrue(TEXT("(400,400) 的外角 = (420, 420)"), Skins.Right[2].Equals(FVector2D(420.0, 420.0), 1.0e-6));
		TestEqual(TEXT("闭合全长含回到首点那一段"), Skins.Length, 1600.0, 1.0e-6);
	}

	// ④ 近 180° 的折返：斜接封顶在 MaxMitre 倍半墙厚，尖刺不会捅出去几米。
	{
		FCSWallPath Path;
		Path.Points = { FVector2D(0.0, 0.0), FVector2D(300.0, 0.0), FVector2D(0.0, 10.0) };
		Path.BaseZ = { 0.0, 0.0, 0.0 };
		Path.TopZ = { 150.0, 150.0, 150.0 };
		FCSWallSkins Skins;
		if (!TestTrue(TEXT("折返路径建得出皮线"), CSWall::BuildSkins(Path, 40.0f, Skins))) return false;
		const double Reach = FVector2D::Distance(Skins.Right[1], Path.Points[1]);
		AddInfo(FString::Printf(TEXT("折返处皮线伸出 %.2f cm（上限 %.2f）"), Reach, 20.0 * CSWall::MaxMitre));
		TestTrue(TEXT("折返处斜接封顶"), Reach <= 20.0 * CSWall::MaxMitre + 1.0e-6);
	}

	// ⑤ 零长段 / 墙厚为零：拒收。
	{
		FCSWallPath Path = CSWallTest_LPath();
		Path.Points[1] = Path.Points[0];
		FCSWallSkins Skins;
		TestFalse(TEXT("零长段拒收"), CSWall::BuildSkins(Path, 40.0f, Skins));
		TestFalse(TEXT("墙厚为零拒收"), CSWall::BuildSkins(CSWallTest_LPath(), 0.0f, Skins));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallBodySoupTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.BodySoupClosedAndOutward",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallBodySoupTest::RunTest(const FString& Parameters)
{
	auto Check = [this](const TCHAR* Name, const FCSWallPath& Path, float Thickness, int32 ExpectedTris)
	{
		FCSWallSkins Skins;
		if (!TestTrue(FString::Printf(TEXT("%s：皮线"), Name), CSWall::BuildSkins(Path, Thickness, Skins))) return;
		FCSGpuMeshCPUData S;
		const int32 Tris = CSWall::BuildBody(Path, Skins, CSWall::FBodyParams(), S);
		TestEqual(FString::Printf(TEXT("%s：三角形数 = 段数×8 + 端面"), Name), Tris, ExpectedTris);
		TestTrue(FString::Printf(TEXT("%s：快照自洽"), Name), S.IsValid());
		TestEqual(FString::Printf(TEXT("%s：两组 UV（UV1 = 裁剪场）"), Name), S.NumTexCoordChannels, 2);

		// 闭合 + 朝向一致：外法线面积向量之和为零；体积 = 墙厚 × 墙高 × 中线全长（斜接带的面积恰是 T·L）。
		FVector AreaSum;
		double Volume = 0.0;
		CSWallTest_SurfaceIntegrals(S, AreaSum, Volume);
		const double Expected = double(Thickness) * 150.0 * Skins.Length;
		AddInfo(FString::Printf(TEXT("%s：面积向量和 %s，体积 %.0f（期望 %.0f）"), Name, *AreaSum.ToString(), Volume, Expected));
		TestTrue(FString::Printf(TEXT("%s：表面闭合（面积向量和≈0）"), Name), AreaSum.Size() < 1.0);
		TestTrue(FString::Printf(TEXT("%s：体积为正且等于 T·H·L（朝外）"), Name), FMath::Abs(Volume - Expected) < Expected * 1.0e-3);

		// 通道：UV1 恒为哨兵、顶点色 B = 255（这块没有洞）—— 墙材质的 OpacityMask 据此整块保留。
		const FVector2f Sentinel = FCSOpeningClipField().Eval(0.0f, 0.0f);
		bool bSentinel = true, bNoHole = true;
		for (int32 V = 0; V < S.Positions.Num(); ++V)
		{
			bSentinel &= S.TexCoordChannels[1][V].Equals(Sentinel);
			bNoHole &= FMath::IsNearlyEqual(S.Colors[V].Z, 1.0f);
		}
		TestTrue(FString::Printf(TEXT("%s：UV1 全是哨兵"), Name), bSentinel);
		TestTrue(FString::Printf(TEXT("%s：顶点色 B 全是 255"), Name), bNoHole);
	};

	Check(TEXT("L 形"), CSWallTest_LPath(), 40.0f, 2 * 8 + 4);
	Check(TEXT("闭合正方形"), CSWallTest_Square(), 40.0f, 4 * 8);
	Check(TEXT("弯墙"), CSWallTest_Arc(), 34.0f, 20 * 8 + 4);

	// 弯墙的侧面法线是平滑的：同一个接点上两段墙面给出同一根法线（没有棱）。
	{
		FCSWallPath Path = CSWallTest_Arc();
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		FCSGpuMeshCPUData S;
		CSWall::BuildBody(Path, Skins, CSWall::FBodyParams(), S);
		int32 Horizontal = 0, Sides = 0;
		for (int32 V = 0; V < S.Normals.Num(); ++V)
		{
			if (FMath::Abs(S.Normals[V].Z) > 0.5f) continue;   // 顶 / 底
			++Sides;
			Horizontal += FMath::Abs(S.Normals[V].Z) < 1.0e-4f ? 1 : 0;
		}
		TestEqual(TEXT("弯墙侧面法线全水平"), Horizontal, Sides);
		// 取第 5 个接点：它在第 4 段的终点与第 5 段的起点上各出现一次（右侧面），两处法线相同。
		const FVector3f Expected(FVector(Skins.Normal[5].X, Skins.Normal[5].Y, 0.0));
		int32 Matches = 0;
		const FVector3f Corner(FVector(Skins.Right[5].X, Skins.Right[5].Y, 0.0));
		for (int32 V = 0; V < S.Positions.Num(); ++V)
		{
			if (!S.Positions[V].Equals(Corner, 1.0e-3f)) continue;
			if (FMath::Abs(S.Normals[V].Z) > 0.5f) continue;
			Matches += S.Normals[V].Equals(Expected, 1.0e-4f) ? 1 : 0;
		}
		TestTrue(TEXT("接点两侧共用平分线法线"), Matches >= 2);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallTopAndQuoinsTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.TopBricksAndQuoins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallTopAndQuoinsTest::RunTest(const FString& Parameters)
{
	CSHouseFrame::FBrickParams Bricks;
	Bricks.Length = 30.0f;
	Bricks.MaxBricks = 4096;
	CSWall::FTopParams Top;
	Top.BrickLength = 30.0f;
	Top.CourseHeight = 18.0f;
	Top.Seed = 7;

	// ① L 形：在直角处断成两段；开口段块数取奇数 ⇒ 首尾两块都是垛。
	//    600 / 30 = 20（偶）→ 21（砖长 28.6，比 19 块的 31.6 更接近标称）；450 / 30 = 15（奇）。
	{
		const FCSWallPath Path = CSWallTest_LPath();
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		TArray<CSHouseFrame::FElement> Elements;
		const CSWall::FTopResult R = CSWall::BuildTopElements(Path, Skins, Top, Bricks, Elements);
		AddInfo(FString::Printf(TEXT("L 形墙顶：%d 段，压顶 %d，垛口 %d"), R.Runs, R.CopingBricks, R.MerlonBricks));
		TestEqual(TEXT("直角处断开：两段"), R.Runs, 2);
		TestEqual(TEXT("压顶 21 + 15"), R.CopingBricks, 36);
		TestEqual(TEXT("垛口 11 + 8（奇数块、首尾都是垛）"), R.MerlonBricks, 19);
		TestTrue(TEXT("每条砖路恰好一块砖"), CSWallTest_AllBricksAreSingle(Elements));
		TestEqual(TEXT("砖序连续"), CSHouseFrame::NextBrickSlot(Elements), 36 + 19);

		// 第一块压顶砖从墙头起铺；两段在拐角处各自收头（第二段第一块从拐角点起）。
		TestTrue(TEXT("第一块砖起点在墙头"), FVector(Elements[0].Frame.Origin).Equals(FVector(0.0, 0.0, 150.0), 1.0e-3));
		bool bCornerStart = false;
		for (const CSHouseFrame::FElement& E : Elements)
		{
			bCornerStart |= FVector(E.Frame.Origin).Equals(FVector(600.0, 0.0, 150.0), 1.0e-3)
				&& FVector(E.Frame.AxisU).Equals(FVector(0.0, 1.0, 0.0), 1.0e-4);
		}
		TestTrue(TEXT("第二段从拐角点起铺、朝 +Y"), bCornerStart);

		// 角石：两个墙端各两根 + 拐角外侧一根。
		TArray<CSHouseQuoin::FQuoin> Quoins;
		TestEqual(TEXT("L 形角石柱数 = 4 + 1"), CSWallTest_Quoins(Path, Skins, true, true, 30.0f, Quoins), 5);
		const CSHouseQuoin::FQuoin* Corner = Quoins.FindByPredicate([](const CSHouseQuoin::FQuoin& Q) { return Q.CornerIndex == 1; });
		if (TestNotNull(TEXT("拐角那根"), Corner))
		{
			TestTrue(TEXT("拐角角石在外皮（右皮）上"), Corner->Point.Equals(Skins.Right[1], 1.0e-6));
			TestTrue(TEXT("平分线朝外"), Corner->Outward.Equals(FVector2D(1.0, -1.0).GetSafeNormal(), 1.0e-6));
			TestEqual(TEXT("直角"), double(Corner->HalfTurnCos), double(UE_INV_SQRT_2), 1.0e-5);
			TestEqual(TEXT("柱底 = 墙脚"), Corner->BottomZ, 0.0f);
			TestEqual(TEXT("柱顶 = 墙顶"), Corner->TopZ, 150.0f);
		}
		TArray<CSHouseQuoin::FQuoin> EndsOnly;
		TestEqual(TEXT("只要墙端：4 根"), CSWallTest_Quoins(Path, Skins, true, false, 30.0f, EndsOnly), 4);
	}

	// ② 闭合正方形：四段各自偶数块（整圈的接缝处不出两个相邻的垛）；没有墙端，四个直角各一根。
	{
		const FCSWallPath Path = CSWallTest_Square();
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		TArray<CSHouseFrame::FElement> Elements;
		const CSWall::FTopResult R = CSWall::BuildTopElements(Path, Skins, Top, Bricks, Elements);
		TestEqual(TEXT("正方形四段"), R.Runs, 4);
		// 400 / 30 = 13（奇）→ 开口段取奇数 ⇒ 保持 13；四段都是两头挨着拐角的开口段。
		TestEqual(TEXT("正方形压顶 4 × 13"), R.CopingBricks, 52);
		TArray<CSHouseQuoin::FQuoin> Quoins;
		TestEqual(TEXT("正方形角石 4 根（没有墙端）"), CSWallTest_Quoins(Path, Skins, true, true, 30.0f, Quoins), 4);
	}

	// ③ 弯墙：中间的点不是控制点 ⇒ 一段到底、没有拐角角石；只有两个墙端的四根。
	{
		const FCSWallPath Path = CSWallTest_Arc();
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		TArray<CSHouseFrame::FElement> Elements;
		const CSWall::FTopResult R = CSWall::BuildTopElements(Path, Skins, Top, Bricks, Elements);
		TestEqual(TEXT("弯墙一段到底"), R.Runs, 1);
		TestTrue(TEXT("弯墙压顶块数为奇数"), (R.CopingBricks & 1) == 1);
		TArray<CSHouseQuoin::FQuoin> Quoins;
		TestEqual(TEXT("弯墙只有墙端角石"), CSWallTest_Quoins(Path, Skins, true, true, 30.0f, Quoins), 4);

		// 整圈闭合的弯墙（圆）：一段、偶数块。
		FCSWallPath Circle;
		for (int32 K = 0; K < 32; ++K)
		{
			const double A = double(UE_TWO_PI) * K / 32;
			Circle.Points.Add(FVector2D(500.0 * FMath::Cos(A), 500.0 * FMath::Sin(A)));
			Circle.BaseZ.Add(0.0);
			Circle.TopZ.Add(150.0);
		}
		Circle.bClosed = true;
		FCSWallSkins CircleSkins;
		CSWall::BuildSkins(Circle, 34.0f, CircleSkins);
		TArray<CSHouseFrame::FElement> CircleElements;
		const CSWall::FTopResult C = CSWall::BuildTopElements(Circle, CircleSkins, Top, Bricks, CircleElements);
		TestEqual(TEXT("圆墙一段"), C.Runs, 1);
		TestTrue(TEXT("圆墙压顶块数为偶数"), (C.CopingBricks & 1) == 0);
		TestEqual(TEXT("圆墙垛口 = 压顶的一半"), C.MerlonBricks, C.CopingBricks / 2);
	}

	// ④ 坡上的墙顶：砖的 U 轴带坡度（砖贴着墙顶起伏，不是一级一级的台阶）。
	{
		FCSWallPath Path;
		Path.Points = { FVector2D(0.0, 0.0), FVector2D(600.0, 0.0) };
		Path.BaseZ = { 0.0, 60.0 };
		Path.TopZ = { 150.0, 210.0 };
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		TArray<CSHouseFrame::FElement> Elements;
		CSWall::BuildTopElements(Path, Skins, Top, Bricks, Elements);
		const FVector Expected = FVector(600.0, 0.0, 60.0).GetSafeNormal();
		bool bAllSloped = !Elements.IsEmpty();
		for (const CSHouseFrame::FElement& E : Elements) bAllSloped &= FVector(E.Frame.AxisU).Equals(Expected, 1.0e-4);
		TestTrue(TEXT("坡上砖的 U 轴顺着墙顶的坡"), bAllSloped);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallCornersByAngleTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.CornersByAngle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallCornersByAngleTest::RunTest(const FString& Parameters)
{
	// 用户 2026-09-22："当 spline 转化为线段夹角过大时就不会产生转角，而是产生圆滑的墙"。
	// 判据只有一条（`CSWall::IsCorner`）：相邻两段转角 ≥ 阈值才是拐角，与点是不是样条控制点无关；
	// 墙体法线 / 墙顶砖断开 / 角石 / 藤的法线四处问的是同一条。
	auto Make = [](const FVector2D& C, const TArray<uint8>& Kinks)
	{
		FCSWallPath Path;
		Path.Points = { FVector2D(0.0, 0.0), FVector2D(300.0, 0.0), C };
		Path.BaseZ = { 0.0, 0.0, 0.0 };
		Path.TopZ = { 150.0, 150.0, 150.0 };
		Path.Kinks = Kinks;
		return Path;
	};
	CSHouseFrame::FBrickParams Bricks;
	Bricks.Length = 30.0f;
	Bricks.MaxBricks = 4096;
	auto Flat = [](const FVector2D&, float& OutZ) { OutZ = 0.0f; return true; };

	struct FResult { bool bCorner = false; int32 Quoins = 0; int32 Runs = 0; int32 FacePoints = 0; bool bSharedNormal = false; };
	auto Evaluate = [&](const FCSWallPath& Path, float Threshold)
	{
		FResult R;
		FCSWallSkins Skins;
		CSWall::BuildSkins(Path, 34.0f, Skins);
		R.bCorner = CSWall::IsCorner(Skins, 1, Threshold);
		TArray<CSHouseQuoin::FQuoin> Quoins;
		R.Quoins = CSWallTest_Quoins(Path, Skins, true, true, Threshold, Quoins);
		CSWall::FTopParams Top;
		Top.CornerTurnDegrees = Threshold;
		TArray<CSHouseFrame::FElement> Elements;
		R.Runs = CSWall::BuildTopElements(Path, Skins, Top, Bricks, Elements).Runs;
		TArray<CSHouseVine::FWallStrip> Strips;
		CSWallTest_VineStrips(Path, Skins, 0.0f, 140.0f, 100.0f, Threshold, Flat, Strips);
		R.FacePoints = Strips.Num() > 0 ? Strips[0].PathBase.Num() : 0;
		// 墙体：接点 1 的右皮墙脚点在两段右侧面上出现，法线是不是同一根。
		CSWall::FBodyParams Body;
		Body.CornerTurnDegrees = Threshold;
		FCSGpuMeshCPUData S;
		CSWall::BuildBody(Path, Skins, Body, S);
		const FVector3f Corner(FVector(Skins.Right[1].X, Skins.Right[1].Y, 0.0));
		TArray<FVector3f> Normals;
		for (int32 V = 0; V < S.Positions.Num(); ++V)
		{
			if (S.Positions[V].Equals(Corner, 1.0e-3f) && FMath::Abs(S.Normals[V].Z) < 0.5f) Normals.AddUnique(S.Normals[V]);
		}
		R.bSharedNormal = Normals.Num() == 1;
		return R;
	};

	// ① 90° 的转，落在**不是**控制点的采样点上：照样是拐角。
	{
		const FResult R = Evaluate(Make(FVector2D(300.0, 300.0), { 0, 0, 0 }), 30.0f);
		TestTrue(TEXT("90° 采样点是拐角（与控制点无关）"), R.bCorner);
		TestEqual(TEXT("90°：两个墙端各两根 + 拐角一根"), R.Quoins, 5);
		TestEqual(TEXT("90°：墙顶砖在拐角处断成两段"), R.Runs, 2);
		TestEqual(TEXT("90°：藤的墙面在拐角处放两个点（各用各的面法线）"), R.FacePoints, 4);
		TestFalse(TEXT("90°：拐角两侧面法线不共用（硬棱）"), R.bSharedNormal);
	}

	// ② 20° 的转，落在控制点上：夹角大（160°），是圆滑的墙。
	const FVector2D Gentle(300.0 + 300.0 * FMath::Cos(FMath::DegreesToRadians(20.0)), 300.0 * FMath::Sin(FMath::DegreesToRadians(20.0)));
	{
		const FResult R = Evaluate(Make(Gentle, { 1, 1, 1 }), 30.0f);
		TestFalse(TEXT("20° 控制点不是拐角（圆滑的墙）"), R.bCorner);
		TestEqual(TEXT("20°：只有墙端角石"), R.Quoins, 4);
		TestEqual(TEXT("20°：墙顶砖一段到底"), R.Runs, 1);
		TestEqual(TEXT("20°：藤的墙面不断开"), R.FacePoints, 3);
		TestTrue(TEXT("20°：两段侧面共用平分线法线（无棱）"), R.bSharedNormal);
	}

	// ③ 阈值是唯一的旋钮：阈值降到 15°，同一个 20° 的接点四处一起变成拐角。
	{
		const FResult R = Evaluate(Make(Gentle, { 1, 1, 1 }), 15.0f);
		TestTrue(TEXT("阈值 15°：20° 的接点成了拐角"), R.bCorner);
		TestEqual(TEXT("阈值 15°：多出一根拐角角石"), R.Quoins, 5);
		TestEqual(TEXT("阈值 15°：墙顶砖断开"), R.Runs, 2);
		TestEqual(TEXT("阈值 15°：藤的墙面断开"), R.FacePoints, 4);
		TestFalse(TEXT("阈值 15°：棱变硬"), R.bSharedNormal);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallVineStripsTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.VineStripsBothFaces",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSWallVineStripsTest::RunTest(const FString& Parameters)
{
	const FCSWallPath Path = CSWallTest_LPath();
	FCSWallSkins Skins;
	CSWall::BuildSkins(Path, 40.0f, Skins);
	auto Flat = [](const FVector2D&, float& OutZ) { OutZ = 0.0f; return true; };
	auto Nothing = [](const FVector2D&, float&) { return false; };

	// ① 开口墙绕墙一圈四条：右面 → 远端面 → 左面 → 近端面。
	TArray<CSHouseVine::FWallStrip> Strips;
	TestEqual(TEXT("开口墙四条墙面"), CSWallTest_VineStrips(Path, Skins, 0.0f, 140.0f, 100.0f, 35.0f, Flat, Strips), 4);
	if (Strips.Num() != 4) return false;
	// 右皮外角 (620,−20)：620 + 470；左皮内角 (580,20)：430 + 580；两个端面各一个墙厚。
	TestEqual(TEXT("右面（外侧）长 1090"), double(Strips[0].Length), 1090.0, 1.0e-3);
	TestEqual(TEXT("远端面宽 = 墙厚"), double(Strips[1].Length), 40.0, 1.0e-3);
	TestEqual(TEXT("左面（内侧）长 1010"), double(Strips[2].Length), 1010.0, 1.0e-3);
	TestEqual(TEXT("近端面宽 = 墙厚"), double(Strips[3].Length), 40.0, 1.0e-3);
	TestTrue(TEXT("两个侧面是折线墙、两个端面是平面墙"), Strips[0].HasPath() && Strips[2].HasPath() && !Strips[1].HasPath() && !Strips[3].HasPath());
	TestTrue(TEXT("远端面朝 +Y（L 形第二段的方向）"), Strips[1].N.Equals(FVector(0.0, 1.0, 0.0), 1.0e-6));
	TestTrue(TEXT("近端面朝 −X"), Strips[3].N.Equals(FVector(-1.0, 0.0, 0.0), 1.0e-6));
	TestTrue(TEXT("左面从墙尾起算"), FVector2D(Strips[2].PathBase[0]).Equals(Skins.Left[2], 1.0e-6));
	TestTrue(TEXT("首尾相接：右面的终点 = 远端面的起点"), Strips[0].PathToWorld(Strips[0].Length, 0.0f, 0.0f).Equals(Strips[1].Origin, 1.0e-3));
	TestTrue(TEXT("首尾相接：远端面的终点 = 左面的起点"), (Strips[1].Origin + Strips[1].U * Strips[1].Length).Equals(Strips[2].PathBase[0], 1.0e-3));

	// ② 两个侧面都满足 N = U × Up；直墙段上法线是这一段的面法线（拐角两侧各用各的，不插值）。
	for (int32 Face : { 0, 2 })
	{
		const CSHouseVine::FWallStrip& W = Strips[Face];
		for (float S : { 100.0f, 500.0f, 900.0f })
		{
			const FVector U = (W.PathToWorld(S + 1.0f, 0.0f, 0.0f) - W.PathToWorld(S, 0.0f, 0.0f)).GetSafeNormal();
			const FVector N = W.PathNormal(S);
			TestTrue(FString::Printf(TEXT("面 %d 在 S=%.0f 处 N = U×Up"), Face, S),
				FVector::CrossProduct(U, FVector::UpVector).Equals(N, 1.0e-3));
		}
	}
	TestTrue(TEXT("右面拐角前的直墙段法线恒为 −Y"), Strips[0].PathNormal(10.0f).Equals(FVector(0.0, -1.0, 0.0), 1.0e-6)
		&& Strips[0].PathNormal(610.0f).Equals(FVector(0.0, -1.0, 0.0), 1.0e-6));
	TestTrue(TEXT("右面拐角后的直墙段法线恒为 +X"), Strips[0].PathNormal(630.0f).Equals(FVector(1.0, 0.0, 0.0), 1.0e-6));

	// ③ 规划：两个侧面都长藤、端面不起藤；每一条记录都贴着墙面（在墙外、离墙面 StandOff）。
	CSHouseVine::FParams Params;
	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
	int32 PerFace[4] = { 0, 0, 0, 0 };
	for (const CSHouseVine::FStrand& Strand : Plan.Strands) ++PerFace[FMath::Clamp(Strand.RootEdgeIndex, 0, 3)];
	AddInfo(FString::Printf(TEXT("藤：右面 %d 根 / 左面 %d 根 / 端面 %d+%d 根，枝 %d 段，叶 %d 片"),
		PerFace[0], PerFace[2], PerFace[1], PerFace[3], Plan.Branch.Num(), Plan.Leaf.Num()));
	TestTrue(TEXT("右面长藤"), PerFace[0] > 0);
	TestTrue(TEXT("左面长藤"), PerFace[2] > 0);
	TestEqual(TEXT("端面不起藤"), PerFace[1] + PerFace[3], 0);
	int32 OffWall = 0;
	for (const CSHouseVine::FRecord& R : Plan.Branch)
	{
		const double D = CSWallTest_BandDistance(Skins, false, FVector2D(R.WorldPos.X, R.WorldPos.Y));
		// 凹角附近离另一面更近，所以只卡两头：不在墙里、也不离墙面超过 StandOff。
		if (D < -0.01 || D > Params.StandOff + 1.0) ++OffWall;
	}
	TestEqual(TEXT("每一段枝的起点都在墙外、离墙面不超过 StandOff"), OffWall, 0);

	// 管子折线同一条映射：点都贴着墙（跨面的那一段在凸角处会切掉一点角，允许几厘米）。
	CSHouseVine::FTubePath Tube;
	CSHouseVine::PackTubePath(Strips, Plan, Params, 2, 0.2f, TArray<float>(), Tube);
	int32 TubeOff = 0;
	double WorstIn = 0.0, WorstOut = 0.0;
	for (const FVector4f& P : Tube.Points)
	{
		const double D = CSWallTest_BandDistance(Skins, false, FVector2D(P.X, P.Y));
		WorstIn = FMath::Min(WorstIn, D);
		WorstOut = FMath::Max(WorstOut, D);
		if (D < -8.0 || D > Params.StandOff + 8.0) ++TubeOff;
	}
	AddInfo(FString::Printf(TEXT("管子点离墙面 [%.1f, %.1f] cm"), WorstIn, WorstOut));
	TestTrue(TEXT("管子有点"), Tube.Points.Num() > 0);
	TestEqual(TEXT("管子的每个点都贴着墙面"), TubeOff, 0);

	// ④ 脚下没有地面：一根都不长（与房子 09-21 同一条裁决）。
	TArray<CSHouseVine::FWallStrip> Floating;
	CSWallTest_VineStrips(Path, Skins, 0.0f, 140.0f, 100.0f, 35.0f, Nothing, Floating);
	CSHouseVine::FPlan NoPlan;
	CSHouseVine::BuildPlan(Floating, TArray<FCSWallOpening>(), Params, NoPlan);
	TestEqual(TEXT("悬空的墙零藤"), NoPlan.Strands.Num(), 0);

	// ⑤ 闭合的墙：两面各自成环（bLoop），藤跨过 S 的接缝接着长，不去找"隔壁那面墙"。
	{
		const FCSWallPath Square = CSWallTest_Square();
		FCSWallSkins SquareSkins;
		CSWall::BuildSkins(Square, 40.0f, SquareSkins);
		TArray<CSHouseVine::FWallStrip> Loops;
		TestEqual(TEXT("闭合墙两条墙面"), CSWallTest_VineStrips(Square, SquareSkins, 0.0f, 140.0f, 100.0f, 35.0f, Flat, Loops), 2);
		TestTrue(TEXT("闭合墙两面都成环"), Loops.Num() == 2 && Loops[0].bLoop && Loops[1].bLoop);
		if (Loops.Num() == 2) TestEqual(TEXT("闭合墙外面长 = 4 × 440"), double(Loops[0].Length), 1760.0, 1.0e-3);
		CSHouseVine::FPlan LoopPlan;
		CSHouseVine::BuildPlan(Loops, TArray<FCSWallOpening>(), Params, LoopPlan);
		TestTrue(TEXT("闭合墙长藤"), LoopPlan.Strands.Num() > 0);
		int32 LoopOff = 0;
		for (const CSHouseVine::FRecord& R : LoopPlan.Branch)
		{
			const double D = CSWallTest_BandDistance(SquareSkins, true, FVector2D(R.WorldPos.X, R.WorldPos.Y));
			if (D < -0.01 || D > Params.StandOff + 1.0) ++LoopOff;
		}
		TestEqual(TEXT("闭合墙每一段枝的起点都在墙外、离墙面不超过 StandOff"), LoopOff, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSWallActorOnGroundTest,
	"PCGPlugins.ComputeShaderGenerator.Wall.ActorOnGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSWallActorOnGroundTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 地面：默认 64×64 格 × 50 cm，从原点铺到 (3200, 3200)。石阶 / 地被 / 灌木都关掉，本用例只看墙。
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->bGroundCoverEnabled = false;
	Ground->bBuildingBushesEnabled = false;
	Ground->RebuildGroundMesh();
	// 墙拐角旁一个土包：墙的后一段从平地爬上坡。
	ACSGroundShaperActor* Mound = World->SpawnActor<ACSGroundShaperActor>(FVector(1750.0, 1300.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Mound"), Mound)) return false;
	Mound->MaskSize = 2.0f * float(200.0f);   // 圆盘直径（2026-09-29 起形状是 mask，没设 mask = 解析圆盘）
	Mound->FalloffDistance = 300.0f;
	// 台顶 = actor 高度（2026-09-29）；乘 (1 + 二次抬升) 保持与旧口径「抬 LiftHeight、台顶再照抬 2%」逐项相同的剖面。
	Mound->SetActorLocation(Mound->GetActorLocation() + FVector(0.0, 0.0, double(150.0f) * (1.0 + double(Mound->SecondaryLiftScale))));
	Mound->RebuildTerrain();

	// 墙：默认 L 形样条（本地 (0,0) → (600,0) → (600,450)），落在 (1000, 1000)。
	ACSWallActor* Wall = World->SpawnActor<ACSWallActor>(FVector(1000.0, 1000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Wall actor"), Wall)) return false;

	// ① 样条采样：50 cm 一个点，控制点恰好落在采样点上并打了角标记。
	const FCSWallPath& Path = Wall->GetBuiltPath();
	TestEqual(TEXT("采样点数 = 12 + 9 + 1"), Path.NumPoints(), 22);
	TestTrue(TEXT("三个控制点都是角"), Path.IsKink(0) && Path.IsKink(12) && Path.IsKink(21));
	TestFalse(TEXT("中间采样点不是角"), Path.IsKink(5));
	TestTrue(TEXT("接到了地面"), Wall->IsOnGround());

	// ② 贴地：墙脚 = 三处地面最低点 − 埋深；墙顶 ≈ 中线地面 + 墙高（平滑过，坡上放宽）。
	int32 BadBase = 0, BadTop = 0;
	double MinBase = TNumericLimits<double>::Max(), MaxBase = -TNumericLimits<double>::Max();
	const FCSWallSkins& Skins = Wall->GetBuiltSkins();
	for (int32 I = 0; I < Path.NumPoints(); ++I)
	{
		float GC = 0.0f, GL = 0.0f, GR = 0.0f;
		Ground->TrySampleHeight(Path.Points[I], GC);
		Ground->TrySampleHeight(Skins.Left[I], GL);
		Ground->TrySampleHeight(Skins.Right[I], GR);
		const double Expected = FMath::Min3(GC, GL, GR) - Wall->GroundSink;
		if (FMath::Abs(Path.BaseZ[I] - Expected) > 0.5) ++BadBase;
		if (FMath::Abs(Path.TopZ[I] - (GC + Wall->WallHeight)) > 40.0) ++BadTop;
		MinBase = FMath::Min(MinBase, Path.BaseZ[I]);
		MaxBase = FMath::Max(MaxBase, Path.BaseZ[I]);
	}
	AddInfo(FString::Printf(TEXT("墙脚高度 [%.1f, %.1f]"), MinBase, MaxBase));
	TestEqual(TEXT("墙脚贴地（三处最低点 − 埋深）"), BadBase, 0);
	TestEqual(TEXT("墙顶离地一个墙高（±40 cm 平滑余量）"), BadTop, 0);
	TestTrue(TEXT("墙脚跟着土包起伏"), MaxBase - MinBase > 50.0);

	// ③ 产物：墙体、砖（GPU 回读对 CPU 计数）、藤（GPU 回读对 CPU 计数）。
	TestEqual(TEXT("墙体三角形 = 21 段 × 8 + 两个端面"), Wall->GetBodyTriangleCount(), 21 * 8 + 4);
	TestTrue(TEXT("压顶砖"), Wall->GetCopingBrickCount() > 30);
	TestTrue(TEXT("垛口"), Wall->GetMerlonCount() > 15);
	TestEqual(TEXT("角石柱：两端各两根 + 拐角一根"), Wall->GetQuoinColumnCount(), 5);
	const int32 GpuBricks = Wall->DebugReadBrickCountGpuSync();
	AddInfo(FString::Printf(TEXT("砖：CPU %d / GPU %d；藤：%d 根、叶 %d、花 %d"), Wall->GetBrickCount(), GpuBricks,
		Wall->GetVineStrandCount(), Wall->GetVineLeafCount(), Wall->GetVineFlowerCount()));
	TestEqual(TEXT("GPU 真的在画这么多块砖"), GpuBricks, Wall->GetBrickCount());
	TestTrue(TEXT("墙两面长了藤"), Wall->GetVineStrandCount() > 4);
	TestEqual(TEXT("GPU 真的在画这么多片叶"), Wall->DebugReadVineLeafCountGpuSync(), Wall->GetVineLeafCount());
	TestEqual(TEXT("画的是资产本身"), Wall->DebugGetGpuAssetMismatchSync(), FString());

	// ④ 幂等：什么都没变的重求值，一次上传 / 一次重排都不做。
	const int32 Uploads = Wall->GetBodyUploadCount();
	const int32 Scatters = Wall->GetBrickScatterCount();
	Wall->ReevaluateSite();
	TestEqual(TEXT("无变化：墙体不重传"), Wall->GetBodyUploadCount(), Uploads);
	TestEqual(TEXT("无变化：砖不重排"), Wall->GetBrickScatterCount(), Scatters);

	// ⑤ 改样条：拖远末点 ⇒ 墙变长、重建一次。
	const float LengthBefore = Wall->GetWallLength();
	Wall->GetSpline()->SetLocationAtSplinePoint(2, FVector(600.0, 800.0, 0.0), ESplineCoordinateSpace::Local, true);
	Wall->FlushPendingReevaluate();
	TestTrue(TEXT("改样条后墙变长"), Wall->GetWallLength() > LengthBefore + 300.0f);
	TestTrue(TEXT("改样条后墙体重传"), Wall->GetBodyUploadCount() > Uploads);

	// ⑥ 闭合：墙端没了（端头角石清掉），拐角角石按转角判（这条闭合边的两个角都比直角尖，不出）。
	Wall->GetSpline()->SetClosedLoop(true, true);
	Wall->FlushPendingReevaluate();
	TestTrue(TEXT("闭合后路径闭合"), Wall->GetBuiltPath().bClosed);
	TestEqual(TEXT("闭合后只剩一根拐角角石"), Wall->GetQuoinColumnCount(), 1);
	TestEqual(TEXT("闭合后墙体没有端面"), Wall->GetBodyTriangleCount(), Wall->GetPathPointCount() * 8);
	Wall->GetSpline()->SetClosedLoop(false, true);

	// ⑦ 地面没了：藤清空（收不到地面通知的藤不留），墙照样砌在样条高度上。
	Ground->Destroy();
	Wall->RebuildWall();
	TestFalse(TEXT("没有地面"), Wall->IsOnGround());
	TestEqual(TEXT("没有地面：零藤"), Wall->GetVineStrandCount(), 0);
	TestTrue(TEXT("没有地面：墙体照砌"), Wall->GetBodyTriangleCount() > 0);
	TestEqual(TEXT("没有地面：墙脚 = 样条高度"), Wall->GetBuiltPath().BaseZ[0], 0.0, 1.0e-6);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

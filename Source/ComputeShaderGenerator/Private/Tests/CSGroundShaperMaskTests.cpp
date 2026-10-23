#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSGroundShaperActor.h"
#include "CSGroundShaperField.h"
#include "CSMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

// -----------------------------------------------------------------------------
// 塑形物改 mask 驱动（2026-09-29）的验收。
//
//   ① MaskDiscMatchesAnalytic —— 圆形 mask 烘出来的场与解析圆盘几乎重合（烘焙链路本身没偏）。
//   ② ActorHeightIsTop       —— actor 在多高，台顶就是多高（含二次抬升、含平滑合成）。
//   ③ SmoothingRoundsStar    —— 五角星的凸尖被削、凹口那道裙边折痕被抹平；台顶不超 actor 高度。
//   ④ SmoothMergeFillsCrease —— 两座交汇处的折痕被 smooth-max 倒角，只往上填、不超峰高。
//   ⑤ MaskCpuGpuParity       —— mask + 平滑合成 + 中心隆起 + 坑下 CPU 镜像与 GPU 逐顶点同数。需要真 RHI。
//   ⑥ CenterBulgePeaksAtActor —— 中心隆起的最高点恒 = actor 高度、落在离边最远处；平滑只压低其余部分。
//   ⑦ NegativeHeightDigsPit   —— actor 在地面以下 = 坑，坑底恒 = actor 高度；只往下挖、不抬高。
//
// ①–④ 纯 CPU，理由同 CSGroundShaperFieldTests.cpp 的文件头：被测的是公式，⑤ 钉住两侧不分叉。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSShaperMaskTest_ 前缀。

constexpr int32 CSShaperMaskTest_Res = 256;

/** 程序化 mask（G8，白 = 里面）。不落盘：只填 Source，烘焙只读 Source。 */
UTexture2D* CSShaperMaskTest_MakeTexture(TFunctionRef<bool(double U, double V)> Inside)
{
	TArray<uint8> Pixels;
	Pixels.SetNumUninitialized(CSShaperMaskTest_Res * CSShaperMaskTest_Res);
	for (int32 Y = 0; Y < CSShaperMaskTest_Res; ++Y)
	{
		for (int32 X = 0; X < CSShaperMaskTest_Res; ++X)
		{
			const double U = (double(X) + 0.5) / CSShaperMaskTest_Res;
			const double V = (double(Y) + 0.5) / CSShaperMaskTest_Res;
			Pixels[Y * CSShaperMaskTest_Res + X] = Inside(U, V) ? 255 : 0;
		}
	}
	UTexture2D* Texture = NewObject<UTexture2D>(GetTransientPackage(), NAME_None, RF_Transient);
	Texture->Source.Init(CSShaperMaskTest_Res, CSShaperMaskTest_Res, 1, 1, TSF_G8, Pixels.GetData());
	Texture->SRGB = false;
	return Texture;
}

UTexture2D* CSShaperMaskTest_Disc()
{
	return CSShaperMaskTest_MakeTexture([](double U, double V)
	{
		return FMath::Square(U - 0.5) + FMath::Square(V - 0.5) <= 0.25;
	});
}

/** 五角星：外半径 0.48、内半径 0.19（uv），第一个尖朝局部 +X。 */
constexpr double CSShaperMaskTest_StarOuter = 0.48;
constexpr double CSShaperMaskTest_StarInner = 0.19;

UTexture2D* CSShaperMaskTest_Star()
{
	TArray<FVector2D> Poly;
	for (int32 I = 0; I < 10; ++I)
	{
		const double A = UE_DOUBLE_PI * double(I) / 5.0;
		const double R = (I % 2 == 0) ? CSShaperMaskTest_StarOuter : CSShaperMaskTest_StarInner;
		Poly.Add(FVector2D(0.5 + R * FMath::Cos(A), 0.5 + R * FMath::Sin(A)));
	}
	return CSShaperMaskTest_MakeTexture([Poly](double U, double V)
	{
		bool bIn = false;
		for (int32 I = 0, J = Poly.Num() - 1; I < Poly.Num(); J = I++)
		{
			if (((Poly[I].Y > V) != (Poly[J].Y > V))
				&& (U < (Poly[J].X - Poly[I].X) * (V - Poly[I].Y) / (Poly[J].Y - Poly[I].Y) + Poly[I].X))
			{
				bIn = !bIn;
			}
		}
		return bIn;
	});
}

ACSGroundShaperActor* CSShaperMaskTest_Spawn(UWorld* World, const FVector& Location, UTexture2D* Mask,
	float MaskSize, float Falloff, float Smoothing)
{
	ACSGroundShaperActor* Shaper = World->SpawnActor<ACSGroundShaperActor>(Location, FRotator::ZeroRotator);
	if (!Shaper) return nullptr;
	Shaper->Mask = Mask;
	Shaper->MaskSize = MaskSize;
	Shaper->FalloffDistance = Falloff;
	Shaper->Smoothing = Smoothing;
	Shaper->SkirtNoiseAmount = 0.0f;   // 本文件量的是形状，噪声只会给每条断言加一个无关的容差
	Shaper->RebuildTerrain();
	return Shaper;
}

/** 几座塑形物的合成场（与地面镜像、GPU 同一份打包 + EvalField）。 */
float CSShaperMaskTest_Field(TConstArrayView<const ACSGroundShaperActor*> Shapers, const FVector2D& P)
{
	TArray<CSGroundShaperField::FShaperHeader> Headers;
	TArray<TConstArrayView<float>> Sdfs;
	for (const ACSGroundShaperActor* Shaper : Shapers)
	{
		Headers.Add(Shaper->GetFieldHeader());
		Sdfs.Add(Shaper->GetMaskSdf());
	}
	TArray<FVector4f> Packed;
	CSGroundShaperField::PackField(Headers, Sdfs, Packed);
	return CSGroundShaperField::EvalField(FVector2f(float(P.X), float(P.Y)), Packed);
}

/** 沿方向 Dir 过点 P 的离散二阶差分的绝对值（步长 H）—— 折痕越尖越大。 */
double CSShaperMaskTest_Kink(TFunctionRef<double(const FVector2D&)> Height, const FVector2D& P, const FVector2D& Dir, double H)
{
	return FMath::Abs(Height(P + Dir * H) - 2.0 * Height(P) + Height(P - Dir * H)) / (H * H);
}
}

// -----------------------------------------------------------------------------
// ① 圆形 mask ≈ 解析圆盘
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperMaskDiscTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.MaskDiscMatchesAnalytic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperMaskDiscTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	const FVector Loc(0.0, 0.0, 400.0);
	ACSGroundShaperActor* Masked = CSShaperMaskTest_Spawn(World, Loc, CSShaperMaskTest_Disc(), 600.0f, 400.0f, 0.0f);
	ACSGroundShaperActor* Analytic = CSShaperMaskTest_Spawn(World, Loc, nullptr, 600.0f, 400.0f, 0.0f);
	if (!TestNotNull(TEXT("Masked"), Masked) || !TestNotNull(TEXT("Analytic"), Analytic)) return false;
	TestTrue(TEXT("The mask really got baked into an SDF"), Masked->GetMaskSdf().Num() > 0);
	TestEqual(TEXT("No mask = no SDF payload"), Analytic->GetMaskSdf().Num(), 0);

	double Worst = 0.0;
	for (int32 Iy = -40; Iy <= 40; ++Iy)
	{
		for (int32 Ix = -40; Ix <= 40; ++Ix)
		{
			const FVector2D P(Ix * 20.0, Iy * 20.0);
			Worst = FMath::Max(Worst, FMath::Abs(double(Masked->SampleShapeHeight(P)) - double(Analytic->SampleShapeHeight(P))));
		}
	}
	// 1% 台高：256² 的 mask、10 cm 的 SDF 格、1.5 格的去锯齿模糊，零等值线的偏差在一两厘米，
	// 放到最陡的裙边中段（斜率 1.5 × 400/400）也就几厘米。
	TestTrue(*FString::Printf(TEXT("A disc mask reproduces the analytic disc (worst %.2f cm)"), Worst), Worst < 4.0);
	TestTrue(TEXT("Contour radius of the baked disc ≈ 300"), FMath::IsNearlyEqual(Masked->GetContourRadius(), 300.0f, 20.0f));

	World->DestroyActor(Masked);
	World->DestroyActor(Analytic);
	return true;
}

// -----------------------------------------------------------------------------
// ② actor 高度就是台顶
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperActorHeightIsTopTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.ActorHeightIsTop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperActorHeightIsTopTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundShaperActor* A = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, 600.0), CSShaperMaskTest_Star(), 1200.0f, 400.0f, 150.0f);
	ACSGroundShaperActor* B = CSShaperMaskTest_Spawn(World, FVector(700.0, 0.0, 600.0), nullptr, 800.0f, 400.0f, 150.0f);
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B)) return false;
	TestTrue(TEXT("Secondary lift is on by default (the case that used to overshoot)"), A->SecondaryLiftScale > 0.0f);
	A->SkirtNoiseAmount = 0.5f;   // 噪声只减不加，开着也不许超

	const TArray<const ACSGroundShaperActor*> Both = { A, B };
	double Peak = 0.0;
	for (int32 Iy = -60; Iy <= 60; ++Iy)
	{
		for (int32 Ix = -60; Ix <= 100; ++Ix)
		{
			Peak = FMath::Max(Peak, double(CSShaperMaskTest_Field(Both, FVector2D(Ix * 15.0, Iy * 15.0))));
		}
	}
	TestTrue(*FString::Printf(TEXT("Actor at 600 ⇒ terrain tops out at 600 (peak %.3f)"), Peak),
		FMath::IsNearlyEqual(Peak, 600.0, 0.05));
	TestTrue(TEXT("Plateau centre is exactly the actor height"),
		FMath::IsNearlyEqual(double(A->SampleShapeHeight(FVector2D::ZeroVector)), 600.0, 0.05));

	World->DestroyActor(A);
	World->DestroyActor(B);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 平滑把星形的尖处理掉
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperSmoothingStarTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.SmoothingRoundsStar",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperSmoothingStarTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr double Size = 1500.0;
	constexpr double Top = 500.0;
	UTexture2D* Star = CSShaperMaskTest_Star();
	ACSGroundShaperActor* Sharp = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, Top), Star, float(Size), 500.0f, 0.0f);
	ACSGroundShaperActor* Smooth = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, Top), Star, float(Size), 500.0f, 250.0f);
	if (!TestNotNull(TEXT("Sharp"), Sharp) || !TestNotNull(TEXT("Smooth"), Smooth)) return false;
	auto HSharp = [Sharp](const FVector2D& P) { return double(Sharp->SampleShapeHeight(P)); };
	auto HSmooth = [Smooth](const FVector2D& P) { return double(Smooth->SampleShapeHeight(P)); };

	// 凸尖：第一个尖朝 +X。沿尖的轴线量台顶（h == 台高）一直延伸到哪：不平滑时几乎到尖端，
	// 平滑后尖被削圆，台顶明显往回缩。
	const double TipR = CSShaperMaskTest_StarOuter * Size;
	auto PlateauEnd = [TipR, Top](TFunctionRef<double(const FVector2D&)> H)
	{
		double End = 0.0;
		for (double X = 0.0; X <= TipR + 100.0; X += 2.0)
		{
			if (H(FVector2D(X, 0.0)) >= Top - 0.01) End = X;
		}
		return End;
	};
	const double SharpEnd = PlateauEnd(HSharp);
	const double SmoothEnd = PlateauEnd(HSmooth);
	AddInfo(FString::Printf(TEXT("尖端 %.0f：台顶沿轴线延伸到 不平滑 %.0f / 平滑 %.0f"), TipR, SharpEnd, SmoothEnd));
	TestTrue(TEXT("Without smoothing the plateau reaches (almost) the tip"), SharpEnd > TipR - 40.0);
	TestTrue(TEXT("Smoothing shaves the convex tip: the plateau recedes by > 50 cm"), SmoothEnd < SharpEnd - 50.0);

	// 凹口：两尖之间（36°）的方向。凹口外的裙边上，距离场有一道沿角平分线的梯度断线 ⇒ 地形上一道折痕。
	// 横跨平分线量二阶差分：平滑后要明显变钝。
	const double NotchA = UE_DOUBLE_PI / 5.0;
	const FVector2D Bisector(FMath::Cos(NotchA), FMath::Sin(NotchA));
	const FVector2D Across(-Bisector.Y, Bisector.X);
	const double NotchR = CSShaperMaskTest_StarInner * Size;
	double KinkSharp = 0.0, KinkSmooth = 0.0;
	for (double Out = 80.0; Out <= 320.0; Out += 40.0)
	{
		const FVector2D P = Bisector * (NotchR + Out);
		KinkSharp = FMath::Max(KinkSharp, CSShaperMaskTest_Kink(HSharp, P, Across, 25.0));
		KinkSmooth = FMath::Max(KinkSmooth, CSShaperMaskTest_Kink(HSmooth, P, Across, 25.0));
	}
	AddInfo(FString::Printf(TEXT("凹口外裙边横向二阶差分：不平滑 %.4f / 平滑 %.4f"), KinkSharp, KinkSmooth));
	TestTrue(TEXT("The notch crease is really there without smoothing"), KinkSharp > 0.01);
	TestTrue(TEXT("Smoothing blunts the notch crease to less than half"), KinkSmooth < 0.5 * KinkSharp);

	// 平滑不许抬高台顶。
	double Peak = 0.0;
	for (int32 Iy = -50; Iy <= 50; ++Iy)
	{
		for (int32 Ix = -50; Ix <= 50; ++Ix) Peak = FMath::Max(Peak, HSmooth(FVector2D(Ix * 25.0, Iy * 25.0)));
	}
	TestTrue(*FString::Printf(TEXT("Smoothing never lifts above the actor (peak %.3f)"), Peak), Peak <= Top + 0.05);

	World->DestroyActor(Sharp);
	World->DestroyActor(Smooth);
	return true;
}

// -----------------------------------------------------------------------------
// ④ 两座交汇的折痕被倒角
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperSmoothMergeTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.SmoothMergeFillsCrease",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperSmoothMergeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 高台 500 与低台 300 相接：高台的裙坡落在低台顶上，坡脚是一道"谷"形折痕。
	ACSGroundShaperActor* High = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, 500.0), nullptr, 600.0f, 500.0f, 0.0f);
	ACSGroundShaperActor* Low = CSShaperMaskTest_Spawn(World, FVector(900.0, 0.0, 300.0), nullptr, 1000.0f, 400.0f, 0.0f);
	if (!TestNotNull(TEXT("High"), High) || !TestNotNull(TEXT("Low"), Low)) return false;
	const TArray<const ACSGroundShaperActor*> Pair = { High, Low };

	auto Field = [&Pair](const FVector2D& P) { return double(CSShaperMaskTest_Field(Pair, P)); };
	// 折痕位置：沿 +X 找高台贡献跌到低台台顶的那一点。
	double CreaseX = 0.0;
	for (double X = 300.0; X < 800.0; X += 1.0)
	{
		if (double(High->SampleShapeHeight(FVector2D(X, 0.0))) <= double(Low->SampleShapeHeight(FVector2D(X, 0.0)))) { CreaseX = X; break; }
	}
	if (!TestTrue(TEXT("Found the crease"), CreaseX > 0.0)) return false;

	auto MaxKink = [&](double Lo, double Hi)
	{
		double K = 0.0;
		for (double X = Lo; X <= Hi; X += 5.0) K = FMath::Max(K, CSShaperMaskTest_Kink(Field, FVector2D(X, 0.0), FVector2D(1.0, 0.0), 10.0));
		return K;
	};
	const double KinkOff = MaxKink(CreaseX - 60.0, CreaseX + 60.0);

	High->Smoothing = 200.0f;
	Low->Smoothing = 200.0f;
	const double KinkOn = MaxKink(CreaseX - 60.0, CreaseX + 60.0);
	AddInfo(FString::Printf(TEXT("折痕 x=%.0f，二阶差分：平滑关 %.4f / 开 %.4f"), CreaseX, KinkOff, KinkOn));
	TestTrue(TEXT("Smooth merge blunts the junction crease to less than half"), KinkOn < 0.5 * KinkOff);

	// 只往上填、封顶在最高台顶、远离交汇处逐位不变。
	int32 Dug = 0;
	double Peak = 0.0;
	for (double X = -900.0; X <= 1900.0; X += 10.0)
	{
		for (double Y = -600.0; Y <= 600.0; Y += 50.0)
		{
			const FVector2D P(X, Y);
			const double H = Field(P);
			const double M = FMath::Max(double(High->SampleShapeHeight(P)), double(Low->SampleShapeHeight(P)));
			if (H < M - 1.0e-3) ++Dug;
			Peak = FMath::Max(Peak, H);
		}
	}
	TestEqual(TEXT("Smooth merge only fills, never digs below max"), Dug, 0);
	TestTrue(*FString::Printf(TEXT("Smooth merge is capped by the tallest plateau (peak %.3f)"), Peak), Peak <= 500.0 + 0.05);
	const FVector2D Far(1700.0, 0.0);   // 只有低台（外沿），高台够不着
	TestEqual(TEXT("Away from the junction the field is untouched"),
		Field(Far), double(Low->SampleShapeHeight(Far)), 1.0e-4);

	World->DestroyActor(High);
	World->DestroyActor(Low);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ mask + 平滑合成下 CPU 镜像与 GPU 逐顶点同数
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperMaskParityTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.MaskCpuGpuParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundShaperMaskParityTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->RebuildGroundMesh();

	// 默认地面 64 格 × 50 cm = 3200 cm。一座转了 20° 的星形 + 一座圆盘 mask，交汇、带噪声、带平滑。
	ACSGroundShaperActor* A = World->SpawnActor<ACSGroundShaperActor>(FVector(1200.0, 1600.0, 450.0), FRotator(0.0, 20.0, 0.0));
	ACSGroundShaperActor* B = World->SpawnActor<ACSGroundShaperActor>(FVector(2200.0, 1600.0, 300.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("A"), A) || !TestNotNull(TEXT("B"), B)) return false;
	A->Mask = CSShaperMaskTest_Star();
	A->MaskSize = 1400.0f;
	A->FalloffDistance = 300.0f;
	A->Smoothing = 150.0f;
	B->Mask = CSShaperMaskTest_Disc();
	B->MaskSize = 700.0f;
	B->FalloffDistance = 350.0f;
	B->Smoothing = 200.0f;
	B->SkirtNoiseSeed = 777;
	A->CenterBulge = 0.6f;
	// 一座坑压在星形的一条臂和平地之间：坑的合成趟也要两侧同数。
	ACSGroundShaperActor* Pit = World->SpawnActor<ACSGroundShaperActor>(FVector(1500.0, 2500.0, -250.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Pit"), Pit)) return false;
	Pit->Mask = CSShaperMaskTest_Disc();
	Pit->MaskSize = 600.0f;
	Pit->FalloffDistance = 300.0f;
	Pit->Smoothing = 100.0f;
	Pit->CenterBulge = 0.5f;

	const int32 BaselineDisplaces = Ground->GetGpuDisplaceCount();
	A->RebuildTerrain();
	B->RebuildTerrain();
	Pit->RebuildTerrain();
	TestTrue(TEXT("The GPU displacement pass really ran"), Ground->GetGpuDisplaceCount() > BaselineDisplaces);
	TestTrue(TEXT("Both masks baked"), A->GetMaskSdf().Num() > 0 && B->GetMaskSdf().Num() > 0);

	UCSMesh* Mesh = Ground->GetTinyGladeMesh();
	if (!TestNotNull(TEXT("Ground GPU mesh"), Mesh)) return false;
	FCSGpuMeshCPUData Readback;
	if (!TestTrue(TEXT("Reading the ground mesh back"), Mesh->ReadbackMeshSync(Readback))) return false;

	const int32 VertsX = Ground->NumCellsX + 1;
	const int32 VertsY = Ground->NumCellsY + 1;
	if (!TestEqual(TEXT("Read back one vertex per grid point"), Readback.Positions.Num(), VertsX * VertsY)) return false;

	const FVector Origin = Ground->GetActorLocation();
	double WorstDelta = 0.0;
	int32 NonFlat = 0;
	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			const FVector2D WorldXY(Origin.X + X * Ground->CellSize, Origin.Y + Y * Ground->CellSize);
			const double Cpu = double(Ground->SampleHeight(WorldXY));   // 格点上 = 镜像值（EvalField）
			const double Gpu = double(Readback.Positions[Y * VertsX + X].Z);
			if (Cpu - Origin.Z > 1.0) ++NonFlat;
			WorstDelta = FMath::Max(WorstDelta, FMath::Abs(Cpu - Gpu));
		}
	}
	TestTrue(*FString::Printf(TEXT("The sweep really covered the mounds (nonFlat=%d)"), NonFlat), NonFlat > 400);
	// 镜像写入有 0.01 cm 的幂等短路（IsNearlyEqual），所以容差同档。
	TestTrue(*FString::Printf(TEXT("CPU mirror and GPU agree with masks + smooth merge (worst=%.6f cm)"), WorstDelta), WorstDelta < 0.02);

	int32 Dug = 0;
	for (int32 I = 0; I < Readback.Positions.Num(); ++I) Dug += Readback.Positions[I].Z < Origin.Z - 1.0 ? 1 : 0;
	TestTrue(*FString::Printf(TEXT("The pit really dug into the GPU mesh (below=%d)"), Dug), Dug > 50);

	World->DestroyActor(A);
	World->DestroyActor(B);
	World->DestroyActor(Pit);
	World->DestroyActor(Ground);
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 中心隆起：顺着轮廓往中间鼓，最高点恒 = actor 高度
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperCenterBulgeTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.CenterBulgePeaksAtActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperCenterBulgeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr double Top = 600.0;
	auto ScanPeak = [](ACSGroundShaperActor* S, double Half, FVector2D& OutAt)
	{
		double Peak = -1.0e9;
		for (double Y = -Half; Y <= Half; Y += 10.0)
		{
			for (double X = -Half; X <= Half; X += 10.0)
			{
				const double H = double(S->SampleShapeHeight(FVector2D(X, Y)));
				if (H > Peak) { Peak = H; OutAt = FVector2D(X, Y); }
			}
		}
		return Peak;
	};

	// 圆盘（解析）：峰在圆心、= actor 高度；沿半径单调不增；轮廓上明显低于峰。
	ACSGroundShaperActor* Disc = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, Top), nullptr, 1000.0f, 400.0f, 0.0f);
	if (!TestNotNull(TEXT("Disc"), Disc)) return false;
	Disc->CenterBulge = 0.7f;
	FVector2D At;
	const double DiscPeak = ScanPeak(Disc, 500.0, At);
	TestTrue(*FString::Printf(TEXT("Disc bulge peak = actor height (%.3f at %.0f,%.0f)"), DiscPeak, At.X, At.Y),
		DiscPeak <= Top + 0.05 && DiscPeak > Top - 0.5 && At.Size() < 15.0);
	double Prev = 1.0e9;
	int32 Rises = 0;
	for (double R = 0.0; R <= 900.0; R += 10.0)
	{
		const double H = double(Disc->SampleShapeHeight(FVector2D(R, 0.0)));
		if (H > Prev + 1.0e-3) ++Rises;
		Prev = H;
	}
	TestEqual(TEXT("The bulge falls monotonically from the centre"), Rises, 0);
	const double Rim = double(Disc->SampleShapeHeight(FVector2D(500.0, 0.0)));
	AddInfo(FString::Printf(TEXT("圆盘隆起 0.7：峰 %.1f，轮廓处 %.1f"), DiscPeak, Rim));
	TestTrue(TEXT("The rim sits clearly below the peak"), Rim < Top - 100.0);

	// 星形 + 平滑：峰仍 = actor 高度；平滑只把其余部分往下压（臂上某点平滑后更低或持平，而峰不动）。
	UTexture2D* Star = CSShaperMaskTest_Star();
	for (const float Smooth : { 0.0f, 250.0f })
	{
		ACSGroundShaperActor* S = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, Top), Star, 1500.0f, 400.0f, Smooth);
		if (!TestNotNull(TEXT("Star"), S)) return false;
		S->CenterBulge = 1.0f;
		const double Peak = ScanPeak(S, 750.0, At);
		const double Arm = double(S->SampleShapeHeight(FVector2D(0.6 * CSShaperMaskTest_StarOuter * 1500.0, 0.0)));
		AddInfo(FString::Printf(TEXT("星形隆起 1.0 平滑 %.0f：峰 %.2f 于 (%.0f,%.0f)，臂中段 %.1f"), Smooth, Peak, At.X, At.Y, Arm));
		TestTrue(*FString::Printf(TEXT("Star bulge peak = actor height with smoothing %.0f (%.3f)"), Smooth, Peak),
			Peak <= Top + 0.05 && Peak > Top - 0.5);   // 扫描步长未必踩中最深格点；上界是硬的
		TestTrue(TEXT("The peak sits near the star's centre, not out on an arm"), At.Size() < 150.0);
		TestTrue(TEXT("The arms are lower than the peak"), Arm < Top - 50.0);
		World->DestroyActor(S);
	}

	World->DestroyActor(Disc);
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 负高度 = 坑
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperPitTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.NegativeHeightDigsPit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperPitTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 平地上一座坑：坑底 = actor 高度，坑外 = 0。
	ACSGroundShaperActor* Pit = CSShaperMaskTest_Spawn(World, FVector(0.0, 0.0, -300.0), CSShaperMaskTest_Star(), 1200.0f, 400.0f, 150.0f);
	if (!TestNotNull(TEXT("Pit"), Pit)) return false;
	Pit->SkirtNoiseAmount = 0.5f;   // 噪声只会让坑变浅，不许挖过坑底
	TestEqual(TEXT("Top height is signed"), Pit->ComputeTopHeight(), -300.0, 1.0e-6);
	const TArray<const ACSGroundShaperActor*> Alone = { Pit };
	double Lowest = 0.0, Highest = -1.0e9;
	for (double Y = -900.0; Y <= 900.0; Y += 15.0)
	{
		for (double X = -900.0; X <= 900.0; X += 15.0)
		{
			const double H = double(CSShaperMaskTest_Field(Alone, FVector2D(X, Y)));
			Lowest = FMath::Min(Lowest, H);
			Highest = FMath::Max(Highest, H);
		}
	}
	TestTrue(*FString::Printf(TEXT("Actor at -300 ⇒ the pit bottoms out at -300 (lowest %.3f)"), Lowest), FMath::IsNearlyEqual(Lowest, -300.0, 0.05));
	TestTrue(*FString::Printf(TEXT("A pit never raises the ground (highest %.4f)"), Highest), Highest <= 1.0e-4);
	TestEqual(TEXT("Outside the pit the ground is untouched"), double(CSShaperMaskTest_Field(Alone, FVector2D(1400.0, 0.0))), 0.0, 1.0e-6);

	// 坑挖在土台上：坑底仍是绝对的 −300；坑只往下挖（任何点都不高于只有土台时）。
	ACSGroundShaperActor* Mound = CSShaperMaskTest_Spawn(World, FVector(500.0, 0.0, 400.0), nullptr, 1600.0f, 400.0f, 0.0f);
	if (!TestNotNull(TEXT("Mound"), Mound)) return false;
	Pit->CenterBulge = 0.8f;   // 中心隆起对坑同样成立：最深点 = actor 高度
	const TArray<const ACSGroundShaperActor*> Both = { Mound, Pit };
	const TArray<const ACSGroundShaperActor*> MoundOnly = { Mound };
	int32 Raised = 0;
	Lowest = 0.0;
	for (double Y = -900.0; Y <= 900.0; Y += 15.0)
	{
		for (double X = -900.0; X <= 1500.0; X += 15.0)
		{
			const FVector2D P(X, Y);
			const double H = double(CSShaperMaskTest_Field(Both, P));
			if (H > double(CSShaperMaskTest_Field(MoundOnly, P)) + 1.0e-3) ++Raised;
			Lowest = FMath::Min(Lowest, H);
		}
	}
	TestEqual(TEXT("A pit on a mound only digs"), Raised, 0);
	TestTrue(*FString::Printf(TEXT("The pit on a mound still bottoms out at the absolute -300 (lowest %.3f)"), Lowest),
		FMath::IsNearlyEqual(Lowest, -300.0, 0.05));

	World->DestroyActor(Pit);
	World->DestroyActor(Mound);
	return true;
}

#endif   // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

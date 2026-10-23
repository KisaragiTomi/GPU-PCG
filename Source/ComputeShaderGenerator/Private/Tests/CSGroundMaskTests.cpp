#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSMeshRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 地面遮罩（2026-09-29 用户："地形 mask 决定草；经过处理的地面 mask 作为地面 mesh 的遮罩控制贴图"）
//
// 同一条路、同一份种子，`GroundMaskExtendDistance` 0 与 150 各算一次，钉住四件事：
//  ① 延伸 0 时遮罩 = 原始路权重（逐格点与 CPU 镜像相同）—— 不处理就是不处理，R、G 两个通道都是；
//  ② 延伸 150 时遮罩只往外加：路上仍是 1，路缘外单调衰减，核够不着的地方恰为 0；
//     G（只糊的场 = TG 的 path_mask_blurred，材质的深浅土高度竞争读它）处处不超过 R，画出来的路缘处 ≈ 0.5，窄路中线升不满 1；
//  ③ 草读的就是这张遮罩：只会更少、更矮、更挺，离路越远越接近原样，核外逐位不变；
//  ④ 地面材质画的是带着这张贴图的动态实例（项目的地面材质资产在时才查）；
//  ⑤ 采样扰动（TG 的 path mask domain warp）：贴图本身不变，路缘草线沿路左右游走，远处逐位不变。
// ①–④ 在扰动关着的时候查（`GroundMaskWarpStrength = 0`）：扰动会把路缘的草线挪几十厘米，按距离分箱的判据就不再精确。
// 草的判据按株配对而不是看分布：草本身高矮不齐（簇共享 + 逐叶抖动），光看均值分不出压矮带有多宽。
// -----------------------------------------------------------------------------

namespace
{
	/** 落点的定位键：量化到 0.1 mm，足以区分不同格、又吸掉浮点噪声（同 `Ground.CoverNearFarLayout`）。 */
	FIntVector2 CSGroundMaskTest_Key(const FVector& P)
	{
		return FIntVector2(FMath::RoundToInt(P.X * 100.0), FMath::RoundToInt(P.Y * 100.0));
	}

	struct FCSGroundMaskTest_Plant
	{
		double Height = 0.0;
		double Bend = 0.0;
	};

	/** 一次散布的全部草：落点键 → (高度缩放, 弯曲幅度)。两块缓冲按原点配对（槽位顺序由 GPU 决定，不能按下标比）。 */
	bool CSGroundMaskTest_ReadGrass(FAutomationTestBase& Test, const ACSGroundActor* Ground, TMap<FIntVector2, FCSGroundMaskTest_Plant>& Out)
	{
		Out.Reset();
		TArray<FVector> Origins, BendOrigins;
		TArray<float> Heights, Bends, Reserved;
		const int32 Count = Ground->DebugReadGroundCoverHeightsSync(0, Origins, Heights);
		const int32 BendCount = Ground->DebugReadGroundCoverCustomDataSync(0, Bends, Reserved, BendOrigins);
		if (!Test.TestTrue(TEXT("读得到草，两块缓冲株数一致"), Count > 0 && Count == BendCount)) return false;
		for (int32 Index = 0; Index < Count; ++Index) Out.FindOrAdd(CSGroundMaskTest_Key(Origins[Index])).Height = Heights[Index];
		if (!Test.TestEqual(TEXT("落点不重复"), Out.Num(), Count)) return false;
		for (int32 Index = 0; Index < BendCount; ++Index)
		{
			FCSGroundMaskTest_Plant* Plant = Out.Find(CSGroundMaskTest_Key(BendOrigins[Index]));
			if (!Test.TestNotNull(TEXT("custom data 与 packed 行按原点配得上"), Plant)) return false;
			Plant->Bend = Bends[Index];
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundMaskDrivesCoverTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.GroundMaskDrivesCover",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundMaskDrivesCoverTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube"), Cube)) return false;

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 默认 64×64 格 × 50 cm：从原点铺到 (3200, 3200)。只留草，花与石阶关掉。
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->Flowers.Empty();
	Ground->bGroundCoverEnabled = true;
	Ground->Grass.Mesh = Cube;
	Ground->Grass.DensityPerSqM = 16.0f;       // 格距 25 cm：路缘两侧每 50 cm 一箱有几百株
	Ground->Grass.LOD.bEnabled = false;
	Ground->GroundMaskExtendDistance = 0.0f;
	Ground->GroundMaskWarpStrength = 0.0f;     // ⑤ 之前都不扰动
	Ground->BrushRadius = 75.0f;               // 与 L_HouseGroundDemo 的窄路同一量级（72.8 cm）
	// 项目的地面材质（有遮罩参数）在就用它，好把 ④ 一起查了；不在也不影响前三条。
	UMaterialInterface* GroundMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/PCGPlugins/HouseTest/MI_TinyGladeGround.MI_TinyGladeGround"));
	Ground->GroundMaterial = GroundMaterial;
	Ground->RebuildGroundMesh();

	const int32 VertsX = Ground->NumCellsX + 1;
	const int32 VertsY = Ground->NumCellsY + 1;
	const double Cell = double(Ground->CellSize);
	if (!TestNotNull(TEXT("地面遮罩贴图建出来了"), Ground->GroundMask.Get())) return false;
	TestTrue(TEXT("地面遮罩一个 texel 对一个地面格点"), Ground->GroundMask->SizeX == VertsX && Ground->GroundMask->SizeY == VertsY);

	// 一条沿 Y 的直路，x = 1600 正好落在格点上。笔刷是 3D 球，落笔要贴地（平地上 SampleHeight 恒为 0）。
	const double RoadX = 1600.0;
	const double RoadY0 = 200.0;
	const double RoadY1 = 3000.0;
	Ground->BeginPaintStroke();
	for (int32 Dab = 0; Dab <= 112; ++Dab)
	{
		const double Y = FMath::Lerp(RoadY0, RoadY1, double(Dab) / 112.0);
		Ground->ApplyPaintStroke(FVector(RoadX, Y, Ground->SampleHeight(FVector2D(RoadX, Y))));
	}
	Ground->EndPaintStroke();   // 内部 FlushPaintToGpu(true) → 地面遮罩 → 地被
	FlushRenderingCommands();

	// ① 延伸 0：遮罩逐格点 = CPU 镜像的原始路权重（不糊时 G 也就是原值）
	TArray<float> MaskRaw, InnerRaw;
	if (!TestEqual(TEXT("遮罩读得回来（延伸 0）"), Ground->DebugReadGroundMaskSync(MaskRaw, &InnerRaw), VertsX * VertsY)) return false;
	if (!TestEqual(TEXT("G 通道读得回来（延伸 0）"), InnerRaw.Num(), VertsX * VertsY)) return false;
	int32 RawMismatch = 0;
	int32 InnerRawMismatch = 0;
	int32 RoadVerts = 0;
	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			const float Mirror = Ground->SampleRoadWeight(FVector2D(double(X) * Cell, double(Y) * Cell));
			if (Mirror > 0.5f) ++RoadVerts;
			if (!FMath::IsNearlyEqual(MaskRaw[Y * VertsX + X], Mirror, 1e-6f)) ++RawMismatch;
			if (!FMath::IsNearlyEqual(InnerRaw[Y * VertsX + X], Mirror, 1e-6f)) ++InnerRawMismatch;
		}
	}
	AddInfo(FString::Printf(TEXT("路格点 %d 个"), RoadVerts));
	if (!TestTrue(TEXT("路画上了"), RoadVerts > 100)) return false;
	TestEqual(TEXT("延伸 0 时遮罩 = 原始路权重（逐格点）"), RawMismatch, 0);
	TestEqual(TEXT("延伸 0 时 G 通道也 = 原始路权重（逐格点）"), InnerRawMismatch, 0);

	// 画出来的路缘 = 原始权重的 0.5 等值线（路段中部横向扫，1 cm 一步）。
	double Edge = -1.0;
	for (double Dx = 0.0; Dx < 400.0; Dx += 1.0)
	{
		if (Ground->SampleRoadWeight(FVector2D(RoadX + Dx, 1600.0)) < 0.5f)
		{
			Edge = Dx;
			break;
		}
	}
	AddInfo(FString::Printf(TEXT("路缘（R = 0.5）离中线 %.0f cm"), Edge));
	if (!TestTrue(TEXT("路缘找得到"), Edge > 30.0 && Edge < 150.0)) return false;

	TMap<FIntVector2, FCSGroundMaskTest_Plant> Off, On;
	if (!CSGroundMaskTest_ReadGrass(*this, Ground, Off)) return false;

	// ② 延伸 150：只往外加、单调衰减、核外恰为 0（编辑器里改属性走 PostEditChangeProperty，两步同一个次序）
	Ground->GroundMaskExtendDistance = 150.0f;
	Ground->RebuildGroundMask();
	Ground->RebuildGroundCover();
	FlushRenderingCommands();

	TArray<float> MaskExt, InnerExt;
	if (!TestEqual(TEXT("遮罩读得回来（延伸 150）"), Ground->DebugReadGroundMaskSync(MaskExt, &InnerExt), VertsX * VertsY)) return false;
	if (!TestEqual(TEXT("G 通道读得回来（延伸 150）"), InnerExt.Num(), VertsX * VertsY)) return false;
	int32 Shrunk = 0;
	int32 InnerAbove = 0;
	for (int32 Index = 0; Index < MaskExt.Num(); ++Index)
	{
		if (MaskExt[Index] < MaskRaw[Index] - 1e-6f) ++Shrunk;
		if (InnerExt[Index] > MaskExt[Index] + 1e-6f) ++InnerAbove;
	}
	TestEqual(TEXT("延伸只往外加：没有一个格点比原始路权重小"), Shrunk, 0);
	TestEqual(TEXT("G 处处不超过 R（浅土路不会越过深土）"), InnerAbove, 0);

	// 路段中部的一行：路上 1，路外单调不增，核够不着的地方恰为 0。
	// 核够得着的最远处 = 有权重的格点离中线 ≤ 笔刷半径（进位到格点）+ 核伸展 1.5E（进位到格）。
	const int32 MidRow = FMath::RoundToInt32(1600.0 / Cell);
	const int32 RoadCol = FMath::RoundToInt32(RoadX / Cell);
	const int32 ReachCols = FMath::CeilToInt32(double(Ground->BrushRadius) / Cell) + FMath::CeilToInt32(1.5 * 150.0 / Cell);
	TestEqual(TEXT("路中线上遮罩为 1"), MaskExt[MidRow * VertsX + RoadCol], 1.0f);
	bool bMonotone = true;
	FString Profile;
	for (int32 Dx = 0; RoadCol + Dx < VertsX && Dx <= ReachCols + 2; ++Dx)
	{
		const float Here = MaskExt[MidRow * VertsX + RoadCol + Dx];
		Profile += FString::Printf(TEXT(" %.3f"), Here);
		if (Dx > 0 && Here > MaskExt[MidRow * VertsX + RoadCol + Dx - 1] + 1e-6f) bMonotone = false;
	}
	AddInfo(FString::Printf(TEXT("中线往 +X 每格的遮罩：%s"), *Profile));
	TestTrue(TEXT("路缘外单调衰减"), bMonotone);

	// G：画出来的路缘处 ≈ 0.5（材质的锐度参数以它为中心），往路中间升、往外降；这条路只有 3 格宽，
	// 中线升不满 1 —— 正是 R 做不到、要另存 G 的原因（R 在路缘以内恒 1）。
	bool bInnerMonotone = true;
	FString InnerProfile;
	for (int32 Dx = 0; RoadCol + Dx < VertsX && Dx <= ReachCols + 2; ++Dx)
	{
		const float Here = InnerExt[MidRow * VertsX + RoadCol + Dx];
		InnerProfile += FString::Printf(TEXT(" %.3f"), Here);
		if (Dx > 0 && Here > InnerExt[MidRow * VertsX + RoadCol + Dx - 1] + 1e-6f) bInnerMonotone = false;
	}
	AddInfo(FString::Printf(TEXT("中线往 +X 每格的 G：%s"), *InnerProfile));
	TestTrue(TEXT("G 从中线往外单调不增"), bInnerMonotone);
	const float InnerCentre = InnerExt[MidRow * VertsX + RoadCol];
	TestTrue(TEXT("窄路中线的 G 在 0.5 与 1 之间（糊开了、没被推回 1）"), InnerCentre > 0.5f && InnerCentre < 0.95f);
	const double EdgeCol = (RoadX + Edge) / Cell;
	const int32 EdgeCol0 = FMath::FloorToInt32(EdgeCol);
	const float EdgeFrac = float(EdgeCol - double(EdgeCol0));
	const float InnerAtEdge = FMath::Lerp(InnerExt[MidRow * VertsX + EdgeCol0], InnerExt[MidRow * VertsX + EdgeCol0 + 1], EdgeFrac);
	AddInfo(FString::Printf(TEXT("路缘处的 G = %.3f（R 同处 = %.3f）"), InnerAtEdge,
		FMath::Lerp(MaskExt[MidRow * VertsX + EdgeCol0], MaskExt[MidRow * VertsX + EdgeCol0 + 1], EdgeFrac)));
	TestTrue(TEXT("画出来的路缘处 G ≈ 0.5"), InnerAtEdge > 0.35f && InnerAtEdge < 0.6f);
	int32 FarNonZero = 0;
	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			if (FMath::Abs(X - RoadCol) > ReachCols && MaskExt[Y * VertsX + X] != 0.0f) ++FarNonZero;
		}
	}
	TestEqual(TEXT("核够不着的格点遮罩恰为 0"), FarNonZero, 0);

	if (!CSGroundMaskTest_ReadGrass(*this, Ground, On)) return false;

	// ③ 草读的就是这张遮罩
	int32 Appeared = 0;
	for (const TPair<FIntVector2, FCSGroundMaskTest_Plant>& Pair : On)
	{
		if (!Off.Contains(Pair.Key)) ++Appeared;
	}
	AddInfo(FString::Printf(TEXT("草：延伸 0 时 %d 株，延伸 150 时 %d 株"), Off.Num(), On.Num()));
	TestEqual(TEXT("延伸只会让草变少，不会在别处冒出新的"), Appeared, 0);
	TestTrue(TEXT("路边露土的地方草确实少了"), On.Num() < Off.Num());

	const double FieldReach = double(ReachCols + 1) * Cell;   // 再加一格双线性
	constexpr int32 NumBins = 5;   // 路缘外 [0,50) [50,100) [100,150) [150,200) [200,250) cm
	double RatioSum[NumBins] = {};
	int32 Survivors[NumBins] = {};
	int32 Before[NumBins] = {};
	int32 Taller = 0, Bendier = 0, FarSamples = 0, FarChanged = 0;
	for (const TPair<FIntVector2, FCSGroundMaskTest_Plant>& Pair : Off)
	{
		const double X = double(Pair.Key.X) / 100.0;
		const double Y = double(Pair.Key.Y) / 100.0;
		const double FromCentre = FMath::Abs(X - RoadX);
		const FCSGroundMaskTest_Plant* After = On.Find(Pair.Key);
		const FCSGroundMaskTest_Plant& Was = Pair.Value;

		if (After && After->Height > Was.Height * (1.0 + 1e-5) + 1e-6) ++Taller;
		if (After && After->Bend > Was.Bend + 1e-5) ++Bendier;

		if (FromCentre > FieldReach)
		{
			++FarSamples;
			if (!After || After->Height != Was.Height || After->Bend != Was.Bend) ++FarChanged;
			continue;
		}

		// 只取路段中部：路头那一圈的路缘是弯的，到"路缘"的距离不再是 |x − 中线| − Edge。
		if (Y < RoadY0 + 400.0 || Y > RoadY1 - 400.0) continue;
		const double Outside = FromCentre - Edge;
		if (Outside < 0.0) continue;
		const int32 Bin = int32(Outside / 50.0);
		if (Bin >= NumBins) continue;
		++Before[Bin];
		if (!After) continue;
		++Survivors[Bin];
		RatioSum[Bin] += After->Height / FMath::Max(Was.Height, 1e-6);
	}

	TestEqual(TEXT("只会更矮，不会更高"), Taller, 0);
	TestEqual(TEXT("只会更挺，不会更弯"), Bendier, 0);
	AddInfo(FString::Printf(TEXT("核够不着的区域（离中线 > %.0f cm）%d 株，其中变了 %d 株"), FieldReach, FarSamples, FarChanged));
	TestTrue(TEXT("核够不着的区域有样本"), FarSamples > 1000);
	TestEqual(TEXT("核够不着的地方逐位不变"), FarChanged, 0);

	double Mean[NumBins] = {};
	for (int32 Bin = 0; Bin < NumBins; ++Bin)
	{
		Mean[Bin] = Survivors[Bin] > 0 ? RatioSum[Bin] / Survivors[Bin] : 0.0;
		AddInfo(FString::Printf(TEXT("路缘外 %3d–%3d cm：%4d → %4d 株，留下的高度比均值 %.3f"),
			Bin * 50, Bin * 50 + 50, Before[Bin], Survivors[Bin], Mean[Bin]));
	}
	TestTrue(TEXT("路缘外 0–50 cm 基本露土（留下不到两成）"), Before[0] > 50 && Survivors[0] * 5 < Before[0]);
	TestTrue(TEXT("50 cm 往外每一箱都还有草"), Survivors[1] > 20 && Survivors[2] > 50 && Survivors[3] > 50 && Survivors[4] > 50);
	TestTrue(TEXT("离路越远越接近原高（后四箱单调上升）"), Mean[1] < Mean[2] && Mean[2] < Mean[3] && Mean[3] <= Mean[4] + 1e-9);
	TestTrue(TEXT("50–100 cm 留下的草明显压矮（高度比均值 < 0.9）"), Mean[1] < 0.9);
	TestTrue(TEXT("离路 2 m 外基本复原（高度比均值 > 0.97）"), Mean[4] > 0.97);

	// ⑤ 采样扰动：走编辑器改属性的那条路（PostEditChangeProperty ⇒ 重绑材质 + 重散）。
	// 同一份随机数、只挪了采样点 ⇒ 草线相对不扰动时的横移就是扰动本身在路缘上的投影。
	Ground->GroundMaskWarpStrength = 60.0f;
	{
		FPropertyChangedEvent WarpChanged(FindFProperty<FProperty>(ACSGroundActor::StaticClass(), GET_MEMBER_NAME_CHECKED(ACSGroundActor, GroundMaskWarpStrength)));
		Ground->PostEditChangeProperty(WarpChanged);
	}
	FlushRenderingCommands();

	TArray<float> MaskWarped;
	if (!TestEqual(TEXT("遮罩读得回来（扰动开）"), Ground->DebugReadGroundMaskSync(MaskWarped), VertsX * VertsY)) return false;
	TestTrue(TEXT("扰动只挪采样点，遮罩贴图本身逐位不变"), MaskWarped == MaskExt);

	TMap<FIntVector2, FCSGroundMaskTest_Plant> Warped;
	if (!CSGroundMaskTest_ReadGrass(*this, Ground, Warped)) return false;

	// 路段中部 +X 一侧，每 25 cm 一段取离中线最近的那株 = 这一段的草线位置。
	auto GrassLine = [&](const TMap<FIntVector2, FCSGroundMaskTest_Plant>& Plants)
	{
		TMap<int32, double> Line;
		for (const TPair<FIntVector2, FCSGroundMaskTest_Plant>& Pair : Plants)
		{
			const double X = double(Pair.Key.X) / 100.0;
			const double Y = double(Pair.Key.Y) / 100.0;
			if (X <= RoadX || Y < RoadY0 + 400.0 || Y > RoadY1 - 400.0) continue;
			double& Nearest = Line.FindOrAdd(FMath::FloorToInt32(Y / 25.0), TNumericLimits<double>::Max());
			Nearest = FMath::Min(Nearest, X - RoadX);
		}
		return Line;
	};
	const TMap<int32, double> LineStraight = GrassLine(On);
	const TMap<int32, double> LineWarped = GrassLine(Warped);
	double ShiftSum = 0.0, ShiftSqSum = 0.0, ShiftMax = 0.0;
	int32 Slices = 0;
	for (const TPair<int32, double>& Pair : LineStraight)
	{
		const double* After = LineWarped.Find(Pair.Key);
		if (!After) continue;
		const double Shift = *After - Pair.Value;
		ShiftSum += Shift;
		ShiftSqSum += Shift * Shift;
		ShiftMax = FMath::Max(ShiftMax, FMath::Abs(Shift));
		++Slices;
	}
	const double ShiftMean = Slices > 0 ? ShiftSum / Slices : 0.0;
	const double ShiftStd = Slices > 0 ? FMath::Sqrt(FMath::Max(ShiftSqSum / Slices - ShiftMean * ShiftMean, 0.0)) : 0.0;
	AddInfo(FString::Printf(TEXT("扰动开：路缘草线 %d 段，相对不扰动的横移 均值 %.1f / σ %.1f / 最大 %.1f cm"), Slices, ShiftMean, ShiftStd, ShiftMax));
	TestTrue(TEXT("路段中部每一段都有草线"), Slices > 60);
	TestTrue(TEXT("草线确实在游走（横移 σ > 5 cm）"), ShiftStd > 5.0);
	TestTrue(TEXT("游走幅度受强度约束（最大横移 < 1.2 m）"), ShiftMax < 120.0);

	// 位移每个分量最多 0.775 × 60 ≈ 47 cm（斜向 66 cm），核外再让出 1 m 就一定够不着。
	int32 WarpFarSamples = 0, WarpFarChanged = 0;
	for (const TPair<FIntVector2, FCSGroundMaskTest_Plant>& Pair : On)
	{
		if (FMath::Abs(double(Pair.Key.X) / 100.0 - RoadX) <= FieldReach + 100.0) continue;
		++WarpFarSamples;
		const FCSGroundMaskTest_Plant* After = Warped.Find(Pair.Key);
		if (!After || After->Height != Pair.Value.Height || After->Bend != Pair.Value.Bend) ++WarpFarChanged;
	}
	TestTrue(TEXT("扰动够不着的区域有样本"), WarpFarSamples > 1000);
	TestEqual(TEXT("扰动够不着的地方逐位不变"), WarpFarChanged, 0);

	// ④ 地面材质画的是带着这张遮罩的动态实例
	if (GroundMaterial)
	{
		UMaterialInstanceDynamic* Drawn = Cast<UMaterialInstanceDynamic>(Ground->GetTinyGladeMeshComponent()->MeshMaterial);
		if (TestNotNull(TEXT("地面网格画的是动态实例"), Drawn))
		{
			TestTrue(TEXT("动态实例的父材质就是 GroundMaterial"), Drawn->Parent == GroundMaterial);
			UTexture* Bound = nullptr;
			if (Drawn->GetTextureParameterValue(FHashedMaterialParameterInfo(CSGroundMaskMaterial::TextureParameter), Bound))
			{
				TestTrue(TEXT("材质采的就是地面遮罩这张贴图"), Bound == Ground->GroundMask.Get());
			}
			else
			{
				AddWarning(TEXT("地面材质还没有 GroundMask 参数（没跑过 Scripts/TinyGladeMakeGroundMaterial.py），跳过贴图绑定的检查"));
			}
			// 土跟着草走：深土层的两端就是草的拒绝区间（草长满处不露土）。
			float SoilStart = -1.0f, SoilEnd = -1.0f;
			if (Drawn->GetScalarParameterValue(FHashedMaterialParameterInfo(CSGroundMaskMaterial::SoilStartParameter), SoilStart)
				&& Drawn->GetScalarParameterValue(FHashedMaterialParameterInfo(CSGroundMaskMaterial::SoilEndParameter), SoilEnd))
			{
				TestEqual(TEXT("深土从草开始变稀处起（= 草的 MaskStart）"), SoilStart, Ground->Grass.MaskStart);
				TestEqual(TEXT("深土到草长不出来处满（= 草的 MaskEnd）"), SoilEnd, Ground->Grass.MaskEnd);
			}
			else
			{
				AddWarning(TEXT("地面材质还没有 GroundMaskSoilStart/End 参数（没跑过新版 TinyGladeMakeGroundMaterial.py），跳过土跟草的检查"));
			}
			// 扰动：材质与草按同一对参数挪采样点（⑤ 已经走 PostEditChangeProperty 把 60 cm 重绑进来了）。
			FLinearColor Warp;
			if (Drawn->GetVectorParameterValue(FHashedMaterialParameterInfo(CSGroundMaskMaterial::WarpParameter), Warp))
			{
				TestEqual(TEXT("材质的扰动强度 = actor 的（草用的也是它）"), Warp.R, Ground->GroundMaskWarpStrength);
				TestTrue(TEXT("材质的扰动频率 = 1 / 波长"), FMath::IsNearlyEqual(Warp.G, 1.0f / Ground->GroundMaskWarpWavelength, 1e-7f));
				TestEqual(TEXT("材质拿来还原局部坐标的周期 = UVWorldPeriod"), Warp.B, Ground->UVWorldPeriod);
			}
			else
			{
				AddWarning(TEXT("地面材质还没有 GroundMaskWarp 参数（没跑过新版 TinyGladeMakeGroundMaterial.py），跳过扰动参数的检查"));
			}
		}
	}
	else
	{
		AddInfo(TEXT("没找到 MI_TinyGladeGround，跳过地面材质的检查"));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

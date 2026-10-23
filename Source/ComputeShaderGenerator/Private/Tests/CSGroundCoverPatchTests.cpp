#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "RenderingThread.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 地被的「聚散」与 WPO 淡出（2026-09-29）
//
// 聚散：花的保留概率随位置成片变化（`FCSGroundCoverSpecies::PatchSize` 那一组）。钉三件事：
//  ① 保留比例 ≈ Coverage + (1 − Coverage) × Sparse（按噪声分位数算，允许 ±4% 的分布误差）；
//  ② 真的成片：按 2 m 格数株数，离散指数（方差 / 均值）远大于均匀散布 —— 抖动网格本身离散指数 < 1；
//  ③ 重扫不闪：同一组参数散两次，落点逐位相同（抽签用格身份）。
// WPO 淡出：交界 = 草的近 / 远边界，草与花的组件都拿到同一个淡出区间与 WPO 关闭距离，远处组合草不拿；
// 投影的物种（花）开影子副本、不投影的（草）不开。
// -----------------------------------------------------------------------------

namespace
{
	UCSGpuInstancedMeshComponent* CSCoverPatchTest_Find(const ACSGroundActor* Ground, const UStaticMesh* Mesh)
	{
		TInlineComponentArray<UCSGpuInstancedMeshComponent*> Components;
		Ground->GetComponents(Components);
		for (UCSGpuInstancedMeshComponent* One : Components) if (One->BaseMesh == Mesh) return One;
		return nullptr;
	}

	/** 按 Bin 厘米的方格数株数，返回离散指数（方差 / 均值）。只数完全落在地面内的格，边上不满的一圈不算。 */
	double CSCoverPatchTest_Dispersion(const TArray<FVector>& Origins, const FBox2D& Rect, double Bin)
	{
		const int32 NX = FMath::FloorToInt32((Rect.Max.X - Rect.Min.X) / Bin);
		const int32 NY = FMath::FloorToInt32((Rect.Max.Y - Rect.Min.Y) / Bin);
		if (NX <= 0 || NY <= 0) return 0.0;
		TArray<int32> Counts;
		Counts.Init(0, NX * NY);
		for (const FVector& P : Origins)
		{
			const int32 X = FMath::FloorToInt32((P.X - Rect.Min.X) / Bin);
			const int32 Y = FMath::FloorToInt32((P.Y - Rect.Min.Y) / Bin);
			if (X >= 0 && Y >= 0 && X < NX && Y < NY) ++Counts[Y * NX + X];
		}
		double Mean = 0.0;
		for (int32 C : Counts) Mean += C;
		Mean /= Counts.Num();
		double Var = 0.0;
		for (int32 C : Counts) Var += FMath::Square(C - Mean);
		Var /= Counts.Num();
		return Mean > 0.0 ? Var / Mean : 0.0;
	}

	ACSGroundActor* CSCoverPatchTest_MakeGround(UWorld* World)
	{
		ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
		if (!Ground) return nullptr;
		Ground->StairMesh = nullptr;
		Ground->NumCellsX = 128;   // 64 m 见方（默认格距 50 cm）
		Ground->NumCellsY = 128;
		Ground->bGroundCoverEnabled = true;
		Ground->Flowers.Empty();
		return Ground;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundCoverPatchTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.CoverFlowerPatches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundCoverPatchTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Engine cube"), Cube)) return false;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSGroundActor* Ground = CSCoverPatchTest_MakeGround(World);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// 只留一种"花"（草关掉：Mesh 为空 = 整条跳过），密度 4 株/m² ⇒ 64 m 见方约 1.6 万株。
	Ground->Grass.Mesh = nullptr;
	FCSGroundCoverSpecies Flower;
	Flower.Mesh = Cube;
	Flower.DensityPerSqM = 4.0f;
	Flower.Salt = 11;
	Flower.PatchSize = 0.0f;
	Ground->Flowers.Add(Flower);
	Ground->RebuildGroundMesh();
	FlushRenderingCommands();

	const FBox2D Rect = Ground->GetWorldRect2D();
	TArray<FVector> Uniform;
	const int32 UniformCount = Ground->DebugReadGroundCoverOriginsSync(0, Uniform);
	const double UniformDispersion = CSCoverPatchTest_Dispersion(Uniform, Rect, 200.0);
	AddInfo(FString::Printf(TEXT("关聚散：%d 株，2 m 格离散指数 %.2f"), UniformCount, UniformDispersion));
	if (!TestTrue(TEXT("关聚散时平地一格一株（约 1.6 万）"), UniformCount > 15000)) return false;
	TestTrue(TEXT("抖动网格本身不成簇（离散指数 < 1）"), UniformDispersion < 1.0);

	// 开聚散：片 4 m、覆盖 35%、片边 0.3、片外 0.1 ⇒ 期望保留 0.1 + 0.9 × 0.35 = 0.415。
	constexpr float Coverage = 0.35f;
	constexpr float Sparse = 0.1f;
	Ground->Flowers[0].PatchSize = 400.0f;
	Ground->Flowers[0].PatchCoverage = Coverage;
	Ground->Flowers[0].PatchSoftness = 0.3f;
	Ground->Flowers[0].PatchSparseDensity = Sparse;
	Ground->RebuildGroundCover();
	FlushRenderingCommands();

	TArray<FVector> Patchy;
	const int32 PatchyCount = Ground->DebugReadGroundCoverOriginsSync(0, Patchy);
	const double Kept = double(PatchyCount) / double(FMath::Max(UniformCount, 1));
	const double Expected = Sparse + (1.0 - Sparse) * Coverage;
	const double PatchyDispersion = CSCoverPatchTest_Dispersion(Patchy, Rect, 200.0);
	AddInfo(FString::Printf(TEXT("开聚散：%d 株（保留 %.3f，期望 %.3f），2 m 格离散指数 %.2f"), PatchyCount, Kept, Expected, PatchyDispersion));
	// 64 m 见方里只有约 16×16 个底格，片的数量本身有涨落 —— 容差放到 ±0.08。
	TestTrue(TEXT("① 保留比例 ≈ Coverage + (1 − Coverage) × Sparse"), FMath::Abs(Kept - Expected) < 0.08);
	TestTrue(TEXT("② 成片：离散指数远大于均匀散布（> 3 且 > 均匀的 5 倍）"), PatchyDispersion > 3.0 && PatchyDispersion > UniformDispersion * 5.0);

	// ③ 重扫不闪：改一个参数再改回来，落点逐位相同。
	Ground->Flowers[0].PatchSize = 401.0f;
	Ground->RebuildGroundCover();
	Ground->Flowers[0].PatchSize = 400.0f;
	Ground->RebuildGroundCover();
	FlushRenderingCommands();
	TArray<FVector> Again;
	Ground->DebugReadGroundCoverOriginsSync(0, Again);
	auto Sorted = [](TArray<FVector> Points)
	{
		Points.Sort([](const FVector& A, const FVector& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
		return Points;
	};
	const TArray<FVector> SortedA = Sorted(Patchy);
	const TArray<FVector> SortedB = Sorted(Again);
	bool bSame = SortedA.Num() == SortedB.Num();
	for (int32 Index = 0; bSame && Index < SortedA.Num(); ++Index) bSame = SortedA[Index].Equals(SortedB[Index], 0.01);
	TestTrue(TEXT("③ 同一组参数散两次，落点逐位相同（抽签用格身份）"), bSame);

	Ground->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundCoverWPOFadeTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.CoverWPOFade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundCoverWPOFadeTest::RunTest(const FString& Parameters)
{
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UStaticMesh* Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (!TestNotNull(TEXT("Engine meshes"), Cube) || !Cylinder || !Cone) return false;
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSGroundActor* Ground = CSCoverPatchTest_MakeGround(World);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	Ground->NumCellsX = 512;   // 256 m 见方：近处窗口（半宽 70 m）装得下、不贴边
	Ground->NumCellsY = 512;
	Ground->Grass.Mesh = Cube;
	Ground->Grass.DensityPerSqM = 0.25f;
	Ground->Grass.bCastShadow = false;
	Ground->Grass.LOD.bEnabled = true;
	Ground->Grass.LOD.FarMesh = Cylinder;
	Ground->Grass.LOD.FarDensityPerSqM = 0.02f;
	Ground->Grass.LOD.NearDistance = 6000.0f;
	Ground->Grass.LOD.FadeDistance = 1500.0f;
	FCSGroundCoverSpecies Flower;
	Flower.Mesh = Cone;
	Flower.DensityPerSqM = 0.1f;
	Flower.bCastShadow = true;
	Flower.Salt = 23;
	Ground->Flowers.Add(Flower);
	Ground->CoverWPOFadeLength = 2000.0f;
	Ground->RebuildGroundMesh();
	Ground->SetGroundCoverViewOverride(FVector(12800.0, 12800.0, 0.0));
	FlushRenderingCommands();

	UCSGpuInstancedMeshComponent* NearGrass = CSCoverPatchTest_Find(Ground, Cube);
	UCSGpuInstancedMeshComponent* FarGrass = CSCoverPatchTest_Find(Ground, Cylinder);
	UCSGpuInstancedMeshComponent* Flowers = CSCoverPatchTest_Find(Ground, Cone);
	if (!TestNotNull(TEXT("近处草组件"), NearGrass) || !TestNotNull(TEXT("远处组合草组件"), FarGrass) || !TestNotNull(TEXT("花组件"), Flowers)) return false;

	auto Data = [](const UCSGpuInstancedMeshComponent* Component, int32 Index)
	{
		const TArray<float>& Values = Component->GetCustomPrimitiveData().Data;
		return Values.IsValidIndex(Index) ? Values[Index] : -1.0f;
	};
	const float Boundary = NearGrass->InstanceEndCullDistance;
	AddInfo(FString::Printf(TEXT("交界 %.0f cm；近处草 WPO 淡出 [%.0f, %.0f]、关闭 %d；花 [%.0f, %.0f]、关闭 %d"),
		Boundary, Data(NearGrass, ACSGroundActor::CoverWPOFadeStartIndex), Data(NearGrass, ACSGroundActor::CoverWPOFadeEndIndex), NearGrass->InstanceWPODisableDistance,
		Data(Flowers, ACSGroundActor::CoverWPOFadeStartIndex), Data(Flowers, ACSGroundActor::CoverWPOFadeEndIndex), Flowers->InstanceWPODisableDistance));

	TestTrue(TEXT("近处草：淡出终点 = 交界"), FMath::IsNearlyEqual(Data(NearGrass, ACSGroundActor::CoverWPOFadeEndIndex), Boundary, 0.5f));
	TestTrue(TEXT("近处草：淡出起点 = 交界 − CoverWPOFadeLength"), FMath::IsNearlyEqual(Data(NearGrass, ACSGroundActor::CoverWPOFadeStartIndex), Boundary - 2000.0f, 0.5f));
	TestEqual(TEXT("近处草：交界之外不求 WPO"), NearGrass->InstanceWPODisableDistance, FMath::CeilToInt32(Boundary));
	TestTrue(TEXT("花与草共用同一条交界（直至交界处彻底消失）"),
		FMath::IsNearlyEqual(Data(Flowers, ACSGroundActor::CoverWPOFadeEndIndex), Boundary, 0.5f)
		&& FMath::IsNearlyEqual(Data(Flowers, ACSGroundActor::CoverWPOFadeStartIndex), Boundary - 2000.0f, 0.5f));
	TestEqual(TEXT("花：交界之外不求 WPO"), Flowers->InstanceWPODisableDistance, FMath::CeilToInt32(Boundary));
	TestTrue(TEXT("远处组合草：材质本来没有 WPO，两个下标写 0、不关"),
		Data(FarGrass, ACSGroundActor::CoverWPOFadeEndIndex) == 0.0f && FarGrass->InstanceWPODisableDistance == 0);

	TestTrue(TEXT("投影的花开影子副本"), Flowers->bClassicShadowOnNanitePath);
	TestFalse(TEXT("不投影的草不开影子副本"), NearGrass->bClassicShadowOnNanitePath);

	FVector View;
	TestTrue(TEXT("脚本钉住的机位也推给了组件（Nanite 路的距离带按它判）"), NearGrass->GetDistanceCullViewOverride(View) && View.Equals(FVector(12800.0, 12800.0, 0.0)));
	Ground->ClearGroundCoverViewOverride();
	TestFalse(TEXT("放开钉住 ⇒ 组件也放开"), NearGrass->GetDistanceCullViewOverride(View));

	// 改淡出长度只动组件上的数（哈希里带着它 ⇒ 真会走到 EnsureCoverComponents）。
	Ground->SetGroundCoverViewOverride(FVector(12800.0, 12800.0, 0.0));
	Ground->CoverWPOFadeLength = 500.0f;
	Ground->RebuildGroundCover();
	TestTrue(TEXT("改 CoverWPOFadeLength ⇒ 淡出起点跟着变"),
		FMath::IsNearlyEqual(Data(NearGrass, ACSGroundActor::CoverWPOFadeStartIndex), Boundary - 500.0f, 0.5f));

	Ground->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSGroundShaperActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/ConstructorHelpers.h"

// -----------------------------------------------------------------------------
// 石阶 S1（marching squares）的**核心验收项**：两座相接土台的接合处不断裂。
//
// 为什么非要跑真 GPU：S1 的整个裁决就是"层 / 弧段 / 摆位全部由 GPU 从高度场推导，CPU 不接触
// 单个台阶"。CPU 侧没有任何可以单独断言的判定函数 —— 造一个 CPU 孪生来断言，等于把刚刚删掉的
// 那条 CPU 决策链又写了一遍，而且是没人会去维护的那一份。所以这里回读一次真实结果，
// 用 ACSGroundActor::DebugReadStairsSync（它存在的唯一理由就是这条验收）。
//
// 场景是**已删的旧路**（`ACSGroundShaperActor::BuildStepPlan`，裁决一第二步删除）做不到的
// 那一个：两座土台中心相距 800，各自 Radius 300 / Falloff 400，裙边彻底融成一体
// （接合线上高度 253 cm，不是 0）。路**沿接合线**画，与两座中心的连线垂直 ——
// 这条路上的每一条等值线都是合成之后的"腰"，不是关于任何一座中心的圆。
// 旧路的闭式环半径只对"关于本座中心星形"的等值线成立，在这里必然错位/断段。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSStairsTest_ 前缀
// （与 CSGroundStairs.cpp 的 CSStairs_ 也不同 —— 同名符号在 unity blob 里会打架，
//  而报错位置会指向一个跟改动无关的文件）。

constexpr double CSStairsTest_MoundY = 1600.0;
constexpr double CSStairsTest_JunctionX = 1600.0;   // 两座中心 (1200, 2000) 的中点 = 接合线
constexpr float CSStairsTest_StepHeight = 30.0f;

ACSGroundShaperActor* CSStairsTest_SpawnMound(UWorld* World, double X)
{
	ACSGroundShaperActor* Shaper = World->SpawnActor<ACSGroundShaperActor>(
		FVector(X, CSStairsTest_MoundY, 0.0), FRotator::ZeroRotator);
	if (!Shaper) return nullptr;
	Shaper->MaskSize = 2.0f * float(300.0f);   // 圆盘直径（2026-09-29 起形状是 mask，没设 mask = 解析圆盘）
	Shaper->FalloffDistance = 400.0f;
	// 台顶 = actor 高度（2026-09-29）；乘 (1 + 二次抬升) 保持与旧口径「抬 LiftHeight、台顶再照抬 2%」逐项相同的剖面。
	Shaper->SetActorLocation(Shaper->GetActorLocation() + FVector(0.0, 0.0, double(300.0f) * (1.0 + double(Shaper->SecondaryLiftScale))));
	Shaper->RebuildTerrain();
	return Shaper;
}

/** 沿直线落笔。笔刷是 3D 球，每一笔都必须自己贴地 —— 固定 Z 会让抬高的坡面整段落在球外。 */
void CSStairsTest_PaintLine(ACSGroundActor* Ground, double X0, double Y0, double X1, double Y1, int32 Steps)
{
	Ground->BeginPaintStroke();
	for (int32 Index = 0; Index <= Steps; ++Index)
	{
		const double T = double(Index) / double(Steps);
		const double X = FMath::Lerp(X0, X1, T);
		const double Y = FMath::Lerp(Y0, Y1, T);
		Ground->ApplyPaintStroke(FVector(X, Y, Ground->SampleHeight(FVector2D(X, Y))));
	}
	Ground->EndPaintStroke();   // 内部 FlushPaintToGpu(true) → RebuildStairs()
}

/**
 * `CSGroundStairs.usf` 里 `CSStairs_CellSeed` / `CSStairs_Hash01` 的 CPU 孪生。
 *
 * **这不是"把决策链再写一遍"**（那条纪律针对的是 marching squares 本身）：这里复算的只是
 * 一个 6 行的整数哈希，而它是 S2 唯一一条"必须是格身份、绝不能是槽位"的纪律的载体。
 * 不复算就只能断言"看起来没跳"，那正是本项目已经被推翻过四次的那种断言。
 * 哈希一旦在 shader 里被改动，这两份会立刻分叉、断言报红 —— 那是想要的行为。
 */
uint32 CSStairsTest_CellSeed(uint32 CellX, uint32 CellY, int32 Level, uint32 Seg, uint32 Salt, uint32 JitterSeed)
{
	uint32 H = CellX * 0x9E3779B1u;
	H ^= CellY * 0x85EBCA77u;
	H ^= uint32(Level + 1048576) * 0xC2B2AE3Du;
	H ^= Seg * 0x27D4EB2Fu;
	H ^= Salt * 0x165667B1u;
	H ^= JitterSeed * 0x9E3779B9u;
	return H;
}

float CSStairsTest_Hash01(uint32 H)
{
	H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
	return float(((H >> 22) ^ H) & 0xFFFFFFu) / float(0x1000000u);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundStairsJunctionTest,
	"PCGPlugins.ComputeShaderGenerator.GroundStairs.Junction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundStairsJunctionTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* StepMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Step base mesh"), StepMesh)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// 默认 64 × 64 格 × 50 cm = 3200 cm 见方，两座土台（reach 700）完全落在里面。
	Ground->StairMesh = StepMesh;
	Ground->StairStepHeight = CSStairsTest_StepHeight;
	Ground->StairCellSize = 100.0f;             // ≈ 石阶长度（StairBlockSize.Y）
	Ground->StairBlockSize = FVector(60.0, 100.0, 30.0);
	Ground->StairZOffset = 0.0f;
	// **本用例是 S1 的回归，抖动一律关掉**：下面「每一级都落在层网格上」那条断言的容差是
	// 0.02 层，而 S2 的 Z 抖动按设计就会把块底连同 StairRise 一起抖开（±SizeJitter × Rise）。
	// 不关掉的话这条断言测的就不再是"层高等距"，而是"抖动幅度小于容差"，两件事。
	Ground->StairLengthJitter = 0.0f;
	Ground->StairSizeJitter = 0.0f;
	Ground->StairYawJitter = 0.0f;
	Ground->RebuildGroundMesh();

	ACSGroundShaperActor* MoundA = CSStairsTest_SpawnMound(World, 1200.0);
	ACSGroundShaperActor* MoundB = CSStairsTest_SpawnMound(World, 2000.0);
	if (!TestNotNull(TEXT("Mound A"), MoundA) || !TestNotNull(TEXT("Mound B"), MoundB)) return false;

	// 两座是真的"相接"：接合线上高度既不是 0（各自独立）也不是 300（完全重合）。
	// 这个数值就是旧路星形假设失效的地方 —— 这里的等值线属于合成体，不属于任何一座。
	const double JunctionHeight = Ground->SampleHeight(FVector2D(CSStairsTest_JunctionX, CSStairsTest_MoundY));
	TestTrue(TEXT("The two mounds really merge at the junction"), JunctionHeight > 200.0 && JunctionHeight < 290.0);

	TArray<FVector> Origins;

	// ---- 没画路 ⇒ 一级都不长（与门拱同一条"道路决定开口"的语言）----
	Ground->ResetPaint();
	Ground->RebuildStairs();
	TestEqual(TEXT("No road grows no stairs"), Ground->DebugReadStairsSync(Origins), 0);

	// ---- 沿接合线画路（与两座中心的连线垂直）⇒ 长出石阶 ----
	CSStairsTest_PaintLine(Ground, CSStairsTest_JunctionX, 1000.0, CSStairsTest_JunctionX, 2200.0, 24);
	const int32 Count = Ground->DebugReadStairsSync(Origins);
	TestTrue(TEXT("A road over the junction grows stairs"), Count > 0);
	if (Count <= 0) return false;
	TestEqual(TEXT("Every counted instance came back with a row"), Origins.Num(), Count);

	// ---- 验收项 ①：石阶真的落在接合线上，不是只长在两座各自的坡上 ----
	int32 OnJunction = 0;
	int32 BelowAxis = 0;
	int32 AboveAxis = 0;
	for (const FVector& P : Origins)
	{
		if (FMath::Abs(P.X - CSStairsTest_JunctionX) <= Ground->StairCellSize) ++OnJunction;
		if (P.Y < CSStairsTest_MoundY) ++BelowAxis;
		else ++AboveAxis;
	}
	TestTrue(TEXT("Stairs land on the junction line itself"), OnJunction > 0);
	// 合成体的"腰"在中心连线两侧各穿过路一次：只出现在一侧就说明弧段在接合处断了。
	TestTrue(TEXT("The waist contour crosses the road on both sides of the mound axis"),
		BelowAxis > 0 && AboveAxis > 0);

	// ---- 验收项 ②：层是连续的，中间不许缺层 ----
	// 缺一层 = 那一层的等值线在路上断掉，正是旧路在接合处的症状。层号相对最低层算，
	// 这样断言不依赖 StairRise / StairZOffset 的具体推导（那是实现细节，不是验收内容）。
	double MinZ = TNumericLimits<double>::Max();
	for (const FVector& P : Origins) MinZ = FMath::Min(MinZ, P.Z);
	TSet<int32> Levels;
	for (const FVector& P : Origins) Levels.Add(FMath::RoundToInt32((P.Z - MinZ) / double(CSStairsTest_StepHeight)));

	int32 MaxLevel = 0;
	for (const int32 Level : Levels) MaxLevel = FMath::Max(MaxLevel, Level);
	bool bContiguous = true;
	for (int32 Level = 0; Level <= MaxLevel; ++Level) bContiguous &= Levels.Contains(Level);

	// 路从平地爬到接合线的 253 cm，30 cm 一层 ⇒ 至少 8 层。
	TestTrue(TEXT("The climb produces at least eight distinct levels"), MaxLevel >= 7);
	TestTrue(TEXT("No level is missing: the contour never breaks across the junction"), bContiguous);

	// 每一级的高度必须落在层网格上（组装 Z 用的就是 Level × StepHeight）。
	bool bOnLevelGrid = true;
	for (const FVector& P : Origins)
	{
		const double Offset = (P.Z - MinZ) / double(CSStairsTest_StepHeight);
		bOnLevelGrid &= FMath::Abs(Offset - FMath::RoundToDouble(Offset)) < 0.02;
	}
	TestTrue(TEXT("Every stair sits exactly on an equal-height level"), bOnLevelGrid);

	// ---- 幂等：同一世界状态再扫一次，结果不许变 ----
	Ground->RebuildStairs();
	TestEqual(TEXT("Rescanning the same state is idempotent"), Ground->DebugReadStairsSync(Origins), Count);

	// ---- 擦掉路 ⇒ 石阶全消 ----
	Ground->ResetPaint();
	Ground->RebuildStairs();
	TestEqual(TEXT("Erasing the road removes every stair"), Ground->DebugReadStairsSync(Origins), 0);

	World->DestroyActor(MoundA);
	World->DestroyActor(MoundB);
	World->DestroyActor(Ground);
	return true;
}

// -----------------------------------------------------------------------------
// S2 验收 ①：抖动**只由格身份决定**（GPU，需要真 RHI）。
//
// 这条是 S2 的核心纪律：随机源必须是「格坐标 + 层号 + 段号 + 种子」，**不能是 InterlockedAdd
// 拿到的槽位**。槽位由线程组完成顺序决定，同一份世界状态两次 dispatch 可以给同一块石阶不同
// 的槽；而这条路上重扫是每一 dab 一次的，用槽位的症状就是"画一笔路，整片石阶乱跳"。
//
// 断言形态：回读整份 packed 行，从原点反推出这块石阶属于哪一格哪一层，再用 CPU 孪生哈希
// **精确预测**它的三条抖动通道（PerInstanceRandom / 进深缩放 / 高度缩放）。三条 24 bit 通道
// 必须在**同一个候选**上同时对上 —— 蒙对的概率是 18 × 2⁻⁴⁸，所以这是等价于"逐位相同"的断言。
// 只断言"两次重扫结果一样"是不够的：那在槽位恰好稳定时会静默通过。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundStairsJitterTest,
	"PCGPlugins.ComputeShaderGenerator.GroundStairs.JitterIsCellDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundStairsJitterTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* StepMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Step base mesh"), StepMesh)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// actor 变换是单位阵 ⇒ 组件空间 = 世界空间，回读到的基长度就是缩放本身，不用再拆矩阵。
	const FVector BlockSize(60.0, 100.0, 45.0);
	const float CellSize = 100.0f;
	const float StepHeight = CSStairsTest_StepHeight;

	Ground->StairMesh = StepMesh;
	Ground->StairStepHeight = StepHeight;
	Ground->StairCellSize = CellSize;
	Ground->StairBlockSize = BlockSize;
	// **Embed 必须为 0**：它沿最陡方向平移原点，原点会漂进邻格，格身份就反推不出来了。
	// 这不是把被测代码改简单 —— 抖动通道与 Embed 完全无关，关掉它只是让"这块属于哪一格"可判。
	Ground->StairEmbed = 0.0f;
	Ground->StairZOffset = 0.0f;
	Ground->StairLengthBloat = 1.06f;
	Ground->StairLengthJitter = 0.10f;
	Ground->StairSizeJitter = 0.12f;
	Ground->StairYawJitter = 6.0f;
	Ground->StairJitterSeed = 7;
	Ground->RebuildGroundMesh();

	ACSGroundShaperActor* Mound = CSStairsTest_SpawnMound(World, 1600.0);
	if (!TestNotNull(TEXT("Mound"), Mound)) return false;

	CSStairsTest_PaintLine(Ground, 1600.0, 900.0, 1600.0, 2300.0, 24);

	TArray<FVector4f> Rows;
	const int32 Count = Ground->DebugReadStairRowsSync(Rows);
	TestTrue(TEXT("A road over the mound grows stairs"), Count > 0);
	if (Count <= 0) return false;
	TestEqual(TEXT("Every counted instance came back with five packed rows"), Rows.Num(), Count * 5);
	if (Rows.Num() != Count * 5) return false;

	const FBox2D Rect = Ground->GetWorldRect2D();
	const FBox LocalBox = StepMesh->GetBoundingBox();
	const FVector MeshSize = LocalBox.GetSize();
	const float ScaleX = float(BlockSize.X / MeshSize.X);
	const float ScaleY = float(BlockSize.Y / MeshSize.Y);
	const float ScaleZ = float(BlockSize.Z / MeshSize.Z);
	const float Rise = float(-LocalBox.Min.Z) * ScaleZ;
	const float NominalY = ScaleY * float(MeshSize.Y);
	const uint32 Seed = uint32(Ground->StairJitterSeed);

	int32 Matched = 0;
	int32 TreadTilted = 0;
	int32 TooShort = 0;
	int32 TooLong = 0;
	float MinLen = TNumericLimits<float>::Max();
	float MaxLen = 0.0f;
	TSet<uint32> DistinctRandom;

	// 负缝的前提：每块的长度都不短于「标称长度 × 胀大系数」（长度轴只许单侧加长）。
	const float MinAllowedLen = NominalY * Ground->StairLengthBloat - 0.05f;
	const float MaxAllowedLen = CellSize * UE_SQRT_2 * Ground->StairLengthBloat
		* (1.0f + Ground->StairLengthJitter) + 0.05f;

	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector4f RowX = Rows[Index * 5 + 0];
		const FVector4f RowY = Rows[Index * 5 + 1];
		const FVector4f RowZ = Rows[Index * 5 + 2];
		const FVector4f RowO = Rows[Index * 5 + 3];

		// 踏面必须水平：偏航绕世界 +Z 转，按构造动不了 Z 轴基。抖成斜的就说明抖错了轴。
		if (FMath::Abs(RowZ.X) > 1e-3f || FMath::Abs(RowZ.Y) > 1e-3f || RowZ.Z <= 0.0f) ++TreadTilted;

		const float LenWorld = FVector3f(RowY.X, RowY.Y, RowY.Z).Size() * float(MeshSize.Y);
		MinLen = FMath::Min(MinLen, LenWorld);
		MaxLen = FMath::Max(MaxLen, LenWorld);
		if (LenWorld < MinAllowedLen) ++TooShort;
		if (LenWorld > MaxAllowedLen) ++TooLong;

		const float GotDepth = FVector3f(RowX.X, RowX.Y, RowX.Z).Size() / ScaleX;   // 1 + SizeJitter·JDepth
		const float GotHigh = FVector3f(RowZ.X, RowZ.Y, RowZ.Z).Size() / ScaleZ;    // 1 + SizeJitter·JHigh
		const float GotRandom = RowO.W;
		DistinctRandom.Add(uint32(GotRandom * 16777216.0f));

		// 反推格身份：Embed = 0 ⇒ 原点 XY 就是弦的中点，必落在自己那一格内；
		// 层号用未抖的 Rise 反推，残差只有 ±SizeJitter × Rise / StepHeight ≈ 0.09 层。
		const int32 Level = FMath::RoundToInt32((RowO.Z - Rise) / StepHeight);
		const int32 BaseCellX = FMath::FloorToInt32((RowO.X - float(Rect.Min.X)) / CellSize);
		const int32 BaseCellY = FMath::FloorToInt32((RowO.Y - float(Rect.Min.Y)) / CellSize);

		bool bFound = false;
		for (int32 Dy = -1; Dy <= 1 && !bFound; ++Dy)
		for (int32 Dx = -1; Dx <= 1 && !bFound; ++Dx)
		for (uint32 Seg = 0u; Seg < 2u && !bFound; ++Seg)
		{
			const uint32 CellX = uint32(BaseCellX + Dx);
			const uint32 CellY = uint32(BaseCellY + Dy);
			const float WantDepth = 1.0f + Ground->StairSizeJitter *
				(CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 23u, Seed)) * 2.0f - 1.0f);
			const float WantHigh = 1.0f + Ground->StairSizeJitter *
				(CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 37u, Seed)) * 2.0f - 1.0f);
			const float WantRandom = CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 71u, Seed));
			// 三条独立通道必须在**同一个**候选上同时对上。
			bFound = FMath::IsNearlyEqual(GotDepth, WantDepth, 1e-4f)
				&& FMath::IsNearlyEqual(GotHigh, WantHigh, 1e-4f)
				&& FMath::IsNearlyEqual(GotRandom, WantRandom, 1e-5f);
		}
		if (bFound) ++Matched;
	}

	TestEqual(TEXT("Every instance's jitter is exactly reproducible from its cell identity alone"), Matched, Count);
	TestEqual(TEXT("Yaw jitter never tilts the tread out of horizontal"), TreadTilted, 0);
	TestEqual(TEXT("No stone is shorter than nominal length times the bloat (the negative-seam premise)"), TooShort, 0);
	TestEqual(TEXT("No stone exceeds the worst-case length the conservative bounds were sized for"), TooLong, 0);
	// 抖动真的在动 —— 全部相等的话上面几条会全绿而画面毫无变化。
	TestTrue(TEXT("Per-instance random actually varies across instances"), DistinctRandom.Num() > Count / 4);
	TestTrue(TEXT("Stone length actually varies (it follows the chord, not a constant)"),
		MaxLen - MinLen > 1.0f);

	AddInfo(FString::Printf(TEXT("stairs=%d matched=%d len=[%.1f, %.1f] cm (nominal=%.1f)"),
		Count, Matched, MinLen, MaxLen, NominalY));

	World->DestroyActor(Mound);
	World->DestroyActor(Ground);
	return true;
}

// -----------------------------------------------------------------------------
// S2 验收 ②：**负缝**（纯 CPU，读 CDO 出厂值 —— 与 House.FrameBrickOverlap 同一形态）。
//
// 石阶与门框砖在这里的差别是本轮最要紧的一条结论：
//   · 门框砖的槽距由铺装排布（`CSHouseFrame::SolveRun`）定成近恒定 ⇒ 一个常数胀大系数就保证负缝。
//   · 石阶的槽距是**几何**决定的：marching squares 每格每层出一级，弦长在 [格距, √2×格距]
//     上变（等值线与格成 45° 时最长）。**定长块（S1）在斜走的等值线上必然露正缝**，
//     而且缝宽随走向变化 41%，没有任何常数系数盖得住。
// 所以负缝的做法必须换：长度跟着弦长走（TG 实证），常数胀大只负责把"恰好首尾相接"推成
// "确定互相穿插"。对照组直接把 S1 的定长块算一遍，量出它露多少缝 —— 谁把长度改回定长，
// 这条断言立刻红。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundStairsOverlapTest,
	"PCGPlugins.ComputeShaderGenerator.GroundStairs.StepOverlap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundStairsOverlapTest::RunTest(const FString& Parameters)
{
	const ACSGroundActor* CDO = GetDefault<ACSGroundActor>();
	if (!TestNotNull(TEXT("Ground actor CDO"), CDO)) return false;

	const double Cell = CDO->StairCellSize;
	const double Nominal = CDO->StairBlockSize.Y;
	const double Bloat = CDO->StairLengthBloat;
	const double LenJitter = CDO->StairLengthJitter;
	const double YawRad = FMath::DegreesToRadians(double(CDO->StairYawJitter));

	TestTrue(TEXT("The bloat is a genuine bloat (a factor below 1 is exactly the positive seam it removes)"),
		Bloat >= 1.0);
	TestTrue(TEXT("Length jitter is one-sided: a symmetric one would cancel the negative seam"),
		LenJitter >= 0.0);

	// 相邻两块共用同一个穿越点 X（同一条格边，两格各解一次）。段 A→X 的块以中点为心、
	// 长度 L 时，末端沿自身切向越过 X 的距离是 (L·cos δ − c) / 2 —— **与拐弯角度无关**，
	// 拐弯只改变两块各自的切向，不改变各自越过 X 多少。所以逐块判即可。
	auto EndOverlapCm = [](double Chord, double Length, double Yaw) -> double
	{
		return 0.5 * (Length * FMath::Cos(Yaw) - Chord);
	};

	double WorstNow = TNumericLimits<double>::Max();
	// 弦长扫 [0, √2 × 格距]（marching squares 的全域：角上一刀切出来的短弦到整条对角线）。
	for (int32 I = 0; I <= 400; ++I)
	{
		const double Chord = (double(I) / 400.0) * Cell * UE_DOUBLE_SQRT_2;
		for (const double H : { 0.0, 0.5, 1.0 })                  // 单侧抖动的全域
		{
			for (const double Yaw : { -YawRad, 0.0, YawRad })     // 偏航的全域（cos 是偶函数，两端等价）
			{
				const double Length = FMath::Max(Chord, Nominal) * Bloat * (1.0 + LenJitter * H);
				WorstNow = FMath::Min(WorstNow, EndOverlapCm(Chord, Length, Yaw));
			}
		}
	}
	TestTrue(TEXT("Every stone overlaps its neighbour at every chord length, jitter and yaw"),
		WorstNow > 0.0);

	// 闭式充要条件（上面那趟扫描的解析形式）：Bloat × cos(YawMax) > 1。
	// 单独写出来是因为它是**可调坏的两个参数之间的关系** —— 调大偏航或调小胀大就会破。
	TestTrue(TEXT("Bloat covers the projection lost to yaw: bloat * cos(yaw) > 1"),
		Bloat * FMath::Cos(YawRad) > 1.0);

	// 对照组 = S1 的定长块（长度恒为标称值，不跟弦长）。斜走的等值线上必然露缝。
	const double DiagonalChord = Cell * UE_DOUBLE_SQRT_2;
	const double S1Gap = -2.0 * EndOverlapCm(DiagonalChord, Nominal, 0.0);
	TestTrue(TEXT("Control group: S1's constant-length block opens a real gap on a 45-degree contour"),
		S1Gap > 0.0);

	// Z 抖动不许把"相邻两级竖直重叠"这条不变量抖没（块底钉在等值线上，踏面前缘一定悬空，
	// 靠下一级挡住 —— Z 比层高小就露馅，而且一个断言都不会红）。
	const double MinBlockZ = CDO->StairBlockSize.Z * (1.0 - CDO->StairSizeJitter);
	TestTrue(TEXT("Even the shortest jittered stone still overlaps the next level vertically"),
		MinBlockZ > CDO->StairStepHeight);

	AddInfo(FString::Printf(
		TEXT("worst overlap per joint end = %.2f cm; S1 constant-length control gap on a 45deg contour = %.2f cm; ")
		TEXT("bloat*cos(yaw) = %.4f; min jittered block Z = %.1f vs step %.1f cm"),
		WorstNow, S1Gap, Bloat * FMath::Cos(YawRad), MinBlockZ, CDO->StairStepHeight));
	return true;
}

// -----------------------------------------------------------------------------
// 小石子（TG 的 15% 支线，`_rocky_terrain_stairs_stairs.cs:511-547`）。
//
// **本用例真正守的是随机源**，不是"有没有石子"。石子的抽签、大小、落点如果拿 InterlockedAdd
// 的槽位当种子，画一笔路石子就会整片重掷 —— 而那件事**不会让任何数量类断言变红**
// （数量分布一模一样），S1 就是这么在 `.w` 上错了一整轮。所以判据必须落在
// "能不能从格身份精确复算出来"上：用 CPU 孪生哈希预测每颗石子的**缩放**与 **PerInstanceRandom**，
// 两条独立的 24 bit 通道必须在**同一个候选**（格 × 层 × 段）上同时对上。
//
// 顺带守两条便宜但会静默坏掉的：概率 0 时必须真的一颗都不出（counter 有没有被清），
// 以及石子必须**站在地面上**（照抄石阶的 Level 会让它浮在弦的弧垂上方几厘米）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundStairsPebbleTest,
	"PCGPlugins.ComputeShaderGenerator.GroundStairs.PebblesAreCellDeterministic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundStairsPebbleTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* StepMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* PebbleMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (!TestNotNull(TEXT("Step base mesh"), StepMesh)) return false;
	if (!TestNotNull(TEXT("Pebble base mesh"), PebbleMesh)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	const float CellSize = 100.0f;
	const double PebbleLo = 27.0;
	const double PebbleHi = 54.0;

	Ground->StairMesh = StepMesh;
	Ground->StairStepHeight = CSStairsTest_StepHeight;
	Ground->StairCellSize = CellSize;
	Ground->StairBlockSize = FVector(60.0, 100.0, 45.0);
	// **Embed 必须为 0**（同 JitterIsCellDeterministic 那条）：石阶的 embed 把原点沿最陡方向
	// 平移，原点会漂进邻格。石子本来就不吃 embed，但下面要拿石阶的原点反推格身份。
	Ground->StairEmbed = 0.0f;
	Ground->StairZOffset = 0.0f;
	Ground->StairJitterSeed = 5;
	Ground->StairPebbleMesh = PebbleMesh;
	Ground->StairPebbleChance = 0.15f;
	Ground->StairPebbleSize = FVector2D(PebbleLo, PebbleHi);
	Ground->RebuildGroundMesh();

	ACSGroundShaperActor* Mound = CSStairsTest_SpawnMound(World, 1600.0);
	if (!TestNotNull(TEXT("Mound"), Mound)) return false;
	// ⚠️ **裙边噪声必须关掉，但不是为了让被测代码变简单**：下面"石子站在地面上"那条要拿
	// `SampleHeight`（镜像在 **50 cm 格**上的双线性重建）当尺子去量 kernel 用解析场算出来的 Z，
	// 而默认噪声的波长是 300 cm —— 50 cm 格重建一条 300 cm 的波，格中间的下垂量本身就有
	// ~20 cm 量级，比这条断言要抓的错还大，尺子先坏了。
	// （解析场与地面网格在噪声下确实对不齐，这是全项目已知且已记录的性质：石阶同样读解析场。）
	Mound->SkirtNoiseAmount = 0.0f;
	Mound->RebuildTerrain();

	CSStairsTest_PaintLine(Ground, 1600.0, 900.0, 1600.0, 2300.0, 24);

	TArray<FVector> StairOrigins;
	TArray<FVector4f> PebbleRows;
	const int32 StairCount = Ground->DebugReadStairsSync(StairOrigins);
	const int32 PebbleCount = Ground->DebugReadStairPebbleRowsSync(PebbleRows);
	TestTrue(TEXT("A road over the mound grows stairs"), StairCount > 0);
	if (StairCount <= 0) return false;
	TestTrue(TEXT("The 15% branch actually produces pebbles"), PebbleCount > 0);
	TestEqual(TEXT("Every counted pebble came back with five packed rows"), PebbleRows.Num(), PebbleCount * 5);
	if (PebbleCount <= 0 || PebbleRows.Num() != PebbleCount * 5) return false;

	// 二项分布 N = StairCount、p = 0.15。带宽取 [5%, 30%] —— 只用来抓"门开反了 / 概率没接上"
	// 这类整数量级的错，不是在测随机数质量（那是下面复算哈希那条的事）。
	const double Ratio = double(PebbleCount) / double(StairCount);
	TestTrue(TEXT("The pebble rate is in the neighbourhood of TG's 15%"), Ratio > 0.05 && Ratio < 0.30);

	const FBox2D Rect = Ground->GetWorldRect2D();
	const FBox PebbleLocal = PebbleMesh->GetBoundingBox();
	const FVector PebbleMeshSize = PebbleLocal.GetSize();
	const double PebbleLongest = FMath::Max3(PebbleMeshSize.X, PebbleMeshSize.Y, PebbleMeshSize.Z);
	const float ScaleMin = float(PebbleLo / PebbleLongest);
	const float ScaleMax = float(PebbleHi / PebbleLongest);
	const uint32 Seed = uint32(Ground->StairJitterSeed);
	const float GroundZ = float(Ground->GetActorLocation().Z);

	int32 Matched = 0;
	int32 OffGround = 0;
	int32 OutOfSizeBand = 0;
	float WorstAboveGround = 0.0f;
	float MinWorldSize = TNumericLimits<float>::Max();
	float MaxWorldSize = 0.0f;

	for (int32 Index = 0; Index < PebbleCount; ++Index)
	{
		const FVector4f RowX = PebbleRows[Index * 5 + 0];
		const FVector4f RowO = PebbleRows[Index * 5 + 3];

		// 均匀缩放 ⇒ 任一基向量的长度就是缩放本身（actor 变换是单位阵，组件空间 = 世界空间）。
		const float GotScale = FVector3f(RowX.X, RowX.Y, RowX.Z).Size();
		const float WorldSize = GotScale * float(PebbleLongest);
		MinWorldSize = FMath::Min(MinWorldSize, WorldSize);
		MaxWorldSize = FMath::Max(MaxWorldSize, WorldSize);
		if (WorldSize < PebbleLo - 0.05 || WorldSize > PebbleHi + 0.05) ++OutOfSizeBand;

		// **站在地面上**：石子的 Z 是在落点重新求了一次高度场得来的，所以它必须与
		// SampleHeight 在同一点上吻合。照抄石阶的 `Level` 会让它浮在弦的弧垂之上。
		const double Surface = Ground->SampleHeight(FVector2D(RowO.X, RowO.Y));   // 已含 actor 的世界 Z
		const float Above = float(FMath::Abs(double(RowO.Z) - Surface));
		WorstAboveGround = FMath::Max(WorstAboveGround, Above);
		// 容差取**半层**：判据是"石子离地面比离任何一个层平面都近"。写成半层而不是一个 cm 数，
		// 是因为这条断言要抓的就是"照抄了石阶的 Level / 顺手加了 StairRise"这一类整层量级的错，
		// 而镜像双线性重建与解析场之间那几厘米的固有差是它容得下的。
		if (Above > 0.5f * CSStairsTest_StepHeight) ++OffGround;

		// 反推格身份：落点是弦上的一点，弦的两端在本格的边上 ⇒ 落点必在本格闭区间内。
		// 层号从地表高度取整（弦的弧垂远小于半层），前后各扩一层兜住边界。
		const int32 BaseCellX = FMath::FloorToInt32((RowO.X - float(Rect.Min.X)) / CellSize);
		const int32 BaseCellY = FMath::FloorToInt32((RowO.Y - float(Rect.Min.Y)) / CellSize);
		const int32 BaseLevel = FMath::RoundToInt32((double(RowO.Z) - GroundZ) / double(CSStairsTest_StepHeight));

		bool bFound = false;
		for (int32 DL = -1; DL <= 1 && !bFound; ++DL)
		for (int32 Dy = -1; Dy <= 1 && !bFound; ++Dy)
		for (int32 Dx = -1; Dx <= 1 && !bFound; ++Dx)
		for (uint32 Seg = 0u; Seg < 2u && !bFound; ++Seg)
		{
			const uint32 CellX = uint32(BaseCellX + Dx);
			const uint32 CellY = uint32(BaseCellY + Dy);
			const int32 Level = BaseLevel + DL;
			const float WantScale = FMath::Lerp(ScaleMin, ScaleMax,
				CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 101u, Seed)));
			const float WantRandom = CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 103u, Seed));
			// 抽签那一支也必须成立：这个候选如果压根抽不中，它就不是这颗石子的出处。
			const bool bGatePasses =
				CSStairsTest_Hash01(CSStairsTest_CellSeed(CellX, CellY, Level, Seg, 89u, Seed)) < Ground->StairPebbleChance;
			bFound = bGatePasses
				&& FMath::IsNearlyEqual(GotScale, WantScale, 1e-5f)
				&& FMath::IsNearlyEqual(RowO.W, WantRandom, 1e-5f);
		}
		if (bFound) ++Matched;
	}

	TestEqual(TEXT("Every pebble is exactly reproducible from its cell identity alone (not the slot)"),
		Matched, PebbleCount);
	TestEqual(TEXT("Every pebble sits on the ground, not on the stair's level plane"), OffGround, 0);
	TestEqual(TEXT("Every pebble's world size stays inside StairPebbleSize"), OutOfSizeBand, 0);
	// 尺寸真的在变 —— 全部相等的话上面几条会全绿而画面上是一地同样大的球。
	TestTrue(TEXT("Pebble size actually varies across instances"), MaxWorldSize - MinWorldSize > 1.0f);

	// ---- 重扫幂等：同一份世界状态，石子逐位相同 ----
	Ground->RebuildStairs();
	TArray<FVector4f> Again;
	TestEqual(TEXT("Rescanning reproduces the same pebbles"), Ground->DebugReadStairPebbleRowsSync(Again), PebbleCount);

	// ---- 概率 0 ⇒ 一颗都不出（counter 每趟都被清零，这是石子唯一的"关掉"路径）----
	Ground->StairPebbleChance = 0.0f;
	Ground->RebuildStairs();
	TArray<FVector> None;
	TestEqual(TEXT("Chance 0 really clears every pebble"), Ground->DebugReadStairPebblesSync(None), 0);
	// 石阶不受影响 —— 石子是支线，关掉它不该动主线一级台阶。
	TestEqual(TEXT("Turning the pebbles off leaves the stairs untouched"),
		Ground->DebugReadStairsSync(StairOrigins), StairCount);

	AddInfo(FString::Printf(TEXT("stairs=%d pebbles=%d (%.1f%%) matched=%d size=[%.1f, %.1f] cm worstAboveGround=%.2f cm"),
		StairCount, PebbleCount, Ratio * 100.0, Matched, MinWorldSize, MaxWorldSize, WorstAboveGround));

	World->DestroyActor(Mound);
	World->DestroyActor(Ground);
	return true;
}

#endif

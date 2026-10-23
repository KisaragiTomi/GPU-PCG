#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSGroundShaperActor.h"
#include "CSGroundShaperField.h"
#include "CSMesh.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 链 A 的两处补齐（裙边噪声 + 二次抬升）的验收。
//
// 三条用例分别守三件事：
//   ① SkirtNoiseInvariants —— 规格里"必须保住"的两条性质（只减不加、权重 1−S），
//      外加二次抬升的幅度上限。纯 CPU。
//   ② CreaseIsBroken —— 计划裁决六派给噪声的活：两座相接塑形物的折痕被打碎。
//      判据是量化的（折痕线的横向游走 + 横向曲率的变异系数），不是肉眼。纯 CPU。
//   ③ CpuGpuFieldParity —— CPU 孪生与 GPU 逐顶点同数。需要真 RHI。
//
// 为什么 ① ② 可以纯 CPU：被测的是**公式**，而公式的权威副本就是
// Public/CSGroundShaperField.h ↔ CSGroundShaperField.ush 那一对；③ 负责钉住这一对不分叉。
// 有了 ③ 之后 ① ② 在 CPU 上断言就等于在两侧都断言了；反过来把 ① ② 也搬上 GPU 只会让它们
// 依赖真 RHI，覆盖面一个字都不增加。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSShaperFieldTest_ 前缀
// （同名符号在 unity blob 里会打架，而报错位置会指向一个跟改动无关的文件）。

constexpr double CSShaperFieldTest_Radius = 300.0;
constexpr double CSShaperFieldTest_Falloff = 400.0;
constexpr double CSShaperFieldTest_Lift = 300.0;

ACSGroundShaperActor* CSShaperFieldTest_SpawnMound(UWorld* World, double X, double Y)
{
	ACSGroundShaperActor* Shaper = World->SpawnActor<ACSGroundShaperActor>(FVector(X, Y, 0.0), FRotator::ZeroRotator);
	if (!Shaper) return nullptr;
	Shaper->MaskSize = 2.0f * float(CSShaperFieldTest_Radius);   // 圆盘直径（2026-09-29 起形状是 mask，没设 mask = 解析圆盘）
	Shaper->FalloffDistance = float(CSShaperFieldTest_Falloff);
	// 台顶 = actor 高度（2026-09-29）；乘 (1 + 二次抬升) 保持与旧口径「抬 LiftHeight、台顶再照抬 2%」逐项相同的剖面。
	Shaper->SetActorLocation(Shaper->GetActorLocation() + FVector(0.0, 0.0, CSShaperFieldTest_Lift * (1.0 + double(Shaper->SecondaryLiftScale))));
	return Shaper;
}

/** 关掉噪声（保留二次抬升）后的同一座 —— "只减不加"那条断言的参照。 */
double CSShaperFieldTest_NoNoise(ACSGroundShaperActor* Shaper, const FVector2D& P)
{
	const float Saved = Shaper->SkirtNoiseAmount;
	Shaper->SkirtNoiseAmount = 0.0f;
	const double H = double(Shaper->SampleShapeHeight(P));
	Shaper->SkirtNoiseAmount = Saved;
	return H;
}
}

// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperSkirtNoiseTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.SkirtNoiseInvariants",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperSkirtNoiseTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundShaperActor* Mound = CSShaperFieldTest_SpawnMound(World, 1600.0, 1600.0);
	if (!TestNotNull(TEXT("Mound"), Mound)) return false;

	// 默认必须是**开着**的：折痕那条活是它承担的，默认关掉等于这条改动没做。
	TestTrue(TEXT("Skirt noise is on by default"), Mound->SkirtNoiseAmount > 0.0f);

	const FVector2D Centre(1600.0, 1600.0);
	const double Top = CSShaperFieldTest_Lift;

	// ---- 台顶完全不受扰：权重是 (1−S)，盘内 S ≡ 1 ⇒ 侵蚀项恒为 0（逐位相等，不是"接近"）----
	TestEqual(TEXT("The plateau is bit-for-bit untouched by the noise"),
		Mound->SampleShapeHeight(Centre), float(CSShaperFieldTest_NoNoise(Mound, Centre)));
	TestEqual(TEXT("The plateau is untouched anywhere inside the disc"),
		Mound->SampleShapeHeight(Centre + FVector2D(250.0, 0.0)),
		float(CSShaperFieldTest_NoNoise(Mound, Centre + FVector2D(250.0, 0.0))));

	// ---- 二次抬升：台顶恰好被抬 SecondaryLiftScale 一档（原型 attribwrangle5 就是照抬台顶的）----
	TestEqual(TEXT("The secondary lift raises the plateau by exactly its scale"),
		double(Mound->SampleShapeHeight(Centre)), Top * (1.0 + double(Mound->SecondaryLiftScale)), 0.02);

	// ---- 只减不加 + 权重 (1−S)：整片裙边扫一遍 ----
	int32 Samples = 0;
	int32 Raised = 0;
	double DeepestErosion = 0.0;
	double InnerErosion = 0.0, OuterErosion = 0.0;
	int32 InnerCount = 0, OuterCount = 0;
	for (int32 Iy = 0; Iy <= 160; ++Iy)
	{
		for (int32 Ix = 0; Ix <= 160; ++Ix)
		{
			const FVector2D P(Centre.X - 720.0 + Ix * 9.0, Centre.Y - 720.0 + Iy * 9.0);
			const double R = FVector2D::Distance(P, Centre);
			if (R <= CSShaperFieldTest_Radius || R >= CSShaperFieldTest_Radius + CSShaperFieldTest_Falloff) continue;

			const double Off = CSShaperFieldTest_NoNoise(Mound, P);
			const double On = double(Mound->SampleShapeHeight(P));
			++Samples;
			if (On > Off + 1.0e-3) ++Raised;                    // 一次都不许发生
			DeepestErosion = FMath::Max(DeepestErosion, Off - On);

			// 裙边内半（S 高）vs 外半（S 低）：权重 (1−S) 必须让外半被啃得更狠。
			if (R < CSShaperFieldTest_Radius + 0.5 * CSShaperFieldTest_Falloff) { InnerErosion += Off - On; ++InnerCount; }
			else { OuterErosion += Off - On; ++OuterCount; }
		}
	}
	TestTrue(TEXT("The skirt sweep actually covered something"), Samples > 5000 && InnerCount > 0 && OuterCount > 0);
	TestEqual(TEXT("The noise never raises the ground: it only erodes (abs, then subtract)"), Raised, 0);
	TestTrue(*FString::Printf(TEXT("The noise really bites: deepest erosion %.1f cm is tens of centimetres"), DeepestErosion),
		DeepestErosion > 20.0);

	const double InnerMean = InnerErosion / InnerCount;
	const double OuterMean = OuterErosion / OuterCount;
	TestTrue(*FString::Printf(TEXT("The (1-S) weight bites the outer skirt harder than the inner: inner=%.2f cm outer=%.2f cm ratio=%.2f"),
			InnerMean, OuterMean, OuterMean / FMath::Max(InnerMean, 1.0e-6)),
		OuterMean > 2.0 * InnerMean);

	// ---- 关掉噪声 ⇒ 逐位回到纯 smoothstep 剖面（这条改动必须是可关的）----
	// ⚠️ 这里原先还有两条 `HasAnalyticProfile()` 断言，随裁决一第二步一起删了：那个谓词只
	// 服务于旧路 `BuildStepPlan` 的闭式环半径（噪声一开闭式解失效 ⇒ 退回数值求交），旧路删掉
	// 之后它连一个消费者都没有。剖面本身"关掉噪声就逐位回到纯 smoothstep"这条不变量由下面
	// 两条数值断言守着，比一个布尔谓词强。
	// 台顶 = actor 高度（2026-09-29）：关掉二次抬升后台顶仍是 actor 高度，剖面是纯 smoothstep。
	Mound->SkirtNoiseAmount = 0.0f;
	Mound->SecondaryLiftScale = 0.0f;
	const double ActorTop = Mound->ComputeTopHeight();
	TestEqual(TEXT("Turning both off restores the plain smoothstep plateau"),
		double(Mound->SampleShapeHeight(Centre)), ActorTop, 1.0e-2);
	const FVector2D SkirtMid = Centre + FVector2D(CSShaperFieldTest_Radius + 0.5 * CSShaperFieldTest_Falloff, 0.0);
	TestEqual(TEXT("Turning both off restores the plain smoothstep skirt midpoint"),
		double(Mound->SampleShapeHeight(SkirtMid)), 0.5 * ActorTop, 1.0e-2);

	World->DestroyActor(Mound);
	return true;
}

// -----------------------------------------------------------------------------
// 计划裁决六：噪声加在 max **之前**，两座相接塑形物的折痕才会被各自的噪声打碎。
//
// 判据必须是量化的。这里用两个互补的量，都沿折痕线取 51 个采样：
//   A) **折痕线的横向游走**：折痕就是 argmax 的切换处，即 hA(x) − hB(x) 的零点。
//      噪声关时它是一条**精确**的中垂线（两座的贡献严格对称，偏差逐点为 0）；
//      噪声开时两侧拿到两份不同的噪声实现（噪声域是各自的局部坐标），零点因此左右游走。
//      这个量信噪比极高 —— 关时恒等于 0，不是"比较小"。
//   B) **横向曲率沿折痕的变异系数**：K(y) = h(x₀−d) − 2h(x₀) + h(x₀+d)，d = 25 cm。
//      噪声关时 K(y) 沿整条折痕几乎是常数（一条干净、笔直、截面处处相同的几何脊）；
//      噪声开时 K(y) 忽大忽小、局部甚至反号 —— "折痕不再是一条连续的干净线"就是这个。
//
// ⚠️ **中心距选 1200，不是石阶用例的 800。** 权重 (1−S) 是把噪声按剖面高度关小的：
// 两座重叠到"接合处已经爬到 S≈0.84"（相距 800）时，噪声在折痕处只剩 16% 的权重，
// 实测折痕纹丝不动（横向游走 σ 只有 4 cm，曲率均值 43.7 → 39.3）。相距 1200 时接合处
// S≈0.16，噪声几乎满权重。这是原型公式**自带**的性质，不是本实现的取舍 —— 记在这里
// 是因为它决定了这条验收该选什么场景，也划出了"深度重叠的两座折痕仍然干净"这个已知边界。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperCreaseTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.CreaseIsBroken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundShaperCreaseTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	constexpr double Separation = 1200.0;
	constexpr double MidX = 1600.0;
	constexpr double AxisY = 1600.0;
	constexpr double Band = 200.0;        // 折痕上下各取这么长；再远两座的贡献都掉到裙边尽头了
	constexpr int32 SampleCount = 51;
	constexpr double Probe = 25.0;        // 横向二阶差分的步长

	ACSGroundShaperActor* MoundA = CSShaperFieldTest_SpawnMound(World, MidX - 0.5 * Separation, AxisY);
	ACSGroundShaperActor* MoundB = CSShaperFieldTest_SpawnMound(World, MidX + 0.5 * Separation, AxisY);
	if (!TestNotNull(TEXT("Mound A"), MoundA) || !TestNotNull(TEXT("Mound B"), MoundB)) return false;

	auto Composite = [MoundA, MoundB](double X, double Y)
	{
		const FVector2D P(X, Y);
		return FMath::Max(double(MoundA->SampleShapeHeight(P)), double(MoundB->SampleShapeHeight(P)));
	};
	// 折痕的横向位置：hA − hB 的零点。两座都是单峰的，所以在 [MidX−Reach, MidX+Reach] 上唯一。
	auto CreaseOffset = [MoundA, MoundB](double Y, bool& bFound)
	{
		const double Reach = CSShaperFieldTest_Radius + CSShaperFieldTest_Falloff;
		auto Diff = [MoundA, MoundB, Y](double X)
		{
			const FVector2D P(X, Y);
			return double(MoundA->SampleShapeHeight(P)) - double(MoundB->SampleShapeHeight(P));
		};
		double Lo = MidX - Reach, Hi = MidX + Reach;
		const double DLo = Diff(Lo);
		bFound = DLo * Diff(Hi) <= 0.0;
		if (!bFound) return 0.0;
		for (int32 Iter = 0; Iter < 60; ++Iter)
		{
			const double Mid = 0.5 * (Lo + Hi);
			if ((Diff(Mid) > 0.0) == (DLo > 0.0)) Lo = Mid; else Hi = Mid;
		}
		return 0.5 * (Lo + Hi) - MidX;
	};

	// 两座是真的"相接"，而且接合处落在**裙边**（S 低）—— 噪声在这里才有权重。
	const double JunctionHeight = Composite(MidX, AxisY);
	TestTrue(*FString::Printf(TEXT("The two mounds merge, and they merge down in the skirt: h=%.1f cm"), JunctionHeight),
		JunctionHeight > 20.0 && JunctionHeight < 0.35 * CSShaperFieldTest_Lift);

	double OffMaxWander = 0.0, OnWanderSum = 0.0, OnWanderSumSq = 0.0, OnMaxWander = 0.0;
	double OffKSum = 0.0, OffKSumSq = 0.0, OnKSum = 0.0, OnKSumSq = 0.0;
	double OffKMin = TNumericLimits<double>::Max();
	int32 OnBelowHalf = 0;
	int32 Found = 0;

	TArray<double> OffK;
	OffK.Reserve(SampleCount);

	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bNoiseOn = (Pass == 1);
		MoundA->SkirtNoiseAmount = bNoiseOn ? 0.5f : 0.0f;
		MoundB->SkirtNoiseAmount = bNoiseOn ? 0.5f : 0.0f;

		for (int32 Index = 0; Index < SampleCount; ++Index)
		{
			const double Y = AxisY - Band + (2.0 * Band * Index) / (SampleCount - 1);
			const double K = Composite(MidX - Probe, Y) - 2.0 * Composite(MidX, Y) + Composite(MidX + Probe, Y);
			bool bFound = false;
			const double Offset = CreaseOffset(Y, bFound);

			if (!bNoiseOn)
			{
				OffMaxWander = FMath::Max(OffMaxWander, FMath::Abs(Offset));
				OffKSum += K;
				OffKSumSq += K * K;
				OffKMin = FMath::Min(OffKMin, K);
				OffK.Add(K);
			}
			else
			{
				if (bFound) ++Found;
				OnWanderSum += Offset;
				OnWanderSumSq += Offset * Offset;
				OnMaxWander = FMath::Max(OnMaxWander, FMath::Abs(Offset));
				OnKSum += K;
				OnKSumSq += K * K;
				if (OffK.IsValidIndex(Index) && K < 0.5 * OffK[Index]) ++OnBelowHalf;
			}
		}
	}

	const double N = double(SampleCount);
	const double OffKMean = OffKSum / N;
	const double OffKStd = FMath::Sqrt(FMath::Max(OffKSumSq / N - OffKMean * OffKMean, 0.0));
	const double OnKMean = OnKSum / N;
	const double OnKStd = FMath::Sqrt(FMath::Max(OnKSumSq / N - OnKMean * OnKMean, 0.0));
	const double OnWanderMean = OnWanderSum / N;
	const double OnWanderStd = FMath::Sqrt(FMath::Max(OnWanderSumSq / N - OnWanderMean * OnWanderMean, 0.0));
	const double OffCv = OffKStd / FMath::Max(OffKMean, 1.0e-6);
	const double OnCv = OnKStd / FMath::Max(OnKMean, 1.0e-6);

	TestEqual(TEXT("Every sample found the crease"), Found, SampleCount);

	// ---- 参照系先立住：噪声关时折痕确实是一条干净的直脊 ----
	TestTrue(*FString::Printf(TEXT("Noise off: the crease is exactly the perpendicular bisector (maxWander=%.4f cm)"), OffMaxWander),
		OffMaxWander < 0.05);
	TestTrue(*FString::Printf(TEXT("Noise off: the crease is a sharp kink, not a smooth valley (Kmean=%.2f Kmin=%.2f cm)"), OffKMean, OffKMin),
		OffKMean > 20.0 && OffKMin > 10.0);
	TestTrue(*FString::Printf(TEXT("Noise off: that ridge has the same cross-section all along its length (cv=%.3f)"), OffCv),
		OffCv < 0.20);

	// ---- 验收本体 ----
	TestTrue(*FString::Printf(TEXT("Noise on: the crease line wanders off the bisector (std=%.2f cm max=%.2f cm; off max=%.4f cm)"),
			OnWanderStd, OnMaxWander, OffMaxWander),
		OnWanderStd > 6.0 && OnMaxWander > 20.0);
	TestTrue(*FString::Printf(TEXT("Noise on: the crease stops being a uniform ridge (cvOff=%.3f cvOn=%.3f)"), OffCv, OnCv),
		OnCv > 2.0 * OffCv);
	TestTrue(*FString::Printf(TEXT("Noise on: the average crease curvature is pressed down (Koff=%.2f Kon=%.2f ratio=%.2f)"),
			OffKMean, OnKMean, OnKMean / OffKMean),
		OnKMean < 0.85 * OffKMean);
	TestTrue(*FString::Printf(TEXT("Noise on: a good part of the crease loses its kink outright (belowHalf=%d/%d)"), OnBelowHalf, SampleCount),
		OnBelowHalf >= SampleCount / 8);

	AddInfo(FString::Printf(
		TEXT("crease metrics: wander std 0.00 -> %.2f cm (max %.4f -> %.2f); K mean %.2f -> %.2f cm; K cv %.3f -> %.3f; belowHalf %d/%d"),
		OnWanderStd, OffMaxWander, OnMaxWander, OffKMean, OnKMean, OffCv, OnCv, OnBelowHalf, SampleCount));

	World->DestroyActor(MoundA);
	World->DestroyActor(MoundB);
	return true;
}

// -----------------------------------------------------------------------------
// CPU 孪生 ↔ GPU：同一张地面上逐顶点比高度。
//
// 这条断言的全部意义是"别让 Public/CSGroundShaperField.h 与 CSGroundShaperField.ush 漂开"。
// 分叉不报任何错：石阶浮在坡面上方几厘米、房子陷进土台几厘米，只在裙边中段看得出来，
// 而且两条路各自都"自洽"。噪声进来之后这条风险陡增 —— 哈希、value noise 的插值、
// fbm 的求值顺序，任何一处写法不同都会给出不同的数。
//
// ⚠️ 必须先确认这一趟真的走了 GPU 位移 pass：`RefreshHeightsInRegion` 在"网格还没建 /
// actor 被拖过"时会退回 `RebuildGroundMesh()`，那条是拿 CPU 镜像直接上传的，比出来当然相等，
// 断言绿着却什么都没测。`GetGpuDisplaceCount()` 存在的唯一理由就是堵住这个静默通过。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundShaperFieldParityTest,
	"PCGPlugins.ComputeShaderGenerator.GroundShaper.CpuGpuFieldParity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundShaperFieldParityTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;   // 本用例只看高度场，石阶那条路不参与
	Ground->RebuildGroundMesh();

	// 两座相接（默认 64×64 格 × 50 cm = 3200 cm 见方，reach 700 的两座完全落在里面），
	// 而且**参数各不相同** —— 种子那 20 bit 的打包路径与非默认波长都要被走到。
	ACSGroundShaperActor* MoundA = CSShaperFieldTest_SpawnMound(World, 1100.0, 1600.0);
	ACSGroundShaperActor* MoundB = CSShaperFieldTest_SpawnMound(World, 2100.0, 1600.0);
	if (!TestNotNull(TEXT("Mound A"), MoundA) || !TestNotNull(TEXT("Mound B"), MoundB)) return false;
	MoundB->SkirtNoiseSeed = 12345;
	MoundB->SkirtNoiseWavelength = 220.0f;

	const int32 BaselineDisplaces = Ground->GetGpuDisplaceCount();
	// 网格已经建好且位置对齐 ⇒ 这两趟必然走区域位移 pass，而不是退回全量重建。
	MoundA->RebuildTerrain();
	MoundB->RebuildTerrain();
	TestTrue(*FString::Printf(TEXT("The GPU displacement pass really ran, otherwise this compares CPU with CPU (displaces %d -> %d)"),
			BaselineDisplaces, Ground->GetGpuDisplaceCount()),
		Ground->GetGpuDisplaceCount() > BaselineDisplaces);

	UCSMesh* Mesh = Ground->GetTinyGladeMesh();
	if (!TestNotNull(TEXT("Ground GPU mesh"), Mesh)) return false;

	FCSGpuMeshCPUData Readback;
	if (!TestTrue(TEXT("Reading the ground mesh back"), Mesh->ReadbackMeshSync(Readback))) return false;

	const int32 VertsX = Ground->NumCellsX + 1;
	const int32 VertsY = Ground->NumCellsY + 1;
	if (!TestEqual(TEXT("Read back one vertex per grid point"), Readback.Positions.Num(), VertsX * VertsY)) return false;

	const FVector Origin = Ground->GetActorLocation();
	double WorstDelta = 0.0;
	int32 WorstX = 0, WorstY = 0;
	int32 NonFlat = 0;
	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			const FVector2D WorldXY(Origin.X + X * Ground->CellSize, Origin.Y + Y * Ground->CellSize);
			const double Cpu = Origin.Z
				+ FMath::Max(double(MoundA->SampleShapeHeight(WorldXY)), double(MoundB->SampleShapeHeight(WorldXY)));
			const double Gpu = double(Readback.Positions[Y * VertsX + X].Z);
			if (Cpu - Origin.Z > 1.0) ++NonFlat;
			const double Delta = FMath::Abs(Cpu - Gpu);
			if (Delta > WorstDelta) { WorstDelta = Delta; WorstX = X; WorstY = Y; }
		}
	}

	// 覆盖面自证：两座 reach 700 的土台在 50 cm 格上至少铺满几百个顶点，其中大半在裙边 ——
	// 没有这一条，一张全平的地面也能让下面的差值断言绿着。
	TestTrue(*FString::Printf(TEXT("The sweep really covered the mounds (nonFlat=%d)"), NonFlat), NonFlat > 400);
	// 容差 0.01 cm：噪声本身是纯整数哈希 + 加减乘，两侧同数；剩下的自由度只有编译器把
	// a*b+c 收成 fma、以及 sqrt 的最后一位，都在 1 ulp 量级（这里高度 ~10² cm ⇒ ~1e-5 cm）。
	// 真分叉（少一个倍频、插值写法不同、种子截断不同）直接是几十厘米，两者差六个数量级。
	TestTrue(*FString::Printf(TEXT("CPU twin and GPU agree on the height field vertex by vertex (worst=%.6f cm at grid %d,%d)"),
			WorstDelta, WorstX, WorstY),
		WorstDelta < 0.01);
	AddInfo(FString::Printf(TEXT("CPU/GPU parity: worst |dz| = %.6g cm over %d vertices (%d on the mounds)"),
		WorstDelta, VertsX * VertsY, NonFlat));

	World->DestroyActor(MoundA);
	World->DestroyActor(MoundB);
	World->DestroyActor(Ground);
	return true;
}

#endif   // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

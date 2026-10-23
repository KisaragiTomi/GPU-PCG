#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSGroundDecor.h"
#include "CSGroundShaperActor.h"
#include "CSGpuInstancedMeshComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/Material.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 塑形物裙边摆件（D12 锚点层的**第五家**）的验收。
//
// 分工与房子那四家逐字相同，因为失效方式也相同：
//   · 前六条是**纯 CPU** 的 —— 锚点在不在等值带上、密度是不是真由锚点数量决定、
//     随机稳不稳、被邻座埋掉的那一段有没有让开、路上有没有摆、上界够不够。
//     到了 GPU 那一侧这些只剩下一个实例计数，什么都断言不了。
//   · 第七条走**真烘焙**（裁决六 ①②③），抄的是 `GpuInstancedMesh.FrameBricksSurviveBake`。
//   · 第七条里还包着一条**画面侧**判据 `GetSkirtDecorUndrawableReason()`，
//     并且**当场做一次故意破坏**证明那道门不是空的 —— 石阶那个坑
//     （`StairMesh` 恒 NULL、画面一撮黑块、readback 全绿）就是"只有数值判据"的后果。
//   · 像素那一侧另有 `Scripts/TinyGladeShotSkirt.py`。四者缺一不可。
//
// ⚠️ 这里**不测复杂度场**：计划 D12 的 `RT_DecorField` + tile-argmax 在 TG 里没有对位物，
// C2 已拍板保留自有场但本轮不实现它。裙边这一家是**锚点**，不是场。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSSkirtTest_ 前缀
//（与 CSRockShellTest_ / CSStairsTest_ / CSDecorTest_ / CSGroundDecor_ 必须都不同）。

constexpr float CSSkirtTest_Radius = 600.0f;
constexpr float CSSkirtTest_Falloff = 800.0f;
constexpr float CSSkirtTest_Lift = 700.0f;

/** 地面：128 格 × 50 cm = 64 m，与岩壳那份夹具同一档（两座土台都摆得下）。 */
constexpr int32 CSSkirtTest_Cells = 128;
constexpr float CSSkirtTest_CellSize = 50.0f;
constexpr double CSSkirtTest_Centre = CSSkirtTest_Cells * CSSkirtTest_CellSize * 0.5;

CSGroundDecor::FShaperRing CSSkirtTest_MakeRing(double X, double Y, const TCHAR* Name,
	float Radius = CSSkirtTest_Radius, float Falloff = CSSkirtTest_Falloff, float Lift = CSSkirtTest_Lift)
{
	CSGroundDecor::FShaperRing Ring;
	// 布局与 `CSGroundShaperField.h` 逐字对应：Profile = (中心 X, 中心 Y, Radius, Falloff)。
	// 噪声幅度留 0：本文件量的是"锚点落在哪一圈"，开着噪声只会给每条断言加一个与它无关的容差。
	Ring.Header.Profile = FVector4f(float(X), float(Y), Radius, Falloff);
	Ring.Header.Top = FVector4f(Lift, 0.0f, 0.0f, 0.021f);
	Ring.Header.Noise = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	Ring.Key = CSGroundDecor::RingKey(Name);
	return Ring;
}

TArray<CSHouseDecor::FPaletteRange> CSSkirtTest_MakeRanges(int32 PaletteCount = 1)
{
	TArray<CSHouseDecor::FPaletteRange> Ranges;
	Ranges.SetNum(int32(CSHouseDecor::EFamily::Count));
	// **只有裙边那一格非空** —— 房子那四家的载体是房子，地面对它们一无所知。
	Ranges[int32(CSHouseDecor::EFamily::Skirt)] = { 0, PaletteCount };
	return Ranges;
}

/** 摆出来的记录，按 palette 摊平（顺序即规划顺序，逐位比较用）。 */
TArray<CSHouseDecor::FRecord> CSSkirtTest_Flatten(const CSHouseDecor::FPlan& Plan)
{
	TArray<CSHouseDecor::FRecord> Out;
	for (const TArray<CSHouseDecor::FRecord>& One : Plan.ByPalette) Out.Append(One);
	return Out;
}

CSHouseDecor::FPlan CSSkirtTest_Plan(const CSGroundDecor::FSite& Site, const CSHouseDecor::FParams& Params)
{
	TArray<CSHouseDecor::FAnchor> Anchors;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Anchors);
	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, CSSkirtTest_MakeRanges(), 1, Plan);
	return Plan;
}
}

// -----------------------------------------------------------------------------
// ① 锚点真的骑在**等值带**上：一圈、等弧长、朝外、立直
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorRidesTheBandTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtAnchorsRideTheBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundDecorRidesTheBandTest::RunTest(const FString&)
{
	CSGroundDecor::FSite Site;
	Site.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A")));
	const CSHouseDecor::FParams Params;

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Anchors);

	const float BandRadius = CSGroundDecor::RingRadius(Site.Rings[0], Params);
	const int32 Expected = FMath::Max(1, FMath::RoundToInt(UE_TWO_PI * BandRadius / Params.SkirtSpacing));
	AddInfo(FString::Printf(TEXT("环半径 %.1f cm ⇒ 锚点 %d 个（期望 %d，间距 %.0f cm）"),
		BandRadius, Anchors.Num(), Expected, Params.SkirtSpacing));

	// **锚点数 = 周长 / 间距**，这就是"密度完全由锚点个数决定"的形式化：它只依赖塑形物的
	// 尺寸与一个间距常数，不依赖格子数、不依赖任何场、更不依赖随机数。
	if (!TestEqual(TEXT("锚点数 = round(环周长 / 弧长间距)"), Anchors.Num(), Expected)) return false;

	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		TestEqual(TEXT("每一个锚点都属于裙边那一家"), int32(Anchor.Family), int32(CSHouseDecor::EFamily::Skirt));
		// 落在等值带上：到台心的**平面**距离恒等于 Radius + Falloff × BandT。
		// 这一条就是"等值带"这个说法的执行面 —— 差一点点的话摆件会站到台顶或平地上，
		// 而那两处都不是裙边，画面上读起来像"随手撒的"。
		const double Plane = FVector2D(Anchor.Location.X, Anchor.Location.Y).Size();
		TestTrue(FString::Printf(TEXT("锚点落在等值带上（%.2f vs %.2f）"), Plane, double(BandRadius)),
			FMath::IsNearlyEqual(Plane, double(BandRadius), 0.5));

		// 朝外 = 背离台心（同墙脚那一家的 `Strip.N`）。
		const FVector2D Radial = FVector2D(Anchor.Location.X, Anchor.Location.Y).GetSafeNormal();
		TestTrue(TEXT("朝向背离台心"),
			FVector2D::DotProduct(Radial, FVector2D(Anchor.Facing.X, Anchor.Facing.Y)) > 0.999);
		// 立直而不是跟着坡歪：歪着的桶读起来像穿模（屋顶那两家也是这么定的）。
		TestTrue(TEXT("上方向是世界上方向"), Anchor.Up.Equals(FVector::UpVector, 1e-4));
	}

	// 采样器缺席时的落高必须是**解析场**，不是平地 —— 退回平地会让这一条变成空判据，
	// 而"裙边摆件站在坡上"恰恰是这一家存在的全部理由。
	if (Anchors.Num() > 0)
	{
		const float BandS = CSGroundShaperField::EvalShaper(
			FVector2f(float(Anchors[0].Location.X), float(Anchors[0].Location.Y)),
			Site.Rings[0].Header, Site.Rings[0].Sdf);
		AddInfo(FString::Printf(TEXT("带上的解析高度 %.1f cm（台高 %.0f）"), BandS, CSSkirtTest_Lift));
		TestTrue(TEXT("锚点站在坡上（0 < 解析高度 < 台高）"), BandS > 1.0f && BandS < CSSkirtTest_Lift);
		TestTrue(TEXT("落高 = 解析高度 − 埋深"),
			FMath::IsNearlyEqual(Anchors[0].Location.Z, double(BandS) - double(Params.SkirtEmbed), 0.5));
	}
	return true;
}

// -----------------------------------------------------------------------------
// ② 密度由锚点个数决定：一个锚点最多一件，台子大一倍锚点就多一倍
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorDensityFollowsAnchorsTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtDensityFollowsAnchorCount",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundDecorDensityFollowsAnchorsTest::RunTest(const FString&)
{
	const CSHouseDecor::FParams Params;

	CSGroundDecor::FSite Small;
	Small.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A")));

	CSGroundDecor::FSite Big;
	Big.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A"),
		CSSkirtTest_Radius * 2.0f, CSSkirtTest_Falloff * 2.0f));

	TArray<CSHouseDecor::FAnchor> SmallAnchors, BigAnchors;
	CSGroundDecor::BuildSkirtAnchors(Small, Params, SmallAnchors);
	CSGroundDecor::BuildSkirtAnchors(Big, Params, BigAnchors);

	const CSHouseDecor::FPlan SmallPlan = CSSkirtTest_Plan(Small, Params);
	AddInfo(FString::Printf(TEXT("小 %d 锚 / %d 件；大 %d 锚"),
		SmallAnchors.Num(), SmallPlan.TotalRecords(), BigAnchors.Num()));

	TestTrue(TEXT("一座土台就长得出锚点"), SmallAnchors.Num() > 0);
	// **这条就是 D12「密度完全由锚点个数决定」的钉子**（与房子那四家的同名断言逐字同形）：
	// 一个锚点最多一件，所以摆件数永远追不上锚点数，也永远不会由"场的阈值"决定。
	TestTrue(FString::Printf(TEXT("摆件数 ≤ 锚点数（%d ≤ %d）"), SmallPlan.TotalRecords(), SmallAnchors.Num()),
		SmallPlan.TotalRecords() <= SmallAnchors.Num());
	TestTrue(TEXT("填充概率之下仍然摆出了东西"), SmallPlan.TotalRecords() > 0);

	// 半径翻倍 ⇒ 环周长翻倍 ⇒ 锚点数翻倍（±1 是取整）。若哪天改成了场，这条会立刻失效。
	TestTrue(FString::Printf(TEXT("台子大一倍，锚点就多一倍（%d vs %d）"), BigAnchors.Num(), SmallAnchors.Num()),
		FMath::Abs(BigAnchors.Num() - 2 * SmallAnchors.Num()) <= 1);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 随机源纪律：身份既不含**列表次序**，也不含**世界位置**
//
// ⚠️ 这是 S1 栽过的那一枪在裙边这一家的执行面。两种错法的症状都是"看起来随机"、
// **没有任何断言会自己报红**，所以必须专门钉一条。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorIdentityIsStableTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtIdentityIgnoresOrderAndPosition",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundDecorIdentityIsStableTest::RunTest(const FString&)
{
	const CSHouseDecor::FParams Params;

	// 三座离得足够远（互不遮挡、也不会互相挤掉），只是登记顺序不同。
	const double Span = 4000.0;
	CSGroundDecor::FSite A;
	A.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A")));
	A.Rings.Add(CSSkirtTest_MakeRing(Span, 0.0, TEXT("Mound_B")));
	A.Rings.Add(CSSkirtTest_MakeRing(0.0, Span, TEXT("Mound_C")));

	CSGroundDecor::FSite B;   // 同一批，倒着登记 —— 世界扫描的顺序本来就不可靠
	B.Rings.Add(A.Rings[2]);
	B.Rings.Add(A.Rings[0]);
	B.Rings.Add(A.Rings[1]);

	const TArray<CSHouseDecor::FRecord> RecA = CSSkirtTest_Flatten(CSSkirtTest_Plan(A, Params));
	const TArray<CSHouseDecor::FRecord> RecB = CSSkirtTest_Flatten(CSSkirtTest_Plan(B, Params));
	AddInfo(FString::Printf(TEXT("三座土台：正序 %d 件 / 逆序 %d 件"), RecA.Num(), RecB.Num()));

	if (!TestTrue(TEXT("三座土台摆出了东西"), RecA.Num() > 0)) return false;
	if (!TestEqual(TEXT("换个登记顺序，件数一件不差"), RecB.Num(), RecA.Num())) return false;
	for (int32 Index = 0; Index < RecA.Num(); ++Index)
	{
		// 逐位相同 —— 位置、随机数、缩放、朝向全都不许动。这一条钉死的是
		// "遍历序按稳定键排"那一步：不排的话最小间距球会按不同的顺序挤人。
		TestTrue(FString::Printf(TEXT("第 %d 件逐位相同"), Index),
			RecA[Index].WorldPos.Equals(RecB[Index].WorldPos, 1e-3f)
			&& FMath::IsNearlyEqual(RecA[Index].Random01, RecB[Index].Random01, 1e-6f)
			&& FMath::IsNearlyEqual(RecA[Index].Scale, RecB[Index].Scale, 1e-6f));
	}

	// **整体平移**：土台被拖走 1 km，摆件应当**跟着平移**，随机数一位都不许变。
	// 身份里混进世界坐标的话这里会全体重掷 —— 症状是"拖塑形物时整圈摆件不停变样"，
	// 而位置断言仍然全绿（它们确实还在环上）。
	const FVector2f Shift(1000.0f, 500.0f);
	CSGroundDecor::FSite Moved = A;
	for (CSGroundDecor::FShaperRing& Ring : Moved.Rings)
	{
		Ring.Header.Profile.X += Shift.X;
		Ring.Header.Profile.Y += Shift.Y;
	}
	const TArray<CSHouseDecor::FRecord> RecMoved = CSSkirtTest_Flatten(CSSkirtTest_Plan(Moved, Params));
	if (!TestEqual(TEXT("平移之后件数不变"), RecMoved.Num(), RecA.Num())) return false;
	for (int32 Index = 0; Index < RecA.Num(); ++Index)
	{
		TestTrue(FString::Printf(TEXT("第 %d 件的逐实例随机不随位置变"), Index),
			FMath::IsNearlyEqual(RecMoved[Index].Random01, RecA[Index].Random01, 1e-6f));
		TestTrue(FString::Printf(TEXT("第 %d 件正好跟着平移"), Index),
			FMath::IsNearlyEqual(RecMoved[Index].WorldPos.X - RecA[Index].WorldPos.X, Shift.X, 0.5f)
			&& FMath::IsNearlyEqual(RecMoved[Index].WorldPos.Y - RecA[Index].WorldPos.Y, Shift.Y, 0.5f));
	}
	return true;
}

// -----------------------------------------------------------------------------
// ④ 被邻座埋掉的那一段不长东西（合成场取 max ⇒ 那儿的地表不是本座的裙）
//
// **这一条就是"为什么归地面不归塑形物"的可执行证据**：判据要读**全部**塑形物，
// 而单座 actor 只看得见自己。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorBuriedArcStaysEmptyTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtBuriedByNeighbourGrowsNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundDecorBuriedArcStaysEmptyTest::RunTest(const FString&)
{
	const CSHouseDecor::FParams Params;

	CSGroundDecor::FSite Alone;
	Alone.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A")));
	TArray<CSHouseDecor::FAnchor> AloneAnchors;
	CSGroundDecor::BuildSkirtAnchors(Alone, Params, AloneAnchors);

	// 第二座压在第一座的裙边上：中心相距一个 Radius + Falloff，两盘不相交但裙彻底融成一体。
	const double Separation = double(CSSkirtTest_Radius) + double(CSSkirtTest_Falloff);
	CSGroundDecor::FSite Pair = Alone;
	Pair.Rings.Add(CSSkirtTest_MakeRing(Separation, 0.0, TEXT("Mound_B")));

	TArray<CSHouseDecor::FAnchor> PairAnchors;
	CSGroundDecor::BuildSkirtAnchors(Pair, Params, PairAnchors);
	AddInfo(FString::Printf(TEXT("单座 %d 锚；两座 %d 锚（各自整圈之和应是 %d）"),
		AloneAnchors.Num(), PairAnchors.Num(), AloneAnchors.Num() * 2));

	if (!TestTrue(TEXT("单座长得出锚点"), AloneAnchors.Num() > 0)) return false;
	// 少于两倍 = 接合处确实让开了。等于两倍就说明合成场那道判据没接上 ——
	// 而画面上的症状只是"接合处摆件挤成一堆"，没有任何计数会报红。
	TestTrue(FString::Printf(TEXT("接合处让开了（%d < %d）"), PairAnchors.Num(), AloneAnchors.Num() * 2),
		PairAnchors.Num() < AloneAnchors.Num() * 2);
	TestTrue(TEXT("但也没把两座都清空"), PairAnchors.Num() > AloneAnchors.Num());

	// 逐点判：两心之间那一段（x 落在 (Radius, Separation − Radius) 之内、且 |y| 很小）
	// 是被压得最狠的一带，那里一个锚点都不该有。
	int32 InJunction = 0;
	for (const CSHouseDecor::FAnchor& Anchor : PairAnchors)
	{
		if (Anchor.Location.X > double(CSSkirtTest_Radius)
			&& Anchor.Location.X < Separation - double(CSSkirtTest_Radius)
			&& FMath::Abs(Anchor.Location.Y) < 200.0)
		{
			++InJunction;
		}
	}
	TestEqual(TEXT("两心之间那条腰上一个锚点都没有"), InJunction, 0);

	// 台高为 0 的塑形物压根不隆起 ⇒ 它没有裙。摆一圈桶在平地上读起来像凭空掉的。
	CSGroundDecor::FSite Flat;
	Flat.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_Flat"),
		CSSkirtTest_Radius, CSSkirtTest_Falloff, /*Lift*/ 0.0f));
	TArray<CSHouseDecor::FAnchor> FlatAnchors;
	CSGroundDecor::BuildSkirtAnchors(Flat, Params, FlatAnchors);
	TestEqual(TEXT("台高为 0 的塑形物没有裙，也就没有裙边摆件"), FlatAnchors.Num(), 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 路上不摆（与石阶严格互补）+ ⑥ 上限装得下
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorRoadAndCapacityTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtRoadRejectsAndFitsCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSGroundDecorRoadAndCapacityTest::RunTest(const FString&)
{
	const CSHouseDecor::FParams Params;

	CSGroundDecor::FSite Site;
	Site.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A")));
	TArray<CSHouseDecor::FAnchor> Clean;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Clean);

	// 一条沿 X 轴穿过土台的路：|y| < 250 的锚点都该被排掉（石阶就长在那一段上）。
	Site.SampleRoadWeight = [](const FVector2D& XY) { return FMath::Abs(XY.Y) < 250.0 ? 1.0f : 0.0f; };
	TArray<CSHouseDecor::FAnchor> Roaded;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Roaded);
	AddInfo(FString::Printf(TEXT("无路 %d 锚 → 有路 %d 锚"), Clean.Num(), Roaded.Num()));

	TestTrue(FString::Printf(TEXT("路把一部分锚点排掉了（%d < %d）"), Roaded.Num(), Clean.Num()),
		Roaded.Num() < Clean.Num());
	TestTrue(TEXT("但没有把整圈排光"), Roaded.Num() > 0);
	for (const CSHouseDecor::FAnchor& Anchor : Roaded)
	{
		TestTrue(TEXT("没有任何锚点站在路面上"), FMath::Abs(Anchor.Location.Y) >= 250.0);
	}

	// 上界必须**不依赖**当前有几个锚点被排掉 —— 依赖的话画一笔路就可能越过一次容量台阶，
	// 当场付一次设备同步（零阻塞纪律）。所以两种情形要拿到同一个上界。
	const int32 BoundClean = CSGroundDecor::MaxRecordsBound(Site, Params);
	CSGroundDecor::FSite NoRoad = Site;
	NoRoad.SampleRoadWeight = nullptr;
	TestEqual(TEXT("容量上限与「路把多少锚点排掉了」无关"),
		CSGroundDecor::MaxRecordsBound(NoRoad, Params), BoundClean);
	TestTrue(FString::Printf(TEXT("上界装得下整圈（%d ≥ %d）"), BoundClean, Clean.Num()),
		BoundClean >= Clean.Num());

	// 连续拉半径：每一档都要装得下。这一条守的是"上界是半径的连续函数"这件事本身 ——
	// 漏了它就会在某个尺寸上静默截断（少摆几件，没有任何断言会红）。
	for (int32 Step = 0; Step <= 8; ++Step)
	{
		CSGroundDecor::FSite One;
		One.Rings.Add(CSSkirtTest_MakeRing(0.0, 0.0, TEXT("Mound_A"),
			200.0f + float(Step) * 175.0f, CSSkirtTest_Falloff));
		TArray<CSHouseDecor::FAnchor> Grown;
		CSGroundDecor::BuildSkirtAnchors(One, Params, Grown);
		const int32 Bound = CSGroundDecor::MaxRecordsBound(One, Params);
		TestTrue(FString::Printf(TEXT("半径档 %d：上界 %d ≥ 锚点 %d"), Step, Bound, Grown.Num()),
			Bound >= Grown.Num());
	}
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 烘焙出口（裁决六 ①②③）+ 画面侧那道门是不是活的
//
// 抄的是 `GpuInstancedMesh.FrameBricksSurviveBake` 的形状：**真烘一遍**，再从烘出来的
// 资产里读回来。多做的一件事是**当场故意破坏世界侧**（摩掉母材质的实例化标志），
// 确认 `GetSkirtDecorUndrawableReason()` 真的会红 —— 一道从来没红过的门等于没有门。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundDecorSurvivesBakeTest,
	"PCGPlugins.ComputeShaderGenerator.GroundDecor.SkirtPropsSurviveBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundDecorSurvivesBakeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 引擎的 Cube 顶了 TG 的 clutter：这条测的是**出口**不是资产，用引擎自带件才不会因为
	// `Content/` 没跟进 git 就跑不起来（同门框砖那条）。Cube 自带 UV0 + 光照图 UV1，
	// 「多组 UV 都要活下来」这条断言因此有内容。
	UStaticMesh* PropMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("摆件基础网格"), PropMesh)) return false;

	// 瞬态母材质：判据只读 `bUsedWithInstancedStaticMeshes` 这一个标志，所以不需要真编译。
	// 用它而不是现成资产，是为了下面那次**故意破坏**能安全地把标志摩掉再摩回来。
	UMaterial* PropMaterial = NewObject<UMaterial>(GetTransientPackage());
	if (!TestNotNull(TEXT("摆件材质"), PropMaterial)) return false;
	PropMaterial->bUsedWithInstancedStaticMeshes = true;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->NumCellsX = CSSkirtTest_Cells;
	Ground->NumCellsY = CSSkirtTest_Cells;
	Ground->CellSize = CSSkirtTest_CellSize;
	// 石阶与岩壳关掉：本文件量的是裙边摆件，多两条 GPU 路只会多一份耗时与噪声。
	Ground->StairMesh = nullptr;
	Ground->bRockShell = false;
	Ground->bSkirtDecorEnabled = true;
	Ground->SkirtDecorMeshes = { PropMesh };
	Ground->SkirtDecorMaterial = PropMaterial;
	Ground->RebuildGroundMesh();

	ACSGroundShaperActor* Mound = World->SpawnActor<ACSGroundShaperActor>(
		FVector(CSSkirtTest_Centre, CSSkirtTest_Centre, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;
	Mound->MaskSize = 2.0f * float(CSSkirtTest_Radius);   // 圆盘直径（2026-09-29 起形状是 mask，没设 mask = 解析圆盘）
	Mound->FalloffDistance = CSSkirtTest_Falloff;
	// 台顶 = actor 高度（2026-09-29）；乘 (1 + 二次抬升) 保持与旧口径「抬 LiftHeight、台顶再照抬 2%」逐项相同的剖面。
	Mound->SetActorLocation(Mound->GetActorLocation() + FVector(0.0, 0.0, double(CSSkirtTest_Lift) * (1.0 + double(Mound->SecondaryLiftScale))));
	Mound->RebuildTerrain();
	Ground->RebuildSkirtDecor();

	const int32 Anchors = Ground->GetSkirtDecorAnchorCount();
	const int32 Instances = Ground->GetSkirtDecorInstanceCount();
	AddInfo(FString::Printf(TEXT("锚点 %d 件 %d"), Anchors, Instances));
	if (!TestTrue(FString::Printf(TEXT("土台长出了裙边锚点（%d 个）"), Anchors), Anchors > 0)) return false;
	if (!TestTrue(FString::Printf(TEXT("锚点上真的摆了东西（%d 件）"), Instances), Instances > 0)) return false;
	TestTrue(TEXT("摆件数 ≤ 锚点数（密度由锚点个数决定）"), Instances <= Anchors);

	// ---- 画面侧那道门：先证明它现在是绿的 ----
	// ⚠️ 调原因版，别调 `IsSkirtDecorDrawable()`（那一版在 Python 侧会丢原因串，见声明注释）。
	const FString Reason = Ground->GetSkirtDecorUndrawableReason();
	if (!TestEqual(TEXT("裙边摆件画得出来（组件/快照/实例源/材质支持实例化）"), Reason, FString())) return false;

	// ---- **故意破坏世界侧**：摩掉母材质的实例化标志，那道门必须当场变红 ----
	// 这一枪就是坑表里那条"没勾 `bUsedWithInstancedStaticMeshes` ⇒ 引擎静默换成默认材质"，
	// 症状与"没绑材质"逐像素相同，而所有 readback 断言照绿。一道从来没红过的门等于没有门。
	PropMaterial->bUsedWithInstancedStaticMeshes = false;
	const FString Broken = Ground->GetSkirtDecorUndrawableReason();
	AddInfo(FString::Printf(TEXT("故意破坏后的原因串：%s"), *Broken));
	TestTrue(TEXT("摩掉实例化标志之后那道门真的报红（门不是空的）"), !Broken.IsEmpty());
	TestTrue(TEXT("而且报的正是实例化标志那一条"), Broken.Contains(TEXT("bUsedWithInstancedStaticMeshes")));
	PropMaterial->bUsedWithInstancedStaticMeshes = true;

	// ---- GPU 上真的在画的那一份必须与 CPU 的账对得上 ----
	const int32 GpuInstances = Ground->DebugReadSkirtDecorInstanceCountGpuSync();
	if (!TestEqual(TEXT("GPU 计数器与 CPU 的账一致"), GpuInstances, Instances)) return false;

	// ---- 真烘一遍（裁决六 ①）----
	const FString BakePath = FString::Printf(
		TEXT("/Game/Automation/GroundDecor/SM_SkirtDecor_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	int32 Triangles = 0, VertexInstances = 0, UVChannels = 0, DistinctUVs = 0, DistinctRandoms = 0, BakedInstances = 0;
	bool bRandomsMatchGpu = false;
	const bool bBaked = Ground->DebugBakeSkirtDecorSync(
		BakePath, Triangles, VertexInstances, UVChannels, DistinctUVs, DistinctRandoms, BakedInstances, bRandomsMatchGpu);
	AddInfo(FString::Printf(TEXT("烘成 StaticMesh：%d 三角 / %d 角点 / %d 组 UV（%d 种取值）；逐实例随机 %d 种（%d 个实例）"),
		Triangles, VertexInstances, UVChannels, DistinctUVs, DistinctRandoms, BakedInstances));
	if (!TestTrue(TEXT("裙边摆件有一条走得通的 SaveToStaticMesh 出口（裁决六 ①）"), bBaked)) return false;

	// 几何：烘出来的必须是**整族**摆件，不是一件。三角数从资产现取，不写常数
	// （引擎的 BasicShapes/Cube 换过面数，写死会在下次引擎升级时报一个与出口无关的红）。
	TestEqual(TEXT("烘出来的实例数 = GPU 上的实例数"), BakedInstances, Instances);
	TestEqual(TEXT("三角数 = 实例数 × 基础网格三角数"), Triangles, Instances * PropMesh->GetNumTriangles(0));
	TestEqual(TEXT("角点数 = 三角数 × 3"), VertexInstances, Triangles * 3);

	// 裁决六 ②（UV 那一半）。⚠️ **这里期望的是 1 组，不是资产的 2 组，而且这是有意的**：
	// 摆件的基础网格走 `CSHouseVine::BuildBaseMesh`（clutter 与 `ivy_branch` 一样可能根本没有
	// 法线与 UV，那个读取器负责现补），它把 `NumTexCoordChannels` **钉成 1** ——
	// 多组 UV 那一半由 `GpuInstancedMesh.FrameBricksSurviveBake` 守着（门框砖走 `SetBaseMesh`，
	// 原样保留资产的全部通道）。写成"与资产一致"会报一个与出口无关的红。
	// 真正在这条路上会静默失效的是**UV 退化成全 (0,0)**：不报错，烘焙件贴图变成一整片同一个像素。
	AddInfo(FString::Printf(TEXT("基础网格资产有 %d 组 UV；实例路快照按 BuildBaseMesh 的口径只带 1 组"),
		PropMesh->GetNumTexCoords(0)));
	TestEqual(TEXT("烘焙件带着快照那一组 UV（BuildBaseMesh 口径：恒 1 组）"), UVChannels, 1);
	TestTrue(FString::Printf(TEXT("那一组 UV 不是退化的（%d 种取值）"), DistinctUVs), DistinctUVs > 1);

	// **这一条就是裁决六 ③ 的判据本身**：烘完就没有实例了，`PerInstanceRandom` 恒 0，
	// 材质那条 `lerp(0.78, 1.22, rnd)` 会把整片摆件烘成同一个色，而三角数 / 角点数 /
	// UV 组数 / 实例数上面四条**全都会照绿**。随机数烘在顶点色 A。
	TestTrue(FString::Printf(TEXT("逐实例随机不止一种取值（%d 种 / %d 件）"), DistinctRandoms, Instances),
		DistinctRandoms > 1);
	TestTrue(TEXT("烘焙件里的逐实例随机与 GPU 上那一份逐个对得上"), bRandomsMatchGpu);

	UEditorAssetLibrary::DeleteAsset(BakePath);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

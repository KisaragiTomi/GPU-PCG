#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"      // (18) 脚下有没有地面
#include "CSGroundShaperSteps.h"  // (17) CSShaperSteps::CapacityStep —— 管子容量的台阶
#include "CSHouseActor.h"
#include "CSHouseProfile.h"
#include "CSHouseVine.h"
#include "CSMesh.h"             // (17) 管子常驻网格的容量与阻塞刷新计数器
#include "CSVineTube.h"         // (17) 折线 → 管子的对外入口
#include "Math/NumericLimits.h"
#include "RenderingThread.h"    // (17) FlushRenderingCommands —— 泵异步编辑的游戏线程尾巴
#include "UObject/Package.h"    // (17) GetTransientPackage
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 墙面藤蔓（D13）的验收。
//
// ⚠️ **这里的三条都是纯 CPU 判据，刻意不碰 RHI**：藤蔓最容易错的三件事
// （避不避墙洞、随机稳不稳、藤有没有跑出墙）在 GPU 那一侧只剩下一个实例计数，
// 到那时候什么都断言不了。GPU 那一侧的判据是**另一条**：
// `ACSHouseActor::IsVineDrawable`（渲染环节逐环检查）+ 出图脚本的像素门。
// 两者缺一不可 —— 石阶那个坑（`StairMesh` 恒 NULL、画面黑块、readback 全绿）
// 正是"只有数值判据"的后果。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSVineTest_ 前缀
// （与 CSHouseVine.cpp 的 CSHouseVine_、CSHouseLogicTests 的 CSHouseTest_ 都不同）。

constexpr float CSVineTest_Length = 600.0f;
constexpr float CSVineTest_Height = 300.0f;

/** 一面沿 +X 的墙，墙脚在原点，外法线 −Y。四面墙里最简单的那一面，够钉所有几何判据。 */
CSHouseVine::FWallStrip CSVineTest_MakeStrip(int32 EdgeIndex = 0)
{
	CSHouseVine::FWallStrip Strip;
	Strip.EdgeIndex = EdgeIndex;
	Strip.Origin = FVector(0.0, 0.0, 0.0);
	Strip.U = FVector(1.0, 0.0, 0.0);
	Strip.Up = FVector(0.0, 0.0, 1.0);
	Strip.N = FVector(0.0, -1.0, 0.0);
	Strip.Length = CSVineTest_Length;
	Strip.Height = CSVineTest_Height;
	return Strip;
}

CSHouseVine::FParams CSVineTest_MakeParams()
{
	CSHouseVine::FParams P;   // 默认值 = ACSHouseActor 的出厂值
	return P;
}

/** 世界点 → 墙面参数 (s, z)。墙脚在原点、U = +X、Up = +Z ⇒ 直接取分量。 */
FVector2D CSVineTest_ToWall(const FVector3f& World)
{
	return FVector2D(World.X, World.Z);
}

/**
 * (s, z) 真的落在洞形里？**直接调材质那份判据**（`CSHouse_ClipKeeps`），不是洞的外接矩形。
 *
 * ⚠️ 这一条是有意与被测代码"同源"的：被测的是"藤让开的地方与墙被切掉的地方**是不是同一块**"，
 * 所以判据必须是墙那边的那一份。另写一份近似的反而会把两边的分歧掩盖掉。
 */
bool CSVineTest_InArchProfile(const FCSWallOpening& Opening, const FVector2D& SZ)
{
	if (SZ.Y < Opening.Z0) return false;
	const FCSOpeningClipField Field = CSHouse_ComputeClipField(Opening);
	return !CSHouse_ClipKeeps(Field, Field.Eval(float(SZ.X), float(SZ.Y)));
}

/** 洞的**外接矩形**里（第一档的判据）。拱肩那两块在矩形里、却是实心墙。 */
bool CSVineTest_InOpeningRect(const FCSWallOpening& Opening, const FVector2D& SZ)
{
	return SZ.X >= Opening.S0() && SZ.X <= Opening.S1() && SZ.Y >= Opening.Z0 && SZ.Y <= Opening.Z1;
}

/** 墙正中一个 200 cm 宽、220 cm 高的落地拱门。 */
FCSWallOpening CSVineTest_MakeDoor(float CenterS = CSVineTest_Length * 0.5f, float Width = 200.0f)
{
	FCSWallOpening Door;
	Door.Type = ECSOpeningType::Door;
	Door.Shape = ECSOpeningShape::Arch;
	Door.EdgeIndex = 0;
	Door.CenterS = CenterS;
	Door.Width = Width;
	Door.Z0 = 0.0f;
	Door.Z1 = 220.0f;
	return Door;
}
}

// -----------------------------------------------------------------------------
// ① 藤长在墙上，而且长在墙的**外**面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineOnWallTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineStaysOnWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineOnWallTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	TestTrue(TEXT("一面 6 m × 3 m 的墙上排得出藤"), Plan.Branch.Num() > 20);
	TestTrue(TEXT("排得出叶子"), Plan.Leaf.Num() > 5);

	// 藤脚**必须**贴着墙脚：整根藤是从 z=0 长上去的，第一段的起点就是根。
	// 这一条会在"起点被错误地写成段中点"时报红 —— 症状是整片藤悬空半段。
	float MinZ = TNumericLimits<float>::Max();
	float MaxZ = -TNumericLimits<float>::Max();
	float MinS = TNumericLimits<float>::Max();
	float MaxS = -TNumericLimits<float>::Max();
	float MaxOffWall = 0.0f;
	for (const CSHouseVine::FRecord& R : Plan.Branch)
	{
		const FVector2D SZ = CSVineTest_ToWall(R.WorldPos);
		MinS = FMath::Min(MinS, float(SZ.X));
		MaxS = FMath::Max(MaxS, float(SZ.X));
		MinZ = FMath::Min(MinZ, float(SZ.Y));
		MaxZ = FMath::Max(MaxZ, float(SZ.Y));
		// 离墙距离 = −Y（外法线是 −Y）。整片藤都应当正好在 StandOff 上。
		MaxOffWall = FMath::Max(MaxOffWall, FMath::Abs(-R.WorldPos.Y - Params.StandOff));
	}

	TestEqual(TEXT("藤脚正好落在墙脚（z = 0）"), MinZ, 0.0f, 0.001f);
	TestTrue(FString::Printf(TEXT("藤爬得上去（最高 %.1f cm，墙高 %.0f）"), MaxZ, CSVineTest_Height),
		MaxZ > CSVineTest_Height * 0.4f);
	TestTrue(FString::Printf(TEXT("藤不许爬出墙顶（最高 %.1f cm ≤ %.0f）"), MaxZ, CSVineTest_Height),
		MaxZ <= CSVineTest_Height + 0.001f);
	TestTrue(FString::Printf(TEXT("藤不许跑出墙的左端（最小 s = %.1f）"), MinS), MinS >= -0.001f);
	TestTrue(FString::Printf(TEXT("藤不许跑出墙的右端（最大 s = %.1f ≤ %.0f）"), MaxS, CSVineTest_Length),
		MaxS <= CSVineTest_Length + 0.001f);
	TestEqual(TEXT("整片藤都贴在墙外皮上（离墙距离恒 = StandOff）"), MaxOffWall, 0.0f, 0.01f);

	// 负缝：段的**渲染长度**必须比它跨过的几何距离长（Bloat > 1），否则每个折点都会露一条亮缝。
	// 这是门框砖那条 TG 实证（`FrameBrickOverlap`）在藤上的同构版本。
	int32 PositiveSeam = 0;
	for (const CSHouseVine::FRecord& R : Plan.Branch)
	{
		if (R.LengthScale <= Params.SegmentLength * 1.0001f) ++PositiveSeam;
	}
	TestEqual(TEXT("没有任何一段是正缝（渲染长度 > 几何段长）"), PositiveSeam, 0);

	return true;
}

// -----------------------------------------------------------------------------
// ② 逐实例随机只由身份决定 —— "不许拿 InterlockedAdd 的槽位当种子"那条纪律的执行面
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineIdentityTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineRandomIsIdentityBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineIdentityTest::RunTest(const FString& Parameters)
{
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	// ⓐ 同一份输入两次规划**逐位相同**。规划是纯函数，所以这一条是可判定的；
	//    S1 那个 bug（种子取槽位）在这条断言下依然会绿 —— 所以还有 ⓑ。
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	CSHouseVine::FPlan A, B;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, A);
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, B);
	TestEqual(TEXT("两次规划的段数相同"), B.Branch.Num(), A.Branch.Num());
	int32 Differ = 0;
	for (int32 i = 0; i < FMath::Min(A.Branch.Num(), B.Branch.Num()); ++i)
	{
		if (A.Branch[i].Random01 != B.Branch[i].Random01) ++Differ;
	}
	TestEqual(TEXT("两次规划的逐实例随机逐位相同"), Differ, 0);

	// ⓑ **这才是那条纪律**：把第 0 面墙换成"更长的墙"⇒ 段数变、槽位全体重排，
	//    但**同一根藤的同一段**必须拿到同一个随机数。取槽位的话这一条必红。
	TArray<CSHouseVine::FWallStrip> Longer;
	CSHouseVine::FWallStrip Wide = CSVineTest_MakeStrip();
	Wide.Length = CSVineTest_Length * 2.0f;
	Longer.Add(Wide);
	CSHouseVine::FPlan C;
	CSHouseVine::BuildPlan(Longer, TArray<FCSWallOpening>(), Params, C);
	TestTrue(TEXT("更长的墙确实排出了更多段（槽位一定重排过）"), C.Branch.Num() > A.Branch.Num());

	// 直接对身份哈希本身取样：同一身份恒等、不同身份必不同。
	const uint32 H0 = CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed);
	const uint32 H1 = CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed);
	TestEqual(TEXT("同一身份两次取样恒等"), H1, H0);
	TestNotEqual(TEXT("换一根藤就换一个随机"), CSHouseVine::IdentityHash(0, 4, 7, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换一段就换一个随机"), CSHouseVine::IdentityHash(0, 3, 8, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换一面墙就换一个随机"), CSHouseVine::IdentityHash(1, 3, 7, 71u, Params.Seed), H0);
	TestNotEqual(TEXT("换用户种子就换一个随机"), CSHouseVine::IdentityHash(0, 3, 7, 71u, Params.Seed + 1), H0);

	// 随机数必须真的铺满 [0,1)：一个恒定的"随机"同样能让上面几条全绿。
	float Lo = 1.0f, Hi = 0.0f;
	for (const CSHouseVine::FRecord& R : A.Branch) { Lo = FMath::Min(Lo, R.Random01); Hi = FMath::Max(Hi, R.Random01); }
	TestTrue(FString::Printf(TEXT("随机数铺得开（[%.3f, %.3f]）"), Lo, Hi), Hi - Lo > 0.8f);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 藤避让墙洞 —— TG 的 `ivy_grower` 读集里有 `PrevWallHoles`（对照文档 §6.1【确凿】）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineAvoidsHolesTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineAvoidsWallHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineAvoidsHolesTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	const FCSWallOpening Door = CSVineTest_MakeDoor();
	TArray<FCSWallOpening> Openings;
	Openings.Add(Door);

	CSHouseVine::FPlan Clear, Holed;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Clear);
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);

	// ⓐ 洞**形**里（拱 = 矩形下身 + 半圆顶，不含外扩带）一段都不许有。
	// ⚠️ 判据从"洞的外接矩形"换成了 `CSHouse_ClipKeeps` 那条曲线（2026-08-31，第二档）：
	// 拱肩那两块在外接矩形里、却是**实心墙**，材质也确实把它们画出来。拿矩形当判据的话
	// 下面 ⓔ 那条改进就无法成立（它恰恰要求藤长到拱肩上去）。
	int32 Inside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Branch)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++Inside;
	}
	TestEqual(TEXT("门洞里一段藤都没有"), Inside, 0);

	// ⓑ 对照组：没有洞的时候那块地方**本来是有藤的** —— 否则 ⓐ 是恒真的空话。
	int32 WouldHaveBeen = 0;
	for (const CSHouseVine::FRecord& R : Clear.Branch)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++WouldHaveBeen;
	}
	TestTrue(FString::Printf(TEXT("没洞时那块地方本来有 %d 段藤（否则 ⓐ 是空话）"), WouldHaveBeen),
		WouldHaveBeen > 0);

	// ⓒ 洞只吃掉洞附近的藤，不是把整面墙的藤都干掉。
	TestTrue(FString::Printf(TEXT("洞外的藤还在（%d / %d）"), Holed.Branch.Num(), Clear.Branch.Num()),
		Holed.Branch.Num() > Clear.Branch.Num() / 3);

	// ⓓ 叶子也一起避让 —— 叶挂在段中点上，段没了叶自然没了；这一条钉的是"没有漏网的叶"。
	int32 LeavesInside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Leaf)
	{
		if (CSVineTest_InArchProfile(Door, CSVineTest_ToWall(R.WorldPos))) ++LeavesInside;
	}
	TestEqual(TEXT("门洞里一片叶子都没有"), LeavesInside, 0);

	// ⓔ 拱肩（拱脚线以上、外接矩形以内、拱形以外）是**实心墙**，把判据从外接矩形换成解析
	// clip 场之后藤可以长上去。
	//
	// ⚠️ **这一条只出数、不设门**，理由与状态文件里裙边噪声那条 `pair3q` 同型：信号量到不了
	// 能与噪声分开的量级。实测过三种形状，肩上分别只有 0 / 1 / 2 段 —— 因为 12 cm 外扩
	// 把拱肩上"藤够得着"的那部分几乎吃光了（高拱：半宽 100、拱脚 120 ⇒ 外扩后半径 112，
	// 自由带在 Z=220 处宽 49.6 cm、Z=180 处只剩 5.4 cm，往下直接闭合，而藤是从墙脚爬上来的）。
	// ⇒ **换判据这件事本身几乎不改变画面**，别把它当成"拱附近变稀"的解药 ——
	//    那条的解药是藤脚侧移（见 VineRootEscapesHoles，实测保留率 0.663）。
	//    换判据的价值在**一致性**：藤让开的地方与墙真被切掉的地方从此是同一条曲线（裁决三），
	//    那一条由 VineHoleFieldMatchesClipField 的 7381 个取样点守着。
	// 一面 12 m 长的墙 + 一个纯半圆拱（宽 400 ⇒ 拱脚落到 Z1 − 半宽 = 0）：肩是两块
	// 从拱脚一路开到洞顶的大楔形，藤贴着拱缘爬上去正好落在里面。
	FCSWallOpening Squat = CSVineTest_MakeDoor(600.0f, 400.0f);
	Squat.Z1 = 200.0f;
	TArray<FCSWallOpening> SquatOnly;
	SquatOnly.Add(Squat);
	TArray<CSHouseVine::FWallStrip> LongWall;
	{
		CSHouseVine::FWallStrip Wide = CSVineTest_MakeStrip();
		Wide.Length = 1200.0f;
		LongWall.Add(Wide);
	}
	CSHouseVine::FPlan SquatPlan;
	CSHouseVine::BuildPlan(LongWall, SquatOnly, Params, SquatPlan);

	int32 OnShoulder = 0, InSquatHole = 0;
	for (const CSHouseVine::FRecord& R : SquatPlan.Branch)
	{
		const FVector2D SZ = CSVineTest_ToWall(R.WorldPos);
		if (CSVineTest_InArchProfile(Squat, SZ)) ++InSquatHole;
		else if (CSVineTest_InOpeningRect(Squat, SZ)) ++OnShoulder;
	}
	TestEqual(TEXT("半圆拱的洞形里也一段藤都没有"), InSquatHole, 0);
	AddInfo(FString::Printf(TEXT("拱肩（矩形内、拱形外）上 %d 段 —— 只记录，不设门（见上）"), OnShoulder));
	return true;
}

// -----------------------------------------------------------------------------
// ⑩ 洞外扩量不许小于门框砖伸出去的那一半 —— "要不要再放宽"这个问题的可判定形式
//
// `VineHoleClearance` 存在的**唯一**理由是别让藤穿进门框砖：那一列砖骑在墩心、横向占
// `FrameBrickDepth`（默认 20 cm）⇒ 它伸到洞缘外 10 cm。外扩小于 10 就等于让藤长进砖里，
// 而症状是"拱缘上一圈藤和砖互相穿插"——只在贴脸看时才看得出来，出图的差异率读不出来。
// 所以"能不能再放宽"这件事有一条硬下界，本条把它钉住，省得将来有人为了让拱周围密一点
// 顺手把它调小。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineClearanceCoversFrameTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineClearanceCoversFrameBricks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineClearanceCoversFrameTest::RunTest(const FString& Parameters)
{
	const ACSHouseActor* Defaults = GetDefault<ACSHouseActor>();
	if (!Defaults) { AddError(TEXT("拿不到 ACSHouseActor 的 CDO")); return false; }

	const float Reach = Defaults->FrameBrickDepth * 0.5f;
	TestTrue(FString::Printf(TEXT("洞外扩 %.1f cm ≥ 门框砖伸出的 %.1f cm"),
		Defaults->VineHoleClearance, Reach), Defaults->VineHoleClearance >= Reach);
	// 也别虚高：外扩每多 1 cm，拱周围就少一圈藤。三倍是"还算得上贴着砖"的上界。
	TestTrue(FString::Printf(TEXT("洞外扩 %.1f cm 没有虚高到 3 × %.1f cm 以上"),
		Defaults->VineHoleClearance, Reach), Defaults->VineHoleClearance <= Reach * 3.0f);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 洞判据与材质那份 clip 场**逐点一致** —— 裁决三（渲染层挖洞）的一条可判定形式
//
// 洞是逐像素 discard 切出来的，所以"墙在哪里没了"的唯一真源是 `CSHouse_ClipKeeps`。
// 藤要是自己另写一份近似（第一档就是外接矩形），两边的分歧不会有任何断言报红 ——
// 只会在画面上表现成"拱周围一圈莫名其妙的秃斑"，而所有计数照绿。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineHoleFieldParityTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineHoleFieldMatchesClipField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineHoleFieldParityTest::RunTest(const FString& Parameters)
{
	TArray<FCSWallOpening> Openings;
	Openings.Add(CSVineTest_MakeDoor());
	{
		FCSWallOpening Window = CSVineTest_MakeDoor(120.0f, 90.0f);
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.Z0 = 110.0f;
		Window.Z1 = 200.0f;
		Openings.Add(Window);
	}

	// Clearance = 0 时两份判据必须**逐点相同**。取样格刻意跨过拱脚线与窗台，
	// 那两条正是两种形状各自的分段点。
	int32 Mismatch = 0;
	int32 InsideCount = 0;
	for (int32 IS = 0; IS <= 120; ++IS)
	{
		for (int32 IZ = 0; IZ <= 60; ++IZ)
		{
			const float S = float(IS) * (CSVineTest_Length / 120.0f);
			const float Z = float(IZ) * (CSVineTest_Height / 60.0f);
			bool bReference = false;
			for (const FCSWallOpening& O : Openings) bReference |= CSVineTest_InArchProfile(O, FVector2D(S, Z));
			const bool bVine = CSHouseVine::IsInsideOpening(Openings, 0, S, Z, 0.0f);
			if (bVine != bReference) ++Mismatch;
			if (bReference) ++InsideCount;
		}
	}
	TestTrue(FString::Printf(TEXT("取样格上真的有点落在洞里（%d 个，否则这条是空话）"), InsideCount),
		InsideCount > 100);
	TestEqual(TEXT("藤的洞判据与材质的 clip 场逐点一致"), Mismatch, 0);

	// 外扩把洞胀大**而不是把结论外扩**：拱胀大以后拱脚线逐位不变。
	// 破了它的症状是拱一开外扩就整体上移，藤在拱顶留一条月牙形秃带。
	FCSWallOpening Door = CSVineTest_MakeDoor();
	const float SpringZ = Door.Z1 - Door.HalfWidth();
	TestTrue(TEXT("拱脚线正上方一点点、洞外半宽 + 一半外扩处仍算洞内"),
		CSHouseVine::IsInsideOpening(Openings, 0, Door.CenterS + Door.HalfWidth() + 6.0f, SpringZ - 1.0f, 12.0f));
	TestFalse(TEXT("同一高度上、洞外半宽 + 两倍外扩处已经在洞外"),
		CSHouseVine::IsInsideOpening(Openings, 0, Door.CenterS + Door.HalfWidth() + 24.0f, SpringZ - 1.0f, 12.0f));
	// 洞底以下是实心（窗台那一截）：Arch 的 clip 场在拱脚线以下无下界，这条边界只能自己补。
	FCSWallOpening Sill = CSVineTest_MakeDoor(120.0f, 90.0f);
	Sill.Z0 = 110.0f;
	Sill.Z1 = 200.0f;
	TArray<FCSWallOpening> Only;
	Only.Add(Sill);
	TestFalse(TEXT("拱形窗的窗台以下是实心墙"), CSHouseVine::IsInsideOpening(Only, 0, Sill.CenterS, 40.0f, 12.0f));
	TestTrue(TEXT("拱形窗的洞口里是洞"), CSHouseVine::IsInsideOpening(Only, 0, Sill.CenterS, 140.0f, 12.0f));
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 藤脚落在洞里时**侧移**而不是整根丢掉
//
// 第一档："藤脚在洞里 ⇒ 这根不长"。落地拱把整个洞宽的墙脚都吃掉，所以拱越多秃得越狠。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineRootEscapesHolesTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineRootEscapesHoles",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineRootEscapesHolesTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	// 三个落地拱（演示房子那面长墙的形状）。
	TArray<FCSWallOpening> Openings;
	for (int32 K = 0; K < 3; ++K) Openings.Add(CSVineTest_MakeDoor(120.0f + 180.0f * float(K), 150.0f));

	CSHouseVine::FPlan Clear, Holed;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Clear);
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);

	// 洞形里仍然一段都没有 —— 放宽不许换来穿帮。
	int32 Inside = 0;
	for (const CSHouseVine::FRecord& R : Holed.Branch)
	{
		for (const FCSWallOpening& O : Openings)
		{
			if (CSVineTest_InArchProfile(O, CSVineTest_ToWall(R.WorldPos))) { ++Inside; break; }
		}
	}
	TestEqual(TEXT("三个拱全开时洞形里一段藤都没有"), Inside, 0);

	// 藤脚必须**贴地**：侧移只沿墙脚挪，不许把根抬到半空（那会长出一片悬空的藤）。
	float MinZ = TNumericLimits<float>::Max();
	for (const CSHouseVine::FRecord& R : Holed.Branch) MinZ = FMath::Min(MinZ, R.WorldPos.Z);
	TestEqual(TEXT("侧移过的藤脚仍然落在墙脚（z = 0）"), MinZ, 0.0f, 0.001f);

	// 三个拱吃掉 450 / 600 cm 的墙脚（含外扩后自由墙脚只剩 78 / 600 cm ≈ 13%），
	// 藤仍应保住**大部分**段数。阈值 0.45 的依据是两头都够远：
	//   · 实测 0.598（本条自己 AddInfo 出来的数，重跑即可核对）。
	//     ⚠️ 2026-09-12 之前这个数是 **0.663**，掉下来是**有意的**：那一轮拿掉了洞缘上的
	//     倾角镜像（一次折 131.8° 的"折断"式急拐），藤不再靠在 20 cm 窄道里来回弹跳攒段数。
	//     下面那条"平均爬到多高"是配套的判据 —— 段数掉了 6.5 个百分点，而**高度不掉**
	//     （288 vs 无洞对照 288），所以掉的是"皱"不是"覆盖"。改动与标定表见
	//     `Docs/TinyGlade/Vine_ObstacleTurning.md` 与 `FParams::MaxTurn`。
	//   · 第一档的同一场景在解析上**不可能**超过 13% —— 它的规则是"藤脚在洞里就整根不长"，
	//     而自由墙脚只有 13%。所以 0.45 既在实测之下有余量，又远在第一档之上，不会两头都松。
	const float Kept = float(Holed.Branch.Num()) / FMath::Max(float(Clear.Branch.Num()), 1.0f);
	AddInfo(FString::Printf(TEXT("三拱全开保留率 %.3f（%d / %d 段）"),
		Kept, Holed.Branch.Num(), Clear.Branch.Num()));

	// 段数不是覆盖面：**同时报藤数与爬到的高度**，否则"段数掉了"分不清是"藤少了"还是
	// "藤不再皱了"。2026-09-12 那一轮正是后者 —— 拿掉洞缘上的镜像折角之后，原来靠在
	// 20 cm 窄道里来回弹跳攒出来的段没了，藤改成近乎直着爬上去，段数掉而高度不掉。
	auto TopZ = [](const CSHouseVine::FPlan& P)
	{
		float Sum = 0.0f;
		for (const CSHouseVine::FStrand& S : P.Strands)
		{
			float Top = 0.0f;
			for (const CSHouseVine::FStrandPoint& Pt : S.Points) Top = FMath::Max(Top, Pt.WallSZ.Y);
			Sum += Top;
		}
		return P.Strands.Num() > 0 ? Sum / float(P.Strands.Num()) : 0.0f;
	};
	AddInfo(FString::Printf(TEXT("藤数 %d / %d，平均爬到 %.0f / %.0f cm（墙高 %.0f）"),
		Holed.Strands.Num(), Clear.Strands.Num(), TopZ(Holed), TopZ(Clear), CSVineTest_Height));
	TestTrue(FString::Printf(TEXT("三个落地拱之下藤不会成片秃掉（保留 %.1f%%）"), Kept * 100.0f), Kept > 0.45f);
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 转角跨墙（TG 的 `check_for_wall_jump`）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineWallJumpTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineJumpsAtCorners",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineWallJumpTest::RunTest(const FString& Parameters)
{
	// 一圈四面墙，摆成一个真的矩形（这一条必须有真几何：跨墙那一段的法线取两面墙的平均，
	// 四面墙叠在一起的话这条断言恒真）。
	const double HX = 300.0, HY = 200.0, T = 24.0;
	auto MakeRing = [&](TArray<CSHouseVine::FWallStrip>& Out)
	{
		Out.Reset();
		const FVector2D Starts[4] = { {-HX, -HY}, {HX, -HY + T}, {HX, HY}, {-HX, HY - T} };
		const FVector2D Us[4] = { {1, 0}, {0, 1}, {-1, 0}, {0, -1} };
		const FVector2D Ns[4] = { {0, -1}, {1, 0}, {0, 1}, {-1, 0} };
		const double Lens[4] = { 2 * HX, 2 * HY - 2 * T, 2 * HX, 2 * HY - 2 * T };
		for (int32 E = 0; E < 4; ++E)
		{
			CSHouseVine::FWallStrip Strip;
			Strip.EdgeIndex = E;
			Strip.Origin = FVector(Starts[E].X, Starts[E].Y, 0.0);
			Strip.U = FVector(Us[E].X, Us[E].Y, 0.0);
			Strip.Up = FVector::UpVector;
			Strip.N = FVector(Ns[E].X, Ns[E].Y, 0.0);
			Strip.Length = float(Lens[E]);
			Strip.Height = CSVineTest_Height;
			Out.Add(Strip);
		}
	};

	TArray<CSHouseVine::FWallStrip> Strips;
	MakeRing(Strips);

	CSHouseVine::FParams Jump = CSVineTest_MakeParams();
	CSHouseVine::FParams NoJump = CSVineTest_MakeParams();
	NoJump.JumpChance = 0.0f;

	CSHouseVine::FPlan WithJump, Without;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Jump, WithJump);
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), NoJump, Without);

	// 跨墙那一段的截面法线是**两面墙法线的平均**（45°斜角），同墙段则逐位等于墙法线。
	// 数"斜角段"就能判定真的跨过墙了，而且这个量在关掉跨墙时必须恰好是 0。
	auto CountDiagonal = [](const CSHouseVine::FPlan& Plan)
	{
		int32 N = 0;
		for (const CSHouseVine::FRecord& R : Plan.Branch)
		{
			// 轴对齐的墙法线只有一个非零分量；平均出来的斜角两个分量都 ≈ 0.707。
			if (FMath::Abs(R.Normal.X) > 0.2f && FMath::Abs(R.Normal.Y) > 0.2f) ++N;
		}
		return N;
	};
	const int32 Diagonal = CountDiagonal(WithJump);
	AddInfo(FString::Printf(TEXT("跨墙段 %d / %d"), Diagonal, WithJump.Branch.Num()));
	TestTrue(FString::Printf(TEXT("开着跨墙时真的有藤绕过转角（%d 段）"), Diagonal), Diagonal > 0);
	TestEqual(TEXT("关掉跨墙就一段都不许跨"), CountDiagonal(Without), 0);

	// 跨墙不许把藤送出这一圈墙之外：所有段仍在 footprint 的外皮上（含 StandOff）。
	const double Limit = FMath::Max(HX, HY) + Jump.StandOff + 1.0;
	int32 OutOfRing = 0;
	for (const CSHouseVine::FRecord& R : WithJump.Branch)
	{
		if (FMath::Abs(R.WorldPos.X) > Limit || FMath::Abs(R.WorldPos.Y) > Limit) ++OutOfRing;
	}
	TestEqual(TEXT("跨墙以后藤仍然贴在这一圈墙上"), OutOfRing, 0);

	// 身份稳定性：跨不跨墙由身份哈希决定 ⇒ 两次规划仍必须逐位相同。
	CSHouseVine::FPlan Again;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Jump, Again);
	TestEqual(TEXT("开着跨墙时两次规划的段数仍相同"), Again.Branch.Num(), WithJump.Branch.Num());
	int32 Differ = 0;
	for (int32 I = 0; I < FMath::Min(Again.Branch.Num(), WithJump.Branch.Num()); ++I)
	{
		if (Again.Branch[I].Random01 != WithJump.Branch[I].Random01) ++Differ;
	}
	TestEqual(TEXT("开着跨墙时逐实例随机仍逐位相同"), Differ, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑨ 花（`ivy_flower`）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineFlowersTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineFlowers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineFlowersTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	CSHouseVine::FParams Params = CSVineTest_MakeParams();
	Params.FlowerChance = 1.0f;   // 概率钉死，判据才不会因为掷不到而空转

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
	TestTrue(FString::Printf(TEXT("开得出花（%d 朵）"), Plan.Flower.Num()), Plan.Flower.Num() > 0);
	TestTrue(TEXT("花比枝少（只开在藤的上半截）"), Plan.Flower.Num() < Plan.Branch.Num());

	// 花簇朝外上方张开、基准向量不与朝向共线 —— 后者共线的话 kernel 的退化判据会把它整批丢掉，
	// 而 counter 照样是对的（"实例数对得上但屏幕上什么都没有"）。
	int32 Degenerate = 0, PointingIn = 0, TooLow = 0;
	const float FromZ = float(FMath::CeilToInt(float(Params.MaxSegments) * Params.FlowerFromFrac)) * Params.SegmentLength
		* FMath::Cos(Params.MaxLean);
	for (const CSHouseVine::FRecord& R : Plan.Flower)
	{
		const FVector3f Dir = R.Dir.GetSafeNormal();
		const FVector3f Ref = R.Normal.GetSafeNormal();
		if (Dir.IsNearlyZero() || Ref.IsNearlyZero() || FMath::Abs(FVector3f::DotProduct(Dir, Ref)) > 0.999f) ++Degenerate;
		// 墙的外法线是 −Y，花该朝外 ⇒ Dir.Y 必须为负。
		if (Dir.Y > -0.3f) ++PointingIn;
		if (R.WorldPos.Z < FromZ - 0.001f) ++TooLow;
	}
	TestEqual(TEXT("没有一朵花的基与朝向共线（共线会被 kernel 静默丢掉）"), Degenerate, 0);
	TestEqual(TEXT("每一朵花都朝屋外张开"), PointingIn, 0);
	TestEqual(TEXT("没有花开在藤的下半截"), TooLow, 0);

	// 概率是 0 时一朵都不许有 —— 也是"没配花网格 ⇒ 不排花记录"那条的执行面。
	Params.FlowerChance = 0.0f;
	CSHouseVine::FPlan NoFlower;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, NoFlower);
	TestEqual(TEXT("概率为 0 时一朵花都没有"), NoFlower.Flower.Num(), 0);
	TestEqual(TEXT("关掉花不影响枝"), NoFlower.Branch.Num(), Plan.Branch.Num());

	// 花也避让墙洞（它挂在段末端上，段没了花自然没了）。
	TArray<FCSWallOpening> Openings;
	Openings.Add(CSVineTest_MakeDoor());
	Params.FlowerChance = 1.0f;
	CSHouseVine::FPlan Holed;
	CSHouseVine::BuildPlan(Strips, Openings, Params, Holed);
	int32 InHole = 0;
	for (const CSHouseVine::FRecord& R : Holed.Flower)
	{
		if (CSVineTest_InArchProfile(Openings[0], CSVineTest_ToWall(R.WorldPos))) ++InHole;
	}
	TestEqual(TEXT("门洞里一朵花都没有"), InHole, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ④ 容量上限是**纯配置量** —— 零阻塞纪律的可判定形式
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineCapacityBoundTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineFitsReservedCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineCapacityBoundTest::RunTest(const FString& Parameters)
{
	// `ACSHouseActor::EnsureVineComponents` 按 (周长 / 间距 + 4) × MaxSegments 一次预留容量，
	// 之后**永不扩容**。这条断言钉的就是那个上限真的是上限 —— 破了它的后果不是崩，
	// 而是交互期的某一帧突然付一次设备同步（或者藤被静默截断），两种都很难归因。
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();
	const double FootprintX = 600.0, FootprintY = 400.0, WallThickness = 24.0;

	TArray<CSHouseVine::FWallStrip> Strips;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		CSHouseVine::FWallStrip Strip = CSVineTest_MakeStrip(Edge);
		Strip.Length = float((Edge & 1) ? FootprintY - 2 * WallThickness : FootprintX);
		Strip.Height = CSVineTest_Height;
		// 四面墙摆在不同位置/朝向不影响计数，这里只关心长度。
		Strips.Add(Strip);
	}

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	const double Perimeter = 2.0 * (FootprintX + FootprintY);
	const int32 MaxStrands = FMath::CeilToInt(Perimeter / FMath::Max(Params.StrandSpacing, 20.0f)) + 4;
	// 解析上限（未量化）。`ACSHouseActor::EnsureVineComponents` 再把它过一遍
	// `CSShaperSteps::ReserveCount`（×1.5 对齐 4096）才交给 ReserveCapacity —— 那一步是为了
	// **拖尺寸时不重新分配**（上限是周长的连续函数），与这里要钉的"上限真的是上限"是两回事。
	const int32 Bound = MaxStrands * FMath::Clamp(Params.MaxSegments, 1, 128);

	TestTrue(FString::Printf(TEXT("枝装得下解析上限（%d ≤ %d）"), Plan.Branch.Num(), Bound),
		Plan.Branch.Num() <= Bound);
	TestTrue(FString::Printf(TEXT("叶装得下解析上限（%d ≤ %d）"), Plan.Leaf.Num(), Bound),
		Plan.Leaf.Num() <= Bound);
	// 上限也不该离谱地虚高：超过 4 倍就说明公式与规划走岔了，而症状不是崩 ——
	// 是显存白付、或者反过来（公式偏小）在某个尺寸上**静默截断**几段藤。
	TestTrue(FString::Printf(TEXT("上限没有离谱虚高（%d ≤ 4 × %d）"), Bound, Plan.Branch.Num()),
		Bound <= FMath::Max(Plan.Branch.Num(), 1) * 4);
	return true;
}

// -----------------------------------------------------------------------------
// (12) 折线与管子路径（2026-09-06 裁决 1/2：枝改扫掠管子、不分叉）
//
// ⚠️ 这一族钉的是 `PackTubePath` 交给 vinegenerator 的**缓冲格式**，而那份格式在 GPU 那一侧
// 出错时**不会有任何断言**：`PathPointMeta` 少填一半只是让切线在端点乱掉、`Axes` 没清零只是让
// 回退不触发、`SegmentMeta` 越界只是画出一条飞线。全是"画面不对而 readback 全绿"那一类。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineTubePathTest,
	"PCGPlugins.TinyGladeHouse.Vine.TubePath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineTubePathTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
	TestTrue(TEXT("规划出了折线"), Plan.Strands.Num() > 0);

	// (a) 不分叉：每根藤是一条链，点数 >= 2。
	for (const CSHouseVine::FStrand& S : Plan.Strands)
	{
		TestTrue(TEXT("每根藤至少两个点（单点连不成管子）"), S.Points.Num() >= 2);
	}

	const int32 Subdivide = 2;
	const float CircleScale = 1.0f;
	CSHouseVine::FTubePath Path;
	CSHouseVine::PackTubePath(Strips, Plan, Params, Subdivide, CircleScale, TArrayView<const float>(), Path);

	TestTrue(TEXT("打包出了点"), Path.Points.Num() > 0);
	// (b) 五条并行数组必须等长 —— 长度对不齐时 GPU 侧读的是别人的数据，没有任何检查会拦。
	TestEqual(TEXT("Axes 与 Points 等长"), Path.Axes.Num(), Path.Points.Num());
	TestEqual(TEXT("PointMeta 与 Points 等长"), Path.PointMeta.Num(), Path.Points.Num());
	TestEqual(TEXT("Growth 与 Points 等长"), Path.Growth.Num(), Path.Points.Num());
	TestEqual(TEXT("段数 = 点数 − 线数"), Path.SegmentMeta.Num(), Path.Points.Num() - Plan.Strands.Num());

	// (c) Axes 必须**逐位为零**：`BuildRawVoxelVineFrame` 的回退判据是 dot(Axis,Axis) > 1e-8，
	//     非零（比如池子里的旧内容）会让它误以为有轴可用，切线取到上一次的值。
	int32 NonZeroAxes = 0;
	for (const FVector4f& A : Path.Axes)
	{
		if (!FVector3f(A.X, A.Y, A.Z).IsNearlyZero()) ++NonZeroAxes;
	}
	TestEqual(TEXT("Axes 全零（回退判据的前提）"), NonZeroAxes, 0);

	// (d) Meta 的两套口径都要对：Pass C 读 .x/.y（prev/next），GetLinePointIndex 读 .z/.w（base/count）。
	int32 BadMeta = 0, BadRange = 0, BadSeg = 0;
	for (int32 i = 0; i < Path.PointMeta.Num(); ++i)
	{
		const FIntVector4& M = Path.PointMeta[i];
		const int32 Base = M.Z, Count = M.W;
		if (Base < 0 || Count < 2 || Base + Count > Path.Points.Num()) { ++BadRange; continue; }
		if (i < Base || i >= Base + Count) { ++BadRange; continue; }
		// prev/next 必须落在**本条线内**，且在端点处夹回自身（TG 的折线没有环，两端不外推）。
		const int32 ExpectPrev = FMath::Max(i - 1, Base);
		const int32 ExpectNext = FMath::Min(i + 1, Base + Count - 1);
		if (M.X != ExpectPrev || M.Y != ExpectNext) ++BadMeta;
	}
	TestEqual(TEXT("PointMeta 的 base/count 自洽"), BadRange, 0);
	TestEqual(TEXT("PointMeta 的 prev/next 夹在本线内"), BadMeta, 0);

	for (const FIntVector4& Seg : Path.SegmentMeta)
	{
		const bool bOk = Path.PointMeta.IsValidIndex(Seg.X) && Path.PointMeta.IsValidIndex(Seg.Y)
			&& Path.PointMeta[Seg.X].Z == Path.PointMeta[Seg.Y].Z;   // 同一条线
		if (!bOk) ++BadSeg;
	}
	TestEqual(TEXT("每段的两端同线且下标有效"), BadSeg, 0);

	// (e) 弧长必须**单调不减**且每条线从 0 起算 —— 生长动画的前沿直接拿它比。
	int32 BadArc = 0;
	for (int32 i = 0; i < Path.Growth.Num(); ++i)
	{
		const FIntVector4& M = Path.PointMeta[i];
		if (i == M.Z)
		{
			if (!FMath::IsNearlyZero(Path.Growth[i].Y)) ++BadArc;
		}
		else if (Path.Growth[i].Y < Path.Growth[i - 1].Y - UE_KINDA_SMALL_NUMBER)
		{
			++BadArc;
		}
	}
	TestEqual(TEXT("弧长逐线从 0 起、单调不减"), BadArc, 0);

	// (f) 半径缩放：Pass C 的环半径 = 10 * CircleScale * w，锥度从根到梢 1.0 -> 0.55。
	//     只查上下界，不查逐点 —— 逐点等于把 Lerp 抄一遍，那种测试只会锁死实现。
	const float RootR = Params.Thickness * 0.5f;
	int32 BadRadius = 0;
	for (const FVector4f& P : Path.Points)
	{
		const float R = 10.0f * CircleScale * P.W;
		if (R > RootR + 0.01f || R < RootR * 0.55f - 0.01f) ++BadRadius;
	}
	TestEqual(TEXT("逐点半径落在 [0.55R, R] 内"), BadRadius, 0);

	return true;
}

// -----------------------------------------------------------------------------
// (13) 细分不许把线拽离墙面 —— 「Catmull-Rom 是仿射组合、同墙共面」那条立论的可判定形式
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineTubeStaysOnWallTest,
	"PCGPlugins.TinyGladeHouse.Vine.TubeStaysOnWall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineTubeStaysOnWallTest::RunTest(const FString& Parameters)
{
	// **单面墙**：没有跨墙段 => 每一个细分点都必须落在那一个平面上。
	// 有跨墙段的场景不能用这条判据（转角本来就该切出去）。
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	CSHouseVine::FTubePath Path;
	CSHouseVine::PackTubePath(Strips, Plan, Params, 3, 1.0f, TArrayView<const float>(), Path);
	TestTrue(TEXT("打包出了点"), Path.Points.Num() > 0);

	// 墙面 = 过 (Origin + N*StandOff)、法线 N 的平面。细分后每个点到它的距离应当是 0。
	const CSHouseVine::FWallStrip& W = Strips[0];
	const FVector PlanePoint = W.Origin + W.N * Params.StandOff;
	double MaxOff = 0.0;
	for (const FVector4f& P : Path.Points)
	{
		MaxOff = FMath::Max(MaxOff,
			FMath::Abs(FVector::DotProduct(FVector(P.X, P.Y, P.Z) - PlanePoint, W.N)));
	}
	TestTrue(FString::Printf(TEXT("细分后仍贴墙（最大离面 %.4f cm <= 0.01）"), MaxOff), MaxOff <= 0.01);

	// 细分真的发生了：点数应当明显多于原始折线点数，而不是原样。
	int32 RawPoints = 0;
	for (const CSHouseVine::FStrand& S : Plan.Strands) RawPoints += S.Points.Num();
	TestTrue(FString::Printf(TEXT("细分确实加密了（%d -> %d）"), RawPoints, Path.Points.Num()),
		Path.Points.Num() > RawPoints);
	return true;
}

// -----------------------------------------------------------------------------
// (14) 折线与实例记录同源 —— 两处各写一份世界映射的话，症状是"藤和叶对不上"而两边各自自洽
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineStrandMatchesRecordsTest,
	"PCGPlugins.TinyGladeHouse.Vine.StrandMatchesRecords",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineStrandMatchesRecordsTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);

	// 折线的**段数**与枝记录数必须一致：两者是同一次游走的两种记法。
	int32 StrandSegments = 0;
	for (const CSHouseVine::FStrand& S : Plan.Strands) StrandSegments += S.Points.Num() - 1;
	TestEqual(TEXT("折线段数 = 枝记录数"), StrandSegments, Plan.Branch.Num());

	// 不细分时，折线的每个非末点映射回世界应当逐点落在对应枝记录的**起点**上。
	CSHouseVine::FTubePath Path;
	CSHouseVine::PackTubePath(Strips, Plan, Params, 0, 1.0f, TArrayView<const float>(), Path);
	int32 Mismatched = 0, RecordCursor = 0, PointBase = 0;
	for (const CSHouseVine::FStrand& S : Plan.Strands)
	{
		for (int32 i = 0; i + 1 < S.Points.Num(); ++i, ++RecordCursor)
		{
			if (!Plan.Branch.IsValidIndex(RecordCursor) || !Path.Points.IsValidIndex(PointBase + i))
			{
				++Mismatched;
				continue;
			}
			const FVector4f& P = Path.Points[PointBase + i];
			const FVector3f& Rec = Plan.Branch[RecordCursor].WorldPos;
			if ((FVector3f(P.X, P.Y, P.Z) - Rec).Size() > 0.01f) ++Mismatched;
		}
		PointBase += S.Points.Num();
	}
	TestEqual(TEXT("折线点与枝记录的世界位置逐点相同"), Mismatched, 0);

	// RootKey 必须与 SpawnTime 的键口径一致（不含位置、不含长宽）。
	int32 BadKey = 0;
	for (const CSHouseVine::FStrand& S : Plan.Strands)
	{
		if (S.RootKey != CSHouseVine::IdentityHash(S.RootEdgeIndex, S.StrandIndex, -1, 7u, Params.Seed)) ++BadKey;
	}
	TestEqual(TEXT("RootKey 与身份哈希口径一致"), BadKey, 0);
	return true;
}

// -----------------------------------------------------------------------------
// (15) 悬空不长藤（用户裁决 2026-09-06）
//
// 与承重柱互补：柱子在 Gap > PillarMinGap 处**出现**，藤在同处**消失**。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineNoVineWhenAirborneTest,
	"PCGPlugins.TinyGladeHouse.Vine.NoVineWhenAirborne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineNoVineWhenAirborneTest::RunTest(const FString& Parameters)
{
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();

	// (a) 没有采样 = 不知道 = 按贴地处理。这一条护着既有的十几条几何断言：
	//     它们用的墙都没有地面可采，缺省成"悬空"会让它们一次全红。
	{
		TArray<CSHouseVine::FWallStrip> Strips;
		Strips.Add(CSVineTest_MakeStrip());
		CSHouseVine::FPlan Plan;
		CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
		TestTrue(TEXT("没有地面采样时照常长藤（空数组 = 贴地）"), Plan.Strands.Num() > 0);
	}

	// (b) 整面墙悬空 => 一根都不长。
	{
		TArray<CSHouseVine::FWallStrip> Strips;
		CSHouseVine::FWallStrip S = CSVineTest_MakeStrip();
		S.GroundGaps.Init(Params.MaxGroundGap + 50.0f, 8);
		Strips.Add(S);
		CSHouseVine::FPlan Plan;
		CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
		TestEqual(TEXT("整面墙悬空时一根藤都没有"), Plan.Strands.Num(), 0);
		TestEqual(TEXT("连枝记录也没有"), Plan.Branch.Num(), 0);
	}

	// (c) 半边悬空 => 只在贴地那半边长。判据取**藤脚**的 S，不是整根。
	{
		TArray<CSHouseVine::FWallStrip> Strips;
		CSHouseVine::FWallStrip S = CSVineTest_MakeStrip();
		const int32 N = 9;
		S.GroundGaps.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i)
		{
			// 前半贴地、后半悬空。中点附近由线性插值过渡。
			S.GroundGaps[i] = (float(i) / float(N - 1) < 0.5f) ? 0.0f : Params.MaxGroundGap + 50.0f;
		}
		Strips.Add(S);
		CSHouseVine::FPlan Plan;
		CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
		TestTrue(TEXT("半边悬空时仍有藤"), Plan.Strands.Num() > 0);

		int32 OnAirborneHalf = 0;
		for (const CSHouseVine::FStrand& Strand : Plan.Strands)
		{
			// 藤脚的 S 落在悬空那半边（留一个插值过渡带的余量）就算越界。
			if (Strand.Points.Num() > 0 && Strand.Points[0].WallSZ.X > CSVineTest_Length * 0.6f) ++OnAirborneHalf;
		}
		TestEqual(TEXT("悬空那半边没有藤脚"), OnAirborneHalf, 0);
	}

	// (d) 阈值真的是阈值：刚好在阈下要长、阈上不长。
	{
		auto CountAt = [&Params](float Gap)
		{
			TArray<CSHouseVine::FWallStrip> Strips;
			CSHouseVine::FWallStrip S = CSVineTest_MakeStrip();
			S.GroundGaps.Init(Gap, 4);
			Strips.Add(S);
			CSHouseVine::FPlan Plan;
			CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
			return Plan.Strands.Num();
		};
		TestTrue(TEXT("阈下照常长"), CountAt(Params.MaxGroundGap - 1.0f) > 0);
		TestEqual(TEXT("阈上不长"), CountAt(Params.MaxGroundGap + 1.0f), 0);
	}
	return true;
}


// -----------------------------------------------------------------------------
// (16) 相邻段夹角有上界 —— 2026-09-12 那个画面缺陷的可判定形式
//
// 缺陷：旧算法在障碍处**直接改写**倾角（墙角 `Angle = -Angle`、洞里再 `Angle = 0`），
// 而倾角是相对竖直方向的**绝对**偏角 ⇒ 取反一次相邻段就折过 `2·|Angle|`，
// `MaxLean` 默认 1.15 rad 时上界是 **131.8°**：藤在门洞边缘和墙角上"折断"式急拐。
//
// ⚠️ **这一条只能在折线上断言**，不能在 `FParams` 上断言：`MaxTurn` 存在不等于它被执行了，
// 而"执行了"的唯一诚实证据就是逐段量出来的转折角。反过来，画面上这个缺陷极其显眼，
// 却不会让任何既有断言报红 —— 洞照样避开了、随机照样稳、藤照样贴在墙上。
//
// 反编译对照：TG 的 `ivy_grower` 里一次镜像/归零都没有，方向来自
// `IvyDirectionProposer::get_direction`（位置的连续函数，两层 FastNoise / 2.8 m 波长 /
// 0.21 m 步长）。详见 `Docs/TinyGlade/Vine_ObstacleTurning.md`。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineTurnRateTest,
	"PCGPlugins.TinyGladeHouse.Vine.TurnRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineTurnRateTest::RunTest(const FString& Parameters)
{
	// 一面 12 m 长的墙 + 两个落地拱 + 一个矩形窗。**墙刻意取长**（默认单测那面只有 6 m）：
	// 本条要量的是"相邻段"，而相邻段对数 ≈ 藤数 × 段数，短墙 + 密洞给不出统计量 ——
	// 演示用的 6 m 墙上开三个 150 宽的拱，含外扩之后自由墙脚只剩 13%，藤大半在根上就没了，
	// 只能量到三四十对，两条门槛都会变成噪声。洞之间留出宽走廊，藤才既长得起来又真撞得到洞。
	TArray<CSHouseVine::FWallStrip> Strips;
	{
		CSHouseVine::FWallStrip Wide = CSVineTest_MakeStrip();
		Wide.Length = 1200.0f;
		Strips.Add(Wide);
	}

	TArray<FCSWallOpening> Openings;
	Openings.Add(CSVineTest_MakeDoor(200.0f, 180.0f));
	Openings.Add(CSVineTest_MakeDoor(600.0f, 180.0f));
	{
		// 矩形窗：洞缘是**竖直**的两条线，正是"带着大倾角撞上去、一段之内换不了横移方向"
		// 那一档的最纯形态（拱的缘往上收，反而好绕）。
		FCSWallOpening Window;
		Window.Type = ECSOpeningType::Window;
		Window.Shape = ECSOpeningShape::Rect;
		Window.EdgeIndex = 0;
		Window.CenterS = 900.0f;
		Window.Width = 120.0f;
		Window.Z0 = 90.0f;
		Window.Z1 = 210.0f;
		Openings.Add(Window);
	}

	// 逐段量转折角。**在 (S, Z) 里量**而不是在世界里：跨墙那一段横跨两个墙平面，
	// 世界方向会额外转一个二面角（矩形房就是 90°），而 S 已经跳进隔壁墙的参数化里 ⇒
	// 涉及跨墙段的那一对本来就没有可比的角。本条的墙只有一面，所以过滤器不会真的丢掉
	// 任何一对，它是给将来四面墙的用例留的。
	struct FTurnStats
	{
		int32 Pairs = 0;
		int32 Over = 0;        // 折过 MaxTurn 的对数
		int32 InsideHole = 0;  // 落在洞形里的点数
		float Worst = 0.0f;    // 最大转折（弧度）
	};
	auto Measure = [&Strips, &Openings](const CSHouseVine::FParams& P, CSHouseVine::FPlan& OutPlan)
	{
		CSHouseVine::BuildPlan(Strips, Openings, P, OutPlan);
		FTurnStats Stats;
		for (const CSHouseVine::FStrand& Strand : OutPlan.Strands)
		{
			for (const CSHouseVine::FStrandPoint& Point : Strand.Points)
			{
				for (const FCSWallOpening& O : Openings)
				{
					if (CSVineTest_InArchProfile(O, FVector2D(Point.WallSZ.X, Point.WallSZ.Y)))
					{
						++Stats.InsideHole;
						break;
					}
				}
			}
			for (int32 I = 0; I + 2 < Strand.Points.Num(); ++I)
			{
				const CSHouseVine::FStrandPoint& P0 = Strand.Points[I];
				const CSHouseVine::FStrandPoint& P1 = Strand.Points[I + 1];
				const CSHouseVine::FStrandPoint& P2 = Strand.Points[I + 2];
				if (P0.EdgeIndex != P1.EdgeIndex || P1.EdgeIndex != P2.EdgeIndex) continue;

				const FVector2f D0 = P1.WallSZ - P0.WallSZ;
				const FVector2f D1 = P2.WallSZ - P1.WallSZ;
				if (D0.IsNearlyZero() || D1.IsNearlyZero()) continue;

				// 倾角 = 相对 +Z 的偏角，与 `BuildPlan` 里那个 `Angle` 同一口径。
				const float Turn = FMath::Abs(FMath::UnwindRadians(
					FMath::Atan2(D1.X, D1.Y) - FMath::Atan2(D0.X, D0.Y)));
				++Stats.Pairs;
				Stats.Worst = FMath::Max(Stats.Worst, Turn);
				if (Turn > P.MaxTurn + 0.001f) ++Stats.Over;
			}
		}
		return Stats;
	};

	const CSHouseVine::FParams Params = CSVineTest_MakeParams();
	CSHouseVine::FPlan Plan;
	const FTurnStats Base = Measure(Params, Plan);
	AddInfo(FString::Printf(TEXT("%d 根藤、%d 对相邻段，最大转折 %.1f°（上界 %.1f°）"),
		Plan.Strands.Num(), Base.Pairs, FMath::RadiansToDegrees(Base.Worst),
		FMath::RadiansToDegrees(Params.MaxTurn)));

	// 样本量：没有足够的相邻段对的话下面那条是空话。
	TestTrue(FString::Printf(TEXT("量到了足够多的相邻段对（%d）"), Base.Pairs), Base.Pairs > 100);
	TestEqual(FString::Printf(TEXT("没有一对相邻段折过 %.1f°（最大 %.1f°）"),
		FMath::RadiansToDegrees(Params.MaxTurn), FMath::RadiansToDegrees(Base.Worst)), Base.Over, 0);

	// 上界不许是靠"藤直接穿过障碍"换来的。
	TestEqual(TEXT("折线上没有一个点落在洞形里"), Base.InsideHole, 0);

	// ⓐ **反向门**：把预算放到 2 × MaxLean 以上（= 旧代码那句镜像的全部权限），同一场景
	//    量出来的折角必须**超过**默认上界。破了它就说明这个场景本来就没有大转向的需求，
	//    上面那条"没有一对折过 40.1°"是恒真的空话。
	//    （旧代码在同型场景里量出 100.4°，见 `Docs/TinyGlade/Vine_ObstacleTurning.md`。）
	{
		CSHouseVine::FParams Loose = CSVineTest_MakeParams();
		Loose.MaxTurn = Loose.MaxLean * 2.0f + 0.1f;
		CSHouseVine::FPlan LoosePlan;
		const FTurnStats Wide = Measure(Loose, LoosePlan);
		AddInfo(FString::Printf(TEXT("预算放开到 %.2f rad 时最大转折 %.1f°"),
			Loose.MaxTurn, FMath::RadiansToDegrees(Wide.Worst)));
		TestTrue(FString::Printf(TEXT("场景真的有大转向的需求（放开预算量到 %.1f° > %.1f°）"),
			FMath::RadiansToDegrees(Wide.Worst), FMath::RadiansToDegrees(Params.MaxTurn)),
			Wide.Worst > Params.MaxTurn);
	}

	// ⓑ `MaxTurn` 真的是那条闸：调小它，量出来的上界必须跟着下来。
	//    这一条挡的是"上界其实来自别的地方（比如 Wander），MaxTurn 根本没接上"。
	{
		CSHouseVine::FParams Tight = CSVineTest_MakeParams();
		Tight.MaxTurn = 0.15f;
		CSHouseVine::FPlan TightPlan;
		const FTurnStats Narrow = Measure(Tight, TightPlan);
		AddInfo(FString::Printf(TEXT("MaxTurn = 0.15 时最大转折 %.1f°"),
			FMath::RadiansToDegrees(Narrow.Worst)));
		TestEqual(FString::Printf(TEXT("调小 MaxTurn 上界跟着下来（%.1f° ≤ 8.6°）"),
			FMath::RadiansToDegrees(Narrow.Worst)), Narrow.Over, 0);
	}

	// ⓒ 同一份输入两次规划**逐位相同**。扫描本身不掷随机，跨墙掷也提到了扫描之外，
	//    所以这一条是可判定的 —— 破了它就说明有什么东西开始依赖"扫到第几档"（自指）。
	CSHouseVine::FPlan Again;
	CSHouseVine::BuildPlan(Strips, Openings, Params, Again);
	TestEqual(TEXT("两次规划的藤数相同"), Again.Strands.Num(), Plan.Strands.Num());
	int32 PointDiff = 0;
	for (int32 I = 0; I < FMath::Min(Again.Strands.Num(), Plan.Strands.Num()); ++I)
	{
		const CSHouseVine::FStrand& X = Again.Strands[I];
		const CSHouseVine::FStrand& Y = Plan.Strands[I];
		if (X.Points.Num() != Y.Points.Num() || X.RootKey != Y.RootKey) { ++PointDiff; continue; }
		for (int32 J = 0; J < X.Points.Num(); ++J)
		{
			// 逐位 —— 刻意不给容差：规划是纯函数，同一份输入的浮点结果必须一个 bit 都不差。
			if (X.Points[J].WallSZ.X != Y.Points[J].WallSZ.X
				|| X.Points[J].WallSZ.Y != Y.Points[J].WallSZ.Y
				|| X.Points[J].EdgeIndex != Y.Points[J].EdgeIndex) ++PointDiff;
		}
	}
	TestEqual(TEXT("两次规划的折线逐位相同"), PointDiff, 0);
	return true;
}

// -----------------------------------------------------------------------------
// (17) 管子变长不阻塞：常驻容量按台阶预留（2026-09-14 拖尺寸回归 `flushes=1` 的守门人）
//
// ⚠️ **本文件里碰 RHI 的一条**（`NonNullRHI`）。它钉的不是规划，而是 `CSVineTube::BuildTubeIntoMesh`
// 向常驻网格**要容量的方式**：按精确数要的话，管子只要比这张网格历史上最长的那根多出一个点，就付一次
// 重分配 + 拷贝的阻塞刷新。拖尺寸时藤的点数随 footprint 非单调地跳（5 cm 就可能多一段），于是"拖动的
// 第一帧阻塞一次"—— L_HouseGroundDemo 09-09 版实测：600×400 的管子 2472 顶点 / 14592 索引，605×400
// 要 2664 / 15744。演示回归那条 `flushes=0` 只在关卡恰好长出更长的管子时才红（当前关卡就不红），
// 这一条不依赖任何关卡内容。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineTubeRegrowthZeroFlushTest,
	"PCGPlugins.TinyGladeHouse.Vine.TubeRegrowthZeroFlush",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSHouseVineTubeRegrowthZeroFlushTest::RunTest(const FString& Parameters)
{
	TArray<CSHouseVine::FWallStrip> Strips;
	Strips.Add(CSVineTest_MakeStrip());
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();
	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
	if (!TestTrue(TEXT("规划出了折线"), Plan.Strands.Num() > 0)) return false;

	// 同一份规划、细分 2 → 3：第二根管子**严格更长**（点与段各多约三分之一），仍在台阶的 1.5 倍余量之内
	// —— 与"拖 5 cm 多出几段藤"同一个量级。CircleScale 取房子那条路的实际值（`CSHouseVine_TubeCircleScale`）。
	constexpr float CircleScale = 0.2f;
	CSHouseVine::FTubePath Short;
	CSHouseVine::FTubePath Long;
	CSHouseVine::PackTubePath(Strips, Plan, Params, 2, CircleScale, TArrayView<const float>(), Short);
	CSHouseVine::PackTubePath(Strips, Plan, Params, 3, CircleScale, TArrayView<const float>(), Long);

	// 反空判据：第二根不真的更长的话，下面那条"零阻塞"恒真，精确要容量的实现也会绿。
	if (!TestTrue(FString::Printf(TEXT("第二根管子的点更多（%d > %d）"), Long.Points.Num(), Short.Points.Num()),
		Long.Points.Num() > Short.Points.Num())) return false;
	if (!TestTrue(FString::Printf(TEXT("第二根管子的段更多（%d > %d）"), Long.SegmentMeta.Num(), Short.SegmentMeta.Num()),
		Long.SegmentMeta.Num() > Short.SegmentMeta.Num())) return false;

	UCSMesh* Mesh = NewObject<UCSMesh>(GetTransientPackage());
	if (!TestNotNull(TEXT("管子网格"), Mesh)) return false;
	CSVineTube::FParams TubeParams;
	TubeParams.CircleScale = CircleScale;

	auto Build = [&](const CSHouseVine::FTubePath& Path)
	{
		return CSVineTube::BuildTubeIntoMesh(Mesh, Path.Points, Path.Axes, Path.PointMeta, Path.SegmentMeta,
			Path.Growth, TubeParams, nullptr);
	};
	// 异步编辑的完成回调是一个游戏线程任务，只有 flush 泵得到它；在途时第二次递交会被直接拒掉。
	auto Settle = [&]()
	{
		for (int32 Pump = 0; Pump < 8 && Mesh->IsEditInFlight(); ++Pump) FlushRenderingCommands();
		return !Mesh->IsEditInFlight();
	};

	// ① 第一根：空网格第一次要容量本来就是阻塞的（分配），不在测量窗口里。
	if (!TestTrue(TEXT("第一根管子递交成功"), Build(Short))) return false;
	if (!TestTrue(TEXT("第一根管子的异步编辑已落地"), Settle())) return false;
	const int32 VertexCapacity = Mesh->GetVertexCapacity();
	const int32 IndexCapacity = Mesh->GetIndexCapacity();
	AddInfo(FString::Printf(TEXT("短管 %d 点 / %d 段，容量 %d 顶点 / %d 索引；长管 %d 点 / %d 段"),
		Short.Points.Num(), Short.SegmentMeta.Num(), VertexCapacity, IndexCapacity, Long.Points.Num(), Long.SegmentMeta.Num()));
	// 构建**完成回调**会按需缩容。它若按精确数缩，上面按台阶要足的余量建完就没了，下面那条"零阻塞"必红 ——
	// 09-14 第一版修复正是这么栽的（容量 4096 建完被缩回精确的 2496）。顶点容量只可能来自台阶，所以必是台阶的整数倍。
	TestEqual(TEXT("第一根管子建完之后顶点容量仍停在台阶上（完成回调没有缩回精确数）"),
		VertexCapacity % CSShaperSteps::CapacityStep, 0);

	// ② 第二根更长：**零阻塞、零重分配**。
	const int64 FlushesBefore = UCSMesh::GetBlockingFlushCount();
	const bool bIssued = Build(Long);
	const int64 Flushes = UCSMesh::GetBlockingFlushCount() - FlushesBefore;
	TestTrue(TEXT("第二根管子递交成功"), bIssued);
	TestEqual(TEXT("管子变长的那次重建不付阻塞刷新（容量按台阶预留，不按精确数要）"), Flushes, int64(0));
	TestEqual(TEXT("顶点容量没有被重新分配"), Mesh->GetVertexCapacity(), VertexCapacity);
	TestEqual(TEXT("索引容量没有被重新分配"), Mesh->GetIndexCapacity(), IndexCapacity);
	TestTrue(TEXT("第二根管子的异步编辑已落地"), Settle());
	return true;
}

// -----------------------------------------------------------------------------
// (18) 脚下没有地面的墙不长藤（用户裁决 2026-09-21）
//
// 收不到地面通知的藤直接清掉：`BuildVineStrips` 在无地面处写 `NoGroundGap`，走的是既有的
// "悬空不长藤"判据。两条：纯 CPU 那条钉住哨兵值与插值（∞ 会插出 NaN、NaN 不大于阈值 ⇒ 藤反而
// 长出来）；actor 那条钉住三种"房下什么都没有"——场景里没有地面、地面后出现、房子在地面范围外。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineNoGroundSentinelTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineSkipsWallsWithoutGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseVineNoGroundSentinelTest::RunTest(const FString& Parameters)
{
	const CSHouseVine::FParams Params = CSVineTest_MakeParams();
	const float NoGround = CSHouseVine::NoGroundGap;

	// 哨兵之间、哨兵与贴地之间的插值都必须仍然判成悬空，不能是 NaN。
	CSHouseVine::FWallStrip Probe = CSVineTest_MakeStrip();
	Probe.GroundGaps = { NoGround, NoGround };
	TestTrue(TEXT("两个无地面采样之间插值仍是悬空"), Probe.SampleGroundGap(CSVineTest_Length * 0.37f) > Params.MaxGroundGap);
	Probe.GroundGaps = { 0.0f, NoGround };
	TestTrue(TEXT("贴地 → 无地面之间插值是有限值"), FMath::IsFinite(Probe.SampleGroundGap(CSVineTest_Length * 0.5f)));

	auto CountStrands = [&Params](const TArray<float>& Gaps)
	{
		TArray<CSHouseVine::FWallStrip> Strips;
		Strips.Add(CSVineTest_MakeStrip());
		Strips[0].GroundGaps = Gaps;
		CSHouseVine::FPlan Plan;
		CSHouseVine::BuildPlan(Strips, TArray<FCSWallOpening>(), Params, Plan);
		return Plan.Strands.Num();
	};

	const int32 Grounded = CountStrands({ 0.0f, 0.0f, 0.0f, 0.0f });
	const int32 NoGroundAtAll = CountStrands({ NoGround, NoGround, NoGround, NoGround });
	const int32 HalfOnGround = CountStrands({ 0.0f, 0.0f, NoGround, NoGround });
	AddInfo(FString::Printf(TEXT("藤根数：贴地 %d / 半边无地面 %d / 全无地面 %d"), Grounded, HalfOnGround, NoGroundAtAll));
	TestTrue(TEXT("贴地的墙排得出藤（否则下面两条是空对空）"), Grounded > 2);
	TestEqual(TEXT("整面墙脚下都没有地面：一根藤都不长"), NoGroundAtAll, 0);
	TestTrue(TEXT("半边没有地面：只有贴地那半边长藤"), HalfOnGround > 0 && HalfOnGround < Grounded);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseVineNeedsGroundUnderneathTest,
	"PCGPlugins.ComputeShaderGenerator.House.VineNeedsGroundUnderneath",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSHouseVineNeedsGroundUnderneathTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// ① 场景里没有地面：以前按"不知道 = 贴地"照长，现在整栋按悬空。
	// 默认地面 64×64 格 × 50 cm，从原点铺到 (3200, 3200)；这栋房子等下会落在它正中。
	ACSHouseActor* Early = World->SpawnActor<ACSHouseActor>(FVector(1600.0, 1600.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("先于地面出现的房子"), Early)) return false;
	Early->FlushPendingReevaluate();
	TestTrue(TEXT("场景里没有地面：零藤是合法状态"), Early->IsVineSuppressedByGroundGap());

	// ② 地面后出现：它的全量重建要叫醒这栋还没拿到地面指针的房子，否则藤永远秃着。
	const int64 ReevaluatesBefore = Early->GetReevaluateCount();
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;   // 本用例只看高度场，石阶那条路不参与
	Ground->RebuildGroundMesh();
	TestTrue(TEXT("地面重建叫醒了还没找到地面的房子"), Early->IsReevaluatePending() || Early->GetReevaluateCount() > ReevaluatesBefore);
	Early->FlushPendingReevaluate();
	TestTrue(TEXT("房子确实重求值过"), Early->GetReevaluateCount() > ReevaluatesBefore);
	TestFalse(TEXT("地面就在脚下：藤回来了"), Early->IsVineSuppressedByGroundGap());

	// ③ 有地面，但房子在地面范围外：`SampleHeight` 会在这里垫一块假想平地，必须不被它骗。
	ACSHouseActor* OffGround = World->SpawnActor<ACSHouseActor>(FVector(20000.0, 20000.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("地面范围外的房子"), OffGround)) return false;
	OffGround->FlushPendingReevaluate();
	TestTrue(TEXT("地面范围外：脚下什么都没有，零藤"), OffGround->IsVineSuppressedByGroundGap());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

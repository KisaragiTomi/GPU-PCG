#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSHouseRoof.h"
#include "CSHouseTile.h"
#include "Math/NumericLimits.h"

// -----------------------------------------------------------------------------
// 屋面瓦的排布：不碰 RHI / world，只钉数学。
//
// 这一层是屋面唯一能被断言的地方 —— 到了 GPU 那一侧就只剩一个实例计数，"瓦有没有贴在屋面上"
// "四个坡面是不是都铺到了""角斜脊两侧对不对得齐"这三件事一个字都说不了。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo builds share a TU，file-local 一律 CSHouseTileTest_ 前缀。

FCSRoofDesc CSHouseTileTest_MakeRoof(double SizeX, double SizeY)
{
	FCSRoofDesc Desc;
	Desc.Footprint = FCSHouseFootprint::MakeRect(FVector2D(SizeX, SizeY));
	Desc.EaveZ = 300.0f;
	Desc.Pitch = 35.0f;
	Desc.Overhang = 25.0f;
	return Desc;
}

/** 抖动全关的参数：几何断言必须在**确定**的排布上做，抖动那几条另外单测。 */
CSHouseTile::FParams CSHouseTileTest_MakeParams()
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
	// ⚠️ **脊瓦在这份夹具里默认关掉。** 下面那几条用例统计的是"四个坡面各铺了什么"
	// （逐排高度对齐 / 每面瓦数相同 / 长宽比扫过正方形不跳变），而脊瓦沿的是**五条脊线**、
	// 不属于任何一个坡面 —— 混进同一个数组会让那些分类恒错（实测 11 排变 23、每面 107 变 95）。
	// 脊瓦另有专门的用例 `House.TileRidgeCaps`。这不是"调到能过"，是让每条用例只测它该测的东西。
	Params.RidgeCapScale = 0.0f;
	Params.Axes.UpSlope = 0;
	Params.Axes.AlongRow = 1;
	Params.Axes.Normal = 2;
	Params.Axes.NormalSign = 1.0f;
	Params.Axes.NativeSize = FVector3f(20.0f, 24.0f, 3.0f);
	return Params;
}

/** 瓦的三条轴，按**局部轴序**取出来（记录里就是这么存的）。 */
void CSHouseTileTest_Axes(const CSHouseTile::FRecord& R, FVector& OutX, FVector& OutY, FVector& OutZ)
{
	OutX = FVector(R.AxisX);
	OutY = FVector(R.AxisY);
	OutZ = FVector(R.AxisZ);
}
}

// -----------------------------------------------------------------------------
// ① 每一块瓦都**贴在屋面上**：位置落在高度场上、法线与求值器一致
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileOnRoofTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileOnRoof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileOnRoofTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);
	const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();

	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
	TestTrue(FString::Printf(TEXT("The roof gets tiled at all (%d tiles)"), Tiles.Num()), Tiles.Num() > 100);

	// StandOff / 抖动都关着 ⇒ 瓦心必须**逐点**落在高度场上。差一点就是"瓦浮在屋面上方
	// 或陷进阁楼里"，那是斜看才看得出来的一类错，靠出图很难抓。
	int32 OffSurface = 0, OutsideRoof = 0, BadNormal = 0, NotOrthonormal = 0, LeftHanded = 0;
	float WorstZ = 0.0f;
	for (const CSHouseTile::FRecord& R : Tiles)
	{
		const FVector2D XY(R.WorldPos.X, R.WorldPos.Y);
		const float WantZ = CSHouseRoof_EvalZ(Roof, XY);
		WorstZ = FMath::Max(WorstZ, FMath::Abs(float(R.WorldPos.Z) - WantZ));
		if (!FMath::IsNearlyEqual(float(R.WorldPos.Z), WantZ, 0.01f)) ++OffSurface;
		if (!CSHouseRoof_IsUnderRoof(Roof, XY)) ++OutsideRoof;

		FVector AxisX, AxisY, AxisZ;
		CSHouseTileTest_Axes(R, AxisX, AxisY, AxisZ);
		// 局部 Z 是法线轴（本用例的排列是恒等）。角斜脊上求值器给的是两面的平均，
		// 而瓦属于其中**一个**面 —— 那些点上不比对法线，只比对"朝上"。
		const FVector WantN = FVector(CSHouseRoof_EvalNormal(Roof, XY));
		if (AxisZ.Z <= 0.0) ++BadNormal;
		if (FVector::DotProduct(AxisZ, WantN) < 0.70) ++BadNormal;

		if (!FMath::IsNearlyEqual(AxisX.Size(), 1.0, 1.0e-3) || !FMath::IsNearlyEqual(AxisY.Size(), 1.0, 1.0e-3)
			|| !FMath::IsNearlyEqual(AxisZ.Size(), 1.0, 1.0e-3)
			|| FMath::Abs(FVector::DotProduct(AxisX, AxisY)) > 1.0e-3
			|| FMath::Abs(FVector::DotProduct(AxisY, AxisZ)) > 1.0e-3
			|| FMath::Abs(FVector::DotProduct(AxisX, AxisZ)) > 1.0e-3)
		{
			++NotOrthonormal;
		}
		// 左手基会让瓦整块**镜像**过去（背面朝外、光照翻掉），而实例数与位置断言全绿。
		if (FVector::DotProduct(FVector::CrossProduct(AxisX, AxisY), AxisZ) < 0.9) ++LeftHanded;
	}

	TestEqual(FString::Printf(TEXT("every tile sits exactly on the roof surface (worst %.3f cm)"), WorstZ), OffSurface, 0);
	TestEqual(TEXT("no tile lands outside the roof outline"), OutsideRoof, 0);
	TestEqual(TEXT("every tile faces the slope it belongs to"), BadNormal, 0);
	TestEqual(TEXT("every tile basis is orthonormal"), NotOrthonormal, 0);
	TestEqual(TEXT("every tile basis is right-handed (a mirrored tile renders inside out)"), LeftHanded, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ② 四个坡面都铺到了，而且铺**满**：从檐口外沿一直到脊
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileCoversAllSlopesTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileCoversAllSlopes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileCoversAllSlopesTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);
	const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();

	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);

	// 四个象限方向各数一份：+X / −X 那两片是四坡**独有**的（双坡时那儿是山墙），
	// 漏了它们的症状是"两头的屋顶是空的"，而总瓦数看着还挺多。
	int32 PerFace[4] = { 0, 0, 0, 0 };
	float LowestZ = TNumericLimits<float>::Max(), HighestZ = -TNumericLimits<float>::Max();
	for (const CSHouseTile::FRecord& R : Tiles)
	{
		const FVector2D XY(R.WorldPos.X, R.WorldPos.Y);
		const FVector2D Half = Roof.Footprint.GetBounds().Max;   // 居中矩形的半尺寸
		// 哪条边最近就属于哪个面（与高度场的 min 同一条判据）。
		const double D[4] = { Half.Y + XY.Y, Half.X - XY.X, Half.Y - XY.Y, Half.X + XY.X };
		int32 Best = 0;
		for (int32 I = 1; I < 4; ++I)
		{
			if (D[I] < D[Best]) Best = I;
		}
		++PerFace[Best];
		LowestZ = FMath::Min(LowestZ, float(R.WorldPos.Z));
		HighestZ = FMath::Max(HighestZ, float(R.WorldPos.Z));
	}
	for (int32 Side = 0; Side < 4; ++Side)
	{
		TestTrue(FString::Printf(TEXT("slope %d is tiled (%d tiles)"), Side, PerFace[Side]), PerFace[Side] > 10);
	}

	// 最低的一排在**檐口外沿**那一带（低于墙顶，因为外挑段继续往下走），
	// 最高的一排贴着脊 —— 两头都咬住，中间是等分的，所以整面必然铺满。
	const float EaveOuterZ = CSHouseRoof_EaveOuterZ(Roof);
	const float RidgeZ = CSHouseRoof_RidgeZ(Roof);
	const float RowRise = Roof.SinPitch() * 26.0f;   // 一排在竖直方向上抬多少（排距 × sin）
	TestTrue(FString::Printf(TEXT("the lowest course sits on the overhang (%.1f, eave outer %.1f, wall top %.1f)"),
		LowestZ, EaveOuterZ, Roof.EaveZ), LowestZ < Roof.EaveZ && LowestZ >= EaveOuterZ - 0.01f);
	TestTrue(FString::Printf(TEXT("the top course reaches the ridge (%.1f vs %.1f)"), HighestZ, RidgeZ),
		HighestZ > RidgeZ - RowRise - 0.01f && HighestZ <= RidgeZ + 0.01f);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 排在四个面上**对齐**，且瓦比间距大（相邻瓦互相压住，不留正缝）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileCoursesLineUpTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileCoursesLineUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileCoursesLineUpTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);
	const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();

	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);

	// 排高按 1 cm 量化收集：四个面共用同一份排数与排距 ⇒ 高度集合必须只有"排数"那么多个值。
	// 错开半排的症状是角斜脊上两侧的瓦犬牙交错，只有贴脸看才看得出来。
	TSet<int32> Courses;
	for (const CSHouseTile::FRecord& R : Tiles) Courses.Add(FMath::RoundToInt(R.WorldPos.Z));
	const float SlopeLen = (float(Roof.MaxInset()) + Roof.Overhang) / Roof.CosPitch();
	const int32 Rows = FMath::Max(1, FMath::RoundToInt(SlopeLen / 26.0f));
	TestEqual(FString::Printf(TEXT("all four slopes share the same %d courses"), Rows), Courses.Num(), Rows);

	// 瓦画多大 = 实际间距 × 重叠系数 ⇒ 每一块都必须比它占的那一格**大**。
	// 正缝会在每一道接缝上露出屋面底下的天空（TG 的砖也是故意胀大互穿的，同一条纪律）。
	const float RowStep = SlopeLen / float(Rows);
	int32 TooSmall = 0;
	for (const CSHouseTile::FRecord& R : Tiles)
	{
		// 排列是恒等：X = 上坡向、Y = 沿排。
		if (R.SizeX < RowStep * 1.05f) ++TooSmall;
		if (R.SizeY < 1.0f) ++TooSmall;
	}
	TestEqual(TEXT("every tile is drawn bigger than its slot (no positive seams)"), TooSmall, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ④ 正方形 → 金字塔；长宽比变化时瓦数连续变化（没有翻轴那种跳变）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTilePyramidTest,
	"PCGPlugins.ComputeShaderGenerator.House.TilePyramid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTilePyramidTest::RunTest(const FString& Parameters)
{
	const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();

	// 正方形：四个面全等，瓦数必须四等分（金字塔）。
	{
		const FCSRoofDesc Square = CSHouseTileTest_MakeRoof(400.0, 400.0);
		TArray<CSHouseTile::FRecord> Tiles;
		CSHouseTile::BuildPlan(Square, FTransform::Identity, Params, Tiles);
		TestTrue(FString::Printf(TEXT("a square roof still gets tiled (%d)"), Tiles.Num()), Tiles.Num() > 50);

		int32 PerFace[4] = { 0, 0, 0, 0 };
		for (const CSHouseTile::FRecord& R : Tiles)
		{
			const FVector2D Half = Square.Footprint.GetBounds().Max;
			const double D[4] = { Half.Y + R.WorldPos.Y, Half.X - R.WorldPos.X,
				Half.Y - R.WorldPos.Y, Half.X + R.WorldPos.X };
			int32 Best = 0;
			for (int32 I = 1; I < 4; ++I)
			{
				if (D[I] < D[Best]) Best = I;
			}
			++PerFace[Best];
		}
		for (int32 Side = 1; Side < 4; ++Side)
		{
			TestEqual(TEXT("the pyramid's four slopes carry the same number of tiles"), PerFace[Side], PerFace[0]);
		}
	}

	// 长宽比连续扫过正方形：瓦数不许出现跳变。**这就是"四坡没有翻轴事件"的可断言形态** ——
	// 双坡那套一旦翻轴，脊与山墙原地转 90°，任何按面统计的量都会在那一步断掉。
	{
		int32 Previous = 0, WorstJump = 0;
		for (int32 Step = 0; Step <= 20; ++Step)
		{
			const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(400.0, 360.0 + double(Step) * 4.0);
			TArray<CSHouseTile::FRecord> Tiles;
			CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
			if (Step > 0) WorstJump = FMath::Max(WorstJump, FMath::Abs(Tiles.Num() - Previous));
			Previous = Tiles.Num();
		}
		// 一步 4 cm 最多让排数涨一格（多铺一整圈课），几十块的量级；上百就说明形态跳了 ——
		// 双坡那套一翻轴，脊与山墙原地转 90°，按面统计的量会在那一步直接掉一半。
		TestTrue(FString::Printf(TEXT("sweeping through square changes the tile count smoothly (worst step %d)"), WorstJump),
			WorstJump < 80);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 身份稳定：随机只由 (面, 排, 列, 种子) 决定，**不含位置**
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileIdentityTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileIdentityTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);
	CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();
	Params.ScaleJitter = 0.08f;
	Params.YawJitter = 0.05f;
	Params.LiftJitter = 0.7f;

	TArray<CSHouseTile::FRecord> A, B, Moved;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, A);
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, B);
	// 同一份世界状态两次规划必须**逐位**相同 —— 差一位就是"重建一次全场变样"。
	TestEqual(TEXT("two plans agree on the tile count"), B.Num(), A.Num());
	int32 Differ = 0;
	for (int32 Index = 0; Index < FMath::Min(A.Num(), B.Num()); ++Index)
	{
		if (A[Index].Random01 != B[Index].Random01 || A[Index].WorldPos != B[Index].WorldPos) ++Differ;
	}
	TestEqual(TEXT("two plans agree bit for bit"), Differ, 0);

	// 把房子搬走 + 转个角：位置当然会变，但**逐实例随机一位都不许变**。
	// 身份里含位置的话，拖房子时整片屋顶会不停重掷（藤蔓那条纪律的同一个执行面）。
	const FTransform World(FRotator(0.0, 37.0, 0.0), FVector(1200.0, -800.0, 55.0));
	CSHouseTile::BuildPlan(Roof, World, Params, Moved);
	TestEqual(TEXT("moving the house does not change the tile count"), Moved.Num(), A.Num());
	int32 RandomDiffer = 0, PositionSame = 0;
	for (int32 Index = 0; Index < FMath::Min(A.Num(), Moved.Num()); ++Index)
	{
		if (A[Index].Random01 != Moved[Index].Random01) ++RandomDiffer;
		if (A[Index].WorldPos == Moved[Index].WorldPos) ++PositionSame;
	}
	TestEqual(TEXT("moving the house re-rolls nothing"), RandomDiffer, 0);
	TestEqual(TEXT("...but the tiles really did move (otherwise the assertion above is vacuous)"), PositionSame, 0);

	// 换种子必须真的换一批随机数（不然上面那条"不变"是恒真的）。
	Params.Seed = 99;
	TArray<CSHouseTile::FRecord> Reseeded;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Reseeded);
	int32 SeedDiffer = 0;
	for (int32 Index = 0; Index < FMath::Min(A.Num(), Reseeded.Num()); ++Index)
	{
		if (A[Index].Random01 != Reseeded[Index].Random01) ++SeedDiffer;
	}
	TestTrue(FString::Printf(TEXT("a new seed really re-rolls (%d of %d)"), SeedDiffer, A.Num()),
		SeedDiffer > A.Num() / 2);
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 容量上限：真的是上限，而且没有离谱虚高
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileFitsBoundTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileFitsBound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileFitsBoundTest::RunTest(const FString& Parameters)
{
	// 覆盖面就是这条断言的价值：容量按上限一次付清、之后**永不扩容**，破了它的后果不是崩，
	// 而是交互期的某一帧突然截断半面屋顶（或者反过来白付显存）。
	struct FCase { double SizeX, SizeY; float Pitch, Overhang, RowPitch, ColumnPitch; const TCHAR* What; };
	const FCase Cases[] = {
		{  600, 400, 35.0f, 25.0f, 26.0f, 30.0f, TEXT("demo size") },
		{  400, 600, 35.0f, 25.0f, 26.0f, 30.0f, TEXT("demo size, long axis Y") },
		{  400, 400, 20.0f,  0.0f, 26.0f, 30.0f, TEXT("square, zero overhang") },
		{ 1000, 320, 15.0f, 60.0f, 40.0f, 45.0f, TEXT("shallow, deep overhang, coarse tiles") },
		{  260, 250, 70.0f, 25.0f, 12.0f, 12.0f, TEXT("near square, steep, fine tiles") },
		{  200, 200, 45.0f, 90.0f, 26.0f, 30.0f, TEXT("tiny footprint under a huge overhang") },
	};

	for (const FCase& Case : Cases)
	{
		FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(Case.SizeX, Case.SizeY);
		Roof.Pitch = Case.Pitch;
		Roof.Overhang = Case.Overhang;
		CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();
		Params.RowPitch = Case.RowPitch;
		Params.ColumnPitch = Case.ColumnPitch;

		TArray<CSHouseTile::FRecord> Tiles;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
		const int32 Bound = CSHouseTile::MaxTilesBound(Roof, Params);

		TestTrue(FString::Printf(TEXT("[%s] the tiles fit the analytic bound (%d <= %d)"), Case.What, Tiles.Num(), Bound),
			Tiles.Num() <= Bound);
		// 虚高的后果不是崩，是白付显存 —— 4 倍是"梯形 vs 外接矩形"那点差距的宽松上界。
		TestTrue(FString::Printf(TEXT("[%s] the bound is not absurdly high (%d <= 4 x %d)"), Case.What, Bound, Tiles.Num()),
			Bound <= FMath::Max(Tiles.Num(), 1) * 4);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 轴排列：换一张轴向不同的瓦资产时，方向必须搬到对应的局部轴上，且基仍是右手
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileMeshAxesTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileMeshAxes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileMeshAxesTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);

	CSHouseTile::FParams Identity = CSHouseTileTest_MakeParams();
	TArray<CSHouseTile::FRecord> Base;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Identity, Base);

	// 局部 X = 沿排、局部 Y = 上坡向、局部 Z = 法线：一次**对换**（奇排列）⇒ 基会变成左手的，
	// 实现必须在沿排轴上补一个负号掰回来。挑这个排列就是为了让那条补偿真的被执行到。
	CSHouseTile::FParams Permuted = Identity;
	Permuted.Axes.UpSlope = 1;
	Permuted.Axes.AlongRow = 0;
	Permuted.Axes.Normal = 2;
	Permuted.Axes.NativeSize = FVector3f(24.0f, 20.0f, 3.0f);   // 跟着排列一起换
	TArray<CSHouseTile::FRecord> Swapped;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Permuted, Swapped);

	TestEqual(TEXT("the permutation does not change how many tiles there are"), Swapped.Num(), Base.Num());

	int32 WrongSlot = 0, LeftHanded = 0;
	for (int32 Index = 0; Index < FMath::Min(Base.Num(), Swapped.Num()); ++Index)
	{
		const CSHouseTile::FRecord& A = Base[Index];
		const CSHouseTile::FRecord& B = Swapped[Index];
		// 恒等排列下：X = 上坡、Y = 沿排、Z = 法线。换到 (上坡→Y, 沿排→X, 法线→Z) 之后
		// 那三条**世界方向**必须原样搬进新槽位；沿排那条被手性补了个负号，所以取绝对值比。
		if (!FVector(B.AxisY).Equals(FVector(A.AxisX), 1.0e-3)) ++WrongSlot;
		if (!FVector(B.AxisZ).Equals(FVector(A.AxisZ), 1.0e-3)) ++WrongSlot;
		if (FMath::Abs(FVector::DotProduct(FVector(B.AxisX), FVector(A.AxisY))) < 0.999) ++WrongSlot;
		// 尺寸同样跟着槽位走。
		if (!FMath::IsNearlyEqual(B.SizeY, A.SizeX, 1.0e-3f)) ++WrongSlot;
		if (!FMath::IsNearlyEqual(B.SizeX, A.SizeY, 1.0e-3f)) ++WrongSlot;

		if (FVector::DotProduct(FVector::CrossProduct(FVector(B.AxisX), FVector(B.AxisY)), FVector(B.AxisZ)) < 0.9)
		{
			++LeftHanded;
		}
	}
	TestEqual(TEXT("每条世界方向都搬进了它该在的局部轴"), WrongSlot, 0);
	TestEqual(TEXT("奇排列之后基仍然是右手的（否则整片瓦会镜像过去）"), LeftHanded, 0);

	// 排列填错（两轴撞号）时不许产出奇异基：兜底会退回 (0, 1, 2)，画得对不对另说，至少画得出来。
	CSHouseTile::FParams Broken = Identity;
	Broken.Axes.UpSlope = 1;
	Broken.Axes.AlongRow = 1;
	TArray<CSHouseTile::FRecord> Fallback;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Broken, Fallback);
	int32 Degenerate = 0;
	for (const CSHouseTile::FRecord& R : Fallback)
	{
		const double Det = FVector::DotProduct(
			FVector::CrossProduct(FVector(R.AxisX), FVector(R.AxisY)), FVector(R.AxisZ));
		if (FMath::Abs(Det) < 0.9) ++Degenerate;
	}
	TestTrue(TEXT("a broken axis permutation still produces tiles"), Fallback.Num() > 0);
	TestEqual(TEXT("a broken axis permutation never produces a singular basis"), Degenerate, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑧ 脊瓦：四条角斜脊 + 一条屋脊，骑在接缝上遮住两坡的对切口
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileRidgeCapsTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileRidgeCaps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileRidgeCapsTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);

	CSHouseTile::FParams NoCaps = CSHouseTileTest_MakeParams();      // RidgeCapScale = 0
	CSHouseTile::FParams WithCaps = NoCaps;
	WithCaps.RidgeCapScale = 1.15f;

	TArray<CSHouseTile::FRecord> Plain, Capped;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, NoCaps, Plain);
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, WithCaps, Capped);

	const int32 CapCount = Capped.Num() - Plain.Num();
	AddInfo(FString::Printf(TEXT("坡面瓦 %d，脊瓦 %d"), Plain.Num(), CapCount));
	TestTrue(FString::Printf(TEXT("脊瓦真的产出了（%d 块）"), CapCount), CapCount > 0);
	// 关掉脊瓦不许影响坡面那一批：脊瓦是**追加**的，不是重排。
	for (int32 Index = 0; Index < Plain.Num(); ++Index)
	{
		if (!Capped[Index].WorldPos.Equals(Plain[Index].WorldPos, 0.01f))
		{
			TestTrue(TEXT("开脊瓦不改动坡面那一批瓦（脊瓦是追加，不是重排）"), false);
			break;
		}
	}

	// ⓐ 每一块脊瓦的基都必须右手 —— 镜像基会让瓦背面朝外、光照整个翻掉，而位置与数量全绿。
	int32 Mirrored = 0;
	for (int32 Index = Plain.Num(); Index < Capped.Num(); ++Index)
	{
		const CSHouseTile::FRecord& R = Capped[Index];
		if (FVector3f::DotProduct(FVector3f::CrossProduct(R.AxisX, R.AxisY), R.AxisZ) <= 0.0f) ++Mirrored;
	}
	TestEqual(TEXT("每块脊瓦的基都是右手的"), Mirrored, 0);

	// ⓑ 脊瓦必须**骑在**屋脊或角斜脊上：法线在两坡法线之间 ⇒ 竖直分量比任一坡面的都大。
	//    单坡的法线竖直分量 = cos(pitch)；角平分之后一定更接近竖直。
	const float CosP = Roof.CosPitch();
	int32 TooFlat = 0;
	for (int32 Index = Plain.Num(); Index < Capped.Num(); ++Index)
	{
		if (Capped[Index].AxisZ.Z < CosP - 1e-3f) ++TooFlat;
	}
	TestEqual(FString::Printf(TEXT("脊瓦的法线比坡面更竖直（cos(pitch)=%.4f）"), CosP), TooFlat, 0);

	// ⓒ 正方形（金字塔）没有屋脊，只剩四条角斜脊 ⇒ 脊瓦仍然有，但比矩形少。
	{
		const FCSRoofDesc Square = CSHouseTileTest_MakeRoof(500.0, 500.0);
		TArray<CSHouseTile::FRecord> SqPlain, SqCapped;
		CSHouseTile::BuildPlan(Square, FTransform::Identity, NoCaps, SqPlain);
		CSHouseTile::BuildPlan(Square, FTransform::Identity, WithCaps, SqCapped);
		const int32 SqCaps = SqCapped.Num() - SqPlain.Num();
		AddInfo(FString::Printf(TEXT("金字塔的脊瓦 %d 块"), SqCaps));
		TestTrue(FString::Printf(TEXT("金字塔仍然有四条角斜脊的盖瓦（%d 块）"), SqCaps), SqCaps > 0);
	}

	// ⓓ 容量上界必须把脊瓦算进去 —— 漏算的症状是"屋脊末端少几块"，而瓦数、零阻塞全绿。
	TestTrue(FString::Printf(TEXT("MaxTilesBound 覆盖脊瓦（bound %d ≥ 实产 %d）"),
		CSHouseTile::MaxTilesBound(Roof, WithCaps), Capped.Num()),
		CSHouseTile::MaxTilesBound(Roof, WithCaps) >= Capped.Num());

	// TG hip caps are one narrow slab along the bisector, not two wide leaves across it.
	TArray<const CSHouseTile::FRecord*> Hips, Slabs;
	for (int32 Index = Plain.Num(); Index < Capped.Num(); ++Index)
	{
		if (Capped[Index].AxisZ.Z > 0.999f) Slabs.Add(&Capped[Index]);
		else Hips.Add(&Capped[Index]);
	}
	TestEqual(TEXT("four hips, each with five slabs for this 600x400 fixture"), Hips.Num(), 20);
	for (const CSHouseTile::FRecord* H : Hips)
	{
		TestTrue(TEXT("hip slab width stays 25 cm independent of tile pitch"), FMath::IsNearlyEqual(H->SizeY, 25.0f, 0.01f));
		TestTrue(TEXT("hip normal bisects the adjacent face normals"), H->AxisZ.Z > Roof.CosPitch() + 0.01f);
		TestTrue(TEXT("hip slab length axis runs uphill along the hip"), FMath::Abs(H->AxisX.Z) > 0.1f);
		TestEqual(TEXT("hip slabs have no tile-end lift or clipping"), H->ClipPlane.X, 0.0f);
	}

	// ⓔ 水平屋脊（TG `assemble_rectangular_roof`，2026-09-29）：600 × 400 的脊从 (−100, 0) 到 (100, 0)，
	//    分点 round(200 / 62.5) = 3 个 ⇒ 两块平躺的板，长轴沿脊、骑在脊上，按段长 × 1.4 首尾相压盖满整条脊。
	const float RidgeZ = CSHouseRoof_RidgeZ(Roof);
	TestEqual(TEXT("the level ridge carries one flat slab per piece"), Slabs.Num(), 2);
	float SlabMin = TNumericLimits<float>::Max(), SlabMax = TNumericLimits<float>::Lowest();
	for (const CSHouseTile::FRecord* S : Slabs)
	{
		TestTrue(TEXT("a ridge slab lies along the ridge"), FMath::Abs(S->AxisX.X) > 0.999f);
		TestTrue(TEXT("a ridge slab sits on the ridge line"), FMath::Abs(S->WorldPos.Y) < 0.01f && S->WorldPos.Z >= RidgeZ - 0.01f);
		SlabMin = FMath::Min(SlabMin, S->WorldPos.X - S->SizeX * 0.5f);
		SlabMax = FMath::Max(SlabMax, S->WorldPos.X + S->SizeX * 0.5f);
	}
	TestTrue(FString::Printf(TEXT("the slabs cover the whole ridge (%.1f .. %.1f)"), SlabMin, SlabMax), SlabMin <= -100.0f && SlabMax >= 100.0f);
	if (Slabs.Num() == 2)
	{
		const CSHouseTile::FRecord* Lo = Slabs[0]->WorldPos.X < Slabs[1]->WorldPos.X ? Slabs[0] : Slabs[1];
		const CSHouseTile::FRecord* Hi = Lo == Slabs[0] ? Slabs[1] : Slabs[0];
		TestTrue(TEXT("neighbouring ridge slabs overlap"), Lo->WorldPos.X + Lo->SizeX * 0.5f > Hi->WorldPos.X - Hi->SizeX * 0.5f);
	}

	// 回归用户截图里的斜脊被盖住：真实资产厚 13 cm / 中心 Z=1.5，材质去掉烘焙翘端后
	// 盖瓦只有 10 cm 基础板厚，坡面瓦另抬端 6 cm。检查最终顶面，不能只断言实例中心在脊线上。
	for (const float Pitch : { 20.0f, 35.0f, 55.0f })
	{
		FCSRoofDesc ActualRoof = Roof;
		ActualRoof.Pitch = Pitch;
		CSHouseTile::FParams Actual;
		Actual.RowPitch = Actual.ColumnPitch = 45.0f;
		Actual.Axes.UpSlope = 1;
		Actual.Axes.AlongRow = 0;
		Actual.Axes.NativeSize = FVector3f(49.7064f, 64.0f, 13.0f);
		Actual.Axes.NativeCentre = FVector3f(0.0f, 0.0f, 1.5f);
		TArray<CSHouseTile::FRecord> ActualTiles;
		CSHouseTile::BuildPlan(ActualRoof, FTransform::Identity, Actual, ActualTiles);
		int32 Checked = 0, Buried = 0, Floating = 0;
		const double BodyTop = Actual.StandOff + Actual.LiftJitter - 1.5 + 5.0 + 6.0;
		double LowestRidgeTop = TNumericLimits<double>::Max();
		double HighestHipTop = TNumericLimits<double>::Lowest();
		for (const CSHouseTile::FRecord& R : ActualTiles)
		{
			if (R.ClipPlane.X > 0.0f) continue;
			const FVector HalfSlab = FVector(R.AxisZ) * (5.0 * R.SizeZ / 13.0);
			const FVector Top = FVector(R.WorldPos) + HalfSlab;
			if (R.AxisZ.Z > 0.999f)
			{
				LowestRidgeTop = FMath::Min(LowestRidgeTop, Top.Z);
				continue;
			}
			++Checked;
			HighestHipTop = FMath::Max(HighestHipTop, Top.Z + FMath::Abs(double(R.AxisY.Z)) * R.SizeY * 0.5);
			const FVector Bottom = FVector(R.WorldPos) - HalfSlab;
			const double TopAboveRoof = (Top.Z - CSHouseRoof_EvalZ(ActualRoof, FVector2D(Top.X, Top.Y))) * ActualRoof.CosPitch();
			const double BottomAboveRoof = (Bottom.Z - CSHouseRoof_EvalZ(ActualRoof, FVector2D(Bottom.X, Bottom.Y))) * ActualRoof.CosPitch();
			if (TopAboveRoof <= BodyTop) ++Buried;
			if (BottomAboveRoof >= BodyTop) ++Floating;
		}
		TestTrue(TEXT("clearance check includes all four hips"), Checked >= 4);
		TestEqual(FString::Printf(TEXT("%.0f degree hip tops clear the raised body tiles"), Pitch), Buried, 0);
		TestEqual(FString::Printf(TEXT("%.0f degree hip bases still overlap the tile layer"), Pitch), Floating, 0);
		TestTrue(FString::Printf(TEXT("%.0f degree horizontal ridge covers the hip endpoints"), Pitch), LowestRidgeTop > HighestHipTop);
	}

	return true;
}

// -----------------------------------------------------------------------------
// 裁剪面随位置/yaw变换；保留本坡，切掉穿过角脊的部分。无需扩大盖瓦去遮交叉瓦端。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileClipPlanesTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileClipPlanes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileClipPlanesTest::RunTest(const FString& Parameters)
{
	const FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(600.0, 400.0);
	const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();
	const FTransform Moved(FRotator(0.0, 73.0, 0.0), FVector(12345.0, -6789.0, 91.0));
	TArray<CSHouseTile::FRecord> Local, World;
	CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Local);
	CSHouseTile::BuildPlan(Roof, Moved, Params, World);
	TestEqual(TEXT("moving a roof preserves tile count"), Local.Num(), World.Num());
	int32 Invalid = 0, LostCentre = 0, WrongTransform = 0, NoCut = 0;
	for (int32 I = 0; I < Local.Num(); ++I)
	{
		const auto Eval = [](const CSHouseTile::FRecord& R, const FVector& P)
		{
			const double Angle = double(R.ClipPlane.X) - 4.0;
			return FMath::Cos(Angle) * P.X + FMath::Sin(Angle) * P.Y + double(R.ClipPlane.Y);
		};
		const auto& L = Local[I];
		const auto& W = World[I];
		if (L.ClipPlane.X <= 0.0f || W.ClipPlane.X <= 0.0f) ++Invalid;
		const double Distance = Eval(L, FVector(L.WorldPos));
		if (Distance > 0.01) ++LostCentre;
		if (!FMath::IsNearlyEqual(Distance, Eval(W, FVector(W.WorldPos)), 0.02)) ++WrongTransform;
		const double Angle = double(L.ClipPlane.X) - 4.0;
		const FVector Across = FVector(L.WorldPos) + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.0) * (2.0 - Distance);
		if (Eval(L, Across) < 1.99 || Eval(W, Moved.TransformPosition(Across)) < 1.97) ++NoCut;
	}
	TestEqual(TEXT("all slope tiles carry a clip plane"), Invalid, 0);
	TestEqual(TEXT("clipping retains the tile centre"), LostCentre, 0);
	TestEqual(TEXT("clip distance is invariant under house yaw and translation"), WrongTransform, 0);
	TestEqual(TEXT("the opposite side of each hip is discarded"), NoCut, 0);
	return true;
}

// ⑨ 折线屋面（footprint 3e）：六边形 / 不规则五边形上，瓦照样贴面、铺满每个坡面、装得进上界
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTilePolylineRoofTest,
	"PCGPlugins.ComputeShaderGenerator.House.TilePolylineRoof",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTilePolylineRoofTest::RunTest(const FString& Parameters)
{
	TArray<FVector2D> Hexagon;
	for (int32 i = 0; i < 6; ++i)
	{
		const double A = UE_DOUBLE_TWO_PI * double(i) / 6.0;
		Hexagon.Add(FVector2D(320.0 * FMath::Cos(A), 320.0 * FMath::Sin(A)));
	}
	struct FCase { const TCHAR* What; TArray<FVector2D> Verts; };
	const FCase Cases[] = {
		{ TEXT("hexagon"), Hexagon },
		{ TEXT("irregular pentagon"), { FVector2D(-320, -180), FVector2D(260, -220), FVector2D(380, 60),
			FVector2D(40, 290), FVector2D(-290, 150) } },
	};

	for (const FCase& C : Cases)
	{
		FCSRoofDesc Roof = CSHouseTileTest_MakeRoof(1.0, 1.0);
		Roof.Footprint.Verts = C.Verts;
		const int32 NumFaces = Roof.Footprint.NumEdges();

		const CSHouseTile::FParams Params = CSHouseTileTest_MakeParams();
		TArray<CSHouseTile::FRecord> Tiles;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, Params, Tiles);
		TestTrue(FString::Printf(TEXT("[%s] the roof gets tiled (%d)"), C.What, Tiles.Num()), Tiles.Num() > 100);

		TArray<int32> PerFace;
		PerFace.SetNumZeroed(NumFaces);
		int32 OffSurface = 0, OutsideRoof = 0, BadNormal = 0, LeftHanded = 0;
		for (const CSHouseTile::FRecord& R : Tiles)
		{
			const FVector2D XY(R.WorldPos.X, R.WorldPos.Y);
			if (!FMath::IsNearlyEqual(float(R.WorldPos.Z), CSHouseRoof_EvalZ(Roof, XY), 0.01f)) ++OffSurface;
			if (!CSHouseRoof_IsUnderRoof(Roof, XY)) ++OutsideRoof;
			if (FVector::DotProduct(FVector(R.AxisZ), CSHouseRoof_EvalNormal(Roof, XY)) < 0.70) ++BadNormal;
			if (FVector3f::DotProduct(FVector3f::CrossProduct(R.AxisX, R.AxisY), R.AxisZ) < 0.9f) ++LeftHanded;

			int32 Best = 0;
			double BestD = TNumericLimits<double>::Max();
			for (int32 Face = 0; Face < NumFaces; ++Face)
			{
				const FCSHouseEdgeFrame F = CSHouse_GetEdge(Face, Roof.Footprint, 0.0f);
				const double D = FVector2D::DotProduct(XY - F.Start, F.In);
				if (D < BestD) { BestD = D; Best = Face; }
			}
			++PerFace[Best];
		}
		TestEqual(FString::Printf(TEXT("[%s] every tile sits on the roof surface"), C.What), OffSurface, 0);
		TestEqual(FString::Printf(TEXT("[%s] no tile lands outside the eave outline"), C.What), OutsideRoof, 0);
		TestEqual(FString::Printf(TEXT("[%s] every tile faces its slope"), C.What), BadNormal, 0);
		TestEqual(FString::Printf(TEXT("[%s] every tile basis is right-handed"), C.What), LeftHanded, 0);
		for (int32 Face = 0; Face < NumFaces; ++Face)
		{
			TestTrue(FString::Printf(TEXT("[%s] slope %d is tiled (%d)"), C.What, Face, PerFace[Face]), PerFace[Face] > 5);
		}

		// 脊瓦沿骨架的每条弧：右手基、比坡面更竖直、容量上界盖得住。
		CSHouseTile::FParams WithCaps = Params;
		WithCaps.RidgeCapScale = 1.15f;
		TArray<CSHouseTile::FRecord> Capped;
		CSHouseTile::BuildPlan(Roof, FTransform::Identity, WithCaps, Capped);
		const int32 Caps = Capped.Num() - Tiles.Num();
		TestTrue(FString::Printf(TEXT("[%s] ridge caps exist (%d)"), C.What, Caps), Caps > NumFaces);
		int32 CapMirrored = 0, CapTooFlat = 0;
		for (int32 Index = Tiles.Num(); Index < Capped.Num(); ++Index)
		{
			const CSHouseTile::FRecord& R = Capped[Index];
			if (FVector3f::DotProduct(FVector3f::CrossProduct(R.AxisX, R.AxisY), R.AxisZ) <= 0.0f) ++CapMirrored;
			if (R.AxisZ.Z < Roof.CosPitch() - 1.0e-3f) ++CapTooFlat;
		}
		TestEqual(FString::Printf(TEXT("[%s] every ridge cap basis is right-handed"), C.What), CapMirrored, 0);
		TestEqual(FString::Printf(TEXT("[%s] every ridge cap rides a ridge (steeper normal than a slope)"), C.What), CapTooFlat, 0);
		const int32 Bound = CSHouseTile::MaxTilesBound(Roof, WithCaps);
		TestTrue(FString::Printf(TEXT("[%s] tiles + caps fit the bound (%d <= %d)"), C.What, Capped.Num(), Bound), Capped.Num() <= Bound);
		TestTrue(FString::Printf(TEXT("[%s] the bound is not absurdly high (%d <= 4 x %d)"), C.What, Bound, Capped.Num()),
			Bound <= Capped.Num() * 4);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

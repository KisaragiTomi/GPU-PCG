#include "CSGroundActor.h"
#include "CSGroundRockShell.h"
#include "CSGroundShaperActor.h"
#include "CSGroundShaperField.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Math/RandomStream.h"
#include "Misc/AutomationTest.h"
#include "EditorAssetLibrary.h"
#include "MeshDescription.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_EDITOR

/**
 * 披挂岩壳（计划 D9 链 B，2026-09-30 起是稀疏程序化版本）的验收：
 *
 *   ① `RockShell.Contract`               —— 纯 CPU：默认值之间承重的不等式 + 固定拓扑模板的自洽。
 *   ② `RockShell.DrapesOnSlopes`         —— 陡坡上出现 / 平地上不出现 / 高度跟住解析场 / **它真的画得出来**。
 *   ③ `RockShell.RoadSinksNotHides`      —— 画路之后壳**连续下沉**而不是消失；路缘是缓坡不是折痕。
 *   ④ `RockShell.CapSkirtSurvivesBake`   —— 盖 / 裙顶点色烘成 StaticMesh 之后还在。
 *   ⑤ `RockShell.CellPolygon`            —— 纯 CPU：胞腔多边形恰好划分平面（每个点只属于它的 Voronoi 主人）。
 *   ⑥ `RockShell.SparseFarCorner`        —— 512 m 地面的远角落照样长石头（原件那一张 tile 做不到）。
 *   ⑦ `RockShell.SparseScalesWithContent`—— 零塑形物零胞腔；同一塑形物在 64 m / 512 m 地面上激活与位置逐位相同。
 *   ⑧ `RockShell.ActivationConservative` —— 暴力扫一遍：凡是坡度过阈的点，它所在的胞腔一定被激活了。
 *
 * ⚠️ ② 里有一条 `IsRockShellDrawable`：readback 证明的是"buffer 里有数"，对"画的是哪张网格、有没有
 * 材质"一个字都没说（石阶那个坑：StairMesh 一直是 NULL，画面是一撮黑块，单测全绿）。
 */
namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSRockShellTest_ 前缀。

/** 地面：128 格 × 50 cm = 64 m。中心 3200，塑形物影响半径 1400 ⇒ 内外都留得下平地。 */
constexpr int32 CSRockShellTest_Cells = 128;
constexpr float CSRockShellTest_CellSize = 50.0f;
constexpr double CSRockShellTest_Centre = CSRockShellTest_Cells * CSRockShellTest_CellSize * 0.5;

/**
 * 与演示关卡同一档：裙边 8 m 装得下胞腔；max|∇h| = Lift × 1.5 / Falloff = 1.3125 过得了 0.75 的坡度软阈。
 * 三个数字是一条链，单改其中一个会把本文件的断言拆掉。
 */
constexpr float CSRockShellTest_Radius = 600.0f;
constexpr float CSRockShellTest_Falloff = 800.0f;
constexpr float CSRockShellTest_Lift = 700.0f;
constexpr double CSRockShellTest_MaxSlope = double(CSRockShellTest_Lift) * 1.5 / double(CSRockShellTest_Falloff);

ACSGroundActor* CSRockShellTest_SpawnGround(UWorld* World, UMaterialInterface* ShellMaterial, int32 Cells = CSRockShellTest_Cells)
{
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!Ground) return nullptr;
	Ground->NumCellsX = Cells;
	Ground->NumCellsY = Cells;
	Ground->CellSize = CSRockShellTest_CellSize;
	// 石阶关掉：本文件量的是岩壳，多一条 GPU 路只会多一份噪声与耗时。
	Ground->StairMesh = nullptr;
	Ground->bRockShell = true;
	Ground->RockShellMaterial = ShellMaterial;
	Ground->RebuildGroundMesh();
	return Ground;
}

ACSGroundShaperActor* CSRockShellTest_SpawnMound(UWorld* World, double X, double Y,
	float Lift = CSRockShellTest_Lift, float Radius = CSRockShellTest_Radius, float Falloff = CSRockShellTest_Falloff, float Bulge = 0.0f)
{
	ACSGroundShaperActor* Shaper = World->SpawnActor<ACSGroundShaperActor>(FVector(X, Y, 0.0), FRotator::ZeroRotator);
	if (!Shaper) return nullptr;
	Shaper->MaskSize = 2.0f * Radius;   // 没设 mask = 解析圆盘，直径 MaskSize
	Shaper->FalloffDistance = Falloff;
	Shaper->CenterBulge = Bulge;
	// 台顶 = actor 高度；乘 (1 + 二次抬升) 保持与旧口径逐项相同的剖面。负的 Lift = 坑。
	Shaper->SetActorLocation(FVector(X, Y, double(Lift) * (Lift > 0.0f ? (1.0 + double(Shaper->SecondaryLiftScale)) : 1.0)));
	Shaper->RebuildTerrain();
	return Shaper;
}

/** 沿直线落 N 笔。⚠️ 笔刷是 3D 球，坡面上必须自己 SampleHeight 贴地，否则整段落空。 */
void CSRockShellTest_PaintLine(ACSGroundActor* Ground, double X0, double Y0, double X1, double Y1, int32 Dabs)
{
	Ground->BeginPaintStroke();
	for (int32 Index = 0; Index <= Dabs; ++Index)
	{
		const double T = double(Index) / double(FMath::Max(Dabs, 1));
		const double X = FMath::Lerp(X0, X1, T);
		const double Y = FMath::Lerp(Y0, Y1, T);
		Ground->ApplyPaintStroke(FVector(X, Y, Ground->SampleHeight(FVector2D(X, Y))));
	}
	Ground->EndPaintStroke();   // 内部 FlushPaintToGpu(true) → RebuildStairs() → RebuildRockShell()
}

/**
 * 三角活着吗。⚠️ **判据是"第 0 个顶点不是 NaN"**：kernel 关掉一个三角时只写第 0 个顶点一个 NaN
 * （同 TG），另外两个顶点原样留着上一次的值；空槽建池时也按这个约定标死。
 */
bool CSRockShellTest_TriangleAlive(const TArray<FVector>& Positions, int32 Tri)
{
	const int32 Base = Tri * 3;
	return Positions.IsValidIndex(Base + 2) && !Positions[Base].ContainsNaN();
}

/**
 * 活三角三个顶点到 Centre 的最近 / 最远水平距离。显隐是**逐三角**「任一顶点 Rock > 0.1」（TG 原样），
 * 跨在坡脚上的三角另外两个顶点可以落在平地上 ⇒ "平地上不长"要按**整个三角**判，不能只看第 0 个顶点
 * （中心扇的第 0 个顶点是质心，胞腔一大就离坡带很远）。
 */
void CSRockShellTest_TriRadiusRange(const TArray<FVector>& Positions, int32 Tri, const FVector2D& Centre, double& OutMin, double& OutMax)
{
	OutMin = TNumericLimits<double>::Max();
	OutMax = 0.0;
	for (int32 K = 0; K < 3; ++K)
	{
		const FVector& P = Positions[Tri * 3 + K];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		OutMin = FMath::Min(OutMin, R);
		OutMax = FMath::Max(OutMax, R);
	}
}

/** 材质：引擎基础材质 —— 判据是"非空"，不是"哪一张"，所以不碰项目资产。 */
UMaterialInterface* CSRockShellTest_LoadShellMaterial()
{
	return LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
}

/** 池里一共多少个三角（= 容量 × 5M；回读的顶点数 / 3 应当与它相等）。 */
int32 CSRockShellTest_PoolTriangles(const ACSGroundActor* Ground)
{
	int32 Active = 0, Capacity = 0, PerCell = 0;
	return Ground->GetRockShellPoolStats(Active, Capacity, PerCell) ? Capacity * PerCell : 0;
}

int32 CSRockShellTest_ActiveCells(const ACSGroundActor* Ground)
{
	int32 Active = 0, Capacity = 0, PerCell = 0;
	Ground->GetRockShellPoolStats(Active, Capacity, PerCell);
	return Active;
}

/** 点 P（世界 XY）的 Voronoi / 功率图主人：暴力扫本格周围 9×9 个种子。 */
FIntPoint CSRockShellTest_Owner(const CSRockShell::FPatternParams& P, const FVector2D& GridOrigin, const FVector2D& Point)
{
	const double S = double(P.CellSize);
	const int32 I0 = FMath::FloorToInt32((Point.X - GridOrigin.X) / S);
	const int32 J0 = FMath::FloorToInt32((Point.Y - GridOrigin.Y) / S);
	FIntPoint Best(I0, J0);
	double BestD = TNumericLimits<double>::Max();
	for (int32 Dj = -4; Dj <= 4; ++Dj)
	{
		for (int32 Di = -4; Di <= 4; ++Di)
		{
			const FVector2D Seed = CSRockShell::SeedWorld(P, GridOrigin, I0 + Di, J0 + Dj);
			const double D = FVector2D::DistSquared(Seed, Point) - double(CSRockShell::SeedWeight(P, I0 + Di, J0 + Dj));
			if (D < BestD) { BestD = D; Best = FIntPoint(I0 + Di, J0 + Dj); }
		}
	}
	return Best;
}

/** 点在逆时针凸多边形内（边上算在内，Eps 向外放宽）。 */
bool CSRockShellTest_InsideConvex(const TArray<FVector2f>& Poly, const FVector2f& Point, float Eps)
{
	for (int32 K = 0; K < Poly.Num(); ++K)
	{
		const FVector2f A = Poly[K];
		const FVector2f E = Poly[(K + 1) % Poly.Num()] - A;
		const float L = E.Size();
		if (L < 1e-6f) continue;
		// 逆时针 ⇒ 内部在左侧：cross(E, P − A) ≥ 0
		if ((E.X * (Point.Y - A.Y) - E.Y * (Point.X - A.X)) / L < -Eps) return false;
	}
	return true;
}
}

// -----------------------------------------------------------------------------
// ① 默认值之间那几条承重的不等式 + 固定拓扑模板的自洽（纯 CPU）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellContractTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.Contract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRockShellContractTest::RunTest(const FString&)
{
	const ACSGroundActor* CDO = GetDefault<ACSGroundActor>();

	// 岩壳默认是开的，而且开关不是"材质为空"（石阶那个坑）。
	TestTrue(TEXT("岩壳默认开着"), CDO->bRockShell);
	// 坡度软阈：Hi 必须严格大于 Lo，否则 smoothstep 是 0/0 ⇒ 整片 mask 变 NaN。
	TestTrue(TEXT("坡度软阈 Hi > Lo"), CDO->RockShellSlopeHi > CDO->RockShellSlopeLo);

	// **壳与石阶严格互补**：road = 1/RoadFade 是壳眼里的足迹边缘，石阶出现的 StairRoadThreshold 必须落在足迹之内。
	const float ShellFootprintAt = 1.0f / FMath::Max(CDO->RockShellRoadFade, 1.0f);
	TestTrue(
		FString::Printf(TEXT("壳的路足迹(%.3f) 早于石阶出现(%.3f)"), ShellFootprintAt, CDO->StairRoadThreshold),
		ShellFootprintAt < CDO->StairRoadThreshold);

	// 下沉量必须盖得住壳自身的起伏（竖向四项是世界 cm，属性值就是实际值）。
	const float OwnRelief = CDO->RockShellCellRelief + CDO->RockShellNoiseAmount
		+ CDO->RockShellChipAmount + CDO->RockShellBaseLift;
	TestTrue(
		FString::Printf(TEXT("下沉量 %.0f cm 盖得住壳自身起伏 %.0f cm"), CDO->RockShellRoadSink, OwnRelief),
		CDO->RockShellRoadSink > OwnRelief);

	// 厚度层不是可选项：调到 0，披挂出来的就只是一张贴着地形的毯子。
	TestTrue(TEXT("沿法线的厚度层是开着的"), CDO->RockShellCellRelief > 0.0f);

	// 用户 2026-09-30 定：程序化图案、M = 16；胞腔与 08-31 的 0.35 同一观感。
	TestEqual(TEXT("每胞腔边界点 M = 16（用户 2026-09-30 定）"), CDO->RockShellBoundaryPoints, 16);
	TestTrue(FString::Printf(TEXT("默认胞腔格距 = 0.35 × TG 原件（%.1f cm）"), CDO->RockShellCellSize),
		FMath::IsNearlyEqual(CDO->RockShellCellSize, 0.35f * CSRockShell::TGNativeCellSpacingCm, 1.0f));
	TestEqual(TEXT("加性磕碰默认关（TG 原样）"), CDO->RockShellChipAmount, 0.0f);

	// --- 固定拓扑模板 ---
	for (const int32 M : { CSRockShell::MinBoundaryPoints, 16, CSRockShell::MaxBoundaryPoints })
	{
		const CSRockShell::FTemplate& Tmpl = CSRockShell::GetTemplate(M);
		const int32 Tris = 5 * M;
		if (!TestEqual(FString::Printf(TEXT("M=%d：邻接表 3 × 5M"), M), Tmpl.Neighbours.Num(), Tris * 3)) continue;
		TestEqual(FString::Printf(TEXT("M=%d：入射范围表 2 × 15M"), M), Tmpl.IncidentRange.Num(), Tris * 3 * 2);

		int32 Outline = 0;
		bool bSymmetric = true;
		bool bOutlineIsBottom = true;
		for (int32 T = 0; T < Tris; ++T)
		{
			for (int32 K = 0; K < 3; ++K)
			{
				const int32 Nb = Tmpl.Neighbours[T * 3 + K];
				if (Nb < 0)
				{
					++Outline;
					const uint32 A = CSRockShell::FTemplate::CornerVertex(M, T, (K + 1) % 3);
					const uint32 B = CSRockShell::FTemplate::CornerVertex(M, T, (K + 2) % 3);
					bOutlineIsBottom &= CSRockShell::FTemplate::VertexKind(M, A) == 3 && CSRockShell::FTemplate::VertexKind(M, B) == 3;
					continue;
				}
				bool bBack = false;
				for (int32 K2 = 0; K2 < 3; ++K2) bBack |= Tmpl.Neighbours[Nb * 3 + K2] == T;
				bSymmetric &= bBack;
			}
		}
		// 每个边界点一条底边 ⇒ 恰好 M 条外轮廓；其余边都在石头内部、互相配对。
		TestEqual(FString::Printf(TEXT("M=%d：外轮廓边恰好 M 条"), M), Outline, M);
		TestTrue(FString::Printf(TEXT("M=%d：外轮廓边全是底圈边（石头之间没有邻接）"), M), bOutlineIsBottom);
		TestTrue(FString::Printf(TEXT("M=%d：邻接对称"), M), bSymmetric);

		// 入射表：每个角的范围里的三角都真的含这个模板顶点。
		bool bIncidentOk = true;
		for (int32 Corner = 0; Corner < Tris * 3; ++Corner)
		{
			const uint32 V = CSRockShell::FTemplate::CornerVertex(M, Corner / 3, Corner % 3);
			const uint32 Offset = Tmpl.IncidentRange[Corner * 2 + 0];
			const uint32 Count = Tmpl.IncidentRange[Corner * 2 + 1];
			for (uint32 I = 0; I < Count; ++I)
			{
				const int32 T = int32(Tmpl.IncidentTris[int32(Offset + I)]);
				bool bHas = false;
				for (int32 K = 0; K < 3; ++K) bHas |= CSRockShell::FTemplate::CornerVertex(M, T, K) == V;
				bIncidentOk &= bHas;
			}
		}
		TestTrue(FString::Printf(TEXT("M=%d：入射表自洽"), M), bIncidentOk);

		// 绕序：在一个规则的平胞腔上，全部 5M 个三角的 cross(P2−P0, P1−P0) 都朝 +Z（引擎正面 + TG 的面法线口径）。
		auto Pos = [M](uint32 V) -> FVector2f
		{
			const int32 Kind = CSRockShell::FTemplate::VertexKind(M, V);
			if (Kind == 0) return FVector2f::ZeroVector;
			const int32 Index = Kind == 1 ? int32(V) - 1 : (Kind == 2 ? int32(V) - 1 - M : int32(V) - 1 - 2 * M);
			const float R = Kind == 1 ? 45.0f : (Kind == 2 ? 90.0f : 100.0f);
			const float A = 2.0f * PI * float(Index) / float(M);
			return FVector2f(FMath::Cos(A), FMath::Sin(A)) * R;
		};
		int32 Up = 0;
		for (int32 T = 0; T < Tris; ++T)
		{
			const FVector2f P0 = Pos(CSRockShell::FTemplate::CornerVertex(M, T, 0));
			const FVector2f P1 = Pos(CSRockShell::FTemplate::CornerVertex(M, T, 1));
			const FVector2f P2 = Pos(CSRockShell::FTemplate::CornerVertex(M, T, 2));
			const float Z = (P2.X - P0.X) * (P1.Y - P0.Y) - (P2.Y - P0.Y) * (P1.X - P0.X);
			Up += Z > 0.0f;
		}
		TestEqual(FString::Printf(TEXT("M=%d：模板三角全部俯视顺时针（面法线朝上）"), M), Up, Tris);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ② 陡坡上出现 / 平地上不出现 / 高度跟住解析场 / 它真的画得出来
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellDrapeTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.DrapesOnSlopes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellDrapeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	if (!TestNotNull(TEXT("岩壳材质"), ShellMaterial)) return false;

	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	// ⚠️ **披挂契约要在「隆起中性」下量**：隆起 / 基准偏移 / 扩张是**有意**把壳抬离地面的，
	// 与本用例要钉的「壳贴着坡面、偏差只等于壳自身厚度」正好相反 —— 两件事分开测。
	const float SavedRiseMultiplier = Ground->RockShellRiseMultiplier;
	const float SavedRiseExtend = Ground->RockShellRiseExtend;
	const float SavedRiseNoise = Ground->RockShellRiseNoiseAmount;
	const float SavedCellExpand = Ground->RockShellCellExpand;
	const float SavedCellSize = Ground->RockShellCellSize;
	const float SavedReliefFloor = Ground->RockShellReliefFloor;
	const float SavedBaseLift = Ground->RockShellBaseLift;
	const float SavedBaseSink = Ground->RockShellBaseSink;
	Ground->RockShellBaseLift = 0.0f;
	Ground->RockShellBaseSink = 0.0f;
	Ground->RockShellRiseMultiplier = 1.0f;
	Ground->RockShellRiseExtend = 0.0f;
	Ground->RockShellRiseNoiseAmount = 0.0f;
	Ground->RockShellCellExpand = 0.0f;
	// 胞腔也归中性（TG 原生 553.1 cm ⇒ 胞腔缩放 = 1）：下面的容差是在原生密度下标定的，
	// 而它量的是几千个采样的**最大值** —— 密度越高、同一份镜像重建误差取到的最大值越差。
	Ground->RockShellCellSize = CSRockShell::TGNativeCellSpacingCm;
	Ground->RockShellReliefFloor = 0.0f;
	Ground->RebuildHeightsFromShapers();

	// 披挂 pass 必须真的跑过 —— 否则下面读到的只是建池时那份死三角。
	TestTrue(FString::Printf(TEXT("披挂 pass 跑过（%d 次）"), Ground->GetRockShellDisplaceCount()),
		Ground->GetRockShellDisplaceCount() > 0);

	int32 ActiveCells = 0, CapacityCells = 0, TrisPerCell = 0;
	if (!TestTrue(TEXT("槽位池已建"), Ground->GetRockShellPoolStats(ActiveCells, CapacityCells, TrisPerCell))) return false;
	AddInfo(FString::Printf(TEXT("槽位池：活跃 %d / 容量 %d 槽 × %d 三角"), ActiveCells, CapacityCells, TrisPerCell));
	TestEqual(TEXT("每槽 5M 个三角"), TrisPerCell, 5 * Ground->RockShellBoundaryPoints);
	TestTrue(TEXT("土台激活了胞腔"), ActiveCells > 0);

	// **它真的画得出来**：组件注册 / 可见、网格绑定、常驻流已分配、包围盒有效、材质非空。
	FString Reason;
	TestTrue(FString::Printf(TEXT("岩壳会被画出来（%s）"), *Reason), Ground->IsRockShellDrawable(Reason));

	TArray<FVector> Positions;
	const int32 Count = Ground->DebugReadRockShellSync(Positions);
	const int32 Triangles = CSRockShellTest_PoolTriangles(Ground);
	AddInfo(FString::Printf(TEXT("回读 %d 个岩壳顶点"), Count));
	if (!TestEqual(TEXT("回读到的顶点数 = 池容量 × 5M × 3"), Count, Triangles * 3)) return false;

	// 分三个环带统计：台顶（平）/ 裙边（陡）/ 盘外远处（平）。
	const FVector2D Centre(CSRockShellTest_Centre, CSRockShellTest_Centre);
	int32 LiveOnPlateau = 0, LiveOnSkirt = 0, LiveOnFlat = 0, LiveTotal = 0;
	double WorstDrapeError = 0.0;
	int32 DrapeSamples = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Positions, Tri)) continue;
		++LiveTotal;
		const FVector& P = Positions[Tri * 3];
		const double R = FVector2D::Distance(FVector2D(P.X, P.Y), Centre);
		double RMin = 0.0, RMax = 0.0;
		CSRockShellTest_TriRadiusRange(Positions, Tri, Centre, RMin, RMax);
		if (RMax < CSRockShellTest_Radius * 0.6) ++LiveOnPlateau;
		else if (RMin > CSRockShellTest_Radius + CSRockShellTest_Falloff + 400.0) ++LiveOnFlat;
		else if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff) ++LiveOnSkirt;

		// 披挂必须贴着解析坡面（裁决三的构造保证）。量的是**到坡面的垂直距离**，不是 Z 差：
		// 壳沿地形法线推出一个厚度，法线在陡坡上是斜的 ⇒ 顶点 XY 也跑了，除以 sqrt(1+|∇h|²) 把那一项消掉。
		if (DrapeSamples < 4000 && R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
		{
			const double D = double(Ground->CellSize);
			const double Field = Ground->SampleHeight(FVector2D(P.X, P.Y));
			const double Gx = (Ground->SampleHeight(FVector2D(P.X + D, P.Y)) - Ground->SampleHeight(FVector2D(P.X - D, P.Y))) / (2.0 * D);
			const double Gy = (Ground->SampleHeight(FVector2D(P.X, P.Y + D)) - Ground->SampleHeight(FVector2D(P.X, P.Y - D))) / (2.0 * D);
			const double Perp = FMath::Abs(double(P.Z) - Field) / FMath::Sqrt(1.0 + Gx * Gx + Gy * Gy);
			WorstDrapeError = FMath::Max(WorstDrapeError, Perp);
			++DrapeSamples;
		}
	}
	AddInfo(FString::Printf(TEXT("活着的三角 %d：台顶 %d / 裙边 %d / 盘外远处 %d；到坡面的最大垂直距离 %.1f cm（%d 个采样）"),
		LiveTotal, LiveOnPlateau, LiveOnSkirt, LiveOnFlat, WorstDrapeError, DrapeSamples));

	// ⓐ 陡坡（裙边）上出现
	TestTrue(FString::Printf(TEXT("陡坡上长出岩壳（裙边活三角 %d）"), LiveOnSkirt), LiveOnSkirt > 100);
	// ⓑ 平地上不出现（整个三角都落在平地上的活三角 = 0）
	TestEqual(TEXT("平台顶上不长岩壳"), LiveOnPlateau, 0);
	TestEqual(TEXT("羽化之外的平地上不长岩壳"), LiveOnFlat, 0);
	// ⓒ 披挂真的贴在坡面上。容差 = 壳自己的厚度（沿法线的 Relief + 表面起伏 ∝ 坡度）+ 镜像重建余量。
	// 竖向四项是世界 cm ⇒ 属性值就是实际值。
	const double DrapeTolerance =
		double(Ground->RockShellCellRelief)
		+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
		+ double(Ground->RockShellChipAmount)
		+ 30.0;
	TestTrue(FString::Printf(TEXT("披挂贴住解析坡面（到面垂直距离 %.1f cm ≤ %.1f）"), WorstDrapeError, DrapeTolerance),
		DrapeSamples > 0 && WorstDrapeError <= DrapeTolerance);

	// ⓒ' 「石头隆起」：倍数打开，同一批裙边顶点必须**整体抬高**（量差值，把壳厚 / 噪声 / 浮高全消掉）。
	auto MeanSkirtZ = [](const TArray<FVector>& Pos, int32& OutCount)
	{
		const FVector2D C(CSRockShellTest_Centre, CSRockShellTest_Centre);
		double Sum = 0.0;
		OutCount = 0;
		for (int32 Tri = 0; Tri * 3 + 2 < Pos.Num(); ++Tri)
		{
			if (!CSRockShellTest_TriangleAlive(Pos, Tri)) continue;
			const FVector& P = Pos[Tri * 3];
			const double R = FVector2D::Distance(FVector2D(P.X, P.Y), C);
			if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff)
			{
				Sum += double(P.Z);
				++OutCount;
			}
		}
		return OutCount > 0 ? Sum / double(OutCount) : 0.0;
	};
	int32 NumFlat = 0;
	const double MeanFlat = MeanSkirtZ(Positions, NumFlat);

	Ground->RockShellRiseMultiplier = 2.0f;
	Ground->RebuildHeightsFromShapers();
	TArray<FVector> Raised;
	Ground->DebugReadRockShellSync(Raised);
	int32 NumRaised = 0;
	const double MeanRaised = MeanSkirtZ(Raised, NumRaised);
	if (NumFlat > 0 && NumRaised > 0)
	{
		AddInfo(FString::Printf(TEXT("隆起：倍数 1.0 时裙边均高 %.1f cm（%d 点），倍数 2.0 时 %.1f cm（%d 点）"),
			MeanFlat, NumFlat, MeanRaised, NumRaised));
		TestTrue(FString::Printf(TEXT("隆起倍数把壳抬起来了（+%.1f cm）"), MeanRaised - MeanFlat), MeanRaised - MeanFlat > 20.0);
	}
	else
	{
		TestTrue(TEXT("两种配置下裙边都有采样点（否则上面那条是空判据）"), false);
	}

	// ⓒ'' TG `:563` 的**基准偏移**：BaseLift 打开，同一批裙边顶点同样必须整体抬高。
	// 权重是壳的 mask（TG `_440`，Rock = 0.2 就饱和）⇒ 裙边大半条带都拿到满额。
	Ground->RockShellRiseMultiplier = 1.0f;
	Ground->RockShellBaseLift = 100.0f;
	Ground->RebuildHeightsFromShapers();
	TArray<FVector> Lifted;
	Ground->DebugReadRockShellSync(Lifted);
	int32 NumLifted = 0;
	const double MeanLifted = MeanSkirtZ(Lifted, NumLifted);
	if (NumFlat > 0 && NumLifted > 0)
	{
		AddInfo(FString::Printf(TEXT("基准偏移：BaseLift 0 时裙边均高 %.1f cm（%d 点），100 cm 时 %.1f cm（%d 点）"),
			MeanFlat, NumFlat, MeanLifted, NumLifted));
		// 2026-09-30 之前权重是 Rock（裙边上大多 0.1~0.5）⇒ 当时的下界只敢写 20；换成 `_440` 之后大半条带满额。
		TestTrue(FString::Printf(TEXT("基准偏移把壳浮起来了（+%.1f cm）"), MeanLifted - MeanFlat), MeanLifted - MeanFlat > 50.0);
	}
	else
	{
		TestTrue(TEXT("BaseLift 打开后裙边仍有采样点（否则上面那条是空判据）"), false);
	}

	Ground->RockShellBaseLift = SavedBaseLift;
	Ground->RockShellBaseSink = SavedBaseSink;
	Ground->RockShellRiseMultiplier = SavedRiseMultiplier;
	Ground->RockShellRiseExtend = SavedRiseExtend;
	Ground->RockShellRiseNoiseAmount = SavedRiseNoise;
	Ground->RockShellCellExpand = SavedCellExpand;
	Ground->RockShellCellSize = SavedCellSize;
	Ground->RockShellReliefFloor = SavedReliefFloor;
	Ground->RebuildHeightsFromShapers();

	// ⓒ''' **反火山口：默认档下壳的绝对形态必须是 TG 的** —— 没有任何活顶点高过台顶 + 有界预算。
	// 预算按属性现算（竖向四项是世界 cm）。
	{
		TArray<FVector> Defaults;
		Ground->DebugReadRockShellSync(Defaults);
		const double PlateauZ = Ground->SampleHeight(FVector2D(CSRockShellTest_Centre, CSRockShellTest_Centre));
		double WorstAbovePlateau = -TNumericLimits<double>::Max();
		int32 DefaultLive = 0;
		for (int32 Tri = 0; Tri * 3 + 2 < Defaults.Num(); ++Tri)
		{
			if (!CSRockShellTest_TriangleAlive(Defaults, Tri)) continue;
			++DefaultLive;
			WorstAbovePlateau = FMath::Max(WorstAbovePlateau, double(Defaults[Tri * 3].Z) - PlateauZ);
		}
		const double CrownBudget =
			double(Ground->RockShellBaseLift) + double(Ground->RockShellCellRelief)
			+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
			+ double(Ground->RockShellChipAmount) + 40.0;
		AddInfo(FString::Printf(TEXT("默认档：活三角 %d，最高点相对台顶 %+.1f cm（预算 %.1f）"),
			DefaultLive, WorstAbovePlateau, CrownBudget));
		TestTrue(FString::Printf(TEXT("默认档下没有石墙冠：最高点 ≤ 台顶 + %.0f cm（实测 %+.1f）"),
			CrownBudget, WorstAbovePlateau),
			DefaultLive > 100 && WorstAbovePlateau <= CrownBudget);
	}

	// ⓓ 删掉塑形物 ⇒ 高度场塌回 ⇒ 激活集合清空 ⇒ 全部槽失效。**裁决二的执行面**：没有一行注销代码。
	World->DestroyActor(Mound);
	Ground->RebuildHeightsFromShapers();
	Ground->DebugReadRockShellSync(Positions);
	int32 LiveAfter = 0;
	for (int32 Tri = 0; Tri * 3 + 2 < Positions.Num(); ++Tri) { if (CSRockShellTest_TriangleAlive(Positions, Tri)) ++LiveAfter; }
	AddInfo(FString::Printf(TEXT("删掉塑形物之后活着的三角 %d / %d，活跃胞腔 %d"), LiveAfter, Positions.Num() / 3, CSRockShellTest_ActiveCells(Ground)));
	TestEqual(TEXT("删掉塑形物后岩壳自己消失（不需要任何注销代码）"), LiveAfter, 0);
	TestEqual(TEXT("删掉塑形物后一个胞腔都不激活"), CSRockShellTest_ActiveCells(Ground), 0);
	return true;
}

// -----------------------------------------------------------------------------
// ③ 画路之后壳**连续下沉**而不是消失（裁决五）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellRoadSinkTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.RoadSinksNotHides",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellRoadSinkTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	TArray<FVector> Before;
	if (!TestTrue(TEXT("画路之前读得到岩壳"), Ground->DebugReadRockShellSync(Before) > 0)) return false;
	const int32 Triangles = Before.Num() / 3;
	int32 LiveBefore = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri) { if (CSRockShellTest_TriangleAlive(Before, Tri)) ++LiveBefore; }
	if (!TestTrue(FString::Printf(TEXT("画路之前壳是长出来的（%d 个活三角）"), LiveBefore), LiveBefore > 100)) return false;

	// 一条穿台的路。⚠️ 笔刷是 3D 球，必须贴地落笔。
	const double C = CSRockShellTest_Centre;
	const double Reach = CSRockShellTest_Radius + CSRockShellTest_Falloff;
	CSRockShellTest_PaintLine(Ground, C, C - Reach - 200.0, C, C + Reach + 200.0, 32);

	TArray<FVector> After;
	const int32 CountAfter = Ground->DebugReadRockShellSync(After);
	// 路不改高度场 ⇒ 激活集合不变 ⇒ 槽位一个不动（顶点身份逐下标稳定，下面才能逐下标比 Z）。
	if (!TestEqual(TEXT("画路前后顶点数不变（槽位池没重建）"), CountAfter, Before.Num())) return false;

	int32 LiveAfter = 0;
	int32 SunkSamples = 0;
	double SumSink = 0.0, DeepestSink = 0.0;
	int32 OffRoadSamples = 0;
	double WorstOffRoadDrift = 0.0;
	const double OffRoadX = double(Ground->BrushRadius + Ground->RockShellRoadBlurRadius + Ground->RockShellRoadSink) + 100.0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		const bool bAliveAfter = CSRockShellTest_TriangleAlive(After, Tri);
		if (bAliveAfter) ++LiveAfter;
		// 只比**两次都活着**的三角：死三角的后两个顶点保留着上一次的值。
		if (!bAliveAfter || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = Tri * 3 + K;
			const double Delta = double(After[Index].Z) - double(Before[Index].Z);
			const double Road = Ground->SampleRoadWeight(FVector2D(Before[Index].X, Before[Index].Y));
			if (Road > 0.5)
			{
				SumSink += Delta;
				DeepestSink = FMath::Min(DeepestSink, Delta);
				++SunkSamples;
			}
			// ⚠️ "路外"按路的几何判（x = Centre 的一条直线），不按顶点当前 XY 的路权重：下沉沿法线，XY 也跑了。
			else if (FMath::Abs(Before[Index].X - CSRockShellTest_Centre) > OffRoadX)
			{
				WorstOffRoadDrift = FMath::Max(WorstOffRoadDrift, FMath::Abs(Delta));
				++OffRoadSamples;
			}
		}
	}
	const double MeanSink = SunkSamples > 0 ? SumSink / double(SunkSamples) : 0.0;
	AddInfo(FString::Printf(TEXT("路下 %d 个顶点：平均 ΔZ %.1f cm、最深 %.1f cm；路外 %d 个顶点最大漂移 %.2f cm"),
		SunkSamples, MeanSink, DeepestSink, OffRoadSamples, WorstOffRoadDrift));

	// ⓐ **不是隐藏**：路不改变高度场 ⇒ 活着的三角必须**一个不少**。
	TestEqual(TEXT("画路之后活三角数一个都没少（壳没有被关掉）"), LiveAfter, LiveBefore);
	// ⓑ **是沉下去**：量下沉量本身（RoadSink 是世界口径，不随胞腔缩放）。
	if (!TestTrue(FString::Printf(TEXT("路下确实有壳可量（%d 个采样）"), SunkSamples), SunkSamples > 20)) return true;
	TestTrue(FString::Printf(TEXT("路下的壳沉下去了（平均 %.1f cm）"), MeanSink), MeanSink < -40.0);
	TestTrue(FString::Printf(TEXT("最深处沉了大半个 RoadSink（%.1f cm / %.0f cm）"), DeepestSink, Ground->RockShellRoadSink),
		DeepestSink < -0.5 * double(Ground->RockShellRoadSink));
	// 沉降不许超过物理上限：RoadSink + 画路会撤掉的正向量（厚度、起伏、基准偏移整段落差，都是世界 cm）。
	const double SinkLimit =
		double(Ground->RockShellRoadSink)
		+ double(Ground->RockShellCellRelief)
		+ double(Ground->RockShellNoiseAmount) * CSRockShellTest_MaxSlope
		+ double(Ground->RockShellChipAmount)
		+ double(Ground->RockShellBaseLift + Ground->RockShellBaseSink)
		+ 5.0;
	TestTrue(FString::Printf(TEXT("下沉量不超过 RoadSink + 自身厚度（%.1f cm ≤ %.1f）"), -DeepestSink, SinkLimit),
		-DeepestSink <= SinkLimit);
	// ⓒ 路外的壳**一动不动**。
	TestTrue(FString::Printf(TEXT("路外的壳纹丝不动（最大漂移 %.2f cm）"), WorstOffRoadDrift),
		OffRoadSamples == 0 || WorstOffRoadDrift < 0.01);

	// ⓔ **路缘是缓坡，不是折痕**（用户裁决 2026-09-11）：活顶点按到路中线的距离分 25 cm 一箱，
	// 比平均下沉剖面的最陡处。相对判据：同一条路，只把半径切到 0 再披挂一趟当对照。
	{
		auto SteepestSinkProfile = [&Before, Triangles](const TArray<FVector>& Sunk) -> double
		{
			constexpr double BinCm = 25.0;
			constexpr int32 NumBins = 36;
			constexpr int32 MinPerBin = 20;
			TArray<double> SumDz;
			TArray<int32> Count;
			SumDz.Init(0.0, NumBins);
			Count.Init(0, NumBins);
			for (int32 Tri = 0; Tri < Triangles; ++Tri)
			{
				if (!CSRockShellTest_TriangleAlive(Sunk, Tri) || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;
				for (int32 K = 0; K < 3; ++K)
				{
					const int32 Index = Tri * 3 + K;
					const int32 Bin = int32(FMath::Abs(double(Before[Index].X) - CSRockShellTest_Centre) / BinCm);
					if (Bin >= NumBins) continue;
					SumDz[Bin] += double(Sunk[Index].Z) - double(Before[Index].Z);
					++Count[Bin];
				}
			}
			double Steepest = 0.0;
			for (int32 Bin = 0; Bin + 1 < NumBins; ++Bin)
			{
				if (Count[Bin] < MinPerBin || Count[Bin + 1] < MinPerBin) continue;
				const double Step = SumDz[Bin + 1] / double(Count[Bin + 1]) - SumDz[Bin] / double(Count[Bin]);
				Steepest = FMath::Max(Steepest, FMath::Abs(Step) / BinCm);
			}
			return Steepest;
		};

		const double SteepBlurred = SteepestSinkProfile(After);
		const float BlurRadius = Ground->RockShellRoadBlurRadius;
		Ground->RockShellRoadBlurRadius = 0.0f;
		Ground->RebuildRockShell();
		TArray<FVector> AfterSharp;
		Ground->DebugReadRockShellSync(AfterSharp);
		const double SteepSharp = SteepestSinkProfile(AfterSharp);
		Ground->RockShellRoadBlurRadius = BlurRadius;
		Ground->RebuildRockShell();

		AddInfo(FString::Printf(TEXT("路缘平均下沉剖面的最陡处：模糊半径 %.0f cm 时 %.2f，半径 0 时 %.2f（cm/cm）"),
			BlurRadius, SteepBlurred, SteepSharp));
		TestTrue(FString::Printf(TEXT("对照组（半径 0）确实有折痕（最陡 %.2f）"), SteepSharp), SteepSharp > 1.0);
		TestTrue(FString::Printf(TEXT("模糊之后路缘至少缓一半（%.2f vs %.2f）"), SteepBlurred, SteepSharp),
			BlurRadius > 0.0f && SteepBlurred < 0.5 * SteepSharp);
	}

	// ⓓ 擦掉路 ⇒ 壳原样浮回来（绝对式而非增量式）。
	Ground->ResetPaint();
	TArray<FVector> Restored;
	Ground->DebugReadRockShellSync(Restored);
	double WorstRestoreError = 0.0;
	int32 RestoreSamples = 0;
	for (int32 Tri = 0; Tri < Triangles; ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Restored, Tri) || !CSRockShellTest_TriangleAlive(Before, Tri)) continue;
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 Index = Tri * 3 + K;
			WorstRestoreError = FMath::Max(WorstRestoreError, double(FMath::Abs(Restored[Index].Z - Before[Index].Z)));
			++RestoreSamples;
		}
	}
	AddInfo(FString::Printf(TEXT("擦掉路之后 %d 个顶点最大复位误差 %.3f cm"), RestoreSamples, WorstRestoreError));
	TestTrue(FString::Printf(TEXT("擦掉路之后壳逐位复位（最大误差 %.3f cm）"), WorstRestoreError),
		RestoreSamples > 100 && WorstRestoreError < 0.01);
	return true;
}

// -----------------------------------------------------------------------------
// ④ 盖 / 裙分离：`bIsCapTri` 到得了材质，而且**烘成 StaticMesh 之后还在**
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellCapSkirtBakeTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.CapSkirtSurvivesBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 盖 / 裙写在岩壳网格的**顶点色 R**（1 = 盖 / 0 = 裙），由 `M_TG_Texture` 的 `RockShellCapSkirt` 支路消费。
 * 守的是裁决六 ②「顶点色通道字典与多组 UV 必须随网格一起保住」。它必须**真烘一遍**：
 * GPU 常驻流 → `ReadbackResidentSync` → `BuildGpuMeshDescription` → StaticMesh 构建，任何一道把顶点色丢掉都不报错。
 */
bool FCSRockShellCapSkirtBakeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* ShellMaterial = CSRockShellTest_LoadShellMaterial();
	if (!TestNotNull(TEXT("岩壳材质"), ShellMaterial)) return false;
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, ShellMaterial);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	int32 ActiveCells = 0, CapacityCells = 0, TrisPerCell = 0;
	if (!TestTrue(TEXT("槽位池已建"), Ground->GetRockShellPoolStats(ActiveCells, CapacityCells, TrisPerCell))) return false;
	const int32 Triangles = CapacityCells * TrisPerCell;
	const int32 M = TrisPerCell / 5;

	// --- GPU 侧：通道里到底有没有两种值 ---
	int32 CapVerts = 0, SkirtVerts = 0;
	const int32 TotalVerts = Ground->DebugReadRockShellCapSplitSync(CapVerts, SkirtVerts);
	AddInfo(FString::Printf(TEXT("顶点色 R：盖 %d / 裙 %d，共 %d（池 %d 槽 × %d 三角）"),
		CapVerts, SkirtVerts, TotalVerts, CapacityCells, TrisPerCell));
	if (!TestEqual(TEXT("顶点色回读到的顶点数 = 池三角数 × 3"), TotalVerts, Triangles * 3)) return false;

	TestTrue(FString::Printf(TEXT("盖三角顶点存在（%d 个）"), CapVerts), CapVerts > 0);
	TestTrue(FString::Printf(TEXT("裙三角顶点存在（%d 个）"), SkirtVerts), SkirtVerts > 0);
	TestEqual(TEXT("盖 + 裙 = 全部顶点（没有第三种值）"), CapVerts + SkirtVerts, TotalVerts);
	// **逐三角常量**：个数必然是 3 的倍数，否则色流被按顶点插值 / 焊接过了。
	TestEqual(TEXT("盖顶点数是 3 的倍数（逐三角常量）"), CapVerts % 3, 0);
	TestEqual(TEXT("裙顶点数是 3 的倍数（逐三角常量）"), SkirtVerts % 3, 0);
	// 模板：每个边界点 3 个盖三角 + 2 个裙三角 ⇒ 全池 3M : 2M。
	TestEqual(TEXT("盖三角数 = 容量 × 3M"), CapVerts / 3, CapacityCells * 3 * M);
	TestEqual(TEXT("裙三角数 = 容量 × 2M"), SkirtVerts / 3, CapacityCells * 2 * M);

	// --- 烘一遍：走的是组件自己那条 `SaveToStaticMesh` 出口，不另拼等价路径 ---
	const FString BakePath = FString::Printf(
		TEXT("/Game/Automation/RockShell/SM_RockShellCapSkirt_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));
	int32 BakedCap = 0, BakedSkirt = 0, BakedTris = 0;
	const bool bBaked = Ground->DebugBakeRockShellCapSplitSync(BakePath, BakedCap, BakedSkirt, BakedTris);
	AddInfo(FString::Printf(TEXT("烘成 StaticMesh：%d 三角，角点 盖 %d / 裙 %d"), BakedTris, BakedCap, BakedSkirt));
	if (!TestTrue(TEXT("岩壳有一条走得通的 SaveToStaticMesh 出口（裁决六 ①）"), bBaked)) return false;

	TestTrue(FString::Printf(TEXT("烘出来的资产有三角（%d 个）"), BakedTris), BakedTris > 0);
	// 烘的是**活着**的那一批：NaN 顶点在 BuildGpuMeshDescription 里被整个三角丢掉。
	TestTrue(FString::Printf(TEXT("烘出来的三角是活着的那一批（%d < %d）"), BakedTris, Triangles), BakedTris < Triangles);
	TestEqual(TEXT("烘出来的角点数 = 三角数 × 3"), BakedCap + BakedSkirt, BakedTris * 3);
	TestTrue(FString::Printf(TEXT("烘完之后盖角点还在（%d 个）"), BakedCap), BakedCap > 0);
	TestTrue(FString::Printf(TEXT("烘完之后裙角点还在（%d 个）"), BakedSkirt), BakedSkirt > 0);
	TestEqual(TEXT("烘完之后盖角点数仍是 3 的倍数（没被插值掉）"), BakedCap % 3, 0);
	TestEqual(TEXT("烘完之后裙角点数仍是 3 的倍数（没被插值掉）"), BakedSkirt % 3, 0);

	UEditorAssetLibrary::DeleteAsset(BakePath);
	return true;
}

// -----------------------------------------------------------------------------
// ⑤ 胞腔多边形恰好划分平面（纯 CPU）
//
// 随机撒点：每个点必须落在它的功率图主人（暴力 9×9 求 argmin |x − p|² − w）的多边形里，
// 且不落在主人周围 3×3 其它胞腔的多边形**内部**。这一条同时钉住三件事：7×7 的邻域够不够、
// 半平面的符号、以及 CPU 孪生与离线原型（Scripts/PrototypeRockShellSparse.py）是同一套几何。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellCellPolygonTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.CellPolygon",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRockShellCellPolygonTest::RunTest(const FString&)
{
	struct FConfig { float Jitter; float Warp; float Wavelength; float Variance; const TCHAR* Name; };
	const FConfig Configs[] = {
		{ 1.0f, 0.5f, 3.0f, 0.0f, TEXT("默认（扭曲 0.5）") },
		{ 1.0f, 0.0f, 3.0f, 0.7f, TEXT("功率权重 0.7") },
		{ 1.0f, 0.8f, 5.0f, 0.7f, TEXT("扭曲 0.8 + 功率权重 0.7") },
	};
	const FVector2D GridOrigin(-1234.5, 678.9);   // 非零原点：格键与世界坐标的换算也在被测范围里
	for (const FConfig& Config : Configs)
	{
		CSRockShell::FPatternParams P;
		P.SeedJitter = Config.Jitter;
		P.ClusterWarp = Config.Warp;
		P.ClusterWavelength = Config.Wavelength;
		P.SizeVariance = Config.Variance;
		P = P.Sanitized();
		const double S = double(P.CellSize);

		TMap<FIntPoint, TArray<FVector2f>> Polys;
		auto PolyOf = [&Polys, &P](const FIntPoint& Cell) -> const TArray<FVector2f>&
		{
			if (const TArray<FVector2f>* Found = Polys.Find(Cell)) return *Found;
			TArray<FVector2f> Poly;
			CSRockShell::CellPolygon(P, Cell.X, Cell.Y, Poly);
			return Polys.Add(Cell, MoveTemp(Poly));
		};

		FRandomStream Rand(20260930);
		int32 NotInOwner = 0, InOther = 0;
		const int32 Samples = 3000;
		for (int32 N = 0; N < Samples; ++N)
		{
			const FVector2D Point(GridOrigin.X + Rand.FRandRange(-20.0f, 20.0f) * S, GridOrigin.Y + Rand.FRandRange(-20.0f, 20.0f) * S);
			const FIntPoint Owner = CSRockShellTest_Owner(P, GridOrigin, Point);
			const FVector2D OwnerSeed = CSRockShell::SeedWorld(P, GridOrigin, Owner.X, Owner.Y);
			const TArray<FVector2f>& OwnerPoly = PolyOf(Owner);
			if (OwnerPoly.Num() < 3 || !CSRockShellTest_InsideConvex(OwnerPoly, FVector2f(Point - OwnerSeed), 0.05f)) ++NotInOwner;
			for (int32 Dj = -1; Dj <= 1; ++Dj)
			{
				for (int32 Di = -1; Di <= 1; ++Di)
				{
					const FIntPoint Other(Owner.X + Di, Owner.Y + Dj);
					if (Other == Owner) continue;
					const TArray<FVector2f>& OtherPoly = PolyOf(Other);
					if (OtherPoly.Num() < 3) continue;
					const FVector2D OtherSeed = CSRockShell::SeedWorld(P, GridOrigin, Other.X, Other.Y);
					// 严格内部（向内收 0.05 cm）：边上的点两边都算，不是错。
					if (CSRockShellTest_InsideConvex(OtherPoly, FVector2f(Point - OtherSeed), -0.05f)) ++InOther;
				}
			}
		}
		AddInfo(FString::Printf(TEXT("%s：%d 个点，不在主人多边形里 %d，落在别人多边形内部 %d"), Config.Name, Samples, NotInOwner, InOther));
		TestEqual(FString::Printf(TEXT("%s：每个点都在自己主人的胞腔里"), Config.Name), NotInOwner, 0);
		TestEqual(FString::Printf(TEXT("%s：没有点落在别人胞腔的内部"), Config.Name), InOther, 0);

		// 外接半径的粗上界（激活粗筛用）必须真的是上界。
		const double Bound = (UE_DOUBLE_SQRT_2 + double(P.ClusterWarp) + double(P.SizeVariance)) * S;
		double Worst = 0.0;
		for (int32 J = -6; J <= 6; ++J)
		{
			for (int32 I = -6; I <= 6; ++I) Worst = FMath::Max(Worst, double(CSRockShell::CellCircumradius(P, I, J)));
		}
		TestTrue(FString::Printf(TEXT("%s：外接半径 %.1f ≤ 粗上界 %.1f"), Config.Name, Worst, Bound), Worst <= Bound);
	}
	return true;
}

// -----------------------------------------------------------------------------
// ⑥ 512 m 地面的远角落照样长石头
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellSparseFarCornerTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.SparseFarCorner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellSparseFarCornerTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, CSRockShellTest_LoadShellMaterial(), 1024);   // 512 m
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// 旧做法（一张 TG 原件 × 0.35 = 47.8 m，锚在地面正中）在 (480 m, 480 m) 一块石头都长不出来。
	const FVector2D Far(48000.0, 48000.0);
	ACSGroundShaperActor* Mound = CSRockShellTest_SpawnMound(World, Far.X, Far.Y);
	if (!TestNotNull(TEXT("Shaper mound"), Mound)) return false;

	const int32 Active = CSRockShellTest_ActiveCells(Ground);
	TArray<FVector> Positions;
	Ground->DebugReadRockShellSync(Positions);
	int32 LiveOnSkirt = 0, LiveElsewhere = 0;
	for (int32 Tri = 0; Tri * 3 + 2 < Positions.Num(); ++Tri)
	{
		if (!CSRockShellTest_TriangleAlive(Positions, Tri)) continue;
		const double R = FVector2D::Distance(FVector2D(Positions[Tri * 3].X, Positions[Tri * 3].Y), Far);
		double RMin = 0.0, RMax = 0.0;
		CSRockShellTest_TriRadiusRange(Positions, Tri, Far, RMin, RMax);
		if (RMin > CSRockShellTest_Radius + CSRockShellTest_Falloff + 600.0) ++LiveElsewhere;
		else if (R > CSRockShellTest_Radius && R < CSRockShellTest_Radius + CSRockShellTest_Falloff) ++LiveOnSkirt;
	}
	AddInfo(FString::Printf(TEXT("512 m 地面、塑形物在 (480 m, 480 m)：活跃胞腔 %d，裙边活三角 %d，远处活三角 %d"),
		Active, LiveOnSkirt, LiveElsewhere));
	TestTrue(TEXT("远角落激活了胞腔"), Active > 0);
	TestTrue(FString::Printf(TEXT("远角落的裙边长出石头（%d 个活三角）"), LiveOnSkirt), LiveOnSkirt > 100);
	TestEqual(TEXT("离塑形物远处一个活三角都没有"), LiveElsewhere, 0);
	return true;
}

// -----------------------------------------------------------------------------
// ⑦ 开销随内容而不随世界：零塑形物零胞腔；同一塑形物在 64 m / 512 m 地面上激活与位置逐位相同
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellSparseScalesTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.SparseScalesWithContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellSparseScalesTest::RunTest(const FString&)
{
	struct FRun
	{
		TArray<FIntPoint> Cells;
		TMap<FIntPoint, TArray<FVector>> PositionsByCell;   // 每个活跃胞腔自己那一槽的 15M 个顶点
	};
	auto RunOnGround = [](int32 Cells, FRun& Out, int32& OutEmptyActive) -> bool
	{
		UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
		if (!World) return false;
		ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, CSRockShellTest_LoadShellMaterial(), Cells);
		if (!Ground) return false;
		OutEmptyActive = CSRockShellTest_ActiveCells(Ground);
		if (!CSRockShellTest_SpawnMound(World, CSRockShellTest_Centre, CSRockShellTest_Centre)) return false;
		Ground->DebugGetRockShellActiveCells(Out.Cells);
		TArray<FVector> Positions;
		Ground->DebugReadRockShellSync(Positions);
		const int32 VertsPerCell = 15 * Ground->RockShellBoundaryPoints;
		for (const FIntPoint& Cell : Out.Cells)
		{
			const int32 Slot = Ground->DebugGetRockShellCellSlot(Cell);
			if (Slot < 0 || (Slot + 1) * VertsPerCell > Positions.Num()) return false;
			Out.PositionsByCell.Add(Cell, TArray<FVector>(Positions.GetData() + Slot * VertsPerCell, VertsPerCell));
		}
		return true;
	};

	FRun Small, Big;
	int32 EmptySmall = -1, EmptyBig = -1;
	if (!TestTrue(TEXT("64 m 地面跑通"), RunOnGround(CSRockShellTest_Cells, Small, EmptySmall))) return false;
	if (!TestTrue(TEXT("512 m 地面跑通"), RunOnGround(1024, Big, EmptyBig))) return false;

	TestEqual(TEXT("64 m 地面、零塑形物 ⇒ 零胞腔"), EmptySmall, 0);
	TestEqual(TEXT("512 m 地面、零塑形物 ⇒ 零胞腔"), EmptyBig, 0);
	AddInfo(FString::Printf(TEXT("同一座土台：64 m 地面激活 %d 个胞腔，512 m 地面激活 %d 个"), Small.Cells.Num(), Big.Cells.Num()));
	TestTrue(TEXT("土台激活了胞腔"), Small.Cells.Num() > 0);
	TestEqual(TEXT("激活数与世界大小无关"), Big.Cells.Num(), Small.Cells.Num());
	TestTrue(TEXT("激活的是同一批格键"), Big.Cells == Small.Cells);

	// 逐三角比：死活必须一致；活三角比三个顶点。⚠️ **死三角的后两个顶点不能比** —— 它们停在建池时的
	// 包围盒底角，而包围盒 Z 半高随地面尺寸变（64 m：2000 cm / 512 m：12800 cm，差的正好是 10800）。
	double Worst = 0.0;
	int32 Compared = 0, AliveMismatch = 0;
	for (const TPair<FIntPoint, TArray<FVector>>& Pair : Small.PositionsByCell)
	{
		const TArray<FVector>* Other = Big.PositionsByCell.Find(Pair.Key);
		if (!Other) continue;
		for (int32 Tri = 0; Tri * 3 + 2 < Pair.Value.Num(); ++Tri)
		{
			const bool bAliveA = CSRockShellTest_TriangleAlive(Pair.Value, Tri);
			const bool bAliveB = CSRockShellTest_TriangleAlive(*Other, Tri);
			if (bAliveA != bAliveB) { ++AliveMismatch; continue; }
			if (!bAliveA) continue;
			for (int32 K = 0; K < 3; ++K) Worst = FMath::Max(Worst, (Pair.Value[Tri * 3 + K] - (*Other)[Tri * 3 + K]).GetAbsMax());
			++Compared;
		}
	}
	AddInfo(FString::Printf(TEXT("同键胞腔逐三角比较：活三角 %d 个，最大差 %.4f cm，死活不一致 %d 个"), Compared, Worst, AliveMismatch));
	TestEqual(TEXT("同键胞腔的三角死活一致"), AliveMismatch, 0);
	TestTrue(FString::Printf(TEXT("同键胞腔的活三角位置逐位相同（最大差 %.4f cm）"), Worst), Compared > 0 && Worst < 0.01);
	return true;
}

// -----------------------------------------------------------------------------
// ⑧ 激活只许过标不许漏标：暴力扫一遍，坡度过阈的点所在的胞腔一定被激活了
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSRockShellActivationConservativeTest,
	"PCGPlugins.ComputeShaderGenerator.RockShell.ActivationConservative",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRockShellActivationConservativeTest::RunTest(const FString&)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	ACSGroundActor* Ground = CSRockShellTest_SpawnGround(World, CSRockShellTest_LoadShellMaterial());
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;

	// 四类：两座相交的土台、一个坑、一座中心隆起（整盘都有坡）。
	TestNotNull(TEXT("土台 A"), CSRockShellTest_SpawnMound(World, 1800.0, 1800.0, 500.0f, 300.0f, 600.0f));
	TestNotNull(TEXT("土台 B（与 A 相交）"), CSRockShellTest_SpawnMound(World, 3500.0, 2600.0, 700.0f, 500.0f, 700.0f));
	TestNotNull(TEXT("坑"), CSRockShellTest_SpawnMound(World, 4700.0, 4700.0, -300.0f, 300.0f, 500.0f));
	TestNotNull(TEXT("中心隆起"), CSRockShellTest_SpawnMound(World, 1900.0, 4700.0, 600.0f, 500.0f, 500.0f, 1.0f));
	Ground->RebuildHeightsFromShapers();

	TArray<FIntPoint> ActiveArray;
	Ground->DebugGetRockShellActiveCells(ActiveArray);
	TSet<FIntPoint> Active(ActiveArray);
	TArray<FVector4f> Packed;
	Ground->BuildShaperGpuParams(Packed);
	const CSRockShell::FPatternParams P = Ground->MakeRockShellPatternParams();
	const FVector2D GridOrigin(Ground->GetActorLocation().X, Ground->GetActorLocation().Y);
	const float D = Ground->CellSize;
	const float Lo = Ground->RockShellSlopeLo;
	const float Hi = FMath::Max(Ground->RockShellSlopeHi, Lo + 1e-3f);

	int32 SteepPoints = 0, Missed = 0;
	const double Span = double(CSRockShellTest_Cells) * double(CSRockShellTest_CellSize);
	for (double Y = 0.0; Y <= Span; Y += 25.0)
	{
		for (double X = 0.0; X <= Span; X += 25.0)
		{
			const FVector2f Q = FVector2f(float(X), float(Y));   // ⚠️ 别写成 Q(float(X), float(Y))：那是函数声明
			const float Gx = (CSGroundShaperField::EvalField(Q + FVector2f(D, 0.0f), Packed) - CSGroundShaperField::EvalField(Q - FVector2f(D, 0.0f), Packed)) / (2.0f * D);
			const float Gy = (CSGroundShaperField::EvalField(Q + FVector2f(0.0f, D), Packed) - CSGroundShaperField::EvalField(Q - FVector2f(0.0f, D), Packed)) / (2.0f * D);
			const float T = FMath::Clamp((FMath::Sqrt(Gx * Gx + Gy * Gy) - Lo) / (Hi - Lo), 0.0f, 1.0f);
			if (T * T * (3.0f - 2.0f * T) <= 0.1f) continue;   // 与 kernel 的显隐门槛同一个：Rock > 0.1
			++SteepPoints;
			if (!Active.Contains(CSRockShellTest_Owner(P, GridOrigin, FVector2D(X, Y)))) ++Missed;
		}
	}
	// 过标有多少只记不判：64 m 地面上四座塑形物的环带 ⊕ 外接半径本来就占掉一大片，
	// 稀疏性由 ⑦（零塑形物零胞腔、与世界大小无关）钉。
	const double DomainCells = FMath::Square(Span / double(P.CellSize));
	AddInfo(FString::Printf(TEXT("坡度过阈的采样点 %d 个，所在胞腔没被激活的 %d 个；激活 %d 个胞腔 / 地面约 %.0f 个"),
		SteepPoints, Missed, Active.Num(), DomainCells));
	TestTrue(TEXT("场景里真有陡坡（否则是空判据）"), SteepPoints > 1000);
	TestEqual(TEXT("没有漏激活的胞腔"), Missed, 0);
	return true;
}

#endif   // WITH_EDITOR

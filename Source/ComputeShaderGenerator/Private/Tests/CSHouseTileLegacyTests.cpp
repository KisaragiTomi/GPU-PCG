#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSHouseProfile.h"
#include "CSHouseRoof.h"
#include "CSHouseTile.h"
#include "CSHouseVine.h"

// -----------------------------------------------------------------------------
// 等坡度屋面逐位不变：对照**冻结的旧实现**。
//
// 2026-10-02 屋面求值器加了逐边坡度倍率与山墙边（脊长可调，`CSHouseRoof.h` 抬头）。承诺是：没有倍率的屋面
// （`FCSRoofDesc::EdgeSteep` 为空 —— 所有旧存档、所有异形房子、脊长留在"自动"的矩形房子）骨架、坡面排区间、
// 每一片瓦的每一个 float 都与改动之前相同，容量上界也相同。
//
// 「新代码里全 1 的倍率 = 空表」只说明新代码自洽，说明不了它等于旧代码。所以这里把改动之前的实现
// （提交 3307d60 的 `CSHouseRoof.{h,cpp}` 与 `CSHouseTile.cpp`）**原样**冻下来逐字段比 —— 与
// `Wall.HouseBodyMatchesLegacy` 同一条做法。下面这段除了改名（`CSHouseTileLegacy_` 前缀、类型补命名空间限定）
// 一个字符都没动；它是基准，**别改它**。屋面的等坡度行为哪天真要变，删掉这条测试，而不是调它。
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseTileLegacy_ 前缀。

// ===== 冻结：CSHouseRoof.cpp（3307d60）的波前顶点与骨架 =====

/** 波前上的一个活动顶点：它夹在两条活动边之间，随内距 d 沿角平分线匀速走。 */
struct FCSHouseTileLegacy_Vertex
{
	/** 当前内距处的位置。 */
	FVector2D P = FVector2D::ZeroVector;
	/** 内距每增加 1，位置走多少（= 两条边内法线之和 / (1 + 点积)，长度 1/cos(转角/2)）。 */
	FVector2D Velocity = FVector2D::ZeroVector;
	/** 这个顶点划出的骨架弧从哪儿起算。 */
	FVector2D OriginP = FVector2D::ZeroVector;
	double OriginInset = 0.0;
	/** 原始角号（footprint 的角）；骨架节点为 INDEX_NONE。 */
	int32 Corner = INDEX_NONE;
	int32 EdgeIn = INDEX_NONE;
	int32 EdgeOut = INDEX_NONE;
};

FVector2D CSHouseTileLegacy_Velocity(const FVector2D& InA, const FVector2D& InB)
{
	const double Denom = 1.0 + FVector2D::DotProduct(InA, InB);
	// 两条边平行反向：它们之间的波前宽度恒定，顶点不再移动（矩形脊两端就是这种顶点）。
	if (Denom <= 1.0e-9) return FVector2D::ZeroVector;
	return (InA + InB) / Denom;
}

void CSHouseTileLegacy_BuildSkeleton(const FCSHouseFootprint& Footprint, double Overhang, FCSRoofSkeleton& Out)
{
	Out = FCSRoofSkeleton();
	const int32 N = Footprint.NumEdges();
	if (N < 3) return;

	TArray<FVector2D, TInlineAllocator<16>> In;
	In.SetNum(N);
	TArray<FVector2D, TInlineAllocator<16>> U;
	U.SetNum(N);
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
		if (F.Len <= 0.0f) return;   // 零长边：不是合法 footprint，输出空骨架
		In[Edge] = F.In;
		U[Edge] = F.U;
	}

	// 顶点 i 在 Verts[i]，夹在 i−1 号边与 i 号边之间 ⇒ 它是角 i−1（角 k 在 Verts[k+1]）。
	TArray<FCSHouseTileLegacy_Vertex, TInlineAllocator<16>> Ring;
	for (int32 Index = 0; Index < N; ++Index)
	{
		FCSHouseTileLegacy_Vertex V;
		V.EdgeIn = (Index + N - 1) % N;
		V.EdgeOut = Index;
		V.Corner = V.EdgeIn;
		V.Velocity = CSHouseTileLegacy_Velocity(In[V.EdgeIn], In[V.EdgeOut]);
		V.P = Footprint.Verts[Index];
		// 角斜脊从檐口外角起算：内距 −Overhang 处的波前顶点。
		V.OriginP = V.P - V.Velocity * Overhang;
		V.OriginInset = -Overhang;
		Ring.Add(V);
	}

	TArray<FCSRoofSkeletonArc> Hips;
	Hips.SetNum(N);
	TArray<FCSRoofSkeletonArc> Ridges;

	auto Emit = [&Hips, &Ridges](const FCSHouseTileLegacy_Vertex& V, const FVector2D& EndP, double EndInset)
	{
		FCSRoofSkeletonArc Arc;
		Arc.A = V.OriginP;
		Arc.InsetA = V.OriginInset;
		Arc.B = EndP;
		Arc.InsetB = EndInset;
		Arc.FaceA = V.EdgeIn;
		Arc.FaceB = V.EdgeOut;
		if (V.Corner != INDEX_NONE)
		{
			Hips[V.Corner] = Arc;               // 角斜脊一条不落，零长也记（按角号取）
		}
		else if (FVector2D::DistSquared(Arc.A, Arc.B) > 1.0e-12)
		{
			Ridges.Add(Arc);                    // 节点刚生成就被吃掉的零长弧不记
		}
	};

	double Now = 0.0;
	while (Ring.Num() >= 3)
	{
		// 最早塌掉的活动边：边 i 从 Ring[i] 走到 Ring[i+1]，长度按 −(两端 tan(转角/2) 之和) 的速率收短。
		int32 Best = INDEX_NONE;
		double BestT = TNumericLimits<double>::Max();
		for (int32 Index = 0; Index < Ring.Num(); ++Index)
		{
			const FCSHouseTileLegacy_Vertex& V0 = Ring[Index];
			const FCSHouseTileLegacy_Vertex& V1 = Ring[(Index + 1) % Ring.Num()];
			const FVector2D& Dir = U[V0.EdgeOut];
			const double Length = FVector2D::DotProduct(V1.P - V0.P, Dir);
			const double Rate = FVector2D::DotProduct(V1.Velocity - V0.Velocity, Dir);
			if (Rate >= -1.0e-12) continue;     // 不收短（两端都共线）的边永远不塌
			const double T = FMath::Max(Length, 0.0) / -Rate;
			// 严格小于：同时塌的边取环上靠前的那条，结果与输入顺序一一对应、可复现。
			if (T < BestT)
			{
				BestT = T;
				Best = Index;
			}
		}
		if (Best == INDEX_NONE) break;          // 非凸输入会走到这里：到此为止，不猜

		Now += BestT;
		for (FCSHouseTileLegacy_Vertex& V : Ring) V.P += V.Velocity * BestT;

		const int32 NextIndex = (Best + 1) % Ring.Num();
		const FCSHouseTileLegacy_Vertex V0 = Ring[Best];
		const FCSHouseTileLegacy_Vertex V1 = Ring[NextIndex];
		const FVector2D Node = (V0.P + V1.P) * 0.5;
		Emit(V0, Node, Now);
		Emit(V1, Node, Now);

		FCSHouseTileLegacy_Vertex Merged;
		Merged.EdgeIn = V0.EdgeIn;
		Merged.EdgeOut = V1.EdgeOut;
		Merged.P = Node;
		Merged.OriginP = Node;
		Merged.OriginInset = Now;
		Merged.Velocity = CSHouseTileLegacy_Velocity(In[Merged.EdgeIn], In[Merged.EdgeOut]);

		// 先覆盖再删：NextIndex 为 0 时删掉的是环首，覆盖过的那一格顺移一位，环序不变。
		Ring[Best] = Merged;
		Ring.RemoveAt(NextIndex);
	}

	Out.MaxInset = Now;
	for (const FCSHouseTileLegacy_Vertex& V : Ring) Emit(V, V.P, Now);

	// 剩下两个顶点（两条平行反向的边之间）就是最高那段脊的两端；三角形收成一点时两端重合。
	if (Ring.Num() == 2)
	{
		FCSRoofSkeletonArc Top;
		Top.A = Ring[0].P;
		Top.B = Ring[1].P;
		Top.InsetA = Top.InsetB = Now;
		Top.FaceA = Ring[0].EdgeOut;
		Top.FaceB = Ring[1].EdgeOut;
		if (Top.B.X < Top.A.X || (Top.B.X == Top.A.X && Top.B.Y < Top.A.Y))
		{
			Swap(Top.A, Top.B);
		}
		Out.TopA = Top.A;
		Out.TopB = Top.B;
		Ridges.Add(Top);
	}
	else if (Ring.Num() == 1)
	{
		Out.TopA = Out.TopB = Ring[0].P;
	}

	Out.NumHips = N;
	Out.Arcs = MoveTemp(Hips);
	Out.Arcs.Append(Ridges);
}

// ===== 冻结：CSHouseRoof.h（3307d60）的坡面排区间 =====

inline bool CSHouseTileLegacy_FaceSpanAtInset(const FCSHouseFootprint& Footprint, int32 Edge, double Inset,
	double& OutT0, double& OutT1)
{
	const int32 N = Footprint.NumEdges();
	if (N < 3 || Edge < 0 || Edge >= N) return false;
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
	if (F.Len <= 0.0f) return false;

	const FVector2D Base = F.Start + F.In * Inset;
	const double Unbounded = 1.0e30;
	double T0 = -Unbounded, T1 = Unbounded;
	for (int32 Other = 0; Other < N; ++Other)
	{
		if (Other == Edge) continue;
		const FCSHouseEdgeFrame G = CSHouse_GetEdge(Other, Footprint, 0.0f);
		if (G.Len <= 0.0f) continue;
		// 「(Base + U·t − G.Start)·G.In ≥ d」⇔「Q + C·t ≥ 0」。
		const double C = FVector2D::DotProduct(F.U, G.In);
		const double Q = FVector2D::DotProduct(Base - G.Start, G.In) - Inset;
		if (FMath::Abs(C) <= 1.0e-9)
		{
			if (Q < 0.0) return false;   // 平行边已经把这一排整条吃掉
			continue;
		}
		const double R = -Q / C;
		if (C > 0.0) T0 = FMath::Max(T0, R);
		else T1 = FMath::Min(T1, R);
	}
	if (T0 <= -Unbounded || T1 >= Unbounded || T1 <= T0) return false;
	OutT0 = T0;
	OutT1 = T1;
	return true;
}

// ===== 冻结：CSHouseTile.cpp（3307d60）的常量、辅助函数、容量上界与铺瓦 =====

/** 排数 / 列数的硬上限：极端参数（间距被设成 0.01）下不该让容量与循环无界增长。 */
constexpr int32 CSHouseTileLegacy_MaxRows = 256;
constexpr int32 CSHouseTileLegacy_MaxColumns = 512;

/**
 * 水平屋脊的 TG 常数（`system_roof::utils::assemble_rectangular_roof`，2026-09-29 逆向）：
 * 分点数 = max(round(脊长 / 0.625 m), 2)、中间分点随机挪 ±0.0625 m（`utils::random_splits`，幅度另夹在段长的 0.495 倍内），
 * 每段一块平躺的板：长 = 段长 × 1.4、宽 0.4 m 起（随屋顶参数到 0.55）、厚 0.15 m = 坡面瓦板厚 0.1 m 的 1.5 倍。
 */
constexpr double CSHouseTileLegacy_RidgePiece = 62.5;
constexpr double CSHouseTileLegacy_RidgeWiggle = 12.5;
constexpr double CSHouseTileLegacy_RidgeOverlap = 1.4;
constexpr float CSHouseTileLegacy_RidgeWidth = 40.0f;
constexpr float CSHouseTileLegacy_RidgeDepth = 1.5f;
constexpr float CSHouseTileLegacy_HipWidth = 25.0f;
constexpr float CSHouseTileLegacy_HipDepth = 1.8f;

/**
 * 轴排列的兜底：三根轴必须互不相同，否则基会退化成奇异矩阵（画出来是一片拉到无穷远的黑面）。
 * 自动判定失手或蓝图里手填错时退回 (上坡 X / 沿排 Y / 法线 Z)，画得对不对另说，至少画得出来。
 */
CSHouseTile::FMeshAxes CSHouseTileLegacy_SanitizeAxes(const CSHouseTile::FMeshAxes& In)
{
	CSHouseTile::FMeshAxes Out = In;
	Out.UpSlope = FMath::Clamp(Out.UpSlope, 0, 2);
	Out.AlongRow = FMath::Clamp(Out.AlongRow, 0, 2);
	Out.Normal = FMath::Clamp(Out.Normal, 0, 2);
	if (Out.UpSlope == Out.AlongRow || Out.UpSlope == Out.Normal || Out.AlongRow == Out.Normal)
	{
		Out.UpSlope = 0;
		Out.AlongRow = 1;
		Out.Normal = 2;
	}
	if (!(FMath::Abs(Out.NormalSign) > 0.0f)) Out.NormalSign = 1.0f;
	return Out;
}

/** 有效排距 cm。≤ 0 时由网格尺寸反解：**瓦按原尺寸画，排距把它压出 RowOverlap 那么多重叠**。 */
float CSHouseTileLegacy_RowPitch(const CSHouseTile::FParams& Params)
{
	if (Params.RowPitch > 0.0f) return Params.RowPitch;
	const float Native = FMath::Max(Params.Axes.NativeAlongSlope(), 1.0f);
	return FMath::Max(Native / FMath::Max(Params.RowOverlap, 0.05f), 1.0f);
}

/** 有效列距 cm。同上。 */
float CSHouseTileLegacy_ColumnPitch(const CSHouseTile::FParams& Params)
{
	if (Params.ColumnPitch > 0.0f) return Params.ColumnPitch;
	const float Native = FMath::Max(Params.Axes.NativeAcrossRow(), 1.0f);
	return FMath::Max(Native / FMath::Max(Params.ColumnOverlap, 0.05f), 1.0f);
}

/**
 * 坡长（檐口外沿 → 屋面最高处，沿坡面量），所有坡面共用同一个值（同坡度、同外挑）。
 * 到不了最高处的坡面（矩形的短边面、多边形上先收尖的面）在后面几排自然区间为空。
 */
float CSHouseTileLegacy_SlopeLength(const FCSRoofDesc& Roof, double MaxInset)
{
	const float CosP = FMath::Max(Roof.CosPitch(), UE_KINDA_SMALL_NUMBER);
	return (float(MaxInset) + FMath::Max(Roof.Overhang, 0.0f)) / CosP;
}

int32 CSHouseTileLegacy_MaxTilesBound(const FCSRoofDesc& Roof, const CSHouseTile::FParams& InParams);

void CSHouseTileLegacy_BuildPlan(const FCSRoofDesc& Roof, const FTransform& World, const CSHouseTile::FParams& InParams, TArray<CSHouseTile::FRecord>& OutTiles)
{
	OutTiles.Reset();

	const FCSHouseFootprint& Footprint = Roof.Footprint;
	const int32 NumFaces = Footprint.NumEdges();
	if (Roof.bFlat || !Footprint.IsValidFootprint()) return;

	FCSRoofSkeleton Skeleton;
	CSHouseTileLegacy_BuildSkeleton(Footprint, double(FMath::Max(Roof.Overhang, 0.0f)), Skeleton);
	// 最大内切圆半径不到 1 cm 的"屋顶"不铺（矩形时代的「任一半边 ≤ 1 cm」）。
	if (Skeleton.MaxInset <= 1.0) return;

	CSHouseTile::FParams Params = InParams;
	Params.Axes = CSHouseTileLegacy_SanitizeAxes(Params.Axes);

	const float CosP = FMath::Max(Roof.CosPitch(), UE_KINDA_SMALL_NUMBER);
	const float SinP = Roof.SinPitch();
	const float TanP = Roof.TanPitch();
	const float Overhang = FMath::Max(Roof.Overhang, 0.0f);
	const float SlopeLen = CSHouseTileLegacy_SlopeLength(Roof, Skeleton.MaxInset);
	if (SlopeLen <= 1.0f) return;

	// 排：沿坡面**等分**（目标间距只决定份数）。所有坡面共用同一份排数与排距 ⇒ 每一排的高度
	// 在每个面上逐位相同，角斜脊上两侧的瓦因此是对齐的，不会错半排。
	const int32 Rows = FMath::Clamp(FMath::RoundToInt(SlopeLen / CSHouseTileLegacy_RowPitch(Params)), 1, CSHouseTileLegacy_MaxRows);
	const double RowStep = double(SlopeLen) / double(Rows);
	const float ColumnPitch = CSHouseTileLegacy_ColumnPitch(Params);

	const int32 AxisUp = Params.Axes.UpSlope;
	const int32 AxisAlong = Params.Axes.AlongRow;
	const int32 AxisNormal = Params.Axes.Normal;

	// 手性：排列的奇偶 × 法线取向决定这组基是左手还是右手，左手基会把瓦**镜像**过去
	// （背面朝外、光照整个翻掉，而实例数与位置断言全绿）。在**沿排轴**上补一个符号掰回来 ——
	// 沿排方向本来就没有正反之分（瓦沿排是对称的），法线与上坡向都不能动。
	FVector Probe[3];
	Probe[AxisUp] = FVector(1.0, 0.0, 0.0);
	Probe[AxisAlong] = FVector(0.0, 1.0, 0.0);
	Probe[AxisNormal] = FVector(0.0, 0.0, double(Params.Axes.NormalSign) >= 0.0 ? 1.0 : -1.0);
	const double AlongSign = FVector::DotProduct(FVector::CrossProduct(Probe[0], Probe[1]), Probe[2]) < 0.0 ? -1.0 : 1.0;

	// ≤ 0 = 沿用网格原生厚度（旧行为）；> 0 = 直接给绝对厚度 cm。见 CSHouseTile::FParams::Thickness 的注释。
	const float Thickness = Params.Thickness > 0.0f
		? Params.Thickness
		: FMath::Max(Params.Axes.NativeThickness(), 0.1f);
	const float SizeScale = FMath::Max(Params.SizeScale, 0.01f);

	OutTiles.Reserve(CSHouseTileLegacy_MaxTilesBound(Roof, Params));

	for (int32 Side = 0; Side < NumFaces; ++Side)
	{
		// 外法线与沿边方向。⚠️ 沿边取 `(n.y, -n.x)` 而不是 `(-n.y, n.x)`：只有这一支能让
		// (上坡, 沿排, 法线) 成右手基，另一支差一个镜像。逆时针折线上它恰好是 −U。
		const FCSHouseEdgeFrame Edge = CSHouse_GetEdge(Side, Footprint, 0.0f);
		if (Edge.Len <= 0.0f) continue;
		const FVector2D Outward(-Edge.In.X, -Edge.In.Y);
		const FVector2D Along(Outward.Y, -Outward.X);

		// 局部（actor 空间）的三条轴。法线朝上外、上坡向指向屋脊、沿排水平。
		const FVector NormalLocal(Outward.X * SinP, Outward.Y * SinP, CosP);
		const FVector UpSlopeLocal(-Outward.X * CosP, -Outward.Y * CosP, SinP);
		const FVector AlongLocal(Along.X * AlongSign, Along.Y * AlongSign, 0.0);

		for (int32 Row = 0; Row < Rows; ++Row)
		{
			// 沿坡面的弧长（自檐口外沿起）→ 内距。d = −Overhang 是檐口外沿，d = MaxInset 是屋面最高处。
			const double SlopeS = (double(Row) + 0.5) * RowStep;
			const double Inset = -double(Overhang) + SlopeS * double(CosP);

			// 这一排在本坡面上的沿边区间（见 `CSHouseTileLegacy_FaceSpanAtInset`）。矩形上是 [d, Len − d]，
			// 即"半宽随内距 1:1 收窄"；收到不足 1 cm 就是角上的尖，那一排没有瓦。
			double T0 = 0.0, T1 = 0.0;
			if (!CSHouseTileLegacy_FaceSpanAtInset(Footprint, Side, Inset, T0, T1)) continue;
			const double Width = T1 - T0;
			if (Width <= 1.0) continue;
			const FVector2D RowBase = Edge.Start + Edge.In * Inset;

			const bool bStagger = (Row & 1) != 0;
			const int32 Columns = FMath::Clamp(FMath::RoundToInt(Width / ColumnPitch), 1, CSHouseTileLegacy_MaxColumns - 1);
			const double ColumnStep = Width / double(Columns);

			for (int32 Column = 0; Column < Columns + int32(bStagger); ++Column)
			{
				// 奇数排错开半片，两端用半片收齐；不能直接挪整排，否则一侧漏空、另一侧越过角脊。
				double Cell0 = FMath::Max(0.0, (Column - (bStagger ? 0.5 : 0.0)) * ColumnStep);
				double Cell1 = FMath::Min(Width, (Column + (bStagger ? 0.5 : 1.0)) * ColumnStep);
				// 身份 = (面号, 排号, 列号, 佐料, 用户种子)。**刻意不含位置** —— 拖房子时
				// 屋面在动，位置派生的种子会让整片瓦在拖动过程里不停重掷（同藤蔓那条纪律）。
				const uint32 Id = CSHouseVine::IdentityHash(Side, Row, Column, 71u, Params.Seed);
				const float Random01 = CSHouseVine::Hash01(Id);
				const float ScaleJ = 1.0f + Params.ScaleJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 72u, Params.Seed)) - 0.5f) * 2.0f;
				const float LengthJ = 1.0f + Params.LengthJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 75u, Params.Seed)) - 0.5f) * 2.0f;
				const float YawJ = Params.YawJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 73u, Params.Seed)) - 0.5f) * 2.0f;
				const float LiftJ = Params.LiftJitter * (CSHouseVine::Hash01(
					CSHouseVine::IdentityHash(Side, Row, Column, 74u, Params.Seed)) - 0.5f) * 2.0f;
				// 端瓦必须覆盖到自己下沿处的坡面边界，再由裁剪面收口。只按瓦心所在排的宽度
				// 缩成半瓦会在斜脊两侧留下三角缺口，迫使盖瓦浮在上面遮洞。
				double LowerT0 = T0, LowerT1 = T1;
				const double HalfLength = RowStep * Params.RowOverlap * ScaleJ * LengthJ * SizeScale * 0.5;
				if (CSHouseTileLegacy_FaceSpanAtInset(Footprint, Side, Inset - HalfLength * CosP, LowerT0, LowerT1))
				{
					// 留一小段余量，让瓦心仍在自己的坡内（尤其金字塔顶端的半瓦），不能落在共享边界上。
					const double CellWidth = (Cell1 - Cell0) * 0.95;
					if (Column == 0) Cell0 -= FMath::Min(CellWidth, FMath::Max(LowerT1 - T1, 0.0));
					if (Column == Columns + int32(bStagger) - 1) Cell1 += FMath::Min(CellWidth, FMath::Max(T0 - LowerT0, 0.0));
				}

				// 列从 T1 端起排（沿 Along = −U 走），与矩形时代「U 从 −半宽起」同序 ⇒ 身份 (面, 排, 列) 不变。
				const FVector2D XY = RowBase + Edge.U * (T1 - (Cell0 + Cell1) * 0.5);
				const double Z = double(Roof.EaveZ) + double(TanP) * Inset;
				FVector Local = FVector(XY.X, XY.Y, Z) + NormalLocal * (double(Params.StandOff) + double(LiftJ));

				// 绕法线抖一点朝向：(上坡, 沿排, 法线) 是右手基，绕第三根轴转 a 就是这两句。
				const double CosY = FMath::Cos(double(YawJ)), SinY = FMath::Sin(double(YawJ));
				const FVector UpJittered = UpSlopeLocal * CosY + AlongLocal * SinY;
				const FVector AlongJittered = AlongLocal * CosY - UpSlopeLocal * SinY;

				FVector Dirs[3];
				Dirs[AxisUp] = World.TransformVectorNoScale(UpJittered).GetSafeNormal();
				Dirs[AxisAlong] = World.TransformVectorNoScale(AlongJittered).GetSafeNormal();
				Dirs[AxisNormal] = World.TransformVectorNoScale(NormalLocal * double(Params.Axes.NormalSign)).GetSafeNormal();

				float Sizes[3];
				// 瓦画多大 = 实际间距 × 重叠系数：等分给出的实际间距逐排不同，瓦跟着变，
				// 屋面因此永远铺满 —— 拿目标间距去算的话最后一排/一列会露出底下的天空。
				// `SizeScale` 只乘平面内两轴：排距不动 ⇒ 瓦数不变，只是每片变大或变小。
				// 想改瓦数请调 RowPitch / ColumnPitch，那是另一件事。`LengthJ` 只乘瓦长（TG 同排瓦等宽、长短不一）。
				Sizes[AxisUp] = float(RowStep) * Params.RowOverlap * ScaleJ * LengthJ * SizeScale;
				Sizes[AxisAlong] = float(Cell1 - Cell0) * Params.ColumnOverlap * ScaleJ * SizeScale;
				Sizes[AxisNormal] = Thickness;
				const double Remaining = (double(SlopeLen) - SlopeS);
				const double Excess = FMath::Max(double(Sizes[AxisUp]) * 0.5 - Remaining, 0.0);
				Sizes[AxisUp] -= float(Excess);
				Local -= UpSlopeLocal * (Excess * 0.5);


				// 枢轴补偿：实例变换把网格顶点 v 送到 `原点 + Σ v_i · 方向_i · 缩放_i`，
				// 所以要让**包围盒中心**落在 Local 上，原点得先把中心那一项减掉。
				// 枢轴本来就在中心的资产（NativeCentre = 0）这一步是恒等，一分钱不花。
				FVector Centred = World.TransformPosition(Local);
				for (int32 Axis = 0; Axis < 3; ++Axis)
				{
					const double Scale = double(Sizes[Axis]) / double(FMath::Max(Params.Axes.NativeSize[Axis], 0.01f));
					Centred -= Dirs[Axis] * (double(Params.Axes.NativeCentre[Axis]) * Scale);
				}

				CSHouseTile::FRecord& Rec = OutTiles.AddDefaulted_GetRef();
				Rec.WorldPos = FVector3f(Centred);
				Rec.Random01 = Random01;
				Rec.AxisX = FVector3f(Dirs[0]);
				Rec.AxisY = FVector3f(Dirs[1]);
				Rec.AxisZ = FVector3f(Dirs[2]);
				Rec.SizeX = Sizes[0];
				Rec.SizeY = Sizes[1];
				Rec.SizeZ = Sizes[2];

				// 两坡等距线就是角脊，不能用宽大的骑脊双片去遮越界的瓦。
				// 和 TG 的逐瓦 clipping_plane_ 一样，选离瓦心最近的坡面边界；世界空间方程随房屋 yaw/位移一起变换。
				double Closest = TNumericLimits<double>::Max();
				for (int32 Other = 0; Other < NumFaces; ++Other)
				{
					if (Other == Side) continue;
					const FCSHouseEdgeFrame Neighbour = CSHouse_GetEdge(Other, Footprint, 0.0f);
					const FVector2D Difference = Edge.In - Neighbour.In;
					const double Denom = Difference.Size();
					if (Denom < UE_SMALL_NUMBER) continue;
					const FVector2D N = Difference / Denom;
					const double D = (FVector2D::DotProduct(Neighbour.Start, Neighbour.In) - FVector2D::DotProduct(Edge.Start, Edge.In)) / Denom;
					const double Distance = -FVector2D::DotProduct(N, XY) - D;
					if (Distance >= Closest) continue;
					Closest = Distance;
					const FVector WorldN = World.TransformVectorNoScale(FVector(N.X, N.Y, 0.0));
					Rec.ClipPlane = FVector2f(float(FMath::Atan2(WorldN.Y, WorldN.X) + 4.0),
						float(D - FVector::DotProduct(WorldN, World.GetLocation())));
				}
			}
		}
	}

	// -------------------------------------------------------------------------
	// 角斜脊 / 屋脊的盖瓦（用户 2026-08-31：「瓦片交汇处有 mesh 进行遮蔽」）
	//
	// 同一瓦片沿骨架的每条弧收口（矩形上 = 四条角斜脊 + 一条屋脊）。TG 的原始资产含
	// roof_tile / lod1 / backface，不是三个可随机互换的瓦型；这里只复用经 VS 尺寸适配的可见瓦片。
	//
	// TG：水平脊与角斜脊都沿脊分段、每段一块板。角脊板只有 25 cm 宽，由上面的逐瓦裁剪面收齐坡面瓦。
	if (Params.RidgeCapScale > 0.0f)
	{
		// 一条脊线 = 起点 → 终点 + 该处两坡法线的角平分；两端内距相同 = 水平的脊。
		struct FRidgeLine { FVector A; FVector B; FVector Normal; bool bLevel; };
		TArray<FRidgeLine, TInlineAllocator<16>> Lines;

		// 角平分线是闭式的：两侧坡面的外法线是 (外法线_a · sinP, cosP) 与 (外法线_b · sinP, cosP)，
		// 和归一化即得。脊上两面对冲 ⇒ 恒为竖直向上。
		for (const FCSRoofSkeletonArc& Arc : Skeleton.Arcs)
		{
			const FCSHouseEdgeFrame FA = CSHouse_GetEdge(Arc.FaceA, Footprint, 0.0f);
			const FCSHouseEdgeFrame FB = CSHouse_GetEdge(Arc.FaceB, Footprint, 0.0f);
			const FVector N(-(FA.In.X + FB.In.X) * double(SinP), -(FA.In.Y + FB.In.Y) * double(SinP), 2.0 * double(CosP));
			Lines.Add({
				FVector(Arc.A.X, Arc.A.Y, double(Roof.EaveZ) + Arc.InsetA * double(TanP)),
				FVector(Arc.B.X, Arc.B.Y, double(Roof.EaveZ) + Arc.InsetB * double(TanP)),
				N.GetSafeNormal(),
				FMath::Abs(Arc.InsetB - Arc.InsetA) <= 0.5 });
		}

		for (int32 Line = 0; Line < Lines.Num(); ++Line)
		{
			const FRidgeLine& L = Lines[Line];
			const FVector Delta = L.B - L.A;
			const double Length = Delta.Size();
			if (Length <= 1.0) continue;
			const FVector Dir = Delta / Length;

			{
				// 板的翘端（网格 −Y = TG 的 +y）朝 A：从 A 往 B 铺，后一块的翘端搭在前一块上，相邻两块各压段长的两成。
				// 板顶高过坡面瓦在脊顶的上沿 ⇒ 盖住两坡在脊上的对切口；板沿落回两侧瓦面（35° 时离瓦面约 5 cm）。
				const int32 Points = FMath::Clamp(FMath::RoundToInt(Length / CSHouseTileLegacy_RidgePiece), 2, CSHouseTileLegacy_MaxColumns);
				const double Nominal = Length / double(Points - 1);
				const double Wiggle = FMath::Min(Nominal * 0.495, CSHouseTileLegacy_RidgeWiggle);
				// 盖瓦要压在坡面瓦之上。本项目的瓦端另有 WPO 抬升，且烘焙包围盒中心还会参与下面的枢轴补偿；
				// 不能把 TG 的 -5 cm 平移直接当作这里的下沉量，否则窄斜脊会埋进两侧瓦面。
				// 正脊再高四分之一厚度，保持它压住斜脊端头的顺序，避免交汇处穿出交叉条纹。
				const double CapStandOff = (L.bLevel ? 0.75 : 0.5) * double(Thickness)
					+ FMath::Max(double(Params.LiftJitter), 0.0);
				const FVector Lift = L.Normal * (double(Params.StandOff) + CapStandOff);
				double S0 = 0.0;
				for (int32 Piece = 0; Piece + 1 < Points; ++Piece)
				{
					const double S1 = Piece + 2 == Points ? Length : Nominal * double(Piece + 1) + Wiggle * double(
						CSHouseVine::Hash01(CSHouseVine::IdentityHash(NumFaces + Line, Piece, 1, 76u, Params.Seed)) - 0.5f);
					FVector Dirs[3];
					Dirs[AxisUp] = World.TransformVectorNoScale(Dir).GetSafeNormal();
					Dirs[AxisNormal] = World.TransformVectorNoScale(L.Normal * double(Params.Axes.NormalSign)).GetSafeNormal();
					Dirs[AxisAlong] = FVector::CrossProduct(Dirs[AxisNormal], Dirs[AxisUp]).GetSafeNormal();
					if (FVector::DotProduct(FVector::CrossProduct(Dirs[0], Dirs[1]), Dirs[2]) < 0.0) Dirs[AxisAlong] = -Dirs[AxisAlong];
					float Sizes[3];
					Sizes[AxisUp] = float(L.bLevel ? (S1 - S0) * CSHouseTileLegacy_RidgeOverlap : S1 - S0 + 5.0);
					Sizes[AxisAlong] = (L.bLevel ? CSHouseTileLegacy_RidgeWidth * Params.RidgeCapScale : CSHouseTileLegacy_HipWidth) * SizeScale;
					Sizes[AxisNormal] = Thickness * (L.bLevel ? CSHouseTileLegacy_RidgeDepth : CSHouseTileLegacy_HipDepth);
					FVector Centred = World.TransformPosition(L.A + Dir * ((S0 + S1) * 0.5) + Lift);
					for (int32 Axis = 0; Axis < 3; ++Axis) Centred -= Dirs[Axis] * (double(Params.Axes.NativeCentre[Axis]) * Sizes[Axis] / FMath::Max(Params.Axes.NativeSize[Axis], 0.01f));
					CSHouseTile::FRecord& Rec = OutTiles.AddDefaulted_GetRef();
					Rec.WorldPos = FVector3f(Centred);
					Rec.Random01 = CSHouseVine::Hash01(CSHouseVine::IdentityHash(NumFaces + Line, Piece, 0, 71u, Params.Seed));
					Rec.AxisX = FVector3f(Dirs[0]);
					Rec.AxisY = FVector3f(Dirs[1]);
					Rec.AxisZ = FVector3f(Dirs[2]);
					Rec.SizeX = Sizes[0];
					Rec.SizeY = Sizes[1];
					Rec.SizeZ = Sizes[2];
					Rec.ClipPlane.X = L.bLevel && (Piece == 0 || Piece + 2 == Points) ? -1.0f : 0.0f;
					S0 = S1;
				}
			}

		}
	}
}

int32 CSHouseTileLegacy_MaxTilesBound(const FCSRoofDesc& Roof, const CSHouseTile::FParams& InParams)
{
	const FCSHouseFootprint& Footprint = Roof.Footprint;
	if (Roof.bFlat || !Footprint.IsValidFootprint()) return 0;

	const float Overhang = FMath::Max(Roof.Overhang, 0.0f);
	FCSRoofSkeleton Skeleton;
	CSHouseTileLegacy_BuildSkeleton(Footprint, double(Overhang), Skeleton);
	if (Skeleton.MaxInset <= 1.0) return 0;

	CSHouseTile::FParams Params = InParams;
	Params.Axes = CSHouseTileLegacy_SanitizeAxes(Params.Axes);

	const float SlopeLen = CSHouseTileLegacy_SlopeLength(Roof, Skeleton.MaxInset);
	// +1 是等分那一步的取整余量（`RoundToInt` 最多多给半格）。
	const int32 Rows = FMath::Clamp(FMath::CeilToInt(SlopeLen / CSHouseTileLegacy_RowPitch(Params)) + 1, 1, CSHouseTileLegacy_MaxRows);
	const float ColumnPitch = CSHouseTileLegacy_ColumnPitch(Params);

	int32 Total = 0;
	for (int32 Side = 0; Side < Footprint.NumEdges(); ++Side)
	{
		// 最宽的一排就是檐口外沿那一排：凸折线上每个坡面的排宽随内距单调收窄。
		double T0 = 0.0, T1 = 0.0;
		if (!CSHouseTileLegacy_FaceSpanAtInset(Footprint, Side, -double(Overhang), T0, T1)) continue;
		const int32 Columns = FMath::Clamp(
			FMath::CeilToInt((T1 - T0) / double(ColumnPitch)) + 1, 1, CSHouseTileLegacy_MaxColumns);
		Total += Rows * Columns;
	}

	// 脊瓦：骨架的每条弧（矩形上 4 条角斜脊 + 1 条屋脊）。容量是**一次预留、超了截断**（零阻塞纪律），
	// 漏算这一段的症状是"屋脊末端少几块盖瓦"，而瓦数、零阻塞、三角数全都正常 —— 只有出图看得见。
	if (InParams.RidgeCapScale > 0.0f)
	{
		const double TanP = double(Roof.TanPitch());
		for (const FCSRoofSkeletonArc& Arc : Skeleton.Arcs)
		{
			const double Length = FMath::Sqrt(FVector2D::DistSquared(Arc.A, Arc.B)
				+ FMath::Square((Arc.InsetB - Arc.InsetA) * TanP));
			// 水平脊和角斜脊都是 round(L / 62.5) 个分点，一段一块。
			Total += FMath::Clamp(FMath::CeilToInt(Length / CSHouseTileLegacy_RidgePiece) + 1, 1, CSHouseTileLegacy_MaxColumns);
		}
	}
	return Total;
}

// ===== 以上是冻结的基准 =====

bool CSHouseTileLegacy_SameRecord(const CSHouseTile::FRecord& A, const CSHouseTile::FRecord& B)
{
	return A.WorldPos == B.WorldPos && A.Random01 == B.Random01
		&& A.AxisX == B.AxisX && A.AxisY == B.AxisY && A.AxisZ == B.AxisZ
		&& A.SizeX == B.SizeX && A.SizeY == B.SizeY && A.SizeZ == B.SizeZ && A.ClipPlane == B.ClipPlane;
}

/** 失败时说清楚是哪几个字段、差多少（%.9g 足够还原一个 float）。 */
FString CSHouseTileLegacy_DescribeDifference(const CSHouseTile::FRecord& A, const CSHouseTile::FRecord& B)
{
	FString Out;
	auto Vec = [&Out](const TCHAR* Name, const FVector3f& L, const FVector3f& N)
	{
		if (L != N) Out += FString::Printf(TEXT(" %s (%.9g, %.9g, %.9g) -> (%.9g, %.9g, %.9g)"), Name, L.X, L.Y, L.Z, N.X, N.Y, N.Z);
	};
	auto Scalar = [&Out](const TCHAR* Name, float L, float N)
	{
		if (L != N) Out += FString::Printf(TEXT(" %s %.9g -> %.9g"), Name, L, N);
	};
	Vec(TEXT("WorldPos"), A.WorldPos, B.WorldPos);
	Scalar(TEXT("Random01"), A.Random01, B.Random01);
	Vec(TEXT("AxisX"), A.AxisX, B.AxisX);
	Vec(TEXT("AxisY"), A.AxisY, B.AxisY);
	Vec(TEXT("AxisZ"), A.AxisZ, B.AxisZ);
	Scalar(TEXT("SizeX"), A.SizeX, B.SizeX);
	Scalar(TEXT("SizeY"), A.SizeY, B.SizeY);
	Scalar(TEXT("SizeZ"), A.SizeZ, B.SizeZ);
	Scalar(TEXT("ClipPlane.X"), A.ClipPlane.X, B.ClipPlane.X);
	Scalar(TEXT("ClipPlane.Y"), A.ClipPlane.Y, B.ClipPlane.Y);
	return Out;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseTileEqualPitchMatchesLegacyTest,
	"PCGPlugins.ComputeShaderGenerator.House.TileEqualPitchMatchesLegacy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseTileEqualPitchMatchesLegacyTest::RunTest(const FString& Parameters)
{
	// ---- 屋面：矩形（长短轴各一、正方形、狭长）、陡 / 缓、零外挑 / 大外挑 / 负外挑、凸五边形、六边形 ----
	struct FRoofCase { const TCHAR* What; FCSRoofDesc Roof; };
	TArray<FRoofCase> Roofs;
	auto AddRect = [&Roofs](const TCHAR* What, double X, double Y, float Pitch, float Overhang, float EaveZ)
	{
		FCSRoofDesc Roof;
		Roof.Footprint = FCSHouseFootprint::MakeRect(FVector2D(X, Y));
		Roof.EaveZ = EaveZ;
		Roof.Pitch = Pitch;
		Roof.Overhang = Overhang;
		Roofs.Add({ What, Roof });
	};
	AddRect(TEXT("demo 600x400"), 600, 400, 35.0f, 25.0f, 312.0f);
	AddRect(TEXT("demo 1134x357"), 1134, 357, 35.0f, 25.0f, 312.0f);
	AddRect(TEXT("long axis Y"), 400, 600, 35.0f, 25.0f, 300.0f);
	AddRect(TEXT("square"), 400, 400, 20.0f, 0.0f, 300.0f);
	AddRect(TEXT("shallow, deep overhang"), 1000, 320, 15.0f, 60.0f, 250.0f);
	AddRect(TEXT("near square, steep"), 260, 250, 70.0f, 25.0f, 300.0f);
	AddRect(TEXT("tiny under a huge overhang"), 200, 200, 45.0f, 90.0f, 300.0f);
	AddRect(TEXT("negative overhang"), 600, 400, 35.0f, -10.0f, 300.0f);
	{
		FCSRoofDesc Roof;
		Roof.Footprint.Verts = { FVector2D(-320, -180), FVector2D(260, -220), FVector2D(380, 60), FVector2D(40, 290), FVector2D(-290, 150) };
		Roof.EaveZ = 300.0f;
		Roof.Pitch = 35.0f;
		Roof.Overhang = 25.0f;
		Roofs.Add({ TEXT("convex pentagon"), Roof });
		FCSRoofDesc Hex = Roof;
		Hex.Footprint.Verts.Reset();
		for (int32 I = 0; I < 6; ++I)
		{
			const double A = UE_DOUBLE_TWO_PI * double(I) / 6.0;
			Hex.Footprint.Verts.Add(FVector2D(300.0 * FMath::Cos(A), 300.0 * FMath::Sin(A)));
		}
		Roofs.Add({ TEXT("hexagon"), Hex });
	}

	// ---- 瓦参数：演示房的那一套（TG 尺寸的瓦、带脊瓦与瓦长抖动、枢轴不在中心）；单测夹具那一套；换轴的一套 ----
	struct FParamCase { const TCHAR* What; CSHouseTile::FParams Params; };
	TArray<FParamCase> ParamSets;
	{
		CSHouseTile::FParams Demo;
		Demo.RowPitch = 45.0f;
		Demo.ColumnPitch = 45.0f;
		Demo.RowOverlap = 1.43f;
		Demo.ColumnOverlap = 1.10f;
		Demo.Thickness = 13.0f;
		Demo.SizeScale = 1.0f;
		Demo.RidgeCapScale = 1.15f;
		Demo.StandOff = 0.5f;
		Demo.ScaleJitter = 0.0f;
		Demo.LengthJitter = 0.2f;
		Demo.YawJitter = 0.0f;
		Demo.LiftJitter = 0.6f;
		Demo.Seed = 1;
		Demo.Axes.UpSlope = 1;
		Demo.Axes.AlongRow = 0;
		Demo.Axes.Normal = 2;
		Demo.Axes.NativeSize = FVector3f(49.7064f, 64.0f, 13.0f);
		Demo.Axes.NativeCentre = FVector3f(0.0f, 0.0f, 1.5f);
		ParamSets.Add({ TEXT("demo tiles"), Demo });

		CSHouseTile::FParams Fixture;
		Fixture.RowPitch = 26.0f;
		Fixture.ColumnPitch = 30.0f;
		Fixture.RowOverlap = 1.6f;
		Fixture.ColumnOverlap = 1.06f;
		Fixture.StandOff = 0.0f;
		Fixture.ScaleJitter = 0.08f;
		Fixture.YawJitter = 0.05f;
		Fixture.LiftJitter = 0.7f;
		Fixture.Seed = 7;
		Fixture.RidgeCapScale = 0.0f;
		Fixture.Axes.NativeSize = FVector3f(20.0f, 24.0f, 3.0f);
		ParamSets.Add({ TEXT("fixture tiles, no caps"), Fixture });

		CSHouseTile::FParams Derived = Fixture;   // 间距由网格尺寸反解、法线轴翻面、带脊瓦
		Derived.RowPitch = 0.0f;
		Derived.ColumnPitch = 0.0f;
		Derived.RidgeCapScale = 0.9f;
		Derived.Axes.UpSlope = 2;
		Derived.Axes.AlongRow = 0;
		Derived.Axes.Normal = 1;
		Derived.Axes.NormalSign = -1.0f;
		Derived.Axes.NativeSize = FVector3f(24.0f, 3.0f, 20.0f);
		ParamSets.Add({ TEXT("derived pitches, permuted axes"), Derived });
	}

	const FTransform Worlds[] = { FTransform::Identity, FTransform(FRotator(0.0, 37.0, 0.0), FVector(1200.0, -800.0, 55.0)) };

	int32 Compared = 0;
	int32 Explained = 0;
	for (const FRoofCase& RoofCase : Roofs)
	{
		const FCSRoofDesc& Roof = RoofCase.Roof;

		// 骨架：旧签名与新的 desc 签名都要逐字段等于冻结的实现。
		FCSRoofSkeleton Legacy, ByFootprint, ByDesc;
		CSHouseTileLegacy_BuildSkeleton(Roof.Footprint, double(Roof.Overhang), Legacy);
		CSHouseRoof_BuildSkeleton(Roof.Footprint, double(Roof.Overhang), ByFootprint);
		CSHouseRoof_BuildSkeleton(Roof, ByDesc);
		for (const FCSRoofSkeleton* Now : { &ByFootprint, &ByDesc })
		{
			bool bSame = Legacy.Arcs.Num() == Now->Arcs.Num() && Legacy.NumHips == Now->NumHips && Legacy.MaxInset == Now->MaxInset
				&& Legacy.TopA == Now->TopA && Legacy.TopB == Now->TopB;
			for (int32 Index = 0; bSame && Index < Legacy.Arcs.Num(); ++Index)
			{
				const FCSRoofSkeletonArc& L = Legacy.Arcs[Index];
				const FCSRoofSkeletonArc& N = Now->Arcs[Index];
				bSame = L.A == N.A && L.B == N.B && L.InsetA == N.InsetA && L.InsetB == N.InsetB
					&& L.FaceA == N.FaceA && L.FaceB == N.FaceB && !N.bVerge;
			}
			TestTrue(FString::Printf(TEXT("[%s] the skeleton matches the frozen implementation bit for bit"), RoofCase.What), bSame);
		}

		// 坡面排区间：两个签名，沿坡取一串内距（含外挑段与收尖之后）。
		int32 SpanDiffers = 0;
		for (int32 Edge = 0; Edge < Roof.Footprint.NumEdges(); ++Edge)
		{
			for (double Inset = -double(FMath::Abs(Roof.Overhang)) - 5.0; Inset < Legacy.MaxInset + 20.0; Inset += 7.3)
			{
				double L0 = 0, L1 = 0, A0 = 0, A1 = 0, B0 = 0, B1 = 0;
				const bool bL = CSHouseTileLegacy_FaceSpanAtInset(Roof.Footprint, Edge, Inset, L0, L1);
				const bool bA = CSHouseRoof_FaceSpanAtInset(Roof.Footprint, Edge, Inset, A0, A1);
				const bool bB = CSHouseRoof_FaceSpanAtInset(Roof, Edge, Inset, B0, B1);
				if (bL != bA || bL != bB || (bL && (L0 != A0 || L1 != A1 || L0 != B0 || L1 != B1))) ++SpanDiffers;
			}
		}
		TestEqual(FString::Printf(TEXT("[%s] face rows match the frozen implementation bit for bit"), RoofCase.What), SpanDiffers, 0);

		for (const FParamCase& ParamCase : ParamSets)
		{
			TestEqual(FString::Printf(TEXT("[%s / %s] the capacity bound is unchanged"), RoofCase.What, ParamCase.What),
				CSHouseTile::MaxTilesBound(Roof, ParamCase.Params), CSHouseTileLegacy_MaxTilesBound(Roof, ParamCase.Params));

			for (const FTransform& World : Worlds)
			{
				TArray<CSHouseTile::FRecord> Old, New;
				CSHouseTileLegacy_BuildPlan(Roof, World, ParamCase.Params, Old);
				CSHouseTile::BuildPlan(Roof, World, ParamCase.Params, New);
				int32 Differ = Old.Num() == New.Num() ? 0 : FMath::Max(Old.Num(), New.Num());
				for (int32 Index = 0; Index < FMath::Min(Old.Num(), New.Num()); ++Index)
				{
					if (CSHouseTileLegacy_SameRecord(Old[Index], New[Index])) continue;
					// 只细说头几处：一处算错通常整面全错，刷几千行没有信息量。
					if (++Differ <= 2 && Explained++ < 12)
					{
						AddError(FString::Printf(TEXT("[%s / %s] tile %d of %d (now %d):%s"), RoofCase.What, ParamCase.What,
							Index, Old.Num(), New.Num(), *CSHouseTileLegacy_DescribeDifference(Old[Index], New[Index])));
					}
				}
				TestEqual(FString::Printf(TEXT("[%s / %s] every tile matches the frozen implementation bit for bit (%d tiles)"),
					RoofCase.What, ParamCase.What, Old.Num()), Differ, 0);
				Compared += Old.Num();
			}
		}
	}
	// 断言不是空转：确实比过成千上万片瓦。
	TestTrue(FString::Printf(TEXT("the comparison covered a real workload (%d tiles)"), Compared), Compared > 5000);
	return true;
}

#endif

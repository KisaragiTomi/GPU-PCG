#include "CSHouseRoof.h"

void CSHouseRoof_BuildParapet(const FCSRoofDesc& Roof, float Thickness, float Height, TArray<FCSRoofParapetBlock>& OutBlocks,
	TArrayView<const FCSRoofParapetOpening> Openings)
{
	OutBlocks.Reset();
	if (!Roof.bFlat || !Roof.Footprint.IsValidFootprint() || Height <= 0.0f) return;
	const double Depth = FMath::Clamp(double(Thickness), 12.0, FMath::Max(12.0, Roof.MaxInset() * 0.5));
	const double CourseHeight = double(Height) * 0.5;
	for (int32 Edge = 0; Edge < Roof.Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Roof.Footprint, 0.0f);
		double T0 = 0.0, T1 = 0.0;
		if (!CSHouseRoof_FaceSpanAtInset(Roof.Footprint, Edge, Depth * 0.5, T0, T1)) continue;
		const double Length = T1 - T0;
		const int32 Count = FMath::Clamp(FMath::RoundToInt(Length / 65.0), 1, 512);
		const double Step = Length / Count;
		const FQuat Rotation(FVector::UpVector, FMath::Atan2(F.U.Y, F.U.X));
		// 转角整砖盖住两条围边端面之间的缺口，顶面与垛口齐平。
		const FVector2D Corner = F.Start + F.In * (Depth * 0.5) + F.U * T0;
		OutBlocks.Add({ FVector(Corner.X, Corner.Y, Roof.EaveZ + Height * 0.5), FVector(Depth, Depth, Height), Rotation });
		auto AddBlock = [&](double Along, double Width, double Z)
		{
			TArray<FVector2D> Spans = { FVector2D(Along - Width * 0.5, Along + Width * 0.5) };
			for (const FCSRoofParapetOpening& Opening : Openings)
			{
				if (Opening.Edge != Edge || Opening.End <= Opening.Start) continue;
				TArray<FVector2D> Remaining;
				for (const FVector2D& Span : Spans)
				{
					if (Opening.End <= Span.X || Opening.Start >= Span.Y) Remaining.Add(Span);
					else
					{
						if (Opening.Start > Span.X) Remaining.Add(FVector2D(Span.X, Opening.Start));
						if (Opening.End < Span.Y) Remaining.Add(FVector2D(Opening.End, Span.Y));
					}
				}
				Spans = MoveTemp(Remaining);
			}
			for (const FVector2D& Span : Spans)
			{
				if (Span.Y - Span.X < 1.0) continue;
				const FVector2D XY = F.Start + F.In * (Depth * 0.5) + F.U * ((Span.X + Span.Y) * 0.5);
				OutBlocks.Add({ FVector(XY.X, XY.Y, Z), FVector(Span.Y - Span.X, Depth, CourseHeight), Rotation });
			}
		};
		for (int32 K = 0; K < Count; ++K) AddBlock(T0 + (K + 0.5) * Step, Step + 1.0, Roof.EaveZ + CourseHeight * 0.5);
		// 半砖垛在每条边两端接成转角，中间隔一个空档；无随机计数，尺寸拖动时稳定。
		const int32 Merlons = FMath::Clamp(FMath::RoundToInt(Length / 110.0), 1, 256);
		for (int32 K = 0; K <= Merlons; ++K)
		{
			const double Half = FMath::Min(Length / Merlons * 0.24, 28.0);
			const double A = FMath::Max(T0, T0 + Length * K / Merlons - Half);
			const double B = FMath::Min(T1, T0 + Length * K / Merlons + Half);
			AddBlock((A + B) * 0.5, B - A + 1.0, Roof.EaveZ + CourseHeight * 1.5);
		}
	}
}

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouseRoof_ 前缀。

/** 波前上的一个活动顶点：它夹在两条活动边之间，随内距 d 沿角平分线匀速走。 */
struct FCSHouseRoof_Vertex
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

/**
 * 「InA·x = A 且 InB·x = B」的解。两条内法线（近似）平行时没有解，返回零向量 ——
 * 波前上那是夹在两条平行反向的边之间的顶点：两条线已经重合，顶点不再移动（矩形脊两端就是这种顶点）。
 */
FVector2D CSHouseRoof_Solve(const FVector2D& InA, const FVector2D& InB, double A, double B)
{
	const double Det = InA.X * InB.Y - InA.Y * InB.X;
	if (FMath::Abs(Det) <= 1.0e-9) return FVector2D::ZeroVector;
	return FVector2D(A * InB.Y - B * InA.Y, B * InA.X - A * InB.X) / Det;
}

/**
 * 夹在两条活动边之间的波前顶点的速度（等效内距每增加 1 走多少）：两条边各以自己的 `Speed` 向内推。
 * 两边都是 1（等坡度）时走原来那条式子 —— 等坡度骨架的每一步浮点运算因此与加倍率之前相同。
 */
FVector2D CSHouseRoof_Velocity(const FVector2D& InA, const FVector2D& InB, double SpeedA, double SpeedB)
{
	if (SpeedA == 1.0 && SpeedB == 1.0)
	{
		const double Denom = 1.0 + FVector2D::DotProduct(InA, InB);
		// 两条边平行反向：它们之间的波前宽度恒定，顶点不再移动（矩形脊两端就是这种顶点）。
		if (Denom <= 1.0e-9) return FVector2D::ZeroVector;
		return (InA + InB) / Denom;
	}
	return CSHouseRoof_Solve(InA, InB, SpeedA, SpeedB);
}

/**
 * 骨架的核。波前按**等效内距** e 推进（`FCSRoofDesc::EquivalentInset`）：倍率 w 的坡面，它的波前线在
 * 内距 (e + Overhang) / w − Overhang 处，即以 1 / w 的速度向内走、e = 0 时停在 Overhang·(1 / w − 1)；
 * 山墙边的那张竖直面不动，恒在内距 −GableOverhang 处。`EdgeSteep` 为空 = 全 1。
 */
void CSHouseRoof_BuildSkeletonCore(const FCSHouseFootprint& Footprint, double Overhang,
	TConstArrayView<float> EdgeSteep, double GableOverhang, FCSRoofSkeleton& Out)
{
	Out = FCSRoofSkeleton();
	const int32 N = Footprint.NumEdges();
	if (N < 3) return;

	TArray<FVector2D, TInlineAllocator<16>> In;
	In.SetNum(N);
	TArray<FVector2D, TInlineAllocator<16>> U;
	U.SetNum(N);
	// 每条边的推进速度（山墙边 0）与 e = 0 时波前线所在的内距。
	TArray<double, TInlineAllocator<16>> Speed;
	Speed.SetNum(N);
	TArray<double, TInlineAllocator<16>> Rest;
	Rest.SetNum(N);
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
		if (F.Len <= 0.0f) return;   // 零长边：不是合法 footprint，输出空骨架
		In[Edge] = F.In;
		U[Edge] = F.U;

		const double W = EdgeSteep.IsValidIndex(Edge) ? double(EdgeSteep[Edge]) : 1.0;
		if (!(W > 0.0))
		{
			Speed[Edge] = 0.0;
			Rest[Edge] = -GableOverhang;
		}
		else if (W == 1.0)
		{
			Speed[Edge] = 1.0;
			Rest[Edge] = 0.0;
		}
		else
		{
			Speed[Edge] = 1.0 / W;
			Rest[Edge] = Overhang * (1.0 / W - 1.0);
		}
	}

	// 顶点 i 在 Verts[i]，夹在 i−1 号边与 i 号边之间 ⇒ 它是角 i−1（角 k 在 Verts[k+1]）。
	TArray<FCSHouseRoof_Vertex, TInlineAllocator<16>> Ring;
	for (int32 Index = 0; Index < N; ++Index)
	{
		FCSHouseRoof_Vertex V;
		V.EdgeIn = (Index + N - 1) % N;
		V.EdgeOut = Index;
		V.Corner = V.EdgeIn;
		V.Velocity = CSHouseRoof_Velocity(In[V.EdgeIn], In[V.EdgeOut], Speed[V.EdgeIn], Speed[V.EdgeOut]);
		V.P = Footprint.Verts[Index];
		// e = 0 时的波前顶点：两条线都停在墙外皮上（等坡度）时就是 footprint 顶点本身，否则是两条偏移线的交点。
		if (Rest[V.EdgeIn] != 0.0 || Rest[V.EdgeOut] != 0.0)
		{
			V.P += CSHouseRoof_Solve(In[V.EdgeIn], In[V.EdgeOut], Rest[V.EdgeIn], Rest[V.EdgeOut]);
		}
		// 角上的弧从檐口外角起算：等效内距 −Overhang 处的波前顶点（所有坡面的檐口外沿同高）。
		V.OriginP = V.P - V.Velocity * Overhang;
		V.OriginInset = -Overhang;
		Ring.Add(V);
	}

	TArray<FCSRoofSkeletonArc> Hips;
	Hips.SetNum(N);
	TArray<FCSRoofSkeletonArc> Ridges;

	auto Emit = [&Hips, &Ridges, &Speed](const FCSHouseRoof_Vertex& V, const FVector2D& EndP, double EndInset)
	{
		FCSRoofSkeletonArc Arc;
		Arc.A = V.OriginP;
		Arc.InsetA = V.OriginInset;
		Arc.B = EndP;
		Arc.InsetB = EndInset;
		Arc.FaceA = V.EdgeIn;
		Arc.FaceB = V.EdgeOut;
		Arc.bVerge = Speed[V.EdgeIn] == 0.0 || Speed[V.EdgeOut] == 0.0;
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
			const FCSHouseRoof_Vertex& V0 = Ring[Index];
			const FCSHouseRoof_Vertex& V1 = Ring[(Index + 1) % Ring.Num()];
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
		for (FCSHouseRoof_Vertex& V : Ring) V.P += V.Velocity * BestT;

		const int32 NextIndex = (Best + 1) % Ring.Num();
		const FCSHouseRoof_Vertex V0 = Ring[Best];
		const FCSHouseRoof_Vertex V1 = Ring[NextIndex];
		const FVector2D Node = (V0.P + V1.P) * 0.5;
		Emit(V0, Node, Now);
		Emit(V1, Node, Now);

		FCSHouseRoof_Vertex Merged;
		Merged.EdgeIn = V0.EdgeIn;
		Merged.EdgeOut = V1.EdgeOut;
		Merged.P = Node;
		Merged.OriginP = Node;
		Merged.OriginInset = Now;
		Merged.Velocity = CSHouseRoof_Velocity(In[Merged.EdgeIn], In[Merged.EdgeOut], Speed[Merged.EdgeIn], Speed[Merged.EdgeOut]);

		// 先覆盖再删：NextIndex 为 0 时删掉的是环首，覆盖过的那一格顺移一位，环序不变。
		Ring[Best] = Merged;
		Ring.RemoveAt(NextIndex);
	}

	Out.MaxInset = Now;
	for (const FCSHouseRoof_Vertex& V : Ring) Emit(V, V.P, Now);

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
}

void CSHouseRoof_BuildSkeleton(const FCSRoofDesc& Roof, FCSRoofSkeleton& Out)
{
	CSHouseRoof_BuildSkeletonCore(Roof.Footprint, double(Roof.Overhang), Roof.EdgeSteep, double(Roof.GableOverhang), Out);
}

void CSHouseRoof_BuildSkeleton(const FCSHouseFootprint& Footprint, double Overhang, FCSRoofSkeleton& Out)
{
	CSHouseRoof_BuildSkeletonCore(Footprint, Overhang, TConstArrayView<float>(), 0.0, Out);
}

bool CSHouseRoof_ApplyRidge(FCSRoofDesc& Desc, float Ridge01, int32 RidgeAxis, float InGableOverhang)
{
	Desc.EdgeSteep.Reset();
	Desc.GableOverhang = 0.0f;
	if (!CSHouseRoof_IsRectangle(Desc.Footprint)) return false;

	// 平行于脊的那对边 = 主坡面；另一对 = 两端。矩形上对边等长，各取一条量即可。
	const int32 Axis = RidgeAxis & 1;
	const int32 End = Axis ^ 1;
	const double Along = double(CSHouse_GetEdge(Axis, Desc.Footprint, 0.0f).Len);
	const double Across = double(CSHouse_GetEdge(End, Desc.Footprint, 0.0f).Len);
	const double Overhang = FMath::Max(double(Desc.Overhang), 0.0);

	Desc.EdgeSteep.Init(1.0f, 4);

	// TG 的放样面：端坡面从檐口矩形的端边（墙外皮外 Overhang）爬到脊端，水平进深 `Run`；
	// 主坡面从侧边爬到脊，进深 = 半跨 + Overhang。两者升到同一个脊高 ⇒ 倍率 = 进深之比。
	double RidgeLength = CSHouseRoof_RidgeLengthFrom01(double(Ridge01), Along);
	if (RidgeLength <= CSHouseRoof_RidgeMin) RidgeLength = 0.0;   // TG：不超过 0.1 m 的脊是一个尖
	const double Run = Along * 0.5 + Overhang - RidgeLength * 0.5;

	// `Roof::is_gable`：参数恰好到 1。外挑为 0 时差一点到 1 的端坡面进深趋零（倍率发散），一并按山墙收。
	if (Ridge01 >= 1.0f || Run <= 0.5)
	{
		Desc.EdgeSteep[End] = 0.0f;
		Desc.EdgeSteep[End + 2] = 0.0f;
		Desc.GableOverhang = InGableOverhang;
		return true;
	}

	const float Steep = float((Across * 0.5 + Overhang) / Run);
	Desc.EdgeSteep[End] = Steep;
	Desc.EdgeSteep[End + 2] = Steep;
	return true;
}

bool CSHouseRoof_BuildGableProfile(const FCSRoofDesc& Roof, int32 Edge, TArray<FVector2D>& OutProfile)
{
	OutProfile.Reset();
	const int32 N = Roof.Footprint.NumEdges();
	if (N < 3 || Edge < 0 || Edge >= N || !Roof.IsGable(Edge)) return false;
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Roof.Footprint, 0.0f);
	if (F.Len <= 0.0f) return false;

	// 沿墙线，每个坡面的等效内距都是 S 的一次函数 `A + B·S`；屋面是它们的下包络。
	struct FLine { double A; double B; };
	TArray<FLine, TInlineAllocator<16>> Lines;
	for (int32 Other = 0; Other < N; ++Other)
	{
		if (Other == Edge || Roof.IsGable(Other)) continue;
		const FCSHouseEdgeFrame G = CSHouse_GetEdge(Other, Roof.Footprint, 0.0f);
		if (G.Len <= 0.0f) continue;
		const double At0 = Roof.EquivalentInset(Other, FVector2D::DotProduct(F.Start - G.Start, G.In));
		const double At1 = Roof.EquivalentInset(Other, FVector2D::DotProduct(F.Start + F.U * double(F.Len) - G.Start, G.In));
		Lines.Add({ At0, (At1 - At0) / double(F.Len) });
	}
	if (Lines.IsEmpty()) return false;

	// 下包络的折点只可能出在两条线的交点上：N 是个位数，全列出来排序即可。
	TArray<double, TInlineAllocator<32>> Stops;
	Stops.Add(0.0);
	Stops.Add(double(F.Len));
	for (int32 I = 0; I < Lines.Num(); ++I)
	for (int32 J = I + 1; J < Lines.Num(); ++J)
	{
		const double Slope = Lines[I].B - Lines[J].B;
		if (FMath::Abs(Slope) <= 1.0e-12) continue;
		const double S = (Lines[J].A - Lines[I].A) / Slope;
		if (S > 1.0e-6 && S < double(F.Len) - 1.0e-6) Stops.Add(S);
	}
	Stops.Sort();

	auto Envelope = [&Lines](double S)
	{
		double Min = TNumericLimits<double>::Max();
		for (const FLine& L : Lines) Min = FMath::Min(Min, L.A + L.B * S);
		return Min;
	};
	for (const double S : Stops)
	{
		const FVector2D Point(S, FMath::Max(Envelope(S), 0.0));
		if (OutProfile.Num() > 0 && S - OutProfile.Last().X <= 1.0e-6) continue;   // 重合的交点
		// 与前两点共线的中间点（不在下包络上的交点）不留。
		while (OutProfile.Num() >= 2)
		{
			const FVector2D& P0 = OutProfile[OutProfile.Num() - 2];
			const FVector2D& P1 = OutProfile.Last();
			const double Cross = (P1.X - P0.X) * (Point.Y - P0.Y) - (P1.Y - P0.Y) * (Point.X - P0.X);
			if (FMath::Abs(Cross) > 1.0e-6 * FMath::Max(double(F.Len), 1.0)) break;
			OutProfile.Pop(EAllowShrinking::No);
		}
		OutProfile.Add(Point);
	}
	return OutProfile.Num() >= 2;
}

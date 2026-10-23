#include "CSGroundShaperField.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSGroundShaperField_ 前缀。

/** 足够大的"无穷"：平方距离用，512² 网格的最大平方距离是 2·511² ≈ 5.2e5，差五个数量级。 */
constexpr float CSGroundShaperField_Inf = 1.0e20f;

/**
 * Felzenszwalb & Huttenlocher 的一维平方距离变换（下包络抛物线）：
 * Out[q] = min_p ((q − p)² + F[p])。二维 = 先逐列再逐行各跑一趟，结果是精确欧氏平方距离。
 */
void CSGroundShaperField_Edt1D(const float* F, float* Out, int32 N, int32* V, double* Z)
{
	// 只把有限点放进下包络（无穷远点永远不会是最近的），省掉 INF − INF 的精度问题。
	int32 K = -1;
	for (int32 Q = 0; Q < N; ++Q)
	{
		if (F[Q] >= CSGroundShaperField_Inf) continue;
		double S = 0.0;
		while (K >= 0)
		{
			const int32 P = V[K];
			S = ((double(F[Q]) + double(Q) * Q) - (double(F[P]) + double(P) * P)) / (2.0 * double(Q - P));
			if (S > Z[K]) break;
			--K;
		}
		if (K < 0)
		{
			K = 0;
			V[0] = Q;
			Z[0] = -1.0e30;
			Z[1] = 1.0e30;
			continue;
		}
		++K;
		V[K] = Q;
		Z[K] = S;
		Z[K + 1] = 1.0e30;
	}
	if (K < 0)
	{
		for (int32 Q = 0; Q < N; ++Q) Out[Q] = CSGroundShaperField_Inf;
		return;
	}
	K = 0;
	for (int32 Q = 0; Q < N; ++Q)
	{
		while (Z[K + 1] < double(Q)) ++K;
		const double D = double(Q - V[K]);
		Out[Q] = float(D * D + double(F[V[K]]));
	}
}

/** 到"Seed 为真的 texel 中心"的欧氏距离（单位：格），N×N 行主序。 */
void CSGroundShaperField_Edt2D(const TArray<bool>& Seed, int32 N, TArray<float>& OutDist)
{
	TArray<float> Grid;
	Grid.SetNumUninitialized(N * N);
	for (int32 I = 0; I < N * N; ++I) Grid[I] = Seed[I] ? 0.0f : CSGroundShaperField_Inf;

	TArray<float> Line, Result;
	TArray<double> Z;
	TArray<int32> V;
	Line.SetNumUninitialized(N);
	Result.SetNumUninitialized(N);
	V.SetNumUninitialized(N);
	Z.SetNumUninitialized(N + 1);

	for (int32 X = 0; X < N; ++X)   // 逐列
	{
		for (int32 Y = 0; Y < N; ++Y) Line[Y] = Grid[Y * N + X];
		CSGroundShaperField_Edt1D(Line.GetData(), Result.GetData(), N, V.GetData(), Z.GetData());
		for (int32 Y = 0; Y < N; ++Y) Grid[Y * N + X] = Result[Y];
	}
	OutDist.SetNumUninitialized(N * N);
	for (int32 Y = 0; Y < N; ++Y)   // 逐行
	{
		CSGroundShaperField_Edt1D(&Grid[Y * N], Result.GetData(), N, V.GetData(), Z.GetData());
		for (int32 X = 0; X < N; ++X) OutDist[Y * N + X] = Result[X] >= CSGroundShaperField_Inf ? CSGroundShaperField_Inf : FMath::Sqrt(Result[X]);
	}
}

/** 双线性采 mask（像素中心在 +0.5，图外 = 0）。 */
float CSGroundShaperField_SampleMask(TConstArrayView<float> Pixels, int32 W, int32 H, double U, double V)
{
	const double Px = U * double(W) - 0.5;
	const double Py = V * double(H) - 0.5;
	const int32 X0 = FMath::FloorToInt32(Px);
	const int32 Y0 = FMath::FloorToInt32(Py);
	const double Tx = Px - double(X0);
	const double Ty = Py - double(Y0);
	auto At = [&](int32 X, int32 Y) -> double
	{
		return (X < 0 || Y < 0 || X >= W || Y >= H) ? 0.0 : double(Pixels[Y * W + X]);
	};
	const double A = FMath::Lerp(At(X0, Y0), At(X0 + 1, Y0), Tx);
	const double B = FMath::Lerp(At(X0, Y0 + 1), At(X0 + 1, Y0 + 1), Tx);
	return float(FMath::Lerp(A, B, Ty));
}

/** 可分离高斯，边缘复制。半径 ≤ 0 时原样返回。 */
void CSGroundShaperField_Blur(TArray<float>& Values, int32 N, float RadiusCells)
{
	const int32 R = FMath::CeilToInt32(RadiusCells);
	if (R <= 0) return;
	const double Sigma = FMath::Max(double(RadiusCells) / 3.0, 1.0e-3);
	TArray<float> Kernel;
	Kernel.SetNumUninitialized(2 * R + 1);
	double Sum = 0.0;
	for (int32 I = -R; I <= R; ++I)
	{
		const double W = FMath::Exp(-0.5 * double(I * I) / (Sigma * Sigma));
		Kernel[I + R] = float(W);
		Sum += W;
	}
	for (float& W : Kernel) W = float(double(W) / Sum);

	TArray<float> Tmp;
	Tmp.SetNumUninitialized(N * N);
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			double Acc = 0.0;
			for (int32 I = -R; I <= R; ++I) Acc += double(Kernel[I + R]) * double(Values[Y * N + FMath::Clamp(X + I, 0, N - 1)]);
			Tmp[Y * N + X] = float(Acc);
		}
	}
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			double Acc = 0.0;
			for (int32 I = -R; I <= R; ++I) Acc += double(Kernel[I + R]) * double(Tmp[FMath::Clamp(Y + I, 0, N - 1) * N + X]);
			Values[Y * N + X] = float(Acc);
		}
	}
}
}

namespace CSGroundShaperField
{
void PackField(TConstArrayView<FShaperHeader> Headers, TConstArrayView<TConstArrayView<float>> Sdfs, TArray<FVector4f>& OutPacked)
{
	const int32 Count = Headers.Num();
	int32 PoolFloats = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Sdfs.IsValidIndex(Index)) PoolFloats += Sdfs[Index].Num();
	}
	const int32 PoolStart = PrefixFloat4s + Count * Float4sPerShaper;

	OutPacked.Reset();
	OutPacked.SetNumZeroed(PoolStart + (PoolFloats + 3) / 4);
	// 下标以 float 存：2^24 以内是精确整数（512² 一座也只有 26 万）。
	OutPacked[0] = FVector4f(float(Count), float(PoolStart), 0.0f, 0.0f);

	int32 Offset = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FShaperHeader H = Headers[Index];
		const TConstArrayView<float> Sdf = Sdfs.IsValidIndex(Index) ? Sdfs[Index] : TConstArrayView<float>();
		const int32 N = int32(H.Noise.Z);
		if (N >= 2 && Sdf.Num() == N * N)
		{
			H.Noise.W = float(Offset);
			for (int32 T = 0; T < Sdf.Num(); ++T)
			{
				const int32 F = Offset + T;
				OutPacked[PoolStart + F / 4][F % 4] = Sdf[T];
			}
			Offset += Sdf.Num();
		}
		else
		{
			H.Noise.Z = 0.0f;   // 没有配套的 SDF ⇒ 解析圆盘
			H.Noise.W = 0.0f;
		}
		const int32 Base = PrefixFloat4s + Index * Float4sPerShaper;
		OutPacked[Base + 0] = H.Profile;
		OutPacked[Base + 1] = H.Top;
		OutPacked[Base + 2] = H.Noise;
		OutPacked[Base + 3] = H.Frame;
		OutPacked[Base + 4] = H.Shape;
	}
}

void BakeMaskSdf(TConstArrayView<float> MaskPixels, int32 Width, int32 Height,
	float MaskSize, float FalloffDistance, float Smoothing, float TargetCellSize, FMaskSdf& Out)
{
	Out = FMaskSdf();
	if (Width <= 0 || Height <= 0 || MaskSize <= 0.0f || MaskPixels.Num() < Width * Height) return;

	// 网格 = mask 本身 + 羽化 + 平滑半径 + 两格余量：羽化在网格里走完，网格外那条
	// "边缘值 + 到网格的距离"的外推只落在高度早已为 0 的地方。
	constexpr int32 MaxN = 512;
	const double Target = FMath::Max(double(TargetCellSize), 1.0);
	const double Pad = 0.5 * double(MaskSize) + FMath::Max(double(FalloffDistance), 0.0) + FMath::Max(double(Smoothing), 0.0);
	const int32 N = FMath::Clamp(FMath::CeilToInt32(2.0 * (Pad + 2.0 * Target) / Target) + 1, 16, MaxN);
	const double Cell = 2.0 * (Pad + 2.0 * Target) / double(N - 1);
	const double Half = 0.5 * double(N - 1);

	// ① 取样 + 二值化，在**细网格**上做：二值化把零等值线量化到半个格，直接在 SDF 格上做的话
	//    10 cm 格就是 ±5 cm 的轮廓误差，裙边斜率 1.5 一放大就是 7–8 cm 的高度误差（实测 8.3 cm，
	//    圆形 mask 对解析圆盘）。细网格每 Sub 格对齐一个粗格点，EDT 在细网格上做完直接按粗格点取值。
	const int32 Sub = FMath::Clamp(2048 / N, 1, 4);
	const int32 M = (N - 1) * Sub + 1;
	const double FineCell = Cell / double(Sub);
	const double FineHalf = 0.5 * double(M - 1);
	TArray<bool> Inside, Outside;
	Inside.SetNumUninitialized(M * M);
	Outside.SetNumUninitialized(M * M);
	int32 InsideCount = 0;
	for (int32 Y = 0; Y < M; ++Y)
	{
		for (int32 X = 0; X < M; ++X)
		{
			const double Lx = (double(X) - FineHalf) * FineCell;
			const double Ly = (double(Y) - FineHalf) * FineCell;
			const float Mask = CSGroundShaperField_SampleMask(MaskPixels, Width, Height,
				Lx / double(MaskSize) + 0.5, Ly / double(MaskSize) + 0.5);
			const bool bIn = Mask >= 0.5f;
			Inside[Y * M + X] = bIn;
			Outside[Y * M + X] = !bIn;
			InsideCount += bIn ? 1 : 0;
		}
	}

	Out.N = N;
	Out.CellSize = float(Cell);
	Out.Values.SetNumUninitialized(N * N);

	// 空 mask：整张都在外面，给一个远大于羽化的正距离 ⇒ 高度处处为 0（而不是退回解析圆盘）。
	if (InsideCount == 0)
	{
		for (float& V : Out.Values) V = float(4.0 * (Pad + 2.0 * Target));
		return;
	}

	// ② 精确 EDT：外面的点量到最近的里面 texel，里面的点量到最近的外面 texel；
	//    各减半格 ⇒ 零等值线落在内外 texel 中心的正中间。
	TArray<float> ToInside, ToOutside;
	CSGroundShaperField_Edt2D(Inside, M, ToInside);
	CSGroundShaperField_Edt2D(Outside, M, ToOutside);
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			const int32 I = (Y * Sub) * M + X * Sub;
			const float D = Inside[I]
				? -(FMath::Min(ToOutside[I], float(2 * M)) - 0.5f)
				: (FMath::Min(ToInside[I], float(2 * M)) - 0.5f);
			Out.Values[Y * N + X] = D * float(FineCell);
		}
	}

	// ③ 圆滑：距离场上的可分离高斯。下限 1.5 格只为抹掉二值化留下的锯齿（直边不受影响）。
	const double BlurRadius = FMath::Max(double(Smoothing), 1.5 * Cell);
	CSGroundShaperField_Blur(Out.Values, N, float(BlurRadius / Cell));

	// 零等值线的最远半径（多给一格）：足迹矩形、裙边摆件圈都拿它当上界。
	double MaxR2 = 0.0;
	float Deepest = 0.0f;
	for (int32 Y = 0; Y < N; ++Y)
	{
		for (int32 X = 0; X < N; ++X)
		{
			if (Out.Values[Y * N + X] > 0.0f) continue;
			// 最深处取格点上的最小值：双线性插值永远不低于格点最小值 ⇒ 隆起的峰恰好落在这个格点上、= 台高。
			Deepest = FMath::Max(Deepest, -Out.Values[Y * N + X]);
			const double Lx = (double(X) - Half) * Cell;
			const double Ly = (double(Y) - Half) * Cell;
			MaxR2 = FMath::Max(MaxR2, Lx * Lx + Ly * Ly);
		}
	}
	Out.ContourRadius = float(FMath::Sqrt(MaxR2) + Cell);
	Out.DeepestInside = Deepest;
}
}

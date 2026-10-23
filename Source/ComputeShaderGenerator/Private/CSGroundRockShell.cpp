#include "CSGroundRockShell.h"

#include "CSGpuMeshTypes.h"
#include "CSGroundRoadField.h"
#include "CSGroundShaperField.h"
#include "CSMeshOps.h"
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSRockShell, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSRockShell_ 前缀（与 CSGround_ / CSStairs_ /
// CSShaperSteps_ 必须都不同，否则 unity blob 里同名符号打架，而报错位置会指向一个跟改动无关的文件）。

constexpr int32 CSRockShell_GroupSizeX = 64;
/** usf 的 ROCKSHELL_MAX_POLY：裁剪中间结果的顶点上限。CPU 孪生同一个上限，两侧才会在同一处截断。 */
constexpr int32 CSRockShell_MaxPoly = 32;
/** 一维派发的组数上限（D3D12 每维 65535）。槽位池上限按它反推，见 ACSGroundActor::RockShellMaxCells。 */
constexpr int32 CSRockShell_MaxGroups = 65535;

// -----------------------------------------------------------------------------
// shader 声明。⚠️ 每个参数结构只放本趟 kernel 真读的东西：`FComputeShaderUtils::AddPass` 会调
// `ClearUnusedGraphResources` 把没读的 RDG 绑定置空、连带抹掉那条依赖边（藤蔓 SC pass 就是这么挂过 GPU 的）。
// -----------------------------------------------------------------------------

#define CSROCKSHELL_PATTERN_PARAMETERS() \
	SHADER_PARAMETER(float, RockShellCellSize) \
	SHADER_PARAMETER(float, RockShellSeedJitter) \
	SHADER_PARAMETER(float, RockShellClusterWarp) \
	SHADER_PARAMETER(float, RockShellClusterWavelength) \
	SHADER_PARAMETER(float, RockShellSizeVariance) \
	SHADER_PARAMETER(uint32, RockShellPatternSeed) \
	SHADER_PARAMETER(FVector2f, RockShellGridOrigin) \
	SHADER_PARAMETER(float, RockShellLipCm)

class FCSRockShellBuildCellsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellBuildCellsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellBuildCellsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		CSROCKSHELL_PATTERN_PARAMETERS()
		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER(uint32, RockShellGeomStride)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, RockShellNewCells)
		SHADER_PARAMETER(uint32, RockShellNewCellCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_RockShellCellGeom)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSRockShellBuildCellsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "BuildRockShellCellsCS", SF_Compute);

class FCSRockShellWriteStaticCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellWriteStaticCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellWriteStaticCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, RockShellPatternSeed)
		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER(uint32, RockShellGeomStride)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, RockShellNewCells)
		SHADER_PARAMETER(uint32, RockShellNewCellCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, RockShellCellGeom)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RockShellColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellTexCoords)
		SHADER_PARAMETER(uint32, RockShellTexCoordStride)
		SHADER_PARAMETER(float, RockShellInvUVWorldPeriod)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSRockShellWriteStaticCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "WriteRockShellStaticCS", SF_Compute);

class FCSRockShellKillCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellKillCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellKillCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellJobs)
		SHADER_PARAMETER(uint32, RockShellJobCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellPositions)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSRockShellKillCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "KillRockShellSlotsCS", SF_Compute);

class FCSGroundRockShellCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundRockShellCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundRockShellCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// 名字必须与 CSGroundShaperField.ush 里的声明逐字相同：这两个是与地面位移 pass 共享的
		// 那一份高度场的输入，改名等于把绑定悄悄拆掉（不报错，只是地面永远读成平的）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundShaperParams)
		SHADER_PARAMETER(uint32, GroundShaperCount)

		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER(uint32, RockShellGeomStride)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, RockShellCellGeom)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellJobs)
		SHADER_PARAMETER(uint32, RockShellJobCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellRoadField)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RockShellTangents)

		SHADER_PARAMETER(FVector2f, RockShellDomainMin)
		SHADER_PARAMETER(FVector2f, RockShellDomainMax)
		SHADER_PARAMETER(float, RockShellCellRadiusCm)
		SHADER_PARAMETER(FVector2f, RockShellGroundOriginXY)
		SHADER_PARAMETER(float, RockShellGroundCellSize)
		SHADER_PARAMETER(FUintVector2, RockShellGroundVerts)
		SHADER_PARAMETER(float, RockShellGroundBaseZ)
		SHADER_PARAMETER(float, RockShellSlopeLo)
		SHADER_PARAMETER(float, RockShellSlopeHi)
		SHADER_PARAMETER(float, RockShellRoadSink)
		SHADER_PARAMETER(float, RockShellCellJitter)
		SHADER_PARAMETER(float, RockShellCellRelief)
		SHADER_PARAMETER(float, RockShellReliefFloor)
		SHADER_PARAMETER(float, RockShellCellExpand)
		SHADER_PARAMETER(float, RockShellSkirtTilt)
		SHADER_PARAMETER(float, RockShellBaseLift)
		SHADER_PARAMETER(float, RockShellBaseSink)
		SHADER_PARAMETER(float, RockShellNoiseAmp)
		SHADER_PARAMETER(float, RockShellNoiseFreq)
		SHADER_PARAMETER(float, RockShellRiseMultiplier)
		SHADER_PARAMETER(float, RockShellRiseNoiseAmp)
		SHADER_PARAMETER(float, RockShellRiseNoiseFreq)
		SHADER_PARAMETER(float, RockShellRiseExtend)
		SHADER_PARAMETER(float, RockShellEdgeCeiling)
		SHADER_PARAMETER(float, RockShellPeakHeight)
		SHADER_PARAMETER(float, RockShellChipAmount)
		SHADER_PARAMETER(float, RockShellChipFreq)
		SHADER_PARAMETER(uint32, RockShellSeed)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSGroundRockShellCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "DisplaceRockShellCS", SF_Compute);

/**
 * 第二趟：法线平均（`bRockShellSmoothNormals`，带夹角阈值）。一线程一角。
 * 为什么不在第一趟里顺手算：面法线要三个角的**最终**位置，而邻居分散在别的线程组里。
 * 分成两趟由 RDG 靠 Positions 的 UAV→SRV 转换定序，比手工插 barrier 少一份会漂掉的规则。
 */
class FCSRockShellAverageNormalsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellAverageNormalsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellAverageNormalsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellPositionsRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellTemplateIncidentRange)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellTemplateIncidentTris)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellJobs)
		SHADER_PARAMETER(uint32, RockShellJobCount)
		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_RockShellTangents)
		SHADER_PARAMETER(float, RockShellSmoothCosThreshold)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSRockShellAverageNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "AverageRockShellNormalsCS", SF_Compute);

/** 第三趟：假倒角 v3 的逐像素载荷（垂距 one-hot + 邻面 / 自身法线）写进 UV1..UV6。 */
class FCSRockShellBevelPayloadCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSRockShellBevelPayloadCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRockShellBevelPayloadCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RockShellPositionsRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<int>, RockShellTemplateNeighbours)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RockShellJobs)
		SHADER_PARAMETER(uint32, RockShellJobCount)
		SHADER_PARAMETER(uint32, RockShellBoundaryPoints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RockShellTexCoords)
		SHADER_PARAMETER(uint32, RockShellTexCoordStride)
	END_SHADER_PARAMETER_STRUCT()

	CSGEN_SHADER_PERM_SM5()
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSRockShell_GroupSizeX);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSRockShellBevelPayloadCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRockShell.usf", "RockShellBevelPayloadCS", SF_Compute);

#undef CSROCKSHELL_PATTERN_PARAMETERS

/** 一维派发（组数 ≤ 65535，调用方已按上限截过作业数）。 */
FIntVector CSRockShell_Groups(uint64 Threads)
{
	const uint64 Groups = (Threads + uint64(CSRockShell_GroupSizeX) - 1u) / uint64(CSRockShell_GroupSizeX);
	return FIntVector(int32(FMath::Clamp<uint64>(Groups, 1u, uint64(CSRockShell_MaxGroups))), 1, 1);
}

template <typename T>
TArray<T> CSRockShell_NonEmpty(const TArray<T>& In, const T& Pad)
{
	// 结构化 buffer 不能是 0 长度；计数另传，kernel 按计数截，垫的那一个永远读不到。
	if (!In.IsEmpty()) return In;
	TArray<T> Out;
	Out.Add(Pad);
	return Out;
}

/** 与 usf `BuildRockShellCellsCS` 裁剪段逐行对应的 CPU 孪生（只给激活用：外接半径）。 */
bool CSRockShell_ClipCell(const CSRockShell::FPatternParams& P, int32 I, int32 J, TArray<FVector2f>& Out)
{
	const float S = P.CellSize;
	const FVector2f Pp = CSRockShell::SeedLocal(P, I, J);
	const float Wp = CSRockShell::SeedWeight(P, I, J);
	TArray<FVector2f, TInlineAllocator<CSRockShell_MaxPoly>> A;
	TArray<FVector2f, TInlineAllocator<CSRockShell_MaxPoly>> B;
	A.Add(FVector2f(-3.0f * S, -3.0f * S));
	A.Add(FVector2f( 3.0f * S, -3.0f * S));
	A.Add(FVector2f( 3.0f * S,  3.0f * S));
	A.Add(FVector2f(-3.0f * S,  3.0f * S));
	const int32 R = CSRockShell::NeighbourRadius;
	for (int32 Dj = -R; Dj <= R; ++Dj)
	{
		for (int32 Di = -R; Di <= R; ++Di)
		{
			if (Di == 0 && Dj == 0) continue;
			const FVector2f Pq = CSRockShell::SeedLocal(P, I + Di, J + Dj);
			const FVector2f D = (FVector2f(float(Di), float(Dj)) + Pq - Pp) * S;
			const FVector2f Nrm = D * 2.0f;
			const float Cst = FVector2f::DotProduct(D, D) + Wp - CSRockShell::SeedWeight(P, I + Di, J + Dj);
			B.Reset();
			const int32 NA = A.Num();
			for (int32 K = 0; K < NA; ++K)
			{
				const FVector2f Pa = A[K];
				const FVector2f Pb = A[(K + 1 == NA) ? 0 : K + 1];
				const float Da = FVector2f::DotProduct(Nrm, Pa) - Cst;
				const float Db = FVector2f::DotProduct(Nrm, Pb) - Cst;
				if (Da <= 0.0f && B.Num() < CSRockShell_MaxPoly) B.Add(Pa);
				if ((Da <= 0.0f) != (Db <= 0.0f) && B.Num() < CSRockShell_MaxPoly)
				{
					const float T = Da / (Da - Db);
					B.Add(Pa + (Pb - Pa) * T);
				}
			}
			A = B;
			if (A.Num() < 3) return false;
		}
	}
	Out.Reset();
	for (const FVector2f& V : A)
	{
		if (Out.IsEmpty() || (V - Out.Last()).Size() > 0.01f) Out.Add(V);
	}
	if (Out.Num() > 1 && (Out[0] - Out.Last()).Size() <= 0.01f) Out.Pop();
	while (Out.Num() > P.BoundaryPoints && Out.Num() > 3)
	{
		int32 Shortest = 0;
		float Best = TNumericLimits<float>::Max();
		for (int32 K = 0; K < Out.Num(); ++K)
		{
			const float L = (Out[(K + 1 == Out.Num()) ? 0 : K + 1] - Out[K]).Size();
			if (L < Best) { Best = L; Shortest = K; }
		}
		const int32 Next = (Shortest + 1 == Out.Num()) ? 0 : Shortest + 1;
		Out[Shortest] = (Out[Shortest] + Out[Next]) * 0.5f;
		Out.RemoveAt(Next);
	}
	return Out.Num() >= 3;
}
}

namespace CSRockShell
{
FPatternParams FPatternParams::Sanitized() const
{
	FPatternParams Out = *this;
	Out.CellSize = FMath::Clamp(CellSize, 20.0f, 5000.0f);
	Out.SeedJitter = FMath::Clamp(SeedJitter, 0.0f, 1.0f);
	Out.ClusterWarp = FMath::Clamp(ClusterWarp, 0.0f, 1.0f);
	Out.ClusterWavelength = FMath::Clamp(ClusterWavelength, 1.0f, 64.0f);
	Out.SizeVariance = FMath::Clamp(SizeVariance, 0.0f, 1.0f);
	Out.BoundaryPoints = FMath::Clamp(BoundaryPoints, MinBoundaryPoints, MaxBoundaryPoints);
	return Out;
}

uint32 FPatternParams::GeometryHash() const
{
	const FPatternParams P = Sanitized();
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };
	uint32 H = ::GetTypeHash(P.BoundaryPoints);
	H = HashFloat(H, P.CellSize);
	H = HashFloat(H, P.SeedJitter);
	H = HashFloat(H, P.ClusterWarp);
	H = HashFloat(H, P.ClusterWavelength);
	H = HashFloat(H, P.SizeVariance);
	H = HashCombine(H, ::GetTypeHash(P.Seed));
	return H;
}

FVector2f SeedLocal(const FPatternParams& P, int32 I, int32 J)
{
	// 与 usf `CSRockShell_SeedLocal` 逐字对应（格键按位转 uint，负坐标照样有定义）。
	const uint32 Cx = uint32(I);
	const uint32 Cy = uint32(J);
	const float U = CSGroundShaperField::Hash01(Cx, Cy, P.Seed * 3u + 1u);
	const float V = CSGroundShaperField::Hash01(Cx, Cy, P.Seed * 3u + 2u);
	FVector2f Out(0.5f + P.SeedJitter * (U - 0.5f), 0.5f + P.SeedJitter * (V - 0.5f));
	if (P.ClusterWarp > 0.0f)
	{
		const float Inv = 1.0f / FMath::Max(P.ClusterWavelength, 1e-3f);
		const float Gx = (float(I) + Out.X) * Inv;
		const float Gy = (float(J) + Out.Y) * Inv;
		const float Wx = CSGroundShaperField::ValueNoise(Gx, Gy, P.Seed * 7u + 101u);
		const float Wy = CSGroundShaperField::ValueNoise(Gx + 17.31f, Gy - 9.77f, P.Seed * 7u + 103u);
		Out.X += Wx * P.ClusterWarp;
		Out.Y += Wy * P.ClusterWarp;
	}
	return Out;
}

float SeedWeight(const FPatternParams& P, int32 I, int32 J)
{
	const float R = P.SizeVariance * P.CellSize * CSGroundShaperField::Hash01(uint32(I), uint32(J), P.Seed * 3u + 3u);
	return R * R;
}

FVector2D SeedWorld(const FPatternParams& P, const FVector2D& GridOrigin, int32 I, int32 J)
{
	const FVector2f L = SeedLocal(P, I, J);
	return GridOrigin + FVector2D(double(I) + double(L.X), double(J) + double(L.Y)) * double(P.CellSize);
}

bool CellPolygon(const FPatternParams& InParams, int32 I, int32 J, TArray<FVector2f>& OutLocal)
{
	return CSRockShell_ClipCell(InParams.Sanitized(), I, J, OutLocal);
}

float CellCircumradius(const FPatternParams& Params, int32 I, int32 J)
{
	TArray<FVector2f> Poly;
	if (!CellPolygon(Params, I, J, Poly)) return -1.0f;
	float R = 0.0f;
	for (const FVector2f& V : Poly) R = FMath::Max(R, V.Size());
	return R;
}

uint32 FTemplate::CornerVertex(int32 M, int32 LocalTri, int32 Corner)
{
	// 与 usf `CSRockShell_CornerVertex` 逐字对应（五个三角俯视顺时针，顺序注释在 usf）。
	const uint32 Mu = uint32(M);
	const uint32 K = uint32(LocalTri) / 5u;
	const uint32 R = uint32(LocalTri) - K * 5u;
	const uint32 N = (K + 1u >= Mu) ? 0u : K + 1u;
	const uint32 Rg = 1u;
	const uint32 Tt = 1u + Mu;
	const uint32 Bb = 1u + 2u * Mu;
	const uint32 C = uint32(Corner);
	if (R == 0u) return C == 0u ? 0u     : (C == 1u ? Rg + N : Rg + K);
	if (R == 1u) return C == 0u ? Rg + K : (C == 1u ? Tt + N : Tt + K);
	if (R == 2u) return C == 0u ? Rg + K : (C == 1u ? Rg + N : Tt + N);
	if (R == 3u) return C == 0u ? Tt + K : (C == 1u ? Bb + N : Bb + K);
	return              C == 0u ? Tt + K : (C == 1u ? Tt + N : Bb + N);
}

FTemplate FTemplate::Build(int32 InM)
{
	FTemplate Out;
	Out.M = FMath::Clamp(InM, MinBoundaryPoints, MaxBoundaryPoints);
	const int32 Tris = 5 * Out.M;
	const int32 Corners = 3 * Tris;
	const int32 Vertices = 1 + 3 * Out.M;

	// 入射表：同一个模板顶点 = 一个重合组（槽内），石头与石头之间天然没有组。
	TArray<TArray<uint32>> ByVertex;
	ByVertex.SetNum(Vertices);
	for (int32 T = 0; T < Tris; ++T)
	{
		for (int32 K = 0; K < 3; ++K) ByVertex[int32(CornerVertex(Out.M, T, K))].Add(uint32(T));
	}
	TArray<uint32> VertexOffset;
	VertexOffset.SetNum(Vertices);
	for (int32 V = 0; V < Vertices; ++V)
	{
		VertexOffset[V] = uint32(Out.IncidentTris.Num());
		Out.IncidentTris.Append(ByVertex[V]);
	}
	Out.IncidentRange.SetNum(Corners * 2);
	for (int32 T = 0; T < Tris; ++T)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			const int32 V = int32(CornerVertex(Out.M, T, K));
			Out.IncidentRange[(T * 3 + K) * 2 + 0] = VertexOffset[V];
			Out.IncidentRange[(T * 3 + K) * 2 + 1] = uint32(ByVertex[V].Num());
		}
	}

	// 逐边邻接：槽 k 对着角 k，= 边 (v[k+1], v[k+2])；边由两端模板顶点的无序对定义。
	Out.Neighbours.Init(-1, Tris * 3);
	TMap<uint64, int32> FirstSlot;
	for (int32 T = 0; T < Tris; ++T)
	{
		for (int32 K = 0; K < 3; ++K)
		{
			const uint32 A = CornerVertex(Out.M, T, (K + 1) % 3);
			const uint32 B = CornerVertex(Out.M, T, (K + 2) % 3);
			const uint64 Key = (uint64(FMath::Min(A, B)) << 32) | uint64(FMath::Max(A, B));
			if (int32* First = FirstSlot.Find(Key))
			{
				Out.Neighbours[T * 3 + K] = *First / 3;
				Out.Neighbours[*First] = T;
			}
			else
			{
				FirstSlot.Add(Key, T * 3 + K);
			}
		}
	}
	return Out;
}

const FTemplate& GetTemplate(int32 M)
{
	// 游戏线程独占（所有调用方都在 actor 的重建路径上），不需要锁。M 只有 13 种取值。
	static TMap<int32, FTemplate> Cache;
	const int32 Key = FMath::Clamp(M, MinBoundaryPoints, MaxBoundaryPoints);
	if (const FTemplate* Found = Cache.Find(Key)) return *Found;
	return Cache.Add(Key, FTemplate::Build(Key));
}

void CollectActiveCells(const FPatternParams& InParams, const FVector2D& GridOrigin, const FBox2D& Domain,
	TConstArrayView<FVector4f> ShaperParams, float MarginCm, float AliveSlope,
	TMap<FIntPoint, float>& InOutRadiusCache, TArray<FIntPoint>& OutCells)
{
	OutCells.Reset();
	const FPatternParams P = InParams.Sanitized();
	const double S = double(P.CellSize);
	const int32 NumShapers = CSGroundShaperField::NumShapers(ShaperParams);
	if (NumShapers <= 0 || !Domain.bIsValid) return;

	// 外接半径的粗上界：x 落在自己那格的种子 ≤ √2·S 之内（抖动满格），扭曲再挪 A 格、功率权重再撑 V 格。
	const double RcBound = (UE_DOUBLE_SQRT_2 + double(P.ClusterWarp) + double(P.SizeVariance)) * S;
	const double Margin = FMath::Max(double(MarginCm), 0.0);
	const double LambdaMax = 1.0 + UE_DOUBLE_SQRT_2;
	TSet<FIntPoint> Active;

	for (int32 Shaper = 0; Shaper < NumShapers; ++Shaper)
	{
		const CSGroundShaperField::FShaperHeader H = CSGroundShaperField::GetHeader(ShaperParams, Shaper);
		const float Peak = CSGroundShaperField::PeakHeight(H.Top);
		if (FMath::Abs(Peak) < 1e-3f) continue;   // 零高度的座不产生坡

		const FVector2D Centre(H.Profile.X, H.Profile.Y);
		const double Falloff = FMath::Max(double(H.Profile.W), 1e-3);
		const bool bMask = H.Noise.Z >= 2.0f;
		const uint32 PoolOffset = uint32(FMath::Max(H.Noise.W, 0.0f));
		auto Fetch = [ShaperParams, PoolOffset](uint32 Index) { return CSGroundShaperField::FetchPool(ShaperParams, PoolOffset + Index); };

		// 中心隆起把平台变成山丘：坡度上界 = ρ (1 + Lift) |H| × 1.5 / (Falloff + 最深距离)，
		// 过阈（留两成余量）才激活整盘，否则只激活环带。
		const float Lift = FMath::Max(H.Top.W, 0.0f);
		const float Bulge = FMath::Clamp(H.Shape.X, 0.0f, 1.0f);
		const float HillSlope = Bulge * (1.0f + Lift) * FMath::Abs(H.Top.X) * 1.5f
			/ (float(Falloff) + FMath::Max(H.Shape.Y, 1.0f)) * (bMask ? UE_SQRT_2 : 1.0f);
		const bool bInterior = Bulge > 0.0f && HillSlope > AliveSlope * 0.8f;
		const double LowerBound = bInterior ? -TNumericLimits<double>::Max() : 0.0;

		// 触及范围：零等值线最远半径 + 羽化 + 一个 SDF 格（插值余量）。
		const double SdfCell = bMask ? double(H.Frame.W) : 0.0;
		const double Reach = double(H.Profile.Z) + Falloff + SdfCell;
		const double Pad = Reach + LambdaMax * RcBound + Margin;
		FBox2D Box(Centre - FVector2D(Pad), Centre + FVector2D(Pad));
		const FBox2D DomainPad(Domain.Min - FVector2D(RcBound + Margin), Domain.Max + FVector2D(RcBound + Margin));
		Box.Min = FVector2D::Max(Box.Min, DomainPad.Min);
		Box.Max = FVector2D::Min(Box.Max, DomainPad.Max);
		if (Box.Min.X > Box.Max.X || Box.Min.Y > Box.Max.Y) continue;

		const int32 I0 = FMath::FloorToInt32((Box.Min.X - GridOrigin.X) / S) - 1;
		const int32 I1 = FMath::FloorToInt32((Box.Max.X - GridOrigin.X) / S) + 1;
		const int32 J0 = FMath::FloorToInt32((Box.Min.Y - GridOrigin.Y) / S) - 1;
		const int32 J1 = FMath::FloorToInt32((Box.Max.Y - GridOrigin.Y) / S) + 1;
		for (int32 J = J0; J <= J1; ++J)
		{
			for (int32 I = I0; I <= I1; ++I)
			{
				const FIntPoint Key(I, J);
				if (Active.Contains(Key)) continue;
				const FVector2D Seed = SeedWorld(P, GridOrigin, I, J);

				// λ：解析圆盘 1；mask 在 SDF 网格内 √2（双线性对角最坏）、网格外 1 + √2（再加一项到网格的距离）。
				double Lambda = 1.0;
				if (bMask)
				{
					const double Dx = Seed.X - Centre.X;
					const double Dy = Seed.Y - Centre.Y;
					const double Lx = Dx * H.Frame.X + Dy * H.Frame.Y;
					const double Ly = Dy * H.Frame.X - Dx * H.Frame.Y;
					const double Half = (double(H.Noise.Z) - 1.0) * 0.5;
					const double Gx = Lx * H.Frame.Z + Half;
					const double Gy = Ly * H.Frame.Z + Half;
					const bool bInGrid = Gx >= 0.0 && Gy >= 0.0 && Gx <= double(H.Noise.Z) - 1.0 && Gy <= double(H.Noise.Z) - 1.0;
					Lambda = bInGrid ? UE_DOUBLE_SQRT_2 : LambdaMax;
				}
				const double Sd = double(CSGroundShaperField::SignedDistance(FVector2f(Seed), H, Fetch));

				// 粗筛（外接半径上界）→ 精判（这一格的真外接半径，按格键缓存）。
				auto Hits = [&](double Rc)
				{
					if (Sd - Lambda * Rc - Margin > Falloff) return false;
					if (Sd + Lambda * Rc + Margin < LowerBound) return false;
					// 与地面矩形求交（kernel 里还有一道逐三角的域判据）
					const double Ex = FMath::Max3(Domain.Min.X - Seed.X, 0.0, Seed.X - Domain.Max.X);
					const double Ey = FMath::Max3(Domain.Min.Y - Seed.Y, 0.0, Seed.Y - Domain.Max.Y);
					return Ex * Ex + Ey * Ey <= FMath::Square(Rc + Margin);
				};
				if (!Hits(RcBound)) continue;
				float* Cached = InOutRadiusCache.Find(Key);
				const float Rc = Cached ? *Cached : InOutRadiusCache.Add(Key, CellCircumradius(P, I, J));
				if (Rc < 0.0f) continue;   // 空胞腔（功率图）
				if (!Hits(double(Rc))) continue;
				Active.Add(Key);
			}
		}
	}

	OutCells = Active.Array();
	OutCells.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
}

bool BuildMesh(UCSMesh* ShellMesh, const FPatternParams& InPattern, int32 CapacityCells, const FBox& HardWorldBounds)
{
	if (!ShellMesh || CapacityCells <= 0) return false;
	const FPatternParams Pattern = InPattern.Sanitized();
	const int32 VertsPerCell = Pattern.VerticesPerCell();
	const int64 NumVerts64 = int64(CapacityCells) * int64(VertsPerCell);
	if (NumVerts64 > int64(MAX_int32)) return false;
	const int32 NumVerts = int32(NumVerts64);

	// --- 1) 声明流集：标准集（7 组 UV）+ 胞腔几何流。**在第一次分配之前声明**，下一次分配就按它建。 ---
	FCSMeshStreamLayout Layout;
	Layout.NumTexCoordSets = uint32(TexCoord::NumSets);
	{
		FCSGpuStreamDesc Desc;
		Desc.DebugName = TEXT("CSRockShell.CellGeom");
		Desc.Role = ECSGpuStreamRole::AuxVertex;
		Desc.BytesPerElement = sizeof(FVector4f);
		Desc.ElementsPerUnit = uint32(CapacityCells) * uint32(Pattern.GeomFloat4sPerCell());
		Desc.CountSource = ECSGpuCountSource::Fixed;
		Desc.SrvFormat = PF_A32B32G32R32F;            // -> Buffer<float4>
		Desc.VfType = VET_None;                       // aux 不进顶点工厂
		Desc.TexCoordIndex = uint8(EAuxSlot::CellGeom);
		Layout.ExtraStreams.Add(Desc);
	}
	if (!ShellMesh->SetStreamLayoutSync(Layout))
	{
		// AddStream 的槽位冲撞只返回 false 且不写日志 —— 这里必须自己喊出来。
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] aux 流声明被拒（槽位 32 冲撞或显存预检不过），岩壳关闭。"));
		return false;
	}

	// --- 2) 基底几何：三角汤，索引 0..V-1，位置全部叠在包围盒底角（退化）。上传后 3) 里再跑一趟
	// 「全部槽失效」把每个三角的第 0 个顶点写成 NaN —— 与 ⑤ 显隐同一个约定（回读侧判死活只看第 0 个顶点）。
	// ⚠️ NaN 不能直接放进快照：`BuildUploadPayload` 会拿全部位置累加包围盒。
	// 槽被分配时 ①② 写胞腔几何与静态属性，⑤ 写真位置。走 CopyFromMeshSnapshot 是因为它把计数器 +
	// indirect args + 索引一次性写对；自己拼这几样等于把 AddSetCountersPass 那套约定再抄一遍。
	FCSGpuMeshCPUData Snapshot;
	Snapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	Snapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	Snapshot.NumTexCoordChannels = TexCoord::NumSets;
	const FVector3f Parked(HardWorldBounds.Min);
	Snapshot.Positions.Init(Parked, NumVerts);
	Snapshot.Normals.Init(FVector3f(0.0f, 0.0f, 1.0f), NumVerts);
	Snapshot.Tangents.Init(FVector3f(1.0f, 0.0f, 0.0f), NumVerts);
	for (int32 Set = 0; Set < TexCoord::NumSets; ++Set) Snapshot.TexCoordChannels[Set].SetNumZeroed(NumVerts);
	// 顶点色的 R（盖 / 裙）只由模板决定 ⇒ 建池时就按模板写好；B（逐石头相位）等槽被分配时由 ② 补。
	Snapshot.Colors.SetNumUninitialized(NumVerts);
	for (int32 V = 0; V < NumVerts; ++V)
	{
		const int32 LocalTri = (V % VertsPerCell) / 3;
		Snapshot.Colors[V] = FVector4f(FTemplate::IsCapTri(LocalTri) ? VertexColor::CapValue : VertexColor::SkirtValue, 1.0f, 0.0f, 1.0f);
	}
	Snapshot.Indices.SetNumUninitialized(NumVerts);
	for (int32 V = 0; V < NumVerts; ++V) Snapshot.Indices[V] = uint32(V);
	if (!UCSMeshOps::CopyFromMeshSnapshot(ShellMesh, Snapshot))
	{
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] 槽位池上传失败（多半是显存预检拒了 %d 顶点 / %d 槽），岩壳关闭。"),
			NumVerts, CapacityCells);
		return false;
	}

	// --- 3) **写死包围盒** + 全部槽失效。包围盒：kernel 用 NaN 关掉看不见的三角，NaN 会污染任何
	// "从顶点算出来"的包围盒。失效：见 2) —— 走的是 ③ 那个 kernel，作业 = 全部槽。 ---
	const uint32 SlotCount = uint32(CapacityCells);
	const uint32 PoolM = uint32(Pattern.BoundaryPoints);
	const uint32 PoolTris = uint32(Pattern.TrianglesPerCell());
	const bool bBounds = ShellMesh->EditMeshSync([&HardWorldBounds, SlotCount, PoolM, PoolTris](FCSMeshEditContext& Context)
	{
		Context.SetWorldBounds(HardWorldBounds);
		FRDGBufferRef Positions = Context.Positions();
		if (!Positions) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		// 上传源必须活到图执行：放进图自己的分配器（NoCopy），不指向任何会先于图结束的数组。
		uint32* Slots = GraphBuilder.AllocPODArray<uint32>(SlotCount);
		for (uint32 Slot = 0; Slot < SlotCount; ++Slot) Slots[Slot] = Slot;
		FRDGBufferRef Jobs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), SlotCount), TEXT("CSRockShell.KillAll"));
		GraphBuilder.QueueBufferUpload(Jobs, Slots, SlotCount * sizeof(uint32), ERDGInitialDataFlags::NoCopy);
		FCSRockShellKillCS::FParameters* KillParams = GraphBuilder.AllocParameters<FCSRockShellKillCS::FParameters>();
		KillParams->RockShellBoundaryPoints = PoolM;
		KillParams->RockShellJobs = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Jobs));
		KillParams->RockShellJobCount = SlotCount;
		KillParams->RW_RockShellPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
		TShaderMapRef<FCSRockShellKillCS> KillShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.KillAll"), KillShader, KillParams,
			CSRockShell_Groups(uint64(SlotCount) * PoolTris));
	});
	if (!bBounds)
	{
		UE_LOG(LogCSRockShell, Warning, TEXT("[CSRockShell] 写包围盒失败，岩壳关闭。"));
		return false;
	}
	UE_LOG(LogCSRockShell, Log, TEXT("[CSRockShell] 槽位池已建：%d 槽 × %d 三角（M = %d）= %d 顶点，胞腔 %.1f cm。"),
		CapacityCells, Pattern.TrianglesPerCell(), Pattern.BoundaryPoints, NumVerts, Pattern.CellSize);
	return true;
}

bool Displace(
	UCSMesh* ShellMesh,
	const FCSMeshResidentRef& GroundResident,
	const FDisplaceParams& InParams,
	const TArray<FVector4f>& ShaperParams,
	const FJobs& Jobs)
{
	if (!ShellMesh || !GroundResident.IsValid()) return false;
	const FCSMeshResidentRef ShellResident = ShellMesh->GetResident();
	if (!ShellResident.IsValid() || !ShellResident->IsAllocated()) return false;
	if (Jobs.IsEmpty()) return true;

	FDisplaceParams Params = InParams;
	Params.Pattern = InParams.Pattern.Sanitized();
	const FPatternParams& Pattern = Params.Pattern;
	const uint32 M = uint32(Pattern.BoundaryPoints);
	const uint32 TrisPerCell = uint32(Pattern.TrianglesPerCell());
	const uint32 VertsPerCell = uint32(Pattern.VerticesPerCell());
	const uint32 CapacityCells = ShellResident->VertexCapacity / FMath::Max(VertsPerCell, 1u);

	// 作业里的槽号必须落在池里；越界就是调用方的簿记错了 —— 丢掉并喊出来，不许写出界。
	auto Filter = [CapacityCells](TArray<uint32> In, const TCHAR* What)
	{
		const int32 Before = In.Num();
		In.RemoveAll([CapacityCells](uint32 Slot) { return Slot >= CapacityCells; });
		if (In.Num() != Before)
		{
			UE_LOG(LogCSRockShell, Error, TEXT("[CSRockShell] %s 作业里有 %d 个槽号越界（池 %u 槽），已丢弃。"), What, Before - In.Num(), CapacityCells);
		}
		return In;
	};
	TArray<uint32> Drape = Filter(Jobs.Drape, TEXT("披挂"));
	TArray<uint32> Kill = Filter(Jobs.Kill, TEXT("失效"));
	TArray<FUintVector4> NewCells = Jobs.NewCells;
	NewCells.RemoveAll([CapacityCells](const FUintVector4& Job) { return Job.X >= CapacityCells; });

	// 一维派发的组数上限：作业 × 每作业线程数 / 64 ≤ 65535（池上限就是按它定的，这里只是兜底）。
	const uint32 MaxJobs = uint32(CSRockShell_MaxGroups) * uint32(CSRockShell_GroupSizeX) / FMath::Max(VertsPerCell, 1u);
	if (uint32(Drape.Num()) > MaxJobs) Drape.SetNum(int32(MaxJobs));
	if (uint32(Kill.Num()) > MaxJobs) Kill.SetNum(int32(MaxJobs));
	if (uint32(NewCells.Num()) > MaxJobs) NewCells.SetNum(int32(MaxJobs));

	TArray<FVector4f> UploadParams = ShaperParams;
	const int32 ShaperCount = CSGroundShaperField::NumShapers(UploadParams);
	if (UploadParams.IsEmpty()) UploadParams.Add(FVector4f::Zero());

	const FTemplate& Tmpl = GetTemplate(int32(M));
	TArray<int32> TemplateNeighbours = Tmpl.Neighbours;
	TArray<uint32> TemplateIncidentRange = Tmpl.IncidentRange;
	TArray<uint32> TemplateIncidentTris = CSRockShell_NonEmpty(Tmpl.IncidentTris, 0u);

	ENQUEUE_RENDER_COMMAND(CSRockShellDisplace)(
		[Shell = ShellResident, Ground = GroundResident, Params, UploadParams, ShaperCount,
		 Drape = MoveTemp(Drape), Kill = MoveTemp(Kill), NewCells = MoveTemp(NewCells),
		 TemplateNeighbours = MoveTemp(TemplateNeighbours), TemplateIncidentRange = MoveTemp(TemplateIncidentRange),
		 TemplateIncidentTris = MoveTemp(TemplateIncidentTris), M, TrisPerCell, VertsPerCell](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRockShell.Displace"));
			{
				// 两份常驻流各开一个编辑作用域：壳写，地面只读色流。访问状态由 ~FCSMeshRenderThreadEdit 恢复。
				FCSMeshRenderThreadEdit ShellEdit(GraphBuilder, *Shell);
				FCSMeshRenderThreadEdit GroundEdit(GraphBuilder, *Ground);

				FRDGBufferRef Positions = ShellEdit->Positions();
				FRDGBufferRef Tangents = ShellEdit->Tangents();
				FRDGBufferRef TexCoords = ShellEdit->TexCoords();
				FRDGBufferRef Colors = ShellEdit->Colors();
				FRDGBufferRef CellGeom = ShellEdit->Find(ECSGpuStreamRole::AuxVertex, uint8(EAuxSlot::CellGeom));
				FRDGBufferRef GroundColors = GroundEdit->Colors();
				const FPatternParams& P = Params.Pattern;
				const uint32 GeomStride = uint32(P.GeomFloat4sPerCell());
				const uint32 TexStride = uint32(TexCoord::NumSets) * 2u;

				if (Positions && Tangents && TexCoords && Colors && CellGeom)
				{
					TShaderMapRef<FCSRockShellBuildCellsCS> BuildShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FCSRockShellWriteStaticCS> StaticShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					TShaderMapRef<FCSRockShellKillCS> KillShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

					// ① ② 新槽：胞腔几何 → 静态属性。
					if (!NewCells.IsEmpty())
					{
						CSHelper::FRDGStructuredBufferRefs NewRefs = CSHelper::CreateUploadedStructuredBuffer<FUintVector4>(
							GraphBuilder, NewCells, TEXT("CSRockShell.NewCells"), false, true);

						FCSRockShellBuildCellsCS::FParameters* BuildParams = GraphBuilder.AllocParameters<FCSRockShellBuildCellsCS::FParameters>();
						BuildParams->RockShellCellSize = P.CellSize;
						BuildParams->RockShellSeedJitter = P.SeedJitter;
						BuildParams->RockShellClusterWarp = P.ClusterWarp;
						BuildParams->RockShellClusterWavelength = P.ClusterWavelength;
						BuildParams->RockShellSizeVariance = P.SizeVariance;
						BuildParams->RockShellPatternSeed = P.Seed;
						BuildParams->RockShellGridOrigin = Params.GridOrigin;
						BuildParams->RockShellLipCm = P.LipCm();
						BuildParams->RockShellBoundaryPoints = M;
						BuildParams->RockShellGeomStride = GeomStride;
						BuildParams->RockShellNewCells = NewRefs.SRV;
						BuildParams->RockShellNewCellCount = uint32(NewCells.Num());
						BuildParams->RW_RockShellCellGeom = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CellGeom, PF_A32B32G32R32F));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.BuildCells"), BuildShader, BuildParams,
							CSRockShell_Groups(uint64(NewCells.Num())));

						FCSRockShellWriteStaticCS::FParameters* StaticParams = GraphBuilder.AllocParameters<FCSRockShellWriteStaticCS::FParameters>();
						StaticParams->RockShellPatternSeed = P.Seed;
						StaticParams->RockShellBoundaryPoints = M;
						StaticParams->RockShellGeomStride = GeomStride;
						StaticParams->RockShellNewCells = NewRefs.SRV;
						StaticParams->RockShellNewCellCount = uint32(NewCells.Num());
						StaticParams->RockShellCellGeom = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CellGeom, PF_A32B32G32R32F));
						StaticParams->RW_RockShellColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Colors, PF_R32_UINT));
						StaticParams->RW_RockShellTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
						StaticParams->RockShellTexCoordStride = TexStride;
						StaticParams->RockShellInvUVWorldPeriod = 1.0f / FMath::Max(Params.UVWorldPeriod, 1.0f);
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.WriteStatic"), StaticShader, StaticParams,
							CSRockShell_Groups(uint64(NewCells.Num()) * VertsPerCell));
					}

					// ③ 失效槽。
					if (!Kill.IsEmpty())
					{
						CSHelper::FRDGStructuredBufferRefs KillRefs = CSHelper::CreateUploadedStructuredBuffer<uint32>(
							GraphBuilder, Kill, TEXT("CSRockShell.KillJobs"), false, true);
						FCSRockShellKillCS::FParameters* KillParams = GraphBuilder.AllocParameters<FCSRockShellKillCS::FParameters>();
						KillParams->RockShellBoundaryPoints = M;
						KillParams->RockShellJobs = KillRefs.SRV;
						KillParams->RockShellJobCount = uint32(Kill.Num());
						KillParams->RW_RockShellPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.Kill"), KillShader, KillParams,
							CSRockShell_Groups(uint64(Kill.Num()) * TrisPerCell));
					}

					if (!Drape.IsEmpty() && GroundColors)
					{
						CSHelper::FRDGStructuredBufferRefs ShaperRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
							GraphBuilder, UploadParams, TEXT("CSRockShell.ShaperParams"), false, true);
						CSHelper::FRDGStructuredBufferRefs JobRefs = CSHelper::CreateUploadedStructuredBuffer<uint32>(
							GraphBuilder, Drape, TEXT("CSRockShell.DrapeJobs"), false, true);

						// ④ 第零趟：色流 → 模糊足迹（地面路场，与地被共用一份 kernel）。RoadFade 是 **PreGain**：先截足迹、再糊。
						CSGroundRoadField::FBlurDesc RoadBlur;
						RoadBlur.GroundVerts = Params.GroundVerts;
						RoadBlur.GroundCellSize = Params.GroundCellSize;
						RoadBlur.Channel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);   // R = 道路权重
						RoadBlur.PreGain = Params.RoadFade;
						RoadBlur.PostGain = 1.0f;
						RoadBlur.Radius = Params.RoadBlurRadius;
						const FRDGBufferRef RoadField = CSGroundRoadField::AddBlurPasses(GraphBuilder, GroundColors, RoadBlur, TEXT("CSRockShell.RoadField"));

						// ⑤ 披挂。
						FCSGroundRockShellCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSGroundRockShellCS::FParameters>();
						PassParams->GroundShaperParams = ShaperRefs.SRV;
						PassParams->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
						PassParams->RockShellBoundaryPoints = M;
						PassParams->RockShellGeomStride = GeomStride;
						PassParams->RockShellCellGeom = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CellGeom, PF_A32B32G32R32F));
						PassParams->RockShellJobs = JobRefs.SRV;
						PassParams->RockShellJobCount = uint32(Drape.Num());
						PassParams->RockShellRoadField = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RoadField, PF_R32_FLOAT));
						PassParams->RW_RockShellPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
						PassParams->RW_RockShellTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
						PassParams->RockShellDomainMin = Params.DomainMin;
						PassParams->RockShellDomainMax = Params.DomainMax;
						PassParams->RockShellCellRadiusCm = FMath::Max(P.CellRadiusCm(), 1.0f);
						PassParams->RockShellGroundOriginXY = Params.GroundOriginXY;
						PassParams->RockShellGroundCellSize = Params.GroundCellSize;
						PassParams->RockShellGroundVerts = FUintVector2(
							uint32(FMath::Max(Params.GroundVerts.X, 0)), uint32(FMath::Max(Params.GroundVerts.Y, 0)));
						PassParams->RockShellGroundBaseZ = Params.GroundBaseZ;
						PassParams->RockShellSlopeLo = Params.SlopeLo;
						// Hi 必须严格大于 Lo：smoothstep 在两者相等时是 0/0，整片 mask 变 NaN。
						PassParams->RockShellSlopeHi = FMath::Max(Params.SlopeHi, Params.SlopeLo + 1e-3f);
						PassParams->RockShellRoadSink = FMath::Max(Params.RoadSink, 0.0f);
						PassParams->RockShellCellJitter = FMath::Max(Params.CellJitter, 0.0f);
						PassParams->RockShellCellRelief = FMath::Max(Params.CellRelief, 0.0f);
						PassParams->RockShellReliefFloor = FMath::Clamp(Params.ReliefFloor, 0.0f, 1.0f);
						PassParams->RockShellCellExpand = FMath::Max(Params.CellExpand, 0.0f);
						PassParams->RockShellSkirtTilt = FMath::Max(Params.SkirtTilt, 0.0f);
						PassParams->RockShellBaseLift = FMath::Max(Params.BaseLift, 0.0f);
						PassParams->RockShellBaseSink = FMath::Max(Params.BaseSink, 0.0f);
						PassParams->RockShellNoiseAmp = FMath::Max(Params.NoiseAmp, 0.0f);
						PassParams->RockShellNoiseFreq = FMath::Max(Params.NoiseFrequency, 0.0f);
						PassParams->RockShellRiseMultiplier = FMath::Max(Params.RiseMultiplier, 0.0f);
						PassParams->RockShellRiseNoiseAmp = FMath::Max(Params.RiseNoiseAmp, 0.0f);
						PassParams->RockShellRiseNoiseFreq = FMath::Max(Params.RiseNoiseFrequency, 0.0f);
						PassParams->RockShellRiseExtend = FMath::Max(Params.RiseExtend, 0.0f);
						// ⑧ 夹到 < 1：等于 1 的话末端与地面共面，出图上是一圈 z-fighting。
						PassParams->RockShellEdgeCeiling = FMath::Clamp(Params.EdgeCeiling, 0.0f, 0.999f);
						PassParams->RockShellPeakHeight = FMath::Max(Params.PeakHeight, 1.0f);
						PassParams->RockShellChipAmount = FMath::Max(Params.ChipAmount, 0.0f);
						PassParams->RockShellChipFreq = Params.ChipFrequency;
						PassParams->RockShellSeed = Params.Seed;
						TShaderMapRef<FCSGroundRockShellCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.Drape"), Shader, PassParams,
							CSRockShell_Groups(uint64(Drape.Num()) * TrisPerCell));

						// ⑥ 平均法线（可选）。**必须在披挂之后**：读的正是上一趟刚写出来的位置。
						if (Params.bSmoothNormals)
						{
							CSHelper::FRDGStructuredBufferRefs RangeRefs = CSHelper::CreateUploadedStructuredBuffer<uint32>(
								GraphBuilder, TemplateIncidentRange, TEXT("CSRockShell.TemplateIncidentRange"), false, true);
							CSHelper::FRDGStructuredBufferRefs TrisRefs = CSHelper::CreateUploadedStructuredBuffer<uint32>(
								GraphBuilder, TemplateIncidentTris, TEXT("CSRockShell.TemplateIncidentTris"), false, true);
							FCSRockShellAverageNormalsCS::FParameters* AvgParams = GraphBuilder.AllocParameters<FCSRockShellAverageNormalsCS::FParameters>();
							AvgParams->RockShellPositionsRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Positions, PF_R32_FLOAT));
							AvgParams->RockShellTemplateIncidentRange = RangeRefs.SRV;
							AvgParams->RockShellTemplateIncidentTris = TrisRefs.SRV;
							AvgParams->RockShellJobs = JobRefs.SRV;
							AvgParams->RockShellJobCount = uint32(Drape.Num());
							AvgParams->RockShellBoundaryPoints = M;
							AvgParams->RW_RockShellTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
							// 夹角阈值 → cos。夹到 [0,180]：负角会让 cos > 1 ⇒ 一个邻面都不收。
							AvgParams->RockShellSmoothCosThreshold = FMath::Cos(
								FMath::DegreesToRadians(FMath::Clamp(Params.SmoothAngleDeg, 0.0f, 180.0f)));
							TShaderMapRef<FCSRockShellAverageNormalsCS> AvgShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
							FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.AverageNormals"), AvgShader, AvgParams,
								CSRockShell_Groups(uint64(Drape.Num()) * VertsPerCell));
						}

						// ⑦ 假倒角 v3 的载荷。与 ⑥ 互不依赖（一个写切线、一个写 UV），共同前置只有 Positions 的 UAV→SRV。
						{
							CSHelper::FRDGStructuredBufferRefs NbrRefs = CSHelper::CreateUploadedStructuredBuffer<int32>(
								GraphBuilder, TemplateNeighbours, TEXT("CSRockShell.TemplateNeighbours"), false, true);
							FCSRockShellBevelPayloadCS::FParameters* BevelParams = GraphBuilder.AllocParameters<FCSRockShellBevelPayloadCS::FParameters>();
							BevelParams->RockShellPositionsRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Positions, PF_R32_FLOAT));
							BevelParams->RockShellTemplateNeighbours = NbrRefs.SRV;
							BevelParams->RockShellJobs = JobRefs.SRV;
							BevelParams->RockShellJobCount = uint32(Drape.Num());
							BevelParams->RockShellBoundaryPoints = M;
							BevelParams->RW_RockShellTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
							BevelParams->RockShellTexCoordStride = TexStride;
							TShaderMapRef<FCSRockShellBevelPayloadCS> BevelShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
							FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRockShell.BevelPayload"), BevelShader, BevelParams,
								CSRockShell_Groups(uint64(Drape.Num()) * TrisPerCell));
						}
					}
				}
			}
			GraphBuilder.Execute();
		});

	return true;
}
}

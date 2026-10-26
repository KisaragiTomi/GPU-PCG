#include "CSClothSim.h"

#include "CSGpuMeshTypes.h"
#include "CSClothLayout.ush"   // CS_CLOTH_FREE_PARTICLE / CS_CLOTH_CONSTRAINT_*（与 CSClothSim.usf 同一份）

#include "Algo/Sort.h"

DEFINE_LOG_CATEGORY(LogCSCloth);

namespace
{
// Unity/jumbo 构建共享 TU：本文件的 file-local 名字一律带 CSCloth_ 前缀（CSMB_ 的教训）。

uint32 CSCloth_FloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

uint64 CSCloth_EdgeKey(uint32 A, uint32 B)
{
	return A < B ? (uint64(A) << 32) | uint64(B) : (uint64(B) << 32) | uint64(A);
}

/** 一条边两侧的对顶点。非流形边（≥ 3 个三角）只记前两个，并且不出弯曲约束。 */
struct FCSClothEdgeAdjacency
{
	uint32 Opposite[2] = { 0, 0 };
	int32 TriangleCount = 0;
};

float CSCloth_ColorChannel(const FVector4f& Color, ECSClothPinColorChannel Channel)
{
	switch (Channel)
	{
	case ECSClothPinColorChannel::Red:   return Color.X;
	case ECSClothPinColorChannel::Green: return Color.Y;
	case ECSClothPinColorChannel::Blue:  return Color.Z;
	case ECSClothPinColorChannel::Alpha: return Color.W;
	default:                             return 0.0f;
	}
}

/**
 * 位置焊接：网格哈希，格宽 = 容差，查 27 个邻格。UV 缝与硬边两侧的副本逐位相等，所以命中的
 * 几乎都是同一格；查邻格只为容差内但恰好跨格的那一点点浮点噪声。
 */
void CSCloth_WeldPositions(const TArray<FVector3f>& Positions, float Tolerance,
	TArray<uint32>& OutCornerToParticle, TArray<int32>& OutParticleSource)
{
	const double Cell = FMath::Max(double(Tolerance), 1e-6);
	const double ToleranceSq = Cell * Cell;

	TMap<FIntVector, TArray<int32, TInlineAllocator<2>>> Grid;
	Grid.Reserve(Positions.Num());
	OutCornerToParticle.SetNumUninitialized(Positions.Num());
	OutParticleSource.Reset();

	for (int32 Vertex = 0; Vertex < Positions.Num(); ++Vertex)
	{
		const FVector P(Positions[Vertex]);
		const FIntVector Key(
			FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell), FMath::FloorToInt32(P.Z / Cell));

		int32 Found = INDEX_NONE;
		for (int32 DZ = -1; DZ <= 1 && Found == INDEX_NONE; ++DZ)
		for (int32 DY = -1; DY <= 1 && Found == INDEX_NONE; ++DY)
		for (int32 DX = -1; DX <= 1 && Found == INDEX_NONE; ++DX)
		{
			const TArray<int32, TInlineAllocator<2>>* Bucket = Grid.Find(Key + FIntVector(DX, DY, DZ));
			if (!Bucket) continue;
			for (const int32 Particle : *Bucket)
			{
				if (FVector::DistSquared(FVector(Positions[OutParticleSource[Particle]]), P) <= ToleranceSq)
				{
					Found = Particle;
					break;
				}
			}
		}

		if (Found == INDEX_NONE)
		{
			Found = OutParticleSource.Add(Vertex);
			Grid.FindOrAdd(Key).Add(Found);
		}
		OutCornerToParticle[Vertex] = uint32(Found);
	}
}

/** 多源 Dijkstra：从全部钉住粒子出发沿拉伸边走，得到每个粒子到最近钉点的测地距离与那个钉点。 */
void CSCloth_GeodesicFromPins(int32 NumParticles, const TArray<FUintVector4>& StretchConstraints,
	const TArray<float>& InvMass, TArray<float>& OutDistance, TArray<int32>& OutAnchor)
{
	// CSR 邻接表。
	TArray<int32> Offsets;
	Offsets.SetNumZeroed(NumParticles + 1);
	for (const FUintVector4& C : StretchConstraints) { ++Offsets[C.X + 1]; ++Offsets[C.Y + 1]; }
	for (int32 P = 0; P < NumParticles; ++P) Offsets[P + 1] += Offsets[P];

	TArray<int32> Cursor = Offsets;
	TArray<int32> Neighbor;
	TArray<float> Weight;
	Neighbor.SetNumUninitialized(Offsets[NumParticles]);
	Weight.SetNumUninitialized(Offsets[NumParticles]);
	for (const FUintVector4& C : StretchConstraints)
	{
		float Rest = 0.0f;
		FMemory::Memcpy(&Rest, &C.Z, sizeof(Rest));
		Neighbor[Cursor[C.X]] = int32(C.Y); Weight[Cursor[C.X]++] = Rest;
		Neighbor[Cursor[C.Y]] = int32(C.X); Weight[Cursor[C.Y]++] = Rest;
	}

	OutDistance.Init(TNumericLimits<float>::Max(), NumParticles);
	OutAnchor.Init(INDEX_NONE, NumParticles);

	struct FNode
	{
		float Distance;
		int32 Particle;
		bool operator<(const FNode& Other) const { return Distance < Other.Distance; }
	};
	TArray<FNode> Heap;
	for (int32 P = 0; P < NumParticles; ++P)
	{
		if (InvMass[P] > 0.0f) continue;
		OutDistance[P] = 0.0f;
		OutAnchor[P] = P;
		Heap.HeapPush(FNode{ 0.0f, P });
	}

	while (Heap.Num() > 0)
	{
		FNode Node;
		Heap.HeapPop(Node, EAllowShrinking::No);
		if (Node.Distance > OutDistance[Node.Particle]) continue;
		for (int32 Edge = Offsets[Node.Particle]; Edge < Offsets[Node.Particle + 1]; ++Edge)
		{
			const int32 Next = Neighbor[Edge];
			const float Candidate = Node.Distance + Weight[Edge];
			if (Candidate >= OutDistance[Next]) continue;
			OutDistance[Next] = Candidate;
			OutAnchor[Next] = OutAnchor[Node.Particle];
			Heap.HeapPush(FNode{ Candidate, Next });
		}
	}
}

/**
 * 贪心上色 [First, First + Count) 这一段约束，颜色号从 ColorBase 起，结果写进 OutColor。返回用掉的颜色数，
 * 超出上限（某个可动粒子挂了 ≥ 256 条约束，不是布）返回 INDEX_NONE。
 *
 * 只有**可动**端点算冲突：钉住的粒子只读不写，任意多条同色约束共享它都没有竞争。
 */
int32 CSCloth_GreedyColor(int32 NumParticles, const TArray<float>& InvMass, const TArray<FUintVector4>& Constraints,
	int32 First, int32 Count, int32 ColorBase, TArray<int32>& OutColor)
{
	constexpr int32 Words = 4;   // 256 种颜色
	TArray<uint64> Used;
	Used.SetNumZeroed(NumParticles * Words);
	int32 NumColors = 0;
	for (int32 C = First; C < First + Count; ++C)
	{
		const FUintVector4& Record = Constraints[C];
		const bool bMoveA = InvMass[Record.X] > 0.0f;
		const bool bMoveB = InvMass[Record.Y] > 0.0f;
		int32 Chosen = INDEX_NONE;
		for (int32 Word = 0; Word < Words && Chosen == INDEX_NONE; ++Word)
		{
			uint64 Taken = 0;
			if (bMoveA) Taken |= Used[Record.X * Words + Word];
			if (bMoveB) Taken |= Used[Record.Y * Words + Word];
			if (Taken != ~0ull) Chosen = Word * 64 + int32(FMath::CountTrailingZeros64(~Taken));
		}
		if (Chosen == INDEX_NONE) return INDEX_NONE;

		const uint64 Bit = 1ull << (Chosen % 64);
		if (bMoveA) Used[Record.X * Words + Chosen / 64] |= Bit;
		if (bMoveB) Used[Record.Y * Words + Chosen / 64] |= Bit;
		OutColor[C] = ColorBase + Chosen;
		NumColors = FMath::Max(NumColors, Chosen + 1);
	}
	return NumColors;
}
}

int64 FCSClothSetupData::GetGpuBytes() const
{
	// 粒子域：Positions / PrevPositions / RestPositions / ParticleNormals / RestNormals 各 float4，
	// NormalAccum int3，PinGroups uint。约束域：uint4 记录 + float λ。
	// 映射域：CornerToParticle uint + RestTangents 2 uint。三角：3 uint。见 CSClothSimRender.cpp 的分配。
	const int64 PerParticle = 5 * 16 + 12 + 4;
	const int64 PerConstraint = 16 + 4;
	const int64 PerRenderVertex = 4 + 8;
	return int64(NumParticles) * PerParticle
		+ int64(Constraints.Num()) * PerConstraint
		+ int64(NumRenderVertices) * PerRenderVertex
		+ int64(TriangleParticles.Num()) * 4
		+ 64;
}

namespace CSClothSetup
{
bool BuildFromSnapshot(const FCSGpuMeshCPUData& Snapshot, const FCSClothSetupOptions& Options, FCSClothSetupData& OutData)
{
	OutData = FCSClothSetupData();

	if (Snapshot.AttrLayout != FCSGpuMeshCPUData::EAttrLayout::PerVertex)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] Setup refused: the snapshot is per-corner; the cloth needs the per-vertex layout UCSMesh::ReadbackMeshSync produces."));
		return false;
	}
	const int32 NumVertices = Snapshot.Positions.Num();
	if (NumVertices < 3 || Snapshot.Indices.Num() < 3 || Snapshot.Indices.Num() % 3 != 0)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] Setup refused: %d vertices / %d indices is not a triangle mesh."),
			NumVertices, Snapshot.Indices.Num());
		return false;
	}

	// --- 焊接：渲染顶点 → 粒子
	TArray<int32> ParticleSource;   // 每粒子取哪个渲染顶点的位置
	CSCloth_WeldPositions(Snapshot.Positions, Options.WeldTolerance, OutData.CornerToParticle, ParticleSource);
	const int32 NumParticles = ParticleSource.Num();
	OutData.NumParticles = NumParticles;
	OutData.NumRenderVertices = NumVertices;

	TArray<FVector3f> SimPositions;
	SimPositions.SetNumUninitialized(NumParticles);
	for (int32 P = 0; P < NumParticles; ++P)
	{
		SimPositions[P] = FVector3f(FVector(Snapshot.Positions[ParticleSource[P]]) - Options.SimOrigin);
		OutData.RestBounds += SimPositions[P];
	}

	// --- 三角：映射到粒子，剔除退化与重复（双面网格的背面）
	TSet<FIntVector> SeenTriangles;
	SeenTriangles.Reserve(Snapshot.Indices.Num() / 3);
	OutData.TriangleParticles.Reserve(Snapshot.Indices.Num());
	for (int32 Corner = 0; Corner + 2 < Snapshot.Indices.Num(); Corner += 3)
	{
		const uint32 I0 = Snapshot.Indices[Corner + 0];
		const uint32 I1 = Snapshot.Indices[Corner + 1];
		const uint32 I2 = Snapshot.Indices[Corner + 2];
		if (int32(I0) >= NumVertices || int32(I1) >= NumVertices || int32(I2) >= NumVertices)
		{
			UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] Setup refused: index %d points past the %d vertices."), Corner, NumVertices);
			OutData = FCSClothSetupData();
			return false;
		}
		const uint32 A = OutData.CornerToParticle[I0];
		const uint32 B = OutData.CornerToParticle[I1];
		const uint32 C = OutData.CornerToParticle[I2];
		if (A == B || B == C || A == C) continue;

		uint32 Sorted[3] = { A, B, C };
		Algo::Sort(Sorted);
		bool bAlreadySeen = false;
		SeenTriangles.Add(FIntVector(int32(Sorted[0]), int32(Sorted[1]), int32(Sorted[2])), &bAlreadySeen);
		if (bAlreadySeen) continue;

		OutData.TriangleParticles.Append({ A, B, C });
	}
	const int32 NumTriangles = OutData.TriangleParticles.Num() / 3;
	if (NumTriangles == 0)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] Setup refused: every triangle is degenerate after welding (tolerance %.4f cm)."),
			Options.WeldTolerance);
		OutData = FCSClothSetupData();
		return false;
	}

	// --- 质量：相邻三角面积的 1/3（cm² → m²）
	TArray<double> Mass;
	Mass.SetNumZeroed(NumParticles);
	TMap<uint64, FCSClothEdgeAdjacency> Edges;
	Edges.Reserve(NumTriangles * 2);
	const double KgPerCm2 = double(FMath::Max(Options.ArealDensity, 1e-4f)) * 1e-4;
	for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		const uint32 V[3] = { OutData.TriangleParticles[Tri * 3 + 0], OutData.TriangleParticles[Tri * 3 + 1], OutData.TriangleParticles[Tri * 3 + 2] };
		const FVector PA(SimPositions[V[0]]), PB(SimPositions[V[1]]), PC(SimPositions[V[2]]);
		const double Area = 0.5 * FVector::CrossProduct(PB - PA, PC - PA).Size();
		for (const uint32 Particle : V) Mass[Particle] += Area * KgPerCm2 / 3.0;

		for (int32 Side = 0; Side < 3; ++Side)
		{
			FCSClothEdgeAdjacency& Adjacency = Edges.FindOrAdd(CSCloth_EdgeKey(V[Side], V[(Side + 1) % 3]));
			if (Adjacency.TriangleCount < 2) Adjacency.Opposite[Adjacency.TriangleCount] = V[(Side + 2) % 3];
			++Adjacency.TriangleCount;
		}
	}

	// --- 钉点：钉盒（先到先得），再顶点色遮罩
	OutData.PinGroups.Init(CS_CLOTH_FREE_PARTICLE, NumParticles);
	const int32 NumGroups = FMath::Max(Options.NumPinGroups, 1);
	for (int32 P = 0; P < NumParticles; ++P)
	{
		const FVector World(Snapshot.Positions[ParticleSource[P]]);
		for (const FCSClothSetupOptions::FPinBox& Pin : Options.PinBoxes)
		{
			if (!Pin.WorldBox.IsValid || !Pin.WorldBox.IsInsideOrOn(World)) continue;
			OutData.PinGroups[P] = FMath::Min(Pin.Group, uint32(NumGroups - 1));
			break;
		}
	}
	if (Options.PinColorChannel != ECSClothPinColorChannel::None && Snapshot.Colors.Num() == NumVertices)
	{
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			const uint32 Particle = OutData.CornerToParticle[Vertex];
			if (OutData.PinGroups[Particle] != CS_CLOTH_FREE_PARTICLE) continue;
			if (CSCloth_ColorChannel(Snapshot.Colors[Vertex], Options.PinColorChannel) >= Options.PinColorThreshold)
				OutData.PinGroups[Particle] = 0;
		}
	}

	TArray<float> InvMass;
	InvMass.SetNumUninitialized(NumParticles);
	OutData.RestPositions.SetNumUninitialized(NumParticles);
	for (int32 P = 0; P < NumParticles; ++P)
	{
		// 不属于任何三角的粒子（孤立顶点、只在退化三角里出现的顶点）没有质量，也不是布：
		// 当成钉在组 0 上，跟着宿主走，永远不会被积分出 NaN。
		if (Mass[P] <= 0.0 && OutData.PinGroups[P] == CS_CLOTH_FREE_PARTICLE) OutData.PinGroups[P] = 0;
		const bool bPinned = OutData.PinGroups[P] != CS_CLOTH_FREE_PARTICLE;
		InvMass[P] = bPinned ? 0.0f : float(1.0 / Mass[P]);
		OutData.RestPositions[P] = FVector4f(SimPositions[P], InvMass[P]);
		OutData.TotalMassKg += Mass[P];
		if (bPinned) ++OutData.NumPinned;
	}

	// --- 约束：拉伸（每条唯一边）| 弯曲（流形内边两侧对顶点之间的跨边距离）| tether
	OutData.Constraints.Reserve(Edges.Num() * 2 + NumParticles);
	for (const TPair<uint64, FCSClothEdgeAdjacency>& Edge : Edges)
	{
		const uint32 A = uint32(Edge.Key >> 32);
		const uint32 B = uint32(Edge.Key & 0xffffffffu);
		if (InvMass[A] <= 0.0f && InvMass[B] <= 0.0f) continue;   // 两端都钉死：约束永远解不动
		const float Rest = FVector3f::Dist(SimPositions[A], SimPositions[B]);
		OutData.Constraints.Add(FUintVector4(A, B, CSCloth_FloatBits(Rest), CS_CLOTH_CONSTRAINT_STRETCH));
	}
	OutData.NumStretch = OutData.Constraints.Num();

	// 测地距离要走全部边（包括两端都钉死的那些），所以另建一份完整的边表。
	TArray<FUintVector4> AllEdges;
	AllEdges.Reserve(Edges.Num());
	for (const TPair<uint64, FCSClothEdgeAdjacency>& Edge : Edges)
	{
		const uint32 A = uint32(Edge.Key >> 32);
		const uint32 B = uint32(Edge.Key & 0xffffffffu);
		AllEdges.Add(FUintVector4(A, B, CSCloth_FloatBits(FVector3f::Dist(SimPositions[A], SimPositions[B])), 0));
	}

	TSet<uint64> BendPairs;
	BendPairs.Reserve(Edges.Num());
	for (const TPair<uint64, FCSClothEdgeAdjacency>& Edge : Edges)
	{
		if (Edge.Value.TriangleCount != 2) continue;
		const uint32 A = Edge.Value.Opposite[0];
		const uint32 B = Edge.Value.Opposite[1];
		if (A == B || (InvMass[A] <= 0.0f && InvMass[B] <= 0.0f)) continue;
		bool bDuplicate = false;
		BendPairs.Add(CSCloth_EdgeKey(A, B), &bDuplicate);
		if (bDuplicate) continue;
		const float Rest = FVector3f::Dist(SimPositions[A], SimPositions[B]);
		OutData.Constraints.Add(FUintVector4(A, B, CSCloth_FloatBits(Rest), CS_CLOTH_CONSTRAINT_BEND));
	}
	OutData.NumBend = OutData.Constraints.Num() - OutData.NumStretch;

	TArray<float> Geodesic;
	TArray<int32> Anchor;
	CSCloth_GeodesicFromPins(NumParticles, AllEdges, InvMass, Geodesic, Anchor);

	const float TetherScale = FMath::Max(Options.TetherScale, 1.0f);
	if (Options.bBuildTethers && OutData.NumPinned > 0)
	{
		for (int32 P = 0; P < NumParticles; ++P)
		{
			if (InvMass[P] <= 0.0f || Anchor[P] == INDEX_NONE || Geodesic[P] <= UE_KINDA_SMALL_NUMBER) continue;
			OutData.Constraints.Add(FUintVector4(uint32(P), uint32(Anchor[P]),
				CSCloth_FloatBits(Geodesic[P] * TetherScale), CS_CLOTH_CONSTRAINT_TETHER));
		}
	}
	OutData.NumTether = OutData.Constraints.Num() - OutData.NumStretch - OutData.NumBend;

	// --- Gauss-Seidel 上色：两段各自上色、各自占一组颜色 —— 拉伸 + 弯曲在前，tether 整段排在最后，
	// 每个 substep 都以「把超出上限的拉回来」收尾（长程约束的惯常用法）。每个自由粒子只有一条 tether、
	// 另一端是钉住的，所以 tether 通常只占一种颜色。
	TArray<int32> Color;
	Color.SetNumUninitialized(OutData.Constraints.Num());
	const int32 NumDistance = OutData.NumStretch + OutData.NumBend;
	const int32 DistanceColors = CSCloth_GreedyColor(NumParticles, InvMass, OutData.Constraints, 0, NumDistance, 0, Color);
	const int32 TetherColors = DistanceColors == INDEX_NONE ? INDEX_NONE
		: CSCloth_GreedyColor(NumParticles, InvMass, OutData.Constraints, NumDistance, OutData.NumTether, DistanceColors, Color);
	if (TetherColors == INDEX_NONE)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] Setup refused: a particle carries more than 256 constraints, which is not a cloth mesh."));
		OutData = FCSClothSetupData();
		return false;
	}

	// 按颜色稳定排序（计数排序），同色约束连续存放；ColorRanges 就是每次派发的 [Offset, Count)。
	const int32 NumColors = DistanceColors + TetherColors;
	TArray<int32> Cursor;
	Cursor.SetNumZeroed(NumColors);
	for (const int32 C : Color) ++Cursor[C];
	OutData.ColorRanges.SetNumUninitialized(NumColors);
	int32 Offset = 0;
	for (int32 K = 0; K < NumColors; ++K)
	{
		OutData.ColorRanges[K] = FIntPoint(Offset, Cursor[K]);
		const int32 Size = Cursor[K];
		Cursor[K] = Offset;
		Offset += Size;
	}
	TArray<FUintVector4> Sorted;
	Sorted.SetNumUninitialized(OutData.Constraints.Num());
	for (int32 C = 0; C < OutData.Constraints.Num(); ++C) Sorted[Cursor[Color[C]]++] = OutData.Constraints[C];
	OutData.Constraints = MoveTemp(Sorted);

	// --- 包围盒素材：每钉组的钉住部分与可达半径；连不到钉点的粒子另记
	OutData.PinGroupRestBounds.Init(FBox3f(ForceInit), NumGroups);
	OutData.PinGroupReach.Init(0.0f, NumGroups);
	// 没有 tether 时拉伸约束不是硬上限（柔度 > 0 时布可以被拉长），可达半径按 1.5 倍测地距离保守估计。
	const float ReachScale = (Options.bBuildTethers && OutData.NumPinned > 0) ? TetherScale : 1.5f;
	for (int32 P = 0; P < NumParticles; ++P)
	{
		if (OutData.PinGroups[P] != CS_CLOTH_FREE_PARTICLE)
		{
			OutData.PinGroupRestBounds[OutData.PinGroups[P]] += SimPositions[P];
			continue;
		}
		if (Anchor[P] == INDEX_NONE)
		{
			OutData.UnanchoredRestBounds += SimPositions[P];
			continue;
		}
		const uint32 Group = OutData.PinGroups[Anchor[P]];
		OutData.PinGroupReach[Group] = FMath::Max(OutData.PinGroupReach[Group], Geodesic[P] * ReachScale);
	}

	return true;
}

void BuildGridSnapshot(const FVector2D& Size, const FIntPoint& Quads, const FTransform& Transform, bool bDoubleSided,
	FCSGpuMeshCPUData& OutSnapshot)
{
	OutSnapshot.Reset();
	const int32 QX = FMath::Max(Quads.X, 1);
	const int32 QY = FMath::Max(Quads.Y, 1);
	const int32 VertsX = QX + 1;
	const int32 VertsY = QY + 1;
	const int32 SideVertices = VertsX * VertsY;
	const int32 Sides = bDoubleSided ? 2 : 1;

	const FVector3f Normal = FVector3f(Transform.TransformVectorNoScale(FVector::UpVector).GetSafeNormal());
	const FVector3f Tangent = FVector3f(Transform.TransformVectorNoScale(FVector::ForwardVector).GetSafeNormal());

	OutSnapshot.Positions.Reserve(SideVertices * Sides);
	for (int32 Side = 0; Side < Sides; ++Side)
	{
		for (int32 Y = 0; Y < VertsY; ++Y)
		{
			for (int32 X = 0; X < VertsX; ++X)
			{
				const FVector Local(
					(double(X) / QX - 0.5) * Size.X,
					(double(Y) / QY - 0.5) * Size.Y,
					0.0);
				OutSnapshot.Positions.Add(FVector3f(Transform.TransformPosition(Local)));
				OutSnapshot.Normals.Add(Side == 0 ? Normal : -Normal);
				OutSnapshot.Tangents.Add(Tangent);
				// 背面法线反了而 UV 没变：副切线（沿 V 的方向）在世界里不动，sign 就得跟着翻，法线贴图才不反。
				OutSnapshot.BinormalSigns.Add(Side == 0 ? 1.0f : -1.0f);
				OutSnapshot.TexCoords().Add(FVector2f(float(X) / QX, float(Y) / QY));
				OutSnapshot.Colors.Add(FVector4f(1.0f, 1.0f, 1.0f, 1.0f));
			}
		}
	}

	// 引擎绕序：正面 = cross(C−A, B−A) 朝法线（与 CSGroundActor 的地面网格同一口径）。
	// 背面用同一批角点、反过来的绕序。
	OutSnapshot.Indices.Reserve(QX * QY * 6 * Sides);
	for (int32 Side = 0; Side < Sides; ++Side)
	{
		const uint32 Base = uint32(Side * SideVertices);
		for (int32 Y = 0; Y < QY; ++Y)
		{
			for (int32 X = 0; X < QX; ++X)
			{
				const uint32 V00 = Base + uint32(Y * VertsX + X);
				const uint32 V10 = V00 + 1;
				const uint32 V01 = V00 + uint32(VertsX);
				const uint32 V11 = V01 + 1;
				if (Side == 0) OutSnapshot.Indices.Append({ V00, V01, V10, V10, V01, V11 });
				else           OutSnapshot.Indices.Append({ V00, V10, V01, V10, V11, V01 });
			}
		}
	}
	OutSnapshot.TriangleMaterialSlots.Init(0, OutSnapshot.Indices.Num() / 3);
	OutSnapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	OutSnapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
}
}

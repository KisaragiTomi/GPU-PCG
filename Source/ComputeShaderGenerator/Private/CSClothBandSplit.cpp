#include "CSClothBandSplit.h"

#include "Math/RandomStream.h"

namespace
{
// Unity/jumbo 构建共享 TU：本文件的 file-local 名字一律带 CSClothSplit_ 前缀。

uint64 CSClothSplit_EdgeKey(int32 A, int32 B)
{
	return A < B ? (uint64(uint32(A)) << 32) | uint64(uint32(B)) : (uint64(uint32(B)) << 32) | uint64(uint32(A));
}

/** 无向图（CSR），边权 = 边长。 */
struct FCSClothSplitGraph
{
	TArray<int32> Offsets;
	TArray<int32> Neighbors;
	TArray<float> Lengths;

	void Build(int32 NumNodes, const TArray<FIntVector>& Triangles, TFunctionRef<float(int32, int32)> Length,
		const TMap<uint64, float>* Shortcuts = nullptr)
	{
		TMap<uint64, float> Edges;
		Edges.Reserve(Triangles.Num() * 2 + (Shortcuts ? Shortcuts->Num() : 0));
		auto AddEdge = [&Edges, &Length](int32 A, int32 B)
		{
			const uint64 Key = CSClothSplit_EdgeKey(A, B);
			if (!Edges.Contains(Key)) Edges.Add(Key, Length(A, B));
		};
		for (const FIntVector& T : Triangles)
		{
			AddEdge(T.X, T.Y);
			AddEdge(T.Y, T.Z);
			AddEdge(T.Z, T.X);
		}
		if (Shortcuts)
		{
			for (const TPair<uint64, float>& Shortcut : *Shortcuts)
			{
				float& Existing = Edges.FindOrAdd(Shortcut.Key, Shortcut.Value);
				Existing = FMath::Min(Existing, Shortcut.Value);
			}
		}
		Offsets.Init(0, NumNodes + 1);
		for (const TPair<uint64, float>& Edge : Edges)
		{
			++Offsets[int32(Edge.Key >> 32) + 1];
			++Offsets[int32(Edge.Key & 0xffffffffu) + 1];
		}
		for (int32 Node = 0; Node < NumNodes; ++Node) Offsets[Node + 1] += Offsets[Node];
		TArray<int32> Cursor = Offsets;
		Neighbors.SetNumUninitialized(Offsets[NumNodes]);
		Lengths.SetNumUninitialized(Offsets[NumNodes]);
		for (const TPair<uint64, float>& Edge : Edges)
		{
			const int32 A = int32(Edge.Key >> 32);
			const int32 B = int32(Edge.Key & 0xffffffffu);
			Neighbors[Cursor[A]] = B;
			Lengths[Cursor[A]++] = Edge.Value;
			Neighbors[Cursor[B]] = A;
			Lengths[Cursor[B]++] = Edge.Value;
		}
	}
};

/**
 * 跨一条内边的捷径：把两个相邻三角摊平到同一平面，两个对顶点之间的直线若穿过这条公共边，它就是一段曲面上的
 * 直线路径，长度 = 摊平后的距离。只沿网格边走的 Dijkstra 在规则网格上最多把距离多算约 41%（斜边走反的方向），
 * 等距线成了菱形；加上这些捷径，误差降到几个百分点，条带才是圆的。返回 < 0 表示直线不穿过公共边。
 */
float CSClothSplit_UnfoldedDistance(const FVector3f& A, const FVector3f& B, const FVector3f& C, const FVector3f& D)
{
	const FVector3f Edge = B - A;
	const float EdgeLength = Edge.Size();
	if (!(EdgeLength > 1e-6f)) return -1.0f;
	const FVector3f U = Edge / EdgeLength;
	const float CX = (C - A) | U;
	const float CY = ((C - A) - U * CX).Size();
	const float DX = (D - A) | U;
	const float DY = ((D - A) - U * DX).Size();
	if (!(CY + DY > 1e-6f)) return -1.0f;
	const float Cross = CX + (DX - CX) * (CY / (CY + DY));
	if (Cross < 0.0f || Cross > EdgeLength) return -1.0f;
	return FMath::Sqrt(FMath::Square(DX - CX) + FMath::Square(CY + DY));
}

struct FCSClothSplitHeapNode
{
	float Distance;
	int32 Node;
	bool operator<(const FCSClothSplitHeapNode& Other) const { return Distance < Other.Distance; }
};

constexpr float CSClothSplit_Unreached = TNumericLimits<float>::Max();

/**
 * 多源 Dijkstra。调用前 Distance 里没走到的节点必须是 CSClothSplit_Unreached；本函数只写走到的节点并记进 OutVisited，
 * 调用方据此复位（逐块跑的时候不必每次把整张表重刷一遍）。超过 MaxDistance 不再展开。Nearest 可选：每个节点最近的源。
 */
void CSClothSplit_Dijkstra(const FCSClothSplitGraph& Graph, TConstArrayView<int32> Sources, float MaxDistance,
	TArray<float>& Distance, TArray<int32>* Nearest, TArray<int32>& OutVisited)
{
	TArray<FCSClothSplitHeapNode> Heap;
	for (const int32 Source : Sources)
	{
		if (Distance[Source] == 0.0f) continue;
		if (Distance[Source] == CSClothSplit_Unreached) OutVisited.Add(Source);
		Distance[Source] = 0.0f;
		if (Nearest) (*Nearest)[Source] = Source;
		Heap.HeapPush(FCSClothSplitHeapNode{ 0.0f, Source });
	}
	while (Heap.Num() > 0)
	{
		FCSClothSplitHeapNode Top;
		Heap.HeapPop(Top, EAllowShrinking::No);
		if (Top.Distance > Distance[Top.Node]) continue;
		for (int32 Edge = Graph.Offsets[Top.Node]; Edge < Graph.Offsets[Top.Node + 1]; ++Edge)
		{
			const int32 Next = Graph.Neighbors[Edge];
			const float Candidate = Top.Distance + Graph.Lengths[Edge];
			if (Candidate >= Distance[Next] || Candidate > MaxDistance) continue;
			if (Distance[Next] == CSClothSplit_Unreached) OutVisited.Add(Next);
			Distance[Next] = Candidate;
			if (Nearest) (*Nearest)[Next] = (*Nearest)[Top.Node];
			Heap.HeapPush(FCSClothSplitHeapNode{ Candidate, Next });
		}
	}
}

int32 CSClothSplit_FindRoot(TArray<int32>& Parent, int32 X)
{
	while (Parent[X] != X)
	{
		Parent[X] = Parent[Parent[X]];
		X = Parent[X];
	}
	return X;
}

/** 三角面上线性插值的标量场在面内的梯度：g·E1 = F1 − F0、g·E2 = F2 − F0。退化三角返回零。 */
FVector3f CSClothSplit_TriangleGradient(const FVector3f& P0, const FVector3f& P1, const FVector3f& P2, float F0, float F1, float F2)
{
	const FVector3f E1 = P1 - P0;
	const FVector3f E2 = P2 - P0;
	const float A = E1 | E1;
	const float B = E1 | E2;
	const float C = E2 | E2;
	const float Det = A * C - B * B;
	if (!(FMath::Abs(Det) > 1e-12f)) return FVector3f::ZeroVector;
	const float D1 = F1 - F0;
	const float D2 = F2 - F0;
	return E1 * ((D1 * C - D2 * B) / Det) + E2 * ((D2 * A - D1 * B) / Det);
}
}

namespace CSClothBandSplit
{
bool Split(const FCSGpuMeshCPUData& Source, const FCSClothBandSplitSettings& Settings, const TArray<FVector>& SeedPoints,
	FCSClothBandSplitResult& OutResult)
{
	OutResult = FCSClothBandSplitResult();
	if (Source.AttrLayout != FCSGpuMeshCPUData::EAttrLayout::PerVertex || !Source.IsValid())
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] Refused: the source must be a valid per-vertex triangle mesh."));
		return false;
	}
	const int32 NumVertices = Source.Positions.Num();
	const int32 NumSourceTriangles = Source.Indices.Num() / 3;

	// --- 1. 焊接：渲染顶点 → 节点。UV 缝两侧是同一个节点，测地距离才连得过去。
	TArray<uint32> VertexToNode;
	TArray<int32> NodeSource;
	CSClothSetup::WeldPositions(Source.Positions, 0.01f, nullptr, VertexToNode, NodeSource);
	const int32 NumNodes = NodeSource.Num();
	TArray<FVector3f> NodePos;
	NodePos.SetNumUninitialized(NumNodes);
	for (int32 Node = 0; Node < NumNodes; ++Node) NodePos[Node] = Source.Positions[NodeSource[Node]];

	// --- 2. 节点三角（剔除焊接后退化的），记着来自哪个源三角
	TArray<FIntVector> Triangles;
	TArray<int32> TriangleSource;
	Triangles.Reserve(NumSourceTriangles);
	TriangleSource.Reserve(NumSourceTriangles);
	for (int32 Tri = 0; Tri < NumSourceTriangles; ++Tri)
	{
		const int32 A = int32(VertexToNode[Source.Indices[Tri * 3 + 0]]);
		const int32 B = int32(VertexToNode[Source.Indices[Tri * 3 + 1]]);
		const int32 C = int32(VertexToNode[Source.Indices[Tri * 3 + 2]]);
		if (A == B || B == C || A == C) continue;
		Triangles.Add(FIntVector(A, B, C));
		TriangleSource.Add(Tri);
	}
	if (Triangles.IsEmpty())
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] Refused: every triangle is degenerate after welding."));
		return false;
	}
	const int32 NumTriangles = Triangles.Num();

	TArray<FVector3f> NodeNormal;
	NodeNormal.SetNumZeroed(NumNodes);
	if (Source.Normals.Num() == NumVertices)
	{
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex) NodeNormal[VertexToNode[Vertex]] += Source.Normals[Vertex];
	}
	else
	{
		for (const FIntVector& Tri : Triangles)
		{
			// 引擎正面口径：cross(C−A, B−A)。
			const FVector3f Face = FVector3f::CrossProduct(NodePos[Tri.Z] - NodePos[Tri.X], NodePos[Tri.Y] - NodePos[Tri.X]);
			NodeNormal[Tri.X] += Face;
			NodeNormal[Tri.Y] += Face;
			NodeNormal[Tri.Z] += Face;
		}
	}
	for (FVector3f& Normal : NodeNormal) Normal = Normal.GetSafeNormal();

	// 边 → 两侧的面（切口、开放边界、捷径都要它）。
	TMap<uint64, FIntPoint> EdgeFaces;
	EdgeFaces.Reserve(NumTriangles * 2);
	for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		const int32 Corners[3] = { Triangles[Tri].X, Triangles[Tri].Y, Triangles[Tri].Z };
		for (int32 Side = 0; Side < 3; ++Side)
		{
			const uint64 Key = CSClothSplit_EdgeKey(Corners[Side], Corners[(Side + 1) % 3]);
			if (FIntPoint* Faces = EdgeFaces.Find(Key)) Faces->Y = Tri;
			else EdgeFaces.Add(Key, FIntPoint(Tri, INDEX_NONE));
		}
	}
	auto OppositeCorner = [&Triangles](int32 Tri, int32 A, int32 B)
	{
		const FIntVector& T = Triangles[Tri];
		return (T.X != A && T.X != B) ? T.X : ((T.Y != A && T.Y != B) ? T.Y : T.Z);
	};

	// 开放边界上的节点：切口平滑不动它们（否则会被拉出原网格的范围，锚点再也贴不回去）。
	TArray<bool> OnBorder;
	OnBorder.Init(false, NumNodes);
	TMap<uint64, float> Shortcuts;
	Shortcuts.Reserve(EdgeFaces.Num());
	for (const TPair<uint64, FIntPoint>& Edge : EdgeFaces)
	{
		const int32 A = int32(Edge.Key >> 32);
		const int32 B = int32(Edge.Key & 0xffffffffu);
		if (Edge.Value.Y == INDEX_NONE)
		{
			OnBorder[A] = true;
			OnBorder[B] = true;
			continue;
		}
		const int32 C = OppositeCorner(Edge.Value.X, A, B);
		const int32 D = OppositeCorner(Edge.Value.Y, A, B);
		if (C == D) continue;
		const float Length = CSClothSplit_UnfoldedDistance(NodePos[A], NodePos[B], NodePos[C], NodePos[D]);
		if (Length > 0.0f)
		{
			float& Existing = Shortcuts.FindOrAdd(CSClothSplit_EdgeKey(C, D), Length);
			Existing = FMath::Min(Existing, Length);
		}
	}

	FCSClothSplitGraph Graph;
	Graph.Build(NumNodes, Triangles, [&NodePos](int32 A, int32 B) { return FVector3f::Dist(NodePos[A], NodePos[B]); }, &Shortcuts);

	// --- 3. 起点：起点附近 SeedRadius 以内的节点；一个都没罩住就取最近的那个；没有起点取最高点
	TArray<int32> Seeds;
	{
		TSet<int32> SeedSet;
		const float RadiusSq = FMath::Square(FMath::Max(Settings.SeedRadius, 0.1f));
		for (const FVector& SeedPoint : SeedPoints)
		{
			const FVector3f Seed(SeedPoint);
			int32 Nearest = INDEX_NONE;
			float NearestSq = TNumericLimits<float>::Max();
			bool bCaptured = false;
			for (int32 Node = 0; Node < NumNodes; ++Node)
			{
				const float DistSq = FVector3f::DistSquared(NodePos[Node], Seed);
				if (DistSq <= RadiusSq)
				{
					SeedSet.Add(Node);
					bCaptured = true;
				}
				if (DistSq < NearestSq)
				{
					NearestSq = DistSq;
					Nearest = Node;
				}
			}
			if (!bCaptured && Nearest != INDEX_NONE) SeedSet.Add(Nearest);
		}
		if (SeedSet.IsEmpty())
		{
			int32 Top = 0;
			for (int32 Node = 1; Node < NumNodes; ++Node) if (NodePos[Node].Z > NodePos[Top].Z) Top = Node;
			SeedSet.Add(Top);
		}
		Seeds = SeedSet.Array();
	}

	// --- 4. 测地距离。与起点不连通的碎块（眼球、单独的配件）退化成到最近起点的直线距离。
	TArray<float> Dist;
	Dist.Init(CSClothSplit_Unreached, NumNodes);
	{
		TArray<int32> Visited;
		CSClothSplit_Dijkstra(Graph, Seeds, CSClothSplit_Unreached, Dist, nullptr, Visited);
	}
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		if (Dist[Node] != CSClothSplit_Unreached) continue;
		float Best = CSClothSplit_Unreached;
		for (const int32 Seed : Seeds) Best = FMath::Min(Best, FVector3f::Dist(NodePos[Node], NodePos[Seed]));
		Dist[Node] = Best;
	}

	// --- 5. 条带号 floor((dist + 噪声) / 带宽)；面取三个角的最小值（attribpromote 的 min）
	FRandomStream Random(Settings.Seed);
	const FVector NoiseOffset(Random.FRandRange(0.0f, 97.0f), Random.FRandRange(0.0f, 97.0f), Random.FRandRange(0.0f, 97.0f));
	const float Width = FMath::Max(Settings.BandWidth, 0.5f);
	const float InvPeriod = 1.0f / FMath::Max(Settings.BoundaryNoisePeriod, 0.1f);
	TArray<int32> NodeBand;
	NodeBand.SetNumUninitialized(NumNodes);
	for (int32 Node = 0; Node < NumNodes; ++Node)
	{
		const float Noise = Settings.BoundaryNoise > 0.0f
			? Settings.BoundaryNoise * FMath::PerlinNoise3D(FVector(NodePos[Node]) * InvPeriod + NoiseOffset) : 0.0f;
		NodeBand[Node] = FMath::FloorToInt32((Dist[Node] + Noise) / Width);
	}
	TArray<int32> FaceBand;
	FaceBand.SetNumUninitialized(NumTriangles);
	int32 MinBand = MAX_int32;
	int32 MaxBand = MIN_int32;
	for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		const FIntVector& T = Triangles[Tri];
		FaceBand[Tri] = FMath::Min3(NodeBand[T.X], NodeBand[T.Y], NodeBand[T.Z]);
		MinBand = FMath::Min(MinBand, FaceBand[Tri]);
		MaxBand = FMath::Max(MaxBand, FaceBand[Tri]);
	}

	// --- 6. 切口平滑：切口曲线（两侧面的条带号不同的边）沿自身做拉普拉斯，再带动 SmoothFalloff 以内的点。
	// 开放边界上的节点一律不动。
	TArray<FVector3f> Smoothed = NodePos;
	{
		TArray<TArray<int32, TInlineAllocator<2>>> CutNeighbors;
		CutNeighbors.SetNum(NumNodes);
		TArray<int32> CutNodes;
		for (const TPair<uint64, FIntPoint>& Edge : EdgeFaces)
		{
			if (Edge.Value.Y == INDEX_NONE || FaceBand[Edge.Value.X] == FaceBand[Edge.Value.Y]) continue;
			const int32 A = int32(Edge.Key >> 32);
			const int32 B = int32(Edge.Key & 0xffffffffu);
			if (CutNeighbors[A].IsEmpty()) CutNodes.Add(A);
			if (CutNeighbors[B].IsEmpty()) CutNodes.Add(B);
			CutNeighbors[A].Add(B);
			CutNeighbors[B].Add(A);
		}

		const float Step = FMath::Clamp(Settings.BoundarySmoothStep, 0.0f, 1.0f);
		if (Settings.BoundarySmoothIterations > 0 && Step > 0.0f && CutNodes.Num() > 0)
		{
			TArray<FVector3f> Next;
			for (int32 Iteration = 0; Iteration < Settings.BoundarySmoothIterations; ++Iteration)
			{
				Next = Smoothed;
				for (const int32 Node : CutNodes)
				{
					// 只平滑曲线中间的点：端点（1 个邻居）与三岔口（≥ 3 个）固定，切口不缩短、岔口不被拉散。
					if (CutNeighbors[Node].Num() != 2 || OnBorder[Node]) continue;
					const FVector3f Mid = (Smoothed[CutNeighbors[Node][0]] + Smoothed[CutNeighbors[Node][1]]) * 0.5f;
					Next[Node] = Smoothed[Node] + (Mid - Smoothed[Node]) * Step;
				}
				Smoothed = MoveTemp(Next);
			}

			// 附近的点跟着最近的切口节点挪同一个向量 × 线性衰减（Houdini 是把它们往平滑后的曲线上拉，
			// 那样会把切口两侧的三角压扁；平移不改三角形状）。
			const float Falloff = FMath::Max(Settings.SmoothFalloff, 0.0f);
			if (Falloff > 0.0f)
			{
				TArray<float> Near;
				Near.Init(CSClothSplit_Unreached, NumNodes);
				TArray<int32> NearCut;
				NearCut.Init(INDEX_NONE, NumNodes);
				TArray<int32> Reached;
				CSClothSplit_Dijkstra(Graph, CutNodes, Falloff, Near, &NearCut, Reached);
				for (const int32 Node : Reached)
				{
					if (!CutNeighbors[Node].IsEmpty() || OnBorder[Node] || NearCut[Node] == INDEX_NONE) continue;
					const int32 Cut = NearCut[Node];
					Smoothed[Node] = NodePos[Node] + (Smoothed[Cut] - NodePos[Cut]) * (1.0f - Near[Node] / Falloff);
				}
			}
		}
	}

	// --- 7. 旋流方向：Houdini BackVel 的表面项 v = cross(N, up)，up = 沿表面离开起点的方向（测地距离的梯度）。
	TArray<FVector3f> Swirl;
	Swirl.SetNumZeroed(NumNodes);
	{
		TArray<FVector3f> Gradient;
		Gradient.SetNumZeroed(NumNodes);
		for (const FIntVector& T : Triangles)
		{
			const FVector3f G = CSClothSplit_TriangleGradient(NodePos[T.X], NodePos[T.Y], NodePos[T.Z], Dist[T.X], Dist[T.Y], Dist[T.Z]);
			const float Area = 0.5f * FVector3f::CrossProduct(NodePos[T.Y] - NodePos[T.X], NodePos[T.Z] - NodePos[T.X]).Size();
			Gradient[T.X] += G * Area;
			Gradient[T.Y] += G * Area;
			Gradient[T.Z] += G * Area;
		}
		for (int32 Node = 0; Node < NumNodes; ++Node)
		{
			const FVector3f& N = NodeNormal[Node];
			const FVector3f Up = (Gradient[Node] - N * (N | Gradient[Node])).GetSafeNormal();
			if (!Up.IsNearlyZero()) Swirl[Node] = FVector3f::CrossProduct(N, Up).GetSafeNormal();
		}
	}

	// --- 8. 归一化高度（Houdini KaLou_faceattrib 的 high = @P.y，再 FitValue 到 [0, 1]；UE 是 Z 朝上）与逐点随机数
	float MinZ = TNumericLimits<float>::Max();
	float MaxZ = -TNumericLimits<float>::Max();
	for (const FVector3f& P : NodePos)
	{
		MinZ = FMath::Min(MinZ, P.Z);
		MaxZ = FMath::Max(MaxZ, P.Z);
	}
	const float InvHeight = MaxZ > MinZ ? 1.0f / (MaxZ - MinZ) : 0.0f;
	TArray<float> NodeRandom;
	NodeRandom.SetNumUninitialized(NumNodes);
	for (float& Value : NodeRandom) Value = Random.GetFraction();

	// --- 9. 块与沿条带的参数。单元 = (节点, 条带)：切口上的节点按两侧条带各算一个单元，同一个面的三个单元相连。
	TMap<uint64, int32> UnitOf;
	UnitOf.Reserve(NumNodes + NumNodes / 4);
	TArray<int32> UnitNode;
	TArray<FIntVector> UnitTriangles;
	UnitTriangles.SetNumUninitialized(NumTriangles);
	auto UnitKey = [MinBand](int32 Node, int32 Band) { return (uint64(uint32(Node)) << 32) | uint64(uint32(Band - MinBand)); };
	for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		const int32 Corners[3] = { Triangles[Tri].X, Triangles[Tri].Y, Triangles[Tri].Z };
		int32 Units[3];
		for (int32 K = 0; K < 3; ++K)
		{
			const uint64 Key = UnitKey(Corners[K], FaceBand[Tri]);
			if (const int32* Found = UnitOf.Find(Key)) Units[K] = *Found;
			else
			{
				Units[K] = UnitNode.Add(Corners[K]);
				UnitOf.Add(Key, Units[K]);
			}
		}
		UnitTriangles[Tri] = FIntVector(Units[0], Units[1], Units[2]);
	}
	const int32 NumUnits = UnitNode.Num();

	TArray<int32> Parent;
	Parent.SetNumUninitialized(NumUnits);
	for (int32 Unit = 0; Unit < NumUnits; ++Unit) Parent[Unit] = Unit;
	for (const FIntVector& T : UnitTriangles)
	{
		Parent[CSClothSplit_FindRoot(Parent, T.Y)] = CSClothSplit_FindRoot(Parent, T.X);
		Parent[CSClothSplit_FindRoot(Parent, T.Z)] = CSClothSplit_FindRoot(Parent, T.X);
	}
	TArray<int32> UnitPiece;
	UnitPiece.SetNumUninitialized(NumUnits);
	TArray<int32> PieceFirstUnit;
	{
		TMap<int32, int32> PieceOfRoot;
		for (int32 Unit = 0; Unit < NumUnits; ++Unit)
		{
			const int32 Root = CSClothSplit_FindRoot(Parent, Unit);
			if (const int32* Found = PieceOfRoot.Find(Root)) UnitPiece[Unit] = *Found;
			else
			{
				UnitPiece[Unit] = PieceFirstUnit.Add(Unit);
				PieceOfRoot.Add(Root, UnitPiece[Unit]);
			}
		}
	}

	// 每块做两次「最远点」扫描（kalou_createcurveuv 的近似）：u = 到一端的距离 / 到两端的距离之和；
	// Houdini 把 curvev < 0.5 的那一半的放开速度取反，条带两半往相反方向甩。
	TArray<float> UnitSign;
	UnitSign.Init(1.0f, NumUnits);
	{
		FCSClothSplitGraph UnitGraph;
		UnitGraph.Build(NumUnits, UnitTriangles,
			[&Smoothed, &UnitNode](int32 A, int32 B) { return FVector3f::Dist(Smoothed[UnitNode[A]], Smoothed[UnitNode[B]]); });
		TArray<float> Sweep;
		Sweep.Init(CSClothSplit_Unreached, NumUnits);
		TArray<float> FromA;
		FromA.SetNumZeroed(NumUnits);
		TArray<float> FromB;
		FromB.SetNumZeroed(NumUnits);
		TArray<int32> Visited;
		auto Farthest = [&UnitGraph, &Sweep, &Visited](int32 Start, TArray<float>* Keep)
		{
			Visited.Reset();
			CSClothSplit_Dijkstra(UnitGraph, TConstArrayView<int32>(&Start, 1), CSClothSplit_Unreached, Sweep, nullptr, Visited);
			int32 Far = Start;
			for (const int32 Unit : Visited)
			{
				if (Sweep[Unit] > Sweep[Far]) Far = Unit;
				if (Keep) (*Keep)[Unit] = Sweep[Unit];
			}
			for (const int32 Unit : Visited) Sweep[Unit] = CSClothSplit_Unreached;
			return Far;
		};
		for (const int32 First : PieceFirstUnit)
		{
			const int32 EndA = Farthest(First, nullptr);
			const int32 EndB = Farthest(EndA, &FromA);
			Farthest(EndB, &FromB);
		}
		for (int32 Unit = 0; Unit < NumUnits; ++Unit)
		{
			const float Total = FromA[Unit] + FromB[Unit];
			UnitSign[Unit] = (Total > 1e-6f && FromA[Unit] / Total < 0.5f) ? -1.0f : 1.0f;
		}
	}

	// --- 10. 被平滑挪动过的节点：锚点落在原网格上离它最近的三角里（在节点自己与一圈邻居的三角里找），
	// 宿主每帧交原网格的动画位置，锚点按重心坐标插值 —— 与 Houdini 用 sourceprim / sourceprimuv 绑回动画角色同理。
	TArray<FIntVector> NodeAnchor;
	NodeAnchor.Init(FIntVector(INDEX_NONE), NumNodes);
	TArray<FVector3f> NodeAnchorWeight;
	NodeAnchorWeight.Init(FVector3f(1.0f, 0.0f, 0.0f), NumNodes);
	{
		TArray<int32> TriOffsets;
		TriOffsets.Init(0, NumNodes + 1);
		for (const FIntVector& T : Triangles) { ++TriOffsets[T.X + 1]; ++TriOffsets[T.Y + 1]; ++TriOffsets[T.Z + 1]; }
		for (int32 Node = 0; Node < NumNodes; ++Node) TriOffsets[Node + 1] += TriOffsets[Node];
		TArray<int32> Cursor = TriOffsets;
		TArray<int32> NodeTriangles;
		NodeTriangles.SetNumUninitialized(TriOffsets[NumNodes]);
		for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
		{
			NodeTriangles[Cursor[Triangles[Tri].X]++] = Tri;
			NodeTriangles[Cursor[Triangles[Tri].Y]++] = Tri;
			NodeTriangles[Cursor[Triangles[Tri].Z]++] = Tri;
		}

		for (int32 Node = 0; Node < NumNodes; ++Node)
		{
			if (FVector3f::DistSquared(Smoothed[Node], NodePos[Node]) < 1e-8f) continue;
			++OutResult.NumSmoothedVertices;
			const FVector Point(Smoothed[Node]);
			double BestSq = TNumericLimits<double>::Max();
			auto TryTriangles = [&](int32 Around)
			{
				for (int32 I = TriOffsets[Around]; I < TriOffsets[Around + 1]; ++I)
				{
					const int32 Tri = NodeTriangles[I];
					const FVector A(NodePos[Triangles[Tri].X]), B(NodePos[Triangles[Tri].Y]), C(NodePos[Triangles[Tri].Z]);
					const FVector Closest = FMath::ClosestPointOnTriangleToPoint(Point, A, B, C);
					const double DistSq = FVector::DistSquared(Closest, Point);
					if (DistSq >= BestSq) continue;
					const FVector Bary = FMath::ComputeBaryCentric2D(Closest, A, B, C);
					if (Bary.ContainsNaN()) continue;
					BestSq = DistSq;
					const int32 SourceTri = TriangleSource[Tri];
					NodeAnchor[Node] = FIntVector(int32(Source.Indices[SourceTri * 3 + 0]), int32(Source.Indices[SourceTri * 3 + 1]),
						int32(Source.Indices[SourceTri * 3 + 2]));
					NodeAnchorWeight[Node] = FVector3f(Bary);
				}
			};
			TryTriangles(Node);
			for (int32 Edge = Graph.Offsets[Node]; Edge < Graph.Offsets[Node + 1]; ++Edge) TryTriangles(Graph.Neighbors[Edge]);
		}
	}

	// --- 11. 输出：顶点键 = (源渲染顶点, 条带)。同一条带里的 UV 缝副本照旧分开（setup 按位置 + 焊接键焊回一个粒子），
	// 切口两侧焊接键不同，焊不回去。
	FCSGpuMeshCPUData& Mesh = OutResult.Mesh;
	Mesh.Reset();
	Mesh.NumTexCoordChannels = FMath::Clamp(Source.NumTexCoordChannels, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	Mesh.Materials = Source.Materials;
	Mesh.SourceSpace = Source.SourceSpace;
	Mesh.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	const bool bNormals = Source.Normals.Num() == NumVertices;
	const bool bTangents = Source.Tangents.Num() == NumVertices;
	const bool bSigns = Source.BinormalSigns.Num() == NumVertices;
	const bool bColors = Source.Colors.Num() == NumVertices;
	const bool bSlots = Source.TriangleMaterialSlots.Num() == NumSourceTriangles;
	FCSClothVertexBinding& Binding = OutResult.Binding;

	TMap<uint64, int32> OutputOf;
	OutputOf.Reserve(NumVertices + NumVertices / 4);
	auto Emit = [&](int32 SourceVertex, int32 Node, int32 Unit, int32 Band)
	{
		const uint64 Key = (uint64(uint32(SourceVertex)) << 32) | uint64(uint32(Band - MinBand));
		if (const int32* Found = OutputOf.Find(Key)) return *Found;
		const int32 Index = Mesh.Positions.Add(Smoothed[Node]);
		Mesh.Normals.Add(bNormals ? Source.Normals[SourceVertex] : NodeNormal[Node]);
		Mesh.Tangents.Add(bTangents ? Source.Tangents[SourceVertex] : FVector3f(1.0f, 0.0f, 0.0f));
		Mesh.BinormalSigns.Add(bSigns ? Source.BinormalSigns[SourceVertex] : 1.0f);
		for (int32 Channel = 0; Channel < Mesh.NumTexCoordChannels; ++Channel)
		{
			const TArray<FVector2f>& UVs = Source.TexCoordChannels[Channel];
			Mesh.TexCoordChannels[Channel].Add(UVs.IsValidIndex(SourceVertex) ? UVs[SourceVertex] : FVector2f::ZeroVector);
		}
		if (bColors) Mesh.Colors.Add(Source.Colors[SourceVertex]);

		Binding.WeldKeys.Add(uint32(Band - MinBand));
		if (NodeAnchor[Node].X == INDEX_NONE)
		{
			Binding.AnchorSources.Add(FIntVector(SourceVertex, INDEX_NONE, INDEX_NONE));
			Binding.AnchorWeights.Add(FVector3f(1.0f, 0.0f, 0.0f));
		}
		else
		{
			Binding.AnchorSources.Add(NodeAnchor[Node]);
			Binding.AnchorWeights.Add(NodeAnchorWeight[Node]);
		}
		Binding.AnchorParams.Add(FVector4f((NodePos[Node].Z - MinZ) * InvHeight, NodeRandom[Node], UnitSign[Unit], 0.0f));
		Binding.SwirlDirs.Add(Swirl[Node]);
		OutResult.Distance.Add(Dist[Node]);
		OutResult.PieceIds.Add(UnitPiece[Unit]);
		OutputOf.Add(Key, Index);
		return Index;
	};

	Mesh.Indices.Reserve(NumTriangles * 3 * (Settings.bDoubleSided ? 2 : 1));
	for (int32 Tri = 0; Tri < NumTriangles; ++Tri)
	{
		const int32 SourceTri = TriangleSource[Tri];
		const int32 Corners[3] = { Triangles[Tri].X, Triangles[Tri].Y, Triangles[Tri].Z };
		const int32 Units[3] = { UnitTriangles[Tri].X, UnitTriangles[Tri].Y, UnitTriangles[Tri].Z };
		for (int32 K = 0; K < 3; ++K)
		{
			Mesh.Indices.Add(uint32(Emit(int32(Source.Indices[SourceTri * 3 + K]), Corners[K], Units[K], FaceBand[Tri])));
		}
		Mesh.TriangleMaterialSlots.Add(bSlots ? Source.TriangleMaterialSlots[SourceTri] : 0);
	}

	// 背面：同一批位置、法线取反、副切线符号跟着翻（UV 没变，世界里的副切线不动），绕序反过来；绑定原样复制，
	// setup 把它焊到正面的同一批粒子上，并按「同一组粒子的重复三角」把它从约束与法线累加里剔掉。
	if (Settings.bDoubleSided)
	{
		const int32 Front = Mesh.Positions.Num();
		for (int32 Vertex = 0; Vertex < Front; ++Vertex)
		{
			// 先拷成局部值：TArray::Add 不收来自自身的引用（扩容会把它搬走）。
			const FVector3f Position = Mesh.Positions[Vertex];
			const FVector3f Normal = Mesh.Normals[Vertex];
			const FVector3f Tangent = Mesh.Tangents[Vertex];
			const float Sign = Mesh.BinormalSigns[Vertex];
			Mesh.Positions.Add(Position);
			Mesh.Normals.Add(-Normal);
			Mesh.Tangents.Add(Tangent);
			Mesh.BinormalSigns.Add(-Sign);
			for (int32 Channel = 0; Channel < Mesh.NumTexCoordChannels; ++Channel)
			{
				const FVector2f UV = Mesh.TexCoordChannels[Channel][Vertex];
				Mesh.TexCoordChannels[Channel].Add(UV);
			}
			if (bColors)
			{
				const FVector4f Color = Mesh.Colors[Vertex];
				Mesh.Colors.Add(Color);
			}
			const uint32 WeldKey = Binding.WeldKeys[Vertex];
			const FIntVector AnchorSource = Binding.AnchorSources[Vertex];
			const FVector3f AnchorWeight = Binding.AnchorWeights[Vertex];
			const FVector4f AnchorParams = Binding.AnchorParams[Vertex];
			const FVector3f SwirlDir = Binding.SwirlDirs[Vertex];
			const float Distance = OutResult.Distance[Vertex];
			const int32 Piece = OutResult.PieceIds[Vertex];
			Binding.WeldKeys.Add(WeldKey);
			Binding.AnchorSources.Add(AnchorSource);
			Binding.AnchorWeights.Add(AnchorWeight);
			Binding.AnchorParams.Add(AnchorParams);
			Binding.SwirlDirs.Add(SwirlDir);
			OutResult.Distance.Add(Distance);
			OutResult.PieceIds.Add(Piece);
		}
		const int32 FrontIndices = Mesh.Indices.Num();
		const uint32 Offset = uint32(Front);
		for (int32 Index = 0; Index < FrontIndices; Index += 3)
		{
			const uint32 A = Mesh.Indices[Index + 0] + Offset;
			const uint32 B = Mesh.Indices[Index + 1] + Offset;
			const uint32 C = Mesh.Indices[Index + 2] + Offset;
			Mesh.Indices.Append({ A, C, B });
			const int32 Slot = Mesh.TriangleMaterialSlots[Index / 3];
			Mesh.TriangleMaterialSlots.Add(Slot);
		}
	}

	OutResult.NumBands = MaxBand - MinBand + 1;
	OutResult.NumPieces = PieceFirstUnit.Num();
	OutResult.NumSeedVertices = Seeds.Num();
	UE_LOG(LogCSCloth, Log, TEXT("[CSClothSplit] %d source vertices -> %d bands, %d pieces, %d output vertices (%d seed vertices, %d smoothed)."),
		NumVertices, OutResult.NumBands, OutResult.NumPieces, Mesh.Positions.Num(), OutResult.NumSeedVertices, OutResult.NumSmoothedVertices);
	return true;
}
}

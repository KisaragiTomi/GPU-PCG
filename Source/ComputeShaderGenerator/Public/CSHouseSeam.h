#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"       // FPath / FWallFrame / FElement / SolveRun —— 接缝砖是门框砖的一条新路径类型
#include "CSHouseProfile.h"     // FCSWallCut / CSHouse_GetEdge —— 墙在哪儿只能有一个真源
#include "Misc/Guid.h"          // 规范序的键
#include "Templates/TypeHash.h" // HashCombine —— 接缝身份 → 逐实例随机数的基

/**
 * 两栋房交汇处的接缝（TinyGladeHouse D7；2026-08-30「裁决二」）。
 *
 * -----------------------------------------------------------------------------
 * 为什么是纯函数而不是 actor
 * -----------------------------------------------------------------------------
 * 用户原话：「两栋房交汇时只产生接缝砖，其它任何内容都是独立的。」⇒ `ACSHouseSeamActor`
 * （中立 actor + 两端弱引用 + 自 tick + 交点表生命周期 + 角柱邻近合并 + 跨接缝仲裁）**整套否决**。
 * TG 侧的实证同向且更硬（`Docs/TinyGlade/Progress_ModuleComparison.md（卷一）` §2.3）：那边这一级只有两个参数
 * —— `Res<IntraShapeCorners> → ResMut<InterShapeBrickStitches>`，零地形、零历史、零撤销、零 UI。
 *
 * 所以这里一个可变量都没有：输入两房的 footprint / 朝向 / 高度，输出砖列。**两栋房各自算出
 * 同一条接缝是有意的**，不是浪费 —— 它换掉的是"谁拥有这条缝"这个问题，连带换掉归属、撤销、
 * 生命周期、以及"A 删了 B 身上的缝谁来收"这一整类 bug。
 *
 * ⚠️ **正因为两边都画，逐位相同就不再是洁癖而是正确性**：同一条缝会有两份重叠的砖实例，
 * 位置只要差一个 ulp 就可能在深度上互相闪烁。`Canonical()` 把无序对钉成有序对，之后的每一步
 * 都是同一个函数吃同一份参数 ⇒ 两边的输出**逐位**相同。单测 `House.SeamOrderInvariant`
 * 拿 memcmp 钉住这条。
 *
 * ⚠️ 逐实例随机数同理，且它**不在**几何里：砖的槽位是"这栋房自己的第几块砖"，两栋房必然不同。
 * 所以砖路带一个 `RandomBase`（`CSHouseFrame::FElement`），接缝那条从**接缝身份**派生而不是
 * 从槽位派生 —— 不这么做的话，将来谁给砖材质接一个 `PerInstanceRandom` 色差，这两份重叠的砖
 * 就会以不同的颜色互相闪，而几何断言全绿。
 *
 * -----------------------------------------------------------------------------
 * 相交处那个"洞"是 clip 出来的，不是挖出来的
 * -----------------------------------------------------------------------------
 * TG 在形状相交处先开洞再砌缝砖（`add_hole_at_shape_intersection` → `WallHoles`）。本项目**不能
 * 照抄那一步**：2026-08-30「裁决三」是全局不变量 —— 避免所有真几何洞，几何永远实心，要挖一律
 * 走顶点色或逐像素 clip 场。
 *
 * 所以接缝的洞复用门窗那套设施，一行新 shader 都没加：`CutOnEdge` 算出"这面墙被邻居 footprint
 * 盖住的那一段"，交给 `CSHouse_SeamClipField` 变成一块矩形裁剪场，`CSHouse_BuildBodySoup` 照常
 * 把那截墙砌成实心盒、由墙材质按 UV1 逐像素 discard。代价与门拱逐字相同（距离场 / Lumen 把
 * masked 洞当实心墙），用户已知情接受。
 *
 * -----------------------------------------------------------------------------
 * 本轮**不做**的（裁决二明写的退出范围，别顺手加）
 * -----------------------------------------------------------------------------
 * 角柱邻近合并、跨接缝仲裁、接缝接受 openings。第三条的落点在 `CSHouse_BuildBodySoup`：
 * 洞面板自带的裁剪场优先，落在接缝区间里的洞**不受接缝裁剪影响** —— 一块面板只带一个裁剪场，
 * 而"两个场怎么合"正是被划出去的那件事。
 */
namespace CSHouseSeam
{
/** 交点 / 裁剪区间短于这个就不值得出砖出洞 cm（与房体面板那条 0.5 cm 的下限同量级）。 */
constexpr float MinSpan = 1.0f;

/**
 * 一栋房参与接缝所需的**全部**输入。
 *
 * 刻意只有权威属性（身份 / 摆位 / 尺寸），**没有任何派生表或缓存** —— 裁决二那句"零共享状态"
 * 约束的是状态，不是只读的输入。地面高度也**故意不在**里面：接缝的底取两房房底的较高者，
 * 而房底本来就已经把地面吸收进去了（`ACSHouseActor::ComputeSeatZ`）。再引一次地面等于给
 * 这条纯函数加一个它不需要的依赖，且会让"交换顺序结果相同"多一个能坏掉的地方。
 */
struct FHouse
{
	/** 稳定身份。**只用来定规范序**，不参与任何几何。 */
	FGuid Id;
	/** 房子在世界里的 XY 与 yaw（度）—— 与 `ACSHouseActor::GetBuildTransform()` 同口径（只取 yaw）。 */
	FVector2D Center = FVector2D::ZeroVector;
	float Yaw = 0.0f;
	/** 局部空间的闭合折线（逆时针），与 `ACSHouseActor::GetFootprint()` 同一份。 */
	FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));
	/** 房底世界 Z（落座之后）。 */
	float BaseZ = 0.0f;
	float WallHeight = 300.0f;
	float WallThickness = 24.0f;

	float EaveZ() const { return BaseZ + WallHeight; }
	/** 外接圆半径：邻近粗筛用（离局部原点最远的顶点；矩形上就是半对角线）。 */
	float Reach() const
	{
		double MaxSq = 0.0;
		for (const FVector2D& V : Footprint.Verts) MaxSq = FMath::Max(MaxSq, V.SizeSquared());
		return float(FMath::Sqrt(MaxSq));
	}
};

/**
 * 无序对 → 有序对。**这是"交换两房顺序结果逐位相同"的唯一机关**，其余每一步都只是同一个
 * 函数吃同一份参数。
 *
 * 按 GUID 而不是按几何定序：几何量是浮点的，两栋同尺寸的房子可以在某一帧比出相反的大小
 * （而且只在那一帧），于是砖列会无声地换一次序。GUID 是稳定的、离散的、且与摆位无关。
 */
inline bool IdLess(const FGuid& A, const FGuid& B)
{
	if (A.A != B.A) return A.A < B.A;
	if (A.B != B.B) return A.B < B.B;
	if (A.C != B.C) return A.C < B.C;
	return A.D < B.D;
}

/** 规范序。两个 GUID 相等（同一栋房 / 都没设）时按原样返回，调用方本来就该先排除自己。 */
inline void Canonical(const FHouse& A, const FHouse& B, const FHouse*& OutFirst, const FHouse*& OutSecond)
{
	const bool bAFirst = !IdLess(B.Id, A.Id);
	OutFirst = bAFirst ? &A : &B;
	OutSecond = bAFirst ? &B : &A;
}

/** 接缝身份 → 逐实例随机数的基。规范序之下两栋房算出同一个值（这正是它存在的理由）。 */
inline uint32 SeamSeed(const FHouse& A, const FHouse& B)
{
	const FHouse* First = nullptr;
	const FHouse* Second = nullptr;
	Canonical(A, B, First, Second);
	return HashCombine(GetTypeHash(First->Id), GetTypeHash(Second->Id));
}

/** 世界 XY → 房子局部（`GetBuildTransform()` 的逆，只有 yaw 与平移）。 */
inline FVector2D ToLocal(const FHouse& H, const FVector2D& World)
{
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	const FVector2D D = World - H.Center;
	return FVector2D(D.X * C + D.Y * S, -D.X * S + D.Y * C);
}

/** 房子局部 XY → 世界。 */
inline FVector2D ToWorld(const FHouse& H, const FVector2D& Local)
{
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	return H.Center + FVector2D(Local.X * C - Local.Y * S, Local.X * S + Local.Y * C);
}

/** 轮廓顶点表（世界 XY）。多数房子不超过 8 个顶点，放栈上。 */
using FContour = TArray<FVector2D, TInlineAllocator<8>>;

/**
 * footprint 轮廓的顶点（世界 XY），**顺序与 `CSHouse_GetEdge` 的边号一一对应**：
 * 第 k 条轮廓边 = Out[k] → Out[(k+1) % N]。
 *
 * 用的是外皮线段 —— 斜接口径下它与 `CSHouse_GetEdge` 的 `Start → Start + U·Len` 是同一条线。
 * （直角对接时代这里特意不用墙段，因为那时奇数边两端各缩了 T，拿它求交会在角附近漏交点。）
 */
inline void FootprintCorners(const FHouse& H, FContour& Out)
{
	Out.Reset();
	for (const FVector2D& V : H.Footprint.Verts) Out.Add(ToWorld(H, V));
}

/** 第 k 条轮廓边的**世界外法线**（局部 −In，见 `CSHouse_GetEdge`）。 */
inline FVector2D EdgeOutward(const FHouse& H, int32 EdgeIndex)
{
	const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, H.Footprint, H.WallThickness);
	const FVector2D LocalOut(-F.In.X, -F.In.Y);
	const float Rad = FMath::DegreesToRadians(H.Yaw);
	const double C = FMath::Cos(Rad), S = FMath::Sin(Rad);
	return FVector2D(LocalOut.X * C - LocalOut.Y * S, LocalOut.X * S + LocalOut.Y * C);
}

/** 两房的 Z 区间交集（世界）。返回 false = 一栋整个在另一栋上面，够不着。 */
inline bool OverlapZ(const FHouse& A, const FHouse& B, float& OutBottom, float& OutTop)
{
	OutBottom = FMath::Max(A.BaseZ, B.BaseZ);
	OutTop = FMath::Min(A.EaveZ(), B.EaveZ());
	return OutTop - OutBottom > MinSpan;
}

/**
 * 两房是否真的交汇：**footprint 真重叠 + Z 区间相交**（用户裁决：不是"靠得近"）。
 *
 * 分离轴取两条折线**每一条边**的法线 —— 2D 下凸多边形的候选分离轴就是这些，没有第三类。
 * 矩形时代只测两根（另两条边与它们平行），折线上少测一根就会把「包围盒相交、斜边之间隔着
 * 一条缝」的两栋房判成相交。
 *
 * 凹折线上它是**保守**的：找到分离轴就一定分离，找不到时可能误报相交。误报无害 ——
 * 下游的交点与裁剪段是逐边精确求的，误报的一对算出来什么都没有。
 */
inline bool IntersectsXY(const FHouse& A, const FHouse& B)
{
	if (!A.Footprint.IsValidFootprint() || !B.Footprint.IsValidFootprint()) return false;

	FContour CornersA, CornersB;
	FootprintCorners(A, CornersA);
	FootprintCorners(B, CornersB);

	auto Separated = [](const FContour& P, const FContour& Q, const FVector2D& Axis)
	{
		// 从第 0 个角起算而不是从 ±FLT_MAX 起算：少一个"极值常量选错类型"的坑，也少一个 include。
		double PMin = FVector2D::DotProduct(P[0], Axis), PMax = PMin;
		double QMin = FVector2D::DotProduct(Q[0], Axis), QMax = QMin;
		for (int32 K = 1; K < P.Num(); ++K)
		{
			const double DP = FVector2D::DotProduct(P[K], Axis);
			PMin = FMath::Min(PMin, DP); PMax = FMath::Max(PMax, DP);
		}
		for (int32 K = 1; K < Q.Num(); ++K)
		{
			const double DQ = FVector2D::DotProduct(Q[K], Axis);
			QMin = FMath::Min(QMin, DQ); QMax = FMath::Max(QMax, DQ);
		}
		// 恰好相切不算相交：触发条件是"真正重叠"，相切的两栋房各自的墙面正好贴上，没有穿插要遮。
		return PMax <= QMin + MinSpan || QMax <= PMin + MinSpan;
	};

	for (int32 K = 0; K < CornersA.Num(); ++K)
	{
		if (Separated(CornersA, CornersB, EdgeOutward(A, K))) return false;
	}
	for (int32 K = 0; K < CornersB.Num(); ++K)
	{
		if (Separated(CornersA, CornersB, EdgeOutward(B, K))) return false;
	}
	return true;
}

/** footprint 真重叠 **且** Z 区间相交。横缝（承托 / 悬挑）只看前一半，见 `FCSHouseContact::Classify`。 */
inline bool Intersects(const FHouse& A, const FHouse& B)
{
	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(A, B, Bottom, Top)) return false;
	return IntersectsXY(A, B);
}

/** 粗筛：外接圆都够不着就连交点都不用算。 */
inline bool WithinReach(const FHouse& A, const FHouse& B)
{
	return FVector2D::DistSquared(A.Center, B.Center) <= FMath::Square(A.Reach() + B.Reach());
}

/** 一个轮廓交点 = 一根接缝砖柱。 */
struct FCorner
{
	/** 世界 XY。 */
	FVector2D Point = FVector2D::ZeroVector;
	/** 两墙外法线的角平分（单位，世界 XY）：砖的进深轴朝它，也就是朝"两栋房外面"那个象限。 */
	FVector2D Outward = FVector2D(1.0, 0.0);
	/** 两面墙各自的外法线（单位，世界 XY）：`CornerNearlyParallel` 用它判"近乎贴合、不出柱"。 */
	FVector2D NormalA = FVector2D(1.0, 0.0);
	FVector2D NormalB = FVector2D(0.0, 1.0);
	/** 世界 Z 区间：底取两房房底的较高者，顶取两檐口的较低者。 */
	float BottomZ = 0.0f;
	float TopZ = 0.0f;
};

/**
 * 两房轮廓的交点表。**规范序内部化** ⇒ `BuildCorners(A,B)` 与 `BuildCorners(B,A)` 逐位相同。
 *
 * N×M 次线段求交，顺序固定为"规范序第一栋的边号 × 第二栋的边号"，所以交点次序也是确定的
 * —— 不排序、也不能排序：按坐标排序又会把浮点比较请回来。
 *
 * 两个凸轮廓相交出偶数个交点（两矩形一般 2 或 4，带 yaw 最多 8）。平行边共线的退化情形直接跳过：那种情形下
 * 两面墙是贴合的，没有穿插要遮，硬造一根柱子反而多出一个孤零零的砖堆。
 */
inline int32 BuildCorners(const FHouse& A, const FHouse& B, TArray<FCorner>& OutCorners)
{
	OutCorners.Reset();
	if (!Intersects(A, B)) return 0;

	const FHouse* First = nullptr;
	const FHouse* Second = nullptr;
	Canonical(A, B, First, Second);

	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(*First, *Second, Bottom, Top)) return 0;

	FContour P, Q;
	FootprintCorners(*First, P);
	FootprintCorners(*Second, Q);

	for (int32 I = 0; I < P.Num(); ++I)
	{
		const FVector2D P0 = P[I], D1 = P[(I + 1) % P.Num()] - P0;
		const FVector2D NI = EdgeOutward(*First, I);
		for (int32 J = 0; J < Q.Num(); ++J)
		{
			const FVector2D Q0 = Q[J], D2 = Q[(J + 1) % Q.Num()] - Q0;
			const double Denom = D1.X * D2.Y - D1.Y * D2.X;
			if (FMath::Abs(Denom) <= UE_DOUBLE_KINDA_SMALL_NUMBER) continue;   // 平行/共线：见上
			const FVector2D W = Q0 - P0;
			const double T = (W.X * D2.Y - W.Y * D2.X) / Denom;
			const double U = (W.X * D1.Y - W.Y * D1.X) / Denom;
			if (T < 0.0 || T > 1.0 || U < 0.0 || U > 1.0) continue;

			const FVector2D NJ = EdgeOutward(*Second, J);
			FCorner Corner;
			Corner.Point = P0 + D1 * T;
			// 角平分指向"两房外面"那个象限：交点周围四个象限里，只有 (·NI > 0 且 ·NJ > 0)
			// 那一个是两栋房都在外面的，也正是两条裁剪断口露头的地方。两法线接近反向时
			// （两面墙近乎平行对撞）和退化成零向量，退回其中一条法线 —— 那种情形下砖朝哪
			// 都一样，重要的是别产出一个零向量把整根柱子变成镜像。
			const FVector2D Bisect = NI + NJ;
			Corner.Outward = Bisect.SizeSquared() > UE_DOUBLE_KINDA_SMALL_NUMBER ? Bisect.GetSafeNormal() : NI;
			Corner.NormalA = NI;
			Corner.NormalB = NJ;
			Corner.BottomZ = Bottom;
			Corner.TopZ = Top;
			OutCorners.Add(Corner);
		}
	}
	return OutCorners.Num();
}

/**
 * `Self` 的第 `EdgeIndex` 面墙上被 `Other` 的 footprint 盖住的那一段（墙空间 S 区间 + Z 区间）。
 *
 * 参数化按 `CSHouse_GetEdge`（外皮线段、S 从外角点起算，与面板同一个 S），再把两端点拿到
 * Other 的局部系里做 Cyrus–Beck 区间裁剪：Other 的每条边是一个半平面「(P − Start)·In ≥ 0」，
 * 各夹一次即可 —— 没有迭代，夹的顺序不改结果。矩形上这四个半平面就是原来 Liang–Barsky 的四条轴对齐边界。
 *
 * ⚠️ **只对凸的 Other 成立**：凹折线的内部不是半平面的交，一面墙可能被盖住两段，而 `FCSWallCut`
 * 一次只装一段。凹 footprint 落地时这里要换成「逐边求交 + 输出多段」。
 */
inline bool CutOnEdge(const FHouse& Self, const FHouse& Other, int32 EdgeIndex, FCSWallCut& OutCut)
{
	OutCut = FCSWallCut();
	float Bottom = 0.0f, Top = 0.0f;
	if (!OverlapZ(Self, Other, Bottom, Top)) return false;

	const FCSHouseEdgeFrame F = CSHouse_GetEdge(EdgeIndex, Self.Footprint, Self.WallThickness);
	if (F.Len <= MinSpan) return false;

	if (!Other.Footprint.IsValidFootprint()) return false;
	const FVector2D A = ToLocal(Other, ToWorld(Self, F.Start));
	const FVector2D Bv = ToLocal(Other, ToWorld(Self, F.Start + F.U * F.Len));
	const FVector2D Dir = Bv - A;

	double T0 = 0.0, T1 = 1.0;
	auto Clip = [&T0, &T1](double P, double Q)
	{
		if (FMath::Abs(P) <= UE_DOUBLE_SMALL_NUMBER) return Q >= 0.0;   // 平行于这条边界：在界内才继续
		const double R = Q / P;
		if (P < 0.0) { if (R > T1) return false; if (R > T0) T0 = R; }
		else { if (R < T0) return false; if (R < T1) T1 = R; }
		return true;
	};
	for (int32 J = 0; J < Other.Footprint.NumEdges(); ++J)
	{
		// 半平面「(A + t·Dir − Start)·In ≥ 0」⇔「−(Dir·In)·t ≤ (A − Start)·In」。
		// 边框架的 In 在轴对齐边上是精确的 (±1, 0) / (0, ±1)，矩形上与原来的四次夹逐位相同。
		const FCSHouseEdgeFrame G = CSHouse_GetEdge(J, Other.Footprint, Other.WallThickness);
		if (G.Len <= 0.0f) continue;
		if (!Clip(-FVector2D::DotProduct(Dir, G.In), FVector2D::DotProduct(A - G.Start, G.In))) return false;
	}

	OutCut.EdgeIndex = EdgeIndex;
	OutCut.MinS = float(T0 * F.Len);
	OutCut.MaxS = float(T1 * F.Len);
	// 墙空间：Z 从这栋房自己的房底起算。
	OutCut.BottomZ = Bottom - Self.BaseZ;
	OutCut.TopZ = FMath::Min(Top - Self.BaseZ, Self.WallHeight);
	return OutCut.IsValid() && OutCut.MaxS - OutCut.MinS > MinSpan;
}

/**
 * 交点 → 砖路元素（**追加**写，返回本次追加的砖数）。
 *
 * 砖路就是墩那一种：只有一条竖直段（`EMidKind::None` + 只出左樘），从 `BottomZ` 砌到 `TopZ`。
 * 复用 `CSHouseFrame` 的好处不只是省代码 —— 容量、交接、剔除球、`SaveToStaticMesh` 出口、
 * 以及那条"材质勾没勾 `bUsedWithInstancedStaticMeshes`"的执行面判据全都跟着白拿。
 *
 * 世界框架的取法：路径是竖直的 ⇒ 切向恒 (0,1) ⇒ kernel 里的面内朝外法线 = 切向逆时针转 90°
 * = (−1, 0)，也就是 **−AxisU**。所以 `AxisU` 要取角平分的**反向**，砖的进深轴才朝着两房外面
 * 那个象限。写成 `AxisU = Outward` 的症状是砖整根朝里、进深轴插进房间（而位置完全正确）。
 */
inline int32 BuildCornerElements(const TArray<FCorner>& Corners, uint32 Seed,
	const CSHouseFrame::FBrickParams& Params, TArray<CSHouseFrame::FElement>& InOutElements)
{
	// 全局砖序号跨"门框砖 + 接缝砖 + 角石"连续（整栋房子一个 dispatch），所以起点要接着已有元素数。
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	const int32 Before = Cursor;

	for (int32 Index = 0; Index < Corners.Num(); ++Index)
	{
		const FCorner& Corner = Corners[Index];
		// 逐实例随机数从**接缝身份 + 交点序号**派生，不从槽位派生：槽位是"这栋房自己的第几块砖"，
		// 两栋房必然不同，而这两份砖是重叠的（见文件头）。
		CSHouseFrame::AppendColumn(Corner.Point, Corner.Outward, Corner.BottomZ, Corner.TopZ,
			CSHouseFrame::PathRandomBase(Seed, CSHouseFrame::EPathFamily::Seam, Index),
			Params, InOutElements, Cursor);
	}
	return Cursor - Before;
}
/**
 * 交点离任一房 footprint 顶点是否不足 `Clearance` cm（世界 XY）。
 *
 * TG 的对位是 `spawn_stitches` 里那条「tag=1 且离最近矩形顶点 < 0.2 m 就不砌」：转角砖（角石）
 * 已经在顶点上，再立一根缝柱只会撞在一起。用它取代早先计划里的「外露走线长 SeamMinExposure」。
 */
inline bool CornerNearVertex(const FHouse& A, const FHouse& B, const FVector2D& Point, float Clearance)
{
	if (Clearance <= 0.0f) return false;
	const double ClearSq = double(Clearance) * double(Clearance);
	for (const FHouse* H : { &A, &B })
	{
		for (const FVector2D& V : H->Footprint.Verts)
		{
			if (FVector2D::DistSquared(ToWorld(*H, V), Point) < ClearSq) return true;
		}
	}
	return false;
}

/** 两墙外法线近乎平行同向（点积 ≥ MinDot）：两面墙近乎贴合，没有穿插要遮。TG 用 0.9。 */
inline bool CornerNearlyParallel(const FCorner& Corner, float MinDot)
{
	return FVector2D::DotProduct(Corner.NormalA, Corner.NormalB) >= MinDot;
}

/**
 * 一根待砌的柱（交点经过滤之后的样子，也是邻近合并的输入与输出）。
 *
 * 没有归属、没有种子：柱只在它所属的那条接触里产生与合并，两者都是接触自己的（`FCSHouseContact::Owner` /
 * `Seed`）。早先的 `OwnerId` / `Seed` 字段是给跨接触聚簇用的，随 2026-09-16 晚的裁决一起删掉。
 */
struct FPost
{
	FVector2D Point = FVector2D::ZeroVector;
	FVector2D Outward = FVector2D(1.0, 0.0);
	float BottomZ = 0.0f;
	float TopZ = 0.0f;
};

/** 出柱的四道门槛，都取**出砖方自己**的参数（`ACSHouseActor::MakeSeamPostParams`）。 */
struct FPostParams
{
	/** 交点离任一房 footprint 顶点不足它就不立（角石已在那儿）。TG 0.2 m。 */
	float VertexClearance = 20.0f;
	/** 两墙外法线点积 ≥ 它（近乎平行同向）不立。TG 0.9。 */
	float ParallelDot = 0.9f;
	/** 同一条接触里 XY 更近的交点并成一根。 */
	float MergeDistance = 30.0f;
	/** 柱高不足它不砌。TG 1.5 m（3 块砖）。 */
	float MinHeight = 0.0f;
};

/**
 * 同一条接触内的邻近合并：XY 距离 < `MergeDistance` 的交点并成一根柱 —— XY 取簇质心、朝向取成员平均、
 * Z 取 [min Bottom, max Top]。并查集的根恒为最小下标，输出按簇的最小成员下标排列 ⇒ 只由输入顺序决定。
 * 返回柱数。
 *
 * 🛑 **只喂一条接触的交点，跨接触绝不合并**（2026-09-16 晚用户裁决「数量上必须要对应」）。A 在下、B 摞在
 * A 上、C 从地面横穿两栋：C 手里 A–C 与 B–C 两组交点 XY 相同、Z 相接，仍是**两组**柱，各归各的出砖方。
 * 跨接触合并的毛病不只是数量：合并柱的归属取决于每栋房各自看得到哪些接触（C 看得到两条，A、B 各一条），
 * 三栋的 GUID 六种序里四种会少砌一截（`House.ContactStackedPostsPerPair` 钉着）。同一条接触的交点两端
 * 看到的是同一个对象，合并结果天然一致。
 *
 * 同一条接触的交点 Z 区间相同（两房的 [max 房底, min 檐口]），所以这里不分上下段；取并集只为将来逐点底高
 * （TG 取交点处地形高）进来时仍然成立。
 */
inline int32 MergePosts(const TArray<FPost>& In, float MergeDistance, TArray<FPost>& Out)
{
	Out.Reset();
	const int32 N = In.Num();
	if (N == 0) return 0;

	TArray<int32> Parent;
	Parent.SetNumUninitialized(N);
	for (int32 I = 0; I < N; ++I) Parent[I] = I;
	auto Find = [&Parent](int32 I) { while (Parent[I] != I) { Parent[I] = Parent[Parent[I]]; I = Parent[I]; } return I; };
	const double MergeSq = double(MergeDistance) * double(MergeDistance);
	for (int32 I = 0; I < N; ++I)
	{
		for (int32 J = I + 1; J < N; ++J)
		{
			if (FVector2D::DistSquared(In[I].Point, In[J].Point) >= MergeSq) continue;
			const int32 RI = Find(I), RJ = Find(J);
			if (RI != RJ) Parent[FMath::Max(RI, RJ)] = FMath::Min(RI, RJ);   // 根恒为最小下标 ⇒ 与合并先后无关
		}
	}

	for (int32 Root = 0; Root < N; ++Root)
	{
		if (Find(Root) != Root) continue;
		FPost Post = In[Root];
		FVector2D Centroid = FVector2D::ZeroVector, Outward = FVector2D::ZeroVector;
		int32 Count = 0;
		for (int32 I = Root; I < N; ++I)   // 簇成员的下标都不小于根
		{
			if (Find(I) != Root) continue;
			Centroid += In[I].Point;
			Outward += In[I].Outward;
			Post.BottomZ = FMath::Min(Post.BottomZ, In[I].BottomZ);
			Post.TopZ = FMath::Max(Post.TopZ, In[I].TopZ);
			++Count;
		}
		Post.Point = Centroid / double(Count);
		if (Outward.SizeSquared() > UE_DOUBLE_KINDA_SMALL_NUMBER) Post.Outward = Outward.GetSafeNormal();
		Out.Add(Post);
	}
	return Out.Num();
}

/**
 * 一条接触出几根柱、各在哪。`First` / `Second` 是求交那一刻两房的快照（与交点表同一份输入），`Corners` 是
 * 它们的交点表。TG 的两道过滤（近平行不立、贴顶点不立）→ 同接触内邻近合并 → 柱高门槛。
 * 纯函数：同一条接触、同一份参数，逐位相同。返回柱数。
 */
inline int32 BuildPosts(const FHouse& First, const FHouse& Second, const TArray<FCorner>& Corners,
	const FPostParams& Params, TArray<FPost>& Out)
{
	TArray<FPost> Candidates;
	Candidates.Reserve(Corners.Num());
	for (const FCorner& Corner : Corners)
	{
		if (CornerNearlyParallel(Corner, Params.ParallelDot)) continue;
		if (CornerNearVertex(First, Second, Corner.Point, Params.VertexClearance)) continue;
		FPost& Post = Candidates.AddDefaulted_GetRef();
		Post.Point = Corner.Point;
		Post.Outward = Corner.Outward;
		Post.BottomZ = Corner.BottomZ;
		Post.TopZ = Corner.TopZ;
	}
	MergePosts(Candidates, Params.MergeDistance, Out);
	Out.RemoveAll([&Params](const FPost& Post) { return Post.TopZ - Post.BottomZ < Params.MinHeight; });   // 保序
	return Out.Num();
}
}

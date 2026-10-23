#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"    // FBrickParams / FElement / EPathFamily
#include "CSHouseTrim.h"     // FBand / FRun / BuildBand —— 一条按洞裁过的横带
#include "CSHouseProfile.h"  // FCSWallOpening / CSHouse_GetEdge

/**
 * **砖层**（两层墙之 A，用户裁决 2026-09-06「挖洞策略改成 Tiny Glade 的真两层」）。
 *
 * 一句话：**砖层 = 一摞包边带**。`CSHouseTrim::BuildBand` 本来就是"沿四条边铺一行砖、
 * 按洞把行切断"，把它从两条（压顶 / 勒脚）叠到从房底一直摞到檐口，就是整面砖墙。
 * 复用而不是另写，是卷五 A11 那条纪律的直接后果 —— TG 侧同向且更硬：转角、墙裙、缝砖、
 * 雉堞、承重柱在那边**全是同一个 `brick` × 逐实例非均匀缩放**，138 个网格里砖只有一个。
 *
 * -----------------------------------------------------------------------------
 * 洞缘的四级里，这里做掉了①②
 * -----------------------------------------------------------------------------
 * 计划 D4「洞缘的四级处理」（读 `_wall_wall_brick_lod0` 的 VS/PS 之后订正的那份）：
 *
 *   ① **删实例**：整块落在洞内的砖不发 —— `CSHouseTrim::SplitEdge` 把边的 S 区间减去
 *      "与本行 Z 区间相交"的洞的 S 区间。判据带 Z，所以高窗不切勒脚、落地门不切压顶。
 *      TG 对位物 `utils::trim_rows` 走 `split_heightmap_by_threshold<RowTrimmerSink>` `[PDB]`。
 *   ② **水平贴合**：`AppendFlatRun` 的 `SolveRun` 解出一个铺装缩放，让整数块砖**正好填满**
 *      切出来的那一段 ⇒ 洞两侧那一列砖天然抵住洞缘。TG 对位物是逐实例非均匀缩放
 *      （`InstancedWallData` 里没有 mesh 索引字段，改尺寸只能改 transform）。
 *
 * ③（洞上缘那些砖的局部 z 按洞高曲线缩放，TG 的 `flags & 32`）与 ④（普通砖逐像素裁）
 * **都在 GPU 侧**，不在本文件。③ 只服务门拱：窗的洞缘由附属物自带的预制框盖住
 * （2026-09-06 裁决，与 TG 同构）。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 容量是硬上限，只截断不扩容
 * -----------------------------------------------------------------------------
 * `FBrickParams::MaxBricks` 与门框 / 接缝 / 角石 / 包边**共用同一份常驻容量**：砖层只是同一个
 * 组件里排在最后的那些行。撞到上限就停发，**绝不扩容** —— 扩容是一次阻塞刷新，落在用户恰好
 * 画到的那一笔上（零阻塞纪律）。所以动工前先用 `EstimateBricks` 对一次预算：一面 6×4 m、
 * 檐高 3 m 的房子按 26 cm 砖 / 20 cm 层高约 **1400 块**，比默认容量大一个量级。
 */
namespace CSHouseBrickWall
{
/**
 * 砖层的分层方案：从房底往上均匀摞，最后一层的**顶**对齐檐口。
 *
 * 层高取砖的**进深**（`AppendFlatRun` 里砖的进深轴朝上，见那条注释），所以一层的竖向占位
 * 就是 `FrameBrickDepth`。层数向上取整之后回算实际层高，避免檐口下留一条半层的缝。
 */
struct FCourses
{
	int32 Count = 0;
	/** 实际层高 cm（= WallHeight / Count，可能略小于请求值）。 */
	float Height = 0.0f;

	/** 第 `Index` 层的中心 Z（墙空间，房底为 0）。 */
	float CenterZ(int32 Index) const { return (float(Index) + 0.5f) * Height; }
	/** 判据用的半高。取实际层高的一半 ⇒ 相邻层的 Z 区间首尾相接、不重不漏。 */
	float HalfHeight() const { return Height * 0.5f; }
};

/**
 * 分层。`RequestedHeight <= 0` 或墙高 <= 0 ⇒ 空方案（`Count == 0`），调用方据此整层跳过。
 */
inline FCourses PlanCourses(float WallHeight, float RequestedHeight)
{
	FCourses Out;
	if (WallHeight <= 0.0f || RequestedHeight <= 0.0f) return Out;

	// 向上取整：宁可层高比请求的略矮，也不要在檐口下留半层。
	Out.Count = FMath::Max(FMath::CeilToInt(WallHeight / RequestedHeight), 1);
	Out.Height = WallHeight / float(Out.Count);
	return Out;
}

/**
 * 砖数的**上界**估计（不减洞）：四条边的周长 / 砖长 × 层数。
 *
 * 只用来对预算与写断言 —— 真实值一定不大于它（洞只会让砖变少），而且每段不足半块砖时
 * `AppendFlatRun` 直接不出，所以短段还会再少一点。
 */
inline int32 EstimateBricks(const FCSHouseFootprint& Footprint, float WallThickness, const FCourses& Courses, float BrickLength)
{
	if (Courses.Count <= 0) return 0;
	const float Length = FMath::Max(BrickLength, 1.0f);

	// 周长取**中线处的斜接区间**之和，与 `CSHouseTrim::BuildBand` 真正铺砖的区间同一个口径：
	// 凸角处它比外皮短、凹角处比外皮长，拿外皮全长当上界在凹角房子上会偏小。
	float Perimeter = 0.0f;
	for (int32 Edge = 0; Edge < Footprint.NumEdges(); ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, WallThickness);
		float S0 = 0.0f, S1 = 0.0f;
		F.SpanAtDepth(WallThickness * 0.5f, WallThickness, S0, S1);
		Perimeter += FMath::Max(S1 - S0, 0.0f);
	}
	// 每条边各自向上取整会更准，但这里要的是**上界**，一次取整就够，且与边数无关地偏大。
	return FMath::CeilToInt(Perimeter / Length) * Courses.Count;
}

/**
 * 铺整面砖墙：逐层调 `CSHouseTrim::BuildBand`，返回实际发出的砖数。
 *
 * `OutRuns` 是**每层复用**的暂存（`BuildBand` 内部会 `Reset`），传进来只为免掉逐层分配；
 * 想要哈希的话在回调里取，别指望它在返回后还留着最后一层以外的内容。
 *
 * 逐实例随机数从 **房子身份 + 家族盐 + 层号** 派生（`PathRandomBase` 的第三个参数换成层号），
 * 不从槽位派生 —— 砖层排在砖序最后，前面任何一段（开一扇门、来一个邻居）都会把它整体推走，
 * 跟着槽位走就会在改门的那一刻让整面墙的随机色重新洗一遍。
 */
template <typename FCourseSink>
inline int32 BuildWall(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	const FCourses& Courses, float Clearance, TArrayView<const FCSWallOpening> AllOpenings,
	uint32 Seed, const CSHouseFrame::FBrickParams& Params,
	TArray<CSHouseTrim::FRun>& OutRuns, TArray<CSHouseFrame::FElement>& InOutElements,
	FCourseSink&& OnCourse)
{
	int32 Total = 0;
	for (int32 Index = 0; Index < Courses.Count; ++Index)
	{
		CSHouseTrim::FBand Band;
		Band.CenterZ = Courses.CenterZ(Index);
		Band.HalfHeight = Courses.HalfHeight();

		// 家族盐固定、层号进随机数基 ⇒ 每层的随机序列不同，但只要层号不变就逐位可复现。
		const int32 Added = CSHouseTrim::BuildBand(World, Footprint, WallThickness, Band, Clearance,
			AllOpenings, Seed, CSHouseFrame::EPathFamily::BrickWall + uint32(Index), Params,
			OutRuns, InOutElements);
		Total += Added;
		OnCourse(Index, Band, Added, OutRuns);

		// 撞到容量上限之后再摞也只是空转（`AppendFlatRun` 会返回 0），提前收手省掉逐层扫洞表。
		if (Added == 0 && Index > 0 && Total >= Params.MaxBricks) break;
	}
	return Total;
}

// -----------------------------------------------------------------------------
// 山墙上的砖（2026-10-02）：墙顶之上逐层续摞，沿屋面线收成台阶
// -----------------------------------------------------------------------------

/** 一面山墙在砖层眼里的样子：几号边 + 上沿（沿边弧长 S, 比墙顶高出多少 cm；按 S 升序）。 */
struct FGable
{
	int32 EdgeIndex = INDEX_NONE;
	TConstArrayView<FVector2D> Profile;
};

/**
 * 山墙上沿线不低于 `Level` 的那一段（沿边弧长）—— **裸判据**，不吸附、不加宽。返回 false = 上沿处处低于它。
 * 上沿是坡面的下包络，沿边单峰，所以只有一段。`GableCourseSpan` 与露砖带的内端都拿它量。
 */
inline bool GableSpanAbove(TConstArrayView<FVector2D> Profile, double Level, double& OutS0, double& OutS1)
{
	bool bFound = false;
	for (int32 Index = 0; Index + 1 < Profile.Num(); ++Index)
	{
		const FVector2D& A = Profile[Index];
		const FVector2D& B = Profile[Index + 1];
		if (A.Y < Level && B.Y < Level) continue;
		// 这一小段里上沿线不低于 `Level` 的那一截。
		const double Slope = B.Y - A.Y;
		const double Cross = FMath::Abs(Slope) > UE_DOUBLE_SMALL_NUMBER ? A.X + (B.X - A.X) * (Level - A.Y) / Slope : A.X;
		const double Lo = A.Y >= Level ? A.X : Cross;
		const double Hi = B.Y >= Level ? B.X : Cross;
		if (!bFound) OutS0 = Lo;
		OutS1 = Hi;
		bFound = true;
	}
	return bFound;
}

/**
 * 山墙上高出墙顶 `MidRise` 的那一层砖铺在哪一段（沿边弧长）。返回 false = 这一层已经高过山墙。
 *
 * TG `utils::trim_rows`（14123EEF0）→ `split_heightmap_by_threshold`（141FE0750）：**一行只存在于「墙顶线 ≥ 这一行
 * 中线」的那一段**，行端落在坡线与行中线的交点上 —— 砖是整块的，于是山墙的斜边读成一级一级的台阶（阶梯山墙）。
 * 之后的两条后处理照抄（14123F249..14123F3B7）：
 *   · 行端吸附到 0.2 块砖（非山墙的墙是 0.5 块），顶在边两头的不动；
 *   · 不足一块砖的行段**加宽成一块砖**而不是丢弃 —— 山墙尖上那一两块就是这么留下来的。
 *     （TG 的加宽式子是「旧宽 / 4 + 0.75 块」，这里取整一块、居中，差别不到四分之一块。）
 * 山墙的上沿是坡面的下包络，沿边是单峰的，所以每层只有一段。
 */
inline bool GableCourseSpan(TConstArrayView<FVector2D> Profile, float EdgeLength, float MidRise, float BrickLength,
	float& OutS0, float& OutS1)
{
	double S0 = 0.0, S1 = 0.0;
	if (!GableSpanAbove(Profile, double(MidRise), S0, S1)) return false;

	const double Brick = FMath::Max(double(BrickLength), 1.0);
	const double Length = double(EdgeLength);
	const double Step = Brick * 0.2;
	if (S0 > UE_DOUBLE_KINDA_SMALL_NUMBER) S0 = FMath::RoundToDouble(S0 / Step) * Step;
	if (S1 < Length - UE_DOUBLE_KINDA_SMALL_NUMBER) S1 = FMath::RoundToDouble(S1 / Step) * Step;
	if (S1 - S0 <= Brick)
	{
		const double Centre = (S0 + S1) * 0.5;
		S0 = FMath::Clamp(Centre - Brick * 0.5, 0.0, FMath::Max(Length - Brick, 0.0));
		S1 = FMath::Min(S0 + Brick, Length);
	}
	OutS0 = float(FMath::Clamp(S0, 0.0, Length));
	OutS1 = float(FMath::Clamp(S1, 0.0, Length));
	return OutS1 - OutS0 >= float(Brick) * 0.5f;
}

/** 山墙要在墙顶之上续摞几层：最高那一点 / 层高，向上取整。 */
inline int32 GableCourseCount(TConstArrayView<FGable> Gables, const FCourses& Courses)
{
	if (Courses.Count <= 0 || Courses.Height <= 0.0f) return 0;
	double MaxRise = 0.0;
	for (const FGable& Gable : Gables)
	{
		for (const FVector2D& Point : Gable.Profile) MaxRise = FMath::Max(MaxRise, Point.Y);
	}
	return MaxRise > 0.0 ? FMath::CeilToInt(float(MaxRise) / Courses.Height) : 0;
}

/**
 * 山墙砖数的**上界**估计：每面山墙按「整条边 × 续摞的层数」算满（矩形盖住三角形），再各留一块给尖上被加宽的那一段。
 * 只用来对预算 —— 容量注册期一次付清、之后只截断不扩容，见文件头。
 */
inline int32 EstimateGableBricks(const FCSHouseFootprint& Footprint, float WallThickness, TConstArrayView<FGable> Gables,
	const FCourses& Courses, float BrickLength)
{
	const int32 Extra = GableCourseCount(Gables, Courses);
	if (Extra <= 0) return 0;
	const float Length = FMath::Max(BrickLength, 1.0f);
	int32 Total = 0;
	for (const FGable& Gable : Gables)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Gable.EdgeIndex, Footprint, WallThickness);
		Total += (FMath::CeilToInt(FMath::Max(F.Len, 0.0f) / Length) + 1) * Extra;
	}
	return Total;
}

/**
 * 铺山墙的砖：接着墙体那一摞的层号往上（同一个层高，砖缝与下面的墙对齐）。返回实际发出的砖数。
 *
 * 砖路与墙体的砖同一个口径（走墙厚正中、家族盐 + 层号）；山墙压在整条边上，不让转角 —— 墙顶之上
 * 已经没有相邻的那面墙来分转角方块了。洞不上山墙（窗的谓词卡在檐口以下），所以这里不看洞表。
 * `OnCourse` 的签名与 `BuildWall` 相同，层号从 `Courses.Count` 起。
 *
 * 两种口径，只差「灰泥盖到哪」：
 *  · `ExposedBand <= 0`（砖层）：每层每面山墙一整段，从台阶的一头铺到另一头。
 *  · `ExposedBand > 0`（**压顶砖**，没开砖层的房子）：只出露在灰泥外面的那一圈。TG 的灰泥栅格在山墙上只抹到
 *    「墙顶线 − 0.46 m」（`generate_plaster_mesh_and_entities` 1412279C8..1412279E5 的边缘距离），再往上是露着的砖；
 *    这里把同一件事落成几何：一层砖里**整块埋在灰泥下**的那一段（灰泥线不低于这层的顶）不出，两头各剩一截。
 *    露出来的那一截不足一块砖时补到一块（陡屋面上否则每层只剩一条缝）；两截碰头就还是一整段 —— 山墙尖上那几层。
 */
template <typename FCourseSink>
inline int32 BuildGableCourses(const FTransform& World, const FCSHouseFootprint& Footprint, float WallThickness,
	const FCourses& Courses, TConstArrayView<FGable> Gables, uint32 Seed, const CSHouseFrame::FBrickParams& Params,
	TArray<CSHouseTrim::FRun>& OutRuns, TArray<CSHouseFrame::FElement>& InOutElements, FCourseSink&& OnCourse,
	float ExposedBand = 0.0f, uint32 FamilySalt = CSHouseFrame::EPathFamily::BrickWall)
{
	const int32 Extra = GableCourseCount(Gables, Courses);
	const float WallHeight = Courses.Height * float(Courses.Count);
	const float Brick = FMath::Max(Params.Length, 1.0f);
	int32 Total = 0;
	for (int32 K = 0; K < Extra; ++K)
	{
		const int32 Index = Courses.Count + K;
		CSHouseTrim::FBand Band;
		Band.CenterZ = Courses.CenterZ(Index);
		Band.HalfHeight = Courses.HalfHeight();
		const float MidRise = Band.CenterZ - WallHeight;

		OutRuns.Reset();
		for (const FGable& Gable : Gables)
		{
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Gable.EdgeIndex, Footprint, WallThickness);
			float S0 = 0.0f, S1 = 0.0f;
			if (F.Len <= 0.0f || !GableCourseSpan(Gable.Profile, F.Len, MidRise, Brick, S0, S1)) continue;

			double Covered0 = 0.0, Covered1 = 0.0;
			if (ExposedBand > 0.0f
				&& GableSpanAbove(Gable.Profile, double(MidRise) + double(Band.HalfHeight) + double(ExposedBand), Covered0, Covered1))
			{
				const float LeftEnd = FMath::Max(float(Covered0), S0 + Brick);
				const float RightStart = FMath::Min(float(Covered1), S1 - Brick);
				if (LeftEnd < RightStart)
				{
					OutRuns.Add({ Gable.EdgeIndex, S0, LeftEnd, 0.0f, 0.0f });
					OutRuns.Add({ Gable.EdgeIndex, RightStart, S1, 0.0f, 0.0f });
					continue;
				}
			}
			OutRuns.Add({ Gable.EdgeIndex, S0, S1, 0.0f, 0.0f });
		}
		if (OutRuns.IsEmpty()) break;   // 这一层已经高过所有山墙：再往上也不会有了

		const int32 Added = CSHouseTrim::BuildTrimElements(World, Footprint, WallThickness, OutRuns, Band, Seed,
			FamilySalt + uint32(Index), Params, InOutElements);
		Total += Added;
		OnCourse(Index, Band, Added, OutRuns);
		if (Added == 0 && Total >= Params.MaxBricks) break;
	}
	return Total;
}
}

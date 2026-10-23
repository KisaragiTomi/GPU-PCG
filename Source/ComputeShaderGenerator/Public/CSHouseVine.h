#pragma once

#include "CoreMinimal.h"

// ⚠️ 这三条**显式写出来**，别指望 CoreMinimal 捎带：`TArrayView`（`PackTubePath` 的入参）、
// `FIntVector4`（`FTubePath::PointMeta` / `SegmentMeta`）、`FVector2f`（`FStrandPoint::WallSZ`）。
// 本插件是 unity 构建，漏 include 在全量构建里**一声不吭**、只在 Live Coding 或 `-SingleFile`
// 下炸 —— `CSGpuMeshComponent.cpp` 的 `MD_Surface` 已经栽过一次，那个坑藏了很久。
#include "Containers/ArrayView.h"
#include "Math/IntVector.h"
#include "Math/Vector2D.h"

#include "CSGroundShaperSteps.h"   // FPaletteBuffers / ReserveCapacity / ReleaseOnRenderThread

class UStaticMesh;
struct FCSGpuMeshCPUData;
struct FCSWallOpening;

/**
 * 墙面藤蔓（TinyGladeHouse D13）。
 *
 * 分工与门框砖同型：**CPU 只解"藤爬到哪里"**，实例行的组装在渲染线程的一张 RDG 图里跑完，
 * CPU 不算一个变换、不回读一个字节。区别只有一条 —— 门框走曲线求值，藤蔓的段是**直线段**，
 * 所以不需要样条，一趟 dispatch 打完。
 *
 * ⚠️ **不要拿 `VineScatter::Scatter*` 那三个入口来做这件事**：`CollectSurfaceTriangles`
 * （`CSVineScatter.cpp:38`）只遍历 `UStaticMeshComponent`，而房子挂在 `UCSMeshRenderComponent`
 * 上 ⇒ 它对房子**静默返回空三角集、不报错**。这条路是新写的「墙矩形 → 点集 → 填 ISM」。
 *
 * ⚠️ **随机数一律由记录自带**，绝不取 `InterlockedAdd` 的槽位：槽位由线程组完成顺序决定，
 * 同一份世界状态两次 dispatch 可以把同一段藤放进不同槽 ⇒ 材质拿它做颜色变化时"重建一次
 * 全场变色"，而且**不会有任何断言报红**（S1 已经栽过一次）。这里的身份是
 * (墙号, 藤号, 段号, 用户种子)，在 CPU 侧可精确复算，所以"随机只由身份决定"是可断言的。
 */
namespace CSHouseVine
{
/**
 * 采样点下面**根本没有地面**时写进 `FWallStrip::GroundGaps` 的空隙（cm）：按悬空处理，这段墙不长藤
 * （用户裁决 2026-09-21：收不到地面通知的藤直接清掉，留着只会是永远不刷新的陈旧结果）。
 *
 * ⚠️ 不能用 +∞ 或 `TNumericLimits<float>::Max()`：`SampleGroundGap` 在相邻两个采样间 `Lerp`，
 * ∞ − ∞ = NaN，而 `NaN > MaxGroundGap` 为假 ⇒ 藤反而长出来；空隙摘要还要过 `CSHouse_Q` 的 int32
 * 量化进哈希，float 上限会溢出。10 km 远超任何 `MaxGroundGap`，插值与量化都安全。
 */
inline constexpr float NoGroundGap = 1.0e6f;

/** 一面墙的外皮矩形。原点在墙脚外棱，U 沿墙、Up 向上、N 朝屋外，三者右手正交。 */
struct FWallStrip
{
	FVector Origin = FVector::ZeroVector;   // 世界，墙脚外棱的起点
	FVector U = FVector::ForwardVector;     // 世界单位，沿墙
	FVector Up = FVector::UpVector;         // 世界单位，向上
	FVector N = FVector::RightVector;       // 世界单位，墙面外法线
	float Length = 0.0f;
	float Height = 0.0f;                    // 墙顶高（= 墙高）。四坡屋顶下四面墙顶都是平的，藤爬到这条线就停
	int32 EdgeIndex = 0;                    // 与 FCSWallOpening::EdgeIndex 同一套编号

	/**
	 * 沿墙**均匀采样**的地面空隙（房底 Z − 该处地面高度，cm），首尾含端点。
	 *
	 * 用途只有一个：**悬空不长藤**（用户裁决 2026-09-06）。藤是从地里长出来爬上墙的，
	 * 房子底下悬空的那一段没有土，藤也就无从谈起 —— 这与承重柱恰好互补：柱子在
	 * `Gap > PillarMinGap` 处**出现**，藤在同一处**消失**。
	 *
	 * ⚠️ **空数组 = 不知道，按贴地处理**，不是按悬空处理。判据是纯函数的入参，而单测里
	 * 的墙没有地面可采 —— 缺省成"悬空"会让既有的十几条几何断言一次全红，而且红得毫无信息。
	 * 采样点数由调用方定；`ACSHouseActor::BuildVineStrips` 按 `VineGroundSampleSpacing` 取，
	 * 并且**总会填满**：脚下没有地面的点写 `NoGroundGap`（悬空），不留空数组。
	 */
	TArray<float> GroundGaps;

	/** 在弧长 S 处线性插值出空隙。空数组返回 0（= 贴地）。 */
	float SampleGroundGap(float S) const
	{
		// 局部名一律避开 N —— 那是本结构的**墙面法线**成员，而 /we4458 把遮蔽当错误。
		const int32 SampleCount = GroundGaps.Num();
		if (SampleCount == 0) return 0.0f;
		if (SampleCount == 1 || Length <= UE_KINDA_SMALL_NUMBER) return GroundGaps[0];
		const float T = FMath::Clamp(S / Length, 0.0f, 1.0f) * float(SampleCount - 1);
		const int32 Index = FMath::Clamp(int32(T), 0, SampleCount - 2);
		return FMath::Lerp(GroundGaps[Index], GroundGaps[Index + 1], T - float(Index));
	}

	/**
	 * 可选：**沿墙折线**（2026-09-22，样条墙 `ACSWallActor`）。非空（≥ 2 点）时这面墙不是平面矩形，而是
	 * 一条竖直的带：(S, Z) → 世界 = 墙脚折线在弧长 S 处的点 + Up·Z，外法线也按 S 在相邻两点之间插值。
	 * 弯墙的一面就是这样一条带 —— 拆成几十面半米长的短墙的话，藤每走半米就撞一次"墙角"。
	 *
	 * 空 = 平面矩形（房子的四面墙）：`Origin / U / N` 照旧生效，两处映射都保留原式，**逐位不变**。
	 * 三个数组等长：`PathBase[i]` 是墙脚那条线上的点（世界），`PathS[i]` 是它的累计弧长
	 * （首 0、末 = `Length`），`PathN[i]` 是该点的墙面外法线（单位、水平）。
	 */
	TArray<FVector> PathBase;
	TArray<float> PathS;
	TArray<FVector> PathN;

	/**
	 * 这面墙首尾相接（闭合样条墙的一面）：藤走过 S 的两端就绕回来，而不是去找隔壁那面墙。
	 * 只对带折线的墙生效（平面墙没有"绕回来"这回事）。
	 */
	bool bLoop = false;

	bool HasPath() const
	{
		return PathBase.Num() >= 2 && PathS.Num() == PathBase.Num() && PathN.Num() == PathBase.Num();
	}

	/** 闭合带把 S 绕回 [0, Length)；其余原样返回。 */
	float WrapS(float S) const
	{
		if (!bLoop || Length <= UE_KINDA_SMALL_NUMBER) return S;
		S = FMath::Fmod(S, Length);
		return S < 0.0f ? S + Length : S;
	}

	/** 弧长 S 落在折线的第几段、段内比例多少（S 先绕回、再夹进 [0, Length]）。 */
	void LocatePath(float S, int32& OutIndex, float& OutAlpha) const
	{
		const float Clamped = FMath::Clamp(WrapS(S), 0.0f, PathS.Last());
		// 二分：弯墙的一面有几十到几百个点，藤的每一步都要问一次。
		int32 Lo = 0, Hi = PathS.Num() - 1;
		while (Hi - Lo > 1)
		{
			const int32 Mid = (Lo + Hi) / 2;
			if (PathS[Mid] <= Clamped) Lo = Mid; else Hi = Mid;
		}
		const float Span = PathS[Hi] - PathS[Lo];
		OutIndex = Lo;
		OutAlpha = Span > UE_KINDA_SMALL_NUMBER ? FMath::Clamp((Clamped - PathS[Lo]) / Span, 0.0f, 1.0f) : 0.0f;
	}

	/** 折线墙：(S, Z) → 世界（含离墙 `StandOff`）。**平面墙不走这里** —— 两个调用点对平面墙保留原式。 */
	FVector PathToWorld(float S, float Z, float StandOff) const
	{
		int32 Index = 0;
		float Alpha = 0.0f;
		LocatePath(S, Index, Alpha);
		const FVector Base = FMath::Lerp(PathBase[Index], PathBase[Index + 1], double(Alpha));
		const FVector Normal = FMath::Lerp(PathN[Index], PathN[Index + 1], double(Alpha)).GetSafeNormal(UE_SMALL_NUMBER, PathN[Index]);
		return Base + Up * double(Z) + Normal * double(StandOff);
	}

	/** 折线墙在弧长 S 处的外法线（相邻两点之间插值）。 */
	FVector PathNormal(float S) const
	{
		int32 Index = 0;
		float Alpha = 0.0f;
		LocatePath(S, Index, Alpha);
		return FMath::Lerp(PathN[Index], PathN[Index + 1], double(Alpha)).GetSafeNormal(UE_SMALL_NUMBER, PathN[Index]);
	}
};

/** 藤蔓的形态参数。全部由 `ACSHouseActor` 的属性喂进来，这里不留默认策略。 */
struct FParams
{
	float StrandSpacing = 90.0f;      // 沿墙每隔多少 cm 起一根藤
	float SegmentLength = 26.0f;      // 一段的世界长度（= 一个 ivy_branch 实例）
	int32 MaxSegments = 22;           // 一根藤最多几段（也是高度上限的另一半保险）
	float Wander = 0.55f;             // 每段方向的随机扰动（弧度）
	float MaxLean = 1.15f;            // 相对"正上"的最大偏角（弧度），超过就夹住
	/**
	 * **一段之内倾角最多转多少**（弧度）。默认 0.70 ≈ 40.1°。
	 *
	 * 这是"相邻段夹角有上界"的**唯一**来源，而那条上界本身是 2026-09-12 修的一个画面缺陷：
	 * 旧代码在障碍处**直接改写**倾角（墙角 `Angle = -Angle`、洞里再 `Angle = 0`），
	 * 而倾角是相对竖直方向的**绝对**偏角 ⇒ 取反一次相邻段就折过 `2·|Angle|`，
	 * `MaxLean` 默认 1.15 时上界是 **2.30 rad ≈ 131.8°**，藤在洞缘和墙角上"折断"式急拐。
	 * 改法见 `BuildPlan` 里那段候选扫描：障碍处不再改写倾角，只在 `[±MaxTurn]` 的预算内**转向**。
	 *
	 * 反编译实证（`Docs/TinyGlade/Vine_ObstacleTurning.md`）：TG 的 `ivy_grower` 里
	 * **一次镜像/归零都没有** —— 方向来自 `IvyDirectionProposer::get_direction`，
	 * 它是**位置的连续函数**（两层 FastNoise 在 `pos / 2.8 m` 上取样），而步长只有 0.21 m，
	 * 一步只走过 7.5% 个波长，所以 TG 的小折角是结构性的、不是调出来的。
	 * 我们的游走是"累加绝对倾角"，只能靠这个闸把同一条性质补回来。
	 *
	 * ⚠️ **取值不要小于 `Wander`**：游走本身就是每段 ±`Wander` 的转向，预算更小会把
	 * **无障碍处**的形状也一起改掉（取得比 `Wander` 大时，无障碍段与旧代码逐位相同）。
	 * ⚠️ **取 0 = 藤笔直不转**，碰到任何障碍立刻收尾 —— 那是退化行为，不是"更稳"。
	 *
	 * 0.70 是标定出来的拐点，不是拍的。演示那面 6 m 墙上开三个落地拱（`VineRootEscapesHoles`
	 * 的场景）扫一遍：拱之间含外扩只剩 20 cm 的窄道，预算不够的藤在窄道里拐不过弯就收尾，
	 * **爬不到墙顶**：
	 *
	 *   MaxTurn    0.40   0.55   0.70   0.85   1.00   1.15   1.50
	 *   保留段数   0.380  0.495  0.598  0.596  0.626  0.616  0.606
	 *   平均高度   214    234    288    287    289    293    285     （无洞对照 ≈ 290）
	 *
	 * 0.70 是"带洞的墙与无洞的墙爬得一样高"的第一个值；再大只多出几个百分点的段数
	 * （那是折角换来的"皱"），而折角上界跟着涨。上界一侧的参照是 TG：它步长 0.21 m、
	 * 我们 0.26 m，按单位弧长折算 TG 单步的转向量在 23°–46°，40.1° 在这个区间里。
	 */
	float MaxTurn = 0.70f;
	float Bloat = 1.15f;              // 长度轴胀大系数 —— 与门框砖同一条理由：正缝会露出断口
	float Thickness = 9.0f;           // 截面直径（世界 cm）
	float StandOff = 3.0f;            // 沿墙面法线离墙多远，避免与墙面 z-fighting
	float HoleClearance = 12.0f;      // 墙洞外扩多少才算"撞上"
	float LeafChance = 0.72f;         // 每段长叶子的概率
	float LeafSize = 26.0f;           // 叶片的长度（世界 cm）
	float LeafSizeJitter = 0.35f;     // 叶片尺寸的对称抖动
	float FlowerChance = 0.10f;       // 段的开花概率（只在 FlowerFromFrac 以上的段上掷）
	float FlowerFromFrac = 0.45f;     // 从一根藤的第几成开始才可能开花
	float FlowerSize = 22.0f;         // 花簇的**宽度**（世界 cm）
	/**
	 * 花簇高/宽比。`ivy_flower` 实测包围盒 (76.04, 77.99, 38.40) ⇒ 38.40 / 77.0 ≈ 0.50。
	 * 这个数在这里而不是硬编在 kernel 里，是因为 `FPaletteBuffers::BlockSize` 把 xy 钉成
	 * 边长 = `FlowerSize` 的正方形截面，z 只留 1/网格长度 —— 记录必须自己把"想要多高"说出来，
	 * 否则花会被拉成 `FlowerSize` 高的柱子（枝是管、拉长正确；花是簇、拉长就穿帮）。
	 */
	float FlowerAspect = 0.50f;
	/**
	 * 转角跨墙的概率（TG 的 `check_for_wall_jump`）。藤走到墙角时：
	 * 掷中 ⇒ 拐上相邻那面墙继续长；没掷中 ⇒ 那个候选算不通过，退回 `MaxTurn` 的预算里
	 * 另找一个倾角，找不到就收尾（TG 在墙端同样是直接停 —— 见 `MaxTurn` 的注释）。
	 * 0 = 完全关掉（一根藤都不绕过转角）。
	 */
	float JumpChance = 0.5f;
	/**
	 * 藤脚允许的最大地面空隙（cm）。超过它那一根就不长 —— 房子整个悬空时四面墙都超阈，
	 * 于是**一根藤都没有**，正是用户要的行为；只有一角翘起时则只秃那一角。
	 *
	 * ⚠️ 取值应当**略大于** `ACSHouseActor::PillarMinGap`（默认 10）：两者共用同一个
	 * `Gap` 量，取成一样的话会出现"柱子已经冒出来了、藤还在长"或反过来的一线之差，
	 * 而那条线上的抖动只由地面采样噪声决定，看起来像 bug。
	 */
	/**
	 * 梢部收紧的**辐射长度**（cm）：从梢往回这么长的一段里，管径由主锥度平滑压到
	 * `TipTaperMin`。用户裁决 2026-09-06："收紧需要辐射一段距离"。
	 *
	 * ⚠️ 只压最后一两环的后果是"钝头上插了个小锥子" —— 转折太急，读起来像被截断后
	 * 又接了一节，而不是长出来的尖。取值与藤总长同量级的 1/5 上下比较自然。
	 */
	float TipTaperLength = 60.0f;

	/** 梢尖处相对主锥度的残留比例。**不能取 0** —— 整圈环顶点收到同一点会退化成
	 *  零面积三角形、法线变 NaN（与材质里 `VineThickenStart` 同一条）。 */
	float TipTaperMin = 0.06f;

	float MaxGroundGap = 25.0f;
	int32 Seed = 1;
};

/**
 * 一条实例记录 = 3 个 float4，与 `CSHouseVine.usf` 文件头的布局逐字对应。
 * 分开成结构体只是为了让 CPU 侧读得懂，上传时按 `FVector4f` 平铺。
 */
struct FRecord
{
	FVector3f WorldPos = FVector3f::ZeroVector;
	float LengthScale = 1.0f;
	FVector3f Dir = FVector3f(0.0f, 0.0f, 1.0f);
	float Random01 = 0.0f;
	FVector3f Normal = FVector3f(0.0f, 1.0f, 0.0f);
	float SizeScale = 1.0f;

	/**
	 * 生长动画的两个量 → per-instance custom data（2026-09-06 裁决 5）。
	 * `SpawnTime` = 这根藤首次出现时的 GameTime（秒）；`ArcLength` = 该实例在藤上的弧长（cm）。
	 *
	 * ⚠️ **它们不参与几何**，也不该进任何形状哈希 —— `SpawnTime` 只是相位。
	 * ⚠️ 弧长沿**未细分**的折线累加，而管子的弧长是在细分后的折线上量的，两者差几个百分点。
	 * 叶子因此可能比枝的前沿早/晚一点点冒出来，用 `VineLeafLag` 补即可，别去追平 ——
	 * 追平要么把细分搬进 BuildPlan，要么让叶子等管子建完，都不值当。
	 */
	float SpawnTime = 0.0f;
	float ArcLength = 0.0f;

	/**
	 * 这条记录属于 `FPlan::Strands` 的第几根。**`SpawnTime` 的回填靠它**。
	 *
	 * 为什么不把 SpawnTime 做成 `BuildPlan` 的入参：它的键是 `FStrand::RootKey`，
	 * 而 RootKey 要规划跑完才知道 —— 做成入参就得先跑一遍规划去问，等于每次重求值
	 * 都白跑一遍纯函数。带上下标，回填只是一趟 O(n) 扫描。
	 */
	int32 StrandIndex = INDEX_NONE;
};

/**
 * 折线上的一个点。**存墙面参数坐标**，不存世界 —— 与 TG 的 `WallCoord` 同构：
 * 在这个坐标里做的一切（游走、避洞、细分）都天然贴墙，映射到世界是最后一步。
 */
struct FStrandPoint
{
	FVector2f WallSZ = FVector2f::ZeroVector;   // (沿墙弧长 S, 墙面高度 Z)
	int32 EdgeIndex = 0;                        // 该点所在的墙。跨墙以后会变，细分要据此断开
};

/**
 * 一根藤 = 一条**不分叉**的折线（2026-09-06 裁决 2，与 TG 的 `Ivy`/`VecDeque<IvyPoint>` 同构）。
 *
 * 它是枝（管子）的**唯一形状来源**；叶与花仍从 `FRecord` 走实例路，两者由 `RootKey` 关联 ——
 * 同一根藤的枝与叶必须共用一个 `SpawnTime`，否则生长动画会各长各的。
 */
struct FStrand
{
	TArray<FStrandPoint> Points;

	/**
	 * 与 `Points` 等长的**累计世界弧长**（cm），`Arc[0] = 0`。
	 *
	 * 它是生长动画的**唯一一把尺**：枝（管子）、叶、花、以及"改了门之后从哪儿接着长"
	 * 那个变化点，全部以它为单位。管子的顶点虽然经过细分，生长通道也是从这把尺**插值**
	 * 出来的、而不是重新累加细分后的长度 —— 后者比它长几个百分点，两把尺混用的症状是
	 * "叶子比枝的前沿早/晚一点点冒出来"，以及续长时接缝对不齐。
	 */
	TArray<float> Arc;
	int32 RootEdgeIndex = 0;   // 起点那面墙。身份哈希用它，跨墙不改（见 IdentityHash 的注释）
	int32 StrandIndex = 0;     // 这面墙上的第几根
	uint32 RootKey = 0;        // = IdentityHash(RootEdgeIndex, StrandIndex, -1, 7u, Seed)，SpawnTime 的键
};

/** 一次规划的产物：三个调色板（0 = 枝、1 = 叶、2 = 花）各自的记录，外加枝的折线。 */
struct FPlan
{
	/**
	 * ⚠️ **`Branch` 已不是交付物**（2026-09-06 裁决 1）：枝改成扫掠管子，形状由 `Strands` 给。
	 * 这个数组留着只为两件事 —— 幂等哈希的诚实来源、以及单测里"段数/避洞"那一族断言。
	 * 别再拿它去填实例。
	 */
	TArray<FRecord> Branch;
	TArray<FRecord> Leaf;
	TArray<FRecord> Flower;

	/** 枝的折线，逐根藤。喂给 `PackTubePath`。 */
	TArray<FStrand> Strands;

	bool IsEmpty() const { return Branch.IsEmpty() && Leaf.IsEmpty() && Flower.IsEmpty() && Strands.IsEmpty(); }
	void Reset() { Branch.Reset(); Leaf.Reset(); Flower.Reset(); Strands.Reset(); }
};

/** `PackTubePath` 的产物：vinegenerator 的 **CPU 折线通路**（`bUseGPULines = false`）那四条缓冲，外加生长通道。 */
struct FTubePath
{
	/** xyz = 世界位置，**w = 逐点半径缩放** —— Pass C 的环半径 = `10 * CircleScale * w`。锥度就靠它。 */
	TArray<FVector4f> Points;
	/** 逐点轴向覆盖。我们不用，但**必须显式清零**：`BuildRawVoxelVineFrame` 的回退判据是
	 *  `dot(Axis, Axis) > 1e-8`，池子里的旧内容会让它误以为有轴可用。 */
	TArray<FVector4f> Axes;
	/** `int4(prev, next, 线起点下标, 线点数)`。⚠️ **两套口径都在用**：Pass C 读 `.x/.y`，
	 *  `VineFrameCommon.ush::GetLinePointIndex` 读 `.z/.w`。只填一半的症状是切线在端点处乱。 */
	TArray<FIntVector4> PointMeta;
	/** `int4(点A, 点B, 0, 0)`，一段一条。 */
	TArray<FIntVector4> SegmentMeta;
	/** 生长动画通道：`(SpawnTime 秒, 该点的世界弧长 cm)` → Pass C 写进 UV1。 */
	TArray<FVector2f> Growth;

	int32 NumPoints() const { return Points.Num(); }
	bool IsEmpty() const { return Points.IsEmpty() || SegmentMeta.IsEmpty(); }
	void Reset() { Points.Reset(); Axes.Reset(); PointMeta.Reset(); SegmentMeta.Reset(); Growth.Reset(); }
};

/**
 * 管子的环半径系数。**两个调用点必须用同一个值**：`PackTubePath` 按 `w = 想要的半径 / (10 * CircleScale)` 反解逐点缩放，
 * Pass C 再按 `半径 = 10 * CircleScale * w` 还原。取值本身是任意的（两边约掉了），所以它是个常量而不是属性 —— 暴露出去
 * 只会让人以为调它能改粗细（那是 `Thickness`），而真正的后果是两边取不同值时管子整体差一个常数倍、且两边各自都自洽。
 * 房子与样条墙共用这一个（2026-09-22 之前各抄一份 `CSHouseVine_TubeCircleScale` / `CSWallActor_TubeCircleScale`）。
 */
inline constexpr float TubeCircleScale = 0.2f;

/**
 * 一根藤上一轮的生长记录（`ResolveSpawnTimes` 的记忆，键 = `FStrand::RootKey`）。
 *
 * ⚠️ 存的是**墙面参数坐标**而不是世界坐标：拖房子 / 拖墙时世界坐标整体在动，逐点比较会判成"处处都变了"，
 * 于是每拖一帧整根藤重新长一遍。
 */
struct FStrandHistory
{
	float SpawnTime = 0.0f;
	TArray<FVector2f> PointsSZ;
	TArray<int32> Edges;
};

/**
 * 生长相位（**纯函数**，房子与样条墙共用 —— 2026-09-22 之前两边各抄一份）：
 *   · 全新的一根（第一次生成、或跨过藤位间距新增的那根）：`bGrowOnLoad ? Now : 长成`；
 *   · 与上一轮逐点相同（墙面参数坐标 + 所在墙，容差 0.5 cm —— 更严会把浮点噪声判成"变了"，每次重求值都重新长一遍）：沿用；
 *   · 改过的：前沿还没长到第一个不同的点 ⇒ 相位不动；越过了 ⇒ 把前沿拉回变化点，从那里接着长。
 * `History` **整表替换**成这一轮的（只加不删会随编辑次数无界增长，而且泄漏得毫无症状）。
 * `OutSpawnTimes` 与 `Plan.Strands` 一一对应。
 */
COMPUTESHADERGENERATOR_API void ResolveSpawnTimes(TMap<uint32, FStrandHistory>& History, const FPlan& Plan,
	float Now, float GrowSpeed, bool bGrowOnLoad, TArray<float>& OutSpawnTimes);

/** 调色板序号。**与 `ACSHouseActor::VineGpuBuffers` / `Pack` 的下标是同一套**，别各写各的。 */
enum EPalette : int32 { Palette_Branch = 0, Palette_Leaf = 1, Palette_Flower = 2, Palette_Num = 3 };

/**
 * 身份哈希：(墙号, 藤号, 段号, 佐料, 用户种子) → uint。
 *
 * ⚠️ **这就是"不许用槽位"那条纪律的执行面**。身份里刻意**不含位置** —— 拖房子时墙面在动，
 * 位置派生的种子会让整片藤在拖动过程里不停重掷；身份只在"这面墙上第几根藤的第几段"
 * 真的变化时才变。TG 的对位物是 `_rocky_terrain_stairs_stairs.cs:504` 的
 * `uint((Tx+Ty+Tz)*100)`（位置派生），这里比它更稳。
 */
COMPUTESHADERGENERATOR_API uint32 IdentityHash(int32 EdgeIndex, int32 Strand, int32 Segment, uint32 Salt, int32 Seed);

/** uint → [0,1)，与 `CSGpuInstancedMesh.usf:120-126` 的收尾段同一份数学。 */
COMPUTESHADERGENERATOR_API float Hash01(uint32 H);

/**
 * 纯函数：墙矩形 + 洞表 + 参数 → 记录。**不碰任何 GPU 资源，可以在纯 CPU 单测里跑。**
 *
 * 这条"能在没有 world 的用例里跑"的性质不是附赠品：藤蔓最容易错的两件事
 * （避不避墙洞、身份稳不稳）都只能在这一层断言，等到 GPU 那一侧就只剩一个实例计数了。
 */
COMPUTESHADERGENERATOR_API void BuildPlan(const TArray<FWallStrip>& Strips, const TArray<FCSWallOpening>& Openings,
	const FParams& Params, FPlan& OutPlan);

/**
 * 墙面点 (S, Z) 落在这面墙的某个洞里（洞形外扩 `Clearance`）？
 *
 * ⚠️ **判据必须与材质那份 clip 场同源，不能拿洞的外接矩形凑合**（第一档就是矩形）：
 * 拱的**肩角**（拱脚线以上、半圆之外的那两块）在几何上是**实心墙**，材质也确实把它画出来。
 * 这里走 `CSHouse_ComputeClipField` + `CSHouse_ClipKeeps` —— 与逐像素 discard **同一条曲线**，
 * 所以"藤让开的地方"与"墙真的被切掉的地方"逐点一致（裁决三的一条可判定形式）。
 *
 * ⚠️ **别指望它能治"拱附近变稀"**（实测，2026-08-31）：换成解析场之后拱肩上也只多出 0~2 段，
 * 因为 `HoleClearance`（12 cm，下界由门框砖的 `FrameBrickDepth / 2` 钉死）几乎把肩上
 * "藤够得着"的那条自由带吃光了。变稀的主因是**落地拱把整条墙脚吃掉**、第一档"藤脚在洞里
 * 就整根不长"，解药是 `CSHouseVine_EscapeRoot` 的侧移。这条注释是给下一个想"再放宽一点"
 * 的人看的：放宽外扩不行（会穿进门框砖），放宽形状已经做完了，剩下的空间在**起点**上。
 *
 * 外扩的做法是把**洞本身**胀大 `Clearance` 再算场（不是把场的结果外扩）：拱胀大以后拱脚线
 * `Z1 − HalfWidth` 逐位不变、半圆半径 +Clearance，正好是那条曲线的等距外偏移。
 *
 * ⚠️ 两条**有意**与材质保持一致的偏差，别当成漏洞去"修"：
 *   · `Skew`（楼梯斜洞顶）不参与 —— `FCSOpeningClipField` 里根本没有这个量，材质也不读；
 *   · Arch 的场在拱脚线以下**无下界**，所以这里另外补一条 `Z ≥ Z0 − Clearance`
 *     （窗台以下那一截是实心盒，藤该长在上面）。
 */
COMPUTESHADERGENERATOR_API bool IsInsideOpening(const TArray<FCSWallOpening>& Openings, int32 EdgeIndex,
	float S, float Z, float Clearance);

/**
 * 折线 → vinegenerator 的 CPU 折线通路缓冲（`FVineBuildInput` 的 `PathPoints/Axes/Meta/SegmentMeta`）。
 *
 * **纯函数，不碰 GPU**，与 `BuildPlan` 一样能在没有 world 的用例里跑。
 *
 * 三件事在这里发生：
 *  ① **细分**：同一面墙上的连续点做 Catmull-Rom（`Subdivide` 段/原段）。
 *     ⚠️ 细分在**世界空间**做是安全的，不必回到 `(S,Z)` —— Catmull-Rom 是**仿射组合**（权重和为 1），
 *     同一面墙上的点共面，仿射组合必然还在那个平面里，所以不会被平滑拽离墙面。
 *     **但跨墙那一段不能这么做**：两端不共面，四个控制点混着算会把线甩到墙里去。跨墙段走线性插值
 *     （几何上就是把转角切一刀，正是想要的圆角）。
 *  ② **锥度**：写进 `Points[i].w`。Pass C 的环半径 = `10 * CircleScale * w`，所以这里给的是
 *     "想要的半径 / (10 * CircleScale)"，与 `FParams::Thickness` 对齐。
 *  ③ **弧长**：沿**细分后**的世界折线累加，写进 `Growth[i].y`（cm）。
 *     ⚠️ 别拿 shader 里的 `CurveV` 当它用 —— 那个除过平均环周长，单位是"周长"，细枝会长得快。
 *
 * `StrandSpawnTimes` 与 `Plan.Strands` 一一对应；给空数组则 `Growth[i].x` 全填 0。
 * `CircleScale` 必须与递交给 vinegenerator 的那个值一致，否则管子粗细对不上。
 */
COMPUTESHADERGENERATOR_API void PackTubePath(const TArray<FWallStrip>& Strips, const FPlan& Plan,
	const FParams& Params, int32 Subdivide, float CircleScale,
	TArrayView<const float> StrandSpawnTimes, FTubePath& OutPath);

/**
 * 把基础网格读成一份自带**法线与 UV** 的快照，并把它的长度轴换到 +Z。
 *
 * ⚠️ **这个函数存在的唯一理由是 `ivy_branch` 的顶点流只有 `Vertex_Position`** ——
 * 没有法线也没有 UV（对照文档 §7.4 实测）。直接丢给任何常规材质都是一块死黑，
 * 而 `UCSGpuInstancedMeshComponent::SetBaseMesh` 会原样把那份空的切线基搬进快照。
 * TG 自己的做法是在 VS 里现搭截面基；本项目不写自定义顶点工厂，所以改成**导入期补齐**：
 *   · 法线 = 相邻面法线累加（`ivy_branch` 是 12 顶点 / 6 三角的开口三棱管，顶点不共享 ⇒
 *     累加出来正好是逐面平法线，这对棱柱是正确答案，不是近似）；
 *   · UV = 绕长度轴的柱面展开（U = 极角，V = 沿长度归一），贴图因此不会被非均匀缩放拉成条。
 * 源网格自带非退化的法线/UV 时（`ivy_leaf` / `ivy_flower` 就是）原样沿用，只做换轴。
 *
 * `LengthAxis` 是**源网格**的长度轴（0=X 1=Y 2=Z），换轴后一律变成 +Z ——
 * `CSHouseVine.usf` 的基约定只认 +Z，两种网格共用一支 kernel。
 *
 * 材质也照抄 LOD0：`TriangleMaterialSlots` = 每个三角所在段的材质槽，`Materials` = 资产的材质槽表
 * （2026-09-15）。实例组件没设整体覆盖材质时靠这两样逐段画资产自己的材质（摆件 / 裙边摆件就是这么画的）。
 *
 * 返回是否读出了可用的几何（读不出时 Out 被清空，调用方应当据此报错而不是画一块灰）。
 */
COMPUTESHADERGENERATOR_API bool BuildBaseMesh(UStaticMesh* Source, int32 LengthAxis, FCSGpuMeshCPUData& Out);

/**
 * 录一趟打包 pass：三个调色板各上传一次记录 → 各一个 dispatch 写满 packed 行 + counter。
 *
 * **录完直接返回，不阻塞**（Palettes 按值捕获，`TRefCountPtr` 拷贝即加引用）。前置条件是
 * `CSShaperSteps::ReserveCapacity` 已经把容量备好 —— 这里只用现有容量，一个字节都不分配，
 * 所以交互期（拖房子、画路）走不到任何阻塞路径。容量不够时**截断**而不是扩容：
 * 少画几段藤远好过在拖动的某一帧里付一次设备同步。
 *
 * 返回是否真的录了 pass。
 */
COMPUTESHADERGENERATOR_API bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent);
}

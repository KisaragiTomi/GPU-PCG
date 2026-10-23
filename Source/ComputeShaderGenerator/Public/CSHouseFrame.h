#pragma once

#include "CoreMinimal.h"
#include "CSArchCurve.ush"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers —— 实例源的容器，与石阶/藤/摆件同一份
#include "CSHouseProfile.h"        // FCSWallOpening / FCSOpeningClipField
#include "Templates/TypeHash.h"   // HashCombine —— 柱类砖路的逐实例随机数基

/**
 * 门框砖的 **100% GPU 解析推导**（TinyGladeHouse D6；2026-08-30「裁决一」选乙）。
 *
 * -----------------------------------------------------------------------------
 * 为什么不再走「样条 + 逐砖记录」那条路（那条路**已删**，本节是它的墓志铭）
 * -----------------------------------------------------------------------------
 * 旧路（`ACSHouseActor::BuildFramePlan`，连同 `csh.FrameLegacy` 开关一起删于裁决一第二步）
 * 把洞缘折线**等距重采样**成控制点、喂给
 * `RDG_SmoothSpline` 求值成均匀三次 B 样条，再按弧长占比给每块砖排一条 `(alpha, scale)` 记录。
 * 它带着三样代价，而这三样在**洞缘本来就是解析曲线**这个事实面前全都是白付的：
 *
 *   ① **B 样条会抹掉「门樘 → 拱 → 门樘」的两个 90° 折角**。均匀三次 B 样条在控制点处取
 *      `(P₋₁ + 4P₀ + P₁)/6`，等距 h 的直角上偏移恰好是 `h·√2/6` —— 26 cm 砖、h≈13 cm 时约 3 cm。
 *      砖因此在起拱线附近整体离开设计曲线（早期没做重采样时更狠，实测设计拱脚 Z=160、样条只到 135）。
 *   ② **容量随砖数走**。记录是逐砖的 ⇒ 那条路的 `CSShaperSteps::EnsureCapacity`（同样已删）
 *      必须能扩容 ⇒ 扩容那一趟是阻塞刷新，落在"用户恰好画到这一笔"上。解析推导的砖数只是
 *      一个标量，容量恒定，交互期永远走不到扩容。
 *   ③ **槽位来自 `InterlockedAdd`** ⇒ 逐实例随机数由线程组完成顺序决定 ⇒ 同一份世界状态
 *      两次散布可以给同一块砖不同的随机数（S1 已经栽过一次，症状是"画一笔路全场变色"且
 *      不会有任何断言报红）。这里线程 i 恒写第 i 槽，随机数因此是砖身份的纯函数。
 *
 * ⇒ **连带作废**：`CSShaperSteps::ResampleUniform` 那条「喂控制点前必须先重采样成等弧长」的
 *   契约**只对 B 样条那条路成立**。两条 B 样条路（门框旧路、地形石阶旧路）都已删除，
 *   所以这条契约现在**没有任何产线消费者** —— `ResampleUniform` 本身留着，因为等价性单测
 *   `House.FrameAnalyticMatchesLegacy` 的 CPU 镜像要逐字复现旧路的组路方式。
 *
 * -----------------------------------------------------------------------------
 * 定位与定向的依据：洞缘就是 `FCSOpeningClipField` 那条解析曲线
 * -----------------------------------------------------------------------------
 * 洞在画面上的边缘**不是**任何折线 —— 墙板是实心盒，洞由材质按 `FCSOpeningClipField` 逐像素
 * discard 切出来（裁决三：避免所有真几何洞）。所以「砖该骑在哪条线上」这个问题只有一个正确
 * 答案：**clip 场那条解析曲线本身**。四个参数一一对上：
 *
 *   `CenterS`      → 圆弧的横向圆心（也是竖直段的对称中心）
 *   `1/InvHalfWidth` → 拱/圆的半径；矩形洞的半宽
 *   `RefZ`         → 拱 = 起拱线；圆 = 圆心；矩形 = 洞中
 *   `1/InvScaleZ`  → 矩形洞的半高 / 拱的竖向半径（圆洞等于半宽）
 *
 * 定向同样是闭式的：路径切向 `T` 在墙空间 (S, Z) 里直接可导，**面内朝外法线 = T 逆时针转 90°**
 * （`(-T.z, T.s)`）。旧 kernel 要靠「减去曲线中心求径向 + 投影掉平面法线 + 判号翻转」三步才
 * 得到同一个方向，那三步在拱心退化或曲线不绕中心时会翻错号；解析式没有可翻错的号。
 *
 * -----------------------------------------------------------------------------
 * 砖路的形状：竖直段 + 中段 + 竖直段
 * -----------------------------------------------------------------------------
 * 一条砖路（`FPath`）最多三段，按弧长首尾相接，三段都可缺席：
 *
 *   ① 左竖直段：S = `LeftS`，Z 从 `BaseZ` 升到 `TopZ`，切向 (0, +1)
 *   ② 中段：圆弧（θ 从 0 扫到 `MidSweep`，S = Cs − R·cosθ、Z = TopZ + Rise·sinθ）
 *          或水平段（矩形洞的平顶，长 `FlatLen`，切向 (+1, 0)）
 *   ③ 右竖直段：S = `RightS`，Z 从 `TopZ` 降回 `BaseZ`，切向 (0, −1)
 *
 * 洞型与段的对应：
 *   · 门（落地的拱）= 左樘 + 半圆（Sweep = π，横半径 = 半宽，竖半径 = Rise，中心在起拱线）+ 右樘
 *   · 圆   = 只有中段，Sweep = 2π（一圈砖，起点在最左）
 *   · 窗**不走这里**：洞缘由附属物自带的预制框盖住，`BuildEdgeElements` 对窗一块砖都不出。
 *     窗周围的砖头补全（曾经沿洞底补的「窗台底边」第四段）已于 2026-09-10 按用户裁决整条删除。
 *
 * 半圆两端的切向与门樘**连续**（θ=0 恰是 (0,1)、θ=π 恰是 (0,−1)）；矩形洞平顶两端则是
 * **正交**折角：砖在折角处转 90°，靠 `FrameBrickBloat` 的负缝互相咬住。
 *
 * -----------------------------------------------------------------------------
 * 拱间墩：相邻两拱在墩上只砌**一列**砖（2026-08-30 观感缺陷）
 * -----------------------------------------------------------------------------
 * 旧路让每个拱各出一条门樘砖脚：跨度 = `PierWidth`、砖的面内进深 = `FrameBrickDepth`，
 * 两条砖脚各骑在跨度两端、各伸进跨度一半 ⇒ **恰好在墩正中对接**，于是墩上有一条从地面
 * 一直贯到起拱线的竖缝（两列砖共面相抵，法线与深度都断开，出图 `pier_after_pier.png` 可见）。
 * 而 TG 本体实拍（计划 D6「Tiny Glade 本体实拍」一节）里墩就是**一列**：拱圈的砖不间断地
 * 砌到地面，中间那根墩是相邻两拱的砖脚**合在一起**，看不出"这里是这一拱、那里是那一拱"。
 *
 * 解析推导正好有条件照实拍做：砖位置既然是从拱参数直接算的，墩就不必是"两条砖脚的产物"，
 * 可以**升格成一条自己的砖路** ——
 *
 *   · 判到墩（`CSHouse_StylePierAfter` / `Before`，双阈迟回已在 `ResolvePierSpans` 里算好）时，
 *     **两侧的拱都不出那一侧的门樘**；
 *   · 由左边那一拱**一次性**产出一条竖直砖路，S = 墩心、Z 从地面到墩顶（两拱起拱线的较低者，
 *     与 `CSHouse_PierSpanBetween` 同口径）。
 *
 * 覆盖条件**一字未变**：一列砖覆盖 `[墩心 − 进深/2, 墩心 + 进深/2]`，要盖住整条跨度就要求
 * `PierWidth ≤ FrameBrickDepth` —— 与旧路"两条砖脚各伸进一半"给出的条件完全相同
 * （演示回归里那条 `the shipped pier is narrow enough...` 因此继续成立、继续有意义）。
 * 换来的是墩上**没有共面对接**：拱圈第一块砖骑在起拱线上、横向占 `[S₁−d/2, S₁+d/2]`，
 * 墩顶那块砖横向占 `[S₁, S₁+PierWidth]`，两者横向搭接半块、竖向靠 `FrameBrickBloat` 互相穿插
 * ⇒ 接缝是**错缝咬合**而不是一条直缝。
 *
 * -----------------------------------------------------------------------------
 * 裁决六（终局要存成 StaticMesh）的对齐
 * -----------------------------------------------------------------------------
 * 输出仍然是 `UCSGpuInstancedMeshComponent` 那套 packed 实例行（5 个 float4 / 实例），
 * **没有引入任何新的逐实例语义** —— 逐实例随机数从"槽位哈希"改成"砖序号哈希"，值域与用法
 * 都不变，且改成了确定的。所以门框砖原有的 `SaveToStaticMesh` 出口一字不用改。
 */
namespace CSHouseFrame
{
/** 中段的形态。`None` = 只有竖直段（墩就是这一种）。 */
enum class EMidKind : uint8
{
	None,
	/** 圆弧：θ ∈ [0, MidSweep]，S = CenterS − R·cosθ，Z = TopZ + Rise·sinθ。 */
	Arc,
	/** 水平直段（矩形洞的平顶）：从 LeftS 走到 RightS，长 FlatLen。 */
	Flat,
};

/**
 * 一条砖路的**解析**描述。所有量都在墙空间 (S = 沿边弧长, Z = 从房底起算的高度)。
 *
 * 这个结构体就是 GPU 侧逐路常量的 CPU 原本 —— `CSHouseFrame.usf` 里的八行 float4 与它逐字对应，
 * 两处必须一起改。
 */
struct FPath
{
	/** 竖直段的底（洞底；门恒 0，墩恒 0）。 */
	float BaseZ = 0.0f;
	/** 竖直段的顶（拱 = 起拱线 / 矩形 = 洞顶 / 墩 = 墩顶）。 */
	float TopZ = 0.0f;
	/** 左竖直段所在的 S。 */
	float LeftS = 0.0f;
	/** 右竖直段所在的 S。 */
	float RightS = 0.0f;
	/** 圆弧的横向圆心（= clip 场的 CenterS）。 */
	float CenterS = 0.0f;
	/** 圆弧半径（= clip 场的 1/InvHalfWidth）。 */
	float Radius = 0.0f;
	/** Vertical radius from the opening profile; zero keeps circular callers compatible. */
	float ArcRise = 0.0f;
	/** 圆弧扫过的角度：拱 = π，圆 = 2π。 */
	float MidSweep = 0.0f;
	/** 水平中段的长度（`EMidKind::Flat` 时才有意义）。 */
	float FlatLen = 0.0f;

	EMidKind MidKind = EMidKind::None;
	bool bLeftJamb = false;
	bool bRightJamb = false;

	float JambLen() const { return FMath::Max(TopZ - BaseZ, 0.0f); }
	float LeftLen() const { return bLeftJamb ? JambLen() : 0.0f; }
	float RightLen() const { return bRightJamb ? JambLen() : 0.0f; }
	float MidLen() const
	{
		switch (MidKind)
		{
		case EMidKind::Arc:  return CSArch_Length(FMath::Max(Radius, 0.0f), ArcRise, MidSweep);
		case EMidKind::Flat: return FMath::Max(FlatLen, 0.0f);
		default:             return 0.0f;
		}
	}
	float TotalLen() const { return LeftLen() + MidLen() + RightLen(); }
};

/**
 * 弧长 → 墙空间位置与**单位**切向。**`CSHouseFrame.usf` 的 `FrameEvalPath` 是它的逐字翻译**，
 * 两处写不一样就会出现"单测说砖在这儿、画面上砖在那儿"，而两边各自都自洽 —— 与
 * `CSHouse_ClipKeeps` / 材质那对判据同一条纪律。
 */
inline void EvalPath(const FPath& Path, float Arc, FVector2f& OutSZ, FVector2f& OutTangent)
{
	const float L0 = Path.LeftLen();
	const float L1 = Path.MidLen();
	const float L2 = Path.RightLen();
	const float Total = L0 + L1 + L2;
	const float A = FMath::Clamp(Arc, 0.0f, FMath::Max(Total, 0.0f));

	// 分支顺序即段序；每一段都带"我存在吗"与"我后面还有段吗"的前置判断，退化路径
	// （只有中段的圆洞 / 只有左段的墩）因此不会掉进后面那段的公式里去算出一个看着合理的错位置。
	if (L0 > 0.0f && (A <= L0 || (L1 <= 0.0f && L2 <= 0.0f)))
	{
		OutSZ = FVector2f(Path.LeftS, Path.BaseZ + FMath::Min(A, L0));
		OutTangent = FVector2f(0.0f, 1.0f);
		return;
	}
	if (L1 > 0.0f && (A <= L0 + L1 || L2 <= 0.0f))
	{
		const float T = FMath::Min(A - L0, L1);
		if (Path.MidKind == EMidKind::Arc)
		{
			const float Rise = CSArch_Rise(Path.Radius, Path.ArcRise);
			const float Theta = CSArch_Angle(Path.Radius, Rise, Path.MidSweep, T);
			const float C = FMath::Cos(Theta), S = FMath::Sin(Theta);
			OutSZ = FVector2f(Path.CenterS - Path.Radius * C, Path.TopZ + Rise * S);
			OutTangent = FVector2f(Path.Radius * S, Rise * C).GetSafeNormal();   // d/dθ 的单位化：θ=0 → (0,1)，θ=π → (0,−1)
		}
		else
		{
			OutSZ = FVector2f(Path.LeftS + T, Path.TopZ);
			OutTangent = FVector2f(1.0f, 0.0f);
		}
		return;
	}
	if (L2 > 0.0f)
	{
		OutSZ = FVector2f(Path.RightS, Path.TopZ - FMath::Min(A - L0 - L1, L2));
		OutTangent = FVector2f(0.0f, -1.0f);
		return;
	}
	// 三段全空（调用方本该早退）：给一个不会让 kernel 除零的确定值。
	OutSZ = FVector2f(Path.LeftS, Path.BaseZ);
	OutTangent = FVector2f(0.0f, 1.0f);
}

/**
 * 单条目 palette 下 `ACSSplineBlockActor::SolveBlockLayout` 的**同式重写**：给定弧长，
 * 求砖数与整体铺装缩放。
 *
 * ⚠️ **必须逐句照抄那边的贪心累加，不能改写成 `ceil((Total + Gap)/(Len + Gap))`**：那个闭式
 * 在实数上等价，在 float 上不等价 —— 贪心是"反复 `Sum += Gap; Sum += Len`"，累加误差与一次
 * 除法的舍入方向不同，弧长恰好落在跳变点附近时两者会差**一整块砖**。砖数是几何的一部分
 * （进 desc 哈希、进回归断言），差一块就是画面差一块。单测 `House.FrameRunMatchesLayout`
 * 拿密扫把这条钉住。
 *
 * 返回砖数；`OutScale` 是铺装缩放（砖长与砖缝都乘它）。
 */
inline int32 SolveRun(float TotalLength, float BrickLength, float Gap, float& OutScale)
{
	OutScale = 0.0f;
	if (TotalLength <= UE_KINDA_SMALL_NUMBER) return 0;
	const float Len = BrickLength;
	if (Len <= UE_KINDA_SMALL_NUMBER) return 0;
	const float SafeGap = FMath::Max(Gap, 0.0f);

	// ① 贪心填到首次越界：`Sum >= TotalLength` 才停，所以最后一块必然是越界块。
	constexpr int32 MaxBlocks = 65536;
	float Sum = 0.0f;       // 含最后一块（及其前置 gap）
	float PrevSum = 0.0f;   // 不含最后一块
	int32 Count = 0;
	while (Sum < TotalLength)
	{
		if (Count >= MaxBlocks) return 0;
		PrevSum = Sum;
		if (Count > 0) Sum += SafeGap;
		Sum += Len;
		++Count;
	}

	// ② 候选 A：保留最后一块整体压缩；候选 B：去掉最后一块整体拉伸。|log(scale)| 小者胜。
	const float ScaleA = TotalLength / Sum;
	if (Count <= 1 || PrevSum <= UE_KINDA_SMALL_NUMBER)
	{
		OutScale = ScaleA;
		return Count;
	}
	const float ScaleB = TotalLength / PrevSum;
	if (FMath::Abs(FMath::Loge(ScaleA)) <= FMath::Abs(FMath::Loge(ScaleB)))
	{
		OutScale = ScaleA;
		return Count;
	}
	OutScale = ScaleB;
	return Count - 1;
}

/**
 * 墙面在世界里的框架：墙空间 (S, Z) → 世界 = `Origin + AxisU·S + AxisV·Z`；`AxisN` 是墙外法线。
 *
 * ⚠️ `AxisN` **不进 GPU 记录**：kernel 用 `cross(面内朝外, −切向)` 现算，那样得到的基一定是
 * 右手系。传一个外部法线进去再拿它当基的 +Z，一旦它与另外两轴的定向不自洽，实例就成了镜像
 * —— 法线朝里、光照当场坏掉，而几何位置看着完全正常。这里留着它只是给 CPU 侧读者/单测点明
 * 这面墙朝哪边。
 */
struct FWallFrame
{
	FVector3f Origin = FVector3f::ZeroVector;
	FVector3f AxisU = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f AxisV = FVector3f(0.0f, 0.0f, 1.0f);
	FVector3f AxisN = FVector3f(0.0f, -1.0f, 0.0f);
};

/** 砖的排布参数（尺寸不在这里 —— 那是 `FPaletteBuffers::BlockSize` 的事）。 */
struct FBrickParams
{
	/** 普通砖路的位置抖动 cm。角石发射器将它作为 TG 基准长边抖动，按层高折算。 */
	float Jitter = 0.0f;
	/** **分层随机化的抖动幅度（cm）**，见 `FElement::SplitJitter`。默认 0 = 等分。 */
	float SplitJitter = 0.0f;

	float Length = 26.0f;
	float Gap = 0.0f;
	/** 全部砖路加起来的硬上限 = 常驻容量。**超了就截断，绝不扩容**（零阻塞纪律）。 */
	int32 MaxBricks = 512;
	/**
	 * 拱间墩两端的柱头 / 柱础：横截面放大倍数与每块的高度 cm。`CapitalHeight <= 0` 或
	 * `CapitalScale <= 1` = 不出（墩退回一条均匀的竖直砖路）。只有门框那条路填它；
	 * 接缝柱与角石沿用默认 0 —— 角石通高到檐口，柱头放在檐下没有意义。
	 */
	float CapitalScale = 1.5f;
	float CapitalHeight = 0.0f;
};

/** 一条砖路 + 它的世界框架 + 它在全局砖序里占的区间。上传给 GPU 的就是它。 */
struct FElement
{
	FPath Path;
	FWallFrame Frame;
	/** 全局砖序号的起点（各元素前缀和）—— 线程 i 靠它找到自己属于哪条路。 */
	int32 BrickBegin = 0;
	int32 BrickCount = 0;
	/** 相邻砖中心距 = (Length + Gap)·Scale；第一块砖的弧长 = HalfLen = Length·Scale/2。 */
	float Pitch = 0.0f;
	float HalfLen = 0.0f;
	/** 铺装缩放：砖的长度轴额外乘它（kernel 里的 `BlockScale.y *= LengthScale`）。 */
	float LayoutScale = 1.0f;
	/**
	 * 本条路第一块砖的**逐实例随机数序号**（kernel 取 `FrameInstanceRandom(RandomBase + 路内序号)`）。
	 *
	 * 门框砖填 `BrickBegin` —— 与"随机数 = 砖的全局槽位"逐位等价，那条路一个字都没变。
	 * 存在的理由是 **D7 接缝**：同一条缝由两栋房各画一份重叠的砖（裁决二：零归属），而槽位是
	 * "这栋房自己的第几块砖"，两边必然不同。让随机数跟着槽位走，就会在将来谁给砖材质接上
	 * `PerInstanceRandom` 色差的那一天，让两份重叠的砖以不同颜色互相闪 —— 而所有几何断言全绿。
	 * 接缝那条因此从**接缝身份**派生（`CSHouseSeam::SeamSeed`）。
	 */
	uint32 RandomBase = 0;
	/**
	 * 横截面缩放：进深轴（kernel 的 +X）与墙法线轴（+Z）同乘，长度轴不动。柱头 / 柱础 > 1，
	 * 其余恒 1。走 `R5.w`（那一格原本空着）。2026-09-04 为拱间墩的小石柱加的 —— 实拍
	 * `img/tiny-glade-ref-twin-arch-pier.jpg`：墩是柱础 + 柱身 + **更宽的柱头**，两道拱圈收在柱头上。
	 */
	float CrossScale = 1.0f;
	/**
	 * **世界 Z 低于它的砖不画**（2026-09-05 用户裁决："转角门处角柱要剔除"）。
	 * `<= 0` = 不剔（默认，所有既有砖路逐位不变）。
	 *
	 * 实现走**逐实例随机数的负值哨兵**：kernel 给要剔的砖写 `-1` 到 packed 行的 `.w`。随机数本身恒 ≥ 0，
	 * 负值因此是一个不会与任何真实取值相撞的哨兵；而被剔掉的砖也不需要色差。
	 * 2026-09-15 起哨兵是 `UCSGpuInstancedMeshComponent` 的组件级契约（剔除 pass 跳过、Nanite 路写 HIDDEN、
	 * 烘焙不带），与门框砖画什么材质无关 —— `FrameMaterial` 留空改画资产材质之后，
	 * `M_TinyGladeBrick` 的 `OpacityMask = saturate(PerInstanceRandom + 1)` 已经不在画面上了。
	 *
	 * ⚠️ **为什么不走 `PerInstanceCustomData`**：那条通道在 `FCSGpuInstancedMeshVertexFactory`
	 * 里是关的（`NumCustomDataFloats = 0`），接通它要给 packed 实例加一个 float 流，
	 * 而那 5 个 float4 一份的布局是 `CSHouseFrame` / `CSHouseVine` / `CSHouseDecor` /
	 * `CSHouseTile` 与剔除 pass **共用**的契约 —— 为一根角柱去动它不划算。
	 */
	float CullBelowZ = 0.0f;

	/**
	 * 端头砖的**剪切量** cm（≥ 0，洞缘四级的③）：第一块 / 最后一块砖的顶边朝洞多伸出这么多，
	 * 底边不动。只有砖层的横带会填它，门框 / 接缝 / 角石 / 包边恒 0。
	 *
	 * ⚠️ **一条路只有一块砖、两端又都挨着洞时只应用较大的那个**：剪切是平行四边形，一个自由度
	 * 救不了上下两头都要动的梯形。硬凑两头会把砖推进洞里，而画面上只是"洞缘多了一小块"——不报错。
	 */
	float ShearAtS0 = 0.0f;
	float ShearAtS1 = 0.0f;
	/** 逐砖变化幅度 cm。普通砖路沿 AxisX 平移；QuoinScale > 0 时只改变角石长边。
	 * TG 的 0.16 是长边变化，并非角平分线平移；见 A7 的 2026-09-12 二进制订正。 */
	float Jitter = 0.0f;
	/**
	 * **分层随机化的抖动幅度（cm）**，沿路的弧长方向。`<= 0` = 等分（默认，既有砖路逐位不变）。
	 * 走 `R7.x`；`R7.y` 同时带上本条路的总弧长（kernel 要拿它把 cm 归一化）。
	 *
	 * 照抄 TG 的 `utils::random_splits`（VA 0x140C90350 → 真身 0x140C90050）：
	 *
	 *     assert(splits >= 2);                  // panic 消息原文 "assertion failed: splits >= 2"
	 *     if (splits < 3) return {0, 1};
	 *     step = 1 / (splits - 1);
	 *     amp  = min(jitter, 0.495 * step);     // ← .rdata 常量 0.495
	 *     out[0] = 0;
	 *     for (i = 1 .. splits-2) out[i] = i*step + (rng.f32() - 0.5) * amp;
	 *     out[splits-1] = 1;
	 *
	 * ⚠️ **那个 0.495 是关键**（不是 0.5）：抖幅被夹到**不到半个间距**，于是分点永远保序、
	 * 相邻间距最小 `0.505·step > 0` —— **不可能退化成零厚度的层**。0.495 是刻意留的余量。
	 * 这也比"标称层高 ×(1±抖动)"稳：那种写法会改总长度，这个只动分界、总长恒定。
	 */
	float SplitJitter = 0.0f;
	/** 角石比例：FrameBrickLength / TG 标称层高 69 cm。0 = 普通砖路；走 R7.z。 */
	float QuoinScale = 0.0f;
	/** 角石基础网格 X/Z 尺寸的倒数；R6.yz 在角石分支中取代端头剪切。 */
	FVector2f QuoinMeshInvSize = FVector2f::ZeroVector;
	/**
	 * 角石所在角的 `cos(转角 / 2)`，走 R7.w（原先保留为 0 的那一格）。只在 `QuoinScale > 0` 时上传；
	 * `<= 0` 按直角（1/√2）处理，所以没填它的旧路径画面不变。见 `CSHouseQuoinLayout::PlaceQuoin`。
	 */
	float QuoinHalfTurnCos = 0.0f;
};

/**
 * 柱类砖路的**逐实例随机数基** = 身份 × 家族 × 序号。
 *
 * ⚠️ **不能写成 `Seed ^ (Index * K)`**（本函数取代的就是那个写法）：`Index == 0` 时它是
 * **恒等映射**，于是任意两个家族只要 seed 撞上，各自的 0 号柱就共享同一个随机数。
 * 症状要等到有人给砖材质接上 `PerInstanceRandom` 色差那天才显形，而在那之前
 * 砖数 / 位置 / 三角形数所有几何断言全绿 —— 单测
 * `House.QuoinSharesTheColumnEmitter` 钉住这条（它就是这么抓到的）。
 *
 * `FamilySalt` 区分家族（接缝柱 / 角石 / 将来的墙裙与垛口）；同族内靠 `Index` 区分。
 */
inline uint32 PathRandomBase(uint32 Seed, uint32 FamilySalt, int32 Index)
{
	return HashCombine(HashCombine(Seed, FamilySalt), uint32(Index) * 2654435761u + 0x9E3779B9u);
}

/** 家族盐。加新家族时在这里登记，别就地写字面量 —— 撞盐与撞 seed 是同一种静默故障。 */
namespace EPathFamily
{
	static constexpr uint32 Seam     = 0x5345414Du;   // 'SEAM' —— 两房交汇处的接缝柱
	static constexpr uint32 Quoin    = 0x51554F4Eu;   // 'QUON' —— 房子自身四角的角石
	static constexpr uint32 TrimTop  = 0x54524D54u;   // 'TRMT' —— 墙顶包边（压顶石）
	static constexpr uint32 TrimBase = 0x54524D42u;   // 'TRMB' —— 墙脚包边（勒脚石）
	static constexpr uint32 CornerPier = 0x43505252u; // 'CPRR' —— 转角配成墩时角上的柱础/柱身/柱头
	/**
	 * 'BWAL' —— 砖层（两层墙之 A，2026-09-06）。⚠️ **它后面 65536 个值都归它**：
	 * `CSHouseBrickWall::BuildWall` 拿 `BrickWall + 层号` 当盐，好让每层的随机序列不同。
	 * 再加新家族时从 `0x42574C00u` 之后另起，别插进这段里。
	 */
	static constexpr uint32 BrickWall = 0x4257414Cu;
	/** 'WCOP' / 'WMRL' —— 样条墙（`CSWall::BuildTopElements`）墙顶的压顶砖与垛口。 */
	static constexpr uint32 WallCoping = 0x57434F50u;
	static constexpr uint32 WallMerlon = 0x574D524Cu;
	/**
	 * 'GCOP' —— 阶梯山墙的压顶砖（`CSHouseBrickWall::BuildGableCourses` 的露砖带那一支，2026-10-02）。
	 * 与砖层同一条规矩：拿 `GableCoping + 层号` 当盐，**它后面 65536 个值都归它**。
	 */
	static constexpr uint32 GableCoping = 0x47434F50u;
}

/**
 * **一根竖直砖柱**的通用发射器：柱心 + 朝外方向 + 柱底/柱顶 → 追加一条砖路，返回本次的砖数。
 *
 * 三家共用它（合卷卷五 A11）：D7 接缝交点柱（`CSHouseSeam::BuildCornerElements`）、
 * D7 墙自身转角的角石（`CSHouseQuoin::BuildQuoinElements`）、以及将来的墙裙 / 垛口。
 * TG 侧的实证是同向的：转角、墙裙、缝砖、雉堞、承重柱在那边**全是同一个 `brick` × 逐实例
 * 非均匀缩放**，连独立的网格资产都没有（合卷卷五 §1、§3.3）。
 *
 * ⚠️ **`AxisU` 取的是朝外方向的反向，这不是笔误。** 路径竖直 ⇒ 切向恒 `(0,1)` ⇒ kernel 里
 * 的面内朝外 `OutwardSZ = (−1, 0)` ⇒ `AxisX = −AxisU`，而 `AxisX` 正是砖的**进深轴**
 * （`FrameBrickDepth`）。写成 `AxisU = Outward` 的症状是砖整根朝里、进深轴插进房间，
 * 而位置完全正确 —— 位置断言一条都不会红。
 *
 * ⚠️ **只截断，绝不扩容**：`InOutCursor` 撞到 `Params.MaxBricks` 就返回 0。容量是注册期
 * 一次付清的常量（`ReserveCapacity`），扩容是阻塞刷新，落在用户恰好画到的那一笔上。
 */
/** 竖直柱路的墙框架：原点在角点、世界高度吃进 Origin.z，U 指向房内（−Outward），V 竖直。三种柱类砖路共用。 */
inline FWallFrame ColumnFrame(const FVector2D& Point, const FVector2D& Outward, float BottomZ)
{
	FWallFrame Frame;
	Frame.Origin = FVector3f(float(Point.X), float(Point.Y), BottomZ);
	Frame.AxisU = FVector3f(float(-Outward.X), float(-Outward.Y), 0.0f).GetSafeNormal();
	Frame.AxisV = FVector3f(0.0f, 0.0f, 1.0f);
	Frame.AxisN = FVector3f(float(-Outward.Y), float(Outward.X), 0.0f).GetSafeNormal();
	return Frame;
}

inline int32 AppendColumn(const FVector2D& Point, const FVector2D& Outward, float BottomZ, float TopZ,
	uint32 RandomBase, const FBrickParams& Params, TArray<FElement>& InOutElements, int32& InOutCursor)
{
	const float Length = FMath::Max(Params.Length, 1.0f);
	const int32 MaxBricks = FMath::Max(Params.MaxBricks, 0);
	// 半块砖都摆不下就不出 —— 与门框那条下限同一个口径。
	if (TopZ - BottomZ < Length * 0.5f) return 0;

	FPath Path;
	Path.BaseZ = 0.0f;                      // 世界高度全部吃进 Frame.Origin，路自己从 0 起算
	Path.TopZ = TopZ - BottomZ;
	Path.LeftS = Path.RightS = Path.CenterS = 0.0f;
	Path.MidKind = EMidKind::None;
	Path.bLeftJamb = true;

	float Scale = 0.0f;
	int32 Count = SolveRun(Path.TotalLen(), Length, Params.Gap, Scale);
	if (Count <= 0 || Scale <= 0.0f) return 0;
	Count = FMath::Min(Count, MaxBricks - InOutCursor);
	if (Count <= 0) return 0;

	FElement Element;
	Element.Path = Path;
	Element.Frame = ColumnFrame(Point, Outward, BottomZ);
	Element.BrickBegin = InOutCursor;
	Element.BrickCount = Count;
	Element.Pitch = (Length + FMath::Max(Params.Gap, 0.0f)) * Scale;
	Element.HalfLen = Length * Scale * 0.5f;
	Element.LayoutScale = Scale;
	Element.RandomBase = RandomBase;
	Element.Jitter = FMath::Max(Params.Jitter, 0.0f);
	Element.SplitJitter = FMath::Max(Params.SplitJitter, 0.0f);
	InOutElements.Add(Element);
	InOutCursor += Count;
	return Count;
}

/**
 * 一块横截面放大的**单砖**（柱头 / 柱础），立在竖直柱路的框架上，恰好铺满世界高度 [Z0, Z1]。
 * `BuildEdgeElements` 里 `EmitSlab` 的柱路版本：那个只能沿墙边摆（吃边框架 + S），这个吃角点 + 朝向。
 * 追加写，返回砖数（0 或 1）。
 */
inline int32 AppendSlab(const FVector2D& Point, const FVector2D& Outward, float Z0, float Z1,
	uint32 RandomBase, const FBrickParams& Params, TArray<FElement>& InOutElements, int32& InOutCursor)
{
	const float H = Z1 - Z0;
	if (H <= UE_KINDA_SMALL_NUMBER || InOutCursor >= FMath::Max(Params.MaxBricks, 0)) return 0;

	FElement Element;
	Element.Path.BaseZ = 0.0f;
	Element.Path.TopZ = H;
	Element.Path.LeftS = Element.Path.RightS = Element.Path.CenterS = 0.0f;
	Element.Path.MidKind = EMidKind::None;
	Element.Path.bLeftJamb = true;
	Element.Frame = ColumnFrame(Point, Outward, Z0);
	Element.BrickBegin = InOutCursor;
	Element.BrickCount = 1;
	Element.Pitch = 0.0f;
	Element.HalfLen = H * 0.5f;
	Element.LayoutScale = H / FMath::Max(Params.Length, 1.0f);
	Element.RandomBase = RandomBase;
	Element.CrossScale = Params.CapitalScale;
	InOutElements.Add(Element);
	++InOutCursor;
	return 1;
}

/**
 * 转角墩：柱础 + 柱身 + 柱头，与 `BuildEdgeElements` 给拱廊的墩砌的三段**同一个口径**
 * （柱头 / 柱础夹在墩高的 40% 内；`CapitalHeight <= 0` 或 `CapitalScale <= 1` 退回一条均匀柱路）。
 * 区别只在框架：拱廊的墩沿墙边摆，这根立在角点上、沿角平分线。追加写，返回砖数。
 *
 * 三段各取一个随机数基（`Index * 3 + 段序`），与 `EmitSlab` 那边"每段一个元素"的结构一致。
 */
inline int32 AppendCornerPier(const FVector2D& Point, const FVector2D& Outward, float BottomZ, float TopZ,
	uint32 Seed, int32 Index, const FBrickParams& Params, TArray<FElement>& InOutElements, int32& InOutCursor)
{
	const float H = TopZ - BottomZ;
	if (H <= UE_KINDA_SMALL_NUMBER) return 0;
	auto Base = [&](int32 Part) { return PathRandomBase(Seed, EPathFamily::CornerPier, Index * 3 + Part); };

	const float Cap = FMath::Clamp(Params.CapitalHeight, 0.0f, H * 0.4f);
	if (Cap <= UE_KINDA_SMALL_NUMBER || Params.CapitalScale <= 1.0f)
	{
		return AppendColumn(Point, Outward, BottomZ, TopZ, Base(1), Params, InOutElements, InOutCursor);
	}
	int32 Added = 0;
	Added += AppendSlab(Point, Outward, BottomZ, BottomZ + Cap, Base(0), Params, InOutElements, InOutCursor);
	Added += AppendColumn(Point, Outward, BottomZ + Cap, TopZ - Cap, Base(1), Params, InOutElements, InOutCursor);
	Added += AppendSlab(Point, Outward, TopZ - Cap, TopZ, Base(2), Params, InOutElements, InOutCursor);
	return Added;
}

/**
 * **一段水平砖路**的通用发射器：沿墙边从 `S0` 铺到 `S1`、课程中心高 `BandZ`。
 *
 * 包边石（墙顶压顶 / 墙脚勒脚，`CSHouseTrim`）走它。与 `AppendColumn` 的唯一区别是路的形状：
 * 那边是竖直段（`EMidKind::None` + 只出左樘），这边是平顶段（`EMidKind::Flat`），两者都只有
 * 一段、都由同一个 `SolveRun` 定砖数与铺装缩放。
 *
 * ⚠️ **砖的三轴在这里换了个位置，不是笔误。** kernel 对切向 `(1,0)` 算出面内朝外
 * `OutwardSZ = (0,1)` ⇒ `AxisX`（砖的 `FrameBrickDepth`）指向**竖直向上**、`AxisY`（长度轴）
 * 沿边、`AxisZ`（`FrameBrickThickness`，默认退化成墙厚）横跨墙厚。也就是说包边这一课的
 * **高度是 `FrameBrickDepth`**、进深是墙厚 —— 和拱缘砖共用同一组尺寸，因为它们共用一个组件、
 * 一份 `BlockSize`（TG 全库也只有一块 `brick`）。想让包边比拱缘更厚只能连拱缘一起改。
 */
inline int32 AppendFlatRun(const FWallFrame& Frame, float S0, float S1, float BandZ,
	uint32 RandomBase, const FBrickParams& Params, TArray<FElement>& InOutElements, int32& InOutCursor,
	float ShearAtS0 = 0.0f, float ShearAtS1 = 0.0f)
{
	const float Length = FMath::Max(Params.Length, 1.0f);
	const int32 MaxBricks = FMath::Max(Params.MaxBricks, 0);
	const float Span = S1 - S0;
	// 半块砖都摆不下就不出 —— 与门框、角石同一个下限。门洞切出来的碎段靠它自然消失。
	if (Span < Length * 0.5f) return 0;

	FPath Path;
	Path.MidKind = EMidKind::Flat;
	Path.FlatLen = Span;
	Path.LeftS = S0;
	Path.RightS = S1;
	// 平顶段取 `TopZ` 作高度（`EvalPath` 的 Flat 分支：`OutSZ = (LeftS + T, TopZ)`）。
	// `BaseZ` 写成同一个值只是为了让读到这个结构体的人不必猜哪个字段在起作用。
	Path.TopZ = BandZ;
	Path.BaseZ = BandZ;
	// 三个 bool 全 false ⇒ `TotalLen() == FlatLen`，路上只有中段。

	float Scale = 0.0f;
	int32 Count = SolveRun(Path.TotalLen(), Length, Params.Gap, Scale);
	if (Count <= 0 || Scale <= 0.0f) return 0;
	Count = FMath::Min(Count, MaxBricks - InOutCursor);
	if (Count <= 0) return 0;

	FElement Element;
	Element.Path = Path;
	Element.Frame = Frame;
	Element.BrickBegin = InOutCursor;
	Element.BrickCount = Count;
	Element.Pitch = (Length + FMath::Max(Params.Gap, 0.0f)) * Scale;
	Element.HalfLen = Length * Scale * 0.5f;
	Element.LayoutScale = Scale;
	Element.RandomBase = RandomBase;
	Element.ShearAtS0 = FMath::Max(ShearAtS0, 0.0f);
	Element.ShearAtS1 = FMath::Max(ShearAtS1, 0.0f);
	InOutElements.Add(Element);
	InOutCursor += Count;
	return Count;
}

/** 已排定的砖数 = 全部元素的 `BrickBegin + BrickCount` 上确界。柱类发射器靠它接着数。 */
inline int32 NextBrickSlot(const TArray<FElement>& Elements)
{
	int32 Cursor = 0;
	for (const FElement& E : Elements) Cursor = FMath::Max(Cursor, E.BrickBegin + E.BrickCount);
	return Cursor;
}

/**
 * 一个洞 → 它的解析砖路。`PierBefore/After` 来自 `FCSWallOpening::StyleFlags`：
 * 那一侧是拱间墩 ⇒ 这一拱**不出**那一侧的门樘（墩由左邻那一拱单独出一条竖直砖路）。
 *
 * 返回 false = 这个洞不产砖（退化尺寸）。
 */
COMPUTESHADERGENERATOR_API bool MakeOpeningPath(const FCSWallOpening& Opening, FPath& OutPath);

/** 拱间墩那条独立砖路：S = 墩心，Z 从地面到墩顶。`CSHouse_PierSpanBetween` 给跨度与墩顶。 */
COMPUTESHADERGENERATOR_API bool MakePierPath(const FCSWallOpening& Left, const FCSWallOpening& Right, FPath& OutPath);

/**
 * 一面墙上的洞集合 → 砖路元素表（**纯函数**，单测直接吃它）。
 *
 * `EdgeOpenings` 必须是**同一条边、按 CenterS 升序**的连续片段 —— `ACSHouseActor::ComputeDoors`
 * 末尾那次排序已经保证了这一点，`ResolvePierSpans` 打的墩样式位也建立在同一个顺序上。
 *
 * 追加进 `InOutElements`，返回本次追加的砖数（已按 `Params.MaxBricks` 截断）。
 */
COMPUTESHADERGENERATOR_API int32 BuildEdgeElements(const FWallFrame& Frame, TArrayView<const FCSWallOpening> EdgeOpenings,
	const FBrickParams& Params, TArray<FElement>& InOutElements);

/**
 * 录一趟散布：**一线程一砖**，没有样条、没有逐砖记录、没有原子累加。
 *
 * 与已删的 `CSShaperSteps::Scatter` 的区别不只是"少了一步" —— 那边每条曲线都要先跑一次
 * `RDG_SmoothSpline`（一个 dispatch）再跑一次散布（又一个 dispatch），且实例槽位靠
 * `InterlockedAdd` 抢；这里**整栋房子只有一个 dispatch**，槽位恒等于砖序号。
 *
 * 录完 pass 直接返回，不阻塞：`Palettes` 按值捕获（`TRefCountPtr` 拷贝即加引用）。
 * 前置条件是容量已由 `ReserveCapacity` 备好 —— 这里**只用现有容量，绝不分配**。
 */
COMPUTESHADERGENERATOR_API bool Scatter(const TArray<FElement>& Elements, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent);
}

#pragma once

#include "CoreMinimal.h"
#include "CSHouseProfile.h"   // FCSHouseFootprint / CSHouse_GetEdge —— 屋面长在 footprint 上，边在哪只有一个真源
#include "CSHouseRoof.generated.h"

/**
 * 屋面的共享求值器（Plan_House.md D4「屋面：抽一个共享求值器，脊向要显式」）。
 *
 * 为什么必须共享：Tiny Glade 的屋面是被瓦、梁、尖顶、雪、老虎窗**共同引用的单一求值器**
 * （逆向报告 §3.3【确凿】：支撑梁沿 circle_normal 按 roof_profile 平移并施加与瓦片**完全
 * 相同**的屋面凹陷噪声；雪 mesh 加与瓦片一致的抖动噪声保证贴合）。不共享就会脱开 —— 屋面
 * 方程一旦散在各自的生成函数里，铺瓦/铺梁/落窗谓词就会各写一份，彼此差一点点就穿帮。
 *
 * -----------------------------------------------------------------------------
 * 2026-08-31：双坡 + 山墙 → **四坡（hip）**，屋面本体交给瓦片
 * -----------------------------------------------------------------------------
 * 实拍俯视（用户提供）：TG 的屋顶是**四个坡面** + 四条角斜脊 + 中间一条短脊，且整面**全由瓦
 * 铺成**。逆向侧对得上的是 `roof_shape::ridge_length_01_from_rectangle_ratio` —— 脊长是矩形
 * 长宽比的连续函数，越接近正方形脊越短，正方形处连续退化成金字塔。
 *
 * 据此删掉的三样（都不是"暂时不做"，是**在四坡下不存在**）：
 *  · **山墙**：四坡的四面墙全是檐墙，墙顶一律平在 `EaveZ`。
 *  · **翻轴事件**：脊向由形状连续导出，正方形处脊长为 0，「脊朝哪」这个问题根本不出现。
 *  · **实体屋面板**：屋顶是瓦片实例，房体三角汤里一片屋面都不产，墙顶就是平的 `EaveZ`。
 *
 * -----------------------------------------------------------------------------
 * 2026-09-13：矩形 → **凸折线**（footprint 折线化 3e）
 * -----------------------------------------------------------------------------
 * 「每个坡面同一个坡度」的屋面就是 footprint 的**直骨架**屋面。凸多边形上它有一行闭式：
 *
 *     Z(p) = EaveZ + tan(pitch) · min_k ((p − Start_k) · In_k)
 *
 * 即到**每条边所在直线**的有向内距取最小。矩形上这就是原来的 `min(HalfX − |x|, HalfY − |y|)`，
 * 一个字的特例都没有。min 在哪条边上取到，那一点就属于哪个坡面；两条边并列取到的轨迹是骨架的
 * 弧（角斜脊 / 脊）。外挑段内距为负，屋面照同一个式子继续往下走，角上沿平分线外延。
 *
 * ⚠️ **只对凸折线成立**：凹折线的内部不再是半平面的交，「到直线的距离」会被远处的边抢走 min。
 * 凹 footprint 落地时这里要换成真正的直骨架（含分裂事件）。
 *
 * 骨架本身（弧的端点、最高那段脊）由 `CSHouseRoof_BuildSkeleton` 按边塌缩事件求出 —— 脊瓦沿弧铺、
 * 尖顶立在最高那段脊的两端、鸟窝沿它排。
 *
 * -----------------------------------------------------------------------------
 * 2026-10-02：**脊长可调**；脊长 = 房长时四坡 → 双坡 + 山墙（用户裁决，照 TG 反汇编）
 * -----------------------------------------------------------------------------
 * 09-13 那句「脊长与角斜脊都是推论，不是参数，别给它们加旋钮」**作废**：TG 的脊长是存盘参数
 * `RoofShapeParams::ridge_length_01`，玩家拿 `ui_edit_roof_ridge_length` 的抓手拖；
 * `Roof::is_gable` ⇔ `ridge_length_01 == 1.0`，此时两端的坡面消失、端墙升成三角形
 * （`RecalculateGableRoofWallHeights`）。逐条证据与地址见 `Docs/TinyGlade/RE_RoofRidgeGable.md`。
 *
 * TG 的屋面是「檐口矩形（墙矩形外扩）→ 脊矩形（脊长 × 0.1 m）」之间的一张放样面：两端坡面的水平进深
 * = (檐口矩形顺脊长 − 脊长) / 2，与两侧坡面的进深无关 ⇒ **两端坡面的坡度与两侧不同**。落到上面那条
 * 闭式里只多一个量 —— **逐边的坡度倍率** `w_k`（`FCSRoofDesc::EdgeSteep`）：
 *
 *     Z(p) = 檐口外沿高 + tan(pitch) · min_k ( w_k · (内距_k(p) + Overhang) )
 *
 * `w_k ≡ 1` 时逐位退回上一节的式子（外挑段加了又减掉）。四条檐口外沿仍然同高（放样面的下口是平的），
 * 脊高仍然只由 `w = 1` 的那对坡面决定。**山墙边**（`w_k ≤ 0`）不进这个 min：那一头没有坡面，屋面被一张
 * 竖直面收在墙外皮外 `GableOverhang` 处，墙自己沿屋面升上去（`CSHouseRoof_BuildGableProfile`）。
 * 骨架、坡面排区间、法线、覆盖谓词全都吃同一组 `w_k`，仍然没有哪一处为「脊 / 角脊 / 山墙」单写特例。
 *
 * TG 的参数口径（`ridge_length_01` / 脊向 / 默认值）只在 `CSHouseRoof_ApplyRidge` 一处换算成 `w_k`，
 * 且只对矩形 footprint 生效（TG 同：`RoofRidgeDims::from` 对非矩形恒给一个尖）。
 *
 * 全部是无 GPU 依赖的纯函数，可直接进 automation 测试。
 */

/**
 * TG 的脊长下限 0.1 m（`RoofRidgeDims::calc` 1408E2700：脊长 = 0.1 + r·(顺脊边长 − 0.1)）。
 * 不超过它的"脊"在 TG 里是一个尖（`RoofRidgeDims::to_world_space` 1408E27E2 的 `0.1 >= max(dims)` 分支）。
 */
inline constexpr double CSHouseRoof_RidgeMin = 10.0;

/**
 * 脊向（TG `RoofRidgeDirection`，`Roof` + 0x3C）。只在脊长是显式参数时有意义 —— 自动（等坡度）时脊恒沿长轴。
 * 取值就是 `CSHouseRoof_ApplyRidge` 的 `RidgeAxis`：脊平行于几号边。
 */
UENUM(BlueprintType)
enum class ECSRoofRidgeAxis : uint8
{
	/** 脊平行于 0 号边（矩形房子的局部 X）；两端是 1 / 3 号边。 */
	X = 0,
	/** 脊平行于 1 号边（局部 Y）；两端是 0 / 2 号边。 */
	Y = 1,
};

/**
 * 山墙的两种做法（TG `GableRoofStyle`，`Roof` + 0x54；取值与 TG 的 serde 变体表 142C8F798 同序）。
 *
 * ⚠️ 在 TG 里它**不是玩家选项**，是墙体样式的派生量：`create_roof` 1421AB16B 把它设成 `WallStyle::is_halftimbered`，
 * 之后每次墙变 `update_roof_spatial` 1421A79AF 再对齐一次 —— 砖石 / 灰泥墙恒为 `Stepped`，半木构墙恒为 `Overhang`。
 * 本项目没有半木构墙，所以默认 `Stepped`；`Overhang` 留作可选的另一种观感。
 */
UENUM(BlueprintType)
enum class ECSGableRoofStyle : uint8
{
	/**
	 * 阶梯山墙（TG 的砖石墙）：屋面每头**缩进**山墙外皮 26.5 cm，山墙顶线比屋面线高出 `CSHouseRoof_SteppedGableRise`，
	 * 像一道女儿墙立在屋面两头；砖层逐层按行中线收成台阶，角石跟着砌到抬高的角点。
	 */
	Stepped = 0,
	/** 悬挑山墙（TG 的半木构墙）：屋面每头**伸出**山墙外皮 26.5 cm + 檐口出挑，山墙顶线贴着屋面线、藏在瓦底下。 */
	Overhang = 1,
};

/**
 * 阶梯山墙的顶线比屋面线高出多少 cm。TG `init_wall_height_and_arch_segment_height` 14078C19A..14078C1FF：
 * 山墙边上逐点的墙顶 += c + H·(1 − s / W)^k，c = 0.46 m（半木构墙 c = 0）。0.46 m 同时是 TG 一块砖的标称长度。
 */
inline constexpr float CSHouseRoof_SteppedGableRise = 46.0f;

/** 一座屋面的完整描述。房屋 actor 每次生成时现组，不序列化（存盘的是 `ACSHouseActor` 上的那几个屋顶属性）。 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSRoofDesc
{
	GENERATED_BODY()

	/** 底面轮廓（局部 XY，逆时针闭合折线）= 墙外皮。与 `ACSHouseActor::GetFootprint()` 同一份。 */
	UPROPERTY() FCSHouseFootprint Footprint = FCSHouseFootprint::MakeRect(FVector2D(600.0, 400.0));

	/** 檐口高 = 墙高（局部 Z）。倍率为 1 的坡面在 footprint 边界处恰好等于它。 */
	UPROPERTY() float EaveZ = 300.0f;

	/** 坡度（度）= 倍率为 1 的坡面的坡度。其余坡面按 `EdgeSteep` 换算。 */
	UPROPERTY() float Pitch = 35.0f;

	/** 屋檐外挑 cm（每条有坡面的边都挑同样多，沿边的法线量）。 */
	UPROPERTY() float Overhang = 25.0f;

	/** 派生的露台形态；由房子按屋脊相对檐口的高度统一判定。 */
	UPROPERTY() bool bFlat = false;

	/**
	 * 逐边的**坡度倍率**（按边号；坡度的 tan 相对 `Pitch` 的倍数）。**空 = 每条边都是 1** = 等坡度直骨架，
	 * 2026-10-02 之前的全部形态，求值逐位不变。
	 *   · `> 0`：这条边的坡面坡度 = atan(倍率 · tan(Pitch))；
	 *   · `≤ 0`：**山墙边** —— 这条边没有坡面，见 `GableOverhang`。
	 * 别手填：矩形房子由 `CSHouseRoof_ApplyRidge` 从 TG 的脊参数换算。
	 */
	UPROPERTY() TArray<float> EdgeSteep;

	/**
	 * 山墙端：屋面沿脊向伸出墙外皮多少 cm（负 = 缩进墙内，TG 的阶梯山墙）。只对山墙边生效。
	 * 取值见 `CSHouseRoof_GableOverhang`。
	 */
	UPROPERTY() float GableOverhang = 0.0f;

	float TanPitch() const { return FMath::Tan(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/** cos(pitch)。铺瓦时"沿坡量的长度 → 竖直/水平分量"都按它换算，别在生成器里再写一遍三角函数。 */
	float CosPitch() const { return FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	float SinPitch() const { return FMath::Sin(FMath::DegreesToRadians(FMath::Clamp(Pitch, 0.0f, 89.0f))); }

	/** 第 `Edge` 条边的坡度倍率；越界 / 空表 = 1。 */
	double Steep(int32 Edge) const { return EdgeSteep.IsValidIndex(Edge) ? double(EdgeSteep[Edge]) : 1.0; }

	/** 第 `Edge` 条边是不是山墙边（没有坡面）。 */
	bool IsGable(int32 Edge) const { return EdgeSteep.IsValidIndex(Edge) && !(EdgeSteep[Edge] > 0.0f); }

	bool HasGable() const
	{
		for (const float W : EdgeSteep)
		{
			if (!(W > 0.0f)) return true;
		}
		return false;
	}

	/**
	 * 第 `Edge` 个坡面自己的 tan / cos / sin。倍率恰为 1 时返回 `Pitch` 的那三个值。
	 * ⚠️ 「数值上相同」不等于「逐位相同」：引擎开着 /fp:fast，`CosPitch()` 与 `SinPitch()` 紧挨着取时编译器把它们并成
	 * 一次 sincos，结果与经这里分支之后单独取的可以差 1 ulp（35° 的 cos：0.819151998 对 0.819152057）。
	 * 要对旧存档逐位不变的调用方（铺瓦、`CSHouseRoof_EvalNormal`）在倍率为 1 时直接用 `CosPitch()` / `SinPitch()`，
	 * 只对倍率不是 1 的面调这三个。
	 */
	float FaceTan(int32 Edge) const
	{
		const double W = Steep(Edge);
		return W == 1.0 ? TanPitch() : float(FMath::Max(W, 0.0) * double(TanPitch()));
	}
	float FaceCos(int32 Edge) const
	{
		if (Steep(Edge) == 1.0) return CosPitch();
		const double T = double(FaceTan(Edge));
		return float(1.0 / FMath::Sqrt(1.0 + T * T));
	}
	float FaceSin(int32 Edge) const
	{
		if (Steep(Edge) == 1.0) return SinPitch();
		const double T = double(FaceTan(Edge));
		return float(T / FMath::Sqrt(1.0 + T * T));
	}

	/**
	 * 第 `Edge` 个坡面上「离本边内距 `Inset`」那条等高线的**等效内距** = (那里的屋面高 − `EaveZ`) / tan(pitch)。
	 * 倍率 1 的坡面上它就是内距本身；其余坡面从檐口外沿（内距 −Overhang，四面同高）起按倍率爬升。
	 */
	double EquivalentInset(int32 Edge, double Inset) const
	{
		const double W = Steep(Edge);
		return W == 1.0 ? Inset : W * (Inset + double(Overhang)) - double(Overhang);
	}

	/** `EquivalentInset` 的反函数：第 `Edge` 个坡面升到等效内距 `Equivalent` 时离本边的内距。山墙边没有定义。 */
	double FaceInset(int32 Edge, double Equivalent) const
	{
		const double W = Steep(Edge);
		return W == 1.0 ? Equivalent : (Equivalent + double(Overhang)) / FMath::Max(W, 1.0e-6) - double(Overhang);
	}

	/**
	 * 高度场的核心：各坡面**等效内距的最小值**（墙外皮线上为 0、内部为正、外挑段为负；山墙边不参与）。
	 * 等坡度屋面上它就是到每条边所在直线的最小有向内距。退化 footprint（少于 3 个顶点）返回 0。
	 */
	double InsetDistance(const FVector2D& LocalXY) const
	{
		const int32 N = Footprint.NumEdges();
		if (N < 3) return 0.0;
		double Min = TNumericLimits<double>::Max();
		for (int32 Edge = 0; Edge < N; ++Edge)
		{
			if (IsGable(Edge)) continue;
			const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
			if (F.Len <= 0.0f) continue;
			Min = FMath::Min(Min, EquivalentInset(Edge, FVector2D::DotProduct(LocalXY - F.Start, F.In)));
		}
		return Min == TNumericLimits<double>::Max() ? 0.0 : Min;
	}

	/** 屋面最高处的等效内距（等坡度时 = 最大内切圆半径；矩形 = 垂直于脊的那条边的一半）。现算一次骨架，N 很小。 */
	double MaxInset() const;
};

/** 骨架的一条弧 = 两个坡面的交线（角斜脊或脊），或一个坡面被山墙面截出的那条斜边。 */
struct FCSRoofSkeletonArc
{
	/** 两端（局部 XY）与各自的**等效**内距；高度 = EaveZ + tan(pitch) · 内距。 */
	FVector2D A = FVector2D::ZeroVector;
	FVector2D B = FVector2D::ZeroVector;
	double InsetA = 0.0;
	double InsetB = 0.0;
	/** 两侧坡面的边号。 */
	int32 FaceA = INDEX_NONE;
	int32 FaceB = INDEX_NONE;
	/**
	 * 两侧里有一条是山墙边：这条弧不是两坡相交，是坡面的**山墙侧斜边**（从檐口外角沿山墙面爬到脊端）。
	 * 那里没有第二个坡面，不盖斜脊瓦。
	 */
	bool bVerge = false;
};

struct FCSRoofSkeleton
{
	/**
	 * 前 `NumHips` 条是角上起的弧，**按角号**排（角 k = `CSHouse_GetCorner(k)`，夹在 k 号边与 k+1 号边之间），
	 * A 端在檐口外角（等效内距 −Overhang）、B 端在它汇入的骨架节点 —— 角斜脊，或山墙侧斜边（`bVerge`）；
	 * 其后是节点之间的脊，按生成顺序，最后一条是最高的那段脊（金字塔时长度为 0，调用方按长度过滤）。
	 * 矩形上恰好是「四条角上的弧 + 一条脊」。
	 */
	TArray<FCSRoofSkeletonArc> Arcs;
	int32 NumHips = 0;
	/** 屋面最高处的等效内距。 */
	double MaxInset = 0.0;
	/** 最高那段脊的两端（字典序小的在 A：先比 X 再比 Y），金字塔尖时两端重合。 */
	FVector2D TopA = FVector2D::ZeroVector;
	FVector2D TopB = FVector2D::ZeroVector;
};

/**
 * 凸折线屋面的骨架（边塌缩事件，O(N²)）：坡面按各自的倍率爬升，山墙边的那张竖直面不动。
 * 等坡度（`EdgeSteep` 为空或全 1）时就是直骨架，逐位等于下面那个旧签名。退化 footprint 输出空骨架。
 */
COMPUTESHADERGENERATOR_API void CSHouseRoof_BuildSkeleton(const FCSRoofDesc& Roof, FCSRoofSkeleton& Out);

/** 等坡度的直骨架。`Overhang` 只用来把角斜脊的起点外延到檐口外角。 */
COMPUTESHADERGENERATOR_API void CSHouseRoof_BuildSkeleton(const FCSHouseFootprint& Footprint, double Overhang, FCSRoofSkeleton& Out);

inline double FCSRoofDesc::MaxInset() const
{
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(*this, Skeleton);
	return Skeleton.MaxInset;
}

/**
 * 坡面排区间的核：第 `Edge` 条边内偏 `Inset` 的那条直线，被其余每条边各裁一次一维半平面。
 * `Need(Other)` 给出「这一排对 `Other` 号边至少要有多少内距」。两个公开签名只差这一个量。
 */
template <typename FNeed>
inline bool CSHouseRoof_ClipFaceRow(const FCSHouseFootprint& Footprint, int32 Edge, double Inset, FNeed&& Need,
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
		// 「(Base + U·t − G.Start)·G.In ≥ Need」⇔「Q + C·t ≥ 0」。
		const double C = FVector2D::DotProduct(F.U, G.In);
		const double Q = FVector2D::DotProduct(Base - G.Start, G.In) - Need(Other);
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

/**
 * 第 `Edge` 个坡面在内距 `Inset` 处那一排的沿边区间 `[OutT0, OutT1]`（从边起点 `Start` 沿 `U` 量）—— **等坡度**口径。
 *
 * 坡面 k 在内距 d 处的点 = 本边内偏 d 的那条直线上、同时满足「到其余每条边的内距 ≥ d」的那一段。
 * 于是逐条边做一次一维半平面裁剪即可 —— 短边先收成尖、两条原本不相邻的边在尖之后变成相邻，
 * 这些骨架事件全都自动包含在内，不需要知道骨架长什么样。矩形上它给出 `[d, Len − d]`，
 * 就是原来「半宽 = 半边长 − 内距」那条式子。
 *
 * 返回 false = 这一排已经没有这个坡面了（或退化输入）。
 */
inline bool CSHouseRoof_FaceSpanAtInset(const FCSHouseFootprint& Footprint, int32 Edge, double Inset,
	double& OutT0, double& OutT1)
{
	return CSHouseRoof_ClipFaceRow(Footprint, Edge, Inset, [Inset](int32) { return Inset; }, OutT0, OutT1);
}

/**
 * 同上，吃逐边倍率与山墙边：`Inset` 仍是离**本边**的内距，其余坡面按「同一高度」换算各自要让出的内距，
 * 山墙边把这一排截在墙外皮外 `GableOverhang` 处（双坡的排因此一样长，一直通到山墙外）。
 * 山墙边自己没有坡面，恒返回 false。等坡度时与上面那个逐位相同。
 */
inline bool CSHouseRoof_FaceSpanAtInset(const FCSRoofDesc& Roof, int32 Edge, double Inset, double& OutT0, double& OutT1)
{
	if (Roof.IsGable(Edge)) return false;
	const double Level = Roof.EquivalentInset(Edge, Inset);
	return CSHouseRoof_ClipFaceRow(Roof.Footprint, Edge, Inset,
		[&Roof, Level](int32 Other)
		{
			return Roof.IsGable(Other) ? -double(Roof.GableOverhang) : Roof.FaceInset(Other, Level);
		}, OutT0, OutT1);
}

/** 屋面在局部 XY 处的高度。不判是否落在轮廓内（那是 `CSHouseRoof_IsUnderRoof` 的事）。 */
inline float CSHouseRoof_EvalZ(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	return Desc.EaveZ + Desc.TanPitch() * float(Desc.InsetDistance(LocalXY));
}

/** 屋脊高（局部 Z）= 最高处的等效内距 × 坡度。矩形上只由**垂直于脊的那条边**决定（等坡度时即短边）。 */
inline float CSHouseRoof_RidgeZ(const FCSRoofDesc& Desc)
{
	return Desc.EaveZ + Desc.TanPitch() * float(Desc.MaxInset());
}

/** 高度把手用厘米编辑起伏，再反算坡度；零高可逆地退化为露台。 */
inline float CSHouseRoof_PitchFromRise(float Rise, double MaxInset)
{
	if (MaxInset <= UE_KINDA_SMALL_NUMBER) return 0.0f;
	return FMath::Clamp(FMath::RadiansToDegrees(FMath::Atan(FMath::Max(Rise, 0.0f) / MaxInset)), 0.0, 70.0);
}

/** 露台矮墙/垛口的局部砖块，网格轴 X 沿边、Y 横向、Z 朝上。 */
struct FCSRoofParapetBlock
{
	FVector Center = FVector::ZeroVector;
	FVector Size = FVector::OneVector;
	FQuat Rotation = FQuat::Identity;
};

/** 围边入口：本边起点沿 U 的弧长区间，房屋从楼梯的接合诉求现算。 */
struct FCSRoofParapetOpening
{
	int32 Edge = INDEX_NONE;
	double Start = 0.0;
	double End = 0.0;
};

COMPUTESHADERGENERATOR_API void CSHouseRoof_BuildParapet(const FCSRoofDesc& Roof, float Thickness,
	float Height, TArray<FCSRoofParapetBlock>& OutBlocks, TArrayView<const FCSRoofParapetOpening> Openings = {});

/** 檐口外沿高（局部 Z）——外挑最外一圈。四面同高（同坡度、同外挑）。 */
inline float CSHouseRoof_EaveOuterZ(const FCSRoofDesc& Desc)
{
	return Desc.EaveZ - Desc.TanPitch() * Desc.Overhang;
}

/**
 * 屋面外法线（单位，朝上外）。
 *
 * 坡面 k 的法线是 (外法线_k · sin p_k, cos p_k)（p_k = 这个坡面自己的坡度）；把**所有并列取到最小等效内距**的
 * 边的法线相加再归一化，角斜脊上自然给出两面的平均、脊上两面对冲成正上、金字塔尖上全部相消退化成正上方 ——
 * 不需要为脊 / 角脊 / 尖顶各写一个特例。山墙边没有坡面，不参与。
 */
inline FVector CSHouseRoof_EvalNormal(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	const int32 N = Desc.Footprint.NumEdges();
	if (N < 3) return FVector::UpVector;

	TArray<double, TInlineAllocator<16>> Dist;
	TArray<FVector2D, TInlineAllocator<16>> Fall;
	TArray<int32, TInlineAllocator<16>> Faces;
	double MinDist = TNumericLimits<double>::Max();
	for (int32 Edge = 0; Edge < N; ++Edge)
	{
		if (Desc.IsGable(Edge)) continue;
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Desc.Footprint, 0.0f);
		if (F.Len <= 0.0f) continue;
		Dist.Add(Desc.EquivalentInset(Edge, FVector2D::DotProduct(LocalXY - F.Start, F.In)));
		Fall.Add(-F.In);                         // 坡面的下降方向（水平分量）= 外法线
		Faces.Add(Edge);
		MinDist = FMath::Min(MinDist, Dist.Last());
	}

	const float SinP = Desc.SinPitch();
	FVector Sum(0, 0, 0);
	// 容差按 cm 取：并列与否是"这一点在不在角斜脊上"，那是厘米量级的事，不是 ulp 量级的。
	for (int32 I = 0; I < Dist.Num(); ++I)
	{
		if (Dist[I] > MinDist + 0.01) continue;
		// 倍率为 1 的面走加倍率之前的那一句，写法原样（/fp:fast 下换个写法 sin / cos 可以差 1 ulp）。
		if (Desc.Steep(Faces[I]) == 1.0)
		{
			Sum += FVector(Fall[I].X * SinP, Fall[I].Y * SinP, Desc.CosPitch());
			continue;
		}
		const float FaceSinP = Desc.FaceSin(Faces[I]);
		Sum += FVector(Fall[I].X * FaceSinP, Fall[I].Y * FaceSinP, Desc.FaceCos(Faces[I]));
	}
	return Sum.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
}

/**
 * 该局部 XY 是否被屋面覆盖（外挑都算）。D8 那条"落屋顶 → 不生成窗"的谓词用它。
 * 山墙那一头按 `GableOverhang` 算：屋面在山墙面外那么远处被竖直收住。
 */
inline bool CSHouseRoof_IsUnderRoof(const FCSRoofDesc& Desc, const FVector2D& LocalXY)
{
	if (!Desc.Footprint.IsValidFootprint() || !(Desc.InsetDistance(LocalXY) >= -double(Desc.Overhang))) return false;
	for (int32 Edge = 0; Edge < Desc.EdgeSteep.Num() && Edge < Desc.Footprint.NumEdges(); ++Edge)
	{
		if (!Desc.IsGable(Edge)) continue;
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Desc.Footprint, 0.0f);
		if (F.Len > 0.0f && FVector2D::DotProduct(LocalXY - F.Start, F.In) < -double(Desc.GableOverhang)) return false;
	}
	return true;
}

// -----------------------------------------------------------------------------
// TG 的脊参数（`country_core::resources::roofs::{roof_shape, roof_ridge}`，反汇编见 RE_RoofRidgeGable.md）
// -----------------------------------------------------------------------------

/**
 * TG `RoofRidgeDims::calc`（1408E2700）：脊长 = (1 − r)·0.1 m + r·顺脊边长。`r = 1` ⇒ 脊与房子一样长（山墙）。
 * 入参出参都是 cm。
 */
inline double CSHouseRoof_RidgeLengthFrom01(double Ridge01, double AlongLength)
{
	const double R = FMath::Clamp(Ridge01, 0.0, 1.0);
	return (1.0 - R) * CSHouseRoof_RidgeMin + R * AlongLength;
}

/**
 * TG `roof_ridge::inverse_ridge_01_param`（1408E30F0）：r = clamp((脊长 − 0.1 m) / (顺脊边长 − 0.1 m), 0, 1)。
 * 拖脊长抓手时用它把厘米折回参数；**钳到 1 的那一下就是山墙的来源**（`Roof::is_gable` 要求恰好等于 1）。
 * 顺脊边长不超过 0.1 m 时 TG 直接 panic，这里返回 0。
 */
inline double CSHouseRoof_Ridge01FromLength(double RidgeLength, double AlongLength)
{
	const double Span = AlongLength - CSHouseRoof_RidgeMin;
	if (!(Span > UE_DOUBLE_KINDA_SMALL_NUMBER)) return 0.0;
	return FMath::Clamp((RidgeLength - CSHouseRoof_RidgeMin) / Span, 0.0, 1.0);
}

/**
 * TG `roof_shape::ridge_length_01_from_rectangle_ratio`（1408E3B60）—— 新建矩形房子时脊长参数的默认值：
 *
 *     s = smoothstep01((max(a, b) − 2.25 m) / 5.25 m)
 *     r = smoothstep01((max(a/b, b/a) + 1.5·s − 1.5) / 1.5)
 *
 * 小房子要长宽比到 3 才是山墙，大房子（长边 ≥ 7.5 m）长宽比 1.5 就是；大正方形是 0.74。入参 cm。
 */
inline float CSHouseRoof_DefaultRidgeLength01(double SizeA, double SizeB)
{
	if (!(SizeA > 0.0) || !(SizeB > 0.0)) return 0.0f;
	auto Smooth01 = [](double X)
	{
		const double T = FMath::Clamp(X, 0.0, 1.0);
		return T * T * (3.0 - 2.0 * T);
	};
	const double Ratio = FMath::Max(SizeA / SizeB, SizeB / SizeA);
	const double Big = Smooth01((FMath::Max(SizeA, SizeB) - 225.0) / 525.0);
	return float(Smooth01((Big * 1.5 + Ratio - 1.5) / 1.5));
}

/**
 * 山墙端屋面沿脊向伸出墙外皮的量 cm（`FCSRoofDesc::GableOverhang`）—— TG `assemble_rectangular_roof` 1421AF07E..1421AF101：
 * 山墙时把檐口矩形的顺脊长改成「原长 + d」，悬挑式 d = +(0.25 m + eave)、阶梯式 d = −(0.81 m + eave)；
 * 原长 = 墙矩形 + 0.28 m + eave（`get_roof_expanded_top_and_bottom` 140A880C0，eave = `eave_length_ws`）。
 * 折到每一头：悬挑式伸出 0.265 m + eave，阶梯式缩进 0.265 m（与 eave 无关）。
 *
 * 本项目没有 eave 这个量，只有每侧外挑 `Overhang`，它对应 TG 的 0.14 m + eave / 2 ⇒ eave = 2·(Overhang − 14 cm)，
 * 外挑不足 14 cm 时按 eave = 0。
 */
inline float CSHouseRoof_GableOverhang(ECSGableRoofStyle Style, float Overhang)
{
	if (Style == ECSGableRoofStyle::Stepped) return -26.5f;
	return 26.5f + FMath::Max(2.0f * (Overhang - 14.0f), 0.0f);
}

/** footprint 是不是矩形（四条边、四个直角；朝向任意）。脊参数只对它生效。 */
inline bool CSHouseRoof_IsRectangle(const FCSHouseFootprint& Footprint)
{
	if (Footprint.NumEdges() != 4) return false;
	for (int32 Edge = 0; Edge < 4; ++Edge)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Edge, Footprint, 0.0f);
		const FCSHouseEdgeFrame G = CSHouse_GetEdge((Edge + 1) & 3, Footprint, 0.0f);
		if (F.Len <= 0.0f || FMath::Abs(FVector2D::DotProduct(F.U, G.U)) > 1.0e-4) return false;
	}
	return true;
}

/**
 * 把 TG 的脊参数落成 `EdgeSteep` / `GableOverhang`（覆盖写）。只对矩形 footprint 生效，其余清空倍率返回 false。
 *
 * `RidgeAxis`（TG `RoofRidgeDirection`）：0 = 脊平行于 0 号边（`MakeRect` 上即局部 X），1 = 平行于 1 号边。
 * 平行于脊的那对边是**主坡面**（倍率 1，`Pitch` 说的就是它们）；另一对是两端：
 *   · `Ridge01 < 1`：端坡面。倍率 = (半跨 + Overhang) / (半长 + Overhang − 脊长 / 2) —— TG 的放样面：
 *     檐口矩形（墙矩形四面各外扩 Overhang）到脊线段的连线。脊长不超过 `CSHouseRoof_RidgeMin` 时按一个尖算。
 *   · `Ridge01 ≥ 1`（TG `Roof::is_gable` 1408E33B0：`ridge_length_01 == 1.0`）：山墙边，屋面沿脊伸出 `InGableOverhang`。
 */
COMPUTESHADERGENERATOR_API bool CSHouseRoof_ApplyRidge(FCSRoofDesc& Desc, float Ridge01, int32 RidgeAxis, float InGableOverhang);

/**
 * 山墙边上方那片墙的**上沿**：`OutProfile` 的每个点是 (沿边弧长 S, 屋面在墙外皮线上的等效内距)，按 S 升序，
 * 首尾恒在 `S = 0` 与 `S = Len`。等效内距 × tan(pitch) 就是那一点的墙比檐口高出多少 —— 矩形上是一个等腰三角形，
 * 顶点在脊端正下方。**与屋面是同一个求值器**（沿墙线取 `InsetDistance`），墙顶因此逐点贴着屋面，不会一个陡一个缓。
 * 不是山墙边返回 false。
 */
COMPUTESHADERGENERATOR_API bool CSHouseRoof_BuildGableProfile(const FCSRoofDesc& Roof, int32 Edge, TArray<FVector2D>& OutProfile);

// 屋面**不起伏、檐口不起翘**（用户 2026-09-15 看实拍后裁决）。TG 的瓦 VS 两项都有 ——
// `_nani_instanced_roof…b903…vs_main.glsl` :213 檐口起翘、:224-283 按离屋顶基准 `roof_pivot_y` 的高度加权的
// value-noise 起伏 —— 是**有意不移植**，不是漏了（09-14/15 移植过两版，都被否掉）。瓦只保留材质 WPO 里的
// 瓦端抬高与水平抖动（`Scripts/TinyGladeMakeRoofMaterial.py` 文件头），所以尖顶、檐口 / 屋脊摆件直接落在
// 上面这组屋面求值器上，不需要位移孪生函数。

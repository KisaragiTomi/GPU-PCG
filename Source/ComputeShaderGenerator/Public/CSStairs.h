#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"   // RandomSplits 的入参 —— 别指望 unity 构建替你带进来
#include "CSStairs.generated.h"

UENUM(BlueprintType)
enum class ECSStairsRailing : uint8
{
	None,
	Low,
	High,
	Wooden,
};

/**
 * 玩家绘制楼梯的纯函数层（TG §4.1 `playermade` 的对位物，2026-09-16）。
 *
 * 逐条出处与常量见 `Docs/TinyGlade/RE_PlayerStairs.md`（下称「附录 D」）。
 * `ACSStairsActor` 只负责把样条与地面翻成这里的输入、把输出的砖交给实例组件；**判据全在这里**，
 * 所以无头单测不需要 world。
 *
 * -----------------------------------------------------------------------------
 * TG 的做法一句话：**没有踏高 / 踏深参数**
 * -----------------------------------------------------------------------------
 * 每条边（两个楼梯点之间）先按 ≤ 50 cm 密采样，高度在两端之间线性插值、且不许低于地面 10 cm；
 * 按首尾弦坡度分成平走道 / 踏步 / 梯子；同类型的一串点连成 3D 折线，再**按 50 cm 等弧长重采样**，
 * 相邻两点就是一级。⇒ 每级的斜边长恒约 50 cm，踏高 = 50·sinθ、踏深 = 50·cosθ（附录 D §2.1）。
 *
 * 每级是一块**从踏面往下至少 60 cm 的实心砖**（或横向切成几块），前后级进深互相搭 13%、
 * 竖向互相叠压 —— 所以底面永远看不到，也不需要"楼梯斜梁"这种东西（附录 D §2.2）。
 *
 * -----------------------------------------------------------------------------
 * 与 TG 的有意差异（MVP，逐条写在对应函数上）
 * -----------------------------------------------------------------------------
 * - 节点高度是**样条点的世界 Z**，不是 TG 的「地形 + 偏移」：UE 里直接在视口拖样条点最顺手，
 *   而"不许低于地面 10 cm"那条夹子照样保证楼梯不会整段埋进土里。
 * - 梯子（坡度 > 2.5）复用木板网格装配边梁和横档；三种栏杆随踏步路径生成。尺寸为本项目适配。
 * - 顶面四角的随机起伏（`sin_deform_y`，±5 cm）不做：那是 TG 砖顶点着色器里的形变，
 *   本项目的实例组件画的是刚体单位盒。
 * - 支撑：`CSStairsSupport` 规划拱洞，复用房屋的洞剖面、横带切分和拱圈排砖；托架尚未接入。
 * - 门前踏步（§4.0）在这一层（`BuildDoorSteps`），由窗贴墙脚变成的门调用。
 */
namespace CSStairs
{
/** 分段类型（TG `determine_segment_type` 的返回值，数值照抄）。 */
enum class ESegmentType : uint8
{
	/** 坡度 ≤ 0.25：平走道，踏面抬到地面 + 15 cm 以上。 */
	Walkway = 1,
	/** 0.25 < 坡度 ≤ 2.5：踏步。 */
	Steps = 2,
	/** 坡度 > 2.5：生成木梯边梁与横档。 */
	Ladder = 3,
};

/** 全部常量。默认值一律是 TG 的实测值（米 → cm），出处在各字段的注释里。 */
struct FParams
{
	/** 密采样的点距上限 cm（`densely_sample_spline_segment`：每米至少两个点）。 */
	float SampleSpacing = 50.0f;
	/** 重采样的弧长步长 cm = 一级踏步的斜边长（`Curve::try_resample(0.5)`）。 */
	float StepLength = 50.0f;
	/** 首尾弦坡度 ≤ 它 ⇒ 平走道（`__real@3e800000` = 0.25）。 */
	float WalkwayMaxSlope = 0.25f;
	/** 首尾弦坡度 > 它 ⇒ 梯子（`__real@40200000` = 2.5）。 */
	float LadderMinSlope = 2.5f;
	/** 高度最多比地面低多少 cm（`height = max(lerp, ground − 10)`）。 */
	float BuryTolerance = 10.0f;
	/** 踏面至少高出地面多少 cm（走道抬升与每级踏面共用，都是 15）。 */
	float TreadClearance = 15.0f;
	/** 一块踏步砖的最小竖向尺寸 cm（`h = max(|Δy|, 60)`）。 */
	float MinBlockHeight = 60.0f;
	/** 较高那一端水平多远以内的级要加厚 cm（`k < floor(0.56 / d01)`）。 */
	float TopThickenReach = 56.0f;
	/** 前后级的进深搭接倍率（进深 = 两点水平距离 × 1.13）。 */
	float DepthOverlap = 1.13f;
	/** 宽度下限 cm（TG 的 `min_width` 取 30 或 63，这里取小的那档）。 */
	float MinWidth = 30.0f;
	/** 有栏杆时宽度收窄 cm（`narrow_by_railing`）。 */
	float RailingNarrow = 25.0f;
	bool bNarrowByRailing = false;
	/** 横向切砖：平均宽 < 66 一块；66–120 随机一或两块；≥ 120 按砖长切。 */
	float SingleBrickMaxWidth = 66.0f;
	float TwoBrickMaxWidth = 120.0f;
	/** 宽楼梯的砖长 = smoothstep(rand) · lerp(70, 130, clamp((w − 200) / 200)) + 46。 */
	float BrickLengthBase = 46.0f;
	float BrickLengthNarrow = 70.0f;
	float BrickLengthWide = 130.0f;
	/**
	 * 横向切砖的**抖动量**下限 cm（`max(0.3ℓ, 32)`，除以宽度后交给 `RandomSplits`）。
	 * ⚠️ 这不是"最窄一片"：TG 的 `random_splits` 是近似等分 + 分界抖动（附录 D §6.4）。
	 */
	float SplitJitterWidth = 32.0f;
	/** Enable grounded masonry using the shared wall profile and brick layout. */
	bool bSolidToGround = true;
	/** Fit through arches wherever the stair clearance permits; false produces a solid wall. */
	bool bArchedSupport = true;
	float SupportCourseHeight = 25.0f;
	ECSStairsRailing Railing = ECSStairsRailing::None;
};

/**
 * 门前踏步的全部常量（TG `check_for_door_stairs` + `construct_door_stairs`，附录 D §6 / 附录 E §4.3）。
 * 默认值一律是 TG 实测（米 → cm）。
 */
struct FDoorStepsParams
{
	/** 门洞底高出门下地面超过它 ⇒ 不出（门悬空，交给栏杆）。 */
	float MaxSillGap = 20.0f;
	/** 门外探测点的距离。 */
	float ProbeDistance = 106.0f;
	/** 门下地面比门外高不过它 ⇒ 不出（门外没有下坡）。 */
	float MinDrop = 10.0f;
	/** 落差达到它 ⇒ 不出（太高，TG 交给别的结构）。 */
	float MaxDrop = 150.0f;
	/** 踏步总外伸（调用点写死 0.56）。 */
	float Outreach = 56.0f;
	/** 第一排中心离门心多远。 */
	float FirstRowOffset = 28.0f;
	/** 前后排的进深搭接。 */
	float RowOverlap = 10.0f;
	/** 一层砖的名义厚度（层数、排数都按它算）。 */
	float LayerHeight = 20.0f;
	/** 层分界的抖动 ±cm。 */
	float LayerJitter = 2.0f;
	/** 横向每块的名义宽度（分界点数 = max(ceil(w / 45), 3)）。 */
	float LateralPieceWidth = 45.0f;
	/** 横向分界的抖动 ±cm。 */
	float LateralJitter = 9.0f;
	int32 MinRows = 2;
	int32 MaxRows = 5;
};

/** 一扇门在世界里的样子（踏步只需要这四样）。 */
struct FDoorStepsInput
{
	/** 门心的世界 XY（墙厚中线上）。 */
	FVector2D DoorXY = FVector2D::ZeroVector;
	/** 门朝外的水平方向。 */
	FVector2D Outward = FVector2D(1.0, 0.0);
	/** 门洞底的世界 Z。 */
	float DoorBottomZ = 0.0f;
	/** 门宽 cm（= 洞宽，踏步不加宽）。 */
	float Width = 100.0f;
};

/** 楼梯点（样条控制点）：世界位置（Z = 节点高度）+ 宽度 cm。 */
struct FNode
{
	FVector Position = FVector::ZeroVector;
	float Width = 150.0f;
};

/** 地面采样：世界 XY → 世界 Z。**空 = 没有地面**：不夹高度、不抬踏面。 */
using FGroundSampler = TFunction<float(const FVector2D&)>;

/** 密采样点（一条边内）。 */
struct FSample
{
	FVector Position = FVector::ZeroVector;
	float Width = 0.0f;
	/** 水平前进方向（单位向量）。 */
	FVector2D Dir = FVector2D(1.0, 0.0);
};

/** 同类型的一串连续采样点 —— 一段重采样与出砖的单位。 */
struct FRun
{
	ESegmentType Type = ESegmentType::Steps;
	TArray<FSample> Samples;
};

/** 一块砖：单位立方体（100 cm 居中）的实例 —— 中心、朝向、尺寸 cm。 */
struct FBrick
{
	FVector Center = FVector::ZeroVector;
	FQuat Rotation = FQuat::Identity;
	FVector Size = FVector(100.0);
	/** 第几级（从 run 起点数），诊断与单测用。 */
	int32 StepIndex = 0;
};

/**
 * 首尾弦坡度 → 分段类型（TG `determine_segment_type`：`s = |Δh| / |Δxz|`）。
 * 水平弦为零（竖直段）按梯子算。
 */
COMPUTESHADERGENERATOR_API ESegmentType ClassifySegment(float DeltaHeight, float HorizontalChord, const FParams& Params);

/**
 * 地面夹高（附录 D §2.1 第 1 步）：左右各 1/4 宽处取较高的地面，`height = max(h, ground − BuryTolerance)`。
 * 没有地面时原样返回。
 */
COMPUTESHADERGENERATOR_API float ClampToGround(const FVector& Position, const FVector2D& Dir, float Width,
	const FGroundSampler& Ground, const FParams& Params);

/**
 * 3D 折线按弧长等距重采样（TG `Curve::try_resample`）：`N = max(round(L / Step), 1)`，输出 **N + 1** 个点，
 * 端点逐位保留。`OutSource[i] = (源段下标, 段内 t)`，调用方拿它插值宽度与方向。
 * 输入少于 2 个点或总长为 0 时输出为空。
 */
COMPUTESHADERGENERATOR_API void ResamplePolyline(TArrayView<const FVector> Points, float Step,
	TArray<FVector>& OutPoints, TArray<TPair<int32, float>>& OutSource);

/**
 * 一段 run → 踏步砖（TG `construct_stair_steps`，附录 D §2.2）。
 * `Seed` 决定横向切砖与进深的随机数；同一份输入同一个种子逐位相同。返回这段出了几级。
 */
COMPUTESHADERGENERATOR_API int32 BuildRunBricks(const FRun& Run, const FParams& Params, const FGroundSampler& Ground,
	uint32 Seed, TArray<FBrick>& OutBricks);

/** 沿踏步两侧生成栏杆，梯子另出两根边梁与等距横档；输出尺寸以 cm 计。 */
COMPUTESHADERGENERATOR_API void BuildRunRails(const FRun& Run, const FParams& Params, const FGroundSampler& Ground,
	TArray<FBrick>& OutRails);
COMPUTESHADERGENERATOR_API int32 BuildLadder(const FRun& Run, const FParams& Params, TArray<FBrick>& OutWood);

/**
 * 切分（TG `utils::random_splits(n, jitter)`，附录 D §6.4）：把 [0, 1] **近似等分**成 `Count` 片，
 * 每条内部分界再抖 `(rand − 0.5) · j`，`j = min(0.495 / Count, Jitter)`。输出 `Count + 1` 个递增边界，首 0 尾 1。
 *
 * ⚠️ **不是最小片宽约束**。`0.495 / Count` 那个上限保证相邻分界各自最多挪不到四分之一片，
 * 永远不会交叉 —— 所以不需要任何"放不下就减片"的逻辑。
 */
COMPUTESHADERGENERATOR_API void RandomSplits(int32 Count, float Jitter, FRandomStream& Rand, TArray<float>& OutBounds);

/**
 * 门前踏步（TG §4.0：`check_for_door_stairs` 判、`construct_door_stairs` 砌，附录 D §6）。
 *
 * 判据：门槛贴地（门洞底离门下地面 ≤ 20 cm）**且**门外 106 cm 处地面下沉 10–150 cm。
 * ⚠️ 读法：**不是**"门悬在半空就补台阶"—— 门悬空时 TG 直接不出（交给栏杆，见 `ShouldAddDoorRails`）。
 *
 * 砌法：一座 56 cm 外伸的小金字塔。排数 `N = clamp(ceil(H / 20), 2, 5)`，每排进深 `56 / N`；从门下地面到
 * 最低点均分 `L = max(ceil(H / 20), 2) − 1` 层（分界抖 ±2 cm），自上而下第 k 层铺 `min(k + 1, N)` 排 ——
 * 顶层一排贴门、往下每层多外伸一排。每排横向切 `max(ceil(w / 45), 3) − 1` 块（分界抖 ±9 cm）。
 * TG 那边水面以下的"沉底"（`sink`）没有对位物，恒 0。
 *
 * @return false = 这扇门不出踏步（`OutBricks` 不动）。
 */
COMPUTESHADERGENERATOR_API bool BuildDoorSteps(const FDoorStepsInput& Door, const FDoorStepsParams& Params,
	const FGroundSampler& Ground, uint32 Seed, TArray<FBrick>& OutBricks);

/**
 * 门口要不要栏杆（附录 E §4.2）：落地门、**没出踏步**、且墙脚比门下地面高出 15 cm 以上。
 * 墙脚悬空（下面是柱子 / 坡）又没法铺踏步时，门口加一道栏杆兜底。
 */
inline bool ShouldAddDoorRails(float WallBaseZ, float GroundZ, bool bStepsAdded, float Clearance = 15.0f)
{
	return !bStepsAdded && WallBaseZ > GroundZ + Clearance;
}
}

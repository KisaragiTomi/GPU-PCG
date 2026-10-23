#pragma once

#include "CoreMinimal.h"

#include "CSGroundShaperSteps.h"   // FPaletteBuffers

/**
 * 承重柱的砖石装配（TinyGladeHouse D9 的表现层，2026-09-06 用户裁决"用 TG 的模型与逻辑"）。
 *
 * **TG 的对位物**（PDB 实证，逆向文档里没有覆盖到这一族）：
 * `system_wall_constructor::construct_elevation_supports`，模块结构是
 *   · `detect_overhang_support_segments` / `find_overlap_segments` —— 先找出哪几段悬空；
 *   · `RecalculateRectangleSupports` → `construct_rectangle_columns` / `_pillars` /
 *     `_brackets` / `_small_brackets` —— 矩形 footprint 那一支（我们正是矩形）；
 *   · `util_pillar_construction` —— 共用装配器：`construct_brick_columns`、
 *     `util_cross_brick_pillar::construct_crossbrick_pillar`、`construct_wooden_pillar_w_stone_base`。
 *
 * ⚠️ **TG 没有柱子网格**：`assemble_stone_pillar` 是**装配**，砖就是墙上那块 `brick`
 * （1×1×1 居中、600 顶点的倒角石块，**字典 mesh** —— 非均匀缩放本身就是砖的尺寸）。
 * 所以这里也不引入新资产，与门框砖 / 墙砖共用同一块。
 *
 * 本档落地的是**石柱 + 托架**（用户 2026-09-06 选的 B 档）：圆形分支（`circle_*`）与木柱
 * 石基变体不做 —— 前者的前提是圆 footprint，本项目的墙是刚性矩形，没有落点。
 */
namespace CSHousePillar
{
/** 一块砖 = 原点 + 三条**已经缩放过**的轴。与 `CSHousePack.usf` 里 PackHousePillarCS 的记录布局逐字对应。 */
struct FBrick
{
	FVector3f Origin = FVector3f::ZeroVector;   // 世界，砖的**中心**（`brick` 的原点在包围盒正中）
	FVector3f AxisX = FVector3f(1.0f, 0.0f, 0.0f);
	FVector3f AxisY = FVector3f(0.0f, 1.0f, 0.0f);
	FVector3f AxisZ = FVector3f(0.0f, 0.0f, 1.0f);
	float Random01 = 0.0f;
};

/** 柱子的形态参数。全部由 `ACSHouseActor` 的属性喂进来，这里不留默认策略。 */
struct FParams
{
	float BrickWidth = 34.0f;      // 砖的截面边长（世界 cm）
	float CourseHeight = 20.0f;    // 一层砖的高度
	float YawJitter = 0.035f;      // 每层在 90° 旋转档上微偏转，装配时上限 0.05 rad，保留连续承压面
	float SizeJitter = 0.04f;      // 对称尺寸抖动，上限 5%；两条水平轴共同缩放
	/**
	 * 托架（TG 的 `construct_rectangle_brackets` / `_small_brackets`）：顶部这几层向外**逐层出挑**，
	 * 把房底"托"住。0 = 不做托架，退回纯直柱。
	 *
	 * ⚠️ 出挑必须**逐层递增且总量有限**：一次挑出去太多会让最上层砖悬在柱心之外，
	 * 读起来是"顶上粘了一圈砖"而不是砌出来的托。TG 分了 brackets / small_brackets 两档，
	 * 这里用一条曲线覆盖 —— 顶层挑 `BracketOverhang`，往下按层数线性收回。
	 */
	int32 BracketCourses = 3;
	float BracketOverhang = 0.55f;   // 顶层相对砖宽的出挑比例
	int32 Seed = 1;
};

/**
 * 纯函数：柱位 + 柱长 → 砖。**不碰任何 GPU 资源，可以在纯 CPU 单测里跑。**
 *
 * `Centers` / `Lengths` 直接吃 `ACSHouseActor::ComputePillars` 的产物：中心是**构建空间**的
 * 局部坐标（z = 0 是房底），长度是"房底到地面的空隙 + 埋深"。`World` 把它们搬到世界。
 *
 * ⚠️ 随机一律走 `(柱号, 层号, 佐料, 种子)` 的身份哈希，**绝不取槽位**——与藤蔓、石阶同一条纪律：
 * 槽位由线程组完成顺序决定，同一份世界状态两次装配会把同一块砖放进不同槽，
 * 而材质拿它做颜色变化时就是"重建一次全场变色"，且不会有任何断言报红。
 */
COMPUTESHADERGENERATOR_API void BuildBricks(const TArray<FVector>& Centers, const TArray<float>& Lengths,
	const FTransform& World, const FParams& Params, TArray<FBrick>& OutBricks);

/**
 * 录一趟打包 pass：上传记录 → 一个 dispatch 写满 packed 行 + counter。**录完直接返回，不阻塞。**
 *
 * 前置条件是 `CSShaperSteps::ReserveCapacity` 已经把容量备好 —— 这里只用现有容量，一个字节
 * 都不分配。容量不够时**截断**而不是扩容：少砌几块砖远好过在拖动的某一帧付一次设备同步。
 */
COMPUTESHADERGENERATOR_API bool Pack(const TArray<FBrick>& Bricks, const CSShaperSteps::FPaletteBuffers& Palette,
	const FMatrix44f& WorldToComponent);
}

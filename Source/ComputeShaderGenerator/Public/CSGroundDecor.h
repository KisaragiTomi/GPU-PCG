#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperField.h"   // 高度场的 CPU 孪生 —— 裙边在哪儿只能由它说了算
#include "CSHouseDecor.h"          // FAnchor / FParams / BuildPlan / Pack：规划那一段五家共用

/**
 * 装饰摆件锚点层的**第五家：塑形物裙边**（TinyGladeHouse D12）。
 *
 * -----------------------------------------------------------------------------
 * 归属：为什么挂在 `ACSGroundActor` 上，而不是 `ACSGroundShaperActor`
 * -----------------------------------------------------------------------------
 * 这一家当初被跳过的理由（"它得挂在塑形物上，而那边的容量走 `CSShaperSteps::EnsureCapacity`
 * —— 正是石阶 S3 要删的东西"）已经随 S3 执行完而消失。但**归属不能照抄那句话里的假设**：
 * 塑形物今天已经被削成只负责高度场（类注释原文：「这里因此不再有任何 palette、实例组件或
 * GPU 缓冲 —— 塑形物只负责高度场」），给它重新挂一个实例组件等于把刚做完的简化推回去。
 *
 * 归地面的三条依据，**都不是"哪边写起来方便"**：
 *
 * 1. **裙边在哪儿是合成场说了算，不是任一座说了算。** 高度场的合成律是"全部塑形物取 max"
 *    （计划 D9）。A 座裙边上的一段，只要 B 座在那儿更高，那段地表根本不是 A 的裙 ——
 *    它是 B 的坡。锚点因此必须能看见**全部**塑形物，而单座 actor 天然只看得见自己。
 *    本模块的 `BuildSkirtAnchors` 正是拿整份环表算合成场再逐点判"这一段是不是本座的地表"，
 *    这段判断在塑形物那一侧写不出来。
 * 2. **与已有的两条派生链同归。** 石阶（marching squares 扫合成后的高度场）与披挂岩壳
 *    （mask 由合成后的坡度决定）都已经按同一条理由归了地面；裁决二给碎石写的那句
 *    「覆盖整张地面、与任何单座塑形物无关；mask 由**全部塑形物合成后**的坡度决定，
 *    归任一座都不对」在这里逐字成立。三条派生链归属一致，删一座塑形物时的生命周期也一致
 *    （高度场塌回 ⇒ 锚点自己消失，**没有一行注销代码**）。
 * 3. **交互期的成本只在地面这一侧付得起。** 摆件走 GPU 实例路，要 palette / 容量预留 /
 *    包围盒交接。把这一套按塑形物分片，N 座就是 N 份组件与 N 次交接；地面这边是一份，
 *    而且与石阶 / 岩壳共用同一条"落笔 → 重扫"的时序（`FlushPaintToGpu` 之后），
 *    不需要任何跨 actor 同步。
 *
 * ⇒ **塑形物只提供高度场，地面负责派生几何。** 塑形物这一侧一行代码都不加。
 *
 * -----------------------------------------------------------------------------
 * 锚点怎么取：**解析剖面的等值带上按弧长布点**
 * -----------------------------------------------------------------------------
 * 每座塑形物出一圈锚点，名义半径 `R = 零等值线最远半径 + FalloffDistance × SkirtBandT`，
 * 按 `N = round(2πR / SkirtSpacing)` 取 N 个方向，第 i 个在角 `2π(i + 0.5)/N`。
 * 圆盘就落在半径 R 的圆上；mask 形状（2026-09-29 起）沿该方向找距离场 = `Falloff × SkirtBandT`
 * 的最外一个交点（同一条等值带，只是不再是圆），朝向取距离场的梯度。因此：
 *
 *   · **密度完全由锚点个数决定**（D12 的口径）：台子拉大 ⇒ 周长变长 ⇒ 锚点变多，
 *     一个锚点最多一件摆件。没有场、没有阈值、没有随机撒点。
 *   · **那条圆真的是等值带**，不是随手取的半径：`CSGroundShaperField::EvalShaper` 的剖面
 *     参数是 `T = (r − Radius) / FalloffDistance`，取常数 T 得到的就是常数 S 的等值线。
 *     台子拉大拉高时锚点仍停在剖面的同一档，参数不用重调。
 *
 * **为什么不按坡度带取**（另一个显而易见的候选）：坡度带要扫格子求梯度、再取阈值内的胞腔 ——
 * 那是一个**场**（正是 C2 里挂起的那一半），而且密度会由格子数而不是构件数决定，
 * 与 D12 已经钉死的口径冲突。坡度带能做到的"陡的地方多摆点"，`SkirtBandT` 一个标量就给了。
 *
 * **为什么不跟着噪声后的等值线走**：裙边噪声只做侵蚀（`EvalShaper` 里那条只减不加的项），
 * 跟着它的等值线走就得解等高线 —— 那是石阶那条 marching squares 的机器，一整个数量级的复杂度。
 * 这里的做法是"锚点的 XY 按解析圆取，Z 由地面镜像采"：摆件最终**站在真实的、被侵蚀过的地表上**，
 * 只是它的平面位置来自解析剖面。噪声改变的是地面高低，不改变"这一圈该有几件"。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 随机源与遍历序：两条都不许取"数组下标"
 * -----------------------------------------------------------------------------
 * 逐实例随机由 `CSHouseDecor::IdentityHash(家族, 锚点 id, 佐料, 种子)` 给，而锚点 id 由
 * **(本座的稳定键, 环上第几号)** 混出来。三种看似省事的取法都是错的：
 *
 *   · **`InterlockedAdd` 的槽位** —— 槽位由线程组完成顺序决定，同一份世界状态两次散布可以
 *     把同一件摆件放进不同槽，症状是"画一笔路全场变色"，**而且不会有任何断言报红**
 *     （S1 已经栽过一次）。本模块的记录由 CPU 排定、线程 i 写第 i 行，压根没有原子。
 *   · **`ACSGroundActor::Shapers` 的数组下标** —— 那个数组是世界扫描 + 各自登记攒出来的，
 *     顺序不随关卡保存、也不保证两次加载相同。拿它当身份 = 换一次加载全场重掷。
 *   · **塑形物的世界坐标** —— 拖动塑形物时每一帧都在变，摆件会在整个拖动过程里不停重掷，
 *     而它们本该只是跟着土台平移（这与 `CSHouseDecor::FAnchor::AnchorId` 那条
 *     "身份里刻意不含位置"是同一条纪律）。
 *
 * 用的是 **actor 名的 CRC**（`RingKey`）：随关卡序列化、拖动不变、加载顺序无关。
 * 遍历序也按这个键排 —— `BuildPlan` 的最小间距是"先放的挤掉后放的"，顺序必须是确定的。
 */
namespace CSGroundDecor
{
/**
 * 一座塑形物在裙边生产者眼里的样子 = **它的高度场打包头 + mask 距离场** + 一个稳定键。
 *
 * 直接存打包参数而不是存 (中心, 半径, 台高)，是为了让本模块与 GPU 位移 pass、石阶扫描、
 * CPU 镜像重导出**读同一份数**（`ACSGroundActor::BuildShaperGpuParams` 那一份）。
 * 各自从 actor 属性再算一遍的症状是"摆件那圈的半径与土台差了一点点"，只在改过
 * `SecondaryLiftScale` 之类的参数之后才显形。布局见 `CSGroundShaperField.h`。
 */
struct FShaperRing
{
	/** 打包头（布局见 `CSGroundShaperField.h`）。`Header.Noise.Z == 0` ⇒ 解析圆盘，半径 = `Header.Profile.Z`。 */
	CSGroundShaperField::FShaperHeader Header;
	/** mask 距离场（圆盘时为空）。拷一份而不是存视图：Site 的生命周期不归塑形物管。 */
	TArray<float> Sdf;

	/** 稳定身份，见文件头。0 是合法值，但同一份 Site 里两座撞键会让它们共用随机数。 */
	uint32 Key = 0;
};

/**
 * 生产锚点要读的世界。两个采样器可以为空（纯 CPU 单测就不传）：
 * 空 `SampleGroundZ` ⇒ 一律落在 `BaseZ` 加上本座的解析高度；空 `SampleRoadWeight` ⇒ 道路权重恒 0。
 *
 * ⚠️ 采样器缺席时的落高**不是**平地：裙边摆件的整个意义就是站在坡上，退回 `BaseZ` 会让
 * 单测里那一圈锚点全落在同一个 Z 上，"斜坡上的落高"那条断言于是变成空判据。
 * 所以缺席时用解析场补上 —— 它与镜像本来就是同一个公式的两侧（`CpuGpuFieldParity` 守着）。
 */
struct FSite
{
	TArray<FShaperRing> Rings;
	/** 地面基面的世界 Z（`ACSGroundActor` 的 actor Z）。 */
	double BaseZ = 0.0;

	TFunction<float(const FVector2D&)> SampleGroundZ;
	TFunction<float(const FVector2D&)> SampleRoadWeight;
};

/** actor 名 → 稳定键。两侧（产线与测试）必须走同一个函数，否则身份口径会分叉。 */
COMPUTESHADERGENERATOR_API uint32 RingKey(const FString& ShaperName);

/**
 * 锚点生产者（第五家）。读世界、不碰 GPU。
 *
 * 产出顺序钉死为「环按 `Key` 升序 → 环上按角序号」—— `BuildPlan` 的最小间距测试天然顺序
 * 相关（先放的挤掉后放的），而 `FSite::Rings` 的来源（世界扫描 + 登记）顺序不可靠。
 */
COMPUTESHADERGENERATOR_API void BuildSkirtAnchors(const FSite& Site, const CSHouseDecor::FParams& Params,
	TArray<CSHouseDecor::FAnchor>& OutAnchors);

/**
 * 容量上限（每个 palette）。**与规划无关**：它是"这批塑形物再怎么被道路/邻座削也装得下"的那个数
 * —— 每座按整圈算，一件都不减。
 *
 * ⚠️ 它是半径的**连续函数**（周长 / 间距），所以调用方**必须**再把它过一遍
 * `CSShaperSteps::ReserveCount`（×1.5 对齐 4096）：`ReserveCapacity` 只对齐到 64，
 * 拖半径时每涨过一个间距就重新分配一次。藤蔓那轮漏了这一步，实测一段拖动 21 次阻塞刷新。
 */
COMPUTESHADERGENERATOR_API int32 MaxRecordsBound(const FSite& Site, const CSHouseDecor::FParams& Params);

/** 一圈锚点的名义半径（= `Radius + Falloff × SkirtBandT`）。上界、包围盒、断言共用这一份。 */
COMPUTESHADERGENERATOR_API float RingRadius(const FShaperRing& Ring, const CSHouseDecor::FParams& Params);
}

#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers / ReserveCapacity / ReleaseOnRenderThread
#include "CSHouseProfile.h"        // FCSWallOpening
#include "CSHouseRoof.h"           // 屋面求值器 —— 檐口/屋脊的高度**只能**从这里取
#include "CSHouseVine.h"           // FWallStrip：一面墙的外皮矩形，口径只允许有一份


/**
 * 装饰摆件的**锚点层**（TinyGladeHouse D12 的一半）。
 *
 * -----------------------------------------------------------------------------
 * 这一半为什么可以先做，另一半为什么不在这里
 * -----------------------------------------------------------------------------
 * 计划 D12 写的是「复杂度场 `RT_DecorField` + tile-argmax」，而实测 Tiny Glade 里**没有
 * 对位物**：TG 的 `system_clutter::autoclutter` 是**七家锚点生产者**（围着已放的窗/门/树/
 * 水/屋顶长）＋**候选点烘在资产里**（`*_flowerbed_locations` 是零三角形的纯点集），密度由
 * **锚点数量**决定，全程不存在任何场。两套方案的取舍是**挂起的决策 C2**
 * （`Docs/TinyGlade/Progress_ModuleComparison.md（卷零）`「待用户拍板」），本模块**只实现锚点这一半** ——
 * 那是 TG 的做法，也是两个候选方案共同的地基。**这里不实现复杂度场，也不替 C2 下结论。**
 *
 * -----------------------------------------------------------------------------
 * 锚点 ≠ 撒点：为什么"密度"这个词在这里指的是锚点个数
 * -----------------------------------------------------------------------------
 * 一件摆件对应**一个**锚点（不是"一个锚点上摆 N 件"），所以摆件多少完全由构件多少决定：
 * 多开一个门 ⇒ 门那一家多两个锚；房子拉长 ⇒ 墙脚那一家多几个锚。这正是 TG 的性质
 * （"有几扇窗就有几个花箱位"），也是它与场方案最本质的差别 —— 场能回答"房子周围该有多热闹"，
 * 锚点只能回答"这个构件该配点什么"。把两件事写进同一个函数会让 C2 无从选择。
 *
 * -----------------------------------------------------------------------------
 * 窗户那两家（`add_autoclutter_around_windows` / `add_autoclutter_on_windows`）**不实现**
 * -----------------------------------------------------------------------------
 * 它们要的是"已放置的窗"，而 D8 的窗户主体（`ACSWindowMarker`）还没落地 —— 窗一扇都长不
 * 出来，围着窗的锚点自然也长不出来。（**C1 已于 2026-08-30 拍板选甲**：谓词降维成一维 S
 * 区间，那条"谓词说能放、几何砌不出"的阻塞已经解除，剩下的纯粹是标记 actor 没写。）
 * `EFamily::Window` 与 `BuildAnchors` 里那个空分支是**有意留的接口**：窗户落地之后，
 * 那一家只需要把 `Type == Window` 的洞按对照文档 §5.2 实测的
 * `_flowerbed_locations` 候选点表展开即可（那 6 个网格是零三角形点集、导入器会丢，坐标硬编）。
 *
 * -----------------------------------------------------------------------------
 * 第五家（塑形物裙边）的生产者**不在本文件里**
 * -----------------------------------------------------------------------------
 * `EFamily::Skirt` 的锚点由 `CSGroundDecor.h` 生产、由 `ACSGroundActor` 持有 ——
 * 归属的完整依据写在那个头文件里，一句话是：**塑形物只负责高度场，地面负责派生几何**。
 * 本文件这一侧只多了两样东西：枚举里的一格，以及 `FParams` 里那四个 `Skirt*` 旋钮
 * （填充概率必须与其余家族同表，`BuildPlan` 拿家族当下标查它）。
 *
 * 分界因此是「**生产者按载体分家，规划一律共用**」：`BuildAnchors`（房子四家）与
 * `CSGroundDecor::BuildSkirtAnchors`（地面一家）各读各的世界，产出同一种 `FAnchor`，
 * 之后 `BuildPlan` / `Pack` / `IdentityHash` **只有这一份**。这不是为了省代码 ——
 * 「一个锚点最多一件」「最小间距先放的挤掉后放的」「随机源是身份不是槽位」这三条纪律
 * 只要有第二份实现，就一定会有一天只在其中一份里成立。
 *
 * -----------------------------------------------------------------------------
 * 用户否决派生物（TG 的 `DeletedAutoClutter` + `ui_convert_autoclutter_to_decoclutter` 钉住）
 * -----------------------------------------------------------------------------
 * **本轮不实现**：它与楼梯 A6 的 `SuppressedDerived`（`StairsRemovedSupports`）是同一形状的
 * 第二个实例，两者已建议合并成**一次**裁决（都触碰「派生物纯函数、不序列化」这条反复裁决过
 * 的原则）。位置已经留好了 —— `IdentityHash()` 算出来的那个 uint **就是**将来抑制集要用的键：
 * 它只由"锚在哪个构件上、这家的第几号"决定，不含位置也不含数组下标，所以玩家删掉的那一件在
 * 房子被推拉、道路被重画之后仍然是同一个 id。裁决落下来之后，这里只需要在 `BuildPlan` 里加
 * 一次集合查询，不需要动身份的定义。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 逐实例随机**必须**由身份哈希给，绝不能取 `InterlockedAdd` 的槽位
 * -----------------------------------------------------------------------------
 * 槽位由线程组完成顺序决定 ⇒ 同一份世界状态两次散布可以把同一件摆件放进不同槽 ⇒ 材质拿它做
 * 颜色/尺寸变化时"画一笔路全场变色"，**而且不会有任何断言报红**（S1 已经栽过一次）。
 * 这里的记录由 CPU 排定、线程 i 写第 i 行，槽位恒等于记录序号，所以随机数可以由记录自带。
 */
namespace CSHouseDecor
{
/**
 * 锚点家族 = **谁把它生出来的**，不是"摆的是什么东西"。
 * 与 TG 的七家生产者一一对应（对照文档 §5.1 的 `[PDB]` 表）。
 */
enum class EFamily : uint8
{
	/** 门/拱：洞两侧的墩上 + 门前引道两侧。TG 的 `add_autoclutter_around_gates`。 */
	Gate = 0,
	/** 墙脚：四条墙的底边。TG 侧最近的是 `add_preplaced_autoclutter`（开局预置）。 */
	WallFoot = 1,
	/** 檐口下沿。TG 的 `add_birdnests`（读集里有 `Query<&Roof>`）。 */
	Eave = 2,
	/** 屋脊。同上一家，只是锚在脊线而不是檐口。 */
	Ridge = 3,
	/** **预留不实现** —— 等 C1 拍板、D8 长出窗，见文件头。 */
	Window = 4,
	/**
	 * 塑形物裙边。**生产者不在本文件里** —— 它归地面（`CSGroundDecor.h`），理由见那个头文件。
	 * TG 侧没有严格对位物：七家生产者里最近的是 `add_preplaced_autoclutter`（开局预置），
	 * 而"围着土台摆一圈"在 TG 里是玩家自己放的 decoclutter。**这一家是自有的，别标成抄 TG。**
	 */
	Skirt = 5,
	Count = 6,
};

/**
 * 一个锚点 = 一件摆件的位置与朝向。
 *
 * ⚠️ **锚点在产出时就已经落好高度、也已经过了道路排除** —— 那两件事要读地面镜像，
 * 而 `BuildPlan` 必须是能进纯 CPU 单测的纯函数。分界因此划在这里：
 * 生产者（`BuildAnchors`）读世界，规划（`BuildPlan`）只读锚点。
 */
struct FAnchor
{
	FVector Location = FVector::ZeroVector;   // 世界，摆件的落点（已落高）
	FVector Facing = FVector::ForwardVector;  // 世界单位，背离构件的水平方向
	FVector Up = FVector::UpVector;           // 世界单位
	EFamily Family = EFamily::WallFoot;
	/**
	 * 稳定身份。**必须由构件的几何导出，不能用数组下标** —— 洞表按边遍历序排，
	 * 在 0 号边多开一个门会把后面所有洞的下标推一位，于是全屋摆件重掷一遍。
	 * 几何导出的 id（边号 + 量化后的沿边位置）只在那个门自己动了的时候才变。
	 */
	int32 AnchorId = 0;
};

/**
 * 形态参数。**默认值即出厂值** —— 与藤蔓那份「不留默认策略、全部由 actor 喂进来」有意不同：
 * 这里有十几个旋钮，全塞进细节面板没人会调，也会把真正要调的那五六个淹掉。
 * `ACSHouseActor` 只暴露改观感的那几个，其余就以这里为准。
 */
struct FParams
{
	// --- 门/拱（TG 的 add_autoclutter_around_gates）---------------------------
	/** 从洞缘再往外多远起摆。小于半个摆件宽就会把箱子摆进门洞的净空里。 */
	float GateSideOffset = 70.0f;
	/** 离墙面多远（沿墙外法线）。 */
	float GateStandOff = 55.0f;
	/** 门前引道两侧的锚：离门多远、横向岔开多少。**这就是"道路两侧"那一家** —— 它锚在门上，
	 *  而门本身是道路推导出来的（D6），所以不需要、也不允许去扫描道路栅格找路缘。 */
	float GateApproachDistance = 250.0f;
	float GateApproachSpread = 165.0f;
	float GateFillChance = 0.9f;

	// --- 墙脚 -----------------------------------------------------------------
	float WallFootSpacing = 150.0f;
	float WallFootStandOff = 48.0f;
	float WallFootFillChance = 0.65f;
	/** 洞外扩多少算"占住了这段墙脚"。门口正前方要留净空，不然一开门就撞上一堆桶。 */
	float WallFootHoleClearance = 130.0f;

	// --- 檐口 / 屋脊（TG 的 add_birdnests）------------------------------------
	float EaveSpacing = 220.0f;
	float EaveFillChance = 0.5f;
	float RidgeSpacing = 250.0f;
	float RidgeFillChance = 0.4f;
	/** 沿屋面法线抬起一点，避免与瓦面 z-fighting。 */
	float RoofStandOff = 5.0f;

	// --- 塑形物裙边（生产者在 `CSGroundDecor.h`，归地面）------------------------
	//
	// 参数放在这张表里而不是另开一份，是因为 `FillChance` 必须对**所有**家族有定义 ——
	// `BuildPlan` 是五家共用的那一段，它拿家族当下标查填充概率。既然填充概率非放不可，
	// 几何量跟着它放在同一处，比"三个旋钮在这里、两个在别的头文件"少一个能配错的地方。

	/** 裙边上锚点之间的**弧长**间距 cm。锚点数 = 环周长 / 它 ⇒ 台子越大摆件越多（D12 的密度口径）。 */
	float SkirtSpacing = 260.0f;
	/**
	 * 锚点落在裙边的哪一档：0 = 台顶边缘（`Radius`），1 = 裙边外沿（`Radius + FalloffDistance`）。
	 *
	 * 这个 T 就是解析剖面的**等值参数**（`CSGroundShaperField::EvalShaper` 里的
	 * `T = Skirt / A.W`），所以一圈锚点正好落在同一条等值带上 —— 台子拉大拉高时它们仍在
	 * 剖面的同一个位置，不需要重新调。0.62 偏外沿：0.5 是坡最陡的一档（smoothstep 的拐点），
	 * 摆件在那里读起来像挂在墙上；再往外坡度已经缓下来，东西站得住。
	 */
	float SkirtBandT = 0.62f;
	/**
	 * 沿 −Z 埋进地表多少 cm。
	 *
	 * 斜坡上摆件的底面与地表只在**一点**相切，不埋的话下坡那半边是悬空的（石阶埋深那条
	 * 教训的同型，只是这里不需要闭式解 —— 摆件不是块，坡度也不由本模块定，取个保守常数即可）。
	 * 12 cm 是按 `barrel` 级别（底面直径约 106 cm × `BaseScale`）在裙边中段坡度上量的量级。
	 */
	float SkirtEmbed = 12.0f;
	float SkirtFillChance = 0.55f;

	// --- 通用 -----------------------------------------------------------------
	/** 道路权重过阈就不摆。TG 的对位物是 `PathRaster` 那条 mask 订阅（对照文档 §5.2 第 4 条）。 */
	float RoadReject = 0.3f;
	/** 摆件之间的最小间距（球测）。计划 D12 的「同类间距球」在只有一层锚点时退化成这一条。 */
	float MinSpacing = 90.0f;
	float BaseScale = 1.0f;
	float ScaleJitter = 0.16f;
	/** yaw 抖动（弧度）。TG 的 clutter 大多轴对称，正面朝向留给窗户那家的候选点。 */
	float YawJitter = 0.6f;
	int32 Seed = 7;

	float FillChance(EFamily Family) const;
};

/** 一条实例记录 = 3 个 float4，与 `CSHouseDecor.usf` 文件头的布局逐字对应。 */
struct FRecord
{
	FVector3f WorldPos = FVector3f::ZeroVector;
	float Scale = 1.0f;                                 // 横向（XY）缩放
	FVector3f Facing = FVector3f(1.0f, 0.0f, 0.0f);     // 单位，已含 yaw 抖动
	float Random01 = 0.0f;                              // 身份哈希导出，材质用它做逐实例微差
	FVector3f Up = FVector3f(0.0f, 0.0f, 1.0f);
	float ScaleZ = 1.0f;                                // 竖向缩放（与 Scale 相乘）
};

/** 每一家能用 palette 的哪一段。TG 的对位物是每个生产者各自的 `ClutterMeshes` 读集。 */
struct FPaletteRange
{
	int32 First = 0;
	int32 Count = 0;
};

/** 一次规划的产物：逐 palette 的记录（一个 palette = 一张网格 = 一个实例化组件）。 */
struct FPlan
{
	TArray<TArray<FRecord>> ByPalette;

	int32 TotalRecords() const
	{
		int32 Sum = 0;
		for (const TArray<FRecord>& One : ByPalette) Sum += One.Num();
		return Sum;
	}
	void Reset(int32 PaletteCount)
	{
		ByPalette.Reset();
		ByPalette.SetNum(FMath::Max(PaletteCount, 0));
	}
};

/**
 * 生产锚点要读的世界。两个采样器可以为空（纯 CPU 单测就不传）：
 * 空 `SampleGroundZ` ⇒ 一律落在 `BaseZ`；空 `SampleRoadWeight` ⇒ 道路权重恒 0（谁都不排除）。
 */
struct FSite
{
	/** 每面墙的外皮条带（折线有几条边就几条）。**与藤蔓、房体面板同一份 `CSHouse_GetEdge`** —— 墙在哪儿只能有一个真源。 */
	TArray<CSHouseVine::FWallStrip> Strips;
	TArray<FCSWallOpening> Openings;
	/** 屋面。檐口/屋脊的高度一律过 `CSHouseRoof_*` 求值器，**不在这里另写屋顶方程**（计划 D4）。 */
	FCSRoofDesc Roof;
	/** 局部 → 世界（只取 yaw 的那份，与 `BuildVineStrips` 用的是同一个变换）。 */
	FTransform World = FTransform::Identity;
	/** 房底世界 Z。地面采样器缺席时的落高兜底。 */
	double BaseZ = 0.0;

	TFunction<float(const FVector2D&)> SampleGroundZ;
	TFunction<float(const FVector2D&)> SampleRoadWeight;
};

/**
 * 身份哈希：(家族, 锚点 id, 佐料, 用户种子) → uint。
 *
 * ⚠️ **这就是"不许用槽位"那条纪律的执行面**，也是将来 `DeletedAutoClutter` 抑制集的键
 * （见文件头）。身份里刻意**不含位置**：拖房子时墙面在动，位置派生的种子会让整片摆件在
 * 拖动过程中不停重掷，而它们本该只是跟着墙平移。
 */
COMPUTESHADERGENERATOR_API uint32 IdentityHash(EFamily Family, int32 AnchorId, uint32 Salt, int32 Seed);

/** uint → [0,1)，与 `CSGpuInstancedMesh.usf:120-126` 的收尾段同一份数学。 */
COMPUTESHADERGENERATOR_API float Hash01(uint32 H);

/**
 * 锚点生产者（四家）。读世界、不碰 GPU。产出顺序**钉死为构件遍历序**
 * （家族 → 边号 → 沿边序号），因为 `BuildPlan` 的最小间距测试天然顺序相关
 * （先放的挤掉后放的）—— 计划 D12「层内处理顺序钉死为格坐标字典序」是同一条纪律。
 */
COMPUTESHADERGENERATOR_API void BuildAnchors(const FSite& Site, const FParams& Params, TArray<FAnchor>& OutAnchors);

/**
 * 纯函数：锚点 + 参数 + 每家的 palette 段 → 记录。**不碰任何 GPU 资源与世界，可以在纯 CPU
 * 单测里跑。** 摆件最容易错的两件事（会不会挡在门口、同一输入两次散布是否逐位相同）都只能在
 * 这一层断言 —— 到了 GPU 那一侧就只剩一个实例计数了。
 *
 * `Ranges` 按 `EFamily` 下标索引，长度必须是 `EFamily::Count`。
 */
COMPUTESHADERGENERATOR_API void BuildPlan(const TArray<FAnchor>& Anchors, const FParams& Params,
	const TArray<FPaletteRange>& Ranges, int32 PaletteCount, FPlan& OutPlan);

/**
 * 容量上限（每个 palette）。**纯配置量，与规划无关**：它必须是"不管世界长成什么样都装得下"
 * 的那个数，否则交互期的某一帧会突然付一次设备同步。
 *
 * ⚠️ 只覆盖**房子那四家** —— 裙边那家的载体是塑形物，房子对它一无所知，
 * 上界由 `CSGroundDecor::MaxRecordsBound` 另算（同一条纪律，不同的输入）。
 *
 * ⚠️ 它是 `Footprint` 的**连续函数**（周长 / 间距），所以调用方**必须**再把它过一遍
 * `CSShaperSteps::ReserveCount`（×1.5 对齐 4096）—— 藤蔓那轮就是漏了这一步：`ReserveCapacity`
 * 只对齐到 64，拖尺寸时每涨过一根藤的间距就重新分配一次，实测一段拖动累计 21 次阻塞刷新。
 */
COMPUTESHADERGENERATOR_API int32 MaxRecordsBound(const FCSHouseFootprint& Footprint, float Overhang, float PierWidth,
	const FParams& Params);

/**
 * 录一趟打包 pass：逐 palette 上传记录 → 一个 dispatch 写满 packed 行 + counter。
 *
 * **录完直接返回，不阻塞**（Palettes 按值捕获，`TRefCountPtr` 拷贝即加引用）。前置条件是
 * `CSShaperSteps::ReserveCapacity` 已经把容量备好 —— 这里只用现有容量、一个字节都不分配。
 * 容量不够时**截断**而不是扩容：少摆几件远好过在拖动的某一帧里付一次设备同步。
 */
COMPUTESHADERGENERATOR_API bool Pack(const FPlan& Plan, const TArray<CSShaperSteps::FPaletteBuffers>& Palettes,
	const FMatrix44f& WorldToComponent);
}

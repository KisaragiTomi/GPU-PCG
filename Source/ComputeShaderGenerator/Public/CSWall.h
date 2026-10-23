#pragma once

#include "CoreMinimal.h"
#include "CSHouseFrame.h"     // FElement / FBrickParams / AppendFlatRun —— 墙顶砖与门框砖同一条砖路
#include "CSHouseProfile.h"   // FCSWallOpening / FCSWallCut / 裁剪场 / FCSHouseFootprint / 边框架与角框架 —— 洞与斜接的真源
#include "CSHouseQuoin.h"     // FQuoin / BuildQuoinElements —— 墙端、墙角的角石
#include "CSHouseVine.h"      // FWallStrip —— 藤的墙面

struct FCSGpuMeshCPUData;

/**
 * 墙（TG `PublicWalls` 那一层）的**纯函数核心** —— 2026-09-22「房子变成墙」，计划见
 * `Docs/TinyGlade/Plan_Wall.md`。
 *
 * TG 里实体只有墙：`InnerWalls` → `publish_public_wall_state` → `PublicWalls`，屋顶是另挂的实体，
 * "建筑"只是"围合的墙 + 屋顶"的派生叫法。这一层把**墙自己的那部分**按"一条路径（开口或闭合）"描述：
 *
 *   路径（中线折线 + 逐点墙脚 / 墙顶高） → 两条皮线（斜接） → 墙体三角汤 / 墙顶砖 / 角石 / 藤的墙面
 *
 * **两种墙，同一套函数**（2026-09-22 晚「统一房子和墙的逻辑」起，房子与样条墙都走这里）：
 *
 * | | 样条墙 `ACSWallActor` | 房子 `ACSHouseActor` |
 * | --- | --- | --- |
 * | 路径来源 | 样条 = 中线（`BuildSkins`） | footprint = 外皮（`BuildEnclosureRing`） |
 * | 空间 | 世界 | 局部 + `World`（`GetBuildTransform()`） |
 * | 墙的种类 `ECSWallKind` | `Freestanding`：两面都是外面 | `Enclosure`：右皮朝外、左皮朝屋里 |
 * | 墙体表面 `ECSWallSurface` | `Continuous`：连续 UV、贴地、弯墙平滑 | `Faceted`：逐面板实心斜接棱柱（房子一贯的画面） |
 * | 墙顶 | 压顶 + 垛口（`BuildTopElements`） | **平顶**（坡屋顶下 TG 的墙就是墙本身的顶面；平屋顶的露台垛口归屋顶，`CSHouseRoof_BuildParapet`） |
 * | 洞 / 接缝 | 未接（计划第 2 / 5 步） | 门、窗、墩、接缝裁剪 —— 面板规划器只有一份 |
 *
 * ⚠️ **房子走这里之后画面逐位不变**：`Faceted` 与 `Enclosure` 的每一条公式都是从房子原代码逐字搬来的
 * （`Wall.HouseBodyMatchesLegacy` / `Wall.HouseQuoinsAndVinesMatchLegacy` 拿冻结的旧实现逐个 float 对照）。
 *
 * 与房子其它模块的关系（别另起炉灶）：
 *   · 斜接口径与 `CSHouse_CornerInset` / `FCSHouseCornerFrame::PointAtDepth` 同一个式子；
 *   · 墙顶砖、角石走 `CSHouseFrame` 那条 GPU 解析砖路（与门框砖 / 包边 / 角石同一个组件、同一个 kernel）；
 *   · 藤走 `CSHouseVine::BuildPlan`，墙面是 `FWallStrip`（平面或折线形态）；
 *   · 顶点色 / UV1 口径与房体同一张通道字典（`ACSHouseActor` 类注释），墙材质 `MI_TinyGladeWall` 直接能画。
 */
struct COMPUTESHADERGENERATOR_API FCSWallPath
{
	/** 墙厚中线的折线（XY）。闭合时首尾自动相连，不要重复首点。 */
	TArray<FVector2D> Points;
	/** 逐点墙脚高（Z），与 `Points` 等长。 */
	TArray<double> BaseZ;
	/** 逐点墙顶高（Z），与 `Points` 等长。 */
	TArray<double> TopZ;
	/**
	 * 逐点标记：1 = 这个点来自样条控制点 / footprint 顶点。可空（= 全 0）。**只做记录，不参与判角** ——
	 * 拐角一律按相邻两段的夹角判（`CSWall::IsCorner`，2026-09-22 用户："线段夹角过大时就不会产生转角，
	 * 而是产生圆滑的墙"），与点是不是控制点无关。
	 */
	TArray<uint8> Kinks;
	bool bClosed = false;

	int32 NumPoints() const { return Points.Num(); }
	int32 NumSegments() const
	{
		const int32 N = Points.Num();
		return bClosed ? (N >= 3 ? N : 0) : FMath::Max(N - 1, 0);
	}
	int32 NextIndex(int32 Index) const { return (Index + 1) % Points.Num(); }
	bool IsKink(int32 Index) const { return Kinks.IsValidIndex(Index) && Kinks[Index] != 0; }

	bool IsValid() const
	{
		const int32 N = Points.Num();
		return N >= (bClosed ? 3 : 2) && BaseZ.Num() == N && TopZ.Num() == N && (Kinks.IsEmpty() || Kinks.Num() == N);
	}

	/** 第 `Segment` 段的水平长度。 */
	double SegmentLength(int32 Segment) const
	{
		return FVector2D::Distance(Points[Segment], Points[NextIndex(Segment)]);
	}

	/** 中线全长（水平）；闭合时含回到首点那一段。 */
	double TotalLength() const
	{
		double Sum = 0.0;
		for (int32 Segment = 0; Segment < NumSegments(); ++Segment) Sum += SegmentLength(Segment);
		return Sum;
	}
};

/** 路径的两条皮线（斜接）与逐点法线。由 `CSWall::BuildSkins` / `BuildEnclosureRing` 派生，别手填。 */
struct COMPUTESHADERGENERATOR_API FCSWallSkins
{
	/** 行进方向**左手**那条皮线（XY），逐点，与路径同号。逆时针的闭合墙上它是**内**皮。 */
	TArray<FVector2D> Left;
	/** 右手那条皮线。逆时针的闭合墙上它是**外**皮（房子的 footprint 就是它）。 */
	TArray<FVector2D> Right;
	/**
	 * 逐点右皮的外法线（单位、水平）= 两段右法线的平分方向；左皮外法线取反。
	 * 开口端点就是那一段自己的右法线。
	 */
	TArray<FVector2D> Normal;
	/** 逐点"转角"正弦：> 0 左转、< 0 右转、端点 0。判角石、判平滑法线都用它。 */
	TArray<double> TurnSin;
	/** 逐点 `cos(转角/2)`（端点 1）。 */
	TArray<double> HalfTurnCos;
	/** 中线累计弧长：`S[0] = 0`，逐点。 */
	TArray<double> S;
	/** 中线全长（闭合时含回到首点那一段）。 */
	double Length = 0.0;
	float Thickness = 0.0f;

	bool IsValid() const { return Left.Num() >= 2 && Left.Num() == Right.Num() && Right.Num() == Normal.Num(); }

	/** 转角的度数（端点 0）。 */
	double TurnDegrees(int32 Index) const
	{
		return FMath::RadiansToDegrees(2.0 * FMath::Acos(FMath::Clamp(HalfTurnCos[Index], 0.0, 1.0)));
	}
};

/**
 * 墙的种类 —— TG 的 `default_building_color_id` / `default_freeform_wall_color_id` 那一分：同一种墙，两套规矩。
 */
enum class ECSWallKind : uint8
{
	/**
	 * 独立墙（样条墙）：两面都是外面。墙端出角石；拐角按 `CornerTurnDegrees` 判、凸侧（哪一面凸就是哪一面）出角石；
	 * 藤两面都长，一面是一整条折线带（弯墙不断开），开口墙在墙端绕过端面。
	 */
	Freestanding,
	/**
	 * 围合的墙（房子）：右皮朝外、左皮朝屋里（逆时针闭合环）。只有外皮的凸角出角石（凹角 / 锐角不出，理由见
	 * `CSHouseQuoin`），任一边放不下两头的斜接让出量 ⇒ 整圈不出；角号 = 房子的口径（k 号角在 k+1 号顶点上）；
	 * 藤只长外皮、逐边一条平面带（屋里不长藤）。
	 */
	Enclosure,
};

/** 墙体三角汤的出面方式（面板规划器只有一份，出面方式二选一）。 */
enum class ECSWallSurface : uint8
{
	/**
	 * 房子一贯的画面：每块面板一个实心斜接棱柱（六面封闭，UV 每个面从 0 起、内皮转 90°），法线逐面。
	 * 墙基平、墙高恒定 —— 取每条边起点的墙脚 / 墙顶。面板的 S 是**外皮**弧长（`CSHouse_GetEdge` 的口径）。
	 */
	Faceted,
	/**
	 * 独立墙：侧面 UV 沿中线弧长连续、墙脚 / 墙顶逐点跟地形、非拐角接点平滑法线、端面只在开口路径的两头。
	 * 面板的 S 是**中线**弧长（段内）。
	 */
	Continuous,
};

namespace CSWall
{
/**
 * 斜接长度上限：皮线外角沿平分线最多伸出**半个墙厚的这么多倍**。`1/cos(转角/2)` 在近 180° 的折返处
 * 发散，一根尖刺会捅出去几米 —— 样条上把点拖回头就能画出这种角。3 倍对应约 141° 的转角，比这更尖的
 * 角被削平（两条皮线在那里不再平行，墙在尖角处略薄）。只管 `BuildSkins`；房子的环不封顶（与 `CSHouse_CornerInset` 同口径）。
 */
inline constexpr double MaxMitre = 3.0;

/** 世界 cm → UV 的平铺周期。与房体同一个值：同一张墙材质，同一个贴图密度。 */
inline constexpr float UVScale = 200.0f;

/** 默认的拐角阈值（度）：相邻两段的转角小于它 = 圆滑的墙，大于等于它 = 拐角。 */
inline constexpr float DefaultCornerTurnDegrees = 30.0f;

/**
 * **拐角的唯一判据**：相邻两段的转角 ≥ `CornerTurnDegrees`（即两段的夹角 ≤ 180° − 它）。
 * 夹角大（转得缓）的接点就是圆滑的墙：侧面共用平分线法线、不出角石、墙顶砖不断开、藤的法线照常插值；
 * 转得急的接点才是拐角：硬棱、凸侧出角石（不比直角尖时）、墙顶砖在此断开、藤两侧各用各的面法线。
 * 墙体 / 墙顶砖 / 角石 / 藤四处都问这一条，阈值只有一个 —— 两个阈值会出现"墙面是圆的、砖却断开了"。
 * 与点是不是样条控制点无关（`FCSWallPath::Kinks` 只做记录）。开口路径的两个端点恒不是拐角。
 */
inline bool IsCorner(const FCSWallSkins& Skins, int32 Index, float CornerTurnDegrees)
{
	const double Threshold = FMath::Cos(FMath::DegreesToRadians(double(FMath::Clamp(CornerTurnDegrees, 0.0f, 179.0f))) * 0.5);
	return Skins.HalfTurnCos.IsValidIndex(Index) && Skins.HalfTurnCos[Index] < Threshold;
}

/**
 * 路径（中线）→ 两条皮线（**纯函数**，样条墙的路径来源）。墙以中线为准、两边各偏 `Thickness / 2`，接点处沿
 * 平分线斜接（与 `FCSHouseCornerFrame::PointAtDepth` 同一个式子），所以相邻两段的墙面在接缝上严丝合缝。
 * 返回 false = 路径退化（点不够、零长段太多、`Thickness <= 0`）。
 */
COMPUTESHADERGENERATOR_API bool BuildSkins(const FCSWallPath& Path, float Thickness, FCSWallSkins& Out);

/**
 * footprint（**外皮**）→ 闭合路径 + 皮线（**纯函数**，房子的路径来源）。footprint 是逆时针的外皮环，墙往里长一个墙厚。
 *
 * 逐点的值**原样取自** `CSHouse_GetCorner`（房子"角在哪"的唯一真源，k 号角在 k+1 号顶点上）：
 * 右皮 = 顶点本身、左皮 = `PointAtDepth(T)`、中线 = `PointAtDepth(T/2)`、法线 / 转角 = 角框架的平分线与转角 ——
 * 不从中线再算一遍，所以下游拿右皮现组的边框架与 `CSHouse_GetEdge` **逐位相同**。
 * 墙脚 / 墙顶逐点取 `BaseZ` / `TopZ`（房子：局部 0 与 `WallHeight`）。顶点不足 3 个返回 false。
 */
COMPUTESHADERGENERATOR_API bool BuildEnclosureRing(const FCSHouseFootprint& Outer, float Thickness, double BaseZ, double TopZ,
	FCSWallPath& OutPath, FCSWallSkins& OutSkins);

/**
 * 第 `Segment` 段**外皮（右皮）**的斜接边框架：`Start` = 右皮起点、`Len` = 右皮段长、`InsetStart` / `InsetEnd` = 内皮在两头让出的量。
 * 与 `CSHouse_MakeEdgeFrame` 同一个核（房子的环上与 `CSHouse_GetEdge` 逐位相同）；开口路径的两个墙端是方头（让出 0）。
 */
COMPUTESHADERGENERATOR_API FCSHouseEdgeFrame OuterEdgeFrame(const FCSWallPath& Path, const FCSWallSkins& Skins, int32 Segment);

/** 路径在中线弧长 S 处的点、墙顶高与切向。闭合路径 S 绕回。 */
COMPUTESHADERGENERATOR_API void EvalCenter(const FCSWallPath& Path, const FCSWallSkins& Skins, double S,
	FVector2D& OutXY, double& OutTopZ, FVector2D& OutTangent);

struct FBodyParams
{
	ECSWallSurface Surface = ECSWallSurface::Continuous;
	/** 写进快照时的变换（路径所在空间 → 世界）。样条墙的路径本来就在世界里（恒等）；房子传 `GetBuildTransform()`。 */
	FTransform World = FTransform::Identity;
	/**
	 * 洞（几何上不挖，按 UV1 逐像素 clip —— 2026-08-30 裁决三）。`EdgeIndex` = 路径段号；
	 * `CenterS` = 段内弧长（`Faceted` 量外皮、`Continuous` 量中线）。
	 */
	TArrayView<const FCSWallOpening> Openings;
	/** D7 接缝要抹掉的段（同样逐像素 clip），口径同 `Openings`。 */
	TArrayView<const FCSWallCut> SeamCuts;
	/** 相邻洞之间保留的墩宽 cm。 */
	float PierWidth = 40.0f;
	/** `Continuous` 的墙底（朝下）。`Faceted` 的棱柱本来就六面封闭，不看它。 */
	bool bBottomFace = true;
	/** 闭合墙围出的那块地（内皮环、朝下的单面）—— 房子的"房底"（2026-09-17）。要求逆时针的环，顺时针的环自动不铺。 */
	bool bInteriorFloor = false;
	/** 拐角阈值（见 `IsCorner`）：`Continuous` 里不是拐角的接点两段墙面共用平分线法线（弯墙无棱）。 */
	float CornerTurnDegrees = DefaultCornerTurnDegrees;
};

/**
 * 墙体三角汤（**纯函数**，追加写，返回追加的三角形数）—— 房子与样条墙共用的那一份（2026-09-22「统一房子和墙」）。
 *
 * 逐段先过**面板规划器**（房体原来那一整套：洞按格切面板、墩跨度整片裁、接缝段单独成板并合并重叠 ——
 * ⚠️ 任何情况下都不许靠"不生成面板"来开洞，裁决三），再按 `Surface` 出面：
 *   · `Faceted`：逐面板斜接棱柱（`FCSWallMeshWriter::AddWallPrism`），窗台是下面另一块实心棱柱；
 *   · `Continuous`：两个侧面 + 墙顶 + 墙底，开口路径两头封端面；UV0 = (中线弧长, 离墙脚的高度) / `UVScale`。
 * 之后按需铺房底（`bInteriorFloor`）。
 *
 * 通道口径与房体同一张字典：顶点色 = (墙 0, 洞 Tag, 形状 id / 255 = 没有洞, 0)，UV1 = 裁剪场（无洞写哨兵 (8, 8)）。
 * 绕序：面法线按 cross(B−A, C−A) 朝外，索引按引擎绕序（交换 1/2）写。
 */
COMPUTESHADERGENERATOR_API int32 BuildBody(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FBodyParams& Params, FCSGpuMeshCPUData& Out);

struct FTopParams
{
	/** 压顶砖的标称长度 cm（= 砖路调色板的 `BlockSize` 那一份，逐块再按实际弧长缩放）。 */
	float BrickLength = 30.0f;
	/** 一层砖的高度 cm（= 调色板的进深轴，见 `CSHouseFrame::AppendFlatRun` 的轴向说明）。 */
	float CourseHeight = 18.0f;
	/** 压顶砖往墙顶里埋多深 cm：坡上墙顶是斜的，砖是方的，埋一点才不露缝。 */
	float Sink = 3.0f;
	bool bCoping = true;
	/** 垛口：压顶层之上每隔 `MerlonEvery` 块抬一块（TG 截图里那排参差的城齿）。 */
	bool bMerlons = true;
	int32 MerlonEvery = 2;
	/** 拐角阈值（见 `IsCorner`）：墙顶砖在拐角处断开 —— 压顶砖按弦铺，跨过拐角的那块会切进角里。 */
	float CornerTurnDegrees = DefaultCornerTurnDegrees;
	/** 身份种子（逐块随机数从它 + 家族盐 + 块号派生，不从槽位派生 —— 理由同角石）。 */
	uint32 Seed = 0;

	/**
	 * **平顶**：墙顶就是墙体自己的顶面，一块砖都不出。房子的墙就是这一种（2026-08-31 用户裁决「墙的上沿我看过 TG 中是
	 * 没有的，可以去掉」；坡屋顶的檐口压在墙顶上，出砖就会从瓦里戳出来）—— 平屋顶的露台垛口是屋顶的构件，不走这里。
	 */
	static FTopParams Plain()
	{
		FTopParams P;
		P.bCoping = false;
		P.bMerlons = false;
		return P;
	}
	bool IsPlain() const { return !bCoping && !bMerlons; }
};

struct FTopResult
{
	int32 CopingBricks = 0;
	int32 MerlonBricks = 0;
	/** 墙顶被拐角断成了几段。 */
	int32 Runs = 0;
};

/**
 * 墙顶砖 → 砖路元素（**纯函数**，追加写）：压顶一层 + 垛口一层，全是 `CSHouseFrame::AppendFlatRun`。平顶（`FTopParams::Plain()`）一块不出。
 *
 * 逐块排而不是逐面板排：弯墙几十块面板、每块只有半米，按面板排会让砖长逐面板跳变、垛口的奇偶
 * 在每个接点重开。这里按**中线弧长**把每一段（拐角之间）均分成 n 块，每块一条单砖路，框架的
 * `AxisU` 取那块砖两端墙顶点的连线（含坡度），砖因此贴着墙顶的起伏。
 * 开口段的块数取奇数：两端都落垛口，读起来是"收了头"；闭合整圈取偶数，接缝处不出两个相邻的垛。
 * 路径按世界空间读（样条墙）。
 */
COMPUTESHADERGENERATOR_API FTopResult BuildTopElements(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FTopParams& Params, const CSHouseFrame::FBrickParams& BrickParams, TArray<CSHouseFrame::FElement>& InOutElements);

struct FQuoinParams
{
	ECSWallKind Kind = ECSWallKind::Freestanding;
	/** 墙端（开口路径的两头）出不出角石。`Enclosure` 是闭合环，没有墙端。 */
	bool bEnds = true;
	/** 拐角出不出角石。 */
	bool bCorners = true;
	/** 拐角阈值（见 `IsCorner`）。`Enclosure` 传 0：房子的每个凸顶点都是角。 */
	float CornerTurnDegrees = DefaultCornerTurnDegrees;
	/** 包角外棱沿角平分线向内缩的距离 cm（房子的 `QuoinInset`）。 */
	float Inset = 0.0f;
	/** 路径所在空间 → 世界（房子：`GetBuildTransform()`；样条墙：恒等）。 */
	FTransform World = FTransform::Identity;
	/** 柱底 / 柱顶的世界 Z = `ZOffset + float(墙脚 / 墙顶)`（房子：房底的世界 Z，路径里是局部 0 与墙高）。 */
	float ZOffset = 0.0f;
};

/**
 * 角石柱（**纯函数**，追加写，返回柱数）。
 *   · **墙端**（`Freestanding` 开口路径的两头）：端面与两个侧面各夹一个直角 ⇒ 每头两根，`HalfTurnCos = 1/√2`，
 *     编号 100000 + {0,1,2,3}（身份稳定，不随拐角增减漂）；
 *   · **拐角**（`IsCorner`，且不比直角尖 —— 上界与 `CSHouseQuoin::IsQuoinCorner` 相同）：
 *     `Freestanding` 凸侧那条皮线上一根、编号 = 路径点号；`Enclosure` 只出外皮凸角、编号 = 房子的角号（k 号角在 k+1 号顶点上），
 *     按角号顺序出，任一边外皮放不下两头的斜接让出量就整圈不出（`CSHouseQuoin::BuildQuoins` 的规矩，房子逐位不变）。
 */
COMPUTESHADERGENERATOR_API int32 BuildQuoins(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FQuoinParams& Params, TArray<CSHouseQuoin::FQuoin>& Out);

/** 地面采样：给世界 XY，返回 false = 那里没有地面。 */
using FGroundSampler = TFunctionRef<bool(const FVector2D& /*WorldXY*/, float& /*OutWorldZ*/)>;

struct FVineStripParams
{
	ECSWallKind Kind = ECSWallKind::Freestanding;
	/** 路径所在空间 → 世界（只有 `Enclosure` 用；`Freestanding` 的路径就在世界里）。 */
	FTransform World = FTransform::Identity;
	/** 地面空隙的参照高（世界 Z，只有 `Enclosure` 用）：空隙 = 它 − 地面高（房子：房底 = actor 的 Z）。 */
	double GroundRefZ = 0.0;
	/** `Freestanding`：墙脚线 = 墙脚 + `BaseLift`（墙脚埋进地里那么深，藤从地面长起）。 */
	float BaseLift = 0.0f;
	/** `Freestanding`：藤爬到离墙脚线多高为止（整面墙一个值）。`Enclosure` 逐边取那条边的墙高。 */
	float VineHeight = 0.0f;
	/** 地面空隙的采样间距 cm。 */
	float GroundSampleSpacing = 100.0f;
	/** 拐角阈值（见 `IsCorner`，只有 `Freestanding` 用）。 */
	float CornerTurnDegrees = DefaultCornerTurnDegrees;
};

/**
 * 墙面 → 藤的墙面（**纯函数**，覆盖写，返回条数）。
 *
 * `Enclosure`（房子）：外皮逐边一条**平面**带（边号 = 段号），`N` 朝外、`Height` = 这条边的墙高，
 * 地面空隙 = `GroundRefZ` − 地面、逐边按 `GroundSampleSpacing` 采（最多 64 个）—— `ACSHouseActor::BuildVineStrips`
 * 原来那一份，逐位不变。屋里不长藤。
 *
 * `Freestanding`（样条墙）：
 *   · 开口墙绕墙一圈四条：0 = 右皮（顺路径走）、1 = 远端面、2 = 左皮（**倒着**走）、3 = 近端面。
 *     四条都满足 `N = U × Up`（外法线朝外），而且首尾相接成环 —— `BuildPlan` 按边号取模找"隔壁那面墙"，
 *     藤走到墙端就绕过端面上另一面（`JumpChance`），不会从墙体里穿过去。端面只有墙厚那么宽，不起藤。
 *   · 闭合墙两面各自 `bLoop`（S 绕回），边号 0 / 1。
 *   · 不是拐角（`IsCorner`）的接点按平分线插值法线（弯墙是一整面曲面）；拐角两侧各用各的面法线
 *     （同一位置放两个点）—— 在拐角上插值会让整段直墙的法线从 0° 慢慢转到 45°。
 *   · 地面空隙逐 `GroundSampleSpacing` 采：脚下没有地面写 `CSHouseVine::NoGroundGap`（悬空不长藤）。
 */
COMPUTESHADERGENERATOR_API int32 BuildVineStrips(const FCSWallPath& Path, const FCSWallSkins& Skins,
	const FVineStripParams& Params, FGroundSampler Ground, TArray<CSHouseVine::FWallStrip>& Out);
}

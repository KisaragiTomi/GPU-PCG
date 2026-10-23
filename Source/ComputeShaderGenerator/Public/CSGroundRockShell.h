#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"

/**
 * 披挂岩壳（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）—— **稀疏程序化**版本（2026-09-30，
 * 设计与证据：`Docs/TinyGlade/RockShell_Sparse.md`）。
 *
 * **裁决一（2026-09-30 用户改：用程序化方案，M = 16）**：前半句不变 —— 碎裂图案是 2D 的，三维形态
 * 100% 运行时披挂；后半句「静态资产 / 用 TG 原件」换掉：一张不能平铺的原件盖不住大世界。图案现在是
 * 世界锚定的抖动网格 Voronoi（可选低频扭曲 = 成片疏密、功率权重 = 逐粒大小），任意 XY 都能求值。
 *
 * **稀疏**：每个胞腔一个定长槽位（固定拓扑模板 5M 个三角）。CPU 按塑形物的 SDF 判出「可能长石头」的
 * 胞腔（裙边环带，见 `CollectActiveCells`）、分配槽位、上传作业表；GPU 只跑作业表里的槽。
 * 开销 ∝ 石头面积，与世界大小无关；没有塑形物 = 零胞腔、零派发。
 *
 * **裁决二：碎石归地面 actor，不归塑形物。** mask 由**全部塑形物合成后**的坡度决定。
 * **裁决三：解析高度场直接喂。** kernel 调 `CSGroundShaperField.ush` 的 `GroundShaperHeightAtXY`。
 * 与 `CSGroundStairs` 严格互补（石阶要路、碎石要没路），但壳**没有**任何 road 显隐判据，只有连续下沉。
 */
namespace CSRockShell
{
/** TG 原件的胞腔间距（tile 13650 cm / √609 胞腔）。`PatternScale = CellSize / 本值`。 */
constexpr float TGNativeCellSpacingCm = 553.13f;
/** TG 原件底圈相对盖边界的外移（`RockShell_Pattern.md` §3 实测 19.5 cm）。按 PatternScale 缩。 */
constexpr float TGLipCm = 19.5f;
/** 边界点数 M 的范围（usf 的 `ROCKSHELL_MAX_M` 同值）。胞腔实测最多 11 个角点 ⇒ 下限 12。 */
constexpr int32 MinBoundaryPoints = 12;
constexpr int32 MaxBoundaryPoints = 24;
/** 胞腔裁剪的邻域半径（7×7；usf 的 `ROCKSHELL_NEIGHBOUR_RADIUS` 同值，理由写在那里）。 */
constexpr int32 NeighbourRadius = 3;
/** 内环在质心与顶圈之间的位置（usf 的 `ROCKSHELL_RING_FRACTION` 同值）。 */
constexpr float RingFraction = 0.5f;

/**
 * 岩壳网格的**顶点色通道字典 v2**（假倒角三通道的机制与消费端见 `Docs/TinyGlade/RockShell_EdgeBevel.md`）。
 *
 * | 通道 | 含义 |
 * | --- | --- |
 * | R | `bIsCapTri`：**1 = 盖三角（顶盖）/ 0 = 裙三角（侧壁）** |
 * | G | v2 的折痕距离；运行时壳写中性 1（v3 材质不读它） |
 * | B | 逐石头相位（格键哈希），假倒角噪声的域偏移 |
 * | A | v2 的外向角；运行时壳写中性 1（v3 材质不读它） |
 *
 * 为什么是顶点色：它是标准流，`ReadbackResidentSync` 会读、`BuildGpuMeshDescription` 会写成
 * `VertexInstanceColors` ⇒ 烘成 StaticMesh 之后仍在。**中性值必须是 1**：没有色流的网格在材质里读出来是白色。
 * 程序化之后这四个量由 GPU 在分配槽位时写一次（`WriteRockShellStaticCS`），之后每趟披挂不再碰。
 */
namespace VertexColor
{
constexpr int32 CapTriChannel = 0;      ///< R
constexpr float CapValue = 1.0f;        ///< 盖三角（也是"没有色流"时材质读到的中性值）
constexpr float SkirtValue = 0.0f;      ///< 裙三角
/**
 * 消费材质的标量参数名。`ACSGroundActor::EnsureRockShellMesh` 给 `RockShellMaterial` 造一个动态子实例并写入
 * `PatternScale`（= 胞腔尺寸 / TG 原生间距）：假倒角的带宽 / 咬深 / 噪声尺度全是 TG 原生的**图案口径**，
 * 材质用它换算回世界 ⇒ 胞腔缩到 1.94 m 时缺口跟着缩。
 */
inline const TCHAR* PatternScaleParameterName = TEXT("RockShellPatternScale");
}

/**
 * 岩壳 `UCSMesh` 的额外 aux 流槽位。从 32 起：AuxVertex 槽 0 归标准集的逐三角材质 id，
 * `CSGpuInstancedMeshSceneProxy` 那个 leaf 已经占了 16..22。
 * ⚠️ **槽位冲撞不会报错**：`AddStream` 直接返回 false，之后绑上一个空 buffer，日志里什么都没有。
 */
enum class EAuxSlot : uint8
{
	/**
	 * Buffer<float4>，(2 + M) / **槽**（Fixed）：槽的胞腔几何，全部世界 XY ——
	 * [0] = (质心.xy, asfloat(格 I), asfloat(格 J))；[1] = (asfloat(角点掩码), 实际唇宽, asfloat(角点数), 活着)；
	 * [2 + k] = (底圈 B[k].xy, 顶圈 T[k].xy)。只在槽被分配时由 `BuildRockShellCellsCS` 写一次（多边形是静态的）。
	 */
	CellGeom = 32,
};

/**
 * 岩壳 UV 通道字典（假倒角 v3）。通道 0 在分配槽位时写死，**通道 1..6 由倒角载荷趟每趟披挂重写**。
 *
 * 为什么不是顶点色：8 bit 让法线方向按 1.4° 一档跳。为什么必须重写：壳每趟按地形重新披挂，
 * 盖裙夹角随坡度变，烘死的邻面法线与垂距到运行时都是错的。与 TG 同构 ——
 * 原版 `Triangle.normal` 由 `displace_rocky_terrain.cs` 每帧重写，只有 `neighbours` 是烘死的
 * （本版的邻接是模板常量，同样烘死）。
 */
namespace TexCoord
{
constexpr int32 World      = 0;   ///< 世界 XY / UVWorldPeriod，三平面贴图用
constexpr int32 EdgeDist01 = 1;   ///< (d0, d1) 世界 cm：到「对着角 0 / 角 1 的边」的垂距
constexpr int32 EdgeDist2  = 2;   ///< (d2, 0)  世界 cm；y 留白
constexpr int32 NbrNormal0 = 3;   ///< 八面体编码的邻面法线，对应边 0（外轮廓写自身法线 ⇒ 混合退化成不变）
constexpr int32 NbrNormal1 = 4;   ///< 同上，边 1
constexpr int32 NbrNormal2 = 5;   ///< 同上，边 2
/** 八面体编码的**自身**面法线（逐三角常量）：基础法线平滑时材质靠它画 band 两侧的真硬边。 */
constexpr int32 FaceNormal = 6;
constexpr int32 NumSets    = 7;   ///< 壳声明的 UV 组数；上限见 FCSGpuMeshCPUData::MaxTexCoordChannels
}

/**
 * 程序化图案的参数。**改任何一项胞腔多边形都会变** ⇒ 槽位池整体重建（见 `GeometryHash`）。
 * 字段逐个对应 usf 顶部同名 uniform（`RockShellCellSize` 等）。
 */
struct FPatternParams
{
	float CellSize = 193.6f;           // 种子格距 cm（= 0.35 × TG 原生 553.13）
	float SeedJitter = 1.0f;           // 格内抖动 0..1
	float ClusterWarp = 0.5f;          // 成片疏密：扭曲幅度（格）
	float ClusterWavelength = 3.0f;    // 成片疏密：扭曲波长（格）
	float SizeVariance = 0.0f;         // 功率权重半径 / 格距；> 0.4 开始出细条
	uint32 Seed = 1;
	int32 BoundaryPoints = 16;         // M：每胞腔 5M 个三角

	/** 夹到合法范围（CellSize ≥ 20 cm、M ∈ [12, 24] 等）。所有入口先过一遍。 */
	COMPUTESHADERGENERATOR_API FPatternParams Sanitized() const;
	/** 胞腔尺寸相对 TG 原件的缩放。平面上的 TG 长度（唇宽、胀缩、扩张、倾斜、起伏噪声的域）乘它；竖向的厚度 / 基准偏移 / 起伏是世界 cm。 */
	float PatternScale() const { return CellSize / TGNativeCellSpacingCm; }
	int32 TrianglesPerCell() const { return 5 * BoundaryPoints; }
	int32 VerticesPerCell() const { return 15 * BoundaryPoints; }
	int32 GeomFloat4sPerCell() const { return 2 + BoundaryPoints; }
	float LipCm() const { return TGLipCm * PatternScale(); }
	/** 标称胞腔半径（半径淡入用）。 */
	float CellRadiusCm() const { return 0.5f * CellSize; }
	/** 决定胞腔多边形与模板的全部量的哈希（槽位池的身份）。 */
	COMPUTESHADERGENERATOR_API uint32 GeometryHash() const;
};

// ---- CPU 孪生（与 usf 的 CSRockShell_SeedLocal / SeedWeight / BuildRockShellCellsCS 裁剪段逐行对应）----
// CPU 这一份只给**激活**用（要胞腔的外接半径），几何本身由 GPU 算；两侧 float 舍入的差异远小于激活余量。

/** 格 (I, J) 的种子相对本格左下角的偏移（单位 = 格）。 */
COMPUTESHADERGENERATOR_API FVector2f SeedLocal(const FPatternParams& Params, int32 I, int32 J);
/** 功率图权重 cm²。 */
COMPUTESHADERGENERATOR_API float SeedWeight(const FPatternParams& Params, int32 I, int32 J);
/** 种子的世界 XY（格 (0,0) 的左下角在 `GridOrigin`）。 */
COMPUTESHADERGENERATOR_API FVector2D SeedWorld(const FPatternParams& Params, const FVector2D& GridOrigin, int32 I, int32 J);
/** 胞腔多边形：相对种子的局部 cm、逆时针、已合掉重合点。空胞腔（功率图）返回 false。 */
COMPUTESHADERGENERATOR_API bool CellPolygon(const FPatternParams& Params, int32 I, int32 J, TArray<FVector2f>& OutLocal);
/** 种子到多边形角点的最远距离 cm；空胞腔返回 −1。 */
COMPUTESHADERGENERATOR_API float CellCircumradius(const FPatternParams& Params, int32 I, int32 J);

/**
 * 固定拓扑模板（只随 M 变）。模板顶点号：0 = 质心，1+i = 内环，1+M+i = 顶圈，1+2M+i = 底圈；
 * 每个边界点 k 五个三角（俯视顺时针，顺序见 usf 的模板小节）。邻接与入射表是常量，每趟上传一份。
 */
struct FTemplate
{
	int32 M = 0;
	/** 3 / 模板三角：越过「对着角 k 的边」（= 边 (v[k+1], v[k+2])）的局部三角号，−1 = 石头外轮廓（底边）。 */
	TArray<int32> Neighbours;
	/** 2 / 模板角（15M 个）：本角所属模板顶点的入射局部三角在 `IncidentTris` 里的 (偏移, 数量)。 */
	TArray<uint32> IncidentRange;
	/** 展平的入射局部三角号。 */
	TArray<uint32> IncidentTris;

	/** 局部三角 `LocalTri` 的第 `Corner` 个角是哪个模板顶点（usf `CSRockShell_CornerVertex` 的孪生）。 */
	static COMPUTESHADERGENERATOR_API uint32 CornerVertex(int32 M, int32 LocalTri, int32 Corner);
	static bool IsCapTri(int32 LocalTri) { return LocalTri % 5 < 3; }
	/** 模板顶点号的种类：0 质心 / 1 内环 / 2 顶圈 / 3 底圈。 */
	static int32 VertexKind(int32 M, uint32 VertexId)
	{
		if (VertexId == 0u) return 0;
		if (VertexId <= uint32(M)) return 1;
		if (VertexId <= uint32(2 * M)) return 2;
		return 3;
	}
	static COMPUTESHADERGENERATOR_API FTemplate Build(int32 M);
};

/** 进程内缓存的模板（按 M）。游戏线程。 */
COMPUTESHADERGENERATOR_API const FTemplate& GetTemplate(int32 M);

/** 一趟 GPU 作业。三张表都按值搬进渲染命令。 */
struct FJobs
{
	TArray<FUintVector4> NewCells;   // (槽, uint(I), uint(J), 0)：刚分配的槽，先建几何、写静态属性，再披挂
	TArray<uint32> Drape;            // 要重披挂的槽（含新胞腔的槽）
	TArray<uint32> Kill;             // 失效的槽：第 0 个顶点写 NaN
	bool IsEmpty() const { return NewCells.IsEmpty() && Drape.IsEmpty() && Kill.IsEmpty(); }
};

/**
 * 激活：列出**可能**长石头的胞腔（排好序，j 优先）。
 *
 * 判据（推导见 `RockShell_Sparse.md`「二、激活」）：合成高度场的坡只存在于各座的裙边环带
 * `0 < Sd < Falloff`（中心隆起过阈时是整盘）⇒ 胞腔外接圆与某座的环带相交就激活。SDF 是 Lipschitz 的，
 * 只需在种子处求一次 `Sd`：`Sd − λ·r_c − m ≤ Falloff` 且 `Sd + λ·r_c + m ≥ 下界`。
 *   λ = 1（解析圆盘）/ √2（mask，种子在 SDF 网格内）/ 1 + √2（mask，网格外）。
 * `MarginCm` = 坡度差分跨距 + 扩张 / 倾斜 / 胀缩的最大平面位移。`AliveSlope` 只用于中心隆起的整盘判定。
 * 只许**过标**不许漏标：误激活的胞腔只是多付一个槽、被 kernel 写 NaN；漏掉的胞腔就是一块缺石。
 * `InOutRadiusCache` 按格键缓存外接半径（多边形是静态的；图案参数一变调用方必须清空它）。
 */
COMPUTESHADERGENERATOR_API void CollectActiveCells(const FPatternParams& Params, const FVector2D& GridOrigin,
	const FBox2D& Domain, TConstArrayView<FVector4f> ShaperParams, float MarginCm, float AliveSlope,
	TMap<FIntPoint, float>& InOutRadiusCache, TArray<FIntPoint>& OutCells);

/** 一趟披挂的全部标量参数。命名与 usf 的 uniform 一一对应；**长度一律已经是世界 cm**。 */
struct FDisplaceParams
{
	FPatternParams Pattern;
	/** 格 (0,0) 左下角的世界 XY（= 地面 actor 位置）。 */
	FVector2f GridOrigin = FVector2f::ZeroVector;
	float UVWorldPeriod = 100.0f;

	/** 允许出现壳的世界 XY 矩形（= 地面矩形）。矩形外的三角直接写 NaN。 */
	FVector2f DomainMin = FVector2f::ZeroVector;
	FVector2f DomainMax = FVector2f::ZeroVector;

	/** 法线是否走平均（第二趟）。false = 只留面法线（TG 原样）。 */
	bool bSmoothNormals = false;
	/** 平均的夹角阈值（度）：0 = 只认自己，180 = 全平均。 */
	float SmoothAngleDeg = 30.0f;

	/** 地面镜像格（只用来取道路权重：色流的 R 通道，V = Y * VertsX + X）。 */
	FVector2f GroundOriginXY = FVector2f::ZeroVector;
	float GroundCellSize = 50.0f;
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundBaseZ = 0.0f;

	/** Rock = smoothstep(SlopeLo, SlopeHi, |∇h|)，与 TG 的 rocky_terrain.x 同口径。 */
	float SlopeLo = 0.75f;
	float SlopeHi = 1.25f;
	/** 路足迹的增益（TG 10×），乘在模糊**之前**（地面路场的 PreGain）。 */
	float RoadFade = 10.0f;
	/** 路足迹进披挂之前的高斯模糊半径 cm。 */
	float RoadBlurRadius = 300.0f;
	/** road 满值时沿地形法线的下沉量 cm（世界口径，不随胞腔缩放）。 */
	float RoadSink = 160.0f;

	// —— 平面：TG 长度 × PatternScale（跟多边形尺寸配套），竖向：世界 cm（不缩，2026-09-30 用户嫌露出太少）——
	float CellJitter = 18.2f;        // 平面 · 最大收缩（52 × 0.35）；与浮高共用 r，范围 [-0.375, 1]
	float CellRelief = 30.0f;        // TG 位移法线的名义厚度；叉积长度保留，不再归一化
	float ReliefFloor = 0.0f;        // 厚度所乘 mask 的下限
	float CellExpand = 4.2f;         // 平面 · 每片朝外扩张（12 × 0.35）
	float SkirtTilt = 7.0f;          // 平面 · 只推底圈的裙圈倾斜（20 × 0.35）
	float BaseLift = 20.0f;          // 竖向 · TG `:563` +0.2 m
	float BaseSink = 40.0f;          // 竖向 · TG `:563` −0.4 m
	float NoiseAmp = 30.0f;          // 竖向 · TG `:721` 0.3 m（坡度 = 1 时的满幅）
	float NoiseFrequency = 1.0f / (13000.0f * 0.35f);   // 平面 · TG `:618` 的底频（世界 1/cm）

	// —— 「石头隆起」那一组（世界 cm，不随胞腔缩放）——
	float RiseMultiplier = 1.0f;
	float RiseNoiseAmp = 0.0f;
	float RiseNoiseFrequency = 0.0f;
	float RiseExtend = 0.0f;
	float EdgeCeiling = 0.9f;
	float PeakHeight = 1.0f;

	/** 可选的加性磕碰（TG 没有，默认 0）。 */
	float ChipAmount = 0.0f;
	float ChipFrequency = 1.0f / 40.0f;
	uint32 Seed = 1;
};

/**
 * 建 / 重建槽位池：声明流集（标准集 + 7 组 UV + 胞腔几何流）→ 容量 = `CapacityCells` 个槽 →
 * 全部三角退化（位置叠在包围盒底角，面积为 0）→ `WorldBounds` 按地面矩形**写死**。
 *
 * **阻塞**（声明流集 / 分配 / 上传各 flush 一次），只在「图案参数变了 / 容量不够」时走。交互期只走
 * `Displace`。返回是否建成（显存预检拒绝时返回 false，调用方把整条路关掉）。
 */
COMPUTESHADERGENERATOR_API bool BuildMesh(UCSMesh* ShellMesh, const FPatternParams& Pattern, int32 CapacityCells,
	const FBox& HardWorldBounds);

/**
 * 跑一趟作业：①建新槽的胞腔几何 → ②写新槽的静态属性 → ③关失效槽 → 路场模糊 → ⑤披挂 → ⑥平均法线（可选）→ ⑦倒角载荷。
 * **录完 pass 直接返回，不阻塞。** `GroundResident` 只读它的色流取道路权重。
 * `ShaperParams` 是 `ACSGroundActor::BuildShaperGpuParams` 打的那一份，按值搬进渲染命令。返回是否真的录了图。
 */
COMPUTESHADERGENERATOR_API bool Displace(
	UCSMesh* ShellMesh,
	const FCSMeshResidentRef& GroundResident,
	const FDisplaceParams& Params,
	const TArray<FVector4f>& ShaperParams,
	const FJobs& Jobs);
}

#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"
#include "RenderGraphResources.h"

class UTextureRenderTarget2D;

/**
 * 地被（草 + 花）：100% GPU 散布 + 零回读。形状照 Tiny Glade 的 `_generate_grass.cs` 那条链
 * （对照见 `Docs/TinyGlade/`）。
 *
 * -----------------------------------------------------------------------------
 * 归属：和石阶 / 岩壳 / 裙边摆件同一条理由 —— 归**地面**
 * -----------------------------------------------------------------------------
 * 草长在哪儿由两样东西决定：**合成后**的高度场（坡太陡不长）与地面的**顶点色遮罩**
 * （画了路的地方不长）。两样都只有 `ACSGroundActor` 手上有 —— 塑形物只看得见自己那一座，
 * 而遮罩本来就是地面的笔刷双写出来的。这与裙边摆件那三条依据逐字同形，不再重复。
 *
 * -----------------------------------------------------------------------------
 * 一株一个 instance，不是"一簇一个"
 * -----------------------------------------------------------------------------
 * TG 的草是**每叶一个 instance**（`_grass.raster` 用 `gl_InstanceIndex` 取叶参数、
 * `gl_VertexIndex` 取叶内顶点），没有"簇"这个概念；落点用低差异序列在 tile 内分层散布，
 * 本来就是为了不扎堆。本模块用**抖动网格**（一格一株、落点由格身份哈希抖动）拿到同一条性质，
 * 省掉 TG 那一整套 tile/bundle 前缀和 —— 它需要那套是因为它每帧重算并做 tile 级 HZB 剔除，
 * 本项目是"地面变了才重扫"，剔除交给 `UCSGpuInstancedMeshComponent` 自己的 pass。
 *
 * -----------------------------------------------------------------------------
 * ⚠️ 三条纪律（都是别处踩过的坑，照抄不要重新发明）
 * -----------------------------------------------------------------------------
 * 1. **随机源是格身份，不是 `InterlockedAdd` 的槽位**。槽位由线程组完成顺序决定，同一份世界
 *    状态两次 dispatch 能把同一株草放进不同的槽；而本 pass 每一笔落笔都重扫，症状是"画一笔路
 *    整片草原地重掷"，且不会有任何断言报红（石阶 S1 栽过一次）。
 * 2. **容量一次性定死、永不重算**。CPU 不知道长了几株（只有 GPU 知道），所以既不能按实例数
 *    算容量，也不能按实例数算包围盒 —— 后者会让 `bNeedHandover` 每一笔都成立，把阻塞的
 *    `SetInstanceSourceGPU` 拖进交互热路径。
 * 3. **遮罩读 GPU 上的那一份，不读 CPU 镜像的拷贝**。默认读地面遮罩（地面 actor 从笔刷双写出来的
 *    色流现算的那张贴图，地面材质混贴图也用它 —— 2026-09-29 用户："地形 mask 决定草"）；上传一份
 *    拷贝是第三条路，迟早与另外两条错开一帧，症状（草比路慢一笔）只在快速涂抹时才看得见。
 */
namespace CSGroundCover
{
/**
 * 一个物种（草，或某一种花）的固定容量实例源，直接喂
 * `UCSGpuInstancedMeshComponent::SetInstanceSourceGPU`。
 *
 * 一个物种一套，不共享一块 buffer：一个实例组件只绑一张基础网格 ⇒ 一张网格一个组件 ⇒
 * 一个组件一套实例源。`FCSGpuInstanceSourceGPU` 里也没有偏移量字段，塞进同一块 buffer 的
 * 后半段是画不出来的（石子那条已经把这个结论写死过一次）。
 */
struct FCoverBuffers
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances;   // Buffer<float4>，5 个 / 实例
	TRefCountPtr<FRDGPooledBuffer> Counter;           // Buffer<uint>，[0] = 活跃数（只有 GPU 知道）
	/**
	 * 逐实例 custom data，`Buffer<float>`，`CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS` 个 / 实例。
	 * 材质侧就是 `Per Instance Custom Data` 节点。
	 *
	 * **地被这一家的语义**：`[0]` = 弯曲幅度（TG 的 `_997`），`[1]` **保留、恒写 0**
	 * —— 它曾经存"该株世界高度"，但那是冗余的（材质能从逐实例 Local→World 推出来，
	 * 理由写在 `CSGroundCover.usf` 的对应注释里）。
	 * ⚠️ 语义是**逐组件**的：藤蔓那一家在同样的 `[0]/[1]` 上放的是 SpawnTime 与弧长
	 * （见 `CSGpuInstancedMeshComponent.h`），两家互不干扰，也**不需要**为此扩步长。
	 *
	 * 走并列缓冲而不是把 packed 行加宽，理由见 `FCSGpuInstanceSourceGPU::CustomData`：
	 * 那个 `* 5u` 的步长散在两个 .usf 与四处 CPU 路径里，动它的代价与风险都不对等。
	 */
	TRefCountPtr<FRDGPooledBuffer> CustomData;
	uint32 Capacity = 0;

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && CustomData.IsValid() && Capacity > 0; }
	void Reset() { *this = FCoverBuffers(); }
};

/** 一个物种散布一趟的全部标量参数。命名与 `CSGroundCover.usf` 的 uniform 一一对应。 */
struct FScatterParams
{
	FMatrix44f WorldToComponent = FMatrix44f::Identity;

	/** 散布格：原点是 (0,0) 格的角。`CellSize` = 1/√密度（换算在 `MakeGridForDensity`）。 */
	FVector2f GridOriginXY = FVector2f::ZeroVector;
	float CellSize = 20.0f;
	/** 这一趟派的格数：整张网格，或近处窗口。 */
	FIntPoint GridDims = FIntPoint(0, 0);
	/**
	 * **窗口散布**（2026-09-22 地被近 / 远两档：近处单根草只撒相机周围一块）：窗口左下角在整张网格里的格号，
	 * 与整张网格的格数。随机源用整张网格里的格号 ⇒ 窗口跟着相机挪时同一格的草一株不变。
	 * `GlobalDims` 留 (0,0) = 整张网格散布本身（偏移恒 0）—— 草、花、灌木的老路逐位不变。
	 */
	FIntPoint CellOffset = FIntPoint(0, 0);
	FIntPoint GlobalDims = FIntPoint(0, 0);

	/** 地面镜像格（取遮罩用；V = Y * VertsX + X）。 */
	FVector2f GroundOriginXY = FVector2f::ZeroVector;
	float GroundCellSize = 50.0f;
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundBaseZ = 0.0f;

	/** 遮罩通道的 one-hot（RGBA）。默认 (1,0,0,0) = R = 道路权重（见 `FCSGroundMirror::Colors`）。 */
	FVector4f MaskChannel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
	float MaskStart = 0.15f;
	float MaskEnd = 0.45f;
	/** 遮罩 → 压高比例。TG 在路上把草高乘 (1 − path × 0.7)，路边因此是矮草过渡而不是一刀切。 */
	float MaskShorten = 0.7f;
	/**
	 * 遮罩读**地面遮罩**（`Scatter` 的 `GroundMask`：处理过的路权重，地面材质混贴图用的也是它）。
	 * false = 读色流的原始 `MaskChannel`（选了 G/B/A 的物种，或地面还没有遮罩）。
	 * 为真却没传 `GroundMask` 时按 false 处理（占位贴图，读到的全是 0 ⇒ 满地长草，不会整片消失）。
	 */
	bool bUseGroundMask = false;
	/**
	 * 读地面遮罩前的采样扰动（`CSGroundMaskWarp.ush`，TG 的 path mask domain warp）：强度（cm，0 = 关）与噪声底层波长（cm）。
	 * 地面材质按同一对参数扰动 —— 由 `ACSGroundActor` 统一喂，两边不许各设各的。只在 `bUseGroundMask` 时生效。
	 */
	float MaskWarpStrength = 0.0f;
	float MaskWarpWavelength = 133.3f;

	float Chance = 1.0f;
	float Jitter = 1.0f;
	/** 地形法线 .z 的下限。由 `ACSGroundActor` 从"最大坡度角"换算（cos）。 */
	float MinSlopeCos = 0.5f;
	float Sink = 2.0f;
	/**
	 * 整个物种的世界 Z 偏移（cm，可正可负），**不乘高度缩放**。垂直方向只有这一个旋钮 ——
	 * 网格原点即落点，没有"按包围盒自动坐底"那一层（理由见 `FCSGroundCoverSpecies::HeightOffset`）。
	 */
	float HeightOffset = 0.0f;

	FVector2f ScaleRange = FVector2f(0.85f, 1.2f);
	float HeightJitter = 0.25f;
	/** 最大倾倒角，**单侧 [0, max]，整簇共用**（TG：0.3 × 90° = 27°）。 */
	float LeanMaxRad = 0.0f;
	float AlignToNormal = 0.0f;

	// --- 簇朝向（TG `_generate_grass:270-300` / `:391-416`）---
	/** 簇格边长（cm）。TG = 2.5 单位 = 250 cm。 */
	float ClumpSize = 250.0f;
	/** 整簇"从簇心向外辐射"的概率，其余是"整簇共用一个随机朝向"。TG = 0.30。 */
	float ClumpRadialChance = 0.30f;
	/** 簇朝向在 slerp 里的权重上限（实际再减 0.2·rand）。TG = 0.5 ⇒ 权重 0.3–0.5。 */
	float ClumpAlignment = 0.5f;

	/**
	 * 缩放取样有多少来自**簇**（1 = 完全簇共享，0 = 完全逐叶）。TG 的高度基准是簇共享的
	 * （`hash01(簇id*13)*1.5 + 0.5`），逐叶只叠一个高斯抖动 —— 所以整簇高矮成片，而不是逐株乱跳。
	 */
	float ScaleClumpShare = 1.0f;

	// --- 弯曲幅度：写进 custom data[0]，供材质做 WPO（TG `_945`/`_997`）---
	/** 弯曲幅度区间，逐叶均匀取样。TG = `hash*1.5 + 0.5` ⇒ [0.5, 2.0]。 */
	FVector2f BendRange = FVector2f(0.5f, 2.0f);
	/** **辐射簇**的弯曲幅度倍率。TG 实测 0.5 —— 朝外辐射的那 30% 簇同时也更挺。 */
	float RadialBendScale = 0.5f;

	uint32 Seed = 0;
	/** 物种盐。**同一份 Site 里两个物种撞盐会让它们逐格完全相关** —— 花就永远长在草心里。 */
	uint32 Salt = 0;

	FVector3f BaseSphereCentre = FVector3f::ZeroVector;
	float BaseSphereRadius = 0.0f;

	// --- 房屋环带：建筑周边灌木（`CSGroundCover.usf` 的「2.5) 房屋环带」）---
	float RingInner = 0.0f;              // 离外皮多远才开始长（cm）
	float RingWidth = 0.0f;              // 环带宽度（cm）；≤ 0 = 不走这道门控（草、花）
	float RingNoiseCell = 100.0f;        // 值噪声格距（cm），TG = 1 m
	float RingNoiseThreshold = 0.5f;     // 值噪声门槛，TG = 0.5
	float RingMinScale = 0.4f;           // 环带缩放低于它就不长
	/** 叠加物种：父物种长了的格子里再抽一次签（参数与盐必须和父物种逐项相同）。1 = 不抽。 */
	float OverlayChance = 1.0f;

	// --- 聚散（2026-09-29 用户："让花的分布有一些聚散"，`CSGroundCover.usf` 的「2.6) 聚散」）---
	float PatchSize = 0.0f;              // 成片的特征尺寸（cm，三层值噪声的底格）；≤ 0 = 不走这道门控（草、灌木）
	float PatchCoverage = 0.35f;         // 成片区域占地面的比例（按噪声分位数，实测误差 ±2%）
	float PatchSoftness = 0.3f;          // 片边软硬：过渡带在分位数上的宽度（0 = 一刀切）
	float PatchSparseDensity = 0.1f;     // 片外还留多少（相对片内的密度比例）
};

/** 房屋外皮多边形的打包布局：每栋 `BuildingStride` 个 float4（与 `CSGroundCover.usf` 同一份，经宏注入）。 */
constexpr int32 BuildingMaxVerts = 16;
constexpr int32 BuildingStride = 2 + BuildingMaxVerts / 2;

/**
 * 把一组**世界 XY 凸多边形**打包成 kernel 读的布局：[0] = AABB，[1].x = 顶点数，其后两个顶点一组。
 * 顺时针的自动翻成逆时针；顶点多于 `BuildingMaxVerts` 的退成它的 AABB 矩形（保守：环带只会更大，不会漏）。
 */
COMPUTESHADERGENERATOR_API void PackBuildingPolygons(TConstArrayView<TArray<FVector2D>> Polygons, TArray<FVector4f>& OutPacked);

/**
 * 密度 → 格距 + 格数，并保证 `GridX * GridY ≤ MaxCells`。
 *
 * ⚠️ 钳位不是保险丝而是必需：格数 = 面积 × 密度，而地面镜像最大 1024²×50 cm = 512 m 见方，
 * 50 株/m² 就是 1300 万线程 —— 没有这个钳位，把地面拉大一档就会挂掉整个编辑器。钳住之后
 * 密度**自动退让**（格距按 √(cells/max) 放大），行为是"草变稀"而不是"卡死"。
 *
 * 返回是否发生了退让（调用方可以据此打一条日志）。
 */
COMPUTESHADERGENERATOR_API bool MakeGridForDensity(
	const FBox2D& WorldRectXY, float DensityPerSqM, int32 MaxCells,
	FVector2f& OutOriginXY, float& OutCellSize, FIntPoint& OutGridDims);

/** 把容量补到 `Capacity`（只涨不缩）。真扩容那一趟**阻塞**，所以只该在注册/加载/改配置时碰到。 */
COMPUTESHADERGENERATOR_API bool EnsureBuffers(FCoverBuffers& Buffers, uint32 Capacity);

/**
 * 散布一趟：**一张 RDG 图里把全部物种一起录完**（每个物种一次 clear + 一次 dispatch）。
 * 录完直接返回，不阻塞。
 *
 * 一张图而不是每个物种一张：`FCSMeshRenderThreadEdit` 的进出、色流 SRV、塑形物参数上传
 * 都只需要一份；分成 N 张图等于把这三样各做 N 遍，而且 N 张图之间没有任何顺序保证。
 *
 * `Buffers` 与 `Params` **按下标一一对应**，长度必须相同（不同则整趟拒绝：错位的症状是
 * "花长成草的密度"，没有任何报错）。空 palette 也要跑 —— counter 清零正是"关掉某个物种"
 * 的唯一路径。
 */
COMPUTESHADERGENERATOR_API bool Scatter(
	const FCSMeshResidentRef& GroundResident,
	const TArray<FCoverBuffers>& Buffers,
	const TArray<FScatterParams>& Params,
	const TArray<FVector4f>& ShaperParams,
	const TArray<FVector4f>& BuildingPolygons = TArray<FVector4f>(),   // PackBuildingPolygons 的产物；只有 RingWidth > 0 的物种读它
	UTextureRenderTarget2D* GroundMask = nullptr);                      // 地面遮罩（`ACSGroundActor::GroundMask`）；bUseGroundMask 的物种读它

COMPUTESHADERGENERATOR_API void ReleaseOnRenderThread(FCoverBuffers& Buffers);

/** 诊断 / 验收专用，**阻塞**：GPU counter（按容量钳过）。行内容与 custom data 按需带出。 */
COMPUTESHADERGENERATOR_API int32 DebugReadInstancesSync(
	const FCoverBuffers& Buffers, TArray<FVector>* OutOrigins, TArray<FVector4f>* OutRows,
	TArray<float>* OutCustomData = nullptr);
}

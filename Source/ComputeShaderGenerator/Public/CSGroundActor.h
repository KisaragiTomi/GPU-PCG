#pragma once

#include "CoreMinimal.h"
#include "CSGroundCover.h"        // 地被（草 + 花）：FCoverBuffers / FScatterParams
#include "CSGroundDecor.h"        // 裙边摆件（D12 第五家）：FSite / FShaperRing，规划段共用 CSHouseDecor
#include "CSGroundRockShell.h"
#include "CSGroundStairs.h"
#include "CSMeshOps.h"
#include "CSTinyGlade.h"
#include "UObject/ObjectKey.h"   // 房屋外皮登记表的键
#include "CSGroundActor.generated.h"

class ACSGroundActor;
class ACSGroundShaperActor;
class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UCSGroundCollisionComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;
class UTextureRenderTarget2D;

/** 地面材质里接地面遮罩的参数名（`Scripts/TinyGladeMakeGroundMaterial.py` 建的节点，两边必须逐字相同）。 */
namespace CSGroundMaskMaterial
{
	/** 地面遮罩贴图（`ACSGroundActor::GroundMask`）。 */
	inline constexpr const TCHAR* TextureParameter = TEXT("GroundMask");
	/** 遮罩 UV = UV0 × xy + zw：UV0 是地面局部 XY / UVWorldPeriod，换算成"一个 texel 对一个格点"的中心对齐。 */
	inline constexpr const TCHAR* UVTransformParameter = TEXT("GroundMaskUVTransform");
	/** 1 = 用地面遮罩，0 = 退回顶点色 R（没有地面 actor 喂参数时，材质仍按老办法画）。 */
	inline constexpr const TCHAR* EnabledParameter = TEXT("GroundMaskEnabled");
	/**
	 * 草闸门 = smoothstep(SoilStart, SoilEnd, 遮罩 R)，两端喂草的 `MaskStart` / `MaskEnd`：= 这里不长草的概率，深土与浅土都乘它，
	 * 草长满处不露土（用户 2026-09-29："保留草的 mask，让土向外延申；现在看来草长在了土上"）。
	 */
	inline constexpr const TCHAR* SoilStartParameter = TEXT("GroundMaskSoilStart");
	inline constexpr const TCHAR* SoilEndParameter = TEXT("GroundMaskSoilEnd");
	/** 采样扰动 (强度 cm, 1/波长 cm⁻¹, UVWorldPeriod, 0)：与地被 / 灌木同一对参数（`GroundMaskWarpStrength` / `GroundMaskWarpWavelength`）。 */
	inline constexpr const TCHAR* WarpParameter = TEXT("GroundMaskWarp");
}

DECLARE_MULTICAST_DELEGATE_OneParam(FCSGroundPaintEditorRequest, ACSGroundActor*);
DECLARE_MULTICAST_DELEGATE_TwoParams(FCSGroundChanged, ACSGroundActor*, const FBox& /*ChangedWorldBounds*/);

/**
 * 地面的 CPU 权威镜像。高度 + 顶点色随关卡序列化；GPU 网格（UCSMesh）只是它的投影，
 * 加载/改参后由 RebuildGroundMesh() 全量重建，交互期间由笔刷做"镜像 + GPU"双写。
 *
 * 所有 gameplay 查询（道路权重、地面高度、拾取）只打镜像，永不回读 GPU —— 这是本 actor
 * 的第一纪律：任何新的写入路径必须双写，否则查询与画面分叉。
 *
 * 顶点布局约定：Id = y * NumVertsX + x（行主序），笔刷 pass 与镜像换算共同依赖。
 */
USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSGroundMirror
{
	GENERATED_BODY()

	UPROPERTY() int32 NumVertsX = 0;      // 顶点数 = 格数 + 1
	UPROPERTY() int32 NumVertsY = 0;
	UPROPERTY() float CellSize = 50.0f;   // cm/格，镜像自带一份防止 actor 配置改了老数据错配

	UPROPERTY() TArray<float> Heights;    // 相对 actor 的局部高度，初始全 0 = 平地
	UPROPERTY() TArray<FColor> Colors;    // 权威顶点色；R 通道 = 道路权重（见计划文档 D6）

	bool IsInitialized() const { return NumVertsX > 1 && NumVertsY > 1 && Heights.Num() == NumVertsX * NumVertsY && Colors.Num() == Heights.Num(); }
	int32 VertexIndex(int32 X, int32 Y) const { return Y * NumVertsX + X; }
};

/**
 * 地被读镜像顶点色的哪一个通道当遮罩。
 *
 * 默认 R —— 它就是笔刷画出来的**道路权重**（`FCSGroundMirror::Colors` 的约定，D6），
 * 所以"画一笔路"当场就是"这条路上不长草"，不需要再画第二张遮罩。G/B/A 目前没有别的消费者，
 * 想给某个物种单开一张遮罩时把笔刷的 `PaintChannelMask` 换过去即可。
 */
UENUM(BlueprintType)
enum class ECSGroundCoverMaskChannel : uint8
{
	Red     UMETA(DisplayName = "R（道路权重）"),
	Green   UMETA(DisplayName = "G"),
	Blue    UMETA(DisplayName = "B"),
	Alpha   UMETA(DisplayName = "A"),
};

/**
 * 地被的**近 / 远两档**（2026-09-22 用户："让远处使用组合草，近处使用单根草，并且远处只有法线运动没有 WPO"；
 * "草的数量限制上升到 100w 根"）。
 *
 * - **近处**：单根草（物种自己的 `Mesh` / `Material`，WPO 风），只撒在**相机周围一块窗口**里、满密度。
 *   `MaxInstances`（≈ 100 万）从此是这块窗口的预算，而不是摊在整张 512 m 地面上 —— 以前在大地面上
 *   密度会被自动退让到 4 株/m²，现在近处始终是 `DensityPerSqM`。相机水平走出 `RescatterDistance`
 *   才把窗口搬过去重撒一次（GPU 上一趟 dispatch，不阻塞）；随机源用整张网格里的格号，同一格的草一株不变。
 * - **远处**：组合草（`FarMesh`：一簇好几根低面片草叶），整张地面静态撒一次，只画在 `NearDistance` 之外。
 *   材质 `FarMaterial` **不做 WPO**（远处整片不动顶点，省掉 WPO 那一整套代价），风只扰动法线。
 * - 交界：两个组件共用一条过渡带（`FadeDistance`），带内逐株按随机数二选一 —— 近处渐稀、远处渐密，
 *   不出一道硬圈（`UCSGpuInstancedMeshComponent::InstanceCullFadeDistance`；Nanite 路在 GPU-Scene 写入时做同一件事）。
 * - 近处的 WPO 不是画到交界才断：交界前 `ACSGroundActor::CoverWPOFadeLength` 那一段里先淡到 0，交界之外不求 WPO
 *   （2026-09-29，花也按同一条交界淡出）。
 *
 * **只有 `FarMesh` 非空才生效**：没有远处网格时退回整张地面一档散布，不会出现"远处一根草都没有"。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSGroundCoverLOD
{
	GENERATED_BODY()

	/** 开关（还要 `FarMesh` 非空才生效）。草默认开（`ACSGroundActor` 构造函数里钉），花默认关。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD")
	bool bEnabled = false;

	/**
	 * 近处单根草画到离相机多远（cm）。之外交给组合草。
	 * 窗口半宽 = 它 + `RescatterDistance`；窗口格数超过 `MaxInstances` 时窗口（连同这个距离）自动收小并打日志。
	 * 默认 60 m：50 株/m² 时窗口约 140 m 见方、约 98 万株，正好用满 100 万的预算。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD", meta = (ClampMin = "500.0", ClampMax = "50000.0"))
	float NearDistance = 6000.0f;

	/** 近 / 远交界的过渡带宽（cm）：带内逐株按随机数二选一。0 = 硬边（会看见一道圈）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD", meta = (ClampMin = "0.0", ClampMax = "20000.0"))
	float FadeDistance = 1500.0f;

	/** 相机水平挪过多远才把近处窗口搬过去重撒一次（cm）。越小窗口越省（半宽 = NearDistance + 它），重撒越勤。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD", meta = (ClampMin = "100.0", ClampMax = "20000.0"))
	float RescatterDistance = 1000.0f;

	/** 远处的组合草网格（一簇好几根低面片草叶，`Scripts/TinyGladeMakeGrassClump.py` 造的 `SM_TG_GrassClump`）。空 = 不分两档。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD")
	TObjectPtr<UStaticMesh> FarMesh;

	/**
	 * 远处的材质：**不做 WPO**，风只扰动法线（`MI_TG_GrassFar`）。⚠️ 同样必须勾 `bUsedWithInstancedStaticMeshes`。
	 * 空 = 用物种自己的材质（那就带着 WPO，只剩网格换成了组合草）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD")
	TObjectPtr<UMaterialInterface> FarMaterial;

	/** 远处组合草几簇 / m²（一簇约等于若干片草叶，远处看不出单根）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD", meta = (ClampMin = "0.01", ClampMax = "50.0"))
	float FarDensityPerSqM = 1.5f;

	/** 远处组合草的格数天花板（同 `MaxInstances`：超了密度自动退让并打日志）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD", meta = (ClampMin = "64", ClampMax = "1048576"))
	int32 FarMaxInstances = 1048576;

	/** 远处组合草的整体缩放区间（逐簇均匀取样；再叠物种的 `HeightJitter`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD")
	FVector2D FarScaleRange = FVector2D(0.9, 1.2);
};

/**
 * 一个地被物种（草，或某一种花）。草与花共用这一份结构 —— 两者在 GPU 上跑的是**同一个
 * kernel**，只差 uniform（密度、遮罩阈值、缩放、盐），分两套参数只会在下一次调参时分叉。
 *
 * ⚠️ `MaxInstances` 是**唯一**同时约束显存与 dispatch 规模的旋钮：散布格的格数被它钳住
 * （`CSGroundCover::MakeGridForDensity`），超预算时**密度自动退让**而不是把编辑器跑挂。
 * 地面镜像最大 1024² × 50 cm = 512 m 见方，50 株/m² 就是 1300 万线程 —— 没有这个钳位，
 * 把地面拉大一档就会挂掉整个编辑器。
 *
 * 它是**天花板不是预算**：缓冲按实际格数分配，所以默认就顶到上限，让密度在正常尺寸的地面上
 * 原样生效；只有真的算超了才退让（并打日志说明退到了多少）。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSGroundCoverSpecies
{
	GENERATED_BODY()

	/** 基础网格。**为空 = 这个物种整条关掉**（不建组件、不分配显存、不发 dispatch）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover")
	TObjectPtr<UStaticMesh> Mesh;

	/**
	 * 材质。⚠️ **必须勾 `bUsedWithInstancedStaticMeshes`** —— 没勾的材质在实例路径上会被引擎
	 * **静默换成默认材质**，画面一片灰而所有 readback 断言照绿（与裙边摆件同一条陷阱，
	 * `GetGroundCoverUndrawableReason()` 把它做成了显式判据）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover")
	TObjectPtr<UMaterialInterface> Material;

	/** 每平方米几株。TG 满密度实测 ≈ 50 株/m²（tile 2.03 m 见方、≤ 204 叶）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover", meta = (ClampMin = "0.0", ClampMax = "400.0"))
	float DensityPerSqM = 50.0f;

	/**
	 * 散布格的**格数天花板**。见结构注释里的 ⚠️。
	 *
	 * 默认顶到上限：显存**按实际格数分配**（`Capacity = GridX × GridY`，density 说了算），
	 * 天花板只在"这块地面按这个密度算出来的格数超了"时才生效 —— 所以调高它在密度用不到的
	 * 时候一个字节都不多花，只是把"自动退让"的触发点推远。
	 *
	 * 触发点参考：1048576 格 ≈ 50 株/m² 铺满 **145 m 见方**；真触发时每 1 万格 ≈ 0.8 MB。
	 *
	 * 开了近 / 远两档（`LOD`）时，它是**近处窗口**的预算（窗口满密度，不退让；装不下时窗口收小）——
	 * 2026-09-22 用户"草的数量限制上升到 100w 根"：这 100 万根从此都落在相机周围，而不是摊在整张地面上。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover", meta = (ClampMin = "64", ClampMax = "1048576"))
	int32 MaxInstances = 1048576;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Mask")
	ECSGroundCoverMaskChannel MaskChannel = ECSGroundCoverMaskChannel::Red;

	/** 遮罩 ≤ Start 完全不受抑制；≥ End 完全不长；中间按**概率**拒绝（硬阈值会切出一条直边）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Mask", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaskStart = 0.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Mask", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaskEnd = 0.45f;

	/** 遮罩 → 压高比例。TG 在路上把草高乘 (1 − path × 0.7)，路边因此是矮草过渡而不是一刀切。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Mask", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MaskShorten = 0.7f;

	/**
	 * 比这更陡的坡不长草。坡度由**合成后**的高度场中心差分求，与岩壳 mask 同源。
	 *
	 * ⚠️ **2026-09-06 由 55° 降到 30°（用户裁决：草默认不要长在斜面上）**。55° 的老默认值让草一路
	 * 长到 `tan 55° = 1.43` 的坡上 —— 那已经**比岩壳完全铺满的坡（`RockShellSlopeHi = 1.25`）还陡**，
	 * 于是草直接从石头缝里长出来。参照系：岩壳从 `RockShellSlopeLo = 0.75`（≈ 37°）开始淡入，所以
	 * 30° 让草在石头露头之前就收干净。想更严往 20~25° 调；想让草与石头正好咬合就调到 37°。
	 *
	 * ⚠️ 这一条是**硬阈值**（`CSGroundCover.usf` 的「4) 坡度门控」直接 `return`），而它上面那条遮罩
	 * 门控是**概率拒绝** —— 理由就写在那里：硬阈值会沿等值线切出一条肉眼可见的直边。阈值挂在 55°
	 * 时坡本身就少见，这条边基本看不到；降到 30° 之后它落进常见坡度区，直边会明显起来。真被看出来
	 * 了就把坡度门控也改成概率拒绝（与遮罩共用同一套格身份哈希，边界才不会重扫时闪烁）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Mask", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float MaxSlopeDegrees = 30.0f;

	/** 落点抖动幅度，格距的比例。0 = 规则网格（会看出格子），1 = 整格随机（默认）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Jitter = 1.0f;

	/** 均匀缩放区间（乘在基础网格上）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0"))
	FVector2D ScaleRange = FVector2D(0.85, 1.25);

	/** Z 轴额外抖动（±），只改高矮不改粗细 —— 与均匀缩放共用一个哈希会读成"几种尺寸的同一株"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0", ClampMax = "0.95"))
	float HeightJitter = 0.25f;

	/**
	 * 最大倾倒角（度）。**单侧 [0, 本值]，而且整簇共用** —— 不是逐叶对称抖动。
	 * TG 实测 `hash01(簇id 派生) × 0.3`，换算成 0°–27°（VS 里 `−byte/255 × 1.5696 rad`）。
	 * 倾倒方向 = 株的朝向，与基础网格的弯曲方向同向。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0", ClampMax = "80.0"))
	float LeanDegrees = 27.0f;

	// --- 簇朝向：TG 的"朝向丰富"就出在这三条（`_generate_grass:270-300` / `:391-416`）---
	//
	// 叶片先在 3×3 邻域里找最近的**抖动站点**（jittered-grid Worley），得到簇心与簇 id；
	// **整簇共用一个朝向和一个倾倒角**，逐叶再 slerp 一个纯随机方向进去。三层叠起来才像草：
	// 全逐叶随机读成噪声，全对齐读成梳过的地毯。⚠️ 成簇的是**朝向**，不是位置 —— 撒点仍然
	// 是分层抖动网格（均匀不扎堆）。

	/** 簇格边长（cm）。TG = 2.5 单位 = 250 cm。调小 = 朝向变化更碎，调大 = 大片同向。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Clump", meta = (ClampMin = "1.0"))
	float ClumpSize = 250.0f;

	/** 整簇"从簇心向外辐射"的概率，其余是"整簇共用一个随机朝向"。TG 实测 0.30。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ClumpRadialChance = 0.30f;

	/**
	 * 簇朝向在 slerp 里占的权重**上限**（实际权重再减 `0.2 × 逐叶随机`，沿用 TG 的抖动量）。
	 * 0 = 完全逐叶随机（成簇关掉），1 = 完全跟簇。TG = 0.5 ⇒ 实际权重 0.3–0.5。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ClumpAlignment = 0.5f;

	/**
	 * `ScaleRange` 的取样有多少来自**簇**（1 = 整簇同高，0 = 完全逐叶）。
	 * TG 的高度基准就是簇共享的（`hash01(簇id*13)*1.5 + 0.5`），逐叶只叠一个高斯抖动 ——
	 * 所以整簇高矮成片而不是逐株乱跳。设 0 与加这条之前逐位相同。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Clump", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ScaleClumpShare = 1.0f;

	// --- 弯曲：写进逐实例 custom data[0]，材质用 `Per Instance Custom Data` 读 ---
	//
	// 本家族的 custom data 语义：**[0] = 弯曲幅度、[1] = 该株的世界高度 cm**。
	// 语义是逐组件的 —— 藤蔓那一家在同样的 [0]/[1] 上放 SpawnTime 与弧长，互不干扰，
	// 也**不需要**为地被扩步长（`CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS`）。

	/** 弯曲幅度区间，逐叶均匀取样。TG = `hash*1.5 + 0.5` ⇒ [0.5, 2.0]。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Bend", meta = (ClampMin = "0.0"))
	FVector2D BendRange = FVector2D(0.5, 2.0);

	/**
	 * **辐射簇**的弯曲幅度倍率。TG 实测 0.5 —— 朝外辐射的那 30% 簇同时也更挺。
	 * 给成 1.0 的话辐射簇会读成"一朵塌下去的花"而不是"一丛支棱着的草"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Bend", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float RadialBendScale = 0.5f;

	/** 0 = 一律世界上（TG 的草就是这样），1 = 完全贴地形法线。花插在坡上时给一点更自然。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float AlignToNormal = 0.0f;

	/** 根部沉入地表的深度（cm）。整株浮在地表上会读成"掉了个道具"（石子那条的实测结论）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (ClampMin = "0.0"))
	float Sink = 2.0f;

	/**
	 * 整个物种的**世界 Z 偏移（cm，可正可负）**：正 = 整片抬高，负 = 整片压低。
	 * 垂直方向就这一个整体旋钮 —— 换一朵花、或者想让花冠浮在草尖之上时调它。
	 *
	 * **网格原点即落点，不做任何自动修正**（2026-09-09 用户裁定，推翻了原先的 `bSeatOnBase`）：
	 * 从前这里会把网格的包围盒底面自动钉到地表（原点抬 −Min.Z × 高度缩放），依据是
	 * "`lowpoly_flower` 的包围盒离原点 30 cm 才开始 ⇒ 不补这一项整片花会悬空"。**那条依据是
	 * 错的** —— 花有时候就是该高出地面，那 30 cm 是网格作者摆出来的，不是缺陷。自动修正把
	 * 作者的意图抹掉了，而且它是**逐网格**的：换一张花的网格，全片花的高度会跟着新包围盒
	 * 无声地跳一次。现在的约定是**网格摆在哪儿就是哪儿**，要挪就在这里显式挪 —— 数字写在
	 * 面板上，看得见也 diff 得出来。
	 *
	 * ⚠️ **不乘高度缩放**：它表达的是"整片一起挪这么多厘米"。乘了缩放的话高的那株挪得更多，
	 * 一片花就不再落在同一个平面上，读起来像是随机浮空而不是整体抬高。
	 *
	 * 与 `Sink` 不重复：`Sink` 只往下、钳在 ≥ 0，语义是"根埋进土里"；本条可正可负，是整体位移。
	 * 上下界只是 UI 滑条范围（`UIMin/UIMax`），不是硬钳位 —— 输入框里能填更大的值；
	 * 保守包围盒会把它算进最坏伸展，所以填多大都不会在边缘被剔掉。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape", meta = (UIMin = "-100.0", UIMax = "100.0"))
	float HeightOffset = 0.0f;

	/**
	 * 这个物种投不投阴影。**默认开**（2026-09-12 用户裁决：花要有投影），但
	 * **`ACSGroundActor::ACSGroundActor` 把 `Grass` 这一份钉回 `false`** —— 所以实际语义是
	 * 「花默认投影、草默认不投影」。两者分开的理由是密度差着两个数量级：
	 *
	 * · 草满密度 50 株/m²，投影是这条路上最贵的一项：每一株都要进阴影深度 pass 再画一遍，
	 *   而叶片本身只有几个三角 —— 付的是 draw 侧的固定开销，不是像素。观感上也不缺：
	 *   TG 的草同样不投影，草地的明暗来自地面自己的阴影与 AO。（2026-09-06 裁决，仍然有效。）
	 * · 花是稀疏点缀，一朵的影子是看得见的形状，不是一片糊在地上的噪声。
	 *
	 * ⚠️ 写在**组件**上（`EnsureCoverComponents` 里 `SetCastShadow`），不是材质开关 ——
	 * 材质那一层关不掉阴影 pass 的 draw。
	 *
	 * ⚠️ 2026-09-29 起花与草的网格开了 Nanite（主通道由 Nanite 画）：投影的物种同时开组件的
	 * `bClassicShadowOnNanitePath`（`EnsureCoverComponents` 按本条写），Nanite 替身不投影，影子由一份只投影的
	 * 经典实例代理画 —— 所以下面这段 CSM 的机制照旧成立，影子的软度与切 Nanite 之前相同。
	 *
	 * ⚠️ 光这一条**不足以**让影子出现：地被走实例路（`UCSGpuInstancedMeshComponent`），它的
	 * 顶点工厂故意不声明 primitive-id 流 ⇒ `SupportsGPUScene()` 为假 ⇒ 进不了 VSM，只能进
	 * 常规 CSM 级联。而方向光开 VSM 时那份回退级联默认是**不创建**的 ——
	 * 必须同时有 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0`（本工程
	 * `Config/DefaultEngine.ini` 已设，那里写了完整的机制与出处）。少了它的症状是
	 * 「开关打开、组件 CastShadow 为真、一个影子像素都没有」，没有任何报错。
	 *
	 * ⚠️ 改默认值只影响**新建**的物种行（以及 CDO）。`Flowers` 是 TArray，已存盘的关卡里
	 * 整个数组都是序列化过的 ⇒ 老关卡里的花仍然带着当年存下的 `false`，要在 details 面板里
	 * 手动勾一次。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Shape")
	bool bCastShadow = true;

	/**
	 * 物种盐。⚠️ **两个物种撞盐会让它们逐格完全相关** —— 每一朵花的位置上必定也有一株草，
	 * 花因此永远长在草心里，看起来像穿模而不是像野花。默认值刻意各不相同。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover")
	int32 Salt = 1;

	// --- 聚散（2026-09-29 用户："让花的分布有一些聚散"；参考图 Docs/TinyGlade/img/tiny-glade-ref-flower-clusters-20260929.png）---
	//
	// 抖动网格本身是均匀的（草要的正是这个）。开了聚散之后，**保留概率**随位置成片变化：片内全留、片外只留一小部分，
	// 片边渐变。位置仍是抖动网格，所以片内是"均匀地密"而不是扎成一坨。片的形状由三层值噪声定，按格身份抽签 ⇒ 重扫不闪。
	// 平均密度 ≈ `DensityPerSqM × (Coverage + (1 − Coverage) × SparseDensity)`：想保持总株数就把 `DensityPerSqM` 相应调高。

	/**
	 * 成片的特征尺寸（cm）。**0 = 关**（草默认关）。这是三层值噪声的底格：实际的片大大小小（约 0.5×–2× 本值）、边缘不规则。
	 * 参考图里低矮的花一片约 2–4 m，成丛的高花（风信子那类）一丛约 0.5–1.5 m。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Patch", meta = (ClampMin = "0.0", UIMax = "2000.0"))
	float PatchSize = 0.0f;

	/** 成片区域占地面的比例（按噪声分位数算，实测误差 ±2%）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Patch", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PatchCoverage = 0.35f;

	/** 片边软硬：过渡带在分位数上的宽度。0 = 一刀切（片边是一条清楚的轮廓），越大越像"越往外越稀"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Patch", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PatchSoftness = 0.3f;

	/** 片外还留多少（相对片内密度的比例）。0 = 片外一朵都没有，参考图里片与片之间还有零星几朵，约 0.1。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|Patch", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PatchSparseDensity = 0.1f;

	/**
	 * 近 / 远两档（见 `FCSGroundCoverLOD`）：开着且 `FarMesh` 非空时，本物种只撒相机周围一块窗口（`MaxInstances`
	 * 是这块窗口的预算），远处交给组合草。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cover|LOD")
	FCSGroundCoverLOD LOD;
};

/**
 * 建筑周边灌木（照 TG `_garden_spawn_bushes_buildings.cs`）。
 *
 * 与地被共用同一个散布 kernel（`CSGroundCover.usf`），多一道**房屋环带**门控：离外皮
 * `RingInner`–`RingInner + RingWidth` 之间、过一层值噪声门槛，尺寸按环带里的位置取鼓包。
 * 房子只把外皮凸多边形推给地面（`ACSGroundActor::SetBuildingFootprint`），灌木归地面 ——
 * 由**全部**房子的环带并集决定，单栋房子看不见别人（归属依据同 `CSGroundDecor.h`）。
 *
 * 每次输入变了就**整块地面重散**（TG 同样整体重算）：候选点按格身份哈希确定，没变的地方
 * 逐位相同，所以不闪。房子拖动时只重散灌木，不连累草。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSGroundBuildingBushes
{
	GENERATED_BODY()

	/** 灌木本体。**为空 = 整条关掉**（不建组件、不分配显存、不发 dispatch）。开了 Nanite 的资产自动走 Nanite。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	TObjectPtr<UStaticMesh> Mesh;

	/** 覆盖材质；空 = 用资产自带材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	TObjectPtr<UMaterialInterface> Material;

	/** 叠在一部分灌木上的花，与本体**同一个变换**（TG：`FlowerXform` 直接复用 `BushXform`）。空 = 不开花。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	TObjectPtr<UStaticMesh> FlowerMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	TObjectPtr<UMaterialInterface> FlowerMaterial;

	/** 开花的比例。TG = 0.3（`hash > 0.7`）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowerChance = 0.3f;

	/** 候选密度（株/m²），环带与噪声门槛之前。TG 的候选格约 2 m ⇒ 0.25。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes", meta = (ClampMin = "0.01", ClampMax = "4.0"))
	float DensityPerSqM = 0.25f;

	/** 离外皮多远才开始长（cm）。0 会让灌木心压在墙线上、半棵插进墙里。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Ring", meta = (ClampMin = "0.0"))
	float RingInner = 30.0f;

	/** 环带宽度（cm）。尺寸鼓包在环带 40% 处最大，两头缩到 0.2 倍。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Ring", meta = (ClampMin = "1.0"))
	float RingWidth = 300.0f;

	/** 值噪声格距（cm）。TG = 1 m：灌木因此成团，而不是均匀地绕房子一圈。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Ring", meta = (ClampMin = "1.0"))
	float NoiseCellSize = 100.0f;

	/** 值噪声门槛。TG = 0.5（约一半的候选被筛掉）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Ring", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float NoiseThreshold = 0.5f;

	/** 环带缩放低于它就不长（TG 丢掉环带两头那些太小的）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Ring", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float MinRingScale = 0.4f;

	/** 基础缩放区间，再乘环带鼓包（0.2–1）。`bush_body` 原件约 5 m 见方。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape")
	FVector2D ScaleRange = FVector2D(0.45, 0.6);

	/** 高度额外抖动（±），只改高矮不改粗细。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape", meta = (ClampMin = "0.0", ClampMax = "0.95"))
	float HeightJitter = 0.25f;

	/** 比这更陡的坡不长。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape", meta = (ClampMin = "0.0", ClampMax = "89.0"))
	float MaxSlopeDegrees = 35.0f;

	/** 路（顶点色 R）门控：TG 在 `path > 0.2` 处不长。Start 以下完全不受影响，End 以上完全不长。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RoadMaskStart = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RoadMaskEnd = 0.2f;

	/** 根部沉入地表（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape", meta = (ClampMin = "0.0"))
	float Sink = 10.0f;

	/** 整片的世界 Z 偏移（cm，不乘缩放）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes|Shape")
	float HeightOffset = 0.0f;

	/**
	 * 实例容量上限（本体与花各一份）。**与散布格数无关**：候选格铺满整块地面（512 m 地面约 6.5 万格），
	 * 但只有房子周围那一圈长得出来，所以容量按"能长多少"定，不按格数 —— 越界的静默丢弃。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes", meta = (ClampMin = "64", ClampMax = "1048576"))
	int32 MaxInstances = 8192;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	bool bCastShadow = true;

	/** 灌木的盐（本体与花共用 —— 花必须落在本体同一批格子上）。与地被物种的盐分开取值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bushes")
	int32 Salt = 101;
};

/**
 * gpumesh 地面：一块规则网格 UCSMesh + CPU 权威镜像（FCSGroundMirror），不用 Landscape。
 * Plan_House.md 的 D1/D2。网格底座（渲染组件 / UCSMesh / 快照上传）在基类
 * ACSTinyGlade；地面是权威数据源，不消费世界变化，因此不 override ReevaluateSite()。
 *
 * 交互入口沿用点笔刷的启动模式：StartVertexColorPaint()（CallInEditor 按钮）广播静态
 * 委托，PCGEditorProcess 应答并激活 FCSGroundPaintEdMode；EdMode 的光标 trace 走
 * RaycastGround —— GPU 网格全线 NoCollision，引擎 line trace 打不到它。
 *
 * 变换约定：仅支持平移。常驻流是世界空间，旋转/缩放会让镜像查询（世界 XY → 格点）失效，
 * 编辑器拖动期间用 TranslateMesh 增量平移，松手后全量重建对齐。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API ACSGroundActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	/**
	 * 只做一件事：把 `Grass.bCastShadow` 钉回 `false`。
	 *
	 * `FCSGroundCoverSpecies::bCastShadow` 的声明默认值是 `true`（花要投影），而草和花共用
	 * 同一个结构体 ⇒ 草那一份必须在这里显式压回去。没有"逐成员默认值"这种东西，构造函数
	 * 就是唯一的落点。理由与两者的密度差写在那个字段的注释里。
	 */
	ACSGroundActor();

	/** Raised by StartVertexColorPaint(); the editor module answers it by activating the paint EdMode. */
	static FCSGroundPaintEditorRequest OnGroundPaintEditorRequest;

	/**
	 * 任一地面完成一次全量重建（生成 / 加载 / 显式重建 / 拖动松手）后广播，参数同 `OnGroundChanged`。
	 * 给**还没拿到地面指针**的消费者用：实例级的 `OnGroundChanged` 只发给已订阅者，地面比房子
	 * 晚出现时，房子永远等不到这条广播。类级委托，所有世界共用 —— 订阅方自己按 World 过滤。
	 * CSSceneDirty3D 落地后由空间拉取取代（发布方不必认识消费者，消费者也不必先拿到发布方指针）。
	 */
	static FCSGroundChanged OnAnyGroundRebuilt;

	/**
	 * 地面变动的直推通知（v1 架构裁决：不用 CSSceneDirty3D，任何变动必然广播，消费者
	 * 无条件重求值、靠幂等哈希兜底）。笔刷每次落笔、全量重建、编辑器拖动平移都会触发。
	 *
	 * 参数带本次变化的世界盒；v1 消费者可以无视它（全员重算就是正确语义），它是将来
	 * 切回 dirty 系统时的过滤接缝。注意：广播只覆盖"之后的变化"——消费者注册/加载时
	 * 必须自己先主动重求值一次，别指望赶上地面加载重建的那一次广播（注册顺序无保证）。
	 */
	FCSGroundChanged OnGroundChanged;

	// -------------------------------------------------------------------------
	// Ground Settings
	// -------------------------------------------------------------------------

	/** X 方向格数。顶点数是格数 + 1；改动会重置镜像（高度/顶点色清空重来）。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 NumCellsX = 64;

	/** Y 方向格数。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1", ClampMax = "1024"))
	int32 NumCellsY = 64;

	/** 格尺寸 cm。改动会重置镜像。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS Ground", meta = (ClampMin = "1.0"))
	float CellSize = 50.0f;

	/** UV0 的世界平铺周期 cm：UV = 局部坐标 / 周期，材质密度不随地面尺寸变。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground", meta = (ClampMin = "1.0"))
	float UVWorldPeriod = 500.0f;

	/**
	 * 整片地面的材质。草地 / 深土 / 浅土路按**地面遮罩**（`GroundMask`）混；网格实际画的是它的动态子实例，
	 * 遮罩贴图与 UV 换算由本 actor 喂进去（参数名见 `CSGroundMaskMaterial`）。材质里没有这几个参数时照常画，
	 * 只是不吃遮罩。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground")
	TObjectPtr<UMaterialInterface> GroundMaterial;

	/**
	 * 镜像重置时铺的底色。R 分量同时是"无道路"的语义零点。
	 *
	 * **改它不会自动重刷已有顶点色** —— 权威是 Mirror.Colors，快照只搬镜像、不读本字段，
	 * 所以早先"改底色就全量重建"是一次纯空转（13 万三角重传 + 全场唤醒，画面零变化）。
	 * 要把新底色铺下去请点 ResetPaint()。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground")
	FLinearColor BaseColor = FLinearColor(0.0f, 0.0f, 0.0f, 1.0f);

	// -------------------------------------------------------------------------
	// Ground Mask（地面遮罩：处理过的路权重，地面材质与草 / 花 / 灌木共用这一张）
	// -------------------------------------------------------------------------

	/**
	 * 地面遮罩向路外**平滑延伸**多远（cm）（2026-09-29 用户："道路的 mask 变化太剧烈，结果需要做平滑延申"；
	 * "地形 mask 决定草，经过处理的地面 mask 作为地面 mesh 的遮罩控制贴图"）。
	 *
	 * 笔刷画的路权重（顶点色 R）是逐顶点的，格距 50 cm ⇒ 路缘在一格之内从 0 跳到 1。地面遮罩把它糊开再 ×2、
	 * 与原值取 max（`CSGroundRoadField`，σ = 本值 / 2）：画出来的路以内恒 1，路缘以外平滑衰减，离路本值处约剩 5%。
	 * **地面材质按它混土路 / 深土 / 草地三层贴图，草、花、灌木的拒绝 / 压矮 / 弯曲也都读它** ——
	 * 贴图上露土的地方就不长草，两边构造上同源。
	 *
	 * 0 = 不处理（遮罩 = 原始路权重）。上限受模糊核 32 格截顶：50 cm 格距下约 10 m。
	 * ⚠️ 门洞、石阶、岩壳、裙边摆件仍按**原始**路权重判：它们要的是"路画到了哪"，不是视觉上的过渡。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Ground Mask", meta = (ClampMin = "0.0", ClampMax = "1000.0", UIMax = "400.0"))
	float GroundMaskExtendDistance = 150.0f;

	/**
	 * 读地面遮罩时把采样点挪多远（cm）——草线、深土、浅土路三条交界跟着一起抖（用户 2026-09-29："mask 还是需要有一些扰动的，
	 * 现在看上去过于平均了" / "交界过于生硬"）。
	 *
	 * 照 TG 的 path mask domain warp（`_paths_path_mask_visual`：两层 value noise fbm，位移 = fbm × 0.6 m，fbm 约 ±0.775
	 * ⇒ 每个分量最多约 ±47 cm），默认值就是 TG 的 60。地面材质与草 / 花 / 灌木按**同一个位移**采（`CSGroundMaskWarp.ush`），
	 * 土边抖到哪草线就跟到哪。遮罩贴图本身不带扰动。0 = 关。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Ground Mask", meta = (ClampMin = "0.0", UIMax = "200.0"))
	float GroundMaskWarpStrength = 60.0f;

	/** 采样扰动噪声的底层波长（cm）：越小抖得越碎。TG 0.75/m ⇒ 133.3 cm；第二层是它的 1/1.9。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Ground Mask", meta = (ClampMin = "10.0", UIMin = "50.0", UIMax = "1000.0"))
	float GroundMaskWarpWavelength = 133.3f;

	/**
	 * 地面遮罩本体：一个 texel 对一个地面格点，RG32F。运行时生成、不存盘，笔刷每落一笔、地面每重建一次都在 GPU 上重算。
	 * 挂在面板上是为了能直接打开看（纹理查看器）。
	 *   R = 上面说的成品（草 / 花 / 灌木与材质的深土层读它）；
	 *   G = 只糊、不 ×2、不取 max 的场 = TG 的 path_mask_blurred：画出来的路缘处 0.5，往路中间才升到 1（窄路升不满：
	 *       3 格宽的路中线 ≈ 0.69）。地面材质照 TG 的 floor PS 用它：深土与浅土对同一个高度竞争权重
	 *       HeightWeight(G, dirtpath_heightmap) 取两段 smoothstep，深浅由噪声图决定；两层再乘草闸门（R 上草的拒绝曲线）。
	 *       采样时按 `GroundMaskWarpStrength` 扰动（与草同一个位移）。
	 */
	UPROPERTY(VisibleInstanceOnly, Transient, Category = "CS Ground|Ground Mask")
	TObjectPtr<UTextureRenderTarget2D> GroundMask;

	// -------------------------------------------------------------------------
	// Brush Settings（EdMode 每次落笔读这里，和点笔刷一样归目标 actor 所有）
	// -------------------------------------------------------------------------

	/** 笔刷球半径 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "1.0"))
	float BrushRadius = 300.0f;

	/** 衰减占半径的比例：0 硬边到底，1 从球心开始衰减。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrushFalloff = 0.5f;

	/** 整笔强度缩放。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BrushStrength = 1.0f;

	/** 笔刷颜色。默认画 R = 道路权重。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	FLinearColor PaintColor = FLinearColor(1.0f, 0.0f, 0.0f, 1.0f);

	/** 通道门：> 0 的通道才被笔刷影响。默认只动 R。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	FLinearColor PaintChannelMask = FLinearColor(1.0f, 0.0f, 0.0f, 0.0f);

	/** 混合公式见 ECSMeshPaintBlendOp 注释；擦除道路把它切到 Erase。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	ECSMeshPaintBlendOp PaintBlendOp = ECSMeshPaintBlendOp::Replace;

	/** Leave the brush mode after each committed stroke. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Brush")
	bool bExitAfterCommit = false;

	// -------------------------------------------------------------------------
	// Stairs（计划「石阶改造：100% GPU 决策 + 零回读」的 S1/S2/S3）
	//
	// **为什么石阶归地面而不归塑形物**：扫描域是全局的 —— marching squares 扫的是全部塑形物
	// 合成之后的高度场，格不与任何一座对齐，跨在两座接合处的那一格照常出等值线。归任一座
	// 都不对，也正是这一点让"两座相接土台的接合处石阶断掉"那条缺陷从根上消失。
	//
	// 曾经并行的旧路（塑形物自持的 `RebuildSteps` + `CSShaperSteps`）已随 2026-08-30
	// 「裁决一」第二步整条删除（S3）⇒ **这里是全项目唯一的一条石阶路**。
	// -------------------------------------------------------------------------

	/** 石阶基础网格。**留空 = 整条石阶路径关闭**（不建组件、不分配显存、不发 dispatch）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	TObjectPtr<UStaticMesh> StairMesh;

	/** 石阶的整体覆盖材质。**留空 = 逐段画 `StairMesh` 资产自带的材质**；设了 = 盖住每一段。
	 *  母材质必须勾 "Used with Instanced Static Meshes"（资产开了 Nanite 时是 "Used with Nanite"）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	TObjectPtr<UMaterialInterface> StairMaterial;

	/**
	 * 等值线扫描格的边长 cm。
	 *
	 * ⚠️ **它直接决定石阶间距**：marching squares 每格每层出一级，间距完全由格密度决定 ——
	 * 太密石阶互相穿模，太疏断断续续。所以它必须 ≈ `StairBlockSize.Y`（石阶长度），
	 * 而**不能**复用地面的 `CellSize`（50 cm，会密一倍）。这也是 Tiny Glade 把 contouring
	 * 格单开一个 `contouring_grid_dims`、而不是复用 heightmap 分辨率的原因。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "10.0"))
	float StairCellSize = 100.0f;

	/** 每级台阶的升高 cm。等距分层（不学 TG 的非等距 mix(0.8, 10, i/16)）：石阶语义要求等高。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "1.0"))
	float StairStepHeight = 30.0f;

	/** 道路权重阈值：格心的 R 通道过阈才算"路经过这里"。与岩壳的隐藏阈值共用同一个数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StairRoadThreshold = 0.35f;

	/**
	 * **塑形物峰高低于这个数就整座不出台阶**（cm）。0 = 关掉这条门控。
	 *
	 * 2026-09-06 用户裁决：矮包不需要台阶。默认 60 = 2 × `StairStepHeight` —— 一级 30 cm 的
	 * 落差是个路沿不是楼梯，抬脚就上去了，摆一排石块反而像路障。
	 *
	 * ⚠️ 判据是**整座塑形物的峰高**，不是本格的地面高度：按本格高度判会把高包裙边最低那几级
	 * 也删掉（那里本格高度同样很小），而那几级正是人要踩的。峰高 = 塑形物 actor 相对地面的高度
	 * （2026-09-29 起台顶就是 actor 高度），落地在 `GroundShaperHeightAtXYTallOnly`。
	 *
	 * ⚠️ 高包与矮包重叠的那一片**照旧出台阶** —— 门控问的是「这里有没有够高的包」，
	 * 高包的裙边只要够得到，台阶就该有。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "0.0"))
	float StairMinMoundHeight = 60.0f;

	/**
	 * **掐掉每座最顶上那一级台阶**（2026-09-06 用户裁决）。
	 *
	 * 台顶那圈等值线贴着平台边缘走，人踩到它的时候已经站在台上了 —— 那一级不承担上行，
	 * 只在平台沿上留一道绊脚的坎。
	 *
	 * ⚠️ 顶层号由**整座的峰高**推，不是本格高度：`ceil(峰高 / StairStepHeight) − 1` 是最顶那层，
	 * 上限压到它减一。落地在 `CSGroundStairs.usf` 的层数钳位一段，峰高来自 `GroundShaperPeakAtXY`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	bool bStairDropTopStep = true;

	/**
	 * 沿坡**向上**推进 cm，让踏面扎进坡里（原型 attribwrangle7 的 Noffset）。负值则向外挑出。
	 *
	 * ⚠️ **默认值不是调出来的，是闭式解：`StairEmbed = StairBlockSize.X / 2`。**
	 *
	 * 摆放规则是"块**心**落在等值点、块底钉在该层高度上"，于是块的**下坡底棱**悬在空中，
	 * 悬空量 `f = (X/2 − e) · 坡度`。把 e 取成 X/2 时下坡底棱正好落在自己那条等值线上 ——
	 * **`f ≡ 0` 与坡度无关**，等值线因此变成踏面的前缘（踏步鼻），这正是"台阶"该读出来的样子。
	 *
	 * 两次踩过的坑都是**拿一个与坡度相关的数当默认值**：
	 *   · 继承来的 25：默认剖面（台高 300 / 羽化 400，坡度 1.125）下把 30 cm 踏步整个埋掉，
	 *     裙边中段只露 3 cm。
	 *   · S1 那轮改的 10：踏面是露出来了，但 `f = +26.8 cm`（岩壳把台高抬到 700 之后，
	 *     坡度 1.342）—— 每块石阶的前缘都吊在半空，整条读成一道**支棱着的石墙**而不是台阶。
	 * 两个数都会在剖面一改就重新出错，X/2 不会。
	 *
	 * 与之配套、同样与坡度无关的两条不变量（改 e 时必须一起复核）：
	 *   · 可见踏面 `T = min(StepHeight, StairBlockSize.Z) / 坡度` —— 取 X/2 时上界恰好由
	 *     `Z ≥ StepHeight` 保证（本文件下面那条"Z 略大于 StairStepHeight"就是它）。
	 *   · 可见踏步高恒 = `StairStepHeight`（相邻两块的顶面差），**与 e、与坡度都无关**。
	 *
	 * ⚠️ 剩下的一条**没法用 e 修**：踏步高 / 踏面 = 坡度本身（22.4 cm 踏面配 30 cm 踏步，
	 * 坡度 1.342）。石阶贴着地形走，它的坡度就是地形坡度，`StepHeight` 只能同时缩放两者。
	 * 想要"能走"的比例只能改地形剖面或让路径斜切上坡 —— 那是塑形物/路径的事，不是本参数的。
	 *
	 * 逐实例进深抖动（`StairSizeJitter`，±12%）会让 `f` 在 `±0.12 · X/2 · 坡度`（约 ±4.8 cm）
	 * 上下浮动 —— 量级已经落到裙边噪声之下，读起来就是石头本该有的参差，不是悬空。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	float StairEmbed = 30.0f;

	/** 相对该层高度的竖直微调 cm（块底默认贴在等值线上，见 FScanParams::Rise）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	float StairZOffset = 0.0f;

	/**
	 * 想要的石阶三轴尺寸 cm：X = 踏面进深、Y = 沿等值线的长度、Z = 高。
	 * 内部除以基础网格自身的包围盒尺寸得到缩放 —— 喂居中单位立方体（`stairs_step` 是 100³
	 * 的居中盒）时这里填的就是最终尺寸；某一轴填 0 表示"这一轴保持网格自带的尺寸"。
	 *
	 * 两条尺寸关系（不满足时画面会露馅，但一个断言都不会红）：
	 *   · Y ≈ `StairCellSize`。⚠️ **S2 起 Y 只是下限**：渲染长度跟着本格的等值线**弦长**走
	 *     （`max(弦长, 本值) × StairLengthBloat × (1 + 单侧拖动)`）—— 弦长在 [格距, √2×格距]
	 *     上变，定长块在斜走的等值线上会露正缝。详见 `StairLengthBloat` 的注释。
	 *   · Z **略大于** `StairStepHeight` —— 地面是光滑斜坡不是真台阶，块底钉在等值线上时，
	 *     踏面前缘一定悬空（悬空量 = 半个进深 × 坡度）。Z 比层高大一截，相邻两级在竖直方向
	 *     重叠，下一级正好挡住上一级的悬空前缘，整条读起来才是连续的石阶而不是一排浮块。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs")
	FVector StairBlockSize = FVector(60.0, 100.0, 45.0);

	/** 单格最多跨几层。`LiftHeight` 越大层数越线性膨胀，没有这个钳位一格就能吃光固定容量。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "1", ClampMax = "256"))
	int32 StairMaxLayersPerCell = 32;

	/**
	 * 石阶实例的**固定**容量。零回读的代价与保证：CPU 不知道实际有几级，所以容量一次性定死、
	 * 永不重算，越界静默丢弃。改它会重新分配（一次阻塞），所以它是配置项、不是运行期的量。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs", meta = (ClampMin = "64", ClampMax = "1048576"))
	int32 MaxStairInstances = 4096;

	// -------------------------------------------------------------------------
	// 小石子（TG `_rocky_terrain_stairs_stairs.cs:511-547`）
	//
	// TG 的石阶 CS 每摆一级台阶，都有 15% 的概率在同一段等值线上再撒一颗小石子。它是"石阶不像
	// 一排整齐积木"的主要来源之一，成本只有一次哈希 —— 但它必须与 S2 的抖动**共用格身份哈希**，
	// 不能用 InterlockedAdd 的槽位（理由见 `StairJitterSeed` 那一节，S1 已经栽过一次）。
	//
	// 石子与石阶是两张基础网格，而一个实例组件只绑一张 ⇒ 走**自己的**组件与实例源。
	// -------------------------------------------------------------------------

	/**
	 * 小石子的基础网格（提取资产里现成的 `stairs_pebble`）。
	 * **留空 = 只关掉石子这一支**，石阶照常（与 `StairMesh` 留空关掉整条路径不是一回事）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	TObjectPtr<UStaticMesh> StairPebbleMesh;

	/** 石子的整体覆盖材质。同石阶：**留空 = 逐段画 `StairPebbleMesh` 资产自带的材质**，设了 = 盖住每一段。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	TObjectPtr<UMaterialInterface> StairPebbleMaterial;

	/** 每段等值线额外出一颗石子的概率。0.15 是 TG 反编译实测（`:522` 的 `> 0.85`）。0 = 关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float StairPebbleChance = 0.15f;

	/**
	 * 石子最长轴的目标尺寸 cm，`X = 下限 / Y = 上限`（均匀缩放，在这个区间里随机取）。
	 *
	 * 默认值是 TG 那条 `mix(0.2, 0.4)` 的 cm 换算，不是调出来的：TG 的均匀缩放乘在
	 * `stairs_pebble` 原件上，而原件实测最长轴 1.352 m ⇒ **27 – 54 cm**。
	 * 这里写 cm 而不是写 0.2/0.4，是为了与 `StairBlockSize` 同一口径 ——
	 * 缩放系数在内部由本值除以基础网格自身的包围盒算出来，将来谁把资产按别的比例重导，
	 * 石子的**世界尺寸不会跟着悄悄变**（写系数就会）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Pebble")
	FVector2D StairPebbleSize = FVector2D(27.0, 54.0);

	// -------------------------------------------------------------------------
	// S2：逐实例抖动
	//
	// **随机源是格身份（格坐标 + 层号 + 段号 + 种子），不是 InterlockedAdd 拿到的槽位。**
	// 槽位由线程组完成顺序决定，同一份世界状态两次 dispatch 可以给同一块石阶不同的槽；
	// 而这条路上重扫是**每一 dab 一次**的，用槽位当种子的症状就是"画一笔路，整片石阶乱跳"。
	// （S1 写 packed 行 .w 的 PerInstanceRandom 用的正是槽位，本轮一并订正。）
	//
	// TG 的对位物：`_rocky_terrain_stairs_stairs.cs:504` 的种子是 `uint((Tx+Ty+Tz)*100)`
	// —— 实例平移量之和量化到 cm，同样是位置派生、与槽位无关。改用格身份是因为拖塑形物时
	// 高度场在动、等值线跟着滑，位置派生的种子会在整个拖动过程里不停重掷。
	// -------------------------------------------------------------------------

	/**
	 * 长度轴（沿等值线）的**胀大系数**：渲染长度 = 弦长 × 本值，摆位一步不动 ⇒ 相邻两块互相
	 * 穿插，**砖缝是负的**。小于 1 会被夹回 1 —— 正缝正是它要消掉的缺陷。
	 *
	 * ⚠️ **它和 `ACSHouseActor::FrameBrickBloat` 不是同一件事，别照抄那边的做法**：
	 * 门框砖的槽距由 `SolveBlockLayout` 定成近恒定，所以一个常数系数就能保证负缝；石阶的槽距
	 * 是**几何决定**的 —— 每格每层出一级，等值线与格成 45° 时弦长 = √2 × 格距，比轴向大 41%。
	 * 定长块（S1 的做法）在斜走的等值线上会露出 0.41 × 格距 的**正缝**，任何常数系数都盖不住
	 * 一个随走向变化 √2 倍的量。所以长度轴必须**跟着弦长走**（`StairBlockSize.Y` 退化成下限），
	 * 本系数只负责把"恰好首尾相接"推成"确定互相穿插"。
	 *
	 * TG 的实证是 k ≡ 1（`_rocky_terrain_stairs_stairs.cs:509` 把 X 轴缩放直接写成
	 * `distance(A, B)`）—— 它靠石头自身的不规则轮廓 + VS 的 bevel 噪声藏接缝；我们用的是
	 * 光板方盒，零缝会读成一条折痕，所以叠一个常数胀大。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "1.0", ClampMax = "1.5"))
	float StairLengthBloat = 1.06f;

	/**
	 * 长度轴的抖动幅度（比例），**单侧：只增不减**。
	 *
	 * 单侧是硬要求不是口味：负缝的保证是"每块的长度 ≥ 自己那条弦"，对称抖动会让两块同时缩、
	 * 把接缝重新拉开 —— 与门框砖"只胀有邻居的那一轴、且只增不减"是同一条纪律。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StairLengthJitter = 0.10f;

	/**
	 * 进深（+X）与高（+Z）的**对称**抖动幅度（比例）。这两轴沿等值线方向都没有邻居，所以敢双向抖。
	 *
	 * ⚠️ 与 `StairStepHeight` 有一条不成文的关系：`StairBlockSize.Z × (1 − 本值)` 必须仍然
	 * **大于** `StairStepHeight`，否则抖矮的那些块不再与下一级竖直重叠，踏面前缘的悬空就露出来
	 * （断言：GroundStairs.StepOverlap）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StairSizeJitter = 0.12f;

	/**
	 * 绕世界 +Z 的偏航抖动**度数**，±。
	 *
	 * 只抖 yaw：踏面必须是水平的（贴着坡面倾斜的是岩壳不是台阶，见 kernel 文件头），
	 * pitch/roll 一抖这条就没了。
	 *
	 * ⚠️ **它会吃掉负缝**：块转过 δ 之后沿等值线方向的投影长度只剩 cos δ 倍，所以负缝的
	 * 充要条件是 `StairLengthBloat × cos(本值) > 1`。默认 1.06 × cos6° = 1.054 ⇒ 每个接头
	 * 仍然互相穿插约 5.4% 弦长。调大本值或调小胀大系数会直接把这条断言打红。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter", meta = (ClampMin = "0.0", ClampMax = "30.0"))
	float StairYawJitter = 6.0f;

	/** 抖动种子。同种子同格身份 ⇒ 逐位相同的结果（`RebuildStairs` 幂等的一部分）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Stairs|Jitter")
	int32 StairJitterSeed = 1;

	// -------------------------------------------------------------------------
	// Rock Shell（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）
	//
	// **裁决二：碎石归地面，不归塑形物**（这推翻了计划 D9 :504 岩石那一半；石阶不变，
	// 仍归塑形物）。碎裂图案覆盖整张地面、与任何单座塑形物无关，mask 由**全部塑形物合成后**
	// 的坡度决定，归任一座都不对。删掉塑形物 → 高度场塌回 → 坡度降到阈下 → 那批胞腔自己
	// 写 NaN，**归属簿记整个消失**，不是变简单。
	//
	// 与石阶严格互补：石阶要路、碎石要没路。但两者实现方式不同（裁决五）——
	// 石阶用 `StairRoadThreshold` 阈值门控，壳**没有任何 road 显隐判据**，只有连续下沉。
	// -------------------------------------------------------------------------

	/**
	 * 整条岩壳路径的总开关。
	 *
	 * ⚠️ **默认开，而且刻意不拿"材质为空"当开关**（`StairMesh` 那条就是这么写的，而两张演示
	 * 关卡里它一直是 NULL，石阶因此在画面里是一撮黑块，单测与回归却全绿）。壳的几何是生成的，
	 * 材质为空只会退回引擎默认表面材质 —— 那仍然是"画出来了"，所以材质不能兼任开关。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	bool bRockShell = true;

	/**
	 * 顶点法线走**全平均**（按静止姿态的重合组，面积加权），而不是逐三角的面法线。
	 *
	 * ⚠️ **2026-09-04 改为默认关 = TG 原样**（用户：先按 TG 的方案做一版）。TG 只有一趟，
	 * `_rocky_terrain_displace.cs:768` 每三角写 `normalize(cross(...))`，全程不平滑；软化完全
	 * 靠 PS 那条噪声混合。**打开 = 我们比 TG 多的那一层**（带 `RockShellSmoothAngleDeg` 阈值），
	 * 用来从根上治刻面 —— 这一个勾就是「TG 原样」与「阈值平滑版」的 A/B 开关。
	 *
	 * 2026-09-03 曾裁决默认开且不设阈值：面法线给的是"每个三角一个硬边"，盖面在缓坡上被地形
	 * 弯出来的那点起伏全部读成刻面，用户判为"非常干扰"。那一版的问题是把折痕也一起圆掉了。
	 *
	 * ⚠️ **2026-09-04 用户裁决：加回夹角阈值**（`RockShellSmoothAngleDeg`），推翻上面「不设
	 * 阈值、全平均」那一半。全平滑之后盖裙折痕整个圆掉，材质再怎么画也补不回一道真硬边。
	 * 当初否掉阈值的理由是「有阈值就得烘逐边邻接、还要两趟比较角度」——**那条不成立**：按
	 * 面法线夹角判只要逐角的重合组表，而第二趟本来就在读每个入射三角的位置，一行点积、
	 * 零额外烘焙、零额外 dispatch。
	 *
	 * ⚠️ 另一条同日被推翻的：原记「组不跨胞腔 ⇒ 石头之间照旧是硬边」。实测 74,180 条内部边里
	 * **8,743 条跨胞腔**（11.8%），相邻石头确实共享顶点 —— 阈值一并管住了这里：石头之间的
	 * 夹角通常远大于阈值，于是自动断开。
	 *
	 * 关掉 = 回到面法线。留这个开关是为了调材质时能直接 A/B，不用重编。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	bool bRockShellSmoothNormals = false;

	/**
	 * 平滑的夹角阈值（度）。入射三角的面法线与本三角面法线夹角超过它就不参与平均 ⇒ 那条边
	 * 在几何这一层是硬边。默认 30 沿用 Houdini 原型的 `cuspangle=29.9`。
	 *
	 * 两端是有意义的调试档：**0 = 只认自己**，等价于关掉 `bRockShellSmoothNormals`（面法线，
	 * 每三角一道硬边）；**180 = 全平均**，等价于 2026-09-03..09-04 之间的行为。
	 * 盖裙折痕的二面角远大于 30°，所以它是硬的；盖面被地形弯出来的缓起伏远小于 30°，照旧平滑。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell",
		meta = (EditCondition = "bRockShellSmoothNormals", ClampMin = "0.0", ClampMax = "180.0"))
	float RockShellSmoothAngleDeg = 30.0f;

	/**
	 * 岩壳材质。为空退回引擎默认表面材质（壳照样画，只是灰的）。
	 *
	 * 实际绘制的是它的一个**动态子实例**（`RockShellMaterialInstance`），只多带一个标量
	 * `RockShellPatternScale` = 本 actor 的图案缩放，供假倒角材质把 TG 原生的图案口径换算到
	 * 世界（`Docs/TinyGlade/RockShell_EdgeBevel.md`）。网格的 `Materials[0]` 仍是本资产 ——
	 * `SaveToStaticMesh` 带走的是它，瞬态实例进不了资产。演示关卡填的是 `MI_rocky_terrain`
	 * （母材质 `M_TG_Texture`，倒角子图挂在它的静态开关 `RockShellBevel` 后面）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	TObjectPtr<UMaterialInterface> RockShellMaterial;

	// -------------------------------------------------------------------------
	// 程序化碎裂图案（2026-09-30 用户裁决：用程序化方案、M = 16；取代 TG 原件那一张 tile）
	//
	// 图案 = 相对地面原点的抖动网格 Voronoi，任意 XY 都能求值：没有覆盖上限、不平铺也不重复，
	// 地面多大都一样。只有「可能长石头」的胞腔（塑形物裙边环带）才分配槽位，开销 ∝ 石头面积。
	// 设计与离线证据：`Docs/TinyGlade/RockShell_Sparse.md`、`Scripts/PrototypeRockShellSparse.py`。
	// 改这一组任何一项，胞腔多边形都会变 ⇒ 槽位池整体重建（阻塞一次）。
	// -------------------------------------------------------------------------

	/**
	 * 种子格距 cm（= 胞腔平均间距）。默认 193.6 = 0.35 × TG 原件的 553.1 cm —— 与 2026-08-31 裁决的
	 * `PatternScale = 0.35` 同一观感（8 m 裙边排得下四层）。
	 *
	 * **平面上的 TG 长度按本值 / 553.1 等比缩放**（唇宽 19.5 cm、`CellJitter` / `CellExpand` / `SkirtTilt`、
	 * 起伏噪声的底波长）—— 它们得跟多边形尺寸配套。**竖向的厚度 / 基准偏移 / 起伏幅度是世界 cm，不缩**：
	 * 2026-09-30 一度也按本值缩（默认档只剩 0.35 倍），用户看图判「露出的部分变少」，当天改回。
	 * 材质的 `RockShellPatternScale` 标量就是本值 / 553.1（假倒角的带宽按它换算）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "20.0", ClampMax = "5000.0"))
	float RockShellCellSize = 193.6f;

	/** 种子在格内的抖动 0..1（1 = 可落在格内任意处）。小于 1 胞腔更规整。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RockShellSeedJitter = 1.0f;

	/**
	 * 成片疏密：种子整体过一个低频位移场的幅度（**单位 = 格**）。0 = 均匀大小。
	 * TG 原件的胞腔是「一片小块、一片大块」地聚着的，这一项给的就是那个成片感（离线实测 0.5 ⇒ 面积 CV 0.28，
	 * 最小内切半径 30 cm，没有细条）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RockShellClusterWarp = 0.5f;

	/** 成片疏密的波长（**单位 = 格**）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "1.0", ClampMax = "64.0"))
	float RockShellClusterWavelength = 3.0f;

	/**
	 * 逐粒大小差异：功率图权重半径 / 格距。0 = 普通 Voronoi。
	 * ⚠️ 要逼近 TG 原件的面积离散度（CV 0.43）得开到 0.6~0.7，但 0.4 起就会出细长胞腔（离线实测 V = 0.6 时
	 * 1.5% 的胞腔内切半径 < 20 cm，唇宽会被夹小）。默认关。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RockShellSizeVariance = 0.0f;

	/**
	 * 每胞腔的边界点数 M（角点全保留，其余按边长分配）⇒ 每胞腔 5M 个三角。
	 * 16（用户 2026-09-30 定）= 80 三角 / 胞腔，与 TG 原件的 81.4 同密度；12 省 25% 显存。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "12", ClampMax = "24"))
	int32 RockShellBoundaryPoints = 16;

	/**
	 * 槽位池上限（胞腔数）。池按需要长（活跃数 × 1.5，拖动期不长、松手补齐），长到这里为止；
	 * 超了告警并按格键截断。M = 16 时每槽 20.3 KiB ⇒ 8192 槽 ≈ 162 MiB。上限同时受一维派发
	 * 65535 组的约束（M = 24 时 ≈ 11,650 槽），所以夹在 8192。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Pattern", meta = (ClampMin = "64", ClampMax = "8192"))
	int32 RockShellMaxCells = 8192;

	/** 当前参数下的图案口径（`CSRockShell::FPatternParams`，已夹过范围）。建池、激活、披挂共用这一份。 */
	CSRockShell::FPatternParams MakeRockShellPatternParams() const;

	/**
	 * 厚度（`RockShellCellRelief`）所乘 mask 的**下限**。0 = TG 口径，1 = 完全不衰减。
	 *
	 * ⚠️ **2026-09-30 起乘的是壳的 mask `smoothstep(0.1, 0.2, Rock)`（TG 的 `_440`），不再是 `Rock` 本身**
	 * —— 下面 08-31 那段「Rock 只有 0.1~0.5、厚度只剩几成」的现象，根子就是当时把 `_440` 读成了 `Rock`
	 * （反编译 `displace_rocky_terrain.cs:203/:382/:577`）。`_440` 在 Rock = 0.2 就饱和，本值因此几乎用不上。
	 *
	 * ⚠️ **这一条是"壳有没有体积"的主开关，不是微调**（2026-08-31 实测）。kernel 原本写
	 * `Relief * Rock`，而 `Rock = smoothstep(SlopeLo, SlopeHi, |∇h|)`：演示土台最陡才 1.3125，
	 * 只有极窄一圈能到 `Rock ≈ 1`，**大部分活胞腔的 Rock 只有 0.1~0.5** ⇒ 30 cm 的设定值
	 * 实际只剩 3~15 cm，整张壳读成"开裂的平毯"。TG `displace:577` 那条是在真悬崖上跑的，
	 * 那里 Rock 本来就 ≈1，照抄到缓坡上就失真。
	 *
	 * ⚠️ **默认已改回 0（TG 口径）** —— 本值一度默认 0.5，作为"壳没有体积"的临时补丁。
	 * 后来反编译 `_rocky_terrain_displace_rocky_terrain.cs:721` 查清了真机制：TG 的
	 * `Relief × mask` 与我们原来写的**一模一样**，它的体积来自另外两项我们完全没有的东西
	 * （突起量 ∝ 坡度、以及 `rocky_terrain.y` 那份偏移）。体积的职责因此交给下面
	 * 「石头隆起」那一组（`RockShellRiseMultiplier` 等），本值退回纯调参用途、默认不偏离 TG。
	 *
	 * 抬它的副作用是实测过的：0.5 会让 `RockShell.DrapesOnSlopes` 的最大垂直距离
	 * 从 66 cm 以内涨到 83.9 cm —— 那条容差是按"厚度被 mask 压着"标定的。
	 * 想再抬它就得连那条容差一起重新标定，别只改一头。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float RockShellReliefFloor = 0.0f;

	// =========================================================================
	// 「石头隆起」（用户规格 2026-08-31）—— 六个参数，全部蓝图可调
	//
	// 这一组回答的是"包边为什么没有体积"：石头**不再只是贴着地面披挂**，而是按地形隆起的倍数
	// 自己再抬起来一截，边缘再扎回地里。与上面 `RockShellCellRelief`（逐胞腔的壳厚）是两件事：
	// 那个给的是"每块石头有多厚"，这一组给的是"整片石头比地面高多少"。
	// =========================================================================

	/**
	 * ② ⑤ 切分后的每一片沿径向**朝外**扩张的距离（**TG 原生口径 cm**，世界 = × `RockShellCellSize` / 553.1），
	 * 用来闭合片与片之间的缝。
	 *
	 * 图案本身是零重叠零空隙的（盖 86.60% + 裙 13.40% = 100.0000%），所以缝不是图案漏的 ——
	 * 它来自逐片的位移（`CellRelief` 让相邻盖错开、`CellJitter` 让它们胀缩）。本值把每片整体
	 * 往外推一点，让相邻片互相压住，与门框砖的 `FrameBrickBloat` 是同一条 TG 纪律（**负缝**）。
	 *
	 * ⚠️ 方向靠 `-DirToCentroid`：契约写的是朝外，**实测指向质心**，所以 kernel 里用的是减号。
	 * 位移带一个到质心的半径淡入，否则顶盖内部点会被推过质心、翻转周围三角。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float RockShellCellExpand = 12.0f;

	/**
	 * 裙圈倾斜（用户方案 2026-08-31）：**只把底圈**再沿 −DirToCentroid 朝外推的距离
	 * （**TG 原生口径 cm**，世界 = × `RockShellCellSize` / 553.1）。
	 *
	 * 目的与 `CellExpand` 不同：那个整片平移（含盖），闭合的是缝的**顶宽**；本值只动底圈，
	 * 改的是裙墙的**倾角** —— 竖直裸墙变外撇斜壁，相邻胞腔的底圈各自越过 Voronoi 边界钻到
	 * 对方裙下，两片斜壁在 V 槽中段**互相交叉**，缝隙从机制上封死（黑槽 = 相邻盖浮高不同时
	 * 暴露的竖直墙间隙，斜壁交叉后没有视线能进槽底）。
	 *
	 * 盖的形状一动不动 ⇒ 图案观感、`DrapesOnSlopes` 的披挂契约都不受影响（每个顶点仍在
	 * 自己 XY 采地面高度，只是底圈的 XY 挪远了一点）。调 0 = 旧行为（竖直裙墙）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0", ClampMax = "200.0"))
	float RockShellSkirtTilt = 20.0f;

	/**
	 * ③ 石头隆起相对**地形隆起**的倍数：`石头高 = 地形高 × 本值`。
	 *
	 * ⚠️ **默认已从 1.25 归回 1.0（2026-08-31 用户看图裁决）**：TG 的壳**从不离开地形** ——
	 * 它的位移是有界的（基准 ±0.2/−0.4 m + 厚度 0.3 m + 起伏 ∝ 坡度），而本值是随台高线性长的
	 * **无界**量。演示档 `Lift=700` 下 1.25 意味着台肩处 +175 cm，再叠基准/浮高/噪波就是
	 * 约 2 m 的石墙冠 —— 画面上读成一圈**独立的火山口壁**，台顶陷在墙圈里面。
	 * 体积的职责移交给 TG 口径的三层（`BaseLift`/`BaseSink`、`CellRelief`、坡度比例起伏）。
	 * 本组四个参数保留为**风格化旋钮**（默认全中性），拧它们就是有意离开 TG 形态。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float RockShellRiseMultiplier = 1.0f;

	/**
	 * ④ 隆起时叠加的噪波幅度 cm。按隆起量淡入 —— 平地一点不加，否则整片地面起毛刺。
	 * ⚠️ 默认 25 → 0（同上那次裁决）：TG 没有这条沿世界 Z 的噪波，它的表面细节全在
	 * `RockShellNoiseAmount` 那条**沿法线、∝ 坡度**的起伏里。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0"))
	float RockShellRiseNoiseAmount = 0.0f;

	/** ④ 隆起噪波的波长 cm。太短会低于地面网格采样率，看着像"石头和地面对不上"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "1.0"))
	float RockShellRiseNoiseWavelength = 400.0f;

	/**
	 * ⑥ 石头隆起的**台顶外扩距离** cm：隆起的衰减比地面**晚**这么远才开始。
	 *
	 * 实现是把每座塑形物的 `Radius` 临时加上本值再求值（不是缩 `Falloff`）—— 前者把台顶整体
	 * 外推、裙边形状原样保留；后者会把裙边压陡，连带改掉坡度 mask 与石阶的等值线。
	 *
	 * ⚠️ 默认 150 → 0（同上那次裁决）：外扩让壳在地面已经落下去的地方还端着满高，
	 * 正是截图里"近垂直石墙外立面"的来源。TG 没有对位物。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0"))
	float RockShellRiseExtend = 0.0f;

	/**
	 * ⑧ **末端边缘的隆起相对该点地面高度的上限比例，必须 < 1。**
	 *
	 * 硬不变量：石头的末端边缘隆起值一定小于可采样的地面高度 —— 石头必须扎回地里，不许悬空。
	 * 只在边缘生效（权重取 `1 − RockMask`：内部不约束，band 外缘满约束）。
	 * 取 1.0 会让最外圈与地面共面 ⇒ 一圈 z-fighting，所以内部再夹到 0.999。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0", ClampMax = "0.999"))
	float RockShellEdgeCeiling = 0.9f;

	/**
	 * 壳的 mask 满时，整张壳沿**世界 Z** 浮起的高度（**世界 cm**，不随胞腔缩放 —— 理由见 `RockShellCellRelief`）。
	 * TG `displace_rocky_terrain.cs:563` 的 +0.2 m。想让石头整体更「露出」地面、又不加深石头间的缝，先调它。
	 *
	 * 与 `RockShellRiseMultiplier`（③）是两件事，而且**互补**：③ 是随台高线性长的无界量，
	 * 在缓坡小土台上几乎给不出体积；本项是有界常量，多高的台子都只浮这么多，缓坡上照给。
	 * 完整式子是 `lerp(−BaseSink, +BaseLift, saturate(Mask − Road))`，Mask = `smoothstep(0.1, 0.2, Rock)`，
	 * 对位 TG 的 `mix(-0.4, 0.2, clamp((_440 + rocky_terrain.z) − 10·path, 0, 1))`
	 * （`rocky_terrain.z` 是笔画自带的岩石度，本项目没有那条通道）。
	 * ⚠️ 2026-09-30 之前权重用的是 `Rock`：条带里 Rock 大多 < 0.5 ⇒ 偏移停在 −BaseSink 附近 ⇒ 壳整片沉进地里、
	 * 缝里露出地面，读成沟壑。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0"))
	float RockShellBaseLift = 20.0f;

	/**
	 * mask 空、或路上时，整张壳沿**世界 Z** 沉下的高度（**世界 cm**，不随胞腔缩放）。
	 * TG 同一行的 −0.4 m。
	 *
	 * ⚠️ 它同时在给 ⑧ 兜底：末端 `Rock → 0` ⇒ 本项取满 ⇒ 外缘被额外往地里按。
	 * 调到 0 不会报错，只是外缘会更容易露出来。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell|Rise", meta = (ClampMin = "0.0"))
	float RockShellBaseSink = 40.0f;

	/** 坡度软阈的下端：|∇h| 低于它完全没有壳。与 TG 的 `smoothstep(0.75, 1.25, ·)` 同口径。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellSlopeLo = 0.75f;

	/** 坡度软阈的上端。必须 > `RockShellSlopeLo`，否则 smoothstep 退化成 0/0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellSlopeHi = 1.25f;

	/**
	 * 道路权重的足迹增益（TG 实测 10×）：`saturate(road × 本值)` 就是壳眼里的"这里有路"，
	 * `road ≥ 1/本值` 即足迹之内。足迹再经 `RockShellRoadBlurRadius` 糊开才去压壳。
	 *
	 * ⚠️ **这是壳与石阶"严格互补"的实现方式**，而互补不是靠阈值判断做到的（裁决五禁止
	 * 在显隐判据里出现 road）：`1/RoadFade` 必须**小于** `StairRoadThreshold` —— 默认
	 * 1/10 = 0.1 < 0.35，即石阶长出来的地方一定落在足迹之内。
	 * 单测 `RockShell.Contract` 守着这条不等式（`CSGroundRockShellTests.cpp`）。
	 * 模糊之后"足迹内 = 沉到底"只在离足迹边缘 ≳ 半径处严格成立，边缘本身只沉一半 ——
	 * 石阶离足迹边缘还隔着笔刷衰减带那一截；默认值下沉一半的壳顶也已在地面以下约 40 cm
	 * （`RoadSink` 是米级，壳自身厚度与起伏合计才几十厘米）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "1.0"))
	float RockShellRoadFade = 10.0f;

	/**
	 * road 满值时沿地形法线的下沉量 cm（TG 三项合计约 1.6 m）。
	 *
	 * ⚠️ **必须比壳自身的起伏厚**，否则路上还会露出石头尖。它也是"沉下去不是隐藏"这条裁决
	 * 的唯一执行面：用 NaN 关掉的话，画路时三角会一个一个啪地消失（popping）；连续沉降的
	 * 观感才是"石头慢慢埋进土里"。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellRoadSink = 160.0f;

	/**
	 * 路足迹压到壳上之前的**高斯模糊半径** cm（= 核的单侧伸展；σ = 本值 / 3，核截在 3σ）。
	 * 0 = 不模糊（与 2026-09-11 之前的行为只差在跨足迹边缘的那一格里 saturate 与双线性的先后）。
	 *
	 * 用户裁决 2026-09-11（看图：「岩壳受到道路的下压太过激烈，应该采样道路经过模糊后的 buffer」）。
	 * 病根：默认笔刷边缘的道路权重从 0 爬到 1/RoadFade = 0.1 只要 ~30 cm，`RoadSink` + 基准偏移
	 * 那两米多的落差全挤进不到一个三角里，出图是一道道 V 形折痕。糊开之后足迹边缘处沉一半，
	 * 内外各按高斯累积分布走完：**缓坡宽度 ≈ 本值**（10%→90% 约 0.85 × 本值，2%→98% 约 1.4 × 本值）。
	 * 默认 300 ⇒ σ = 1 m。看得见的只是缓坡最外那一截（沉降超过壳自身几十厘米的高度就没进地面了），
	 * 所以 `RoadSink` 越深，露出来的那段越陡 —— 两者要一起调。
	 *
	 * ⚠️ **先截足迹再糊**，不是先糊原始权重再乘 `RoadFade` —— 反过来的话 ×10 只取模糊场最底下
	 * 那 10% 的尾巴，缓坡又被压回一刀。机制与 TG 的差异写在 `CSGroundRoadField.usf` 的头注释（岩壳与地被共用的地面路场）。
	 *
	 * ⚠️ 调大有两个代价：壳在离路更远处就开始沉（路两侧的无石带变宽），以及比 ~3 × σ 还窄的路
	 * 中线上不再沉到底。上限 32 格（kernel 侧的抽头钳位），50 cm 格距下即 16 m。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0", UIMax = "1000.0"))
	float RockShellRoadBlurRadius = 300.0f;

	/**
	 * 逐胞腔的最大径向收缩（TG 原生 cm；世界 = × `RockShellCellSize` / 553.1）。
	 * 与顶盖浮高共用随机数 r：位移 = 本值 × lerp(-0.375, 1, r)，正值朝质心收缩。
	 * 默认 52 对应 TG 的 -19.5..52 cm；浮得高的盖同时收得多。0 关闭这层胀缩。
	 * 程序化小胞腔保留半径淡入，内收上限为当前半径的 75%，避免顶点穿过质心。
	 * 当前独立裙圈尚未复现 TG 跨 cell_id 的共同接缝，不能仅凭这项保证消除沟壑。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellCellJitter = 52.0f;

	/**
	 * 壳沿地形位移法线的名义厚度（世界 cm，不随胞腔缩放）：底圈沉本值，顶圈浮 0..本值。
	 * 位移向量采用 TG 的两个单位切向量叉积，不再归一化；斜向陡坡实际幅度小于名义值。
	 * **块感的唯一来源。** 乘的是壳的 mask（TG `_440`），见 `RockShellReliefFloor`。
	 * 调大 = 石头更厚、相邻石头的高低差更大，但底圈也沉得更深（缝更深）—— 要配 `RockShellBaseLift` 一起抬。
	 *
	 * ⚠️ **竖向四项（本值、`BaseLift`、`BaseSink`、`NoiseAmount`）是世界 cm，平面四项（唇宽、`CellExpand`、
	 * `SkirtTilt`、`CellJitter`）才随胞腔缩放。** 2026-09-30 同一天里竖向也按胞腔缩过（默认 1.94 m 的石头只剩
	 * 10.5 cm 厚），用户看图判「露出的部分变少」，改回不缩。「缝如沟壑」的病根是基准偏移 / 厚度乘了原始 Rock
	 * （见 `RockShellBaseLift`）与高频起伏 + 磕碰，不是厚度没缩；平面四项要缩，是因为它们得跟多边形尺寸配套。
	 *
	 * TG `displace:577` 的 `mix(-0.3, 0.1*mix(0,3,rand(cell)), cell_bby)`，而 `cell_bby`
	 * **实测就是 `bIsTopRim`**（顶圈恒 1 / 底圈恒 0，零例外）—— `Ground_Shaper.md` 读作
	 * 「本胞腔沿法线凹还是凸」已被推翻。所以它不是"有的胞腔沉、有的凸"，而是给整张壳一个
	 * 沿法线的厚度，再由逐胞腔的随机浮高让相邻的盖互相错开。
	 *
	 * ⚠️ **不是可选项**：调到 0，披挂出来的就只是一张贴着地形的毯子，读不成一块块石头
	 * （计划 P3 把这一层列为"让壳读成石头"的那一层，实测重读之后结论不变）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellCellRelief = 30.0f;

	/**
	 * 表面起伏沿法线的幅度（**世界 cm**，不随胞腔缩放），**口径是「坡度 = 1 时的满幅」而不是常幅**。
	 *
	 * 逐字照 TG `displace_rocky_terrain.cs:618 + :721`：
	 * `本值 × 坡度 × mix(-0.2, 1 − smoothstep(4,7,坡度), n01) × smoothstep(0,0.3,Rock)`，
	 * n01 = `0.25 + 0.5 × noise(rest / RockShellNoiseWavelength + 20 × Rock)` ∈ [0.25, 0.75] ⇒ 因子 ∈ [0.1, 0.7]，**只鼓不瘪**。
	 * 默认 30 就是 TG 的 0.3 m。
	 *
	 * ⚠️ **2026-09-30 改回 TG**：之前的 n01 是 3 倍频 fbm（波长 1.5 m）—— 在 2 m 的石头上起伏不断，
	 * 缝两侧的盖沿拿到不同的鼓量，缝被啃得坑坑洼洼。TG 的 n01 是单倍频、底波长 ~130 m、靠 mask 域扭曲变化：
	 * mask 饱和的带内整块石头**均匀**地鼓，只在条带边缘相邻顶点才互不相关。
	 *
	 * 角点不加（TG `displace:579`）—— 角点被推动会让两侧的裙错开，缝张开或叠上。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellNoiseAmount = 30.0f;

	/**
	 * 起伏噪声 n01 的**底波长**（**TG 原生口径 cm**，× 胞腔缩放）。默认 13000 = TG `:618` 的
	 * `rest.xz × 0.5`（rest 以 65 m 为单位）。变化主要来自 `20 × Rock` 的域扭曲，底波长只兜 mask 恰好平坦的走廊。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "1.0"))
	float RockShellNoiseWavelength = 13000.0f;

	/**
	 * **可选的**加性边缘磕碰幅度 cm（世界口径）。**默认 0 = TG 原样**。
	 *
	 * 2026-08-31 用户要过「边缘磕碰」，当时把 TG `:618` 那条单倍频 + mask 域扭曲的噪声当成了一个**单独的加性项**；
	 * 2026-09-30 对照反编译重读：那条噪声在 TG 里是**起伏的 n01**（见 `RockShellNoiseAmount`），TG 并没有加性磕碰。
	 * 保留这个旋钮给风格化用；打开后同样集中在 mask 爬坡的条带边缘、角点照旧钉死。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "0.0"))
	float RockShellChipAmount = 0.0f;

	/** 加性磕碰的**空间底**波长 cm（世界口径）。主频来自 mask 域扭曲（20×）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell", meta = (ClampMin = "1.0"))
	float RockShellChipWavelength = 300.0f;

	/** 逐胞腔随机与表面噪声的种子。同种子同胞腔 ⇒ 逐位相同的结果（`RebuildRockShell` 幂等的一部分）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Rock Shell")
	int32 RockShellSeed = 1;

	// -------------------------------------------------------------------------
	// 塑形物裙边摆件（D12 锚点层的第五家）
	//
	// **归地面，不归塑形物**：完整依据（三条，都不是"哪边写起来方便"）写在
	// `CSGroundDecor.h` 的文件头，一句话是「塑形物只提供高度场，地面负责派生几何」——
	// 与石阶、披挂岩壳同一条归属理由，塑形物那一侧因此一行代码都不用加。
	//
	// 锚点取法也在那个头文件里：**解析剖面的等值带上按弧长布点**，
	// 锚点数 = 环周长 / `SkirtDecorSpacing` ⇒ 密度完全由锚点个数决定（D12 的口径），
	// 没有场、没有阈值、没有随机撒点。
	// -------------------------------------------------------------------------

	/**
	 * 关掉整条裙边摆件路径（不建组件、不分配显存、不发 dispatch）。留空网格表等价。
	 *
	 * ⚠️ **2026-09-06 改为默认关**（用户裁决：地形隆起之后不要自动长出柴堆 / 桶这类摆件）。
	 * 网格表与材质仍旧配在 `BP_TinyGladeGround` 上，所以这是一个勾就能拿回来的开关，
	 * 不是把这条路删了。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	bool bSkirtDecorEnabled = false;

	/** 摆件网格表（一张网格 = 一个 palette = 一个实例化组件）。全空 = 这一家关掉。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	TArray<TObjectPtr<UStaticMesh>> SkirtDecorMeshes;

	/**
	 * 摆件的整体覆盖材质。
	 *
	 * **留空 = 每张摆件网格逐段画它资产上挂的材质**（2026-09-15，与房子摆件的 `DecorMaterial` 同一口径）；
	 * 设了 = 盖住每一张网格的每一段（旧用法：`Scripts/TinyGladeMakeDecorMaterial.py` 那张只读顶点色的 `M_TinyGladeDecor`）。
	 *
	 * ⚠️ **母材质必须勾 `bUsedWithInstancedStaticMeshes`**：没勾的材质在实例路径上会被引擎
	 * **静默换成默认材质**，画面一片灰而所有 readback 断言照绿。`GetSkirtDecorUndrawableReason()`
	 * 逐段查每一张解析出来的材质（覆盖或资产）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	TObjectPtr<UMaterialInterface> SkirtDecorMaterial;

	/** 裙边环上每隔多少 cm 一个锚点（**弧长**，不是角度）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "40.0"))
	float SkirtDecorSpacing = 260.0f;

	/** 锚点落在裙边的哪一档：0 = 台顶边缘，1 = 裙边外沿。依据见 `CSHouseDecor::FParams::SkirtBandT`。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SkirtDecorBandT = 0.62f;

	/** 摆件之间的最小间距（球测）。两座塑形物挨得近时靠它挡掉挤在一起的那几件。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0"))
	float SkirtDecorMinSpacing = 140.0f;

	/** 道路权重过阈就不摆 —— 与石阶严格互补（路穿裙边的那一段长的是台阶，摆件让开）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float SkirtDecorRoadReject = 0.3f;

	/** 摆件缩放。clutter 网格实测 100–250 cm，原尺寸摆在裙边上比土台还高（同房子那边的 0.5 档）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.05"))
	float SkirtDecorScale = 0.6f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor", meta = (ClampMin = "0.0", ClampMax = "0.8"))
	float SkirtDecorScaleJitter = 0.18f;

	/**
	 * 逐实例随机的种子。⚠️ 随机源是 **(本座 actor 名的 CRC, 环上第几号)** 的身份哈希，
	 * **不是** `InterlockedAdd` 的槽位、不是塑形物在 `Shapers` 数组里的下标、也不是它的世界坐标
	 * —— 三条为什么都不行，逐条写在 `CSGroundDecor.h` 的文件头（S1 已经在槽位上栽过一次，
	 * 症状是"画一笔路全场变色"且没有任何断言报红）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Skirt Decor")
	int32 SkirtDecorSeed = 11;

	// -------------------------------------------------------------------------
	// 地被：草 + 花（第六条派生链）
	//
	// **归地面**，与石阶 / 岩壳 / 裙边摆件同一条理由：长不长草由两样东西决定 ——
	// **合成后**的高度场（坡太陡不长）与地面的**顶点色遮罩**（画了路的地方不长），
	// 两样都只有本 actor 手上有。完整依据与三条纪律写在 `CSGroundCover.h` 的文件头。
	//
	// 草与花是**同一个 kernel 的不同物种**（只差密度/遮罩/缩放/盐），一株一个 instance
	// —— TG 的草也是每叶一个 instance，没有"一簇"这个概念。
	// -------------------------------------------------------------------------

	/** 关掉整条地被路径（不建组件、不分配显存、不发 dispatch）。物种网格留空等价。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Cover")
	bool bGroundCoverEnabled = true;

	/** 草。一株 = 一片叶（`SM_TG_GrassBlade` 就是 TG 那条 VS 公式的静态烘焙件）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Cover")
	FCSGroundCoverSpecies Grass;

	/**
	 * 花。**一种花 = 一张网格 = 一个 palette = 一个实例组件**（一个组件只绑一张基础网格，
	 * 塞进同一块 buffer 的后半段是画不出来的 —— 石子那条已经把这个结论写死过一次）。
	 * 想加几种就加几行；每一行记得给一个**互不相同的 `Salt`**。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Cover")
	TArray<FCSGroundCoverSpecies> Flowers;

	/**
	 * 地被的全局种子。⚠️ 随机源是 **(散布格坐标, 物种盐, 通道盐, 本种子)** 的身份哈希，
	 * **不是** `InterlockedAdd` 的槽位 —— 槽位由线程组完成顺序决定，而本 pass 每一笔落笔
	 * 都重扫，拿槽位当种子的症状是"画一笔路整片草原地重掷"，且没有任何断言会报红
	 * （石阶 S1 栽过一次，现场在 `CSGroundStairs.usf:CSStairs_CellSeed`）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Cover")
	int32 GroundCoverSeed = 7;

	/**
	 * 地被 WPO 的淡出长度（cm）。2026-09-29 用户："花草随着远离 WPO 降低，直至交界处彻底消失，并 disable WPO，
	 * 只留下法线变化"；追问后定为「只在最后一段减弱」。
	 *
	 * 交界 = 草的近 / 远边界（近处窗口真正画到的距离，窗口被预算收小时跟着收；草没分两档时取 `Grass.LOD.NearDistance`）。
	 * 草与每一种花都在 [交界 − 本值, 交界] 里把 WPO 线性淡到 0（材质做，按相机距离），交界之外由引擎 Nanite 的
	 * 逐实例 WPO 关闭距离整个关掉（组件的 `InstanceWPODisableDistance`），只剩材质里的法线风动。远处组合草本来就没有 WPO。
	 * 淡出区间写进每个地被组件的 custom primitive data（下标 `CoverWPOFadeStartIndex` / `CoverWPOFadeEndIndex`），
	 * `M_TG_Grass` 读它 —— 同一张材质在 Nanite 替身与经典实例路上读到同一个值。0 = 在交界处一刀切。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Cover", meta = (ClampMin = "0.0", UIMax = "10000.0"))
	float CoverWPOFadeLength = 2000.0f;

	/** 地被组件 custom primitive data 的下标：WPO 淡出起点 / 终点（cm，终点 ≤ 0 = 材质不淡出）。与 `M_TG_Grass` 的两个参数对应。 */
	static constexpr int32 CoverWPOFadeStartIndex = 0;
	static constexpr int32 CoverWPOFadeEndIndex = 1;

	// -------------------------------------------------------------------------
	// 建筑周边灌木（第七条派生链，照 TG `_garden_spawn_bushes_buildings.cs`）
	//
	// **归地面**：环带由全部房子的外皮并集决定（两栋房的环会合并、任何一栋的内部都要排除），
	// 单栋房子看不见别人。房子只登记外皮多边形（`SetBuildingFootprint`），地面不调任何房屋逻辑。
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Building Bushes")
	bool bBuildingBushesEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Ground|Building Bushes")
	FCSGroundBuildingBushes BuildingBushes;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** Opens the editor-side vertex-colour paint tool for this actor. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground", meta = (DevelopmentOnly))
	void StartVertexColorPaint();

	/** 从镜像全量重建 GPU 网格（加载、改参、需要对齐时）。镜像尺寸与配置不符时先重置镜像。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void RebuildGroundMesh();

	/**
	 * 塑形物登记（计划 D9）：塑形物在 PostRegisterAllComponents 登记、销毁时注销，两处都会
	 * 触发一次高度重导出。登记幂等 —— 重复调用只是重算一遍，结果相同。
	 */
	void RegisterShaper(ACSGroundShaperActor* Shaper);
	void UnregisterShaper(ACSGroundShaperActor* Shaper);

	/**
	 * 声明式重导出：Mirror.Heights = 基底 0 与全部登记塑形物贡献的 max（计划 D9 "重叠取 max"），
	 * 高度真的变了才重建网格 + 标脏 + 直推广播 —— 幂等，加载期/无效唤醒自然短路。
	 *
	 * v1 是全域重导出（256² 顶点 × 塑形物数，纯 CPU 距离场，微秒级）；将来切区域更新时
	 * 只需把遍历范围换成 union(旧足迹, 新足迹)，公式一行不动。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void RebuildHeightsFromShapers();

	/**
	 * 区域版重导出：只重算 WorldRectXY 覆盖的格点，GPU 侧走一个 compute pass 原地改常驻流的
	 * Z 与法线（`UCSMeshOps::DisplaceGroundShapers`），**不再重建整张快照、不再整网格重传**。
	 *
	 * 调用方负责给出 union(旧足迹, 新足迹) —— 撤掉旧位置的隆起与压出新位置的隆起是同一趟。
	 * 公式是"基底 0 与全部塑形物取 max"的绝对式，所以区域内重算与全量重算结果逐位相同。
	 */
	void RefreshHeightsInRegion(const FBox2D& WorldRectXY, bool bCommitted = true);

	/**
	 * 最近一次 `OnGroundChanged` 广播是不是**提交**（松手 / 收笔 / 显式重建），而不是拖动 / 落笔途中的一帧。
	 *
	 * 房子（D7 接缝）只在提交广播上做跨房的接缝写入：塑形物拖动是每帧重导出 + 广播、房子每帧落座，
	 * 接缝写入若跟着落座走，拖一次塑形物就是每帧写邻居。委托签名不动，订阅者在回调里读这个标记。
	 */
	bool IsLastChangeCommitted() const { return bLastChangeCommitted; }

	/** 塑形物高度场的 GPU 参数（每座 2 个 float4），CPU 镜像与 compute pass 共用同一份构造。 */
	void BuildShaperGpuParams(TArray<FVector4f>& OutParams) const;

	/**
	 * 重扫一遍石阶（计划 S1）：一个 dispatch，CPU 侧只递交"扫哪块矩形、格多大、阈值多少"。
	 *
	 * 幂等且便宜到不值得做哈希短路 —— 整条链就是一次 dispatch，没有"短路点落在昂贵计算之后"
	 * 那种问题（已删的塑形物石阶旧路记在案的缺陷 M4）。`StairMesh` 为空时零成本返回。
	 *
	 * 调用时机的硬约束：它读的是**地面网格 GPU 色流**里的道路权重，而落笔只写镜像 + 排队，
	 * 所以必须排在 `FlushPaintToGpu` 之后。渲染命令 FIFO 保证了顺序，但漏调的症状是
	 * "石阶比路慢一笔"。本类内部已经在每条会改变高度或道路的路径末尾调过它。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Stairs")
	void RebuildStairs();

	/**
	 * **诊断 / 自动化测试专用**：阻塞回读石阶实例（数量 + 世界空间原点）。
	 *
	 * 运行路径一个字节都不回读；这个函数存在的唯一理由是让"接合处不断裂"这条 S1 核心验收项
	 * 可断言 —— 摆位判定全在 GPU 上，除了把结果读回来没有别的办法证明它。别在每帧路径上调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Stairs", meta = (DevelopmentOnly))
	int32 DebugReadStairsSync(TArray<FVector>& OutWorldOrigins);

	/**
	 * **自动化测试专用**：连 packed 行一起回读（5 行 / 实例，**组件空间**）。
	 * S2 的抖动只体现在被缩放过的基上，不看基就断言不了"同一格同一层恒等"。
	 * 不是 UFUNCTION —— 回读是阻塞的，不该出现在任何脚本/蓝图路径上。
	 */
	int32 DebugReadStairRowsSync(TArray<FVector4f>& OutRows);

	/**
	 * **诊断 / 自动化测试专用**：阻塞回读小石子（数量 + 世界空间原点）。
	 *
	 * 石子和石阶一样，摆位判定全在 GPU 上 —— "15% 这一支真的在出东西、而且是按格身份出的"
	 * 除了回读没有别的证明办法。别在每帧路径上调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Stairs", meta = (DevelopmentOnly))
	int32 DebugReadStairPebblesSync(TArray<FVector>& OutWorldOrigins);

	/**
	 * **自动化测试专用**：石子的 packed 行（5 行 / 实例，**组件空间**）。
	 * "石子的随机源是格身份不是槽位"这条纪律只能靠**复算哈希**来证明，而哈希落在被缩放过的基
	 * 与第 4 行的 `.w` 上 —— 只看数量/位置的断言在槽位恰好稳定时会静默通过。
	 */
	int32 DebugReadStairPebbleRowsSync(TArray<FVector4f>& OutRows);

	// -------------------------------------------------------------------------
	// GPU 侧真值（**诊断 / 验收专用，阻塞**）
	//
	// ⚠️ **绝对不许进交互路径**：回读就是阻塞，且每一次都被 `UCSMesh::GetBlockingFlushCount()`
	// 数到 —— 十一条 `flushes=0` 断言会在误用的那一刻报红，那是有意的警戒线。
	//
	// 与上面那几条 `DebugRead*Sync` 的分工（**不是重复**）：那几条读的是**生产者自持**的
	// 那批 buffer，证明"散布 pass 写出了什么"；这几条读的是**组件手上那一份**，也就是剔除
	// pass 与 indirect draw 真正消费的那个计数器。门框砖那个既有 bug 的现场恰恰是两者不一致：
	// 生产者以为清干净了，组件却被重新交接回同一批带着陈旧计数器的 buffer。
	// -------------------------------------------------------------------------

	/** 石阶在 GPU 上的实例计数器（组件手上那一份）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadStairCountGpuSync() const;

	/** 小石子在 GPU 上的实例计数器（组件手上那一份）。−1 = 读不到，不是 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadStairPebbleCountGpuSync() const;

	/**
	 * 岩壳这一帧的 `DrawIndexedIndirect` 会消费多少个索引 —— 即 GPU 上那份间接绘制参数的
	 * `IndexCountPerInstance`，不是 CPU 侧的三角数。
	 *
	 * 岩壳走的是网格路不是实例路（一张 `UCSMesh` 槽位池，一次间接绘制），所以它的"GPU 侧真值"
	 * 就是这个数。⚠️ 注意它**不会跟着壳的死活变**：壳是靠往顶点写 NaN / 退化让三角自己塌掉的
	 * （裁决二那条"没有一行注销代码"），索引数恒等于**池容量 × 5M × 3**。它答的是
	 * "这次绘制到底会不会读到东西"，答不了"还剩几个三角活着" —— 后者请用
	 * `DebugReadRockShellSync` 逐顶点判 NaN。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadRockShellDrawIndexCountGpuSync() const;

	// -------------------------------------------------------------------------
	// Rock Shell（链 B）
	// -------------------------------------------------------------------------

	/**
	 * 重跑一趟披挂岩壳（计划 D9 链 B）。
	 *
	 * ⚠️ **第一句就是哈希比较** —— 短路必须发生在昂贵计算**之前**（已删的塑形物石阶旧路
	 * 恰好把短路点放在 181 行的 `BuildStepPlan` 之后，那是它记在案的缺陷）。
	 * 哈希覆盖"会改变壳形状的一切"：
	 * 塑形物集合与它们的高度场参数、地面几何配置、壳自己的参数、以及**落笔计数**
	 * （道路权重变了必须重披挂 —— 那正是"沉下去不是隐藏"的执行面）。
	 *
	 * 调用时机的硬约束与石阶相同：它读的是**地面网格 GPU 色流**里的道路权重，而落笔只写镜像 +
	 * 排队，所以必须排在 `FlushPaintToGpu` 之后。本类内部已经在每条会改变高度或道路的路径
	 * 末尾调过它。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Rock Shell")
	void RebuildRockShell();

	/**
	 * **诊断 / 验收专用**：岩壳这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * ⚠️ **这个函数存在的唯一理由是今天刚踩过的那个坑**：GPU 石阶的 `StairMesh` /
	 * `StairMaterial` 在两张演示关卡里一直是 NULL，石阶在画面里是一撮黑块，而单测 53/53、
	 * 回归 55 条全绿 —— 因为验收全部走 readback 断言，而 **readback 证明的是"buffer 里有数"，
	 * 对"画的是哪张网格、有没有材质、组件注册没注册"一个字都没说**。
	 *
	 * 所以它检查的是渲染那一侧的每一环：组件存在且已注册且可见、网格已绑定、常驻流已分配、
	 * 三角容量非零、且**解析得到一个非空材质**。任何一环断掉都会写进 `OutReason`。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool IsRockShellDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ **`IsRockShellDrawable(FString&)` 在 UE Python 侧是残废的**（实测）：Python 把
	 * "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串、**不可画时拿到 `None`**，
	 * 原因串整个丢掉，恰好在唯一需要它的时候失效。出图/回归脚本一律调这一版。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	FString GetRockShellUndrawableReason() const;

	/**
	 * **自动化测试专用**：披挂 pass 跑过几次。
	 *
	 * 与 `GetGpuDisplaceCount` 同一个用途：让"壳真的被重算了"这件事可断言，
	 * 而不是断言一个从来没跑过的 pass 留下的初始值。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 GetRockShellDisplaceCount() const { return RockShellDisplaceCount; }

	/**
	 * **自动化测试专用**：阻塞回读岩壳的逐顶点世界位置。
	 *
	 * 运行路径一个字节都不回读；这个函数存在的唯一理由是让链 B 的三条核心验收项可断言
	 * （陡坡上出现 / 平地上不出现 / 画路之后**连续下沉**）。别在任何每帧路径上调它。
	 *
	 * 被 NaN 关掉的三角在返回值里就是 NaN，调用方自己判 `ContainsNaN()`。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 DebugReadRockShellSync(TArray<FVector>& OutWorldPositions);

	/**
	 * **自动化测试 / 诊断专用**：岩壳顶点色 R 通道的盖 / 裙分离统计（回读 GPU 常驻流）。
	 *
	 * 存在的理由与 `IsRockShellDrawable` 同一条：`bIsCapTri` 在 aux 槽 33 里躺着**没人读**，
	 * 而 aux 流既进不了顶点工厂也进不了回读集 —— 材质拿不到它，`SaveToStaticMesh` 也带不走它，
	 * 而这两件事**都不会报错**。盖/裙因此被写进顶点色 R（通道字典见 `CSGroundRockShell.h`），
	 * 这个函数是那条通道的执行面：数出来的两个数必须与图案里的盖/裙三角数逐个对上。
	 *
	 * 返回读到的顶点总数（0 = 回读失败）。⚠️ 阻塞回读，**只给测试与诊断**，不许进交互路径。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 DebugReadRockShellCapSplitSync(int32& OutCapVerts, int32& OutSkirtVerts);

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把岩壳走真正的 `SaveToStaticMesh` 出口烘成资产，再从烘出来的
	 * `FMeshDescription` 里数盖 / 裙角点 —— 裁决六 ②「顶点色通道字典必须随网格保住」的判据。
	 *
	 * 为什么必须真烘一遍而不是只看 GPU 侧那份：中间隔着 `ReadbackResidentSync` →
	 * `BuildGpuMeshDescription` → StaticMesh 构建三道，任何一道把顶点色丢掉都**不报错**，
	 * 症状只是"烘出来的资产在编辑器里是一整片同色"，而那时人已经离开这条链很久了。
	 *
	 * `AssetPath` 传测试自己的临时路径；调用方负责删。返回是否烘成功。
	 * ⚠️ 阻塞 + 建资产，**只给测试**。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool DebugBakeRockShellCapSplitSync(const FString& AssetPath, int32& OutCapCorners,
		int32& OutSkirtCorners, int32& OutTriangles);
#endif

	/**
	 * **自动化测试 / 诊断专用**：稀疏岩壳的槽位池现状。活跃胞腔数 / 池容量（槽）/ 每槽三角数（5M）。
	 * 返回池是否已建。活跃 = 这一趟激活判据选中、且分到了槽的胞腔（拖动期超容量的那部分不算）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	bool GetRockShellPoolStats(int32& OutActiveCells, int32& OutCapacityCells, int32& OutTrianglesPerCell) const;

	/** **自动化测试专用**：格键 → 槽号（没激活返回 −1）。槽 s 的三角是 `[s × 5M, (s+1) × 5M)`。 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	int32 DebugGetRockShellCellSlot(FIntPoint Cell) const;

	/** **自动化测试专用**：当前全部活跃胞腔的格键（j 优先排序）。 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Rock Shell", meta = (DevelopmentOnly))
	void DebugGetRockShellActiveCells(TArray<FIntPoint>& OutCells) const;

	/**
	 * **自动化测试专用**：区域位移 pass（`UCSMeshOps::DisplaceGroundShapers`）跑过几次。
	 *
	 * 存在的唯一理由是让 CPU/GPU 高度场一致性那条断言**不会静默通过**：
	 * `RefreshHeightsInRegion` 在"网格还没建 / actor 被拖过"时会退回 `RebuildGroundMesh()`，
	 * 那条路是拿 CPU 镜像直接上传的 —— 回读到的顶点 Z 与镜像当然相等，断言绿着却什么都没测。
	 * 测试必须先看这个计数涨了，才知道自己比的是 GPU 那一份。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Diagnostics", meta = (DevelopmentOnly))
	int32 GetGpuDisplaceCount() const { return GpuDisplaceCount; }

	// -------------------------------------------------------------------------
	// 塑形物裙边摆件（D12 第五家）
	// -------------------------------------------------------------------------

	/**
	 * 重摆一遍裙边摆件。**幂等**，且第一句就是哈希短路（短路点在任何昂贵计算之前，
	 * 同 `RebuildRockShell` —— 已删的塑形物石阶旧路把短路放在 181 行规划之后，那是它记在案的缺陷）。
	 *
	 * 调用时机的硬约束与石阶 / 岩壳相同：它读的是**镜像**里的高度与道路权重，而落笔只写镜像 +
	 * 排队，所以排在同一条尾巴上就够了。本类内部已经在每条会改变高度或道路的路径末尾调过它。
	 *
	 * 交互期零阻塞：容量在这里按**配置上限**（`CSGroundDecor::MaxRecordsBound` 再过一遍
	 * `CSShaperSteps::ReserveCount`）一次付清，包围盒量化只涨不缩，之后画笔刷 / 拖塑形物
	 * 走的永远是"哈希没变，直接返回"这条零成本分支。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Skirt Decor")
	void RebuildSkirtDecor();

	/** 这一轮生产出来的裙边锚点数。**摆件数 ≤ 锚点数**是 D12「密度由锚点个数决定」的形式化。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor")
	int32 GetSkirtDecorAnchorCount() const { return CurrentSkirtDecorAnchorCount; }

	/** 这一轮真的摆出去的件数（锚点过了填充概率与最小间距球之后剩下的）。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor")
	int32 GetSkirtDecorInstanceCount() const { return CurrentSkirtDecorInstanceCount; }

	/**
	 * **验收专用**：裙边摆件这一帧到底会不会被画出来，不会的话原因是什么。
	 *
	 * 执行面照抄 `ACSHouseActor::GetDecorUndrawableReason` —— 逐环检查渲染那一侧
	 * （组件存在/注册/可见、实例源已交接、基础网格快照非空、GPU 网格已分配、材质非空、
	 * **且母材质勾了 `bUsedWithInstancedStaticMeshes`**）。最后那一条是本项目栽过的三条
	 * "静默换默认材质"里的一条，readback 断言对它一个字都说不了。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	bool IsSkirtDecorDrawable(FString& OutReason) const;

	/**
	 * 同上，但**把原因当返回值给出来**（空串 = 画得出来）。
	 *
	 * ⚠️ 脚本一律调这一版：UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 ——
	 * 可画时拿到空串、**不可画时拿到 `None`**，原因串整个丢掉，恰好在唯一需要它的时候失效。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	FString GetSkirtDecorUndrawableReason() const;

	/**
	 * **诊断 / 验收专用，阻塞**：裙边摆件在 GPU 上的实例计数器（**组件手上那一份**，
	 * 也就是剔除 pass 与 indirect draw 真正消费的那个）之和。
	 *
	 * 与 `GetSkirtDecorInstanceCount()` 的分工同门框砖那条：CPU 的账与 GPU 的账不一致
	 * 正是"生产者以为清干净了、组件却握着陈旧计数器"那类 bug 的现场。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	int32 DebugReadSkirtDecorInstanceCountGpuSync() const;

#if WITH_EDITOR
	/**
	 * **自动化测试专用**：把裙边摆件走真正的 `UCSGpuInstancedMeshComponent::SaveToStaticMesh`
	 * 出口烘成资产，再从烘出来的 `FMeshDescription` 里读回判据 —— 裁决六 ①②③ 的执行面。
	 *
	 * 判据与 `ACSHouseActor::DebugBakeFrameBricksSync` 逐条同形（那条守着门框砖）：
	 * ① 出口走不走得通；② 顶点色通道与 UV 有没有活下来；
	 * ③ **逐实例随机有没有活下来** —— 烘完就没有实例了，`PerInstanceRandom` 恒 0，
	 *    材质那条 `lerp(0.78, 1.22, rnd)` 会把整片摆件烘成同一个色，而三角数 / 包围盒 /
	 *    实例数**全都看不见它**。随机数烘在**顶点色 A**（材质两条路统一读
	 *    `PerInstanceRandom + VertexColor.A`），所以判据就是"A 通道还剩几种取值"。
	 *
	 * ⚠️ **UV 那一半与门框砖不同，且是有意的**：摆件的基础网格走 `CSHouseVine::BuildBaseMesh`
	 * （clutter 与 `ivy_branch` 一样可能**根本没有法线与 UV**，那个读取器负责现补），
	 * 它把 `NumTexCoordChannels` 钉成 1 ⇒ 这条路上**只有一组 UV**，多组 UV 那一半由门框砖
	 * 那条（走 `SetBaseMesh`，原样保留资产的全部通道）守着。所以这里的判据是
	 * "那**一组** UV 活下来了且不是退化的"，`OutDistinctUVs` 就是后半句 ——
	 * 全是 (0,0) 的 UV 不报错，症状只是烘焙件贴图变成一整片同一个像素。
	 *
	 * 只烘第 0 个 palette（多 palette 时其余同构，多烘几张只是多花时间）。⚠️ 阻塞 + 建资产。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Skirt Decor", meta = (DevelopmentOnly))
	bool DebugBakeSkirtDecorSync(const FString& AssetPath, int32& OutTriangles, int32& OutVertexInstances,
		int32& OutUVChannels, int32& OutDistinctUVs, int32& OutDistinctBakedRandoms, int32& OutGpuInstanceCount,
		bool& bOutRandomsMatchGpu);
#endif

	// --- 地面遮罩 ------------------------------------------------------------

	/**
	 * 在 GPU 上重算地面遮罩（`GroundMask`：色流 R → 平滑 + 延伸，见 `GroundMaskExtendDistance`），并把它连同
	 * UV 换算喂给地面材质的动态实例。录完直接返回，不阻塞。
	 *
	 * 幂等：落笔计数、格子、延伸距离都没变就早退。**必须排在地被 / 灌木重散之前**（同一条渲染命令 FIFO）：
	 * 它们读的就是这张图，排反了的症状是"草比路慢一笔"，只在快速涂抹时看得见。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Ground Mask")
	void RebuildGroundMask();

	/**
	 * **诊断 / 验收专用，阻塞**：把地面遮罩整张读回（行主序，一个值对一个地面格点）。`OutValues` 是 R（成品），
	 * `OutInner` 非空时另给 G（只糊的场）。返回格点数，没有遮罩时 0。
	 */
	int32 DebugReadGroundMaskSync(TArray<float>& OutValues, TArray<float>* OutInner = nullptr) const;

	// --- 地被（草 + 花）------------------------------------------------------

	/**
	 * 重新散布地被。**草与所有花在同一张 RDG 图里一起录完**（每个物种一次 clear + 一次
	 * dispatch）—— 色流 SRV、塑形物参数上传、`FCSMeshRenderThreadEdit` 的进出都只需要一份。
	 *
	 * 幂等：输入哈希没变就整趟早退，一次 enqueue 都不发（同岩壳 / 裙边摆件）。哈希里带
	 * `PaintRevision`，所以画笔一落它就必然重扫 —— 这正是"路上不长草"要的时序。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Cover")
	void RebuildGroundCover();

	/**
	 * 房子登记 / 更新自己的外皮（**世界 XY 凸多边形**，逆时针或顺时针都行）。房子每次重求值都会推一遍，
	 * 没变就当场返回；变了才重散灌木。`Key` 只当键用（通常就是房子本身），地面不调它任何东西。
	 */
	void SetBuildingFootprint(const UObject* Key, TConstArrayView<FVector2D> WorldPolygon);
	void RemoveBuildingFootprint(const UObject* Key);

	/** 当前登记的房屋外皮数（诊断用）。 */
	int32 GetBuildingFootprintCount() const { return BuildingFootprints.Num(); }

	/** 重散建筑周边灌木（整块地面一次，幂等：输入哈希没变就一次 enqueue 都不发）。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground|Building Bushes")
	void RebuildBuildingBushes();

	/** 灌木的实例组件：0 = 本体，1 = 花（没有则 nullptr）。 */
	UCSGpuInstancedMeshComponent* GetBuildingBushComponent(int32 Layer) const
	{
		return BushComponents.IsValidIndex(Layer) ? ToRawPtr(BushComponents[Layer]) : nullptr;
	}

	/** **诊断专用，阻塞**：灌木某一层（0 = 本体，1 = 花）的实例世界原点。−1 = 那一层没有组件。 */
	int32 DebugReadBuildingBushOriginsSync(int32 Layer, TArray<FVector>& OutWorldOrigins) const;

	/** 上一趟散布用的组件数（0 = 一个都没建组件；含远处组合草那几条，它们排在物种之后）。诊断用，不回读 GPU。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover")
	int32 GetGroundCoverSpeciesCount() const { return CoverComponents.Num(); }

	/**
	 * 某个物种（0 = 草，1.. = 花）的**远处组合草**在组件表里的下标（喂 `DebugReadGroundCover*` 那一族）；
	 * −1 = 这个物种没分两档。物种自己的下标不变（远处条目一律排在全部物种之后）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover")
	int32 GetGroundCoverFarIndex(int32 SpeciesIndex) const { return CoverFarIndexOfSpecies.IsValidIndex(SpeciesIndex) ? CoverFarIndexOfSpecies[SpeciesIndex] : INDEX_NONE; }

	/**
	 * 钉住近处窗口的中心（世界位置，只用 XY）—— 出图 / 验收脚本用：无头进程没有视口、拿不到相机位置。
	 * 钉住期间不跟相机；`ClearGroundCoverViewOverride` 放开。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Cover")
	void SetGroundCoverViewOverride(FVector WorldLocation);

	UFUNCTION(BlueprintCallable, Category = "CS Ground|Cover")
	void ClearGroundCoverViewOverride();

	/** 近处窗口此刻的中心（世界 XY，Z = 地面 actor 的 Z）。诊断用。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover")
	FVector GetGroundCoverViewCenter() const;

	/** 近处窗口跟着相机搬过几次（每次 = 重撒一趟）。诊断用。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover")
	int32 GetGroundCoverWindowMoveCount() const { return CoverWindowMoveCount; }

	/**
	 * 画得出来吗（**不回读 GPU**，只查资产/组件/材质那几条会让画面一片灰或空白的前置条件）。
	 * 与岩壳 / 裙边摆件同形：把"静默换材质"这类不报错的失败做成显式判据。
	 */
	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover", meta = (DevelopmentOnly))
	bool IsGroundCoverDrawable(FString& OutReason) const;

	UFUNCTION(BlueprintPure, Category = "CS Ground|Cover", meta = (DevelopmentOnly))
	FString GetGroundCoverUndrawableReason() const;

	/**
	 * **诊断 / 验收专用，阻塞**：某个物种在 GPU 上的实例计数（0 = 草，1.. = 花的下标 + 1）。
	 * −1 = 那个物种没有组件，与"真的是 0 株"分开 —— 把读不到当 0 会让守着"关掉之后必须归零"
	 * 的断言在管线坏掉时假绿（同 `DebugReadStairCountGpuSync` 的口径）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Cover", meta = (DevelopmentOnly))
	int32 DebugReadGroundCoverCountGpuSync(int32 SpeciesIndex) const;

	/** **诊断专用，阻塞**：某个物种的实例世界原点（已按 GPU counter 截断）。 */
	int32 DebugReadGroundCoverOriginsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins) const;

	/**
	 * **诊断专用，阻塞**：实例世界原点 + 高度缩放（kernel 的 `HeightScale`，已含遮罩压矮）。
	 * "离路越近越矮"只能量、不好看 —— 草叶本身高矮不齐，肉眼分不出压矮带有多宽（`Ground.CoverRoadExtend`）。
	 */
	int32 DebugReadGroundCoverHeightsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins, TArray<float>& OutHeightScale) const;

	/**
	 * **诊断 / 验收专用，阻塞**：实例的世界原点 + **水平朝向**（单位向量）。
	 *
	 * 朝向从 packed 行的第 2 行（`LocalY`，= 株的朝向轴 = 基础网格的弯曲方向）取，去掉缩放
	 * 与 Z 分量再单位化。存在的理由是"朝向成簇"这条**只能量、不能看**：肉眼分不清
	 * "逐叶随机"和"簇权重 0.15"，而两者的画面差别恰恰是这一轮要复刻的东西。
	 * 判据配方见 `Scripts/TinyGladeVerifyCoverOrientation.py`。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Cover", meta = (DevelopmentOnly))
	int32 DebugReadGroundCoverFacingsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins,
		TArray<FVector2D>& OutFacingXY) const;

	/**
	 * **诊断 / 验收专用，阻塞**：逐实例 custom data 的两个通道 + 该实例的世界原点。
	 *
	 * `OutBendAmp` = custom data[0]（弯曲幅度）、`OutReserved` = custom data[1]（保留，应恒 0）、
	 * `OutWorldOrigins` = packed 行第 4 行的原点。
	 *
	 * 带出原点是为了让"custom data 与 packed 行**逐实例配对**"这条**可断言**：弯曲幅度的公式
	 * 末尾有 `× (1 − 遮罩)`，所以每一株都必须满足
	 * `BendAmp ≤ BendRange.Max × (1 − 遮罩(该株原点))`。下标错位的症状是数值张冠李戴、
	 * 剔除一变就换一批 —— 光看数值范围是绝对看不出来的，必须有一条把两块缓冲绑在一起的判据。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground|Cover", meta = (DevelopmentOnly))
	int32 DebugReadGroundCoverCustomDataSync(int32 SpeciesIndex, TArray<float>& OutBendAmp,
		TArray<float>& OutReserved, TArray<FVector>& OutWorldOrigins) const;

	/** 顶点色全部铺回 BaseColor（高度不动），并重建。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Ground")
	void ResetPaint();

	/**
	 * 一次落笔：GPU 球刷 pass + 镜像 CPU 孪生双写，公式严格同 ECSMeshPaintBlendOp 契约。
	 * EdMode 在 stroke 期间每次鼠标移动调一次；Python/测试也可直接调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void ApplyPaintStroke(FVector WorldCenter);

	/** Stroke 括号：Begin 清累计脏盒，End 标脏包并补一次**提交**广播（逐笔落笔的广播是未提交的，见 IsLastChangeCommitted）。 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void BeginPaintStroke();

	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void EndPaintStroke();

	/**
	 * 把本帧攒下的笔刷落笔一次性推给 GPU（**一次异步编辑，零 flush**）。EdMode 每帧调一次。
	 *
	 * 落笔本身只写镜像（本就是权威）并把这一笔排进队列 —— 交互热路径上一次设备同步都没有。
	 * 副作用只有"GPU 顶点色比镜像滞后 ≤1 帧"，纯视觉：所有 gameplay 查询都走镜像。
	 *
	 * bBlockIfNeeded 只给收笔用：异步在途时退回一次同步提交，保证抬笔后 GPU 与镜像一致
	 * （否则 EdMode 退出后队列没人再推，画面会一直停在上一帧）。一次 stroke 至多一次。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Ground")
	void FlushPaintToGpu(bool bBlockIfNeeded = false);

	/** 镜像双线性采样：世界 XY → 世界 Z。超出范围返回 actor Z。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	float SampleHeight(FVector2D WorldXY) const;

	/** 同 `SampleHeight`，但**超出范围 / 镜像未就绪时返回 false**，不退回 actor Z。
	 *  "脚下到底有没有地面"要靠它答 —— `SampleHeight` 在范围外给的是一块假想的无限平地。 */
	bool TrySampleHeight(FVector2D WorldXY, float& OutWorldZ) const;

	/** 镜像双线性采样：世界 XY → 道路权重（R 通道，0..1）。超出范围返回 0。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	float SampleRoadWeight(FVector2D WorldXY) const;

	/** 镜像双线性采样：完整顶点色。 */
	UFUNCTION(BlueprintPure, Category = "CS Ground")
	FLinearColor SampleColor(FVector2D WorldXY) const;

	/** 射线 ∩ 地面（解析，不走碰撞）。平地走平面求交，有起伏时定步进 march。 */
	bool RaycastGround(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHit) const;

	/** 地面的世界 XY 矩形（笔刷范围提示用）。 */
	FBox2D GetWorldRect2D() const;

	//~ AActor interface（EndPlay / Destroyed 在基类：只调下面的 ReleaseInstancedBuffers 放生产者那一份，
	//  组件那一份由组件销毁时自己放）
	virtual void PostRegisterAllComponents() override;
	/** 旧岩壳属性的迁移（`RockShellPatternScale` → `RockShellCellSize`），见那个私有属性的注释。 */
	virtual void PostLoad() override;
	/** 地被近处窗口跟相机（没有开两档的物种时什么都不做）。 */
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
#endif

protected:
	//~ ACSTinyGlade interface
	/**
	 * 石阶 / 石子 / 每个裙边摆件 palette / 每个地被物种。
	 *
	 * ⚠️ 石阶那两族进诊断的理由就是坑表里那两条：`StairMesh` / `StairMaterial` 一直是 NULL 时
	 * 画面上是一撮黑块而 readback 全绿；母材质没勾 `bUsedWithInstancedStaticMeshes` 时引擎
	 * **静默换成默认材质**，症状与"没绑材质"逐像素相同。`IsRockShellDrawable` 那一族只查"非空"。
	 */
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const override;
	virtual void ReleaseInstancedBuffers() override;

private:
	/** 扫场景收集全部塑形物（加载顺序无保证，塑形物自己也会登记，两条路都幂等）。 */
	void ResolveShapers();

	/** 镜像尺寸与配置不符时重置为平地 + BaseColor。返回是否发生了重置。 */
	bool EnsureMirrorInitialized();

	/** 镜像 → 世界空间快照（位置/法线/UV/颜色/索引），交给 CopyFromMeshSnapshot 上传。 */
	void BuildSnapshotFromMirror(struct FCSGpuMeshCPUData& OutSnapshot) const;

	/** GPU 球刷的 CPU 孪生：对镜像里笔刷矩形内的顶点跑同一套权重/混合公式。 */
	void ApplyPaintToMirror(const FVector& WorldCenter);

	/**
	 * 笔刷球在镜像格点上的闭区间矩形。CPU 孪生的遍历范围与 GPU 区域派发的范围**共用这一个**
	 * 口径——两边各算一份就会在边界上分叉（镜像有色而画面没有，或反过来）。
	 * 返回 false 表示笔刷完全落在镜像外，这一笔什么都不用做。
	 */
	bool ComputeBrushGridRect(const FVector& WorldCenter, FIntPoint& OutMin, FIntPoint& OutMax) const;

	/** 镜像格点的世界坐标（含高度）。 */
	FVector VertexWorldPosition(int32 X, int32 Y) const;

	/** 世界 XY → 连续格坐标；返回是否落在镜像矩形内（出界时也写出 clamp 后的值）。 */
	bool WorldToGrid(const FVector2D& WorldXY, FVector2D& OutGrid) const;

	/** 双线性采样的公共实现。 */
	bool SampleBilinear(const FVector2D& WorldXY, float& OutHeight, FLinearColor& OutColor) const;

	/** 整块地面的世界 AABB（XY 矩形 × 高度范围），全量类广播用。 */
	FBox ComputeGroundWorldBox() const;

	/**
	 * 保证石阶实例组件存在并绑好基础网格/材质，同时刷新从网格推导出来的三个量
	 * （包围球、块缩放、抬升）。`StairMesh` 为空时销毁组件并释放缓冲区。返回是否可以扫。
	 */
	bool EnsureStairComponent();

	/**
	 * 保证岩壳的槽位池与渲染组件存在、包围盒已按地面矩形写死。`NeededCells` = 本趟激活的胞腔数。
	 * 返回是否可以派发；`bOutRebuilt` = 池刚重建过（全部槽清空，调用方要把活跃胞腔全部当新胞腔重分配）。
	 *
	 * ⚠️ **重建 / 扩容是阻塞的**（声明流集 / 分配 / 上传各 flush 一次），所以它自己带一层短路：
	 * 图案几何、地面矩形都没变且容量够用就直接返回，一次 enqueue 都不发。容量不够时 ×1.5 扩，
	 * 但**拖动期（`bLastChangeCommitted == false`）不扩**：超出的胞腔这一趟暂缺、置 `bRockShellGrowPending`，
	 * 松手提交那一趟补齐 —— 守住「交互期一次阻塞都不发」（与 `CSGroundStairs::EnsureBuffers` 同一条纪律）。
	 */
	bool EnsureRockShellMesh(int32 NeededCells, bool& bOutRebuilt);

	/**
	 * 岩壳的输入哈希：塑形物集合与它们的高度场参数、地面几何配置、壳自己的参数、落笔计数。
	 * `RebuildRockShell()` 的第一句就用它短路，短路点在任何昂贵计算之前。
	 */
	uint32 RockShellInputHash() const;

	/**
	 * 保证裙边摆件的实例组件、基础网格快照、GPU 缓冲与容量都就位。返回是否可以摆。
	 *
	 * 稳态下**零阻塞**：容量按配置上限一次付清（只涨不缩），包围盒量化只涨不缩，
	 * 两者都没变时直接返回，一次 enqueue 都不发 —— 同 `EnsureRockShellMesh` / `EnsureStairComponent`。
	 * bOutHandedOver = 这一趟真把实例源交给了组件（扩容后是清零的新 buffer），
	 * `RebuildSkirtDecor` 的哈希早退门看到它就不许早退（2026-09-07 审查 B1）。
	 */
	bool EnsureSkirtDecorComponents(bool& bOutHandedOver);

	/** 把登记在案的塑形物读成裙边生产者要的环表（**高度场参数与 GPU 位移 pass 同一份来源**）。 */
	void BuildSkirtDecorSite(CSGroundDecor::FSite& OutSite) const;

	/** 参数打包：细节面板只暴露改观感的那几个，其余以 `CSHouseDecor::FParams` 的默认值为准。 */
	CSHouseDecor::FParams MakeSkirtDecorParams() const;

	/** 地被组件表里一条的身份：整张地面一档 / 近处窗口 / 远处组合草。 */
	enum class ECoverRole : uint8 { Plain, Near, Far };

	/** 近处窗口的几何（整张网格 + 窗口大小；窗口位置另算，见 `CoverNearWindowOffset`）。 */
	struct FCoverNearWindow
	{
		FVector2f Origin = FVector2f::ZeroVector;   // 整张网格 (0,0) 格的角（世界 XY）
		float CellSize = 1.0f;
		FIntPoint GlobalDims = FIntPoint(0, 0);
		FIntPoint Dims = FIntPoint(0, 0);           // 窗口格数（恒定 ⇒ 容量恒定 ⇒ 窗口挪动不交接）
		int32 HalfCells = 0;
		/** 近处真正画到多远（cm）：`NearDistance`，窗口被预算收小时相应收小。 */
		float Boundary = 0.0f;
		bool bShrunk = false;
	};

	/** 组件表里的一条：物种参数（远处条目是现组的）+ 身份 + 距离带。 */
	struct FCoverEntry
	{
		FCSGroundCoverSpecies Species;
		ECoverRole Role = ECoverRole::Plain;
		/** 近 / 远两档共用的距离带边界与过渡带宽（cm），写到组件的剔除距离上。 */
		float Boundary = 0.0f;
		float Fade = 0.0f;
		/** 近处条目的窗口几何。 */
		FCoverNearWindow Window;
	};

	/**
	 * 把 `Grass` + `Flowers` 收成一张按下标对齐的组件表：下标 0 恒为草，1.. 为网格非空的花（空网格的整条跳过，
	 * 所以表长 ≠ `Flowers.Num() + 1`）；开了两档的物种再各追加一条远处组合草，**排在全部物种之后**
	 * （物种自己的下标因此不变，脚本里 0 = 草照旧成立）。`OutFarIndexOfSpecies` 给出物种 → 远处条目的下标。
	 */
	void CollectCoverEntries(TArray<FCoverEntry>& OutEntries, TArray<int32>* OutFarIndexOfSpecies = nullptr) const;

	/** 近处窗口的几何（只由参数与地面尺寸决定，与相机在哪无关）。 */
	bool ComputeCoverNearWindow(const FCSGroundCoverSpecies& Species, FCoverNearWindow& Out) const;

	/** 近处窗口此刻的左下角格号（跟着 `ResolveCoverViewXY`，夹在地面内）。 */
	FIntPoint CoverNearWindowOffset(const FCoverNearWindow& Window) const;

	/** 近处窗口的中心：钉住的位置 > 相机上一次落定的位置 > 地面中心。 */
	FVector2D ResolveCoverViewXY() const;

	/**
	 * 相机走出重撒距离就把近处窗口搬过去（Tick 调）。
	 *
	 * 相机位置来自 `UWorld::ViewLocationsRenderedLastFrame`（只有透视编辑器视口与游戏视口往里填）。
	 * ⚠️ 取**离当前窗口最近的那一项**：多视口时表里有好几项且次序随绘制次序变，取 `[0]` 会让窗口
	 * 在两个机位之间来回跳 —— 每跳一次整窗重撒一趟，逐帧卡死且没有任何报错。
	 */
	void UpdateGroundCoverView();

	/**
	 * 保证地被的实例组件、GPU 缓冲与容量都就位。返回是否可以散布。
	 *
	 * 稳态下**零阻塞**：容量按"格数上限"一次付清（只涨不缩），组件与物种表按下标对齐，
	 * 两者都没变时直接返回 —— 同 `EnsureRockShellMesh` / `EnsureStairComponent`。
	 */
	bool EnsureCoverComponents(const TArray<FCoverEntry>& Entries);

	/**
	 * 地被的输入哈希：塑形物集合与高度场参数、地面几何配置、**落笔计数**、每个物种的配置。
	 * `RebuildGroundCover()` 的第一句就用它短路，短路点在任何昂贵计算之前。
	 *
	 * ⚠️ **必须带上 `PaintRevision`**：遮罩就是顶点色，画一笔路就必须重散一次。而顶点色是
	 * 257² 个字节，逐笔哈希整张表太贵；一个单调计数器给出同样的"变了没有"判定，代价是常数
	 * （与岩壳那条逐字同理）。
	 */
	uint32 CoverInputHash(const TArray<FCoverEntry>& Entries) const;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> StairComponent;

	/** 小石子的实例组件。石子与石阶是两张基础网格，一个组件只绑一张 ⇒ 必须各自一个。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> StairPebbleComponent;

	/** 岩壳的 GPU 投影与它的渲染组件（都是 transient：常驻数据不随关卡存盘）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> RockShellMesh;

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> RockShellComponent;

	/** 物理碰撞（gpumesh 本身不带）。编辑器拖放 / 贴地要靠它，见组件类注释。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Ground")
	TObjectPtr<UCSGroundCollisionComponent> GroundCollision;

	/** 按镜像重烘碰撞。只在高度**已提交**地变了之后调（全量重建、塑形物松手 / 改参）。 */
	void RefreshGroundCollision();

	/** `RockShellMaterial` 的动态子实例（见该属性的注释）。父材质换了才重建，缩放变了只改标量。 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> RockShellMaterialInstance;

	/**
	 * 地面网格实际画的材质：`GroundMaterial` 的动态子实例，多带地面遮罩贴图与它的 UV 换算（同岩壳的做法：
	 * 组件画 MID，网格槽放资产本身 —— 烘焙抓的是槽，transient 的 MID 不能进资产）。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> GroundMaterialInstance;

	/** 地面遮罩贴图的尺寸对上当前镜像（格点数）；不对就换一张新的（换对象而不是改尺寸：材质那头靠指针变了才重取资源）。 */
	bool EnsureGroundMaskTexture();
	/** 建 / 更新地面材质的动态实例（遮罩贴图、UV 换算、开关），绑到地面网格组件上。 */
	void BindGroundMaterial();

	/** 槽位池的身份：图案几何哈希 + 地面矩形（包围盒写死用）+ 容量（槽）。三者任一变了才走阻塞的建池路径。 */
	uint32 RockShellPoolGeometryHash = 0;
	FBox2D RockShellPoolRect = FBox2D(ForceInit);
	int32 RockShellPoolCapacity = 0;
	int32 RockShellPoolTrianglesPerCell = 0;   // 建池时的 5M（属性改了还没重建时，诊断读的是真在 GPU 上的那一份）

	/**
	 * 槽位簿记（运行时状态，不序列化 —— 加载后第一趟重建会从塑形物重新导出）：
	 * 格键 → 槽；槽 → 格键（空槽 = `INDEX_NONE` 哨兵）；空闲槽按**降序**存、从尾部取 ⇒ 总是先用小号，
	 * 同一组输入分到的槽号逐位相同（便于逐槽对照）。
	 */
	TMap<FIntPoint, int32> RockShellSlotOfCell;
	TArray<FIntPoint> RockShellCellOfSlot;
	TArray<int32> RockShellFreeSlots;

	/** 胞腔外接半径缓存（多边形是静态的）；图案几何哈希一变整张清空。 */
	TMap<FIntPoint, float> RockShellRadiusCache;
	uint32 RockShellRadiusCacheHash = 0;

	/** 拖动期容量不够时置位：下一次重建必须扩容补齐（哈希没变也不许短路）。 */
	bool bRockShellGrowPending = false;

	/**
	 * 旧属性（TG 原件那一张 tile 的缩放，2026-09-30 随程序化图案退役）。**只为加载老关卡留着**：
	 * 存盘里有值（≠ −1）时 `PostLoad` 换算成 `RockShellCellSize = 本值 × 553.13` 再置回 −1。
	 * 用原名留作私有 `UPROPERTY()` 而不是 `_DEPRECATED` + redirect：后者实测读不回老值（`LiftHeight` 那次踩过）。
	 */
	UPROPERTY()
	float RockShellPatternScale = -1.0f;

	/** 上次披挂时的输入哈希（0 = 还没披挂过）。 */
	uint32 RockShellBuiltHash = 0;

	/** 披挂 pass 跑过的次数（诊断用，见 GetRockShellDisplaceCount）。 */
	int32 RockShellDisplaceCount = 0;

	/**
	 * 落笔计数：每次改动镜像顶点色就 +1。
	 *
	 * **为什么岩壳的哈希必须带上它**：壳在路上是"连续下沉"而不是隐藏（裁决五），下沉量由
	 * 道路权重驱动 ⇒ 画一笔路就必须重披挂一次。而顶点色是 257² 个字节，逐笔哈希整张表太贵；
	 * 一个单调计数器给出同样的"变了没有"判定，代价是常数。
	 */
	uint32 PaintRevision = 0;

	/** 固定容量的实例行与计数（渲染线程分配、渲染线程释放）。 */
	CSGroundStairs::FStairBuffers StairBuffers;

	/**
	 * 从 `StairMesh` + `StairBlockSize` 推导出来的量，`EnsureStairComponent` 刷新。
	 * `StairRise` = −局部包围盒 Min.Z × Z 缩放：把块抬起半个身位，否则盒心落在等值线上、
	 * 石块一半埋在地里（旧路 `PaletteRise` 记下来的实测修正，这里逐字沿用）。
	 */
	FVector3f StairBaseSphereCentre = FVector3f::ZeroVector;
	float StairBaseSphereRadius = 0.0f;
	FVector3f StairBlockScale = FVector3f(1.0f, 1.0f, 1.0f);
	float StairRise = 0.0f;
	/** 基础网格的局部 Y 尺寸：kernel 要拿它把"想要的世界长度"（跟着弦长走）换算回缩放。 */
	float StairBaseSizeY = 1.0f;

	/** 从 `StairPebbleMesh` + `StairPebbleSize` 推导：包围球（剔除用）与均匀缩放的上下限。
	 *  `EnsureStairComponent` 与石阶那三个量一起刷新。网格为空时缩放为 0（= 这一支关掉）。 */
	FVector3f StairPebbleSphereCentre = FVector3f::ZeroVector;
	float StairPebbleSphereRadius = 0.0f;
	float StairPebbleScaleMin = 0.0f;
	float StairPebbleScaleMax = 0.0f;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。
	 *  容量是固定的、包围盒按地面矩形 × MaxAbsHeight 写死，所以稳态下这里永远不触发。 */
	CSShaperSteps::FHandoverCache StairHandover;
	CSShaperSteps::FHandoverCache PebbleHandover;

	/**
	 * 权威数据，随关卡序列化。别在 details 里展开它 —— 就是两条百万级数组。
	 *
	 * **NonTransactional 是正确性必需，不是优化**：镜像若进事务缓冲，"改个属性 → 画路 →
	 * Ctrl+Z 撤那次属性改动"会把镜像整份回滚成"无路"，而屏幕上路还在（GPU 顶点色不在事务里）
	 * ⇒ SampleRoadWeight 说没路、房子下次重求值时拱全关，即"镜像/GPU 双写漂移"的事务成因。
	 * 与"笔刷家族无 Undo"的既定裁决同向；附带省下 257² 每次事务捕获的约 528 KB。
	 * 撤销后的对齐由 PostEditUndo() 负责。
	 */
	UPROPERTY(NonTransactional)
	FCSGroundMirror Mirror;

	/** 上次把镜像烘进常驻流时的 actor 位置；PostEditMove 的增量平移相对它算。 */
	FVector MeshBuiltAtLocation = FVector::ZeroVector;

	/** 高度绝对值上界，决定 RaycastGround 走平面还是 march。由 EnsureMirrorInitialized 从镜像刷新。 */
	float MaxAbsHeight = 0.0f;

	/** 区域位移 pass 跑过的次数（诊断用，见 GetGpuDisplaceCount）。 */
	int32 GpuDisplaceCount = 0;

	/** 登记在本地面上的塑形物（transient：加载后由各自 PostRegisterAllComponents 重新登记）。 */
	TArray<TWeakObjectPtr<ACSGroundShaperActor>> Shapers;

	// --- 裙边摆件（D12 第五家）：与房子那四家同构，一个 palette = 一张网格 = 一个组件 ---

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> SkirtDecorComponents;

	TArray<CSShaperSteps::FPaletteBuffers> SkirtDecorGpuBuffers;

	/** 按 `CSHouseDecor::EFamily` 下标。**只有 `Skirt` 那一格非空** —— 其余四家的载体是房子，
	 *  地面对它们一无所知，留 `{0, 0}` 就是"这一家在这里一件都不长"。 */
	TArray<CSHouseDecor::FPaletteRange> SkirtDecorPaletteRanges;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache SkirtDecorHandover;

	/** 上次那一轮的输入哈希（0 = 还没摆过）。`RebuildSkirtDecor()` 的第一句就用它短路。 */
	uint32 SkirtDecorHash = 0;
	int32 CurrentSkirtDecorAnchorCount = 0;
	int32 CurrentSkirtDecorInstanceCount = 0;

	/** 基础网格快照建好了没有；组件数或网格资产一变就必须重建（palette 与组件**按下标**对齐）。 */
	bool bSkirtDecorBaseMeshReady = false;

	/** 快照是从哪几张网格建的。⚠️ **不能只靠上面那个 bool**：在细节面板里换掉网格时组件不一定
	 *  被重建，那时 bool 仍是 true，画面上还是旧网格 —— 症状是"换了资产但什么都没发生"。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> SkirtDecorMeshesBuiltFrom;

	// --- 地被（第六条派生链）：一个物种 = 一张网格 = 一个组件 = 一套实例源 ---

	/** 下标 0 恒为草，1.. 为**网格非空**的花（顺序同 `Flowers`，空网格的那几行被跳过）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> CoverComponents;

	TArray<CSGroundCover::FCoverBuffers> CoverBuffers;

	/** 组件是从哪几张网格建的。⚠️ **不能只靠"组件数对不对"**：在细节面板里换掉网格时组件数
	 *  不变，只看数量就会得到"换了资产但什么都没发生"（裙边摆件那条踩过）。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> CoverMeshesBuiltFrom;

	/** 每个物种基础网格的局部包围球（未缩放），剔除球从它按最大轴缩放放大。 */
	TArray<FVector3f> CoverBaseSphereCentres;
	TArray<float> CoverBaseSphereRadii;

	/** 上次交给组件的容量/包围盒：只有它们真变了才需要再走一次阻塞的 SetInstanceSourceGPU。 */
	CSShaperSteps::FHandoverCache CoverHandover;

	/** 上次那一趟的输入哈希（0 = 还没散过）。`RebuildGroundCover()` 的第一句就用它短路。 */
	uint32 CoverBuiltHash = 0;

	/** 地面遮罩上次重算时的输入哈希（0 = 还没算过；全量重建换了常驻流时清零）。 */
	uint32 GroundMaskBuiltHash = 0;

	/** 物种下标 → 它的远处组合草在组件表里的下标（−1 = 没分两档）。上一趟 `RebuildGroundCover` 定的。 */
	TArray<int32> CoverFarIndexOfSpecies;

	/** 近处窗口跟着的相机位置（世界 XY）：相机走出重撒距离才更新。 */
	FVector2D CoverViewXY = FVector2D::ZeroVector;
	bool bCoverViewValid = false;
	/** 脚本钉住的窗口中心（无头进程没有相机）。 */
	TOptional<FVector2D> CoverViewOverride;
	/** 同一次钉住给的完整位置（含 Z）：Nanite 路的距离带按它判（`UCSGpuInstancedMeshComponent::SetDistanceCullViewOverride`）。 */
	FVector CoverViewOverrideLocation = FVector::ZeroVector;
	int32 CoverWindowMoveCount = 0;
	/** "近处窗口被预算收小"那条日志只在窗口尺寸变了时打一次（窗口每挪一次都打会刷屏）。 */
	FIntPoint CoverLastLoggedNearDims = FIntPoint(0, 0);

	// --- 建筑周边灌木（第七条派生链）：[0] = 本体，[1] = 花（与本体同一个变换）---

	/** 房屋外皮登记表（世界 XY）。Transient：房子加载 / 重求值时会重推，地面晚到时由 `OnAnyGroundRebuilt` 叫醒房子再推。 */
	TMap<TObjectKey<UObject>, TArray<FVector2D>> BuildingFootprints;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>> BushComponents;

	TArray<CSGroundCover::FCoverBuffers> BushBuffers;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMesh>> BushMeshesBuiltFrom;

	TArray<FVector3f> BushBaseSphereCentres;
	TArray<float> BushBaseSphereRadii;
	CSShaperSteps::FHandoverCache BushHandover;
	uint32 BushBuiltHash = 0;

	/** 灌木要画的层（网格、材质、叠加签按层对齐）。关掉 / 没网格时为空。 */
	void CollectBushLayers(TArray<UStaticMesh*>& OutMeshes, TArray<UMaterialInterface*>& OutMaterials, TArray<float>& OutOverlayChance) const;
	bool EnsureBushComponents(const TArray<UStaticMesh*>& Meshes, const TArray<UMaterialInterface*>& Materials);
	uint32 BushInputHash(const TArray<UStaticMesh*>& Meshes, const TArray<UMaterialInterface*>& Materials) const;

	/** 本次 stroke 的累计世界脏盒：EndPaintStroke 判断要不要标脏包；也是将来切 dirty 系统时的区域发布素材。 */
	FBox StrokeDirtyBounds = FBox(ForceInit);
	bool bPaintStrokeOpen = false;

	/** 见 `IsLastChangeCommitted`。每个 `OnGroundChanged.Broadcast` 站点广播前先写它。 */
	bool bLastChangeCommitted = true;

	/**
	 * 还没推给 GPU 的落笔队列（按落笔顺序）。混合公式对同一顶点是可结合地按序作用的，
	 * 所以"一帧内攒 N 笔、一张图里按序录 N 个 pass"与"逐笔各录一次"逐位等价。
	 * 异步在途被拒时队列原样留到下一帧，不丢也不乱序。
	 */
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> PendingPaintDabs;
};

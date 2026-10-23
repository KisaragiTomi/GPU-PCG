// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Curves/CurveLinearColor.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "MeshGeneratorBrushCache.h"
#include "ComputeShaderDebugParams.h"
// PendingSurfaceVoxelInputs 是按值持有的成员，必须是完整类型，不能只前向声明。
#include "CSSurfaceVoxelPasses.h"
#include "InstancedFoliageActor.h"
#include "FoliageType.h"

#include "GeometryEditorActor.generated.h"

class AStaticMeshActor;
class UCSMesh;
class UCSMeshRenderComponent;
struct FCSVineTriangleSurfaceInputs;

USTRUCT(BlueprintType, meta = (DisplayName = "SC Options"))
struct COMPUTESHADERGENERATOR_API FSpaceColonizationOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Iteration = 55;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 Activetime = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 ExtentPlus = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float RandGrow = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float Seed = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float BackGrowRange = .8f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VoxelSize = 2.5f;

	// SpaceColonization-specific parameters
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	int32 BackGrowCount = 3;
	// Fork Tapering starts shrinking the branch once it backtracks past the Nth non-primary
	// fork (1 = the first fork, 2 = the second, ...). Forks before the Nth are passed through
	// at full scale; from the Nth fork onward the retained ancestor points (BackGrowCount of
	// them) taper continuously down toward the end scale and may cross further forks without
	// resetting or stopping the taper.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 ForkTaperForkOrdinal = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	float InfluenceRadius = 200.0f;

};

USTRUCT(BlueprintType, meta = (DisplayName = "VV Options"))
struct COMPUTESHADERGENERATOR_API FVV
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseScale = 2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CurlNoiseFre = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseScale = 11;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float PerlinNoiseFre = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float ResampleLength = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float LineScale = 0.1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float CircleScale = 0.2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float MergeDistMult = 3;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	float VinesOffset = 5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float UVScaleInfluence = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.001", ClampMax = "1.0"))
	float UVScaleFloor = 0.08f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float UVScalePower = 1.25f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|UV", meta = (ClampMin = "0.000001"))
	float UVLengthScale = 1.0f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	UCurveLinearColor* CurveControl = nullptr;

	/**
	 * 表面吸附总开关（TinyGladeHouse D13，2026-09-06 裁决 3）。
	 *
	 * 真 = 原行为：把 SC 解出的中心线**投射吸附**到场景表面的体素场上，再沿表面法线外推
	 * `VinesOffset` 浮起来。这是"藤爬在任意 mesh 表面上"那条路的立身之本，柱子 / 地形 /
	 * 岩石上的藤只能靠它。
	 *
	 * 假 = 中心线**原样使用**。墙面藤走这一档：折线本来就长在墙面参数坐标里，映射到世界时
	 * 已经带了 `StandOff`，没有可吸附的必要。关掉之后整条链连 CPU 准备一起跳过 ——
	 * `PrepareSurfaceVoxelPassInputs` / `AddCSSurfaceVoxelPasses` / `BuildVoxelHash` /
	 * `FinalProject` / `ResampleSurface` / `SmoothPath`。实测基线里 `GenerateVineGPU.Total`
	 * 的 12.4–14.6 ms 有 **10–12 ms 是 `PrepareSurfaceVoxelInputs`**（由 `VoxelSize` 与包围盒
	 * 决定、与 target 数无关），所以这一档不是微优化。
	 *
	 * ⚠️ **`BuildAxes` 不在被跳过之列，理由很不直观**：Pass C 的环心读的是
	 * `PathPointSurfaceTargets` 而**不是** `PathPoints`，而那条流的唯一播种者就是
	 * `BuildVVVoxelAxesCS`。把它当成"体素那一族"一起跳掉的后果是环心全塌到世界原点、
	 * 整棵藤缩成一个结 —— 不崩、不报错、没有断言。它改为走
	 * `bHasVoxelSample = false` 那一支（`BuildRawVoxelVineFrame` 本来就带对了回退）。
	 *
	 * ⚠️ 为假时下列参数**全部失效**：`FSpaceColonizationOptions::VoxelSize`、本结构的
	 * `VinesOffset` / `GenerateVineVoxelNormalBlurIterations` /
	 * `VisVineGPUPostProjection*` / `bVisVineGPUResampleSurfaceEnabled`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bSurfaceAdsorption = true;

	/** 直接使用 Nanite 误差截面 / 普通网格三角形吸附；关闭可对比旧体素路径。当前抽取准备为同步。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|Surface")
	bool bUseNaniteTriangleSurface = true;

	/** 世界空间 cm，越大面数越少。0 使用当前驻留最细层；不依赖玩家视角。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|Surface", meta = (ClampMin = "0"))
	float NaniteSurfaceCutError = 1.0f;

	/** 表面新增 GPU 缓冲预算（MiB），Nanite / 普通网格 / 地形共用，含抽取临时数据及 BVH。
	 * 按实际缓冲布局换算三角形容量；超限拒绝，不截断表面。不含源资产、Nanite 流送池及藤蔓输出。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|Surface", meta = (ClampMin = "1", ClampMax = "128"))
	int32 SurfaceMemoryBudgetMiB = 128;

	/** 仅 GenerateVineGPUWithGlobalDistanceField 使用；原三角形 / 体素入口不读取这些参数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|GlobalDistanceField", meta = (ClampMin = "1"))
	float GDFMaxProjectionDistance = 200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|GlobalDistanceField", meta = (ClampMin = "0.01"))
	float GDFProjectionTolerance = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Options|GlobalDistanceField", meta = (ClampMin = "1", ClampMax = "32"))
	int32 GDFProjectionIterations = 8;

	// --- moved from FVisVineParameters ---
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 GenerateVineVoxelNormalBlurIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmoothIterations = 3;

	// Half-width (in path points) of each post-projection smoothing pass. Larger values round
	// sharp folds harder per iteration; 1 reproduces the legacy immediate-neighbor smoothing.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1"))
	int32 VisVineGPUPostProjectionSmoothKernelRadius = 4;

	// Light radius-1 (immediate prev/next only) smoothing passes applied after the wide-kernel
	// smoothing, as a final local cleanup.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUPostProjectionSmallSmoothIterations = 2;

	// How strongly the local corner angle modulates the path smoothing. The angle between the
	// incoming/outgoing segments at each point drives the strength: a smaller angle (sharper fold)
	// smooths harder, while near-straight points are left almost untouched. 0 == legacy uniform
	// smoothing (every interior point fully averaged); 1 == full angle-driven modulation.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0", ClampMax = "1"))
	float VisVineGPUPostProjectionSmoothAngleStrength = 1.0f;

	// Before the final tangent smoothing, redistribute each vine's surface points to uniform
	// arc-length spacing along the post-projection surface polyline. Point count is preserved.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options)
	bool bVisVineGPUResampleSurfaceEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "0"))
	int32 VisVineGPUNoiseIterations = 10;

	// Pass C 扫掠截面（Tube）的圆周段数：每个路径点环上的截面顶点数。3 = 三角形（原始行为），
	// 数值越大截面越接近圆。仅影响 Tube 截面，Plane 截面始终为 2 点。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "3"))
	int32 VisVineGPUTubeSegments = 3;

	// 整批藤蔓（全部源共用）的路径点上限，同时也是 GPU buffer 的分配容量。真实点数只有 GPU
	// 知道，所以下游一律按这个上限分配、按 GPU 计数绘制；调大只是多占显存，不改变结果。
	// 每源容量取 min(TargetCount × (SC_MAX_BACKTRACK+1), 本值 / 源数量)，所以在目标数很少时
	// 仍按更紧的理论界分配，不会浪费。
	//
	// 默认值必须覆盖“常见源数 × 理论界”，否则每源份额会低于理论界，EmitSpaceColonizationLinesCS
	// 会在回溯途中撞上容量直接 break —— 藤蔓被悄悄截短，表现为莫名其妙变稀变短。
	// 参考：980 个 target 的理论界是 980×101 ≈ 99K，1M 可以喂满 10 个源；旧实现每源独立按理论界
	// 分配（硬顶 4M），不除源数，所以这里给低了就是纯回归。被截断时会打 Warning 并给出建议值。
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = Options, meta = (ClampMin = "1024"))
	int32 MaxVinePointCount = 1048576;

};


// VisVine debug parameter structs moved to ComputeShaderDebugParams.h

// CPU-prepped inputs for the space-colonization solve. The solve is recorded into the vine mesh
// RDG graph and its output never leaves the GPU, so this bundle is the whole hand-off.
// Defined in GeometryEditorActor.cpp.
struct FVineFusedSCInputs;

UCLASS()
class COMPUTESHADERGENERATOR_API AVineContainer : public AMeshGeneratorBrushCache
{
	GENERATED_BODY()

public:
	AVineContainer(const FObjectInitializer& ObjectInitializer);
	virtual void PostLoad() override;
	virtual void PostRegisterAllComponents() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	// ---- References ----

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* GrowTarget;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UInstancedStaticMeshComponent* TubeVineSource;

	// The vine's renderer: binds VineGeometry and draws it through an indirect draw. It builds nothing —
	// VisVineGPUInternal writes the geometry into VineGeometry and then binds it here. Kept at an
	// identity world transform (the vine renders in world space).
	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UCSMeshRenderComponent* VineGpuMesh;

	// Single source of truth for the vine surface material. It used to live on slot 0 of a
	// UDynamicMeshComponent this actor owned; that component is gone, so the assignment lives on the
	// actor and is pushed down (never read back) to two places at once — VineGpuMesh->MeshMaterial,
	// which the draw uses, and VineGeometry's slot 0, which is what Save Mesh bakes into the asset. See
	// ApplyVineMaterialToLeaf. Leaving it empty is fine: the renderer falls back to the engine
	// default surface material.
	//
	// MeshMaterial is EditAnywhere on the inherited component, so it *looks* authorable there. It is
	// not: every load and every generation overwrites it from here, and a value typed onto the
	// component is neither saved nor honoured.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GrowReference")
	TObjectPtr<UMaterialInterface> VineMaterial;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TargetType;

	UPROPERTY(BlueprintReadWrite, Category = "GrowReference")
	UFoliageType* TubeType;

	// ---- Options ----

	UPROPERTY(BlueprintReadWrite, Category = "Options")
	FVV VV;

	UPROPERTY(BlueprintReadWrite, Category = "Options")
	FSpaceColonizationOptions SC;

	// ---- Debug Options ----

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineGPUProjectionDebugOptions GPUProjectionDebug;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FVisVineSurfaceVoxelDebugOptions SurfaceVoxelDebug;

	// ---- Transient State ----

	FBox InstanceBound;

	// The vine geometry itself: the retained GPU streams the build writes and VineGpuMesh draws.
	// Owned here rather than by the component because this actor, not a render state, decides when a
	// vine exists — that is exactly what survives a proxy recreation now. Transient because GPU data
	// does not survive a level reload; the property exists to hold the object against GC, not to
	// serialize it (EnsureVineGeometry is what brings a vine back after a load).
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> VineGeometry;

	UPROPERTY(Transient)
	TObjectPtr<AActor> DebugVineSplineActor;

	// ---- Core Operations ----

	/** Idempotent "the vine I own must exist" entry point: runs a generation only when there is no
	 *  vine geometry, and returns whether there is one by the time the call is over. Safe to call
	 *  from any lifecycle event and any number of times — the whole point is that this actor, not a
	 *  render-state recreation, decides when the vine is generated.
	 *
	 *  Nothing about a vine is serialized except its sources and targets, so restoring one means
	 *  re-running the whole generation (voxels -> space colonization -> mesh); there is no cached
	 *  bundle to re-push. Now that UCSMeshRenderComponent draws a UCSMesh, a render-state recreation is
	 *  a rebind, which makes this the ONLY thing that regenerates the vine. */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool EnsureVineGeometry();

	/** 藤蔓生成的唯一入口：表面体素 -> 空间殖民求解 -> concat -> 建网格，全部合并进 VineGeometry
	 *  那一次 EditMeshSync 的 RDG 图。以前这里是 4 张图外加一次 FlushRenderingCommands，而藤蔓
	 *  这条路根本不回读数据，那次阻塞是白等的。
	 *  包围盒由本函数从当前 source/target 现算，同时决定体素化范围：蓝图侧拿不到 transform 列表，
	 *  让它自己拼 FBox 只会拼出一个和体素化范围对不上的盒子。
	 *
	 *  **异步**：走 UCSMesh::EditMeshAsync，录完图就返回，游戏线程不等渲染线程。返回值因此表示
	 *  "这次生成已递交"，**不是**"藤蔓已经建好"——绑定 mesh、失败时释放、调试绘制和那两行
	 *  Built/Timing 日志都发生在之后的完成回调里。上一次还在飞时本次直接被拒（见
	 *  bVineBuildInFlight），所以连点生成不会叠加。 */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool GenerateVineGPU();

	/** 独立 GDF 模式：在本世界真实渲染视图中采样全局距离场，不抽取三角形 / 构建表面体素。
	 * 返回值仅表示递交；无可用视图 / GDF 时超时失败。原 GenerateVineGPU 保持原有模式选择。 */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	bool GenerateVineGPUWithGlobalDistanceField();

	/** Drops everything a generation produced — the cached lines and surface triangles, the debug
	 *  spline actor, and the vine geometry itself, which is unbound and released rather than merely
	 *  emptied. Releasing is what keeps EnsureVineGeometry()'s contract: a retained mesh would answer
	 *  "there is already a vine" and refuse to regenerate for the rest of this actor's life. */
	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void Clean();

	// ---- Foliage ----

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	void RebuildDisplayInstancesFromTransformArrays();

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ImportFoliageToTransformArray(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = ContainerCheck)
	virtual void ExportTransformArrayToFoliage(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void FetchFoliage();

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void RevertFoliage();

	// ---- Vine Actions ----

	UFUNCTION(BlueprintCallable, Category = "VineActions")
	void SaveStaticmesh();

	// ---- Scatter ----
	//
	// 藤蔓的两组输入（吸引子 GrowTarget / 起点 TubeVineSource）现场从一个表面 actor 生成，
	// 免去手摆实例。目前吃 StaticMesh 表面（测试拿 cube），将来 TinyGlade 房屋落地后换成
	// 参数化墙面即可 —— 采样源变了，下面的"贴地带筛选 + 最远点抽稀"这套筛法不用动。

	/** 散点的表面来源。空则用本 actor 自己（没有静态网格时什么也采不到）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter")
	TObjectPtr<AActor> ScatterSurfaceActor;

	/** GrowTarget 点数上限。它直接决定每源容量的理论界（TargetCount × 101），别只当密度旋钮。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter", meta = (ClampMin = "1"))
	int32 ScatterTargetCount = 500;

	/** 目标点最小间距 cm（采样器的体素去重格边长）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter", meta = (ClampMin = "0.1"))
	float ScatterTargetSpacing = 20.0f;

	/**
	 * 起点数。**要克制**：每源容量 = min(TargetCount × 101, MaxVinePointCount / 源数)，
	 * 起点一多就会把每源份额压到理论界以下，藤蔓被静默截短（只有一条 Warning）。
	 * 覆盖面靠 GrowTarget 的分布去铺，不是靠堆起点。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter", meta = (ClampMin = "1"))
	int32 ScatterSourceCount = 4;

	/**
	 * 只在"墙面"散点：法线 |Z| 超过本值的三角形（屋顶 / 地板 / 缓坡）整个跳过。
	 * 0.5 ≈ 保留与水平面夹角大于 60° 的面。设 1.0 关掉过滤，全表面散。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float ScatterMaxSurfaceNormalZ = 0.5f;

	/** 散点随机种子。同一个种子出同一批点 —— 性能对比要可复现就别乱改。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter")
	int32 ScatterSeed = 12345;

	/** 贴地带厚度 cm：表面点的 Z 落在 [地面高度, 地面高度 + 本值] 内才算"交界处"。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VineActions|Scatter", meta = (ClampMin = "0.0"))
	float GroundContactBandHeight = 40.0f;

	/** 采样 ScatterSurfaceActor 的表面，重填 GrowTarget。返回落地的实例数。 */
	UFUNCTION(BlueprintCallable, Category = "VineActions|Scatter")
	int32 ScatterTargetsFromSurfaceActor();

	/**
	 * 在 ScatterSurfaceActor 与地面的交界带上重填 TubeVineSource。返回落地的实例数。
	 * 地面高度优先查 ACSGroundActor 的 CPU 镜像（gpumesh 全线 NoCollision，trace 打不到），
	 * 查不到再退到向下 line trace（Landscape / 有碰撞的静态网格）。
	 */
	UFUNCTION(BlueprintCallable, Category = "VineActions|Scatter")
	int32 ScatterSourcesAtGroundContact();

	/** 上面两步一起做（不生成藤蔓）。CallInEditor 的按钮只认无参无返回值的函数，故有这层包装。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "VineActions|Scatter")
	void ScatterVineInputs();

	/** 散点 → 起点 → GenerateVineGPU 一条龙，逐段计时并打出容量诊断。性能评估入口。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "VineActions|Scatter")
	void ScatterAndGenerateTimed();

protected:
	//~ AComputeShaderMeshGenerator interface
	/** 沿用"标签优先"的资产基名（基类默认用 GetName()），保证已烘好的资产名不变。 */
	virtual FString GetResultAssetBaseName() const override;

public:

	// ---- SpaceColonization ----

	// Non-reflected worker (FVineFusedSCInputs isn't a USTRUCT): prepares the CPU side of the
	// fused space-colonization solve. Dispatches nothing; the passes are recorded into the vine
	// mesh graph by the build operator, inside VineGeometry's UCSMesh::EditMeshSync.
	bool PrepareVineFusedSCInputs(const TArray<FTransform>& SourceTransforms, const TArray<FTransform>& TargetTransforms, FVineFusedSCInputs& OutInputs);

	// ---- Debug ----

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug")
	void ClearDebugVineSplineActor();

	UFUNCTION(BlueprintCallable, Category = "VineActions|Debug", meta = (DevelopmentOnly, DisplayName = "Draw Debug Vine Surface Voxel Arrows"))
	int32 DrawDebugVineSurfaceVoxelArrows(float Duration = 5.0f, bool bUseCachedVoxels = false);


private:

	/** Queues one EnsureVineGeometry() for the next engine tick. See the rationale on the
	 *  definition for why the restore is hooked where it is and why it is not run inline. */
	void ScheduleEnsureVineGeometry();

	/** Latched at the first schedule and never cleared: the automatic restore is one shot per actor
	 *  lifetime. Re-arming it on every re-registration would stack tickers, and — because a
	 *  generation that keeps failing leaves the geometry absent — would retry the whole solve every
	 *  frame. Anything past the first attempt is the owner's call (GenerateVineGPU /
	 *  EnsureVineGeometry are both public). */
	bool bVineGeometryRestoreAttempted = false;

	/** True between GenerateVineGPU() recording an async build and that build's completion callback.
	 *  串行化的责任转移了：以前是 EditMeshSync 的 flush 天然保证一次只有一个生成在跑，异步之后
	 *  这个标志才是。没有它，连点两次生成会让第二次覆写第一次还在排队的那批常驻流，且两个完成
	 *  回调都会去绑 mesh。 */
	bool bVineBuildInFlight = false;
	bool GenerateVineGPUInternal(bool bGlobalDistanceField);
	uint64 VineGDFRequestGeneration = 0;

	// GenerateVineGPU 备好、等着交给构建那张图的体素输入。不是 UPROPERTY：里面是 PIMPL 持有的
	// 三角形请求数组，且只在一次生成内有效 —— VisVineGPUInternal 会把它 MoveTemp 进 build
	// bundle，消费后即失效，所以每次 GenerateVineGPU 都重新准备。
	FCSSurfaceVoxelPassInputs PendingSurfaceVoxelInputs;
	TSharedPtr<FCSVineTriangleSurfaceInputs, ESPMode::ThreadSafe> PendingTriangleSurface;

	// One-shot data upgrade for packages saved while this actor still owned a UDynamicMeshComponent.
	// That component used to hold the vine surface material on slot 0, so dropping it would silently
	// strand the artist's assignment. See MigrateLegacyVineMaterial() for how the removed subobject
	// is still read back.
	void MigrateLegacyVineMaterial();

	/** 把丢失的 VineGpuMesh 指针接回同名组件。BP_VineSource 及其关卡实例的存盘数据里这个指针是
	 *  null（组件对象本身在，只是没接上），不修就会在 GenerateVineGPU() 的 !VineGpuMesh 早退处
	 *  静默失败。幂等，指针已有时立即返回。 */
	void RebindVineGpuMeshIfMissing();

	/** Pushes VineMaterial down to the GPU leaf, the one direction the assignment ever travels. */
	void ApplyVineMaterialToLeaf();

	// Set when MigrateLegacyVineMaterial() actually moved a material. MarkPackageDirty is refused
	// while a package is loading, so the dirty flag has to be re-issued from
	// PostRegisterAllComponents — that is what turns the upgrade into a save the user can commit.
	bool bPendingLegacyVineMaterialDirty = false;
};

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSNaniteCut.generated.h"

class UActorComponent;
class UCSMesh;
class UMaterialInterface;
class UStaticMesh;
class UStaticMeshComponent;

// -----------------------------------------------------------------------------
// Nanite 截面抽取：把 Nanite 网格按一个网格局部空间的误差阈值（CutError）切出来，写进 UCSMesh。
//
// Nanite 没有"第几级 LOD"，只有误差：父级误差大于 CutError 才往下走，cluster 自身误差小于 CutError
// 就输出。CutError 越大截面越粗，0 是当前驻留的最细一级。截面与视角无关 —— 同一个 CutError、同样的
// 驻留数据，解出来的三角形逐位相同。
//
// 与 UCSMeshOps::CopyFromStaticMesh 的区别：后者读 RenderData->LODResources，对 Nanite 网格拿到的是
// fallback（构建期切一刀再额外简化，每资产只有一份）；这里读的是 Nanite 本体，CutError 任意。
//
// 全程在 GPU 上：层级遍历与挑选是一趟计数 pass（回读几 KB 的计划），解码写出是第二趟。同步、阻塞，
// 与 UCSMeshOps 的既有契约一致，面向编辑器 / 离线用途（HLOD 构建）。设计见插件根 NaniteClusterHLOD_Plan.md。
// -----------------------------------------------------------------------------

/** 一个源的抽取结果。 */
UENUM(BlueprintType)
enum class ECSNaniteCutStatus : uint8
{
	/** 截面就是该 CutError 的精确结果，与相机和流送历史无关。 */
	Complete,
	/** 有些地方更细的页没驻留，被迫用了更粗的 cluster（截面仍然无缝，只是那几处比要求的粗）。 */
	Incomplete,
	/** Nanite 资源还没登记进流送管理器（资产刚加载、渲染资源没就绪），这次什么都没写。 */
	NotReady,
	/** 空网格，或没有 Nanite 数据（没开 Nanite、平台不支持）。 */
	NotNanite,
	/** 暂不支持的 Nanite 数据：Assembly、多根资源、Voxelize 形状保持（远处几层是体素，没有三角形）。 */
	Unsupported,
	/** Incomplete 且策略是 SkipSource，按要求没写。 */
	Skipped,
	/** 容量估错、显存预检拒绝等，整批没写。 */
	Failed,
};

/** 更细的页没驻留时怎么办。 */
UENUM(BlueprintType)
enum class ECSNaniteCutIncompletePolicy : uint8
{
	/** 照写驻留着的：缺页处用更粗的 cluster，截面无缝，只是那几处比要求的粗。 */
	UseAvailable,
	/** 这个源整个不写。 */
	SkipSource,
};

/** 一个要抽取的源：网格、摆放、阈值、材质。 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSNaniteCutSource
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	TObjectPtr<UStaticMesh> Mesh = nullptr;

	/** 局部到世界的变换，烘进写出的位置（UCSMesh 的常驻流约定是世界空间）。行列式为负（镜像）时自动翻转绕序。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	FTransform Transform = FTransform::Identity;

	/** 网格局部空间的误差阈值（cm）。≤0 取当前驻留的最细一级。按距离换算见 CutErrorForScreenError，
	 *  那里给的是世界空间值，要除以实例三个轴缩放的最小值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut", meta = (ClampMin = "0"))
	float CutError = 0.0f;

	/** 按材质槽覆盖材质（通常取组件上的 GetMaterial(i)）。缺的槽或空指针用网格自己的材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	TArray<TObjectPtr<UMaterialInterface>> OverrideMaterials;

	/** 额外翻转绕序（与镜像自动翻转叠加）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	bool bFlipWinding = false;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSNaniteCutOptions
{
	GENERATED_BODY()

	/** 本次新增三角形的上限；0 不限制。计数后、分配和解码前拒绝超限，绝不截断表面。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut", meta = (ClampMin = "0"))
	int32 MaxTriangles = 0;

	/** Optional C++ caller budget for live extraction buffers, including traversal and decode scratch.
	 * Zero preserves the existing unlimited contract. Source Nanite streaming resources are excluded. */
	uint64 MaxWorkingMemoryBytes = 0;

	/** 追加到 Target 已有内容之后；关掉则替换。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	bool bAppend = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	ECSNaniteCutIncompletePolicy IncompletePolicy = ECSNaniteCutIncompletePolicy::UseAvailable;

	/**
	 * Incomplete 的源显式请求它全部流送页（Nanite::FStreamingManager::RequestNanitePages）。
	 * 页要跨帧才流进来：这次的结果不会变，之后（编辑器过几帧）再抽一次才会是完整的。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	bool bRequestMissingPages = false;

	/**
	 * 目标材质表不按材质去重：每个源的每个槽各占一项，逐三角材质号因此能反查出三角来自哪个源
	 * （FCSNaniteCutResult::MaterialSources）。烘焙要按源给材质喂它自己的变换与图元数据时用；
	 * 代价是 section 数 = 源数 × 槽数。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS NaniteCut")
	bool bUniqueMaterialsPerSource = false;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSNaniteCutSourceResult
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	ECSNaniteCutStatus Status = ECSNaniteCutStatus::Failed;

	/** 选中的三角形 cluster 数。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 Clusters = 0;

	/** 写出的顶点数（cluster 边界上的顶点各存一份，位置逐位相同，需要共享时跑 WeldVertices）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 Vertices = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 Triangles = 0;

	/** 误差不够小、只因更细的页没驻留才被迫输出的 cluster 数。0 才是精确截面。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 ForcedClusters = 0;

	/** 选中但没有三角形的体素 cluster 数（跳过未写）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 VoxelClusters = 0;
};

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSNaniteCutResult
{
	GENERATED_BODY()

	/** 与传入的源一一对应。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	TArray<FCSNaniteCutSourceResult> Sources;

	/** 这次写进 Target 的顶点 / 三角数（不含 Target 原有内容）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 WrittenVertices = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	int32 WrittenTriangles = 0;

	/** 每个源都是 Complete。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	bool bAllComplete = false;

	/**
	 * 与写出后的目标材质表逐项对应：这一项来自哪个源（Sources 下标）。只在 bUniqueMaterialsPerSource
	 * 时有意义；去重合并的项、追加前就有的项为 -1。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	TArray<int32> MaterialSources;

	/** 同上，这一项来自源网格的哪个材质槽。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS NaniteCut")
	TArray<int32> MaterialSourceSlots;
};

UCLASS()
class COMPUTESHADERGENERATOR_API UCSNaniteCutOps : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 把一组 Nanite 网格的截面写进 Target。一次计数 + 一次写出，源再多也只有这两趟（加一次扩容）。
	 *
	 * 材质：每个源的材质（覆盖优先）按指针去重并入 Target->Materials，逐三角材质号指向合并后的表。
	 * 写出之后 Target 没有 section 表（与所有改计数的算子一样），要分材质画就再跑 BuildMaterialSections。
	 *
	 * 游戏线程；阻塞。Target 为空时原样返回。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Nanite")
	static UPARAM(DisplayName = "Target") UCSMesh* AppendNaniteCuts(
		UCSMesh* Target, const TArray<FCSNaniteCutSource>& Sources, const FCSNaniteCutOptions& Options, FCSNaniteCutResult& OutResult);

	/** 单个源的便捷版。 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Nanite")
	static UPARAM(DisplayName = "Target") UCSMesh* AppendNaniteCut(
		UCSMesh* Target, UStaticMesh* Mesh, const FTransform& Transform, float CutError,
		const FCSNaniteCutOptions& Options, FCSNaniteCutResult& OutResult);

	/**
	 * 世界空间的 CutError：Nanite 在 Distance 处画一个缩放为 1 的物体时，误差恰好是 PixelError 个像素。
	 *
	 * 由 Nanite 自己的判据反推（NaniteShared.cpp:92 的 LODScale、NaniteClusterCulling.usf:320 的可画条件）：
	 * LODScale = ScreenWidth / (2·tan(HorizontalFOV/2))，CutError = PixelError × Distance / LODScale。
	 * 默认参考视图同引擎 HLOD Simplify 构建器（1920 宽、水平 FOV 90°），此时 CutError ≈ Distance / 960。
	 * 喂给 FCSNaniteCutSource::CutError 前要除以实例三个轴缩放的最小值。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Nanite")
	static float CutErrorForScreenError(float Distance, float PixelError = 1.0f, float ScreenWidth = 1920.0f, float HorizontalFOVDegrees = 90.0f);

	/**
	 * 显式请求网格的全部 Nanite 流送页。页要跨帧才流进来，所以调完之后过几帧再抽取才会是 Complete。
	 * 返回是否发出了请求（网格没有 Nanite 数据、资源没登记、没有流送页时为 false）。游戏线程。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GpuMesh|Nanite")
	static bool RequestNaniteResidency(UStaticMesh* Mesh);

	// -------------------------------------------------------------------------
	// HLOD 收集：WP 构建器（UCSNaniteCutHLODBuilder）与关卡内试验台（ACSNaniteCutHLODActor）共用，
	// 两边的筛选规则因此不会分叉。
	// -------------------------------------------------------------------------

	/** 默认的排除标签：actor 或组件带着它就不进截面 HLOD。 */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Nanite")
	static FName DefaultExcludeTag();

	/** 组件自己或它的 actor 带 ExcludeTag。ExcludeTag 为 None 时谁都不排除。 */
	static bool IsExcludedByTag(const UActorComponent* Component, FName ExcludeTag);

	/** 默认的挑选标签：PickTag 填上之后，只有带这个标签的 actor / 组件进截面 HLOD。 */
	UFUNCTION(BlueprintPure, Category = "CS GpuMesh|Nanite")
	static FName DefaultPickTag();

	/** 组件自己或它的 actor 带 PickTag。PickTag 为 None 时都算挑中（不限制），与 ExcludeTag 正好相反。 */
	static bool IsPickedByTag(const UActorComponent* Component, FName PickTag);

	/** 组件自己或它的 actor 带这个标签；比较时忽略空格 / 下划线 / 连字符与大小写。 */
	static bool ComponentOrOwnerHasTag(const UActorComponent* Component, FName Tag);

	/**
	 * 一组静态网格组件 → 抽取源。ISM / HISM 每个实例一个源；材质按组件上的覆盖逐槽取；
	 * CutError = WorldCutError ÷ 实例三个轴缩放的最小值（误差在网格局部空间，Nanite 也这么换算）。
	 * 跳过带 ExcludeTag 的（计入 OutNumExcluded）与 foliage 组件（计入 OutNumFoliage，小物体归 foliage 管，
	 * 不进 HLOD）；没挂网格的直接忽略。OutMaxTexCoords 是这些网格 LOD0 用到的最大 UV 组数（≥1）。
	 * OutSourceComponentIndices（可选）与 OutSources 一一对应，记每个源来自 Components 的第几个 ——
	 * 抽取结果按源下标给出，要知道哪些组件真进了 HLOD 就靠它反查。
	 */
	static void MakeSourcesFromComponents(
		const TArray<UStaticMeshComponent*>& Components, float WorldCutError, FName ExcludeTag,
		TArray<FCSNaniteCutSource>& OutSources, int32& OutNumExcluded, int32& OutNumFoliage, int32& OutMaxTexCoords,
		TArray<int32>* OutSourceComponentIndices = nullptr);
};

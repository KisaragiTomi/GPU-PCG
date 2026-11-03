#pragma once

#include "CoreMinimal.h"
#include "CSClothBandSplit.h"
#include "Engine/AssetUserData.h"
#include "GameFramework/Actor.h"
#include "CSClothSplitActor.generated.h"

class FSkeletalMeshLODRenderData;
class FSkinWeightVertexBuffer;
class UAnimationAsset;
class UCSClothSimComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class USkeletalMesh;
class USkeletalMeshComponent;
class UStaticMesh;

/**
 * 切好的布挂在 StaticMesh 上的那份 primuv 绑定（ACSClothSplitActor::BakeSplitToAsset 写，运行时读）。
 *
 * 每个数组按「烘焙顶点号」排：StaticMesh 构建会合并、重排顶点，烘焙时把顶点号写进第 VertexIdChannel 条 UV
 * （全精度 float，整数精确），运行时按每个渲染顶点的这条 UV 取回自己那一项。
 */
UCLASS()
class COMPUTESHADERGENERATOR_API UCSClothSplitBakeData : public UAssetUserData
{
	GENERATED_BODY()

public:
	/** 绑定所对的骨骼网格：锚点是它 LOD 0 的渲染顶点号，换了网格就对不上（运行时按顶点数核对）。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	TSoftObjectPtr<USkeletalMesh> SourceMesh;

	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	int32 SourceNumVertices = 0;

	/** 烘焙时的动画与时刻：切分做在这个姿态上。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	TSoftObjectPtr<UAnimationAsset> Animation;

	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split", meta = (Units = "s"))
	float StartTime = 0.0f;

	/** 切分前的重网格边长；0 = 没重网格，直接切的 LOD 0。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split", meta = (Units = "cm"))
	float RemeshEdgeLength = 0.0f;

	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	FCSClothBandSplitSettings Split;

	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	int32 NumBands = 0;

	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	int32 NumPieces = 0;

	/** 存烘焙顶点号的 UV 通道。 */
	UPROPERTY(VisibleAnywhere, Category = "CS Cloth Split")
	int32 VertexIdChannel = 1;

	// 每烘焙顶点一项，含义同 FCSClothVertexBinding。
	UPROPERTY()
	TArray<uint32> WeldKeys;

	UPROPERTY()
	TArray<FIntVector> AnchorSources;

	UPROPERTY()
	TArray<FVector3f> AnchorWeights;

	UPROPERTY()
	TArray<FVector4f> AnchorParams;

	UPROPERTY()
	TArray<FVector3f> SwirlDirs;
};

namespace CSClothSkinning
{
/**
 * 一个 LOD 的 CPU 蒙皮位置（组件空间）：USkinnedMeshComponent::ComputeSkinnedPositions 的并行版，逐顶点算式相同
 * （权重为 0 的骨骼跳过）。引擎那份逐顶点单线程，4.6 万顶点的角色每帧 ~4.8 ms（2026-10-28 实测）。
 * RefToLocals 来自 USkinnedMeshComponent::CacheRefToLocalMatrices。
 */
COMPUTESHADERGENERATOR_API void ComputeSkinnedPositions(const FSkeletalMeshLODRenderData& LODData, const FSkinWeightVertexBuffer& Weights,
	TConstArrayView<FMatrix44f> RefToLocals, TArray<FVector3f>& OutPositions);
}

/**
 * 骨骼网格角色碎成布条：Houdini 场景 AnimationFallSplit 的 UE 移植（Docs/HoudiniFallSplit_Port.md）。
 *
 * Body 播一段动画；重建时取它此刻的蒙皮姿态，按到起点的测地距离切成一圈圈条带（CSClothBandSplit），上传成布。
 * Body 随后藏起来但接着播 —— 它只用来每帧算锚点；布在放开之前硬跟着锚点走，看上去就是那个角色在动。
 * 放开（布料组件的 ReleaseDelay）之后条带带着动画速度 × 5 甩出去，旋流场、噪声风、按曲线收放的绳把它们留在身边。
 *
 * 编辑器视口里也跑（不进 PIE）；LoopPeriod > 0 时到点 Replay() 重播一次（沿用切好的条带，不重建）。
 * 重网格 + 切分可以事先烘成 StaticMesh（BakeSplit）：设了 BakedSplit，运行时只读它和它的 primuv 绑定。
 * ⚠️ 蒙皮位置在 CPU 上算（CSClothSkinning::ComputeSkinnedPositions）：编辑器里骨骼网格的 CPU 数据常在；
 * cook 出去的包要在骨骼网格的 LOD 0 上勾 Allow CPU Access，否则重建失败并写日志。
 */
UCLASS(meta = (PrioritizeCategories = "CS Cloth Split"))
class COMPUTESHADERGENERATOR_API ACSClothSplitActor : public AActor
{
	GENERATED_BODY()

public:
	ACSClothSplitActor();

	/** 被切开的角色：在这里设骨骼网格。藏起来以后仍每帧求姿态（锚点要用）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Cloth Split")
	TObjectPtr<USkeletalMeshComponent> Body;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Cloth Split")
	TObjectPtr<UCSMeshRenderComponent> RenderComponent;

	/** 布料：放开时间、绳的两条曲线、旋流与风都在它身上（默认值是 Houdini 场景的参数）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Cloth Split")
	TObjectPtr<UCSClothSimComponent> ClothComponent;

	/** 单节点模式播放的动画。为空则用 Body 现有的姿态。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split")
	TObjectPtr<UAnimationAsset> Animation;

	/** 从动画的第几秒起播：切分在这一刻的姿态上做。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split", meta = (ClampMin = "0.0", Units = "s"))
	float StartTime = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split", meta = (ClampMin = "0.0"))
	float PlayRate = 1.0f;

	/** > 0：每隔这么多秒 Replay() 一次（编辑器里反复看效果）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split", meta = (ClampMin = "0.0", Units = "s"))
	float LoopPeriod = 6.0f;

	/** 条带的材质。为空用 Body 的第 0 号材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split")
	TObjectPtr<UMaterialInterface> Material;

	/** 切分参数。起点写在本 actor 的局部空间里；为空时从头顶切起。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split")
	FCSClothBandSplitSettings Split;

	/** 切分前先把 Body 重网格成这个边长的均匀三角（Houdini remesh2：1 cm）。≤ 0 = 不重网格，直接切 LOD 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split", meta = (ClampMin = "0.0", Units = "cm"))
	float RemeshEdgeLength = 1.0f;

	/**
	 * 切好的布：BakeSplit 烘出来的 StaticMesh（带 UCSClothSplitBakeData）。设了就直接用它：运行时不重网格、不切分，
	 * 只按烘好的 primuv 绑定把每个顶点挂到 Body 的蒙皮三角上（Houdini：attribinterpolate 按 sourceprim / sourceprimuv）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split")
	TObjectPtr<UStaticMesh> BakedSplit;

#if WITH_EDITORONLY_DATA
	/** BakeSplit 的资产路径；为空 = /Game/ClothSplit/SM_<骨骼网格名>_Split。 */
	UPROPERTY(EditAnywhere, Category = "CS Cloth Split")
	FString BakeAssetPath;
#endif

	/** 过本 actor 原点的水平地面（Houdini 的 ground plane 在角色脚底 y = 0）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth Split")
	bool bGroundPlane = true;

	/**
	 * 重建：动画回到 StartTime，重新切分、重新 setup，回到放开之前。5 万粒子的演示要占游戏线程 ~280 ms
	 * （2026-10-28 实测）；只想再放一遍用 Replay()。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Cloth Split")
	void Restart();

	/**
	 * 重播：Body 回到 StartTime，布回到 setup 时的 rest 姿态、重新进入跟随阶段。沿用切好的条带与约束 ——
	 * 起始姿态每次都一样，不必重新切分、重新 setup。还没建过或布已停摆时退回 Restart()。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Cloth Split")
	void Replay();

#if WITH_EDITOR
	/** 在 StartTime 的姿态上重网格、切分，烘到 BakeAssetPath（标脏、不存盘），并设成 BakedSplit。 */
	UFUNCTION(CallInEditor, Category = "CS Cloth Split")
	void BakeSplit();

	/** 同 BakeSplit，路径与是否存盘由调用方定（脚本）。AssetPath 为空时用默认路径。失败返回 nullptr。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth Split")
	UStaticMesh* BakeSplitToAsset(const FString& AssetPath, bool bSaveToDisk);

	/** 烘进调用方给的 StaticMesh（可以是 transient 的，测试用），并设成 BakedSplit。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth Split")
	bool BakeSplitInto(UStaticMesh* StaticMesh);
#endif

	UFUNCTION(BlueprintPure, Category = "CS Cloth Split")
	int32 GetNumPieces() const { return NumPieces; }

	UFUNCTION(BlueprintPure, Category = "CS Cloth Split")
	int32 GetNumBands() const { return NumBands; }

	UFUNCTION(BlueprintPure, Category = "CS Cloth Split")
	UCSMesh* GetClothMesh() const { return ClothMesh; }

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void PostRegisterAllComponents() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditUndo() override;
#endif

private:
	/** 没有 GPU 的场合（cook / commandlet / -NullRHI）不建：本插件的全局 shader 在那里不存在。 */
	bool CanBuild() const;

	/** Body 回到 StartTime 的姿态，当场求一次骨骼（编辑器里还没 tick 过、刚换了动画都要这一步）。 */
	void PoseBodyAtStart();

	/** LOD 0 的蒙皮位置（Body 的组件空间）。没有 CPU 数据时返回 false。 */
	bool ComputeBodyPositions(TArray<FVector3f>& OutPositions);

	/** 当前姿态的世界空间快照（位置、蒙皮后的切线基、UV、顶点色、按材质分段的三角）。 */
	bool CaptureBody(FCSGpuMeshCPUData& OutSnapshot);

	/** 把本帧的蒙皮位置交给布料组件当锚点源。 */
	void PushAnchors();

	/** 现场切：抓 Body 快照 → 重网格（RemeshEdgeLength > 0）→ 切分 → primuv 绑回 LOD 0。网格在世界空间。 */
	bool BuildSplit(FCSGpuMeshCPUData& OutMesh, FCSClothVertexBinding& OutBinding);

	/** 从 BakedSplit 取网格（Body 组件空间 → 世界）与绑定；不是本 Body 的烘焙结果、没有 CPU 数据时返回 false 并写日志。 */
	bool LoadBakedSplit(FCSGpuMeshCPUData& OutMesh, FCSClothVertexBinding& OutBinding);

#if WITH_EDITOR
	/** 切好的网格写进 StaticMesh：Body 组件空间、UV1 = 烘焙顶点号、挂上 UCSClothSplitBakeData。 */
	bool WriteBakedSplit(UStaticMesh* StaticMesh, const FCSGpuMeshCPUData& Mesh, const FCSClothVertexBinding& Binding);
#endif

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> ClothMesh;

	bool bRebuildPending = true;
	bool bWarnedNoCpuData = false;
	double TimeSinceRestart = 0.0;
	int32 NumPieces = 0;
	int32 NumBands = 0;
	TArray<FMatrix44f> CachedRefToLocals;
	TArray<FVector3f> SkinnedPositions;
};

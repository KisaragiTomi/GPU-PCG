#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderGenerateHelper.h"
#include "MeshGeneratorBrushCache.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Components/StaticMeshComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/DecalComponent.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "TimerManager.h"

#define CSSW_VELOCITY_CLAMP 4

class FObjectPreSaveContext;
class AStaticMeshActor;
class ADecalActor;

#include "ComputeShaderShallowWater.generated.h"

UENUM(BlueprintType)
enum class EWaterfallExpansion : uint8
{
	Expansion_5  = 0 UMETA(DisplayName = "5"),
	Expansion_7  = 1 UMETA(DisplayName = "7"),
	Expansion_10 = 2 UMETA(DisplayName = "10"),
	MAX          = 3 UMETA(Hidden),
};



UCLASS(HideCategories=(Replication), meta=(PrioritizeCategories="SWParameter"))
class COMPUTESHADERGENERATOR_API ACSShallowWaterCapture : public AMeshGeneratorBrushCache
{
	GENERATED_BODY()
public:
	ACSShallowWaterCapture(const FObjectInitializer& ObjectInitializer);

	// -------------------------------------------------------------------------
	// Components
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UStaticMeshComponent* ReusltMesh;

	UPROPERTY(Transient, BlueprintReadWrite, DuplicateTransient, Category = "Debug")
	UHierarchicalInstancedStaticMeshComponent* SimVisHISM;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UDecalComponent* CausticsDecal;

	// -------------------------------------------------------------------------
	// Simulation Parameters
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* WaterMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	UMaterialInterface* DecalMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	bool CloseBound = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000, ClampMin="1", ClampMax="32", UIMin="1", UIMax="8"))
	int32 Iteration = 1;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	EWaterfallExpansion WaterfallExpansionIterations = EWaterfallExpansion::Expansion_10;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float DT = .1;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float Friction = 0.005;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float AdvectFoam = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float FoamFadeSpeed = 0.001;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter",
		Meta=(Priority=1000, ClampMin="0", UIMin="0",
			  ToolTip="Max height gap (cm) before a cell is clamped to its neighbours. 0 = disabled."))
	float SpikeClampHeight = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float MaxWaterRisePerFrame = 40.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float WorldPixelSize = 40;

	UPROPERTY(BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	float SeaLevel = -1000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWParameter", Meta=(Priority=1000))
	FName SWCaptureTag = FName("CSSW");

	// -------------------------------------------------------------------------
	// Capture Parameters
	// -------------------------------------------------------------------------

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	FName SWTag = FName("CSSW_Bake");

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float MaxHeight = 10000;

	UPROPERTY(BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float Scale3DZ = 100;

	int32 TextureSize = 256;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	float CaptureSize = 10000;

	/** 带 tag 的道具用渲染器拍深度（俯视正交、只画深度的 custom render pass），得到与画面一致的高度：
	 *  **透明材质不写深度**、Masked 逐像素裁出剪影、WPO 生效、Nanite 是全精度而不是 fallback 低模。
	 *  渲染器拍不到的组件（隐藏、无场景代理）仍走三角形光栅化，不会从高度图里消失。
	 *  关掉则全部道具走三角形光栅化——那条路**不看材质**：玻璃 / 水面片会被当成实心挡板写进深度图，
	 *  树叶卡片是整块矩形，Nanite 只读得到 fallback 低模（没有 fallback 的直接缺席）。
	 *  代价：LOD、cull distance 与 WPO 由渲染器说了算，高度图因此不再逐位可复现。地形不受影响。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer", Meta=(Priority=1000))
	bool bCaptureWithRenderer = true;

	// -------------------------------------------------------------------------
	// Bake System
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=1000))
	bool bUseBakedResultMesh = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(EditCondition="bUseBakedResultMesh", Priority=1000))
	UStaticMesh* BakedResultMesh = nullptr;

	/** 烘焙出的水面材质实例（MI_CSSW_Water_<编号>），指向落盘的 T_CSSW_* 贴图。
	 *  展示烘焙结果时用它替代运行时 MID：MID 绑的是 transient render target，关卡重开即失效。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(EditCondition="bUseBakedResultMesh", Priority=1000))
	UMaterialInterface* BakedWaterMaterial = nullptr;

	/** 烘焙出的焦散 decal 材质实例（MI_CSSW_Decal_<编号>），与 BakedWaterMaterial 同批生成。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(EditCondition="bUseBakedResultMesh", Priority=1000))
	UMaterialInterface* BakedDecalMaterial = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader|Bake", Meta=(Priority=999))
	UStaticMesh* SimulationPreviewMesh = nullptr;

	// -------------------------------------------------------------------------
	// Simulation Runtime (Transient)
	// -------------------------------------------------------------------------

	UPROPERTY(Transient, NonTransactional, EditAnywhere, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_DebugView;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_VelocityHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultVelHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_ResultDepthWet;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SmoothHeight;

	UPROPERTY(Transient, NonTransactional, BlueprintReadWrite, Category = "Debug")
	UTextureRenderTarget2D* RT_SceneDepth = nullptr;

	UPROPERTY(Transient, NonTransactional)
	UTextureRenderTarget2D* RT_TileMask = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	FVector2D SimUVCenter;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVSize = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "SimVis")
	float SimUVInvSize = 0.f;

	FTimerHandle ConstructionDebounceHandle;
	int32 SolverGeneration = 1;
	uint64 LastSolverFrameNumber = 0;

	// -------------------------------------------------------------------------
	// Debug
	// -------------------------------------------------------------------------

	/** 烘焙资产名尾部的稳定编号（DDHHMMSS）。负值表示尚未生成，首次烘焙时由
	 *  EnsureSWUniqueID() 赋值并随 actor 存盘，之后每次烘焙都复用，因此重复烘焙覆盖同一批资产。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug", Meta=(Priority=1000))
	int32 SWUniqueID = -99999;

	/** 返回（首次调用时生成）本 actor 的烘焙编号。SM_/MI_/T_ 三类烘焙资产共用它。 */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	int32 EnsureSWUniqueID();

	UPROPERTY(BlueprintReadWrite, Category = "Debug")
	UStaticMesh* DebugMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Debug|DebugViewPlane")
	UMaterialInterface* DebugViewPlaneMaterial;

	// -------------------------------------------------------------------------
	// Public Functions
	// -------------------------------------------------------------------------

	virtual void PostLoad() override;
	virtual void BeginPlay() override;
	virtual void PreSave(FObjectPreSaveContext ObjectSaveContext) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
	virtual void BeginDestroy() override;
	virtual void OnConstruction(const FTransform& Transform) override;

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ConstructionComponent();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CheckAndCreateTexture_SWSourcePoint()
	{
		if (!GetWorld() || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return false;

		const int32 SafeTextureSize = FMath::RoundUpToPowerOfTwo(
			FMath::Max(16, FMath::CeilToInt32(CaptureSize / FMath::Max(WorldPixelSize, 1.0f))));
		TextureSize = SafeTextureSize;

		if (RT_DebugView == nullptr) RT_DebugView = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		if (RT_VelocityHeight == nullptr) RT_VelocityHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_ResultVelHeight == nullptr) RT_ResultVelHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(0, 0, -9999, 1), true, false);
		if (RT_SmoothHeight == nullptr) RT_SmoothHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA32f,FLinearColor(-9999, -9999, -9999, -9999), true, false);
		if (!RT_DebugView || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_SmoothHeight) return false;

		if (RT_ResultDepthWet == nullptr)
		{
			RT_ResultDepthWet = UKismetRenderingLibrary::CreateRenderTarget2D(this, SafeTextureSize, SafeTextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor(-9999, -9999, -9999, -9999), true, false);
			if (!RT_ResultDepthWet) return false;
		}
		SetMaterialParameter();
		return true;
	}

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void ShallowWaterSolverSoucePoint(int32 InIteration);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CaptureAll();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void SetHeight();

	/** Replace the entire simulation from ReusltMesh (or BakedResultMesh while previewing).
	 *  Restores surface height and CSSW vertex-color velocity / wet coverage / foam against current ground.
	 *  Uncovered cells become dry. Keeps the solver's running or paused state. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Bake")
	void ApplyResultMeshToSimulation();


	UFUNCTION(BlueprintNativeEvent)
	void SetMaterialParameter();
	virtual void SetMaterialParameter_Implementation();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void ReleaseTransientRenderResources();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void Clean();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader", Meta=(ClampMin="0", UIMin="0"))
	void StartSolver(float TimerRate = 0.0f,
		UPARAM(meta = (ClampMin = "1", ClampMax = "32", UIMin = "1", UIMax = "8")) int32 InIteration = 1);

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader")
	void StopSolver();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader")
	void OnSolverStarted();
	virtual void OnSolverStarted_Implementation();

	DECLARE_DELEGATE_OneParam(FOnBakeResultMesh, ACSShallowWaterCapture*);
	static FOnBakeResultMesh OnBakeResultMeshDelegate;

	UFUNCTION(BlueprintCallable, Category = "SWParameter", Meta=(DevelopmentOnly))
	void BakeResultMesh();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void BrowseBakedAssets();

	UFUNCTION(BlueprintNativeEvent, Category = "ComputeShader|Bake")
	void OnBakeComplete();
	virtual void OnBakeComplete_Implementation();

	/** 生成两个静态代理 actor：StaticMeshActor（复制 ReusltMesh 当前网格/材质/变换）+
	 *  DecalActor（复制 CausticsDecal 当前材质/DecalSize/变换），并挂到本 actor 下。
	 *  先销毁本 actor 已挂载的代理，再重建，因此可重复调用。不销毁自身，销毁时机由调用方控制。 */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader|Bake")
	void SpawnResultProxyActors(AStaticMeshActor*& OutMeshActor, ADecalActor*& OutDecalActor);

	/** 有烘焙/结果网格时，生成一对静态代理 actor（StaticMeshActor + DecalActor）挂到本 actor 下、
	 *  打上 AutoProxyTag，并隐藏本体的 ReusltMesh / CausticsDecal —— 让关卡里留下的是普通 actor，
	 *  不再依赖本 actor 的运行时组件。没有结果网格时整个跳过（不生成、也不隐藏本体）。
	 *  重复调用安全：挂载前先销毁已挂载的同 tag 代理。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "ComputeShader|Bake")
	void SpawnAutoResultActors();

	/** 结果网格的资产路径（CSSW_<SWUniqueID>_SM）。覆写基类的 SM_<BaseName>_<Tag> 模板：
	 *  那套前缀在前的命名与 CSSW 的后缀命名对不上。NameSuffix 仍追加在最后。 */
	virtual FString BuildResultAssetPath(const FString& NameSuffix = TEXT("")) override;

	/** 烘焙产物的统一取名入口：<CSSWData 目录>/CSSW_<SWUniqueID>_<Suffix>。
	 *  SM / Water / Decal / 贴图全部经由它取名，烘焙流程（PCGEditorProcess）也调用它，
	 *  所以命名规则只此一处。 */
	FString BuildBakedAssetPath(const FString& Suffix);
	
	UFUNCTION(BlueprintCallable, Category = "Debug|DebugViewPlane")
	void ShowDebugViewPlane(float Duration = 5.0f);

protected:
	// 烘焙产物统一命名为 CSSW_<SWUniqueID>_<后缀>，全部落在关卡同级的 CSSWData 文件夹里。
	// 基类的 SM_<BaseName>_<Tag> 模板前缀在前，与这套后缀命名对不上，所以直接覆写路径构造；
	// GetResultAssetBaseName 因此不再参与 CSSW 的命名，不再覆写它。
	virtual FString GetResultAssetFolderName() const override { return TEXT("CSSWData"); }
	virtual FString GetResultAssetUniqueTag() override;

private:
	/** 代理 actor 的 tag：SpawnAutoResultActors 生成时打上，Clean 与再次生成时据此回收。 */
	static const FName AutoProxyTag;

	/** 把烘焙 MIC（BakedWaterMaterial / BakedDecalMaterial）贴回结果网格 / SimVis / 焦散 decal，
	 *  保证运行时 MID 不会覆盖掉指向落盘贴图的烘焙材质。 */
	void ApplyBakedMaterials();

	/** 销毁挂在本 actor 下、带 Tag 的 actor；Tag 为 None 时销毁全部挂载 actor。返回销毁个数。 */
	int32 DestroyAttachedProxyActors(FName Tag);

	/** 按 ReusltMesh / CausticsDecal 当前的网格、材质与世界变换生成一对代理 actor，打上 ProxyTag
	 *  并挂到本 actor 下。不负责回收旧代理，调用方自行决定回收策略。 */
	void BuildProxyActors(FName ProxyTag, AStaticMeshActor*& OutMeshActor, ADecalActor*& OutDecalActor);

	void ClearSolverTimer();
	void ScheduleSolverTimerTick();
	void StopSimulationRuntime();
	void ResetSolverState(bool bAdvanceGeneration);
	bool CleanRenderTargets();
	void RefreshConstructionLayout();
	void RebuildSimulationVisualization();
	void ShowBakedResult();
	void UpdateSimUV();
	void EnsureTileMask(int32 Width, int32 Height);
	void EnsureRTSizes();

	TWeakObjectPtr<AActor> DebugViewPlaneActor;
	FTimerHandle DebugViewPlaneTimerHandle;
	FTimerHandle SolverTimerHandle;
	float SolverTimerRate = 0.0f;
};




UCLASS()
class COMPUTESHADERGENERATOR_API ACSSHallowWaterSource : public AActor
{
	GENERATED_BODY()
};

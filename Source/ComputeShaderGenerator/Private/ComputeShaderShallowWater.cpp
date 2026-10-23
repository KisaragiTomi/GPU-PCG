#include "ComputeShaderShallowWater.h"
#include "CSBoxSceneCollection.h"
#include "CSBoxSceneCollectionImpl.h"
#include "CSNaniteHeightCapture.h"
#include "ComputeShaderGenerateHelper.h"
#include "EngineUtils.h"
#include "Landscape.h"
#if WITH_EDITOR
#include "AssetCompilingManager.h"
#endif
#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DecalActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"
#include "SparseTileDispatchHelper.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ObjectSaveContext.h"
#if WITH_EDITOR
#include "Editor.h"
#endif

DECLARE_STATS_GROUP(TEXT("CSSW"), STATGROUP_CSSW, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CSSW Execute"), STAT_CSSW_Execute, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Capture"), STAT_CSSW_Capture, STATGROUP_CSSW)
DECLARE_CYCLE_STAT(TEXT("CSSW Total"), STAT_CSSW_Total, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW CollectSources"), STAT_CSSW_CollectSources, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW EnqueueRender"), STAT_CSSW_EnqueueRender, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW BuildHISM"), STAT_CSSW_BuildHISM, STATGROUP_CSSW);
DECLARE_CYCLE_STAT(TEXT("CSSW SetMaterial"), STAT_CSSW_SetMaterial, STATGROUP_CSSW);
DECLARE_GPU_STAT_NAMED(Stat_ShallowWater, TEXT("ShallowWater"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Compact, TEXT("SW_Compact"));
DECLARE_GPU_STAT_NAMED(Stat_SW_VelHeight, TEXT("SW_VelHeight"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Integrate, TEXT("SW_Integrate"));
DECLARE_GPU_STAT_NAMED(Stat_SW_Result, TEXT("SW_Result"));


#ifdef NUM_THREADS_PER_GROUP_DIMENSION_X
#undef NUM_THREADS_PER_GROUP_DIMENSION_X
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_X 16

#ifdef NUM_THREADS_PER_GROUP_DIMENSION_Y
#undef NUM_THREADS_PER_GROUP_DIMENSION_Y
#endif

#define NUM_THREADS_PER_GROUP_DIMENSION_Y 16



class FShallowWaterSim : public FGlobalShader
{
public:
		
	enum class EShallowWaterSimStep : uint8
	{
		SW_CompactActiveTiles,
		SW_FinalizeCompact,
		SW_VelocityHeightSim,
		SW_ShallowIntegrate,
		SW_Result,
		SW_SetHeight,
		MAX
	};
	class FShallowWaterSimStep : SHADER_PERMUTATION_ENUM_CLASS("SWS", EShallowWaterSimStep);
	class FSplineRange : SHADER_PERMUTATION_BOOL("USESPLINERANGE");
	class FWaterfallExtentPerm : SHADER_PERMUTATION_ENUM_CLASS("WATERFALL_PERM", EWaterfallExpansion);
	using FPermutationDomain = TShaderPermutationDomain<FShallowWaterSimStep, FSplineRange, FWaterfallExtentPerm>;

	static TShaderMapRef<FShallowWaterSim> CreatePermutation(EShallowWaterSimStep Permutation, bool UseSplineRange = false, EWaterfallExpansion WfExtent = EWaterfallExpansion::Expansion_5)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FSplineRange>(UseSplineRange);
		PermutationVector.Set<FShallowWaterSimStep>(Permutation);
		PermutationVector.Set<FWaterfallExtentPerm>(WfExtent);
		TShaderMapRef<FShallowWaterSim> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	DECLARE_GLOBAL_SHADER(FShallowWaterSim);
	SHADER_USE_PARAMETER_STRUCT(FShallowWaterSim, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_VelocityHeight)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_SplineScaleDist)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_CopyLandscape)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, T_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_VelHeightSimB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_DebugView)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultVelHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_SmoothHeightB)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_ResultSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_TileMask)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RWB_SourceUVRads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWB_CompactCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWB_CompactIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactTileCoords)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, B_CompactCounter)
		RDG_BUFFER_ACCESS(CompactIndirectArgs, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER(int, CloseBound)
		SHADER_PARAMETER(int, BCount_SourceUVRads)
		SHADER_PARAMETER(int, DispatchExpandPixels)

		SHADER_PARAMETER(FVector4f, SourceUVRad)
		SHADER_PARAMETER(FVector4f, ModifierUVRad)
		SHADER_PARAMETER(FVector2f, CopyValidUV)
		SHADER_PARAMETER(float, DT)
		SHADER_PARAMETER(float, Friction)
		SHADER_PARAMETER(float, SeaLevel)
		SHADER_PARAMETER(float, ActorLocationZ)
		SHADER_PARAMETER(float, AdvectFoam)
		SHADER_PARAMETER(float, FoamFadeSpeed)
		SHADER_PARAMETER(float, MaxWaterRisePerFrame)
		SHADER_PARAMETER(float, SpikeClampHeight)
		SHADER_PARAMETER_SAMPLER(SamplerState, Sampler)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const auto Step = PermutationVector.Get<FShallowWaterSimStep>();
		const auto WfPerm = PermutationVector.Get<FWaterfallExtentPerm>();
		if (Step != EShallowWaterSimStep::SW_VelocityHeightSim && WfPerm != EWaterfallExpansion::Expansion_5)
			return false;
		return true;
	}
	static inline void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), NUM_THREADS_PER_GROUP_DIMENSION_X);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), NUM_THREADS_PER_GROUP_DIMENSION_Y);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Z"), NUM_THREADS_PER_GROUP_DIMENSION_Z);
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("SW_COMPACTACTIVETILES"),
			TEXT("SW_FINALIZECOMPACT"),
			TEXT("SW_VELOCITYHEIGHTSIM"),
			TEXT("SW_SHALLOWINTEGRATE"),
			TEXT("SW_RESULT"),
			TEXT("SW_SETHEIGHT"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EShallowWaterSimStep::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FShallowWaterSimStep>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);

		OutEnvironment.SetDefine(TEXT("MAX_HEIGHT"), 10000);
		OutEnvironment.SetDefine(TEXT("VELOCITY_CLAMP"), CSSW_VELOCITY_CLAMP);
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_VelocityHeightSim)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalVelocityHeight_SmoothData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 0);
		}
		if (PermutationVector.Get<FShallowWaterSimStep>() == EShallowWaterSimStep::SW_Result)
		{
			OutEnvironment.SetDefine(TEXT("CREATE_SHARE_DATA_FUNC"), TEXT("CalShallowWaterResult_ShareData"));
			OutEnvironment.SetDefine(TEXT("GENERAL_SHAREGROUP_EXTENT"), 2);
		}
		if (PermutationVector.Get<FSplineRange>() == true)
		{
			OutEnvironment.SetDefine(TEXT("USESPLINERANGE"), 1);
		}

		constexpr int32 WaterfallExtentLUT[] = { 5, 7, 10 };
		const int32 WfIdx = FMath::Clamp(static_cast<int32>(PermutationVector.Get<FWaterfallExtentPerm>()), 0, 2);
		OutEnvironment.SetDefine(TEXT("WATERFALL_EXTENT"), WaterfallExtentLUT[WfIdx]);
	}
};

IMPLEMENT_GLOBAL_SHADER(FShallowWaterSim, "/Plugin/PCGPlugins/Shaders/Private/ShallowWater.usf", "ShallowWater", SF_Compute);

namespace
{
int32 ComputeDispatchExpandPixels(int32 Iteration, int32 TextureResolution)
{
	const int32 IterationMargin = FMath::Max(Iteration, 1) * 2;
	return FMath::Clamp(IterationMargin + 4, 4, FMath::Max(TextureResolution, 4));
}
}


using namespace CSHelper;
ACSShallowWaterCapture::ACSShallowWaterCapture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = false;
	PrimaryActorTick.bStartWithTickEnabled = false;

	GeneratorBounds->SetBoxExtent(FVector(50,50,50));
	ReusltMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("VisualizationMesh"));
	ReusltMesh->BoundsScale = 100;
	ReusltMesh->bVisibleInRayTracing = false;
	ReusltMesh->SetCastShadow(false);
	ReusltMesh->SetupAttachment(SceneRoot, TEXT("VisualizationMesh"));
	SimVisHISM = CreateDefaultSubobject<UHierarchicalInstancedStaticMeshComponent>(TEXT("SimVisHISM"));
	SimVisHISM->SetupAttachment(SceneRoot, TEXT("SimVisHISM"));
#if WITH_EDITOR
	SimVisHISM->SetIsVisualizationComponent(true);
#endif
	SimVisHISM->SetVisibility(false);
	SimVisHISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SimVisHISM->SetCastShadow(false);
	SimVisHISM->bVisibleInRayTracing = false;
	SimVisHISM->NumCustomDataFloats = 0;
	SimVisHISM->bNeverDistanceCull = true;
	SimVisHISM->bAllowCullDistanceVolume = false;
	CausticsDecal = CreateDefaultSubobject<UDecalComponent>(TEXT("CausticsDecal"));
	CausticsDecal->SetupAttachment(SceneRoot, TEXT("CausticsDecal"));
}

void ACSShallowWaterCapture::ClearSolverTimer()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SolverTimerHandle);
	}
	SolverTimerHandle.Invalidate();
}

void ACSShallowWaterCapture::ScheduleSolverTimerTick()
{
	if (IsActorBeingDestroyed())
	{
		ClearSolverTimer();
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		ClearSolverTimer();
		return;
	}

	const int32 ExpectedSolverGeneration = SolverGeneration;
	FTimerDelegate SolverTimerDelegate = FTimerDelegate::CreateWeakLambda(this, [this, ExpectedSolverGeneration]()
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_Total);
		if (ExpectedSolverGeneration != SolverGeneration || IsActorBeingDestroyed())
		{
			return;
		}

		ScheduleSolverTimerTick();
		ShallowWaterSolverSoucePoint(Iteration);
	});

	if (SolverTimerRate > 0.0f)
	{
		World->GetTimerManager().SetTimer(SolverTimerHandle, SolverTimerDelegate, SolverTimerRate, false);
	}
	else
	{
		SolverTimerHandle = World->GetTimerManager().SetTimerForNextTick(SolverTimerDelegate);
	}
}

void ACSShallowWaterCapture::StopSimulationRuntime()
{
	ClearSolverTimer();
	ResetSolverState(true);
}

void ACSShallowWaterCapture::ResetSolverState(bool bAdvanceGeneration)
{
	if (bAdvanceGeneration)
	{
		SolverGeneration++;
		if (SolverGeneration <= 0) SolverGeneration = 1;
	}
	LastSolverFrameNumber = 0;
}

void ACSShallowWaterCapture::Clean()
{
	ClearSolverTimer();
	ResetSolverState(true);
	// 生成出来的代理 actor 是这次仿真的产物，清理时一并收走，否则关卡里会留下孤立的
	// StaticMeshActor/DecalActor（本体组件已隐藏，视觉上还以为水体没清干净）。
	if (const int32 Removed = DestroyAttachedProxyActors(AutoProxyTag))
	{
		UE_LOG(LogTemp, Log, TEXT("[CSSW] Clean removed %d attached '%s' proxy actor(s) on %s"),
			Removed, *AutoProxyTag.ToString(), *GetName());
	}
	if (!CleanRenderTargets())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Clean failed on %s: render targets could not be initialized."), *GetName());
	}
}

bool ACSShallowWaterCapture::CleanRenderTargets()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return false;

	const int32 SafeTextureSize = FMath::Max(16, TextureSize);
	const FLinearColor DebugViewClearColor = FLinearColor::Black;
	const FLinearColor VelocityHeightClearColor(0, 0, 0, 0);
	const FLinearColor ResultVelHeightClearColor(0, 0, -9999, 0);
	const FLinearColor ResultDepthWetClearColor(-9999, -9999, -9999, -9999);
	const FLinearColor SmoothHeightClearColor(0, 0, 0, 0);
	const FLinearColor SceneDepthClearColor(GetActorLocation().Z + MaxHeight + 9000.0f, 0, 0, 0);

	if (!RT_DebugView || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_ResultDepthWet || !RT_SmoothHeight)
	{
		return false;
	}

	RT_DebugView->ClearColor = DebugViewClearColor;
	RT_VelocityHeight->ClearColor = VelocityHeightClearColor;
	RT_ResultVelHeight->ClearColor = ResultVelHeightClearColor;
	RT_ResultDepthWet->ClearColor = ResultDepthWetClearColor;
	RT_SmoothHeight->ClearColor = SmoothHeightClearColor;

	RT_DebugView->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_VelocityHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_ResultVelHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_ResultDepthWet->ResizeTarget(SafeTextureSize, SafeTextureSize);
	RT_SmoothHeight->ResizeTarget(SafeTextureSize, SafeTextureSize);

	if (!RT_SceneDepth)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(
			this,
			SafeTextureSize,
			SafeTextureSize,
			ETextureRenderTargetFormat::RTF_RGBA32f,
			SceneDepthClearColor,
			true,
			false);
		if (!RT_SceneDepth) return false;
	}
	RT_SceneDepth->ClearColor = SceneDepthClearColor;
	RT_SceneDepth->ResizeTarget(SafeTextureSize, SafeTextureSize);

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);
	if (!RT_TileMask) return false;

	auto ClearRenderTarget = [this](UTextureRenderTarget2D* RenderTarget, const FLinearColor& ClearColor)
	{
		if (RenderTarget)
		{
			UKismetRenderingLibrary::ClearRenderTarget2D(this, RenderTarget, ClearColor);
		}
	};

	ClearRenderTarget(RT_DebugView, DebugViewClearColor);
	ClearRenderTarget(RT_VelocityHeight, VelocityHeightClearColor);
	ClearRenderTarget(RT_ResultVelHeight, ResultVelHeightClearColor);
	ClearRenderTarget(RT_ResultDepthWet, ResultDepthWetClearColor);
	ClearRenderTarget(RT_SmoothHeight, SmoothHeightClearColor);
	ClearRenderTarget(RT_SceneDepth, SceneDepthClearColor);
	ClearRenderTarget(RT_TileMask, FLinearColor::Black);

	return true;
}

void ACSShallowWaterCapture::PostLoad()
{
	Super::PostLoad();

	StopSimulationRuntime();

}

void ACSShallowWaterCapture::BeginPlay()
{
	Super::BeginPlay();

	StopSimulationRuntime();
	SetActorTickEnabled(false);
}

void ACSShallowWaterCapture::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ConstructionDebounceHandle);
		World->GetTimerManager().SetTimer(ConstructionDebounceHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]() { ConstructionComponent(); }),
			0.01f, false);
	}
	else
	{
		ConstructionComponent();
	}
	// FVector CLocation = CausticsDecal->GetComponentLocation();
	// CausticsDecal->SetWorldLocation(FVector(CLocation.X, CLocation.Y, 0));
}


void ACSShallowWaterCapture::ConstructionComponent()
{
	bool bRestartSolverAfterConstruction = SolverTimerHandle.IsValid();
	if (UWorld* World = GetWorld())
	{
		bRestartSolverAfterConstruction = World->GetTimerManager().TimerExists(SolverTimerHandle);
	}
	if (bRestartSolverAfterConstruction)
	{
		ClearSolverTimer();
	}

	RefreshConstructionLayout();

	if (bRestartSolverAfterConstruction)
	{
		CaptureAll();
		ScheduleSolverTimerTick();
	}
	else
	{
		ClearSolverTimer();
	}
}

void ACSShallowWaterCapture::RefreshConstructionLayout()
{
	FVector RelativeScale = FVector(CaptureSize / 100, CaptureSize / 100, MaxHeight / 100);
	GeneratorBounds->SetRelativeScale3D(RelativeScale);
	GeneratorBounds->SetRelativeLocation(FVector(0, 0, Scale3DZ * 50));

	FVector DecalRelativeScale = FVector(MaxHeight / 100, CaptureSize / 100, CaptureSize / 100);
	CausticsDecal->SetRelativeScale3D(DecalRelativeScale);
	CausticsDecal->SetRelativeRotation(FRotator(-90, 0, 0));
	CausticsDecal->DecalSize = FVector(500, 50, 50);

	TextureSize = FMath::RoundUpToPowerOfTwo(
		FMath::Max(16, FMath::CeilToInt32(CaptureSize / FMath::Max(WorldPixelSize, 1.0f))));

	UpdateSimUV();
	SetActorScale3D(FVector::OneVector);
}

void ACSShallowWaterCapture::UpdateSimUV()
{
	const FVector Loc = GetActorLocation();
	SimUVCenter = FVector2D(Loc.X, Loc.Y);
	SimUVSize = CaptureSize;
	SimUVInvSize = CaptureSize > 0.f ? 1.f / CaptureSize : 0.f;
}

void ACSShallowWaterCapture::EnsureTileMask(int32 Width, int32 Height)
{
	if (!RT_TileMask)
	{
		RT_TileMask = NewObject<UTextureRenderTarget2D>(this);
	}

	const bool bNeedsRebuild =
		RT_TileMask->SizeX != Width ||
		RT_TileMask->SizeY != Height ||
		RT_TileMask->RenderTargetFormat != ETextureRenderTargetFormat::RTF_RGBA16f ||
		!RT_TileMask->bCanCreateUAV;

	RT_TileMask->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	RT_TileMask->bCanCreateUAV = true;
	RT_TileMask->ClearColor = FLinearColor::Black;

	if (bNeedsRebuild)
	{
		RT_TileMask->InitAutoFormat(Width, Height);
		RT_TileMask->UpdateResourceImmediate(true);
	}
}

void ACSShallowWaterCapture::EnsureRTSizes()
{
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_VelocityHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultVelHeight->ResizeTarget(TextureSize, TextureSize);
	RT_ResultDepthWet->ResizeTarget(TextureSize, TextureSize);
	RT_SmoothHeight->ResizeTarget(TextureSize, TextureSize);
}

void ACSShallowWaterCapture::RebuildSimulationVisualization()
{
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_BuildHISM);
		if (SimVisHISM)
		{
			UStaticMesh* PreviewMesh = SimulationPreviewMesh;
			if (!PreviewMesh && ReusltMesh) PreviewMesh = ReusltMesh->GetStaticMesh();
			if (!PreviewMesh && DebugMesh) PreviewMesh = DebugMesh;
			if (PreviewMesh && SimVisHISM->GetStaticMesh() != PreviewMesh)
			{
				SimVisHISM->SetStaticMesh(PreviewMesh);
			}
			if (!SimVisHISM->GetStaticMesh())
			{
				SimVisHISM->ClearInstances();
				return;
			}
			if (WaterMaterial)
			{
				SimVisHISM->SetMaterial(0, WaterMaterial);
			}

			constexpr int32 SimVisSubsampleFactor = 2;
			const int32 SafeTextureSize = FMath::Max(16, TextureSize);
			const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
			const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
			const int32 GridCountX = FMath::DivideAndRoundUp(TileMaskWidth, SimVisSubsampleFactor);
			const int32 GridCountY = FMath::DivideAndRoundUp(TileMaskHeight, SimVisSubsampleFactor);
			const int32 TotalSlots = GridCountX * GridCountY;
			const float TileWorldSize = CaptureSize / (float)GridCountX;
			const float HalfCapture = CaptureSize * 0.5f;
			const FVector TileScale(TileWorldSize / 100.0f, TileWorldSize / 100.0f, 1.0f);

			SimVisHISM->ClearInstances();

			TArray<FTransform> Transforms;
			Transforms.Reserve(TotalSlots);
			for (int32 Slot = 0; Slot < TotalSlots; Slot++)
			{
				const int32 TX = Slot % GridCountX;
				const int32 TY = Slot / GridCountX;
				const float CenterX = (TX + 0.5f) * TileWorldSize - HalfCapture;
				const float CenterY = (TY + 0.5f) * TileWorldSize - HalfCapture;
				Transforms.Emplace(FQuat::Identity, FVector(CenterX, CenterY, 0.0f), TileScale);
			}

			SimVisHISM->AddInstances(Transforms, false, false);
			SimVisHISM->BuildTreeIfOutdated(false, true);
			SimVisHISM->MarkRenderStateDirty();
		}
	}
}

void ACSShallowWaterCapture::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	StopSimulationRuntime();
	Super::PreSave(ObjectSaveContext);
}

void ACSShallowWaterCapture::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopSimulationRuntime();
	Super::EndPlay(EndPlayReason);
}

void ACSShallowWaterCapture::ShallowWaterSolverSoucePoint(int32 InIteration)
{
	if (GFrameCounter == LastSolverFrameNumber) return;
	LastSolverFrameNumber = GFrameCounter;

	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);

	UpdateSimUV();

	TArray<FVector4> SourceData;
	{
		SCOPE_CYCLE_COUNTER(STAT_CSSW_CollectSources);
		UWorld* World = GetWorld();
		if (!World)
		{
			return;
		}

		const FBox SourceBounds = FBox::BuildAABB(
			GeneratorBounds->Bounds.Origin,
			GeneratorBounds->Bounds.BoxExtent * FVector(1.2, 1.2, 9999));

		for (TActorIterator<ACSSHallowWaterSource> It(World, ACSSHallowWaterSource::StaticClass()); It; ++It)
		{
			ACSSHallowWaterSource* Actor = *It;
			const FVector Location = Actor->GetActorLocation();
			if (!SourceBounds.IsInsideOrOn(Location))
			{
				continue;
			}

			SourceData.Add(FVector4(Location.X, Location.Y, Location.Z, Actor->GetActorScale3D().X / CaptureSize * 500));
		}
	}

	TArray<FVector4f> SourceUVRads;
	FBoxSphereBounds Bounds = GeneratorBounds->Bounds;
	FVector ActorLocation = GetActorLocation();
	SourceUVRads.Reserve(SourceData.Num());
	for (int i = 0; i < SourceData.Num(); i++)
	{
		FVector4 PerSourceData = SourceData[i];
		FVector SourceLocation = FVector(PerSourceData.X, PerSourceData.Y, PerSourceData.Z);
		FVector RelativeSourceLocation = SourceLocation - (Bounds.Origin - Bounds.BoxExtent);
		FVector SourceUV = RelativeSourceLocation / ( Bounds.BoxExtent * 2 );
		FVector4f SourceUVRad = FVector4f( SourceUV.X, SourceUV.Y, SourceLocation.Z, PerSourceData.W);
		SourceUVRads.Add(SourceUVRad);
	}
	
	if (!RT_SceneDepth)
	{
		CaptureAll();
		if (!RT_SceneDepth)
		{
			UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null after CaptureAll — cannot run solver"));
			return;
		}
	}
	EnsureRTSizes();

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(TextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(TextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);
	FTextureRenderTargetResource* R_TileMask = RT_TileMask->GameThread_GetRenderTargetResource();

	InIteration = FMath::Max(InIteration, 1);
	const EWaterfallExpansion CapturedWaterfallExpansionIterations = WaterfallExpansionIterations;
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedAdvectFoam = AdvectFoam;
	const float CapturedFoamFadeSpeed = FoamFadeSpeed;
	const float CapturedMaxWaterRisePerFrame = MaxWaterRisePerFrame;
	const float CapturedSpikeClampHeight = SpikeClampHeight;
	const int32 CapturedCloseBound = CloseBound;
	const int32 CapturedInIteration = InIteration;
	const int32 CapturedSourceCount = SourceUVRads.Num();
	// Restored water must keep evolving without an emitter; bind a dummy SRV for an empty source list.
	if (SourceUVRads.IsEmpty()) SourceUVRads.Add(FVector4f(0, 0, 0, 0));

	SCOPE_CYCLE_COUNTER(STAT_CSSW_EnqueueRender);
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight, R_TileMask,
	 SourceUVRads = MoveTemp(SourceUVRads), CapturedWaterfallExpansionIterations,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, ActorLocation, CapturedAdvectFoam, CapturedFoamFadeSpeed,
	 CapturedMaxWaterRisePerFrame, CapturedSpikeClampHeight, CapturedCloseBound, CapturedInIteration, TileMaskWidth, TileMaskHeight, CapturedSourceCount
	 ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_ShallowWater);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;

			TShaderMapRef<FShallowWaterSim> ComputeShader_CompactActiveTiles = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_CompactActiveTiles);
			TShaderMapRef<FShallowWaterSim> ComputeShader_FinalizeCompact = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_FinalizeCompact);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalVelocityHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_VelocityHeightSim, false, CapturedWaterfallExpansionIterations);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalShallowIntegrate = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_ShallowIntegrate);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			const FIntVector SingleGroupCount(1, 1, 1);
			
			CREATE_TEXTURE_UAV_16_OUT(DebugView)
			CREATE_TEXTURE_UAV_16_OUT(ResultVelHeight)
			CREATE_TEXTURE_UAV_16_OUTP(ResultDepthWet)
			
			CREATE_TEXTURE_UAV_32_OUT(ResultSmoothHeight)
			
			CREATE_TEXTURE_UAV_32(VelHeightSimA)
			CREATE_TEXTURE_UAV_32(VelHeightSimB)
			CREATE_TEXTURE_UAV_32(SmoothHeightA)
			CREATE_TEXTURE_UAV_32(SmoothHeightB)
			
			CREATE_RDG(SceneDepth)
			CREATE_RDG(VelocityHeight)
			
			CREATE_RDG_STRUCTURED_UPLOAD_SRV(SourceUVRads, FVector4f, SourceUVRads, TEXT("SourceUVRads"))

			FCompactTileBuffers CompactBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);
			FCompactTileBuffers VisualTileMaskBuffers = CreateCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

			FRDGTextureRef TRDG_TileMask = nullptr;
			FRDGTextureUAVRef RDGUAV_TileMask = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TRDG_TileMask, RDGUAV_TileMask, FIntPoint(TileMaskWidth, TileMaskHeight), PF_FloatRGBA, TEXT("UAV_TileMask"), FLinearColor::Black);
			FRDGTextureRef RDG_TileMask = RegisterExternalTexture(GraphBuilder, R_TileMask->GetRenderTargetTexture(), TEXT("TileMask_RT"));

			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = ActorLocation.Z;
			PassParameters->AdvectFoam = CapturedAdvectFoam;
			PassParameters->FoamFadeSpeed = CapturedFoamFadeSpeed;
			PassParameters->MaxWaterRisePerFrame = CapturedMaxWaterRisePerFrame;
			PassParameters->SpikeClampHeight = CapturedSpikeClampHeight;
			PassParameters->CloseBound = CapturedCloseBound;
			PassParameters->BCount_SourceUVRads = CapturedSourceCount;
			PassParameters->RWB_SourceUVRads = SourceUVRadsSRV;
			PassParameters->DispatchExpandPixels = ComputeDispatchExpandPixels(CapturedInIteration, TextureSize.X);
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_VelocityHeight, TRDG_VelHeightSimA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, TRDG_SmoothHeightA, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultVelHeight, TRDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_ResultDepthWet, TRDG_ResultDepthWet, FRHICopyTextureInfo());

			const FIntVector FullTileGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FIntVector(NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y, 1));
			{
				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Compact);
				ResetCompactCounter(GraphBuilder, CompactBuffers);

				FShallowWaterSim::FParameters* CompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*CompactPassParameters = *PassParameters;
				CompactPassParameters->B_CompactTileCoords = nullptr;
				CompactPassParameters->B_CompactCounter = nullptr;
				CompactPassParameters->CompactIndirectArgs = nullptr;
				CompactPassParameters->RWB_CompactIndirectArgs = nullptr;
				CompactPassParameters->RW_TileMask = RDGUAV_TileMask;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CompactActiveTiles"),
					ComputePassFlags,
					ComputeShader_CompactActiveTiles,
					CompactPassParameters,
					FullTileGroupCount);

				FShallowWaterSim::FParameters* FinalizeCompactPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*FinalizeCompactPassParameters = *PassParameters;
				NullifyAllCompactTileBindings(FinalizeCompactPassParameters);
				FinalizeCompactPassParameters->RWB_CompactCounter = CompactBuffers.CounterUAV;
				FinalizeCompactPassParameters->RWB_CompactIndirectArgs = CompactBuffers.IndirectArgsUAV;
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("FinalizeCompact"),
					ComputePassFlags,
					ComputeShader_FinalizeCompact,
					FinalizeCompactPassParameters,
					SingleGroupCount);
			}


			FRDGTextureRef CurrentVelHeightTextureA = TRDG_VelHeightSimA;
			FRDGTextureRef CurrentVelHeightTextureB = TRDG_VelHeightSimB;
			FRDGTextureRef CurrentSmoothHeightTextureA = TRDG_SmoothHeightA;
			FRDGTextureRef CurrentSmoothHeightTextureB = TRDG_SmoothHeightB;
			FRDGTextureUAVRef CurrentVelHeightSimA = RDGUAV_VelHeightSimA;
			FRDGTextureUAVRef CurrentVelHeightSimB = RDGUAV_VelHeightSimB;
			FRDGTextureUAVRef CurrentSmoothHeightA = RDGUAV_SmoothHeightA;
			FRDGTextureUAVRef CurrentSmoothHeightB = RDGUAV_SmoothHeightB;

			for (int32 i = 0 ; i < CapturedInIteration; i++)
			{
				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_VelHeight);
				FShallowWaterSim::FParameters* VelocityHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*VelocityHeightPassParameters = *PassParameters;
				VelocityHeightPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				VelocityHeightPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				VelocityHeightPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				VelocityHeightPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(VelocityHeightPassParameters);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CalVelocityHeight"),
					ComputePassFlags,
					ComputeShader_CalVelocityHeight,
					VelocityHeightPassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentSmoothHeightTextureA, CurrentSmoothHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
				Swap(CurrentSmoothHeightA, CurrentSmoothHeightB);

				RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Integrate);
				FShallowWaterSim::FParameters* ShallowIntegratePassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
				*ShallowIntegratePassParameters = *PassParameters;
				ShallowIntegratePassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
				ShallowIntegratePassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
				ShallowIntegratePassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
				ShallowIntegratePassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
				NullifyCompactTileUAVs(ShallowIntegratePassParameters);
				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("CalShallowIntegrate"),
					ComputePassFlags,
					ComputeShader_CalShallowIntegrate,
					ShallowIntegratePassParameters,
					CompactBuffers.IndirectArgsBuffer,
					0);

				Swap(CurrentVelHeightTextureA, CurrentVelHeightTextureB);
				Swap(CurrentVelHeightSimA, CurrentVelHeightSimB);
			}

			RDG_GPU_STAT_SCOPE(GraphBuilder, Stat_SW_Result);
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			ResultPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			ResultPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			ResultPassParameters->RW_SmoothHeightA = CurrentSmoothHeightA;
			ResultPassParameters->RW_SmoothHeightB = CurrentSmoothHeightB;
			NullifyCompactTileUAVs(ResultPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Result"),
				ComputePassFlags,
				ComputeShader_CalResult,
				ResultPassParameters,
				CompactBuffers.IndirectArgsBuffer,
				0);

			ResetCompactCounter(GraphBuilder, VisualTileMaskBuffers);
			FShallowWaterSim::FParameters* VisualTileMaskPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*VisualTileMaskPassParameters = *PassParameters;
			BindCompactTileBuffers(VisualTileMaskPassParameters, VisualTileMaskBuffers);
			VisualTileMaskPassParameters->B_CompactTileCoords = nullptr;
			VisualTileMaskPassParameters->B_CompactCounter = nullptr;
			VisualTileMaskPassParameters->CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->RWB_CompactIndirectArgs = nullptr;
			VisualTileMaskPassParameters->T_ResultDepthWet = TRDG_ResultDepthWet;
			VisualTileMaskPassParameters->RW_VelHeightSimA = CurrentVelHeightSimA;
			VisualTileMaskPassParameters->RW_VelHeightSimB = CurrentVelHeightSimB;
			VisualTileMaskPassParameters->RW_TileMask = RDGUAV_TileMask;
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("CompactActiveTiles_SimVis"),
				ComputePassFlags,
				ComputeShader_CompactActiveTiles,
				VisualTileMaskPassParameters,
				FullTileGroupCount);
			
			AddCopyTexturePass(GraphBuilder, TRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultVelHeight, RDG_ResultVelHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, CurrentVelHeightTextureA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TRDG_TileMask, RDG_TileMask, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
}


void ACSShallowWaterCapture::SetHeight()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;
	if (!RT_SceneDepth) { UE_LOG(LogTemp, Error, TEXT("[CSSW] RT_SceneDepth is null in SetHeight")); return; }
	SCOPE_CYCLE_COUNTER(STAT_CSSW_Execute);
	
	EnsureRTSizes();

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_VelocityHeight = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultVelHeight = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultDepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ResultSmoothHeight = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	const float CapturedDT = DT;
	const float CapturedFriction = Friction;
	const float CapturedSeaLevel = SeaLevel;
	const float CapturedActorLocationZ = GetActorLocation().Z;

	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[R_SceneDepth, R_DebugView, R_VelocityHeight, R_ResultVelHeight, R_ResultDepthWet, R_ResultSmoothHeight,
	 CapturedDT, CapturedFriction, CapturedSeaLevel, CapturedActorLocationZ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_SceneDepth->GetSizeXY().X;
			float SizeY = R_SceneDepth->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);
			const ERDGPassFlags ComputePassFlags = ERDGPassFlags::Compute;
			TShaderMapRef<FShallowWaterSim> ComputeShader_SetHeight = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_SetHeight);
			TShaderMapRef<FShallowWaterSim> ComputeShader_CalResult = FShallowWaterSim::CreatePermutation(FShallowWaterSim::EShallowWaterSimStep::SW_Result);
			
			FShallowWaterSim::FParameters* PassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("UAV_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			FRDGTextureRef RDG_VelHeightSimA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Sim_A"));
			FRDGTextureUAVRef RDGUAV_VelHeightSimA = GraphBuilder.CreateUAV(RDG_VelHeightSimA);
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_Result"));
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);
			FRDGTextureRef TmpRDG_ResultDepthWet = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_FloatRGBA, TEXT("UAV_ResultDepthWet"));
			FRDGTextureUAVRef RDGUAV_ResultDepthWet = GraphBuilder.CreateUAV(TmpRDG_ResultDepthWet);
			FRDGTextureRef TmpRDG_ResultSmoothHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_ResultSmoothHeight"));
			FRDGTextureUAVRef RDGUAV_ResultSmoothHeight = GraphBuilder.CreateUAV(TmpRDG_ResultSmoothHeight);
			FRDGTextureRef RDG_SmoothHeightA = ConvertToUVATextureFormat(GraphBuilder, TextureSize, PF_A32B32G32R32F, TEXT("UAV_SmoothHeightA"));
			FRDGTextureUAVRef RDGUAV_SmoothHeightA = GraphBuilder.CreateUAV(RDG_SmoothHeightA);
			
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("SceneDepth_RT"));
			FRDGTextureRef RDG_VelocityHeight = RegisterExternalTexture(GraphBuilder, R_VelocityHeight->GetRenderTargetTexture(), TEXT("VelocityHeight_RT"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_ResultVelHeight->GetRenderTargetTexture(), TEXT("Result_RT"));
			FRDGTextureRef RDG_ResultDepthWet = RegisterExternalTexture(GraphBuilder, R_ResultDepthWet->GetRenderTargetTexture(), TEXT("ResultDepthWet_RT"));
			FRDGTextureRef RDG_ResultSmoothHeight = RegisterExternalTexture(GraphBuilder, R_ResultSmoothHeight->GetRenderTargetTexture(), TEXT("ResultSmoothHeight_RT"));

			FCompactTileBuffers CompactBuffers = CreateFullScreenCompactTileBuffers(
				GraphBuilder, (uint32)SizeX, (uint32)SizeY,
				NUM_THREADS_PER_GROUP_DIMENSION_X, NUM_THREADS_PER_GROUP_DIMENSION_Y);

			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_VelocityHeight = RDG_VelocityHeight;
			PassParameters->T_ResultDepthWet = RDG_ResultDepthWet;
			PassParameters->T_ResultSmoothHeight = RDG_ResultSmoothHeight;
			PassParameters->DT = CapturedDT;
			PassParameters->Friction = CapturedFriction;
			PassParameters->SeaLevel = CapturedSeaLevel;
			PassParameters->ActorLocationZ = CapturedActorLocationZ;
			PassParameters->MaxWaterRisePerFrame = 1e10f;
			PassParameters->SpikeClampHeight = 0.0f;
			PassParameters->BCount_SourceUVRads = 0;
			PassParameters->DispatchExpandPixels = 0;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_ResultVelHeight = RDGUAV_Result;
			PassParameters->RW_ResultDepthWet = RDGUAV_ResultDepthWet;
			PassParameters->RW_ResultSmoothHeight = RDGUAV_ResultSmoothHeight;
			PassParameters->RW_VelHeightSimA = RDGUAV_VelHeightSimA;
			PassParameters->RW_SmoothHeightA = RDGUAV_SmoothHeightA;
			PassParameters->RWB_SourceUVRads = nullptr;
			BindCompactTileBuffers(PassParameters, CompactBuffers);
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			AddCopyTexturePass(GraphBuilder, RDG_ResultSmoothHeight, RDG_SmoothHeightA, FRHICopyTextureInfo());

			FShallowWaterSim::FParameters* SetHeightPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*SetHeightPassParameters = *PassParameters;
			NullifyAllCompactTileBindings(SetHeightPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("SetHeight"),
				ComputePassFlags,
				ComputeShader_SetHeight,
				SetHeightPassParameters,
				GroupCount);
			
			// Result pass — uses CompactIndirectArgs, only needs SRV reads
			FShallowWaterSim::FParameters* ResultPassParameters = GraphBuilder.AllocParameters<FShallowWaterSim::FParameters>();
			*ResultPassParameters = *PassParameters;
			NullifyCompactTileUAVs(ResultPassParameters);
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("Result"),
				ComputePassFlags,
				ComputeShader_CalResult,
				ResultPassParameters,
				CompactBuffers.IndirectArgsBuffer,
				0);
			AddCopyTexturePass(GraphBuilder, TmpRDG_Result, RDG_Result, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultDepthWet, RDG_ResultDepthWet, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultSmoothHeight, RDG_ResultSmoothHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, RDG_VelHeightSimA, RDG_VelocityHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
}


void ACSShallowWaterCapture::SetMaterialParameter_Implementation()
{
	SCOPE_CYCLE_COUNTER(STAT_CSSW_SetMaterial);

	auto ApplySimParams = [this](UMaterialInstanceDynamic* MID)
	{
		if (!MID) return;
		MID->SetVectorParameterValue(FName("CSSW_SimCenter"), FLinearColor(SimUVCenter.X, SimUVCenter.Y, 0, 0));
		MID->SetScalarParameterValue(FName("CSSW_SimInvSize"), SimUVInvSize);
		if (RT_ResultVelHeight) MID->SetTextureParameterValue(FName("CSSW_VelHeight"), RT_ResultVelHeight);
		if (RT_ResultDepthWet) MID->SetTextureParameterValue(FName("CSSW_DepthWet"), RT_ResultDepthWet);
	};

	auto EnsureMID = [this](UPrimitiveComponent* Comp, int32 SlotIndex, UMaterialInterface* ParentMat) -> UMaterialInstanceDynamic*
	{
		if (!Comp || !ParentMat) return nullptr;
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Comp->GetMaterial(SlotIndex));
		if (!MID || MID->Parent != ParentMat)
		{
			MID = UMaterialInstanceDynamic::Create(ParentMat, this);
			Comp->SetMaterial(SlotIndex, MID);
		}
		return MID;
	};

	// 展示烘焙结果时不铺 MID：烘焙 MIC 指向落盘贴图，MID 只绑 transient render target，
	// 一旦覆盖上去，关卡重开或 RT 释放后水面与焦散就变成空白。本函数会被 CaptureAll /
	// 求解 / Clean 间接调用，所以这道判断必须在这里，而不是只在展示时设一次。
	if (bUseBakedResultMesh && (BakedWaterMaterial || BakedDecalMaterial))
	{
		ApplyBakedMaterials();
		return;
	}

	ApplySimParams(EnsureMID(ReusltMesh, 0, WaterMaterial));
	ApplySimParams(EnsureMID(SimVisHISM, 0, WaterMaterial));

	if (CausticsDecal && DecalMaterial)
	{
		UMaterialInstanceDynamic* DecalMID = Cast<UMaterialInstanceDynamic>(CausticsDecal->GetDecalMaterial());
		if (!DecalMID || DecalMID->Parent != DecalMaterial)
		{
			DecalMID = UMaterialInstanceDynamic::Create(DecalMaterial, this);
			CausticsDecal->SetDecalMaterial(DecalMID);
		}
		ApplySimParams(DecalMID);
	}
}

void ACSShallowWaterCapture::ApplyBakedMaterials()
{
	if (BakedWaterMaterial)
	{
		if (ReusltMesh && ReusltMesh->GetMaterial(0) != BakedWaterMaterial) ReusltMesh->SetMaterial(0, BakedWaterMaterial);
		if (SimVisHISM && SimVisHISM->GetMaterial(0) != BakedWaterMaterial) SimVisHISM->SetMaterial(0, BakedWaterMaterial);
	}
	if (BakedDecalMaterial && CausticsDecal && CausticsDecal->GetDecalMaterial() != BakedDecalMaterial)
	{
		CausticsDecal->SetDecalMaterial(BakedDecalMaterial);
	}
}

void ACSShallowWaterCapture::CaptureAll()
{
	if (!CheckAndCreateTexture_SWSourcePoint()) return;

	const int32 SafeTextureSize = FMath::Max(16, TextureSize);
	// Alpha stays zero when nothing is captured; the heightmap rasterizer writes one.
	const FLinearColor SceneDepthClearColor(GetActorLocation().Z + MaxHeight + 9000.0f, 0, 0, 0);

	if (!RT_SceneDepth)
	{
		RT_SceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(
			this, SafeTextureSize, SafeTextureSize,
			ETextureRenderTargetFormat::RTF_RGBA32f,
			SceneDepthClearColor, true, false);
		if (!RT_SceneDepth) return;
	}
	RT_SceneDepth->ClearColor = SceneDepthClearColor;

	const int32 TileMaskWidth = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_X);
	const int32 TileMaskHeight = FMath::DivideAndRoundUp(SafeTextureSize, (int32)NUM_THREADS_PER_GROUP_DIMENSION_Y);
	EnsureTileMask(TileMaskWidth, TileMaskHeight);

	EnsureRTSizes();
	UKismetRenderingLibrary::ClearRenderTarget2D(this, RT_SceneDepth, SceneDepthClearColor);

	SCOPE_CYCLE_COUNTER(STAT_CSSW_Capture);
	UWorld* World = GetWorld();
	if (!World) return;

	FBox QueryBox = GetGeneratorBoundsWorldBox();
	if (!QueryBox.IsValid) return;

	const float ActorZ = GetActorLocation().Z;
	QueryBox.Min.Z = FMath::Min(QueryBox.Min.Z, ActorZ - MaxHeight);
	QueryBox.Max.Z = FMath::Max(QueryBox.Max.Z, ActorZ + MaxHeight);

	// 走公用的无状态收集器；actor 只交出自身的排除策略、LOD 与三角上限，
	// 「只收带 SWCaptureTag 的道具」是本次捕获的策略，故在这里显式给出。
	// 深度捕获只需要位置：不做 MeshDescription 全精度 CPU 提取（道具交给下面的渲染器拍，
	// 关掉 bCaptureWithRenderer 时整批退回三角形光栅化），地形不走 CPU 逐 quad 提取而是
	// 下面的 GPU RenderHeightmap——这两处正是旧路径的内存大头。
	FCSBoxSceneCollectOptions CollectOptions = MakeBoxSceneCollectOptions(QueryBox);
	if (!SWCaptureTag.IsNone()) CollectOptions.RequiredActorTags = { SWCaptureTag };
	CollectOptions.bIncludeLandscape = false;
	CollectOptions.bUseMeshDescriptionSourceTriangles = false;
	const bool bRenderProps = bCaptureWithRenderer && CSNaniteHeightCapture::IsAvailable(World);
	if (bRenderProps)
	{
		// 分流看的是组件当前的场景代理：刚改过可见性 / 网格的组件要先把帧末更新推下去，
		// 否则会按旧代理分流（渲染器随后在 CaptureScene 里反正也要推这一次）。
		World->SendAllEndOfFrameUpdates();
		// 单腿化：所有拍得到的道具都走引擎 depth pass，透明材质不写深度、Masked 出剪影。
		// 拍不到的（隐藏、无代理）由收集器留在下面的三角形路径上。
		CollectOptions.bCollectAllRenderComponents = true;
	}
	FCSBoxScenePreparedData Prepared = CSBoxSceneCollection::CollectBoxSceneTriangles(World, CollectOptions);

	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	const float CapturedCameraHeight = ActorZ + MaxHeight;
	const FBox CapturedBounds = QueryBox;

	// 渲染器拍不到的那些带 tag 的 static mesh：index/position buffer 直读的融合光栅化（不落
	// triangle soup），min 合并进 RT_SceneDepth，空 texel 保留清屏哨兵。这条路不看材质，
	// 所以只该剩下渲染器够不着的组件（或整个关掉 bCaptureWithRenderer 时的全部道具）。
	if (Prepared.IsValid() && !Prepared.Impl->ResolvedRequests.IsEmpty())
	{
		ENQUEUE_RENDER_COMMAND(CaptureAllSceneDepth)(
		[R_SceneDepth, Prepared, CapturedCameraHeight, CapturedBounds](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_SceneDepth = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(R_SceneDepth->GetRenderTargetTexture(), TEXT("CSSW.RT_SceneDepth")));

			CSMeshGenInternal::RasterizeResolvedMeshesToHeightmapRDG(
				GraphBuilder, RHICmdList, Prepared.Impl->ResolvedRequests, RDG_SceneDepth,
				CapturedBounds, CapturedCameraHeight);

			GraphBuilder.Execute();
		});
	}

	// 道具主路：渲染器俯视正交拍深度（与画面一致的全精度，含 WPO / masked / 透明不写深度），同样 min 合并。
	// 渲染在这里立刻入队，与上面的光栅化、下面的地形合并及随后的求解保持命令队列顺序。
	if (bRenderProps && Prepared.IsValid() && !Prepared.Impl->NaniteRenderComponents.IsEmpty())
	{
		CSNaniteHeightCapture::FHeightmapRequest NaniteRequest;
		NaniteRequest.Components = Prepared.Impl->NaniteRenderComponents;
		NaniteRequest.Heightmap = RT_SceneDepth;
		NaniteRequest.WorldBounds = CapturedBounds;
		NaniteRequest.CameraHeight = CapturedCameraHeight;
		if (!CSNaniteHeightCapture::CaptureIntoHeightmap(World, NaniteRequest)) UE_LOG(LogTemp, Warning, TEXT("[CSSW] %s: renderer capture failed, %d component(s) are missing from RT_SceneDepth"), *GetName(), NaniteRequest.Components.Num());
	}

	// 地形：ALandscape::RenderHeightmap 在 GPU 上出 G16 高度图，LandscapeG16ToDepthCS 按 min
	// 合并进 RT_SceneDepth（更高的地面赢，与网格光栅化可任意排序）。TempRT 清成全 0
	//（raw==0 表示"无地形"），RenderHeightmap 对未覆盖/未加载的 texel 不写入，合并 shader
	// 据此跳过，不会在网格深度或清屏哨兵上盖出幽灵地板。
	const FVector CaptureCenter = CapturedBounds.GetCenter();
	const FVector CaptureExtent = CapturedBounds.GetExtent();
	for (TActorIterator<ALandscape> It(World); It; ++It)
	{
		ALandscape* Landscape = *It;
		if (!IsValid(Landscape)) continue;

#if WITH_EDITOR
		// 冷启动（无头一次性运行/编辑器刚开图）时 RenderHeightmap 的输入可能没就绪，有两种冷态，
		// 都表现为"返回 true 但一个 texel 都不写"：
		// 1) 高度图纹理仍在异步编译，GPU 侧还是占位小纹理——FinishAllCompilation 等它编完；
		// 2) edit-layer 存在待处理合并请求——ForceLayersFullUpdate 同步补齐。
		// 交互编辑器稳态两步都近乎立即返回，不会引入可感知的卡顿。
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (!Landscape->IsUpToDate())
		{
			UE_LOG(LogTemp, Log, TEXT("[CSSW] Landscape '%s' edit layers not merged yet; forcing full update before RenderHeightmap"), *Landscape->GetName());
			Landscape->ForceLayersFullUpdate();
		}
#endif

		UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
		TempRT->ClearColor = FLinearColor(0, 0, 0, 0);
		TempRT->InitAutoFormat(SafeTextureSize, SafeTextureSize);
		TempRT->UpdateResourceImmediate(true);

		const FTransform AreaTransform(FQuat::Identity, CaptureCenter, FVector::OneVector);
		const FBox2D AreaExtents(FVector2D(-CaptureExtent.X, -CaptureExtent.Y), FVector2D(CaptureExtent.X, CaptureExtent.Y));
		if (!Landscape->RenderHeightmap(AreaTransform, AreaExtents, TempRT))
		{
			TempRT->MarkAsGarbage();
			continue;
		}

		const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
		const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
		FTextureRenderTargetResource* R_LandscapeRGBA = TempRT->GameThread_GetRenderTargetResource();

		ENQUEUE_RENDER_COMMAND(CaptureAllLandscapeDepth)(
		[this, R_LandscapeRGBA, R_SceneDepth, CapturedCameraHeight, LandscapeScaleZ, LandscapeOriginZ](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_LandscapeRGBA = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(R_LandscapeRGBA->GetRenderTargetTexture(), TEXT("CSSW.LandscapeG16")));
			FRDGTextureRef RDG_SceneDepth = GraphBuilder.RegisterExternalTexture(
				CreateRenderTarget(R_SceneDepth->GetRenderTargetTexture(), TEXT("CSSW.RT_SceneDepth")));

			ConvertLandscapeHeightmapToDepthRDG(
				GraphBuilder, RDG_LandscapeRGBA, RDG_SceneDepth,
				CapturedCameraHeight, LandscapeScaleZ, LandscapeOriginZ);

			GraphBuilder.Execute();
		});

		// 立即弃置是安全的：GC 销毁 UObject 走 BeginReleaseResource（渲染命令），
		// 排在上面的合并命令之后；合并命令自身持有的只是 resource 指针，不触碰 UObject。
		TempRT->MarkAsGarbage();
	}

	// 有意不 FlushRenderingCommands：捕获结果的全部消费者（SetHeight/求解 pass/像素读回）
	// 要么同在渲染命令队列里排在其后，要么读取 API 自带 flush。同步等待只会把游戏线程
	// 挂在 GPU 光栅化上——这正是旧路径"远慢于 SceneCapture"观感的另一半来源。
}

void ACSShallowWaterCapture::StartSolver(float TimerRate, int32 InIteration)
{
	ClearSolverTimer();
	SolverTimerRate = FMath::Max(TimerRate, 0.0f);
	Iteration = FMath::Max(InIteration, 1);
	ResetSolverState(true);
	if (SimulationPreviewMesh && ReusltMesh)
	{
		ReusltMesh->SetStaticMesh(SimulationPreviewMesh);
		ReusltMesh->SetRelativeScale3D(FVector::OneVector);
	}
	bUseBakedResultMesh = false;
	RefreshConstructionLayout();
	RebuildSimulationVisualization();
	if (SimVisHISM) SimVisHISM->SetVisibility(true);
	if (ReusltMesh) ReusltMesh->SetVisibility(false);
	CaptureAll();
	ScheduleSolverTimerTick();
	OnSolverStarted();
	UE_LOG(LogTemp, Log, TEXT("[CSSW] StartSolver: %s Iteration=%d CaptureSize=%.2f TextureSize=%d TimerRate=%.4f"),
		*GetName(), Iteration, CaptureSize, TextureSize, SolverTimerRate);
}

void ACSShallowWaterCapture::StopSolver()
{
	StopSimulationRuntime();
}

void ACSShallowWaterCapture::OnSolverStarted_Implementation()
{
	UE_LOG(LogTemp, Log, TEXT("OnSolverStarted_Implementation: Default C++ implementation called on %s"), *GetName());
}

ACSShallowWaterCapture::FOnBakeResultMesh ACSShallowWaterCapture::OnBakeResultMeshDelegate;

void ACSShallowWaterCapture::BakeResultMesh()
{
	if (OnBakeResultMeshDelegate.IsBound())
	{
		OnBakeResultMeshDelegate.Execute(this);
		OnBakeComplete();
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("BakeResultMesh: No handler bound (Editor module not loaded?) on %s"), *GetName());
	}
}

int32 ACSShallowWaterCapture::EnsureSWUniqueID()
{
	if (SWUniqueID < 0)
	{
		const FDateTime Time = FDateTime::Now();
		SWUniqueID = Time.GetDay() * 1000000 + Time.GetHour() * 10000 + Time.GetMinute() * 100 + Time.GetSecond();
		// 编号要随 actor 存盘，否则下次打开关卡重新烘焙会写到一组新名字上，旧资产被留在原地。
		Modify();
	}
	return SWUniqueID;
}

FString ACSShallowWaterCapture::GetResultAssetUniqueTag()
{
	return LexToString(EnsureSWUniqueID());
}

FString ACSShallowWaterCapture::BuildBakedAssetPath(const FString& Suffix)
{
	const FString ResultFolderPath = GetResultAssetFolderPath();
	if (ResultFolderPath.IsEmpty()) return FString();

	// 统一命名：CSSW_<SWUniqueID>_<后缀>。编号随 actor 存盘，所以重复烘焙会覆盖同一批资产。
	return FString::Printf(TEXT("%s/CSSW_%s_%s"), *ResultFolderPath, *GetResultAssetUniqueTag(), *Suffix);
}

FString ACSShallowWaterCapture::BuildResultAssetPath(const FString& NameSuffix)
{
	// 结果网格走同一套命名（CSSW_<编号>_SM），NameSuffix 仍追加在最后，供"一个 actor 多份产出"使用。
	return BuildBakedAssetPath(TEXT("SM") + NameSuffix);
}

void ACSShallowWaterCapture::BrowseBakedAssets()
{
#if WITH_EDITOR
	const FString AssetFolderPath = GetResultAssetFolderPath();

	TArray<UObject*> ObjectsToSync;

	if (BakedResultMesh)
	{
		ObjectsToSync.Add(BakedResultMesh);
	}

	if (SWUniqueID >= 0 && !AssetFolderPath.IsEmpty())
	{
		FString MICPath = BuildBakedAssetPath(TEXT("Water"));
		UObject* MIC = StaticLoadObject(UObject::StaticClass(), nullptr, *MICPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
		if (MIC) ObjectsToSync.Add(MIC);
	}

	if (ObjectsToSync.Num() > 0 && GEditor)
	{
		GEditor->SyncBrowserToObjects(ObjectsToSync);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] No baked assets found for %s (SWUniqueID=%d)"), *GetName(), SWUniqueID);
	}
#endif
}

void ACSShallowWaterCapture::OnBakeComplete_Implementation()
{
	ShowBakedResult();
}

void ACSShallowWaterCapture::ShowBakedResult()
{
	StopSolver();
	if (SimVisHISM)
	{
		// 只隐藏，不清实例：这批实例就是烘焙的源网格栅格，清掉之后再烘焙一次就没有源了
		// （只能退回单个预览网格，落在一两个 texel 上，产出零三角）。StartSolver 每次都会
		// 重建它们，留着不会累积。
		SimVisHISM->SetVisibility(false);
	}
	if (ReusltMesh)
	{
		if (BakedResultMesh)
		{
			ReusltMesh->SetStaticMesh(BakedResultMesh);
			ReusltMesh->SetRelativeScale3D(FVector::OneVector);
			bUseBakedResultMesh = true;
		}
		ReusltMesh->SetVisibility(true);
	}
	// 烘焙 MIC 优先于运行时 MID：贴的是落盘贴图，关卡重开后依然有画面。
	ApplyBakedMaterials();
	if (CausticsDecal && (BakedDecalMaterial || DecalMaterial)) CausticsDecal->SetVisibility(true);
}

const FName ACSShallowWaterCapture::AutoProxyTag(TEXT("Auto"));

int32 ACSShallowWaterCapture::DestroyAttachedProxyActors(FName Tag)
{
	TArray<AActor*> AttachedActors;
	GetAttachedActors(AttachedActors);
	int32 Removed = 0;
	for (AActor* Actor : AttachedActors)
	{
		if (!IsValid(Actor)) continue;
		// Tag 为 None 表示"收走全部挂载 actor"；带 tag 时只收自己生成的那批，
		// 免得把美术手工挂上来的东西一起销毁。
		if (!Tag.IsNone() && !Actor->ActorHasTag(Tag)) continue;
		Actor->Destroy();
		++Removed;
	}
	return Removed;
}

void ACSShallowWaterCapture::BuildProxyActors(FName ProxyTag, AStaticMeshActor*& OutMeshActor, ADecalActor*& OutDecalActor)
{
	OutMeshActor = nullptr;
	OutDecalActor = nullptr;

	UWorld* World = GetWorld();
	if (!World || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject)) return;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = this;

	// StaticMeshActor：复制 ReusltMesh 当前网格 + 材质 + 世界变换。
	// 取的是组件"当前"的材质而不是 WaterMaterial：ShowBakedResult 已经把烘焙 MIC 贴上去了，
	// 照抄当前值才能让代理拿到落盘材质（而不是绑 transient RT 的 MID）。
	UStaticMesh* ProxyMesh = ReusltMesh ? ReusltMesh->GetStaticMesh() : nullptr;
	if (ReusltMesh && ProxyMesh)
	{
		AStaticMeshActor* MeshActor = World->SpawnActor<AStaticMeshActor>(
			AStaticMeshActor::StaticClass(), ReusltMesh->GetComponentTransform(), SpawnParams);
		if (MeshActor)
		{
			UStaticMeshComponent* SMC = MeshActor->GetStaticMeshComponent();
			SMC->SetMobility(ReusltMesh->Mobility);
			SMC->SetStaticMesh(ProxyMesh);
			const int32 NumMats = ReusltMesh->GetNumMaterials();
			for (int32 i = 0; i < NumMats; ++i) SMC->SetMaterial(i, ReusltMesh->GetMaterial(i));
			SMC->SetCastShadow(ReusltMesh->CastShadow);
			SMC->bVisibleInRayTracing = ReusltMesh->bVisibleInRayTracing;
			SMC->SetCollisionEnabled(ReusltMesh->GetCollisionEnabled());
			MeshActor->Tags.Add(ProxyTag);
			MeshActor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
			OutMeshActor = MeshActor;
		}
	}

	// DecalActor：位置/缩放/材质/DecalSize 沿用 CausticsDecal。烘焙 MIC 优先，其次是组件上
	// 当前的材质，最后才回退到作者指定的 DecalMaterial。
	UMaterialInterface* ProxyDecalMat = BakedDecalMaterial;
	if (!ProxyDecalMat && CausticsDecal) ProxyDecalMat = CausticsDecal->GetDecalMaterial();
	if (!ProxyDecalMat) ProxyDecalMat = DecalMaterial;
	if (CausticsDecal && ProxyDecalMat)
	{
		ADecalActor* DecalActor = World->SpawnActor<ADecalActor>(
			ADecalActor::StaticClass(), CausticsDecal->GetComponentTransform(), SpawnParams);
		if (DecalActor)
		{
			if (UDecalComponent* DC = DecalActor->GetDecal())
			{
				DC->SetMobility(EComponentMobility::Movable);
				// 用 SetWorldRotation 直接锁死组件世界朝向（俯视投影），不靠 spawn 旋转：
				// ADecalActor 根组件自带 -90° pitch 偏移，会与 spawn 传入的旋转叠加。
				DC->SetWorldRotation(FRotator(-90, 0, 0));
				DC->SetDecalMaterial(ProxyDecalMat);
				DC->DecalSize = CausticsDecal->DecalSize;
				DC->SortOrder = CausticsDecal->SortOrder;
				DC->MarkRenderStateDirty();
			}
			DecalActor->Tags.Add(ProxyTag);
			DecalActor->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
			OutDecalActor = DecalActor;
		}
	}
}

void ACSShallowWaterCapture::SpawnResultProxyActors(AStaticMeshActor*& OutMeshActor, ADecalActor*& OutDecalActor)
{
	// 这个入口的语义是"重建全部代理"：先收走本 actor 下已挂的所有 actor，再按当前状态重建。
	DestroyAttachedProxyActors(NAME_None);
	BuildProxyActors(SWTag, OutMeshActor, OutDecalActor);
}

void ACSShallowWaterCapture::SpawnAutoResultActors()
{
	// 先看有没有结果可代理。没有网格就整个跳过：既不生成、也不隐藏本体组件，
	// 否则会把水体变成"什么都看不见"的状态，比不做还糟。
	UStaticMesh* ResultMesh = ReusltMesh ? ReusltMesh->GetStaticMesh() : nullptr;
	if (!ResultMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] SpawnAutoResultActors: %s has no result mesh (bake or start the solver first); nothing spawned."), *GetName());
		return;
	}

	// 重复调用只替换自己那批：按 tag 回收，不碰手工挂上来的其它 actor。
	DestroyAttachedProxyActors(AutoProxyTag);

	AStaticMeshActor* MeshActor = nullptr;
	ADecalActor* DecalActor = nullptr;
	BuildProxyActors(AutoProxyTag, MeshActor, DecalActor);
	if (!MeshActor && !DecalActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] SpawnAutoResultActors: %s spawned nothing; keeping own components visible."), *GetName());
		return;
	}

	// 代理已经接管显示，本体组件让位；隐藏而不是销毁，重新仿真/烘焙时还要用它们。
	if (ReusltMesh) ReusltMesh->SetVisibility(false);
	if (CausticsDecal) CausticsDecal->SetVisibility(false);

	UE_LOG(LogTemp, Log, TEXT("[CSSW] SpawnAutoResultActors on %s: mesh=%s decal=%s (tag '%s'), own mesh/decal hidden."),
		*GetName(), MeshActor ? *MeshActor->GetName() : TEXT("none"),
		DecalActor ? *DecalActor->GetName() : TEXT("none"), *AutoProxyTag.ToString());
}

void ACSShallowWaterCapture::ShowDebugViewPlane(float Duration)
{
	UWorld* World = GetWorld();
	if (!World) return;

	TArray<AActor*> Found;
	UGameplayStatics::GetAllActorsOfClassWithTag(World, AStaticMeshActor::StaticClass(), FName("CSSWVM"), Found);
	for (AActor* A : Found)
	{
		if (A) A->Destroy();
	}
	DebugViewPlaneActor.Reset();

	if (!DebugMesh) return;

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	const float PlaneScale = CaptureSize / 100.0f;
	FTransform SpawnTransform(GetActorRotation(), GetActorLocation(), FVector(PlaneScale, PlaneScale, 1.0f));
	AStaticMeshActor* Spawned = World->SpawnActor<AStaticMeshActor>(
		AStaticMeshActor::StaticClass(),
		SpawnTransform,
		SpawnParams);
	if (!Spawned) return;

	UStaticMeshComponent* SMC = Spawned->GetStaticMeshComponent();
	SMC->SetStaticMesh(DebugMesh);

	UMaterialInstanceDynamic* MID = SMC->CreateDynamicMaterialInstance(0, DebugMesh->GetMaterial(0));
	if (MID && RT_DebugView)
	{
		MID->SetTextureParameterValue(FName("Height"), RT_DebugView);
	}

	Spawned->Tags.Add(FName("CSSWVM"));
	DebugViewPlaneActor = Spawned;

	if (Duration > 0.f)
	{
		World->GetTimerManager().ClearTimer(DebugViewPlaneTimerHandle);
		World->GetTimerManager().SetTimer(DebugViewPlaneTimerHandle,
			FTimerDelegate::CreateWeakLambda(this, [this]()
			{
				if (DebugViewPlaneActor.IsValid())
				{
					DebugViewPlaneActor->Destroy();
					DebugViewPlaneActor.Reset();
				}
			}),
			Duration, false);
	}
}

void ACSShallowWaterCapture::Destroyed()
{
	ClearSolverTimer();
	Super::Destroyed();
}

void ACSShallowWaterCapture::BeginDestroy()
{
	ClearSolverTimer();
	ReleaseTransientRenderResources();
	Super::BeginDestroy();
}

void ACSShallowWaterCapture::ReleaseTransientRenderResources()
{
	ClearMeshGeneratorCache();
	ClearGeneratedDataTextureCache();
}

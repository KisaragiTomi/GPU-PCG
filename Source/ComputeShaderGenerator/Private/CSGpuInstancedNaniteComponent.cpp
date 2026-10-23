#include "CSGpuInstancedNaniteComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h" // CSGpuInstancedAddPackPointsPass：点云打包与经典路共用一份

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/CollisionProfile.h"
#include "Engine/InstancedStaticMesh.h"   // FInstancedStaticMeshSceneProxy（非 Nanite 资产的回退）
#include "Engine/StaticMesh.h"
#include "Engine/World.h"                 // FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates
#include "GlobalShader.h"
#include "GPUSceneWriter.h"
#include "HAL/LowLevelMemTracker.h"
#include "InstanceDataSceneProxy.h"
#include "InstancedStaticMeshSceneProxyDesc.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "NaniteSceneProxy.h"
#include "PrimitiveSceneDesc.h"
#include "PrimitiveSceneProxy.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"                  // UseGPUScene
#include "SceneInterface.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedNanite, Log, All);

// GPU-Scene 写入 pass 的分段计时（与经典路那四条同一套口径，见 CSGpuInstancedMeshSceneProxy.cpp）。
DECLARE_GPU_STAT_NAMED(CSGpuInstancedNaniteWrite, TEXT("CSGpuInstanced NaniteWrite"));

// 承诺了"改了就通知"的 GPU 源（FCSGpuInstanceSourceGPU::bNotifiesChanges）要不要真的跳过没变的帧。
// 0 = 不信那份承诺，照旧每帧重写 —— 对照用的基线开关（InstanceCullingOptimization.md §9.3），也是怀疑某个生产者漏通知时的排查手段：
// 设成 0 画面就对了，说明它有一条改 buffer 的路径没通知。
static TAutoConsoleVariable<int32> CVarCSGpuInstancedNaniteSkipUnchangedWrites(
	TEXT("r.CSGpuInstanced.Nanite.SkipUnchangedWrites"),
	1,
	TEXT("Nanite 路：承诺了改动通知的 GPU 实例源，数据没变的帧不重写 GPU-Scene。\n")
	TEXT(" 0: 每帧都重写（2026-10-02 以前的行为，对照 / 排查漏通知用）。\n")
	TEXT(" 1: 只在被通知、图元动了、代理重建或距离带的视点挪动时重写（默认）。"),
	ECVF_Default);

// -----------------------------------------------------------------------------
// GPU-Scene 写入 pass（Shaders/Private/CSGpuInstancedNaniteWriter.usf）
// -----------------------------------------------------------------------------

class FCSGpuInstancedNaniteWriteCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSGpuInstancedNaniteWriteCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGpuInstancedNaniteWriteCS, FGlobalShader);

	static constexpr uint32 NumThreadsPerGroup = 64;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcInstances)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcInstanceCount)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SrcCustomData)
		SHADER_PARAMETER(uint32, bSrcHasCustomData)
		SHADER_PARAMETER(uint32, SrcMaxInstances)
		SHADER_PARAMETER(uint32, WriteNumSlots)
		SHADER_PARAMETER(uint32, WritePrimitiveId)
		SHADER_PARAMETER(uint32, WritePayloadFlags)
		SHADER_PARAMETER(uint32, WriteCustomDataCount)
		// 距离带（与经典路 InstanceCullCS 同一条判据；bCullBand = 0 时整段不跑）。
		SHADER_PARAMETER(uint32, bCullBand)
		SHADER_PARAMETER(FVector3f, CullViewLocal)
		SHADER_PARAMETER(float, CullComponentScale)
		SHADER_PARAMETER(float, CullStart)
		SHADER_PARAMETER(float, CullEnd)
		SHADER_PARAMETER(float, CullFade)
		// 整份旧式参数结构而不是只挂 GPUSceneWriterUB：SceneDefinitions.h 把
		// ENABLE_SCENE_DATA_DX11_UB_ERROR_WORKAROUND 定成 1，着色器那一侧读的是全局 RW buffer，
		// 只绑 UB 的话 GPU-Scene 的 UAV 根本没绑上（PCG 的场景写入器也是这么绑的）。
		SHADER_PARAMETER_STRUCT_INCLUDE(FGPUSceneWriterParameters, GPUSceneWriterParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("USE_GPU_SCENE_DATA_RW"), 1);
		OutEnvironment.SetDefine(TEXT("NUM_THREADS_PER_GROUP"), NumThreadsPerGroup);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGpuInstancedNaniteWriteCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedNaniteWriter.usf", "WriteInstancesCS", SF_Compute);

namespace
{
	/** 一次写入要的全部东西，按值拷进委托：渲染线程执行时不回头读组件。 */
	struct FCSGpuNaniteWriteInputs
	{
		FCSGpuNaniteInstanceFeed Feed;
		FMatrix44f WorldToComponent = FMatrix44f::Identity; // 只有点云源用
		uint32 NumSlots = 0;
		TSharedPtr<FCSGpuNaniteWriteStats, ESPMode::ThreadSafe> Stats;

		/** 距离带：视点换进组件空间，距离再乘组件缩放回到世界单位（与经典路剔除同一口径）。 */
		bool bCullBand = false;
		FVector3f CullViewLocal = FVector3f::ZeroVector;
		float CullComponentScale = 1.0f;
		float CullStart = 0.0f;
		float CullEnd = 0.0f;
		float CullFade = 0.0f;

		/**
		 * 本体同时挂着经典路的只投影副本，而且那份副本画的是「可见源行号」布局：它的绘制经裸 SRV **直接读生产者的
		 * 行 / custom data**。写入 pass 读完之后要把这两条交回 SRVMask（CSGpuInstancedHandOffProducerBuffer），
		 * 否则它们停在 compute 的访问状态上。别的情形不交 —— 没有绘制读它们，让 RDG 照常在图末收尾。
		 */
		bool bHandOffProducerBuffers = false;
	};

	/** 渲染线程，GPU-Scene 更新时序里（FGPUScene::UpdateInternal 的 UpdateInstancesFromCompute 段）。 */
	void CSGpuInstancedNanite_AddWritePasses(FRDGBuilder& GraphBuilder, const FGPUSceneWriteDelegateParams& Params, const FCSGpuNaniteWriteInputs& In)
	{
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CSGpuInstancedNaniteWrite, "CSGpuInstanced.NaniteWrite");
		UE_LOG(LogCSGpuInstancedNanite, Verbose, TEXT("[NaniteWrite] dispatch: PersistentPrimitiveId=%u InstanceSceneDataOffset=%u Slots=%u Source=%s"),
			Params.PersistentPrimitiveId, Params.InstanceSceneDataOffset, In.NumSlots,
			In.Feed.Packed.IsValid() ? TEXT("packed") : (In.Feed.Points.IsValid() ? TEXT("points") : TEXT("cpu")));
		if (Params.PersistentPrimitiveId == uint32(INDEX_NONE) || In.NumSlots == 0) return;

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		const FCSGpuNaniteInstanceFeed& Feed = In.Feed;

		FRDGBufferRef Rows = nullptr;
		FRDGBufferRef Count = nullptr;
		FRDGBufferRef CustomData = nullptr;
		// 生产者自己的那两条（读完要交回外部访问状态，见下面 AddPass 之后）。临时建的行 / custom data 不算。
		FRDGBufferRef ProducerRows = nullptr;
		FRDGBufferRef ProducerCustomData = nullptr;
		uint32 MaxSourceInstances = 0;

		if (Feed.Packed.IsValid())
		{
			// 生产者的 buffer：只读（与剔除 pass 同一条口径）。同一张图里经典路的只投影副本可能已经读过它、
			// 并把它交到外部访问状态了（「可见源行号」布局），所以经 CSGpuInstancedRegisterProducerBuffer 拿回来再用。
			ProducerRows = CSGpuInstancedRegisterProducerBuffer(GraphBuilder, Feed.Packed.PackedInstances);
			Rows = ProducerRows;
			Count = GraphBuilder.RegisterExternalBuffer(Feed.Packed.Counter);
			if (Feed.Packed.CustomData.IsValid())
			{
				ProducerCustomData = CSGpuInstancedRegisterProducerBuffer(GraphBuilder, Feed.Packed.CustomData);
				CustomData = ProducerCustomData;
			}
			MaxSourceInstances = Feed.Packed.Capacity;
		}
		else if (Feed.Points.IsValid())
		{
			// 点云没有现成的行：先用经典路那个打包 pass 打进一块临时行，行布局因此与经典路逐位相同。
			// 剔除球那一行 Nanite 不读，球心半径给零。
			Count = GraphBuilder.RegisterExternalBuffer(Feed.Points.Counter);
			Rows = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Feed.Points.Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSGpuInstanced.NanitePointRows"));
			CSGpuInstancedAddPackPointsPass(GraphBuilder, ShaderMap, Feed.Points, Count, Rows, In.WorldToComponent, FVector3f::ZeroVector, 0.0f, Feed.Points.Capacity);
			MaxSourceInstances = Feed.Points.Capacity;
		}
		else if (Feed.CpuRows.IsValid())
		{
			// CPU 数组只在变了（或代理换了、图元动了）时才会走到这里，所以每次整份上传是划算的：
			// 省下的是一块跟着组件活的常驻 buffer 和它的全部生命周期管理。
			const TArray<FVector4f>& CpuRows = *Feed.CpuRows;
			const uint32 LiveCount = uint32(CpuRows.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
			Rows = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), FMath::Max(LiveCount * CS_GPU_INSTANCED_ROW_FLOAT4S, 1u)), TEXT("CSGpuInstanced.NaniteCpuRows"));
			// None：RDG 自己拷一份，委托（和它持有的数组）不必活到图执行。
			GraphBuilder.QueueBufferUpload(Rows, CpuRows.GetData(), LiveCount * CS_GPU_INSTANCED_ROW_FLOAT4S * sizeof(FVector4f), ERDGInitialDataFlags::None);
			Count = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGpuInstanced.NaniteCpuCount"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Count, PF_R32_UINT)), LiveCount);
			MaxSourceInstances = LiveCount;
		}
		if (!Rows || !Count) return;

		const bool bHasCustomData = CustomData != nullptr;
		if (!bHasCustomData)
		{
			// RDG 拒绝空 SRV：拿一块清零的小 buffer 顶上，bSrcHasCustomData 挡着，一个字节都不读。
			CustomData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(float), CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS), TEXT("CSGpuInstanced.NaniteNoCustomData"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CustomData, PF_R32_UINT)), 0u);
		}

		FCSGpuInstancedNaniteWriteCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCSGpuInstancedNaniteWriteCS::FParameters>();
		PassParameters->SrcInstances = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Rows, PF_A32B32G32R32F));
		PassParameters->SrcInstanceCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Count, PF_R32_UINT));
		PassParameters->SrcCustomData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CustomData, PF_R32_FLOAT));
		PassParameters->bSrcHasCustomData = bHasCustomData ? 1u : 0u;
		PassParameters->SrcMaxInstances = MaxSourceInstances;
		PassParameters->WriteNumSlots = In.NumSlots;
		PassParameters->WritePrimitiveId = Params.PersistentPrimitiveId;
		PassParameters->WritePayloadFlags = Params.PackedInstanceSceneDataFlags;
		PassParameters->WriteCustomDataCount = Params.NumCustomDataFloats == uint32(INDEX_NONE) ? 0u : Params.NumCustomDataFloats;
		PassParameters->bCullBand = In.bCullBand ? 1u : 0u;
		PassParameters->CullViewLocal = In.CullViewLocal;
		PassParameters->CullComponentScale = In.CullComponentScale;
		PassParameters->CullStart = In.CullStart;
		PassParameters->CullEnd = In.CullEnd;
		PassParameters->CullFade = In.CullFade;
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		PassParameters->GPUSceneWriterParameters = Params.GPUWriteParams;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS

		TShaderMapRef<FCSGpuInstancedNaniteWriteCS> Shader(ShaderMap);
		// NeverCull：写的是 GPU-Scene 自己的 buffer，这张图里没有别的 pass 读它，RDG 会以为没人要结果。
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("WriteInstances %u slots", In.NumSlots),
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull, Shader, PassParameters,
			FComputeShaderUtils::GetGroupCountWrapped(int32(FMath::DivideAndRoundUp(In.NumSlots, FCSGpuInstancedNaniteWriteCS::NumThreadsPerGroup))));

		// 同一个生产者还喂着经典路的只投影副本时，那边的绘制经裸 SRV 直接读这两条：读完交回 SRVMask，
		// 而不是让它们停在这个 compute pass 的访问状态上（理由见 bHandOffProducerBuffers）。
		if (In.bHandOffProducerBuffers)
		{
			CSGpuInstancedHandOffProducerBuffer(GraphBuilder, ProducerRows);
			CSGpuInstancedHandOffProducerBuffer(GraphBuilder, ProducerCustomData);
		}

		if (In.Stats.IsValid()) In.Stats->DispatchedWrites.fetch_add(1);
		CSV_CUSTOM_STAT(CSGpuInstanced, NaniteWrites, 1, ECsvCustomStatOp::Accumulate);
		CSV_CUSTOM_STAT(CSGpuInstanced, NaniteWriteSlots, int32(In.NumSlots), ECsvCustomStatOp::Accumulate);
	}
}

// -----------------------------------------------------------------------------
// UCSGpuInstancedNaniteComponent
// -----------------------------------------------------------------------------

UCSGpuInstancedNaniteComponent::UCSGpuInstancedNaniteComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, Stats(MakeShared<FCSGpuNaniteWriteStats, ESPMode::ThreadSafe>())
{
	// GPU 源每帧重抄、渲染设置每帧从本体拉 —— 编辑器里不跑 PIE 也得转。
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	PrimaryComponentTick.bTickEvenWhenPaused = true;
	PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
	bTickInEditor = true;

	// Movable：非 Movable 的静态网格组件一动就重建代理（ShouldRecreateProxyOnUpdateTransform），
	// 而重建 = 新实例区间 = 全体 HIDDEN 等下一次写入。生产者拖尺寸时包围盒每帧在变，那就是每帧闪。
	Mobility = EComponentMobility::Movable;

	// 不是可选的：引擎对 GPU-only 实例的 Lumen card 与距离场是 check()，开着就崩（见类注释）。
	bAffectDynamicIndirectLighting = false;
	bAffectDistanceFieldLighting = false;

	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SetGenerateOverlapEvents(false);
	bCanEverAffectNavigation = false;
	bUseAsOccluder = false;
	bEnableAutoLODGeneration = false;
}

void UCSGpuInstancedNaniteComponent::SyncFromOwner(const UCSGpuInstancedMeshComponent& Owner)
{
	// 网格走 SetStaticMesh：它自己处理渲染状态，并更新 IStaticMeshComponent 反查表 ——
	// 资产重建时引擎靠那张表找到这个组件（这个替身存在的全部理由）。
	if (GetStaticMesh() != Owner.BaseMesh) SetStaticMesh(Owner.BaseMesh);

	bool bDirty = false;

	// InstanceMaterial 在经典路是"每个实例都用它画"，这里保持同一个意思：设了就盖住资产的每一个
	// 材质槽；没设就用资产自己的材质（经典路没设时是引擎默认材质 —— 那是因为它的快照里没有材质，
	// Nanite 这边资产的材质就在手边，没有理由画成一片灰）。
	TArray<TObjectPtr<UMaterialInterface>> WantedOverrides;
	if (Owner.InstanceMaterial && GetStaticMesh()) WantedOverrides.Init(Owner.InstanceMaterial, GetStaticMesh()->GetStaticMaterials().Num());
	if (OverrideMaterials != WantedOverrides)
	{
		OverrideMaterials = MoveTemp(WantedOverrides);
		bDirty = true;
	}

	// 距离带：原值留给写入 pass 逐实例抖；引擎只拿带的两道最外沿当硬边（见 InstanceMinDrawDistance 的注释）。
	BandStart = FMath::Max(Owner.InstanceStartCullDistance, 0.0f);
	BandEnd = FMath::Max(Owner.InstanceEndCullDistance, 0.0f);
	BandFade = FMath::Max(Owner.InstanceCullFadeDistance, 0.0f);
	const int32 WantedMinDraw = BandStart > 0.0f ? FMath::Max(0, FMath::RoundToInt(BandStart - BandFade)) : 0;
	int32 WantedEndCull = BandEnd > 0.0f ? FMath::Max(1, FMath::RoundToInt(BandEnd)) : 0;
	// 引擎只在 End > 0 时才认 Min（Nanite::FSceneProxy::GetInstanceDrawDistanceMinMax）：只有起点的组件（远处组合草）
	// 给 End 填一个远到不起作用的数，否则它的起点被整个忽略，近处也画一层组合草。
	if (WantedMinDraw > 0 && WantedEndCull == 0) WantedEndCull = MAX_int32 / 2;
	if (InstanceEndCullDistance != WantedEndCull || InstanceMinDrawDistance != WantedMinDraw)
	{
		InstanceEndCullDistance = WantedEndCull;
		InstanceMinDrawDistance = WantedMinDraw;
		bDirty = true;
	}

	// 位域成员不能绑引用，逐条比。只抄生产者真会改的那几条（投影开关是逐物种的，见 CSGroundActor）。
	// 影子交给经典副本时（bClassicShadowOnNanitePath）替身自己不投影 —— 否则 VSM 里再落一份硬影子。
	const bool bWantCastShadow = Owner.CastShadow && !Owner.IsClassicShadowCopyActive();
	if (CastShadow != bWantCastShadow)
	{
		CastShadow = bWantCastShadow;
		bDirty = true;
	}
	if (bCastDynamicShadow != Owner.bCastDynamicShadow)
	{
		bCastDynamicShadow = Owner.bCastDynamicShadow;
		bDirty = true;
	}
	if (bReceivesDecals != Owner.bReceivesDecals)
	{
		bReceivesDecals = Owner.bReceivesDecals;
		bDirty = true;
	}
	if (bRenderCustomDepth != Owner.bRenderCustomDepth || CustomDepthStencilValue != Owner.CustomDepthStencilValue)
	{
		bRenderCustomDepth = Owner.bRenderCustomDepth;
		CustomDepthStencilValue = Owner.CustomDepthStencilValue;
		bDirty = true;
	}
	if (bVisibleInRayTracing != Owner.bVisibleInRayTracing)
	{
		bVisibleInRayTracing = Owner.bVisibleInRayTracing;
		bDirty = true;
	}
	// GPU 源每帧重写实例 ⇒ VSM 每帧把这批实例的缓存页当"动了"作废。本体上设成 Static 就是告诉引擎
	// "别因为更新作废影子"：换来缓存，代价是生产者真改了实例时影子要等下次整体失效才跟上。
	if (ShadowCacheInvalidationBehavior != Owner.ShadowCacheInvalidationBehavior)
	{
		ShadowCacheInvalidationBehavior = Owner.ShadowCacheInvalidationBehavior;
		bDirty = true;
	}
	const FLightingChannels& Channels = Owner.LightingChannels;
	if (LightingChannels.bChannel0 != Channels.bChannel0 || LightingChannels.bChannel1 != Channels.bChannel1 || LightingChannels.bChannel2 != Channels.bChannel2)
	{
		LightingChannels = Channels;
		bDirty = true;
	}

	if (bDirty) MarkRenderStateDirty();

	// WPO 关闭距离有自己的 setter：代理在就当场改它（Nanite 代理有游戏线程入口），不重建。
	if (WorldPositionOffsetDisableDistance != FMath::Max(Owner.InstanceWPODisableDistance, 0))
	{
		SetWorldPositionOffsetDisableDistance(FMath::Max(Owner.InstanceWPODisableDistance, 0));
	}

	// CustomPrimitiveData：经典路的 proxy 把本体的这份送进 primitive uniform buffer，替身也得有同一份，
	// 同一张材质在两条路上才读到同一个值。
	// setter 自带 memcmp 短路、不重建 proxy，所以逐帧抄是免费的。
	const TArray<float>& WantedPrimitiveData = Owner.GetCustomPrimitiveData().Data;
	if (WantedPrimitiveData.Num() > 0) SetCustomPrimitiveDataFloatArray(0, TConstArrayView<float>(WantedPrimitiveData));

	// 这两条有自己的 setter（会处理渲染状态），不走 bDirty。
	if (GetVisibleFlag() != Owner.GetVisibleFlag()) SetVisibility(Owner.GetVisibleFlag());
	if (bHiddenInGame != Owner.bHiddenInGame) SetHiddenInGame(Owner.bHiddenInGame);
}

void UCSGpuInstancedNaniteComponent::SetFeed(FCSGpuNaniteInstanceFeed&& InFeed, const FBox& InInstancesLocalBounds)
{
	const uint32 NewSlots = InFeed.IsValid() ? InFeed.GetSlotCount() : 0u;
	const bool bSlotsChanged = NewSlots != NumGpuSceneSlots;
	const bool bBoundsChanged = !(InInstancesLocalBounds == InstancesLocalBounds);

	Feed = MoveTemp(InFeed);
	NumGpuSceneSlots = NewSlots;
	InstancesLocalBounds = InInstancesLocalBounds;

	if (bSlotsChanged)
	{
		// 槽数写在代理的 FInstanceSceneDataBuffers 里（NumInstancesGPUOnly），只能换代理；
		// 新代理的写入由 CreateRenderState_Concurrent 发，这里对旧代理发反而会被新区间作废。
		UpdateBounds();
		MarkRenderStateDirty();
		return;
	}

	if (bBoundsChanged)
	{
		// 包围盒跟着变换一起下发；SendRenderTransform_Concurrent 那边会顺手再写一次，无害。
		UpdateBounds();
		MarkRenderTransformDirty();
	}

	bWritePending = true;
	IssueGpuSceneWrite();
}

bool UCSGpuInstancedNaniteComponent::NeedsWrite() const
{
	if (bWritePending) return true;
	return SceneProxy != nullptr && (SceneProxy != WrittenProxy || RenderStateSerial != WrittenSerial);
}

void UCSGpuInstancedNaniteComponent::IssueGpuSceneWrite()
{
	// 渲染状态的两个钩子都声明了必须在游戏线程跑（RequiresGameThreadEndOfFrame*），走到别的线程
	// 只可能是引擎某条没见过的路径 —— 记下欠一次，交给下一帧的 Tick。
	if (!IsInGameThread())
	{
		bWritePending = true;
		return;
	}

	FSceneInterface* Scene = GetScene();
	FPrimitiveSceneProxy* Proxy = SceneProxy;
	if (!Scene || !Proxy || NumGpuSceneSlots == 0 || !Feed.IsValid())
	{
		// 没代理但有东西要画 = 代理建在后面（批量注册 / 资产还在编译），欠着等它出现。
		bWritePending = !Proxy && NumGpuSceneSlots > 0 && Feed.IsValid();
		return;
	}

	FCSGpuNaniteWriteInputs Inputs;
	Inputs.Feed = Feed;
	Inputs.WorldToComponent = FMatrix44f(GetComponentTransform().ToInverseMatrixWithScale());
	Inputs.NumSlots = NumGpuSceneSlots;
	Inputs.Stats = Stats;
	if (const UCSGpuInstancedMeshComponent* Owner = Cast<UCSGpuInstancedMeshComponent>(GetOuter()))
	{
		Inputs.bHandOffProducerBuffers = Owner->IsClassicShadowCopyActive() && Owner->GetGpuLayout().bVisibleSourceIndex;
	}

	// 距离带：视点每次写入时现取（不通知的 GPU 源每帧都写，带因此跟着相机走；会通知的源由 Tick 里的 10 cm 判据触发）。取不到视点就不抖 —— 引擎的硬边还在，
	// 两档只是在带里重叠，而不是露出一圈空地。
	FVector CullView;
	if (IsDistanceBandActive() && ResolveCullView(CullView))
	{
		const FTransform& ComponentTransform = GetComponentTransform();
		const FVector Scale = ComponentTransform.GetScale3D().GetAbs();
		Inputs.bCullBand = true;
		Inputs.CullViewLocal = FVector3f(ComponentTransform.InverseTransformPosition(CullView));
		Inputs.CullComponentScale = FMath::Max(float(FMath::Max3(Scale.X, Scale.Y, Scale.Z)), UE_KINDA_SMALL_NUMBER);
		Inputs.CullStart = BandStart;
		Inputs.CullEnd = BandEnd;
		Inputs.CullFade = BandFade;
		LastWrittenCullView = CullView;
		bWroteWithBand = true;
	}
	else
	{
		bWroteWithBand = false;
	}

	FPrimitiveSceneDesc PrimitiveSceneDesc;
	PrimitiveSceneDesc.SceneProxy = Proxy;

	// 同一帧里对同一个图元发多次，只有最后一次生效（TSceneUpdateCommandQueue 每种 payload 只留最新的），
	// 所以 SetFeed / 变换钩子 / Tick 各发一次不会叠成三次写入。
	PRAGMA_DISABLE_EXPERIMENTAL_WARNINGS
	Scene->UpdatePrimitiveInstancesFromCompute(&PrimitiveSceneDesc, FGPUSceneWriteDelegate::CreateLambda(
		[Inputs = MoveTemp(Inputs)](FRDGBuilder& GraphBuilder, const FGPUSceneWriteDelegateParams& Params)
		{
			CSGpuInstancedNanite_AddWritePasses(GraphBuilder, Params, Inputs);
		}));
	PRAGMA_ENABLE_EXPERIMENTAL_WARNINGS

	UE_LOG(LogCSGpuInstancedNanite, Verbose, TEXT("[NaniteWrite] issue: %s proxy=%p serial=%u slots=%u frame=%llu"),
		*GetPathName(), Proxy, RenderStateSerial, NumGpuSceneSlots, GFrameCounter);

	WrittenProxy = Proxy;
	WrittenSerial = RenderStateSerial;
	bWritePending = false;
}

void UCSGpuInstancedNaniteComponent::OnRegister()
{
	Super::OnRegister();
	if (!PreEndOfFrameUpdatesHandle.IsValid())
	{
		PreEndOfFrameUpdatesHandle = FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.AddUObject(this, &UCSGpuInstancedNaniteComponent::HandlePreEndOfFrameUpdates);
	}
}

void UCSGpuInstancedNaniteComponent::OnUnregister()
{
	FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.Remove(PreEndOfFrameUpdatesHandle);
	PreEndOfFrameUpdatesHandle.Reset();
	Super::OnUnregister();
}

void UCSGpuInstancedNaniteComponent::HandlePreEndOfFrameUpdates(UWorld* InWorld)
{
	// 这里还在帧末更新之外（广播早于 bPostTickComponentUpdate 置位），发写入是合法的。
	if (InWorld == GetWorld() && NeedsWrite()) IssueGpuSceneWrite();
}

void UCSGpuInstancedNaniteComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	// 替身的 Outer 就是本体（UCSGpuInstancedMeshComponent::UpdateNaniteComponent 建的）。
	if (const UCSGpuInstancedMeshComponent* Owner = Cast<UCSGpuInstancedMeshComponent>(GetOuter())) SyncFromOwner(*Owner);

	// 会通知的源（CPU 行、承诺了 bNotifiesChanges 的 GPU 源）平时只在变了时写 —— 变了的那一笔记在 bWritePending 上，
	// NeedsWrite 会看到。开着距离带时带要跟相机走，视点挪过 10 cm 就重写一次。
	// 不通知的 GPU 源（老契约）每帧都写：生产者原地改了什么没人知道。
	const bool bEveryFrame = !Feed.NotifiesChanges() || (Feed.IsGpu() && CVarCSGpuInstancedNaniteSkipUnchangedWrites.GetValueOnGameThread() == 0);
	bool bBandMoved = false;
	if (!bEveryFrame && IsDistanceBandActive())
	{
		FVector CullView;
		if (ResolveCullView(CullView)) bBandMoved = !bWroteWithBand || FVector::DistSquared(CullView, LastWrittenCullView) > 100.0;
	}

	if (bEveryFrame || NeedsWrite() || bBandMoved) IssueGpuSceneWrite();
}

bool UCSGpuInstancedNaniteComponent::ResolveCullView(FVector& OutWorldLocation)
{
	const UCSGpuInstancedMeshComponent* Owner = Cast<UCSGpuInstancedMeshComponent>(GetOuter());
	if (Owner && Owner->GetDistanceCullViewOverride(OutWorldLocation))
	{
		LastCullView = OutWorldLocation;
		bHasLastCullView = true;
		return true;
	}

	// 上一帧画过这个世界的透视视点（编辑器透视视口 / 游戏视口往里填，场景捕获不填）。
	// ⚠️ 多视口时这张表的次序随绘制次序变，取 [0] 会让带在几个机位之间来回跳；取离上次最近的那个
	// （与 ACSGroundActor::UpdateGroundCoverView 同一个理由）。它在 UWorld::Tick 末尾清空 —— 帧末更新
	// 那一次写入读到的是空表，沿用上次的视点。
	const UWorld* World = GetWorld();
	if (World && !World->ViewLocationsRenderedLastFrame.IsEmpty())
	{
		FVector Best = World->ViewLocationsRenderedLastFrame[0];
		if (bHasLastCullView)
		{
			double BestDistance = TNumericLimits<double>::Max();
			for (const FVector& Candidate : World->ViewLocationsRenderedLastFrame)
			{
				const double Distance = FVector::DistSquared(Candidate, LastCullView);
				if (Distance >= BestDistance) continue;
				BestDistance = Distance;
				Best = Candidate;
			}
		}
		LastCullView = Best;
		bHasLastCullView = true;
	}

	if (!bHasLastCullView) return false;
	OutWorldLocation = LastCullView;
	return true;
}

void UCSGpuInstancedNaniteComponent::CreateRenderState_Concurrent(FRegisterComponentContext* Context)
{
	Super::CreateRenderState_Concurrent(Context);

	// 新代理 = 新分配的实例区间，引擎把每个槽初始化成 HIDDEN（GPUSceneDataManagement.usf 的
	// GPUSceneSetInstancePrimitiveIdCS）。这一帧不写进去，整族就消失一帧 —— 换槽数、换网格、编辑器里
	// 改任何属性都会走到这里。写入命令排在 AddPrimitive 之后，同一次场景更新里先分配区间再跑写入 pass。
	// ⚠️ 帧末批量重建时这里还没有代理（见 HandlePreEndOfFrameUpdates），IssueGpuSceneWrite 只记下欠一次。
	++RenderStateSerial;
	IssueGpuSceneWrite();
}

void UCSGpuInstancedNaniteComponent::SendRenderTransform_Concurrent()
{
	Super::SendRenderTransform_Concurrent();

	// GPU-Scene 里存的是实例的世界变换（相对图元 tile），图元一动就全部过期，而引擎对 GPU-only 实例
	// 从不重传。写入排在变换更新之后：同一次 GPU-Scene 更新里先传图元的新 LocalToWorld，再跑写入 pass
	// 拿它把组件局部的行升到世界。
	IssueGpuSceneWrite();
}

FPrimitiveSceneProxy* UCSGpuInstancedNaniteComponent::CreateSceneProxy()
{
	if (NumGpuSceneSlots == 0 || !GetStaticMesh()) return nullptr;
	return Super::CreateSceneProxy();
}

FPrimitiveSceneProxy* UCSGpuInstancedNaniteComponent::CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite)
{
	LLM_SCOPE(ELLMTag::InstancedMesh);

	FSceneInterface* Scene = GetScene();
	if (!Scene) return nullptr;
	if (!UseGPUScene(Scene->GetShaderPlatform(), Scene->GetFeatureLevel()))
	{
		UE_LOG(LogCSGpuInstancedNanite, Warning, TEXT("%s: GPU-only instances need GPU Scene, which this platform does not have. Nothing will be drawn."), *GetPathName());
		return nullptr;
	}

	FInstanceSceneDataBuffers InstanceSceneDataBuffers(/*InbInstanceDataIsGPUOnly=*/true);
	{
		FInstanceSceneDataBuffers::FAccessTag AccessTag(PointerHash(this));
		FInstanceSceneDataBuffers::FWriteView ProxyData = InstanceSceneDataBuffers.BeginWriteAccess(AccessTag);

		InstanceSceneDataBuffers.SetPrimitiveLocalToWorld(GetRenderMatrix(), AccessTag);

		ProxyData.NumInstancesGPUOnly = int32(NumGpuSceneSlots);
		// 固定写满 CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS：生产者给不给 custom data 都一样宽，没给的写零 ——
		// 与经典路给可见槽写零同一个口径，材质的 Per Instance Custom Data 两条路读到的一样。
		ProxyData.NumCustomDataFloats = CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS;
		ProxyData.Flags.bHasPerInstanceCustomData = ProxyData.NumCustomDataFloats > 0;
		ProxyData.Flags.bHasPerInstanceRandom = true;
		// 所有实例共用资产的包围盒（没有逐实例包围盒），Nanite 的实例剔除用它。
		ProxyData.InstanceLocalBounds.SetNum(1);
		ProxyData.InstanceLocalBounds[0] = GetStaticMesh()->GetBounds();

		InstanceSceneDataBuffers.EndWriteAccess(AccessTag);
		InstanceSceneDataBuffers.ValidateData();
	}

	FInstancedStaticMeshSceneProxyDesc Desc;
	Desc.InitializeFromStaticMeshComponent(this);
	Desc.InstanceDataSceneProxy = MakeShared<FInstanceDataSceneProxy, ESPMode::ThreadSafe>(MoveTemp(InstanceSceneDataBuffers));
	Desc.InstanceMinDrawDistance = InstanceMinDrawDistance;
	Desc.InstanceEndCullDistance = InstanceEndCullDistance;
	Desc.bUseGpuLodSelection = true;

	if (bCreateNanite) return ::new Nanite::FSceneProxy(NaniteMaterials, Desc);

	// 资产没有 Nanite 数据（或 r.Nanite 关着 / 材质不被 Nanite 支持）：引擎的 ISM 代理同样吃 GPU-Scene
	// 里的实例，画的是回退 LOD。能画总比不画好，但名不副实要说出来（每个替身一次，重建代理不重复刷）。
	// 资产上刚把 Nanite 关掉的那一帧不算：本体在下一次帧末更新前就会拆掉这个替身、换回 GPU 剔除路
	// （UCSGpuInstancedMeshComponent::HandleBaseMeshRebuilt），那是换路的过渡，报成警告只会误导。
	if (!bWarnedNotNanite && GetStaticMesh()->IsNaniteEnabled())
	{
		bWarnedNotNanite = true;
		UE_LOG(LogCSGpuInstancedNanite, Warning,
			TEXT("%s: '%s' is not rendering through Nanite (no Nanite data, r.Nanite off, or an unsupported material); drawing its fallback LODs through the engine's instanced static mesh proxy instead."),
			*GetPathName(), *GetNameSafe(GetStaticMesh()));
	}
	return ::new FInstancedStaticMeshSceneProxy(Desc, Scene->GetFeatureLevel());
}

FBoxSphereBounds UCSGpuInstancedNaniteComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	// 不能用 UStaticMeshComponent 的：那是一个实例的包围盒，而这个图元覆盖整族实例 ——
	// 场景剔除对 GPU-only 实例是整个图元进一个格子（SceneCulling.cpp 的 SingleCell），包围盒小了整族一起被剔。
	if (InstancesLocalBounds.IsValid) return FBoxSphereBounds(InstancesLocalBounds.TransformBy(LocalToWorld));
	return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0);
}

FString UCSGpuInstancedNaniteComponent::DebugDescribeMismatchSync(const UCSGpuInstancedMeshComponent& Owner)
{
	if (!IsRegistered()) return TEXT("Nanite 替身没注册（一个实例都画不出来）");
	if (GetStaticMesh() != Owner.BaseMesh)
	{
		return FString::Printf(TEXT("Nanite 替身画的是 '%s'，不是 BaseMesh '%s'"), *GetNameSafe(GetStaticMesh()), *GetNameSafe(Owner.BaseMesh));
	}
	if (NumGpuSceneSlots == 0) return TEXT("Nanite 替身没有实例槽（实例源是空的）");

	// 渲染状态可能还挂在帧末没落地（改完属性、换完源的同一帧）。诊断本来就允许阻塞，先落地再问 ——
	// 问一个马上就要被换掉的代理，得到的答案对画面没有意义。
	DoDeferredRenderUpdates_Concurrent();
	if (NeedsWrite()) IssueGpuSceneWrite();

	const FPrimitiveSceneProxy* Proxy = SceneProxy;
	if (!Proxy)
	{
		return FString::Printf(TEXT("Nanite 替身没有场景代理（'%s' 还在编译 / 渲染数据没就绪 / PSO 还在预缓存 / 平台没有 GPU-Scene）"),
			*GetNameSafe(GetStaticMesh()));
	}
	if (!Proxy->IsNaniteMesh())
	{
		return FString::Printf(TEXT("'%s' 开了 Nanite 但没走 Nanite（Nanite 数据没建出来、材质不被 Nanite 支持、或 r.Nanite=0），画的是回退网格的引擎 ISM 代理"),
			*GetNameSafe(GetStaticMesh()));
	}
	if (WrittenProxy != Proxy || WrittenSerial != RenderStateSerial)
	{
		return TEXT("代理建好了但 GPU-Scene 写入没发给它（它的实例槽全是 HIDDEN）");
	}

	// Nanite 代理对不满足 MATUSAGE_InstancedStaticMeshes 的材质段会静默换成默认材质
	// （NaniteResources.cpp 的 bIsInstancedMesh 那一段），症状与"没绑材质"逐像素相同。
	for (int32 MaterialIndex = 0; MaterialIndex < GetNumMaterials(); ++MaterialIndex)
	{
		UMaterialInterface* Material = GetMaterial(MaterialIndex);
		const UMaterial* BaseMaterial = Material ? Material->GetMaterial() : nullptr;
		// 引擎默认材质为所有用途都编了着色器，不看这面旗子。
		if (BaseMaterial && BaseMaterial->IsDefaultMaterial()) continue;
		if (!BaseMaterial || !BaseMaterial->bUsedWithInstancedStaticMeshes)
		{
			return FString::Printf(TEXT("材质槽 %d '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（Nanite 代理会静默换成默认材质）"),
				MaterialIndex, *GetNameSafe(Material));
		}
	}
	return FString();
}

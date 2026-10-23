#include "CSGpuInstancedMeshSceneProxy.h"
#include "CSGpuInstancedMeshVertexFactory.h"

#include "ConvexVolume.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Engine.h"
#include "GlobalShader.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MeshBatch.h"
#include "ProfilingDebugging/RealtimeGPUProfiler.h" // DECLARE_GPU_STAT_NAMED
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"
#include "SceneInterface.h"
#include "SceneManagement.h"
#include "SceneView.h"
#include "SceneViewExtension.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedProxy, Log, All);

// 分段 GPU 计时（`stat gpu` / `r.GPUCsvStatsEnabled 1` 的 CSV）。每个 pass 一条，全部代理累加到同一条上；
// 基线与每一步优化的 A/B 都读这几条（InstanceCullingOptimization.md §4.1），所以改 pass 结构时别把 scope 弄丢。
DECLARE_GPU_STAT_NAMED(CSGpuInstancedPackPoints, TEXT("CSGpuInstanced PackPoints"));
DECLARE_GPU_STAT_NAMED(CSGpuInstancedClusterCull, TEXT("CSGpuInstanced ClusterCull"));
DECLARE_GPU_STAT_NAMED(CSGpuInstancedInstanceCull, TEXT("CSGpuInstanced InstanceCull"));
DECLARE_GPU_STAT_NAMED(CSGpuInstancedBuildArgs, TEXT("CSGpuInstanced BuildArgs"));

CSV_DEFINE_CATEGORY(CSGpuInstanced, true);

// 每个代理每帧的剔除建图（InstanceCullingOptimization.md §6.4：组件多了之后这是最大的一笔固定成本）。
// 两项各占一位，可以单独开关（单独量、单独排查）：
//   位 0（1）只登记剔除真用到的常驻流（间接参数 + aux，不登记基础网格那七八条）；
//   位 1（2）CPU 实例数组的行数走 uniform，不再每帧建一块计数 buffer 再清一遍（每个代理每帧 5 个 pass → 4 个）。
// 0 = 2026-10-02 以前的做法，留作对照。
// 逐 LOD 计数器的清零 pass 留着：把它并进 BuildArgs（抄完顺手清）量过，与位 1 同开时 GPU 反而更慢，见
// Docs/InstanceCulling/Step3_20261002.md。
namespace CSGpuInstancedLeanCull
{
	constexpr int32 SubsetStreams = 1;
	constexpr int32 CountUniform = 2;
}

static TAutoConsoleVariable<int32> CVarCSGpuInstancedLeanCullSetup(
	TEXT("r.CSGpuInstanced.LeanCullSetup"),
	CSGpuInstancedLeanCull::SubsetStreams | CSGpuInstancedLeanCull::CountUniform,
	TEXT("UCSGpuInstancedMeshComponent 经典剔除路每帧的建图方式（位标志，默认 3）。\n")
	TEXT(" 0: 登记全部常驻流、CPU 源每帧建一块计数 buffer（旧做法，留作对照）。\n")
	TEXT(" 1: 只登记剔除用到的流。 2: CPU 源的实例数走 uniform。"),
	ECVF_RenderThreadSafe);

// -----------------------------------------------------------------------------
// Cull / LOD compute shaders (Shaders/Private/CSGpuInstancedMesh.usf)
// -----------------------------------------------------------------------------

namespace
{
	constexpr uint32 CullGroupSize = 64;
	constexpr uint32 IndirectArgsPerDraw = 5;

	bool IsSupportedPlatform(EShaderPlatform Platform)
	{
		return IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	}
}

class FCSInstancedPackPointsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedPackPointsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedPackPointsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcPointPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcPointNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcInstanceCount)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWPackedInstances)
		SHADER_PARAMETER(FMatrix44f, WorldToComponent)
		SHADER_PARAMETER(FVector3f, BaseSphereCentre)
		SHADER_PARAMETER(float, BaseSphereRadius)
		SHADER_PARAMETER(float, PointInstanceScale)
		SHADER_PARAMETER(uint32, MaxSourceInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedClusterCullCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedClusterCullCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedClusterCullCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcClusterBounds)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterVisible)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
		SHADER_PARAMETER(FVector3f, ViewOriginLocal)
		SHADER_PARAMETER(float, ComponentScale)
		SHADER_PARAMETER(float, MaxDrawDistanceSq)
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(uint32, bFrustumCull)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedInstanceCullCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedInstanceCullCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedInstanceCullCS, FGlobalShader);

	/** 输出形态：关 = 抄完整的可见属性（四条 RWVis* 缓冲），开 = 只写源行号（RWVisSourceIndices）。
	 *  permutation 而不是 uniform 分支：两种形态声明的 UAV 根本不是同一批，没用到的那批不该出现在着色器里。 */
	class FOutputSourceIndex : SHADER_PERMUTATION_BOOL("CS_INSTANCE_CULL_OUTPUT_INDEX");
	/** 实例数从哪儿来：关 = SrcInstanceCount[0]（GPU 源的计数器），开 = uniform SrcInstanceCountValue（CPU 实例数组）。 */
	class FCountUniform : SHADER_PERMUTATION_BOOL("CS_INSTANCE_COUNT_UNIFORM");
	using FPermutationDomain = TShaderPermutationDomain<FOutputSourceIndex, FCountUniform>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, SrcInstances)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SrcCustomData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SrcInstanceCount)
		SHADER_PARAMETER(uint32, SrcInstanceCountValue)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWClusterVisible)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisTransforms)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisOrigins)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisLightmap)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWVisCustomData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWVisSourceIndices)
		// 0 = 生产者没给 custom data（石阶 / 摆件 / 砖 / 瓦都不给）⇒ 可见槽写零。
		// 用 uniform 而不是 permutation：分支全 wave 一致，运行时代价为零。
		SHADER_PARAMETER(uint32, bHasCustomData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWLodCounters)
		SHADER_PARAMETER_ARRAY(FVector4f, FrustumPlanes, [6])
		SHADER_PARAMETER(FVector3f, ViewOriginLocal)
		SHADER_PARAMETER(float, ComponentScale)
		SHADER_PARAMETER(float, ScreenMultiple)
		SHADER_PARAMETER(FVector4f, LodScreenSizes)
		SHADER_PARAMETER(float, MaxDrawDistanceSq)
		SHADER_PARAMETER(float, MinDrawDistance)
		SHADER_PARAMETER(float, DrawFadeDistance)
		SHADER_PARAMETER(uint32, NumLods)
		SHADER_PARAMETER(uint32, NumClusters)
		SHADER_PARAMETER(uint32, ClusterSize)
		SHADER_PARAMETER(uint32, MaxInstancesPerLod)
		SHADER_PARAMETER(uint32, MaxSourceInstances)
		SHADER_PARAMETER(uint32, bFrustumCull)
		SHADER_PARAMETER(uint32, bLodSelect)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

class FCSInstancedBuildArgsCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSInstancedBuildArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSInstancedBuildArgsCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWLodCounters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIndirectArgs)
		// 每个 draw 一行 (IndexCount, FirstIndex, BaseVertex, Lod)；长度必须与 .usf 的数组同为 CS_GPU_INSTANCED_MAX_DRAWS。
		SHADER_PARAMETER_ARRAY(FUintVector4, DrawTable, [CS_GPU_INSTANCED_MAX_DRAWS])
		SHADER_PARAMETER(uint32, NumDraws)
		SHADER_PARAMETER(uint32, MaxInstancesPerLod)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsSupportedPlatform(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSInstancedPackPointsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "PackPointInstancesCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedClusterCullCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "ClusterCullCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedInstanceCullCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "InstanceCullCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSInstancedBuildArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedMesh.usf", "BuildArgsCS", SF_Compute);

FRDGBufferRef CSGpuInstancedRegisterProducerBuffer(FRDGBuilder& GraphBuilder, const TRefCountPtr<FRDGPooledBuffer>& Pooled)
{
	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(Pooled);
	// 同一张图里前面可能已经有人（另一条路的 pass、另一个 view family）把它交到外部访问状态了；拿回来对内部状态是空操作。
	GraphBuilder.UseInternalAccessMode(Buffer);
	return Buffer;
}

void CSGpuInstancedHandOffProducerBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer)
{
	if (Buffer) GraphBuilder.UseExternalAccessMode(Buffer, ERHIAccess::SRVMask);
}

void CSGpuInstancedAddPackPointsPass(
	FRDGBuilder& GraphBuilder,
	FGlobalShaderMap* ShaderMap,
	const FCSGpuInstancePointSourceGPU& Points,
	FRDGBufferRef InstanceCount,
	FRDGBufferRef OutPackedInstances,
	const FMatrix44f& WorldToComponent,
	const FVector3f& BaseSphereCentre,
	float BaseSphereRadius,
	uint32 MaxSourceInstances)
{
	FCSInstancedPackPointsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedPackPointsCS::FParameters>();
	Params->SrcPointPositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Points.Positions), PF_A32B32G32R32F));
	Params->SrcPointNormals = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Points.Normals), PF_A32B32G32R32F));
	Params->SrcInstanceCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InstanceCount, PF_R32_UINT));
	Params->RWPackedInstances = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutPackedInstances, PF_A32B32G32R32F));
	// Point positions are absolute world space. The component sits at ordinary level
	// coordinates, so a float matrix keeps sub-millimetre accuracy over the level.
	Params->WorldToComponent = WorldToComponent;
	Params->BaseSphereCentre = BaseSphereCentre;
	Params->BaseSphereRadius = BaseSphereRadius;
	Params->PointInstanceScale = Points.InstanceScale;
	Params->MaxSourceInstances = MaxSourceInstances;

	TShaderMapRef<FCSInstancedPackPointsCS> Shader(ShaderMap);
	RDG_EVENT_SCOPE_STAT(GraphBuilder, CSGpuInstancedPackPoints, "CSGpuInstanced.PackPoints");
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("PackPointInstances"), Shader, Params,
		FComputeShaderUtils::GetGroupCount(MaxSourceInstances, CullGroupSize));
}

// -----------------------------------------------------------------------------
// Shared per-frame cull driver
//
// The cull passes must land in the renderer's own graph ahead of the base pass — a proxy has no
// per-frame hook of its own, so one view extension walks every live instanced proxy. Registration
// happens on the render thread from Create/DestroyRenderThreadResources, which is also where the
// callback runs, so the set needs no locking.
// -----------------------------------------------------------------------------

namespace
{
	TSet<FCSGpuInstancedMeshSceneProxy*>& GetRegisteredProxies()
	{
		static TSet<FCSGpuInstancedMeshSceneProxy*> Proxies;
		return Proxies;
	}

	class FCSGpuInstancedCullViewExtension : public FSceneViewExtensionBase
	{
	public:
		explicit FCSGpuInstancedCullViewExtension(const FAutoRegister& AutoReg)
			: FSceneViewExtensionBase(AutoReg)
		{
		}

		virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily& InViewFamily) override
		{
			bCulledThisFamily = false;
		}

		virtual void PreRenderView_RenderThread(FRDGBuilder& GraphBuilder, FSceneView& InView) override
		{
			if (!bLoggedFirstCall)
			{
				bLoggedFirstCall = true;
				UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] view extension first fired; %d proxy(s) registered"),
					GetRegisteredProxies().Num());
			}

			// One cull per family: the visible-instance buffers are per-proxy, not per-view, so a
			// second view would overwrite the first one's result before either is drawn.
			if (bCulledThisFamily) return;
			bCulledThisFamily = true;

#if CSV_PROFILER_STATS
			const double SetupStartSeconds = FPlatformTime::Seconds();
#endif
			// 只剔这个视图族所在场景的代理。登记表是全进程一张（编辑器世界、PIE 世界、材质 / 缩略图预览场景都在里面），
			// 以前每个视图族都把所有场景的代理剔一遍：别的世界的相机压出来的结果没人画，纯属白干，
			// 开着 PIE 或一排缩略图时这笔白干的活是成倍的。
			const FSceneInterface* FamilyScene = InView.Family ? InView.Family->Scene : nullptr;
			for (FCSGpuInstancedMeshSceneProxy* Proxy : GetRegisteredProxies())
			{
				if (FamilyScene && &Proxy->GetScene() != FamilyScene) continue;
				Proxy->RunCulling(GraphBuilder, InView);
			}
#if CSV_PROFILER_STATS
			// 累加而不是覆盖：一帧里可能有几个 view family（主视图 + 场景捕获），每个都剔一遍。
			CSV_CUSTOM_STAT(CSGpuInstanced, CullSetupMs, float((FPlatformTime::Seconds() - SetupStartSeconds) * 1000.0), ECsvCustomStatOp::Accumulate);
			CSV_CUSTOM_STAT(CSGpuInstanced, CullProxies, GetRegisteredProxies().Num(), ECsvCustomStatOp::Accumulate);
#endif
		}

	private:
		bool bCulledThisFamily = false;
		bool bLoggedFirstCall = false;
	};

	TSharedPtr<FCSGpuInstancedCullViewExtension, ESPMode::ThreadSafe> GCullViewExtension;
}

void FCSGpuInstancedMeshSceneProxy::EnsureCullServiceStarted()
{
	check(IsInGameThread());
	if (!GCullViewExtension.IsValid() && GEngine)
	{
		GCullViewExtension = FSceneViewExtensions::NewExtension<FCSGpuInstancedCullViewExtension>();
		UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] cull view extension registered (valid=%d)"),
			GCullViewExtension.IsValid() ? 1 : 0);
	}
}

// -----------------------------------------------------------------------------
// FCSGpuInstancedMeshSceneProxy
// -----------------------------------------------------------------------------

FCSGpuInstancedMeshSceneProxy::FCSGpuInstancedMeshSceneProxy(UCSGpuInstancedMeshComponent* Component, const FCSMeshResidentRef& InResident,
	const TArray<UMaterialInterface*>& InDrawMaterials)
	: FCSGpuMeshSceneProxy(Component, InDrawMaterials.Num() > 0 ? InDrawMaterials[0] : nullptr, "FCSGpuInstancedMeshSceneProxy")
	, Resident(InResident)
	, LODs(Component->GetBaseMeshSnapshot().LODs)
	, Sections(Component->GetBaseMeshSnapshot().Sections)
	, GpuSource(Component->GetInstanceSourceGPU())
	, GpuPointSource(Component->GetInstancePointSourceGPU())
	, EndCullDistance(FMath::Max(Component->InstanceEndCullDistance, 0.0f))
	, StartCullDistance(FMath::Max(Component->InstanceStartCullDistance, 0.0f))
	, CullFadeDistance(FMath::Max(Component->InstanceCullFadeDistance, 0.0f))
	, LodScreenSizeScale(FMath::Max(Component->LODScreenSizeScale, 0.01f))
	, bFrustumCull(Component->bGpuFrustumCulling)
	, bLodSelect(Component->bGpuLODSelection)
	, bShadowOnly(Component->IsClassicShadowCopyActive())
	, Layout(Component->GetGpuLayout())
{
	// Holding the shared reference (not a raw pointer) is what makes both teardown orders safe:
	// the component can be destroyed first, or the mesh object can be collected first.
	SetExternalStreams(InResident);

	// The base recomputed Lumen visibility in its own constructor, where this leaf's
	// WantsRayTracingGeometry() override was not yet visible; recompute with it in place so an
	// instanced primitive without a BLAS is not reported to Lumen as traceable.
	UpdateVisibleInLumenScene();
	// Same story for the base's surface-cache setup: it turned off redundant-transform skipping so
	// its capture batches follow every edit, but this leaf never registers any.
	SetCanSkipRedundantTransformUpdates(true);

	const FBox& BaseBounds = Component->GetBaseMeshSnapshot().LocalBounds;
	BaseSphereCentre = FVector3f(BaseBounds.GetCenter());
	BaseSphereRadius = float(BaseBounds.GetExtent().Size());

	// The layout was derived from this same snapshot on the same thread, so this cannot fire — but
	// RunCulling indexes LODs[Lod] straight out of it, and reading one LOD past the end is not a
	// symptom anyone would trace back to a layout that disagreed with its own base mesh.
	Layout.NumLODs = FMath::Clamp(Layout.NumLODs, 1u, uint32(FMath::Max(LODs.Num(), 1)));

	// 同一个理由钳 draw 表：arg set 的个数是声明进常驻流的 NumDraws，多写一段就越过 args 缓冲的末尾。
	// 属于已经被丢弃的 LOD 的段也一并去掉（快照丢 LOD 时本来就同步截了段，这里只是不信任何一方）。
	Sections.SetNum(FMath::Min(Sections.Num(), int32(FMath::Min(Layout.NumDraws, uint32(CS_GPU_INSTANCED_MAX_DRAWS)))));
	Sections.RemoveAll([this](const FCSGpuInstancedSection& Section) { return Section.LodIndex < 0 || Section.LodIndex >= int32(Layout.NumLODs); });

	// 一段一张材质，恒非空。基座构造函数只按第 0 张算了相关性（不透明 / 双面 / 速度……），多段时必须并上
	// 其余各张 —— 漏掉的话，比如"第 0 段不透明、第 1 段半透明"会让第 1 段根本进不了半透明通道。
	DrawMaterials.Reserve(Sections.Num());
	for (int32 Index = 0; Index < Sections.Num(); ++Index)
	{
		UMaterialInterface* DrawMaterial = InDrawMaterials.IsValidIndex(Index) ? InDrawMaterials[Index] : nullptr;
		if (!DrawMaterial) DrawMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		DrawMaterials.Add(DrawMaterial);
		if (Index > 0) MaterialRelevance |= DrawMaterial->GetRelevance_Concurrent(GetScene().GetShaderPlatform());
	}

	// Each LOD needs its own fixed-size region in the visible buffers because SV_InstanceID
	// restarts per draw, so the cost scales with LOD count as well as instance count. 80 bytes
	// per slot (3+1+1 float4). Still worth a warning even though the mesh's VRAM pre-flight now
	// refuses an allocation past the device's share outright: a request that fits is not the same
	// as a request that was a good idea, and it is measured against the ratcheted capacity.
	const uint64 VisibleBytesPerSlot = Layout.bVisibleSourceIndex ? sizeof(uint32) : uint64(CS_GPU_INSTANCED_ROW_FLOAT4S) * sizeof(FVector4f);
	const uint64 VisibleBytes = uint64(Layout.InstanceCapacity) * Layout.NumLODs * VisibleBytesPerSlot;
	if (VisibleBytes > 64ull * 1024ull * 1024ull)
	{
		UE_LOG(LogCSGpuInstancedProxy, Warning,
			TEXT("%s: %u instances x %u LODs need %.1f MiB of visible-instance buffers. Reduce the LOD count or split the component."),
			*GetOwnerName().ToString(), Layout.InstanceCapacity, Layout.NumLODs, double(VisibleBytes) / (1024.0 * 1024.0));
	}
}

FCSGpuInstancedMeshSceneProxy::~FCSGpuInstancedMeshSceneProxy()
{
}

SIZE_T FCSGpuInstancedMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

TUniquePtr<FLocalVertexFactory> FCSGpuInstancedMeshSceneProxy::CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const
{
	if (Layout.bVisibleSourceIndex) return MakeUnique<FCSGpuInstancedIndexedVertexFactory>(InFeatureLevel, InDebugName);
	return MakeUnique<FCSGpuInstancedMeshVertexFactory>(InFeatureLevel, InDebugName);
}

void FCSGpuInstancedMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	// Constant for the proxy's lifetime: no fade, nothing selected, no dithered LOD transition —
	// the cull pass moves an instance between LODs outright. Without this the vertex shader would
	// read an unbound uniform buffer and collapse every instance to the origin.
	FInstancedStaticMeshVFLooseUniformShaderParameters LooseParameters;
	LooseParameters.InstancingViewZCompareZero = FVector4f(ForceInit);
	LooseParameters.InstancingViewZCompareOne = FVector4f(ForceInit);
	LooseParameters.InstancingViewZConstant = FVector4f(ForceInit);
	LooseParameters.InstancingTranslatedWorldViewOriginZero = FVector4f(ForceInit);
	LooseParameters.InstancingTranslatedWorldViewOriginOne = FVector4f(ForceInit);
	// x = fade start distance, y = 1 / fade range, z = render-selected, w = render-deselected.
	LooseParameters.InstancingFadeOutParams = FVector4f(UE_BIG_NUMBER, 0.0f, 1.0f, 1.0f);
	InstancedLooseUniformBuffer = FInstancedStaticMeshVFLooseUniformShaderParametersRef::CreateUniformBufferImmediate(
		LooseParameters, UniformBuffer_MultiFrame);

	FCSGpuMeshSceneProxy::CreateRenderThreadResources(RHICmdList);

	if (!bRegisteredForCulling)
	{
		GetRegisteredProxies().Add(this);
		bRegisteredForCulling = true;
	}
}

void FCSGpuInstancedMeshSceneProxy::DestroyRenderThreadResources()
{
	if (bRegisteredForCulling)
	{
		GetRegisteredProxies().Remove(this);
		bRegisteredForCulling = false;
	}

	if (DiagnosticReadback)
	{
		delete DiagnosticReadback;
		DiagnosticReadback = nullptr;
	}

	InstancedLooseUniformBuffer.SafeRelease();
	FCSGpuMeshSceneProxy::DestroyRenderThreadResources();
	// 顶点工厂（上一行里已经释放）手上那两个裸指针指的就是它们，所以排在后面。
	ProducerRowsSRV.SafeRelease();
	ProducerCustomDataSRV.SafeRelease();
}

// -----------------------------------------------------------------------------
// Streams
// -----------------------------------------------------------------------------

void CSGpuInstancedBuildAuxStreamDescs(
	TArray<FCSGpuStreamDesc>& OutStreams,
	const FCSGpuInstancedGpuLayout& Layout,
	bool bExternalPackedSource)
{
	// Every LOD gets its own full-capacity region in the visible buffers: SV_InstanceID restarts
	// at 0 for each indirect draw, so the per-LOD start offset has to be a CPU-side constant and
	// cannot depend on the GPU-decided counts.
	const uint32 VisibleSlots = Layout.InstanceCapacity * Layout.NumLODs;
	const uint32 ClusterCapacity = Layout.ClusterSize > 0
		? uint32(FMath::DivideAndRoundUp(Layout.InstanceCapacity, Layout.ClusterSize))
		: 0u;

	auto AddAux = [&OutStreams](const TCHAR* DebugName, ECSGpuInstancedAuxSlot Slot, uint32 BytesPerElement,
		EPixelFormat Format, uint32 ElementCount)
	{
		FCSGpuStreamDesc D;
		D.DebugName = DebugName;
		D.Role = ECSGpuStreamRole::AuxVertex;
		D.BytesPerElement = BytesPerElement;
		// A zero-element descriptor is refused outright by the mesh (it would allocate nothing and
		// then report the whole mesh as unallocated), so an unused slot still gets one element.
		D.ElementsPerUnit = FMath::Max(ElementCount, 1u);
		D.CountSource = ECSGpuCountSource::Fixed;
		D.SrvFormat = Format;
		D.VfType = VET_None;
		D.TexCoordIndex = uint8(Slot);
		OutStreams.Add(D);
	};

	// The packed rows are supplied directly by a packed GPU source; for a point source the cull
	// builds them itself each frame, so it needs the room.
	AddAux(TEXT("CSGpuInstanced.SourceInstances"), ECSGpuInstancedAuxSlot::SourceInstances, sizeof(FVector4f), PF_A32B32G32R32F,
		bExternalPackedSource ? 1u : Layout.InstanceCapacity * CS_GPU_INSTANCED_ROW_FLOAT4S);
	AddAux(TEXT("CSGpuInstanced.ClusterBounds"), ECSGpuInstancedAuxSlot::ClusterBounds, sizeof(FVector4f), PF_A32B32G32R32F, ClusterCapacity);
	AddAux(TEXT("CSGpuInstanced.ClusterVisible"), ECSGpuInstancedAuxSlot::ClusterVisible, sizeof(uint32), PF_R32_UINT, ClusterCapacity);
	if (Layout.bVisibleSourceIndex)
	{
		// 每个可见槽 4 字节的源行号（旧布局是 88 字节的完整属性）；绘制经它直接读源实例行。
		AddAux(TEXT("CSGpuInstanced.VisibleSourceIndices"), ECSGpuInstancedAuxSlot::VisibleSourceIndices, sizeof(uint32), PF_R32_UINT, VisibleSlots);
		// 一行零：生产者没给 custom data 时材质读它（见 ECSGpuInstancedAuxSlot::CustomDataFallback）。大小与实例数无关。
		AddAux(TEXT("CSGpuInstanced.CustomDataFallback"), ECSGpuInstancedAuxSlot::CustomDataFallback, sizeof(float), PF_R32_FLOAT,
			uint32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS));
	}
	else
	{
		AddAux(TEXT("CSGpuInstanced.VisibleTransforms"), ECSGpuInstancedAuxSlot::VisibleTransforms, sizeof(FVector4f), PF_A32B32G32R32F, VisibleSlots * 3u);
		AddAux(TEXT("CSGpuInstanced.VisibleOrigins"), ECSGpuInstancedAuxSlot::VisibleOrigins, sizeof(FVector4f), PF_A32B32G32R32F, VisibleSlots);
		AddAux(TEXT("CSGpuInstanced.VisibleLightmap"), ECSGpuInstancedAuxSlot::VisibleLightmap, sizeof(FVector4f), PF_A32B32G32R32F, VisibleSlots);
		AddAux(TEXT("CSGpuInstanced.VisibleCustomData"), ECSGpuInstancedAuxSlot::VisibleCustomData, sizeof(float), PF_R32_FLOAT,
			VisibleSlots * uint32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS));
	}
	// Fixed at the maximum rather than sized to NumLODs: 16 bytes, and it keeps a base mesh gaining
	// or losing a LOD from re-declaring this stream (which reallocates the whole resident set).
	AddAux(TEXT("CSGpuInstanced.LodCounters"), ECSGpuInstancedAuxSlot::LodCounters, sizeof(uint32), PF_R32_UINT, CS_GPU_INSTANCED_MAX_LODS);
}

void FCSGpuInstancedMeshSceneProxy::OnStreamsAllocated(FRHICommandListBase& RHICmdList)
{
	// Recorded here rather than in the constructor because this is the moment the buffers were
	// actually adopted: the mesh can be reallocated between proxy creation on the game thread and
	// this hook running on the render thread, and a generation captured before that would pin the
	// proxy as stale from birth. See AdoptedAllocationGeneration.
	AdoptedAllocationGeneration = Resident.IsValid() ? Resident->AllocationGeneration : 0u;

	// 剔除只碰间接参数与本叶子的 aux 流（那一行 custom data 零除外：它只在上传时清一次）。流表变了分配代数就变，
	// RunCulling 开头那道代数检查会先拦住，所以这张位图不会对着另一张流表用。
	CullStreamMask = ~0ull;
	if (Resident.IsValid() && Resident->Streams.Num() <= 64)
	{
		CullStreamMask = 0ull;
		for (int32 Index = 0; Index < Resident->Streams.Num(); ++Index)
		{
			const FCSGpuStreamDesc& Desc = Resident->Streams[Index].Desc;
			const bool bLeafAux = Desc.Role == ECSGpuStreamRole::AuxVertex
				&& Desc.TexCoordIndex >= uint8(ECSGpuInstancedAuxSlot::SourceInstances)
				&& Desc.TexCoordIndex != uint8(ECSGpuInstancedAuxSlot::CustomDataFallback);
			if (bLeafAux || Desc.Role == ECSGpuStreamRole::IndirectArgs) CullStreamMask |= 1ull << Index;
		}
	}

	auto* InstancedVF = static_cast<FCSGpuInstancedMeshVertexFactory*>(VertexFactory.Get());
	if (!InstancedVF) return;

	if (Layout.bVisibleSourceIndex)
	{
		// 源行：packed GPU 源读生产者自己那块 buffer（常驻流里那条只是占位），其余读常驻流 SourceInstances
		// （CPU 源是上传进去的，点云源是每帧剔除前现打进去的）。
		FRHIShaderResourceView* Rows = nullptr;
		if (GpuSource.IsValid())
		{
			ProducerRowsSRV = RHICmdList.CreateShaderResourceView(GpuSource.PackedInstances->GetRHI(),
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_A32B32G32R32F));
			Rows = ProducerRowsSRV;
		}
		else
		{
			Rows = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::SourceInstances));
		}

		// custom data：生产者给了就直接读它那条（逐源行，下标就是源行号）；没给读那一行零，下标钉成 0。
		const bool bSourceCustomData = GpuSource.IsValid() && GpuSource.CustomData.IsValid();
		FRHIShaderResourceView* CustomData = nullptr;
		if (bSourceCustomData)
		{
			ProducerCustomDataSRV = RHICmdList.CreateShaderResourceView(GpuSource.CustomData->GetRHI(),
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(PF_R32_FLOAT));
			CustomData = ProducerCustomDataSRV;
		}
		else
		{
			CustomData = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::CustomDataFallback));
		}

		FRHIShaderResourceView* VisibleIndices = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleSourceIndices));
		if (!Rows || !VisibleIndices || !CustomData)
		{
			UE_LOG(LogCSGpuInstancedProxy, Error,
				TEXT("[CSGpuInstanced] %s: the visible-source-index streams did not resolve (rows=%d indices=%d customdata=%d). ")
				TEXT("The mesh's stream layout is missing this leaf's aux slots — check ECSGpuInstancedAuxSlot against the retained set."),
				*GetOwnerName().ToString(), Rows ? 1 : 0, VisibleIndices ? 1 : 0, CustomData ? 1 : 0);
			return;
		}
		static_cast<FCSGpuInstancedIndexedVertexFactory*>(InstancedVF)->SetIndexedInstanceStreams(Rows, VisibleIndices, CustomData, bSourceCustomData);
		return;
	}

	FRHIShaderResourceView* Origins = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleOrigins));
	FRHIShaderResourceView* Transforms = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleTransforms));
	FRHIShaderResourceView* Lightmap = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleLightmap));
	FRHIShaderResourceView* CustomData = GetStreamSRV(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleCustomData));

	// The one place an aux-slot mistake becomes visible. A slot that collides with another stream's
	// (Role, TexCoordIndex) is refused at declaration time and simply never exists, and a factory
	// handed null instance SRVs draws garbage or faults rather than logging anything — which is
	// precisely the failure the slots were renumbered away from (see ECSGpuInstancedAuxSlot).
	if (!Origins || !Transforms || !Lightmap)
	{
		UE_LOG(LogCSGpuInstancedProxy, Error,
			TEXT("[CSGpuInstanced] %s: the visible-instance streams did not resolve (origins=%d transforms=%d lightmap=%d). ")
			TEXT("The mesh's stream layout is missing this leaf's aux slots — check ECSGpuInstancedAuxSlot against the retained set."),
			*GetOwnerName().ToString(), Origins ? 1 : 0, Transforms ? 1 : 0, Lightmap ? 1 : 0);
		return;
	}

	// CustomData 允许为空（老网格的常驻集里没有这条槽位时）：工厂那边会把
	// NumCustomDataFloats 置 0，材质读到 0 而不是去读一条冒名顶替的缓冲。
	InstancedVF->SetInstanceStreams(Origins, Transforms, Lightmap, CustomData);
}

// -----------------------------------------------------------------------------
// Per-frame culling
// -----------------------------------------------------------------------------

void FCSGpuInstancedMeshSceneProxy::RunCulling(FRDGBuilder& GraphBuilder, const FSceneView& View)
{
	if (DiagnosticState == EDiagnosticState::Pending)
	{
		UE_LOG(LogCSGpuInstancedProxy, Log, TEXT("[CSGpuInstanced] RunCulling entered: DrawValid=%d InstanceCapacity=%u NumClusters=%u"),
			DrawDesc.bValid ? 1 : 0, Layout.InstanceCapacity, Layout.NumClusters);
	}

	if (!DrawDesc.bValid || !Layout.IsValid()) return;
	if (!Resident.IsValid() || !Resident->IsAllocated()) return;
	// The set moved out from under the buffers this proxy adopted; anything written now lands
	// somewhere the draw does not look. See AdoptedAllocationGeneration.
	if (Resident->AllocationGeneration != AdoptedAllocationGeneration) return;

	RDG_EVENT_SCOPE(GraphBuilder, "CSGpuInstanced.Cull");

	// Cull in component-local space: the frustum and the view origin come across in double
	// precision and land as plain floats, which keeps LWC out of the shader entirely.
	const FMatrix ComponentToWorld = GetLocalToWorld();
	const FMatrix WorldToLocal = ComponentToWorld.Inverse();

	FVector4f FrustumPlanes[6];
	const FConvexVolume& Frustum = View.GetCullingFrustum();
	for (int32 i = 0; i < 6; ++i)
	{
		// Fewer than six planes means an unbounded side (an editor viewport's culling frustum
		// routinely has four). The shader tests dot(N, C) - W <= Radius, so a plane that rejects
		// nothing needs W = +BIG, not -BIG: with -BIG the test reads dot + BIG <= Radius and
		// rejects EVERY instance, which silently blanked the whole component in those views.
		if (!Frustum.Planes.IsValidIndex(i))
		{
			FrustumPlanes[i] = FVector4f(0.0f, 0.0f, 1.0f, UE_BIG_NUMBER);
			continue;
		}

		FPlane Plane = Frustum.Planes[i].TransformBy(WorldToLocal);
		const FVector Normal(Plane.X, Plane.Y, Plane.Z);
		const double Length = Normal.Size();
		if (Length > UE_DOUBLE_SMALL_NUMBER) FrustumPlanes[i] = FVector4f(FVector3f(Normal / Length), float(Plane.W / Length));
		else FrustumPlanes[i] = FVector4f(0.0f, 0.0f, 1.0f, UE_BIG_NUMBER);
	}

	const FVector4 ViewOriginLocal4 = WorldToLocal.TransformPosition(View.ViewMatrices.GetViewOrigin());
	const FVector3f ViewOriginLocal = FVector3f(float(ViewOriginLocal4.X), float(ViewOriginLocal4.Y), float(ViewOriginLocal4.Z));
	const FVector Scale = ComponentToWorld.GetScaleVector();
	const float ComponentScale = FMath::Max(float(FMath::Max3(Scale.X, Scale.Y, Scale.Z)), UE_KINDA_SMALL_NUMBER);

	const FMatrix& ProjMatrix = View.ViewMatrices.GetProjectionMatrix();
	const float ScreenMultiple = FMath::Max(0.5f * float(ProjMatrix.M[0][0]), 0.5f * float(ProjMatrix.M[1][1]));

	FVector4f LodScreenSizes(ForceInit);
	for (int32 Lod = 0; Lod < int32(Layout.NumLODs); ++Lod) LodScreenSizes[Lod] = LODs[Lod].ScreenSize * LodScreenSizeScale;
	// Draws with no sections to draw means a layout that disagrees with its own snapshot; culling into it is
	// harmless but drawing is not, and GetDynamicMeshElements already draws nothing in that case.
	if (Sections.IsEmpty()) return;

	// The mesh's own render-thread edit. It registers every resident stream into the caller's graph
	// exactly as UCSMesh::EditMeshSync does, takes back the ones last frame handed off in external
	// access mode, and — when the scope closes at the end of this function — leaves each of them in
	// CSGpuMeshStreams::FinalAccessForRole's state. That restoration used to be written out by hand
	// down there, which made this function the second copy of a rule whose violations are silent: a
	// stream left in RDG's default epilogue (SRVMask) is illegal for index / indirect use and the
	// mesh simply stops drawing. A pass added below now needs no list kept in step with it.
	const int32 LeanFlags = CVarCSGpuInstancedLeanCullSetup.GetValueOnRenderThread();
	const bool bLeanStreams = (LeanFlags & CSGpuInstancedLeanCull::SubsetStreams) != 0;
	const bool bLeanCount = (LeanFlags & CSGpuInstancedLeanCull::CountUniform) != 0;
	FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident, bLeanStreams ? CullStreamMask : ~0ull);

	// A missing slot means the layout was refused at declaration time (OnStreamsAllocated already
	// logged which one); culling into nothing is the right response every frame after that.
	auto Aux = [&Edit](ECSGpuInstancedAuxSlot Slot)
	{
		return Edit->Find(ECSGpuStreamRole::AuxVertex, uint8(Slot));
	};

	const bool bOutputSourceIndex = Layout.bVisibleSourceIndex;

	FRDGBufferRef ClusterBoundsBuffer = Aux(ECSGpuInstancedAuxSlot::ClusterBounds);
	FRDGBufferRef ClusterVisible = Aux(ECSGpuInstancedAuxSlot::ClusterVisible);
	// 两种布局各有各的可见缓冲（见 CSGpuInstancedBuildAuxStreamDescs），另一种的那几条压根没声明。
	FRDGBufferRef VisTransforms = bOutputSourceIndex ? nullptr : Aux(ECSGpuInstancedAuxSlot::VisibleTransforms);
	FRDGBufferRef VisOrigins = bOutputSourceIndex ? nullptr : Aux(ECSGpuInstancedAuxSlot::VisibleOrigins);
	FRDGBufferRef VisLightmap = bOutputSourceIndex ? nullptr : Aux(ECSGpuInstancedAuxSlot::VisibleLightmap);
	FRDGBufferRef VisCustomData = bOutputSourceIndex ? nullptr : Aux(ECSGpuInstancedAuxSlot::VisibleCustomData);
	FRDGBufferRef VisSourceIndices = bOutputSourceIndex ? Aux(ECSGpuInstancedAuxSlot::VisibleSourceIndices) : nullptr;
	FRDGBufferRef LodCounters = Aux(ECSGpuInstancedAuxSlot::LodCounters);
	FRDGBufferRef IndirectArgs = Edit->IndirectArgs();
	// A packed GPU source replaces this stream outright; the mesh then carries only a placeholder.
	// It belongs to the producer rather than to the mesh, so it is registered directly and is not
	// the edit's to restore — the cull only ever reads it.
	FRDGBufferRef ProducerRows = GpuSource.IsValid() ? CSGpuInstancedRegisterProducerBuffer(GraphBuilder, GpuSource.PackedInstances) : nullptr;
	FRDGBufferRef SourceInstances = ProducerRows ? ProducerRows : Aux(ECSGpuInstancedAuxSlot::SourceInstances);

	if (!ClusterBoundsBuffer || !ClusterVisible) return;
	if (bOutputSourceIndex ? !VisSourceIndices : (!VisTransforms || !VisOrigins || !VisLightmap || !VisCustomData)) return;

	// 生产者的 custom data（可空）。与 SourceInstances 同一条口径：它属于生产者而不是这张网格，
	// 直接 register，剔除只读不写。源行号布局下剔除根本不碰它 —— 绘制经源行号直接读（见 OnStreamsAllocated）。
	FRDGBufferRef SourceCustomData = (!bOutputSourceIndex && GpuSource.IsValid() && GpuSource.CustomData.IsValid())
		? CSGpuInstancedRegisterProducerBuffer(GraphBuilder, GpuSource.CustomData)
		: nullptr;
	if (!LodCounters || !IndirectArgs || !SourceInstances) return;

	// Instance count: a plain uniform for the CPU array, the producer's GPU counter otherwise. The
	// CPU value is the LIVE row count, not the capacity — the source buffer ratchets, so the rows
	// past the live count are the previous instance set and would draw as ghosts.
	FRDGBufferRef InstanceCount = nullptr;
	if (GpuSource.IsValid()) InstanceCount = GraphBuilder.RegisterExternalBuffer(GpuSource.Counter);
	else if (GpuPointSource.IsValid()) InstanceCount = GraphBuilder.RegisterExternalBuffer(GpuPointSource.Counter);
	else if (!bLeanCount)
	{
		InstanceCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGpuInstanced.InstanceCount"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(InstanceCount, PF_R32_UINT)), Layout.NumSourceInstances);
	}
	// 精简建图下 CPU 实例数组没有计数 buffer：行数直接当 uniform 交给剔除（FCountUniform permutation）。
	const bool bCountUniform = InstanceCount == nullptr;

	FRDGBufferUAVRef ClusterVisibleUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ClusterVisible, PF_R32_UINT));
	FRDGBufferUAVRef LodCountersUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(LodCounters, PF_R32_UINT));
	FRDGBufferUAVRef IndirectArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT));

	AddClearUAVPass(GraphBuilder, LodCountersUAV, 0u);

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GetScene().GetFeatureLevel());
	const float MaxDrawDistanceSq = EndCullDistance > 0.0f ? EndCullDistance * EndCullDistance : 0.0f;

	// Point source: build this frame's instance rows before anything reads them. Running it here
	// rather than when the producer appends is what removes the need for any notification — the
	// rows are always rebuilt from whatever the point buffer holds right now.
	if (GpuPointSource.IsValid())
	{
		CSGpuInstancedAddPackPointsPass(GraphBuilder, ShaderMap, GpuPointSource, InstanceCount, SourceInstances,
			FMatrix44f(WorldToLocal), BaseSphereCentre, BaseSphereRadius, Layout.InstanceCapacity);
	}

	// Coarse level.
	if (Layout.NumClusters > 0)
	{
		FCSInstancedClusterCullCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedClusterCullCS::FParameters>();
		Params->SrcClusterBounds = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(ClusterBoundsBuffer, PF_A32B32G32R32F));
		Params->RWClusterVisible = ClusterVisibleUAV;
		for (int32 i = 0; i < 6; ++i) Params->FrustumPlanes[i] = FrustumPlanes[i];
		Params->ViewOriginLocal = ViewOriginLocal;
		Params->ComponentScale = ComponentScale;
		Params->MaxDrawDistanceSq = MaxDrawDistanceSq;
		Params->NumClusters = Layout.NumClusters;
		Params->bFrustumCull = bFrustumCull ? 1u : 0u;

		TShaderMapRef<FCSInstancedClusterCullCS> Shader(ShaderMap);
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CSGpuInstancedClusterCull, "CSGpuInstanced.ClusterCull");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("ClusterCull"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(Layout.NumClusters, CullGroupSize));
	}

	// Fine level + compaction.
	{
		FCSInstancedInstanceCullCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedInstanceCullCS::FParameters>();
		Params->SrcInstances = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SourceInstances, PF_A32B32G32R32F));
		// 计数 buffer 与 uniform 二选一，另一个在那个 permutation 里不存在，留空即可。
		if (bCountUniform) Params->SrcInstanceCountValue = Layout.NumSourceInstances;
		else Params->SrcInstanceCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(InstanceCount, PF_R32_UINT));
		Params->RWClusterVisible = ClusterVisibleUAV;
		if (bOutputSourceIndex)
		{
			// 只写源行号：四条可见属性缓冲与 custom data 源在这个 permutation 里都不存在，留空即可
			// （FComputeShaderUtils::AddPass 按着色器实际用到的参数校验）。
			Params->RWVisSourceIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisSourceIndices, PF_R32_UINT));
		}
		else
		{
			Params->RWVisTransforms = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisTransforms, PF_A32B32G32R32F));
			Params->RWVisOrigins = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisOrigins, PF_A32B32G32R32F));
			Params->RWVisLightmap = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisLightmap, PF_A32B32G32R32F));
			Params->RWVisCustomData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisCustomData, PF_R32_FLOAT));
			// 源为空时也必须绑一条**非空** SRV（RDG 拒绝 null 绑定）：拿可见缓冲自己当哑源，
			// 有 bHasCustomData 挡着，一个字节都不会被读。
			Params->SrcCustomData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(
				SourceCustomData ? SourceCustomData : VisCustomData, PF_R32_FLOAT));
			Params->bHasCustomData = SourceCustomData ? 1u : 0u;
		}
		Params->RWLodCounters = LodCountersUAV;
		for (int32 i = 0; i < 6; ++i) Params->FrustumPlanes[i] = FrustumPlanes[i];
		Params->ViewOriginLocal = ViewOriginLocal;
		Params->ComponentScale = ComponentScale;
		Params->ScreenMultiple = ScreenMultiple;
		Params->LodScreenSizes = LodScreenSizes;
		Params->MaxDrawDistanceSq = MaxDrawDistanceSq;
		// 距离带（起始距离 + 过渡带）只在逐实例这一级判：簇级只做最大距离，恒保守（过渡带只会把边界往里收）。
		Params->MinDrawDistance = StartCullDistance;
		Params->DrawFadeDistance = CullFadeDistance;
		Params->NumLods = Layout.NumLODs;
		Params->NumClusters = Layout.NumClusters;
		Params->ClusterSize = Layout.NumClusters > 0 ? Layout.ClusterSize : 0u;
		Params->MaxInstancesPerLod = Layout.InstanceCapacity;
		Params->MaxSourceInstances = Layout.InstanceCapacity;
		Params->bFrustumCull = bFrustumCull ? 1u : 0u;
		Params->bLodSelect = bLodSelect ? 1u : 0u;

		FCSInstancedInstanceCullCS::FPermutationDomain Permutation;
		Permutation.Set<FCSInstancedInstanceCullCS::FOutputSourceIndex>(bOutputSourceIndex);
		Permutation.Set<FCSInstancedInstanceCullCS::FCountUniform>(bCountUniform);
		TShaderMapRef<FCSInstancedInstanceCullCS> Shader(ShaderMap, Permutation);
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CSGpuInstancedInstanceCull, "CSGpuInstanced.InstanceCull");
		// 派发范围 = 布局里的源行数，不是容量（InstanceCullingOptimization.md §6.2）：CPU 实例数组的行数 CPU 自己知道，
		// 容量按 1.5 倍棘轮，多出来的那三分之一线程组一进来就返回、没必要派。GPU 源的行数只有 GPU 计数器知道，
		// 布局里记的就是容量，仍然整段派发 —— 改成间接派发要多一个建参数的 pass，而实测 70 万行空槽才 0.06–0.08 ms，不划算。
		const uint32 DispatchRows = FMath::Clamp(Layout.NumSourceInstances, 1u, Layout.InstanceCapacity);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("InstanceCull"), Shader, Params,
			FComputeShaderUtils::GetGroupCount(DispatchRows, CullGroupSize));
		CSV_CUSTOM_STAT(CSGpuInstanced, CullDispatchRows, int32(DispatchRows), ECsvCustomStatOp::Accumulate);
	}

	// Indirect args, one set per draw (a material section of a LOD). Every section of a LOD reads that
	// LOD's compaction counter, so the per-material split costs draw calls and nothing in the cull.
	{
		FCSInstancedBuildArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSInstancedBuildArgsCS::FParameters>();
		Params->RWLodCounters = LodCountersUAV;
		Params->RWIndirectArgs = IndirectArgsUAV;
		for (int32 Draw = 0; Draw < Sections.Num(); ++Draw)
		{
			const FCSGpuInstancedSection& Section = Sections[Draw];
			Params->DrawTable[Draw] = FUintVector4(Section.NumIndices, Section.FirstIndex,
				LODs[Section.LodIndex].BaseVertex, uint32(Section.LodIndex));
		}
		Params->NumDraws = uint32(Sections.Num());
		Params->MaxInstancesPerLod = Layout.InstanceCapacity;

		TShaderMapRef<FCSInstancedBuildArgsCS> Shader(ShaderMap);
		RDG_EVENT_SCOPE_STAT(GraphBuilder, CSGpuInstancedBuildArgs, "CSGpuInstanced.BuildArgs");
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("BuildArgs"), Shader, Params, FIntVector(1, 1, 1));
	}

	// One-shot diagnostics: copy the args the cull just produced and log them next frame.
	if (DiagnosticState == EDiagnosticState::Pending)
	{
		DiagnosticReadback = new FRHIGPUBufferReadback(TEXT("CSGpuInstanced.DiagArgs"));
		AddEnqueueCopyPass(GraphBuilder, DiagnosticReadback, IndirectArgs, sizeof(uint32) * IndirectArgsPerDraw * uint32(Sections.Num()));
		DiagnosticState = EDiagnosticState::Waiting;
	}
	else if (DiagnosticState == EDiagnosticState::Waiting && DiagnosticReadback && DiagnosticReadback->IsReady())
	{
		const uint32 NumArgs = IndirectArgsPerDraw * uint32(Sections.Num());
		if (const uint32* Args = static_cast<const uint32*>(DiagnosticReadback->Lock(sizeof(uint32) * NumArgs)))
		{
			for (int32 Draw = 0; Draw < Sections.Num(); ++Draw)
			{
				const uint32* A = Args + uint32(Draw) * IndirectArgsPerDraw;
				UE_LOG(LogCSGpuInstancedProxy, Log,
					TEXT("[CSGpuInstanced] %s draw %d (LOD%d, slot %d, '%s') args: IndexCount=%u Instances=%u FirstIndex=%u BaseVertex=%u (capacity %u instances, %u live, %u clusters, verts %u, indices %u)"),
					*GetOwnerName().ToString(), Draw, Sections[Draw].LodIndex, Sections[Draw].MaterialIndex, *GetNameSafe(DrawMaterials[Draw]),
					A[0], A[1], A[2], A[3],
					Layout.InstanceCapacity, Layout.NumSourceInstances, Layout.NumClusters, VertexCapacity, IndexCapacity);
			}
			DiagnosticReadback->Unlock();
		}

		delete DiagnosticReadback;
		DiagnosticReadback = nullptr;
		DiagnosticState = EDiagnosticState::Done;
	}

	// 源行号布局下，本帧后面的绘制经裸 SRV 直接读生产者那块行 buffer（顶点着色器）。RDG 不知道那些读者：
	// 不在这里交出去的话，它停在剔除 pass 留下的访问状态上，而不是绘制要的 SRVMask。常驻流那一半由下面的析构负责。
	if (bOutputSourceIndex) CSGpuInstancedHandOffProducerBuffer(GraphBuilder, ProducerRows);

	// Nothing to do here on the way out: ~FCSMeshRenderThreadEdit runs next and transitions every
	// resident stream to CSGpuMeshStreams::FinalAccessForRole's state, immediately rather than in
	// the graph epilogue — the draw that reads them is a later pass in this same graph. The
	// diagnostic copy above therefore has to stay inside the scope, since it needs the args buffer
	// while RDG still owns it.
}

// -----------------------------------------------------------------------------
// Draw
// -----------------------------------------------------------------------------

FPrimitiveViewRelevance FCSGpuInstancedMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result = FCSGpuMeshSceneProxy::GetViewRelevance(View);
	if (bShadowOnly)
	{
		// 主通道由 Nanite 替身画；这份只为把影子送进 CSM 级联（本工厂进不了 VSM，见组件构造函数）。
		// 阴影收集走 bShadowRelevance，与这四项无关 —— 2026-09-14 树的只投影副本就是这么验过的
		// （当时关的是组件的 bRenderInMainPass，效果与这里相同：影子逐像素重合、主通道一个像素都不画）。
		Result.bRenderInMainPass = false;
		Result.bRenderInDepthPass = false;
		Result.bRenderCustomDepth = false;
		Result.bVelocityRelevance = false;
	}
	return Result;
}

void FCSGpuInstancedMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;
	if (DrawDesc.IndirectArgsBuffer == nullptr || !Layout.IsValid()) return;
	if (Sections.IsEmpty() || DrawMaterials.Num() != Sections.Num()) return;

	if (!bLoggedFirstDraw)
	{
		bLoggedFirstDraw = true;
		FString MaterialNames;
		for (const UMaterialInterface* DrawMaterial : DrawMaterials) MaterialNames += (MaterialNames.IsEmpty() ? TEXT("") : TEXT(", ")) + GetNameSafe(DrawMaterial);
		UE_LOG(LogCSGpuInstancedProxy, Log,
			TEXT("[CSGpuInstanced] %s first draw: %u LODs, %d draws, materials [%s], bounds radius %.1f"),
			*GetOwnerName().ToString(), Layout.NumLODs, Sections.Num(), *MaterialNames, GetBounds().SphereRadius);
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		// CustomPrimitiveData 必须送进来：这条路不走 GPU-Scene，材质的 `GetPrimitiveData(...).CustomPrimitiveData`
		// 只读这个 uniform buffer。传 nullptr（2026-09-15 之前）会被 builder 清零，组件上
		// `SetCustomPrimitiveData*` 设多少材质都读到 0 且不报错。proxy 这份由 `FScene::UpdateCustomPrimitiveData`
		// 在渲染线程更新，每帧取当前值即可。
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), GetLocalToWorld(), GetLocalToWorld(),
			GetBounds(), GetLocalBounds(), GetLocalBounds(), ReceivesDecals(), false, false, GetCustomPrimitiveData());

		// One indirect draw per material section of each LOD. The instance count sits in the args the
		// cull pass wrote, so a LOD nobody selected costs empty draw calls and nothing else.
		for (int32 Draw = 0; Draw < Sections.Num(); ++Draw)
		{
			const uint32 Lod = uint32(Sections[Draw].LodIndex);
			FMeshBatch& Mesh = Collector.AllocateMesh();
			Mesh.VertexFactory = VertexFactory.Get();
			Mesh.MaterialRenderProxy = DrawMaterials[Draw]->GetRenderProxy();
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Mesh.CastShadow = bBatchCastShadow;

			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			BatchElement.IndexBuffer = DrawDesc.IndexBuffer;
			BatchElement.FirstIndex = 0;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = DrawDesc.MaxVertexIndex;
			BatchElement.NumPrimitives = 0; // 0 => read the count from IndirectArgsBuffer
			BatchElement.IndirectArgsBuffer = DrawDesc.IndirectArgsBuffer;
			BatchElement.IndirectArgsOffset = uint32(Draw) * IndirectArgsPerDraw * sizeof(uint32);
			// Start of this LOD's region in the visible-instance buffers; the vertex factory adds
			// SV_InstanceID to it. Every section of the LOD reads the same region. The stride is the
			// capacity the buffers were sized from, which is why the proxy copies the layout instead
			// of re-deriving it from the live count.
			BatchElement.UserIndex = int32(Lod * Layout.InstanceCapacity);
			BatchElement.LooseParametersUniformBuffer = InstancedLooseUniformBuffer;
			BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
			// This factory has no primitive-id stream (that is what puts the shader on the
			// manual-fetch instancing path), so FMeshElementCollector::AddMesh skips the GPU-Scene
			// dynamic-primitive registration that would otherwise set PrimitiveIdMode. Left at its
			// PrimID_FromPrimitiveSceneInfo default the renderer ensures on the primitive uniform
			// buffer above and drops the whole batch. The shader never reads a primitive id — it
			// reads the Primitive uniform buffer — so force it to zero.
			BatchElement.PrimitiveIdMode = PrimID_ForceZero;

			Collector.AddMesh(ViewIndex, Mesh);
		}
	}
}

void FCSGpuInstancedMeshSceneProxy::GetBatchMaterials(TArray<FMaterialRenderProxy*, TInlineAllocator<8>>& OutMaterials) const
{
	OutMaterials.Reset(DrawMaterials.Num());
	for (UMaterialInterface* DrawMaterial : DrawMaterials) OutMaterials.Add(DrawMaterial->GetRenderProxy());
}

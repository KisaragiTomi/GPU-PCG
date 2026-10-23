#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshSceneProxy.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSMesh.h" // FCSMeshResidentRef: the buffer set this proxy borrows
#include "ProfilingDebugging/CsvProfiler.h"

class FRDGBuilder;
class FSceneView;
class UCSGpuInstancedMeshComponent;

/**
 * 实例路的 CSV 统计分类（定义在 CSGpuInstancedMeshSceneProxy.cpp）。只在 `CsvProfile` 采集期间有开销，
 * 是 InstanceCullingOptimization.md §4.1 那份基线的 CPU 侧口径：
 *   CullSetupMs       渲染线程给全部经典代理建剔除图花的毫秒（每个 view family 累加）
 *   CullProxies       这一帧参加剔除的经典代理数（累加）
 *   CullDispatchRows  InstanceCull 派发覆盖的源行数之和（= 各代理的容量，不是活跃数）
 *   NaniteWrites / NaniteWriteSlots  这一帧派发的 GPU-Scene 写入 pass 数与它们覆盖的槽数
 * GPU 侧的分段耗时走 `DECLARE_GPU_STAT`（`r.GPUCsvStatsEnabled 1` 时进同一张 CSV 的 GPU/ 分类）。
 */
CSV_DECLARE_CATEGORY_EXTERN(CSGpuInstanced);

/**
 * Which extra buffer an AuxVertex stream is. A stream is addressed by (Role, TexCoordIndex) —
 * FCSGpuMeshSceneProxy::FindStream, FCSMeshResident::FindStream and FCSMeshEditContext::Find all
 * key on that pair — so the index doubles as the aux-slot id, the same trick the base already uses
 * to tell TexCoord0 from TexCoord1.
 *
 * THESE START AT 16, NOT 0, AND MUST NOT BE MOVED DOWN. AuxVertex slot 0 belongs to the retained
 * set's per-triangle material-id stream, which FCSMeshStreamLayout forces on and offers no way to
 * disable (every operator in CSMeshOps needs it). This leaf declared SourceInstances at slot 0 while
 * its buffers were proxy-owned, where no material-id stream existed; the moment the same declaration
 * went to UCSMesh the two collided. What a collision does is the reason for the gap: an aux stream
 * simply does not get registered — UCSMesh::SetStreamLayoutSync refuses the whole layout and returns
 * false, and FCSMeshResident::AddStream returns false for one stream — after which a null buffer is
 * bound at draw time, with nothing in the log pointing at the cause.
 *
 * The gap to 16 is so a later standard aux stream (they are numbered upwards from 0) cannot walk
 * into this block. TexCoordIndex is only a key for the AuxVertex role — nothing sizes an array by
 * it, and only the TexCoord role feeds FLocalVertexFactory::FDataType::NumTexCoords — so a high
 * value costs nothing.
 */
enum class ECSGpuInstancedAuxSlot : uint8
{
	SourceInstances = 16,   // Buffer<float4>, 5 per instance (see FCSGpuInstanceSourceGPU)
	ClusterBounds = 17,     // Buffer<float4>, centre.xyz + radius per cluster
	ClusterVisible = 18,    // Buffer<uint>,   coarse-cull result per cluster
	VisibleTransforms = 19, // Buffer<float4>, 3 per visible slot -> VertexFetch_InstanceTransformBuffer
	VisibleOrigins = 20,    // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceOriginBuffer
	VisibleLightmap = 21,   // Buffer<float4>, 1 per visible slot -> VertexFetch_InstanceLightmapBuffer
	LodCounters = 22,       // Buffer<uint>,   compaction cursor per LOD
	/**
	 * Buffer<float>, CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS per visible slot
	 * -> FInstancedStaticMeshVertexFactoryUniformShaderParameters::InstanceCustomDataBuffer。
	 * ⚠️ 格式**必须 PF_R32_FLOAT**：引擎按 `Buffer<float>` 取，照抄邻居那几条的
	 * float4 视图会读出错位数据，而画面上只是"数值不对"，不报错。
	 */
	VisibleCustomData = 23,

	// ---- 「可见源行号」布局（FCSGpuInstancedGpuLayout::bVisibleSourceIndex）才声明下面两条，
	//      同时**不**声明上面的 VisibleTransforms / VisibleOrigins / VisibleLightmap / VisibleCustomData ----

	/** Buffer<uint>, 1 per visible slot：InstanceCullCS 压出来的源行号 -> CSVisibleSourceIndices。 */
	VisibleSourceIndices = 24,
	/**
	 * Buffer<float>, CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS 个，恒零（常驻流分配出来就是零，没有人写它）。
	 * 生产者没给 custom data 时顶在 InstanceVF.InstanceCustomDataBuffer 上：材质读到 0，与旧布局"可见槽写零"同一口径。
	 * 格式同样必须 PF_R32_FLOAT。
	 */
	CustomDataFallback = 25,
};

/** Appends this leaf's aux stream descriptors to OutStreams, sized from Layout — the visible-attribute
 *  set, or (Layout.bVisibleSourceIndex) the visible-source-index set.
 *  bExternalPackedSource means a producer supplies the packed rows in its own buffer, so the mesh
 *  only needs a placeholder for that slot. Lives with the enum because the slot assignment and the
 *  slot numbering must never be edited apart. */
void CSGpuInstancedBuildAuxStreamDescs(
	TArray<FCSGpuStreamDesc>& OutStreams,
	const FCSGpuInstancedGpuLayout& Layout,
	bool bExternalPackedSource);

/**
 * 把**生产者自己的** buffer（packed 行 / custom data）注册进渲染器的图，供一个 pass 读。
 *
 * 与直接 `RegisterExternalBuffer` 的区别只有一步：先把它从外部访问状态拿回来。同一张图里可能已经有人把它交出去了 ——
 * 经典路的剔除、Nanite 路的 GPU-Scene 写入、另一个 view family 的剔除都读同一块 buffer —— 而外部访问状态下的资源
 * 不许再进 pass。对还在内部状态的资源这是空操作。
 */
FRDGBufferRef CSGpuInstancedRegisterProducerBuffer(FRDGBuilder& GraphBuilder, const TRefCountPtr<FRDGPooledBuffer>& Pooled);

/**
 * 读完之后把生产者的 buffer 交到外部访问状态（SRVMask），本图余下的部分 RDG 不再管它。
 *
 * 为什么要交：「可见源行号」布局下，本帧后面的绘制经裸 SRV **直接读生产者的 buffer**（顶点着色器读行，
 * 材质的顶点 / 像素着色器读 custom data），RDG 不知道那些读者；不交的话 buffer 停在上一个 compute pass 留下的
 * 访问状态上，像素着色器那一侧读的就是一个没为它转过状态的资源。图结束时它留在 SRVMask，正是生产者约定的常态。
 * 空指针忽略。
 */
void CSGpuInstancedHandOffProducerBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef Buffer);

/**
 * 点云源 -> packed 实例行（CSGpuInstancedMesh.usf 的 PackPointInstancesCS）。
 *
 * 两条路共用这一个入口：经典路每帧在剔除前打进 SourceInstances 槽，Nanite 路在 GPU-Scene 写入前
 * 打进一块临时行。抽出来是为了让"点怎么变成实例"只有一份 —— 两边各写一份的话，法线朝向或随机数
 * 哪天改了一边，另一边的画面就静默地对不上。
 *
 * WorldToComponent：点位是世界空间，行是组件局部空间。BaseSphere* 只决定第 5 行（剔除球），
 * 不读那一行的调用方传零即可。
 */
void CSGpuInstancedAddPackPointsPass(
	FRDGBuilder& GraphBuilder,
	class FGlobalShaderMap* ShaderMap,
	const FCSGpuInstancePointSourceGPU& Points,
	FRDGBufferRef InstanceCount,
	FRDGBufferRef OutPackedInstances,
	const FMatrix44f& WorldToComponent,
	const FVector3f& BaseSphereCentre,
	float BaseSphereRadius,
	uint32 MaxSourceInstances);

/**
 * Scene proxy for UCSGpuInstancedMeshComponent.
 *
 * It owns no buffers. The component's UCSMesh holds the base mesh (all LODs concatenated into one
 * vertex + one index buffer), the per-LOD indirect args and the seven aux streams; this proxy
 * adopts that set through FCSGpuMeshSceneProxy::SetExternalStreams and, every frame, runs the
 * cluster/instance cull + LOD selection passes of CSGpuInstancedMesh.usf, which compact the visible
 * instances into per-LOD regions and fill one DrawIndexedIndirect arg set per material section of
 * each LOD (all sections of a LOD share that LOD's region; each draws its own index run with its
 * own material).
 *
 * What it does NOT adopt is UCSMeshRenderComponent's proxy. That one hard-wires FLocalVertexFactory
 * through FCSMeshRenderSceneProxy, and this path exists precisely because the instance transform has
 * to be manual-fetched by SV_InstanceID out of compute-written buffers — see
 * FCSGpuInstancedMeshVertexFactory. Retention and the vertex factory are separable concerns, and the
 * base's external-streams mode is what separates them: RegisterStreams()/BuildGeometry() are never
 * called in that mode, but CreateVertexFactory() and OnStreamsAllocated() still are.
 *
 * The per-frame work has to happen inside the renderer's own graph and before the base pass, so
 * a shared FSceneViewExtension drives it; the proxy registers itself for the duration of its
 * render-thread resources. The cull runs once per frame for the first view — with split screen
 * every view therefore draws the primary view's visible set, which over-draws rather than
 * under-draws.
 */
class FCSGpuInstancedMeshSceneProxy final : public FCSGpuMeshSceneProxy
{
public:
	/** InDrawMaterials：每个材质段（= 每个 draw，与快照 Sections 同序）一张，调用方已解析好覆盖 / 资产材质
	 *  并做完用途检查（CreateSceneProxy）；空项画引擎默认材质。 */
	FCSGpuInstancedMeshSceneProxy(UCSGpuInstancedMeshComponent* Component, const FCSMeshResidentRef& InResident,
		const TArray<UMaterialInterface*>& InDrawMaterials);
	virtual ~FCSGpuInstancedMeshSceneProxy() override;

	/** Creates the shared cull view extension if it does not exist yet. Game thread. */
	static void EnsureCullServiceStarted();

	//~ FPrimitiveSceneProxy interface
	virtual SIZE_T GetTypeHash() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	/** 只投影的副本（组件的 bClassicShadowOnNanitePath，主通道由 Nanite 替身画）在这里把主通道 / 深度 / 自定义深度 /
	 *  速度相关性全部关掉，只留阴影。基类那几面旗子（bRenderInMainPass 等）是私有的，只能在相关性这一层改。 */
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;

	/** Adds this frame's cull + LOD + indirect-args passes to the renderer's graph, through the
	 *  mesh's own FCSMeshRenderThreadEdit so the resident streams end the frame in the access states
	 *  CSGpuMeshStreams::FinalAccessForRole demands. Render thread. */
	void RunCulling(FRDGBuilder& GraphBuilder, const FSceneView& View);

protected:
	//~ FCSGpuMeshSceneProxy interface
	// Never called: the base skips both hooks in external-streams mode, where the buffers already
	// exist and already hold geometry. They stay pure-virtual on the base because every
	// proxy-owning leaf still needs them.
	virtual void RegisterStreams() override {}
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) override {}
	virtual TUniquePtr<FLocalVertexFactory> CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const override;
	virtual void OnStreamsAllocated(FRHICommandListBase& RHICmdList) override;
	/** No BLAS: the resident streams hold the source mesh once and the instance transforms live in
	 *  a GPU buffer (FCSGpuInstanceSourceGPU), which a single-instance BLAS cannot express. Ray
	 *  tracing for this leaf needs GPU-side TLAS instance data and is a separate piece of work. */
	virtual bool WantsRayTracingGeometry() const override { return false; }
	/** 一个 draw 一张，与 Sections 同序（基座的 BLAS / card-capture 按下标配材质；本叶子两者都不开，补齐是为了口径一致）。 */
	virtual void GetBatchMaterials(TArray<FMaterialRenderProxy*, TInlineAllocator<8>>& OutMaterials) const override;

private:
	/** The buffer set this proxy borrows, held as the shared reference so either teardown order is
	 *  safe. The base holds the same object (SetExternalStreams) and derives its vertex-factory
	 *  bindings from it; RunCulling needs it directly, because an edit is declared over the resident
	 *  set rather than over a list of buffers somebody remembered to enumerate. */
	FCSMeshResidentRef Resident;

	/** Resident.AllocationGeneration at the moment this proxy adopted the buffers. The set can be
	 *  reallocated while a proxy is live (a capacity change, a stream resize), and the proxy then
	 *  holds the buffers it adopted while the resident set points at new ones — culling into those
	 *  would fill buffers the draw does not read, which looks exactly like a cull that found
	 *  nothing. The component marks its render state dirty on every such change, so refusing to
	 *  cull costs the frame or two before the replacement proxy arrives. */
	uint32 AdoptedAllocationGeneration = 0;

	/** 剔除要用的那几条常驻流（Resident->Streams 的下标位图）：间接参数 + 本叶子的 aux 流。基础网格的
	 *  位置 / 切线 / UV / 索引等剔除从不碰，精简建图下就不登记它们（FCSMeshRenderThreadEdit 的掩码）。OnStreamsAllocated 里算。 */
	uint64 CullStreamMask = ~0ull;

	// Game-thread snapshot; the proxy is recreated whenever any of it changes. Deliberately not the
	// whole FCSGpuInstancedBaseMesh or the packed instance array any more — those live in the mesh
	// object, and copying them per proxy is exactly the cost retention removes.
	TArray<FCSGpuInstancedLODRange> LODs;
	/** 快照的材质段，一段一个 draw（arg set 下标 = 段号）。截到 Layout.NumDraws。 */
	TArray<FCSGpuInstancedSection> Sections;
	/** 每段的材质，恒非空（空的已换成引擎默认材质）。与 Sections 同长。 */
	TArray<UMaterialInterface*> DrawMaterials;
	FVector3f BaseSphereCentre = FVector3f::ZeroVector; // base bounds, for the point-source packer
	float BaseSphereRadius = 0.0f;
	FCSGpuInstanceSourceGPU GpuSource;
	FCSGpuInstancePointSourceGPU GpuPointSource;

	float EndCullDistance = 0.0f;
	/** 近于它的实例不画（0 = 不限）；与 EndCullDistance 配成距离带（地被近 / 远两档各拿一半）。 */
	float StartCullDistance = 0.0f;
	/** 起止边界的过渡带宽：带内按逐实例随机数取边界，近 / 远两个组件互补（见组件同名属性）。 */
	float CullFadeDistance = 0.0f;
	float LodScreenSizeScale = 1.0f;
	bool bFrustumCull = true;
	bool bLodSelect = true;
	/** 本代理只是 Nanite 替身的影子副本：只进阴影（见 GetViewRelevance）。 */
	bool bShadowOnly = false;

	/** Copied from the component, never re-derived — see FCSGpuInstancedGpuLayout. */
	FCSGpuInstancedGpuLayout Layout;

	// Constant across frames; built once so GetDynamicMeshElements stays allocation-free.
	FUniformBufferRHIRef InstancedLooseUniformBuffer;

	/**
	 * 「可见源行号」布局 + packed GPU 源：顶点着色器直接读生产者那块行 buffer（与可选的 custom data），
	 * 它们不在常驻流里，SRV 得自己建。生命周期跟着本代理（GpuSource 攥着 buffer 的引用）。
	 * 其余情形（CPU 源 / 点云源）读的是常驻流 SourceInstances，用基座的 GetStreamSRV，这两条为空。
	 */
	FShaderResourceViewRHIRef ProducerRowsSRV;
	FShaderResourceViewRHIRef ProducerCustomDataSRV;

	bool bRegisteredForCulling = false;

	// One-shot diagnostics. Nothing about this path is visible from the game thread — if the cull
	// writes a zero instance count the draw is simply a no-op and the log stays silent — so the
	// first frame's indirect args are copied back and logged once per proxy.
	enum class EDiagnosticState : uint8 { Pending, Waiting, Done };
	EDiagnosticState DiagnosticState = EDiagnosticState::Pending;
	class FRHIGPUBufferReadback* DiagnosticReadback = nullptr;
	mutable bool bLoggedFirstDraw = false;
};

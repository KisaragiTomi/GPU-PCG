#pragma once

#include "CoreMinimal.h"
#include "PrimitiveSceneProxy.h"
#include "LocalVertexFactory.h"
#include "RenderResource.h"
#include "RenderGraphResources.h"
#include "CSGpuMeshTypes.h"

class UPrimitiveComponent;
class UMaterialInterface;
class FRHIGPUBufferReadback;
class FRayTracingGeometry;
class FRayTracingInstanceCollector;
class FMaterialRenderProxy;
class FCardRepresentationData;

/**
 * Base scene proxy that draws a GPU-resident mesh directly through the render
 * pipeline: it owns an FLocalVertexFactory over GPU buffers and submits one
 * FMeshBatch per view in GetDynamicMeshElements. Vertex/index data lives only on
 * the GPU — nothing is read back to the CPU (except an explicit, opt-in save) and
 * no UDynamicMesh is involved.
 *
 * The buffer set is DESCRIPTOR-DRIVEN. A subclass supplies geometry by implementing
 * two hooks:
 *   RegisterStreams()  — push FCSGpuStreamDesc entries (usually AddStandardTriangleStreams())
 *                        and set VertexCapacity/IndexCapacity.
 *   BuildGeometry()    — register the base's pooled buffers into an FRDGBuilder and run
 *                        the leaf's compute passes that fill them (incl. MeshCounters).
 * The base then owns everything shared by every GPU mesh (roads, the compute-shader mesh
 * generator, ...): buffer allocation, vertex-factory binding, the draw path, teardown,
 * and the CPU readback used by save-to-StaticMesh. Adding a new buffer is one more
 * AddStream(...) in RegisterStreams(); the alloc / VF-bind / readback code is untouched.
 *
 * Ray tracing: the base also owns a BLAS over the Position and Index streams, so hardware Lumen,
 * ray traced shadows and reflections see the mesh. It is built from the draw-args mirror the
 * resident set publishes (the only CPU copy of a GPU-decided triangle count) by an end-of-frame
 * pump, and rebuilt whenever a new readback lands. See RefreshRayTracingGeometry for the rules.
 *
 * Lumen surface cache: a BLAS alone only occludes. Hardware Lumen shades a ray hit by looking the
 * hit primitive up in its surface cache, and a primitive without mesh cards comes back black —
 * measured 2026-09-11, hits on GPU meshes returned no bounce at all. So the base also hands Lumen
 * a card set (six axis-aligned cards over the local bounds, the skeletal-mesh recipe) and one
 * card-capture batch per draw batch. See DrawStaticElements for why those are static batches that
 * nothing but the card capture ever draws.
 */
class COMPUTESHADERGENERATOR_API FCSGpuMeshSceneProxy : public FPrimitiveSceneProxy
{
public:
	FCSGpuMeshSceneProxy(const UPrimitiveComponent* Component, UMaterialInterface* InMaterial, const char* DebugName);
	virtual ~FCSGpuMeshSceneProxy() override;

	// Heap-owned and never copied; the buffer registry (TArray<TUniquePtr<...>>) is move-only,
	// so deleting the copy operations keeps MSVC from trying to synthesise a copy-assignment.
	FCSGpuMeshSceneProxy(const FCSGpuMeshSceneProxy&) = delete;
	FCSGpuMeshSceneProxy& operator=(const FCSGpuMeshSceneProxy&) = delete;

	//~ FPrimitiveSceneProxy interface
	virtual uint32 GetMemoryFootprint() const override;
	virtual void CreateRenderThreadResources(FRHICommandListBase& RHICmdList) override;
	virtual void DestroyRenderThreadResources() override;
	virtual void GetDynamicMeshElements(const TArray<const FSceneView*>& Views, const FSceneViewFamily& ViewFamily,
		uint32 VisibilityMap, FMeshElementCollector& Collector) const override;
	virtual FPrimitiveViewRelevance GetViewRelevance(const FSceneView* View) const override;
	virtual bool CanBeOccluded() const override;
	// GetTypeHash() stays pure-virtual: each concrete proxy must return its own unique hash.

	//~ Lumen surface cache
	/** Six cards over the local bounds, or null while the proxy takes no part in the surface cache.
	 *  Lumen reads this once, when it builds the primitive's card set, and afterwards only moves
	 *  those cards — a proxy whose geometry outgrew them has to be replaced, which is the owning
	 *  component's call (UCSMeshRenderComponent does it when a landed readback finds new bounds). */
	virtual const FCardRepresentationData* GetMeshCardRepresentation() const override;

	/** The card-capture batches: one direct draw per draw batch, counts from the published mirror.
	 *
	 *  Static rather than dynamic because the card capture has no dynamic path at all — it walks
	 *  the primitive's cached static mesh draw commands (LumenSceneCardCapture.cpp:856 in 5.7.4).
	 *  Nothing else draws them: GetViewRelevance never reports static relevance, and every other
	 *  pass only visits a static mesh through that flag (SceneVisibility.cpp:1512).
	 *
	 *  Direct, not indirect, for the same reason the VSM shadow batch is: cached commands go through
	 *  GPU-Scene instance culling, which rebuilds the args from the CPU-side NumPrimitives. While the
	 *  counts are unknown — first readback pending, or an edit in flight — nothing is registered at
	 *  all: an empty capture is replaced on the next refresh, a capture of stale counts over fresh
	 *  indices would bake scrambled triangles into the cards. The engine calls this again on every
	 *  transform update (RendererScene.cpp:5956-5960), which is how a refresh reaches it. */
	virtual void DrawStaticElements(FStaticPrimitiveDrawInterface* PDI) override;

	/** Keeps the card set's bounds in step with the proxy's local bounds. */
	virtual void OnTransformChanged(FRHICommandListBase& RHICmdList) override;

	/** r.CSGpuMesh.SurfaceCache. Any thread. */
	static bool IsSurfaceCacheEnabled();

	/** GetDrawArgsPublishSerial() value the registered card-capture batches were built from;
	 *  0 = none registered. Written from DrawStaticElements, so read it on the render thread
	 *  after the scene has collected the static batches. For tests. */
	uint32 GetSurfaceCacheBatchSerialForTest() const { return SurfaceCacheBatchSerial; }

#if RHI_RAYTRACING
	//~ Ray tracing. Relevance follows WantsRayTracingGeometry(); a leaf that cannot be expressed as
	//  one BLAS instance (the GPU-instanced leaf) opts out there rather than here.
	virtual bool IsRayTracingRelevant() const override { return WantsRayTracingGeometry(); }
	virtual bool HasRayTracingRepresentation() const override { return WantsRayTracingGeometry(); }
	virtual void GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector) override;

	/** The current BLAS, or null while no counts are known. Render thread only; for tests. */
	const FRayTracingGeometry* GetRayTracingGeometryForTest() const { return RayTracingGeometry.Get(); }
	/** GetDrawArgsPublishSerial() value the current BLAS was built from; 0 = never built. For tests. */
	uint32 GetRayTracingBuiltSerialForTest() const { return RayTracingBuiltSerial; }

	/** One pass of the end-of-frame BLAS refresh over every live proxy. Production code reaches it
	 *  only through the OnEndFrameRT pump; tests call it directly because a synchronous test never
	 *  reaches the end of a frame. Render thread only. */
	static void PumpRayTracingGeometries_RenderThread();
#endif

	/** Module start / stop hooks for the end-of-frame BLAS pump. No-ops without RHI_RAYTRACING. */
	static void RegisterRayTracingPump();
	static void UnregisterRayTracingPump();

	// -------------------------------------------------------------------------
	// Readback API (used by UCSGpuMeshComponent::ReadbackMeshSync). Render thread only.
	// -------------------------------------------------------------------------

	/** Snapshots this proxy's stream set as a plain FCSMeshResident so the shared readback
	 *  (CSMeshReadback::ReadbackResidentSync) can consume it. The proxy keeps owning the
	 *  buffers; the view only adds references. Render thread only.
	 *
	 *  This is what makes readback independent of the proxy: the counter read and the
	 *  descriptor loop used to be proxy methods, so a GPU mesh that was not being drawn
	 *  could not be saved. The proxy is now just one possible source of a resident set. */
	void BuildResidentView(struct FCSMeshResident& OutResident) const;

	/** Submit an indexed GPU-buffer draw for every visible view. This is shared by the base
	 *  triangle path and leaf-owned debug geometry; it never maps or reads either buffer.
	 *
	 *  ShadowArgs is the escape hatch for virtual shadow maps and is used for NOTHING else. A VSM
	 *  refuses to draw from the args buffer this path normally hands it — its non-Nanite raster
	 *  goes through GPU-Scene instance culling, which substitutes args built on the CPU from
	 *  FMeshDrawCommand::NumPrimitives, and a batch carrying an IndirectArgsBuffer must report
	 *  NumPrimitives as 0. So a shadow-depth view gets a direct-draw batch built from this CPU
	 *  copy of the arg set instead, and every other view keeps the exact GPU-decided indirect
	 *  draw. Pass null (the default) when no copy is available: the shadow batch then falls back
	 *  to the indirect form, which is today's behaviour — correct everywhere except in a VSM. */
	static void SubmitGpuBufferDraw(
		const FPrimitiveSceneProxy& SceneProxy,
		const TArray<const FSceneView*>& Views,
		uint32 VisibilityMap,
		FMeshElementCollector& Collector,
		const FVertexFactory& InVertexFactory,
		FMaterialRenderProxy& MaterialProxy,
		const FIndexBuffer& IndexBuffer,
		EPrimitiveType PrimitiveType,
		uint32 NumPrimitives,
		uint32 MaxVertexIndex,
		bool bCastShadow = false,
		FRHIBuffer* IndirectArgsBuffer = nullptr,
		uint32 IndirectArgsOffset = 0,
		const FCSGpuDrawArgs* ShadowArgs = nullptr);

protected:
	// The pooled-buffer render-resource wrappers live in CSGpuMeshTypes.h so debug geometry
	// (FCSGpuDebugDraw) and non-derived proxies can use the same types; these aliases keep the
	// long-standing member/leaf spelling.
	using FPooledVertexBuffer = FCSPooledVertexBuffer;
	using FPooledIndexBuffer = FCSPooledIndexBuffer;

	// One entry in the descriptor-driven buffer set. Heap-allocated (TUniquePtr in the
	// Streams array) so &VB / &IB stay stable for the vertex-factory streams even as the
	// array grows — FVertexStreamComponent stores the FVertexBuffer pointer.
	struct FCSGpuStreamRuntime
	{
		FCSGpuStreamDesc Desc;
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		FShaderResourceViewRHIRef SRV;
		FPooledVertexBuffer VB;
		FPooledIndexBuffer IB;
	};

	// Everything GetDynamicMeshElements needs to describe one draw. AllocateStreamsAndBindVF
	// fills this from the registered streams; the base reads it every frame. Set bValid last.
	struct FDrawDesc
	{
		// Index buffer to draw with. Null => the base skips the draw (this proxy path
		// always draws indexed; non-indexed geometry supplies an identity index buffer).
		FIndexBuffer* IndexBuffer = nullptr;
		// When set, the draw uses DrawIndexedIndirect with these args; NumPrimitives is
		// ignored. When null, the base issues a direct indexed draw of NumPrimitives.
		FRHIBuffer* IndirectArgsBuffer = nullptr;
		uint32 IndirectArgsOffset = 0;
		uint32 NumPrimitives = 0;
		uint32 FirstIndex = 0;
		uint32 MinVertexIndex = 0;
		uint32 MaxVertexIndex = 0;
		bool bValid = false;
	};

	// -------------------------------------------------------------------------
	// Leaf hooks (pure-virtual)
	// -------------------------------------------------------------------------

	/** Push the buffer descriptors (typically AddStandardTriangleStreams()) and set
	 *  VertexCapacity / IndexCapacity. Called first from InitGpuGeometry(). */
	virtual void RegisterStreams() = 0;

	/** Register the base's pooled buffers into an FRDGBuilder and run the compute passes
	 *  that fill them (positions/tangents/uv/color/indices/indirect args/MeshCounters).
	 *  Called after the base has allocated the buffers and bound the vertex factory. */
	virtual void BuildGeometry(FRHICommandListBase& RHICmdList) = 0;

	// -------------------------------------------------------------------------
	// Base services callable by leaves
	// -------------------------------------------------------------------------

	/** Bind an externally-owned resident buffer set instead of allocating one.
	 *
	 *  Call from a leaf's constructor. It switches InitGpuGeometry from
	 *  "RegisterStreams -> allocate -> BuildGeometry" to "adopt the given buffers -> bind the
	 *  vertex factory", so a render-state recreation stops re-running the leaf's generation
	 *  compute and becomes a rebind. RegisterStreams()/BuildGeometry() are not called at all
	 *  in this mode; the proxy holds a reference to the set, so the buffers outlive the mesh
	 *  object being garbage-collected mid-frame. */
	void SetExternalStreams(TSharedPtr<struct FCSMeshResident, ESPMode::ThreadSafe> InResident);

	/** Register one stream. Call from RegisterStreams(). */
	void AddStream(const FCSGpuStreamDesc& Desc);

	/** SRV of a registered stream (only streams whose desc set SrvFormat have one). */
	FRHIShaderResourceView* GetStreamSRV(ECSGpuStreamRole Role, uint8 Index = 0) const;

	/** One DrawIndexedIndirect arg set as the CPU last saw it; false when it is not known. Only
	 *  the external-streams mode can have one: it lives on the resident set, refreshed a few
	 *  frames behind every edit (FCSMeshResident::GetDrawArgs). Feed it to SubmitGpuBufferDraw's
	 *  ShadowArgs and nowhere else — it is stale by construction, and every pass except a virtual
	 *  shadow map already reads the exact values out of the args buffer. */
	bool GetShadowDrawArgs(int32 ArgSetIndex, FCSGpuDrawArgs& OutArgs) const;

	// -------------------------------------------------------------------------
	// Vertex-factory hooks
	// -------------------------------------------------------------------------

	/** Create the vertex factory this proxy draws with. Called on the render thread from
	 *  InitGpuGeometry, so leaves can return an FLocalVertexFactory subclass with extra
	 *  streams (the instanced leaf returns one that manual-fetches per-instance transforms). */
	virtual TUniquePtr<FLocalVertexFactory> CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const;

	/** Called after every stream's pooled buffer + SRV exists but before the vertex factory is
	 *  given its data and initialised — the one point where a leaf can hand its own SRVs to
	 *  the vertex factory it created. */
	virtual void OnStreamsAllocated(FRHICommandListBase& RHICmdList) {}

	/** Whether this proxy's geometry is one mesh drawn once, which is what a BLAS instance can
	 *  express. The GPU-instanced leaf returns false: its buffers hold the source mesh once and the
	 *  instance transforms live in a GPU buffer, so a BLAS built here would put a single phantom
	 *  copy of the source mesh at the component origin. Read at scene-add time and by the pump. */
	virtual bool WantsRayTracingGeometry() const { return true; }

	/** The material of every draw batch, batch i drawing from arg set i — the same split
	 *  GetDynamicMeshElements uses, restated for everything that mirrors those batches outside the
	 *  mesh-element gather: each BLAS segment and each card-capture batch gets the material of the
	 *  batch it stands for. The base draws one batch of Material; a leaf with a section table returns
	 *  one entry per section. Render thread, or the parallel static-mesh gather. */
	virtual void GetBatchMaterials(TArray<FMaterialRenderProxy*, TInlineAllocator<8>>& OutMaterials) const;

	// Shared vertex factory; created by CreateVertexFactory() and configured by the base from
	// the registered streams. Heap-held so leaves can substitute a subclass.
	TUniquePtr<FLocalVertexFactory> VertexFactory;
	const char* VertexFactoryDebugName = "FCSGpuMeshSceneProxy";

	UMaterialInterface* Material = nullptr;
	FMaterialRelevance MaterialRelevance;
	FDrawDesc DrawDesc;

	// Set by the leaf in RegisterStreams(). Soup: IndexCapacity == VertexCapacity;
	// indexed (road): independent.
	uint32 VertexCapacity = 0;
	uint32 IndexCapacity = 0;

	// Batch-level flag mirrored from the component; subclasses may flip it before the
	// proxy is registered. Actual shadow casting is still gated by the component's
	// CastShadow / the proxy's shadow relevance.
	bool bBatchCastShadow = true;

private:
	/** Base orchestrator. Owned mode: RegisterStreams() -> AllocateStreamsAndBindVF() ->
	 *  BuildGeometry(). External mode: adopt the resident set -> bind only. */
	void InitGpuGeometry(FRHICommandListBase& RHICmdList);
	/** Release SRVs then pooled buffers for every stream (after the VF is released).
	 *  In external mode this only drops this proxy's references; the set outlives it. */
	void ReleaseGpuGeometry();

	/** Create SRVs / render-resource wrappers for every stream, bind the vertex-factory
	 *  streams, and fill DrawDesc's index/indirect handles. bAllocateBuffers=false expects
	 *  every stream's pooled buffer to already be set (the adopt path). */
	void AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList, bool bAllocateBuffers = true);

	/** Externally-owned buffer set, when this proxy borrows instead of allocating. */
	TSharedPtr<struct FCSMeshResident, ESPMode::ThreadSafe> ExternalResident;

	/** Find a stream runtime by role (+ TexCoord index for the TexCoord role). */
	const FCSGpuStreamRuntime* FindStream(ECSGpuStreamRole Role, uint8 Index = 0) const;

	TArray<TUniquePtr<FCSGpuStreamRuntime>> Streams;

	/** One arg set per draw batch, from the published mirror (external mode) or from the direct
	 *  draw description (a leaf that draws a CPU-known count). False while nothing is known yet.
	 *  OutSerial identifies the publication the args came from and is never 0 on success. Shared by
	 *  the BLAS and the card-capture batches — the two consumers that need a count on the CPU. */
	bool GatherDrawArgs(TArray<FCSGpuDrawArgs, TInlineAllocator<8>>& OutArgs, uint32& OutSerial) const;

	/** Whether this proxy takes part in the Lumen surface cache: the CVar is on, Lumen tracks the
	 *  primitive (hardware Lumen only — this proxy has no distance field for the software path), and
	 *  the geometry is one mesh drawn once, the same condition the BLAS has. */
	bool WantsSurfaceCache() const;

	/** Rebuilds CardRepresentation from the current local bounds; drops it when the proxy takes no
	 *  part in the surface cache. Render thread. */
	void UpdateCardRepresentation();

	/** Six axis-aligned cards over the local bounds; null when WantsSurfaceCache() is false. */
	TUniquePtr<FCardRepresentationData> CardRepresentation;

	/** See GetSurfaceCacheBatchSerialForTest. */
	uint32 SurfaceCacheBatchSerial = 0;

#if RHI_RAYTRACING
	/** Rebuilds the BLAS when the published draw args have advanced since the last build, drops
	 *  it when ray tracing is off or the mesh is empty, and keeps it while a readback is in flight.
	 *  Called by the end-of-frame pump on the render thread. */
	void RefreshRayTracingGeometry(FRHICommandListBase& RHICmdList);
	void ReleaseRayTracingGeometry();

	/** Owned BLAS over the Position and Index streams; null until the first publish lands. */
	TUniquePtr<FRayTracingGeometry> RayTracingGeometry;
	/** Publication the current BLAS (or the current "empty, nothing to build") came from. */
	uint32 RayTracingBuiltSerial = 0;
	/** Arg sets the current BLAS was built from, in segment order. The instance's mesh batches
	 *  must describe these, not whatever the mirror holds by the time the gather runs. */
	TArray<FCSGpuDrawArgs, TInlineAllocator<8>> RayTracingBuiltArgs;
#endif
};

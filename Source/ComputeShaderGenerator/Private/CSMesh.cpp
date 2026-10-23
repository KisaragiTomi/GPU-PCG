#include "CSMesh.h"

#include <atomic>

#include "CSGpuMemoryBudget.h"
#include "Async/Async.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CoreDelegates.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "RHICommandList.h"
#include "RHIGPUReadback.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSMesh, Log, All);

// 阻塞刷新的进程级计数器。每一次 FlushRenderingCommands 都要走 CSMesh_CountedFlush ——
// 直接调 FlushRenderingCommands 会让计数器悄悄失真，而它正是"交互热路径零设备同步"这条
// 纪律唯一的自动化防线。
static std::atomic<int64> GCSMeshBlockingFlushCount{ 0 };

static void CSMesh_CountedFlush()
{
	GCSMeshBlockingFlushCount.fetch_add(1, std::memory_order_relaxed);
	FlushRenderingCommands();
}

namespace
{
// Unity/jumbo builds share a translation unit, so every file-local helper in this module
// carries a CSMesh_ prefix (the CSMB_ collision lesson).

/** One triangle. Sizing a stream from zero would ask the RHI for a zero-byte buffer. */
constexpr uint32 CSMesh_MinCapacity = 3u;

/** DrawIndexedIndirect: IndexCountPerInstance, InstanceCount, StartIndexLocation,
 *  BaseVertexLocation, StartInstanceLocation. Must match BuildStandardTriangleStreamDescs. */
constexpr uint32 CSMesh_IndirectArgsUintsPerSet = 5u;

/** Ceiling on arg sets (1.25 MiB of args). Far past any plausible per-LOD or per-material
 *  count, so a request above it is an unclamped count that came out of a GPU buffer, not a
 *  workload — and turning that into a multi-gigabyte allocation helps nobody. */
constexpr uint32 CSMesh_MaxIndirectArgSets = 65536u;

float CSMesh_UnpackSnorm8(uint32 PackedValue, uint32 ByteIndex)
{
	const int8 SignedValue = static_cast<int8>((PackedValue >> (ByteIndex * 8u)) & 0xffu);
	return FMath::Clamp(float(SignedValue) / 127.0f, -1.0f, 1.0f);
}

FVector3f CSMesh_UnpackSnorm8888XYZ(uint32 PackedValue)
{
	return FVector3f(
		CSMesh_UnpackSnorm8(PackedValue, 0),
		CSMesh_UnpackSnorm8(PackedValue, 1),
		CSMesh_UnpackSnorm8(PackedValue, 2)).GetSafeNormal();
}

/** Restores every stream's per-role access state and executes. The owned-graph half of the
 *  discipline; FCSMeshEditContext::RestoreStreamAccess is where it actually lives, so the
 *  render-thread path cannot drift away from it. */
void CSMesh_FinalizeGraph(FRDGBuilder& GraphBuilder, FCSMeshEditContext& Context)
{
	// Every owned-graph edit ends here, which makes this the one place that cannot forget to
	// refresh the CPU-side copy of the arg sets' index counts — the counts a virtual shadow map
	// needs because it will not read the GPU ones (FCSMeshResident::GetArgIndexCount).
	//
	// Before RestoreStreamAccess, so the copy is an ordinary pass in this graph: RDG transitions
	// the args to CopySrc for it and back to IndirectArgs in the epilogue. Added after, the copy
	// would be reading a buffer the epilogue has already handed away.
	Context.Resident.RequestDrawArgsReadback(GraphBuilder, Context.IndirectArgs());
	Context.RestoreStreamAccess();
	GraphBuilder.Execute();
}

/** Zeroes the counters and the indirect args: draws nothing, reads back as empty. */
void CSMesh_AddClearCountersPasses(FCSMeshEditContext& Context)
{
	if (FRDGBufferRef Counters = Context.Counters())
	{
		AddClearUAVPass(Context.GraphBuilder, Context.GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counters, PF_R32_UINT)), 0u);
	}
	if (FRDGBufferRef Args = Context.IndirectArgs())
	{
		AddClearUAVPass(Context.GraphBuilder, Context.GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Args, PF_R32_UINT)), 0u);
	}
	Context.SetKnownCounts(0, 0);
	// The batches described geometry that is now gone, and the arg sets they index have just
	// been zeroed. Keeping the table would turn "the mesh is empty" into "the mesh draws
	// nothing for a reason nobody can find".
	Context.InvalidateSections();
}

/** How many units of its CountSource a stream covers at those capacities. Fixed (indirect
 *  args, counters) resolves to one, so ElementsPerUnit is the whole count. */
uint32 CSMesh_StreamUnits(const FCSGpuStreamDesc& Desc, uint32 VertUnits, uint32 IdxUnits)
{
	return FMath::Max(CSGpuMeshStreams::UnitsForCountSource(Desc.CountSource, VertUnits, IdxUnits), 1u);
}

uint64 CSMesh_StreamBytes(const FCSGpuStreamDesc& Desc, uint32 VertUnits, uint32 IdxUnits)
{
	return uint64(Desc.BytesPerElement) * uint64(Desc.ElementsPerUnit) * uint64(CSMesh_StreamUnits(Desc, VertUnits, IdxUnits));
}

FRDGBufferDesc CSMesh_MakeStreamBufferDesc(const FCSGpuStreamDesc& Desc, uint32 VertUnits, uint32 IdxUnits)
{
	if (Desc.Role == ECSGpuStreamRole::IndirectArgs) return FRDGBufferDesc::CreateIndirectDesc(Desc.BytesPerElement, Desc.ElementsPerUnit);

	FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateBufferDesc(
		Desc.BytesPerElement, Desc.ElementsPerUnit * CSMesh_StreamUnits(Desc, VertUnits, IdxUnits));
	if (Desc.Role == ECSGpuStreamRole::Index) BufferDesc.Usage = (BufferDesc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
	return BufferDesc;
}

/** The resident set's standard descriptors. Both extras are forced on rather than exposed:
 *  every operator in CSMeshOps requires the material-id stream, and a retained mesh that ends
 *  up being saved wants its vertex colours to survive the trip. */
void CSMesh_BuildStandardDescs(TArray<FCSGpuStreamDesc>& OutDescs, uint32 NumIndirectDraws, uint32 NumTexCoordSets = 1)
{
	CSGpuMeshStreams::FStandardStreamOptions Options;
	Options.NumIndirectDraws = FMath::Max(NumIndirectDraws, 1u);
	Options.bMaterialIds = true;
	Options.bReadbackColors = true;
	Options.NumTexCoordSets = FMath::Clamp(NumTexCoordSets, 1u, uint32(FCSGpuMeshCPUData::MaxTexCoordChannels));
	CSGpuMeshStreams::BuildStandardTriangleStreamDescs(OutDescs, Options);
}

/** A descriptor that would allocate nothing is refused up front: the buffer would come back
 *  null and the mesh would simply report itself unallocated, with no hint why. */
bool CSMesh_IsStreamDescUsable(const FCSGpuStreamDesc& Desc)
{
	if (Desc.BytesPerElement != 0 && Desc.ElementsPerUnit != 0) return true;
	UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream '%s' rejected: %u bytes x %u elements allocates nothing."),
		Desc.DebugName, Desc.BytesPerElement, Desc.ElementsPerUnit);
	return false;
}

/** Appends one descriptor, refusing a (Role, TexCoordIndex) pair that is already taken. Find()
 *  returns the first match, so a duplicate does not fail — it shadows the stream it collides
 *  with, and the shadowed one is then never written by anything. */
bool CSMesh_AppendUniqueStreamDesc(TArray<FCSGpuStreamDesc>& Descs, const FCSGpuStreamDesc& Desc)
{
	if (!CSMesh_IsStreamDescUsable(Desc)) return false;
	for (const FCSGpuStreamDesc& Existing : Descs)
	{
		if (Existing.Role != Desc.Role || Existing.TexCoordIndex != Desc.TexCoordIndex) continue;
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream '%s' rejected: role %u slot %u is already held by '%s'."),
			Desc.DebugName, uint32(Desc.Role), uint32(Desc.TexCoordIndex), Existing.DebugName);
		return false;
	}
	Descs.Add(Desc);
	return true;
}

/** True when a declaration would allocate and address exactly the streams the set already has,
 *  which lets a consumer re-declare its layout on every bind without paying for a realloc. */
bool CSMesh_LayoutMatches(const TArray<FCSMeshResident::FStream>& Streams, const TArray<FCSGpuStreamDesc>& Descs)
{
	if (Streams.Num() != Descs.Num()) return false;
	for (int32 Index = 0; Index < Descs.Num(); ++Index)
	{
		const FCSGpuStreamDesc& Current = Streams[Index].Desc;
		const FCSGpuStreamDesc& Wanted = Descs[Index];
		if (Current.Role != Wanted.Role || Current.TexCoordIndex != Wanted.TexCoordIndex) return false;
		if (Current.BytesPerElement != Wanted.BytesPerElement || Current.ElementsPerUnit != Wanted.ElementsPerUnit) return false;
		if (Current.CountSource != Wanted.CountSource) return false;
	}
	return true;
}

int64 CSMesh_RequiredBytesForDescs(const TArray<FCSGpuStreamDesc>& Descs, uint32 InVertexCapacity, uint32 InIndexCapacity)
{
	const uint32 VertUnits = FMath::Max(InVertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(InIndexCapacity, CSMesh_MinCapacity);
	int64 Bytes = 0;
	for (const FCSGpuStreamDesc& Desc : Descs) Bytes += int64(CSMesh_StreamBytes(Desc, VertUnits, IdxUnits));
	return Bytes;
}

/**
 * Reallocates the resident set at a new capacity and/or a new declaration, copying across
 * whatever survives. Old and new streams are matched by (role, slot), so a stream the layout
 * change did not touch keeps its contents, and a capacity change copies the surviving prefix —
 * which is what makes growing *and* shrinking non-destructive.
 *
 * Render thread. Ends with CSMesh_FinalizeGraph: this is the allocation path, the one
 * sanctioned exception to "never write a resident stream outside EditMeshSync".
 */
void CSMesh_ReallocateResidentWithDescs(
	FRHICommandListImmediate& RHICmdList,
	FCSMeshResident& Resident,
	const TArray<FCSGpuStreamDesc>& NewDescs,
	uint32 NewVertexCapacity,
	uint32 NewIndexCapacity)
{
	check(IsInRenderingThread());

	// Copying the stream array is what keeps the old pooled buffers referenced until the graph
	// that reads them has executed.
	const TArray<FCSMeshResident::FStream> OldStreams = Resident.Streams;
	const uint32 OldVertUnits = FMath::Max(Resident.VertexCapacity, CSMesh_MinCapacity);
	const uint32 OldIdxUnits = FMath::Max(Resident.IndexCapacity, CSMesh_MinCapacity);

	Resident.Streams.Reset(NewDescs.Num());
	for (const FCSGpuStreamDesc& Desc : NewDescs) Resident.Streams.AddDefaulted_GetRef().Desc = Desc;
	Resident.VertexCapacity = FMath::Max(NewVertexCapacity, CSMesh_MinCapacity);
	Resident.IndexCapacity = FMath::Max(NewIndexCapacity, CSMesh_MinCapacity);
	Resident.AllocateBuffers();

	const uint32 NewVertUnits = Resident.VertexCapacity;
	const uint32 NewIdxUnits = Resident.IndexCapacity;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.Reallocate"));
	FCSMeshEditContext Context(GraphBuilder, Resident);
	for (int32 NewIndex = 0; NewIndex < Resident.Streams.Num(); ++NewIndex)
	{
		const FCSGpuStreamDesc& NewDesc = Resident.Streams[NewIndex].Desc;
		FRDGBufferRef Dst = Context.StreamBuffers[NewIndex];
		if (!Dst) continue;

		// Pooled buffers arrive holding the previous tenant's bytes. For most streams that is
		// merely stale data beyond the counts; for the indirect args it is a draw call built
		// from someone else's numbers, so those are zeroed before anything is written back.
		if (NewDesc.Role == ECSGpuStreamRole::IndirectArgs) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst, PF_R32_UINT)), 0u);

		const FCSMeshResident::FStream* Old = nullptr;
		for (const FCSMeshResident::FStream& Candidate : OldStreams)
		{
			if (Candidate.Desc.Role != NewDesc.Role || Candidate.Desc.TexCoordIndex != NewDesc.TexCoordIndex) continue;
			Old = &Candidate;
			break;
		}
		// A stream the layout adds, and a stream whose per-unit stride changed, come back ZEROED
		// instead of holding the pool's previous tenant. The prefix copy below only preserves meaning
		// when both sides lay their units out the same way: EnsureTexCoordSets widening the
		// interleaved UV stream from 2 to 4 floats would land vertex 1's UV0 on vertex 0's UV1, so
		// that case is zeroed too and the caller re-uploads it — which every caller of a widening
		// already does (2026-09-07 review B8). Fixed-count streams (indirect args, counters, the
		// instanced leaf's aux slots) keep the prefix copy: their element count is the whole buffer,
		// not a stride, and set 0 / the counters survive a set-count change on purpose.
		const bool bStrideChanged = Old && NewDesc.CountSource != ECSGpuCountSource::Fixed
			&& (Old->Desc.BytesPerElement != NewDesc.BytesPerElement
				|| Old->Desc.ElementsPerUnit != NewDesc.ElementsPerUnit
				|| Old->Desc.CountSource != NewDesc.CountSource);
		if (!Old || !Old->Pooled.IsValid() || bStrideChanged)
		{
			if (NewDesc.Role != ECSGpuStreamRole::IndirectArgs) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst, PF_R32_UINT)), 0u);
			continue;
		}

		const uint64 CopyBytes = FMath::Min(
			CSMesh_StreamBytes(Old->Desc, OldVertUnits, OldIdxUnits),
			CSMesh_StreamBytes(NewDesc, NewVertUnits, NewIdxUnits));
		if (CopyBytes == 0) continue;

		FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(Old->Pooled, Old->Desc.DebugName);
		AddCopyBufferPass(GraphBuilder, Dst, 0, Src, 0, CopyBytes);
	}
	CSMesh_FinalizeGraph(GraphBuilder, Context);
}

/** Same, keeping the current declaration: the capacity is all that changes. */
void CSMesh_ReallocateResident(
	FRHICommandListImmediate& RHICmdList, FCSMeshResident& Resident, uint32 NewVertexCapacity, uint32 NewIndexCapacity)
{
	TArray<FCSGpuStreamDesc> Descs;
	Descs.Reserve(Resident.Streams.Num());
	for (const FCSMeshResident::FStream& Stream : Resident.Streams) Descs.Add(Stream.Desc);
	CSMesh_ReallocateResidentWithDescs(RHICmdList, Resident, Descs, NewVertexCapacity, NewIndexCapacity);
}

/**
 * Reallocates the named streams and nothing else: every other stream keeps its pooled buffer, so
 * neither its identity nor its contents move. The generic form of CSMesh_ReallocateIndirectArgs,
 * and the reason changing one aux stream's size does not drag the base-mesh geometry through a
 * copy the way a re-declaration does.
 *
 * The new buffers are zeroed and the old contents are not copied across — see
 * UCSMesh::ResizeStreamsSync for why that is the contract rather than an omission.
 *
 * Render thread. Every entry has already been validated on the game thread; anything that would
 * be a no-op here is filtered out before the enqueue, so the graph is never built for nothing.
 */
void CSMesh_ReallocateStreams(
	FRHICommandListImmediate& RHICmdList, FCSMeshResident& Resident, const TArray<FCSMeshStreamResize>& Resizes)
{
	check(IsInRenderingThread());

	const uint32 VertUnits = FMath::Max(Resident.VertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(Resident.IndexCapacity, CSMesh_MinCapacity);

	TArray<int32, TInlineAllocator<8>> ResizedStreams;
	for (const FCSMeshStreamResize& Resize : Resizes)
	{
		const int32 StreamIndex = Resident.FindStreamIndex(Resize.Role, Resize.SlotIndex);
		if (StreamIndex == INDEX_NONE) continue;

		FCSMeshResident::FStream& Stream = Resident.Streams[StreamIndex];
		if (Stream.Desc.ElementsPerUnit == Resize.ElementCount) continue;

		// The old TRefCountPtr is dropped here, on the render thread, only so the pool sees it after
		// the commands already queued; the pool itself keeps it alive for frames in flight (review B7).
		Stream.Desc.ElementsPerUnit = Resize.ElementCount;
		Stream.Pooled = AllocatePooledBuffer(
			CSMesh_MakeStreamBufferDesc(Stream.Desc, VertUnits, IdxUnits), Stream.Desc.DebugName);
		ResizedStreams.Add(StreamIndex);
	}
	if (ResizedStreams.Num() == 0) return;

	Resident.MarkBuffersChanged();

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.ResizeStreams"));
	// A PF_R32_UINT view covers the whole buffer whatever the stream's own element size is: a typed
	// buffer view takes its stride from the format, not from the buffer's descriptor.
	FCSMeshEditContext Context(GraphBuilder, Resident);
	for (int32 StreamIndex : ResizedStreams) if (FRDGBufferRef Dst = Context.StreamBuffers[StreamIndex]) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst, PF_R32_UINT)), 0u);
	CSMesh_FinalizeGraph(GraphBuilder, Context);
}

/** Reallocates the IndirectArgs stream alone, keeping the arg sets that already existed. The
 *  geometry does not move just because the draw layout grew, so nothing else is touched.
 *  Render thread. */
void CSMesh_ReallocateIndirectArgs(FRHICommandListImmediate& RHICmdList, FCSMeshResident& Resident, uint32 NumArgSets)
{
	check(IsInRenderingThread());

	const int32 ArgsIndex = Resident.FindStreamIndex(ECSGpuStreamRole::IndirectArgs);
	if (ArgsIndex == INDEX_NONE) return;

	FCSMeshResident::FStream& Args = Resident.Streams[ArgsIndex];
	const TRefCountPtr<FRDGPooledBuffer> OldPooled = Args.Pooled;
	const uint64 OldBytes = OldPooled.IsValid() ? CSMesh_StreamBytes(Args.Desc, CSMesh_MinCapacity, CSMesh_MinCapacity) : 0;

	Args.Desc.ElementsPerUnit = CSMesh_IndirectArgsUintsPerSet * FMath::Max(NumArgSets, 1u);
	Args.Pooled = AllocatePooledBuffer(
		CSMesh_MakeStreamBufferDesc(Args.Desc, CSMesh_MinCapacity, CSMesh_MinCapacity), Args.Desc.DebugName);
	Resident.NumIndirectDraws = FMath::Max(NumArgSets, 1u);
	Resident.MarkBuffersChanged();

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.GrowIndirectArgs"));
	FCSMeshEditContext Context(GraphBuilder, Resident);
	if (FRDGBufferRef Dst = Context.StreamBuffers[ArgsIndex])
	{
		// An arg set nobody has written yet must draw nothing rather than five uints of
		// whatever the pool handed us.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dst, PF_R32_UINT)), 0u);
		if (OldBytes > 0)
		{
			FRDGBufferRef Src = GraphBuilder.RegisterExternalBuffer(OldPooled, Args.Desc.DebugName);
			AddCopyBufferPass(GraphBuilder, Dst, 0, Src, 0, OldBytes);
		}
	}
	CSMesh_FinalizeGraph(GraphBuilder, Context);
}
}

// -----------------------------------------------------------------------------
// FCSMeshResident
// -----------------------------------------------------------------------------

void FCSMeshResident::AddStandardTriangleStreams(uint32 InNumIndirectDraws)
{
	NumIndirectDraws = FMath::Max(InNumIndirectDraws, 1u);

	TArray<FCSGpuStreamDesc> Descs;
	CSMesh_BuildStandardDescs(Descs, NumIndirectDraws);

	Streams.Reset(Descs.Num());
	for (const FCSGpuStreamDesc& Desc : Descs)
	{
		FStream& Stream = Streams.AddDefaulted_GetRef();
		Stream.Desc = Desc;
	}
}

bool FCSMeshResident::AddStream(const FCSGpuStreamDesc& Desc)
{
	if (!CSMesh_IsStreamDescUsable(Desc)) return false;
	if (const FStream* Existing = FindStream(Desc.Role, Desc.TexCoordIndex))
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream '%s' rejected: role %u slot %u is already held by '%s'."),
			Desc.DebugName, uint32(Desc.Role), uint32(Desc.TexCoordIndex), Existing->Desc.DebugName);
		return false;
	}
	Streams.AddDefaulted_GetRef().Desc = Desc;
	return true;
}

void FCSMeshResident::AllocateBuffers()
{
	check(IsInRenderingThread());

	const uint32 VertUnits = FMath::Max(VertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, CSMesh_MinCapacity);

	for (FStream& Stream : Streams)
	{
		const FRDGBufferDesc BufferDesc = CSMesh_MakeStreamBufferDesc(Stream.Desc, VertUnits, IdxUnits);
		Stream.Pooled = AllocatePooledBuffer(BufferDesc, Stream.Desc.DebugName);
	}
	MarkBuffersChanged();
}

void FCSMeshResident::ReleaseBuffers()
{
	for (FStream& Stream : Streams) Stream.Pooled.SafeRelease();
	KnownVertexCount = 0;
	KnownIndexCount = 0;
	MarkBuffersChanged();
}

void FCSMeshResident::MarkBuffersChanged()
{
	Sections.Reset();
	// The counts describe arg sets that are about to stop existing. Keeping them would put the
	// previous geometry's triangle count into the next shadow pass — which draws whatever the
	// index buffer happens to hold that far in, not nothing.
	DiscardDrawArgs();
	++AllocationGeneration;
}

// -----------------------------------------------------------------------------
// Draw-args readback pump
// -----------------------------------------------------------------------------
//
// Why a pump instead of polling where the answer is used: FRHIGPUBufferReadback::Lock() goes
// through the immediate command list, so it may only be touched on the render thread — and
// GetDynamicMeshElements, the only consumer, never runs on it. Not even the "serial" gather
// pass does: it is a task tagged EParallelRenderingThread (ShadowSetup.cpp:4777 in 5.7.4).
// FCoreDelegates::OnEndFrameRT is the render thread, once per frame, and costs nothing while
// no readback is outstanding — the delegate is only registered while the list below is
// non-empty, so a project that never edits a GPU mesh never pays for this at all.

namespace
{
/** Sets with a readback in flight. Registration, the walk, and removal all happen on the render
 *  thread, which is what makes the raw pointers safe: a set can only die there too. */
TArray<FCSMeshResident*> GCSMeshDrawArgsPending;
FDelegateHandle GCSMeshDrawArgsPumpHandle;

void CSMesh_PumpDrawArgs();

void CSMesh_SetDrawArgsPumpRegistered(bool bRegistered)
{
	if (bRegistered == GCSMeshDrawArgsPumpHandle.IsValid()) return;
	// Unregistering when the last readback lands rather than at module shutdown: a delegate
	// outliving the module it points into is a crash on hot reload, and this way there is no
	// shutdown hook that can be forgotten.
	if (bRegistered) GCSMeshDrawArgsPumpHandle = FCoreDelegates::OnEndFrameRT.AddStatic(&CSMesh_PumpDrawArgs);
	else
	{
		FCoreDelegates::OnEndFrameRT.Remove(GCSMeshDrawArgsPumpHandle);
		GCSMeshDrawArgsPumpHandle.Reset();
	}
}

void CSMesh_PumpDrawArgs()
{
	check(IsInRenderingThread());
	for (int32 Index = GCSMeshDrawArgsPending.Num() - 1; Index >= 0; --Index)
		if (GCSMeshDrawArgsPending[Index]->TryPublishDrawArgs()) GCSMeshDrawArgsPending.RemoveAtSwap(Index);
	CSMesh_SetDrawArgsPumpRegistered(GCSMeshDrawArgsPending.Num() > 0);
}

void CSMesh_TrackDrawArgsPending(FCSMeshResident* Resident, bool bPending)
{
	if (bPending) GCSMeshDrawArgsPending.AddUnique(Resident);
	else GCSMeshDrawArgsPending.RemoveSingleSwap(Resident);
	CSMesh_SetDrawArgsPumpRegistered(GCSMeshDrawArgsPending.Num() > 0);
}
}

FCSMeshResident::~FCSMeshResident()
{
	// Deliberately not check(IsInRenderingThread()): the readback view UCSGpuMeshComponent builds
	// on the stack is a game-thread FCSMeshResident that never requests counts, and asserting here
	// would make that legitimate use illegal. A set that does hold a readback can only have got it
	// on the render thread and can only be released there, so the cleanup below is on it.
	if (DrawArgsReadback == nullptr) return;
	CSMesh_TrackDrawArgsPending(this, false);
	delete DrawArgsReadback;
	DrawArgsReadback = nullptr;
}

bool FCSMeshResident::GetDrawArgs(int32 ArgSetIndex, FCSGpuDrawArgs& OutArgs) const
{
	FScopeLock Lock(&PublishedDrawArgsLock);
	if (!PublishedDrawArgs.IsValidIndex(ArgSetIndex)) return false;
	OutArgs = PublishedDrawArgs[ArgSetIndex];
	return true;
}

void FCSMeshResident::RequestDrawArgsReadback(FRDGBuilder& GraphBuilder, FRDGBufferRef IndirectArgsBuffer)
{
	check(IsInRenderingThread());
	if (IndirectArgsBuffer == nullptr) return;

	const uint32 WantBytes = FMath::Max(NumIndirectDraws, 1u) * CSMesh_IndirectArgsUintsPerSet * sizeof(uint32);
	// NumIndirectDraws and the args descriptor are kept in step by every path that moves either,
	// but this copy is a GPU read: clamping costs one Min and turns a drift bug into short counts
	// instead of a read past the end of the buffer.
	const uint32 Bytes = FMath::Min<uint32>(WantBytes, IndirectArgsBuffer->GetSize());
	if (Bytes < sizeof(uint32) * CSMesh_IndirectArgsUintsPerSet) return;

	// The edit about to execute makes what was published describe geometry that will no longer be
	// there, so it is retired NOW rather than left standing until its replacement lands. The
	// asymmetry is what forces this: an over-count is not a slightly wrong shadow, it is a wrong
	// one — the index slots past the new end still hold the previous generation's indices, which
	// now point at re-used vertex slots, so a mesh that shrinks would cast a shadow of scrambled
	// triangles for the two or three frames the readback is in flight. Undercounting to nothing
	// costs those same frames of shadow and is invisible.
	{
		FScopeLock Lock(&PublishedDrawArgsLock);
		PublishedDrawArgs.Reset();
	}

	// A request that arrives while one is in flight SUPERSEDES it rather than being dropped, and
	// reuses the same object to do so: EnqueueCopy clears the fence and keeps its staging buffer
	// (RHIGPUReadback.cpp:38), so this costs no allocation and IsReady() stays false until the
	// newest copy lands. Dropping was the obvious policy and it was wrong in a way worth
	// recording: a mesh is normally GROWN and then FILLED by two separate graphs, so the drop
	// published the allocation graph's freshly zeroed args and the fill's real counts were never
	// asked for at all — a mesh that would then never cast a virtual shadow.
	if (DrawArgsReadback == nullptr) DrawArgsReadback = new FRHIGPUBufferReadback(TEXT("CSMesh.DrawArgs"));
	AddEnqueueCopyPass(GraphBuilder, DrawArgsReadback, IndirectArgsBuffer, Bytes);
	CSMesh_TrackDrawArgsPending(this, true);
}

bool FCSMeshResident::TryPublishDrawArgs()
{
	check(IsInRenderingThread());
	if (DrawArgsReadback == nullptr) return true;
	if (!DrawArgsReadback->IsReady()) return false;

	const uint32 NumSets = FMath::Max(NumIndirectDraws, 1u);
	const uint64 Bytes = FMath::Min<uint64>(
		uint64(NumSets) * CSMesh_IndirectArgsUintsPerSet * sizeof(uint32), DrawArgsReadback->GetGPUSizeBytes());
	const uint32 ReadableSets = uint32(Bytes / (CSMesh_IndirectArgsUintsPerSet * sizeof(uint32)));

	TArray<FCSGpuDrawArgs> Published;
	if (ReadableSets > 0)
	{
		if (const uint32* Args = static_cast<const uint32*>(DrawArgsReadback->Lock(uint32(Bytes))))
		{
			// Everything except the instance count (arg 1, the cull pass's) and
			// StartInstanceLocation (arg 4, always 0 here). StartIndexLocation is not optional:
			// with sections each set points at its own run, and a batch that took only the count
			// would draw that many triangles from the start of the mesh instead.
			Published.Reserve(int32(ReadableSets));
			for (uint32 Set = 0; Set < ReadableSets; ++Set)
			{
				const uint32* Args5 = Args + Set * CSMesh_IndirectArgsUintsPerSet;
				FCSGpuDrawArgs& Out = Published.AddDefaulted_GetRef();
				Out.IndexCount = Args5[0];
				Out.FirstIndex = Args5[2];
				Out.BaseVertexIndex = int32(Args5[3]);
			}
			DrawArgsReadback->Unlock();
		}
	}

	delete DrawArgsReadback;
	DrawArgsReadback = nullptr;

	// A read that produced nothing publishes nothing, which leaves the set in the same "unknown"
	// state RequestDrawArgsReadback put it in: the shadow batch keeps falling back to the indirect
	// form until some later edit's readback succeeds. A mesh that really emptied is a different
	// case and lands here with real zeroes, which do get published.
	if (Published.IsEmpty()) return true;

	FScopeLock Lock(&PublishedDrawArgsLock);
	PublishedDrawArgs = MoveTemp(Published);
	++DrawArgsPublishSerial;
	return true;
}

uint32 FCSMeshResident::GetDrawArgsPublishSerial() const
{
	FScopeLock Lock(&PublishedDrawArgsLock);
	return DrawArgsPublishSerial;
}

void FCSMeshResident::DiscardDrawArgs()
{
	if (DrawArgsReadback != nullptr)
	{
		check(IsInRenderingThread());
		CSMesh_TrackDrawArgsPending(this, false);
		// Not waited on. The RHI defers the staging buffer's destruction until the copy that
		// targets it has retired, so abandoning one mid-flight is a release, not a hazard.
		delete DrawArgsReadback;
		DrawArgsReadback = nullptr;
	}
	FScopeLock Lock(&PublishedDrawArgsLock);
	PublishedDrawArgs.Reset();
}

bool FCSMeshResident::IsAllocated() const
{
	if (Streams.Num() == 0) return false;
	for (const FStream& Stream : Streams)
		if (!Stream.Pooled.IsValid()) return false;
	return true;
}

int32 FCSMeshResident::FindStreamIndex(ECSGpuStreamRole Role, uint8 Index) const
{
	for (int32 StreamIndex = 0; StreamIndex < Streams.Num(); ++StreamIndex)
	{
		const FCSGpuStreamDesc& Desc = Streams[StreamIndex].Desc;
		if (Desc.Role == Role && Desc.TexCoordIndex == Index) return StreamIndex;
	}
	return INDEX_NONE;
}

const FCSMeshResident::FStream* FCSMeshResident::FindStream(ECSGpuStreamRole Role, uint8 Index) const
{
	const int32 StreamIndex = FindStreamIndex(Role, Index);
	return (StreamIndex == INDEX_NONE) ? nullptr : &Streams[StreamIndex];
}

int64 FCSMeshResident::GetAllocatedBytes() const
{
	const uint32 VertUnits = FMath::Max(VertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, CSMesh_MinCapacity);

	int64 Bytes = 0;
	for (const FStream& Stream : Streams)
	{
		if (!Stream.Pooled.IsValid()) continue;
		Bytes += int64(CSMesh_StreamBytes(Stream.Desc, VertUnits, IdxUnits));
	}
	return Bytes;
}

int64 FCSMeshResident::GetRequiredBytes(uint32 InVertexCapacity, uint32 InIndexCapacity) const
{
	const uint32 VertUnits = FMath::Max(InVertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(InIndexCapacity, CSMesh_MinCapacity);

	int64 Bytes = 0;
	for (const FStream& Stream : Streams) Bytes += int64(CSMesh_StreamBytes(Stream.Desc, VertUnits, IdxUnits));
	return Bytes;
}

// -----------------------------------------------------------------------------
// FCSMeshEditContext
// -----------------------------------------------------------------------------

FCSMeshEditContext::FCSMeshEditContext(FRDGBuilder& InGraphBuilder, FCSMeshResident& InResident, EKind InKind, uint64 InStreamMask)
	: GraphBuilder(InGraphBuilder)
	, Resident(InResident)
	, Kind(InKind)
{
	// A borrowed graph is by definition somebody else's render-thread graph, and the assert is not
	// decoration: from the game thread this would hand raw resident pointers to a graph that
	// executes later, with no flush fencing either those pointers or the mesh object's lifetime.
	// The game thread's way in is UCSMesh::EditMeshSync.
	check(Kind != EKind::BorrowedGraph || IsInParallelRenderingThread());

	StreamBuffers.Reserve(Resident.Streams.Num());
	for (int32 StreamIndex = 0; StreamIndex < Resident.Streams.Num(); ++StreamIndex)
	{
		const FCSMeshResident::FStream& Stream = Resident.Streams[StreamIndex];
		// 掩码之外的流这次编辑不碰：不登记，留在它平时的访问状态上（见构造函数声明处的注释）。
		const bool bWanted = StreamIndex >= 64 || (InStreamMask & (1ull << StreamIndex)) != 0;
		StreamBuffers.Add(bWanted && Stream.Pooled.IsValid()
			? GraphBuilder.RegisterExternalBuffer(Stream.Pooled, Stream.Desc.DebugName)
			: nullptr);
	}

	// A stream a previous edit left in external access mode cannot be used in a pass until RDG has
	// it back, and taking it back is a no-op for a stream that is already internal — which is every
	// stream of a graph this edit owns, and every stream of a graph that has not seen them yet.
	// Doing it for the whole set rather than for the streams this particular caller happens to
	// write is the point: the caller does not have to know which of them last frame handed off.
	if (Kind != EKind::BorrowedGraph) return;
	for (FRDGBufferRef Buffer : StreamBuffers) if (Buffer) GraphBuilder.UseInternalAccessMode(Buffer);
}

void FCSMeshEditContext::RestoreStreamAccess()
{
	for (int32 Index = 0; Index < StreamBuffers.Num(); ++Index)
	{
		FRDGBufferRef Buffer = StreamBuffers[Index];
		if (!Buffer) continue;

		const ECSGpuStreamRole Role = Resident.Streams[Index].Desc.Role;
		// Both owned-graph kinds transition at the end of the graph they own; only the fence after
		// it differs, and RDG knows nothing about that.
		if (Kind != EKind::BorrowedGraph) CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, Buffer, Role);
		// The consumers of a borrowed graph's streams — the vertex factory's raw SRVs, the index
		// buffer binding, the DrawIndexedIndirect args — are things RDG knows nothing about, reached
		// by passes later in the very graph this edit is adding to. SetBufferAccessFinal would
		// transition at the end of it, long after the draw has read the wrong state.
		else GraphBuilder.UseExternalAccessMode(Buffer, CSGpuMeshStreams::FinalAccessForRole(Role));
	}
}

FRDGBufferRef FCSMeshEditContext::Find(ECSGpuStreamRole Role, uint8 Index) const
{
	for (int32 StreamIndex = 0; StreamIndex < Resident.Streams.Num(); ++StreamIndex)
	{
		const FCSGpuStreamDesc& Desc = Resident.Streams[StreamIndex].Desc;
		if (Desc.Role == Role && Desc.TexCoordIndex == Index) return StreamBuffers[StreamIndex];
	}
	return nullptr;
}

FRDGBufferRef FCSMeshEditContext::FindBySemantic(ECSGpuMeshSemantic Semantic) const
{
	if (Semantic == ECSGpuMeshSemantic::None) return nullptr;
	for (int32 StreamIndex = 0; StreamIndex < Resident.Streams.Num(); ++StreamIndex)
		if (Resident.Streams[StreamIndex].Desc.CpuSemantic == Semantic) return StreamBuffers[StreamIndex];
	return nullptr;
}

/** Sections, WorldBounds and the known counts are game-thread state. A synchronous edit writes
 *  them behind its flush, an asynchronous one stages them for the completion hop; a borrowed
 *  render-thread graph has neither and is refused. */
static bool CSMesh_CanPublish(const FCSMeshEditContext& Context, const TCHAR* What)
{
	if (Context.GetKind() != FCSMeshEditContext::EKind::BorrowedGraph) return true;
	UE_LOG(LogCSMesh, Warning,
		TEXT("[CSMesh] %s ignored: a render-thread edit has no flush or completion hop behind which to publish state the game thread reads."),
		What);
	return false;
}

FBox FCSMeshEditContext::GetWorldBounds() const
{
	return Staged.WorldBounds.IsSet() ? Staged.WorldBounds.GetValue() : Resident.WorldBounds;
}

void FCSMeshEditContext::SetWorldBounds(const FBox& Bounds)
{
	if (!CSMesh_CanPublish(*this, TEXT("SetWorldBounds"))) return;
	if (Kind == EKind::OwnedGraphAsync)
	{
		Staged.WorldBounds = Bounds;
		return;
	}
	Resident.WorldBounds = Bounds;
}

void FCSMeshEditContext::InvalidateSections()
{
	if (!CSMesh_CanPublish(*this, TEXT("InvalidateSections"))) return;
	if (Kind == EKind::OwnedGraphAsync)
	{
		Staged.bInvalidateSections = true;
		return;
	}
	Resident.Sections.Reset();
}

void FCSMeshEditContext::SetKnownCounts(int32 VertexCount, int32 IndexCount)
{
	if (!CSMesh_CanPublish(*this, TEXT("SetKnownCounts"))) return;
	if (Kind == EKind::OwnedGraphAsync)
	{
		Staged.bSetKnownCounts = true;
		Staged.KnownVertexCount = VertexCount;
		Staged.KnownIndexCount = IndexCount;
		return;
	}
	Resident.KnownVertexCount = VertexCount;
	Resident.KnownIndexCount = IndexCount;
}

void FCSMeshEditContext::InvalidateKnownCounts()
{
	SetKnownCounts(INDEX_NONE, INDEX_NONE);
}

void FCSMeshEditContext::FStagedPublish::ApplyTo(FCSMeshResident& InResident) const
{
	check(IsInGameThread());
	if (bInvalidateSections) InResident.Sections.Reset();
	if (WorldBounds.IsSet()) InResident.WorldBounds = WorldBounds.GetValue();
	if (!bSetKnownCounts) return;
	InResident.KnownVertexCount = KnownVertexCount;
	InResident.KnownIndexCount = KnownIndexCount;
}

void FCSMeshEditContext::KeepAliveResource(const FShaderResourceViewRHIRef& View)
{
	if (!View.IsValid()) return;
	// On a borrowed graph this context dies at the end of the caller's scope, which is long before
	// the caller executes — so the reference has to belong to the graph rather than to us, or the
	// pass reads a view that was released while it was still queued.
	if (Kind == EKind::BorrowedGraph) GraphBuilder.AllocObject<FShaderResourceViewRHIRef>(View);
	else KeepAliveViews.Add(View);
}

// -----------------------------------------------------------------------------
// FCSMeshRenderThreadEdit
// -----------------------------------------------------------------------------

FCSMeshRenderThreadEdit::FCSMeshRenderThreadEdit(FRDGBuilder& InGraphBuilder, FCSMeshResident& InResident, uint64 InStreamMask)
	: Context(InGraphBuilder, InResident, FCSMeshEditContext::EKind::BorrowedGraph, InStreamMask)
{
}

FCSMeshRenderThreadEdit::~FCSMeshRenderThreadEdit()
{
	Context.RestoreStreamAccess();
}

// -----------------------------------------------------------------------------
// Readback (D4): acts on a resident set, not on a scene proxy
// -----------------------------------------------------------------------------

namespace CSMeshReadback
{
bool ReadUintBufferSync(const TRefCountPtr<FRDGPooledBuffer>& Buffer, uint32 NumUints,
	ECSGpuStreamRole FinalAccessRole, TArray<uint32>& OutValues)
{
	OutValues.Reset();
	if (!IsInGameThread() || !Buffer.IsValid() || NumUints == 0) return false;

	const uint32 Bytes = NumUints * sizeof(uint32);
	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSMesh.UintReadback"));
	TRefCountPtr<FRDGPooledBuffer> Pooled = Buffer;
	ENQUEUE_RENDER_COMMAND(CSMeshEnqueueUintReadback)(
		[Pooled, Readback, Bytes, FinalAccessRole](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.ReadbackUints"));
			FRDGBufferRef Ref = GraphBuilder.RegisterExternalBuffer(Pooled);
			AddEnqueueCopyPass(GraphBuilder, Readback, Ref, Bytes);
			// 读完必须放回常态访问状态，理由逐字见头文件那段注释。
			CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, Ref, FinalAccessRole);
			GraphBuilder.Execute();
		});
	// 诊断回读按设计就是阻塞的，但也要计数 —— 计数器要么数到每一次阻塞，要么就不能拿它当证据。
	// （零阻塞的测量窗口里不该出现任何 Debug*Sync，真出现了就该报红。）
	CSMesh_CountedFlush();

	bool bRead = false;
	TArray<uint32> Values;
	ENQUEUE_RENDER_COMMAND(CSMeshConsumeUintReadback)(
		[Readback, Bytes, NumUints, &Values, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= uint64(Bytes))
			{
				if (const uint32* Data = static_cast<const uint32*>(Readback->Lock(Bytes)))
				{
					Values.Append(Data, int32(NumUints));
					Readback->Unlock();
					bRead = true;
				}
			}
			delete Readback;
		});
	CSMesh_CountedFlush();

	if (!bRead)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: uint buffer readback did not complete."));
		return false;
	}
	OutValues = MoveTemp(Values);
	return true;
}

bool ReadCountersSync(const FCSMeshResident& Resident, uint32& OutVertexCount, uint32& OutIndexCount)
{
	OutVertexCount = 0;
	OutIndexCount = 0;
	if (!IsInGameThread()) return false;

	const FCSMeshResident::FStream* Counters = Resident.FindStream(ECSGpuStreamRole::MeshCounters);
	if (!Counters || !Counters->Pooled.IsValid())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: MeshCounters buffer is unavailable."));
		return false;
	}

	FRHIGPUBufferReadback* CountersReadback = new FRHIGPUBufferReadback(TEXT("CSMesh.CountersReadback"));
	TRefCountPtr<FRDGPooledBuffer> Pooled = Counters->Pooled;
	ENQUEUE_RENDER_COMMAND(CSMeshEnqueueCounters)(
		[Pooled, CountersReadback](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.ReadbackCounters"));
			FRDGBufferRef CountersRDG = GraphBuilder.RegisterExternalBuffer(Pooled);
			AddEnqueueCopyPass(GraphBuilder, CountersReadback, CountersRDG, sizeof(uint32) * 2u);
			CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, CountersRDG, ECSGpuStreamRole::MeshCounters);
			GraphBuilder.Execute();
		});
	CSMesh_CountedFlush();

	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSMeshConsumeCounters)(
		[CountersReadback, &VertexCount, &IndexCount, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			if (!CountersReadback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (CountersReadback->IsReady() && CountersReadback->GetGPUSizeBytes() >= sizeof(uint32) * 2u)
			{
				if (const uint32* Counts = static_cast<const uint32*>(CountersReadback->Lock(sizeof(uint32) * 2u)))
				{
					VertexCount = Counts[0];
					IndexCount = Counts[1];
					CountersReadback->Unlock();
					bRead = true;
				}
			}
			delete CountersReadback;
		});
	CSMesh_CountedFlush();

	if (!bRead)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: MeshCounters GPU readback did not complete."));
		return false;
	}
	OutVertexCount = VertexCount;
	OutIndexCount = IndexCount;
	return true;
}

bool ReadbackResidentSync(const FCSMeshResident& Resident, FCSGpuMeshCPUData& OutMeshData)
{
	OutMeshData.Reset();
	if (!IsInGameThread())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback rejected: not on the game thread."));
		return false;
	}
	if (!Resident.IsAllocated())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback rejected: the mesh has no GPU allocation."));
		return false;
	}

	// --- 1) GPU-decided counts
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	if (!ReadCountersSync(Resident, VertexCount, IndexCount)) return false;

	const uint32 VertexCapacity = FMath::Max(Resident.VertexCapacity, 1u);
	const uint32 IndexCapacity = FMath::Max(Resident.IndexCapacity, 1u);
	if (VertexCount < 3 || VertexCount > VertexCapacity)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: vertex count %u invalid for capacity %u."), VertexCount, VertexCapacity);
		return false;
	}
	if (IndexCount < 3 || IndexCount % 3u != 0 || IndexCount > IndexCapacity)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: index count %u invalid for capacity %u."), IndexCount, IndexCapacity);
		return false;
	}

	// --- 2) one readback per mesh-readback stream. Descriptor and readback travel as a pair
	//        here, so the two loops below cannot drift out of order.
	struct FReadStream
	{
		FCSGpuStreamDesc Desc;
		TRefCountPtr<FRDGPooledBuffer> Pooled;
		FRHIGPUBufferReadback* Readback = nullptr;
		uint32 Bytes = 0;
	};
	TArray<FReadStream> ReadStreams;
	for (const FCSMeshResident::FStream& Stream : Resident.Streams)
	{
		if (!Stream.Desc.bReadback || Stream.Desc.CpuSemantic == ECSGpuMeshSemantic::None) continue;
		FReadStream& Read = ReadStreams.AddDefaulted_GetRef();
		Read.Desc = Stream.Desc;
		Read.Pooled = Stream.Pooled;
		Read.Readback = new FRHIGPUBufferReadback(TEXT("CSMesh.StreamReadback"));
		const uint32 Units = CSGpuMeshStreams::UnitsForCountSource(Stream.Desc.CountSource, VertexCount, IndexCount);
		Read.Bytes = Units * Stream.Desc.ElementsPerUnit * Stream.Desc.BytesPerElement;
	}
	if (ReadStreams.Num() == 0)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: the mesh exposes no readback streams."));
		return false;
	}

	ENQUEUE_RENDER_COMMAND(CSMeshEnqueueStreams)(
		[&ReadStreams](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.ReadbackData"));
			for (const FReadStream& Read : ReadStreams)
			{
				if (Read.Bytes == 0) continue;
				FRDGBufferRef BufRDG = GraphBuilder.RegisterExternalBuffer(Read.Pooled);
				AddEnqueueCopyPass(GraphBuilder, Read.Readback, BufRDG, Read.Bytes);
				CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, BufRDG, Read.Desc.Role);
			}
			GraphBuilder.Execute();
		});
	CSMesh_CountedFlush();

	// --- 3) lock each stream and fill the CPU snapshot by semantic
	OutMeshData.Positions.SetNumUninitialized(VertexCount);
	OutMeshData.Normals.SetNumUninitialized(VertexCount);
	OutMeshData.Tangents.SetNumUninitialized(VertexCount);
	OutMeshData.TexCoords().SetNumUninitialized(VertexCount);
	OutMeshData.Indices.SetNumUninitialized(IndexCount);
	{
		// ⚠️ 一条 TexCoord 流可以**交错**承载多组 UV（ElementsPerUnit = 2 × 组数，见
		// FStandardStreamOptions::NumTexCoordSets）。只取 TexCoordIndex 的最大值等于只认「第几条
		// 流」，会把 N 组的那条流算成 1 组 —— 症状是回读 / SaveToStaticMesh 之后 UV1 起全丢，
		// 而且下面的消费循环还会按 stride 2 走进别的组的数据里，连 UV0 一起错位，且无任何报错。
		int32 HighestTexCoordIndex = 0;
		for (const FReadStream& Read : ReadStreams)
		{
			if (Read.Desc.CpuSemantic != ECSGpuMeshSemantic::TexCoord) continue;
			const int32 NumSets = FMath::Max<int32>(int32(Read.Desc.ElementsPerUnit) / 2, 1);
			HighestTexCoordIndex = FMath::Max<int32>(
				HighestTexCoordIndex, int32(Read.Desc.TexCoordIndex) + NumSets - 1);
		}
		OutMeshData.NumTexCoordChannels = FMath::Clamp(
			HighestTexCoordIndex + 1, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
		for (int32 Channel = 1; Channel < OutMeshData.NumTexCoordChannels; ++Channel)
			OutMeshData.TexCoordChannels[Channel].SetNumZeroed(VertexCount);
	}

	bool bMeshRead = false;
	ENQUEUE_RENDER_COMMAND(CSMeshConsumeStreams)(
		[&ReadStreams, VertexCount, IndexCount, &OutMeshData, &bMeshRead](FRHICommandListImmediate& RHICmdList)
		{
			bool bAnyNotReady = false;
			for (const FReadStream& Read : ReadStreams)
				if (!Read.Readback->IsReady()) bAnyNotReady = true;
			if (bAnyNotReady) RHICmdList.SubmitAndBlockUntilGPUIdle();

			bool bAllReady = true;
			for (const FReadStream& Read : ReadStreams)
				if (!Read.Readback->IsReady() || Read.Readback->GetGPUSizeBytes() < Read.Bytes) { bAllReady = false; break; }

			if (bAllReady)
			{
				bool bLockedOk = true;
				for (const FReadStream& Read : ReadStreams)
				{
					if (!bLockedOk) break;
					const void* Raw = Read.Readback->Lock(Read.Bytes);
					if (!Raw) { bLockedOk = false; break; }

					switch (Read.Desc.CpuSemantic)
					{
					case ECSGpuMeshSemantic::Position:
					{
						const float* P = static_cast<const float*>(Raw);
						for (uint32 v = 0; v < VertexCount; ++v)
							OutMeshData.Positions[v] = FVector3f(P[v * 3 + 0], P[v * 3 + 1], P[v * 3 + 2]);
						break;
					}
					case ECSGpuMeshSemantic::TangentBasis:
					{
						const uint32* T = static_cast<const uint32*>(Raw);
						for (uint32 v = 0; v < VertexCount; ++v)
						{
							OutMeshData.Tangents[v] = CSMesh_UnpackSnorm8888XYZ(T[v * 2 + 0]);
							OutMeshData.Normals[v] = CSMesh_UnpackSnorm8888XYZ(T[v * 2 + 1]);
						}
						break;
					}
					case ECSGpuMeshSemantic::TexCoord:
					{
						// 交错布局：每顶点 NumSets 组，第 s 组落到 TexCoordIndex + s 号通道。
						// stride 必须按 NumSets 走而不是写死 2 —— 写死的话 N ≥ 2 时第 1 个顶点起
						// 连 UV0 都读到别组的数据上，且不会报错。
						const int32 NumSets = FMath::Max<int32>(int32(Read.Desc.ElementsPerUnit) / 2, 1);
						const float* UV = static_cast<const float*>(Raw);
						for (int32 Set = 0; Set < NumSets; ++Set)
						{
							const int32 Channel = int32(Read.Desc.TexCoordIndex) + Set;
							if (Channel >= OutMeshData.NumTexCoordChannels) break;
							TArray<FVector2f>& ChannelUVs = OutMeshData.TexCoordChannels[Channel];
							if (ChannelUVs.Num() != int32(VertexCount)) ChannelUVs.SetNumUninitialized(VertexCount);
							for (uint32 v = 0; v < VertexCount; ++v)
							{
								const uint32 Base = (v * uint32(NumSets) + uint32(Set)) * 2u;
								ChannelUVs[v] = FVector2f(UV[Base + 0u], UV[Base + 1u]);
							}
						}
						break;
					}
					case ECSGpuMeshSemantic::Color:
					{
						// The colour stream is packed BGRA (RoadBuilder.usf's documented layout,
						// and what VET_Color expects); the CPU side wants float4 RGBA, which is
						// what the MeshDescription vertex-instance colour attribute takes.
						const uint32* C = static_cast<const uint32*>(Raw);
						OutMeshData.Colors.SetNumUninitialized(VertexCount);
						for (uint32 v = 0; v < VertexCount; ++v)
						{
							const uint32 Packed = C[v];
							OutMeshData.Colors[v] = FVector4f(
								float((Packed >> 16) & 0xffu) / 255.0f, // R
								float((Packed >> 8) & 0xffu) / 255.0f,  // G
								float((Packed >> 0) & 0xffu) / 255.0f,  // B
								float((Packed >> 24) & 0xffu) / 255.0f); // A
						}
						break;
					}
					case ECSGpuMeshSemantic::Index:
					{
						const uint32* Idx = static_cast<const uint32*>(Raw);
						for (uint32 k = 0; k < IndexCount; ++k)
							OutMeshData.Indices[k] = Idx[k];
						break;
					}
					case ECSGpuMeshSemantic::MaterialId:
					{
						const uint32 TriangleCount = IndexCount / 3u;
						const uint32* Ids = static_cast<const uint32*>(Raw);
						OutMeshData.TriangleMaterialSlots.SetNumUninitialized(TriangleCount);
						for (uint32 t = 0; t < TriangleCount; ++t)
						{
							// CS_NO_MATERIAL_ID (~0u) and any other out-of-range id fall back to
							// slot 0 rather than losing the triangle at MeshDescription assembly.
							const uint32 Id = Ids[t];
							OutMeshData.TriangleMaterialSlots[t] = (Id > uint32(MAX_int32)) ? 0 : int32(Id);
						}
						break;
					}
					default:
						break;
					}
					Read.Readback->Unlock();
				}
				bMeshRead = bLockedOk;
			}

			for (const FReadStream& Read : ReadStreams) delete Read.Readback;
		});
	CSMesh_CountedFlush();

	if (!bMeshRead)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Readback failed: stream copies did not complete for %u verts / %u indices."),
			VertexCount, IndexCount);
		OutMeshData.Reset();
		return false;
	}
	return OutMeshData.IsValid();
}
}

// -----------------------------------------------------------------------------
// UCSMesh
// -----------------------------------------------------------------------------

UCSMesh::UCSMesh()
{
	Resident = MakeShared<FCSMeshResident, ESPMode::ThreadSafe>();
	Resident->AddStandardTriangleStreams();
}

void UCSMesh::BeginDestroy()
{
	// Not because pooled buffers must die on the render thread — they are atomically ref-counted and
	// the pool outlives any frame still reading them (see FCSMeshResident). The hop sequences the
	// drop behind the commands already queued and keeps ~FCSMeshResident, which deletes the
	// render-thread-only draw-args readback, off the game thread.
	if (Resident.IsValid())
	{
		FCSMeshResidentRef Doomed = MoveTemp(Resident);
		ENQUEUE_RENDER_COMMAND(CSMeshReleaseResident)(
			[Doomed](FRHICommandListImmediate&) mutable
			{
				if (Doomed.IsValid()) Doomed->ReleaseBuffers();
				Doomed.Reset();
			});
	}
	Super::BeginDestroy();
}

void UCSMesh::SetMaterial(int32 Index, UMaterialInterface* Material)
{
	if (Index < 0) return;
	if (Index >= Materials.Num()) Materials.SetNum(Index + 1);
	if (Materials[Index] == Material) return;
	Materials[Index] = Material;
	NotifyMaterialsChanged();
}

void UCSMesh::NotifyMaterialsChanged()
{
	// Deliberately does not touch Generation: nothing about the geometry moved, and a consumer
	// that keyed off Generation would see a content edit that never happened. The broadcast is
	// the whole payload — the render component re-resolves its batch materials from the table.
	OnMeshChanged.Broadcast(this);
}

void UCSMesh::Reset()
{
	if (!Resident.IsValid() || !Resident->IsAllocated()) return;
	EditMeshSync([](FCSMeshEditContext& Context)
	{
		CSMesh_AddClearCountersPasses(Context);
		Context.SetWorldBounds(FBox(ForceInit));
	});
}

void UCSMesh::ReleaseSync()
{
	if (!Resident.IsValid()) return;
	FCSMeshResident* ResidentPtr = Resident.Get();
	ENQUEUE_RENDER_COMMAND(CSMeshRelease)(
		[ResidentPtr](FRHICommandListImmediate&) { ResidentPtr->ReleaseBuffers(); });
	CSMesh_CountedFlush();
	Resident->VertexCapacity = 0;
	Resident->IndexCapacity = 0;
	Resident->WorldBounds = FBox(ForceInit);
	++Resident->Generation;
	OnMeshChanged.Broadcast(this);
}

bool UCSMesh::ReleaseDeferred()
{
	if (!Resident.IsValid()) return true;
	if (bAsyncEditInFlight) return false;

	FCSMeshResidentRef Doomed = MoveTemp(Resident);
	// 先让绑着的组件换下代理（HandleMeshChanged 看到分配代数归零就重建），代理的销毁于是排在
	// 下面那条释放之前 —— 代理借用的正是这几条流。
	OnMeshChanged.Broadcast(this);
	ENQUEUE_RENDER_COMMAND(CSMeshReleaseDeferred)(
		[Doomed = MoveTemp(Doomed)](FRHICommandListImmediate&) mutable
		{
			if (Doomed.IsValid()) Doomed->ReleaseBuffers();
			Doomed.Reset();
		});
	return true;
}

bool UCSMesh::IsEmpty() const
{
	if (!Resident.IsValid() || !Resident->IsAllocated()) return true;
	// INDEX_NONE means "only the GPU knows"; report non-empty rather than pretend.
	return Resident->KnownVertexCount == 0 || Resident->KnownIndexCount == 0;
}

int32 UCSMesh::GetTriangleCountSync()
{
	uint32 VertexCount = 0;
	uint32 IndexCount = 0;
	if (!GetCountsSync(VertexCount, IndexCount)) return 0;
	return int32(IndexCount / 3u);
}

bool UCSMesh::GetCountsSync(uint32& OutVertexCount, uint32& OutIndexCount)
{
	OutVertexCount = 0;
	OutIndexCount = 0;
	if (!Resident.IsValid() || !Resident->IsAllocated()) return false;
	if (!CSMeshReadback::ReadCountersSync(*Resident, OutVertexCount, OutIndexCount)) return false;

	// Cache what the GPU said so IsEmpty() stops guessing until the next edit.
	Resident->KnownVertexCount = int32(FMath::Min<uint32>(OutVertexCount, MAX_int32));
	Resident->KnownIndexCount = int32(FMath::Min<uint32>(OutIndexCount, MAX_int32));
	return true;
}

FBox UCSMesh::GetWorldBoundsApprox() const
{
	return Resident.IsValid() ? Resident->WorldBounds : FBox(ForceInit);
}

int32 UCSMesh::GetVertexCapacity() const
{
	return Resident.IsValid() ? int32(Resident->VertexCapacity) : 0;
}

int32 UCSMesh::GetIndexCapacity() const
{
	return Resident.IsValid() ? int32(Resident->IndexCapacity) : 0;
}

uint32 UCSMesh::GetGeneration() const
{
	return Resident.IsValid() ? Resident->Generation : 0u;
}

bool UCSMesh::EditMeshSync(TFunctionRef<void(FCSMeshEditContext&)> EditFunc)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Edit rejected: not on the game thread."));
		return false;
	}
	if (!Resident.IsValid() || !Resident->IsAllocated())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Edit rejected: the mesh has no GPU allocation (call EnsureCapacitySync first)."));
		return false;
	}

	FCSMeshResident* ResidentPtr = Resident.Get();
	// EditFunc is a non-owning view of a caller-stack callable; the flush below is the fence
	// that keeps it (and every raw pointer the operator captured) alive across the hand-off.
	ENQUEUE_RENDER_COMMAND(CSMeshEdit)(
		[ResidentPtr, EditFunc](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.Edit"));
			FCSMeshEditContext Context(GraphBuilder, *ResidentPtr);
			EditFunc(Context);
			CSMesh_FinalizeGraph(GraphBuilder, Context);
		});
	CSMesh_CountedFlush();

	++Resident->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

int64 UCSMesh::GetBlockingFlushCount()
{
	return GCSMeshBlockingFlushCount.load(std::memory_order_relaxed);
}

void UCSMesh::CountedBlockingFlush()
{
	// 计数器是 file-local static，别的 TU 够不着；这条是它们唯一的入口。
	// unity 构建里别的 TU **恰好**能看见 GCSMeshBlockingFlushCount，
	// 直接引用它会在非 unity（-SingleFile）下才炸 —— 所以只能走这个函数。
	CSMesh_CountedFlush();
}

bool UCSMesh::EditMeshAsync(TFunction<void(FCSMeshEditContext&)> EditFunc, TFunction<void(bool)> OnComplete)
{
	if (!IsInGameThread())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Async edit rejected: not on the game thread."));
		return false;
	}
	if (!Resident.IsValid() || !Resident->IsAllocated())
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Async edit rejected: the mesh has no GPU allocation (call EnsureCapacitySync first)."));
		return false;
	}
	if (bAsyncEditInFlight)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Async edit rejected: one is already in flight on this mesh."));
		return false;
	}
	if (!EditFunc)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Async edit rejected: no operator was supplied."));
		return false;
	}

	bAsyncEditInFlight = true;

	// A shared ref, not the raw pointer EditMeshSync hands over: without the flush, this UCSMesh can
	// be collected before the graph runs, and the resident set has to outlive the edit that writes
	// it. The weak pointer is only for the game-thread tail, which touches the UObject.
	FCSMeshResidentRef ResidentRef = Resident;
	TWeakObjectPtr<UCSMesh> WeakThis(this);

	ENQUEUE_RENDER_COMMAND(CSMeshEditAsync)(
		[ResidentRef, WeakThis, EditFunc = MoveTemp(EditFunc), OnComplete = MoveTemp(OnComplete)](FRHICommandListImmediate& RHICmdList) mutable
		{
			FCSMeshEditContext::FStagedPublish Staged;
			{
				FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.EditAsync"));
				FCSMeshEditContext Context(GraphBuilder, *ResidentRef, FCSMeshEditContext::EKind::OwnedGraphAsync);
				EditFunc(Context);
				CSMesh_FinalizeGraph(GraphBuilder, Context);
				Staged = Context.GetStagedPublish();
			}

			// Generation, OnMeshChanged and the caller's callback are all game-thread state, and so is
			// everything the operator published (bounds, section table, counts): the context staged
			// those instead of writing them next to a game thread that was never fenced, and this hop
			// — enqueued after the graph executed, running on the game thread — is where they land,
			// before Generation moves and before anybody is told the mesh changed.
			AsyncTask(ENamedThreads::GameThread, [ResidentRef, WeakThis, Staged, OnComplete = MoveTemp(OnComplete)]() mutable
			{
				Staged.ApplyTo(*ResidentRef);
				++ResidentRef->Generation;

				UCSMesh* Mesh = WeakThis.Get();
				if (Mesh)
				{
					Mesh->bAsyncEditInFlight = false;
					Mesh->OnMeshChanged.Broadcast(Mesh);
				}
				if (OnComplete) OnComplete(Mesh != nullptr);
			});
		});

	return true;
}

bool UCSMesh::ConfirmGpuMemoryBudget(int64 RequestedBytes, const TCHAR* OperationName) const
{
	if (!bCheckGpuMemoryBudget || RequestedBytes <= 0) return true;

	// A reallocation holds both buffer sets at once — the copy reads the old one while writing
	// the new — so the peak is what has to fit, not the size we end up at.
	const int64 AlreadyHeld = Resident.IsValid() ? Resident->GetAllocatedBytes() : 0;
	const int64 PeakBytes = RequestedBytes + AlreadyHeld;

	const CSGpuMemoryBudget::FMemorySnapshot Snapshot = CSGpuMemoryBudget::QueryMemorySnapshot();
	const int64 Available = Snapshot.AvailableVideoMemory > 0 ? Snapshot.AvailableVideoMemory : Snapshot.TotalVideoMemory;
	if (Available <= 0)
	{
		// Same call CSGpuMemoryBudget makes for BudgetUnknown: a probe that failed must never be
		// the reason a mesh cannot be built.
		UE_LOG(LogCSMesh, Log, TEXT("[CSMesh] %s: VRAM unknown, proceeding with %.1f MiB."),
			OperationName, double(PeakBytes) / (1024.0 * 1024.0));
		return true;
	}

	const int64 Limit = int64(double(Available) * double(FMath::Clamp(GpuMemoryBudgetSafetyRatio, 0.05f, 1.0f)));
	if (PeakBytes <= Limit) return true;

	UE_LOG(LogCSMesh, Warning,
		TEXT("[CSMesh] %s refused: needs %.1f MiB (%.1f MiB peak with the buffers it replaces) but only %.1f MiB of the %.1f MiB free VRAM is budgeted (%s, safety %.2f)."),
		OperationName,
		double(RequestedBytes) / (1024.0 * 1024.0),
		double(PeakBytes) / (1024.0 * 1024.0),
		double(Limit) / (1024.0 * 1024.0),
		double(Available) / (1024.0 * 1024.0),
		Snapshot.bAvailableIsMeasured ? TEXT("measured") : TEXT("estimated"),
		GpuMemoryBudgetSafetyRatio);
	return false;
}

bool UCSMesh::AllocateSync(uint32 InVertexCapacity, uint32 InIndexCapacity)
{
	if (!Resident.IsValid()) Resident = MakeShared<FCSMeshResident, ESPMode::ThreadSafe>();
	if (Resident->Streams.Num() == 0) Resident->AddStandardTriangleStreams(Resident->NumIndirectDraws);

	const uint32 WantVertices = FMath::Max(InVertexCapacity, CSMesh_MinCapacity);
	// Round the index capacity up to a whole triangle so the per-face streams (material ids)
	// and the readback's IndexCount % 3 check agree with the allocation.
	const uint32 WantIndices = FMath::Max(((InIndexCapacity + 2u) / 3u) * 3u, CSMesh_MinCapacity);
	if (!ConfirmGpuMemoryBudget(Resident->GetRequiredBytes(WantVertices, WantIndices), TEXT("Allocate"))) return false;

	FCSMeshResident* ResidentPtr = Resident.Get();
	ResidentPtr->VertexCapacity = WantVertices;
	ResidentPtr->IndexCapacity = WantIndices;

	ENQUEUE_RENDER_COMMAND(CSMeshAllocate)(
		[ResidentPtr](FRHICommandListImmediate& RHICmdList)
		{
			ResidentPtr->AllocateBuffers();
			// Freshly pooled buffers hold whatever the previous tenant left. Zeroing the
			// counters and the indirect args is what makes an unwritten mesh draw nothing
			// instead of garbage.
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSMesh.Allocate"));
			FCSMeshEditContext Context(GraphBuilder, *ResidentPtr);
			CSMesh_AddClearCountersPasses(Context);
			CSMesh_FinalizeGraph(GraphBuilder, Context);
		});
	CSMesh_CountedFlush();

	ResidentPtr->WorldBounds = FBox(ForceInit);
	++ResidentPtr->Generation;
	return true;
}

bool UCSMesh::EnsureCapacitySync(int32 InVertexCapacity, int32 InIndexCapacity)
{
	if (!IsInGameThread()) return false;

	const uint32 WantVertices = uint32(FMath::Max(InVertexCapacity, 3));
	const uint32 WantIndices = uint32(FMath::Max(InIndexCapacity, 3));

	if (!Resident.IsValid() || !Resident->IsAllocated())
	{
		if (!AllocateSync(WantVertices, WantIndices)) return false;
		OnMeshChanged.Broadcast(this);
		return true;
	}
	if (WantVertices <= Resident->VertexCapacity && WantIndices <= Resident->IndexCapacity) return true;

	// Grow: allocate at the new size and copy the old contents across. Never shrink here — the
	// caller asked for a floor, and dropping capacity would throw away geometry. Giving it back
	// deliberately is ShrinkCapacitySync's job.
	FCSMeshResident* ResidentPtr = Resident.Get();
	const uint32 TargetVertices = FMath::Max(WantVertices, ResidentPtr->VertexCapacity);
	const uint32 TargetIndices = FMath::Max(((WantIndices + 2u) / 3u) * 3u, ResidentPtr->IndexCapacity);
	if (!ConfirmGpuMemoryBudget(ResidentPtr->GetRequiredBytes(TargetVertices, TargetIndices), TEXT("Grow"))) return false;

	ENQUEUE_RENDER_COMMAND(CSMeshGrow)(
		[ResidentPtr, TargetVertices, TargetIndices](FRHICommandListImmediate& RHICmdList)
		{
			CSMesh_ReallocateResident(RHICmdList, *ResidentPtr, TargetVertices, TargetIndices);
		});
	CSMesh_CountedFlush();

	++ResidentPtr->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

bool UCSMesh::ShrinkCapacitySync(int32 InVertexCapacity, int32 InIndexCapacity, bool bAllowCounterReadback)
{
	if (!IsInGameThread()) return false;
	if (!Resident.IsValid() || !Resident->IsAllocated()) return false;

	// The live counts are the real floor, and INDEX_NONE means only the GPU knows them. Trusting
	// the caller's request instead would cut whatever the last operator produced off the end of
	// the mesh, with nothing to read it back from afterwards.
	if (Resident->KnownVertexCount == INDEX_NONE || Resident->KnownIndexCount == INDEX_NONE)
	{
		uint32 LiveVertices = 0;
		uint32 LiveIndices = 0;
		if (!bAllowCounterReadback || !GetCountsSync(LiveVertices, LiveIndices))
		{
			UE_LOG(LogCSMesh, Log,
				TEXT("[CSMesh] Shrink skipped: the live counts are GPU-decided and %s."),
				bAllowCounterReadback ? TEXT("the counter readback failed") : TEXT("no readback was allowed"));
			return false;
		}
	}

	const uint32 LiveVertices = uint32(FMath::Max(Resident->KnownVertexCount, 0));
	const uint32 LiveIndices = uint32(FMath::Max(Resident->KnownIndexCount, 0));

	// Never below the live counts, never above what is already allocated (a request larger than
	// the current capacity is a grow, and this entry point does not grow).
	const uint32 RequestedIndices = FMath::Max(uint32(FMath::Max(InIndexCapacity, 0)), LiveIndices);
	const uint32 TargetVertices = FMath::Min(
		FMath::Max3(uint32(FMath::Max(InVertexCapacity, 0)), LiveVertices, CSMesh_MinCapacity), Resident->VertexCapacity);
	const uint32 TargetIndices = FMath::Min(
		FMath::Max(((RequestedIndices + 2u) / 3u) * 3u, CSMesh_MinCapacity), Resident->IndexCapacity);

	// Hysteresis, so a mesh whose size oscillates does not reallocate and copy itself on every
	// rebuild. Either axis carrying more than the tolerated surplus is enough to trigger it.
	const double SlackFactor = 1.0 + double(FMath::Max(ShrinkSlackRatio, 0.0f));
	const bool bWorthShrinking =
		double(Resident->VertexCapacity) > double(TargetVertices) * SlackFactor
		|| double(Resident->IndexCapacity) > double(TargetIndices) * SlackFactor;
	if (!bWorthShrinking) return false;

	// Not budget-gated on purpose: the transient peak is bounded by the allocation that already
	// exists, and refusing the one operation that gives VRAM back would leave a mesh oversized
	// exactly when the device can least afford it.
	FCSMeshResident* ResidentPtr = Resident.Get();
	UE_LOG(LogCSMesh, Verbose, TEXT("[CSMesh] Shrinking %u/%u to %u/%u (live %u/%u)."),
		ResidentPtr->VertexCapacity, ResidentPtr->IndexCapacity, TargetVertices, TargetIndices, LiveVertices, LiveIndices);

	ENQUEUE_RENDER_COMMAND(CSMeshShrink)(
		[ResidentPtr, TargetVertices, TargetIndices](FRHICommandListImmediate& RHICmdList)
		{
			CSMesh_ReallocateResident(RHICmdList, *ResidentPtr, TargetVertices, TargetIndices);
		});
	CSMesh_CountedFlush();

	++ResidentPtr->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

// -----------------------------------------------------------------------------
// Stream layout and draw batches
// -----------------------------------------------------------------------------

bool UCSMesh::SetStreamLayoutSync(const FCSMeshStreamLayout& Layout)
{
	if (!IsInGameThread()) return false;
	if (!Resident.IsValid()) Resident = MakeShared<FCSMeshResident, ESPMode::ThreadSafe>();

	const uint32 NumIndirectDraws = FMath::Max(Layout.NumIndirectDraws, 1u);

	// Build and validate the whole declaration before touching the mesh: half an applied layout
	// on an allocated mesh is a buffer set nothing can draw from.
	TArray<FCSGpuStreamDesc> Descs;
	CSMesh_BuildStandardDescs(Descs, NumIndirectDraws, Layout.NumTexCoordSets);
	for (const FCSGpuStreamDesc& Extra : Layout.ExtraStreams) if (!CSMesh_AppendUniqueStreamDesc(Descs, Extra)) return false;

	// Re-declaring the same layout is how a consumer binds without having to know whether it is
	// the first one to do so, so it must not cost a reallocation.
	FCSMeshResident* ResidentPtr = Resident.Get();
	const bool bSameLayout = ResidentPtr->NumIndirectDraws == NumIndirectDraws && CSMesh_LayoutMatches(ResidentPtr->Streams, Descs);
	if (bSameLayout) return true;

	// Any buffer at all, not IsAllocated(): rebuilding the stream array here would drop the last
	// reference to a pooled buffer on the game thread, and render resources have to die on the
	// render thread. A partially declared set therefore goes through the reallocation too.
	bool bHoldsBuffers = false;
	for (const FCSMeshResident::FStream& Stream : ResidentPtr->Streams) bHoldsBuffers |= Stream.Pooled.IsValid();

	ResidentPtr->NumIndirectDraws = NumIndirectDraws;
	if (!bHoldsBuffers)
	{
		ResidentPtr->Streams.Reset(Descs.Num());
		for (const FCSGpuStreamDesc& Desc : Descs) ResidentPtr->Streams.AddDefaulted_GetRef().Desc = Desc;
		ResidentPtr->Sections.Reset();
		return true;
	}

	const uint32 VertexCapacity = ResidentPtr->VertexCapacity;
	const uint32 IndexCapacity = ResidentPtr->IndexCapacity;
	if (!ConfirmGpuMemoryBudget(CSMesh_RequiredBytesForDescs(Descs, VertexCapacity, IndexCapacity), TEXT("Stream layout"))) return false;

	ENQUEUE_RENDER_COMMAND(CSMeshSetStreamLayout)(
		[ResidentPtr, Descs, VertexCapacity, IndexCapacity](FRHICommandListImmediate& RHICmdList)
		{
			CSMesh_ReallocateResidentWithDescs(RHICmdList, *ResidentPtr, Descs, VertexCapacity, IndexCapacity);
		});
	CSMesh_CountedFlush();

	++ResidentPtr->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

bool UCSMesh::ResizeStreamsSync(const TArray<FCSMeshStreamResize>& Resizes)
{
	if (!IsInGameThread() || !Resident.IsValid()) return false;
	if (Resizes.Num() == 0) return true;

	FCSMeshResident* ResidentPtr = Resident.Get();
	const uint32 VertUnits = FMath::Max(ResidentPtr->VertexCapacity, CSMesh_MinCapacity);
	const uint32 IdxUnits = FMath::Max(ResidentPtr->IndexCapacity, CSMesh_MinCapacity);

	// Validate and cost the whole batch before touching anything. A half-applied resize leaves some
	// streams sized for the new count and some for the old, and the CPU-side strides that index them
	// can only be right for one of the two — which is a mesh that draws garbage, not one that errors.
	TArray<FCSMeshStreamResize> Pending;
	Pending.Reserve(Resizes.Num());
	int64 AddedBytes = 0;
	for (int32 Index = 0; Index < Resizes.Num(); ++Index)
	{
		const FCSMeshStreamResize& Resize = Resizes[Index];
		const int32 StreamIndex = ResidentPtr->FindStreamIndex(Resize.Role, Resize.SlotIndex);
		if (StreamIndex == INDEX_NONE)
		{
			UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream resize rejected: no stream holds role %u slot %u."),
				uint32(Resize.Role), uint32(Resize.SlotIndex));
			return false;
		}

		// A duplicate would allocate a buffer the next entry immediately throws away, and which of
		// the two sizes survived would come down to the caller's array order.
		bool bDuplicate = false;
		for (int32 Earlier = 0; Earlier < Index; ++Earlier) bDuplicate |= Resizes[Earlier].Role == Resize.Role && Resizes[Earlier].SlotIndex == Resize.SlotIndex;
		if (bDuplicate)
		{
			UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream resize rejected: role %u slot %u is named twice in one batch."),
				uint32(Resize.Role), uint32(Resize.SlotIndex));
			return false;
		}

		FCSGpuStreamDesc Wanted = ResidentPtr->Streams[StreamIndex].Desc;
		if (Resize.ElementCount == 0)
		{
			UE_LOG(LogCSMesh, Warning,
				TEXT("[CSMesh] Stream '%s' resize rejected: zero elements allocates nothing, which reports the whole mesh as unallocated."),
				Wanted.DebugName);
			return false;
		}
		if (Wanted.CountSource != ECSGpuCountSource::Fixed)
		{
			UE_LOG(LogCSMesh, Warning,
				TEXT("[CSMesh] Stream '%s' resize rejected: its size follows the mesh capacity, and ElementsPerUnit is the per-unit stride the vertex factory and the readback decode by. Use EnsureCapacitySync / ShrinkCapacitySync."),
				Wanted.DebugName);
			return false;
		}
		if (Wanted.Role == ECSGpuStreamRole::IndirectArgs || Wanted.Role == ECSGpuStreamRole::MeshCounters)
		{
			UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Stream '%s' resize rejected: %s."), Wanted.DebugName,
				Wanted.Role == ECSGpuStreamRole::IndirectArgs
					? TEXT("the indirect args carry NumIndirectDraws alongside the descriptor, so they are grown by EnsureIndirectDrawCapacitySync")
					: TEXT("the counters stream is two uints by contract with the counter readback"));
			return false;
		}

		if (Wanted.ElementsPerUnit == Resize.ElementCount) continue;

		Wanted.ElementsPerUnit = Resize.ElementCount;
		AddedBytes += int64(CSMesh_StreamBytes(Wanted, VertUnits, IdxUnits));
		Pending.Add(Resize);
	}
	if (Pending.Num() == 0) return true;

	// Costed as an addition on purpose: the buffers being replaced are only released once the
	// resize's graph has stopped referencing them, so both sets are held at the peak. A shrink is
	// therefore over-charged, which errs the safe way.
	if (!ConfirmGpuMemoryBudget(AddedBytes, TEXT("Stream resize"))) return false;

	// No buffer behind a stream yet: the declaration is all there is, and the next allocation sizes
	// the buffer from it. Keyed on each stream's own buffer rather than on IsAllocated(), because
	// widening a descriptor while a smaller buffer is still bound leaves every consumer reading
	// past the end of it.
	TArray<FCSMeshStreamResize> Reallocate;
	Reallocate.Reserve(Pending.Num());
	for (const FCSMeshStreamResize& Resize : Pending)
	{
		FCSMeshResident::FStream& Stream = ResidentPtr->Streams[ResidentPtr->FindStreamIndex(Resize.Role, Resize.SlotIndex)];
		if (Stream.Pooled.IsValid()) Reallocate.Add(Resize);
		else Stream.Desc.ElementsPerUnit = Resize.ElementCount;
	}
	if (Reallocate.Num() == 0) return true;

	ENQUEUE_RENDER_COMMAND(CSMeshResizeStreams)(
		[ResidentPtr, Reallocate](FRHICommandListImmediate& RHICmdList)
		{
			CSMesh_ReallocateStreams(RHICmdList, *ResidentPtr, Reallocate);
		});
	CSMesh_CountedFlush();

	++ResidentPtr->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

bool UCSMesh::ResizeStreamSync(ECSGpuStreamRole Role, uint8 SlotIndex, int32 NewElementCount)
{
	TArray<FCSMeshStreamResize> Resizes;
	FCSMeshStreamResize& Resize = Resizes.AddDefaulted_GetRef();
	Resize.Role = Role;
	Resize.SlotIndex = SlotIndex;
	Resize.ElementCount = uint32(FMath::Max(NewElementCount, 0));
	return ResizeStreamsSync(Resizes);
}

bool UCSMesh::EnsureIndirectDrawCapacitySync(int32 NumArgSets)
{
	if (!IsInGameThread() || !Resident.IsValid()) return false;

	const uint32 WantSets = uint32(FMath::Max(NumArgSets, 1));
	if (WantSets <= Resident->NumIndirectDraws) return true;
	if (WantSets > CSMesh_MaxIndirectArgSets)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Indirect-draw growth rejected: %d arg sets is past the %u ceiling."),
			NumArgSets, CSMesh_MaxIndirectArgSets);
		return false;
	}

	FCSMeshResident* ResidentPtr = Resident.Get();
	const int32 ArgsIndex = ResidentPtr->FindStreamIndex(ECSGpuStreamRole::IndirectArgs);
	if (ArgsIndex == INDEX_NONE)
	{
		UE_LOG(LogCSMesh, Warning, TEXT("[CSMesh] Indirect-draw growth rejected: the mesh declares no IndirectArgs stream."));
		return false;
	}

	// No buffer behind the stream yet: the declaration is all there is, and the next allocation
	// sizes the buffer from it. Keyed on this stream's own buffer rather than on IsAllocated(),
	// because widening the descriptor while a smaller buffer is still bound would leave every
	// consumer reading arg sets past the end of it.
	if (!ResidentPtr->Streams[ArgsIndex].Pooled.IsValid())
	{
		ResidentPtr->Streams[ArgsIndex].Desc.ElementsPerUnit = CSMesh_IndirectArgsUintsPerSet * WantSets;
		ResidentPtr->NumIndirectDraws = WantSets;
		return true;
	}

	ENQUEUE_RENDER_COMMAND(CSMeshGrowIndirectArgs)(
		[ResidentPtr, WantSets](FRHICommandListImmediate& RHICmdList)
		{
			CSMesh_ReallocateIndirectArgs(RHICmdList, *ResidentPtr, WantSets);
		});
	CSMesh_CountedFlush();

	++ResidentPtr->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

int32 UCSMesh::GetIndirectDrawCount() const
{
	return Resident.IsValid() ? int32(Resident->NumIndirectDraws) : 0;
}

TArray<FCSMeshSection> UCSMesh::GetSections() const
{
	return Resident.IsValid() ? Resident->Sections : TArray<FCSMeshSection>();
}

bool UCSMesh::SetSections(const TArray<FCSMeshSection>& InSections)
{
	if (!IsInGameThread() || !Resident.IsValid()) return false;

	if (uint32(InSections.Num()) > Resident->NumIndirectDraws)
	{
		UE_LOG(LogCSMesh, Warning,
			TEXT("[CSMesh] Section table refused: %d sections against %u indirect arg sets (call EnsureIndirectDrawCapacitySync first)."),
			InSections.Num(), Resident->NumIndirectDraws);
		return false;
	}

	for (const FCSMeshSection& Section : InSections)
	{
		if (Materials.IsValidIndex(Section.MaterialIndex)) continue;
		UE_LOG(LogCSMesh, Warning,
			TEXT("[CSMesh] Section material index %d is outside the %d-entry material table; that batch has no material to draw with."),
			Section.MaterialIndex, Materials.Num());
		break;
	}

	Resident->Sections = InSections;
	++Resident->Generation;
	OnMeshChanged.Broadcast(this);
	return true;
}

bool UCSMesh::ReadbackMeshSync(FCSGpuMeshCPUData& OutMeshData) const
{
	if (!Resident.IsValid()) return false;
	if (!CSMeshReadback::ReadbackResidentSync(*Resident, OutMeshData)) return false;
	OutMeshData.Materials = Materials;
	return true;
}

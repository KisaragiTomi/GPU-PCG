#include "CSGpuMeshSceneProxy.h"
#include "CSMesh.h"

#include "Components/PrimitiveComponent.h"
#include "ComponentRecreateRenderStateContext.h"   // r.CSGpuMesh.SurfaceCache: batches and cards are registered at scene add
#include "Engine/Engine.h"                          // GEngine->WireframeMaterial：线框视图模式
#include "Materials/Material.h"
#include "MeshCardBuild.h"                          // FCardRepresentationData; MeshCardRepresentation::SetCardsFromBounds
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "MaterialShared.h"
#include "MeshBatch.h"
#include "SceneManagement.h"
#include "SceneView.h"   // GetDynamicMeshElementsShadowCullFrustum: the shadow-depth-view test
#include "SceneInterface.h"
#include "RHICommandList.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"      // VelocityIncludeStationaryPrimitives
#include "RHIGPUReadback.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "PrimitiveUniformShaderParametersBuilder.h"
#include "RayTracingGeometry.h"
#include "RayTracingInstance.h"
#include "RenderingThread.h"
#include "RHI.h"

#if RHI_RAYTRACING
namespace { void CSGpuMesh_TrackRayTracingProxy(FCSGpuMeshSceneProxy* Proxy, bool bTrack); }
#endif

FCSGpuMeshSceneProxy::FCSGpuMeshSceneProxy(const UPrimitiveComponent* Component, UMaterialInterface* InMaterial, const char* DebugName)
	: FPrimitiveSceneProxy(Component)
	, VertexFactoryDebugName(DebugName)
	, Material(InMaterial)
{
	if (!Material) Material = UMaterial::GetDefaultMaterial(MD_Surface);
	MaterialRelevance = Material->GetRelevance_Concurrent(GetScene().GetShaderPlatform());

	bVerifyUsedMaterials = false;
	bSupportsDistanceFieldRepresentation = false;

	// Lumen decides whether to track a primitive from HasRayTracingRepresentation() and the
	// distance-field flags, and the base constructor evaluated that before this class's overrides
	// existed. Every engine proxy that changes those answers recomputes here
	// (StaticMeshSceneProxy.cpp:476, BaseDynamicMeshSceneProxy.cpp:70 in 5.7.4). A leaf that
	// overrides WantsRayTracingGeometry() is not visible from this constructor either and must
	// recompute again in its own.
	UpdateVisibleInLumenScene();

	// The surface cache's capture batches are collected again on a transform update and nowhere
	// else (RendererScene.cpp:5956-5960 in 5.7.4), and the owner sends one for exactly that — on
	// every content edit and after every landed readback. Neither moves the transform, and after a
	// content edit the bounds usually stay put too, which is precisely the update the engine drops
	// by default (r.SkipRedundantTransformUpdate, RendererScene.cpp:1600): the batches would keep
	// the old counts over rewritten indices.
	if (IsSurfaceCacheEnabled()) SetCanSkipRedundantTransformUpdates(false);
}

FCSGpuMeshSceneProxy::~FCSGpuMeshSceneProxy()
{
}

uint32 FCSGpuMeshSceneProxy::GetMemoryFootprint() const
{
	return sizeof(*this) + GetAllocatedSize() + Streams.GetAllocatedSize();
}

bool FCSGpuMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

void FCSGpuMeshSceneProxy::CreateRenderThreadResources(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::CreateRenderThreadResources(RHICmdList);
	InitGpuGeometry(RHICmdList);
#if RHI_RAYTRACING
	// Only registered here, not built: this can run on a parallel task, and the pump (render thread
	// proper, end of this frame) builds from whatever the mirror already holds — a proxy recreated
	// over a mesh with published counts gets its BLAS back one frame later, not never.
	if (WantsRayTracingGeometry()) CSGpuMesh_TrackRayTracingProxy(this, true);
#endif
}

void FCSGpuMeshSceneProxy::DestroyRenderThreadResources()
{
#if RHI_RAYTRACING
	// First: once this returns the proxy may be deleted, so the pump must no longer be able to
	// reach it. The BLAS goes before the buffers it was built over.
	CSGpuMesh_TrackRayTracingProxy(this, false);
	ReleaseRayTracingGeometry();
#endif
	// Release the vertex factory before the buffers it streams from (matches the
	// original per-proxy teardown order).
	if (VertexFactory)
	{
		VertexFactory->ReleaseResource();
		VertexFactory.Reset();
	}
	ReleaseGpuGeometry();
	FPrimitiveSceneProxy::DestroyRenderThreadResources();
}

void FCSGpuMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily, uint32 VisibilityMap, FMeshElementCollector& Collector) const
{
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;

	FCSGpuDrawArgs ShadowArgs;
	const bool bHaveShadowArgs = GetShadowDrawArgs(0, ShadowArgs);

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	SubmitGpuBufferDraw(*this, Views, VisibilityMap, Collector, *VertexFactory, *MaterialProxy,
		*DrawDesc.IndexBuffer, PT_TriangleList, DrawDesc.NumPrimitives, DrawDesc.MaxVertexIndex,
		bBatchCastShadow, DrawDesc.IndirectArgsBuffer, DrawDesc.IndirectArgsOffset,
		bHaveShadowArgs ? &ShadowArgs : nullptr);
}

bool FCSGpuMeshSceneProxy::GetShadowDrawArgs(int32 ArgSetIndex, FCSGpuDrawArgs& OutArgs) const
{
	return ExternalResident.IsValid() && ExternalResident->GetDrawArgs(ArgSetIndex, OutArgs);
}

void FCSGpuMeshSceneProxy::SubmitGpuBufferDraw(
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
	bool bCastShadow,
	FRHIBuffer* IndirectArgsBuffer,
	uint32 IndirectArgsOffset,
	const FCSGpuDrawArgs* ShadowArgs)
{

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1 << ViewIndex)) == 0) continue;

		// A shadow-depth gather is the one caller that cannot use our indirect args, and this is
		// how it identifies itself: FProjectedShadowInfo::GatherDynamicMeshElements sets the cull
		// frustum on its view before every gather (ShadowSetup.cpp:2763/2770/2778) and nothing
		// else in the renderer ever sets it — FSceneView leaves it null (SceneView.cpp:861).
		// Preferred over asking "is this a VSM": the direct batch is equally correct for a plain
		// shadow map, and a proxy cannot see which kind of shadow map it is being gathered for.
		const bool bShadowDepthView = Views[ViewIndex]->GetDynamicMeshElementsShadowCullFrustum() != nullptr;
		// No CPU copy yet ⇒ fall back to the indirect form, which is the pre-existing behaviour:
		// right in a cascaded shadow map, empty in a virtual one. Better than drawing a guessed
		// count, because index slots past the real end hold whatever the last, larger mesh left
		// there — garbage triangles in the shadow map instead of a missing shadow.
		const bool bDirectShadowDraw = bShadowDepthView && ShadowArgs != nullptr && ShadowArgs->IsDrawable();

		FMeshBatch& Mesh = Collector.AllocateMesh();
		Mesh.VertexFactory = &InVertexFactory;
		Mesh.MaterialRenderProxy = &MaterialProxy;
		Mesh.ReverseCulling = SceneProxy.IsLocalToWorldDeterminantNegative();
		Mesh.Type = PrimitiveType;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastShadow = bCastShadow;

#if !UE_BUILD_TEST
		// 线框视图模式：上面关掉了引擎的通用视图模式覆盖，线框这一项得自己接，否则线框模式下
		// 整块画成黑色实体。写法同 ApplyViewModeOverrides（PrimitiveDrawingUtils.cpp:1627）：
		// 材质改顶点位置（WPO）时不能整个换成线框材质，否则线框跟不上形变。
		const FEngineShowFlags& ShowFlags = Views[ViewIndex]->Family->EngineShowFlags;
		if (!bShadowDepthView && ShowFlags.Wireframe && AllowDebugViewmodes())
		{
			const FLinearColor WireColor = GetSelectionColor(ShowFlags.ActorColoration ? SceneProxy.GetPrimitiveColor() : SceneProxy.GetWireframeColor(),
				SceneProxy.IsSelected(), SceneProxy.IsHovered(), false);
			const bool bModifiesPosition = MaterialProxy.GetIncompleteMaterialWithFallback(Views[ViewIndex]->GetFeatureLevel()).MaterialModifiesMeshPosition_RenderThread();
			FMaterialRenderProxy* WireframeProxy = bModifiesPosition
				? static_cast<FMaterialRenderProxy*>(new FOverrideSelectionColorMaterialRenderProxy(&MaterialProxy, WireColor))
				: static_cast<FMaterialRenderProxy*>(new FColoredMaterialRenderProxy(GEngine->WireframeMaterial->GetRenderProxy(), WireColor));
			Collector.RegisterOneFrameMaterialProxy(WireframeProxy);
			Mesh.MaterialRenderProxy = WireframeProxy;
			Mesh.bWireframe = true;
		}
#endif

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = &IndexBuffer;
		BatchElement.FirstIndex = 0;
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = MaxVertexIndex;
		// The two forms are mutually exclusive, and the engine enforces it in both directions
		// (MeshPassProcessor.cpp:930/934): an args buffer demands NumPrimitives == 0, and a
		// non-zero NumPrimitives demands no args buffer.
		if (bDirectShadowDraw)
		{
			// FirstIndex has to come across too, and not only for the direct draw: instance
			// culling copies it into the args it substitutes (AllocateIndirectArgs,
			// InstanceCullingContext.cpp:275). A section drawing from arg set i starts at that
			// set's own StartIndexLocation, so taking the count alone would cast every section's
			// shadow from the front of the mesh.
			BatchElement.NumPrimitives = ShadowArgs->IndexCount / 3u;
			BatchElement.FirstIndex = ShadowArgs->FirstIndex;
			BatchElement.BaseVertexIndex = uint32(FMath::Max(ShadowArgs->BaseVertexIndex, 0));
		}
		else if (IndirectArgsBuffer)
		{
			BatchElement.IndirectArgsBuffer = IndirectArgsBuffer;
			BatchElement.IndirectArgsOffset = IndirectArgsOffset;
			BatchElement.NumPrimitives = 0; // 0 => use IndirectArgsBuffer
		}
		else
		{
			BatchElement.NumPrimitives = NumPrimitives;
		}

		FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer =
			Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
		DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), SceneProxy.GetLocalToWorld(), SceneProxy.GetLocalToWorld(),
			SceneProxy.GetBounds(), SceneProxy.GetLocalBounds(), SceneProxy.GetLocalBounds(), SceneProxy.ReceivesDecals(), false, false, nullptr);
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;

		Collector.AddMesh(ViewIndex, Mesh);
	}
}

FPrimitiveViewRelevance FCSGpuMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bStaticRelevance = false;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();
	MaterialRelevance.SetPrimitiveViewRelevance(Result);
	// 必须与引擎预通道 / 速度通道的分工一致（公式抄 FStaticMeshSceneProxy::GetViewRelevance）。
	// 全深度预通道是 DDM_AllOpaqueNoVelocity 时（本工程 r.VelocityOutputPass=0 就是这一档），
	// FDepthPassMeshProcessor::AddMeshBatch 会按 DrawsVelocity() / AlwaysHasVelocity() 把"本帧有速度"
	// 的图元踢出预通道，指望速度通道替它写深度；而基通道是 DepthRead，谁都不写。这里曾硬写 false，
	// 结果材质接了 WPO（草的风动，代理构造时就带上 bHasWorldPositionOffsetVelocity）或 Movable 组件
	// 变换变化的那一帧，深度缓冲里根本没有它。实测症状（2026-09-05）：草在天空背景下被大气 / 雾按远平面
	// 合成成半透明白片，背景是地面时借地面深度"看着正常"、且后画的花把它整片盖掉；拖动 actor 时
	// 花、石阶、石子同样丢深度一帧。依赖 bOpaque，所以必须放在 SetPrimitiveViewRelevance 之后。
	// 速度着色器对两面 / WPO 材质本来就在 shader map 里（TVelocityVS::ShouldCompilePermutation），不需要新编译。
	Result.bVelocityRelevance = (VelocityIncludeStationaryPrimitives(View->GetShaderPlatform()) || DrawsVelocity())
		&& Result.bOpaque && Result.bRenderInMainPass;
	return Result;
}

// -----------------------------------------------------------------------------
// Descriptor-driven buffer set
// -----------------------------------------------------------------------------

void FCSGpuMeshSceneProxy::AddStream(const FCSGpuStreamDesc& Desc)
{
	TUniquePtr<FCSGpuStreamRuntime> Runtime = MakeUnique<FCSGpuStreamRuntime>();
	Runtime->Desc = Desc;
	Streams.Add(MoveTemp(Runtime));
}

const FCSGpuMeshSceneProxy::FCSGpuStreamRuntime* FCSGpuMeshSceneProxy::FindStream(ECSGpuStreamRole Role, uint8 Index) const
{
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
		if (S->Desc.Role == Role && S->Desc.TexCoordIndex == Index) return S.Get();
	return nullptr;
}

FRHIShaderResourceView* FCSGpuMeshSceneProxy::GetStreamSRV(ECSGpuStreamRole Role, uint8 Index) const
{
	const FCSGpuStreamRuntime* S = FindStream(Role, Index);
	return S ? S->SRV.GetReference() : nullptr;
}

TUniquePtr<FLocalVertexFactory> FCSGpuMeshSceneProxy::CreateVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName) const
{
	return MakeUnique<FLocalVertexFactory>(InFeatureLevel, InDebugName);
}

void FCSGpuMeshSceneProxy::BuildResidentView(FCSMeshResident& OutResident) const
{
	OutResident.Streams.Reset(Streams.Num());
	for (const TUniquePtr<FCSGpuStreamRuntime>& S : Streams)
	{
		FCSMeshResident::FStream& Stream = OutResident.Streams.AddDefaulted_GetRef();
		Stream.Desc = S->Desc;
		Stream.Pooled = S->Pooled;
	}
	OutResident.VertexCapacity = VertexCapacity;
	OutResident.IndexCapacity = IndexCapacity;
}

void FCSGpuMeshSceneProxy::SetExternalStreams(TSharedPtr<FCSMeshResident, ESPMode::ThreadSafe> InResident)
{
	ExternalResident = MoveTemp(InResident);
}

void FCSGpuMeshSceneProxy::InitGpuGeometry(FRHICommandListBase& RHICmdList)
{
	Streams.Reset();
	VertexCapacity = 0;
	IndexCapacity = 0;
	DrawDesc = FDrawDesc();

	if (ExternalResident.IsValid())
	{
		// Adopt: the buffers already exist and already hold geometry, so there is nothing to
		// allocate and nothing to generate. This is what turns a render-state recreation from
		// "re-run the generation compute" into "rebind the vertex factory".
		for (const FCSMeshResident::FStream& Stream : ExternalResident->Streams)
		{
			AddStream(Stream.Desc);
			Streams.Last()->Pooled = Stream.Pooled;
		}
		VertexCapacity = ExternalResident->VertexCapacity;
		IndexCapacity = ExternalResident->IndexCapacity;
		AllocateStreamsAndBindVF(RHICmdList, /*bAllocateBuffers*/ false);
		return;
	}

	RegisterStreams();                    // leaf: push descriptors + set capacities
	AllocateStreamsAndBindVF(RHICmdList); // base: alloc pooled buffers + SRVs + VF + DrawDesc handles
	BuildGeometry(RHICmdList);            // leaf: run compute into the base-owned buffers
}

void FCSGpuMeshSceneProxy::AllocateStreamsAndBindVF(FRHICommandListBase& RHICmdList, bool bAllocateBuffers)
{
	const uint32 VertUnits = FMath::Max(VertexCapacity, 1u);
	const uint32 IdxUnits = FMath::Max(IndexCapacity, 1u);

	// Leaf-selected vertex factory; created here (not in the ctor) so the virtual dispatch works.
	if (!VertexFactory) VertexFactory = CreateVertexFactory(GetScene().GetFeatureLevel(), VertexFactoryDebugName);

	FLocalVertexFactory::FDataType Data;

	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		const FCSGpuStreamDesc& D = S.Desc;

		// --- allocate the pooled buffer (adopt mode arrives with one already attached)
		if (bAllocateBuffers)
		{
			FRDGBufferDesc Desc;
			if (D.Role == ECSGpuStreamRole::IndirectArgs)
			{
				Desc = FRDGBufferDesc::CreateIndirectDesc(D.BytesPerElement, D.ElementsPerUnit);
			}
			else
			{
				const uint32 Units = FMath::Max(CSGpuMeshStreams::UnitsForCountSource(D.CountSource, VertUnits, IdxUnits), 1u);
				Desc = FRDGBufferDesc::CreateBufferDesc(D.BytesPerElement, D.ElementsPerUnit * Units);
				if (D.Role == ECSGpuStreamRole::Index)
					Desc.Usage = (Desc.Usage & ~EBufferUsageFlags::VertexBuffer) | EBufferUsageFlags::IndexBuffer;
			}
			S.Pooled = AllocatePooledBuffer(Desc, D.DebugName);
		}
		if (!S.Pooled.IsValid()) continue;

		// --- manual-fetch SRV
		if (D.SrvFormat != PF_Unknown)
			S.SRV = RHICmdList.CreateShaderResourceView(S.Pooled->GetRHI(),
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(D.SrvFormat));

		// --- render-resource wrappers for the VF streams / index buffer
		if (D.Role == ECSGpuStreamRole::Index)
		{
			S.IB.Pooled = S.Pooled;
			S.IB.InitResource(RHICmdList);
		}
		else if (D.VfType != VET_None)
		{
			S.VB.Pooled = S.Pooled;
			S.VB.InitResource(RHICmdList);
		}

		// --- vertex-factory binding
		const uint32 VertexStride = D.BytesPerElement * D.ElementsPerUnit;
		switch (D.Role)
		{
		case ECSGpuStreamRole::Position:
			Data.PositionComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, D.VfType);
			Data.PositionComponentSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TangentBasis:
			Data.TangentBasisComponents[0] = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_PackedNormal);
			Data.TangentBasisComponents[1] = FVertexStreamComponent(&S.VB, sizeof(uint32), VertexStride, VET_PackedNormal);
			Data.TangentsSRV = S.SRV;
			break;
		case ECSGpuStreamRole::TexCoord:
		{
			// 一条流可以承载多组 UV（交错，每组 2 个 float）—— 见 FStandardStreamOptions::NumTexCoordSets。
			// 逐组各挂一个 stream component（偏移 8×组号、步长 = 整条 unit），SRV 只有一个槽位、
			// 设一次即可：引擎的 manual fetch 正是按 NumTexCoords 做交错索引的。
			//
			// ⚠️ **stream component 最多只挂 4 个，而 NumTexCoords 可以到 8**，两个数不是一回事：
			// `FStaticMeshDataType::TextureCoordinates` 是 `TFixedAllocator<MAX_STATIC_TEXCOORDS / 2>`
			// （引擎 `Components.h:46`）—— 第 5 个 Add 当场撑爆固定分配器；顶点声明那边也只排属性
			// 4..7（`LocalVertexFactory.cpp:530-549`，不足 4 个还拿最后一个补齐），引擎自己同样把
			// 声明侧钳在 `MAX_TEXCOORDS = 4`（`StaticMeshVertexBuffer.cpp:626`）。
			// 第 5..8 组只经 manual fetch 的 SRV 到达 shader：`MANUAL_VERTEX_FETCH` 在
			// `RHISupportsManualVertexFetch()` 成立时恒开（`LocalVertexFactory.cpp:306-308`），而本插件
			// 只面向 SM5+（`CSGpuInstancedMeshVertexFactory.h:18-19`）⇒ 恒成立。要往非 SM5 平台移植
			// 时，UV4..7 会静默读不到，得先把这条钳位的前提重新论证。
			const int32 NumSets = FMath::Max(int32(D.ElementsPerUnit) / 2, 1);
			const int32 NumDeclaredSets = FMath::Min(NumSets, int32(MAX_STATIC_TEXCOORDS) / 2);
			for (int32 Set = 0; Set < NumDeclaredSets; ++Set)
			{
				Data.TextureCoordinates.Add(FVertexStreamComponent(
					&S.VB, uint32(Set) * 2u * sizeof(float), VertexStride, VET_Float2));
			}
			Data.TextureCoordinatesSRV = S.SRV;
			const int32 DesiredTexCoords = int32(D.TexCoordIndex) + NumSets;
			if (int32(Data.NumTexCoords) < DesiredTexCoords) Data.NumTexCoords = DesiredTexCoords;
			if (D.TexCoordIndex == 0)
			{
				Data.LightMapCoordinateIndex = 0;
				Data.LightMapCoordinateComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Float2);
			}
			break;
		}
		case ECSGpuStreamRole::Color:
			Data.ColorComponent = FVertexStreamComponent(&S.VB, 0, VertexStride, VET_Color);
			Data.ColorComponentsSRV = S.SRV;
			Data.ColorIndexMask = ~0u;
			break;
		case ECSGpuStreamRole::Index:
			DrawDesc.IndexBuffer = &S.IB;
			break;
		case ECSGpuStreamRole::IndirectArgs:
			DrawDesc.IndirectArgsBuffer = S.Pooled->GetRHI();
			DrawDesc.IndirectArgsOffset = 0;
			break;
		default:
			break; // MeshCounters, AuxVertex: no VF / draw binding
		}
	}

	OnStreamsAllocated(RHICmdList);

	VertexFactory->SetData(RHICmdList, Data);
	VertexFactory->InitResource(RHICmdList);

	DrawDesc.FirstIndex = 0;
	DrawDesc.MinVertexIndex = 0;
	DrawDesc.MaxVertexIndex = VertUnits - 1;
	DrawDesc.bValid = (DrawDesc.IndexBuffer != nullptr);
}

void FCSGpuMeshSceneProxy::ReleaseGpuGeometry()
{
	DrawDesc = FDrawDesc();
	for (TUniquePtr<FCSGpuStreamRuntime>& SPtr : Streams)
	{
		FCSGpuStreamRuntime& S = *SPtr;
		// Release exactly the wrappers that were initialised in AllocateStreamsAndBindVF.
		if (S.Desc.Role == ECSGpuStreamRole::Index) S.IB.ReleaseResource();
		else if (S.Desc.VfType != VET_None) S.VB.ReleaseResource();
		S.SRV.SafeRelease();
		S.Pooled.SafeRelease();
	}
	Streams.Reset();
}

void FCSGpuMeshSceneProxy::GetBatchMaterials(TArray<FMaterialRenderProxy*, TInlineAllocator<8>>& OutMaterials) const
{
	OutMaterials.Reset(1);
	OutMaterials.Add(Material->GetRenderProxy());
}

bool FCSGpuMeshSceneProxy::GatherDrawArgs(TArray<FCSGpuDrawArgs, TInlineAllocator<8>>& OutArgs, uint32& OutSerial) const
{
	OutArgs.Reset();
	if (ExternalResident.IsValid())
	{
		// External mode: the counts are the resident set's mirror, one arg set per draw batch.
		const uint32 Serial = ExternalResident->GetDrawArgsPublishSerial();
		if (Serial == 0) return false;

		TArray<FMaterialRenderProxy*, TInlineAllocator<8>> Materials;
		GetBatchMaterials(Materials);
		for (int32 BatchIndex = 0; BatchIndex < Materials.Num(); ++BatchIndex)
		{
			// A set the mirror does not cover (retired by an edit in flight, or a table longer than
			// the args) makes the whole mesh unknown: a BLAS with a guessed segment is a wrong BLAS.
			if (!ExternalResident->GetDrawArgs(BatchIndex, OutArgs.AddDefaulted_GetRef())) return false;
		}
		OutSerial = Serial;
		return OutArgs.Num() > 0;
	}

	// Owned mode. A leaf that draws a CPU-known count is fully described by DrawDesc and never
	// changes for the proxy's life; a leaf that draws indirect has no mirror to read the count from.
	if (DrawDesc.IndirectArgsBuffer != nullptr || DrawDesc.NumPrimitives == 0) return false;
	FCSGpuDrawArgs& Args = OutArgs.AddDefaulted_GetRef();
	Args.IndexCount = DrawDesc.NumPrimitives * 3u;
	Args.FirstIndex = DrawDesc.FirstIndex;
	Args.BaseVertexIndex = 0;
	OutSerial = 1;
	return true;
}

// -----------------------------------------------------------------------------
// Lumen surface cache: cards from the bounds, capture batches from the published counts
// -----------------------------------------------------------------------------
//
// What the BLAS could not do on its own (measured 2026-09-11, L_HouseGroundDemo): with the
// project's default surface-cache lighting a hardware Lumen ray that hits a card-less primitive
// gets bValid=false and radiance 0 (LumenSurfaceCacheSampling.ush:585), and the GI trace writes
// that 0 straight out — there is no retrace for invalid hits on the GI side, only for reflections.
// The GPU meshes occluded and bounced nothing; a wall facing sunlit ground sat at the same
// brightness as one facing black.
//
// Why cards from the bounds rather than a real card build: the engine's card generator is an
// editor-only offline build over a static mesh's triangles, and these triangles live on the GPU.
// Six axis-aligned cards over the bounds is what the engine itself gives skeletal meshes
// (SkeletalMeshSceneProxy.cpp:670). The price is coverage: a face hidden from all six directions
// (the back of a window recess, the underside of a house on stilts) has no texels and stays as it
// was — black — while every outward face gets real lit colour.
//
// Who keeps it fresh: the counts behind the batches change with every landed readback, and the
// engine only asks for static batches when the primitive is added or its transform updated. The
// proxy cannot make either happen from the render thread, so UCSMeshRenderComponent watches the
// publish serial and pushes a transform update plus a surface-cache invalidation when it moves.

static TAutoConsoleVariable<int32> CVarCSGpuMeshSurfaceCache(
	TEXT("r.CSGpuMesh.SurfaceCache"),
	1,
	TEXT("Give GPU-resident meshes a Lumen surface cache: six mesh cards over their bounds, captured from their published triangles, so hardware Lumen rays that hit them pick up their lit colour and bounce it.\n")
	TEXT("0 leaves them occlusion-only (hits shade black under surface-cache lighting). Changing it recreates every render state."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable*)
	{
		// Cards and capture batches are registered when the primitive is added to the scene.
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_RenderThreadSafe);

bool FCSGpuMeshSceneProxy::IsSurfaceCacheEnabled()
{
	return CVarCSGpuMeshSurfaceCache.GetValueOnAnyThread() != 0;
}

bool FCSGpuMeshSceneProxy::WantsSurfaceCache() const
{
	return IsSurfaceCacheEnabled() && WantsRayTracingGeometry() && IsVisibleInLumenScene() && AffectsDynamicIndirectLighting();
}

void FCSGpuMeshSceneProxy::UpdateCardRepresentation()
{
	const FBox LocalBox = GetLocalBounds().GetBox();
	if (!WantsSurfaceCache() || !LocalBox.IsValid)
	{
		CardRepresentation.Reset();
		return;
	}
	if (CardRepresentation && CardRepresentation->MeshCardsBuildData.Bounds.Equals(LocalBox)) return;

	// A fresh object rather than an edit in place: each one carries its own CardRepresentationDataId,
	// and Lumen copies what it needs out of it when it builds the card set — nothing holds on to the
	// old one past this point, which is the render thread (SetTransform) like every reader.
	TUniquePtr<FCardRepresentationData> Cards = MakeUnique<FCardRepresentationData>();
	Cards->MeshCardsBuildData.Bounds = LocalBox;
	// Not two-sided: these meshes are closed or face outward, and the flag would add a 50-unit
	// sampling bias meant for foliage (LumenSurfaceCacheSampling.ush:451).
	Cards->MeshCardsBuildData.bMostlyTwoSided = false;
	MeshCardRepresentation::SetCardsFromBounds(Cards->MeshCardsBuildData);
	CardRepresentation = MoveTemp(Cards);
}

void FCSGpuMeshSceneProxy::OnTransformChanged(FRHICommandListBase& RHICmdList)
{
	FPrimitiveSceneProxy::OnTransformChanged(RHICmdList);
	// SetTransform stores the new local bounds right before calling this, on every transform
	// update including the first one at scene add, so the cards can never describe older bounds
	// than the proxy's own.
	UpdateCardRepresentation();
}

const FCardRepresentationData* FCSGpuMeshSceneProxy::GetMeshCardRepresentation() const
{
	return CardRepresentation.Get();
}

void FCSGpuMeshSceneProxy::DrawStaticElements(FStaticPrimitiveDrawInterface* PDI)
{
	SurfaceCacheBatchSerial = 0;
	if (!WantsSurfaceCache()) return;
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory || !VertexFactory->IsInitialized()) return;

	TArray<FCSGpuDrawArgs, TInlineAllocator<8>> Args;
	uint32 Serial = 0;
	if (!GatherDrawArgs(Args, Serial)) return;   // counts unknown: register nothing rather than guess

	TArray<FMaterialRenderProxy*, TInlineAllocator<8>> Materials;
	GetBatchMaterials(Materials);
	// Batch i is matched to arg set i by index; a split the args do not describe is skipped whole,
	// exactly as the BLAS instance does.
	if (Materials.Num() != Args.Num()) return;

	PDI->ReserveMemoryForMeshes(Args.Num());
	for (int32 BatchIndex = 0; BatchIndex < Args.Num(); ++BatchIndex)
	{
		const FCSGpuDrawArgs& BatchArgs = Args[BatchIndex];
		const uint32 NumTriangles = BatchArgs.IndexCount / 3u;
		if (NumTriangles == 0) continue;

		FMeshBatch Mesh;
		Mesh.VertexFactory = VertexFactory.Get();
		Mesh.MaterialRenderProxy = Materials[BatchIndex];
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.LODIndex = 0;
		Mesh.MeshIdInPrimitive = uint16(BatchIndex);
		Mesh.bCanApplyViewModeOverrides = false;
		// The card capture takes a non-landscape primitive's bUseForMaterial batches
		// (LumenSceneCardCapture.cpp:861). Every other pass this batch could be cached for is
		// switched off, so the only cost it leaves behind is the capture command itself.
		Mesh.bUseForMaterial = true;
		Mesh.bUseForDepthPass = false;
		Mesh.bUseAsOccluder = false;
		Mesh.CastShadow = false;
		Mesh.bSelectable = false;
#if RHI_RAYTRACING
		Mesh.CastRayTracedShadow = false;
#endif

		FMeshBatchElement& Element = Mesh.Elements[0];
		Element.IndexBuffer = DrawDesc.IndexBuffer;
		// FirstIndex and BaseVertexIndex come across with the count for the reason the shadow batch
		// takes them: with sections, set i describes its own run of the shared index buffer.
		Element.FirstIndex = BatchArgs.FirstIndex;
		Element.NumPrimitives = NumTriangles;
		Element.BaseVertexIndex = uint32(FMath::Max(BatchArgs.BaseVertexIndex, 0));
		Element.MinVertexIndex = 0;
		Element.MaxVertexIndex = DrawDesc.MaxVertexIndex;

		PDI->DrawMesh(Mesh, 1.0f);
	}
	SurfaceCacheBatchSerial = Serial;
}

// -----------------------------------------------------------------------------
// Ray tracing: a BLAS over the raster buffers, rebuilt when the draw-args mirror lands
// -----------------------------------------------------------------------------
//
// Why the BLAS follows the published mirror and not the buffers directly: an acceleration
// structure build needs its primitive count on the CPU (FRayTracingGeometrySegment::NumPrimitives
// — the RHI has no indirect BLAS build, only indirect TLAS instance data), and for a GPU-decided
// mesh the only CPU copy of that count is the draw-args mirror FCSMeshResident publishes. So the
// BLAS is (re)built when a readback lands, which is also exactly when the buffers behind the counts
// have finished changing. While a readback is in flight the previous BLAS stays: it is a baked
// structure and does not read the buffers again, so a mesh edited every frame keeps last frame's
// ray tracing shape until the edits pause, rather than vanishing or being rebuilt per frame. (The
// mirror's supersede-in-flight policy, CSMesh.cpp RequestDrawArgsReadback, is what makes this a
// natural throttle: continuous edits starve the publish until they stop.)
//
// Why an end-of-frame pump and not the gather or CreateRenderThreadResources: both of those can
// run inside a ParallelFor (RayTracing.cpp:1185 in 5.7.4), and creating / releasing render
// resources from there needs more care than this deserves. FCoreDelegates::OnEndFrameRT is the
// render thread proper, once per frame, and runs after the draw-args pump in CSMesh.cpp has
// published (it was registered later), so a landed readback becomes a BLAS in the same frame.
//
// What it costs: one full fast-build per landed edit, over the whole mesh, whatever changed. A
// vertex-colour paint rebuilds a million-triangle ground exactly like a displacement does. Refit
// (bAllowUpdate + EAccelerationStructureBuildMode::Update) would make same-count edits an order of
// magnitude cheaper, but FRayTracingGeometryManager::RequestBuildAccelerationStructure ignores the
// build mode in 5.7.4, so that needs its own scratch-managed build call — deferred to the frame
// quota work (Docs/FrameQuotaScheduler_Plan.md).

#if RHI_RAYTRACING

static TAutoConsoleVariable<int32> CVarCSGpuMeshRayTracing(
	TEXT("r.CSGpuMesh.RayTracing"),
	1,
	TEXT("Build a ray tracing BLAS for GPU-resident meshes so hardware Lumen, ray traced shadows and reflections see them.\n")
	TEXT("0 drops every existing BLAS at the next end of frame and builds no new ones."),
	ECVF_RenderThreadSafe);

namespace
{
/** Proxies with live render-thread resources. Registration and removal come from
 *  Create/DestroyRenderThreadResources, which the scene may run from parallel tasks, hence the
 *  lock; removal precedes the proxy's deletion, which is what keeps the raw pointers valid. */
FCriticalSection GCSGpuMeshRayTracingProxiesLock;
TArray<FCSGpuMeshSceneProxy*> GCSGpuMeshRayTracingProxies;
FDelegateHandle GCSGpuMeshRayTracingPumpHandle;

void CSGpuMesh_TrackRayTracingProxy(FCSGpuMeshSceneProxy* Proxy, bool bTrack)
{
	FScopeLock Lock(&GCSGpuMeshRayTracingProxiesLock);
	if (bTrack) GCSGpuMeshRayTracingProxies.AddUnique(Proxy);
	else GCSGpuMeshRayTracingProxies.RemoveSingleSwap(Proxy);
}
}

void FCSGpuMeshSceneProxy::RegisterRayTracingPump()
{
	// The delegate itself is only touched on the render thread: TMulticastDelegate is not safe
	// against a concurrent broadcast, and OnEndFrameRT broadcasts there. Before the rendering
	// thread exists this runs inline, which is the same thread.
	ENQUEUE_RENDER_COMMAND(CSGpuMeshRegisterRayTracingPump)([](FRHICommandListImmediate&)
	{
		if (GCSGpuMeshRayTracingPumpHandle.IsValid()) return;
		GCSGpuMeshRayTracingPumpHandle = FCoreDelegates::OnEndFrameRT.AddStatic(&FCSGpuMeshSceneProxy::PumpRayTracingGeometries_RenderThread);
	});
}

void FCSGpuMeshSceneProxy::UnregisterRayTracingPump()
{
	ENQUEUE_RENDER_COMMAND(CSGpuMeshUnregisterRayTracingPump)([](FRHICommandListImmediate&)
	{
		if (!GCSGpuMeshRayTracingPumpHandle.IsValid()) return;
		FCoreDelegates::OnEndFrameRT.Remove(GCSGpuMeshRayTracingPumpHandle);
		GCSGpuMeshRayTracingPumpHandle.Reset();
	});
	// The pump points into this module: it has to be gone before the module is (hot reload).
	FlushRenderingCommands();
}

/** `CSGpuMesh.DumpRayTracing`: logs every tracked proxy's BLAS state from the render thread, so a
 *  live editor can be asked "is the mesh actually in the ray tracing scene right now" without a
 *  debugger. Companion of r.CSGpuMesh.RayTracing. */
static FAutoConsoleCommand GCSGpuMeshDumpRayTracingCmd(
	TEXT("CSGpuMesh.DumpRayTracing"),
	TEXT("Log the ray tracing BLAS state of every live GPU-mesh scene proxy (segments, primitives, publish serial)."),
	FConsoleCommandDelegate::CreateLambda([]()
	{
		ENQUEUE_RENDER_COMMAND(CSGpuMeshDumpRayTracing)([](FRHICommandListImmediate&)
		{
			FScopeLock Lock(&GCSGpuMeshRayTracingProxiesLock);
			UE_LOG(LogTemp, Display, TEXT("[CSGpuMesh.RT] %d tracked proxies, r.CSGpuMesh.RayTracing=%d, IsRayTracingAllowed=%d"),
				GCSGpuMeshRayTracingProxies.Num(), CVarCSGpuMeshRayTracing.GetValueOnRenderThread(), IsRayTracingAllowed() ? 1 : 0);
			for (const FCSGpuMeshSceneProxy* Proxy : GCSGpuMeshRayTracingProxies)
			{
				const FRayTracingGeometry* Geometry = Proxy->GetRayTracingGeometryForTest();
				if (!Geometry)
				{
					UE_LOG(LogTemp, Display, TEXT("[CSGpuMesh.RT]   %s: no BLAS (serial %u)"), *Proxy->GetOwnerName().ToString(), Proxy->GetRayTracingBuiltSerialForTest());
					continue;
				}
				uint32 Enabled = 0;
				for (const FRayTracingGeometrySegment& Segment : Geometry->Initializer.Segments) Enabled += Segment.bEnabled ? 1u : 0u;
				UE_LOG(LogTemp, Display, TEXT("[CSGpuMesh.RT]   %s: BLAS valid=%d segments=%d (enabled %u) primitives=%u serial=%u pendingBuild=%d"),
					*Proxy->GetOwnerName().ToString(), Geometry->IsValid() ? 1 : 0, Geometry->Initializer.Segments.Num(), Enabled,
					Geometry->Initializer.TotalPrimitiveCount, Proxy->GetRayTracingBuiltSerialForTest(), Geometry->HasPendingBuildRequest() ? 1 : 0);
			}
		});
	}));

void FCSGpuMeshSceneProxy::PumpRayTracingGeometries_RenderThread()
{
	check(IsInRenderingThread());
	FScopeLock Lock(&GCSGpuMeshRayTracingProxiesLock);
	if (GCSGpuMeshRayTracingProxies.IsEmpty()) return;
	FRHICommandListImmediate& RHICmdList = GetImmediateCommandList_ForRenderCommand();
	for (FCSGpuMeshSceneProxy* Proxy : GCSGpuMeshRayTracingProxies) Proxy->RefreshRayTracingGeometry(RHICmdList);
}

void FCSGpuMeshSceneProxy::ReleaseRayTracingGeometry()
{
	if (RayTracingGeometry)
	{
		RayTracingGeometry->ReleaseResource();
		RayTracingGeometry.Reset();
	}
	RayTracingBuiltArgs.Reset();
	RayTracingBuiltSerial = 0;
}

void FCSGpuMeshSceneProxy::RefreshRayTracingGeometry(FRHICommandListBase& RHICmdList)
{
	if (CVarCSGpuMeshRayTracing.GetValueOnRenderThread() == 0 || !IsRayTracingAllowed() || !WantsRayTracingGeometry())
	{
		ReleaseRayTracingGeometry();
		return;
	}
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr) return;

	TArray<FCSGpuDrawArgs, TInlineAllocator<8>> Args;
	uint32 Serial = 0;
	if (!GatherDrawArgs(Args, Serial)) return;   // counts unknown: keep the baked BLAS we have
	if (Serial == RayTracingBuiltSerial) return;           // nothing landed since the last build

	const FCSGpuStreamRuntime* PositionStream = FindStream(ECSGpuStreamRole::Position);
	const FCSGpuStreamRuntime* IndexStream = FindStream(ECSGpuStreamRole::Index);
	if (!PositionStream || !IndexStream || !PositionStream->Pooled.IsValid() || !IndexStream->Pooled.IsValid()) return;

	// The Position stream is VET_Float3 at 12 bytes (BuildStandardTriangleStreamDescs); read the
	// stride from the descriptor anyway so a layout change cannot silently misread positions.
	const uint32 VertexStride = PositionStream->Desc.BytesPerElement * PositionStream->Desc.ElementsPerUnit;

	FRayTracingGeometryInitializer Initializer;
	Initializer.DebugName = FName(VertexFactoryDebugName);
	Initializer.IndexBuffer = IndexStream->Pooled->GetRHI();
	Initializer.GeometryType = RTGT_Triangles;
	// Rebuilt on every landed edit: build speed over trace speed, same choice as the engine's
	// procedural mesh and landscape geometries.
	Initializer.bFastBuild = true;
	Initializer.bAllowUpdate = false;

	uint32 TotalPrimitives = 0;
	for (const FCSGpuDrawArgs& BatchArgs : Args)
	{
		const uint32 BaseVertex = uint32(FMath::Max(BatchArgs.BaseVertexIndex, 0));
		FRayTracingGeometrySegment& Segment = Initializer.Segments.AddDefaulted_GetRef();
		Segment.VertexBuffer = PositionStream->Pooled->GetRHI();
		Segment.VertexBufferElementType = VET_Float3;
		Segment.VertexBufferStride = VertexStride;
		Segment.VertexBufferOffset = BaseVertex * VertexStride;
		// Conservative: the whole capacity past the base vertex. The RHI only needs it to cover
		// the largest index the segment can reference, and the capacity does by construction.
		Segment.MaxVertices = FMath::Max(VertexCapacity - FMath::Min(BaseVertex, VertexCapacity), 1u);
		Segment.FirstPrimitive = BatchArgs.FirstIndex / 3u;
		Segment.NumPrimitives = BatchArgs.IndexCount / 3u;
		// An empty section keeps its slot so segment i stays batch i (materials are matched by
		// index) but contributes nothing to the build.
		Segment.bEnabled = Segment.NumPrimitives > 0;
		TotalPrimitives += Segment.NumPrimitives;
	}
	Initializer.TotalPrimitiveCount = TotalPrimitives;

	if (TotalPrimitives == 0)
	{
		// A mesh that really emptied. Remember the publication so this does not rebuild "nothing"
		// every frame until the next edit.
		ReleaseRayTracingGeometry();
		RayTracingBuiltSerial = Serial;
		return;
	}

	// Same object across rebuilds, as the engine's procedural mesh does on a section update: the
	// RHI keeps the previous BLAS alive for any frame still in flight.
	if (RayTracingGeometry) RayTracingGeometry->ReleaseResource();
	else RayTracingGeometry = MakeUnique<FRayTracingGeometry>();
	RayTracingGeometry->SetInitializer(MoveTemp(Initializer));
	RayTracingGeometry->InitResource(RHICmdList);
	RayTracingBuiltArgs = Args;
	RayTracingBuiltSerial = Serial;
}

void FCSGpuMeshSceneProxy::GetDynamicRayTracingInstances(FRayTracingInstanceCollector& Collector)
{
	if (!RayTracingGeometry || !RayTracingGeometry->IsValid()) return;
	if (!DrawDesc.bValid || DrawDesc.IndexBuffer == nullptr || !VertexFactory) return;

	TArray<FMaterialRenderProxy*, TInlineAllocator<8>> Materials;
	GetBatchMaterials(Materials);
	// Segment i is matched to material i by index. A split that changed under a BLAS the pump has
	// not caught up with yet would pair them wrong; skipping a frame is the honest answer.
	if (Materials.Num() != RayTracingBuiltArgs.Num()) return;

	FRayTracingInstance Instance;
	Instance.Geometry = RayTracingGeometry.Get();
	Instance.InstanceTransforms.Add(GetLocalToWorld());

	// One primitive uniform buffer for the instance, shared by every segment's batch — the same
	// dynamic-primitive route the raster path takes, because this proxy has no GPU-Scene slot of
	// its own to point a cached uniform buffer at.
	FDynamicPrimitiveUniformBuffer& DynamicPrimitiveUniformBuffer = Collector.AllocateOneFrameResource<FDynamicPrimitiveUniformBuffer>();
	FPrimitiveUniformShaderParametersBuilder Builder;
	BuildUniformShaderParameters(Builder);
	DynamicPrimitiveUniformBuffer.Set(Collector.GetRHICommandList(), Builder);

	// One instance per visible view, as the engine's own proxies do; the geometry and batches are
	// shared, and the shadow flag comes from the first active view like the mesh-element path.
	TConstArrayView<const FSceneView*> Views = Collector.GetViews();
	const uint32 VisibilityMap = Collector.GetVisibilityMap();
	const int32 FirstActiveViewIndex = FMath::CountTrailingZeros(VisibilityMap);
	if (!Views.IsValidIndex(FirstActiveViewIndex)) return;
	const bool bCastRayTracedShadow = bBatchCastShadow && IsShadowCast(Views[FirstActiveViewIndex]);
	Instance.Materials.Reserve(Materials.Num());
	for (int32 SegmentIndex = 0; SegmentIndex < Materials.Num(); ++SegmentIndex)
	{
		const FCSGpuDrawArgs& BatchArgs = RayTracingBuiltArgs[SegmentIndex];
		FMeshBatch& Mesh = Instance.Materials.AddDefaulted_GetRef();
		Mesh.VertexFactory = VertexFactory.Get();
		Mesh.MaterialRenderProxy = Materials[SegmentIndex];
		Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
		Mesh.Type = PT_TriangleList;
		Mesh.DepthPriorityGroup = SDPG_World;
		Mesh.bCanApplyViewModeOverrides = false;
		Mesh.CastRayTracedShadow = bCastRayTracedShadow;
		Mesh.SegmentIndex = SegmentIndex;

		FMeshBatchElement& BatchElement = Mesh.Elements[0];
		BatchElement.IndexBuffer = DrawDesc.IndexBuffer;
		BatchElement.FirstIndex = BatchArgs.FirstIndex;
		BatchElement.NumPrimitives = BatchArgs.IndexCount / 3u;
		BatchElement.BaseVertexIndex = uint32(FMath::Max(BatchArgs.BaseVertexIndex, 0));
		BatchElement.MinVertexIndex = 0;
		BatchElement.MaxVertexIndex = DrawDesc.MaxVertexIndex;
		BatchElement.PrimitiveUniformBufferResource = &DynamicPrimitiveUniformBuffer.UniformBuffer;
	}

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		if ((VisibilityMap & (1u << ViewIndex)) == 0) continue;
		Collector.AddRayTracingInstance(ViewIndex, Instance);
	}
}

#else // RHI_RAYTRACING

void FCSGpuMeshSceneProxy::RegisterRayTracingPump() {}
void FCSGpuMeshSceneProxy::UnregisterRayTracingPump() {}

#endif // RHI_RAYTRACING

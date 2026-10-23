#include "GDFSampleService.h"

#include "ComputeShaderGeneral.h"
#include "ComputeShaderBasicFunction.h"

#include "EngineModule.h"
#include "Engine/Engine.h"
#include "RendererInterface.h"
#include "GlobalDistanceFieldParameters.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"
#include "SceneView.h"
#include "ShaderParameterStruct.h"
#include "Async/Async.h"

// 单组 block-sums 扫描的容量上限：SCAN_BLOCK_SIZE 个 block => SCAN_BLOCK_SIZE^2 列。
// 与 VoxelCavitySpan.usf 的 SCAN_BLOCK_SIZE 保持一致。

// ─────────────────────────────────────────────────────────────────────────────
// View extension：唯一能拿到真 FViewInfo 的地方。
// 选 PostRenderBasePassDeferred_RenderThread 是因为此刻 GDF 参数数据已被渲染器填充
// （PrepareDistanceFieldScene 在 base pass 之前完成），而 PreRenderView 阶段还是空的。
// ─────────────────────────────────────────────────────────────────────────────
class FGDFViewExtension : public FSceneViewExtensionBase
{
public:
	FGDFViewExtension(const FAutoRegister& AutoReg, FGDFSampleService& InOwner)
		: FSceneViewExtensionBase(AutoReg)
		, Owner(InOwner)
	{
	}

	virtual void PostRenderBasePassDeferred_RenderThread(
		FRDGBuilder& GraphBuilder,
		FSceneView& InView,
		const FRenderTargetBindingSlots& RenderTargets,
		TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTextures) override;

private:
	FGDFSampleService& Owner;

	void ProcessPointSample(FRDGBuilder& GraphBuilder, const FSceneView& View, FGDFPointSampleRequest&& Req);
	void ProcessCavitySpan(FRDGBuilder& GraphBuilder, const FSceneView& View, FGDFCavitySpanRequest&& Req);
};

// ─────────────────────────────────────────────────────────────────────────────
// 单例 + 生命周期
// ─────────────────────────────────────────────────────────────────────────────
FGDFSampleService& FGDFSampleService::Get()
{
	static FGDFSampleService Singleton;
	return Singleton;
}

void FGDFSampleService::Startup()
{
	if (!ViewExtension.IsValid() && GEngine)
	{
		ViewExtension = FSceneViewExtensions::NewExtension<FGDFViewExtension>(*this);
	}
}

void FGDFSampleService::Shutdown()
{
	ViewExtension.Reset();
	// 清空残留请求，避免回调悬空。
	FGDFPointSampleRequest P;
	while (PointQueue.Dequeue(P)) {}
	FGDFCavitySpanRequest C;
	while (CavityQueue.Dequeue(C)) {}
	FGDFJobRequest J;
	while (JobQueue.Dequeue(J)) {}
}

void FGDFSampleService::EnqueuePointSample(FGDFPointSampleRequest&& Request)
{
	if (Request.WorldPositions.IsEmpty()) return;
	Startup();
	PointQueue.Enqueue(MoveTemp(Request));
}

void FGDFSampleService::EnqueueCavitySpan(FGDFCavitySpanRequest&& Request)
{
	Startup();
	CavityQueue.Enqueue(MoveTemp(Request));
}

void FGDFSampleService::EnqueueGDFJob(FGDFJobRequest&& Request)
{
	if (!Request.Build) return;
	Startup();
	JobQueue.Enqueue(MoveTemp(Request));
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染线程 readback 轮询。引擎主图里不能 Flush，所以 readback 完成与否要逐帧轮询：
// 把"检查函数"挂到一个渲染线程列表，每次回调开头先跑一遍，ready 的就消费并移除。
// 检查函数 ready 后负责把结果 AsyncTask 回游戏线程触发用户回调。
// ─────────────────────────────────────────────────────────────────────────────
namespace
{
	// 返回 true 表示已完成（可移除）。仅渲染线程访问。
	TArray<TFunction<bool()>>& GetPendingReadbacks()
	{
		static TArray<TFunction<bool()>> Pending;
		return Pending;
	}

	void PumpPendingReadbacks()
	{
		TArray<TFunction<bool()>>& Pending = GetPendingReadbacks();
		for (int32 i = Pending.Num() - 1; i >= 0; --i)
		{
			if (Pending[i]())
			{
				Pending.RemoveAtSwap(i);
			}
		}
	}

	void AddPendingReadback(TFunction<bool()>&& Poll)
	{
		GetPendingReadbacks().Add(MoveTemp(Poll));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 渲染线程回调：先轮询在途 readback，再排空两个队列逐个处理。
// ─────────────────────────────────────────────────────────────────────────────
void FGDFViewExtension::PostRenderBasePassDeferred_RenderThread(
	FRDGBuilder& GraphBuilder,
	FSceneView& InView,
	const FRenderTargetBindingSlots& /*RenderTargets*/,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> /*SceneTextures*/)
{
	PumpPendingReadbacks();

	// 仅处理主视图，避免多视图重复消费同一请求。
	if (!InView.bIsViewInfo)
	{
		return;
	}

	FGDFPointSampleRequest PointReq;
	while (Owner.PointQueue.Dequeue(PointReq))
	{
		ProcessPointSample(GraphBuilder, InView, MoveTemp(PointReq));
	}

	FGDFCavitySpanRequest CavityReq;
	while (Owner.CavityQueue.Dequeue(CavityReq))
	{
		ProcessCavitySpan(GraphBuilder, InView, MoveTemp(CavityReq));
	}

	FGDFJobRequest JobReq;
	TArray<FGDFJobRequest> DeferredJobs;
	while (Owner.JobQueue.Dequeue(JobReq))
	{
		if (!JobReq.Build) continue;
		if (JobReq.IsCancelled && JobReq.IsCancelled()) continue;
		if (JobReq.Scene && (InView.bIsSceneCapture || !InView.Family || JobReq.Scene != InView.Family->Scene))
		{
			DeferredJobs.Add(MoveTemp(JobReq));
			continue;
		}
		const FGlobalDistanceFieldParameterData* GDFData =
			GetRendererModule().GetGlobalDistanceFieldParameterData(InView);
		if (!GDFData)
		{
			if (JobReq.Scene) DeferredJobs.Add(MoveTemp(JobReq));
			continue;
		}
		if (JobReq.Scene && (GDFData->NumGlobalSDFClipmaps <= 0 || GDFData->GlobalDFResolution <= 0.0f
			|| !GDFData->PageAtlasTexture || !GDFData->PageTableTexture))
		{
			DeferredJobs.Add(MoveTemp(JobReq));
			continue;
		}

		FGDFJobOutputs Outputs;
		JobReq.Build(GraphBuilder, InView, *GDFData, Outputs);

		// 没有完成回调：纯 fire-and-forget，Build 产出的常驻 buffer 由调用方自行在别处持有/读取。
		if (!JobReq.OnComplete)
		{
			continue;
		}

		// 挂一个 sentinel readback 作为 GPU 完成 fence：它排在 Build 的所有 pass 之后，
		// 一旦 readback ready，说明这张图的 GPU 工作已执行完，常驻 buffer 数据已就位。
		FRDGBufferRef Sentinel = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GDFJob.Sentinel"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Sentinel, PF_R32_UINT)), 0u);

		FRHIGPUBufferReadback* FenceRB = new FRHIGPUBufferReadback(TEXT("GDFJob.FenceRB"));
		AddEnqueueCopyPass(GraphBuilder, FenceRB, Sentinel, sizeof(uint32));

		TFunction<void(FGDFJobOutputs&)> OnComplete = MoveTemp(JobReq.OnComplete);
		AddPendingReadback([FenceRB, Outputs = MoveTemp(Outputs), OnComplete = MoveTemp(OnComplete)]() mutable -> bool
		{
			if (!FenceRB->IsReady())
			{
				return false;
			}
			delete FenceRB;

			AsyncTask(ENamedThreads::GameThread,
				[OnComplete = MoveTemp(OnComplete), Outputs = MoveTemp(Outputs)]() mutable
				{
					OnComplete(Outputs);
				});
			return true;
		});
	}
	for (FGDFJobRequest& Deferred : DeferredJobs) Owner.JobQueue.Enqueue(MoveTemp(Deferred));
}

void FGDFViewExtension::ProcessPointSample(FRDGBuilder& GraphBuilder, const FSceneView& View, FGDFPointSampleRequest&& Req)
{
	const int32 NumPositions = Req.WorldPositions.Num();
	if (NumPositions <= 0) return;

	const FGlobalDistanceFieldParameterData* GDFData =
		GetRendererModule().GetGlobalDistanceFieldParameterData(View);
	if (!GDFData) return;

	TArray<FVector4f> UploadData;
	UploadData.SetNum(NumPositions);
	for (int32 i = 0; i < NumPositions; ++i)
	{
		UploadData[i] = FVector4f((FVector3f)Req.WorldPositions[i], 0.0f);
	}

	const uint32 ReadbackBytes = sizeof(FVector4f) * NumPositions;
	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("GDF_PointSampleReadback"));

	FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), NumPositions);
	FRDGBufferRef PositionsBuffer = GraphBuilder.CreateBuffer(BufferDesc, TEXT("GDF_PositionsBuffer"));
	GraphBuilder.QueueBufferUpload(PositionsBuffer, UploadData.GetData(), ReadbackBytes, ERDGInitialDataFlags::None);

	FGlobalDistanceFieldForCS::FParameters* P = GraphBuilder.AllocParameters<FGlobalDistanceFieldForCS::FParameters>();
	P->RW_PointsToSampleBuffer0 = GraphBuilder.CreateUAV(PositionsBuffer);
	P->InputIntData0 = NumPositions;
	P->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	P->View = View.ViewUniformBuffer;
	P->GlobalDistanceFieldParameters = SetupGlobalDistanceFieldParameters_Minimal(*GDFData);
	P->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	P->GlobalDistanceFieldParameters.GlobalDistanceFieldPageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
	P->GlobalDistanceFieldParameters.GlobalDistanceFieldMipTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	TShaderMapRef<FGlobalDistanceFieldForCS> ComputeShader =
		FGlobalDistanceFieldForCS::CreateTempShaderPermutation(FGlobalDistanceFieldForCS::ESDFShader::GDF_SampleAtPositions);
	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumPositions, 1, 1), 32);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDF_SampleAtPositions"), ComputeShader, P, GroupCount);

	AddEnqueueCopyPass(GraphBuilder, Readback, PositionsBuffer, ReadbackBytes);

	// 轮询闭包：ready 后读回 -> 切游戏线程触发回调 -> 释放 Readback。
	TFunction<void(const TArray<float>&, const TArray<FVector>&)> OnComplete = MoveTemp(Req.OnComplete);
	AddPendingReadback([Readback, NumPositions, ReadbackBytes, OnComplete = MoveTemp(OnComplete)]() mutable -> bool
	{
		if (!Readback->IsReady())
		{
			return false;
		}

		TArray<float> Distances;
		TArray<FVector> Gradients;
		Distances.SetNumZeroed(NumPositions);
		Gradients.SetNumZeroed(NumPositions);

		if (const FVector4f* Src = static_cast<const FVector4f*>(Readback->Lock(ReadbackBytes)))
		{
			for (int32 i = 0; i < NumPositions; ++i)
			{
				Gradients[i] = FVector(Src[i].X, Src[i].Y, Src[i].Z);
				Distances[i] = Src[i].W;
			}
			Readback->Unlock();
		}
		delete Readback;

		if (OnComplete)
		{
			AsyncTask(ENamedThreads::GameThread,
				[OnComplete = MoveTemp(OnComplete), Distances = MoveTemp(Distances), Gradients = MoveTemp(Gradients)]()
				{
					OnComplete(Distances, Gradients);
				});
		}
		return true;
	});
}

void FGDFViewExtension::ProcessCavitySpan(FRDGBuilder& GraphBuilder, const FSceneView& View, FGDFCavitySpanRequest&& Req)
{
	const FIntVector GridSize(
		FMath::Clamp(Req.GridSize.X, 1, 65535),
		FMath::Clamp(Req.GridSize.Y, 1, 65535),
		FMath::Clamp(Req.GridSize.Z, 1, 65535));
	const uint32 ColumnCount = (uint32)GridSize.X * (uint32)GridSize.Y;
	const uint32 TotalVoxels = ColumnCount * (uint32)GridSize.Z;
	const uint32 OccupancyWords = FMath::DivideAndRoundUp(TotalVoxels, 32u);
	const uint32 NumBlocks = FMath::DivideAndRoundUp(ColumnCount, (uint32)VOXEL_CAVITY_SCAN_BLOCK_SIZE);

	// 单组 block-sums 扫描容量上限：SCAN_BLOCK_SIZE 个 block。
	if (NumBlocks > (uint32)VOXEL_CAVITY_SCAN_BLOCK_SIZE)
	{
		UE_LOG(LogTemp, Warning, TEXT("GDFCavitySpan: column count %u exceeds single-pass scan capacity (%d). Reduce GridSize."),
			ColumnCount, VOXEL_CAVITY_SCAN_BLOCK_SIZE * VOXEL_CAVITY_SCAN_BLOCK_SIZE);
		return;
	}

	const FGlobalDistanceFieldParameterData* GDFData =
		GetRendererModule().GetGlobalDistanceFieldParameterData(View);
	if (!GDFData) return;

	// 一列内 cavity span 的上界：相邻实体段之间的空隙数 <= GridSize.Z/2 + 1。
	// 主图里不能 Flush 回读 total 再分配，故直接按此上界分配紧凑前的容量。
	const uint32 SpanCapacityPerColumn = (uint32)(GridSize.Z / 2 + 1);
	const uint32 SpanCapacity = FMath::Max(ColumnCount * SpanCapacityPerColumn, 1u);

	const FVector BoxSize = Req.BoxSize;
	const FVector CellSizeLocal(BoxSize.X / GridSize.X, BoxSize.Y / GridSize.Y, BoxSize.Z / GridSize.Z);
	const FVector BoxMinLocal = BoxSize * -0.5;
	const FVector3f BoxLocalMinF = (FVector3f)BoxMinLocal;
	const FVector3f CellSizeLocalF = (FVector3f)CellSizeLocal;
	const FMatrix44f BoxToWorldF = (FMatrix44f)Req.BoxTransform.ToMatrixWithScale();

	// ── 6 passes on the engine graph ──
	FRDGBufferRef Occupancy = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(OccupancyWords, 1u)), TEXT("GDFCavity.Occupancy"));
	FRDGBufferUAVRef OccupancyUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Occupancy, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, OccupancyUAV, 0u);

	FRDGBufferRef SpanCount = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ColumnCount), TEXT("GDFCavity.SpanCount"));
	FRDGBufferUAVRef SpanCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpanCount, PF_R32_UINT));

	FRDGBufferRef SpanStart = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), ColumnCount), TEXT("GDFCavity.SpanStart"));
	FRDGBufferUAVRef SpanStartUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpanStart, PF_R32_UINT));

	FRDGBufferRef BlockSums = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), FMath::Max(NumBlocks, 1u)), TEXT("GDFCavity.BlockSums"));
	FRDGBufferUAVRef BlockSumsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(BlockSums, PF_R32_UINT));

	FRDGBufferRef TotalCount = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("GDFCavity.TotalCount"));
	FRDGBufferUAVRef TotalCountUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TotalCount, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, TotalCountUAV, 0u);

	FRDGBufferRef Spans = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), SpanCapacity), TEXT("GDFCavity.Spans"));
	FRDGBufferUAVRef SpansUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Spans, PF_R32_UINT));
	AddClearUAVPass(GraphBuilder, SpansUAV, 0u);

	// Pass A: GDF fill occupancy.
	{
		FVoxelCavityGDFFill::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityGDFFill::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->VoxelBoxLocalMin = BoxLocalMinF;
		P->VoxelCellSizeLocal = CellSizeLocalF;
		P->VoxelBoxToWorld = BoxToWorldF;
		P->OccupancyThreshold = Req.OccupancyThreshold;
		P->RW_Occupancy = OccupancyUAV;
		P->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
		P->View = View.ViewUniformBuffer;
		P->GlobalDistanceFieldParameters = SetupGlobalDistanceFieldParameters_Minimal(*GDFData);
		P->GlobalDistanceFieldParameters.GlobalDistanceFieldCoverageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
		P->GlobalDistanceFieldParameters.GlobalDistanceFieldPageAtlasTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Wrap, AM_Wrap, AM_Wrap>::GetRHI();
		P->GlobalDistanceFieldParameters.GlobalDistanceFieldMipTextureSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		TShaderMapRef<FVoxelCavityGDFFill> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector Groups = FComputeShaderUtils::GetGroupCount(GridSize, FIntVector(8, 8, 1));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.GDFFill"), Shader, P, Groups);
	}

	// Pass B: count cavity spans per column.
	{
		FVoxelCavityCount::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityCount::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->B_Occupancy = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Occupancy, PF_R32_UINT));
		P->RW_ColumnSpanCount = SpanCountUAV;
		TShaderMapRef<FVoxelCavityCount> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector Groups = FComputeShaderUtils::GetGroupCount(FIntVector(GridSize.X, GridSize.Y, 1), FIntVector(8, 8, 1));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.Count"), Shader, P, Groups);
	}

	// Pass C: per-block exclusive scan.
	{
		FVoxelCavityScanBlocks::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityScanBlocks::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->B_ColumnSpanCount = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SpanCount, PF_R32_UINT));
		P->RW_ColumnSpanStart = SpanStartUAV;
		P->RW_BlockSums = BlockSumsUAV;
		TShaderMapRef<FVoxelCavityScanBlocks> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.ScanBlocks"), Shader, P, FIntVector(NumBlocks, 1, 1));
	}

	// Pass D: scan block sums + grand total.
	{
		FVoxelCavityScanBlockSums::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityScanBlockSums::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->NumBlocks = NumBlocks;
		P->RW_BlockSums = BlockSumsUAV;
		P->RW_TotalCount = TotalCountUAV;
		TShaderMapRef<FVoxelCavityScanBlockSums> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.ScanBlockSums"), Shader, P, FIntVector(1, 1, 1));
	}

	// Pass E: add scanned block offsets back to per-column starts.
	{
		FVoxelCavityAddOffsets::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityAddOffsets::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->B_BlockSums = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(BlockSums, PF_R32_UINT));
		P->RW_ColumnSpanStart = SpanStartUAV;
		TShaderMapRef<FVoxelCavityAddOffsets> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.AddOffsets"), Shader, P, FIntVector(NumBlocks, 1, 1));
	}

	// Pass F: emit packed cavity spans into the CSR-laid-out span buffer.
	{
		FVoxelCavityEmit::FParameters* P = GraphBuilder.AllocParameters<FVoxelCavityEmit::FParameters>();
		P->VoxelGridSize = GridSize;
		P->ColumnCount = ColumnCount;
		P->TotalSpanCapacity = SpanCapacity;
		P->B_Occupancy = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Occupancy, PF_R32_UINT));
		P->B_ColumnSpanStart = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SpanStart, PF_R32_UINT));
		P->RW_Spans = SpansUAV;
		TShaderMapRef<FVoxelCavityEmit> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector Groups = FComputeShaderUtils::GetGroupCount(FIntVector(GridSize.X, GridSize.Y, 1), FIntVector(8, 8, 1));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("GDFCavity.Emit"), Shader, P, Groups);
	}

	FRHIGPUBufferReadback* StartRB = new FRHIGPUBufferReadback(TEXT("GDFCavity.StartRB"));
	FRHIGPUBufferReadback* CountRB = new FRHIGPUBufferReadback(TEXT("GDFCavity.CountRB"));
	FRHIGPUBufferReadback* SpanRB = new FRHIGPUBufferReadback(TEXT("GDFCavity.SpanRB"));
	AddEnqueueCopyPass(GraphBuilder, StartRB, SpanStart, ColumnCount * sizeof(uint32));
	AddEnqueueCopyPass(GraphBuilder, CountRB, SpanCount, ColumnCount * sizeof(uint32));
	AddEnqueueCopyPass(GraphBuilder, SpanRB, Spans, SpanCapacity * sizeof(uint32));

	// 重建几何所需的参数（值拷贝，回调时填进 Result）。
	FGDFCavitySpanRequest::FResult Meta;
	Meta.GridSize = GridSize;
	Meta.BoxTransform = Req.BoxTransform;
	Meta.CellSizeWorld = CellSizeLocal;
	Meta.BoxMinLocal = BoxMinLocal;
	Meta.ColumnCount = ColumnCount;

	TFunction<void(const FGDFCavitySpanRequest::FResult&)> OnComplete = MoveTemp(Req.OnComplete);
	AddPendingReadback(
		[StartRB, CountRB, SpanRB, ColumnCount, SpanCapacity, Meta = MoveTemp(Meta), OnComplete = MoveTemp(OnComplete)]() mutable -> bool
	{
		if (!StartRB->IsReady() || !CountRB->IsReady() || !SpanRB->IsReady())
		{
			return false;
		}

		FGDFCavitySpanRequest::FResult Result = MoveTemp(Meta);
		Result.ColumnSpanStart.SetNumZeroed(ColumnCount);
		Result.ColumnSpanCount.SetNumZeroed(ColumnCount);
		Result.Spans.SetNumZeroed(SpanCapacity);

		auto CopyBack = [](FRHIGPUBufferReadback* RB, TArray<uint32>& Dst, uint32 Count)
		{
			if (const uint32* Src = static_cast<const uint32*>(RB->Lock(Count * sizeof(uint32))))
			{
				FMemory::Memcpy(Dst.GetData(), Src, Count * sizeof(uint32));
				RB->Unlock();
			}
		};
		CopyBack(StartRB, Result.ColumnSpanStart, ColumnCount);
		CopyBack(CountRB, Result.ColumnSpanCount, ColumnCount);
		CopyBack(SpanRB, Result.Spans, SpanCapacity);
		delete StartRB; delete CountRB; delete SpanRB;

		uint32 Total = 0;
		for (uint32 c = 0; c < ColumnCount; ++c) { Total += Result.ColumnSpanCount[c]; }
		Result.TotalSpanCount = Total;

		if (OnComplete)
		{
			AsyncTask(ENamedThreads::GameThread,
				[OnComplete = MoveTemp(OnComplete), Result = MoveTemp(Result)]()
				{
					OnComplete(Result);
				});
		}
		return true;
	});
}

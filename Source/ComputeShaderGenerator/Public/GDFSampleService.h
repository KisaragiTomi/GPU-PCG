#pragma once

#include "CoreMinimal.h"
#include "SceneViewExtension.h"
#include "Containers/Queue.h"
#include "Containers/Map.h"
#include "Math/IntVector.h"
#include "Templates/Function.h"
#include "Templates/RefCounting.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class FSceneView;
class FGlobalDistanceFieldParameterData;

//// GSDF 异步采样服务。
////
//// 为什么需要它：全局距离场（GDF）的参数数据（FGlobalDistanceFieldParameterData）只存在于
//// 渲染器内部的真 FViewInfo 上，而且只有在渲染管线跑到 base pass 之后才被填充。任何手工构造的
//// FSceneView（bIsViewInfo=false）都拿不到，强取会触发 ensure(View.bIsViewInfo) 崩溃。
////
//// 因此唯一正确做法：注册一个 SceneViewExtension，在 PostRenderBasePassDeferred_RenderThread
//// 回调里拿到真 FViewInfo（此时 GDF 已就绪），当帧把待处理的采样请求挂到引擎的 GraphBuilder 上，
//// 发起 GPU readback，readback 完成后在游戏线程触发回调。全程异步、零阻塞、不取景、不 Flush。
//
// GDF 的纹理是裸 FRHITexture*，只在回调那一帧有效，所以请求必须在回调当帧消费，不跨帧缓存。

// 一次 GDF 点采样的输入。
struct FGDFPointSampleRequest
{
	TArray<FVector> WorldPositions;
	// 完成回调（游戏线程触发）。Distances/Gradients 与 WorldPositions 等长同序。
	TFunction<void(const TArray<float>& Distances, const TArray<FVector>& Gradients)> OnComplete;
};

// 一次 cavity span 构建的输入（旋转盒内体素化 GDF -> 每 XY 列的空腔区间）。
struct FGDFCavitySpanRequest
{
	FTransform BoxTransform = FTransform::Identity; // 盒本地坐标系（旋转+中心）-> 世界
	FVector    BoxSize = FVector::ZeroVector;       // 盒在本地各轴的全长
	FIntVector GridSize = FIntVector(64, 64, 64);   // 体素分辨率 (X,Y,Z)
	float      OccupancyThreshold = 0.0f;           // GDF 距离 <= 阈值视为被占据

	// 完成回调（游戏线程触发）。回传 CSR 三件套（已从 GPU 读回的 CPU 副本）以及重建几何所需的参数。
	struct FResult
	{
		TArray<uint32> ColumnSpanStart; // [ColumnCount] 每列在 Spans 里的起始下标
		TArray<uint32> ColumnSpanCount; // [ColumnCount] 每列的 span 数
		TArray<uint32> Spans;           // [TotalSpanCount] 打包 (low16=zStart, high16=zEnd)
		FIntVector GridSize = FIntVector::ZeroValue;
		FTransform BoxTransform = FTransform::Identity;
		FVector    CellSizeWorld = FVector::ZeroVector; // 单个体素的世界尺寸
		FVector    BoxMinLocal = FVector::ZeroVector;   // cell(0,0,0) 最小角的盒本地坐标
		uint32     ColumnCount = 0;
		uint32     TotalSpanCount = 0;
	};
	TFunction<void(const FResult& Result)> OnComplete;
};

// 通用 GDF 作业：在 GDF 就绪那一帧，直接拿到引擎主 GraphBuilder + 真 FViewInfo + GDF 参数数据，
// 由调用方在 Build lambda 里自行构图（绑 GDF、跑 compute 等），并把需要跨帧持有的常驻 buffer
// 通过 FGDFJobOutputs::Add 交给 service。service 负责统一挂 fence 探测 GPU 完成，待该帧 GPU 真正
// 写完后，在游戏线程触发 OnComplete，把同一批常驻 buffer 交还调用方。
// 这样调用方只需关心两件事：Build 里挂自己的 pass、OnComplete 里拿结果，不再手写
// ConvertToExternalBuffer 之外的 AsyncTask / fence / 轮询样板。

// Build 产出的 GPU 常驻 buffer 容器（按名取用）。值即 ConvertToExternalBuffer 的返回。
struct FGDFJobOutputs
{
	TMap<FName, TRefCountPtr<FRDGPooledBuffer>> Buffers;

	void Add(FName Key, TRefCountPtr<FRDGPooledBuffer> Buffer)
	{
		Buffers.Add(Key, MoveTemp(Buffer));
	}

	TRefCountPtr<FRDGPooledBuffer> Find(FName Key) const
	{
		const TRefCountPtr<FRDGPooledBuffer>* Found = Buffers.Find(Key);
		return Found ? *Found : TRefCountPtr<FRDGPooledBuffer>();
	}
};

struct FGDFJobRequest
{
	// Optional scene restriction/cancellation for view-dependent consumers. Legacy jobs leave these empty.
	const FSceneInterface* Scene = nullptr;
	TFunction<bool()> IsCancelled;

	// 渲染线程：GDF 就绪那一帧构图。把需要跨帧持有的常驻 buffer 填进 Outputs（用 ConvertToExternalBuffer）。
	TFunction<void(FRDGBuilder& GraphBuilder, const FSceneView& View, const FGlobalDistanceFieldParameterData& GDFData, FGDFJobOutputs& Outputs)> Build;

	// 游戏线程：该帧 GPU 真正写完后触发，Outputs 即 Build 填入的常驻 buffer。可空（fire-and-forget）。
	TFunction<void(FGDFJobOutputs& Outputs)> OnComplete;
};

// 进程级单例：持有 view extension 与请求队列。
class COMPUTESHADERGENERATOR_API FGDFSampleService
{
public:
	static FGDFSampleService& Get();

	// 模块启动/关闭时调用（注册/注销 view extension）。
	void Startup();
	void Shutdown();

	// 游戏线程入队，立即返回。回调在结果就绪后于游戏线程触发。
	void EnqueuePointSample(FGDFPointSampleRequest&& Request);
	void EnqueueCavitySpan(FGDFCavitySpanRequest&& Request);
	void EnqueueGDFJob(FGDFJobRequest&& Request);

private:
	friend class FGDFViewExtension;

	TSharedPtr<class FGDFViewExtension, ESPMode::ThreadSafe> ViewExtension;

	// 渲染线程从这里取走当帧要处理的请求。生产者=游戏线程，消费者=渲染线程。
	TQueue<FGDFPointSampleRequest, EQueueMode::Mpsc> PointQueue;
	TQueue<FGDFCavitySpanRequest, EQueueMode::Mpsc> CavityQueue;
	TQueue<FGDFJobRequest, EQueueMode::Mpsc> JobQueue;
};

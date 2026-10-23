#include "CSGroundShaperSteps.h"

#include "CSMesh.h"                    // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "RenderGraphBuilder.h"         // ZeroCounters 自建图（同上，unity 会掩盖）
#include "RenderGraphUtils.h"           // AllocatePooledBuffer / AddClearUAVPass（unity 构建会掩盖掉这一条，-SingleFile 才抓得到）
#include "GameFramework/Actor.h"     // EnsureInstancedComponent 要 GetRootComponent（unity 会掩盖）
#include "RenderingThread.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSShaperSteps, Log, All);

namespace CSShaperSteps
{
/** 在渲染线程把不够大的 palette 补齐到 Capacities 给的尺寸。阻塞：Work 在栈上按引用捕获。 */
static bool CSShaperSteps_GrowTo(TArray<FPaletteBuffers>& Palettes, const TArray<uint32>& Capacities, const TArray<int32>& Grow)
{
	if (Grow.IsEmpty()) return true;

	TArray<FPaletteBuffers> Work = Palettes;
	ENQUEUE_RENDER_COMMAND(CSGroundStepEnsureCapacity)(
		[&Work, Grow, Capacities](FRHICommandListImmediate& RHICmdList)
		{
			// 分配完立刻清零：池子把同尺寸、无人引用的 buffer 直接复用（RenderGraphResourcePool.cpp 的
			// TryFindPooledBuffer），新 buffer 里就是上一位租客 —— 常常正是本家上一代 —— 的字节。不清的话，
			// 扩容后交接出去的是一份带着陈旧 counter 与残行的实例源，剔除 pass 照着它画，而砖数 / 三角数 /
			// 零阻塞断言全绿（2026-09-07 审查 B1）。三条都清：counter 是画不画的开关，行与 custom data
			// 是画出来是什么。清零走 PF_R32_UINT 视图 —— typed 视图的步长取自格式，整块 buffer 都盖得到
			// （同 CSMesh.cpp 的 ResizeStreams）。
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSShaperSteps.GrowTo"));
			auto AllocateZeroed = [&GraphBuilder](uint32 BytesPerElement, uint32 NumElements, const TCHAR* Name)
			{
				TRefCountPtr<FRDGPooledBuffer> Pooled = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
				FRDGBufferRef Ref = GraphBuilder.RegisterExternalBuffer(Pooled, Name);
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Ref, PF_R32_UINT)), 0u);
				// 打包 pass 与剔除 pass 都按 SRVMask 读它们，这里就停在那个状态上（同 ZeroCounters）。
				GraphBuilder.SetBufferAccessFinal(Ref, ERHIAccess::SRVMask);
				return Pooled;
			};
			for (const int32 Index : Grow)
			{
				// typed buffer（不是 structured）：剔除 pass 用 Buffer<float4> / Buffer<uint> 视图。
				Work[Index].PackedInstances = AllocateZeroed(
					sizeof(FVector4f), Capacities[Index] * CS_GPU_INSTANCED_ROW_FLOAT4S, TEXT("CSShaperSteps.PackedInstances"));
				Work[Index].Counter = AllocateZeroed(sizeof(uint32), 1u, TEXT("CSShaperSteps.Counter"));
				Work[Index].CustomData = AllocateZeroed(
					sizeof(float), Capacities[Index] * uint32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS), TEXT("CSShaperSteps.CustomData"));
				Work[Index].Capacity = Capacities[Index];
			}
			GraphBuilder.Execute();
		});
	// 阻塞到 pooled 引用可用为止（同 CSPointBrushActor 的上传路径）。
	// **必须走计数通道**：这里以前是一个裸 FlushRenderingCommands，而计数器在 CSMesh.cpp 里，
	// 于是"交互期零阻塞"那几条断言对这条路径完全瞎 —— 拖尺寸每帧扩容也照样报 0 次刷新。
	// 断言看不见的阻塞等于不存在的纪律。
	UCSMesh::CountedBlockingFlush();

	Palettes = MoveTemp(Work);
	return true;
}

bool ReserveCapacity(TArray<FPaletteBuffers>& Palettes, uint32 MinCapacityPerPalette)
{
	if (Palettes.IsEmpty()) return false;
	const uint32 Want = FMath::Max(Align(MinCapacityPerPalette, 64u), 64u);

	TArray<uint32> Capacities;
	TArray<int32> Grow;
	Capacities.SetNumUninitialized(Palettes.Num());
	for (int32 Index = 0; Index < Palettes.Num(); ++Index)
	{
		Capacities[Index] = FMath::Max(Want, Palettes[Index].Capacity);
		if (!Palettes[Index].PackedInstances.IsValid() || !Palettes[Index].Counter.IsValid()
			|| Palettes[Index].Capacity < Capacities[Index])
		{
			Grow.Add(Index);
		}
	}
	return CSShaperSteps_GrowTo(Palettes, Capacities, Grow);
}

void ZeroCounters(TArray<FPaletteBuffers>& Palettes)
{
	bool bAnyValid = false;
	for (const FPaletteBuffers& Buffers : Palettes) bAnyValid |= Buffers.IsValid();
	if (!bAnyValid) return;

	// 按值捕获（`TRefCountPtr` 拷贝即加引用），录完直接 return —— 这条路不许阻塞：
	// 它会被"关掉藤/关掉摆件"这类属性改动踩到，而那也是拖动交互的一部分。
	ENQUEUE_RENDER_COMMAND(CSShaperStepsZeroCounters)(
		[Work = Palettes](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSShaperSteps.ZeroCounters"));
			for (const FPaletteBuffers& Buffers : Work)
			{
				if (!Buffers.IsValid()) continue;
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Buffers.Counter, TEXT("CSShaperSteps.Counter"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT)), 0u);
				// 剔除 pass 只读这个 buffer 且不负责恢复状态 —— producer 自己留在 SRVMask
				// （与 CSHouseFrame::Scatter / CSHouseVine::Pack 同一条约定）。
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			}
			GraphBuilder.Execute();
		});
}

void ReleaseOnRenderThread(TArray<FPaletteBuffers>& Palettes)
{
	bool bAnyValid = false;
	for (const FPaletteBuffers& Buffers : Palettes) bAnyValid |= Buffers.PackedInstances.IsValid() || Buffers.Counter.IsValid();
	if (!bAnyValid)
	{
		Palettes.Reset();
		return;
	}

	ENQUEUE_RENDER_COMMAND(CSGroundStepRelease)(
		[Released = MoveTemp(Palettes)](FRHICommandListImmediate&) mutable
		{
			Released.Reset();
		});
	Palettes.Reset();
}

// --- 实例组件的建/补 ---------------------------------------------------------------------

bool EnsureInstancedComponent(AActor* Owner, TObjectPtr<UCSGpuInstancedMeshComponent>& Component)
{
	if (!Owner || ::IsValid(Component)) return false;

	UCSGpuInstancedMeshComponent* Made = NewObject<UCSGpuInstancedMeshComponent>(Owner, NAME_None, RF_Transient);
	Made->SetupAttachment(Owner->GetRootComponent());
	Made->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格，必须先注册再喂
	Component = Made;
	return true;
}

bool EnsureInstancedComponents(
	AActor* Owner, TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, int32 Num)
{
	bool bChanged = false;
	while (Components.Num() > Num)
	{
		TObjectPtr<UCSGpuInstancedMeshComponent> Extra = Components.Pop();
		// 不必先撤实例源：组件在 OnComponentDestroyed 里当场放掉它那几份 pooled 引用与自己的显存。
		if (::IsValid(Extra)) Extra->DestroyComponent();
		bChanged = true;
	}
	while (Components.Num() < Num)
	{
		Components.Add(nullptr);
		bChanged = true;
	}
	for (TObjectPtr<UCSGpuInstancedMeshComponent>& One : Components) bChanged |= EnsureInstancedComponent(Owner, One);
	return bChanged;
}

// --- 实例源交接 ---------------------------------------------------------------------------

namespace
{
/**
 * 包围盒的等值阈值，cm。
 *
 * 它**不是**稳态判据本身：真正把"随尺寸连续变化的量"吸成阶梯的是 `QuantizeUp` 的 200 cm 台阶，
 * 这里只是最后一道等值判定。原来九处各自写死 `1.0`，改这个数得改九遍。
 */
constexpr double HandoverBoundsEpsilon = 1.0;

bool HandoverBoundsDiffer(const FBox& Cached, const FBox& Wanted)
{
	return !Cached.IsValid
		|| !Cached.Min.Equals(Wanted.Min, HandoverBoundsEpsilon)
		|| !Cached.Max.Equals(Wanted.Max, HandoverBoundsEpsilon);
}

void HandOverOne(const CSShaperSteps::FHandoverSource& Source, const FBox& LocalBounds, bool bNotifiesChanges)
{
	FCSGpuInstanceSourceGPU Out;
	Out.bNotifiesChanges = bNotifiesChanges;
	// 保留自己那份引用：这批 buffer 下一轮重散布 / 重打包还要写。
	Out.PackedInstances = Source.PackedInstances;
	Out.Counter = Source.Counter;
	Out.CustomData = Source.CustomData;
	Out.Capacity = Source.Capacity;
	Out.LocalBounds = LocalBounds;
	Source.Component->SetInstanceSourceGPU(Out);
}
}

FBox MergeHandoverBounds(FBox LocalBounds, const FHandoverCache& Cache, bool bFullRebuild)
{
	if (!bFullRebuild && Cache.LocalBounds.IsValid) LocalBounds += Cache.LocalBounds;
	return LocalBounds;
}

FHandoverSource MakeHandoverSource(
	UCSGpuInstancedMeshComponent* Component, const FPaletteBuffers& Buffers, bool bWithCustomData)
{
	FHandoverSource Source;
	Source.Component = Component;
	Source.PackedInstances = Buffers.PackedInstances;
	Source.Counter = Buffers.Counter;
	if (bWithCustomData) Source.CustomData = Buffers.CustomData;
	Source.Capacity = Buffers.Capacity;
	return Source;
}

EHandoverResult HandOverInstanceSources(
	TConstArrayView<FHandoverSource> Sources,
	const FBox& LocalBounds,
	FHandoverCache& Cache,
	const FHandoverOptions& Options,
	const UObject* LogOwner)
{
	if (Sources.IsEmpty()) return EHandoverResult::UpToDate;

	const bool bBoundsChanged = HandoverBoundsDiffer(Cache.LocalBounds, LocalBounds);
	// 数不一致 = 调色板重排过（换了资产 / 加减了物种），按下标对齐的前提没了，缓存整条作废。
	const bool bCountChanged = Cache.Capacities.Num() != Sources.Num();
	if (bCountChanged) Cache.Capacities.SetNumZeroed(Sources.Num());

	auto WantsHandover = [&](int32 Index)
	{
		const FHandoverSource& One = Sources[Index];
		if (!::IsValid(One.Component)) return true;   // 组件没了 => 缓存必然是陈的
		return bBoundsChanged
			|| Cache.Capacities[Index] != One.Capacity
			// 蓝图重跑构造脚本会销毁并重建实例组件：新组件身上没有实例源，只看缓存就会
			// 永远画不出东西 —— 拿组件自己的状态兜底。
			|| !One.Component->HasInstanceSourceGPU();
	};

	auto LogHandover = [&](int32 Index, uint32 Capacity)
	{
		if (!Options.LogLabel) return;
		UE_LOG(LogCSShaperSteps, Log, TEXT("[CSShaperSteps] %s %s[%d] 实例源交接（capacity=%u）"),
			LogOwner ? *LogOwner->GetName() : TEXT("?"), Options.LogLabel, Index, Capacity);
	};

	if (Options.Mode == EHandoverMode::PerPalette)
	{
		EHandoverResult Result = EHandoverResult::UpToDate;
		for (int32 Index = 0; Index < Sources.Num(); ++Index)
		{
			if (!WantsHandover(Index))
			{
				// 不用重新交接，但 buffer 刚被原地改过（调用方重散完才会来这里）：承诺了通知的那一批在这里兑现。
				if (Options.bNotifiesChanges) Sources[Index].Component->NotifyInstanceSourceChanged();
				continue;
			}
			if (!Sources[Index].IsReady())
			{
				Result = EHandoverResult::NotReady;
				continue;
			}
			HandOverOne(Sources[Index], LocalBounds, Options.bNotifiesChanges);
			Cache.Capacities[Index] = Sources[Index].Capacity;
			LogHandover(Index, Sources[Index].Capacity);
			if (Result == EHandoverResult::UpToDate) Result = EHandoverResult::HandedOver;
		}
		// 逐调色板模式下盒子是全体共用的，所以无条件回写：某一个没准备好不该让别的下一趟
		// 又被判成"盒子变了"而白交一次。它自己那格容量还留着旧值，轮到它就绪时照样会交。
		Cache.LocalBounds = LocalBounds;
		return Result;
	}

	bool bNeedHandover = bCountChanged;
	for (int32 Index = 0; !bNeedHandover && Index < Sources.Num(); ++Index) bNeedHandover = WantsHandover(Index);
	if (!bNeedHandover)
	{
		// 同上：整批都不用重新交接，但都刚被原地改过。WantsHandover 全为假 ⇒ 每个组件都有效。
		if (Options.bNotifiesChanges)
		{
			for (const FHandoverSource& One : Sources) One.Component->NotifyInstanceSourceChanged();
		}
		return EHandoverResult::UpToDate;
	}

	// 全或无：调色板与组件按下标对齐，交一半等于错位，所以先整批验一遍再动手。
	for (const FHandoverSource& One : Sources)
	{
		if (!One.IsReady()) return EHandoverResult::NotReady;
	}

	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		HandOverOne(Sources[Index], LocalBounds, Options.bNotifiesChanges);
		Cache.Capacities[Index] = Sources[Index].Capacity;
		LogHandover(Index, Sources[Index].Capacity);
	}
	Cache.LocalBounds = LocalBounds;
	return EHandoverResult::HandedOver;
}

EHandoverResult HandOverInstanceSource(
	const FHandoverSource& Source,
	const FBox& LocalBounds,
	FHandoverCache& Cache,
	const FHandoverOptions& Options,
	const UObject* LogOwner)
{
	return HandOverInstanceSources(MakeArrayView(&Source, 1), LocalBounds, Cache, Options, LogOwner);
}
}

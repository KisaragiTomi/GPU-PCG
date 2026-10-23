#include "CSGroundStairs.h"

#include "CSGpuMeshTypes.h"
#include "CSGpuSharedLayout.ush"       // CS_GPU_INSTANCED_ROW_FLOAT4S（与 .usf 同一份）
#include "CSGroundShaperField.h"
#include "CSMesh.h"                    // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGroundStairs, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSStairs_ 前缀
// （与 CSGroundShaperSteps.cpp 的 CSShaperSteps_、CSGroundActor.cpp 的 CSGround_ 必须都不同，
//  否则 unity blob 里同名符号打架，而报错位置会指向一个跟改动无关的文件）。

constexpr int32 CSStairs_GroupSizeX = 8;
constexpr int32 CSStairs_GroupSizeY = 8;

class FCSGroundStairsScanCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundStairsScanCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundStairsScanCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// 名字必须与 CSGroundShaperField.ush 里的声明逐字相同：这两个是与地面位移 pass 共享的
		// 那一份高度场的输入，改名等于把绑定悄悄拆掉（不报错，只是地面永远读成平的）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundShaperParams)
		SHADER_PARAMETER(uint32, GroundShaperCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, StairGroundColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWStairInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWStairCounter)

		SHADER_PARAMETER(FMatrix44f, StairWorldToComponent)
		SHADER_PARAMETER(FVector2f, StairGridOriginXY)
		SHADER_PARAMETER(float, StairCellSize)
		SHADER_PARAMETER(FUintVector2, StairGridDims)
		SHADER_PARAMETER(FVector2f, StairGroundOriginXY)
		SHADER_PARAMETER(float, StairGroundCellSize)
		SHADER_PARAMETER(FUintVector2, StairGroundVerts)
		SHADER_PARAMETER(float, StairGroundBaseZ)
		SHADER_PARAMETER(float, StairStepHeight)
		SHADER_PARAMETER(float, StairRoadThreshold)
		SHADER_PARAMETER(float, StairMinMoundHeight)
		SHADER_PARAMETER(uint32, StairDropTopStep)
		SHADER_PARAMETER(float, StairEmbed)
		SHADER_PARAMETER(float, StairRise)
		SHADER_PARAMETER(float, StairZOffset)
		SHADER_PARAMETER(uint32, StairMaxLayers)
		SHADER_PARAMETER(uint32, StairMaxInstances)
		SHADER_PARAMETER(FVector3f, StairBaseSphereCentre)
		SHADER_PARAMETER(float, StairBaseSphereRadius)
		SHADER_PARAMETER(FVector3f, StairBlockSize)
		SHADER_PARAMETER(float, StairBaseSizeY)
		SHADER_PARAMETER(float, StairLengthBloat)
		SHADER_PARAMETER(float, StairLengthJitter)
		SHADER_PARAMETER(float, StairSizeJitter)
		SHADER_PARAMETER(float, StairYawJitter)
		SHADER_PARAMETER(uint32, StairJitterSeed)

		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWStairPebbles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWStairPebbleCounter)
		SHADER_PARAMETER(float, StairPebbleChance)
		SHADER_PARAMETER(FVector2f, StairPebbleScale)
		SHADER_PARAMETER(FVector3f, StairPebbleSphereCentre)
		SHADER_PARAMETER(float, StairPebbleSphereRadius)
		SHADER_PARAMETER(uint32, StairMaxPebbles)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSStairs_GroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), CSStairs_GroupSizeY);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundStairsScanCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundStairs.usf", "ScanGroundStairsCS", SF_Compute);
}

namespace CSGroundStairs
{
bool EnsureBuffers(FStairBuffers& Buffers, uint32 Capacity, uint32 PebbleCapacity)
{
	const uint32 Want = FMath::Max(Align(Capacity, 64u), 64u);
	// 石子的 UAV 是无条件绑定的（RDG 不接受空参数），所以哪怕关掉石子也要有一块最小 buffer；
	// 真正的开关是 `FScanParams::PebbleChance = 0`，那条路径一个字节都不写。
	const uint32 WantPebbles = FMath::Max(Align(PebbleCapacity, 64u), 64u);
	// 两块一起判：只补了一半就返回，会让下一趟 Scan 拿着一个空的石子 UAV 去绑定。
	if (Buffers.IsValid() && Buffers.Capacity >= Want
		&& Buffers.HasPebbles() && Buffers.PebbleCapacity >= WantPebbles)
	{
		return true;   // 交互期走的就是这条：零 enqueue
	}

	// 走到这里一定阻塞一次（AllocatePooledBuffer 必须在渲染线程，而结果要回到游戏线程侧）。
	// 固定容量的意义就是让这一趟只发生在注册 / 加载 / 改配置时，摆位路径永远碰不到它。
	FStairBuffers Work = Buffers;
	ENQUEUE_RENDER_COMMAND(CSGroundStairsEnsureBuffers)(
		[&Work, Want, WantPebbles](FRHICommandListImmediate&)
		{
			// typed buffer（不是 structured）：实例组件的剔除 pass 用 Buffer<float4> / Buffer<uint> 视图。
			if (!Work.PackedInstances.IsValid() || Work.Capacity < Want)
			{
				Work.PackedInstances = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Want * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSGroundStairs.PackedInstances"));
				Work.Counter = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGroundStairs.Counter"));
				Work.Capacity = Want;
			}
			if (!Work.PebbleInstances.IsValid() || Work.PebbleCapacity < WantPebbles)
			{
				Work.PebbleInstances = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), WantPebbles * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSGroundStairs.PebbleInstances"));
				Work.PebbleCounter = AllocatePooledBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGroundStairs.PebbleCounter"));
				Work.PebbleCapacity = WantPebbles;
			}
		});
	// 阻塞到 pooled 引用可用为止（同 CSShaperSteps::CSShaperSteps_GrowTo）。
	// **必须走计数通道**：裸 FlushRenderingCommands 不进 CSMesh 的计数器，
	// “交互期零阻塞”的断言就看不见它 —— 而固定容量这个设计的全部意义
	// 恰恰就是“让交互期碰不到这一趟”，没人监视的话它会静默地退化回去。
	UCSMesh::CountedBlockingFlush();

	Buffers = MoveTemp(Work);
	return Buffers.IsValid() && Buffers.HasPebbles();
}

bool Scan(
	const FCSMeshResidentRef& GroundResident,
	const FStairBuffers& Buffers,
	const FScanParams& Params,
	const TArray<FVector4f>& ShaperParams)
{
	if (!GroundResident.IsValid() || !Buffers.IsValid() || !Buffers.HasPebbles()) return false;
	if (Params.GridDims.X <= 0 || Params.GridDims.Y <= 0 || Params.CellSize <= 0.0f) return false;

	// 空 palette 也要跑：塑形物被删光时正是"全 0 高度场"把石阶清空的那一趟
	// （counter 清零后没有一格跨层，实例数自然归 0，不需要额外的 clear 路径）。
	TArray<FVector4f> UploadParams = ShaperParams;
	const int32 ShaperCount = CSGroundShaperField::NumShapers(UploadParams);   // 前缀里读：池子跟在头后面，不能再按长度除
	if (UploadParams.IsEmpty()) UploadParams.Add(FVector4f::Zero());   // 结构化 buffer 不能是 0 长度

	ENQUEUE_RENDER_COMMAND(CSGroundStairsScan)(
		[Resident = GroundResident, Work = Buffers, Params, UploadParams, ShaperCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGroundStairs.Scan"));

			FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work.PackedInstances, TEXT("CSGroundStairs.PackedInstances"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work.Counter, TEXT("CSGroundStairs.Counter"));
			FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
			FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));
			FRDGBufferRef PebbleRef = GraphBuilder.RegisterExternalBuffer(Work.PebbleInstances, TEXT("CSGroundStairs.PebbleInstances"));
			FRDGBufferRef PebbleCounterRef = GraphBuilder.RegisterExternalBuffer(Work.PebbleCounter, TEXT("CSGroundStairs.PebbleCounter"));
			FRDGBufferUAVRef PebbleUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PebbleRef, PF_A32B32G32R32F));
			FRDGBufferUAVRef PebbleCounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PebbleCounterRef, PF_R32_UINT));
			// 组件永远不会替你清 counter：不先清零，每次重扫都在上一趟的值上继续叠，
			// 表现是石阶越画越少（槽位一路涨过容量后全被静默丢弃）。
			// 石子那份**同样必须清**，而且清零是它唯一的"关掉"路径：PebbleChance = 0 时
			// kernel 一个字节都不写，只有清零才让上一趟的石子真的消失。
			AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
			AddClearUAVPass(GraphBuilder, PebbleCounterUAV, 0u);

			CSHelper::FRDGStructuredBufferRefs ShaperRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, UploadParams, TEXT("CSGroundStairs.ShaperParams"), false, true);
			if (!ShaperRefs.SRV)
			{
				GraphBuilder.Execute();
				return;
			}

			{
				// 地面网格的常驻流：进出都走 mesh 层自己的入口，访问状态由 ~FCSMeshRenderThreadEdit
				// 恢复。手工写流再手工恢复是同一条规则的第二份拷贝，而漂掉的那份不报错 ——
				// 留在 RDG 默认 epilogue（SRVMask）的流对索引 / indirect 用途是非法的，只会安静地不画。
				FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);
				FRDGBufferRef Colors = Edit->Colors();
				if (Colors)
				{
					FCSGroundStairsScanCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSGroundStairsScanCS::FParameters>();
					PassParams->GroundShaperParams = ShaperRefs.SRV;
					PassParams->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
					PassParams->StairGroundColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Colors, PF_R32_UINT));
					PassParams->RWStairInstances = PackedUAV;
					PassParams->RWStairCounter = CounterUAV;
					PassParams->StairWorldToComponent = Params.WorldToComponent;
					PassParams->StairGridOriginXY = Params.GridOriginXY;
					PassParams->StairCellSize = Params.CellSize;
					PassParams->StairGridDims = FUintVector2(uint32(Params.GridDims.X), uint32(Params.GridDims.Y));
					PassParams->StairGroundOriginXY = Params.GroundOriginXY;
					PassParams->StairGroundCellSize = Params.GroundCellSize;
					PassParams->StairGroundVerts = FUintVector2(uint32(FMath::Max(Params.GroundVerts.X, 0)), uint32(FMath::Max(Params.GroundVerts.Y, 0)));
					PassParams->StairGroundBaseZ = Params.GroundBaseZ;
					PassParams->StairStepHeight = Params.StepHeight;
					PassParams->StairRoadThreshold = Params.RoadThreshold;
					PassParams->StairMinMoundHeight = FMath::Max(Params.MinMoundHeight, 0.0f);
					PassParams->StairDropTopStep = Params.bDropTopStep ? 1u : 0u;
					PassParams->StairEmbed = Params.Embed;
					PassParams->StairRise = Params.Rise;
					PassParams->StairZOffset = Params.ZOffset;
					PassParams->StairMaxLayers = FMath::Max(Params.MaxLayersPerCell, 1u);
					PassParams->StairMaxInstances = Work.Capacity;
					PassParams->StairBaseSphereCentre = Params.BaseSphereCentre;
					PassParams->StairBaseSphereRadius = Params.BaseSphereRadius;
					PassParams->StairBlockSize = Params.BlockSize;
					PassParams->StairBaseSizeY = Params.BaseSizeY;
					// 胀大系数夹在 [1, ...]：小于 1 就是**正缝**，正是这一步要消掉的缺陷
					// （同 ACSHouseActor::FrameBrickBloat 的 ClampMin，两处必须一致）。
					PassParams->StairLengthBloat = FMath::Max(Params.LengthBloat, 1.0f);
					PassParams->StairLengthJitter = FMath::Max(Params.LengthJitter, 0.0f);
					PassParams->StairSizeJitter = FMath::Clamp(Params.SizeJitter, 0.0f, 0.9f);
					PassParams->StairYawJitter = Params.YawJitterRad;
					PassParams->StairJitterSeed = Params.JitterSeed;

					PassParams->RWStairPebbles = PebbleUAV;
					PassParams->RWStairPebbleCounter = PebbleCounterUAV;
					// 概率钳在 [0, 1]：> 1 会让每一段都出石子（TG 是 0.15），< 0 与 0 同义。
					PassParams->StairPebbleChance = FMath::Clamp(Params.PebbleChance, 0.0f, 1.0f);
					// 下限不许超过上限：lerp 会照样算，但结果是"最大的反而最小"，
					// 一个断言都不会红，只在画面上表现为石子尺寸分布反着来。
					PassParams->StairPebbleScale = FVector2f(
						FMath::Max(Params.PebbleScaleMin, 0.0f),
						FMath::Max(Params.PebbleScaleMax, FMath::Max(Params.PebbleScaleMin, 0.0f)));
					PassParams->StairPebbleSphereCentre = Params.PebbleSphereCentre;
					PassParams->StairPebbleSphereRadius = Params.PebbleSphereRadius;
					PassParams->StairMaxPebbles = Work.PebbleCapacity;

					TShaderMapRef<FCSGroundStairsScanCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGroundStairs.ScanCells"), Shader, PassParams,
						FComputeShaderUtils::GetGroupCount(FIntPoint(Params.GridDims.X, Params.GridDims.Y),
							FIntPoint(CSStairs_GroupSizeX, CSStairs_GroupSizeY)));
				}
			}

			// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
			GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(PebbleRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(PebbleCounterRef, ERHIAccess::SRVMask);

			GraphBuilder.Execute();
		});

	return true;
}

void ReleaseOnRenderThread(FStairBuffers& Buffers)
{
	if (!Buffers.PackedInstances.IsValid() && !Buffers.Counter.IsValid()
		&& !Buffers.PebbleInstances.IsValid() && !Buffers.PebbleCounter.IsValid())
	{
		Buffers.Reset();
		return;
	}

	ENQUEUE_RENDER_COMMAND(CSGroundStairsRelease)(
		[Released = MoveTemp(Buffers)](FRHICommandListImmediate&) mutable
		{
			Released.Reset();
		});
	Buffers.Reset();
}

int32 DebugReadInstancesSync(const FStairBuffers& Buffers, TArray<FVector>* OutOrigins, TArray<FVector4f>* OutRows, bool bPebbles)
{
	if (OutOrigins) OutOrigins->Reset();
	if (OutRows) OutRows->Reset();
	if (bPebbles ? !Buffers.HasPebbles() : !Buffers.IsValid()) return 0;

	int32 Count = 0;
	TArray<FVector4f> Rows;
	ENQUEUE_RENDER_COMMAND(CSGroundStairsDebugReadback)(
		[&Count, &Rows, Work = Buffers, bPebbles, bWantRows = (OutOrigins != nullptr || OutRows != nullptr)](FRHICommandListImmediate& RHICmdList)
		{
			// 石阶与石子是同一份代码读两对 buffer —— 分两份实现的话，将来只有一份会跟着
			// packed 行布局改，而漂掉的那份不报错、只是安静地读出垃圾。
			const TRefCountPtr<FRDGPooledBuffer>& SrcRows = bPebbles ? Work.PebbleInstances : Work.PackedInstances;
			const TRefCountPtr<FRDGPooledBuffer>& SrcCounter = bPebbles ? Work.PebbleCounter : Work.Counter;
			const uint32 SrcCapacity = bPebbles ? Work.PebbleCapacity : Work.Capacity;

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGroundStairs.DebugReadback"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(SrcCounter, TEXT("CSGroundStairs.Counter"));

			FRHIGPUBufferReadback CounterReadback(TEXT("CSGroundStairs.CounterReadback"));
			FRHIGPUBufferReadback RowReadback(TEXT("CSGroundStairs.RowReadback"));
			AddEnqueueCopyPass(GraphBuilder, &CounterReadback, CounterRef, sizeof(uint32));

			// 这些 buffer 被 Scan 留在 SRVMask（给剔除 pass 用）；回读要 CopySrc，
			// 所以读完必须自己把它们放回去，否则下一帧的剔除 pass 会在错误的状态上撞见它们。
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			const uint32 RowBytes = SrcCapacity * CS_GPU_INSTANCED_ROW_FLOAT4S * sizeof(FVector4f);
			if (bWantRows)
			{
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(SrcRows, TEXT("CSGroundStairs.PackedInstances"));
				AddEnqueueCopyPass(GraphBuilder, &RowReadback, PackedRef, RowBytes);
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			}
			GraphBuilder.Execute();

			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (const uint32* Value = static_cast<const uint32*>(CounterReadback.Lock(sizeof(uint32))))
			{
				// GPU 的 counter 会数到越界丢弃的那些（InterlockedAdd 先加后判），按容量钳 —— 但超容量本身
				// 要出声：丢了哪些格由线程组完成顺序决定，"格身份决定一切"在超容量那一刻已经失效，而钳过的
				// 计数对所有断言都像"刚好装满"（2026-09-07 审查 B4b）。
				if (*Value > SrcCapacity)
				{
					UE_LOG(LogCSGroundStairs, Warning,
						TEXT("[CSGroundStairs] %s 散布超容量：counter=%u capacity=%u，超出的格被静默丢弃（丢哪些不确定）。"),
						bPebbles ? TEXT("石子") : TEXT("石阶"), *Value, SrcCapacity);
				}
				Count = int32(FMath::Min(*Value, SrcCapacity));
				CounterReadback.Unlock();
			}
			if (bWantRows && Count > 0)
			{
				if (const FVector4f* Data = static_cast<const FVector4f*>(RowReadback.Lock(RowBytes)))
				{
					Rows.Append(Data, int32(SrcCapacity) * CS_GPU_INSTANCED_ROW_FLOAT4S);
					RowReadback.Unlock();
				}
			}
		});
	// 诊断回读：按设计就是阻塞的，但也要计数 —— 计数器要么数到每一次阻塞，
	// 要么就不能拿它当证据。（测量窗口里不该出现 Debug*Sync，真出现了应该报红。）
	UCSMesh::CountedBlockingFlush();

	if (OutOrigins)
	{
		OutOrigins->Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Row = Index * CS_GPU_INSTANCED_ROW_FLOAT4S + 3;   // 第 4 行 = 原点 + 每实例随机数
			if (!Rows.IsValidIndex(Row)) break;
			OutOrigins->Add(FVector(Rows[Row].X, Rows[Row].Y, Rows[Row].Z));
		}
	}
	if (OutRows && Rows.Num() >= Count * CS_GPU_INSTANCED_ROW_FLOAT4S)
	{
		// 只带出活跃实例那一段：容量之外是上一趟的残值，谁读谁误判。
		OutRows->Append(Rows.GetData(), Count * CS_GPU_INSTANCED_ROW_FLOAT4S);
	}
	return Count;
}
}

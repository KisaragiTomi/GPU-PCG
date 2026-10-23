#include "CSNaniteCut.h"

#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSGpuSurfaceBudget.h"
#include "CSNaniteCutLayout.ush"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "Materials/MaterialInterface.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "Rendering/NaniteResources.h"
#include "Rendering/NaniteStreamingManager.h"
#include "RHICommandList.h"
#include "ShaderParameterStruct.h"
#include "ShaderCompilerCore.h"
#include "StaticMeshResources.h"
#if WITH_EDITOR
#include "StaticMeshCompiler.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogCSNaniteCut, Log, All);

// -----------------------------------------------------------------------------
// Shaders (Shaders/Private/CSNaniteCut.usf)
// -----------------------------------------------------------------------------

/**
 * Nanite 着色器的编译环境，照抄 FNaniteGlobalShader（NaniteShared.h:372）—— 它在 Renderer 私有目录里，插件拿不到。
 * NANITE_VOXEL_DATA / NANITE_ASSEMBLY_DATA 这类决定页数据布局的宏由着色器编译器对所有着色器统一设置
 * （ShaderCompiler.cpp:4159），不用、也不能在这里另设。
 */
class FCSNaniteCutShader : public FGlobalShader
{
public:
	FCSNaniteCutShader() = default;
	FCSNaniteCutShader(const ShaderMetaType::CompiledShaderInitializerType& Initializer) : FGlobalShader(Initializer) {}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		// Nanite 头文件用了模板函数（GetRawAttributeData<N>），要 HLSL 2021 + DXC。
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_HLSL2021);
	}
};

class FCSNaniteCutInitTraversalCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutInitTraversalCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutInitTraversalCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_Output)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, CSNC_NodeQueueOut)
		SHADER_PARAMETER(uint32, CSNC_NumRequests)
		SHADER_PARAMETER(uint32, CSNC_NodeCapacity)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutInitTraversalCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "InitTraversalCS", SF_Compute);

class FCSNaniteCutPrepareNodeArgsCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutPrepareNodeArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutPrepareNodeArgsCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_Output)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_DispatchArgs)
		SHADER_PARAMETER(uint32, CSNC_InSlot)
		SHADER_PARAMETER(uint32, CSNC_OutSlot)
		SHADER_PARAMETER(uint32, CSNC_NodeCapacity)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutPrepareNodeArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "PrepareNodeArgsCS", SF_Compute);

class FCSNaniteCutNodeCullCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutNodeCullCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutNodeCullCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, HierarchyBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, CSNC_Requests)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_Output)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, CSNC_NodeQueueIn)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, CSNC_NodeQueueOut)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, CSNC_CandidatesOut)
		SHADER_PARAMETER(uint32, CSNC_InSlot)
		SHADER_PARAMETER(uint32, CSNC_OutSlot)
		SHADER_PARAMETER(uint32, CSNC_NodeCapacity)
		SHADER_PARAMETER(uint32, CSNC_CandidateCapacity)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutNodeCullCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "NodeCullCS", SF_Compute);

class FCSNaniteCutPrepareClusterArgsCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutPrepareClusterArgsCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutPrepareClusterArgsCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_Output)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_DispatchArgs)
		SHADER_PARAMETER(uint32, CSNC_CandidateCapacity)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutPrepareClusterArgsCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "PrepareClusterArgsCS", SF_Compute);

class FCSNaniteCutClusterSelectCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutClusterSelectCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutClusterSelectCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, ClusterPageData)
		SHADER_PARAMETER(FIntVector4, PageConstants)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, CSNC_Requests)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, CSNC_Output)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, CSNC_Candidates)
		SHADER_PARAMETER(uint32, CSNC_NumRequests)
		SHADER_PARAMETER(uint32, CSNC_CandidateCapacity)
		SHADER_PARAMETER(uint32, CSNC_RecordCapacity)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutClusterSelectCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "ClusterSelectCS", SF_Compute);

class FCSNaniteCutWriteClustersCS : public FCSNaniteCutShader
{
	DECLARE_GLOBAL_SHADER(FCSNaniteCutWriteClustersCS);
	SHADER_USE_PARAMETER_STRUCT(FCSNaniteCutWriteClustersCS, FCSNaniteCutShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer, ClusterPageData)
		SHADER_PARAMETER(FIntVector4, PageConstants)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FCSNaniteCutPlanEntry>, CSNC_Plan)
		SHADER_PARAMETER(uint32, CSNC_NumPlanEntries)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CSNC_WriteMatrices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, CSNC_WriteInfo)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CSNC_MaterialRemap)
		SHADER_PARAMETER(uint32, CSNC_NumTexCoordSets)
		SHADER_PARAMETER(uint32, CSNC_VertexCapacity)
		SHADER_PARAMETER(uint32, CSNC_IndexCapacity)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Tangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_TexCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Colors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_Indices)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MaterialIds)
	END_SHADER_PARAMETER_STRUCT()
};
IMPLEMENT_GLOBAL_SHADER(FCSNaniteCutWriteClustersCS, "/Plugin/PCGPlugins/Shaders/Private/CSNaniteCut.usf", "WriteClustersCS", SF_Compute);

// -----------------------------------------------------------------------------
// Internals
// -----------------------------------------------------------------------------

namespace
{
// Unity 构建下整个模块可能共用一个 TU，文件内的名字都带 CSNaniteCut_ 前缀。

/**
 * 层级里的误差是 f16（NaniteDataDecode.ush:693-694），根组的父级误差 1e10 存成 f16 后是无穷大或 65504。
 * 阈值再大就可能连根都进不去，于是钳到 f16 最大有限值以下：这么大的阈值对正常网格本来就只剩根 cluster。
 */
constexpr float CSNaniteCut_MaxCutError = 65000.0f;

/** 一次计数趟最多容纳的 cluster 数（候选 / 记录 buffer 都按它定）。单个资源超过它时独占一趟。 */
constexpr uint32 CSNaniteCut_MaxClustersPerChunk = 1u << 21;

/** 与 CSNaniteCut.usf 的 FCSNaniteCutPlanEntry 逐字段对应。 */
struct FCSNaniteCutPlanEntry
{
	uint32 PageIndex = 0;
	uint32 ClusterIndex = 0;
	uint32 RequestIndex = 0;
	uint32 VertexBase = 0;
	uint32 IndexBase = 0;
	uint32 Pad0 = 0;
	uint32 Pad1 = 0;
	uint32 Pad2 = 0;
};
static_assert(sizeof(FCSNaniteCutPlanEntry) == 32, "FCSNaniteCutPlanEntry must match CSNaniteCut.usf");

/** 计数趟回读的一条记录（CSNC_RECORD_*）。 */
struct FCSNaniteCutRecord
{
	uint32 Page = 0;
	uint32 Cluster = 0;
	uint32 Request = 0;   // 全局请求号（已加上本趟的起点）
	uint32 Key = 0;
	uint32 Vertices = 0;
	uint32 Triangles = 0;
};

/** 游戏线程上校验过的一个源。 */
struct FCSNaniteCutPrepared
{
	int32 SourceIndex = INDEX_NONE;
	const Nanite::FResources* Resources = nullptr;
	float CutError = 0.0f;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	FMatrix44f NormalToWorld = FMatrix44f::Identity;
	uint32 Flags = 0;
	FBox WorldBounds = FBox(ForceInit);
};

/** 渲染线程上读出来的运行时状态（HierarchyOffset 等由流送管理器在渲染线程写）。 */
struct FCSNaniteCutRuntimeInfo
{
	bool bReady = false;
	uint32 HierarchyOffset = 0;
	uint32 NumHierarchyNodes = 0;
	uint32 NumClusters = 0;
};

uint32 CSNaniteCut_FloatBits(float Value)
{
	uint32 Bits = 0;
	FMemory::Memcpy(&Bits, &Value, sizeof(Bits));
	return Bits;
}

/** 层级与根页都已分配、数据也已上传（调用前已确认流送管理器没有待处理的新资源）。渲染线程。 */
bool CSNaniteCut_IsResourceReady(const Nanite::FResources& Resources)
{
	return Resources.RuntimeResourceID != MAX_uint32
		&& Resources.HierarchyOffset != MAX_uint32
		&& Resources.RootPageIndex != INDEX_NONE;
}

/**
 * 新登记的资源在 FStreamingManager::Add 里就分好了层级偏移与根页槽位，但数据要等 BeginAsyncUpdate 里的
 * ProcessNewResources 才上传（NaniteStreamingManager.cpp:2320）—— 平时那是下一次渲染的事。这里在两帧之间
 * 自己推一次：Begin 无条件处理新资源，页流送那部分按帧号门控（:2325），同一帧里再推不会重复读回请求。
 * 渲染线程；返回之后流送管理器是否已经没有待处理的新资源。
 */
bool CSNaniteCut_SettleNewResources(FRHICommandListImmediate& RHICmdList)
{
	if (Nanite::GStreamingManager.IsSafeForRendering()) return true;
	if (Nanite::GStreamingManager.IsAsyncUpdateInProgress()) return false;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSNaniteCut.SettleNewResources"));
	Nanite::GStreamingManager.BeginAsyncUpdate(GraphBuilder);
	Nanite::GStreamingManager.EndAsyncUpdate(GraphBuilder);
	GraphBuilder.Execute();
	return Nanite::GStreamingManager.IsSafeForRendering();
}

/**
 * 显式请求资源的全部流送页。格式见 FStreamingManager::AddPendingExplicitRequests（NaniteStreamingManager.cpp:2222）：
 * 先是 PersistentHash，随后每页一个 uint —— bit0 = "后面还有这个资源的页"，bit1.. = 页号，高位 = 优先级
 * （全 1 会被钳到 NANITE_MAX_PRIORITY_BEFORE_PARENTS）。渲染线程，且不能在流送更新进行中。
 */
bool CSNaniteCut_RequestAllPages(const Nanite::FResources& Resources)
{
	if (Resources.PersistentHash == NANITE_INVALID_PERSISTENT_HASH) return false;
	if (Nanite::GStreamingManager.IsAsyncUpdateInProgress()) return false;

	const uint32 NumPages = uint32(Resources.PageStreamingStates.Num());
	if (NumPages <= Resources.NumRootPages) return false;

	TArray<uint32> RequestData;
	RequestData.Reserve(1 + NumPages - Resources.NumRootPages);
	RequestData.Add(Resources.PersistentHash);
	const uint32 PriorityBits = ~0u << (NANITE_MAX_RESOURCE_PAGES_BITS + 1);
	for (uint32 PageIndex = Resources.NumRootPages; PageIndex < NumPages; ++PageIndex)
	{
		const uint32 bMoreFollow = (PageIndex + 1u < NumPages) ? 1u : 0u;
		RequestData.Add(PriorityBits | ((PageIndex & NANITE_MAX_RESOURCE_PAGES_MASK) << 1) | bMoreFollow);
	}
	Nanite::GStreamingManager.RequestNanitePages(RequestData);
	return true;
}

/** 游戏线程取得的网格 Nanite 资源；没有就返回空。编辑器下先等异步编译完。 */
const Nanite::FResources* CSNaniteCut_GetResources(UStaticMesh* Mesh)
{
	if (!Mesh) return nullptr;
#if WITH_EDITOR
	if (Mesh->IsCompiling()) FStaticMeshCompilingManager::Get().FinishCompilation({ Mesh });
#endif
	if (!Mesh->HasValidNaniteData()) return nullptr;
	const FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
	return RenderData ? RenderData->NaniteResourcesPtr.Get() : nullptr;
}

/**
 * 一趟计数：层级遍历 → 叶切片展开 → 挑选 → 记录，整个 Output buffer 抽成 pooled buffer 交回。
 * RuntimeInfos / CutErrors 已按本趟的请求顺序排好。渲染线程。
 */
TRefCountPtr<FRDGPooledBuffer> CSNaniteCut_AddCountPasses(
	FRHICommandListImmediate& RHICmdList,
	const TArray<FCSNaniteCutRuntimeInfo>& RuntimeInfos,
	const TArray<float>& CutErrors,
	uint32 NodeCapacity,
	uint32 CandidateCapacity,
	uint32 NumOutputUints)
{
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSNaniteCut.Count"));
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	const uint32 NumRequests = uint32(RuntimeInfos.Num());

	TArray<FUintVector4> Requests;
	Requests.Reserve(RuntimeInfos.Num());
	for (int32 Index = 0; Index < RuntimeInfos.Num(); ++Index)
		Requests.Add(FUintVector4(RuntimeInfos[Index].HierarchyOffset, CSNaniteCut_FloatBits(CutErrors[Index]), 0u, 0u));

	FRDGBufferRef RequestsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CSNaniteCut.Requests"), Requests);
	FRDGBufferRef Output = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumOutputUints), TEXT("CSNaniteCut.Output"));
	FRDGBufferUAVRef OutputUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output, PF_R32_UINT));
	FRDGBufferRef Queues[2] =
	{
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2u, NodeCapacity), TEXT("CSNaniteCut.NodeQueue0")),
		GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2u, NodeCapacity), TEXT("CSNaniteCut.NodeQueue1")),
	};
	FRDGBufferRef Candidates = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 4u, CandidateCapacity), TEXT("CSNaniteCut.Candidates"));
	FRDGBufferRef DispatchArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("CSNaniteCut.DispatchArgs"));
	FRDGBufferUAVRef DispatchArgsUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(DispatchArgs, PF_R32_UINT));

	FRDGBufferSRVRef HierarchySRV = Nanite::GStreamingManager.GetHierarchySRV(GraphBuilder);
	FRDGBufferSRVRef ClusterPageDataSRV = Nanite::GStreamingManager.GetClusterPageDataSRV(GraphBuilder);
	const FIntVector4 PageConstants(0, int32(Nanite::GStreamingManager.GetMaxStreamingPages()), 0, 0);

	AddClearUAVPass(GraphBuilder, OutputUAV, 0u);

	{
		FCSNaniteCutInitTraversalCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutInitTraversalCS::FParameters>();
		Params->CSNC_Output = OutputUAV;
		Params->CSNC_NodeQueueOut = GraphBuilder.CreateUAV(Queues[0]);
		Params->CSNC_NumRequests = NumRequests;
		Params->CSNC_NodeCapacity = NodeCapacity;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.InitTraversal"),
			TShaderMapRef<FCSNaniteCutInitTraversalCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCountWrapped(FMath::DivideAndRoundUp(NumRequests, uint32(CSNC_TRAVERSE_GROUP_SIZE))));
	}

	// 逐层：每层一个"算派发参数"的单线程 pass 加一个间接派发的遍历 pass。层级深度的上限就是
	// NANITE_MAX_CLUSTER_HIERARCHY_DEPTH；实际更浅的资源走到底之后，剩下的层派发 0 个组。
	for (uint32 Level = 0; Level < NANITE_MAX_CLUSTER_HIERARCHY_DEPTH; ++Level)
	{
		const uint32 InSlot = (Level & 1u) ? CSNC_SLOT_QUEUE1 : CSNC_SLOT_QUEUE0;
		const uint32 OutSlot = (Level & 1u) ? CSNC_SLOT_QUEUE0 : CSNC_SLOT_QUEUE1;
		FRDGBufferRef InQueue = Queues[Level & 1u];
		FRDGBufferRef OutQueue = Queues[(Level + 1u) & 1u];

		{
			FCSNaniteCutPrepareNodeArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutPrepareNodeArgsCS::FParameters>();
			Params->CSNC_Output = OutputUAV;
			Params->CSNC_DispatchArgs = DispatchArgsUAV;
			Params->CSNC_InSlot = InSlot;
			Params->CSNC_OutSlot = OutSlot;
			Params->CSNC_NodeCapacity = NodeCapacity;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.PrepareNodeArgs(%u)", Level),
				TShaderMapRef<FCSNaniteCutPrepareNodeArgsCS>(ShaderMap), Params, FIntVector(1, 1, 1));
		}
		{
			FCSNaniteCutNodeCullCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutNodeCullCS::FParameters>();
			Params->HierarchyBuffer = HierarchySRV;
			Params->CSNC_Requests = GraphBuilder.CreateSRV(RequestsBuffer);
			Params->CSNC_Output = OutputUAV;
			Params->CSNC_NodeQueueIn = GraphBuilder.CreateSRV(InQueue);
			Params->CSNC_NodeQueueOut = GraphBuilder.CreateUAV(OutQueue);
			Params->CSNC_CandidatesOut = GraphBuilder.CreateUAV(Candidates);
			Params->CSNC_InSlot = InSlot;
			Params->CSNC_OutSlot = OutSlot;
			Params->CSNC_NodeCapacity = NodeCapacity;
			Params->CSNC_CandidateCapacity = CandidateCapacity;
			Params->IndirectArgs = DispatchArgs;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.NodeCull(%u)", Level),
				TShaderMapRef<FCSNaniteCutNodeCullCS>(ShaderMap), Params, DispatchArgs, 0);
		}
	}

	{
		FCSNaniteCutPrepareClusterArgsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutPrepareClusterArgsCS::FParameters>();
		Params->CSNC_Output = OutputUAV;
		Params->CSNC_DispatchArgs = DispatchArgsUAV;
		Params->CSNC_CandidateCapacity = CandidateCapacity;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.PrepareClusterArgs"),
			TShaderMapRef<FCSNaniteCutPrepareClusterArgsCS>(ShaderMap), Params, FIntVector(1, 1, 1));
	}
	{
		FCSNaniteCutClusterSelectCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutClusterSelectCS::FParameters>();
		Params->ClusterPageData = ClusterPageDataSRV;
		Params->PageConstants = PageConstants;
		Params->CSNC_Requests = GraphBuilder.CreateSRV(RequestsBuffer);
		Params->CSNC_Output = OutputUAV;
		Params->CSNC_Candidates = GraphBuilder.CreateSRV(Candidates);
		Params->CSNC_NumRequests = NumRequests;
		Params->CSNC_CandidateCapacity = CandidateCapacity;
		Params->CSNC_RecordCapacity = CandidateCapacity;
		Params->IndirectArgs = DispatchArgs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.ClusterSelect"),
			TShaderMapRef<FCSNaniteCutClusterSelectCS>(ShaderMap), Params, DispatchArgs, 0);
	}

	TRefCountPtr<FRDGPooledBuffer> PooledOutput;
	GraphBuilder.QueueBufferExtraction(Output, &PooledOutput);
	GraphBuilder.Execute();
	return PooledOutput;
}

void CSNaniteCut_SetAllStatus(FCSNaniteCutResult& Result, const TArray<FCSNaniteCutPrepared>& Prepared, const TArray<int32>& Subset, ECSNaniteCutStatus Status)
{
	for (int32 PreparedIndex : Subset) Result.Sources[Prepared[PreparedIndex].SourceIndex].Status = Status;
}
}

// -----------------------------------------------------------------------------
// UCSNaniteCutOps
// -----------------------------------------------------------------------------

UCSMesh* UCSNaniteCutOps::AppendNaniteCuts(
	UCSMesh* Target, const TArray<FCSNaniteCutSource>& Sources, const FCSNaniteCutOptions& Options, FCSNaniteCutResult& OutResult)
{
	OutResult = FCSNaniteCutResult();
	OutResult.Sources.SetNum(Sources.Num());
	if (!Target) return Target;
	if (!IsInGameThread())
	{
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] AppendNaniteCuts must be called from the game thread."));
		return Target;
	}
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		for (FCSNaniteCutSourceResult& SourceResult : OutResult.Sources) SourceResult.Status = ECSNaniteCutStatus::NotNanite;
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] This shader platform does not support Nanite; nothing to extract."));
		return Target;
	}

	// ---- 1. 游戏线程：校验每个源 ------------------------------------------------------------
	TArray<FCSNaniteCutPrepared> Prepared;
	for (int32 SourceIndex = 0; SourceIndex < Sources.Num(); ++SourceIndex)
	{
		const FCSNaniteCutSource& Source = Sources[SourceIndex];
		FCSNaniteCutSourceResult& SourceResult = OutResult.Sources[SourceIndex];

		const Nanite::FResources* Resources = CSNaniteCut_GetResources(Source.Mesh);
		if (!Resources)
		{
			SourceResult.Status = ECSNaniteCutStatus::NotNanite;
			continue;
		}

		// Assembly 的部件变换要读 FResources::AssemblyTransformOffset，多根资源（几何集合那种）的根不在节点 0，
		// 两条路径都没验过，先不收。Voxelize 的粗层是体素 cluster，没有三角形可写。
		bool bUnsupported = Resources->AssemblyTransforms.Num() > 0 || Resources->HierarchyRootOffsets.Num() > 1;
#if WITH_EDITORONLY_DATA
		bUnsupported |= Source.Mesh->GetNaniteSettings().ShapePreservation == ENaniteShapePreservation::Voxelize;
#endif
		if (bUnsupported)
		{
			SourceResult.Status = ECSNaniteCutStatus::Unsupported;
			continue;
		}

		const FMatrix LocalToWorld = Source.Transform.ToMatrixWithScale();
		const bool bMirrored = LocalToWorld.Determinant() < 0.0;

		FCSNaniteCutPrepared& Entry = Prepared.AddDefaulted_GetRef();
		Entry.SourceIndex = SourceIndex;
		Entry.Resources = Resources;
		Entry.CutError = FMath::Clamp(Source.CutError, 0.0f, CSNaniteCut_MaxCutError);
		Entry.LocalToWorld = FMatrix44f(LocalToWorld);
		// 法线要逆转置，否则非均匀缩放会把它们拧离表面（同 CopyFromStaticMesh）。
		Entry.NormalToWorld = FMatrix44f(LocalToWorld.Inverse().GetTransposed());
		if (bMirrored != Source.bFlipWinding) Entry.Flags |= CSNC_WRITE_FLAG_FLIP_WINDING;
		if (bMirrored) Entry.Flags |= CSNC_WRITE_FLAG_MIRRORED;
		Entry.WorldBounds = Source.Mesh->GetBoundingBox().TransformBy(Source.Transform);
	}

	auto Finish = [&OutResult]()
	{
		OutResult.bAllComplete = OutResult.Sources.Num() > 0;
		for (const FCSNaniteCutSourceResult& SourceResult : OutResult.Sources)
			if (SourceResult.Status != ECSNaniteCutStatus::Complete) OutResult.bAllComplete = false;
	};

	if (Prepared.Num() == 0)
	{
		if (!Options.bAppend) Target->Reset();
		Finish();
		return Target;
	}

	// ---- 2. 渲染线程：让新登记的资源落地，读出运行时状态 -------------------------------------
	TArray<FCSNaniteCutRuntimeInfo> RuntimeInfos;
	RuntimeInfos.SetNum(Prepared.Num());
	ENQUEUE_RENDER_COMMAND(CSNaniteCutGatherRuntimeInfo)([&Prepared, &RuntimeInfos](FRHICommandListImmediate& RHICmdList)
	{
		// 还有待上传的新资源时分不清是谁，一律当作没就绪 —— 字段早在 Add 里就填好了，数据却还没上去。
		if (!CSNaniteCut_SettleNewResources(RHICmdList)) return;
		for (int32 Index = 0; Index < Prepared.Num(); ++Index)
		{
			const Nanite::FResources& Resources = *Prepared[Index].Resources;
			if (!CSNaniteCut_IsResourceReady(Resources)) continue;
			FCSNaniteCutRuntimeInfo& Info = RuntimeInfos[Index];
			Info.bReady = true;
			Info.HierarchyOffset = Resources.HierarchyOffset;
			Info.NumHierarchyNodes = FMath::Max(Resources.NumHierarchyNodes, 1u);
			Info.NumClusters = FMath::Max(Resources.NumClusters, 1u);
		}
	});
	UCSMesh::CountedBlockingFlush();

	TArray<int32> Ready;   // 全局请求号 → Prepared 下标
	for (int32 Index = 0; Index < Prepared.Num(); ++Index)
	{
		if (RuntimeInfos[Index].bReady) Ready.Add(Index);
		else OutResult.Sources[Prepared[Index].SourceIndex].Status = ECSNaniteCutStatus::NotReady;
	}
	if (Ready.Num() == 0)
	{
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] No Nanite resource is registered with the streaming manager yet; nothing was extracted."));
		if (!Options.bAppend) Target->Reset();
		Finish();
		return Target;
	}

	// ---- 3. 计数趟（按 cluster 预算分趟），回读每趟用到的前缀 -------------------------------
	uint64 ExistingBufferBytes = 0;
	if (const FCSMeshResident* Resident = Target->GetResidentPtr())
	{
		for (const auto& Stream : Resident->Streams)
		{
			if (Stream.Pooled) ExistingBufferBytes += CSGpuSurfaceBudget::BufferBytes(Stream.Pooled->GetSize());
		}
	}
	const auto CountBufferBytes = [ExistingBufferBytes](uint64 Requests, uint64 Nodes, uint64 Clusters)
	{
		using CSGpuSurfaceBudget::BufferBytes;
		return ExistingBufferBytes + BufferBytes(16 * Requests)
			+ BufferBytes(4 * (CSNC_HEADER_UINTS + Requests * CSNC_STATS_UINTS + Clusters * CSNC_RECORD_UINTS))
			+ 2 * BufferBytes(8 * Nodes) + BufferBytes(16 * Clusters) + BufferBytes(12);
	};
	TArray<FCSNaniteCutRecord> Records;
	TArray<uint32> Stats;   // 按全局请求号，CSNC_STATS_UINTS 个一组
	Stats.SetNumZeroed(Ready.Num() * CSNC_STATS_UINTS);

	for (int32 ChunkStart = 0; ChunkStart < Ready.Num();)
	{
		int32 ChunkEnd = ChunkStart;
		uint64 ChunkClusters = 0;
		uint64 ChunkNodes = 0;
		while (ChunkEnd < Ready.Num())
		{
			const FCSNaniteCutRuntimeInfo& Info = RuntimeInfos[Ready[ChunkEnd]];
			if (ChunkEnd > ChunkStart && ChunkClusters + Info.NumClusters > CSNaniteCut_MaxClustersPerChunk) break;
			const uint64 NextBytes = CountBufferBytes(uint64(ChunkEnd - ChunkStart + 1),
				ChunkNodes + Info.NumHierarchyNodes, ChunkClusters + Info.NumClusters);
			if (Options.MaxWorkingMemoryBytes > 0 && NextBytes > Options.MaxWorkingMemoryBytes)
			{
				if (ChunkEnd > ChunkStart) break;
				UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] One source's traversal exceeds the working buffer budget (%llu > %llu bytes); no extraction allocated."), NextBytes, Options.MaxWorkingMemoryBytes);
				CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
				Finish();
				return Target;
			}
			ChunkClusters += Info.NumClusters;
			ChunkNodes += Info.NumHierarchyNodes;
			++ChunkEnd;
		}

		TArray<FCSNaniteCutRuntimeInfo> ChunkInfos;
		TArray<float> ChunkCutErrors;
		for (int32 Global = ChunkStart; Global < ChunkEnd; ++Global)
		{
			ChunkInfos.Add(RuntimeInfos[Ready[Global]]);
			ChunkCutErrors.Add(Prepared[Ready[Global]].CutError);
		}

		const uint32 NumChunkRequests = uint32(ChunkEnd - ChunkStart);
		const uint32 NodeCapacity = uint32(FMath::Min<uint64>(ChunkNodes, MAX_uint32));
		const uint32 CandidateCapacity = uint32(ChunkClusters);
		const uint32 HeaderAndStatsUints = CSNC_HEADER_UINTS + NumChunkRequests * CSNC_STATS_UINTS;
		const uint32 NumOutputUints = HeaderAndStatsUints + CandidateCapacity * CSNC_RECORD_UINTS;

		TRefCountPtr<FRDGPooledBuffer> PooledOutput;
		ENQUEUE_RENDER_COMMAND(CSNaniteCutCount)([&PooledOutput, &ChunkInfos, &ChunkCutErrors, NodeCapacity, CandidateCapacity, NumOutputUints](FRHICommandListImmediate& RHICmdList)
		{
			PooledOutput = CSNaniteCut_AddCountPasses(RHICmdList, ChunkInfos, ChunkCutErrors, NodeCapacity, CandidateCapacity, NumOutputUints);
		});
		UCSMesh::CountedBlockingFlush();

		// 先读头部与统计（几十个 uint），知道选中了多少再只读用到的那段记录 —— 容量是按全部 cluster 估的，
		// 真正选中的通常只占一小部分。
		TArray<uint32> Values;
		if (!PooledOutput.IsValid() || !CSMeshReadback::ReadUintBufferSync(PooledOutput, HeaderAndStatsUints, ECSGpuStreamRole::AuxVertex, Values))
		{
			UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Count pass readback failed."));
			CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
			Finish();
			return Target;
		}
		if (Values[CSNC_SLOT_OVERFLOW] != 0u)
		{
			// 容量都是按资源自身的节点 / cluster 总数给的上限，溢出说明层级不是预想的那棵树 —— 宁可整批不写。
			UE_LOG(LogCSNaniteCut, Error, TEXT("[CSNaniteCut] Count pass overflowed (flags 0x%x, nodes %u, candidates %u)."),
				Values[CSNC_SLOT_OVERFLOW], NodeCapacity, CandidateCapacity);
			CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
			Finish();
			return Target;
		}

		const uint32 NumRecords = FMath::Min(Values[CSNC_SLOT_EMITS], CandidateCapacity);
		for (uint32 Local = 0; Local < NumChunkRequests; ++Local)
			for (uint32 Field = 0; Field < CSNC_STATS_UINTS; ++Field)
				Stats[(ChunkStart + Local) * CSNC_STATS_UINTS + Field] = Values[CSNC_HEADER_UINTS + Local * CSNC_STATS_UINTS + Field];

		if (NumRecords > 0)
		{
			if (!CSMeshReadback::ReadUintBufferSync(PooledOutput, HeaderAndStatsUints + NumRecords * CSNC_RECORD_UINTS, ECSGpuStreamRole::AuxVertex, Values))
			{
				UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Count pass record readback failed."));
				CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
				Finish();
				return Target;
			}
			Records.Reserve(Records.Num() + int32(NumRecords));
			for (uint32 RecordIndex = 0; RecordIndex < NumRecords; ++RecordIndex)
			{
				const uint32* Raw = &Values[HeaderAndStatsUints + RecordIndex * CSNC_RECORD_UINTS];
				if (Raw[CSNC_RECORD_REQUEST] >= NumChunkRequests) continue;
				FCSNaniteCutRecord& Record = Records.AddDefaulted_GetRef();
				Record.Page = Raw[CSNC_RECORD_PAGE];
				Record.Cluster = Raw[CSNC_RECORD_CLUSTER];
				Record.Request = uint32(ChunkStart) + Raw[CSNC_RECORD_REQUEST];
				Record.Key = Raw[CSNC_RECORD_KEY];
				Record.Vertices = Raw[CSNC_RECORD_VERTICES];
				Record.Triangles = Raw[CSNC_RECORD_TRIANGLES];
			}
		}

		ChunkStart = ChunkEnd;
	}

	// ---- 4. CPU：定状态、排序、算写出偏移、并材质 ---------------------------------------------
	TArray<bool> WriteRequest;
	WriteRequest.Init(false, Ready.Num());
	TArray<const Nanite::FResources*> IncompleteResources;
	for (int32 Global = 0; Global < Ready.Num(); ++Global)
	{
		const uint32* RequestStats = &Stats[Global * CSNC_STATS_UINTS];
		FCSNaniteCutSourceResult& SourceResult = OutResult.Sources[Prepared[Ready[Global]].SourceIndex];
		SourceResult.Clusters = int32(RequestStats[CSNC_STAT_CLUSTERS]);
		SourceResult.Vertices = int32(RequestStats[CSNC_STAT_VERTICES]);
		SourceResult.Triangles = int32(RequestStats[CSNC_STAT_TRIANGLES]);
		SourceResult.ForcedClusters = int32(RequestStats[CSNC_STAT_FORCED]);
		SourceResult.VoxelClusters = int32(RequestStats[CSNC_STAT_VOXELS]);
		SourceResult.Status = SourceResult.ForcedClusters == 0 ? ECSNaniteCutStatus::Complete : ECSNaniteCutStatus::Incomplete;
		if (SourceResult.Status == ECSNaniteCutStatus::Incomplete) IncompleteResources.Add(Prepared[Ready[Global]].Resources);

		WriteRequest[Global] = SourceResult.Triangles > 0;
		if (SourceResult.Status == ECSNaniteCutStatus::Incomplete && Options.IncompletePolicy == ECSNaniteCutIncompletePolicy::SkipSource)
		{
			SourceResult.Status = ECSNaniteCutStatus::Skipped;
			WriteRequest[Global] = false;
		}
	}

	if (Options.bRequestMissingPages && IncompleteResources.Num() > 0)
	{
		ENQUEUE_RENDER_COMMAND(CSNaniteCutRequestPages)([IncompleteResources](FRHICommandListImmediate&)
		{
			for (const Nanite::FResources* Resources : IncompleteResources) CSNaniteCut_RequestAllPages(*Resources);
		});
	}

	// 写出顺序由原子分配决定、每次都不一样；按 (请求号, 稳定键) 排序后，同样的输入永远写出同样的网格。
	Records.RemoveAll([&WriteRequest](const FCSNaniteCutRecord& Record) { return !WriteRequest[int32(Record.Request)]; });
	Records.Sort([](const FCSNaniteCutRecord& A, const FCSNaniteCutRecord& B)
	{
		return A.Request != B.Request ? A.Request < B.Request : A.Key < B.Key;
	});

	uint32 BaseVertices = 0;
	uint32 BaseIndices = 0;
	if (Options.bAppend) Target->GetCountsSync(BaseVertices, BaseIndices);   // 没分配时返回 false 并给出 0

	TArray<FCSNaniteCutPlanEntry> Plan;
	Plan.Reserve(Records.Num());
	uint64 NextVertex = BaseVertices;
	uint64 NextIndex = BaseIndices;
	for (const FCSNaniteCutRecord& Record : Records)
	{
		FCSNaniteCutPlanEntry& Entry = Plan.AddDefaulted_GetRef();
		Entry.PageIndex = Record.Page;
		Entry.ClusterIndex = Record.Cluster;
		Entry.RequestIndex = Record.Request;
		Entry.VertexBase = uint32(NextVertex);
		Entry.IndexBase = uint32(NextIndex);
		NextVertex += Record.Vertices;
		NextIndex += uint64(Record.Triangles) * 3u;
	}
	if (Options.MaxTriangles > 0 && (NextIndex - BaseIndices) / 3u > uint64(Options.MaxTriangles))
	{
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Triangle budget exceeded (%llu > %d); increase CutError or reduce the query region."),
			(NextIndex - BaseIndices) / 3u, Options.MaxTriangles);
		CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
		Finish();
		return Target;
	}
	if (NextVertex > uint64(MAX_int32) || NextIndex > uint64(MAX_int32))
	{
		UE_LOG(LogCSNaniteCut, Error, TEXT("[CSNaniteCut] Result too large for one mesh (%llu vertices, %llu indices)."), NextVertex, NextIndex);
		CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
		Finish();
		return Target;
	}
	const uint32 NewVertexCount = uint32(NextVertex);
	const uint32 NewIndexCount = uint32(NextIndex);

	if (Plan.Num() == 0)
	{
		if (!Options.bAppend) Target->Reset();
		Finish();
		return Target;
	}

	// 材质按指针去重并进目标表；逐请求一张"源槽 → 目标下标"的表。追加时保留目标原有的表。
	TArray<TObjectPtr<UMaterialInterface>> NewMaterials;
	if (Options.bAppend) NewMaterials = Target->Materials;
	OutResult.MaterialSources.Init(INDEX_NONE, NewMaterials.Num());
	OutResult.MaterialSourceSlots.Init(INDEX_NONE, NewMaterials.Num());
	TArray<FVector4f> WriteMatrices;
	WriteMatrices.SetNumZeroed(Ready.Num() * CSNC_WRITE_MATRIX_ROWS);
	TArray<FUintVector4> WriteInfo;
	WriteInfo.SetNumZeroed(Ready.Num());
	TArray<uint32> MaterialRemap;
	FBox WrittenBounds(ForceInit);
	for (int32 Global = 0; Global < Ready.Num(); ++Global)
	{
		const FCSNaniteCutPrepared& Entry = Prepared[Ready[Global]];
		for (int32 Row = 0; Row < 4; ++Row)
		{
			const float* L = Entry.LocalToWorld.M[Row];
			const float* N = Entry.NormalToWorld.M[Row];
			WriteMatrices[Global * CSNC_WRITE_MATRIX_ROWS + Row] = FVector4f(L[0], L[1], L[2], L[3]);
			WriteMatrices[Global * CSNC_WRITE_MATRIX_ROWS + 4 + Row] = FVector4f(N[0], N[1], N[2], N[3]);
		}
		if (!WriteRequest[Global]) continue;

		const FCSNaniteCutSource& Source = Sources[Entry.SourceIndex];
		const TArray<FStaticMaterial>& StaticMaterials = Source.Mesh->GetStaticMaterials();
		const int32 NumSlots = FMath::Max(StaticMaterials.Num(), 1);
		const uint32 RemapOffset = uint32(MaterialRemap.Num());
		for (int32 Slot = 0; Slot < NumSlots; ++Slot)
		{
			UMaterialInterface* Material = Source.OverrideMaterials.IsValidIndex(Slot) ? Source.OverrideMaterials[Slot].Get() : nullptr;
			if (!Material && StaticMaterials.IsValidIndex(Slot)) Material = StaticMaterials[Slot].MaterialInterface;
			// 按源不去重时每个源的每个槽各占一项：逐三角材质号就能反查出三角来自哪个源。
			int32 TargetIndex = Options.bUniqueMaterialsPerSource ? INDEX_NONE : NewMaterials.IndexOfByKey(Material);
			if (TargetIndex == INDEX_NONE)
			{
				TargetIndex = NewMaterials.Add(Material);
				OutResult.MaterialSources.Add(Options.bUniqueMaterialsPerSource ? Entry.SourceIndex : INDEX_NONE);
				OutResult.MaterialSourceSlots.Add(Options.bUniqueMaterialsPerSource ? Slot : INDEX_NONE);
			}
			MaterialRemap.Add(uint32(TargetIndex));
		}
		WriteInfo[Global] = FUintVector4(RemapOffset, uint32(NumSlots), Entry.Flags, 0u);
		WrittenBounds += Entry.WorldBounds;
	}
	if (MaterialRemap.Num() == 0) MaterialRemap.Add(0u);
	if (Options.MaxWorkingMemoryBytes > 0)
	{
		using CSGpuSurfaceBudget::BufferBytes;
		FCSMeshResident DefaultResident;
		DefaultResident.AddStandardTriangleStreams();
		const FCSMeshResident* Resident = Target->GetResidentPtr();
		if (!Resident || Resident->Streams.IsEmpty()) Resident = &DefaultResident;
		const uint32 VertexCapacity = FMath::Max3(NewVertexCount, Resident->VertexCapacity, 3u);
		const uint32 IndexCapacity = FMath::Max3(NewIndexCount, Resident->IndexCapacity, 3u);
		// Include the old allocation during growth; one alignment allowance per new stream.
		const uint64 DecodeBytes = ExistingBufferBytes + uint64(Resident->GetRequiredBytes(VertexCapacity, IndexCapacity))
			+ uint64(Resident->Streams.Num()) * CSGpuSurfaceBudget::Alignment
			+ BufferBytes(uint64(Plan.Num()) * sizeof(FCSNaniteCutPlanEntry))
			+ BufferBytes(uint64(WriteMatrices.Num()) * sizeof(FVector4f))
			+ BufferBytes(uint64(WriteInfo.Num()) * sizeof(FUintVector4))
			+ BufferBytes(uint64(MaterialRemap.Num()) * sizeof(uint32));
		if (DecodeBytes > Options.MaxWorkingMemoryBytes)
		{
			UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Decode exceeds the working buffer budget (%llu > %llu bytes); no geometry allocated."), DecodeBytes, Options.MaxWorkingMemoryBytes);
			CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
			Finish();
			return Target;
		}
	}

	// ---- 5. 扩容 + 写出趟 ---------------------------------------------------------------------
	// 计数趟与写出趟之间游戏线程一直阻塞着，不会渲染任何一帧，流送管理器也就不会挪动页 ——
	// 记录里的 GPU 页号在写出时仍然有效。扩容自己的 flush 同样不渲染。
	if (!Target->EnsureCapacitySync(int32(NewVertexCount), int32(NewIndexCount)))
	{
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Target capacity refused (%u vertices, %u indices)."), NewVertexCount, NewIndexCount);
		CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
		Finish();
		return Target;
	}
	const uint32 NumTexCoordSets = uint32(FMath::Max(UCSMeshOps::GetTexCoordSets(Target), 1));

	bool bWrote = false;
	Target->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		FRDGBufferRef Positions = Context.Positions();
		FRDGBufferRef Tangents = Context.Tangents();
		FRDGBufferRef TexCoords = Context.TexCoords();
		FRDGBufferRef Colors = Context.Colors();
		FRDGBufferRef Indices = Context.Indices();
		FRDGBufferRef MaterialIds = Context.MaterialIds();
		if (!Positions || !Tangents || !TexCoords || !Colors || !Indices || !MaterialIds) return;

		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		FRDGBufferRef PlanBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CSNaniteCut.Plan"), Plan);
		FRDGBufferRef MatricesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CSNaniteCut.WriteMatrices"), WriteMatrices);
		FRDGBufferRef InfoBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("CSNaniteCut.WriteInfo"), WriteInfo);
		FRDGBufferRef RemapBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaterialRemap.Num()), TEXT("CSNaniteCut.MaterialRemap"));
		GraphBuilder.QueueBufferUpload(RemapBuffer, MaterialRemap.GetData(), MaterialRemap.Num() * sizeof(uint32));

		FCSNaniteCutWriteClustersCS::FParameters* Params = GraphBuilder.AllocParameters<FCSNaniteCutWriteClustersCS::FParameters>();
		Params->ClusterPageData = Nanite::GStreamingManager.GetClusterPageDataSRV(GraphBuilder);
		Params->PageConstants = FIntVector4(0, int32(Nanite::GStreamingManager.GetMaxStreamingPages()), 0, 0);
		Params->CSNC_Plan = GraphBuilder.CreateSRV(PlanBuffer);
		Params->CSNC_NumPlanEntries = uint32(Plan.Num());
		Params->CSNC_WriteMatrices = GraphBuilder.CreateSRV(MatricesBuffer);
		Params->CSNC_WriteInfo = GraphBuilder.CreateSRV(InfoBuffer);
		Params->CSNC_MaterialRemap = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(RemapBuffer, PF_R32_UINT));
		Params->CSNC_NumTexCoordSets = NumTexCoordSets;
		Params->CSNC_VertexCapacity = Context.Resident.VertexCapacity;
		Params->CSNC_IndexCapacity = Context.Resident.IndexCapacity;
		Params->RW_Positions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Positions, PF_R32_FLOAT));
		Params->RW_Tangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tangents, PF_R32_UINT));
		Params->RW_TexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoords, PF_R32_FLOAT));
		Params->RW_Colors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Colors, PF_R32_UINT));
		Params->RW_Indices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Indices, PF_R32_UINT));
		Params->RW_MaterialIds = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT));

		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSNaniteCut.WriteClusters"),
			TShaderMapRef<FCSNaniteCutWriteClustersCS>(GetGlobalShaderMap(GMaxRHIFeatureLevel)), Params,
			FComputeShaderUtils::GetGroupCountWrapped(Plan.Num()));

		// 计数与间接参数一起写；它顺带丢掉 section 表（改了三角数的算子都得丢）。
		UCSMeshOps::AddSetCountersPass(Context, NewVertexCount, NewIndexCount);
		Context.SetWorldBounds(Options.bAppend ? (Context.GetWorldBounds() + WrittenBounds) : WrittenBounds);
		bWrote = true;
	});

	if (!bWrote)
	{
		UE_LOG(LogCSNaniteCut, Warning, TEXT("[CSNaniteCut] Target has no standard stream set to write into."));
		CSNaniteCut_SetAllStatus(OutResult, Prepared, Ready, ECSNaniteCutStatus::Failed);
		Finish();
		return Target;
	}

	Target->Materials = MoveTemp(NewMaterials);
	Target->NotifyMaterialsChanged();

	OutResult.WrittenVertices = int32(NewVertexCount - BaseVertices);
	OutResult.WrittenTriangles = int32((NewIndexCount - BaseIndices) / 3u);
	Finish();
	return Target;
}

UCSMesh* UCSNaniteCutOps::AppendNaniteCut(
	UCSMesh* Target, UStaticMesh* Mesh, const FTransform& Transform, float CutError,
	const FCSNaniteCutOptions& Options, FCSNaniteCutResult& OutResult)
{
	FCSNaniteCutSource Source;
	Source.Mesh = Mesh;
	Source.Transform = Transform;
	Source.CutError = CutError;
	return AppendNaniteCuts(Target, { Source }, Options, OutResult);
}

float UCSNaniteCutOps::CutErrorForScreenError(float Distance, float PixelError, float ScreenWidth, float HorizontalFOVDegrees)
{
	const float HalfFOV = FMath::DegreesToRadians(FMath::Clamp(HorizontalFOVDegrees, 1.0f, 179.0f) * 0.5f);
	const float LODScale = FMath::Max(ScreenWidth, 1.0f) / (2.0f * FMath::Tan(HalfFOV));
	return FMath::Max(PixelError, 0.0f) * FMath::Max(Distance, 0.0f) / LODScale;
}

FName UCSNaniteCutOps::DefaultExcludeTag()
{
	static const FName Tag(TEXT("NaniteCutHLOD_Exclude"));
	return Tag;
}

FName UCSNaniteCutOps::DefaultPickTag()
{
	static const FName Tag(TEXT("NaniteCutHLOD_Pick"));
	return Tag;
}

namespace
{
/** 手输的标签容忍写法差异：空格 / 下划线 / 连字符去掉再比，大小写 FName 本来就不分。 */
FString CSNaniteCut_NormalizeTag(FName Tag)
{
	FString Text = Tag.ToString();
	Text.ReplaceInline(TEXT("_"), TEXT(""));
	Text.ReplaceInline(TEXT("-"), TEXT(""));
	Text.ReplaceInline(TEXT(" "), TEXT(""));
	return Text.ToLower();
}

bool CSNaniteCut_TagsContain(const TArray<FName>& Tags, FName Wanted, const FString& WantedNormalized)
{
	// 先按原样比（绝大多数情况一次命中，不建临时字符串），不中再按归一化比。
	for (FName Tag : Tags)
		if (Tag == Wanted) return true;
	for (FName Tag : Tags)
		if (CSNaniteCut_NormalizeTag(Tag) == WantedNormalized) return true;
	return false;
}
}

bool UCSNaniteCutOps::ComponentOrOwnerHasTag(const UActorComponent* Component, FName Tag)
{
	if (!Component || Tag.IsNone()) return false;
	const FString Normalized = CSNaniteCut_NormalizeTag(Tag);
	if (CSNaniteCut_TagsContain(Component->ComponentTags, Tag, Normalized)) return true;
	const AActor* Owner = Component->GetOwner();
	return Owner && CSNaniteCut_TagsContain(Owner->Tags, Tag, Normalized);
}

bool UCSNaniteCutOps::IsExcludedByTag(const UActorComponent* Component, FName ExcludeTag)
{
	return ComponentOrOwnerHasTag(Component, ExcludeTag);
}

bool UCSNaniteCutOps::IsPickedByTag(const UActorComponent* Component, FName PickTag)
{
	// None = 不限制：盒子里的都收，与加这个属性之前的行为一致。
	return PickTag.IsNone() || ComponentOrOwnerHasTag(Component, PickTag);
}

void UCSNaniteCutOps::MakeSourcesFromComponents(
	const TArray<UStaticMeshComponent*>& Components, float WorldCutError, FName ExcludeTag,
	TArray<FCSNaniteCutSource>& OutSources, int32& OutNumExcluded, int32& OutNumFoliage, int32& OutMaxTexCoords,
	TArray<int32>* OutSourceComponentIndices)
{
	OutNumExcluded = 0;
	OutNumFoliage = 0;
	OutMaxTexCoords = 1;
	if (OutSourceComponentIndices) OutSourceComponentIndices->Reset();

	for (int32 ComponentIndex = 0; ComponentIndex < Components.Num(); ++ComponentIndex)
	{
		UStaticMeshComponent* Component = Components[ComponentIndex];
		if (!Component) continue;
		if (IsExcludedByTag(Component, ExcludeTag))
		{
			++OutNumExcluded;
			continue;
		}
		if (Component->IsA<UFoliageInstancedStaticMeshComponent>())
		{
			++OutNumFoliage;
			continue;
		}
		UStaticMesh* Mesh = Component->GetStaticMesh();
		if (!Mesh) continue;

		TArray<TObjectPtr<UMaterialInterface>> Materials;
		for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot) Materials.Add(Component->GetMaterial(Slot));
		if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
			if (RenderData->LODResources.Num() > 0) OutMaxTexCoords = FMath::Max(OutMaxTexCoords, int32(RenderData->LODResources[0].GetNumTexCoords()));

		auto AddSource = [&OutSources, OutSourceComponentIndices, ComponentIndex, Mesh, &Materials, WorldCutError](const FTransform& Transform)
		{
			const double MinScale = FMath::Max(Transform.GetScale3D().GetAbs().GetMin(), UE_KINDA_SMALL_NUMBER);
			FCSNaniteCutSource& Source = OutSources.AddDefaulted_GetRef();
			Source.Mesh = Mesh;
			Source.Transform = Transform;
			Source.CutError = float(WorldCutError / MinScale);
			Source.OverrideMaterials = Materials;
			if (OutSourceComponentIndices) OutSourceComponentIndices->Add(ComponentIndex);
		};

		if (const UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Component))
		{
			for (int32 Instance = 0; Instance < Instanced->GetInstanceCount(); ++Instance)
			{
				FTransform InstanceTransform;
				if (Instanced->GetInstanceTransform(Instance, InstanceTransform, /*bWorldSpace*/ true)) AddSource(InstanceTransform);
			}
		}
		else
		{
			AddSource(Component->GetComponentTransform());
		}
	}
}

bool UCSNaniteCutOps::RequestNaniteResidency(UStaticMesh* Mesh)
{
	if (!IsInGameThread()) return false;
	const Nanite::FResources* Resources = CSNaniteCut_GetResources(Mesh);
	if (!Resources) return false;
	if (Resources->PersistentHash == NANITE_INVALID_PERSISTENT_HASH) return false;
	if (uint32(Resources->PageStreamingStates.Num()) <= Resources->NumRootPages) return false;

	ENQUEUE_RENDER_COMMAND(CSNaniteCutRequestResidency)([Resources](FRHICommandListImmediate&)
	{
		CSNaniteCut_RequestAllPages(*Resources);
	});
	return true;
}

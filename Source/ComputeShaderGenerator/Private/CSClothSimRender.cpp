#include "CSClothSimRender.h"

#include "CSClothLayout.ush"   // CS_CLOTH_*（与 CSClothSim.usf 同一份）
#include "CSMesh.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"

// -----------------------------------------------------------------------------
// Shaders (Shaders/Private/CSClothSim.usf)
// -----------------------------------------------------------------------------

namespace
{
// Unity/jumbo 构建共享 TU：file-local 名字一律带 CSCloth_ 前缀。
constexpr uint32 CSCloth_GroupSize = 64;
}

#define CSCLOTH_SHADER_BODY(ShaderClass) \
	DECLARE_GLOBAL_SHADER(ShaderClass); \
	SHADER_USE_PARAMETER_STRUCT(ShaderClass, FGlobalShader); \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ \
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
	} \
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); \
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSCloth_GroupSize); \
	}

class FCSClothPredictCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothPredictCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_PrevPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RestPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PinGroups)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ParticleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PinTransforms)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(float, SubstepDt)
		SHADER_PARAMETER(float, InvPrevSubstepDt)
		SHADER_PARAMETER(float, SubstepAlpha)
		SHADER_PARAMETER(float, DampingFactor)
		SHADER_PARAMETER(FVector3f, Gravity)
		SHADER_PARAMETER(float, MaxSpeed)
		SHADER_PARAMETER(FVector3f, WindVelocity)
		SHADER_PARAMETER(float, WindTurbulence)
		SHADER_PARAMETER(FVector3f, WindNoiseOffset)
		SHADER_PARAMETER(float, WindNoiseFrequency)
		SHADER_PARAMETER(float, WindNormalDrag)
		SHADER_PARAMETER(float, WindTangentialDrag)
		SHADER_PARAMETER(uint32, bWind)
		SHADER_PARAMETER(uint32, bTeleport)
		SHADER_PARAMETER(FVector4f, TeleportRow0)
		SHADER_PARAMETER(FVector4f, TeleportRow1)
		SHADER_PARAMETER(FVector4f, TeleportRow2)
	END_SHADER_PARAMETER_STRUCT()
};

class FCSClothSolveColorCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothSolveColorCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, Constraints)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, RW_Lambdas)
		SHADER_PARAMETER(uint32, ColorOffset)
		SHADER_PARAMETER(uint32, ColorCount)
		SHADER_PARAMETER(float, InvSubstepDtSq)
		SHADER_PARAMETER(float, StretchCompliance)
		SHADER_PARAMETER(float, CompressionCompliance)
		SHADER_PARAMETER(float, BendCompliance)
		SHADER_PARAMETER(float, TetherCompliance)
		SHADER_PARAMETER(float, MaxCorrection)
		SHADER_PARAMETER(uint32, bFirstIteration)
	END_SHADER_PARAMETER_STRUCT()
};

class FCSClothCollideCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothCollideCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PrevPositions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Colliders)
		SHADER_PARAMETER(uint32, NumParticles)
		SHADER_PARAMETER(uint32, NumColliders)
		SHADER_PARAMETER(float, SubstepAlpha)
		SHADER_PARAMETER(float, CollisionThickness)
		SHADER_PARAMETER(float, Friction)
	END_SHADER_PARAMETER_STRUCT()
};

class FCSClothAccumulateNormalsCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothAccumulateNormalsCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TriangleParticles)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, RW_NormalAccum)
		SHADER_PARAMETER(uint32, NumTriangles)
	END_SHADER_PARAMETER_STRUCT()
};

class FCSClothFinalizeNormalsCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothFinalizeNormalsCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, RW_NormalAccum)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float4>, RW_ParticleNormals)
		SHADER_PARAMETER(uint32, NumParticles)
	END_SHADER_PARAMETER_STRUCT()
};

class FCSClothSkinBackCS : public FGlobalShader
{
	CSCLOTH_SHADER_BODY(FCSClothSkinBackCS)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Positions)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CornerToParticle)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, ParticleNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, RestNormals)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, RestTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_MeshPositions)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RW_MeshTangents)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RW_Status)
		SHADER_PARAMETER(FVector3f, SimOrigin)
		SHADER_PARAMETER(uint32, NumRenderVertices)
	END_SHADER_PARAMETER_STRUCT()
};

#undef CSCLOTH_SHADER_BODY

IMPLEMENT_GLOBAL_SHADER(FCSClothPredictCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "PredictCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSClothSolveColorCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "SolveColorCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSClothCollideCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "CollideCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSClothAccumulateNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "AccumulateNormalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSClothFinalizeNormalsCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "FinalizeNormalsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FCSClothSkinBackCS, "/Plugin/PCGPlugins/Shaders/Private/CSClothSim.usf", "SkinBackCS", SF_Compute);

// -----------------------------------------------------------------------------
// Pass helpers
// -----------------------------------------------------------------------------

namespace
{
FGlobalShaderMap* CSCloth_ShaderMap()
{
	return GetGlobalShaderMap(GMaxRHIFeatureLevel);
}

FIntVector CSCloth_Groups(uint32 Count)
{
	return FComputeShaderUtils::GetGroupCountWrapped(int32(FMath::Max(Count, 1u)), int32(CSCloth_GroupSize));
}

/** 一帧里注册进图的全部 sim buffer。 */
struct FCSClothGraphBuffers
{
	FRDGBufferRef Positions = nullptr;
	FRDGBufferRef PrevPositions = nullptr;
	FRDGBufferRef RestPositions = nullptr;
	FRDGBufferRef PinGroups = nullptr;
	FRDGBufferRef NormalAccum = nullptr;
	FRDGBufferRef ParticleNormals = nullptr;
	FRDGBufferRef RestNormals = nullptr;
	FRDGBufferRef Constraints = nullptr;
	FRDGBufferRef Lambdas = nullptr;
	FRDGBufferRef TriangleParticles = nullptr;
	FRDGBufferRef CornerToParticle = nullptr;
	FRDGBufferRef RestTangents = nullptr;
	FRDGBufferRef Status = nullptr;
};

FCSClothGraphBuffers CSCloth_Register(FRDGBuilder& GraphBuilder, const FCSClothGpuState& State)
{
	// sim 自持的 buffer 只在本插件的 compute pass 之间流转，跨图的访问态交给 RDG 默认收尾（SRVMask）：
	// 每帧重新登记、按需转成 UAV / SRV，没有图外的消费者会撞见它们（风险一节「sim 自有 buffer 的访问态」）。
	FCSClothGraphBuffers B;
	B.Positions = GraphBuilder.RegisterExternalBuffer(State.Positions, TEXT("CSCloth.Positions"));
	B.PrevPositions = GraphBuilder.RegisterExternalBuffer(State.PrevPositions, TEXT("CSCloth.PrevPositions"));
	B.RestPositions = GraphBuilder.RegisterExternalBuffer(State.RestPositions, TEXT("CSCloth.RestPositions"));
	B.PinGroups = GraphBuilder.RegisterExternalBuffer(State.PinGroups, TEXT("CSCloth.PinGroups"));
	B.NormalAccum = GraphBuilder.RegisterExternalBuffer(State.NormalAccum, TEXT("CSCloth.NormalAccum"));
	B.ParticleNormals = GraphBuilder.RegisterExternalBuffer(State.ParticleNormals, TEXT("CSCloth.ParticleNormals"));
	B.RestNormals = GraphBuilder.RegisterExternalBuffer(State.RestNormals, TEXT("CSCloth.RestNormals"));
	B.Constraints = GraphBuilder.RegisterExternalBuffer(State.Constraints, TEXT("CSCloth.Constraints"));
	B.Lambdas = GraphBuilder.RegisterExternalBuffer(State.Lambdas, TEXT("CSCloth.Lambdas"));
	B.TriangleParticles = GraphBuilder.RegisterExternalBuffer(State.TriangleParticles, TEXT("CSCloth.TriangleParticles"));
	B.CornerToParticle = GraphBuilder.RegisterExternalBuffer(State.CornerToParticle, TEXT("CSCloth.CornerToParticle"));
	B.RestTangents = GraphBuilder.RegisterExternalBuffer(State.RestTangents, TEXT("CSCloth.RestTangents"));
	B.Status = GraphBuilder.RegisterExternalBuffer(State.Status, TEXT("CSCloth.Status"));
	return B;
}

/** 两条法线 pass：三角 → 定点累加，粒子 → 归一化写 ParticleNormals 并清零累加器。setup 与每帧共用。 */
void CSCloth_AddNormalPasses(FRDGBuilder& GraphBuilder, const FCSClothGraphBuffers& B, uint32 NumTriangles, uint32 NumParticles)
{
	FGlobalShaderMap* ShaderMap = CSCloth_ShaderMap();
	{
		FCSClothAccumulateNormalsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSClothAccumulateNormalsCS::FParameters>();
		Params->Positions = GraphBuilder.CreateSRV(B.Positions);
		Params->TriangleParticles = GraphBuilder.CreateSRV(B.TriangleParticles);
		Params->RW_NormalAccum = GraphBuilder.CreateUAV(B.NormalAccum);
		Params->NumTriangles = NumTriangles;
		TShaderMapRef<FCSClothAccumulateNormalsCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.AccumulateNormals"), Shader, Params, CSCloth_Groups(NumTriangles));
	}
	{
		FCSClothFinalizeNormalsCS::FParameters* Params = GraphBuilder.AllocParameters<FCSClothFinalizeNormalsCS::FParameters>();
		Params->RW_NormalAccum = GraphBuilder.CreateUAV(B.NormalAccum);
		Params->RW_ParticleNormals = GraphBuilder.CreateUAV(B.ParticleNormals);
		Params->NumParticles = NumParticles;
		TShaderMapRef<FCSClothFinalizeNormalsCS> Shader(ShaderMap);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.FinalizeNormals"), Shader, Params, CSCloth_Groups(NumParticles));
	}
}

/** 只登记网格的 Position 与 TangentBasis 两条流（每帧的借图编辑不必为十几条不碰的流建图）。 */
uint64 CSCloth_MeshStreamMask(const FCSMeshResident& Resident)
{
	const int32 PositionIndex = Resident.FindStreamIndex(ECSGpuStreamRole::Position);
	const int32 TangentIndex = Resident.FindStreamIndex(ECSGpuStreamRole::TangentBasis);
	if (PositionIndex == INDEX_NONE || TangentIndex == INDEX_NONE) return 0;
	// 下标 ≥ 64 的流恒登记（见 FCSMeshEditContext 的构造函数），掩码里不用管它们。
	uint64 Mask = 0;
	if (PositionIndex < 64) Mask |= 1ull << PositionIndex;
	if (TangentIndex < 64) Mask |= 1ull << TangentIndex;
	return Mask;
}

/**
 * 一帧的尾巴：法线 → 写回网格流 → 非阻塞状态回读。模拟帧与重置共用。
 *
 * 写回借 FCSMeshRenderThreadEdit 只登记 Position / TangentBasis 两条流，作用域结束时恢复它们的常态访问
 * 状态。不 bump Generation、不广播 —— 每帧只有 buffer 内容在变，渲染叶子照常画同一批 buffer（计划 D2）。
 */
void CSCloth_AddWriteBackPasses(FRDGBuilder& GraphBuilder, FCSClothGpuState& State, const FCSClothGraphBuffers& B,
	FCSMeshResident& Resident, const FVector3f& SimOrigin)
{
	CSCloth_AddNormalPasses(GraphBuilder, B, State.NumTriangles, State.NumParticles);

	// 状态在写回前清零：RW_Status[0] 数的是「这一帧」写回时撞见的非有限粒子。
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(B.Status), 0u);

	if (State.NumRenderVertices <= Resident.VertexCapacity)
	{
		FCSMeshRenderThreadEdit Edit(GraphBuilder, Resident, CSCloth_MeshStreamMask(Resident));
		FRDGBufferRef MeshPositions = Edit->Positions();
		FRDGBufferRef MeshTangents = Edit->Tangents();
		if (MeshPositions && MeshTangents)
		{
			FCSClothSkinBackCS::FParameters* P = GraphBuilder.AllocParameters<FCSClothSkinBackCS::FParameters>();
			P->Positions = GraphBuilder.CreateSRV(B.Positions);
			P->CornerToParticle = GraphBuilder.CreateSRV(B.CornerToParticle);
			P->ParticleNormals = GraphBuilder.CreateSRV(B.ParticleNormals);
			P->RestNormals = GraphBuilder.CreateSRV(B.RestNormals);
			P->RestTangents = GraphBuilder.CreateSRV(B.RestTangents);
			P->RW_MeshPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshPositions, PF_R32_FLOAT));
			P->RW_MeshTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshTangents, PF_R32_UINT));
			P->RW_Status = GraphBuilder.CreateUAV(B.Status);
			P->SimOrigin = SimOrigin;
			P->NumRenderVertices = State.NumRenderVertices;
			TShaderMapRef<FCSClothSkinBackCS> Shader(CSCloth_ShaderMap());
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.SkinBack"), Shader, P, CSCloth_Groups(State.NumRenderVertices));
		}
	}

	// 非阻塞的状态回读：上一份还在途就跳过这一帧（NaN 不会自己消失，下一份照样能看见）。
	if (!State.StatusReadback)
	{
		State.StatusReadback = new FRHIGPUBufferReadback(TEXT("CSCloth.StatusReadback"));
		AddEnqueueCopyPass(GraphBuilder, State.StatusReadback, B.Status, sizeof(uint32) * CS_CLOTH_STATUS_UINTS);
	}
}

/** 把落地的状态回读收进原子量；没落地就什么都不做（下一帧再看）。 */
void CSCloth_PollStatus(FCSClothGpuState& State)
{
	if (!State.StatusReadback || !State.StatusReadback->IsReady()) return;
	if (const uint32* Values = static_cast<const uint32*>(State.StatusReadback->Lock(sizeof(uint32) * CS_CLOTH_STATUS_UINTS)))
	{
		State.NonFiniteParticles.store(Values[0], std::memory_order_relaxed);
		State.StatusReadback->Unlock();
	}
	delete State.StatusReadback;
	State.StatusReadback = nullptr;
}
}

FCSClothGpuState::~FCSClothGpuState()
{
	// 回读对象只能在渲染线程删：组件的释放路径保证最后一份引用在渲染命令里放掉。
	// 万一走到别的线程（引擎关闭期间的强制回收），宁可漏掉这几十字节也不在错误的线程上碰 RHI。
	if (StatusReadback && IsInRenderingThread()) delete StatusReadback;
	StatusReadback = nullptr;
}

namespace CSClothRender
{
void InitState_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
	const FCSClothSetupData& Data, FCSMeshResident& Resident)
{
	check(IsInRenderingThread());

	State.NumParticles = uint32(Data.NumParticles);
	State.NumConstraints = uint32(Data.Constraints.Num());
	State.NumTriangles = uint32(Data.TriangleParticles.Num() / 3);
	State.NumRenderVertices = uint32(Data.NumRenderVertices);
	State.NumPinGroups = uint32(FMath::Max(Data.PinGroupReach.Num(), 1));
	State.ColorRanges = Data.ColorRanges;
	State.LastSubstepDt = 0.0f;
	State.NonFiniteParticles.store(0, std::memory_order_relaxed);

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCloth.Setup"));
	const uint32 N = FMath::Max(State.NumParticles, 1u);

	FCSClothGraphBuffers B;
	B.Positions = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.Positions"), Data.RestPositions);
	B.PrevPositions = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.PrevPositions"), Data.RestPositions);
	B.RestPositions = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.RestPositions"), Data.RestPositions);
	B.PinGroups = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.PinGroups"), Data.PinGroups);
	B.Constraints = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.Constraints"), Data.Constraints);
	B.TriangleParticles = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.TriangleParticles"), Data.TriangleParticles);
	B.CornerToParticle = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.CornerToParticle"), Data.CornerToParticle);

	// 粒子法线先填 +Z：法线 pass 只覆盖累加出非零和的粒子，其余（不属于任何三角的孤立粒子）留着这个兜底。
	TArray<FVector4f> UpNormals;
	UpNormals.Init(FVector4f(0.0f, 0.0f, 1.0f, 0.0f), int32(N));
	B.ParticleNormals = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.ParticleNormals"), UpNormals);
	B.RestNormals = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), N), TEXT("CSCloth.RestNormals"));

	B.NormalAccum = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(int32), N * 3), TEXT("CSCloth.NormalAccum"));
	B.Lambdas = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(float), FMath::Max(State.NumConstraints, 1u)), TEXT("CSCloth.Lambdas"));
	B.Status = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), CS_CLOTH_STATUS_UINTS), TEXT("CSCloth.Status"));
	B.RestTangents = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max(State.NumRenderVertices, 1u) * 2), TEXT("CSCloth.RestTangents"));

	// 法线累加器的不变式是「两次使用之间恒为零」：Finalize 读完即清，起点在这里清。
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(B.NormalAccum), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(B.Lambdas), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(B.Status), 0u);

	// rest 切线基 = 网格切线流此刻的原样拷贝（含副切线符号；CPU 快照解包时把符号丢了，所以不从那边来）。
	{
		FCSMeshRenderThreadEdit Edit(GraphBuilder, Resident, CSCloth_MeshStreamMask(Resident));
		FRDGBufferRef MeshTangents = Edit->Tangents();
		const uint64 Bytes = uint64(State.NumRenderVertices) * 2u * sizeof(uint32);
		if (MeshTangents && Bytes > 0 && Bytes <= MeshTangents->Desc.GetSize())
		{
			AddCopyBufferPass(GraphBuilder, B.RestTangents, 0, MeshTangents, 0, Bytes);
		}
		else
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(B.RestTangents), 0u);
		}
	}

	CSCloth_AddNormalPasses(GraphBuilder, B, State.NumTriangles, State.NumParticles);
	AddCopyBufferPass(GraphBuilder, B.RestNormals, B.ParticleNormals);

	State.Positions = GraphBuilder.ConvertToExternalBuffer(B.Positions);
	State.PrevPositions = GraphBuilder.ConvertToExternalBuffer(B.PrevPositions);
	State.RestPositions = GraphBuilder.ConvertToExternalBuffer(B.RestPositions);
	State.PinGroups = GraphBuilder.ConvertToExternalBuffer(B.PinGroups);
	State.NormalAccum = GraphBuilder.ConvertToExternalBuffer(B.NormalAccum);
	State.ParticleNormals = GraphBuilder.ConvertToExternalBuffer(B.ParticleNormals);
	State.RestNormals = GraphBuilder.ConvertToExternalBuffer(B.RestNormals);
	State.Constraints = GraphBuilder.ConvertToExternalBuffer(B.Constraints);
	State.Lambdas = GraphBuilder.ConvertToExternalBuffer(B.Lambdas);
	State.TriangleParticles = GraphBuilder.ConvertToExternalBuffer(B.TriangleParticles);
	State.CornerToParticle = GraphBuilder.ConvertToExternalBuffer(B.CornerToParticle);
	State.RestTangents = GraphBuilder.ConvertToExternalBuffer(B.RestTangents);
	State.Status = GraphBuilder.ConvertToExternalBuffer(B.Status);

	GraphBuilder.Execute();
}

void SimulateFrame_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
	const FCSClothFrameParams& Params, FCSMeshResident& Resident)
{
	check(IsInRenderingThread());
	if (!State.IsInitialized() || State.NumParticles == 0) return;

	// 上一帧的状态回读落地了就收下（非阻塞：没落地就下一帧再看）。
	CSCloth_PollStatus(State);

	const int32 Substeps = FMath::Clamp(Params.Substeps, 1, 64);
	const int32 Iterations = FMath::Clamp(Params.Iterations, 1, 16);
	const float Dt = FMath::Max(Params.SubstepDt, 1e-6f);
	const float PrevDt = State.LastSubstepDt > 0.0f ? State.LastSubstepDt : Dt;
	if (Params.PinTransforms.Num() < int32(State.NumPinGroups) * CS_CLOTH_PIN_GROUP_FLOAT4S) return;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCloth.Simulate"));
	FGlobalShaderMap* ShaderMap = CSCloth_ShaderMap();
	const FCSClothGraphBuffers B = CSCloth_Register(GraphBuilder, State);

	FRDGBufferRef PinTransforms = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.PinTransforms"), Params.PinTransforms);
	const uint32 NumColliders = uint32(Params.Colliders.Num() / CS_CLOTH_COLLIDER_FLOAT4S);
	FRDGBufferRef Colliders = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.Colliders"), Params.Colliders);

	for (int32 Substep = 0; Substep < Substeps; ++Substep)
	{
		const float Alpha = float(Substep + 1) / float(Substeps);
		{
			FCSClothPredictCS::FParameters* P = GraphBuilder.AllocParameters<FCSClothPredictCS::FParameters>();
			P->RW_Positions = GraphBuilder.CreateUAV(B.Positions);
			P->RW_PrevPositions = GraphBuilder.CreateUAV(B.PrevPositions);
			P->RestPositions = GraphBuilder.CreateSRV(B.RestPositions);
			P->PinGroups = GraphBuilder.CreateSRV(B.PinGroups);
			P->ParticleNormals = GraphBuilder.CreateSRV(B.ParticleNormals);
			P->PinTransforms = GraphBuilder.CreateSRV(PinTransforms);
			P->NumParticles = State.NumParticles;
			P->SubstepDt = Dt;
			P->InvPrevSubstepDt = 1.0f / (Substep == 0 ? PrevDt : Dt);
			P->SubstepAlpha = Alpha;
			P->DampingFactor = Params.DampingFactor;
			P->Gravity = Params.Gravity;
			P->MaxSpeed = Params.MaxSpeed;
			P->WindVelocity = Params.WindVelocity;
			P->WindTurbulence = Params.WindTurbulence;
			P->WindNoiseOffset = Params.WindNoiseOffset;
			P->WindNoiseFrequency = Params.WindNoiseFrequency;
			P->WindNormalDrag = Params.WindNormalDrag;
			P->WindTangentialDrag = Params.WindTangentialDrag;
			P->bWind = Params.bWind ? 1u : 0u;
			// 瞬移只在一帧的第一个 substep 做一次：之后的 substep 已经在新位置上了。
			P->bTeleport = (Params.bTeleport && Substep == 0) ? 1u : 0u;
			P->TeleportRow0 = Params.TeleportRows[0];
			P->TeleportRow1 = Params.TeleportRows[1];
			P->TeleportRow2 = Params.TeleportRows[2];
			TShaderMapRef<FCSClothPredictCS> Shader(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.Predict"), Shader, P, CSCloth_Groups(State.NumParticles));
		}

		// 上色的 Gauss-Seidel：颜色之间按顺序依赖（RDG 按同一个 UAV 的写后读排序，并插 UAV 屏障），
		// 颜色之内无冲突。λ 每个 substep 从第一次迭代起算。
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			for (const FIntPoint& Color : State.ColorRanges)
			{
				if (Color.Y <= 0) continue;
				FCSClothSolveColorCS::FParameters* P = GraphBuilder.AllocParameters<FCSClothSolveColorCS::FParameters>();
				P->RW_Positions = GraphBuilder.CreateUAV(B.Positions);
				P->Constraints = GraphBuilder.CreateSRV(B.Constraints);
				P->RW_Lambdas = GraphBuilder.CreateUAV(B.Lambdas);
				P->ColorOffset = uint32(Color.X);
				P->ColorCount = uint32(Color.Y);
				P->InvSubstepDtSq = 1.0f / (Dt * Dt);
				P->StretchCompliance = Params.StretchCompliance;
				P->CompressionCompliance = Params.CompressionCompliance;
				P->BendCompliance = Params.BendCompliance;
				P->TetherCompliance = Params.TetherCompliance;
				P->MaxCorrection = Params.MaxCorrection;
				P->bFirstIteration = Iteration == 0 ? 1u : 0u;
				TShaderMapRef<FCSClothSolveColorCS> Shader(ShaderMap);
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.SolveColor"), Shader, P, CSCloth_Groups(uint32(Color.Y)));
			}
		}

		if (NumColliders > 0)
		{
			FCSClothCollideCS::FParameters* P = GraphBuilder.AllocParameters<FCSClothCollideCS::FParameters>();
			P->RW_Positions = GraphBuilder.CreateUAV(B.Positions);
			P->PrevPositions = GraphBuilder.CreateSRV(B.PrevPositions);
			P->Colliders = GraphBuilder.CreateSRV(Colliders);
			P->NumParticles = State.NumParticles;
			P->NumColliders = NumColliders;
			P->SubstepAlpha = Alpha;
			P->CollisionThickness = Params.CollisionThickness;
			P->Friction = Params.Friction;
			TShaderMapRef<FCSClothCollideCS> Shader(ShaderMap);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCloth.Collide"), Shader, P, CSCloth_Groups(State.NumParticles));
		}
	}
	State.LastSubstepDt = Dt;

	CSCloth_AddWriteBackPasses(GraphBuilder, State, B, Resident, Params.SimOrigin);
	GraphBuilder.Execute();
	State.FramesSimulated.fetch_add(1, std::memory_order_relaxed);
}

void ResetToPositions_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
	const TArray<FVector4f>& Positions, FCSMeshResident& Resident, const FVector3f& SimOrigin)
{
	check(IsInRenderingThread());
	if (!State.IsInitialized() || Positions.Num() != int32(State.NumParticles)) return;

	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCloth.Reset"));
	const FCSClothGraphBuffers B = CSCloth_Register(GraphBuilder, State);
	FRDGBufferRef Upload = CreateStructuredBuffer(GraphBuilder, TEXT("CSCloth.ResetPositions"), Positions);
	AddCopyBufferPass(GraphBuilder, B.Positions, Upload);
	AddCopyBufferPass(GraphBuilder, B.PrevPositions, Upload);
	// 法线、写回与每帧同一条尾巴：网格当场回到 rest 姿态，不必等下一帧模拟（暂停时也一样）。
	CSCloth_AddWriteBackPasses(GraphBuilder, State, B, Resident, SimOrigin);
	GraphBuilder.Execute();

	// 速度从零开始：下一帧第一个 substep 的「上一步步长」取它自己的步长。
	State.LastSubstepDt = 0.0f;
}

bool ReadbackParticlesSync(const FCSClothGpuStateRef& State, TArray<FVector4f>& OutPositions)
{
	OutPositions.Reset();
	if (!IsInGameThread() || !State.IsValid()) return false;

	const uint32 NumParticles = State->NumParticles;
	if (NumParticles == 0) return false;
	const uint32 Bytes = NumParticles * sizeof(FVector4f);

	FRHIGPUBufferReadback* Readback = new FRHIGPUBufferReadback(TEXT("CSCloth.ParticleReadback"));
	ENQUEUE_RENDER_COMMAND(CSClothEnqueueParticleReadback)(
		[State, Readback, Bytes](FRHICommandListImmediate& RHICmdList)
		{
			if (!State->Positions.IsValid()) return;
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCloth.ReadbackParticles"));
			AddEnqueueCopyPass(GraphBuilder, Readback, GraphBuilder.RegisterExternalBuffer(State->Positions), Bytes);
			GraphBuilder.Execute();
		});
	UCSMesh::CountedBlockingFlush();

	bool bRead = false;
	ENQUEUE_RENDER_COMMAND(CSClothConsumeParticleReadback)(
		[Readback, Bytes, NumParticles, &OutPositions, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			if (!Readback->IsReady()) RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (Readback->IsReady() && Readback->GetGPUSizeBytes() >= uint64(Bytes))
			{
				if (const FVector4f* Data = static_cast<const FVector4f*>(Readback->Lock(Bytes)))
				{
					OutPositions.Append(Data, int32(NumParticles));
					Readback->Unlock();
					bRead = true;
				}
			}
			delete Readback;
		});
	UCSMesh::CountedBlockingFlush();
	return bRead;
}
}

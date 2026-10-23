#include "CSGroundRoadField.h"

#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSRoadField_ 前缀（与 CSRockShell_ / CSCover_ 等必须都不同）。

/** 2D 线程组边长，经 `ROADFIELD_GROUP` 喂给 kernel，两边只有这一处定义。 */
constexpr int32 CSRoadField_GroupSize = 8;

/**
 * X 趟：色流的一个通道 × PreGain 截到 [0,1]，再沿 X 做高斯。一线程一地面格点。
 * ⚠️ 参数结构里每一条都被 kernel 真读（`FComputeShaderUtils::AddPass` 会清掉没读的绑定）。
 */
class FCSGroundRoadFieldBlurXCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundRoadFieldBlurXCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundRoadFieldBlurXCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RoadFieldColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RoadField)
		SHADER_PARAMETER(FUintVector2, RoadFieldVerts)
		SHADER_PARAMETER(FVector4f, RoadFieldChannel)
		SHADER_PARAMETER(float, RoadFieldPreGain)
		SHADER_PARAMETER(uint32, RoadFieldTaps)
		SHADER_PARAMETER(float, RoadFieldInvTwoSigmaSq)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ROADFIELD_GROUP"), CSRoadField_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundRoadFieldBlurXCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRoadField.usf", "RoadFieldBlurXCS", SF_Compute);

/**
 * Y 趟（buffer 出口）：读 X 趟的结果沿 Y 做高斯，× PostGain 截到 [0,1]，可选与源值取 max。
 * 色流 / 通道 / PreGain 只在取 max 时读，但 kernel 里是动态分支 ⇒ 照样算"真读"，绑定不会被清掉。
 */
class FCSGroundRoadFieldBlurYCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundRoadFieldBlurYCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundRoadFieldBlurYCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RoadFieldBlurRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RoadFieldColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RW_RoadField)
		SHADER_PARAMETER(FUintVector2, RoadFieldVerts)
		SHADER_PARAMETER(FVector4f, RoadFieldChannel)
		SHADER_PARAMETER(float, RoadFieldPreGain)
		SHADER_PARAMETER(float, RoadFieldPostGain)
		SHADER_PARAMETER(uint32, RoadFieldTaps)
		SHADER_PARAMETER(float, RoadFieldInvTwoSigmaSq)
		SHADER_PARAMETER(uint32, RoadFieldMaxWithSource)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ROADFIELD_GROUP"), CSRoadField_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundRoadFieldBlurYCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRoadField.usf", "RoadFieldBlurYCS", SF_Compute);

/** Y 趟（贴图出口）：同上，写进 `RWTexture2D<float2>`（地面遮罩：R = 成品，G = 只糊的场）。 */
class FCSGroundRoadFieldBlurYTexCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundRoadFieldBlurYTexCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundRoadFieldBlurYTexCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, RoadFieldBlurRO)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, RoadFieldColors)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float2>, RW_RoadFieldTex)
		SHADER_PARAMETER(FUintVector2, RoadFieldVerts)
		SHADER_PARAMETER(FVector4f, RoadFieldChannel)
		SHADER_PARAMETER(float, RoadFieldPreGain)
		SHADER_PARAMETER(float, RoadFieldPostGain)
		SHADER_PARAMETER(uint32, RoadFieldTaps)
		SHADER_PARAMETER(float, RoadFieldInvTwoSigmaSq)
		SHADER_PARAMETER(uint32, RoadFieldMaxWithSource)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("ROADFIELD_GROUP"), CSRoadField_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundRoadFieldBlurYTexCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundRoadField.usf", "RoadFieldBlurYTexCS", SF_Compute);

/** 核的口径（都以**格**为单位）：伸展 = ceil(半径 / 格距)，σ = 半径 / 3（核截在 3σ）。 */
struct FCSRoadField_Kernel
{
	FIntPoint Verts = FIntPoint(0, 0);
	uint32 Taps = 0;
	float InvTwoSigmaSq = 0.0f;
	bool bUsable = false;
};

FCSRoadField_Kernel CSRoadField_MakeKernel(FRDGBufferRef GroundColors, const CSGroundRoadField::FBlurDesc& Desc)
{
	FCSRoadField_Kernel Out;
	Out.Verts = FIntPoint(FMath::Max(Desc.GroundVerts.X, 0), FMath::Max(Desc.GroundVerts.Y, 0));
	const uint64 NumVerts = uint64(Out.Verts.X) * uint64(Out.Verts.Y);
	Out.bUsable = Out.Verts.X >= 2 && Out.Verts.Y >= 2 && NumVerts <= uint64(MAX_uint32)
		&& GroundColors && uint64(GroundColors->Desc.NumElements) >= NumVerts;

	// 半径为 0 时一个抽头都没有，这时 1/(2σ²) 是 1/0 —— 给 0，kernel 里 K = 0 那一项的
	// exp(−0 × inv) 才不会变成 exp(NaN)。
	const float RadiusCells = FMath::Clamp(Desc.Radius / FMath::Max(Desc.GroundCellSize, 1e-3f), 0.0f, float(CSGroundRoadField::MaxTaps));
	Out.Taps = uint32(FMath::CeilToInt(RadiusCells));
	const float Sigma = RadiusCells / 3.0f;
	Out.InvTwoSigmaSq = Out.Taps > 0u ? 1.0f / FMath::Max(2.0f * Sigma * Sigma, 1e-6f) : 0.0f;
	return Out;
}

/** X 趟 → 临时 buffer。两个出口共用。 */
FRDGBufferRef CSRoadField_AddXPass(FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors,
	const CSGroundRoadField::FBlurDesc& Desc, const FCSRoadField_Kernel& Kernel)
{
	FRDGBufferRef Temp = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), uint32(Kernel.Verts.X * Kernel.Verts.Y)), TEXT("CSGroundRoadField.BlurX"));

	FCSGroundRoadFieldBlurXCS::FParameters* Params = GraphBuilder.AllocParameters<FCSGroundRoadFieldBlurXCS::FParameters>();
	Params->RoadFieldColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroundColors, PF_R32_UINT));
	Params->RW_RoadField = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Temp, PF_R32_FLOAT));
	Params->RoadFieldVerts = FUintVector2(uint32(Kernel.Verts.X), uint32(Kernel.Verts.Y));
	Params->RoadFieldChannel = Desc.Channel;
	// PreGain 在**模糊之前**乘（岩壳为什么要这个顺序见 usf 头注释）。
	Params->RoadFieldPreGain = FMath::Max(Desc.PreGain, 0.0f);
	Params->RoadFieldTaps = Kernel.Taps;
	Params->RoadFieldInvTwoSigmaSq = Kernel.InvTwoSigmaSq;
	TShaderMapRef<FCSGroundRoadFieldBlurXCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGroundRoadField.BlurX"), Shader, Params,
		FComputeShaderUtils::GetGroupCount(Kernel.Verts, CSRoadField_GroupSize));
	return Temp;
}

/** 两个 Y 出口共用的参数填写（两个参数结构逐字段同名，按模板填）。 */
template <typename ParamsType>
void CSRoadField_FillY(FRDGBuilder& GraphBuilder, ParamsType* Params, FRDGBufferRef Temp, FRDGBufferRef GroundColors,
	const CSGroundRoadField::FBlurDesc& Desc, const FCSRoadField_Kernel& Kernel)
{
	Params->RoadFieldBlurRO = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Temp, PF_R32_FLOAT));
	Params->RoadFieldColors = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GroundColors, PF_R32_UINT));
	Params->RoadFieldVerts = FUintVector2(uint32(Kernel.Verts.X), uint32(Kernel.Verts.Y));
	Params->RoadFieldChannel = Desc.Channel;
	Params->RoadFieldPreGain = FMath::Max(Desc.PreGain, 0.0f);
	Params->RoadFieldPostGain = FMath::Max(Desc.PostGain, 0.0f);
	Params->RoadFieldTaps = Kernel.Taps;
	Params->RoadFieldInvTwoSigmaSq = Kernel.InvTwoSigmaSq;
	Params->RoadFieldMaxWithSource = Desc.bMaxWithSource ? 1u : 0u;
}
}

namespace CSGroundRoadField
{
FRDGBufferRef AddBlurPasses(FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors, const FBlurDesc& Desc, const TCHAR* DebugName)
{
	const FCSRoadField_Kernel Kernel = CSRoadField_MakeKernel(GroundColors, Desc);
	const uint64 NumVerts = uint64(Kernel.Verts.X) * uint64(Kernel.Verts.Y);

	FRDGBufferRef Field = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(float), uint32(FMath::Max<uint64>(NumVerts, 1))), DebugName);
	if (!Kernel.bUsable)
	{
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Field, PF_R32_FLOAT)), 0.0f);
		return Field;
	}

	const FRDGBufferRef Temp = CSRoadField_AddXPass(GraphBuilder, GroundColors, Desc, Kernel);
	FCSGroundRoadFieldBlurYCS::FParameters* YParams = GraphBuilder.AllocParameters<FCSGroundRoadFieldBlurYCS::FParameters>();
	CSRoadField_FillY(GraphBuilder, YParams, Temp, GroundColors, Desc, Kernel);
	YParams->RW_RoadField = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Field, PF_R32_FLOAT));
	TShaderMapRef<FCSGroundRoadFieldBlurYCS> YShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGroundRoadField.BlurY"), YShader, YParams,
		FComputeShaderUtils::GetGroupCount(Kernel.Verts, CSRoadField_GroupSize));
	return Field;
}

void AddBlurPassesToTexture(FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors, const FBlurDesc& Desc, FRDGTextureRef Target)
{
	if (!Target) return;
	const FCSRoadField_Kernel Kernel = CSRoadField_MakeKernel(GroundColors, Desc);
	FRDGTextureUAVRef TargetUAV = GraphBuilder.CreateUAV(Target);
	// 尺寸对不上就清零而不是越界写：贴图与格点必须一一对应，差一格的话材质与地被会错开半格。
	if (!Kernel.bUsable || Target->Desc.Extent != Kernel.Verts)
	{
		AddClearUAVPass(GraphBuilder, TargetUAV, 0.0f);
		return;
	}

	const FRDGBufferRef Temp = CSRoadField_AddXPass(GraphBuilder, GroundColors, Desc, Kernel);
	FCSGroundRoadFieldBlurYTexCS::FParameters* YParams = GraphBuilder.AllocParameters<FCSGroundRoadFieldBlurYTexCS::FParameters>();
	CSRoadField_FillY(GraphBuilder, YParams, Temp, GroundColors, Desc, Kernel);
	YParams->RW_RoadFieldTex = TargetUAV;
	TShaderMapRef<FCSGroundRoadFieldBlurYTexCS> YShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGroundRoadField.BlurYTex"), YShader, YParams,
		FComputeShaderUtils::GetGroupCount(Kernel.Verts, CSRoadField_GroupSize));
}

bool UpdateMaskTexture(const FCSMeshResidentRef& Ground, UTextureRenderTarget2D* Target, const FBlurDesc& Desc)
{
	if (!Ground.IsValid() || !Target) return false;
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	if (!Resource) return false;

	ENQUEUE_RENDER_COMMAND(CSGroundRoadFieldUpdateMask)(
		[Resident = Ground, Resource, Desc](FRHICommandListImmediate& RHICmdList)
		{
			FRHITexture* RHITexture = Resource->GetRenderTargetTexture();
			if (!RHITexture) return;

			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGroundRoadField.UpdateMask"));
			FRDGTextureRef Texture = RegisterExternalTexture(GraphBuilder, RHITexture, TEXT("CSGround.GroundMask"));
			{
				// 地面网格的常驻流：进出都走 mesh 层自己的入口，访问状态由 ~FCSMeshRenderThreadEdit 恢复。
				FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);
				AddBlurPassesToTexture(GraphBuilder, Edit->Colors(), Desc, Texture);
			}
			// 材质在基础 pass 里采它、地被在自己的图里 Load 它 —— 两边都要 SRV 状态。
			GraphBuilder.SetTextureAccessFinal(Texture, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
	return true;
}
}

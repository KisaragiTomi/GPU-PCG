#pragma once

#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "Engine/TextureRenderTarget2D.h"
#include "TextureResource.h"
#include "components/InstancedStaticMeshComponent.h"
#include "IntVectorTypes.h"
#include "Components/BoxComponent.h"
#include "ComputeShaderSceneCapture.h"


#define CREATE_UAV(Size, Format, Name) \
FRDGTextureRef TRDG_##Name = nullptr; \
FRDGTextureUAVRef RDGUAV_##Name = nullptr; \
ConvertToUVATextureFormat(GraphBuilder, TRDG_##Name, RDGUAV_##Name, Size, Format, TEXT("UAV_"#Name)); \
PassParameters->RW_##Name = RDGUAV_##Name;

#define CREATE_TEXTURE_UAV_32(Name) \
CREATE_UAV(TextureSize, PF_A32B32G32R32F, Name)

#define CREATE_TEXTURE_UAV_16_OUT(Name) \
CREATE_UAV(TextureSize, PF_FloatRGBA, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); 


#define CREATE_TEXTURE_UAV_16_OUTP(Name) \
CREATE_UAV(TextureSize, PF_FloatRGBA, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); \
PassParameters->T_##Name = RDG_##Name;

#define CREATE_TEXTURE_UAV_32_OUT(Name) \
CREATE_UAV(TextureSize, PF_A32B32G32R32F, Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name)); 


#define CREATE_RDG(Name) \
FRDGTextureRef RDG_##Name = RegisterExternalTexture(GraphBuilder, R_##Name->GetRenderTargetTexture(), TEXT("R_"#Name));\
PassParameters->T_##Name = RDG_##Name;

#define CREATE_RDG_STRUCTURED_UPLOAD_SRV(Name, ElementType, ArrayData, DebugName) \
CSHelper::FRDGStructuredBufferRefs Name##Refs = CSHelper::CreateUploadedStructuredBuffer<ElementType>(GraphBuilder, ArrayData, DebugName, false, true); \
FRDGBufferRef Name##Buffer = Name##Refs.Buffer; \
FRDGBufferSRVRef Name##SRV = Name##Refs.SRV;

#define CREATE_RDG_STRUCTURED_UAV_SRV(Name, ElementType, ElementCount, DebugName) \
CSHelper::FRDGStructuredBufferRefs Name##Refs = CSHelper::CreateStructuredBuffer(GraphBuilder, sizeof(ElementType), ElementCount, DebugName, true, true); \
FRDGBufferRef Name##Buffer = Name##Refs.Buffer; \
FRDGBufferUAVRef Name##UAV = Name##Refs.UAV; \
FRDGBufferSRVRef Name##SRV = Name##Refs.SRV;


// --- Shader 声明样板宏（模块内原有 ~29 份手写副本收敛于此；变体声明保持手写） ---

// SM5 门槛的 ShouldCompilePermutation。
#define CSGEN_SHADER_PERM_SM5() \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ \
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); \
	}

// 恒真 ShouldCompilePermutation（平台无关的工具/后处理 shader）。
#define CSGEN_SHADER_PERM_ALWAYS() \
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) \
	{ \
		return true; \
	}

// SM5 + THREADGROUPSIZE_X（一维 compute 的标准组合）。
#define CSGEN_SHADER_PERM_SM5_GROUPSIZE_X(SizeX) \
	CSGEN_SHADER_PERM_SM5() \
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment) \
	{ \
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment); \
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), SizeX); \
	}

DECLARE_STATS_GROUP(TEXT("CSTest"), STATGROUP_CSTest, STATCAT_Advanced);

using namespace UE::Geometry;

namespace CSHelper
{
	struct FRDGStructuredBufferRefs
	{
		FRDGBufferRef Buffer = nullptr;
		FRDGBufferUAVRef UAV = nullptr;
		FRDGBufferSRVRef SRV = nullptr;
	};

	inline FRDGStructuredBufferRefs CreateStructuredBuffer(FRDGBuilder& GraphBuilder, uint32 BytesPerElement, uint32 NumElements, const TCHAR* Name = TEXT("StructuredBuffer"), bool bCreateUAV = true, bool bCreateSRV = true)
	{
		FRDGStructuredBufferRefs Refs;
		if (BytesPerElement == 0 || NumElements == 0)
		{
			return Refs;
		}

		Refs.Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(BytesPerElement, NumElements), Name);
		if (bCreateUAV)
		{
			Refs.UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Refs.Buffer));
		}
		if (bCreateSRV)
		{
			Refs.SRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Refs.Buffer));
		}
		return Refs;
	}

	template<typename ElementType>
	inline FRDGStructuredBufferRefs CreateUploadedStructuredBuffer(FRDGBuilder& GraphBuilder, const TArray<ElementType>& ArrayData, const TCHAR* Name = TEXT("StructuredBuffer"), bool bCreateUAV = false, bool bCreateSRV = true)
	{
		FRDGStructuredBufferRefs Refs = CreateStructuredBuffer(GraphBuilder, sizeof(ElementType), uint32(ArrayData.Num()), Name, bCreateUAV, bCreateSRV);
		if (Refs.Buffer)
		{
			GraphBuilder.QueueBufferUpload(Refs.Buffer, ArrayData.GetData(), sizeof(ElementType) * ArrayData.Num());
		}
		return Refs;
	}

	static FRDGTextureRef ConvertToUVATexture(FTextureRenderTargetResource* RenderTarget, FRDGBuilder& GraphBuilder, FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0), const TCHAR* Name = TEXT("TempTexture") )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(RenderTarget->GetSizeXY(), PF_FloatRGBA, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static FRDGTextureRef ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* RenderTarget,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(RenderTarget->GetSizeXY(), Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static FRDGTextureRef ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(Size, Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef TmpTexture = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, TmpTexture, ClearColor);
		return TmpTexture;
	}

	static void ConvertToUVATextureFormat(FRDGBuilder& GraphBuilder, FRDGTextureRef& OutTmpRDG, FRDGTextureUAVRef& OutRDGUAV, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc Desc_View(FRDGTextureDesc::Create2D(Size, Format, FClearValueBinding::White, TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV));
		OutTmpRDG = GraphBuilder.CreateTexture(Desc_View, Name);
		AddClearRenderTargetPass(GraphBuilder, OutTmpRDG, ClearColor);

		OutRDGUAV = GraphBuilder.CreateUAV(OutTmpRDG);

	}

	static void CreateUVATextureArrayFormat(FRDGBuilder& GraphBuilder, int32 ArraySize, FRDGTextureRef& OutTmpRDG, FRDGTextureUAVRef& OutRDGUAV, FIntPoint Size,  EPixelFormat Format = PF_FloatRGBA, const TCHAR* Name = TEXT("TempTexture"), FLinearColor ClearColor = FLinearColor(0 ,0 ,0, 0) )
	{
		FRDGTextureDesc TextureArrayDesc = FRDGTextureDesc::Create2DArray(
		Size,   // 单层纹理尺寸
		Format,                // 像素格式（如 PF_R8G8B8A8）
		FClearValueBinding::Black,  // 默认值
		TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_UAV, // 支持 UAV 读写
		ArraySize                   // 数组层数（即 RenderTarget 数量）
		);
		OutTmpRDG = GraphBuilder.CreateTexture(TextureArrayDesc, Name);
		AddClearRenderTargetPass(GraphBuilder, OutTmpRDG, ClearColor);

		OutRDGUAV = GraphBuilder.CreateUAV(OutTmpRDG);
	}
	
	static int32 GenerateTextureSize(int32 Iteration)
	{
		for (int i = 0; i < 12; i ++)
		{
			if (FMath::Pow(2.0, i) - 2 > Iteration)
				return FMath::Pow(2.0, i);
		}
		ensureMsgf(false, TEXT("GenerateTextureSize: Iteration %d exceeds max supported range (2046), clamping to 4096"), Iteration);
		return 4096;
	}

	// --- typed buffer "四连招"（Create→UAV→SRV→ClearUAV）收敛为一次调用 ---
	// 清零值的重载按 0u / 0.0f 自动分派；无 SRV 需求用双 out-param 重载。

	inline void CreateClearedTypedBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& OutBuffer, FRDGBufferUAVRef& OutUAV, FRDGBufferSRVRef& OutSRV, uint32 BytesPerElement, uint32 NumElements, EPixelFormat Format, const TCHAR* Name, uint32 ClearValue)
	{
		OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuffer, Format));
		OutSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OutBuffer, Format));
		AddClearUAVPass(GraphBuilder, OutUAV, ClearValue);
	}

	inline void CreateClearedTypedBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& OutBuffer, FRDGBufferUAVRef& OutUAV, FRDGBufferSRVRef& OutSRV, uint32 BytesPerElement, uint32 NumElements, EPixelFormat Format, const TCHAR* Name, float ClearValue)
	{
		OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuffer, Format));
		OutSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(OutBuffer, Format));
		AddClearUAVPass(GraphBuilder, OutUAV, ClearValue);
	}

	inline void CreateClearedTypedBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& OutBuffer, FRDGBufferUAVRef& OutUAV, uint32 BytesPerElement, uint32 NumElements, EPixelFormat Format, const TCHAR* Name, uint32 ClearValue)
	{
		OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuffer, Format));
		AddClearUAVPass(GraphBuilder, OutUAV, ClearValue);
	}

	inline void CreateClearedTypedBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& OutBuffer, FRDGBufferUAVRef& OutUAV, uint32 BytesPerElement, uint32 NumElements, EPixelFormat Format, const TCHAR* Name, float ClearValue)
	{
		OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		OutUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutBuffer, Format));
		AddClearUAVPass(GraphBuilder, OutUAV, ClearValue);
	}

	inline void CreateRWBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& RDGBuffer, FRDGBufferUAVRef& RDGUAVBuffer, uint32 NumElements, uint32 BytesPerElement, EPixelFormat Format = PF_A16B16G16R16, const TCHAR* Name = TEXT("UAV_Buffer"))
	{
		if (NumElements == 0 || BytesPerElement == 0) return;
		RDGBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), Name);
		RDGUAVBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RDGBuffer, Format));
		AddClearUAVPass(GraphBuilder,RDGUAVBuffer, 0);
	}

	template<typename T>
	void ConvertToUAVRWBuffer(FRDGBuilder& GraphBuilder, FRDGBufferRef& RDGBuffer, FRDGBufferUAVRef& RDGUAVBuffer, TArray<T> ArrayData, EPixelFormat Format = PF_A16B16G16R16, const TCHAR* Name = TEXT("UAV_Buffer"))
	{
		if (ArrayData.Num() == 0 || sizeof (T) == 0) return;
		RDGBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof (T), ArrayData.Num()), Name);
		GraphBuilder.QueueBufferUpload(RDGBuffer, ArrayData.GetData(), ArrayData.Num() * sizeof(T));
		RDGUAVBuffer = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RDGBuffer, Format));
	}

				
}

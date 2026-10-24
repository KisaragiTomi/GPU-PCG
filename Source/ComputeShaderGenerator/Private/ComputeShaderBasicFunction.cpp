#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGeneral.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderCliffGenerate.h"
#include "CSMesh.h"   // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口

IMPLEMENT_GLOBAL_SHADER(FConnectivityPixel, "/Plugin/PCGPlugins/Shaders/Private/Connectivity.usf", "ConnectivityPixel", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FBlurTexture, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "BlurTexture", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FUpPixelsMask, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "UpPixel", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGeneralFunctionShader, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "GeneralFunctionSet", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FTreeWindShader, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "TreeWind", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSampleSpline, "/Plugin/PCGPlugins/Shaders/Private/SampleSpline.usf", "SampleSpline", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSmoothSplineEval, "/Plugin/PCGPlugins/Shaders/Private/SmoothSpline.usf", "SmoothSplineEval", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FGlobalDistanceFieldForCS, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "DistanceFieldFunction", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FMeshFillMult, "/Plugin/PCGPlugins/Shaders/Private/MeshFill.usf", "MeshFillMult", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityGDFFill, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "FillOccupancyCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityCount, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "CountCavityCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityScanBlocks, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "ScanBlocksCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityScanBlockSums, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "ScanBlockSumsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityAddOffsets, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "AddOffsetsCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FVoxelCavityEmit, "/Plugin/PCGPlugins/Shaders/Private/VoxelCavitySpan.usf", "EmitCavityCS", SF_Compute);
#include "ComputeShaderGenerateHelper.h"
#include "CSRangeGeneratorActor.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2DArray.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "EngineModule.h"
#include "RHIGPUReadback.h"
#include "ImageCoreUtils.h"
#include "Landscape.h"
#if WITH_EDITOR
#include "LandscapeEditResourcesSubsystem.h"
#endif
#include "PixelShaderUtils.h"
#include "GeometryScript/PolyPathFunctions.h"
#include "DrawDebugHelpers.h"
#include "SceneView.h"
#include "GDFSampleService.h"

using namespace CSHelper;
using namespace FImageCoreUtils;

DECLARE_CYCLE_STAT(TEXT("CS Execute"), STAT_CSTest_Execute, STATGROUP_CSTest)

class FDrawCopyTexturePS : public FGlobalShader
{
public:

	enum class EDrawCopy : uint8
	{
		DC_CopyRWTexture,
		DC_CopyTexture,
		MAX
	};
	class FDrawCopy : SHADER_PERMUTATION_ENUM_CLASS("DrawCopy", EDrawCopy);
	using FPermutationDomain = TShaderPermutationDomain<FDrawCopy>;

	static TShaderMapRef<FDrawCopyTexturePS> CreatePermutation(EDrawCopy Permutation)
	{
		typename FPermutationDomain PermutationVector;
		PermutationVector.Set<FDrawCopy>(Permutation);
		TShaderMapRef<FDrawCopyTexturePS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
		return ComputeShader;
	}
	
	DECLARE_GLOBAL_SHADER(FDrawCopyTexturePS);
	SHADER_USE_PARAMETER_STRUCT(FDrawCopyTexturePS, FGlobalShader);
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, T_CopySource)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_CopySource)
		SHADER_PARAMETER_SAMPLER(SamplerState, SourceSampler)
		// SHADER_PARAMETER_SAMPLER(SamplerState, SourceSSProfilesSampler)
		// SHADER_PARAMETER(FVector4f, TextureSizeAndPixelSize)
		// SHADER_PARAMETER(int32, SourceSubsurfaceProfileInt)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return FDataDrivenShaderPlatformInfo::GetSupportsVertexShaderSRVs(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("DC_COPYRWTEXTURE"),
			TEXT("DC_COPYTEXTURE"),
		}; 
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)EDrawCopy::MAX, "Enum doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FDrawCopy>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
	}



};

IMPLEMENT_GLOBAL_SHADER(FDrawCopyTexturePS, "/Plugin/PCGPlugins/Shaders/Private/BasicFunction.usf", "FDrawCopyTexture", SF_Pixel);

namespace CSRTUpload
{
template<typename TPixel>
void UploadPixelsToRT(FTextureRenderTargetResource* TextureTarget, TArray<TPixel> Pixels)
{
	if (!TextureTarget) return;
	const int32 Width = TextureTarget->GetSizeXY().X;
	const int32 Height = TextureTarget->GetSizeXY().Y;
	if (Width * Height > Pixels.Num()) return;

	ENQUEUE_RENDER_COMMAND(CSUploadPixelsToRT)(
		[TextureTarget, Width, Height, Pixels = MoveTemp(Pixels)](FRHICommandListImmediate& RHICmdList)
	{
		FTextureRHIRef TextureRHI = TextureTarget->GetRenderTargetTexture();
		if (!TextureRHI.IsValid()) return;

		uint32 DestStride = 0;
		void* DestData = RHILockTexture2D(TextureRHI, 0, RLM_WriteOnly, DestStride, false);
		if (!DestData || DestStride == 0)
		{
			if (DestData) RHIUnlockTexture2D(TextureRHI, 0, false);
			return;
		}

		constexpr uint32 PixelBytes = sizeof(TPixel);
		const uint32 RowBytes = uint32(Width) * PixelBytes;
		const uint8* SourceBytes = reinterpret_cast<const uint8*>(Pixels.GetData());
		uint8* DestBytes = static_cast<uint8*>(DestData);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			FMemory::Memcpy(DestBytes + uint64(Y) * DestStride, SourceBytes + uint64(Y) * RowBytes, RowBytes);
		}

		RHIUnlockTexture2D(TextureRHI, 0, false);
	});
	UCSMesh::CountedBlockingFlush();
}
}

void UComputeShaderBasicFunction::DrawLinearColorsToRenderTarget32(UTextureRenderTarget2D* InTextureTarget,
	TArray<FLinearColor> Colors)
{
	if (!InTextureTarget) return;
	CSRTUpload::UploadPixelsToRT(InTextureTarget->GameThread_GetRenderTargetResource(), MoveTemp(Colors));
}

void UComputeShaderBasicFunction::DrawLinearColorsToRenderTarget16(UTextureRenderTarget2D* InTextureTarget,
	TArray<FLinearColor> Colors)
{
	if (!InTextureTarget) return;
	const int32 TexturePixelCount = InTextureTarget->SizeX * InTextureTarget->SizeY;
	if (TexturePixelCount > Colors.Num()) return;
	TArray<FFloat16Color> Colors16;
	Colors16.SetNumZeroed(TexturePixelCount);
	for (int32 i = 0; i < Colors.Num(); ++i)
	{
		Colors16[i] = FFloat16Color(Colors[i]);
	}
	DrawFFloat16ColorsToRenderTarget(InTextureTarget, MoveTemp(Colors16));
}

void UComputeShaderBasicFunction::DrawFFloat16ColorsToRenderTarget(UTextureRenderTarget2D* InTextureTarget,
	TArray<FFloat16Color> Colors16)
{
	if (!InTextureTarget) return;
	CSRTUpload::UploadPixelsToRT(InTextureTarget->GameThread_GetRenderTargetResource(), MoveTemp(Colors16));
}

void UComputeShaderBasicFunction::ConnectivityPixel(UTextureRenderTarget2D* InTextureTarget,
                                                    UTextureRenderTarget2D* InConnectivityMap, UTextureRenderTarget2D* InDebugView, int32 Channel, int32 TextureSize)
{
	if (InTextureTarget == nullptr || InConnectivityMap == nullptr || InDebugView == nullptr)
		return;
	
	// int32 TextureSize = 409;
	InTextureTarget->ResizeTarget(TextureSize, TextureSize);
	InConnectivityMap->ResizeTarget(TextureSize, TextureSize);
	InDebugView->ResizeTarget(TextureSize, TextureSize);
	FTextureRenderTargetResource* TextureTarget = InTextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* ConnectivityMap = InConnectivityMap->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DebugView = InDebugView->GameThread_GetRenderTargetResource();
	{
		SCOPE_CYCLE_COUNTER(STAT_CSTest_Execute);
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[=](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			{
				
				TShaderMapRef<FConnectivityPixel> ComputeShader_Init = FConnectivityPixel::CreateConnectivityPermutation(FConnectivityPixel::EConnectivityStep::CP_Init);
				TShaderMapRef<FConnectivityPixel> ComputeShader_FindIslands = FConnectivityPixel::CreateConnectivityPermutation(FConnectivityPixel::EConnectivityStep::CP_FindIslands);
				TShaderMapRef<FConnectivityPixel> ComputeShader_Count = FConnectivityPixel::CreateConnectivityPermutation(FConnectivityPixel::EConnectivityStep::CP_Count);
				TShaderMapRef<FConnectivityPixel> ComputeShader_NormalizeResult = FConnectivityPixel::CreateConnectivityPermutation(FConnectivityPixel::EConnectivityStep::CP_NormalizeResult);
				TShaderMapRef<FConnectivityPixel> ComputeShader_DrawTexture = FConnectivityPixel::CreateConnectivityPermutation(FConnectivityPixel::EConnectivityStep::CP_DrawTexture);
				
				FConnectivityPixel::FParameters* PassParameters = GraphBuilder.AllocParameters<FConnectivityPixel::FParameters>();
				FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureTarget->GetSizeXY().X, TextureTarget->GetSizeXY().Y, 1), 32);
				FIntPoint TextureSizeXY = ConnectivityMap->GetSizeXY();
				
				FRDGTextureRef TmpTexture_ConnectivityMap = ConvertToUVATexture(ConnectivityMap, GraphBuilder);
				FRDGTextureRef TmpTexture_DebugView = ConvertToUVATextureFormat(GraphBuilder,DebugView);
				FRDGTextureRef TextureTargetTexture = RegisterExternalTexture(GraphBuilder, TextureTarget->GetRenderTargetTexture(), TEXT("Input_RT"));
				
				FRDGTextureRef TmpTexture_LabelBufferA = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_A32B32G32R32F, TEXT("LabelBufferA"));
				FRDGTextureUAVRef TmpTextureUAV_LabelBufferA = GraphBuilder.CreateUAV(TmpTexture_LabelBufferA);
				FRDGTextureRef TmpTexture_LabelBufferB = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_A32B32G32R32F, TEXT("LabelBufferB"));
				FRDGTextureUAVRef TmpTextureUAV_LabelBufferB = GraphBuilder.CreateUAV(TmpTexture_LabelBufferB);
				FRDGTextureRef TmpTexture_CountBuffer = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_R32_UINT, TEXT("CountBuffer"));
				FRDGTextureUAVRef TmpTextureUAV_CountBuffer = GraphBuilder.CreateUAV(TmpTexture_CountBuffer);


				const uint32 NumElements = TextureTarget->GetSizeXY().X * TextureTarget->GetSizeXY().Y;
				const uint32 BytesPerElement = sizeof(uint32);
				FRDGBufferRef TmpRDGBuffer_NormalizeCounter; 
				FRDGBufferUAVRef RDGUAVBuffer_NormalizeCounter;
				CreateRWBuffer(GraphBuilder, TmpRDGBuffer_NormalizeCounter, RDGUAVBuffer_NormalizeCounter, 1, BytesPerElement, EPixelFormat::PF_R32_UINT, TEXT("NormalizeCountBuffer"));
				
				const uint32 BytesPerElementFloat4 = sizeof(FVector4f);
				FRDGBufferRef TmpRDGBuffer_Result;
				FRDGBufferUAVRef RDGUAVBuffer_Result;
				// 每个像素最多一个岛，结果按 float4 存（Connectivity.usf 的 RW_ResultBuffer）。
				CreateRWBuffer(GraphBuilder, TmpRDGBuffer_Result, RDGUAVBuffer_Result, NumElements, BytesPerElementFloat4, EPixelFormat::PF_A32B32G32R32F, TEXT("ResultBuffer"));
				
				// FRHIBuffer* brhi = Tmp_CountBuffer->GetRHI();
				// brhi->GetType();
				// GraphBuilder.QueueBufferUpload(Tmp_CountBuffer, test.GetData(), 4);
				
				
				PassParameters->InputTexture = TextureTargetTexture;
				PassParameters->RW_ConnectivityPixel = GraphBuilder.CreateUAV(TmpTexture_ConnectivityMap);
				PassParameters->RW_LabelBufferA = TmpTextureUAV_LabelBufferA;
				PassParameters->RW_LabelBufferB = TmpTextureUAV_LabelBufferB;
				PassParameters->RW_DebugView = GraphBuilder.CreateUAV(TmpTexture_DebugView);
				PassParameters->RW_LabelCounters = TmpTextureUAV_CountBuffer;
				PassParameters->RW_ResultBuffer = RDGUAVBuffer_Result;
				PassParameters->RW_NormalizeCounter = RDGUAVBuffer_NormalizeCounter;
				PassParameters->Channel = Channel;
				PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
				PassParameters->PieceNum = 0;
				
				//Init
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Init"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_Init, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Init, *PassParameters, GroupCount);
					});
				
				int32 Iteration = GroupCount.X * 2 ;
				// Iteration = 1;
				for (int32 i = 0; i < Iteration; i++)
				{
					GraphBuilder.AddPass(
					RDG_EVENT_NAME("FindIslands"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[i, PassParameters, ComputeShader_FindIslands, GroupCount, TmpTextureUAV_LabelBufferB, TmpTextureUAV_LabelBufferA](FRHIComputeCommandList& RHICmdList)
					{

						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FindIslands, *PassParameters, GroupCount);
						if ( i % 2 == 0)
						{
							PassParameters->RW_LabelBufferA = TmpTextureUAV_LabelBufferB;
							PassParameters->RW_LabelBufferB = TmpTextureUAV_LabelBufferA;
						}
						else
						{
							PassParameters->RW_LabelBufferA = TmpTextureUAV_LabelBufferA;
							PassParameters->RW_LabelBufferB = TmpTextureUAV_LabelBufferB;
						}

					});
				}
				// ERDGPassFlags::Raster
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Count"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_Count, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Count, *PassParameters, GroupCount);
					});
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("NormalizeResult"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_NormalizeResult, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_NormalizeResult, *PassParameters, GroupCount);
					});
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("DrawTexture"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_DrawTexture, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_DrawTexture, *PassParameters, GroupCount);
					});
				
				FRDGTextureRef ConnectivityMapTexture = RegisterExternalTexture(GraphBuilder, ConnectivityMap->GetRenderTargetTexture(), TEXT("Connectivity_RT"));
				AddCopyTexturePass(GraphBuilder, TmpTexture_ConnectivityMap, ConnectivityMapTexture, FRHICopyTextureInfo());
				
				FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
				AddCopyTexturePass(GraphBuilder, TmpTexture_DebugView, DebugViewTexture, FRHICopyTextureInfo());

				// TRefCountPtr<FRDGPooledBuffer> test = GraphBuilder.ConvertToExternalBuffer(Tmp_CountBuffer);
				// FRHIBuffer* Buffer = test->GetRHI();
				// void* DestData = RHILockBuffer(Buffer, )
				
			}
			GraphBuilder.Execute();
		});
		UCSMesh::CountedBlockingFlush();
	}
	
}

// ─── Blur shared internal helper ──────────────────────────────
static void BlurTexture_Internal(FRDGBuilder& GraphBuilder, FTextureRenderTargetResource* TextureTarget, FTextureRenderTargetResource* BlurTextureRT, float BlurScale, FBlurTexture::EBlurType BlurType)
{
	typename FBlurTexture::FPermutationDomain PermutationVector;
	PermutationVector.Set<FBlurTexture::FBlurFunctionSet>(BlurType);
	TShaderMapRef<FBlurTexture> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
	if (!ComputeShader.IsValid()) return;

	FBlurTexture::FParameters* PassParameters = GraphBuilder.AllocParameters<FBlurTexture::FParameters>();
	auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureTarget->GetSizeXY().X, TextureTarget->GetSizeXY().Y, 1), FComputeShaderUtils::kGolden2DGroupSize);
	FRDGTextureRef TmpTexture_BlurTexture = ConvertToUVATexture(BlurTextureRT, GraphBuilder);
	FRDGTextureRef TextureTargetTexture = RegisterExternalTexture(GraphBuilder, TextureTarget->GetRenderTargetTexture(), TEXT("Input_RT"));
	FRDGTextureRef BlurTextureTexture = RegisterExternalTexture(GraphBuilder, BlurTextureRT->GetRenderTargetTexture(), TEXT("Blur_RT"));
	PassParameters->T_BlurTexture = TextureTargetTexture;
	PassParameters->RW_BlurTexture = GraphBuilder.CreateUAV(TmpTexture_BlurTexture);
	PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->BlurScale = BlurScale;
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("Blur"),
		PassParameters,
		ERDGPassFlags::AsyncCompute,
		[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
		{
			FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
		});
	AddCopyTexturePass(GraphBuilder, TmpTexture_BlurTexture, BlurTextureTexture, FRHICopyTextureInfo());
}

void UComputeShaderBasicFunction::BlurTexture(UTextureRenderTarget2D* InTextureTarget,
	UTextureRenderTarget2D* OutBlurTexture, float BlurScale)
{
	if (InTextureTarget == nullptr || OutBlurTexture == nullptr)	return;
	SCOPE_CYCLE_COUNTER(STAT_CSTest_Execute);
	FTextureRenderTargetResource* TextureTarget = InTextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* BlurTextureRT = OutBlurTexture->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[TextureTarget, BlurTextureRT, BlurScale](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			BlurTexture_Internal(GraphBuilder, TextureTarget, BlurTextureRT, BlurScale, FBlurTexture::EBlurType::BT_BLUR3X3);
		}
		GraphBuilder.Execute();
	});
	UCSMesh::CountedBlockingFlush();
}

void UComputeShaderBasicFunction::BlurTextureRDG(FRDGBuilder& GraphBuilder, FRDGTextureRef& InTexture, FRDGTextureUAVRef& InTextureUAV, FRDGTextureRef& OutTexture, FIntVector GroupCount, FBlurTexture::EBlurType Type,float BlurScale)
{
	typename FBlurTexture::FPermutationDomain PermutationVector;
	PermutationVector.Set<FBlurTexture::FBlurFunctionSet>(Type);
	TShaderMapRef<FBlurTexture> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);

	FBlurTexture::FParameters* PassParameters = GraphBuilder.AllocParameters<FBlurTexture::FParameters>();
			
	PassParameters->T_BlurTexture = OutTexture;
	PassParameters->RW_BlurTexture = InTextureUAV ;
	PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->BlurScale = BlurScale;
			

	GraphBuilder.AddPass(
	RDG_EVENT_NAME("Blur"),
	PassParameters,
	ERDGPassFlags::AsyncCompute,
	[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
	{
		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
	});
	AddCopyTexturePass(GraphBuilder,InTexture , OutTexture, FRHICopyTextureInfo());
}

void UComputeShaderBasicFunction::BlurNormalTexture(UTextureRenderTarget2D* InTextureTarget,
	UTextureRenderTarget2D* OutBlurTexture, float BlurScale)
{
	if (InTextureTarget == nullptr || OutBlurTexture == nullptr) return;
	FTextureRenderTargetResource* TextureTarget = InTextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* BlurTextureRT = OutBlurTexture->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[TextureTarget, BlurTextureRT, BlurScale](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			BlurTexture_Internal(GraphBuilder, TextureTarget, BlurTextureRT, BlurScale, FBlurTexture::EBlurType::BT_BLURNORMAL3X3);
		}
		GraphBuilder.Execute();
	});
}

void UComputeShaderBasicFunction::UpPixelsMask(UTextureRenderTarget2D* InTextureTarget,
                                                UTextureRenderTarget2D* OutUpTexture, float Threshold, int32 Channel)
{
	if (InTextureTarget == nullptr || OutUpTexture == nullptr)
		return;

	FTextureRenderTargetResource* TextureTarget = InTextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* UpTexture = OutUpTexture->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			
			typename FUpPixelsMask::FPermutationDomain PermutationVector;
			TShaderMapRef<FUpPixelsMask> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
			
			bool bIsShaderValid = ComputeShader.IsValid();
		
			if (bIsShaderValid)
			{
				FUpPixelsMask::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpPixelsMask::FParameters>();
				auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TextureTarget->GetSizeXY().X, TextureTarget->GetSizeXY().Y, 1), FComputeShaderUtils::kGolden2DGroupSize);
				
				FRDGTextureRef TmpTexture_UpTexture = ConvertToUVATexture(UpTexture, GraphBuilder);
				FRDGTextureRef TextureTargetTexture = RegisterExternalTexture(GraphBuilder, TextureTarget->GetRenderTargetTexture(), TEXT("Input_RT"));
				FRDGTextureRef UpTextureTexture = RegisterExternalTexture(GraphBuilder, UpTexture->GetRenderTargetTexture(), TEXT("Connectivity_RT"));
				
				PassParameters->T_UpPixel = TextureTargetTexture;
				PassParameters->RW_UpPixel = GraphBuilder.CreateUAV(TmpTexture_UpTexture);
				PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
				PassParameters->UpPixelThreshold = Threshold;
				PassParameters->Channel = Channel;

				AddCopyTexturePass(GraphBuilder, UpTextureTexture, TmpTexture_UpTexture, FRHICopyTextureInfo());
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("ExecuteExampleComputeShader"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
					});
				
				AddCopyTexturePass(GraphBuilder, TmpTexture_UpTexture, UpTextureTexture, FRHICopyTextureInfo());
			}
		}
		GraphBuilder.Execute();

	});
}

void UComputeShaderBasicFunction::ExtentMaskFast(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* InDebugView, int32 Channel, int32 NumExtend)
{

	FTextureRenderTargetResource* TextureTarget = InTextureTarget->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DebugView = InDebugView->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FIntVector GroupSize = FIntVector(TextureTarget->GetSizeXY().X, TextureTarget->GetSizeXY().Y, 1);
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(GroupSize, 32);
			FIntPoint TextureSizeXY = TextureTarget->GetSizeXY();
			TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_MaskExtendFast);
			FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
			
			
			
			FRDGTextureRef TmpTexture_HeightNormal = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("InTexture_RWTexture")); 
			FRDGTextureUAVRef TmpTextureUAV_HeightNormal = GraphBuilder.CreateUAV(TmpTexture_HeightNormal);
			FRDGTextureRef TmpTexture_DebugView = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("DebugView_RWTexture")); 
			FRDGTextureUAVRef TmpTextureUAV_DebugView = GraphBuilder.CreateUAV(TmpTexture_DebugView);
			
			
			FRDGTextureRef InTexture =  RegisterExternalTexture(GraphBuilder, TextureTarget->GetRenderTargetTexture(), TEXT("InTexture_RT"));
			
			PassParameters->T_ProcssTexture0 = InTexture;
			PassParameters->RW_ProcssTexture0 = TmpTextureUAV_HeightNormal;
			PassParameters->RW_DebugView = TmpTextureUAV_DebugView;
			PassParameters->InputIntData0 = Channel;
			PassParameters->InputIntData1 = NumExtend;
			PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("MaskExtendFast"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});

			FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, DebugView->GetRenderTargetTexture(), TEXT("OutTexture_RT"));
			AddCopyTexturePass(GraphBuilder, TmpTexture_DebugView, DebugViewTexture, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpTexture_HeightNormal, DebugViewTexture, FRHICopyTextureInfo());

			
			
		}
		GraphBuilder.Execute();
	});
	UCSMesh::CountedBlockingFlush();
}

void UComputeShaderBasicFunction::DrawLinearColorToRenderTarget(UTextureRenderTarget2D* InTextureTarget,
	TArray<FLinearColor> InColors)
{
	if (InTextureTarget == nullptr || InColors.Num() == 0) return;
	
	FTextureRenderTargetResource* R_TextureTartet = InTextureTarget->GameThread_GetRenderTargetResource();
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_TextureTartet->GetSizeXY().X;
			float SizeY = R_TextureTartet->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);

			TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_DrawTexture16);
			FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			
			
			FRDGTextureRef TmpRDG_Texture = nullptr;
			FRDGTextureUAVRef RDGUAV_Texture = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_Texture, RDGUAV_Texture, TextureSize, InTextureTarget->GetFormat(), TEXT("UAV_Texture"));

			FRDGBufferRef TmpRDGB_Data = nullptr;
			FRDGBufferUAVRef RDGUAVB_Data = nullptr;
			ConvertToUAVRWBuffer(GraphBuilder, TmpRDGB_Data, RDGUAVB_Data, InColors, EPixelFormat::PF_A32B32G32R32F, TEXT("UAV_Data"));

			
			FRDGTextureRef RDG_Texture = RegisterExternalTexture(GraphBuilder, R_TextureTartet->GetRenderTargetTexture(), TEXT("R_Texture"));


			
			PassParameters->T_ProcssTexture0 = RDG_Texture;
			PassParameters->RW_ProcssTexture0 = RDGUAV_Texture;
			PassParameters->RWB_Float4_0 = RDGUAVB_Data;
			PassParameters->InputIntData0 = InColors.Num();
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CalVelocityHeight"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_Texture, RDG_Texture, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
	UCSMesh::CountedBlockingFlush();
}

void UComputeShaderBasicFunction::CalTreeWindTexture(UTextureRenderTarget2D* InWindTexture_PivotIndex,
                                                     UTextureRenderTarget2D* InWindTexture_DirExtent,
                                                     UTextureRenderTarget2D* InDebugView,
                                                     TArray<FVector4f> RootCenter4f, TArray<FVector4f> RootNormal4f,
                                                     int32 TextureSizeX,
                                                     int32 TextureSizeY)
{
	if (InWindTexture_PivotIndex == nullptr || InWindTexture_DirExtent == nullptr) return;
	InWindTexture_PivotIndex->ResizeTarget(TextureSizeX, TextureSizeY);
	InWindTexture_DirExtent->ResizeTarget(TextureSizeX, TextureSizeY);
	InDebugView->ResizeTarget(TextureSizeX, TextureSizeY);


	if (RootCenter4f.Num() == 0) return;
	
	FTextureRenderTargetResource* R_PivotIndex = InWindTexture_PivotIndex->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DirExtent = InWindTexture_DirExtent->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = InDebugView->GameThread_GetRenderTargetResource();
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			float SizeX = R_PivotIndex->GetSizeXY().X;
			float SizeY = R_DirExtent->GetSizeXY().Y;
			FIntPoint TextureSize = FIntPoint(SizeX, SizeY);

			typename FUpPixelsMask::FPermutationDomain PermutationVector;
			TShaderMapRef<FTreeWindShader> ComputeShader_TreeWind(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
			
			FTreeWindShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FTreeWindShader::FParameters>();
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			

			FRDGTextureRef TmpRDG_PivotIndex = nullptr;
			FRDGTextureUAVRef RDGUAV_PivotIndex = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_PivotIndex, RDGUAV_PivotIndex, TextureSize, PF_FloatRGBA, TEXT("UAV_PivotIndex"));

			FRDGTextureRef TmpRDG_DirExtent = nullptr;
			FRDGTextureUAVRef RDGUAV_DirExtent = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DirExtent, RDGUAV_DirExtent, TextureSize, PF_FloatRGBA, TEXT("UAV_DirExtent"));

			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_DebugView"));
			

			FRDGBufferRef TmpRDGB_PivotIndex = nullptr;
			FRDGBufferUAVRef RDGUAVB_PivotIndex = nullptr;
			ConvertToUAVRWBuffer(GraphBuilder, TmpRDGB_PivotIndex, RDGUAVB_PivotIndex, RootCenter4f, EPixelFormat::PF_A32B32G32R32F, TEXT("UAV_PivotIndex"));

			FRDGBufferRef TmpRDGB_DirExtent = nullptr;
			FRDGBufferUAVRef RDGUAVB_DirExtent = nullptr;
			ConvertToUAVRWBuffer(GraphBuilder, TmpRDGB_DirExtent, RDGUAVB_DirExtent, RootNormal4f, EPixelFormat::PF_A32B32G32R32F, TEXT("UAV_DirExtent"));

			
			FRDGTextureRef RDG_PivotIndex = RegisterExternalTexture(GraphBuilder, R_PivotIndex->GetRenderTargetTexture(), TEXT("R_PivotIndex"));
			FRDGTextureRef RDG_DirExtent = RegisterExternalTexture(GraphBuilder, R_DirExtent->GetRenderTargetTexture(), TEXT("R_DirExtent"));
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("R_DebugView"));

			
			PassParameters->T_PivotIndex = RDG_PivotIndex;
			PassParameters->T_DirExtent = RDG_DirExtent;
			PassParameters->T_DebugView = RDG_DirExtent;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RWB_PivotIndex = RDGUAVB_PivotIndex;
			PassParameters->RWB_DirExtent = RDGUAVB_DirExtent;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("CalVelocityHeight"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_TreeWind, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_TreeWind, *PassParameters, GroupCount);
				});
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_PivotIndex, RDG_PivotIndex, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DirExtent, RDG_DirExtent, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
		}
		GraphBuilder.Execute();
	});
	UCSMesh::CountedBlockingFlush();
}

TArray<FTransform> UComputeShaderBasicFunction::SampleSpline(UTextureRenderTarget2D* InSampleDistRotate,
                                                             UTextureRenderTarget2D* InSampleGradientHeight,
                                                             UTextureRenderTarget2D* InDebugView,
                                                             TArray<USplineComponent*> InSplineComponents,
                                                             FBoxSphereBounds Bounds,
                                                             int32 TextureSize)
{
	TArray<FTransform> OutTransforms;
	if (InSampleDistRotate == nullptr || InSampleGradientHeight == nullptr || InDebugView == nullptr || InSplineComponents.Num() == 0) return OutTransforms;

	
	int32 SplineMaxElement = 1024;
	int32 NumSpline = InSplineComponents.Num();
	// int32 MaxNumPointPerCell = 128;
	int32 BufferNumElement = SplineMaxElement * NumSpline;
	int32 ElementSize = sizeof(FLinearColor);
	
	
	TArray<TArray<FTransform>> ResampleTransform;
	for (USplineComponent* SplineComponent : InSplineComponents)
	{

		float SplineLength = SplineComponent->GetSplineLength();
		TArray<FTransform> Frames;
		TArray<double> FrameTimes;
		FGeometryScriptSplineSamplingOptions SamplingOptions;
		SamplingOptions.NumSamples = SplineMaxElement;
		if (!UGeometryScriptLibrary_PolyPathFunctions::SampleSplineToTransforms(SplineComponent, Frames, FrameTimes, SamplingOptions, FTransform::Identity)) continue;
		ResampleTransform.Add(Frames);
		
	}

	FVector BoundMin = Bounds.Origin - Bounds.BoxExtent;
	FVector BoundMax = Bounds.Origin + Bounds.BoxExtent;
	FVector BoundSize = BoundMax - BoundMin;


	TArray<int32> SplinePointCount;
	TArray<FLinearColor> SplinePoints;
	SplinePoints.AddZeroed(BufferNumElement);
	SplinePointCount.AddZeroed(NumSpline);
	OutTransforms.Reserve(NumSpline * SplineMaxElement);
	for (int32 i = 0; i < NumSpline; i++)
	{
		FTransform SplineTransform = InSplineComponents[i]->GetComponentTransform();
		SplinePointCount[i] = ResampleTransform[i].Num();
		for (int32 j = 0; j < ResampleTransform[i].Num(); j++)
		{
			FTransform Transform = ResampleTransform[i][j] * SplineTransform;
			float Rotate = Transform.GetRotation().Rotator().Yaw;
			FVector Location = Transform.GetLocation();
			FVector LocationUVW = (Location - BoundMin) / BoundSize;
			FLinearColor Color = FLinearColor(Location.X, Location.Y, Location.Z, Rotate);
			SplinePoints[i * SplineMaxElement + j] = Color;
			OutTransforms.Add(Transform);
		}
	}
	{
		SCOPE_CYCLE_COUNTER(STAT_CSTest_Execute);
		InSampleDistRotate->ResizeTarget(TextureSize, TextureSize);
		InSampleGradientHeight->ResizeTarget(TextureSize, TextureSize);
		InDebugView->ResizeTarget(TextureSize, TextureSize);
		
		FTextureRenderTargetResource* SampleDistRotate = InSampleDistRotate->GameThread_GetRenderTargetResource();
		FTextureRenderTargetResource* SampleGradientHeight = InSampleGradientHeight->GameThread_GetRenderTargetResource();
		FTextureRenderTargetResource* DebugView = InDebugView->GameThread_GetRenderTargetResource();
		ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
		[=](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			{
				
				FIntVector GroupSize = FIntVector(SampleDistRotate->GetSizeXY().X, SampleDistRotate->GetSizeXY().Y, 1);
				FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(GroupSize, 32);
				// GroupCount = FIntVector(1, 1, 1);
				FIntPoint TextureSizeXY = SampleDistRotate->GetSizeXY();
				
				TShaderMapRef<FSampleSpline> ComputeShader = FSampleSpline::CreateTempShaderPermutation(FSampleSpline::ESampleStep::SS_SampleSpline);
				FSampleSpline::FParameters* PassParameters = GraphBuilder.AllocParameters<FSampleSpline::FParameters>();
				
				FRDGTextureRef TmpRDG_DirMinDistRotate = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("DirMinDistRotate_RWTexture")); 
				FRDGTextureUAVRef RDGUAV_DirMinDistRotate = GraphBuilder.CreateUAV(TmpRDG_DirMinDistRotate);
				FRDGTextureRef TmpRDG_GradientHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("GradientHeight_RWTexture")); 
				FRDGTextureUAVRef RDGUAV_GradientHeight = GraphBuilder.CreateUAV(TmpRDG_GradientHeight);
				FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("DebugView_RWTexture")); 
				FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
				
				FRDGBufferRef Tmp_SplineDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(ElementSize, BufferNumElement), TEXT("SplineDataBuffer"));
				GraphBuilder.QueueBufferUpload(Tmp_SplineDataBuffer, SplinePoints.GetData(), BufferNumElement * ElementSize);
				FRDGBufferUAVRef Tmp_SplineDataBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_SplineDataBuffer, EPixelFormat::PF_A32B32G32R32F));
				
				FRDGBufferRef Tmp_SplinePointCountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(int32), NumSpline), TEXT("SplinePointCountBuffer"));
				GraphBuilder.QueueBufferUpload(Tmp_SplinePointCountBuffer, SplinePointCount.GetData(), SplinePointCount.Num() * sizeof(int32));
				FRDGBufferUAVRef Tmp_SplinePointCountBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_SplinePointCountBuffer, EPixelFormat::PF_R32_UINT));
				
				
				PassParameters->RW_SampleResult_DirMinDistRotate = RDGUAV_DirMinDistRotate;
				PassParameters->RW_SampleResult_GradientHeight = RDGUAV_GradientHeight;
				PassParameters->RW_DebugView = RDGUAV_DebugView;
				PassParameters->RW_PointsToSampleBuffer = Tmp_SplineDataBufferUAV;
				PassParameters->RW_SplinePointCount = Tmp_SplinePointCountBufferUAV;
				PassParameters->NumSpline = NumSpline;
				PassParameters->BoundsMin = FVector3f(BoundMin.X, BoundMin.Y, BoundMin.Z);
				PassParameters->BoundsSize = FVector3f(BoundSize.X, BoundSize.Y, BoundSize.Z);
				
				PassParameters->Sampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("SampleSpline"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
					{
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
					});

				FRDGTextureRef RDG_DirMinDistRotate =  RegisterExternalTexture(GraphBuilder, SampleDistRotate->GetRenderTargetTexture(), TEXT("InTexture_RT"));
				FRDGTextureRef RDG_GradientHeight =  RegisterExternalTexture(GraphBuilder, SampleGradientHeight->GetRenderTargetTexture(), TEXT("InTexture_RT"));
				FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
				AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, DebugViewTexture, FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpRDG_DirMinDistRotate, RDG_DirMinDistRotate, FRHICopyTextureInfo());
				AddCopyTexturePass(GraphBuilder, TmpRDG_GradientHeight, RDG_GradientHeight, FRHICopyTextureInfo());
			}
			GraphBuilder.Execute();
		});
		UCSMesh::CountedBlockingFlush();
	}
	
	return OutTransforms;
}

void UComputeShaderBasicFunction::GenerateMapCliff(TSubclassOf<ACSPlaneRangeGenerator> ManagerClass, TArray<TSubclassOf<ACSCliffGenerateCapture>> GeneratorClass, ACSPlaneRangeGenerator*& OutGeneratorManager)
{
	ALandscape* Landscape = nullptr;
	for (TActorIterator<ALandscape> It(GWorld, ALandscape::StaticClass()); It; ++It)
	{
		Landscape = *It;
		break;
	}
	if (Landscape == nullptr) return;
	FVector Origin = FVector::ZeroVector;
	FVector Extent = FVector::ZeroVector;
	Landscape->GetActorBounds(false, Origin, Extent);

	

	for (TActorIterator<AActor> It(GWorld, AActor::StaticClass()); It; ++It)
	{
		if (It->Tags.Contains("Generate")) It->Destroy();
		
	}

	
	FActorSpawnParameters SpawnParameters;
	ACSPlaneRangeGenerator* GeneratorManager = GWorld->SpawnActor<ACSPlaneRangeGenerator>(ManagerClass, SpawnParameters);
	GeneratorManager->Tags = {"Generate"};

	GeneratorManager->SetActorLocation(Origin);
	GeneratorManager->SetActorScale3D(FVector(Extent.X / 50, Extent.Y / 50, 100));

	TArray<ACSGenerateCaptureScene*> Generators;
	Generators.Reserve(GeneratorClass.Num());
	for (int32 i = 0; i < GeneratorClass.Num(); i++)
	{
		ACSGenerateCaptureScene* Generator = GWorld->SpawnActor<ACSCliffGenerateCapture>(GeneratorClass[i], SpawnParameters);
		Generator->Construction();
		Generator->Tags = {"Generate"};
		Generators.Add(Generator);
	}

	
	GeneratorManager->CaptureSize = Generators[0]->CaptureSize;
	GeneratorManager->GenerateTransformsCount();

	if (GeneratorManager->StoreCaptureTransforms.Num() == 0) return;

	GeneratorManager->MultGenerateCount = 0;
	GeneratorManager->GeneratorCount = 0;
	GeneratorManager->CaptureSceneGenerators = Generators;
	GeneratorManager->DoGenerate = true;
	OutGeneratorManager = GeneratorManager;
	

}


void UComputeShaderBasicFunction::RDG_SmoothSpline(
	FRDGBuilder& GraphBuilder,
	const TArray<FVector4f>& ControlPoints,
	int32 NumSamples,
	FRDGBufferRef& OutPositions,
	FRDGBufferRef& OutTangents)
{
	const int32 NumCtrl = ControlPoints.Num();
	if (NumCtrl < 2 || NumSamples < 1) return;

	FRDGBufferRef CtrlBuffer = CreateUploadBuffer(
		GraphBuilder, TEXT("SmoothSpline.ControlPoints"),
		sizeof(FVector4f), NumCtrl,
		ControlPoints.GetData(), NumCtrl * sizeof(FVector4f));
	FRDGBufferSRVRef CtrlSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CtrlBuffer, PF_A32B32G32R32F));

	OutPositions = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NumSamples), TEXT("SmoothSpline.OutPositions"));
	OutTangents = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), NumSamples), TEXT("SmoothSpline.OutTangents"));
	FRDGBufferUAVRef PosUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutPositions, PF_A32B32G32R32F));
	FRDGBufferUAVRef TanUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutTangents, PF_A32B32G32R32F));

	TShaderMapRef<FSmoothSplineEval> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FSmoothSplineEval::FParameters* PassParameters = GraphBuilder.AllocParameters<FSmoothSplineEval::FParameters>();
	PassParameters->ControlPoints = CtrlSRV;
	PassParameters->RW_OutPositions = PosUAV;
	PassParameters->RW_OutTangents = TanUAV;
	PassParameters->NumControlPoints = NumCtrl;
	PassParameters->NumSamples = NumSamples;

	const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(NumSamples, 1, 1), 64);
	FComputeShaderUtils::AddPass(
		GraphBuilder, RDG_EVENT_NAME("SmoothSplineEval"),
		ComputeShader, PassParameters, GroupCount);
}

void UComputeShaderBasicFunction::DrawSmoothSplinePoints(
	UObject* WorldContextObject,
	USplineComponent* InSpline,
	int32 NumSamples,
	float Duration,
	float PointSize,
	float ArrowLength)
{
	if (InSpline == nullptr) return;
	UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	if (World == nullptr) World = GWorld;
	if (World == nullptr) return;

	NumSamples = FMath::Max(NumSamples, 2);

	const int32 NumCtrl = InSpline->GetNumberOfSplinePoints();
	if (NumCtrl < 2) return;

	TArray<FVector4f> ControlPoints;
	ControlPoints.Reserve(NumCtrl);
	for (int32 i = 0; i < NumCtrl; ++i)
	{
		const FVector P = InSpline->GetLocationAtSplinePoint(i, ESplineCoordinateSpace::World);
		ControlPoints.Add(FVector4f((float)P.X, (float)P.Y, (float)P.Z, 0.f));
	}

	TArray<FVector4f> OutPositions; OutPositions.SetNumZeroed(NumSamples);
	TArray<FVector4f> OutTangents;  OutTangents.SetNumZeroed(NumSamples);

	ENQUEUE_RENDER_COMMAND(SmoothSplineReadback)(
		[ControlPoints, NumSamples,
		 PosDst = OutPositions.GetData(), TanDst = OutTangents.GetData()]
		(FRHICommandListImmediate& RHICmdList)
	{
		FRHIGPUBufferReadback PosReadback(TEXT("SmoothSpline.PosReadback"));
		FRHIGPUBufferReadback TanReadback(TEXT("SmoothSpline.TanReadback"));
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef OutPos = nullptr;
			FRDGBufferRef OutTan = nullptr;
			UComputeShaderBasicFunction::RDG_SmoothSpline(GraphBuilder, ControlPoints, NumSamples, OutPos, OutTan);
			if (OutPos && OutTan)
			{
				AddEnqueueCopyPass(GraphBuilder, &PosReadback, OutPos, NumSamples * sizeof(FVector4f));
				AddEnqueueCopyPass(GraphBuilder, &TanReadback, OutTan, NumSamples * sizeof(FVector4f));
			}
			GraphBuilder.Execute();
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();

		auto CopyBack = [](FRHIGPUBufferReadback& Readback, FVector4f* Dst, int32 Count)
		{
			if (Readback.IsReady())
			{
				if (const FVector4f* Src = static_cast<const FVector4f*>(Readback.Lock(Count * sizeof(FVector4f))))
				{
					FMemory::Memcpy(Dst, Src, Count * sizeof(FVector4f));
					Readback.Unlock();
				}
			}
		};
		CopyBack(PosReadback, PosDst, NumSamples);
		CopyBack(TanReadback, TanDst, NumSamples);
	});

	UCSMesh::CountedBlockingFlush();

	const bool bPersistent = (Duration <= 0.f);
	for (int32 i = 0; i < NumSamples; ++i)
	{
		if (OutPositions[i].W <= 0.f) continue;
		const FVector Pos((double)OutPositions[i].X, (double)OutPositions[i].Y, (double)OutPositions[i].Z);
		const FVector Tan((double)OutTangents[i].X, (double)OutTangents[i].Y, (double)OutTangents[i].Z);

		DrawDebugPoint(World, Pos, PointSize, FColor::Green, bPersistent, Duration);
		if (Tan.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			DrawDebugDirectionalArrow(World, Pos, Pos + Tan.GetSafeNormal() * ArrowLength,
				ArrowLength * 0.3f, FColor::Red, bPersistent, Duration, 0, 1.5f);
		}
	}
}

void UComputeShaderBasicFunction::DebugDrawDistanceFieldInBox(
	UObject* WorldContextObject,
	FVector BoxCenter,
	FVector BoxExtent,
	int32 Resolution,
	float DistanceThreshold,
	float PointSize,
	float Duration,
	bool bColorByDistance)
{
	if (!WorldContextObject) return;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return;

	const int32 N = FMath::Max(Resolution, 1);
	const int32 NumPoints = N * N * N;

	FGDFPointSampleRequest Req;
	Req.WorldPositions.Reserve(NumPoints);

	const FVector BoxMin = BoxCenter - BoxExtent;
	const FVector BoxSize = BoxExtent * 2.0f;
	const float Denom = (N > 1) ? float(N - 1) : 1.0f;

	for (int32 z = 0; z < N; ++z)
	{
		for (int32 y = 0; y < N; ++y)
		{
			for (int32 x = 0; x < N; ++x)
			{
				const FVector T(
					(N > 1) ? (float(x) / Denom) : 0.5f,
					(N > 1) ? (float(y) / Denom) : 0.5f,
					(N > 1) ? (float(z) / Denom) : 0.5f);
				Req.WorldPositions.Add(BoxMin + BoxSize * T);
			}
		}
	}

	// 结果就绪后在游戏线程画点阵：World 用弱引用，回调时世界可能已销毁。
	TWeakObjectPtr<UWorld> WeakWorld(World);
	const bool bPersistent = (Duration <= 0.0f);

	Req.OnComplete =
		[WeakWorld, Positions = Req.WorldPositions, DistanceThreshold, PointSize, Duration, bPersistent, bColorByDistance, BoxCenter, BoxExtent, N]
		(const TArray<float>& Distances, const TArray<FVector>& /*Gradients*/)
	{
		UWorld* W = WeakWorld.Get();
		if (!W || Distances.Num() != Positions.Num()) return;

		const float SafeThreshold = FMath::Max(DistanceThreshold, KINDA_SMALL_NUMBER);
		int32 DrawnCount = 0;

		FlushPersistentDebugLines(W);

		for (int32 i = 0; i < Positions.Num(); ++i)
		{
			const float Dist = Distances[i];
			if (FMath::Abs(Dist) >= DistanceThreshold) continue;

			FColor PointColor = FColor::Green;
			if (bColorByDistance)
			{
				const float Alpha = FMath::Clamp(FMath::Abs(Dist) / SafeThreshold, 0.0f, 1.0f);
				PointColor = FLinearColor::LerpUsingHSV(FLinearColor::Red, FLinearColor::Green, Alpha).ToFColor(true);
			}

			DrawDebugPoint(W, Positions[i], PointSize, PointColor, bPersistent, Duration);
			++DrawnCount;
		}

		DrawDebugBox(W, BoxCenter, BoxExtent, FColor::White, bPersistent, Duration);

		UE_LOG(LogTemp, Log,
			TEXT("DebugDrawDistanceFieldInBox: sampled %d points (N=%d), drew %d voxels with abs(distance) < %.2f."),
			Positions.Num(), N, DrawnCount, DistanceThreshold);
	};

	FGDFSampleService::Get().EnqueuePointSample(MoveTemp(Req));
}

void UComputeShaderBasicFunction::DebugDrawVoxelCavitySpansInBox(
	UObject* WorldContextObject,
	FVector BoxCenter,
	FVector BoxExtent,
	FRotator BoxRotation,
	int32 GridX,
	int32 GridZ,
	float OccupancyThreshold,
	float Duration,
	float LineThickness)
{
	if (!WorldContextObject) return;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!World) return;

	FGDFCavitySpanRequest Req;
	Req.BoxTransform = FTransform(BoxRotation.Quaternion(), BoxCenter, FVector::OneVector);
	Req.BoxSize = BoxExtent * 2.0;
	Req.GridSize = FIntVector(FMath::Max(GridX, 1), FMath::Max(GridX, 1), FMath::Max(GridZ, 1));
	Req.OccupancyThreshold = OccupancyThreshold;

	// 结果就绪后在游戏线程逐列画竖直线段，每段贯穿一个空腔区间。
	TWeakObjectPtr<UWorld> WeakWorld(World);
	const bool bPersistent = (Duration <= 0.0f);
	const float Thickness = FMath::Max(LineThickness, 0.5f);
	const FColor LineColor = FColor::Cyan;

	Req.OnComplete =
		[WeakWorld, Duration, bPersistent, Thickness, LineColor](const FGDFCavitySpanRequest::FResult& Result)
	{
		UWorld* W = WeakWorld.Get();
		if (!W) return;

		const FTransform& BoxTransform = Result.BoxTransform;
		const FIntVector GridSize = Result.GridSize;
		const FVector CellSize = Result.CellSizeWorld;
		const FVector BoxMinLocal = Result.BoxMinLocal;
		const uint32 SpanCapacity = (uint32)Result.Spans.Num();

		FlushPersistentDebugLines(W);

		int32 DrawnSpans = 0;
		for (int32 Y = 0; Y < GridSize.Y; ++Y)
		{
			for (int32 X = 0; X < GridSize.X; ++X)
			{
				const uint32 Col = (uint32)Y * (uint32)GridSize.X + (uint32)X;
				if (Col >= (uint32)Result.ColumnSpanCount.Num()) continue;
				const uint32 Count = Result.ColumnSpanCount[Col];
				if (Count == 0) continue;
				const uint32 Start = Result.ColumnSpanStart[Col];

				const double LocalX = BoxMinLocal.X + (X + 0.5) * CellSize.X;
				const double LocalY = BoxMinLocal.Y + (Y + 0.5) * CellSize.Y;

				for (uint32 r = 0; r < Count; ++r)
				{
					const uint32 Idx = Start + r;
					if (Idx >= SpanCapacity) break;
					const uint32 Packed = Result.Spans[Idx];
					const uint32 ZStart = Packed & 0xFFFFu;
					const uint32 ZEnd = (Packed >> 16u) & 0xFFFFu;

					const double BottomLocalZ = BoxMinLocal.Z + (ZStart + 0.5) * CellSize.Z;
					const double TopLocalZ = BoxMinLocal.Z + (ZEnd + 0.5) * CellSize.Z;

					const FVector LineStart = BoxTransform.TransformPosition(FVector(LocalX, LocalY, BottomLocalZ));
					const FVector LineEnd = BoxTransform.TransformPosition(FVector(LocalX, LocalY, TopLocalZ));

					DrawDebugLine(W, LineStart, LineEnd, LineColor, bPersistent, Duration, 0, Thickness);
					++DrawnSpans;
				}
			}
		}

		const FVector BoxExtentLocal(
			0.5 * CellSize.X * GridSize.X,
			0.5 * CellSize.Y * GridSize.Y,
			0.5 * CellSize.Z * GridSize.Z);
		DrawDebugBox(W, BoxTransform.GetLocation(), BoxExtentLocal,
			BoxTransform.GetRotation(), FColor::White, bPersistent, Duration, 0, Thickness);

		UE_LOG(LogTemp, Log, TEXT("DebugDrawVoxelCavitySpansInBox: drew %d cavity spans (Grid=%dx%dx%d, Total=%u, Duration=%.1fs)."),
			DrawnSpans, GridSize.X, GridSize.Y, GridSize.Z, Result.TotalSpanCount, Duration);
	};

	FGDFSampleService::Get().EnqueueCavitySpan(MoveTemp(Req));
}

void UComputeShaderBasicFunction::CopyTexture(UTextureRenderTarget2D* InOrig, UTextureRenderTarget2D* InCopy)
{
	if (InCopy == nullptr || InOrig == nullptr) return;
	if (InCopy->SizeX != InOrig->SizeX || InCopy->SizeY != InOrig->SizeY) return;
	FTextureRenderTargetResource* CopyData = InCopy->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* OrigData = InOrig->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			FRDGTextureRef RDG_OrigData = RegisterExternalTexture(GraphBuilder, OrigData->GetRenderTargetTexture(), TEXT("Orig_RT"));
			FRDGTextureRef RDG_CopyData = RegisterExternalTexture(GraphBuilder, CopyData->GetRenderTargetTexture(), TEXT("Copy_RT"));
			// DrawCopyTexture(GraphBuilder, RDG_OrigData, RDG_CopyData);
			AddCopyTexturePass(GraphBuilder, RDG_OrigData, RDG_CopyData, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
	UCSMesh::CountedBlockingFlush();
}

void UComputeShaderBasicFunction::DrawCopyTexture(FRDGBuilder& GraphBuilder, FRDGTextureUAVRef RDGUAV_CopySource, FRDGTextureRef& RDG_CopyTarget)
{
	FDrawCopyTexturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FDrawCopyTexturePS::FParameters>();
	
	PassParameters->RenderTargets[0] = FRenderTargetBinding(RDG_CopyTarget, ERenderTargetLoadAction::EClear, 0);
	PassParameters->RW_CopySource = RDGUAV_CopySource;
	PassParameters->SourceSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	FIntRect ViewRect = FIntRect(FIntPoint(0, 0), FIntPoint(RDG_CopyTarget->Desc.Extent.X, RDG_CopyTarget->Desc.Extent.Y));
	const auto GlobalShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
	TShaderMapRef<FDrawCopyTexturePS> PixelShader = FDrawCopyTexturePS::CreatePermutation(FDrawCopyTexturePS::EDrawCopy::DC_CopyRWTexture);
	
	FPixelShaderUtils::AddFullscreenPass(GraphBuilder, GlobalShaderMap, RDG_EVENT_NAME("DrawCopyTexture"), PixelShader, PassParameters, ViewRect);
	
}

void UComputeShaderBasicFunction::BuildTextureArray(FRDGBuilder& GraphBuilder, int32& Index, FRDGTextureRef& RDG_CopySource,
	FRDGTextureUAVRef& RDG_CopyTarget, FIntVector& GroupCount)
{
	
	TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_BuildTextureArray);
	
	FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
	
	PassParameters->RW_TextureArray0 = RDG_CopyTarget;
	PassParameters->T_ProcssTexture0 = RDG_CopySource;
	PassParameters->InputData0 = Index;
	
	GraphBuilder.AddPass(
	RDG_EVENT_NAME("BuildTextureArray"),
	PassParameters,
	ERDGPassFlags::AsyncCompute,
	[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
	{
		FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
	});
}

#if WITH_EDITOR


void UComputeShaderBasicFunction::Test(UTexture2DArray* InArray, UTexture2D* InTexture, UTextureRenderTarget2D* InDebugView)
{
	UTexture2DArray* TestArray = NewObject<UTexture2DArray>();
	InArray->SourceTextures.Empty();
	TestArray->SourceTextures.Add(InTexture);
	TestArray->UpdateSourceFromSourceTextures();
	UGameViewportClient* ViewportClient = GWorld->GetGameViewport();
	// ViewportClient->GetMousePosition();
	ViewportClient->Viewport ;
	FSceneViewport* SceneViewport = ViewportClient->GetGameViewport();
	// FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Game))
	// 							.SetTime(FGameTime()));
	// FSceneView* View = ViewportClient->CalcSceneView()
	APlayerController* PC = GWorld->GetFirstPlayerController();
	FVector CameraLocation;
	FRotator CameraRotation;
	PC->PlayerCameraManager->GetCameraViewPoint(CameraLocation, CameraRotation);
    
	// 设置视图参数
	// FSceneViewInitOptions ViewInitOptions;
	// ViewInitOptions.ViewFamily = &ViewFamily;
	// ViewInitOptions.SetViewRectangle(ViewRect);
	// ViewInitOptions.ViewOrigin = -Origin;
	// ViewInitOptions.ViewRotationMatrix = ViewRotationMatrix;
	// ViewInitOptions.ProjectionMatrix = ProjectionMatrix;
	// ViewInitOptions.BackgroundColor = FLinearColor::Black;
	//
	// FSceneView* NewView = new FSceneView(ViewInitOptions);
	



	
	// SceneViewport->view
	// this->EditorViews
	FTextureRenderTargetResource* DebugView = InDebugView->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			TShaderMapRef<FGlobalDistanceFieldForCS> ComputeShader = FGlobalDistanceFieldForCS::CreateTempShaderPermutation(FGlobalDistanceFieldForCS::ESDFShader::GDF_DistanceToNearestSurface);
			FGlobalDistanceFieldForCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FGlobalDistanceFieldForCS::FParameters>();
			
			FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(TestArray->GetSizeX(), TestArray->GetSizeY(), 1), 32);
			
			FRDGTextureRef TmpTexture_DebugView = CSHelper::ConvertToUVATexture(DebugView, GraphBuilder);
			FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			FRDGTextureRef TextureArray = RegisterExternalTexture(GraphBuilder, TestArray->GetResource()->GetTextureRHI(), TEXT("Input_TA"));

			PassParameters->RW_DebugView = GraphBuilder.CreateUAV(TmpTexture_DebugView);
			
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			

			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteExampleComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{

				});
			
			AddCopyTexturePass(GraphBuilder, TmpTexture_DebugView, DebugViewTexture, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

	});
	
}



void UComputeShaderBasicFunction::UpdateTextureArray(UTexture2DArray* Texture2DArray)
{
	if (!Texture2DArray->CheckArrayTexturesCompatibility()) 
	{
		return;
	}
	
	if (Texture2DArray->SourceTextures.Num() > 0)
	{
		Texture2DArray->Modify();
	
		int32 SizeX = Texture2DArray->SourceTextures[0]->Source.GetSizeX();
		int32 SizeY = Texture2DArray->SourceTextures[0]->Source.GetSizeY();
		ETextureSourceFormat Format = Texture2DArray->SourceTextures[0]->Source.GetFormat();
		bool bSRGB = Texture2DArray->SourceTextures[0]->Source.GetGammaSpace(0) == EGammaSpace::sRGB;
	
		bool bMismatchedSize = false;
		bool bMismatchedAspectRatio = false;
		bool bMismatchedFormats = false;
		bool bMismatchedGammaSpace = false;
	
		for (int32 SourceTextureIndex = 1; SourceTextureIndex < Texture2DArray->SourceTextures.Num(); ++SourceTextureIndex)
		{
			if (Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeX() != SizeX || Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeY() != SizeY)
			{
				bMismatchedSize = true;
				if (Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeX() * SizeY != Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeY() * SizeX)
				{
					bMismatchedAspectRatio = true;
				}
				// Dimensions of the texture array source are set to the maximum SizeX and SizeY among the array element's sources in order to minimize quality loss.
				SizeX = FMath::Max(SizeX, Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeX());
				SizeY = FMath::Max(SizeY, Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetSizeY());
			}
	
			if ((Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetGammaSpace(0) == EGammaSpace::sRGB) != bSRGB)
			{
				bMismatchedGammaSpace = true;
				bSRGB = false;
				Format = TSF_RGBA32F;
			}
	
			if (Format != Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetFormat())
			{
				bMismatchedFormats = true;
				Format = FImageCoreUtils::GetCommonSourceFormat(Format, Texture2DArray->SourceTextures[SourceTextureIndex]->Source.GetFormat());
			}
		}
	
		if (bMismatchedSize)
		{
			UE_LOG(LogTexture, Log, TEXT("Mismatched sizes of the source textures, resizing all the array elements to %dx%d%s ..."),
				SizeX, SizeY, bMismatchedAspectRatio ? TEXT(" (this will also affect aspect ratio of some source textures)") : TEXT(""));
		}
	
		if (bMismatchedGammaSpace)
		{
			UE_LOG(LogTexture, Log, TEXT("Mismatched source gamma spaces, converting all to RGBA32F/Linear ..."));
		}
		else if (bMismatchedFormats)
		{
			UE_LOG(LogTexture, Log, TEXT("Mismatched source pixel formats, converting all to %s/%s ..."),
				ERawImageFormat::GetName(FImageCoreUtils::ConvertToRawImageFormat(Format)), bSRGB ? TEXT("sRGB") : TEXT("Linear"));
		}
	
		int32 FormatDataSize = FTextureSource::GetBytesPerPixel(Format);
		uint32 ArraySize = Texture2DArray->SourceTextures.Num();
	
		// This should be false when texture is updated to avoid overriding user settings.
		bool bCreatingNewTexture = true;
		if (bCreatingNewTexture)
		{
			Texture2DArray->CompressionSettings = Texture2DArray->SourceTextures[0]->CompressionSettings;
			Texture2DArray->MipGenSettings = TMGS_NoMipmaps;
			Texture2DArray->PowerOfTwoMode = ETexturePowerOfTwoSetting::None;
			Texture2DArray->LODGroup = Texture2DArray->SourceTextures[0]->LODGroup;
			Texture2DArray->NeverStream = true;
		}
	
		Texture2DArray->SRGB = bSRGB;
	
		// Create the source texture for this Texture2DArray.
		// Currently only single-mip Texture2DArray's are supported, therefore only the first mip is copied from each element source.
		Texture2DArray->Source.Init(SizeX, SizeY, ArraySize, 1, Format);
	
		uint8* DestMipData = Texture2DArray->Source.LockMip(0);
		int64 MipSizeBytes = Texture2DArray->Source.CalcMipSize(0) / ArraySize;
	
		for (int32 SourceTexIndex = 0; SourceTexIndex < Texture2DArray->SourceTextures.Num(); ++SourceTexIndex)
		{
			FTextureSource& TextureSource = Texture2DArray->SourceTextures[SourceTexIndex]->Source;
			void* DestSliceData = DestMipData + MipSizeBytes * SourceTexIndex;
			if (TextureSource.GetSizeX() == SizeX && TextureSource.GetSizeY() == SizeY && TextureSource.GetFormat() == Format)
			{
				const uint8* SourceData = TextureSource.LockMipReadOnly(0);
				check(TextureSource.CalcMipSize(0) == MipSizeBytes);
				FMemory::Memcpy(DestSliceData, SourceData, MipSizeBytes);
				TextureSource.UnlockMip(0);
			}
			else
			{
				FImage SourceImage;
				Texture2DArray->SourceTextures[SourceTexIndex]->Source.GetMipImage(SourceImage, 0);
	
				if (TextureSource.GetFormat() != Format)
				{
					// note: there is no need to check if the gamma space is different, because in such case we would fallback to TSF_RGBA32F, which is always linear
					FImage ConvertedSourceImage;
					SourceImage.CopyTo(ConvertedSourceImage, FImageCoreUtils::ConvertToRawImageFormat(Format), bSRGB ? EGammaSpace::sRGB : EGammaSpace::Linear);
					ConvertedSourceImage.Swap(SourceImage);
				}
	
				if (TextureSource.GetSizeX() != SizeX || TextureSource.GetSizeY() != SizeY)
				{
					FImage ResizedSourceImage;
					SourceImage.ResizeTo(ResizedSourceImage, SizeX, SizeY, SourceImage.Format, SourceImage.GammaSpace);
					ResizedSourceImage.Swap(SourceImage);
				}
				
				check(SourceImage.RawData.Num() == MipSizeBytes);
				FMemory::Memcpy(DestSliceData, SourceImage.RawData.GetData(), MipSizeBytes);
			}
		}
	
		Texture2DArray->Source.UnlockMip(0);
	
		Texture2DArray->ValidateSettingsAfterImportOrEdit();
		Texture2DArray->SetLightingGuid();
		Texture2DArray->UpdateResource();
	}
}

#endif

void UComputeShaderBasicFunction::SyncRenderThread()
{
	UCSMesh::CountedBlockingFlush();
}

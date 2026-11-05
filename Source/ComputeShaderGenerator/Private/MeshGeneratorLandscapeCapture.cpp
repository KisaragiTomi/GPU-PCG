// -----------------------------------------------------------------------------
// AComputeShaderMeshGenerator - Landscape 捕获 TU
//
// 从 ComputeShaderMeshGenerator.cpp 拆出的 landscape 高度图/深度/法线/三角捕获实现：
// ConvertLandscapeHeightmapToDepthRDG / CaptureLandscapeHeightmap(+ToDepth/GPU) /
// ConvertLandscapeHeightmapToNormalHeightRDG / RenderLandscapeToNormalHeightRT /
// CaptureLandscapeTrianglesGPU，以及仅本 TU 使用的 3 个 FLandscape* shader。
// 通用的 mesh->heightmap 光栅化（IndexedMeshToHeightmapRDG）
// 仍在 ComputeShaderMeshGenerator.cpp（被 ShallowWater / LandscapeRoad 复用）。
// -----------------------------------------------------------------------------

#include "ComputeShaderMeshGenerator.h"
#include "MeshGeneratorInternal.h"
#include "CSMesh.h"   // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口

#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GlobalShader.h"
#include "Landscape.h"
#include "LandscapeComponent.h"
#include "LandscapeProxy.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

class FLandscapeG16ToDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeG16ToDepthCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeG16ToDepthCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(float, LHM_CameraHeight)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeG16ToDepthCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeG16ToDepthCS", SF_Compute);

class FLandscapeG16ToNormalHeightCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeG16ToNormalHeightCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeG16ToNormalHeightCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RW_HeightmapFloat)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
		SHADER_PARAMETER(FVector2f, LHM_TexelWorldSize)
		SHADER_PARAMETER(uint32, LHM_MergeByMaxZ)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeG16ToNormalHeightCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeG16ToNormalHeightCS", SF_Compute);

class FLandscapeHeightmapToTrianglesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLandscapeHeightmapToTrianglesCS);
	SHADER_USE_PARAMETER_STRUCT(FLandscapeHeightmapToTrianglesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float4>, T_LandscapeRGBA)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FVector4f>, RW_TriangleVerts)
		SHADER_PARAMETER(float, LHM_LandscapeScaleZ)
		SHADER_PARAMETER(float, LHM_LandscapeOriginZ)
		SHADER_PARAMETER(FIntPoint, LHM_TextureSize)
		SHADER_PARAMETER(FVector2f, LHM_WorldOriginXY)
		SHADER_PARAMETER(FVector2f, LHM_TexelWorldSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FLandscapeHeightmapToTrianglesCS, "/Plugin/PCGPlugins/Shaders/Private/StaticMeshPointSampler.usf", "LandscapeHeightmapToTrianglesCS", SF_Compute);

namespace
{
// 收集世界中所有有效 ALandscape（各捕获入口共用；原为 3 份手写循环）。
TArray<ALandscape*> CSMeshGenLC_GatherLandscapes(UWorld* World)
{
	TArray<ALandscape*> Landscapes;
	for (TActorIterator<ALandscape> It(World); It; ++It)
		if (IsValid(*It)) Landscapes.Add(*It);
	return Landscapes;
}

// ALandscape::RenderHeightmap 的临时 RGBA8 目标（G16 高度打包在 RG 通道，ClearColor 半高；
// 原为 4 份逐字相同的手写设置块）。
UTextureRenderTarget2D* CSMeshGenLC_CreateTempHeightmapRT(int32 SizeX, int32 SizeY)
{
	UTextureRenderTarget2D* TempRT = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	TempRT->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA8;
	TempRT->bCanCreateUAV = false;
	TempRT->ClearColor = FLinearColor(0.5f, 0, 0, 0);
	TempRT->InitAutoFormat(SizeX, SizeY);
	TempRT->UpdateResourceImmediate(true);
	return TempRT;
}
} // namespace

void AComputeShaderMeshGenerator::ConvertLandscapeHeightmapToDepthRDG(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef LandscapeG16Texture,
	FRDGTextureRef OutputHeightmap,
	float CameraHeight,
	float LandscapeScaleZ,
	float LandscapeOriginZ)
{
	if (!LandscapeG16Texture || !OutputHeightmap) return;

	FIntPoint TexSize(OutputHeightmap->Desc.Extent.X, OutputHeightmap->Desc.Extent.Y);

	TShaderMapRef<FLandscapeG16ToDepthCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* PassParams = GraphBuilder.AllocParameters<FLandscapeG16ToDepthCS::FParameters>();
	PassParams->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(LandscapeG16Texture));
	PassParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputHeightmap);
	PassParams->LHM_CameraHeight = CameraHeight;
	PassParams->LHM_LandscapeScaleZ = LandscapeScaleZ;
	PassParams->LHM_LandscapeOriginZ = LandscapeOriginZ;
	PassParams->LHM_TextureSize = TexSize;

	FIntVector GroupCount(
		FMath::DivideAndRoundUp(TexSize.X, 8),
		FMath::DivideAndRoundUp(TexSize.Y, 8),
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("LandscapeG16ToDepth"),
		ERDGPassFlags::Compute,
		CS,
		PassParams,
		GroupCount);
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmap(UTextureRenderTarget2D* OutRT, bool bOutputWorldHeight)
{
	const FBox Box = GetGeneratorBoundsWorldBox();
	if (!Box.IsValid) return false;
	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	const float CaptureExtent = FMath::Max(Extent.X, Extent.Y);
	const float CameraHeight = Center.Z + Extent.Z;

	if (OutRT == nullptr)
	{
		UWorld* World = GetWorld();
		if (!World) return false;

		constexpr int32 GridSize = 32;
		const float WorldSize = CaptureExtent * 2.0f;
		const float StepSize = WorldSize / GridSize;

		const float TraceTop    = CameraHeight + 100000.0f;
		const float TraceBottom = Center.Z - Extent.Z - 100000.0f;

		FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(LandscapeHeightDD), false);
		QueryParams.AddIgnoredActor(this);

		int32 HitCount = 0;
		const double TraceStart = FPlatformTime::Seconds();
		for (int32 Y = 0; Y < GridSize; ++Y)
		{
			for (int32 X = 0; X < GridSize; ++X)
			{
				const float WorldX = Center.X - CaptureExtent + (X + 0.5f) * StepSize;
				const float WorldY = Center.Y - CaptureExtent + (Y + 0.5f) * StepSize;

				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit,
					FVector(WorldX, WorldY, TraceTop),
					FVector(WorldX, WorldY, TraceBottom),
					ECC_WorldStatic, QueryParams)
					&& Hit.GetActor() && Hit.GetActor()->IsA<ALandscapeProxy>())
				{
					DrawDebugPoint(World, Hit.ImpactPoint + FVector(0,0,10), 8.0f, FColor::Green, false, 15.0f);
					++HitCount;
				}
			}
		}
		const double TraceMs = (FPlatformTime::Seconds() - TraceStart) * 1000.0;
		UE_LOG(LogTemp, Log, TEXT("[CaptureLandscapeHeightmap] DD mode: %d/%d landscape hits, %.2f ms, Center=(%.0f,%.0f,%.0f) Extent=%.0f"),
			HitCount, GridSize * GridSize, TraceMs, Center.X, Center.Y, Center.Z, CaptureExtent);
		return HitCount > 0;
	}

	const bool bResult = bOutputWorldHeight
		? CaptureLandscapeHeightmapGPU(Center, CaptureExtent, OutRT)
		: CaptureLandscapeHeightmapToDepth(Center, CaptureExtent, CameraHeight, OutRT);

	return bResult;
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmapToDepth(
	FVector WorldCenter,
	float CaptureExtent,
	float CameraHeight,
	UTextureRenderTarget2D* OutDepthRT)
{
	if (!OutDepthRT) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	TArray<ALandscape*> Landscapes = CSMeshGenLC_GatherLandscapes(World);
	if (Landscapes.IsEmpty()) return false;

	const int32 TexSize = OutDepthRT->SizeX;

	// Pre-clear output to very large depth so min-merge works across multiple landscapes.
	// The shader writes min(existing, newDepth), so existing must start high.
	FTextureRenderTargetResource* R_Depth = OutDepthRT->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(ClearDepthToMax)(
	[R_Depth](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		FRDGTextureRef RDG = RegisterExternalTexture(
			GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DepthClear"));
		AddClearRenderTargetPass(GraphBuilder, RDG, FLinearColor(1e10f, 0, 0, 1));
		GraphBuilder.Execute();
	});

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));

	TArray<UTextureRenderTarget2D*> TempRTs;
	bool bAnySuccess = false;

	for (ALandscape* Landscape : Landscapes)
	{
		UTextureRenderTarget2D* TempRT = CSMeshGenLC_CreateTempHeightmapRT(TexSize, TexSize);

		if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
		{
			TempRT->MarkAsGarbage();
			continue;
		}

		const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
		const float LandscapeOriginZ = Landscape->GetActorLocation().Z;

		FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

		ENQUEUE_RENDER_COMMAND(LandscapeRGBAToDepth)(
		[this, R_RGBA, R_Depth, CameraHeight, LandscapeScaleZ, LandscapeOriginZ, TexSize](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
			FRDGTextureRef RDG_Depth = RegisterExternalTexture(GraphBuilder, R_Depth->GetRenderTargetTexture(), TEXT("DepthOutput_RT"));

			ConvertLandscapeHeightmapToDepthRDG(
				GraphBuilder, RDG_RGBA, RDG_Depth,
				CameraHeight, LandscapeScaleZ, LandscapeOriginZ);

			GraphBuilder.Execute();
		});

		TempRTs.Add(TempRT);
		bAnySuccess = true;
	}

	UCSMesh::CountedBlockingFlush();

	for (UTextureRenderTarget2D* TempRT : TempRTs)
	{
		TempRT->MarkAsGarbage();
	}

	return bAnySuccess;
}

void AComputeShaderMeshGenerator::ConvertLandscapeHeightmapToNormalHeightRDG(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef LandscapeG16Texture,
	FRDGTextureRef OutputNormalHeight,
	float LandscapeScaleZ,
	float LandscapeOriginZ,
	FVector2f TexelWorldSize,
	bool bMergeByMaxZ)
{
	if (!LandscapeG16Texture || !OutputNormalHeight) return;

	FIntPoint TexSize(OutputNormalHeight->Desc.Extent.X, OutputNormalHeight->Desc.Extent.Y);

	TShaderMapRef<FLandscapeG16ToNormalHeightCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	auto* PassParams = GraphBuilder.AllocParameters<FLandscapeG16ToNormalHeightCS::FParameters>();
	PassParams->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(LandscapeG16Texture));
	PassParams->RW_HeightmapFloat = GraphBuilder.CreateUAV(OutputNormalHeight);
	PassParams->LHM_LandscapeScaleZ = LandscapeScaleZ;
	PassParams->LHM_LandscapeOriginZ = LandscapeOriginZ;
	PassParams->LHM_TextureSize = TexSize;
	PassParams->LHM_TexelWorldSize = TexelWorldSize;
	PassParams->LHM_MergeByMaxZ = bMergeByMaxZ ? 1u : 0u;

	FIntVector GroupCount(
		FMath::DivideAndRoundUp(TexSize.X, 8),
		FMath::DivideAndRoundUp(TexSize.Y, 8),
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("LandscapeG16ToNormalHeight"),
		ERDGPassFlags::Compute,
		CS,
		PassParams,
		GroupCount);
}

bool AComputeShaderMeshGenerator::CaptureLandscapeHeightmapGPU(
	FVector WorldCenter,
	float CaptureExtent,
	UTextureRenderTarget2D* OutNormalHeightRT)
{
	if (!OutNormalHeightRT) return false;

	UWorld* World = GetWorld();
	if (!World) return false;

	TArray<ALandscape*> Landscapes = CSMeshGenLC_GatherLandscapes(World);
	if (Landscapes.IsEmpty()) return false;

	const int32 TexSize = OutNormalHeightRT->SizeX;
	const bool bMultipleLandscapes = Landscapes.Num() > 1;

	// Pre-clear output: Normal=(0,0,1) up, Height=-1e10 (very low → any real terrain wins merge)
	FTextureRenderTargetResource* R_Out = OutNormalHeightRT->GameThread_GetRenderTargetResource();
	if (bMultipleLandscapes)
	{
		ENQUEUE_RENDER_COMMAND(ClearNormalHeightToMin)(
		[R_Out](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef RDG = RegisterExternalTexture(
				GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NHClear"));
			AddClearRenderTargetPass(GraphBuilder, RDG, FLinearColor(0, 0, 1, -1e10f));
			GraphBuilder.Execute();
		});
	}

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));
	const FVector2f CapturedTexelWorldSize(
		(CaptureExtent * 2.0f) / TexSize,
		(CaptureExtent * 2.0f) / TexSize);

	TArray<UTextureRenderTarget2D*> TempRTs;
	bool bAnySuccess = false;

	for (ALandscape* Landscape : Landscapes)
	{
		UTextureRenderTarget2D* TempRT = CSMeshGenLC_CreateTempHeightmapRT(TexSize, TexSize);

		if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
		{
			TempRT->MarkAsGarbage();
			continue;
		}

		const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
		const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
		const bool bMerge = bMultipleLandscapes;

		FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

		ENQUEUE_RENDER_COMMAND(LandscapeRGBAToNormalHeight)(
		[R_RGBA, R_Out, LandscapeScaleZ, LandscapeOriginZ, CapturedTexelWorldSize, bMerge](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);

			FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
			FRDGTextureRef RDG_Out = RegisterExternalTexture(GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NormalHeightOutput_RT"));

			ConvertLandscapeHeightmapToNormalHeightRDG(
				GraphBuilder, RDG_RGBA, RDG_Out,
				LandscapeScaleZ, LandscapeOriginZ, CapturedTexelWorldSize, bMerge);

			GraphBuilder.Execute();
		});

		TempRTs.Add(TempRT);
		bAnySuccess = true;
	}

	UCSMesh::CountedBlockingFlush();

	for (UTextureRenderTarget2D* TempRT : TempRTs)
	{
		TempRT->MarkAsGarbage();
	}

	return bAnySuccess;
}

bool AComputeShaderMeshGenerator::RenderLandscapeToNormalHeightRT(
	ALandscape* Landscape,
	FVector WorldCenter,
	FVector WorldExtentXY,
	UTextureRenderTarget2D* OutNormalHeightRT)
{
	if (!Landscape || !OutNormalHeightRT) return false;

	const int32 TexSizeX = OutNormalHeightRT->SizeX;
	const int32 TexSizeY = OutNormalHeightRT->SizeY;
	if (TexSizeX < 4 || TexSizeY < 4) return false;

	const float ExtX = FMath::Abs(WorldExtentXY.X);
	const float ExtY = FMath::Abs(WorldExtentXY.Y);
	if (ExtX < 1.0f || ExtY < 1.0f) return false;

	UTextureRenderTarget2D* TempRT = CSMeshGenLC_CreateTempHeightmapRT(TexSizeX, TexSizeY);

	FTransform AreaTransform(FQuat::Identity, WorldCenter, FVector::OneVector);
	FBox2D Extents(FVector2D(-ExtX, -ExtY), FVector2D(ExtX, ExtY));

	if (!Landscape->RenderHeightmap(AreaTransform, Extents, TempRT))
	{
		TempRT->MarkAsGarbage();
		return false;
	}

	const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
	const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
	const FVector2f TexelWorldSize(
		(ExtX * 2.0f) / TexSizeX,
		(ExtY * 2.0f) / TexSizeY);

	FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Out = OutNormalHeightRT->GameThread_GetRenderTargetResource();

	ENQUEUE_RENDER_COMMAND(LandscapeRGBAToNormalHeight_Static)(
	[R_RGBA, R_Out, LandscapeScaleZ, LandscapeOriginZ, TexelWorldSize](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGTextureRef RDG_RGBA = RegisterExternalTexture(GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_RT"));
		FRDGTextureRef RDG_Out = RegisterExternalTexture(GraphBuilder, R_Out->GetRenderTargetTexture(), TEXT("NormalHeight_RT"));

		// 与成员版共用同一实现（此前这里逐字重写过一遍 dispatch；成员版现已是 static）。
		ConvertLandscapeHeightmapToNormalHeightRDG(
			GraphBuilder, RDG_RGBA, RDG_Out,
			LandscapeScaleZ, LandscapeOriginZ, TexelWorldSize, /*bMergeByMaxZ*/ false);

		GraphBuilder.Execute();
	});

	UCSMesh::CountedBlockingFlush();

	TempRT->MarkAsGarbage();
	return true;
}

FCSTriangleMeshData AComputeShaderMeshGenerator::CaptureLandscapeTrianglesGPU(int32 TextureSize)
{
	FCSTriangleMeshData Result;

	const FBox Box = GetGeneratorBoundsWorldBox();
	if (!Box.IsValid) return Result;

	UWorld* World = GetWorld();
	if (!World) return Result;

	const FVector Center = Box.GetCenter();
	const FVector Extent = Box.GetExtent();
	const float CaptureExtent = FMath::Max(Extent.X, Extent.Y);
	TextureSize = FMath::Clamp(TextureSize, 4, 2048);

	TArray<ALandscape*> Landscapes = CSMeshGenLC_GatherLandscapes(World);
	if (Landscapes.IsEmpty()) return Result;

	ALandscape* Landscape = Landscapes[0];

	UTextureRenderTarget2D* TempRT = CSMeshGenLC_CreateTempHeightmapRT(TextureSize, TextureSize);

	FTransform AreaTransform(FQuat::Identity, Center, FVector::OneVector);
	FBox2D AreaExtents(FVector2D(-CaptureExtent, -CaptureExtent), FVector2D(CaptureExtent, CaptureExtent));

	if (!Landscape->RenderHeightmap(AreaTransform, AreaExtents, TempRT))
	{
		TempRT->MarkAsGarbage();
		return Result;
	}

	const float LandscapeScaleZ = Landscape->GetActorScale3D().Z;
	const float LandscapeOriginZ = Landscape->GetActorLocation().Z;
	const FVector2f TexelWorldSize(
		(CaptureExtent * 2.0f) / TextureSize,
		(CaptureExtent * 2.0f) / TextureSize);
	const FVector2f WorldOriginXY(
		float(Center.X - CaptureExtent),
		float(Center.Y - CaptureExtent));

	const int32 GridCells = TextureSize - 1;
	const int32 TotalVerts = GridCells * GridCells * 6;
	const uint32 ReadbackBytes = uint32(int64(TotalVerts) * sizeof(FVector4f));

	FTextureRenderTargetResource* R_RGBA = TempRT->GameThread_GetRenderTargetResource();

	FRHIGPUBufferReadback* VertReadback = new FRHIGPUBufferReadback(TEXT("LandscapeTriangles_VertReadback"));
	bool bRenderWorkQueued = false;

	ENQUEUE_RENDER_COMMAND(LandscapeToTriangles)(
	[R_RGBA, LandscapeScaleZ, LandscapeOriginZ, TexelWorldSize, WorldOriginXY,
	 TextureSize, GridCells, TotalVerts, ReadbackBytes, VertReadback,
	 &bRenderWorkQueued](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);

		FRDGTextureRef RDG_RGBA = RegisterExternalTexture(
			GraphBuilder, R_RGBA->GetRenderTargetTexture(), TEXT("LandscapeRGBA_Tri"));

		FRDGBufferRef TriBuffer = GraphBuilder.CreateBuffer(
			FRDGBufferDesc::CreateStructuredDesc(sizeof(FVector4f), TotalVerts),
			TEXT("LandscapeTriVerts"));

		TShaderMapRef<FLandscapeHeightmapToTrianglesCS> CS(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		auto* Params = GraphBuilder.AllocParameters<FLandscapeHeightmapToTrianglesCS::FParameters>();
		Params->T_LandscapeRGBA = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(RDG_RGBA));
		Params->RW_TriangleVerts = GraphBuilder.CreateUAV(TriBuffer);
		Params->LHM_LandscapeScaleZ = LandscapeScaleZ;
		Params->LHM_LandscapeOriginZ = LandscapeOriginZ;
		Params->LHM_TextureSize = FIntPoint(TextureSize, TextureSize);
		Params->LHM_WorldOriginXY = WorldOriginXY;
		Params->LHM_TexelWorldSize = TexelWorldSize;

		FIntVector GroupCount(
			FMath::DivideAndRoundUp(GridCells, 8),
			FMath::DivideAndRoundUp(GridCells, 8),
			1);
		FComputeShaderUtils::AddPass(
			GraphBuilder, RDG_EVENT_NAME("LandscapeHeightmapToTriangles"),
			ERDGPassFlags::Compute, CS, Params, GroupCount);

		AddEnqueueCopyPass(GraphBuilder, VertReadback, TriBuffer, ReadbackBytes);

		GraphBuilder.Execute();
		bRenderWorkQueued = true;
	});

	UCSMesh::CountedBlockingFlush();

	if (bRenderWorkQueued)
	{
		ENQUEUE_RENDER_COMMAND(LandscapeTrianglesReadback)(
		[VertReadback, ReadbackBytes, TotalVerts, &Result](FRHICommandListImmediate& RHICmdList)
		{
			if (!VertReadback->IsReady())
				RHICmdList.SubmitAndBlockUntilGPUIdle();

			if (VertReadback->IsReady() && VertReadback->GetGPUSizeBytes() >= ReadbackBytes)
			{
				if (const FVector4f* SrcData = static_cast<const FVector4f*>(VertReadback->Lock(ReadbackBytes)))
				{
					Result.Vertices.SetNumUninitialized(TotalVerts);
					for (int32 i = 0; i < TotalVerts; ++i)
						Result.Vertices[i] = FVector(SrcData[i].X, SrcData[i].Y, SrcData[i].Z);
					Result.VertexCount = TotalVerts;
					VertReadback->Unlock();
				}
			}
			delete VertReadback;
		});
		UCSMesh::CountedBlockingFlush();
	}
	else
	{
		delete VertReadback;
	}

	TempRT->MarkAsGarbage();

	UE_LOG(LogTemp, Log, TEXT("[CaptureLandscapeTrianglesGPU] %d verts (%d tris), TexSize=%d, Center=(%.0f,%.0f) Extent=%.0f"),
		Result.VertexCount, Result.VertexCount / 3, TextureSize, Center.X, Center.Y, CaptureExtent);
	return Result;
}

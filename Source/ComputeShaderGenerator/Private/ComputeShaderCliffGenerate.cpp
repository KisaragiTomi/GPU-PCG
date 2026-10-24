#include "ComputeShaderCliffGenerate.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGenerateHelper.h"
#include "CSRangeGeneratorActor.h"
#include "CSMesh.h"   // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "GlobalShader.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "EngineUtils.h"
#include "Engine/StaticMesh.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "Engine/Texture2DArray.h"

DECLARE_STATS_GROUP(TEXT("CSCliffGenerate"), STATGROUP_CSCliffGenerate, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CS Execute"), STAT_CSCliffGenerate_Execute, STATGROUP_CSCliffGenerate)
DECLARE_CYCLE_STAT(TEXT("CS Capture"), STAT_CSCliffGenerate_Capture, STATGROUP_CSCliffGenerate)
DECLARE_CYCLE_STAT(TEXT("CS Tatal"), STAT_CSCliffGenerate_Tatal, STATGROUP_CSCliffGenerate);

using namespace CSHelper;

ACSCliffGenerateCapture::ACSCliffGenerateCapture()
: Super()
{

}

void ACSCliffGenerateCapture::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

}
void ACSCliffGenerateCapture::Generate(int32 NumIteration, float InSpawnSize)
{
	Super::Generate(NumIteration, InSpawnSize);

	SCOPE_CYCLE_COUNTER(STAT_CSCliffGenerate_Tatal);
	CheckTexture();
	if (!IsParameterValidMult()) return;
	
	RT_ObjectDepth->ResizeTarget(TextureSize, TextureSize);
	RT_ObjectNormal->ResizeTarget(TextureSize, TextureSize);
	RT_SceneNormal->ResizeTarget(TextureSize, TextureSize);
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_CurrentSceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_HeightNormal->ResizeTarget(TextureSize, TextureSize);
	RT_TargetHeight->ResizeTarget(TextureSize, TextureSize);
	RT_HeightData->ResizeTarget(TextureSize, TextureSize);
	// RT_WorldPosition->ResizeTarget(TextureSize, TextureSize);

	
	SpawnSize = InSpawnSize;
	TArray<FCSMeshFillData> GenerateDatas;
	GenerateDatas.Reserve(NumIteration);
	for (int32 i = 0; i < NumIteration; i++)
	{
		int32 SelectIndex = FMath::RandRange(0, MeshDataAssets.Num() - 1);
		// float RandomRotate = FMath::FRandRange(0.0, 7.0);
		// float RandomHeightOffset = FMath::FRandRange(-350.0, 550.0) * (SelectIndex > 1);
		
		FCSMeshFillData GenerateData;
		GenerateData.SelectIndex = SelectIndex;
		GenerateData.RandomScale = MeshDataAssets[SelectIndex]->RandomScale;
		GenerateData.RandomRotate = MeshDataAssets[SelectIndex]->RandomRotate;
		GenerateData.RandomHeightOffset = MeshDataAssets[SelectIndex]->RandomHeightOffset;
		GenerateDatas.Add(GenerateData);
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_CSCliffGenerate_Capture);
		CaptureMeshsInBox();
		GenerateTargetHeightCal();
		// FlushRenderingCommands();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_CSCliffGenerate_Execute);
		GenerateCliffVerticalCal(GenerateDatas);
		UCSMesh::CountedBlockingFlush();
	}
	
	TArray<FLinearColor> LinearSamples;
	
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FIntRect SampleRect(0, 0, RT_Result->SizeX - 1, RT_Result->SizeY - 1);
	FReadSurfaceDataFlags ReadSurfaceDataFlags = FReadSurfaceDataFlags(RCM_MinMax);
	R_Result->ReadLinearColorPixels(LinearSamples, ReadSurfaceDataFlags, SampleRect);
	
	// switch (EPixelFormat ReadRenderTargetHelper(Samples, LinearSamples, WorldContextObject, TextureRenderTarget, X, Y, 1, 1))
	// {
	// case PF_B8G8R8A8:
	// 	check(Samples.Num() == 1 && LinearSamples.Num() == 0);
	// 	return Samples[0];
	// case PF_FloatRGBA:
	// 	check(Samples.Num() == 0 && LinearSamples.Num() == 1);
	// 	return LinearSamples[0].ToFColor(true);
	TArray<FLinearColor> Colors;
	UKismetRenderingLibrary::ReadRenderTargetRaw(this, RT_Result, Colors, false);
	int32 MaxGenerate = FMath::RoundToInt(Colors[Colors.Num() - 1].R);
	if (MaxGenerate == 0)	return;

	
	TArray<ACSInstanceContainer*> InstanceContainers; 
	for (UCSMeshAsset* MeshDataAsset : MeshDataAssets)
	{
		FName MeshName = MeshDataAsset->CSStaticMesh->GetFName();
		ACSInstanceContainer* Container = nullptr;
		for (TActorIterator<ACSInstanceContainer> It(GWorld, ACSInstanceContainer::StaticClass()); It; ++It)
		{
			if (It->Tags.Contains(MeshName))Container = *It;
		}
		if (Container != nullptr)
		{
			InstanceContainers.Add(Container);
			continue;
		}
		
		FActorSpawnParameters SpawnParameters;
		Container = GWorld->SpawnActor<ACSInstanceContainer>(SpawnParameters);
		Container->Instances->SetStaticMesh(MeshDataAsset->CSStaticMesh);
		Container->Tags.Add(MeshName);
		Container->Tags.Add(Tag);
		InstanceContainers.Add(Container);
	}


	for (int32 i = 0; i < MaxGenerate; i++)
	{
		FLinearColor LocationResultColor = Colors[i];
		float LocationX = LocationResultColor.R;
		float LocationY = LocationResultColor.G;
		float LocationZ = LocationResultColor.B;
		
		FLinearColor RSIResultColor = Colors[i + RT_Result->SizeX];
		float ResultRotate = RSIResultColor.R;
		float ResultScale = RSIResultColor.G;
		int32 SelectIndex = RSIResultColor.B;

		if (ResultScale < .01) continue;
		// UStaticMesh* GenerateStaticMesh = MeshDataAssets[SelectIndex]->CSStaticMesh;
		float MeshSize = MeshDataAssets[SelectIndex]->CSMeshSize;
		float Rotate = FMath::RadiansToDegrees(ResultRotate);
		FVector SpawnLocation = FVector(LocationX * CaptureSize, LocationY * CaptureSize, LocationZ ) - FVector(1, 1, 0) * CaptureSize / 2  + GetActorLocation();
		FRotator SpawnRotation = FRotator(0, Rotate, 0);
		FVector SpawnScale = FVector::OneVector * ResultScale / MeshSize * CaptureSize;
		
		FTransform SpawnTransform(SpawnRotation, SpawnLocation, SpawnScale);
		
		// SpawnMesh->SetActorTransform(SpawnTransform);
		// SpawnMesh->GetStaticMeshComponent()->SetStaticMesh(GenerateStaticMesh);
		// SpawnTransform.SetLocation(FVector(SpawnTransform.GetLocation().X, SpawnTransform.GetLocation().Y, SpawnTransform.GetLocation().Z - MaxHeight * 4));
		// // SpawnMeshCopy->SetActorTransform(SpawnTransform);
		// // SpawnMeshCopy->GetStaticMeshComponent()->SetStaticMesh(GenerateStaticMesh);
		// // SpawnMeshCopy->Tags = {Tag};
		// SpawnMesh->Tags = {Tag};
		InstanceContainers[SelectIndex]->Instances->AddInstance(SpawnTransform);
	}
}



void ACSCliffGenerateCapture::GenerateCliffVertical(int32 NumIteration, float InSpawnSize)
{
	Generate(NumIteration, InSpawnSize);

}

void ACSCliffGenerateCapture::GenerateTargetHeightCal()
{

	if (!IsParameterValidMult()) return;



	FTextureRenderTargetResource* R_ObjectNormal = RT_ObjectNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneNormal = RT_SceneNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_CurrentSceneDepth = RT_CurrentSceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_TargetHeight = RT_TargetHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ObjectDepth = RT_ObjectDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_HeightNormal = RT_HeightNormal->GameThread_GetRenderTargetResource();
	float SizeX = R_SceneDepth->GetSizeXY().X;
	float SizeY = R_SceneDepth->GetSizeXY().Y;
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{

			// RDG_EVENT_SCOPE and RDG_GPU_STAT_SCOPE disabled to avoid MSVC ICE in UE 5.7.4

			const auto ComputeShader_InitTargetHeight = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_InitTargetHeight);
			const auto ComputeShader_TargetHeight = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_TargetHeight);


			auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), FComputeShaderUtils::kGolden2DGroupSize);
			FMeshFillMult::FParameters* PassParameters = GraphBuilder.AllocParameters<FMeshFillMult::FParameters>();
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("TmpRDG_DebugView")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);

			FRDGTextureRef TmpRDG_CurrentSceneDepth = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("TmpRDG_CurrentSceneDepth")); 
			FRDGTextureUAVRef RDGUAV_CurrentSceneDepth = GraphBuilder.CreateUAV(TmpRDG_CurrentSceneDepth);
			

			FRDGTextureRef TmpRDG_TargetHeight = ConvertToUVATextureFormat(GraphBuilder, R_TargetHeight, PF_FloatRGBA, TEXT("TmpRDG_TargetHeight"));
			FRDGTextureUAVRef RDGUAV_TargetHeight = GraphBuilder.CreateUAV(TmpRDG_TargetHeight);
			FRDGTextureRef TmpRDG_A = ConvertToUVATextureFormat(GraphBuilder, R_TargetHeight, PF_FloatRGBA, TEXT("TmpRDG_A"));
			FRDGTextureUAVRef RDGUAV_A = GraphBuilder.CreateUAV(TmpRDG_A);
			FRDGTextureRef TmpRDG_B = ConvertToUVATextureFormat(GraphBuilder, R_TargetHeight, PF_FloatRGBA, TEXT("TmpRDG_B"));
			FRDGTextureUAVRef RDGUAV_B = GraphBuilder.CreateUAV(TmpRDG_B);
			
			
			FRDGTextureRef RDG_CurrentSceneDepth =  RegisterExternalTexture(GraphBuilder, R_CurrentSceneDepth->GetRenderTargetTexture(), TEXT("RDG_CurrentSceneDepth"));
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("RDG_SceneDepth"));
			FRDGTextureRef RDG_SceneNormal = RegisterExternalTexture(GraphBuilder, R_SceneNormal->GetRenderTargetTexture(), TEXT("RDG_SceneNormal"));
			FRDGTextureRef RDG_ObjectNormal = RegisterExternalTexture(GraphBuilder, R_ObjectNormal->GetRenderTargetTexture(), TEXT("RDG_ObjectNormal"));
			FRDGTextureRef RDG_TargetHeight = RegisterExternalTexture(GraphBuilder, R_TargetHeight->GetRenderTargetTexture(), TEXT("RDG_TargetHeight"));
			FRDGTextureRef RDG_ObjectDepth = RegisterExternalTexture(GraphBuilder, R_ObjectDepth->GetRenderTargetTexture(), TEXT("RDG_ObjectDepth"));
			FRDGTextureRef RDG_HeightNormal = RegisterExternalTexture(GraphBuilder, R_HeightNormal->GetRenderTargetTexture(), TEXT("RDG_HeightNormal"));
			
			PassParameters->T_HeightNormal = RDG_HeightNormal;
			PassParameters->T_ObjectDepth = RDG_ObjectDepth;
			PassParameters->T_TargetHeight = RDG_TargetHeight;
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_SceneNormal = RDG_SceneNormal;
			PassParameters->T_CurrentSceneDepth = RDG_CurrentSceneDepth;
			PassParameters->T_ObjectNormal = RDG_ObjectNormal;
			PassParameters->RW_CurrentSceneDepth = RDGUAV_CurrentSceneDepth;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_TargetHeight = RDGUAV_TargetHeight;
			PassParameters->RW_TempA = RDGUAV_A;
			PassParameters->RW_TempB = RDGUAV_B;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("MeshFillVerticalRock"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_InitTargetHeight, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_InitTargetHeight, *PassParameters, GroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_CurrentSceneDepth, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_TargetHeight, RDG_TargetHeight, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_TargetHeight, TmpRDG_A, FRHICopyTextureInfo());
			AddCopyTexturePass(GraphBuilder, TmpRDG_TargetHeight, TmpRDG_B, FRHICopyTextureInfo());
			
			for (int32 i = 0; i < GroupCount.X * 2; i++)
			{
				GraphBuilder.AddPass(
				RDG_EVENT_NAME("GenerateTargetHeight"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_TargetHeight, GroupCount, i, RDGUAV_A, RDGUAV_B](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_TargetHeight, *PassParameters, GroupCount);
					if ( i % 2 == 0)
					{
						PassParameters->RW_TempA = RDGUAV_B;
						PassParameters->RW_TempB = RDGUAV_A;
					}
					else
					{
						PassParameters->RW_TempA = RDGUAV_A;
						PassParameters->RW_TempB = RDGUAV_B;
					}
				});
			}
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_A, RDG_TargetHeight, FRHICopyTextureInfo());
			
			FBlurTexture::FPermutationDomain PermutationVector;
			PermutationVector.Set<FBlurTexture::FBlurFunctionSet>(FBlurTexture::EBlurType::BT_BLUR15X15);
			TShaderMapRef<FBlurTexture> ComputeShader_Blur(GetGlobalShaderMap(GMaxRHIFeatureLevel), PermutationVector);
			
			FBlurTexture::FParameters* BlurPassParameters = GraphBuilder.AllocParameters<FBlurTexture::FParameters>();
			
			BlurPassParameters->T_BlurTexture = RDG_TargetHeight;
			BlurPassParameters->RW_BlurTexture = RDGUAV_TargetHeight ;
			BlurPassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			BlurPassParameters->BlurScale = 1;
			
			for (int32 i = 0; i < 1; i++)
			{
				GraphBuilder.AddPass(
				RDG_EVENT_NAME("Blur"),
				BlurPassParameters,
				ERDGPassFlags::AsyncCompute,
				[BlurPassParameters, ComputeShader_Blur, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Blur, *BlurPassParameters, GroupCount);
				});
				AddCopyTexturePass(GraphBuilder,TmpRDG_TargetHeight , RDG_TargetHeight, FRHICopyTextureInfo());
			}
			
			FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, DebugViewTexture, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();
	});
}

void ACSCliffGenerateCapture::CheckTexture()
{
	Super::CheckTexture();
	
	
	if (RT_HeightNormal == nullptr) RT_HeightNormal = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_CurrentSceneDepth == nullptr) RT_CurrentSceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_TargetHeight == nullptr) RT_TargetHeight = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_HeightData == nullptr) RT_HeightData = UKismetRenderingLibrary::CreateRenderTarget2D(this, TextureSize, TextureSize, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
}


void ACSCliffGenerateCapture::GenerateCliffVerticalCal(TArray<FCSMeshFillData> GenerateDatas)
{
	if (!IsParameterValidMult() || GenerateDatas.Num() == 0) return;
	
	//InConnectivityClassify->ResizeTarget(TextureSize, TextureSize);
	RT_TargetHeight->ResizeTarget(TextureSize, TextureSize);
	RT_Result->ResizeTarget(1024, 2);
	
	FTextureRenderTargetResource* R_ObjectNormal = RT_ObjectNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneNormal = RT_SceneNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_HeightNormal = RT_HeightNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_CurrentSceneDepth = RT_CurrentSceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_TargetHeight = RT_TargetHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ObjectDepth = RT_ObjectDepth->GameThread_GetRenderTargetResource();
	// FTextureRenderTargetResource* R_WorldPosition = RT_WorldPosition->GameThread_GetRenderTargetResource();
	
	float SizeX = R_SceneDepth->GetSizeXY().X;
	float SizeY = R_SceneDepth->GetSizeXY().Y;

	// InMeshHeightArray->SourceTextures.Empty();
	// //MeshHeightArray->SourceTextures.Add();
	// for (int32 i = 0; i < MeshDataAssets.Num(); i++)
	// {
	// 	InMeshHeightArray->SourceTextures.Add(MeshDataAssets[i]->CSMeshHeightTexture);
	// }
	// // InMeshHeightArray->UpdateSourceFromSourceTextures();
	// UComputeShaderBasicFunction::UpdateTextureArray(InMeshHeightArray);
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			
			const auto ComputeShader_Init = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_InitVerticalRockHeight);
			const auto ComputeShader_FVR = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_FillVerticalRock);
			const auto ComputeShader_FindPixelRW = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_FindBestPixelRW_256);
			const auto ComputeShader_ExtentGenerateMask = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_ExtentGenerateMask);
			const auto ComputeShader_UpdateCurrentHeightMult = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_UpdateCurrentHeightMult);
			
			FIntVector MeshCheckGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			FIntVector GeneralGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			FIntPoint TextureSizeXY = R_SceneDepth->GetSizeXY();
			FIntPoint FilterResultSizeXY = FIntPoint(MeshCheckGroupCount.X, MeshCheckGroupCount.Y);
			
			FMeshFillMult::FParameters* PassParameters = GraphBuilder.AllocParameters<FMeshFillMult::FParameters>();
			
			FRDGTextureRef TmpRDG_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("DebugView_Texture")); 
			FRDGTextureUAVRef RDGUAV_DebugView = GraphBuilder.CreateUAV(TmpRDG_DebugView);
			
			FRDGTextureRef TmpRDG_CurrentSceneDepth = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepth_Texture")); 
			FRDGTextureUAVRef RDGUAV_CurrentSceneDepth = GraphBuilder.CreateUAV(TmpRDG_CurrentSceneDepth);
			
			FRDGTextureRef TmpRDG_CurrentSceneDepthA = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepthA_Texture")); 
			FRDGTextureUAVRef RDGUAV_CurrentSceneDepthA = GraphBuilder.CreateUAV(TmpRDG_CurrentSceneDepthA);
			FRDGTextureRef TmpRDG_CurrentSceneDepthB = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepthB_Texture")); 
			FRDGTextureUAVRef RDGUAV_CurrentSceneDepthB = GraphBuilder.CreateUAV(TmpRDG_CurrentSceneDepthB);
			
			FRDGTextureRef TmpRDG_Result = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("Result_Texture")); 
			FRDGTextureUAVRef RDGUAV_Result = GraphBuilder.CreateUAV(TmpRDG_Result);
			
			FRDGTextureRef TmpRDG_ResultA = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("ResultA_Texture")); 
			FRDGTextureUAVRef RDGUAV_ResultA = GraphBuilder.CreateUAV(TmpRDG_ResultA);
			FRDGTextureRef TmpRDG_ResultB = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("ResultB_Texture")); 
			FRDGTextureUAVRef RDGUAV_ResultB = GraphBuilder.CreateUAV(TmpRDG_ResultB);

			
			FRDGTextureRef TmpRDG_TargetHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("TmpRDG_TargetHeight"));
			FRDGTextureUAVRef RDGUAV_TargetHeight = GraphBuilder.CreateUAV(TmpRDG_TargetHeight);
			
			FRDGTextureRef TmpRDG_FilterResult = ConvertToUVATextureFormat(GraphBuilder, FilterResultSizeXY, PF_A32B32G32R32F, TEXT("TmpRDG_FilterResult")); 
			FRDGTextureUAVRef RDGUAV_FilterResult = GraphBuilder.CreateUAV(TmpRDG_FilterResult);

			FRDGTextureRef TmpRDG_SaveRotateScale = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("TmpRDG_SaveRotateScale"));
			FRDGTextureUAVRef RDGUAV_SaveRotateScale = GraphBuilder.CreateUAV(TmpRDG_SaveRotateScale);

			FRDGTextureRef TmpRDG_FilterResulteMult = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("TmpRDG_FilterResulteMult"));
			FRDGTextureUAVRef RDGUAV_FilterResulteMult = GraphBuilder.CreateUAV(TmpRDG_FilterResulteMult);
			
			FRDGTextureRef TmpRDG_Deduplication = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("TmpRDG_Deduplication"));
			FRDGTextureUAVRef RDGUAV_Deduplication = GraphBuilder.CreateUAV(TmpRDG_Deduplication);


			
			FRDGTextureRef RDG_CurrentSceneDepth =  RegisterExternalTexture(GraphBuilder, R_CurrentSceneDepth->GetRenderTargetTexture(), TEXT("RDG_CurrentSceneDepth"));
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("RDG_SceneDepth"));
			FRDGTextureRef RDG_SceneNormal = RegisterExternalTexture(GraphBuilder, R_SceneNormal->GetRenderTargetTexture(), TEXT("RDG_SceneNormal"));
			FRDGTextureRef RDG_ObjectNormal = RegisterExternalTexture(GraphBuilder, R_ObjectNormal->GetRenderTargetTexture(), TEXT("RDG_ObjectNormal"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("RDG_Result"));
			FRDGTextureRef RDG_MeshDepthTextureArray = RegisterExternalTexture(GraphBuilder, InMeshHeightArray->GetResource()->GetTextureRHI(), TEXT("RDG_MeshDepthTextureArray"));
			FRDGTextureRef RDG_TargetHeight = RegisterExternalTexture(GraphBuilder, R_TargetHeight->GetRenderTargetTexture(), TEXT("RDG_TargetHeight"));
			FRDGTextureRef RDG_ObjectDepth = RegisterExternalTexture(GraphBuilder, R_ObjectDepth->GetRenderTargetTexture(), TEXT("RDG_ObjectDepth"));
			FRDGTextureRef RDG_HeightNormal = RegisterExternalTexture(GraphBuilder, R_HeightNormal->GetRenderTargetTexture(), TEXT("RDG_HeightNormal"));


			FRDGTextureRef TmpRDG_MeshTexture2DArray;
			FRDGTextureUAVRef RDGUAV_MeshHeightTexture2DArray;
			int32 ArraySize =  MeshDataAssets.Num();
			//unkown issue
			CreateUVATextureArrayFormat(GraphBuilder, ArraySize, TmpRDG_MeshTexture2DArray,RDGUAV_MeshHeightTexture2DArray , TextureSizeXY );
			
			TArray<FRDGTextureRef> MeshHeightTextures;
			MeshHeightTextures.Reserve(ArraySize);
			for (int32 i = 0 ; i < ArraySize ; i++)
			{
				UTexture2D* MeshHeightMap = MeshDataAssets[i]->CSMeshHeightTexture;
				FRDGTextureRef TMeshDepthTexture = RegisterExternalTexture(GraphBuilder, MeshHeightMap->GetResource()->GetTextureRHI(), TEXT("MeshHeight_T"));
				MeshHeightTextures.Add(TMeshDepthTexture);
			}
			for (int32 i = 0 ; i < ArraySize ; i++)
			{

				TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_BuildTextureArray);
				FGeneralFunctionShader::FParameters* TAPassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
					
				TAPassParameters->RW_TextureArray0 = RDGUAV_MeshHeightTexture2DArray;
				TAPassParameters->T_ProcssTexture0 = MeshHeightTextures[i];
				TAPassParameters->InputIntData0 = i;   // 目标切片 = 资产号：后续 pass 按 SelectIndex 取片
					
				GraphBuilder.AddPass(
				RDG_EVENT_NAME("BuildTextureArray"),
				TAPassParameters,
				ERDGPassFlags::AsyncCompute,
				[TAPassParameters, ComputeShader, GeneralGroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *TAPassParameters, GeneralGroupCount);
				});
			}
			
			const uint32 NumElements = SizeX * SizeY;
			constexpr uint32 BytesPerElement = sizeof(FVector4f);
			FRDGBufferRef Tmp_CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), TEXT("CountBuffer"));
			FRDGBufferUAVRef Tmp_CountBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_FloatRGBA));
			AddClearUAVPass(GraphBuilder,Tmp_CountBufferUAV, 0.0);
			
			FRDGBufferRef Tmp_FilterResult_Number = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, 256), TEXT("FilterResult_Number"));
			FRDGBufferUAVRef Tmp_FilterResult_Number_UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_R32G32_UINT));
			AddClearUAVPass(GraphBuilder,Tmp_FilterResult_Number_UAV, -1.0);

			FRDGBufferRef Tmp_FilterResult_NumberCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, 1), TEXT("FilterResult_NumberCount"));
			FRDGBufferUAVRef Tmp_FilterResult_NumberCount_UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_R32_UINT));
			AddClearUAVPass(GraphBuilder,Tmp_FilterResult_NumberCount_UAV, 0.0);


			PassParameters->T_HeightNormal = RDG_HeightNormal;
			PassParameters->T_ObjectDepth = RDG_ObjectDepth;
			PassParameters->T_TargetHeight = RDG_TargetHeight;
			PassParameters->T_Result = RDG_Result;
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_SceneNormal = RDG_SceneNormal;
			PassParameters->T_CurrentSceneDepth = RDG_CurrentSceneDepth;
			PassParameters->T_ObjectNormal = RDG_ObjectNormal;
			PassParameters->T_TMeshDepth = MeshHeightTextures[0];
			PassParameters->TA_MeshHeight = RDG_MeshDepthTextureArray;
			PassParameters->RW_SaveRotateScale = RDGUAV_SaveRotateScale;
			PassParameters->RW_CurrentSceneDepth = RDGUAV_CurrentSceneDepth;
			PassParameters->RW_CurrentSceneDepthA = RDGUAV_CurrentSceneDepthA;
			PassParameters->RW_CurrentSceneDepthB = RDGUAV_CurrentSceneDepthB;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->RW_Result = RDGUAV_Result;
			PassParameters->RW_ResultA = RDGUAV_ResultA;
			PassParameters->RW_ResultB = RDGUAV_ResultB;
			PassParameters->RW_FilterResult = RDGUAV_FilterResult;
			PassParameters->RW_TargetHeight = RDGUAV_TargetHeight;
			PassParameters->RW_Deduplication = RDGUAV_Deduplication;
			PassParameters->RW_FilterResulteMult = RDGUAV_FilterResulteMult;
			PassParameters->RW_FindPixelBuffer = Tmp_CountBufferUAV;
			PassParameters->RW_FindPixelBufferResult_Number = Tmp_FilterResult_Number_UAV;
			PassParameters->RW_FindPixelBufferResult_NumberCount = Tmp_FilterResult_NumberCount_UAV;
			PassParameters->RWA_MeshHeight = RDGUAV_MeshHeightTexture2DArray;
			PassParameters->SelectIndex = -1;
			PassParameters->GenerateThreshold = GenerateThreshold;
			PassParameters->UnGenerateThreshold = UnGenerateThreshold;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			PassParameters->CaptureSize = CaptureSize;
			
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Init"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_Init, GeneralGroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Init, *PassParameters, GeneralGroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_CurrentSceneDepth, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExtentNormalMask"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_ExtentGenerateMask, GeneralGroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_ExtentGenerateMask, *PassParameters, GeneralGroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_CurrentSceneDepth, TmpRDG_CurrentSceneDepthA, FRHICopyTextureInfo());
			
			
			for (int32 i = 0; i < GenerateDatas.Num(); i++)
			{
				UTexture2D* MeshHeightMap = MeshDataAssets[GenerateDatas[i].SelectIndex]->CSMeshHeightTexture;
				FRDGTextureRef TMeshDepthTexture = RegisterExternalTexture(GraphBuilder, MeshHeightMap->GetResource()->GetTextureRHI(), TEXT("MeshHeight_T"));
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("MeshFillVerticalRock"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_FVR, MeshCheckGroupCount, i, GenerateDatas, MeshHeightTextures, this, TMeshDepthTexture](FRHIComputeCommandList& RHICmdList)
					{
						float MeshSize = MeshDataAssets[GenerateDatas[i].SelectIndex]->CSMeshSize;
						float DrawSize = MeshSize / CaptureSize * SpawnSize ;
						PassParameters->Size = DrawSize;
						PassParameters->RandomRange = FVector4f(GenerateDatas[i].RandomRotate.X, GenerateDatas[i].RandomRotate.Y, GenerateDatas[i].RandomHeightOffset.X, GenerateDatas[i].RandomRotate.Y);
						PassParameters->HeightOffset = FMath::FRandRange(GenerateDatas[i].RandomHeightOffset.X, GenerateDatas[i].RandomRotate.Y);
						PassParameters->SelectIndex = GenerateDatas[i].SelectIndex;

						PassParameters->T_TMeshDepth = TMeshDepthTexture;
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FVR, *PassParameters, MeshCheckGroupCount);
					});
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("FindBestPiexelRW"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_FindPixelRW, i, RDGUAV_ResultA, RDGUAV_ResultB, FilterResultSizeXY,  GenerateDatas, MeshHeightTextures, TMeshDepthTexture](FRHIComputeCommandList& RHICmdList)
					{
						PassParameters->T_TMeshDepth = TMeshDepthTexture;
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FindPixelRW, *PassParameters, FIntVector(1, 1, 1));
						if ( i % 2 == 0)
						{
							PassParameters->RW_ResultA = RDGUAV_ResultB;
							PassParameters->RW_ResultB = RDGUAV_ResultA;
						}
						else
						{
							PassParameters->RW_ResultA = RDGUAV_ResultA;
							PassParameters->RW_ResultB = RDGUAV_ResultB;
						}
					});
				
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Update"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_UpdateCurrentHeightMult, GeneralGroupCount, i, RDGUAV_ResultA, RDGUAV_ResultB, RDGUAV_CurrentSceneDepthA, RDGUAV_CurrentSceneDepthB,  GenerateDatas, MeshHeightTextures, TMeshDepthTexture](FRHIComputeCommandList& RHICmdList)
					{
						PassParameters->T_TMeshDepth = TMeshDepthTexture;
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_UpdateCurrentHeightMult, *PassParameters, GeneralGroupCount);
						if ( i % 2 == 0)
						{
							PassParameters->RW_ResultA = RDGUAV_ResultB;
							PassParameters->RW_ResultB = RDGUAV_ResultA;
							PassParameters->RW_CurrentSceneDepthA = RDGUAV_CurrentSceneDepthB;
							PassParameters->RW_CurrentSceneDepthB = RDGUAV_CurrentSceneDepthA;
						}
						else
						{
							PassParameters->RW_ResultA = RDGUAV_ResultA;
							PassParameters->RW_ResultB = RDGUAV_ResultB;
							PassParameters->RW_CurrentSceneDepthA = RDGUAV_CurrentSceneDepthA;
							PassParameters->RW_CurrentSceneDepthB = RDGUAV_CurrentSceneDepthB;
						}
					});

			}
			if (GenerateDatas.Num() % 2 == 0)
			{
				AddCopyTexturePass(GraphBuilder, TmpRDG_CurrentSceneDepthA, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
				// AddCopyTexturePass(GraphBuilder, TmpRDG_ResultA, RDG_Result, FRHICopyTextureInfo());
			}
			else
			{
				AddCopyTexturePass(GraphBuilder, TmpRDG_CurrentSceneDepthB, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
				// AddCopyTexturePass(GraphBuilder, TmpRDG_ResultB, RDG_Result, FRHICopyTextureInfo());
			}
			
			AddCopyTexturePass(GraphBuilder, TmpRDG_ResultA, RDG_Result, FRHICopyTextureInfo());
			FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("DebugView_RT"));
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, DebugViewTexture, FRHICopyTextureInfo());
			
		}
		
		GraphBuilder.Execute();
	});
}

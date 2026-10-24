#include "ComputeShaderMeshFill.h"

#include "ClearQuad.h"
#include "ComputeShaderBasicFunction.h"
#include "ComputeShaderGenerateHelper.h"
#include "CSRangeGeneratorActor.h"
#include "CSMesh.h"   // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "GlobalShader.h"
#include "MaterialShader.h"

#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RenderTargetPool.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/Texture2DArray.h"
#include "GeometryScript/ShapeFunctions.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet/KismetSystemLibrary.h"

DECLARE_STATS_GROUP(TEXT("CSMeshFill"), STATGROUP_CSGenerate, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("CS Execute"), STAT_CSGenerate_Execute, STATGROUP_CSGenerate)
DECLARE_CYCLE_STAT(TEXT("CS Capture"), STAT_CSGenerate_Capture, STATGROUP_CSGenerate)
DECLARE_CYCLE_STAT(TEXT("CS Tatal"), STAT_CSGenerate_Tatal, STATGROUP_CSGenerate);



/// <summary>
///// This class carries our parameter declarations and acts as the bridge between cpp and HLSL.
/// </summary>
///

using namespace CSHelper;
using namespace UE::Geometry;


ACSFillTarget::ACSFillTarget()
{
	CaptureLight = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CaptureLight"));
	CaptureLight->OrthoWidth = CaptureSize;
	CaptureLight->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureLight->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
	CaptureLight->bAlwaysPersistRenderingState = true;
	CaptureLight->bCaptureEveryFrame = false;
	CaptureLight->SetupAttachment(CaptureSceneDepth, TEXT("CaptureLight"));

	CapturePostMask = CreateDefaultSubobject<USceneCaptureComponent2D>(TEXT("CapturePostMask"));
	CapturePostMask->OrthoWidth = CaptureSize;
	CapturePostMask->ProjectionType = ECameraProjectionMode::Orthographic;
	CapturePostMask->CaptureSource = ESceneCaptureSource::SCS_FinalColorHDR;
	CapturePostMask->bAlwaysPersistRenderingState = true;
	CapturePostMask->bCaptureEveryFrame = false;
	CapturePostMask->SetupAttachment(CaptureSceneDepth, TEXT("CapturePostMask"));
	
}

void ACSFillTarget::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);

	Box->SetRelativeScale3D(FVector(CaptureSceneDepth->OrthoWidth / 100, CaptureSceneDepth->OrthoWidth / 100, MaxHeight / 100));
	Box->SetRelativeLocation(FVector(0, 0, -Scale3DZ * 50));
	Box->AttachToComponent(RootComponent, FAttachmentTransformRules::KeepRelativeTransform);
	
	CaptureSceneDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureSceneDepth->SetRelativeLocation(FVector(0, 0, 0));
	CaptureObjectDepth->SetRelativeRotation(FRotator(-90, -90, 0));
	CaptureObjectDepth->SetRelativeLocation(FVector(0, 0, 0));
	CaptureSceneDepth->OrthoWidth = CaptureSize;
	CaptureObjNormal->OrthoWidth = CaptureSize;
	CaptureObjBaseColor->OrthoWidth = CaptureSize;
	CaptureSceneNormal->OrthoWidth = CaptureSize;
	CaptureObjectDepth->OrthoWidth = CaptureSize;
	CaptureLight->OrthoWidth = CaptureSize;
	CapturePostMask->OrthoWidth = CaptureSize;
}

void ACSFillTarget::CheckTexture()
{
	Super::CheckTexture();
	if (RT_CurrentSceneDepth == nullptr ) RT_CurrentSceneDepth = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
	if (RT_SceneLight == nullptr)
	{
		RT_SceneLight = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CaptureLight->TextureTarget = RT_SceneLight;
		
	}
	if (RT_Mask == nullptr)
	{
		RT_Mask = UKismetRenderingLibrary::CreateRenderTarget2D(this, 256, 256, ETextureRenderTargetFormat::RTF_RGBA16f, FLinearColor::Black, true, false);
		CapturePostMask->TextureTarget = RT_Mask;
	}
}

void ACSFillTarget::CaptureMeshsInBox()
{
	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes{ UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic) , UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic)};
	TArray<AActor*> ActorsToIgnore ;
	TArray<AActor*> OverlapOutActors;
	UKismetSystemLibrary::ComponentOverlapActors(Box, Box->GetComponentTransform(), ObjectTypes, AStaticMeshActor::StaticClass(), ActorsToIgnore, OverlapOutActors);

	TArray<AActor*> OverlapOutActorsWithTag;
	TArray<AActor*> OverlapOutActorsWithoutTag;
	for (AActor* OverlapActor : OverlapOutActors)
	{
		if (OverlapActor->Tags.Num() == 0 )
		{
			
			OverlapOutActorsWithoutTag.Add(OverlapActor);
		}
		else
		{
			if (OverlapActor->Tags.Contains(Tag)) OverlapOutActorsWithTag.Add(OverlapActor);
		}
	}

	for (TActorIterator<AActor> It(GWorld, AActor::StaticClass()); It; ++It)
	{
		if (It->Tags.Contains(Tag)) OverlapOutActorsWithTag.Add(*It);
	}
	
	CaptureObjectDepth->ShowOnlyActors = OverlapOutActorsWithTag;
	CaptureObjNormal->ShowOnlyActors = OverlapOutActorsWithTag;
	CaptureObjBaseColor->ShowOnlyActors = OverlapOutActorsWithTag;
	CaptureSceneDepth->HiddenActors = OverlapOutActorsWithTag;
	CaptureSceneNormal->HiddenActors = OverlapOutActorsWithTag;
	CaptureLight->HiddenActors = OverlapOutActorsWithTag;
	if (CaptureObjNormal->TextureTarget != nullptr)		CaptureObjNormal->CaptureScene();
	if (CaptureObjBaseColor->TextureTarget != nullptr)	CaptureObjBaseColor->CaptureScene();
	if (CaptureObjectDepth->TextureTarget != nullptr)	CaptureObjectDepth->CaptureScene();
	if (CaptureSceneDepth->TextureTarget != nullptr)	CaptureSceneDepth->CaptureScene();
	if (CaptureSceneNormal->TextureTarget != nullptr)	CaptureSceneNormal->CaptureScene();
	if (CaptureLight->TextureTarget != nullptr)			CaptureLight->CaptureScene();
	if (CapturePostMask->TextureTarget != nullptr)		CapturePostMask->CaptureScene();
}

void ACSFillTarget::FillTargetCal(TArray<FCSMeshFillData> GenerateDatas)
{
	CheckTexture();
	if (!IsParameterValidMult()) return;
	
	
	int32 ResultSize = GenerateTextureSize(GenerateDatas.Num());
	RT_Result->ResizeTarget(1024, 3);
	
	FTextureRenderTargetResource* R_ObjectNormal = RT_ObjectNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneNormal = RT_SceneNormal->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_CurrentSceneDepth = RT_CurrentSceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_ObjectDepth = RT_ObjectDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneLight = RT_SceneLight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_Mask = RT_Mask->GameThread_GetRenderTargetResource();
	float SizeX = R_SceneDepth->GetSizeXY().X;
	float SizeY = R_SceneDepth->GetSizeXY().Y;
	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			// QUICK_SCOPE_CYCLE_COUNTER(STAT_RPCSManager_Execute); 
			// SCOPED_DRAW_EVENT(RHICmdList, RPCSManager_Execute);
			// DECLARE_GPU_STAT(CSFillTarget)
			// RDG_EVENT_SCOPE(GraphBuilder, "CSFillTarget");
			// RDG_GPU_STAT_SCOPE(GraphBuilder, CSFillTarget);
			
			TShaderMapRef<FMeshFillMult> ComputeShader_InitFillTarget = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_InitFillTarget);
			TShaderMapRef<FMeshFillMult> ComputeShader_FMM = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_FillMeshMult, false, true, true);
			TShaderMapRef<FMeshFillMult> ComputeShader_FindPixelRW = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_FindBestPixelRW_256);
			TShaderMapRef<FMeshFillMult> ComputeShader_Update = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_UpdateCurrentHeight);
			TShaderMapRef<FMeshFillMult> ComputeShader_ExtentGenerateMask = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_ExtentGenerateMask);
			TShaderMapRef<FMeshFillMult> ComputeShader_UpdateCurrentHeightMult = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_UpdateCurrentHeightMult, true);
			
			FIntVector MeshCheckGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 16);
			FIntVector GeneralGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			FIntPoint TextureSizeXY = R_SceneDepth->GetSizeXY();
			FIntPoint FilterResultSizeXY = FIntPoint(MeshCheckGroupCount.X, MeshCheckGroupCount.Y);
			
			FIntVector ReductionGroupSize = FIntVector(SizeX * SizeY / SHAREGROUP_FINDEXT_SIZE, 1, 1);
			FMeshFillMult::FParameters* PassParameters = GraphBuilder.AllocParameters<FMeshFillMult::FParameters>();
			
			FRDGTextureRef TmpTexture_DebugView = ConvertToUVATextureFormat(GraphBuilder, R_DebugView, PF_FloatRGBA, TEXT("DebugView_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_DebugView = GraphBuilder.CreateUAV(TmpTexture_DebugView);
			
			FRDGTextureRef TmpTexture_CurrentSceneDepth = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepth_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_CurrentSceneDepth = GraphBuilder.CreateUAV(TmpTexture_CurrentSceneDepth);
			
			FRDGTextureRef TmpTexture_CurrentSceneDepthA = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepthA_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_CurrentSceneDepthA = GraphBuilder.CreateUAV(TmpTexture_CurrentSceneDepthA);
			FRDGTextureRef TmpTexture_CurrentSceneDepthB = ConvertToUVATextureFormat(GraphBuilder, R_CurrentSceneDepth, PF_FloatRGBA, TEXT("CurrentSceneDepthB_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_CurrentSceneDepthB = GraphBuilder.CreateUAV(TmpTexture_CurrentSceneDepthB);
			
			FRDGTextureRef TmpTexture_Result = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("Result_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_Result = GraphBuilder.CreateUAV(TmpTexture_Result);
			
			FRDGTextureRef TmpTexture_ResultA = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("ResultA_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_ResultA = GraphBuilder.CreateUAV(TmpTexture_ResultA);
			FRDGTextureRef TmpTexture_ResultB = ConvertToUVATextureFormat(GraphBuilder, R_Result, PF_FloatRGBA, TEXT("ResultB_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_ResultB = GraphBuilder.CreateUAV(TmpTexture_ResultB);

			
			FRDGTextureRef TmpTexture_TargetHeight = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("TargetHeight_Texture"));
			FRDGTextureUAVRef TmpTextureUAV_TargetHeight = GraphBuilder.CreateUAV(TmpTexture_TargetHeight);
			
			FRDGTextureRef TmpTexture_FilterResult = ConvertToUVATextureFormat(GraphBuilder, FilterResultSizeXY, PF_A32B32G32R32F, TEXT("FilterResult_Texture")); 
			FRDGTextureUAVRef TmpTextureUAV_FilterResult = GraphBuilder.CreateUAV(TmpTexture_FilterResult);

			FRDGTextureRef TmpTexture_SaveRotateScale = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("SaveRotateSacle_Texture"));
			FRDGTextureUAVRef TmpTextureUAV_SaveRotateScale = GraphBuilder.CreateUAV(TmpTexture_SaveRotateScale);

			FRDGTextureRef TmpTexture_FilterResulteMult = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("FilterResulteMult_Texture"));
			FRDGTextureUAVRef TmpTextureUAV_FilterResulteMult = GraphBuilder.CreateUAV(TmpTexture_FilterResulteMult);
			
			FRDGTextureRef TmpTexture_Deduplication = ConvertToUVATextureFormat(GraphBuilder, TextureSizeXY, PF_FloatRGBA, TEXT("Deduplication_Texture"));
			FRDGTextureUAVRef TmpTextureUAV_Deduplication = GraphBuilder.CreateUAV(TmpTexture_Deduplication);
			
			FRDGTextureRef RDG_CurrentSceneDepth =  RegisterExternalTexture(GraphBuilder, R_CurrentSceneDepth->GetRenderTargetTexture(), TEXT("RDG_CurrentSceneDepth"));
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetRenderTargetTexture(), TEXT("RDG_SceneDepth"));
			FRDGTextureRef RDG_SceneNormal = RegisterExternalTexture(GraphBuilder, R_SceneNormal->GetRenderTargetTexture(), TEXT("RDG_SceneNormal"));
			FRDGTextureRef RDG_ObjectNormal = RegisterExternalTexture(GraphBuilder, R_ObjectNormal->GetRenderTargetTexture(), TEXT("RDG_ObjectNormal"));
			FRDGTextureRef RDG_Result = RegisterExternalTexture(GraphBuilder, R_Result->GetRenderTargetTexture(), TEXT("RDG_Result"));
			// FRDGTextureRef RDG_TMeshDepthArray = RegisterExternalTexture(GraphBuilder, InMeshHeightArray->GetResource()->GetTextureRHI(), TEXT("RDG_MeshHeight"));
			FRDGTextureRef RDG_ObjectDepth = RegisterExternalTexture(GraphBuilder, R_ObjectDepth->GetRenderTargetTexture(), TEXT("RDG_ObjectDepth"));
			FRDGTextureRef RDG_SceneLight = RegisterExternalTexture(GraphBuilder, R_SceneLight->GetRenderTargetTexture(), TEXT("RDG_SceneLight"));
			FRDGTextureRef RDG_Mask = RegisterExternalTexture(GraphBuilder, R_Mask->GetRenderTargetTexture(), TEXT("RDG_Mask"));


			FRDGTextureRef TmpRDG_MeshTexture2DArray;
			FRDGTextureUAVRef RDGUAV_MeshHeightTexture2DArray;
			int32 ArraySize =  MeshDataAssets.Num();
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
			const uint32 BytesPerElement = sizeof(FVector4f);
			FRDGBufferRef Tmp_CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, NumElements), TEXT("FindPixelBuffer"));
			FRDGBufferUAVRef Tmp_CountBufferUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_FloatRGBA));
			AddClearUAVPass(GraphBuilder,Tmp_CountBufferUAV, 0.0);
			
			FRDGBufferRef Tmp_FilterResult_Number = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, 256), TEXT("FindPixelBuffer"));
			FRDGBufferUAVRef Tmp_FilterResult_Number_UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_R32G32_UINT));
			AddClearUAVPass(GraphBuilder,Tmp_FilterResult_Number_UAV, -1.0);
			
			FRDGBufferRef Tmp_FilterResult_NumberCount = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(BytesPerElement, 1), TEXT("FindPixelBuffer"));
			FRDGBufferUAVRef Tmp_FilterResult_NumberCount_UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Tmp_CountBuffer, EPixelFormat::PF_R32_UINT));
			AddClearUAVPass(GraphBuilder,Tmp_FilterResult_NumberCount_UAV, 0.0);

			FRotator ActorRotator = GetActorTransform().Rotator();
			FTransform ActorTransformOrig = GetActorTransform();
			FTransform TransformFromXZ = UGeometryScriptLibrary_TransformFunctions::MakeTransformFromAxes(FVector(0, 0, 0), GetActorUpVector(), GetActorForwardVector()).Inverse();
			FTransform ActorTransform = FTransform(GetActorTransform().Rotator(), FVector::Zero(), FVector::OneVector).Inverse();
			FMatrix44f ActorMatrix = (FMatrix44f)ActorTransform.ToMatrixWithScale();
			FVector3f CameraDir = (FVector3f)GetActorUpVector();
			
			PassParameters->T_ObjectDepth = RDG_ObjectDepth;
			PassParameters->T_Result = RDG_Result;
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_SceneNormal = RDG_SceneNormal;
			PassParameters->T_CurrentSceneDepth = RDG_CurrentSceneDepth;
			PassParameters->T_ObjectNormal = RDG_ObjectNormal;
			PassParameters->T_SceneLight = RDG_SceneLight;
			PassParameters->T_Mask = RDG_Mask;
			PassParameters->RW_SaveRotateScale = TmpTextureUAV_SaveRotateScale;
			PassParameters->RW_CurrentSceneDepth = TmpTextureUAV_CurrentSceneDepth;
			PassParameters->RW_CurrentSceneDepthA = TmpTextureUAV_CurrentSceneDepthA;
			PassParameters->RW_CurrentSceneDepthB = TmpTextureUAV_CurrentSceneDepthB;
			PassParameters->RW_DebugView = TmpTextureUAV_DebugView;
			PassParameters->RW_Result = TmpTextureUAV_Result;
			PassParameters->RW_ResultA = TmpTextureUAV_ResultA;
			PassParameters->RW_ResultB = TmpTextureUAV_ResultB;
			PassParameters->RW_FilterResult = TmpTextureUAV_FilterResult;
			PassParameters->RW_TargetHeight = TmpTextureUAV_TargetHeight;
			PassParameters->RW_Deduplication = TmpTextureUAV_Deduplication;
			PassParameters->RW_FilterResulteMult = TmpTextureUAV_FilterResulteMult;
			PassParameters->RW_FindPixelBuffer = Tmp_CountBufferUAV;
			PassParameters->RW_FindPixelBufferResult_Number = Tmp_FilterResult_Number_UAV;
			PassParameters->RW_FindPixelBufferResult_NumberCount = Tmp_FilterResult_NumberCount_UAV;
			PassParameters->RWA_MeshHeight = RDGUAV_MeshHeightTexture2DArray;
			PassParameters->GenerateThreshold = GenerateThreshold;
			PassParameters->UnGenerateThreshold = UnGenerateThreshold;
			PassParameters->CameraDir = CameraDir;
			PassParameters->CaptureSize = CaptureSize;
			PassParameters->ActorTransform = ActorMatrix;
			PassParameters->SelectIndex = -1;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("InitFillTarget"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_InitFillTarget, GeneralGroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_InitFillTarget, *PassParameters, GeneralGroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpTexture_CurrentSceneDepth, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
			
			for (int32 i = 0; i < GenerateDatas.Num(); i++)
			{
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("MeshFillTarget"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_FMM, MeshCheckGroupCount, i, GenerateDatas, MeshHeightTextures, this](FRHIComputeCommandList& RHICmdList)
					{
						float MeshSize = MeshDataAssets[GenerateDatas[i].SelectIndex]->CSMeshSize;
						float DrawSize = MeshSize / CaptureSize * SpawnSize ;
						PassParameters->Size = DrawSize;
						PassParameters->RandomRange = FVector4f(GenerateDatas[i].RandomRotate.X, GenerateDatas[i].RandomRotate.Y, GenerateDatas[i].RandomHeightOffset.X, GenerateDatas[i].RandomRotate.Y);
						PassParameters->HeightOffset = FMath::FRandRange(GenerateDatas[i].RandomHeightOffset.X, GenerateDatas[i].RandomRotate.Y);
						PassParameters->SelectIndex = GenerateDatas[i].SelectIndex;

						PassParameters->T_TMeshDepth = MeshHeightTextures[GenerateDatas[i].SelectIndex];
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FMM, *PassParameters, MeshCheckGroupCount);
					});
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("FindBestPiexelRW"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_FindPixelRW, i, TmpTextureUAV_ResultA, TmpTextureUAV_ResultB, FilterResultSizeXY, MeshHeightTextures](FRHIComputeCommandList& RHICmdList)
					{
						PassParameters->T_TMeshDepth = MeshHeightTextures[0];
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_FindPixelRW, *PassParameters, FIntVector(1, 1, 1));
						if ( i % 2 == 0)
						{
							PassParameters->RW_ResultA = TmpTextureUAV_ResultB;
							PassParameters->RW_ResultB = TmpTextureUAV_ResultA;
						}
						else
						{
							PassParameters->RW_ResultA = TmpTextureUAV_ResultA;
							PassParameters->RW_ResultB = TmpTextureUAV_ResultB;
						}
					});
				
				
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("UpdateMult"),
					PassParameters,
					ERDGPassFlags::AsyncCompute,
					[PassParameters, ComputeShader_UpdateCurrentHeightMult, GeneralGroupCount, i, TmpTextureUAV_ResultA, TmpTextureUAV_ResultB, TmpTextureUAV_CurrentSceneDepthA, TmpTextureUAV_CurrentSceneDepthB, MeshHeightTextures, GenerateDatas](FRHIComputeCommandList& RHICmdList)
					{
						PassParameters->T_TMeshDepth = MeshHeightTextures[0];
						FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_UpdateCurrentHeightMult, *PassParameters, GeneralGroupCount);
						if ( i % 2 == 0)
						{
							PassParameters->RW_ResultA = TmpTextureUAV_ResultB;
							PassParameters->RW_ResultB = TmpTextureUAV_ResultA;
							PassParameters->RW_CurrentSceneDepthA = TmpTextureUAV_CurrentSceneDepthB;
							PassParameters->RW_CurrentSceneDepthB = TmpTextureUAV_CurrentSceneDepthA;
						}
						else
						{
							PassParameters->RW_ResultA = TmpTextureUAV_ResultA;
							PassParameters->RW_ResultB = TmpTextureUAV_ResultB;
							PassParameters->RW_CurrentSceneDepthA = TmpTextureUAV_CurrentSceneDepthA;
							PassParameters->RW_CurrentSceneDepthB = TmpTextureUAV_CurrentSceneDepthB;
						}
					});

			}
			if (GenerateDatas.Num() % 2 == 0)
			{
				AddCopyTexturePass(GraphBuilder, TmpTexture_CurrentSceneDepthA, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
				// AddCopyTexturePass(GraphBuilder, TmpTexture_ResultA, ResultTexture, FRHICopyTextureInfo());
			}
			else
			{
				AddCopyTexturePass(GraphBuilder, TmpTexture_CurrentSceneDepthB, RDG_CurrentSceneDepth, FRHICopyTextureInfo());
				// AddCopyTexturePass(GraphBuilder, TmpTexture_ResultB, ResultTexture, FRHICopyTextureInfo());
			}
			
			AddCopyTexturePass(GraphBuilder, TmpTexture_ResultA, RDG_Result, FRHICopyTextureInfo());
			FRDGTextureRef DebugViewTexture = RegisterExternalTexture(GraphBuilder, R_DebugView->GetRenderTargetTexture(), TEXT("RDG_DebugView"));
			AddCopyTexturePass(GraphBuilder, TmpTexture_DebugView, DebugViewTexture, FRHICopyTextureInfo());
			
		}
		
		GraphBuilder.Execute();
	});
}

void ACSFillTarget::FillTarget(int32 NumIteration, float InSpawnSize)
{
	Generate(NumIteration, InSpawnSize);
}

void ACSFillTarget::Generate(int32 NumIteration, float InSpawnSize)
{
	Super::Generate(NumIteration, InSpawnSize);

	SCOPE_CYCLE_COUNTER(STAT_CSGenerate_Tatal);
	CheckTexture();
	if (!IsParameterValidMult()) return;

	RT_ObjectDepth->ResizeTarget(TextureSize, TextureSize);
	RT_ObjectNormal->ResizeTarget(TextureSize, TextureSize);
	RT_SceneNormal->ResizeTarget(TextureSize, TextureSize);
	RT_SceneDepth->ResizeTarget(TextureSize, TextureSize);
	RT_DebugView->ResizeTarget(TextureSize, TextureSize);
	RT_CurrentSceneDepth->ResizeTarget(TextureSize, TextureSize);

	SpawnSize = InSpawnSize;
	TArray<FCSMeshFillData> GenerateDatas;
	GenerateDatas.Reserve(NumIteration);
	for (int32 i = 0; i < NumIteration; i++)
	{
		int32 SelectIndex = FMath::RandRange(0, MeshDataAssets.Num() - 1);
		
		FCSMeshFillData GenerateData;
		GenerateData.SelectIndex = SelectIndex;
		GenerateData.RandomScale = MeshDataAssets[SelectIndex]->RandomScale;
		GenerateData.RandomRotate = MeshDataAssets[SelectIndex]->RandomRotate;
		GenerateData.RandomHeightOffset = MeshDataAssets[SelectIndex]->RandomHeightOffset;
		GenerateDatas.Add(GenerateData);
	}

	{
		CaptureMeshsInBox();
		UCSMesh::CountedBlockingFlush();
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_CSGenerate_Execute);
		FillTargetCal(GenerateDatas);
		UCSMesh::CountedBlockingFlush();
	}
	
	TArray<FLinearColor> LinearSamples;
	
	FTextureRenderTargetResource* R_Result = RT_Result->GameThread_GetRenderTargetResource();
	FIntRect SampleRect(0, 0, RT_Result->SizeX - 1, RT_Result->SizeY - 1);
	FReadSurfaceDataFlags ReadSurfaceDataFlags = FReadSurfaceDataFlags(RCM_MinMax);
	R_Result->ReadLinearColorPixels(LinearSamples, ReadSurfaceDataFlags, SampleRect);
	
	float ResultSize = RT_Result->SizeX;
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

		FLinearColor NormalColor = Colors[i + RT_Result->SizeX * 2];
		FVector ResultNormal = FVector(NormalColor.R, NormalColor.G, NormalColor.B);

		
		UStaticMesh* GenerateStaticMesh = MeshDataAssets[SelectIndex]->CSStaticMesh;
		float MeshSize = MeshDataAssets[SelectIndex]->CSMeshSize;
		float Rotate = FMath::RadiansToDegrees(ResultRotate);

		FVector WorldSpaceResultNormal = GetActorTransform().Inverse().TransformVector(ResultNormal);
		FRotator SceneNormalRotator = FQuat::FindBetweenNormals(FVector(0, 0, 1), WorldSpaceResultNormal).Rotator();
		

		FVector ActorLocation = GetActorLocation();
		FRotator ActorRotation = GetActorRotation();
		FTransform ActorTransform = GetActorTransform();
		FVector ActorOffset = FVector(1, 1, 0) * (CaptureSize / 2 * FVector::OneVector - GetActorLocation());
		FVector SpawnLocation = FVector((LocationX - 0.5) * CaptureSize , (LocationY - 0.5) * CaptureSize, LocationZ - MaxHeight);
		FRotator SpawnRotation = FRotator(0, Rotate, 0);
		SpawnRotation = FRotator(FQuat(SceneNormalRotator) * FQuat(SpawnRotation));
		FVector SpawnScale = FVector::OneVector * ResultScale / MeshSize * CaptureSize;
		
		FTransform SpawnTransform(SpawnRotation, SpawnLocation, SpawnScale);
		SpawnTransform *= ActorTransform;
		
		// SpawnMesh->SetActorTransform(SpawnTransform);
		// SpawnMesh->GetStaticMeshComponent()->SetStaticMesh(GenerateStaticMesh);
		InstanceContainers[SelectIndex]->Instances->AddInstance(SpawnTransform);
		// SpawnTransform.SetLocation(FVector(SpawnTransform.GetLocation().X, SpawnTransform.GetLocation().Y, SpawnTransform.GetLocation().Z - MaxHeight * 4));
	}
	
	// TArray<AActor*> StaticMeshActors;
	// for (TActorIterator<AActor> It(GWorld, AStaticMeshActor::StaticClass()); It; ++It)
	// {
	// 	AActor* Actor = *It;
	// 	if (IsValid(Actor) && Actor->ActorHasTag(Tag))
	// 	{
	// 		StaticMeshActors.Add(Actor);
	// 	}
	// }
	// CaptureObjNormal->ShowOnlyActors = StaticMeshActors;
	// CaptureMeshsInBox();
}

void ACSFillTarget::CurvaturePostTest()
{
	CheckTexture();
	CaptureSceneDepth->CaptureScene();
	CaptureSceneNormal->CaptureScene();
	FTextureRenderTargetResource* R_DebugView = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* R_SceneNormal = RT_SceneNormal->GameThread_GetRenderTargetResource();
	

	
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[=, this ](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			// QUICK_SCOPE_CYCLE_COUNTER(STAT_RPCSManager_Execute); 
			// SCOPED_DRAW_EVENT(RHICmdList, RPCSManager_Execute);
			// DECLARE_GPU_STAT(CSFillTarget)
			// RDG_EVENT_SCOPE(GraphBuilder, "CSFillTarget");
			// RDG_GPU_STAT_SCOPE(GraphBuilder, CSFillTarget);
			
			TShaderMapRef<FMeshFillMult> ComputeShader_Test = FMeshFillMult::CreateMeshFillPermutation(FMeshFillMult::EMeshFillFunction::MF_Test);
			
			float SizeX = R_DebugView->GetSizeXY().X;
			float SizeY = R_DebugView->GetSizeXY().Y;
			FIntPoint TextureSizeXY = R_DebugView->GetSizeXY();
			FIntVector GeneralGroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(SizeX, SizeY, 1), 32);
			
			FIntVector ReductionGroupSize = FIntVector(SizeX * SizeY / SHAREGROUP_FINDEXT_SIZE, 1, 1);
			FMeshFillMult::FParameters* PassParameters = GraphBuilder.AllocParameters<FMeshFillMult::FParameters>();
			
			FRDGTextureRef TmpRDG_DebugView = nullptr;
			FRDGTextureUAVRef RDGUAV_DebugView = nullptr;
			ConvertToUVATextureFormat(GraphBuilder, TmpRDG_DebugView, RDGUAV_DebugView, TextureSize, PF_FloatRGBA, TEXT("UAV_DebugView"));
			
			FRDGTextureRef RDG_DebugView = RegisterExternalTexture(GraphBuilder, R_DebugView->GetTextureRHI(), TEXT("RDG_MeshHeight"));
			FRDGTextureRef RDG_SceneDepth = RegisterExternalTexture(GraphBuilder, R_SceneDepth->GetTextureRHI(), TEXT("RDG_SceneDepth"));
			FRDGTextureRef RDG_SceneNormal = RegisterExternalTexture(GraphBuilder, R_SceneNormal->GetTextureRHI(), TEXT("RDG_SceneNormal"));

			FTransform ActorTransform = FTransform(GetActorTransform().Rotator(), FVector::Zero(), FVector::OneVector).Inverse();
			FMatrix44f ActorMatrix = (FMatrix44f)ActorTransform.ToMatrixWithScale();
			
			PassParameters->T_SceneDepth = RDG_SceneDepth;
			PassParameters->T_SceneNormal = RDG_SceneNormal;
			PassParameters->RW_DebugView = RDGUAV_DebugView;
			PassParameters->ActorTransform = ActorMatrix;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Test"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader_Test, GeneralGroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader_Test, *PassParameters, GeneralGroupCount);
				});
			AddCopyTexturePass(GraphBuilder, TmpRDG_DebugView, RDG_DebugView, FRHICopyTextureInfo());
			
			
		}
		
		GraphBuilder.Execute();
	});
}


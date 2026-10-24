// Fill out your copyright notice in the Description page of Project Settings.


#include "CSAssetProcess.h"

#include "ClearQuad.h"
#include "GlobalShader.h"
#include "MaterialShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderGraphBuilder.h"
#include "RHIGPUReadback.h"
#include "RenderTargetPool.h"
#include "ComputeShaderGenerateHelper.h"
#include "EngineUtils.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "ComputeShaderGeneral.h"
#include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryHelpers.h"
#include "Engine/SceneCapture.h"
#include "Engine/SceneCapture2D.h"
#include "SceneRendererInterface.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ComputeShaderSceneCapture.h"
#include "EditorAssetLibrary.h"
#include "Materials/MaterialInstanceConstant.h"


#include "Engine/StaticMeshActor.h"
// #include "IAssetTools.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "ComputeShaderBasicFunction.h"
#include "MaterialUtilities.h"
#include "ModuleDescriptor.h"
#include "Selection.h"
#include "Editor/MaterialEditor/Public/MaterialEditingLibrary.h"
#include "Engine/DecalActor.h"
#include "Kismet/KismetMathLibrary.h"
#include "Runtime/Experimental/Voronoi/Private/voro++/src/container.hh"
#include "Subsystems/EditorAssetSubsystem.h"
#include "MeshDescription.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "StaticMeshAttributes.h"


class UEditorAssetSubsystem;
class FAssetRegistryModule;
class ISceneRenderer;

void UCSAssetProcess::CaptureMeshHeight(AStaticMeshActor* InMeshActor, UTextureRenderTarget2D*& OutRenderTarget2D, TArray<AActor*>& OutActors, int32 InTextureSize)
{
	if (InMeshActor == nullptr) return;
	UStaticMesh* ContainerMesh = InMeshActor->GetStaticMeshComponent()->GetStaticMesh();
	if (ContainerMesh == nullptr) return;

	FBoxSphereBounds Bounds = ContainerMesh->GetBounds();

	float MaxHeight = 10000;

	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	FActorSpawnParameters SpawnParameters;
	AStaticMeshActor *SpawnMeshTargetMesh = GWorld->SpawnActor<AStaticMeshActor>(SpawnParameters);
	AStaticMeshActor *SpawnMeshPlane = GWorld->SpawnActor<AStaticMeshActor>(SpawnParameters);
	SpawnMeshTargetMesh->GetStaticMeshComponent()->SetStaticMesh(ContainerMesh);
	SpawnMeshPlane->GetStaticMeshComponent()->SetStaticMesh(PlaneMesh);
	SpawnMeshPlane->SetActorScale3D(FVector(9999, 9999, 1));
	TArray<AActor*> TargetActors;
	TargetActors.Add(SpawnMeshPlane);
	TargetActors.Add(SpawnMeshTargetMesh);
	
	
	UTextureRenderTarget2D* NewRenderTarget2D = NewObject<UTextureRenderTarget2D>();
	check(NewRenderTarget2D);
	NewRenderTarget2D->RenderTargetFormat = ETextureRenderTargetFormat::RTF_RGBA16f;
	NewRenderTarget2D->ClearColor = FLinearColor::Black;
	NewRenderTarget2D->bAutoGenerateMips = false;
	NewRenderTarget2D->bCanCreateUAV = true;
	NewRenderTarget2D->InitAutoFormat(InTextureSize, InTextureSize);
	
	ASceneCapture2D * CaptureTarget = GWorld->SpawnActor<ASceneCapture2D>(SpawnParameters);
	FTransform CaptureTransform(FRotator(0, -90, -90), FVector(Bounds.Origin.X, Bounds.Origin.Y, MaxHeight), FVector::OneVector);
	
	
	CaptureTarget->SetActorTransform(CaptureTransform);
	CaptureTarget->GetCaptureComponent2D()->TextureTarget = NewRenderTarget2D;
	CaptureTarget->GetCaptureComponent2D()->OrthoWidth = FMath::Max(Bounds.BoxExtent.X, Bounds.BoxExtent.Y) * 2;
	CaptureTarget->GetCaptureComponent2D()->ShowOnlyActors = TargetActors;
	CaptureTarget->GetCaptureComponent2D()->ProjectionType = ECameraProjectionMode::Orthographic;
	CaptureTarget->GetCaptureComponent2D()->PrimitiveRenderMode = ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList;
	CaptureTarget->GetCaptureComponent2D()->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;

	OutActors.Add(SpawnMeshPlane);
	OutActors.Add(SpawnMeshTargetMesh);
	OutActors.Add(CaptureTarget);
}

void UCSAssetProcess::CalculateMeshHeight(AStaticMeshActor* InMeshActor,  UTextureRenderTarget2D* NewRenderTarget2D)
{
	
	float MaxHeight = 10000;
	if (InMeshActor == nullptr || NewRenderTarget2D == nullptr) return;
	UStaticMesh* ContainerMesh = InMeshActor->GetStaticMeshComponent()->GetStaticMesh(); 
	if (ContainerMesh == nullptr) return;

	FBoxSphereBounds Bounds = ContainerMesh->GetBounds();
	ULevel* CurrentLevel = InMeshActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetPath = LevelPath.Append("/MeshHeightData");
	IAssetRegistry &AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetData> OutAssetData;
	if (!AssetRegistry.GetAssetsByPath(FName(AssetPath), OutAssetData)) return;
	
	FTextureRenderTargetResource* NewTextureTarget = NewRenderTarget2D->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(SceneDrawCompletion)(
	[NewTextureTarget, MaxHeight](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList);
		{
			TShaderMapRef<FGeneralFunctionShader> ComputeShader = FGeneralFunctionShader::CreateShaderPermutation(FGeneralFunctionShader::EGeneralShader::GTS_ProcessMeshHeightTexture);

			FGeneralFunctionShader::FParameters* PassParameters = GraphBuilder.AllocParameters<FGeneralFunctionShader::FParameters>();
			auto GroupCount = FComputeShaderUtils::GetGroupCount(FIntVector(NewTextureTarget->GetSizeXY().X, NewTextureTarget->GetSizeXY().Y, 1), FComputeShaderUtils::kGolden2DGroupSize);
			
			FRDGTextureRef TmpTexture_ProcssTexture = CSHelper::ConvertToUVATexture(NewTextureTarget, GraphBuilder);
			FRDGTextureRef ProcssTexture = RegisterExternalTexture(GraphBuilder, NewTextureTarget->GetRenderTargetTexture(), TEXT("Input_RT"));
			
			PassParameters->T_ProcssTexture0 = ProcssTexture;
			PassParameters->RW_ProcssTexture0 = GraphBuilder.CreateUAV(TmpTexture_ProcssTexture);
			PassParameters->InputData0 = MaxHeight;
			PassParameters->Sampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();

			
			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ExecuteExampleComputeShader"),
				PassParameters,
				ERDGPassFlags::AsyncCompute,
				[PassParameters, ComputeShader, GroupCount](FRHIComputeCommandList& RHICmdList)
				{
					FComputeShaderUtils::Dispatch(RHICmdList, ComputeShader, *PassParameters, GroupCount);
				});
			
			AddCopyTexturePass(GraphBuilder, TmpTexture_ProcssTexture, ProcssTexture, FRHICopyTextureInfo());
		}
		GraphBuilder.Execute();

	});
	
	FlushRenderingCommands();
	
	
	TArray<FAssetData> AssetDatas;
	TArray<UTexture2D*> AssetTextures;
	for (int32 i = 0; i < OutAssetData.Num(); i++)
	{
		UCSMeshAsset* MeshAsset = Cast<UCSMeshAsset>(OutAssetData[i].GetAsset());
		if (MeshAsset != nullptr)
		{
			AssetDatas.Add(MeshAsset);
			continue;
		}

		UTexture2D* Texture = Cast<UTexture2D>(OutAssetData[i].GetAsset());
		if (Texture != nullptr)
		{
			AssetTextures.Add(Texture);
		}
	}

	

	
	FString ContainerMeshPathName = GetPathNameSafe(ContainerMesh);
	FString ContainerMeshFileName;
	FString ContainerMeshPath;
	FString ContainerMeshExtension;
	FPaths::Split(ContainerMeshPathName, ContainerMeshPath, ContainerMeshFileName, ContainerMeshExtension);


	IAssetTools &AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools").Get();
	// TScriptInterface<IAssetTools> AssetTools =  UAssetToolsHelpers::GetAssetTools();

	FString TextureAssetName = "T_";
	
	TextureAssetName.Append(ContainerMeshFileName);
	TextureAssetName.Append("_Height");
	
	FString TextureAssetPath = AssetPath.Append("/");
	TextureAssetPath.Append(TextureAssetName);

	UTexture2D* CurrentTextureObject = nullptr;
	bool FindTexture = false;
	if (UEditorAssetLibrary::DoesAssetExist(TextureAssetPath))
	{
		UObject* LoadObject = UEditorAssetLibrary::LoadAsset(TextureAssetPath);
		UTexture2D* Texture = Cast<UTexture2D>(LoadObject);
		if (Texture != nullptr)
		{
			CurrentTextureObject = Texture;
			FindTexture = true;
		}
	}
	if (!FindTexture)
	{
		UObject* NewTextureObject = AssetTools.CreateAsset(TextureAssetName, AssetPath, UTexture2D::StaticClass(), nullptr);
		UTexture2D* NewTexture = Cast<UTexture2D>(NewTextureObject);
		if (NewTexture != nullptr)
		{
			CurrentTextureObject = NewTexture;
		}
	}


	FString MeshAssetName = "MA_";
	
	MeshAssetName.Append(ContainerMeshFileName);
	MeshAssetName.Append("_MeshData");
	
	FString MeshAssetPath = AssetPath.Append("/");
	MeshAssetPath.Append(MeshAssetName);
	
	UCSMeshAsset* CurrentMeshAssetObject = nullptr;
	bool FindMeshAsset = false;
	if (UEditorAssetLibrary::DoesAssetExist(MeshAssetPath))
	{
		UObject* LoadObject = UEditorAssetLibrary::LoadAsset(MeshAssetPath);
		UCSMeshAsset* MeshAsset = Cast<UCSMeshAsset>(LoadObject);
		if (MeshAsset != nullptr)
		{
			CurrentMeshAssetObject = MeshAsset;
			FindMeshAsset = true;
		}
	}
	if (!FindMeshAsset)
	{
		
		UObject* NewMeshAssetObject = AssetTools.CreateAsset(MeshAssetName, AssetPath, UCSMeshAsset::StaticClass(), nullptr);
		UCSMeshAsset* NewMeshAsset = Cast<UCSMeshAsset>(NewMeshAssetObject);
		if (NewMeshAsset != nullptr)
		{
			CurrentMeshAssetObject = NewMeshAsset;
		}
	}
	
	CurrentTextureObject->CompressionNoAlpha = true;
	CurrentTextureObject->MipGenSettings = TMGS_NoMipmaps;
	CurrentTextureObject->SRGB = false;
	CurrentTextureObject->DeferCompression = true;
	CurrentTextureObject->AddressX = TextureAddress::TA_Clamp;
	CurrentTextureObject->AddressY = TextureAddress::TA_Clamp;
	
	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(GWorld, NewRenderTarget2D, CurrentTextureObject);
	
	CurrentTextureObject->CompressionSettings = TextureCompressionSettings::TC_HDR;
	
	CurrentMeshAssetObject->CSStaticMesh = ContainerMesh;
	CurrentMeshAssetObject->CSMeshHeightTexture = CurrentTextureObject;
	CurrentMeshAssetObject->CSMeshPivot = FVector(-Bounds.Origin.X, -Bounds.Origin.Y, Bounds.BoxExtent.Z - Bounds.Origin.Z);
	CurrentMeshAssetObject->CSMeshMaxHeight = Bounds.BoxExtent.Z * 2;
	CurrentMeshAssetObject->CSMeshSize =  FMath::Max(ContainerMesh->GetBounds().BoxExtent.X, ContainerMesh->GetBounds().BoxExtent.Y) * 2;

	UEditorAssetLibrary::SaveLoadedAsset(CurrentTextureObject, false);
	UEditorAssetLibrary::SaveLoadedAsset(CurrentMeshAssetObject, false);

	
	NewRenderTarget2D = nullptr;
}

UTexture2D* UCSAssetProcess::SaveTextureData(UTextureRenderTarget2D* RenderTarget, FString AssetName)
{
	if (RenderTarget == nullptr) return nullptr;
	
	ULevel* CurrentLevel = GWorld->GetLevel(0);
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFloderPath = LevelPath.Append("/ResultTexture");

	

	if (AssetName == "")
	{
		FDateTime Time = FDateTime::Now();
		int32 UniqueID = Time.GetDay() * 1e6 + Time.GetHour() * 1e4 + Time.GetMinute() * 1e2 + Time.GetSecond();
		AssetName = FString::Printf(TEXT("T_%d"), UniqueID);
	}
	else
	{
		AssetName = "T_" + AssetName;
	}

	UTexture2D* ResultTexture = UCSAssetProcess::ConveretAndSaveRTAsset(AssetName, AssetFloderPath, RenderTarget);
	return  ResultTexture;
}


template<typename T>
T* UCSAssetProcess::FindOrCreateAsset(FString AssetName, FString AssetFolderPath, TFunction<T*()> Func)
{

	FString AssetPath = AssetFolderPath + "/" + AssetName;
	T* ResultObject = nullptr;
	if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		ResultObject = Cast<T>(UEditorAssetLibrary::LoadAsset(AssetPath));
		if (ResultObject != nullptr) return ResultObject;
	}
	ResultObject = Func();
	
	return ResultObject;
}

UTexture2D* UCSAssetProcess::ConveretAndSaveRTAsset(FString AssetName, FString AssetFolderPath, UTextureRenderTarget2D* RenderTarget)
{
	TextureCompressionSettings InCompressionSettings = TC_Default;
	UTexture2D* OutTexture = FindOrCreateAsset<UTexture2D>(AssetName, AssetFolderPath,[&]()
	{
		UTexture2D* NewTexture = nullptr;
		EPixelFormat Format = RenderTarget->GetFormat();
		switch (Format)
		{
			//Convert Texture will fail when format is PF_A32B32G32R32F
			case EPixelFormat::PF_A32B32G32R32F:
				InCompressionSettings = TC_HDR_F32;
				break;
			case EPixelFormat::PF_A16B16G16R16:
				InCompressionSettings = TC_HDR;
				break;
			default:
				break;
		}
		
		NewTexture = UKismetRenderingLibrary::RenderTargetCreateStaticTexture2DEditorOnly(RenderTarget, AssetFolderPath + "/" + AssetName, InCompressionSettings, TextureMipGenSettings::TMGS_FromTextureGroup);
		return NewTexture;
	});


	//
	// // OutTexture->CompressionNoAlpha = true;
	// OutTexture->MipGenSettings = TMGS_NoMipmaps;
	// OutTexture->SRGB = false;
	// // OutTexture->DeferCompression = true;
	// OutTexture->AddressX = TextureAddress::TA_Clamp;
	// OutTexture->AddressY = TextureAddress::TA_Clamp;

	UKismetRenderingLibrary::ConvertRenderTargetToTexture2DEditorOnly(GWorld, RenderTarget, OutTexture);
	// UEditorAssetLibrary::SaveLoadedAsset(OutTexture, false);
	
	return OutTexture;
}

UMaterialInstance* UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset(FString SourcePath, FString TargetPath, bool Overwirte)
{
	FString TargetFolderName;
	FString TargetFileName;
	FString TargetExtension;
	FPaths::Split(TargetPath, TargetFolderName, TargetFileName, TargetExtension);
	if (Overwirte == true && UEditorAssetLibrary::DoesAssetExist(TargetFolderName / TargetFileName)) UEditorAssetLibrary::DeleteAsset(TargetFolderName / TargetFileName);

	if (!UEditorAssetLibrary::DoesAssetExist(SourcePath)) return nullptr;
	FString SourceFolderName;
	FString SourceFileName;
	FString SourceExtension;
	FPaths::Split(SourcePath, SourceFolderName, SourceFileName, SourceExtension);
	UMaterialInstance* Parent = Cast<UMaterialInstance>(UEditorAssetLibrary::LoadAsset(SourceFolderName / SourceFileName));
	if (Parent == nullptr) return nullptr;

	UMaterialInstance* OutMaterialInstance = nullptr;
	
	OutMaterialInstance = FindOrCreateAsset<UMaterialInstance>(TargetFileName, TargetFolderName,[&]()
	{
		
		UObject* NewObject = UEditorAssetLibrary::DuplicateAsset(SourcePath, TargetPath);
		UMaterialInstance* NewMaterialInstance = Cast<UMaterialInstance>(NewObject);
		return NewMaterialInstance;
	});
	
	UMaterialInstanceConstant* Constant = Cast<UMaterialInstanceConstant>(OutMaterialInstance);

	FStaticSwitchParameter SwitchParameter;
	// SwitchParameter.ParameterInfo.Name = TEXT("UseCustomUV");
	// SwitchParameter.Value = true;
	// SwitchParameter.bOverride = true;
	// NewStaticParameterSet.StaticSwitchParameters.Add(SwitchParameter);
	//
	// SwitchParameter.ParameterInfo.Name = *(TEXT("UseUV") + FString::FromInt(MeshData.TextureCoordinateIndex));
	// NewStaticParameterSet.StaticSwitchParameters.Add(SwitchParameter);
	OutMaterialInstance->InitStaticPermutation();

	UMaterialEditingLibrary::SetMaterialInstanceParent(Constant, Parent);
	// UEditorAssetLibrary::SaveLoadedAsset(OutMaterialInstance, false);
	
	
	return OutMaterialInstance;
}

UMaterialInstance* UCSAssetProcess::FindOrCreateMaterialInstanceAsset(FString AssetName, FString AssetFolderPath, UMaterialInterface* Parent)
{
	if (!Parent) return nullptr;

	FString AssetPath = AssetFolderPath / AssetName;

	if (UEditorAssetLibrary::DoesAssetExist(AssetPath))
	{
		UObject* Existing = UEditorAssetLibrary::LoadAsset(AssetPath);
		UMaterialInstanceConstant* ExistingMIC = Cast<UMaterialInstanceConstant>(Existing);
		if (ExistingMIC)
		{
			UMaterialEditingLibrary::SetMaterialInstanceParent(ExistingMIC, Parent);
			return ExistingMIC;
		}
		UEditorAssetLibrary::DeleteAsset(AssetPath);
	}

	UPackage* Package = CreatePackage(*AssetPath);
	UMaterialInstanceConstant* NewMIC = NewObject<UMaterialInstanceConstant>(
		Package, *AssetName, RF_Public | RF_Standalone);
	NewMIC->SetParentEditorOnly(Parent);
	NewMIC->PostEditChange();

	FAssetRegistryModule::AssetCreated(NewMIC);
	NewMIC->MarkPackageDirty();

	return NewMIC;
}

void UCSAssetProcess::CreateDebugTexture(AActor* TargetActor, UTextureRenderTarget2D* InDebugView, FString DebugName)
{
	ULevel* CurrentLevel =TargetActor ->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetPath = LevelPath.Append("/" + DebugName + "Debug");
	UCSAssetProcess::ConveretAndSaveRTAsset(TEXT("Debug"), AssetPath, InDebugView);
}

void UCSAssetProcess::DisplaceMeshByRTBlueChannel(
	UTextureRenderTarget2D* InRenderTarget,
	UStaticMesh* InStaticMesh,
	float DisplaceScale)
{
	if (!InRenderTarget || !InStaticMesh) return;

	// --- 1. 读取 16bit RT 像素 ---
	FTextureRenderTargetResource* RTResource = InRenderTarget->GameThread_GetRenderTargetResource();
	if (!RTResource) return;

	const int32 RTWidth  = InRenderTarget->SizeX;
	const int32 RTHeight = InRenderTarget->SizeY;

	TArray<FFloat16Color> Float16Pixels;
	Float16Pixels.SetNumUninitialized(RTWidth * RTHeight);

	if (!RTResource->ReadFloat16Pixels(Float16Pixels))
	{
		UE_LOG(LogTemp, Error, TEXT("DisplaceMeshByRTBlueChannel: ReadFloat16Pixels failed."));
		return;
	}

	// --- 2. 计算 RG 通道的 min/max 用于归一化 ---
	float MinR =  MAX_FLT, MaxR = -MAX_FLT;
	float MinG =  MAX_FLT, MaxG = -MAX_FLT;

	for (const FFloat16Color& Px : Float16Pixels)
	{
		float R = Px.R.GetFloat();
		float G = Px.G.GetFloat();
		MinR = FMath::Min(MinR, R);  MaxR = FMath::Max(MaxR, R);
		MinG = FMath::Min(MinG, G);  MaxG = FMath::Max(MaxG, G);
	}

	const float RangeR = (MaxR - MinR) > KINDA_SMALL_NUMBER ? (MaxR - MinR) : 1.0f;
	const float RangeG = (MaxG - MinG) > KINDA_SMALL_NUMBER ? (MaxG - MinG) : 1.0f;

	// --- 3. 获取 MeshDescription ---
	if (!InStaticMesh->IsSourceModelValid(0))
	{
		UE_LOG(LogTemp, Error, TEXT("DisplaceMeshByRTBlueChannel: LOD0 source model is not valid."));
		return;
	}

	FMeshDescription* MeshDesc = InStaticMesh->GetMeshDescription(0);
	if (!MeshDesc)
	{
		UE_LOG(LogTemp, Error, TEXT("DisplaceMeshByRTBlueChannel: Failed to get MeshDescription for LOD0."));
		return;
	}

	// 确保顶点色属性已注册（引擎默认 Plane 可能没有）
	FStaticMeshAttributes Attributes(*MeshDesc);
	Attributes.Register();

	TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector2f> VertexUVs = Attributes.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> VertexColors = Attributes.GetVertexInstanceColors();

	const int32 NumVertexInstances = MeshDesc->VertexInstances().Num();

	// 记录已处理的 VertexID，防止同一顶点被多个 Instance 重复位移
	TSet<FVertexID> DisplacedVertices;

	// --- 4. 遍历顶点: Z 位移 + 写入顶点色 ---
	for (int32 i = 0; i < NumVertexInstances; ++i)
	{
		FVertexInstanceID InstanceID(i);
		FVertexID VertexID = MeshDesc->GetVertexInstanceVertex(InstanceID);

		// UV0 -> 像素坐标
		FVector2f UV = VertexUVs.Get(InstanceID, 0);
		int32 PixelX = FMath::Clamp(FMath::FloorToInt32(UV.X * RTWidth),  0, RTWidth  - 1);
		int32 PixelY = FMath::Clamp(FMath::FloorToInt32(UV.Y * RTHeight), 0, RTHeight - 1);

		const FFloat16Color& Pixel = Float16Pixels[PixelY * RTWidth + PixelX];

		// B 通道 -> Z 位移（每个顶点只位移一次）
		if (!DisplacedVertices.Contains(VertexID))
		{
			float BlueValue = Pixel.B.GetFloat();
			FVector3f Pos = VertexPositions[VertexID];
			Pos.Z += BlueValue * DisplaceScale;
			VertexPositions[VertexID] = Pos;
			DisplacedVertices.Add(VertexID);
		}

		// RG 归一化 -> 顶点色 RG，A 直接写入
		float NormR = (Pixel.R.GetFloat() - MinR) / RangeR;
		float NormG = (Pixel.G.GetFloat() - MinG) / RangeG;
		float AlphaValue = Pixel.A.GetFloat();

		FVector4f Color(
			FMath::Clamp(NormR, 0.0f, 1.0f),
			FMath::Clamp(NormG, 0.0f, 1.0f),
			0.0f,
			FMath::Clamp(AlphaValue, 0.0f, 1.0f)
		);
		VertexColors.Set(InstanceID, Color);
	}

	// --- 5. 提交并重建 ---
	InStaticMesh->CommitMeshDescription(0);
	InStaticMesh->Build(false);
	InStaticMesh->PostEditChange();

	UE_LOG(LogTemp, Log, TEXT("DisplaceMeshByRTBlueChannel: Displaced %d verts, wrote vertex colors on '%s'."),
		NumVertexInstances, *InStaticMesh->GetName());
}

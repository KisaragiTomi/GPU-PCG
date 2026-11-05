#pragma once
//
#include "CoreMinimal.h"
#include "DynamicMeshActor.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Engine/DataAsset.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Components/SceneCaptureComponent2D.h"
//
#include "ComputeShaderSceneCapture.generated.h"


USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSMeshData
{
	GENERATED_USTRUCT_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	UStaticMesh* CSStaticMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	UTexture* CSMeshHeight = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	FVector CSMeshPivot = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	float CSMeshMaxHeight = 0;

};


//
// //This struct act as a container for all the parameters that the client needs to pass to the Compute Shader Manager.
UCLASS()
class COMPUTESHADERGENERATOR_API ACSGenerateCaptureScene : public AActor
{
	GENERATED_BODY()
public:
	ACSGenerateCaptureScene();
	UPROPERTY(BlueprintReadWrite, Category = CliffGenerate)
	UBoxComponent* Box;
	USceneComponent* SceneComponent;

	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_SceneDepth;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_SceneNormal;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_ObjectNormal;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_ObjectBaseColor;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_ObjectDepth;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_DebugView;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	UTextureRenderTarget2D* RT_Result;


	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	TArray<UCSMeshAsset*> MeshDataAssets;
	
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	USceneCaptureComponent2D* CaptureObjectDepth;
	UPROPERTY( BlueprintReadWrite, Category = "Capturer")
	USceneCaptureComponent2D* CaptureSceneDepth;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	USceneCaptureComponent2D* CaptureObjNormal;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	USceneCaptureComponent2D* CaptureObjBaseColor;
	UPROPERTY(BlueprintReadWrite, Category = "Capturer")
	USceneCaptureComponent2D* CaptureSceneNormal;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	FName Tag = FName(TEXT("Auto"));
	
	
	virtual void OnConstruction(const FTransform& Transform) override;
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void CaptureAll();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	virtual void CaptureMeshsInBox();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	virtual void CheckTexture();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	virtual  void Generate(int32 NumIteration = 1, float InSpawnSize = 1);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	bool CreateLandscapeMesh();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void Construction();
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float CaptureSize = 2048;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float MaxHeight = 10000;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float Scale3DZ = 100;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float TextureSize = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float GenerateThreshold = .5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Capturer")
	float UnGenerateThreshold = .9;
};

UCLASS(BlueprintType)
class COMPUTESHADERGENERATOR_API UCSMeshAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	UStaticMesh* CSStaticMesh = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	UTexture2D* CSMeshHeightTexture = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	FVector CSMeshPivot = FVector::ZeroVector;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	float CSMeshMaxHeight = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	float CSMeshSize = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	FVector2D RandomHeightOffset = FVector2D(0, 0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	FVector2D RandomRotate = FVector2D(0, 0);
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CSMeshData")
	FVector2D RandomScale = FVector2D(1, 1);
};


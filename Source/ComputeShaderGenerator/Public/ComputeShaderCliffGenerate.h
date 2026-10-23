#pragma once
//
#include "CoreMinimal.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ComputeShaderMeshFill.h"
#include "ComputeShaderSceneCapture.h"
//
#include "ComputeShaderCliffGenerate.generated.h"
//
//
// //This struct act as a container for all the parameters that the client needs to pass to the Compute Shader Manager.



UCLASS()
class COMPUTESHADERGENERATOR_API ACSCliffGenerateCapture : public ACSGenerateCaptureScene
{
	GENERATED_BODY()
public:

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* InMask;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* RT_HeightData;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* RT_HeightNormal;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTexture2DArray* InMeshHeightArray;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* RT_CurrentSceneDepth;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* InConnectivityClassify;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	UTextureRenderTarget2D* RT_TargetHeight;

	

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	float SpawnSize = 1;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = CliffGenerate)
	float RandomRotation = 0;




	ACSCliffGenerateCapture();

	virtual void OnConstruction(const FTransform& Transform) override;
	
	bool IsParameterValidMult()
	{
		bool Check = true;
		if (MeshDataAssets.Num() == 0) Check = false;
		if (RT_HeightNormal == nullptr) Check = false;
		if (InMeshHeightArray == nullptr) Check = false;
		if (RT_HeightData == nullptr) Check = false;
		if (RT_SceneDepth == nullptr) Check = false;
		if (RT_SceneNormal == nullptr) Check = false;
		if (RT_DebugView == nullptr) Check = false;
		if (RT_Result == nullptr) Check = false;
		if (RT_CurrentSceneDepth == nullptr) Check = false;
		return Check;
	}
	virtual  void Generate(int32 NumIteration = 1, float InSpawnSize = 1) override;
	
	void GenerateCliffVerticalCal(TArray<FCSMeshFillData> GenerateDatas);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void GenerateCliffVertical(int32 NumIteration = 1, float InSpawnSize = 1);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void GenerateTargetHeightCal();

	virtual void CheckTexture() override;
	
	
};


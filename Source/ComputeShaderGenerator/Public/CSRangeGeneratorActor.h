#pragma once

// -----------------------------------------------------------------------------
// Range/Instance 生成器 Actor（自 ComputeShaderGenerateHelper.h 拆出，2026-08）。
// 同模块内搬迁不改变 /Script/ComputeShaderGenerator.* 类路径，BP 子类与关卡实例不受影响。
// -----------------------------------------------------------------------------

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Components/BoxComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "ComputeShaderSceneCapture.h"

#include "CSRangeGeneratorActor.generated.h"

class UStaticMesh;

UCLASS()
class COMPUTESHADERGENERATOR_API ACSInstanceContainer : public AActor
{
	GENERATED_BODY()
public:
	
	ACSInstanceContainer(const FObjectInitializer& ObjectInitializer)
	{
		Instances = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Instances"));
	}
	UPROPERTY(BlueprintReadWrite, Category = "CubeAttrib")
	UInstancedStaticMeshComponent* Instances;
};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSRangeGenerator : public AActor
{
	GENERATED_BODY()
public:
	ACSRangeGenerator();

	USceneComponent* SceneComponent;
	
	// UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	UBoxComponent* Box;

	UBoxComponent* CollisionBox;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	UInstancedStaticMeshComponent* Instances;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	FIntVector DivideCount = FIntVector(10, 10, 10);
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float CaptureSize = 1024;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	float MaxDepth = 10000;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 GeneratorCount = 0;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	int32 MultGenerateCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	bool DoGenerate = false;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	UStaticMesh* CollisionMesh;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	TArray<AActor*> ActorsInBox;

	UPROPERTY(BlueprintReadWrite, Category = "ComputeShader")
	TArray<FTransform> StoreCaptureTransforms;

	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ComputeShader")
	TArray<ACSGenerateCaptureScene*> CaptureSceneGenerators;

	virtual void OnConstruction(const FTransform& Transform) override;

	virtual void Tick(float DeltaTime) override;

	virtual void GenerateInternal();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	void Generate();

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	TArray<FTransform> GenerateTransformsCount();

	virtual TArray<FTransform> GenerateTransformsInternal();


};

UCLASS()
class COMPUTESHADERGENERATOR_API ACSBoxRangeGenerator : public ACSRangeGenerator
{
	GENERATED_BODY()
public:
	
	ACSBoxRangeGenerator();

	
	virtual TArray<FTransform> GenerateTransformsInternal() override;
};


UCLASS()
class COMPUTESHADERGENERATOR_API ACSPlaneRangeGenerator : public ACSRangeGenerator
{
	GENERATED_BODY()
public:
	
	ACSPlaneRangeGenerator();

	virtual void Tick(float DeltaTime) override;
	
	virtual TArray<FTransform> GenerateTransformsInternal() override;
};


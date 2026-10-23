// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryEditorActor.h"
#include "FoliageConverter.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UInstancedStaticMeshComponent;
class UPrimitiveComponent;
class UStaticMesh;

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UFoliageConverter : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void ConvertFoliageToTransformArray(UFoliageType* InFoliageType, AVineContainer* Container);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void ConvertTransformArrayToFoliage(UFoliageType* InFoliageType, AVineContainer* Container);

	UFUNCTION(BlueprintPure, Category = FoliageExtra)
	static UStaticMesh* GetStaticMesh(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static UHierarchicalInstancedStaticMeshComponent* FindFoliageInstanceComponent(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta = (ScriptMethod))
	static void GetAllInstancesTransform(UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra, meta = (ScriptMethod))
	static void RefreshFoliage(UFoliageType* InFoliageType);

	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta = (ScriptMethod))
	static UPARAM(DisplayName = "Get Foliage Type") const UFoliageType* GetFoliageType(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintPure, Category = FoliageExtra, meta = (ScriptMethod))
	static const UFoliageType* AddFoliageType(const UFoliageType* InType);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetFoliageInstanceTransform(UStaticMesh* StaticMesh, const int32 InstanceIndex, const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transform);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void DistanceSort(TMap<float, int32> InMap, TMap<float, int32>& OutMap);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32 BaseID);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool GetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32& BaseID);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool GetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float& CustomDataValue);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static bool SetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue);

	UFUNCTION(BlueprintCallable, Category = FoliageExtra)
	static void AddFoliage();
};

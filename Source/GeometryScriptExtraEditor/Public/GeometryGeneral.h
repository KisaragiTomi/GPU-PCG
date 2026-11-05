// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "DynamicMeshEditor.h"
#include "UDynamicMesh.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"

class UStaticMesh;
class AActor;
class FSceneView;
class UMeshComponent;

#include "GeometryGeneral.generated.h"

/**
 * 
 */

struct FDynamicMeshComponentData
{
public:
	int Class = -1;
	int ParentClass= -1;
			
	TArray<int> TIDs;
	TArray<FVector> VPoss;

};


struct FDynamicMeshComponentReduceData
{
public:
	float Count = 0;
	float MaxDist = -TNumericLimits<float>::Max();
	float MinDist = TNumericLimits<float>::Max();
	int ParentClass = -1;

	virtual bool  CollectedData(FDynamicMesh3& EditMesh, int TID, float Dist) = 0;
};

struct FWindTreeCombineLeafData :public FDynamicMeshComponentReduceData
{
public:

	virtual bool  CollectedData(FDynamicMesh3& EditMesh, int TID, float InDist)
	{
		if (InDist < MinDist)
		{
			MinDist = fmin(MinDist, InDist);
			Count += 1;
			return true;
		}
		return false;
	}
};

using namespace UE::Geometry;

UCLASS()
class GEOMETRYSCRIPTEXTRAEDITOR_API UGeometryGeneral : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration = 5, bool RecomputeNormals = true);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormals(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh);
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* PrimNormal(UDynamicMesh* TargetMesh, FVector TestPos, FVector& OutVector);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static void WindDataForTree(UDynamicMesh* TargetMesh, TArray<FLinearColor>& PivotIndexData, TArray<FLinearColor>& DirExtentData, int32& tXx, int32
	                            & tXy, TArray<FVector>& OutHolePositions, TMap<int, FVector>& DebugClassNum, int LeafMaterialIndex = -1, float
	                            CombineDistThreshold = 50, float FindParentThreshold = 5, bool OutDebugColor = false);
	

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* WeldDynamicMesh(UDynamicMesh* TargetMesh, float Tolerance = 0.001);
	
	static void WeldVertices(FDynamicMesh3& EditMesh, float Tolerance);


	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* ColorAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success, FLinearColor ChannelMask = FLinearColor(1, 1, 1, 1));
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* UVAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success, int UVNum = 1, FLinearColor ChannelMask = FLinearColor(1, 1, 1, 1));
	
	
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* FillUVData(UDynamicMesh* TargetMesh, int32 UVLayerNum);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static UDynamicMesh* AddCustomAttribute(UDynamicMesh* TargetMesh, FName AttributeName);

	UFUNCTION(BlueprintCallable, Category = Generate)
	static void CalculateOBBUpDir(TArray<FVector> OrientSpaceVertices, FVector& BoxUpDir);

	/** 材质槽从 MaterialSource 组件按网格里出现过的最大材质 ID 逐槽取；其余与下面那个重载一致。 */
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UStaticMesh* SaveDynamicMeshToStaticMesh(
		UDynamicMesh* TargetMesh,
		const FString& AssetPathAndName,
		UMeshComponent* MaterialSource = nullptr,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = false,
		bool bMarkPackageDirty = true);

	/**
	 * DynamicMesh 落盘成 StaticMesh 资产的唯一实现：建目录、按需删除已有资产、拷贝网格、
	 * 建资产、标脏、可选写盘。材质槽由调用方直接给。
	 *
	 * UGeometryEditorFunction::CreateStaticMeshAsset 曾经自己拼一份 FStaticMeshAssetOptions，
	 * 缺了建目录/覆盖/标脏/落盘，还带三个缺陷（TargetMesh 为空直接解引用、不检查返回码就用
	 * ResultData.StaticMesh、一次没有配对 BeginTransaction 的 EndTransaction）。它现在转调这里。
	 */
	UFUNCTION(BlueprintCallable, Category = Generate)
	static UStaticMesh* SaveDynamicMeshToStaticMeshWithMaterials(
		UDynamicMesh* TargetMesh,
		const FString& AssetPathAndName,
		const TArray<UMaterialInterface*>& Materials,
		bool bReplaceExistingAsset = true,
		bool bSaveAsset = false,
		bool bMarkPackageDirty = true);

	static FVector GetNearestLocationNormal(FDynamicMesh3& EditMesh, FGeometryScriptTrianglePoint NearestPoint);


	
	
	
	static void AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>());

	UFUNCTION(BlueprintCallable, Category = "DistanceField")
	static bool HasMeshDistanceField(UStaticMesh* StaticMesh);

	UFUNCTION(BlueprintCallable, Category = "DistanceField")
	static FBox GetMeshDistanceFieldBounds(UStaticMesh* StaticMesh);
};

#pragma once

#include "ComputeShaderMeshGenerator.h"
#include "MeshGeneratorBrushCache.generated.h"

UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AMeshGeneratorBrushCache : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()

public:
	// -------------------------------------------------------------------------
	// Brush System
	// -------------------------------------------------------------------------

	static FCSInstanceBrushEditorRequest OnInstanceBrushEditorRequest;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> InstanceBrushMesh = nullptr;

	float InstanceBrushRadius = 500.0f;

	int32 InstanceBrushSamplesPerMouseMove = 16;

	float InstanceBrushMinSpacing = 100.0f;

	float InstanceBrushTraceRadius = 0.0f;

	float InstanceBrushPreviewPointSize = 8.0f;

	float InstanceBrushPreviewLifetime = 0.1f;

	bool bInstanceBrushAlignToNormal = true;

	bool bInstanceBrushUseGeneratorBounds = true;

	bool bInstanceBrushExitAfterCommit = false;

	FVector2D InstanceBrushUniformScaleRange = FVector2D(1.0f, 1.0f);

	float InstanceBrushRandomYawDegrees = 360.0f;

	TArray<FCSInstancePaintComponentSlot> PaintedInstanceComponents;

	/** Opens the editor-side instance brush tool for this generator. */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Mesh Generator|Instance Brush", meta = (DevelopmentOnly))
	void StartInstanceBrush();

	/** Finds or creates the HISM component used to store painted instances for the given mesh. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Instance Brush")
	UHierarchicalInstancedStaticMeshComponent* GetOrCreatePaintComponent(UStaticMesh* Mesh);

	/** Returns the existing painted-instance component for a mesh, or nullptr if none exists. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Instance Brush")
	UHierarchicalInstancedStaticMeshComponent* FindPaintComponent(UStaticMesh* Mesh) const;

	/** Appends world-space instance transforms to the paint component for Mesh and returns the added count. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Instance Brush")
	int32 CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh);

	/** Tests whether a brush placement point is inside the generator bounds when bounds filtering is enabled. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Instance Brush")
	bool IsInstanceBrushPointAllowed(const FVector& WorldPosition) const;
};

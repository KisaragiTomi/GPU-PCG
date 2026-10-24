#pragma once

#include "ComputeShaderMeshGenerator.h"
#include "MeshGeneratorBrushCache.generated.h"

UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API AMeshGeneratorBrushCache : public AComputeShaderMeshGenerator
{
	GENERATED_BODY()

public:
	// -------------------------------------------------------------------------
	// Dirty Cache System — Render Targets
	// -------------------------------------------------------------------------

	// 这三张 RT 由 NewObject 现建（CreateCacheRenderTargets），Outer 不保活：
	// 必须是 UPROPERTY 才能让 GC 看见它们，否则下一次 GC 就会被回收而指针还在用。
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> VoxelMetaRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> TriangleVertexRT;

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> TriangleNormalRT;

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

	// -------------------------------------------------------------------------
	// Dirty Cache System — Public API
	// -------------------------------------------------------------------------

	/** Ensures the triangle cache covers the request's active cells and refreshes dirty pages as needed. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCache(const FCSMeshGeneratorTriangleCacheRequest& Request);

	/** Ensures a default bounds-based cache request using the current GeneratorBounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(
		FName RequestId,
		bool bForceFullRebuild = false);

	/** Ensures a bounds-based cache request using an explicit box center and extent. */
	virtual FCSMeshGeneratorTriangleCacheHandle EnsureTriangleCacheByBox(
		FName RequestId,
		const FVector& BoxCenter,
		const FVector& BoxExtent,
		bool bForceFullRebuild = false);

	/** Refreshes the default GeneratorBounds-based cache request. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void UpdateMeshGeneratorCacheByBox(bool bForceFullRebuild = false);

	/** Removes a persistent cache interest request and frees cells no longer needed by any request. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void ReleaseTriangleCacheRequest(FName RequestId);

	/** Clears all cache requests, active cells, dirty state, and GPU cache render targets. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void ClearMeshGeneratorCache();

	/** Marks every currently active voxel page dirty so the next update rewrites cached triangle data. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache")
	virtual void MarkAllActiveVoxelsDirty();

	/** Returns a lightweight summary of the current triangle-cache state. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Triangle Cache")
	FCSMeshGeneratorTriangleCacheHandle GetTriangleCacheHandle() const;

	/** Returns the world-space bounds currently covered by the triangle cache. */
	UFUNCTION(BlueprintPure, Category = "CS Mesh Generator|Triangle Cache")
	FBox GetCachedWorldBounds() const { return CacheState.CachedWorldBounds; }

	/** Draws active cache voxel cells, optionally limited to one request and including the cache bounds. */
	UFUNCTION(BlueprintCallable, Category = "CS Mesh Generator|Triangle Cache|Debug", meta = (DevelopmentOnly))
	int32 DrawDebugActiveVoxels(
		const FCSDebugActiveVoxelOptions& Options = FCSDebugActiveVoxelOptions()) const;

protected:
	// -------------------------------------------------------------------------
	// Lifecycle
	// -------------------------------------------------------------------------

	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	// -------------------------------------------------------------------------
	// Dirty Cache System — Internals
	// -------------------------------------------------------------------------

	virtual bool DoesInputRequireFullRebuild(const FBox& InputWorldBounds) const;
	virtual void RebuildCacheResources(const FBox& InputWorldBounds);
	virtual void BuildActiveCellsFromReferencePoints(float ActivationRadius, TSet<FIntVector>& OutCells) const;
	virtual void BuildActiveCellsFromReferencePoints(const TArray<FVector>& InReferencePoints, float ActivationRadius, TSet<FIntVector>& OutCells) const;
	virtual void BuildUnionActiveCells(TSet<FIntVector>& OutCells) const;
	virtual void DiffActiveCells(const TSet<FIntVector>& NewActiveCells);
	virtual void AllocatePagesForCells(const TSet<FIntVector>& Cells);
	virtual void ReleasePagesForCells(const TSet<FIntVector>& Cells);
	virtual void DispatchDirtyVoxelTriangleCacheUpdate();

	FIntVector ComputeGridSize(const FBox& InputWorldBounds) const;
	FIntVector WorldPositionToCell(FVector WorldPosition) const;
	FBox GetCellWorldBounds(const FIntVector& Cell) const;
	void ReleaseCacheResources();
	void ResetCacheRuntime(bool bClearRequests);
	void InitializeFreePages();
	void CreateCacheRenderTargets();
	void RebuildRequestActiveCellsFromLastRequests();
	bool HasValidCacheResources() const;
	bool AreBoundsCompatible(const FBox& A, const FBox& B) const;
	FName NormalizeRequestId(FName RequestId) const;

	FCSMeshGeneratorVoxelCacheState CacheState;

	TMap<FName, TSet<FIntVector>> RequestActiveCells;
	TMap<FName, FCSMeshGeneratorTriangleCacheRequest> LastRequests;
};

// A Cities: Skylines II road network built in UE: compositions on the CPU, then the GeometrySystem
// jobs, the composition parameters, RoadLab's junction seams and node refinement and the mesh
// deformation on the GPU, straight into a UCSMesh; the lane markings and crosswalks (RoadLab's
// decals) are projected onto that surface into a second one.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "CSRoadNetworkActor.generated.h"

class FRHIGPUBufferReadback;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
namespace CSRoad
{
class FAssets;
struct FGpuDecalInput;
struct FPooledDecalReceivers;
struct FGpuRoadMeshInput;
}

UCLASS()
class CSROADNET_API ACSRoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadNetworkActor();

	/** Road snapshot (format cs2-road-network-v1), e.g. CitiesSkylines-Road2-C#
	 *  Artifacts/public-saves/palmsland-3/network.json. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (FilePathFilter = "json"))
	FFilePath NetworkFile;

	/** prefabs.json, raw-prefabs.json and pieces.bin. Empty: <CS2ROADLAB or D:/MyProject/CitiesSkylines-Road2-C#>/Assets/CS2. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	FDirectoryPath AssetDirectory;

	/** Clamp terrain-following vertices to the snapshot's heightmaps. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	bool bUseTerrain = true;

	/** Build when the actor is loaded or duplicated (the resident mesh is never saved). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	bool bBuildOnLoad = true;

	/** Searched for M_CS2_<NN>_<BaseColor> materials (Scripts/ImportCS2Roads.py). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ContentDir))
	FDirectoryPath MaterialFolder;

	/** Used for atlases with no material in MaterialFolder; null draws the engine default. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	TObjectPtr<UMaterialInterface> FallbackMaterial;

	/** Lane markings, tire tracks and crosswalks: RoadLab's decals, drawn with the M_CS2 decal materials. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	bool bBuildMarkings = true;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UCSMeshRenderComponent> RoadMesh;

	/** The decals; no shadows, kept out of ray tracing (they float 2 cm over the road). */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UCSMeshRenderComponent> MarkingMesh;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumRoads = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumVertices = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTriangles = 0;

	/** Decal triangles written, dropped (off-surface) grid cells included as degenerate ones. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumMarkingTriangles = 0;

	/** Whole rebuild, game thread: file loads, compositions, GPU passes and the flush. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	float BuildMilliseconds = 0.0f;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	TArray<FString> MissingAtlases;

	/** Decal surfaces with no material in MaterialFolder (drawn with FallbackMaterial). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	TArray<FString> MissingMarkingSurfaces;

	/** Why the last rebuild failed. When only the decals failed (say, too little VRAM), the roads are still built. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	FString LastError;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "CS2 Road")
	void Rebuild();

	virtual void PostRegisterAllComponents() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> Markings;

	FString ResolveAssetDirectory() const;
	bool BuildRoads(FString& OutError);
	// The decal mesh: reads back the crosswalk clip counts, lays the decals out and draws them onto
	// the road mesh just built (its receivers gathered into Receivers).
	bool BuildMarkingMesh(CSRoad::FGpuDecalInput& Decals, const CSRoad::FGpuRoadMeshInput& MeshInput, const TMap<FString, UMaterialInterface*>& ByBaseColor,
		const CSRoad::FAssets& Assets, const CSRoad::FPooledDecalReceivers& Receivers, FRHIGPUBufferReadback* ClipReadback,
		const FMatrix44f& LocalToWorld, const FMatrix44f& WorldToLocal, const FBox& WorldBounds, FString& OutError);
};

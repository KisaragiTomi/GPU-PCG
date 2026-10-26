// A Cities: Skylines II road network built in UE: compositions on the CPU, then the GeometrySystem
// jobs, the composition parameters, RoadLab's junction seams and node refinement and the mesh
// deformation on the GPU, straight into a UCSMesh; the lane markings and crosswalks (RoadLab's
// decals) are projected onto that surface into a second one. The roads come from a snapshot file
// or from spline roads (ACSRoadSplineActor), which a snapshot can also be converted into.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "CSRoadNetworkActor.generated.h"

class FRHIGPUBufferReadback;
class ULevel;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
namespace CSRoad
{
class FAssets;
struct FGpuDecalInput;
struct FPooledDecalReceivers;
struct FGpuRoadMeshInput;
struct FSplineRoad;
}

UENUM()
enum class ECSRoadNetworkSource : uint8
{
	/** The roads of NetworkFile. */
	NetworkFile,
	/** Spline roads: ACSRoadSplineActors hanging under this actor (or, when it is the level's only network building from
	 *  splines, every one in the level) and spline components on it. NetworkFile, if set, only lends its terrain. */
	Splines,
};

USTRUCT()
struct FCSRoadSplineSettings
{
	GENERATED_BODY()

	/** Longer segments are split (0: never). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float MaxSegmentLength = 96.0f;

	/** Segments whose heading changes by more are split (0: never). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", ClampMax = "180", Units = "deg"))
	float MaxSegmentTurn = 30.0f;

	/** Segments whose ends are closer are merged into their neighbour where the point between them is a plain bend of
	 *  one spline; splits never go below it. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float MinSegmentLength = 8.0f;

	/** Spline points this close share a node, and a road ending this close to another joins it. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0.01", Units = "m"))
	float SnapDistance = 1.0f;

	/** Roads crossing at most this far apart in height meet in a junction; farther, one passes over the other. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float JunctionHeight = 2.0f;
};

UCLASS()
class CSROADNET_API ACSRoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadNetworkActor();

	/** Where the roads come from. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	ECSRoadNetworkSource Source = ECSRoadNetworkSource::NetworkFile;

	/** Road snapshot (format cs2-road-network-v1), e.g. CitiesSkylines-Road2-C#
	 *  Artifacts/public-saves/palmsland-3/network.json. Building from splines, only its terrain is used. */
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

	/** The prefab of spline components that are not UCSRoadSplineComponents. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines", meta = (GetOptions = "GetRoadPrefabOptions"))
	FString DefaultSplinePrefab = TEXT("Small Road");

	/** Traffic drives on the left (a snapshot says so itself). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines")
	bool bLeftHandTraffic = false;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines")
	FCSRoadSplineSettings SplineSettings;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UCSMeshRenderComponent> RoadMesh;

	/** The decals; no shadows, kept out of ray tracing (they float 2 cm over the road). */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road")
	TObjectPtr<UCSMeshRenderComponent> MarkingMesh;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumRoads = 0;

	/** Spline roads the last build took. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumSplines = 0;

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

	/** NetworkFile's roads as ACSRoadSplineActors under this actor, one per run of edges where the road goes on, with
	 *  every node's and edge's data, so they build into the same roads; then builds from them. Undoable. */
	UFUNCTION(CallInEditor, BlueprintCallable, Category = "CS2 Road|Splines")
	void ConvertToSplines();

	// Rebuilds on the next tick: spline roads call it as they change, many times a frame.
	void RequestRebuild();

	// The road prefabs of the network's asset directory (the default one without a network).
	static TArray<FString> RoadPrefabNames(const ACSRoadNetworkActor* Network);
	// The level's network building from splines if it has exactly one, which also takes the level's
	// spline roads that hang under no network.
	static ACSRoadNetworkActor* LevelSplineNetwork(const ULevel* Level);

	UFUNCTION()
	TArray<FString> GetRoadPrefabOptions() const;

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
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

	bool bRebuildRequested = false;

	FString ResolveAssetDirectory() const;
	// The spline roads this network builds, in its space (Unity metres).
	TArray<CSRoad::FSplineRoad> GatherSplines() const;
	bool BuildRoads(FString& OutError);
	// The decal mesh: reads back the crosswalk clip counts, lays the decals out and draws them onto
	// the road mesh just built (its receivers gathered into Receivers).
	bool BuildMarkingMesh(CSRoad::FGpuDecalInput& Decals, const CSRoad::FGpuRoadMeshInput& MeshInput, const TMap<FString, UMaterialInterface*>& ByBaseColor,
		const CSRoad::FAssets& Assets, const CSRoad::FPooledDecalReceivers& Receivers, FRHIGPUBufferReadback* ClipReadback,
		const FMatrix44f& LocalToWorld, const FMatrix44f& WorldToLocal, const FBox& WorldBounds, FString& OutError);
};

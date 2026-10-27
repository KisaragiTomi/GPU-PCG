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
class UInstancedStaticMeshComponent;
class UStaticMesh;
struct FCSRoadNetworkPreview;
namespace CSRoad
{
class FAssets;
struct FGpuDecalInput;
struct FPooledDecalReceivers;
struct FGpuRoadMeshInput;
struct FRoadSpec;
struct FSplineRoad;
class FGraph;
class FRoadLanes;
class FTileState;
struct FGpuGeometryInput;
struct FGpuGeometryResult;
struct FSavedTerrain;
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

	/** Alleys are cut back this far from a main road's edge, past their own half width. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float AlleyClearance = 1.0f;
};

/** How the batch buildings take the plots along the roads. */
USTRUCT()
struct FCSCityLotSettings
{
	GENERATED_BODY()

	/** Along the road per plot, of which Gap stays free between neighbours. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "4", Units = "m"))
	float Frontage = 16.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float Gap = 2.0f;

	/** From the road's edge to the plot. Plots keep half of it from every other road too. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "0", Units = "m"))
	float Setback = 3.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "2", Units = "m"))
	float MinDepth = 10.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "2", Units = "m"))
	float MaxDepth = 18.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "1", Units = "m"))
	float MinHeight = 6.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (ClampMin = "1", Units = "m"))
	float MaxHeight = 24.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	int32 Seed = 1;
};

/** The batch buildings of one tile. */
USTRUCT()
struct FCSCityBuildingTile
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> Component;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> Mesh;

	// What its buildings came out as: equal hashes, same buildings.
	uint64 Hash = 0;
	int32 Buildings = 0;
};

/** One tile of a network built in tiles: the road and decal meshes of the roads whose middle lies in it. */
USTRUCT()
struct FCSRoadTile
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> Road;

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> Markings;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> RoadMesh;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> MarkingMesh;

	int32 Roads = 0;
	int32 Vertices = 0;
	int32 Triangles = 0;
	int32 MarkingTriangles = 0;
};

UCLASS()
class CSROADNET_API ACSRoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadNetworkActor();
	virtual ~ACSRoadNetworkActor() override;

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

	/** Draw the roads in square tiles of TileSize and, after an edit, rebuild only the tiles whose roads came out
	 *  different, or meet such a road (or a removed one) at a node. The graph and the GPU geometry still cover
	 *  the whole network, which is what tells the changed roads apart; refinement, deformation, decals and the
	 *  mesh writes run for the rebuilt tiles only. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update")
	bool bLocalUpdate = false;

	/** A road belongs to the tile its curve's middle lies in. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (ClampMin = "16", Units = "m", EditCondition = "bLocalUpdate"))
	float TileSize = 256.0f;

	/** Road tiles, and batch building tiles, a rebuild writes at most (0: no limit). The rest wait for the next ticks, a
	 *  batch a tick, the tiles the latest edit changed first. Everything a rebuild writes goes up in one frame, and the
	 *  upload memory and pooled buffers it takes come back only after the frame: the limit bounds them. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (ClampMin = "0"))
	int32 MaxTilesPerBatch = 16;

	/** Tiles with roads. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTiles = 0;

	/** Tiles the last rebuild wrote or removed. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 TilesRebuilt = 0;

	/** Road and building tiles left for the next batches. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 PendingTiles = 0;

	/** Roads drawn in the tiles the last rebuild wrote. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 RoadsRebuilt = 0;

	/** Fill the plots along the roads with buildings (convex hulls pushed up), clear of the roads, of each other and
	 *  of the plots ACSCityBuildingActors reserve. A plot belongs to the spline it faces (its segment, side and place
	 *  along it), so editing one road leaves the buildings along the others as they were. Drawn in tiles of
	 *  TileSize; a rebuild rewrites only the tiles whose buildings changed. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Buildings")
	bool bBatchBuildings = false;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Buildings", meta = (EditCondition = "bBatchBuildings"))
	FCSCityLotSettings Lots;

	/** Empty: /PCGPlugins/MeshBoolean/M_VertexColor. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Buildings", meta = (EditCondition = "bBatchBuildings"))
	TObjectPtr<UMaterialInterface> BuildingMaterial;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumBuildings = 0;

	/** Building tiles the last rebuild wrote or removed. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 BuildingTilesRebuilt = 0;

	/** Show and animate passenger cars. Switching this off pauses the preview without rebuilding roads. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, BlueprintSetter = SetShowTraffic, Category = "CS2 Road|Traffic", meta = (DisplayName = "Show Traffic"))
	bool bShowTraffic = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic", meta = (ClampMin = "0", ClampMax = "4096"))
	int32 TrafficVehicleCount = 256;

	/** Upper speed limit for the preview; junction connectors also impose a turn speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic", meta = (ClampMin = "0", ClampMax = "198", Units = "km/h"))
	float TrafficSpeed = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic", meta = (ClampMin = "0.5", ClampMax = "20", Units = "m"))
	float TrafficGap = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic")
	int32 TrafficSeed = 12345;

	/** Preview signal phases for nodes marked with traffic lights. Conflict reservations always apply. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic")
	bool bTrafficSignals = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS2 Road|Traffic")
	bool bTrafficInEditor = true;

	/** Optional +X-forward vehicle mesh, fitted to 4.2 x 1.8 x 1.5 m. Empty uses a simple body/cabin proxy. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Traffic")
	TObjectPtr<UStaticMesh> TrafficVehicleMesh;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Traffic")
	TObjectPtr<UMaterialInterface> TrafficMaterial;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge", meta = (DisplayName = "Build Bridge Piers"))
	bool bBuildBridgePiers = true;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge", meta = (ClampMin = "8", ClampMax = "100", Units = "m"))
	float BridgePierSpacing = 30.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge", meta = (ClampMin = "1", Units = "m"))
	float BridgePierMinHeight = 3.0f;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge", meta = (ClampMin = "0.5", ClampMax = "5", Units = "m"))
	float BridgePierWidth = 1.5f;

	/** Optional visible-world ground trace, falling back to snapshot terrain or local Z=0. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge")
	bool bTraceBridgeGround = true;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Bridge")
	TObjectPtr<UMaterialInterface> BridgePierMaterial;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road|Traffic")
	TObjectPtr<UInstancedStaticMeshComponent> TrafficBodies;

	UPROPERTY(VisibleAnywhere, Category = "CS2 Road|Traffic")
	TObjectPtr<UInstancedStaticMeshComponent> TrafficCabins;

	/** Columns, caps and footings, generated as one instanced component. */
	UPROPERTY(VisibleAnywhere, Category = "CS2 Road|Bridge")
	TObjectPtr<UInstancedStaticMeshComponent> BridgePiers;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTrafficVehicles = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTrafficLanes = 0;

	/** The existing lane builder does not yet connect roundabouts. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 UnsupportedTrafficRoundabouts = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumBridgePiers = 0;

	/** Candidate spans with insufficient height or no safe foundation position. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 SkippedBridgePiers = 0;

	UFUNCTION(BlueprintCallable, BlueprintSetter, Category = "CS2 Road|Traffic")
	void SetShowTraffic(bool bShow);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "CS2 Road|Traffic")
	void ResetTraffic();

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
	// The network an actor of the layout (a spline road, a building, an alley region) belongs to: the
	// network it hangs under, else its level's only network building from splines.
	static ACSRoadNetworkActor* NetworkOf(const AActor* Actor);

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
	TUniquePtr<FCSRoadNetworkPreview> Preview;
	void BuildPreview(const CSRoad::FGraph& Graph, const CSRoad::FRoadLanes& Lanes, const CSRoad::FGpuGeometryResult& Geometry, const CSRoad::FSavedTerrain* Terrain);
	void TickPreview(float DeltaSeconds);
	void RenderTraffic();
	void ClearPreview();

	// Tiles by (column, row) of the network's ground plane, when it builds in tiles. Kept out of undo:
	// the tiles are rebuilt outside transactions, so a restored map would name components gone since.
	UPROPERTY(Transient, NonTransactional)
	TMap<FIntPoint, FCSRoadTile> Tiles;

	TUniquePtr<CSRoad::FTileState> TileState;

	// Where one road mesh and its decal mesh go, and what came of them.
	struct FMeshTargets
	{
		UCSMeshRenderComponent* Road = nullptr;
		TObjectPtr<UCSMesh>* RoadMesh = nullptr;
		UCSMeshRenderComponent* Markings = nullptr;
		TObjectPtr<UCSMesh>* MarkingMesh = nullptr;
		int32 MarkingTriangles = 0;
		// Set when only the decals failed: the road mesh stands.
		FString MarkingError;
		double AllocationMs = 0.0;
		double RoadMs = 0.0;
		double MarkingMs = 0.0;
	};

	FString ResolveAssetDirectory() const;
	// The spline roads this network builds, in its space (Unity metres).
	TArray<CSRoad::FSplineRoad> GatherSplines() const;
	bool BuildRoads(FString& OutError);
	// BuildRoads in tiles: the graph's geometry decides which tiles to write (CSRoadTiles.h).
	bool BuildTiles(CSRoad::FAssets& Assets, const CSRoad::FGraph& Graph, const CSRoad::FGpuGeometryInput& Geometry, const TMap<FString, UMaterialInterface*>& ByBaseColor,
		const CSRoad::FSavedTerrain* Terrain, bool bMarkings, FString& OutError);
	// Allocates the targets and records the road mesh (and the decals, when Decals has items) into them.
	bool WriteMeshes(const CSRoad::FGpuGeometryInput& Geometry, const CSRoad::FGpuRoadMeshInput& MeshInput, CSRoad::FGpuDecalInput* Decals,
		const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FAssets& Assets, FMeshTargets& Targets, FString& OutError);
	// The decal mesh: reads back the crosswalk clip counts, lays the decals out and draws them onto
	// the road mesh just built (its receivers gathered into Receivers).
	bool BuildMarkingMesh(CSRoad::FGpuDecalInput& Decals, const CSRoad::FGpuRoadMeshInput& MeshInput, const TMap<FString, UMaterialInterface*>& ByBaseColor,
		const CSRoad::FAssets& Assets, const CSRoad::FPooledDecalReceivers& Receivers, FRHIGPUBufferReadback* ClipReadback, UCSMesh* RoadObject, FMeshTargets& Targets,
		const FMatrix44f& LocalToWorld, const FMatrix44f& WorldToLocal, const FBox& WorldBounds, FString& OutError);
	// The tile's components, made when it first gets roads.
	FCSRoadTile& EnsureTile(const FIntPoint& Key);
	void RemoveTile(const FIntPoint& Key);
	// Every tile gone and forgotten (building whole again, or the settings changed under them).
	void ClearTiles();
	// What the tiles' meshes depend on besides the roads.
	uint64 TileSignature(bool bLeftHand) const;

	// As Tiles, by the same (column, row).
	UPROPERTY(Transient, NonTransactional)
	TMap<FIntPoint, FCSCityBuildingTile> BuildingTiles;

	// Building tiles whose buildings changed but were left for a later batch.
	int32 PendingBuildingTiles = 0;

	// The batch buildings along the roads as built (Roads, the network's space). The plots face the
	// segments of Splines, or building from a snapshot the roads themselves.
	void BuildBatchBuildings(const TArray<CSRoad::FSplineRoad>& Splines, const TArray<CSRoad::FRoadSpec>& Roads, CSRoad::FAssets& Assets);
	void ClearBuildingTiles();
};

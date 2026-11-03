// A Cities: Skylines II road network built in UE: compositions on the CPU, then the GeometrySystem
// jobs, the composition parameters, RoadLab's junction seams and node refinement and the mesh
// deformation on the GPU, straight into a UCSMesh; the lane markings and crosswalks (RoadLab's
// decals) are instanced mesh decals laid along their curves (CSRoadDecalInstances.h). The roads are
// spline roads (ACSRoadSplineActor); a snapshot file lends its terrain, and its roads can be
// converted into spline roads (UCSRoadNetTools).
//
// The actor is the network's manager: its settings, operations and stats. What it builds goes into an
// ACSRoadNetworkContent of its own, so selecting it stays quick however big the network is.
#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/Actor.h"
#include "CSRoadNetworkActor.generated.h"

class ACSRoadNetworkContent;
class UCSRoadTerrainRenderer;
class ULevel;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UInstancedStaticMeshComponent;
class UStaticMesh;
class UStaticMeshComponent;
struct FCSRoadNetworkPreview;
struct FCSRoadNetworkActor_SplineCache;
struct FCSRoadNetworkActor_TileJob;
namespace CSRoad
{
class FAssets;
class FDecalFitReadback;
class FRoadBake;
struct FGpuDecalInput;
struct FGpuRoadMeshInput;
struct FPooledGeometry;
struct FRoadSpec;
struct FSavedNetwork;
struct FSavedNode;
struct FSplineRoad;
class FGraph;
class FRoadLanes;
class FTileState;
struct FGpuGeometryInput;
struct FGpuGeometryResult;
struct FSavedTerrain;
struct FTerrainFootprint;
}
namespace CSCity
{
struct FLotState;
}

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

/** One tile of a network built in tiles: the road mesh and the decals of the roads whose middle lies in it. */
USTRUCT()
struct FCSRoadTile
{
	GENERATED_BODY()

	UPROPERTY(Transient)
	TObjectPtr<UCSMeshRenderComponent> Road;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> RoadMesh;

	/** The decals as instanced mesh decals, one component per material slot and priority. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UInstancedStaticMeshComponent>> Decals;

	// The heights the decals of each write since the last drawn ones are fitted to, oldest first: the
	// latest to land draws, and those before it go.
	TArray<TSharedPtr<CSRoad::FDecalFitReadback>> DecalFits;

	/** The road baked into a Nanite mesh (bBakeNanite): drawn in place of RoadMesh, which is then freed. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> Nanite;

	/** What a bake in progress builds. */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> Baking;

	TSharedPtr<CSRoad::FRoadBake> Bake;

	int32 Roads = 0;
	int32 Vertices = 0;
	int32 Triangles = 0;
	int32 DecalInstances = 0;
	// When RoadMesh was last written: a bake starts once it has stood a moment.
	double WrittenSeconds = 0.0;
	// RoadMesh as written could not be baked: it stays until written again.
	bool bBakeFailed = false;
};

UCLASS()
class CSROADNET_API ACSRoadNetworkActor : public AActor
{
	GENERATED_BODY()

public:
	ACSRoadNetworkActor();
	virtual ~ACSRoadNetworkActor() override;
	/** Declared here so UHT does not define it in the .gen.cpp: that constructor destroys the TUniquePtr members on
	 *  unwind, and FTileState / FCSRoadNetworkPreview are incomplete there unless the unity blob happens to define them
	 *  first (C4150 once the blob order changes). Defined in the .cpp next to the destructor. */
	ACSRoadNetworkActor(FVTableHelper& Helper);

	/** Road snapshot (format cs2-road-network-v1), its heightmaps beside it; a relative path starts in the plugin's
	 *  Data/CS2Road, e.g. Palmsland/network.json. It lends its terrain; UCSRoadNetTools turns its roads into
	 *  spline roads and its terrain into a Landscape. The roads built are the spline roads: ACSRoadSplineActors hanging
	 *  under this actor (or, when it is the level's only network, every one in the level) and spline components on it. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (FilePathFilter = "json"))
	FFilePath NetworkFile;

	/** prefabs.json, raw-prefabs.json and pieces.bin. Empty: the plugin's Data/CS2Road/Prefabs, or <CS2ROADLAB>/Assets/CS2
	 *  when that environment variable names a CitiesSkylines-Road2-C# checkout. */
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

	/** Lane markings, tire tracks and crosswalks: RoadLab's decals, drawn as instanced mesh decals with DecalMaterial
	 *  and the albedo and tint of each surface's M_CS2 material. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road")
	bool bBuildMarkings = true;

	/** Lays a unit strip along a lane decal's curve (Scripts/CS2Road/MakeRoadDecalMaterial.py). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (EditCondition = "bBuildMarkings"))
	TSoftObjectPtr<UMaterialInterface> DecalMaterial;

	/** Lays a unit grid on the surface along a junction connector's or crosswalk's curve, at the heights found for it. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road", meta = (EditCondition = "bBuildMarkings"))
	TSoftObjectPtr<UMaterialInterface> FittedDecalMaterial;

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

	/** Bake each road tile into a Nanite mesh once it has stood unchanged for a moment, and draw that in place of its GPU
	 *  mesh, which is then freed (editor only). A tile an edit rewrites draws its GPU mesh again until the new bake is in. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (EditCondition = "bLocalUpdate"))
	bool bBakeNanite = true;

	/** Video memory the GPU meshes of the tiles not yet baked may hold (MiB): past it, tiles wait to be built until bakes
	 *  free theirs. Counts their streams as the pools hand them out and their ray tracing BLAS. 0: no limit. Nothing is
	 *  baked while a mouse button is held (an edit dragged, a slider, the camera flown), so a long drag can fill it: the
	 *  tiles then wait for the button to come up. Bakes go nearest the camera first. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (ClampMin = "0", DisplayName = "GPU Mesh Budget (MiB)", EditCondition = "bLocalUpdate && bBakeNanite"))
	int32 GpuMeshBudget = 200;

	/** While the GPU has less video memory than this left (MiB, as its driver budgets this process, other programs' use
	 *  included), the network stops updating as edits come: no tile, road or building, is written, and nothing fails;
	 *  they wait, nearest the camera first, until memory comes back (bakes, which free it, go on). 0: never. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (ClampMin = "0", DisplayName = "Min Free Video Memory (MiB)", EditCondition = "bLocalUpdate"))
	int32 MinFreeVideoMemory = 512;

	/** After an edit, convert only the splines around the ones that changed into the network, taking the rest as the last
	 *  conversion left it; the network comes out as converting every spline makes it. Other prefab data, settings or
	 *  terrain, splines without an id of their own, or a change reaching half the splines convert them all. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update")
	bool bIncrementalSplines = true;

	/** Convert every spline as well after each conversion around a change, and compare: a difference is logged with the
	 *  first value that differs, and the whole conversion used. Slow; for checking. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Local Update", meta = (EditCondition = "bIncrementalSplines"))
	bool bVerifyIncremental = false;

	/** Tiles with roads. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTiles = 0;

	/** Tiles drawn from their Nanite bake. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumNaniteTiles = 0;

	/** What the tiles' GPU meshes hold now, as GpuMeshBudget counts it (MiB). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats", meta = (DisplayName = "GPU Mesh MiB"))
	float GpuMeshMiB = 0.0f;

	/** Video memory left when the network last looked (MiB; -1: unknown). Below MinFreeVideoMemory it waits. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats", meta = (DisplayName = "Free Video Memory MiB"))
	float FreeVideoMemoryMiB = -1.0f;

	/** The last update stopped for want of video memory (MinFreeVideoMemory, or the GPU mesh refusing to grow). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	bool bWaitingForVideoMemory = false;

	/** Tiles the last rebuild wrote or removed. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 TilesRebuilt = 0;

	/** Of the tiles the last rebuild wrote or removed, those edits dirtied (the others were left by earlier batches). A
	 *  rebuild an edit made writes only those. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 EditedTilesRebuilt = 0;

	/** Road and building tiles left for the next batches. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 PendingTiles = 0;

	/** Roads drawn in the tiles the last rebuild wrote. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 RoadsRebuilt = 0;

	/** Roads the last rebuild in tiles built as a graph through the GPU geometry: those near what changed and near the
	 *  tiles it wrote (every road the first time, or after a change of settings). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 RoadsGraphed = 0;

	/** Fill the plots along the roads with buildings (convex hulls pushed up), clear of the roads, of each other and
	 *  of the plots ACSCityBuildingActors reserve. Only roads CS2 zones along have plots (not highways or bridges);
	 *  on the snapshot's terrain the buildings stand on the ground, and none front a road 3 m above or below it. A
	 *  plot belongs to the spline it faces (its segment, side and place along it), so editing one road leaves the
	 *  buildings along the others as they were. Drawn in tiles of TileSize; a rebuild rewrites only the tiles whose
	 *  buildings changed. */
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

	/** Building tiles whose plots the last rebuild laid out again: those near what changed, or all of them after the
	 *  layout's settings changed. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 BuildingTilesLaidOut = 0;

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

	/** Shape the level's Landscape to the ground roads (bridges, viaducts and tunnels apart) through an edit layer of
	 *  its own, "CS2 Roads": the ground under a road goes to its surface, stays flat TerrainMargin past its edge, then
	 *  blends back into the terrain over TerrainShoulder. The layer is made when the network next builds its whole
	 *  preview (its first build, then whenever edits settle) and follows the roads from then on. Off, the layer goes
	 *  and the terrain is as it was. Editor only. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Terrain")
	bool bShapeTerrain = false;

	/** How far the ground under the roads goes to them: 0 not at all, 1 all the way. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Terrain", meta = (ClampMin = "0", ClampMax = "1", EditCondition = "bShapeTerrain"))
	float TerrainInfluence = 1.0f;

	/** The ground under a road this far from its centre line's height (below it when negative). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Terrain", meta = (Units = "cm", EditCondition = "bShapeTerrain"))
	float TerrainHeightOffset = -30.0f;

	/** How far past a road's edge (its sidewalks included) the ground stays flat. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Terrain", meta = (ClampMin = "0", Units = "cm", EditCondition = "bShapeTerrain"))
	float TerrainMargin = 100.0f;

	/** Over how far beyond the flat part the ground blends back into the terrain. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Terrain", meta = (ClampMin = "0", Units = "cm", EditCondition = "bShapeTerrain"))
	float TerrainShoulder = 800.0f;

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

	/** Road pieces (about 4 m each) the terrain layer holds the ground to. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 TerrainPieces = 0;

	UFUNCTION(BlueprintCallable, BlueprintSetter, Category = "CS2 Road|Traffic")
	void SetShowTraffic(bool bShow);

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "CS2 Road|Traffic")
	void ResetTraffic();

	/** The prefab of spline components that are not UCSRoadSplineComponents. */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines", meta = (GetOptions = "GetRoadPrefabOptions"))
	FString DefaultSplinePrefab = TEXT("Small Road");

	/** Traffic drives on the left (UCSRoadNetTools::ConvertToSplines takes it from the snapshot). */
	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines")
	bool bLeftHandTraffic = false;

	UPROPERTY(EditAnywhere, Category = "CS2 Road|Splines")
	FCSRoadSplineSettings SplineSettings;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumRoads = 0;

	/** Spline roads the last build took. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumSplines = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumVertices = 0;

	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumTriangles = 0;

	/** Decal instances drawn: a decal takes one per CSRoad::DecalStripRows rows. */
	UPROPERTY(VisibleAnywhere, Transient, Category = "CS2 Road|Stats")
	int32 NumMarkingInstances = 0;

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

	// UCSRoadNetTools' conversions (there, the details panel stays free of them), editor only: false or null with
	// LastError set.
	bool ConvertToSplines();
	AActor* ConvertTerrainToLandscape(UMaterialInterface* Material);

	// Rebuilds on the next tick: spline roads call it as they change, many times a frame.
	void RequestRebuild();

	/** What the network has built, in an actor of its own (null before its first build). */
	UFUNCTION(BlueprintPure, Category = "CS2 Road")
	ACSRoadNetworkContent* GetContent() const { return Content; }

	// The renderer of the landscape edit layer LayerGuid, when the network shapes the terrain through it.
	UCSRoadTerrainRenderer* TerrainRendererOf(const FGuid& LayerGuid) const;

	// Tests: waits until every tile mesh edit on its way has run and every decal fit has landed, and
	// draws them.
	void FinishWrites();
	// Tests: whether a build in tiles waits for the GPU between ticks (Tick ends it).
	bool IsBuilding() const;

	// The road prefabs of the network's asset directory (the default one without a network).
	static TArray<FString> RoadPrefabNames(const ACSRoadNetworkActor* Network);
	// A road prefab's width there (cm), 0 for none of that name.
	static double RoadWidth(const ACSRoadNetworkActor* Network, const FString& Prefab);
	// The level's network if it has exactly one, which also takes the level's spline roads that hang
	// under no network.
	static ACSRoadNetworkActor* LevelSplineNetwork(const ULevel* Level);
	// The network an actor of the layout (a spline road, a building, an alley region) belongs to: the
	// network it hangs under, else its level's only network.
	static ACSRoadNetworkActor* NetworkOf(const AActor* Actor);

	UFUNCTION()
	TArray<FString> GetRoadPrefabOptions() const;

	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	virtual void PostRegisterAllComponents() override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	// Everything built, in an actor of its own (ACSRoadNetworkContent). Kept out of undo and copies: it is
	// rebuilt rather than restored.
	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	TObjectPtr<ACSRoadNetworkContent> Content;
	// The content, spawned when first asked for (null on a template or without a world).
	ACSRoadNetworkContent* EnsureContent();
	// The content gone, and with it everything the network remembers of what it built: the next build makes it
	// all again.
	void DestroyContent();
	// The content shown or hidden as the network is.
	void SyncContentVisibility();

	// The landscape edit layer the network shapes the terrain through (bShapeTerrain); a copy makes its own.
	UPROPERTY(DuplicateTransient)
	FGuid TerrainLayerGuid;

	UPROPERTY(Transient, DuplicateTransient, NonTransactional)
	TObjectPtr<UCSRoadTerrainRenderer> TerrainRenderer;

	// The terrain layer as the settings ask (editor worlds): made, or found, on the level's Landscape to draw Footprint,
	// the landscape merged again when what it draws changed; gone when bShapeTerrain is off.
	void UpdateTerrain(const TSharedPtr<const CSRoad::FTerrainFootprint>& Footprint);
	void RemoveTerrainLayer();

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> Mesh;

	// The unit meshes the decal instances draw, and DecalMaterial or FittedDecalMaterial per decal
	// surface (keyed "<surface>" and "<surface>|fitted").
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> DecalStrip;

	// By columns.
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UStaticMesh>> DecalGrids;

	// MarkingDecals' heights on their way, as a tile's DecalFits.
	TArray<TSharedPtr<CSRoad::FDecalFitReadback>> MarkingFits;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> DecalSurfaces;

	bool bRebuildRequested = false;
	TUniquePtr<FCSRoadNetworkPreview> Preview;
	void BuildPreview(const CSRoad::FGraph& Graph, const CSRoad::FRoadLanes& Lanes, const CSRoad::FGpuGeometryResult& Geometry, const CSRoad::FSavedTerrain* Terrain);
	void TickPreview(float DeltaSeconds);
	void RenderTraffic();
	void ClearPreview();

	// The traffic lanes and bridge piers need the whole network's lanes and geometry, which a rebuild in
	// tiles no longer makes: the last preview stays up while edits go on, and is made again for the
	// network as last built once they settle (no rebuild for a moment).
	bool bPreviewStale = false;
	double LastBuildSeconds = 0.0;
	// The roads and nodes as last built, and the snapshot lending the terrain (null: none).
	TSharedPtr<const CSRoad::FSavedNetwork> PreviewRoads;
	TSharedPtr<const CSRoad::FSavedNetwork> PreviewTerrain;
	void RefreshPreview();

	// Tiles by (column, row) of the network's ground plane, when it builds in tiles. Kept out of undo:
	// the tiles are rebuilt outside transactions, so a restored map would name components gone since.
	UPROPERTY(Transient, NonTransactional)
	TMap<FIntPoint, FCSRoadTile> Tiles;

	TUniquePtr<CSRoad::FTileState> TileState;

	// The rebuild asked for only finishes pending tiles (no edit since): it waits until the budget has
	// room for the next tile (NextTileBytes, as the last build found it), and builds from the last
	// build's spline network (LastSplines, PreviewRoads) without gathering the splines again.
	bool bPendingOnly = false;
	bool bReuseNetwork = false;
	int64 NextTileBytes = 0;
	TSharedPtr<const TArray<CSRoad::FSplineRoad>> LastSplines;
	// Moves the tiles' Nanite bakes on, a little game-thread time a tick; a finished one replaces its
	// tile's GPU mesh.
	void TickBakes();
	// NumNaniteTiles and GpuMeshMiB as the tiles stand.
	void UpdateBakeStats();
	// What a tile's GPU mesh holds as GpuMeshBudget counts it, and all of them.
	static int64 TileGpuBytes(const FCSRoadTile& Tile);
	int64 GpuMeshBytes() const;
	// What a tile of Roads roads will hold: as its last build (Known), else TrianglesPerRoad a road.
	static int64 EstimateTileBytes(const FCSRoadTile* Known, int32 Roads, double TrianglesPerRoad);
	// GpuMeshBudget in bytes; MAX_int64 when it does not apply.
	int64 GpuMeshBudgetBytes() const;
	// Whether writing Bytes more would leave less video memory than MinFreeVideoMemory: FreeVideoMemoryMiB, read again
	// at most every quarter second, less what tiles took since (VideoMemoryTaken). False when unknown or not applying.
	bool VideoMemoryShort(int64 Bytes);
	double VideoMemorySeconds = -1.0;
	int64 VideoMemoryTaken = 0;

	// Where one road mesh goes, and what it took. With Decals (over the road mesh input), the same
	// graph finds the heights their connectors and crosswalks lie at: Fit, unless the decals have none.
	struct FMeshTargets
	{
		UCSMeshRenderComponent* Road = nullptr;
		TObjectPtr<UCSMesh>* RoadMesh = nullptr;
		const CSRoad::FGpuDecalInput* Decals = nullptr;
		// The mesh input and Decals held for an edit that does not wait for the render thread
		// (EditMeshAsync), which keeps them until it has run. Unset: the edit waits (EditMeshSync).
		TSharedPtr<const CSRoad::FGpuRoadMeshInput> SharedMesh;
		TSharedPtr<const CSRoad::FGpuDecalInput> SharedDecals;
		TSharedPtr<CSRoad::FDecalFitReadback> Fit;
		double AllocationMs = 0.0;
		double RoadMs = 0.0;
		// The GPU mesh refused to grow for want of video memory: nothing of the mesh was changed.
		bool bRefused = false;
	};

	FString ResolveAssetDirectory() const;
	FString ResolveNetworkFile() const;
	// The spline roads this network builds, in its space (Unity metres). A road spline that has not
	// changed since the last time (SplineCache) is taken as read then.
	TArray<CSRoad::FSplineRoad> GatherSplines();
	TUniquePtr<FCSRoadNetworkActor_SplineCache> SplineCache;
	// Rebuild's work. bWait: every GPU round trip waited for. Else (Tick) a build in tiles waits for the
	// GPU a tick at a time (TileJob) and writes the tiles without waiting for the render thread.
	void RunBuild(bool bWait);
	// What a build that ended leaves: its error, the stats, the next build its pending tiles ask for.
	void EndBuild(bool bBuilt, const FString& Error, double Milliseconds);
	bool BuildRoads(bool bWait, FString& OutError);
	// BuildRoads in tiles, from sub-graphs of the roads near what changed (CSRoadTiles.h): Job's stages
	// run until it ends (false on error) or, not waiting, until it waits for the GPU.
	bool AdvanceTiles(FCSRoadNetworkActor_TileJob& Job, FString& OutError);
	// The build in tiles waiting for the GPU between ticks.
	TUniquePtr<FCSRoadNetworkActor_TileJob> TileJob;
	// Allocates the target and records the road mesh into it, on the build's geometry when
	// SharedGeometry holds it (else the graph runs it again).
	bool WriteMeshes(const CSRoad::FGpuGeometryInput& Geometry, const CSRoad::FPooledGeometry& SharedGeometry, const CSRoad::FGpuRoadMeshInput& MeshInput,
		const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FAssets& Assets, FMeshTargets& Targets, FString& OutError);
	// The decals' instances into Components (made or dropped to fit, named Name_<n>); returns how many.
	// The connectors and crosswalks need FitHeights (else they are left out).
	int32 WriteDecals(const CSRoad::FGpuDecalInput& Decals, const TArray<float>* FitHeights, TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name);
	// Decals written with Write: drawn now unless Fit holds heights still to land (queued in Fits).
	int32 DrawDecals(const CSRoad::FGpuDecalInput& Decals, const TSharedPtr<CSRoad::FDecalFitReadback>& Fit, TArray<TSharedPtr<CSRoad::FDecalFitReadback>>& Fits,
		TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name);
	// Draws whichever queued decals have landed; the latest of them wins. Returns whether any did.
	bool TickDecalFits(TArray<TSharedPtr<CSRoad::FDecalFitReadback>>& Fits, TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name, int32& OutInstances);
	void TickAllDecalFits();
	// Runs the tiles' mesh edits still on their way to the end (their game-thread ends included).
	void LandTileEdits();
	bool AnyTileEditInFlight() const;
	// DecalMaterial and FittedDecalMaterial with the albedo and tint of each of the decals' surfaces'
	// M_CS2 materials, made now for when the decals draw (MissingMarkingSurfaces: those without).
	void ResolveDecalMaterials(const CSRoad::FGpuDecalInput& Decals, const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FAssets& Assets);
	// As resolved for a surface; null when it could not be.
	UMaterialInterface* DecalSurfaceMaterial(const FString& Surface, bool bFitted) const;
	static void DestroyComponents(TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components);
	// The tile's components, made when it first gets roads.
	FCSRoadTile& EnsureTile(const FIntPoint& Key);
	void RemoveTile(const FIntPoint& Key);
	// Every tile gone and forgotten (building whole again, or the settings changed under them).
	void ClearTiles();
	// What the tiles' meshes depend on besides the roads.
	uint64 TileSignature(bool bLeftHand) const;
	// Where the camera is, in tiles of the network's ground plane (fractional): the editor's perspective
	// viewport on this world, else the first player's camera. The tiles waiting to be written go nearest
	// it first. False: no camera.
	bool TileFocus(FVector2D& OutFocus) const;

	// As Tiles, by the same (column, row).
	UPROPERTY(Transient, NonTransactional)
	TMap<FIntPoint, FCSCityBuildingTile> BuildingTiles;

	// Building tiles whose buildings changed but were left for a later batch.
	int32 PendingBuildingTiles = 0;

	// What the batch buildings were last laid out from, and the tiles still to write.
	TUniquePtr<CSCity::FLotState> LotState;

	// The batch buildings along the roads as built (Roads, the network's space). The plots face the
	// segments of Splines. Only the tiles within reach of an input that changed are laid out again; the
	// plots there come out as a full layout's would.
	void BuildBatchBuildings(const TArray<CSRoad::FSplineRoad>& Splines, const TArray<CSRoad::FRoadSpec>& Roads, CSRoad::FAssets& Assets, const CSRoad::FSavedTerrain* Terrain);
	void ClearBuildingTiles();
};

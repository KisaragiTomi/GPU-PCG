#include "CSRoadNetworkActor.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CSCityBuildingActor.h"
#include "CSCityGarage.h"
#include "CSCityLayout.h"
#include "CSGpuMemoryBudget.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadDecalInstances.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadGpuMesh.h"
#include "CSRoadNanite.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkContent.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadNetworkPreview.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTerrainLayer.h"
#include "CSRoadTiles.h"
#include "Components/SplineComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Hash/xxhash.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "RenderTimer.h"
#include "RHICommandList.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#if WITH_EDITOR
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Landscape.h"
#include "LandscapeInfo.h"
#include "LevelEditorViewport.h"
#include "ScopedTransaction.h"
#endif

namespace
{
TAutoConsoleVariable<int32> CVarCSRoadBakeLog(TEXT("CSRoad.BakeLog"), 0,
	TEXT("1: road networks log the game-thread time their tick spends past 2 ms (rebuilds and Nanite bakes, by step), and once a second the frame's game, render, RHI and GPU times."));

// What a road mesh of that many vertices and indices takes in VRAM as the budget counts it: its eight
// streams (positions, tangents, one UV set, colours, indices, material ids, draw args, counters) in the
// pools' 64 KiB granules, and its ray tracing BLAS, about 31 bytes a triangle on Palmsland.
int64 CSRoadNetworkActor_GpuMeshBytes(uint32 Vertices, uint32 Indices)
{
	const int64 Granule = 64 << 10;
	int64 Bytes = int64(Indices / 3) * 31;
	for (const int64 Stream : { int64(Vertices) * 12, int64(Vertices) * 8, int64(Vertices) * 8, int64(Vertices) * 4, int64(Indices) * 4, int64(Indices / 3) * 4, int64(1), int64(1) }) Bytes += Align(Stream, Granule);
	return Bytes;
}

// Where the refinement warning points: the node whose mesh left the most leaves unconverged (world cm).
FString CSRoadNetworkActor_Unconverged(const CSRoad::FGraph& Graph, const CSRoad::FGpuRoadMeshInput& Input, const FTransform& NetworkToWorld)
{
	if (!Input.Instances.IsValidIndex(Input.MostUnconvergedInstance)) return FString();
	const int32 Slot = int32(Input.Instances[Input.MostUnconvergedInstance].Parameters);
	const auto& Ends = Graph.EdgeData[Graph.Roads[Slot / 3].Entity];
	const auto& Position = Graph.NodeData[Slot % 3 == 1 ? Ends.m_Start : Ends.m_End].m_Position;
	const FVector World = NetworkToWorld.TransformPosition(FVector(Position.x, -Position.z, Position.y) * 100.0);
	return FString::Printf(TEXT(", %u of them at the node at %s"), Input.MostUnconverged, *World.ToCompactString());
}

// The prefab data is large (pieces.bin is ~120 MB) and never changes: loaded once per directory
// and shared by every network actor. FAssets keeps pointers into the byte arrays. The plugin's
// custom roads (Data/CustomRoads.json, made from the prefabs' roads) join every directory's.
struct FCSRoadNetworkActor_Library
{
	TArray<uint8> Prefabs;
	TArray<uint8> Raw;
	TArray64<uint8> Pieces;
	TArray<uint8> CustomRoads;
	CSRoad::FAssets Assets;
};

TSharedPtr<FCSRoadNetworkActor_Library> CSRoadNetworkActor_Library(const FString& Directory, FString& OutError)
{
	static TMap<FString, TSharedPtr<FCSRoadNetworkActor_Library>> Cache;
	const FString Key = FPaths::ConvertRelativePathToFull(Directory);
	if (const TSharedPtr<FCSRoadNetworkActor_Library>* Found = Cache.Find(Key)) return *Found;
	TSharedPtr<FCSRoadNetworkActor_Library> Library = MakeShared<FCSRoadNetworkActor_Library>();
	if (!FFileHelper::LoadFileToArray(Library->Prefabs, *FPaths::Combine(Key, TEXT("prefabs.json")))
		|| !FFileHelper::LoadFileToArray(Library->Raw, *FPaths::Combine(Key, TEXT("raw-prefabs.json")))
		|| !FFileHelper::LoadFileToArray(Library->Pieces, *FPaths::Combine(Key, TEXT("pieces.bin"))))
	{
		OutError = FString::Printf(TEXT("prefabs.json, raw-prefabs.json or pieces.bin missing in %s"), *Key);
		return nullptr;
	}
	FFileHelper::LoadFileToArray(Library->CustomRoads, *FPaths::Combine(FPaths::GetPath(CSRoadDataDir()), TEXT("CustomRoads.json")), FILEREAD_Silent);
	if (!Library->Assets.Load(Library->Prefabs, Library->Raw, &Library->Pieces, OutError, &Library->CustomRoads)) return nullptr;
	Cache.Add(Key, Library);
	UE_LOG(LogCSRoadNet, Log, TEXT("CS2 road prefabs loaded from %s"), *Key);
	return Library;
}

// The snapshot at Path, read again only when the file changes. One at a time: terrains are large.
TSharedPtr<const CSRoad::FSavedNetwork> CSRoadNetworkActor_Network(const FString& Path, CSRoad::FAssets& Assets, bool bTerrain, FString& OutError)
{
	struct FCached
	{
		FString Key;
		FDateTime Stamp;
		TSharedPtr<const CSRoad::FSavedNetwork> Network;
	};
	static FCached Cached;
	const FString Full = FPaths::ConvertRelativePathToFull(Path);
	const FString Key = FString::Printf(TEXT("%s|%d|%p"), *Full, bTerrain ? 1 : 0, &Assets);
	const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Full);
	if (Cached.Network && Cached.Key == Key && Cached.Stamp == Stamp) return Cached.Network;
	TSharedPtr<CSRoad::FSavedNetwork> Network = MakeShared<CSRoad::FSavedNetwork>();
	if (!Network->Load(Full, &Assets, bTerrain, OutError)) return nullptr;
	Cached = { Key, Stamp, Network };
	return Network;
}

// The network's space (Unity metres, y up) to the actor's (cm, z up) and back; both exact.
FVector CSRoadNetworkActor_ToUnreal(const FVector3d& V) { return FVector(V.X, -V.Z, V.Y) * 100.0; }
FVector3d CSRoadNetworkActor_ToUnity(const FVector& V) { return FVector3d(V.X, V.Z, -V.Y) / 100.0; }

CSRoad::CompositionFlags CSRoadNetworkActor_Flags(const FIntVector& V) { return CSRoad::MakeFlags(static_cast<uint32>(V.X), static_cast<uint32>(V.Y), static_cast<uint32>(V.Z)); }
FIntVector CSRoadNetworkActor_Flags(const CSRoad::CompositionFlags& F) { return FIntVector(static_cast<int32>(F.m_General), static_cast<int32>(F.m_Left), static_cast<int32>(F.m_Right)); }

// M_CS2_<NN>_<BaseColor> materials (Scripts/ImportCS2Roads.py) by base-colour texture name. Kept
// from one rebuild to the next until an asset is added, removed or renamed (looking them up took
// ~10 ms a rebuild).
TMap<FString, UMaterialInterface*> CSRoadNetworkActor_Materials(const FString& Folder)
{
	TMap<FString, UMaterialInterface*> Result;
	if (Folder.IsEmpty()) return Result;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	static TMap<FString, TMap<FString, TWeakObjectPtr<UMaterialInterface>>> Known;
	static bool bWatching = false;
	if (!bWatching)
	{
		bWatching = true;
		Registry.OnAssetsAdded().AddLambda([](TConstArrayView<FAssetData>) { Known.Reset(); });
		Registry.OnAssetsRemoved().AddLambda([](TConstArrayView<FAssetData>) { Known.Reset(); });
		Registry.OnAssetRenamed().AddLambda([](const FAssetData&, const FString&) { Known.Reset(); });
	}
	if (const TMap<FString, TWeakObjectPtr<UMaterialInterface>>* Kept = Known.Find(Folder))
	{
		bool bAll = true;
		for (const TPair<FString, TWeakObjectPtr<UMaterialInterface>>& Pair : *Kept)
		{
			if (UMaterialInterface* Material = Pair.Value.Get()) Result.Add(Pair.Key, Material);
			else bAll = false;
		}
		if (bAll) return Result;
		Result.Reset();
	}
	// Scanned from disk once a session (the scan took ~10 ms a rebuild); the registry follows what is
	// added there after.
	static TSet<FString> Scanned;
	bool bScanned = false;
	Scanned.Add(Folder, &bScanned);
	if (!bScanned) Registry.ScanPathsSynchronous({ Folder }, false);
	TArray<FAssetData> Assets;
	Registry.GetAssetsByPath(FName(*Folder), Assets, true);
	for (const FAssetData& Asset : Assets)
	{
		FString Name = Asset.AssetName.ToString();
		if (!Name.RemoveFromStart(TEXT("M_CS2_"))) continue;
		int32 Separator = INDEX_NONE;
		if (Name.FindChar(TEXT('_'), Separator)) Name.RightChopInline(Separator + 1);
		if (UMaterialInterface* Material = Cast<UMaterialInterface>(Asset.GetAsset())) Result.Add(Name, Material);
	}
	TMap<FString, TWeakObjectPtr<UMaterialInterface>>& Kept = Known.Add(Folder);
	for (const TPair<FString, UMaterialInterface*>& Pair : Result) Kept.Add(Pair.Key, Pair.Value);
	return Result;
}

// Some of a network's roads (spec indices, in spec order) built on their own: the graph, its GPU
// geometry read back, and the geometry kept for the build's later graphs; with it, in the same GPU
// round trip, the refinement of the instances of some of its roads (Counted).
struct FCSRoadNetworkActor_Subgraph
{
	TArray<int32> Specs;
	// Graph.Roads index per spec of the network (INDEX_NONE: not in it).
	TArray<int32> RoadOfSpec;
	CSRoad::FGraph Graph;
	CSRoad::FGpuGeometryInput Input;
	CSRoad::FGpuGeometryResult Result;
	CSRoad::FPooledGeometry Shared;
	CSRoad::FGpuRoadMeshInput Counted;
	bool bCounted = false;
	// The GPU round trip on its way, holding Input and Counted until it lands
	// (CSRoadNetworkActor_LandSubgraph).
	TUniquePtr<CSRoad::FRefineCountsReadback> Readback;

	bool Holds(const TArray<int32>& Roads) const
	{
		for (const int32 Road : Roads) if (!RoadOfSpec.IsValidIndex(Road) || RoadOfSpec[Road] == INDEX_NONE) return false;
		return true;
	}

	// Whether Counted has the refinement of every instance of Refs (Graph's roads).
	bool Counts(const TArray<CSRoad::FGpuInstanceRef>& Refs) const
	{
		if (!bCounted) return false;
		for (const CSRoad::FGpuInstanceRef& Ref : Refs) if (!Counted.InstanceOfSlot.IsValidIndex(Ref.Slot()) || Counted.InstanceOfSlot[Ref.Slot()] == INDEX_NONE) return false;
		return true;
	}
};

// CountSpecs (optional, specs among Taken): the roads whose instances' refinement comes back with the
// geometry. The GPU round trip is left on its way (Out.Readback); KeepTerrain holds Terrain until it
// is recorded.
bool CSRoadNetworkActor_BuildSubgraph(CSRoad::FAssets& Assets, const TArray<CSRoad::FRoadSpec>& Specs, const TArray<CSRoad::FSavedNode>& Nodes, bool bLeftHand,
	const CSRoad::FSavedTerrain* Terrain, TSharedPtr<const void> KeepTerrain, TArray<int32> Taken, FCSRoadNetworkActor_Subgraph& Out, FString& OutError,
	const TArray<int32>* CountSpecs = nullptr, CSRoad::FCompositionMeshCache* Compositions = nullptr)
{
	Out.Specs = MoveTemp(Taken);
	Out.Graph.bLeftHandTraffic = bLeftHand;
	bool bBuilt = false;
	if (Out.Specs.Num() == Specs.Num()) bBuilt = Out.Graph.Build(Assets, Specs, Nodes, OutError);
	else
	{
		TArray<CSRoad::FRoadSpec> Part;
		Part.Reserve(Out.Specs.Num());
		for (const int32 Spec : Out.Specs) Part.Add(Specs[Spec]);
		bBuilt = Out.Graph.Build(Assets, Part, Nodes, OutError);
	}
	if (!bBuilt || !Out.Input.Build(Out.Graph, Assets, Terrain, OutError)) return false;
	Out.RoadOfSpec.Init(INDEX_NONE, Specs.Num());
	for (int32 Road = 0; Road < Out.Graph.Roads.Num(); Road++) Out.RoadOfSpec[Out.Specs[Out.Graph.Roads[Road].SpecIndex]] = Road;
	CSRoad::FGpuRoadMeshInput Counting;
	if (CountSpecs)
	{
		TArray<CSRoad::FGpuInstanceRef> Refs;
		for (const int32 Spec : *CountSpecs) if (Out.RoadOfSpec[Spec] != INDEX_NONE) for (int32 Kind = 0; Kind < 3; Kind++) Refs.Add({ Out.RoadOfSpec[Spec], Kind });
		if (!Counting.BuildSubset(Out.Graph, Assets, Refs, {}, Compositions, OutError)) return false;
	}
	Out.bCounted = CountSpecs != nullptr;
	Out.Readback = MakeUnique<CSRoad::FRefineCountsReadback>(MoveTemp(Out.Input), MoveTemp(Counting), MoveTemp(KeepTerrain));
	return true;
}

// Once Out.Readback has landed: Input back, the geometry read back and kept, Counted refined.
bool CSRoadNetworkActor_LandSubgraph(FCSRoadNetworkActor_Subgraph& Out, FString& OutError)
{
	const TUniquePtr<CSRoad::FRefineCountsReadback> Readback = MoveTemp(Out.Readback);
	CSRoad::FGpuRoadMeshInput Counting;
	if (!Readback->Finish(Out.Input, Counting, Out.Result, Out.Shared, OutError)) return false;
	if (Out.bCounted) Out.Counted = MoveTemp(Counting);
	return true;
}

// The road mesh into an edit of its target: on Shared's geometry when it holds it (else Geometry's
// passes run again); with Decals, the heights their connectors and crosswalks lie at (into Fit); then
// the material sections (OutSorted: sorted, to publish once the edit has run). False when the target
// has no vertex streams.
bool CSRoadNetworkActor_RecordRoadMesh(FCSMeshEditContext& Context, const CSRoad::FGpuGeometryInput* Geometry, const CSRoad::FPooledGeometry& Shared, const CSRoad::FGpuRoadMeshInput& Mesh,
	const CSRoad::FGpuDecalInput* Decals, const TSharedPtr<CSRoad::FDecalFitReadback>& Fit, const FMatrix44f& LocalToWorld, const FMatrix44f& WorldToLocal, const FBox& WorldBounds,
	int32 NumSlots, bool& OutSorted)
{
	if (!Context.Positions() || !Context.Tangents() || !Context.TexCoords() || !Context.Colors() || !Context.Indices() || !Context.MaterialIds()) return false;
	FRDGBuilder& GraphBuilder = Context.GraphBuilder;
	const CSRoad::FGpuGeometryOutput GeometryOut = Shared.IsValid() ? Shared.Register(GraphBuilder) : CSRoad::AddGeometryPasses(GraphBuilder, *Geometry);
	CSRoad::FGpuRoadMeshTarget Target;
	Target.Positions = Context.Positions();
	Target.Tangents = Context.Tangents();
	Target.TexCoords = Context.TexCoords();
	Target.Colors = Context.Colors();
	Target.Indices = Context.Indices();
	Target.MaterialIds = Context.MaterialIds();
	Target.VertexCapacity = Context.Resident.VertexCapacity;
	Target.IndexCapacity = Context.Resident.IndexCapacity;
	Target.LocalToWorld = LocalToWorld;
	CSRoad::AddRoadMeshPasses(GraphBuilder, GeometryOut, Mesh, Target);
	UCSMeshOps::AddSetCountersPass(Context, Mesh.NumDrawnVertices, Mesh.NumDrawnIndices);
	Context.SetWorldBounds(WorldBounds);
	if (Decals)
	{
		CSRoad::FGpuRoadStreams Road;
		Road.Positions = Context.Positions();
		Road.Tangents = Context.Tangents();
		Road.Indices = Context.Indices();
		Road.WorldToLocal = WorldToLocal;
		const CSRoad::FGpuDecalReceivers Receivers = CSRoad::AddDecalReceiverPasses(GraphBuilder, Mesh, *Decals, Road);
		Fit->Enqueue(GraphBuilder, CSRoad::AddDecalFitPasses(GraphBuilder, Mesh, *Decals, Road, Receivers), Decals->NumFitHeights);
	}
	OutSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
	return true;
}

// What an edit that does not wait recorded, for its end on the game thread.
struct FCSRoadNetworkActor_Recorded
{
	bool bRecorded = false;
	bool bSorted = false;
};
}

// What GatherSplines read of each road spline, and what it read it from: the spline's revision and
// settings, its placement and the network's, and the names its Id comes from. An edit that reaches
// the network without a notice (points set without UpdateSpline, metadata set without
// PostEditChangeProperty) is not seen until the spline's next notice.
struct FCSRoadNetworkActor_SplineCache
{
	struct FEntry
	{
		uint32 Revision = 0;
		int32 NumPoints = 0;
		bool bClosed = false;
		FString Prefab;
		ECSRoadTier Tier = ECSRoadTier::Main;
		bool bAdjust = true;
		FIntVector Upgrade = FIntVector::ZeroValue;
		FTransform ComponentToWorld;
		FTransform NetworkToWorld;
		FName ComponentName;
		FName OwnerName;
		CSRoad::FSplineRoad Road;

		bool Matches(const UCSRoadSplineComponent& Spline, const FTransform& InComponentToWorld, const FTransform& InNetworkToWorld) const
		{
			return Revision == Spline.Revision && NumPoints == Spline.GetNumberOfSplinePoints() && bClosed == Spline.IsClosedLoop() && Prefab == Spline.Prefab && Tier == Spline.Tier
				&& bAdjust == Spline.bAdjustSegments && Upgrade == Spline.Upgrade && ComponentToWorld.Equals(InComponentToWorld, 0.0) && NetworkToWorld.Equals(InNetworkToWorld, 0.0)
				&& ComponentName == Spline.GetFName() && OwnerName == (Spline.GetOwner() ? Spline.GetOwner()->GetFName() : NAME_None);
		}
	};
	TMap<TObjectKey<USplineComponent>, FEntry> Entries;
	// The last gathering: finding the spline components, and how many were taken as read.
	double FindMs = 0.0;
	int32 Hits = 0;
	// The network the splines made, for the next build to convert only around the ones that changed.
	CSRoad::FSplineNetworkCache Network;
};

// A build in tiles (ACSRoadNetworkActor::AdvanceTiles): what it builds from, and what it has built so
// far, kept on the actor (TileJob) while it waits for the GPU between ticks.
struct FCSRoadNetworkActor_TileJob
{
	// What it builds from, held while it waits: the prefab data, the spline network, the snapshot
	// lending the terrain (null: none).
	TSharedPtr<FCSRoadNetworkActor_Library> Library;
	TSharedPtr<const CSRoad::FSavedNetwork> Network;
	TSharedPtr<const CSRoad::FSavedNetwork> Snapshot;
	TMap<FString, UMaterialInterface*> ByBaseColor;
	bool bMarkings = false;
	// Every GPU round trip waited for (Rebuild); else the job waits a tick at a time and writes the
	// tiles without waiting for the render thread (Tick).
	bool bWait = true;

	// Detect: the roads whose output changed, from a sub-graph around them. Batch: the tiles to write
	// now, and a sub-graph for them unless the detection's holds them. Write: the tiles.
	enum class EStage : uint8 { Detect, Batch, Write, Done };
	EStage Stage = EStage::Detect;
	uint64 Signature = 0;
	bool bEverything = false;
	CSRoad::FTileNetwork Tiles;
	TArray<int32> Everything;
	TArray<int32> Affected;
	FCSRoadNetworkActor_Subgraph Detected;
	TArray<uint64> Outputs;
	TMap<FIntPoint, TArray<int32>> Dirty;
	// Of the dirty tiles, the first Edited are an edit's, Fresh of them this build's (FTileState::Dirty).
	int32 Edited = 0;
	int32 Fresh = 0;
	TArray<FIntPoint> Written;
	TArray<int32> Batch;
	int64 Budget = MAX_int64;
	FCSRoadNetworkActor_Subgraph Drawing;
	// Detected, or Drawing when it was needed.
	FCSRoadNetworkActor_Subgraph* Graphed = nullptr;

	// Game-thread time: before the tiles (OtherMs), in them (GameMs), and per stage, where waiting for
	// the GPU (WaitMs, bWait) is not counted; ticks waiting for it (not bWait).
	double OtherMs = 0.0;
	double GameMs = 0.0;
	double NetMs = 0.0;
	double DetectMs = 0.0;
	double DirtyMs = 0.0;
	double GraphMs = 0.0;
	double WaitMs = 0.0;
	int32 Frames = 0;

	const CSRoad::FSavedTerrain* Terrain() const { return Snapshot && Snapshot->Terrain.IsValid() ? &Snapshot->Terrain : nullptr; }
	FCSRoadNetworkActor_Subgraph* Waiting() { return Detected.Readback ? &Detected : Drawing.Readback ? &Drawing : nullptr; }
};

ACSRoadNetworkActor::ACSRoadNetworkActor()
{
	// Ticks to carry out rebuilds that spline roads asked for, and to keep the content shown as the network is.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	MaterialFolder.Path = TEXT("/PCGPlugins/CS2Roads/Materials");
	DecalMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/PCGPlugins/CSRoadNet/M_RoadDecal.M_RoadDecal")));
	FittedDecalMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/PCGPlugins/CSRoadNet/M_RoadDecalFitted.M_RoadDecalFitted")));
}

ACSRoadNetworkActor::~ACSRoadNetworkActor() = default;

ACSRoadNetworkActor::ACSRoadNetworkActor(FVTableHelper& Helper)
	: Super(Helper)
{
}

FString ACSRoadNetworkActor::ResolveAssetDirectory() const
{
	if (!AssetDirectory.Path.IsEmpty()) return AssetDirectory.Path;
	const FString Lab = FPlatformMisc::GetEnvironmentVariable(TEXT("CS2ROADLAB"));
	return Lab.IsEmpty() ? FPaths::Combine(CSRoadDataDir(), TEXT("Prefabs")) : FPaths::Combine(Lab, TEXT("Assets"), TEXT("CS2"));
}

FString ACSRoadNetworkActor::ResolveNetworkFile() const
{
	const FString& Path = NetworkFile.FilePath;
	return Path.IsEmpty() || !FPaths::IsRelative(Path) ? Path : FPaths::Combine(CSRoadDataDir(), Path);
}

void ACSRoadNetworkActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// Levels saved before the content had its own actor hold the network's RoadMesh: loaded, it hangs on as a
	// component of nothing. It goes (unsaved, the level is not dirtied; the next save leaves it out).
	TInlineComponentArray<UCSMeshRenderComponent*> Stale(this);
	for (UCSMeshRenderComponent* Component : Stale) Component->DestroyComponent();
	// The content is transient: a loaded or duplicated network has none until it builds.
	if (!bBuildOnLoad || IsTemplate() || !GetWorld()) return;
	if (Content || Tiles.Num() > 0) return;
	// Spline roads may register after this actor: build once they all have.
	RequestRebuild();
}

void ACSRoadNetworkActor::Destroyed()
{
	RemoveTerrainLayer();
	DestroyContent();
	Super::Destroyed();
}

UCSRoadTerrainRenderer* ACSRoadNetworkActor::TerrainRendererOf(const FGuid& LayerGuid) const
{
	return bShapeTerrain && TerrainLayerGuid.IsValid() && TerrainLayerGuid == LayerGuid ? TerrainRenderer.Get() : nullptr;
}

void ACSRoadNetworkActor::UpdateTerrain(const TSharedPtr<const CSRoad::FTerrainFootprint>& Footprint)
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World || World->WorldType != EWorldType::Editor || IsTemplate()) return;
	if (!bShapeTerrain)
	{
		RemoveTerrainLayer();
		return;
	}
	TActorIterator<ALandscape> Found(World);
	ALandscape* Landscape = Found ? *Found : nullptr;
	if (!Landscape) return;
	if (!TerrainLayerGuid.IsValid() || Landscape->GetLayerIndex(TerrainLayerGuid) == INDEX_NONE)
	{
		// On top of the landscape's other layers: the roads have the last word on the ground under them.
		const int32 Index = Landscape->CreateLayer(FName(TEXT("CS2 Roads")), UCSRoadTerrainLayer::StaticClass());
		const FLandscapeLayer* Layer = Index != INDEX_NONE ? Landscape->GetLayerConst(Index) : nullptr;
		if (!Layer || !Layer->EditLayer)
		{
			UE_LOG(LogCSRoadNet, Warning, TEXT("%s: could not add the CS2 Roads edit layer to %s"), *GetName(), *Landscape->GetName());
			return;
		}
		TerrainLayerGuid = Layer->EditLayer->GetGuid();
		MarkPackageDirty();
	}
	if (!TerrainRenderer) TerrainRenderer = NewObject<UCSRoadTerrainRenderer>(this, NAME_None, RF_Transient);
	const bool bSame = TerrainRenderer->Draws(Footprint.Get(), TerrainInfluence, TerrainHeightOffset);
	TerrainRenderer->Set(Footprint, Landscape, TerrainInfluence, TerrainHeightOffset);
	TerrainPieces = Footprint ? Footprint->Ends.Num() : 0;
	if (!bSame) Landscape->RequestLayersContentUpdateForceAll(ELandscapeLayerUpdateMode::Update_All);
#endif
}

void ACSRoadNetworkActor::RemoveTerrainLayer()
{
#if WITH_EDITOR
	TerrainRenderer = nullptr;
	TerrainPieces = 0;
	if (!TerrainLayerGuid.IsValid() || !GetWorld()) return;
	for (ALandscape* Landscape : TActorRange<ALandscape>(GetWorld()))
	{
		const int32 Index = Landscape->GetLayerIndex(TerrainLayerGuid);
		if (Index != INDEX_NONE) Landscape->DeleteLayer(Index);
	}
	TerrainLayerGuid.Invalidate();
	MarkPackageDirty();
#endif
}

ACSRoadNetworkContent* ACSRoadNetworkActor::EnsureContent()
{
	if (Content) return Content;
	UWorld* World = GetWorld();
	if (!World || IsTemplate()) return nullptr;
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetLevel();
	Parameters.Owner = this;
	Parameters.ObjectFlags = RF_Transient | RF_DuplicateTransient;
	Parameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Parameters.bHideFromSceneOutliner = true;
	Content = World->SpawnActor<ACSRoadNetworkContent>(ACSRoadNetworkContent::StaticClass(), GetActorTransform(), Parameters);
	if (!Content) return nullptr;
	// The preview's instances lie in the network's space; the content goes where the network goes.
	Content->AttachToActor(this, FAttachmentTransformRules::SnapToTargetIncludingScale);
#if WITH_EDITOR
	Content->SetActorLabel(GetActorLabel() + TEXT(" Content"), false);
#endif
	SyncContentVisibility();
	return Content;
}

void ACSRoadNetworkActor::DestroyContent()
{
	TileJob.Reset();
	LandTileEdits();
	Tiles.Reset();
	BuildingTiles.Reset();
	if (TileState) TileState->Reset();
	LotState.Reset();
	MarkingFits.Reset();
	ClearPreview();
	if (Content) Content->Destroy();
	Content = nullptr;
	NumTiles = NumNaniteTiles = NumBuildings = 0;
}

void ACSRoadNetworkActor::SyncContentVisibility()
{
	if (!Content) return;
	if (Content->IsHidden() != IsHidden()) Content->SetActorHiddenInGame(IsHidden());
#if WITH_EDITOR
	if (Content->IsTemporarilyHiddenInEditor() != IsTemporarilyHiddenInEditor()) Content->SetIsTemporarilyHiddenInEditor(IsTemporarilyHiddenInEditor());
#endif
}

void ACSRoadNetworkActor::RequestRebuild()
{
	bRebuildRequested = true;
	bPendingOnly = false;
}

void ACSRoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Start = FPlatformTime::Seconds();
	SyncContentVisibility();
	TickBakes();
	TickAllDecalFits();
	const double BakeMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	if (TileJob)
	{
		// A build in tiles waiting for the GPU goes on once its round trip has landed.
		FString Error;
		const bool bBuilt = AdvanceTiles(*TileJob, Error);
		if (!bBuilt || TileJob->Stage == FCSRoadNetworkActor_TileJob::EStage::Done)
		{
			const double Milliseconds = TileJob->OtherMs + TileJob->GameMs;
			TileJob.Reset();
			EndBuild(bBuilt, Error, Milliseconds);
		}
	}
	else if (bRebuildRequested)
	{
		// Tiles left pending wait until bakes free room in the budget for the next one.
		const int64 Held = bPendingOnly ? GpuMeshBytes() : 0;
		if (!bPendingOnly || Held == 0 || Held + NextTileBytes <= GpuMeshBudgetBytes())
		{
			bReuseNetwork = bPendingOnly;
			RunBuild(false);
			bReuseNetwork = false;
		}
	}
	// Edits have settled and no tiles wait: the preview of the whole network as last built.
	else if (bPreviewStale && FPlatformTime::Seconds() - LastBuildSeconds > 0.3) RefreshPreview();
	if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0)
	{
		const double TickMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		if (TickMs > 2.0) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tick %.1f ms (bakes %.1f ms)"), *GetName(), TickMs, BakeMs);
		static double LastFrameLog = 0.0;
		if (Start - LastFrameLog > 1.0)
		{
			LastFrameLog = Start;
			UE_LOG(LogCSRoadNet, Log, TEXT("%s: frame: game %.1f ms, render %.1f ms, RHI %.1f ms, GPU %.1f ms; %d tiles, %d pending, %d baked, GPU meshes %.1f MiB"), *GetName(),
				FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime), FPlatformTime::ToMilliseconds(GRHIThreadTime), FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)),
				NumTiles, PendingTiles, NumNaniteTiles, GpuMeshMiB);
		}
	}
	TickPreview(DeltaSeconds);
}

TArray<FString> ACSRoadNetworkActor::RoadPrefabNames(const ACSRoadNetworkActor* Network)
{
	FString Error;
	const ACSRoadNetworkActor* From = Network ? Network : GetDefault<ACSRoadNetworkActor>();
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(From->ResolveAssetDirectory(), Error);
	TArray<FString> Names;
	if (Library) Names = Library->Assets.RoadNames();
	Names.Sort();
	return Names;
}

double ACSRoadNetworkActor::RoadWidth(const ACSRoadNetworkActor* Network, const FString& Prefab)
{
	FString Error;
	const ACSRoadNetworkActor* From = Network ? Network : GetDefault<ACSRoadNetworkActor>();
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(From->ResolveAssetDirectory(), Error);
	return Library && Library->Assets.HasRoad(Prefab) ? double(Library->Assets.Geometry(Prefab).m_DefaultWidth) * 100.0 : 0.0;
}

TArray<FString> ACSRoadNetworkActor::GetRoadPrefabOptions() const
{
	return RoadPrefabNames(this);
}

ACSRoadNetworkActor* ACSRoadNetworkActor::LevelSplineNetwork(const ULevel* Level)
{
	UWorld* World = Level ? Level->GetWorld() : nullptr;
	if (!World) return nullptr;
	ACSRoadNetworkActor* Found = nullptr;
	for (ACSRoadNetworkActor* Network : TActorRange<ACSRoadNetworkActor>(World))
	{
		if (Network->GetLevel() != Level) continue;
		if (Found) return nullptr;
		Found = Network;
	}
	return Found;
}

ACSRoadNetworkActor* ACSRoadNetworkActor::NetworkOf(const AActor* Actor)
{
	for (AActor* It = const_cast<AActor*>(Actor); It; It = It->GetAttachParentActor()) if (ACSRoadNetworkActor* Network = Cast<ACSRoadNetworkActor>(It)) return Network;
	return Actor ? LevelSplineNetwork(Actor->GetLevel()) : nullptr;
}

TArray<CSRoad::FSplineRoad> ACSRoadNetworkActor::GatherSplines()
{
	// Spline components on this actor, the spline roads that are this network's, and the spline
	// components of any other actor hanging under it.
	const double Start = FPlatformTime::Seconds();
	if (!SplineCache) SplineCache = MakeUnique<FCSRoadNetworkActor_SplineCache>();
	TArray<USplineComponent*> Components;
	GetComponents<USplineComponent>(Components);
	if (UWorld* World = GetWorld()) for (ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World)) if (Road->Spline && Road->Spline->FindNetwork() == this) Components.Add(Road->Spline.Get());
	// Not GetAttachedActors: its AddUnique made this quadratic in the spline roads hanging under it.
	TSet<AActor*> Seen;
	TArray<AActor*> Below;
	ForEachAttachedActors([&Below](AActor* Child) { Below.Add(Child); return true; });
	while (Below.Num() > 0)
	{
		AActor* Actor = Below.Pop(EAllowShrinking::No);
		bool bSeen = false;
		Seen.Add(Actor, &bSeen);
		if (bSeen) continue;
		Actor->ForEachAttachedActors([&Below](AActor* Child) { Below.Add(Child); return true; });
		if (Actor->IsA<ACSRoadSplineActor>()) continue;
		TArray<USplineComponent*> Own;
		Actor->GetComponents<USplineComponent>(Own);
		Components.Append(Own);
	}
	SplineCache->FindMs = (FPlatformTime::Seconds() - Start) * 1000.0;

	TArray<CSRoad::FSplineRoad> Splines;
	Splines.Reserve(Components.Num());
	const FTransform NetworkToWorld = GetActorTransform();
	SplineCache->Hits = 0;
	TMap<TObjectKey<USplineComponent>, FCSRoadNetworkActor_SplineCache::FEntry> Kept;
	Kept.Reserve(Components.Num());
	for (const USplineComponent* Component : Components)
	{
		const int32 Count = Component->GetNumberOfSplinePoints();
		if (Count < 2) continue;
		const UCSRoadSplineComponent* Road = Cast<UCSRoadSplineComponent>(Component);
		const FTransform ComponentToWorld = Component->GetComponentTransform();
		// Reading a spline's points and names took ~10 ms for Palmsland's 3,894 splines: a road spline
		// that has had no notice since, in the same place, is taken as read then.
		FCSRoadNetworkActor_SplineCache::FEntry Cached;
		if (Road && SplineCache->Entries.RemoveAndCopyValue(Component, Cached) && Cached.Matches(*Road, ComponentToWorld, NetworkToWorld))
		{
			Splines.Add(Cached.Road);
			Kept.Add(Component, MoveTemp(Cached));
			SplineCache->Hits++;
			continue;
		}
		auto Position = [&](const FVector& Local) { return CSRoadNetworkActor_ToUnity(NetworkToWorld.InverseTransformPosition(ComponentToWorld.TransformPosition(Local))); };
		auto Direction = [&](const FVector& Local) { return CSRoadNetworkActor_ToUnity(NetworkToWorld.InverseTransformVector(ComponentToWorld.TransformVector(Local))); };
		CSRoad::FSplineRoad& Out = Splines.AddDefaulted_GetRef();
		Out.Name = Component->GetOwner() ? Component->GetOwner()->GetActorNameOrLabel() : Component->GetName();
		// Actor and component names, the same in every session and in PIE (no package or world).
		const FString Identity = Component->GetPathName(Component->GetOwner() ? Component->GetOwner()->GetOuter() : nullptr);
		Out.Id = FXxHash64::HashBuffer(*Identity, Identity.Len() * sizeof(TCHAR)).Hash;
		Out.Prefab = Road ? Road->Prefab : DefaultSplinePrefab;
		Out.Upgrade = Road ? CSRoadNetworkActor_Flags(Road->Upgrade) : CSRoad::CompositionFlags{};
		Out.bAdjust = Road ? Road->bAdjustSegments : true;
		Out.bAlley = Road && Road->Tier == ECSRoadTier::Alley;
		Out.bClosed = Component->IsClosedLoop();
		for (int32 I = 0; I < Count; I++)
		{
			Out.Points.Add(Position(Component->GetLocationAtSplinePoint(I, ESplineCoordinateSpace::Local)));
			Out.Arrive.Add(Direction(Component->GetArriveTangentAtSplinePoint(I, ESplineCoordinateSpace::Local)));
			Out.Leave.Add(Direction(Component->GetLeaveTangentAtSplinePoint(I, ESplineCoordinateSpace::Local)));
			const ESplinePointType::Type Type = Component->GetSplinePointType(I);
			Out.Linear.Add(Type == ESplinePointType::Linear || Type == ESplinePointType::Constant);
			CSRoad::FSplinePointData& Data = Out.Data.AddDefaulted_GetRef();
			if (!Road || !Road->RoadMetadata || !Road->RoadMetadata->Points.IsValidIndex(I)) continue;
			const FCSRoadSplinePoint& Point = Road->RoadMetadata->Points[I];
			Data.NodeOffset = Direction(Point.NodeOffset);
			Data.NodePrefab = Point.NodePrefab;
			Data.bNodeElevation = Point.bNodeElevation;
			Data.NodeElevation = CSRoad::float2(static_cast<float>(Point.NodeElevation.X), static_cast<float>(Point.NodeElevation.Y));
			Data.NodeUpgrade = CSRoadNetworkActor_Flags(Point.NodeUpgrade);
			Data.bSegmentElevation = Point.bSegmentElevation;
			Data.SegmentElevation = CSRoad::float2(static_cast<float>(Point.SegmentElevation.X), static_cast<float>(Point.SegmentElevation.Y));
			Data.bOutside = Point.bOutsideConnection;
		}
		if (!Road) continue;
		FCSRoadNetworkActor_SplineCache::FEntry& Entry = Kept.Add(Component);
		Entry.Revision = Road->Revision;
		Entry.NumPoints = Count;
		Entry.bClosed = Out.bClosed;
		Entry.Prefab = Road->Prefab;
		Entry.Tier = Road->Tier;
		Entry.bAdjust = Road->bAdjustSegments;
		Entry.Upgrade = Road->Upgrade;
		Entry.ComponentToWorld = ComponentToWorld;
		Entry.NetworkToWorld = NetworkToWorld;
		Entry.ComponentName = Component->GetFName();
		Entry.OwnerName = Component->GetOwner() ? Component->GetOwner()->GetFName() : NAME_None;
		Entry.Road = Out;
	}
	SplineCache->Entries = MoveTemp(Kept);
	return Splines;
}

void ACSRoadNetworkActor::AddGarageRoads(TArray<CSRoad::FSplineRoad>& Splines, CSRoad::FAssets& Assets)
{
	NumGarages = 0;
	NumJoinedGarages = 0;
	UWorld* World = GetWorld();
	if (!World) return;
	const FTransform NetworkToWorld = GetActorTransform();
	auto ToNetwork = [&NetworkToWorld](const FVector& Point) { return CSRoadNetworkActor_ToUnity(NetworkToWorld.InverseTransformPosition(Point)); };
	// Every building's footprint is in the way of the driveways.
	TArray<ACSCityBuildingActor*> Buildings;
	TArray<CSCity::FObstacle> Obstacles;
	for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
	{
		if (NetworkOf(Building) != this) continue;
		CSCity::FObstacle& Obstacle = Obstacles.AddDefaulted_GetRef();
		Obstacle.Name = Building->GetActorNameOrLabel();
		for (const FVector2D& Corner : Building->Footprint())
		{
			const FVector3d Point = ToNetwork(FVector(Corner, Building->GetActorLocation().Z));
			Obstacle.Outline.Add(FVector2d(Point.X, Point.Z));
		}
		Buildings.Add(Building);
	}
	TArray<CSRoad::FSplineRoad> Driveways;
	for (int32 B = 0; B < Buildings.Num(); B++)
	{
		ACSCityBuildingActor* Building = Buildings[B];
		Building->GarageStatus.Reset();
		FVector Door;
		FVector Facing;
		if (!Building->GarageDoor(Door, Facing)) continue;
		NumGarages++;
		CSCity::FGarage Garage;
		Garage.Name = Building->GetActorNameOrLabel() + TEXT(" driveway");
		// The building's actor name, the same in every session (as the spline roads' ids).
		const FString Identity = Building->GetPathName(Building->GetOuter()) + TEXT(".Garage");
		Garage.Id = FXxHash64::HashBuffer(*Identity, Identity.Len() * sizeof(TCHAR)).Hash;
		Garage.Prefab = Building->GarageRoad;
		Garage.Door = ToNetwork(Door);
		const FVector3d Ahead = ToNetwork(Door + Facing * 100.0) - Garage.Door;
		Garage.Facing = FVector2d(Ahead.X, Ahead.Z).GetSafeNormal();
		Garage.Reach = Building->GarageReach;
		Garage.Clearance = SplineSettings.AlleyClearance;
		Garage.Own = B;
		CSRoad::FSplineRoad Driveway;
		if (!CSCity::Driveway(Garage, Splines, Obstacles, Assets, Driveway, Building->GarageStatus)) continue;
		NumJoinedGarages++;
		if (Driveway.Points.Num() > 0) Driveways.Add(MoveTemp(Driveway));
	}
	Splines.Append(MoveTemp(Driveways));
}

bool ACSRoadNetworkActor::ConvertToSplines()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World) return false;
	FString Error;
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(ResolveAssetDirectory(), Error);
	const TSharedPtr<const CSRoad::FSavedNetwork> Network = Library ? CSRoadNetworkActor_Network(ResolveNetworkFile(), Library->Assets, false, Error) : nullptr;
	if (!Network)
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
		return false;
	}
	for (ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World))
	{
		if (Road->GetAttachParentActor() != this) continue;
		LastError = TEXT("Spline roads already hang under this network; delete them before converting again");
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *LastError);
		return false;
	}
	TArray<CSRoad::FSplineRoad> Splines;
	CSRoad::ChainNetwork(Network->Nodes, Network->Roads, Splines);

	const FScopedTransaction Transaction(NSLOCTEXT("CSRoadNet", "ConvertToSplines", "Convert Road Network to Splines"));
	Modify();
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetLevel();
	for (const CSRoad::FSplineRoad& Road : Splines)
	{
		ACSRoadSplineActor* Actor = World->SpawnActor<ACSRoadSplineActor>(ACSRoadSplineActor::StaticClass(), FTransform::Identity, Parameters);
		if (!Actor || !Actor->Spline) continue;
		// The actor sits on the road's first point; its points are relative to it.
		const FVector First = CSRoadNetworkActor_ToUnreal(Road.Points[0]);
		Actor->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
		Actor->SetActorRelativeTransform(FTransform(First));
		Actor->SetActorLabel(Road.Name);
		UCSRoadSplineComponent* Spline = Actor->Spline;
		Spline->Prefab = Road.Prefab;
		Spline->Upgrade = CSRoadNetworkActor_Flags(Road.Upgrade);
		Spline->bAdjustSegments = Road.bAdjust;
		Spline->ClearSplinePoints(false);
		for (int32 I = 0; I < Road.Points.Num(); I++) Spline->AddSplinePoint(CSRoadNetworkActor_ToUnreal(Road.Points[I]) - First, ESplineCoordinateSpace::Local, false);
		for (int32 I = 0; I < Road.Points.Num(); I++) Spline->SetTangentsAtSplinePoint(I, CSRoadNetworkActor_ToUnreal(Road.Arrive[I]), CSRoadNetworkActor_ToUnreal(Road.Leave[I]), ESplineCoordinateSpace::Local, false);
		Spline->SetClosedLoop(Road.bClosed, false);
		Spline->UpdateSpline();
		TArray<FCSRoadSplinePoint>& Points = Spline->RoadMetadata->Points;
		Points.SetNum(Road.Points.Num());
		for (int32 I = 0; I < Road.Points.Num(); I++)
		{
			const CSRoad::FSplinePointData& Data = Road.Data[I];
			Points[I].NodeOffset = CSRoadNetworkActor_ToUnreal(Data.NodeOffset);
			Points[I].NodePrefab = Data.NodePrefab;
			Points[I].bNodeElevation = Data.bNodeElevation;
			Points[I].NodeElevation = FVector2D(Data.NodeElevation.x, Data.NodeElevation.y);
			Points[I].NodeUpgrade = CSRoadNetworkActor_Flags(Data.NodeUpgrade);
			Points[I].bSegmentElevation = Data.bSegmentElevation;
			Points[I].SegmentElevation = FVector2D(Data.SegmentElevation.x, Data.SegmentElevation.y);
			Points[I].bOutsideConnection = Data.bOutside;
		}
	}
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d edges of %s converted to %d spline roads"), *GetName(), Network->Roads.Num(), *ResolveNetworkFile(), Splines.Num());
	bLeftHandTraffic = Network->bLeftHandTraffic;
	Rebuild();
	return true;
#else
	return false;
#endif
}

AActor* ACSRoadNetworkActor::ConvertTerrainToLandscape(UMaterialInterface* Material)
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World) return nullptr;
	FString Error;
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(ResolveAssetDirectory(), Error);
	const TSharedPtr<const CSRoad::FSavedNetwork> Snapshot = Library ? CSRoadNetworkActor_Network(ResolveNetworkFile(), Library->Assets, true, Error) : nullptr;
	if (Snapshot && !Snapshot->Terrain.IsValid()) Error = TEXT("The snapshot has no terrain");
	else if (Snapshot && TActorIterator<ALandscapeProxy>(World)) Error = TEXT("The level has a landscape already; delete it before converting again");
	if (!Snapshot || !Error.IsEmpty())
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
		return nullptr;
	}
	// A vertex per texel of the playable map, as many whole components as it holds, cropped evenly; the rows
	// run the other way (Unity z is UE -Y).
	const CSRoad::FSavedTerrain& Terrain = Snapshot->Terrain;
	const CSRoad::FSavedTerrain::FMap& Map = Terrain.Playable;
	constexpr int32 QuadsPerSection = 127;
	constexpr int32 Sections = 2;
	const int32 Components = (FMath::Min(Map.Width, Map.Height) - 1) / (QuadsPerSection * Sections);
	if (Components < 1)
	{
		LastError = TEXT("The snapshot's terrain map is too small for a landscape component");
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *LastError);
		return nullptr;
	}
	const int32 Size = Components * QuadsPerSection * Sections + 1;
	const int32 I0 = (Map.Width - Size) / 2;
	const int32 J0 = (Map.Height - Size) / 2;
	TArray<uint16> Heights;
	Heights.SetNumUninitialized(Size * Size);
	for (int32 Y = 0; Y < Size; Y++) FMemory::Memcpy(&Heights[Y * Size], &Map.Data[(J0 + Size - 1 - Y) * Map.Width + I0], Size * sizeof(uint16));
	// The texels' centres in the network's space as UE cm. A landscape height unit is Scale.Z / 128 cm from
	// 32768, a map's HeightScaleOffset.x / 65535 m from 0; half a metre lower.
	const double TexelX = double(Map.Size.x) / Map.Width;
	const double TexelZ = double(Map.Size.y) / Map.Height;
	const double Unit = double(Terrain.HeightScaleOffset.x) / 65535.0 * 100.0;
	const FVector Location((Map.Offset.x + (I0 + 0.5) * TexelX) * 100.0, -(Map.Offset.y + (J0 + Size - 0.5) * TexelZ) * 100.0, double(Terrain.HeightScaleOffset.y) * 100.0 + 32768.0 * Unit - 50.0);
	const FTransform Placement = FTransform(FQuat::Identity, Location, FVector(TexelX * 100.0, TexelZ * 100.0, Unit * 128.0)) * GetActorTransform();

	const FScopedTransaction Transaction(NSLOCTEXT("CSRoadNet", "ConvertTerrainToLandscape", "Convert Road Network Terrain to Landscape"));
	FActorSpawnParameters Parameters;
	Parameters.OverrideLevel = GetLevel();
	ALandscape* Landscape = World->SpawnActor<ALandscape>(ALandscape::StaticClass(), FTransform(Placement.GetRotation(), Placement.GetLocation()), Parameters);
	if (!Landscape) return nullptr;
	Landscape->LandscapeMaterial = Material ? Material : LoadObject<UMaterialInterface>(nullptr, TEXT("/PCGPlugins/CSRoadNet/M_SplineRoadDemoGround.M_SplineRoadDemoGround"));
	Landscape->SetActorRelativeScale3D(Placement.GetScale3D());
	// As the landscape editor does: a lighting LOD that keeps Lightmass in bounds.
	Landscape->StaticLightingLOD = FMath::DivideAndRoundUp(FMath::CeilLogTwo((Size * Size) / (2048 * 2048) + 1), uint32(2));
	TMap<FGuid, TArray<uint16>> HeightData;
	HeightData.Add(FGuid(), MoveTemp(Heights));
	TMap<FGuid, TArray<FLandscapeImportLayerInfo>> LayerData;
	LayerData.Add(FGuid());
	Landscape->Import(FGuid::NewGuid(), 0, 0, Size - 1, Size - 1, Sections, QuadsPerSection, HeightData, TEXT(""), LayerData, ELandscapeImportAlphamapType::Additive, TArrayView<const FLandscapeLayer>());
	if (ULandscapeInfo* Info = Landscape->GetLandscapeInfo()) Info->UpdateLayerInfoMap(Landscape);
	Landscape->SetActorLabel(GetActorLabel() + TEXT(" Terrain"));
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: the terrain of %s as a %d x %d landscape (%d x %d components), %.1f m a quad"), *GetName(), *ResolveNetworkFile(), Size, Size, Components, Components, TexelX);
	return Landscape;
#else
	return nullptr;
#endif
}

#if WITH_EDITOR
void ACSRoadNetworkActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FProperty* Property = PropertyChangedEvent.MemberProperty;
	if (Property && Property->GetMetaData(TEXT("Category")) == TEXT("CS2 Road|Traffic"))
	{
		// Visibility and simulation settings do not invalidate the road's GPU buffers.
		if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ACSRoadNetworkActor, bShowTraffic)) SetShowTraffic(bShowTraffic);
		else ResetTraffic();
		return;
	}
	if (Property && Property->GetMetaData(TEXT("Category")) == TEXT("CS2 Road|Terrain"))
	{
		// The terrain layer follows the whole network's preview, made again shortly (a slider dragged settles
		// first); the roads stay as they are.
		bPreviewStale = true;
		LastBuildSeconds = FPlatformTime::Seconds();
		return;
	}
	// A slider dragged in the details panel changes a property many times a frame: the network builds
	// once on the next tick.
	RequestRebuild();
}

void ACSRoadNetworkActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// The resident streams are world space, so the geometry follows the actor only by rebuilding, on
	// every tick while it is dragged.
	RequestRebuild();
}
#endif

void ACSRoadNetworkActor::Rebuild()
{
	// A build waiting for the GPU gives way: it has written nothing yet.
	TileJob.Reset();
	RunBuild(true);
}

bool ACSRoadNetworkActor::IsBuilding() const
{
	return TileJob.IsValid();
}

void ACSRoadNetworkActor::RunBuild(bool bWait)
{
	const double StartTime = FPlatformTime::Seconds();
	// What spline roads asked for until now is in this build. The build makes the preview again, or,
	// building in tiles, RefreshPreview does once edits settle; until then the last one stays up.
	bRebuildRequested = false;
	bWaitingForVideoMemory = false;
	FString Error;
	const bool bBuilt = BuildRoads(bWait, Error);
	const double Milliseconds = (FPlatformTime::Seconds() - StartTime) * 1000.0;
	// Left waiting for the GPU: Tick ends it.
	if (bBuilt && TileJob)
	{
		TileJob->OtherMs = Milliseconds - TileJob->GameMs;
		return;
	}
	EndBuild(bBuilt, Error, Milliseconds);
}

void ACSRoadNetworkActor::EndBuild(bool bBuilt, const FString& Error, double Milliseconds)
{
	if (bBuilt) LastError.Reset();
	else
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
	}
	UpdateBakeStats();
	PendingTiles = (TileState ? TileState->NumPending() : 0) + PendingBuildingTiles;
	// The tiles past the batch go on the next ticks, until none are left or a build fails. An edit made
	// while the build waited for the GPU has asked for a whole build already.
	if (!bRebuildRequested)
	{
		bPendingOnly = PendingTiles > 0 && LastError.IsEmpty();
		bRebuildRequested = bPendingOnly;
	}
	LastBuildSeconds = FPlatformTime::Seconds();
	BuildMilliseconds = float(Milliseconds);
}

void ACSRoadNetworkActor::RefreshPreview()
{
	bPreviewStale = false;
	if (!PreviewRoads || PreviewRoads->Roads.Num() == 0)
	{
		ClearPreview();
		return;
	}
	const double StartTime = FPlatformTime::Seconds();
	FString Error;
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(ResolveAssetDirectory(), Error);
	const CSRoad::FSavedTerrain* Terrain = PreviewTerrain && PreviewTerrain->Terrain.IsValid() ? &PreviewTerrain->Terrain : nullptr;
	CSRoad::FGraph Graph;
	Graph.bLeftHandTraffic = PreviewRoads->bLeftHandTraffic;
	CSRoad::FGpuGeometryInput Geometry;
	CSRoad::FGpuRoadMeshInput NoInstances;
	CSRoad::FGpuGeometryResult Result;
	if (!Library || !Graph.Build(Library->Assets, PreviewRoads->Roads, PreviewRoads->Nodes, Error) || !Geometry.Build(Graph, Library->Assets, Terrain, Error)
		|| !CSRoad::RunRefineCountsBlocking(Geometry, NoInstances, Error, &Result))
	{
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: the traffic and bridge preview could not be made: %s"), *GetName(), *Error);
		return;
	}
	CSRoad::FRoadLanes Lanes;
	Lanes.Build(Graph, Library->Assets, Result);
	ClearPreview();
	BuildPreview(Graph, Lanes, Result, Terrain);
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: traffic and bridge preview of %d roads made again; %.0f ms"), *GetName(), Graph.Roads.Num(), (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

bool ACSRoadNetworkActor::BuildRoads(bool bWait, FString& OutError)
{
	if (!EnsureContent())
	{
		OutError = TEXT("No world to build in");
		return false;
	}
	double LapStart = FPlatformTime::Seconds();
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};
	TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(ResolveAssetDirectory(), OutError);
	if (!Library) return false;
	const double LibraryMs = Lap();
	// The snapshot lends its terrain.
	TSharedPtr<const CSRoad::FSavedNetwork> Snapshot;
	if (bUseTerrain && !NetworkFile.FilePath.IsEmpty())
	{
		Snapshot = CSRoadNetworkActor_Network(ResolveNetworkFile(), Library->Assets, true, OutError);
		if (!Snapshot) return false;
	}
	const CSRoad::FSavedTerrain* Terrain = Snapshot && Snapshot->Terrain.IsValid() ? &Snapshot->Terrain : nullptr;
	// The spline roads and their network. A rebuild that only writes pending tiles (nothing changed
	// since the last one) takes the last build's as they are.
	TSharedPtr<const TArray<CSRoad::FSplineRoad>> Splines;
	TSharedPtr<const CSRoad::FSavedNetwork> Network;
	if (bReuseNetwork && LastSplines && PreviewRoads)
	{
		Splines = LastSplines;
		Network = PreviewRoads;
	}
	else
	{
		TSharedPtr<TArray<CSRoad::FSplineRoad>> Gathered = MakeShared<TArray<CSRoad::FSplineRoad>>(GatherSplines());
		AddGarageRoads(*Gathered, Library->Assets);
		const double GatherMs = Lap();
		TSharedPtr<CSRoad::FSavedNetwork> Built = MakeShared<CSRoad::FSavedNetwork>();
		CSRoad::FSplineNetworkSettings Settings;
		Settings.MaxEdgeLength = SplineSettings.MaxSegmentLength;
		Settings.MaxEdgeTurn = FMath::DegreesToRadians(static_cast<double>(SplineSettings.MaxSegmentTurn));
		Settings.MinEdgeLength = SplineSettings.MinSegmentLength;
		Settings.SnapDistance = SplineSettings.SnapDistance;
		Settings.JunctionHeight = SplineSettings.JunctionHeight;
		Settings.AlleyClearance = SplineSettings.AlleyClearance;
		CSRoad::FSplineNetworkStats Stats;
		CSRoad::FSplineNetworkCache& Cache = SplineCache->Network;
		if (!bIncrementalSplines) Cache.Reset();
		if (!CSRoad::BuildSplineNetwork(Library->Assets, *Gathered, Settings, Terrain, Built->Nodes, Built->Roads, Stats, OutError, bIncrementalSplines ? &Cache : nullptr, bVerifyIncremental)) return false;
		Built->bLeftHandTraffic = bLeftHandTraffic;
		// Converting only around the splines that changed, the counts are of the splines converted.
		FString Around;
		if (!Stats.bWhole && Stats.Changed == 0) Around = TEXT("; no spline changed");
		else if (!Stats.bWhole) Around = FString::Printf(TEXT("; %d converted around %d changed in %d rounds (finding them %.1f ms, putting the edges in %.1f ms)"), Stats.Converted, Stats.Changed, Stats.Rounds, Stats.DiffMs, Stats.MergeMs);
		else if (Stats.Changed > 0) Around = FString::Printf(TEXT("; all converted, %d changed (%.1f ms finding them and trying %d rounds)"), Stats.Changed, Stats.DiffMs, Stats.Rounds);
		if (Cache.Diff.bValid) Around += FString::Printf(TEXT(", %d edges and %d nodes came, %d and %d went"), Cache.Diff.AddedRoads.Num(), Cache.Diff.AddedNodes.Num(), Cache.Diff.RemovedRoads.Num(), Cache.Diff.RemovedNodes.Num());
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d splines make %d nodes and %d edges; %d alley stretches cut under main roads, %d junctions joined, %d segments split, %d merged, %d too short left out; gathering %.1f ms (finding them %.1f ms, %d as read before), ordering %.1f ms, alleys %.1f ms, nodes %.1f ms, roads ending on roads %.1f ms, crossings %.1f ms, merging and splitting %.1f ms, snapshot form %.1f ms%s"),
			*GetName(), Gathered->Num(), Built->Nodes.Num(), Built->Roads.Num(), Stats.AlleyCuts, Stats.Junctions, Stats.Splits, Stats.Merges, Stats.Dropped,
			GatherMs, SplineCache->FindMs, SplineCache->Hits, Stats.SortMs, Stats.AlleyMs, Stats.NodeMs, Stats.JoinMs, Stats.CrossMs, Stats.ShapeMs, Stats.OutputMs, *Around);
		Splines = Gathered;
		Network = Built;
	}
	NumSplines = Splines->Num();
	const TArray<CSRoad::FSavedNode>& Nodes = Network->Nodes;
	const TArray<CSRoad::FRoadSpec>& Specs = Network->Roads;
	const bool bLeftHand = Network->bLeftHandTraffic;
	const double NetworkMs = Lap();
	// The buildings need only the roads' curves: they stand even if the road meshes fail below. They
	// log their own time.
	if (bBatchBuildings) BuildBatchBuildings(*Splines, Specs, Library->Assets, Terrain);
	else ClearBuildingTiles();
	(void)Lap();
	const bool bMarkings = bBuildMarkings;
	// One material slot per atlas (decal surface); MeshTriangleMaterials indexes these.
	const TMap<FString, UMaterialInterface*> ByBaseColor = CSRoadNetworkActor_Materials(MaterialFolder.Path);
	MissingAtlases.Reset();
	MissingMarkingSurfaces.Reset();
	NumMarkingInstances = 0;
	const double MaterialMs = Lap();
	if (bLocalUpdate)
	{
		TUniquePtr<FCSRoadNetworkActor_TileJob> Job = MakeUnique<FCSRoadNetworkActor_TileJob>();
		Job->Library = Library;
		Job->Network = Network;
		Job->Snapshot = Terrain ? Snapshot : nullptr;
		Job->ByBaseColor = ByBaseColor;
		Job->bMarkings = bMarkings;
		Job->bWait = bWait;
		const bool bBuilt = AdvanceTiles(*Job, OutError);
		// Waiting for the GPU: Tick goes on with it.
		if (bBuilt && Job->Stage != FCSRoadNetworkActor_TileJob::EStage::Done) TileJob = MoveTemp(Job);
		// What RefreshPreview makes the preview from once edits settle.
		PreviewRoads = Network;
		LastSplines = Splines;
		PreviewTerrain = Terrain ? Snapshot : nullptr;
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: prefab data %.0f ms, splines %.0f ms, materials %.1f ms, tiles %.0f ms"), *GetName(), LibraryMs, NetworkMs, MaterialMs, Lap());
		return bBuilt;
	}
	ClearTiles();
	PreviewRoads.Reset();
	LastSplines.Reset();
	PreviewTerrain.Reset();
	bPreviewStale = false;
	CSRoad::FGraph Graph;
	Graph.bLeftHandTraffic = bLeftHand;
	CSRoad::FGpuGeometryInput Geometry;
	if (!Graph.Build(Library->Assets, Specs, Nodes, OutError)) return false;
	const double GraphMs = Lap();
	if (!Geometry.Build(Graph, Library->Assets, Terrain, OutError)) return false;
	const double InputMs = Lap();
	CSRoad::FGpuRoadMeshInput MeshInput;
	if (!MeshInput.Build(Graph, Library->Assets, OutError)) return false;
	const double MeshMs = Lap();
	// First graph: the refined node meshes' size is decided on the GPU, and the lane graph and the
	// decal setup run on the CPU from the GPU geometry. The geometry is kept for the mesh graph.
	CSRoad::FGpuGeometryResult GeometryResult;
	CSRoad::FPooledGeometry SharedGeometry;
	// Cache the lane graph even with markings/traffic hidden, so the visibility switch is instant.
	if (!CSRoad::RunRefineCountsBlocking(Geometry, MeshInput, OutError, &GeometryResult, &SharedGeometry)) return false;
	if (MeshInput.NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes%s"), *GetName(), MeshInput.NumUnconverged, *CSRoadNetworkActor_Unconverged(Graph, MeshInput, GetActorTransform()));
	const double RefineMs = Lap();
	CSRoad::FGpuDecalInput Decals;
	if (bMarkings && !Decals.Build(Graph, Library->Assets, GeometryResult, MeshInput, ResolveAssetDirectory(), OutError)) return false;
	const double DecalSetupMs = Lap();
	if (MeshInput.NumVertices == 0 || MeshInput.NumIndices == 0)
	{
		OutError = TEXT("The network has no road surface");
		return false;
	}

	FMeshTargets Targets;
	Targets.Road = Content->RoadMesh;
	Targets.RoadMesh = &Mesh;
	if (bMarkings)
	{
		ResolveDecalMaterials(Decals, ByBaseColor, Library->Assets);
		Targets.Decals = &Decals;
	}
	if (!WriteMeshes(Geometry, SharedGeometry, MeshInput, ByBaseColor, Library->Assets, Targets, OutError)) return false;
	(void)Lap();
	if (bMarkings)
	{
		const int32 Drawn = DrawDecals(Decals, Targets.Fit, MarkingFits, Content->MarkingDecals, TEXT("Marking"));
		if (Drawn >= 0) NumMarkingInstances = Drawn;
	}
	else
	{
		MarkingFits.Reset();
		DestroyComponents(Content->MarkingDecals);
	}
	const double DecalMs = Lap();
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads, %u vertices, %u triangles, %d compositions, %d decals (%d lanes, %d instances); prefab data %.0f ms, splines %.0f ms, graph and compositions %.0f ms, geometry input %.0f ms, composition meshes %.0f ms, refinement counts %.0f ms, lanes and decal setup %.0f ms, allocation %.0f ms, road GPU build %.0f ms, decals %.0f ms"),
		*GetName(), Graph.Roads.Num(), MeshInput.NumVertices, MeshInput.NumIndices / 3, Graph.Compositions.Num() - 1, Decals.Items.Num(), Decals.Lanes.Lanes.Num(), NumMarkingInstances,
		LibraryMs, NetworkMs, GraphMs, InputMs, MeshMs, RefineMs, DecalSetupMs, Targets.AllocationMs, Targets.RoadMs, DecalMs);

	NumRoads = Graph.Roads.Num();
	NumVertices = int32(MeshInput.NumVertices);
	NumTriangles = int32(MeshInput.NumIndices / 3);
	CSRoad::FRoadLanes PreviewLanes;
	if (!bMarkings) PreviewLanes.Build(Graph, Library->Assets, GeometryResult);
	ClearPreview();
	BuildPreview(Graph, bMarkings ? Decals.Lanes : PreviewLanes, GeometryResult, Terrain);
	return true;
}

bool ACSRoadNetworkActor::AdvanceTiles(FCSRoadNetworkActor_TileJob& Job, FString& OutError)
{
	using EStage = FCSRoadNetworkActor_TileJob::EStage;
	const double Start = FPlatformTime::Seconds();
	double LapStart = Start;
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};
	ON_SCOPE_EXIT { Job.GameMs += (FPlatformTime::Seconds() - Start) * 1000.0; };
	if (!TileState) TileState = MakeUnique<CSRoad::FTileState>();
	auto Fail = [this]()
	{
		TileState->Reset();
		return false;
	};
	CSRoad::FAssets& Assets = Job.Library->Assets;
	const TArray<CSRoad::FRoadSpec>& Specs = Job.Network->Roads;
	const TArray<CSRoad::FSavedNode>& Nodes = Job.Network->Nodes;
	const bool bLeftHand = Job.Network->bLeftHandTraffic;
	const CSRoad::FSavedTerrain* Terrain = Job.Terrain();
	while (Job.Stage != EStage::Done)
	{
		// A sub-graph's GPU round trip on its way: landed, or (not waiting) the job waits for a later tick.
		if (FCSRoadNetworkActor_Subgraph* Waiting = Job.Waiting())
		{
			double& StageMs = Waiting == &Job.Detected ? Job.DetectMs : Job.GraphMs;
			if (Job.bWait)
			{
				StageMs += Lap();
				Waiting->Readback->Wait();
				Job.WaitMs += Lap();
			}
			else if (!Waiting->Readback->Poll())
			{
				Job.Frames++;
				return true;
			}
			if (!CSRoadNetworkActor_LandSubgraph(*Waiting, OutError)) return Fail();
			StageMs += Lap();
		}

		if (Job.Stage == EStage::Detect)
		{
			// The mesh of a whole build gives way to the tiles, its VRAM now rather than at the next GC: a
			// network the size of Palmsland holds over half a gigabyte that the tiles are about to ask for again.
			if (Content->RoadMesh && Content->RoadMesh->GetGpuMesh()) Content->RoadMesh->SetGpuMesh(nullptr);
			if (Mesh) Mesh->ReleaseDeferred();
			Mesh = nullptr;
			MarkingFits.Reset();
			DestroyComponents(Content->MarkingDecals);

			Job.Signature = TileSignature(bLeftHand);
			Job.bEverything = !TileState->Matches(Job.Signature);
			Job.Tiles.Build(Specs, Nodes, double(TileSize));
			if (Job.bEverything) for (int32 Road = 0; Road < Specs.Num(); Road++) Job.Everything.Add(Road);
			// The roads whose output may differ from the last build's, built with two rings more: they come out
			// as in the whole network (Subgraph.Reach). Every road the first time, or after a change of settings.
			Job.Affected = Job.bEverything ? Job.Everything : TileState->Affected(Job.Tiles);
			Job.NetMs = Lap();
			if (Job.Affected.Num() > 0)
			{
				// After an edit the sub-graph serves the drawing too, and comes back with the refinement counts: one
				// GPU round trip instead of three. The tiles to draw again are those of the roads at a node of a
				// road whose output changed (affected) and those of the roads that went; it holds their roads and
				// three rings more (the drawing's reach, two of them the detection's), and counts the instances of
				// their roads and of the roads at their nodes (what those tiles write).
				TArray<int32> Taken = Job.Everything;
				TArray<int32> Counting;
				if (!Job.bEverything)
				{
					TSet<FIntPoint> Reach = TileState->TilesGone(Job.Tiles);
					for (const int32 Road : Job.Tiles.Around(Job.Affected, {}, 1)) Reach.Add(Job.Tiles.Roads[Road].Tile);
					TArray<int32> InReach;
					for (int32 Road = 0; Road < Job.Tiles.Roads.Num(); Road++) if (Reach.Contains(Job.Tiles.Roads[Road].Tile)) InReach.Add(Road);
					Taken = Job.Tiles.Around(InReach, {}, 3);
					Counting = Job.Tiles.Around(InReach, {}, 1);
				}
				if (!CSRoadNetworkActor_BuildSubgraph(Assets, Specs, Nodes, bLeftHand, Terrain, Job.Snapshot, MoveTemp(Taken), Job.Detected, OutError,
					Job.bEverything ? nullptr : &Counting, &TileState->Compositions)) return Fail();
			}
			Job.DetectMs += Lap();
			Job.Stage = EStage::Batch;
			continue;
		}

		if (Job.Stage == EStage::Batch)
		{
			Job.Outputs.Reserve(Job.Affected.Num());
			for (const int32 Road : Job.Affected) Job.Outputs.Add(CSRoad::TileOutput(Job.Detected.Graph, Job.Detected.Result, Job.Detected.RoadOfSpec[Road], Job.Tiles.Roads[Road].Key));
			Job.DetectMs += Lap();
			TArray<FIntPoint> Existing;
			Tiles.GetKeys(Existing);
			FVector2D Focus;
			const bool bFocus = TileFocus(Focus);
			Job.Dirty = TileState->Dirty(Job.Tiles, Job.Affected, Job.Outputs, Job.bEverything, Existing, Job.Edited, Job.Fresh, bFocus ? &Focus : nullptr);
			// One batch of the tiles to rebuild, the ones edits dirtied first; the rest stay pending. A build an
			// edit made writes only edits' tiles, so the backlog does not hold it up; the builds after it take the
			// backlog. While tiles bake into Nanite, the batch takes what the GPU mesh budget has room for (a tile
			// drawn now gives back what its GPU mesh holds), and one tile regardless while no GPU mesh holds any.
			const int32 Candidates = Job.Fresh > 0 ? Job.Edited : Job.Dirty.Num();
			Job.Budget = GpuMeshBudgetBytes();
			NextTileBytes = 0;
			int64 Held = Job.Budget < MAX_int64 ? GpuMeshBytes() : 0;
			double TrianglesPerRoad = 2350.0;
			if (Job.Budget < MAX_int64)
			{
				int64 Triangles = 0;
				int64 Drawn = 0;
				for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles)
				{
					Triangles += Pair.Value.Triangles;
					Drawn += Pair.Value.Roads;
				}
				if (Drawn > 0) TrianglesPerRoad = double(Triangles) / double(Drawn);
			}
			int32 Index = 0;
			for (const TPair<FIntPoint, TArray<int32>>& Pair : Job.Dirty)
			{
				if (Index++ >= Candidates || (MaxTilesPerBatch > 0 && Job.Written.Num() >= MaxTilesPerBatch)) break;
				// A tile whose mesh's last edit is still on its way waits for a later batch: writing it now would
				// wait for that edit.
				const FCSRoadTile* Known = Tiles.Find(Pair.Key);
				if (!Job.bWait && Known && Known->RoadMesh && Known->RoadMesh->IsEditInFlight()) continue;
				if (Job.Budget < MAX_int64 && Pair.Value.Num() > 0)
				{
					const int64 Change = EstimateTileBytes(Known, Pair.Value.Num(), TrianglesPerRoad) - (Known ? TileGpuBytes(*Known) : 0);
					NextTileBytes = Change;
					if (Held > 0 && Held + Change > Job.Budget) break;
					Held += Change;
				}
				Job.Written.Add(Pair.Key);
				Job.Batch.Append(Pair.Value);
			}
			Job.Batch.Sort();
			Job.DirtyMs = Lap();
			if (UE_LOG_ACTIVE(LogCSRoadNet, Verbose))
			{
				FString Keys;
				for (const FIntPoint& Tile : Job.Written) Keys += FString::Printf(TEXT(" (%d, %d)"), Tile.X, Tile.Y);
				UE_LOG(LogCSRoadNet, Verbose, TEXT("%s: writing%s (%d roads)"), *GetName(), *Keys, Job.Batch.Num());
			}
			// What the batch draws, the roads at its roads' nodes (their lanes meet the batch's connectors) and
			// two rings more: in a sub-graph of those, the batch and its neighbours come out as in the whole
			// network. The detection's sub-graph serves when it holds them all.
			Job.Graphed = &Job.Detected;
			if (Job.Batch.Num() > 0)
			{
				const TArray<int32> Needed = Job.bEverything ? Job.Everything : Job.Tiles.Around(Job.Batch, {}, 3);
				if (!Job.Detected.Holds(Needed))
				{
					const TArray<int32> Counting = Job.Tiles.Around(Job.Batch, {}, 1);
					if (!CSRoadNetworkActor_BuildSubgraph(Assets, Specs, Nodes, bLeftHand, Terrain, Job.Snapshot, Needed, Job.Drawing, OutError, &Counting, &TileState->Compositions)) return Fail();
					Job.Graphed = &Job.Drawing;
				}
			}
			Job.GraphMs += Lap();
			Job.Stage = EStage::Write;
			continue;
		}

		// The materials as they stand now, for a job that waited ticks for the GPU.
		if (Job.Frames > 0) Job.ByBaseColor = CSRoadNetworkActor_Materials(MaterialFolder.Path);
		const TMap<FString, UMaterialInterface*>& ByBaseColor = Job.ByBaseColor;
		const bool bMarkings = Job.bMarkings;
		const FCSRoadNetworkActor_Subgraph* Graphed = Job.Graphed;
		const CSRoad::FGraph& Graph = Graphed->Graph;
		CSRoad::FRoadLanes Lanes;
		if (Job.Batch.Num() > 0 && (bMarkings || Job.bEverything)) Lanes.Build(Graph, Assets, Graphed->Result);
		const double LanesMs = Lap();

		// What each tile of the batch writes; every refined instance among them is counted in one graph.
		TArray<int32> RoadOfEntity;
		RoadOfEntity.Init(INDEX_NONE, Graph.NumEntities());
		for (int32 Road = 0; Road < Graph.Roads.Num(); Road++) RoadOfEntity[Graph.Roads[Road].Entity] = Road;
		struct FWork
		{
			FIntPoint Key = FIntPoint::ZeroValue;
			// The tile's roads in the sub-graph, in its (the specs') order.
			TArray<int32> Roads;
			TArray<CSRoad::FGpuInstanceRef> Drawn;
			// Written, not drawn: the receivers of its connectors and crosswalks from the other roads there.
			TArray<CSRoad::FGpuInstanceRef> Context;
		};
		TArray<FWork> Work;
		TArray<CSRoad::FGpuInstanceRef> All;
		TSet<int32> Listed;
		for (const FIntPoint& Key : Job.Written)
		{
			FWork& Item = Work.AddDefaulted_GetRef();
			Item.Key = Key;
			for (const int32 Road : Job.Dirty.FindChecked(Key)) Item.Roads.Add(Graphed->RoadOfSpec[Road]);
			if (Item.Roads.Num() == 0) continue;
			CSRoad::TileInstances(Graph, RoadOfEntity, Item.Roads, Item.Drawn, bMarkings ? &Item.Context : nullptr);
			for (const TArray<CSRoad::FGpuInstanceRef>* List : { &Item.Drawn, &Item.Context })
			{
				for (const CSRoad::FGpuInstanceRef& Ref : *List)
				{
					bool bListed = false;
					Listed.Add(Ref.Slot(), &bListed);
					if (!bListed) All.Add(Ref);
				}
			}
		}
		// Counted with the sub-graph; a batch past what it counted (rare) is counted again here, waiting.
		CSRoad::FGpuRoadMeshInput Recounted;
		const CSRoad::FGpuRoadMeshInput* Counted = &Recounted;
		if (Graphed->Counts(All)) Counted = &Graphed->Counted;
		else if (All.Num() > 0 && (!Recounted.BuildSubset(Graph, Assets, All, {}, &TileState->Compositions, OutError) || !CSRoad::RunRefineCountsBlocking(Graphed->Input, Recounted, OutError, nullptr, &Job.Graphed->Shared))) return Fail();
		if (Counted->NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes%s"), *GetName(), Counted->NumUnconverged, *CSRoadNetworkActor_Unconverged(Graph, *Counted, GetActorTransform()));
		const double CountMs = Lap();

		// Each tile: the roads in it drawn, and their decals. A tile whose GPU mesh, its size now known, does
		// not fit the budget while another GPU mesh holds some stops the batch: it and the rest stay pending.
		// Writes that wait land the edits still on their way first, whose state would land after theirs.
		if (Job.bWait) LandTileEdits();
		int32 Done = 0;
		// Of the tiles' time, allocating and editing their GPU meshes (with Rebuild, waiting for the render thread).
		double EditMs = 0.0;
		for (const FWork& Item : Work)
		{
			// Shared: an edit that does not wait keeps them until it has run.
			const TSharedRef<CSRoad::FGpuRoadMeshInput> SharedMesh = MakeShared<CSRoad::FGpuRoadMeshInput>();
			const TSharedRef<CSRoad::FGpuDecalInput> SharedDecals = MakeShared<CSRoad::FGpuDecalInput>();
			CSRoad::FGpuRoadMeshInput& MeshInput = *SharedMesh;
			CSRoad::FGpuDecalInput& Decals = *SharedDecals;
			if (Item.Drawn.Num() > 0 && (!MeshInput.BuildSubset(Graph, Assets, Item.Drawn, Item.Context, &TileState->Compositions, OutError) || !MeshInput.ApplyRefinementFrom(*Counted, OutError))) return Fail();
			if (MeshInput.NumDrawnIndices == 0)
			{
				RemoveTile(Item.Key);
				Done++;
				continue;
			}
			const FCSRoadTile* Known = Tiles.Find(Item.Key);
			const bool bKnown = Known != nullptr;
			const bool bHadRoad = Known && Known->Road;
			const int64 Held = Known ? TileGpuBytes(*Known) : 0;
			const int64 Bytes = CSRoadNetworkActor_GpuMeshBytes(MeshInput.NumVertices, MeshInput.NumIndices);
			if (Job.Budget < MAX_int64)
			{
				const int64 Others = GpuMeshBytes() - Held;
				NextTileBytes = Bytes;
				if (Others > 0 && Others + NextTileBytes > Job.Budget) break;
			}
			// Short of video memory: this tile and the rest wait as they are.
			if (VideoMemoryShort(Bytes - Held))
			{
				bWaitingForVideoMemory = true;
				break;
			}
			TBitArray<> Keep(false, Graph.Roads.Num());
			for (const int32 Road : Item.Roads) Keep[Road] = true;
			CSRoad::FDecalScope Scope;
			Scope.Lanes = &Lanes;
			Scope.Keep = &Keep;
			if (bMarkings && !Decals.Build(Graph, Assets, Graphed->Result, MeshInput, OutError, &Scope)) return Fail();
			FCSRoadTile& Tile = EnsureTile(Item.Key);
			FMeshTargets Targets;
			Targets.Road = Tile.Road;
			Targets.RoadMesh = &Tile.RoadMesh;
			if (!Job.bWait) Targets.SharedMesh = SharedMesh;
			if (bMarkings)
			{
				ResolveDecalMaterials(Decals, ByBaseColor, Assets);
				Targets.Decals = &Decals;
				if (!Job.bWait) Targets.SharedDecals = SharedDecals;
			}
			if (!WriteMeshes(Graphed->Input, Graphed->Shared, MeshInput, ByBaseColor, Assets, Targets, OutError))
			{
				if (!Targets.bRefused) return Fail();
				// The GPU mesh found too little video memory to grow: the tile stays as it was (a component made for it
				// goes again), pending with the rest.
				if (!bKnown) RemoveTile(Item.Key);
				else if (!bHadRoad)
				{
					Tile.Road->DestroyComponent();
					Tile.Road = nullptr;
					Tile.RoadMesh = nullptr;
				}
				OutError.Reset();
				bWaitingForVideoMemory = true;
				break;
			}
			VideoMemoryTaken += FMath::Max<int64>(Bytes - Held, 0);
			EditMs += Targets.AllocationMs + Targets.RoadMs;
			// A new GPU mesh: whatever was baked or baking is of the old one.
			Tile.WrittenSeconds = FPlatformTime::Seconds();
			Tile.bBakeFailed = false;
			Tile.Bake.Reset();
			Tile.Baking = nullptr;
			if (Tile.Nanite) Tile.Nanite->SetStaticMesh(nullptr);
			Tile.Roads = Item.Roads.Num();
			Tile.Vertices = int32(MeshInput.NumDrawnVertices);
			Tile.Triangles = int32(MeshInput.NumDrawnIndices / 3);
			if (bMarkings)
			{
				const int32 Drawn = DrawDecals(Decals, Targets.Fit, Tile.DecalFits, Tile.Decals, FString::Printf(TEXT("MarkingTile_%d_%d"), Item.Key.X, Item.Key.Y));
				if (Drawn >= 0) Tile.DecalInstances = Drawn;
			}
			else
			{
				Tile.DecalFits.Reset();
				DestroyComponents(Tile.Decals);
				Tile.DecalInstances = 0;
			}
			Done++;
		}
		Job.Written.SetNum(Done);
		Work.SetNum(Done);
		TileState->Commit(Job.Tiles, Job.Affected, Job.Outputs, Job.Signature, Job.Dirty, Job.Edited, Job.Written);
		const double TileMs = Lap();

		NumRoads = Specs.Num();
		NumTiles = Tiles.Num();
		TilesRebuilt = Work.Num();
		EditedTilesRebuilt = 0;
		{
			int32 Index = 0;
			for (const TPair<FIntPoint, TArray<int32>>& Pair : Job.Dirty)
			{
				if (Index++ >= Job.Edited) break;
				if (Job.Written.Contains(Pair.Key)) EditedTilesRebuilt++;
			}
		}
		RoadsRebuilt = Job.Batch.Num();
		RoadsGraphed = Job.Detected.Graph.Roads.Num() + (Graphed == &Job.Drawing ? Job.Drawing.Graph.Roads.Num() : 0);
		NumVertices = 0;
		NumTriangles = 0;
		NumMarkingInstances = 0;
		for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles)
		{
			NumVertices += Pair.Value.Vertices;
			NumTriangles += Pair.Value.Triangles;
			NumMarkingInstances += Pair.Value.DecalInstances;
		}
		// The preview needs the whole network: made now when this build had it, else once edits settle.
		if (Job.bEverything && Job.Batch.Num() > 0)
		{
			ClearPreview();
			BuildPreview(Graph, Lanes, Graphed->Result, Terrain);
			bPreviewStale = false;
		}
		else bPreviewStale = true;
		// Where the tiles written are, when they are few.
		FString At;
		if (Job.Written.Num() <= 8) for (const FIntPoint& Tile : Job.Written) At += FString::Printf(TEXT(" (%d, %d)"), Tile.X, Tile.Y);
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads in %d tiles, %d tiles (%d roads) rebuilt%s (%d of them edits' tiles, %d dirtied by this one), %d pending; %d roads graphed (%d affected); road keys and reach %.1f ms, detection %.1f ms, dirty tiles %.1f ms, drawing graph %.1f ms, lanes %.1f ms, refinement counts %.1f ms, tile meshes %.1f ms (GPU mesh edits %.1f ms), preview %.1f ms; waiting for the GPU %.1f ms or %d ticks"),
			*GetName(), NumRoads, NumTiles, TilesRebuilt, RoadsRebuilt, *At, EditedTilesRebuilt, Job.Fresh, TileState->NumPending(), RoadsGraphed, Job.Affected.Num(), Job.NetMs, Job.DetectMs, Job.DirtyMs, Job.GraphMs, LanesMs,
			CountMs, TileMs, EditMs, Lap(), Job.WaitMs, Job.Frames);
		Job.Stage = EStage::Done;
	}
	return true;
}

bool ACSRoadNetworkActor::WriteMeshes(const CSRoad::FGpuGeometryInput& Geometry, const CSRoad::FPooledGeometry& SharedGeometry, const CSRoad::FGpuRoadMeshInput& MeshInput,
	const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FAssets& Assets, FMeshTargets& Targets, FString& OutError)
{
	double LapStart = FPlatformTime::Seconds();
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};
	// One material slot per atlas; MeshTriangleMaterials indexes these.
	TArray<TObjectPtr<UMaterialInterface>> Materials;
	for (const FString& Atlas : MeshInput.MaterialSlots)
	{
		UMaterialInterface* const* Found = ByBaseColor.Find(Assets.AtlasBaseColor(Atlas));
		if (!Found) MissingAtlases.AddUnique(Atlas);
		Materials.Add(Found ? *Found : FallbackMaterial.Get());
	}

	UCSMeshRenderComponent* RoadComponent = Targets.Road;
	TObjectPtr<UCSMesh>& RoadObject = *Targets.RoadMesh;
	if (!RoadObject || RoadObject->GetOuter() != RoadComponent) RoadObject = NewObject<UCSMesh>(RoadComponent);
	RoadObject->Materials = Materials;
	RoadObject->NotifyMaterialsChanged();
	RoadComponent->MeshMaterial = Materials.Num() > 0 ? Materials[0].Get() : FallbackMaterial.Get();
	const int32 NumSlots = FMath::Max(1, Materials.Num());
	if (!RoadObject->EnsureCapacitySync(int32(MeshInput.NumVertices), int32(MeshInput.NumIndices)))
	{
		OutError = FString::Printf(TEXT("Could not allocate %u vertices / %u indices"), MeshInput.NumVertices, MeshInput.NumIndices);
		return false;
	}
	RoadObject->EnsureIndirectDrawCapacitySync(NumSlots);
	RoadComponent->SetGpuMesh(RoadObject);
	Targets.AllocationMs = Lap();

	// One graph: the road surface; the heights its connectors and crosswalks lie at, from its triangles
	// by their place in the index stream; then its material sections, which reorder them.
	const FTransform ActorToWorld = GetActorTransform();
	const FMatrix44f LocalToWorld(ActorToWorld.ToMatrixWithScale());
	const FMatrix44f WorldToLocal(ActorToWorld.ToInverseMatrixWithScale());
	const FBox WorldBounds = MeshInput.Bounds.TransformBy(ActorToWorld);
	const CSRoad::FGpuDecalInput* Decals = Targets.Decals && Targets.Decals->NumFitHeights > 0 ? Targets.Decals : nullptr;
	TSharedPtr<CSRoad::FDecalFitReadback> Fit;
	if (Decals) Fit = MakeShared<CSRoad::FDecalFitReadback>();
	Targets.Fit = Fit;
	// Without waiting: the edit holds what it reads, and publishes the sections once it has run. Behind
	// an edit of this mesh still on its way, it waits for both (EditMeshSync).
	const TSharedPtr<const CSRoad::FGpuDecalInput> SharedDecals = Decals ? Targets.SharedDecals : nullptr;
	if (Targets.SharedMesh && (!Decals || SharedDecals) && SharedGeometry.IsValid() && !RoadObject->IsEditInFlight())
	{
		const TSharedRef<FCSRoadNetworkActor_Recorded> Recorded = MakeShared<FCSRoadNetworkActor_Recorded>();
		const bool bAccepted = RoadObject->EditMeshAsync(
			[Shared = SharedGeometry, Mesh = Targets.SharedMesh, SharedDecals, Fit, LocalToWorld, WorldToLocal, WorldBounds, NumSlots, Recorded](FCSMeshEditContext& Context)
			{
				Recorded->bRecorded = CSRoadNetworkActor_RecordRoadMesh(Context, nullptr, Shared, *Mesh, SharedDecals.Get(), Fit, LocalToWorld, WorldToLocal, WorldBounds, NumSlots, Recorded->bSorted);
			},
			[Written = TWeakObjectPtr<UCSMesh>(RoadObject), Recorded, Name = GetName()](bool bBuilt)
			{
				if (!bBuilt || !Written.IsValid()) return;
				if (Recorded->bSorted) UCSMeshOps::PublishMaterialSections(Written.Get());
				if (!Recorded->bRecorded) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: a road tile mesh has no vertex streams"), *Name);
			});
		if (bAccepted)
		{
			Targets.RoadMs = Lap();
			return true;
		}
	}
	bool bSorted = false;
	bool bRecorded = false;
	RoadObject->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		bRecorded = CSRoadNetworkActor_RecordRoadMesh(Context, &Geometry, SharedGeometry, MeshInput, Decals, Fit, LocalToWorld, WorldToLocal, WorldBounds, NumSlots, bSorted);
	});
	if (!bRecorded)
	{
		OutError = TEXT("The road mesh has no vertex streams");
		return false;
	}
	if (bSorted) UCSMeshOps::PublishMaterialSections(RoadObject);
	Targets.RoadMs = Lap();
	return true;
}

int32 ACSRoadNetworkActor::WriteDecals(const CSRoad::FGpuDecalInput& Decals, const TArray<float>* FitHeights, TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name)
{
	TArray<CSRoad::FDecalInstances> Batches;
	CSRoad::BuildDecalInstances(Decals, GetActorTransform(), Batches, FitHeights);
	if (!DecalStrip) DecalStrip = CSRoad::MakeDecalStripMesh(this);
	int32 Used = 0;
	int32 Instances = 0;
	for (const CSRoad::FDecalInstances& Batch : Batches)
	{
		UMaterialInterface* Material = DecalSurfaceMaterial(Decals.MaterialSlots[Batch.MaterialSlot], Batch.bFitted);
		UStaticMesh* Unit = DecalStrip.Get();
		if (Batch.bFitted)
		{
			TObjectPtr<UStaticMesh>& Grid = DecalGrids.FindOrAdd(Batch.Columns);
			if (!Grid) Grid = CSRoad::MakeDecalGridMesh(this, Batch.Columns);
			Unit = Grid.Get();
		}
		if (!Material || Batch.Transforms.Num() == 0) continue;
		if (!Components.IsValidIndex(Used) || !Components[Used])
		{
			// Made at build time, never saved or copied, as the road tiles. The instances are in world
			// space: the component keeps an absolute identity transform.
			const FName ComponentName = MakeUniqueObjectName(Content, UInstancedStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("%s_%d"), *Name, Used));
			UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(Content, ComponentName, RF_Transient | RF_DuplicateTransient);
			Component->SetupAttachment(Content->GetRootComponent());
			Component->SetUsingAbsoluteLocation(true);
			Component->SetUsingAbsoluteRotation(true);
			Component->SetUsingAbsoluteScale(true);
			Component->SetRelativeTransform(FTransform::Identity);
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Component->SetCanEverAffectNavigation(false);
			Component->SetCastShadow(false);
			Component->SetReceivesDecals(false);
			Component->bVisibleInRayTracing = false;
			Component->bAffectDistanceFieldLighting = false;
			Component->SetStaticMesh(Unit);
			Component->RegisterComponent();
			if (Components.IsValidIndex(Used)) Components[Used] = Component;
			else Components.Add(Component);
		}
		UInstancedStaticMeshComponent* Component = Components[Used++];
		Component->ClearInstances();
		if (Component->GetStaticMesh() != Unit) Component->SetStaticMesh(Unit);
		Component->SetMaterial(0, Material);
		// RoadDecals' renderer priority: mesh decals sort by it.
		Component->SetTranslucentSortPriority(Batch.Priority);
		Component->SetNumCustomDataFloats(Batch.Floats);
		Component->AddInstances(Batch.Transforms, false, false, false);
		Component->SetCustomData(0, Batch.Transforms.Num() - 1, Batch.Data, false);
		Component->MarkRenderStateDirty();
		Instances += Batch.Transforms.Num();
	}
	while (Components.Num() > Used)
	{
		if (UInstancedStaticMeshComponent* Extra = Components.Pop()) Extra->DestroyComponent();
	}
	return Instances;
}

int32 ACSRoadNetworkActor::DrawDecals(const CSRoad::FGpuDecalInput& Decals, const TSharedPtr<CSRoad::FDecalFitReadback>& Fit, TArray<TSharedPtr<CSRoad::FDecalFitReadback>>& Fits,
	TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name)
{
	// The connectors and crosswalks wait for their heights, a frame or two: until then the decals
	// drawn before stay.
	if (Fit)
	{
		Fit->Items = Decals.Items;
		Fit->MaterialSlots = Decals.MaterialSlots;
		Fits.Add(Fit);
		return INDEX_NONE;
	}
	Fits.Reset();
	return WriteDecals(Decals, nullptr, Components, Name);
}

bool ACSRoadNetworkActor::TickDecalFits(TArray<TSharedPtr<CSRoad::FDecalFitReadback>>& Fits, TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components, const FString& Name, int32& OutInstances)
{
	int32 Landed = INDEX_NONE;
	for (int32 I = Fits.Num() - 1; I >= 0 && Landed == INDEX_NONE; I--) if (Fits[I]->Poll()) Landed = I;
	if (Landed == INDEX_NONE) return false;
	const TSharedPtr<CSRoad::FDecalFitReadback> Fit = Fits[Landed];
	Fits.RemoveAt(0, Landed + 1);
	CSRoad::FGpuDecalInput Decals;
	Decals.Items = Fit->Items;
	Decals.MaterialSlots = Fit->MaterialSlots;
	if (Fit->IsFailed()) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: the heights of %s's connectors and crosswalks did not come back from the GPU; they are left out"), *GetName(), *Name);
	OutInstances = WriteDecals(Decals, Fit->IsFailed() ? nullptr : &Fit->Heights(), Components, Name);
	return true;
}

void ACSRoadNetworkActor::TickAllDecalFits()
{
	bool bTiles = false;
	for (TPair<FIntPoint, FCSRoadTile>& Pair : Tiles)
	{
		if (Pair.Value.DecalFits.Num() == 0) continue;
		int32 Instances = 0;
		if (!TickDecalFits(Pair.Value.DecalFits, Pair.Value.Decals, FString::Printf(TEXT("MarkingTile_%d_%d"), Pair.Key.X, Pair.Key.Y), Instances)) continue;
		Pair.Value.DecalInstances = Instances;
		bTiles = true;
	}
	int32 Instances = 0;
	if (MarkingFits.Num() > 0 && Content && TickDecalFits(MarkingFits, Content->MarkingDecals, TEXT("Marking"), Instances)) NumMarkingInstances = Instances;
	if (!bTiles) return;
	NumMarkingInstances = 0;
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) NumMarkingInstances += Pair.Value.DecalInstances;
}

bool ACSRoadNetworkActor::AnyTileEditInFlight() const
{
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) if (Pair.Value.RoadMesh && Pair.Value.RoadMesh->IsEditInFlight()) return true;
	return false;
}

void ACSRoadNetworkActor::LandTileEdits()
{
	// Each round: the render thread done with the edits, then their ends on the game thread (a flush
	// runs game-thread tasks only while it waits).
	for (int32 Round = 0; Round < 100 && AnyTileEditInFlight(); Round++)
	{
		FlushRenderingCommands();
		if (!FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread)) FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
}

void ACSRoadNetworkActor::FinishWrites()
{
	LandTileEdits();
	auto Pending = [this]()
	{
		if (MarkingFits.Num() > 0) return true;
		for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) if (Pair.Value.DecalFits.Num() > 0) return true;
		return false;
	};
	// Each round: the GPU done with every copy, a look at each, then what landed drawn.
	for (int32 Round = 0; Round < 100 && Pending(); Round++)
	{
		ENQUEUE_RENDER_COMMAND(CSRoadFinishWrites)([](FRHICommandListImmediate& RHICmdList) { RHICmdList.BlockUntilGPUIdle(); });
		FlushRenderingCommands();
		TickAllDecalFits();
		FlushRenderingCommands();
		TickAllDecalFits();
	}
}

void ACSRoadNetworkActor::ResolveDecalMaterials(const CSRoad::FGpuDecalInput& Decals, const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FAssets& Assets)
{
	// What each surface draws: strips (1), fitted grids (2).
	TArray<uint8> Kinds;
	Kinds.SetNumZeroed(Decals.MaterialSlots.Num());
	for (const CSRoad::FGpuDecalItem& Item : Decals.Items) if (Kinds.IsValidIndex(int32(Item.MaterialSlot))) Kinds[Item.MaterialSlot] |= Item.Kind == uint32(CSRoad::EDecalKind::Strip) ? 1 : 2;
	for (int32 Slot = 0; Slot < Decals.MaterialSlots.Num(); Slot++)
	{
		const FString& Surface = Decals.MaterialSlots[Slot];
		for (const bool bFitted : { false, true })
		{
			if ((Kinds[Slot] & (bFitted ? 2 : 1)) == 0) continue;
			const TSoftObjectPtr<UMaterialInterface>& Master = bFitted ? FittedDecalMaterial : DecalMaterial;
			UMaterialInterface* Parent = Master.LoadSynchronous();
			const FString Key = bFitted ? Surface + TEXT("|fitted") : Surface;
			if (const TObjectPtr<UMaterialInstanceDynamic>* Known = DecalSurfaces.Find(Key); Known && *Known && (*Known)->Parent == Parent) continue;
			DecalSurfaces.Remove(Key);
			UMaterialInterface* const* Look = ByBaseColor.Find(Assets.SurfaceBaseColor(Surface));
			if (!Parent) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: no decal material %s (Scripts/CS2Road/MakeRoadDecalMaterial.py makes it)"), *GetName(), *Master.ToString());
			if (!Look || !Parent)
			{
				MissingMarkingSurfaces.AddUnique(Surface);
				continue;
			}
			// The M_CS2 decal material's look (Scripts/ImportCS2Roads.py): its albedo texture and tint.
			UMaterialInstanceDynamic* Instance = UMaterialInstanceDynamic::Create(Parent, this);
			UTexture* Albedo = nullptr;
			if ((*Look)->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("albedo")), Albedo) && Albedo) Instance->SetTextureParameterValue(TEXT("albedo"), Albedo);
			FLinearColor Tint = FLinearColor::White;
			if ((*Look)->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Tint")), Tint)) Instance->SetVectorParameterValue(TEXT("Tint"), Tint);
			DecalSurfaces.Add(Key, Instance);
		}
	}
}

UMaterialInterface* ACSRoadNetworkActor::DecalSurfaceMaterial(const FString& Surface, bool bFitted) const
{
	const TObjectPtr<UMaterialInstanceDynamic>* Known = DecalSurfaces.Find(bFitted ? Surface + TEXT("|fitted") : Surface);
	return Known ? Known->Get() : nullptr;
}

void ACSRoadNetworkActor::DestroyComponents(TArray<TObjectPtr<UInstancedStaticMeshComponent>>& Components)
{
	for (UInstancedStaticMeshComponent* Component : Components) if (Component) Component->DestroyComponent();
	Components.Reset();
}

void ACSRoadNetworkActor::TickBakes()
{
#if WITH_EDITOR
	// Nothing bakes while a mouse button is held: a drag rewrites its tiles again and again, and a swap would hitch it.
	const bool bMouseHeld = FSlateApplication::IsInitialized() && FSlateApplication::Get().GetPressedMouseButtons().Num() > 0;
	if (bLocalUpdate && bBakeNanite && Content && !bMouseHeld)
	{
		const double Start = FPlatformTime::Seconds();
		// Game-thread time a tick for starting bakes, handing them to the compiler and swapping meshes, the tiles nearest
		// the camera first.
		const double Deadline = Start + 0.002;
		FVector2D Focus;
		const bool bFocus = TileFocus(Focus);
		TArray<FIntPoint> Order;
		Tiles.GetKeys(Order);
		Order.Sort([&Focus, bFocus](const FIntPoint& A, const FIntPoint& B) { return CSRoad::TileBefore(A, B, bFocus ? &Focus : nullptr); });
		for (const FIntPoint& Key : Order)
		{
			FCSRoadTile& Tile = Tiles.FindChecked(Key);
			if (!Tile.RoadMesh || !Tile.Road || Tile.bBakeFailed) continue;
			if (!Tile.Bake)
			{
				// Once the mesh has stood a moment (an edit dragged across a tile rewrites it every tick) and
				// its last edit has run.
				if (Start - Tile.WrittenSeconds < 0.3 || Tile.RoadMesh->IsEditInFlight() || FPlatformTime::Seconds() > Deadline) continue;
				TArray<UMaterialInterface*> Materials;
				for (const TObjectPtr<UMaterialInterface>& Material : Tile.RoadMesh->Materials) Materials.Add(Material.Get());
				const double StartStart = FPlatformTime::Seconds();
				Tile.Baking = CSRoad::NewNaniteMesh(this, Materials);
				Tile.Bake = MakeShared<CSRoad::FRoadBake>();
				if (!Tile.Bake->Start(*Tile.RoadMesh, uint32(Tile.Vertices), uint32(Tile.Triangles) * 3u, Tile.Baking))
				{
					UE_LOG(LogCSRoadNet, Warning, TEXT("%s: tile (%d, %d) has no road mesh streams to bake"), *GetName(), Key.X, Key.Y);
					Tile.Bake.Reset();
					Tile.Baking = nullptr;
					Tile.bBakeFailed = true;
				}
				if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - StartStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) bake start took %.1f ms"), *GetName(), Key.X, Key.Y, (FPlatformTime::Seconds() - StartStart) * 1000.0);
				continue;
			}
			const double StepStart = FPlatformTime::Seconds();
			const CSRoad::FRoadBake::EStage Before = Tile.Bake->Stage();
			const CSRoad::FRoadBake::EStage Stage = Tile.Bake->Tick(StepStart < Deadline);
			if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - StepStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) bake step %d -> %d took %.1f ms"), *GetName(), Key.X, Key.Y, int32(Before), int32(Stage), (FPlatformTime::Seconds() - StepStart) * 1000.0);
			if (Stage == CSRoad::FRoadBake::EStage::Failed)
			{
				UE_LOG(LogCSRoadNet, Warning, TEXT("%s: tile (%d, %d) could not be baked into a Nanite mesh; it keeps its GPU mesh"), *GetName(), Key.X, Key.Y);
				Tile.Bake.Reset();
				Tile.Baking = nullptr;
				Tile.bBakeFailed = true;
				continue;
			}
			if (Stage != CSRoad::FRoadBake::EStage::Done || FPlatformTime::Seconds() > Deadline) continue;
			// The bake in place of the GPU mesh, whose VRAM goes now rather than at the next GC.
			const double SwapStart = FPlatformTime::Seconds();
			if (!Tile.Nanite)
			{
				// As the other tile components: made at build time, never saved or copied.
				const FName Name = MakeUniqueObjectName(Content, UStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("NaniteTile_%d_%d"), Key.X, Key.Y));
				Tile.Nanite = NewObject<UStaticMeshComponent>(Content, Name, RF_Transient | RF_DuplicateTransient);
				Tile.Nanite->SetupAttachment(Content->GetRootComponent());
				Tile.Nanite->SetUsingAbsoluteLocation(true);
				Tile.Nanite->SetUsingAbsoluteRotation(true);
				Tile.Nanite->SetUsingAbsoluteScale(true);
				Tile.Nanite->SetMobility(EComponentMobility::Movable);
				Tile.Nanite->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Tile.Nanite->SetCanEverAffectNavigation(false);
				Tile.Nanite->RegisterComponent();
			}
			CSRoad::RedrawShadowsOnStreaming();
			Tile.Nanite->SetWorldTransform(FTransform(Tile.Bake->Origin()));
			Tile.Nanite->SetStaticMesh(Tile.Baking);
			Tile.Bake.Reset();
			Tile.Baking = nullptr;
			// The GPU mesh goes with its component (removing the primitive drops whatever the renderer
			// cached of it); the tile gets a new one when it is next written.
			Tile.Road->DestroyComponent();
			Tile.Road = nullptr;
			Tile.RoadMesh = nullptr;
			if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - SwapStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) swap took %.1f ms"), *GetName(), Key.X, Key.Y, (FPlatformTime::Seconds() - SwapStart) * 1000.0);
		}
	}
#endif
	UpdateBakeStats();
}

void ACSRoadNetworkActor::UpdateBakeStats()
{
	int32 Baked = 0;
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) Baked += !Pair.Value.RoadMesh && Pair.Value.Nanite && Pair.Value.Nanite->GetStaticMesh() ? 1 : 0;
	NumNaniteTiles = Baked;
	GpuMeshMiB = float(double(GpuMeshBytes()) / double(1 << 20));
}

int64 ACSRoadNetworkActor::TileGpuBytes(const FCSRoadTile& Tile)
{
	const FCSMeshResident* Resident = Tile.RoadMesh ? Tile.RoadMesh->GetResidentPtr() : nullptr;
	if (!Resident) return 0;
	// The streams as the pools hand them out (64 KiB granules, rhi.DumpResourceMemory), and the ray
	// tracing BLAS of the mesh: about 31 bytes a triangle on Palmsland.
	int64 Bytes = int64(Tile.Triangles) * 31;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams) if (Stream.Pooled.IsValid()) Bytes += Align(int64(Stream.Pooled->GetAlignedSize()), int64(64 << 10));
	return Bytes;
}

int64 ACSRoadNetworkActor::GpuMeshBytes() const
{
	int64 Bytes = 0;
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) Bytes += TileGpuBytes(Pair.Value);
	return Bytes;
}

int64 ACSRoadNetworkActor::GpuMeshBudgetBytes() const
{
#if WITH_EDITOR
	if (bLocalUpdate && bBakeNanite && GpuMeshBudget > 0) return int64(GpuMeshBudget) << 20;
#endif
	return MAX_int64;
}

bool ACSRoadNetworkActor::VideoMemoryShort(int64 Bytes)
{
	if (!bLocalUpdate || MinFreeVideoMemory <= 0) return false;
	// The driver's budget for this process less what it uses (a DXGI query): measured or not at all.
	const double Now = FPlatformTime::Seconds();
	if (VideoMemorySeconds < 0.0 || Now - VideoMemorySeconds > 0.25)
	{
		VideoMemorySeconds = Now;
		VideoMemoryTaken = 0;
		const CSGpuMemoryBudget::FMemorySnapshot Snapshot = CSGpuMemoryBudget::QueryMemorySnapshot();
		FreeVideoMemoryMiB = Snapshot.bAvailableIsMeasured ? float(double(Snapshot.AvailableVideoMemory) / double(1 << 20)) : -1.0f;
	}
	if (FreeVideoMemoryMiB < 0.0f) return false;
	return double(FreeVideoMemoryMiB) - double(VideoMemoryTaken + FMath::Max<int64>(Bytes, 0)) / double(1 << 20) < double(MinFreeVideoMemory);
}

int64 ACSRoadNetworkActor::EstimateTileBytes(const FCSRoadTile* Known, int32 Roads, double TrianglesPerRoad)
{
	// As its last build, else the tiles' average per road; road meshes have about 1.1 vertices a triangle.
	if (Known && Known->Triangles > 0) return CSRoadNetworkActor_GpuMeshBytes(uint32(Known->Vertices), uint32(Known->Triangles) * 3u);
	const double Triangles = double(Roads) * TrianglesPerRoad;
	return CSRoadNetworkActor_GpuMeshBytes(uint32(Triangles * 1.1), uint32(Triangles) * 3u);
}

FCSRoadTile& ACSRoadNetworkActor::EnsureTile(const FIntPoint& Key)
{
	FCSRoadTile& Tile = Tiles.FindOrAdd(Key);
	// Made at build time, never saved or copied: a loaded or duplicated network builds its own.
	if (!Tile.Road)
	{
		const FName Name = MakeUniqueObjectName(Content, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("RoadTile_%d_%d"), Key.X, Key.Y));
		Tile.Road = NewObject<UCSMeshRenderComponent>(Content, Name, RF_Transient | RF_DuplicateTransient);
		Tile.Road->SetupAttachment(Content->GetRootComponent());
		Tile.Road->bReceivesDecals = true;
		Tile.Road->RegisterComponent();
	}
	return Tile;
}

void ACSRoadNetworkActor::RemoveTile(const FIntPoint& Key)
{
	FCSRoadTile Tile;
	if (!Tiles.RemoveAndCopyValue(Key, Tile)) return;
	// A component frees the meshes it owns when destroyed.
	if (Tile.Road) Tile.Road->DestroyComponent();
	if (Tile.Nanite) Tile.Nanite->DestroyComponent();
	DestroyComponents(Tile.Decals);
}

void ACSRoadNetworkActor::ClearTiles()
{
	TArray<FIntPoint> Keys;
	Tiles.GetKeys(Keys);
	for (const FIntPoint& Key : Keys) RemoveTile(Key);
	if (TileState) TileState->Reset();
	NumTiles = 0;
	TilesRebuilt = 0;
	EditedTilesRebuilt = 0;
	RoadsRebuilt = 0;
}

uint64 ACSRoadNetworkActor::TileSignature(bool bLeftHand) const
{
	FXxHash64Builder Hash;
	auto Add = [&Hash](const auto& Value) { Hash.Update(&Value, sizeof(Value)); };
	Add(TileSize);
	Add(bLeftHand);
	Add(bBuildMarkings);
	Add(bBakeNanite);
	const FMatrix Placement = GetActorTransform().ToMatrixWithScale();
	Add(Placement);
	const UMaterialInterface* Fallback = FallbackMaterial.Get();
	Add(Fallback);
	const FString Folders = MaterialFolder.Path + TEXT("|") + ResolveAssetDirectory() + TEXT("|") + DecalMaterial.ToString() + TEXT("|") + FittedDecalMaterial.ToString();
	Hash.Update(*Folders, Folders.Len() * sizeof(TCHAR));
	return Hash.Finalize().Hash;
}

bool ACSRoadNetworkActor::TileFocus(FVector2D& OutFocus) const
{
	const UWorld* World = GetWorld();
	if (!World || TileSize <= 0.0f) return false;
	TOptional<FVector> Camera;
#if WITH_EDITOR
	if (GEditor && World->WorldType == EWorldType::Editor)
	{
		// The viewport last worked in when it looks at this world in perspective, else any that does.
		auto Looks = [World](const FLevelEditorViewportClient* Client) { return Client && Client->IsPerspective() && Client->GetWorld() == World; };
		const FLevelEditorViewportClient* Client = Looks(GCurrentLevelEditingViewportClient) ? GCurrentLevelEditingViewportClient : nullptr;
		for (const FLevelEditorViewportClient* Other : GEditor->GetLevelViewportClients()) if (!Client && Looks(Other)) Client = Other;
		if (Client) Camera = Client->GetViewLocation();
	}
#endif
	const APlayerController* Player = Camera ? nullptr : World->GetFirstPlayerController();
	if (Player && Player->PlayerCameraManager) Camera = Player->PlayerCameraManager->GetCameraLocation();
	if (!Camera) return false;
	// The tiles' frame: Unity x and z (UE x and -y) of the network's space, TileSize to a tile.
	const FVector Local = GetActorTransform().InverseTransformPosition(*Camera);
	const double TileCm = double(TileSize) * 100.0;
	OutFocus = FVector2D(Local.X / TileCm, -Local.Y / TileCm);
	return true;
}

void ACSRoadNetworkActor::BuildBatchBuildings(const TArray<CSRoad::FSplineRoad>& Splines, const TArray<CSRoad::FRoadSpec>& Roads, CSRoad::FAssets& Assets, const CSRoad::FSavedTerrain* Terrain)
{
	const double StartTime = FPlatformTime::Seconds();
	const FTransform NetworkToWorld = GetActorTransform();
	const double TileCm = double(TileSize) * 100.0;
	CSCity::FLotSettings Settings;
	Settings.Frontage = double(Lots.Frontage) * 100.0;
	Settings.Gap = double(Lots.Gap) * 100.0;
	Settings.Setback = double(Lots.Setback) * 100.0;
	Settings.MinDepth = double(Lots.MinDepth) * 100.0;
	Settings.MaxDepth = double(FMath::Max(Lots.MinDepth, Lots.MaxDepth)) * 100.0;
	Settings.MinHeight = double(Lots.MinHeight) * 100.0;
	Settings.MaxHeight = double(FMath::Max(Lots.MinHeight, Lots.MaxHeight)) * 100.0;
	Settings.bFill = Lots.bFillBlocks;
	Settings.FillDepth = double(Lots.FillDepth) * 100.0;
	Settings.Seed = Lots.Seed;
	UMaterialInterface* Surface = BuildingMaterial ? BuildingMaterial.Get() : ACSCityBuildingActor::DefaultMaterial();
	if (!LotState) LotState = MakeUnique<CSCity::FLotState>();
	CSCity::FLotState& State = *LotState;

	// Whatever else the plots and their meshes are made from: when it changes, everything is laid out
	// again.
	FXxHash64Builder SignatureHash;
	auto Sign = [&SignatureHash](const auto& Value) { SignatureHash.Update(&Value, sizeof(Value)); };
	for (const double Value : { Settings.Frontage, Settings.Gap, Settings.Setback, Settings.MinDepth, Settings.MaxDepth, Settings.MinHeight, Settings.MaxHeight, Settings.FillDepth, TileCm }) Sign(Value);
	Sign(Settings.bFill);
	Sign(Settings.Seed);
	Sign(Surface);
	const FMatrix Placement = NetworkToWorld.ToMatrixWithScale();
	Sign(Placement);
	const uint64 TerrainSerial = Terrain ? Terrain->Serial : 0;
	Sign(TerrainSerial);
	const uint64 Signature = SignatureHash.Finalize().Hash;
	const bool bFull = !State.bValid || State.Signature != Signature;

	auto HalfWidth = [&Assets](const FString& Prefab) { return double(Assets.Geometry(Prefab).m_DefaultWidth) * 50.0; };
	// A custom road's own setback and frontage (Data/CustomRoads.json "plots"), else the network's.
	auto SetbackOf = [&Assets, &Settings](const FString& Prefab)
	{
		const CSRoad::FAssets::FPlots* Custom = Assets.Plots(Prefab);
		return Custom && Custom->Setback >= 0.0 ? Custom->Setback * 100.0 : Settings.Setback;
	};
	auto FrontageOf = [&Assets](const FString& Prefab)
	{
		const CSRoad::FAssets::FPlots* Custom = Assets.Plots(Prefab);
		return Custom && Custom->Frontage > 0.0 ? Custom->Frontage * 100.0 : -1.0;
	};
	auto Combine = [](uint64 A, uint64 B)
	{
		const uint64 Values[] = { A, B };
		return FXxHash64::HashBuffer(Values, sizeof(Values)).Hash;
	};
	auto Point = [](const CSRoad::float3& P) { return FVector3d(P.x, P.y, P.z); };
	// The roads' tiles: the network's ground plane (Unity x and z) in TileSize squares.
	auto TileOf = [&NetworkToWorld, TileCm](const FVector2D& World)
	{
		const FVector Local = NetworkToWorld.InverseTransformPosition(FVector(World, 0.0));
		return FIntPoint(FMath::FloorToInt32(Local.X / TileCm), FMath::FloorToInt32(-Local.Y / TileCm));
	};
	// The tiles (inclusive) holding every point within Reach of a world box.
	auto TilesOf = [&TileOf](const FBox2D& Box, double Reach)
	{
		const FBox2D Grown = Box.ExpandBy(Reach);
		FIntRect Tiles(TileOf(Grown.Min), TileOf(Grown.Min));
		for (const FVector2D& Corner : { FVector2D(Grown.Max.X, Grown.Min.Y), Grown.Max, FVector2D(Grown.Min.X, Grown.Max.Y) })
		{
			const FIntPoint Tile = TileOf(Corner);
			Tiles.Min = Tiles.Min.ComponentMin(Tile);
			Tiles.Max = Tiles.Max.ComponentMax(Tile);
		}
		return Tiles;
	};

	// Every input with its fingerprint and the tiles of the plots it can change (CSCity::AnchorReach and
	// the like). Anchors and roads keep their curve in the network's space, sampled only if laid out.
	enum class EInput : uint8 { Anchor, Road, Reserved };
	struct FInput
	{
		EInput Kind = EInput::Road;
		uint64 Key = 0;
		FIntRect Tiles;
		FVector3d Curve[4];
		double HalfWidth = 0.0;
		double Setback = 0.0;
		double Frontage = -1.0;
		uint64 LineId = 0;
		// The spline it is part of (CSCity::FRoadLine::Group).
		uint64 Group = 0;
		bool bZoned = true;
		int32 Plot = INDEX_NONE;
	};
	TArray<FInput> Inputs;
	TArray<CSCity::FPolygon> Plots;
	auto AddCurve = [&](EInput Kind, uint64 LineId, uint64 Group, const FString& Prefab, bool bZoned, const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector3d& D)
	{
		FInput& Input = Inputs.AddDefaulted_GetRef();
		Input.Kind = Kind;
		Input.LineId = LineId;
		Input.Group = Group;
		Input.HalfWidth = HalfWidth(Prefab);
		Input.Setback = SetbackOf(Prefab);
		Input.Frontage = FrontageOf(Prefab);
		Input.bZoned = bZoned;
		Input.Curve[0] = A;
		Input.Curve[1] = B;
		Input.Curve[2] = C;
		Input.Curve[3] = D;
		FXxHash64Builder Hash;
		Hash.Update(&Kind, sizeof(Kind));
		Hash.Update(&LineId, sizeof(LineId));
		Hash.Update(&Group, sizeof(Group));
		Hash.Update(&Input.HalfWidth, sizeof(Input.HalfWidth));
		Hash.Update(&Input.Setback, sizeof(Input.Setback));
		Hash.Update(&Input.Frontage, sizeof(Input.Frontage));
		Hash.Update(&bZoned, sizeof(bZoned));
		Hash.Update(Input.Curve, sizeof(Input.Curve));
		Input.Key = Hash.Finalize().Hash;
		// A cubic Bezier lies within its control points' box.
		FBox2D Box(ForceInit);
		for (const FVector3d& Control : Input.Curve) Box += FVector2D(NetworkToWorld.TransformPosition(CSRoadNetworkActor_ToUnreal(Control)));
		Input.Tiles = TilesOf(Box, Kind == EInput::Anchor ? CSCity::AnchorReach(Settings, Input.HalfWidth, Input.Setback) : CSCity::RoadReach(Settings, Input.HalfWidth, Input.Setback));
	};
	// Only roads CS2 zones along (with zone blocks: not highways, not bridges), and custom roads asking for plots, have
	// plots; never a spline without plots (a garage's driveway).
	TMap<FString, bool> Zoning;
	auto Zoned = [&Assets, &Zoning](const FString& Prefab)
	{
		if (const bool* Known = Zoning.Find(Prefab)) return *Known;
		return Zoning.Add(Prefab, Assets.HasRoad(Prefab) && ((Assets.Road(Prefab).m_Flags & CSRoad::RoadFlags_EnableZoning) != 0 || Assets.Plots(Prefab)));
	};
	TSet<uint64> NoPlots;
	for (const CSRoad::FSplineRoad& Spline : Splines) if (!Spline.bPlots) NoPlots.Add(Spline.Id);
	// The roads as built, which the plots keep clear of; the plots of other roads stop halfway to those with plots.
	for (const CSRoad::FRoadSpec& Road : Roads)
	{
		const bool bZoned = !NoPlots.Contains(Road.Spline) && Zoned(Road.Prefab);
		AddCurve(EInput::Road, 0, Road.Spline, Road.Prefab, bZoned, Point(Road.Curve.a), Point(Road.Curve.b), Point(Road.Curve.c), Point(Road.Curve.d));
	}
	// The plots face the splines' segments as drawn, not the edges cut from them: a road joining the
	// spline, or an edit of another segment, leaves the slots where they were.
	for (const CSRoad::FSplineRoad& Spline : Splines)
	{
		if (!Spline.bPlots || !Zoned(Spline.Prefab)) continue;
		const int32 Count = Spline.Points.Num();
		for (int32 I = 0; I < (Spline.bClosed ? Count : Count - 1); I++)
		{
			const int32 J = (I + 1) % Count;
			const FVector3d& A = Spline.Points[I];
			const FVector3d& D = Spline.Points[J];
			const bool bLinear = Spline.Linear.IsValidIndex(I) && Spline.Linear[I];
			// A USplineComponent segment's Hermite tangents as Bezier handles.
			AddCurve(EInput::Anchor, Combine(Spline.Id, uint64(I)), Spline.Id, Spline.Prefab, true, A, bLinear ? FMath::Lerp(A, D, 1.0 / 3.0) : A + Spline.Leave[I] / 3.0, bLinear ? FMath::Lerp(A, D, 2.0 / 3.0) : D - Spline.Arrive[J] / 3.0, D);
		}
	}
	if (UWorld* World = GetWorld())
	{
		for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
		{
			if (NetworkOf(Building) != this) continue;
			CSCity::FPolygon Plot = Building->Reservation();
			if (Plot.Num() < 3) continue;
			FInput& Input = Inputs.AddDefaulted_GetRef();
			Input.Kind = EInput::Reserved;
			Input.Plot = Plots.Num();
			FXxHash64Builder Hash;
			Hash.Update(&Input.Kind, sizeof(Input.Kind));
			Hash.Update(Plot.GetData(), Plot.Num() * sizeof(FVector2D));
			Input.Key = Hash.Finalize().Hash;
			Input.Tiles = TilesOf(FBox2D(Plot), CSCity::ReservedReach(Settings));
			Plots.Add(MoveTemp(Plot));
		}
	}

	// Dirty: the tiles an input that came or went can change (fresh), and those left by the last
	// batches. A batch of them is laid out at most: the fresh ones first, then those earlier edits
	// dirtied, then the rest, each nearest the camera first; a layout an edit made takes only edits'
	// tiles. The rest stay pending.
	TMap<uint64, FIntRect> Reach;
	Reach.Reserve(Inputs.Num());
	for (const FInput& Input : Inputs) Reach.Add(Input.Key, Input.Tiles);
	TSet<FIntPoint> Dirty;
	TArray<FIntPoint> Deferred;
	TSet<FIntPoint> Edited;
	FVector2D Focus;
	const FVector2D* Near = TileFocus(Focus) ? &Focus : nullptr;
	if (!bFull)
	{
		TSet<FIntPoint> Fresh;
		auto Mark = [&Fresh](const FIntRect& Tiles) { for (int32 Y = Tiles.Min.Y; Y <= Tiles.Max.Y; Y++) for (int32 X = Tiles.Min.X; X <= Tiles.Max.X; X++) Fresh.Add(FIntPoint(X, Y)); };
		for (const TPair<uint64, FIntRect>& Pair : Reach) if (!State.Reach.Contains(Pair.Key)) Mark(Pair.Value);
		for (const TPair<uint64, FIntRect>& Pair : State.Reach) if (!Reach.Contains(Pair.Key)) Mark(Pair.Value);
		Edited = Fresh;
		for (const FIntPoint& Tile : State.Edited) if (State.Pending.Contains(Tile)) Edited.Add(Tile);
		auto Rank = [&Fresh, &Edited](const FIntPoint& Tile) { return Fresh.Contains(Tile) ? 0 : Edited.Contains(Tile) ? 1 : 2; };
		TArray<FIntPoint> Order = Fresh.Array();
		for (const FIntPoint& Tile : State.Pending) if (!Fresh.Contains(Tile)) Order.Add(Tile);
		Order.Sort([&Rank, Near](const FIntPoint& A, const FIntPoint& B)
		{
			const int32 RankA = Rank(A);
			const int32 RankB = Rank(B);
			if (RankA != RankB) return RankA < RankB;
			return CSRoad::TileBefore(A, B, Near);
		});
		const int32 Candidates = Fresh.Num() > 0 ? Edited.Num() : Order.Num();
		for (int32 I = 0; I < Order.Num(); I++)
		{
			if (I >= Candidates || (MaxTilesPerBatch > 0 && I >= MaxTilesPerBatch)) Deferred.Add(Order[I]);
			else Dirty.Add(Order[I]);
		}
	}
	auto Touches = [&Dirty](const FIntRect& Tiles)
	{
		for (int32 Y = Tiles.Min.Y; Y <= Tiles.Max.Y; Y++) for (int32 X = Tiles.Min.X; X <= Tiles.Max.X; X++) if (Dirty.Contains(FIntPoint(X, Y))) return true;
		return false;
	};

	// Laid out from the inputs within reach of a dirty tile (every input after a change of settings):
	// the plots centred in a dirty tile come out as a full layout's would.
	auto Sample = [&NetworkToWorld](const FVector3d (&Curve)[4], TArray<FVector>& Out)
	{
		const FVector3d& A = Curve[0];
		const FVector3d& B = Curve[1];
		const FVector3d& C = Curve[2];
		const FVector3d& D = Curve[3];
		// A cubic Bezier of the network's space as world points about 2 m apart.
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32((FVector3d::Distance(A, B) + FVector3d::Distance(B, C) + FVector3d::Distance(C, D)) / 2.0), 1, 256);
		for (int32 S = 0; S <= Steps; S++)
		{
			const double T = double(S) / Steps;
			const double U = 1.0 - T;
			Out.Add(NetworkToWorld.TransformPosition(CSRoadNetworkActor_ToUnreal(A * (U * U * U) + B * (3.0 * U * U * T) + C * (3.0 * U * T * T) + D * (T * T * T))));
		}
	};
	TArray<CSCity::FRoadLine> Anchors;
	TArray<CSCity::FRoadLine> Lines;
	TArray<CSCity::FPolygon> Reserved;
	for (const FInput& Input : Inputs)
	{
		if (!bFull && !Touches(Input.Tiles)) continue;
		if (Input.Kind == EInput::Reserved)
		{
			Reserved.Add(Plots[Input.Plot]);
			continue;
		}
		CSCity::FRoadLine& Line = (Input.Kind == EInput::Anchor ? Anchors : Lines).AddDefaulted_GetRef();
		Line.Id = Input.LineId;
		Line.Group = Input.Group;
		Line.HalfWidth = Input.HalfWidth;
		Line.Setback = Input.Setback;
		Line.Frontage = Input.Frontage;
		Line.bZoned = Input.bZoned;
		Sample(Input.Curve, Line.Points);
	}
	// On the snapshot's terrain a plot stands on the ground, half a metre into it below its lowest corner,
	// and none fronts a road more than 3 m above or below the ground there (a bridge, a ramp, a cutting).
	auto GroundAt = [&NetworkToWorld, Terrain](const FVector2D& World)
	{
		const FVector3d Unity = CSRoadNetworkActor_ToUnity(NetworkToWorld.InverseTransformPosition(FVector(World, 0.0)));
		const double Height = Terrain->Sample(CSRoad::float3(static_cast<float>(Unity.X), 0.0f, static_cast<float>(Unity.Z)));
		return NetworkToWorld.TransformPosition(CSRoadNetworkActor_ToUnreal(FVector3d(Unity.X, Height, Unity.Z))).Z;
	};
	auto Place = [Terrain, &GroundAt](CSCity::FLot& Lot)
	{
		if (!Terrain) return true;
		double Lowest = TNumericLimits<double>::Max();
		double Highest = TNumericLimits<double>::Lowest();
		for (const FVector2D& Corner : Lot.Corners)
		{
			const double Ground = GroundAt(Corner);
			Lowest = FMath::Min(Lowest, Ground);
			Highest = FMath::Max(Highest, Ground);
		}
		if (Lot.Base - Highest > 300.0 || Lowest - Lot.Base > 300.0) return false;
		Lot.Base = Lowest - 50.0;
		return true;
	};
	const TArray<CSCity::FLot> Laid = CSCity::LayoutLots(Anchors, Lines, Reserved, Settings, Place);
	TMap<FIntPoint, TArray<const CSCity::FLot*>> ByTile;
	for (const CSCity::FLot& Lot : Laid)
	{
		const FIntPoint Tile = TileOf(Lot.Centre);
		if (bFull || Dirty.Contains(Tile)) ByTile.FindOrAdd(Tile).Add(&Lot);
	}

	// The tiles to write or remove, nearest the camera first: the dirty ones, or every one there is or
	// was. A batch of them is written at most; the others stay pending and are laid out again later.
	TArray<FIntPoint> Visit;
	if (bFull)
	{
		ByTile.GetKeys(Visit);
		for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) if (!ByTile.Contains(Pair.Key)) Visit.Add(Pair.Key);
	}
	else Visit = Dirty.Array();
	Visit.Sort([Near](const FIntPoint& A, const FIntPoint& B) { return CSRoad::TileBefore(A, B, Near); });
	int32 Written = 0;
	int32 Removed = 0;
	State.Pending.Reset();
	State.Pending.Append(Deferred);
	for (const FIntPoint& Key : Visit)
	{
		TArray<const CSCity::FLot*>* TileLots = ByTile.Find(Key);
		if (!TileLots)
		{
			FCSCityBuildingTile Gone;
			if (!BuildingTiles.RemoveAndCopyValue(Key, Gone)) continue;
			if (Gone.Component) Gone.Component->DestroyComponent();
			Removed++;
			continue;
		}
		TileLots->Sort([](const CSCity::FLot& A, const CSCity::FLot& B) { return A.Id < B.Id; });
		FXxHash64Builder Hash;
		Hash.Update(&Surface, sizeof(Surface));
		for (const CSCity::FLot* Lot : *TileLots)
		{
			Hash.Update(&Lot->Id, sizeof(Lot->Id));
			Hash.Update(&Lot->Base, sizeof(Lot->Base));
			Hash.Update(&Lot->Height, sizeof(Lot->Height));
			Hash.Update(&Lot->Color, sizeof(Lot->Color));
			Hash.Update(Lot->Corners.GetData(), Lot->Corners.Num() * sizeof(FVector2D));
		}
		const uint64 Value = Hash.Finalize().Hash;
		const FCSCityBuildingTile* Known = BuildingTiles.Find(Key);
		if (Known && Known->Component && Known->Mesh && Known->Hash == Value) continue;
		if (MaxTilesPerBatch > 0 && Written >= MaxTilesPerBatch)
		{
			State.Pending.Add(Key);
			continue;
		}
		FCSCityBuildingTile& Tile = BuildingTiles.FindOrAdd(Key);
		if (!Tile.Component)
		{
			// As the road tiles: made at build time, never saved or copied.
			const FName Name = MakeUniqueObjectName(Content, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("BuildingTile_%d_%d"), Key.X, Key.Y));
			Tile.Component = NewObject<UCSMeshRenderComponent>(Content, Name, RF_Transient | RF_DuplicateTransient);
			Tile.Component->SetupAttachment(Content->GetRootComponent());
			Tile.Component->RegisterComponent();
		}
		if (!Tile.Mesh || Tile.Mesh->GetOuter() != Tile.Component) Tile.Mesh = NewObject<UCSMesh>(Tile.Component);
		FCSGpuMeshCPUData Data;
		for (const CSCity::FLot* Lot : *TileLots) CSCity::AppendPrism(Data, Lot->Corners, Lot->Base, Lot->Height, Lot->Color);
		Tile.Mesh->Materials = { Surface };
		Tile.Mesh->NotifyMaterialsChanged();
		Tile.Component->MeshMaterial = Surface;
		const bool bCopied = UCSMeshOps::CopyFromMeshSnapshot(Tile.Mesh, Data);
		Tile.Component->SetGpuMesh(bCopied ? Tile.Mesh.Get() : nullptr);
		// A failed copy is written again when the tile is next laid out.
		if (!bCopied) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: the batch buildings of tile (%d, %d) could not be uploaded"), *GetName(), Key.X, Key.Y);
		Tile.Hash = bCopied ? Value : 0;
		Tile.Buildings = TileLots->Num();
		Written++;
	}
	State.Edited.Reset();
	for (const FIntPoint& Tile : Edited) if (State.Pending.Contains(Tile)) State.Edited.Add(Tile);
	State.Reach = MoveTemp(Reach);
	State.Signature = Signature;
	State.bValid = true;
	PendingBuildingTiles = State.Pending.Num();
	BuildingTilesRebuilt = Written + Removed;
	BuildingTilesLaidOut = bFull ? ByTile.Num() : Dirty.Num();
	NumBuildings = 0;
	for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) NumBuildings += Pair.Value.Buildings;
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d batch buildings in %d tiles around %d reserved plots; %d tiles laid out%s (%d inputs of %d), %d rebuilt, %d pending; %.0f ms"),
		*GetName(), NumBuildings, BuildingTiles.Num(), Plots.Num(), BuildingTilesLaidOut, bFull ? TEXT(" from scratch") : TEXT(""), Anchors.Num() + Lines.Num() + Reserved.Num(), Inputs.Num(),
		BuildingTilesRebuilt, PendingBuildingTiles, (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

void ACSRoadNetworkActor::ClearBuildingTiles()
{
	for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) if (Pair.Value.Component) Pair.Value.Component->DestroyComponent();
	BuildingTiles.Reset();
	if (LotState) LotState->Reset();
	NumBuildings = 0;
	BuildingTilesRebuilt = 0;
	BuildingTilesLaidOut = 0;
	PendingBuildingTiles = 0;
}

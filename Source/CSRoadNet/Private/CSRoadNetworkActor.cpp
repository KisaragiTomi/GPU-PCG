#include "CSRoadNetworkActor.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CSCityBuildingActor.h"
#include "CSCityLayout.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadGpuMesh.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadNetworkPreview.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTiles.h"
#include "Components/SplineComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Hash/xxhash.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGPUReadback.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

namespace
{
// The prefab data is large (pieces.bin is ~120 MB) and never changes: loaded once per directory
// and shared by every network actor. FAssets keeps pointers into the byte arrays.
struct FCSRoadNetworkActor_Library
{
	TArray<uint8> Prefabs;
	TArray<uint8> Raw;
	TArray64<uint8> Pieces;
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
	if (!Library->Assets.Load(Library->Prefabs, Library->Raw, &Library->Pieces, OutError)) return nullptr;
	Cache.Add(Key, Library);
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

// M_CS2_<NN>_<BaseColor> materials (Scripts/ImportCS2Roads.py) by base-colour texture name.
TMap<FString, UMaterialInterface*> CSRoadNetworkActor_Materials(const FString& Folder)
{
	TMap<FString, UMaterialInterface*> Result;
	if (Folder.IsEmpty()) return Result;
	IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	Registry.ScanPathsSynchronous({ Folder }, false);
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
	return Result;
}
}

ACSRoadNetworkActor::ACSRoadNetworkActor()
{
	// Ticks only to carry out rebuilds that spline roads asked for.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// The meshes are world-space and draw with an absolute transform, so they hang under a root of
	// their own: as the root, a mesh takes the actor's placement and so applies it twice.
	SetRootComponent(CreateDefaultSubobject<USceneComponent>(TEXT("Root")));
	RoadMesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("RoadMesh"));
	RoadMesh->SetupAttachment(RootComponent);
	MarkingMesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("MarkingMesh"));
	MarkingMesh->SetupAttachment(RootComponent);
	MarkingMesh->SetCastShadow(false);
	MarkingMesh->bVisibleInRayTracing = false;
	MarkingMesh->bAffectDistanceFieldLighting = false;
	MaterialFolder.Path = TEXT("/PCGPlugins/CS2Roads/Materials");
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	TrafficBodies = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TrafficBodies"));
	TrafficCabins = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("TrafficCabins"));
	BridgePiers = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("BridgePiers"));
	for (UInstancedStaticMeshComponent* Component : {TrafficBodies.Get(), TrafficCabins.Get(), BridgePiers.Get()})
	{
		Component->SetupAttachment(RootComponent);
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetStaticMesh(Cube.Object);
		Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Component->SetCanEverAffectNavigation(false);
		// Generated instance transforms must never be serialized into a level.
		Component->SetFlags(RF_Transient | RF_DuplicateTransient);
	}
	TrafficBodies->SetVisibility(false);
	TrafficCabins->SetVisibility(false);
}

ACSRoadNetworkActor::~ACSRoadNetworkActor() = default;

FString ACSRoadNetworkActor::ResolveAssetDirectory() const
{
	if (!AssetDirectory.Path.IsEmpty()) return AssetDirectory.Path;
	const FString Lab = FPlatformMisc::GetEnvironmentVariable(TEXT("CS2ROADLAB"));
	return FPaths::Combine(Lab.IsEmpty() ? FString(TEXT("D:/MyProject/CitiesSkylines-Road2-C#")) : Lab, TEXT("Assets"), TEXT("CS2"));
}

void ACSRoadNetworkActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The resident mesh is transient: a loaded or duplicated actor has no geometry until it builds.
	if (!bBuildOnLoad || IsTemplate() || !GetWorld()) return;
	if ((RoadMesh && RoadMesh->HasGeneratedGeometry()) || Tiles.Num() > 0) return;
	// Spline roads may register after this actor: build once they all have.
	if (Source == ECSRoadNetworkSource::Splines) RequestRebuild();
	else if (!NetworkFile.FilePath.IsEmpty()) Rebuild();
}

void ACSRoadNetworkActor::RequestRebuild()
{
	if (Source == ECSRoadNetworkSource::Splines) bRebuildRequested = true;
}

void ACSRoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bRebuildRequested)
	{
		bRebuildRequested = false;
		Rebuild();
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
		if (Network->GetLevel() != Level || Network->Source != ECSRoadNetworkSource::Splines) continue;
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

TArray<CSRoad::FSplineRoad> ACSRoadNetworkActor::GatherSplines() const
{
	// Spline components on this actor, the spline roads that are this network's, and the spline
	// components of any other actor hanging under it.
	TArray<USplineComponent*> Components;
	GetComponents<USplineComponent>(Components);
	if (UWorld* World = GetWorld()) for (ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World)) if (Road->Spline && Road->Spline->FindNetwork() == this) Components.Add(Road->Spline.Get());
	TArray<AActor*> Attached;
	GetAttachedActors(Attached, true, true);
	for (AActor* Actor : Attached)
	{
		if (Actor->IsA<ACSRoadSplineActor>()) continue;
		TArray<USplineComponent*> Own;
		Actor->GetComponents<USplineComponent>(Own);
		Components.Append(Own);
	}

	TArray<CSRoad::FSplineRoad> Splines;
	const FTransform NetworkToWorld = GetActorTransform();
	for (const USplineComponent* Component : Components)
	{
		const int32 Count = Component->GetNumberOfSplinePoints();
		if (Count < 2) continue;
		const UCSRoadSplineComponent* Road = Cast<UCSRoadSplineComponent>(Component);
		const FTransform ComponentToWorld = Component->GetComponentTransform();
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
			Data.SegmentOrder = Point.SegmentOrder;
			Data.bOutside = Point.bOutsideConnection;
			Data.NodeOwner = Point.NodeOwner;
			Data.SegmentOwner = Point.SegmentOwner;
		}
	}
	return Splines;
}

void ACSRoadNetworkActor::ConvertToSplines()
{
#if WITH_EDITOR
	UWorld* World = GetWorld();
	if (!World) return;
	FString Error;
	const TSharedPtr<FCSRoadNetworkActor_Library> Library = CSRoadNetworkActor_Library(ResolveAssetDirectory(), Error);
	const TSharedPtr<const CSRoad::FSavedNetwork> Network = Library ? CSRoadNetworkActor_Network(NetworkFile.FilePath, Library->Assets, false, Error) : nullptr;
	if (!Network)
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
		return;
	}
	for (ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World))
	{
		if (Road->GetAttachParentActor() != this) continue;
		LastError = TEXT("Spline roads already hang under this network; delete them before converting again");
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *LastError);
		return;
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
			Points[I].SegmentOrder = Data.SegmentOrder;
			Points[I].bOutsideConnection = Data.bOutside;
			Points[I].NodeOwner = Data.NodeOwner;
			Points[I].SegmentOwner = Data.SegmentOwner;
		}
	}
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d edges of %s converted to %d spline roads"), *GetName(), Network->Roads.Num(), *NetworkFile.FilePath, Splines.Num());
	Source = ECSRoadNetworkSource::Splines;
	bLeftHandTraffic = Network->bLeftHandTraffic;
	Rebuild();
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
	// A slider dragged in the details panel changes a property many times a frame: spline networks
	// build once on the next tick.
	if (Source == ECSRoadNetworkSource::Splines) RequestRebuild();
	else if (!NetworkFile.FilePath.IsEmpty()) Rebuild();
}

void ACSRoadNetworkActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// The resident streams are world space, so the geometry follows the actor only by rebuilding:
	// a spline network on every tick while it is dragged, a snapshot (a second or more) once let go.
	if (Source == ECSRoadNetworkSource::Splines) RequestRebuild();
	else if (bFinished && !NetworkFile.FilePath.IsEmpty()) Rebuild();
}
#endif

void ACSRoadNetworkActor::Rebuild()
{
	const double StartTime = FPlatformTime::Seconds();
	ClearPreview();
	// What spline roads asked for until now is in this build.
	bRebuildRequested = false;
	FString Error;
	if (BuildRoads(Error)) LastError.Reset();
	else
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
	}
	PendingTiles = (TileState ? TileState->NumPending() : 0) + PendingBuildingTiles;
	// The tiles past the batch go on the next ticks, until none are left or a build fails.
	if (PendingTiles > 0 && LastError.IsEmpty()) bRebuildRequested = true;
	BuildMilliseconds = float((FPlatformTime::Seconds() - StartTime) * 1000.0);
}

bool ACSRoadNetworkActor::BuildRoads(FString& OutError)
{
	if (!RoadMesh)
	{
		OutError = TEXT("No render component");
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
	// The snapshot: the roads, or building from splines only its terrain.
	const bool bSplines = Source == ECSRoadNetworkSource::Splines;
	TSharedPtr<const CSRoad::FSavedNetwork> Snapshot;
	if (!NetworkFile.FilePath.IsEmpty() && (!bSplines || bUseTerrain))
	{
		Snapshot = CSRoadNetworkActor_Network(NetworkFile.FilePath, Library->Assets, bUseTerrain, OutError);
		if (!Snapshot) return false;
	}
	else if (!bSplines)
	{
		OutError = TEXT("No network file");
		return false;
	}
	const CSRoad::FSavedTerrain* Terrain = bUseTerrain && Snapshot && Snapshot->Terrain.IsValid() ? &Snapshot->Terrain : nullptr;
	CSRoad::FSavedNetwork FromSplines;
	TArray<CSRoad::FSplineRoad> Splines;
	NumSplines = 0;
	if (bSplines)
	{
		Splines = GatherSplines();
		NumSplines = Splines.Num();
		CSRoad::FSplineNetworkSettings Settings;
		Settings.MaxEdgeLength = SplineSettings.MaxSegmentLength;
		Settings.MaxEdgeTurn = FMath::DegreesToRadians(static_cast<double>(SplineSettings.MaxSegmentTurn));
		Settings.MinEdgeLength = SplineSettings.MinSegmentLength;
		Settings.SnapDistance = SplineSettings.SnapDistance;
		Settings.JunctionHeight = SplineSettings.JunctionHeight;
		Settings.AlleyClearance = SplineSettings.AlleyClearance;
		CSRoad::FSplineNetworkStats Stats;
		if (!CSRoad::BuildSplineNetwork(Library->Assets, Splines, Settings, Terrain, FromSplines.Nodes, FromSplines.Roads, Stats, OutError)) return false;
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d splines make %d nodes and %d edges; %d alley stretches cut under main roads, %d junctions joined, %d segments split, %d merged, %d too short left out"),
			*GetName(), NumSplines, FromSplines.Nodes.Num(), FromSplines.Roads.Num(), Stats.AlleyCuts, Stats.Junctions, Stats.Splits, Stats.Merges, Stats.Dropped);
	}
	const TArray<CSRoad::FSavedNode>& Nodes = bSplines ? FromSplines.Nodes : Snapshot->Nodes;
	const TArray<CSRoad::FRoadSpec>& Specs = bSplines ? FromSplines.Roads : Snapshot->Roads;
	const double NetworkMs = Lap();
	// The buildings need only the roads' curves: they stand even if the road meshes fail below. They
	// log their own time.
	if (bBatchBuildings) BuildBatchBuildings(Splines, Specs, Library->Assets);
	else ClearBuildingTiles();
	(void)Lap();
	CSRoad::FGraph Graph;
	Graph.bLeftHandTraffic = bSplines ? bLeftHandTraffic : Snapshot->bLeftHandTraffic;
	CSRoad::FGpuGeometryInput Geometry;
	if (!Graph.Build(Library->Assets, Specs, Nodes, OutError) || !Geometry.Build(Graph, Library->Assets, Terrain, OutError)) return false;
	const double GraphMs = Lap();
	const bool bMarkings = bBuildMarkings && MarkingMesh;
	// One material slot per atlas (decal surface); MeshTriangleMaterials indexes these.
	const TMap<FString, UMaterialInterface*> ByBaseColor = CSRoadNetworkActor_Materials(MaterialFolder.Path);
	MissingAtlases.Reset();
	MissingMarkingSurfaces.Reset();
	NumMarkingTriangles = 0;
	if (bLocalUpdate)
	{
		const bool bBuilt = BuildTiles(Library->Assets, Graph, Geometry, ByBaseColor, Terrain, bMarkings, OutError);
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: prefab data %.0f ms, snapshot %.0f ms, graph and compositions %.0f ms, tiles %.0f ms"), *GetName(), LibraryMs, NetworkMs, GraphMs, Lap());
		return bBuilt;
	}
	ClearTiles();
	CSRoad::FGpuRoadMeshInput MeshInput;
	if (!MeshInput.Build(Graph, Library->Assets, OutError)) return false;
	const double MeshMs = Lap();
	// First graph: the refined node meshes' size is decided on the GPU, and the lane graph and the
	// decal setup run on the CPU from the GPU geometry. The geometry is kept for the mesh graph.
	CSRoad::FGpuGeometryResult GeometryResult;
	CSRoad::FPooledGeometry SharedGeometry;
	// Cache the lane graph even with markings/traffic hidden, so the visibility switch is instant.
	if (!CSRoad::RunRefineCountsBlocking(Geometry, MeshInput, OutError, &GeometryResult, &SharedGeometry)) return false;
	if (MeshInput.NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes"), *GetName(), MeshInput.NumUnconverged);
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
	Targets.Road = RoadMesh;
	Targets.RoadMesh = &Mesh;
	Targets.Markings = MarkingMesh;
	Targets.MarkingMesh = &Markings;
	if (!WriteMeshes(Geometry, SharedGeometry, MeshInput, bMarkings ? &Decals : nullptr, ByBaseColor, Library->Assets, Targets, OutError)) return false;
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads, %u vertices, %u triangles, %d compositions, %d decals (%d lanes); prefab data %.0f ms, snapshot %.0f ms, graph and compositions %.0f ms, composition meshes %.0f ms, refinement counts %.0f ms, lanes and decal setup %.0f ms, allocation %.0f ms, road GPU build %.0f ms, decals %.0f ms"),
		*GetName(), Graph.Roads.Num(), MeshInput.NumVertices, MeshInput.NumIndices / 3, Graph.Compositions.Num() - 1, Decals.Items.Num(), Decals.Lanes.Lanes.Num(),
		LibraryMs, NetworkMs, GraphMs, MeshMs, RefineMs, DecalSetupMs, Targets.AllocationMs, Targets.RoadMs, Targets.MarkingMs);

	NumRoads = Graph.Roads.Num();
	NumVertices = int32(MeshInput.NumVertices);
	NumTriangles = int32(MeshInput.NumIndices / 3);
	NumMarkingTriangles = Targets.MarkingTriangles;
	CSRoad::FRoadLanes PreviewLanes;
	if (!bMarkings) PreviewLanes.Build(Graph, Library->Assets, GeometryResult);
	BuildPreview(Graph, bMarkings ? Decals.Lanes : PreviewLanes, GeometryResult, Terrain);
	if (Targets.MarkingError.IsEmpty()) return true;
	OutError = Targets.MarkingError;
	return false;
}

bool ACSRoadNetworkActor::BuildTiles(CSRoad::FAssets& Assets, const CSRoad::FGraph& Graph, const CSRoad::FGpuGeometryInput& Geometry, const TMap<FString, UMaterialInterface*>& ByBaseColor,
	const CSRoad::FSavedTerrain* Terrain, bool bMarkings, FString& OutError)
{
	double LapStart = FPlatformTime::Seconds();
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};
	if (!TileState) TileState = MakeUnique<CSRoad::FTileState>();
	// The meshes of a whole build give way to the tiles, their VRAM now rather than at the next GC: a
	// network the size of Palmsland holds over a gigabyte that the tiles are about to ask for again.
	if (RoadMesh && RoadMesh->GetGpuMesh()) RoadMesh->SetGpuMesh(nullptr);
	if (MarkingMesh && MarkingMesh->GetGpuMesh()) MarkingMesh->SetGpuMesh(nullptr);
	for (UCSMesh* Whole : { Mesh.Get(), Markings.Get() }) if (Whole) Whole->ReleaseDeferred();
	Mesh = nullptr;
	Markings = nullptr;

	// First graph: the whole network's geometry, read back. It tells which roads came out different
	// from the last build, and the lanes (decals, traffic) are built from it. The geometry is kept:
	// every later graph of the build (refinement counts, each tile's meshes) registers it instead of
	// running it, and uploading its tables, again.
	CSRoad::FGpuRoadMeshInput NoInstances;
	CSRoad::FGpuGeometryResult GeometryResult;
	CSRoad::FPooledGeometry SharedGeometry;
	if (!CSRoad::RunRefineCountsBlocking(Geometry, NoInstances, OutError, &GeometryResult, &SharedGeometry)) return false;
	CSRoad::FRoadLanes Lanes;
	Lanes.Build(Graph, Assets, GeometryResult);
	const double GeometryMs = Lap();

	const uint64 Signature = TileSignature(Graph.bLeftHandTraffic);
	TArray<FIntPoint> Existing;
	Tiles.GetKeys(Existing);
	const CSRoad::FTilePlan Plan = TileState->Plan(Graph, GeometryResult, double(TileSize), Signature, Existing);
	// What each tile of the batch writes; every refined instance among them is counted in one graph.
	TArray<int32> RoadOfEntity;
	RoadOfEntity.Init(INDEX_NONE, Graph.NumEntities());
	for (int32 Road = 0; Road < Graph.Roads.Num(); Road++) RoadOfEntity[Graph.Roads[Road].Entity] = Road;
	struct FWork
	{
		FIntPoint Key = FIntPoint::ZeroValue;
		const TArray<int32>* Roads = nullptr;
		TArray<CSRoad::FGpuInstanceRef> Drawn;
		TArray<CSRoad::FGpuInstanceRef> Context;
	};
	TArray<FWork> Work;
	TArray<FIntPoint> Written;
	int32 WrittenRoads = 0;
	TArray<CSRoad::FGpuInstanceRef> All;
	TSet<int32> Listed;
	for (const TPair<FIntPoint, TArray<int32>>& Pair : Plan.Dirty)
	{
		// One batch; the tiles after it stay pending for the next ticks.
		if (MaxTilesPerBatch > 0 && Work.Num() >= MaxTilesPerBatch) break;
		Written.Add(Pair.Key);
		WrittenRoads += Pair.Value.Num();
		FWork& Item = Work.AddDefaulted_GetRef();
		Item.Key = Pair.Key;
		Item.Roads = &Pair.Value;
		CSRoad::TileInstances(Graph, RoadOfEntity, Pair.Value, Item.Drawn, Item.Context);
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
	CSRoad::FGpuRoadMeshInput Counted;
	if (All.Num() > 0 && (!Counted.BuildSubset(Graph, Assets, All, {}, &TileState->Compositions, OutError) || !CSRoad::RunRefineCountsBlocking(Geometry, Counted, OutError, nullptr, &SharedGeometry)))
	{
		TileState->Reset();
		return false;
	}
	if (Counted.NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes"), *GetName(), Counted.NumUnconverged);
	const double CountMs = Lap();

	// Each tile: the roads in it drawn, the roads around it as decal receivers only.
	FString MarkingError;
	for (const FWork& Item : Work)
	{
		CSRoad::FGpuRoadMeshInput MeshInput;
		if (Item.Drawn.Num() > 0 && (!MeshInput.BuildSubset(Graph, Assets, Item.Drawn, Item.Context, &TileState->Compositions, OutError) || !MeshInput.ApplyRefinementFrom(Counted, OutError)))
		{
			TileState->Reset();
			return false;
		}
		if (MeshInput.NumDrawnIndices == 0)
		{
			RemoveTile(Item.Key);
			continue;
		}
		TBitArray<> Keep(false, Graph.Roads.Num());
		for (const int32 Road : *Item.Roads) Keep[Road] = true;
		CSRoad::FDecalScope Scope;
		Scope.Lanes = &Lanes;
		Scope.Keep = &Keep;
		CSRoad::FGpuDecalInput Decals;
		if (bMarkings && !Decals.Build(Graph, Assets, GeometryResult, MeshInput, ResolveAssetDirectory(), OutError, &Scope))
		{
			TileState->Reset();
			return false;
		}
		FCSRoadTile& Tile = EnsureTile(Item.Key);
		FMeshTargets Targets;
		Targets.Road = Tile.Road;
		Targets.RoadMesh = &Tile.RoadMesh;
		Targets.Markings = Tile.Markings;
		Targets.MarkingMesh = &Tile.MarkingMesh;
		if (!WriteMeshes(Geometry, SharedGeometry, MeshInput, bMarkings ? &Decals : nullptr, ByBaseColor, Assets, Targets, OutError))
		{
			TileState->Reset();
			return false;
		}
		Tile.Roads = Item.Roads->Num();
		Tile.Vertices = int32(MeshInput.NumDrawnVertices);
		Tile.Triangles = int32(MeshInput.NumDrawnIndices / 3);
		Tile.MarkingTriangles = Targets.MarkingTriangles;
		if (MarkingError.IsEmpty()) MarkingError = Targets.MarkingError;
	}
	TileState->Commit(Plan, Signature, Written);
	const double TileMs = Lap();

	NumRoads = Graph.Roads.Num();
	NumTiles = Tiles.Num();
	TilesRebuilt = Work.Num();
	RoadsRebuilt = WrittenRoads;
	NumVertices = 0;
	NumTriangles = 0;
	NumMarkingTriangles = 0;
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles)
	{
		NumVertices += Pair.Value.Vertices;
		NumTriangles += Pair.Value.Triangles;
		NumMarkingTriangles += Pair.Value.MarkingTriangles;
	}
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads in %d tiles, %d tiles (%d roads) rebuilt, %d pending; geometry and lanes %.0f ms, refinement counts %.0f ms, tile meshes %.0f ms"),
		*GetName(), NumRoads, NumTiles, TilesRebuilt, RoadsRebuilt, TileState->NumPending(), GeometryMs, CountMs, TileMs);
	BuildPreview(Graph, Lanes, GeometryResult, Terrain);
	if (MarkingError.IsEmpty()) return true;
	OutError = MarkingError;
	return false;
}

bool ACSRoadNetworkActor::WriteMeshes(const CSRoad::FGpuGeometryInput& Geometry, const CSRoad::FPooledGeometry& SharedGeometry, const CSRoad::FGpuRoadMeshInput& MeshInput, CSRoad::FGpuDecalInput* Decals,
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
	// Instances written only as decal receivers sit past the drawn ones: in the capacity, not the counters.
	if (!RoadObject->EnsureCapacitySync(int32(MeshInput.NumVertices), int32(MeshInput.NumIndices)))
	{
		OutError = FString::Printf(TEXT("Could not allocate %u vertices / %u indices"), MeshInput.NumVertices, MeshInput.NumIndices);
		return false;
	}
	RoadObject->EnsureIndirectDrawCapacitySync(NumSlots);
	RoadComponent->SetGpuMesh(RoadObject);
	Targets.AllocationMs = Lap();

	// Second graph: the road surface, and the decals' receivers gathered from it. The decals refer to
	// road triangles by their place in the index stream, so the road's material sort (which reorders
	// it) waits until they have been drawn.
	const FTransform ActorToWorld = GetActorTransform();
	const FMatrix44f LocalToWorld(ActorToWorld.ToMatrixWithScale());
	const FMatrix44f WorldToLocal(ActorToWorld.ToInverseMatrixWithScale());
	const FBox WorldBounds = MeshInput.Bounds.TransformBy(ActorToWorld);
	const bool bDecals = Decals && Decals->Items.Num() > 0;
	CSRoad::FPooledDecalReceivers PooledReceivers;
	TUniquePtr<FRHIGPUBufferReadback> ClipReadback;
	if (bDecals && Decals->ClippedItems.Num() > 0) ClipReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("CSRoad.ClipCountsReadback"));
	FRHIGPUBufferReadback* ClipReadbackPtr = ClipReadback.Get();
	const uint32 ClipBytes = bDecals ? uint32(Decals->ClippedItems.Num() * sizeof(uint32)) : 0u;
	bool bSorted = false;
	bool bRecorded = false;
	RoadObject->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		if (!Context.Positions() || !Context.Tangents() || !Context.TexCoords() || !Context.Colors() || !Context.Indices() || !Context.MaterialIds()) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		const CSRoad::FGpuGeometryOutput GeometryOut = SharedGeometry.IsValid() ? SharedGeometry.Register(GraphBuilder) : CSRoad::AddGeometryPasses(GraphBuilder, Geometry);
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
		CSRoad::AddRoadMeshPasses(GraphBuilder, GeometryOut, MeshInput, Target);
		UCSMeshOps::AddSetCountersPass(Context, MeshInput.NumDrawnVertices, MeshInput.NumDrawnIndices);
		Context.SetWorldBounds(WorldBounds);
		if (bDecals)
		{
			CSRoad::FGpuRoadStreams Road;
			Road.Positions = Context.Positions();
			Road.Tangents = Context.Tangents();
			Road.Indices = Context.Indices();
			Road.WorldToLocal = WorldToLocal;
			const CSRoad::FGpuDecalReceivers Receivers = CSRoad::AddDecalReceiverPasses(GraphBuilder, MeshInput, *Decals, Road);
			PooledReceivers.Extract(GraphBuilder, Receivers);
			if (ClipReadbackPtr) AddEnqueueCopyPass(GraphBuilder, ClipReadbackPtr, Receivers.ClipCounts, ClipBytes);
		}
		else bSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
		bRecorded = true;
	});
	if (!bRecorded)
	{
		OutError = TEXT("The road mesh has no vertex streams");
		return false;
	}
	if (bSorted) UCSMeshOps::PublishMaterialSections(RoadObject);
	Targets.RoadMs = Lap();

	// Decals that cannot be built (say, too little VRAM for their mesh) leave the roads standing; the
	// error is kept in the targets.
	Targets.MarkingTriangles = 0;
	Targets.MarkingError.Reset();
	bool bMarked = false;
	if (bDecals)
	{
		bMarked = BuildMarkingMesh(*Decals, MeshInput, ByBaseColor, Assets, PooledReceivers, ClipReadbackPtr, RoadObject, Targets, LocalToWorld, WorldToLocal, WorldBounds, Targets.MarkingError);
		// Fourth graph: the road's material sections, now that the decals have read its triangles.
		RoadObject->EditMeshSync([&](FCSMeshEditContext& Context) { bSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots); });
		if (bSorted) UCSMeshOps::PublishMaterialSections(RoadObject);
		Targets.MarkingMs = Lap();
	}
	if (Targets.Markings) Targets.Markings->SetVisibility(bMarked);
	return true;
}

bool ACSRoadNetworkActor::BuildMarkingMesh(CSRoad::FGpuDecalInput& Decals, const CSRoad::FGpuRoadMeshInput& MeshInput, const TMap<FString, UMaterialInterface*>& ByBaseColor,
	const CSRoad::FAssets& Assets, const CSRoad::FPooledDecalReceivers& Receivers, FRHIGPUBufferReadback* ClipReadback, UCSMesh* RoadObject, FMeshTargets& Targets,
	const FMatrix44f& LocalToWorld, const FMatrix44f& WorldToLocal, const FBox& WorldBounds, FString& OutError)
{
	if (!Receivers.IsValid())
	{
		OutError = TEXT("The decal receivers were not gathered");
		return false;
	}
	// The clipped crosswalks' triangle counts, from the road mesh's graph.
	TArray<uint32> ClipCounts;
	ClipCounts.SetNumZeroed(Decals.ClippedItems.Num());
	if (ClipReadback)
	{
		bool bRead = false;
		void* Target = ClipCounts.GetData();
		const uint32 Bytes = uint32(ClipCounts.Num() * sizeof(uint32));
		ENQUEUE_RENDER_COMMAND(CSRoadClipCounts)([ClipReadback, Target, Bytes, &bRead](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.SubmitAndBlockUntilGPUIdle();
			const void* Data = ClipReadback->IsReady() ? ClipReadback->Lock(Bytes) : nullptr;
			if (!Data) return;
			FMemory::Memcpy(Target, Data, Bytes);
			ClipReadback->Unlock();
			bRead = true;
		});
		FlushRenderingCommands();
		if (!bRead)
		{
			OutError = TEXT("The crosswalk clip counts were not read back");
			return false;
		}
	}
	if (!Decals.ApplyClipCounts(ClipCounts, OutError)) return false;
	UCSMeshRenderComponent* MarkingComponent = Targets.Markings;
	if (Decals.NumVertices == 0 || Decals.NumIndices == 0)
	{
		// Nothing to draw: no stale decals either.
		MarkingComponent->SetGpuMesh(nullptr);
		return true;
	}

	TArray<TObjectPtr<UMaterialInterface>> Materials;
	for (const FString& Surface : Decals.MaterialSlots)
	{
		UMaterialInterface* const* Found = ByBaseColor.Find(Assets.SurfaceBaseColor(Surface));
		if (!Found) MissingMarkingSurfaces.AddUnique(Surface);
		Materials.Add(Found ? *Found : FallbackMaterial.Get());
	}
	TObjectPtr<UCSMesh>& MarkingObject = *Targets.MarkingMesh;
	if (!MarkingObject || MarkingObject->GetOuter() != MarkingComponent) MarkingObject = NewObject<UCSMesh>(MarkingComponent);
	// UV0 the decal texture, UV1 the distances to both ends, UV2 / UV3 the colour (CSRoadDecal.usf).
	if (!UCSMeshOps::EnsureTexCoordSets(MarkingObject, 4))
	{
		OutError = TEXT("The decal mesh could not take four UV sets");
		return false;
	}
	MarkingObject->Materials = Materials;
	MarkingObject->NotifyMaterialsChanged();
	MarkingComponent->MeshMaterial = Materials.Num() > 0 ? Materials[0].Get() : FallbackMaterial.Get();
	const int32 NumSlots = FMath::Max(1, Materials.Num());
	if (!MarkingObject->EnsureCapacitySync(int32(Decals.NumVertices), int32(Decals.NumIndices)))
	{
		OutError = FString::Printf(TEXT("Could not allocate %u decal vertices / %u indices"), Decals.NumVertices, Decals.NumIndices);
		return false;
	}
	MarkingObject->EnsureIndirectDrawCapacitySync(NumSlots);
	MarkingComponent->SetGpuMesh(MarkingObject);

	// Third graph: the decals, reading the road mesh's streams.
	FCSMeshResident* RoadResident = RoadObject ? RoadObject->GetResidentPtr() : nullptr;
	bool bSorted = false;
	bool bRecorded = false;
	MarkingObject->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		if (!Context.Positions() || !Context.Tangents() || !Context.TexCoords() || !Context.Colors() || !Context.Indices() || !Context.MaterialIds() || !RoadResident) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		{
			FCSMeshRenderThreadEdit RoadView(GraphBuilder, *RoadResident);
			CSRoad::FGpuRoadStreams Road;
			Road.Positions = RoadView->Positions();
			Road.Tangents = RoadView->Tangents();
			Road.Indices = RoadView->Indices();
			Road.WorldToLocal = WorldToLocal;
			if (!Road.Positions || !Road.Tangents || !Road.Indices) return;
			CSRoad::FGpuDecalTarget Target;
			Target.Positions = Context.Positions();
			Target.Tangents = Context.Tangents();
			Target.TexCoords = Context.TexCoords();
			Target.Colors = Context.Colors();
			Target.Indices = Context.Indices();
			Target.MaterialIds = Context.MaterialIds();
			Target.VertexCapacity = Context.Resident.VertexCapacity;
			Target.IndexCapacity = Context.Resident.IndexCapacity;
			Target.LocalToWorld = LocalToWorld;
			CSRoad::AddDecalMeshPasses(GraphBuilder, MeshInput, Decals, Road, Receivers.Register(GraphBuilder), Target);
		}
		UCSMeshOps::AddSetCountersPass(Context, Decals.NumVertices, Decals.NumIndices);
		Context.SetWorldBounds(WorldBounds);
		bSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
		bRecorded = true;
	});
	if (!bRecorded)
	{
		OutError = TEXT("The decal mesh or the road mesh has no vertex streams");
		return false;
	}
	if (bSorted) UCSMeshOps::PublishMaterialSections(MarkingObject);
	Targets.MarkingTriangles = int32(Decals.NumIndices / 3);
	return true;
}

FCSRoadTile& ACSRoadNetworkActor::EnsureTile(const FIntPoint& Key)
{
	FCSRoadTile& Tile = Tiles.FindOrAdd(Key);
	// Made at build time, never saved or copied: a loaded or duplicated network builds its own.
	auto Make = [this, &Key](const TCHAR* Kind)
	{
		const FName Name = MakeUniqueObjectName(this, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("%sTile_%d_%d"), Kind, Key.X, Key.Y));
		UCSMeshRenderComponent* Component = NewObject<UCSMeshRenderComponent>(this, Name, RF_Transient | RF_DuplicateTransient);
		Component->SetupAttachment(RootComponent);
		return Component;
	};
	if (!Tile.Road)
	{
		Tile.Road = Make(TEXT("Road"));
		Tile.Road->RegisterComponent();
	}
	if (!Tile.Markings)
	{
		// As MarkingMesh: the decals float 2 cm over the road.
		Tile.Markings = Make(TEXT("Marking"));
		Tile.Markings->SetCastShadow(false);
		Tile.Markings->bVisibleInRayTracing = false;
		Tile.Markings->bAffectDistanceFieldLighting = false;
		Tile.Markings->RegisterComponent();
	}
	return Tile;
}

void ACSRoadNetworkActor::RemoveTile(const FIntPoint& Key)
{
	FCSRoadTile Tile;
	if (!Tiles.RemoveAndCopyValue(Key, Tile)) return;
	// A component frees the meshes it owns when destroyed.
	for (UCSMeshRenderComponent* Component : { Tile.Road.Get(), Tile.Markings.Get() }) if (Component) Component->DestroyComponent();
}

void ACSRoadNetworkActor::ClearTiles()
{
	TArray<FIntPoint> Keys;
	Tiles.GetKeys(Keys);
	for (const FIntPoint& Key : Keys) RemoveTile(Key);
	if (TileState) TileState->Reset();
	NumTiles = 0;
	TilesRebuilt = 0;
	RoadsRebuilt = 0;
}

uint64 ACSRoadNetworkActor::TileSignature(bool bLeftHand) const
{
	FXxHash64Builder Hash;
	auto Add = [&Hash](const auto& Value) { Hash.Update(&Value, sizeof(Value)); };
	Add(TileSize);
	Add(bLeftHand);
	Add(bBuildMarkings);
	const FMatrix Placement = GetActorTransform().ToMatrixWithScale();
	Add(Placement);
	const UMaterialInterface* Fallback = FallbackMaterial.Get();
	Add(Fallback);
	const FString Folders = MaterialFolder.Path + TEXT("|") + ResolveAssetDirectory();
	Hash.Update(*Folders, Folders.Len() * sizeof(TCHAR));
	return Hash.Finalize().Hash;
}

void ACSRoadNetworkActor::BuildBatchBuildings(const TArray<CSRoad::FSplineRoad>& Splines, const TArray<CSRoad::FRoadSpec>& Roads, CSRoad::FAssets& Assets)
{
	const double StartTime = FPlatformTime::Seconds();
	const FTransform NetworkToWorld = GetActorTransform();
	auto HalfWidth = [&Assets](const FString& Prefab) { return double(Assets.Geometry(Prefab).m_DefaultWidth) * 50.0; };
	auto Combine = [](uint64 A, uint64 B)
	{
		const uint64 Values[] = { A, B };
		return FXxHash64::HashBuffer(Values, sizeof(Values)).Hash;
	};
	// A cubic Bezier of the network's space as world points about 2 m apart.
	auto Sample = [&NetworkToWorld](const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector3d& D, TArray<FVector>& Out)
	{
		const int32 Steps = FMath::Clamp(FMath::CeilToInt32((FVector3d::Distance(A, B) + FVector3d::Distance(B, C) + FVector3d::Distance(C, D)) / 2.0), 1, 256);
		for (int32 S = 0; S <= Steps; S++)
		{
			const double T = double(S) / Steps;
			const double U = 1.0 - T;
			Out.Add(NetworkToWorld.TransformPosition(CSRoadNetworkActor_ToUnreal(A * (U * U * U) + B * (3.0 * U * U * T) + C * (3.0 * U * T * T) + D * (T * T * T))));
		}
	};
	auto Point = [](const CSRoad::float3& P) { return FVector3d(P.x, P.y, P.z); };

	// The roads as built, which the plots keep clear of; building from a snapshot the plots face
	// them, and its edge ids keep their slots.
	TArray<CSCity::FRoadLine> Built;
	for (int32 I = 0; I < Roads.Num(); I++)
	{
		const CSRoad::FRoadSpec& Road = Roads[I];
		CSCity::FRoadLine& Line = Built.AddDefaulted_GetRef();
		Line.Id = Combine(Road.bHasSource ? uint64(uint32(Road.SourceId)) : uint64(I), Road.bHasSource ? 1 : 0);
		Line.HalfWidth = HalfWidth(Road.Prefab);
		Sample(Point(Road.Curve.a), Point(Road.Curve.b), Point(Road.Curve.c), Point(Road.Curve.d), Line.Points);
	}
	// Building from splines the plots face their segments as drawn, not the edges cut from them: a road
	// joining the spline, or an edit of another segment, leaves the slots where they were.
	TArray<CSCity::FRoadLine> Segments;
	for (const CSRoad::FSplineRoad& Spline : Splines)
	{
		const int32 Count = Spline.Points.Num();
		const double Half = HalfWidth(Spline.Prefab);
		for (int32 I = 0; I < (Spline.bClosed ? Count : Count - 1); I++)
		{
			const int32 J = (I + 1) % Count;
			const FVector3d& A = Spline.Points[I];
			const FVector3d& D = Spline.Points[J];
			const bool bLinear = Spline.Linear.IsValidIndex(I) && Spline.Linear[I];
			CSCity::FRoadLine& Line = Segments.AddDefaulted_GetRef();
			Line.Id = Combine(Spline.Id, uint64(I));
			Line.HalfWidth = Half;
			// A USplineComponent segment's Hermite tangents as Bezier handles.
			Sample(A, bLinear ? FMath::Lerp(A, D, 1.0 / 3.0) : A + Spline.Leave[I] / 3.0, bLinear ? FMath::Lerp(A, D, 2.0 / 3.0) : D - Spline.Arrive[J] / 3.0, D, Line.Points);
		}
	}
	TArray<CSCity::FPolygon> Reserved;
	if (UWorld* World = GetWorld())
	{
		for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
		{
			if (NetworkOf(Building) != this) continue;
			const CSCity::FPolygon Plot = Building->Reservation();
			if (Plot.Num() >= 3) Reserved.Add(Plot);
		}
	}
	CSCity::FLotSettings Settings;
	Settings.Frontage = double(Lots.Frontage) * 100.0;
	Settings.Gap = double(Lots.Gap) * 100.0;
	Settings.Setback = double(Lots.Setback) * 100.0;
	Settings.MinDepth = double(Lots.MinDepth) * 100.0;
	Settings.MaxDepth = double(FMath::Max(Lots.MinDepth, Lots.MaxDepth)) * 100.0;
	Settings.MinHeight = double(Lots.MinHeight) * 100.0;
	Settings.MaxHeight = double(FMath::Max(Lots.MinHeight, Lots.MaxHeight)) * 100.0;
	Settings.Seed = Lots.Seed;
	const TArray<CSCity::FLot> Plots = CSCity::LayoutLots(Source == ECSRoadNetworkSource::Splines ? Segments : Built, Built, Reserved, Settings);

	// By the roads' tiles (the network's ground plane, Unity x and z, in TileSize squares), each in id
	// order: what a tile's mesh is made of.
	const double TileCm = double(TileSize) * 100.0;
	TMap<FIntPoint, TArray<const CSCity::FLot*>> ByTile;
	for (const CSCity::FLot& Lot : Plots)
	{
		const FVector Local = NetworkToWorld.InverseTransformPosition(FVector(Lot.Centre, Lot.Base));
		ByTile.FindOrAdd(FIntPoint(FMath::FloorToInt32(Local.X / TileCm), FMath::FloorToInt32(-Local.Y / TileCm))).Add(&Lot);
	}
	// Tiles in row order, a batch of them rewritten at most: the others keep their old buildings, and
	// hash, until a later tick.
	ByTile.KeySort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
	UMaterialInterface* Surface = BuildingMaterial ? BuildingMaterial.Get() : ACSCityBuildingActor::DefaultMaterial();
	int32 Rebuilt = 0;
	PendingBuildingTiles = 0;
	for (TPair<FIntPoint, TArray<const CSCity::FLot*>>& Pair : ByTile)
	{
		TArray<const CSCity::FLot*>& TileLots = Pair.Value;
		TileLots.Sort([](const CSCity::FLot& A, const CSCity::FLot& B) { return A.Id < B.Id; });
		FXxHash64Builder Hash;
		Hash.Update(&Surface, sizeof(Surface));
		for (const CSCity::FLot* Lot : TileLots)
		{
			Hash.Update(&Lot->Id, sizeof(Lot->Id));
			Hash.Update(&Lot->Base, sizeof(Lot->Base));
			Hash.Update(&Lot->Height, sizeof(Lot->Height));
			Hash.Update(&Lot->Color, sizeof(Lot->Color));
			Hash.Update(Lot->Corners.GetData(), Lot->Corners.Num() * sizeof(FVector2D));
		}
		const uint64 Value = Hash.Finalize().Hash;
		const FCSCityBuildingTile* Known = BuildingTiles.Find(Pair.Key);
		if (Known && Known->Component && Known->Mesh && Known->Hash == Value) continue;
		if (MaxTilesPerBatch > 0 && Rebuilt >= MaxTilesPerBatch)
		{
			PendingBuildingTiles++;
			continue;
		}
		FCSCityBuildingTile& Tile = BuildingTiles.FindOrAdd(Pair.Key);
		if (!Tile.Component)
		{
			// As the road tiles: made at build time, never saved or copied.
			const FName Name = MakeUniqueObjectName(this, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("BuildingTile_%d_%d"), Pair.Key.X, Pair.Key.Y));
			Tile.Component = NewObject<UCSMeshRenderComponent>(this, Name, RF_Transient | RF_DuplicateTransient);
			Tile.Component->SetupAttachment(RootComponent);
			Tile.Component->RegisterComponent();
		}
		if (!Tile.Mesh || Tile.Mesh->GetOuter() != Tile.Component) Tile.Mesh = NewObject<UCSMesh>(Tile.Component);
		FCSGpuMeshCPUData Data;
		for (const CSCity::FLot* Lot : TileLots) CSCity::AppendPrism(Data, Lot->Corners, Lot->Base, Lot->Height, Lot->Color);
		Tile.Mesh->Materials = { Surface };
		Tile.Mesh->NotifyMaterialsChanged();
		Tile.Component->MeshMaterial = Surface;
		const bool bCopied = UCSMeshOps::CopyFromMeshSnapshot(Tile.Mesh, Data);
		Tile.Component->SetGpuMesh(bCopied ? Tile.Mesh.Get() : nullptr);
		// A failed copy is tried again next time.
		Tile.Hash = bCopied ? Value : 0;
		Tile.Buildings = TileLots.Num();
		Rebuilt++;
	}
	TArray<FIntPoint> Gone;
	for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) if (!ByTile.Contains(Pair.Key)) Gone.Add(Pair.Key);
	for (const FIntPoint& Key : Gone)
	{
		FCSCityBuildingTile Tile;
		BuildingTiles.RemoveAndCopyValue(Key, Tile);
		if (Tile.Component) Tile.Component->DestroyComponent();
	}
	NumBuildings = Plots.Num();
	BuildingTilesRebuilt = Rebuilt + Gone.Num();
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d batch buildings in %d tiles around %d reserved plots, %d tiles rebuilt, %d pending; %.0f ms"),
		*GetName(), NumBuildings, BuildingTiles.Num(), Reserved.Num(), BuildingTilesRebuilt, PendingBuildingTiles, (FPlatformTime::Seconds() - StartTime) * 1000.0);
}

void ACSRoadNetworkActor::ClearBuildingTiles()
{
	for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) if (Pair.Value.Component) Pair.Value.Component->DestroyComponent();
	BuildingTiles.Reset();
	NumBuildings = 0;
	BuildingTilesRebuilt = 0;
	PendingBuildingTiles = 0;
}

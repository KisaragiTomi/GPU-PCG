#include "CSRoadNetworkActor.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CSCityBuildingActor.h"
#include "CSCityLayout.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadDecalInstances.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadGpuMesh.h"
#include "CSRoadNanite.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadNetworkPreview.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTiles.h"
#include "Components/SplineComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "UObject/ConstructorHelpers.h"
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
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "RenderingThread.h"
#include "RenderTimer.h"
#include "RHICommandList.h"
#if WITH_EDITOR
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

// Some of a network's roads (spec indices, in spec order) built on their own: the graph, its GPU
// geometry read back, and the geometry kept for the build's later graphs.
struct FCSRoadNetworkActor_Subgraph
{
	TArray<int32> Specs;
	// Graph.Roads index per spec of the network (INDEX_NONE: not in it).
	TArray<int32> RoadOfSpec;
	CSRoad::FGraph Graph;
	CSRoad::FGpuGeometryInput Input;
	CSRoad::FGpuGeometryResult Result;
	CSRoad::FPooledGeometry Shared;

	bool Holds(const TArray<int32>& Roads) const
	{
		for (const int32 Road : Roads) if (!RoadOfSpec.IsValidIndex(Road) || RoadOfSpec[Road] == INDEX_NONE) return false;
		return true;
	}
};

bool CSRoadNetworkActor_BuildSubgraph(CSRoad::FAssets& Assets, const TArray<CSRoad::FRoadSpec>& Specs, const TArray<CSRoad::FSavedNode>& Nodes, bool bLeftHand,
	const CSRoad::FSavedTerrain* Terrain, TArray<int32> Taken, FCSRoadNetworkActor_Subgraph& Out, FString& OutError)
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
	CSRoad::FGpuRoadMeshInput NoInstances;
	if (!bBuilt || !Out.Input.Build(Out.Graph, Assets, Terrain, OutError) || !CSRoad::RunRefineCountsBlocking(Out.Input, NoInstances, OutError, &Out.Result, &Out.Shared)) return false;
	Out.RoadOfSpec.Init(INDEX_NONE, Specs.Num());
	for (int32 Road = 0; Road < Out.Graph.Roads.Num(); Road++) Out.RoadOfSpec[Out.Specs[Out.Graph.Roads[Road].SpecIndex]] = Road;
	return true;
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
	// The decals are DBuffer mesh decals: they show only on receivers.
	RoadMesh->bReceivesDecals = true;
	MaterialFolder.Path = TEXT("/PCGPlugins/CS2Roads/Materials");
	DecalMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/PCGPlugins/CSRoadNet/M_RoadDecal.M_RoadDecal")));
	FittedDecalMaterial = TSoftObjectPtr<UMaterialInterface>(FSoftObjectPath(TEXT("/PCGPlugins/CSRoadNet/M_RoadDecalFitted.M_RoadDecalFitted")));
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

ACSRoadNetworkActor::ACSRoadNetworkActor(FVTableHelper& Helper)
	: Super(Helper)
{
}

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
	if (Source != ECSRoadNetworkSource::Splines) return;
	bRebuildRequested = true;
	bPendingOnly = false;
}

void ACSRoadNetworkActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Start = FPlatformTime::Seconds();
	TickBakes();
	TickAllDecalFits();
	const double BakeMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	if (bRebuildRequested)
	{
		// Tiles left pending wait until bakes free room in the budget for the next one.
		const int64 Held = bPendingOnly ? GpuMeshBytes() : 0;
		if (!bPendingOnly || Held == 0 || Held + NextTileBytes <= GpuMeshBudgetBytes())
		{
			bRebuildRequested = false;
			bReuseNetwork = bPendingOnly;
			Rebuild();
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
			Data.bOutside = Point.bOutsideConnection;
			Data.bNodeOwned = Point.bNodeOwned;
			Data.bSegmentOwned = Point.bSegmentOwned;
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
			Points[I].bOutsideConnection = Data.bOutside;
			Points[I].bNodeOwned = Data.bNodeOwned;
			Points[I].bSegmentOwned = Data.bSegmentOwned;
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
	// What spline roads asked for until now is in this build. The build makes the preview again, or,
	// building in tiles, RefreshPreview does once edits settle; until then the last one stays up.
	bRebuildRequested = false;
	FString Error;
	if (BuildRoads(Error)) LastError.Reset();
	else
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
	}
	UpdateBakeStats();
	PendingTiles = (TileState ? TileState->NumPending() : 0) + PendingBuildingTiles;
	// The tiles past the batch go on the next ticks, until none are left or a build fails.
	bPendingOnly = PendingTiles > 0 && LastError.IsEmpty();
	if (bPendingOnly) bRebuildRequested = true;
	LastBuildSeconds = FPlatformTime::Seconds();
	BuildMilliseconds = float((LastBuildSeconds - StartTime) * 1000.0);
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
	// Building from splines: the spline roads and their network. A rebuild that only writes pending tiles
	// (nothing changed since the last one) takes the last build's as they are.
	TSharedPtr<const TArray<CSRoad::FSplineRoad>> Splines = MakeShared<TArray<CSRoad::FSplineRoad>>();
	TSharedPtr<const CSRoad::FSavedNetwork> FromSplines;
	if (bSplines && bReuseNetwork && LastSplines && PreviewRoads)
	{
		Splines = LastSplines;
		FromSplines = PreviewRoads;
	}
	else if (bSplines)
	{
		TSharedPtr<TArray<CSRoad::FSplineRoad>> Gathered = MakeShared<TArray<CSRoad::FSplineRoad>>(GatherSplines());
		TSharedPtr<CSRoad::FSavedNetwork> Built = MakeShared<CSRoad::FSavedNetwork>();
		CSRoad::FSplineNetworkSettings Settings;
		Settings.MaxEdgeLength = SplineSettings.MaxSegmentLength;
		Settings.MaxEdgeTurn = FMath::DegreesToRadians(static_cast<double>(SplineSettings.MaxSegmentTurn));
		Settings.MinEdgeLength = SplineSettings.MinSegmentLength;
		Settings.SnapDistance = SplineSettings.SnapDistance;
		Settings.JunctionHeight = SplineSettings.JunctionHeight;
		Settings.AlleyClearance = SplineSettings.AlleyClearance;
		CSRoad::FSplineNetworkStats Stats;
		if (!CSRoad::BuildSplineNetwork(Library->Assets, *Gathered, Settings, Terrain, Built->Nodes, Built->Roads, Stats, OutError)) return false;
		Built->bLeftHandTraffic = bLeftHandTraffic;
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d splines make %d nodes and %d edges; %d alley stretches cut under main roads, %d junctions joined, %d segments split, %d merged, %d too short left out"),
			*GetName(), Gathered->Num(), Built->Nodes.Num(), Built->Roads.Num(), Stats.AlleyCuts, Stats.Junctions, Stats.Splits, Stats.Merges, Stats.Dropped);
		Splines = Gathered;
		FromSplines = Built;
	}
	NumSplines = Splines->Num();
	const TArray<CSRoad::FSavedNode>& Nodes = bSplines ? FromSplines->Nodes : Snapshot->Nodes;
	const TArray<CSRoad::FRoadSpec>& Specs = bSplines ? FromSplines->Roads : Snapshot->Roads;
	const bool bLeftHand = bSplines ? bLeftHandTraffic : Snapshot->bLeftHandTraffic;
	const double NetworkMs = Lap();
	// The buildings need only the roads' curves: they stand even if the road meshes fail below. They
	// log their own time.
	if (bBatchBuildings) BuildBatchBuildings(*Splines, Specs, Library->Assets);
	else ClearBuildingTiles();
	(void)Lap();
	const bool bMarkings = bBuildMarkings;
	// One material slot per atlas (decal surface); MeshTriangleMaterials indexes these.
	const TMap<FString, UMaterialInterface*> ByBaseColor = CSRoadNetworkActor_Materials(MaterialFolder.Path);
	MissingAtlases.Reset();
	MissingMarkingSurfaces.Reset();
	NumMarkingInstances = 0;
	if (bLocalUpdate)
	{
		const bool bBuilt = BuildTiles(Library->Assets, Specs, Nodes, bLeftHand, ByBaseColor, Terrain, bMarkings, OutError);
		// What RefreshPreview makes the preview from once edits settle.
		PreviewRoads = bSplines ? FromSplines : Snapshot;
		LastSplines = bSplines ? Splines : nullptr;
		PreviewTerrain = Terrain ? Snapshot : nullptr;
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: prefab data %.0f ms, snapshot %.0f ms, tiles %.0f ms"), *GetName(), LibraryMs, NetworkMs, Lap());
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
	if (MeshInput.NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes"), *GetName(), MeshInput.NumUnconverged);
	const double RefineMs = Lap();
	CSRoad::FGpuDecalInput Decals;
	Decals.bFitted = true;
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
	if (bMarkings)
	{
		ResolveDecalMaterials(Decals, ByBaseColor, Library->Assets);
		Targets.Decals = &Decals;
	}
	if (!WriteMeshes(Geometry, SharedGeometry, MeshInput, ByBaseColor, Library->Assets, Targets, OutError)) return false;
	(void)Lap();
	if (bMarkings)
	{
		const int32 Drawn = DrawDecals(Decals, Targets.Fit, MarkingFits, MarkingDecals, TEXT("Marking"));
		if (Drawn >= 0) NumMarkingInstances = Drawn;
	}
	else
	{
		MarkingFits.Reset();
		DestroyComponents(MarkingDecals);
	}
	const double DecalMs = Lap();
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads, %u vertices, %u triangles, %d compositions, %d decals (%d lanes, %d instances); prefab data %.0f ms, snapshot %.0f ms, graph and compositions %.0f ms, geometry input %.0f ms, composition meshes %.0f ms, refinement counts %.0f ms, lanes and decal setup %.0f ms, allocation %.0f ms, road GPU build %.0f ms, decals %.0f ms"),
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

bool ACSRoadNetworkActor::BuildTiles(CSRoad::FAssets& Assets, const TArray<CSRoad::FRoadSpec>& Specs, const TArray<CSRoad::FSavedNode>& Nodes, bool bLeftHand,
	const TMap<FString, UMaterialInterface*>& ByBaseColor, const CSRoad::FSavedTerrain* Terrain, bool bMarkings, FString& OutError)
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
	auto Fail = [this]()
	{
		TileState->Reset();
		return false;
	};
	// The mesh of a whole build gives way to the tiles, its VRAM now rather than at the next GC: a
	// network the size of Palmsland holds over half a gigabyte that the tiles are about to ask for again.
	if (RoadMesh && RoadMesh->GetGpuMesh()) RoadMesh->SetGpuMesh(nullptr);
	if (Mesh) Mesh->ReleaseDeferred();
	Mesh = nullptr;
	MarkingFits.Reset();
	DestroyComponents(MarkingDecals);

	const uint64 Signature = TileSignature(bLeftHand);
	const bool bEverything = !TileState->Matches(Signature);
	CSRoad::FTileNetwork Network;
	Network.Build(Specs, Nodes, double(TileSize));
	TArray<int32> Everything;
	if (bEverything) for (int32 Road = 0; Road < Specs.Num(); Road++) Everything.Add(Road);

	// The roads whose output may differ from the last build's, built with two rings more: they come out
	// as in the whole network (Subgraph.Reach). Every road the first time, or after a change of settings.
	const TArray<int32> Affected = bEverything ? Everything : TileState->Affected(Network);
	FCSRoadNetworkActor_Subgraph Detected;
	TArray<uint64> Outputs;
	if (Affected.Num() > 0)
	{
		if (!CSRoadNetworkActor_BuildSubgraph(Assets, Specs, Nodes, bLeftHand, Terrain, bEverything ? Everything : Network.Around(Affected, {}, {}, 2), Detected, OutError)) return Fail();
		Outputs.Reserve(Affected.Num());
		for (const int32 Road : Affected) Outputs.Add(CSRoad::TileOutput(Detected.Graph, Detected.Result, Detected.RoadOfSpec[Road], Network.Roads[Road].Key));
	}
	const double DetectMs = Lap();

	TArray<FIntPoint> Existing;
	Tiles.GetKeys(Existing);
	const TMap<FIntPoint, TArray<int32>> Dirty = TileState->Dirty(Network, Affected, Outputs, bEverything, Existing);
	// One batch of the tiles to rebuild, the ones this edit dirtied first; the rest stay pending. While
	// tiles bake into Nanite, the batch takes what the GPU mesh budget has room for (a tile drawn now
	// gives back what its GPU mesh holds), and one tile regardless while no GPU mesh holds any.
	TArray<FIntPoint> Written;
	TArray<int32> Batch;
	const int64 Budget = GpuMeshBudgetBytes();
	NextTileBytes = 0;
	int64 Held = Budget < MAX_int64 ? GpuMeshBytes() : 0;
	double TrianglesPerRoad = 2350.0;
	if (Budget < MAX_int64)
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
	for (const TPair<FIntPoint, TArray<int32>>& Pair : Dirty)
	{
		if (MaxTilesPerBatch > 0 && Written.Num() >= MaxTilesPerBatch) break;
		if (Budget < MAX_int64 && Pair.Value.Num() > 0)
		{
			const FCSRoadTile* Known = Tiles.Find(Pair.Key);
			const int64 Change = EstimateTileBytes(Known, Pair.Value.Num(), TrianglesPerRoad) - (Known ? TileGpuBytes(*Known) : 0);
			NextTileBytes = Change;
			if (Held > 0 && Held + Change > Budget) break;
			Held += Change;
		}
		Written.Add(Pair.Key);
		Batch.Append(Pair.Value);
	}
	Batch.Sort();
	// What the batch draws, the roads at its roads' nodes (their lanes meet the batch's connectors) and
	// two rings more: in a sub-graph of those, the batch and its neighbours come out as in the whole
	// network. The detection's sub-graph serves when it holds them all.
	FCSRoadNetworkActor_Subgraph Drawing;
	FCSRoadNetworkActor_Subgraph* Graphed = &Detected;
	if (Batch.Num() > 0)
	{
		const TArray<int32> Needed = bEverything ? Everything : Network.Around(Batch, {}, {}, 3);
		if (!Detected.Holds(Needed))
		{
			if (!CSRoadNetworkActor_BuildSubgraph(Assets, Specs, Nodes, bLeftHand, Terrain, Needed, Drawing, OutError)) return Fail();
			Graphed = &Drawing;
		}
	}
	const CSRoad::FGraph& Graph = Graphed->Graph;
	const double GraphMs = Lap();
	CSRoad::FRoadLanes Lanes;
	if (Batch.Num() > 0 && (bMarkings || bEverything)) Lanes.Build(Graph, Assets, Graphed->Result);
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
	for (const FIntPoint& Key : Written)
	{
		FWork& Item = Work.AddDefaulted_GetRef();
		Item.Key = Key;
		for (const int32 Road : Dirty.FindChecked(Key)) Item.Roads.Add(Graphed->RoadOfSpec[Road]);
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
	CSRoad::FGpuRoadMeshInput Counted;
	if (All.Num() > 0 && (!Counted.BuildSubset(Graph, Assets, All, {}, &TileState->Compositions, OutError) || !CSRoad::RunRefineCountsBlocking(Graphed->Input, Counted, OutError, nullptr, &Graphed->Shared))) return Fail();
	if (Counted.NumUnconverged > 0) UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %u node mesh triangles did not converge in 20 refinement passes"), *GetName(), Counted.NumUnconverged);
	const double CountMs = Lap();

	// Each tile: the roads in it drawn, and their decals. A tile whose GPU mesh, its size now known, does
	// not fit the budget while another GPU mesh holds some stops the batch: it and the rest stay pending.
	int32 Done = 0;
	for (const FWork& Item : Work)
	{
		CSRoad::FGpuRoadMeshInput MeshInput;
		if (Item.Drawn.Num() > 0 && (!MeshInput.BuildSubset(Graph, Assets, Item.Drawn, Item.Context, &TileState->Compositions, OutError) || !MeshInput.ApplyRefinementFrom(Counted, OutError))) return Fail();
		if (MeshInput.NumDrawnIndices == 0)
		{
			RemoveTile(Item.Key);
			Done++;
			continue;
		}
		if (Budget < MAX_int64)
		{
			const FCSRoadTile* Known = Tiles.Find(Item.Key);
			const int64 Others = GpuMeshBytes() - (Known ? TileGpuBytes(*Known) : 0);
			NextTileBytes = CSRoadNetworkActor_GpuMeshBytes(MeshInput.NumVertices, MeshInput.NumIndices);
			if (Others > 0 && Others + NextTileBytes > Budget) break;
		}
		TBitArray<> Keep(false, Graph.Roads.Num());
		for (const int32 Road : Item.Roads) Keep[Road] = true;
		CSRoad::FDecalScope Scope;
		Scope.Lanes = &Lanes;
		Scope.Keep = &Keep;
		CSRoad::FGpuDecalInput Decals;
		Decals.bFitted = true;
		if (bMarkings && !Decals.Build(Graph, Assets, Graphed->Result, MeshInput, ResolveAssetDirectory(), OutError, &Scope)) return Fail();
		FCSRoadTile& Tile = EnsureTile(Item.Key);
		FMeshTargets Targets;
		Targets.Road = Tile.Road;
		Targets.RoadMesh = &Tile.RoadMesh;
		if (bMarkings)
		{
			ResolveDecalMaterials(Decals, ByBaseColor, Assets);
			Targets.Decals = &Decals;
		}
		if (!WriteMeshes(Graphed->Input, Graphed->Shared, MeshInput, ByBaseColor, Assets, Targets, OutError)) return Fail();
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
	Written.SetNum(Done);
	Work.SetNum(Done);
	TileState->Commit(Network, Affected, Outputs, Signature, Dirty, Written);
	const double TileMs = Lap();

	NumRoads = Specs.Num();
	NumTiles = Tiles.Num();
	TilesRebuilt = Work.Num();
	RoadsRebuilt = Batch.Num();
	RoadsGraphed = Detected.Graph.Roads.Num() + (Graphed == &Drawing ? Drawing.Graph.Roads.Num() : 0);
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
	if (bEverything && Batch.Num() > 0)
	{
		ClearPreview();
		BuildPreview(Graph, Lanes, Graphed->Result, Terrain);
		bPreviewStale = false;
	}
	else bPreviewStale = true;
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads in %d tiles, %d tiles (%d roads) rebuilt, %d pending; %d roads graphed (%d affected); detection %.0f ms, drawing graph %.0f ms, lanes %.0f ms, refinement counts %.0f ms, tile meshes %.0f ms, preview %.0f ms"),
		*GetName(), NumRoads, NumTiles, TilesRebuilt, RoadsRebuilt, TileState->NumPending(), RoadsGraphed, Affected.Num(), DetectMs, GraphMs, LanesMs, CountMs, TileMs, Lap());
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
		if (Decals)
		{
			CSRoad::FGpuRoadStreams Road;
			Road.Positions = Context.Positions();
			Road.Tangents = Context.Tangents();
			Road.Indices = Context.Indices();
			Road.WorldToLocal = WorldToLocal;
			const CSRoad::FGpuDecalReceivers Receivers = CSRoad::AddDecalReceiverPasses(GraphBuilder, MeshInput, *Decals, Road);
			Fit->Enqueue(GraphBuilder, CSRoad::AddDecalFitPasses(GraphBuilder, MeshInput, *Decals, Road, Receivers), Decals->NumFitHeights);
		}
		bSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots);
		bRecorded = true;
	});
	if (!bRecorded)
	{
		OutError = TEXT("The road mesh has no vertex streams");
		return false;
	}
	if (bSorted) UCSMeshOps::PublishMaterialSections(RoadObject);
	Targets.Fit = Fit;
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
			const FName ComponentName = MakeUniqueObjectName(this, UInstancedStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("%s_%d"), *Name, Used));
			UInstancedStaticMeshComponent* Component = NewObject<UInstancedStaticMeshComponent>(this, ComponentName, RF_Transient | RF_DuplicateTransient);
			Component->SetupAttachment(RootComponent);
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
	if (MarkingFits.Num() > 0 && TickDecalFits(MarkingFits, MarkingDecals, TEXT("Marking"), Instances)) NumMarkingInstances = Instances;
	if (!bTiles) return;
	NumMarkingInstances = 0;
	for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) NumMarkingInstances += Pair.Value.DecalInstances;
}

void ACSRoadNetworkActor::FinishDecalFits()
{
	auto Pending = [this]()
	{
		if (MarkingFits.Num() > 0) return true;
		for (const TPair<FIntPoint, FCSRoadTile>& Pair : Tiles) if (Pair.Value.DecalFits.Num() > 0) return true;
		return false;
	};
	// Each round: the GPU done with every copy, a look at each, then what landed drawn.
	for (int32 Round = 0; Round < 100 && Pending(); Round++)
	{
		ENQUEUE_RENDER_COMMAND(CSRoadFinishDecalFits)([](FRHICommandListImmediate& RHICmdList) { RHICmdList.BlockUntilGPUIdle(); });
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
	if (bLocalUpdate && bBakeNanite)
	{
		const double Start = FPlatformTime::Seconds();
		// Game-thread time a tick for starting bakes, handing them to the compiler and swapping meshes.
		const double Deadline = Start + 0.002;
		for (TPair<FIntPoint, FCSRoadTile>& Pair : Tiles)
		{
			FCSRoadTile& Tile = Pair.Value;
			if (!Tile.RoadMesh || !Tile.Road || Tile.bBakeFailed) continue;
			if (!Tile.Bake)
			{
				// Once the mesh has stood a moment: an edit dragged across a tile rewrites it every tick.
				if (Start - Tile.WrittenSeconds < 0.3 || FPlatformTime::Seconds() > Deadline) continue;
				TArray<UMaterialInterface*> Materials;
				for (const TObjectPtr<UMaterialInterface>& Material : Tile.RoadMesh->Materials) Materials.Add(Material.Get());
				const double StartStart = FPlatformTime::Seconds();
				Tile.Baking = CSRoad::NewNaniteMesh(this, Materials);
				Tile.Bake = MakeShared<CSRoad::FRoadBake>();
				if (!Tile.Bake->Start(*Tile.RoadMesh, uint32(Tile.Vertices), uint32(Tile.Triangles) * 3u, Tile.Baking))
				{
					UE_LOG(LogCSRoadNet, Warning, TEXT("%s: tile (%d, %d) has no road mesh streams to bake"), *GetName(), Pair.Key.X, Pair.Key.Y);
					Tile.Bake.Reset();
					Tile.Baking = nullptr;
					Tile.bBakeFailed = true;
				}
				if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - StartStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) bake start took %.1f ms"), *GetName(), Pair.Key.X, Pair.Key.Y, (FPlatformTime::Seconds() - StartStart) * 1000.0);
				continue;
			}
			const double StepStart = FPlatformTime::Seconds();
			const CSRoad::FRoadBake::EStage Before = Tile.Bake->Stage();
			const CSRoad::FRoadBake::EStage Stage = Tile.Bake->Tick(StepStart < Deadline);
			if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - StepStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) bake step %d -> %d took %.1f ms"), *GetName(), Pair.Key.X, Pair.Key.Y, int32(Before), int32(Stage), (FPlatformTime::Seconds() - StepStart) * 1000.0);
			if (Stage == CSRoad::FRoadBake::EStage::Failed)
			{
				UE_LOG(LogCSRoadNet, Warning, TEXT("%s: tile (%d, %d) could not be baked into a Nanite mesh; it keeps its GPU mesh"), *GetName(), Pair.Key.X, Pair.Key.Y);
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
				const FName Name = MakeUniqueObjectName(this, UStaticMeshComponent::StaticClass(), *FString::Printf(TEXT("NaniteTile_%d_%d"), Pair.Key.X, Pair.Key.Y));
				Tile.Nanite = NewObject<UStaticMeshComponent>(this, Name, RF_Transient | RF_DuplicateTransient);
				Tile.Nanite->SetupAttachment(RootComponent);
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
			if (CVarCSRoadBakeLog.GetValueOnGameThread() != 0 && FPlatformTime::Seconds() - SwapStart > 0.002) UE_LOG(LogCSRoadNet, Log, TEXT("%s: tile (%d, %d) swap took %.1f ms"), *GetName(), Pair.Key.X, Pair.Key.Y, (FPlatformTime::Seconds() - SwapStart) * 1000.0);
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
		const FName Name = MakeUniqueObjectName(this, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("RoadTile_%d_%d"), Key.X, Key.Y));
		Tile.Road = NewObject<UCSMeshRenderComponent>(this, Name, RF_Transient | RF_DuplicateTransient);
		Tile.Road->SetupAttachment(RootComponent);
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

void ACSRoadNetworkActor::BuildBatchBuildings(const TArray<CSRoad::FSplineRoad>& Splines, const TArray<CSRoad::FRoadSpec>& Roads, CSRoad::FAssets& Assets)
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
	Settings.Seed = Lots.Seed;
	UMaterialInterface* Surface = BuildingMaterial ? BuildingMaterial.Get() : ACSCityBuildingActor::DefaultMaterial();
	if (!LotState) LotState = MakeUnique<CSCity::FLotState>();
	CSCity::FLotState& State = *LotState;

	// Whatever else the plots and their meshes are made from: when it changes, everything is laid out
	// again.
	FXxHash64Builder SignatureHash;
	auto Sign = [&SignatureHash](const auto& Value) { SignatureHash.Update(&Value, sizeof(Value)); };
	for (const double Value : { Settings.Frontage, Settings.Gap, Settings.Setback, Settings.MinDepth, Settings.MaxDepth, Settings.MinHeight, Settings.MaxHeight, TileCm }) Sign(Value);
	Sign(Settings.Seed);
	Sign(Source);
	Sign(Surface);
	const FMatrix Placement = NetworkToWorld.ToMatrixWithScale();
	Sign(Placement);
	const uint64 Signature = SignatureHash.Finalize().Hash;
	const bool bFull = !State.bValid || State.Signature != Signature;

	auto HalfWidth = [&Assets](const FString& Prefab) { return double(Assets.Geometry(Prefab).m_DefaultWidth) * 50.0; };
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
		uint64 LineId = 0;
		int32 Plot = INDEX_NONE;
	};
	TArray<FInput> Inputs;
	TArray<CSCity::FPolygon> Plots;
	auto AddCurve = [&](EInput Kind, uint64 LineId, double Half, const FVector3d& A, const FVector3d& B, const FVector3d& C, const FVector3d& D)
	{
		FInput& Input = Inputs.AddDefaulted_GetRef();
		Input.Kind = Kind;
		Input.LineId = LineId;
		Input.HalfWidth = Half;
		Input.Curve[0] = A;
		Input.Curve[1] = B;
		Input.Curve[2] = C;
		Input.Curve[3] = D;
		FXxHash64Builder Hash;
		Hash.Update(&Kind, sizeof(Kind));
		Hash.Update(&LineId, sizeof(LineId));
		Hash.Update(&Half, sizeof(Half));
		Hash.Update(Input.Curve, sizeof(Input.Curve));
		Input.Key = Hash.Finalize().Hash;
		// A cubic Bezier lies within its control points' box.
		FBox2D Box(ForceInit);
		for (const FVector3d& Control : Input.Curve) Box += FVector2D(NetworkToWorld.TransformPosition(CSRoadNetworkActor_ToUnreal(Control)));
		Input.Tiles = TilesOf(Box, Kind == EInput::Anchor ? CSCity::AnchorReach(Settings, Half) : CSCity::RoadReach(Settings, Half));
	};
	// The roads as built, which the plots keep clear of; building from a snapshot the plots face them
	// too, their slots named by the road's prefab and curve (and how many such roads came before). A road
	// is told apart by its content alone: an edge that only moved in the list is the same road.
	const bool bSplines = Source == ECSRoadNetworkSource::Splines;
	TMap<uint64, uint64> Repeats;
	for (int32 I = 0; I < Roads.Num(); I++)
	{
		const CSRoad::FRoadSpec& Road = Roads[I];
		const double Half = HalfWidth(Road.Prefab);
		const FVector3d A = Point(Road.Curve.a);
		const FVector3d B = Point(Road.Curve.b);
		const FVector3d C = Point(Road.Curve.c);
		const FVector3d D = Point(Road.Curve.d);
		AddCurve(EInput::Road, 0, Half, A, B, C, D);
		if (bSplines) continue;
		FXxHash64Builder Content;
		Content.Update(*Road.Prefab, Road.Prefab.Len() * sizeof(TCHAR));
		Content.Update(&Road.Curve, sizeof(Road.Curve));
		const uint64 RoadId = Content.Finalize().Hash;
		AddCurve(EInput::Anchor, Combine(RoadId, Repeats.FindOrAdd(RoadId)++), Half, A, B, C, D);
	}
	// Building from splines the plots face their segments as drawn, not the edges cut from them: a road
	// joining the spline, or an edit of another segment, leaves the slots where they were.
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
			// A USplineComponent segment's Hermite tangents as Bezier handles.
			AddCurve(EInput::Anchor, Combine(Spline.Id, uint64(I)), Half, A, bLinear ? FMath::Lerp(A, D, 1.0 / 3.0) : A + Spline.Leave[I] / 3.0, bLinear ? FMath::Lerp(A, D, 2.0 / 3.0) : D - Spline.Arrive[J] / 3.0, D);
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
	// batches. A batch of them is laid out at most, the fresh ones first; the rest stay pending.
	TMap<uint64, FIntRect> Reach;
	Reach.Reserve(Inputs.Num());
	for (const FInput& Input : Inputs) Reach.Add(Input.Key, Input.Tiles);
	TSet<FIntPoint> Dirty;
	TArray<FIntPoint> Deferred;
	if (!bFull)
	{
		TSet<FIntPoint> Fresh;
		auto Mark = [&Fresh](const FIntRect& Tiles) { for (int32 Y = Tiles.Min.Y; Y <= Tiles.Max.Y; Y++) for (int32 X = Tiles.Min.X; X <= Tiles.Max.X; X++) Fresh.Add(FIntPoint(X, Y)); };
		for (const TPair<uint64, FIntRect>& Pair : Reach) if (!State.Reach.Contains(Pair.Key)) Mark(Pair.Value);
		for (const TPair<uint64, FIntRect>& Pair : State.Reach) if (!Reach.Contains(Pair.Key)) Mark(Pair.Value);
		TArray<FIntPoint> Order = Fresh.Array();
		for (const FIntPoint& Tile : State.Pending) if (!Fresh.Contains(Tile)) Order.Add(Tile);
		Order.Sort([&Fresh](const FIntPoint& A, const FIntPoint& B)
		{
			const bool bFreshA = Fresh.Contains(A);
			if (bFreshA != Fresh.Contains(B)) return bFreshA;
			return A.Y != B.Y ? A.Y < B.Y : A.X < B.X;
		});
		for (int32 I = 0; I < Order.Num(); I++)
		{
			if (MaxTilesPerBatch > 0 && I >= MaxTilesPerBatch) Deferred.Add(Order[I]);
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
		Line.HalfWidth = Input.HalfWidth;
		Sample(Input.Curve, Line.Points);
	}
	const TArray<CSCity::FLot> Laid = CSCity::LayoutLots(Anchors, Lines, Reserved, Settings);
	TMap<FIntPoint, TArray<const CSCity::FLot*>> ByTile;
	for (const CSCity::FLot& Lot : Laid)
	{
		const FIntPoint Tile = TileOf(Lot.Centre);
		if (bFull || Dirty.Contains(Tile)) ByTile.FindOrAdd(Tile).Add(&Lot);
	}

	// The tiles to write or remove, in row order: the dirty ones, or every one there is or was. A
	// batch of them is written at most; the others stay pending and are laid out again later.
	TArray<FIntPoint> Visit;
	if (bFull)
	{
		ByTile.GetKeys(Visit);
		for (const TPair<FIntPoint, FCSCityBuildingTile>& Pair : BuildingTiles) if (!ByTile.Contains(Pair.Key)) Visit.Add(Pair.Key);
	}
	else Visit = Dirty.Array();
	Visit.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
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
			const FName Name = MakeUniqueObjectName(this, UCSMeshRenderComponent::StaticClass(), *FString::Printf(TEXT("BuildingTile_%d_%d"), Key.X, Key.Y));
			Tile.Component = NewObject<UCSMeshRenderComponent>(this, Name, RF_Transient | RF_DuplicateTransient);
			Tile.Component->SetupAttachment(RootComponent);
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

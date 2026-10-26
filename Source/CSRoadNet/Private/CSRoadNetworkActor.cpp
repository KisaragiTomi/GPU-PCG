#include "CSRoadNetworkActor.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadGpuMesh.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "Components/SplineComponent.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
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
	if (RoadMesh && RoadMesh->HasGeneratedGeometry()) return;
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
	if (!bRebuildRequested) return;
	bRebuildRequested = false;
	Rebuild();
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
		Out.Prefab = Road ? Road->Prefab : DefaultSplinePrefab;
		Out.Upgrade = Road ? CSRoadNetworkActor_Flags(Road->Upgrade) : CSRoad::CompositionFlags{};
		Out.bAdjust = Road ? Road->bAdjustSegments : true;
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
	// What spline roads asked for until now is in this build.
	bRebuildRequested = false;
	FString Error;
	if (BuildRoads(Error)) LastError.Reset();
	else
	{
		LastError = Error;
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: %s"), *GetName(), *Error);
	}
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
	NumSplines = 0;
	if (bSplines)
	{
		const TArray<CSRoad::FSplineRoad> Splines = GatherSplines();
		NumSplines = Splines.Num();
		CSRoad::FSplineNetworkSettings Settings;
		Settings.MaxEdgeLength = SplineSettings.MaxSegmentLength;
		Settings.MaxEdgeTurn = FMath::DegreesToRadians(static_cast<double>(SplineSettings.MaxSegmentTurn));
		Settings.MinEdgeLength = SplineSettings.MinSegmentLength;
		Settings.SnapDistance = SplineSettings.SnapDistance;
		Settings.JunctionHeight = SplineSettings.JunctionHeight;
		CSRoad::FSplineNetworkStats Stats;
		if (!CSRoad::BuildSplineNetwork(Library->Assets, Splines, Settings, Terrain, FromSplines.Nodes, FromSplines.Roads, Stats, OutError)) return false;
		UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d splines make %d nodes and %d edges; %d junctions joined, %d segments split, %d merged, %d too short left out"),
			*GetName(), NumSplines, FromSplines.Nodes.Num(), FromSplines.Roads.Num(), Stats.Junctions, Stats.Splits, Stats.Merges, Stats.Dropped);
	}
	const TArray<CSRoad::FSavedNode>& Nodes = bSplines ? FromSplines.Nodes : Snapshot->Nodes;
	const TArray<CSRoad::FRoadSpec>& Specs = bSplines ? FromSplines.Roads : Snapshot->Roads;
	const double NetworkMs = Lap();
	CSRoad::FGraph Graph;
	Graph.bLeftHandTraffic = bSplines ? bLeftHandTraffic : Snapshot->bLeftHandTraffic;
	CSRoad::FGpuGeometryInput Geometry;
	CSRoad::FGpuRoadMeshInput MeshInput;
	if (!Graph.Build(Library->Assets, Specs, Nodes, OutError) || !Geometry.Build(Graph, Library->Assets, Terrain, OutError)) return false;
	const double GraphMs = Lap();
	if (!MeshInput.Build(Graph, Library->Assets, OutError)) return false;
	const double MeshMs = Lap();
	// First graph: the refined node meshes' size is decided on the GPU, and the lane graph and the
	// decal setup run on the CPU from the GPU geometry.
	const bool bMarkings = bBuildMarkings && MarkingMesh;
	CSRoad::FGpuGeometryResult GeometryResult;
	if (!CSRoad::RunRefineCountsBlocking(Geometry, MeshInput, OutError, bMarkings ? &GeometryResult : nullptr)) return false;
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

	// One material slot per atlas; MeshTriangleMaterials indexes these.
	const TMap<FString, UMaterialInterface*> ByBaseColor = CSRoadNetworkActor_Materials(MaterialFolder.Path);
	TArray<TObjectPtr<UMaterialInterface>> Materials;
	MissingAtlases.Reset();
	for (const FString& Atlas : MeshInput.MaterialSlots)
	{
		UMaterialInterface* const* Found = ByBaseColor.Find(Library->Assets.AtlasBaseColor(Atlas));
		if (!Found) MissingAtlases.Add(Atlas);
		Materials.Add(Found ? *Found : FallbackMaterial.Get());
	}

	UObject* MeshOuter = RoadMesh;
	if (!Mesh || Mesh->GetOuter() != MeshOuter) Mesh = NewObject<UCSMesh>(MeshOuter);
	Mesh->Materials = Materials;
	Mesh->NotifyMaterialsChanged();
	RoadMesh->MeshMaterial = Materials.Num() > 0 ? Materials[0].Get() : FallbackMaterial.Get();
	const int32 NumSlots = FMath::Max(1, Materials.Num());
	if (!Mesh->EnsureCapacitySync(int32(MeshInput.NumVertices), int32(MeshInput.NumIndices)))
	{
		OutError = FString::Printf(TEXT("Could not allocate %u vertices / %u indices"), MeshInput.NumVertices, MeshInput.NumIndices);
		return false;
	}
	Mesh->EnsureIndirectDrawCapacitySync(NumSlots);
	RoadMesh->SetGpuMesh(Mesh);
	const double AllocationMs = Lap();

	// Second graph: the road surface, and the decals' receivers gathered from it. The decals refer to
	// road triangles by their place in the index stream, so the road's material sort (which reorders
	// it) waits until they have been drawn.
	const FTransform ActorToWorld = GetActorTransform();
	const FMatrix44f LocalToWorld(ActorToWorld.ToMatrixWithScale());
	const FMatrix44f WorldToLocal(ActorToWorld.ToInverseMatrixWithScale());
	const FBox WorldBounds = MeshInput.Bounds.TransformBy(ActorToWorld);
	const bool bDecals = bMarkings && Decals.Items.Num() > 0;
	CSRoad::FPooledDecalReceivers PooledReceivers;
	TUniquePtr<FRHIGPUBufferReadback> ClipReadback;
	if (bDecals && Decals.ClippedItems.Num() > 0) ClipReadback = MakeUnique<FRHIGPUBufferReadback>(TEXT("CSRoad.ClipCountsReadback"));
	FRHIGPUBufferReadback* ClipReadbackPtr = ClipReadback.Get();
	const uint32 ClipBytes = uint32(Decals.ClippedItems.Num() * sizeof(uint32));
	bool bSorted = false;
	bool bRecorded = false;
	Mesh->EditMeshSync([&](FCSMeshEditContext& Context)
	{
		if (!Context.Positions() || !Context.Tangents() || !Context.TexCoords() || !Context.Colors() || !Context.Indices() || !Context.MaterialIds()) return;
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;
		const CSRoad::FGpuGeometryOutput GeometryOut = CSRoad::AddGeometryPasses(GraphBuilder, Geometry);
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
		UCSMeshOps::AddSetCountersPass(Context, MeshInput.NumVertices, MeshInput.NumIndices);
		Context.SetWorldBounds(WorldBounds);
		if (bDecals)
		{
			CSRoad::FGpuRoadStreams Road;
			Road.Positions = Context.Positions();
			Road.Tangents = Context.Tangents();
			Road.Indices = Context.Indices();
			Road.WorldToLocal = WorldToLocal;
			const CSRoad::FGpuDecalReceivers Receivers = CSRoad::AddDecalReceiverPasses(GraphBuilder, MeshInput, Decals, Road);
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
	if (bSorted) UCSMeshOps::PublishMaterialSections(Mesh);
	const double RoadMs = Lap();

	double MarkingMs = 0.0;
	NumMarkingTriangles = 0;
	MissingMarkingSurfaces.Reset();
	// Decals that cannot be built (say, too little VRAM for their mesh) leave the roads standing; the
	// error is still returned.
	FString MarkingError;
	bool bMarked = false;
	if (bDecals)
	{
		bMarked = BuildMarkingMesh(Decals, MeshInput, ByBaseColor, Library->Assets, PooledReceivers, ClipReadbackPtr, LocalToWorld, WorldToLocal, WorldBounds, MarkingError);
		// Fourth graph: the road's material sections, now that the decals have read its triangles.
		Mesh->EditMeshSync([&](FCSMeshEditContext& Context) { bSorted = UCSMeshOps::AddMaterialSectionPasses(Context, NumSlots); });
		if (bSorted) UCSMeshOps::PublishMaterialSections(Mesh);
		MarkingMs = Lap();
	}
	if (MarkingMesh) MarkingMesh->SetVisibility(bMarked);
	UE_LOG(LogCSRoadNet, Log, TEXT("%s: %d roads, %u vertices, %u triangles, %d compositions, %d decals (%d lanes); prefab data %.0f ms, snapshot %.0f ms, graph and compositions %.0f ms, composition meshes %.0f ms, refinement counts %.0f ms, lanes and decal setup %.0f ms, allocation %.0f ms, road GPU build %.0f ms, decals %.0f ms"),
		*GetName(), Graph.Roads.Num(), MeshInput.NumVertices, MeshInput.NumIndices / 3, Graph.Compositions.Num() - 1, Decals.Items.Num(), Decals.Lanes.Lanes.Num(),
		LibraryMs, NetworkMs, GraphMs, MeshMs, RefineMs, DecalSetupMs, AllocationMs, RoadMs, MarkingMs);

	NumRoads = Graph.Roads.Num();
	NumVertices = int32(MeshInput.NumVertices);
	NumTriangles = int32(MeshInput.NumIndices / 3);
	if (!bDecals || bMarked) return true;
	OutError = MarkingError;
	return false;
}

bool ACSRoadNetworkActor::BuildMarkingMesh(CSRoad::FGpuDecalInput& Decals, const CSRoad::FGpuRoadMeshInput& MeshInput, const TMap<FString, UMaterialInterface*>& ByBaseColor,
	const CSRoad::FAssets& Assets, const CSRoad::FPooledDecalReceivers& Receivers, FRHIGPUBufferReadback* ClipReadback,
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
	if (Decals.NumVertices == 0 || Decals.NumIndices == 0) return true;

	TArray<TObjectPtr<UMaterialInterface>> Materials;
	for (const FString& Surface : Decals.MaterialSlots)
	{
		UMaterialInterface* const* Found = ByBaseColor.Find(Assets.SurfaceBaseColor(Surface));
		if (!Found) MissingMarkingSurfaces.Add(Surface);
		Materials.Add(Found ? *Found : FallbackMaterial.Get());
	}
	if (!Markings || Markings->GetOuter() != MarkingMesh) Markings = NewObject<UCSMesh>(MarkingMesh);
	// UV0 the decal texture, UV1 the distances to both ends, UV2 / UV3 the colour (CSRoadDecal.usf).
	if (!UCSMeshOps::EnsureTexCoordSets(Markings, 4))
	{
		OutError = TEXT("The decal mesh could not take four UV sets");
		return false;
	}
	Markings->Materials = Materials;
	Markings->NotifyMaterialsChanged();
	MarkingMesh->MeshMaterial = Materials.Num() > 0 ? Materials[0].Get() : FallbackMaterial.Get();
	const int32 NumSlots = FMath::Max(1, Materials.Num());
	if (!Markings->EnsureCapacitySync(int32(Decals.NumVertices), int32(Decals.NumIndices)))
	{
		OutError = FString::Printf(TEXT("Could not allocate %u decal vertices / %u indices"), Decals.NumVertices, Decals.NumIndices);
		return false;
	}
	Markings->EnsureIndirectDrawCapacitySync(NumSlots);
	MarkingMesh->SetGpuMesh(Markings);

	// Third graph: the decals, reading the road mesh's streams.
	FCSMeshResident* RoadResident = Mesh->GetResidentPtr();
	bool bSorted = false;
	bool bRecorded = false;
	Markings->EditMeshSync([&](FCSMeshEditContext& Context)
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
	if (bSorted) UCSMeshOps::PublishMaterialSections(Markings);
	NumMarkingTriangles = int32(Decals.NumIndices / 3);
	return true;
}

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
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGPUReadback.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"

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
	PrimaryActorTick.bCanEverTick = false;
	RoadMesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("RoadMesh"));
	RootComponent = RoadMesh;
	MarkingMesh = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("MarkingMesh"));
	MarkingMesh->SetupAttachment(RoadMesh);
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
	if (!bBuildOnLoad || IsTemplate() || !GetWorld() || NetworkFile.FilePath.IsEmpty()) return;
	if (RoadMesh && RoadMesh->HasGeneratedGeometry()) return;
	Rebuild();
}

#if WITH_EDITOR
void ACSRoadNetworkActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	if (!NetworkFile.FilePath.IsEmpty()) Rebuild();
}

void ACSRoadNetworkActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// The resident streams are world space, so the geometry follows the actor only by rebuilding.
	if (bFinished && !NetworkFile.FilePath.IsEmpty()) Rebuild();
}
#endif

void ACSRoadNetworkActor::Rebuild()
{
	const double StartTime = FPlatformTime::Seconds();
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
	CSRoad::FSavedNetwork Network;
	if (!Network.Load(NetworkFile.FilePath, &Library->Assets, bUseTerrain, OutError)) return false;
	const double NetworkMs = Lap();
	CSRoad::FGraph Graph;
	Graph.bLeftHandTraffic = Network.bLeftHandTraffic;
	CSRoad::FGpuGeometryInput Geometry;
	CSRoad::FGpuRoadMeshInput MeshInput;
	const CSRoad::FSavedTerrain* Terrain = bUseTerrain && Network.Terrain.IsValid() ? &Network.Terrain : nullptr;
	if (!Graph.Build(Library->Assets, Network.Roads, Network.Nodes, OutError) || !Geometry.Build(Graph, Library->Assets, Terrain, OutError)) return false;
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

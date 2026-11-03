#include "CSRoadMeshWrite.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "Engine/World.h"

namespace
{
// Writes queued whose OnLanded has not run yet (game thread).
int32 GCSRoadMeshWrites = 0;

// Run now, unless the world is in its end-of-frame component update, where render state may not change (a flush
// there runs game-thread tasks, a write's landing among them): then at the core ticker's next tick.
void CSRoadMeshWrite_AtSafePoint(const TWeakObjectPtr<UWorld>& World, TFunction<void()> Run)
{
	const UWorld* In = World.Get();
	if (!In || !In->bPostTickComponentUpdate)
	{
		Run();
		return;
	}
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run = MoveTemp(Run)](float)
	{
		Run();
		return false;
	}));
}
}

namespace CSRoad
{
EMeshWrite WriteMeshAsync(UCSMeshRenderComponent& Component, TObjectPtr<UCSMesh>& Mesh, const TArray<TObjectPtr<UMaterialInterface>>& Materials,
	uint32 Vertices, uint32 Indices, int32 NumSlots, TFunction<void(FCSMeshEditContext&)> Edit, TFunction<void(UCSMesh*)> OnLanded)
{
	UCSMesh* Target = Mesh && Mesh->GetOuter() == &Component ? Mesh.Get() : nullptr;
	if (Target && Target->IsEditInFlight()) return EMeshWrite::Busy;
	const int32 Slots = FMath::Max(NumSlots, 1);
	const FCSMeshResident* Resident = Target ? Target->GetResidentPtr() : nullptr;
	const bool bInPlace = Resident && Resident->IsAllocated() && Resident->VertexCapacity >= FMath::Max(Vertices, 3u) && Resident->IndexCapacity >= FMath::Max(Indices, 3u)
		&& Resident->NumIndirectDraws >= uint32(Slots);

	GCSRoadMeshWrites++;
	auto Land = [World = TWeakObjectPtr<UWorld>(Component.GetWorld()), OnLanded = MoveTemp(OnLanded)](UCSMesh* Written)
	{
		CSRoadMeshWrite_AtSafePoint(World, [Written = TWeakObjectPtr<UCSMesh>(Written), OnLanded]()
		{
			GCSRoadMeshWrites--;
			if (OnLanded) OnLanded(Written.Get());
		});
	};
	if (bInPlace)
	{
		Target->Materials = Materials;
		Target->NotifyMaterialsChanged();
		if (Target->EditMeshAsync(MoveTemp(Edit), [Land, Written = TWeakObjectPtr<UCSMesh>(Target)](bool bBuilt) { Land(bBuilt ? Written.Get() : nullptr); })) return EMeshWrite::Queued;
		GCSRoadMeshWrites--;
		return EMeshWrite::Busy;
	}
	// A mesh of its own until it has landed: the render thread allocates it with the edit, no flush.
	UCSMesh* Fresh = NewObject<UCSMesh>(&Component);
	Fresh->Materials = Materials;
	// Not allocated yet: this only declares the arg sets the allocation makes.
	Fresh->EnsureIndirectDrawCapacitySync(Slots);
	const int32 VertexCapacity = int32(FMath::Min<uint32>(Vertices, uint32(MAX_int32)));
	const int32 IndexCapacity = int32(FMath::Min<uint32>(Indices, uint32(MAX_int32)));
	if (!Fresh->AllocateAndEditAsync(VertexCapacity, IndexCapacity, MoveTemp(Edit), [Land, Written = TWeakObjectPtr<UCSMesh>(Fresh)](bool bBuilt) { Land(bBuilt ? Written.Get() : nullptr); }))
	{
		GCSRoadMeshWrites--;
		return EMeshWrite::Refused;
	}
	Mesh = Fresh;
	return EMeshWrite::Queued;
}

void ShowLatest(UCSMeshRenderComponent* Component, UCSMesh* Latest, UCSMesh* Landed)
{
	if (!IsValid(Component))
	{
		if (Landed) Landed->ReleaseDeferred();
		return;
	}
	UCSMesh* Shown = Component->GetGpuMesh();
	const FCSMeshResident* Resident = Latest ? Latest->GetResidentPtr() : nullptr;
	if (Latest && Latest != Shown && Latest->GetOuter() == Component && !Latest->IsEditInFlight() && Resident && Resident->IsAllocated())
	{
		Component->SetGpuMesh(Latest);
		if (Shown && Shown->GetOuter() == Component) Shown->ReleaseDeferred();
		Shown = Latest;
	}
	if (Landed && Landed != Shown && Landed != Latest && Landed->GetOuter() == Component) Landed->ReleaseDeferred();
}

int32 MeshWritesInFlight()
{
	return GCSRoadMeshWrites;
}

void LandMeshWrites()
{
	// Each round: the render thread done with the writes, then their landings on the game thread (a flush runs
	// game-thread tasks only while it waits).
	for (int32 Round = 0; Round < 100 && GCSRoadMeshWrites > 0; Round++)
	{
		UCSMesh::CountedBlockingFlush();
		if (!FTaskGraphInterface::Get().IsThreadProcessingTasks(ENamedThreads::GameThread)) FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}
}
}

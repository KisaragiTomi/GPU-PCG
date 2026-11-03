// Writing the GPU mesh a component draws without waiting for the render thread. A mesh with the room
// is edited in place (UCSMesh::EditMeshAsync); one without, or none, is written into a new mesh that the
// render thread allocates with the edit (UCSMesh::AllocateAndEditAsync) and that takes the old one's
// place once it has landed: until then the old one stays drawn, and no flush is paid for the room.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
struct FCSMeshEditContext;

namespace CSRoad
{
enum class EMeshWrite : uint8
{
	// On its way: OnLanded runs on the game thread a frame or two later.
	Queued,
	// The mesh's last write is still on its way: nothing done; write again once it has landed.
	Busy,
	// The video memory check refused a new mesh the room: nothing done.
	Refused,
};

// Edit writes the mesh Component draws. Mesh is the latest written (Component draws it once it has landed): edited
// in place when it has room for Vertices and Indices and NumSlots indirect arg sets, else Mesh becomes a new mesh in
// Component, with Materials. OnLanded runs once the edit has run, on the game thread outside the end-of-frame update,
// with the mesh written (null: it went first); it puts the latest mesh on screen (ShowLatest), finding it where the
// caller keeps it: a later write may have replaced this one by then.
EMeshWrite WriteMeshAsync(UCSMeshRenderComponent& Component, TObjectPtr<UCSMesh>& Mesh, const TArray<TObjectPtr<UMaterialInterface>>& Materials,
	uint32 Vertices, uint32 Indices, int32 NumSlots, TFunction<void(FCSMeshEditContext&)> Edit, TFunction<void(UCSMesh*)> OnLanded);

// Component draws Latest (a mesh of its own) once it has landed, and the mesh it drew before is handed back; Landed,
// written and passed over (neither drawn nor the latest), is handed back too. Null Component: Landed handed back.
void ShowLatest(UCSMeshRenderComponent* Component, UCSMesh* Latest, UCSMesh* Landed);

// Writes queued and not yet landed, every mesh (OnLanded run).
int32 MeshWritesInFlight();
// Waits until they have landed (tests, and builds that wait).
void LandMeshWrites();
}

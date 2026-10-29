// A tile's road mesh, built on the GPU, baked into a Nanite static mesh without stalling the GPU or
// the game thread: its streams are copied back as the render thread gets to them, laid into a mesh
// description and committed to the static mesh on a worker, then built by the editor's static mesh
// compiler on its own threads (and cached in the DDC by content). The network actor draws the GPU
// mesh until the static mesh is ready, then draws that and frees the GPU mesh. Editor only: Nanite
// meshes are not built at runtime.
#pragma once

#include "CoreMinimal.h"
#include "Tasks/Task.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

class UCSMesh;
class UMaterialInterface;
class UStaticMesh;

namespace CSRoad
{
class FRoadBake
{
public:
	enum class EStage : uint8 { Copying, Describing, Compiling, Done, Failed };

	~FRoadBake();

	// Game thread: starts copying the first NumVertices / NumIndices of Mesh's streams for Target (a
	// NewNaniteMesh). False when the mesh lacks a stream it needs.
	bool Start(const UCSMesh& Mesh, uint32 NumVertices, uint32 NumIndices, UStaticMesh* Target);
	// Game thread, once a tick: moves the bake on as far as it can without waiting. bMayLaunch: a
	// finished description may hand the static mesh to the compiler (the caller paces those).
	EStage Tick(bool bMayLaunch);
	EStage Stage() const { return Current; }
	// The static mesh's local space in world space, once the copies have landed.
	FVector Origin() const;

private:
	struct FShared;
	TSharedPtr<FShared, ESPMode::ThreadSafe> Shared;
	UE::Tasks::FTask Describe;
	TWeakObjectPtr<UStaticMesh> Target;
	EStage Current = EStage::Failed;
};

// A transient static mesh in Outer to bake a road mesh into: Nanite on, one material slot per
// material id, normals and tangents kept as the GPU wrote them, no lightmap UVs or distance field.
UStaticMesh* NewNaniteMesh(UObject* Outer, const TArray<UMaterialInterface*>& Materials);

// Game thread, before a bake is first drawn: turns r.Nanite.VSMInvalidateOnLODDelta on, unless an ini
// or the console set it. A view caches its virtual shadow map pages as first drawn, from the clusters
// resident then: for a bake just swapped in, or a tile first seen up close, its root clusters, which
// over 256 m are a metre or two off, and the coarse curbs shade the road beside them black. With it
// on, pages drawn from clusters coarser than wanted are drawn again until the finer ones stream in.
void RedrawShadowsOnStreaming();
}

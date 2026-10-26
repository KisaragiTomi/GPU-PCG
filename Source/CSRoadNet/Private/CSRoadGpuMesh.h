// Road surface meshes on the GPU (Shaders/Private/CSRoad/CSRoadMesh.usf): CompositionParameters
// per road, RoadLab's junction seams and node mesh refinement, every composition mesh deformed
// onto the GPU geometry and written into the typed streams of a UCSMesh resident set.
//
// Refinement decides the node meshes' size on the GPU, so a build is two graphs: RunRefineCountsBlocking
// counts and lays the output out (FGpuRoadMeshInput::ApplyRefinement), then AddRoadMeshPasses
// writes it. Both recompute the geometry; the passes are deterministic, so the second graph
// reproduces the counts of the first.
#pragma once

#include "CSRoadCompositionMesh.h"
#include "CSRoadGpuGeometry.h"

namespace CSRoad
{
struct FGpuMeshRange
{
	uint32 VertexOffset = 0;
	uint32 VertexCount = 0;
	uint32 IndexOffset = 0;
	uint32 IndexCount = 0;
	// Node compositions: their base triangles in RefineTriangles.
	uint32 RefineOffset = 0;
	uint32 RefineCount = 0;
};

// One composition mesh drawn with one CompositionParameters entry (road * 3 + edge/start/end).
struct FGpuMeshInstance
{
	uint32 Mesh = 0;
	uint32 Parameters = 0;
	uint32 VertexOffset = 0;
	uint32 IndexOffset = 0;
	// Parameters for node instances, whose deform goes through their junction surface; NoJunction for edges.
	uint32 JunctionSlot = NoJunction;
	// Refined node instances: first of their per-base-triangle items.
	uint32 RefineItemOffset = 0;
	// After ApplyRefinement: the instance's midpoints (after the composition mesh's own vertices) and
	// leaf triangles. Zero counts draw the composition mesh as is.
	uint32 RefinedVertexOffset = 0;
	uint32 RefinedVertexCount = 0;
	uint32 RefinedTriangleCount = 0;

	static constexpr uint32 NoJunction = 0xFFFFFFFFu;
};

// CSRoadRefine.ush FRefineTriangle: composition-local corners; Flags = material slot | flips of the
// edges ab, bc, ca against their defining direction (bits 29, 30, 31).
struct FGpuRefineTriangle
{
	uint32 A = 0;
	uint32 B = 0;
	uint32 C = 0;
	uint32 Flags = 0;
};

// The composition meshes a graph uses (GenerateCompositionMesh, CPU) and the output layout.
struct FGpuRoadMeshInput
{
	TArray<FCompositionVertex> Vertices;
	// Local to the composition mesh's own vertices.
	TArray<uint32> Indices;
	TArray<uint32> TriangleMaterials;
	// Per graph composition index (0 unused).
	TArray<FGpuMeshRange> Meshes;
	// Per road: edge, start node, end node.
	TArray<FGpuMeshInstance> Instances;
	// Road index (FGraph::Roads position) of each entity, -1 for nodes.
	TArray<int32> RoadOfEntity;
	// Base triangles of the node compositions, in Refine's order (material groups, then index order).
	TArray<FGpuRefineTriangle> RefineTriangles;
	// Instances whose node mesh is refined (RoadRenderer.Add refines General.Node compositions).
	TArray<uint32> RefineInstances;
	uint32 NumRefineItems = 0;
	bool bRefinementApplied = false;
	uint32 NumMidpoints = 0;
	// Leaves Refine would still split after its 20 passes (the C# throws); drawn as they are.
	uint32 NumUnconverged = 0;
	// Atlas name of each material slot (MeshTriangleMaterials values).
	TArray<FString> MaterialSlots;
	uint32 NumVertices = 0;
	uint32 NumIndices = 0;
	// Conservative bounds of the output before LocalToWorld (UE space, cm).
	FBox Bounds = FBox(ForceInit);

	// bRefine = false draws the node meshes unrefined (the deform tests compare those).
	bool Build(const FGraph& Graph, FAssets& Assets, FString& OutError, bool bRefine = true);
	bool NeedsRefinementCounts() const { return RefineInstances.Num() > 0 && !bRefinementApplied; }
	// Per refine instance (leaves, midpoints, unconverged, 0) from RunRefineCountsBlocking: sizes the
	// refined instances and lays every instance out again.
	bool ApplyRefinement(const TArray<FUintVector4>& Totals, FString& OutError);

private:
	bool Layout(FString& OutError);
};

// Destination of AddRoadMeshPasses: a UCSMesh's typed streams, or DebugVertices for the tests.
struct FGpuRoadMeshTarget
{
	FRDGBufferRef Positions = nullptr;
	FRDGBufferRef Tangents = nullptr;
	FRDGBufferRef TexCoords = nullptr;
	FRDGBufferRef Colors = nullptr;
	FRDGBufferRef Indices = nullptr;
	FRDGBufferRef MaterialIds = nullptr;
	uint32 VertexCapacity = 0;
	uint32 IndexCapacity = 0;
	FMatrix44f LocalToWorld = FMatrix44f::Identity;
	// When set, the deform writes Unity-space position, normal and UV (8 floats per vertex) here
	// instead of the streams, and only the refined instances' indices are written (to Indices).
	FRDGBufferRef DebugVertices = nullptr;
};

// Render thread, after AddGeometryPasses in the same graph. Mesh is copied into the graph. A mesh
// with refined instances must have had ApplyRefinement.
void AddRoadMeshPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target);

// Game thread: the first graph of a refined build (geometry, parameters, seams, refinement counts),
// then Mesh.ApplyRefinement; with OutGeometry it also reads back the roads' edge and node geometry
// (the CPU lane graph and decal setup need it). A no-op when there is nothing to count or read.
bool RunRefineCountsBlocking(const FGpuGeometryInput& Geometry, FGpuRoadMeshInput& Mesh, FString& OutError, FGpuGeometryResult* OutGeometry = nullptr);

// Game thread, tests: geometry + deform, reading back the Unity-space vertices (8 floats each), and
// optionally the junction seam partner of every side (parameter slot * 2 + side: 0 = none, else
// partner + 1, before the mutuality check) and the index buffer (refined instances only, UE winding).
bool RunDeformBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<float>& OutVertices, TArray<uint32>* OutPartners,
	TArray<uint32>* OutIndices, FString& OutError);

// Game thread: records one graph, runs it, waits for the GPU and copies each returned buffer's first
// Targets[i].Value bytes into Targets[i].Key. Record returns the buffers in Targets order.
bool RunGraphBlocking(TFunctionRef<TArray<FRDGBufferRef>(FRDGBuilder&)> Record, const TArray<TPair<void*, uint32>>& Targets);

// Game thread, tests: geometry, parameters and seam matching only; OutPartners as above.
bool RunJunctionBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<uint32>& OutPartners, FString& OutError);
}

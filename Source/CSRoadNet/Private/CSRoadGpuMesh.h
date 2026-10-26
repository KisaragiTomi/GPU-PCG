// Road surface meshes on the GPU (Shaders/Private/CSRoad/CSRoadMesh.usf): CompositionParameters
// per road, every composition mesh deformed onto the GPU geometry and written into the typed
// streams of a UCSMesh resident set.
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
};

// One composition mesh drawn with one CompositionParameters entry (road * 3 + edge/start/end).
struct FGpuMeshInstance
{
	uint32 Mesh = 0;
	uint32 Parameters = 0;
	uint32 VertexOffset = 0;
	uint32 IndexOffset = 0;
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
	// Atlas name of each material slot (MeshTriangleMaterials values).
	TArray<FString> MaterialSlots;
	uint32 NumVertices = 0;
	uint32 NumIndices = 0;
	// Conservative bounds of the output before LocalToWorld (UE space, cm).
	FBox Bounds = FBox(ForceInit);

	bool Build(const FGraph& Graph, FAssets& Assets, FString& OutError);
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
	// instead of the streams, and no indices are written.
	FRDGBufferRef DebugVertices = nullptr;
};

// Render thread, after AddGeometryPasses in the same graph. Mesh is copied into the graph.
void AddRoadMeshPasses(FRDGBuilder& GraphBuilder, const FGpuGeometryOutput& Geometry, const FGpuRoadMeshInput& Mesh, const FGpuRoadMeshTarget& Target);

// Game thread, tests: geometry + deform, reading back the Unity-space vertices (8 floats each).
bool RunDeformBlocking(const FGpuGeometryInput& Geometry, const FGpuRoadMeshInput& Mesh, TArray<float>& OutVertices, FString& OutError);
}

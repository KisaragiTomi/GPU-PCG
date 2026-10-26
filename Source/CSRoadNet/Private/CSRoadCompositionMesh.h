// Port of Game.Rendering.BatchMeshHelpers.GenerateCompositionMesh: piece meshes combined into
// one composition mesh in composition space (x lateral 0..1, y height, z curve parameter),
// with per-vertex curve-matrix indices and sync flags in the colour channels.
#pragma once

#include "CSRoadAssets.h"

namespace CSRoad
{
// BatchMeshHelpers.VertexData. Colour packs Color32 r | g << 8 | b << 16 | a << 24;
// UV0 holds the half4 values already rounded through Unity's f32tof16.
struct FCompositionVertex
{
	float3 Position;
	uint Normal = 0;
	uint Tangent = 0;
	uint Color = 0;
	float4 UV0;
};

struct FCompositionMesh
{
	TArray<FCompositionVertex> Vertices;
	TArray<int32> Indices;
	TArray<MeshMaterial> Materials;
};

uint F32ToF16(float X);
float F16ToF32(uint X);

// RoadMeshes.Compile: hidden sections are dropped before the original builder runs.
void GenerateCompositionMesh(FAssets& Assets, const FComposition& Composition, FCompositionMesh& Out);
}

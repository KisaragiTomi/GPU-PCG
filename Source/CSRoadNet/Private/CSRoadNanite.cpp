#include "CSRoadNanite.h"

#if WITH_EDITOR
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSRoadNet.h"
#include "Engine/StaticMesh.h"
#include "HAL/IConsoleManager.h"
#include "MeshDescription.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "StaticMeshAttributes.h"

#include <atomic>

namespace CSRoad
{
namespace
{
// A stream the bake reads, and its copy on the way back.
struct FRoadBakeStream
{
	ECSGpuMeshSemantic Semantic = ECSGpuMeshSemantic::None;
	ECSGpuStreamRole Role = ECSGpuStreamRole::AuxVertex;
	uint32 Bytes = 0;
	// Per vertex, index or triangle.
	uint32 UnitBytes = 0;
	TRefCountPtr<FRDGPooledBuffer> Pooled;
	FRHIGPUBufferReadback* Readback = nullptr;
	TArray<uint8> Data;
};

float RoadNanite_Snorm8(uint32 Packed, uint32 Byte)
{
	return FMath::Clamp(float(int8((Packed >> (Byte * 8u)) & 0xffu)) / 127.0f, -1.0f, 1.0f);
}

FVector3f RoadNanite_Unpack(uint32 Packed)
{
	return FVector3f(RoadNanite_Snorm8(Packed, 0), RoadNanite_Snorm8(Packed, 1), RoadNanite_Snorm8(Packed, 2)).GetSafeNormal();
}

FName RoadNanite_Slot(int32 Slot)
{
	return FName(*FString::Printf(TEXT("Slot_%d"), Slot));
}

constexpr ECSGpuMeshSemantic RoadNanite_Semantics[] = { ECSGpuMeshSemantic::Position, ECSGpuMeshSemantic::TangentBasis, ECSGpuMeshSemantic::TexCoord, ECSGpuMeshSemantic::Index, ECSGpuMeshSemantic::MaterialId };

TAutoConsoleVariable<float> CVarCSRoadNaniteFallbackError(TEXT("CSRoad.NaniteFallbackError"), 1.0f,
	TEXT("Relative error of the baked road tiles' fallback meshes (what ray tracing and non-Nanite views draw): the build stops simplifying at this times 1% of the square root of the mesh's area."));
}

struct FRoadBake::FShared
{
	TArray<FRoadBakeStream> Streams;
	uint32 NumVertices = 0;
	uint32 NumIndices = 0;
	// 0: copying, 1: on the CPU, 2: a copy could not be read.
	std::atomic<int32> Copied { 0 };
	// The description task's: where the mesh's local space sits, and whether it was committed.
	FVector Origin = FVector::ZeroVector;
	bool bDescribed = false;

	const FRoadBakeStream* Find(ECSGpuMeshSemantic Semantic) const
	{
		return Streams.FindByPredicate([Semantic](const FRoadBakeStream& Stream) { return Stream.Semantic == Semantic; });
	}

	~FShared()
	{
		TArray<FRHIGPUBufferReadback*> Readbacks;
		for (FRoadBakeStream& Stream : Streams) if (Stream.Readback) Readbacks.Add(Stream.Readback);
		if (Readbacks.Num() == 0) return;
		// On the render thread, after any command that still reads them.
		ENQUEUE_RENDER_COMMAND(CSRoadBakeRelease)([Readbacks](FRHICommandListImmediate&) { for (FRHIGPUBufferReadback* Readback : Readbacks) delete Readback; });
	}

	// The copies as a mesh description committed to Mesh, in the space of its vertices' box centre.
	void Describe(UStaticMesh& Mesh)
	{
		const FRoadBakeStream* Positions = Find(ECSGpuMeshSemantic::Position);
		const FRoadBakeStream* Tangents = Find(ECSGpuMeshSemantic::TangentBasis);
		const FRoadBakeStream* TexCoords = Find(ECSGpuMeshSemantic::TexCoord);
		const FRoadBakeStream* Indices = Find(ECSGpuMeshSemantic::Index);
		const FRoadBakeStream* Materials = Find(ECSGpuMeshSemantic::MaterialId);
		const float* P = reinterpret_cast<const float*>(Positions->Data.GetData());
		const uint32* T = reinterpret_cast<const uint32*>(Tangents->Data.GetData());
		const float* UV = reinterpret_cast<const float*>(TexCoords->Data.GetData());
		const uint32 UVStride = TexCoords->UnitBytes / sizeof(float);
		const uint32* I = reinterpret_cast<const uint32*>(Indices->Data.GetData());
		const uint32* M = reinterpret_cast<const uint32*>(Materials->Data.GetData());

		// On whole centimetres: the tiles' Nanite meshes then quantize the points they share alike.
		FBox Box(ForceInit);
		for (uint32 V = 0; V < NumVertices; V++) Box += FVector(P[V * 3 + 0], P[V * 3 + 1], P[V * 3 + 2]);
		const FVector Centre = Box.GetCenter();
		Origin = FVector(FMath::RoundToDouble(Centre.X), FMath::RoundToDouble(Centre.Y), FMath::RoundToDouble(Centre.Z));

		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> VertexPositions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> TangentXs = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(1);
		const int32 NumSlots = FMath::Max(1, Mesh.GetStaticMaterials().Num());
		TArray<FPolygonGroupID> Groups;
		for (int32 Slot = 0; Slot < NumSlots; Slot++)
		{
			Groups.Add(Description.CreatePolygonGroup());
			Attributes.GetPolygonGroupMaterialSlotNames()[Groups.Last()] = RoadNanite_Slot(Slot);
		}
		Description.ReserveNewVertices(int32(NumVertices));
		Description.ReserveNewVertexInstances(int32(NumVertices));
		Description.ReserveNewTriangles(int32(NumIndices / 3));
		TArray<FVertexInstanceID> Corners;
		Corners.SetNumUninitialized(int32(NumVertices));
		for (uint32 V = 0; V < NumVertices; V++)
		{
			const FVertexID Vertex = Description.CreateVertex();
			VertexPositions[Vertex] = FVector3f(FVector(P[V * 3 + 0], P[V * 3 + 1], P[V * 3 + 2]) - Origin);
			const FVertexInstanceID Corner = Description.CreateVertexInstance(Vertex);
			// TangentX, then TangentZ (the normal) with the binormal's sign in w.
			TangentXs[Corner] = RoadNanite_Unpack(T[V * 2 + 0]);
			Normals[Corner] = RoadNanite_Unpack(T[V * 2 + 1]);
			Signs[Corner] = RoadNanite_Snorm8(T[V * 2 + 1], 3) < 0.0f ? -1.0f : 1.0f;
			UVs.Set(Corner, 0, FVector2f(UV[V * UVStride + 0], UV[V * UVStride + 1]));
			Corners[V] = Corner;
		}
		for (uint32 Triangle = 0; Triangle < NumIndices / 3; Triangle++)
		{
			const uint32 A = I[Triangle * 3 + 0];
			const uint32 B = I[Triangle * 3 + 1];
			const uint32 C = I[Triangle * 3 + 2];
			if (A >= NumVertices || B >= NumVertices || C >= NumVertices || A == B || B == C || A == C) continue;
			// Ids past the slots (none: ~0u) fall back to the first, as the GPU mesh draws them.
			const int32 Slot = M[Triangle] < uint32(NumSlots) ? int32(M[Triangle]) : 0;
			const FVertexInstanceID Triangle3[3] = { Corners[A], Corners[B], Corners[C] };
			Description.CreateTriangle(Groups[Slot], Triangle3);
		}
		if (Description.Triangles().Num() == 0) return;
		Mesh.CreateMeshDescription(0, MoveTemp(Description));
		// Any thread with the package left alone; the hash keys the DDC by content.
		UStaticMesh::FCommitMeshDescriptionParams Commit;
		Commit.bMarkPackageDirty = false;
		Commit.bUseHashAsGuid = true;
		Mesh.CommitMeshDescription(0, Commit);
		bDescribed = true;
	}
};

FRoadBake::~FRoadBake()
{
	// The task fills the static mesh: it may not outlive the bake holding the mesh.
	if (Describe.IsValid()) Describe.Wait();
}

bool FRoadBake::Start(const UCSMesh& Mesh, uint32 NumVertices, uint32 NumIndices, UStaticMesh* InTarget)
{
	Current = EStage::Failed;
	const FCSMeshResident* Resident = Mesh.GetResidentPtr();
	if (!Resident || !InTarget || NumVertices < 3 || NumIndices < 3) return false;
	TSharedPtr<FShared, ESPMode::ThreadSafe> State = MakeShared<FShared, ESPMode::ThreadSafe>();
	State->NumVertices = NumVertices;
	State->NumIndices = NumIndices / 3 * 3;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams)
	{
		const ECSGpuMeshSemantic Semantic = Stream.Desc.CpuSemantic;
		if (!Stream.Desc.bReadback || !Stream.Pooled.IsValid() || State->Find(Semantic)) continue;
		if (Semantic == ECSGpuMeshSemantic::TexCoord && Stream.Desc.TexCoordIndex != 0) continue;
		bool bNeeded = false;
		for (const ECSGpuMeshSemantic Needed : RoadNanite_Semantics) bNeeded |= Semantic == Needed;
		if (!bNeeded) continue;
		FRoadBakeStream& Read = State->Streams.AddDefaulted_GetRef();
		Read.Semantic = Semantic;
		Read.Role = Stream.Desc.Role;
		Read.Pooled = Stream.Pooled;
		Read.UnitBytes = Stream.Desc.ElementsPerUnit * Stream.Desc.BytesPerElement;
		Read.Bytes = CSGpuMeshStreams::UnitsForCountSource(Stream.Desc.CountSource, NumVertices, State->NumIndices) * Read.UnitBytes;
	}
	for (const ECSGpuMeshSemantic Needed : RoadNanite_Semantics) if (!State->Find(Needed)) return false;
	// After the edits already queued: the mesh as written.
	ENQUEUE_RENDER_COMMAND(CSRoadBakeCopy)([State](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRoad.BakeReadback"));
		for (FRoadBakeStream& Stream : State->Streams)
		{
			Stream.Readback = new FRHIGPUBufferReadback(TEXT("CSRoad.BakeReadback"));
			FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(Stream.Pooled);
			AddEnqueueCopyPass(GraphBuilder, Stream.Readback, Buffer, Stream.Bytes);
			CSGpuMeshStreams::SetStreamAccessFinal(GraphBuilder, Buffer, Stream.Role);
		}
		GraphBuilder.Execute();
	});
	Shared = State;
	Target = InTarget;
	Current = EStage::Copying;
	return true;
}

FRoadBake::EStage FRoadBake::Tick(bool bMayLaunch)
{
	UStaticMesh* Mesh = Target.Get();
	if (!Mesh && Current != EStage::Done) Current = EStage::Failed;
	switch (Current)
	{
	case EStage::Copying:
	{
		const int32 Copied = Shared->Copied.load(std::memory_order_acquire);
		if (Copied == 2)
		{
			Current = EStage::Failed;
			break;
		}
		if (Copied == 0)
		{
			// Read whatever has landed; nothing waits on the GPU.
			ENQUEUE_RENDER_COMMAND(CSRoadBakePoll)([State = Shared](FRHICommandListImmediate&)
			{
				if (State->Copied.load(std::memory_order_relaxed) != 0) return;
				for (const FRoadBakeStream& Stream : State->Streams) if (!Stream.Readback || !Stream.Readback->IsReady()) return;
				for (FRoadBakeStream& Stream : State->Streams)
				{
					const void* Raw = Stream.Readback->Lock(Stream.Bytes);
					if (!Raw)
					{
						State->Copied.store(2, std::memory_order_release);
						return;
					}
					Stream.Data.SetNumUninitialized(int32(Stream.Bytes));
					FMemory::Memcpy(Stream.Data.GetData(), Raw, Stream.Bytes);
					Stream.Readback->Unlock();
				}
				State->Copied.store(1, std::memory_order_release);
			});
			break;
		}
		Describe = UE::Tasks::Launch(UE_SOURCE_LOCATION, [State = Shared, Mesh]() { State->Describe(*Mesh); });
		Current = EStage::Describing;
		break;
	}
	case EStage::Describing:
		if (!Describe.IsCompleted() || !bMayLaunch) break;
		if (!Shared->bDescribed)
		{
			Current = EStage::Failed;
			break;
		}
		// The editor's static mesh compiler builds it on its threads (or here, when it may not).
		Mesh->Build(true);
		Current = EStage::Compiling;
		break;
	case EStage::Compiling:
		if (Mesh->IsCompiling()) break;
		Current = Mesh->HasValidNaniteData() ? EStage::Done : EStage::Failed;
		// What the build read, about 9 MB a Palmsland tile: the mesh is never built again (an edit bakes
		// a new one), so its source goes.
		Mesh->ClearMeshDescriptions();
		if (FMeshDescriptionBulkData* Source = Mesh->GetSourceModel(0).GetMeshDescriptionBulkData()) Source->Empty();
		break;
	default:
		break;
	}
	return Current;
}

FVector FRoadBake::Origin() const
{
	return Shared ? Shared->Origin : FVector::ZeroVector;
}

UStaticMesh* NewNaniteMesh(UObject* Outer, const TArray<UMaterialInterface*>& Materials)
{
	UStaticMesh* Mesh = NewObject<UStaticMesh>(Outer, NAME_None, RF_Transient);
	Mesh->NeverStream = true;
	const int32 NumSlots = FMath::Max(1, Materials.Num());
	for (int32 Slot = 0; Slot < NumSlots; Slot++) Mesh->GetStaticMaterials().Add(FStaticMaterial(Materials.IsValidIndex(Slot) ? Materials[Slot] : nullptr, RoadNanite_Slot(Slot), RoadNanite_Slot(Slot)));
	Mesh->SetNumSourceModels(1);
	FMeshBuildSettings& Build = Mesh->GetSourceModel(0).BuildSettings;
	Build.bRecomputeNormals = false;
	Build.bRecomputeTangents = false;
	Build.bComputeWeightedNormals = false;
	Build.bRemoveDegenerates = true;
	Build.bBuildReversedIndexBuffer = false;
	Build.bGenerateLightmapUVs = false;
	Build.bUseFullPrecisionUVs = false;
	Build.bUseHighPrecisionTangentBasis = false;
	Build.DistanceFieldResolutionScale = 0.0f;
	FMeshNaniteSettings Nanite = Mesh->GetNaniteSettings();
	Nanite.bEnabled = true;
	// Steps of 1/16 cm on every tile alike (their origins are whole centimetres).
	Nanite.PositionPrecision = 4;
	// The fallback is what ray tracing (Lumen's hardware path) and non-Nanite views draw: kept coarse.
	Nanite.GenerateFallback = ENaniteGenerateFallback::Enabled;
	Nanite.FallbackTarget = ENaniteFallbackTarget::RelativeError;
	Nanite.FallbackRelativeError = CVarCSRoadNaniteFallbackError.GetValueOnGameThread();
	Mesh->SetNaniteSettings(Nanite);
	return Mesh;
}

void RedrawShadowsOnStreaming()
{
	static bool bChecked = false;
	if (bChecked) return;
	bChecked = true;
	IConsoleVariable* Redraw = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Nanite.VSMInvalidateOnLODDelta"));
	if (!Redraw || (Redraw->GetFlags() & ECVF_SetByMask) != ECVF_SetByConstructor) return;
	Redraw->Set(1, ECVF_SetByProjectSetting);
	UE_LOG(LogCSRoadNet, Log, TEXT("r.Nanite.VSMInvalidateOnLODDelta set to 1 for the baked road tiles: shadow pages drawn before a tile's finer clusters stream in are drawn again once they have"));
}
}
#endif

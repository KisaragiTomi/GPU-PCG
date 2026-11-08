// What the city's GPU writers share (CSCityFacade.h, CSCityGround.h): the city's field as the passes read it, the instance buffers
// one owner writes, and a write on its way: its counts come back without waiting, then it writes that many, then it lands on the
// game thread. Nothing here waits for the render thread or the GPU (CSRoad::LandMeshWrites does, for tests).
#pragma once

#include "CoreMinimal.h"
#include "RenderGraphDefinitions.h"
#include "RenderGraphResources.h"

#include <atomic>

class AActor;
class FRDGBuilder;
class FRHICommandListImmediate;
class UCSGpuInstancedMeshComponent;
class UStaticMesh;
class USceneComponent;

namespace CSCity
{
inline constexpr int32 MaxGpuWriteSlots = 33;

// The GPU copy of a field: only the render thread looks inside once it is uploaded.
class FFacadeFieldGpu
{
public:
	TRefCountPtr<FRDGPooledBuffer> Table;
	TRefCountPtr<FRDGPooledBuffer> Pages;
	TRefCountPtr<FRDGPooledBuffer> Heights;
	FIntPoint FirstPage = FIntPoint::ZeroValue;
	FIntPoint TableSize = FIntPoint::ZeroValue;
};

// A field as a pass binds it (with none, one-element buffers that are never read).
struct FFieldBinding
{
	FRDGBufferSRVRef Table = nullptr;
	FRDGBufferSRVRef Pages = nullptr;
	FRDGBufferSRVRef Heights = nullptr;
	FIntVector4 Box = FIntVector4(0, 0, 0, 0);
	uint32 On = 0;
	uint32 HeightsOn = 0;
};
FFieldBinding BindField(FRDGBuilder& GraphBuilder, const FFacadeFieldGpu* Field);

// The instance buffers of one owner's writes, slot by slot, which only the render thread touches.
class FFacadeGpu
{
public:
	struct FSlot
	{
		TRefCountPtr<FRDGPooledBuffer> Rows;
		TRefCountPtr<FRDGPooledBuffer> Counter;
		uint32 Capacity = 0;
	};
	// Render thread only.
	FSlot Slots[MaxGpuWriteSlots];
	// The last write asked for: an older one still on its way comes to nothing.
	std::atomic<uint32> Latest { 0 };
	// Game thread only: writes on their way.
	int32 InFlight = 0;
};

// A slot's buffers grown to take Count modules (a quarter more, so that a building or two more does not grow them again) or, with
// none, let go. Render thread.
void GrowSlot(FFacadeGpu::FSlot& Slot, uint32 Count);

// One write on its way.
struct FGpuWrite
{
	virtual ~FGpuWrite() = default;
	// Render thread: the counts, if they are back, and on with the write. Sets State.
	virtual void Look(FRHICommandListImmediate& RHICmdList) = 0;
	// Game thread, once State is 1: the components take what landed.
	virtual void Land() = 0;

	TSharedPtr<FFacadeGpu> Gpu;
	uint32 Serial = 0;
	// 0: on its way, 1: landed, 2: superseded or could not be read.
	std::atomic<int32> State { 0 };
};
// Watched until it lands; counted by CSRoad::MeshWritesInFlight, and CSRoad::LandMeshWrites brings it on.
void AddGpuWrite(const TSharedRef<FGpuWrite>& Write);

// A slot's component after a write: made if it has modules and none yet, given the buffers that landed, or destroyed with none.
void LandSlot(AActor& Owner, USceneComponent* Parent, const FString& Name, bool bFollowParent, const FVector& Origin, const FBox& Bounds, UStaticMesh* Mesh, uint32 Count, const FFacadeGpu::FSlot& Landed, TObjectPtr<UCSGpuInstancedMeshComponent>& Component);
}

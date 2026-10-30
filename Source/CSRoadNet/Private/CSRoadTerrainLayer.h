// The road network's hold on the terrain: a procedural edit layer of the level's Landscape ("CS2 Roads") that
// takes the ground under the ground roads to their surface, a shoulder blending it into the terrain around
// (Shaders/Private/CSRoad/CSRoadTerrain.usf). Bridges, viaducts and tunnels leave the ground alone.
//
// The network (ACSRoadNetworkActor, bShapeTerrain) owns the layer: it makes it on the landscape, and each time it
// builds the whole network's preview (its first build, then whenever edits settle) hands its renderer the roads'
// footprint; the landscape merges the layer on the GPU when the footprint changed. The terrain under the layer
// stays as it was, so turning the layer off (or deleting it) puts it back.
#pragma once

#include "CoreMinimal.h"
#include "LandscapeEditLayer.h"
#include "LandscapeEditLayerRenderer.h"
#include "LandscapeEditTypes.h"
#include "RenderGraphResources.h"
#include "CSRoadTerrainLayer.generated.h"

class ALandscape;

namespace CSRoad
{
class FGraph;
struct FGpuGeometryResult;
struct FSavedTerrain;

// The ground roads' centre lines in short pieces of world space, and where to find them.
struct FTerrainFootprint
{
	// Per piece: its ends in the ground plane (cm: A.xy, B.xy), their heights (cm), and the half width it holds the
	// ground flat over (cm: the road's, sidewalks included, and the margin).
	TArray<FVector4f> Ends;
	TArray<FVector4f> Heights;
	// The pieces whose reach (half width and shoulder) may cover a square of the ground plane: square S (row-major
	// from GridOrigin) holds CellPieces[CellStart[S] .. CellStart[S + 1]).
	FVector2f GridOrigin = FVector2f::ZeroVector;
	float CellSize = 1600.0f;
	FIntPoint GridSize = FIntPoint(1, 1);
	TArray<uint32> CellStart;
	TArray<uint32> CellPieces;
	// The world ground plane the pieces reach.
	FBox2D Bounds = FBox2D(ForceInit);
	float Shoulder = 0.0f;
	// Of the pieces and the shoulder: equal hashes, the same ground.
	uint64 Hash = 0;
};

// The footprint of the ground roads of a built graph, from its GPU geometry (network space, Unity metres; Terrain:
// the snapshot's ground, or null), placed by NetworkToWorld. Margin: how far past a road's edge the ground stays
// flat; Shoulder: how far beyond it blends back (cm).
TSharedRef<const FTerrainFootprint> MakeTerrainFootprint(const FGraph& Graph, const FGpuGeometryResult& Geometry, const FSavedTerrain* Terrain, const FTransform& NetworkToWorld, float Margin,
	float Shoulder);
}

/** A landscape edit layer drawn by a road network: the ground taken to its ground roads' surface. */
UCLASS(meta = (DisplayName = "CS2 Roads"))
class UCSRoadTerrainLayer : public ULandscapeEditLayerProcedural
{
	GENERATED_BODY()

public:
	// ULandscapeEditLayerProcedural leaves these pure virtual.
	virtual bool SupportsMultiple() const override { return true; }
	virtual bool SupportsTargetType(ELandscapeToolTargetType InType) const override { return InType == ELandscapeToolTargetType::Heightmap; }
	virtual bool NeedsPersistentTextures() const override { return false; }
	virtual FString GetDefaultName() const override { return TEXT("CS2 Roads"); }

#if WITH_EDITOR
	// The renderer of the network owning this layer.
	virtual TArray<UE::Landscape::EditLayers::FEditLayerRendererState> GetEditLayerRendererStates(const UE::Landscape::EditLayers::FMergeContext* InMergeContext) override;
#endif
};

/** What draws a network's terrain layer in the merge: its footprint, uploaded to the GPU once each. */
UCLASS(Transient)
class UCSRoadTerrainRenderer : public UObject, public ILandscapeEditLayerRenderer
{
	GENERATED_BODY()

public:
	// The footprint drawn from now on (null: nothing), and how. Uploads it when it is another one.
	void Set(const TSharedPtr<const CSRoad::FTerrainFootprint>& InFootprint, ALandscape* InLandscape, float InInfluence, float InHeightOffset);
	// Whether it draws that ground (the footprint by its hash) as asked already: the landscape need not merge again.
	bool Draws(const CSRoad::FTerrainFootprint* InFootprint, float InInfluence, float InHeightOffset) const
	{
		return Footprint.IsValid() == (InFootprint != nullptr) && (!InFootprint || Footprint->Hash == InFootprint->Hash) && Influence == InInfluence && HeightOffset == InHeightOffset;
	}

#if WITH_EDITOR
	virtual FString GetEditLayerRendererDebugName() const override { return TEXT("CS2 Roads"); }
	virtual void GetRendererStateInfo(const UE::Landscape::EditLayers::FMergeContext* InMergeContext, UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutSupportedTargetTypeState,
		UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutEnabledTargetTypeState, TArray<UE::Landscape::EditLayers::FTargetLayerGroup>& OutTargetLayerGroups) const override;
	virtual TArray<UE::Landscape::EditLayers::FEditLayerRenderItem> GetRenderItems(const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const override;
	virtual UE::Landscape::EditLayers::ERenderFlags GetRenderFlags(const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const override;
	virtual bool RenderLayer(UE::Landscape::EditLayers::FRenderParams& RenderParams, UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder) override;
#endif

private:
	TSharedPtr<const CSRoad::FTerrainFootprint> Footprint;
	TWeakObjectPtr<ALandscape> Landscape;
	float Influence = 1.0f;
	float HeightOffset = 0.0f;

	// The footprint's buffers, filled on the render thread by the upload Set enqueues; the merge's passes, enqueued
	// after it, register them.
	struct FGpu
	{
		TRefCountPtr<FRDGPooledBuffer> Ends;
		TRefCountPtr<FRDGPooledBuffer> Heights;
		TRefCountPtr<FRDGPooledBuffer> CellStart;
		TRefCountPtr<FRDGPooledBuffer> CellPieces;
	};
	TSharedPtr<FGpu, ESPMode::ThreadSafe> Gpu;
};

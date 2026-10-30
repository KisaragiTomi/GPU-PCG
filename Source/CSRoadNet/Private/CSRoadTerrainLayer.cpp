#include "CSRoadTerrainLayer.h"

#include "CSRoadCurves.h"
#include "CSRoadGpuGeometry.h"
#include "CSRoadGraph.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadNetworkFile.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "EngineUtils.h"
#include "GlobalShader.h"
#include "Hash/xxhash.h"
#include "Landscape.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#if WITH_EDITOR
#include "Engine/TextureRenderTarget2D.h"
#include "LandscapeEditLayerMergeRenderContext.h"
#include "LandscapeEditLayerRendererState.h"
#include "LandscapeEditLayerTargetTypeState.h"
#include "LandscapeEditResourcesSubsystem.h"
#include "LandscapeUtils.h"
#include "TextureResource.h"
#endif

namespace CSRoad
{
TSharedRef<const FTerrainFootprint> MakeTerrainFootprint(const FGraph& Graph, const FGpuGeometryResult& Geometry, const FSavedTerrain* Terrain, const FTransform& NetworkToWorld, float Margin, float Shoulder)
{
	TSharedRef<FTerrainFootprint> Out = MakeShared<FTerrainFootprint>();
	Out->Shoulder = Shoulder;
	auto World = [&NetworkToWorld](const float3& P) { return NetworkToWorld.TransformPosition(FVector(P.x * 100.0, -P.z * 100.0, P.y * 100.0)); };
	// A stretch of road surface between its two borders (the GPU geometry's, at the heights the road is drawn at),
	// in pieces of about 4 m along its middle, each as wide as the borders there are apart. Pieces more than
	// MaxDeparture above or below the snapshot's ground (ramps CS2 does not call elevated) leave it alone.
	constexpr float MaxDeparture = 4.0f;
	auto AddStretch = [&](const Segment& Stretch)
	{
		const float Length = FMath::Max(Stretch.m_Length.x, Stretch.m_Length.y);
		if (!(Length > 0.01f)) return;
		const int32 Pieces = FMath::Clamp(FMath::CeilToInt32(Length / 4.0f), 1, 64);
		FVector From;
		float FromHalf = 0.0f;
		bool bFromOn = false;
		for (int32 K = 0; K <= Pieces; K++)
		{
			const float T = float(K) / Pieces;
			const float3 Left = MathUtils::Position(Stretch.m_Left, T);
			const float3 Right = MathUtils::Position(Stretch.m_Right, T);
			const float3 Middle = (Left + Right) * 0.5f;
			const bool bOn = !Terrain || FMath::Abs(Middle.y - Terrain->Sample(Middle)) <= MaxDeparture;
			const FVector To = World(Middle);
			const float Half = float(FVector2D::Distance(FVector2D(World(Left)), FVector2D(World(Right)))) * 0.5f + Margin;
			if (K > 0 && bOn && bFromOn)
			{
				Out->Ends.Add(FVector4f(From.X, From.Y, To.X, To.Y));
				Out->Heights.Add(FVector4f(From.Z, To.Z, FMath::Max(FromHalf, Half), 0.0f));
			}
			From = To;
			FromHalf = Half;
			bFromOn = bOn;
		}
	};
	for (const FRoadInstance& Road : Graph.Roads)
	{
		if (!Road.EdgeComposition || !Geometry.Edges.IsValidIndex(Road.Entity)) continue;
		// Bridges, viaducts and tunnels leave the ground alone.
		if (Road.EdgeComposition->Data.m_Flags.m_General & (CompositionFlags_General_Elevated | CompositionFlags_General_Tunnel)) continue;
		// The road's own stretch, then its share of the junctions at its ends.
		AddStretch(Geometry.Edges[Road.Entity].m_Start);
		AddStretch(Geometry.Edges[Road.Entity].m_End);
		for (const TArray<EdgeNodeGeometry>* Ends : { &Geometry.StartNodes, &Geometry.EndNodes })
		{
			if (!Ends->IsValidIndex(Road.Entity)) continue;
			AddStretch((*Ends)[Road.Entity].m_Left);
			AddStretch((*Ends)[Road.Entity].m_Right);
		}
	}
	const int32 Count = Out->Ends.Num();
	if (Count == 0) return Out;

	// Each piece in the squares its reach (a capsule, taken as its box) may cover.
	auto Reach = [&Out, Shoulder](int32 I)
	{
		const FVector4f& E = Out->Ends[I];
		const double R = Out->Heights[I].Z + Shoulder;
		return FBox2D(FVector2D(FMath::Min(E.X, E.Z) - R, FMath::Min(E.Y, E.W) - R), FVector2D(FMath::Max(E.X, E.Z) + R, FMath::Max(E.Y, E.W) + R));
	};
	for (int32 I = 0; I < Count; I++) Out->Bounds += Reach(I);
	Out->GridOrigin = FVector2f(Out->Bounds.Min);
	const FVector2D Extent = Out->Bounds.GetSize();
	Out->GridSize = FIntPoint(FMath::Max(1, FMath::CeilToInt32(Extent.X / Out->CellSize)), FMath::Max(1, FMath::CeilToInt32(Extent.Y / Out->CellSize)));
	auto Squares = [&Out, &Reach](int32 I)
	{
		const FBox2D Box = Reach(I);
		auto Square = [&Out](const FVector2D& P) { return FIntPoint(FMath::Clamp(FMath::FloorToInt32((P.X - Out->GridOrigin.X) / Out->CellSize), 0, Out->GridSize.X - 1), FMath::Clamp(FMath::FloorToInt32((P.Y - Out->GridOrigin.Y) / Out->CellSize), 0, Out->GridSize.Y - 1)); };
		return FIntRect(Square(Box.Min), Square(Box.Max));
	};
	Out->CellStart.SetNumZeroed(Out->GridSize.X * Out->GridSize.Y + 1);
	for (int32 I = 0; I < Count; I++)
	{
		const FIntRect Rect = Squares(I);
		for (int32 Y = Rect.Min.Y; Y <= Rect.Max.Y; Y++) for (int32 X = Rect.Min.X; X <= Rect.Max.X; X++) Out->CellStart[Y * Out->GridSize.X + X + 1]++;
	}
	for (int32 S = 1; S < Out->CellStart.Num(); S++) Out->CellStart[S] += Out->CellStart[S - 1];
	Out->CellPieces.SetNumUninitialized(Out->CellStart.Last());
	TArray<uint32> Next = Out->CellStart;
	for (int32 I = 0; I < Count; I++)
	{
		const FIntRect Rect = Squares(I);
		for (int32 Y = Rect.Min.Y; Y <= Rect.Max.Y; Y++) for (int32 X = Rect.Min.X; X <= Rect.Max.X; X++) Out->CellPieces[Next[Y * Out->GridSize.X + X]++] = uint32(I);
	}

	FXxHash64Builder Hash;
	Hash.Update(Out->Ends.GetData(), Out->Ends.Num() * sizeof(FVector4f));
	Hash.Update(Out->Heights.GetData(), Out->Heights.Num() * sizeof(FVector4f));
	Hash.Update(&Shoulder, sizeof(Shoulder));
	Out->Hash = Hash.Finalize().Hash;
	return Out;
}
}

#if WITH_EDITOR
class FCSRoadTerrainCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSRoadTerrainCS);
	SHADER_USE_PARAMETER_STRUCT(FCSRoadTerrainCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, Input)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, Output)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Ends)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, Heights)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellStart)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CellPieces)
		SHADER_PARAMETER(FVector3f, Origin)
		SHADER_PARAMETER(FVector3f, AxisX)
		SHADER_PARAMETER(FVector3f, AxisY)
		SHADER_PARAMETER(FVector2f, GridOrigin)
		SHADER_PARAMETER(float, CellSize)
		SHADER_PARAMETER(FIntPoint, GridSize)
		SHADER_PARAMETER(FIntPoint, Size)
		SHADER_PARAMETER(float, LandscapeZScale)
		SHADER_PARAMETER(float, LandscapeZOffset)
		SHADER_PARAMETER(float, Influence)
		SHADER_PARAMETER(float, HeightOffset)
		SHADER_PARAMETER(float, Shoulder)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
};
IMPLEMENT_GLOBAL_SHADER(FCSRoadTerrainCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSRoadTerrain.usf", "MainCS", SF_Compute);

TArray<UE::Landscape::EditLayers::FEditLayerRendererState> UCSRoadTerrainLayer::GetEditLayerRendererStates(const UE::Landscape::EditLayers::FMergeContext* InMergeContext)
{
	TArray<UE::Landscape::EditLayers::FEditLayerRendererState> States;
	const ALandscape* Landscape = GetTypedOuter<ALandscape>();
	UWorld* World = Landscape ? Landscape->GetWorld() : nullptr;
	if (!World) return States;
	for (ACSRoadNetworkActor* Network : TActorRange<ACSRoadNetworkActor>(World))
	{
		UCSRoadTerrainRenderer* Renderer = Network->TerrainRendererOf(GetGuid());
		if (Renderer) States.Emplace(InMergeContext, TScriptInterface<ILandscapeEditLayerRenderer>(Renderer));
	}
	return States;
}
#endif

void UCSRoadTerrainRenderer::Set(const TSharedPtr<const CSRoad::FTerrainFootprint>& InFootprint, ALandscape* InLandscape, float InInfluence, float InHeightOffset)
{
	Landscape = InLandscape;
	Influence = InInfluence;
	HeightOffset = InHeightOffset;
	if (Footprint == InFootprint) return;
	Footprint = InFootprint;
	Gpu = MakeShared<FGpu, ESPMode::ThreadSafe>();
	if (!Footprint || Footprint->Ends.Num() == 0) return;
	ENQUEUE_RENDER_COMMAND(CSRoadTerrainUpload)([Into = Gpu, From = Footprint](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSRoadTerrainUpload"));
		auto Upload = [&GraphBuilder](const TCHAR* Name, uint32 Stride, const void* Data, int32 Num)
		{
			return GraphBuilder.ConvertToExternalBuffer(CreateStructuredBuffer(GraphBuilder, Name, Stride, uint32(Num), Data, uint64(Stride) * uint64(Num)));
		};
		Into->Ends = Upload(TEXT("CSRoadTerrain.Ends"), sizeof(FVector4f), From->Ends.GetData(), From->Ends.Num());
		Into->Heights = Upload(TEXT("CSRoadTerrain.Heights"), sizeof(FVector4f), From->Heights.GetData(), From->Heights.Num());
		Into->CellStart = Upload(TEXT("CSRoadTerrain.CellStart"), sizeof(uint32), From->CellStart.GetData(), From->CellStart.Num());
		Into->CellPieces = Upload(TEXT("CSRoadTerrain.CellPieces"), sizeof(uint32), From->CellPieces.GetData(), FMath::Max(From->CellPieces.Num(), 1));
		GraphBuilder.Execute();
	});
}

#if WITH_EDITOR
void UCSRoadTerrainRenderer::GetRendererStateInfo(const UE::Landscape::EditLayers::FMergeContext* InMergeContext, UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutSupportedTargetTypeState,
	UE::Landscape::EditLayers::FEditLayerTargetTypeState& OutEnabledTargetTypeState, TArray<UE::Landscape::EditLayers::FTargetLayerGroup>& OutTargetLayerGroups) const
{
	OutSupportedTargetTypeState.AddTargetTypeMask(ELandscapeToolTargetTypeFlags::Heightmap);
	if (Footprint && Footprint->Ends.Num() > 0) OutEnabledTargetTypeState.AddTargetTypeMask(ELandscapeToolTargetTypeFlags::Heightmap);
}

TArray<UE::Landscape::EditLayers::FEditLayerRenderItem> UCSRoadTerrainRenderer::GetRenderItems(const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const
{
	using namespace UE::Landscape::EditLayers;
	// Each texel reads only the heights merged so far at its own place.
	const FEditLayerTargetTypeState Enabled(InMergeContext, ELandscapeToolTargetTypeFlags::Heightmap);
	return { FEditLayerRenderItem(Enabled, FInputWorldArea::CreateLocalComponent(), FOutputWorldArea::CreateLocalComponent(), false) };
}

UE::Landscape::EditLayers::ERenderFlags UCSRoadTerrainRenderer::GetRenderFlags(const UE::Landscape::EditLayers::FMergeContext* InMergeContext) const
{
	return UE::Landscape::EditLayers::ERenderFlags::RenderMode_Immediate;
}

bool UCSRoadTerrainRenderer::RenderLayer(UE::Landscape::EditLayers::FRenderParams& RenderParams, UE::Landscape::FRDGBuilderRecorder& RDGBuilderRecorder)
{
	if (!RenderParams.MergeRenderContext->IsHeightmapMerge() || !Footprint || Footprint->Ends.Num() == 0 || !Gpu) return false;
	RenderParams.MergeRenderContext->CycleBlendRenderTargets(RDGBuilderRecorder);
	ULandscapeScratchRenderTarget* WriteRT = RenderParams.MergeRenderContext->GetBlendRenderTargetWrite();
	ULandscapeScratchRenderTarget* ReadRT = RenderParams.MergeRenderContext->GetBlendRenderTargetRead();
	WriteRT->TransitionTo(ERHIAccess::RTV, RDGBuilderRecorder);
	ReadRT->TransitionTo(ERHIAccess::SRVMask, RDGBuilderRecorder);
	FTextureRenderTargetResource* Read = ReadRT->GetRenderTarget2D()->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Write = WriteRT->GetRenderTarget2D()->GameThread_GetRenderTargetResource();
	if (!Read || !Write) return false;

	// A texel of the render area is a landscape vertex: the area's transform takes (x, y) to it.
	const FTransform& Area = RenderParams.RenderAreaWorldTransform;
	const FIntPoint Size = RenderParams.RenderAreaSectionRect.Size();
	const FVector3f Origin(Area.TransformPosition(FVector::ZeroVector));
	const FVector3f AxisX(Area.TransformVector(FVector::ForwardVector));
	const FVector3f AxisY(Area.TransformVector(FVector::RightVector));
	const ALandscape* Ground = Landscape.Get();
	const float ZScale = Ground ? float(Ground->GetActorScale3D().Z) : 1.0f;
	const float ZOffset = Ground ? float(Ground->GetActorLocation().Z) : 0.0f;
	const TSharedPtr<const CSRoad::FTerrainFootprint> Drawn = Footprint;
	const TSharedPtr<FGpu, ESPMode::ThreadSafe> Buffers = Gpu;
	const float CapturedInfluence = FMath::Clamp(Influence, 0.0f, 1.0f);
	const float CapturedOffset = HeightOffset;
	auto Command = [=](FRDGBuilder& GraphBuilder)
	{
		if (!Buffers->Ends) return;
		FRDGTextureRef Input = RegisterExternalTexture(GraphBuilder, Read->GetRenderTargetTexture(), TEXT("CSRoadTerrain.Input"));
		FRDGTextureRef Blended = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Size, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV), TEXT("CSRoadTerrain.Blended"));
		FCSRoadTerrainCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCSRoadTerrainCS::FParameters>();
		Parameters->Input = Input;
		Parameters->Output = GraphBuilder.CreateUAV(Blended);
		Parameters->Ends = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffers->Ends));
		Parameters->Heights = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffers->Heights));
		Parameters->CellStart = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffers->CellStart));
		Parameters->CellPieces = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Buffers->CellPieces));
		Parameters->Origin = Origin;
		Parameters->AxisX = AxisX;
		Parameters->AxisY = AxisY;
		Parameters->GridOrigin = Drawn->GridOrigin;
		Parameters->CellSize = Drawn->CellSize;
		Parameters->GridSize = Drawn->GridSize;
		Parameters->Size = Size;
		Parameters->LandscapeZScale = ZScale;
		Parameters->LandscapeZOffset = ZOffset;
		Parameters->Influence = CapturedInfluence;
		Parameters->HeightOffset = CapturedOffset;
		Parameters->Shoulder = Drawn->Shoulder;
		TShaderMapRef<FCSRoadTerrainCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSRoadTerrain"), Shader, Parameters, FComputeShaderUtils::GetGroupCount(Size, 8));
		FRDGTextureRef Output = RegisterExternalTexture(GraphBuilder, Write->GetRenderTargetTexture(), TEXT("CSRoadTerrain.Output"));
		FRDGDrawTextureInfo DrawInfo;
		DrawInfo.Size = Size;
		AddDrawTexturePass(GraphBuilder, GetGlobalShaderMap(GMaxRHIFeatureLevel), Blended, Output, DrawInfo);
	};
	using FExternalAccess = UE::Landscape::FRDGBuilderRecorder::FRDGExternalTextureAccessFinal;
	const TArray<FExternalAccess> Accesses = { { Read, ERHIAccess::SRVMask }, { Write, ERHIAccess::RTV } };
	RDGBuilderRecorder.EnqueueRDGCommand(MoveTemp(Command), Accesses);
	return true;
}
#endif

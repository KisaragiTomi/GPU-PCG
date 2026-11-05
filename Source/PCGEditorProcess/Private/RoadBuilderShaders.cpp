#include "RoadBuilderShaders.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHI.h"
#include "RHICommandList.h"

IMPLEMENT_GLOBAL_SHADER(FRoadClearCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "ClearCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadIntersectCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "IntersectCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadClusterCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "ClusterCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadCornerSolveCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "CornerSolveCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadEmitRoadCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "EmitRoadCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadEmitCornerCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "EmitCornerCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadFinalizeCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "FinalizeCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadDepthToResultCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadDepthToResultCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadDiffuseCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadDiffuseCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FRoadUnpremultCS, "/Plugin/PCGPlugins/Shaders/Private/RoadBuilder.usf", "RoadUnpremultCS", SF_Compute);


void AddRoadDepthToResultPass(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FRDGTextureRef DepthTexture,
	FRDGTextureRef ResultTexture,
	float CameraHeight,
	float Influence,
	float HeightOffset,
	int32 DiffuseIterations)
{
	if (!DepthTexture || !ResultTexture) return;

	// Must match RoadBuilder.usf.
	constexpr int32 kDiffTile = 16;   // ROAD_DIFF_TILE
	constexpr int32 kDiffLocal = 4;   // ROAD_DIFF_LOCAL

	const FIntPoint Size(ResultTexture->Desc.Extent.X, ResultTexture->Desc.Extent.Y);
	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);
	const FIntVector Groups8 = FComputeShaderUtils::GetGroupCount(FIntVector(Size.X, Size.Y, 1), FIntVector(8, 8, 1));

	auto MakeStateTex = [&](const TCHAR* Name)
	{
		const FRDGTextureDesc D = FRDGTextureDesc::Create2D(Size, PF_FloatRGBA,
			FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		return GraphBuilder.CreateTexture(D, Name);
	};
	FRDGTextureRef StateA = MakeStateTex(TEXT("Road.DiffuseA"));
	FRDGTextureRef StateB = MakeStateTex(TEXT("Road.DiffuseB"));

	// Pass 1: depth -> initial diffusion state (WorldZ*Weight, Weight, isSource) in StateA.
	{
		auto* P = GraphBuilder.AllocParameters<FRoadDepthToResultCS::FParameters>();
		P->T_RoadDepth = DepthTexture;
		P->RW_RoadResult = GraphBuilder.CreateUAV(StateA);
		P->RoadCameraHeight = CameraHeight;
		P->RoadInfluence = Influence;
		P->RoadHeightOffset = HeightOffset;
		P->RoadResultSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.DepthToState"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadDepthToResultCS>(ShaderMap), P, Groups8);
	}

	// Pass 2: temporal-blocked height diffusion. Each dispatch runs up to kDiffLocal
	// iterations in groupshared, so global ping-pong happens ceil(N/kDiffLocal) times.
	const int32 TotalIters = FMath::Clamp(DiffuseIterations, 0, 256);
	const FIntVector DiffGroups(FMath::DivideAndRoundUp(Size.X, kDiffTile),
		FMath::DivideAndRoundUp(Size.Y, kDiffTile), 1);
	FRDGTextureRef Cur = StateA;
	FRDGTextureRef Nxt = StateB;
	for (int32 Done = 0; Done < TotalIters; Done += kDiffLocal)
	{
		auto* P = GraphBuilder.AllocParameters<FRoadDiffuseCS::FParameters>();
		P->T_RoadDiffuseIn = Cur;
		P->RW_RoadDiffuseOut = GraphBuilder.CreateUAV(Nxt);
		P->RoadDiffuseIterations = FMath::Min(kDiffLocal, TotalIters - Done);
		P->RoadDiffuseSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Diffuse"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadDiffuseCS>(ShaderMap), P, DiffGroups);
		Swap(Cur, Nxt);
	}

	// Pass 3: un-premultiply the final diffusion state -> ResultTexture (WorldZ, 0, 0, coverage).
	{
		auto* P = GraphBuilder.AllocParameters<FRoadUnpremultCS::FParameters>();
		P->T_RoadUnpremult = Cur;
		P->RW_RoadUnpremult = GraphBuilder.CreateUAV(ResultTexture);
		P->RoadUnpremultSize = Size;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Unpremult"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadUnpremultCS>(ShaderMap), P, Groups8);
	}
}

// -----------------------------------------------------------------------------
// Reusable road-build producer: the pass sequence itself, independent of where it writes
// -----------------------------------------------------------------------------

namespace
{
	constexpr uint32 GRoadMaxLegs = 12; // must match MAX_LEGS in RoadBuilder.usf

	// GPU mirror struct -- layout must match RoadBuilder.usf.
	struct FSplineInfoGPU
	{
		uint32 FirstPoint;
		uint32 NumPoints;
		float Length;
		float HalfWidth;
	};

	// The bounds to hand the mesh, in the input's own space. Input.LocalBounds is what the road
	// actor supplies; deriving a fallback matters because an invalid box does not fail loudly --
	// the road simply gets culled from some angles, and that symptom points at the renderer rather
	// than at a bound nobody set.
	FBox RoadBuilder_InputBounds(const FRoadBuildInput& Input)
	{
		if (Input.LocalBounds.IsValid) return Input.LocalBounds;

		FBox Bounds(ForceInit);
		for (const FRoadSplinePoint& Point : Input.Points) Bounds += FVector(Point.Position);
		if (!Bounds.IsValid) return Bounds;

		// The ribbon reaches half a road width past the centre line, and a junction corner strip
		// reaches CornerSolveCS's CornerDist (three half-widths, at least one sample step) past that.
		const double Margin = double(FMath::Max(Input.RoadHalfWidth * 4.0f, Input.SampleStep * 2.0f));
		return Bounds.ExpandBy(FVector(Margin));
	}
}

void BuildRoadGeometryRDG(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	const FRoadBuildInput& Input,
	const FRoadGeometryBuffers& Out)
{
	const uint32 NumSplines = Input.Splines.Num();
	if (NumSplines == 0) return;
	if (!Out.Positions || !Out.Tangents || !Out.TexCoords || !Out.Colors || !Out.Indices || !Out.IndirectArgs) return;

	// ---- upload spline samples (the only CPU -> GPU data transfer)
	TArray<FVector4f> PointData;
	PointData.Reserve(Input.Points.Num());
	for (const FRoadSplinePoint& P : Input.Points)
		PointData.Add(FVector4f(P.Position.X, P.Position.Y, P.Position.Z, P.DistanceAlongSpline));

	TArray<FSplineInfoGPU> SplineData;
	TArray<uint32> SegPrefixData;
	uint32 NumSegments = 0;
	for (const FRoadSplineRange& R : Input.Splines)
	{
		FSplineInfoGPU S;
		S.FirstPoint = R.FirstPoint;
		S.NumPoints = R.NumPoints;
		S.Length = R.Length;
		S.HalfWidth = R.HalfWidth;
		SplineData.Add(S);
		SegPrefixData.Add(NumSegments);
		NumSegments += R.NumPoints - 1;
	}
	SegPrefixData.Add(NumSegments);

	FRDGBufferRef SplinePointsBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.SplinePoints"),
		sizeof(FVector4f), PointData.Num(), PointData.GetData(), sizeof(FVector4f) * PointData.Num());
	FRDGBufferRef SplinesBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.Splines"),
		sizeof(FSplineInfoGPU), SplineData.Num(), SplineData.GetData(), sizeof(FSplineInfoGPU) * SplineData.Num());
	FRDGBufferRef SegPrefixBuf = CreateStructuredBuffer(GraphBuilder, TEXT("Road.SegPrefix"),
		sizeof(uint32), SegPrefixData.Num(), SegPrefixData.GetData(), sizeof(uint32) * SegPrefixData.Num());

	// ---- transient working buffers
	const uint32 MaxCrossRecs = FMath::Max(Input.MaxIntersections * 4, 64u);
	const uint32 MaxJunctions = FMath::Max(Input.MaxIntersections, 16u);
	const uint32 MaxLegsTotal = MaxJunctions * GRoadMaxLegs;

	FRDGBufferRef CrossRecsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxCrossRecs), TEXT("Road.CrossRecs"));
	FRDGBufferRef JunctionsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxJunctions), TEXT("Road.Junctions"));
	FRDGBufferRef LegsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(32, MaxLegsTotal), TEXT("Road.Legs"));
	FRDGBufferRef CornersBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateStructuredDesc(48, MaxLegsTotal), TEXT("Road.Corners"));
	FRDGBufferRef CountersBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 8), TEXT("Road.Counters"));
	FRDGBufferRef SplineFlagsBuf = GraphBuilder.CreateBuffer(
		FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), NumSplines), TEXT("Road.SplineFlags"));

	FRDGBufferRef PositionsRDG = Out.Positions;
	FRDGBufferRef TangentsRDG = Out.Tangents;
	FRDGBufferRef TexCoordsRDG = Out.TexCoords;
	FRDGBufferRef ColorsRDG = Out.Colors;
	FRDGBufferRef IndicesRDG = Out.Indices;
	FRDGBufferRef IndirectArgsRDG = Out.IndirectArgs;

	const auto CountersUAV = [&] { return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountersBuf, PF_R32_UINT)); };
	const auto SplineFlagsUAV = [&] { return GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SplineFlagsBuf, PF_R32_UINT)); };

	FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(FeatureLevel);

	// ---- pass 1: clear
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadClearCS::FParameters>();
		Params->RWCounters = CountersUAV();
		Params->RWIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsRDG, PF_R32_UINT));
		Params->RWSplineFlags = SplineFlagsUAV();
		Params->RWJunctions = GraphBuilder.CreateUAV(JunctionsBuf);
		Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndicesRDG, PF_R32_UINT));
		Params->NumSplines = NumSplines;
		Params->MaxJunctions = MaxJunctions;
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		const uint32 MaxItems = FMath::Max(FMath::Max(FMath::Max(NumSplines, MaxJunctions), 8u), Params->MaxIndices);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Clear"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadClearCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxItems), 64));
	}

	// ---- pass 2: curve-curve crossings + endpoint T snapping
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadIntersectCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->SegPrefix = GraphBuilder.CreateSRV(SegPrefixBuf);
		Params->RWCrossRecs = GraphBuilder.CreateUAV(CrossRecsBuf);
		Params->RWCounters = CountersUAV();
		Params->NumSplines = NumSplines;
		Params->NumSegments = NumSegments;
		Params->EndSnapRadius = Input.RoadHalfWidth * 2.0f;
		Params->MaxCrossRecs = MaxCrossRecs;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Intersect"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadIntersectCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(NumSegments + NumSplines * 2), 64));
	}

	// ---- pass 3: cluster crossings into junctions, build + sort legs
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadClusterCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->RWCrossRecs = GraphBuilder.CreateUAV(CrossRecsBuf);
		Params->RWJunctions = GraphBuilder.CreateUAV(JunctionsBuf);
		Params->RWLegs = GraphBuilder.CreateUAV(LegsBuf);
		Params->RWCounters = CountersUAV();
		Params->RWSplineFlags = SplineFlagsUAV();
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->MergeRadius = Input.RoadHalfWidth * 2.0f * Input.IntersectionMergeRadius;
		Params->MaxCrossRecs = MaxCrossRecs;
		Params->MaxJunctions = MaxJunctions;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Cluster"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadClusterCS>(ShaderMap), Params, FIntVector(1, 1, 1));
	}

	// ---- pass 4: tangent-arc corner solve
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadCornerSolveCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->Junctions = GraphBuilder.CreateSRV(JunctionsBuf);
		Params->Legs = GraphBuilder.CreateSRV(LegsBuf);
		Params->Counters = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CountersBuf, PF_R32_UINT));
		Params->RWCorners = GraphBuilder.CreateUAV(CornersBuf);
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->CornerDist = FMath::Max(Input.RoadHalfWidth * 3.0f, Input.SampleStep);
		Params->MiterDotThreshold = 0.75f;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.CornerSolve"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadCornerSolveCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxLegsTotal), 64));
	}

	const auto MakeVertexUAVs = [&](auto* Params)
	{
		Params->RWCounters = CountersUAV();
		Params->RWPositions = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PositionsRDG, PF_R32_FLOAT));
		Params->RWTangents = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TangentsRDG, PF_R32_UINT));
		Params->RWTexCoords = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(TexCoordsRDG, PF_R32_FLOAT));
		Params->RWColors = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ColorsRDG, PF_R32_UINT));
		Params->RWIndices = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndicesRDG, PF_R32_UINT));
	};

	// ---- pass 5: plain ribbons for splines without junctions
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadEmitRoadCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->SplineFlags = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(SplineFlagsBuf, PF_R32_UINT));
		MakeVertexUAVs(Params);
		Params->NumSplines = NumSplines;
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->UVInvTile = Input.UVLengthScale;
		Params->MaxVertices = FMath::Max(Input.MaxVertices, 64u);
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.EmitRoad"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadEmitRoadCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(NumSplines), 64));
	}

	// ---- pass 6: corner strips (junction surface)
	{
		auto* Params = GraphBuilder.AllocParameters<FRoadEmitCornerCS::FParameters>();
		Params->SplinePoints = GraphBuilder.CreateSRV(SplinePointsBuf);
		Params->Splines = GraphBuilder.CreateSRV(SplinesBuf);
		Params->Junctions = GraphBuilder.CreateSRV(JunctionsBuf);
		Params->Legs = GraphBuilder.CreateSRV(LegsBuf);
		Params->Corners = GraphBuilder.CreateSRV(CornersBuf);
		MakeVertexUAVs(Params);
		Params->SampleStep = Input.SampleStep;
		Params->HalfWidth = Input.RoadHalfWidth;
		Params->UVInvTile = Input.UVLengthScale;
		Params->MaxVertices = FMath::Max(Input.MaxVertices, 64u);
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		Params->MaxCornerSamples = 256;
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.EmitCorner"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadEmitCornerCS>(ShaderMap), Params,
			FComputeShaderUtils::GetGroupCount(int32(MaxLegsTotal), 64));
	}

	// ---- pass 7: fill DrawIndexedIndirect arguments + persist the mesh counters
	{
		// MeshCounters is optional (the heightmap path never reads it back); give the shader a
		// throwaway buffer when the caller didn't supply one so the UAV binding is always valid.
		FRDGBufferRef MeshCountersRDG = Out.MeshCounters
			? Out.MeshCounters
			: GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 2), TEXT("Road.MeshCountersScratch"));

		auto* Params = GraphBuilder.AllocParameters<FRoadFinalizeCS::FParameters>();
		Params->RWCounters = CountersUAV();
		Params->RWIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgsRDG, PF_R32_UINT));
		Params->RWMeshCounters = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MeshCountersRDG, PF_R32_UINT));
		Params->MaxVertices = FMath::Max(Input.MaxVertices, 64u);
		Params->MaxIndices = FMath::Max(Input.MaxIndices, 192u);
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Road.Finalize"), ERDGPassFlags::Compute,
			TShaderMapRef<FRoadFinalizeCS>(ShaderMap), Params, FIntVector(1, 1, 1));
	}
}

// -----------------------------------------------------------------------------
// The road as a UCSMesh operator
// -----------------------------------------------------------------------------

bool BuildRoadGeometryIntoMesh(UCSMesh* Target, const FRoadBuildInput& Input, const FTransform& InputToWorld)
{
	if (!Target || Input.Splines.Num() == 0) return false;
	// The bail-out the scene proxy used to make. None of the road kernels compile below SM5, and
	// building anyway would leave the caller with an allocated, empty mesh instead of a refusal.
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return false;

	// A genuinely indexed mesh: the two capacities are independent (IndexCapacity != VertexCapacity).
	// The floors are the ones the kernels clamp their writes against.
	const uint32 VertexCapacity = FMath::Max(Input.MaxVertices, 64u);
	const uint32 IndexCapacity = FMath::Max(Input.MaxIndices, 192u);

	// Load-bearing rather than defensive: the emit kernels bound their writes by Input.MaxVertices /
	// MaxIndices, not by what the mesh actually got. A refused allocation leaves the previous,
	// possibly smaller, buffers in place -- and building into those writes past the end of them.
	if (!Target->EnsureCapacitySync(int32(VertexCapacity), int32(IndexCapacity)))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[RoadMesh] Road build skipped: capacity for %u vertices / %u indices was refused."),
			VertexCapacity, IndexCapacity);
		return false;
	}

	const FBox InputBounds = RoadBuilder_InputBounds(Input);

	// Set from inside the edit, so it reports whether the passes were actually added rather than
	// whether the edit ran. EditMeshSync's flush is what makes writing to a caller-stack local here
	// safe (same fence that keeps the lambda itself alive).
	bool bBuilt = false;
	Target->EditMeshSync([&Input, &InputBounds, &bBuilt](FCSMeshEditContext& Context)
	{
		FRoadGeometryBuffers Out;
		Out.Positions = Context.Positions();
		Out.Tangents = Context.Tangents();
		Out.TexCoords = Context.TexCoords();
		Out.Colors = Context.Colors();
		Out.Indices = Context.Indices();
		Out.IndirectArgs = Context.IndirectArgs();
		Out.MeshCounters = Context.Counters();
		if (!Out.Positions || !Out.Tangents || !Out.TexCoords || !Out.Colors) return;
		if (!Out.Indices || !Out.IndirectArgs || !Out.MeshCounters) return;

		// No SetStreamAccessFinal and no GraphBuilder.Execute() here: both belong to
		// EditMeshSync, which restores every resident stream's access state for the whole edit.
		// Doing either by hand is the failure that has no symptom but "it stopped drawing".
		BuildRoadGeometryRDG(Context.GraphBuilder, GMaxRHIFeatureLevel, Input, Out);

		// The road writes no material ids, but every UCSMesh carries that stream and the save path
		// reads it back as the per-triangle slot. Left holding whatever the buffer pool's previous
		// tenant wrote, a one-material road would come out of the saver scattered across dozens of
		// slots that have no materials behind them. Zero is the road's only slot.
		FRDGBufferRef MaterialIds = Context.MaterialIds();
		if (MaterialIds) AddClearUAVPass(Context.GraphBuilder, Context.GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT)), 0u);

		// FinalizeCS writes arg set 0 alone, exactly like the shared counter kernels: a table left
		// over from an earlier life of this mesh would keep indexing arg sets that describe a
		// triangle layout this build has just replaced.
		UCSMeshOps::InvalidateSections(Context);
		// Junction count and corner sample count are decided on the GPU, so the emitted size is too
		// -- the game thread must not claim to know it.
		Context.InvalidateKnownCounts();
		Context.SetWorldBounds(InputBounds);
		bBuilt = true;
	});
	if (!bBuilt) return false;

	// Into world space, which is what the resident set is by contract. Skipped at identity so a road
	// authored at the origin (and every heightmap-only caller) pays nothing for it.
	if (!InputToWorld.Equals(FTransform::Identity)) UCSMeshOps::TransformMesh(Target, InputToWorld);

	// Retention introduces a ratchet the proxy-owned path never had: the buffers used to be
	// reallocated from scratch on every rebuild, and now only EnsureCapacitySync moves them, which
	// never goes down. Without this a road edited shorter keeps the largest allocation it ever had
	// for the rest of the session. Gated on the CPU-side request because the road's counts are
	// GPU-decided, which makes ShrinkCapacitySync pay for a counter readback -- a full GPU stall --
	// before it even reaches its own hysteresis check.
	const bool bRoadNeedsLessThanItHolds = int32(VertexCapacity) < Target->GetVertexCapacity()
		|| int32(IndexCapacity) < Target->GetIndexCapacity();
	if (bRoadNeedsLessThanItHolds) Target->ShrinkCapacitySync(int32(VertexCapacity), int32(IndexCapacity));

	return true;
}

#include "CSVineTriangleSurface.h"

#include "CSBoxSceneCollectionImpl.h"
#include "CSGpuTriangleUtilities.h"
#include "CSGpuSurfaceBudget.h"
#include "CSNaniteCut.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "Misc/ScopeExit.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	class FVineSurfaceSoupCS : public FGlobalShader
	{
		DECLARE_GLOBAL_SHADER(FVineSurfaceSoupCS);
		SHADER_USE_PARAMETER_STRUCT(FVineSurfaceSoupCS, FGlobalShader);
		BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float>, SurfacePositions)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SurfaceIndices)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<float4>, OtherTriangles)
			SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, OtherCounter)
			SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RW_SurfaceSoup)
			SHADER_PARAMETER(uint32, NaniteTriangleCount)
			SHADER_PARAMETER(uint32, OtherTriangleCapacity)
			SHADER_PARAMETER(FVector3f, SurfaceBoundsMin)
			SHADER_PARAMETER(FVector3f, SurfaceBoundsMax)
		END_SHADER_PARAMETER_STRUCT()
		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
		{
			return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
		}
	};
}

IMPLEMENT_GLOBAL_SHADER(FVineSurfaceSoupCS, "/Plugin/PCGPlugins/Shaders/Private/CSVineTriangleSurface.usf", "BuildVineSurfaceSoupCS", SF_Compute);

TSharedPtr<FCSVineTriangleSurfaceInputs, ESPMode::ThreadSafe> PrepareVineTriangleSurface(
	UObject* Outer, UWorld* World, const FCSBoxSceneCollectOptions& Options,
	float WorldCutError, int32 MemoryBudgetMiB, float MaxProjectionDistance)
{
	using namespace CSMeshGenInternal;
	using namespace CSGpuSurfaceBudget;
	if (!World || !Options.QueryBox.IsValid) return nullptr;
	const uint64 BudgetBytes = uint64(FMath::Clamp(MemoryBudgetMiB, 1, 128)) * 1024ull * 1024ull;
	const int32 PoolCapacity = int32(TriangleCapacity(BudgetBytes));
	auto Result = MakeShared<FCSVineTriangleSurfaceInputs, ESPMode::ThreadSafe>();
	Result->MemoryBudgetBytes = BudgetBytes;
	Result->Bounds = Options.QueryBox;
	Result->MaxProjectionDistance = FMath::Max(MaxProjectionDistance, 1.0f);
	TArray<FCSStaticMeshTriangleRequest> Requests;
	BuildBoxSceneTriangleRequestsInternal(World, Options.QueryBox, Options.LODIndex, Requests, Options.RequiredActorTags);
	TArray<FCSStaticMeshTriangleRequest> OtherRequests;
	TArray<FCSNaniteCutSource> NaniteSources;
	for (const FCSStaticMeshTriangleRequest& Request : Requests)
	{
		if (!Request.StaticMesh || Request.SourceActor == Options.ExcludedActor) continue;
		bool bExcluded = false;
		for (FName Tag : Options.ExcludedActorTags)
		{
			if (Request.SourceActor && Request.SourceActor->ActorHasTag(Tag)) bExcluded = true;
		}
		if (bExcluded) continue;
		if (!Request.StaticMesh->HasValidNaniteData())
		{
			OtherRequests.Add(Request);
			continue;
		}
		const double MinScale = Request.LocalToWorld.GetScale3D().GetAbs().GetMin();
		if (MinScale <= UE_SMALL_NUMBER) continue;
		FCSNaniteCutSource& Source = NaniteSources.AddDefaulted_GetRef();
		Source.Mesh = Request.StaticMesh;
		Source.Transform = Request.LocalToWorld;
		Source.CutError = FMath::Max(WorldCutError, 0.0f) / float(MinScale);
	}

	auto Other = MakeShared<FCSBoxScenePreparedDataImpl, ESPMode::ThreadSafe>();
	Other->TotalStaticMeshTriangleCount = ResolveStaticMeshTriangleRequests(
		OtherRequests, Options.ExcludedActor, Options.ExcludedActorTags, true, Other->ResolvedRequests);
	// No reference-point filtering: a large triangle spanning a wall corner must remain available.
	if (Options.bIncludeLandscape)
	{
		BuildBoxSceneLandscapeTrianglesInternal(World, Options.QueryBox, TArray<FVector>(), 0.0f,
			PoolCapacity + 1, Other->LandscapeTriangleData);
	}
	const uint32 LandscapeCount = uint32(GetTriangleMeshDataTriangleCount(Other->LandscapeTriangleData));
	const uint64 OtherCount = Other->TotalStaticMeshTriangleCount + LandscapeCount;
	uint64 MaterialMapBytes = 0;
	for (const auto& Request : Other->ResolvedRequests) MaterialMapBytes += BufferBytes(4ull * FMath::Max(Request.TriangleCount, 1));
	const int32 Budget = OtherCount <= uint64(PoolCapacity)
		? int32(TriangleCapacity(BudgetBytes, uint32(OtherCount), LandscapeCount, MaterialMapBytes, !NaniteSources.IsEmpty())) : 0;
	if (Budget == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineSurface] %llu non-Nanite triangles plus collection/BVH buffers exceed the shared %d MiB budget. Reduce the region or select a coarser source LOD."), OtherCount, FMath::Clamp(MemoryBudgetMiB, 1, 128));
		return nullptr;
	}
	Other->SafeMaxTriangles = FMath::Max(int32(OtherCount), 1);
	Result->OtherGeometry.Impl = Other;
	Result->OtherTriangleCapacity = uint32(OtherCount);
	if (!NaniteSources.IsEmpty())
	{
		if (OtherCount == uint64(Budget)) return nullptr;
		TStrongObjectPtr<UCSMesh> Mesh(NewObject<UCSMesh>(Outer));
		// Drop unused decoded attributes before the vine graph, including on extraction failure.
		ON_SCOPE_EXIT { Mesh->ReleaseDeferred(); };
		FCSNaniteCutOptions CutOptions;
		CutOptions.bAppend = false;
		CutOptions.MaxTriangles = Budget - int32(OtherCount);
		CutOptions.MaxWorkingMemoryBytes = BudgetBytes;
		FCSNaniteCutResult Cut;
		UCSNaniteCutOps::AppendNaniteCuts(Mesh.Get(), NaniteSources, CutOptions, Cut);
		for (const FCSNaniteCutSourceResult& Source : Cut.Sources)
		{
			if (Source.Status != ECSNaniteCutStatus::Complete && Source.Status != ECSNaniteCutStatus::Incomplete)
			{
				UE_LOG(LogTemp, Warning, TEXT("[VineSurface] Nanite extraction failed (status %d); vine generation cancelled, no fallback surface substituted."), int32(Source.Status));
				return nullptr;
			}
		}
		if (Cut.WrittenTriangles <= 0) return nullptr;
		// Copy pooled references: UCSMesh::BeginDestroy explicitly releases its resident streams.
		Result->NanitePositions = Mesh->GetResident()->FindStream(ECSGpuStreamRole::Position)->Pooled;
		Result->NaniteIndices = Mesh->GetResident()->FindStream(ECSGpuStreamRole::Index)->Pooled;
		Result->NaniteTriangles = uint32(Cut.WrittenTriangles);
		UE_LOG(LogTemp, Display, TEXT("[VineSurface] Nanite CutError %.3f cm: %d sources, %d triangles, complete=%d; other geometry capacity %u."),
			WorldCutError, NaniteSources.Num(), Cut.WrittenTriangles, Cut.bAllComplete, Result->OtherTriangleCapacity);
	}
	if (Result->NaniteTriangles + Result->OtherTriangleCapacity == 0u) return nullptr;
	const uint64 RetainedBytes = Result->NaniteTriangles > 0
		? BufferBytes(Result->NanitePositions->GetSize()) + BufferBytes(Result->NaniteIndices->GetSize()) : 0;
	Result->SurfaceBufferBytes = SurfaceBytes(Result->NaniteTriangles + Result->OtherTriangleCapacity,
		Result->OtherTriangleCapacity, LandscapeCount, MaterialMapBytes, RetainedBytes);
	if (Result->SurfaceBufferBytes > BudgetBytes)
	{
		UE_LOG(LogTemp, Warning, TEXT("[VineSurface] Actual decoded capacity plus surface buffers exceeds the shared budget (%llu > %llu bytes)."), Result->SurfaceBufferBytes, BudgetBytes);
		return nullptr;
	}
	UE_LOG(LogTemp, Display, TEXT("[VineSurface] Shared budget %.1f MiB, batch capacity %d triangles, admitted %u Nanite + %u other; surface buffers %.2f MiB including collection/BVH scratch. Before region rejection; no compaction."),
		double(BudgetBytes) / (1024.0 * 1024.0), Budget, Result->NaniteTriangles, Result->OtherTriangleCapacity,
		double(Result->SurfaceBufferBytes) / (1024.0 * 1024.0));
	return Result;
}

FCSVineTriangleParameters AddVineTriangleSurfacePasses(FRDGBuilder& GraphBuilder, const FCSVineTriangleSurfaceInputs* Input)
{
	FCSVineTriangleParameters Result{};
	FRDGBufferRef Dummy = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 1), TEXT("Vine.Surface.Dummy"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Dummy, PF_R32_UINT)), 0u);
	Result.VineSurfaceTriangles = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Dummy, PF_A32B32G32R32F));
	Result.VineSurfaceBVH = Result.VineSurfaceTriangles;
	if (!Input) return Result;
	check(Input->SurfaceBufferBytes <= Input->MemoryBudgetBytes);
	const uint32 Count = Input->NaniteTriangles + Input->OtherTriangleCapacity;
	check(Count > 0u);
	FRDGBufferRef Soup = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Count * 3u), TEXT("Vine.Surface.Triangles"));
	auto* P = GraphBuilder.AllocParameters<FVineSurfaceSoupCS::FParameters>();
	P->SurfacePositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Dummy, PF_R32_FLOAT));
	P->SurfaceIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Dummy, PF_R32_UINT));
	if (Input->NaniteTriangles > 0u)
	{
		check(Input->NanitePositions && Input->NaniteIndices);
		P->SurfacePositions = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Input->NanitePositions), PF_R32_FLOAT));
		P->SurfaceIndices = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Input->NaniteIndices), PF_R32_UINT));
	}
	P->OtherTriangles = Result.VineSurfaceTriangles;
	P->OtherCounter = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Dummy, PF_R32_UINT));
	if (Input->OtherTriangleCapacity > 0u)
	{
		const auto Other = AComputeShaderMeshGenerator::AddPreparedBoxSceneTrianglesToRDG(
			GraphBuilder, GraphBuilder.RHICmdList, Input->OtherGeometry, TEXT("Vine.Surface.Other"));
		check(Other.TriangleVertices && Other.TriangleCounter);
		P->OtherTriangles = Other.TriangleVerticesSRV;
		P->OtherCounter = Other.TriangleCounterSRV;
	}
	P->RW_SurfaceSoup = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Soup, PF_A32B32G32R32F));
	P->NaniteTriangleCount = Input->NaniteTriangles;
	P->OtherTriangleCapacity = Input->OtherTriangleCapacity;
	P->SurfaceBoundsMin = FVector3f(Input->Bounds.Min);
	P->SurfaceBoundsMax = FVector3f(Input->Bounds.Max);
	TShaderMapRef<FVineSurfaceSoupCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("Vine.Surface.Soup %u", Count), Shader, P,
		FComputeShaderUtils::GetGroupCount(Count, 64));
	Result.VineSurfaceTriangles = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Soup, PF_A32B32G32R32F));
	const FVector Extent = Input->Bounds.GetSize().ComponentMax(FVector(1.0));
	const auto BVH = CSGpuTriangleUtilities::AddTriangleLBVHBuildPasses(GraphBuilder, Result.VineSurfaceTriangles,
		int32(Count), int32(FMath::RoundUpToPowerOfTwo(Count)), FVector3f(Input->Bounds.Min), FVector3f(FVector(1.0) / Extent));
	Result.VineSurfaceBVH = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(BVH.Nodes, PF_A32B32G32R32F));
	Result.VineSurfaceTriangleCount = Count;
	Result.VineSurfaceMaxDistance = Input->MaxProjectionDistance;
	return Result;
}

#include "ComputeShaderShallowWater.h"

#include "Engine/StaticMesh.h"
#include "GlobalShader.h"
#include "GlobalRenderResources.h"
#include "RenderGraphUtils.h"
#include "ShaderParameterStruct.h"
#include "StaticMeshResources.h"
#include "StaticMeshComponentLODInfo.h"

BEGIN_SHADER_PARAMETER_STRUCT(FSWRestoreMeshParameters, )
	SHADER_PARAMETER_SRV(Buffer<float>, RestorePositions)
	SHADER_PARAMETER_SRV(Buffer<uint>, RestoreIndices)
	SHADER_PARAMETER_SRV(Buffer<float4>, RestoreColors)
	SHADER_PARAMETER(uint32, RestoreTriangleCount)
	SHADER_PARAMETER(uint32, RestoreVertexCount)
	SHADER_PARAMETER(uint32, RestorePositionStride)
	SHADER_PARAMETER(uint32, RestoreHasColors)
	SHADER_PARAMETER(FIntPoint, RestoreSize)
	SHADER_PARAMETER(float, RestoreInvCaptureSize)
	SHADER_PARAMETER(FMatrix44f, RestoreLocalToSim)
	SHADER_PARAMETER(FMatrix44f, RestoreVelocityRotation)
END_SHADER_PARAMETER_STRUCT()

class FSWRestoreRasterCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSWRestoreRasterCS);
	SHADER_USE_PARAMETER_STRUCT(FSWRestoreRasterCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSWRestoreMeshParameters, Mesh)
		SHADER_PARAMETER(uint32, RestoreRasterPhase)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RestoreHeightKeys)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RestoreTriangleIds)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FSWRestoreApplyCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSWRestoreApplyCS);
	SHADER_USE_PARAMETER_STRUCT(FSWRestoreApplyCS, FGlobalShader);
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSWRestoreMeshParameters, Mesh)
		SHADER_PARAMETER(float, RestoreActorZ)
		SHADER_PARAMETER(float, RestoreEmptySceneDepth)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RestoreTriangleIds)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RestoreSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreState)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreResult)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreDepthWet)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreSmoothHeight)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreDebug)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RestoreTileMask)
	END_SHADER_PARAMETER_STRUCT()
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("RESTORE_VELOCITY_MAX"), CSSW_VELOCITY_CLAMP);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSWRestoreRasterCS, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterRestore.usf", "RasterizeResultMeshCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FSWRestoreApplyCS, "/Plugin/PCGPlugins/Shaders/Private/ShallowWaterRestore.usf", "ApplyResultMeshCS", SF_Compute);

void ACSShallowWaterCapture::ApplyResultMeshToSimulation()
{
	if (!GetWorld() || !ResultMeshComponent || HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || IsActorBeingDestroyed()) return;
	UStaticMesh* Source = ResultMeshComponent->GetStaticMesh();
	// StartSolver replaces the result component's mesh with the preview tile. Never restore that tile.
	if (!Source || Source == SimulationPreviewMesh) Source = BakedResultMesh;
	const FStaticMeshRenderData* RenderData = Source ? Source->GetRenderData() : nullptr;
	if (!RenderData || RenderData->LODResources.IsEmpty() || Source == SimulationPreviewMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] ApplyResultMeshToSimulation: %s has no usable result mesh."), *GetName());
		return;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const uint32 VertexCount = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	const uint32 TriangleCount = LOD.IndexBuffer.GetNumIndices() / 3;
	FShaderResourceViewRHIRef Positions = LOD.VertexBuffers.PositionVertexBuffer.GetSRV();
	FBufferRHIRef Indices = LOD.IndexBuffer.GetRHI();
	const FColorVertexBuffer* ColorBuffer = &LOD.VertexBuffers.ColorVertexBuffer;
	if (Source == ResultMeshComponent->GetStaticMesh() && !ResultMeshComponent->LODData.IsEmpty() && ResultMeshComponent->LODData[0].OverrideVertexColors) ColorBuffer = ResultMeshComponent->LODData[0].OverrideVertexColors;
	FShaderResourceViewRHIRef Colors = ColorBuffer->GetColorComponentsSRV();
	const bool bHasColors = Colors.IsValid() && ColorBuffer->GetNumVertices() == VertexCount;
	if (!Positions.IsValid() || !Indices.IsValid() || VertexCount == 0u || TriangleCount == 0u
		|| !FMath::IsFinite(CaptureSize) || CaptureSize <= 0 || !FMath::IsFinite(WorldPixelSize) || WorldPixelSize <= 0
		|| GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] ApplyResultMeshToSimulation: invalid domain or unavailable LOD0 GPU buffers on %s; state unchanged."), *GetName());
		return;
	}
	const FTransform SourceTransform = ResultMeshComponent->GetComponentTransform();
	const FBox SourceBounds = Source->GetBoundingBox().TransformBy(SourceTransform);
	const FVector Origin = GetActorLocation();
	const double HalfSize = CaptureSize * 0.5;
	if (SourceTransform.ContainsNaN() || !SourceBounds.IsValid || SourceBounds.Max.X < Origin.X - HalfSize
		|| SourceBounds.Min.X > Origin.X + HalfSize || SourceBounds.Max.Y < Origin.Y - HalfSize || SourceBounds.Min.Y > Origin.Y + HalfSize)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] ApplyResultMeshToSimulation: result mesh is outside the capture domain on %s; state unchanged."), *GetName());
		return;
	}
	UStaticMesh* Preview = SimulationPreviewMesh;
	if (!Preview && SimVisHISM) Preview = SimVisHISM->GetStaticMesh();
	if (!Preview || Preview == Source || Preview == BakedResultMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] ApplyResultMeshToSimulation: set SimulationPreviewMesh to a simulation tile on %s; state unchanged."), *GetName());
		return;
	}

	// Preserve timer state. Commands already queued execute before this replacement, later solver ticks after it.
	RefreshConstructionLayout();
	CaptureAll();
	if (!RT_SceneDepth || !RT_VelocityHeight || !RT_ResultVelHeight || !RT_ResultDepthWet || !RT_SmoothHeight || !RT_DebugView || !RT_TileMask) return;
	// Blueprint material hooks can touch render targets; finish them before enqueueing the replacement.
	bUseBakedResultMesh = false;
	if (!SimulationPreviewMesh) SimulationPreviewMesh = Preview;
	RebuildSimulationVisualization();
	SetMaterialParameter();
	if (SimVisHISM && SimVisHISM->GetInstanceCount() > 0) SimVisHISM->SetVisibility(true);
	ResultMeshComponent->SetVisibility(false);

	const FTransform LocalToSim(SourceTransform.GetRotation(), SourceTransform.GetTranslation() - Origin, SourceTransform.GetScale3D());
	FSWRestoreMeshParameters Mesh{};
	Mesh.RestoreTriangleCount = TriangleCount;
	Mesh.RestoreVertexCount = VertexCount;
	Mesh.RestorePositionStride = LOD.VertexBuffers.PositionVertexBuffer.GetStride() / sizeof(float);
	Mesh.RestoreHasColors = bHasColors ? 1u : 0u;
	Mesh.RestoreSize = FIntPoint(TextureSize, TextureSize);
	Mesh.RestoreInvCaptureSize = 1.0f / CaptureSize;
	Mesh.RestoreLocalToSim = FMatrix44f(LocalToSim.ToMatrixWithScale());
	Mesh.RestoreVelocityRotation = FMatrix44f(FTransform(SourceTransform.GetRotation()).ToMatrixNoScale());
	const bool bIndices32 = LOD.IndexBuffer.Is32Bit();
	const float ActorZ = Origin.Z;
	const float EmptySceneDepth = Origin.Z + MaxHeight + 9000.0f;
	FTextureRenderTargetResource* SceneDepth = RT_SceneDepth->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* State = RT_VelocityHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Result = RT_ResultVelHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* DepthWet = RT_ResultDepthWet->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Smooth = RT_SmoothHeight->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* Debug = RT_DebugView->GameThread_GetRenderTargetResource();
	FTextureRenderTargetResource* TileMask = RT_TileMask->GameThread_GetRenderTargetResource();
	ENQUEUE_RENDER_COMMAND(CSSWRestoreResultMesh)(
		[Mesh, Positions, Indices, Colors, bIndices32, ActorZ, EmptySceneDepth, SceneDepth, State, Result, DepthWet, Smooth, Debug, TileMask](FRHICommandListImmediate& RHICmdList) mutable
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			// Strong RHI references keep the input alive if the component switches meshes before execution.
			FShaderResourceViewRHIRef IndexSRV = RHICmdList.CreateShaderResourceView(Indices,
				FRHIViewDesc::CreateBufferSRV().SetType(FRHIViewDesc::EBufferType::Typed).SetFormat(bIndices32 ? PF_R32_UINT : PF_R16_UINT));
			Mesh.RestorePositions = Positions;
			Mesh.RestoreIndices = IndexSRV;
			Mesh.RestoreColors = Colors;
			// The color-free branch still declares a typed SRV; supply a valid unused binding.
			if (!Mesh.RestoreHasColors) Mesh.RestoreColors = GNullColorVertexBuffer.VertexBufferSRV;
			const uint32 PixelCount = uint32(Mesh.RestoreSize.X) * uint32(Mesh.RestoreSize.Y);
			FRDGBufferRef HeightKeys = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PixelCount), TEXT("CSSW.Restore.HeightKeys"));
			FRDGBufferRef TriangleIds = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), PixelCount), TEXT("CSSW.Restore.TriangleIds"));
			FRDGBufferUAVRef HeightUAV = GraphBuilder.CreateUAV(HeightKeys);
			FRDGBufferUAVRef TriangleUAV = GraphBuilder.CreateUAV(TriangleIds);
			AddClearUAVPass(GraphBuilder, HeightUAV, 0u);
			AddClearUAVPass(GraphBuilder, TriangleUAV, MAX_uint32);
			TShaderMapRef<FSWRestoreRasterCS> RasterShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const uint32 Groups = FMath::DivideAndRoundUp(Mesh.RestoreTriangleCount, 64u);
			const FIntVector RasterGroups(FMath::Min(Groups, 65535u), FMath::DivideAndRoundUp(Groups, 65535u), 1);
			for (uint32 Phase = 0; Phase < 2; ++Phase)
			{
				auto* Parameters = GraphBuilder.AllocParameters<FSWRestoreRasterCS::FParameters>();
				Parameters->Mesh = Mesh;
				Parameters->RestoreRasterPhase = Phase;
				Parameters->RestoreHeightKeys = HeightUAV;
				Parameters->RestoreTriangleIds = TriangleUAV;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.Restore.Raster%d", Phase), RasterShader, Parameters, RasterGroups);
			}
			auto Register = [&GraphBuilder](FTextureRenderTargetResource* Resource, const TCHAR* Name)
			{
				return GraphBuilder.RegisterExternalTexture(CreateRenderTarget(Resource->GetRenderTargetTexture(), Name));
			};
			// Existing simulation RTs are not created with UAV support. Match the solver's transient + copy path.
			TArray<TPair<FRDGTextureRef, FRDGTextureRef>> OutputCopies;
			auto Output = [&GraphBuilder, &Register, &OutputCopies](FTextureRenderTargetResource* Resource, const TCHAR* Name)
			{
				FRDGTextureRef Destination = Register(Resource, Name);
				FRDGTextureRef Texture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Destination->Desc.Extent,
					Destination->Desc.Format, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV), Name);
				OutputCopies.Emplace(Texture, Destination);
				return GraphBuilder.CreateUAV(Texture);
			};
			auto* Parameters = GraphBuilder.AllocParameters<FSWRestoreApplyCS::FParameters>();
			Parameters->Mesh = Mesh;
			Parameters->RestoreActorZ = ActorZ;
			Parameters->RestoreEmptySceneDepth = EmptySceneDepth;
			Parameters->RestoreTriangleIds = TriangleUAV;
			Parameters->RestoreSceneDepth = Register(SceneDepth, TEXT("CSSW.Restore.SceneDepth"));
			Parameters->RestoreState = Output(State, TEXT("CSSW.Restore.State"));
			Parameters->RestoreResult = Output(Result, TEXT("CSSW.Restore.Result"));
			Parameters->RestoreDepthWet = Output(DepthWet, TEXT("CSSW.Restore.DepthWet"));
			Parameters->RestoreSmoothHeight = Output(Smooth, TEXT("CSSW.Restore.Smooth"));
			Parameters->RestoreDebug = Output(Debug, TEXT("CSSW.Restore.Debug"));
			Parameters->RestoreTileMask = Output(TileMask, TEXT("CSSW.Restore.TileMask"));
			TShaderMapRef<FSWRestoreApplyCS> ApplyShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSSW.Restore.Apply"), ApplyShader, Parameters,
				FComputeShaderUtils::GetGroupCount(Mesh.RestoreSize, FIntPoint(16, 16)));
			for (const auto& Copy : OutputCopies) AddCopyTexturePass(GraphBuilder, Copy.Key, Copy.Value);
			GraphBuilder.Execute();
		});

	LastSolverFrameNumber = 0;
	UE_LOG(LogTemp, Log, TEXT("[CSSW] ApplyResultMeshToSimulation: queued %s (%u triangles) into %dx%d on %s; uncovered cells cleared, solver timer preserved."),
		*GetNameSafe(Source), TriangleCount, TextureSize, TextureSize, *GetName());
}

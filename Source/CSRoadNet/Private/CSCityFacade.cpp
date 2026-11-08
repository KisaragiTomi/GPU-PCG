#include "CSCityFacade.h"

#include "ComputeShaderGenerateHelper.h"
#include "Containers/Ticker.h"
#include "CSCityFieldActor.h"
#include "CSCityGpuWrite.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadMeshWrite.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "Hash/xxhash.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

const FCSCityFacadeModule& UCSCityFacadeKit::Module(int32 Slot) const
{
	if (Slot >= CSCity::FacadeBaseSlots)
	{
		const int32 Index = Slot - CSCity::FacadeBaseSlots;
		const FCSCityCornerSet* Sets[CSCity::FacadeCornerStyles] = { &SquareCorners, &ChamferCorners, &RoundCorners, &RoundLargeCorners };
		const FCSCityCornerSet& Set = *Sets[FMath::Clamp(Index / 4, 0, CSCity::FacadeCornerStyles - 1)];
		switch (CSCity::EFacadeCornerPiece(Index % 4))
		{
		case CSCity::EFacadeCornerPiece::Roof: return Set.RoofCorner;
		case CSCity::EFacadeCornerPiece::Parapet: return Set.ParapetCorner;
		case CSCity::EFacadeCornerPiece::Plinth: return Set.PlinthCorner;
		default: return Set.Corner;
		}
	}
	switch (CSCity::EFacadeSlot(Slot))
	{
	case CSCity::EFacadeSlot::Window: return Window;
	case CSCity::EFacadeSlot::Door: return Door;
	case CSCity::EFacadeSlot::Corner: return Corner;
	case CSCity::EFacadeSlot::Parapet: return Parapet;
	case CSCity::EFacadeSlot::Roof: return Roof;
	case CSCity::EFacadeSlot::CornerInner: return CornerInner;
	case CSCity::EFacadeSlot::GroundWall: return GroundWall;
	case CSCity::EFacadeSlot::GroundWindow: return GroundWindow;
	case CSCity::EFacadeSlot::RoofCorner: return RoofCorner;
	case CSCity::EFacadeSlot::ParapetCorner: return ParapetCorner;
	case CSCity::EFacadeSlot::Soffit: return Soffit;
	case CSCity::EFacadeSlot::Column: return Column;
	case CSCity::EFacadeSlot::Plinth: return Plinth;
	case CSCity::EFacadeSlot::PlinthCorner: return PlinthCorner;
	case CSCity::EFacadeSlot::Platform: return Platform;
	case CSCity::EFacadeSlot::Stair: return Stair;
	default: return Wall;
	}
}

UCSCityFacadeKit* UCSCityFacadeKit::Default()
{
	return LoadObject<UCSCityFacadeKit>(nullptr, TEXT("/PCGPlugins/CSRoadNet/Houses/Facade/DA_FacadeKit_Simple.DA_FacadeKit_Simple"));
}

BEGIN_SHADER_PARAMETER_STRUCT(FCSCityFacadeCommon, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FacadeBuildingsF)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FacadeBuildingsU)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FacadeFieldTable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FacadeFieldPages)
	SHADER_PARAMETER(FIntVector4, FacadeFieldBox)
	SHADER_PARAMETER(uint32, FacadeFieldOn)
	SHADER_PARAMETER(FVector4f, FacadeOrigin)
	SHADER_PARAMETER(uint32, FacadeNumBuildings)
	SHADER_PARAMETER(uint32, FacadeNumThreads)
	SHADER_PARAMETER_ARRAY(FVector4f, FacadeSlotMin, [CSCity::FacadeSlots])
	SHADER_PARAMETER_ARRAY(FVector4f, FacadeSlotSize, [CSCity::FacadeSlots])
	SHADER_PARAMETER_ARRAY(FVector4f, FacadeStyleCorner, [CSCity::FacadeCornerStyles + 1])
	SHADER_PARAMETER(FVector4f, FacadeShapeWeights)
	SHADER_PARAMETER(FVector4f, FacadeChances)
	SHADER_PARAMETER(float, FacadeStorey)
	SHADER_PARAMETER(float, FacadeGround)
	SHADER_PARAMETER(float, FacadeParapet)
	SHADER_PARAMETER(float, FacadePlinthSink)
	SHADER_PARAMETER(float, FacadeWindowChance)
	SHADER_PARAMETER(float, FacadeGroundWindowChance)
	SHADER_PARAMETER(float, FacadeColumnInset)
	SHADER_PARAMETER(float, FacadePlatformDepth)
	SHADER_PARAMETER(float, FacadePlatformMinDrop)
	SHADER_PARAMETER(float, FacadeMaxCorner)
	SHADER_PARAMETER(uint32, FacadeColumnGroup)
	SHADER_PARAMETER(uint32, FacadeColumnEvery)
	SHADER_PARAMETER(uint32, FacadeStairEvery)
	SHADER_PARAMETER(uint32, FacadeOverhangBays)
	SHADER_PARAMETER(uint32, FacadeMinSetbackStoreys)
END_SHADER_PARAMETER_STRUCT()

class FCSCityFacadeMassCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityFacadeMassCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityFacadeMassCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCSCityFacadeCommon, Common)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint4>, FacadeMassOut)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("FACADE_PASS"), 0);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSCityFacadeMassCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityFacade.usf", "FacadeMassCS", SF_Compute);

class FCSCityFacadeCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityFacadeCountCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityFacadeCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCSCityFacadeCommon, Common)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FacadeMass)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, FacadeCounts)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("FACADE_PASS"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSCityFacadeCountCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityFacade.usf", "FacadeCellCS", SF_Compute);

class FCSCityFacadeModulesCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityFacadeModulesCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityFacadeModulesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCSCityFacadeCommon, Common)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FacadeMass)
		SHADER_PARAMETER(FUintVector4, FacadeSlotIdsLo)
		SHADER_PARAMETER(uint32, FacadeSlotIdsHi)
		SHADER_PARAMETER(FUintVector4, FacadeCapacityLo)
		SHADER_PARAMETER(uint32, FacadeCapacityHi)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, FacadeRows0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, FacadeRows1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, FacadeRows2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, FacadeRows3)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, FacadeRows4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FacadeCounter0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FacadeCounter1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FacadeCounter2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FacadeCounter3)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, FacadeCounter4)
	END_SHADER_PARAMETER_STRUCT()

	// The slots one dispatch writes (more would be more UAVs than a shader may have).
	static constexpr int32 SlotsPerPass = 5;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("FACADE_PASS"), 2);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSCityFacadeModulesCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityFacade.usf", "FacadeCellCS", SF_Compute);

namespace CSCity
{
int32 FacadeCornerSlot(ECSCityCornerStyle Style, EFacadeCornerPiece Piece)
{
	if (Style == ECSCityCornerStyle::Auto)
	{
		switch (Piece)
		{
		case EFacadeCornerPiece::Roof: return int32(EFacadeSlot::RoofCorner);
		case EFacadeCornerPiece::Parapet: return int32(EFacadeSlot::ParapetCorner);
		case EFacadeCornerPiece::Plinth: return int32(EFacadeSlot::PlinthCorner);
		default: return int32(EFacadeSlot::Corner);
		}
	}
	return FacadeBaseSlots + (int32(Style) - 1) * 4 + int32(Piece);
}

TSharedPtr<FFacadeFieldGpu> MakeFacadeField(TArray<uint32> Table, TArray<uint32> Pages, TArray<uint32> Heights, const FIntPoint& FirstPage, const FIntPoint& TableSize)
{
	TSharedPtr<FFacadeFieldGpu> Field = MakeShared<FFacadeFieldGpu>();
	Field->FirstPage = FirstPage;
	Field->TableSize = TableSize;
	if (Table.Num() == 0 || Pages.Num() == 0) return Field;
	ENQUEUE_RENDER_COMMAND(CSCityFieldUpload)([Field, Table = MoveTemp(Table), Pages = MoveTemp(Pages), Heights = MoveTemp(Heights)](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityField.Upload"));
		const FRDGBufferRef TableBuffer = CSHelper::CreateUploadedStructuredBuffer<uint32>(GraphBuilder, Table, TEXT("CSCityField.Table"), false, false).Buffer;
		const FRDGBufferRef PagesBuffer = CSHelper::CreateUploadedStructuredBuffer<uint32>(GraphBuilder, Pages, TEXT("CSCityField.Pages"), false, false).Buffer;
		GraphBuilder.QueueBufferExtraction(TableBuffer, &Field->Table, ERHIAccess::SRVMask);
		GraphBuilder.QueueBufferExtraction(PagesBuffer, &Field->Pages, ERHIAccess::SRVMask);
		if (Heights.Num() > 0)
		{
			const FRDGBufferRef HeightsBuffer = CSHelper::CreateUploadedStructuredBuffer<uint32>(GraphBuilder, Heights, TEXT("CSCityField.Heights"), false, false).Buffer;
			GraphBuilder.QueueBufferExtraction(HeightsBuffer, &Field->Heights, ERHIAccess::SRVMask);
		}
		GraphBuilder.Execute();
	});
	return Field;
}

FFieldBinding BindField(FRDGBuilder& GraphBuilder, const FFacadeFieldGpu* Field)
{
	FFieldBinding Out;
	static const TArray<uint32> Nothing = { 0u };
	const FRDGBufferSRVRef None = CSHelper::CreateUploadedStructuredBuffer<uint32>(GraphBuilder, Nothing, TEXT("CSCityField.None"), false, true).SRV;
	Out.Table = None;
	Out.Pages = None;
	Out.Heights = None;
	if (!Field || !Field->Table.IsValid() || !Field->Pages.IsValid()) return Out;
	Out.Table = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Field->Table)));
	Out.Pages = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Field->Pages)));
	Out.Box = FIntVector4(Field->FirstPage.X, Field->FirstPage.Y, Field->TableSize.X, Field->TableSize.Y);
	Out.On = 1;
	if (Field->Heights.IsValid())
	{
		Out.Heights = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(GraphBuilder.RegisterExternalBuffer(Field->Heights)));
		Out.HeightsOn = 1;
	}
	return Out;
}

void GrowSlot(FFacadeGpu::FSlot& Slot, uint32 Count)
{
	if (Count == 0)
	{
		Slot = FFacadeGpu::FSlot();
		return;
	}
	if (Slot.Rows.IsValid() && Slot.Capacity >= Count) return;
	const uint32 Capacity = Align(Count + Count / 4, 16u);
	FRDGBufferDesc RowsDesc = FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S);
	RowsDesc.Usage |= EBufferUsageFlags::SourceCopy;
	Slot.Rows = AllocatePooledBuffer(RowsDesc, TEXT("CSCity.Rows"));
	Slot.Counter = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSCity.Counter"));
	Slot.Capacity = Capacity;
}

void LandSlot(AActor& Owner, USceneComponent* Parent, const FString& Name, bool bFollowParent, const FVector& Origin, const FBox& Bounds, UStaticMesh* Mesh, uint32 Count, const FFacadeGpu::FSlot& Landed, TObjectPtr<UCSGpuInstancedMeshComponent>& Component)
{
	if (Count == 0 || !Mesh)
	{
		if (Component) Component->DestroyComponent();
		Component = nullptr;
		return;
	}
	if (!Component || !IsValid(Component))
	{
		// Made at build time, never saved or copied.
		const FName Unique = MakeUniqueObjectName(&Owner, UCSGpuInstancedMeshComponent::StaticClass(), FName(*Name));
		Component = NewObject<UCSGpuInstancedMeshComponent>(&Owner, Unique, RF_Transient | RF_DuplicateTransient);
		if (Parent) Component->SetupAttachment(Parent);
		Component->SetUsingAbsoluteLocation(!bFollowParent);
		Component->SetUsingAbsoluteRotation(!bFollowParent);
		Component->SetUsingAbsoluteScale(true);
		Component->RegisterComponent();
	}
	if (Component->BaseMesh != Mesh) Component->SetBaseMesh(Mesh);
	if (!Component->GetComponentLocation().Equals(Origin, 0.01) || !Component->GetComponentQuat().Equals(FQuat::Identity, 1e-6)) Component->SetWorldLocationAndRotation(Origin, FQuat::Identity);
	FCSGpuInstanceSourceGPU Source;
	Source.PackedInstances = Landed.Rows;
	Source.Counter = Landed.Counter;
	Source.Capacity = Landed.Capacity;
	Source.LocalBounds = Bounds.IsValid ? Bounds : FBox(FVector(-100.0), FVector(100.0));
	// Every write lands here and says so.
	Source.bNotifiesChanges = true;
	const FCSGpuInstanceSourceGPU& Held = Component->GetInstanceSourceGPU();
	const bool bSame = Held.PackedInstances == Source.PackedInstances && Held.Counter == Source.Counter && Held.Capacity == Source.Capacity && Held.LocalBounds.Equals(Source.LocalBounds) && Held.bNotifiesChanges;
	if (bSame) Component->NotifyInstanceSourceChanged();
	else Component->SetInstanceSourceGPU(Source);
}

namespace
{
// A box turned by quarter turns about Z, counter-clockwise from above.
FBox CSCityFacade_Turned(const FBox& Box, int32 Turns)
{
	FBox Out(ForceInit);
	for (const FVector& Corner : { Box.Min, Box.Max, FVector(Box.Min.X, Box.Max.Y, Box.Min.Z), FVector(Box.Max.X, Box.Min.Y, Box.Max.Z) })
	{
		FVector P = Corner;
		for (int32 T = 0; T < (Turns & 3); T++) P = FVector(-P.Y, P.X, P.Z);
		Out += P;
	}
	return Out;
}

// What the passes take: the kit's numbers.
struct FCSCityFacadeConstants
{
	FVector4f SlotMin[FacadeSlots];
	FVector4f SlotSize[FacadeSlots];
	FVector4f StyleCorner[FacadeCornerStyles + 1];
	FVector4f ShapeWeights;
	FVector4f Chances;
	float Storey, Ground, Parapet, PlinthSink, WindowChance, GroundWindowChance, ColumnInset, PlatformDepth, PlatformMinDrop, MaxCorner;
	uint32 ColumnGroup, ColumnEvery, StairEvery, OverhangBays, MinSetbackStoreys;
};

FCSCityFacadeConstants CSCityFacade_Constants(const FFacadeKitShape& Shape)
{
	FCSCityFacadeConstants Out;
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		const FBox& Box = Shape.Bounds[Slot];
		const bool bHas = Shape.bHas[Slot];
		// A kit put together by hand may leave a slot's box out: the counts do not need it.
		const bool bBox = bHas && Box.IsValid;
		Out.SlotMin[Slot] = bBox ? FVector4f(FVector3f(Box.Min), float(Shape.Turns[Slot])) : FVector4f::Zero();
		Out.SlotSize[Slot] = bBox ? FVector4f(FVector3f(Box.GetSize()), 1.0f) : FVector4f(1.0f, 1.0f, 1.0f, bHas ? 1.0f : 0.0f);
	}
	const bool bCorner = Shape.bHas[int32(EFacadeSlot::Corner)];
	Out.StyleCorner[0] = FVector4f(bCorner ? float(Shape.Corner) : 0.0f, bCorner ? float(Shape.CornerDepth) : 0.0f, 0.0f, 0.0f);
	double Widest = bCorner ? FMath::Max(Shape.Corner, Shape.CornerDepth) : 0.0;
	for (int32 Style = 0; Style < FacadeCornerStyles; Style++)
	{
		Out.StyleCorner[Style + 1] = FVector4f(float(Shape.StyleCorner[Style]), float(Shape.StyleDepth[Style]), 0.0f, 0.0f);
		Widest = FMath::Max3(Widest, Shape.StyleCorner[Style], Shape.StyleDepth[Style]);
	}
	Out.MaxCorner = float(Widest);
	Out.ShapeWeights = FVector4f(Shape.ShapeWeights[0], Shape.ShapeWeights[1], Shape.ShapeWeights[2], Shape.ShapeWeights[3]);
	Out.Chances = FVector4f(Shape.ShapeWeights[4], Shape.SetbackChance, Shape.PodiumChance, Shape.OverhangChance);
	Out.Storey = float(Shape.Storey);
	Out.Ground = float(Shape.GroundHeight());
	Out.Parapet = float(Shape.Parapet);
	Out.PlinthSink = float(Shape.PlinthSink);
	Out.WindowChance = float(Shape.WindowChance);
	Out.GroundWindowChance = float(Shape.GroundWindowChance);
	Out.ColumnInset = float(Shape.ColumnInset);
	Out.PlatformDepth = float(Shape.PlatformDepth);
	Out.PlatformMinDrop = float(Shape.PlatformMinDrop);
	Out.ColumnGroup = uint32(Shape.ColumnGroup);
	Out.ColumnEvery = uint32(FMath::Max(Shape.ColumnEvery, 1));
	Out.StairEvery = uint32(FMath::Max(Shape.StairEvery, 1));
	Out.OverhangBays = uint32(FMath::Max(Shape.OverhangBays, 1));
	Out.MinSetbackStoreys = uint32(Shape.MinSetbackStoreys);
	return Out;
}

// One write, from the game thread to the render thread and back.
struct FFacadeJob : public FGpuWrite
{
	virtual void Look(FRHICommandListImmediate& RHICmdList) override;
	virtual void Land() override;

	TArray<FVector4f> BuildingsF;
	TArray<FUintVector4> BuildingsU;
	FCSCityFacadeConstants Constants;
	FVector Origin = FVector::ZeroVector;
	uint32 NumThreads = 0;
	TSharedPtr<FFacadeFieldGpu> Field;
	bool bReadMass = false;
	bool bReadRows = false;

	// Render thread, until it lands.
	FRHIGPUBufferReadback* CountsReadback = nullptr;
	FRHIGPUBufferReadback* MassReadback = nullptr;
	FRHIGPUBufferReadback* RowsReadback[FacadeSlots] = {};
	uint32 Counts[FacadeSlots] = {};
	TArray<FUintVector4> Mass;
	TArray<FVector4f> Rows[FacadeSlots];
	FFacadeGpu::FSlot Landed[FacadeSlots];

	// Game thread, where it lands.
	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<USceneComponent> Parent;
	FString Name;
	bool bFollowParent = false;
	FBox Bounds = FBox(ForceInit);
	TWeakObjectPtr<UStaticMesh> Meshes[FacadeSlots];
	FFacadeComponents Components;
	TFunction<void(TConstArrayView<FFacadeMassing>)> OnMassing;

	virtual ~FFacadeJob() override
	{
		TArray<FRHIGPUBufferReadback*> Readbacks;
		if (CountsReadback) Readbacks.Add(CountsReadback);
		if (MassReadback) Readbacks.Add(MassReadback);
		for (FRHIGPUBufferReadback* Readback : RowsReadback) if (Readback) Readbacks.Add(Readback);
		if (Readbacks.Num() == 0) return;
		ENQUEUE_RENDER_COMMAND(CSCityFacadeReadbacks)([Readbacks = MoveTemp(Readbacks)](FRHICommandListImmediate&) { for (FRHIGPUBufferReadback* Readback : Readbacks) delete Readback; });
	}
};

// What every pass reads: the buildings and the field.
void CSCityFacade_Common(FRDGBuilder& GraphBuilder, const FFacadeJob& Job, FCSCityFacadeCommon& Out)
{
	Out.FacadeBuildingsF = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(GraphBuilder, Job.BuildingsF, TEXT("CSCityFacade.BuildingsF"), false, true).SRV;
	Out.FacadeBuildingsU = CSHelper::CreateUploadedStructuredBuffer<FUintVector4>(GraphBuilder, Job.BuildingsU, TEXT("CSCityFacade.BuildingsU"), false, true).SRV;
	const FFieldBinding Field = BindField(GraphBuilder, Job.Field.Get());
	Out.FacadeFieldTable = Field.Table;
	Out.FacadeFieldPages = Field.Pages;
	Out.FacadeFieldBox = Field.Box;
	Out.FacadeFieldOn = Field.On;
	Out.FacadeOrigin = FVector4f(FVector3f(Job.Origin), 0.0f);
	Out.FacadeNumBuildings = uint32(Job.BuildingsU.Num());
	Out.FacadeNumThreads = Job.NumThreads;
	const FCSCityFacadeConstants& C = Job.Constants;
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		Out.FacadeSlotMin[Slot] = C.SlotMin[Slot];
		Out.FacadeSlotSize[Slot] = C.SlotSize[Slot];
	}
	for (int32 Style = 0; Style <= FacadeCornerStyles; Style++) Out.FacadeStyleCorner[Style] = C.StyleCorner[Style];
	Out.FacadeShapeWeights = C.ShapeWeights;
	Out.FacadeChances = C.Chances;
	Out.FacadeStorey = C.Storey;
	Out.FacadeGround = C.Ground;
	Out.FacadeParapet = C.Parapet;
	Out.FacadePlinthSink = C.PlinthSink;
	Out.FacadeWindowChance = C.WindowChance;
	Out.FacadeGroundWindowChance = C.GroundWindowChance;
	Out.FacadeColumnInset = C.ColumnInset;
	Out.FacadePlatformDepth = C.PlatformDepth;
	Out.FacadePlatformMinDrop = C.PlatformMinDrop;
	Out.FacadeMaxCorner = C.MaxCorner;
	Out.FacadeColumnGroup = C.ColumnGroup;
	Out.FacadeColumnEvery = C.ColumnEvery;
	Out.FacadeStairEvery = C.StairEvery;
	Out.FacadeOverhangBays = C.OverhangBays;
	Out.FacadeMinSetbackStoreys = C.MinSetbackStoreys;
}

FRDGBufferRef CSCityFacade_AddMass(FRDGBuilder& GraphBuilder, const FCSCityFacadeCommon& Common, const FFacadeJob& Job)
{
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FUintVector4), uint32(Job.BuildingsU.Num()) * 4u);
	Desc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef Mass = GraphBuilder.CreateBuffer(Desc, TEXT("CSCityFacade.Mass"));
	FCSCityFacadeMassCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityFacadeMassCS::FParameters>();
	Params->Common = Common;
	Params->FacadeMassOut = GraphBuilder.CreateUAV(Mass);
	TShaderMapRef<FCSCityFacadeMassCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityFacade.Mass"), Shader, Params, FComputeShaderUtils::GetGroupCount(Job.BuildingsU.Num(), 64));
	return Mass;
}

// The buildings' massing and how many modules each slot gets, on their way back.
void CSCityFacade_RunCount(FRHICommandListImmediate& RHICmdList, FFacadeJob& Job)
{
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityFacade.Count"));
	FCSCityFacadeCommon Common;
	CSCityFacade_Common(GraphBuilder, Job, Common);
	FRDGBufferRef Mass = CSCityFacade_AddMass(GraphBuilder, Common, Job);
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FacadeSlots);
	Desc.Usage |= EBufferUsageFlags::SourceCopy;
	FRDGBufferRef Counts = GraphBuilder.CreateBuffer(Desc, TEXT("CSCityFacade.Counts"));
	FRDGBufferUAVRef CountsUAV = GraphBuilder.CreateUAV(Counts);
	AddClearUAVPass(GraphBuilder, CountsUAV, 0u);
	FCSCityFacadeCountCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityFacadeCountCS::FParameters>();
	Params->Common = Common;
	Params->FacadeMass = GraphBuilder.CreateSRV(Mass);
	Params->FacadeCounts = CountsUAV;
	TShaderMapRef<FCSCityFacadeCountCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityFacade.Count"), Shader, Params, FComputeShaderUtils::GetGroupCount(int32(Job.NumThreads), 64));
	Job.CountsReadback = new FRHIGPUBufferReadback(TEXT("CSCityFacade.CountsReadback"));
	AddEnqueueCopyPass(GraphBuilder, Job.CountsReadback, Counts, sizeof(uint32) * FacadeSlots);
	if (Job.bReadMass)
	{
		Job.MassReadback = new FRHIGPUBufferReadback(TEXT("CSCityFacade.MassReadback"));
		AddEnqueueCopyPass(GraphBuilder, Job.MassReadback, Mass, sizeof(FUintVector4) * 4u * uint32(Job.BuildingsU.Num()));
	}
	GraphBuilder.Execute();
}

// The counts are back: each slot's buffers grown or let go, and the modules written. The massing is drawn again: the seeds and the
// field are the same, so that is what was counted, and if the field has changed a later write is on its way and the capacity holds.
void CSCityFacade_RunModules(FRHICommandListImmediate& RHICmdList, FFacadeJob& Job)
{
	FFacadeGpu& Gpu = *Job.Gpu;
	FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityFacade.Modules"));
	TArray<int32> Used;
	FRDGBufferRef Rows[FacadeSlots] = {};
	FRDGBufferRef Counters[FacadeSlots] = {};
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		FFacadeGpu::FSlot& One = Gpu.Slots[Slot];
		GrowSlot(One, Job.Counts[Slot]);
		if (Job.Counts[Slot] == 0) continue;
		Used.Add(Slot);
		Rows[Slot] = GraphBuilder.RegisterExternalBuffer(One.Rows, TEXT("CSCityFacade.Rows"));
		Counters[Slot] = GraphBuilder.RegisterExternalBuffer(One.Counter, TEXT("CSCityFacade.Counter"));
		// The component never clears it: a write starts from none.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counters[Slot], PF_R32_UINT)), 0u);
	}
	if (Used.Num() > 0)
	{
		FCSCityFacadeCommon Common;
		CSCityFacade_Common(GraphBuilder, Job, Common);
		FRDGBufferSRVRef MassSRV = GraphBuilder.CreateSRV(CSCityFacade_AddMass(GraphBuilder, Common, Job));
		TShaderMapRef<FCSCityFacadeModulesCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		// A pass short of slots binds a scratch buffer pair in the rest (cleared, never written: no slot maps there).
		FRDGBufferRef SpareRows = nullptr;
		FRDGBufferRef SpareCounter = nullptr;
		for (int32 First = 0; First < Used.Num(); First += FCSCityFacadeModulesCS::SlotsPerPass)
		{
			const int32 Count = FMath::Min(FCSCityFacadeModulesCS::SlotsPerPass, Used.Num() - First);
			FCSCityFacadeModulesCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityFacadeModulesCS::FParameters>();
			Params->Common = Common;
			Params->FacadeMass = MassSRV;
			uint32 Ids[FCSCityFacadeModulesCS::SlotsPerPass] = { MAX_uint32, MAX_uint32, MAX_uint32, MAX_uint32, MAX_uint32 };
			uint32 Capacity[FCSCityFacadeModulesCS::SlotsPerPass] = {};
			FRDGBufferUAVRef* RowUAVs[FCSCityFacadeModulesCS::SlotsPerPass] = { &Params->FacadeRows0, &Params->FacadeRows1, &Params->FacadeRows2, &Params->FacadeRows3, &Params->FacadeRows4 };
			FRDGBufferUAVRef* CounterUAVs[FCSCityFacadeModulesCS::SlotsPerPass] = { &Params->FacadeCounter0, &Params->FacadeCounter1, &Params->FacadeCounter2, &Params->FacadeCounter3, &Params->FacadeCounter4 };
			for (int32 Local = 0; Local < FCSCityFacadeModulesCS::SlotsPerPass; Local++)
			{
				FRDGBufferRef RowBuffer;
				FRDGBufferRef CounterBuffer;
				if (Local < Count)
				{
					const int32 Slot = Used[First + Local];
					RowBuffer = Rows[Slot];
					CounterBuffer = Counters[Slot];
					Ids[Local] = uint32(Slot);
					Capacity[Local] = Gpu.Slots[Slot].Capacity;
				}
				else
				{
					if (!SpareRows)
					{
						SpareRows = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSCityFacade.SpareRows"));
						SpareCounter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSCityFacade.SpareCounter"));
						AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpareRows, PF_A32B32G32R32F)), 0.0f);
						AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpareCounter, PF_R32_UINT)), 0u);
					}
					RowBuffer = SpareRows;
					CounterBuffer = SpareCounter;
				}
				*RowUAVs[Local] = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RowBuffer, PF_A32B32G32R32F));
				*CounterUAVs[Local] = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterBuffer, PF_R32_UINT));
			}
			Params->FacadeSlotIdsLo = FUintVector4(Ids[0], Ids[1], Ids[2], Ids[3]);
			Params->FacadeSlotIdsHi = Ids[4];
			Params->FacadeCapacityLo = FUintVector4(Capacity[0], Capacity[1], Capacity[2], Capacity[3]);
			Params->FacadeCapacityHi = Capacity[4];
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityFacade.Modules %d of %d slots", Count, Used.Num()), Shader, Params, FComputeShaderUtils::GetGroupCount(int32(Job.NumThreads), 64));
		}
	}
	for (const int32 Slot : Used)
	{
		if (Job.bReadRows)
		{
			Job.RowsReadback[Slot] = new FRHIGPUBufferReadback(TEXT("CSCityFacade.RowsReadback"));
			AddEnqueueCopyPass(GraphBuilder, Job.RowsReadback[Slot], Rows[Slot], Job.Counts[Slot] * CS_GPU_INSTANCED_ROW_FLOAT4S * sizeof(FVector4f));
		}
		// The instanced component's passes only read them, and leave their state to whoever wrote them.
		GraphBuilder.SetBufferAccessFinal(Rows[Slot], ERHIAccess::SRVMask);
		GraphBuilder.SetBufferAccessFinal(Counters[Slot], ERHIAccess::SRVMask);
	}
	GraphBuilder.Execute();
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++) Job.Landed[Slot] = Gpu.Slots[Slot];
}

// On the render thread: the counts, if they are back, and on with the write.
void FFacadeJob::Look(FRHICommandListImmediate& RHICmdList)
{
	FFacadeJob& Job = *this;
	if (Job.State.load(std::memory_order_relaxed) != 0) return;
	if (Job.NumThreads > 0)
	{
		if (!Job.CountsReadback || !Job.CountsReadback->IsReady() || (Job.MassReadback && !Job.MassReadback->IsReady())) return;
		const void* CountsBack = Job.CountsReadback->Lock(sizeof(uint32) * FacadeSlots);
		if (!CountsBack)
		{
			Job.State.store(2, std::memory_order_release);
			return;
		}
		FMemory::Memcpy(Job.Counts, CountsBack, sizeof(uint32) * FacadeSlots);
		Job.CountsReadback->Unlock();
		if (Job.MassReadback)
		{
			const uint32 Bytes = sizeof(FUintVector4) * 4u * uint32(Job.BuildingsU.Num());
			if (const void* MassBack = Job.MassReadback->Lock(Bytes))
			{
				Job.Mass.SetNumUninitialized(Job.BuildingsU.Num() * 4);
				FMemory::Memcpy(Job.Mass.GetData(), MassBack, Bytes);
				Job.MassReadback->Unlock();
			}
		}
	}
	if (Job.Serial != Job.Gpu->Latest.load(std::memory_order_acquire))
	{
		Job.State.store(2, std::memory_order_release);
		return;
	}
	CSCityFacade_RunModules(RHICmdList, Job);
	Job.State.store(1, std::memory_order_release);
}

// A building's massing from its 4 rows.
FFacadeMassing CSCityFacade_Massing(const FUintVector4* Rows)
{
	FFacadeMassing Out;
	Out.GridX = int32(Rows[0].X & 127u);
	Out.GridY = int32((Rows[0].X >> 7) & 127u);
	Out.Shape = ECSCityMassShape((Rows[0].X >> 14) & 7u);
	Out.Stack = ECSCityMassStack((Rows[0].X >> 17) & 7u);
	const int32 Tiers = int32((Rows[0].X >> 20) & 3u);
	Out.Storeys = int32(Rows[0].Y);
	Out.OverhangFaces = Rows[0].Z & 15u;
	Out.OverhangDepth = int32(Rows[0].Z >> 4);
	auto In = [](uint32 Rect, int32 I, int32 J) { return I >= int32(Rect & 255u) && I < int32((Rect >> 8) & 255u) && J >= int32((Rect >> 16) & 255u) && J < int32(Rect >> 24); };
	const uint32 Clips[3] = { Rows[2].X, Rows[2].Z, Rows[3].X };
	const uint32 Ranges[3] = { Rows[2].Y, Rows[2].W, Rows[3].Y };
	for (int32 T = 0; T < Tiers; T++)
	{
		FFacadeTier& Tier = Out.Tiers.AddDefaulted_GetRef();
		Tier.First = int32(Ranges[T] & 0xFFFFu);
		Tier.End = int32(Ranges[T] >> 16);
		Tier.Cells.SetNumZeroed(Out.GridX * Out.GridY);
		for (int32 J = 0; J < Out.GridY; J++) for (int32 I = 0; I < Out.GridX; I++) Tier.Cells[J * Out.GridX + I] = In(Clips[T], I, J) && !In(Rows[1].X, I, J) && !In(Rows[1].Y, I, J) ? 1 : 0;
	}
	return Out;
}

// A write's inputs: the kit's numbers and each building with a box, a thread per bay.
void CSCityFacade_Inputs(const FFacadeKitShape& Shape, TConstArrayView<FFacadeBuilding> Buildings, const FVector& Origin, FFacadeJob& Job)
{
	Job.Constants = CSCityFacade_Constants(Shape);
	Job.Origin = Origin;
	Job.BuildingsF.Reserve(Buildings.Num() * 3);
	Job.BuildingsU.Reserve(Buildings.Num());
	for (const FFacadeBuilding& Building : Buildings)
	{
		const FBlock& Block = Building.Block;
		if (Block.Width <= 0.0 || Block.Depth <= 0.0 || Block.Height <= 0.0) continue;
		const int32 NX = FMath::Clamp(FMath::RoundToInt32(Block.Width / Shape.Bay), 1, 64);
		const int32 NY = FMath::Clamp(FMath::RoundToInt32(Block.Depth / Shape.Bay), 1, 64);
		const int32 S = FMath::Clamp(FMath::RoundToInt32(Block.Height / Shape.Storey), 1, 0xFFFF);
		const FVector Base = Block.Base - Origin;
		const FVector2D Front = Block.Front.GetSafeNormal();
		Job.BuildingsF.Add(FVector4f(float(Base.X), float(Base.Y), float(Base.Z), float(Building.Drop)));
		Job.BuildingsF.Add(FVector4f(float(Front.X), float(Front.Y), float(Building.DropX), float(Building.DropY)));
		Job.BuildingsF.Add(FVector4f(float(Block.Width), float(Block.Depth), float(Building.FrontRoom), float(Building.DoorAt)));
		const uint32 Asked = uint32(Building.Shape) | (uint32(Building.Stack) << 4) | (uint32(Building.Overhang) << 8) | (Building.bLockMassing ? 1u << 12 : 0u) | (uint32(Building.CornerStyle) << 13);
		Job.BuildingsU.Add(FUintVector4(Building.Seed, uint32(NX) | (uint32(NY) << 8) | (uint32(S) << 16), Asked, Job.NumThreads));
		Job.NumThreads += uint32(NX * NY);
	}
}

// Writes on their way, and whether the ticker is looking at them.
TArray<TSharedRef<FGpuWrite>> GGpuWrites;
bool GGpuWritesTicking = false;
std::atomic<bool> GGpuWritesLooking { false };

// Game thread: the components take what landed.
void FFacadeJob::Land()
{
	FFacadeJob& Job = *this;
	if (Job.Serial != Job.Gpu->Latest.load(std::memory_order_acquire)) return;
	AActor* Target = Job.Owner.Get();
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>* Held = Job.Components ? Job.Components() : nullptr;
	if (!IsValid(Target) || !Held) return;
	Held->SetNum(FacadeSlots);
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++) LandSlot(*Target, Job.Parent.Get(), Job.Name, Job.bFollowParent, Job.Origin, Job.Bounds, Job.Meshes[Slot].Get(), Job.Counts[Slot], Job.Landed[Slot], (*Held)[Slot]);
	if (Job.OnMassing && Job.Mass.Num() > 0)
	{
		TArray<FFacadeMassing> Massing;
		for (int32 Row = 0; Row + 4 <= Job.Mass.Num(); Row += 4) Massing.Add(CSCityFacade_Massing(&Job.Mass[Row]));
		Job.OnMassing(Massing);
	}
}

// Game thread: those that have landed or come to nothing, off the list.
void CSCityGpuWrites_Finish()
{
	for (int32 I = 0; I < GGpuWrites.Num();)
	{
		const TSharedRef<FGpuWrite> Write = GGpuWrites[I];
		const int32 State = Write->State.load(std::memory_order_acquire);
		if (State == 0)
		{
			I++;
			continue;
		}
		GGpuWrites.RemoveAt(I);
		Write->Gpu->InFlight--;
		CSRoad::AddWriteInFlight(-1);
		if (State == 1) Write->Land();
	}
}

void CSCityGpuWrites_Look()
{
	if (GGpuWrites.Num() == 0 || GGpuWritesLooking.exchange(true)) return;
	ENQUEUE_RENDER_COMMAND(CSCityGpuWritesLook)([Writes = GGpuWrites](FRHICommandListImmediate& RHICmdList)
	{
		GGpuWritesLooking.store(false);
		for (const TSharedRef<FGpuWrite>& Write : Writes) Write->Look(RHICmdList);
	});
}

// While LandMeshWrites waits: the GPU done with everything queued, a look at the counts, then what landed.
void CSCityGpuWrites_Pump()
{
	if (GGpuWrites.Num() == 0) return;
	ENQUEUE_RENDER_COMMAND(CSCityGpuWritesWait)([](FRHICommandListImmediate& RHICmdList) { RHICmdList.SubmitAndBlockUntilGPUIdle(); });
	CSCityGpuWrites_Look();
	FlushRenderingCommands();
	CSCityGpuWrites_Finish();
}
}

void AddGpuWrite(const TSharedRef<FGpuWrite>& Write)
{
	GGpuWrites.Add(Write);
	Write->Gpu->InFlight++;
	CSRoad::AddWriteInFlight(1);
	CSRoad::SetLandPump(&CSCityGpuWrites_Pump);
	if (GGpuWritesTicking) return;
	GGpuWritesTicking = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		CSCityGpuWrites_Finish();
		CSCityGpuWrites_Look();
		GGpuWritesTicking = GGpuWrites.Num() > 0;
		return GGpuWritesTicking;
	}));
}

FFacadeKitShape KitShape(const UCSCityFacadeKit& Kit)
{
	FFacadeKitShape Shape;
	Shape.Bay = FMath::Max(double(Kit.BayWidth), 50.0);
	Shape.Storey = FMath::Max(double(Kit.StoreyHeight), 100.0);
	Shape.Ground = Kit.GroundStoreyHeight > 0.0f ? FMath::Max(double(Kit.GroundStoreyHeight), 100.0) : 0.0;
	Shape.WindowChance = FMath::Clamp(double(Kit.WindowChance), 0.0, 1.0);
	Shape.GroundWindowChance = FMath::Clamp(double(Kit.GroundWindowChance), 0.0, 1.0);
	Shape.ColumnGroup = FMath::Clamp(Kit.ColumnGroup, 1, 8);
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		const FCSCityFacadeModule& Module = Kit.Module(Slot);
		const FBox Box = Module.Mesh ? Module.Mesh->GetBoundingBox() : FBox(ForceInit);
		if (!Box.IsValid) continue;
		Shape.bHas[Slot] = true;
		Shape.Turns[Slot] = Module.QuarterTurns & 3;
		Shape.Bounds[Slot] = CSCityFacade_Turned(Box, Shape.Turns[Slot]);
	}
	if (Shape.bHas[int32(EFacadeSlot::Corner)])
	{
		const FVector Size = Shape.Bounds[int32(EFacadeSlot::Corner)].GetSize();
		Shape.Corner = Size.X;
		Shape.CornerDepth = Size.Y;
	}
	for (int32 Style = 0; Style < FacadeCornerStyles; Style++)
	{
		const int32 Slot = FacadeBaseSlots + Style * 4;
		if (!Shape.bHas[Slot]) continue;
		const FVector Size = Shape.Bounds[Slot].GetSize();
		Shape.StyleCorner[Style] = Size.X;
		Shape.StyleDepth[Style] = Size.Y;
	}
	if (Shape.bHas[int32(EFacadeSlot::Parapet)]) Shape.Parapet = Shape.Bounds[int32(EFacadeSlot::Parapet)].GetSize().Z;
	Shape.OverhangChance = FMath::Clamp(Kit.OverhangChance, 0.0f, 1.0f);
	Shape.OverhangBays = FMath::Clamp(Kit.OverhangBays, 1, 4);
	Shape.ColumnEvery = FMath::Clamp(Kit.ColumnEvery, 1, 16);
	Shape.ColumnInset = FMath::Max(double(Kit.ColumnInset), 0.0);
	Shape.PlinthSink = Shape.bHas[int32(EFacadeSlot::Plinth)] ? FMath::Max(double(Kit.PlinthSink), 0.0) : 0.0;
	Shape.PlatformDepth = FMath::Max(double(Kit.PlatformDepth), 0.0);
	Shape.PlatformMinDrop = FMath::Max(double(Kit.PlatformMinDrop), 0.0);
	Shape.StairEvery = FMath::Clamp(Kit.StairEvery, 1, 32);
	const float Weights[5] = { Kit.BoxWeight, Kit.LWeight, Kit.UWeight, Kit.TWeight, Kit.CourtyardWeight };
	for (int32 K = 0; K < 5; K++) Shape.ShapeWeights[K] = FMath::Max(Weights[K], 0.0f);
	Shape.SetbackChance = FMath::Clamp(Kit.SetbackChance, 0.0f, 1.0f);
	Shape.PodiumChance = FMath::Clamp(Kit.PodiumChance, 0.0f, 1.0f);
	Shape.MinSetbackStoreys = FMath::Max(Kit.MinSetbackStoreys, 2);
	return Shape;
}

uint64 KitHash(const UCSCityFacadeKit& Kit)
{
	const FFacadeKitShape Shape = KitShape(Kit);
	FXxHash64Builder Hash;
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		const UStaticMesh* Mesh = Shape.bHas[Slot] ? Kit.Module(Slot).Mesh.Get() : nullptr;
		Hash.Update(&Mesh, sizeof(Mesh));
		Hash.Update(&Shape.Turns[Slot], sizeof(int32));
		if (Shape.bHas[Slot]) Hash.Update(&Shape.Bounds[Slot], sizeof(FBox));
	}
	for (const double Value : { Shape.Bay, Shape.Storey, Shape.Ground, Shape.WindowChance, Shape.GroundWindowChance, Shape.ColumnInset, Shape.PlinthSink, Shape.PlatformDepth, Shape.PlatformMinDrop }) Hash.Update(&Value, sizeof(Value));
	Hash.Update(&Shape.ColumnGroup, sizeof(Shape.ColumnGroup));
	Hash.Update(Shape.ShapeWeights, sizeof(Shape.ShapeWeights));
	for (const float Value : { Shape.SetbackChance, Shape.PodiumChance, Shape.OverhangChance }) Hash.Update(&Value, sizeof(Value));
	for (const int32 Value : { Shape.MinSetbackStoreys, Shape.OverhangBays, Shape.ColumnEvery, Shape.StairEvery }) Hash.Update(&Value, sizeof(Value));
	return Hash.Finalize().Hash;
}

FFacadeBuilding LotBuilding(const FLot& Lot)
{
	FFacadeBuilding Building;
	Building.Block = LotBlock(Lot);
	Building.Seed = uint32(Lot.Id ^ (Lot.Id >> 32));
	Building.Drop = Lot.Drop;
	Building.DropX = Lot.DropX;
	Building.DropY = Lot.DropY;
	Building.FrontRoom = Lot.FrontRoom;
	return Building;
}

uint32 FFacadeLayout::Modules() const
{
	uint32 Sum = 0;
	for (const uint32 Count : Counts) Sum += Count;
	return Sum;
}

FFacadeLayout LayoutFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building, const TSharedPtr<FFacadeFieldGpu>& Field)
{
	FFacadeLayout Out;
	const TSharedRef<FFacadeJob> Job = MakeShared<FFacadeJob>();
	CSCityFacade_Inputs(Kit, MakeArrayView(&Building, 1), FVector::ZeroVector, *Job);
	if (Job->NumThreads == 0) return Out;
	Job->Field = Field;
	Job->Gpu = MakeShared<FFacadeGpu>();
	Job->Serial = 1;
	Job->Gpu->Latest = 1;
	Job->bReadMass = true;
	Job->bReadRows = true;
	ENQUEUE_RENDER_COMMAND(CSCityFacadeLayout)([Job](FRHICommandListImmediate& RHICmdList)
	{
		CSCityFacade_RunCount(RHICmdList, *Job);
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		for (int32 Round = 0; Round < 1000 && Job->State.load() == 0; Round++)
		{
			Job->Look(RHICmdList);
			if (Job->State.load() == 0) FPlatformProcess::Sleep(0.001f);
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
		{
			FRHIGPUBufferReadback* Readback = Job->RowsReadback[Slot];
			if (!Readback) continue;
			for (int32 Round = 0; Round < 1000 && !Readback->IsReady(); Round++) FPlatformProcess::Sleep(0.001f);
			const uint32 Rows = Job->Counts[Slot] * CS_GPU_INSTANCED_ROW_FLOAT4S;
			if (const void* Data = Readback->Lock(Rows * sizeof(FVector4f)))
			{
				Job->Rows[Slot].SetNumUninitialized(int32(Rows));
				FMemory::Memcpy(Job->Rows[Slot].GetData(), Data, Rows * sizeof(FVector4f));
				Readback->Unlock();
			}
		}
	});
	FlushRenderingCommands();
	if (Job->Mass.Num() >= 4) Out.Massing = CSCityFacade_Massing(Job->Mass.GetData());
	const FBlock& Block = Building.Block;
	const FVector2D Front = Block.Front.GetSafeNormal();
	const FVector2D AxisX(Front.Y, -Front.X);
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		Out.Counts[Slot] = Job->Counts[Slot];
		const TArray<FVector4f>& Rows = Job->Rows[Slot];
		const FBox& Bounds = Kit.Bounds[Slot];
		for (int32 Row = 0; Row + CS_GPU_INSTANCED_ROW_FLOAT4S <= Rows.Num(); Row += CS_GPU_INSTANCED_ROW_FLOAT4S)
		{
			const FVector X(Rows[Row]), Y(Rows[Row + 1]), Z(Rows[Row + 2]), O(Rows[Row + 3]);
			FBox Box(ForceInit);
			for (int32 Corner = 0; Corner < 8; Corner++)
			{
				const FVector M((Corner & 1) ? Bounds.Max.X : Bounds.Min.X, (Corner & 2) ? Bounds.Max.Y : Bounds.Min.Y, (Corner & 4) ? Bounds.Max.Z : Bounds.Min.Z);
				const FVector P = X * M.X + Y * M.Y + Z * M.Z + O - Block.Base;
				Box += FVector(FVector2D(P).Dot(AxisX), FVector2D(P).Dot(Front), P.Z);
			}
			Out.Boxes[Slot].Add(Box);
		}
	}
	auto Count = [&Out](EFacadeSlot Slot) { return Out.Counts[int32(Slot)]; };
	Out.Doors = Count(EFacadeSlot::Door);
	Out.GroundCells = Count(EFacadeSlot::GroundWall) + Count(EFacadeSlot::GroundWindow) + Out.Doors;
	Out.FacadeCells = Count(EFacadeSlot::Wall) + Count(EFacadeSlot::Window) + Out.GroundCells;
	Out.Corners = Count(EFacadeSlot::Corner);
	Out.InnerCorners = Count(EFacadeSlot::CornerInner);
	Out.Parapets = Count(EFacadeSlot::Parapet);
	Out.Roofs = Count(EFacadeSlot::Roof);
	Out.RoofCorners = Count(EFacadeSlot::RoofCorner);
	Out.ParapetCorners = Count(EFacadeSlot::ParapetCorner);
	Out.Soffits = Count(EFacadeSlot::Soffit);
	Out.Columns = Count(EFacadeSlot::Column);
	Out.Plinths = Count(EFacadeSlot::Plinth);
	Out.PlinthCorners = Count(EFacadeSlot::PlinthCorner);
	Out.Platforms = Count(EFacadeSlot::Platform);
	Out.Stairs = Count(EFacadeSlot::Stair);
	if (Out.Boxes[int32(EFacadeSlot::Door)].Num() > 0 && Out.Massing.GridX > 0)
	{
		const double Middle = Out.Boxes[int32(EFacadeSlot::Door)][0].GetCenter().X;
		Out.DoorCell = FMath::Clamp(FMath::FloorToInt32((Middle + 0.5 * Block.Width) / (Block.Width / Out.Massing.GridX)), 0, Out.Massing.GridX - 1);
	}
	double Far = TNumericLimits<double>::Lowest();
	for (const FBox& Box : Out.Boxes[int32(EFacadeSlot::Platform)]) Far = FMath::Max(Far, Box.Max.Y);
	if (Out.Platforms > 0)
	{
		const double Rim = Kit.bHas[int32(EFacadeSlot::Plinth)] && Kit.Bounds[int32(EFacadeSlot::Plinth)].IsValid ? Kit.Bounds[int32(EFacadeSlot::Plinth)].GetSize().Y : 0.0;
		Out.PlatformDepth = Far + Rim - 0.5 * Block.Depth;
	}
	return Out;
}

FFacadeMassing MassFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building, const TSharedPtr<FFacadeFieldGpu>& Field)
{
	return LayoutFacade(Kit, Building, Field).Massing;
}

bool FacadeWritePending(const TSharedPtr<FFacadeGpu>& Gpu)
{
	return Gpu && Gpu->InFlight > 0;
}

void WriteFacade(AActor& Owner, USceneComponent* Parent, const FString& Name, const FVector& Origin, const UCSCityFacadeKit& Kit, TConstArrayView<FFacadeBuilding> Buildings,
	FFacadeComponents Components, TSharedPtr<FFacadeGpu>& Gpu, bool bFollowParent, TFunction<void(TConstArrayView<FFacadeMassing>)> OnMassing)
{
	if (!Gpu) Gpu = MakeShared<FFacadeGpu>();
	const FFacadeKitShape Shape = KitShape(Kit);
	const TSharedRef<FFacadeJob> Job = MakeShared<FFacadeJob>();
	CSCityFacade_Inputs(Shape, Buildings, Origin, *Job);
	if (const ACSCityFieldActor* Field = ACSCityFieldActor::Find(Owner.GetWorld())) Job->Field = Field->GpuField();
	Job->Gpu = Gpu;
	Job->Serial = Gpu->Latest.fetch_add(1, std::memory_order_acq_rel) + 1u;
	Job->bReadMass = bool(OnMassing);
	Job->Owner = &Owner;
	Job->Parent = Parent;
	Job->Name = Name;
	Job->bFollowParent = bFollowParent;
	Job->Components = MoveTemp(Components);
	Job->OnMassing = MoveTemp(OnMassing);
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++) if (Shape.bHas[Slot]) Job->Meshes[Slot] = Kit.Module(Slot).Mesh.Get();

	// The boxes they may fill: the parapet over the top and the modules' thickness round them, the platform in front, and under the
	// base as far as the plane of the ground goes over the box and the room in front, and the sink below that.
	const bool bPlatform = Shape.bHas[int32(EFacadeSlot::Platform)];
	for (const FFacadeBuilding& Building : Buildings)
	{
		const FBlock& Block = Building.Block;
		if (Block.Width <= 0.0 || Block.Depth <= 0.0 || Block.Height <= 0.0) continue;
		const FVector Base = Block.Base - Origin;
		const FVector2D Front = Block.Front.GetSafeNormal();
		const FVector2D Half = FVector2D(FMath::Abs(Front.Y) * Block.Width + FMath::Abs(Front.X) * Block.Depth, FMath::Abs(Front.X) * Block.Width + FMath::Abs(Front.Y) * Block.Depth) * 0.5 + FVector2D(bPlatform ? Building.FrontRoom : 0.0);
		const double Top = Shape.StoreyBase(FMath::Max(FMath::RoundToInt32(Block.Height / Shape.Storey), 1)) + Shape.Parapet;
		const double Under = FMath::Abs(Building.Drop) + FMath::Abs(Building.DropX) * Block.Width + FMath::Abs(Building.DropY) * (Block.Depth + Building.FrontRoom) + Shape.PlinthSink;
		Job->Bounds += FBox(FVector(Base.X - Half.X, Base.Y - Half.Y, Base.Z - Under), FVector(Base.X + Half.X, Base.Y + Half.Y, Base.Z + Top));
	}
	if (Job->Bounds.IsValid) Job->Bounds = Job->Bounds.ExpandBy(200.0);

	if (Job->NumThreads > 0)
	{
		ENQUEUE_RENDER_COMMAND(CSCityFacadeCount)([Job](FRHICommandListImmediate& RHICmdList) { CSCityFacade_RunCount(RHICmdList, *Job); });
	}
	AddGpuWrite(Job);
}

void ReleaseFacade(TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu)
{
	for (UCSGpuInstancedMeshComponent* Component : Components) if (Component) Component->DestroyComponent();
	Components.Reset();
	if (!Gpu) return;
	Gpu->Latest.fetch_add(1, std::memory_order_acq_rel);
	// The buffers go on the render thread, after any write still to run there.
	ENQUEUE_RENDER_COMMAND(CSCityFacadeRelease)([Released = MoveTemp(Gpu)](FRHICommandListImmediate&) mutable { Released.Reset(); });
	Gpu.Reset();
}
}

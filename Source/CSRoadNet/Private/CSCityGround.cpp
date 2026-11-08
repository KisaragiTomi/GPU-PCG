#include "CSCityGround.h"

#include "ComputeShaderGenerateHelper.h"
#include "CSCityFieldActor.h"
#include "CSCityGpuWrite.h"
#include "CSGpuInstancedMeshComponent.h"
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

UCSCityGroundKit* UCSCityGroundKit::Default()
{
	return LoadObject<UCSCityGroundKit>(nullptr, TEXT("/PCGPlugins/CSRoadNet/Houses/NCSimple/DA_NCSimple_Ground.DA_NCSimple_Ground"));
}

BEGIN_SHADER_PARAMETER_STRUCT(FCSCityGroundCommon, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundRoads)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundLots)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GroundFieldTable)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GroundFieldPages)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, GroundFieldHeights)
	SHADER_PARAMETER(FIntVector4, GroundFieldBox)
	SHADER_PARAMETER(uint32, GroundFieldOn)
	SHADER_PARAMETER(uint32, GroundFieldHeightsOn)
	SHADER_PARAMETER(FVector4f, GroundOrigin)
	SHADER_PARAMETER(FVector4f, GroundTile)
	SHADER_PARAMETER(uint32, GroundNumX)
	SHADER_PARAMETER(uint32, GroundNumRoads)
	SHADER_PARAMETER(uint32, GroundNumLots)
	SHADER_PARAMETER(uint32, GroundNumThreads)
	SHADER_PARAMETER_ARRAY(FVector4f, GroundSlotMin, [CSCity::GroundSlots])
	SHADER_PARAMETER_ARRAY(FVector4f, GroundSlotSize, [CSCity::GroundSlots])
	SHADER_PARAMETER(float, GroundReach)
	SHADER_PARAMETER(float, GroundEdgeSink)
	SHADER_PARAMETER(float, GroundRoadMargin)
	SHADER_PARAMETER(float, GroundRoadShoulder)
	SHADER_PARAMETER(float, GroundPlotBlend)
	SHADER_PARAMETER(float, GroundMaxSlope)
	SHADER_PARAMETER(float, GroundWallMinStep)
	SHADER_PARAMETER(float, GroundBuildingClearance)
	SHADER_PARAMETER(float, GroundRoadClearance)
	SHADER_PARAMETER(float, GroundMaxPropSlope)
	SHADER_PARAMETER(float, GroundAutoPlanterChance)
	SHADER_PARAMETER(float, GroundPlanterChance)
	SHADER_PARAMETER(float, GroundCartChance)
END_SHADER_PARAMETER_STRUCT()

class FCSCityGroundCountCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityGroundCountCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityGroundCountCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCSCityGroundCommon, Common)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, GroundCounts)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GROUND_PASS"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSCityGroundCountCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityGround.usf", "GroundCellCS", SF_Compute);

class FCSCityGroundWriteCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityGroundWriteCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityGroundWriteCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FCSCityGroundCommon, Common)
		SHADER_PARAMETER(FUintVector4, GroundCapacityLo)
		SHADER_PARAMETER(FUintVector4, GroundCapacityHi)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows3)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, GroundRows5)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter0)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter1)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter2)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter3)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter4)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, GroundCounter5)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("GROUND_PASS"), 2);
	}
};
IMPLEMENT_GLOBAL_SHADER(FCSCityGroundWriteCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityGround.usf", "GroundCellCS", SF_Compute);

namespace CSCity
{
namespace
{
const FCSCityFacadeModule& CSCityGround_Module(const UCSCityGroundKit& Kit, int32 Slot)
{
	switch (EGroundSlot(Slot))
	{
	case EGroundSlot::Wall: return Kit.Wall;
	case EGroundSlot::Planter: return Kit.Planter;
	case EGroundSlot::Planter2: return Kit.Planter2;
	case EGroundSlot::CartShop: return Kit.CartShop;
	case EGroundSlot::CartShop2: return Kit.CartShop2;
	default: return Kit.Slab;
	}
}

FBox CSCityGround_Turned(const FBox& Box, int32 Turns)
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

constexpr double CSCityGround_CellCm = 1200.0;

// One write of a tile, from the game thread to the render thread and back.
struct FGroundJob : public FGpuWrite
{
	FGroundKitShape Shape;
	FVector Origin = FVector::ZeroVector;
	FVector2f TileMin = FVector2f::ZeroVector;
	float TileSize = 0.0f;
	uint32 NumX = 0;
	TArray<FVector4f> Roads;
	TArray<FVector4f> Lots;
	TSharedPtr<FFacadeFieldGpu> Field;
	bool bReadRows = false;

	FRHIGPUBufferReadback* CountsReadback = nullptr;
	FRHIGPUBufferReadback* RowsReadback[GroundSlots] = {};
	uint32 Counts[GroundSlots] = {};
	TArray<FVector4f> Rows[GroundSlots];
	FFacadeGpu::FSlot Landed[GroundSlots];

	TWeakObjectPtr<AActor> Owner;
	TWeakObjectPtr<USceneComponent> Parent;
	FString Name;
	FBox Bounds = FBox(ForceInit);
	TWeakObjectPtr<UStaticMesh> Meshes[GroundSlots];
	FFacadeComponents Components;

	virtual ~FGroundJob() override
	{
		TArray<FRHIGPUBufferReadback*> Readbacks;
		if (CountsReadback) Readbacks.Add(CountsReadback);
		for (FRHIGPUBufferReadback* Readback : RowsReadback) if (Readback) Readbacks.Add(Readback);
		if (Readbacks.Num() == 0) return;
		ENQUEUE_RENDER_COMMAND(CSCityGroundReadbacks)([Readbacks = MoveTemp(Readbacks)](FRHICommandListImmediate&) { for (FRHIGPUBufferReadback* Readback : Readbacks) delete Readback; });
	}

	uint32 Threads() const { return NumX * NumX; }

	void FillCommon(FRDGBuilder& GraphBuilder, FCSCityGroundCommon& Out) const
	{
		static const TArray<FVector4f> NoRoads = { FVector4f::Zero(), FVector4f::Zero() };
		static const TArray<FVector4f> NoLots = { FVector4f::Zero(), FVector4f::Zero(), FVector4f::Zero() };
		Out.GroundRoads = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(GraphBuilder, Roads.Num() > 0 ? Roads : NoRoads, TEXT("CSCityGround.Roads"), false, true).SRV;
		Out.GroundLots = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(GraphBuilder, Lots.Num() > 0 ? Lots : NoLots, TEXT("CSCityGround.Lots"), false, true).SRV;
		const FFieldBinding Binding = BindField(GraphBuilder, Field.Get());
		Out.GroundFieldTable = Binding.Table;
		Out.GroundFieldPages = Binding.Pages;
		Out.GroundFieldHeights = Binding.Heights;
		Out.GroundFieldBox = Binding.Box;
		Out.GroundFieldOn = Binding.On;
		Out.GroundFieldHeightsOn = Binding.HeightsOn;
		Out.GroundOrigin = FVector4f(FVector3f(Origin), 0.0f);
		Out.GroundTile = FVector4f(TileMin.X, TileMin.Y, TileSize, 0.0f);
		Out.GroundNumX = NumX;
		Out.GroundNumRoads = uint32(Roads.Num() / 2);
		Out.GroundNumLots = uint32(Lots.Num() / 3);
		Out.GroundNumThreads = Threads();
		for (int32 Slot = 0; Slot < GroundSlots; Slot++)
		{
			const FBox& Box = Shape.Bounds[Slot];
			const bool bBox = Shape.bHas[Slot] && Box.IsValid;
			Out.GroundSlotMin[Slot] = bBox ? FVector4f(FVector3f(Box.Min), float(Shape.Turns[Slot])) : FVector4f::Zero();
			Out.GroundSlotSize[Slot] = bBox ? FVector4f(FVector3f(Box.GetSize()), 1.0f) : FVector4f(1.0f, 1.0f, 1.0f, Shape.bHas[Slot] ? 1.0f : 0.0f);
		}
		Out.GroundReach = float(Shape.Reach);
		Out.GroundEdgeSink = float(Shape.EdgeSink);
		Out.GroundRoadMargin = float(Shape.RoadMargin);
		Out.GroundRoadShoulder = float(Shape.RoadShoulder);
		Out.GroundPlotBlend = float(Shape.PlotBlend);
		Out.GroundMaxSlope = float(Shape.MaxSlope);
		Out.GroundWallMinStep = float(Shape.WallMinStep);
		Out.GroundBuildingClearance = float(Shape.BuildingClearance);
		Out.GroundRoadClearance = float(Shape.RoadClearance);
		Out.GroundMaxPropSlope = float(Shape.MaxPropSlope);
		Out.GroundAutoPlanterChance = float(Shape.AutoPlanterChance);
		Out.GroundPlanterChance = float(Shape.PlanterChance);
		Out.GroundCartChance = float(Shape.CartChance);
	}

	void RunCount(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityGround.Count"));
		FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), GroundSlots);
		Desc.Usage |= EBufferUsageFlags::SourceCopy;
		FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, TEXT("CSCityGround.Counts"));
		FRDGBufferUAVRef UAV = GraphBuilder.CreateUAV(Buffer);
		AddClearUAVPass(GraphBuilder, UAV, 0u);
		FCSCityGroundCountCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityGroundCountCS::FParameters>();
		FillCommon(GraphBuilder, Params->Common);
		Params->GroundCounts = UAV;
		TShaderMapRef<FCSCityGroundCountCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityGround.Count"), Shader, Params, FComputeShaderUtils::GetGroupCount(int32(Threads()), 16));
		CountsReadback = new FRHIGPUBufferReadback(TEXT("CSCityGround.CountsReadback"));
		AddEnqueueCopyPass(GraphBuilder, CountsReadback, Buffer, sizeof(uint32) * GroundSlots);
		GraphBuilder.Execute();
	}

	void RunWrite(FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityGround.Write"));
		FRDGBufferRef RowBuffers[GroundSlots] = {};
		FRDGBufferRef CounterBuffers[GroundSlots] = {};
		FRDGBufferRef SpareRows = nullptr;
		FRDGBufferRef SpareCounter = nullptr;
		bool bAny = false;
		for (int32 Slot = 0; Slot < GroundSlots; Slot++)
		{
			FFacadeGpu::FSlot& One = Gpu->Slots[Slot];
			GrowSlot(One, Counts[Slot]);
			if (Counts[Slot] == 0)
			{
				// A slot with nothing in it binds a scratch pair: nothing is written there.
				if (!SpareRows)
				{
					SpareRows = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSCityGround.SpareRows"));
					SpareCounter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSCityGround.SpareCounter"));
					AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpareRows, PF_A32B32G32R32F)), 0.0f);
					AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(SpareCounter, PF_R32_UINT)), 0u);
				}
				RowBuffers[Slot] = SpareRows;
				CounterBuffers[Slot] = SpareCounter;
				continue;
			}
			bAny = true;
			RowBuffers[Slot] = GraphBuilder.RegisterExternalBuffer(One.Rows, TEXT("CSCityGround.Rows"));
			CounterBuffers[Slot] = GraphBuilder.RegisterExternalBuffer(One.Counter, TEXT("CSCityGround.Counter"));
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterBuffers[Slot], PF_R32_UINT)), 0u);
		}
		if (bAny)
		{
			FCSCityGroundWriteCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityGroundWriteCS::FParameters>();
			FillCommon(GraphBuilder, Params->Common);
			Params->GroundCapacityLo = FUintVector4(Gpu->Slots[0].Capacity, Gpu->Slots[1].Capacity, Gpu->Slots[2].Capacity, Gpu->Slots[3].Capacity);
			Params->GroundCapacityHi = FUintVector4(Gpu->Slots[4].Capacity, Gpu->Slots[5].Capacity, 0u, 0u);
			FRDGBufferUAVRef* RowUAVs[GroundSlots] = { &Params->GroundRows0, &Params->GroundRows1, &Params->GroundRows2, &Params->GroundRows3, &Params->GroundRows4, &Params->GroundRows5 };
			FRDGBufferUAVRef* CounterUAVs[GroundSlots] = { &Params->GroundCounter0, &Params->GroundCounter1, &Params->GroundCounter2, &Params->GroundCounter3, &Params->GroundCounter4, &Params->GroundCounter5 };
			for (int32 Slot = 0; Slot < GroundSlots; Slot++)
			{
				*RowUAVs[Slot] = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(RowBuffers[Slot], PF_A32B32G32R32F));
				*CounterUAVs[Slot] = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterBuffers[Slot], PF_R32_UINT));
			}
			TShaderMapRef<FCSCityGroundWriteCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityGround.Write"), Shader, Params, FComputeShaderUtils::GetGroupCount(int32(Threads()), 16));
		}
		for (int32 Slot = 0; Slot < GroundSlots; Slot++)
		{
			if (Counts[Slot] == 0) continue;
			if (bReadRows)
			{
				RowsReadback[Slot] = new FRHIGPUBufferReadback(TEXT("CSCityGround.RowsReadback"));
				AddEnqueueCopyPass(GraphBuilder, RowsReadback[Slot], RowBuffers[Slot], Counts[Slot] * CS_GPU_INSTANCED_ROW_FLOAT4S * sizeof(FVector4f));
			}
			GraphBuilder.SetBufferAccessFinal(RowBuffers[Slot], ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterBuffers[Slot], ERHIAccess::SRVMask);
		}
		GraphBuilder.Execute();
		for (int32 Slot = 0; Slot < GroundSlots; Slot++) Landed[Slot] = Gpu->Slots[Slot];
	}

	virtual void Look(FRHICommandListImmediate& RHICmdList) override
	{
		if (State.load(std::memory_order_relaxed) != 0 || !CountsReadback || !CountsReadback->IsReady()) return;
		const void* Data = CountsReadback->Lock(sizeof(uint32) * GroundSlots);
		if (!Data)
		{
			State.store(2, std::memory_order_release);
			return;
		}
		FMemory::Memcpy(Counts, Data, sizeof(uint32) * GroundSlots);
		CountsReadback->Unlock();
		if (Serial != Gpu->Latest.load(std::memory_order_acquire))
		{
			State.store(2, std::memory_order_release);
			return;
		}
		RunWrite(RHICmdList);
		State.store(1, std::memory_order_release);
	}

	virtual void Land() override
	{
		if (Serial != Gpu->Latest.load(std::memory_order_acquire)) return;
		AActor* Target = Owner.Get();
		TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>* Held = Components ? Components() : nullptr;
		if (!IsValid(Target) || !Held) return;
		Held->SetNum(GroundSlots);
		for (int32 Slot = 0; Slot < GroundSlots; Slot++) LandSlot(*Target, Parent.Get(), Name, /*bFollowParent*/ false, Origin, Bounds, Meshes[Slot].Get(), Counts[Slot], Landed[Slot], (*Held)[Slot]);
	}
};

// A tile's inputs, relative to its middle.
void CSCityGround_Inputs(const FGroundKitShape& Shape, const FGroundTile& Tile, FGroundJob& Job)
{
	Job.Shape = Shape;
	const FVector2D Middle = Tile.Min + FVector2D(Tile.Size * 0.5);
	Job.Origin = FVector(Middle, 0.0);
	Job.TileMin = FVector2f(FVector2D(-Tile.Size * 0.5));
	Job.TileSize = float(Tile.Size);
	Job.NumX = uint32(FMath::Max(FMath::CeilToInt32(Tile.Size / CSCityGround_CellCm), 1));
	Job.Roads.Reserve(Tile.Roads.Num() * 2);
	for (const FGroundRoad& Road : Tile.Roads)
	{
		const FVector A = Road.A - Job.Origin;
		const FVector B = Road.B - Job.Origin;
		Job.Roads.Add(FVector4f(float(A.X), float(A.Y), float(A.Z), float(Road.HalfWidth)));
		Job.Roads.Add(FVector4f(float(B.X), float(B.Y), float(B.Z), 0.0f));
	}
	Job.Lots.Reserve(Tile.Lots.Num() * 3);
	for (const FGroundLot& Lot : Tile.Lots)
	{
		const FVector2D Centre = Lot.Centre - Middle;
		const FVector2D Front = Lot.Front.GetSafeNormal();
		Job.Lots.Add(FVector4f(float(Centre.X), float(Centre.Y), float(Front.X), float(Front.Y)));
		Job.Lots.Add(FVector4f(float(Lot.HalfWidth), float(Lot.HalfDepth), float(Lot.Base), float(Lot.Drop)));
		Job.Lots.Add(FVector4f(float(Lot.DropX), float(Lot.DropY), Lot.bLevel ? 1.0f : 0.0f, 0.0f));
	}
}
}

FGroundKitShape GroundKitShape(const UCSCityGroundKit& Kit)
{
	FGroundKitShape Shape;
	for (int32 Slot = 0; Slot < GroundSlots; Slot++)
	{
		const FCSCityFacadeModule& Module = CSCityGround_Module(Kit, Slot);
		const FBox Box = Module.Mesh ? Module.Mesh->GetBoundingBox() : FBox(ForceInit);
		if (!Box.IsValid) continue;
		Shape.bHas[Slot] = true;
		Shape.Turns[Slot] = Module.QuarterTurns & 3;
		Shape.Bounds[Slot] = CSCityGround_Turned(Box, Shape.Turns[Slot]);
	}
	Shape.Reach = FMath::Max(double(Kit.Reach), 0.0) * 100.0;
	Shape.EdgeSink = FMath::Max(double(Kit.EdgeSink), 0.0);
	Shape.RoadMargin = FMath::Max(double(Kit.RoadMargin), 0.0) * 100.0;
	Shape.RoadShoulder = FMath::Max(double(Kit.RoadShoulder), 0.01) * 100.0;
	Shape.PlotBlend = FMath::Max(double(Kit.PlotBlend), 0.01) * 100.0;
	Shape.MaxSlope = FMath::Clamp(double(Kit.MaxSlope), 0.0, 1.0);
	Shape.WallMinStep = FMath::Max(double(Kit.WallMinStep), 0.0);
	Shape.BuildingClearance = FMath::Max(double(Kit.BuildingClearance), 0.0) * 100.0;
	Shape.RoadClearance = FMath::Max(double(Kit.RoadClearance), 0.0) * 100.0;
	Shape.MaxPropSlope = FMath::Clamp(double(Kit.MaxPropSlope), 0.0, 1.0);
	Shape.AutoPlanterChance = FMath::Clamp(double(Kit.AutoPlanterChance), 0.0, 1.0);
	Shape.PlanterChance = FMath::Clamp(double(Kit.PlanterChance), 0.0, 1.0);
	Shape.CartChance = FMath::Clamp(double(Kit.CartChance), 0.0, 1.0);
	return Shape;
}

uint64 GroundKitHash(const UCSCityGroundKit& Kit)
{
	const FGroundKitShape Shape = GroundKitShape(Kit);
	FXxHash64Builder Hash;
	for (int32 Slot = 0; Slot < GroundSlots; Slot++)
	{
		const UStaticMesh* Mesh = Shape.bHas[Slot] ? CSCityGround_Module(Kit, Slot).Mesh.Get() : nullptr;
		Hash.Update(&Mesh, sizeof(Mesh));
		Hash.Update(&Shape.Turns[Slot], sizeof(int32));
		// Not the FBox whole: what pads it is anything.
		if (Shape.bHas[Slot]) for (const FVector& Corner : { Shape.Bounds[Slot].Min, Shape.Bounds[Slot].Max }) Hash.Update(&Corner, sizeof(Corner));
	}
	for (const double Value : { Shape.Reach, Shape.EdgeSink, Shape.RoadMargin, Shape.RoadShoulder, Shape.PlotBlend, Shape.MaxSlope, Shape.WallMinStep, Shape.BuildingClearance, Shape.RoadClearance, Shape.MaxPropSlope, Shape.AutoPlanterChance, Shape.PlanterChance, Shape.CartChance }) Hash.Update(&Value, sizeof(Value));
	return Hash.Finalize().Hash;
}

FGroundLot GroundLot(const FLot& Lot, bool bLevel)
{
	FGroundLot Out;
	Out.Centre = Lot.Centre;
	Out.Front = Lot.Front;
	Out.HalfWidth = Lot.Width * 0.5;
	Out.HalfDepth = Lot.Depth * 0.5;
	Out.Base = Lot.Base;
	Out.Drop = Lot.Drop;
	Out.DropX = Lot.DropX;
	Out.DropY = Lot.DropY;
	Out.bLevel = bLevel;
	return Out;
}

void WriteGround(AActor& Owner, USceneComponent* Parent, const FString& Name, const UCSCityGroundKit& Kit, const FGroundTile& Tile, FFacadeComponents Components, TSharedPtr<FFacadeGpu>& Gpu)
{
	if (!Gpu) Gpu = MakeShared<FFacadeGpu>();
	const FGroundKitShape Shape = GroundKitShape(Kit);
	const TSharedRef<FGroundJob> Job = MakeShared<FGroundJob>();
	CSCityGround_Inputs(Shape, Tile, *Job);
	double Raised = 0.0;
	if (const ACSCityFieldActor* Field = ACSCityFieldActor::Find(Owner.GetWorld()))
	{
		Job->Field = Field->GpuField();
		Raised = Field->MaxElevation();
	}
	Job->Gpu = Gpu;
	Job->Serial = Gpu->Latest.fetch_add(1, std::memory_order_acq_rel) + 1u;
	Job->Owner = &Owner;
	Job->Parent = Parent;
	Job->Name = Name;
	Job->Components = MoveTemp(Components);
	for (int32 Slot = 0; Slot < GroundSlots; Slot++) if (Shape.bHas[Slot]) Job->Meshes[Slot] = CSCityGround_Module(Kit, Slot).Mesh.Get();
	// As high and low as the roads and the plots go, what the field does to that, and what stands on it.
	double Low = TNumericLimits<double>::Max();
	double High = TNumericLimits<double>::Lowest();
	for (const FGroundRoad& Road : Tile.Roads)
	{
		Low = FMath::Min3(Low, Road.A.Z, Road.B.Z);
		High = FMath::Max3(High, Road.A.Z, Road.B.Z);
	}
	for (const FGroundLot& Lot : Tile.Lots)
	{
		const double Lowest = Lot.Base - FMath::Abs(Lot.Drop) - FMath::Abs(Lot.DropX) * Lot.HalfWidth - FMath::Abs(Lot.DropY) * Lot.HalfDepth;
		Low = FMath::Min(Low, Lowest);
		High = FMath::Max(High, Lot.Base);
	}
	if (Low > High)
	{
		Low = 0.0;
		High = 0.0;
	}
	const double Half = Tile.Size * 0.5 + 200.0;
	Job->Bounds = FBox(FVector(-Half, -Half, Low - Raised - 200.0), FVector(Half, Half, High + Raised + 600.0));
	ENQUEUE_RENDER_COMMAND(CSCityGroundCount)([Job](FRHICommandListImmediate& RHICmdList) { Job->RunCount(RHICmdList); });
	AddGpuWrite(Job);
}

FGroundLayout LayoutGround(const FGroundKitShape& Kit, const FGroundTile& Tile, const TSharedPtr<FFacadeFieldGpu>& Field)
{
	FGroundLayout Out;
	const TSharedRef<FGroundJob> Job = MakeShared<FGroundJob>();
	CSCityGround_Inputs(Kit, Tile, *Job);
	Job->Field = Field;
	Job->Gpu = MakeShared<FFacadeGpu>();
	Job->Serial = 1;
	Job->Gpu->Latest = 1;
	Job->bReadRows = true;
	ENQUEUE_RENDER_COMMAND(CSCityGroundLayout)([Job](FRHICommandListImmediate& RHICmdList)
	{
		Job->RunCount(RHICmdList);
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		for (int32 Round = 0; Round < 1000 && Job->State.load() == 0; Round++)
		{
			Job->Look(RHICmdList);
			if (Job->State.load() == 0) FPlatformProcess::Sleep(0.001f);
		}
		RHICmdList.SubmitAndBlockUntilGPUIdle();
		for (int32 Slot = 0; Slot < GroundSlots; Slot++)
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
	for (int32 Slot = 0; Slot < GroundSlots; Slot++)
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
				Box += X * M.X + Y * M.Y + Z * M.Z + O + Job->Origin;
			}
			Out.Boxes[Slot].Add(Box);
		}
	}
	return Out;
}
}

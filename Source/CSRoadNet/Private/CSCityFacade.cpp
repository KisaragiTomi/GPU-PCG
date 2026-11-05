#include "CSCityFacade.h"

#include "ComputeShaderGenerateHelper.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadMeshWrite.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Actor.h"
#include "GlobalShader.h"
#include "Hash/xxhash.h"
#include "Math/RandomStream.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

const FCSCityFacadeModule& UCSCityFacadeKit::Module(int32 Slot) const
{
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
	default: return Wall;
	}
}

UCSCityFacadeKit* UCSCityFacadeKit::Default()
{
	return LoadObject<UCSCityFacadeKit>(nullptr, TEXT("/PCGPlugins/CSRoadNet/Houses/Facade/DA_FacadeKit_Simple.DA_FacadeKit_Simple"));
}

class FCSCityFacadeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCSCityFacadeCS);
	SHADER_USE_PARAMETER_STRUCT(FCSCityFacadeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FacadeBuildingsF)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, FacadeBuildingSeeds)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, FacadeRunsF)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint4>, FacadeRunsU)
		SHADER_PARAMETER(uint32, FacadeNumRuns)
		SHADER_PARAMETER(uint32, FacadeNumThreads)
		SHADER_PARAMETER_ARRAY(FVector4f, FacadeSlotMin, [CSCity::FacadeSlots])
		SHADER_PARAMETER_ARRAY(FVector4f, FacadeSlotSize, [CSCity::FacadeSlots])
		SHADER_PARAMETER(uint32, FacadeSlotFirst)
		SHADER_PARAMETER(uint32, FacadeSlotCount)
		SHADER_PARAMETER(FUintVector4, FacadeCapacityLo)
		SHADER_PARAMETER(uint32, FacadeCapacityHi)
		SHADER_PARAMETER(float, FacadeStorey)
		SHADER_PARAMETER(float, FacadeGround)
		SHADER_PARAMETER(float, FacadeCorner)
		SHADER_PARAMETER(float, FacadeParapet)
		SHADER_PARAMETER(float, FacadeWindowChance)
		SHADER_PARAMETER(float, FacadeGroundWindowChance)
		SHADER_PARAMETER(uint32, FacadeColumnGroup)
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
};
IMPLEMENT_GLOBAL_SHADER(FCSCityFacadeCS, "/Plugin/PCGPlugins/Shaders/Private/CSRoad/CSCityFacade.usf", "FacadeCS", SF_Compute);

namespace CSCity
{
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
	FSlot Slots[FacadeSlots];
};

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
	if (Shape.bHas[int32(EFacadeSlot::Corner)]) Shape.Corner = Shape.Bounds[int32(EFacadeSlot::Corner)].GetSize().X;
	if (Shape.bHas[int32(EFacadeSlot::Parapet)]) Shape.Parapet = Shape.Bounds[int32(EFacadeSlot::Parapet)].GetSize().Z;
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
	for (const double Value : { Shape.Bay, Shape.Storey, Shape.Ground, Shape.WindowChance, Shape.GroundWindowChance }) Hash.Update(&Value, sizeof(Value));
	Hash.Update(&Shape.ColumnGroup, sizeof(Shape.ColumnGroup));
	Hash.Update(Shape.ShapeWeights, sizeof(Shape.ShapeWeights));
	for (const float Value : { Shape.SetbackChance, Shape.PodiumChance }) Hash.Update(&Value, sizeof(Value));
	Hash.Update(&Shape.MinSetbackStoreys, sizeof(Shape.MinSetbackStoreys));
	return Hash.Finalize().Hash;
}

FFacadeBuilding LotBuilding(const FLot& Lot)
{
	FFacadeBuilding Building;
	Building.Block = LotBlock(Lot);
	Building.Seed = uint32(Lot.Id ^ (Lot.Id >> 32));
	return Building;
}

FFacadeMassing MassFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building)
{
	FFacadeMassing Out;
	const FBlock& Block = Building.Block;
	if (Block.Width <= 0.0 || Block.Depth <= 0.0 || Block.Height <= 0.0) return Out;
	const int32 NX = FMath::Clamp(FMath::RoundToInt32(Block.Width / Kit.Bay), 1, 64);
	const int32 NY = FMath::Clamp(FMath::RoundToInt32(Block.Depth / Kit.Bay), 1, 64);
	const int32 S = FMath::Max(1, FMath::RoundToInt32(Block.Height / Kit.Storey));
	Out.GridX = NX;
	Out.GridY = NY;
	Out.Storeys = S;
	// Every draw comes from the seed, in the same order, so a building is put together the same every time.
	FRandomStream Random(int32(Building.Seed ^ 0x5bd1e995u));

	// The footprint: which shapes the grid has room for, then the one asked for or drawn.
	const bool bFits[5] = { true, NX >= 2 && NY >= 2, NX >= 3 && NY >= 2, NX >= 3 && NY >= 2, NX >= 3 && NY >= 3 };
	ECSCityMassShape Shape = Building.Shape;
	if (Shape == ECSCityMassShape::Auto)
	{
		float Total = 0.0f;
		for (int32 K = 0; K < 5; K++) Total += bFits[K] ? Kit.ShapeWeights[K] : 0.0f;
		float Pick = Random.FRand() * Total;
		Shape = ECSCityMassShape::Box;
		for (int32 K = 0; K < 5 && Total > 0.0f; K++)
		{
			if (!bFits[K] || Kit.ShapeWeights[K] <= 0.0f) continue;
			Shape = ECSCityMassShape(int32(ECSCityMassShape::Box) + K);
			if (Pick < Kit.ShapeWeights[K]) break;
			Pick -= Kit.ShapeWeights[K];
		}
	}
	if (!bFits[int32(Shape) - int32(ECSCityMassShape::Box)]) Shape = ECSCityMassShape::Box;
	TArray<uint8> Base;
	Base.Init(1, NX * NY);
	auto Cut = [NX](TArray<uint8>& Cells, int32 I0, int32 I1, int32 J0, int32 J1)
	{
		for (int32 J = J0; J < J1; J++) for (int32 I = I0; I < I1; I++) Cells[J * NX + I] = 0;
	};
	// Cuts come from the back (row 0); the front row stays whole.
	switch (Shape)
	{
	case ECSCityMassShape::L:
	{
		const int32 A = Random.RandRange(1, FMath::Max(1, NX / 2));
		const int32 B = Random.RandRange(1, FMath::Max(1, FMath::Min(NY - 1, (NY + 1) / 2)));
		if (Random.FRand() < 0.5f) Cut(Base, 0, A, 0, B);
		else Cut(Base, NX - A, NX, 0, B);
		break;
	}
	case ECSCityMassShape::U:
	{
		const int32 A = Random.RandRange(1, FMath::Max(1, NX - 2));
		const int32 I0 = Random.RandRange(1, NX - 1 - A);
		const int32 B = Random.RandRange(1, FMath::Max(1, FMath::Min(NY - 1, (NY + 1) / 2)));
		Cut(Base, I0, I0 + A, 0, B);
		break;
	}
	case ECSCityMassShape::T:
	{
		const int32 A = Random.RandRange(1, FMath::Max(1, (NX - 1) / 2));
		const int32 B = Random.RandRange(FMath::Max(1, NY / 3), NY - 1);
		Cut(Base, 0, A, 0, B);
		Cut(Base, NX - A, NX, 0, B);
		break;
	}
	case ECSCityMassShape::Courtyard:
	{
		const int32 Ring = NX >= 7 && NY >= 7 ? 2 : 1;
		Cut(Base, Ring, NX - Ring, Ring, NY - Ring);
		break;
	}
	default: break;
	}
	Out.Shape = Shape;

	// The stacking.
	ECSCityMassStack Stack = Building.Stack;
	if (Stack == ECSCityMassStack::Auto)
	{
		const float PodiumRoll = Random.FRand();
		const float SetbackRoll = Random.FRand();
		if (S >= 6 && Shape == ECSCityMassShape::Box && NX * NY >= 4 && PodiumRoll < Kit.PodiumChance) Stack = ECSCityMassStack::PodiumTower;
		else if (S >= Kit.MinSetbackStoreys && NY >= 2 && SetbackRoll < Kit.SetbackChance) Stack = ECSCityMassStack::Setback;
		else Stack = ECSCityMassStack::None;
	}
	auto AddTier = [&Out](int32 First, int32 End, const TArray<uint8>& Cells)
	{
		if (End <= First || !Cells.Contains(uint8(1))) return false;
		FFacadeTier& Tier = Out.Tiers.AddDefaulted_GetRef();
		Tier.First = First;
		Tier.End = End;
		Tier.Cells = Cells;
		return true;
	};
	if (Stack == ECSCityMassStack::Setback && S >= 2 && NY >= 2)
	{
		const int32 K1 = FMath::Clamp(FMath::RoundToInt32(S * FMath::Lerp(0.4f, 0.75f, Random.FRand())), 1, S - 1);
		TArray<uint8> Upper = Base;
		const int32 Front = NY >= 5 && Random.FRand() < 0.5f ? 2 : 1;
		Cut(Upper, 0, NX, NY - Front, NY);
		// Sometimes in from the sides and the back as well.
		if (NX >= 4 && NY >= 4 && Random.FRand() < 0.35f)
		{
			Cut(Upper, 0, 1, 0, NY);
			Cut(Upper, NX - 1, NX, 0, NY);
			Cut(Upper, 0, NX, 0, 1);
		}
		if (Upper.Contains(uint8(1)))
		{
			AddTier(0, K1, Base);
			// Tall ones step back once more, a row nearer the back.
			int32 Rows = 0;
			for (int32 J = 0; J < NY; J++) for (int32 I = 0; I < NX; I++) if (Upper[J * NX + I]) Rows = FMath::Max(Rows, J + 1);
			TArray<uint8> Top = Upper;
			if (Rows >= 3) Cut(Top, 0, NX, Rows - 1, NY);
			if (S - K1 >= 6 && Rows >= 3 && Top.Contains(uint8(1)) && Random.FRand() < 0.6f)
			{
				const int32 K2 = K1 + (S - K1) / 2;
				AddTier(K1, K2, Upper);
				AddTier(K2, S, Top);
			}
			else AddTier(K1, S, Upper);
		}
		else Stack = ECSCityMassStack::None;
	}
	else if (Stack == ECSCityMassStack::PodiumTower && S >= 2 && NX * NY >= 2)
	{
		const int32 Podium = FMath::Clamp(Random.RandRange(1, 3), 1, S - 1);
		int32 TW = FMath::Clamp(FMath::RoundToInt32(NX * FMath::Lerp(0.45f, 0.7f, Random.FRand())), 1, NX);
		int32 TD = FMath::Clamp(FMath::RoundToInt32(NY * FMath::Lerp(0.45f, 0.7f, Random.FRand())), 1, NY);
		if (TW == NX && TD == NY)
		{
			if (NX >= 2) TW = NX - 1;
			else TD = NY - 1;
		}
		const int32 I0 = (NX - TW) / 2;
		const int32 J0 = (NY - TD) / 2;
		TArray<uint8> Tower;
		Tower.Init(0, NX * NY);
		for (int32 J = J0; J < J0 + TD; J++) for (int32 I = I0; I < I0 + TW; I++) Tower[J * NX + I] = Base[J * NX + I];
		if (Tower.Contains(uint8(1)))
		{
			AddTier(0, Podium, Base);
			AddTier(Podium, S, Tower);
		}
		else Stack = ECSCityMassStack::None;
	}
	else Stack = ECSCityMassStack::None;
	if (Stack == ECSCityMassStack::None) AddTier(0, S, Base);
	Out.Stack = Stack;
	return Out;
}

uint32 FFacadeRun::Threads() const
{
	switch (Kind)
	{
	case EFacadeRun::Facade: return uint32(Count) * uint32(End - First);
	case EFacadeRun::Corner:
	case EFacadeRun::CornerInner: return uint32(End - First) + (bParapetPiece ? 1u : 0u);
	case EFacadeRun::Parapet: return uint32(Count);
	default: return uint32(Count) * uint32(Rows);
	}
}

uint32 FFacadeLayout::Threads() const
{
	uint32 Sum = 0;
	for (const FFacadeRun& Run : Runs) Sum += Run.Threads();
	return Sum;
}

namespace
{
// Walking a side of a tier's outline: the cell at position Pos of line Line in walking order, the neighbour out of the
// side, a cell's start and end corners on the grid, and a grid corner's Along and Out in the side's frame.
const FIntPoint CSCityFacade_OutStep[4] = { FIntPoint(0, 1), FIntPoint(1, 0), FIntPoint(0, -1), FIntPoint(-1, 0) };

FIntPoint CSCityFacade_CellOf(int32 Side, int32 Line, int32 Pos, int32 NX, int32 NY)
{
	switch (Side)
	{
	case 0: return FIntPoint(Pos, Line);
	case 1: return FIntPoint(Line, NY - 1 - Pos);
	case 2: return FIntPoint(NX - 1 - Pos, Line);
	default: return FIntPoint(Line, Pos);
	}
}

FIntPoint CSCityFacade_StartVertex(int32 Side, const FIntPoint& C)
{
	switch (Side)
	{
	case 0: return FIntPoint(C.X, C.Y + 1);
	case 1: return FIntPoint(C.X + 1, C.Y + 1);
	case 2: return FIntPoint(C.X + 1, C.Y);
	default: return FIntPoint(C.X, C.Y);
	}
}

FIntPoint CSCityFacade_EndVertex(int32 Side, const FIntPoint& C)
{
	switch (Side)
	{
	case 0: return FIntPoint(C.X + 1, C.Y + 1);
	case 1: return FIntPoint(C.X + 1, C.Y);
	case 2: return FIntPoint(C.X, C.Y);
	default: return FIntPoint(C.X, C.Y + 1);
	}
}

struct FCSCityFacadeGrid
{
	double X0 = 0.0;
	double Y0 = 0.0;
	double CX = 0.0;
	double CY = 0.0;
	double Along(int32 Side, const FIntPoint& V) const
	{
		switch (Side)
		{
		case 0: return X0 + V.X * CX;
		case 1: return -(Y0 + V.Y * CY);
		case 2: return -(X0 + V.X * CX);
		default: return Y0 + V.Y * CY;
		}
	}
	double Out(int32 Side, const FIntPoint& V) const
	{
		switch (Side)
		{
		case 0: return Y0 + V.Y * CY;
		case 1: return X0 + V.X * CX;
		case 2: return -(Y0 + V.Y * CY);
		default: return -(X0 + V.X * CX);
		}
	}
};

// A row of edge cells on one side.
struct FCSCityFacadeWalk
{
	FIntPoint First;
	FIntPoint Last;
	int32 Count = 0;
};

enum class ECSCityFacadeCorner : uint8 { None, Outer, Inner };
}

FFacadeLayout LayoutFacade(const FFacadeKitShape& Kit, const FFacadeBuilding& Building)
{
	FFacadeLayout Out;
	Out.Massing = MassFacade(Kit, Building);
	const FFacadeMassing& M = Out.Massing;
	if (M.Tiers.Num() == 0) return Out;
	const FBlock& Block = Building.Block;
	const int32 NX = M.GridX;
	const int32 NY = M.GridY;
	FCSCityFacadeGrid Grid;
	Grid.CX = Block.Width / NX;
	Grid.CY = Block.Depth / NY;
	Grid.X0 = -0.5 * Block.Width;
	Grid.Y0 = -0.5 * Block.Depth;
	Out.CellX = Grid.CX;
	Out.CellY = Grid.CY;
	auto Has = [&Kit](EFacadeSlot Slot) { return Kit.bHas[int32(Slot)]; };
	const bool bFacade = Has(EFacadeSlot::Wall) || Has(EFacadeSlot::Window) || Has(EFacadeSlot::Door) || Has(EFacadeSlot::GroundWall) || Has(EFacadeSlot::GroundWindow);
	const double Corner = Has(EFacadeSlot::Corner) ? Kit.Corner : 0.0;
	const int32 DoorI = Has(EFacadeSlot::Door) && Building.DoorAt >= 0.0 && bFacade ? FMath::Clamp(FMath::FloorToInt32(Building.DoorAt * NX), 0, NX - 1) : INDEX_NONE;

	for (int32 T = 0; T < M.Tiers.Num(); T++)
	{
		const FFacadeTier& Tier = M.Tiers[T];
		auto In = [&M, T](const FIntPoint& C) { return M.At(T, C.X, C.Y); };
		auto Above = [&M, T](const FIntPoint& C) { return M.At(T + 1, C.X, C.Y); };
		auto CornerAt = [&M](int32 Tier, const FIntPoint& V)
		{
			const bool A = M.At(Tier, V.X - 1, V.Y - 1);
			const bool B = M.At(Tier, V.X, V.Y - 1);
			const bool C = M.At(Tier, V.X - 1, V.Y);
			const bool D = M.At(Tier, V.X, V.Y);
			const int32 N = int32(A) + int32(B) + int32(C) + int32(D);
			// Two cells touching only at the corner: each has its own outer corner there.
			if (N == 1 || (N == 2 && A == D)) return ECSCityFacadeCorner::Outer;
			return N == 3 ? ECSCityFacadeCorner::Inner : ECSCityFacadeCorner::None;
		};
		auto Walk = [&](int32 Side, TFunctionRef<bool(const FIntPoint&)> Keep)
		{
			TArray<FCSCityFacadeWalk> Walks;
			const int32 Lines = (Side & 1) == 0 ? NY : NX;
			const int32 Positions = (Side & 1) == 0 ? NX : NY;
			for (int32 Line = 0; Line < Lines; Line++)
			{
				FCSCityFacadeWalk Current;
				for (int32 Pos = 0; Pos <= Positions; Pos++)
				{
					bool bEdge = false;
					FIntPoint Cell;
					if (Pos < Positions)
					{
						Cell = CSCityFacade_CellOf(Side, Line, Pos, NX, NY);
						bEdge = In(Cell) && !In(Cell + CSCityFacade_OutStep[Side]) && Keep(Cell);
					}
					if (bEdge)
					{
						if (Current.Count == 0) Current.First = Cell;
						Current.Last = Cell;
						Current.Count++;
					}
					else if (Current.Count > 0)
					{
						Walks.Add(Current);
						Current = FCSCityFacadeWalk();
					}
				}
			}
			return Walks;
		};
		auto ColumnOf = [](int32 Side, const FIntPoint& C) { return (Side & 1) == 0 ? C.X : C.Y; };
		auto Step = [](int32 Side) { return Side == 0 || Side == 3 ? 1 : -1; };
		// A row of cells along a side, its ends trimmed by the outer corners' width; cells left too narrow dropped.
		auto Row = [&](EFacadeRun Kind, int32 Side, const FCSCityFacadeWalk& W, FFacadeRun& Run)
		{
			const FIntPoint StartV = CSCityFacade_StartVertex(Side, W.First);
			const FIntPoint EndV = CSCityFacade_EndVertex(Side, W.Last);
			Run.Kind = Kind;
			Run.Side = uint8(Side);
			Run.First = Tier.First;
			Run.End = Tier.End;
			Run.Count = W.Count;
			Run.Cell = (Side & 1) == 0 ? Grid.CX : Grid.CY;
			Run.Along = Grid.Along(Side, StartV);
			Run.Out = Grid.Out(Side, StartV);
			Run.Column = ColumnOf(Side, W.First);
			Run.ColumnStep = Step(Side);
			Run.bTrimStart = Corner > 0.0 && CornerAt(T, StartV) == ECSCityFacadeCorner::Outer;
			Run.bTrimEnd = Corner > 0.0 && CornerAt(T, EndV) == ECSCityFacadeCorner::Outer;
			// A single cell trimmed at both ends, or an end cell narrower than the corner, has no room.
			if (Run.Count == 1 && Run.Cell - Corner * (int32(Run.bTrimStart) + int32(Run.bTrimEnd)) <= 1.0) return false;
			if (Run.bTrimStart && Run.Cell - Corner <= 1.0)
			{
				Run.Along += Run.Cell;
				Run.Column += Run.ColumnStep;
				Run.Count--;
				Run.bTrimStart = false;
			}
			if (Run.bTrimEnd && Run.Cell - Corner <= 1.0)
			{
				Run.Count--;
				Run.bTrimEnd = false;
			}
			return Run.Count > 0;
		};

		for (int32 Side = 0; Side < 4; Side++)
		{
			const TArray<FCSCityFacadeWalk> Walls = Walk(Side, [](const FIntPoint&) { return true; });
			for (const FCSCityFacadeWalk& W : Walls)
			{
				// The corner column at the run's start: an outer one or an inner one, one more piece on top if it stops
				// at this tier's roof.
				const FIntPoint StartV = CSCityFacade_StartVertex(Side, W.First);
				const ECSCityFacadeCorner Kind = CornerAt(T, StartV);
				const EFacadeSlot Slot = Kind == ECSCityFacadeCorner::Outer ? EFacadeSlot::Corner : EFacadeSlot::CornerInner;
				if (Kind != ECSCityFacadeCorner::None && Has(Slot))
				{
					FFacadeRun& Run = Out.Runs.AddDefaulted_GetRef();
					Run.Kind = Kind == ECSCityFacadeCorner::Outer ? EFacadeRun::Corner : EFacadeRun::CornerInner;
					Run.Side = uint8(Side);
					Run.First = Tier.First;
					Run.End = Tier.End;
					Run.Along = Grid.Along(Side, StartV);
					Run.Out = Grid.Out(Side, StartV);
					const bool bGoesOn = Above(W.First) && !Above(W.First + CSCityFacade_OutStep[Side]) && CornerAt(T + 1, StartV) == Kind;
					Run.bParapetPiece = Has(EFacadeSlot::Parapet) && !bGoesOn;
					(Kind == ECSCityFacadeCorner::Outer ? Out.Corners : Out.InnerCorners) += Run.Threads();
				}
				if (!bFacade) continue;
				FFacadeRun Run;
				if (!Row(EFacadeRun::Facade, Side, W, Run)) continue;
				// The door: on the ground storey, in the front row's run.
				if (T == 0 && Side == 0 && W.First.Y == NY - 1 && DoorI != INDEX_NONE)
				{
					const int32 Cell = DoorI - Run.Column;
					if (Cell >= 0 && Cell < Run.Count)
					{
						Run.DoorCell = Cell;
						Out.DoorCell = DoorI;
						Out.Doors = 1;
					}
				}
				Out.FacadeCells += Run.Threads();
				if (Run.First == 0) Out.GroundCells += uint32(Run.Count);
				Out.Runs.Add(Run);
			}
			// The parapet round this tier's roof: its edge where the tier above does not go on.
			if (Has(EFacadeSlot::Parapet))
			{
				for (const FCSCityFacadeWalk& W : Walk(Side, [&Above](const FIntPoint& C) { return !Above(C); }))
				{
					FFacadeRun Run;
					if (!Row(EFacadeRun::Parapet, Side, W, Run)) continue;
					Out.Parapets += Run.Threads();
					Out.Runs.Add(Run);
				}
			}
		}

		// Roof tiles over this tier's cells that the tier above leaves open, as rectangles.
		if (Has(EFacadeSlot::Roof))
		{
			TArray<uint8> Open;
			Open.Init(0, NX * NY);
			for (int32 J = 0; J < NY; J++) for (int32 I = 0; I < NX; I++) Open[J * NX + I] = In(FIntPoint(I, J)) && !Above(FIntPoint(I, J)) ? 1 : 0;
			for (int32 J = 0; J < NY; J++)
			{
				for (int32 I = 0; I < NX; I++)
				{
					if (!Open[J * NX + I]) continue;
					int32 I1 = I;
					while (I1 < NX && Open[J * NX + I1]) I1++;
					int32 J1 = J + 1;
					while (J1 < NY)
					{
						bool bRow = true;
						for (int32 K = I; K < I1 && bRow; K++) bRow = Open[J1 * NX + K] != 0;
						if (!bRow) break;
						J1++;
					}
					for (int32 B = J; B < J1; B++) for (int32 K = I; K < I1; K++) Open[B * NX + K] = 0;
					FFacadeRun& Run = Out.Runs.AddDefaulted_GetRef();
					Run.Kind = EFacadeRun::Roof;
					Run.First = Tier.First;
					Run.End = Tier.End;
					Run.Count = I1 - I;
					Run.Rows = J1 - J;
					Run.Along = Grid.X0 + I * Grid.CX;
					Run.Out = Grid.Y0 + J * Grid.CY;
					Run.Cell = Grid.CX;
					Run.CellY = Grid.CY;
					Out.Roofs += Run.Threads();
				}
			}
		}
	}
	return Out;
}

namespace
{
UCSGpuInstancedMeshComponent* CSCityFacade_MakeComponent(AActor& Owner, USceneComponent* Parent, const FString& Name, bool bFollowParent)
{
	// Made at build time, never saved or copied.
	const FName Unique = MakeUniqueObjectName(&Owner, UCSGpuInstancedMeshComponent::StaticClass(), FName(*Name));
	UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(&Owner, Unique, RF_Transient | RF_DuplicateTransient);
	if (Parent) Component->SetupAttachment(Parent);
	Component->SetUsingAbsoluteLocation(!bFollowParent);
	Component->SetUsingAbsoluteRotation(!bFollowParent);
	Component->SetUsingAbsoluteScale(true);
	Component->RegisterComponent();
	return Component;
}
}

void WriteFacade(AActor& Owner, USceneComponent* Parent, const FString& Name, const FVector& Origin, const UCSCityFacadeKit& Kit, TConstArrayView<FFacadeBuilding> Buildings,
	TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu, bool bFollowParent)
{
	const FFacadeKitShape Shape = KitShape(Kit);
	Components.SetNum(FacadeSlots);
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		TObjectPtr<UCSGpuInstancedMeshComponent>& Component = Components[Slot];
		if (!Shape.bHas[Slot])
		{
			if (Component) Component->DestroyComponent();
			Component = nullptr;
			continue;
		}
		if (!Component || !IsValid(Component)) Component = CSCityFacade_MakeComponent(Owner, Parent, Name, bFollowParent);
		if (Component->BaseMesh != Kit.Module(Slot).Mesh) Component->SetBaseMesh(Kit.Module(Slot).Mesh);
		if (!Component->GetComponentLocation().Equals(Origin, 0.01) || !Component->GetComponentQuat().Equals(FQuat::Identity, 1e-6)) Component->SetWorldLocationAndRotation(Origin, FQuat::Identity);
	}
	if (!Gpu) Gpu = MakeShared<FFacadeGpu>();

	// Each building's frame and seed, each run's record and first thread, and how many modules each slot can take at most.
	TArray<FVector4f> BuildingsF;
	TArray<uint32> Seeds;
	TArray<FVector4f> RunsF;
	TArray<FUintVector4> RunsU;
	uint32 Threads = 0;
	uint32 Need[FacadeSlots] = {};
	FBox Bounds(ForceInit);
	for (const FFacadeBuilding& Building : Buildings)
	{
		const FFacadeLayout Layout = LayoutFacade(Shape, Building);
		if (Layout.Threads() == 0) continue;
		const FBlock& Block = Building.Block;
		const FVector Base = Block.Base - Origin;
		const FVector2D Front = Block.Front.GetSafeNormal();
		const uint32 Index = uint32(Seeds.Num());
		BuildingsF.Add(FVector4f(float(Base.X), float(Base.Y), float(Base.Z), 0.0f));
		BuildingsF.Add(FVector4f(float(Front.X), float(Front.Y), 0.0f, 0.0f));
		Seeds.Add(Building.Seed);
		for (const FFacadeRun& Run : Layout.Runs)
		{
			const uint32 RunThreads = Run.Threads();
			if (RunThreads == 0) continue;
			const uint32 Flags = (Run.bTrimStart ? 1u : 0u) | (Run.bTrimEnd ? 2u : 0u) | (Run.bParapetPiece ? 4u : 0u) | (Run.ColumnStep < 0 ? 8u : 0u);
			RunsF.Add(FVector4f(float(Run.Along), float(Run.Out), float(Run.Cell), float(Run.CellY)));
			RunsU.Add(FUintVector4(Threads, Index, uint32(Run.Kind) | uint32(Run.Side) << 4 | Flags << 8, uint32(Run.Count) | uint32(Run.Rows) << 16));
			RunsU.Add(FUintVector4(uint32(Run.First) | uint32(Run.End) << 16, uint32(FMath::Max(Run.Column, 0)), uint32(Run.DoorCell + 1), RunThreads));
			Threads += RunThreads;
		}
		Need[int32(EFacadeSlot::Wall)] += Layout.FacadeCells;
		Need[int32(EFacadeSlot::Window)] += Layout.FacadeCells;
		Need[int32(EFacadeSlot::GroundWall)] += Layout.GroundCells;
		Need[int32(EFacadeSlot::GroundWindow)] += Layout.GroundCells;
		Need[int32(EFacadeSlot::Door)] += Layout.Doors;
		Need[int32(EFacadeSlot::Corner)] += Layout.Corners;
		Need[int32(EFacadeSlot::CornerInner)] += Layout.InnerCorners;
		Need[int32(EFacadeSlot::Parapet)] += Layout.Parapets;
		Need[int32(EFacadeSlot::Roof)] += Layout.Roofs;
		// The building's box, the parapet over the top and the modules' thickness round it.
		const FVector2D Half = FVector2D(FMath::Abs(Front.Y) * Block.Width + FMath::Abs(Front.X) * Block.Depth, FMath::Abs(Front.X) * Block.Width + FMath::Abs(Front.Y) * Block.Depth) * 0.5;
		const double Top = Shape.StoreyBase(Layout.Massing.Storeys) + Shape.Parapet;
		Bounds += FBox(FVector(Base.X - Half.X, Base.Y - Half.Y, Base.Z), FVector(Base.X + Half.X, Base.Y + Half.Y, Base.Z + Top));
	}
	if (Bounds.IsValid) Bounds = Bounds.ExpandBy(200.0);
	const uint32 NumRuns = uint32(RunsF.Num());

	struct FConstants
	{
		FVector4f SlotMin[FacadeSlots];
		FVector4f SlotSize[FacadeSlots];
		float Storey, Ground, Corner, Parapet, WindowChance, GroundWindowChance;
		uint32 ColumnGroup;
	} Constants;
	for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
	{
		const FBox& Box = Shape.Bounds[Slot];
		Constants.SlotMin[Slot] = Shape.bHas[Slot] ? FVector4f(FVector3f(Box.Min), float(Shape.Turns[Slot])) : FVector4f::Zero();
		Constants.SlotSize[Slot] = Shape.bHas[Slot] ? FVector4f(FVector3f(Box.GetSize()), 1.0f) : FVector4f::Zero();
	}
	Constants.Storey = float(Shape.Storey);
	Constants.Ground = float(Shape.GroundHeight());
	Constants.Corner = float(Shape.bHas[int32(EFacadeSlot::Corner)] ? Shape.Corner : 0.0);
	Constants.Parapet = float(Shape.Parapet);
	Constants.WindowChance = float(Shape.WindowChance);
	Constants.GroundWindowChance = float(Shape.GroundWindowChance);
	Constants.ColumnGroup = uint32(Shape.ColumnGroup);

	// The render thread grows the slots' buffers when they lack room, clears the counters and writes; what it leaves is
	// handed to the components once it has run.
	struct FLanded
	{
		FFacadeGpu::FSlot Slots[FacadeSlots];
	};
	const TSharedRef<FLanded> Landed = MakeShared<FLanded>();
	TArray<uint32> Needs(Need, FacadeSlots);
	ENQUEUE_RENDER_COMMAND(CSCityFacadeWrite)([Gpu, Landed, BuildingsF = MoveTemp(BuildingsF), Seeds = MoveTemp(Seeds), RunsF = MoveTemp(RunsF), RunsU = MoveTemp(RunsU), Needs = MoveTemp(Needs), Threads, NumRuns, Constants](FRHICommandListImmediate& RHICmdList)
	{
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSCityFacade.Write"));
		FRDGBufferRef Rows[FacadeSlots];
		FRDGBufferRef Counters[FacadeSlots];
		for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
		{
			FFacadeGpu::FSlot& One = Gpu->Slots[Slot];
			if (!One.Rows.IsValid() || One.Capacity < Needs[Slot])
			{
				// A quarter more than asked, so that a building or two more does not grow it again.
				const uint32 Capacity = FMath::Max(Align(Needs[Slot] + Needs[Slot] / 4, 64u), 64u);
				One.Rows = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSCityFacade.Rows"));
				One.Counter = AllocatePooledBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSCityFacade.Counter"));
				One.Capacity = Capacity;
			}
			Rows[Slot] = GraphBuilder.RegisterExternalBuffer(One.Rows, TEXT("CSCityFacade.Rows"));
			Counters[Slot] = GraphBuilder.RegisterExternalBuffer(One.Counter, TEXT("CSCityFacade.Counter"));
			// The component never clears it: a write starts from none.
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Counters[Slot], PF_R32_UINT)), 0u);
		}
		if (Threads > 0 && NumRuns > 0)
		{
			FRDGBufferSRVRef BuildingsSRV = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(GraphBuilder, BuildingsF, TEXT("CSCityFacade.BuildingsF"), false, true).SRV;
			FRDGBufferSRVRef SeedsSRV = CSHelper::CreateUploadedStructuredBuffer<uint32>(GraphBuilder, Seeds, TEXT("CSCityFacade.Seeds"), false, true).SRV;
			FRDGBufferSRVRef RunsFSRV = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(GraphBuilder, RunsF, TEXT("CSCityFacade.RunsF"), false, true).SRV;
			FRDGBufferSRVRef RunsUSRV = CSHelper::CreateUploadedStructuredBuffer<FUintVector4>(GraphBuilder, RunsU, TEXT("CSCityFacade.RunsU"), false, true).SRV;
			TShaderMapRef<FCSCityFacadeCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			// A pass short of slots binds a scratch buffer pair in the rest (cleared, never written: no slot maps there).
			FRDGBufferRef SpareRows = nullptr;
			FRDGBufferRef SpareCounter = nullptr;
			for (int32 First = 0; First < FacadeSlots; First += FCSCityFacadeCS::SlotsPerPass)
			{
				const int32 Count = FMath::Min(FCSCityFacadeCS::SlotsPerPass, FacadeSlots - First);
				FCSCityFacadeCS::FParameters* Params = GraphBuilder.AllocParameters<FCSCityFacadeCS::FParameters>();
				Params->FacadeBuildingsF = BuildingsSRV;
				Params->FacadeBuildingSeeds = SeedsSRV;
				Params->FacadeRunsF = RunsFSRV;
				Params->FacadeRunsU = RunsUSRV;
				Params->FacadeNumRuns = NumRuns;
				Params->FacadeNumThreads = Threads;
				for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
				{
					Params->FacadeSlotMin[Slot] = Constants.SlotMin[Slot];
					Params->FacadeSlotSize[Slot] = Constants.SlotSize[Slot];
				}
				uint32 Capacity[FCSCityFacadeCS::SlotsPerPass] = {};
				FRDGBufferUAVRef* RowUAVs[FCSCityFacadeCS::SlotsPerPass] = { &Params->FacadeRows0, &Params->FacadeRows1, &Params->FacadeRows2, &Params->FacadeRows3, &Params->FacadeRows4 };
				FRDGBufferUAVRef* CounterUAVs[FCSCityFacadeCS::SlotsPerPass] = { &Params->FacadeCounter0, &Params->FacadeCounter1, &Params->FacadeCounter2, &Params->FacadeCounter3, &Params->FacadeCounter4 };
				for (int32 Local = 0; Local < FCSCityFacadeCS::SlotsPerPass; Local++)
				{
					FRDGBufferRef RowBuffer;
					FRDGBufferRef CounterBuffer;
					if (Local < Count)
					{
						RowBuffer = Rows[First + Local];
						CounterBuffer = Counters[First + Local];
						Capacity[Local] = Gpu->Slots[First + Local].Capacity;
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
				Params->FacadeSlotFirst = uint32(First);
				Params->FacadeSlotCount = uint32(Count);
				Params->FacadeCapacityLo = FUintVector4(Capacity[0], Capacity[1], Capacity[2], Capacity[3]);
				Params->FacadeCapacityHi = Capacity[4];
				Params->FacadeStorey = Constants.Storey;
				Params->FacadeGround = Constants.Ground;
				Params->FacadeCorner = Constants.Corner;
				Params->FacadeParapet = Constants.Parapet;
				Params->FacadeWindowChance = Constants.WindowChance;
				Params->FacadeGroundWindowChance = Constants.GroundWindowChance;
				Params->FacadeColumnGroup = Constants.ColumnGroup;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSCityFacade.Modules %d-%d", First, First + Count - 1), Shader, Params, FComputeShaderUtils::GetGroupCount(int32(Threads), 64));
			}
		}
		// The instanced component's passes only read them, and leave their state to whoever wrote them.
		for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
		{
			GraphBuilder.SetBufferAccessFinal(Rows[Slot], ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(Counters[Slot], ERHIAccess::SRVMask);
		}
		GraphBuilder.Execute();
		for (int32 Slot = 0; Slot < FacadeSlots; Slot++) Landed->Slots[Slot] = Gpu->Slots[Slot];
	});

	TArray<TWeakObjectPtr<UCSGpuInstancedMeshComponent>> Targets;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : Components) Targets.Add(Component.Get());
	CSRoad::AfterRenderThread(Owner.GetWorld(), [Targets = MoveTemp(Targets), Landed, Bounds]()
	{
		for (int32 Slot = 0; Slot < FacadeSlots; Slot++)
		{
			UCSGpuInstancedMeshComponent* Component = Targets[Slot].Get();
			if (!IsValid(Component)) continue;
			FCSGpuInstanceSourceGPU Source;
			Source.PackedInstances = Landed->Slots[Slot].Rows;
			Source.Counter = Landed->Slots[Slot].Counter;
			Source.Capacity = Landed->Slots[Slot].Capacity;
			Source.LocalBounds = Bounds.IsValid ? Bounds : FBox(FVector(-100.0), FVector(100.0));
			// Every write lands here and says so.
			Source.bNotifiesChanges = true;
			const FCSGpuInstanceSourceGPU& Held = Component->GetInstanceSourceGPU();
			const bool bSame = Held.PackedInstances == Source.PackedInstances && Held.Counter == Source.Counter && Held.Capacity == Source.Capacity && Held.LocalBounds.Equals(Source.LocalBounds) && Held.bNotifiesChanges;
			if (bSame) Component->NotifyInstanceSourceChanged();
			else Component->SetInstanceSourceGPU(Source);
		}
	});
}

void ReleaseFacade(TArray<TObjectPtr<UCSGpuInstancedMeshComponent>>& Components, TSharedPtr<FFacadeGpu>& Gpu)
{
	for (UCSGpuInstancedMeshComponent* Component : Components) if (Component) Component->DestroyComponent();
	Components.Reset();
	if (!Gpu) return;
	// The buffers go on the render thread, after any write still to run there.
	ENQUEUE_RENDER_COMMAND(CSCityFacadeRelease)([Released = MoveTemp(Gpu)](FRHICommandListImmediate&) mutable { Released.Reset(); });
	Gpu.Reset();
}
}

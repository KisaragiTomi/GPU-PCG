#include "CSCityStreetActor.h"

#include "CSCityBuildingActor.h"
#include "CSCityFacade.h"
#include "CSCityFacadeKit.h"
#include "CSCityLayout.h"
#include "CSGpuInstancedMeshComponent.h"
#include "CSRoadNet.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Hash/xxhash.h"
#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif

// What the street was last laid out from and into.
struct FCSCityStreetActor_Layout
{
	// The network's roads near it as built (world space), and the width of the road it follows (cm; 0: not known).
	TArray<CSCity::FRoadLine> Roads;
	double FollowedWidth = 0.0;
	// What the modules were last written from; none yet.
	uint64 Signature = 0;
	bool bWritten = false;
	TArray<CSCity::FLot> Lots;
	TArray<CSCity::FPolygon> Reservations;
};

namespace
{
uint64 CSCityStreet_Hash(const FString& Text)
{
	return FXxHash64::HashBuffer(*Text, Text.Len() * sizeof(TCHAR)).Hash;
}

uint64 CSCityStreet_Mix(uint64 A, uint64 B)
{
	const uint64 Values[] = { A, B };
	return FXxHash64::HashBuffer(Values, sizeof(Values)).Hash;
}

// [0, 1) from a hash.
double CSCityStreet_Unit(uint64 Hash)
{
	return double(Hash >> 11) * (1.0 / 9007199254740992.0);
}

// A spline's points about 2 m apart, world space, from From to To along it (cm).
TArray<FVector> CSCityStreet_Sample(const USplineComponent& Spline, double From, double To)
{
	TArray<FVector> Points;
	if (To - From < 1.0) return Points;
	const int32 Steps = FMath::Clamp(FMath::CeilToInt32((To - From) / 200.0), 1, 4096);
	for (int32 S = 0; S <= Steps; S++) Points.Add(Spline.GetLocationAtDistanceAlongSpline(FMath::Lerp(From, To, double(S) / Steps), ESplineCoordinateSpace::World));
	return Points;
}

const TCHAR* CSCityStreet_ShapeName(ECSCityMassShape Shape)
{
	switch (Shape)
	{
	case ECSCityMassShape::L: return TEXT("L");
	case ECSCityMassShape::U: return TEXT("U");
	case ECSCityMassShape::T: return TEXT("T");
	case ECSCityMassShape::Courtyard: return TEXT("courtyard");
	default: return TEXT("box");
	}
}

const TCHAR* CSCityStreet_StackName(ECSCityMassStack Stack)
{
	switch (Stack)
	{
	case ECSCityMassStack::Setback: return TEXT(", setback");
	case ECSCityMassStack::PodiumTower: return TEXT(", tower on a podium");
	default: return TEXT("");
	}
}
}

UCSCityStreetSplineComponent::UCSCityStreetSplineComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// A straight 60 m street to start from.
	ClearSplinePoints(false);
	AddSplinePoint(FVector(-3000.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	AddSplinePoint(FVector(3000.0, 0.0, 0.0), ESplineCoordinateSpace::Local, false);
	UpdateSpline();
}

void UCSCityStreetSplineComponent::UpdateSpline()
{
	Super::UpdateSpline();
	if (IsTemplate()) return;
	if (ACSCityStreetActor* Owner = Cast<ACSCityStreetActor>(GetOwner())) Owner->Rebuild();
}

ACSCityStreetActor::ACSCityStreetActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Street = CreateDefaultSubobject<UCSCityStreetSplineComponent>(TEXT("Street"));
	RootComponent = Street;
}

ACSCityStreetActor::~ACSCityStreetActor() = default;

ACSCityStreetActor::ACSCityStreetActor(FVTableHelper& Helper)
	: Super(Helper)
{
}

ACSRoadSplineActor* ACSCityStreetActor::FollowedRoad() const
{
	ACSRoadSplineActor* Road = bFollowRoad ? Cast<ACSRoadSplineActor>(GetAttachParentActor()) : nullptr;
	return Road && Road->Spline && Road->Spline->GetNumberOfSplinePoints() >= 2 ? Road : nullptr;
}

TArray<FVector> ACSCityStreetActor::CentreLine() const
{
	if (const ACSRoadSplineActor* Road = FollowedRoad())
	{
		const double Length = Road->Spline->GetSplineLength();
		const double From = FMath::Clamp(double(FollowFrom) * 100.0, 0.0, Length);
		const double To = FollowTo > 0.0f ? FMath::Clamp(double(FollowTo) * 100.0, From, Length) : Length;
		return CSCityStreet_Sample(*Road->Spline, From, To);
	}
	return Street && Street->GetNumberOfSplinePoints() >= 2 ? CSCityStreet_Sample(*Street, 0.0, Street->GetSplineLength()) : TArray<FVector>();
}

const TArray<TArray<FVector2D>>& ACSCityStreetActor::Reservations() const
{
	static const TArray<TArray<FVector2D>> None;
	return Layout ? Layout->Reservations : None;
}

TArray<TArray<FVector2D>> ACSCityStreetActor::PlotCorners() const
{
	TArray<TArray<FVector2D>> Out;
	if (Layout) for (const CSCity::FLot& Lot : Layout->Lots) Out.Add(Lot.Corners);
	return Out;
}

void ACSCityStreetActor::Rebuild()
{
	if (!Street || IsTemplate() || !GetWorld()) return;
	ACSRoadNetworkActor* Network = ACSRoadNetworkActor::NetworkOf(this);
	// With no network, nobody hands it roads.
	if (!Network && Layout)
	{
		Layout->Roads.Reset();
		Layout->FollowedWidth = 0.0;
	}
	Relayout();
	if (Network) Network->RequestRebuild();
	else RelayoutStreets(GetLevel(), this);
}

bool ACSCityStreetActor::UpdateFromNetwork(const TArray<CSCity::FRoadLine>& Roads, double FollowedWidth)
{
	if (!Layout) Layout = MakeUnique<FCSCityStreetActor_Layout>();
	Layout->Roads = Roads;
	Layout->FollowedWidth = FollowedWidth;
	return Relayout();
}

void ACSCityStreetActor::RelayoutStreets(const ULevel* Level, const ACSCityStreetActor* After)
{
	UWorld* World = Level ? Level->GetWorld() : nullptr;
	if (!World) return;
	// A street gives way to the plots of those before it (by name): only those after After change.
	const FString From = After ? After->GetPathName() : FString();
	TArray<ACSCityStreetActor*> Streets;
	for (ACSCityStreetActor* Street : TActorRange<ACSCityStreetActor>(World)) if (Street != After && Street->GetLevel() == Level && (!After || Street->GetPathName() > From)) Streets.Add(Street);
	Streets.Sort([](const ACSCityStreetActor& A, const ACSCityStreetActor& B) { return A.GetPathName() < B.GetPathName(); });
	for (ACSCityStreetActor* Street : Streets) Street->Relayout();
}

bool ACSCityStreetActor::Relayout()
{
	if (!Street || IsTemplate() || !GetWorld()) return false;
	if (!Layout) Layout = MakeUnique<FCSCityStreetActor_Layout>();
	FCSCityStreetActor_Layout& State = *Layout;
	UCSCityFacadeKit* Used = Kit ? Kit.Get() : UCSCityFacadeKit::Default();
	const ACSRoadSplineActor* Road = FollowedRoad();
	const FString Identity = GetPathName(GetOuter());

	// The street: its centre line, its half width and its own road (a plot never makes way for that).
	CSCity::FRoadLine Anchor;
	Anchor.Id = CSCityStreet_Hash(Identity);
	Anchor.Points = CentreLine();
	Anchor.Setback = double(Setback) * 100.0;
	Anchor.Sides = Sides == ECSCityStreetSides::Right ? 1 : Sides == ECSCityStreetSides::Left ? 2 : 3;
	double HalfWidth = double(StreetWidth) * 50.0;
	if (Road)
	{
		const USplineComponent* RoadSpline = Road->Spline;
		// As the network names the spline it builds (its roads' group).
		Anchor.Group = CSCityStreet_Hash(RoadSpline->GetPathName(Road->GetOuter()));
		double Width = State.FollowedWidth;
		if (Width <= 0.0) Width = ACSRoadNetworkActor::RoadWidth(ACSRoadNetworkActor::NetworkOf(this), Road->Spline->Prefab);
		HalfWidth = Width * 0.5;
		const double Length = RoadSpline->GetSplineLength() * 0.01;
		Status = FString::Printf(TEXT("Follows %s (%s, %.1f m wide) from %.0f to %.0f m"), *Road->GetActorNameOrLabel(), *Road->Spline->Prefab, Width * 0.01, FMath::Min(double(FollowFrom), Length),
			FollowTo > 0.0f ? FMath::Clamp(double(FollowTo), double(FollowFrom), Length) : Length);
		if (Width <= 0.0) Status += TEXT(" (no width known for its prefab: lined from its centre)");
	}
	else
	{
		Status = FString::Printf(TEXT("Own spline, %.0f m"), Street->GetSplineLength() * 0.01);
		if (bFollowRoad && GetAttachParentActor()) Status += TEXT(" (hung under no road to follow)");
	}
	Anchor.HalfWidth = HalfWidth;

	CSCity::FLotSettings Settings;
	Settings.Frontage = double(Frontage) * 100.0;
	Settings.Gap = FMath::Min(double(Gap), double(Frontage) * 0.5) * 100.0;
	Settings.Setback = double(Setback) * 100.0;
	Settings.MinDepth = double(MinDepth) * 100.0;
	Settings.MaxDepth = double(FMath::Max(MinDepth, MaxDepth)) * 100.0;
	Settings.bFill = true;
	Settings.FillDepth = Settings.MaxDepth;
	Settings.Seed = Seed;

	// What stands in the way: the network's roads, the special buildings' plots and those of the streets before it.
	TArray<CSCity::FPolygon> Reserved;
	UWorld* World = GetWorld();
	for (ACSCityBuildingActor* Building : TActorRange<ACSCityBuildingActor>(World))
	{
		if (Building->GetLevel() != GetLevel()) continue;
		CSCity::FPolygon Plot = Building->Reservation();
		if (Plot.Num() >= 3) Reserved.Add(MoveTemp(Plot));
	}
	const FString Mine = GetPathName();
	for (ACSCityStreetActor* Other : TActorRange<ACSCityStreetActor>(World))
	{
		if (Other == this || Other->GetLevel() != GetLevel() || !(Other->GetPathName() < Mine)) continue;
		for (CSCity::FPolygon& Plot : Other->PlotCorners()) if (Plot.Num() >= 3) Reserved.Add(MoveTemp(Plot));
	}

	// Each plot on the ground below it, else on the street.
	FCollisionQueryParams Query(SCENE_QUERY_STAT(CSCityStreetGround), false, this);
	if (Road) Query.AddIgnoredActor(Road);
	auto Place = [this, World, &Query](CSCity::FLot& Lot)
	{
		if (!bOnGround) return true;
		double Lowest = TNumericLimits<double>::Max();
		TArray<FVector2D> Probes = Lot.Corners;
		Probes.Add(Lot.Centre);
		for (const FVector2D& Probe : Probes)
		{
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, FVector(Probe, Lot.Base + 5000.0), FVector(Probe, Lot.Base - 10000.0), ECC_WorldStatic, Query)) Lowest = FMath::Min(Lowest, Hit.ImpactPoint.Z);
		}
		if (Lowest < TNumericLimits<double>::Max()) Lot.Base = Lowest - 50.0;
		return true;
	};
	State.Lots = Anchor.Points.Num() >= 2 && Settings.Frontage > Settings.Gap ? CSCity::LayoutLots({ Anchor }, State.Roads, Reserved, Settings, Place) : TArray<CSCity::FLot>();
	State.Lots.Sort([](const CSCity::FLot& A, const CSCity::FLot& B) { return A.Id < B.Id; });

	// Which plot is which: its side and slot (slots are no narrower than two thirds of Frontage).
	double Length = 0.0;
	for (int32 I = 1; I < Anchor.Points.Num(); I++) Length += FVector2D::Distance(FVector2D(Anchor.Points[I - 1]), FVector2D(Anchor.Points[I]));
	TMap<uint64, TPair<ECSCityStreetSide, int32>> Slots;
	const int32 MaxSlots = Settings.Frontage > 0.0 ? FMath::CeilToInt32(Length / (Settings.Frontage * 2.0 / 3.0)) + 2 : 0;
	for (int32 Slot = 0; Slot < MaxSlots; Slot++)
	{
		Slots.Add(CSCity::LotId(Anchor.Id, Slot, true), { ECSCityStreetSide::Right, Slot });
		Slots.Add(CSCity::LotId(Anchor.Id, Slot, false), { ECSCityStreetSide::Left, Slot });
	}

	// What stands on each: its storeys (drawn log-normal between the median and the tenth from the top, scaled by the
	// own spline's points), its massing, and whether it stands at all.
	const double StoreyHeight = Used ? FMath::Max(double(Used->StoreyHeight), 100.0) : 400.0;
	const double Median = FMath::Max(double(MedianStoreys), 1.0);
	const double Spread = FMath::Loge(FMath::Max(double(P90Storeys) / Median, 1.0)) / 1.2815516;
	const uint64 Salt = CSCityStreet_Mix(uint64(uint32(Seed)), 0x57EE7ull);
	const CSCity::FFacadeKitShape Shape = Used ? CSCity::KitShape(*Used) : CSCity::FFacadeKitShape();
	TArray<CSCity::FFacadeBuilding> Buildings;
	Plots.Reset();
	FXxHash64Builder Signature;
	const uint64 KitHash = Used ? CSCity::KitHash(*Used) : 0;
	Signature.Update(&KitHash, sizeof(KitHash));
	const FVector Origin = GetActorLocation();
	Signature.Update(&Origin, sizeof(Origin));
	for (CSCity::FLot& Lot : State.Lots)
	{
		const TPair<ECSCityStreetSide, int32>* Key = Slots.Find(Lot.Id);
		const FCSCityStreetLot* Override = Key ? Overrides.FindByPredicate([Key](const FCSCityStreetLot& One) { return One.Side == Key->Key && One.Slot == Key->Value; }) : nullptr;
		const FString Name = Key ? FString::Printf(TEXT("%s%d"), Key->Key == ECSCityStreetSide::Right ? TEXT("R") : TEXT("L"), Key->Value) : TEXT("?");
		const uint64 Draw = CSCityStreet_Mix(Lot.Id, Salt);
		const bool bOpen = Override ? Override->bEmpty : CSCityStreet_Unit(CSCityStreet_Mix(Draw, 1)) < double(Vacancy);
		if (bOpen || !Used)
		{
			Plots.Add(FString::Printf(TEXT("%s: open, %.1f x %.1f m"), *Name, Lot.Width * 0.01, Lot.Depth * 0.01));
			continue;
		}
		int32 Storeys = Override && Override->Storeys > 0 ? Override->Storeys : 0;
		if (Storeys == 0)
		{
			// Box-Muller: a standard normal from two draws.
			const double U1 = FMath::Max(CSCityStreet_Unit(CSCityStreet_Mix(Draw, 2)), 1e-12);
			const double U2 = CSCityStreet_Unit(CSCityStreet_Mix(Draw, 3));
			const double Z = FMath::Sqrt(-2.0 * FMath::Loge(U1)) * FMath::Cos(2.0 * UE_DOUBLE_PI * U2);
			double Scale = 1.0;
			if (!Road && Street) Scale = FMath::Max(double(Street->GetScaleAtSplineInputKey(Street->FindInputKeyClosestToWorldLocation(FVector(Lot.Centre, Lot.Base))).Z), 0.0);
			Storeys = FMath::Clamp(FMath::RoundToInt32(Median * FMath::Exp(Z * Spread) * Scale), MinStoreys, FMath::Max(MinStoreys, MaxStoreys));
		}
		Lot.Height = Storeys * StoreyHeight;
		CSCity::FFacadeBuilding Building = CSCity::LotBuilding(Lot);
		Building.Seed = uint32(Draw ^ (Draw >> 32));
		Building.DoorAt = FMath::Lerp(0.2, 0.8, CSCityStreet_Unit(CSCityStreet_Mix(Draw, 4)));
		Building.Shape = Override ? Override->MassShape : ECSCityMassShape::Auto;
		Building.Stack = Override ? Override->MassStack : ECSCityMassStack::Auto;
		if (Building.Stack == ECSCityMassStack::Auto && bOverrideStacking)
		{
			if (Storeys >= 6 && CSCityStreet_Unit(CSCityStreet_Mix(Draw, 5)) < double(PodiumChance))
			{
				Building.Stack = ECSCityMassStack::PodiumTower;
				// A tower rises from a whole podium, as the kit's own draw has it.
				if (Building.Shape == ECSCityMassShape::Auto) Building.Shape = ECSCityMassShape::Box;
			}
			else Building.Stack = Storeys >= 4 && CSCityStreet_Unit(CSCityStreet_Mix(Draw, 6)) < double(SetbackChance) ? ECSCityMassStack::Setback : ECSCityMassStack::None;
		}
		const CSCity::FFacadeMassing Massing = CSCity::MassFacade(Shape, Building);
		Plots.Add(FString::Printf(TEXT("%s: %d storeys, %.1f x %.1f m, %s%s"), *Name, Massing.Storeys, Lot.Width * 0.01, Lot.Depth * 0.01, CSCityStreet_ShapeName(Massing.Shape), CSCityStreet_StackName(Massing.Stack)));
		Signature.Update(&Building.Block, sizeof(Building.Block));
		Signature.Update(&Building.Seed, sizeof(Building.Seed));
		Signature.Update(&Building.DoorAt, sizeof(Building.DoorAt));
		Signature.Update(&Building.Shape, sizeof(Building.Shape));
		Signature.Update(&Building.Stack, sizeof(Building.Stack));
		Buildings.Add(Building);
	}
	NumBuildings = Buildings.Num();

	// The ground it keeps: its strip on each side it builds on, in pieces about 8 m along, from the street's edge back as
	// deep as a plot may be and a Gap; and its plots, which a curve may carry past the strip.
	State.Reservations.Reset();
	const double Near = HalfWidth;
	const double Far = HalfWidth + Settings.Setback + Settings.MaxDepth + Settings.Gap;
	const int32 Every = 4;
	for (int32 I = 0; I + 1 < Anchor.Points.Num(); I += Every)
	{
		const FVector2D A(Anchor.Points[I]);
		const FVector2D B(Anchor.Points[FMath::Min(I + Every, Anchor.Points.Num() - 1)]);
		const FVector2D Tangent = (B - A).GetSafeNormal();
		if (Tangent.IsNearlyZero()) continue;
		const FVector2D Right(-Tangent.Y, Tangent.X);
		for (const double Side : { 1.0, -1.0 })
		{
			if ((Anchor.Sides & (Side > 0.0 ? 1 : 2)) == 0) continue;
			const FVector2D N = Right * Side;
			CSCity::FPolygon Piece = CSCity::ConvexHull({ A + N * Near, B + N * Near, B + N * Far, A + N * Far });
			if (Piece.Num() >= 3) State.Reservations.Add(MoveTemp(Piece));
		}
	}
	for (const CSCity::FLot& Lot : State.Lots) if (Lot.Corners.Num() >= 3) State.Reservations.Add(Lot.Corners);

	const uint64 Value = Signature.Finalize().Hash;
	bool bComponents = Buildings.Num() == 0;
	for (const UCSGpuInstancedMeshComponent* Component : Facade) bComponents |= IsValid(Component);
	if (State.bWritten && State.Signature == Value && bComponents) return false;
	State.Signature = Value;
	State.bWritten = true;
	if (!Used || Buildings.Num() == 0)
	{
		CSCity::ReleaseFacade(Facade, FacadeGpu);
		return true;
	}
	CSCity::WriteFacade(*this, Street, TEXT("Facade"), Origin, *Used, Buildings, Facade, FacadeGpu, /*bFollowParent*/ true);
	return true;
}

void ACSCityStreetActor::ApplyFabric(ECSCityFabric Preset)
{
	// From the fabrics' plot coverage and roof heights (median and nine in ten, 4 m storeys) in NightCity_BuildingKit.md.
	struct FFabric
	{
		float Frontage, Gap, Setback, MinDepth, MaxDepth, Vacancy, Median, P90;
		int32 MinStoreys, MaxStoreys;
		float Podium, Stepped;
	};
	FFabric Values;
	switch (Preset)
	{
	case ECSCityFabric::LowScatter: Values = { 14.0f, 6.0f, 4.0f, 8.0f, 12.0f, 0.35f, 2.25f, 3.5f, 1, 4, 0.0f, 0.1f }; break;
	case ECSCityFabric::MidMixed: Values = { 18.0f, 3.0f, 3.0f, 10.0f, 18.0f, 0.2f, 4.0f, 9.0f, 1, 14, 0.1f, 0.35f }; break;
	case ECSCityFabric::StreetWall: Values = { 15.0f, 0.0f, 2.0f, 14.0f, 22.0f, 0.03f, 6.25f, 16.75f, 3, 30, 0.1f, 0.45f }; break;
	case ECSCityFabric::HighCore: Values = { 30.0f, 0.0f, 3.0f, 24.0f, 36.0f, 0.0f, 22.25f, 44.5f, 6, 80, 0.6f, 0.3f }; break;
	default: return;
	}
	Frontage = Values.Frontage;
	Gap = Values.Gap;
	Setback = Values.Setback;
	MinDepth = Values.MinDepth;
	MaxDepth = Values.MaxDepth;
	Vacancy = Values.Vacancy;
	MedianStoreys = Values.Median;
	P90Storeys = Values.P90;
	MinStoreys = Values.MinStoreys;
	MaxStoreys = Values.MaxStoreys;
	bOverrideStacking = true;
	PodiumChance = Values.Podium;
	SetbackChance = Values.Stepped;
}

void ACSCityStreetActor::FollowNearestRoad()
{
	UWorld* World = GetWorld();
	if (!World || !Street) return;
	const FVector Start = Street->GetLocationAtSplinePoint(0, ESplineCoordinateSpace::World);
	ACSRoadSplineActor* Best = nullptr;
	double BestDistance = TNumericLimits<double>::Max();
	for (ACSRoadSplineActor* Road : TActorRange<ACSRoadSplineActor>(World))
	{
		if (Road->GetLevel() != GetLevel() || !Road->Spline || Road->Spline->GetNumberOfSplinePoints() < 2) continue;
		const double Distance = FVector::Dist2D(Start, Road->Spline->FindLocationClosestToWorldLocation(Start, ESplineCoordinateSpace::World));
		if (Distance < BestDistance)
		{
			BestDistance = Distance;
			Best = Road;
		}
	}
	if (!Best)
	{
		UE_LOG(LogCSRoadNet, Warning, TEXT("%s: no road in the level to follow"), *GetName());
		return;
	}
#if WITH_EDITOR
	const FScopedTransaction Transaction(NSLOCTEXT("CSRoadNet", "FollowNearestRoad", "Follow Nearest Road"));
#endif
	Modify();
	AttachToActor(Best, FAttachmentTransformRules::KeepWorldTransform);
	bFollowRoad = true;
	Rebuild();
}

void ACSCityStreetActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	Rebuild();
}

void ACSCityStreetActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// The modules are made at build time and never saved: a loaded or copied street writes them.
	if (!IsTemplate() && GetWorld() && Facade.Num() == 0) Rebuild();
}

void ACSCityStreetActor::Destroyed()
{
	CSCity::ReleaseFacade(Facade, FacadeGpu);
	if (Layout)
	{
		Layout->Lots.Reset();
		Layout->Reservations.Reset();
		Layout->bWritten = false;
	}
	if (ACSRoadNetworkActor* Network = ACSRoadNetworkActor::NetworkOf(this)) Network->RequestRebuild();
	else RelayoutStreets(GetLevel(), this);
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSCityStreetActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// Before the construction script runs again (in Super), which lays the street out.
	if (PropertyChangedEvent.GetMemberPropertyName() == GET_MEMBER_NAME_CHECKED(ACSCityStreetActor, Fabric)) ApplyFabric(Fabric);
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void ACSCityStreetActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	Rebuild();
}

void ACSCityStreetActor::PostEditUndo()
{
	Super::PostEditUndo();
	Rebuild();
}
#endif

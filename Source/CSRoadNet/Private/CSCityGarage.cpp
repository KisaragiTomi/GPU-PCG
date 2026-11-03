#include "CSCityGarage.h"

#include "CSRoadAssets.h"

namespace
{
// A spline segment as a cubic Bezier, as the network reads it: Hermite tangents per unit of the segment parameter.
struct FCSCityGarage_Curve
{
	FVector3d A;
	FVector3d B;
	FVector3d C;
	FVector3d D;

	FVector3d At(double T) const
	{
		const double U = 1.0 - T;
		return A * (U * U * U) + B * (3.0 * U * U * T) + C * (3.0 * U * T * T) + D * (T * T * T);
	}
	FVector3d Tangent(double T) const
	{
		const double U = 1.0 - T;
		return (B - A) * (3.0 * U * U) + (C - B) * (6.0 * U * T) + (D - C) * (3.0 * T * T);
	}
	// At least the curve's length.
	double Length() const { return FVector3d::Distance(A, B) + FVector3d::Distance(B, C) + FVector3d::Distance(C, D); }
};

FCSCityGarage_Curve CSCityGarage_Segment(const CSRoad::FSplineRoad& Road, int32 I)
{
	const int32 J = (I + 1) % Road.Points.Num();
	const FVector3d& A = Road.Points[I];
	const FVector3d& D = Road.Points[J];
	if (Road.Linear.IsValidIndex(I) && Road.Linear[I]) return { A, FMath::Lerp(A, D, 1.0 / 3.0), FMath::Lerp(A, D, 2.0 / 3.0), D };
	return { A, A + Road.Leave[I] / 3.0, D - Road.Arrive[J] / 3.0, D };
}

FVector2d CSCityGarage_Flat(const FVector3d& V)
{
	return FVector2d(V.X, V.Z);
}

// Inside a convex outline of either winding, or on it.
bool CSCityGarage_Inside(const TArray<FVector2d>& Outline, const FVector2d& P)
{
	if (Outline.Num() < 3) return false;
	int32 Sign = 0;
	for (int32 I = 0; I < Outline.Num(); I++)
	{
		const FVector2d& A = Outline[I];
		const FVector2d& B = Outline[(I + 1) % Outline.Num()];
		const double Cross = FVector2d::CrossProduct(B - A, P - A);
		const int32 Side = Cross > 0.0 ? 1 : Cross < 0.0 ? -1 : 0;
		if (Side == 0) continue;
		if (Sign == 0) Sign = Side;
		else if (Side != Sign) return false;
	}
	return true;
}

double CSCityGarage_Distance(const TArray<FVector2d>& Outline, const FVector2d& P)
{
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Outline.Num(); I++)
	{
		const FVector2d& A = Outline[I];
		const FVector2d& B = Outline[(I + 1) % Outline.Num()];
		const FVector2d AB = B - A;
		const double T = AB.SizeSquared() > 0.0 ? FMath::Clamp(FVector2d::DotProduct(P - A, AB) / AB.SizeSquared(), 0.0, 1.0) : 0.0;
		Best = FMath::Min(Best, FVector2d::Distance(P, A + AB * T));
	}
	return Best;
}
}

bool CSCity::Driveway(const FGarage& Garage, const TArray<CSRoad::FSplineRoad>& Splines, const TArray<FObstacle>& Obstacles, CSRoad::FAssets& Assets,
	CSRoad::FSplineRoad& Out, FString& OutWhy)
{
	if (!Assets.HasRoad(Garage.Prefab))
	{
		OutWhy = FString::Printf(TEXT("no road prefab \"%s\" for the driveway"), *Garage.Prefab);
		return false;
	}
	// The nearest point of a main road in front of the door within reach, about every quarter metre, then closer around it.
	const FVector2d Door = CSCityGarage_Flat(Garage.Door);
	constexpr double MinCosine = 0.5;
	double Best = Garage.Reach;
	const CSRoad::FSplineRoad* Target = nullptr;
	FCSCityGarage_Curve Curve;
	double At = 0.0;
	double Step = 0.0;
	for (const CSRoad::FSplineRoad& Road : Splines)
	{
		const int32 Count = Road.Points.Num();
		if (Road.bAlley || Count < 2 || Road.Arrive.Num() != Count || Road.Leave.Num() != Count || !Assets.HasRoad(Road.Prefab)) continue;
		for (int32 I = 0; I < (Road.bClosed ? Count : Count - 1); I++)
		{
			const FCSCityGarage_Curve Segment = CSCityGarage_Segment(Road, I);
			// A Bezier lies within its control points' box: a box farther than the best so far holds nothing nearer.
			FBox2d Box(ForceInit);
			for (const FVector3d& Control : { Segment.A, Segment.B, Segment.C, Segment.D }) Box += CSCityGarage_Flat(Control);
			if (Box.ComputeSquaredDistanceToPoint(Door) >= Best * Best) continue;
			const int32 Steps = FMath::Clamp(FMath::CeilToInt32(Segment.Length() * 4.0), 8, 4096);
			for (int32 S = 0; S <= Steps; S++)
			{
				const FVector2d To = CSCityGarage_Flat(Segment.At(double(S) / Steps)) - Door;
				const double Distance = To.Length();
				if (Distance >= Best || Distance < UE_KINDA_SMALL_NUMBER || FVector2d::DotProduct(To / Distance, Garage.Facing) < MinCosine) continue;
				Best = Distance;
				Target = &Road;
				Curve = Segment;
				At = double(S) / Steps;
				Step = 1.0 / Steps;
			}
		}
	}
	if (!Target)
	{
		OutWhy = FString::Printf(TEXT("no main road within %.0f m in front of the garage"), Garage.Reach);
		return false;
	}
	double Low = FMath::Max(At - Step, 0.0);
	double High = FMath::Min(At + Step, 1.0);
	for (int32 K = 0; K < 40; K++)
	{
		const double Left = Low + (High - Low) / 3.0;
		const double Right = High - (High - Low) / 3.0;
		if ((CSCityGarage_Flat(Curve.At(Left)) - Door).SizeSquared() < (CSCityGarage_Flat(Curve.At(Right)) - Door).SizeSquared()) High = Right;
		else Low = Left;
	}
	At = (Low + High) * 0.5;
	const FVector3d Foot = Curve.At(At);
	FVector2d Along = CSCityGarage_Flat(Curve.Tangent(At)).GetSafeNormal();
	if (Along.IsNearlyZero()) Along = CSCityGarage_Flat(Curve.D - Curve.A).GetSafeNormal();
	// The road's side the door is on.
	FVector2d Normal(-Along.Y, Along.X);
	if (FVector2d::DotProduct(Normal, Door - CSCityGarage_Flat(Foot)) < 0.0) Normal = -Normal;
	const double HalfWidth = double(Assets.Geometry(Garage.Prefab).m_DefaultWidth) * 0.5;
	const double RoadHalfWidth = double(Assets.Geometry(Target->Prefab).m_DefaultWidth) * 0.5;
	const double Length = (CSCityGarage_Flat(Foot) - Door).Length();
	// The network cuts the driveway back to its half width and the clearance past the road's edge, and joins it on the
	// centre line from there. A door nearer than that and the driveway's width opens onto the road: no driveway.
	if (Length < RoadHalfWidth + HalfWidth * 2.0 + Garage.Clearance)
	{
		Out = CSRoad::FSplineRoad();
		OutWhy = FString::Printf(TEXT("opens onto %s, %.1f m from the door"), *Target->Name, Length);
		return true;
	}

	// Out of the door square, into the road square; the handles a third of the way.
	const FVector3d Ahead(Garage.Facing.X, 0.0, Garage.Facing.Y);
	const FVector3d Into(-Normal.X, 0.0, -Normal.Y);
	CSRoad::FSplineRoad Road;
	Road.Name = Garage.Name;
	Road.Id = Garage.Id;
	Road.Prefab = Garage.Prefab;
	Road.bAlley = true;
	Road.bPlots = false;
	Road.Points = { Garage.Door, Foot };
	Road.Arrive = { Ahead * Length, Into * Length };
	Road.Leave = Road.Arrive;
	Road.Linear = { false, false };
	Road.Data.SetNum(2);
	// Clear of the footprints: of its own (the way leaving it at the door), and by half its width of the others.
	const FCSCityGarage_Curve Way = CSCityGarage_Segment(Road, 0);
	const int32 Steps = FMath::Clamp(FMath::CeilToInt32(Way.Length() * 2.0), 8, 2048);
	for (int32 S = 1; S <= Steps; S++)
	{
		const FVector2d P = CSCityGarage_Flat(Way.At(double(S) / Steps));
		for (int32 O = 0; O < Obstacles.Num(); O++)
		{
			const TArray<FVector2d>& Outline = Obstacles[O].Outline;
			if (!CSCityGarage_Inside(Outline, P) && (O == Garage.Own || CSCityGarage_Distance(Outline, P) >= HalfWidth)) continue;
			OutWhy = FString::Printf(TEXT("the way to %s runs through %s"), *Target->Name, *Obstacles[O].Name);
			return false;
		}
	}
	Out = MoveTemp(Road);
	OutWhy = FString::Printf(TEXT("joins %s, %.1f m from the door"), *Target->Name, Length);
	return true;
}

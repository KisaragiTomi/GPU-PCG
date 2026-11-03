#include "CSRoadSplineNetwork.h"

#include "CSRoadCurves.h"
#include "CSRoadNet.h"

namespace CSRoad
{
namespace
{
// Splines are worked on in double: a fixed spline's points and tangents, read back as they were
// written, give its edges' float control points bit for bit.
struct FCSRoadSplineNetwork_Curve
{
	FVector3d A = FVector3d::ZeroVector;
	FVector3d B = FVector3d::ZeroVector;
	FVector3d C = FVector3d::ZeroVector;
	FVector3d D = FVector3d::ZeroVector;

	FVector3d Position(double T) const
	{
		const double S = 1.0 - T;
		return A * (S * S * S) + B * (3.0 * S * S * T) + C * (3.0 * S * T * T) + D * (T * T * T);
	}

	FVector3d Tangent(double T) const
	{
		const double S = 1.0 - T;
		return (B - A) * (3.0 * S * S) + (C - B) * (6.0 * S * T) + (D - C) * (3.0 * T * T);
	}

	FVector3d Curvature(double T) const { return ((C - B * 2.0 + A) * (1.0 - T) + (D - C * 2.0 + B) * T) * 6.0; }

	void Split(double T, FCSRoadSplineNetwork_Curve& Left, FCSRoadSplineNetwork_Curve& Right) const
	{
		const FVector3d AB = FMath::Lerp(A, B, T);
		const FVector3d BC = FMath::Lerp(B, C, T);
		const FVector3d CD = FMath::Lerp(C, D, T);
		const FVector3d ABC = FMath::Lerp(AB, BC, T);
		const FVector3d BCD = FMath::Lerp(BC, CD, T);
		const FVector3d P = FMath::Lerp(ABC, BCD, T);
		Left = { A, AB, ABC, P };
		Right = { P, BCD, CD, D };
	}

	// The part from T0 to T1.
	FCSRoadSplineNetwork_Curve Cut(double T0, double T1) const
	{
		FCSRoadSplineNetwork_Curve Head = *this;
		FCSRoadSplineNetwork_Curve Tail;
		if (T1 < 1.0) Split(T1, Head, Tail);
		if (T0 <= 0.0) return Head;
		FCSRoadSplineNetwork_Curve Part;
		Head.Split(T0 / T1, Tail, Part);
		return Part;
	}

	// Moves an end together with its control point, so the end tangent stays.
	void MoveStart(const FVector3d& To)
	{
		B += To - A;
		A = To;
	}

	void MoveEnd(const FVector3d& To)
	{
		C += To - D;
		D = To;
	}

	FBox3d Box() const
	{
		FBox3d Result(A, A);
		Result += B;
		Result += C;
		Result += D;
		return Result;
	}
};

constexpr int32 CSRoadSplineNetwork_Samples = 64;

double CSRoadSplineNetwork_Length(const FCSRoadSplineNetwork_Curve& Curve)
{
	double Length = 0.0;
	FVector3d Previous = Curve.A;
	for (int32 I = 1; I <= CSRoadSplineNetwork_Samples; I++)
	{
		const FVector3d Point = Curve.Position(double(I) / CSRoadSplineNetwork_Samples);
		Length += FVector3d::Dist(Previous, Point);
		Previous = Point;
	}
	return Length;
}

// How much the heading changes along the curve in the ground plane, in radians.
double CSRoadSplineNetwork_Turn(const FCSRoadSplineNetwork_Curve& Curve)
{
	double Turn = 0.0;
	double Previous = 0.0;
	bool bPrevious = false;
	for (int32 I = 0; I <= CSRoadSplineNetwork_Samples; I++)
	{
		const FVector3d Tangent = Curve.Tangent(double(I) / CSRoadSplineNetwork_Samples);
		if (FMath::Square(Tangent.X) + FMath::Square(Tangent.Z) < 1e-12) continue;
		const double Heading = FMath::Atan2(Tangent.Z, Tangent.X);
		if (bPrevious) Turn += FMath::Abs(FMath::UnwindRadians(Heading - Previous));
		Previous = Heading;
		bPrevious = true;
	}
	return Turn;
}

// The parameters dividing the curve into Count parts of equal length.
TArray<double> CSRoadSplineNetwork_EqualParts(const FCSRoadSplineNetwork_Curve& Curve, int32 Count)
{
	constexpr int32 Steps = 256;
	TArray<double> Distance;
	Distance.SetNumUninitialized(Steps + 1);
	Distance[0] = 0.0;
	FVector3d Previous = Curve.A;
	for (int32 I = 1; I <= Steps; I++)
	{
		const FVector3d Point = Curve.Position(double(I) / Steps);
		Distance[I] = Distance[I - 1] + FVector3d::Dist(Previous, Point);
		Previous = Point;
	}
	TArray<double> Params;
	int32 Step = 0;
	for (int32 Part = 1; Part < Count; Part++)
	{
		const double Target = Distance[Steps] * Part / Count;
		while (Step < Steps - 1 && Distance[Step + 1] < Target) Step++;
		const double Span = Distance[Step + 1] - Distance[Step];
		Params.Add((Step + (Span > 0.0 ? (Target - Distance[Step]) / Span : 0.0)) / Steps);
	}
	return Params;
}

// The parameter of the curve point nearest to Point.
double CSRoadSplineNetwork_Nearest(const FCSRoadSplineNetwork_Curve& Curve, const FVector3d& Point)
{
	double T = 0.0;
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I <= CSRoadSplineNetwork_Samples; I++)
	{
		const double Candidate = double(I) / CSRoadSplineNetwork_Samples;
		const double Distance = FVector3d::DistSquared(Curve.Position(Candidate), Point);
		if (Distance >= Best) continue;
		Best = Distance;
		T = Candidate;
	}
	// Newton on (P(t) - Point) . P'(t) = 0, kept only where it gets closer.
	double Refined = T;
	for (int32 Iteration = 0; Iteration < 8; Iteration++)
	{
		const FVector3d Offset = Curve.Position(Refined) - Point;
		const FVector3d Tangent = Curve.Tangent(Refined);
		const double Slope = Tangent.Dot(Tangent) + Offset.Dot(Curve.Curvature(Refined));
		if (FMath::Abs(Slope) < 1e-12) break;
		Refined = FMath::Clamp(Refined - Offset.Dot(Tangent) / Slope, 0.0, 1.0);
	}
	return FVector3d::DistSquared(Curve.Position(Refined), Point) < Best ? Refined : T;
}

// Where two curves cross in the ground plane, as parameter pairs.
void CSRoadSplineNetwork_Crossings(const FCSRoadSplineNetwork_Curve& P, const FCSRoadSplineNetwork_Curve& Q, double LengthP, double LengthQ, TArray<FVector2d>& Out)
{
	auto Flatten = [](const FCSRoadSplineNetwork_Curve& Curve, double Length)
	{
		const int32 Count = FMath::Clamp(FMath::CeilToInt32(Length / 2.0), 16, 256);
		TArray<FVector2d> Points;
		Points.SetNumUninitialized(Count + 1);
		for (int32 I = 0; I <= Count; I++)
		{
			const FVector3d Point = Curve.Position(double(I) / Count);
			Points[I] = FVector2d(Point.X, Point.Z);
		}
		return Points;
	};
	const TArray<FVector2d> SP = Flatten(P, LengthP);
	const TArray<FVector2d> SQ = Flatten(Q, LengthQ);
	const int32 NP = SP.Num() - 1;
	const int32 NQ = SQ.Num() - 1;
	for (int32 I = 0; I < NP; I++)
	{
		const FVector2d A0 = SP[I];
		const FVector2d A1 = SP[I + 1];
		for (int32 J = 0; J < NQ; J++)
		{
			const FVector2d B0 = SQ[J];
			const FVector2d B1 = SQ[J + 1];
			if (FMath::Max(A0.X, A1.X) < FMath::Min(B0.X, B1.X) || FMath::Max(B0.X, B1.X) < FMath::Min(A0.X, A1.X)) continue;
			if (FMath::Max(A0.Y, A1.Y) < FMath::Min(B0.Y, B1.Y) || FMath::Max(B0.Y, B1.Y) < FMath::Min(A0.Y, A1.Y)) continue;
			const FVector2d DA = A1 - A0;
			const FVector2d DB = B1 - B0;
			const double Denominator = DA.X * DB.Y - DA.Y * DB.X;
			if (FMath::Abs(Denominator) < 1e-12) continue;
			const FVector2d W = B0 - A0;
			const double S = (W.X * DB.Y - W.Y * DB.X) / Denominator;
			const double U = (W.X * DA.Y - W.Y * DA.X) / Denominator;
			if (S < -1e-9 || S > 1.0 + 1e-9 || U < -1e-9 || U > 1.0 + 1e-9) continue;
			// Newton on P(t1) - Q(t2) = 0 in the ground plane, from the polyline crossing.
			double T1 = (I + S) / NP;
			double T2 = (J + U) / NQ;
			bool bConverged = false;
			for (int32 Iteration = 0; Iteration < 12 && !bConverged; Iteration++)
			{
				const FVector3d F = P.Position(T1) - Q.Position(T2);
				bConverged = FMath::Square(F.X) + FMath::Square(F.Z) < 1e-14;
				if (bConverged) break;
				const FVector3d TP = P.Tangent(T1);
				const FVector3d TQ = Q.Tangent(T2);
				const double Determinant = -TP.X * TQ.Z + TQ.X * TP.Z;
				if (FMath::Abs(Determinant) < 1e-14) break;
				T1 = FMath::Clamp(T1 + (F.X * TQ.Z - TQ.X * F.Z) / Determinant, 0.0, 1.0);
				T2 = FMath::Clamp(T2 + (TP.X * F.Z - TP.Z * F.X) / Determinant, 0.0, 1.0);
			}
			if (!bConverged)
			{
				const FVector3d F = P.Position(T1) - Q.Position(T2);
				if (FMath::Square(F.X) + FMath::Square(F.Z) > 1e-8) continue;
			}
			// Neighbouring polyline pieces find the same crossing.
			bool bKnown = false;
			for (const FVector2d& Known : Out) bKnown |= FMath::Abs(Known.X - T1) < 1e-6 && FMath::Abs(Known.Y - T2) < 1e-6;
			if (!bKnown) Out.Add(FVector2d(T1, T2));
		}
	}
}

FVector3d CSRoadSplineNetwork_Double(const float3& V) { return FVector3d(V.x, V.y, V.z); }
// Three times the control arm from From to To, in double: exact, so From + arm / 3 gives To back.
FVector3d CSRoadSplineNetwork_Arm(const float3& From, const float3& To) { return (CSRoadSplineNetwork_Double(To) - CSRoadSplineNetwork_Double(From)) * 3.0; }
float3 CSRoadSplineNetwork_Float(const FVector3d& V) { return float3(static_cast<float>(V.X), static_cast<float>(V.Y), static_cast<float>(V.Z)); }

struct FCSRoadSplineNetwork_Point
{
	int32 Spline = 0;
	int32 Index = 0;
	FVector3d NodePosition = FVector3d::ZeroVector;
	// Of a spline that is not adjusted.
	bool bFixed = false;
	// Its curve end follows the node: an adjusted spline's point with no node offset.
	bool bFree = false;
};

struct FCSRoadSplineNetwork_Node
{
	FVector3d Position = FVector3d::ZeroVector;
	FString Prefab;
	bool bElevation = false;
	float2 Elevation;
	CompositionFlags Upgrade{};
	bool bOutside = false;
	// Never merged away: it carries data, joins splines or belongs to a fixed one.
	bool bPinned = false;
	// Joins the road it lies on whatever its degree (FSplinePointData::bJoin).
	bool bJoin = false;
};

struct FCSRoadSplineNetwork_Piece
{
	FCSRoadSplineNetwork_Curve Curve;
	int32 Spline = 0;
	int32 Start = 0;
	int32 End = 0;
	bool bStartFree = false;
	bool bEndFree = false;
	bool bElevation = false;
	float2 Elevation;
	bool bAlive = true;
	// The piece it was split from for length or turn, else itself: its shape depends on that whole piece.
	int32 Origin = INDEX_NONE;
};

// -1, 0 or 1 as L comes before, with or after R: by x, then y, then z.
int32 CSRoadSplineNetwork_Compare(const FVector3d& L, const FVector3d& R)
{
	for (int32 I = 0; I < 3; I++) if (L[I] != R[I]) return L[I] < R[I] ? -1 : 1;
	return 0;
}

int32 CSRoadSplineNetwork_Compare(const CompositionFlags& L, const CompositionFlags& R)
{
	if (L.m_General != R.m_General) return L.m_General < R.m_General ? -1 : 1;
	if (L.m_Left != R.m_Left) return L.m_Left < R.m_Left ? -1 : 1;
	if (L.m_Right != R.m_Right) return L.m_Right < R.m_Right ? -1 : 1;
	return 0;
}

// Splines in an order of their own content: points, tangents, prefab, upgrades and kind. Only copies tie.
bool CSRoadSplineNetwork_SplineBefore(const FSplineRoad& L, const FSplineRoad& R)
{
	for (TArray<FVector3d> FSplineRoad::* Member : { &FSplineRoad::Points, &FSplineRoad::Arrive, &FSplineRoad::Leave })
	{
		const TArray<FVector3d>& A = L.*Member;
		const TArray<FVector3d>& B = R.*Member;
		for (int32 I = 0; I < FMath::Min(A.Num(), B.Num()); I++) if (const int32 Order = CSRoadSplineNetwork_Compare(A[I], B[I])) return Order < 0;
		if (A.Num() != B.Num()) return A.Num() < B.Num();
	}
	if (const int32 Order = L.Prefab.Compare(R.Prefab, ESearchCase::CaseSensitive)) return Order < 0;
	if (const int32 Order = CSRoadSplineNetwork_Compare(L.Upgrade, R.Upgrade)) return Order < 0;
	if (L.bAdjust != R.bAdjust) return R.bAdjust;
	if (L.bAlley != R.bAlley) return R.bAlley;
	return !L.bClosed && R.bClosed;
}

// A piece is split at T, where node Node goes.
struct FCSRoadSplineNetwork_Cut
{
	double T = 0.0;
	int32 Node = 0;
};

int32 CSRoadSplineNetwork_Segments(const FSplineRoad& Road) { return Road.Points.Num() < 2 ? 0 : Road.bClosed ? Road.Points.Num() : Road.Points.Num() - 1; }

// Segment I of a spline (to the next point), as its piece starts out.
FCSRoadSplineNetwork_Curve CSRoadSplineNetwork_Segment(const FSplineRoad& Road, int32 I)
{
	const int32 J = (I + 1) % Road.Points.Num();
	FCSRoadSplineNetwork_Curve Curve;
	Curve.A = Road.Points[I];
	Curve.B = Road.Points[I] + Road.Leave[I] / 3.0;
	Curve.C = Road.Points[J] - Road.Arrive[J] / 3.0;
	Curve.D = Road.Points[J];
	if (Road.Linear.IsValidIndex(I) && Road.Linear[I])
	{
		Curve.B = FMath::Lerp(Curve.A, Curve.D, 1.0 / 3.0);
		Curve.C = FMath::Lerp(Curve.A, Curve.D, 2.0 / 3.0);
	}
	return Curve;
}

// A main road's segment in the ground plane, sampled about every metre: what the alleys give way to.
struct FCSRoadSplineNetwork_Corridor
{
	TArray<FVector3d> Points;
	double HalfWidth = 0.0;
	FBox3d Box = FBox3d(ForceInit);
};

// The point of a corridor's centre line nearest to P in the ground plane, the line's direction there
// (unit, ground plane), on which side of it P lies (+1 / -1), and how far the corridor covers an alley
// (its half width and the alley's reach; set by the alleys' search).
struct FCSRoadSplineNetwork_OnCorridor
{
	double Distance = TNumericLimits<double>::Max();
	FVector3d Point = FVector3d::ZeroVector;
	FVector2d Direction = FVector2d::ZeroVector;
	double Side = 0.0;
	double Limit = 0.0;
};

FCSRoadSplineNetwork_OnCorridor CSRoadSplineNetwork_Nearest(const FCSRoadSplineNetwork_Corridor& Corridor, const FVector3d& P)
{
	FCSRoadSplineNetwork_OnCorridor Best;
	for (int32 I = 0; I + 1 < Corridor.Points.Num(); I++)
	{
		const FVector3d A = Corridor.Points[I];
		const FVector3d B = Corridor.Points[I + 1];
		const FVector2d AB(B.X - A.X, B.Z - A.Z);
		const FVector2d AP(P.X - A.X, P.Z - A.Z);
		const double Length2 = AB.SquaredLength();
		const double T = Length2 > 1e-12 ? FMath::Clamp(AP.Dot(AB) / Length2, 0.0, 1.0) : 0.0;
		const FVector3d On = FMath::Lerp(A, B, T);
		const double Distance = FVector2d(P.X - On.X, P.Z - On.Z).Length();
		if (Distance >= Best.Distance) continue;
		Best.Distance = Distance;
		Best.Point = On;
		Best.Direction = Length2 > 1e-12 ? AB / FMath::Sqrt(Length2) : FVector2d::ZeroVector;
		Best.Side = AB.X * AP.Y - AB.Y * AP.X >= 0.0 ? 1.0 : -1.0;
	}
	return Best;
}

// Alleys give way to the main roads over them. Out holds every spline, in order: a main road or an
// alley no main road covers as it is, a covered alley as the parts left of it (kept in OutParts). False
// (Out unused) when no alley is covered.
bool CSRoadSplineNetwork_ClipAlleys(FAssets& Assets, const TArray<const FSplineRoad*>& Splines, const FSplineNetworkSettings& Settings, TArray<FSplineRoad>& OutParts,
	TArray<const FSplineRoad*>& Out, FSplineNetworkStats& OutStats)
{
	// Without an alley there is nothing to sample the main roads for.
	if (!Splines.ContainsByPredicate([](const FSplineRoad* Road) { return Road->bAlley; })) return false;
	TArray<FCSRoadSplineNetwork_Corridor> Corridors;
	// The main roads' nodes (a point and its node offset): an alley's node on one (within the snap distance, a
	// centimetre where either spline is fixed; at about its height) is a junction drawn with that road, which the
	// alley keeps as drawn.
	const double Snap = FMath::Max(Settings.SnapDistance, 0.01);
	auto PointCell = [Snap](const FVector3d& P) { return FIntPoint(FMath::FloorToInt32(P.X / Snap), FMath::FloorToInt32(P.Z / Snap)); };
	auto NodeOf = [](const FSplineRoad& Road, int32 I) { return Road.Data.IsValidIndex(I) ? Road.Points[I] + Road.Data[I].NodeOffset : Road.Points[I]; };
	TMap<FIntPoint, TArray<TPair<FVector3d, bool>>> MainPoints;
	for (const FSplineRoad* Main : Splines)
	{
		const FSplineRoad& Road = *Main;
		if (Road.bAlley || !Assets.HasRoad(Road.Prefab) || Road.Arrive.Num() != Road.Points.Num() || Road.Leave.Num() != Road.Points.Num()) continue;
		for (int32 I = 0; I < Road.Points.Num(); I++) MainPoints.FindOrAdd(PointCell(NodeOf(Road, I))).Add({ NodeOf(Road, I), Road.bAdjust });
		const double HalfWidth = double(Assets.Geometry(Road.Prefab).m_DefaultWidth) * 0.5;
		for (int32 I = 0; I < CSRoadSplineNetwork_Segments(Road); I++)
		{
			const FCSRoadSplineNetwork_Curve Curve = CSRoadSplineNetwork_Segment(Road, I);
			const int32 Count = FMath::Clamp(FMath::CeilToInt32(CSRoadSplineNetwork_Length(Curve)), 8, 1024);
			FCSRoadSplineNetwork_Corridor& Corridor = Corridors.AddDefaulted_GetRef();
			Corridor.HalfWidth = HalfWidth;
			for (int32 K = 0; K <= Count; K++)
			{
				Corridor.Points.Add(Curve.Position(double(K) / Count));
				Corridor.Box += Corridor.Points.Last();
			}
		}
	}
	if (Corridors.Num() == 0) return false;
	auto OnMain = [&MainPoints, &PointCell, &Settings, Snap](const FVector3d& P, bool bAdjust)
	{
		const FIntPoint Cell = PointCell(P);
		for (int32 Y = Cell.Y - 1; Y <= Cell.Y + 1; Y++)
		{
			for (int32 X = Cell.X - 1; X <= Cell.X + 1; X++)
			{
				const TArray<TPair<FVector3d, bool>>* Points = MainPoints.Find(FIntPoint(X, Y));
				if (!Points) continue;
				for (const TPair<FVector3d, bool>& Q : *Points)
				{
					const double Within = bAdjust && Q.Value ? Snap : 0.01;
					if (FVector2d(P.X - Q.Key.X, P.Z - Q.Key.Z).Length() <= Within && FMath::Abs(P.Y - Q.Key.Y) <= Settings.JunctionHeight) return true;
				}
			}
		}
		return false;
	};
	// Where to look for the main roads near a point: the corridors whose box, grown by its half width, the farthest
	// reach of any alley and the metre the joins look past it, covers a square of the ground plane; in corridor
	// order, as the search before went through them all (the first of equal candidates wins).
	double MaxReach = 0.0;
	for (const FSplineRoad* Drawn : Splines) if (Drawn->bAlley && Assets.HasRoad(Drawn->Prefab)) MaxReach = FMath::Max(MaxReach, double(Assets.Geometry(Drawn->Prefab).m_DefaultWidth) * 0.5 + Settings.AlleyClearance);
	constexpr double CellSize = 32.0;
	auto CellOf = [](double X, double Z) { return FIntPoint(FMath::FloorToInt32(X / CellSize), FMath::FloorToInt32(Z / CellSize)); };
	TMap<FIntPoint, TArray<int32>> Grid;
	for (int32 C = 0; C < Corridors.Num(); C++)
	{
		const FBox3d& Box = Corridors[C].Box;
		const double Grow = Corridors[C].HalfWidth + MaxReach + 1.0;
		const FIntPoint Min = CellOf(Box.Min.X - Grow, Box.Min.Z - Grow);
		const FIntPoint Max = CellOf(Box.Max.X + Grow, Box.Max.Z + Grow);
		for (int32 Y = Min.Y; Y <= Max.Y; Y++) for (int32 X = Min.X; X <= Max.X; X++) Grid.FindOrAdd(FIntPoint(X, Y)).Add(C);
	}

	// Each spline in order: itself, or (null) one of the parts left of it, by index into OutParts.
	TArray<TPair<const FSplineRoad*, int32>> Kept;
	Kept.Reserve(Splines.Num());
	bool bCut = false;
	for (const FSplineRoad* Drawn : Splines)
	{
		const FSplineRoad& Road = *Drawn;
		const int32 Segments = CSRoadSplineNetwork_Segments(Road);
		if (!Road.bAlley || Segments == 0 || !Assets.HasRoad(Road.Prefab) || Road.Arrive.Num() != Road.Points.Num() || Road.Leave.Num() != Road.Points.Num())
		{
			Kept.Add({ Drawn, INDEX_NONE });
			continue;
		}
		const double Reach = double(Assets.Geometry(Road.Prefab).m_DefaultWidth) * 0.5 + Settings.AlleyClearance;
		TArray<FCSRoadSplineNetwork_Curve> Curves;
		for (int32 I = 0; I < Segments; I++) Curves.Add(CSRoadSplineNetwork_Segment(Road, I));
		auto At = [&Curves, Segments](double U) { const int32 I = FMath::Clamp(FMath::FloorToInt32(U), 0, Segments - 1); return Curves[I].Position(U - I); };
		// No main road near the alley: nothing covers it.
		FBox3d Hull(ForceInit);
		for (const FCSRoadSplineNetwork_Curve& Curve : Curves) for (const FVector3d& Control : { Curve.A, Curve.B, Curve.C, Curve.D }) Hull += Control;
		const FIntPoint HullMin = CellOf(Hull.Min.X, Hull.Min.Z);
		const FIntPoint HullMax = CellOf(Hull.Max.X, Hull.Max.Z);
		bool bNear = false;
		for (int32 Y = HullMin.Y; Y <= HullMax.Y && !bNear; Y++) for (int32 X = HullMin.X; X <= HullMax.X && !bNear; X++) bNear = Grid.Contains(FIntPoint(X, Y));
		if (!bNear)
		{
			Kept.Add({ Drawn, INDEX_NONE });
			continue;
		}
		// The main road nearest to P relative to its reach (its half width plus the alley's), among those
		// at about P's height; covered within that reach. Margin widens the search past the reach.
		auto Nearest = [&Corridors, &Grid, &CellOf, &Settings, Reach](const FVector3d& P, double Margin, FCSRoadSplineNetwork_OnCorridor& OutOn)
		{
			double Best = TNumericLimits<double>::Max();
			const TArray<int32>* Near = Grid.Find(CellOf(P.X, P.Z));
			if (!Near) return Best;
			for (const int32 Index : *Near)
			{
				const FCSRoadSplineNetwork_Corridor& Corridor = Corridors[Index];
				const double Limit = Corridor.HalfWidth + Reach;
				const double Search = Limit + Margin;
				if (P.X < Corridor.Box.Min.X - Search || P.X > Corridor.Box.Max.X + Search || P.Z < Corridor.Box.Min.Z - Search || P.Z > Corridor.Box.Max.Z + Search) continue;
				const FCSRoadSplineNetwork_OnCorridor On = CSRoadSplineNetwork_Nearest(Corridor, P);
				if (On.Distance > Search || FMath::Abs(P.Y - On.Point.Y) > Settings.JunctionHeight || On.Distance / Limit >= Best) continue;
				Best = On.Distance / Limit;
				OutOn = On;
				OutOn.Limit = Limit;
			}
			return Best;
		};
		auto Covered = [&Nearest](const FVector3d& P)
		{
			FCSRoadSplineNetwork_OnCorridor On;
			return Nearest(P, 0.0, On) <= 1.0;
		};

		// Samples about every half metre along the whole spline (parameter U: segment + t).
		TArray<double> U;
		for (int32 I = 0; I < Segments; I++)
		{
			const int32 Count = FMath::Clamp(FMath::CeilToInt32(CSRoadSplineNetwork_Length(Curves[I]) * 2.0), 4, 4096);
			for (int32 K = 0; K < Count; K++) U.Add(I + double(K) / Count);
		}
		U.Add(double(Segments));
		TArray<bool> Inside;
		for (const double V : U) Inside.Add(Covered(At(V)));
		if (!Inside.Contains(true))
		{
			Kept.Add({ Drawn, INDEX_NONE });
			continue;
		}
		// Where the alley enters and leaves cover, to a few millimetres.
		auto Edge = [&](double Outside, double In)
		{
			for (int32 Step = 0; Step < 20; Step++)
			{
				const double Middle = (Outside + In) * 0.5;
				if (Covered(At(Middle))) In = Middle;
				else Outside = Middle;
			}
			return Outside;
		};
		struct FRun
		{
			double Enter = 0.0;
			double Leave = 0.0;
			bool bStart = false;
			bool bEnd = false;
		};
		TArray<FRun> Runs;
		for (int32 K = 0; K < U.Num(); K++)
		{
			if (!Inside[K] || (K > 0 && Inside[K - 1])) continue;
			FRun& Run = Runs.AddDefaulted_GetRef();
			Run.bStart = K == 0;
			Run.Enter = Run.bStart ? U[0] : Edge(U[K - 1], U[K]);
			int32 Last = K;
			while (Last + 1 < U.Num() && Inside[Last + 1]) Last++;
			Run.bEnd = Last == U.Num() - 1;
			Run.Leave = Run.bEnd ? U.Last() : Edge(U[Last + 1], U[Last]);
		}
		// A stretch through a junction drawn with the main road (a node of the alley on one of the road's) stays as
		// drawn: the alley meets the road there already. A main road moved off it gets the cut and join instead.
		Runs.RemoveAll([&](const FRun& Run)
		{
			for (int32 K = FMath::CeilToInt32(Run.Enter - 1e-9); K <= FMath::FloorToInt32(Run.Leave + 1e-9); K++) if (OnMain(NodeOf(Road, K % Road.Points.Num()), Road.bAdjust)) return true;
			return false;
		});
		if (Runs.Num() == 0)
		{
			Kept.Add({ Drawn, INDEX_NONE });
			continue;
		}
		bCut = true;
		OutStats.AlleyCuts += Runs.Num();
		// A closed alley covered where it starts goes on round to the cover's far side.
		if (Road.bClosed && Runs.Num() > 1 && Runs[0].bStart && Runs.Last().bEnd)
		{
			Runs[0].Enter = Runs.Last().Enter - Segments;
			Runs[0].bStart = false;
			Runs.Pop();
		}
		// Where a cut end joins the main road: one point for both ends of a stretch that crosses it
		// straight over, else the nearest point of the road; with the road's direction there and the side
		// the end comes from.
		auto Join = [&](const FRun& Run, bool bBefore)
		{
			const double Wrap = Road.bClosed ? double(Segments) : 0.0;
			auto Wrapped = [Wrap](double V) { return Wrap > 0.0 ? FMath::Fmod(FMath::Fmod(V, Wrap) + Wrap, Wrap) : V; };
			// The run's ends lie just outside cover: look a little past the reach.
			FCSRoadSplineNetwork_OnCorridor Enter;
			FCSRoadSplineNetwork_OnCorridor Leave;
			Nearest(At(Wrapped(Run.Enter)), 1.0, Enter);
			Nearest(At(Wrapped(Run.Leave)), 1.0, Leave);
			if (!Run.bStart && !Run.bEnd && Enter.Side * Leave.Side < 0.0)
			{
				FCSRoadSplineNetwork_OnCorridor Closest;
				for (int32 Step = 0; Step <= 64; Step++)
				{
					FCSRoadSplineNetwork_OnCorridor On;
					if (Nearest(At(Wrapped(FMath::Lerp(Run.Enter, Run.Leave, Step / 64.0))), 1.0, On) <= 1.0 && On.Distance < Closest.Distance) Closest = On;
				}
				Closest.Side = bBefore ? Enter.Side : Leave.Side;
				return Closest;
			}
			return bBefore ? Enter : Leave;
		};
		// A cut end that would run on along the main road (meeting it flatter than 30 degrees: an alley that
		// ran nearly along it) leaves it square instead, towards the side it comes from. Two roads overlapping
		// that far make a sliver of a junction the GPU node geometry does not resolve (it hung the GPU). An end
		// continuing a road past its end is left as it is. True when the end turned.
		auto Square = [&Covered](FCSRoadSplineNetwork_Curve& Curve, const FCSRoadSplineNetwork_OnCorridor& On, bool bEnd)
		{
			const FVector3d Tip = bEnd ? Curve.D : Curve.A;
			FVector3d& Handle = bEnd ? Curve.C : Curve.B;
			FVector2d Back(Handle.X - Tip.X, Handle.Z - Tip.Z);
			// Twice the reach back along the end, a road it meets at 30 degrees or more is out of reach.
			if (!Back.Normalize() || On.Direction.IsNearlyZero() || !Covered(FVector3d(Tip.X + Back.X * On.Limit * 2.0, Tip.Y, Tip.Z + Back.Y * On.Limit * 2.0))) return false;
			const FVector2d Normal = FVector2d(-On.Direction.Y, On.Direction.X) * On.Side;
			const double Arm = FVector2d(Curve.D.X - Curve.A.X, Curve.D.Z - Curve.A.Z).Length() / 3.0;
			Handle = FVector3d(Tip.X + Normal.X * Arm, Handle.Y, Tip.Z + Normal.Y * Arm);
			return true;
		};

		// The parts between the stretches, each a spline ending on the main roads it was cut by.
		TArray<TPair<double, double>> Parts;
		TArray<TPair<const FRun*, const FRun*>> Bounds;
		if (Road.bClosed && Runs.Num() > 0)
		{
			for (int32 R = 0; R < Runs.Num(); R++)
			{
				const FRun& From = Runs[R];
				const FRun& To = Runs[(R + 1) % Runs.Num()];
				Parts.Add({ From.Leave, R + 1 < Runs.Num() ? To.Enter : To.Enter + Segments });
				Bounds.Add({ &From, &To });
			}
		}
		else
		{
			double From = 0.0;
			const FRun* Before = nullptr;
			for (const FRun& Run : Runs)
			{
				Parts.Add({ From, Run.Enter });
				Bounds.Add({ Before, &Run });
				From = Run.Leave;
				Before = &Run;
			}
			Parts.Add({ From, double(Segments) });
			Bounds.Add({ Before, nullptr });
		}
		for (int32 P = 0; P < Parts.Num(); P++)
		{
			const double From = Parts[P].Key;
			const double To = Parts[P].Value;
			if (To - From < 1e-6) continue;
			FSplineRoad Part;
			Part.Name = FString::Printf(TEXT("%s (part %d)"), *Road.Name, P + 1);
			// Still the drawn spline's road, for what derives from it.
			Part.Id = Road.Id;
			Part.Prefab = Road.Prefab;
			Part.Upgrade = Road.Upgrade;
			Part.bAdjust = Road.bAdjust;
			Part.bAlley = true;
			TArray<FCSRoadSplineNetwork_Curve> PartCurves;
			TArray<int32> Original;
			for (int32 S = FMath::FloorToInt32(From); S < FMath::CeilToInt32(To); S++)
			{
				const double T0 = FMath::Max(From - S, 0.0);
				const double T1 = FMath::Min(To - S, 1.0);
				if (T1 - T0 < 1e-6) continue;
				const int32 Segment = ((S % Segments) + Segments) % Segments;
				PartCurves.Add(Curves[Segment].Cut(T0, T1));
				Original.Add(Segment);
			}
			if (PartCurves.Num() == 0) continue;
			const FRun* Before = Bounds[P].Key;
			const FRun* After = Bounds[P].Value;
			bool bSquareStart = false;
			bool bSquareEnd = false;
			if (Before)
			{
				const FCSRoadSplineNetwork_OnCorridor On = Join(*Before, false);
				PartCurves[0].MoveStart(On.Point);
				bSquareStart = Square(PartCurves[0], On, false);
			}
			if (After)
			{
				const FCSRoadSplineNetwork_OnCorridor On = Join(*After, true);
				PartCurves.Last().MoveEnd(On.Point);
				bSquareEnd = Square(PartCurves.Last(), On, true);
			}
			if (FVector3d::Dist(PartCurves[0].A, PartCurves.Last().D) < 1.0 && PartCurves.Num() == 1) continue;
			for (int32 C = 0; C < PartCurves.Num(); C++)
			{
				const FCSRoadSplineNetwork_Curve& Curve = PartCurves[C];
				const int32 Segment = Original[C];
				if (C == 0)
				{
					// The part's first point is the spline's own only where the part starts on it.
					const bool bOwnPoint = !Before && FMath::IsNearlyEqual(From, double(FMath::RoundToInt32(From)));
					Part.Points.Add(Curve.A);
					Part.Arrive.Add((Curve.B - Curve.A) * 3.0);
					Part.Data.Add(bOwnPoint && Road.Data.IsValidIndex(Segment) ? Road.Data[Segment] : FSplinePointData());
				}
				Part.Leave.Add((Curve.B - Curve.A) * 3.0);
				// A squared end bends its piece.
				const bool bSquared = (C == 0 && bSquareStart) || (C + 1 == PartCurves.Num() && bSquareEnd);
				Part.Linear.Add(!bSquared && Road.Linear.IsValidIndex(Segment) && Road.Linear[Segment]);
				const int32 Next = (Segment + 1) % Road.Points.Num();
				const bool bOwnEnd = C + 1 < PartCurves.Num() || (!After && FMath::IsNearlyEqual(To, double(FMath::RoundToInt32(To))));
				Part.Points.Add(Curve.D);
				Part.Arrive.Add((Curve.D - Curve.C) * 3.0);
				Part.Data.Add(bOwnEnd && Road.Data.IsValidIndex(Next) ? Road.Data[Next] : FSplinePointData());
			}
			// The last point leaves the way it arrives; cut ends join the main road they were cut by.
			Part.Leave.Add(Part.Arrive.Last());
			if (Before) Part.Data[0].bJoin = true;
			if (After) Part.Data.Last().bJoin = true;
			Kept.Add({ nullptr, OutParts.Add(MoveTemp(Part)) });
		}
	}
	Out.Reset(Kept.Num());
	for (const TPair<const FSplineRoad*, int32>& Entry : Kept) Out.Add(Entry.Key ? Entry.Key : &OutParts[Entry.Value]);
	return bCut;
}

// What the conversion makes up to the snapshot form: the splines in their order of content (the parts of
// cut alleys kept in Parts), the nodes, and the pieces (those left out not alive).
struct FCSRoadSplineNetwork_Converted
{
	TArray<FSplineRoad> Parts;
	TArray<const FSplineRoad*> Splines;
	TArray<FCSRoadSplineNetwork_Node> Nodes;
	TArray<FCSRoadSplineNetwork_Piece> Pieces;
};

// Every stage up to the snapshot form, for the splines Drawn (in any order).
bool CSRoadSplineNetwork_Convert(FAssets& Assets, const TArray<const FSplineRoad*>& Drawn, const FSplineNetworkSettings& Settings, FCSRoadSplineNetwork_Converted& Out,
	FSplineNetworkStats& OutStats, FString& OutError)
{
	double LapStart = FPlatformTime::Seconds();
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};
	// The cuts and what follows keep the first of equal candidates in places (of two main roads exactly
	// as near an alley, the first), so the splines go through them in an order of their own content, not
	// the one they came in. Alleys give way to the main roads over them; the splines drawn stay as they
	// are, and the parts of a cut alley take their own places in that order.
	TArray<const FSplineRoad*> Ordered = Drawn;
	Ordered.StableSort(CSRoadSplineNetwork_SplineBefore);
	OutStats.SortMs = Lap();
	TArray<FSplineRoad>& AlleyParts = Out.Parts;
	TArray<const FSplineRoad*>& Splines = Out.Splines;
	if (CSRoadSplineNetwork_ClipAlleys(Assets, Ordered, Settings, AlleyParts, Splines, OutStats)) Splines.StableSort(CSRoadSplineNetwork_SplineBefore);
	else Splines = MoveTemp(Ordered);
	OutStats.AlleyMs = Lap();
	const double Snap = FMath::Max(Settings.SnapDistance, 0.01);
	static const FSplinePointData NoData;
	auto DataAt = [&Splines](int32 Spline, int32 Index) -> const FSplinePointData& { return Splines[Spline]->Data.IsValidIndex(Index) ? Splines[Spline]->Data[Index] : NoData; };

	// Every point, at its node's position.
	TArray<FCSRoadSplineNetwork_Point> Points;
	TArray<int32> FirstPoint;
	int32 NumPoints = 0;
	for (const FSplineRoad* Road : Splines) NumPoints += Road->Points.Num();
	Points.Reserve(NumPoints);
	FirstPoint.Reserve(Splines.Num());
	for (int32 S = 0; S < Splines.Num(); S++)
	{
		const FSplineRoad& Road = *Splines[S];
		if (!Assets.HasRoad(Road.Prefab))
		{
			OutError = FString::Printf(TEXT("%s: unknown road prefab '%s'"), *Road.Name, *Road.Prefab);
			return false;
		}
		if (Road.Arrive.Num() != Road.Points.Num() || Road.Leave.Num() != Road.Points.Num())
		{
			OutError = FString::Printf(TEXT("%s: every point needs both tangents"), *Road.Name);
			return false;
		}
		FirstPoint.Add(Points.Num());
		for (int32 I = 0; I < Road.Points.Num(); I++)
		{
			const FSplinePointData& Data = DataAt(S, I);
			FCSRoadSplineNetwork_Point& Point = Points.AddDefaulted_GetRef();
			Point.Spline = S;
			Point.Index = I;
			Point.NodePosition = Road.Points[I] + Data.NodeOffset;
			// An alley's end cut under a main road (bJoin) was never drawn: it snaps and follows its node as an
			// adjusted spline's would, on a fixed (converted) alley too.
			Point.bFixed = !Road.bAdjust && !Data.bJoin;
			Point.bFree = (Road.bAdjust && Data.NodeOffset.IsZero()) || Data.bJoin;
		}
	}

	// Points that meet share a node: within the snap distance, or within a centimetre when both
	// belong to fixed splines (a converted snapshot keeps its nodes apart however close they are).
	TArray<int32> Parent;
	Parent.SetNumUninitialized(Points.Num());
	for (int32 I = 0; I < Points.Num(); I++) Parent[I] = I;
	auto Find = [&Parent](int32 I)
	{
		while (Parent[I] != I)
		{
			Parent[I] = Parent[Parent[I]];
			I = Parent[I];
		}
		return I;
	};
	{
		// Cells twice the snap distance across: a point's neighbours within it lie in the 2 x 2 x 2 cells
		// around the corner of its cell nearest to it. Each cell lists its points through Next.
		const double CellSize = Snap * 2.0;
		TMap<FIntVector, int32> Head;
		Head.Reserve(Points.Num());
		TArray<int32> Next;
		Next.Init(INDEX_NONE, Points.Num());
		for (int32 I = 0; I < Points.Num(); I++)
		{
			const FVector3d Scaled = Points[I].NodePosition / CellSize;
			const FIntVector Home(FMath::FloorToInt32(Scaled.X), FMath::FloorToInt32(Scaled.Y), FMath::FloorToInt32(Scaled.Z));
			const FIntVector Toward(Scaled.X - Home.X < 0.5 ? -1 : 1, Scaled.Y - Home.Y < 0.5 ? -1 : 1, Scaled.Z - Home.Z < 0.5 ? -1 : 1);
			for (int32 K = 0; K < 8; K++)
			{
				const int32* First = Head.Find(Home + FIntVector((K & 1) ? Toward.X : 0, (K & 2) ? Toward.Y : 0, (K & 4) ? Toward.Z : 0));
				for (int32 J = First ? *First : INDEX_NONE; J != INDEX_NONE; J = Next[J])
				{
					const double Limit = Points[I].bFixed && Points[J].bFixed ? 0.01 : Snap;
					if (FVector3d::Dist(Points[I].NodePosition, Points[J].NodePosition) > Limit) continue;
					const int32 RootI = Find(I);
					const int32 RootJ = Find(J);
					if (RootI != RootJ) Parent[FMath::Max(RootI, RootJ)] = FMath::Min(RootI, RootJ);
				}
			}
			int32& First = Head.FindOrAdd(Home, INDEX_NONE);
			Next[I] = First;
			First = I;
		}
	}

	// Nodes: at a fixed point's position if the node has one, else at its first point's. Data comes
	// from the first point that sets it, upgrades from all of them.
	TArray<FCSRoadSplineNetwork_Node>& Nodes = Out.Nodes;
	TArray<int32> NodeOfPoint;
	NodeOfPoint.SetNumUninitialized(Points.Num());
	{
		TMap<int32, int32> NodeOfRoot;
		NodeOfRoot.Reserve(Points.Num());
		TArray<int32> FirstSpline;
		TArray<bool> bFixedPosition;
		for (int32 I = 0; I < Points.Num(); I++)
		{
			const int32 Root = Find(I);
			const int32* Known = NodeOfRoot.Find(Root);
			const int32 NodeIndex = Known ? *Known : Nodes.AddDefaulted();
			if (!Known)
			{
				NodeOfRoot.Add(Root, NodeIndex);
				Nodes[NodeIndex].Position = Points[I].NodePosition;
				FirstSpline.Add(Points[I].Spline);
				bFixedPosition.Add(false);
			}
			NodeOfPoint[I] = NodeIndex;
			FCSRoadSplineNetwork_Node& Node = Nodes[NodeIndex];
			const FSplinePointData& Data = DataAt(Points[I].Spline, Points[I].Index);
			if (Points[I].bFixed && !bFixedPosition[NodeIndex])
			{
				Node.Position = Points[I].NodePosition;
				bFixedPosition[NodeIndex] = true;
			}
			if (Node.Prefab.IsEmpty()) Node.Prefab = Data.NodePrefab;
			if (!Node.bElevation && Data.bNodeElevation)
			{
				Node.bElevation = true;
				Node.Elevation = Data.NodeElevation;
			}
			Node.Upgrade = Node.Upgrade | Data.NodeUpgrade;
			Node.bOutside |= Data.bOutside;
			Node.bJoin |= Data.bJoin;
			Node.bPinned |= Points[I].bFixed || Data.HasNodeOverrides() || Data.bJoin || Points[I].Spline != FirstSpline[NodeIndex];
		}
	}
	for (const FCSRoadSplineNetwork_Node& Node : Nodes)
	{
		if (Node.Prefab.IsEmpty() || Assets.HasRoad(Node.Prefab)) continue;
		OutError = FString::Printf(TEXT("Unknown node prefab '%s'"), *Node.Prefab);
		return false;
	}

	// One piece per segment; free ends are pulled onto their node.
	TArray<FCSRoadSplineNetwork_Piece>& Pieces = Out.Pieces;
	for (int32 S = 0; S < Splines.Num(); S++)
	{
		const FSplineRoad& Road = *Splines[S];
		const int32 Count = Road.Points.Num();
		const int32 Segments = Road.bClosed ? Count : Count - 1;
		for (int32 I = 0; I < Segments; I++)
		{
			const int32 J = (I + 1) % Count;
			const int32 PointI = FirstPoint[S] + I;
			const int32 PointJ = FirstPoint[S] + J;
			FCSRoadSplineNetwork_Piece Piece;
			Piece.Spline = S;
			Piece.Start = NodeOfPoint[PointI];
			Piece.End = NodeOfPoint[PointJ];
			if (Piece.Start == Piece.End)
			{
				OutStats.Dropped++;
				continue;
			}
			FCSRoadSplineNetwork_Curve& Curve = Piece.Curve;
			Curve.A = Road.Points[I];
			Curve.B = Road.Points[I] + Road.Leave[I] / 3.0;
			Curve.C = Road.Points[J] - Road.Arrive[J] / 3.0;
			Curve.D = Road.Points[J];
			Piece.bStartFree = Points[PointI].bFree;
			Piece.bEndFree = Points[PointJ].bFree;
			if (Piece.bStartFree) Curve.MoveStart(Nodes[Piece.Start].Position);
			if (Piece.bEndFree) Curve.MoveEnd(Nodes[Piece.End].Position);
			if (Road.Linear.IsValidIndex(I) && Road.Linear[I])
			{
				Curve.B = FMath::Lerp(Curve.A, Curve.D, 1.0 / 3.0);
				Curve.C = FMath::Lerp(Curve.A, Curve.D, 2.0 / 3.0);
			}
			const FSplinePointData& Data = DataAt(S, I);
			Piece.bElevation = Data.bSegmentElevation;
			Piece.Elevation = Data.SegmentElevation;
			Pieces.Add(Piece);
		}
	}

	auto Adjusted = [&Splines](const FCSRoadSplineNetwork_Piece& Piece) { return Splines[Piece.Spline]->bAdjust; };
	TArray<TArray<FCSRoadSplineNetwork_Cut>> Cuts;
	Cuts.SetNum(Pieces.Num());
	OutStats.NodeMs = Lap();

	// A road ending on another one (not at its ends, which snapping settles) joins it there, and so do
	// alley ends cut under a main road whatever their degree. At least one of the two must be adjusted,
	// or the end a cut one: a fixed main road is split where a cut alley joins it.
	{
		TArray<int32> Degree;
		TArray<int32> Incident;
		Degree.Init(0, Nodes.Num());
		Incident.Init(INDEX_NONE, Nodes.Num());
		for (int32 P = 0; P < Pieces.Num(); P++)
		{
			Degree[Pieces[P].Start]++;
			Degree[Pieces[P].End]++;
			Incident[Pieces[P].Start] = P;
			Incident[Pieces[P].End] = P;
		}
		for (int32 N = 0; N < Nodes.Num(); N++)
		{
			if (Degree[N] != 1 && !(Degree[N] > 1 && Nodes[N].bJoin)) continue;
			const bool bOwnAdjusted = Adjusted(Pieces[Incident[N]]);
			const FVector3d Position = Nodes[N].Position;
			int32 Best = INDEX_NONE;
			double BestT = 0.0;
			double BestDistance = Snap;
			for (int32 Q = 0; Q < Pieces.Num(); Q++)
			{
				const FCSRoadSplineNetwork_Piece& Other = Pieces[Q];
				if (Q == Incident[N] || Other.Start == N || Other.End == N || (!bOwnAdjusted && !Adjusted(Other) && !Nodes[N].bJoin)) continue;
				if (!Other.Curve.Box().ExpandBy(Snap).IsInsideOrOn(Position)) continue;
				const double T = CSRoadSplineNetwork_Nearest(Other.Curve, Position);
				const FVector3d OnRoad = Other.Curve.Position(T);
				const double Distance = FVector3d::Dist(OnRoad, Position);
				if (Distance > BestDistance || FVector3d::Dist(OnRoad, Other.Curve.A) <= Snap || FVector3d::Dist(OnRoad, Other.Curve.D) <= Snap) continue;
				Best = Q;
				BestT = T;
				BestDistance = Distance;
			}
			if (Best == INDEX_NONE) continue;
			Nodes[N].Position = Pieces[Best].Curve.Position(BestT);
			Nodes[N].bPinned = true;
			// Every road ending there follows the node onto the road it joins.
			for (int32 P = Degree[N] == 1 ? Incident[N] : 0; P < (Degree[N] == 1 ? Incident[N] + 1 : Pieces.Num()); P++)
			{
				FCSRoadSplineNetwork_Piece& Own = Pieces[P];
				if (Own.Start == N && Own.bStartFree) Own.Curve.MoveStart(Nodes[N].Position);
				if (Own.End == N && Own.bEndFree) Own.Curve.MoveEnd(Nodes[N].Position);
			}
			Cuts[Best].Add({ BestT, N });
			OutStats.Junctions++;
		}
	}

	OutStats.JoinMs = Lap();

	// Roads crossing at about the same height meet in a junction; one of the two must be adjusted.
	{
		TArray<double> Lengths;
		Lengths.SetNumUninitialized(Pieces.Num());
		for (int32 P = 0; P < Pieces.Num(); P++) Lengths[P] = CSRoadSplineNetwork_Length(Pieces[P].Curve);
		TArray<FVector2d> Crossings;
		for (int32 P = 0; P < Pieces.Num(); P++)
		{
			if (!Adjusted(Pieces[P])) continue;
			const FBox3d BoxP = Pieces[P].Curve.Box();
			for (int32 Q = 0; Q < Pieces.Num(); Q++)
			{
				// Adjusted pairs are visited once, from the lower index.
				if (Q == P || (Q < P && Adjusted(Pieces[Q]))) continue;
				const FCSRoadSplineNetwork_Piece& PieceP = Pieces[P];
				const FCSRoadSplineNetwork_Piece& PieceQ = Pieces[Q];
				if (PieceP.Start == PieceQ.Start || PieceP.Start == PieceQ.End || PieceP.End == PieceQ.Start || PieceP.End == PieceQ.End) continue;
				const FBox3d BoxQ = PieceQ.Curve.Box();
				if (BoxP.Max.X < BoxQ.Min.X || BoxQ.Max.X < BoxP.Min.X || BoxP.Max.Z < BoxQ.Min.Z || BoxQ.Max.Z < BoxP.Min.Z) continue;
				Crossings.Reset();
				CSRoadSplineNetwork_Crossings(PieceP.Curve, PieceQ.Curve, Lengths[P], Lengths[Q], Crossings);
				for (const FVector2d& Hit : Crossings)
				{
					const FVector3d OnP = PieceP.Curve.Position(Hit.X);
					const FVector3d OnQ = PieceQ.Curve.Position(Hit.Y);
					if (FMath::Abs(OnP.Y - OnQ.Y) > Settings.JunctionHeight) continue;
					if (FVector3d::Dist(OnP, PieceP.Curve.A) <= Snap || FVector3d::Dist(OnP, PieceP.Curve.D) <= Snap) continue;
					if (FVector3d::Dist(OnQ, PieceQ.Curve.A) <= Snap || FVector3d::Dist(OnQ, PieceQ.Curve.D) <= Snap) continue;
					const int32 Node = Nodes.AddDefaulted();
					Nodes[Node].Position = (OnP + OnQ) * 0.5;
					Nodes[Node].bPinned = true;
					Cuts[P].Add({ Hit.X, Node });
					Cuts[Q].Add({ Hit.Y, Node });
					OutStats.Junctions++;
				}
			}
		}
	}

	OutStats.CrossMs = Lap();

	// Split at the junctions; the parts meet exactly on the junction nodes.
	auto Divide = [&Pieces, &Nodes](int32 P, const TArray<FCSRoadSplineNetwork_Cut>& At)
	{
		const FCSRoadSplineNetwork_Piece Whole = Pieces[P];
		double From = 0.0;
		int32 FromNode = Whole.Start;
		bool bFirst = true;
		for (int32 K = 0; K <= At.Num(); K++)
		{
			const bool bLast = K == At.Num();
			const double To = bLast ? 1.0 : At[K].T;
			const int32 ToNode = bLast ? Whole.End : At[K].Node;
			if (ToNode == FromNode || To <= From) continue;
			FCSRoadSplineNetwork_Piece Part = Whole;
			Part.Curve = Whole.Curve.Cut(From, To);
			Part.Start = FromNode;
			Part.End = ToNode;
			Part.bStartFree = FromNode == Whole.Start ? Whole.bStartFree : true;
			Part.bEndFree = bLast ? Whole.bEndFree : true;
			if (FromNode != Whole.Start) Part.Curve.MoveStart(Nodes[FromNode].Position);
			if (!bLast) Part.Curve.MoveEnd(Nodes[ToNode].Position);
			if (bFirst) Pieces[P] = Part;
			else Pieces.Add(Part);
			bFirst = false;
			From = To;
			FromNode = ToNode;
		}
	};
	{
		const int32 Count = Pieces.Num();
		for (int32 P = 0; P < Count; P++)
		{
			if (Cuts[P].Num() == 0) continue;
			Cuts[P].Sort([](const FCSRoadSplineNetwork_Cut& L, const FCSRoadSplineNetwork_Cut& R) { return L.T < R.T; });
			Divide(P, Cuts[P]);
		}
	}

	// Short segments of adjusted splines are merged across the plain bends (degree two, no data, one
	// spline, one direction) at their ends. The merged curve keeps both outer end tangents, scaled to
	// its parameter.
	if (Settings.MinEdgeLength > 0.0)
	{
		bool bMerged = true;
		while (bMerged)
		{
			bMerged = false;
			TArray<int32> Degree;
			TArray<int32> StartingAt;
			TArray<int32> EndingAt;
			Degree.Init(0, Nodes.Num());
			StartingAt.Init(INDEX_NONE, Nodes.Num());
			EndingAt.Init(INDEX_NONE, Nodes.Num());
			for (int32 P = 0; P < Pieces.Num(); P++)
			{
				if (!Pieces[P].bAlive) continue;
				Degree[Pieces[P].Start]++;
				Degree[Pieces[P].End]++;
				StartingAt[Pieces[P].Start] = P;
				EndingAt[Pieces[P].End] = P;
			}
			for (int32 P = 0; P < Pieces.Num() && !bMerged; P++)
			{
				if (!Pieces[P].bAlive || !Adjusted(Pieces[P]) || FVector3d::Dist(Pieces[P].Curve.A, Pieces[P].Curve.D) >= Settings.MinEdgeLength) continue;
				for (const int32 Node : { Pieces[P].End, Pieces[P].Start })
				{
					if (Degree[Node] != 2 || Nodes[Node].bPinned) continue;
					const int32 First = EndingAt[Node];
					const int32 Second = StartingAt[Node];
					if (First == INDEX_NONE || Second == INDEX_NONE || First == Second || Pieces[First].Spline != Pieces[Second].Spline) continue;
					if (Pieces[First].Start == Pieces[Second].End) continue;
					FCSRoadSplineNetwork_Piece& Head = Pieces[First];
					FCSRoadSplineNetwork_Piece& Tail = Pieces[Second];
					const double HeadLength = CSRoadSplineNetwork_Length(Head.Curve);
					const double Total = FMath::Max(HeadLength + CSRoadSplineNetwork_Length(Tail.Curve), 1e-6);
					const double Share = FMath::Clamp(HeadLength / Total, 1e-6, 1.0 - 1e-6);
					FCSRoadSplineNetwork_Curve Curve;
					Curve.A = Head.Curve.A;
					Curve.D = Tail.Curve.D;
					Curve.B = Curve.A + ((Head.Curve.B - Head.Curve.A) / Share).GetClampedToMaxSize(Total);
					Curve.C = Curve.D + ((Tail.Curve.C - Tail.Curve.D) / (1.0 - Share)).GetClampedToMaxSize(Total);
					Head.Curve = Curve;
					Head.End = Tail.End;
					Head.bEndFree = Tail.bEndFree;
					if (!Head.bElevation && Tail.bElevation)
					{
						Head.bElevation = true;
						Head.Elevation = Tail.Elevation;
					}
					Tail.bAlive = false;
					OutStats.Merges++;
					bMerged = true;
					break;
				}
			}
		}
	}

	for (int32 P = 0; P < Pieces.Num(); P++) Pieces[P].Origin = P;
	// Long or sharply turning segments of adjusted splines are split into equal lengths, never
	// shorter than the minimum.
	if (Settings.MaxEdgeLength > 0.0 || Settings.MaxEdgeTurn > 0.0)
	{
		const int32 Count = Pieces.Num();
		for (int32 P = 0; P < Count; P++)
		{
			if (!Pieces[P].bAlive || !Adjusted(Pieces[P])) continue;
			const double Length = CSRoadSplineNetwork_Length(Pieces[P].Curve);
			int32 Parts = 1;
			if (Settings.MaxEdgeLength > 0.0) Parts = FMath::Max(Parts, FMath::CeilToInt32(Length / Settings.MaxEdgeLength - 1e-6));
			if (Settings.MaxEdgeTurn > 0.0) Parts = FMath::Max(Parts, FMath::CeilToInt32(CSRoadSplineNetwork_Turn(Pieces[P].Curve) / Settings.MaxEdgeTurn - 1e-6));
			if (Settings.MinEdgeLength > 0.0) Parts = FMath::Min(Parts, FMath::Max(1, FMath::FloorToInt32(Length / Settings.MinEdgeLength)));
			if (Parts <= 1) continue;
			TArray<FCSRoadSplineNetwork_Cut> At;
			for (const double T : CSRoadSplineNetwork_EqualParts(Pieces[P].Curve, Parts))
			{
				const int32 Node = Nodes.AddDefaulted();
				Nodes[Node].Position = Pieces[P].Curve.Position(T);
				At.Add({ T, Node });
			}
			Divide(P, At);
			OutStats.Splits += Parts - 1;
		}
	}

	// Graph.Build needs the ends apart.
	for (FCSRoadSplineNetwork_Piece& Piece : Pieces)
	{
		if (!Piece.bAlive || FVector3d::Dist(Piece.Curve.A, Piece.Curve.D) >= 0.1) continue;
		Piece.bAlive = false;
		OutStats.Dropped++;
	}

	OutStats.ShapeMs = Lap();
	return true;
}

// An edge of the snapshot form, with what orders the edges: its curve, its spline's prefab and upgrades
// (in Spec), and its elevation as the spline gives it.
struct FCSRoadSplineNetwork_EdgeRecord
{
	FCSRoadSplineNetwork_Curve Curve;
	bool bElevation = false;
	float2 Elevation;
	// What it comes out as, its nodes as indices of node records.
	FRoadSpec Spec;
	// In the ground plane, every edge split from the same piece: a change to one may change them all.
	FBox2d PartBox = FBox2d(ForceInit);
};

// A node as it comes out, and what tells it apart from one build to the next: its position and, among
// nodes at the very same position, how many were made before it.
struct FCSRoadSplineNetwork_NodeRecord
{
	FSavedNode Node;
	FVector3d Position = FVector3d::ZeroVector;
	int32 Ordinal = 0;
};

// A node record's identity: its position's bits (-0 as 0) and its ordinal.
struct FCSRoadSplineNetwork_NodeKey
{
	uint64 X = 0;
	uint64 Y = 0;
	uint64 Z = 0;
	int32 Ordinal = 0;

	FCSRoadSplineNetwork_NodeKey(const FVector3d& Position, int32 InOrdinal)
		: X(Bits(Position.X)), Y(Bits(Position.Y)), Z(Bits(Position.Z)), Ordinal(InOrdinal)
	{
	}

	static uint64 Bits(double Value)
	{
		const double Plain = Value == 0.0 ? 0.0 : Value;
		uint64 Out = 0;
		FMemory::Memcpy(&Out, &Plain, sizeof(Out));
		return Out;
	}

	bool operator==(const FCSRoadSplineNetwork_NodeKey& Other) const { return X == Other.X && Y == Other.Y && Z == Other.Z && Ordinal == Other.Ordinal; }

	friend uint32 GetTypeHash(const FCSRoadSplineNetwork_NodeKey& Key)
	{
		return HashCombineFast(HashCombineFast(::GetTypeHash(Key.X), ::GetTypeHash(Key.Y)), HashCombineFast(::GetTypeHash(Key.Z), ::GetTypeHash(Key.Ordinal)));
	}
};

FCSRoadSplineNetwork_NodeKey CSRoadSplineNetwork_Key(const FCSRoadSplineNetwork_NodeRecord& Record) { return FCSRoadSplineNetwork_NodeKey(Record.Position, Record.Ordinal); }

// Edges in an order of their own content: curve (ends first), prefab, upgrades and elevation. Only
// copies tie.
bool CSRoadSplineNetwork_RecordBefore(const FCSRoadSplineNetwork_EdgeRecord& L, const FCSRoadSplineNetwork_EdgeRecord& R)
{
	for (FVector3d FCSRoadSplineNetwork_Curve::* Point : { &FCSRoadSplineNetwork_Curve::A, &FCSRoadSplineNetwork_Curve::D, &FCSRoadSplineNetwork_Curve::B, &FCSRoadSplineNetwork_Curve::C }) if (const int32 Order = CSRoadSplineNetwork_Compare(L.Curve.*Point, R.Curve.*Point)) return Order < 0;
	if (const int32 Order = L.Spec.Prefab.Compare(R.Spec.Prefab, ESearchCase::CaseSensitive)) return Order < 0;
	if (const int32 Order = CSRoadSplineNetwork_Compare(L.Spec.Upgrade, R.Spec.Upgrade)) return Order < 0;
	if (L.bElevation != R.bElevation) return R.bElevation;
	if (L.Elevation.x != R.Elevation.x) return L.Elevation.x < R.Elevation.x;
	return L.Elevation.y < R.Elevation.y;
}

FVector2d CSRoadSplineNetwork_Ground(const FVector3d& Point) { return FVector2d(Point.X, Point.Z); }

FBox2d CSRoadSplineNetwork_Box(const FCSRoadSplineNetwork_Curve& Curve)
{
	FBox2d Box(ForceInit);
	for (const FVector3d& Point : { Curve.A, Curve.B, Curve.C, Curve.D }) Box += CSRoadSplineNetwork_Ground(Point);
	return Box;
}

// The snapshot form of a conversion: its edges in their order of content, each with what it comes out as,
// and the records of their nodes, made the first time an edge reaches them. The geometry jobs break ties
// and sum in edge order, so the same roads give the same network whatever order the splines come in;
// Graph.Build numbers entities in the order the edges reach them.
void CSRoadSplineNetwork_Records(FAssets& Assets, const FCSRoadSplineNetwork_Converted& In, const FSavedTerrain* Terrain, TArray<FCSRoadSplineNetwork_EdgeRecord>& OutEdges,
	TArray<FCSRoadSplineNetwork_NodeRecord>& OutNodes)
{
	const TArray<FCSRoadSplineNetwork_Node>& Nodes = In.Nodes;
	auto Ground = [Terrain](const FVector3d& Point) { return Terrain ? static_cast<double>(Terrain->Sample(CSRoadSplineNetwork_Float(Point))) : 0.0; };
	TArray<FBox2d> PartBoxes;
	PartBoxes.Init(FBox2d(ForceInit), In.Pieces.Num());
	for (const FCSRoadSplineNetwork_Piece& Piece : In.Pieces) if (Piece.bAlive) PartBoxes[Piece.Origin] += CSRoadSplineNetwork_Box(Piece.Curve);
	OutEdges.Reset();
	OutNodes.Reset();
	for (const FCSRoadSplineNetwork_Piece& Piece : In.Pieces)
	{
		if (!Piece.bAlive) continue;
		const FSplineRoad& Spline = *In.Splines[Piece.Spline];
		FCSRoadSplineNetwork_EdgeRecord& Edge = OutEdges.AddDefaulted_GetRef();
		Edge.Curve = Piece.Curve;
		Edge.bElevation = Piece.bElevation;
		Edge.Elevation = Piece.Elevation;
		Edge.PartBox = PartBoxes[Piece.Origin];
		FRoadSpec& Road = Edge.Spec;
		Road.Prefab = Spline.Prefab;
		Road.Spline = Spline.Id;
		Road.Curve = Bezier4x3(CSRoadSplineNetwork_Float(Piece.Curve.A), CSRoadSplineNetwork_Float(Piece.Curve.B), CSRoadSplineNetwork_Float(Piece.Curve.C), CSRoadSplineNetwork_Float(Piece.Curve.D));
		Road.Upgrade = Spline.Upgrade;
		Road.bHasSource = true;
		// The pieces' nodes, until their records are made.
		Road.SourceStart = Piece.Start;
		Road.SourceEnd = Piece.End;
		Road.bHasElevation = true;
		if (Piece.bElevation) Road.Elevation = Piece.Elevation;
		else
		{
			const FVector3d Middle = Piece.Curve.Position(0.5);
			Road.Elevation = float2(static_cast<float>(Middle.Y - Ground(Middle)));
		}
	}
	OutEdges.StableSort(CSRoadSplineNetwork_RecordBefore);

	// Node prefabs left open take the highest node priority of the roads there, the first in edge order
	// on a tie, as RoadWorld's position nodes do.
	TMap<FString, float> Priorities;
	auto Priority = [&Assets, &Priorities](const FString& Prefab)
	{
		if (const float* Known = Priorities.Find(Prefab)) return *Known;
		return Priorities.Add(Prefab, Assets.Network(Prefab).m_NodePriority);
	};
	TArray<FString> NodePrefabs;
	NodePrefabs.SetNum(Nodes.Num());
	for (const FCSRoadSplineNetwork_EdgeRecord& Edge : OutEdges)
	{
		for (const int32 Node : { Edge.Spec.SourceStart, Edge.Spec.SourceEnd })
		{
			if (!Nodes[Node].Prefab.IsEmpty()) NodePrefabs[Node] = Nodes[Node].Prefab;
			else if (NodePrefabs[Node].IsEmpty() || Priority(Edge.Spec.Prefab) > Priority(NodePrefabs[Node])) NodePrefabs[Node] = Edge.Spec.Prefab;
		}
	}

	// Nodes at the very same position, told apart in the order they were made.
	TArray<int32> Ordinals;
	Ordinals.SetNumUninitialized(Nodes.Num());
	{
		TMap<FCSRoadSplineNetwork_NodeKey, int32> Seen;
		Seen.Reserve(Nodes.Num());
		for (int32 N = 0; N < Nodes.Num(); N++) Ordinals[N] = Seen.FindOrAdd(FCSRoadSplineNetwork_NodeKey(Nodes[N].Position, 0), 0)++;
	}
	TArray<int32> RecordOf;
	RecordOf.Init(INDEX_NONE, Nodes.Num());
	for (FCSRoadSplineNetwork_EdgeRecord& Edge : OutEdges)
	{
		for (int32* End : { &Edge.Spec.SourceStart, &Edge.Spec.SourceEnd })
		{
			const int32 Node = *End;
			if (RecordOf[Node] == INDEX_NONE)
			{
				RecordOf[Node] = OutNodes.Num();
				const FCSRoadSplineNetwork_Node& Source = Nodes[Node];
				FCSRoadSplineNetwork_NodeRecord& Record = OutNodes.AddDefaulted_GetRef();
				Record.Position = Source.Position;
				Record.Ordinal = Ordinals[Node];
				FSavedNode& Out = Record.Node;
				Out.Prefab = NodePrefabs[Node];
				Out.Position = CSRoadSplineNetwork_Float(Source.Position);
				Out.bHasElevation = true;
				Out.Elevation = Source.bElevation ? Source.Elevation : float2(static_cast<float>(Source.Position.Y - Ground(Source.Position)));
				Out.Upgrade = Source.Upgrade;
				Out.bOutside = Source.bOutside;
			}
			*End = RecordOf[Node];
		}
	}
}

// The snapshot form itself: the edges in their order, the nodes in the order the edges reach them
// (OutIndex: each node record's index there).
bool CSRoadSplineNetwork_Assemble(const TArray<FCSRoadSplineNetwork_EdgeRecord>& Edges, const TArray<FCSRoadSplineNetwork_NodeRecord>& Nodes, TArray<FSavedNode>& OutNodes,
	TArray<FRoadSpec>& OutRoads, TArray<int32>& OutIndex, FString& OutError)
{
	OutNodes.Reset(Nodes.Num());
	OutRoads.Reset(Edges.Num());
	OutIndex.Init(INDEX_NONE, Nodes.Num());
	for (const FCSRoadSplineNetwork_EdgeRecord& Edge : Edges)
	{
		FRoadSpec& Road = OutRoads.Add_GetRef(Edge.Spec);
		for (int32* End : { &Road.SourceStart, &Road.SourceEnd })
		{
			int32& Slot = OutIndex[*End];
			if (Slot == INDEX_NONE)
			{
				Slot = OutNodes.Num();
				OutNodes.Add(Nodes[*End].Node);
			}
			*End = Slot;
		}
	}
	if (OutRoads.Num() > 0) return true;
	OutError = TEXT("The splines make no road");
	return false;
}

template <typename T>
bool CSRoadSplineNetwork_SameBits(const T& A, const T& B)
{
	return FMemory::Memcmp(&A, &B, sizeof(T)) == 0;
}

bool CSRoadSplineNetwork_SameNode(const FSavedNode& A, const FSavedNode& B)
{
	return A.Prefab.Equals(B.Prefab, ESearchCase::CaseSensitive) && CSRoadSplineNetwork_SameBits(A.Position, B.Position) && CSRoadSplineNetwork_SameBits(A.Rotation, B.Rotation)
		&& A.bHasElevation == B.bHasElevation && CSRoadSplineNetwork_SameBits(A.Elevation, B.Elevation) && A.Upgrade == B.Upgrade && A.bOutside == B.bOutside;
}

// bNodes: the node indices as well.
bool CSRoadSplineNetwork_SameRoad(const FRoadSpec& A, const FRoadSpec& B, bool bNodes)
{
	return A.Prefab.Equals(B.Prefab, ESearchCase::CaseSensitive) && CSRoadSplineNetwork_SameBits(A.Curve, B.Curve) && A.Upgrade == B.Upgrade && A.bHasSource == B.bHasSource
		&& (!bNodes || (A.SourceStart == B.SourceStart && A.SourceEnd == B.SourceEnd)) && A.bHasElevation == B.bHasElevation && CSRoadSplineNetwork_SameBits(A.Elevation, B.Elevation);
}

// Two edge records alike, their nodes' records included.
bool CSRoadSplineNetwork_SameEdge(const FCSRoadSplineNetwork_EdgeRecord& A, const TArray<FCSRoadSplineNetwork_NodeRecord>& NodesA, const FCSRoadSplineNetwork_EdgeRecord& B,
	const TArray<FCSRoadSplineNetwork_NodeRecord>& NodesB)
{
	if (!CSRoadSplineNetwork_SameBits(A.Curve, B.Curve) || A.bElevation != B.bElevation || !CSRoadSplineNetwork_SameBits(A.Elevation, B.Elevation) || !CSRoadSplineNetwork_SameRoad(A.Spec, B.Spec, false)) return false;
	for (const bool bStart : { true, false })
	{
		const FCSRoadSplineNetwork_NodeRecord& NodeA = NodesA[bStart ? A.Spec.SourceStart : A.Spec.SourceEnd];
		const FCSRoadSplineNetwork_NodeRecord& NodeB = NodesB[bStart ? B.Spec.SourceStart : B.Spec.SourceEnd];
		if (!CSRoadSplineNetwork_SameBits(NodeA.Position, NodeB.Position) || NodeA.Ordinal != NodeB.Ordinal || !CSRoadSplineNetwork_SameNode(NodeA.Node, NodeB.Node)) return false;
	}
	return true;
}

// Values that differ between two snapshot forms, the first described in OutFirst.
int32 CSRoadSplineNetwork_Differences(const TArray<FSavedNode>& NodesA, const TArray<FRoadSpec>& RoadsA, const TArray<FSavedNode>& NodesB, const TArray<FRoadSpec>& RoadsB, FString& OutFirst)
{
	int32 Count = FMath::Abs(NodesA.Num() - NodesB.Num()) + FMath::Abs(RoadsA.Num() - RoadsB.Num());
	if (Count > 0) OutFirst = FString::Printf(TEXT("%d nodes and %d edges against %d and %d"), NodesB.Num(), RoadsB.Num(), NodesA.Num(), RoadsA.Num());
	for (int32 I = 0; I < FMath::Min(NodesA.Num(), NodesB.Num()); I++)
	{
		if (CSRoadSplineNetwork_SameNode(NodesA[I], NodesB[I])) continue;
		if (Count++ == 0) OutFirst = FString::Printf(TEXT("node %d at (%.3f, %.3f, %.3f)"), I, NodesB[I].Position.x, NodesB[I].Position.y, NodesB[I].Position.z);
	}
	for (int32 I = 0; I < FMath::Min(RoadsA.Num(), RoadsB.Num()); I++)
	{
		if (CSRoadSplineNetwork_SameRoad(RoadsA[I], RoadsB[I], true)) continue;
		if (Count++ == 0) OutFirst = FString::Printf(TEXT("edge %d from (%.3f, %.3f, %.3f)"), I, RoadsB[I].Curve.a.x, RoadsB[I].Curve.a.y, RoadsB[I].Curve.a.z);
	}
	return Count;
}

bool CSRoadSplineNetwork_SameData(const FSplinePointData& A, const FSplinePointData& B)
{
	return CSRoadSplineNetwork_SameBits(A.NodeOffset, B.NodeOffset) && A.NodePrefab.Equals(B.NodePrefab, ESearchCase::CaseSensitive) && A.bNodeElevation == B.bNodeElevation
		&& CSRoadSplineNetwork_SameBits(A.NodeElevation, B.NodeElevation) && A.NodeUpgrade == B.NodeUpgrade && A.bOutside == B.bOutside && A.bSegmentElevation == B.bSegmentElevation
		&& CSRoadSplineNetwork_SameBits(A.SegmentElevation, B.SegmentElevation) && A.bJoin == B.bJoin;
}

// The same settings and number of points, with both tangents at every point: compared point by point.
bool CSRoadSplineNetwork_SameShape(const FSplineRoad& A, const FSplineRoad& B)
{
	const int32 Count = A.Points.Num();
	return A.Prefab.Equals(B.Prefab, ESearchCase::CaseSensitive) && A.Upgrade == B.Upgrade && A.bAdjust == B.bAdjust && A.bAlley == B.bAlley && A.bClosed == B.bClosed
		&& B.Points.Num() == Count && A.Arrive.Num() == Count && A.Leave.Num() == Count && B.Arrive.Num() == Count && B.Leave.Num() == Count;
}

// Point I says the same in both: its position, tangents and data, and whether its segment is straight.
bool CSRoadSplineNetwork_SamePoint(const FSplineRoad& A, const FSplineRoad& B, int32 I)
{
	static const FSplinePointData NoData;
	const FSplinePointData& DataA = A.Data.IsValidIndex(I) ? A.Data[I] : NoData;
	const FSplinePointData& DataB = B.Data.IsValidIndex(I) ? B.Data[I] : NoData;
	const bool bLinearA = A.Linear.IsValidIndex(I) && A.Linear[I];
	const bool bLinearB = B.Linear.IsValidIndex(I) && B.Linear[I];
	return CSRoadSplineNetwork_SameBits(A.Points[I], B.Points[I]) && CSRoadSplineNetwork_SameBits(A.Arrive[I], B.Arrive[I]) && CSRoadSplineNetwork_SameBits(A.Leave[I], B.Leave[I])
		&& bLinearA == bLinearB && CSRoadSplineNetwork_SameData(DataA, DataB);
}

bool CSRoadSplineNetwork_SameSpline(const FSplineRoad& A, const FSplineRoad& B)
{
	if (!CSRoadSplineNetwork_SameShape(A, B)) return false;
	for (int32 I = 0; I < A.Points.Num(); I++) if (!CSRoadSplineNetwork_SamePoint(A, B, I)) return false;
	return true;
}

bool CSRoadSplineNetwork_SameSettings(const FSplineNetworkSettings& A, const FSplineNetworkSettings& B)
{
	return A.MaxEdgeLength == B.MaxEdgeLength && A.MaxEdgeTurn == B.MaxEdgeTurn && A.MinEdgeLength == B.MinEdgeLength && A.SnapDistance == B.SnapDistance
		&& A.JunctionHeight == B.JunctionHeight && A.AlleyClearance == B.AlleyClearance;
}

// A spline's segments in the ground plane, each with its control points and its ends' nodes (the points
// moved by their node offsets), and all of them. A spline of one point is one box.
void CSRoadSplineNetwork_Boxes(const FSplineRoad& Road, TArray<FBox2d>& OutSegments, FBox2d& OutBox)
{
	OutSegments.Reset();
	OutBox = FBox2d(ForceInit);
	const int32 Count = Road.Points.Num();
	const bool bTangents = Road.Arrive.Num() == Count && Road.Leave.Num() == Count;
	auto Node = [&Road](int32 K, FBox2d& Box)
	{
		Box += CSRoadSplineNetwork_Ground(Road.Points[K]);
		if (Road.Data.IsValidIndex(K)) Box += CSRoadSplineNetwork_Ground(Road.Points[K] + Road.Data[K].NodeOffset);
	};
	for (int32 I = 0; I < CSRoadSplineNetwork_Segments(Road); I++)
	{
		FBox2d Box = bTangents ? CSRoadSplineNetwork_Box(CSRoadSplineNetwork_Segment(Road, I)) : FBox2d(ForceInit);
		Node(I, Box);
		Node((I + 1) % Count, Box);
		OutSegments.Add(Box);
		OutBox += Box;
	}
	if (Count == 1)
	{
		FBox2d Box(ForceInit);
		Node(0, Box);
		OutSegments.Add(Box);
		OutBox += Box;
	}
}

double CSRoadSplineNetwork_HalfWidth(FAssets& Assets, const FSplineRoad& Road)
{
	return Assets.HasRoad(Road.Prefab) ? double(Assets.Geometry(Road.Prefab).m_DefaultWidth) * 0.5 : 0.0;
}

// Boxes in the ground plane, and the box of them all.
struct FCSRoadSplineNetwork_Region
{
	TArray<FBox2d> Boxes;
	FBox2d Bounds = FBox2d(ForceInit);

	// A box within one already there adds nothing.
	void Add(const FBox2d& Box)
	{
		if (!Box.bIsValid || Contains(Box)) return;
		Boxes.Add(Box);
		Bounds += Box;
	}

	FCSRoadSplineNetwork_Region Grown(double By) const
	{
		FCSRoadSplineNetwork_Region Out;
		for (const FBox2d& Box : Boxes) Out.Add(Box.ExpandBy(By));
		return Out;
	}

	bool Touches(const FBox2d& Box) const
	{
		if (!Box.bIsValid || !Bounds.bIsValid || !Box.Intersect(Bounds)) return false;
		for (const FBox2d& Mine : Boxes) if (Box.Intersect(Mine)) return true;
		return false;
	}

	// Within one of the boxes.
	bool Contains(const FBox2d& Box) const
	{
		if (!Box.bIsValid) return true;
		for (const FBox2d& Mine : Boxes) if (Mine.Min.X <= Box.Min.X && Mine.Min.Y <= Box.Min.Y && Box.Max.X <= Mine.Max.X && Box.Max.Y <= Mine.Max.Y) return true;
		return false;
	}
};

// Whether the edges List (indices into Records, in the order of content) hold one alike Edge, its nodes
// included.
bool CSRoadSplineNetwork_Holds(const TArray<FCSRoadSplineNetwork_EdgeRecord>& Records, const TArray<FCSRoadSplineNetwork_NodeRecord>& Nodes, const TArray<int32>& List,
	const FCSRoadSplineNetwork_EdgeRecord& Edge, const TArray<FCSRoadSplineNetwork_NodeRecord>& EdgeNodes)
{
	int32 Low = 0;
	int32 High = List.Num();
	while (Low < High)
	{
		const int32 Middle = (Low + High) / 2;
		if (CSRoadSplineNetwork_RecordBefore(Records[List[Middle]], Edge)) Low = Middle + 1;
		else High = Middle;
	}
	for (int32 I = Low; I < List.Num() && !CSRoadSplineNetwork_RecordBefore(Edge, Records[List[I]]); I++) if (CSRoadSplineNetwork_SameEdge(Records[List[I]], Nodes, Edge, EdgeNodes)) return true;
	return false;
}

// What changed between two lists of edges in the order of content, Gone of the last build and Come of this
// one, every other edge and node being in both: by content, a changed edge or node one gone and one come.
// LastShared and Shared mark the node records in both builds, left out.
struct FCSRoadSplineNetwork_Changes
{
	// Positions in Gone and in Come.
	TArray<int32> GoneEdges;
	TArray<int32> CameEdges;
	// Node records of the last build and of this one.
	TArray<int32> GoneNodes;
	TArray<int32> CameNodes;
};

FCSRoadSplineNetwork_Changes CSRoadSplineNetwork_Changes(const TArray<FCSRoadSplineNetwork_EdgeRecord>& LastEdges, const TArray<FCSRoadSplineNetwork_NodeRecord>& LastNodes,
	const TArray<int32>& Gone, TFunctionRef<bool(int32)> LastShared, const TArray<FCSRoadSplineNetwork_EdgeRecord>& Edges, const TArray<FCSRoadSplineNetwork_NodeRecord>& Nodes,
	const TArray<int32>& Come, TFunctionRef<bool(int32)> Shared)
{
	FCSRoadSplineNetwork_Changes Out;
	for (int32 I = 0, J = 0; I < Gone.Num() || J < Come.Num();)
	{
		const FCSRoadSplineNetwork_EdgeRecord* A = I < Gone.Num() ? &LastEdges[Gone[I]] : nullptr;
		const FCSRoadSplineNetwork_EdgeRecord* B = J < Come.Num() ? &Edges[Come[J]] : nullptr;
		if (A && B && CSRoadSplineNetwork_SameEdge(*A, LastNodes, *B, Nodes))
		{
			I++;
			J++;
			continue;
		}
		// The earlier of the two in the order of content has no counterpart; with neither earlier, both changed.
		const bool bGone = A && (!B || !CSRoadSplineNetwork_RecordBefore(*B, *A));
		const bool bCame = B && (!A || !CSRoadSplineNetwork_RecordBefore(*A, *B));
		if (bGone) Out.GoneEdges.Add(I++);
		if (bCame) Out.CameEdges.Add(J++);
	}
	TMap<FCSRoadSplineNetwork_NodeKey, int32> Before;
	TBitArray<> Seen(false, LastNodes.Num());
	for (const int32 E : Gone)
	{
		for (const int32 Node : { LastEdges[E].Spec.SourceStart, LastEdges[E].Spec.SourceEnd })
		{
			if (Seen[Node] || LastShared(Node)) continue;
			Seen[Node] = true;
			Before.Add(CSRoadSplineNetwork_Key(LastNodes[Node]), Node);
		}
	}
	Seen.Init(false, Nodes.Num());
	for (const int32 E : Come)
	{
		for (const int32 Node : { Edges[E].Spec.SourceStart, Edges[E].Spec.SourceEnd })
		{
			if (Seen[Node] || Shared(Node)) continue;
			Seen[Node] = true;
			int32 Last = INDEX_NONE;
			if (Before.RemoveAndCopyValue(CSRoadSplineNetwork_Key(Nodes[Node]), Last) && CSRoadSplineNetwork_SameNode(LastNodes[Last].Node, Nodes[Node].Node)) continue;
			if (Last != INDEX_NONE) Out.GoneNodes.Add(Last);
			Out.CameNodes.Add(Node);
		}
	}
	for (const TPair<FCSRoadSplineNetwork_NodeKey, int32>& Pair : Before) Out.GoneNodes.Add(Pair.Value);
	return Out;
}

void CSRoadSplineNetwork_AddTimes(FSplineNetworkStats& To, const FSplineNetworkStats& From)
{
	To.SortMs += From.SortMs;
	To.AlleyMs += From.AlleyMs;
	To.NodeMs += From.NodeMs;
	To.JoinMs += From.JoinMs;
	To.CrossMs += From.CrossMs;
	To.ShapeMs += From.ShapeMs;
	To.OutputMs += From.OutputMs;
}
}

struct FSplineNetworkCache::FState
{
	const FAssets* Assets = nullptr;
	FSplineNetworkSettings Settings;
	const FSavedTerrain* Terrain = nullptr;
	uint64 TerrainSerial = 0;
	// Every spline has an id of its own: they can be told apart from one build to the next.
	bool bIds = false;
	// The widest road's half width the splines have had: how far a change can reach.
	double HalfWidth = 0.0;
	struct FSpline
	{
		FSplineRoad Road;
		// In the ground plane: each segment with its ends' nodes, and all of them.
		TArray<FBox2d> Segments;
		FBox2d Box = FBox2d(ForceInit);
	};
	TMap<uint64, FSpline> Splines;
	// The snapshot form as records, the edges in their order; each node record's index in the output.
	TArray<FCSRoadSplineNetwork_EdgeRecord> Edges;
	TArray<FCSRoadSplineNetwork_NodeRecord> Nodes;
	TArray<int32> NodeOutput;
};

FSplineNetworkCache::FSplineNetworkCache() = default;

FSplineNetworkCache::~FSplineNetworkCache() = default;

void FSplineNetworkCache::Reset()
{
	Diff = FSplineNetworkDiff();
	State.Reset();
}

namespace
{
// Every spline converted, as records.
bool CSRoadSplineNetwork_Whole(FAssets& Assets, const TArray<FSplineRoad>& Drawn, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FCSRoadSplineNetwork_EdgeRecord>& OutEdges, TArray<FCSRoadSplineNetwork_NodeRecord>& OutNodes, FSplineNetworkStats& OutStats, FString& OutError)
{
	TArray<const FSplineRoad*> All;
	All.Reserve(Drawn.Num());
	for (const FSplineRoad& Road : Drawn) All.Add(&Road);
	FCSRoadSplineNetwork_Converted Converted;
	if (!CSRoadSplineNetwork_Convert(Assets, All, Settings, Converted, OutStats, OutError)) return false;
	const double Start = FPlatformTime::Seconds();
	CSRoadSplineNetwork_Records(Assets, Converted, Terrain, OutEdges, OutNodes);
	OutStats.OutputMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	return true;
}

// The cache's state after a build of every spline: its records, and the splines when each has an id of its own.
TUniquePtr<FSplineNetworkCache::FState> CSRoadSplineNetwork_State(FAssets& Assets, const TArray<FSplineRoad>& Drawn, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FCSRoadSplineNetwork_EdgeRecord>&& Edges, TArray<FCSRoadSplineNetwork_NodeRecord>&& Nodes, TArray<int32>&& NodeOutput)
{
	TUniquePtr<FSplineNetworkCache::FState> State = MakeUnique<FSplineNetworkCache::FState>();
	State->Assets = &Assets;
	State->Settings = Settings;
	State->Terrain = Terrain;
	State->TerrainSerial = Terrain ? Terrain->Serial : 0;
	State->Edges = MoveTemp(Edges);
	State->Nodes = MoveTemp(Nodes);
	State->NodeOutput = MoveTemp(NodeOutput);
	State->bIds = true;
	State->Splines.Reserve(Drawn.Num());
	for (const FSplineRoad& Road : Drawn)
	{
		if (Road.Id == 0 || State->Splines.Contains(Road.Id))
		{
			State->bIds = false;
			State->Splines.Empty();
			break;
		}
		FSplineNetworkCache::FState::FSpline& Spline = State->Splines.Add(Road.Id);
		Spline.Road = Road;
		CSRoadSplineNetwork_Boxes(Road, Spline.Segments, Spline.Box);
		State->HalfWidth = FMath::Max(State->HalfWidth, CSRoadSplineNetwork_HalfWidth(Assets, Road));
	}
	return State;
}

enum class ECSRoadSplineNetwork_Around : uint8
{
	Built,
	Failed,
	// Every spline is to be converted: the change reaches too far, or the splines are not told apart.
	Whole,
};

// The network from the cache's last build, converting only the splines around those that changed. A change
// reaches as far as a point snaps, a road ends on another or an alley gives way to a main road (Reach), and
// along a road over the whole piece its edges are split from. The edges of the pieces within Reach of the
// change are made again; every other edge is taken as the last build made it. The edges made again must
// have been converted together with all they meet (Covered), else more splines are converted; and the
// edges meeting them (a node in common) must come out as before, else the change reaches farther. Either
// way the round is made again.
ECSRoadSplineNetwork_Around CSRoadSplineNetwork_Around(FSplineNetworkCache& Cache, FAssets& Assets, const TArray<FSplineRoad>& Drawn, const FSplineNetworkSettings& Settings,
	const FSavedTerrain* Terrain, TArray<FSavedNode>& OutNodes, TArray<FRoadSpec>& OutRoads, FSplineNetworkStats& OutStats, FString& OutError)
{
	using EStep = ECSRoadSplineNetwork_Around;
	using FSpline = FSplineNetworkCache::FState::FSpline;
	FSplineNetworkCache::FState& State = *Cache.State;
	double LapStart = FPlatformTime::Seconds();
	auto Lap = [&LapStart]()
	{
		const double Now = FPlatformTime::Seconds();
		const double Milliseconds = (Now - LapStart) * 1000.0;
		LapStart = Now;
		return Milliseconds;
	};

	// What changed: splines added, gone, or saying anything else; of a spline with the same settings and
	// number of points, only the segments at the points that changed.
	TMap<uint64, const FSplineRoad*> Current;
	Current.Reserve(Drawn.Num());
	for (const FSplineRoad& Road : Drawn)
	{
		if (Road.Id == 0 || Current.Contains(Road.Id)) return EStep::Whole;
		Current.Add(Road.Id, &Road);
	}
	FCSRoadSplineNetwork_Region Change;
	TMap<uint64, FSpline> Fresh;
	double HalfWidth = State.HalfWidth;
	for (const TPair<uint64, const FSplineRoad*>& Pair : Current)
	{
		const FSplineRoad& Road = *Pair.Value;
		const FSpline* Old = State.Splines.Find(Pair.Key);
		if (Old && CSRoadSplineNetwork_SameSpline(Old->Road, Road)) continue;
		FSpline& New = Fresh.Add(Pair.Key);
		CSRoadSplineNetwork_Boxes(Road, New.Segments, New.Box);
		HalfWidth = FMath::Max(HalfWidth, CSRoadSplineNetwork_HalfWidth(Assets, Road));
		if (Old && CSRoadSplineNetwork_SameShape(Old->Road, Road) && Old->Segments.Num() == New.Segments.Num())
		{
			const int32 Count = Road.Points.Num();
			for (int32 I = 0; I < Count; I++)
			{
				if (CSRoadSplineNetwork_SamePoint(Old->Road, Road, I)) continue;
				for (const int32 Segment : { Road.bClosed ? (I + Count - 1) % Count : I - 1, I })
				{
					if (!New.Segments.IsValidIndex(Segment)) continue;
					Change.Add(Old->Segments[Segment]);
					Change.Add(New.Segments[Segment]);
				}
			}
			continue;
		}
		for (const FBox2d& Box : New.Segments) Change.Add(Box);
		if (Old) for (const FBox2d& Box : Old->Segments) Change.Add(Box);
	}
	TArray<uint64> GoneSplines;
	for (const TPair<uint64, FSpline>& Pair : State.Splines)
	{
		if (Current.Contains(Pair.Key)) continue;
		GoneSplines.Add(Pair.Key);
		for (const FBox2d& Box : Pair.Value.Segments) Change.Add(Box);
	}
	OutStats.Changed = Fresh.Num() + GoneSplines.Num();
	OutStats.DiffMs = Lap();
	if (OutStats.Changed == 0)
	{
		OutStats.bWhole = false;
		Cache.Diff.bValid = true;
		TArray<int32> NodeOutput;
		return CSRoadSplineNetwork_Assemble(State.Edges, State.Nodes, OutNodes, OutRoads, NodeOutput, OutError) ? EStep::Built : EStep::Failed;
	}
	if (Change.Boxes.Num() == 0) return EStep::Whole;

	const double Reach = FMath::Max(Settings.SnapDistance * 4.0, HalfWidth * 2.0 + Settings.AlleyClearance + 2.0) + 1.0;
	// The splines within Reach of these boxes are converted: an edge whose piece lies within one of them is
	// converted together with all it meets.
	FCSRoadSplineNetwork_Region Covered;
	for (int32 Round = 1; Round <= 8; Round++)
	{
		OutStats.Rounds = Round;
		const FCSRoadSplineNetwork_Region Near = Change.Grown(Reach);
		// The last build's edges made again, those meeting them, and the others.
		TArray<int32> Replaced;
		TArray<int32> Kept;
		TBitArray<> ReplacedNode(false, State.Nodes.Num());
		for (int32 E = 0; E < State.Edges.Num(); E++)
		{
			const FCSRoadSplineNetwork_EdgeRecord& Edge = State.Edges[E];
			if (!Near.Touches(Edge.PartBox))
			{
				Kept.Add(E);
				continue;
			}
			Replaced.Add(E);
			ReplacedNode[Edge.Spec.SourceStart] = true;
			ReplacedNode[Edge.Spec.SourceEnd] = true;
		}
		TArray<int32> OldRing;
		for (const int32 E : Kept) if (ReplacedNode[State.Edges[E].Spec.SourceStart] || ReplacedNode[State.Edges[E].Spec.SourceEnd]) OldRing.Add(E);
		for (const FBox2d& Box : Change.Grown(Reach * 2.0).Boxes) Covered.Add(Box);
		for (const int32 E : Replaced) Covered.Add(State.Edges[E].PartBox);
		for (const int32 E : OldRing) Covered.Add(State.Edges[E].PartBox);
		const FCSRoadSplineNetwork_Region Context = Covered.Grown(Reach);
		TArray<const FSplineRoad*> Taken;
		for (const FSplineRoad& Road : Drawn)
		{
			const FSpline* Boxes = Fresh.Find(Road.Id);
			if (!Boxes) Boxes = State.Splines.Find(Road.Id);
			if (!Boxes || !Context.Touches(Boxes->Box)) continue;
			for (const FBox2d& Box : Boxes->Segments)
			{
				if (!Context.Touches(Box)) continue;
				Taken.Add(&Road);
				break;
			}
		}
		// Past half the network, converting it all is no dearer.
		if (Taken.Num() * 2 > Drawn.Num()) return EStep::Whole;
		OutStats.DiffMs += Lap();
		FSplineNetworkStats RoundStats;
		FCSRoadSplineNetwork_Converted Converted;
		if (!CSRoadSplineNetwork_Convert(Assets, Taken, Settings, Converted, RoundStats, OutError)) return EStep::Failed;
		(void)Lap();
		TArray<FCSRoadSplineNetwork_EdgeRecord> NewEdges;
		TArray<FCSRoadSplineNetwork_NodeRecord> NewNodes;
		CSRoadSplineNetwork_Records(Assets, Converted, Terrain, NewEdges, NewNodes);
		RoundStats.OutputMs = Lap();
		CSRoadSplineNetwork_AddTimes(OutStats, RoundStats);

		// This round's edges made again, and the others.
		TArray<int32> Made;
		TArray<int32> Rest;
		TBitArray<> MadeNode(false, NewNodes.Num());
		for (int32 E = 0; E < NewEdges.Num(); E++)
		{
			const FCSRoadSplineNetwork_EdgeRecord& Edge = NewEdges[E];
			if (!Near.Touches(Edge.PartBox))
			{
				Rest.Add(E);
				continue;
			}
			Made.Add(E);
			MadeNode[Edge.Spec.SourceStart] = true;
			MadeNode[Edge.Spec.SourceEnd] = true;
		}
		// The change reaches farther, or more splines are to be converted.
		bool bReached = false;
		bool bShort = false;
		for (const int32 E : Made)
		{
			if (Covered.Contains(NewEdges[E].PartBox)) continue;
			Covered.Add(NewEdges[E].PartBox);
			bShort = true;
		}
		// The edges meeting those made again, in this round and in the last build, as the other build has them.
		for (const int32 E : Rest)
		{
			const FCSRoadSplineNetwork_EdgeRecord& Edge = NewEdges[E];
			if ((!MadeNode[Edge.Spec.SourceStart] && !MadeNode[Edge.Spec.SourceEnd]) || CSRoadSplineNetwork_Holds(State.Edges, State.Nodes, Kept, Edge, NewNodes)) continue;
			if (Covered.Contains(Edge.PartBox))
			{
				Change.Add(Edge.PartBox);
				bReached = true;
			}
			else
			{
				Covered.Add(Edge.PartBox);
				bShort = true;
			}
		}
		for (const int32 E : OldRing)
		{
			if (CSRoadSplineNetwork_Holds(NewEdges, NewNodes, Rest, State.Edges[E], State.Nodes)) continue;
			Change.Add(State.Edges[E].PartBox);
			bReached = true;
		}
		// A node the made edges share with the kept ones must be the same in both.
		TBitArray<> KeptNode(false, State.Nodes.Num());
		TMap<FCSRoadSplineNetwork_NodeKey, int32> KeptNodes;
		for (const int32 E : Kept)
		{
			const FCSRoadSplineNetwork_EdgeRecord& Edge = State.Edges[E];
			const bool bNear = Covered.Touches(Edge.PartBox);
			for (const int32 Node : { Edge.Spec.SourceStart, Edge.Spec.SourceEnd })
			{
				KeptNode[Node] = true;
				if (bNear) KeptNodes.Add(CSRoadSplineNetwork_Key(State.Nodes[Node]), Node);
			}
		}
		TArray<int32> SharedWith;
		SharedWith.Init(INDEX_NONE, NewNodes.Num());
		for (TConstSetBitIterator<> It(MadeNode); It; ++It)
		{
			const FCSRoadSplineNetwork_NodeRecord& Record = NewNodes[It.GetIndex()];
			const int32* Known = KeptNodes.Find(CSRoadSplineNetwork_Key(Record));
			if (!Known) continue;
			if (CSRoadSplineNetwork_SameNode(State.Nodes[*Known].Node, Record.Node)) SharedWith[It.GetIndex()] = *Known;
			else
			{
				Change.Add(FBox2d(CSRoadSplineNetwork_Ground(Record.Position), CSRoadSplineNetwork_Ground(Record.Position)));
				bReached = true;
			}
		}
		if (bReached || bShort)
		{
			OutStats.MergeMs += Lap();
			continue;
		}

		// What changed, before the records move.
		const FCSRoadSplineNetwork_Changes Changes = CSRoadSplineNetwork_Changes(State.Edges, State.Nodes, Replaced, [&KeptNode](int32 Node) { return bool(KeptNode[Node]); }, NewEdges,
			NewNodes, Made, [&SharedWith](int32 Node) { return SharedWith[Node] != INDEX_NONE; });

		// The kept edges and the made ones, both in the order of content: merged, a kept edge first where two
		// tie. The kept nodes go first.
		TArray<FCSRoadSplineNetwork_NodeRecord> Nodes;
		Nodes.Reserve(State.Nodes.Num());
		TArray<int32> OldNode;
		OldNode.Init(INDEX_NONE, State.Nodes.Num());
		for (TConstSetBitIterator<> It(KeptNode); It; ++It) OldNode[It.GetIndex()] = Nodes.Add(MoveTemp(State.Nodes[It.GetIndex()]));
		TArray<int32> NewNode;
		NewNode.Init(INDEX_NONE, NewNodes.Num());
		for (TConstSetBitIterator<> It(MadeNode); It; ++It)
		{
			const int32 Node = It.GetIndex();
			NewNode[Node] = SharedWith[Node] != INDEX_NONE ? OldNode[SharedWith[Node]] : Nodes.Add(MoveTemp(NewNodes[Node]));
		}
		TArray<FCSRoadSplineNetwork_EdgeRecord> Edges;
		Edges.Reserve(Kept.Num() + Made.Num());
		TArray<int32> MadeAt;
		MadeAt.SetNumUninitialized(Made.Num());
		auto Take = [&Edges](FCSRoadSplineNetwork_EdgeRecord& Edge, const TArray<int32>& NodeOf)
		{
			FCSRoadSplineNetwork_EdgeRecord& Out = Edges.Add_GetRef(MoveTemp(Edge));
			Out.Spec.SourceStart = NodeOf[Out.Spec.SourceStart];
			Out.Spec.SourceEnd = NodeOf[Out.Spec.SourceEnd];
		};
		for (int32 I = 0, J = 0; I < Kept.Num() || J < Made.Num();)
		{
			if (J >= Made.Num() || (I < Kept.Num() && !CSRoadSplineNetwork_RecordBefore(NewEdges[Made[J]], State.Edges[Kept[I]]))) Take(State.Edges[Kept[I++]], OldNode);
			else
			{
				MadeAt[J] = Edges.Num();
				Take(NewEdges[Made[J++]], NewNode);
			}
		}
		TArray<int32> NodeOutput;
		// The cache's records have moved: on failure it holds no build.
		if (!CSRoadSplineNetwork_Assemble(Edges, Nodes, OutNodes, OutRoads, NodeOutput, OutError)) return EStep::Failed;

		FSplineNetworkDiff& Diff = Cache.Diff;
		Diff.bValid = true;
		for (const int32 Position : Changes.GoneEdges) Diff.RemovedRoads.Add(Replaced[Position]);
		for (const int32 Position : Changes.CameEdges) Diff.AddedRoads.Add(MadeAt[Position]);
		for (const int32 Node : Changes.GoneNodes) Diff.RemovedNodes.Add(State.NodeOutput[Node]);
		for (const int32 Node : Changes.CameNodes) Diff.AddedNodes.Add(NodeOutput[NewNode[Node]]);
		Diff.RemovedNodes.Sort();
		Diff.AddedNodes.Sort();

		State.Edges = MoveTemp(Edges);
		State.Nodes = MoveTemp(Nodes);
		State.NodeOutput = MoveTemp(NodeOutput);
		for (TPair<uint64, FSpline>& Pair : Fresh)
		{
			FSpline& Spline = State.Splines.FindOrAdd(Pair.Key);
			Spline.Road = *Current[Pair.Key];
			Spline.Segments = MoveTemp(Pair.Value.Segments);
			Spline.Box = Pair.Value.Box;
		}
		for (const uint64 Id : GoneSplines) State.Splines.Remove(Id);
		State.HalfWidth = HalfWidth;

		OutStats.Junctions = RoundStats.Junctions;
		OutStats.Splits = RoundStats.Splits;
		OutStats.Merges = RoundStats.Merges;
		OutStats.Dropped = RoundStats.Dropped;
		OutStats.AlleyCuts = RoundStats.AlleyCuts;
		OutStats.bWhole = false;
		OutStats.Converted = Taken.Num();
		OutStats.MergeMs += Lap();
		return EStep::Built;
	}
	return EStep::Whole;
}
}

bool BuildSplineNetwork(FAssets& Assets, const TArray<FSplineRoad>& Drawn, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FSavedNode>& OutNodes, TArray<FRoadSpec>& OutRoads, FSplineNetworkStats& OutStats, FString& OutError, FSplineNetworkCache* Cache, bool bVerify)
{
	using EStep = ECSRoadSplineNetwork_Around;
	OutNodes.Reset();
	OutRoads.Reset();
	OutStats = FSplineNetworkStats();
	if (Cache) Cache->Diff = FSplineNetworkDiff();
	if (Cache && Cache->State && Cache->State->bIds)
	{
		const FSplineNetworkCache::FState& State = *Cache->State;
		const double Start = FPlatformTime::Seconds();
		const bool bSame = State.Assets == &Assets && State.Terrain == Terrain && State.TerrainSerial == (Terrain ? Terrain->Serial : 0) && CSRoadSplineNetwork_SameSettings(State.Settings, Settings);
		const EStep Step = bSame ? CSRoadSplineNetwork_Around(*Cache, Assets, Drawn, Settings, Terrain, OutNodes, OutRoads, OutStats, OutError) : EStep::Whole;
		if (Step == EStep::Failed)
		{
			Cache->Reset();
			return false;
		}
		if (Step == EStep::Built)
		{
			if (!bVerify) return true;
			// Against converting every spline: on a difference, that conversion stands.
			FSplineNetworkStats WholeStats;
			TArray<FCSRoadSplineNetwork_EdgeRecord> Edges;
			TArray<FCSRoadSplineNetwork_NodeRecord> Nodes;
			TArray<FSavedNode> WholeNodes;
			TArray<FRoadSpec> WholeRoads;
			TArray<int32> NodeOutput;
			if (!CSRoadSplineNetwork_Whole(Assets, Drawn, Settings, Terrain, Edges, Nodes, WholeStats, OutError) || !CSRoadSplineNetwork_Assemble(Edges, Nodes, WholeNodes, WholeRoads, NodeOutput, OutError))
			{
				Cache->Reset();
				return false;
			}
			FString First;
			OutStats.Mismatches = CSRoadSplineNetwork_Differences(WholeNodes, WholeRoads, OutNodes, OutRoads, First);
			if (OutStats.Mismatches == 0) return true;
			UE_LOG(LogCSRoadNet, Warning, TEXT("The spline network converted around %d changed splines differs from converting them all in %d values (first: %s); that conversion is used"),
				OutStats.Changed, OutStats.Mismatches, *First);
			OutNodes = MoveTemp(WholeNodes);
			OutRoads = MoveTemp(WholeRoads);
			Cache->State = CSRoadSplineNetwork_State(Assets, Drawn, Settings, Terrain, MoveTemp(Edges), MoveTemp(Nodes), MoveTemp(NodeOutput));
			Cache->Diff = FSplineNetworkDiff();
			return true;
		}
		// Every spline converted after all: what changed still counts, and so does the time spent trying.
		const int32 Changed = OutStats.Changed;
		const int32 Rounds = OutStats.Rounds;
		OutStats = FSplineNetworkStats();
		OutStats.Changed = Changed;
		OutStats.Rounds = Rounds;
		OutStats.DiffMs = (FPlatformTime::Seconds() - Start) * 1000.0;
	}
	TUniquePtr<FSplineNetworkCache::FState> Last = Cache ? MoveTemp(Cache->State) : nullptr;
	TArray<FCSRoadSplineNetwork_EdgeRecord> Edges;
	TArray<FCSRoadSplineNetwork_NodeRecord> Nodes;
	OutStats.Converted = Drawn.Num();
	if (!CSRoadSplineNetwork_Whole(Assets, Drawn, Settings, Terrain, Edges, Nodes, OutStats, OutError)) return false;
	const double Start = FPlatformTime::Seconds();
	TArray<int32> NodeOutput;
	if (!CSRoadSplineNetwork_Assemble(Edges, Nodes, OutNodes, OutRoads, NodeOutput, OutError)) return false;
	OutStats.OutputMs += (FPlatformTime::Seconds() - Start) * 1000.0;
	if (!Cache) return true;
	if (Last)
	{
		// Every edge and node against the last build's.
		const double DiffStart = FPlatformTime::Seconds();
		auto All = [](int32 Count)
		{
			TArray<int32> Out;
			Out.SetNumUninitialized(Count);
			for (int32 I = 0; I < Count; I++) Out[I] = I;
			return Out;
		};
		auto None = [](int32) { return false; };
		const FCSRoadSplineNetwork_Changes Changes = CSRoadSplineNetwork_Changes(Last->Edges, Last->Nodes, All(Last->Edges.Num()), None, Edges, Nodes, All(Edges.Num()), None);
		FSplineNetworkDiff& Diff = Cache->Diff;
		Diff.bValid = true;
		Diff.RemovedRoads = Changes.GoneEdges;
		Diff.AddedRoads = Changes.CameEdges;
		for (const int32 Node : Changes.GoneNodes) Diff.RemovedNodes.Add(Last->NodeOutput[Node]);
		for (const int32 Node : Changes.CameNodes) Diff.AddedNodes.Add(NodeOutput[Node]);
		Diff.RemovedNodes.Sort();
		Diff.AddedNodes.Sort();
		OutStats.DiffMs += (FPlatformTime::Seconds() - DiffStart) * 1000.0;
	}
	Cache->State = CSRoadSplineNetwork_State(Assets, Drawn, Settings, Terrain, MoveTemp(Edges), MoveTemp(Nodes), MoveTemp(NodeOutput));
	return true;
}

void ChainNetwork(const TArray<FSavedNode>& Nodes, const TArray<FRoadSpec>& Roads, TArray<FSplineRoad>& OutSplines)
{
	OutSplines.Reset();
	auto Valid = [&Nodes](const FRoadSpec& Road) { return Road.bHasSource && Nodes.IsValidIndex(Road.SourceStart) && Nodes.IsValidIndex(Road.SourceEnd); };
	TArray<TArray<int32>> Starting;
	TArray<TArray<int32>> Ending;
	Starting.SetNum(Nodes.Num());
	Ending.SetNum(Nodes.Num());
	for (int32 R = 0; R < Roads.Num(); R++)
	{
		if (!Valid(Roads[R])) continue;
		Starting[Roads[R].SourceStart].Add(R);
		Ending[Roads[R].SourceEnd].Add(R);
	}

	// At each node, the road goes on from an edge ending there into one starting at the same point
	// with the same prefab and upgrades: always at a plain bend, only within 30 degrees at a
	// junction, the straightest pairs first.
	auto Heading = [](const float3& V) { return FMath::Atan2(static_cast<double>(V.z), static_cast<double>(V.x)); };
	TArray<int32> Next;
	TArray<int32> Previous;
	Next.Init(INDEX_NONE, Roads.Num());
	Previous.Init(INDEX_NONE, Roads.Num());
	struct FContinuation
	{
		double Turn = 0.0;
		int32 In = 0;
		int32 Out = 0;
	};
	TArray<FContinuation> Continuations;
	for (int32 N = 0; N < Nodes.Num(); N++)
	{
		const bool bJunction = Starting[N].Num() + Ending[N].Num() > 2;
		Continuations.Reset();
		for (const int32 In : Ending[N])
		{
			for (const int32 Out : Starting[N])
			{
				const FRoadSpec& A = Roads[In];
				const FRoadSpec& B = Roads[Out];
				if (In == Out || A.Prefab != B.Prefab || !math::all(A.Curve.d == B.Curve.a)) continue;
				if (A.Upgrade.m_General != B.Upgrade.m_General || A.Upgrade.m_Left != B.Upgrade.m_Left || A.Upgrade.m_Right != B.Upgrade.m_Right) continue;
				const double Turn = FMath::Abs(FMath::UnwindRadians(Heading(MathUtils::StartTangent(B.Curve)) - Heading(MathUtils::EndTangent(A.Curve))));
				if (bJunction && Turn > UE_DOUBLE_PI / 6.0) continue;
				Continuations.Add({ Turn, In, Out });
			}
		}
		Continuations.StableSort([](const FContinuation& L, const FContinuation& R) { return L.Turn < R.Turn; });
		for (const FContinuation& Continuation : Continuations)
		{
			if (Next[Continuation.In] != INDEX_NONE || Previous[Continuation.Out] != INDEX_NONE) continue;
			Next[Continuation.In] = Continuation.Out;
			Previous[Continuation.Out] = Continuation.In;
		}
	}

	// A spline per chain: its points at the edges' ends, the tangents three times the control
	// arms, each point carrying its node's data and its edge's elevation.
	TArray<bool> Visited;
	Visited.Init(false, Roads.Num());
	auto Emit = [&](int32 First, bool bClosed)
	{
		TArray<int32> Chain;
		for (int32 R = First; R != INDEX_NONE && !Visited[R]; R = Next[R])
		{
			Visited[R] = true;
			Chain.Add(R);
		}
		const FRoadSpec& Head = Roads[Chain[0]];
		FSplineRoad& Spline = OutSplines.AddDefaulted_GetRef();
		Spline.Name = FString::Printf(TEXT("%s %d"), *Head.Prefab, OutSplines.Num() - 1);
		Spline.Prefab = Head.Prefab;
		Spline.Upgrade = Head.Upgrade;
		Spline.bAdjust = false;
		Spline.bClosed = bClosed;
		const int32 Count = bClosed ? Chain.Num() : Chain.Num() + 1;
		for (int32 I = 0; I < Count; I++)
		{
			const bool bEnd = I == Chain.Num();
			const FRoadSpec& Road = Roads[bEnd ? Chain.Last() : Chain[I]];
			const float3 Point = bEnd ? Road.Curve.d : Road.Curve.a;
			FVector3d Arrive;
			FVector3d Leave;
			if (bEnd) Arrive = Leave = CSRoadSplineNetwork_Arm(Road.Curve.c, Road.Curve.d);
			else
			{
				Leave = CSRoadSplineNetwork_Arm(Road.Curve.a, Road.Curve.b);
				const bool bBefore = I > 0 || bClosed;
				const FRoadSpec& Before = Roads[I > 0 ? Chain[I - 1] : Chain.Last()];
				Arrive = bBefore ? CSRoadSplineNetwork_Arm(Before.Curve.c, Before.Curve.d) : Leave;
			}
			Spline.Points.Add(CSRoadSplineNetwork_Double(Point));
			Spline.Arrive.Add(Arrive);
			Spline.Leave.Add(Leave);
			Spline.Linear.Add(false);
			FSplinePointData& Data = Spline.Data.AddDefaulted_GetRef();
			if (Valid(Road))
			{
				const FSavedNode& Node = Nodes[bEnd ? Road.SourceEnd : Road.SourceStart];
				Data.NodeOffset = CSRoadSplineNetwork_Double(Node.Position) - CSRoadSplineNetwork_Double(Point);
				Data.NodePrefab = Node.Prefab;
				Data.bNodeElevation = true;
				Data.NodeElevation = Node.bHasElevation ? Node.Elevation : float2(0.0f);
				Data.NodeUpgrade = Node.Upgrade;
				Data.bOutside = Node.bOutside;
			}
			if (!bEnd)
			{
				Data.bSegmentElevation = true;
				Data.SegmentElevation = Road.bHasElevation ? Road.Elevation : float2(0.0f);
			}
		}
	};
	for (int32 R = 0; R < Roads.Num(); R++) if (Previous[R] == INDEX_NONE) Emit(R, false);
	// What is left runs in rings.
	for (int32 R = 0; R < Roads.Num(); R++) if (!Visited[R]) Emit(R, true);
}
}

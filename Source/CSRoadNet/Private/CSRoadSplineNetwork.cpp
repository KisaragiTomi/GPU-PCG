#include "CSRoadSplineNetwork.h"

#include "Algo/BinarySearch.h"
#include "CSRoadCurves.h"

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
	int32 Owner = 0;
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
	int32 Order = INDEX_NONE;
	int32 Owner = 0;
	bool bAlive = true;
};

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

// The point of a corridor's centre line nearest to P in the ground plane, and on which side of the
// line P lies (+1 / -1).
struct FCSRoadSplineNetwork_OnCorridor
{
	double Distance = TNumericLimits<double>::Max();
	FVector3d Point = FVector3d::ZeroVector;
	double Side = 0.0;
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
		Best.Side = AB.X * AP.Y - AB.Y * AP.X >= 0.0 ? 1.0 : -1.0;
	}
	return Best;
}

// Alleys give way to the main roads over them. Out holds every spline, in order: a main road or an
// alley no main road covers as it is, a covered alley as the parts left of it. False (Out unused)
// when no alley is covered.
bool CSRoadSplineNetwork_ClipAlleys(FAssets& Assets, const TArray<FSplineRoad>& Splines, const FSplineNetworkSettings& Settings, TArray<FSplineRoad>& Out, FSplineNetworkStats& OutStats)
{
	TArray<FCSRoadSplineNetwork_Corridor> Corridors;
	bool bAlleys = false;
	for (const FSplineRoad& Road : Splines)
	{
		bAlleys |= Road.bAlley;
		if (Road.bAlley || !Assets.HasRoad(Road.Prefab) || Road.Arrive.Num() != Road.Points.Num() || Road.Leave.Num() != Road.Points.Num()) continue;
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
	if (!bAlleys || Corridors.Num() == 0) return false;

	Out.Reset(Splines.Num());
	bool bCut = false;
	for (const FSplineRoad& Road : Splines)
	{
		const int32 Segments = CSRoadSplineNetwork_Segments(Road);
		if (!Road.bAlley || Segments == 0 || !Assets.HasRoad(Road.Prefab) || Road.Arrive.Num() != Road.Points.Num() || Road.Leave.Num() != Road.Points.Num())
		{
			Out.Add(Road);
			continue;
		}
		const double Reach = double(Assets.Geometry(Road.Prefab).m_DefaultWidth) * 0.5 + Settings.AlleyClearance;
		TArray<FCSRoadSplineNetwork_Curve> Curves;
		for (int32 I = 0; I < Segments; I++) Curves.Add(CSRoadSplineNetwork_Segment(Road, I));
		auto At = [&Curves, Segments](double U) { const int32 I = FMath::Clamp(FMath::FloorToInt32(U), 0, Segments - 1); return Curves[I].Position(U - I); };
		// The main road nearest to P relative to its reach (its half width plus the alley's), among those
		// at about P's height; covered within that reach. Margin widens the search past the reach.
		auto Nearest = [&Corridors, &Settings, Reach](const FVector3d& P, double Margin, FCSRoadSplineNetwork_OnCorridor& OutOn)
		{
			double Best = TNumericLimits<double>::Max();
			for (const FCSRoadSplineNetwork_Corridor& Corridor : Corridors)
			{
				const double Limit = Corridor.HalfWidth + Reach;
				const double Search = Limit + Margin;
				if (P.X < Corridor.Box.Min.X - Search || P.X > Corridor.Box.Max.X + Search || P.Z < Corridor.Box.Min.Z - Search || P.Z > Corridor.Box.Max.Z + Search) continue;
				const FCSRoadSplineNetwork_OnCorridor On = CSRoadSplineNetwork_Nearest(Corridor, P);
				if (On.Distance > Search || FMath::Abs(P.Y - On.Point.Y) > Settings.JunctionHeight || On.Distance / Limit >= Best) continue;
				Best = On.Distance / Limit;
				OutOn = On;
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
			Out.Add(Road);
			continue;
		}
		bCut = true;
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
		OutStats.AlleyCuts += Runs.Num();
		// A closed alley covered where it starts goes on round to the cover's far side.
		if (Road.bClosed && Runs.Num() > 1 && Runs[0].bStart && Runs.Last().bEnd)
		{
			Runs[0].Enter = Runs.Last().Enter - Segments;
			Runs[0].bStart = false;
			Runs.Pop();
		}
		// Where a cut end joins the main road: one point for both ends of a stretch that crosses it
		// straight over, else the nearest point of the road.
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
				return Closest.Point;
			}
			return bBefore ? Enter.Point : Leave.Point;
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
			if (Before) PartCurves[0].MoveStart(Join(*Before, false));
			if (After) PartCurves.Last().MoveEnd(Join(*After, true));
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
				Part.Linear.Add(Road.Linear.IsValidIndex(Segment) && Road.Linear[Segment]);
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
			Out.Add(MoveTemp(Part));
		}
	}
	return bCut;
}
}

bool BuildSplineNetwork(FAssets& Assets, const TArray<FSplineRoad>& Drawn, const FSplineNetworkSettings& Settings, const FSavedTerrain* Terrain,
	TArray<FSavedNode>& OutNodes, TArray<FRoadSpec>& OutRoads, FSplineNetworkStats& OutStats, FString& OutError)
{
	OutNodes.Reset();
	OutRoads.Reset();
	OutStats = FSplineNetworkStats();
	// Alleys give way to the main roads over them; the splines drawn stay as they are.
	TArray<FSplineRoad> Clipped;
	const TArray<FSplineRoad>& Splines = CSRoadSplineNetwork_ClipAlleys(Assets, Drawn, Settings, Clipped, OutStats) ? Clipped : Drawn;
	const double Snap = FMath::Max(Settings.SnapDistance, 0.01);
	static const FSplinePointData NoData;
	auto DataAt = [&Splines](int32 Spline, int32 Index) -> const FSplinePointData& { return Splines[Spline].Data.IsValidIndex(Index) ? Splines[Spline].Data[Index] : NoData; };

	// Every point, at its node's position.
	TArray<FCSRoadSplineNetwork_Point> Points;
	TArray<int32> FirstPoint;
	for (int32 S = 0; S < Splines.Num(); S++)
	{
		const FSplineRoad& Road = Splines[S];
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
			Point.bFixed = !Road.bAdjust;
			Point.bFree = Road.bAdjust && Data.NodeOffset.IsZero();
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
		TMap<FIntVector, TArray<int32>> Cells;
		auto CellOf = [Snap](const FVector3d& P) { return FIntVector(FMath::FloorToInt32(P.X / Snap), FMath::FloorToInt32(P.Y / Snap), FMath::FloorToInt32(P.Z / Snap)); };
		for (int32 I = 0; I < Points.Num(); I++)
		{
			const FIntVector Cell = CellOf(Points[I].NodePosition);
			for (int32 X = -1; X <= 1; X++)
			{
				for (int32 Y = -1; Y <= 1; Y++)
				{
					for (int32 Z = -1; Z <= 1; Z++)
					{
						const TArray<int32>* Near = Cells.Find(Cell + FIntVector(X, Y, Z));
						if (!Near) continue;
						for (const int32 J : *Near)
						{
							const double Limit = Points[I].bFixed && Points[J].bFixed ? 0.01 : Snap;
							if (FVector3d::Dist(Points[I].NodePosition, Points[J].NodePosition) > Limit) continue;
							const int32 RootI = Find(I);
							const int32 RootJ = Find(J);
							if (RootI != RootJ) Parent[FMath::Max(RootI, RootJ)] = FMath::Min(RootI, RootJ);
						}
					}
				}
			}
			Cells.FindOrAdd(Cell).Add(I);
		}
	}

	// Nodes: at a fixed point's position if the node has one, else at its first point's. Data comes
	// from the first point that sets it, upgrades from all of them.
	TArray<FCSRoadSplineNetwork_Node> Nodes;
	TArray<int32> NodeOfPoint;
	NodeOfPoint.SetNumUninitialized(Points.Num());
	{
		TMap<int32, int32> NodeOfRoot;
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
			if (Node.Owner == 0) Node.Owner = Data.NodeOwner;
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
	TArray<FCSRoadSplineNetwork_Piece> Pieces;
	for (int32 S = 0; S < Splines.Num(); S++)
	{
		const FSplineRoad& Road = Splines[S];
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
			Piece.Order = Data.SegmentOrder;
			Piece.Owner = Data.SegmentOwner;
			Pieces.Add(Piece);
		}
	}

	auto Adjusted = [&Splines](const FCSRoadSplineNetwork_Piece& Piece) { return Splines[Piece.Spline].bAdjust; };
	TArray<TArray<FCSRoadSplineNetwork_Cut>> Cuts;
	Cuts.SetNum(Pieces.Num());

	// A road ending on another one (not at its ends, which snapping settles) joins it there, and so do
	// alley ends cut under a main road whatever their degree. At least one of the two must be adjusted.
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
				if (Q == Incident[N] || Other.Start == N || Other.End == N || (!bOwnAdjusted && !Adjusted(Other))) continue;
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

	// Node prefabs left open take the highest node priority of the roads there, the first on a tie,
	// as RoadWorld's position nodes do.
	TMap<FString, float> Priorities;
	auto Priority = [&Assets, &Priorities](const FString& Prefab)
	{
		if (const float* Known = Priorities.Find(Prefab)) return *Known;
		return Priorities.Add(Prefab, Assets.Network(Prefab).m_NodePriority);
	};
	TArray<FString> NodePrefabs;
	NodePrefabs.SetNum(Nodes.Num());
	for (const FCSRoadSplineNetwork_Piece& Piece : Pieces)
	{
		if (!Piece.bAlive) continue;
		const FString& Prefab = Splines[Piece.Spline].Prefab;
		for (const int32 Node : { Piece.Start, Piece.End })
		{
			if (!Nodes[Node].Prefab.IsEmpty()) NodePrefabs[Node] = Nodes[Node].Prefab;
			else if (NodePrefabs[Node].IsEmpty() || Priority(Prefab) > Priority(NodePrefabs[Node])) NodePrefabs[Node] = Prefab;
		}
	}

	// Snapshot form: the edges in their order (unordered ones after, as they come), the nodes in the
	// order the edges reach them, ids that never collide. Graph.Build numbers entities in that order.
	TArray<int32> Order;
	for (int32 P = 0; P < Pieces.Num(); P++) if (Pieces[P].bAlive) Order.Add(P);
	Order.StableSort([&Pieces](int32 L, int32 R) { return static_cast<uint32>(Pieces[L].Order) < static_cast<uint32>(Pieces[R].Order); });
	auto Ground = [Terrain](const FVector3d& Point) { return Terrain ? static_cast<double>(Terrain->Sample(CSRoadSplineNetwork_Float(Point))) : 0.0; };
	// Owners get ids past any node's or edge's, in the order of their own: only which ids are equal
	// matters, and an edit that adds a node or an edge leaves every owner its id (tiles key roads by it).
	TSet<int32> OwnerSet;
	for (const int32 P : Order)
	{
		const FCSRoadSplineNetwork_Piece& Piece = Pieces[P];
		for (const int32 Owner : { Piece.Owner, Nodes[Piece.Start].Owner, Nodes[Piece.End].Owner }) if (Owner != 0) OwnerSet.Add(Owner);
	}
	TArray<int32> Owners = OwnerSet.Array();
	Owners.Sort();
	auto OwnerId = [&Owners](int32 Owner) { return Owner == 0 ? 0 : (1 << 30) + int32(Algo::BinarySearch(Owners, Owner)); };
	TArray<int32> NodeIndex;
	NodeIndex.Init(INDEX_NONE, Nodes.Num());
	for (const int32 P : Order)
	{
		const FCSRoadSplineNetwork_Piece& Piece = Pieces[P];
		for (const int32 Node : { Piece.Start, Piece.End })
		{
			if (NodeIndex[Node] != INDEX_NONE) continue;
			NodeIndex[Node] = OutNodes.Num();
			const FCSRoadSplineNetwork_Node& Source = Nodes[Node];
			FSavedNode& Out = OutNodes.AddDefaulted_GetRef();
			Out.Id = OutNodes.Num();
			Out.Prefab = NodePrefabs[Node];
			Out.Position = CSRoadSplineNetwork_Float(Source.Position);
			Out.bHasElevation = true;
			Out.Elevation = Source.bElevation ? Source.Elevation : float2(static_cast<float>(Source.Position.Y - Ground(Source.Position)));
			Out.Upgrade = Source.Upgrade;
			Out.bOutside = Source.bOutside;
			Out.Owner = OwnerId(Source.Owner);
		}
	}
	for (const int32 P : Order)
	{
		const FCSRoadSplineNetwork_Piece& Piece = Pieces[P];
		const FSplineRoad& Spline = Splines[Piece.Spline];
		FRoadSpec& Road = OutRoads.AddDefaulted_GetRef();
		Road.Prefab = Spline.Prefab;
		Road.Curve = Bezier4x3(CSRoadSplineNetwork_Float(Piece.Curve.A), CSRoadSplineNetwork_Float(Piece.Curve.B), CSRoadSplineNetwork_Float(Piece.Curve.C), CSRoadSplineNetwork_Float(Piece.Curve.D));
		Road.Upgrade = Spline.Upgrade;
		Road.bHasSource = true;
		Road.SourceId = OutNodes.Num() + OutRoads.Num();
		Road.SourceStart = NodeIndex[Piece.Start];
		Road.SourceEnd = NodeIndex[Piece.End];
		Road.Owner = OwnerId(Piece.Owner);
		Road.bHasElevation = true;
		if (Piece.bElevation) Road.Elevation = Piece.Elevation;
		else
		{
			const FVector3d Middle = Piece.Curve.Position(0.5);
			Road.Elevation = float2(static_cast<float>(Middle.Y - Ground(Middle)));
		}
	}
	if (OutRoads.Num() == 0)
	{
		OutError = TEXT("The splines make no road");
		return false;
	}
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
				Data.NodeOwner = Node.Owner;
			}
			if (!bEnd)
			{
				Data.bSegmentElevation = true;
				Data.SegmentElevation = Road.bHasElevation ? Road.Elevation : float2(0.0f);
				Data.SegmentOrder = Chain[I];
				Data.SegmentOwner = Road.Owner;
			}
		}
	};
	for (int32 R = 0; R < Roads.Num(); R++) if (Previous[R] == INDEX_NONE) Emit(R, false);
	// What is left runs in rings.
	for (int32 R = 0; R < Roads.Num(); R++) if (!Visited[R]) Emit(R, true);
}
}

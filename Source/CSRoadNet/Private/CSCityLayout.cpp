#include "CSCityLayout.h"

#include "CSGpuMeshTypes.h"

namespace CSCity
{
namespace
{
// splitmix64: what every choice of the layout is drawn from, so the same inputs give the same city.
uint64 CSCityLayout_Mix(uint64 X)
{
	X += 0x9E3779B97F4A7C15ull;
	X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ull;
	X = (X ^ (X >> 27)) * 0x94D049BB133111EBull;
	return X ^ (X >> 31);
}

// [0, 1)
double CSCityLayout_Unit(uint64 X) { return double(CSCityLayout_Mix(X) >> 11) * (1.0 / 9007199254740992.0); }

double CSCityLayout_Cross(const FVector2D& A, const FVector2D& B) { return A.X * B.Y - A.Y * B.X; }

FBox2D CSCityLayout_Box(const FPolygon& Polygon)
{
	FBox2D Box(ForceInit);
	for (const FVector2D& P : Polygon) Box += P;
	return Box;
}

// The extent of a polygon along Axis (unit).
FVector2D CSCityLayout_Project(const FPolygon& Polygon, const FVector2D& Axis)
{
	FVector2D Range(TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest());
	for (const FVector2D& P : Polygon)
	{
		const double D = P.Dot(Axis);
		Range.X = FMath::Min(Range.X, D);
		Range.Y = FMath::Max(Range.Y, D);
	}
	return Range;
}

double CSCityLayout_Distance(const FPolygon& Polygon, const FVector2D& P)
{
	if (Contains(Polygon, P)) return 0.0;
	double Best = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Polygon.Num(); I++)
	{
		const FVector2D Closest = FMath::ClosestPointOnSegment2D(P, Polygon[I], Polygon[(I + 1) % Polygon.Num()]);
		Best = FMath::Min(Best, FVector2D::Distance(P, Closest));
	}
	return Best;
}

// The convex polygon cut by the line through Point along Direction: the part to its left (Side > 0)
// or right.
FPolygon CSCityLayout_Half(const FPolygon& Polygon, const FVector2D& Point, const FVector2D& Direction, double Side)
{
	FPolygon Out;
	for (int32 I = 0; I < Polygon.Num(); I++)
	{
		const FVector2D A = Polygon[I];
		const FVector2D B = Polygon[(I + 1) % Polygon.Num()];
		const double DA = CSCityLayout_Cross(Direction, A - Point) * Side;
		const double DB = CSCityLayout_Cross(Direction, B - Point) * Side;
		if (DA >= 0.0) Out.Add(A);
		if ((DA >= 0.0) != (DB >= 0.0)) Out.Add(A + (B - A) * (DA / (DA - DB)));
	}
	return Out;
}

// The smallest box around a polygon whose side lies along one of its edges: (long extent, short
// extent) and the long axis (unit).
void CSCityLayout_Extents(const FPolygon& Polygon, double& OutLong, double& OutShort, FVector2D& OutAxis)
{
	double BestArea = TNumericLimits<double>::Max();
	OutLong = OutShort = 0.0;
	OutAxis = FVector2D(1.0, 0.0);
	for (int32 I = 0; I < Polygon.Num(); I++)
	{
		const FVector2D Edge = Polygon[(I + 1) % Polygon.Num()] - Polygon[I];
		if (Edge.SizeSquared() < 1e-6) continue;
		const FVector2D Along = Edge.GetSafeNormal();
		const FVector2D Across(-Along.Y, Along.X);
		const FVector2D A = CSCityLayout_Project(Polygon, Along);
		const FVector2D B = CSCityLayout_Project(Polygon, Across);
		const double LengthA = A.Y - A.X;
		const double LengthB = B.Y - B.X;
		if (LengthA * LengthB >= BestArea) continue;
		BestArea = LengthA * LengthB;
		OutLong = FMath::Max(LengthA, LengthB);
		OutShort = FMath::Min(LengthA, LengthB);
		OutAxis = LengthA >= LengthB ? Along : Across;
	}
}

// The deepest a plot can be.
double CSCityLayout_Deepest(const FLotSettings& Settings)
{
	return FMath::Max3(Settings.MinDepth, Settings.MaxDepth, Settings.bFill ? Settings.FillDepth : 0.0);
}

// The slants a filled plot's back is tried at (across over back): -1.5 to 1.5.
constexpr int32 CSCityLayout_NumSlopes = 13;
double CSCityLayout_Slope(int32 J) { return (J - CSCityLayout_NumSlopes / 2) * 0.25; }

// A small convex polygon (a plot's frame: S as X, T as Y).
using FCSCityLayout_Small = TArray<FVector2D, TInlineAllocator<10>>;

// Keeps what of Polygon lies where A S + B T + C <= 0.
void CSCityLayout_Keep(FCSCityLayout_Small& Polygon, double A, double B, double C)
{
	FCSCityLayout_Small Out;
	for (int32 I = 0; I < Polygon.Num(); I++)
	{
		const FVector2D& P = Polygon[I];
		const FVector2D& Q = Polygon[(I + 1) % Polygon.Num()];
		const double DP = A * P.X + B * P.Y + C;
		const double DQ = A * Q.X + B * Q.Y + C;
		if (DP <= 0.0) Out.Add(P);
		if ((DP <= 0.0) != (DQ <= 0.0)) Out.Add(P + (Q - P) * (DP / (DP - DQ)));
	}
	Polygon = MoveTemp(Out);
}

// A plot in its own frame: S across it (S0..S1, 0 at its slot's middle), T back from its front, its back T = A + B S.
// The farthest back A may lie with the plot outside the disc of Radius about (SR, TR), the disc clear of the front
// (T = 0): where the back first touches it, along its length or at a corner.
double CSCityLayout_ClearLimit(double SR, double TR, double Radius, double B, double S0, double S1)
{
	double Limit = TNumericLimits<double>::Max();
	if (TR + Radius <= 0.0) return Limit;
	const double Slant = FMath::Sqrt(1.0 + B * B);
	// The back's point nearest the disc's middle as it touches.
	const double FootS = SR + B * Radius / Slant;
	const double FootT = TR - Radius / Slant;
	if (FootS >= S0 && FootS <= S1 && FootT >= 0.0) Limit = TR - B * SR - Radius * Slant;
	for (const double Corner : { S0, S1 })
	{
		const double Off = Corner - SR;
		if (FMath::Abs(Off) >= Radius) continue;
		// A side meets the disc from below at Low; a disc reaching under the front there would take in the front.
		const double Low = TR - FMath::Sqrt(Radius * Radius - Off * Off);
		if (Low >= 0.0) Limit = FMath::Min(Limit, Low - B * Corner);
	}
	return Limit;
}

// Whether convex polygons A and B lie Gap or more apart: no overlap, and no corner of one within Gap of a side of the
// other.
bool CSCityLayout_Apart(const FPolygon& A, const FPolygon& B, double Gap)
{
	if (Overlap(A, B)) return false;
	for (const FPolygon* From : { &A, &B })
	{
		const FPolygon& To = From == &A ? B : A;
		for (const FVector2D& P : *From)
		{
			for (int32 I = 0; I < To.Num(); I++) if (FVector2D::DistSquared(P, FMath::ClosestPointOnSegment2D(P, To[I], To[(I + 1) % To.Num()])) < Gap * Gap) return false;
		}
	}
	return true;
}

// The infinite line through Point along Direction, inside the convex polygon.
bool CSCityLayout_Chord(const FPolygon& Polygon, const FVector2D& Point, const FVector2D& Direction, FVector2D& OutA, FVector2D& OutB)
{
	double T0 = TNumericLimits<double>::Lowest();
	double T1 = TNumericLimits<double>::Max();
	for (int32 I = 0; I < Polygon.Num(); I++)
	{
		const FVector2D A = Polygon[I];
		const FVector2D Edge = Polygon[(I + 1) % Polygon.Num()] - A;
		// Inside is to the left of every edge: cross(Edge, X - A) >= 0.
		const double Start = CSCityLayout_Cross(Edge, Point - A);
		const double Rate = CSCityLayout_Cross(Edge, Direction);
		if (FMath::Abs(Rate) < 1e-12)
		{
			if (Start < 0.0) return false;
			continue;
		}
		const double T = -Start / Rate;
		if (Rate > 0.0) T0 = FMath::Max(T0, T);
		else T1 = FMath::Min(T1, T);
	}
	if (T1 - T0 < 1e-6) return false;
	OutA = Point + Direction * T0;
	OutB = Point + Direction * T1;
	return true;
}
}

FPolygon ConvexHull(TArray<FVector2D> Points)
{
	Points.Sort([](const FVector2D& A, const FVector2D& B) { return A.X != B.X ? A.X < B.X : A.Y < B.Y; });
	TArray<FVector2D> Unique;
	for (const FVector2D& P : Points) if (Unique.IsEmpty() || FVector2D::DistSquared(Unique.Last(), P) > 1e-6) Unique.Add(P);
	if (Unique.Num() < 3) return Unique;
	// Andrew's monotone chain: lower hull, then upper hull, keeping left turns only.
	FPolygon Hull;
	Hull.SetNum(Unique.Num() * 2);
	int32 K = 0;
	auto Turn = [](const FVector2D& O, const FVector2D& A, const FVector2D& B) { return CSCityLayout_Cross(A - O, B - O); };
	for (int32 I = 0; I < Unique.Num(); I++)
	{
		while (K >= 2 && Turn(Hull[K - 2], Hull[K - 1], Unique[I]) <= 0.0) K--;
		Hull[K++] = Unique[I];
	}
	for (int32 I = Unique.Num() - 2, Lower = K + 1; I >= 0; I--)
	{
		while (K >= Lower && Turn(Hull[K - 2], Hull[K - 1], Unique[I]) <= 0.0) K--;
		Hull[K++] = Unique[I];
	}
	Hull.SetNum(K - 1);
	return Hull;
}

FPolygon Grow(const FPolygon& Polygon, double Distance)
{
	if (Polygon.Num() < 3 || Distance == 0.0) return Polygon;
	const int32 Count = Polygon.Num();
	FPolygon Out;
	Out.Reserve(Count);
	for (int32 I = 0; I < Count; I++)
	{
		// Corner I joins edge I-1 (into it) and edge I (out of it), both pushed outward (to the right
		// of a counter-clockwise edge).
		const FVector2D In = (Polygon[I] - Polygon[(I + Count - 1) % Count]).GetSafeNormal();
		const FVector2D On = (Polygon[(I + 1) % Count] - Polygon[I]).GetSafeNormal();
		const FVector2D NormalIn(In.Y, -In.X);
		const FVector2D NormalOn(On.Y, -On.X);
		const FVector2D Mitre = (NormalIn + NormalOn).GetSafeNormal();
		const double Cos = FMath::Max(Mitre.Dot(NormalOn), 0.25);
		Out.Add(Polygon[I] + Mitre * (Distance / Cos));
	}
	return Out;
}

bool Contains(const FPolygon& Polygon, const FVector2D& Point)
{
	if (Polygon.Num() < 3) return false;
	for (int32 I = 0; I < Polygon.Num(); I++) if (CSCityLayout_Cross(Polygon[(I + 1) % Polygon.Num()] - Polygon[I], Point - Polygon[I]) < 0.0) return false;
	return true;
}

bool Overlap(const FPolygon& A, const FPolygon& B)
{
	if (A.Num() < 2 || B.Num() < 2) return false;
	for (const FPolygon* Polygon : { &A, &B })
	{
		for (int32 I = 0; I < Polygon->Num(); I++)
		{
			const FVector2D Edge = (*Polygon)[(I + 1) % Polygon->Num()] - (*Polygon)[I];
			if (Edge.SizeSquared() < 1e-12) continue;
			const FVector2D Axis = FVector2D(-Edge.Y, Edge.X).GetSafeNormal();
			const FVector2D RangeA = CSCityLayout_Project(A, Axis);
			const FVector2D RangeB = CSCityLayout_Project(B, Axis);
			// Touching is not overlapping.
			if (RangeA.Y <= RangeB.X + 1e-3 || RangeB.Y <= RangeA.X + 1e-3) return false;
		}
	}
	return true;
}

bool SegmentHits(const FPolygon& Polygon, const FVector2D& A, const FVector2D& B)
{
	return Overlap(Polygon, { A, B });
}

TArray<TPair<FVector2D, FVector2D>> SegmentOutside(const FPolygon& Polygon, const FVector2D& A, const FVector2D& B)
{
	TArray<TPair<FVector2D, FVector2D>> Out;
	FVector2D In0;
	FVector2D In1;
	const FVector2D Direction = B - A;
	if (Polygon.Num() < 3 || Direction.SizeSquared() < 1e-12 || !CSCityLayout_Chord(Polygon, A, Direction, In0, In1))
	{
		Out.Add({ A, B });
		return Out;
	}
	// The chord's parameters along AB (Direction is not unit: 0 at A, 1 at B).
	const double T0 = FMath::Clamp((In0 - A).Dot(Direction) / Direction.SizeSquared(), 0.0, 1.0);
	const double T1 = FMath::Clamp((In1 - A).Dot(Direction) / Direction.SizeSquared(), 0.0, 1.0);
	if (T1 - T0 < 1e-9)
	{
		Out.Add({ A, B });
		return Out;
	}
	if (T0 > 1e-6) Out.Add({ A, A + Direction * T0 });
	if (T1 < 1.0 - 1e-6) Out.Add({ A + Direction * T1, B });
	return Out;
}

TArray<FAlley> LayoutAlleys(const FPolygon& Region, const TArray<FPolygon>& Reserved, const FAlleyLayoutSettings& Settings)
{
	TArray<FAlley> Out;
	if (Region.Num() < 3) return Out;
	TArray<FPolygon> Kept;
	for (const FPolygon& Plot : Reserved) if (Plot.Num() >= 3) Kept.Add(Grow(Plot, Settings.Clearance));
	auto Clear = [&Kept](const FVector2D& A, const FVector2D& B)
	{
		for (const FPolygon& Plot : Kept) if (SegmentHits(Plot, A, B)) return false;
		return true;
	};

	// Round the edge, the stretches past the reserved plots.
	if (Settings.bBoundary)
	{
		for (int32 I = 0; I < Region.Num(); I++)
		{
			TArray<TPair<FVector2D, FVector2D>> Parts = { { Region[I], Region[(I + 1) % Region.Num()] } };
			for (const FPolygon& Plot : Kept)
			{
				TArray<TPair<FVector2D, FVector2D>> Left;
				for (const TPair<FVector2D, FVector2D>& Part : Parts) Left.Append(SegmentOutside(Plot, Part.Key, Part.Value));
				Parts = MoveTemp(Left);
			}
			for (const TPair<FVector2D, FVector2D>& Part : Parts) if (FVector2D::Distance(Part.Key, Part.Value) > Settings.MinBlockSize * 0.25) Out.Add({ Part.Key, Part.Value });
		}
	}

	// Blocks by their path from the region (1: the region, 2N / 2N+1: N's halves): the path, not the
	// order blocks are visited in, draws each block's split.
	struct FBlock
	{
		FPolygon Polygon;
		uint64 Path = 1;
		int32 Depth = 0;
	};
	TArray<FBlock> Stack;
	Stack.Add({ Region, 1, 0 });
	const uint64 Seed = CSCityLayout_Mix(uint64(uint32(Settings.Seed)));
	while (Stack.Num() > 0)
	{
		const FBlock Block = Stack.Pop();
		double Long = 0.0;
		double Short = 0.0;
		FVector2D Axis;
		CSCityLayout_Extents(Block.Polygon, Long, Short, Axis);
		if (Long <= Settings.BlockSize || Block.Depth >= 20) continue;
		const FVector2D Range = CSCityLayout_Project(Block.Polygon, Axis);
		const FVector2D Across(-Axis.Y, Axis.X);
		const double Shift = (CSCityLayout_Unit(Seed ^ CSCityLayout_Mix(Block.Path * 4 + 1)) * 2.0 - 1.0) * Settings.Jitter;
		const double Turn = FMath::DegreesToRadians((CSCityLayout_Unit(Seed ^ CSCityLayout_Mix(Block.Path * 4 + 2)) * 2.0 - 1.0) * Settings.TurnJitter);
		const FVector2D Direction = Across * FMath::Cos(Turn) + Axis * FMath::Sin(Turn);
		// The jittered middle first, then further out, until a split clears the plots and leaves
		// both halves wide enough.
		bool bSplit = false;
		for (const double Place : { 0.5 + Shift, 0.5 - Shift, 0.4, 0.6, 0.3, 0.7 })
		{
			// A point on the long axis at Place, through the middle of the block's width.
			const FVector2D Middle = CSCityLayout_Box(Block.Polygon).GetCenter();
			const FVector2D Point = Middle + Axis * (Range.X + (Range.Y - Range.X) * Place - Middle.Dot(Axis));
			FVector2D A;
			FVector2D B;
			if (!CSCityLayout_Chord(Block.Polygon, Point, Direction, A, B) || !Clear(A, B)) continue;
			const FPolygon Left = CSCityLayout_Half(Block.Polygon, Point, Direction, 1.0);
			const FPolygon Right = CSCityLayout_Half(Block.Polygon, Point, Direction, -1.0);
			double LongLeft = 0.0;
			double ShortLeft = 0.0;
			double LongRight = 0.0;
			double ShortRight = 0.0;
			FVector2D Unused;
			CSCityLayout_Extents(Left, LongLeft, ShortLeft, Unused);
			CSCityLayout_Extents(Right, LongRight, ShortRight, Unused);
			if (FMath::Min(ShortLeft, ShortRight) < Settings.MinBlockSize) continue;
			Out.Add({ A, B });
			Stack.Add({ Right, Block.Path * 2 + 1, Block.Depth + 1 });
			Stack.Add({ Left, Block.Path * 2, Block.Depth + 1 });
			bSplit = true;
			break;
		}
		(void)bSplit;
	}
	return Out;
}

TArray<FAlley> CutAlleys(const TArray<FAlley>& Alleys, const TArray<FRoadLine>& Roads, double Reach, double Height, double HeightTolerance, double MinLength)
{
	// The roads' segments at about the alleys' height, by the 10 m cells within their reach (and a metre
	// more, for the ends found just outside it).
	struct FRoadSegment
	{
		FVector2D A;
		FVector2D B;
		double Limit = 0.0;
	};
	constexpr double Cell = 1000.0;
	TArray<FRoadSegment> Segments;
	TMap<FIntPoint, TArray<int32>> Grid;
	auto CellOf = [](const FVector2D& P) { return FIntPoint(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell)); };
	for (const FRoadLine& Road : Roads)
	{
		for (int32 I = 0; I + 1 < Road.Points.Num(); I++)
		{
			if (FMath::Abs(Road.Points[I].Z - Height) > HeightTolerance || FMath::Abs(Road.Points[I + 1].Z - Height) > HeightTolerance) continue;
			const FRoadSegment Segment{ FVector2D(Road.Points[I]), FVector2D(Road.Points[I + 1]), Road.HalfWidth + Reach };
			const FVector2D Margin(Segment.Limit + 100.0);
			const FIntPoint Min = CellOf(FVector2D::Min(Segment.A, Segment.B) - Margin);
			const FIntPoint Max = CellOf(FVector2D::Max(Segment.A, Segment.B) + Margin);
			for (int32 Y = Min.Y; Y <= Max.Y; Y++) for (int32 X = Min.X; X <= Max.X; X++) Grid.FindOrAdd(FIntPoint(X, Y)).Add(Segments.Num());
			Segments.Add(Segment);
		}
	}
	// The segment nearest to P relative to its limit: that ratio, the nearest point of its centre line, its
	// direction and the side P lies on.
	struct FOnRoad
	{
		double Ratio = TNumericLimits<double>::Max();
		FVector2D Point = FVector2D::ZeroVector;
		FVector2D Direction = FVector2D::ZeroVector;
		double Side = 0.0;
	};
	auto Nearest = [&Segments, &Grid, &CellOf](const FVector2D& P)
	{
		FOnRoad Best;
		const TArray<int32>* Near = Grid.Find(CellOf(P));
		if (!Near) return Best;
		for (const int32 Index : *Near)
		{
			const FRoadSegment& Segment = Segments[Index];
			const FVector2D On = FMath::ClosestPointOnSegment2D(P, Segment.A, Segment.B);
			const double Ratio = FVector2D::Distance(P, On) / Segment.Limit;
			if (Ratio >= Best.Ratio) continue;
			Best.Ratio = Ratio;
			Best.Point = On;
			Best.Direction = (Segment.B - Segment.A).GetSafeNormal();
			Best.Side = CSCityLayout_Cross(Segment.B - Segment.A, P - Segment.A) >= 0.0 ? 1.0 : -1.0;
		}
		return Best;
	};

	TArray<FAlley> Out;
	const double Flat = FMath::Cos(FMath::DegreesToRadians(30.0));
	for (const FAlley& Alley : Alleys)
	{
		auto At = [&Alley](double T) { return FMath::Lerp(Alley.A, Alley.B, T); };
		auto Covered = [&](double T) { return Nearest(At(T)).Ratio <= 1.0; };
		// Samples about every half metre; where the cover starts and ends, to a few millimetres.
		const int32 Count = FMath::Clamp(FMath::CeilToInt32(FVector2D::Distance(Alley.A, Alley.B) / 50.0), 1, 1 << 16);
		TArray<bool> Inside;
		for (int32 K = 0; K <= Count; K++) Inside.Add(Covered(double(K) / Count));
		if (!Inside.Contains(true))
		{
			Out.Add(Alley);
			continue;
		}
		auto Edge = [&Covered](double Outside, double In)
		{
			for (int32 Step = 0; Step < 20; Step++)
			{
				const double Middle = (Outside + In) * 0.5;
				if (Covered(Middle)) In = Middle;
				else Outside = Middle;
			}
			return Outside;
		};
		// The pieces between the covered stretches: their ends (T) and where those go.
		struct FEnd
		{
			double T = 0.0;
			FVector2D Point = FVector2D::ZeroVector;
			FOnRoad Road;
			bool bCut = false;
		};
		TArray<TPair<FEnd, FEnd>> Pieces;
		FEnd From{ 0.0, Alley.A };
		bool bOpen = !Inside[0];
		for (int32 K = 1; K <= Count; K++)
		{
			if (Inside[K] == Inside[K - 1]) continue;
			const double T0 = double(K - 1) / Count;
			const double T1 = double(K) / Count;
			if (Inside[K])
			{
				// Cover starts: the piece so far ends here.
				const double T = Edge(T0, T1);
				Pieces.Add({ From, FEnd{ T, At(T), Nearest(At(T)), true } });
				bOpen = false;
				continue;
			}
			const double T = Edge(T1, T0);
			From = FEnd{ T, At(T), Nearest(At(T)), true };
			bOpen = true;
		}
		if (bOpen) Pieces.Add({ From, FEnd{ 1.0, Alley.B } });
		// A stretch crossing a road straight over: both pieces beside it meet the road at its closest point.
		for (int32 P = 0; P + 1 < Pieces.Num(); P++)
		{
			FEnd& Before = Pieces[P].Value;
			FEnd& After = Pieces[P + 1].Key;
			if (Before.Road.Side * After.Road.Side >= 0.0) continue;
			FOnRoad Closest;
			double Distance = TNumericLimits<double>::Max();
			for (int32 Step = 0; Step <= 64; Step++)
			{
				const FVector2D Q = At(FMath::Lerp(Before.T, After.T, Step / 64.0));
				const FOnRoad On = Nearest(Q);
				if (On.Ratio <= 1.0 && FVector2D::Distance(Q, On.Point) < Distance)
				{
					Distance = FVector2D::Distance(Q, On.Point);
					Closest = On;
				}
			}
			if (Distance == TNumericLimits<double>::Max()) continue;
			Before.Road = Closest;
			After.Road = Closest;
		}
		// Cut ends onto the road's centre line, unless the piece would meet it flatter than 30 degrees.
		for (TPair<FEnd, FEnd>& Piece : Pieces)
		{
			FVector2D Start = Piece.Key.Point;
			FVector2D End = Piece.Value.Point;
			if (Piece.Key.bCut && FMath::Abs((End - Piece.Key.Road.Point).GetSafeNormal().Dot(Piece.Key.Road.Direction)) < Flat) Start = Piece.Key.Road.Point;
			if (Piece.Value.bCut && FMath::Abs((Piece.Value.Road.Point - Start).GetSafeNormal().Dot(Piece.Value.Road.Direction)) < Flat) End = Piece.Value.Road.Point;
			if (FVector2D::Distance(Start, End) >= MinLength) Out.Add({ Start, End });
		}
	}
	return Out;
}

TArray<FLot> LayoutLots(const TArray<FRoadLine>& Anchors, const TArray<FRoadLine>& Roads, const TArray<FPolygon>& Reserved, const FLotSettings& Settings, TFunctionRef<bool(FLot&)> Place)
{
	TArray<FLot> Out;
	if (Settings.Frontage <= Settings.Gap) return Out;
	const uint64 Seed = CSCityLayout_Mix(uint64(uint32(Settings.Seed)) + 0x5EEDull);
	const bool bFill = Settings.bFill;
	auto SetbackOf = [&Settings](const FRoadLine& Line) { return Line.Setback >= 0.0 ? Line.Setback : Settings.Setback; };
	auto GroupOf = [](const FRoadLine& Line) { return Line.Group != 0 ? Line.Group : Line.Id; };

	// The roads as points a metre or less apart, by 10 m cells.
	constexpr double Cell = 1000.0;
	struct FRoadPoint
	{
		FVector2D Point;
		double HalfWidth = 0.0;
		double Setback = 0.0;
		uint64 Group = 0;
		bool bZoned = true;
		// Along the road, and how far along it either way the point stands for.
		FVector2D Tangent = FVector2D::UnitX();
		double Reach = 0.0;
	};
	TArray<FRoadPoint> Points;
	TMap<FIntPoint, TArray<int32>> Grid;
	// The farthest a road point keeps a plot (half width and half setback) and claims ground for its own (half width
	// and setback).
	double MaxClearance = 0.0;
	double MaxClaim = 0.0;
	auto CellOf = [](const FVector2D& P) { return FIntPoint(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell)); };
	for (const FRoadLine& Road : Roads)
	{
		const double Setback = SetbackOf(Road);
		MaxClearance = FMath::Max(MaxClearance, Road.HalfWidth + Setback * 0.5);
		MaxClaim = FMath::Max(MaxClaim, Road.HalfWidth + Setback);
		for (int32 I = 0; I + 1 < Road.Points.Num(); I++)
		{
			const FVector2D A(Road.Points[I]);
			const FVector2D B(Road.Points[I + 1]);
			const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector2D::Distance(A, B) / 100.0));
			for (int32 S = 0; S < Steps + (I + 2 == Road.Points.Num() ? 1 : 0); S++)
			{
				const FVector2D P = FMath::Lerp(A, B, double(S) / Steps);
				Grid.FindOrAdd(CellOf(P)).Add(Points.Num());
				// Half a step, and a little more so that the stretches overlap.
				Points.Add({ P, Road.HalfWidth, Setback, GroupOf(Road), Road.bZoned, (B - A).GetSafeNormal(), FVector2D::Distance(A, B) / Steps * 0.501 });
			}
		}
	}
	// Visit goes through the road points in the cells within Reach of Box, each once, until it returns false.
	auto Near = [&Grid, &CellOf](const FBox2D& Box, double Reach, TFunctionRef<bool(int32)> Visit)
	{
		const FIntPoint Min = CellOf(Box.Min - FVector2D(Reach));
		const FIntPoint Max = CellOf(Box.Max + FVector2D(Reach));
		for (int32 Y = Min.Y; Y <= Max.Y; Y++)
		{
			for (int32 X = Min.X; X <= Max.X; X++)
			{
				const TArray<int32>* InCell = Grid.Find(FIntPoint(X, Y));
				if (!InCell) continue;
				for (const int32 Index : *InCell) if (!Visit(Index)) return;
			}
		}
	};
	// A shape keeps half each road's setback from its edge.
	auto Clear = [&Points, &Near, MaxClearance](const FPolygon& Corners)
	{
		const FBox2D Box = CSCityLayout_Box(Corners);
		bool bClear = true;
		Near(Box, MaxClearance, [&](int32 Index)
		{
			const FRoadPoint& Road = Points[Index];
			const double Reach = Road.HalfWidth + Road.Setback * 0.5;
			if (Box.ExpandBy(Reach).IsInside(Road.Point) && CSCityLayout_Distance(Corners, Road.Point) < Reach) bClear = false;
			return bClear;
		});
		return bClear;
	};
	// Plots keep clear of the reserved plots, with bFill by half a Gap (and of each other by a Gap, a centimetre spared
	// for neighbours exactly that far apart).
	TArray<FPolygon> Kept;
	TArray<FBox2D> KeptBoxes;
	for (const FPolygon& Plot : Reserved)
	{
		if (Plot.Num() < 3) continue;
		Kept.Add(Plot);
		KeptBoxes.Add(CSCityLayout_Box(Plot).ExpandBy(bFill ? Settings.Gap * 0.5 : 0.0));
	}
	const double ReservedGap = Settings.Gap * 0.5;
	const double RivalGap = FMath::Max(Settings.Gap - 1.0, 0.0);

	// A plot as its front's middle, the directions along its road and away from it, the stretch S0..S1 of its front it
	// takes (of its slot, 0 its middle) and its back T = A + B S, T back from the front: the shape it
	// takes with no plot before it.
	const double Width = Settings.Frontage - Settings.Gap;
	const double Deepest = CSCityLayout_Deepest(Settings);
	const double MinFill = FMath::Max(200.0, Settings.MinDepth * 0.5);
	struct FCandidate
	{
		FLot Lot;
		FVector2D Front = FVector2D::ZeroVector;
		FVector2D Along = FVector2D::ZeroVector;
		FVector2D Back = FVector2D::ZeroVector;
		double S0 = 0.0;
		double S1 = 0.0;
		double A = 0.0;
		double B = 0.0;
		// The narrowest stretch of its front it may keep (of its anchor's frontage).
		double MinWidth = 0.0;
		FBox2D Box;
		uint64 Priority = 0;
	};
	auto Shape = [](const FCandidate& C, double A)
	{
		const FVector2D Left = C.Front + C.Along * C.S0;
		const FVector2D Right = C.Front + C.Along * C.S1;
		return ConvexHull({ Left, Right, Right + C.Back * (A + C.B * C.S1), Left + C.Back * (A + C.B * C.S0) });
	};
	// Its shallower side.
	auto Shallow = [](const FCandidate& C, double A) { return A + FMath::Min(C.B * C.S0, C.B * C.S1); };
	auto Finish = [&Shape, &Shallow](FCandidate& C)
	{
		FLot& Lot = C.Lot;
		Lot.Corners = Shape(C, C.A);
		Lot.Width = C.S1 - C.S0;
		Lot.Depth = Shallow(C, C.A);
		Lot.Front = -C.Back;
		Lot.Centre = C.Front + C.Along * ((C.S0 + C.S1) * 0.5) + C.Back * (Lot.Depth * 0.5);
	};
	// The back as far as A or nearer, its shallow side MinFill or more, where Bad (true from some place back on) is not;
	// false when there is none.
	auto Shrink = [&Shallow, MinFill](FCandidate& C, double A, TFunctionRef<bool(double)> Bad)
	{
		double Low = A - Shallow(C, A) + MinFill;
		if (A < Low) return false;
		if (!Bad(A))
		{
			C.A = A;
			return true;
		}
		if (Bad(Low)) return false;
		for (int32 Step = 0; Step < 24; Step++)
		{
			const double Middle = (Low + A) * 0.5;
			if (Bad(Middle)) A = Middle;
			else Low = Middle;
		}
		C.A = Low;
		return true;
	};
	// With bFill: the plot's front gives up what a road's clearance, another road's ground (a Gap short, across too) or a
	// reserved plot takes of its first MinFill, and keeps the widest stretch left, its MinWidth or more. Its back leaves it
	// the most ground as far back as it may lie (halfway to the front of every other road with plots, a Gap short; half
	// each road's setback from its edge; FillDepth), or lies at Depth when nothing is that near. A road claims the ground
	// beside it, nearer its front than the plot's, not what lies beyond its ends. False when no stretch or no back leaves
	// enough.
	struct FProbe
	{
		double S = 0.0;
		double T = 0.0;
		double Radius = 0.0;
	};
	TArray<FProbe> Probes;
	TArray<FCSCityLayout_Small> Claims;
	TArray<FVector2D> Taken;
	auto Fit = [&](FCandidate& C, uint64 Group, double Depth)
	{
		Probes.Reset();
		Claims.Reset();
		Taken.Reset();
		const double SlotHalf = C.S1;
		FBox2D Strip(ForceInit);
		for (const double S : { -SlotHalf, SlotHalf }) for (const double T : { 0.0, Deepest }) Strip += C.Front + C.Along * S + C.Back * T;
		const double Front = MinFill + Settings.Gap * 0.5;
		Near(Strip, MaxClaim + Deepest + Settings.Gap, [&](int32 Index)
		{
			const FRoadPoint& Road = Points[Index];
			const FVector2D Offset = Road.Point - C.Front;
			FProbe& Probe = Probes.AddDefaulted_GetRef();
			Probe.S = Offset.Dot(C.Along);
			Probe.T = Offset.Dot(C.Back);
			Probe.Radius = Road.HalfWidth + Road.Setback * 0.5;
			if (Probe.T + Probe.Radius > 0.0 && Probe.T - Probe.Radius < MinFill) Taken.Add(FVector2D(Probe.S - Probe.Radius, Probe.S + Probe.Radius));
			// A plot leaves ground to the plots of other roads, never to those of its own road's other parts: what lies within
			// Reach of the point along its road and nearer that road's front (half width and setback across it) than the plot's.
			if (!Road.bZoned || Road.Group == Group) return true;
			const FVector2D Normal(-Road.Tangent.Y, Road.Tangent.X);
			const double U = Road.Tangent.Dot(C.Along);
			const double V = Road.Tangent.Dot(C.Back);
			const double W = -Road.Tangent.Dot(Offset);
			const double NU = Normal.Dot(C.Along);
			const double NV = Normal.Dot(C.Back);
			const double NW = -Normal.Dot(Offset);
			const double K = Road.HalfWidth + Road.Setback;
			const double Low = -Settings.Gap;
			const double High = Deepest + Settings.Gap;
			FCSCityLayout_Small Claimed = { FVector2D(-SlotHalf, Low), FVector2D(SlotHalf, Low), FVector2D(SlotHalf, High), FVector2D(-SlotHalf, High) };
			CSCityLayout_Keep(Claimed, U, V, W - Road.Reach);
			CSCityLayout_Keep(Claimed, -U, -V, -W - Road.Reach);
			CSCityLayout_Keep(Claimed, NU, NV - 1.0, NW - K);
			CSCityLayout_Keep(Claimed, -NU, -NV - 1.0, -NW - K);
			if (Claimed.Num() < 3) return true;
			FCSCityLayout_Small InFront = Claimed;
			CSCityLayout_Keep(InFront, 0.0, 1.0, -Front);
			if (InFront.Num() >= 3)
			{
				FVector2D Across(TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest());
				for (const FVector2D& Corner : InFront) Across = FVector2D(FMath::Min(Across.X, Corner.X), FMath::Max(Across.Y, Corner.X));
				Taken.Add(FVector2D(Across.X - Settings.Gap * 0.5, Across.Y + Settings.Gap * 0.5));
			}
			Claims.Add(MoveTemp(Claimed));
			return true;
		});
		for (int32 R = 0; R < Kept.Num(); R++)
		{
			if (!KeptBoxes[R].Intersect(Strip)) continue;
			FVector2D Across(TNumericLimits<double>::Max(), TNumericLimits<double>::Lowest());
			FVector2D Back = Across;
			for (const FVector2D& Corner : Kept[R])
			{
				const FVector2D Offset = Corner - C.Front;
				Across = FVector2D(FMath::Min(Across.X, Offset.Dot(C.Along)), FMath::Max(Across.Y, Offset.Dot(C.Along)));
				Back = FVector2D(FMath::Min(Back.X, Offset.Dot(C.Back)), FMath::Max(Back.Y, Offset.Dot(C.Back)));
			}
			if (Back.Y + ReservedGap > 0.0 && Back.X - ReservedGap < MinFill) Taken.Add(FVector2D(Across.X - ReservedGap, Across.Y + ReservedGap));
		}
		// The widest stretch of the front left, the nearer the slot's middle of two as wide.
		Taken.Sort([](const FVector2D& L, const FVector2D& R) { return L.X < R.X; });
		C.S0 = C.S1 = 0.0;
		double From = -SlotHalf;
		for (int32 I = 0; I <= Taken.Num(); I++)
		{
			const double To = I < Taken.Num() ? FMath::Min(Taken[I].X, SlotHalf) : SlotHalf;
			const bool bWider = To - From > C.S1 - C.S0 + 1e-6;
			const bool bAsWide = FMath::Abs((To - From) - (C.S1 - C.S0)) <= 1e-6 && FMath::Abs(From + To) < FMath::Abs(C.S0 + C.S1);
			if (To > From && (bWider || bAsWide))
			{
				C.S0 = From;
				C.S1 = To;
			}
			if (I < Taken.Num()) From = FMath::Max(From, Taken[I].Y);
			if (From >= SlotHalf) break;
		}
		if (C.S1 - C.S0 < C.MinWidth) return false;

		double Claim[CSCityLayout_NumSlopes];
		double Clearance[CSCityLayout_NumSlopes];
		for (int32 J = 0; J < CSCityLayout_NumSlopes; J++) Claim[J] = Clearance[J] = TNumericLimits<double>::Max();
		for (const FProbe& Probe : Probes)
		{
			for (int32 J = 0; J < CSCityLayout_NumSlopes; J++) Clearance[J] = FMath::Min(Clearance[J], CSCityLayout_ClearLimit(Probe.S, Probe.T, Probe.Radius, CSCityLayout_Slope(J), C.S0, C.S1));
		}
		// The back may come as near a claim as its corner nearest the back.
		for (FCSCityLayout_Small& Claimed : Claims)
		{
			CSCityLayout_Keep(Claimed, -1.0, 0.0, C.S0);
			CSCityLayout_Keep(Claimed, 1.0, 0.0, -C.S1);
			for (const FVector2D& Corner : Claimed)
			{
				for (int32 J = 0; J < CSCityLayout_NumSlopes; J++) Claim[J] = FMath::Min(Claim[J], Corner.Y - CSCityLayout_Slope(J) * Corner.X);
			}
		}
		auto Limit = [&Claim, &Clearance, &Settings](int32 J)
		{
			const double B = CSCityLayout_Slope(J);
			return FMath::Min(Claim[J] - Settings.Gap * 0.5 * FMath::Sqrt(1.0 + B * B), Clearance[J]);
		};
		constexpr int32 Flat = CSCityLayout_NumSlopes / 2;
		C.B = 0.0;
		if (Limit(Flat) >= Settings.FillDepth) C.A = FMath::Min(Depth, Limit(Flat));
		else
		{
			// The most ground; a slant only for a little more of it.
			const double Middle = (C.S0 + C.S1) * 0.5;
			double Best = TNumericLimits<double>::Lowest();
			for (int32 J = 0; J < CSCityLayout_NumSlopes; J++)
			{
				const double B = CSCityLayout_Slope(J);
				const double Deep = FMath::Max(B * C.S0, B * C.S1);
				const double A = FMath::Min(Limit(J), Settings.FillDepth - Deep);
				const double Score = (C.S1 - C.S0) * (A + B * Middle - 0.05 * FMath::Abs(B) * Width);
				if (A + FMath::Min(B * C.S0, B * C.S1) < MinFill || Score <= Best) continue;
				Best = Score;
				C.A = A;
				C.B = B;
			}
			if (Best == TNumericLimits<double>::Lowest()) return false;
		}
		// The limits look at the back and its corners, the stretch at the front: a road passing a side further back, and
		// a reserved plot, are caught here.
		return Shrink(C, C.A, [&](double A)
		{
			const FPolygon Tried = Shape(C, A);
			if (!Clear(Tried)) return true;
			for (const FPolygon& Plot : Kept) if (!CSCityLayout_Apart(Plot, Tried, ReservedGap)) return true;
			return false;
		});
	};

	// Every plot on both sides of every anchor line.
	TArray<FCandidate> Candidates;
	for (const FRoadLine& Anchor : Anchors)
	{
		if (Anchor.Points.Num() < 2) continue;
		const uint64 Group = GroupOf(Anchor);
		const double Inner = Anchor.HalfWidth + SetbackOf(Anchor);
		const double Frontage = Anchor.Frontage > 0.0 ? FMath::Min(Anchor.Frontage, Settings.Frontage) : Settings.Frontage;
		if (Frontage <= Settings.Gap) continue;
		const double SlotMinWidth = FMath::Max(400.0, (Frontage - Settings.Gap) * 0.4);
		TArray<double> Along = { 0.0 };
		for (int32 I = 1; I < Anchor.Points.Num(); I++) Along.Add(Along.Last() + FVector2D::Distance(FVector2D(Anchor.Points[I - 1]), FVector2D(Anchor.Points[I])));
		// With bFill the anchor in whole slots as near its frontage as fits (none when shorter than its least width and a
		// Gap); else one every frontage from its start.
		const double Length = Along.Last();
		const int32 Slots = !bFill ? FMath::FloorToInt32(Length / Frontage) : Length < SlotMinWidth + Settings.Gap ? 0 : FMath::Max(1, FMath::RoundToInt32(Length / Frontage));
		const double Pitch = bFill && Slots > 0 ? Length / Slots : Frontage;
		const double SlotHalf = (Pitch - Settings.Gap) * 0.5;
		int32 Segment = 0;
		for (int32 Slot = 0; Slot < Slots; Slot++)
		{
			const double At = (Slot + 0.5) * Pitch;
			while (Segment + 2 < Anchor.Points.Num() && Along[Segment + 1] < At) Segment++;
			const double Span = FMath::Max(Along[Segment + 1] - Along[Segment], 1e-6);
			const FVector Position = FMath::Lerp(Anchor.Points[Segment], Anchor.Points[Segment + 1], (At - Along[Segment]) / Span);
			const FVector2D Tangent = (FVector2D(Anchor.Points[Segment + 1]) - FVector2D(Anchor.Points[Segment])).GetSafeNormal();
			const FVector2D Normal(-Tangent.Y, Tangent.X);
			for (const double Side : { 1.0, -1.0 })
			{
				FCandidate Candidate;
				FLot& Lot = Candidate.Lot;
				Lot.Id = CSCityLayout_Mix(Anchor.Id ^ CSCityLayout_Mix(uint64(Slot) * 2 + (Side > 0.0 ? 1 : 0)));
				const double Depth = FMath::Lerp(Settings.MinDepth, Settings.MaxDepth, CSCityLayout_Unit(Lot.Id ^ Seed ^ 1));
				// Plaster, stone and brick tones, a little apart from each other.
				static const FLinearColor Tones[] = { FLinearColor(0.82f, 0.76f, 0.66f), FLinearColor(0.70f, 0.70f, 0.68f), FLinearColor(0.66f, 0.42f, 0.33f), FLinearColor(0.88f, 0.84f, 0.74f), FLinearColor(0.56f, 0.60f, 0.64f) };
				const FLinearColor Tone = Tones[CSCityLayout_Mix(Lot.Id ^ Seed ^ 3) % UE_ARRAY_COUNT(Tones)];
				Lot.Color = Tone * float(0.9 + 0.2 * CSCityLayout_Unit(Lot.Id ^ Seed ^ 4));
				Lot.Color.A = 1.0f;
				Candidate.Front = FVector2D(Position) + Normal * (Side * Inner);
				Candidate.Along = Tangent * Side;
				Candidate.Back = Normal * Side;
				Candidate.S0 = -SlotHalf;
				Candidate.S1 = SlotHalf;
				Candidate.MinWidth = SlotMinWidth;
				Candidate.A = Depth;
				if (bFill && !Fit(Candidate, Group, Depth)) continue;
				Finish(Candidate);
				// Without bFill: clear of every road, by half its setback from its edge (a plot's own road, which it faces
				// from the whole setback, cannot be told apart from the others), and of every reserved plot, or none.
				if (!bFill)
				{
					if (!Clear(Lot.Corners)) continue;
					bool bReserved = false;
					for (int32 R = 0; R < Kept.Num() && !bReserved; R++) bReserved = KeptBoxes[R].Intersect(CSCityLayout_Box(Lot.Corners)) && Overlap(Kept[R], Lot.Corners);
					if (bReserved) continue;
				}
				Lot.Base = Position.Z;
				Lot.Height = FMath::Lerp(Settings.MinHeight, Settings.MaxHeight, CSCityLayout_Unit(Lot.Id ^ Seed ^ 2));
				if (!Place(Lot)) continue;
				Candidate.Box = CSCityLayout_Box(Lot.Corners);
				Candidate.Priority = CSCityLayout_Mix(Lot.Id ^ Seed ^ 5);
				Candidates.Add(MoveTemp(Candidate));
			}
		}
	}

	// Among plots that overlap (with bFill: come within a Gap of each other), the one drawn first stands as it is. The
	// others give way to every such plot before them, as that plot is drawn and whether or not it stands: with bFill by
	// coming forward a Gap short of it, else by being left out.
	auto InTheWay = [bFill, RivalGap](const FPolygon& Rival, const FPolygon& Plot) { return bFill ? !CSCityLayout_Apart(Rival, Plot, RivalGap) : Overlap(Rival, Plot); };
	TArray<FBox2D> RivalBoxes;
	for (const FCandidate& Candidate : Candidates) RivalBoxes.Add(Candidate.Box.ExpandBy(bFill ? Settings.Gap : 0.0));
	TMap<FIntPoint, TArray<int32>> Plots;
	for (int32 C = 0; C < Candidates.Num(); C++)
	{
		const FIntPoint Min = CellOf(RivalBoxes[C].Min);
		const FIntPoint Max = CellOf(RivalBoxes[C].Max);
		for (int32 Y = Min.Y; Y <= Max.Y; Y++) for (int32 X = Min.X; X <= Max.X; X++) Plots.FindOrAdd(FIntPoint(X, Y)).Add(C);
	}
	auto First = [&Candidates](int32 A, int32 B) { return Candidates[A].Priority != Candidates[B].Priority ? Candidates[A].Priority > Candidates[B].Priority : Candidates[A].Lot.Id > Candidates[B].Lot.Id; };
	for (int32 C = 0; C < Candidates.Num(); C++)
	{
		const FCandidate& Candidate = Candidates[C];
		TArray<int32> Before;
		const FIntPoint Min = CellOf(Candidate.Box.Min);
		const FIntPoint Max = CellOf(Candidate.Box.Max);
		for (int32 Y = Min.Y; Y <= Max.Y; Y++)
		{
			for (int32 X = Min.X; X <= Max.X; X++)
			{
				for (const int32 Other : Plots.FindRef(FIntPoint(X, Y)))
				{
					if (Other == C || !First(Other, C) || Before.Contains(Other) || !RivalBoxes[Other].Intersect(Candidate.Box) || !InTheWay(Candidates[Other].Lot.Corners, Candidate.Lot.Corners)) continue;
					Before.Add(Other);
				}
			}
		}
		if (Before.Num() == 0)
		{
			Out.Add(Candidate.Lot);
			continue;
		}
		if (!bFill) continue;
		FCandidate Given = Candidate;
		auto Hits = [&](double A)
		{
			const FPolygon Tried = Shape(Given, A);
			for (const int32 Other : Before) if (InTheWay(Candidates[Other].Lot.Corners, Tried)) return true;
			return false;
		};
		if (!Shrink(Given, Given.A, Hits)) continue;
		Finish(Given);
		Out.Add(Given.Lot);
	}
	return Out;
}

double LotStrip(const FLotSettings& Settings)
{
	// With bFill a slot is under one and a half Frontage.
	const double Width = (Settings.bFill ? 1.5 : 1.0) * Settings.Frontage - Settings.Gap;
	return FMath::Sqrt(Width * Width + FMath::Square(CSCityLayout_Deepest(Settings)));
}

// A plot and its centre lie in its strip. A road point bears on the plots whose strip lies within its half width,
// setback, the deepest a plot can be and a Gap (the ground it claims or keeps clear, and whether a plot finds anything
// within FillDepth); a reserved plot on those whose strip comes within half a Gap of it (mitred: twice that); an anchor
// line has its plots' strips within its half width, setback and that depth. A plot gives way only to plots whose strips
// come within a Gap of its own (mitred: four), so whatever bears on a plot bears on those too, a strip further on. The
// extra metre absorbs rounding.
double AnchorReach(const FLotSettings& Settings, double HalfWidth, double Setback)
{
	return HalfWidth + (Setback >= 0.0 ? Setback : Settings.Setback) + CSCityLayout_Deepest(Settings) + LotStrip(Settings) + 4.0 * Settings.Gap + 100.0;
}

double RoadReach(const FLotSettings& Settings, double HalfWidth, double Setback)
{
	return HalfWidth + (Setback >= 0.0 ? Setback : Settings.Setback) + CSCityLayout_Deepest(Settings) + Settings.Gap + 2.0 * LotStrip(Settings) + 4.0 * Settings.Gap + 100.0;
}

double ReservedReach(const FLotSettings& Settings)
{
	return 2.0 * LotStrip(Settings) + 6.0 * Settings.Gap + 100.0;
}

void AppendPrism(FCSGpuMeshCPUData& Out, const FPolygon& Footprint, double Base, double Height, const FLinearColor& Color)
{
	if (Footprint.Num() < 3 || Height <= 0.0) return;
	const FVector4f Tint(Color.R, Color.G, Color.B, 1.0f);
	// Normals face out as cross(B - A, C - A); the indices go in the engine's winding (corners 2 and
	// 3 swapped), as every resident-stream writer does (CSMeshBuild.h).
	auto Face = [&Out, &Tint](const TArray<FVector>& Corners, const FVector& Normal, const FVector& Tangent)
	{
		const int32 First = Out.Positions.Num();
		for (const FVector& Corner : Corners)
		{
			Out.Positions.Add(FVector3f(Corner));
			Out.Normals.Add(FVector3f(Normal));
			Out.Tangents.Add(FVector3f(Tangent));
			Out.TexCoords().Add(FVector2f(float((Corner.X + Corner.Y) / 400.0), float(Corner.Z / 400.0)));
			Out.Colors.Add(Tint);
		}
		for (int32 I = 1; I + 1 < Corners.Num(); I++)
		{
			Out.Indices.Append({ uint32(First), uint32(First + I + 1), uint32(First + I) });
			Out.TriangleMaterialSlots.Add(0);
		}
	};
	const double Top = Base + Height;
	TArray<FVector> Roof;
	for (const FVector2D& P : Footprint) Roof.Add(FVector(P.X, P.Y, Top));
	Face(Roof, FVector::UpVector, FVector(Footprint[1] - Footprint[0], 0.0).GetSafeNormal());
	for (int32 I = 0; I < Footprint.Num(); I++)
	{
		const FVector2D A = Footprint[I];
		const FVector2D B = Footprint[(I + 1) % Footprint.Num()];
		const FVector2D Along = (B - A).GetSafeNormal();
		// Counter-clockwise from above: outward is to the right of each edge.
		Face({ FVector(A, Base), FVector(B, Base), FVector(B, Top), FVector(A, Top) }, FVector(Along.Y, -Along.X, 0.0), FVector(Along, 0.0));
	}
}
}

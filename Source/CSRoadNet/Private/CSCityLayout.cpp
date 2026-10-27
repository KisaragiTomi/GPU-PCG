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

TArray<FLot> LayoutLots(const TArray<FRoadLine>& Anchors, const TArray<FRoadLine>& Roads, const TArray<FPolygon>& Reserved, const FLotSettings& Settings)
{
	TArray<FLot> Out;
	if (Settings.Frontage <= Settings.Gap) return Out;
	const uint64 Seed = CSCityLayout_Mix(uint64(uint32(Settings.Seed)) + 0x5EEDull);

	// The roads as points a metre or less apart, by 10 m cells.
	constexpr double Cell = 1000.0;
	struct FRoadPoint
	{
		FVector2D Point;
		double HalfWidth = 0.0;
	};
	TArray<FRoadPoint> Points;
	TMap<FIntPoint, TArray<int32>> Grid;
	double MaxHalfWidth = 0.0;
	auto CellOf = [](const FVector2D& P) { return FIntPoint(FMath::FloorToInt32(P.X / Cell), FMath::FloorToInt32(P.Y / Cell)); };
	for (const FRoadLine& Road : Roads)
	{
		MaxHalfWidth = FMath::Max(MaxHalfWidth, Road.HalfWidth);
		for (int32 I = 0; I + 1 < Road.Points.Num(); I++)
		{
			const FVector2D A(Road.Points[I]);
			const FVector2D B(Road.Points[I + 1]);
			const int32 Steps = FMath::Max(1, FMath::CeilToInt32(FVector2D::Distance(A, B) / 100.0));
			for (int32 S = 0; S < Steps + (I + 2 == Road.Points.Num() ? 1 : 0); S++)
			{
				const FVector2D P = FMath::Lerp(A, B, double(S) / Steps);
				Grid.FindOrAdd(CellOf(P)).Add(Points.Num());
				Points.Add({ P, Road.HalfWidth });
			}
		}
	}

	// Every plot on both sides of every anchor line.
	struct FCandidate
	{
		FLot Lot;
		FBox2D Box;
		uint64 Priority = 0;
		bool bClear = true;
	};
	TArray<FCandidate> Candidates;
	const double Width = Settings.Frontage - Settings.Gap;
	for (const FRoadLine& Anchor : Anchors)
	{
		if (Anchor.Points.Num() < 2) continue;
		TArray<double> Along = { 0.0 };
		for (int32 I = 1; I < Anchor.Points.Num(); I++) Along.Add(Along.Last() + FVector2D::Distance(FVector2D(Anchor.Points[I - 1]), FVector2D(Anchor.Points[I])));
		const int32 Slots = FMath::FloorToInt32(Along.Last() / Settings.Frontage);
		int32 Segment = 0;
		for (int32 Slot = 0; Slot < Slots; Slot++)
		{
			const double At = (Slot + 0.5) * Settings.Frontage;
			while (Segment + 2 < Anchor.Points.Num() && Along[Segment + 1] < At) Segment++;
			const double Span = FMath::Max(Along[Segment + 1] - Along[Segment], 1e-6);
			const FVector Position = FMath::Lerp(Anchor.Points[Segment], Anchor.Points[Segment + 1], (At - Along[Segment]) / Span);
			const FVector2D Tangent = (FVector2D(Anchor.Points[Segment + 1]) - FVector2D(Anchor.Points[Segment])).GetSafeNormal();
			const FVector2D Normal(-Tangent.Y, Tangent.X);
			for (const double Side : { 1.0, -1.0 })
			{
				FCandidate& Candidate = Candidates.AddDefaulted_GetRef();
				FLot& Lot = Candidate.Lot;
				Lot.Id = CSCityLayout_Mix(Anchor.Id ^ CSCityLayout_Mix(uint64(Slot) * 2 + (Side > 0.0 ? 1 : 0)));
				const double Depth = FMath::Lerp(Settings.MinDepth, Settings.MaxDepth, CSCityLayout_Unit(Lot.Id ^ Seed ^ 1));
				const double Inner = Anchor.HalfWidth + Settings.Setback;
				const FVector2D Base(Position);
				const FVector2D Near = Base + Normal * (Side * Inner);
				const FVector2D Far = Base + Normal * (Side * (Inner + Depth));
				Lot.Corners = ConvexHull({ Near - Tangent * (Width * 0.5), Near + Tangent * (Width * 0.5), Far + Tangent * (Width * 0.5), Far - Tangent * (Width * 0.5) });
				Lot.Base = Position.Z;
				Lot.Height = FMath::Lerp(Settings.MinHeight, Settings.MaxHeight, CSCityLayout_Unit(Lot.Id ^ Seed ^ 2));
				Lot.Centre = (Near + Far) * 0.5;
				// Plaster, stone and brick tones, a little apart from each other.
				static const FLinearColor Tones[] = { FLinearColor(0.82f, 0.76f, 0.66f), FLinearColor(0.70f, 0.70f, 0.68f), FLinearColor(0.66f, 0.42f, 0.33f), FLinearColor(0.88f, 0.84f, 0.74f), FLinearColor(0.56f, 0.60f, 0.64f) };
				const FLinearColor Tone = Tones[CSCityLayout_Mix(Lot.Id ^ Seed ^ 3) % UE_ARRAY_COUNT(Tones)];
				Lot.Color = Tone * float(0.9 + 0.2 * CSCityLayout_Unit(Lot.Id ^ Seed ^ 4));
				Lot.Color.A = 1.0f;
				Candidate.Box = CSCityLayout_Box(Lot.Corners);
				Candidate.Priority = CSCityLayout_Mix(Lot.Id ^ Seed ^ 5);
			}
		}
	}

	// Clear of every road, by half the setback from its edge (a plot's own road, which it faces from
	// the whole setback, cannot be told apart from the others), and of every reserved plot.
	const double Clearance = Settings.Setback * 0.5;
	TArray<FBox2D> ReservedBoxes;
	for (const FPolygon& Plot : Reserved) ReservedBoxes.Add(CSCityLayout_Box(Plot));
	for (FCandidate& Candidate : Candidates)
	{
		const FBox2D Reach = Candidate.Box.ExpandBy(MaxHalfWidth + Clearance);
		const FIntPoint Min = CellOf(Reach.Min);
		const FIntPoint Max = CellOf(Reach.Max);
		for (int32 Y = Min.Y; Y <= Max.Y && Candidate.bClear; Y++)
		{
			for (int32 X = Min.X; X <= Max.X && Candidate.bClear; X++)
			{
				const TArray<int32>* Near = Grid.Find(FIntPoint(X, Y));
				if (!Near) continue;
				for (const int32 Index : *Near)
				{
					const FRoadPoint& Road = Points[Index];
					if (!Candidate.Box.ExpandBy(Road.HalfWidth + Clearance).IsInside(Road.Point) || CSCityLayout_Distance(Candidate.Lot.Corners, Road.Point) >= Road.HalfWidth + Clearance) continue;
					Candidate.bClear = false;
					break;
				}
			}
		}
		for (int32 R = 0; R < Reserved.Num() && Candidate.bClear; R++) if (ReservedBoxes[R].Intersect(Candidate.Box) && Overlap(Reserved[R], Candidate.Lot.Corners)) Candidate.bClear = false;
	}

	// Among clear plots that overlap, the one drawn first stands.
	TMap<FIntPoint, TArray<int32>> Plots;
	for (int32 C = 0; C < Candidates.Num(); C++)
	{
		if (!Candidates[C].bClear) continue;
		const FIntPoint Min = CellOf(Candidates[C].Box.Min);
		const FIntPoint Max = CellOf(Candidates[C].Box.Max);
		for (int32 Y = Min.Y; Y <= Max.Y; Y++) for (int32 X = Min.X; X <= Max.X; X++) Plots.FindOrAdd(FIntPoint(X, Y)).Add(C);
	}
	auto First = [&Candidates](int32 A, int32 B) { return Candidates[A].Priority != Candidates[B].Priority ? Candidates[A].Priority > Candidates[B].Priority : Candidates[A].Lot.Id > Candidates[B].Lot.Id; };
	for (int32 C = 0; C < Candidates.Num(); C++)
	{
		const FCandidate& Candidate = Candidates[C];
		if (!Candidate.bClear) continue;
		bool bStands = true;
		const FIntPoint Min = CellOf(Candidate.Box.Min);
		const FIntPoint Max = CellOf(Candidate.Box.Max);
		for (int32 Y = Min.Y; Y <= Max.Y && bStands; Y++)
		{
			for (int32 X = Min.X; X <= Max.X && bStands; X++)
			{
				for (const int32 Other : Plots.FindRef(FIntPoint(X, Y)))
				{
					if (Other == C || !First(Other, C) || !Candidates[Other].Box.Intersect(Candidate.Box) || !Overlap(Candidates[Other].Lot.Corners, Candidate.Lot.Corners)) continue;
					bStands = false;
					break;
				}
			}
		}
		if (bStands) Out.Add(Candidate.Lot);
	}
	return Out;
}

// A plot is a Frontage - Gap by depth rectangle centred on its centre: it lies within LotRadius of it.
// A road point clears a plot within its half width and half the setback of it; a reserved plot clears
// the plots it overlaps; an anchor line's plots lie within its half width, the setback and the depth
// of it. Each of these also changes the standing of the plots overlapping the ones it touches: two
// more radii. The extra metre absorbs rounding.
double LotRadius(const FLotSettings& Settings)
{
	const double Width = Settings.Frontage - Settings.Gap;
	const double Depth = FMath::Max(Settings.MinDepth, Settings.MaxDepth);
	return 0.5 * FMath::Sqrt(Width * Width + Depth * Depth);
}

double AnchorReach(const FLotSettings& Settings, double HalfWidth)
{
	return HalfWidth + Settings.Setback + FMath::Max(Settings.MinDepth, Settings.MaxDepth) + 2.0 * LotRadius(Settings) + 100.0;
}

double RoadReach(const FLotSettings& Settings, double HalfWidth)
{
	return HalfWidth + Settings.Setback * 0.5 + 3.0 * LotRadius(Settings) + 100.0;
}

double ReservedReach(const FLotSettings& Settings)
{
	return 3.0 * LotRadius(Settings) + 100.0;
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

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadDecalInstances.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadTestUtils.h"
#include "Misc/Paths.h"

using namespace CSRoad;

namespace
{
// The vertices RoadLab writes for a grid item: a strip's two a row, a projected grid's seven.
int32 CSRoadDecalTests_GridVertices(const FGpuDecalItem& Item)
{
	return int32(Item.Rows + 1u) * (EDecalKind(Item.Kind) == EDecalKind::Strip ? 2 : DecalFitColumns + 1);
}

// A triangle's corners in order, whichever way round it is wound.
FIntVector CSRoadDecalTests_Sorted(int32 A, int32 B, int32 C)
{
	if (A > B) Swap(A, B);
	if (B > C) Swap(B, C);
	if (A > B) Swap(A, B);
	return FIntVector(A, B, C);
}

// The frame M_RoadDecal and M_RoadDecalFitted lay a decal in (Scripts/CS2Road/MakeRoadDecalMaterial.py,
// FRAME) at T along its curve, world space: the curve point, right (not normalised) and normal.
void CSRoadDecalTests_Frame(const FTransform& Instance, const float* D, double T, FVector& OutPoint, FVector& OutRight, FVector& OutNormal)
{
	auto Read = [D](int32 At) { return FVector(D[At], D[At + 1], D[At + 2]); };
	const FVector A = Read(DecalData::A);
	const FVector B = Read(DecalData::B);
	const FVector C = Read(DecalData::C);
	const FVector E = Read(DecalData::D);
	const double S = 1.0 - T;
	OutPoint = Instance.GetLocation() + A * (S * S * S) + B * (3.0 * S * S * T) + C * (3.0 * S * T * T) + E * (T * T * T);
	const FVector Derivative = (B - A) * (3.0 * S * S) + (C - B) * (6.0 * S * T) + (E - C) * (3.0 * T * T);
	const FVector Tangent = Derivative.SizeSquared() > 1e-12 ? Derivative.GetUnsafeNormal() : FVector::XAxisVector;
	OutRight = FVector(-Tangent.Y, Tangent.X, 0.0);
	OutNormal = FVector::CrossProduct(Tangent, OutRight);
	OutNormal = OutNormal.SizeSquared() > 1e-12 ? OutNormal.GetUnsafeNormal() : FVector::ZAxisVector;
	if (OutNormal.Z < 0.0) OutNormal = -OutNormal;
	if (D[DecalData::Straight] > 0.5f)
	{
		const FVector Chord = E - A;
		const FVector2D Side(-Chord.Y, Chord.X);
		OutPoint = Instance.GetLocation() + FMath::Lerp(A, E, T);
		OutRight = FVector(Side.SizeSquared() > 1e-12 ? Side.GetSafeNormal() : FVector2D::ZeroVector, 0.0);
		OutNormal = FVector::ZAxisVector;
	}
}

// M_RoadDecal's vertex shader: a strip's point T along and Across (0..1), lifted along the normal.
FVector CSRoadDecalTests_Place(const FTransform& Instance, const float* D, double T, double Across)
{
	FVector Point, Right, Normal;
	CSRoadDecalTests_Frame(Instance, D, T, Point, Right, Normal);
	return Point + Right * ((Across - 0.5) * D[DecalData::Width]) + Normal * D[DecalData::Lift];
}

// M_RoadDecalFitted's vertex shader (HEIGHT, FIT_WPO, FIT_VALID): its grid point (Row, Column of the
// instance's columns) on the surface, lifted straight up (OutPoint), its T along the curve, and
// whether it is on the surface.
bool CSRoadDecalTests_PlaceFitted(const FTransform& Instance, const float* D, int32 Row, int32 Column, FVector& OutPoint, double& OutT)
{
	OutT = FMath::Min(FMath::Lerp(double(D[DecalData::T0]), double(D[DecalData::T1]), double(Row) / DecalStripRows), 1.0);
	FVector Point, Right, Normal;
	CSRoadDecalTests_Frame(Instance, D, OutT, Point, Right, Normal);
	const int32 Columns = int32(D[DecalData::Columns]);
	const int32 K = Row * (Columns + 1) + Column;
	const uint32 Pair = uint32(D[DecalData::Heights + K / 2]);
	const int32 Q = (K & 1) != 0 ? int32(Pair >> 12) : int32(Pair & 4095u);
	OutPoint = Point + Right * ((double(Column) / Columns - 0.5) * D[DecalData::Width]);
	OutPoint.Z = Point.Z + (Q != DecalData::FitHeightOff ? (Q - DecalData::FitHeightZero) * 0.1 : 0.0) + D[DecalData::Lift];
	return Q != DecalData::FitHeightOff;
}

// The surface point M_RoadDecalFitted draws at RoadLab's grid column Column (0..DecalFitColumns) of
// the instance's Row, lift taken off: one of the instance's points (bOutOwn), or between two of them
// where it draws fewer columns. False off the surface.
bool CSRoadDecalTests_FittedSurface(const FTransform& Instance, const float* D, int32 Row, int32 Column, FVector& OutSurface, double& OutT, bool& bOutOwn)
{
	const int32 Columns = int32(D[DecalData::Columns]);
	const int32 Step = DecalFitColumns / FMath::Max(Columns, 1);
	const int32 Low = FMath::Min(Column / Step, Columns - 1);
	const double Fraction = double(Column - Low * Step) / Step;
	FVector A, B;
	const bool bOnA = CSRoadDecalTests_PlaceFitted(Instance, D, Row, Low, A, OutT);
	const bool bOnB = CSRoadDecalTests_PlaceFitted(Instance, D, Row, Low + 1, B, OutT);
	bOutOwn = Column % Step == 0;
	OutSurface = FMath::Lerp(A, B, Fraction) - FVector(0.0, 0.0, D[DecalData::Lift]);
	return Fraction == 0.0 ? bOnA : Fraction == 1.0 ? bOnB : bOnA && bOnB;
}

// Their pixel shaders' UV0 at (T, Across) and, as RoadLab writes them into the vertex, UV1 and the colour.
void CSRoadDecalTests_Shade(const float* D, double T, double Across, FVector2D& OutUV, FVector2D& OutUV2, FVector4& OutColor)
{
	const double S = 1.0 - T;
	OutUV = FVector2D(FMath::Lerp(D[DecalData::UMin], D[DecalData::UMax], Across),
		D[DecalData::VMin] + D[DecalData::VCurve] * S * S * S + D[DecalData::VCurve + 1] * 3.0 * S * S * T + D[DecalData::VCurve + 2] * 3.0 * S * T * T + D[DecalData::VCurve + 3] * T * T * T);
	OutUV2 = FVector2D(T * D[DecalData::Length], S * D[DecalData::Length]);
	OutColor = FVector4(FMath::Lerp(D[DecalData::Weights], D[DecalData::Weights + 1], T), D[DecalData::Connected], D[DecalData::Connected + 1], D[DecalData::Smoothing]);
}

double CSRoadDecalTests_ShadeError(const float* D, double T, double Across, const float* G)
{
	FVector2D UV, UV2;
	FVector4 Color;
	CSRoadDecalTests_Shade(D, T, Across, UV, UV2, Color);
	return FMath::Max(FMath::Max((UV - FVector2D(G[6], G[7])).GetAbsMax(), (UV2 - FVector2D(G[8], G[9])).GetAbsMax()),
		FMath::Max(FMath::Max(FMath::Abs(Color.X - G[10]), FMath::Abs(Color.Y - G[11])), FMath::Max(FMath::Abs(Color.Z - G[12]), FMath::Abs(Color.W - G[13]))));
}

// A golden vertex's position in UE centimetres, the network at the origin.
FVector CSRoadDecalTests_Golden(const float* G) { return FVector(G[0], -G[2], G[1]) * 100.0; }
}

// The decals as the network actor draws them, instances of unit meshes laid along each item's
// curve, against GoldenDump, which runs RoadLab's own RoadDecals (App/RoadDecals.cs compiled against
// a Godot shim) over its projection surfaces. The decal items are built from the golden geometry:
// the synthetic scenarios have lanes whose length is an exact multiple of a strip row, so the GPU
// geometry's last-ulp differences would flip row counts; the GPU builds the road surface the decals
// land on. Compared per block (a road's Add, a connector's AddConnection) and decal surface. A
// strip's vertices are where its instances put their points (position, UV, UV1, colour). A
// junction connector's grid is RoadLab's projected grid point for point: in plan, on the surface
// where RoadLab put it, and a triangle drawn where RoadLab keeps one. A crosswalk's grid points that
// land lie on the triangles RoadLab clipped its footprint to. The surface is the GPU's road mesh, so
// the heights and the kept triangles agree within a budget (CSRoadTest::FBudgetCompare). The
// instances carry the heights DecalFitCS found to the millimetre and draw fewer columns only within
// DecalFitTolerance; every point lies in its instance's box, and the heights come out the same when
// every item takes the way of a candidate list that does not fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadDecalsScenariosTest, "PCGPlugins.CSRoadNet.Decals.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadDecalsScenariosTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const TArray<FString> Ids = CSRoadTest::GoldenScenarioIds();
	if (Ids.Num() == 0)
	{
		AddWarning(TEXT("No golden data in Saved/CSRoadGolden; run Scripts/CS2Road/GoldenDump (skipped)"));
		return true;
	}
	const FString AssetDirectory = FPaths::Combine(CSRoadTest::LabRoot(), TEXT("Assets"), TEXT("CS2"));
	constexpr int32 Stride = DecalFitColumns + 1;
	// The connectors' and crosswalks' surface heights against RoadLab's (m).
	CSRoadTest::FBudgetCompare Surfaces;
	int64 NumItems = 0;
	int64 Instances = 0;
	int64 NumBlocks = 0;
	int64 CountMismatches = 0;
	int64 StripVertices = 0;
	int64 GridVertices = 0;
	int64 GridOff = 0;
	int64 GridTriangles = 0;
	int64 KeepMismatches = 0;
	int64 CrossVertices = 0;
	int64 CrossOff = 0;
	int64 CrossUnmatched = 0;
	int64 CrossEdge = 0;
	// elevated-cross is symmetric to the last bit: its geometry breaks a tie the other way and lands
	// up to 0.9 m from C# (Geometry.Scenarios), and with it the crosswalks clipped against it. Its
	// crosswalks are reported, not budgeted.
	int64 TieCrossVertices = 0;
	int64 TieCrossUnmatched = 0;
	double WorstStrip = 0.0;
	double WorstShade = 0.0;
	double WorstPlan = 0.0;
	double WorstQuantized = 0.0;
	double WorstBetween = 0.0;
	double WorstEdge = 0.0;
	TMap<int32, int64> ColumnsDrawn;
	int32 Unboxed = 0;
	int32 OffMismatches = 0;
	int32 FallbackMismatches = 0;
	int32 Logged = 0;
	FGpuDecalStats Stats;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		if (!CSRoadTest::LoadGolden(Id, Golden, Bin)) continue;
		const TSharedPtr<FJsonObject>* GoldenDecals = nullptr;
		if (!Golden->TryGetObjectField(TEXT("decals"), GoldenDecals) || !(*GoldenDecals)->HasField(TEXT("blocks")))
		{
			AddError(FString::Printf(TEXT("%s: golden data has no decal blocks; rerun GoldenDump"), *Id));
			continue;
		}
		FGraph Graph;
		Graph.bLeftHandTraffic = Golden->GetBoolField(TEXT("leftHand"));
		FString Error;
		FGpuGeometryInput Geometry;
		FGpuGeometryResult GeometryResult;
		FGpuRoadMeshInput Mesh;
		FGpuDecalInput Decals;
		TArray<float> Heights;
		FGpuDecalStats ScenarioStats;
		if (!Graph.Build(Lib.Assets, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Error) || !Geometry.Build(Graph, Lib.Assets, nullptr, Error)
			|| !Mesh.Build(Graph, Lib.Assets, Error) || !RunRefineCountsBlocking(Geometry, Mesh, Error) || !CSRoadTest::GoldenGeometry(Golden, Graph, GeometryResult)
			|| !Decals.Build(Graph, Lib.Assets, GeometryResult, Mesh, AssetDirectory, Error) || !RunDecalFitBlocking(Geometry, Mesh, Decals, Heights, Error, &ScenarioStats))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		Stats.InMemoryRuns += ScenarioStats.InMemoryRuns;
		Stats.PerRunGrids += ScenarioStats.PerRunGrids;
		NumItems += Decals.Items.Num();
		{
			FGpuDecalInput Fallback = Decals;
			Fallback.bForceFallback = true;
			TArray<float> FallbackHeights;
			if (!RunDecalFitBlocking(Geometry, Mesh, Fallback, FallbackHeights, Error)) AddError(FString::Printf(TEXT("%s: fallback: %s"), *Id, *Error));
			else if (FallbackHeights.Num() != Heights.Num() || FMemory::Memcmp(FallbackHeights.GetData(), Heights.GetData(), Heights.Num() * sizeof(float)) != 0) FallbackMismatches++;
		}

		// Each item's instances, as the actor builds them: the item alone makes one batch.
		TArray<FDecalInstances> ItemInstances;
		ItemInstances.SetNum(Decals.Items.Num());
		for (int32 ItemIndex = 0; ItemIndex < Decals.Items.Num(); ItemIndex++)
		{
			const FGpuDecalItem& Item = Decals.Items[ItemIndex];
			FGpuDecalInput One;
			One.Items = { Item };
			One.MaterialSlots = Decals.MaterialSlots;
			TArray<FDecalInstances> Batches;
			BuildDecalInstances(One, FTransform::Identity, Batches, &Heights);
			if (!TestEqual(Id + TEXT(": one batch per item"), Batches.Num(), 1)) continue;
			ItemInstances[ItemIndex] = MoveTemp(Batches[0]);
			const FDecalInstances& Batch = ItemInstances[ItemIndex];
			Instances += Batch.Transforms.Num();
			TestEqual(Id + TEXT(": fitted or not"), Batch.bFitted, EDecalKind(Item.Kind) != EDecalKind::Strip);
			TestEqual(Id + TEXT(": instances per item"), Batch.Transforms.Num(), FMath::DivideAndRoundUp(int32(FMath::Max(Item.Rows, 1u)), DecalStripRows));
		}

		// Golden surfaces by name: where their vertices and indices sit in the bin.
		TMap<FString, TPair<int64, int64>> SurfaceData;
		for (const TSharedPtr<FJsonValue>& Value : (*GoldenDecals)->GetArrayField(TEXT("surfaces")))
		{
			const TSharedPtr<FJsonObject> Surface = Value->AsObject();
			SurfaceData.Add(Surface->GetStringField(TEXT("surface")), TPair<int64, int64>(int64(Surface->GetNumberField(TEXT("vertices"))), int64(Surface->GetNumberField(TEXT("indices")))));
		}
		const TArray<TSharedPtr<FJsonValue>>& Blocks = (*GoldenDecals)->GetArrayField(TEXT("blocks"));
		if (Blocks.Num() != Decals.NumBlocks)
		{
			AddError(FString::Printf(TEXT("%s: %d decal blocks, golden has %d"), *Id, Decals.NumBlocks, Blocks.Num()));
			continue;
		}
		const bool bTie = Id == TEXT("elevated-cross");
		TMap<FString, FIntPoint> Previous;
		for (int32 Block = 0; Block < Blocks.Num(); Block++)
		{
			const TSharedPtr<FJsonObject> Counts = Blocks[Block]->AsObject();
			TSet<FString> SurfacesHere;
			for (const auto& Pair : Counts->Values) SurfacesHere.Add(Pair.Key);
			for (int32 Item = 0; Item < Decals.Items.Num(); Item++) if (Decals.ItemBlocks[Item] == Block) SurfacesHere.Add(Decals.MaterialSlots[Decals.Items[Item].MaterialSlot]);
			for (const FString& Surface : SurfacesHere)
			{
				FIntPoint Now(0, 0);
				const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
				if (Counts->TryGetArrayField(Surface, Pair) && Pair->Num() == 2) Now = FIntPoint(int32((*Pair)[0]->AsNumber()), int32((*Pair)[1]->AsNumber()));
				const FIntPoint Before = Previous.FindRef(Surface);
				Previous.Add(Surface, Now);
				const int32 GoldenVertices = Now.X - Before.X;
				const int32 GoldenIndices = Now.Y - Before.Y;
				// This block's items on this surface, in order. The grids (strips, projected) come before
				// the crosswalk clips: RoadDecals.Add draws the crosswalks last.
				TArray<int32> GridItems;
				TArray<int32> ClipItems;
				int32 GoldenGrid = 0;
				for (int32 Item = 0; Item < Decals.Items.Num(); Item++)
				{
					if (Decals.ItemBlocks[Item] != Block || Decals.MaterialSlots[Decals.Items[Item].MaterialSlot] != Surface) continue;
					if (EDecalKind(Decals.Items[Item].Kind) == EDecalKind::Clipped) ClipItems.Add(Item);
					else
					{
						GridItems.Add(Item);
						GoldenGrid += CSRoadDecalTests_GridVertices(Decals.Items[Item]);
					}
				}
				if (GoldenVertices == 0 && GridItems.Num() + ClipItems.Num() == 0) continue;
				NumBlocks++;
				const FString Label = FString::Printf(TEXT("%s.block%d.%s"), *Id, Block, *Surface);
				const TPair<int64, int64>* Data = SurfaceData.Find(Surface);
				if (!Data || GoldenVertices < GoldenGrid)
				{
					CountMismatches++;
					if (Logged++ < 20) AddInfo(FString::Printf(TEXT("%s: %d grid vertices, golden has %d vertices in all"), *Label, GoldenGrid, GoldenVertices));
					continue;
				}
				auto GoldenVertex = [&Bin, Data, &Before](int32 V, float (&G)[14]) { FMemory::Memcpy(G, Bin.GetData() + Data->Key + (int64(Before.X) + V) * int64(sizeof(G)), sizeof(G)); };
				// RoadLab's grid triangles (this block and surface's vertices, corners in order); its clipped
				// crosswalk vertices are drawn one index per vertex after them.
				const int32 GoldenClipVertices = GoldenVertices - GoldenGrid;
				TSet<FIntVector> GoldenKept;
				for (int32 I = 0; I + 2 < GoldenIndices - GoldenClipVertices; I += 3)
				{
					int32 Corner[3];
					FMemory::Memcpy(Corner, Bin.GetData() + Data->Value + (int64(Before.Y) + I) * int64(sizeof(int32)), sizeof(Corner));
					GoldenKept.Add(CSRoadDecalTests_Sorted(Corner[0] - Before.X, Corner[1] - Before.X, Corner[2] - Before.X));
				}
				TArray<FVector> Clipped;
				for (int32 V = 0; V < GoldenClipVertices; V++)
				{
					float G[14];
					GoldenVertex(GoldenGrid + V, G);
					Clipped.Add(CSRoadDecalTests_Golden(G));
				}
				TArray<double> Mine;
				TArray<double> Theirs;

				// Each fitted instance's points: in its box, and on the surface DecalFitCS found (to the
				// millimetre at its own points, within DecalFitTolerance between them); then Compare
				// with RoadLab at every grid point, its surface point there and T along the curve.
				auto ForFitted = [&](const FGpuDecalItem& Item, const FDecalInstances& Batch, TFunctionRef<void(const float* D, int32 GridRow, int32 Column, bool bOn, const FVector& Surface, double T)> Compare)
				{
					for (int32 Piece = 0; Piece < Batch.Transforms.Num(); Piece++)
					{
						const FTransform& Instance = Batch.Transforms[Piece];
						const FBox Box(Instance.GetLocation(), Instance.GetLocation() + Instance.GetScale3D());
						const float* D = Batch.Data.GetData() + Piece * Batch.Floats;
						const int32 Columns = int32(D[DecalData::Columns]);
						ColumnsDrawn.FindOrAdd(Columns)++;
						for (int32 Row = 0; Row <= DecalStripRows; Row++)
						{
							for (int32 Column = 0; Column <= Columns; Column++)
							{
								FVector Point;
								double T = 0.0;
								CSRoadDecalTests_PlaceFitted(Instance, D, Row, Column, Point, T);
								if (!Box.IsInsideOrOn(Point)) Unboxed++;
							}
							const int32 GridRow = FMath::Min(Piece * DecalStripRows + Row, int32(Item.Rows));
							for (int32 Column = 0; Column < Stride; Column++)
							{
								FVector Surface;
								double T = 0.0;
								bool bOwn = false;
								const bool bOn = CSRoadDecalTests_FittedSurface(Instance, D, Row, Column, Surface, T, bOwn);
								const float Found = Heights[int32(Item.VertexOffset) + GridRow * Stride + Column];
								if (bOn == FMath::IsNaN(Found)) OffMismatches++;
								else if (bOn)
								{
									double& Worst = bOwn ? WorstQuantized : WorstBetween;
									Worst = FMath::Max(Worst, FMath::Abs(Surface.Z - double(Found) * 100.0));
								}
								Compare(D, GridRow, Column, bOn, Surface, T);
							}
						}
					}
				};

				int32 Base = 0;
				for (int32 ItemIndex : GridItems)
				{
					const FGpuDecalItem& Item = Decals.Items[ItemIndex];
					const FDecalInstances& Batch = ItemInstances[ItemIndex];
					const int32 Pieces = Batch.Transforms.Num();
					if (Pieces > 0 && EDecalKind(Item.Kind) == EDecalKind::Strip)
					{
						// Each of RoadLab's vertices: where on the decal it is (t, across), and the instance whose stretch holds it.
						for (uint32 Vertex = 0; Vertex < (Item.Rows + 1u) * 2u; Vertex++)
						{
							float G[14];
							GoldenVertex(Base + int32(Vertex), G);
							const double T = double(Vertex / 2u) / double(Item.Rows);
							const double Across = double(Vertex % 2u);
							const int32 Piece = FMath::Min(int32(T * Pieces), Pieces - 1);
							const FTransform& Instance = Batch.Transforms[Piece];
							const float* D = Batch.Data.GetData() + Piece * Batch.Floats;
							WorstShade = FMath::Max(WorstShade, CSRoadDecalTests_ShadeError(D, T, Across, G));
							const FVector Point = CSRoadDecalTests_Place(Instance, D, T, Across);
							if (!FBox(Instance.GetLocation(), Instance.GetLocation() + Instance.GetScale3D()).IsInsideOrOn(Point)) Unboxed++;
							StripVertices++;
							const double Distance = FVector::Distance(Point, CSRoadDecalTests_Golden(G));
							WorstStrip = FMath::Max(WorstStrip, Distance);
							if (Distance > 0.05 && Logged++ < 5) AddInfo(FString::Printf(TEXT("%s: strip vertex %.3f cm off (t %.3f, across %.0f)"), *Label, Distance, T, Across));
						}
					}
					else if (Pieces > 0)
					{
						// RoadLab's grid vertex at each grid point, lifted along its receiver's normal.
						const double Lift = double(Item.Lift) * 100.0;
						ForFitted(Item, Batch, [&](const float* D, int32 GridRow, int32 Column, bool bOn, const FVector& Surface, double T)
						{
							float G[14];
							GoldenVertex(Base + GridRow * Stride + Column, G);
							WorstShade = FMath::Max(WorstShade, CSRoadDecalTests_ShadeError(D, T, double(Column) / DecalFitColumns, G));
							GridVertices++;
							if (!bOn)
							{
								GridOff++;
								return;
							}
							const FVector GoldSurface = CSRoadDecalTests_Golden(G) - FVector(G[3], -G[5], G[4]) * Lift;
							WorstPlan = FMath::Max(WorstPlan, FVector::Dist2D(Surface, GoldSurface));
							Mine.Add(Surface.Z / 100.0);
							Theirs.Add(GoldSurface.Z / 100.0);
						});
						// Each of RoadLab's grid triangles is kept exactly when all its corners landed.
						for (uint32 Cell = 0; Cell < Item.Rows * uint32(DecalFitColumns); Cell++)
						{
							const int32 Corner = int32((Cell / DecalFitColumns) * Stride + Cell % DecalFitColumns);
							const int32 Corners[2][3] = { { Corner, Corner + Stride, Corner + 1 }, { Corner + 1, Corner + Stride, Corner + Stride + 1 } };
							for (int32 K = 0; K < 2; K++)
							{
								bool bMineKept = true;
								for (const int32 C : Corners[K]) bMineKept &= !FMath::IsNaN(Heights[int32(Item.VertexOffset) + C]);
								const bool bGoldKept = GoldenKept.Contains(CSRoadDecalTests_Sorted(Base + Corners[K][0], Base + Corners[K][1], Base + Corners[K][2]));
								GridTriangles++;
								if (bGoldKept != bMineKept) KeepMismatches++;
							}
						}
					}
					Base += CSRoadDecalTests_GridVertices(Item);
				}

				// A crosswalk's grid points that land: on one of the triangles RoadLab clipped this block's
				// crosswalks on this surface to, lifted straight up.
				for (int32 ItemIndex : ClipItems)
				{
					const FGpuDecalItem& Item = Decals.Items[ItemIndex];
					const double Lift = double(Item.Lift) * 100.0;
					ForFitted(Item, ItemInstances[ItemIndex], [&](const float* D, int32 GridRow, int32 Column, bool bOn, const FVector& Surface, double T)
					{
						(bTie ? TieCrossVertices : CrossVertices)++;
						if (!bOn)
						{
							CrossOff++;
							return;
						}
						// The clipped triangle nearest in plan, and its plane's height there. TryProject takes a
						// point a hair past a receiver's edge that the clip cuts away: one on a clipped edge counts.
						double Nearest = TNumericLimits<double>::Max();
						double PlaneZ = 0.0;
						for (int32 Triangle = 0; Triangle + 2 < Clipped.Num(); Triangle += 3)
						{
							const FVector& P0 = Clipped[Triangle];
							const FVector& P1 = Clipped[Triangle + 1];
							const FVector& P2 = Clipped[Triangle + 2];
							const FVector2D U(P1.X - P0.X, P1.Y - P0.Y);
							const FVector2D V(P2.X - P0.X, P2.Y - P0.Y);
							const FVector2D W(Surface.X - P0.X, Surface.Y - P0.Y);
							const double Det = U ^ V;
							if (FMath::Abs(Det) < 1e-6) continue;
							const double S = (W ^ V) / Det;
							const double R = (U ^ W) / Det;
							double Distance = 0.0;
							if (S < -1e-4 || R < -1e-4 || S + R > 1.0 + 1e-4)
							{
								const FVector2D Corners[3] = { FVector2D(P0.X, P0.Y), FVector2D(P1.X, P1.Y), FVector2D(P2.X, P2.Y) };
								Distance = TNumericLimits<double>::Max();
								for (int32 Edge = 0; Edge < 3; Edge++)
								{
									const FVector2D From = Corners[Edge];
									const FVector2D Along = Corners[(Edge + 1) % 3] - From;
									const double Fraction = FMath::Clamp(FVector2D::DotProduct(FVector2D(Surface.X, Surface.Y) - From, Along) / FMath::Max(Along.SizeSquared(), 1e-12), 0.0, 1.0);
									Distance = FMath::Min(Distance, FVector2D::Distance(FVector2D(Surface.X, Surface.Y), From + Along * Fraction));
								}
							}
							if (Distance >= Nearest) continue;
							Nearest = Distance;
							PlaneZ = P0.Z + S * (P1.Z - P0.Z) + R * (P2.Z - P0.Z) - Lift;
						}
						if (Nearest > 1.0)
						{
							(bTie ? TieCrossUnmatched : CrossUnmatched)++;
							if (!bTie && Logged++ < 5) AddInfo(FString::Printf(TEXT("%s: a crosswalk grid point (t %.3f, column %d) on the surface, %.2f cm from the nearest clipped triangle"), *Label, T, Column, Nearest));
							return;
						}
						if (bTie) return;
						if (Nearest > 0.0) CrossEdge++;
						WorstEdge = FMath::Max(WorstEdge, Nearest);
						Mine.Add(Surface.Z / 100.0);
						Theirs.Add(PlaneZ / 100.0);
					});
				}
				Surfaces.Add(*this, Label + TEXT(".height"), Mine, Theirs);
			}
		}
	}
	FString Drawn;
	ColumnsDrawn.KeySort([](int32 A, int32 B) { return A < B; });
	for (const TPair<int32, int64>& Pair : ColumnsDrawn) Drawn += FString::Printf(TEXT(" %d columns: %lld;"), Pair.Key, Pair.Value);
	AddInfo(FString::Printf(TEXT("%d scenarios: %lld decal items, %lld instances (fitted by columns:%s); %lld block surfaces (%lld with another grid layout); %u runs walked their receivers in memory, %u grids gathered per run"),
		Ids.Num(), NumItems, Instances, *Drawn, NumBlocks, CountMismatches, Stats.InMemoryRuns, Stats.PerRunGrids));
	AddInfo(FString::Printf(TEXT("strips: %lld vertices, worst %.4f cm; connector grids: %lld of RoadLab's points (%lld off the surface), worst %.4f cm in plan, %lld of %lld triangles kept otherwise; crosswalk grids: %lld points (%lld off), %lld on a clipped edge (up to %.3f cm out), %lld farther (elevated-cross: %lld of %lld); UV and colour within %.5f"),
		StripVertices, WorstStrip, GridVertices, GridOff, WorstPlan, KeepMismatches, GridTriangles, CrossVertices, CrossOff, CrossEdge, WorstEdge, CrossUnmatched, TieCrossUnmatched, TieCrossVertices, WorstShade));
	AddInfo(FString::Printf(TEXT("instances against DecalFitCS: %.4f cm at their points, %.4f cm between them, %d landed otherwise; fallback lists differ in %d scenarios"),
		WorstQuantized, WorstBetween, OffMismatches, FallbackMismatches));
	const bool bSurfaces = Surfaces.Report(*this, TEXT("connector and crosswalk heights (m)"), 0.0);
	TestTrue(TEXT("vertices compared"), StripVertices > 0 && GridVertices > 0 && CrossVertices > 0);
	// Vertex counts, dropped cells and clipped triangles hinge on projection decisions at the surface's edge.
	TestTrue(TEXT("block surfaces with RoadLab's grid layout (budget 1%)"), CountMismatches <= FMath::Max<int64>(1, NumBlocks / 100));
	TestTrue(TEXT("strips where RoadLab puts them (0.05 cm)"), WorstStrip < 0.05);
	TestTrue(TEXT("UV, UV1 and colour as RoadLab writes them"), WorstShade < 1e-3);
	TestTrue(TEXT("connector grid points where RoadLab's are in plan (0.01 cm)"), WorstPlan < 0.01);
	TestTrue(TEXT("connector triangles kept as RoadLab keeps them (budget 0.5%)"), KeepMismatches <= FMath::Max<int64>(10, GridTriangles / 200));
	TestTrue(TEXT("crosswalk grid points on RoadLab's clipped surface (budget 0.5%)"), CrossUnmatched <= FMath::Max<int64>(10, CrossVertices / 200));
	TestTrue(TEXT("connector and crosswalk heights within budget"), bSurfaces);
	TestTrue(TEXT("the heights DecalFitCS found, to the millimetre"), WorstQuantized < 0.06);
	TestTrue(TEXT("fewer columns only within DecalFitTolerance"), WorstBetween < DecalFitTolerance + 0.06);
	TestEqual(TEXT("landed where DecalFitCS found a surface"), OffMismatches, 0);
	TestEqual(TEXT("every point inside its instance's box"), Unboxed, 0);
	TestEqual(TEXT("the same heights through the fallback lists"), FallbackMismatches, 0);
	return true;
}

#endif

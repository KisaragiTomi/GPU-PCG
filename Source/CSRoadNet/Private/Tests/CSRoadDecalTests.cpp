#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadDecalInstances.h"
#include "CSRoadGpuDecals.h"
#include "CSRoadTestUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace CSRoad;

namespace
{
uint32 CSRoadDecalTests_Vertices(const FGpuDecalItem& Item)
{
	switch (EDecalKind(Item.Kind))
	{
	case EDecalKind::Strip: return (Item.Rows + 1u) * 2u;
	case EDecalKind::Projected: return (Item.Rows + 1u) * 7u;
	default: return Item.Rows * 3u;
	}
}

uint32 CSRoadDecalTests_Triangles(const FGpuDecalItem& Item)
{
	switch (EDecalKind(Item.Kind))
	{
	case EDecalKind::Strip: return Item.Rows * 2u;
	case EDecalKind::Projected: return Item.Rows * 12u;
	default: return Item.Rows;
	}
}
}

// GPU decals against GoldenDump, which runs RoadLab's own RoadDecals (App/RoadDecals.cs compiled
// against a Godot shim) over its projection surfaces. The decal items are built from the golden
// geometry: the synthetic scenarios have lanes whose length is an exact multiple of a strip row, so
// the GPU geometry's last-ulp differences would flip row counts; the GPU builds the road surface
// the decals are projected onto. Compared per block (a road's Add, a
// connector's AddConnection) and decal surface: the vertex count, every vertex (position, normal,
// uv, uv2, colour) and the triangle list. The GPU reads its receivers back from the road mesh
// streams (snorm8 normals), hence the looser normal agreement.
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
	CSRoadTest::FBudgetCompare Positions;
	CSRoadTest::FBudgetCompare Normals;
	CSRoadTest::FBudgetCompare UVs;
	CSRoadTest::FBudgetCompare Colors;
	Normals.Agreement = 0.02;
	int64 NumBlocks = 0;
	int64 CountMismatches = 0;
	int64 NumTriangles = 0;
	int64 TriangleMismatches = 0;
	int64 NumClipTriangles = 0;
	int64 ClipMismatches = 0;
	// elevated-cross is symmetric to the last bit: its geometry breaks a tie the other way and lands
	// up to 0.9 m from C# (Geometry.Scenarios), and with it the crosswalks clipped against it. Its
	// clips are reported, not budgeted.
	int64 TieClipTriangles = 0;
	int64 TieClipMismatches = 0;
	TArray<double> MineP, GoldenP, MineN, GoldenN, MineUV, GoldenUV, MineC, GoldenC;
	int64 NumItems = 0;
	FGpuDecalStats Stats;
	int32 FallbackMismatches = 0;
	int32 Logged = 0;
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
		TArray<float> Vertices;
		TArray<uint32> Indices;
		FGpuDecalStats ScenarioStats;
		if (!Graph.Build(Lib.Assets, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Error) || !Geometry.Build(Graph, Lib.Assets, nullptr, Error)
			|| !Mesh.Build(Graph, Lib.Assets, Error) || !RunRefineCountsBlocking(Geometry, Mesh, Error) || !CSRoadTest::GoldenGeometry(Golden, Graph, GeometryResult)
			|| !Decals.Build(Graph, Lib.Assets, GeometryResult, Mesh, AssetDirectory, Error) || !RunDecalsBlocking(Geometry, Mesh, Decals, Vertices, Indices, ScenarioStats, Error))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		Stats.InMemoryRuns += ScenarioStats.InMemoryRuns;
		Stats.PerRunGrids += ScenarioStats.PerRunGrids;
		Stats.PerChunkClips += ScenarioStats.PerChunkClips;
		NumItems += Decals.Items.Num();
		// Every item taking the way of a candidate list that does not fit must give the same bits.
		{
			FGpuDecalInput Fallback = Decals;
			Fallback.bForceFallback = true;
			TArray<float> FallbackVertices;
			TArray<uint32> FallbackIndices;
			FGpuDecalStats FallbackStats;
			const bool bRan = RunDecalsBlocking(Geometry, Mesh, Fallback, FallbackVertices, FallbackIndices, FallbackStats, Error);
			const bool bSame = bRan && FallbackVertices.Num() == Vertices.Num() && FallbackIndices.Num() == Indices.Num()
				&& FMemory::Memcmp(FallbackVertices.GetData(), Vertices.GetData(), Vertices.Num() * sizeof(float)) == 0
				&& FMemory::Memcmp(FallbackIndices.GetData(), Indices.GetData(), Indices.Num() * sizeof(uint32)) == 0;
			if (!bSame)
			{
				FallbackMismatches++;
				AddError(FString::Printf(TEXT("%s: the fallback gathers give other decals%s%s"), *Id, bRan ? TEXT("") : TEXT(": "), bRan ? TEXT("") : *Error));
			}
		}
		// Raw output for offline diffing: item count, then per item kind, rows, material slot, vertex
		// offset, index offset, block; then the vertices (14 floats) and the indices.
		{
			TArray<uint8> Dump;
			auto Append = [&Dump](const void* Source, int64 Size) { Dump.Append(static_cast<const uint8*>(Source), Size); };
			const int32 NumDecalItems = Decals.Items.Num();
			Append(&NumDecalItems, sizeof(NumDecalItems));
			for (int32 Item = 0; Item < NumDecalItems; Item++)
			{
				const FGpuDecalItem& Decal = Decals.Items[Item];
				const uint32 Row[6] = { Decal.Kind, Decal.Rows, Decal.MaterialSlot, Decal.VertexOffset, Decal.IndexOffset, uint32(Decals.ItemBlocks[Item]) };
				Append(Row, sizeof(Row));
			}
			Append(Vertices.GetData(), Vertices.Num() * int64(sizeof(float)));
			Append(Indices.GetData(), Indices.Num() * int64(sizeof(uint32)));
			const FString DumpDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CSRoadGpu"), FPaths::GetCleanFilename(CSRoadTest::GoldenDir()));
			FFileHelper::SaveArrayToFile(Dump, *FPaths::Combine(DumpDir, Id + TEXT(".decals.bin")));
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
				int32 GridVertices = 0;
				for (int32 Item = 0; Item < Decals.Items.Num(); Item++)
				{
					if (Decals.ItemBlocks[Item] != Block || Decals.MaterialSlots[Decals.Items[Item].MaterialSlot] != Surface) continue;
					if (EDecalKind(Decals.Items[Item].Kind) == EDecalKind::Clipped) ClipItems.Add(Item);
					else
					{
						GridItems.Add(Item);
						GridVertices += int32(CSRoadDecalTests_Vertices(Decals.Items[Item]));
					}
				}
				if (GoldenVertices == 0 && GridItems.Num() + ClipItems.Num() == 0) continue;
				NumBlocks++;
				const FString Label = FString::Printf(TEXT("%s.block%d.%s"), *Id, Block, *Surface);
				const TPair<int64, int64>* Data = SurfaceData.Find(Surface);
				if (!Data || GoldenVertices < GridVertices)
				{
					CountMismatches++;
					if (Logged++ < 20) AddInfo(FString::Printf(TEXT("%s: %d grid vertices, golden has %d vertices in all"), *Label, GridVertices, GoldenVertices));
					continue;
				}
				auto GoldenVertex = [&Bin, Data, &Before](int32 V, float (&G)[14]) { FMemory::Memcpy(G, Bin.GetData() + Data->Key + (int64(Before.X) + V) * int64(sizeof(G)), sizeof(G)); };
				auto CompareVertex = [&](const float* M, const float (&G)[14])
				{
					for (int32 C = 0; C < 3; C++)
					{
						MineP.Add(M[C]);
						GoldenP.Add(G[C]);
						MineN.Add(M[3 + C]);
						GoldenN.Add(G[3 + C]);
					}
					for (int32 C = 6; C < 10; C++)
					{
						MineUV.Add(M[C]);
						GoldenUV.Add(G[C]);
					}
					for (int32 C = 10; C < 14; C++)
					{
						MineC.Add(M[C]);
						GoldenC.Add(G[C]);
					}
				};

				// Grids: vertex for vertex and triangle for triangle.
				MineP.Reset(); GoldenP.Reset(); MineN.Reset(); GoldenN.Reset(); MineUV.Reset(); GoldenUV.Reset(); MineC.Reset(); GoldenC.Reset();
				TArray<FIntVector> MineTriangles;
				int32 Base = 0;
				for (int32 Item : GridItems)
				{
					const FGpuDecalItem& Decal = Decals.Items[Item];
					const uint32 Count = CSRoadDecalTests_Vertices(Decal);
					for (uint32 V = 0; V < Count; V++)
					{
						float G[14];
						GoldenVertex(Base + int32(V), G);
						CompareVertex(Vertices.GetData() + int64(Decal.VertexOffset + V) * 14, G);
					}
					// Written as corners 0, 2, 1; a dropped grid cell is degenerate.
					for (uint32 T = 0; T < CSRoadDecalTests_Triangles(Decal); T++)
					{
						const uint32* W = Indices.GetData() + Decal.IndexOffset + T * 3u;
						if (W[0] == W[1] && W[1] == W[2]) continue;
						MineTriangles.Add(FIntVector(int32(W[0] - Decal.VertexOffset) + Base, int32(W[2] - Decal.VertexOffset) + Base, int32(W[1] - Decal.VertexOffset) + Base));
					}
					Base += int32(Count);
				}
				// RoadLab's clipped crosswalk vertices are drawn one index per vertex after the grids'.
				const int32 GoldenClipVertices = GoldenVertices - GridVertices;
				TArray<FIntVector> GoldenTriangles;
				for (int32 I = 0; I + 2 < GoldenIndices - GoldenClipVertices; I += 3)
				{
					int32 Corner[3];
					FMemory::Memcpy(Corner, Bin.GetData() + Data->Value + (int64(Before.Y) + I) * int64(sizeof(int32)), sizeof(Corner));
					GoldenTriangles.Add(FIntVector(Corner[0] - Before.X, Corner[1] - Before.X, Corner[2] - Before.X));
				}
				NumTriangles += GoldenTriangles.Num();
				int32 Different = FMath::Abs(MineTriangles.Num() - GoldenTriangles.Num());
				for (int32 I = 0; I < FMath::Min(MineTriangles.Num(), GoldenTriangles.Num()); I++) Different += MineTriangles[I] != GoldenTriangles[I] ? 1 : 0;
				TriangleMismatches += Different;
				if (Different > 0 && Logged++ < 20) AddInfo(FString::Printf(TEXT("%s: %d grid triangles (golden %d), %d differ"), *Label, MineTriangles.Num(), GoldenTriangles.Num(), Different));

				// Clips: the same triangles, but ProjectTriangle visits a receiver at the first 8 m cell it
				// is registered in, and an ulp moves a receiver lying on a cell boundary to the next one,
				// so the order is not compared: each golden triangle is matched to the nearest one.
				TArray<FVector3f> MineCentres;
				TArray<const float*> MineCorners;
				for (int32 Item : ClipItems)
				{
					const FGpuDecalItem& Decal = Decals.Items[Item];
					for (uint32 T = 0; T < Decal.Rows; T++)
					{
						const float* M = Vertices.GetData() + int64(Decal.VertexOffset + T * 3u) * 14;
						MineCorners.Add(M);
						MineCentres.Add((FVector3f(M[0], M[1], M[2]) + FVector3f(M[14], M[15], M[16]) + FVector3f(M[28], M[29], M[30])) / 3.0f);
					}
				}
				TArray<bool> Matched;
				Matched.Init(false, MineCentres.Num());
				int32 Unmatched = FMath::Abs(GoldenClipVertices / 3 - MineCentres.Num());
				NumClipTriangles += GoldenClipVertices / 3;
				for (int32 T = 0; T < GoldenClipVertices / 3; T++)
				{
					float G[3][14];
					for (int32 K = 0; K < 3; K++) GoldenVertex(GridVertices + T * 3 + K, G[K]);
					const FVector3f Centre = (FVector3f(G[0][0], G[0][1], G[0][2]) + FVector3f(G[1][0], G[1][1], G[1][2]) + FVector3f(G[2][0], G[2][1], G[2][2])) / 3.0f;
					int32 Best = INDEX_NONE;
					float BestDistance = 0.01f;
					for (int32 M = 0; M < MineCentres.Num(); M++)
					{
						const float Distance = FVector3f::Dist(MineCentres[M], Centre);
						if (Matched[M] || Distance >= BestDistance) continue;
						Best = M;
						BestDistance = Distance;
					}
					if (Best == INDEX_NONE)
					{
						Unmatched++;
						continue;
					}
					Matched[Best] = true;
					for (int32 K = 0; K < 3; K++) CompareVertex(MineCorners[Best] + K * 14, G[K]);
				}
				const bool bTie = Id == TEXT("elevated-cross");
				(bTie ? TieClipMismatches : ClipMismatches) += Unmatched;
				if (bTie)
				{
					TieClipTriangles += GoldenClipVertices / 3;
					NumClipTriangles -= GoldenClipVertices / 3;
				}
				if (Unmatched > 0 && Logged++ < 20) AddInfo(FString::Printf(TEXT("%s: %d clipped triangles (golden %d), %d unmatched"), *Label, MineCentres.Num(), GoldenClipVertices / 3, Unmatched));
				Positions.Add(*this, Label + TEXT(".position"), MineP, GoldenP);
				Normals.Add(*this, Label + TEXT(".normal"), MineN, GoldenN);
				UVs.Add(*this, Label + TEXT(".uv"), MineUV, GoldenUV);
				Colors.Add(*this, Label + TEXT(".color"), MineC, GoldenC);
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d scenarios: %lld decal items, %lld block surfaces (%lld with another grid layout), %lld grid triangles (%lld differ), %lld clipped triangles (%lld unmatched; elevated-cross %lld of %lld), %u runs walked their receivers in memory, %u grids gathered per run, %u crosswalks per chunk; fallback gathers differ in %d"),
		Ids.Num(), NumItems, NumBlocks, CountMismatches, NumTriangles, TriangleMismatches, NumClipTriangles, ClipMismatches, TieClipMismatches, TieClipTriangles, Stats.InMemoryRuns, Stats.PerRunGrids, Stats.PerChunkClips, FallbackMismatches));
	const bool bPositions = Positions.Report(*this, TEXT("decal positions (m)"), 0.0);
	const bool bNormals = Normals.Report(*this, TEXT("decal normals"), 0.0);
	const bool bUVs = UVs.Report(*this, TEXT("decal uv / uv2"), 0.0);
	const bool bColors = Colors.Report(*this, TEXT("decal colours"), 0.0);
	// Vertex counts and dropped cells hinge on projection decisions at the surface's edge.
	const bool bCounts = CountMismatches <= FMath::Max<int64>(1, NumBlocks / 100);
	const bool bTriangles = TriangleMismatches <= FMath::Max<int64>(10, NumTriangles / 200);
	const bool bClips = ClipMismatches <= FMath::Max<int64>(10, NumClipTriangles / 200);
	if (!bCounts) AddError(FString::Printf(TEXT("%lld of %lld block surfaces have another grid layout (budget 1%%)"), CountMismatches, NumBlocks));
	if (!bTriangles) AddError(FString::Printf(TEXT("%lld of %lld grid triangles differ (budget 0.5%%)"), TriangleMismatches, NumTriangles));
	if (!bClips) AddError(FString::Printf(TEXT("%lld of %lld clipped triangles unmatched (budget 0.5%%)"), ClipMismatches, NumClipTriangles));
	return bPositions && bNormals && bUVs && bColors && bCounts && bTriangles && bClips && FallbackMismatches == 0;
}

namespace
{
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

// Their pixel shaders' UV0 at (T, Across) and, as CSRoadDecal.usf writes them into the vertex, UV1 and the colour.
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

// The decals as the network actor draws them, instances of unit meshes laid along each item's curve,
// against the vertices CSRoadDecal.usf writes for the golden scenarios. A strip's vertices are where
// its instance puts its points (position, UV, UV1, colour). A junction connector's grid is RoadLab's
// projected grid point for point: on the surface where RoadLab put it (its height to the millimetre
// the instance carries), and a triangle drawn exactly where RoadLab keeps one. A crosswalk's grid
// points that land lie on the triangles RoadLab clipped its footprint to. Every point lies in its
// instance's box; the heights come out the same when every item takes the way of a candidate list
// that does not fit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadDecalsInstancesTest, "PCGPlugins.CSRoadNet.Decals.Instances",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadDecalsInstancesTest::RunTest(const FString& Parameters)
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
	int64 Instances = 0;
	int64 StripVertices = 0;
	int64 GridVertices = 0;
	int64 GridOff = 0;
	int64 GridTriangles = 0;
	int64 KeepMismatches = 0;
	int64 CrossVertices = 0;
	int64 CrossOff = 0;
	int64 CrossUnmatched = 0;
	int64 CrossEdge = 0;
	double WorstEdge = 0.0;
	double WorstStrip = 0.0;
	double WorstShade = 0.0;
	double WorstPlan = 0.0;
	double WorstHeight = 0.0;
	double WorstBetween = 0.0;
	TMap<int32, int64> ColumnsDrawn;
	double WorstCross = 0.0;
	int32 Unboxed = 0;
	int32 FallbackMismatches = 0;
	int32 Logged = 0;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		if (!CSRoadTest::LoadGolden(Id, Golden, Bin)) continue;
		FGraph Graph;
		Graph.bLeftHandTraffic = Golden->GetBoolField(TEXT("leftHand"));
		FString Error;
		FGpuGeometryInput Geometry;
		FGpuGeometryResult GeometryResult;
		FGpuRoadMeshInput Mesh;
		FGpuDecalInput Decals;
		FGpuDecalInput Fitted;
		Fitted.bFitted = true;
		TArray<float> Vertices;
		TArray<uint32> Indices;
		TArray<float> Heights;
		FGpuDecalStats Stats;
		if (!Graph.Build(Lib.Assets, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Error) || !Geometry.Build(Graph, Lib.Assets, nullptr, Error)
			|| !Mesh.Build(Graph, Lib.Assets, Error) || !RunRefineCountsBlocking(Geometry, Mesh, Error) || !CSRoadTest::GoldenGeometry(Golden, Graph, GeometryResult)
			|| !Decals.Build(Graph, Lib.Assets, GeometryResult, Mesh, AssetDirectory, Error) || !RunDecalsBlocking(Geometry, Mesh, Decals, Vertices, Indices, Stats, Error)
			|| !Fitted.Build(Graph, Lib.Assets, GeometryResult, Mesh, AssetDirectory, Error) || !RunDecalFitBlocking(Geometry, Mesh, Fitted, Heights, Error))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		if (!TestEqual(Id + TEXT(": the same items fitted"), Fitted.Items.Num(), Decals.Items.Num())) continue;
		{
			FGpuDecalInput Fallback = Fitted;
			Fallback.bForceFallback = true;
			TArray<float> FallbackHeights;
			if (!RunDecalFitBlocking(Geometry, Mesh, Fallback, FallbackHeights, Error)) AddError(FString::Printf(TEXT("%s: fallback: %s"), *Id, *Error));
			else if (FallbackHeights.Num() != Heights.Num() || FMemory::Memcmp(FallbackHeights.GetData(), Heights.GetData(), Heights.Num() * sizeof(float)) != 0) FallbackMismatches++;
		}
		for (int32 ItemIndex = 0; ItemIndex < Fitted.Items.Num(); ItemIndex++)
		{
			const FGpuDecalItem& Item = Fitted.Items[ItemIndex];
			const FGpuDecalItem& Gold = Decals.Items[ItemIndex];
			FGpuDecalInput One;
			One.Items = { Item };
			One.MaterialSlots = Fitted.MaterialSlots;
			// The heights at the item's offset, which the one-item input keeps.
			TArray<FDecalInstances> Batches;
			BuildDecalInstances(One, FTransform::Identity, Batches, &Heights);
			if (!TestEqual(Id + TEXT(": one batch per item"), Batches.Num(), 1)) continue;
			const FDecalInstances& Batch = Batches[0];
			const int32 Pieces = Batch.Transforms.Num();
			Instances += Pieces;
			const EDecalKind Kind = EDecalKind(Item.Kind);
			TestEqual(Id + TEXT(": fitted or not"), Batch.bFitted, Kind != EDecalKind::Strip);
			TestEqual(Id + TEXT(": instances per item"), Pieces, FMath::DivideAndRoundUp(int32(FMath::Max(Item.Rows, 1u)), DecalStripRows));
			auto Box = [&Batch](int32 Piece) { const FTransform& Instance = Batch.Transforms[Piece]; return FBox(Instance.GetLocation(), Instance.GetLocation() + Instance.GetScale3D()); };
			if (Kind == EDecalKind::Strip)
			{
				// Each vertex the GPU wrote: where on the decal it is (t, across), and the instance whose stretch holds it.
				for (uint32 Vertex = 0; Vertex < (Item.Rows + 1u) * 2u; Vertex++)
				{
					const float* G = Vertices.GetData() + int64(Gold.VertexOffset + Vertex) * 14;
					const double T = double(Vertex / 2u) / double(Item.Rows);
					const double Across = double(Vertex % 2u);
					const int32 Piece = FMath::Min(int32(T * Pieces), Pieces - 1);
					const float* Data = Batch.Data.GetData() + Piece * Batch.Floats;
					WorstShade = FMath::Max(WorstShade, CSRoadDecalTests_ShadeError(Data, T, Across, G));
					const FVector Mine = CSRoadDecalTests_Place(Batch.Transforms[Piece], Data, T, Across);
					if (!Box(Piece).IsInsideOrOn(Mine)) Unboxed++;
					StripVertices++;
					const double Distance = FVector::Distance(Mine, CSRoadDecalTests_Golden(G));
					WorstStrip = FMath::Max(WorstStrip, Distance);
					if (Distance > 0.05 && Logged++ < 5) AddInfo(FString::Printf(TEXT("%s: strip vertex %.3f cm off (t %.3f, across %.0f)"), *Id, Distance, T, Across));
				}
				continue;
			}
			const double Lift = double(Item.Lift) * 100.0;
			// The grid point (Row of the item's grid) and whether it landed, as the instances draw it.
			for (int32 Piece = 0; Piece < Pieces; Piece++)
			{
				const float* Data = Batch.Data.GetData() + Piece * Batch.Floats;
				const int32 Columns = int32(Data[DecalData::Columns]);
				const int32 Step = DecalFitColumns / FMath::Max(Columns, 1);
				ColumnsDrawn.FindOrAdd(Columns)++;
				for (int32 Row = 0; Row <= DecalStripRows; Row++)
				{
					const int32 GridRow = FMath::Min(Piece * DecalStripRows + Row, int32(Item.Rows));
					if (Kind == EDecalKind::Projected)
					{
						// RoadLab's grid vertex there (lifted along its receiver's normal): one of the instance's
						// points, or between two of them where it draws fewer columns.
						for (int32 Column = 0; Column < Stride; Column++)
						{
							const int32 Low = FMath::Min(Column / Step, Columns - 1);
							const double Fraction = double(Column - Low * Step) / Step;
							FVector A, B;
							double T = 0.0;
							const bool bOnA = CSRoadDecalTests_PlaceFitted(Batch.Transforms[Piece], Data, Row, Low, A, T);
							const bool bOnB = CSRoadDecalTests_PlaceFitted(Batch.Transforms[Piece], Data, Row, Low + 1, B, T);
							if (!Box(Piece).IsInsideOrOn(A) || !Box(Piece).IsInsideOrOn(B)) Unboxed++;
							const float* G = Vertices.GetData() + int64(Gold.VertexOffset + GridRow * Stride + Column) * 14;
							WorstShade = FMath::Max(WorstShade, CSRoadDecalTests_ShadeError(Data, T, double(Column) / DecalFitColumns, G));
							GridVertices++;
							if (Fraction == 0.0 ? !bOnA : Fraction == 1.0 ? !bOnB : !(bOnA && bOnB))
							{
								GridOff++;
								continue;
							}
							const FVector Surface = FMath::Lerp(A, B, Fraction) - FVector(0.0, 0.0, Lift);
							const FVector GoldSurface = CSRoadDecalTests_Golden(G) - FVector(G[3], -G[5], G[4]) * Lift;
							WorstPlan = FMath::Max(WorstPlan, FVector::Dist2D(Surface, GoldSurface));
							double& Worst = Column % Step == 0 ? WorstHeight : WorstBetween;
							Worst = FMath::Max(Worst, FMath::Abs(Surface.Z - GoldSurface.Z));
						}
						continue;
					}
					for (int32 Column = 0; Column <= Columns; Column++)
					{
						FVector Mine;
						double T = 0.0;
						const bool bOn = CSRoadDecalTests_PlaceFitted(Batch.Transforms[Piece], Data, Row, Column, Mine, T);
						if (!Box(Piece).IsInsideOrOn(Mine)) Unboxed++;
						const FVector Surface = Mine - FVector(0.0, 0.0, Lift);
						// A crosswalk's: on one of the triangles RoadLab clipped it to, lifted straight up.
						CrossVertices++;
						if (!bOn)
						{
							CrossOff++;
							continue;
						}
						// The clipped triangle nearest in plan, and its plane's height there. TryProject takes a point
						// a hair past a receiver's edge that the clip cuts away: one on a clipped edge counts.
						double Nearest = TNumericLimits<double>::Max();
						double Off = 0.0;
						for (uint32 Triangle = 0; Triangle < Gold.Rows; Triangle++)
						{
							const float* G0 = Vertices.GetData() + int64(Gold.VertexOffset + Triangle * 3u) * 14;
							const FVector P0 = CSRoadDecalTests_Golden(G0);
							const FVector P1 = CSRoadDecalTests_Golden(G0 + 14);
							const FVector P2 = CSRoadDecalTests_Golden(G0 + 28);
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
							Off = FMath::Abs(P0.Z + S * (P1.Z - P0.Z) + R * (P2.Z - P0.Z) - Lift - Surface.Z);
						}
						if (Nearest > 1.0)
						{
							CrossUnmatched++;
							if (Logged++ < 5) AddInfo(FString::Printf(TEXT("%s: a crosswalk grid point (t %.3f, column %d) on the surface, %.2f cm from the nearest clipped triangle"), *Id, T, Column, Nearest));
							continue;
						}
						if (Nearest > 0.0) CrossEdge++;
						WorstEdge = FMath::Max(WorstEdge, Nearest);
						WorstCross = FMath::Max(WorstCross, Off);
					}
				}
			}
			if (Kind != EDecalKind::Projected) continue;
			// Each of RoadLab's grid triangles is kept exactly when all its corners landed.
			for (uint32 Cell = 0; Cell < Item.Rows * uint32(DecalFitColumns); Cell++)
			{
				const uint32 Corner = (Cell / DecalFitColumns) * Stride + Cell % DecalFitColumns;
				const uint32 Corners[2][3] = { { Corner, Corner + Stride, Corner + 1 }, { Corner + 1, Corner + Stride, Corner + Stride + 1 } };
				for (int32 K = 0; K < 2; K++)
				{
					const uint32* I = Indices.GetData() + Gold.IndexOffset + (Cell * 2u + K) * 3u;
					const bool bGoldKept = !(I[0] == I[1] && I[1] == I[2]);
					bool bMineKept = true;
					for (const uint32 C : Corners[K]) bMineKept &= !FMath::IsNaN(Heights[int32(Item.VertexOffset + C)]);
					GridTriangles++;
					if (bGoldKept != bMineKept) KeepMismatches++;
				}
			}
		}
	}
	FString Drawn;
	ColumnsDrawn.KeySort([](int32 A, int32 B) { return A < B; });
	for (const TPair<int32, int64>& Pair : ColumnsDrawn) Drawn += FString::Printf(TEXT(" %d columns: %lld;"), Pair.Key, Pair.Value);
	AddInfo(FString::Printf(TEXT("%lld instances (fitted grids by columns:%s); strips: %lld vertices, worst %.4f cm; connector grids: %lld of RoadLab's points (%lld off the surface), worst %.4f cm in plan, %.4f cm in height at the instances' points and %.4f cm between them, %lld of %lld triangles kept otherwise; crosswalk grids: %lld points (%lld off), %lld on a clipped edge (up to %.3f cm out), %lld farther, worst %.4f cm off the plane; UV and colour within %.5f"),
		Instances, *Drawn, StripVertices, WorstStrip, GridVertices, GridOff, WorstPlan, WorstHeight, WorstBetween, KeepMismatches, GridTriangles, CrossVertices, CrossOff, CrossEdge, WorstEdge, CrossUnmatched, WorstCross, WorstShade));
	TestTrue(TEXT("vertices compared"), StripVertices > 0 && GridVertices > 0 && CrossVertices > 0);
	TestTrue(TEXT("strips where CSRoadDecal.usf puts them (0.05 cm)"), WorstStrip < 0.05);
	TestTrue(TEXT("UV, UV1 and colour as CSRoadDecal.usf writes them"), WorstShade < 1e-3);
	TestTrue(TEXT("connector grid points where RoadLab's are (0.01 cm in plan, 0.06 cm in height)"), WorstPlan < 0.01 && WorstHeight < 0.06);
	TestTrue(TEXT("connector grids within DecalFitTolerance of RoadLab's points between theirs"), WorstBetween < DecalFitTolerance + 0.06);
	TestEqual(TEXT("connector triangles kept as RoadLab keeps them"), KeepMismatches, int64(0));
	TestTrue(TEXT("crosswalk grid points on the clipped surface (0.06 cm)"), CrossUnmatched <= CrossVertices / 1000 && WorstCross < 0.06);
	TestEqual(TEXT("every point inside its instance's box"), Unboxed, 0);
	TestEqual(TEXT("the same heights through the fallback lists"), FallbackMismatches, 0);
	return true;
}

#endif

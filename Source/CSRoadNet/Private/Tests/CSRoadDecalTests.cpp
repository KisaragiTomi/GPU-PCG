#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

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

#endif

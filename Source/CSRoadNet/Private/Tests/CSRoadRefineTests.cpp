#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadGpuMesh.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadTestUtils.h"
#include "Misc/Paths.h"

using namespace CSRoad;

namespace
{
static const TCHAR* CSRoadRefineTests_Kinds[3] = { nullptr, TEXT("startInstance"), TEXT("endInstance") };

// Per node instance, the refined triangle count against GoldenDump's RoadMeshRefinement.Refine.
// Each split compares a chord error with 1 cm, so a decision near the threshold flips with the
// geometry's rounding: GoldenDump --perturb=1 (inputs moved by at most one ulp) changes the C#
// reference's own count on 27% of the Palmsland node instances, by 0.14% of all triangles summed
// over instances. The budget is on that sum.
struct FCSRoadRefineTests_Counts
{
	int64 Instances = 0;
	int64 Mismatches = 0;
	int64 AbsoluteDifference = 0;
	int64 Triangles = 0;
	int64 GoldenTriangles = 0;
	int64 Midpoints = 0;
	int64 GoldenVertices = 0;
	int32 Logged = 0;

	void Add(FAutomationTestBase& Test, const FString& Label, const FGpuRoadMeshInput& Mesh, const FGpuMeshInstance& Instance, const TSharedPtr<FJsonObject>& Golden)
	{
		const TSharedPtr<FJsonObject>* Refined = nullptr;
		if (!Golden->TryGetObjectField(TEXT("refined"), Refined)) return;
		const int64 Expected = int64((*Refined)->GetNumberField(TEXT("triangles")));
		Instances++;
		Triangles += Instance.RefinedTriangleCount;
		GoldenTriangles += Expected;
		Midpoints += Instance.RefinedVertexCount;
		GoldenVertices += int64((*Refined)->GetNumberField(TEXT("vertices"))) - int64(Mesh.Meshes[Instance.Mesh].VertexCount);
		if (int64(Instance.RefinedTriangleCount) == Expected) return;
		Mismatches++;
		AbsoluteDifference += FMath::Abs(int64(Instance.RefinedTriangleCount) - Expected);
		if (Logged++ < 20) Test.AddInfo(FString::Printf(TEXT("%s: %u refined triangles, golden has %lld"), *Label, Instance.RefinedTriangleCount, Expected));
	}

	bool Report(FAutomationTestBase& Test, const FString& Name) const
	{
		const double Share = Instances > 0 ? double(Mismatches) / double(Instances) : 0.0;
		const double Difference = GoldenTriangles > 0 ? double(AbsoluteDifference) / double(GoldenTriangles) : 0.0;
		Test.AddInfo(FString::Printf(TEXT("%s: %lld refined node instances, %lld triangles (golden %lld), %lld midpoints (golden %lld new vertices); %lld instances differ (%.2f%%) by %lld triangles in all (%.4f%%)"),
			*Name, Instances, Triangles, GoldenTriangles, Midpoints, GoldenVertices, Mismatches, Share * 100.0, AbsoluteDifference, Difference * 100.0));
		if (Difference <= 0.0025) return true;
		Test.AddError(FString::Printf(TEXT("%s: the refined triangle counts differ by %.4f%% of the golden triangles (budget 0.25%%)"), *Name, Difference * 100.0));
		return false;
	}
};

bool CSRoadRefineTests_Prepare(FAutomationTestBase& Test, const FString& Id, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes, bool bLeftHand,
	const FSavedTerrain* Terrain, TSharedPtr<FJsonObject>& OutGolden, TArray64<uint8>& OutBin, FGraph& Graph, FGpuGeometryInput& Geometry, FGpuRoadMeshInput& Mesh)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!CSRoadTest::LoadGolden(Id, OutGolden, OutBin))
	{
		Test.AddError(FString::Printf(TEXT("%s: golden data missing"), *Id));
		return false;
	}
	Graph.bLeftHandTraffic = bLeftHand;
	FString Error;
	if (!Graph.Build(Lib.Assets, Specs, Nodes, Error) || !Geometry.Build(Graph, Lib.Assets, Terrain, Error) || !Mesh.Build(Graph, Lib.Assets, Error)
		|| !RunRefineCountsBlocking(Geometry, Mesh, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
		return false;
	}
	if (Mesh.NumUnconverged > 0) Test.AddWarning(FString::Printf(TEXT("%s: %u leaves did not converge"), *Id, Mesh.NumUnconverged));
	if (OutGolden->GetArrayField(TEXT("edges")).Num() != Graph.Roads.Num())
	{
		Test.AddError(FString::Printf(TEXT("%s: %d roads, golden has %d"), *Id, Graph.Roads.Num(), OutGolden->GetArrayField(TEXT("edges")).Num()));
		return false;
	}
	return true;
}
}

// Refined and deformed node meshes against GoldenDump: per node instance the triangle count, and
// for instances that agree, every refined triangle's corners (Unity-space position, normal, UV) in
// Refine's triangle order. RoadLab shares a midpoint between the triangles of an edge, the GPU
// creates one per split, so only triangles (not vertex lists) are comparable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadRefineScenariosTest, "PCGPlugins.CSRoadNet.Refine.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadRefineScenariosTest::RunTest(const FString& Parameters)
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
	FCSRoadRefineTests_Counts Counts;
	// As the deform test: every vertex of an instance inherits its geometry, so the symmetric tie in
	// elevated-cross alone puts some corners far off.
	CSRoadTest::FBudgetCompare Positions;
	CSRoadTest::FBudgetCompare Normals;
	CSRoadTest::FBudgetCompare UVs;
	Positions.OutlierBudget = Normals.OutlierBudget = UVs.OutlierBudget = 0.0025;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		CSRoadTest::LoadGolden(Id, Golden, Bin);
		FGraph Graph;
		FGpuGeometryInput Geometry;
		FGpuRoadMeshInput Mesh;
		if (!CSRoadRefineTests_Prepare(*this, Id, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Golden->GetBoolField(TEXT("leftHand")), nullptr, Golden, Bin, Graph, Geometry, Mesh)) continue;
		FString Error;
		TArray<float> Vertices;
		TArray<uint32> Indices;
		if (!RunDeformBlocking(Geometry, Mesh, Vertices, nullptr, &Indices, Error))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
		for (int32 Road = 0; Road < Edges.Num(); Road++)
		{
			for (int32 Kind = 1; Kind <= 2; Kind++)
			{
				const TSharedPtr<FJsonObject> GoldenInstance = Edges[Road]->AsObject()->GetObjectField(CSRoadRefineTests_Kinds[Kind]);
				const FGpuMeshInstance& Instance = Mesh.Instances[Road * 3 + Kind];
				const FString Label = FString::Printf(TEXT("%s.road%d.%s"), *Id, Road, CSRoadRefineTests_Kinds[Kind]);
				const int64 MismatchesBefore = Counts.Mismatches;
				Counts.Add(*this, Label, Mesh, Instance, GoldenInstance);
				const TSharedPtr<FJsonObject>* Refined = nullptr;
				if (Counts.Mismatches != MismatchesBefore || !GoldenInstance->TryGetObjectField(TEXT("refined"), Refined)) continue;
				const int64 Offset = int64((*Refined)->GetNumberField(TEXT("corners")));
				TArray<double> MineP, GoldenP, MineN, GoldenN, MineUV, GoldenUV;
				for (uint32 Triangle = 0; Triangle < Instance.RefinedTriangleCount; Triangle++)
				{
					// The index stream holds corners 0, 2, 1 (UE winding).
					const uint32* Written = Indices.GetData() + Instance.IndexOffset + Triangle * 3u;
					const uint32 Corners[3] = { Written[0], Written[2], Written[1] };
					for (int32 Corner = 0; Corner < 3; Corner++)
					{
						float G[8];
						FMemory::Memcpy(G, Bin.GetData() + Offset + (int64(Triangle) * 3 + Corner) * int64(sizeof(G)), sizeof(G));
						const float* M = Vertices.GetData() + int64(Corners[Corner]) * 8;
						for (int32 C = 0; C < 3; C++)
						{
							MineP.Add(M[C]);
							GoldenP.Add(G[C]);
							MineN.Add(M[3 + C]);
							GoldenN.Add(G[3 + C]);
						}
						MineUV.Append({ M[6], M[7] });
						GoldenUV.Append({ G[6], G[7] });
					}
				}
				Positions.Add(*this, Label + TEXT(".position"), MineP, GoldenP);
				Normals.Add(*this, Label + TEXT(".normal"), MineN, GoldenN);
				UVs.Add(*this, Label + TEXT(".uv"), MineUV, GoldenUV);
			}
		}
	}
	const bool bCounts = Counts.Report(*this, FString::Printf(TEXT("%d scenarios"), Ids.Num()));
	const bool bPositions = Positions.Report(*this, TEXT("refined corner positions (m)"), 0.0);
	const bool bNormals = Normals.Report(*this, TEXT("refined corner normals"), 0.0);
	const bool bUVs = UVs.Report(*this, TEXT("refined corner uvs"), 0.0);
	return bCounts && bPositions && bNormals && bUVs;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadRefinePalmslandTest, "PCGPlugins.CSRoadNet.Refine.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadRefinePalmslandTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	const FString Path = FPaths::Combine(CSRoadTest::LabRoot(), TEXT("Artifacts/public-saves/palmsland-3/network.json"));
	if (!Lib.bLoaded || !FPaths::FileExists(Path) || !FPaths::FileExists(FPaths::Combine(CSRoadTest::GoldenDir(), TEXT("palmsland.json"))))
	{
		AddWarning(TEXT("Palmsland network or golden data missing (skipped)"));
		return true;
	}
	FSavedNetwork Network;
	FString Error;
	if (!Network.Load(Path, &Lib.Assets, true, Error))
	{
		AddError(Error);
		return false;
	}
	TSharedPtr<FJsonObject> Golden;
	TArray64<uint8> Bin;
	FGraph Graph;
	FGpuGeometryInput Geometry;
	FGpuRoadMeshInput Mesh;
	const double Start = FPlatformTime::Seconds();
	if (!CSRoadRefineTests_Prepare(*this, TEXT("palmsland"), Network.Roads, Network.Nodes, Network.bLeftHandTraffic, &Network.Terrain, Golden, Bin, Graph, Geometry, Mesh)) return false;
	const double Milliseconds = (FPlatformTime::Seconds() - Start) * 1000.0;
	FCSRoadRefineTests_Counts Counts;
	const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
	for (int32 Road = 0; Road < Edges.Num(); Road++)
	{
		for (int32 Kind = 1; Kind <= 2; Kind++)
		{
			Counts.Add(*this, FString::Printf(TEXT("palmsland.road%d.%s"), Road, CSRoadRefineTests_Kinds[Kind]), Mesh, Mesh.Instances[Road * 3 + Kind],
				Edges[Road]->AsObject()->GetObjectField(CSRoadRefineTests_Kinds[Kind]));
		}
	}
	AddInfo(FString::Printf(TEXT("palmsland: graph, geometry, composition meshes and the refinement count graph took %.0f ms"), Milliseconds));
	return Counts.Report(*this, FString::Printf(TEXT("palmsland, %d roads"), Network.Roads.Num()));
}

#endif

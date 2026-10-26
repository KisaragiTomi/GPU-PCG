#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadGpuMesh.h"
#include "CSRoadTestUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace CSRoad;

// GPU parameters + deform against GoldenDump's RoadMeshes.Deform output, node instances through
// their junction surface: per vertex of the unrefined composition mesh the Unity-space position,
// normal and UV, for every edge / start / end instance.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadDeformScenariosTest, "PCGPlugins.CSRoadNet.Deform.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadDeformScenariosTest::RunTest(const FString& Parameters)
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
	// Every vertex of an instance inherits that instance's geometry, so the one symmetric tie of the
	// geometry test (elevated-cross) alone puts 0.11% of the positions beyond 10 cm. Normals also
	// differ where a curve collapses to a point: C# gets an exactly zero tangent and the fallback
	// axis, the GPU an ulp-sized one in some direction (zero-area triangles, never visible).
	CSRoadTest::FBudgetCompare Positions;
	CSRoadTest::FBudgetCompare Normals;
	CSRoadTest::FBudgetCompare UVs;
	Positions.OutlierBudget = Normals.OutlierBudget = UVs.OutlierBudget = 0.0025;
	int64 NumVertices = 0;
	static const TCHAR* Kinds[3] = { TEXT("edgeInstance"), TEXT("startInstance"), TEXT("endInstance") };
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		if (!CSRoadTest::LoadGolden(Id, Golden, Bin)) continue;
		FGraph Graph;
		Graph.bLeftHandTraffic = Golden->GetBoolField(TEXT("leftHand"));
		FString Error;
		FGpuGeometryInput Geometry;
		FGpuRoadMeshInput Mesh;
		TArray<float> Vertices;
		if (!Graph.Build(Lib.Assets, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Error) || !Geometry.Build(Graph, Lib.Assets, nullptr, Error)
			|| !Mesh.Build(Graph, Lib.Assets, Error, false) || !RunDeformBlocking(Geometry, Mesh, Vertices, nullptr, nullptr, Error))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		NumVertices += Mesh.NumVertices;
		// Raw output for offline diffing: per road the edge, start and end instance vertices
		// (8 floats each) in golden order.
		{
			TArray<uint8> Dump;
			for (const FGpuMeshInstance& Instance : Mesh.Instances) Dump.Append(reinterpret_cast<const uint8*>(Vertices.GetData() + int64(Instance.VertexOffset) * 8), Mesh.Meshes[Instance.Mesh].VertexCount * 8 * sizeof(float));
			const FString DumpDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CSRoadGpu"), FPaths::GetCleanFilename(CSRoadTest::GoldenDir()));
			FFileHelper::SaveArrayToFile(Dump, *FPaths::Combine(DumpDir, Id + TEXT(".deform.bin")));
		}
		const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
		const TArray<TSharedPtr<FJsonValue>>& Compositions = Golden->GetArrayField(TEXT("compositions"));
		for (int32 Road = 0; Road < Edges.Num() && Road < Graph.Roads.Num(); Road++)
		{
			for (int32 Kind = 0; Kind < 3; Kind++)
			{
				const TSharedPtr<FJsonObject> Instance = Edges[Road]->AsObject()->GetObjectField(Kinds[Kind]);
				const TSharedPtr<FJsonObject> GoldenMesh = Compositions[int32(Instance->GetNumberField(TEXT("composition")))]->AsObject()->GetObjectField(TEXT("mesh"));
				const int32 Count = int32(GoldenMesh->GetNumberField(TEXT("vertexCount")));
				const FGpuMeshInstance& Mine = Mesh.Instances[Road * 3 + Kind];
				const FString Label = FString::Printf(TEXT("%s.road%d.%s"), *Id, Road, Kinds[Kind]);
				if (int32(Mesh.Meshes[Mine.Mesh].VertexCount) != Count)
				{
					AddError(FString::Printf(TEXT("%s: %u vertices, golden has %d"), *Label, Mesh.Meshes[Mine.Mesh].VertexCount, Count));
					continue;
				}
				const int64 Offset = int64(Instance->GetNumberField(TEXT("deformed")));
				TArray<double> MineP, GoldenP, MineN, GoldenN, MineUV, GoldenUV;
				for (int32 V = 0; V < Count; V++)
				{
					float G[8];
					FMemory::Memcpy(G, Bin.GetData() + Offset + int64(V) * int64(sizeof(G)), sizeof(G));
					const float* M = Vertices.GetData() + (int64(Mine.VertexOffset) + V) * 8;
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
				Positions.Add(*this, Label + TEXT(".position"), MineP, GoldenP);
				Normals.Add(*this, Label + TEXT(".normal"), MineN, GoldenN);
				UVs.Add(*this, Label + TEXT(".uv"), MineUV, GoldenUV);
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d scenarios, %lld deformed vertices"), Ids.Num(), NumVertices));
	const bool bPositions = Positions.Report(*this, TEXT("positions (m)"), 0.0);
	const bool bNormals = Normals.Report(*this, TEXT("normals"), 0.0);
	const bool bUVs = UVs.Report(*this, TEXT("uvs"), 0.0);
	return bPositions && bNormals && bUVs;
}

#endif

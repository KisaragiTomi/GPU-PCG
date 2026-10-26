#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadGpuGeometry.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadTestUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

using namespace CSRoad;

namespace
{
template<typename T>
TArray<double> CSRoadGeometryTests_Flatten(const T& Value)
{
	TArray<double> Out;
	CSRoadFlatten(Value, Out);
	return Out;
}

bool CSRoadGeometryTests_Run(FAutomationTestBase& Test, CSRoadTest::FBudgetCompare& Stats, const FString& Id, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes,
	bool bLeftHand, const FSavedTerrain* Terrain, double& OutMilliseconds)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	TSharedPtr<FJsonObject> Golden;
	TArray64<uint8> Bin;
	if (!CSRoadTest::LoadGolden(Id, Golden, Bin))
	{
		Test.AddError(FString::Printf(TEXT("%s: golden data missing"), *Id));
		return false;
	}
	FGraph Graph;
	Graph.bLeftHandTraffic = bLeftHand;
	FString Error;
	FGpuGeometryInput Input;
	FGpuGeometryResult Result;
	if (!Graph.Build(Lib.Assets, Specs, Nodes, Error) || !Input.Build(Graph, Lib.Assets, Terrain, Error) || !RunGeometryBlocking(Input, Result, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
		return false;
	}
	OutMilliseconds += Result.Milliseconds;
	// Raw results for offline diffing (Scripts/CS2Road/diff_gpu_geometry.py): per road edge (58),
	// start (79) and end (79) geometry floats, then per node 9 floats, in golden order.
	{
		TArray<uint8> Dump;
		auto Append = [&Dump](const void* Data, int32 Size) { Dump.Append(static_cast<const uint8*>(Data), Size); };
		for (const FRoadInstance& Road : Graph.Roads)
		{
			Append(&Result.Edges[Road.Entity], sizeof(EdgeGeometry));
			Append(&Result.StartNodes[Road.Entity], sizeof(EdgeNodeGeometry));
			Append(&Result.EndNodes[Road.Entity], sizeof(EdgeNodeGeometry));
		}
		for (int32 Node : Graph.Nodes) Append(&Result.Nodes[Node], sizeof(NodeGeometry));
		const FString DumpDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CSRoadGpu"), FPaths::GetCleanFilename(CSRoadTest::GoldenDir()));
		FFileHelper::SaveArrayToFile(Dump, *FPaths::Combine(DumpDir, Id + TEXT(".bin")));
	}
	const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
	const TArray<TSharedPtr<FJsonValue>>& GoldenNodes = Golden->GetArrayField(TEXT("nodes"));
	if (Graph.Roads.Num() != Edges.Num() || Graph.Nodes.Num() != GoldenNodes.Num())
	{
		Test.AddError(FString::Printf(TEXT("%s: %d roads / %d nodes, golden has %d / %d"), *Id, Graph.Roads.Num(), Graph.Nodes.Num(), Edges.Num(), GoldenNodes.Num()));
		Stats.bShapeMismatch = true;
		return false;
	}
	for (int32 I = 0; I < Edges.Num(); I++)
	{
		const TSharedPtr<FJsonObject> E = Edges[I]->AsObject();
		const int32 Entity = Graph.Roads[I].Entity;
		const FString Label = FString::Printf(TEXT("%s.road%d"), *Id, I);
		Stats.Add(Test, Label + TEXT(".edgeGeometry"), CSRoadGeometryTests_Flatten(Result.Edges[Entity]), CSRoadTest::Numbers(E, TEXT("edgeGeometry")));
		Stats.Add(Test, Label + TEXT(".startGeometry"), CSRoadGeometryTests_Flatten(Result.StartNodes[Entity]), CSRoadTest::Numbers(E, TEXT("startGeometry")));
		Stats.Add(Test, Label + TEXT(".endGeometry"), CSRoadGeometryTests_Flatten(Result.EndNodes[Entity]), CSRoadTest::Numbers(E, TEXT("endGeometry")));
	}
	for (int32 I = 0; I < GoldenNodes.Num(); I++)
	{
		const TSharedPtr<FJsonObject> N = GoldenNodes[I]->AsObject();
		Stats.Add(Test, FString::Printf(TEXT("%s.node%d.geometry"), *Id, I), CSRoadGeometryTests_Flatten(Result.Nodes[Graph.Nodes[I]]), CSRoadTest::Numbers(N, TEXT("geometry")));
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadGeometryScenariosTest, "PCGPlugins.CSRoadNet.Geometry.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadGeometryScenariosTest::RunTest(const FString& Parameters)
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
	CSRoadTest::FBudgetCompare Stats;
	double Milliseconds = 0.0;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		CSRoadTest::LoadGolden(Id, Golden, Bin);
		CSRoadGeometryTests_Run(*this, Stats, Id, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Golden->GetBoolField(TEXT("leftHand")), nullptr, Milliseconds);
	}
	return Stats.Report(*this, FString::Printf(TEXT("%d scenarios"), Ids.Num()), Milliseconds);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadGeometryPalmslandTest, "PCGPlugins.CSRoadNet.Geometry.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadGeometryPalmslandTest::RunTest(const FString& Parameters)
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
	CSRoadTest::FBudgetCompare Stats;
	double Milliseconds = 0.0;
	CSRoadGeometryTests_Run(*this, Stats, TEXT("palmsland"), Network.Roads, Network.Nodes, Network.bLeftHandTraffic, &Network.Terrain, Milliseconds);
	return Stats.Report(*this, FString::Printf(TEXT("palmsland, %d roads"), Network.Roads.Num()), Milliseconds);
}

#endif

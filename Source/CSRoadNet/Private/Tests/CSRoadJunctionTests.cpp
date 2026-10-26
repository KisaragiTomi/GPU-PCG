#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadGpuMesh.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadTestUtils.h"
#include "Misc/Paths.h"

using namespace CSRoad;

namespace
{
// Seam partners of every node instance side against GoldenDump's RoadJunctionSurface.Create
// (startInstance / endInstance "junction": partner slot * 2 + side, or -1).
struct FCSRoadJunctionTests_Stats
{
	int64 Sides = 0;
	int64 Matched = 0;
	int64 Mismatches = 0;
	int32 Logged = 0;
};

bool CSRoadJunctionTests_Run(FAutomationTestBase& Test, FCSRoadJunctionTests_Stats& Stats, const FString& Id, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes,
	bool bLeftHand, const FSavedTerrain* Terrain)
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
	FGpuGeometryInput Geometry;
	FGpuRoadMeshInput Mesh;
	TArray<uint32> Partners;
	if (!Graph.Build(Lib.Assets, Specs, Nodes, Error) || !Geometry.Build(Graph, Lib.Assets, Terrain, Error) || !Mesh.Build(Graph, Lib.Assets, Error)
		|| !RunJunctionBlocking(Geometry, Mesh, Partners, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
	if (Edges.Num() != Graph.Roads.Num())
	{
		Test.AddError(FString::Printf(TEXT("%s: %d roads, golden has %d"), *Id, Graph.Roads.Num(), Edges.Num()));
		return false;
	}
	static const TCHAR* Kinds[3] = { nullptr, TEXT("startInstance"), TEXT("endInstance") };
	for (int32 Road = 0; Road < Edges.Num(); Road++)
	{
		for (int32 Kind = 1; Kind <= 2; Kind++)
		{
			const TArray<double> GoldenSides = CSRoadTest::Numbers(Edges[Road]->AsObject()->GetObjectField(Kinds[Kind]), TEXT("junction"));
			for (int32 Side = 0; Side < 2 && Side < GoldenSides.Num(); Side++)
			{
				// Create's last pass: only mutual partners survive.
				const uint32 Self = uint32((Road * 3 + Kind) * 2 + Side);
				const uint32 Code = Partners[Self];
				const int64 Mine = Code != 0 && Partners[Code - 1] == Self + 1 ? int64(Code - 1) : -1;
				const int64 Expected = int64(GoldenSides[Side]);
				Stats.Sides++;
				if (Expected >= 0) Stats.Matched++;
				if (Mine == Expected) continue;
				Stats.Mismatches++;
				if (Stats.Logged++ < 20) Test.AddInfo(FString::Printf(TEXT("%s.road%d.%s side %d: partner %lld, golden %lld"), *Id, Road, Kinds[Kind], Side, Mine, Expected));
			}
		}
	}
	return true;
}

// The matching thresholds (2 cm collinear / height, 98% overlap, the fold check) sit on GPU
// geometry that agrees with C# to 1 cm only in budget, so a few sides near a threshold may flip.
bool CSRoadJunctionTests_Report(FAutomationTestBase& Test, const FCSRoadJunctionTests_Stats& Stats, const FString& Name)
{
	const double Share = Stats.Sides > 0 ? double(Stats.Mismatches) / double(Stats.Sides) : 0.0;
	Test.AddInfo(FString::Printf(TEXT("%s: %lld sides, %lld with a seam partner, %lld differ (%.3f%%)"), *Name, Stats.Sides, Stats.Matched, Stats.Mismatches, Share * 100.0));
	if (Share <= 0.002) return true;
	Test.AddError(FString::Printf(TEXT("%s: %.3f%% of the junction sides differ from the golden data (budget 0.2%%)"), *Name, Share * 100.0));
	return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadJunctionScenariosTest, "PCGPlugins.CSRoadNet.Junction.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadJunctionScenariosTest::RunTest(const FString& Parameters)
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
	FCSRoadJunctionTests_Stats Stats;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		CSRoadTest::LoadGolden(Id, Golden, Bin);
		CSRoadJunctionTests_Run(*this, Stats, Id, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Golden->GetBoolField(TEXT("leftHand")), nullptr);
	}
	return CSRoadJunctionTests_Report(*this, Stats, FString::Printf(TEXT("%d scenarios"), Ids.Num()));
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadJunctionPalmslandTest, "PCGPlugins.CSRoadNet.Junction.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadJunctionPalmslandTest::RunTest(const FString& Parameters)
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
	FCSRoadJunctionTests_Stats Stats;
	CSRoadJunctionTests_Run(*this, Stats, TEXT("palmsland"), Network.Roads, Network.Nodes, Network.bLeftHandTraffic, &Network.Terrain);
	return CSRoadJunctionTests_Report(*this, Stats, FString::Printf(TEXT("palmsland, %d roads"), Network.Roads.Num()));
}

#endif

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadDecals.h"
#include "CSRoadTestUtils.h"

using namespace CSRoad;

namespace
{
TArray<double> CSRoadLaneTests_Curve(const Bezier4x3& Curve)
{
	TArray<double> Out;
	CSRoadFlatten(Curve, Out);
	return Out;
}
}

// The CPU lane graph and crosswalk curves against GoldenDump's RoadLanes / RoadCrosswalks, built from
// the golden geometry so the C++ port is compared alone: everything should agree to the bit.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadLanesScenariosTest, "PCGPlugins.CSRoadNet.Lanes.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadLanesScenariosTest::RunTest(const FString& Parameters)
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
	CSRoadTest::FCompare Compare(*this);
	int64 NumLanes = 0;
	int64 NumConnectors = 0;
	int64 NumCrossings = 0;
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		if (!CSRoadTest::LoadGolden(Id, Golden, Bin)) continue;
		const TSharedPtr<FJsonObject>* Decals = nullptr;
		if (!Golden->TryGetObjectField(TEXT("decals"), Decals))
		{
			AddError(FString::Printf(TEXT("%s: golden data has no decals; rerun GoldenDump"), *Id));
			continue;
		}
		FGraph Graph;
		Graph.bLeftHandTraffic = Golden->GetBoolField(TEXT("leftHand"));
		FString Error;
		if (!Graph.Build(Lib.Assets, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Error))
		{
			AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
			continue;
		}
		FGpuGeometryResult Geometry;
		if (!CSRoadTest::GoldenGeometry(Golden, Graph, Geometry))
		{
			AddError(FString::Printf(TEXT("%s: golden roads or geometry do not fit the graph"), *Id));
			continue;
		}
		TMap<int32, int32> NodeIndex;
		for (int32 I = 0; I < Graph.Nodes.Num(); I++) NodeIndex.Add(Graph.Nodes[I], I);

		FRoadLanes Lanes;
		Lanes.Build(Graph, Lib.Assets, Geometry);
		const TArray<TSharedPtr<FJsonValue>>& GoldenLanes = (*Decals)->GetArrayField(TEXT("lanes"));
		if (!Compare.Equal(Id + TEXT(".lanes"), Lanes.Lanes.Num(), GoldenLanes.Num())) continue;
		for (int32 I = 0; I < GoldenLanes.Num(); I++)
		{
			const TSharedPtr<FJsonObject> G = GoldenLanes[I]->AsObject();
			const FRoadLane& Lane = Lanes.Lanes[I];
			const FString Label = FString::Printf(TEXT("%s.lane%d"), *Id, I);
			const bool bConnector = G->GetBoolField(TEXT("connector"));
			NumLanes++;
			NumConnectors += bConnector ? 1 : 0;
			Compare.Equal(Label + TEXT(".road"), Lane.Road, int64(G->GetNumberField(TEXT("road"))));
			Compare.Equal(Label + TEXT(".definition"), Lane.Definition.m_Index, int64(G->GetNumberField(TEXT("definition"))));
			Compare.Equal(Label + TEXT(".connector"), Lane.bConnector, bConnector);
			Compare.Equal(Label + TEXT(".reverse"), Lane.bReverse, G->GetBoolField(TEXT("reverse")));
			Compare.Equal(Label + TEXT(".start"), NodeIndex.FindRef(Lane.StartNode), int64(G->GetNumberField(TEXT("start"))));
			Compare.Equal(Label + TEXT(".end"), NodeIndex.FindRef(Lane.EndNode), int64(G->GetNumberField(TEXT("end"))));
			Compare.Equal(Label + TEXT(".from"), Lane.From, int64(G->GetNumberField(TEXT("from"))));
			Compare.Equal(Label + TEXT(".to"), Lane.To, int64(G->GetNumberField(TEXT("to"))));
			const TArray<TSharedPtr<FJsonValue>>& Curves = G->GetArrayField(TEXT("curves"));
			if (!Compare.Equal(Label + TEXT(".curves"), Lane.Curves.Num(), Curves.Num())) continue;
			for (int32 C = 0; C < Curves.Num(); C++) Compare.Arrays(FString::Printf(TEXT("%s.curve%d"), *Label, C), CSRoadLaneTests_Curve(Lane.Curves[C]), CSRoadTest::Numbers(Curves[C]->AsArray()), 0.0);
			const TArray<double> Counts = CSRoadTest::Numbers(G, bConnector ? TEXT("shared") : TEXT("connected"));
			if (Counts.Num() != 2) continue;
			Compare.Equal(Label + TEXT(".count0"), bConnector ? Lane.SharedStartCount : Lane.ConnectedStartCount, int64(Counts[0]));
			Compare.Equal(Label + TEXT(".count1"), bConnector ? Lane.SharedEndCount : Lane.ConnectedEndCount, int64(Counts[1]));
		}

		const TArray<TSharedPtr<FJsonValue>>& GoldenCrosswalks = (*Decals)->GetArrayField(TEXT("crosswalks"));
		TArray<FCrossing> Crossings;
		for (int32 Road = 0; Road < Graph.Roads.Num() && Road < GoldenCrosswalks.Num(); Road++)
		{
			const TArray<TSharedPtr<FJsonValue>>& Ends = GoldenCrosswalks[Road]->AsArray();
			for (int32 End = 0; End < 2 && End < Ends.Num(); End++)
			{
				Crossings.Reset();
				CreateCrosswalks(Graph, Lib.Assets, Geometry, Road, End == 0, Crossings);
				const TArray<TSharedPtr<FJsonValue>>& Expected = Ends[End]->AsArray();
				const FString Label = FString::Printf(TEXT("%s.road%d.%s"), *Id, Road, End == 0 ? TEXT("start") : TEXT("end"));
				if (!Compare.Equal(Label + TEXT(".crosswalks"), Crossings.Num(), Expected.Num())) continue;
				for (int32 C = 0; C < Crossings.Num(); C++)
				{
					const TSharedPtr<FJsonObject> G = Expected[C]->AsObject();
					NumCrossings++;
					Compare.Equal(FString::Printf(TEXT("%s.crossing%d.lane"), *Label, C), Crossings[C].Lane, int64(G->GetNumberField(TEXT("lane"))));
					Compare.Arrays(FString::Printf(TEXT("%s.crossing%d.curve"), *Label, C), CSRoadLaneTests_Curve(Crossings[C].Curve), CSRoadTest::Numbers(G, TEXT("curve")), 0.0);
					Compare.Arrays(FString::Printf(TEXT("%s.crossing%d.marking"), *Label, C), CSRoadLaneTests_Curve(Crossings[C].MarkingCurve), CSRoadTest::Numbers(G, TEXT("marking")), 0.0);
				}
			}
		}
	}
	AddInfo(FString::Printf(TEXT("%d scenarios: %lld lanes (%lld connectors), %lld crossings, %d values, %d mismatches, max error %g"),
		Ids.Num(), NumLanes, NumConnectors, NumCrossings, Compare.Values, Compare.Mismatches, Compare.MaxError));
	return Compare.Mismatches == 0;
}

#endif

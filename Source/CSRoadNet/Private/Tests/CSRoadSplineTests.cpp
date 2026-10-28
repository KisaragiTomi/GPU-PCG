#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMeshRenderComponent.h"
#include "CSRoadGpuGeometry.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTestUtils.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "Tests/AutomationEditorCommon.h"

using namespace CSRoad;

namespace
{
// A spline through the points with Catmull-Rom tangents (a two-point one is a straight line).
FSplineRoad CSRoadSplineTests_Road(const TArray<FVector3d>& Points, bool bAdjust = true)
{
	FSplineRoad Road;
	Road.Name = TEXT("test road");
	Road.Prefab = TEXT("Small Road");
	Road.bAdjust = bAdjust;
	Road.Points = Points;
	for (int32 I = 0; I < Points.Num(); I++)
	{
		const bool bInner = I > 0 && I + 1 < Points.Num();
		const FVector3d Tangent = (Points[FMath::Min(I + 1, Points.Num() - 1)] - Points[FMath::Max(I - 1, 0)]) / (bInner ? 2.0 : 1.0);
		Road.Arrive.Add(Tangent);
		Road.Leave.Add(Tangent);
	}
	return Road;
}

FSplineNetworkSettings CSRoadSplineTests_JunctionsOnly()
{
	FSplineNetworkSettings Settings;
	Settings.MaxEdgeLength = 0.0;
	Settings.MaxEdgeTurn = 0.0;
	Settings.MinEdgeLength = 0.0;
	return Settings;
}

struct FCSRoadSplineTests_Network
{
	TArray<FSavedNode> Nodes;
	TArray<FRoadSpec> Roads;
	FSplineNetworkStats Stats;

	int32 Degree(int32 Node) const
	{
		int32 Count = 0;
		for (const FRoadSpec& Road : Roads) Count += (Road.SourceStart == Node ? 1 : 0) + (Road.SourceEnd == Node ? 1 : 0);
		return Count;
	}

	int32 NodeAt(const FVector3d& Position, double Tolerance = 1e-3) const
	{
		for (int32 N = 0; N < Nodes.Num(); N++) if (FVector3d::Dist(FVector3d(Nodes[N].Position.x, Nodes[N].Position.y, Nodes[N].Position.z), Position) <= Tolerance) return N;
		return INDEX_NONE;
	}

	// Every edge starts and ends exactly on its node.
	bool EdgesOnNodes() const
	{
		for (const FRoadSpec& Road : Roads) if (!math::all(Road.Curve.a == Nodes[Road.SourceStart].Position) || !math::all(Road.Curve.d == Nodes[Road.SourceEnd].Position)) return false;
		return true;
	}
};

bool CSRoadSplineTests_Build(FAutomationTestBase& Test, const FString& Case, const TArray<FSplineRoad>& Splines, const FSplineNetworkSettings& Settings, FCSRoadSplineTests_Network& Out)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	FString Error;
	if (!BuildSplineNetwork(Lib.Assets, Splines, Settings, nullptr, Out.Nodes, Out.Roads, Out.Stats, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Case, *Error));
		return false;
	}
	FGraph Graph;
	if (!Graph.Build(Lib.Assets, Out.Roads, Out.Nodes, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: the graph does not build: %s"), *Case, *Error));
		return false;
	}
	return true;
}

FString CSRoadSplineTests_Key(const Bezier4x3& Curve) { return FString::FromBlob(reinterpret_cast<const uint8*>(&Curve), sizeof(Curve)); }

bool CSRoadSplineTests_Same(const float2& A, const float2& B) { return A.x == B.x && A.y == B.y; }
bool CSRoadSplineTests_Same(const CompositionFlags& A, const CompositionFlags& B) { return A.m_General == B.m_General && A.m_Left == B.m_Left && A.m_Right == B.m_Right; }

template<typename T>
TArray<double> CSRoadSplineTests_Flatten(const T& Value)
{
	TArray<double> Out;
	CSRoadFlatten(Value, Out);
	return Out;
}

// A graph and its GPU geometry, with each spec's edge entity.
struct FCSRoadSplineTests_Geometry
{
	FGraph Graph;
	FGpuGeometryInput Input;
	FGpuGeometryResult Result;
	TArray<int32> EntityOfSpec;

	bool Build(FAssets& Assets, const TArray<FRoadSpec>& Specs, const TArray<FSavedNode>& Nodes, bool bLeftHand, FString& Error)
	{
		Graph.bLeftHandTraffic = bLeftHand;
		if (!Graph.Build(Assets, Specs, Nodes, Error) || !Input.Build(Graph, Assets, nullptr, Error) || !RunGeometryBlocking(Input, Result, Error)) return false;
		EntityOfSpec.Init(0, Specs.Num());
		for (const FRoadInstance& Road : Graph.Roads) EntityOfSpec[Road.SpecIndex] = Road.Entity;
		return true;
	}
};

// How far one build's GPU geometry lies from another's: per road its edge, both ends and both nodes, as
// the golden tests count values.
struct FCSRoadSplineTests_Spread
{
	CSRoadTest::FBudgetCompare Stats;
	int32 Compositions = 0;
	// Per road (the other build's spec) its largest difference.
	TArray<TPair<double, int32>> Roads;
};

// Mine's spec S is Theirs' spec Match[S] (INDEX_NONE: none).
void CSRoadSplineTests_Compare(FAutomationTestBase& Test, const FCSRoadSplineTests_Geometry& Mine, const FCSRoadSplineTests_Geometry& Theirs, const TArray<int32>& Match, FCSRoadSplineTests_Spread& Out)
{
	for (int32 S = 0; S < Match.Num(); S++)
	{
		const int32 T = Match[S];
		if (T == INDEX_NONE) continue;
		const FRoadInstance& RoadA = Mine.Graph.Roads[S];
		const FRoadInstance& RoadB = Theirs.Graph.Roads[T];
		if (RoadA.EdgeComposition != RoadB.EdgeComposition || RoadA.StartComposition != RoadB.StartComposition || RoadA.EndComposition != RoadB.EndComposition) Out.Compositions++;
		const int32 A = Mine.EntityOfSpec[S];
		const int32 B = Theirs.EntityOfSpec[T];
		double Worst = 0.0;
		auto Add = [&](const TCHAR* Part, const TArray<double>& X, const TArray<double>& Y)
		{
			Out.Stats.Add(Test, FString::Printf(TEXT("road%d.%s"), T, Part), X, Y);
			for (int32 I = 0; I < FMath::Min(X.Num(), Y.Num()); I++) Worst = FMath::Max(Worst, FMath::Abs(X[I] - Y[I]));
		};
		Add(TEXT("edge"), CSRoadSplineTests_Flatten(Mine.Result.Edges[A]), CSRoadSplineTests_Flatten(Theirs.Result.Edges[B]));
		Add(TEXT("start"), CSRoadSplineTests_Flatten(Mine.Result.StartNodes[A]), CSRoadSplineTests_Flatten(Theirs.Result.StartNodes[B]));
		Add(TEXT("end"), CSRoadSplineTests_Flatten(Mine.Result.EndNodes[A]), CSRoadSplineTests_Flatten(Theirs.Result.EndNodes[B]));
		Add(TEXT("startNode"), CSRoadSplineTests_Flatten(Mine.Result.Nodes[Mine.Graph.EdgeData[A].m_Start]), CSRoadSplineTests_Flatten(Theirs.Result.Nodes[Theirs.Graph.EdgeData[B].m_Start]));
		Add(TEXT("endNode"), CSRoadSplineTests_Flatten(Mine.Result.Nodes[Mine.Graph.EdgeData[A].m_End]), CSRoadSplineTests_Flatten(Theirs.Result.Nodes[Theirs.Graph.EdgeData[B].m_End]));
		Out.Roads.Add({ Worst, T });
	}
}

void CSRoadSplineTests_Report(FAutomationTestBase& Test, const FString& Name, FCSRoadSplineTests_Spread& Spread, const TArray<FRoadSpec>& Roads)
{
	const CSRoadTest::FBudgetCompare& S = Spread.Stats;
	Test.AddInfo(FString::Printf(TEXT("%s: %lld values, %lld beyond %g, %lld beyond %g, max %.4g at %s; %d roads with other compositions"), *Name, S.Values, S.Disagreements, S.Agreement,
		S.Outliers, S.Outlier, S.MaxError, *S.MaxLabel, Spread.Compositions));
	Spread.Roads.Sort([](const TPair<double, int32>& L, const TPair<double, int32>& R) { return L.Key > R.Key; });
	for (int32 I = 0; I < FMath::Min(10, Spread.Roads.Num()) && Spread.Roads[I].Key > S.Agreement; I++)
	{
		const FRoadSpec& Road = Roads[Spread.Roads[I].Value];
		auto Degree = [&Roads](int32 Node) { int32 Count = 0; for (const FRoadSpec& Other : Roads) Count += (Other.SourceStart == Node ? 1 : 0) + (Other.SourceEnd == Node ? 1 : 0); return Count; };
		Test.AddInfo(FString::Printf(TEXT("  road%d %s: %.4g; from (%.2f, %.2f, %.2f) degree %d, to (%.2f, %.2f, %.2f) degree %d"), Spread.Roads[I].Value, *Road.Prefab, Spread.Roads[I].Key,
			Road.Curve.a.x, Road.Curve.a.y, Road.Curve.a.z, Degree(Road.SourceStart), Road.Curve.d.x, Road.Curve.d.y, Road.Curve.d.z, Degree(Road.SourceEnd)));
	}
	for (const FString& Label : S.OutlierLabels) Test.AddInfo(TEXT("  outlier ") + Label);
}
}

// BuildSplineNetwork on small drawn networks: shared points, crossings and roads ending on roads
// become junctions, overpasses do not, long and turning segments split, short ones merge.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesBasicsTest, "PCGPlugins.CSRoadNet.Splines.Basics",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSplinesBasicsTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const FSplineNetworkSettings JunctionsOnly = CSRoadSplineTests_JunctionsOnly();
	FCSRoadSplineTests_Network N;

	// Two roads crossing at the same height: one junction of four edges.
	if (CSRoadSplineTests_Build(*this, TEXT("cross"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0) }) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("cross: edges"), N.Roads.Num(), 4);
		TestEqual(TEXT("cross: nodes"), N.Nodes.Num(), 5);
		TestEqual(TEXT("cross: junctions"), N.Stats.Junctions, 1);
		const int32 Junction = N.NodeAt(FVector3d::ZeroVector);
		TestTrue(TEXT("cross: junction at the crossing"), Junction != INDEX_NONE && N.Degree(Junction) == 4);
		TestTrue(TEXT("cross: edges end on their nodes"), N.EdgesOnNodes());
	}

	// 10 m apart in height: an overpass, no junction.
	if (CSRoadSplineTests_Build(*this, TEXT("overpass"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), CSRoadSplineTests_Road({ FVector3d(0.0, 10.0, -100.0), FVector3d(0.0, 10.0, 100.0) }) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("overpass: edges"), N.Roads.Num(), 2);
		TestEqual(TEXT("overpass: junctions"), N.Stats.Junctions, 0);
	}

	// 1.5 m apart, within the 2 m junction height: they meet halfway up.
	if (CSRoadSplineTests_Build(*this, TEXT("near cross"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), CSRoadSplineTests_Road({ FVector3d(0.0, 1.5, -100.0), FVector3d(0.0, 1.5, 100.0) }) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("near cross: edges"), N.Roads.Num(), 4);
		TestTrue(TEXT("near cross: junction at mid height"), N.NodeAt(FVector3d(0.0, 0.75, 0.0)) != INDEX_NONE);
	}

	// A road ending 0.6 m short of another joins it: a T of three edges, its end pulled on.
	if (CSRoadSplineTests_Build(*this, TEXT("tee"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, 50.0), FVector3d(0.0, 0.0, 0.6) }) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("tee: edges"), N.Roads.Num(), 3);
		TestEqual(TEXT("tee: nodes"), N.Nodes.Num(), 4);
		const int32 Junction = N.NodeAt(FVector3d::ZeroVector);
		TestTrue(TEXT("tee: junction on the through road"), Junction != INDEX_NONE && N.Degree(Junction) == 3);
		TestTrue(TEXT("tee: edges end on their nodes"), N.EdgesOnNodes());
	}

	// Fixed splines never form junctions where they cross.
	if (CSRoadSplineTests_Build(*this, TEXT("fixed cross"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }, false), CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0) }, false) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("fixed cross: edges"), N.Roads.Num(), 2);
		TestEqual(TEXT("fixed cross: junctions"), N.Stats.Junctions, 0);
	}

	// Points 0.54 m apart share a node; the second road's start moves onto the first one's end.
	if (CSRoadSplineTests_Build(*this, TEXT("snap"), { CSRoadSplineTests_Road({ FVector3d(-100.0, 0.0, 0.0), FVector3d(0.0, 0.0, 0.0) }), CSRoadSplineTests_Road({ FVector3d(0.5, 0.0, 0.2), FVector3d(100.0, 0.0, 0.0) }) }, JunctionsOnly, N))
	{
		TestEqual(TEXT("snap: nodes"), N.Nodes.Num(), 3);
		TestTrue(TEXT("snap: edges end on their nodes"), N.EdgesOnNodes());
	}

	// 500 m with segments of at most 96 m: six equal edges.
	FSplineNetworkSettings Lengths = JunctionsOnly;
	Lengths.MaxEdgeLength = 96.0;
	Lengths.MinEdgeLength = 8.0;
	if (CSRoadSplineTests_Build(*this, TEXT("length"), { CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, 0.0), FVector3d(500.0, 0.0, 0.0) }) }, Lengths, N))
	{
		TestEqual(TEXT("length: edges"), N.Roads.Num(), 6);
		TestEqual(TEXT("length: splits"), N.Stats.Splits, 5);
		for (const FRoadSpec& Road : N.Roads) TestTrue(TEXT("length: equal parts"), FMath::Abs(math::distance(Road.Curve.a, Road.Curve.d) - 500.0f / 6.0f) < 0.05f);
		TestTrue(TEXT("length: edges end on their nodes"), N.EdgesOnNodes());
	}

	// A quarter circle (one Hermite segment) turning at most 30 degrees per segment: three edges.
	FSplineNetworkSettings Turns = JunctionsOnly;
	Turns.MaxEdgeTurn = UE_DOUBLE_PI / 6.0;
	Turns.MinEdgeLength = 8.0;
	{
		FSplineRoad Arc = CSRoadSplineTests_Road({ FVector3d(100.0, 0.0, 0.0), FVector3d(0.0, 0.0, 100.0) });
		const double Handle = 3.0 * 100.0 * 4.0 / 3.0 * FMath::Tan(UE_DOUBLE_PI / 8.0);
		Arc.Leave[0] = Arc.Arrive[0] = FVector3d(0, 0, Handle);
		Arc.Leave[1] = Arc.Arrive[1] = FVector3d(-Handle, 0, 0);
		if (CSRoadSplineTests_Build(*this, TEXT("turn"), { Arc }, Turns, N))
		{
			TestEqual(TEXT("turn: edges"), N.Roads.Num(), 3);
			TestTrue(TEXT("turn: edges end on their nodes"), N.EdgesOnNodes());
		}
	}

	// A 3 m segment at a plain bend merges into its neighbour: one straight edge.
	FSplineNetworkSettings Merges = JunctionsOnly;
	Merges.MinEdgeLength = 8.0;
	if (CSRoadSplineTests_Build(*this, TEXT("merge"), { CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, 0.0), FVector3d(3.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }) }, Merges, N))
	{
		TestEqual(TEXT("merge: edges"), N.Roads.Num(), 1);
		TestEqual(TEXT("merge: merges"), N.Stats.Merges, 1);
		if (N.Roads.Num() == 1)
		{
			const Bezier4x3& Curve = N.Roads[0].Curve;
			TestTrue(TEXT("merge: stays on the line"), Curve.b.y == 0.0f && Curve.b.z == 0.0f && Curve.c.y == 0.0f && Curve.c.z == 0.0f);
		}
	}

	// A closed square of straight sides: four edges, every node a plain bend.
	{
		FSplineRoad Square = CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, 0.0), FVector3d(50.0, 0.0, 0.0), FVector3d(50.0, 0.0, 50.0), FVector3d(0.0, 0.0, 50.0) });
		Square.bClosed = true;
		Square.Linear.Init(true, 4);
		if (CSRoadSplineTests_Build(*this, TEXT("loop"), { Square }, Merges, N))
		{
			TestEqual(TEXT("loop: edges"), N.Roads.Num(), 4);
			TestEqual(TEXT("loop: nodes"), N.Nodes.Num(), 4);
			for (int32 Node = 0; Node < N.Nodes.Num(); Node++) TestEqual(TEXT("loop: degree"), N.Degree(Node), 2);
		}
	}

	// An unknown prefab is an error.
	{
		FSplineRoad Bad = CSRoadSplineTests_Road({ FVector3d(0.0, 0.0, 0.0), FVector3d(50.0, 0.0, 0.0) });
		Bad.Prefab = TEXT("No Such Road");
		FString Error;
		TestFalse(TEXT("unknown prefab fails"), BuildSplineNetwork(Lib.Assets, { Bad }, JunctionsOnly, nullptr, N.Nodes, N.Roads, N.Stats, Error));
	}
	return true;
}

// Palmsland as splines (ChainNetwork) and back (BuildSplineNetwork): the same edges and nodes with
// their data (owners aside), the edges in their content order rather than the file's; the GPU geometry
// then differs only where the geometry jobs break ties or sum in edge order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesPalmslandTest, "PCGPlugins.CSRoadNet.Splines.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSplinesPalmslandTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	const FString Path = FPaths::Combine(CSRoadTest::LabRoot(), TEXT("Artifacts/public-saves/palmsland-3/network.json"));
	if (!Lib.bLoaded || !FPaths::FileExists(Path))
	{
		AddWarning(TEXT("Palmsland network missing (skipped)"));
		return true;
	}
	FSavedNetwork Network;
	FString Error;
	if (!Network.Load(Path, &Lib.Assets, false, Error))
	{
		AddError(Error);
		return false;
	}
	TArray<FSplineRoad> Splines;
	ChainNetwork(Network.Nodes, Network.Roads, Splines);
	int32 Points = 0;
	for (const FSplineRoad& Spline : Splines) Points += Spline.Points.Num();
	FCSRoadSplineTests_Network Back;
	if (!BuildSplineNetwork(Lib.Assets, Splines, FSplineNetworkSettings(), nullptr, Back.Nodes, Back.Roads, Back.Stats, Error))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("edges"), Back.Roads.Num(), Network.Roads.Num());
	TestEqual(TEXT("nothing joined, split, merged or dropped"), Back.Stats.Junctions + Back.Stats.Splits + Back.Stats.Merges + Back.Stats.Dropped, 0);

	// Every edge back with its data and its nodes' data.
	TMap<FString, int32> Original;
	for (int32 R = 0; R < Network.Roads.Num(); R++) Original.Add(CSRoadSplineTests_Key(Network.Roads[R].Curve), R);
	const float2 Zero(0.0f);
	TArray<int32> Match;
	Match.Init(INDEX_NONE, Back.Roads.Num());
	int32 Missing = 0;
	int32 EdgeMismatches = 0;
	int32 NodeMismatches = 0;
	for (int32 R = 0; R < Back.Roads.Num(); R++)
	{
		const FRoadSpec& Mine = Back.Roads[R];
		const int32* Found = Original.Find(CSRoadSplineTests_Key(Mine.Curve));
		if (!Found)
		{
			if (Missing++ < 5) AddInfo(FString::Printf(TEXT("edge %d (%s) has no original"), R, *Mine.Prefab));
			continue;
		}
		Match[R] = *Found;
		const FRoadSpec& Theirs = Network.Roads[*Found];
		if (Mine.Prefab != Theirs.Prefab || !CSRoadSplineTests_Same(Mine.Upgrade, Theirs.Upgrade) || !CSRoadSplineTests_Same(Mine.Elevation, Theirs.bHasElevation ? Theirs.Elevation : Zero)
			|| (Mine.Owner != 0) != (Theirs.Owner != 0)) EdgeMismatches++;
		for (const bool bStart : { true, false })
		{
			const FSavedNode& A = Back.Nodes[bStart ? Mine.SourceStart : Mine.SourceEnd];
			const FSavedNode& B = Network.Nodes[bStart ? Theirs.SourceStart : Theirs.SourceEnd];
			if (!math::all(A.Position == B.Position) || A.Prefab != B.Prefab || !CSRoadSplineTests_Same(A.Elevation, B.bHasElevation ? B.Elevation : Zero) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade)
				|| A.bOutside != B.bOutside || (A.Owner != 0) != (B.Owner != 0)) NodeMismatches++;
		}
	}
	TestEqual(TEXT("edges without an original"), Missing, 0);
	TestEqual(TEXT("edges with other data"), EdgeMismatches, 0);
	TestEqual(TEXT("edge ends on another node"), NodeMismatches, 0);
	AddInfo(FString::Printf(TEXT("palmsland: %d edges as %d splines (%d points), back into %d edges and %d nodes"), Network.Roads.Num(), Splines.Num(), Points, Back.Roads.Num(), Back.Nodes.Num()));

	// The splines in any other order give the same network, edge for edge and node for node.
	{
		TArray<FSplineRoad> Shuffled = Splines;
		FRandomStream Random(7);
		for (int32 I = Shuffled.Num() - 1; I > 0; I--) Shuffled.Swap(I, Random.RandRange(0, I));
		FCSRoadSplineTests_Network Again;
		if (!BuildSplineNetwork(Lib.Assets, Shuffled, FSplineNetworkSettings(), nullptr, Again.Nodes, Again.Roads, Again.Stats, Error))
		{
			AddError(Error);
			return false;
		}
		int32 Differ = FMath::Abs(Again.Roads.Num() - Back.Roads.Num()) + FMath::Abs(Again.Nodes.Num() - Back.Nodes.Num());
		for (int32 R = 0; R < FMath::Min(Again.Roads.Num(), Back.Roads.Num()); R++)
		{
			const FRoadSpec& A = Again.Roads[R];
			const FRoadSpec& B = Back.Roads[R];
			if (A.Prefab != B.Prefab || CSRoadSplineTests_Key(A.Curve) != CSRoadSplineTests_Key(B.Curve) || A.SourceStart != B.SourceStart || A.SourceEnd != B.SourceEnd || A.Owner != B.Owner
				|| !CSRoadSplineTests_Same(A.Elevation, B.Elevation) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade)) Differ++;
		}
		for (int32 N = 0; N < FMath::Min(Again.Nodes.Num(), Back.Nodes.Num()); N++)
		{
			const FSavedNode& A = Again.Nodes[N];
			const FSavedNode& B = Back.Nodes[N];
			if (!math::all(A.Position == B.Position) || A.Prefab != B.Prefab || !CSRoadSplineTests_Same(A.Elevation, B.Elevation) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade) || A.bOutside != B.bOutside
				|| A.Owner != B.Owner) Differ++;
		}
		TestEqual(TEXT("splines shuffled: edges and nodes that differ"), Differ, 0);
	}

	// The GPU geometry against the file's: the same compositions, and values apart only by what the edge order
	// moves (on Palmsland under 3 cm, as much as any other order of the file's own edges moves them).
	FCSRoadSplineTests_Geometry File;
	FCSRoadSplineTests_Geometry FromSplines;
	if (!File.Build(Lib.Assets, Network.Roads, Network.Nodes, Network.bLeftHandTraffic, Error) || !FromSplines.Build(Lib.Assets, Back.Roads, Back.Nodes, Network.bLeftHandTraffic, Error))
	{
		AddError(Error);
		return false;
	}
	FCSRoadSplineTests_Spread Spread;
	CSRoadSplineTests_Compare(*this, FromSplines, File, Match, Spread);
	CSRoadSplineTests_Report(*this, TEXT("palmsland from splines against the file"), Spread, Network.Roads);
	TestEqual(TEXT("edges with other compositions"), Spread.Compositions, 0);
	TestEqual(TEXT("GPU geometry values more than 0.1 m off"), Spread.Stats.Outliers, int64(0));
	return true;
}

// Alleys giving way to main roads: an alley a main road crosses is cut there and both ends meet it in
// one crossing (also on a slant); one a main road runs along loses the stretch under it and its ends
// join the road; a main road passing high overhead leaves it whole; and the alley comes back as it
// was when the main road goes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesAlleysTest, "PCGPlugins.CSRoadNet.Splines.Alleys",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSplinesAlleysTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	const FSplineNetworkSettings Settings = CSRoadSplineTests_JunctionsOnly();
	auto Alley = [](const TArray<FVector3d>& Points)
	{
		FSplineRoad Road = CSRoadSplineTests_Road(Points);
		Road.Prefab = TEXT("Alley");
		Road.bAlley = true;
		return Road;
	};
	auto Main = [](const TArray<FVector3d>& Points)
	{
		FSplineRoad Road = CSRoadSplineTests_Road(Points);
		Road.Prefab = TEXT("Medium Road");
		return Road;
	};
	auto Count = [](const FCSRoadSplineTests_Network& Network, const TCHAR* Prefab)
	{
		int32 Total = 0;
		for (const FRoadSpec& Road : Network.Roads) Total += Road.Prefab == Prefab ? 1 : 0;
		return Total;
	};
	// No alley edge comes closer to the main road's line (from A to B) than its reach, but next to its
	// ends (where it meets the road, on a slant too).
	const double Reach = double(Lib.Assets.Geometry(TEXT("Medium Road")).m_DefaultWidth + Lib.Assets.Geometry(TEXT("Alley")).m_DefaultWidth) * 0.5;
	auto Clear = [&](const FCSRoadSplineTests_Network& Network, const FVector3d& A, const FVector3d& B)
	{
		for (const FRoadSpec& Road : Network.Roads)
		{
			if (Road.Prefab != TEXT("Alley")) continue;
			for (int32 K = 1; K < 16; K++)
			{
				const float3 P = MathUtils::Position(Road.Curve, K / 16.0f);
				const FVector2d Point(P.x, P.z);
				const FVector2d Closest = FMath::ClosestPointOnSegment2D(Point, FVector2d(A.X, A.Z), FVector2d(B.X, B.Z));
				if (FVector2d::Distance(Point, Closest) < Reach - 0.5 && FVector2d::Distance(Point, FVector2d(Road.Curve.a.x, Road.Curve.a.z)) > Reach * 2.0 && FVector2d::Distance(Point, FVector2d(Road.Curve.d.x, Road.Curve.d.z)) > Reach * 2.0) return false;
			}
		}
		return true;
	};

	{
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("crossing"), { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0) }) }, Settings, N)) return false;
		TestEqual(TEXT("crossing: one stretch cut"), N.Stats.AlleyCuts, 1);
		TestEqual(TEXT("crossing: two alley parts"), Count(N, TEXT("Alley")), 2);
		const int32 Cross = N.NodeAt(FVector3d(0.0, 0.0, 0.0), 0.05);
		TestTrue(TEXT("crossing: one crossing node"), Cross != INDEX_NONE && N.Degree(Cross) == 4);
		TestTrue(TEXT("crossing: no alley under the road"), Clear(N, FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0)));
		TestTrue(TEXT("crossing: edges on their nodes"), N.EdgesOnNodes());
	}
	{
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("slant"), { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(-60.0, 0.0, -100.0), FVector3d(60.0, 0.0, 100.0) }) }, Settings, N)) return false;
		TestEqual(TEXT("slant: two alley parts"), Count(N, TEXT("Alley")), 2);
		int32 Crossings = 0;
		for (int32 Node = 0; Node < N.Nodes.Num(); Node++) Crossings += N.Degree(Node) == 4 ? 1 : 0;
		TestEqual(TEXT("slant: one crossing"), Crossings, 1);
		TestTrue(TEXT("slant: no alley under the road"), Clear(N, FVector3d(-60.0, 0.0, -100.0), FVector3d(60.0, 0.0, 100.0)));
	}
	{
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("along"), { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(-40.0, 0.0, 3.0), FVector3d(40.0, 0.0, 3.0) }) }, Settings, N)) return false;
		TestEqual(TEXT("along: one stretch cut"), N.Stats.AlleyCuts, 1);
		TestEqual(TEXT("along: two alley parts"), Count(N, TEXT("Alley")), 2);
		TestTrue(TEXT("along: no alley under the road"), Clear(N, FVector3d(-40.0, 0.0, 3.0), FVector3d(40.0, 0.0, 3.0)));
		// Each part ends on the main road.
		int32 Joined = 0;
		for (const FRoadSpec& Road : N.Roads)
		{
			if (Road.Prefab != TEXT("Alley")) continue;
			for (const FRoadSpec& Other : N.Roads)
			{
				if (Other.Prefab == TEXT("Alley")) continue;
				if (Road.SourceStart == Other.SourceStart || Road.SourceStart == Other.SourceEnd || Road.SourceEnd == Other.SourceStart || Road.SourceEnd == Other.SourceEnd)
				{
					Joined++;
					break;
				}
			}
		}
		TestEqual(TEXT("along: both parts join the main road"), Joined, 2);
	}
	{
		FCSRoadSplineTests_Network Alone;
		FCSRoadSplineTests_Network Over;
		const FSplineRoad Lane = Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) });
		if (!CSRoadSplineTests_Build(*this, TEXT("alone"), { Lane }, Settings, Alone)) return false;
		if (!CSRoadSplineTests_Build(*this, TEXT("overpass"), { Lane, Main({ FVector3d(0.0, 10.0, -100.0), FVector3d(0.0, 10.0, 100.0) }) }, Settings, Over)) return false;
		TestEqual(TEXT("overpass: nothing cut"), Over.Stats.AlleyCuts, 0);
		TestEqual(TEXT("overpass: the alley whole"), Count(Over, TEXT("Alley")), 1);
		bool bSame = Count(Alone, TEXT("Alley")) == 1;
		for (const FRoadSpec& Road : Over.Roads) if (Road.Prefab == TEXT("Alley")) bSame &= CSRoadSplineTests_Key(Road.Curve) == CSRoadSplineTests_Key(Alone.Roads[0].Curve);
		TestTrue(TEXT("overpass: the alley as drawn"), bSame);
	}
	return true;
}

// The actor in a level, placed off the origin and turned: two spline roads under it become roads
// where the splines are, and follow it when it moves or a spline changes. The meshes are
// world-space, so their components stay at identity wherever the actor is.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesActorTest, "PCGPlugins.CSRoadNet.Splines.Actor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRoadSplinesActorTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>(FVector(250000.0, -120000.0, 3000.0), FRotator(0.0, 30.0, 0.0));
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	Network->Source = ECSRoadNetworkSource::Splines;
	// Two 60 m roads crossing at the network's origin.
	const TArray<FVector> Ends = { FVector(-3000.0, 0.0, 0.0), FVector(3000.0, 0.0, 0.0), FVector(0.0, -3000.0, 0.0), FVector(0.0, 3000.0, 0.0) };
	TArray<ACSRoadSplineActor*> Roads;
	for (int32 I = 0; I < Ends.Num(); I += 2)
	{
		ACSRoadSplineActor* Road = Roads.Add_GetRef(World->SpawnActor<ACSRoadSplineActor>());
		Road->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
		Road->Spline->SetSplinePoints({ Ends[I], Ends[I + 1] }, ESplineCoordinateSpace::Local);
	}
	for (const bool bMoved : { false, true })
	{
		const FString Case = bMoved ? TEXT("moved") : TEXT("placed");
		if (bMoved) Network->SetActorLocation(Network->GetActorLocation() + FVector(10000.0, 5000.0, 0.0));
		Network->Rebuild();
		TestEqual(Case + TEXT(": error"), Network->LastError, FString());
		TestEqual(Case + TEXT(": splines"), Network->NumSplines, 2);
		TestEqual(Case + TEXT(": roads"), Network->NumRoads, 4);
		TestTrue(Case + TEXT(": markings"), Network->NumMarkingTriangles > 0);
		TestTrue(Case + TEXT(": road mesh at identity"), Network->RoadMesh->GetComponentTransform().Equals(FTransform::Identity));
		TestTrue(Case + TEXT(": marking mesh at identity"), Network->MarkingMesh->GetComponentTransform().Equals(FTransform::Identity));
		const FBox Bounds = Network->RoadMesh->Bounds.GetBox();
		for (const FVector& End : Ends) TestTrue(Case + TEXT(": bounds hold the road ends"), Bounds.IsInside(Network->GetActorTransform().TransformPosition(End)));
		TestTrue(Case + TEXT(": bounds centred on the crossing"), FVector::Dist2D(Bounds.GetCenter(), Network->GetActorLocation()) < 500.0);
	}

	// A spline edited (by the editor's spline tools, or code) has the network rebuild on its next tick.
	const FVector Moved(6000.0, 0.0, 0.0);
	Roads[0]->Spline->SetLocationAtSplinePoint(1, Moved, ESplineCoordinateSpace::Local);
	Network->Tick(0.0f);
	TestEqual(TEXT("edited: error"), Network->LastError, FString());
	TestTrue(TEXT("edited: bounds hold the moved end"), Network->RoadMesh->Bounds.GetBox().IsInside(Network->GetActorTransform().TransformPosition(Moved)));
	return true;
}

#endif

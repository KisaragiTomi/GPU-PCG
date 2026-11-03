#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMeshRenderComponent.h"
#include "CSRoadGpuGeometry.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadSplineNetwork.h"
#include "CSRoadTestUtils.h"
#include "Components/InstancedStaticMeshComponent.h"
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

// The edges and nodes two networks have otherwise, position for position, and how many more one has.
int32 CSRoadSplineTests_Differences(const FCSRoadSplineTests_Network& Mine, const FCSRoadSplineTests_Network& Theirs)
{
	int32 Differ = FMath::Abs(Mine.Roads.Num() - Theirs.Roads.Num()) + FMath::Abs(Mine.Nodes.Num() - Theirs.Nodes.Num());
	for (int32 R = 0; R < FMath::Min(Mine.Roads.Num(), Theirs.Roads.Num()); R++)
	{
		const FRoadSpec& A = Mine.Roads[R];
		const FRoadSpec& B = Theirs.Roads[R];
		if (A.Prefab != B.Prefab || CSRoadSplineTests_Key(A.Curve) != CSRoadSplineTests_Key(B.Curve) || A.SourceStart != B.SourceStart || A.SourceEnd != B.SourceEnd
			|| !CSRoadSplineTests_Same(A.Elevation, B.Elevation) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade)) Differ++;
	}
	for (int32 N = 0; N < FMath::Min(Mine.Nodes.Num(), Theirs.Nodes.Num()); N++)
	{
		const FSavedNode& A = Mine.Nodes[N];
		const FSavedNode& B = Theirs.Nodes[N];
		if (!math::all(A.Position == B.Position) || A.Prefab != B.Prefab || !CSRoadSplineTests_Same(A.Elevation, B.Elevation) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade) || A.bOutside != B.bOutside) Differ++;
	}
	return Differ;
}

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

FString CSRoadSplineTests_Blob(const void* Data, int32 Size) { return FString::FromBlob(static_cast<const uint8*>(Data), Size); }

// Everything a node holds, as text.
FString CSRoadSplineTests_NodeText(const FSavedNode& Node)
{
	return Node.Prefab + TEXT("|") + CSRoadSplineTests_Blob(&Node.Position, sizeof(Node.Position)) + CSRoadSplineTests_Blob(&Node.Rotation, sizeof(Node.Rotation))
		+ CSRoadSplineTests_Blob(&Node.Elevation, sizeof(Node.Elevation)) + CSRoadSplineTests_Blob(&Node.Upgrade, sizeof(Node.Upgrade)) + (Node.bHasElevation ? TEXT("e") : TEXT("-"))
		+ (Node.bOutside ? TEXT("o") : TEXT("-"));
}

// Everything an edge holds, its nodes' data in place of their indices, as text.
FString CSRoadSplineTests_RoadText(const TArray<FSavedNode>& Nodes, const FRoadSpec& Road)
{
	return Road.Prefab + TEXT("|") + CSRoadSplineTests_Blob(&Road.Curve, sizeof(Road.Curve)) + CSRoadSplineTests_Blob(&Road.Upgrade, sizeof(Road.Upgrade))
		+ CSRoadSplineTests_Blob(&Road.Elevation, sizeof(Road.Elevation)) + (Road.bHasElevation ? TEXT("e") : TEXT("-")) + (Road.bHasSource ? TEXT("s") : TEXT("-")) + TEXT("|")
		+ CSRoadSplineTests_NodeText(Nodes[Road.SourceStart]) + TEXT("|") + CSRoadSplineTests_NodeText(Nodes[Road.SourceEnd]);
}

// The positions where two networks hold anything else (node indices too), and how many more one holds.
int32 CSRoadSplineTests_Mismatches(const FCSRoadSplineTests_Network& A, const FCSRoadSplineTests_Network& B)
{
	int32 Count = FMath::Abs(A.Roads.Num() - B.Roads.Num()) + FMath::Abs(A.Nodes.Num() - B.Nodes.Num());
	for (int32 R = 0; R < FMath::Min(A.Roads.Num(), B.Roads.Num()); R++)
	{
		const FRoadSpec& X = A.Roads[R];
		const FRoadSpec& Y = B.Roads[R];
		if (X.SourceStart != Y.SourceStart || X.SourceEnd != Y.SourceEnd || CSRoadSplineTests_RoadText(A.Nodes, X) != CSRoadSplineTests_RoadText(B.Nodes, Y)) Count++;
	}
	for (int32 N = 0; N < FMath::Min(A.Nodes.Num(), B.Nodes.Num()); N++) if (CSRoadSplineTests_NodeText(A.Nodes[N]) != CSRoadSplineTests_NodeText(B.Nodes[N])) Count++;
	return Count;
}

// The diff takes the last network to this one: the last one less what went is this one less what came,
// edge for edge in order, and node for node as sets.
bool CSRoadSplineTests_DiffHolds(const FCSRoadSplineTests_Network& Last, const FCSRoadSplineTests_Network& Now, const FSplineNetworkDiff& Diff)
{
	auto Valid = [](const TArray<int32>& Indices, int32 Count)
	{
		for (int32 I = 0; I < Indices.Num(); I++) if (Indices[I] < 0 || Indices[I] >= Count || (I > 0 && Indices[I] <= Indices[I - 1])) return false;
		return true;
	};
	if (!Diff.bValid || !Valid(Diff.RemovedRoads, Last.Roads.Num()) || !Valid(Diff.AddedRoads, Now.Roads.Num()) || !Valid(Diff.RemovedNodes, Last.Nodes.Num()) || !Valid(Diff.AddedNodes, Now.Nodes.Num())) return false;
	auto Rest = [](const FCSRoadSplineTests_Network& Network, const TArray<int32>& Left)
	{
		TArray<FString> Out;
		for (int32 R = 0, K = 0; R < Network.Roads.Num(); R++)
		{
			if (K < Left.Num() && Left[K] == R)
			{
				K++;
				continue;
			}
			Out.Add(CSRoadSplineTests_RoadText(Network.Nodes, Network.Roads[R]));
		}
		return Out;
	};
	if (Rest(Last, Diff.RemovedRoads) != Rest(Now, Diff.AddedRoads)) return false;
	TSet<int32> Removed;
	TSet<int32> Added;
	Removed.Append(Diff.RemovedNodes);
	Added.Append(Diff.AddedNodes);
	TMap<FString, int32> Nodes;
	for (int32 N = 0; N < Last.Nodes.Num(); N++) if (!Removed.Contains(N)) Nodes.FindOrAdd(CSRoadSplineTests_NodeText(Last.Nodes[N]))++;
	for (int32 N = 0; N < Now.Nodes.Num(); N++) if (!Added.Contains(N)) Nodes.FindOrAdd(CSRoadSplineTests_NodeText(Now.Nodes[N]))--;
	for (const TPair<FString, int32>& Pair : Nodes) if (Pair.Value != 0) return false;
	return true;
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
// their data, the edges in their content order rather than the file's; the GPU geometry
// then differs only where the geometry jobs break ties or sum in edge order.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesPalmslandTest, "PCGPlugins.CSRoadNet.Splines.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSplinesPalmslandTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	const FString Path = CSRoadTest::PalmslandNetwork();
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
		if (Mine.Prefab != Theirs.Prefab || !CSRoadSplineTests_Same(Mine.Upgrade, Theirs.Upgrade) || !CSRoadSplineTests_Same(Mine.Elevation, Theirs.bHasElevation ? Theirs.Elevation : Zero)) EdgeMismatches++;
		for (const bool bStart : { true, false })
		{
			const FSavedNode& A = Back.Nodes[bStart ? Mine.SourceStart : Mine.SourceEnd];
			const FSavedNode& B = Network.Nodes[bStart ? Theirs.SourceStart : Theirs.SourceEnd];
			if (!math::all(A.Position == B.Position) || A.Prefab != B.Prefab || !CSRoadSplineTests_Same(A.Elevation, B.bHasElevation ? B.Elevation : Zero) || !CSRoadSplineTests_Same(A.Upgrade, B.Upgrade)
				|| A.bOutside != B.bOutside) NodeMismatches++;
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
		TestEqual(TEXT("splines shuffled: edges and nodes that differ"), CSRoadSplineTests_Differences(Again, Back), 0);
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
// was when the main road goes. Every order of the splines builds the same network, also where the
// alley comes into cover exactly as near to two main roads (mirrored about it).
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
	// The same splines in every other order build the same network, edge for edge and node for node.
	// Each rotation, forwards and backwards: every order of three splines or fewer.
	auto EveryOrder = [&](const FString& Case, const TArray<FSplineRoad>& Splines, const FCSRoadSplineTests_Network& First)
	{
		int32 Others = 0;
		for (const bool bBackwards : { false, true })
		{
			for (int32 Rotation = 0; Rotation < Splines.Num(); Rotation++)
			{
				TArray<FSplineRoad> Ordered;
				for (int32 I = 0; I < Splines.Num(); I++) Ordered.Add(Splines[(Rotation + (bBackwards ? Splines.Num() - 1 - I : I)) % Splines.Num()]);
				FCSRoadSplineTests_Network Again;
				if (!CSRoadSplineTests_Build(*this, Case, Ordered, Settings, Again) || CSRoadSplineTests_Differences(Again, First) > 0) Others++;
			}
		}
		TestEqual(Case + TEXT(": orders that build another network"), Others, 0);
	};

	{
		const TArray<FSplineRoad> Splines = { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0) }) };
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("crossing"), Splines, Settings, N)) return false;
		TestEqual(TEXT("crossing: one stretch cut"), N.Stats.AlleyCuts, 1);
		TestEqual(TEXT("crossing: two alley parts"), Count(N, TEXT("Alley")), 2);
		const int32 Cross = N.NodeAt(FVector3d(0.0, 0.0, 0.0), 0.05);
		TestTrue(TEXT("crossing: one crossing node"), Cross != INDEX_NONE && N.Degree(Cross) == 4);
		TestTrue(TEXT("crossing: no alley under the road"), Clear(N, FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 100.0)));
		TestTrue(TEXT("crossing: edges on their nodes"), N.EdgesOnNodes());
		EveryOrder(TEXT("crossing"), Splines, N);
	}
	{
		const TArray<FSplineRoad> Splines = { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(-60.0, 0.0, -100.0), FVector3d(60.0, 0.0, 100.0) }) };
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("slant"), Splines, Settings, N)) return false;
		TestEqual(TEXT("slant: two alley parts"), Count(N, TEXT("Alley")), 2);
		int32 Crossings = 0;
		for (int32 Node = 0; Node < N.Nodes.Num(); Node++) Crossings += N.Degree(Node) == 4 ? 1 : 0;
		TestEqual(TEXT("slant: one crossing"), Crossings, 1);
		TestTrue(TEXT("slant: no alley under the road"), Clear(N, FVector3d(-60.0, 0.0, -100.0), FVector3d(60.0, 0.0, 100.0)));
		EveryOrder(TEXT("slant"), Splines, N);
	}
	{
		// Two main roads mirrored about the alley meet near where it ends (3 m on: ending on their point, it would
		// keep that junction as drawn): where it comes into cover it lies exactly as near to both, and which one
		// it joins must not hang on which came first.
		const TArray<FSplineRoad> Splines = { Alley({ FVector3d(100.0, 0.0, 0.0), FVector3d(3.0, 0.0, 0.0) }), Main({ FVector3d(0.0, 0.0, 0.0), FVector3d(100.0, 0.0, 100.0) }),
			Main({ FVector3d(0.0, 0.0, 0.0), FVector3d(100.0, 0.0, -100.0) }) };
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("tie"), Splines, Settings, N)) return false;
		TestEqual(TEXT("tie: one stretch cut"), N.Stats.AlleyCuts, 1);
		TestEqual(TEXT("tie: one alley part"), Count(N, TEXT("Alley")), 1);
		int32 Tees = 0;
		for (int32 Node = 0; Node < N.Nodes.Num(); Node++) Tees += N.Degree(Node) == 3 ? 1 : 0;
		TestEqual(TEXT("tie: the alley joins one of the main roads"), Tees, 1);
		EveryOrder(TEXT("tie"), Splines, N);
	}
	{
		const TArray<FSplineRoad> Splines = { Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) }), Main({ FVector3d(-40.0, 0.0, 3.0), FVector3d(40.0, 0.0, 3.0) }) };
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("along"), Splines, Settings, N)) return false;
		EveryOrder(TEXT("along"), Splines, N);
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
		// An alley bowing along a main road, in its reach at both ends and out of it between (its middle 20 m
		// off): the part left joins the road from nearly along it, and turns to meet it square instead of in
		// a sliver of a junction (which hung the GPU).
		const TArray<FSplineRoad> Splines = { Alley({ FVector3d(-150.0, 0.0, 8.0), FVector3d(0.0, 0.0, 20.0), FVector3d(150.0, 0.0, 8.0) }), Main({ FVector3d(-200.0, 0.0, 0.0), FVector3d(200.0, 0.0, 0.0) }) };
		FCSRoadSplineTests_Network N;
		if (!CSRoadSplineTests_Build(*this, TEXT("nearly along"), Splines, Settings, N)) return false;
		TestEqual(TEXT("nearly along: two stretches cut"), N.Stats.AlleyCuts, 2);
		// At each node the alley shares with the main road, the alley leaves it 30 degrees or more off the road.
		double Flattest = 180.0;
		int32 Joins = 0;
		for (int32 Node = 0; Node < N.Nodes.Num(); Node++)
		{
			TArray<FVector2d> Alleys;
			TArray<FVector2d> Mains;
			for (const FRoadSpec& Road : N.Roads)
			{
				for (const bool bStart : { true, false })
				{
					if ((bStart ? Road.SourceStart : Road.SourceEnd) != Node) continue;
					const FVector2d Out = bStart ? FVector2d(Road.Curve.b.x - Road.Curve.a.x, Road.Curve.b.z - Road.Curve.a.z) : FVector2d(Road.Curve.c.x - Road.Curve.d.x, Road.Curve.c.z - Road.Curve.d.z);
					(Road.Prefab == TEXT("Alley") ? Alleys : Mains).Add(Out.GetSafeNormal());
				}
			}
			if (Alleys.Num() > 0 && Mains.Num() > 0) Joins++;
			for (const FVector2d& A : Alleys) for (const FVector2d& M : Mains) Flattest = FMath::Min(Flattest, FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(A.Dot(M), -1.0, 1.0))));
		}
		TestEqual(TEXT("nearly along: the part joins the road at both ends"), Joins, 2);
		TestTrue(FString::Printf(TEXT("nearly along: the alley meets the road at 30 degrees or more (%.1f)"), Flattest), Flattest >= 29.0);
		EveryOrder(TEXT("nearly along"), Splines, N);
	}
	{
		FCSRoadSplineTests_Network Alone;
		FCSRoadSplineTests_Network Over;
		const FSplineRoad Lane = Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) });
		if (!CSRoadSplineTests_Build(*this, TEXT("alone"), { Lane }, Settings, Alone)) return false;
		const TArray<FSplineRoad> Splines = { Lane, Main({ FVector3d(0.0, 10.0, -100.0), FVector3d(0.0, 10.0, 100.0) }) };
		if (!CSRoadSplineTests_Build(*this, TEXT("overpass"), Splines, Settings, Over)) return false;
		EveryOrder(TEXT("overpass"), Splines, Over);
		TestEqual(TEXT("overpass: nothing cut"), Over.Stats.AlleyCuts, 0);
		TestEqual(TEXT("overpass: the alley whole"), Count(Over, TEXT("Alley")), 1);
		bool bSame = Count(Alone, TEXT("Alley")) == 1;
		for (const FRoadSpec& Road : Over.Roads) if (Road.Prefab == TEXT("Alley")) bSame &= CSRoadSplineTests_Key(Road.Curve) == CSRoadSplineTests_Key(Alone.Roads[0].Curve);
		TestTrue(TEXT("overpass: the alley as drawn"), bSame);
	}
	{
		// Junctions drawn with the main road (a point of the alley on one of the road's, as a converted snapshot
		// has them) stay as drawn: an alley ending on the road, and one crossing it there.
		const FSplineRoad Road = Main({ FVector3d(0.0, 0.0, -100.0), FVector3d(0.0, 0.0, 0.0), FVector3d(0.0, 0.0, 100.0) });
		for (const bool bCrossing : { false, true })
		{
			const FString Case = bCrossing ? TEXT("drawn crossing") : TEXT("drawn tee");
			FSplineRoad Lane = Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(0.0, 0.0, 0.0) });
			if (bCrossing) Lane = Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(0.0, 0.0, 0.0), FVector3d(100.0, 0.0, 0.0) });
			const TArray<FSplineRoad> Splines = { Lane, Road };
			FCSRoadSplineTests_Network N;
			if (!CSRoadSplineTests_Build(*this, Case, Splines, Settings, N)) return false;
			TestEqual(Case + TEXT(": nothing cut"), N.Stats.AlleyCuts, 0);
			TestEqual(Case + TEXT(": the alley as drawn"), Count(N, TEXT("Alley")), bCrossing ? 2 : 1);
			const int32 Junction = N.NodeAt(FVector3d(0.0, 0.0, 0.0), 0.05);
			TestTrue(Case + TEXT(": the junction where it was drawn"), Junction != INDEX_NONE && N.Degree(Junction) == (bCrossing ? 4 : 3));
			EveryOrder(Case, Splines, N);
		}
		// The road moved 6 m off that point still covers the alley's end: it is cut back and joins the road there,
		// between two of the road's points. Fixed (converted) splines too: the fixed road is split where the cut
		// end joins it, the rest of both stays as drawn.
		for (const bool bFixed : { false, true })
		{
			const FString Case = bFixed ? TEXT("moved fixed road") : TEXT("moved road");
			FSplineRoad Lane = Alley({ FVector3d(-100.0, 0.0, 0.0), FVector3d(0.0, 0.0, 0.0) });
			FSplineRoad Moved = Main({ FVector3d(6.0, 0.0, -100.0), FVector3d(6.0, 0.0, 100.0) });
			Lane.bAdjust = !bFixed;
			Moved.bAdjust = !bFixed;
			const TArray<FSplineRoad> Splines = { Lane, Moved };
			FCSRoadSplineTests_Network N;
			if (!CSRoadSplineTests_Build(*this, Case, Splines, Settings, N)) return false;
			TestEqual(Case + TEXT(": one stretch cut"), N.Stats.AlleyCuts, 1);
			TestEqual(Case + TEXT(": one alley part"), Count(N, TEXT("Alley")), 1);
			int32 Tees = 0;
			for (int32 Node = 0; Node < N.Nodes.Num(); Node++) Tees += N.Degree(Node) == 3 ? 1 : 0;
			TestEqual(Case + TEXT(": the alley joins the road"), Tees, 1);
			TestTrue(Case + TEXT(": no alley under the road"), Clear(N, FVector3d(6.0, 0.0, -100.0), FVector3d(6.0, 0.0, 100.0)));
			EveryOrder(Case, Splines, N);
		}
	}
	return true;
}

// Converting only around the splines an edit changed (BuildSplineNetwork with a cache) makes the network
// converting every spline makes, edge for edge and node for node, through runs of random edits (points
// moved, also onto other splines' points, splines added and removed, other prefabs, point data, fixed or
// adjusted): on a drawn town of adjusted splines (a main grid, streets ending on it, alleys across and
// along main roads, a closed ring, three roads crossing at one point, a fixed spline, an overpass) and on
// Palmsland as fixed splines. The diff takes each network to the next.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadSplinesIncrementalTest, "PCGPlugins.CSRoadNet.Splines.Incremental",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadSplinesIncrementalTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	// A run of edits, each converted with the cache and without, and how they went.
	struct FRun
	{
		FString Name;
		FSplineNetworkCache Cache;
		FCSRoadSplineTests_Network Last;
		bool bLast = false;
		int32 Builds = 0;
		int32 Around = 0;
		int32 Converted = 0;
		int32 Of = 0;
		int32 MaxRounds = 0;
		double AroundMs = 0.0;
		double WholeMs = 0.0;
	};
	auto Step = [this, &Lib](FRun& Run, const FString& Case, const TArray<FSplineRoad>& Splines, const FSplineNetworkSettings& Settings, bool bVerify)
	{
		FCSRoadSplineTests_Network Mine;
		FCSRoadSplineTests_Network Whole;
		FString MineError;
		FString WholeError;
		double Start = FPlatformTime::Seconds();
		const bool bMine = BuildSplineNetwork(Lib.Assets, Splines, Settings, nullptr, Mine.Nodes, Mine.Roads, Mine.Stats, MineError, &Run.Cache, bVerify);
		const double MineMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		Start = FPlatformTime::Seconds();
		const bool bWhole = BuildSplineNetwork(Lib.Assets, Splines, Settings, nullptr, Whole.Nodes, Whole.Roads, Whole.Stats, WholeError);
		const double WholeMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		const FString Where = Run.Name + TEXT(", ") + Case;
		if (bMine != bWhole)
		{
			AddError(FString::Printf(TEXT("%s: only one conversion fails (%s%s)"), *Where, *MineError, *WholeError));
			return false;
		}
		if (!bWhole)
		{
			Run.bLast = false;
			return true;
		}
		const int32 Mismatches = CSRoadSplineTests_Mismatches(Mine, Whole);
		if (Mismatches > 0) AddError(FString::Printf(TEXT("%s: %d values differ from converting every spline (%s: %d of %d converted around %d changed in %d rounds)"), *Where, Mismatches,
			Mine.Stats.bWhole ? TEXT("all converted") : TEXT("around the change"), Mine.Stats.Converted, Splines.Num(), Mine.Stats.Changed, Mine.Stats.Rounds));
		if (bVerify) TestEqual(Where + TEXT(": values the verification finds apart"), Mine.Stats.Mismatches, 0);
		if (!Mine.Stats.bWhole && !Run.Cache.Diff.bValid) AddError(Where + TEXT(": converted around the change without a diff"));
		if (Run.bLast && Run.Cache.Diff.bValid && !CSRoadSplineTests_DiffHolds(Run.Last, Mine, Run.Cache.Diff)) AddError(Where + TEXT(": the diff does not take the last network to this one"));
		if (Run.bLast)
		{
			Run.Builds++;
			Run.MaxRounds = FMath::Max(Run.MaxRounds, Mine.Stats.Rounds);
			Run.AroundMs += MineMs;
			Run.WholeMs += WholeMs;
			if (!Mine.Stats.bWhole)
			{
				Run.Around++;
				Run.Converted += Mine.Stats.Converted;
				Run.Of += Splines.Num();
			}
		}
		Run.Last = MoveTemp(Mine);
		Run.bLast = true;
		return Mismatches == 0;
	};
	auto Report = [this](const FRun& Run)
	{
		AddInfo(FString::Printf(TEXT("%s: %d builds, %d converted around the change (%.1f%% of the splines on average, at most %d rounds); %.2f ms a build, %.2f ms converting every spline"),
			*Run.Name, Run.Builds, Run.Around, Run.Of > 0 ? 100.0 * Run.Converted / Run.Of : 0.0, Run.MaxRounds, Run.AroundMs / FMath::Max(Run.Builds, 1), Run.WholeMs / FMath::Max(Run.Builds, 1)));
	};
	// A random edit: a point moved (by up to Move, or onto a point of some spline), a road added across a
	// point of some spline or a spline removed, another prefab, a point's data, or the other adjustment.
	auto Edit = [](TArray<FSplineRoad>& Splines, FRandomStream& Random, uint64& NextId, double Move, const TArray<FString>& Prefabs) -> FString
	{
		const int32 Pick = Random.RandRange(0, Splines.Num() - 1);
		FSplineRoad& Road = Splines[Pick];
		const int32 Point = Random.RandRange(0, Road.Points.Num() - 1);
		switch (Random.RandRange(0, 9))
		{
		case 0:
		case 1:
		case 2:
			Road.Points[Point] += FVector3d(Random.FRandRange(-Move, Move), Random.FRand() < 0.2f ? Random.FRandRange(-3.0, 3.0) : 0.0, Random.FRandRange(-Move, Move));
			return FString::Printf(TEXT("point %d of %s moved"), Point, *Road.Name);
		case 3:
		{
			const FSplineRoad& Other = Splines[Random.RandRange(0, Splines.Num() - 1)];
			Road.Points[Point] = Other.Points[Random.RandRange(0, Other.Points.Num() - 1)];
			return FString::Printf(TEXT("point %d of %s onto %s"), Point, *Road.Name, *Other.Name);
		}
		case 4:
		{
			const FVector3d Centre = Road.Points[Point];
			const double Angle = Random.FRandRange(0.0, UE_DOUBLE_TWO_PI);
			const FVector3d Along(FMath::Cos(Angle) * 60.0, 0.0, FMath::Sin(Angle) * 60.0);
			FSplineRoad Added = CSRoadSplineTests_Road({ Centre - Along + FVector3d(Random.FRandRange(-20.0, 20.0), 0.0, Random.FRandRange(-20.0, 20.0)), Centre + Along });
			Added.Id = NextId++;
			Added.Prefab = Prefabs[Random.RandRange(0, Prefabs.Num() - 1)];
			Added.bAlley = Added.Prefab == TEXT("Alley");
			Added.Name = FString::Printf(TEXT("added %llu"), Added.Id);
			Splines.Add(MoveTemp(Added));
			return Splines.Last().Name + TEXT(" added");
		}
		case 5:
		{
			const FString Name = Road.Name;
			Splines.RemoveAt(Pick);
			return Name + TEXT(" removed");
		}
		case 6:
			Road.Prefab = Prefabs[Random.RandRange(0, Prefabs.Num() - 1)];
			Road.bAlley = Road.Prefab == TEXT("Alley");
			return FString::Printf(TEXT("%s now %s"), *Road.Name, *Road.Prefab);
		case 7:
		{
			Road.Data.SetNum(Road.Points.Num());
			FSplinePointData& Data = Road.Data[Point];
			Data.bSegmentElevation = !Data.bSegmentElevation;
			Data.SegmentElevation = float2(static_cast<float>(Random.FRandRange(0.0, 2.0)));
			return FString::Printf(TEXT("segment %d of %s with another elevation"), Point, *Road.Name);
		}
		case 8:
		{
			Road.Data.SetNum(Road.Points.Num());
			FSplinePointData& Data = Road.Data[Point];
			Data.NodePrefab = Data.NodePrefab.IsEmpty() ? Prefabs[0] : FString();
			return FString::Printf(TEXT("node %d of %s with another prefab"), Point, *Road.Name);
		}
		default:
			Road.bAdjust = !Road.bAdjust;
			return FString::Printf(TEXT("%s %s"), *Road.Name, Road.bAdjust ? TEXT("adjusted") : TEXT("fixed"));
		}
	};
	const FSplineNetworkSettings Settings;
	const TArray<FString> Prefabs = { TEXT("Small Road"), TEXT("Medium Road"), TEXT("Alley") };

	{
		TArray<FSplineRoad> Town;
		uint64 NextId = 1;
		auto Add = [&Town, &NextId](const TCHAR* Prefab, const TArray<FVector3d>& Points, bool bAdjust = true) -> FSplineRoad&
		{
			FSplineRoad& Road = Town.Add_GetRef(CSRoadSplineTests_Road(Points, bAdjust));
			Road.Id = NextId++;
			Road.Prefab = Prefab;
			Road.bAlley = Road.Prefab == TEXT("Alley");
			Road.Name = FString::Printf(TEXT("%s %llu"), Prefab, Road.Id);
			return Road;
		};
		FRandomStream Random(23);
		// The main grid, six roads each way 180 m apart, each in two splines meeting off the crossings; a street
		// from the middle of each block ending on the main road south of it.
		for (int32 I = 0; I < 6; I++)
		{
			const double Line = I * 180.0 - 450.0;
			Add(TEXT("Medium Road"), { FVector3d(-500.0, 0.0, Line), FVector3d(-250.0, 0.0, Line), FVector3d(0.0, 0.0, Line) });
			Add(TEXT("Medium Road"), { FVector3d(0.0, 0.0, Line), FVector3d(250.0, 0.0, Line), FVector3d(500.0, 0.0, Line) });
			Add(TEXT("Medium Road"), { FVector3d(Line, 0.0, -500.0), FVector3d(Line, 0.0, -250.0), FVector3d(Line, 0.0, 0.0) });
			Add(TEXT("Medium Road"), { FVector3d(Line, 0.0, 0.0), FVector3d(Line, 0.0, 250.0), FVector3d(Line, 0.0, 500.0) });
		}
		for (int32 I = 0; I < 5; I++)
		{
			for (int32 J = 0; J < 5; J++)
			{
				const double X = I * 180.0 - 360.0;
				const double Z = J * 180.0 - 360.0;
				Add(TEXT("Small Road"), { FVector3d(X + Random.FRandRange(-30.0, 30.0), 0.0, Z), FVector3d(X + Random.FRandRange(-40.0, 40.0), 0.0, Z - 90.0) });
			}
		}
		// Alleys across a main road, along one (3 m off its line) and inside a block.
		Add(TEXT("Alley"), { FVector3d(-320.0, 0.0, -300.0), FVector3d(-220.0, 0.0, -310.0) });
		Add(TEXT("Alley"), { FVector3d(-60.0, 0.0, -267.0), FVector3d(60.0, 0.0, -267.0) });
		Add(TEXT("Alley"), { FVector3d(100.0, 0.0, 100.0), FVector3d(160.0, 0.0, 130.0) });
		// A closed ring, three roads crossing at one point, a fixed spline across a main road, an overpass.
		{
			TArray<FVector3d> Ring;
			for (int32 K = 0; K < 8; K++) Ring.Add(FVector3d(180.0 + 40.0 * FMath::Cos(K * UE_DOUBLE_PI / 4.0), 0.0, 180.0 + 40.0 * FMath::Sin(K * UE_DOUBLE_PI / 4.0)));
			Add(TEXT("Small Road"), Ring).bClosed = true;
		}
		for (int32 K = 0; K < 3; K++)
		{
			const double Angle = K * UE_DOUBLE_PI / 3.0 + 0.1;
			const FVector3d Along(FMath::Cos(Angle) * 50.0, 0.0, FMath::Sin(Angle) * 50.0);
			Add(TEXT("Small Road"), { FVector3d(-180.0, 0.0, 200.0) - Along, FVector3d(-180.0, 0.0, 200.0) + Along });
		}
		Add(TEXT("Small Road"), { FVector3d(240.0, 0.0, -400.0), FVector3d(300.0, 0.0, -380.0) }, false);
		Add(TEXT("Medium Road"), { FVector3d(-400.0, 10.0, 400.0), FVector3d(-100.0, 10.0, 420.0) });

		FRun Run;
		Run.Name = TEXT("town");
		if (!Step(Run, TEXT("first build"), Town, Settings, false)) return false;
		{
			// Nothing changed, in another order: taken as it was.
			TArray<FSplineRoad> Shuffled = Town;
			for (int32 I = Shuffled.Num() - 1; I > 0; I--) Shuffled.Swap(I, Random.RandRange(0, I));
			Step(Run, TEXT("splines shuffled"), Shuffled, Settings, false);
			TestTrue(TEXT("town, splines shuffled: nothing converted, nothing in the diff"), !Run.Last.Stats.bWhole && Run.Last.Stats.Changed == 0 && Run.Cache.Diff.bValid
				&& Run.Cache.Diff.AddedRoads.Num() + Run.Cache.Diff.RemovedRoads.Num() + Run.Cache.Diff.AddedNodes.Num() + Run.Cache.Diff.RemovedNodes.Num() == 0);
		}
		for (int32 E = 0; E < 60; E++)
		{
			const FString Case = Edit(Town, Random, NextId, 30.0, Prefabs);
			Step(Run, FString::Printf(TEXT("edit %d (%s)"), E, *Case), Town, Settings, E % 20 == 5);
		}
		FSplineNetworkSettings Other = Settings;
		Other.SnapDistance = 1.5;
		Step(Run, TEXT("other settings"), Town, Other, false);
		TestTrue(TEXT("town, other settings: every spline converted"), Run.Last.Stats.bWhole);
		Report(Run);
		TestTrue(TEXT("town: most edits converted around the change"), Run.Around * 2 > Run.Builds);
	}

	const FString Path = CSRoadTest::PalmslandNetwork();
	if (!FPaths::FileExists(Path))
	{
		AddWarning(TEXT("Palmsland network missing (its run skipped)"));
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
	for (int32 S = 0; S < Splines.Num(); S++) Splines[S].Id = uint64(S) + 1;
	FRun Run;
	Run.Name = TEXT("palmsland");
	if (!Step(Run, TEXT("first build"), Splines, Settings, false)) return false;
	FRandomStream Random(5);
	uint64 NextId = uint64(Splines.Num()) + 1;
	for (int32 E = 0; E < 25; E++)
	{
		const FString Case = Edit(Splines, Random, NextId, 15.0, Prefabs);
		Step(Run, FString::Printf(TEXT("edit %d (%s)"), E, *Case), Splines, Settings, E == 10);
	}
	Report(Run);
	TestTrue(TEXT("palmsland: most edits converted around the change"), Run.Around * 2 > Run.Builds);
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
		Network->FinishWrites();
		TestEqual(Case + TEXT(": error"), Network->LastError, FString());
		TestEqual(Case + TEXT(": splines"), Network->NumSplines, 2);
		TestEqual(Case + TEXT(": roads"), Network->NumRoads, 4);
		TestTrue(Case + TEXT(": markings"), Network->NumMarkingInstances > 0 && Network->GetContent()->MarkingDecals.Num() > 0);
		TestTrue(Case + TEXT(": road mesh at identity"), Network->GetContent()->RoadMesh->GetComponentTransform().Equals(FTransform::Identity));
		for (const UInstancedStaticMeshComponent* Decals : Network->GetContent()->MarkingDecals) TestTrue(Case + TEXT(": decals at identity"), Decals && Decals->GetComponentTransform().Equals(FTransform::Identity));
		const FBox Bounds = Network->GetContent()->RoadMesh->Bounds.GetBox();
		for (const FVector& End : Ends) TestTrue(Case + TEXT(": bounds hold the road ends"), Bounds.IsInside(Network->GetActorTransform().TransformPosition(End)));
		TestTrue(Case + TEXT(": bounds centred on the crossing"), FVector::Dist2D(Bounds.GetCenter(), Network->GetActorLocation()) < 500.0);
	}

	// A spline edited (by the editor's spline tools, or code) has the network rebuild on its next tick.
	const FVector Moved(6000.0, 0.0, 0.0);
	Roads[0]->Spline->SetLocationAtSplinePoint(1, Moved, ESplineCoordinateSpace::Local);
	Network->Tick(0.0f);
	TestEqual(TEXT("edited: error"), Network->LastError, FString());
	TestTrue(TEXT("edited: bounds hold the moved end"), Network->GetContent()->RoadMesh->Bounds.GetBox().IsInside(Network->GetActorTransform().TransformPosition(Moved)));
	return true;
}

#endif

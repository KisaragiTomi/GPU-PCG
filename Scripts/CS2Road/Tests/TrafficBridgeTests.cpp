// Compile/run with RunTrafficBridgeTests.ps1. Exercises the production headers, not a mirror.
#include "CSRoadTrafficSimulation.h"
#include "CSRoadBridgePlacement.h"
#include <cstdio>
#include <cstdlib>
#include <chrono>

static int Checks = 0;
static void Check(bool Condition, const char* Message)
{
	++Checks;
	if (Condition) return;
	std::fprintf(stderr, "FAIL: %s\n", Message);
	std::exit(1);
}
static CSRoadTraffic::FLane Lane(float Length, bool Connector = false, int Junction = 0)
{
	CSRoadTraffic::FLane L;
	L.Length = Length;
	L.bConnector = Connector;
	L.Junction = Junction;
	return L;
}
static CSRoadTraffic::FCar Car(int Id, int OnLane, float Distance, int Next, float Speed = 0)
{
	CSRoadTraffic::FCar C;
	C.Id = Id;
	C.Lane = OnLane;
	C.Distance = Distance;
	C.Next = Next;
	C.Speed = Speed;
	return C;
}
static void Safe(const CSRoadTraffic::FSimulation& S)
{
	for (const auto& A : S.Cars)
	{
		Check(A.Lane >= 0 && A.Lane < int(S.Lanes.size()), "valid lane");
		Check(std::isfinite(A.Distance) && A.Distance >= 0 && A.Distance <= S.Lanes[A.Lane].Length + 0.001f, "finite bounded distance");
		for (const auto& B : S.Cars)
		{
			if (A.Id == B.Id) continue;
			if (A.Lane == B.Lane) Check(std::abs(A.Distance - B.Distance) >= CSRoadTraffic::CarLength + S.Gap - 0.002f, "no same-lane overlap");
			for (const auto& T : B.Tail) if (T.first == A.Lane) Check(S.Lanes[A.Lane].Length + T.second - A.Distance >= CSRoadTraffic::CarLength + S.Gap - 0.002f, "tail remains visible across boundary");
			if (S.Lanes[A.Lane].bConnector && S.Lanes[B.Lane].bConnector) Check(S.Lanes[A.Lane].Junction != S.Lanes[B.Lane].Junction, "exclusive junction reservation");
		}
	}
}

int main()
{
	using namespace CSRoadTraffic;
	FSimulation S;
	S.Lanes = {Lane(200)};
	S.Reset(99999, 42);
	Check(S.TargetPopulation == 4096 && S.Cars.size() <= 10, "population capped by capacity");
	Safe(S);
	S.Lanes = {Lane(200), Lane(200)};
	S.Lanes[1].bDriveable = false;
	S.Reset(100, 42);
	for (const auto& C : S.Cars) Check(C.Lane == 0, "restricted lanes excluded");

	FSimulation A, B;
	A.Lanes = B.Lanes = {Lane(2000)};
	A.Reset(10, 7);
	B.Reset(10, 7);
	for (int I = 0; I < 600; ++I) A.Advance(1.0 / 60);
	for (int I = 0; I < 300; ++I) B.Advance(1.0 / 30);
	Check(A.Steps == B.Steps && A.Cars.size() == B.Cars.size(), "frame-rate independent step count");
	for (int I = 0; I < int(A.Cars.size()); ++I) Check(A.Cars[I].Distance == B.Cars[I].Distance, "frame-rate independent trajectories");
	A.Advance(std::numeric_limits<double>::quiet_NaN());
	Check(A.Steps == B.Steps, "NaN delta ignored");
	A.Advance(10);
	Check(A.Steps - B.Steps <= 8, "hitch catch-up bounded");

	S.Lanes = {Lane(50), Lane(0.2f), Lane(200)};
	S.Lanes[0].Next = {1};
	S.Lanes[1].Next = {2};
	S.Lanes[2].SpeedLimit = 0;
	S.Reset(0, 7);
	S.Cars = {Car(0, 0, 20, 1, 13), Car(1, 2, 8, -1)};
	for (int I = 0; I < 900; ++I)
	{
		S.Advance(Step);
		Safe(S);
	}
	Check(S.Cars[0].Distance < 2 && S.Cars[0].Speed < 0.01f, "leader beyond short boundary stops follower");

	S.Lanes = {Lane(50), Lane(50), Lane(12, true, 1), Lane(12, true, 1), Lane(200), Lane(200)};
	S.Lanes[0].Next = {2};
	S.Lanes[1].Next = {3};
	S.Lanes[2].Next = {4};
	S.Lanes[3].Next = {5};
	S.Reset(0, 7);
	S.Cars = {Car(0, 0, 30, 2, 8), Car(1, 1, 30, 3, 8)};
	for (int I = 0; I < 1200; ++I)
	{
		S.Advance(Step);
		Safe(S);
	}
	Check(S.Transitions == 4 && S.Completed == 2, "both conflicting approaches eventually complete");

	S.Reset(0, 7);
	S.Lanes[4].SpeedLimit = 0;
	S.Cars = {Car(0, 0, 30, 2, 8), Car(1, 4, 3, -1)};
	for (int I = 0; I < 300; ++I) S.Advance(Step);
	Check(S.Cars[0].Lane == 0 && S.Cars[0].Distance <= 47.66f, "blocked exit holds car before stop line");
	S.Lanes[4].SpeedLimit = 13.8f;
	S.Lanes[2].SignalGroup = 1;
	S.Lanes[2].SignalGroups = 2;
	S.Reset(0, 7);
	S.Cars = {Car(0, 0, 30, 2, 8)};
	for (int I = 0; I < 270; ++I) S.Advance(Step);
	Check(S.Cars[0].Lane == 0 && !S.Green(2), "red and all-red stop traffic");
	for (int I = 0; I < 300; ++I) S.Advance(Step);
	Check(S.Transitions >= 2, "green releases waiting car");
	S.bSignals = false;
	Check(S.Green(2), "signal toggle honored");

	// Continuous arrivals at two merging approaches; repeat to catch boundary/tail regressions.
	S.Lanes = {Lane(120), Lane(120), Lane(3, true, 1), Lane(3, true, 1), Lane(100)};
	S.Lanes[0].Next = {2};
	S.Lanes[1].Next = {3};
	S.Lanes[2].Next = S.Lanes[3].Next = {4};
	S.Reset(20, 123);
	for (int I = 0; I < 7200; ++I)
	{
		S.Advance(Step);
		Safe(S);
	}
	Check(S.Completed > 20, "sustained merging flow and replenishment");

	using namespace CSRoadBridge;
	FDeck Bridge;
	Bridge.bElevated = true;
	Bridge.Points = {{-60, 12, 0}, {60, 12, 0}};
	int Skipped = 0;
	auto Flat = [](FPoint) { return 0.0f; };
	auto Piers = Build(std::vector<FDeck>{Bridge}, FSettings{}, Flat, Skipped);
	Check(Piers.size() == 4 && Skipped == 0, "regular elevated supports");
	for (const auto& P : Piers) Check(std::abs(P.Top.Y - 11.3f) < 0.001f && P.Ground == 0, "deck underside and ground anchoring");
	Bridge.bElevated = false;
	Check(Build(std::vector<FDeck>{Bridge}, FSettings{}, Flat, Skipped).empty(), "ground road has no piers");
	Bridge.bElevated = true;
	Check(Build(std::vector<FDeck>{Bridge}, FSettings{}, [](FPoint) { return 10.0f; }, Skipped).empty(), "low ramp has no stub pier");
	Bridge.Points = {{-18, 12, 0}, {18, 12, 0}};
	FDeck Underpass;
	Underpass.Width = 12;
	Underpass.Points = {{0, 0, -30}, {0, 0, 30}};
	std::vector<FDeck> Decks{Bridge, Underpass};
	Piers = Build(Decks, FSettings{}, Flat, Skipped);
	Check(Piers.size() == 1 && std::abs(Piers[0].Top.X) > 9, "pier shifts away from lower road");
	Check(!Obstructed(Piers[0].Top, 0, 2.13f, 0, Decks), "shifted footing clears entire roadway");
	Underpass.Width = 100;
	Check(Build(std::vector<FDeck>{Bridge, Underpass}, FSettings{}, Flat, Skipped).empty() && Skipped == 1, "blocked span reported instead of intersecting road");
	Bridge.Points = {{-60, 12, 0}, {0, 18, 20}, {60, 25, 0}};
	Piers = Build(std::vector<FDeck>{Bridge}, FSettings{}, [](FPoint P) { return 2 + P.X * 0.03f; }, Skipped);
	Check(Piers.size() >= 4, "curved sloped viaduct supported");
	for (const auto& P : Piers) Check(std::abs(P.Ground - (2 + P.Top.X * 0.03f)) < 0.001f && P.Top.Y > P.Ground + 3, "each support samples its own terrain");
	Piers = Build(std::vector<FDeck>{Bridge, Bridge}, FSettings{}, Flat, Skipped);
	for (int I = 0; I < int(Piers.size()); ++I) for (int J = I + 1; J < int(Piers.size()); ++J) Check(Distance(Piers[I].Top, Piers[J].Top) >= 5.9f, "duplicate supports suppressed");
	std::vector<FDeck> City;
	for (int I = 0; I < 6000; ++I)
	{
		FDeck D;
		D.bElevated = true;
		const float X = float(I % 100) * 140, Z = float(I / 100) * 40;
		D.Points = {{X, 12, Z}, {X + 100, 12, Z}};
		City.push_back(D);
	}
	const auto Start = std::chrono::steady_clock::now();
	Piers = Build(City, FSettings{}, Flat, Skipped);
	Check(Piers.size() == 18000 && Skipped == 0, "large network retains all valid supports");
	std::printf("Bridge layout: 6000 roads, 18000 piers, %.1f ms\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count());
	std::printf("PASS: %d checks (traffic progression, following, tails, merging, signals, timestep; bridge spacing, terrain, clearance).\n", Checks);
	return 0;
}

// Deterministic passenger-car preview. Metres/seconds; independent of rendering and UE so
// the exact simulation used by the actor can also be regression-tested without an editor.
// This is a lane-following preview, not CS2's citizen/destination/service-vehicle simulation.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace CSRoadTraffic
{
constexpr float CarLength = 4.2f;
constexpr double Step = 1.0 / 30.0;

struct FLane
{
	float Length = 0;
	float SpeedLimit = 13.888889f;
	bool bDriveable = true;
	bool bConnector = false;
	int Junction = 0;
	int SignalGroup = -1;
	int SignalGroups = 0;
	std::vector<int> Next;
};

struct FCar
{
	int Id = 0;
	int Lane = -1;
	int Next = -1;
	float Distance = 0;
	float Speed = 0;
	// Past lane boundaries stay occupied until the rear and its safety gap have cleared.
	std::vector<std::pair<int, float>> Tail;
};

class FSimulation
{
public:
	std::vector<FLane> Lanes;
	std::vector<FCar> Cars;
	float MaxSpeed = 13.888889f;
	float Gap = 2;
	bool bSignals = true;
	int TargetPopulation = 0;
	int Completed = 0;
	int Transitions = 0;
	std::uint64_t Steps = 0;

	void Reset(int Count, std::uint32_t Seed)
	{
		Cars.clear();
		Completed = Transitions = NextId = 0;
		Steps = 0;
		Remainder = SpawnTime = 0;
		Random = Seed ? Seed : 1;
		TargetPopulation = std::clamp(Count, 0, 4096);
		Gap = std::isfinite(Gap) ? std::clamp(Gap, 0.5f, 20.0f) : 2.0f;
		MaxSpeed = std::isfinite(MaxSpeed) ? std::clamp(MaxSpeed, 0.0f, 55.0f) : 13.888889f;
		for (int Round = 0; Round < 256 && int(Cars.size()) < TargetPopulation; ++Round)
		{
			bool bAdded = false;
			for (int I = 0; I < int(Lanes.size()) && int(Cars.size()) < TargetPopulation; ++I) bAdded |= Spawn(I, Separation() + Round * std::max(20.0f, Separation()));
			if (!bAdded) break;
		}
	}

	void Advance(double Seconds)
	{
		if (!std::isfinite(Seconds) || Seconds <= 0) return;
		// Bounded catch-up prevents an editor hitch from producing a large teleport.
		Remainder += std::min(Seconds, 0.25);
		while (Remainder + 1e-9 >= Step)
		{
			Tick();
			Remainder -= Step;
		}
	}

	bool Green(int Lane) const
	{
		const FLane& L = Lanes[Lane];
		if (!bSignals || L.SignalGroup < 0 || L.SignalGroups < 2) return true;
		// Each incoming road gets eight seconds, then two seconds of all-red clearance.
		const double Phase = std::fmod(Steps * Step, L.SignalGroups * 10.0);
		return int(Phase / 10.0) == L.SignalGroup && std::fmod(Phase, 10.0) < 8.0;
	}

private:
	struct FOccupant { int Car; float Distance; };
	std::vector<std::vector<FOccupant>> Occupancy;
	std::vector<std::pair<int, int>> Reservations;
	std::uint32_t Random = 1;
	int NextId = 0;
	double Remainder = 0;
	double SpawnTime = 0;

	float Separation() const { return CarLength + Gap; }
	bool Valid(int Lane) const { return Lane >= 0 && Lane < int(Lanes.size()) && Lanes[Lane].bDriveable && std::isfinite(Lanes[Lane].Length) && Lanes[Lane].Length > 0.01f; }
	int Choose(int Lane)
	{
		std::vector<int> Options;
		for (int Next : Lanes[Lane].Next) if (Valid(Next)) Options.push_back(Next);
		if (Options.empty()) return -1;
		Random ^= Random << 13;
		Random ^= Random >> 17;
		Random ^= Random << 5;
		return Options[Random % Options.size()];
	}
	bool Spawn(int Lane, float Distance)
	{
		if (!Valid(Lane) || Lanes[Lane].bConnector || Lanes[Lane].Length < Distance + Separation()) return false;
		for (const FCar& Car : Cars)
		{
			if (Car.Lane == Lane && std::abs(Car.Distance - Distance) < Separation() + 1) return false;
			for (const auto& Tail : Car.Tail) if (Tail.first == Lane && Lanes[Lane].Length + Tail.second - Distance < Separation() + 1) return false;
		}
		FCar Car;
		Car.Id = NextId++;
		Car.Lane = Lane;
		Car.Distance = Distance;
		Car.Next = Choose(Lane);
		Cars.push_back(Car);
		return true;
	}
	void Reserve(int Junction, int Car)
	{
		if (Junction > 0) Reservations.emplace_back(Junction, Car);
	}
	bool Free(int Junction, int Car) const
	{
		for (const auto& R : Reservations) if (R.first == Junction && R.second != Car) return false;
		return true;
	}
	float Leader(int Lane, float Distance, int Car) const
	{
		float Ahead = std::numeric_limits<float>::infinity();
		for (const FOccupant& Other : Occupancy[Lane]) if (Other.Car != Car && Other.Distance >= Distance - 0.001f) Ahead = std::min(Ahead, Other.Distance - Distance - Separation());
		return Ahead;
	}
	bool CanEnter(int Connector, int Car) const
	{
		const FLane& L = Lanes[Connector];
		if (!Green(Connector) || !Free(L.Junction, Car)) return false;
		// Never enter a junction whose outgoing lane cannot hold the entire car.
		if (L.Next.empty() || !Valid(L.Next.front())) return false;
		return Leader(L.Next.front(), 0, Car) >= Separation();
	}
	void Tick()
	{
		Occupancy.clear();
		Occupancy.resize(Lanes.size());
		Reservations.clear();
		for (const FCar& Car : Cars)
		{
			Occupancy[Car.Lane].push_back({Car.Id, Car.Distance});
			if (Lanes[Car.Lane].bConnector) Reserve(Lanes[Car.Lane].Junction, Car.Id);
			for (const auto& Tail : Car.Tail)
			{
				Occupancy[Tail.first].push_back({Car.Id, Lanes[Tail.first].Length + Tail.second});
				if (Lanes[Tail.first].bConnector) Reserve(Lanes[Tail.first].Junction, Car.Id);
			}
		}
		for (FCar& Car : Cars)
		{
			float Available = Leader(Car.Lane, Car.Distance, Car.Id);
			float SpeedLimit = std::min(MaxSpeed, Lanes[Car.Lane].SpeedLimit);
			float Offset = Lanes[Car.Lane].Length - Car.Distance;
			int Next = Car.Next;
			// Look ahead across boundaries for braking; connector exits have exactly one successor.
			for (int Depth = 0; Valid(Next) && Depth < 8; ++Depth)
			{
				const FLane& L = Lanes[Next];
				Available = std::min(Available, Offset + Leader(Next, 0, Car.Id));
				if (L.bConnector)
				{
					SpeedLimit = std::min(SpeedLimit, std::sqrt(L.SpeedLimit * L.SpeedLimit + 2 * 4.0f * std::max(0.0f, Offset)));
					if (!CanEnter(Next, Car.Id)) Available = std::min(Available, Offset - CarLength * 0.5f - 0.25f);
				}
				Offset += L.Length;
				if (L.Next.size() != 1) break;
				Next = L.Next.front();
			}
			Available = std::max(0.0f, Available);
			const float Desired = std::min(SpeedLimit, std::sqrt(2 * 4.0f * Available));
			Car.Speed = std::clamp(Desired, std::max(0.0f, Car.Speed - 6.0f * float(Step)), Car.Speed + 2.5f * float(Step));
			float Move = std::min(Car.Speed * float(Step), Available);
			Car.Speed = Move / float(Step);
			// Accumulate actual motion through every boundary, including very short connectors.
			while (Move > 0.000001f && Car.Lane >= 0)
			{
				const float Travel = std::min(Move, std::max(0.0f, Lanes[Car.Lane].Length - Car.Distance));
				Car.Distance += Travel;
				for (auto& Tail : Car.Tail) Tail.second += Travel;
				Move -= Travel;
				if (Car.Distance + 0.000001f < Lanes[Car.Lane].Length) break;
				if (!Valid(Car.Next))
				{
					Car.Lane = -1;
					++Completed;
					break;
				}
				if (Lanes[Car.Next].bConnector && !CanEnter(Car.Next, Car.Id))
				{
					Car.Speed = 0;
					break;
				}
				Car.Tail.emplace_back(Car.Lane, 0.0f);
				Car.Lane = Car.Next;
				Car.Distance = 0;
				Car.Next = Choose(Car.Lane);
				if (Lanes[Car.Lane].bConnector) Reserve(Lanes[Car.Lane].Junction, Car.Id);
				++Transitions;
			}
			Car.Tail.erase(std::remove_if(Car.Tail.begin(), Car.Tail.end(), [this](const auto& T) { return T.second >= Separation(); }), Car.Tail.end());
		}
		Cars.erase(std::remove_if(Cars.begin(), Cars.end(), [](const FCar& Car) { return Car.Lane < 0; }), Cars.end());
		++Steps;
		SpawnTime += Step;
		if (SpawnTime < 1) return;
		SpawnTime = 0;
		for (int I = 0; I < int(Lanes.size()) && int(Cars.size()) < TargetPopulation; ++I) Spawn(I, Separation());
	}
};
}

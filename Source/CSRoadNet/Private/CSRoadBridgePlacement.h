// Bridge-support layout in the road network's metre space (Y up). Shared by the UE
// adapter and the standalone regression tests; no terrain or renderer dependencies.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace CSRoadBridge
{
struct FPoint
{
	float X = 0, Y = 0, Z = 0;
};
inline FPoint Lerp(FPoint A, FPoint B, float T) { return {A.X + (B.X - A.X) * T, A.Y + (B.Y - A.Y) * T, A.Z + (B.Z - A.Z) * T}; }
inline float Distance(FPoint A, FPoint B) { return std::sqrt((A.X - B.X) * (A.X - B.X) + (A.Y - B.Y) * (A.Y - B.Y) + (A.Z - B.Z) * (A.Z - B.Z)); }

struct FDeck
{
	std::vector<FPoint> Points;
	float Width = 8;
	float Bottom = -0.7f;
	bool bElevated = false;
};
struct FPier
{
	FPoint Top, Direction;
	float Ground = 0;
	float Width = 8;
	int Road = -1;
};
struct FSettings
{
	float Spacing = 30;
	float MinHeight = 3;
	float Footprint = 3;
};

inline FPoint Sample(const FDeck& Deck, float At, FPoint& Direction)
{
	for (int I = 1; I < int(Deck.Points.size()); ++I)
	{
		const FPoint A = Deck.Points[I - 1], B = Deck.Points[I];
		const float Length = Distance(A, B);
		if (Length <= 1e-5f) continue;
		if (At <= Length || I + 1 == int(Deck.Points.size()))
		{
			Direction = {(B.X - A.X) / Length, (B.Y - A.Y) / Length, (B.Z - A.Z) / Length};
			return Lerp(A, B, std::clamp(At / Length, 0.0f, 1.0f));
		}
		At -= Length;
	}
	Direction = {1, 0, 0};
	return Deck.Points.empty() ? FPoint{} : Deck.Points.back();
}

inline bool ObstructedBy(FPoint Top, float Ground, float Radius, const FDeck& Other)
{
	for (int I = 1; I < int(Other.Points.size()); ++I)
	{
		const FPoint A = Other.Points[I - 1], B = Other.Points[I];
		const float X = B.X - A.X, Z = B.Z - A.Z;
		const float Denominator = X * X + Z * Z;
		const float T = Denominator > 1e-6f ? std::clamp(((Top.X - A.X) * X + (Top.Z - A.Z) * Z) / Denominator, 0.0f, 1.0f) : 0;
		const FPoint Near = Lerp(A, B, T);
		if (Near.Y < Ground - 0.5f || Near.Y > Top.Y + 0.1f) continue;
		const float Clearance = Other.Width * 0.5f + Radius + 1;
		if ((Top.X - Near.X) * (Top.X - Near.X) + (Top.Z - Near.Z) * (Top.Z - Near.Z) < Clearance * Clearance) return true;
	}
	return false;
}

inline bool Obstructed(FPoint Top, float Ground, float Radius, int Road, const std::vector<FDeck>& Decks)
{
	for (int R = 0; R < int(Decks.size()); ++R) if (R != Road && ObstructedBy(Top, Ground, Radius, Decks[R])) return true;
	return false;
}

template <typename TGround>
std::vector<FPier> Build(const std::vector<FDeck>& Decks, FSettings Settings, TGround GroundAt, int& Skipped)
{
	std::vector<FPier> Result;
	Skipped = 0;
	Settings.Spacing = std::isfinite(Settings.Spacing) ? std::clamp(Settings.Spacing, 8.0f, 100.0f) : 30.0f;
	Settings.MinHeight = std::isfinite(Settings.MinHeight) ? std::max(1.0f, Settings.MinHeight) : 3.0f;
	Settings.Footprint = std::isfinite(Settings.Footprint) ? std::clamp(Settings.Footprint, 0.5f, 10.0f) : 3.0f;
	// Query only nearby roads; a city snapshot can contain thousands of sampled edges.
	std::unordered_map<std::uint64_t, std::vector<int>> NearbyRoads;
	std::unordered_map<std::uint64_t, std::vector<int>> NearbyPiers;
	auto Key = [](int X, int Z) { return (std::uint64_t(std::uint32_t(X)) << 32) | std::uint32_t(Z); };
	auto Cell = [](float Value) { return int(std::floor(Value / 64.0f)); };
	auto PierCell = [Settings](float Value) { return int(std::floor(Value / (Settings.Footprint * 2))); };
	for (int R = 0; R < int(Decks.size()); ++R)
	{
		const FDeck& Deck = Decks[R];
		if (Deck.Points.empty()) continue;
		float MinX = Deck.Points[0].X, MaxX = MinX, MinZ = Deck.Points[0].Z, MaxZ = MinZ;
		for (FPoint P : Deck.Points)
		{
			MinX = std::min(MinX, P.X);
			MaxX = std::max(MaxX, P.X);
			MinZ = std::min(MinZ, P.Z);
			MaxZ = std::max(MaxZ, P.Z);
		}
		const float Radius = Deck.Width * 0.5f + Settings.Footprint * 0.71f + 1;
		for (int X = Cell(MinX - Radius); X <= Cell(MaxX + Radius); ++X) for (int Z = Cell(MinZ - Radius); Z <= Cell(MaxZ + Radius); ++Z) NearbyRoads[Key(X, Z)].push_back(R);
	}
	for (int R = 0; R < int(Decks.size()); ++R)
	{
		const FDeck& Deck = Decks[R];
		if (!Deck.bElevated || Deck.Points.size() < 2) continue;
		float Length = 0;
		for (int I = 1; I < int(Deck.Points.size()); ++I) Length += Distance(Deck.Points[I - 1], Deck.Points[I]);
		// Keep foundations away from the road's junction surfaces and ramp ends.
		const float Margin = std::max(5.0f, Deck.Width * 0.5f);
		if (!std::isfinite(Length) || Length < 2 * Margin) continue;
		const int Count = std::max(1, int(std::ceil((Length - 2 * Margin) / Settings.Spacing)));
		const float Span = (Length - 2 * Margin) / Count;
		for (int I = 0; I < Count; ++I)
		{
			bool bPlaced = false;
			for (float Shift : {0.0f, -0.25f, 0.25f, -0.45f, 0.45f})
			{
				FPoint Direction;
				FPoint Top = Sample(Deck, Margin + (I + 0.5f + Shift) * Span, Direction);
				Top.Y += Deck.Bottom;
				const float Ground = GroundAt(Top);
				if (!std::isfinite(Ground) || !std::isfinite(Top.Y) || Top.Y - Ground < Settings.MinHeight) continue;
				bool bObstructed = false;
				const auto Nearby = NearbyRoads.find(Key(Cell(Top.X), Cell(Top.Z)));
				if (Nearby != NearbyRoads.end()) for (int Other : Nearby->second) if (Other != R && ObstructedBy(Top, Ground, Settings.Footprint * 0.71f, Decks[Other])) bObstructed = true;
				if (bObstructed) continue;
				bool bDuplicate = false;
				const int PX = PierCell(Top.X), PZ = PierCell(Top.Z);
				for (int X = PX - 1; X <= PX + 1; ++X) for (int Z = PZ - 1; Z <= PZ + 1; ++Z)
				{
					const auto NearbyPier = NearbyPiers.find(Key(X, Z));
					if (NearbyPier == NearbyPiers.end()) continue;
					for (int Index : NearbyPier->second)
					{
						const FPier& Existing = Result[Index];
						const float DX = Top.X - Existing.Top.X, DZ = Top.Z - Existing.Top.Z;
						if (DX * DX + DZ * DZ < Settings.Footprint * Settings.Footprint * 4) bDuplicate = true;
					}
				}
				if (bDuplicate) continue;
				NearbyPiers[Key(PX, PZ)].push_back(int(Result.size()));
				Result.push_back({Top, Direction, Ground, Deck.Width, R});
				bPlaced = true;
				break;
			}
			if (!bPlaced) ++Skipped;
		}
	}
	return Result;
}
}

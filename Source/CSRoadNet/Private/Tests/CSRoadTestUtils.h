// Shared helpers for the golden-data tests. Golden files come from
// Scripts/CS2Road/GoldenDump (C# RoadLab port) and live in <Project>/Saved/CSRoadGolden.
#pragma once

#include "CSRoadGraph.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

namespace CSRoadTest
{
// CitiesSkylines-Road2-C# checkout (env CS2ROADLAB overrides the default path).
FString LabRoot();
FString GoldenDir();

struct FLibrary
{
	TArray<uint8> Prefabs;
	TArray<uint8> Raw;
	TArray64<uint8> Pieces;
	CSRoad::FAssets Assets;
	bool bLoaded = false;
	FString Error;
};

// Loaded once per process from <Lab>/Assets/CS2.
FLibrary& Library();

TSharedPtr<FJsonObject> LoadJson(const FString& Path);
bool LoadGolden(const FString& Id, TSharedPtr<FJsonObject>& OutJson, TArray64<uint8>& OutBin);
TArray<FString> GoldenScenarioIds();
TArray<double> Numbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
TArray<double> Numbers(const TArray<TSharedPtr<FJsonValue>>& Values);

// Element-wise comparison with an absolute tolerance scaled by magnitude; logs the first mismatches.
struct FCompare
{
	FAutomationTestBase& Test;
	int32 Values = 0;
	int32 Mismatches = 0;
	double MaxError = 0.0;
	int32 Logged = 0;

	explicit FCompare(FAutomationTestBase& InTest) : Test(InTest) {}
	bool Arrays(const FString& Label, const TArray<double>& Mine, const TArray<double>& Golden, double Tolerance, const TSet<int32>& Masked = TSet<int32>());
	bool Equal(const FString& Label, int64 Mine, int64 Golden);
};

// The GPU stages are not compared bitwise. The geometry jobs are not stable under rounding:
// GoldenDump --perturb (every input x/z moved by at most one ulp) moves the C# reference itself
// by up to 2.7 m in the scenarios and 7.7 m on Palmsland, with 0.62% / 0.20% of the values beyond
// 1 cm, and symmetric scenarios sit on exact ties that the GPU and .NET break differently. So
// values must agree within Agreement, and only a budgeted share may fall outside.
struct FBudgetCompare
{
	double Agreement = 0.01;
	double Outlier = 0.1;
	double AgreementBudget = 0.0025;
	double OutlierBudget = 0.001;
	int64 Values = 0;
	int64 Disagreements = 0;
	int64 Outliers = 0;
	double MaxError = 0.0;
	FString MaxLabel;
	TArray<FString> OutlierLabels;
	bool bShapeMismatch = false;

	void Add(FAutomationTestBase& Test, const FString& Label, const TArray<double>& Mine, const TArray<double>& Golden);
	// Logs the summary; false when a budget is exceeded or a shape differed.
	bool Report(FAutomationTestBase& Test, const FString& Name, double Milliseconds) const;
};

// Non-source specs (scenario graphs) rebuilt from the golden edges: prefab + curve control points.
TArray<CSRoad::FRoadSpec> ScenarioSpecs(const TSharedPtr<FJsonObject>& Golden);
}

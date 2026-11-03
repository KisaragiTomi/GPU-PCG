// Shared helpers for the golden-data tests. The golden files were exported from the C# RoadLab port
// (CitiesSkylines-Road2-C#, which no longer changes) and live in the plugin's Data/CS2Road/Golden.
#pragma once

#include "CSRoadGpuGeometry.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadNetworkContent.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"

namespace CSRoadTest
{
// The CS2 data in the plugin (CSRoadDataDir): prefabs.json, raw-prefabs.json and pieces.bin, and the
// Palmsland snapshot. Env CS2ROADLAB takes them from a CitiesSkylines-Road2-C# checkout instead.
FString PrefabDir();
FString PalmslandNetwork();
// Env CS2ROADGOLDEN selects another export.
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

// Loaded once per process from PrefabDir.
FLibrary& Library();

TSharedPtr<FJsonObject> LoadJson(const FString& Path);
bool LoadGolden(const FString& Id, TSharedPtr<FJsonObject>& OutJson, TArray64<uint8>& OutBin);
TArray<FString> GoldenScenarioIds();
TArray<double> Numbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field);
TArray<double> Numbers(const TArray<TSharedPtr<FJsonValue>>& Values);

// Ticks the network as the editor's frames would until the build a tick starts has ended (a build waits
// for the GPU between ticks), then lands the meshes it wrote without waiting.
void TickBuild(ACSRoadNetworkActor* Network);

// The components of a class the network has built (on its content actor; none before it built).
template <class T>
TArray<T*> Built(const ACSRoadNetworkActor* Network)
{
	TArray<T*> Out;
	if (const ACSRoadNetworkContent* Content = Network->GetContent()) Content->GetComponents<T>(Out);
	return Out;
}

// The eight corners of every batch building piece (an instance of its tile component's mesh), world space.
TArray<FVector3f> BuildingCorners(const ACSRoadNetworkActor* Network);

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
// an export with every input x/z moved by at most one ulp moves the C# reference itself
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

// The golden edge / start / end node geometry of every road of Graph (built from the same golden),
// for the CPU stages that read the geometry back (lanes, crosswalks, decal items).
bool GoldenGeometry(const TSharedPtr<FJsonObject>& Golden, const CSRoad::FGraph& Graph, CSRoad::FGpuGeometryResult& Out);
}

#include "CSRoadTestUtils.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace CSRoadTest
{
FString LabRoot()
{
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("CS2ROADLAB"));
	return Env.IsEmpty() ? FString(TEXT("D:/MyProject/CitiesSkylines-Road2-C#")) : Env;
}

FString GoldenDir()
{
	// CS2ROADGOLDEN selects another export, e.g. a GoldenDump --perturb run.
	const FString Env = FPlatformMisc::GetEnvironmentVariable(TEXT("CS2ROADGOLDEN"));
	return Env.IsEmpty() ? FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("CSRoadGolden")) : Env;
}

FLibrary& Library()
{
	static FLibrary Lib;
	if (Lib.bLoaded || !Lib.Error.IsEmpty()) return Lib;
	const FString Dir = FPaths::Combine(LabRoot(), TEXT("Assets"), TEXT("CS2"));
	if (!FFileHelper::LoadFileToArray(Lib.Prefabs, *FPaths::Combine(Dir, TEXT("prefabs.json")))
		|| !FFileHelper::LoadFileToArray(Lib.Raw, *FPaths::Combine(Dir, TEXT("raw-prefabs.json")))
		|| !FFileHelper::LoadFileToArray(Lib.Pieces, *FPaths::Combine(Dir, TEXT("pieces.bin"))))
	{
		Lib.Error = FString::Printf(TEXT("CS2 road assets not found under %s"), *Dir);
		return Lib;
	}
	Lib.bLoaded = Lib.Assets.Load(Lib.Prefabs, Lib.Raw, &Lib.Pieces, Lib.Error);
	return Lib;
}

TSharedPtr<FJsonObject> LoadJson(const FString& Path)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path)) return nullptr;
	TSharedPtr<FJsonObject> Out;
	return FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Out) ? Out : nullptr;
}

bool LoadGolden(const FString& Id, TSharedPtr<FJsonObject>& OutJson, TArray64<uint8>& OutBin)
{
	OutJson = LoadJson(FPaths::Combine(GoldenDir(), Id + TEXT(".json")));
	OutBin.Reset();
	FFileHelper::LoadFileToArray(OutBin, *FPaths::Combine(GoldenDir(), Id + TEXT(".bin")));
	return OutJson.IsValid();
}

TArray<FString> GoldenScenarioIds()
{
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(GoldenDir(), TEXT("*.json")), true, false);
	TArray<FString> Ids;
	for (const FString& File : Files)
	{
		const FString Id = FPaths::GetBaseFilename(File);
		if (Id != TEXT("layouts") && Id != TEXT("palmsland")) Ids.Add(Id);
	}
	Ids.Sort();
	return Ids;
}

TArray<double> Numbers(const TArray<TSharedPtr<FJsonValue>>& Values)
{
	TArray<double> Out;
	Out.Reserve(Values.Num());
	for (const TSharedPtr<FJsonValue>& V : Values) Out.Add(V->AsNumber());
	return Out;
}

TArray<double> Numbers(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	return Object.IsValid() && Object->TryGetArrayField(Field, Values) ? Numbers(*Values) : TArray<double>();
}

bool FCompare::Arrays(const FString& Label, const TArray<double>& Mine, const TArray<double>& Golden, double Tolerance, const TSet<int32>& Masked)
{
	if (Mine.Num() != Golden.Num())
	{
		Mismatches++;
		if (Logged++ < 40) Test.AddError(FString::Printf(TEXT("%s: %d values, golden has %d"), *Label, Mine.Num(), Golden.Num()));
		return false;
	}
	bool bOk = true;
	for (int32 I = 0; I < Mine.Num(); I++)
	{
		Values++;
		if (Masked.Contains(I)) continue;
		const double Error = FMath::Abs(Mine[I] - Golden[I]);
		const bool bBothNan = FMath::IsNaN(Mine[I]) && FMath::IsNaN(Golden[I]);
		MaxError = FMath::Max(MaxError, bBothNan ? 0.0 : Error);
		if (!bBothNan && !(Error <= Tolerance * FMath::Max(1.0, FMath::Abs(Golden[I]))))
		{
			Mismatches++;
			bOk = false;
			if (Logged++ < 40) Test.AddError(FString::Printf(TEXT("%s[%d]: %.9g, golden %.9g"), *Label, I, Mine[I], Golden[I]));
		}
	}
	return bOk;
}

bool FCompare::Equal(const FString& Label, int64 Mine, int64 Golden)
{
	Values++;
	if (Mine == Golden) return true;
	Mismatches++;
	if (Logged++ < 40) Test.AddError(FString::Printf(TEXT("%s: %lld, golden %lld"), *Label, Mine, Golden));
	return false;
}

void FBudgetCompare::Add(FAutomationTestBase& Test, const FString& Label, const TArray<double>& Mine, const TArray<double>& Golden)
{
	if (Mine.Num() != Golden.Num())
	{
		Test.AddError(FString::Printf(TEXT("%s: %d values, golden has %d"), *Label, Mine.Num(), Golden.Num()));
		bShapeMismatch = true;
		return;
	}
	double Worst = 0.0;
	int32 WorstIndex = 0;
	for (int32 I = 0; I < Mine.Num(); I++)
	{
		const double Error = FMath::Abs(Mine[I] - Golden[I]);
		Values++;
		if (Error > Agreement) Disagreements++;
		if (Error > Outlier) Outliers++;
		if (Error > Worst)
		{
			Worst = Error;
			WorstIndex = I;
		}
	}
	if (Worst > Outlier && OutlierLabels.Num() < 12) OutlierLabels.Add(FString::Printf(TEXT("%s[%d] %.4g vs %.4g"), *Label, WorstIndex, Mine[WorstIndex], Golden[WorstIndex]));
	if (Worst > MaxError)
	{
		MaxError = Worst;
		MaxLabel = FString::Printf(TEXT("%s[%d]"), *Label, WorstIndex);
	}
}

bool FBudgetCompare::Report(FAutomationTestBase& Test, const FString& Name, double Milliseconds) const
{
	const double Total = FMath::Max<double>(1.0, double(Values));
	Test.AddInfo(FString::Printf(TEXT("%s: %lld values, %lld beyond %g (%.3f%%), %lld beyond %g, max error %.3g at %s, %.1f ms"), *Name, Values, Disagreements, Agreement,
		100.0 * double(Disagreements) / Total, Outliers, Outlier, MaxError, *MaxLabel, Milliseconds));
	for (const FString& Label : OutlierLabels) Test.AddInfo(TEXT("  outlier ") + Label);
	bool bOk = !bShapeMismatch;
	if (double(Disagreements) > AgreementBudget * Total)
	{
		Test.AddError(FString::Printf(TEXT("%s: %.3f%% of the values differ by more than %g (budget %.2f%%)"), *Name, 100.0 * double(Disagreements) / Total, Agreement,
			100.0 * AgreementBudget));
		bOk = false;
	}
	if (double(Outliers) > OutlierBudget * Total)
	{
		Test.AddError(FString::Printf(TEXT("%s: %lld values differ by more than %g (budget %.2f%%)"), *Name, Outliers, Outlier, 100.0 * OutlierBudget));
		bOk = false;
	}
	return bOk;
}

TArray<CSRoad::FRoadSpec> ScenarioSpecs(const TSharedPtr<FJsonObject>& Golden)
{
	TArray<CSRoad::FRoadSpec> Specs;
	for (const TSharedPtr<FJsonValue>& Value : Golden->GetArrayField(TEXT("edges")))
	{
		const TSharedPtr<FJsonObject> E = Value->AsObject();
		const TArray<double> C = Numbers(E, TEXT("curve"));
		CSRoad::FRoadSpec Spec;
		Spec.Prefab = E->GetStringField(TEXT("prefab"));
		auto P = [&C](int32 I) { return CSRoad::float3(static_cast<float>(C[I]), static_cast<float>(C[I + 1]), static_cast<float>(C[I + 2])); };
		Spec.Curve = CSRoad::Bezier4x3(P(0), P(3), P(6), P(9));
		Specs.Add(Spec);
	}
	return Specs;
}

namespace
{
// A flattened all-float golden struct back into its C++ layout.
template<typename T>
bool CSRoadTestUtils_Unflatten(const TArray<double>& Values, T& Out)
{
	if (Values.Num() * int32(sizeof(float)) != int32(sizeof(T))) return false;
	float* Floats = reinterpret_cast<float*>(&Out);
	for (int32 I = 0; I < Values.Num(); I++) Floats[I] = static_cast<float>(Values[I]);
	return true;
}
}

bool GoldenGeometry(const TSharedPtr<FJsonObject>& Golden, const CSRoad::FGraph& Graph, CSRoad::FGpuGeometryResult& Out)
{
	const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
	if (Edges.Num() != Graph.Roads.Num()) return false;
	Out.Edges.SetNumZeroed(Graph.NumEntities());
	Out.StartNodes.SetNumZeroed(Graph.NumEntities());
	Out.EndNodes.SetNumZeroed(Graph.NumEntities());
	for (int32 Road = 0; Road < Edges.Num(); Road++)
	{
		const TSharedPtr<FJsonObject> E = Edges[Road]->AsObject();
		const int32 Entity = Graph.Roads[Road].Entity;
		if (!CSRoadTestUtils_Unflatten(Numbers(E, TEXT("edgeGeometry")), Out.Edges[Entity]) || !CSRoadTestUtils_Unflatten(Numbers(E, TEXT("startGeometry")), Out.StartNodes[Entity])
			|| !CSRoadTestUtils_Unflatten(Numbers(E, TEXT("endGeometry")), Out.EndNodes[Entity]))
		{
			return false;
		}
	}
	return true;
}
}

#endif

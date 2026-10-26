#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadCompositionMesh.h"
#include "CSRoadNetworkFile.h"
#include "CSRoadTestUtils.h"

using namespace CSRoad;

namespace
{
// NetCompositionData.m_State (flatten index 21): the port adds CompositionState.BlockZone
// like the original NetCompositionSystem; the C# RoadLab port does not.
TArray<double> CSRoadCompositionTests_Data(const NetCompositionData& Data)
{
	NetCompositionData Copy = Data;
	Copy.m_State &= ~CompositionState_BlockZone;
	TArray<double> Out;
	CSRoadFlatten(Copy, Out);
	return Out;
}

template<typename T>
TArray<double> CSRoadCompositionTests_All(const TArray<T>& Items)
{
	TArray<double> Out;
	for (const T& Item : Items) CSRoadFlatten(Item, Out);
	return Out;
}

TArray<double> CSRoadCompositionTests_GoldenAll(const TSharedPtr<FJsonObject>& Composition, const TCHAR* Field)
{
	TArray<double> Out;
	for (const TSharedPtr<FJsonValue>& Item : Composition->GetArrayField(Field)) Out.Append(CSRoadTest::Numbers(Item->AsArray()));
	return Out;
}

void CSRoadCompositionTests_CompareComposition(CSRoadTest::FCompare& Compare, FAssets& Assets, const FString& Label, const FComposition& Mine,
	const TSharedPtr<FJsonObject>& Golden, const TArray64<uint8>& Bin, bool bMesh)
{
	Compare.Arrays(Label + TEXT(".data"), CSRoadCompositionTests_Data(Mine.Data), CSRoadTest::Numbers(Golden, TEXT("data")), 1e-6);
	Compare.Arrays(Label + TEXT(".pieces"), CSRoadCompositionTests_All(Mine.Pieces), CSRoadCompositionTests_GoldenAll(Golden, TEXT("pieces")), 1e-6);
	Compare.Arrays(Label + TEXT(".lanes"), CSRoadCompositionTests_All(Mine.Lanes), CSRoadCompositionTests_GoldenAll(Golden, TEXT("lanes")), 1e-6);
	Compare.Arrays(Label + TEXT(".carriageways"), CSRoadCompositionTests_All(Mine.Carriageways), CSRoadCompositionTests_GoldenAll(Golden, TEXT("carriageways")), 1e-6);
	Compare.Arrays(Label + TEXT(".crosswalks"), CSRoadCompositionTests_All(Mine.Crosswalks), CSRoadCompositionTests_GoldenAll(Golden, TEXT("crosswalks")), 1e-6);
	const TSharedPtr<FJsonObject>* MeshJson = nullptr;
	if (!bMesh || !Golden->TryGetObjectField(TEXT("mesh"), MeshJson)) return;
	FCompositionMesh Mesh;
	GenerateCompositionMesh(Assets, Mine, Mesh);
	const int32 VertexCount = static_cast<int32>((*MeshJson)->GetNumberField(TEXT("vertexCount")));
	const int32 IndexCount = static_cast<int32>((*MeshJson)->GetNumberField(TEXT("indexCount")));
	if (!Compare.Equal(Label + TEXT(".vertexCount"), Mesh.Vertices.Num(), VertexCount) || !Compare.Equal(Label + TEXT(".indexCount"), Mesh.Indices.Num(), IndexCount)) return;
	const TArray<TSharedPtr<FJsonValue>>& Materials = (*MeshJson)->GetArrayField(TEXT("materials"));
	if (Compare.Equal(Label + TEXT(".materials"), Mesh.Materials.Num(), Materials.Num()))
	{
		for (int32 I = 0; I < Materials.Num(); I++)
		{
			const TSharedPtr<FJsonObject> M = Materials[I]->AsObject();
			const MeshMaterial& Mat = Mesh.Materials[I];
			Compare.Equal(FString::Printf(TEXT("%s.material%d.start"), *Label, I), Mat.m_StartIndex, static_cast<int64>(M->GetNumberField(TEXT("start"))));
			Compare.Equal(FString::Printf(TEXT("%s.material%d.count"), *Label, I), Mat.m_IndexCount, static_cast<int64>(M->GetNumberField(TEXT("count"))));
			Compare.Equal(FString::Printf(TEXT("%s.material%d.startVertex"), *Label, I), Mat.m_StartVertex, static_cast<int64>(M->GetNumberField(TEXT("startVertex"))));
			Compare.Equal(FString::Printf(TEXT("%s.material%d.vertexCount"), *Label, I), Mat.m_VertexCount, static_cast<int64>(M->GetNumberField(TEXT("vertexCount"))));
			if (Assets.Atlases()[Mat.m_MaterialIndex] != M->GetStringField(TEXT("atlas"))) Compare.Equal(FString::Printf(TEXT("%s.material%d.atlas(%s vs %s)"), *Label, I, *Assets.Atlases()[Mat.m_MaterialIndex], *M->GetStringField(TEXT("atlas"))), 0, 1);
		}
	}
	// Raw VertexData: float3 position, uint normal, uint tangent, Color32, half4 UV0 (32 bytes).
	const int64 VertexOffset = static_cast<int64>((*MeshJson)->GetNumberField(TEXT("vertices")));
	const int64 IndexOffset = static_cast<int64>((*MeshJson)->GetNumberField(TEXT("indices")));
	const uint8* Data = Bin.GetData();
	TArray<double> MineVertices;
	TArray<double> GoldenVertices;
	for (int32 V = 0; V < VertexCount; V++)
	{
		const uint8* G = Data + VertexOffset + int64(V) * 32;
		float Position[3];
		uint32 Packed[3];
		uint16 Half[4];
		FMemory::Memcpy(Position, G, 12);
		FMemory::Memcpy(Packed, G + 12, 12);
		FMemory::Memcpy(Half, G + 24, 8);
		const FCompositionVertex& M = Mesh.Vertices[V];
		GoldenVertices.Append({ Position[0], Position[1], Position[2], double(Packed[0]), double(Packed[1]), double(Packed[2]), double(Half[0]), double(Half[1]), double(Half[2]), double(Half[3]) });
		MineVertices.Append({ M.Position.x, M.Position.y, M.Position.z, double(M.Normal), double(M.Tangent), double(M.Color),
			double(F32ToF16(M.UV0.x)), double(F32ToF16(M.UV0.y)), double(F32ToF16(M.UV0.z)), double(F32ToF16(M.UV0.w)) });
	}
	Compare.Arrays(Label + TEXT(".vertices"), MineVertices, GoldenVertices, 0.0);
	TArray<double> MineIndices;
	TArray<double> GoldenIndices;
	for (int32 I = 0; I < IndexCount; I++)
	{
		int32 Index = 0;
		FMemory::Memcpy(&Index, Data + IndexOffset + int64(I) * 4, 4);
		GoldenIndices.Add(Index);
		MineIndices.Add(Mesh.Indices[I]);
	}
	Compare.Arrays(Label + TEXT(".indices"), MineIndices, GoldenIndices, 0.0);
}

bool CSRoadCompositionTests_Run(FAutomationTestBase& Test, CSRoadTest::FCompare& Compare, const FString& Id, const TArray<FRoadSpec>& Specs,
	const TArray<FSavedNode>& Nodes, bool bLeftHand, bool bMesh)
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
	if (!Graph.Build(Lib.Assets, Specs, Nodes, Error))
	{
		Test.AddError(FString::Printf(TEXT("%s: %s"), *Id, *Error));
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>& Edges = Golden->GetArrayField(TEXT("edges"));
	const TArray<TSharedPtr<FJsonValue>>& Compositions = Golden->GetArrayField(TEXT("compositions"));
	if (!Compare.Equal(Id + TEXT(".roads"), Graph.Roads.Num(), Edges.Num())) return false;
	TSet<TPair<int32, const FComposition*>> Seen;
	for (int32 I = 0; I < Edges.Num(); I++)
	{
		const TSharedPtr<FJsonObject> E = Edges[I]->AsObject();
		const FRoadInstance& Road = Graph.Roads[I];
		const FString Label = FString::Printf(TEXT("%s.road%d"), *Id, I);
		TArray<double> Flags;
		CSRoadFlatten(Road.EdgeComposition->Data.m_Flags, Flags);
		Compare.Arrays(Label + TEXT(".edgeFlags"), Flags, CSRoadTest::Numbers(E, TEXT("edgeFlags")), 0.0);
		Flags.Reset();
		CSRoadFlatten(Road.StartComposition->Data.m_Flags, Flags);
		Compare.Arrays(Label + TEXT(".startFlags"), Flags, CSRoadTest::Numbers(E, TEXT("startFlags")), 0.0);
		Flags.Reset();
		CSRoadFlatten(Road.EndComposition->Data.m_Flags, Flags);
		Compare.Arrays(Label + TEXT(".endFlags"), Flags, CSRoadTest::Numbers(E, TEXT("endFlags")), 0.0);
		const TPair<const TCHAR*, const FComposition*> Instances[] = {
			{ TEXT("edgeInstance"), Road.EdgeComposition }, { TEXT("startInstance"), Road.StartComposition }, { TEXT("endInstance"), Road.EndComposition } };
		for (const TPair<const TCHAR*, const FComposition*>& Instance : Instances)
		{
			const int32 GoldenIndex = static_cast<int32>(E->GetObjectField(Instance.Key)->GetNumberField(TEXT("composition")));
			if (Seen.Contains({ GoldenIndex, Instance.Value })) continue;
			Seen.Add({ GoldenIndex, Instance.Value });
			CSRoadCompositionTests_CompareComposition(Compare, Lib.Assets, FString::Printf(TEXT("%s.%s"), *Label, Instance.Key), *Instance.Value,
				Compositions[GoldenIndex]->AsObject(), Bin, bMesh);
		}
	}
	return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadCompositionGoldenTest, "PCGPlugins.CSRoadNet.Composition.Scenarios",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadCompositionGoldenTest::RunTest(const FString& Parameters)
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
	for (const FString& Id : Ids)
	{
		TSharedPtr<FJsonObject> Golden;
		TArray64<uint8> Bin;
		CSRoadTest::LoadGolden(Id, Golden, Bin);
		CSRoadCompositionTests_Run(*this, Compare, Id, CSRoadTest::ScenarioSpecs(Golden), TArray<FSavedNode>(), Golden->GetBoolField(TEXT("leftHand")), true);
	}
	AddInfo(FString::Printf(TEXT("%d scenarios, %d values, %d mismatches, max error %.3g"), Ids.Num(), Compare.Values, Compare.Mismatches, Compare.MaxError));
	return Compare.Mismatches == 0;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadCompositionPalmslandTest, "PCGPlugins.CSRoadNet.Composition.Palmsland",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSRoadCompositionPalmslandTest::RunTest(const FString& Parameters)
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
	if (!Network.Load(Path, &Lib.Assets, false, Error))
	{
		AddError(Error);
		return false;
	}
	CSRoadTest::FCompare Compare(*this);
	CSRoadCompositionTests_Run(*this, Compare, TEXT("palmsland"), Network.Roads, Network.Nodes, Network.bLeftHandTraffic, false);
	AddInfo(FString::Printf(TEXT("%d roads, %d values, %d mismatches, max error %.3g"), Network.Roads.Num(), Compare.Values, Compare.Mismatches, Compare.MaxError));
	return Compare.Mismatches == 0;
}

#endif

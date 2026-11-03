#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSRoadNet.h"
#include "CSRoadNetworkActor.h"
#include "CSRoadSplineActor.h"
#include "CSRoadTestUtils.h"
#include "Dom/JsonObject.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Tests/AutomationEditorCommon.h"

// The plugin's custom roads (Data/CustomRoads.json) on the CS2 prefabs: "Alley 2m" is Alley with only its 1 m shoulders
// either side of its median, so 2 m wide, without zoning, but with plots of its own half a metre from its edge and 9 m
// along it. At a junction with a road that has sidewalks its shoulders become ramps up to them, which join the sidewalks'
// corners (a lane of only a drive section had nothing to do that, and the ground showed there). A 2 m alley T-joined to
// two alleys builds into five roads without an error, and with batch buildings into plots along it too. The borrowed
// form (a section of another road, a flag set) builds what it says.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCSRoadCustomRoadsTest, "PCGPlugins.CSRoadNet.Assets.CustomRoads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSRoadCustomRoadsTest::RunTest(const FString& Parameters)
{
	CSRoadTest::FLibrary& Lib = CSRoadTest::Library();
	if (!Lib.bLoaded)
	{
		AddWarning(Lib.Error + TEXT(" (skipped)"));
		return true;
	}
	TArray<uint8> Custom;
	if (!TestTrue(TEXT("Data/CustomRoads.json read"), FFileHelper::LoadFileToArray(Custom, *FPaths::Combine(FPaths::GetPath(CSRoadDataDir()), TEXT("CustomRoads.json"))))) return false;
	CSRoad::FAssets Assets;
	FString Error;
	if (!TestTrue(TEXT("assets with the custom roads loaded"), Assets.Load(Lib.Prefabs, Lib.Raw, &Lib.Pieces, Error, &Custom))) return false;
	if (!TestTrue(TEXT("Alley 2m is a road"), Assets.HasRoad(TEXT("Alley 2m")))) return false;
	TestEqual(TEXT("Alley 2m width (m)"), Assets.Geometry(TEXT("Alley 2m")).m_DefaultWidth, 2.0f, 0.01f);
	TestEqual(TEXT("Alley width (m)"), Assets.Geometry(TEXT("Alley")).m_DefaultWidth, 8.0f, 0.01f);
	const uint32 Base = Assets.Road(TEXT("Alley")).m_Flags;
	const uint32 Lane = Assets.Road(TEXT("Alley 2m")).m_Flags;
	TestTrue(TEXT("Alley zones"), (Base & CSRoad::RoadFlags_EnableZoning) != 0);
	TestTrue(TEXT("Alley 2m does not"), (Lane & CSRoad::RoadFlags_EnableZoning) == 0);
	const CSRoad::FAssets::FPlots* Plots = Assets.Plots(TEXT("Alley 2m"));
	if (TestNotNull(TEXT("Alley 2m has plots"), Plots))
	{
		TestEqual(TEXT("Alley 2m plot setback (m)"), Plots->Setback, 0.5, 1e-6);
		TestEqual(TEXT("Alley 2m plot frontage (m)"), Plots->Frontage, 9.0, 1e-6);
	}
	TestNull(TEXT("Alley goes by its zoning"), Assets.Plots(TEXT("Alley")));
	TestTrue(TEXT("the CS2 roads are all still there"), Assets.RoadNames().Num() == Lib.Assets.RoadNames().Num() + 1);

	// The pieces by name, from the catalog.
	TSharedPtr<FJsonObject> Catalog;
	const FUTF8ToTCHAR CatalogChars(reinterpret_cast<const ANSICHAR*>(Lib.Prefabs.GetData()), Lib.Prefabs.Num());
	const FString CatalogText(CatalogChars.Length(), CatalogChars.Get());
	if (!TestTrue(TEXT("catalog read"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(CatalogText), Catalog) && Catalog.IsValid())) return false;
	auto PieceName = [&Catalog](int32 Piece)
	{
		const TSharedPtr<FJsonObject>* Entry = nullptr;
		FString Name;
		if (Catalog->GetObjectField(TEXT("pieces"))->TryGetObjectField(FString::FromInt(Piece), Entry)) (*Entry)->TryGetStringField(TEXT("name"), Name);
		return Name;
	};
	auto Count = [&PieceName](const CSRoad::FComposition& Composition, const TCHAR* Name)
	{
		int32 Found = 0;
		for (const CSRoad::NetCompositionPiece& Piece : Composition.Pieces) Found += PieceName(Piece.m_Piece) == Name ? 1 : 0;
		return Found;
	};
	CSRoad::CompositionFlags AtSidewalks{};
	AtSidewalks.m_General = CSRoad::CompositionFlags_General_Node | CSRoad::CompositionFlags_General_Intersection;
	AtSidewalks.m_Left = AtSidewalks.m_Right = CSRoad::CompositionFlags_Side_Sidewalk;
	CSRoad::CompositionFlags AtAlleys{};
	AtAlleys.m_General = CSRoad::CompositionFlags_General_Node | CSRoad::CompositionFlags_General_Intersection;
	TestEqual(TEXT("Alley 2m among sidewalks: shoulders ramping up to them"), Count(Assets.Compose(TEXT("Alley 2m"), AtSidewalks), TEXT("Alley Shoulder To Sidewalk 1")), 2);
	TestEqual(TEXT("Alley 2m among alleys: flat shoulders"), Count(Assets.Compose(TEXT("Alley 2m"), AtAlleys), TEXT("Alley Shoulder 1")), 2);

	// The borrowed form: Alley Oneway's drive section between Alley's 1 m shoulders, the left one its right one turned;
	// and Alley's 1 m shoulders three times, the middle one made the median.
	const FString BorrowedText = TEXT(R"({
		"Lane 5m": { "copy": "Alley Oneway", "sections": [0, { "road": "Alley", "index": 5, "invert": true }, 2, { "road": "Alley", "index": 5 }, 4], "underground": [] },
		"Lane 3m": { "copy": "Alley", "sections": [0, 1, { "index": 5, "median": true }, 5, 6], "underground": [] } })");
	const FTCHARToUTF8 BorrowedUtf8(*BorrowedText);
	const TArray<uint8> Borrowed(reinterpret_cast<const uint8*>(BorrowedUtf8.Get()), BorrowedUtf8.Length());
	CSRoad::FAssets WithBorrowed;
	if (!TestTrue(TEXT("borrowed sections loaded"), WithBorrowed.Load(Lib.Prefabs, Lib.Raw, &Lib.Pieces, Error, &Borrowed))) return false;
	TestEqual(TEXT("Lane 5m width (m)"), WithBorrowed.Geometry(TEXT("Lane 5m")).m_DefaultWidth, 5.0f, 0.01f);
	TestEqual(TEXT("Lane 3m width (m)"), WithBorrowed.Geometry(TEXT("Lane 3m")).m_DefaultWidth, 3.0f, 0.01f);
	CSRoad::CompositionFlags OnEdge{};
	OnEdge.m_General = CSRoad::CompositionFlags_General_Edge;
	bool bTurned = false;
	for (const CSRoad::NetCompositionPiece& Piece : WithBorrowed.Compose(TEXT("Lane 5m"), OnEdge).Pieces) bTurned |= Piece.m_SectionIndex == 1 && (Piece.m_SectionFlags & CSRoad::NetSectionFlags_Invert) != 0;
	TestTrue(TEXT("Lane 5m: the borrowed left shoulder turned"), bTurned);
	bool bMedian = false;
	for (const CSRoad::NetCompositionPiece& Piece : WithBorrowed.Compose(TEXT("Lane 3m"), OnEdge).Pieces) bMedian |= Piece.m_SectionIndex == 2 && (Piece.m_SectionFlags & CSRoad::NetSectionFlags_Median) != 0;
	TestTrue(TEXT("Lane 3m: the middle shoulder the median"), bMedian);

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSRoadNetworkActor* Network = World->SpawnActor<ACSRoadNetworkActor>();
	if (!TestNotNull(TEXT("network actor"), Network)) return false;
	// Two 100 m alleys 60 m apart and a 2 m alley across between their middles.
	struct FSpec
	{
		const TCHAR* Prefab;
		FVector A;
		FVector B;
	};
	const FSpec Specs[] = {
		{ TEXT("Alley"), FVector(-5000.0, 0.0, 0.0), FVector(5000.0, 0.0, 0.0) },
		{ TEXT("Alley"), FVector(-5000.0, 6000.0, 0.0), FVector(5000.0, 6000.0, 0.0) },
		{ TEXT("Alley 2m"), FVector(0.0, 0.0, 0.0), FVector(0.0, 6000.0, 0.0) } };
	for (const FSpec& Spec : Specs)
	{
		ACSRoadSplineActor* Road = World->SpawnActor<ACSRoadSplineActor>();
		Road->AttachToActor(Network, FAttachmentTransformRules::KeepRelativeTransform);
		Road->Spline->Prefab = Spec.Prefab;
		Road->Spline->SetSplinePoints({ Spec.A, Spec.B }, ESplineCoordinateSpace::Local);
	}
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("network: error"), Network->LastError, FString());
	TestEqual(TEXT("network: roads (each alley split by its T junction, and the lane)"), Network->NumRoads, 5);
	// With buildings: the alleys' plots and the lane's (CSCityTests' City.LotsFill checks how they share the ground).
	Network->bBatchBuildings = true;
	Network->Rebuild();
	Network->FinishWrites();
	TestEqual(TEXT("buildings: error"), Network->LastError, FString());
	TestTrue(TEXT("buildings along the alleys and the lane"), Network->NumBuildings > 0);
	return true;
}

#endif

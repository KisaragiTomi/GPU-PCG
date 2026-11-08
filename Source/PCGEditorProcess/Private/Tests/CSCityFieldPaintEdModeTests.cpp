#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSCityFieldActor.h"
#include "CSCityFieldPaintEdMode.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/HitResult.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "Tests/AutomationEditorCommon.h"

// The wiring only a mode instance can show: the button comes through the delegate to the module and hands the mode its target; the cursor finds the
// field's plane where nothing has collision (the ground and the buildings are GPU instances); a stroke's end lets go of what was painted; Esc leaves.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSCityFieldPaintModeTest,
	"PCGPlugins.PCGEditorProcess.City.FieldPaintMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSCityFieldPaintModeTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the mode is registered"), FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSCityFieldPaintEdMode::EM_CSCityFieldPaint));
	TestTrue(TEXT("the module listens for the field's Start Paint"), ACSCityFieldActor::OnPaintRequest.IsBound());
	// The global mode manager hard-crashes in a commandlet.
	if (IsRunningCommandlet() || !GEditor)
	{
		AddInfo(TEXT("No global mode manager here; only the registry half ran."));
		return true;
	}
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("editor world"), World)) return false;
	ACSCityFieldActor* Field = World->SpawnActor<ACSCityFieldActor>(FVector(0.0, 0.0, 250.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("field"), Field)) return false;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	const FEditorModeID ModeID = FCSCityFieldPaintEdMode::EM_CSCityFieldPaint;
	if (ModeTools.IsModeActive(ModeID)) ModeTools.DeactivateMode(ModeID);

	Field->StartPaint();
	TestTrue(TEXT("Start Paint brings the mode up"), ModeTools.IsModeActive(ModeID));
	FCSCityFieldPaintEdMode* Mode = ModeTools.GetActiveModeTyped<FCSCityFieldPaintEdMode>(ModeID);
	if (!TestNotNull(TEXT("the mode"), Mode)) return false;

	// Nothing in this world has collision: the cursor still lands, on the plane the map is drawn on.
	FHitResult Hit;
	TestTrue(TEXT("the cursor finds the field's plane"), Mode->TraceCandidatePoint(FVector(1000.0, 2000.0, 9000.0), FVector(1000.0, 2000.0, -9000.0), Hit));
	TestTrue(TEXT("at the field's height"), Hit.ImpactPoint.Equals(FVector(1000.0, 2000.0, 250.0), 0.01));
	TestFalse(TEXT("a ray that never crosses it misses"), Mode->TraceCandidatePoint(FVector(0.0, 0.0, 9000.0), FVector(0.0, 0.0, 500.0), Hit));

	// A stroke's end with nothing painted is nothing; with a dab, the field stays as painted.
	Field->BrushGap = ECSCityGapStyle::NightMarket;
	Field->PaintDab(FVector2D(1000.0, 2000.0));
	Mode->CommitSamples({});
	TestEqual(TEXT("a stroke's end keeps what it painted"), Field->GapStyleAt(FVector2D(1000.0, 2000.0)), ECSCityGapStyle::NightMarket);
	TestTrue(TEXT("still up after a stroke"), ModeTools.IsModeActive(ModeID));

	TestTrue(TEXT("with a target, Esc is taken"), Mode->InputKey(nullptr, nullptr, EKeys::Escape, IE_Pressed));
	TestFalse(TEXT("and leaves the mode"), ModeTools.IsModeActive(ModeID));
	return true;
}

#endif

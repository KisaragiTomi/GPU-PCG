#include "CSCityFieldPaintEdMode.h"

#include "CSCityFieldActor.h"
#include "Editor.h"
#include "Engine/HitResult.h"

const FEditorModeID FCSCityFieldPaintEdMode::EM_CSCityFieldPaint = TEXT("CSCityFieldPaintEdMode");

void FCSCityFieldPaintEdMode::SetTargetActor(ACSCityFieldActor* InTargetActor)
{
	EndStroke();
	TargetActor = InTargetActor;
	ResetBrushState();
}

AActor* FCSCityFieldPaintEdMode::GetBrushTargetActor() const
{
	return TargetActor.Get();
}

FCSBrushSettings FCSCityFieldPaintEdMode::GetBrushSettings() const
{
	FCSBrushSettings Settings;
	const ACSCityFieldActor* Target = TargetActor.Get();
	if (!Target) return Settings;
	Settings.Radius = Target->BrushRadius * 100.0f;
	Settings.TraceRadius = 0.0f;
	// Cells are written over, not placed: spacing means nothing here.
	Settings.MinSpacing = 0.0f;
	// A dab of the whole brush at the cursor.
	Settings.SamplesPerMouseMove = 1;
	return Settings;
}

void FCSCityFieldPaintEdMode::SamplePendingPoints()
{
	ACSCityFieldActor* Target = TargetActor.Get();
	if (!Target || !IsBrushTraceValid()) return;
	if (!bInTransaction && GEditor)
	{
		GEditor->BeginTransaction(NSLOCTEXT("CSCityFieldPaint", "Stroke", "Paint City Field"));
		Target->Modify();
		bInTransaction = true;
	}
	Target->PaintDab(FVector2D(GetBrushLocation()));
}

void FCSCityFieldPaintEdMode::EndStroke()
{
	if (ACSCityFieldActor* Target = TargetActor.Get()) Target->EndPaintStroke();
	if (bInTransaction && GEditor) GEditor->EndTransaction();
	bInTransaction = false;
}

void FCSCityFieldPaintEdMode::CommitSamples(const TArray<FCSBrushSample>& /*Samples*/)
{
	EndStroke();
}

void FCSCityFieldPaintEdMode::ClearBrushTarget()
{
	// Esc or leaving the mode does not come through the commit: what was painted is painted.
	EndStroke();
	TargetActor.Reset();
}

bool FCSCityFieldPaintEdMode::TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	if (FCSBrushEdModeBase::TraceCandidatePoint(Start, End, OutHit)) return true;
	const ACSCityFieldActor* Target = TargetActor.Get();
	if (!Target) return false;
	// No collision under the cursor: the plane the map is drawn on.
	const double Z = Target->GetActorLocation().Z;
	const double Run = End.Z - Start.Z;
	if (FMath::IsNearlyZero(Run)) return false;
	const double T = (Z - Start.Z) / Run;
	if (T < 0.0 || T > 1.0) return false;
	const FVector Hit = FMath::Lerp(Start, End, T);
	OutHit = FHitResult();
	OutHit.bBlockingHit = true;
	OutHit.ImpactPoint = Hit;
	OutHit.Location = Hit;
	OutHit.ImpactNormal = FVector::UpVector;
	OutHit.Normal = FVector::UpVector;
	return true;
}

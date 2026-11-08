#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class ACSCityFieldActor;

/**
 * Paints the city's attribute field (ACSCityFieldActor) in the viewport.
 *
 * The stroke lifecycle belongs to FCSBrushEdModeBase; two things differ here:
 *  - the cursor trace: the ground and the buildings are GPU instances with no collision, so what collision there is comes first and the
 *    field's own height is the fallback;
 *  - the sampling: no pending set and no preview points. Each stroke update is a dab into the field, whose map shows it at once; the
 *    buildings and the ground over it are written when the mouse comes up (one undo step for the stroke).
 */
class FCSCityFieldPaintEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSCityFieldPaint;

	void SetTargetActor(ACSCityFieldActor* InTargetActor);

	// Public for the mode's test: the cursor's landing and a stroke's end are what it checks.
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const override;

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void SamplePendingPoints() override;
	virtual void ClearBrushTarget() override;

private:
	void EndStroke();

	TWeakObjectPtr<ACSCityFieldActor> TargetActor;
	bool bInTransaction = false;
};

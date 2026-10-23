#pragma once

#include "CoreMinimal.h"
#include "CSBrushEdModeBase.h"

class ACSGroundActor;

/**
 * Paints vertex colours onto ACSGroundActor's ground mesh (Plan_House.md D2).
 *
 * The stroke lifecycle belongs to FCSBrushEdModeBase; two things differ here:
 *  - the cursor trace: the ground's GPU mesh has no collision, so TraceCandidatePoint is
 *    overridden with the actor's analytic RaycastGround instead of FoliageTrace;
 *  - the sampling: there is no pending set and no preview points — every stroke update goes
 *    straight to ACSGroundActor::ApplyPaintStroke, which writes the CPU mirror (the authority)
 *    and queues the dab. The GPU side is pushed once per Tick as a single async edit, so a
 *    stroke costs zero device syncs. Like the point brush's GPU path, Esc cannot take back what
 *    a stroke already painted (no undo, by the same design decision).
 */
class FCSGroundPaintEdMode : public FCSBrushEdModeBase
{
public:
	static const FEditorModeID EM_CSGroundPaint;

	void SetTargetActor(ACSGroundActor* InTargetActor);

protected:
	virtual AActor* GetBrushTargetActor() const override;
	virtual FCSBrushSettings GetBrushSettings() const override;
	virtual void SamplePendingPoints() override;
	virtual void CommitSamples(const TArray<FCSBrushSample>& Samples) override;
	virtual void ClearBrushTarget() override;
	virtual bool TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const override;

	/**
	 * 每帧一次把本帧攒下的落笔推给 GPU（ACSGroundActor::FlushPaintToGpu，一次异步编辑）。
	 * 落笔本身只写 CPU 镜像 —— 交互热路径上因此一次设备同步都没有，代价只是 GPU 顶点色
	 * 比镜像滞后 ≤1 帧，纯视觉（所有查询走镜像）。
	 */
	virtual void Tick(FEditorViewportClient* ViewportClient, float DeltaTime) override;

private:
	TWeakObjectPtr<ACSGroundActor> TargetActor;
};

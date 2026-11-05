#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/SharedPointer.h"

class AActor;
class IAssetViewport;
class SWidget;
class ULevel;
class UWorld;
enum class EMapChangeType : uint8;

class FSelectedActorViewportOverlayBase
{
public:
	virtual ~FSelectedActorViewportOverlayBase();

	void Start();
	void Stop();
	void RefreshFromCurrentSelection();

protected:
	virtual bool SupportsActor(const AActor* Actor) const = 0;
	virtual TSharedRef<SWidget> CreateOverlayWidget(TWeakObjectPtr<AActor> Actor);
	virtual int32 GetOverlayZOrder() const { return 100; }
	virtual FName GetRequiredDetailsCategoryName() const { return NAME_None; }
	virtual FText GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const;
	virtual float GetDetailsCategoryOverlayWidth() const { return 360.0f; }
	virtual float GetDetailsCategoryOverlayMaxHeight() const { return 520.0f; }
	bool ActorHasRequiredDetailsCategory(const AActor* Actor) const;
	bool ActorHasDetailsCategory(const AActor* Actor, FName CategoryName) const;
	TSharedRef<SWidget> CreateDetailsCategoryPanelWidget(TWeakObjectPtr<AActor> Actor) const;

private:
	void HandleActorSelectionChanged(const TArray<UObject*>& NewSelection, bool bForceRefresh);
	void HandleActorDestroyed(AActor* DestroyedActor);
	void HandleMapChanged(UWorld* World, EMapChangeType MapChangeType);
	void HandleNewCurrentLevel();
	void HandlePreLevelRemovedFromWorld(ULevel* Level, UWorld* World);
	void HandleWorldBeginTearDown(UWorld* World);
	void HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
	AActor* FindSupportedActor(const TArray<UObject*>& Selection) const;
	void QueueSelectionRefresh(AActor* NewTarget, bool bForceRefresh);
	bool ApplyQueuedSelectionRefresh(float DeltaTime);
	void CancelQueuedSelectionRefresh();
	void ApplySelectionRefresh(AActor* NewTarget, bool bForceRefresh);
	void ShowOverlayForActor(AActor* Actor);
	void RemoveOverlay();
	void RemoveOverlayForContextChange(bool bCancelQueuedRefresh);
	void ForgetOverlayWithoutTouchingViewport();
	void UnbindActorDestroyed();
	void UnbindWorldDelegates();
	bool IsTrackingActorInLevel(const ULevel* Level, const UWorld* World) const;
	bool IsTrackingActorInWorld(const UWorld* World) const;
	void CacheTargetActorWorldContext(AActor* Actor);
	void ClearTargetActorWorldContext();

	FDelegateHandle ActorSelectionChangedHandle;
	FDelegateHandle ActorDestroyedHandle;
	FDelegateHandle MapChangedHandle;
	FDelegateHandle NewCurrentLevelHandle;
	FDelegateHandle PreLevelRemovedHandle;
	FDelegateHandle WorldBeginTearDownHandle;
	FDelegateHandle WorldCleanupHandle;
	FTSTicker::FDelegateHandle QueuedSelectionRefreshHandle;
	TWeakObjectPtr<AActor> TargetActor;
	TWeakObjectPtr<AActor> QueuedTargetActor;
	TWeakObjectPtr<ULevel> TargetLevel;
	TWeakObjectPtr<UWorld> TargetWorld;
	TWeakPtr<IAssetViewport> OverlayViewport;
	TSharedPtr<SWidget> OverlayWidget;
	bool bQueuedForceRefresh = false;
	bool bStarted = false;
};

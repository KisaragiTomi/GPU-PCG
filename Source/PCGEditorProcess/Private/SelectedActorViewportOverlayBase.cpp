#include "SelectedActorViewportOverlayBase.h"

#include "DetailsCategoryAliasCustomization.h"
#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/Actor.h"
#include "IAssetViewport.h"
#include "ILevelEditor.h"
#include "ObjectEditorUtils.h"
#include "LevelEditor.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "Selection.h"
#include "SLevelViewport.h"
#include "Styling/AppStyle.h"
#include "UnrealEdMisc.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SNullWidget.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWidget.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
FLevelEditorModule* GetLevelEditorModule(bool bLoadIfNeeded)
{
	if (IsEngineExitRequested())
	{
		return nullptr;
	}

	if (FModuleManager::Get().IsModuleLoaded("LevelEditor"))
	{
		return FModuleManager::GetModulePtr<FLevelEditorModule>("LevelEditor");
	}

	return bLoadIfNeeded ? FModuleManager::LoadModulePtr<FLevelEditorModule>("LevelEditor") : nullptr;
}

bool CanTouchViewportOverlay()
{
	return IsInGameThread() && !IsEngineExitRequested() && FSlateApplication::IsInitialized();
}

bool IsActorInWorldTeardown(const AActor* Actor)
{
	const UWorld* World = Actor ? Actor->GetWorld() : nullptr;
	return World && (World->bIsTearingDown || World->IsBeingCleanedUp() || World->IsCleanedUp());
}

TSharedPtr<IAssetViewport> FindOverlayViewport(FLevelEditorModule& LevelEditorModule)
{
	if (TSharedPtr<IAssetViewport> ActiveViewport = LevelEditorModule.GetFirstActiveViewport())
	{
		return ActiveViewport;
	}

	if (TSharedPtr<ILevelEditor> LevelEditor = LevelEditorModule.GetLevelEditorInstance().Pin())
	{
		for (const TSharedPtr<SLevelViewport>& LevelViewport : LevelEditor->GetViewports())
		{
			if (LevelViewport.IsValid())
			{
				return StaticCastSharedPtr<IAssetViewport>(LevelViewport);
			}
		}
	}

	return nullptr;
}

bool DoesFieldMatchDetailsCategory(const FField* Field, FName RequiredCategoryName)
{
	return Field && FDetailsCategoryAliasCustomization::DoesCategoryMatch(FObjectEditorUtils::GetCategoryFName(Field), RequiredCategoryName);
}

class SSelectedActorDetailsCategoryPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SSelectedActorDetailsCategoryPanel) {}
		SLATE_ARGUMENT(TWeakObjectPtr<AActor>, Actor)
		SLATE_ARGUMENT(FName, CategoryName)
		SLATE_ARGUMENT(FText, Title)
		SLATE_ARGUMENT(float, Width)
		SLATE_ARGUMENT(float, MaxHeight)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs)
	{
		Actor = InArgs._Actor;
		CategoryName = InArgs._CategoryName;

		FDetailsViewArgs DetailsViewArgs;
		DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
		DetailsViewArgs.bUpdatesFromSelection = false;
		DetailsViewArgs.bLockable = false;
		DetailsViewArgs.bAllowSearch = false;
		DetailsViewArgs.bHideSelectionTip = true;
		DetailsViewArgs.bShowPropertyMatrixButton = false;
		DetailsViewArgs.bShowOptions = false;
		DetailsViewArgs.bShowObjectLabel = false;
		DetailsViewArgs.bAllowFavoriteSystem = false;
		DetailsViewArgs.bShowScrollBar = true;
		DetailsViewArgs.ColumnWidth = 0.45f;

		FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		TSharedRef<IDetailsView> NewDetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
		DetailsView = NewDetailsView;
		// 只在这个 view 里顶替引擎的 FObjectDetails，把 "ViewEdit" / "View Edit" 这类写法合成一个分类。
		NewDetailsView->RegisterInstancedCustomPropertyLayout(
			UObject::StaticClass(),
			FOnGetDetailCustomizationInstance::CreateLambda([RequiredCategoryName = CategoryName]() -> TSharedRef<IDetailCustomization>
			{
				return MakeShared<FDetailsCategoryAliasCustomization>(RequiredCategoryName);
			}));
		NewDetailsView->SetIsPropertyVisibleDelegate(FIsPropertyVisible::CreateSP(this, &SSelectedActorDetailsCategoryPanel::IsPropertyVisible));
		NewDetailsView->SetIsCustomRowVisibleDelegate(FIsCustomRowVisible::CreateSP(this, &SSelectedActorDetailsCategoryPanel::IsCustomRowVisible));
		NewDetailsView->SetObject(Actor.Get(), true);

		SetVisibility(EVisibility::SelfHitTestInvisible);

		ChildSlot
		[
			SNew(SOverlay)
			+ SOverlay::Slot()
			.HAlign(HAlign_Left)
			.VAlign(VAlign_Top)
			.Padding(16.0f)
			[
				SNew(SBox)
				.WidthOverride(InArgs._Width)
				.MaxDesiredHeight(InArgs._MaxHeight)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
					.BorderBackgroundColor(FLinearColor(0.02f, 0.02f, 0.02f, 0.78f))
					.Padding(FMargin(8.0f))
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot()
						.AutoHeight()
						.Padding(0.0f, 0.0f, 0.0f, 6.0f)
						[
							SNew(STextBlock)
							.Text(InArgs._Title)
							.Font(FAppStyle::GetFontStyle("PropertyWindow.BoldFont"))
							.ColorAndOpacity(FLinearColor::White)
						]
						+ SVerticalBox::Slot()
						.FillHeight(1.0f)
						[
							NewDetailsView
						]
					]
				]
			]
		];
	}

private:
	bool IsPropertyVisible(const FPropertyAndParent& PropertyAndParent) const
	{
		if (DoesFieldMatchDetailsCategory(&PropertyAndParent.Property, CategoryName))
		{
			return true;
		}

		for (const FProperty* ParentProperty : PropertyAndParent.ParentProperties)
		{
			if (DoesFieldMatchDetailsCategory(ParentProperty, CategoryName))
			{
				return true;
			}
		}

		return false;
	}

	bool IsCustomRowVisible(FName RowName, FName ParentName) const
	{
		// ParentName 不一定是分类的 FName：分类的「显示高级属性」那一行传的是显示名（"View Edit"），
		// 以前按原样比较，ViewEdit 里的 AdvancedDisplay 属性在浮窗里就一直展不开。
		return FDetailsCategoryAliasCustomization::DoesCategoryMatch(RowName, CategoryName)
			|| FDetailsCategoryAliasCustomization::DoesCategoryMatch(ParentName, CategoryName);
	}

	TWeakObjectPtr<AActor> Actor;
	FName CategoryName;
	TSharedPtr<IDetailsView> DetailsView;
};
}

FSelectedActorViewportOverlayBase::~FSelectedActorViewportOverlayBase()
{
	Stop();
}

TSharedRef<SWidget> FSelectedActorViewportOverlayBase::CreateOverlayWidget(TWeakObjectPtr<AActor> Actor)
{
	if (!Actor.IsValid() || GetRequiredDetailsCategoryName().IsNone())
	{
		return SNullWidget::NullWidget;
	}

	return CreateDetailsCategoryPanelWidget(Actor);
}

FText FSelectedActorViewportOverlayBase::GetDetailsCategoryTitle(TWeakObjectPtr<AActor> Actor) const
{
	const FName CategoryName = GetRequiredDetailsCategoryName();
	if (const AActor* ActorPtr = Actor.Get())
	{
		return FText::FromString(FString::Printf(TEXT("%s - %s"), *ActorPtr->GetActorLabel(), *CategoryName.ToString()));
	}

	return FText::FromName(CategoryName);
}

bool FSelectedActorViewportOverlayBase::ActorHasRequiredDetailsCategory(const AActor* Actor) const
{
	const FName CategoryName = GetRequiredDetailsCategoryName();
	return CategoryName.IsNone() || ActorHasDetailsCategory(Actor, CategoryName);
}

bool FSelectedActorViewportOverlayBase::ActorHasDetailsCategory(const AActor* Actor, FName CategoryName) const
{
	if (!Actor || CategoryName.IsNone())
	{
		return false;
	}

	const UClass* ActorClass = Actor->GetClass();
	for (TFieldIterator<FProperty> It(ActorClass, EFieldIteratorFlags::IncludeSuper); It; ++It)
	{
		const FProperty* Property = *It;
		if (Property
			&& Property->HasAnyPropertyFlags(CPF_Edit)
			&& !FObjectEditorUtils::IsVariableCategoryHiddenFromClass(Property, ActorClass)
			&& DoesFieldMatchDetailsCategory(Property, CategoryName))
		{
			return true;
		}
	}

	// 与浮窗实际出按钮的同一份名单：只算真能出成按钮的（无参数等），免得浮起一个空窗。
	TArray<UFunction*> Functions;
	FDetailsCategoryAliasCustomization::GetCallInEditorFunctions(ActorClass, CategoryName, Functions);
	return !Functions.IsEmpty();
}

TSharedRef<SWidget> FSelectedActorViewportOverlayBase::CreateDetailsCategoryPanelWidget(TWeakObjectPtr<AActor> Actor) const
{
	return SNew(SSelectedActorDetailsCategoryPanel)
		.Actor(Actor)
		.CategoryName(GetRequiredDetailsCategoryName())
		.Title(GetDetailsCategoryTitle(Actor))
		.Width(GetDetailsCategoryOverlayWidth())
		.MaxHeight(GetDetailsCategoryOverlayMaxHeight());
}

void FSelectedActorViewportOverlayBase::Start()
{
	if (bStarted)
	{
		return;
	}

	FLevelEditorModule* LevelEditorModule = GetLevelEditorModule(true);
	if (!LevelEditorModule)
	{
		return;
	}

	ActorSelectionChangedHandle = LevelEditorModule->OnActorSelectionChanged().AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandleActorSelectionChanged);
	MapChangedHandle = LevelEditorModule->OnMapChanged().AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandleMapChanged);
	NewCurrentLevelHandle = FEditorDelegates::NewCurrentLevel.AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandleNewCurrentLevel);
	PreLevelRemovedHandle = FWorldDelegates::PreLevelRemovedFromWorld.AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandlePreLevelRemovedFromWorld);
	WorldBeginTearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandleWorldBeginTearDown);
	WorldCleanupHandle = FWorldDelegates::OnWorldCleanup.AddRaw(
		this,
		&FSelectedActorViewportOverlayBase::HandleWorldCleanup);

	bStarted = true;
	RefreshFromCurrentSelection();
}

void FSelectedActorViewportOverlayBase::Stop()
{
	CancelQueuedSelectionRefresh();
	RemoveOverlay();

	if (ActorSelectionChangedHandle.IsValid())
	{
		if (FLevelEditorModule* LevelEditorModule = GetLevelEditorModule(false))
		{
			LevelEditorModule->OnActorSelectionChanged().Remove(ActorSelectionChangedHandle);
		}
		ActorSelectionChangedHandle.Reset();
	}

	if (MapChangedHandle.IsValid())
	{
		if (FLevelEditorModule* LevelEditorModule = GetLevelEditorModule(false))
		{
			LevelEditorModule->OnMapChanged().Remove(MapChangedHandle);
		}
		MapChangedHandle.Reset();
	}

	if (NewCurrentLevelHandle.IsValid())
	{
		FEditorDelegates::NewCurrentLevel.Remove(NewCurrentLevelHandle);
		NewCurrentLevelHandle.Reset();
	}

	UnbindWorldDelegates();
	ClearTargetActorWorldContext();
	TargetActor.Reset();
	bStarted = false;
}

void FSelectedActorViewportOverlayBase::RefreshFromCurrentSelection()
{
	if (!bStarted || IsEngineExitRequested())
	{
		return;
	}

	TArray<UObject*> Selection;

	if (GEditor)
	{
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (UObject* Object = *It)
			{
				Selection.Add(Object);
			}
		}
	}

	HandleActorSelectionChanged(Selection, true);
}

void FSelectedActorViewportOverlayBase::HandleActorSelectionChanged(const TArray<UObject*>& NewSelection, bool bForceRefresh)
{
	if (!bStarted || IsEngineExitRequested())
	{
		return;
	}

	AActor* NewTarget = FindSupportedActor(NewSelection);
	QueueSelectionRefresh(NewTarget, bForceRefresh);
}

AActor* FSelectedActorViewportOverlayBase::FindSupportedActor(const TArray<UObject*>& Selection) const
{
	for (UObject* Object : Selection)
	{
		AActor* Actor = Cast<AActor>(Object);
		if (IsValid(Actor) && SupportsActor(Actor) && ActorHasRequiredDetailsCategory(Actor))
		{
			return Actor;
		}
	}

	return nullptr;
}

void FSelectedActorViewportOverlayBase::QueueSelectionRefresh(AActor* NewTarget, bool bForceRefresh)
{
	if (!bStarted || IsEngineExitRequested())
	{
		return;
	}

	if (NewTarget != TargetActor.Get() && OverlayWidget.IsValid())
	{
		RemoveOverlayForContextChange(true);
	}

	const bool bForceQueuedRefresh = bQueuedForceRefresh || bForceRefresh;
	QueuedTargetActor = NewTarget;
	bQueuedForceRefresh = bForceQueuedRefresh;

	if (!bForceQueuedRefresh && NewTarget == TargetActor.Get() && OverlayWidget.IsValid())
	{
		CancelQueuedSelectionRefresh();
		return;
	}

	if (!QueuedSelectionRefreshHandle.IsValid())
	{
		// Selection can change while Slate is still processing input from the overlay.
		QueuedSelectionRefreshHandle = FTSTicker::GetCoreTicker().AddTicker(
			TEXT("SelectedActorViewportOverlaySelectionRefresh"),
			0.0f,
			[this](float DeltaTime)
			{
				return ApplyQueuedSelectionRefresh(DeltaTime);
			});
	}
}

bool FSelectedActorViewportOverlayBase::ApplyQueuedSelectionRefresh(float DeltaTime)
{
	QueuedSelectionRefreshHandle.Reset();
	AActor* NewTarget = QueuedTargetActor.Get();
	const bool bForceRefresh = bQueuedForceRefresh;
	QueuedTargetActor.Reset();
	bQueuedForceRefresh = false;

	if (!bStarted || IsEngineExitRequested())
	{
		return false;
	}

	ApplySelectionRefresh(NewTarget, bForceRefresh);
	return false;
}

void FSelectedActorViewportOverlayBase::CancelQueuedSelectionRefresh()
{
	if (QueuedSelectionRefreshHandle.IsValid())
	{
		FTSTicker::RemoveTicker(QueuedSelectionRefreshHandle);
		QueuedSelectionRefreshHandle.Reset();
	}

	QueuedTargetActor.Reset();
	bQueuedForceRefresh = false;
}

void FSelectedActorViewportOverlayBase::ApplySelectionRefresh(AActor* NewTarget, bool bForceRefresh)
{
	if (!bStarted || IsEngineExitRequested())
	{
		return;
	}

	if (!bForceRefresh && NewTarget == TargetActor.Get() && OverlayWidget.IsValid())
	{
		return;
	}

	if (!NewTarget)
	{
		RemoveOverlay();
		TargetActor.Reset();
		return;
	}

	ShowOverlayForActor(NewTarget);
}

void FSelectedActorViewportOverlayBase::ShowOverlayForActor(AActor* Actor)
{
	RemoveOverlay();
	TargetActor.Reset();

	if (!bStarted || !CanTouchViewportOverlay() || !IsValid(Actor))
	{
		return;
	}

	FLevelEditorModule* LevelEditorModule = GetLevelEditorModule(true);
	if (!LevelEditorModule)
	{
		return;
	}

	TSharedPtr<IAssetViewport> ActiveViewport = FindOverlayViewport(*LevelEditorModule);
	if (!ActiveViewport.IsValid())
	{
		return;
	}

	TargetActor = Actor;
	const TSharedRef<SWidget> NewOverlayWidget = CreateOverlayWidget(TargetActor);
	if (!TargetActor.IsValid() || !CanTouchViewportOverlay())
	{
		TargetActor.Reset();
		return;
	}

	OverlayWidget = NewOverlayWidget;
	OverlayViewport = ActiveViewport;
	CacheTargetActorWorldContext(Actor);
	ActiveViewport->AddOverlayWidget(NewOverlayWidget, GetOverlayZOrder());

	if (GEngine)
	{
		ActorDestroyedHandle = GEngine->OnLevelActorDeleted().AddRaw(this, &FSelectedActorViewportOverlayBase::HandleActorDestroyed);
	}
}

void FSelectedActorViewportOverlayBase::RemoveOverlay()
{
	UnbindActorDestroyed();

	if (OverlayWidget.IsValid())
	{
		if (CanTouchViewportOverlay())
		{
			if (TSharedPtr<IAssetViewport> Viewport = OverlayViewport.Pin())
			{
				Viewport->RemoveOverlayWidget(OverlayWidget.ToSharedRef());
			}
		}
	}

	OverlayWidget.Reset();
	OverlayViewport.Reset();
	ClearTargetActorWorldContext();
}

void FSelectedActorViewportOverlayBase::HandleActorDestroyed(AActor* DestroyedActor)
{
	if (DestroyedActor == TargetActor.Get() || !TargetActor.IsValid())
	{
		if (IsEngineExitRequested() || IsActorInWorldTeardown(DestroyedActor) || !CanTouchViewportOverlay())
		{
			ForgetOverlayWithoutTouchingViewport();
			return;
		}

		QueueSelectionRefresh(nullptr, true);
	}
}

void FSelectedActorViewportOverlayBase::HandleMapChanged(UWorld* World, EMapChangeType MapChangeType)
{
	if (MapChangeType == EMapChangeType::SaveMap)
	{
		return;
	}

	RemoveOverlayForContextChange(true);
}

void FSelectedActorViewportOverlayBase::HandleNewCurrentLevel()
{
	RemoveOverlayForContextChange(true);
}

void FSelectedActorViewportOverlayBase::HandlePreLevelRemovedFromWorld(ULevel* Level, UWorld* World)
{
	if (IsTrackingActorInLevel(Level, World))
	{
		if (CanTouchViewportOverlay())
		{
			RemoveOverlay();
			TargetActor.Reset();
			return;
		}

		ForgetOverlayWithoutTouchingViewport();
	}
}

void FSelectedActorViewportOverlayBase::HandleWorldBeginTearDown(UWorld* World)
{
	if (IsTrackingActorInWorld(World))
	{
		ForgetOverlayWithoutTouchingViewport();
	}
}

void FSelectedActorViewportOverlayBase::HandleWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
	if (IsTrackingActorInWorld(World))
	{
		ForgetOverlayWithoutTouchingViewport();
	}
}

void FSelectedActorViewportOverlayBase::ForgetOverlayWithoutTouchingViewport()
{
	CancelQueuedSelectionRefresh();
	UnbindActorDestroyed();
	OverlayWidget.Reset();
	OverlayViewport.Reset();
	ClearTargetActorWorldContext();
	TargetActor.Reset();
}

void FSelectedActorViewportOverlayBase::RemoveOverlayForContextChange(bool bCancelQueuedRefresh)
{
	if (bCancelQueuedRefresh)
	{
		CancelQueuedSelectionRefresh();
	}

	if (!OverlayWidget.IsValid() && !TargetActor.IsValid() && !TargetLevel.IsValid() && !TargetWorld.IsValid())
	{
		return;
	}

	if (CanTouchViewportOverlay())
	{
		RemoveOverlay();
	}
	else
	{
		ForgetOverlayWithoutTouchingViewport();
	}

	TargetActor.Reset();
}

void FSelectedActorViewportOverlayBase::UnbindActorDestroyed()
{
	if (ActorDestroyedHandle.IsValid())
	{
		if (GEngine)
		{
			GEngine->OnLevelActorDeleted().Remove(ActorDestroyedHandle);
		}
		ActorDestroyedHandle.Reset();
	}
}

void FSelectedActorViewportOverlayBase::UnbindWorldDelegates()
{
	if (PreLevelRemovedHandle.IsValid())
	{
		FWorldDelegates::PreLevelRemovedFromWorld.Remove(PreLevelRemovedHandle);
		PreLevelRemovedHandle.Reset();
	}

	if (WorldBeginTearDownHandle.IsValid())
	{
		FWorldDelegates::OnWorldBeginTearDown.Remove(WorldBeginTearDownHandle);
		WorldBeginTearDownHandle.Reset();
	}

	if (WorldCleanupHandle.IsValid())
	{
		FWorldDelegates::OnWorldCleanup.Remove(WorldCleanupHandle);
		WorldCleanupHandle.Reset();
	}
}

bool FSelectedActorViewportOverlayBase::IsTrackingActorInLevel(const ULevel* Level, const UWorld* World) const
{
	const AActor* Actor = TargetActor.Get();
	const ULevel* ActorLevel = Actor ? Actor->GetLevel() : TargetLevel.Get();
	const UWorld* ActorWorld = Actor ? Actor->GetWorld() : TargetWorld.Get();

	return (!Level || ActorLevel == Level) && (!World || ActorWorld == World);
}

bool FSelectedActorViewportOverlayBase::IsTrackingActorInWorld(const UWorld* World) const
{
	const AActor* Actor = TargetActor.Get();
	const UWorld* ActorWorld = Actor ? Actor->GetWorld() : TargetWorld.Get();
	return World && ActorWorld == World;
}

void FSelectedActorViewportOverlayBase::CacheTargetActorWorldContext(AActor* Actor)
{
	TargetLevel = Actor ? Actor->GetLevel() : nullptr;
	TargetWorld = Actor ? Actor->GetWorld() : nullptr;
}

void FSelectedActorViewportOverlayBase::ClearTargetActorWorldContext()
{
	TargetLevel.Reset();
	TargetWorld.Reset();
}

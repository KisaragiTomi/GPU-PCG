#include "CSBrushEdModeBase.h"

#include "Components/BrushComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/ModelComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorViewportClient.h"
#include "Engine/CollisionProfile.h"
#include "Engine/HitResult.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "FoliageHelper.h"
#include "FoliageInstancedStaticMeshComponent.h"
#include "InstancedFoliage.h"
#include "InstancedFoliageActor.h"
#include "InputCoreTypes.h"
#include "LandscapeHeightfieldCollisionComponent.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "SceneView.h"

namespace
{
// Same allow/deny shape as the foliage painter: landscape and plain static meshes are paintable
// surfaces, BSP / foliage / translucent are not. The target's own painted instances are excluded
// so a stroke never treats what it just placed as ground.
struct FCSBrushGeometryFilter
{
	TWeakObjectPtr<AActor> TargetActor;
	bool bAllowLandscape = true;
	bool bAllowStaticMesh = true;
	bool bAllowBSP = false;
	bool bAllowFoliage = false;
	bool bAllowTranslucent = false;

	bool operator()(const UPrimitiveComponent* Component) const
	{
		if (!Component) return false;

		const AActor* Owner = Component->GetOwner();
		if (Owner == TargetActor.Get() && Component->IsA<UInstancedStaticMeshComponent>()) return false;

		const bool bFoliageOwned = Owner && FFoliageHelper::IsOwnedByFoliage(Owner);
		bool bAllowed =
			(bAllowLandscape && Component->IsA<ULandscapeHeightfieldCollisionComponent>()) ||
			(bAllowStaticMesh && Component->IsA<UStaticMeshComponent>() && !Component->IsA<UFoliageInstancedStaticMeshComponent>() && !bFoliageOwned) ||
			(bAllowBSP && (Component->IsA<UBrushComponent>() || Component->IsA<UModelComponent>())) ||
			(bAllowFoliage && (Component->IsA<UFoliageInstancedStaticMeshComponent>() || bFoliageOwned));

		const UMaterialInterface* Material = Component->GetMaterial(0);
		bAllowed &= bAllowTranslucent || !(Material && IsTranslucentBlendMode(*Material));

		return bAllowed;
	}
};
}

FCSBrushEdModeBase::FCSBrushEdModeBase()
{
	CreateBrushComponent();
}

FCSBrushEdModeBase::~FCSBrushEdModeBase()
{
	DestroyBrushComponent();
}

void FCSBrushEdModeBase::Enter()
{
	FEdMode::Enter();
	CreateBrushComponent();
}

void FCSBrushEdModeBase::Exit()
{
	CancelStroke();
	DestroyBrushComponent();
	ClearBrushTarget();
	FEdMode::Exit();
}

void FCSBrushEdModeBase::AddReferencedObjects(FReferenceCollector& Collector)
{
	FEdMode::AddReferencedObjects(Collector);
	Collector.AddReferencedObject(SphereBrushComponent);
	Collector.AddReferencedObject(BrushMID);
}

void FCSBrushEdModeBase::ResetBrushState()
{
	CancelStroke();
	bBrushTraceValid = false;
}

bool FCSBrushEdModeBase::MouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y)
{
	UpdateBrushTraceFromMouse(ViewportClient, Viewport, x, y);
	UpdateBrushComponent(ViewportClient);
	if (!bStrokeActive) return false;
	UpdateStroke();
	return true;
}

bool FCSBrushEdModeBase::CapturedMouseMove(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 x, int32 y)
{
	return MouseMove(ViewportClient, Viewport, x, y);
}

bool FCSBrushEdModeBase::InputKey(FEditorViewportClient* ViewportClient, FViewport* Viewport, FKey Key, EInputEvent Event)
{
	if (!GetBrushTargetActor()) return false;

	if (Key == EKeys::Escape && Event == IE_Pressed)
	{
		CancelStroke();
		ExitTemporaryMode();
		return true;
	}

	if (Key == EKeys::LeftMouseButton && Event == IE_Pressed)
	{
		const bool bMovingCamera =
			Viewport->KeyState(EKeys::MiddleMouseButton) ||
			Viewport->KeyState(EKeys::RightMouseButton) ||
			Viewport->KeyState(EKeys::LeftAlt) ||
			Viewport->KeyState(EKeys::RightAlt);

		if (!bMovingCamera && ViewportClient->GetCurrentWidgetAxis() == EAxisList::None)
		{
			BeginStroke();
			return true;
		}
	}

	if (Key == EKeys::LeftMouseButton && Event == IE_Released && bStrokeActive)
	{
		CommitStroke();
		return true;
	}

	return FEdMode::InputKey(ViewportClient, Viewport, Key, Event);
}

void FCSBrushEdModeBase::Tick(FEditorViewportClient* ViewportClient, float DeltaTime)
{
	FEdMode::Tick(ViewportClient, DeltaTime);
	UpdateBrushComponent(ViewportClient);
	DrawPendingPreview();
}

void FCSBrushEdModeBase::CreateBrushComponent()
{
	if (SphereBrushComponent) return;

	UStaticMesh* BrushMesh = nullptr;
	UMaterial* BrushMaterial = nullptr;
	if (!IsRunningCommandlet())
	{
		BrushMaterial = LoadObject<UMaterial>(nullptr, TEXT("/Engine/EditorLandscapeResources/FoliageBrushSphereMaterial.FoliageBrushSphereMaterial"));
		BrushMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/EngineMeshes/Sphere.Sphere"));
	}

	BrushMID = BrushMaterial ? UMaterialInstanceDynamic::Create(BrushMaterial, GetTransientPackage()) : nullptr;
	SphereBrushComponent = NewObject<UStaticMeshComponent>(GetTransientPackage(), TEXT("CSBrushSphere"));
	SphereBrushComponent->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	SphereBrushComponent->SetCollisionObjectType(ECC_WorldDynamic);
	SphereBrushComponent->SetStaticMesh(BrushMesh);
	if (BrushMID) SphereBrushComponent->SetMaterial(0, BrushMID);
	SphereBrushComponent->SetAbsolute(true, true, true);
	SphereBrushComponent->CastShadow = false;
	SphereBrushComponent->SetVisibility(false);
}

void FCSBrushEdModeBase::DestroyBrushComponent()
{
	if (SphereBrushComponent && SphereBrushComponent->IsRegistered()) SphereBrushComponent->UnregisterComponent();
	SphereBrushComponent = nullptr;
	BrushMID = nullptr;
}

void FCSBrushEdModeBase::UpdateBrushComponent(FEditorViewportClient* ViewportClient)
{
	if (!SphereBrushComponent) CreateBrushComponent();

	AActor* Target = GetBrushTargetActor();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!SphereBrushComponent || !World || !ViewportClient || !bBrushTraceValid)
	{
		if (SphereBrushComponent && SphereBrushComponent->IsRegistered()) SphereBrushComponent->SetVisibility(false);
		return;
	}

	const float BrushRadius = FMath::Max(1.0f, GetBrushSettings().Radius);
	SphereBrushComponent->SetRelativeTransform(FTransform(FQuat::Identity, BrushLocation, FVector(BrushRadius * 0.00625f)));
	SphereBrushComponent->SetVisibility(true);
	if (!SphereBrushComponent->IsRegistered()) SphereBrushComponent->RegisterComponentWithWorld(World);
}

bool FCSBrushEdModeBase::UpdateBrushTraceFromMouse(FEditorViewportClient* ViewportClient, FViewport* Viewport, int32 MouseX, int32 MouseY)
{
	if (!ViewportClient || !Viewport)
	{
		bBrushTraceValid = false;
		BrushViewState = nullptr;
		return false;
	}

	// Recorded here rather than at stroke start: this is the one place that knows which viewport
	// the cursor is over, and the brush follows the cursor across viewports.
	BrushViewState = ViewportClient->ViewState.GetReference();

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		ViewportClient->Viewport,
		ViewportClient->GetScene(),
		ViewportClient->EngineShowFlags)
		.SetRealtimeUpdate(ViewportClient->IsRealtime()));

	FSceneView* View = ViewportClient->CalcSceneView(&ViewFamily);
	FViewportCursorLocation MouseViewportRay(View, ViewportClient, MouseX, MouseY);
	BrushTraceDirection = MouseViewportRay.GetDirection();

	FVector BrushTraceStart = MouseViewportRay.GetOrigin();
	if (ViewportClient->IsOrtho()) BrushTraceStart += -WORLD_MAX * BrushTraceDirection;

	return TraceBrushRay(ViewportClient, BrushTraceStart, BrushTraceDirection);
}

bool FCSBrushEdModeBase::TraceBrushRay(FEditorViewportClient* ViewportClient, const FVector& RayOrigin, const FVector& RayDirection)
{
	bBrushTraceValid = false;
	AActor* Target = GetBrushTargetActor();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World || !ViewportClient || ViewportClient->IsMovingCamera() || !ViewportClient->IsVisible()) return false;

	const FVector TraceStart = RayOrigin;
	const FVector TraceEnd = RayOrigin + RayDirection * HALF_WORLD_MAX;
	FHitResult Hit;
	if (!TraceCandidatePoint(TraceStart, TraceEnd, Hit) || !IsPointAllowed(Hit.ImpactPoint)) return false;

	// The cursor hit is deliberately NOT run through the surface filters: the sphere stays visible
	// on a surface that cannot be painted so the user can see why nothing lands there. BrushNormal
	// orients the sample disc, so it has to be the real surface normal, not the sweep normal.
	BrushLocation = Hit.ImpactPoint;
	BrushNormal = Hit.ImpactNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	bBrushTraceValid = true;
	return true;
}

bool FCSBrushEdModeBase::TraceCandidatePoint(const FVector& Start, const FVector& End, FHitResult& OutHit) const
{
	AActor* Target = GetBrushTargetActor();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World) return false;

	FCSBrushGeometryFilter Filter;
	Filter.TargetActor = Target;
	FFoliageTraceFilterFunc TraceFilterFunc = [Filter](const UPrimitiveComponent* Component)
	{
		return Filter(Component);
	};

	static const FName NAME_CSBrush(TEXT("CSBrush"));
	const FDesiredFoliageInstance DesiredInstance(Start, End, nullptr, FMath::Max(0.0f, GetBrushSettings().TraceRadius));
	return AInstancedFoliageActor::FoliageTrace(
		World,
		OutHit,
		DesiredInstance,
		NAME_CSBrush,
		false,
		TraceFilterFunc,
		true);
}

bool FCSBrushEdModeBase::IsSurfacePaintable(const FHitResult& Hit, const FVector& TraceDirection, const FCSBrushSettings& Settings) const
{
	if (!Settings.bRejectBackFaces) return true;

	// ImpactNormal is the normal of the triangle actually struck; Hit.Normal is the sweep normal,
	// which for a non-zero TraceRadius points along the sweep rather than along the surface.
	const FVector SurfaceNormal = Hit.ImpactNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

	// A ray only leaves geometry through a face pointing back at it. A face pointing the same way
	// the ray travels was hit from behind, so the sample sits on the inside of the mesh.
	return FVector::DotProduct(SurfaceNormal, TraceDirection) <= 0.0;
}

void FCSBrushEdModeBase::BeginStroke()
{
	CancelStroke();
	bStrokeActive = true;
	SamplePendingPoints();
	DrawPendingPreview();
}

void FCSBrushEdModeBase::UpdateStroke()
{
	SamplePendingPoints();
	DrawPendingPreview();
}

void FCSBrushEdModeBase::CommitStroke()
{
	// Called unconditionally: a leaf that samples on the GPU has nothing in the pending set and
	// still needs to know the stroke ended.
	CommitSamples(PendingSamples);

	const bool bExitAfterCommit = GetBrushTargetActor() && GetBrushSettings().bExitAfterCommit;
	CancelStroke();
	if (bExitAfterCommit) ExitTemporaryMode();
}

void FCSBrushEdModeBase::CancelStroke()
{
	bStrokeActive = false;
	PendingSamples.Reset();
}

void FCSBrushEdModeBase::ExitTemporaryMode()
{
	if (FEditorModeTools* ModeTools = GetModeManager()) ModeTools->DeactivateMode(GetID());
}

void FCSBrushEdModeBase::SamplePendingPoints()
{
	if (!GetBrushTargetActor() || !IsReadyToPaint() || !bBrushTraceValid) return;

	const FCSBrushSettings Settings = GetBrushSettings();
	const float BrushRadius = FMath::Max(1.0f, Settings.Radius);
	const int32 SampleCount = FMath::Max(1, Settings.SamplesPerMouseMove);
	for (int32 SampleIndex = 0; SampleIndex < SampleCount; ++SampleIndex)
	{
		FVector Start = FVector::ZeroVector;
		FVector End = FVector::ZeroVector;
		GetRandomVectorInBrush(BrushRadius, Start, End);

		FHitResult Hit;
		if (!TraceCandidatePoint(Start, End, Hit)) continue;
		if (!IsSurfacePaintable(Hit, (End - Start).GetSafeNormal(), Settings)) continue;
		// ImpactPoint is on the surface; Hit.Location is the sweep centre, which floats a
		// TraceRadius above it.
		if (!IsCandidatePointAllowed(Hit.ImpactPoint, Settings.MinSpacing)) continue;

		FCSBrushSample& Sample = PendingSamples.AddDefaulted_GetRef();
		Sample.Location = Hit.ImpactPoint;
		Sample.Normal = Hit.ImpactNormal.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	}
}

bool FCSBrushEdModeBase::IsCandidatePointAllowed(const FVector& Location, float MinSpacing) const
{
	if (!IsPointAllowed(Location)) return false;

	const float SafeMinSpacing = FMath::Max(0.0f, MinSpacing);
	if (SafeMinSpacing <= 0.0f) return true;

	const float MinSpacingSq = FMath::Square(SafeMinSpacing);
	return !IsTooCloseToPending(Location, MinSpacingSq) && !IsTooCloseToCommitted(Location, MinSpacingSq);
}

bool FCSBrushEdModeBase::IsTooCloseToPending(const FVector& Location, float MinSpacingSq) const
{
	for (const FCSBrushSample& Sample : PendingSamples)
	{
		if (FVector::DistSquared(Sample.Location, Location) < MinSpacingSq) return true;
	}
	return false;
}

void FCSBrushEdModeBase::GetRandomVectorInBrush(float BrushRadius, FVector& OutStart, FVector& OutEnd) const
{
	const float Ru = (2.0f * FMath::FRand()) - 1.0f;
	const float Rv = ((2.0f * FMath::FRand()) - 1.0f) * FMath::Sqrt(FMath::Max(1.0f - FMath::Square(Ru), 0.0f));

	FVector U = FVector::ForwardVector;
	FVector V = FVector::RightVector;
	BrushNormal.FindBestAxisVectors(U, V);
	const FVector Point = (Ru * U) + (Rv * V);
	const FVector Rw = FMath::Sqrt(FMath::Max(1.0f - (FMath::Square(Ru) + FMath::Square(Rv)), 0.001f)) * BrushNormal;

	OutStart = BrushLocation + BrushRadius * (Point + Rw);
	OutEnd = BrushLocation + BrushRadius * (Point - Rw);
}

void FCSBrushEdModeBase::DrawPendingPreview() const
{
	AActor* Target = GetBrushTargetActor();
	UWorld* World = Target ? Target->GetWorld() : nullptr;
	if (!World || PendingSamples.IsEmpty()) return;

	const FCSBrushSettings Settings = GetBrushSettings();
	const float PreviewPointSize = FMath::Max(0.1f, Settings.PreviewPointSize);
	const float PreviewLifetime = FMath::Max(0.01f, Settings.PreviewLifetime);
	for (const FCSBrushSample& Sample : PendingSamples)
	{
		DrawDebugPoint(World, Sample.Location, PreviewPointSize, Settings.PreviewColor, false, PreviewLifetime, SDPG_World);
	}
}

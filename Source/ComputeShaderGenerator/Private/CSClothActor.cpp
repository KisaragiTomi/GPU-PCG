#include "CSClothActor.h"

#include "CSClothSimComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"

#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "CoreGlobals.h"          // IsRunningCommandlet
#include "RHIGlobals.h"           // GUsingNullRHI
#include "UObject/UnrealType.h"   // FPropertyChangedEvent

ACSClothActor::ACSClothActor()
{
	// 编辑器里也要 tick（ShouldTickIfViewportsOnly）：首次上传和属性改动后的重建都在 tick 里做，
	// 一来 OnConstruction 在拖动时会连着来，二来关卡加载时它不保证被调到。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	RenderComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("ClothRender"));
	RenderComponent->SetupAttachment(Root);
	// 布每帧都在变形而变换不动：VSM 的缓存页按变换失效，不标 Always 影子就停在 rest 姿态上。
	RenderComponent->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;

	ClothComponent = CreateDefaultSubobject<UCSClothSimComponent>(TEXT("ClothSim"));
}

void ACSClothActor::BuildGridPinBoxes(TArray<FCSClothPinBox>& OutPinBoxes) const
{
	OutPinBoxes.Reset();
	if (SourceMesh || GridPin == ECSClothGridPin::None) return;

	// 钉盒比一格稍大：刚好罩住那一排 / 那几个格点，而不会吃到相邻的一排。
	const FVector2D Half = GridSize * 0.5;
	const FVector2D Cell(GridSize.X / FMath::Max(GridResolution.X, 1), GridSize.Y / FMath::Max(GridResolution.Y, 1));
	const FVector Pad(Cell.X * 0.25, Cell.Y * 0.25, 1.0);
	auto AddBox = [&OutPinBoxes, &Pad](const FVector& Min, const FVector& Max)
	{
		FCSClothPinBox& Pin = OutPinBoxes.AddDefaulted_GetRef();
		Pin.Box = FBox(Min - Pad, Max + Pad);
	};
	const FVector C00(-Half.X, -Half.Y, 0.0), C01(-Half.X, Half.Y, 0.0), C10(Half.X, -Half.Y, 0.0), C11(Half.X, Half.Y, 0.0);
	switch (GridPin)
	{
	case ECSClothGridPin::EdgeMinX:
		AddBox(C00, C01);
		break;
	case ECSClothGridPin::CornersMinX:
		AddBox(C00, C00);
		AddBox(C01, C01);
		break;
	case ECSClothGridPin::FourCorners:
		AddBox(C00, C00);
		AddBox(C01, C01);
		AddBox(C10, C10);
		AddBox(C11, C11);
		break;
	default:
		break;
	}
}

bool ACSClothActor::CanBuildCloth() const
{
	const UWorld* World = GetWorld();
	if (!World || IsTemplate() || IsRunningCommandlet() || GUsingNullRHI) return false;
	return World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::EditorPreview || World->IsGameWorld();
}

void ACSClothActor::RebuildCloth()
{
	if (!RenderComponent || !ClothComponent || !CanBuildCloth()) return;
	bRebuildPending = false;

	// 网格挂在渲染组件下：显存归它，组件销毁时自己还（见 UCSMeshRenderComponent 的类注释）。
	if (!ClothMesh) ClothMesh = NewObject<UCSMesh>(RenderComponent, NAME_None, RF_Transient);

	UMaterialInterface* DrawMaterial = Material;
	bool bUploaded = false;
	if (SourceMesh)
	{
		FCSMeshFromStaticMeshOptions Options;
		Options.Transform = GetActorTransform();
		UCSMeshOps::CopyFromStaticMesh(ClothMesh, SourceMesh, Options);
		bUploaded = !ClothMesh->IsEmpty();
		if (!DrawMaterial) DrawMaterial = SourceMesh->GetMaterial(0);
	}
	else
	{
		FCSGpuMeshCPUData Snapshot;
		CSClothSetup::BuildGridSnapshot(GridSize, GridResolution, GetActorTransform(), bDoubleSided, Snapshot);
		bUploaded = UCSMeshOps::CopyFromMeshSnapshot(ClothMesh, Snapshot);
	}
	if (!bUploaded)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s: nothing to simulate, the source geometry did not upload."), *GetName());
		return;
	}

	RenderComponent->MeshMaterial = DrawMaterial;
	RenderComponent->SetGpuMesh(ClothMesh);

	TArray<FCSClothPinBox> GridPins;
	BuildGridPinBoxes(GridPins);
	ClothComponent->SetExtraPinBoxes(GridPins);
	ClothComponent->SetTargetMesh(ClothMesh);
	ClothComponent->SetupFromMeshSync();
}

void ACSClothActor::ResetCloth()
{
	if (ClothComponent) ClothComponent->ResetToRestPose();
}

void ACSClothActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (!ClothMesh || bRebuildPending) RebuildCloth();
}

void ACSClothActor::BeginPlay()
{
	Super::BeginPlay();
	// PIE / 运行时的副本不带 transient 的网格过来，也不跑 OnConstruction：在这里建，第一帧就有布。
	if (!ClothMesh || bRebuildPending) RebuildCloth();
}

void ACSClothActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// 兜底：编辑器里加载进来的关卡不重跑 OnConstruction，第一次 tick 时补建。
	if (!ClothMesh || bRebuildPending) RebuildCloth();
}

#if WITH_EDITOR
void ACSClothActor::BakeCurrentPoseToStaticMesh()
{
	if (!RenderComponent || !ClothMesh) return;
	RenderComponent->SaveToStaticMesh(GetActorTransform(), TEXT(""), true, false, true, false, ClothMesh);
}

void ACSClothActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 只有决定几何的属性才重建；挪动 actor（变换属性）不重建，钉点跟着走、布自己摆。
	static const FName SourceProperties[] = {
		GET_MEMBER_NAME_CHECKED(ACSClothActor, SourceMesh),
		GET_MEMBER_NAME_CHECKED(ACSClothActor, Material),
		GET_MEMBER_NAME_CHECKED(ACSClothActor, GridSize),
		GET_MEMBER_NAME_CHECKED(ACSClothActor, GridResolution),
		GET_MEMBER_NAME_CHECKED(ACSClothActor, bDoubleSided),
		GET_MEMBER_NAME_CHECKED(ACSClothActor, GridPin),
	};
	const FName Changed = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	for (const FName& Name : SourceProperties)
	{
		if (Name != Changed) continue;
		bRebuildPending = true;
		break;
	}
}

void ACSClothActor::PostEditUndo()
{
	Super::PostEditUndo();
	bRebuildPending = true;
}
#endif

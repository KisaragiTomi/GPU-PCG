#include "CSHouseRidgeHandleActor.h"

#include "CSHouseActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ACSHouseRidgeHandleActor::ACSHouseRidgeHandleActor()
{
	// 根组件、不 tick、Movable 都由基类构造好了。示意箭头与推墙那只同一套配法（`ACSHouseResizeHandleActor`）。
	ArrowComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Arrow"));
	ArrowComponent->SetupAttachment(RootComponent);
	MakeEditorGizmoProp(ArrowComponent);

	ArrowOutlineComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ArrowOutline"));
	ArrowOutlineComponent->SetupAttachment(ArrowComponent);
	MakeEditorGizmoProp(ArrowOutlineComponent);

	// 先装引擎锥子兜底；TG 箭头在 `InitializeHandle` 里惰性换上（理由见 `ACSHouseResizeHandleActor` 的构造）。
	static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (UStaticMesh* Mesh = ConeMesh.Get()) ArrowComponent->SetStaticMesh(Mesh);
}

void ACSHouseRidgeHandleActor::InitializeHandle(ACSHouseActor* InHost, int32 InEnd)
{
	SetHost(InHost);
	End = InEnd & 1;

	// TG 的脊长标识就是这一只：`brush_arrow_flat_single_foward`（`do_ui_edit_roof_ridge_length` 的 signifier 闭包
	// 1421A9EF0），它没有配描边网格。还没烘（`Scripts/TinyGladeMakeHandleArrows.py`）就退到推墙那一对，
	// 再没有就留构造里的锥子 —— 观感逐级降，交互不受影响。
	if (UStaticMesh* RidgeArrow = LoadTGArrowMesh(TEXT("brush_arrow_flat_single_foward")))
	{
		ArrowComponent->SetStaticMesh(RidgeArrow);
		ArrowOutlineComponent->SetStaticMesh(nullptr);
		bTGArrow = true;
		FallbackScale = 1.0f;
	}
	else if (UStaticMesh* WallArrow = LoadTGArrowMesh(TEXT("flat_arrow")))
	{
		ArrowComponent->SetStaticMesh(WallArrow);
		ArrowOutlineComponent->SetStaticMesh(LoadTGArrowMesh(TEXT("flat_arrow_outline")));
		ApplyOutlineMaterial(ArrowOutlineComponent);
		bTGArrow = true;
		// `flat_arrow` 长 100 cm，脊长那只长 57.4 cm：按长度折算，两者画出来一样长。
		FallbackScale = 0.574f;
	}
	ApplyHighlightMaterial(ArrowComponent);

	SnapToCanonical();
}

FVector ACSHouseRidgeHandleActor::GetOutwardWorld() const
{
	const ACSHouseActor* H = Host.Get();
	FCSHouseRidgeFrame Ridge;
	if (!H || !H->GetRoofRidgeFrame(Ridge)) return FVector::ZeroVector;
	const FVector2D Outward = Ridge.Direction * (End == 0 ? -1.0 : 1.0);
	return FRotator(0.0, H->GetActorRotation().Yaw, 0.0).RotateVector(FVector(Outward.X, Outward.Y, 0.0));
}

FVector ACSHouseRidgeHandleActor::ComputeCanonicalWorldLocation() const
{
	const ACSHouseActor* H = Host.Get();
	FCSHouseRidgeFrame Ridge;
	// 宿主没了 / 脊长不是参数：原地不动（跳回世界原点会让那一瞬间看起来像"抓手飞走了"）。
	if (!H || !H->GetRoofRidgeFrame(Ridge)) return GetActorLocation();

	const FVector2D Outward = Ridge.Direction * (End == 0 ? -1.0 : 1.0);
	const FVector2D XY = Ridge.Centre + Outward * (Ridge.Length * 0.5 + double(HandleOffset));
	// 脊高 = 檐口高 + 屋面整体偏移 + 起伏；与屋顶高度框（`ACSHouseHeightHandleActor`）同一个口径，只是浮得低一些。
	const double Z = double(H->WallHeight + H->RoofHeightOffset + H->GetRoofRise() + HandleLift);
	return H->GetActorTransform().TransformPosition(FVector(XY.X, XY.Y, Z));
}

void ACSHouseRidgeHandleActor::SnapToCanonical()
{
	SetActorLocation(ComputeCanonicalWorldLocation());

	const ACSHouseActor* H = Host.Get();
	FCSHouseRidgeFrame Ridge;
	const bool bApplicable = H && H->GetRoofRidgeFrame(Ridge) && !H->IsFlatRoof();
	if (H)
	{
		// 朝向归位：我们从不读 actor 的旋转，用户转它无害 —— 但箭头会指歪。
		SetActorRotation(H->GetActorRotation());
		if (bApplicable)
		{
			const FVector2D Outward = Ridge.Direction * (End == 0 ? -1.0 : 1.0);
			UpdateArrowPose(FVector(Outward.X, Outward.Y, 0.0));
		}
	}
	// 脊长不是参数（异形 footprint / 露台）时藏起来：留着一只拖不动的箭头只会让人以为坏了。
	// 只动可见性不销毁 —— 拉尺寸把房子拖回坡屋顶、或把形状改回矩形时它要原样回来。
	if (ArrowComponent) ArrowComponent->SetVisibility(bApplicable, /*bPropagateToChildren*/ true);

	// 记账量与摆位**必须一起更新**（同推墙箭头）：只摆位不重置，下一次事件会把程序制造的位移当成用户拖的。
	LastConsumedWorld = GetActorLocation();
}

void ACSHouseRidgeHandleActor::UpdateArrowPose(const FVector& OutwardLocal)
{
	const UStaticMesh* Mesh = ArrowComponent ? ArrowComponent->GetStaticMesh() : nullptr;
	if (!Mesh) return;

	// TG 箭头（两张都是）躺平、头指网格局部 +Y、+Z 是厚度；兜底锥子尖指 +Z
	// （与 `ACSHouseResizeHandleActor::UpdateArrowPose` 同一套摆法）。
	const FRotator Rotation = bTGArrow
		? FRotationMatrix::MakeFromYZ(OutwardLocal, FVector::UpVector).Rotator()
		: FRotationMatrix::MakeFromZ(OutwardLocal).Rotator();
	const FVector Scale = bTGArrow ? FVector(ArrowScale * FallbackScale) : FVector(0.3, 0.3, 0.45);
	const FVector Centre = Rotation.RotateVector(Mesh->GetBounds().Origin * Scale);
	ArrowComponent->SetRelativeTransform(FTransform(Rotation, -Centre, Scale));
}

float ACSHouseRidgeHandleActor::ConsumeDragToHost(bool bFinished)
{
	// 走基类那条唯一执行面 —— 无宿主自毁、最终裁决的时机判定都在那儿，全族一份。
	LastAppliedOffset = 0.0f;
	HandleDrag(bFinished);
	return LastAppliedOffset;
}

bool ACSHouseRidgeHandleActor::OnHandleDrag(bool bFinished)
{
	ACSHouseActor* H = Host.Get();
	// 返回 false ⇒ 基类在 `bFinished` 时按 `bDestroyWhenHostless` 自毁。
	if (!H) return false;

	// 只取沿脊向（朝外）的分量：本端往外走 δ ⇒ 脊居中、两头各伸 δ ⇒ 脊长 + 2δ。
	// 用「当前位置 − 上次消费过的位置」而不是「当前位置 − 规范位置」，理由与推墙箭头逐字相同：
	// 顶在 0 / 1 上时生效量 ≠ 请求量，拿请求值记账残差会一路累积。
	const FVector Outward = GetOutwardWorld();
	const float Offset = float(FVector::DotProduct(GetActorLocation() - LastConsumedWorld, Outward));
	const float Applied = H->PushRidgeLength(Offset * 2.0f, bFinished);

	// 全部抓手统一重摆（含自己与另一头那只）：脊长变了两只箭头都要走，拖歪的分量也在这里清掉。
	H->SnapResizeHandles();

	LastAppliedOffset = Applied;
	return true;
}

void ACSHouseRidgeHandleActor::OnDetachFromHost()
{
	// 房子那张表要把这一格清掉（同另外两族抓手）。基类保证 `Destroyed` 与 `EndPlay` 两条路都会走到这里，且幂等。
	if (ACSHouseActor* H = Host.Get())
	{
		H->NotifyResizeHandleDestroyed(this);
	}
}

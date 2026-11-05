// -----------------------------------------------------------------------------
// AMeshGeneratorBrushCache 实现
//
// 原先整段住在 ComputeShaderMeshGenerator.cpp（该文件一度 6677 行）；2026-08 整理
// 归位到本文件。现在只剩实例笔刷（StartInstanceBrush / paint 组件）。
// 同住过的体素分页三角形缓存产出全程无人消费：2026-08-12 删过一次（fd95aff），被 08-14 的
// 工作区快照提交带回，2026-11-05 再次删除。设计记录见 Docs/ComputeShaderMeshGeneratorVoxelTriangleCache.md。
// -----------------------------------------------------------------------------

#include "MeshGeneratorBrushCache.h"
#include "MeshGeneratorInternal.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "DrawDebugHelpers.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"
#include "UObject/Package.h"

using namespace CSMeshGenInternal;

FCSInstanceBrushEditorRequest AMeshGeneratorBrushCache::OnInstanceBrushEditorRequest;


// -----------------------------------------------------------------------------
// Brush System
// -----------------------------------------------------------------------------

void AMeshGeneratorBrushCache::StartInstanceBrush()
{
#if WITH_EDITOR
	if (!InstanceBrushMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] InstanceBrushMesh is not set. Actor=%s"),
			*GetNameSafe(this));
	}

	if (OnInstanceBrushEditorRequest.IsBound())
	{
		OnInstanceBrushEditorRequest.Broadcast(this);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[AComputeShaderMeshGenerator::StartInstanceBrush] No editor brush handler is registered. Actor=%s"),
			*GetNameSafe(this));
	}
#endif
}

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::FindPaintComponent(UStaticMesh* Mesh) const
{
	if (!Mesh)
	{
		return nullptr;
	}

	for (const FCSInstancePaintComponentSlot& Slot : PaintedInstanceComponents)
	{
		if (Slot.Mesh == Mesh && Slot.Component)
		{
			return Slot.Component;
		}
	}

	return nullptr;
}

UHierarchicalInstancedStaticMeshComponent* AMeshGeneratorBrushCache::GetOrCreatePaintComponent(UStaticMesh* Mesh)
{
	if (!Mesh)
	{
		return nullptr;
	}

	if (UHierarchicalInstancedStaticMeshComponent* ExistingComponent = FindPaintComponent(Mesh))
	{
		return ExistingComponent;
	}

	const FName BaseName(*FString::Printf(TEXT("PaintedInstances_%s"), *Mesh->GetName()));
	const FName ComponentName = MakeUniqueObjectName(this, UHierarchicalInstancedStaticMeshComponent::StaticClass(), BaseName);
	UHierarchicalInstancedStaticMeshComponent* NewComponent = NewObject<UHierarchicalInstancedStaticMeshComponent>(this, ComponentName, RF_Transactional);
	if (!NewComponent)
	{
		return nullptr;
	}

	NewComponent->SetStaticMesh(Mesh);
	NewComponent->SetMobility(EComponentMobility::Static);
	NewComponent->SetupAttachment(SceneRoot ? SceneRoot.Get() : GetRootComponent());
	AddInstanceComponent(NewComponent);
	NewComponent->RegisterComponent();

	FCSInstancePaintComponentSlot& NewSlot = PaintedInstanceComponents.AddDefaulted_GetRef();
	NewSlot.Mesh = Mesh;
	NewSlot.Component = NewComponent;

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return NewComponent;
}

int32 AMeshGeneratorBrushCache::CommitPaintInstances(const TArray<FTransform>& WorldTransforms, UStaticMesh* Mesh)
{
	if (!Mesh || WorldTransforms.IsEmpty())
	{
		return 0;
	}

	UHierarchicalInstancedStaticMeshComponent* PaintComponent = GetOrCreatePaintComponent(Mesh);
	if (!PaintComponent)
	{
		return 0;
	}

	const int32 PreviousInstanceCount = PaintComponent->GetInstanceCount();
	PaintComponent->AddInstances(WorldTransforms, false, true, false);
	PaintComponent->MarkRenderStateDirty();

#if WITH_EDITOR
	MarkPackageDirty();
#endif

	return PaintComponent->GetInstanceCount() - PreviousInstanceCount;
}

bool AMeshGeneratorBrushCache::IsInstanceBrushPointAllowed(const FVector& WorldPosition) const
{
	if (!bInstanceBrushUseGeneratorBounds)
	{
		return true;
	}

	const FBox Bounds = GetGeneratorBoundsWorldBox();
	return Bounds.IsValid && Bounds.IsInsideOrOn(WorldPosition);
}

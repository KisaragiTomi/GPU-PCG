// Fill out your copyright notice in the Description page of Project Settings.

#include "FoliageConverter.h"

#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "InstancedFoliageActor.h"

void UFoliageConverter::ConvertFoliageToTransformArray(UFoliageType* InFoliageType, AVineContainer* Container)
{
	if (Container)
	{
		Container->ImportFoliageToTransformArray(InFoliageType);
	}
}

void UFoliageConverter::ConvertTransformArrayToFoliage(UFoliageType* InFoliageType, AVineContainer* Container)
{
	if (Container)
	{
		Container->ExportTransformArrayToFoliage(InFoliageType);
	}
}

UStaticMesh* UFoliageConverter::GetStaticMesh(UFoliageType* InFoliageType)
{
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo || !FoliageInfo->GetComponent())
		{
			continue;
		}

		if (UStaticMesh* StaticMesh = FoliageInfo->GetComponent()->GetStaticMesh())
		{
			return StaticMesh;
		}
	}
	return nullptr;
}

const UFoliageType* UFoliageConverter::GetFoliageType(UStaticMesh* StaticMesh)
{
	AInstancedFoliageActor* InstancedFoliageActor = nullptr;
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		InstancedFoliageActor = *It;
		break;
	}

	if (!InstancedFoliageActor)
	{
		return nullptr;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	return FoliageTypes.Num() > 0 ? FoliageTypes[0] : nullptr;
}

void UFoliageConverter::GetAllInstancesTransform(UFoliageType* InFoliageType, TArray<FTransform>& OutTransforms)
{
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType);
		if (!FoliageInfo || !FoliageInfo->GetComponent() || FoliageInfo->Instances.Num() == 0)
		{
			continue;
		}

		const int32 InstanceCount = FoliageInfo->GetComponent()->GetInstanceCount();
		OutTransforms.Reserve(OutTransforms.Num() + InstanceCount);
		for (int32 Index = 0; Index < InstanceCount; ++Index)
		{
			FTransform Transform;
			FoliageInfo->GetComponent()->GetInstanceTransform(Index, Transform, true);
			OutTransforms.Add(Transform);
		}
	}
}

void UFoliageConverter::RefreshFoliage(UFoliageType* InFoliageType)
{
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		if (FFoliageInfo* FoliageInfo = It->FindInfo(InFoliageType))
		{
			FoliageInfo->Refresh(true, true);
		}
	}
}

const UFoliageType* UFoliageConverter::AddFoliageType(const UFoliageType* InType)
{
	AInstancedFoliageActor* InstancedFoliageActor = nullptr;
	for (TActorIterator<AInstancedFoliageActor> It(GWorld); It; ++It)
	{
		InstancedFoliageActor = *It;
		break;
	}

	if (!InstancedFoliageActor)
	{
		return nullptr;
	}

	FFoliageInfo* Info = nullptr;
	return InstancedFoliageActor->AddFoliageType(InType, &Info);
}

bool UFoliageConverter::RemoveFoliageInstance(UStaticMesh* StaticMesh, const int32 InstanceIndex)
{
	if (!GWorld)
	{
		return false;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return false;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return false;
	}

	if (FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]))
	{
		TArray<int32> InstanceIndices;
		InstanceIndices.Add(InstanceIndex);
		FoliageInfo->RemoveInstances(InstanceIndices, true);
		FoliageInfo->Refresh(true, true);
		return true;
	}

	return false;
}

bool UFoliageConverter::SetFoliageInstanceTransform(UStaticMesh* StaticMesh, const int32 InstanceIndex, const FTransform& Transform)
{
	if (!GWorld)
	{
		return false;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return false;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return false;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]);
	if (!FoliageInfo || !FoliageInfo->Instances.IsValidIndex(InstanceIndex))
	{
		return false;
	}

	TArray<int32> InstanceIndices;
	InstanceIndices.Add(InstanceIndex);
	FoliageInfo->PreMoveInstances(InstanceIndices);

	FFoliageInstance& Instance = FoliageInfo->Instances[InstanceIndex];
	Instance.Location = Transform.GetLocation();
	Instance.Rotation = Transform.GetRotation().Rotator();
	const FVector Scale = Transform.GetScale3D();
	Instance.DrawScale3D = FVector3f(Scale.X, Scale.Y, Scale.Z);

	FoliageInfo->PostMoveInstances(InstanceIndices, false);
	return true;
}

bool UFoliageConverter::SetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32 BaseID)
{
	if (!GWorld)
	{
		return false;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return false;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return false;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]);
	if (!FoliageInfo || !FoliageInfo->Instances.IsValidIndex(InstanceArray))
	{
		return false;
	}

	FoliageInfo->Instances[InstanceArray].BaseId = BaseID;
	return true;
}

bool UFoliageConverter::GetFoliageInstanceID(UStaticMesh* StaticMesh, int32 InstanceArray, int32& BaseID)
{
	BaseID = -1;
	if (!GWorld)
	{
		return false;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return false;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return false;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]);
	if (!FoliageInfo || !FoliageInfo->Instances.IsValidIndex(InstanceArray))
	{
		return false;
	}

	BaseID = FoliageInfo->Instances[InstanceArray].BaseId;
	return true;
}

bool UFoliageConverter::GetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float& CustomDataValue)
{
	if (!InstanceStaticMesh)
	{
		return false;
	}

	const TArray<FInstancedStaticMeshInstanceData>& PerInstanceSMData = InstanceStaticMesh->PerInstanceSMData;
	const int32 NumCustomDataFloats = InstanceStaticMesh->NumCustomDataFloats;
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomDataIndex < 0 || CustomDataIndex >= NumCustomDataFloats)
	{
		return false;
	}

	CustomDataValue = InstanceStaticMesh->PerInstanceSMCustomData[InstanceIndex * NumCustomDataFloats + CustomDataIndex];
	return true;
}

bool UFoliageConverter::SetCustomDataValue(UInstancedStaticMeshComponent* InstanceStaticMesh, int32 InstanceIndex, int32 CustomDataIndex, float CustomDataValue)
{
	if (!InstanceStaticMesh)
	{
		return false;
	}

	const TArray<FInstancedStaticMeshInstanceData>& PerInstanceSMData = InstanceStaticMesh->PerInstanceSMData;
	if (!PerInstanceSMData.IsValidIndex(InstanceIndex) || CustomDataIndex < 0)
	{
		return false;
	}

	if (InstanceStaticMesh->NumCustomDataFloats <= CustomDataIndex)
	{
		InstanceStaticMesh->NumCustomDataFloats = CustomDataIndex + 1;
		InstanceStaticMesh->PerInstanceSMCustomData.SetNumZeroed(PerInstanceSMData.Num() * InstanceStaticMesh->NumCustomDataFloats);
	}

	InstanceStaticMesh->Modify();
	InstanceStaticMesh->PerInstanceSMCustomData[InstanceIndex * InstanceStaticMesh->NumCustomDataFloats + CustomDataIndex] = CustomDataValue;
	InstanceStaticMesh->MarkRenderStateDirty();
	return true;
}

bool UFoliageConverter::AddFoliageInstance(UStaticMesh* StaticMesh, UPrimitiveComponent* AttachComponent, const FTransform& Transform)
{
	if (!GWorld)
	{
		return false;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return false;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return false;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]);
	if (!FoliageInfo)
	{
		return false;
	}

	FFoliageInstance FoliageInstance;
	FoliageInfo->ReserveAdditionalInstances(FoliageTypes[0], 1);
	if (AttachComponent)
	{
		FoliageInstance.BaseComponent = AttachComponent;
	}

	FoliageInstance.Location = Transform.GetLocation();
	FoliageInstance.Rotation = Transform.GetRotation().Rotator();
	const FVector Scale = Transform.GetScale3D();
	FoliageInstance.DrawScale3D = FVector3f(Scale.X, Scale.Y, Scale.Z);
	FoliageInfo->AddInstance(FoliageTypes[0], FoliageInstance);
	FoliageInfo->Refresh(true, true);
	return true;
}

void UFoliageConverter::DistanceSort(TMap<float, int32> InMap, TMap<float, int32>& OutMap)
{
	InMap.KeySort([](float A, float B) { return A > B; });
	OutMap = InMap;
}

UHierarchicalInstancedStaticMeshComponent* UFoliageConverter::FindFoliageInstanceComponent(UStaticMesh* StaticMesh)
{
	if (!GWorld)
	{
		return nullptr;
	}

	ULevel* DesiredLevel = GWorld->GetCurrentLevel();
	AInstancedFoliageActor* InstancedFoliageActor = AInstancedFoliageActor::GetInstancedFoliageActorForLevel(DesiredLevel, true);
	if (!InstancedFoliageActor)
	{
		return nullptr;
	}

	TArray<const UFoliageType*> FoliageTypes;
	InstancedFoliageActor->GetAllFoliageTypesForSource(StaticMesh, FoliageTypes);
	if (FoliageTypes.Num() == 0 || !FoliageTypes[0])
	{
		return nullptr;
	}

	FFoliageInfo* FoliageInfo = InstancedFoliageActor->FindInfo(FoliageTypes[0]);
	return FoliageInfo ? FoliageInfo->GetComponent() : nullptr;
}

void UFoliageConverter::AddFoliage()
{
}

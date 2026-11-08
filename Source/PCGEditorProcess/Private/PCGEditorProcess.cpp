// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGEditorProcess.h"
#include "CSGroundActor.h"
#include "CSCityFieldActor.h"
#include "CSCityFieldPaintEdMode.h"
#include "CSGroundPaintEdMode.h"
#include "CSHouseActor.h"
#include "CSWindowBrushEdMode.h"
#include "CSHouseResizeSelectionWatcher.h"
#include "CSInstanceBrushEdMode.h"
#include "CSPointBrushActor.h"
#include "CSPointBrushEdMode.h"
#include "MeshGeneratorBrushCache.h"
#include "ComputeShaderShallowWater.h"
#include "CSShallowWaterProcess.h"
#include "GPUSkeletalTree.h"
#include "ViewEditCategoryViewportOverlay.h"
#include "Animation/Skeleton.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "Engine/SkeletalMesh.h"
#include "Textures/SlateIcon.h"
#include "MeshDescription.h"
#include "ReferenceSkeleton.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalMeshAttributes.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "Interfaces/IPluginManager.h"

#define LOCTEXT_NAMESPACE "FPCGEditorProcessModule"

namespace
{
struct FPCGTreeBoneData
{
	FName Name;
	int32 ParentIndex = INDEX_NONE;
	FTransform LocalTransform = FTransform::Identity;
	float Depth = 0.0f;
};

void BuildGPUSkeletalTreeBoneHierarchy(const FTreeBranchParams& Params, TArray<FPCGTreeBoneData>& OutBoneHierarchy)
{
	OutBoneHierarchy.Reset();

	const int32 TrunkSegments = FMath::Max(1, Params.TrunkSegments);
	const int32 BranchesPerLevel = FMath::Max(1, Params.BranchesPerLevel);
	const int32 BranchSegments = FMath::Max(1, Params.BranchSegments);
	const float SegHeight = Params.TrunkHeight / TrunkSegments;

	OutBoneHierarchy.Add({FName("Root"), INDEX_NONE, FTransform::Identity, 0.0f});

	for (int32 i = 0; i < TrunkSegments; ++i)
	{
		const FString Name = FString::Printf(TEXT("Trunk_%02d"), i);
		const FTransform Local(FQuat::Identity, FVector(0, 0, SegHeight));
		OutBoneHierarchy.Add({FName(*Name), i, Local, float(i + 1) / TrunkSegments});
	}

	int32 BoneIdx = OutBoneHierarchy.Num();
	for (int32 Level = 2; Level < TrunkSegments; ++Level)
	{
		const int32 TrunkBoneIdx = Level;
		const float AngleStep = 360.0f / BranchesPerLevel;
		const float BaseAngle = Level * 137.5f;

		for (int32 BranchIndex = 0; BranchIndex < BranchesPerLevel; ++BranchIndex)
		{
			const float Angle = FMath::DegreesToRadians(BaseAngle + BranchIndex * AngleStep);
			const float Pitch = FMath::DegreesToRadians(-30.0f - FMath::RandRange(0.0f, 20.0f));
			const FVector Dir(
				FMath::Cos(Angle) * FMath::Cos(Pitch),
				FMath::Sin(Angle) * FMath::Cos(Pitch),
				FMath::Sin(Pitch));
			const float SegLen = Params.BranchLength / BranchSegments;

			int32 ParentIdx = TrunkBoneIdx;
			for (int32 SegmentIndex = 0; SegmentIndex < BranchSegments; ++SegmentIndex)
			{
				const FString Name = FString::Printf(TEXT("Branch_%d_%d_%d"), Level, BranchIndex, SegmentIndex);
				const FTransform Local(FQuat::Identity, Dir * SegLen);
				const float Depth = float(Level) / TrunkSegments + 0.5f * float(SegmentIndex + 1) / BranchSegments;
				OutBoneHierarchy.Add({FName(*Name), ParentIdx, Local, FMath::Clamp(Depth, 0.0f, 1.0f)});
				ParentIdx = BoneIdx++;
			}
		}
	}
}

void AddGPUSkeletalTreeCylinderSegment(
	TArray<FVector3f>& Vertices,
	TArray<int32>& Indices,
	TArray<FVector3f>& Normals,
	TArray<FVector2f>& UVs,
	TArray<int32>& BoneIdxArr,
	TArray<float>& BoneWeightArr,
	const FVector& Start,
	const FVector& End,
	float RadiusStart,
	float RadiusEnd,
	int32 Radial,
	int32 BoneIdxStart,
	int32 BoneIdxEnd)
{
	const int32 BaseVert = Vertices.Num();
	const FVector Axis = (End - Start).GetSafeNormal();
	FVector Perp1;
	FVector Perp2;
	Axis.FindBestAxisVectors(Perp1, Perp2);

	for (int32 Ring = 0; Ring < 2; ++Ring)
	{
		const FVector Center = Ring == 0 ? Start : End;
		const float Radius = Ring == 0 ? RadiusStart : RadiusEnd;
		const float V = Ring == 0 ? 0.0f : 1.0f;
		const int32 BoneIdx = Ring == 0 ? BoneIdxStart : BoneIdxEnd;

		for (int32 r = 0; r <= Radial; ++r)
		{
			const float Angle = 2.0f * PI * r / Radial;
			const FVector Normal = Perp1 * FMath::Cos(Angle) + Perp2 * FMath::Sin(Angle);
			const FVector Pos = Center + Normal * Radius;

			Vertices.Add(FVector3f(Pos));
			Normals.Add(FVector3f(Normal));
			UVs.Add(FVector2f(float(r) / Radial, V));
			BoneIdxArr.Add(BoneIdx);
			BoneWeightArr.Add(1.0f);
		}
	}

	for (int32 r = 0; r < Radial; ++r)
	{
		const int32 A = BaseVert + r;
		const int32 B = BaseVert + r + 1;
		const int32 C = BaseVert + (Radial + 1) + r;
		const int32 D = BaseVert + (Radial + 1) + r + 1;

		Indices.Add(A);
		Indices.Add(C);
		Indices.Add(B);
		Indices.Add(B);
		Indices.Add(C);
		Indices.Add(D);
	}
}
}

void FPCGEditorProcessModule::StartupModule()
{
	// Bind delegates that don't require Slate/LevelEditor
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.BindStatic(&UCSShallowWaterProcess::SaveSWData);
	AMeshGeneratorBrushCache::OnInstanceBrushEditorRequest.AddRaw(this, &FPCGEditorProcessModule::StartInstanceBrush);
	ACSPointBrushActor::OnPointBrushEditorRequest.AddRaw(this, &FPCGEditorProcessModule::StartPointBrush);
	ACSGroundActor::OnGroundPaintEditorRequest.AddRaw(this, &FPCGEditorProcessModule::StartGroundPaint);
	ACSCityFieldActor::OnPaintRequest.AddRaw(this, &FPCGEditorProcessModule::StartCityFieldPaint);
	ACSHouseActor::OnWindowBrushRequest.AddRaw(this, &FPCGEditorProcessModule::StartWindowBrush);
	AGPUSkeletalTree::OnGenerateTreeEditorRequest.BindRaw(this, &FPCGEditorProcessModule::GenerateGPUSkeletalTree);

	// D5 拉尺寸的失选监听。放在这里而不是 InitializeEditorUI：它绑的是 runtime 侧的静态
	// 委托，不碰 Slate / LevelEditor，没有等 PostEngineInit 的理由；对 GEditor 的依赖是
	// 惰性的（只在真有房子进模式时才去挂 USelection，见 UpdateSelectionBinding）。
	HouseResizeSelectionWatcher = MakeUnique<FCSHouseResizeSelectionWatcher>();
	HouseResizeSelectionWatcher->Start();

	// Defer editor UI initialization until the engine is fully loaded.
	// At PostConfigInit, FCoreStyle and LevelEditor are not yet available.
	const ELoadingPhase::Type CurrentPhase = IPluginManager::Get().GetLastCompletedLoadingPhase();
	if (CurrentPhase == ELoadingPhase::None || CurrentPhase < ELoadingPhase::PostEngineInit)
	{
		PostEngineInitHandle = FCoreDelegates::OnPostEngineInit.AddRaw(this, &FPCGEditorProcessModule::InitializeEditorUI);
	}
	else
	{
		InitializeEditorUI();
	}
}

void FPCGEditorProcessModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	if (HouseResizeSelectionWatcher)
	{
		HouseResizeSelectionWatcher->Stop();
		HouseResizeSelectionWatcher.Reset();
	}

	ViewEditCategoryViewportOverlay.Reset();
	AMeshGeneratorBrushCache::OnInstanceBrushEditorRequest.RemoveAll(this);
	ACSPointBrushActor::OnPointBrushEditorRequest.RemoveAll(this);
	ACSGroundActor::OnGroundPaintEditorRequest.RemoveAll(this);
	ACSCityFieldActor::OnPaintRequest.RemoveAll(this);
	ACSHouseActor::OnWindowBrushRequest.RemoveAll(this);
	AGPUSkeletalTree::OnGenerateTreeEditorRequest.Unbind();
	if (!IsEngineExitRequested() && GEditor)
	{
		if (bEditorModeRegistered) FEditorModeRegistry::Get().UnregisterMode(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
		if (bPointBrushModeRegistered) FEditorModeRegistry::Get().UnregisterMode(FCSPointBrushEdMode::EM_CSPointBrush);
		if (bGroundPaintModeRegistered) FEditorModeRegistry::Get().UnregisterMode(FCSGroundPaintEdMode::EM_CSGroundPaint);
		if (bCityFieldPaintModeRegistered) FEditorModeRegistry::Get().UnregisterMode(FCSCityFieldPaintEdMode::EM_CSCityFieldPaint);
		if (bWindowBrushModeRegistered) FEditorModeRegistry::Get().UnregisterMode(FCSWindowBrushEdMode::EM_CSWindowBrush);
	}
	bEditorModeRegistered = false;
	bPointBrushModeRegistered = false;
	bGroundPaintModeRegistered = false;
	bCityFieldPaintModeRegistered = false;
	bWindowBrushModeRegistered = false;
	ACSShallowWaterCapture::OnBakeResultMeshDelegate.Unbind();
}

void FPCGEditorProcessModule::InitializeEditorUI()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::OnPostEngineInit.Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}

	if (IsEngineExitRequested() || IsRunningCommandlet() || !FApp::CanEverRender() || !GEditor)
	{
		return;
	}

	const bool bModeAlreadyRegistered = FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	if (!bModeAlreadyRegistered)
	{
		FEditorModeRegistry::Get().RegisterMode<FCSInstanceBrushEdMode>(
			FCSInstanceBrushEdMode::EM_CSInstanceBrush,
			LOCTEXT("CSInstanceBrushMode", "CS Instance Brush"),
			FSlateIcon(),
			false);
		bEditorModeRegistered = true;
	}

	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSPointBrushEdMode::EM_CSPointBrush))
	{
		FEditorModeRegistry::Get().RegisterMode<FCSPointBrushEdMode>(
			FCSPointBrushEdMode::EM_CSPointBrush,
			LOCTEXT("CSPointBrushMode", "CS Point Brush"),
			FSlateIcon(),
			false);
		bPointBrushModeRegistered = true;
	}

	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSGroundPaintEdMode::EM_CSGroundPaint))
	{
		FEditorModeRegistry::Get().RegisterMode<FCSGroundPaintEdMode>(
			FCSGroundPaintEdMode::EM_CSGroundPaint,
			LOCTEXT("CSGroundPaintMode", "CS Ground Paint"),
			FSlateIcon(),
			false);
		bGroundPaintModeRegistered = true;
	}

	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSCityFieldPaintEdMode::EM_CSCityFieldPaint))
	{
		FEditorModeRegistry::Get().RegisterMode<FCSCityFieldPaintEdMode>(
			FCSCityFieldPaintEdMode::EM_CSCityFieldPaint,
			LOCTEXT("CSCityFieldPaintMode", "CS City Field Paint"),
			FSlateIcon(),
			false);
		bCityFieldPaintModeRegistered = true;
	}

	if (!FEditorModeRegistry::Get().GetFactoryMap().Contains(FCSWindowBrushEdMode::EM_CSWindowBrush))
	{
		FEditorModeRegistry::Get().RegisterMode<FCSWindowBrushEdMode>(
			FCSWindowBrushEdMode::EM_CSWindowBrush,
			LOCTEXT("CSWindowBrushMode", "CS Window Brush"),
			FSlateIcon(),
			false);
		bWindowBrushModeRegistered = true;
	}

	ViewEditCategoryViewportOverlay = MakeUnique<FViewEditCategoryViewportOverlay>();
	ViewEditCategoryViewportOverlay->Start();
}

void FPCGEditorProcessModule::StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor)
{
	AMeshGeneratorBrushCache* BrushCacheActor = Cast<AMeshGeneratorBrushCache>(TargetActor);
	if (!BrushCacheActor || !GEditor)
	{
		return;
	}

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSInstanceBrushEdMode::EM_CSInstanceBrush);
	if (FCSInstanceBrushEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSInstanceBrushEdMode>(FCSInstanceBrushEdMode::EM_CSInstanceBrush))
	{
		BrushMode->SetTargetActor(BrushCacheActor);
	}
}

void FPCGEditorProcessModule::StartPointBrush(ACSPointBrushActor* TargetActor)
{
	if (!TargetActor || !GEditor) return;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSPointBrushEdMode::EM_CSPointBrush);
	if (FCSPointBrushEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSPointBrushEdMode>(FCSPointBrushEdMode::EM_CSPointBrush))
	{
		BrushMode->SetTargetActor(TargetActor);
	}
}

void FPCGEditorProcessModule::StartWindowBrush(ACSHouseActor* TargetActor)
{
	if (!TargetActor || !GEditor) return;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSWindowBrushEdMode::EM_CSWindowBrush);
	if (FCSWindowBrushEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSWindowBrushEdMode>(FCSWindowBrushEdMode::EM_CSWindowBrush))
	{
		BrushMode->SetTargetActor(TargetActor);
	}
}

void FPCGEditorProcessModule::StartGroundPaint(ACSGroundActor* TargetActor)
{
	if (!TargetActor || !GEditor) return;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSGroundPaintEdMode::EM_CSGroundPaint);
	if (FCSGroundPaintEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSGroundPaintEdMode>(FCSGroundPaintEdMode::EM_CSGroundPaint))
	{
		BrushMode->SetTargetActor(TargetActor);
	}
}

void FPCGEditorProcessModule::StartCityFieldPaint(ACSCityFieldActor* TargetActor)
{
	if (!TargetActor || !GEditor) return;

	FEditorModeTools& ModeTools = GLevelEditorModeTools();
	ModeTools.ActivateMode(FCSCityFieldPaintEdMode::EM_CSCityFieldPaint);
	if (FCSCityFieldPaintEdMode* BrushMode = ModeTools.GetActiveModeTyped<FCSCityFieldPaintEdMode>(FCSCityFieldPaintEdMode::EM_CSCityFieldPaint)) BrushMode->SetTargetActor(TargetActor);
}

void FPCGEditorProcessModule::GenerateGPUSkeletalTree(AGPUSkeletalTree* TargetActor)
{
	if (!TargetActor)
	{
		return;
	}

	TArray<FPCGTreeBoneData> BoneHierarchy;
	BuildGPUSkeletalTreeBoneHierarchy(TargetActor->TreeParams, BoneHierarchy);
	if (BoneHierarchy.IsEmpty())
	{
		return;
	}

	USkeleton* GeneratedSkeleton = NewObject<USkeleton>(TargetActor, NAME_None, RF_Transient);
	FReferenceSkeleton RefSkel;
	{
		FReferenceSkeletonModifier Modifier(RefSkel, nullptr);
		for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
		{
			const FPCGTreeBoneData& BoneData = BoneHierarchy[i];
			const FMeshBoneInfo Info(BoneData.Name, BoneData.Name.ToString(), BoneData.ParentIndex);
			Modifier.Add(Info, BoneData.LocalTransform, i == 0);
		}
	}

	{
		FReferenceSkeletonModifier SkelMod(GeneratedSkeleton);
		for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
		{
			const FPCGTreeBoneData& BoneData = BoneHierarchy[i];
			const FMeshBoneInfo Info(BoneData.Name, BoneData.Name.ToString(), BoneData.ParentIndex);
			SkelMod.Add(Info, BoneData.LocalTransform, i == 0);
		}
	}

	TArray<FVector3f> AllVerts;
	TArray<int32> AllIndices;
	TArray<FVector3f> AllNormals;
	TArray<FVector2f> AllUVs;
	TArray<int32> AllBoneIdx;
	TArray<float> AllBoneWeight;

	TArray<FTransform> WorldBonePoses;
	WorldBonePoses.SetNum(BoneHierarchy.Num());
	for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
	{
		if (BoneHierarchy[i].ParentIndex == INDEX_NONE)
		{
			WorldBonePoses[i] = BoneHierarchy[i].LocalTransform;
		}
		else
		{
			WorldBonePoses[i] = BoneHierarchy[i].LocalTransform * WorldBonePoses[BoneHierarchy[i].ParentIndex];
		}
	}

	const FTreeBranchParams& Params = TargetActor->TreeParams;
	const int32 TrunkSegments = FMath::Max(1, Params.TrunkSegments);
	const int32 RadialSegments = FMath::Max(3, Params.RadialSegments);
	for (int32 i = 1; i < BoneHierarchy.Num(); ++i)
	{
		const int32 ParentIdx = BoneHierarchy[i].ParentIndex;
		const FVector Start = WorldBonePoses[ParentIdx].GetLocation();
		const FVector End = WorldBonePoses[i].GetLocation();

		const bool bTrunk = BoneHierarchy[i].Name.ToString().StartsWith(TEXT("Trunk"));
		float RadS;
		float RadE;
		if (bTrunk)
		{
			const int32 TrunkOrdinal = i - 1;
			const int32 ParentOrdinal = ParentIdx == 0 ? 0 : ParentIdx - 1;
			const float T0 = float(ParentOrdinal) / TrunkSegments;
			const float T1 = float(TrunkOrdinal) / TrunkSegments;
			RadS = FMath::Lerp(Params.TrunkRadiusBase, Params.TrunkRadiusTip, T0);
			RadE = FMath::Lerp(Params.TrunkRadiusBase, Params.TrunkRadiusTip, T1);
		}
		else
		{
			RadS = Params.BranchRadius;
			RadE = Params.BranchRadius * 0.4f;
		}

		AddGPUSkeletalTreeCylinderSegment(
			AllVerts,
			AllIndices,
			AllNormals,
			AllUVs,
			AllBoneIdx,
			AllBoneWeight,
			Start,
			End,
			RadS,
			RadE,
			RadialSegments,
			ParentIdx,
			i);
	}

	USkeletalMesh* GeneratedMesh = NewObject<USkeletalMesh>(TargetActor, NAME_None, RF_Transient);
	GeneratedMesh->SetSkeleton(GeneratedSkeleton);
	GeneratedMesh->SetRefSkeleton(RefSkel);

	FMeshDescription MeshDesc;
	FSkeletalMeshAttributes SkelAttrs(MeshDesc);
	SkelAttrs.Register();

	for (int32 i = 0; i < BoneHierarchy.Num(); ++i)
	{
		const FBoneID BoneID = SkelAttrs.CreateBone();
		SkelAttrs.GetBoneNames().Set(BoneID, BoneHierarchy[i].Name);
		SkelAttrs.GetBoneParentIndices().Set(BoneID, BoneHierarchy[i].ParentIndex);
		SkelAttrs.GetBonePoses().Set(BoneID, BoneHierarchy[i].LocalTransform);
	}

	TArray<FVertexID> VertexIDs;
	VertexIDs.Reserve(AllVerts.Num());
	auto VertexPositions = SkelAttrs.GetVertexPositions();
	for (const FVector3f& Position : AllVerts)
	{
		const FVertexID VertexID = MeshDesc.CreateVertex();
		VertexPositions.Set(VertexID, Position);
		VertexIDs.Add(VertexID);
	}

	FSkinWeightsVertexAttributesRef SkinWeights = SkelAttrs.GetVertexSkinWeights();
	for (int32 VertexIndex = 0; VertexIndex < AllVerts.Num(); ++VertexIndex)
	{
		UE::AnimationCore::FBoneWeights BoneWeights;
		BoneWeights.SetBoneWeight(UE::AnimationCore::FBoneWeight(AllBoneIdx[VertexIndex], AllBoneWeight[VertexIndex]));
		SkinWeights.Set(FVertexID(VertexIndex), BoneWeights);
	}

	const FPolygonGroupID PolyGroup = MeshDesc.CreatePolygonGroup();
	auto VertInstanceNormals = SkelAttrs.GetVertexInstanceNormals();
	auto VertInstanceUVs = SkelAttrs.GetVertexInstanceUVs();

	for (int32 TriangleIndex = 0; TriangleIndex < AllIndices.Num(); TriangleIndex += 3)
	{
		TArray<FVertexInstanceID> TriVerts;
		TriVerts.Reserve(3);

		for (int32 Corner = 0; Corner < 3; ++Corner)
		{
			const int32 SourceIndex = AllIndices[TriangleIndex + Corner];
			const FVertexInstanceID VertexInstanceID = MeshDesc.CreateVertexInstance(VertexIDs[SourceIndex]);
			VertInstanceNormals.Set(VertexInstanceID, AllNormals[SourceIndex]);
			VertInstanceUVs.Set(VertexInstanceID, 0, AllUVs[SourceIndex]);
			TriVerts.Add(VertexInstanceID);
		}

		MeshDesc.CreatePolygon(PolyGroup, TriVerts);
	}

	FSkeletalMeshLODInfo& LODInfo = GeneratedMesh->AddLODInfo();
	LODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
	LODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
	LODInfo.LODHysteresis = 0.02f;
	LODInfo.bAllowCPUAccess = true;

	GeneratedMesh->GetImportedModel()->LODModels.Add(new FSkeletalMeshLODModel());
	GeneratedMesh->CreateMeshDescription(0, MoveTemp(MeshDesc));

	USkeletalMesh::FCommitMeshDescriptionParams CommitParams;
	CommitParams.bMarkPackageDirty = false;
	GeneratedMesh->CommitMeshDescription(0, CommitParams);
	GeneratedMesh->CalculateInvRefMatrices();

	const FBoxSphereBounds Bounds(FBox(FVector(-100, -100, 0), FVector(100, 100, Params.TrunkHeight)));
	GeneratedMesh->SetImportedBounds(Bounds);
	GeneratedMesh->Build();

	TargetActor->SetGeneratedSkeletalMesh(GeneratedMesh, GeneratedSkeleton);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FPCGEditorProcessModule, PCGEditorProcess)

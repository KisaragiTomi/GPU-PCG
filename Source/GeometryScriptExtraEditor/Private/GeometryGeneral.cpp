// Fill out your copyright notice in the Description page of Project Settings.


#include "GeometryGeneral.h"

#include "DynamicMeshEditor.h"
#include "GeometryAttribute.h"
#include "MeshBoundaryLoops.h"
#include "UDynamicMesh.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMesh/MeshTransforms.h"
#include "GeometryScript/MeshGeodesicFunctions.h"
#include "GeometryScript/MeshNormalsFunctions.h"
#include "GeometryScript/MeshPrimitiveFunctions.h"
#include "GeometryScript/MeshSpatialFunctions.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetUtils/CreateStaticMeshUtil.h"
#include "Components/MeshComponent.h"
#include "Operations/SmoothDynamicMeshAttributes.h"
#include "EditorAssetLibrary.h"
#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "Parameterization/DynamicMeshUVEditor.h"
#include "Selections/MeshConnectedComponents.h"
#include "tbb/parallel_reduce.h"
#include "GroupTopology.h"
#include "MinVolumeBox3.h"
#include "OrientedBoxTypes.h"
#include "DynamicMesh/Operations/MergeCoincidentMeshEdges.h"
#include "Engine/TextureRenderTarget2D.h"
#include "openvdb/math/BBox.h"
#include "Spatial/FastWinding.h"
#include "Spatial/MeshAABBTree3.h"
#include "Spatial/PointHashGrid3.h"
#include "Util/IndexPriorityQueue.h"
#include "Engine/StaticMesh.h"
#include "StaticMeshResources.h"
#include "DistanceFieldAtlas.h"


using namespace UE::Geometry;

UDynamicMesh* UGeometryGeneral::BlurVertexNormals(UDynamicMesh* TargetMesh, int32 Iteration, bool RecomputeNormals)
{
	FGeometryScriptCalculateNormalsOptions CalculateOptions;
	if (RecomputeNormals)
	{
		UGeometryScriptLibrary_MeshNormalsFunctions::RecomputeNormals(TargetMesh, CalculateOptions);
	}
	
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
	
		if (Normals->ElementCount() > 0)
		{
			FSmoothDynamicMeshAttributes BlurOp(EditMesh);
			BlurOp.bUseParallel = true;
			BlurOp.NumIterations = Iteration;
			BlurOp.Strength = .5;
			BlurOp.EdgeWeightMethod = static_cast<FSmoothDynamicMeshAttributes::EEdgeWeights>(0);
		
			TArray<bool> NormalsToSmooth;
			NormalsToSmooth.Reserve(3);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
			NormalsToSmooth.Add(true);
		
			FGeometryScriptMeshSelection Selection;
			if (!Selection.IsEmpty())
			{
				Selection.ProcessByVertexID(EditMesh, [&](int32 VertexID)
				{
					BlurOp.Selection.Add(VertexID);
				});
			}
			
			BlurOp.SmoothOverlay(Normals, NormalsToSmooth);
		}
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	CreateVertexNormalFromOverlay(TargetMesh);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::CreateVertexNormalFromOverlay(UDynamicMesh* TargetMesh)
{
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (EditMesh.VertexCount() > 0)
		{
			if (EditMesh.HasVertexNormals() == false)
			{
				EditMesh.EnableVertexNormals(FVector3f::UpVector);
			}
			FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();
			int32 TriCount = EditMesh.TriangleCount();
			for (int32 TriIndex = 0; TriIndex < TriCount; ++TriIndex)
			{
				FIndex3i TVidNormal =  Normals->GetTriangle(TriIndex);
				FIndex3i TVid = EditMesh.GetTriangle(TriIndex);
				for (int32 i = 0; i < 3; ++i)
				{
					if (TVidNormal[i] < 0 || !EditMesh.IsVertex(TVid[i]))
						continue;
					FVector3f PreNormal = EditMesh.GetVertexNormal(TVid[i]);
					FVector3f Normal = Normals->GetElement(TVidNormal[i]);
					EditMesh.SetVertexNormal(TVid[i], Normal);
				}
			}
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::CreateVertexNormals(UDynamicMesh* TargetMesh)
{
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh) 
	{
		if (EditMesh.VertexCount() > 0)
		{
			if (EditMesh.HasVertexNormals() == false)
			{
				EditMesh.EnableVertexNormals(FVector3f::UpVector);
			}
			FMeshNormals Normals(&EditMesh);
			Normals.ComputeVertexNormals();
			Normals.CopyToVertexNormals(&EditMesh, false);
		}

	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return TargetMesh;
}



UDynamicMesh* UGeometryGeneral::FillLine(UDynamicMesh* TargetMesh, TArray<FVector> VertexLoop)
{
	bool Success = true;
	UDynamicMesh* BoundaryMeshCombineOut = NewObject<UDynamicMesh>();
	BoundaryMeshCombineOut->Reset();
	FDynamicMesh3 CopyMesh;
	BoundaryMeshCombineOut->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		int32 VertexCount = VertexLoop.Num();
		for (int32 i = 0; i < VertexCount; i++)
		{
			EditMesh.AppendVertex(VertexLoop[i]);
		}
		FVector3d c = FVector3d::Zero();
		for (int i = 0; i < VertexLoop.Num(); ++i)
		{
			c += VertexLoop[i];
		}
		c *= 1.0 / VertexLoop.Num();
		// add centroid vtx
		int32 NewVertex = EditMesh.AppendVertex(c);
		
		FDynamicMeshEditResult AddFanResult;
		int N = VertexLoop.Num();
		AddFanResult.NewTriangles.Reserve(N);

		int i = 0;
		for (i = 0; i < N; ++i)
		{
			int A = i;
			int B = (i + 1) % N;

			FIndex3i NewT(NewVertex, B, A);
			int NewTID = EditMesh.AppendTriangle(NewT, 0);
			if (NewTID < 0)
			{
				Success = false;
			}

			AddFanResult.NewTriangles.Add(NewTID);
		}
		CopyMesh = EditMesh;
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	if (Success)
	{
		TargetMesh->SetMesh(MoveTemp(CopyMesh));
		return TargetMesh;
	}
	return nullptr;
}

UDynamicMesh* UGeometryGeneral::PrimNormal(UDynamicMesh* TargetMesh, FVector TestPos, FVector& OutVector)
{
	FGeometryScriptDynamicMeshBVH BVH;
	UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(TargetMesh, BVH, nullptr);

	
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		FGeometryScriptSpatialQueryOptions Options;
		FGeometryScriptTrianglePoint NearestPoint;
		EGeometryScriptSearchOutcomePins Outcome;
		UGeometryScriptLibrary_MeshSpatial::FindNearestPointOnMesh(
		TargetMesh, BVH, TestPos, Options, NearestPoint, Outcome, nullptr);
		//VertexLocation = NearestPoint.Position;

		OutVector = GetNearestLocationNormal(EditMesh, NearestPoint);
		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	return TargetMesh;
}

void UGeometryGeneral::WindDataForTree(UDynamicMesh* TargetMesh,
                                       TArray<FLinearColor>& PivotIndexData,

                                       TArray<FLinearColor>& DirExtentData,
                                       int32& tXx,
                                       int32& tXy,
                                       TArray<FVector>& OutHolePositions,
                                       TMap<int, FVector>& DebugClassNum,
                                       int LeafMaterialIndex,
                                       float CombineDistThreshold,
                                       float FindParentThreshold, bool OutDebugColor
)
{

	// float CombineDistThreshold = 30;
	TArray<int> UVIndexTest;
	TArray<FVector2D> UVTestIndex;
	TArray<FVector2D> UVFloat;
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		FMeshBoundaryLoops BLoops(&EditMesh, true);
		EditMesh.EnableAttributes();
		if (EditMesh.Attributes()->NumUVLayers() < 2)
		{
			EditMesh.Attributes()->SetNumUVLayers(2);
		}
		
		
		VI_ATTR(Class, EditMesh, -1);
		VI_ATTR(PreClass, EditMesh, 0);
		VI_ATTR(DiscardPoint, EditMesh, 0);
		VI_ATTR(HolePoint, EditMesh, 0);
		TI_ATTR(Class, EditMesh, -1);



		FMeshConnectedComponents Components(&EditMesh);
		Components.FindConnectedTriangles();
		

		struct FWindTreeComponentData : public FDynamicMeshComponentData
		{
			FVector RootCenter = FVector(0, 0, 0);
			FVector Center = FVector::ZeroVector;
			FVector RootNormal = FVector::UpVector;
			FVector RootUp = FVector::ForwardVector;
			FLinearColor DebugColor = FLinearColor::Black;
			FString Hierarchy = "";
			FString ParentHierarchy = "";
			int AddToClass = -1;
			int UVIndex = 0;
			int StepCount = 0;

			float MaxDistToParent = TNumericLimits<int>::Max();;
			float OriginExtent = 0;

			bool Checked = false;
			bool CheckedTemp = false;
			bool IsLeaf = false;

			TArray<TArray<int>> Holes;
			int ValidHole = 0;
		};


		
		int MaxStepCount = 0;
		FGeometryScriptDynamicMeshBVH BVH;
		UGeometryScriptLibrary_MeshSpatial::BuildBVHForMesh(TargetMesh, BVH, nullptr);
		EditMesh.Attributes()->EnablePrimaryColors();

		TMap<int, FWindTreeComponentData> LeafComponentDatas;
		TMap<int, FWindTreeComponentData> ComponentDatas;
		for ( int c = 0; c < Components.Num(); c++)
		{

			
			FWindTreeComponentData ComponentData;
			ComponentData.TIDs = Components[c].Indices;
			ComponentData.Class = c;
			FDynamicMeshMaterialAttribute* ATI_MaterialID = EditMesh.Attributes()->GetMaterialID();
			int MaterialID = ATI_MaterialID->GetValue(ComponentData.TIDs[0]);
			bool IsLeaf = false;
			if (MaterialID == LeafMaterialIndex) IsLeaf = true;

			FVector3d CenterPos = FVector3d::Zero();
			for (int TID : ComponentData.TIDs)
			{
				FIndex3i TVIDs = EditMesh.GetTriangle(TID);
				for (int i = 0; i < 3; i++)
				{
					AVI_PreClass->SetValue(TVIDs[i], &c);
					CenterPos = CenterPos + EditMesh.GetVertex(TVIDs[i]);
				}
				ATI_Class->SetValue(TID, &c);

			}
			CenterPos /= ComponentData.TIDs.Num() * 3.0;
			ComponentData.Center = (FVector)CenterPos;
			
			if (IsLeaf)
			{
				ComponentData.IsLeaf = true;
				LeafComponentDatas.Add(c, ComponentData);
			}
			else
			{
				ComponentDatas.Add(c, ComponentData);
			}
			
		}

		


		for (TPair<int, FWindTreeComponentData>& ComponentData :  ComponentDatas)
		{
			DebugClassNum.Add(ComponentData.Key, ComponentData.Value.Center);
		}

		//End reduce components
		//=================================================================================


		// Rebuild TID Class
		//=================================================================================
		
		for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		{
			TSet<int> VertexsSet;
			int Class = ComponentData.Key;
			for (int TID : ComponentData.Value.TIDs)
			{
				FIndex3i TVIDs = EditMesh.GetTriangle(TID);
				ATI_Class->SetValue(TID, &Class);
				for (int i = 0 ; i < 3 ; i++)
				{
					AVI_Class->SetValue(TVIDs[i], &Class);
					VertexsSet.Add(TVIDs[i]);
				}
			}
			TArray<int> VertexsArray = VertexsSet.Array();
			TArray<FVector> VertexPoss;
			VertexPoss.Reserve(VertexsArray.Num());
			for (int i = 0 ; i < VertexsArray.Num(); i++)
			{
				FVector VertexPos = EditMesh.GetVertex(VertexsArray[i]);
				VertexPoss.Add(VertexPos);
			}
			ComponentData.Value.VPoss = VertexPoss;
		}
		//End Rebuild TID Class
		//=================================================================================

		

		// Find Root
		//=================================================================================
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
			if ( VertexPos.Z > 0) continue;
			
			int ClassNum = -1;
			ATI_Class->GetValue(TID, &ClassNum);
			if (ClassNum == -1) continue;
			
			ComponentDatas[ClassNum].Hierarchy = "Root";
			ComponentDatas[ClassNum].Checked = true;
		}
		//End Find Root
		//=================================================================================


		//CollectHoles
		//=================================================================================
		for (int i = 0; i < BLoops.GetLoopCount(); i++)
		{
			TArray<int> LoopVertices = BLoops.Loops[i].Vertices;
			int DiscardPoint = 0;
			int LoopClassNum = -1;
			int PreClassNum = -1;

			// AVI_PreClass->GetValue(BLoops.Loops[i].Vertices[0], &PreClassNum);
			AVI_Class->GetValue(BLoops.Loops[i].Vertices[0], &LoopClassNum);
			AVI_DiscardPoint->GetValue(BLoops.Loops[i].Vertices[0], &DiscardPoint);
			
			if (ComponentDatas.Find(LoopClassNum) == nullptr) continue;
			if (DiscardPoint) continue;

			for (int VID : LoopVertices)
			{
				if (!EditMesh.IsVertex(VID)) continue;
				int HolePoint = 1;
				AVI_HolePoint->SetValue(VID, &HolePoint);
			}
			ComponentDatas[LoopClassNum].Holes.Add(LoopVertices);
		}

		//EndCollectHoles
		//=================================================================================
		TArray<FVector> AllLoopVertices;
		for (TPair<int, FWindTreeComponentData>& ComponentData: ComponentDatas)
		{
			
			FWindTreeComponentData& Data = ComponentData.Value;
			int Class = ComponentData.Key;
			if (Data.Holes.Num() < 1) continue;
			
			float MaxSize = -9999999999.0;
			TArray<int> ValidHole = Data.Holes[0];
			TArray<FVector> ValidLoops;
			
			for (int i = 0; i < Data.Holes.Num(); i++)
			{
				TArray<int> Hole = Data.Holes[i];
				TArray<FVector> VertexPoss;
				VertexPoss.Reserve(Hole.Num());
				for (int VID : Data.Holes[i])
				{
					FVector VertexPos = EditMesh.GetVertex(VID);
					VertexPoss.Add(VertexPos);
				}

				FBox Box = FBox(VertexPoss);
				float Size = Box.GetSize().Length();
				
				if (Size > MaxSize)
				{
					ValidLoops = VertexPoss;
					ValidHole = Data.Holes[i];
					MaxSize = fmax(Size, MaxSize);
				}
			}
			if (ValidHole.Num() == 0)
			{
				FIndex3i TVIDs = EditMesh.GetTriangle(Data.TIDs[0]);
				ValidHole.Add(TVIDs[0]);
				ValidLoops.Add(FVector(0, 0, 0));
			}
			FVector Normal = FVector::ZeroVector;
			FVector Center = FVector::ZeroVector;
			
			// TArray<int> LoopVertices = BLoops.Loops[i].Vertices;
			TArray<FVector> LoopVertexPos;
			int DiscardPoint = -1;
			
			// AVI_PreClass->GetValue(LoopVertices.Vertices[0], &PreClassNum);
			// AVI_Class->GetValue(BLoops.Loops[i].Vertices[0], &LoopClassNum);
			AVI_DiscardPoint->GetValue(ValidHole[0], &DiscardPoint);
			
			if (DiscardPoint) continue;
			bool LoopChecked = Data.Checked;
			
			for (int j = 0; j < ValidHole.Num(); j++)
			{
				FVector PrePos = ValidLoops[(j == 0) ? (ValidHole.Num()-1) : (j-1) ];
				FVector Pos = ValidLoops[j];
				FVector NextPos =  ValidLoops[(j + 1) % ValidHole.Num() ];
				
				Center += Pos;
				
				FVector Dir1 = (PrePos - Pos).GetSafeNormal();
				FVector Dir2 = (NextPos - Pos).GetSafeNormal();

				Normal += FVector::CrossProduct(Dir1, Dir2);
			}
			FVector3d ResultN = Normal.GetSafeNormal();
			Center /= float(ValidHole.Num());

			// Find ParentClass
			//=================================================================================
			int ParentClass = TNumericLimits<int>::Max();
			bool TarChecked = false;
			for (int j = 0; j < ValidHole.Num(); j++)
			{
				FVector3d VertexPos = ValidLoops[j];
				
				int HitTID = TNumericLimits<int>::Max();
				IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
				{
					if (TID == TNumericLimits<int>::Max()) return false;
					int FindClass = -1;
					ATI_Class->GetValue(TID, &FindClass);
					if (!ComponentDatas.Find(FindClass)) return false;
					return FindClass != Class;
				});
				double Dist;
				HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
				if (HitTID < 0) continue;
				
				if (Dist > FindParentThreshold ) continue;
				
				int ParentClassNum = -1;
				ATI_Class->GetValue(HitTID, &ParentClassNum);
				
				bool Check = ComponentDatas[ParentClassNum].Checked;
				ParentClass = fmin(ParentClass, ParentClassNum);
				TarChecked = TarChecked || Check;
				
			}
			AllLoopVertices.Append(ValidLoops);
			
			// End Parent Class
			//=================================================================================
			
			if (LoopChecked == 1 ) continue;
			
			Data.RootCenter = Center;
			Data.RootNormal = ResultN;
			Data.ParentClass = ParentClass;

			if  (ParentClass != TNumericLimits<int>::Max()) continue;
			
			Data.RootCenter = FVector::ZeroVector;
			Data.RootNormal = FVector::UpVector;
			Data.ParentClass = -1;
			Data.Hierarchy = "Root";
			Data.Checked = 1;
			
		}
		
		
		//Build hierarchy
		for (int i = 0 ; i < 10; i++)
		{
			for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
			{
				int Class = ComponentData.Key;
				FWindTreeComponentData& Data = ComponentData.Value;
				int ParentClass = Data.ParentClass;
				bool Checked = ComponentData.Value.Checked;

				if (Checked || ParentClass < 0) continue;
				if (!ComponentDatas[ParentClass].Checked) continue;
				
				if (i < 3)
				{
					FString& Hierarchy = ComponentData.Value.Hierarchy;
					FString TartHierarchy = ComponentDatas[ParentClass].Hierarchy;
					Hierarchy += TartHierarchy + "/" + FString::FromInt(Class);
					ComponentData.Value.StepCount = ComponentDatas[ParentClass].StepCount + 1;
					MaxStepCount = FMath::Max(ComponentData.Value.StepCount, MaxStepCount);
				}
				else
				{
					Data.Hierarchy = ComponentDatas[ParentClass].Hierarchy;
					Data.RootCenter = ComponentDatas[ParentClass].RootCenter;
					Data.RootNormal = ComponentDatas[ParentClass].RootNormal;
					Data.ParentClass = ComponentDatas[ParentClass].ParentClass;
					Data.StepCount = ComponentDatas[ParentClass].StepCount;
					Data.DebugColor = FLinearColor::Red;
				}
				Data.CheckedTemp = true;
			}
			for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
			{
				ComponentData.Value.Checked = ComponentData.Value.Checked || ComponentData.Value.CheckedTemp;
			}
		}

		//End build hierarchy
		//=================================================================================

		//CombineLeafComponent
		//=================================================================================
		if (LeafMaterialIndex >= 0)
		{
			TMap<int, FWindTreeCombineLeafData> ReduceComponentMap;
				
			for (TPair<int, FWindTreeComponentData> LeafComponentData :  LeafComponentDatas)
			{
				int ParentClass = TNumericLimits<int>::Max();      
				int Class = LeafComponentData.Key;
				FWindTreeComponentData& Data = LeafComponentData.Value;
				TMap<int, FWindTreeCombineLeafData> TempComponentMap;
				TDynamicMeshTriangleAttribute<int, 1>* AttribClass = static_cast<TDynamicMeshTriangleAttribute<int, 1>*>(EditMesh.Attributes()->GetAttachedAttribute(FName("TI_Class")));
				IMeshSpatial::FQueryOptions QueryOptionsXYZ([&](int32 TID)
				{
					int FindClass = -1;
					AttribClass->GetValue(TID, &FindClass);
					if (!ComponentDatas.Find(FindClass)) return false;
					
					return true;
				});
				bool Find = false;
				float MinDist = 1e10;
				for (int TID : Data.TIDs)
				{
					FIndex3i TVIDs = EditMesh.GetTriangle(TID);
					FVector3d VertexPos = EditMesh.GetVertex(TVIDs[0]);
					int HitTID = TNumericLimits<int>::Max();

					double Dist;
					HitTID = BVH.Spatial->FindNearestTriangle(VertexPos, Dist, QueryOptionsXYZ);
					
					if (HitTID < 0) continue;
					if (Dist > MinDist) continue;

					MinDist = Dist;
					int ParentClassNum = -1;
					AttribClass->GetValue(HitTID, &ParentClassNum);
					float ParentClassDist = 0;
					FWindTreeCombineLeafData ComponentReduceData;
					
					if (TempComponentMap.Find(ParentClassNum) != nullptr) ComponentReduceData = *TempComponentMap.Find(ParentClassNum);

					bool LeafCheck = ComponentReduceData.CollectedData(EditMesh, TID, Dist);
					
					TempComponentMap.Add(ParentClassNum, ComponentReduceData);
					Find = true;
				}
				//Find Main Nearest Component
				//=================================================================================
				if (!Find) continue;
				TPair<int, FWindTreeCombineLeafData> MinDistComponentReduceData;
				for (TPair<int, FWindTreeCombineLeafData> ComponentReduceData: TempComponentMap)
				{
					if ( MinDistComponentReduceData.Value.MinDist >  ComponentReduceData.Value.MinDist)
					{
						MinDistComponentReduceData = ComponentReduceData;
						MinDistComponentReduceData.Value.ParentClass = ComponentReduceData.Key;
					}
				}

				ReduceComponentMap.Add(Class, MinDistComponentReduceData.Value);
				
			}
			for (TPair<int, FWindTreeCombineLeafData> LeafComponent : ReduceComponentMap)
			{
				int Class = LeafComponent.Key;
				FWindTreeCombineLeafData Data = LeafComponent.Value;
				int ParentClass = Data.ParentClass;

				ComponentDatas[ParentClass].TIDs.Append(LeafComponentDatas[Class].TIDs);
			}
		}

		TMap<int, FString> HierarchyTest;
		for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
		{
			HierarchyTest.Add(ComponentData.Key, ComponentData.Value.Hierarchy);
		}
		
		TMap<FString, FWindTreeComponentData> HierarchyCompMap;
		int CompDataIndex = 0;
		for (TPair<int, FWindTreeComponentData>& ComponentData : ComponentDatas)
		{
			int Class = ComponentData.Key;
			FWindTreeComponentData& Data = ComponentData.Value;
			FString ParentHierarchy = "";
			if (Data.ParentClass >= 0) ParentHierarchy = ComponentDatas[Data.ParentClass].Hierarchy;
		
			if (Data.Hierarchy.Len() == 0)
			{
				Data.Hierarchy = "Root";
				Data.ParentClass = -1;
				Data.RootCenter = FVector::ZeroVector;
				Data.RootNormal = FVector(0, 0, 1);
			}
			if (HierarchyCompMap.Find(Data.Hierarchy) == nullptr)
			{
				Data.ParentHierarchy = ParentHierarchy;
				Data.UVIndex = CompDataIndex;
				HierarchyCompMap.Add(Data.Hierarchy, Data);
				CompDataIndex += 1;
			}
			else
			{
				FWindTreeComponentData& RootComponentData = *HierarchyCompMap.Find(Data.Hierarchy);
				RootComponentData.TIDs.Append(Data.TIDs);
				RootComponentData.VPoss.Append(Data.VPoss);
			}
		}
		
		for (TPair<FString, FWindTreeComponentData>& ComponentData : HierarchyCompMap)
		{
			FVector OBBUpDir = FVector::ZeroVector;
			UGeometryGeneral::CalculateOBBUpDir(ComponentData.Value.VPoss, OBBUpDir);
			
			FWindTreeComponentData& Data = ComponentData.Value;
			FVector ObjectBasisVectorX = -OBBUpDir.GetSafeNormal();
			FVector ObjectBasisVectorY = Data.RootNormal.GetSafeNormal();
			FVector ObjectBasisVectorZ = FVector::CrossProduct(ObjectBasisVectorX, -ObjectBasisVectorY).GetSafeNormal();
			FQuat yDhiedral = FQuat::FindBetweenNormals(ObjectBasisVectorY, FVector(0, 1, 0));
			FQuat zDhiedral = FQuat::FindBetweenNormals(ObjectBasisVectorZ, FVector(0, 0, 1));


			for (int v = 0; v < Data.VPoss.Num(); v++)
			{
				FVector VertexPos = Data.VPoss[v];
	
				VertexPos -= Data.RootCenter;
				VertexPos = yDhiedral.RotateVector(VertexPos);
				VertexPos = zDhiedral.RotateVector(VertexPos);
				Data.VPoss[v] = VertexPos;
			}
			FBox BBox = FBox(Data.VPoss);
			FVector BBoxSize = BBox.GetSize();
			FVector OriginExtent = BBoxSize.GetAbs();
			Data.OriginExtent = FMath::Clamp(OriginExtent.X / 2048.0, 1 / 2048.0, 1);
			if (Data.ParentHierarchy.Len() == 0) Data.ParentHierarchy = Data.Hierarchy;
		}
		

		//Calculate TextureXY
		int32 objectToProcessCount, evenNumber, halfEvenNumber, halfNumber, decrementerTotal, modResult;
		int32 rowCounter, newDecrementerTotal, decrementAmount, complete, maxAttempts, attempt;
		
		
		objectToProcessCount = HierarchyCompMap.Num();
		
		decrementerTotal = 256;
		modResult = 1;
		rowCounter = 1;
		evenNumber = (objectToProcessCount % 2)==0;
		halfEvenNumber = (int32(objectToProcessCount/2.0) % 2)==0;
		halfNumber = ceil(objectToProcessCount/2.0);

		if (halfNumber < decrementerTotal)
		{
			newDecrementerTotal = halfNumber;
		}
		else
		{
			newDecrementerTotal = decrementerTotal;
		}

		if (halfEvenNumber==1)
		{
			decrementAmount = 2;
		}
		else
		{
			decrementAmount = 1;
		}
		
		complete = 0;
		attempt = 0;
		maxAttempts = 100;

		while ( complete == 0 || attempt < maxAttempts )
		{
			attempt += 1;
			modResult = objectToProcessCount % newDecrementerTotal;
			if ( modResult == 0 || newDecrementerTotal < 1 )
			{
				complete = 1;
			}
			if ( complete == 0 )
			{
				newDecrementerTotal -= decrementAmount;
			}
			if (newDecrementerTotal < 1)
			{
				newDecrementerTotal = 1;
			}
		}

		// Get Texture Size
		if ( newDecrementerTotal==1 || ( objectToProcessCount / newDecrementerTotal ) > decrementerTotal )
		{
			tXy = int(floor(sqrt(objectToProcessCount)));
			tXx = int(ceil(float(objectToProcessCount)/(floor(tXy))));
		}
		else
		{
			tXx = newDecrementerTotal;
			tXy = objectToProcessCount / newDecrementerTotal;
		}
		FDynamicMeshUVOverlay* UVOverlay2 = EditMesh.Attributes()->GetUVLayer(1);
		if (UVOverlay2 == nullptr) return;


		TMap<FString, FWindTreeComponentData> HierarchyPurMap;
		
		int MapIndex = 0;
		PivotIndexData.SetNumZeroed(HierarchyCompMap.Num());
		DirExtentData.SetNumZeroed(HierarchyCompMap.Num());

		
		float TextureMax = fmax(tXx, tXy);

		UVIndexTest.Reserve(HierarchyCompMap.Num());
		UVTestIndex.Reserve(HierarchyCompMap.Num());
		UVFloat.Reserve(HierarchyCompMap.Num());
		for (TPair<FString, FWindTreeComponentData>& Component : HierarchyCompMap)
		{

			FString HierarchyN = Component.Key;
			FWindTreeComponentData Data = Component.Value;
			UVIndexTest.Add(Data.UVIndex);
			int UVIndex = Data.UVIndex;
			float PosX = rint(fmodf(UVIndex, TextureMax));
			float PosY = floor(UVIndex / TextureMax);
			FVector2f UV2 = FVector2f(PosX / tXx + .5 / tXx, PosY / tXy + 0.5 / tXy);
			for (int TID : Data.TIDs)
			{
				FIndex3i TVIDs = UVOverlay2->GetTriangle(TID);
				for (int v = 0; v < 3; v++)
				{
					if (!UVOverlay2->IsElement(TVIDs[v])) continue;

					UVOverlay2->SetElement(TVIDs[v], UV2);
				}
			}
			UVTestIndex.Add(FVector2D(UV2.X * 103, UV2.Y * 9));
			UVFloat.Add(FVector2D(UV2.X, UV2.Y));

			int ParentIndex = HierarchyCompMap[Data.ParentHierarchy].UVIndex;
			
			int16 ParentIndex16 = static_cast<int16>(ParentIndex); 
			ParentIndex16 += 1024;
			FFloat16 Parent16Value = *reinterpret_cast<FFloat16*>(&ParentIndex16);

			FVector Center = Data.RootCenter;
			FVector Normal = Data.RootNormal;
			Normal = Normal / 2 + FVector::OneVector * .5;
			PivotIndexData[UVIndex] = (FLinearColor(Center.X, Center.Y, Center.Z, Parent16Value));
			DirExtentData[UVIndex] = (FLinearColor(Normal.X, Normal.Y, Normal.Z, fmax(Data.OriginExtent, 0.01)));
			
			//Debug Color

			if (! OutDebugColor) continue;
			
			FLinearColor TestColor = FLinearColor::MakeRandomColor();
			FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();
						
			for (int TID : Data.TIDs)
			{
				FIndex3i TVID = ColorOverlay->GetTriangle(TID);
				for (int v = 0; v < 3; v++)
				{
					float ColorB = 0;
					if (Data.Hierarchy == "Root")
					{
						ColorB = 1;
					}
					ColorOverlay->SetElement(TVID[v], FLinearColor(Data.StepCount / 5.0, Data.Class / 255.0, ColorB, 0));
				}
			}
			//End Debug Color
		}
	});
	
}

UDynamicMesh* UGeometryGeneral::WeldDynamicMesh(UDynamicMesh* TargetMesh, float Tolerance)
{
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		WeldVertices(EditMesh, Tolerance);
	});
	return TargetMesh;
}

void UGeometryGeneral::WeldVertices(FDynamicMesh3& EditMesh, float Tolerance)
{
	// FMergeCoincidentMeshEdges Welder(&EditMesh);
	// Welder.MergeVertexTolerance = Tolerance * 0.001;
	// Welder.MergeSearchTolerance = 2 * Welder.MergeVertexTolerance;
	// Welder.OnlyUniquePairs = false;
	// Welder.Apply();


	FSplitAttributeWelder  SplitAttributeWelder;

	constexpr float DegToRad = static_cast<float>(UE_PI / 180.f);

	SplitAttributeWelder.UVDistSqrdThreshold = 0.01f * 0.01f;;
	SplitAttributeWelder.NormalVecDotThreshold = FMath::Abs(1.f - FMath::Cos( DegToRad * 0.1f));
	SplitAttributeWelder.TangentVecDotThreshold = FMath::Abs(1.f - FMath::Cos( DegToRad * 0.1f));
	SplitAttributeWelder.ColorDistSqrdThreshold = 0.01f * 0.01f;

	FMeshBoundaryLoops BLoops(&EditMesh, true);
	TSet<int32> EdgesToMerge; 
	for (int i = 0; i < BLoops.GetLoopCount(); i++)
	{
		TArray<int> LoopEdges = BLoops.Loops[i].Edges;
		EdgesToMerge.Append(LoopEdges);
	}

	float MergeSearchTolerance =  2 * Tolerance;
	double ToleranceSqr = Tolerance * Tolerance;
	double UseMergeSearchTol = (MergeSearchTolerance > 0) ? MergeSearchTolerance : 2 * Tolerance;
	
	//
	// construct hash table for edge midpoints
	//



	
	TArray<FVector3d> BoundaryMidPoints;
	TArray<int32> ToMidPt;
	ToMidPt.Init(-1, EditMesh.MaxEdgeID());
	for (int32 EID : EditMesh.BoundaryEdgeIndicesItr())
	{
		ToMidPt[EID] = BoundaryMidPoints.Add(EditMesh.GetEdgePoint(EID, 0.5));
	}
	int32 InitialNumBoundaryEdges = BoundaryMidPoints.Num();
	int hashN = 64;
	if (InitialNumBoundaryEdges > 1000)   hashN = 128;
	if (InitialNumBoundaryEdges > 10000)  hashN = 256;
	if (InitialNumBoundaryEdges > 100000)  hashN = 512;
	FAxisAlignedBox3d Bounds = EditMesh.GetBounds(true);
	double CellSize = FMath::Max(FMathd::ZeroTolerance, Bounds.MaxDim() / (double)hashN);
	
	double DensityBasedCellSize = FMath::Max(FMathd::ZeroTolerance, Bounds.MaxDim() / (double)hashN);
	double FinalCellSize = FMath::Max(DensityBasedCellSize, UseMergeSearchTol * 2.0);
	
	TPointHashGrid3<int32, double> MidpointsHash(FinalCellSize, -1);

	UseMergeSearchTol = fmin(UseMergeSearchTol, Bounds.MaxDim());
	// temp values and buffers
	FVector3d A, B, C, D;
	TArray<int> equivBuffer;
	TArray<int> SearchMatches;
	SearchMatches.Reserve(1024);  // allocate buffer
	
	//
	// construct edge equivalence sets. First we find all other edges with same
	// midpoint, and then we form equivalence set for edge from subset that also
	// has same endpoints
	//
	
	typedef TArray<int> EdgesList;
	TArray<EdgesList*> EquivalenceSets;
	EquivalenceSets.Init(nullptr, EditMesh.MaxEdgeID());
	TSet<int> RemainingEdges;
	
	for (int eid : EditMesh.BoundaryEdgeIndicesItr()) 
	{
		const int32 MidPtIdx = ToMidPt[eid];
 		FVector3d midpt = BoundaryMidPoints[MidPtIdx];
	
		// find all other edges with same midpoint in query sphere
		SearchMatches.Reset();
		MidpointsHash.FindPointsInBall(midpt, UseMergeSearchTol, [&](const int32& PtIdx)
			{
				return FVector3d::DistSquared(midpt, BoundaryMidPoints[ToMidPt[PtIdx]]);
			}, SearchMatches);
		// add each point after querying for neighbors, so we only find edges with earlier IDs
		MidpointsHash.InsertPointUnsafe(eid, midpt);
	
		int N = SearchMatches.Num();
		if (N == 0)
		{
			continue;		// edge has no matches
		}
	
		EditMesh.GetEdgeV(eid, A, B);
	
		// if same endpoints, add to equivalence set for this edge (and matching reverse equivalence)
		equivBuffer.Reset();
		for (int i = 0; i < N; ++i) 
		{
			int32 MatchEID = SearchMatches[i];
			EditMesh.GetEdgeV(SearchMatches[i], C, D);
			if ( (DistanceSquared(A,C) < ToleranceSqr && DistanceSquared(B,D) < ToleranceSqr) ||
			(DistanceSquared(A,D) < ToleranceSqr && DistanceSquared(B,C) < ToleranceSqr)) 
			{
				equivBuffer.Add(SearchMatches[i]);
				if (!EquivalenceSets[MatchEID])
				{
					EquivalenceSets[MatchEID] = new EdgesList();
					RemainingEdges.Add(MatchEID);
				}
				EquivalenceSets[MatchEID]->Add(eid);
			}
		}
		if (equivBuffer.Num() > 0)
		{
			EquivalenceSets[eid] = new EdgesList(equivBuffer);
			RemainingEdges.Add(eid);
		}
	}
	
	
	//
	// add potential duplicate edges to priority queue, sorted by number of possible matches. 
	//
	
	// [TODO] could replace remaining hashset w/ PQ, and use conservative count?
	// [TODO] Does this need to be a PQ? Not updating PQ below anyway...
	FIndexPriorityQueue DuplicatesQueue;
	DuplicatesQueue.Initialize(EditMesh.MaxEdgeID());
	for (int eid : RemainingEdges) 
	{
		const float Priority = (float)EquivalenceSets[eid]->Num();
		DuplicatesQueue.Insert(eid, Priority);
	}
	
	//
	// process all potential matches, merging edges as we go in a greedy fashion.
	
	while (DuplicatesQueue.GetCount() > 0) 
	{
		int eid = DuplicatesQueue.Dequeue();
		
		if (EditMesh.IsEdge(eid) == false || EquivalenceSets[eid] == nullptr || RemainingEdges.Contains(eid) == false)
		{
			continue;               // dealt with this edge already
		}
		if (EditMesh.IsBoundaryEdge(eid) == false)
		{
			continue;				// this edge got merged already
		}
	
		EdgesList& Matches = *EquivalenceSets[eid];
		//
		 // select best viable match (currently just "first"...)
		 // @todo could we make better decisions here? prefer planarity?
		 bool bMerged = false;
		 int FailedCount = 0;
		 for (int i = 0; i < Matches.Num() && bMerged == false; ++i) 
		 {
		 	int other_eid = Matches[i];
		 	if (EditMesh.IsEdge(other_eid) == false || EditMesh.IsBoundaryEdge(other_eid) == false)
		 	{
		 		continue;
		 	}
		
		 	// When there is no geometry selection, EdgesToMerge is never initialized
		 	bool bWeldingAcrossEntireMesh = (EdgesToMerge.Num() > 0);
		
		 	// Edges only considered for merging if we're welding the entire mesh or, when using a geometry selection,
		 	// if EITHER edge in the Match are a part of the selection
		 	if (EdgesToMerge.Contains(eid) || EdgesToMerge.Contains(other_eid))
		 	{
		 		FDynamicMesh3::FMergeEdgesInfo MergeInfo;
		 		EMeshResult Result = EditMesh.MergeEdges(eid, other_eid, MergeInfo);
		 		if (Result != EMeshResult::Ok) 
		 		{
		 			// if the operation failed we remove this edge from the equivalence set
		 			Matches.RemoveAt(i);
		 			i--;
		
		 			EquivalenceSets[other_eid]->Remove(eid);
		 			//DuplicatesQueue.UpdatePriority(...);  // should we do this?
		
		 			FailedCount++;
		 		}
		 		else
		 		{
		 			// ok we merged, other edge is no longer free
		 			bMerged = true;
		 			delete EquivalenceSets[other_eid];
		 			EquivalenceSets[other_eid] = nullptr;
		 			RemainingEdges.Remove(other_eid);
		
		 			// weld attributes 

		 			SplitAttributeWelder.WeldSplitElements(EditMesh, MergeInfo.KeptVerts[0]);
		 			SplitAttributeWelder.WeldSplitElements(EditMesh, MergeInfo.KeptVerts[1]);

		 			FIndex2i EVID = EditMesh.GetEdgeV(eid);
		 			FIndex2i EVIDother = EditMesh.GetEdgeV(other_eid);
		 			FVector Pos00 = EditMesh.GetVertex(EVID[0]);
		 			FVector Pos01 = EditMesh.GetVertex(EVID[1]);
		 			FVector Pos10 = EditMesh.GetVertex(EVIDother[0]);
		 			FVector Pos11 = EditMesh.GetVertex(EVIDother[1]);
		 			
		 			
		 		}
		 	}
		 }
	
		 // Removing branch with two identical cases to fix static analysis warning.
		 // However, these two branches are *not* the same...we're just not sure 
		 // what the right thing to do is in the else case
		if (bMerged) 
		{
			 delete EquivalenceSets[eid];
			 EquivalenceSets[eid] = nullptr;
			 RemainingEdges.Remove(eid);
		}
		else
		{
			// should we do something else here? doesn't make sense to put
			// back into Q, as it should be at the top, right?
			delete EquivalenceSets[eid];
			EquivalenceSets[eid] = nullptr;
			RemainingEdges.Remove(eid);
		}
	
	}
	



}

UDynamicMesh* UGeometryGeneral::ColorAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success, FLinearColor ChannelMask)
{
	Success = true;
	ChannelMask.R = rintf(ChannelMask.R);
	ChannelMask.G = rintf(ChannelMask.G);
	ChannelMask.B = rintf(ChannelMask.B);
	ChannelMask.A = rintf(ChannelMask.A);
	
	TSharedPtr<FDynamicMesh3> SourceMeshCopy = MakeShared<FDynamicMesh3>();
	SourceMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh) { SourceMeshCopy->Copy(EditMesh); });
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		FDynamicMeshColorOverlay* TargetColorOverlay = EditMesh.Attributes()->PrimaryColors();
		FDynamicMeshColorOverlay* SourceColorOverlay = SourceMeshCopy->Attributes()->PrimaryColors();
		if (TargetColorOverlay->ElementCount() != SourceColorOverlay->ElementCount())
		{
			Success = false;
			return ;
		}

		for(int EID : TargetColorOverlay->ElementIndicesItr())
		{
			FVector4f SourceColor4f = FVector4f::Zero();
			SourceColorOverlay->GetElement(EID, SourceColor4f);
			FVector4f TargetColor4f = FVector4f::Zero();
			TargetColorOverlay->GetElement(EID, TargetColor4f);
			
			TargetColor4f.X = FMath::Lerp(TargetColor4f.X, SourceColor4f.X, (float)ChannelMask.R) ;
			TargetColor4f.Y = FMath::Lerp(TargetColor4f.Y, SourceColor4f.Y, (float)ChannelMask.G) ;
			TargetColor4f.Z = FMath::Lerp(TargetColor4f.Z, SourceColor4f.Z, (float)ChannelMask.B) ;
			TargetColor4f.W = FMath::Lerp(TargetColor4f.W, SourceColor4f.W, (float)ChannelMask.A) ;
			TargetColorOverlay->SetElement(EID, TargetColor4f);
		}
	});
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::UVAttribTransf(UDynamicMesh* SourceMesh, UDynamicMesh* TargetMesh, bool& Success,
                                               int UVNum, FLinearColor ChannelMask)
{
	Success = true;
	ChannelMask.R = rintf(ChannelMask.R);
	ChannelMask.G = rintf(ChannelMask.G);
	
	TSharedPtr<FDynamicMesh3> SourceMeshCopy = MakeShared<FDynamicMesh3>();
	SourceMesh->ProcessMesh([&](const FDynamicMesh3& EditMesh) { SourceMeshCopy->Copy(EditMesh); });
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		if ( SourceMeshCopy->Attributes()->NumUVLayers() < UVNum )
		{
			Success = false;
			return ;
		}
		FDynamicMeshUVOverlay* SourceUVOverlay = SourceMeshCopy->Attributes()->GetUVLayer(UVNum);
		FDynamicMeshUVOverlay* TargetUVOverlay = nullptr;
		if (EditMesh.Attributes()->NumUVLayers() < UVNum)
		{
			EditMesh.Attributes()->SetNumUVLayers(UVNum + 1);
			TargetUVOverlay = EditMesh.Attributes()->GetUVLayer(UVNum);
			TargetUVOverlay->InitializeTriangles(EditMesh.MaxTriangleID());
			for (int i = 0; i < SourceUVOverlay->ElementCount(); i++)
			{
				FVector2f SourceUV2f = FVector2f::Zero();
				SourceUVOverlay->GetElement(i, SourceUV2f);
				TargetUVOverlay->AppendElement(SourceUV2f);
			}
			
		}
		else
		{
			TargetUVOverlay = EditMesh.Attributes()->GetUVLayer(UVNum);
			for(int EID : SourceUVOverlay->ElementIndicesItr())
			{
				FVector2f SourceUV2f = FVector2f::Zero();
				SourceUVOverlay->GetElement(EID, SourceUV2f);
				FVector2f TargetUV2f = FVector2f::Zero();
				TargetUVOverlay->GetElement(EID, TargetUV2f);
				//
				// TargetUV2f.X = FMath::Lerp(TargetUV2f.X, SourceUV2f.X, (float)ChannelMask.R) ;
				// TargetUV2f.Y = FMath::Lerp(TargetUV2f.Y, SourceUV2f.Y, (float)ChannelMask.G) ;
				TargetUVOverlay->SetElement(EID, SourceUV2f);
			}
		}

		

		// if (TargetUVOverlay->ElementCount() != SourceUVOverlay->ElementCount())
		// {
		// 	Success = false;
		// 	return ;
		// }
	

	});
	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::FillUVData(UDynamicMesh* TargetMesh, int32 UVLayerNum)
{
	FName TestAttribute = "TestAttribute";
	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		if (EditMesh.HasAttributes() == false)
		{
			EditMesh.EnableAttributes();
		}
		if (UVLayerNum != EditMesh.Attributes()->NumUVLayers())
		{
			EditMesh.Attributes()->SetNumUVLayers(UVLayerNum);
		}
		bool a = EditMesh.HasAttributes();
		int32 b =  EditMesh.Attributes()->NumUVLayers();
		FDynamicMeshUVOverlay* UVLayer = EditMesh.Attributes()->GetUVLayer(2);
		FDynamicMeshColorOverlay* ColorOverlay = EditMesh.Attributes()->PrimaryColors();
		UVLayer->InitializeTriangles(EditMesh.MaxTriangleID());
		// UVLayer.
		FDynamicMeshUVOverlay* UVLayer0 = EditMesh.Attributes()->GetUVLayer(0);
		UVLayer->ElementIndicesItr();

		for (int ElementID : UVLayer0->ElementIndicesItr())
		{
			FVector2f Test = UVLayer0->GetElement(ElementID);
		}
		FDynamicMeshUVEditor UVEditor(&EditMesh, UVLayer);
		UVEditor.CopyUVLayer(UVLayer0);
		for (int ElementID : UVLayer->ElementIndicesItr())
		{
			UVLayer->SetElement(ElementID, FVector2f(0, 0));
			FVector2f Test = UVLayer->GetElement(ElementID);
		}

		for (int32 TriangleID : EditMesh.TriangleIndicesItr())
		{
			if (UVLayer0->IsSetTriangle(TriangleID))
			{

				FIndex3i TriangleVertex = EditMesh.GetTriangle(TriangleID);


				FIndex3i TriangleUV = UVLayer->GetTriangle(TriangleID);
				float VertexNum = UVLayer->ElementCount();
				for (int32 i = 0; i < 3; i++)
				{
					FGeometryScriptIndexList VertexIDList;
					bool Errors = 0;
					UGeometryScriptLibrary_MeshGeodesicFunctions::GetShortestVertexPath(TargetMesh,TriangleVertex[i],int32(EditMesh.VertexCount() / 2),VertexIDList,Errors,nullptr);

					if (VertexIDList.List.IsValid())
					{
						TArray<FVector> PathPositions;
						PathPositions.Reset(VertexIDList.List->Num());

					
						for (int32 Idx : *VertexIDList.List)
						{
							PathPositions.Add((FVector)EditMesh.GetVertex(TriangleVertex[i]));
						}
						float PathLength = 0;
						for (int32 k = 0; k < PathPositions.Num() - 1; k++)
						{
							PathLength += FVector::Distance(PathPositions[k], PathPositions[k + 1]);
						}
					}



					FVector3d VertexPos = EditMesh.GetVertex(TriangleVertex[i]);
					FVector3d PreVertex = VertexPos;
					VertexPos *= .02;
					FVector2f UVData = FVector2f( FMath::Abs(VertexPos.X) / VertexNum, FMath::Abs(VertexPos.Y) / VertexNum);
					UVLayer->SetElement(TriangleUV[i], UVData);
					
				}
				
				FIndex3i TriangleColor = ColorOverlay->GetTriangle(TriangleID);
				for (int32 i = 0; i < 3; i++)
				{
					
					FVector3d VertexPos = EditMesh.GetVertex(TriangleVertex[i]);
					FVector3d PreVertex = VertexPos;
					ColorOverlay->SetElement(TriangleUV[i], FVector4f(VertexPos.X, VertexPos.Y, VertexPos.Z, 0));
					
				}
			}
		}
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);

	return TargetMesh;
}

UDynamicMesh* UGeometryGeneral::AddCustomAttribute(UDynamicMesh* TargetMesh, FName AttributeName)
{

	TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
	{
		EditMesh.EnableAttributes();
		VI_ATTR(Class, EditMesh, 0);
		TI_ATTR(Class, EditMesh, 0);

		for (int32 VID : EditMesh.VertexIndicesItr())
		{
			int TestData = 1;
			AVI_Class->SetValue(VID, &TestData);
		}
		
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			int TestData = 1;
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			for (int32 i = 0; i < 3; i++)
			{
				ATI_Class->SetValue(TVIDs[i], &TestData);
			}
		}
		
		for (int32 TID : EditMesh.TriangleIndicesItr())
		{
			int TestData = 0;
			FIndex3i TVIDs = EditMesh.GetTriangle(TID);
			for (int32 i = 0; i < 3; i++)
			{
				ATI_Class->GetValue(TVIDs[i], &TestData);
				TestData = 0;
			}
		}

		for (int32 VID : EditMesh.VertexIndicesItr())
		{
			int TestData = 0;
			AVI_Class->GetValue(VID, &TestData);
			TestData = 0;
		}

		
	}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	
	return nullptr;
}

FVector UGeometryGeneral::GetNearestLocationNormal(FDynamicMesh3& EditMesh, FGeometryScriptTrianglePoint NearestPoint)
{
	if (EditMesh.HasVertexNormals())
	{
		FIndex3i TriVertexIDs = EditMesh.GetTriangle(NearestPoint.TriangleID);
		FVector N = FVector(NearestPoint.BaryCoords[0] * EditMesh.GetVertexNormal(TriVertexIDs[0]) + NearestPoint.BaryCoords[1] * EditMesh.GetVertexNormal(TriVertexIDs[1]) + NearestPoint.BaryCoords[2] * EditMesh.GetVertexNormal(TriVertexIDs[2]));
		N.Normalize();
		return N;
	}
	else
	{
		FDynamicMeshNormalOverlay* Normals = EditMesh.Attributes()->PrimaryNormals();

		FIndex3i TVidNormal =  Normals->GetTriangle(NearestPoint.TriangleID);
		FIndex3i TVid = EditMesh.GetTriangle(NearestPoint.TriangleID);
		FVector VertexNormals[3];
		for (int32 i = 0; i < 3; ++i)
		{
			if (TVidNormal[i] < 0 || !EditMesh.IsVertex(TVid[i]))
				continue;
			FVector3f PreNormal = EditMesh.GetVertexNormal(TVid[i]);
			FVector Normal = (FVector)Normals->GetElement(TVidNormal[i]);
			VertexNormals[i] = Normal;
		}
	
		FVector N = FVector(NearestPoint.BaryCoords[0] * VertexNormals[0] + NearestPoint.BaryCoords[1] * VertexNormals[1] + NearestPoint.BaryCoords[2] * VertexNormals[2]);
		N.Normalize();
	
		return N;
	}
}


static void ApplyPrimitiveOptionsToMesh(
	FDynamicMesh3& Mesh, const FTransform& Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions, 
	FVector3d PreTranslate = FVector3d::Zero(),
	TOptional<FQuaterniond> PreRotate = TOptional<FQuaterniond>())
{
	bool bHasTranslate = PreTranslate.SquaredLength() > 0;
	if (PreRotate.IsSet())
	{
		FFrame3d Frame(PreTranslate, *PreRotate);
		MeshTransforms::FrameCoordsToWorld(Mesh, Frame);
	}
	else if (bHasTranslate)
	{
		MeshTransforms::Translate(Mesh, PreTranslate);
	}

	MeshTransforms::ApplyTransform(Mesh, (FTransformSRT3d)Transform, true);
	if (PrimitiveOptions.PolygroupMode == EGeometryScriptPrimitivePolygroupMode::SingleGroup)
	{
		for (int32 tid : Mesh.TriangleIndicesItr())
		{
			Mesh.SetTriangleGroup(tid, 0);
		}
	}
	if (PrimitiveOptions.bFlipOrientation)
	{
		Mesh.ReverseOrientation(true);
		if (Mesh.HasAttributes())
		{
			FDynamicMeshNormalOverlay* Normals = Mesh.Attributes()->PrimaryNormals();
			for (int elemid : Normals->ElementIndicesItr())
			{
				Normals->SetElement(elemid, -Normals->GetElement(elemid));
			}
		}
	}
}


void UGeometryGeneral::AppendPrimitive(
	UDynamicMesh* TargetMesh,
	FMeshShapeGenerator* Generator, 
	FTransform Transform, 
	FGeometryScriptPrimitiveOptions PrimitiveOptions,
	FVector3d PreTranslate,
	TOptional<FQuaterniond> PreRotate)
{
	if (TargetMesh->IsEmpty())
	{
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			EditMesh.Copy(Generator);
			ApplyPrimitiveOptionsToMesh(EditMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
	else
	{
		FDynamicMesh3 TempMesh(Generator);
		ApplyPrimitiveOptionsToMesh(TempMesh, Transform, PrimitiveOptions, PreTranslate, PreRotate);
		TargetMesh->EditMesh([&](FDynamicMesh3& EditMesh)
		{
			FMeshIndexMappings TmpMappings;
			FDynamicMeshEditor Editor(&EditMesh);
			Editor.AppendMesh(&TempMesh, TmpMappings);
		}, EDynamicMeshChangeType::GeneralEdit, EDynamicMeshAttributeChangeFlags::Unknown, false);
	}
}


void UGeometryGeneral::CalculateOBBUpDir(TArray<FVector> OrientSpaceVertices, FVector& BoxUpDir)
{
	FVector ObjectUpVector = FVector(0, 0, 1);
	FOrientedBox3d Box;
	FMinVolumeBox3d MinBoxCalc;
	bool bMinBoxOK = MinBoxCalc.Solve(OrientSpaceVertices.Num(),[&](int32 Index) { return OrientSpaceVertices[Index]; });
	
	if (!bMinBoxOK || !MinBoxCalc.IsSolutionAvailable()) return;
	MinBoxCalc.GetResult(Box);
	
	FVector3d Extents = Box.Extents;
	FVector3d Center = Box.Frame.Origin;
	FQuaterniond Quat = Box.Frame.Rotation;

	float DotObjectCheck = -1;
	float DotUpCheck = -1;
	float FAxisDot = 0;
	float UpDot = 0;

	FVector3d Corner0 = Box.GetCorner(0);
	FVector3d Corner1 = Box.GetCorner(1);
	FVector3d Corner3 = Box.GetCorner(3);
	FVector3d Corner4 = Box.GetCorner(4);
	FVector3d Axis0 = Corner1 - Corner0;
	FVector3d Axis1 = Corner3 - Corner0;
	FVector3d Axis2 = Corner4 - Corner0;
	
	if (Axis0.Length() < Axis1.Length())
	{
		BoxUpDir = Axis0;
	}
	else
	{
		BoxUpDir = Axis1;
	}
	if (Axis2.Length() < BoxUpDir.Length())
	{
		BoxUpDir = Axis2;
	}
	
	BoxUpDir = BoxUpDir.GetSafeNormal();
	float Dotd = BoxUpDir.Dot(ObjectUpVector);
	if (Dotd < 0)
	{
		BoxUpDir *= -1;
	}
	BoxUpDir = (FVector)BoxUpDir;
}

bool UGeometryGeneral::HasMeshDistanceField(UStaticMesh* StaticMesh)
{
	if (!StaticMesh || !StaticMesh->GetRenderData()) return false;
	const FDistanceFieldVolumeData* DFData = StaticMesh->GetRenderData()->LODResources[0].DistanceFieldData;
	return DFData && DFData->IsValid();
}

FBox UGeometryGeneral::GetMeshDistanceFieldBounds(UStaticMesh* StaticMesh)
{
	if (!StaticMesh || !StaticMesh->GetRenderData()) return FBox(EForceInit::ForceInit);
	const FDistanceFieldVolumeData* DFData = StaticMesh->GetRenderData()->LODResources[0].DistanceFieldData;
	if (!DFData || !DFData->IsValid()) return FBox(EForceInit::ForceInit);
	return FBox((FVector)DFData->LocalSpaceMeshBounds.Min, (FVector)DFData->LocalSpaceMeshBounds.Max);
}

UStaticMesh* UGeometryGeneral::SaveDynamicMeshToStaticMesh(
	UDynamicMesh* TargetMesh,
	const FString& AssetPathAndName,
	UMeshComponent* MaterialSource,
	bool bReplaceExistingAsset,
	bool bSaveAsset,
	bool bMarkPackageDirty)
{
	if (!TargetMesh || TargetMesh->GetTriangleCount() == 0) return nullptr;

	// 槽位数按网格里出现过的最大材质 ID 定，再从组件逐槽取材质。这是本重载与
	// SaveDynamicMeshToStaticMeshWithMaterials 的唯一区别——材质从哪来，落盘本身完全一样。
	int32 NumMaterialSlots = 1;
	TargetMesh->ProcessMesh([&NumMaterialSlots](const FDynamicMesh3& ReadMesh)
	{
		if (!ReadMesh.HasAttributes() || !ReadMesh.Attributes()->HasMaterialID()) return;
		const UE::Geometry::FDynamicMeshMaterialAttribute* MaterialIDs = ReadMesh.Attributes()->GetMaterialID();
		for (const int32 TriangleID : ReadMesh.TriangleIndicesItr())
		{
			NumMaterialSlots = FMath::Max(NumMaterialSlots, MaterialIDs->GetValue(TriangleID) + 1);
		}
	});

	TArray<UMaterialInterface*> Materials;
	Materials.Reserve(NumMaterialSlots);
	for (int32 MaterialIndex = 0; MaterialIndex < NumMaterialSlots; ++MaterialIndex)
	{
		Materials.Add(MaterialSource ? MaterialSource->GetMaterial(MaterialIndex) : nullptr);
	}

	return SaveDynamicMeshToStaticMeshWithMaterials(
		TargetMesh, AssetPathAndName, Materials, bReplaceExistingAsset, bSaveAsset, bMarkPackageDirty);
}

UStaticMesh* UGeometryGeneral::SaveDynamicMeshToStaticMeshWithMaterials(
	UDynamicMesh* TargetMesh,
	const FString& AssetPathAndName,
	const TArray<UMaterialInterface*>& Materials,
	bool bReplaceExistingAsset,
	bool bSaveAsset,
	bool bMarkPackageDirty)
{
	if (!TargetMesh || TargetMesh->GetTriangleCount() == 0 || AssetPathAndName.IsEmpty())
	{
		return nullptr;
	}

	const FString SanitizedAssetPathAndName = UPackageTools::SanitizePackageName(AssetPathAndName);
	const FString AssetFolderPath = FPackageName::GetLongPackagePath(SanitizedAssetPathAndName);
	if (!AssetFolderPath.IsEmpty())
	{
		UEditorAssetLibrary::MakeDirectory(AssetFolderPath);
	}

	if (bReplaceExistingAsset && UEditorAssetLibrary::DoesAssetExist(SanitizedAssetPathAndName))
	{
		UEditorAssetLibrary::DeleteAsset(SanitizedAssetPathAndName);
	}

	FDynamicMesh3 CopyMesh;
	TargetMesh->ProcessMesh([&CopyMesh](const FDynamicMesh3& ReadMesh)
	{
		CopyMesh = ReadMesh;
	});

	const int32 NumMaterialSlots = FMath::Max(1, Materials.Num());

	UE::AssetUtils::FStaticMeshAssetOptions AssetOptions;
	AssetOptions.NewAssetPath = SanitizedAssetPathAndName;
	AssetOptions.NumSourceModels = 1;
	AssetOptions.NumMaterialSlots = NumMaterialSlots;
	AssetOptions.AssetMaterials = Materials;
	AssetOptions.bEnableRecomputeNormals = false;
	AssetOptions.bEnableRecomputeTangents = true;
	AssetOptions.CollisionType = ECollisionTraceFlag::CTF_UseComplexAsSimple;
	AssetOptions.SourceMeshes.DynamicMeshes.Add(&CopyMesh);

	UE::AssetUtils::FStaticMeshResults ResultData;
	const UE::AssetUtils::ECreateStaticMeshResult AssetResult = UE::AssetUtils::CreateStaticMeshAsset(AssetOptions, ResultData);
	UStaticMesh* NewStaticMesh = AssetResult == UE::AssetUtils::ECreateStaticMeshResult::Ok ? ResultData.StaticMesh : nullptr;
	if (!NewStaticMesh)
	{
		return nullptr;
	}

	NewStaticMesh->PostEditChange();
	NewStaticMesh->Modify();
	if (bMarkPackageDirty)
	{
		NewStaticMesh->MarkPackageDirty();
		if (UPackage* StaticMeshPackage = NewStaticMesh->GetOutermost())
		{
			StaticMeshPackage->SetDirtyFlag(true);
		}
	}
	FAssetRegistryModule::AssetCreated(NewStaticMesh);
	if (bSaveAsset)
	{
		UEditorAssetLibrary::SaveLoadedAsset(NewStaticMesh, false);
	}
	return NewStaticMesh;
}


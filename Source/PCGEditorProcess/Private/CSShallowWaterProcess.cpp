#include "CSShallowWaterProcess.h"
#include "CSAssetProcess.h"

#include "EngineUtils.h"
#include "Selection.h"
#include "EditorAssetLibrary.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MaterialUtilities.h"
#include "Editor/MaterialEditor/Public/MaterialEditingLibrary.h"
#include "AssetToolsModule.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "MeshDescription.h"
#include "Misc/PackageName.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshOperations.h"
#include "TimerManager.h"
#include "UObject/SoftObjectPath.h"


void UCSShallowWaterProcess::SaveSWData(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;

	// 文件夹与编号都来自基类的结果资产命名（AComputeShaderMeshGenerator）：关卡同级的 CSSWData
	// 目录 + actor 自己的稳定编号，重复烘焙因此始终写同一批资产名。
	const FString AssetFolderPath = InCSSWActor->GetResultAssetFolderPath();
	if (AssetFolderPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Bake failed: %s 所在关卡没有内容路径（地图未保存？）。"),
			*InCSSWActor->GetActorNameOrLabel());
		return;
	}
	const int32 ActorId = InCSSWActor->EnsureSWUniqueID();

	// 输出资产路径要在选源之前算出来：烘焙结果本身绝不能再当源网格。上一次烘焙后
	// ShowBakedResult 会清空 SimVis 实例并把结果网格挂到 ResultMeshComponent 上，若不排除，
	// 第二次烘焙就会拿"只剩湿区的稀疏网格"当源、再对自己取样，最终三角数归零，
	// 落到下面的"没有三角"分支把上一次的好资产删掉——症状就是"bake 不再生成 StaticMesh"。
	const FString DuplicateMeshPath = InCSSWActor->BuildResultAssetPath(); // <关卡目录>/CSSWData/CSSW_<编号>_SM
	const FString MeshAssetName = FPackageName::GetLongPackageAssetName(DuplicateMeshPath);

	auto IsBakedResult = [&](const UStaticMesh* Mesh) -> bool
	{
		if (!Mesh) return true;
		if (Mesh == InCSSWActor->BakedResultMesh) return true;
		return FSoftObjectPath(Mesh).GetLongPackageName() == DuplicateMeshPath;
	};

	UStaticMesh* SourceMesh = nullptr;
	TArray<FTransform> InstanceTransforms;
	const bool bUseISM = InCSSWActor->SimVisHISM
		&& !IsBakedResult(InCSSWActor->SimVisHISM->GetStaticMesh())
		&& InCSSWActor->SimVisHISM->GetInstanceCount() > 0;

	if (bUseISM)
	{
		SourceMesh = InCSSWActor->SimVisHISM->GetStaticMesh();
		const int32 NumInstances = InCSSWActor->SimVisHISM->GetInstanceCount();
		InstanceTransforms.Reserve(NumInstances);
		for (int32 i = 0; i < NumInstances; i++)
		{
			FTransform T;
			InCSSWActor->SimVisHISM->GetInstanceTransform(i, T, /*bWorldSpace=*/false);
			InstanceTransforms.Add(T);
		}
	}
	else
	{
		// SimVis 网格覆盖整个捕获域，单位变换即可（与 StartSolver 把它挂到 ResultMeshComponent 时
		// 强制 Scale=1 的约定一致）。
		UStaticMesh* Candidate = InCSSWActor->SimulationPreviewMesh;
		if (IsBakedResult(Candidate)) Candidate = InCSSWActor->SimVisHISM ? InCSSWActor->SimVisHISM->GetStaticMesh() : nullptr;
		if (IsBakedResult(Candidate)) Candidate = InCSSWActor->ResultMeshComponent ? InCSSWActor->ResultMeshComponent->GetStaticMesh() : nullptr;
		if (IsBakedResult(Candidate))
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSSW] Bake aborted on %s: 找不到可用的源网格（当前挂的是上一次的烘焙结果）。")
				TEXT("请先设置 SimulationPreviewMesh 或重新 StartSolver 生成仿真网格。已保留现有烘焙资产。"),
				*InCSSWActor->GetActorNameOrLabel());
			return;
		}
		SourceMesh = Candidate;
		InstanceTransforms.Add(FTransform::Identity);
	}

	if (!SourceMesh) return;

	TArray<FFloat16Color> Pixels;
	int32 RTWidth = 0, RTHeight = 0;
	if (InCSSWActor->RT_ResultVelHeight)
	{
		FTextureRenderTargetResource* VHResource = InCSSWActor->RT_ResultVelHeight->GameThread_GetRenderTargetResource();
		if (VHResource)
		{
			FIntPoint VHSize = VHResource->GetSizeXY();
			RTWidth = VHSize.X;
			RTHeight = VHSize.Y;
			VHResource->ReadFloat16Pixels(Pixels);
		}
	}
	if (RTWidth <= 0 || RTHeight <= 0 || Pixels.Num() != RTWidth * RTHeight)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] RT_ResultVelHeight not available for bake. Run simulation first."));
		return;
	}

	TArray<FFloat16Color> DepthWetPixels;
	int32 DWWidth = 0, DWHeight = 0;
	if (InCSSWActor->RT_ResultDepthWet)
	{
		FTextureRenderTargetResource* DWResource = InCSSWActor->RT_ResultDepthWet->GameThread_GetRenderTargetResource();
		if (DWResource)
		{
			FIntPoint DWSize = DWResource->GetSizeXY();
			DWWidth = DWSize.X;
			DWHeight = DWSize.Y;
			DWResource->ReadFloat16Pixels(DepthWetPixels);
		}
	}

	// DepthWet 只是"湿度覆盖"的二次筛选，而这张 RT 很容易在最后一次求解之后被重置
	//（SetMaterialParameter 是 BlueprintNativeEvent，蓝图侧会清它；CaptureAll / 构造脚本
	//  都会间接触发）。此时整张图都是清屏值，逐三角的 alpha 判据会把**所有**三角判成干的，
	//  于是烘焙产出空网格——这正是"bake 不再生成 StaticMesh"的现场。
	//  所以先判断这张图到底有没有有效数据：没有就整条判据停用，只按 VelHeight 的水面高度筛。
	bool bHasDepthWetData = false;
	for (const FFloat16Color& Px : DepthWetPixels)
	{
		if (Px.A.GetFloat() > 0.f) { bHasDepthWetData = true; break; }
	}
	const bool bUseDepthWetFilter = bHasDepthWetData && DWWidth > 0 && DWHeight > 0;
	if (!bUseDepthWetFilter && DepthWetPixels.Num() > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[CSSW] RT_ResultDepthWet 无有效湿度数据（求解后被重置？），本次烘焙跳过湿度筛选。"));
	}

	const FStaticMeshRenderData* RenderData = SourceMesh->GetRenderData();
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Source mesh has no render data: %s"), *GetPathNameSafe(SourceMesh));
		return;
	}
	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const int32 SrcNumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
	const int32 SrcNumIndices = LOD.IndexBuffer.GetNumIndices();
	if (SrcNumVerts == 0 || SrcNumIndices < 3)
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] Source mesh LOD0 empty: Verts=%d Indices=%d"), SrcNumVerts, SrcNumIndices);
		return;
	}

	TArray<FVector3f> SrcPositions;
	TArray<FVector3f> SrcNormals;
	TArray<FVector2f> SrcUVs;
	SrcPositions.SetNum(SrcNumVerts);
	SrcNormals.SetNum(SrcNumVerts);
	SrcUVs.SetNum(SrcNumVerts);
	for (int32 i = 0; i < SrcNumVerts; i++)
	{
		SrcPositions[i] = LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(i);
		SrcNormals[i] = FVector3f(LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(i));
		SrcUVs[i] = LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0);
	}
	TArray<uint32> SrcIndices;
	LOD.IndexBuffer.GetCopy(SrcIndices);
	const int32 SrcNumTris = SrcIndices.Num() / 3;

	UE_LOG(LogTemp, Log, TEXT("[CSSW] SrcMesh RenderData: Verts=%d, Tris=%d, Instances=%d"),
		SrcNumVerts, SrcNumTris, InstanceTransforms.Num());

	UStaticMesh* NewMesh = nullptr;
	const bool bAssetExistedBefore = UEditorAssetLibrary::DoesAssetExist(DuplicateMeshPath);
	if (bAssetExistedBefore)
	{
		NewMesh = Cast<UStaticMesh>(UEditorAssetLibrary::LoadAsset(DuplicateMeshPath));
	}
	if (!NewMesh)
	{
		FString OriginalMeshPath = GetPathNameSafe(SourceMesh);
		UObject* DupObj = UEditorAssetLibrary::DuplicateAsset(OriginalMeshPath, DuplicateMeshPath);
		NewMesh = Cast<UStaticMesh>(DupObj);
	}
	if (!NewMesh) return;

	FMeshDescription MergedDesc;
	FStaticMeshAttributes MergedAttrs(MergedDesc);
	MergedAttrs.Register();

	TVertexAttributesRef<FVector3f> MergedPositions = MergedAttrs.GetVertexPositions();
	TVertexInstanceAttributesRef<FVector3f> MergedNormals = MergedAttrs.GetVertexInstanceNormals();
	TVertexInstanceAttributesRef<FVector2f> MergedUVs = MergedAttrs.GetVertexInstanceUVs();
	TVertexInstanceAttributesRef<FVector4f> MergedColors = MergedAttrs.GetVertexInstanceColors();

	const float HalfCapture = InCSSWActor->CaptureSize * 0.5f;
	const float InvCapture = InCSSWActor->CaptureSize > 0.f ? 1.f / InCSSWActor->CaptureSize : 0.f;
	const float ActorLocationZ = InCSSWActor->GetActorLocation().Z;
	constexpr float DryThreshold = -9000.0f;
	constexpr float VelocityMax = (float)CSSW_VELOCITY_CLAMP;

	auto SRGBToLinear = [](float S) -> float
	{
		return S <= 0.04045f ? S / 12.92f : FMath::Pow((S + 0.055f) / 1.055f, 2.4f);
	};

	int32 DbgTotalTris = 0, DbgDrySkipped = 0, DbgDegenerateSkipped = 0, DbgCreated = 0;
	FPolygonGroupID DefaultGroup = MergedDesc.CreatePolygonGroup();

	constexpr float WeldThreshold = 0.01f;
	TMap<FIntVector, FVertexID> WeldMap;

	auto TrySampleBakedLocalHeight = [&](const FVector3f& LocalPos3f, float& OutLocalHeight) -> bool
	{
		float U = (LocalPos3f.X + HalfCapture) * InvCapture;
		float V = (LocalPos3f.Y + HalfCapture) * InvCapture;
		int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
		int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
		float ResultWorldHeight = Pixels[PY * RTWidth + PX].B.GetFloat();
		if (ResultWorldHeight <= DryThreshold)
		{
			return false;
		}

		OutLocalHeight = ResultWorldHeight;
		return true;
	};

	auto GetOrCreateWeldedVertex = [&](const FVector3f& LocalPos3f, float LocalHeight) -> FVertexID
	{
		FIntVector Key(
			FMath::RoundToInt32(LocalPos3f.X / WeldThreshold),
			FMath::RoundToInt32(LocalPos3f.Y / WeldThreshold),
			0);
		if (FVertexID* Found = WeldMap.Find(Key))
		{
			return *Found;
		}

		FVertexID NewVID = MergedDesc.CreateVertex();
		MergedPositions[NewVID] = FVector3f(LocalPos3f.X, LocalPos3f.Y, LocalHeight);
		WeldMap.Add(Key, NewVID);
		return NewVID;
	};

	int32 DbgDryInstancesSkipped = 0;

	// Precompute the source mesh XY bounds so we can map each instance's footprint into the
	// result texture and skip instances that contain no wet content at all.
	FBox2f SrcBoundsXY(ForceInit);
	for (int32 i = 0; i < SrcNumVerts; i++)
	{
		SrcBoundsXY += FVector2f(SrcPositions[i].X, SrcPositions[i].Y);
	}

	auto InstanceHasWetContent = [&](const FTransform& InstT) -> bool
	{
		// Build the instance's world-space XY footprint from the source mesh bounds corners.
		const FVector2f Corners[4] = {
			FVector2f(SrcBoundsXY.Min.X, SrcBoundsXY.Min.Y),
			FVector2f(SrcBoundsXY.Max.X, SrcBoundsXY.Min.Y),
			FVector2f(SrcBoundsXY.Min.X, SrcBoundsXY.Max.Y),
			FVector2f(SrcBoundsXY.Max.X, SrcBoundsXY.Max.Y),
		};
		float MinU = 1.f, MinV = 1.f, MaxU = 0.f, MaxV = 0.f;
		for (const FVector2f& C : Corners)
		{
			const FVector W = InstT.TransformPosition(FVector(C.X, C.Y, 0.f));
			const float U = ((float)W.X + HalfCapture) * InvCapture;
			const float V = ((float)W.Y + HalfCapture) * InvCapture;
			MinU = FMath::Min(MinU, U); MaxU = FMath::Max(MaxU, U);
			MinV = FMath::Min(MinV, V); MaxV = FMath::Max(MaxV, V);
		}
		const int32 PX0 = FMath::Clamp(FMath::FloorToInt32(MinU * RTWidth), 0, RTWidth - 1);
		const int32 PX1 = FMath::Clamp(FMath::FloorToInt32(MaxU * RTWidth), 0, RTWidth - 1);
		const int32 PY0 = FMath::Clamp(FMath::FloorToInt32(MinV * RTHeight), 0, RTHeight - 1);
		const int32 PY1 = FMath::Clamp(FMath::FloorToInt32(MaxV * RTHeight), 0, RTHeight - 1);
		for (int32 PY = PY0; PY <= PY1; PY++)
		{
			for (int32 PX = PX0; PX <= PX1; PX++)
			{
				if (Pixels[PY * RTWidth + PX].B.GetFloat() > DryThreshold) return true;
			}
		}
		return false;
	};

	for (const FTransform& InstT : InstanceTransforms)
	{
		// Skip whole instances that have no wet content; only baked tiles with water survive.
		if (!InstanceHasWetContent(InstT)) { DbgDryInstancesSkipped++; continue; }

		for (int32 Tri = 0; Tri < SrcNumTris; Tri++)
		{
			DbgTotalTris++;
			const uint32 I0 = SrcIndices[Tri * 3 + 0];
			const uint32 I1 = SrcIndices[Tri * 3 + 1];
			const uint32 I2 = SrcIndices[Tri * 3 + 2];

			FVector3f WP0 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I0])));
			FVector3f WP1 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I1])));
			FVector3f WP2 = FVector3f(InstT.TransformPosition(FVector(SrcPositions[I2])));

			FVector3f WorldVerts[3] = { WP0, WP1, WP2 };
			float LocalHeights[3] = {};
			bool bWetVerts[3] = {};
			int32 WetVertCount = 0;
			float WetHeightSum = 0.0f;
			for (int32 v = 0; v < 3; v++)
			{
				if (TrySampleBakedLocalHeight(WorldVerts[v], LocalHeights[v]))
				{
					bWetVerts[v] = true;
					WetVertCount++;
					WetHeightSum += LocalHeights[v];
				}
			}

			const FVector3f TriCenter = (WorldVerts[0] + WorldVerts[1] + WorldVerts[2]) / 3.0f;
			float CenterLocalHeight = 0.0f;
			const bool bCenterWet = TrySampleBakedLocalHeight(TriCenter, CenterLocalHeight);
			if (WetVertCount == 0 && !bCenterWet)
			{
				DbgDrySkipped++;
				continue;
			}

			const float FallbackLocalHeight = bCenterWet ? CenterLocalHeight : WetHeightSum / FMath::Max(WetVertCount, 1);
			for (int32 v = 0; v < 3; v++)
			{
				if (!bWetVerts[v])
					LocalHeights[v] = FallbackLocalHeight;
			}

			if (bUseDepthWetFilter)
			{
				bool bAllTransparent = true;
				for (int32 v = 0; v < 3; v++)
				{
					float U = (WorldVerts[v].X + HalfCapture) * InvCapture;
					float V = (WorldVerts[v].Y + HalfCapture) * InvCapture;
					int32 DPX = FMath::Clamp(FMath::FloorToInt32(U * DWWidth), 0, DWWidth - 1);
					int32 DPY = FMath::Clamp(FMath::FloorToInt32(V * DWHeight), 0, DWHeight - 1);
					if (DepthWetPixels[DPY * DWWidth + DPX].A.GetFloat() > 0.f)
					{
						bAllTransparent = false;
						break;
					}
				}
				if (bAllTransparent)
				{
					DbgDrySkipped++;
					continue;
				}
			}

			uint32 SrcIdx[3] = { I0, I1, I2 };
			FVertexID NewVIDs[3];
			TArray<FVertexInstanceID> NewPolyVerts;
			NewPolyVerts.Reserve(3);
			TSet<FVertexID> UniqueVerts;

			for (int32 v = 0; v < 3; v++)
			{
				FVertexID NewVID = GetOrCreateWeldedVertex(WorldVerts[v], LocalHeights[v]);
				NewVIDs[v] = NewVID;
				UniqueVerts.Add(NewVID);

				FVertexInstanceID NewVIID = MergedDesc.CreateVertexInstance(NewVID);
				MergedNormals.Set(NewVIID, SrcNormals[SrcIdx[v]]);
				MergedUVs.Set(NewVIID, 0, SrcUVs[SrcIdx[v]]);

				float U = (WorldVerts[v].X + HalfCapture) * InvCapture;
				float V = (WorldVerts[v].Y + HalfCapture) * InvCapture;
				int32 PX = FMath::Clamp(FMath::FloorToInt32(U * RTWidth), 0, RTWidth - 1);
				int32 PY = FMath::Clamp(FMath::FloorToInt32(V * RTHeight), 0, RTHeight - 1);
				const FFloat16Color& Px = Pixels[PY * RTWidth + PX];

				float NormVelX = FMath::Clamp(Px.R.GetFloat() / VelocityMax * 0.5f + 0.5f, 0.f, 1.f);
				float NormVelY = FMath::Clamp(Px.G.GetFloat() / VelocityMax * 0.5f + 0.5f, 0.f, 1.f);
				float Foam = FMath::Clamp(Px.A.GetFloat(), 0.f, 1.f);

				float DepthWetA = 0.f;
				if (bUseDepthWetFilter)
				{
					int32 DPX = FMath::Clamp(FMath::FloorToInt32(U * DWWidth), 0, DWWidth - 1);
					int32 DPY = FMath::Clamp(FMath::FloorToInt32(V * DWHeight), 0, DWHeight - 1);
					DepthWetA = FMath::Clamp(DepthWetPixels[DPY * DWWidth + DPX].A.GetFloat(), 0.f, 1.f);
				}
				MergedColors.Set(NewVIID, FVector4f(
					SRGBToLinear(NormVelX),
					SRGBToLinear(NormVelY),
					SRGBToLinear(DepthWetA),
					Foam));

				NewPolyVerts.Add(NewVIID);
			}

			if (UniqueVerts.Num() >= 3)
			{
				MergedDesc.CreatePolygon(DefaultGroup, NewPolyVerts);
				DbgCreated++;
			}
			else
			{
				DbgDegenerateSkipped++;
			}
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[CSSW] Bake: Total=%d, DryInstSkip=%d, DrySkip=%d, DegenSkip=%d, Created=%d, Instances=%d"),
		DbgTotalTris, DbgDryInstancesSkipped, DbgDrySkipped, DbgDegenerateSkipped, DbgCreated, InstanceTransforms.Num());

	if (DbgCreated == 0)
	{
		// 已经存在的烘焙资产不能因为这次没跑出三角就删掉：那会让"仿真没水"这种可恢复的
		// 操作失误直接毁掉上一次的好结果（关卡里引用它的组件也跟着变空）。
		if (bAssetExistedBefore)
		{
			UE_LOG(LogTemp, Warning, TEXT("[CSSW] No triangles created (仿真结果全是干的？)，保留现有烘焙资产 %s。"), *DuplicateMeshPath);
			return;
		}
		UE_LOG(LogTemp, Warning, TEXT("[CSSW] No triangles created, skipping mesh output."));
		NewMesh->ConditionalBeginDestroy();
		UEditorAssetLibrary::DeleteAsset(DuplicateMeshPath);
		return;
	}

	FStaticMeshOperations::ComputeTriangleTangentsAndNormals(MergedDesc, 0.0f, *MeshAssetName);
	FStaticMeshOperations::ComputeTangentsAndNormals(
		MergedDesc,
		EComputeNTBsFlags::Normals
		| EComputeNTBsFlags::Tangents
		| EComputeNTBsFlags::UseMikkTSpace
		| EComputeNTBsFlags::BlendOverlappingNormals
		| EComputeNTBsFlags::WeightedNTBs
		| EComputeNTBsFlags::IgnoreDegenerateTriangles);

	FMeshDescription* TargetDesc = NewMesh->GetMeshDescription(0);
	*TargetDesc = MoveTemp(MergedDesc);

	NewMesh->CommitMeshDescription(0);

	// 贴图先落盘：水面与焦散两个 MIC 共用同一批贴图，所以放在材质分支之外生成。
	// 资产名统一由 actor 的 BuildBakedAssetPath 决定（CSSW_<编号>_<后缀>），这里只取末段名字：
	// 落盘接口收的是"名字 + 文件夹"，而不是完整包路径。
	auto BakedAssetName = [InCSSWActor](const TCHAR* Suffix)
	{
		return FPackageName::GetLongPackageAssetName(InCSSWActor->BuildBakedAssetPath(Suffix));
	};

	FString VelHeightTexName = BakedAssetName(TEXT("VelHeight"));
	UTexture2D* VelHeightTex = InCSSWActor->RT_ResultVelHeight
		? UCSAssetProcess::ConveretAndSaveRTAsset(VelHeightTexName, AssetFolderPath, InCSSWActor->RT_ResultVelHeight)
		: nullptr;
	if (VelHeightTex)
	{
		VelHeightTex->MarkPackageDirty();
	}

	FString DepthWetTexName = BakedAssetName(TEXT("DepthWet"));
	UTexture2D* DepthWetTex = InCSSWActor->RT_ResultDepthWet
		? UCSAssetProcess::ConveretAndSaveRTAsset(DepthWetTexName, AssetFolderPath, InCSSWActor->RT_ResultDepthWet)
		: nullptr;
	if (DepthWetTex)
	{
		DepthWetTex->MarkPackageDirty();
	}

	// 把烘焙贴图与仿真域参数写进一个 MIC 资产。运行时那套 MID 绑的是 transient render target，
	// 关卡重开就没画面，所以烘焙产物必须是落盘的 MIC。
	auto BakeMaterialInstance = [&](UMaterialInterface* Parent, const TCHAR* NameSuffix) -> UMaterialInterface*
	{
		if (!Parent) return nullptr;
		const FString MIName = BakedAssetName(NameSuffix);
		UMaterialInstance* MI = UCSAssetProcess::FindOrCreateMaterialInstanceAsset(MIName, AssetFolderPath, Parent);
		UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(MI);
		if (!MIC) return Parent;

		if (VelHeightTex) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, FName("CSSW_VelHeight"), VelHeightTex);
		if (DepthWetTex) UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(MIC, FName("CSSW_DepthWet"), DepthWetTex);
		UMaterialEditingLibrary::SetMaterialInstanceVectorParameterValue(MIC, FName("CSSW_SimCenter"),
			FLinearColor(InCSSWActor->SimUVCenter.X, InCSSWActor->SimUVCenter.Y, 0, 0));
		UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, FName("CSSW_SimInvSize"), InCSSWActor->SimUVInvSize);
		UMaterialEditingLibrary::SetMaterialInstanceStaticSwitchParameterValue(MIC, FName("SwithSim"), true);
		MIC->PostEditChange();
		UEditorAssetLibrary::SaveLoadedAsset(MIC, false);
		MIC->MarkPackageDirty();
		return MIC;
	};

	UMaterialInterface* BakeMaterial = BakeMaterialInstance(InCSSWActor->WaterMaterial, TEXT("Water"));
	UMaterialInterface* BakeDecalMaterial = BakeMaterialInstance(InCSSWActor->DecalMaterial, TEXT("Decal"));

	if (BakeMaterial)
	{
		TArray<FStaticMaterial>& Materials = NewMesh->GetStaticMaterials();
		if (Materials.Num() > 0)
			Materials[0].MaterialInterface = BakeMaterial;
		else
			Materials.Add(FStaticMaterial(BakeMaterial, TEXT("Water")));
	}

	NewMesh->Build(false);
	NewMesh->PostEditChange();
	UEditorAssetLibrary::SaveLoadedAsset(NewMesh, false);
	NewMesh->MarkPackageDirty();

	if (InCSSWActor->ResultMeshComponent)
	{
		InCSSWActor->Modify();
		InCSSWActor->ResultMeshComponent->Modify();

		// 记住这次烘焙用的源网格：下一次烘焙要靠它才能避开"拿烘焙结果当源"的死循环。
		if (!InCSSWActor->SimulationPreviewMesh)
			InCSSWActor->SimulationPreviewMesh = SourceMesh;

		InCSSWActor->BakedResultMesh = NewMesh;
		InCSSWActor->BakedWaterMaterial = BakeMaterial;
		InCSSWActor->BakedDecalMaterial = BakeDecalMaterial;
		InCSSWActor->bUseBakedResultMesh = true;

		InCSSWActor->ResultMeshComponent->SetStaticMesh(NewMesh);
		InCSSWActor->ResultMeshComponent->SetRelativeScale3D(FVector::OneVector);
		InCSSWActor->ResultMeshComponent->MarkRenderStateDirty();

		// 贴烘焙 MIC 的活交给随后的 OnBakeComplete -> ShowBakedResult（它内部会调 ApplyBakedMaterials），
		// 这里不再重复调用一次。
	}
}

ACSShallowWaterCapture* UCSShallowWaterProcess::CSSW_GetSelectActor()
{
	ACSShallowWaterCapture* SelectedCapture = nullptr;
	USelection* SelectedActors = GEditor->GetSelectedActors();
	if (SelectedActors == nullptr || SelectedActors->Num() == 0) return SelectedCapture;
	AActor* SelectedActor = Cast<AActor>(SelectedActors->GetSelectedObject(0));

	if (SelectedActor == nullptr) return SelectedCapture;
	
	SelectedCapture = Cast<ACSShallowWaterCapture>(SelectedActor);
	if (SelectedCapture != nullptr) return SelectedCapture;

	AActor* ParentActor = SelectedActor->GetAttachParentActor();
	SelectedCapture = Cast<ACSShallowWaterCapture>(ParentActor);
	if (SelectedCapture != nullptr) return SelectedCapture;

	ACSShallowWaterSource* SelectedSource = Cast<ACSShallowWaterSource>(SelectedActor);
	if (SelectedSource == nullptr) return SelectedCapture;

	TArray<TEnumAsByte<EObjectTypeQuery>> ObjectTypes = {
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldStatic),
		UEngineTypes::ConvertToObjectType(ECollisionChannel::ECC_WorldDynamic)
	};
	FVector BoxExtent = FVector(5, 5, 100000);
	UWorld* World = SelectedSource->GetWorld();
	if (!World) return SelectedCapture;

	TArray<AActor*> OverlapActors;
	TArray<AActor*> ActorsToIgnore;
	UKismetSystemLibrary::BoxOverlapActors(World, SelectedSource->GetActorLocation(), BoxExtent,
	                                       ObjectTypes, ACSShallowWaterCapture::StaticClass(), ActorsToIgnore,
	                                       OverlapActors);
	DrawDebugBox(World, SelectedSource->GetActorLocation(), BoxExtent, FColor(1, 1, 1, 1), false, 1, 0, 1);
	if (OverlapActors.Num() == 0) return SelectedCapture;
	SelectedCapture = Cast<ACSShallowWaterCapture>(OverlapActors[0]);

	return SelectedCapture;
}

void UCSShallowWaterProcess::MaterialTest(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr) return;
	
	UStaticMesh* VisMesh = InCSSWActor->ResultMeshComponent->GetStaticMesh(); 
	if (VisMesh == nullptr) return;

	FName CSSWTag = FName("CSSW_Bake");
	TArray<AActor*> AttachedActors; 
	InCSSWActor->GetAttachedActors(AttachedActors);
	for (AActor* AttachedActor : AttachedActors)
	{
		if (AttachedActor->Tags.Contains(CSSWTag)) AttachedActor->Destroy();
	}
	
	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString AssetFolderPath = LevelPath.Append("/CSSWData");

	FDateTime Time = FDateTime::Now();
	FString ResultVelHeightName = FString::Printf(TEXT("T_CSSW_VelHeight_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString ResultWaterMaterialName = FString::Printf(TEXT("MI_CSSW_Water_%d%d%d"), Time.GetHour(), Time.GetMinute(), Time.GetSecond());
	FString VelHeightAssetPathFull = AssetFolderPath + "/" + ResultVelHeightName + "." + ResultVelHeightName;
	
	FString WaterMaterialPath = GetPathNameSafe(InCSSWActor->WaterMaterial);
	UCSAssetProcess::FindOrDuplicateMaterialInstanceAsset(WaterMaterialPath, VelHeightAssetPathFull, true);
}

void UCSShallowWaterProcess::DebugDumpSWPassResults(ACSShallowWaterCapture* InCSSWActor)
{
	if (InCSSWActor == nullptr)
	{
		UE_LOG(LogTemp, Error, TEXT("DebugDumpSWPassResults: InCSSWActor is null."));
		return;
	}

	ULevel* CurrentLevel = InCSSWActor->GetLevel();
	FString LevelPathName = GetPathNameSafe(CurrentLevel);
	FString FileName;
	FString LevelPath;
	FString Extension;
	FPaths::Split(LevelPathName, LevelPath, FileName, Extension);
	FString DebugFolderPath = LevelPath / TEXT("debug");

	struct FRTEntry
	{
		UTextureRenderTarget2D* RT;
		FString Name;
	};

	TArray<FRTEntry> RTsToSave = {
		{ InCSSWActor->RT_VelocityHeight,   TEXT("Debug_VelocityHeight") },
		{ InCSSWActor->RT_ResultVelHeight,   TEXT("Debug_ResultVelHeight") },
		{ InCSSWActor->RT_ResultDepthWet,    TEXT("Debug_ResultDepthWet") },
		{ InCSSWActor->RT_SmoothHeight,      TEXT("Debug_SmoothHeight") },
		{ InCSSWActor->RT_DebugView,         TEXT("Debug_DebugView") },
	};

	int32 SavedCount = 0;
	for (const FRTEntry& Entry : RTsToSave)
	{
		if (Entry.RT == nullptr)
		{
			UE_LOG(LogTemp, Warning, TEXT("DebugDumpSWPassResults: %s is null, skipping."), *Entry.Name);
			continue;
		}
		UTexture2D* SavedTexture = UCSAssetProcess::ConveretAndSaveRTAsset(Entry.Name, DebugFolderPath, Entry.RT);
		if (SavedTexture)
		{
			UEditorAssetLibrary::SaveLoadedAsset(SavedTexture, false);
			SavedCount++;
			UE_LOG(LogTemp, Log, TEXT("DebugDumpSWPassResults: Saved %s -> %s/%s"), *Entry.Name, *DebugFolderPath, *Entry.Name);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("DebugDumpSWPassResults: Failed to save %s"), *Entry.Name);
		}
	}

	UE_LOG(LogTemp, Log, TEXT("DebugDumpSWPassResults: Done. Saved %d/%d textures to %s"), SavedCount, RTsToSave.Num(), *DebugFolderPath);
}

bool UCSShallowWaterProcess::StartSWSolver(ACSShallowWaterCapture*& OutCSSWActor,
	int32 Iteration, float TimerRate)
{
	OutCSSWActor = CSSW_GetSelectActor();
	if (!OutCSSWActor)
	{
		UE_LOG(LogTemp, Warning, TEXT("StartSWSolver: No CSSW actor selected."));
		return false;
	}

	OutCSSWActor->StartSolver(TimerRate, Iteration);

	return true;
}

void UCSShallowWaterProcess::StopSWSolver(ACSShallowWaterCapture* InCSSWActor)
{
	if (!InCSSWActor) return;

	InCSSWActor->StopSolver();
}

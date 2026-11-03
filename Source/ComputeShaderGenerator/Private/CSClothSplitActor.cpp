#include "CSClothSplitActor.h"

#include "CSClothSimComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSStaticMeshAssetSink.h"

#include "Animation/AnimationAsset.h"
#include "Async/ParallelFor.h"
#include "BoneWeights.h"          // UE::AnimationCore::InvMaxRawBoneWeightFloat
#include "Components/SceneComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "CoreGlobals.h"          // IsRunningCommandlet
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "MeshDescription.h"
#include "Rendering/ColorVertexBuffer.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "RHIGlobals.h"           // GUsingNullRHI
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/UnrealType.h"   // FPropertyChangedEvent

void CSClothSkinning::ComputeSkinnedPositions(const FSkeletalMeshLODRenderData& LODData, const FSkinWeightVertexBuffer& Weights,
	TConstArrayView<FMatrix44f> RefToLocals, TArray<FVector3f>& OutPositions)
{
	OutPositions.SetNumUninitialized(int32(LODData.GetNumVertices()));
	const FPositionVertexBuffer& RefPositions = LODData.StaticVertexBuffers.PositionVertexBuffer;
	const int32 MaxInfluences = Weights.GetMaxBoneInfluences();
	for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
	{
		// 逐顶点同引擎的 GetTypedSkinnedVertexPosition<true>：Σ RefToLocal[BoneMap[i]]·p × w，按影响的顺序累加。
		ParallelFor(TEXT("CSClothSkinning.Positions"), int32(Section.NumVertices), 512, [&](int32 Index)
		{
			const uint32 Vertex = Section.BaseVertexIndex + uint32(Index);
			const FVector3f RefPosition = RefPositions.VertexPosition(Vertex);
			FVector3f Skinned = FVector3f::ZeroVector;
			for (int32 Influence = 0; Influence < MaxInfluences; ++Influence)
			{
				const uint16 RawWeight = Weights.GetBoneWeight(Vertex, uint32(Influence));
				if (RawWeight == 0) continue;
				const int32 BoneMapIndex = int32(Weights.GetBoneIndex(Vertex, uint32(Influence)));
				if (!Section.BoneMap.IsValidIndex(BoneMapIndex)) continue;
				const int32 MeshBone = Section.BoneMap[BoneMapIndex];
				if (!RefToLocals.IsValidIndex(MeshBone)) continue;
				const float Weight = float(RawWeight) * UE::AnimationCore::InvMaxRawBoneWeightFloat;
				Skinned += RefToLocals[MeshBone].TransformPosition(RefPosition) * Weight;
			}
			OutPositions[int32(Vertex)] = Skinned;
		});
	}
}

ACSClothSplitActor::ACSClothSplitActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	// 物理之后：Body 本帧的姿态已经求完；布料组件排在本 actor 之后解算（依赖见 PostRegisterAllComponents）。
	PrimaryActorTick.TickGroup = TG_PostPhysics;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);

	Body = CreateDefaultSubobject<USkeletalMeshComponent>(TEXT("Body"));
	Body->SetupAttachment(Root);
	Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	// 藏起来以后也要接着求姿态：锚点每帧从它的蒙皮位置来。
	Body->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;

	RenderComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("SplitRender"));
	RenderComponent->SetupAttachment(Root);
	// 布每帧都在变形而变换不动：VSM 的缓存页按变换失效，不标 Always 影子就停在放开那一刻。
	RenderComponent->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;

	ClothComponent = CreateDefaultSubobject<UCSClothSimComponent>(TEXT("ClothSim"));
	// Houdini vellumcloth4 / vellumsolver4 / forces 子网的参数（Docs/HoudiniFallSplit_Port.md 的「模拟」一节）。
	ClothComponent->Gravity = FVector::ZeroVector;          // gravity 0
	ClothComponent->ArealDensity = 0.1f;                    // density 0.1
	ClothComponent->StretchCompliance = 1.0f / 3.0e6f;      // 拉伸刚度 0.0003 × 1e10
	ClothComponent->CompressionCompliance = 1.0e-3f;        // 压缩刚度 1 × 1e3
	// Houdini 的弯曲刚度 3e-8（角度型）几乎为零，条带在那边仍是平的（均匀 1 cm 网格、6 次约束迭代 + 10 次平滑迭代）；
	// 这里的弯曲是跨边距离、每 substep 只解一次，在 LOD 0 原网格上柔度给到 100 时条带在三角尺度上皱成锯齿
	// （2026-10-28 实测），1 是视觉上的折中。
	ClothComponent->BendCompliance = 1.0f;
	// 小白人的边长约 1.3 cm：格距 ≤ 2 cm 的布要 16 个 substep 才拉得住（Docs/GpuClothSim_Plan.md 的 substep 扫描）。
	ClothComponent->Substeps = 16;
	ClothComponent->Damping = 0.1f;                         // 风速为 0 时 vellum 的 winddrag 0.1 就是线性阻尼
	ClothComponent->bUseTethers = false;
	ClothComponent->WindTurbulence = 205.0f;                // popwind amp 2.05 m/s
	ClothComponent->WindTurbulenceScale = 467.0f;           // swirlsize 4.67 m
	ClothComponent->WindTurbulenceSpeed = 1.0f;             // pulselength 1 s
	ClothComponent->WindNormalDrag = 0.17f;                 // airresist 0.17：POP 风是各向同性的
	ClothComponent->WindTangentialDrag = 0.17f;
}

bool ACSClothSplitActor::CanBuild() const
{
	const UWorld* World = GetWorld();
	if (!World || IsTemplate() || IsRunningCommandlet() || GUsingNullRHI) return false;
	return World->WorldType == EWorldType::Editor || World->WorldType == EWorldType::EditorPreview || World->IsGameWorld();
}

void ACSClothSplitActor::PoseBodyAtStart()
{
	// 锚点按 LOD 0 的顶点序取：别的 LOD 顶点不同，也不保证只在 LOD 0 用到的骨骼（手指）被更新。
	Body->SetForcedLOD(1);
#if WITH_EDITOR
	Body->SetUpdateAnimationInEditor(true);
#endif
	if (Animation)
	{
		Body->PlayAnimation(Animation, false);
		Body->SetPlayRate(PlayRate);
		Body->SetPosition(StartTime, false);
	}
	Body->TickAnimation(0.0f, false);
	Body->RefreshBoneTransforms();
}

bool ACSClothSplitActor::ComputeBodyPositions(TArray<FVector3f>& OutPositions)
{
	FSkeletalMeshRenderData* RenderData = Body ? Body->GetSkeletalMeshRenderData() : nullptr;
	if (!RenderData || !RenderData->LODRenderData.IsValidIndex(0)) return false;
	FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	FSkinWeightVertexBuffer* Weights = Body->GetSkinWeightBuffer(0);
	if (!Weights || !Weights->GetNeedsCPUAccess() || !LODData.StaticVertexBuffers.PositionVertexBuffer.GetVertexData())
	{
		if (!bWarnedNoCpuData)
		{
			bWarnedNoCpuData = true;
			UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: the skeletal mesh has no CPU copy of its LOD 0 vertices; enable Allow CPU Access on LOD 0."),
				*GetName());
		}
		return false;
	}
	Body->CacheRefToLocalMatrices(CachedRefToLocals);
	CSClothSkinning::ComputeSkinnedPositions(LODData, *Weights, CachedRefToLocals, OutPositions);
	return OutPositions.Num() == int32(LODData.GetNumVertices());
}

bool ACSClothSplitActor::CaptureBody(FCSGpuMeshCPUData& OutSnapshot)
{
	TArray<FVector3f> Positions;
	if (!ComputeBodyPositions(Positions)) return false;

	FSkeletalMeshLODRenderData& LODData = Body->GetSkeletalMeshRenderData()->LODRenderData[0];
	FSkinWeightVertexBuffer& Weights = *Body->GetSkinWeightBuffer(0);
	TArray<FVector3f> TangentXYZ;   // 每顶点 X、Y、Z 三个
	USkeletalMeshComponent::ComputeSkinnedTangentBasis(Body, TangentXYZ, CachedRefToLocals, LODData, Weights);

	const int32 NumVertices = Positions.Num();
	const FTransform ToWorld = Body->GetComponentTransform();
	const FStaticMeshVertexBuffer& VertexBuffer = LODData.StaticVertexBuffers.StaticMeshVertexBuffer;
	const FColorVertexBuffer& Colors = LODData.StaticVertexBuffers.ColorVertexBuffer;
	const int32 NumUVs = FMath::Clamp(int32(VertexBuffer.GetNumTexCoords()), 1, FCSGpuMeshCPUData::MaxTexCoordChannels);
	const bool bTangents = TangentXYZ.Num() == NumVertices * 3;
	const bool bColors = Colors.GetNumVertices() == uint32(NumVertices);

	OutSnapshot.Reset();
	OutSnapshot.NumTexCoordChannels = NumUVs;
	OutSnapshot.Positions.SetNumUninitialized(NumVertices);
	OutSnapshot.Normals.SetNumUninitialized(NumVertices);
	OutSnapshot.Tangents.SetNumUninitialized(NumVertices);
	OutSnapshot.BinormalSigns.SetNumUninitialized(NumVertices);
	for (int32 Channel = 0; Channel < NumUVs; ++Channel) OutSnapshot.TexCoordChannels[Channel].SetNumUninitialized(NumVertices);
	if (bColors) OutSnapshot.Colors.SetNumUninitialized(NumVertices);

	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		OutSnapshot.Positions[Vertex] = FVector3f(ToWorld.TransformPosition(FVector(Positions[Vertex])));
		const FVector3f X = bTangents ? TangentXYZ[Vertex * 3 + 0] : FVector3f(1.0f, 0.0f, 0.0f);
		const FVector3f Y = bTangents ? TangentXYZ[Vertex * 3 + 1] : FVector3f(0.0f, 1.0f, 0.0f);
		const FVector3f Z = bTangents ? TangentXYZ[Vertex * 3 + 2] : FVector3f(0.0f, 0.0f, 1.0f);
		OutSnapshot.Normals[Vertex] = FVector3f(ToWorld.TransformVectorNoScale(FVector(Z)).GetSafeNormal());
		OutSnapshot.Tangents[Vertex] = FVector3f(ToWorld.TransformVectorNoScale(FVector(X)).GetSafeNormal());
		// 引擎约定：副切线 = cross(TangentZ, TangentX) × 符号。
		OutSnapshot.BinormalSigns[Vertex] = (FVector3f::CrossProduct(Z, X) | Y) < 0.0f ? -1.0f : 1.0f;
		for (int32 Channel = 0; Channel < NumUVs; ++Channel)
		{
			OutSnapshot.TexCoordChannels[Channel][Vertex] = FVector2f(VertexBuffer.GetVertexUV(Vertex, Channel));
		}
		if (bColors) OutSnapshot.Colors[Vertex] = FVector4f(FLinearColor(Colors.VertexColor(Vertex)));
	}

	// 三角：按渲染段收，关掉的段跳过，每三角记下材质槽。
	TArray<uint32> Indices;
	LODData.MultiSizeIndexContainer.GetIndexBuffer(Indices);
	for (const FSkelMeshRenderSection& Section : LODData.RenderSections)
	{
		if (Section.bDisabled) continue;
		for (uint32 Tri = 0; Tri < Section.NumTriangles; ++Tri)
		{
			const int32 Base = int32(Section.BaseIndex + Tri * 3);
			if (Base + 2 >= Indices.Num()) break;
			OutSnapshot.Indices.Append({ Indices[Base + 0], Indices[Base + 1], Indices[Base + 2] });
			OutSnapshot.TriangleMaterialSlots.Add(int32(Section.MaterialIndex));
		}
	}
	for (int32 Slot = 0; Slot < Body->GetNumMaterials(); ++Slot) OutSnapshot.Materials.Add(Body->GetMaterial(Slot));
	OutSnapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	OutSnapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	return OutSnapshot.IsValid();
}

void ACSClothSplitActor::PushAnchors()
{
	if (!ClothComponent || !ClothComponent->HasAnchors()) return;
	if (!ComputeBodyPositions(SkinnedPositions)) return;
	ClothComponent->SetAnchorSourcePositions(SkinnedPositions, Body->GetComponentTransform());
}

void ACSClothSplitActor::Restart()
{
	if (!Body || !RenderComponent || !ClothComponent || !CanBuild()) return;
	bRebuildPending = false;
	TimeSinceRestart = 0.0;
	// 任何一步失败都让角色照常可见。
	Body->SetVisibility(true);
	if (!Body->GetSkeletalMeshAsset()) return;

	PoseBodyAtStart();
	FCSGpuMeshCPUData Mesh;
	FCSClothVertexBinding Binding;
	if (!(BakedSplit ? LoadBakedSplit(Mesh, Binding) : BuildSplit(Mesh, Binding))) return;

	// 网格挂在渲染组件下：显存归它，组件销毁时自己还（见 UCSMeshRenderComponent 的类注释）。
	if (!ClothMesh) ClothMesh = NewObject<UCSMesh>(RenderComponent, NAME_None, RF_Transient);
	if (!UCSMeshOps::CopyFromMeshSnapshot(ClothMesh, Mesh))
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: the split mesh did not upload."), *GetName());
		return;
	}
	RenderComponent->MeshMaterial = Material ? Material.Get() : Body->GetMaterial(0);
	RenderComponent->SetGpuMesh(ClothMesh);

	// 地面：跟着本 actor 走的水平面（局部原点、朝上）。关掉就把它摘掉，不碰别人加的碰撞体。
	const int32 GroundIndex = ClothComponent->Colliders.IndexOfByPredicate([this](const FCSClothCollider& Collider)
	{
		return Collider.Type == ECSClothColliderType::Plane && Collider.FollowActor == this;
	});
	if (bGroundPlane)
	{
		FCSClothCollider& Ground = GroundIndex == INDEX_NONE ? ClothComponent->Colliders.AddDefaulted_GetRef() : ClothComponent->Colliders[GroundIndex];
		Ground.Type = ECSClothColliderType::Plane;
		Ground.Center = FVector::ZeroVector;
		Ground.Axis = FVector::UpVector;
		Ground.FollowActor = this;
	}
	else if (GroundIndex != INDEX_NONE)
	{
		ClothComponent->Colliders.RemoveAt(GroundIndex);
	}

	ClothComponent->SetVertexBinding(Binding);
	ClothComponent->SetTargetMesh(ClothMesh);
	if (!ClothComponent->SetupFromMeshSync()) return;
	PushAnchors();
	Body->SetVisibility(false);
}

bool ACSClothSplitActor::BuildSplit(FCSGpuMeshCPUData& OutMesh, FCSClothVertexBinding& OutBinding)
{
	FCSGpuMeshCPUData Snapshot;
	if (!CaptureBody(Snapshot)) return false;
	TArray<FVector> Seeds;
	for (const FVector& Point : Split.SeedPoints) Seeds.Add(GetActorTransform().TransformPosition(Point));

	// Houdini：remesh2 → CharSplit 链 → attribwrangle18 的 xyzdist 绑回冻结的角色。不重网格时切的就是 LOD 0，
	// 锚点本来就是它的渲染顶点，不必再绑。
	const bool bRemesh = RemeshEdgeLength > 0.0f;
	FCSGpuMeshCPUData Remeshed;
	if (bRemesh && !CSClothBandSplit::Remesh(Snapshot, RemeshEdgeLength, Remeshed)) return false;
	FCSClothBandSplitResult Result;
	if (!CSClothBandSplit::Split(bRemesh ? Remeshed : Snapshot, Split, Seeds, Result)) return false;
	if (bRemesh && !CSClothBandSplit::BindToSurface(Snapshot, Result.Mesh, Result.Binding)) return false;
	NumPieces = Result.NumPieces;
	NumBands = Result.NumBands;
	OutMesh = MoveTemp(Result.Mesh);
	OutBinding = MoveTemp(Result.Binding);
	return true;
}

bool ACSClothSplitActor::LoadBakedSplit(FCSGpuMeshCPUData& OutMesh, FCSClothVertexBinding& OutBinding)
{
	const UCSClothSplitBakeData* Data = BakedSplit->GetAssetUserData<UCSClothSplitBakeData>();
	const FStaticMeshRenderData* RenderData = BakedSplit->GetRenderData();
	if (!Data || !RenderData || RenderData->LODResources.IsEmpty())
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: %s is not a baked split (no UCSClothSplitBakeData); bake it with BakeSplit."),
			*GetName(), *BakedSplit->GetName());
		return false;
	}
	const FSkeletalMeshRenderData* BodyData = Body->GetSkeletalMeshRenderData();
	const int32 BodyVertices = BodyData && BodyData->LODRenderData.IsValidIndex(0) ? int32(BodyData->LODRenderData[0].GetNumVertices()) : 0;
	if (Data->SourceNumVertices != BodyVertices)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: %s was baked against %s (%d LOD 0 vertices) but Body has %d; bake it again."),
			*GetName(), *BakedSplit->GetName(), *Data->SourceMesh.ToString(), Data->SourceNumVertices, BodyVertices);
		return false;
	}

	const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
	const FPositionVertexBuffer& Positions = LOD.VertexBuffers.PositionVertexBuffer;
	const FStaticMeshVertexBuffer& Vertices = LOD.VertexBuffers.StaticMeshVertexBuffer;
	const FColorVertexBuffer& Colors = LOD.VertexBuffers.ColorVertexBuffer;
	const int32 IdChannel = Data->VertexIdChannel;
	TArray<uint32> Indices;
	LOD.IndexBuffer.GetCopy(Indices);
	if (!Positions.GetVertexData() || !Vertices.GetTangentData() || !Vertices.GetTexCoordData() || Indices.IsEmpty()
		|| int32(Vertices.GetNumTexCoords()) <= IdChannel)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: %s has no CPU copy of LOD 0 or lost its vertex id channel; enable Allow CPU Access or bake again."),
			*GetName(), *BakedSplit->GetName());
		return false;
	}

	// 烘焙存的是 Body 组件空间：按 Body 现在的变换摆回世界。每个渲染顶点按它那条 UV 里的烘焙顶点号取回自己的绑定。
	const int32 NumVertices = int32(LOD.GetNumVertices());
	const bool bColors = Colors.GetVertexData() && Colors.GetNumVertices() == uint32(NumVertices);
	const FTransform ToWorld = Body->GetComponentTransform();
	OutMesh.Reset();
	OutMesh.Positions.SetNumUninitialized(NumVertices);
	OutMesh.Normals.SetNumUninitialized(NumVertices);
	OutMesh.Tangents.SetNumUninitialized(NumVertices);
	OutMesh.BinormalSigns.SetNumUninitialized(NumVertices);
	OutMesh.TexCoords().SetNumUninitialized(NumVertices);
	if (bColors) OutMesh.Colors.SetNumUninitialized(NumVertices);
	OutBinding = FCSClothVertexBinding();
	OutBinding.WeldKeys.SetNumUninitialized(NumVertices);
	OutBinding.AnchorSources.SetNumUninitialized(NumVertices);
	OutBinding.AnchorWeights.SetNumUninitialized(NumVertices);
	OutBinding.AnchorParams.SetNumUninitialized(NumVertices);
	OutBinding.SwirlDirs.SetNumUninitialized(NumVertices);
	for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
	{
		const int32 Id = FMath::RoundToInt(Vertices.GetVertexUV(uint32(Vertex), uint32(IdChannel)).X);
		if (!Data->AnchorSources.IsValidIndex(Id))
		{
			UE_LOG(LogCSCloth, Warning, TEXT("[CSClothSplit] %s: vertex %d of %s carries id %d outside the baked data; bake it again."),
				*GetName(), Vertex, *BakedSplit->GetName(), Id);
			return false;
		}
		const FVector4f TangentX = Vertices.VertexTangentX(uint32(Vertex));
		const FVector4f TangentZ = Vertices.VertexTangentZ(uint32(Vertex));
		OutMesh.Positions[Vertex] = FVector3f(ToWorld.TransformPosition(FVector(Positions.VertexPosition(uint32(Vertex)))));
		OutMesh.Normals[Vertex] = FVector3f(ToWorld.TransformVectorNoScale(FVector(FVector3f(TangentZ))).GetSafeNormal());
		OutMesh.Tangents[Vertex] = FVector3f(ToWorld.TransformVectorNoScale(FVector(FVector3f(TangentX))).GetSafeNormal());
		OutMesh.BinormalSigns[Vertex] = TangentZ.W < 0.0f ? -1.0f : 1.0f;
		OutMesh.TexCoords()[Vertex] = Vertices.GetVertexUV(uint32(Vertex), 0);
		if (bColors) OutMesh.Colors[Vertex] = FVector4f(FLinearColor(Colors.VertexColor(uint32(Vertex))));
		OutBinding.WeldKeys[Vertex] = Data->WeldKeys[Id];
		OutBinding.AnchorSources[Vertex] = Data->AnchorSources[Id];
		OutBinding.AnchorWeights[Vertex] = Data->AnchorWeights[Id];
		OutBinding.AnchorParams[Vertex] = Data->AnchorParams[Id];
		OutBinding.SwirlDirs[Vertex] = Data->SwirlDirs[Id];
	}
	OutMesh.Indices = MoveTemp(Indices);
	OutMesh.TriangleMaterialSlots.SetNumZeroed(OutMesh.Indices.Num() / 3);
	for (const FStaticMeshSection& Section : LOD.Sections)
	{
		for (uint32 Triangle = 0; Triangle < Section.NumTriangles; ++Triangle)
		{
			const int32 Index = int32(Section.FirstIndex / 3 + Triangle);
			if (OutMesh.TriangleMaterialSlots.IsValidIndex(Index)) OutMesh.TriangleMaterialSlots[Index] = Section.MaterialIndex;
		}
	}
	for (const FStaticMaterial& StaticMaterial : BakedSplit->GetStaticMaterials()) OutMesh.Materials.Add(StaticMaterial.MaterialInterface);
	OutMesh.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	OutMesh.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	NumBands = Data->NumBands;
	NumPieces = Data->NumPieces;
	return OutMesh.IsValid();
}

void ACSClothSplitActor::Replay()
{
	if (!ClothMesh || bRebuildPending || !ClothComponent || ClothComponent->IsHalted() || !ClothComponent->IsSimulationReady())
	{
		Restart();
		return;
	}
	TimeSinceRestart = 0.0;
	PoseBodyAtStart();
	// 布回到 setup 时的 rest 姿态（就是 StartTime 那一刻的身体），放开计时归零；网格被改过之类的失效退回重建。
	if (!ClothComponent->ResetToRestPose())
	{
		Restart();
		return;
	}
	PushAnchors();
}

void ACSClothSplitActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	// 拖动时 OnConstruction 会连着来：只在还没建过或属性改过时重建，挪动 actor 让锚点带着布走。
	if (!ClothMesh || bRebuildPending) Restart();
}

void ACSClothSplitActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// 每帧：Body 求完姿态 → 本 actor 把蒙皮位置交给布料 → 布料解算。会跑多次，AddPrerequisite 去重。
	if (Body) AddTickPrerequisiteComponent(Body);
	if (ClothComponent) ClothComponent->AddTickPrerequisiteActor(this);
}

void ACSClothSplitActor::BeginPlay()
{
	Super::BeginPlay();
	// PIE / 运行时的副本不带 transient 的网格过来：在这里建，动画也从头播。
	Restart();
}

void ACSClothSplitActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!ClothMesh || bRebuildPending)
	{
		Restart();
		return;
	}
	TimeSinceRestart += DeltaSeconds;
	if (LoopPeriod > 0.0f && TimeSinceRestart >= double(LoopPeriod))
	{
		Replay();
		return;
	}
	PushAnchors();
}

#if WITH_EDITOR
void ACSClothSplitActor::BakeSplit()
{
	BakeSplitToAsset(BakeAssetPath, false);
}

UStaticMesh* ACSClothSplitActor::BakeSplitToAsset(const FString& AssetPath, bool bSaveToDisk)
{
	static const TCHAR* LogPrefix = TEXT("[CSClothSplit] Bake:");
	USkeletalMesh* SourceMesh = Body ? Body->GetSkeletalMeshAsset() : nullptr;
	if (!SourceMesh || !ClothComponent || !CanBuild())
	{
		UE_LOG(LogCSCloth, Warning, TEXT("%s %s needs a skeletal mesh on Body (and a GPU)."), LogPrefix, *GetName());
		return nullptr;
	}
	const FString Path = AssetPath.IsEmpty() ? FString::Printf(TEXT("/Game/ClothSplit/SM_%s_Split"), *SourceMesh->GetName()) : AssetPath;

	// 先只读地探路径；切分成功之后才动资产（PrepareMesh 会就地清空同名资产）。
	FCSStaticMeshAssetTarget Target;
	if (!CSStaticMeshAsset::ResolveTarget(Path, /*bReplaceExisting*/ true, LogPrefix, Target)) return nullptr;
	PoseBodyAtStart();
	FCSGpuMeshCPUData Mesh;
	FCSClothVertexBinding Binding;
	if (!BuildSplit(Mesh, Binding)) return nullptr;
	UStaticMesh* StaticMesh = CSStaticMeshAsset::PrepareMesh(Target, LogPrefix);
	if (!StaticMesh || !WriteBakedSplit(StaticMesh, Mesh, Binding)) return nullptr;
	CSStaticMeshAsset::Finalize(StaticMesh, Target, bSaveToDisk, LogPrefix);
	Modify();
	BakedSplit = StaticMesh;
	bRebuildPending = true;
	return StaticMesh;
}

bool ACSClothSplitActor::BakeSplitInto(UStaticMesh* StaticMesh)
{
	if (!StaticMesh || !Body || !Body->GetSkeletalMeshAsset() || !ClothComponent || !CanBuild()) return false;
	PoseBodyAtStart();
	FCSGpuMeshCPUData Mesh;
	FCSClothVertexBinding Binding;
	if (!BuildSplit(Mesh, Binding) || !WriteBakedSplit(StaticMesh, Mesh, Binding)) return false;
	Modify();
	BakedSplit = StaticMesh;
	bRebuildPending = true;
	return true;
}

bool ACSClothSplitActor::WriteBakedSplit(UStaticMesh* StaticMesh, const FCSGpuMeshCPUData& Mesh, const FCSClothVertexBinding& Binding)
{

	// 存 Body 组件空间：烘出来的布不认 actor 摆在哪，运行时按 Body 当时的变换摆回世界。
	// 每个烘焙顶点一个 MeshDescription 顶点，UV1 = 烘焙顶点号：构建只合并同一顶点上属性相同的角，切口两侧位置相同、
	// 编号不同的顶点不会被并回去；构建重排顶点也不怕，运行时按 UV1 认人。
	const FTransform ToWorld = Body->GetComponentTransform();
	const int32 NumVertices = Mesh.Positions.Num();
	const bool bColors = Mesh.Colors.Num() == NumVertices;
	FMeshDescription Description;
	{
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();
		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector3f> Tangents = Attributes.GetVertexInstanceTangents();
		TVertexInstanceAttributesRef<float> Signs = Attributes.GetVertexInstanceBinormalSigns();
		TVertexInstanceAttributesRef<FVector4f> InstanceColors = Attributes.GetVertexInstanceColors();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		UVs.SetNumChannels(2);
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		TArray<FVertexID> VertexIds;
		VertexIds.SetNumUninitialized(NumVertices);
		Description.ReserveNewVertices(NumVertices);
		for (int32 Vertex = 0; Vertex < NumVertices; ++Vertex)
		{
			VertexIds[Vertex] = Description.CreateVertex();
			Positions[VertexIds[Vertex]] = FVector3f(ToWorld.InverseTransformPosition(FVector(Mesh.Positions[Vertex])));
		}
		TMap<int32, FPolygonGroupID> Groups;
		const int32 NumTriangles = Mesh.Indices.Num() / 3;
		Description.ReserveNewVertexInstances(NumTriangles * 3);
		Description.ReserveNewTriangles(NumTriangles);
		for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
		{
			const int32 Slot = Mesh.TriangleMaterialSlots.IsValidIndex(Triangle) ? Mesh.TriangleMaterialSlots[Triangle] : 0;
			FPolygonGroupID* Group = Groups.Find(Slot);
			if (!Group)
			{
				Group = &Groups.Add(Slot, Description.CreatePolygonGroup());
				SlotNames[*Group] = CSStaticMeshAsset::MaterialSlotName(Slot);
			}
			FVertexInstanceID Corners[3];
			for (int32 Corner = 0; Corner < 3; ++Corner)
			{
				const int32 Vertex = int32(Mesh.Indices[Triangle * 3 + Corner]);
				const FVertexInstanceID Instance = Description.CreateVertexInstance(VertexIds[Vertex]);
				Normals[Instance] = FVector3f(ToWorld.InverseTransformVectorNoScale(FVector(Mesh.Normals[Vertex])).GetSafeNormal());
				Tangents[Instance] = FVector3f(ToWorld.InverseTransformVectorNoScale(FVector(Mesh.Tangents[Vertex])).GetSafeNormal());
				Signs[Instance] = Mesh.BinormalSigns.IsValidIndex(Vertex) ? Mesh.BinormalSigns[Vertex] : 1.0f;
				InstanceColors[Instance] = bColors ? Mesh.Colors[Vertex] : FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				UVs.Set(Instance, 0, Mesh.TexCoords()[Vertex]);
				UVs.Set(Instance, 1, FVector2f(float(Vertex), 0.0f));
				Corners[Corner] = Instance;
			}
			Description.CreateTriangle(*Group, Corners);
		}
	}

	// 就地重写：材质槽与 section 表按本次网格重新装配，旧的会串到前面（同 CSStaticMeshAsset::PrepareMesh）。
	StaticMesh->GetStaticMaterials().Empty();
	StaticMesh->GetSectionInfoMap().Clear();
	// 法线切线原样保留；不生成光照 UV（它会写到 UV1，把顶点号盖掉）；全精度 UV（半精度在 2048 以上就丢整数）；
	// 留 CPU 副本（运行时从它读几何与顶点号）。
	StaticMesh->SetNumSourceModels(1);
	FMeshBuildSettings& BuildSettings = StaticMesh->GetSourceModel(0).BuildSettings;
	BuildSettings.bRecomputeNormals = false;
	BuildSettings.bRecomputeTangents = false;
	BuildSettings.bGenerateLightmapUVs = false;
	BuildSettings.bUseFullPrecisionUVs = true;
	BuildSettings.bRemoveDegenerates = false;
	StaticMesh->bAllowCPUAccess = true;
	TArray<UMaterialInterface*> Materials;
	for (UMaterialInterface* SlotMaterial : Mesh.Materials) Materials.Add(SlotMaterial);
	if (!CSStaticMeshAsset::PopulateFromDescription(StaticMesh, Description, Materials, Mesh.TriangleMaterialSlots,
		/*bCommitMeshDescription*/ true, /*bEnableNanite*/ false))
	{
		UE_LOG(LogCSCloth, Error, TEXT("[CSClothSplit] Bake: StaticMesh build failed for '%s'."), *StaticMesh->GetPathName());
		return false;
	}

	UCSClothSplitBakeData* Data = StaticMesh->GetAssetUserData<UCSClothSplitBakeData>();
	if (!Data)
	{
		Data = NewObject<UCSClothSplitBakeData>(StaticMesh, NAME_None, RF_Transactional);
		StaticMesh->AddAssetUserData(Data);
	}
	const FSkeletalMeshRenderData* SourceData = Body->GetSkeletalMeshRenderData();
	Data->SourceMesh = Body->GetSkeletalMeshAsset();
	Data->SourceNumVertices = SourceData && SourceData->LODRenderData.IsValidIndex(0) ? int32(SourceData->LODRenderData[0].GetNumVertices()) : 0;
	Data->Animation = Animation;
	Data->StartTime = StartTime;
	Data->RemeshEdgeLength = FMath::Max(RemeshEdgeLength, 0.0f);
	Data->Split = Split;
	Data->NumBands = NumBands;
	Data->NumPieces = NumPieces;
	Data->VertexIdChannel = 1;
	Data->WeldKeys = Binding.WeldKeys;
	Data->AnchorSources = Binding.AnchorSources;
	Data->AnchorWeights = Binding.AnchorWeights;
	Data->AnchorParams = Binding.AnchorParams;
	Data->SwirlDirs = Binding.SwirlDirs;
	UE_LOG(LogCSCloth, Log, TEXT("[CSClothSplit] Bake: %s: %d vertices, %d triangles, %d bands, %d pieces (remesh %.2f cm) -> %d render vertices."),
		*StaticMesh->GetPathName(), NumVertices, Mesh.Indices.Num() / 3, NumBands, NumPieces, Data->RemeshEdgeLength,
		StaticMesh->GetRenderData() && !StaticMesh->GetRenderData()->LODResources.IsEmpty() ? StaticMesh->GetRenderData()->LODResources[0].GetNumVertices() : 0);
	return true;
}

void ACSClothSplitActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	bRebuildPending = true;
}

void ACSClothSplitActor::PostEditUndo()
{
	Super::PostEditUndo();
	bRebuildPending = true;
}
#endif

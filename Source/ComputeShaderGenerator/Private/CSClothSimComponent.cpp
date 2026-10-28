#include "CSClothSimComponent.h"

#include "CSClothSimRender.h"
#include "CSGpuMemoryBudget.h"
#include "CSGpuMeshTypes.h"
#include "CSClothLayout.ush"   // CS_CLOTH_*（与 CSClothSim.usf 同一份）
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"

#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "RenderingThread.h"
#include "UObject/UnrealType.h"   // FPropertyChangedEvent

namespace
{
// Unity/jumbo 构建共享 TU：file-local 名字一律带 CSCloth_ 前缀。

/** 粒子在 sim 空间里跟着一个世界变换的变化走：x' = O' ∘ D ∘ O（O = 平移到 SimOrigin）。 */
FMatrix CSCloth_SimSpaceDelta(const FTransform& From, const FTransform& To, const FVector& SimOrigin)
{
	const FMatrix WorldDelta = From.ToInverseMatrixWithScale() * To.ToMatrixWithScale();
	return FTranslationMatrix(SimOrigin) * WorldDelta * FTranslationMatrix(-SimOrigin);
}

/** UE 的 FMatrix 是行向量约定（p' = p·M，平移在第 3 行）；shader 要的是列向量的三行 (R | t)。 */
void CSCloth_AppendRows(const FMatrix& M, TArray<FVector4f>& OutRows)
{
	for (int32 Row = 0; Row < 3; ++Row)
		OutRows.Add(FVector4f(float(M.M[0][Row]), float(M.M[1][Row]), float(M.M[2][Row]), float(M.M[3][Row])));
}

void CSCloth_RowsFromMatrix(const FMatrix& M, FVector4f (&OutRows)[3])
{
	for (int32 Row = 0; Row < 3; ++Row)
		OutRows[Row] = FVector4f(float(M.M[0][Row]), float(M.M[1][Row]), float(M.M[2][Row]), float(M.M[3][Row]));
}

/** 这些属性在 setup 时就用掉了，改了要重新 setup。 */
bool CSCloth_IsSetupProperty(FName Name)
{
	static const FName Names[] = {
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, ArealDensity),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, bUseTethers),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, TetherScale),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, WeldTolerance),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, PinBoxes),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, PinColorChannel),
		GET_MEMBER_NAME_CHECKED(UCSClothSimComponent, PinColorThreshold),
	};
	for (const FName& Candidate : Names) if (Candidate == Name) return true;
	return false;
}

/** 三次插值、两侧斜率相同（单位：值 / 秒）的关键帧 —— Houdini 的 bezier() 通道，忽略它的切线权重。 */
void CSCloth_AddCurveKey(FRichCurve& Curve, float Time, float Value, float Slope)
{
	const FKeyHandle Key = Curve.AddKey(Time, Value);
	Curve.SetKeyInterpMode(Key, RCIM_Cubic);
	Curve.SetKeyTangentMode(Key, RCTM_User, false);
	FRichCurveKey& Data = Curve.GetKey(Key);
	Data.ArriveTangent = Slope;
	Data.LeaveTangent = Slope;
}
}

UCSClothSimComponent::UCSClothSimComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.bStartWithTickEnabled = true;
	// 物理之后：宿主、AttachActor、碰撞体的本帧变换都已经定了。渲染命令仍排在本帧场景渲染之前，当帧生效。
	PrimaryComponentTick.TickGroup = TG_PostPhysics;
	bTickInEditor = true;

	// 锚点曲线的默认值 = AnimationFallSplit19.hip 的 sopsolver 通道（25 fps，模拟起点 1004 帧 = 0 秒，斜率 /帧 × 25）。
	// 之后两版把场景改成 60 fps 时关键帧按秒保留、被挪到了 2401 帧以后，落在镜头范围外 —— 那两版里这套绳实际是失效的。
	CSCloth_AddCurveKey(*AnchorStiffnessFade.GetRichCurve(), -0.12f, 1.0f, 0.0f);
	CSCloth_AddCurveKey(*AnchorStiffnessFade.GetRichCurve(), 0.24f, 1.0f, 0.0f);
	CSCloth_AddCurveKey(*AnchorStiffnessFade.GetRichCurve(), 1.80f, 0.0f, 0.0f);
	CSCloth_AddCurveKey(*AnchorReelIn.GetRichCurve(), 0.0f, 0.0f, 0.0f);
	CSCloth_AddCurveKey(*AnchorReelIn.GetRichCurve(), 1.32f, 0.8566f, 0.3968f * 25.0f);
	CSCloth_AddCurveKey(*AnchorReelIn.GetRichCurve(), 2.04f, 1.0f, 0.0f);
}

void UCSClothSimComponent::SetVertexBinding(const FCSClothVertexBinding& InBinding)
{
	Binding = InBinding;
	bHasBinding = true;
}

void UCSClothSimComponent::ClearVertexBinding()
{
	Binding = FCSClothVertexBinding();
	bHasBinding = false;
}

void UCSClothSimComponent::SetAnchorSourcePositions(TConstArrayView<FVector3f> Positions, const FTransform& ToWorld)
{
	PendingAnchorSources.Reset(Positions.Num());
	PendingAnchorSources.Append(Positions.GetData(), Positions.Num());
	PendingAnchorToWorld = ToWorld;
	bPendingAnchors = true;
}

void UCSClothSimComponent::Release()
{
	if (bAnchored && !bReleased) bReleaseRequested = true;
}

void UCSClothSimComponent::SetTargetMesh(UCSMesh* InMesh)
{
	if (InMesh == TargetMesh) return;
	ReleaseSimulation();
	TargetMesh = InMesh;
}

void UCSClothSimComponent::SetExtraPinBoxes(const TArray<FCSClothPinBox>& InPinBoxes)
{
	ExtraPinBoxes = InPinBoxes;
}

bool UCSClothSimComponent::IsSimulationReady() const
{
	return GpuState.IsValid() && TargetMesh != nullptr;
}

int32 UCSClothSimComponent::GetSimulatedFrameCount() const
{
	return GpuState.IsValid() ? int32(GpuState->FramesSimulated.load(std::memory_order_relaxed)) : 0;
}

FTransform UCSClothSimComponent::ResolvePinGroupTransform(const FPinGroup& Group) const
{
	if (const AActor* Actor = Group.Actor.Get()) return Actor->GetActorTransform();
	// 宿主（或 AttachActor）没了：钉点停在最后一次的位置，而不是弹回 setup 时的原点。
	return Group.PrevTransform;
}

bool UCSClothSimComponent::BuildSetupOptions(FCSClothSetupOptions& OutOptions)
{
	const AActor* Owner = GetOwner();
	const FTransform HostTransform = Owner ? Owner->GetActorTransform() : FTransform::Identity;

	OutOptions = FCSClothSetupOptions();
	OutOptions.WeldTolerance = FMath::Max(WeldTolerance, 1e-4f);
	OutOptions.ArealDensity = FMath::Max(ArealDensity, 1e-3f);
	OutOptions.bBuildTethers = bUseTethers;
	OutOptions.TetherScale = FMath::Clamp(TetherScale, 1.0f, 2.0f);
	OutOptions.PinColorChannel = PinColorChannel;
	OutOptions.PinColorThreshold = PinColorThreshold;

	// sim 原点取宿主位置：宿主附近的布最多，float 的分辨率留给它们。
	const FBox MeshBounds = TargetMesh ? TargetMesh->GetWorldBoundsApprox() : FBox(ForceInit);
	SimOrigin = Owner ? HostTransform.GetLocation() : (MeshBounds.IsValid ? MeshBounds.GetCenter() : FVector::ZeroVector);
	OutOptions.SimOrigin = SimOrigin;

	// 钉组 0 = 宿主；每个带 AttachActor 的钉盒按 actor 去重各占一组。
	PinGroups.Reset();
	FPinGroup& HostGroup = PinGroups.AddDefaulted_GetRef();
	HostGroup.Actor = const_cast<AActor*>(Owner);
	HostGroup.SetupTransform = HostTransform;
	HostGroup.PrevTransform = HostTransform;

	auto AddPinBox = [this, &OutOptions, &HostTransform](const FCSClothPinBox& Pin)
	{
		if (!Pin.Box.IsValid) return;
		uint32 Group = 0;
		if (AActor* Attach = Pin.AttachActor.Get())
		{
			int32 Found = PinGroups.IndexOfByPredicate([Attach](const FPinGroup& G) { return G.Actor.Get() == Attach; });
			if (Found == INDEX_NONE)
			{
				FPinGroup& NewGroup = PinGroups.AddDefaulted_GetRef();
				NewGroup.Actor = Attach;
				NewGroup.SetupTransform = Attach->GetActorTransform();
				NewGroup.PrevTransform = NewGroup.SetupTransform;
				Found = PinGroups.Num() - 1;
			}
			Group = uint32(Found);
		}
		FCSClothSetupOptions::FPinBox& Out = OutOptions.PinBoxes.AddDefaulted_GetRef();
		Out.WorldBox = Pin.Box.TransformBy(HostTransform);
		Out.Group = Group;
	};
	for (const FCSClothPinBox& Pin : PinBoxes) AddPinBox(Pin);
	for (const FCSClothPinBox& Pin : ExtraPinBoxes) AddPinBox(Pin);

	OutOptions.NumPinGroups = PinGroups.Num();
	OutOptions.Binding = bHasBinding ? &Binding : nullptr;
	return true;
}

bool UCSClothSimComponent::SetupFromMeshSync()
{
	ReleaseSimulation();
	bHalted = false;

	if (!TargetMesh)
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s: setup skipped, no target mesh."), *GetPathName());
		return false;
	}
	FCSMeshResident* Resident = TargetMesh->GetResidentPtr();
	if (!Resident || !Resident->IsAllocated())
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s: setup skipped, the target mesh has no GPU allocation."), *GetPathName());
		return false;
	}

	FCSClothSetupOptions Options;
	BuildSetupOptions(Options);

	FCSGpuMeshCPUData Snapshot;
	if (!TargetMesh->ReadbackMeshSync(Snapshot))
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s: setup failed, the mesh could not be read back."), *GetPathName());
		return false;
	}

	FCSClothSetupData Data;
	if (!CSClothSetup::BuildFromSnapshot(Snapshot, Options, Data) || !Data.IsValid()) return false;

	// 显存预检：与 UCSMesh::ConfirmGpuMemoryBudget 同一口径（探测失败不是拒绝的理由）。
	const int64 RequiredBytes = Data.GetGpuBytes();
	const CSGpuMemoryBudget::FMemorySnapshot Memory = CSGpuMemoryBudget::QueryMemorySnapshot();
	const int64 Available = Memory.AvailableVideoMemory > 0 ? Memory.AvailableVideoMemory : Memory.TotalVideoMemory;
	if (Available > 0 && RequiredBytes > int64(double(Available) * 0.7))
	{
		UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s: setup refused, %.1f MiB of simulation buffers exceed the VRAM budget (%.1f MiB free)."),
			*GetPathName(), double(RequiredBytes) / (1024.0 * 1024.0), double(Available) / (1024.0 * 1024.0));
		return false;
	}

	SetupStats.NumParticles = Data.NumParticles;
	SetupStats.NumStretch = Data.NumStretch;
	SetupStats.NumBend = Data.NumBend;
	SetupStats.NumTether = Data.NumTether;
	SetupStats.NumPinned = Data.NumPinned;
	SetupStats.NumAnchored = Data.NumAnchored;
	SetupStats.TotalMassKg = Data.TotalMassKg;
	PinGroupRestBounds = Data.PinGroupRestBounds;
	PinGroupReach = Data.PinGroupReach;
	UnanchoredRestBounds = Data.UnanchoredRestBounds;
	RestPositionsCopy = Data.RestPositions;
	RestBoundsSim = Data.RestBounds;
	PrevColliderShapes.Reset();
	SimulatedTime = 0.0;

	bAnchored = Data.HasAnchors();
	ParticleAnchorSources = Data.AnchorSources;
	ParticleAnchorWeights = Data.AnchorWeights;
	bExternalAnchors = false;
	bReleased = false;
	bReleaseRequested = false;
	ReleaseTime = 0.0;

	FCSClothGpuStateRef NewState = MakeShared<FCSClothGpuState, ESPMode::ThreadSafe>();
	FCSMeshResidentRef ResidentRef = TargetMesh->GetResident();
	ENQUEUE_RENDER_COMMAND(CSClothSetup)(
		[NewState, ResidentRef, Data = MoveTemp(Data)](FRHICommandListImmediate& RHICmdList)
		{
			CSClothRender::InitState_RenderThread(RHICmdList, *NewState, Data, *ResidentRef);
		});

	// 包围盒一次性放大到布料够得着的范围（D2 的 Bounds）；同一次同步编辑也把精确计数写上。
	// 这次编辑排在上面那条 setup 命令之后，它的 flush 同时就是 setup 完成的栅栏。
	TArray<FTransform> GroupTransforms;
	for (const FPinGroup& Group : PinGroups) GroupTransforms.Add(Group.SetupTransform);
	const FBox ReachBounds = ComputeReachBounds(GroupTransforms);
	const FBox Inflated = ReachBounds.ExpandBy(FMath::Max(ReachBounds.GetExtent().GetMax() * 0.25, 10.0));
	const int32 VertexCount = Snapshot.Positions.Num();
	const int32 IndexCount = Snapshot.Indices.Num();
	TargetMesh->EditMeshSync([Inflated, VertexCount, IndexCount](FCSMeshEditContext& Context)
	{
		Context.SetWorldBounds(Inflated);
		Context.SetKnownCounts(VertexCount, IndexCount);
	});
	PublishedBounds = Inflated;

	ExpectedGeneration = TargetMesh->GetGeneration();
	ExpectedAllocationGeneration = Resident->AllocationGeneration;
	GpuState = MoveTemp(NewState);

	// 画这张网格的渲染组件：布每帧变形而变换不动，VSM 的缓存页按变换失效、不会自己发现 —— 标 Always，
	// 否则影子停在 rest 姿态上。只管同一个宿主上的组件；别处借这张网格显示的，由那边自己标。
	if (AActor* Owner = GetOwner())
	{
		TInlineComponentArray<UCSMeshRenderComponent*> RenderComponents(Owner);
		for (UCSMeshRenderComponent* Render : RenderComponents)
		{
			if (Render->GetGpuMesh() != TargetMesh
				|| Render->ShadowCacheInvalidationBehavior == EShadowCacheInvalidationBehavior::Always) continue;
			Render->ShadowCacheInvalidationBehavior = EShadowCacheInvalidationBehavior::Always;
			Render->MarkRenderStateDirty();
		}
	}

	UE_LOG(LogCSCloth, Log, TEXT("[CSCloth] %s: %d particles (%d pinned, %d anchored), %d stretch / %d bend / %d tether constraints, %.3f kg, %.2f MiB."),
		*GetPathName(), SetupStats.NumParticles, SetupStats.NumPinned, SetupStats.NumAnchored, SetupStats.NumStretch, SetupStats.NumBend,
		SetupStats.NumTether, SetupStats.TotalMassKg, double(RequiredBytes) / (1024.0 * 1024.0));
	return true;
}

bool UCSClothSimComponent::ResetToRestPose()
{
	if (!IsSimulationReady() || PinGroups.Num() == 0) return false;
	if (!ValidateTargetMesh()) return false;

	// rest 形状跟着宿主当前的变换摆：宿主自 setup 以来挪过的话，布回到「挂在宿主现在位置上」的 rest 姿态，
	// 而不是 setup 时的老地方（那样下一帧钉点会被拽回宿主身边）。
	const FTransform HostNow = ResolvePinGroupTransform(PinGroups[0]);
	const FMatrix HostDelta = CSCloth_SimSpaceDelta(PinGroups[0].SetupTransform, HostNow, SimOrigin);
	TArray<FVector4f> Positions = RestPositionsCopy;
	for (FVector4f& P : Positions)
	{
		const FVector Moved = HostDelta.TransformPosition(FVector(P.X, P.Y, P.Z));
		P = FVector4f(float(Moved.X), float(Moved.Y), float(Moved.Z), P.W);
	}
	for (FPinGroup& Group : PinGroups) Group.PrevTransform = ResolvePinGroupTransform(Group);
	PrevColliderShapes.Reset();

	// 带锚点的布：重置 = 从头再放一次（回到跟随阶段，放开计时归零）。
	if (bAnchored)
	{
		SimulatedTime = 0.0;
		bReleased = false;
		bReleaseRequested = false;
		ReleaseTime = 0.0;
	}

	FCSClothGpuStateRef State = GpuState;
	FCSMeshResidentRef ResidentRef = TargetMesh->GetResident();
	const FVector3f Origin(SimOrigin);
	ENQUEUE_RENDER_COMMAND(CSClothReset)(
		[State, ResidentRef, Origin, Positions = MoveTemp(Positions)](FRHICommandListImmediate& RHICmdList)
		{
			CSClothRender::ResetToPositions_RenderThread(RHICmdList, *State, Positions, *ResidentRef, Origin);
		});
	return true;
}

void UCSClothSimComponent::Halt(const FString& Reason)
{
	if (bHalted) return;
	bHalted = true;
	UE_LOG(LogCSCloth, Warning, TEXT("[CSCloth] %s halted: %s Call SetupFromMeshSync to resume."), *GetPathName(), *Reason);
}

bool UCSClothSimComponent::ValidateTargetMesh()
{
	if (!TargetMesh)
	{
		Halt(TEXT("the target mesh is gone."));
		return false;
	}
	const FCSMeshResident* Resident = TargetMesh->GetResidentPtr();
	if (!Resident || !Resident->IsAllocated())
	{
		Halt(TEXT("the target mesh lost its GPU allocation."));
		return false;
	}
	if (Resident->AllocationGeneration != ExpectedAllocationGeneration)
	{
		Halt(TEXT("the target mesh was reallocated since setup, so the constraints no longer describe its buffers."));
		return false;
	}
	if (Resident->Generation != ExpectedGeneration)
	{
		Halt(TEXT("the target mesh was edited since setup (an operator ran on it), so the constraints no longer describe its geometry."));
		return false;
	}
	return true;
}

void UCSClothSimComponent::AppendColliderShape(const FCSClothCollider& Collider, TArray<FVector4f>& OutShape) const
{
	const AActor* Follow = Collider.FollowActor.Get();
	const FTransform Transform = Follow ? Follow->GetActorTransform() : FTransform::Identity;
	const float Scale = float(Transform.GetMaximumAxisScale());
	const FVector Center = Transform.TransformPosition(Collider.Center) - SimOrigin;
	const FVector Axis = Transform.TransformVectorNoScale(Collider.Axis).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);

	switch (Collider.Type)
	{
	case ECSClothColliderType::Capsule:
	{
		const FVector Half = Axis * double(FMath::Max(Collider.HalfHeight, 0.0f) * Scale);
		const FVector A = Center - Half;
		const FVector B = Center + Half;
		OutShape.Add(FVector4f(float(A.X), float(A.Y), float(A.Z), FMath::Max(Collider.Radius, 0.0f) * Scale));
		OutShape.Add(FVector4f(float(B.X), float(B.Y), float(B.Z), float(CS_CLOTH_COLLIDER_CAPSULE)));
		break;
	}
	case ECSClothColliderType::Plane:
		OutShape.Add(FVector4f(float(Axis.X), float(Axis.Y), float(Axis.Z), float(FVector::DotProduct(Axis, Center))));
		OutShape.Add(FVector4f(0.0f, 0.0f, 0.0f, float(CS_CLOTH_COLLIDER_PLANE)));
		break;
	default:
		OutShape.Add(FVector4f(float(Center.X), float(Center.Y), float(Center.Z), FMath::Max(Collider.Radius, 0.0f) * Scale));
		OutShape.Add(FVector4f(0.0f, 0.0f, 0.0f, float(CS_CLOTH_COLLIDER_SPHERE)));
		break;
	}
}

FBox UCSClothSimComponent::ComputeReachBounds(const TArray<FTransform>& GroupTransforms) const
{
	// 每组：钉住部分 × 当前组变换，外扩挂在这组上的自由粒子能被拉到的最远距离。
	// 连不到钉点的部分（自由落体的布）没有上限可言，按 rest 尺寸放宽 —— 掉出这个范围会被剔除，
	// 那种用法要么加地面碰撞体，要么周期性重新 setup。
	FBox Result(ForceInit);
	for (int32 Group = 0; Group < PinGroupRestBounds.Num() && Group < GroupTransforms.Num() && Group < PinGroups.Num(); ++Group)
	{
		if (!PinGroupRestBounds[Group].IsValid) continue;
		const FMatrix Delta = CSCloth_SimSpaceDelta(PinGroups[Group].SetupTransform, GroupTransforms[Group], SimOrigin);
		const FBox Pinned = FBox(PinGroupRestBounds[Group]).TransformBy(Delta);
		Result += Pinned.ExpandBy(double(PinGroupReach.IsValidIndex(Group) ? PinGroupReach[Group] : 0.0f)).ShiftBy(SimOrigin);
	}
	if (UnanchoredRestBounds.IsValid)
	{
		const FBox Free(UnanchoredRestBounds);
		const FMatrix HostDelta = GroupTransforms.Num() > 0 && PinGroups.Num() > 0
			? CSCloth_SimSpaceDelta(PinGroups[0].SetupTransform, GroupTransforms[0], SimOrigin) : FMatrix::Identity;
		Result += Free.TransformBy(HostDelta).ExpandBy(Free.GetExtent().GetMax() * 2.0 + 200.0).ShiftBy(SimOrigin);
	}
	if (!Result.IsValid && RestBoundsSim.IsValid) Result = FBox(RestBoundsSim).ShiftBy(SimOrigin);
	return Result.ExpandBy(double(CollisionThickness) + 1.0);
}

void UCSClothSimComponent::MaybePublishBounds(const FBox& ReachBounds)
{
	if (!ReachBounds.IsValid || !TargetMesh) return;
	// 还装得下、也没有大出太多（体积 8 倍以内）就不动：包围盒变了要走一次网格编辑，不该每帧都来。
	if (PublishedBounds.IsValid && PublishedBounds.IsInside(ReachBounds)
		&& PublishedBounds.GetVolume() < 8.0 * FMath::Max(ReachBounds.GetVolume(), 1.0)) return;
	if (TargetMesh->IsEditInFlight()) return;   // 别人的异步编辑在途：下一帧再来

	const FBox Inflated = ReachBounds.ExpandBy(FMath::Max(ReachBounds.GetExtent().GetMax() * 0.25, 10.0));
	TWeakObjectPtr<UCSClothSimComponent> WeakThis(this);
	TWeakObjectPtr<UCSMesh> WeakMesh(TargetMesh);
	// 异步编辑：不阻塞，包围盒在完成回调那一跳的游戏线程上落地（与 Generation 同一跳，所以本组件
	// 在回调里把期望的 Generation 跟上，不会把自己的这次编辑误判成「网格被别人改了」）。
	const bool bAccepted = TargetMesh->EditMeshAsync(
		[Inflated](FCSMeshEditContext& Context) { Context.SetWorldBounds(Inflated); },
		[WeakThis, WeakMesh](bool bMeshAlive)
		{
			UCSClothSimComponent* This = WeakThis.Get();
			UCSMesh* Mesh = WeakMesh.Get();
			if (!This || !Mesh || !bMeshAlive || This->TargetMesh != Mesh) return;
			This->ExpectedGeneration = Mesh->GetGeneration();
		});
	if (bAccepted) PublishedBounds = Inflated;
}

bool UCSClothSimComponent::StepSimulation(float DeltaTime)
{
	if (!IsSimulationReady() || bHalted || PinGroups.Num() == 0) return false;
	if (!ValidateTargetMesh()) return false;

	const uint32 NonFinite = GpuState->NonFiniteParticles.load(std::memory_order_relaxed);
	if (NonFinite > 0)
	{
		Halt(FString::Printf(TEXT("%u particles went non-finite (the simulation exploded); their vertices kept the last good pose."), NonFinite));
		return false;
	}

	const float FrameDt = FMath::Min(DeltaTime * FMath::Max(TimeScale, 0.0f), FMath::Max(MaxDeltaTime, 0.001f));
	if (!(FrameDt > 0.0f)) return false;

	FCSClothFrameParams Params;
	Params.Substeps = FMath::Clamp(Substeps, 1, 32);
	Params.Iterations = FMath::Clamp(Iterations, 1, 8);
	Params.SubstepDt = FrameDt / float(Params.Substeps);
	Params.DampingFactor = FMath::Exp(-FMath::Max(Damping, 0.0f) * Params.SubstepDt);
	Params.MaxSpeed = FMath::Max(MaxSpeed, 1.0f);
	Params.Gravity = FVector3f(Gravity);

	SimulatedTime += FrameDt;
	Params.bWind = !WindVelocity.IsNearlyZero() || WindTurbulence > 0.0f;
	Params.WindVelocity = FVector3f(WindVelocity);
	Params.WindTurbulence = FMath::Max(WindTurbulence, 0.0f);
	Params.WindNoiseFrequency = 1.0f / FMath::Max(WindTurbulenceScale, 1.0f);
	// 噪声的置换表 256 一周期：相位取模，长时间运行也不会把 float 的精度耗在相位上。
	const float Phase = float(FMath::Fmod(SimulatedTime * double(FMath::Max(WindTurbulenceSpeed, 0.0f)), 256.0));
	Params.WindNoiseOffset = FVector3f(Phase, Phase * 0.61f, Phase * 0.37f);
	Params.WindNormalDrag = FMath::Max(WindNormalDrag, 0.0f);
	Params.WindTangentialDrag = FMath::Max(WindTangentialDrag, 0.0f);

	Params.StretchCompliance = FMath::Max(StretchCompliance, 0.0f);
	Params.CompressionCompliance = FMath::Max(CompressionCompliance, 0.0f);
	Params.BendCompliance = FMath::Max(BendCompliance, 0.0f);
	Params.TetherCompliance = 0.0f;
	Params.MaxCorrection = FMath::Clamp(MaxCorrection, 0.1f, 100.0f);
	Params.CollisionThickness = FMath::Max(CollisionThickness, 0.0f);
	Params.Friction = FMath::Clamp(Friction, 0.0f, 1.0f);
	Params.SimOrigin = FVector3f(SimOrigin);

	// --- 钉组：上一帧与本帧的 sim 空间变换（相对 setup）
	TArray<FTransform> Now;
	Now.Reserve(PinGroups.Num());
	for (const FPinGroup& Group : PinGroups) Now.Add(ResolvePinGroupTransform(Group));

	const double HostStep = FVector::Dist(PinGroups[0].PrevTransform.GetLocation(), Now[0].GetLocation());
	if (TeleportDistance > 0.0f && HostStep > double(TeleportDistance))
	{
		// 瞬移：自由粒子随宿主刚体搬过去（速度一起转），钉组不在这一帧里扫过中间的路。
		Params.bTeleport = true;
		CSCloth_RowsFromMatrix(CSCloth_SimSpaceDelta(PinGroups[0].PrevTransform, Now[0], SimOrigin), Params.TeleportRows);
		for (int32 Group = 0; Group < PinGroups.Num(); ++Group) PinGroups[Group].PrevTransform = Now[Group];
		PrevColliderShapes.Reset();
	}

	Params.PinTransforms.Reserve(PinGroups.Num() * CS_CLOTH_PIN_GROUP_FLOAT4S);
	for (int32 Group = 0; Group < PinGroups.Num(); ++Group)
	{
		CSCloth_AppendRows(CSCloth_SimSpaceDelta(PinGroups[Group].SetupTransform, PinGroups[Group].PrevTransform, SimOrigin), Params.PinTransforms);
		CSCloth_AppendRows(CSCloth_SimSpaceDelta(PinGroups[Group].SetupTransform, Now[Group], SimOrigin), Params.PinTransforms);
	}

	// --- 碰撞体：上一帧形状 + 本帧形状（数量变了就从本帧形状起步，不在两个不相干的形状之间插值）
	TArray<FVector4f> Shapes;
	Shapes.Reserve(Colliders.Num() * 2);
	for (const FCSClothCollider& Collider : Colliders) AppendColliderShape(Collider, Shapes);
	if (PrevColliderShapes.Num() != Shapes.Num()) PrevColliderShapes = Shapes;
	Params.Colliders.Reserve(Colliders.Num() * CS_CLOTH_COLLIDER_FLOAT4S);
	for (int32 Collider = 0; Collider < Colliders.Num(); ++Collider)
	{
		Params.Colliders.Add(PrevColliderShapes[Collider * 2 + 0]);
		Params.Colliders.Add(PrevColliderShapes[Collider * 2 + 1]);
		Params.Colliders.Add(Shapes[Collider * 2 + 0]);
		Params.Colliders.Add(Shapes[Collider * 2 + 1]);
	}
	PrevColliderShapes = MoveTemp(Shapes);

	// --- 锚点、放开与场（带锚点的布）
	if (bAnchored)
	{
		if (!bReleased && (bReleaseRequested || (ReleaseDelay >= 0.0f && SimulatedTime >= double(ReleaseDelay))))
		{
			bReleased = true;
			bReleaseRequested = false;
			ReleaseTime = SimulatedTime;
			Params.ReleaseState = CS_CLOTH_RELEASE_KICK;
		}
		else
		{
			Params.ReleaseState = bReleased ? CS_CLOTH_RELEASE_FREE : CS_CLOTH_RELEASE_FOLLOW;
		}
		Params.ReleaseKick = ReleaseVelocityScale;

		// Houdini sopsolver 的两行：stiffness = lerp(1000, 0, stiffnesschange)，restlength = lerp(0, noise, clamp(high + 1 − restlengthchange))。
		const float Since = float(SimulatedTime - ReleaseTime);
		const float Fade = FMath::Clamp(AnchorStiffnessFade.GetRichCurveConst()->Eval(Since, 1.0f), 0.0f, 1.0f);
		const float Stiffness = FMath::Max(AnchorMaxStiffness, 0.0f) * (1.0f - Fade);
		Params.bAnchorConstraints = bReleased && Stiffness > UE_KINDA_SMALL_NUMBER;
		Params.AnchorCompliance = Params.bAnchorConstraints ? 1.0f / Stiffness : 0.0f;
		Params.AnchorLengthMin = float(FMath::Max(FMath::Min(AnchorRestLengthRange.X, AnchorRestLengthRange.Y), 0.0));
		Params.AnchorLengthMax = float(FMath::Max(FMath::Max(AnchorRestLengthRange.X, AnchorRestLengthRange.Y), 0.0));
		Params.AnchorReelIn = AnchorReelIn.GetRichCurveConst()->Eval(Since, 0.0f);
		Params.FieldSwirlSpeed = SwirlSpeed;
		Params.FieldOutwardSpeed = OutwardSpeed;
		Params.FieldBlend = (bReleased && FieldDrag > 0.0f) ? 1.0f - FMath::Exp(-FieldDrag * Params.SubstepDt) : 0.0f;

		// 宿主交了源位置：按每粒子的源与权重收成锚点（double 变换到 sim 空间，大世界坐标下不丢精度）。
		if (bPendingAnchors && ParticleAnchorSources.Num() == RestPositionsCopy.Num())
		{
			bExternalAnchors = true;
			FBox AnchorBounds(ForceInit);
			Params.AnchorTargets.SetNumUninitialized(RestPositionsCopy.Num());
			for (int32 Particle = 0; Particle < RestPositionsCopy.Num(); ++Particle)
			{
				const FIntVector Sources = ParticleAnchorSources[Particle];
				const FVector3f Weights = ParticleAnchorWeights[Particle];
				FVector3f Local = FVector3f::ZeroVector;
				float WeightSum = 0.0f;
				for (int32 K = 0; K < 3; ++K)
				{
					if (!PendingAnchorSources.IsValidIndex(Sources[K])) continue;
					Local += PendingAnchorSources[Sources[K]] * Weights[K];
					WeightSum += Weights[K];
				}
				if (WeightSum > UE_KINDA_SMALL_NUMBER)
				{
					const FVector World = PendingAnchorToWorld.TransformPosition(FVector(Local / WeightSum));
					AnchorBounds += World;
					const FVector Sim = World - SimOrigin;
					Params.AnchorTargets[Particle] = FVector4f(float(Sim.X), float(Sim.Y), float(Sim.Z), 0.0f);
				}
				else
				{
					// 没有源的粒子（钉住的那些）：锚点停在 rest 位置，反正它们不读锚点。
					const FVector4f& Rest = RestPositionsCopy[Particle];
					Params.AnchorTargets[Particle] = FVector4f(Rest.X, Rest.Y, Rest.Z, 0.0f);
				}
			}
			if (AnchorBounds.IsValid) LastAnchorWorldBounds = AnchorBounds;
			bPendingAnchors = false;
		}
		Params.bExternalAnchors = bExternalAnchors;
	}

	FCSClothGpuStateRef State = GpuState;
	FCSMeshResidentRef ResidentRef = TargetMesh->GetResident();
	ENQUEUE_RENDER_COMMAND(CSClothSimulate)(
		[State, ResidentRef, Params = MoveTemp(Params)](FRHICommandListImmediate& RHICmdList)
		{
			CSClothRender::SimulateFrame_RenderThread(RHICmdList, *State, Params, *ResidentRef);
		});

	for (int32 Group = 0; Group < PinGroups.Num(); ++Group) PinGroups[Group].PrevTransform = Now[Group];
	if (bAnchored && bExternalAnchors && LastAnchorWorldBounds.IsValid)
	{
		// 外部锚点（动画角色）：包围盒 = 锚点的包围盒，放开后外扩绳长再加自由阶段能飞出去的余量。
		const double Reach = bReleased ? FMath::Max(AnchorRestLengthRange.X, AnchorRestLengthRange.Y) + 300.0 : 20.0;
		MaybePublishBounds(LastAnchorWorldBounds.ExpandBy(Reach));
	}
	else
	{
		MaybePublishBounds(ComputeReachBounds(Now));
	}
	return true;
}

void UCSClothSimComponent::ReleaseSimulation()
{
	if (GpuState.IsValid())
	{
		// 最后一份引用交给渲染线程放：状态里的回读对象只能在那边删（排在已入队的帧命令之后）。
		ENQUEUE_RENDER_COMMAND(CSClothRelease)(
			[State = MoveTemp(GpuState)](FRHICommandListImmediate&) mutable
			{
				State.Reset();
			});
	}
	GpuState.Reset();
	SetupStats = FSetupStats();
	PinGroups.Reset();
	PinGroupRestBounds.Reset();
	PinGroupReach.Reset();
	RestPositionsCopy.Reset();
	PrevColliderShapes.Reset();
	ParticleAnchorSources.Reset();
	ParticleAnchorWeights.Reset();
	LastAnchorWorldBounds = FBox(ForceInit);
	bAnchored = false;
	bReleased = false;
	bReleaseRequested = false;
	bExternalAnchors = false;
}

bool UCSClothSimComponent::ReadbackParticlesSync(TArray<FVector4f>& OutWorldPositions) const
{
	if (!GpuState.IsValid() || !CSClothRender::ReadbackParticlesSync(GpuState, OutWorldPositions)) return false;
	for (FVector4f& P : OutWorldPositions)
	{
		const FVector World = FVector(P.X, P.Y, P.Z) + SimOrigin;
		P = FVector4f(float(World.X), float(World.Y), float(World.Z), P.W);
	}
	return true;
}

void UCSClothSimComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!bSimulate) return;
	const UWorld* World = GetWorld();
	if (!World || (!World->IsGameWorld() && !bSimulateInEditor)) return;
	StepSimulation(DeltaTime);
}

void UCSClothSimComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
	ReleaseSimulation();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UCSClothSimComponent::BeginDestroy()
{
	ReleaseSimulation();
	Super::BeginDestroy();
}

#if WITH_EDITOR
void UCSClothSimComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 只有 setup 用掉的属性需要重建；柔度、风、碰撞体这些每帧上传，下一帧就生效，布不会被重置。
	const FName Changed = PropertyChangedEvent.MemberProperty ? PropertyChangedEvent.MemberProperty->GetFName() : NAME_None;
	if (!CSCloth_IsSetupProperty(Changed) || !TargetMesh) return;

	// setup 以网格此刻的姿态为 rest：先把网格摆回 rest，再重建，免得把摆到一半的形状当成新的 rest。
	if (IsSimulationReady() && !bHalted) ResetToRestPose();
	SetupFromMeshSync();
}

void UCSClothSimComponent::PostEditUndo()
{
	Super::PostEditUndo();
	// 只在已经跑起来时重建：还没 setup 的组件（宿主 actor 的首帧 tick 会建）不该被撤销抢先 setup。
	if (!IsSimulationReady()) return;
	if (!bHalted) ResetToRestPose();
	SetupFromMeshSync();
}
#endif

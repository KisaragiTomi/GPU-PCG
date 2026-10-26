#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSClothActor.h"
#include "CSClothSim.h"
#include "CSClothSimComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSClothLayout.ush"   // CS_CLOTH_*
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// GPU 布料（Docs/GpuClothSim_Plan.md）。P0 的验收门：悬挂布 N 帧后钉点不动、自由端下降、边长误差 < 5%、
// 无 NaN；外加每帧零阻塞、rest 姿态写回恒等、碰撞 / 风 / 顶点色钉点 / 失配停摆 / 跟随宿主 / 瞬移 / 重置。
// GPU 用例必须真 RHI（-NullRHI 下本插件的 compute shader 不存在），用 Scripts/RunPluginAutomationTests.ps1 跑。
// -----------------------------------------------------------------------------

namespace CSClothTest
{
constexpr float FrameDt = 1.0f / 60.0f;

/** 宿主 actor（带根组件，才挪得动）+ 网格 + 布料组件。bSimulate 关着：用例自己逐帧 StepSimulation。 */
struct FRig
{
	UWorld* World = nullptr;
	AActor* Host = nullptr;
	UCSMesh* Mesh = nullptr;
	UCSClothSimComponent* Cloth = nullptr;
	UCSMeshRenderComponent* Render = nullptr;
};

bool MakeRig(FAutomationTestBase& Test, const FCSGpuMeshCPUData& Snapshot,
	TFunctionRef<void(UCSClothSimComponent&)> Configure, FRig& Out, bool bWithRenderComponent = false)
{
	Out = FRig();
	Out.World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!Test.TestNotNull(TEXT("Editor test world"), Out.World)) return false;

	Out.Host = Out.World->SpawnActor<AActor>();
	if (!Test.TestNotNull(TEXT("Host actor"), Out.Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Out.Host);
	Out.Host->SetRootComponent(Root);
	Root->RegisterComponent();

	Out.Mesh = UCSMeshOps::AllocateGpuMesh(Out.World, 3, 3);
	if (!Test.TestTrue(TEXT("Cloth mesh upload"), UCSMeshOps::CopyFromMeshSnapshot(Out.Mesh, Snapshot))) return false;

	if (bWithRenderComponent)
	{
		Out.Render = NewObject<UCSMeshRenderComponent>(Out.Host);
		Out.Render->SetupAttachment(Root);
		Out.Render->RegisterComponent();
		Out.Render->SetGpuMesh(Out.Mesh);
	}

	Out.Cloth = NewObject<UCSClothSimComponent>(Out.Host);
	Out.Cloth->bSimulate = false;
	Configure(*Out.Cloth);
	Out.Cloth->RegisterComponent();
	Out.Cloth->SetTargetMesh(Out.Mesh);
	return Test.TestTrue(TEXT("Cloth setup"), Out.Cloth->SetupFromMeshSync());
}

void Step(FRig& Rig, int32 Frames)
{
	for (int32 Frame = 0; Frame < Frames; ++Frame) Rig.Cloth->StepSimulation(FrameDt);
}

/** 宿主在原点、单位变换时，组件的 setup 选项就是这些（SimOrigin = 宿主位置 = 0）——用例拿它在 CPU 上
 *  复刻同一份粒子编号与约束，用来量边长。 */
FCSClothSetupOptions HostAtOriginOptions(const TArray<FBox>& WorldPinBoxes, bool bTethers = true)
{
	FCSClothSetupOptions Options;
	for (const FBox& Box : WorldPinBoxes)
	{
		FCSClothSetupOptions::FPinBox& Pin = Options.PinBoxes.AddDefaulted_GetRef();
		Pin.WorldBox = Box;
	}
	Options.bBuildTethers = bTethers;
	return Options;
}

void AddPinBoxes(UCSClothSimComponent& Cloth, const TArray<FBox>& Boxes)
{
	for (const FBox& Box : Boxes)
	{
		FCSClothPinBox& Pin = Cloth.PinBoxes.AddDefaulted_GetRef();
		Pin.Box = Box;
	}
}

float RestLength(const FUintVector4& Constraint)
{
	float Rest = 0.0f;
	FMemory::Memcpy(&Rest, &Constraint.Z, sizeof(Rest));
	return Rest;
}

/** 拉伸约束的相对边长误差 |L/L0 − 1| 的均值与最大值。 */
void StretchError(const FCSClothSetupData& Data, const TArray<FVector4f>& Particles, double& OutMean, double& OutMax)
{
	OutMean = 0.0;
	OutMax = 0.0;
	int32 Count = 0;
	for (const FUintVector4& Constraint : Data.Constraints)
	{
		if (Constraint.W != CS_CLOTH_CONSTRAINT_STRETCH) continue;   // 约束按颜色存放，类型看 .W
		const float Rest = RestLength(Constraint);
		if (Rest <= UE_KINDA_SMALL_NUMBER) continue;
		const FVector3f A(Particles[Constraint.X]);
		const FVector3f B(Particles[Constraint.Y]);
		const double Error = FMath::Abs(double(FVector3f::Dist(A, B)) / double(Rest) - 1.0);
		OutMean += Error;
		OutMax = FMath::Max(OutMax, Error);
		++Count;
	}
	if (Count > 0) OutMean /= Count;
}

bool AllFinite(const TArray<FVector4f>& Particles)
{
	for (const FVector4f& P : Particles)
		if (!FMath::IsFinite(P.X) || !FMath::IsFinite(P.Y) || !FMath::IsFinite(P.Z)) return false;
	return true;
}
}

// -----------------------------------------------------------------------------
// setup 的 CPU 半程：焊接、约束计数、质量、钉点、tether、双面网格去重（无需 RHI）
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothSetupTopologyTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.SetupTopology",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSClothSetupTopologyTest::RunTest(const FString& Parameters)
{
	// 4×3 格、格宽 10 cm：5×4 = 20 个格点，24 个三角。
	// 边：横 4×4 + 竖 5×3 + 对角 4×3 = 43；边界边 2×4 + 2×3 = 14 ⇒ 内边 29 = 弯曲约束数。
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(40.0, 30.0), FIntPoint(4, 3), FTransform::Identity, false, Grid);
	TestEqual(TEXT("Grid vertices"), Grid.Positions.Num(), 20);
	TestEqual(TEXT("Grid triangles"), Grid.Indices.Num() / 3, 24);

	// 钉住 −X 那一列（4 个格点）：它们之间的 3 条竖边两端都钉死，不出拉伸约束。
	const FBox PinColumn(FVector(-21.0, -16.0, -1.0), FVector(-19.0, 16.0, 1.0));
	FCSClothSetupData Data;
	if (!TestTrue(TEXT("Setup builds"), CSClothSetup::BuildFromSnapshot(Grid, CSClothTest::HostAtOriginOptions({ PinColumn }), Data))) return false;

	TestEqual(TEXT("One particle per grid point"), Data.NumParticles, 20);
	TestEqual(TEXT("Every triangle survives"), Data.TriangleParticles.Num() / 3, 24);
	TestEqual(TEXT("Stretch: 43 edges minus 3 with both ends pinned"), Data.NumStretch, 40);
	TestEqual(TEXT("Bend: one per interior edge"), Data.NumBend, 29);
	TestEqual(TEXT("Pinned column"), Data.NumPinned, 4);
	TestEqual(TEXT("Tether: one per free particle"), Data.NumTether, 16);
	TestTrue(TEXT("Mass = area × density (1200 cm² × 0.2 kg/m²)"), FMath::IsNearlyEqual(Data.TotalMassKg, 0.024, 1e-6));

	int32 PinnedWithMass = 0;
	for (int32 P = 0; P < Data.NumParticles; ++P)
	{
		const bool bPinned = Data.PinGroups[P] != CS_CLOTH_FREE_PARTICLE;
		if (bPinned != (Data.RestPositions[P].W == 0.0f)) ++PinnedWithMass;
	}
	TestEqual(TEXT("InvMass is zero exactly on the pinned particles"), PinnedWithMass, 0);

	// 可达半径：最远的自由粒子沿网格离钉列 40 cm，× TetherScale（默认 1.02）。
	TestTrue(TEXT("Group 0 reach = 40 cm × TetherScale"), Data.PinGroupReach.Num() == 1
		&& FMath::IsNearlyEqual(Data.PinGroupReach[0], 40.0f * FCSClothSetupOptions().TetherScale, 0.01f));

	bool bTethersAnchored = true;
	for (const FUintVector4& Tether : Data.Constraints)
	{
		if (Tether.W != CS_CLOTH_CONSTRAINT_TETHER) continue;
		bTethersAnchored &= Data.PinGroups[Tether.X] == CS_CLOTH_FREE_PARTICLE
			&& Data.PinGroups[Tether.Y] != CS_CLOTH_FREE_PARTICLE
			&& CSClothTest::RestLength(Tether) >= FVector3f::Dist(FVector3f(Data.RestPositions[Tether.X]), FVector3f(Data.RestPositions[Tether.Y]));
	}
	TestTrue(TEXT("Tethers run from a free particle to a pinned one, never shorter than at rest"), bTethersAnchored);

	// 上色：颜色段首尾相接覆盖全部约束；同一种颜色里没有两条约束共享可动粒子；tether 全在最后的颜色里。
	int32 Covered = 0;
	bool bColorsIndependent = true;
	bool bTethersLast = true;
	bool bSeenTetherColor = false;
	for (const FIntPoint& Range : Data.ColorRanges)
	{
		bColorsIndependent &= Range.X == Covered;
		Covered += Range.Y;
		TSet<uint32> Touched;
		bool bHasTether = false, bHasDistance = false;
		for (int32 C = Range.X; C < Range.X + Range.Y; ++C)
		{
			const FUintVector4& Record = Data.Constraints[C];
			for (const uint32 P : { Record.X, Record.Y })
			{
				if (Data.RestPositions[P].W <= 0.0f) continue;   // 钉住的端点只读，共享不算冲突
				bool bAlready = false;
				Touched.Add(P, &bAlready);
				bColorsIndependent &= !bAlready;
			}
			(Record.W == CS_CLOTH_CONSTRAINT_TETHER ? bHasTether : bHasDistance) = true;
		}
		bTethersLast &= !(bHasTether && bHasDistance) && !(bSeenTetherColor && bHasDistance);
		bSeenTetherColor |= bHasTether;
	}
	TestEqual(TEXT("Colour ranges cover every constraint"), Covered, Data.Constraints.Num());
	TestTrue(TEXT("No two constraints of one colour share a movable particle"), bColorsIndependent);
	TestTrue(TEXT("Tethers get their own colours, solved last"), bTethersLast);
	TestEqual(TEXT("One free particle per tether, so the tethers fit in one colour"),
		Data.ColorRanges.Num() > 0 ? Data.ColorRanges.Last().Y : 0, Data.NumTether);
	AddInfo(FString::Printf(TEXT("%d constraints in %d colours"), Data.Constraints.Num(), Data.ColorRanges.Num()));

	// 双面：背面焊到同一批粒子上，重复三角被剔除（否则正反两面的面法线互相抵消）。
	FCSGpuMeshCPUData DoubleSided;
	CSClothSetup::BuildGridSnapshot(FVector2D(40.0, 30.0), FIntPoint(4, 3), FTransform::Identity, true, DoubleSided);
	TestEqual(TEXT("Double-sided grid has twice the vertices"), DoubleSided.Positions.Num(), 40);
	FCSClothSetupData DoubleData;
	if (!TestTrue(TEXT("Double-sided setup builds"), CSClothSetup::BuildFromSnapshot(DoubleSided, CSClothTest::HostAtOriginOptions({ PinColumn }), DoubleData))) return false;
	TestEqual(TEXT("Back side welds onto the front particles"), DoubleData.NumParticles, 20);
	TestEqual(TEXT("Back-side triangles are dropped as duplicates"), DoubleData.TriangleParticles.Num() / 3, 24);
	TestEqual(TEXT("Same constraints as the single-sided cloth"), DoubleData.Constraints.Num(), Data.Constraints.Num());
	bool bBackMapsToFront = true;
	for (int32 V = 0; V < 20; ++V) bBackMapsToFront &= DoubleData.CornerToParticle[V] == DoubleData.CornerToParticle[V + 20];
	TestTrue(TEXT("Each back vertex shares its front twin's particle"), bBackMapsToFront);

	// 不钉：没有 tether，全部粒子算作连不到钉点。
	FCSClothSetupData Free;
	if (!TestTrue(TEXT("Unpinned setup builds"), CSClothSetup::BuildFromSnapshot(Grid, CSClothTest::HostAtOriginOptions({}), Free))) return false;
	TestEqual(TEXT("No pins, no tethers"), Free.NumTether, 0);
	TestEqual(TEXT("No pins: every edge is a stretch constraint"), Free.NumStretch, 43);
	TestTrue(TEXT("Unanchored bounds cover the whole cloth"), Free.UnanchoredRestBounds.IsValid
		&& Free.UnanchoredRestBounds.GetSize().Equals(FVector3f(40.0f, 30.0f, 0.0f), 0.01f));

	// 逐角点快照被拒：布料的映射域是逐渲染顶点的。
	FCSGpuMeshCPUData PerCorner = Grid;
	PerCorner.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerCorner;
	FCSClothSetupData Refused;
	AddExpectedError(TEXT("per-corner"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Per-corner snapshot is refused"), CSClothSetup::BuildFromSnapshot(PerCorner, CSClothTest::HostAtOriginOptions({}), Refused));
	return true;
}

// -----------------------------------------------------------------------------
// P0 验收：悬挂布
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothHangingTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.HangingCloth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothHangingTest::RunTest(const FString& Parameters)
{
	// 1 m × 1 m、5 cm 一格的水平布，Z = 200，−X 边钉在一根「杆」上：松开后整块往下摆、挂成竖直的帘子。
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(100.0, 100.0), FIntPoint(20, 20), FTransform(FVector(0.0, 0.0, 200.0)), false, Grid);
	const FBox PinEdge(FVector(-51.0, -51.0, 199.0), FVector(-49.0, 51.0, 201.0));

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [&PinEdge](UCSClothSimComponent& Cloth) { CSClothTest::AddPinBoxes(Cloth, { PinEdge }); }, Rig, true)) return false;
	TestEqual(TEXT("Particles"), Rig.Cloth->GetParticleCount(), 21 * 21);
	TestEqual(TEXT("Pinned edge"), Rig.Cloth->GetSetupStats().NumPinned, 21);

	FCSClothSetupData Reference;
	if (!TestTrue(TEXT("CPU reference setup"), CSClothSetup::BuildFromSnapshot(Grid, CSClothTest::HostAtOriginOptions({ PinEdge }), Reference))) return false;

	// 运行循环零阻塞：这是「每帧纯 GPU」那条结论唯一能自动化的证据。
	const int64 FlushesBefore = UCSMesh::GetBlockingFlushCount();
	CSClothTest::Step(Rig, 120);
	TestEqual(TEXT("Stepping 120 frames blocks the game thread zero times"), UCSMesh::GetBlockingFlushCount() - FlushesBefore, int64(0));

	UCSMesh::CountedBlockingFlush();
	TestEqual(TEXT("Every frame ran on the render thread"), Rig.Cloth->GetSimulatedFrameCount(), 120);
	TestFalse(TEXT("Not halted"), Rig.Cloth->IsHalted());

	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	if (!TestEqual(TEXT("Particle count"), Particles.Num(), Reference.NumParticles)) return false;
	TestTrue(TEXT("No NaN / Inf"), CSClothTest::AllFinite(Particles));

	double PinDrift = 0.0;
	double FreeEdgeZ = 0.0;
	int32 FreeEdgeCount = 0;
	for (int32 P = 0; P < Particles.Num(); ++P)
	{
		const FVector3f Rest(Reference.RestPositions[P]);
		if (Reference.PinGroups[P] != CS_CLOTH_FREE_PARTICLE)
		{
			PinDrift = FMath::Max(PinDrift, double(FVector3f::Dist(FVector3f(Particles[P]), Rest)));
		}
		else if (Rest.X > 49.0f)
		{
			FreeEdgeZ += Particles[P].Z;
			++FreeEdgeCount;
		}
	}
	TestTrue(FString::Printf(TEXT("Pinned particles stay put (max drift %.4f cm)"), PinDrift), PinDrift < 0.01);
	FreeEdgeZ /= FMath::Max(FreeEdgeCount, 1);
	TestTrue(FString::Printf(TEXT("The free edge fell (mean Z %.1f, started at 200)"), FreeEdgeZ), FreeEdgeZ < 150.0);

	double MeanError = 0.0, MaxError = 0.0;
	CSClothTest::StretchError(Reference, Particles, MeanError, MaxError);
	AddInfo(FString::Printf(TEXT("Stretch error after 120 frames: mean %.3f%%, max %.3f%%"), MeanError * 100.0, MaxError * 100.0));
	TestTrue(FString::Printf(TEXT("Edge length error < 5%% (max %.2f%%)"), MaxError * 100.0), MaxError < 0.05);

	// 写回：网格的 Position 流就是粒子位置（经 CornerToParticle），法线跟着形变转过来了。
	FCSGpuMeshCPUData Mesh;
	if (!TestTrue(TEXT("Mesh readback"), Rig.Mesh->ReadbackMeshSync(Mesh))) return false;
	double SkinError = 0.0;
	for (int32 V = 0; V < Mesh.Positions.Num(); ++V)
		SkinError = FMath::Max(SkinError, double(FVector3f::Dist(Mesh.Positions[V], FVector3f(Particles[Reference.CornerToParticle[V]]))));
	TestTrue(FString::Printf(TEXT("Mesh positions are the particle positions (max %.4f cm)"), SkinError), SkinError < 0.01);

	// CPU 按同一口径（引擎正面 cross(C−A, B−A)、角度加权）从回读位置重算法线，与 GPU 写回的比夹角。
	TArray<FVector3f> Expected;
	Expected.SetNumZeroed(Reference.NumParticles);
	for (int32 T = 0; T + 2 < Reference.TriangleParticles.Num(); T += 3)
	{
		const uint32 I[3] = { Reference.TriangleParticles[T], Reference.TriangleParticles[T + 1], Reference.TriangleParticles[T + 2] };
		const FVector3f A(Particles[I[0]]), B(Particles[I[1]]), C(Particles[I[2]]);
		const FVector3f Face = FVector3f::CrossProduct(C - A, B - A).GetSafeNormal();
		const FVector3f Corners[3] = { A, B, C };
		for (int32 K = 0; K < 3; ++K)
		{
			const FVector3f U = (Corners[(K + 1) % 3] - Corners[K]).GetSafeNormal();
			const FVector3f W = (Corners[(K + 2) % 3] - Corners[K]).GetSafeNormal();
			Expected[I[K]] += Face * FMath::Acos(FMath::Clamp(FVector3f::DotProduct(U, W), -1.0f, 1.0f));
		}
	}
	int32 BadNormals = 0;
	double WorstNormalDeg = 0.0;
	for (int32 V = 0; V < Mesh.Normals.Num(); ++V)
	{
		const FVector3f Want = Expected[Reference.CornerToParticle[V]].GetSafeNormal();
		const double Degrees = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(double(FVector3f::DotProduct(Want, Mesh.Normals[V].GetSafeNormal())), -1.0, 1.0)));
		WorstNormalDeg = FMath::Max(WorstNormalDeg, Degrees);
		if (Degrees > 5.0) ++BadNormals;
	}
	TestEqual(FString::Printf(TEXT("Written-back normals follow the deformed surface (worst %.2f°)"), WorstNormalDeg), BadNormals, 0);

	// 包围盒：网格发布的包围盒（渲染组件的 CalcBounds 从这里来）装得下每个粒子。
	const FBox Published = Rig.Mesh->GetWorldBoundsApprox();
	int32 Outside = 0;
	for (const FVector4f& P : Particles) Outside += Published.IsInsideOrOn(FVector(P.X, P.Y, P.Z)) ? 0 : 1;
	TestEqual(TEXT("Published bounds contain every particle"), Outside, 0);
	TestTrue(TEXT("The render component draws with VSM invalidation on"),
		Rig.Render && Rig.Render->ShadowCacheInvalidationBehavior == EShadowCacheInvalidationBehavior::Always);

	// 冻结当前姿态：CopyToStaticMesh 读的就是被模拟的流。
	FCSMeshToStaticMeshOptions Bake;
	Bake.bTransient = true;
	Bake.bBakeToLocalSpace = false;
	UStaticMesh* Frozen = UCSMeshOps::CopyToStaticMesh(Rig.Mesh, Rig.World, nullptr, Bake);
	TestTrue(TEXT("The current pose freezes into a StaticMesh"), Frozen != nullptr && Frozen->GetBoundingBox().Min.Z < 160.0);
	return true;
}

// -----------------------------------------------------------------------------
// 无外力时写回恒等：rest 姿态下旋转是单位阵，位置与切线基原样不动
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothRestIdentityTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.RestPoseIsIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothRestIdentityTest::RunTest(const FString& Parameters)
{
	// 斜着摆、双面：切线基有非平凡的方向，背面的法线与副切线符号都是反的 —— 恒等要对每一种都成立。
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(80.0, 60.0), FIntPoint(8, 6),
		FTransform(FRotator(25.0, 40.0, 10.0), FVector(300.0, -200.0, 150.0)), true, Grid);

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [](UCSClothSimComponent& Cloth) { Cloth.Gravity = FVector::ZeroVector; }, Rig)) return false;

	FCSGpuMeshCPUData Before;
	if (!TestTrue(TEXT("Readback before"), Rig.Mesh->ReadbackMeshSync(Before))) return false;
	CSClothTest::Step(Rig, 10);
	FCSGpuMeshCPUData After;
	if (!TestTrue(TEXT("Readback after"), Rig.Mesh->ReadbackMeshSync(After))) return false;
	if (!TestEqual(TEXT("Vertex count"), After.Positions.Num(), Before.Positions.Num())) return false;

	double PositionDrift = 0.0, NormalDrift = 0.0, TangentDrift = 0.0;
	for (int32 V = 0; V < Before.Positions.Num(); ++V)
	{
		PositionDrift = FMath::Max(PositionDrift, double(FVector3f::Dist(Before.Positions[V], After.Positions[V])));
		NormalDrift = FMath::Max(NormalDrift, double(FVector3f::Dist(Before.Normals[V], After.Normals[V])));
		TangentDrift = FMath::Max(TangentDrift, double(FVector3f::Dist(Before.Tangents[V], After.Tangents[V])));
	}
	TestTrue(FString::Printf(TEXT("Positions unchanged (max %.5f cm)"), PositionDrift), PositionDrift < 1e-3);
	// 8 bit SNORM 一格是 1/127；重新单位化与正交化最多挪一格。
	TestTrue(FString::Printf(TEXT("Normals unchanged within quantisation (max %.4f)"), NormalDrift), NormalDrift < 2.0 / 127.0);
	TestTrue(FString::Printf(TEXT("Tangents unchanged within quantisation (max %.4f)"), TangentDrift), TangentDrift < 2.0 / 127.0);
	return true;
}

// -----------------------------------------------------------------------------
// 碰撞：没钉的布落到球上，任何粒子都不在球（+ 厚度）里
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothSphereCollisionTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.SphereCollision",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothSphereCollisionTest::RunTest(const FString& Parameters)
{
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(120.0, 120.0), FIntPoint(24, 24), FTransform(FVector(0.0, 0.0, 100.0)), false, Grid);

	const float Radius = 40.0f;
	const float Thickness = 1.0f;
	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [Radius, Thickness](UCSClothSimComponent& Cloth)
	{
		FCSClothCollider& Sphere = Cloth.Colliders.AddDefaulted_GetRef();
		Sphere.Type = ECSClothColliderType::Sphere;
		Sphere.Center = FVector::ZeroVector;
		Sphere.Radius = Radius;
		Cloth.CollisionThickness = Thickness;
	}, Rig)) return false;
	TestEqual(TEXT("Nothing pinned"), Rig.Cloth->GetSetupStats().NumPinned, 0);

	CSClothTest::Step(Rig, 180);

	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	TestTrue(TEXT("No NaN / Inf"), CSClothTest::AllFinite(Particles));

	double Closest = TNumericLimits<double>::Max();
	double CenterZ = 0.0;
	double CenterDist = TNumericLimits<double>::Max();
	for (const FVector4f& P : Particles)
	{
		const FVector X(P.X, P.Y, P.Z);
		Closest = FMath::Min(Closest, X.Size());
		const double Planar = FVector2D(P.X, P.Y).Size();
		if (Planar < CenterDist) { CenterDist = Planar; CenterZ = P.Z; }
	}
	TestTrue(FString::Printf(TEXT("No particle inside the sphere + thickness (closest %.3f cm, limit %.1f)"), Closest, Radius + Thickness),
		Closest >= double(Radius + Thickness) - 0.05);
	TestTrue(FString::Printf(TEXT("The cloth rests on top of the sphere (centre Z %.2f)"), CenterZ),
		FMath::Abs(CenterZ - double(Radius + Thickness)) < 2.0);
	return true;
}

// -----------------------------------------------------------------------------
// P1 风：竖直挂着的帘子，沿法线吹 +X，自由部分往 +X 鼓
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothWindTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.Wind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothWindTest::RunTest(const FString& Parameters)
{
	// 绕 Y 俯仰 90°：网格局部 +X → 世界 +Z，法线 +Z → 世界 −X。帘子在 X = 0 的 YZ 平面里，顶边 Z = 250 钉住。
	FCSGpuMeshCPUData Curtain;
	CSClothSetup::BuildGridSnapshot(FVector2D(100.0, 100.0), FIntPoint(20, 20), FTransform(FRotator(90.0, 0.0, 0.0), FVector(0.0, 0.0, 200.0)), false, Curtain);
	const FBox TopEdge(FVector(-1.0, -51.0, 249.0), FVector(1.0, 51.0, 251.0));

	auto MeanFreeX = [this, &Curtain, &TopEdge](const FVector& Wind, double& OutMeanX) -> bool
	{
		CSClothTest::FRig Rig;
		if (!CSClothTest::MakeRig(*this, Curtain, [&TopEdge, &Wind](UCSClothSimComponent& Cloth)
		{
			CSClothTest::AddPinBoxes(Cloth, { TopEdge });
			Cloth.WindVelocity = Wind;
		}, Rig)) return false;
		if (!TestEqual(TEXT("Top edge pinned"), Rig.Cloth->GetSetupStats().NumPinned, 21)) return false;
		CSClothTest::Step(Rig, 90);

		TArray<FVector4f> Particles;
		if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
		OutMeanX = 0.0;
		int32 Free = 0;
		for (const FVector4f& P : Particles)
		{
			if (P.W <= 0.0f) continue;
			OutMeanX += P.X;
			++Free;
		}
		OutMeanX /= FMath::Max(Free, 1);
		return TestTrue(TEXT("No NaN / Inf"), CSClothTest::AllFinite(Particles));
	};

	double StillX = 0.0, WindyX = 0.0;
	if (!MeanFreeX(FVector::ZeroVector, StillX)) return false;
	if (!MeanFreeX(FVector(500.0, 0.0, 0.0), WindyX)) return false;
	TestTrue(FString::Printf(TEXT("Without wind the curtain hangs in its plane (mean X %.2f)"), StillX), FMath::Abs(StillX) < 1.0);
	TestTrue(FString::Printf(TEXT("Wind along +X billows the curtain toward +X (mean X %.2f)"), WindyX), WindyX > 10.0);
	return true;
}

// -----------------------------------------------------------------------------
// P1 顶点色钉点：R 通道 = 1 的那条边钉住
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothVertexColorPinTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.VertexColorPins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothVertexColorPinTest::RunTest(const FString& Parameters)
{
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(100.0, 100.0), FIntPoint(10, 10), FTransform(FVector(0.0, 0.0, 200.0)), false, Grid);
	int32 Painted = 0;
	for (int32 V = 0; V < Grid.Positions.Num(); ++V)
	{
		const bool bEdge = Grid.Positions[V].X < -49.0f;
		Grid.Colors[V] = FVector4f(bEdge ? 1.0f : 0.0f, 0.0f, 0.0f, 1.0f);
		Painted += bEdge ? 1 : 0;
	}

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [](UCSClothSimComponent& Cloth) { Cloth.PinColorChannel = ECSClothPinColorChannel::Red; }, Rig)) return false;
	TestEqual(TEXT("Exactly the painted edge is pinned"), Rig.Cloth->GetSetupStats().NumPinned, Painted);

	CSClothTest::Step(Rig, 60);
	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	double PinnedDrift = 0.0, FreeZ = 0.0;
	int32 Free = 0;
	for (const FVector4f& P : Particles)
	{
		if (P.W <= 0.0f)
		{
			PinnedDrift = FMath::Max(PinnedDrift, FMath::Max(FMath::Abs(double(P.Z) - 200.0), FMath::Abs(double(P.X) + 50.0)));
			continue;
		}
		FreeZ += P.Z;
		++Free;
	}
	FreeZ /= FMath::Max(Free, 1);
	TestTrue(FString::Printf(TEXT("Painted particles stay on the rail (drift %.4f)"), PinnedDrift), PinnedDrift < 0.01);
	TestTrue(FString::Printf(TEXT("The unpainted part fell (mean Z %.1f, started at 200)"), FreeZ), FreeZ < 185.0);
	return true;
}

// -----------------------------------------------------------------------------
// 失配停摆：setup 之后别的算子改了网格，布料停下，重新 setup 才恢复
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothHaltOnForeignEditTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.HaltsOnForeignEdit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothHaltOnForeignEditTest::RunTest(const FString& Parameters)
{
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(100.0, 100.0), FIntPoint(10, 10), FTransform(FVector(0.0, 0.0, 200.0)), false, Grid);

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [](UCSClothSimComponent&) {}, Rig)) return false;
	TestTrue(TEXT("Steps while the mesh is untouched"), Rig.Cloth->StepSimulation(CSClothTest::FrameDt));

	UCSMeshOps::SetVertexColors(Rig.Mesh, FLinearColor::Red);
	AddExpectedError(TEXT("edited since setup"), EAutomationExpectedErrorFlags::Contains, 1);
	TestFalse(TEXT("Refuses to step after a foreign edit"), Rig.Cloth->StepSimulation(CSClothTest::FrameDt));
	TestTrue(TEXT("Reports itself halted"), Rig.Cloth->IsHalted());

	TestTrue(TEXT("Setup again"), Rig.Cloth->SetupFromMeshSync());
	TestFalse(TEXT("Setup clears the halt"), Rig.Cloth->IsHalted());
	TestTrue(TEXT("Steps again"), Rig.Cloth->StepSimulation(CSClothTest::FrameDt));
	return true;
}

// -----------------------------------------------------------------------------
// 钉点跟着宿主走；一帧挪太远按瞬移整块搬过去；重置回到挂在宿主现位置上的 rest 姿态
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothFollowHostTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.PinsFollowHost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothFollowHostTest::RunTest(const FString& Parameters)
{
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(100.0, 100.0), FIntPoint(10, 10), FTransform(FVector(0.0, 0.0, 200.0)), false, Grid);
	const FBox PinEdge(FVector(-51.0, -51.0, 199.0), FVector(-49.0, 51.0, 201.0));

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [&PinEdge](UCSClothSimComponent& Cloth) { CSClothTest::AddPinBoxes(Cloth, { PinEdge }); }, Rig)) return false;

	FCSClothSetupData Reference;
	if (!TestTrue(TEXT("CPU reference setup"), CSClothSetup::BuildFromSnapshot(Grid, CSClothTest::HostAtOriginOptions({ PinEdge }), Reference))) return false;

	// 平滑拖动：30 帧挪 +100 cm X（每帧 3.3 cm，远低于瞬移阈值），再停 10 帧。
	for (int32 Frame = 1; Frame <= 30; ++Frame)
	{
		Rig.Host->SetActorLocation(FVector(100.0 * Frame / 30.0, 0.0, 0.0));
		Rig.Cloth->StepSimulation(CSClothTest::FrameDt);
	}
	CSClothTest::Step(Rig, 10);

	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	double PinError = 0.0;
	for (int32 P = 0; P < Particles.Num(); ++P)
	{
		if (Reference.PinGroups[P] == CS_CLOTH_FREE_PARTICLE) continue;
		const FVector3f Want = FVector3f(Reference.RestPositions[P]) + FVector3f(100.0f, 0.0f, 0.0f);
		PinError = FMath::Max(PinError, double(FVector3f::Dist(FVector3f(Particles[P]), Want)));
	}
	TestTrue(FString::Printf(TEXT("Pinned particles followed the host exactly (error %.4f cm)"), PinError), PinError < 0.01);

	// 瞬移：一帧跳 +2000 cm（> TeleportDistance 500），整块布跟着刚体搬过去，而不是被钉边拽着甩过去。
	TArray<FVector4f> BeforeJump = Particles;
	Rig.Host->SetActorLocation(FVector(2100.0, 0.0, 0.0));
	Rig.Cloth->StepSimulation(CSClothTest::FrameDt);
	if (!TestTrue(TEXT("Particle readback after the jump"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	double WorstShift = 0.0;
	for (int32 P = 0; P < Particles.Num(); ++P)
	{
		const double Shift = FVector3f::Dist(FVector3f(Particles[P]), FVector3f(BeforeJump[P]) + FVector3f(2000.0f, 0.0f, 0.0f));
		WorstShift = FMath::Max(WorstShift, Shift);
	}
	// 跳完那一帧布照常受重力、继续摆，所以只要求「整块到了新位置附近」：任何粒子离刚体平移的目标不超过一帧的运动量。
	TestTrue(FString::Printf(TEXT("A teleport carries the whole cloth (worst offset %.2f cm)"), WorstShift), WorstShift < 10.0);

	// 重置：rest 形状挂在宿主现在的位置上，网格当场写回。
	TestTrue(TEXT("Reset"), Rig.Cloth->ResetToRestPose());
	FCSGpuMeshCPUData Mesh;
	if (!TestTrue(TEXT("Mesh readback after reset"), Rig.Mesh->ReadbackMeshSync(Mesh))) return false;
	double ResetError = 0.0;
	for (int32 V = 0; V < Mesh.Positions.Num(); ++V)
		ResetError = FMath::Max(ResetError, double(FVector3f::Dist(Mesh.Positions[V], Grid.Positions[V] + FVector3f(2100.0f, 0.0f, 0.0f))));
	TestTrue(FString::Printf(TEXT("Reset writes the rest pose at the host's current location (error %.4f cm)"), ResetError), ResetError < 0.01);
	return true;
}

// -----------------------------------------------------------------------------
// 规模：~33k 粒子挂 120 帧，数值稳定（计划的「编辑器目视 30k 粒子稳定」的无头半边）
//
// 判据是「稳」而不是「拉得住」：1.1 cm 的格距、181 个粒子长的竖链，默认 8 个 substep 本来就拉不到 P0 那条
// < 5% 的最大边长误差（那条由 HangingCloth 的 5 cm 布验收）。2026-10-26 的扫描：substep 8 / 16 / 32 →
// 平均误差 4.4% / 3.1% / 1.1%（tether 1.1），最坏的边在自由侧边上甩动的那一段。这里守住的是不炸、不停摆、
// 平均误差在 tether 松弛量量级、没有被扯成两倍长的边。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothScaleTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.LargeClothStable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothScaleTest::RunTest(const FString& Parameters)
{
	// 2 m × 2 m、180² 格（1.1 cm 一格）= 32761 个粒子，双面。
	FCSGpuMeshCPUData Grid;
	CSClothSetup::BuildGridSnapshot(FVector2D(200.0, 200.0), FIntPoint(180, 180), FTransform(FVector(0.0, 0.0, 300.0)), true, Grid);
	const FBox PinEdge(FVector(-100.5, -101.0, 299.0), FVector(-99.5, 101.0, 301.0));

	CSClothTest::FRig Rig;
	if (!CSClothTest::MakeRig(*this, Grid, [&PinEdge](UCSClothSimComponent& Cloth) { CSClothTest::AddPinBoxes(Cloth, { PinEdge }); }, Rig, true)) return false;
	TestEqual(TEXT("~33k particles"), Rig.Cloth->GetParticleCount(), 181 * 181);

	// 计时到粒子回读完成：回读等 GPU 空闲，所以这是「CPU 录制 + GPU 执行」的上界，不是纯 GPU 时间。
	const double Start = FPlatformTime::Seconds();
	CSClothTest::Step(Rig, 120);
	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Rig.Cloth->ReadbackParticlesSync(Particles))) return false;
	const double Elapsed = FPlatformTime::Seconds() - Start;
	AddInfo(FString::Printf(TEXT("%d particles, %d constraints: 120 frames done in %.1f ms wall to GPU idle (%.2f ms/frame upper bound)"),
		Rig.Cloth->GetParticleCount(), Rig.Cloth->GetConstraintCount(), Elapsed * 1000.0, Elapsed * 1000.0 / 120.0));

	TestTrue(TEXT("No NaN / Inf"), CSClothTest::AllFinite(Particles));
	TestFalse(TEXT("Not halted"), Rig.Cloth->IsHalted());

	FCSClothSetupData Reference;
	if (!TestTrue(TEXT("CPU reference setup"), CSClothSetup::BuildFromSnapshot(Grid, CSClothTest::HostAtOriginOptions({ PinEdge }), Reference))) return false;
	double MeanError = 0.0, MaxError = 0.0;
	CSClothTest::StretchError(Reference, Particles, MeanError, MaxError);
	AddInfo(FString::Printf(TEXT("Stretch error: mean %.3f%%, max %.3f%%"), MeanError * 100.0, MaxError * 100.0));
	TestTrue(FString::Printf(TEXT("Mean stretch error within the tether slack's order (%.2f%%)"), MeanError * 100.0), MeanError < 0.05);
	TestTrue(FString::Printf(TEXT("No edge torn to double length (max %.2f%%)"), MaxError * 100.0), MaxError < 0.5);
	return true;
}

// -----------------------------------------------------------------------------
// ACSClothActor：放进关卡就有布（首次 tick 上传 + setup），网格布预设钉边生效
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothActorTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.ClothActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothActorTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSClothActor* Actor = World->SpawnActor<ACSClothActor>(FVector(0.0, 0.0, 200.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Cloth actor"), Actor)) return false;
	Actor->GridResolution = FIntPoint(16, 16);
	Actor->GridPin = ECSClothGridPin::CornersMinX;
	Actor->ClothComponent->bSimulate = false;
	Actor->RebuildCloth();

	if (!TestNotNull(TEXT("The actor built its mesh"), Actor->GetClothMesh())) return false;
	TestEqual(TEXT("Render component draws the cloth mesh"), Actor->RenderComponent->GetGpuMesh(), Actor->GetClothMesh());
	TestTrue(TEXT("Simulation ready"), Actor->ClothComponent->IsSimulationReady());
	TestEqual(TEXT("17² particles (the back side welds onto the front)"), Actor->ClothComponent->GetParticleCount(), 17 * 17);
	TestEqual(TEXT("Two corners pinned"), Actor->ClothComponent->GetSetupStats().NumPinned, 2);

	for (int32 Frame = 0; Frame < 30; ++Frame) Actor->ClothComponent->StepSimulation(CSClothTest::FrameDt);
	TArray<FVector4f> Particles;
	if (!TestTrue(TEXT("Particle readback"), Actor->ClothComponent->ReadbackParticlesSync(Particles))) return false;
	double LowestZ = TNumericLimits<double>::Max();
	for (const FVector4f& P : Particles) LowestZ = FMath::Min(LowestZ, double(P.Z));
	TestTrue(TEXT("Hanging from two corners, the cloth sags"), LowestZ < 190.0);
	TestTrue(TEXT("No NaN / Inf"), CSClothTest::AllFinite(Particles));
	return true;
}

// -----------------------------------------------------------------------------
// 出图：挂在杆上的帘子 + 盖在球上的桌布，真渲染一帧（受光一张、世界法线一张），存到 Saved/ClothSimShots/。
//
// 断言只管「渲染路径真的画出了布」——同一机位、布在与不在各拍一张，逐像素比；形状、褶皱与法线对不对
// 靠人看图。这是计划 P0「编辑器目视」在无头环境里的替身，不替代在编辑器里拖着看。
// -----------------------------------------------------------------------------

namespace CSClothTest
{
constexpr int32 ShotWidth = 1280;
constexpr int32 ShotHeight = 720;

/** 摆一个引擎基本体，让它的**包围盒中心**落在 Center（不赌各个基本体的轴心在哪）。 */
AStaticMeshActor* SpawnProp(UWorld* World, const TCHAR* MeshPath, const FRotator& Rotation, const FVector& Center, const FVector& Scale)
{
	UStaticMesh* Mesh = LoadObject<UStaticMesh>(nullptr, MeshPath);
	if (!Mesh) return nullptr;
	const FVector PivotOffset = Rotation.RotateVector(Mesh->GetBoundingBox().GetCenter() * Scale);
	AStaticMeshActor* Prop = World->SpawnActor<AStaticMeshActor>(Center - PivotOffset, Rotation);
	Prop->SetActorScale3D(Scale);
	Prop->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
	Prop->GetStaticMeshComponent()->SetStaticMesh(Mesh);
	return Prop;
}

/** 拍一张，读回 8 bit 颜色。法线图（SCS_Normal）按 n·0.5 + 0.5 映射。 */
TArray<FColor> Capture(UWorld* World, const FVector& Eye, const FVector& Target, ESceneCaptureSource Source)
{
	const bool bNormals = Source == ESceneCaptureSource::SCS_Normal;
	UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
	RenderTarget->RenderTargetFormat = bNormals ? RTF_RGBA16f : RTF_RGBA8;
	RenderTarget->ClearColor = FLinearColor::Black;
	RenderTarget->InitAutoFormat(ShotWidth, ShotHeight);
	RenderTarget->UpdateResourceImmediate(true);

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	ASceneCapture2D* Camera = World->SpawnActor<ASceneCapture2D>(Eye, (Target - Eye).Rotation(), SpawnParameters);
	USceneCaptureComponent2D* Component = Camera->GetCaptureComponent2D();
	Component->TextureTarget = RenderTarget;
	Component->CaptureSource = Source;
	Component->FOVAngle = 55.0f;
	Component->bCaptureEveryFrame = false;
	Component->bCaptureOnMovement = false;
	// 离屏捕获没有 ViewState，自动曝光不收敛：钉成手动曝光，画面亮度只由灯决定。
	Component->PostProcessSettings.bOverride_AutoExposureMethod = true;
	Component->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
	Component->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	Component->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
	Component->PostProcessSettings.bOverride_AutoExposureBias = true;
	Component->PostProcessSettings.AutoExposureBias = 5.5f;

	World->SendAllEndOfFrameUpdates();
	Component->CaptureScene();
	Component->CaptureScene();
	FlushRenderingCommands();

	TArray<FColor> Pixels;
	FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
	if (bNormals)
	{
		TArray<FLinearColor> Linear;
		Resource->ReadLinearColorPixels(Linear);
		Pixels.Reserve(Linear.Num());
		for (const FLinearColor& N : Linear)
		{
			Pixels.Add(FLinearColor(N.R * 0.5f + 0.5f, N.G * 0.5f + 0.5f, N.B * 0.5f + 0.5f, 1.0f).ToFColor(false));
		}
	}
	else
	{
		Resource->ReadPixels(Pixels);
		for (FColor& Pixel : Pixels) Pixel.A = 255;
	}
	Camera->Destroy();
	return Pixels;
}

bool SaveShot(const TArray<FColor>& Pixels, const FString& Name)
{
	if (Pixels.Num() != ShotWidth * ShotHeight) return false;
	const FString Path = FPaths::ProjectSavedDir() / TEXT("ClothSimShots") / Name;
	return FImageUtils::SaveImageByExtension(*Path, FImageView(Pixels.GetData(), ShotWidth, ShotHeight));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSClothVisualCaptureTest,
	"PCGPlugins.ComputeShaderGenerator.ClothSim.VisualCapture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSClothVisualCaptureTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 600.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Sun"), Sun)) return false;
	Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
	Sun->GetLightComponent()->SetIntensity(10.0f);
	// 生成之后再定朝向：ADirectionalLight 模板的根组件自带 −90° 俯仰，SpawnActor 会把它与生成旋转相乘，
	// 直接在 SpawnActor 里给的角度最后几乎是正下方（实测前向 (−0.06, −0.04, −1)），竖着的帘子一点光都接不到。
	Sun->SetActorRotation(FRotator(-35.0, 200.0, 0.0));

	CSClothTest::SpawnProp(World, TEXT("/Engine/BasicShapes/Plane.Plane"), FRotator::ZeroRotator, FVector::ZeroVector, FVector(12.0));
	// 球 r = 50（BasicShapes 的球直径 100 cm），碰撞体同心同半径，布面浮 1 cm（CollisionThickness）。
	const FVector SphereCenter(0.0, 110.0, 50.0);
	CSClothTest::SpawnProp(World, TEXT("/Engine/BasicShapes/Sphere.Sphere"), FRotator::ZeroRotator, SphereCenter, FVector(1.0));
	// 帘杆：沿 Y 的细圆柱，正好在帘子钉住的那条边上。
	CSClothTest::SpawnProp(World, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"), FRotator(0.0, 0.0, 90.0),
		FVector(-75.0, -100.0, 260.0), FVector(0.05, 0.05, 1.7));

	// 帘子：150 cm 见方、48² 格，−X 边钉在杆上，松开后整块摆下来挂成竖帘。
	ACSClothActor* Curtain = World->SpawnActor<ACSClothActor>(FVector(0.0, -100.0, 260.0), FRotator::ZeroRotator);
	Curtain->GridSize = FVector2D(150.0, 150.0);
	Curtain->GridResolution = FIntPoint(48, 48);
	Curtain->GridPin = ECSClothGridPin::EdgeMinX;
	Curtain->ClothComponent->bSimulate = false;
	Curtain->ClothComponent->Damping = 1.0f;   // 让它在拍照前停下来
	Curtain->RebuildCloth();

	// 桌布：160 cm 见方、不钉，落到球上、四角垂到地面。
	ACSClothActor* Tablecloth = World->SpawnActor<ACSClothActor>(FVector(0.0, 110.0, 130.0), FRotator::ZeroRotator);
	Tablecloth->GridSize = FVector2D(160.0, 160.0);
	Tablecloth->GridResolution = FIntPoint(48, 48);
	Tablecloth->GridPin = ECSClothGridPin::None;
	Tablecloth->ClothComponent->bSimulate = false;
	Tablecloth->ClothComponent->Damping = 1.0f;
	FCSClothCollider& Sphere = Tablecloth->ClothComponent->Colliders.AddDefaulted_GetRef();
	Sphere.Type = ECSClothColliderType::Sphere;
	Sphere.Center = SphereCenter;
	Sphere.Radius = 50.0f;
	FCSClothCollider& Ground = Tablecloth->ClothComponent->Colliders.AddDefaulted_GetRef();
	Ground.Type = ECSClothColliderType::Plane;
	Ground.Center = FVector::ZeroVector;
	Ground.Axis = FVector::UpVector;
	Tablecloth->RebuildCloth();

	if (!TestTrue(TEXT("Both cloths are simulating"), Curtain->ClothComponent->IsSimulationReady() && Tablecloth->ClothComponent->IsSimulationReady())) return false;
	for (int32 Frame = 0; Frame < 360; ++Frame)
	{
		Curtain->ClothComponent->StepSimulation(CSClothTest::FrameDt);
		Tablecloth->ClothComponent->StepSimulation(CSClothTest::FrameDt);
	}
	TestFalse(TEXT("Neither cloth halted"), Curtain->ClothComponent->IsHalted() || Tablecloth->ClothComponent->IsHalted());

	const FVector Eye(430.0, -20.0, 230.0);
	const FVector Target(-20.0, 0.0, 110.0);
	const TArray<FColor> Lit = CSClothTest::Capture(World, Eye, Target, ESceneCaptureSource::SCS_FinalColorLDR);
	const TArray<FColor> Normals = CSClothTest::Capture(World, Eye, Target, ESceneCaptureSource::SCS_Normal);
	TestTrue(TEXT("Lit shot saved"), CSClothTest::SaveShot(Lit, TEXT("cloth_lit.png")));
	TestTrue(TEXT("Normal shot saved"), CSClothTest::SaveShot(Normals, TEXT("cloth_normals.png")));

	// 布藏起来再拍一张：差异像素就是布画出来的那部分。
	Curtain->RenderComponent->SetVisibility(false);
	Tablecloth->RenderComponent->SetVisibility(false);
	const TArray<FColor> Without = CSClothTest::Capture(World, Eye, Target, ESceneCaptureSource::SCS_FinalColorLDR);
	int32 Changed = 0;
	for (int32 Index = 0; Index < FMath::Min(Lit.Num(), Without.Num()); ++Index)
	{
		const FColor A = Lit[Index], B = Without[Index];
		Changed += (FMath::Abs(int32(A.R) - B.R) + FMath::Abs(int32(A.G) - B.G) + FMath::Abs(int32(A.B) - B.B)) > 24 ? 1 : 0;
	}
	const double Coverage = double(Changed) / double(CSClothTest::ShotWidth * CSClothTest::ShotHeight);
	AddInfo(FString::Printf(TEXT("Cloth covers %.1f%% of the shot; images in %s"), Coverage * 100.0, *(FPaths::ProjectSavedDir() / TEXT("ClothSimShots"))));
	TestTrue(FString::Printf(TEXT("The cloth is drawn (%.1f%% of the pixels change without it)"), Coverage * 100.0), Coverage > 0.05);
	return true;
}

#endif

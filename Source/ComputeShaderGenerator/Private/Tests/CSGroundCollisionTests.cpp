#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGroundActor.h"
#include "CSGroundCollisionComponent.h"
#include "CSGroundShaperActor.h"
#include "Components/ShapeComponent.h"
#include "Async/TaskGraphInterfaces.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "Tests/AutomationEditorCommon.h"
#include "WorldCollision.h"

// -----------------------------------------------------------------------------
// 地面的物理碰撞（`UCSGroundCollisionComponent`）
//
// 编辑器拖放 / 拖动预览 / End 贴地全走 `LineTraceMultiByObjectType(AllObjects)`，地面的 gpumesh
// 不带碰撞时物体会落到相机前方的默认距离上。这里用**同一种查询**打，并钉住放置射线那两道过滤
// 能放行的条件：命中的不是形状组件、且没有场景代理（有代理就得在视图里可见）。
// 碰撞是**异步 cook** 的：每次触发重烘之后先等它落地（完成回调排在游戏线程上）再打射线。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundPlacementTraceTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.PlacementTraceHitsGround",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundPlacementTraceTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 默认 64×64 格 × 50 cm：从原点铺到 (3200, 3200)。
	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;   // 本用例只看高度场
	UCSGroundCollisionComponent* Collision = Ground->FindComponentByClass<UCSGroundCollisionComponent>();
	if (!TestNotNull(TEXT("地面带碰撞组件"), Collision)) return false;
	auto WaitCook = [Collision]()
	{
		for (int32 Tick = 0; Tick < 1000 && Collision->IsCookInFlight(); ++Tick)
		{
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			FPlatformProcess::Sleep(0.005f);
		}
		return !Collision->IsCookInFlight();
	};
	Ground->RebuildGroundMesh();
	TestTrue(TEXT("重烘确实走了异步（发起后还在途）或已当场落地"), Collision->IsCookInFlight() || Collision->GetBodySetup() != nullptr);
	if (!TestTrue(TEXT("首次碰撞 cook 已落地"), WaitCook())) return false;

	// 与编辑器放置射线同一种查询：按对象类型、全部类型、打复杂碰撞。
	auto TraceDown = [World](const FVector2D& XY, FHitResult& OutHit)
	{
		FCollisionQueryParams Params(SCENE_QUERY_STAT(CSGroundPlacementTraceTest), /*bTraceComplex*/ true);
		return World->LineTraceSingleByObjectType(OutHit, FVector(XY, 5000.0), FVector(XY, -5000.0),
			FCollisionObjectQueryParams(FCollisionObjectQueryParams::InitType::AllObjects), Params);
	};

	// ① 平地：打得到，落点就是镜像高度，命中的是地面自己。
	const FVector2D Center(1600.0, 1600.0);
	FHitResult Hit;
	if (!TestTrue(TEXT("放置射线打得到地面"), TraceDown(Center, Hit))) return false;
	TestEqual(TEXT("落点 = 镜像高度"), Hit.ImpactPoint.Z, double(Ground->SampleHeight(Center)), 1.0);
	TestTrue(TEXT("命中的是地面 actor"), Hit.GetActor() == Ground);

	// ② 放置射线的两道过滤都放行：不是形状组件（游戏线程那道）、没有场景代理（渲染线程那道只管有代理的）。
	const UPrimitiveComponent* HitComponent = Hit.GetComponent();
	if (!TestNotNull(TEXT("命中组件"), HitComponent)) return false;
	TestFalse(TEXT("命中组件不是形状组件（否则放置射线在游戏线程就把它滤了）"), HitComponent->IsA<UShapeComponent>());
	TestNull(TEXT("命中组件没有场景代理（有代理就要求视图里可见）"), HitComponent->SceneProxy);

	// ③ 地面范围外：没东西就是没东西。
	FHitResult Outside;
	TestFalse(TEXT("地面范围外打不到"), TraceDown(FVector2D(-1000.0, -1000.0), Outside));

	// ④ 塑形物松手（提交）抬高地形：碰撞跟着重烘。中心点正落在镜像格点上，三角插值与双线性一致。
	ACSGroundShaperActor* Mound = World->SpawnActor<ACSGroundShaperActor>(FVector(Center, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("塑形物"), Mound)) return false;
	Mound->MaskSize = 2.0f * float(300.0f);   // 圆盘直径（2026-09-29 起形状是 mask，没设 mask = 解析圆盘）
	Mound->FalloffDistance = 200.0f;
	// 台顶 = actor 高度（2026-09-29）；乘 (1 + 二次抬升) 保持与旧口径「抬 LiftHeight、台顶再照抬 2%」逐项相同的剖面。
	Mound->SetActorLocation(Mound->GetActorLocation() + FVector(0.0, 0.0, double(250.0f) * (1.0 + double(Mound->SecondaryLiftScale))));
	Mound->RebuildTerrain(/*bCommitted*/ true);
	if (!TestTrue(TEXT("抬高后的碰撞 cook 已落地"), WaitCook())) return false;
	const double Raised = double(Ground->SampleHeight(Center));
	TestTrue(*FString::Printf(TEXT("塑形物真的抬高了地形（%.1f cm），否则下一条是空对空"), Raised), Raised > 100.0);
	FHitResult OnMound;
	if (!TestTrue(TEXT("抬高后仍打得到"), TraceDown(Center, OnMound))) return false;
	TestEqual(TEXT("落点跟着抬高"), OnMound.ImpactPoint.Z, Raised, 1.0);

	// ⑤ 整块地面平移：碰撞按组件局部坐标烘，跟着 attach 走，不必重烘。
	Ground->SetActorLocation(FVector(1000.0, 0.0, 0.0));
	FHitResult Moved;
	TestTrue(TEXT("平移后在新位置打得到"), TraceDown(FVector2D(1000.0 + 3100.0, 1600.0), Moved));
	TestFalse(TEXT("平移后旧的左缘外侧打不到"), TraceDown(FVector2D(500.0, 1600.0), Moved));
	return true;
}

// -----------------------------------------------------------------------------
// 扩大地面不清掉已画的内容（09-21：演示地面扩到 512 m 时，关卡里画好的路不能跟着没了）
//
// 只改格数、格距不变 ⇒ 同一张网格从左下角往外长，重叠区逐顶点原样保留；格距变了才整片重置。
// 走编辑器改属性的同一条路（PostEditChangeProperty → RebuildGroundMesh → EnsureMirrorInitialized）。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGroundResizeKeepsContentTest,
	"PCGPlugins.ComputeShaderGenerator.Ground.ResizeKeepsPaintedContent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

bool FCSGroundResizeKeepsContentTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSGroundActor* Ground = World->SpawnActor<ACSGroundActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("Ground actor"), Ground)) return false;
	Ground->StairMesh = nullptr;
	Ground->RebuildGroundMesh();

	// 画一笔路（默认笔刷颜色 R=1 = 道路权重）。
	const FVector2D OnRoad(800.0, 1600.0);
	Ground->BeginPaintStroke();
	for (int32 Dab = 0; Dab <= 16; ++Dab)
	{
		const double X = FMath::Lerp(400.0, 1200.0, double(Dab) / 16.0);
		Ground->ApplyPaintStroke(FVector(X, 1600.0, Ground->SampleHeight(FVector2D(X, 1600.0))));
	}
	Ground->EndPaintStroke();
	const float Before = Ground->SampleRoadWeight(OnRoad);
	if (!TestTrue(*FString::Printf(TEXT("路真的画上了（权重 %.2f），否则下面是空对空"), Before), Before > 0.5f)) return false;

	auto ChangeProperty = [Ground](FName Name)
	{
		FPropertyChangedEvent Event(FindFProperty<FProperty>(ACSGroundActor::StaticClass(), Name));
		Ground->PostEditChangeProperty(Event);
	};

	// ① 64 → 256 格（32 m → 128 m），格距不变：路还在原地，新长出来的地方是底色。
	Ground->NumCellsX = 256;
	ChangeProperty(GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsX));
	Ground->NumCellsY = 256;
	ChangeProperty(GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsY));
	TestEqual(TEXT("地面真的变大了"), double(Ground->GetWorldRect2D().Max.X), 256.0 * 50.0, 0.01);
	TestEqual(TEXT("扩大后路还在"), Ground->SampleRoadWeight(OnRoad), Before, 0.01f);
	TestEqual(TEXT("新长出来的地方没有路"), Ground->SampleRoadWeight(FVector2D(10000.0, 10000.0)), 0.0f, 0.01f);

	// ② 格距变了：没有可靠的重采样语义，照旧整片重置。
	Ground->CellSize = 100.0f;
	ChangeProperty(GET_MEMBER_NAME_CHECKED(ACSGroundActor, CellSize));
	TestEqual(TEXT("改格距整片重置"), Ground->SampleRoadWeight(OnRoad), 0.0f, 0.01f);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

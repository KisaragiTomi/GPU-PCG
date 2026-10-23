#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSHouseResizeSelectionWatcher.h"

#include "CSHouseActor.h"
#include "CSHouseHandleActor.h"
#include "CSHouseResizeHandleActor.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Tests/AutomationEditorCommon.h"

// -----------------------------------------------------------------------------
// 拉尺寸模式的失选判定（D5 交互层，用户裁决 2026-09-05）
//
// 判据是**归属**：宿主房本身、或任何挂在它 attach 链下的 actor 被选中，就算仍在编辑。
// 这条谓词值得单测，因为它的两种写错法在画面上都很隐蔽 ——
//   · 写成"只认抓手"：调同一栋房上的窗标记时抓手会莫名消失；
//   · 写成"只要选中集非空就算在编辑"：选中别的房子后旧抓手赖着不走，两套抓手同时在场。
//
// 真正的选择事件时序（引擎换选中是"先清空、再选中"两步，中间那次带空集的广播必须跳过）
// 由 `FCSHouseResizeSelectionWatcher` 的下一 tick 延迟承担，不在本用例范围内。
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSHouseResizeWatcherSelectionTest,
	"PCGPlugins.PCGEditorProcess.House.ResizeWatcherSelection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FCSHouseResizeWatcherSelectionTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	ACSHouseActor* Other = World->SpawnActor<ACSHouseActor>(FVector(5000.0, 0.0, 0.0), FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House"), House)) return false;
	if (!TestNotNull(TEXT("Other house"), Other)) return false;

	House->EnterResizeMode();
	TArray<ACSHouseHandleActor*> Handles = House->GetResizeHandles();
	// N + 5：四个锥子 + 檐口框 + 房底框 + 屋顶框（2026-09-20）+ 脊两端的脊长箭头（2026-10-02）。
	if (!TestEqual(TEXT("The house is in resize mode with nine handles"), Handles.Num(), 9)) return false;

	// ⚠️ `SpawnActor<AActor>` 出来的 actor **没有根组件**，`AttachToActor` 会直接不生效，
	// 而没有根的 actor 恒不"attached to"任何东西 —— 拿它当反例会让断言靠错误的理由通过，
	// 拿它当正例（下面的孙级）则永远失败。所以这里的裸 actor 一律补一个根。
	auto SpawnBareActor = [World](const FVector& Where) -> AActor*
	{
		AActor* Actor = World->SpawnActor<AActor>(Where, FRotator::ZeroRotator);
		if (!Actor) return nullptr;
		USceneComponent* Root = NewObject<USceneComponent>(Actor, TEXT("Root"));
		Actor->SetRootComponent(Root);
		Root->RegisterComponent();
		return Actor;
	};

	// 一个既不是房子、也没挂在房下的旁观者（有根，所以"不算归属"是真判出来的）。
	AActor* Stranger = SpawnBareActor(FVector(100.0, 100.0, 0.0));
	if (!TestNotNull(TEXT("Stranger"), Stranger)) return false;

	auto Selection = [](std::initializer_list<const AActor*> Actors)
	{
		TSet<const AActor*> Set;
		for (const AActor* Actor : Actors) Set.Add(Actor);
		return Set;
	};

	// ① 什么都没选 ⇒ 编辑结束。点空白处就是这一条。
	TestFalse(TEXT("An empty selection ends the edit"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({})));

	// ② 选中宿主房本身 ⇒ 仍在编辑。刚点完 Enter Resize Mode 就是这个状态。
	TestTrue(TEXT("Selecting the host house keeps the edit alive"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ House })));

	// ③ 选中它的任一抓手 ⇒ 仍在编辑。**少了这条，用户点一下抓手准备拖，模式当场就退了。**
	// 六个都要验：四个锥子 + 檐口框 + 房底框。漏掉任一个框的话"点框就退出模式"会漏网。
	for (const ACSHouseHandleActor* Handle : Handles)
	{
		TestTrue(
			FString::Printf(TEXT("Selecting %s keeps the edit alive"), *Handle->GetName()),
			FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ Handle })));
	}

	// ④ 选中**另一栋房子** ⇒ 编辑结束（用户点名的那条）。那栋房有它自己的一套抓手。
	TestFalse(TEXT("Selecting another house ends the edit"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ Other })));

	// 另一栋房的抓手同样不算 —— 它挂在别人的 attach 链下。
	Other->EnterResizeMode();
	TArray<ACSHouseHandleActor*> OtherHandles = Other->GetResizeHandles();
	if (TestEqual(TEXT("The other house has handles too"), OtherHandles.Num(), 9))
	{
		TestFalse(TEXT("Another house's handle does not keep this edit alive"),
			FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ OtherHandles[0] })));
		// 对称：本房的抓手也不该让**那**栋房赖着。
		TestFalse(TEXT("This house's handle does not keep the other edit alive"),
			FCSHouseResizeSelectionWatcher::IsStillEditing(Other, Selection({ Handles[0] })));
	}

	// ⑤ 无关 actor ⇒ 编辑结束；但**只要选中集里同时有归属者就仍在编辑**（框选是常态）。
	TestFalse(TEXT("An unrelated actor ends the edit"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ Stranger })));
	TestTrue(TEXT("A multi-selection containing the house keeps the edit alive"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ Stranger, Other, House })));

	// ⑥ 归属走的是**整条 attach 链**，不只是直接父级：孙级也算。
	// 窗标记之类将来挂得更深的编辑设施靠这一条。
	AActor* Grandchild = SpawnBareActor(FVector::ZeroVector);
	if (TestNotNull(TEXT("Grandchild"), Grandchild))
	{
		Grandchild->AttachToActor(Handles[0], FAttachmentTransformRules::KeepWorldTransform);
		TestTrue(TEXT("An actor two levels under the house keeps the edit alive"),
			FCSHouseResizeSelectionWatcher::IsStillEditing(House, Selection({ Grandchild })));
	}

	// ⑦ 空宿主不许崩（房子在模式里被删掉后，排着的那次判定会拿到空指针）。
	TestFalse(TEXT("A null host is never still editing"),
		FCSHouseResizeSelectionWatcher::IsStillEditing(nullptr, Selection({ House })));

	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#pragma once

#include "CoreMinimal.h"
#include "CSHouseHandleActor.h"
#include "CSHouseResizeHandleActor.generated.h"

class ACSHouseActor;
class UStaticMeshComponent;

/**
 * 拉尺寸抓手（计划 D5 的**交互层**）：一个挂在房子下的临时 actor，选中它用编辑器原生
 * transform gizmo 拖，**拖动本身就是通知源**。
 *
 * 用户裁决：不做自定义 EdMode / HitProxy。抓手是真实的 actor，所以点选、框选、Ctrl+Z、
 * 多视口、VR 编辑这些全部白拿 —— 代价只是四个 `RF_Transient` actor。
 *
 * 它**不是** `ACSTinyGlade`：没有 `UCSMesh`、没有快照、不参与任何重求值。可见的只有一只
 * 编辑器示意箭头（`bIsEditorOnly`），游戏模式下不可见。
 *
 * ## 观感：TG 原版的推墙箭头
 *
 * 每个抓手画一只**躺平**、沿自己那面墙外法线指出去的箭头，箭头中心落在墙外皮再往外
 * `HandleOffset` 处 —— 一眼能看出"抓这个往外拉，这面墙就往外走"。
 *
 * 网格就是 TG 矩形房子拉尺寸时画的那一对（`rectangle/ui_modes/edit_dims.rs`）：本体 `flat_arrow`
 * + 外扩一圈、更薄的描边 `flat_arrow_outline`，由 `Scripts/TinyGladeMakeHandleArrows.py` 从 TG 源 json
 * 烘成 `TinyGladeAsset/Meshes/` 下的同名网格（证据 `Docs/TinyGlade/evidence/edit-signifier-arrows-20260922.txt`）。
 * 本体挂高亮自发光材质（`Scripts/TinyGladeMakeHandleMaterial.py`），描边挂它的压暗版。
 * 网格与材质都在 `InitializeHandle` 里**惰性加载**，缺了网格就退回引擎锥子（照样看得见、点得着），
 * 缺了材质就退成默认材质 —— 交互都不受影响。
 *
 * ⚠️ **调高度是另一个抓手**（`ACSHouseHeightHandleActor`，那个才是四根横条围成的"窗框"）。
 * 本类只管**水平**推拉，四个一组、一面墙一个。
 *
 * ## 本类自己的两条纪律（生命周期那几条在基类 `ACSHouseHandleActor`）
 *
 * ① **记账量法**（`LastConsumedWorld`）—— 不加它，墙的位移是鼠标位移的 **2 倍**。
 *    详见 `ConsumeDragToHost` 的头注释，那里有完整递推。
 *
 * ② **每次推拉后四根条子全部重摆**（含正在被拖的那一根）。
 *    ⚠️ 这一条 2026-09-06 **推翻了原先的"拖拽期不回写正在被拖的那个抓手"**：原规则是设计期的
 *    预判（怕与 gizmo 抢写），代价却是被拖的那根会**跑到光标前面去**。推导：gizmo 把条子移 δ ⇒
 *    墙走 δ、房心走 δ/2 ⇒ attach 又把条子带走 δ/2，于是条子净走 1.5δ 而框的规范位置只走
 *    `(0.5 + FrameScale/2)·δ = 1.1δ`。差值每次事件累积，画面上就是**框裂开**：三根跟着墙走，
 *    被拖的那根越跑越远。重摆之后 `LastConsumedWorld` 恰好等于规范位置，下一次的差仍是纯 δ，
 *    "拖 1 m 墙走 1 m"不受影响（`House.EdgePush` / `House.ResizeHandle` 两处单测钉着）。
 *
 * 宿主弱引用、换宿主纪律、`Destroyed`/`EndPlay` 双解绑、spawn 后那次假松手的降级、
 * 无宿主自毁、以及"唯一执行面"这条分工，全部继承自 `ACSHouseHandleActor`。
 */
UCLASS(NotBlueprintable, NotPlaceable)
class COMPUTESHADERGENERATOR_API ACSHouseResizeHandleActor : public ACSHouseHandleActor
{
	GENERATED_BODY()

public:
	ACSHouseResizeHandleActor();

	/**
	 * 认宿主 + 认边号 + 摆到规范位置。`EnterResizeMode` 生成之后立刻调，只调一次。
	 *
	 * ⚠️ attach 必须在这之前完成（`SnapToCanonical` 写的是世界位置，attach 会把它换算成
	 * 相对量；顺序反了抓手会在房子移动后原地不动）。
	 */
	void InitializeHandle(ACSHouseActor* InHost, int32 InEdgeIndex);

	/** footprint 的边号，与 `CSHouse_GetEdge` 同号（矩形上 0 南 1 东 2 北 3 西）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	int32 GetEdgeIndex() const { return EdgeIndex; }

	/** 所属墙的世界外法线（Z 恒 0，单位）。宿主失效时返回零向量。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	FVector GetOuterNormalWorld() const;

	/**
	 * 规范位置（世界）：所属墙外皮中心，沿外法线偏出 `HandleOffset`，高度取墙高的
	 * `HandleHeightFraction`。宿主失效时返回当前位置（原地不动比跳到原点安全）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	FVector ComputeCanonicalWorldLocation() const;

	/**
	 * 摆回规范位置并**重置记账量**。两者必须一起做：只摆位不重置，下一次 `PostEditMove`
	 * 会把"程序刚刚制造的位移"当成用户拖的，墙会自己跳一下。
	 */
	virtual void SnapToCanonical() override;

	/**
	 * **交互的唯一执行面**：把"我现在在这儿"翻成一次单边推拉。返回实际生效的位移 cm。
	 *
	 * ── 记账量法：为什么不能直接拿"当前位置 − 规范位置" ──────────────────────────
	 * 抓手是房子的**子级**。"对侧不动、中心随动"意味着 `PushEdge` 会把房子中心沿外法线
	 * 移动 `Applied/2`，attach 于是把抓手的世界位置也带走 `Applied/2`，而规范位置随被推的
	 * 那面墙移动了整个 `Applied`。设第 n 次事件开始时残差 `e_n = 位置 − 规范位置`、
	 * 本次 gizmo 增量 `δ`：
	 *
	 *     Offset  = e_n + δ                              # 按"当前位置投影到外法线"算
	 *     e_{n+1} = e_n + δ + Offset/2 − Offset = (e_n + δ)/2
	 *     稳态 e* = δ   ⇒   每次事件 Offset → 2δ         # 拖 1 m，墙走 2 m
	 *
	 * 改成"位置 − 上次消费过的位置"，递推立刻塌成 `Offset ≡ δ`：`PushEdge` 返回时 attach
	 * 已经把抓手拖到新位置了，此刻读一次 `GetActorLocation()` 存起来，下次的差就是纯 gizmo
	 * 增量。⚠️ **必须在 `PushEdge` 之后读，且必须读实际位置**而不是拿 `Offset` 做算术 ——
	 * 顶在 `MinFootprint` 下限上时 `Applied != Offset`，用请求值记账残差会一路累积，
	 * 松手瞬间房子跳一大截（`CSHouseResize.h` 的返回值契约、`House.EdgePush` 第 ③ 例）。
	 *
	 * `bFinished` 直通 `PushEdge`（收紧容量/包围盒余量）。**每次**（不只是松手）都会触发四根
	 * 条子统一回位，理由见类头纪律 ②。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Resize")
	float ConsumeDragToHost(bool bFinished);

	/** 箭头中心（也是 gizmo 所在处）离墙外皮多远 cm。纯观感量，不进任何判定。 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.0"))
	float HandleOffset = 80.0f;

	/** 箭头挂在墙高的百分之几处。0 = 墙脚，1 = 檐口。 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float HandleHeightFraction = 0.5f;

	/**
	 * TG 箭头相对原大的倍数：1 = TG 原尺寸（长 100 cm、宽 30 cm、厚 10 cm）。纯观感量。
	 * 下一次归位（任何一次推拉、`SnapResizeHandles`）生效。退回引擎锥子时不用它。
	 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.1"))
	float ArrowScale = 1.0f;

	// ⚠️ **刻意不 override `PostRegisterAllComponents`**（踩过）：它在 `SpawnActor` 期间就跑，
	// 早于 `EnterResizeMode` 调 `InitializeHandle` —— 在那里写"宿主为空就自毁"的兜底，
	// 抓手会在生出来的那一瞬间把自己删掉，四个抓手一个都留不下。宿主失效的兜底因此由基类
	// 统一放在最终裁决那一刻（`ACSHouseHandleActor::HandleDrag`），那时 `Host` 必然已赋过值。

protected:
	//~ ACSHouseHandleActor
	virtual bool OnHandleDrag(bool bFinished) override;
	virtual void OnDetachFromHost() override;

private:
	UPROPERTY(Transient)
	int32 EdgeIndex = 0;

	/**
	 * 记账量：上一次已经被 `PushEdge` 消费掉的世界位置。
	 * transient 且不进任何哈希 —— 它是一次拖拽内部的状态，跨会话没有意义。
	 */
	UPROPERTY(Transient)
	FVector LastConsumedWorld = FVector::ZeroVector;

	/** 本次 `ConsumeDragToHost` 实际生效的位移，供公开 API 返回。 */
	float LastAppliedOffset = 0.0f;

	/**
	 * 按外法线（局部，Z 恒 0）摆箭头：朝向、缩放、以及让包围盒中心落在 actor 原点上。
	 * 描边挂在本体下面，跟着走。`SnapToCanonical` 里调。
	 */
	void UpdateArrowPose(const FVector& OuterLocal);

	/**
	 * 编辑器示意箭头的本体，沿外法线指向房外。游戏里不存在。
	 * TG 箭头（`bTGArrow`）头指网格局部 +Y、躺平；退回的引擎锥子尖指局部 +Z。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ArrowComponent;

	/** TG 箭头的描边，挂在本体下（同一套网格局部坐标，零相对变换）。退回锥子时没有网格。 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ArrowOutlineComponent;

	/** 本体装上的是 TG 箭头（而不是兜底锥子）。决定 `UpdateArrowPose` 用哪条轴当"指向"。 */
	bool bTGArrow = false;
};

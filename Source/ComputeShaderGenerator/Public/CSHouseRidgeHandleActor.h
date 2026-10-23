#pragma once

#include "CoreMinimal.h"
#include "CSHouseHandleActor.h"
#include "CSHouseRidgeHandleActor.generated.h"

class ACSHouseActor;
class UStaticMeshComponent;

/**
 * 调**脊长**的抓手（2026-10-02）：屋脊两端各一只躺平的箭头，顺着脊拖它，脊就伸缩。
 *
 * 对位物是 TG 的 `system_roof::ui_edit_roof_ridge_length`（证据 `Docs/TinyGlade/RE_RoofRidgeGable.md`）：
 * 玩家抓脊端沿脊向拖，新脊长过 `inverse_ridge_01_param` 折回 `ridge_length_01` 并钳在 [0, 1]。
 * 拖到与房子一样长（参数恰好钳成 1）⇒ `Roof::is_gable`：四坡变双坡，两端的墙升成三角形山墙；
 * 往回拖又是四坡。这里一个字都不重写那套换算 —— 只把位移喂给 `ACSHouseActor::PushRidgeLength`。
 *
 * 脊恒居中（TG 的脊线段以矩形中心为中点），所以**两只箭头改的是同一个量**：任一只往外拖 δ，
 * 脊的两头各伸 δ、脊长加 2δ，两只箭头一起走。做成两只而不是一只，是因为脊有两个端点，
 * 用户从哪一头看房子就抓哪一头（TG 也是两头都能抓）。
 *
 * 与另外两族抓手的分工：推墙箭头管水平（footprint）、高度框管竖直（墙高 / 房底 / 屋顶起伏），
 * 这两只管屋顶的**形状**。记账量法、宿主纪律、每次推完统一重摆，全部与 `ACSHouseResizeHandleActor` 相同
 * （那边的类注释有完整推导，这里不复述）；`PushRidgeLength` 不动 actor 变换，所以没有父子回路。
 *
 * 脊长只对矩形 footprint 的坡屋顶是参数：异形房子与露台上箭头藏起来，拖它什么都不发生。
 */
UCLASS(NotBlueprintable, NotPlaceable)
class COMPUTESHADERGENERATOR_API ACSHouseRidgeHandleActor : public ACSHouseHandleActor
{
	GENERATED_BODY()

public:
	ACSHouseRidgeHandleActor();

	/**
	 * 认宿主 + 认哪一头 + 摆到规范位置。`EnterResizeMode` 生成之后立刻调，只调一次。
	 * ⚠️ attach 必须在这之前完成（`SnapToCanonical` 写的是世界位置）。
	 */
	void InitializeHandle(ACSHouseActor* InHost, int32 InEnd);

	/** 脊的哪一头：0 = 逆着脊向的那头，1 = 顺着脊向的那头（脊向 = `FCSHouseRidgeFrame::Direction`）。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	int32 GetEnd() const { return End; }

	/** 这只箭头的世界朝向 = 从脊中点指向本端（Z 恒 0，单位）。宿主失效或脊长不是参数时返回零向量。 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	FVector GetOutwardWorld() const;

	/**
	 * 规范位置（世界）：本端脊端点沿脊向再往外 `HandleOffset`，高度在屋脊之上 `HandleLift`。
	 * 宿主失效或脊长不是参数时返回当前位置。
	 */
	UFUNCTION(BlueprintPure, Category = "CS House|Resize")
	FVector ComputeCanonicalWorldLocation() const;

	/** 摆回规范位置、摆正箭头、重置记账量，并按"脊长此刻是不是参数"显隐。 */
	virtual void SnapToCanonical() override;

	/**
	 * **交互的唯一执行面**：把沿脊向的位移 δ 翻成一次 `PushRidgeLength(2δ)`。返回实际生效的**脊长**变化 cm
	 * （= 本端实际走的距离的两倍）。垂直于脊的分量忽略，在回位时清掉。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS House|Resize")
	float ConsumeDragToHost(bool bFinished);

	/**
	 * 箭头中心（也是 gizmo 所在处）离脊端点多远 cm（沿脊向往外）。纯观感量。
	 * 默认 80：TG 把箭头的尾放在端点外 0.2 m（1421A9FD1）、箭头画成 2 倍（长 1.15 m），中心落在 0.8 m 处；
	 * TG 的可抓范围是端点外 0 ~ 1.5 m 的一个盒子（`create_ui_widget` 1421AA44D..1421AA598），中心 0.75 m。
	 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.0"))
	float HandleOffset = 80.0f;

	/**
	 * 箭头浮在屋脊线之上多高 cm。纯观感量。TG 的箭头就在脊端点的高度上（它的脊是放样面的上口）；
	 * 这里的屋脊线是瓦的中面，脊瓦还要再高出一截，抬 25 cm 才不埋进脊瓦里。
	 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.0"))
	float HandleLift = 25.0f;

	/** TG 箭头相对原大的倍数。默认 2 = TG 画它的倍数（`scale × 2`，1421A9FA5；悬停时 TG 再乘 1.2，这里不做）。纯观感量。 */
	UPROPERTY(EditAnywhere, Category = "CS House|Resize", meta = (ClampMin = "0.1"))
	float ArrowScale = 2.0f;

protected:
	//~ ACSHouseHandleActor
	virtual bool OnHandleDrag(bool bFinished) override;
	virtual void OnDetachFromHost() override;

private:
	/** 按局部朝向摆箭头（躺平、指向 `OutwardLocal`、包围盒中心落在 actor 原点）。 */
	void UpdateArrowPose(const FVector& OutwardLocal);

	UPROPERTY(Transient)
	int32 End = 0;

	/** 记账量：上一次已经被 `PushRidgeLength` 消费掉的世界位置（同 `ACSHouseResizeHandleActor::LastConsumedWorld`）。 */
	UPROPERTY(Transient)
	FVector LastConsumedWorld = FVector::ZeroVector;

	/** 本次 `ConsumeDragToHost` 实际生效的脊长变化，供公开 API 返回。 */
	float LastAppliedOffset = 0.0f;

	/** 示意箭头本体（TG `brush_arrow_flat_single_foward`；缺资产退到 `flat_arrow`，再退到引擎锥子）。游戏里不存在。 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ArrowComponent;

	/** 描边：只有退到 `flat_arrow` 时才有网格（TG 没给脊长那只做描边）。 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> ArrowOutlineComponent;

	/** 本体装上的是 TG 箭头（而不是兜底锥子）。决定 `UpdateArrowPose` 用哪条轴当"指向"。 */
	bool bTGArrow = false;

	/** 退到 `flat_arrow` 时的长度折算（它比脊长那只长），让 `ArrowScale` 在两张网格上画出同样长的箭头。 */
	float FallbackScale = 1.0f;
};

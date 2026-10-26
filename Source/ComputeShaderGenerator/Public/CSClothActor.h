#pragma once

#include "CoreMinimal.h"
#include "CSClothSim.h"
#include "GameFramework/Actor.h"
#include "CSClothActor.generated.h"

class UCSClothSimComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
class UStaticMesh;

/** 程序化网格布的预设钉点（在 actor 局部空间里自动生成钉盒，网格尺寸改了跟着走）。 */
UENUM(BlueprintType)
enum class ECSClothGridPin : uint8
{
	None,
	/** −X 一整条边：毛巾搭在杆上、旗挂在旗杆上。 */
	EdgeMinX,
	/** −X 边的两个角：挂起来的横幅。 */
	CornersMinX,
	/** 四个角：蹦床 / 遮阳棚。 */
	FourCorners,
};

/**
 * 放进关卡就能看的 GPU 布料：源网格（或程序化网格布）→ UCSMesh → 渲染组件 + 布料组件。
 *
 * 放下去当场就有布（OnConstruction 里建），不等 tick；之后由布料组件在编辑器视口里逐帧模拟
 * （见 UCSClothSimComponent）。本 actor 只负责把几何接上：源相关的属性改了才重建。挪动 actor 不重建 ——
 * 钉点跟着 actor 走，布面靠惯性自己摆过来，这才是在编辑器里拖它时该看到的东西。
 *
 * ⚠️ 编辑器只在有视口开着 Realtime 时 tick actor；编辑器窗口退到后台时（偏好设置 Performance →
 * 「Use Less CPU when in Background」，默认开）所有视口的 Realtime 都被临时关掉，布会停在那一帧，回到前台接着摆。
 *
 * GPU 数据不进存档（UCSMesh 是 transient 的），所以关卡重开、进 PIE 都会重新上传一次（首次 tick / BeginPlay）。
 */
UCLASS(meta = (PrioritizeCategories = "CS Cloth"))
class COMPUTESHADERGENERATOR_API ACSClothActor : public AActor
{
	GENERATED_BODY()

public:
	ACSClothActor();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Cloth")
	TObjectPtr<UCSMeshRenderComponent> RenderComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Cloth")
	TObjectPtr<UCSClothSimComponent> ClothComponent;

	/** 源网格（LOD0，按 actor 当前变换烘进世界空间）。为空则用下面的程序化网格布。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source")
	TObjectPtr<UStaticMesh> SourceMesh;

	/** 为空时：有源网格用它的第 0 号材质，否则用引擎默认材质。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source")
	TObjectPtr<UMaterialInterface> Material;

	/** 程序化网格布的尺寸（cm，actor 局部 XY 平面，法线 +Z）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source", meta = (EditCondition = "SourceMesh == nullptr"))
	FVector2D GridSize = FVector2D(200.0, 200.0);

	/** 每边的格数。粒子数 = (X+1)(Y+1)；默认 64² ≈ 4k 粒子，256² ≈ 66k。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source", meta = (EditCondition = "SourceMesh == nullptr", ClampMin = "1", ClampMax = "512"))
	FIntPoint GridResolution = FIntPoint(64, 64);

	/** 追加翻转的背面：引擎默认材质是单面的，没有背面的布从背后看是透明的。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source", meta = (EditCondition = "SourceMesh == nullptr"))
	bool bDoubleSided = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Source", meta = (EditCondition = "SourceMesh == nullptr"))
	ECSClothGridPin GridPin = ECSClothGridPin::EdgeMinX;

	/** 重新上传源几何并重建模拟（布回到 rest 姿态）。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Cloth")
	void RebuildCloth();

	/** 回到 rest 姿态，不重建。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Cloth")
	void ResetCloth();

#if WITH_EDITOR
	/** 把当前姿态冻成 StaticMesh 资产（烘回 actor 局部空间，落在关卡旁的 AutoResult 目录）。 */
	UFUNCTION(CallInEditor, Category = "CS Cloth")
	void BakeCurrentPoseToStaticMesh();

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** 撤销可能改回了源相关的属性，而撤销不走 PostEditChangeProperty：一律下一帧重建。 */
	virtual void PostEditUndo() override;
#endif

	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	UCSMesh* GetClothMesh() const { return ClothMesh; }

	//~ AActor interface
	/** 放下去（以及改了源属性之后）当场建布：编辑器 world 只在有 Realtime 视口时 tick actor，只靠 Tick 建的话，
	 *  编辑器在后台时放下去的那一刻是空的。拖动时它会被连着调，所以只在还没建过或 bRebuildPending 时才建。 */
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }

private:
	/** 没有 GPU 的场合（cook / commandlet / -NullRHI）一个 compute pass 都不能录：本插件的全局 shader 在那里不存在，录了就断言。 */
	bool CanBuildCloth() const;

	/** 网格布预设钉点对应的钉盒（actor 局部空间）。 */
	void BuildGridPinBoxes(TArray<FCSClothPinBox>& OutPinBoxes) const;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> ClothMesh;

	bool bRebuildPending = true;
};

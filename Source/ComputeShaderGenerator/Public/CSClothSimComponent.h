#pragma once

#include "CoreMinimal.h"
#include "CSClothSim.h"
#include "Components/ActorComponent.h"
#include "CSClothSimComponent.generated.h"

class UCSMesh;
struct FCSClothGpuState;

/**
 * GPU 布料的宿主（计划 D2）：一个固定拓扑的逐帧形变器，不是通用物理引擎。
 *
 * 绑一个 UCSMesh（SetTargetMesh），SetupFromMeshSync 一次性回读 → 焊接 → 建约束 → 上传；之后每帧
 * 游戏线程收集 dt / 钉组变换 / 碰撞体 / 风 → 一条渲染命令 → 渲染线程自建一张图跑完全部 substep →
 * 图尾借 FCSMeshRenderThreadEdit 把位置与切线基写回网格的常驻流。运行循环零回读、零阻塞。
 *
 * 画面由绑同一个 UCSMesh 的 UCSMeshRenderComponent 负责：每帧只有 buffer 内容在变、身份不变，
 * 代理零重建；UCSMeshOps::CopyToStaticMesh 读的就是被模拟的流，随时能把当前姿态冻成资产。
 *
 * 失配停摆：网格在 setup 之后被别人编辑过（Generation 动了）或重新分配过（AllocationGeneration 动了），
 * 约束就不再描述这份几何 —— 停摆并写日志，等下一次 SetupFromMeshSync。写回时撞见非有限粒子同样停摆。
 *
 * 用法见 ACSClothActor（CSClothActor.h）：它把源网格 / 程序化网格布、渲染组件和本组件接在一起。
 */
UCLASS(ClassGroup = Physics, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSClothSimComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UCSClothSimComponent();

	// -------------------------------------------------------------------------
	// 运行
	// -------------------------------------------------------------------------

	/** 每帧自动步进。关掉后只有 StepSimulation 会推进。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver")
	bool bSimulate = true;

	/** 编辑器视口里（不进 PIE）也跑。视口不是实时模式时编辑器 world 不 tick，布料就停在那一帧。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver")
	bool bSimulateInEditor = true;

	/** small-steps XPBD 的 substep 数：比加迭代更划算的收敛手段。8 够 3 cm 以上的格距；格距 1~2 cm 的细布
	 *  要 16~32 才拉得住（2026-10-26 实测 181² 悬挂布：8 / 16 / 32 → 平均边长误差 4.4% / 3.1% / 1.1%）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", meta = (ClampMin = "1", ClampMax = "32", UIMax = "16"))
	int32 Substeps = 8;

	/** 每个 substep 的约束迭代数。>1 时 XPBD 的 λ 跨迭代累积。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", meta = (ClampMin = "1", ClampMax = "8"))
	int32 Iterations = 1;

	/** 一帧最多模拟多少秒。卡顿帧不会让布料一步跨太远而炸开，代价是卡顿时布料变慢。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", meta = (ClampMin = "0.001", ClampMax = "0.1", Units = "s"))
	float MaxDeltaTime = 1.0f / 30.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float TimeScale = 1.0f;

	/** 单条约束单次修正、每个端点的位移上限（cm）：炸开时的保险丝。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", AdvancedDisplay, meta = (ClampMin = "0.1", ClampMax = "100.0", Units = "cm"))
	float MaxCorrection = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", AdvancedDisplay, meta = (ClampMin = "1.0", Units = "CentimetersPerSecond"))
	float MaxSpeed = 5000.0f;

	/** 钉组（宿主或 AttachActor）一帧移动超过这个距离按瞬移处理：整块布刚体搬过去，不被拽着甩。0 = 从不。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Solver", AdvancedDisplay, meta = (ClampMin = "0.0", Units = "cm"))
	float TeleportDistance = 500.0f;

	// -------------------------------------------------------------------------
	// 材质（柔度 = 1 / 刚度，单位 s²/kg，即 1/(N/m)；0 = 尽可能不可伸长）
	// -------------------------------------------------------------------------

	/** 面密度 kg/m²。改它要重新 setup（质量在 setup 时算好上传）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (ClampMin = "0.001", ClampMax = "10.0"))
	float ArealDensity = 0.2f;

	/** 边被拉长时的柔度。布几乎不可伸长，默认 0。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (ClampMin = "0.0", UIMax = "0.1"))
	float StretchCompliance = 0.0f;

	/** 边被压短时的柔度。比拉伸软得多，布受压起皱而不是像橡皮一样顶回去。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (ClampMin = "0.0", UIMax = "1.0"))
	float CompressionCompliance = 0.01f;

	/** 弯曲（跨边距离约束）的柔度：越大越软、越垂。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (ClampMin = "0.0", UIMax = "20.0"))
	float BendCompliance = 1.0f;

	/** 线性阻尼（1/s）：每秒速度衰减到 exp(-Damping)。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (ClampMin = "0.0", UIMax = "10.0"))
	float Damping = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (Units = "CentimetersPerSecondSquared"))
	FVector Gravity = FVector(0.0, 0.0, -980.0);

	/** 长程 tether：每个自由粒子拴到最近钉点，长度上限 = 测地距离 × TetherScale。抗下垂；改它要重新 setup。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material")
	bool bUseTethers = true;

	/** tether 的松弛量。细网格（格距 ≤ 2 cm）上它几乎就是整块布能被拉长的比例：2026-10-26 实测 181² 的悬挂布，
	 *  1.1 时平均边长误差 4.4%、1.02 时 2.9%；1.0 反而更差（tether 与边长约束顶牛）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", meta = (EditCondition = "bUseTethers", ClampMin = "1.0", ClampMax = "2.0"))
	float TetherScale = 1.02f;

	/** 焊接容差（cm）：相距在它以内的渲染顶点并成一个粒子。改它要重新 setup。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Material", AdvancedDisplay, meta = (ClampMin = "0.0001", Units = "cm"))
	float WeldTolerance = 0.01f;

	// -------------------------------------------------------------------------
	// 风（P1）
	// -------------------------------------------------------------------------

	/** 平均风速（世界空间 cm/s）。零风速且零湍流时整套风力不算。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (Units = "CentimetersPerSecond"))
	FVector WindVelocity = FVector::ZeroVector;

	/** 湍流幅度（cm/s）：Perlin 噪声叠在平均风速上。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (ClampMin = "0.0", Units = "CentimetersPerSecond"))
	float WindTurbulence = 0.0f;

	/** 湍流的空间尺度（cm）：噪声每这么远变化一个周期。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (ClampMin = "1.0", Units = "cm"))
	float WindTurbulenceScale = 200.0f;

	/** 湍流随时间变化的快慢（周期 / 秒）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (ClampMin = "0.0"))
	float WindTurbulenceSpeed = 0.5f;

	/** 相对风速沿布面法线的分量往风速收敛的速率（1/s）：布面兜风的强弱。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (ClampMin = "0.0"))
	float WindNormalDrag = 2.0f;

	/** 切向分量的收敛速率（1/s）：擦着风的那部分，远小于法向。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Wind", meta = (ClampMin = "0.0"))
	float WindTangentialDrag = 0.1f;

	// -------------------------------------------------------------------------
	// 钉点（钉盒是 P0，顶点色遮罩是 P1；改了都要重新 setup）
	// -------------------------------------------------------------------------

	/** 钉盒，写在宿主 actor 的局部空间里。见 FCSClothPinBox。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Pins")
	TArray<FCSClothPinBox> PinBoxes;

	/** 顶点色的哪个通道当钉点遮罩（≥ PinColorThreshold 即钉住，跟宿主走）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Pins")
	ECSClothPinColorChannel PinColorChannel = ECSClothPinColorChannel::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Pins", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float PinColorThreshold = 0.5f;

	// -------------------------------------------------------------------------
	// 碰撞（D4 的 analytic 层；运行期随时可改，每帧上传）
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Collision")
	TArray<FCSClothCollider> Colliders;

	/** 布面与碰撞体之间保持的厚度（cm）：小了会被渲染的深度精度咬穿。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Collision", meta = (ClampMin = "0.0", Units = "cm"))
	float CollisionThickness = 1.0f;

	/** 0 = 光滑，1 = 贴上就不滑。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth|Collision", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Friction = 0.2f;

	// -------------------------------------------------------------------------
	// Blueprint API
	// -------------------------------------------------------------------------

	/** 绑定要模拟的网格。换网格会停掉当前模拟，要重新 SetupFromMeshSync。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth")
	void SetTargetMesh(UCSMesh* InMesh);

	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	UCSMesh* GetTargetMesh() const { return TargetMesh; }

	/**
	 * setup（D5）：回读网格 → 焊接 → 建约束 → 上传，再把网格的包围盒放大到布料够得着的范围。
	 * 网格此刻的姿态就是 rest 姿态。阻塞（一次回读 + 一次同步编辑），别每帧调。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth")
	bool SetupFromMeshSync();

	/** 回到 rest 姿态（跟着宿主当前的变换摆）、速度清零，并当场写回网格。不重建约束，不阻塞。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth")
	bool ResetToRestPose();

	/** 宿主追加的钉盒（与 PinBoxes 同一语义，不进存档、不上面板）：ACSClothActor 的网格布预设钉边走这里。
	 *  下一次 SetupFromMeshSync 生效。 */
	void SetExtraPinBoxes(const TArray<FCSClothPinBox>& InPinBoxes);

	/** 手动推进一帧（bSimulate 关掉时由调用方驱动；测试也走这里）。不阻塞。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth")
	bool StepSimulation(float DeltaTime);

	/** 放掉 GPU 状态。网格保持最后一帧的姿态。 */
	UFUNCTION(BlueprintCallable, Category = "CS Cloth")
	void ReleaseSimulation();

	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	bool IsSimulationReady() const;

	/** 因网格失配或数值爆炸而停摆。SetupFromMeshSync 会清掉它。 */
	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	bool IsHalted() const { return bHalted; }

	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	int32 GetParticleCount() const { return SetupStats.NumParticles; }

	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	int32 GetConstraintCount() const { return SetupStats.NumStretch + SetupStats.NumBend + SetupStats.NumTether; }

	/** 自上次 setup 以来渲染线程上实际跑完的帧数。编辑器里它不涨，多半是没有一个视口开着 Realtime：
	 *  编辑器 world 那时按 LEVELTICK_TimeOnly 走，任何 actor / 组件都不 tick。 */
	UFUNCTION(BlueprintPure, Category = "CS Cloth")
	int32 GetSimulatedFrameCount() const;

	/** setup 产物的统计（粒子数、各类约束数、钉住数、总质量）。 */
	struct FSetupStats
	{
		int32 NumParticles = 0;
		int32 NumStretch = 0;
		int32 NumBend = 0;
		int32 NumTether = 0;
		int32 NumPinned = 0;
		double TotalMassKg = 0.0;
	};
	const FSetupStats& GetSetupStats() const { return SetupStats; }

	/** 粒子位置读回（世界空间，w = InvMass）。**测试 / 诊断专用**：阻塞。 */
	bool ReadbackParticlesSync(TArray<FVector4f>& OutWorldPositions) const;

	//~ UActorComponent interface
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	/** 只在销毁时放 GPU 状态，不在 OnUnregister：编辑器里改任何一个属性都会把组件注销再注册一遍
	 *  （UActorComponent::PreEditChange 的 FComponentReregisterContext），在那里放掉等于每拖一下滑条布就重来。 */
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
	virtual void BeginDestroy() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** 撤销可能改回了 setup 用掉的属性，而撤销不走 PostEditChangeProperty：摆回 rest 后重新 setup。 */
	virtual void PostEditUndo() override;
#endif

private:
	/** 一个钉组的跟随目标：组 0 是宿主 actor，其余是钉盒的 AttachActor。 */
	struct FPinGroup
	{
		TWeakObjectPtr<AActor> Actor;
		FTransform SetupTransform = FTransform::Identity;
		FTransform PrevTransform = FTransform::Identity;
	};

	bool BuildSetupOptions(FCSClothSetupOptions& OutOptions);
	void Halt(const FString& Reason);
	bool ValidateTargetMesh();

	/** 解析钉组的当前世界变换（actor 没了就停在上一次的位置）。 */
	FTransform ResolvePinGroupTransform(const FPinGroup& Group) const;

	/** 当前钉组变换下布料够得着的世界包围盒；网格发布的包围盒装不下它时重新发布（非阻塞）。 */
	FBox ComputeReachBounds(const TArray<FTransform>& GroupTransforms) const;
	void MaybePublishBounds(const FBox& ReachBounds);

	void AppendColliderShape(const FCSClothCollider& Collider, TArray<FVector4f>& OutShape) const;

	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> TargetMesh;

	TSharedPtr<FCSClothGpuState, ESPMode::ThreadSafe> GpuState;

	/** setup 时缓存的网格身份；对不上就停摆。 */
	uint32 ExpectedGeneration = 0;
	uint32 ExpectedAllocationGeneration = 0;

	/** sim 空间原点（世界坐标，double）。 */
	FVector SimOrigin = FVector::ZeroVector;

	TArray<FPinGroup> PinGroups;

	/** SetExtraPinBoxes 给的钉盒。 */
	TArray<FCSClothPinBox> ExtraPinBoxes;

	/** 每个钉组：钉住部分的 rest 包围盒（sim 空间）与可达半径；setup 产物的拷贝，包围盒更新用。 */
	TArray<FBox3f> PinGroupRestBounds;
	TArray<float> PinGroupReach;
	FBox3f UnanchoredRestBounds = FBox3f(ForceInit);
	FBox3f RestBoundsSim = FBox3f(ForceInit);

	/** 最近一次发布给网格的包围盒（世界）。 */
	FBox PublishedBounds = FBox(ForceInit);

	/** 上一帧碰撞体的 sim 空间形状（2 个 float4 / 个），插值的起点。数量变了就从本帧形状起步。 */
	TArray<FVector4f> PrevColliderShapes;

	/** setup 时的 CPU 数据里重置要用的那一份（rest 粒子）。 */
	TArray<FVector4f> RestPositionsCopy;

	FSetupStats SetupStats;
	double SimulatedTime = 0.0;
	bool bHalted = false;
};

#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperField.h"
#include "CSTinyGlade.h"
#include "CSGroundShaperActor.generated.h"

class ACSGroundActor;
class UBillboardComponent;
class UTexture2D;

/**
 * 地形塑形物：放在地面上的**不可见高度影响体**（计划 D9；高度不是笔刷画的）。
 *
 * **形状由一张 mask 决定**（用户 2026-09-29）：mask 白 = 台顶，按 `MaskSize` 铺在本 actor 的
 * 局部 XY 上（跟着 actor 的 yaw 转）；**台顶高度 = 本 actor 相对地面的高度** —— actor 放在 600，
 * 地形最高就是 600；放在 −300 就是一个坑，坑底 −300（2026-09-30）。没设 mask 时按直径 `MaskSize` 的解析圆盘走（与改 mask 之前的圆盘逐位相同）。
 *   · 高度场：mask 先被烘成有符号距离场（`CSGroundShaperField::BakeMaskSdf`，编辑器里改 mask /
 *     尺寸 / 平滑时烘一次，结果随关卡存盘，cook 后照用），mask 内恒为台高，mask 外沿距离按
 *     `FalloffDistance` smoothstep 羽化到 0，再叠上裙边噪声（只减不加的侵蚀）与二次抬升。
 *     公式的**唯一权威**是 `Public/CSGroundShaperField.h` <-> `Shaders/Private/CSGroundShaperField.ush` 那一对。
 *   · 圆滑（`Smoothing`）管两件事，照 TG 的手法：单张 mask 的尖角（五角星的凸尖与凹口）在
 *     距离场上做高斯（TG 是对高度图做 GaussBlur，我们做在距离域，场才能在任意点求值）；
 *     多座叠在一起时交汇处的折痕用 smooth-max 倒角（TG 用 max + 各自的噪声打碎折痕）。
 *
 * **石阶不在这里**（2026-08-30 裁决一，旧路已删）：塑形物曾经自持一条
 * 「CPU 解等高线 -> B 样条 -> 沿曲线铺块」的石阶路（`RebuildSteps` / `BuildStepPlan` /
 * `CSShaperSteps::EnsureCapacity` / `CSGroundSteps.usf`）。它已经整条删掉，石阶现在**只有一条**：
 * 地面自己的 `ACSGroundActor::RebuildStairs`（marching squares 等值线 + 定容 +
 * `InterlockedAdd`，见 `CSGroundStairs.usf`）。这里因此不再有任何 palette、实例组件或
 * GPU 缓冲 —— 塑形物只负责高度场。
 *
 * **产物全在 GPU 线程上生成**：地形位移是 `UCSMeshOps::DisplaceGroundShapers` 一个 compute
 * pass（区域更新，不重传整张地面）；CPU 只算一份打包参数，权威数据仍在地面镜像。
 *
 * **归属**（用户提问的裁决）：切割生成的岩石归本 actor 持有。生命周期是纯从属关系
 * （删掉塑形物 = 地形塌回 + 产物全消失，已经是原子的），没有接缝那种对称归属问题；
 * 独立 actor 只会多出必须与地面刷新配对的销毁路径。因此基类从 `AActor` 换成 `ACSTinyGlade`：
 * 基类的 `UCSMesh` 槽位留给将来切割出来的岩石（空实例零 GPU 分配、无 scene proxy，不花钱）。
 *
 * 通知（计划 D9 裁决）：塑形物**只与地面对话**（登记/注销/变更），由地面重导出 `Mirror.Heights`
 * 后广播。⚠️ 反向订阅 `OnGroundChanged` 也随旧路一起删了 —— 它当初只为"别人画路时本座重摆石阶"
 * 而存在，而石阶早已归地面自己管（地面在同一条 `FlushPaintToGpu` 尾巴上重扫）。
 *
 * 可视化：编辑器里仅用 billboard 图标示意，`bIsEditorOnly` + `HiddenInGame`；
 * （将来的）岩石才是真实产物。
 */
UCLASS(Blueprintable)
class COMPUTESHADERGENERATOR_API ACSGroundShaperActor : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSGroundShaperActor();

	// -------------------------------------------------------------------------
	// Shape
	//
	// 台顶高度**不是参数**：它就是本 actor 相对地面 actor 的 Z（没有地面时相对世界 0）。
	// 旧的 `LiftHeight`（"相对 actor 再抬多少"）已并进 actor 的 Z，老关卡加载时自动迁移（见 PostLoad）。
	// -------------------------------------------------------------------------

	/**
	 * 形状 mask（白 = 台顶，按 0.5 二值化；取 R 通道）。u 沿本座局部 +X，贴图第 0 行在局部 −Y 端。
	 * 留空 = 直径 `MaskSize` 的解析圆盘。改了贴图内容后点一次 `RebuildTerrain` 重烘。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper")
	TObjectPtr<UTexture2D> Mask;

	/**
	 * mask 铺在地面上的边长 cm（整张贴图的宽 = 本值）。圆盘模式下就是直径。
	 * 本值与下面的羽化 / 圆滑 / 中心隆起放在 ViewEdit 分类：视口浮窗里直接调（2026-09-30 取代蓝图构造脚本里
	 * 那个 `Set Radius` 变量 —— 它写的是已取消的 `Radius`，构造脚本已从 BP_GroundShaper 删掉）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ViewEdit", meta = (ClampMin = "1.0", UIMin = "100.0", UIMax = "4000.0"))
	float MaskSize = 300.0f;

	/** 羽化裙边宽 cm：mask 外这段距离内从台高降到 0，对应 deformbyinfluence 的 maxdist。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ViewEdit", meta = (ClampMin = "1.0", UIMin = "50.0", UIMax = "2000.0"))
	float FalloffDistance = 200.0f;

	/**
	 * 圆滑半径 cm（0 = 关）。一个数管两处尖锐：
	 *   · 本座 mask 的尖角 —— 五角星的凸尖被削圆、凹口被填圆，从凹口顺着裙边往下的那道折痕一起抹平
	 *     （距离场上的高斯，σ = 本值 / 3）。量级参考：想把尖角圆成半径 r 的圆角，取 2r~3r；
	 *   · 与别的塑形物交汇处的折痕 —— 合成时按本值做 smooth-max 倒角（两座高度差在本值以内的那一带
	 *     被往上填圆；交汇双方取较大的那个平滑值）。倒角被封顶在参与合成的最高台顶，不会冒出 actor 高度。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ViewEdit", meta = (ClampMin = "0.0", UIMax = "1000.0"))
	float Smoothing = 0.0f;

	/**
	 * 中心隆起（0 = 平顶，1 = 整座是一座顺着轮廓往中间鼓的山）。离轮廓越远越高，**最高点在 mask 内离边最远处，
	 * 恰好 = actor 高度**；星形会长出五条汇向中心的脊。最深距离取自平滑之后的距离场，所以 `Smoothing`
	 * 只会把脊、凹口这些较低的部分往下压，不会压低峰顶。坑（actor 在地面以下）同理：最深点 = actor 高度。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "ViewEdit", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CenterBulge = 0.0f;

	/**
	 * 距离场的目标格距 cm（边长上限 512 格，超了自动放大格距）。地面网格是 50 cm，10 cm 足够把
	 * 星形尖角那类细节交给平滑去处理；只有 mask 里真有比这更细的形状才需要调小。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, AdvancedDisplay, Category = "CS Shaper", meta = (ClampMin = "1.0"))
	float SdfCellSize = 10.0f;

	// -------------------------------------------------------------------------
	// 裙边噪声（原型 noisebysourcestress）与二次抬升（原型 attribwrangle5）
	// 公式与两条必须保住的性质写在 Shaders/Private/CSGroundShaperField.ush 里，不在这里重复。
	// -------------------------------------------------------------------------

	/**
	 * 裙边侵蚀幅度，**以台高为单位**（0 = 关掉整条噪声；原型的 turbnoise 幅度就活在这个
	 * 归一化域里，所以这里不写 cm —— 大土台自动得到成比例的侵蚀）。
	 *
	 * 默认 0.5 不是随手取的：它是"两座相接土台的折痕被打碎"这条验收（计划裁决六）能量到
	 * 的最小档。半径 300 / 羽化 400 / 台高 300、中心相距 1200 的两座实测：折痕线的横向游走
	 * 从 0.00 cm（噪声关时折痕是一条**精确**的中垂线）涨到 σ≈14 cm、峰值 46 cm，横向曲率的
	 * 变异系数从 0.11 涨到 0.46。0.35 只到 σ≈10 cm、变异系数 0.34，肉眼仍读得出一条直线。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0.0", ClampMax = "2.0"))
	float SkirtNoiseAmount = 0.5f;

	/**
	 * 噪声底层格的波长 cm。默认 300 与地面 `CellSize`（50）的比是 6 —— 再细地面网格就采不住了，
	 * 高度场上明明有的起伏在画面里会退化成锯齿（石阶读的是解析场，不受这条限制，会先于地面
	 * 显出细节，看着像"石阶和地面对不上"）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "1.0"))
	float SkirtNoiseWavelength = 300.0f;

	/** 噪声种子。噪声域是**本座局部坐标**，所以两座塑形物即使同种子也拿到不同实现；这条只是重掷用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0"))
	int32 SkirtNoiseSeed = 0;

	/**
	 * 二次抬升系数（原型 `attribwrangle5` 的 scale = 0.021）：`+ pow(S, 1.5) · 本值`，偏向台顶的
	 * 纯观感微调。整条剖面再除以 (1 + 本值) ⇒ **台顶恰好 = actor 高度**（2026-09-29 起；
	 * 此前是照原型"照抬 2%"，老关卡迁移时把 actor 抬到了旧台顶，画面不变）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper|Skirt Noise", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float SecondaryLiftScale = 0.021f;

	// -------------------------------------------------------------------------
	// Wiring
	// -------------------------------------------------------------------------

	/** 目标地面。留空则自动找场景里第一个 ACSGroundActor。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Shaper")
	TObjectPtr<ACSGroundActor> Ground;

	// -------------------------------------------------------------------------
	// Public API
	// -------------------------------------------------------------------------

	/** 全量：让地面按塑形物重导出高度（区域更新 + 广播）。参数改动/移动都走这条。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Shaper")
	/** `bCommitted = false` 只在拖动帧（`PostEditMove(false)`）：地面的广播随之标成未提交，房子只落座、不写接缝。 */
	void RebuildTerrain(bool bCommitted = true);

	/** 声明式入口（基类语义）：等价于 RebuildTerrain，幂等。 */
	virtual void ReevaluateSite() override;

	/**
	 * 本塑形物单独对某点的高度贡献（世界 XY → 相对地面基面的抬升，恒 ≥ 0）。
	 * 多座的合成（平滑 max）在地面那边做：`CSGroundShaperField::EvalField`。
	 */
	float SampleShapeHeight(const FVector2D& WorldXY) const;

	/**
	 * 高度场的打包头（4 个 float4，布局见 `CSGroundShaperField.h`）+ 配套的 SDF。
	 * compute pass 与 CPU 镜像**共用这一份打包结果**：`SampleShapeHeight` 也是拿它求值，
	 * 所以两侧连"参数是怎么算出来的"都不会分叉，只剩公式本身要人盯。
	 * `Noise.Z` 在这里已经是 SDF 边长（0 = 解析圆盘）；池起点 `Noise.W` 由 `PackField` 填。
	 */
	CSGroundShaperField::FShaperHeader GetFieldHeader() const;
	TConstArrayView<float> GetMaskSdf() const { return MaskSdf; }

	/** 距离场的内容指纹（mask / 尺寸 / 平滑 / 格距）：换了一张同尺寸的 mask 时头不变，靠它让派生链知道要重跑。 */
	uint32 GetMaskSdfKey() const { return MaskSdfKey; }

	/** 影响范围的世界 XY 矩形（= mask 零等值线的外接圆 + 裙边），也是区域重导出发布的脏区。 */
	FBox2D GetFootprintRect2D() const;

	/** 零等值线到中心的最远距离 cm（圆盘 = MaskSize / 2）。足迹、摆件圈、岩壳覆盖区检查共用。 */
	float GetContourRadius() const;

	/** 台顶相对地面基面的**有符号**高度（= 本 actor 的 Z − 地面 Z；负 = 坑，坑底就是它）。 */
	double ComputeTopHeight() const;

	//~ UObject / AActor interface（OnConstruction → ReevaluateSite 在基类）
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual void PostEditMove(bool bFinished) override;
#endif

private:
	/** 解析地面指针 + 向地面登记自己（幂等）。**不订阅** `OnGroundChanged` —— 见类注释。 */
	void ResolveGroundAndRegister();

	/** 向地面注销（销毁路径）。bRefreshGround 时让地面把自己的隆起抹掉。 */
	void UnregisterFromGround(bool bRefreshGround);

	/**
	 * 编辑器里：mask / 尺寸 / 平滑 / 格距变了就重烘距离场（指纹比对，没变是零成本）。
	 * cook 后的包里读不到贴图源数据，直接用存盘的那份。
	 */
	void EnsureMaskSdf();

	/** 本座当前输入的指纹（编辑器才算得出：要读贴图源数据的 GUID）。 */
	uint32 ComputeMaskSdfKey() const;

	// ---- 烘焙好的距离场（随关卡存盘；布局见 CSGroundShaperField::FMaskSdf）----
	UPROPERTY()
	TArray<float> MaskSdf;

	UPROPERTY()
	int32 MaskSdfN = 0;

	UPROPERTY()
	float MaskSdfCellSize = 1.0f;

	UPROPERTY()
	float MaskSdfContourRadius = 0.0f;

	UPROPERTY()
	float MaskSdfDeepest = 0.0f;

	UPROPERTY()
	uint32 MaskSdfKey = 0;

	// ---- 2026-09-29 之前的形状参数，只为迁移老关卡（PostLoad）----
	// 默认值必须是当时的 C++ 默认：没覆写过的实例在存盘时不写这两项，读回来就是这里的值。
	// ⚠️ 名字必须与老存盘里的**逐字相同**：改成 `X_DEPRECATED` + PropertyRedirects 实测读不回来
	// （L_TerrainOpsDemo 存的 LiftHeight=700 读成了默认 300，塑形物塌成 2 cm），留原名最稳。
	// 私有、不进细节面板、蓝图/Python 都看不见；迁移之后的存盘里它们等于原型值，不会再写出来。
	UPROPERTY()
	float Radius = 150.0f;

	UPROPERTY()
	float LiftHeight = 300.0f;

#if WITH_EDITORONLY_DATA
	UPROPERTY()
	TObjectPtr<UBillboardComponent> SpriteComponent;
#endif

	/** 上次已生效的足迹：移动/改参时要把 union(旧, 新) 交给地面，才能把旧位置的隆起抹掉。 */
	FBox2D LastAppliedFootprint = FBox2D(ForceInit);
};

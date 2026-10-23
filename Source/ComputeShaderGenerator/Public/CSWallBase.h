#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers / FHandoverCache —— 藤的 GPU 实例源
#include "CSHouseVine.h"           // FPlan / FWallStrip / FTubePath / FStrandHistory —— 藤的纯函数
#include "CSTinyGlade.h"
#include "CSWall.h"                // FCSWallPath / FCSWallSkins / ECSWallKind —— 墙的纯函数核心
#include "CSWallBase.generated.h"

class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;

/**
 * 叶子的季节（TG 的 `MI_{summer,autumn,winter}_ivy_leaf_color` 三张齐全，见对照文档 §7）。
 *
 * ⚠️ **切季节不换材质资产**：换资产会在实例路上换一次材质绑定，而季节是会被反复来回切的量；
 * 落地做法是母材质里三张贴图按一个 `Season` 标量混、actor 缓存一个 MID 只写那个标量
 * （见 `ACSWallBase::EnsureVineRig`）。三张贴图恒定采样的代价换掉了 shader 重绑。
 *
 * 2026-09-22 从 `CSHouseActor.h` 挪到这里（墙的基类要用；同模块内挪头文件，枚举路径不变，存档与脚本不受影响）。
 */
UENUM(BlueprintType)
enum class ECSVineSeason : uint8
{
	Summer UMETA(DisplayName = "Summer"),
	Autumn UMETA(DisplayName = "Autumn"),
	Winter UMETA(DisplayName = "Winter"),
};

/**
 * 墙上的藤（常春藤）的全部参数 —— 房子 `Vine*` 那 38 个属性收成一个结构体，默认值逐个照抄房子。
 *
 * 藤的胶水（`ACSWallBase::EnsureVineRig` / `PackVine`）只认它：样条墙直接给它的 `Vine` 属性；房子由
 * `ACSHouseActor::GetVineSettings` 从那 38 个平铺属性**现组**（2026-09-22 统一时刻意**不迁数据**：蓝图 / 关卡里的覆盖值、
 * 脚本里的 `vine_*` 属性名一个都不动；要迁时在 `PostLoad` 里迁一次、并逐个核对蓝图与关卡的覆盖值）。
 * 每一项的取值依据与坑表见 `ACSHouseActor` 同名属性的注释（`VineStrandSpacing` 等），这里不复述。
 *
 * 2026-09-22 从 `CSWallActor.h` 挪到这里（同模块内挪头文件，结构体路径不变）。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSWallVineSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	bool bEnabled = true;

	/** 枝（管子模式下只作旧路回退）。长度轴 +Z。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UStaticMesh> BranchMesh;

	/** 叶。长度轴 +Y。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UStaticMesh> LeafMesh;

	/** 花。留空 = 不开花（合法）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UStaticMesh> FlowerMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UMaterialInterface> BranchMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UMaterialInterface> LeafMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	TObjectPtr<UMaterialInterface> FlowerMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	ECSVineSeason Season = ECSVineSeason::Summer;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "20.0"))
	float StrandSpacing = 90.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "4.0"))
	float SegmentLength = 26.0f;

	/**
	 * 一根藤最多几段（与房子同为 22：墙默认 3 m 高，藤能爬满）。⚠️ 花从第 `FlowerFromFrac × MaxSegments` 段起才开 ——
	 * 把墙调矮（比如 150 cm，藤只爬得出 6 段左右）时要一起调小，否则永远到不了第 10 段、一朵花都没有（实测）。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "1", ClampMax = "128"))
	int32 MaxSegments = 22;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float Wander = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float MaxLean = 1.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float MaxTurn = 0.70f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "1.0"))
	float Bloat = 1.15f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "1.0"))
	float Thickness = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float StandOff = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float HoleClearance = 12.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LeafChance = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "2.0"))
	float LeafSize = 26.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0", ClampMax = "0.9"))
	float LeafSizeJitter = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowerChance = 0.10f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float FlowerFromFrac = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "2.0"))
	float FlowerSize = 22.0f;

	/** 走到墙端时绕到另一面接着长的概率（开口墙的两面在墙端首尾相接）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float JumpChance = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	bool bUseTube = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "3", ClampMax = "24"))
	int32 TubeSegments = 8;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0", ClampMax = "8"))
	int32 TubeSubdivide = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "1.0"))
	float GrowSpeed = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float LeafGrowDelay = 0.75f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float FlowerGrowDelay = 1.8f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.01"))
	float GrowFadeSeconds = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	bool bGrowOnLoad = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float TipTaperLength = 60.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.01", ClampMax = "1.0"))
	float TipTaperMin = 0.06f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "0.0"))
	float MaxGroundGap = 25.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine", meta = (ClampMin = "10.0"))
	float GroundSampleSpacing = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Vine")
	int32 Seed = 1;
};

/**
 * **墙的基类** —— 「房子变成墙」第 4 步（2026-09-22「统一房子和墙的逻辑」），计划见 `Docs/TinyGlade/Plan_Wall.md`。
 *
 * TG 里实体只有墙，"建筑"是"围合的墙 + 屋顶"。这一层是两种墙共同的部分：
 *
 *   · **路径来源是虚函数**（`GetWallPath`）：样条墙给样条采出的中线（世界空间），房子给 footprint 围成的环
 *     （局部空间 + `GetBuildTransform()`）。墙体 / 角石 / 藤的墙面都经 `CSWall` 的同一组纯函数从它派生。
 *   · **墙的种类**（`GetWallKind`）：独立墙 / 围合墙 —— 角石与藤按哪一套规矩出（见 `ECSWallKind`）。
 *   · **藤的胶水只有一份**：三个实例组件（叶 / 花 / 旧路的枝）、管子、三张 MID、基础网格快照、GPU 实例源交接、生长相位。
 *     参数一律从 `GetVineSettings()` 取；容量与交接包围盒各家自己算（墙长 / footprint 周长、屋脊高），由 `EnsureVineRig` 的回调交进来。
 *
 * 屋顶、门窗、接缝、摆件留在房子上（TG：屋顶是另挂在墙上的实体）；墙顶砖（压顶 / 垛口）只有样条墙出 ——
 * 房子的墙是平顶（`CSWall::FTopParams::Plain`），平屋顶的露台垛口归屋顶。
 *
 * ⚠️ 组件生命周期：三个实例组件按需现建（`CSShaperSteps::EnsureInstancedComponent`，Transient）；样条墙在构造函数里
 * 预建了同名默认子对象，现建时看到已有就直接用。管子组件是两边构造函数里各自建的默认子对象（名字都叫 `VineTubeMesh`）。
 */
UCLASS(Abstract)
class COMPUTESHADERGENERATOR_API ACSWallBase : public ACSTinyGlade
{
	GENERATED_BODY()

public:
	ACSWallBase();

	/** 墙的种类（角石 / 藤的规矩）。 */
	virtual ECSWallKind GetWallKind() const { return ECSWallKind::Freestanding; }

	/**
	 * **路径来源**：这堵墙的路径 + 皮线，以及路径所在空间 → 世界的变换。返回 false = 这一刻没有墙。
	 * 样条墙：样条采出的中线（世界空间，`OutWorld` 恒等）；房子：footprint 围成的环（局部空间，`OutWorld = GetBuildTransform()`）。
	 */
	virtual bool GetWallPath(FCSWallPath& OutPath, FCSWallSkins& OutSkins, FTransform& OutWorld) const { return false; }

	/** 藤的全部参数（样条墙：`Vine` 属性；房子：从平铺属性现组）。 */
	virtual FCSWallVineSettings GetVineSettings() const { return FCSWallVineSettings(); }

protected:
	// -------------------------------------------------------------------------
	// 藤的胶水（房子与样条墙共用一份；2026-09-22 之前样条墙抄了房子一份）
	// -------------------------------------------------------------------------

	/**
	 * 组件 + 材质（季节 MID、生长 MID）+ 基础网格快照 + 容量 + 实例源交接。基础网格快照建不起来时返回 `UpToDate`
	 * 且 `bVineBaseMeshReady` 为假（调用方据此早退）。
	 *
	 * `Budget` 在快照就绪之后才调（那时组件一定在）：给出三个调色板的常驻记录容量与交接包围盒（组件空间）。
	 * ⚠️ 容量必须是配置量的台阶函数（`CSShaperSteps::ReserveCount`），包围盒必须量化 + 只涨不缩 —— 两者都是"值连续变 ⇒
	 * 资源每帧重来"那一类阻塞的解法，理由见房子 `EnsureVineComponents` 原来的注释（现在在各家的 `Budget` 里）。
	 */
	CSShaperSteps::EHandoverResult EnsureVineRig(const FCSWallVineSettings& V,
		TFunctionRef<void(uint32& OutMaxRecords, FBox& OutLocalBounds)> Budget, bool bForceFullRebuild);

	/** 设置 → 规划参数。没配花网格时花的概率钉成 0（否则记录照排、counter 指向没有基础网格的组件："数对得上屏幕上什么都没有"）。 */
	static CSHouseVine::FParams MakeVineParams(const FCSWallVineSettings& V);

	/** 三个调色板的 GPU 缓冲都在（交接过、容量备好）。 */
	bool AreVineBuffersReady() const;

	/**
	 * 规划的收尾：生长相位（老藤沿用、改过的从变化点接着长）→ 叶 / 花回填相位 → 打包实例 → 枝的管子。
	 * ⚠️ 相位必须在打包**之前**回填（打包把记录拍平上传，之后再改只是改了一份没人读的 CPU 副本）；
	 * 管子模式下枝不走实例（摘掉记录 ⇒ `Pack` 的空表分支把 counter 清零，否则分段实例与管子同时画）。
	 * `Plan` 会被改（叶 / 花的相位、管子模式下清掉的枝记录）—— 计数请在调用之前取。
	 */
	void PackVine(const FCSWallVineSettings& V, const CSHouseVine::FParams& Params,
		const TArray<CSHouseVine::FWallStrip>& Strips, CSHouseVine::FPlan& Plan);

	/**
	 * 撤掉藤：⚠️ **先把 counter 清零再撤实例源**（撤掉之后交接缓存被清空，下一次会把同一批 buffer 交回组件，
	 * 陈旧计数器会把上一代的藤再画一遍 —— 门框砖因此在画面上留过 12 层砖），再撤管子与交接缓存。
	 * 计数 / 哈希归各家自己清。
	 */
	void ClearVineRig();

	/** 管子：只留最新那一份（在途被拒 ⇒ 停进 `PendingVineTubePath`，完成回调补发）。 */
	void SubmitVineTube(TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> Path);
	void OnVineTubeEditComplete();

	/** 生长相位的时钟（世界时间秒）。 */
	float GetVineClock() const;

	/** 藤的三个 GPU 实例宿主：枝（管子模式下不画，只作旧路回退）、叶、花。按需现建（Transient）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineBranchComponent;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineLeafComponent;

	UPROPERTY(Transient)
	TObjectPtr<UCSGpuInstancedMeshComponent> VineFlowerComponent;

	/**
	 * 藤枝管子（世界空间几何，组件相对变换钉成恒等）。派生类构造函数里建的默认子对象（名字 `VineTubeMesh`）。
	 * 叶与花**不走这里** —— 它们仍是实例，挂在上面那三个组件上。
	 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall|Vine")
	TObjectPtr<UCSMeshRenderComponent> VineTubeComponent;

	/** 管子的网格对象。Transient：派生物，加载后由重求值重建；Outer 是管子组件（组件销毁时它自己还显存）。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> VineTubeMesh;

	/**
	 * 三季叶用的 MID（父 = 叶材质），同时承担叶的生长参数。缓存在 actor 上而不是每次重建 ——
	 * 蓝图重跑构造脚本会销毁组件，MID 活在 actor 上才不会跟着一起没。⚠️ 父换了必须重建（否则"换了材质画面没变"）。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VineLeafSeasonMID;

	/** 枝（管子）/ 花的 MID：只为把生长参数（`VineGrowSpeed` 等）下推过去。父换了必须重建。 */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VineBranchGrowMID;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> VineFlowerGrowMID;

	/**
	 * 基础网格快照是从哪张资产建的。⚠️ 只靠一个 bool 会"换了资产什么都没发生"：快照建过一次就不再重建，
	 * 细节面板里换了网格而画面还是旧的那张。
	 */
	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineBranchMeshBuiltFrom;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineLeafMeshBuiltFrom;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> VineFlowerMeshBuiltFrom;

	/** 三个调色板的 GPU 缓冲（下标 = `CSHouseVine::EPalette`）。生产者那一份归 actor（`ReleaseInstancedBuffers` 里还）。 */
	TArray<CSShaperSteps::FPaletteBuffers> VineGpuBuffers;
	CSShaperSteps::FHandoverCache VineHandover;
	TSharedPtr<CSHouseVine::FTubePath, ESPMode::ThreadSafe> PendingVineTubePath;
	bool bVineBaseMeshReady = false;

	/** 逐藤的生长历史，键 = `FStrand::RootKey`。transient：相位不该跨关卡保留。 */
	TMap<uint32, CSHouseVine::FStrandHistory> VineStrandHistory;
};

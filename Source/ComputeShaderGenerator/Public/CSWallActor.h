#pragma once

#include "CoreMinimal.h"
#include "CSGroundShaperSteps.h"   // FPaletteBuffers / FHandoverCache —— 砖与藤的 GPU 实例源
#include "CSTinyGlade.h"
#include "CSWallBase.h"            // 墙的基类：路径来源 + 藤的胶水（房子也派生自它）；FCSWallVineSettings / ECSVineSeason
#include "CSWall.h"
#include "Containers/Ticker.h"
#include "CSWallActor.generated.h"

class ACSGroundActor;
class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class USplineComponent;
class UStaticMesh;

// `FCSWallVineSettings`（藤的全部参数）2026-09-22 挪到了 `CSWallBase.h`：房子与样条墙的藤共用一份胶水。

/**
 * 样条墙 —— TG 的 freehand 墙（2026-09-22 用户："我需要一个新的 actor，它包含一个 spline component，
 * 可以根据 spline 生产出墙"）。计划与参考截图见 `Docs/TinyGlade/Plan_Wall.md`。
 *
 * **墙 = 样条的中线**：视口里拖点、加点、删点、闭合（样条的 Closed Loop）全是引擎现成的交互。
 * 样条按 `SampleSpacing` 密采成折线，墙以它为中线、两边各偏半个墙厚（斜接），墙脚贴地、墙顶离地
 * `WallHeight`，墙顶一层压顶砖 + 隔块抬起的垛口，墙端与拐角出角石，两面爬藤。
 *
 * 逻辑分三层（「房子变成墙」计划，2026-09-22 第 4 步起房子也走同一套）：
 *   · **纯函数核心** `CSWall`（`CSWall.h`）：路径 → 皮线 → 墙体 / 墙顶砖 / 角石 / 藤的墙面。房子同一组函数。
 *   · **墙的基类** `ACSWallBase`：路径来源（虚函数）+ 藤的胶水（组件、MID、快照、交接、生长相位、管子）。房子也派生自它。
 *   · **本类**：样条 + 地面 → 路径；墙体（`Continuous` 出面）、墙顶砖（压顶 + 垛口）、角石（独立墙规矩）进各自组件，
 *     与房子的门框砖同一套交接纪律：注册期定容、交互期零阻塞。
 *
 * 什么时候重建：构造脚本、样条改动 / 组件变换通知、编辑器拖动、地面变化 —— 一律**本帧标脏、
 * 下一帧重建**（与房子同一条裁决，`RequestReevaluate`）。每一样产物各自哈希守卫，没变就不碰 GPU。
 *
 * 没做的（计划第 2 步以后）：路穿墙开拱、窗 / 门、墙与墙 / 墙与房子相交的开洞与接缝砖、端点吸附合并、
 * 发布到世界级墙表、碰撞（gpumesh 全线无碰撞，编辑器放置射线打不到墙）。
 */
UCLASS(Blueprintable, BlueprintType)
class COMPUTESHADERGENERATOR_API ACSWallActor : public ACSWallBase
{
	GENERATED_BODY()

public:
	ACSWallActor();

	// -------------------------------------------------------------------------
	// 形状
	// -------------------------------------------------------------------------

	/** 墙顶离地高度 cm（压顶砖另算，在它上面）。默认 3 m（用户 2026-09-22），与房子的墙高同一个值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "20.0", ClampMax = "2000.0"))
	float WallHeight = 300.0f;

	/** 墙厚 cm。墙以样条为中线，两边各一半。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "8.0", ClampMax = "300.0"))
	float WallThickness = 34.0f;

	/** 样条的采样间距 cm：弯墙的平滑度与贴地的细度都由它定（地面网格 50 cm，再密没有意义）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "10.0", ClampMax = "400.0"))
	float SampleSpacing = 50.0f;

	/** 墙脚贴地、墙顶随地形起伏。关掉 = 按样条点自己的高度砌（场景里没有地面时等价于关掉）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall")
	bool bFollowGround = true;

	/** 墙脚往地里埋多深 cm（取墙厚两侧与中线三处地面的最低点再往下埋）：坡上不露缝。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "0.0", EditCondition = "bFollowGround"))
	float GroundSink = 10.0f;

	/** 墙顶线的平滑遍数（[1,2,1]/4 核）：地面 50 cm 一格，原样跟着走墙顶会发抖。0 = 不平滑。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "0", ClampMax = "16", EditCondition = "bFollowGround"))
	int32 TopSmoothing = 3;

	/**
	 * **拐角阈值**（度）：样条折成线段后，相邻两段的转角小于它的接点是**圆滑的墙**（侧面共用法线、不出角石、
	 * 墙顶砖不断开、藤的法线照常插值）；大于等于它才是**拐角**（硬棱、凸侧角石、墙顶砖断开）。
	 * 按夹角判、与点是不是样条控制点无关（2026-09-22 用户："线段夹角过大时就不会产生转角，而是产生圆滑的墙"）。
	 * 采样间距 50 cm 时，30° 相当于半径约 1 m 的弯 —— 比这更急的弯按拐角处理。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall", meta = (ClampMin = "5.0", ClampMax = "120.0"))
	float CornerTurnDegrees = 30.0f;

	/** 墙面材质（与房子同一张 `MI_TinyGladeWall`：灰泥 + 剥落露砖；顶点色 / UV1 口径相同）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall")
	TObjectPtr<UMaterialInterface> WallMaterial;

	// -------------------------------------------------------------------------
	// 砖（压顶 / 垛口 / 角石 —— 与房子门框砖同一条 GPU 解析砖路、同一块 `brick`）
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	bool bBricksEnabled = true;

	/** TG 的 `brick`（100 cm 居中单位立方体，逐实例非均匀缩放成任意尺寸）。留空 = 不出砖。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	TObjectPtr<UStaticMesh> BrickMesh;

	/** 整体覆盖材质。留空 = 画 `BrickMesh` 资产自带的材质（与房子 `FrameMaterial` 同一口径）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	TObjectPtr<UMaterialInterface> BrickMaterial;

	/** 砖的标称长度 cm。墙顶按它均分，角石按它分层。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "5.0"))
	float BrickLength = 30.0f;

	/** 一层砖高 cm（压顶 / 垛口的厚度；角石的截面进深）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "2.0"))
	float BrickHeight = 18.0f;

	/** 砖在墙面两侧各凸出多少 cm（砖横跨墙厚 + 两倍它）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "0.0"))
	float BrickProtrude = 4.0f;

	/** 长度轴胀大（TG 那一步：相邻砖互相咬住，负缝不露光）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "1.0", ClampMax = "2.0"))
	float BrickBloat = 1.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	bool bCoping = true;

	/** 垛口（TG 截图里墙顶那排参差的城齿）：压顶之上每隔 `MerlonEvery` 块抬一块。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	bool bMerlons = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "1", ClampMax = "16", EditCondition = "bMerlons"))
	int32 MerlonEvery = 2;

	/** 压顶砖往墙顶里埋多深 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "0.0"))
	float CopingSink = 3.0f;

	/** 墙端两侧的角石柱。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	bool bEndQuoins = true;

	/** 拐角（见 `CornerTurnDegrees`，且不比直角尖）凸侧的角石柱。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	bool bCornerQuoins = true;

	/** 角石的长边抖动 / 分层抖动（TG 基准 cm，与房子 `QuoinJitter` / `QuoinSplitJitter` 同一口径）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "0.0"))
	float QuoinJitter = 16.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "0.0"))
	float QuoinSplitJitter = 21.0f;

	/** 砖的身份种子（逐块随机数只由它 + 家族 + 块号决定，拖样条时不闪）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick")
	int32 BrickSeed = 1;

	/** 砖的常驻容量（注册期一次付清、只截断不扩容）。墙长 / 砖长 × 1.5 + 角石，默认够 300 m 的墙。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Brick", meta = (ClampMin = "64", ClampMax = "65536"))
	int32 BrickReserveCapacity = 2048;

	// -------------------------------------------------------------------------
	// 藤
	// -------------------------------------------------------------------------

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Wall|Vine")
	FCSWallVineSettings Vine;

	// -------------------------------------------------------------------------
	// 入口与观测量
	// -------------------------------------------------------------------------

	/** 全量重建（清掉所有哈希短路）。 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS Wall")
	void RebuildWall();

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	USplineComponent* GetSpline() const { return Spline; }

	/** 上一次重建用的路径（世界空间）。脚本与单测读它对答案。 */
	const FCSWallPath& GetBuiltPath() const { FlushPendingReevaluate(); return BuiltPath; }
	const FCSWallSkins& GetBuiltSkins() const { FlushPendingReevaluate(); return BuiltSkins; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	float GetWallLength() const { FlushPendingReevaluate(); return float(BuiltSkins.Length); }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetPathPointCount() const { FlushPendingReevaluate(); return BuiltPath.NumPoints(); }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetBodyTriangleCount() const { FlushPendingReevaluate(); return CurrentBodyTriangles; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetCopingBrickCount() const { FlushPendingReevaluate(); return CurrentTop.CopingBricks; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetMerlonCount() const { FlushPendingReevaluate(); return CurrentTop.MerlonBricks; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetQuoinColumnCount() const { FlushPendingReevaluate(); return CurrentQuoinColumns; }

	/** 砖路上的总砖数（压顶 + 垛口 + 角石）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetBrickCount() const { FlushPendingReevaluate(); return CurrentBrickCount; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetVineStrandCount() const { FlushPendingReevaluate(); return CurrentVineStrands; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetVineLeafCount() const { FlushPendingReevaluate(); return CurrentVineLeaves; }

	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetVineFlowerCount() const { FlushPendingReevaluate(); return CurrentVineFlowers; }

	/** 真正跑完的重建次数（合批的观测量）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetRebuildCount() const { FlushPendingReevaluate(); return RebuildCount; }

	/** 墙体网格真的重传了几次（哈希短路的观测量）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetBodyUploadCount() const { FlushPendingReevaluate(); return BodyUploadCount; }

	/** 砖真的重排了几次。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall")
	int32 GetBrickScatterCount() const { FlushPendingReevaluate(); return BrickScatterCount; }

	/** 墙脚下有没有接到地面（至少一个采样点落在地面范围内）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall")
	bool IsOnGround() const { FlushPendingReevaluate(); return bOnGround; }

	/** GPU 真的在画几块砖（**阻塞**回读，诊断用）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadBrickCountGpuSync() const;

	/** GPU 真的在画几片藤叶（**阻塞**回读，诊断用）。 */
	UFUNCTION(BlueprintPure, Category = "CS Wall|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadVineLeafCountGpuSync() const;

	/** 补上欠着的那次重建（读派生状态之前调；无头脚本单帧跑完，Tick 插不进来）。 */
	void FlushPendingReevaluate() const;

	/**
	 * 样条 → 路径（只填 XY、样条自己的高度与控制点标记；墙脚 / 墙顶在 `ApplyGround` 里定）。
	 * **公开**是为了单测拿同一份采样口径去核对纯函数。
	 */
	void BuildPathFromSpline(FCSWallPath& Out) const;

	//~ ACSWallBase —— 独立墙：路径 = 样条采出的中线（世界空间）
	virtual ECSWallKind GetWallKind() const override { return ECSWallKind::Freestanding; }
	virtual bool GetWallPath(FCSWallPath& OutPath, FCSWallSkins& OutSkins, FTransform& OutWorld) const override;
	virtual FCSWallVineSettings GetVineSettings() const override { return Vine; }

	//~ ACSTinyGlade
	virtual void ReevaluateSite() override;
	virtual FString DebugGetGpuAssetMismatchSync() const override;

	//~ AActor
	virtual void PostRegisterAllComponents() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
#if WITH_EDITOR
	virtual void PostEditMove(bool bFinished) override;
	virtual void PostEditUndo() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** 默认资产（与房子蓝图 `BP_TinyGladeHouse` 挂的是同一批）。 */
	static const TCHAR* DefaultWallMaterialPath;
	static const TCHAR* DefaultBrickMeshPath;

protected:
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const override;
	virtual void ReleaseInstancedBuffers() override;

private:
	/** 墙的中线 —— **默认子对象**，放进关卡就带着一条 L 形的三点样条（照 TG 参考截图）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USplineComponent> Spline;

	/** 压顶 / 垛口 / 角石（一个组件、一个调色板、一个 dispatch —— 与房子门框砖同构）。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS Wall", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSGpuInstancedMeshComponent> BrickComponent;

	// 藤的三个实例组件、管子组件与网格、三张 MID、快照来源在基类 `ACSWallBase` 上（本类构造函数仍按原名
	// `VineBranch` / `VineLeaf` / `VineFlower` / `VineTubeMesh` 建默认子对象）。

	/** 订阅着的地面。Transient：每次重建按需重新找。 */
	UPROPERTY(Transient)
	TObjectPtr<ACSGroundActor> Ground;

	// --- 派生状态（不存盘） ---
	FCSWallPath BuiltPath;
	FCSWallSkins BuiltSkins;
	FCSMeshSlotState BodySlot;
	uint32 BodyHash = 0;
	int32 CurrentBodyTriangles = 0;
	int32 BodyUploadCount = 0;

	TArray<CSShaperSteps::FPaletteBuffers> BrickGpuBuffers;
	CSShaperSteps::FHandoverCache BrickHandover;
	uint32 BrickHash = 0;
	int32 CurrentBrickCount = 0;
	int32 BrickScatterCount = 0;
	CSWall::FTopResult CurrentTop;
	int32 CurrentQuoinColumns = 0;

	uint32 VineHash = 0;
	int32 CurrentVineStrands = 0;
	int32 CurrentVineBranches = 0;
	int32 CurrentVineLeaves = 0;
	int32 CurrentVineFlowers = 0;

	bool bOnGround = false;
	bool bForceFullRebuild = false;
	bool bReevaluatePending = false;
	bool bInReevaluate = false;
	bool bSubscriptionsDisabled = false;
	int32 RebuildCount = 0;

	FDelegateHandle GroundChangedHandle;
	FDelegateHandle AnyGroundRebuiltHandle;
	FDelegateHandle RootTransformChangedHandle;
	TWeakObjectPtr<USplineComponent> ObservedSpline;
	FDelegateHandle SplineChangedHandle;
	FDelegateHandle SplineUpdatedHandle;
	FDelegateHandle SplineTransformChangedHandle;
#if WITH_EDITOR
	/** 非实时 / 被节流的视口不跑 actor tick：有欠账时挂一次性的编辑器刷新（同楼梯）。 */
	FTSTicker::FDelegateHandle EditorRebuildTicker;
#endif

	void RequestReevaluate();
	void SubscribeChanges();
	void UnsubscribeChanges();
	void UnsubscribeSplineChanges();
	void HandleComponentTransformChanged(USceneComponent* Component, EUpdateTransformFlags Flags, ETeleportType Teleport);

	void ResolveGroundAndSubscribe();
	void UnsubscribeGround();
	void HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& ChangedBounds);
	void HandleAnyGroundRebuilt(ACSGroundActor* RebuiltGround, const FBox& ChangedBounds);
	bool SampleGround(const FVector2D& WorldXY, float& OutZ) const;

	/** 地面 → 逐点墙脚 / 墙顶（`BuildPathFromSpline` 只给了样条高度）。返回是否接到了地面。 */
	bool ApplyGround(FCSWallPath& Path, const FCSWallSkins& Skins) const;

	void RebuildBody();
	void SubmitBody(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot);
	void OnBodyEditComplete();
	void ClearBody();

	CSShaperSteps::EHandoverResult EnsureBrickComponent();
	void RebuildBricks();

	CSShaperSteps::EHandoverResult EnsureVineComponents();
	void RebuildVine();
	void ClearVine();

	/** 世界盒 → 某个实例组件的局部盒（交接包围盒要组件空间）。 */
	static FBox WorldBoundsToComponent(const FBox& WorldBounds, const USceneComponent* Component);
	/** 墙的世界包围盒（皮线 XY × 墙脚到墙顶 + `ExtraTop`）。 */
	FBox GetWallWorldBounds(double ExtraTop) const;
};

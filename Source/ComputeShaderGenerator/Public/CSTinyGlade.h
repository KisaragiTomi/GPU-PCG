#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CSTinyGlade.generated.h"

class UCSGpuInstancedMeshComponent;
class UCSMesh;
class UCSMeshRenderComponent;
class UMaterialInterface;
struct FCSGpuMeshCPUData;

/**
 * 网格槽的簿记：一个"渲染组件 + transient UCSMesh"对的在途状态与摆位基线。
 *
 * 组件与 UCSMesh 本身**不在这里** —— 它们仍是 actor 上的 UPROPERTY（子对象名按名序列化、
 * UPROPERTY 负责压住 GC），调用基类的槽函数时与本结构一起传。基类主网格是一个槽，
 * 房屋的柱子是另一个。
 *
 * 槽网格的 Outer 是槽的渲染组件（`EnsureSlotMesh`）：组件销毁时自己把它的显存还掉，actor 不管。
 */
struct FCSMeshSlotState
{
	/** 在途期间攒下的**最新**一份目标快照（只留最新、不排队）。完成回调取走补发。 */
	TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Pending;
	/** 几何当前烘在哪个世界变换下。常驻流是世界空间，摆位增量相对它算。 */
	FTransform BuiltAt = FTransform::Identity;
	/** 两级哈希：形状变 → 全量重建；只有摆位变 → 一个变换 pass（见 `ReconcileMeshSlot`）。 */
	uint32 ShapeHash = 0;
	uint32 PlacementHash = 0;
};

/** 一次快照上传的材质表与流布局。 */
struct FCSMeshSlotUpload
{
	/** 槽 i = Materials[i]；组件 MeshMaterial = 槽 0。 */
	TArray<TObjectPtr<UMaterialInterface>> Materials;
	/** UV 组数（房体是 2：UV1 传裁剪场）。大于 1 时上传前声明一次，同组数时零成本。 */
	int32 NumTexCoordSets = 1;
	/** 多材质槽：排序分段录进同一张图，完成回调里发布分段表。单槽不需要。 */
	bool bSortSections = false;
};

/**
 * 一族 GPU 实例生成物。诊断与烘焙都按这张表逐族走（显存不走这里 —— 组件自己放）。
 *
 * 不用 `GetComponents<UCSGpuInstancedMeshComponent>()` 扫：实例组件是 NewObject 自动命名的，
 * 名字每次会话都变（烘出来的资产名随之漂，`bReplaceExistingAsset` 永远对不上旧路径），
 * 而且闲置组件会在诊断里被误报成"GPU 网格没分配"。
 */
struct FCSInstancedFamily
{
	UCSGpuInstancedMeshComponent* Component = nullptr;
	/** 诊断文案里的家族前缀。 */
	FString Label;
	/** 烘焙资产名后缀：`SM_<actor>_<AssetSuffix>`。改了就换资产名。 */
	FString AssetSuffix;
	/** 这一轮按设计在画吗（CPU 侧计数 > 0）。false 时诊断跳过它；烘焙照走。 */
	bool bExpectDrawn = true;
};

/**
 * Tiny Glade 交互对象的共同基类（Plan_House.md）：地面 / 房屋 / 塑形物 / 样条块
 * 都是"CPU 权威数据 → FCSGpuMeshCPUData 快照 → UCSMesh"的同构 actor，这里收敛三件事：
 *
 *  1. 网格槽：渲染组件 + transient UCSMesh 的上传、材质绑定、在途只留最新、摆位增量、撤槽。
 *     主网格（TinyGladeMesh）是一个槽；派生类的其它槽（房屋的柱、藤管）走同一套函数，
 *     只有组件与网格的 UPROPERTY 留在派生类。GPU 数据不随关卡存盘，加载后由派生类重建。
 *  2. 声明式重求值入口 ReevaluateSite()：任何唤醒（构造脚本重跑、OnGroundChanged 直推、
 *     subsystem 变换快扫）都汇到这一个虚函数 —— 派生类在里面从权威源重导出目标状态、
 *     哈希比对、变了才重建。基类默认空实现：地面是权威数据源，不消费世界变化。
 *  3. 实例族清单（GetInstancedFamilies）：GPU 实例路产物的诊断与烘焙都按这张表走。派生类只报
 *     "有哪几族"，循环只写一遍 —— 两份手写清单曾各自漏过族。
 *
 * 显存各归各，删除时谁都不替别人收拾：gpumesh 那一份（实例组件手上的实例源与它自己的常驻网格、
 * 渲染组件拥有的槽网格）由组件在 OnComponentDestroyed 里自己放；actor 只放自己分配的生产者缓冲
 * （ReleaseInstancedBuffers，EndPlay / Destroyed 各调一次）。
 *
 * gpumesh 全线 NoCollision——派生类的拾取一律解析实现（镜像高度场 / 参数化 OBB），
 * 不给网格加碰撞体。
 */
UCLASS(Abstract)
class COMPUTESHADERGENERATOR_API ACSTinyGlade : public AActor
{
	GENERATED_BODY()

public:
	ACSTinyGlade();

	/**
	 * 声明式重求值：从权威源重导出目标状态 → 哈希比对 → 变了才重建。
	 * 必须幂等——直推架构下它会被高频、重复、甚至自发地调用，重复调用必须收敛为零成本。
	 * CallInEditor 按钮兼作调试入口：怀疑状态不同步时手动点一次即对齐。
	 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "CS TinyGlade")
	virtual void ReevaluateSite() {}

	UFUNCTION(BlueprintPure, Category = "CS TinyGlade")
	UCSMesh* GetTinyGladeMesh() const { return TinyGladeMesh; }

	UFUNCTION(BlueprintPure, Category = "CS TinyGlade")
	UCSMeshRenderComponent* GetTinyGladeMeshComponent() const { return TinyGladeMeshComponent; }

	/**
	 * 每一族 GPU 实例路上，**GPU 真的在画的基础网格 / 材质**是不是我们以为的那两样。
	 * 空串 = 是；原因串带家族前缀（`FCSInstancedFamily::Label`）。按设计这一轮不画的族跳过。
	 *
	 * 与 `Is*Drawable` 那一族的分工：那边查"能不能画"（组件在不在、注册没注册、非空没非空），
	 * 这边查"画的是不是那个" —— 网格那一半是把上传到 GPU 的那份**回读出来**跟资产对，
	 * 不是信任 CPU 快照。**阻塞**（回读）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS TinyGlade|Diagnostics", meta = (DevelopmentOnly))
	virtual FString DebugGetGpuAssetMismatchSync() const;

#if WITH_EDITOR
	/**
	 * 把本 actor **全部**实例路产物烘成 StaticMesh 资产 —— 裁决六 ① 的用户入口。
	 *
	 * 一族一张，落在 `BakeFolder/SM_<actor>_<family>`（留空 = `/Game/TinyGladeBake/<actor>`）；
	 * 返回真的烘出来的张数（这一族没有实例就跳过，跳过不算失败 —— 一栋不长花的房子是合法的）。
	 *
	 * ⚠️ **阻塞**（每族两次回读 + 一次 StaticMesh 构建），而这是**有意**的：它是用户主动发起的
	 * 离线操作，不在任何交互路径上。它照旧被 `UCSMesh::GetBlockingFlushCount()` 数到，
	 * `flushes=0` 那几条断言会在它被误接进重建链路的那一刻报红。
	 *
	 * 网格路（主网格 / 柱 / 岩壳）走 `UCSMeshRenderComponent::SaveToStaticMesh`，不在这里。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS TinyGlade")
	virtual int32 SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets = false);
#endif

	//~ AActor interface
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

protected:
	// -------------------------------------------------------------------------
	// 网格槽
	// -------------------------------------------------------------------------

	/**
	 * 主网格的**同步**上传：按需建 UCSMesh、绑材质表、CopyFromMeshSnapshot 上传、SetGpuMesh 绑定，
	 * 多材质槽时顺带 BuildMaterialSections（单槽时"整网格一个批次"本来就是正确语义）。
	 * 槽 i = Materials[i]（存盘资产的材质来源），组件 MeshMaterial = 槽 0。
	 * 交互热路径上的槽走 `SubmitMeshSlotAsync`。
	 */
	bool UploadTinyGladeSnapshot(const FCSGpuMeshCPUData& Snapshot, const TArray<TObjectPtr<UMaterialInterface>>& Materials);

	/**
	 * 只重绑主网格的材质表，一个三角形都不重传 —— UCSMesh::SetMaterial / NotifyMaterialsChanged 明写
	 * "Deliberately does not touch Generation"，渲染组件的 HandleMeshChanged 收到后只重解
	 * batch 材质。这是计划 D14 那条纪律的执行面：**纯外观量绝不进 desc 哈希**，改材质走这条，
	 * 不走上传 —— 后者被几何哈希守卫，改材质会被整个吞掉（症状是细节面板换材质画面零变化，
	 * 必须手点重建）。尚未上传过（TinyGladeMesh 为空）时是 no-op，首次上传自会带上材质。
	 */
	void BindTinyGladeMaterials(const TArray<TObjectPtr<UMaterialInterface>>& Materials);

	/**
	 * 任意一个槽的材质绑定：Mesh 的槽 i = Materials[i]，组件 MeshMaterial = DrawMaterial
	 * （空则取槽 0）。**变了才写、变了才广播** —— 每次上传前都会走一遍，稳态不打扰渲染线程。
	 * DrawMaterial 与槽 0 分开给的用例：组件画 MID、槽里放资产本身（烘焙抓的是槽，transient
	 * 的 MID 不能进资产）。Mesh 为空时只绑组件。
	 */
	static void BindMeshSlotMaterials(UCSMeshRenderComponent* Component, UCSMesh* Mesh,
		const TArray<TObjectPtr<UMaterialInterface>>& Materials, UMaterialInterface* DrawMaterial = nullptr);

	/**
	 * 取一个槽的网格，没有就建 —— **Outer 是槽的渲染组件**，于是组件销毁时自己把它的显存还掉
	 * （UCSMeshRenderComponent::OnComponentDestroyed），actor 不必管。现有网格不归这个组件时
	 * 换一张新的：拥有关系只能有一个答案。组件为空时退回挂在 actor 上（那种网格只能等 GC）。
	 */
	UCSMesh* EnsureSlotMesh(UCSMeshRenderComponent* Component, TObjectPtr<UCSMesh>& Mesh);

	/**
	 * 槽网格此刻真有一套 GPU 分配吗。组件销毁时把显存还掉了（撤销删除、复活的是同一批对象时
	 * 就是这样）⇒ 必须重建，只看"网格对象在不在"会把一张空网格当成画好了。
	 */
	static bool IsSlotMeshLive(const UCSMesh* Mesh);

	/**
	 * 一个槽的**异步**上传：整份快照一次 EditMeshAsync（上传 + 可选的排序分段在同一张图里）。
	 *
	 * 在途时 EditMeshAsync 会拒绝第二次（返回 false 且 OnComplete 永不触发），所以在途即把快照
	 * 停进 `Slot.Pending`（**最新态合并**）直接返回 —— OnSettled 里调用方用 `TakePending` 取出补发。
	 * 拖拽 30 Hz 下被拒是常态而非边界情形；这条链给出的速率自动等于 GPU 实际完成速率。
	 *
	 * OnSettled 在游戏线程尾巴上回调，actor 已失效时不调（所以里面可以放心捕获 this）。
	 * 排序分段在它之前已经发布。被拒的原因不是"在途"时退回同步上传一次。
	 */
	void SubmitMeshSlotAsync(UCSMeshRenderComponent* Component, TObjectPtr<UCSMesh>& Mesh, FCSMeshSlotState& Slot,
		const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe>& Snapshot, const FCSMeshSlotUpload& Upload,
		TFunction<void()> OnSettled);

	/**
	 * 形状未变、只是搬了地方：一个位置 + 切线 pass 把已有几何从 `Slot.BuiltAt` 搬到 NewWorld，
	 * 不重建、不重传。**同样走异步**（拖动是交互热路径，一次设备同步都不许有）。
	 *
	 * 返回是否已把几何搬到位；false = 这一次没送出去（在途 / 被拒），调用方不许推进摆位哈希，
	 * 否则这次移动就永远丢了 —— 增量始终相对 BuiltAt 算，下一次重试自然算出更大的那个增量，
	 * 天然自愈、绝不会累加两次。OnSettled 同 `SubmitMeshSlotAsync`。
	 */
	bool ApplyMeshSlotPlacement(UCSMesh* Mesh, FCSMeshSlotState& Slot, const FTransform& NewWorld, TFunction<void()> OnSettled);

	/**
	 * 重求值里的两级哈希分派：bRebuild 或形状变了 ⇒ Rebuild() 并把两个哈希都推到新值；
	 * 否则摆位变了 ⇒ Place()，**只有真送出去了**（返回 true）才推进摆位哈希。
	 */
	static void ReconcileMeshSlot(FCSMeshSlotState& Slot, uint32 ShapeHash, uint32 PlacementHash, bool bRebuild,
		TFunctionRef<void()> Rebuild, TFunctionRef<bool()> Place);

	/** 在途则把 Payload 停进 Pending（只留最新）并返回 true —— 调用方随即 return。 */
	template <typename TPayload>
	static bool ParkIfInFlight(const UCSMesh* Mesh, TSharedPtr<TPayload, ESPMode::ThreadSafe>& Pending,
		const TSharedPtr<TPayload, ESPMode::ThreadSafe>& Payload)
	{
		if (!IsMeshEditInFlight(Mesh)) return false;
		Pending = Payload;
		return true;
	}

	/** 取出在途期间攒下的那一份（没有则为空）。 */
	template <typename TPayload>
	static TSharedPtr<TPayload, ESPMode::ThreadSafe> TakePending(TSharedPtr<TPayload, ESPMode::ThreadSafe>& Pending)
	{
		TSharedPtr<TPayload, ESPMode::ThreadSafe> Next = MoveTemp(Pending);
		Pending.Reset();
		return Next;
	}

	/**
	 * 撤掉一个槽：组件解绑、网格置空，**在途待发的那份一并作废** —— 只撤网格、留着 pending 的话，
	 * 在途编辑的完成回调会把它补发出去、再建一份网格绑回组件，旧几何就这么复活了；
	 * 之后形状哈希不再变，它会一直留着。
	 */
	template <typename TPayload>
	static void ClearMeshSlot(UCSMeshRenderComponent* Component, TObjectPtr<UCSMesh>& Mesh,
		TSharedPtr<TPayload, ESPMode::ThreadSafe>& Pending)
	{
		UnbindMeshComponent(Component);
		Mesh = nullptr;
		Pending.Reset();
	}

	static bool IsMeshEditInFlight(const UCSMesh* Mesh);
	static void UnbindMeshComponent(UCSMeshRenderComponent* Component);

	// -------------------------------------------------------------------------
	// 实例族
	// -------------------------------------------------------------------------

	/**
	 * 报出本 actor 的每一族 GPU 实例生成物（诊断与烘焙共用）。**不许有副作用** —— 它是 const
	 * 诊断入口的一部分；需要先补合批欠账的派生类在诊断入口的 override 里先补。
	 */
	virtual void GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const {}

	/**
	 * 把派生类**自己分配**的那批实例缓冲（生产者那一份）交回去，并清掉对应的交接缓存。
	 * EndPlay 与 Destroyed 都会来，必须幂等。
	 *
	 * 只放自己的：组件手上那份实例源引用、组件自己的常驻网格、渲染组件拥有的槽网格，都由组件在
	 * OnComponentDestroyed 里自己放（删 actor 时引擎先调本类的 Destroyed，再逐个调组件的那一条）。
	 * 编辑器里的删除可以撤销、复活的是同一批对象 —— 需要复活后重建的派生类在这里记一笔。
	 */
	virtual void ReleaseInstancedBuffers() {}

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "CS TinyGlade", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCSMeshRenderComponent> TinyGladeMeshComponent;

	/** GPU 投影。Transient：属性只为压住 GC，不序列化网格数据。 */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> TinyGladeMesh;
};

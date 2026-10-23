#pragma once

#include <atomic>

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "CSGpuInstancedMeshComponent.h" // FCSGpuInstanceSourceGPU / FCSGpuInstancePointSourceGPU
#include "CSGpuInstancedNaniteComponent.generated.h"

/**
 * GPU-Scene 写入 pass 读实例的地方，三选一，与 UCSGpuInstancedMeshComponent 的三种实例源一一对应。
 *
 * 值类型：每次写入把它整份拷进写入委托，渲染线程执行时不再回头读组件 —— 组件在那之前换掉源、
 * 甚至被销毁都不影响那一次写入读到的东西。
 */
struct FCSGpuNaniteInstanceFeed
{
	/** CPU 实例数组：组件打包好的行（5 float4 / 实例，布局见 FCSGpuInstanceSourceGPU），写入时上传。 */
	TSharedPtr<const TArray<FVector4f>, ESPMode::ThreadSafe> CpuRows;
	FCSGpuInstanceSourceGPU Packed;
	FCSGpuInstancePointSourceGPU Points;

	bool IsGpu() const { return Packed.IsValid() || Points.IsValid(); }

	/**
	 * 这份源变了会不会有人来说一声。CPU 行恒为真（组件自己改的，改完就重新交接）；GPU 源看生产者的承诺
	 * （FCSGpuInstanceSourceGPU::bNotifiesChanges）。假 ⇒ 只能每帧重写。
	 */
	bool NotifiesChanges() const
	{
		if (Packed.IsValid()) return Packed.bNotifiesChanges;
		if (Points.IsValid()) return Points.bNotifiesChanges;
		return true;
	}
	bool IsValid() const { return IsGpu() || (CpuRows.IsValid() && CpuRows->Num() >= CS_GPU_INSTANCED_ROW_FLOAT4S); }

	/** GPU-Scene 要给这个图元分几个实例槽。GPU 源取容量：活着的个数只有 GPU 计数器知道，
	 *  多出来的槽由写入 pass 藏掉。 */
	uint32 GetSlotCount() const
	{
		if (Packed.IsValid()) return Packed.Capacity;
		if (Points.IsValid()) return Points.Capacity;
		return CpuRows.IsValid() ? uint32(CpuRows->Num() / CS_GPU_INSTANCED_ROW_FLOAT4S) : 0u;
	}
};

/** 写入委托在渲染线程上真正派发了几次 —— 诊断用，见 DebugGetDispatchedWriteCount。 */
struct FCSGpuNaniteWriteStats
{
	std::atomic<uint32> DispatchedWrites{ 0 };
};

/**
 * UCSGpuInstancedMeshComponent 走 Nanite 路（BaseMesh 开了 Nanite）时的渲染替身 —— **不要直接用**，由本体按需建、按需毁。
 *
 * 为什么要一个单独的 UStaticMeshComponent，而不是让本体自己建 Nanite 代理：Nanite 代理拿的是资产的
 * 渲染数据指针（RenderData / Nanite::FResources / 回退 LOD 的顶点工厂），资产一重建（编辑器里改 Nanite
 * 设置、重导入、异步编译完成）那些指针就作废。引擎靠 `FStaticMeshComponentRecreateRenderStateContext`
 * 在重建前把引用这张网格的组件先摘下来，而它找组件走的是 `IStaticMeshComponent` 反查表 —— 本体是
 * UPrimitiveComponent，不在表里，自己建的 Nanite 代理会在资产重建时悬空。当 UStaticMeshComponent
 * 就一并继承了这些：重建时摘挂、纹理流送、PSO 预缓存、材质用途标记、编辑器选中。
 *
 * 实例数据**只在 GPU 上**（`FInstanceSceneDataBuffers(true)` + `NumInstancesGPUOnly`，与 PCG 的
 * GPU 生成走的同一套引擎接口）。引擎对这种图元只分配实例区间、从不上传实例（GPUScene.cpp 里
 * `NumInstanceUploads` 对它恒为 0），区间里的每个槽初始化成 HIDDEN，写什么全靠我们：
 *   · 代理新建（换槽数 / 换网格 / 任何属性导致的重建）⇒ 新区间，必须当帧写，否则整族闪一帧；
 *   · 图元一动 ⇒ GPU-Scene 里存的是实例的**世界**变换，全部过期，必须重写；
 *   · GPU 源 ⇒ 生产者在同一批 buffer 里原地重写（它们只在容量 / 包围盒变了时才重新交接）。
 *     默认它们**从不通知**，所以**每帧**都重抄一遍 —— 与经典路每帧剔除时现读 buffer 同一个"不需要通知"的口径。
 *     代价要说清楚：每帧的实例更新会让 VSM 把这批实例的缓存页当"动了"作废，影子每帧重画。
 *     承诺了通知的源（FCSGpuInstanceSourceGPU::bNotifiesChanges，2026-10-02）只在被通知时重写：
 *     NotifyFeedChanged 记一笔，下一次帧末更新开头（或 Tick）补发 —— 与"代理新建"那条走的是同一个欠账口。
 *
 * 引擎硬性要求 CPU 端有实例数据的几条特性在这里一律关掉，而且**必须**关：Lumen 场景（card 表示）
 * 与距离场场景对 GPU-only 实例是 `check()`（LumenScene.cpp / DistanceFieldObjectManagement.cpp），
 * 不关就是断言崩溃，不是画面问题。光追不受影响：Nanite 代理的缓存光追实例从 GPU-Scene 取变换，
 * HIDDEN 的槽在建 TLAS 时被剔掉。
 */
UCLASS(MinimalAPI, ClassGroup = Rendering, NotBlueprintable, HideCategories = (Collision, Physics, Navigation, Cooking, HLOD, MeshPainting, Mobile, TextureStreaming))
class UCSGpuInstancedNaniteComponent : public UStaticMeshComponent
{
	GENERATED_BODY()

public:
	UCSGpuInstancedNaniteComponent(const FObjectInitializer& ObjectInitializer);

	/**
	 * 从本体抄"画什么、怎么画"：网格、材质、投影、可见性、自定义深度、剔除距离。有变化才标脏。
	 * 本体推一次（每次重建实例数据），替身每帧再拉一次 —— 生产者会直接改本体的 UPROPERTY
	 * （`InstanceMaterial = ...`、`SetCastShadow(...)`），那些路径上没有任何回调能通知到替身。
	 */
	void SyncFromOwner(const UCSGpuInstancedMeshComponent& Owner);

	/** 换实例源。槽数变了只能换代理（实例区间的大小是代理构造时定的），新代理由
	 *  CreateRenderState_Concurrent 负责写；槽数没变就当帧重发一次写入。 */
	void SetFeed(FCSGpuNaniteInstanceFeed&& InFeed, const FBox& InInstancesLocalBounds);

	/** 生产者原地改写了源（UCSGpuInstancedMeshComponent::NotifyInstanceSourceChanged 转过来的）：欠一次写入，渲染前补上。 */
	void NotifyFeedChanged() { bWritePending = true; }

	uint32 GetGpuSceneSlotCount() const { return NumGpuSceneSlots; }
	bool IsFeedingGpuSource() const { return Feed.IsGpu(); }

	/** 渲染线程上真正派发过几次写入（每次 = 一个 GPU-Scene 更新里的一个写入 pass）。 */
	uint32 DebugGetDispatchedWriteCount() const { return Stats.IsValid() ? Stats->DispatchedWrites.load() : 0u; }

	/**
	 * 「画面上的是不是我们以为的那一族」—— 空串 = 是。**阻塞**（会先把挂在帧末的渲染状态落地）。
	 * 答的是 CPU 能证明的那一半：代理存在、真走了 Nanite、写入发给了**当前**这个代理。写入 pass
	 * 有没有被渲染线程派发要等真的渲染过一帧才知道，见 DebugGetDispatchedWriteCount。
	 */
	FString DebugDescribeMismatchSync(const UCSGpuInstancedMeshComponent& Owner);

	//~ UActorComponent interface
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual bool RequiresGameThreadEndOfFrameUpdates() const override { return true; }
	virtual bool IsHLODRelevant() const override { return false; }

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual bool SupportsStaticLighting() const override { return false; }
	virtual bool IsNavigationRelevant() const override { return false; }

protected:
	//~ UActorComponent interface
	virtual void OnRegister() override;
	virtual void OnUnregister() override;
	virtual bool RequiresGameThreadEndOfFrameRecreate() const override { return true; }
	virtual void CreateRenderState_Concurrent(FRegisterComponentContext* Context) override;
	virtual void SendRenderTransform_Concurrent() override;
	virtual bool ShouldCreatePhysicsState() const override { return false; }

	//~ UStaticMeshComponent interface
	virtual FPrimitiveSceneProxy* CreateStaticMeshSceneProxy(Nanite::FMaterialAudit& NaniteMaterials, bool bCreateNanite) override;

private:
	/**
	 * 每次 SendAllEndOfFrameUpdates 开头（FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates）补发欠着的写入。
	 *
	 * 为什么非有不可：游戏线程的帧末更新把"渲染状态脏了"的静态网格组件交给
	 * FStaticMeshComponentBulkReregisterContext 批量重建（LevelTick.cpp 的 Update_GameThread）——
	 * 它先把旧代理全部摘掉，逐个跑 CreateRenderState_Concurrent 时**不建代理**，等整批跑完才在析构里
	 * BatchAddPrimitives。于是 CreateRenderState_Concurrent 那个钩子里 SceneProxy 还是空的，新代理
	 * 出生时整族 HIDDEN。帧末更新期间又不许再标脏（MarkActorComponentForNeededEndOfFrameUpdate 里
	 * check(!bPostTickComponentUpdate)），能接上的只剩下一次 SendAllEndOfFrameUpdates 的开头 ——
	 * 而渲染器在 BeginRenderingViewFamily 里、场景捕获在 CaptureScene 里都会先调一次，所以补写
	 * 仍然赶在这一帧渲染之前。实测症状（不补的话）：换容量那一帧整族消失，下一帧 Tick 才补回来。
	 */
	void HandlePreEndOfFrameUpdates(UWorld* InWorld);

	/** 当前代理还没收到过写入（新建的、或上次发写入时代理还不存在）。 */
	bool NeedsWrite() const;

	/** 距离带要不要在写入时抖（有过渡带、且至少有一道边界）。没有过渡带时引擎的硬边距离剔除就够了。 */
	bool IsDistanceBandActive() const { return BandFade > 0.0f && (BandStart > 0.0f || BandEnd > 0.0f); }

	/**
	 * 距离带用的视点（世界坐标）：本体设的覆盖视点 > 上一帧画过这个世界的透视视点里离上次最近的那个 >
	 * 上次用过的那个。一个都没有时返回 false（带内不抖，只剩引擎的硬边）。
	 */
	bool ResolveCullView(FVector& OutWorldLocation);

	/** 把 Feed 交给 GPU-Scene 写进当前代理的实例区间。游戏线程；没有代理时只记下"欠一次"。 */
	void IssueGpuSceneWrite();

	FCSGpuNaniteInstanceFeed Feed;

	/** 所有实例合起来的包围盒（本地空间 = 本体的组件空间，替身以恒等相对变换挂在本体下）。 */
	FBox InstancesLocalBounds = FBox(ForceInit);

	/** GPU-Scene 实例槽数，代理构造时读。 */
	uint32 NumGpuSceneSlots = 0;

	/**
	 * 交给引擎的**硬边**距离剔除（Nanite 代理的 InstanceDrawDistance）：End = 本体的 End；Min = 本体的
	 * `Start − Fade`（带的最外沿）。只有 End > 0 引擎才认 Min，所以只有起点的组件把 End 填成一个大数。
	 * 带内逐实例的抖动不归引擎（它的距离剔除没有过渡带），由写入 pass 做 —— 两者口径一致：引擎的硬边
	 * 恒在抖动边界之外，永远不会切掉写入 pass 留下的实例。
	 */
	int32 InstanceMinDrawDistance = 0;
	int32 InstanceEndCullDistance = 0;

	/** 本体的距离带原值（cm），每次写入时连同视点一起交给写入 pass。 */
	float BandStart = 0.0f;
	float BandEnd = 0.0f;
	float BandFade = 0.0f;

	/** ResolveCullView 的记忆：多视口时挑离它最近的视点，列表空着的那几次（帧末更新期间）沿用它。 */
	FVector LastCullView = FVector::ZeroVector;
	bool bHasLastCullView = false;
	/** 上一次带着距离带发出去的视点。会通知的源（CPU 行、承诺了通知的 GPU 源）只在它挪得够远时才重写；别的 GPU 源本来就每帧重写。 */
	FVector LastWrittenCullView = FVector::ZeroVector;
	bool bWroteWithBand = false;

	/** 每建一次渲染状态 +1。与 WrittenProxy 一起判"当前代理收没收到写入" —— 只比指针的话，
	 *  新代理恰好分配在旧代理的地址上就会被当成已经写过，整族 HIDDEN 而不报错。 */
	uint32 RenderStateSerial = 0;
	uint32 WrittenSerial = 0;
	const FPrimitiveSceneProxy* WrittenProxy = nullptr;
	std::atomic<bool> bWritePending{ false };

	/** "没走 Nanite、退到了引擎 ISM 代理" 只提醒一次。 */
	bool bWarnedNotNanite = false;

	FDelegateHandle PreEndOfFrameUpdatesHandle;

	/** 由写入委托（渲染线程）与诊断（游戏线程）共享，所以是引用计数的而不是成员。 */
	TSharedPtr<FCSGpuNaniteWriteStats, ESPMode::ThreadSafe> Stats;
};

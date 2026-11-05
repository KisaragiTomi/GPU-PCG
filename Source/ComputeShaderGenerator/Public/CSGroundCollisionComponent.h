#pragma once

#include "CoreMinimal.h"
#include "Components/PrimitiveComponent.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "CSGroundCollisionComponent.generated.h"

class UBodySetup;
struct FCSGroundMirror;

/**
 * `ACSGroundActor` 的物理碰撞：只有碰撞、不渲染（没有场景代理）。
 *
 * 地面画的是 gpumesh，本身全线 NoCollision —— 而编辑器的拖放放置、拖动预览、End 贴地全靠物理
 * 射线（`UE::Positioning::TraceWorldForPosition` = `LineTraceMultiByObjectType(AllObjects)`），
 * 打不到地面就把物体摆在相机前方的默认距离上。这个组件从 CPU 权威镜像烘一份三角网格碰撞补上。
 *
 * ⚠️ **不能用 Box / Sphere 之类的形状组件凑**：放置射线在游戏线程就把 `UShapeComponent` 的命中
 * 滤掉了（`ObjectPositioning.cpp::FilterHitsGameThread`）。而**没有场景代理**的图元组件会被
 * 保留 —— 渲染线程那道"视图里看不看得见"的过滤只对有代理的组件生效，所以这里刻意不建代理。
 *
 * 只在高度真的变了且**已提交**时重烘（地面全量重建、塑形物松手 / 改参），拖动途中不烘。刷顶点色
 * 不改高度，不触发。碰撞按组件局部坐标烘（= 镜像的相对高度），地面整体平移不必重烘，跟着 attach 走。
 *
 * **异步 cook**（照 `UProceduralMeshComponent` 的做法）：每次重烘新建一个 `UBodySetup` 在后台 cook，
 * 完成回调里才换上去 —— 换上之前旧碰撞照常生效，不会出现"一段时间没有碰撞"的空窗。重烘请求
 * 以最新为准：新请求来时把在途的那次取消（`AbortPhysicsMeshAsyncCreation`，取消的不会回调）。
 * 三角数据在发起 cook 的那一刻就在游戏线程上被取走，之后改本组件的数组不影响在途的 cook。
 *
 * 想关掉：细节面板里把碰撞预设改成 NoCollision。
 */
UCLASS(ClassGroup = (CSGround), meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSGroundCollisionComponent : public UPrimitiveComponent, public IInterface_CollisionDataProvider
{
	GENERATED_BODY()

public:
	UCSGroundCollisionComponent(const FObjectInitializer& ObjectInitializer);

	/**
	 * 按镜像发起一次异步重烘；镜像未就绪时当场清空碰撞。
	 * 每轴格数超过 `MaxQuadsPerAxis` 时按步长抽稀：最大的 1024² 地面会是 200 万三角，
	 * 抽到 256² ≈ 13 万三角，放置精度仍在一格（最多 2 m）之内。
	 */
	void RebuildFromMirror(const FCSGroundMirror& Mirror);

	/** 还有一次重烘在后台 cook（测试据此等它落地）。 */
	bool IsCookInFlight() const { return PendingBodySetup != nullptr; }

	static constexpr int32 MaxQuadsPerAxis = 256;

	//~ IInterface_CollisionDataProvider
	virtual bool GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool InUseAllTriData) override;
	virtual bool ContainsPhysicsTriMeshData(bool InUseAllTriData) const override;
	virtual bool WantsNegXTriMesh() override { return false; }

	//~ UPrimitiveComponent
	virtual UBodySetup* GetBodySetup() override { return CollisionBodySetup; }
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

private:
	void FinishAsyncCook(bool bSuccess, UBodySetup* FinishedBodySetup);

	/** 正在生效的碰撞。 */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> CollisionBodySetup;

	/** 正在后台 cook、完成后换上去的那一份。同一时刻至多一份（新请求会取消旧的）。 */
	UPROPERTY(Transient)
	TObjectPtr<UBodySetup> PendingBodySetup;
	FBox PendingLocalBounds = FBox(ForceInit);

	// 组件局部坐标（X/Y 从镜像原点起算、Z 为相对高度）。cook 时经 GetPhysicsTriMeshData 交出去。
	TArray<FVector3f> CollisionVertices;
	TArray<FTriIndices> CollisionIndices;
	FBox LocalBounds = FBox(ForceInit);
};

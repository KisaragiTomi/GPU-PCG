#pragma once

#include "CoreMinimal.h"
#include "CSGpuMeshComponent.h"
#include "CSGpuMeshTypes.h"
// 与 .usf 共用的布局常量（行 stride / custom data 步长 / LOD 上限），只含 #define。
#include "CSGpuSharedLayout.ush"
#include "CSGpuInstancedMeshComponent.generated.h"

class IConsoleVariable;
class UCSMesh;
class UCSGpuInstancedNaniteComponent;
class UStaticMesh;
class UMaterialInterface;

/**
 * 布局常量 `CS_GPU_INSTANCED_ROW_FLOAT4S` / `CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS` /
 * `CS_GPU_INSTANCED_MAX_LODS` 与 .usf 共用同一份 #define（CSGpuSharedLayout.ush），这里不再各写一份 ——
 * 两边各写一份的症状是错位而不是报错（2026-09-07 审查 B3）。
 *
 * ⚠️ custom data 的步长**是材质侧的读取步长**（引擎按 `Buffer[InstanceId * NumCustomDataFloats + i]` 取），
 * 改它要同时改所有消费它的材质，否则读到的是错位的邻居值 —— 数值看着"有点怪"，不报错、不断言。
 * 所以它是个编译期常量而不是逐组件的属性。当前语义（藤蔓 D13）：[0] = SpawnTime 秒、[1] = 弧长 cm。
 */

/** One LOD of the base mesh inside the shared GPU vertex/index buffers. */
struct FCSGpuInstancedLODRange
{
	uint32 FirstIndex = 0;  // into the shared index buffer
	uint32 NumIndices = 0;
	uint32 BaseVertex = 0;  // added to every index by DrawIndexedIndirect
	float ScreenSize = 1.0f; // switch to this LOD at or below this screen size (LOD0 is largest)
};

/**
 * 基础网格的一个材质段 = 一个 DrawIndexedIndirect。
 *
 * 同一级 LOD 的各段是那一级索引区间里连续、不重叠的几截，按 LOD 升序、段内按索引顺序排；它们共用那一级的
 * 可见实例区段与计数器（剔除只按 LOD 分区，与材质无关），只是各画各的索引区间、各用各的材质。
 * `MaterialIndex` 是材质槽号：资产路 = `UStaticMesh::GetStaticMaterials()` 的下标（`FStaticMeshSection::MaterialIndex`），
 * 外部快照路 = `FCSGpuMeshCPUData::Materials` 的下标（`TriangleMaterialSlots` 的取值）。
 */
struct FCSGpuInstancedSection
{
	int32 LodIndex = 0;
	uint32 FirstIndex = 0;  // into the shared index buffer (absolute, not relative to the LOD)
	uint32 NumIndices = 0;
	int32 MaterialIndex = 0;
};

/**
 * CPU snapshot of the base mesh. The proxy uploads it once into the GPU streams owned by
 * FCSGpuMeshSceneProxy; from then on the geometry is GPU-resident and only the per-instance
 * data changes. All LODs live in one vertex buffer and one index buffer, addressed by
 * FCSGpuInstancedLODRange.
 */
struct FCSGpuInstancedBaseMesh
{
	TArray<FVector3f> Positions;
	TArray<uint32> TangentBasis;  // 2 packed 8888 SNORM per vertex (TangentX, TangentZ)

	/**
	 * 每顶点 NumTexCoordSets 组 UV，**交错**排：第 V 个顶点的第 S 组在 `[V * NumTexCoordSets + S]`。
	 * 交错而不是按组分段，因为它原样上传进 UCSMesh 的 TexCoord 流 —— 那条流加宽的形态、引擎 manual fetch 的
	 * 取数 `VertexFetch_TexCoordBuffer[NumTexCoords * VertexId + CoordIndex]` 都是这个布局
	 * （见 FCSMeshStreamLayout::NumTexCoordSets）。
	 */
	TArray<FVector2f> TexCoords;
	TArray<uint32> Colors;        // 1 packed RGBA8 per vertex
	TArray<uint32> Indices;
	TArray<FCSGpuInstancedLODRange> LODs;

	/**
	 * 全部 LOD 的材质段，一段一个 draw（布局见 FCSGpuInstancedSection）。至多 CS_GPU_INSTANCED_MAX_DRAWS 段 ——
	 * 装不下时快照丢弃后面整级 LOD，LODs 与它同步截断，所以"第 i 段属于哪一级"永远查得到。
	 */
	TArray<FCSGpuInstancedSection> Sections;

	/**
	 * UV 组数，1..FCSGpuMeshCPUData::MaxTexCoordChannels（= MAX_STATIC_TEXCOORDS = 8），取自基础网格实际带的组数。
	 * 逐族决定 GPU 常驻 TexCoord 流有多宽：只有一组 UV 的网格（草、花、石阶）布局与改动前逐位相同，不为别人付显存。
	 * 2026-09-14 以前这里写死一组，TG 叶卡的树冠材质要读 UV1.xy / UV2.x（卡片中心）⇒ 树没法走实例路。
	 * 第 5..8 组只经 manual fetch 到达 shader（stream component 最多挂 4 个），见 CSGpuMeshSceneProxy.cpp 的 TexCoord 分支。
	 */
	int32 NumTexCoordSets = 1;

	/** Local bounds of LOD0, used as the per-instance culling sphere. */
	FBox LocalBounds = FBox(ForceInit);

	bool IsValid() const
	{
		return Positions.Num() >= 3 && Indices.Num() >= 3 && LODs.Num() > 0
			&& Sections.Num() > 0 && Sections.Num() <= CS_GPU_INSTANCED_MAX_DRAWS
			&& TangentBasis.Num() == Positions.Num() * 2
			&& NumTexCoordSets >= 1 && NumTexCoordSets <= FCSGpuMeshCPUData::MaxTexCoordChannels
			&& TexCoords.Num() == Positions.Num() * NumTexCoordSets
			&& Colors.Num() == Positions.Num();
	}

	void Reset()
	{
		Positions.Reset();
		TangentBasis.Reset();
		TexCoords.Reset();
		NumTexCoordSets = 1;
		Colors.Reset();
		Indices.Reset();
		LODs.Reset();
		Sections.Reset();
		LocalBounds = FBox(ForceInit);
	}
};

/**
 * GPU-produced instance source: a compute pass wrote the instances straight into GPU buffers and
 * the CPU never sees them. Layout matches the CPU path's packed source buffer — 5 float4 per
 * instance:
 *   [0..2] rows of the instance-to-component 3x3 (.w = 0)
 *   [3]    origin.xyz in component space, .w = per-instance random (0..1)
 *   [4]    culling sphere: centre.xyz in component space, .w = radius
 * Counter[0] holds the live instance count, so the count never round-trips to the CPU.
 *
 * ⚠️ **[3].w < 0 = 这个实例藏起来**（组件级契约，2026-09-15）：经典路的剔除 pass 跳过它，Nanite 路的
 * GPU-Scene 写入把它写成 HIDDEN，`SaveToStaticMesh` 不烘它。以前这条哨兵只有 `M_TinyGladeBrick` 的
 * OpacityMask 认（门框砖的转角墩剔除），组件改成默认画资产材质之后材质就不再可靠，契约因此下沉到组件。
 *
 * ⚠️ **一个视图族的渲染期间不许改写这几块 buffer**（2026-10-02，「可见源行号」布局的前提，见
 * FCSGpuInstancedGpuLayout::bVisibleSourceIndex）：那种布局下剔除只记"第几行可见"，这一帧后面的每个 pass
 * （预通道、基通道、阴影、速度）都是拿着行号回来读这里的行与 custom data。剔除之后、绘制之前把行原地重排 / 重散，
 * 画出来的就是"上一批的行号 + 这一批的行"。在渲染命令里改（游戏线程 Tick / 编辑操作发出的 ENQUEUE_RENDER_COMMAND，
 * 与视图族渲染是先后两条命令）是安全的，现有生产者全是这样；别在 view extension 的 PostRenderBasePass 之类
 * 帧内回调里改。点云源不受这条约束：它的行是剔除前现打进组件自己的常驻流里的快照。
 */
struct FCSGpuInstanceSourceGPU
{
	TRefCountPtr<FRDGPooledBuffer> PackedInstances; // Buffer<float4>, 5 per instance
	TRefCountPtr<FRDGPooledBuffer> Counter;         // Buffer<uint>, [0] = instance count
	/**
	 * Buffer<float>, CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS per instance。**可空** ——
	 * 只有需要逐实例 custom data 的生产者（藤蔓的叶 / 花）才填，别的留空即可，
	 * 剔除 pass 会给可见槽写零。刻意做成**并列缓冲**而不是把 packed 行加宽：
	 * 行 stride（CS_GPU_INSTANCED_ROW_FLOAT4S）是九条打包路 + 剔除 pass + GPU-Scene 写入 pass 共用的契约，
	 * 加宽它等于让每一家都改，代价与风险都不对等。
	 */
	TRefCountPtr<FRDGPooledBuffer> CustomData;
	uint32 Capacity = 0;                            // instances the buffer can hold
	FBox LocalBounds = FBox(ForceInit);             // conservative bounds of the whole scatter

	/**
	 * 生产者的承诺（Nanite 路用，InstanceCullingOptimization.md §7）：每次**原地**改写这几块 buffer（行 / 计数器 / custom data）
	 * 之后，都会调一次 `UCSGpuInstancedMeshComponent::NotifyInstanceSourceChanged()`（重新交接 SetInstanceSourceGPU 本身就算一次）。
	 *   true   Nanite 路只在被通知的那一帧重写 GPU-Scene：没改的帧不写，VSM 也不再每帧把这批实例的缓存页当"动了"作废；
	 *   false  （默认）照旧每帧整份重写 —— "生产者原地重写、从不通知"的老契约，生产者一个字都不用改。
	 * 承诺了却漏通知的后果：Nanite 路的画面停在上一次写入的样子，没有任何报错。所以它是逐源自愿的，不是组件上的总开关；
	 * 经 `CSShaperSteps::HandOverInstanceSources` 交接的生产者在那边统一开（FHandoverOptions::bNotifiesChanges）。
	 * 经典路不看它：剔除本来就每帧现读 buffer。
	 */
	bool bNotifiesChanges = false;

	bool IsValid() const { return PackedInstances.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FCSGpuInstanceSourceGPU(); }
};

/**
 * Point-cloud form of the GPU instance source: world-space positions + normals instead of packed
 * instance rows. The proxy builds the rows itself at the start of every cull, so a producer that
 * already owns a (position, normal) point buffer — the depth-sampling point brush — can drive the
 * instanced display without knowing the instance layout, and without a readback.
 *
 * Each point becomes one instance whose +Z is its normal, uniformly scaled by InstanceScale.
 * Counter[0] is the live point count, same contract as FCSGpuInstanceSourceGPU.
 */
struct FCSGpuInstancePointSourceGPU
{
	TRefCountPtr<FRDGPooledBuffer> Positions; // Buffer<float4>, xyz = world position
	TRefCountPtr<FRDGPooledBuffer> Normals;   // Buffer<float4>, xyz = world normal
	TRefCountPtr<FRDGPooledBuffer> Counter;   // Buffer<uint>, [0] = live count
	uint32 Capacity = 0;
	float InstanceScale = 1.0f;
	FBox WorldBounds = FBox(ForceInit);

	/** 同 FCSGpuInstanceSourceGPU::bNotifiesChanges：承诺每次原地改写点 / 法线 / 计数器之后都通知。
	 *  点刷不承诺（它在渲染线程的帧内回调里追加点，游戏线程不知道哪一帧加了几个），所以默认 false。 */
	bool bNotifiesChanges = false;

	bool IsValid() const { return Positions.IsValid() && Normals.IsValid() && Counter.IsValid() && Capacity > 0; }
	void Reset() { *this = FCSGpuInstancePointSourceGPU(); }
};

/**
 * What the GPU stream layout was built for.
 *
 * Derived once on the game thread whenever the base mesh or the instance set changes, then copied
 * wholesale into the scene proxy. It is a copy rather than a second derivation because
 * MaxInstancesPerLod is the *stride* of one LOD's region in the visible-instance buffers: a proxy
 * that culled with a different number than the one those buffers were sized from would compact
 * survivors past the end of a region, which is a device fault or silent garbage rather than an
 * error anybody can trace.
 */
struct FCSGpuInstancedGpuLayout
{
	/** LOD levels drawn. */
	uint32 NumLODs = 1;

	/** DrawIndexedIndirect arg sets = material sections over all drawn LODs (FCSGpuInstancedBaseMesh::Sections).
	 *  Part of the declared stream layout: a base mesh with a different section count re-declares it. */
	uint32 NumDraws = 1;

	/** Instances the source and visible buffers are sized for — the region stride, not the live
	 *  count. It ratchets with hysteresis (see UCSGpuInstancedMeshComponent::ResolveInstanceCapacity):
	 *  changing it reallocates six of the mesh's streams, and tracking the live count exactly would
	 *  do that on every single AddInstance. */
	uint32 InstanceCapacity = 0;

	/** Live rows in the source buffer. The GPU sources carry their own counter and leave this at the
	 *  capacity; the CPU array knows it exactly. */
	uint32 NumSourceInstances = 0;

	/** Coarse cull level. Zero means there is none — a GPU instance source has no cluster table, so
	 *  every instance goes straight through the fine cull. */
	uint32 NumClusters = 0;
	uint32 ClusterSize = 0;

	/**
	 * 可见缓冲的形态（InstanceCullingOptimization.md §5），建常驻缓冲时按 `r.CSGpuInstanced.VisibleSourceIndex` 定下来：
	 *   false  剔除把每个可见实例的完整属性抄进可见缓冲（88 B / 槽），绘制读那份副本（引擎的 LocalVertexFactory.ush）；
	 *   true   剔除只写 4 字节的源行号，绘制经它直接读源实例行（CSGpuInstancedIndexedVertexFactory.ush）。
	 * 是布局的一部分：两种形态声明的常驻流不同、代理用的顶点工厂不同，所以它跟着布局一起抄进代理，
	 * 代理不许自己再去读一遍 CVar（建缓冲与建代理之间 CVar 变了的话两边就对不上）。
	 */
	bool bVisibleSourceIndex = false;

	bool IsValid() const { return InstanceCapacity > 0 && NumLODs > 0; }
};

/**
 * CPU 实例数组里一个实例的**稳定句柄** —— InstanceCullingOptimization.md §3.1 的 StableInstanceID。
 *
 * 实例存活期间句柄不变：别的实例增删、`RemoveInstance` 把末尾实例换进空位、Morton 重排、可见槽与 LOD 变化都不动它。
 * 要跟着某一个实例走的状态（逐实例随机数、选中、交互记录、动画起点……）一律挂在句柄上，不要挂在数组下标上。
 *
 *   Slot        稳定槽号。实例活着就不变；实例删掉后槽回收，可以被之后新加的实例**复用**。
 *               取值稠密（不超过同时存活过的实例数峰值），可以直接当一张逐实例状态表的下标。
 *   Generation  这个槽被回收过几次。实例一删掉槽的代数就 +1，所以删掉之后旧句柄立刻失效，
 *               之后占了同一个槽的新实例是**另一个**实例（代数不同），旧句柄访问不到它。
 *
 * 作用域是**单个组件**：两个组件各有各的槽号，句柄不能跨组件比较，也不是全局唯一 ID。
 * 随组件存盘（槽表与代数表都是 UPROPERTY），所以存在别处的句柄跨存读盘仍然指向同一个实例、或者仍然是失效的。
 *
 * "删了再加回来"是**新实例**：哪怕位置一模一样，也拿到新的代数。身份不由位置隐式判定。
 * `SetInstances` / `ClearInstances` 是整套换掉：此前的句柄全部失效，新实例按给入顺序占槽 0..N-1。
 */
USTRUCT(BlueprintType)
struct FCSGpuInstanceHandle
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CS GPU Instanced Mesh|Instances")
	int32 Slot = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "CS GPU Instanced Mesh|Instances")
	int32 Generation = 0;

	FCSGpuInstanceHandle() = default;
	FCSGpuInstanceHandle(int32 InSlot, int32 InGeneration) : Slot(InSlot), Generation(InGeneration) {}

	/** 只说明"填过"，不说明"还活着" —— 后者问组件的 IsInstanceHandleValid。 */
	bool IsSet() const { return Slot != INDEX_NONE; }

	friend bool operator==(const FCSGpuInstanceHandle& A, const FCSGpuInstanceHandle& B) { return A.Slot == B.Slot && A.Generation == B.Generation; }
	friend bool operator!=(const FCSGpuInstanceHandle& A, const FCSGpuInstanceHandle& B) { return !(A == B); }
	friend uint32 GetTypeHash(const FCSGpuInstanceHandle& Handle) { return HashCombine(::GetTypeHash(Handle.Slot), ::GetTypeHash(Handle.Generation)); }
};

/**
 * HISM done on the GPU, on top of UCSGpuMeshComponent.
 *
 * One GPU-resident copy of the base mesh (all LODs concatenated) plus a per-instance transform
 * buffer. Every frame a compute pass culls a two-level hierarchy — clusters first, then the
 * instances inside surviving clusters — picks a LOD per instance from its screen size, compacts
 * the survivors into per-LOD regions of the visible buffer and writes one DrawIndexedIndirect arg
 * set per LOD. What a visible slot holds is the survivor's **source row index** (4 bytes): the
 * vertex shader looks it up by SV_InstanceID and reads the instance straight out of the source rows
 * (FCSGpuInstancedIndexedVertexFactory; default since 2026-10-02 — the older form, a full 88-byte
 * copy of the instance per visible slot drawn by FCSGpuInstancedMeshVertexFactory, is kept behind
 * `r.CSGpuInstanced.VisibleSourceIndex 0` as the comparison baseline). Geometry is stored once no
 * matter how many instances there are and the visible set never touches the CPU.
 *
 * Relative to UHierarchicalInstancedStaticMeshComponent:
 *   - the cluster tree is a flat Morton-ordered cluster list rebuilt on the game thread, and the
 *     culling/LOD decision itself runs on the GPU instead of on the game thread;
 *   - instances can come from the CPU (AddInstance & co, serialized like HISM's) or straight from
 *     a compute shader (SetInstanceSourceGPU) — the render path is the same either way;
 *   - no per-instance collision, no ray tracing, no static lighting, no per-instance custom data.
 *
 * The material must have "Used with Instanced Static Meshes" enabled, exactly as for HISM.
 *
 * Every buffer — the base mesh, the per-LOD indirect args, the instance source and the
 * visible-instance buffers the cull compacts into — lives in a UCSMesh this component owns, not in
 * its scene proxy. A render-state recreation is therefore a rebind: the base mesh and the packed
 * instance rows are uploaded once, when they change, instead of once per proxy. That is the whole
 * reason the mesh object exists here, since a proxy rebuild used to re-upload the entire base mesh
 * and the entire instance array.
 *
 * -----------------------------------------------------------------------------
 * 实例路的顶点色通道字典（**只管这条路上的基础网格**）
 * -----------------------------------------------------------------------------
 *   RGB = 基础网格资产自带的顶点色，原样透传（`M_TinyGladeDecor` 就靠它当反照率）
 *   A   = **逐实例随机 0..1**，语义名 `CSInstanceRandom`
 *
 * 这不是 `ACSHouseActor` 那本 P2 冻结字典的扩充，两者管的是**不同的网格**：那本管房体三角汤
 * （非实例路、`UCSMeshRenderComponent` 画），这份管实例路的基础网格；两条路上的资产没有一件
 * 是共用的，扩这一份不会动到那一份。
 *
 * 为什么非要新占一条通道 —— 裁决六第三句「材质不能依赖只有 gpumesh 代理才提供的逐图元数据」：
 * 烘成 StaticMesh 之后**没有实例了**，`PerInstanceRandom` 在 GPU-Scene 里恒等于 0
 * （非实例图元的 `FInstanceSceneData::RandomID` 就是 0），于是 `lerp(0.88, 1.12, 0)` 让整片
 * 石头/藤/摆件烘成**同一个色**，而且**一条断言都不会红** —— 本项目的经典失效形状。
 *
 * 两条路读同一个语义，靠的是「两者恰好互斥地为零」：
 *   · GPU 实例路：上传基础网格时把 A **清零**（就在 `RebuildBaseMeshSnapshot` /
 *     `SetBaseMeshFromGpuData` 里，各一处），随机数由 packed 行的 `Origin.w` 送到
 *     `PerInstanceRandom`；
 *   · 烘焙路：`PerInstanceRandom` 恒 0，随机数写进该实例**全部顶点**的 A。
 * ⇒ 材质两条路一律写 `CSInstanceRandom = PerInstanceRandom + VertexColor.A`，取值逐位相同，
 *    材质图里没有任何分支、也不需要两份材质实例。
 * ⚠️ 代价说清楚：**实例路的基础网格顶点色 alpha 从此不可用**（现有消费者只有
 *    `M_TinyGladeDecor`，它只读 RGB）。要用 alpha 做别的（叶片遮罩之类）得先改这份字典。
 *    默认改画资产材质之后（见下一节）这条也核过（2026-09-15）：TG 资产库的母材质 `M_TG_Texture` /
 *    `M_TG_VertexColor` / `M_TG_MeshProjected` / `M_TG_Glass` 都不读顶点色 alpha（`M_TG_Texture` 里读 A 的
 *    那个 Append 节点是悬空的，读 R 的那一支被 `RockShellCapSkirt = 0` 乘掉），所以不冲突。
 *
 * -----------------------------------------------------------------------------
 * 材质：逐 section，默认用资产自己的（2026-09-15）
 * -----------------------------------------------------------------------------
 * 基础网格有几个材质段就发几个 draw（FCSGpuInstancedSection，一段一个 DrawIndexedIndirect，同一级 LOD
 * 的各段共用那一级的剔除结果）。每段的材质按 `GetSectionMaterial` 解析：
 *   · `InstanceMaterial` 设了 ⇒ 它盖住**每一段**（整体覆盖，老用法逐像素不变）；
 *   · 空 ⇒ 这一段在基础网格资产上挂的那个材质槽（`SetBaseMeshFromGpuData` 喂的数据取它自带的 `Materials` 表）；
 *   · 还是空，或母材质没勾 `bUsedWithInstancedStaticMeshes` ⇒ 这一段画成引擎默认材质，并打一次警告。
 * 以前空 `InstanceMaterial` 一律画默认材质，于是用户资产上配好的材质在实例路上从来没生效过。
 * `GetMaterialUndrawableReason()` 是这条的非阻塞判据，`DebugGetDrawnAssetMismatchSync()` 再加一道着色器映射探针。
 *
 * -----------------------------------------------------------------------------
 * Nanite 路（BaseMesh 开了 Nanite 时自动走，没有开关）
 * -----------------------------------------------------------------------------
 * 判据只有一条：资产自己的 Nanite 设置（UStaticMesh::IsNaniteEnabled，含 r.Nanite.ForceEnableMeshes）。
 * 开了就走这条路，没开就走上面的 GPU 剔除路 —— 用户在资产上勾 Nanite 就是在说"这张网格该由 Nanite 画"，
 * 组件上再放一个开关只会让两处设置打架。编辑器里改资产的 Nanite 开关，资产重建完（OnPostMeshBuild）
 * 组件会自己重判一次（见 HandleBaseMeshRebuilt）。
 *
 * 同一套实例源，换一条渲染路：本组件不再上传基础网格、不再跑自己的剔除，而是挂一个渲染替身
 * UCSGpuInstancedNaniteComponent（一个 UStaticMeshComponent，网格就是 BaseMesh），引擎在 GPU-Scene
 * 里给它分一段**只在 GPU 上**的实例区间，由 CSGpuInstancedNaniteWriter.usf 把 packed 行直接写进去。
 * 之后剔除 / LOD / 光栅 / 阴影全归引擎的 Nanite 管线 —— 所以 VSM 有影子、主视锥外的实例照样投影，
 * 这两条经典路都做不到（见构造函数里那段实测）。
 *
 *   · 仍然有效：BaseMesh、InstanceMaterial（设了就盖住资产的每个材质槽，没设用资产自己的材质）、
 *     距离带三件（InstanceStart/EndCullDistance + InstanceCullFadeDistance，2026-09-29 起在 GPU-Scene 写入
 *     pass 里按逐实例随机数抖动，与经典路同一条带，见 SetDistanceCullViewOverride）、InstanceWPODisableDistance、
 *     投影 / 可见性等渲染开关（替身每帧从本体抄）、全部实例源 API、烘焙出口与两条诊断。
 *   · 不再生效：InstancesPerCluster / bGpuFrustumCulling / bGpuLODSelection / LODScreenSizeScale。
 *   · 影子想留在 CSM（比 VSM 软）时开 bClassicShadowOnNanitePath：主通道仍由 Nanite 替身画、替身不投影，
 *     本组件自己再挂一份只投影的经典代理 —— 那份进不了 VSM、只进 CSM 级联（见构造函数里那段机制）。
 *   · 资产开了 Nanite 但这台机器画不了 Nanite（r.Nanite 0、平台不支持、材质不被 Nanite 支持）时，
 *     替身由引擎 ISM 代理画资产的回退网格，仍然吃 GPU-Scene 实例；连 GPU-Scene 都没有的平台才退回 GPU 剔除路。
 *   · SetBaseMeshFromGpuData 喂进来的网格不是资产、没有 Nanite 设置，永远走 GPU 剔除路。
 *   · GPU 源每帧重写一遍（生产者原地重写 buffer、从不通知）⇒ VSM 每帧作废这批实例的影子缓存；
 *     嫌贵就把本组件的 ShadowCacheInvalidationBehavior 设成 Static（替身跟着抄）。
 *   ⚠️ 上面那份通道字典在这条路上**不成立**：顶点色是资产自己的数据，alpha 没法清零，材质里的
 *     `PerInstanceRandom + VertexColor.A` 会整体多出资产的 alpha（通常是 1，整族亮一档，不报错）。
 *     用这条等式的材质配开了 Nanite 的资产时，资产的顶点色 alpha 必须是 0。
 *
 * -----------------------------------------------------------------------------
 * 实例的四种"编号"（InstanceCullingOptimization.md §3）—— 只有第一种能拿来认实例
 * -----------------------------------------------------------------------------
 *   · 稳定句柄 `FCSGpuInstanceHandle`：逻辑身份，实例活着就不变。CPU 实例数组的每个实例都有（见 GetInstanceHandle）。
 *   · 数组下标（AddInstance 返回的、RemoveInstance / UpdateInstanceTransform 收的那个 int32）：
 *     `PerInstanceTransforms` 的当前位置。删除是把**末尾**实例换进空位，被换的那个下标就变了。
 *   · 源行号 SourceIndex：packed 行（`PackedInstances` / GPU 源的 buffer）里的位置，剔除与 GPU-Scene 写入按它取数。
 *     CPU 源是 Morton 序，任何一次增删都可能整体重排（包围盒一变编码的归一化范围就变）；
 *     `GetPackedInstanceSlots()` 给出"第几行是哪个槽"。GPU 源的行号归生产者管：只改计数器、不搬行，行号就不变。
 *   · 可见槽 VisibleIndex（SV_InstanceID + 区段起点）与 GPU-Scene 地址（Nanite 路的 `InstanceSceneDataOffset + 行号`）：
 *     前者由原子计数压出来，同一帧内线程到达顺序都不固定；后者随代理重建换区间。都只在当帧内部自洽。
 *
 * 逐实例随机数（packed 行 `Origin.w`、材质里的 `PerInstanceRandom`）：CPU 源按**稳定槽**取（2026-10-02 以前按数组下标取，
 * 中间删一个实例，被换进空位的那个实例随机数就变了）。从没删过实例的内容槽号 == 下标，数值与改动前逐位相同。
 * GPU 源的随机数由生产者写进行里：要它跨重排 / 重新生成保持稳定，生产者得用稳定的生成键（格子坐标、路径参数……）取，
 * 不能用本帧的行号 —— 地被、石阶、门框砖、瓦、藤都是按生成键取的；点云源（PackPointInstancesCS）按行号取，
 * 只在生产者只追加、不搬行时稳定（点刷就是这样）。
 *
 * The one thing that does NOT go through UCSMesh::EditMeshSync is the per-frame cull, which has to
 * run inside the renderer's own graph and can neither build a graph of its own nor block on a
 * flush. It uses the mesh's other sanctioned entry point instead — FCSMeshRenderThreadEdit, scoped
 * around the passes in FCSGpuInstancedMeshSceneProxy::RunCulling — so the resident streams are
 * registered and restored by the same code the game-thread path uses rather than by a second copy
 * of the rule kept in step by hand.
 */
UCLASS(ClassGroup = Rendering, meta = (BlueprintSpawnableComponent))
class COMPUTESHADERGENERATOR_API UCSGpuInstancedMeshComponent : public UCSGpuMeshComponent
{
	GENERATED_BODY()

public:
	UCSGpuInstancedMeshComponent();

	// -------------------------------------------------------------------------
	// Base mesh
	// -------------------------------------------------------------------------

	/** Mesh instanced by this component. Its LODs (up to CS_GPU_INSTANCED_MAX_LODS) become the
	 *  GPU LOD levels, using the asset's own screen sizes. If the asset has Nanite enabled, the
	 *  engine draws the asset itself instead (its Nanite data) and the CPU-side LODs are only read
	 *  for the bake — see "Nanite 路" in the class comment. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "CS GPU Instanced Mesh")
	TObjectPtr<UStaticMesh> BaseMesh;

	/** 整体覆盖材质：设了就盖住基础网格的**每一个**材质段；**留空 = 每段用基础网格资产那个材质槽上挂的材质**
	 *  （两条渲染路同一个意思，见类注释「材质」一节）。母材质必须勾 bUsedWithInstancedStaticMeshes
	 *  （Nanite 路是 bUsedWithNanite），否则那一段被引擎换成默认材质。
	 *  运行时换它请走 SetInstanceMaterial —— 经典路的代理在构造时就把材质抄走了，直接写属性看不出变化。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh")
	TObjectPtr<UMaterialInterface> InstanceMaterial;

	/** 换整体覆盖材质。变了才写、变了才让渲染状态重建（经典路的代理构造时抄走材质）；传空 = 退回资产材质。 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh")
	void SetInstanceMaterial(UMaterialInterface* InMaterial);

	/** 快照里有几个材质段（= 经典路发几个 draw）。Nanite 路没读顶点时为 0 —— 那条路按资产的材质槽画。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh")
	int32 GetNumSections() const { return BaseMeshSnapshot.Sections.Num(); }

	/** 第 SectionIndex 段实际用哪张材质画（整体覆盖 > 资产 / 快照材质槽）。可能为空 —— 空段画成引擎默认材质。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh")
	UMaterialInterface* GetSectionMaterial(int32 SectionIndex) const;

	/** 材质槽 SlotIndex 解析出来的材质（整体覆盖 > 资产 / 快照材质表）。可能为空。 */
	UMaterialInterface* ResolveSlotMaterial(int32 SlotIndex) const;

	/**
	 * 这一族画的每个材质段，材质能不能真的画出来。空串 = 能；原因串带段号、槽号与材质来源（覆盖 / 资产）。
	 *
	 * **不阻塞**，只查 CPU 看得见的两条：解析出来的材质非空、母材质勾了这条渲染路要的用途标记
	 * （经典路 bUsedWithInstancedStaticMeshes、Nanite 路 bUsedWithNanite）。缺哪条引擎都会静默换成默认材质，
	 * 画面一片灰而 readback 断言照绿。"着色器到底编没编出来"要阻塞探针，见 DebugGetDrawnAssetMismatchSync。
	 * 各 actor 的 `Get*UndrawableReason` 查材质都走这一条，不再各自只看 `InstanceMaterial`。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Diagnostics")
	FString GetMaterialUndrawableReason() const;

	/** 这一族是不是交给引擎的 Nanite 管线画：BaseMesh 是一张开了 Nanite 的资产，且平台有 GPU-Scene。
	 *  不是开关，是判据 —— 想换路就去资产上勾 / 取消 Nanite。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh")
	bool IsNaniteRenderPath() const;

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh")
	void SetBaseMesh(UStaticMesh* InMesh);

	/** Feed a GPU-generated mesh (e.g. a readback from another UCSGpuMeshComponent) as the single
	 *  LOD0 base mesh instead of a UStaticMesh. Positions are taken as component-local.
	 *  材质段取自 `TriangleMaterialSlots`（同槽的三角按槽号归到一起，一槽一段；空 = 整张一段、槽 0），
	 *  材质槽取自 `Materials` —— `CSHouseVine::BuildBaseMesh` 从资产 LOD0 把这两样一起抄过来，
	 *  所以摆件走这条路也照样画资产自己的材质。 */
	void SetBaseMeshFromGpuData(const FCSGpuMeshCPUData& InMeshData);

	// -------------------------------------------------------------------------
	// Instances — CPU source (HISM-shaped API, transforms are component-local)
	//
	// Every one of these repacks the whole instance array and re-uploads it, which now includes a
	// blocking render flush. That was always the shape of this API (the sort alone is O(N log N) per
	// call), but the flush makes the difference visible: use AddInstances / SetInstances for more
	// than a handful, or wrap any run of edits in BeginInstanceUpdate / EndInstanceUpdate below
	// (one repack and one upload for the whole run, whatever mix of adds, removes and moves it is).
	//
	// ⚠️ 这一组收 / 返回的 int32 是**数组下标**，不是身份：RemoveInstance 把末尾实例换进空位，被换的那个实例
	// 下标就变了（与 UInstancedStaticMeshComponent 同一个契约）。要长期认一个实例，用下面"稳定句柄"那一组。
	// -------------------------------------------------------------------------

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	int32 AddInstance(const FTransform& InstanceTransform, bool bWorldSpace = false);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	TArray<int32> AddInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace = false);

	/** Removes by swapping the last instance into the hole, so indices after InstanceIndex are
	 *  not stable — same contract as UInstancedStaticMeshComponent::RemoveInstance. */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool RemoveInstance(int32 InstanceIndex);

	/** bMarkRenderStateDirty=false is the batching form: the CPU-side instance array is updated but
	 *  neither the packed rows, the GPU buffers nor the render state are, so a run of edits costs one
	 *  sort and one upload instead of one per edit. The display keeps showing the previous set until a
	 *  call that does update (any other mutator, this one with the flag set, or EndInstanceUpdate) lands.
	 *  （2026-10-02 以前 false 仍然当场整份重排打包、只省上传：10 万个实例连改 8 个要 82 ms，全花在那 8 次排序上。） */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace = false, bool bMarkRenderStateDirty = true);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace = false) const;

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void ClearInstances();

	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	int32 GetInstanceCount() const { return PerInstanceTransforms.Num(); }

	/** Replace the whole instance set in one go — one proxy rebuild instead of N.
	 *  整套换掉：此前的稳定句柄全部失效，新实例按给入顺序占槽 0..N-1（所以同一份输入每次得到同一批逐实例随机数）。 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void SetInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace = false);

	/**
	 * 成批改实例（InstanceCullingOptimization.md §6.1）。Begin 与**最外层** End 之间，这一组 CPU 实例接口
	 * （AddInstance(s) / RemoveInstance(s) / UpdateInstanceTransform / SetInstances / ClearInstances，含按句柄的那几个）
	 * 只改 CPU 侧的实例数组与身份表：不重排、不打包、不上传、不重建渲染状态；最外层 End 时统一做**一次**。可嵌套。
	 *
	 * 区间内：
	 *   · 下标与句柄当场有效 —— AddInstance 的返回值、GetInstanceHandle、按句柄的查 / 改 / 删都照常；
	 *   · 画面、GPU 上的缓冲、GetGpuLayout() 与包围盒停在区间之前；
	 *   · GetPackedInstances() / GetClusterBounds() / GetPackedInstanceSlots() 读的时候当场补一次打包（只是 CPU 那一半，
	 *     所以在区间里每改一个就读一次等于没合批）。
	 * 忘了 End 的后果是这一族的画面再也不更新（没有任何报错）—— C++ 里用 FCSGpuInstancedUpdateScope。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void BeginInstanceUpdate();

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	void EndInstanceUpdate();

	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	bool IsInInstanceUpdate() const { return InstanceUpdateDepth > 0; }

	/** 一次删一批。下标都是**调用这一刻**的下标（内部按从大到小删，前面的删除不会让后面的下标指到别的实例）；
	 *  重复的、越界的跳过。返回实际删掉几个。整批一次重排、一次上传。 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	int32 RemoveInstances(const TArray<int32>& InstanceIndices);

	/** packed 行重排打包过几次（只增不减）。诊断 / 测试用：一批改动该是 +1，不是每个改动 +1。 */
	int32 GetInstancePackSerial() const { return InstancePackSerial; }

	// -------------------------------------------------------------------------
	// Instances — 稳定句柄（CPU 实例源；语义见 FCSGpuInstanceHandle 与类注释「实例的四种编号」）
	//
	// 新增能力，不改上面那组按下标的接口的语义。句柄随实例一生不变，查询是 O(1)。
	// -------------------------------------------------------------------------

	/** 下标 InstanceIndex 上**此刻**那个实例的稳定句柄。下标越界返回未填的句柄（IsSet() 为假）。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	FCSGpuInstanceHandle GetInstanceHandle(int32 InstanceIndex) const;

	/** 句柄指的实例现在在哪个数组下标上；实例已删（或句柄属于被 SetInstances / ClearInstances 换掉的那一批）返回 INDEX_NONE。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	int32 GetInstanceIndexFromHandle(FCSGpuInstanceHandle Handle) const;

	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Instances")
	bool IsInstanceHandleValid(FCSGpuInstanceHandle Handle) const { return GetInstanceIndexFromHandle(Handle) != INDEX_NONE; }

	/** 按句柄删。句柄已失效返回 false、什么都不动 —— 不会误删后来占了同一个槽的实例。 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool RemoveInstanceByHandle(FCSGpuInstanceHandle Handle);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool UpdateInstanceTransformByHandle(FCSGpuInstanceHandle Handle, const FTransform& NewInstanceTransform, bool bWorldSpace = false, bool bMarkRenderStateDirty = true);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Instances")
	bool GetInstanceTransformByHandle(FCSGpuInstanceHandle Handle, FTransform& OutInstanceTransform, bool bWorldSpace = false) const;

	/**
	 * packed 行（`GetPackedInstances()`，Morton 序）的第 r 行是哪个稳定槽 —— SourceIndex → 稳定身份的那张表。
	 * 只有 CPU 实例源有；GPU 源时为空（行归生产者管，身份也归它）。与 PackedInstances 同时重建，长度 = 行数。
	 * 剔除 / GPU-Scene 写入回读出来的是源行号，拿这张表换成槽号才能跨帧、跨重排比较"是不是同一个实例"。
	 */
	const TArray<int32>& GetPackedInstanceSlots() const { EnsureInstancePacking(); return PackedRowSlots; }

	// -------------------------------------------------------------------------
	// Instances — GPU source
	// -------------------------------------------------------------------------

	/** Draw from compute-written instance buffers instead of the CPU array. While a GPU source is
	 *  set the CPU array is ignored (cluster culling is skipped — the source has no cluster table —
	 *  and per-instance frustum/distance culling and LOD selection still run). */
	void SetInstanceSourceGPU(const FCSGpuInstanceSourceGPU& InSource);

	/** Same, but from a GPU point cloud: the proxy turns each point into an instance whose +Z is
	 *  the point normal. Mutually exclusive with SetInstanceSourceGPU. */
	void SetInstanceSourceFromPoints(const FCSGpuInstancePointSourceGPU& InSource);

	void ClearInstanceSourceGPU();
	bool HasInstanceSourceGPU() const { return GpuInstanceSource.IsValid() || GpuPointSource.IsValid(); }

	/**
	 * 生产者刚**原地**改写了 GPU 实例源（行 / 计数器 / custom data，或点云源的点）。不阻塞。
	 * 只对承诺了 `bNotifiesChanges` 的源有意义：Nanite 路据此在下一次渲染之前重写 GPU-Scene。没承诺的源本来每帧都重写，
	 * 经典路每帧现读 buffer —— 那两种情形调不调都一样，所以生产者可以无条件调。
	 */
	void NotifyInstanceSourceChanged();

	// -------------------------------------------------------------------------
	// 诊断 / 验收（**阻塞**，绝对不许出现在交互路径上）
	//
	// 存在的理由：本项目三次栽在"CPU 侧断言全绿、画面是错的"上，而三次都是靠人看图才发现。
	// 其中最难看见的一类是**陈旧的 GPU 计数器** —— 生产者把实例数记成 0 了，而 GPU 上那个
	// 被剔除 pass 消费的 Counter 还留着上一代的值，于是画面上东西还立着，所有无头断言照绿。
	// 下面这两条是那一类 bug 唯一的无头判据：一条读 GPU 上的真值，一条问"画的是不是我们
	// 以为的那张网格 / 那份材质"。
	//
	// ⚠️ 两条都会阻塞（回读就是阻塞），且都走 `UCSMesh::CountedBlockingFlush()` 让计数器
	// 数到它们 —— 十一条 `flushes=0` 断言因此会在它们被误用到交互路径上时**立刻报红**，
	// 这正是想要的结果。名字里的 `Debug...Sync` / `Diagnostics` 分类就是那道警戒线。
	// -------------------------------------------------------------------------

	/**
	 * 回读 GPU 上那个**真正被剔除 pass 与 indirect draw 消费**的实例计数器。
	 *
	 * 不是 CPU 侧的镜像、也不是生产者记的期望值 —— 读的就是
	 * `FCSGpuInstancedMeshSceneProxy::RunCulling` 绑成 `InstanceCount` 的那个 buffer。
	 * 「CPU 说 0、GPU 说 12」这类 bug 只有拿这个数才比得出来。
	 *
	 * 返回 −1 表示**读不到**（没分配 / 回读没完成），与"真的是 0 个实例"区分开：
	 * 把读失败当 0 会让守着"擦掉之后必须归零"的断言在管线坏掉时假绿。
	 * CPU 实例数组那条路上没有 GPU 计数器，剔除 pass 用的是常量 `NumSourceInstances`，
	 * 这里如实返回它（那也确实是 GPU 那一侧看到的数）。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Diagnostics", meta = (DevelopmentOnly))
	int32 DebugReadDrawnInstanceCountSync() const;

	/**
	 * GPU 上画的基础网格 / 材质，是不是我们以为的那两样。空串 = 是。
	 *
	 * `IsXxxDrawable` 那一族只查"非空"，查不到**引擎有没有静默换掉它**：母材质没勾
	 * `bUsedWithInstancedStaticMeshes` 时实例路径会退回默认材质，症状与"没绑材质"逐像素
	 * 相同；基础网格那一半更隐蔽 —— 换掉 `BaseMesh` 资产时组件不一定重建，画面上还是旧网格。
	 * 所以这里把上传到 GPU 的那份网格**回读出来**跟资产对，而不是信任 CPU 侧的快照。
	 *
	 * ⚠️ 材质那一半只做得到"引擎会不会换"，做不到"这一帧画出来的像素用的是哪份着色器" ——
	 * 后者要有真的一帧渲染才存在。判不了的情形会在原因串里说明白，不会假装成通过。
	 *
	 * Nanite 路问的是另一组问题（替身在不在、代理是不是真 Nanite、GPU-Scene 写入发没发给当前代理），
	 * 见 UCSGpuInstancedNaniteComponent::DebugDescribeMismatchSync。
	 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh|Diagnostics", meta = (DevelopmentOnly))
	FString DebugGetDrawnAssetMismatchSync() const;

	// -------------------------------------------------------------------------
	// 烘焙出口（裁决六 ①）—— **离线、阻塞，绝不许出现在交互路径上**
	// -------------------------------------------------------------------------

	/**
	 * 把这一族实例展开成一张 StaticMesh 资产。
	 *
	 * 为什么必须有：裁决六 ① 要求每一类 GPU 生成物都有一条走得通的 `SaveToStaticMesh`，
	 * 而实例路此前**一个出口都没有** —— 变换只活在 GPU buffer 里，网格路那条
	 * （`UCSMeshRenderComponent::SaveToStaticMesh`）读的是常驻三角流，这里根本没有那种流：
	 * 常驻的只有**一份**基础网格 + 一张逐实例变换表。所以这条出口做的是**展开**：
	 * 回读实例行 → 每个实例把 LOD0 的顶点乘上自己的变换 → 拼成一张三角汤 → 走
	 * `UCSGpuMeshComponent::BuildStaticMesh`（与网格路同一条落盘实现，材质槽/多组 UV/
	 * 顶点色的装配规则因此不会两条路各写一份）。
	 *
	 * 阻塞（回读 + StaticMesh 构建）是**有意**的：这是用户主动发起的离线操作。它同样会被
	 * `UCSMesh::GetBlockingFlushCount()` 数到，于是十一条 `flushes=0` 断言会在它被误接到
	 * 交互路径上的那一刻报红 —— 与 `Debug...Sync` 那一族同一道警戒线。
	 *
	 * BakeSpace：烘出来的局部空间。实例原点是**组件空间**的，这里先乘组件变换升到世界，
	 * 再由落盘层烘回 BakeSpace 的局部 —— 与岩壳/道路那两条出口同一口径（传 actor 变换即可）。
	 *
	 * ⚠️ 覆盖面：GPU 实例源（`SetInstanceSourceGPU`，门框砖/接缝砖/藤/摆件/石阶走这条）与
	 * CPU 实例数组都能烘。**点云源（`SetInstanceSourceFromPoints`）烘不出来** —— 它的实例行
	 * 是代理每帧在渲染图里现打的（`PackPointInstancesCS` 写进 aux 槽），组件手上只有点位与法线，
	 * 没有渲染过就压根不存在那批行。要么在 CPU 上把那段打包数学再写一遍（两份会分叉），
	 * 要么给它一条离线 pass —— 都超出本条的范围，故如实返回 nullptr 并落一条日志。
	 */
#if WITH_EDITOR
	UStaticMesh* SaveToStaticMesh(const FTransform& BakeSpace, const FString& AssetPathAndName = TEXT(""),
		bool bReplaceExistingAsset = true, bool bSaveAsset = false, bool bEnableNanite = false);
#endif

	/**
	 * 烘焙时喂给每个实例的随机数。GPU 源上它就是 packed 行里 `Origin.w`（也就是画面上
	 * `PerInstanceRandom` 读到的那个值），CPU 源上是按插入序算的同一个哈希。
	 *
	 * 暴露出来只为一件事：让断言能拿"烘出来的顶点色 A"和"GPU 上那一行的 .w"逐位对，
	 * 而不是各算各的。返回空 = 读不到（与"真的是 0 个实例"区分开）。
	 */
	bool DebugReadInstanceRandomsSync(TArray<float>& OutRandoms) const;

	/**
	 * 回读**上一次剔除**压出来的逐 LOD 可见实例数（`LodCounters` 流：`InstanceCullCS` 的压缩游标）。
	 * **阻塞**，与上面几条同一道警戒线 —— 只给诊断、测试与性能基线（InstanceCullingOptimization.md §4.1）用。
	 *
	 * 读到的是最近跑过的那一次剔除：哪个视图由剔除的 view extension 决定（一个 view family 只剔第一个视图），
	 * 所以要先真渲染过一帧。OutPerLod 的长度 = 布局的 LOD 数，每项按容量钳过（游标是先加后判的）。
	 * 返回 false = **读不到**（没有经典路的常驻缓冲 —— 纯 Nanite 路没有这份计数 —— 或回读没完成），
	 * 与"真的一个都没看见"（返回 true、全 0）分开。
	 */
	bool DebugReadVisibleCountsSync(TArray<int32>& OutPerLod) const;

	/**
	 * 「可见源行号」布局（`GetGpuLayout().bVisibleSourceIndex`）下，回读**上一次剔除**压进可见缓冲的源行号：
	 * 逐 LOD 一张表，长度 = 那一级的可见数。**阻塞**，诊断 / 测试专用。
	 *
	 * 表里的次序没有意义（槽位是原子计数分的，同一帧里线程到达顺序都不固定），当集合用。
	 * 行号是**当前**源行号，不是身份：CPU 实例源拿 `GetPackedInstanceSlots()[行号]` 换成稳定槽之后，才能跨帧、
	 * 跨 Morton 重排比较"看见的是不是同一批实例"。返回 false = 读不到（不是这种布局 / 没分配 / 回读没完成）。
	 */
	bool DebugReadVisibleSourceIndicesSync(TArray<TArray<int32>>& OutPerLod) const;

	/** `r.CSGpuInstanced.VisibleSourceIndex` 变了：布局与它不一致的已注册组件就地重建。只由那个 CVar 的回调调。 */
	static void HandleVisibleSourceIndexCVarChanged(IConsoleVariable* Variable);

	// -------------------------------------------------------------------------
	// Culling / LOD
	// -------------------------------------------------------------------------

	/** Instances per cluster in the coarse cull level. Larger clusters make the cluster pass
	 *  cheaper but reject less. Only used by the CPU instance source. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "1", ClampMax = "4096"))
	int32 InstancesPerCluster = 64;

	/** Instances farther than this from the view are dropped. 0 disables the distance cull. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0"))
	float InstanceEndCullDistance = 0.0f;

	/**
	 * 离视点**近于**这个距离的实例不画（cm，0 = 不限）—— 与 `InstanceEndCullDistance` 配成一条距离带。
	 * 地被的近 / 远两档就是两个组件各拿一半：近处单根草 `End = B`、远处组合草 `Start = B`（2026-09-22）。
	 * 两条路都认：经典路在剔除 pass 里逐视图判；Nanite 路在 GPU-Scene 写入 pass 里按**一个**视点判
	 * （引擎的 Nanite 距离剔除是硬边、没有过渡带，所以带内的抖动只能由我们在写入时做），视点见
	 * `SetDistanceCullViewOverride`。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0"))
	float InstanceStartCullDistance = 0.0f;

	/**
	 * 起止两道距离边界的**过渡带宽**（cm，0 = 硬边）。带内每个实例按自己的逐实例随机数（packed 行 `Origin.w`）
	 * 在 `[边界 − 带宽, 边界]` 里取一个点当自己的边界：近侧组件越往外越稀、远侧组件越往外越密，
	 * 两个组件用同一条带就互补，交界处不会出一道硬圈。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0"))
	float InstanceCullFadeDistance = 0.0f;

	/**
	 * Nanite 路：离视点远于这个距离（cm）的实例**不求 WPO**（0 = 一律求）。交给引擎 Nanite 的逐实例
	 * WPO 关闭距离 —— 关掉的实例还会退回固定功能光栅，省掉可编程光栅那一整套开销。
	 * 这是硬切：要看不出切点，材质自己得先在切点之前把 WPO 淡到 0（地被的 `M_TG_Grass` 按
	 * custom primitive data 里的淡出区间做，见 `ACSGroundActor`）。经典路不认这一条 —— 它的 WPO 全靠材质淡出。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0"))
	int32 InstanceWPODisableDistance = 0;

	/**
	 * Nanite 路上把**影子**留在经典路（2026-09-29 用户：花走 Nanite，但影子仍要 CSM 那份软度）。
	 *
	 * 开着且走 Nanite 路时：Nanite 替身只画主通道、不投影；本组件照经典路建常驻网格与剔除，但代理**只进阴影**
	 * （主通道 / 深度预通道都不画）。经典路的顶点工厂进不了 VSM，于是这份影子落进 CSM 级联 —— 与切 Nanite
	 * 之前逐位同一套投影。代价：实例多剔除一趟、多一份常驻网格（基础网格读的是资产的回退 LOD，编辑器外要开
	 * Allow CPU Access）；可见实例集仍按主视锥压，视锥外的实例不投影（与切换前相同）。
	 * 不走 Nanite 路时这一条什么都不做。
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Shadow")
	bool bClassicShadowOnNanitePath = false;

	/** 现在是不是「Nanite 画主通道 + 经典代理只投影」这种组合（判据，不是开关）。 */
	UFUNCTION(BlueprintPure, Category = "CS GPU Instanced Mesh")
	bool IsClassicShadowCopyActive() const;

	/** 代码里改 bClassicShadowOnNanitePath 走这里：变了才建 / 还那套经典缓冲并重建渲染状态（直接写属性不会触发）。 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh")
	void SetClassicShadowOnNanitePath(bool bEnable);

	/**
	 * Nanite 路的距离带用哪个视点判（世界坐标）。经典路逐视图用各自的视点，不看这里。
	 *
	 * 不设时替身每帧取「上一帧画过这个世界的透视视点」里离上次最近的那一个（多视口时不来回跳）；
	 * 一个都没有（无头、只有场景捕获）时带内不抖、只剩引擎的硬边距离剔除 —— 近处组件画到 End、
	 * 远处组件从 `Start − Fade` 起画，两档在带里重叠而不是露出一圈空地。
	 * 出图脚本用场景捕获时要显式设（场景捕获不往 `ViewLocationsRenderedLastFrame` 里填）。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Culling")
	void SetDistanceCullViewOverride(FVector WorldLocation);

	UFUNCTION(BlueprintCallable, Category = "CS GPU Instanced Mesh|Culling")
	void ClearDistanceCullViewOverride();

	/** 设了覆盖视点就填它并返回 true。 */
	bool GetDistanceCullViewOverride(FVector& OutWorldLocation) const;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling")
	bool bGpuFrustumCulling = true;

	/** Off pins every instance to LOD0. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling")
	bool bGpuLODSelection = true;

	/** Multiplies the source mesh's LOD screen sizes; > 1 keeps higher LODs longer. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS GPU Instanced Mesh|Culling", meta = (ClampMin = "0.01"))
	float LODScreenSizeScale = 1.0f;

	// -------------------------------------------------------------------------
	// Render-thread accessors (used by the scene proxy at creation time)
	// -------------------------------------------------------------------------

	const FCSGpuInstancedBaseMesh& GetBaseMeshSnapshot() const { return BaseMeshSnapshot; }
	const TArray<FVector4f>& GetPackedInstances() const { EnsureInstancePacking(); return PackedInstances; }
	const TArray<FVector4f>& GetClusterBounds() const { EnsureInstancePacking(); return ClusterBounds; }
	const FCSGpuInstanceSourceGPU& GetInstanceSourceGPU() const { return GpuInstanceSource; }
	const FCSGpuInstancePointSourceGPU& GetInstancePointSourceGPU() const { return GpuPointSource; }

	/** The buffer set the proxy binds and the cull writes. Null / unallocated until the component
	 *  has both a base mesh and instances to draw. */
	UCSMesh* GetGpuMesh() const { return InstancedGpuMesh; }

	/** The numbers the current buffer set was sized from. The proxy culls with these and must not
	 *  re-derive them — see FCSGpuInstancedGpuLayout. */
	const FCSGpuInstancedGpuLayout& GetGpuLayout() const { return GpuLayout; }

	/** Nanite 路的渲染替身（诊断 / 测试用）。BaseMesh 没开 Nanite 时为空。 */
	UCSGpuInstancedNaniteComponent* GetNaniteComponent() const { return NaniteComponent; }

	//~ UPrimitiveComponent interface
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	/** 每个材质槽解析出来的材质（与 UStaticMeshComponent 同口径：覆盖优先，否则资产槽）。 */
	virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;
	virtual int32 GetNumMaterials() const override;
	virtual UMaterialInterface* GetMaterial(int32 ElementIndex) const override;
	/** Builds the GPU mesh that the mutators skipped while the component was unregistered. This runs
	 *  before CreateRenderState_Concurrent, which is the whole point: the render state may be
	 *  created off the game thread during the end-of-frame update, where the build's render flush
	 *  would not be legal, so proxy creation is only ever allowed to read what already exists. */
	virtual void OnRegister() override;
	/** Nanite 路的替身跟本组件一起下线：本体都不在世界里了，替身还画着就是一族没人管的实例。 */
	virtual void OnUnregister() override;
	virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;

	//~ UObject interface
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	/** 撤销 / 重做把实例数组与槽表整套换回去了：槽 → 下标的缓存与 packed 行都要跟着重建。 */
	virtual void PostEditUndo() override;
#endif

protected:
	//~ UCSGpuMeshComponent interface
	/** 基座只认一张材质；多段时报第 0 段的（GetUsedMaterials 已被本类覆写成逐槽上报）。 */
	virtual UMaterialInterface* GetRenderMaterial() const override { return GetSectionMaterial(0); }
	/** Nanite 路上本组件没有代理（画的是替身；只投影的经典副本除外），ReadbackMeshSync 那次 static_cast 必须拦在前面。 */
	virtual bool IsGpuMeshProxyActive() const override { return !IsNaniteRenderPath() || IsClassicShadowCopyActive(); }

private:
	/**
	 * 取本组件这一族**活着**的 packed 实例行（5 float4/实例，布局见 FCSGpuInstanceSourceGPU）。
	 * **阻塞**（GPU 源要回读两次：先计数器、再按计数取行）。
	 *
	 * 返回 false = **读不到**，与"真的是 0 个实例"分开 —— 把读失败当空集会让烘焙出口在回读
	 * 管线自己坏掉时安静地产出一张空网格，而空网格与"这一族本来就没实例"在资产里长得一模一样。
	 */
	bool ReadLiveInstanceRowsSync(TArray<FVector4f>& OutRows) const;

	/** Re-extracts BaseMeshSnapshot from BaseMesh (or leaves an externally supplied snapshot
	 *  alone) and recreates the render state. */
	void RebuildBaseMeshSnapshot();

	/**
	 * Morton-sorts the instances into clusters, packs the GPU source layout, recomputes LocalBounds
	 * and hands the result to the GPU mesh. Every mutator ends here, so no path can repack the
	 * instances and forget to upload them — the two used to be separated by a proxy rebuild, and
	 * with a retained mesh nothing else would ever notice the omission.
	 *
	 * bRebuildGpuMesh=false does the CPU half only. (Batching no longer goes through it: a deferred edit
	 * just marks the packing dirty — see NotifyInstancesChanged — and nothing is sorted until somebody needs it.)
	 */
	void RebuildInstanceData(bool bRebuildGpuMesh = true);

	/** The CPU half: Morton sort, packed rows, cluster spheres, LocalBounds. */
	void RebuildInstancePacking();

	/** Declares the stream layout, sizes the mesh and uploads the base mesh + instance source.
	 *  Blocks (render flushes). Releases the mesh instead when there is nothing to draw. */
	void RebuildGpuMesh();

	/** Hands the GPU buffers back and forgets the layout. The live proxy keeps its own references
	 *  to the pooled buffers, so it goes on drawing correctly until its render state is recreated. */
	void ReleaseGpuMesh();

	/** Nanite 路：建（第一次）/ 同步 / 注册渲染替身，把当前实例源交给它。不阻塞。只在已注册时调。 */
	void UpdateNaniteComponent();

	/** 离开 Nanite 路（或本组件被销毁）：替身整个毁掉，不留一个没人管的图元。 */
	void ReleaseNaniteComponent();

	/** 一个实例的本地包围盒：快照有就用快照的，Nanite 路没读顶点时退到资产自己的包围盒。 */
	FBox GetBaseLocalBounds() const;

#if WITH_EDITOR
	/**
	 * 盯住 BaseMesh 的重建（OnPostMeshBuild）：在资产上勾 / 取消 Nanite、重导入、改 LOD 之后，重新判一次
	 * 该走哪条路，顺带把 GPU 剔除路的快照刷新 —— 以前换了资产内容组件不会知道，画面上一直是旧网格。
	 * 换网格时先解绑旧的，所以任何时刻只盯着当前这一张。
	 */
	void BindBaseMeshRebuildEvent();
	void UnbindBaseMeshRebuildEvent();

	/**
	 * 不在 OnPostMeshBuild 里当场重建：那一刻编译管理器还攥着"引用这张网格的组件"清单，紧接着要逐个
	 * 调 PostStaticMeshCompilation（StaticMeshCompiler.cpp），而重判可能正好把替身（也是其中之一）毁掉或新建。
	 * 所以只挂一个一次性的 OnWorldPreSendAllEndOfFrameUpdates，下一次帧末更新开头再做 —— 仍赶在这一帧渲染之前。
	 */
	void HandleBaseMeshRebuilt(UStaticMesh* RebuiltMesh);
	void HandleDeferredBaseMeshRefresh(UWorld* InWorld);

	TWeakObjectPtr<UStaticMesh> RebuildEventMesh;
	FDelegateHandle RebuildEventHandle;
	FDelegateHandle DeferredRefreshHandle;
#endif

	/** Instance-buffer capacity for a live count, with hysteresis. Grows to 1.5x when the count
	 *  passes what is held and shrinks only once three quarters of it are unused.
	 *
	 *  A change here no longer drags the base mesh through a reallocation — UCSMesh::ResizeStreamsSync
	 *  touches the instance-sized streams and nothing else — but it still throws away and re-clears
	 *  six buffers, which at large instance counts is tens of megabytes of visible-instance region
	 *  per call. So the ratchet stays; what it protects against just got much smaller. */
	uint32 ResolveInstanceCapacity(uint32 LiveInstanceCount) const;

	/** Instance transforms in component space, in insertion order. This is the serialized,
	 *  user-facing order; PackedInstances holds the same set in cluster order. */
	UPROPERTY()
	TArray<FTransform> PerInstanceTransforms;

	// -------------------------------------------------------------------------
	// 实例身份（FCSGpuInstanceHandle）。存盘的真值是下面两张表；SlotToIndex / FreeSlots 是能从它们重建的缓存。
	// -------------------------------------------------------------------------

	/** 与 PerInstanceTransforms 并列：下标 i 上那个实例的稳定槽。RemoveAtSwap 时两张表一起换。 */
	UPROPERTY()
	TArray<int32> PerInstanceSlots;

	/** 每个槽的代数，长度 = 槽数（同时存活过的实例数峰值，只增不减 —— 缩掉它旧句柄就可能撞上新实例）。 */
	UPROPERTY()
	TArray<int32> SlotGenerations;

	/** 槽 → 数组下标，INDEX_NONE = 空闲。缓存：不存盘，由 PerInstanceSlots 重建。 */
	TArray<int32> SlotToIndex;

	/** 空闲槽的栈。末尾是下一个要用的；整套重置后按降序压，所以只增不删的内容拿到的槽号恒等于下标。 */
	TArray<int32> FreeSlots;

	/** packed 行 → 稳定槽（Morton 序），见 GetPackedInstanceSlots。 */
	TArray<int32> PackedRowSlots;

	/** 缓存要不要整个重建：读盘、撤销、复制之后为真（那几条路直接换掉了 UPROPERTY，没经过任何 mutator）。 */
	bool bInstanceIdentityCacheDirty = true;

	/**
	 * 把缓存对齐到两张真值表；真值表自己不自洽（老资产没有槽表、槽号越界或重复）时先修：全部实例重新占槽 0..N-1，
	 * 旧槽的代数 +1（此前发出去的句柄一律失效 —— 修出来的身份不该冒充修之前的）。平时 O(1)，要重建时 O(N)。
	 * 逻辑上不改实例集，所以 const 的查询经 const_cast 调它（UPROPERTY 成员不能标 mutable）。
	 */
	void EnsureInstanceIdentity();

	/** 整套换掉实例集时的身份重置：在用的槽代数 +1，N 个新实例按顺序占槽 0..N-1。 */
	void ResetInstanceIdentity(int32 NumInstances);

	/** 给下标 InstanceIndex 上新加的实例发一个槽（先用回收的，没有再开新的）。PerInstanceSlots 必须正好缺这一项。 */
	void AllocateInstanceSlot(int32 InstanceIndex);

	// ---- 成批改实例（BeginInstanceUpdate / EndInstanceUpdate）----

	/** CPU 实例数组刚被改过。bApplyNow 且不在成批区间里 ⇒ 当场重排、打包、上传；否则只记脏，等下一次真要更新的调用。 */
	void NotifyInstancesChanged(bool bApplyNow = true);

	/** 把攒着的改动落下去：重排打包（如果还脏）→ 上传 → 渲染状态与包围盒。 */
	void ApplyInstanceChanges();

	/** packed 行 / 簇球 / 行表落后于实例数组时补一次打包（只是 CPU 那一半，不上传）。读这三张表之前调。 */
	void EnsureInstancePacking() const;

	/** BeginInstanceUpdate 的嵌套深度。> 0 时所有 CPU 实例改动只记脏。 */
	int32 InstanceUpdateDepth = 0;
	/** packed 行 / 簇球 / 行表落后于 PerInstanceTransforms（有改动还没重排打包）。 */
	bool bInstancePackingDirty = false;
	/** 有改动还没传到 GPU、渲染状态与包围盒也还没更新。 */
	bool bInstanceUploadPending = false;
	int32 InstancePackSerial = 0;

	/** Base mesh uploaded to the GPU. Filled from BaseMesh, or directly by
	 *  SetBaseMeshFromGpuData (in which case bBaseMeshIsExternal suppresses re-extraction). */
	FCSGpuInstancedBaseMesh BaseMeshSnapshot;
	bool bBaseMeshIsExternal = false;

	/**
	 * 外部快照（SetBaseMeshFromGpuData）的材质槽表，取自数据自带的 `Materials`。资产路恒空 —— 那条路每次
	 * 现读 `BaseMesh` 的材质槽，资产上改了槽不必等快照重建。UPROPERTY 只为压住 GC：快照是普通结构体，
	 * 代理拿的是裸指针。
	 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> BaseMeshMaterials;

	/** "某一段的材质不能用于实例化、退回默认材质"只警告一次（代理每次重建都会再判一遍）。 */
	bool bWarnedUnusableMaterial = false;

	// GPU source layout, cluster order. 5 float4 per instance — see FCSGpuInstanceSourceGPU.
	// Clusters are fixed-size runs of this array, so the cull shader derives an instance's cluster
	// arithmetically and no explicit range table is needed.
	TArray<FVector4f> PackedInstances;
	TArray<FVector4f> ClusterBounds; // centre.xyz + radius per cluster

	FCSGpuInstanceSourceGPU GpuInstanceSource;
	FCSGpuInstancePointSourceGPU GpuPointSource;

	/** The retained buffer set. Transient because GPU data does not survive a level reload; the
	 *  property exists to hold the object against GC, not to serialize it.
	 *
	 *  No OnMeshChanged subscription, unlike UCSMeshRenderComponent: this component is the only
	 *  thing that ever edits this mesh, so it already knows when to recreate its render state.
	 *  Subscribing would only re-enter the rebuild it is itself in the middle of. */
	UPROPERTY(Transient)
	TObjectPtr<UCSMesh> InstancedGpuMesh;

	/** What InstancedGpuMesh's streams are currently sized for. Reset when the mesh is released, so
	 *  the ratchet does not survive the buffers it describes. */
	FCSGpuInstancedGpuLayout GpuLayout;

	/** Nanite 路的渲染替身。Outer 是本组件，Transient + DuplicateTransient：不存盘、PIE / 复制 actor
	 *  时不跟着拷，副本在自己的 OnRegister 里另建一个。BaseMesh 没开 Nanite 时恒为空。 */
	UPROPERTY(Transient, DuplicateTransient)
	TObjectPtr<UCSGpuInstancedNaniteComponent> NaniteComponent;

	/** SetDistanceCullViewOverride 设的视点（世界坐标）。不存盘：它描述的是这一刻的相机。 */
	TOptional<FVector> DistanceCullViewOverride;

	/**
	 * 有变更在未注册期间被跳过，重注册时必须重建一次。
	 *
	 * 踩过的坑：这条旗子以前不存在，未注册期的 RebuildGpuMesh 直接 ReleaseGpuMesh() 了事 ——
	 * 而 Release 自己就是一次阻塞刷新，还把常驻缓冲整套扔掉，于是 OnRegister 必须重新
	 * AllocateSync + EditMeshSync。编辑器里改一个 actor 属性就会 RerunConstructionScripts
	 * （卸载全部组件 → 跑构造脚本 → 重注册），所以**拖尺寸的每一帧都要付这 3 次阻塞刷新**，
	 * 而且与容量、包围盒、砖数全都无关，纯粹是这一轮卸载/重注册的开销（实测 12 帧 36 次）。
	 * 现在未注册期只记脏、什么都不动，重注册时按这面旗子决定要不要真重建。
	 */
	bool bGpuMeshDirty = false;
};

/**
 * BeginInstanceUpdate / EndInstanceUpdate 的作用域形态：构造时 Begin，析构时 End（提前 return、异常路径都不会漏）。
 * 组件为空时什么都不做。
 */
struct FCSGpuInstancedUpdateScope
{
	explicit FCSGpuInstancedUpdateScope(UCSGpuInstancedMeshComponent* InComponent)
		: Component(InComponent)
	{
		if (Component) Component->BeginInstanceUpdate();
	}

	~FCSGpuInstancedUpdateScope()
	{
		if (Component) Component->EndInstanceUpdate();
	}

	FCSGpuInstancedUpdateScope(const FCSGpuInstancedUpdateScope&) = delete;
	FCSGpuInstancedUpdateScope& operator=(const FCSGpuInstancedUpdateScope&) = delete;

private:
	UCSGpuInstancedMeshComponent* Component = nullptr;
};

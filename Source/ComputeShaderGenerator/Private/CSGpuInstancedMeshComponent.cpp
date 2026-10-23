#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h"
#include "CSGpuInstancedNaniteComponent.h"
// 诊断入口 DebugGetDrawnAssetMismatchSync 要问"材质为本顶点工厂编出着色器了没有"，
// 所以这里必须自己带上工厂类型与材质着色器映射那几个头 —— unity 构建下它们恰好被邻居带进来，
// 漏写只有 -SingleFile 才照得出来（坑表里那条）。
#include "CSGpuInstancedMeshVertexFactory.h"

#include "CSMesh.h"
#include "CSMeshOps.h"

#include "Algo/BinarySearch.h" // LowerBound：外部快照的材质槽 → 段号
#include "Engine/StaticMesh.h"
#include "Engine/World.h"     // FWorldDelegates：资产重建后的延迟重判
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialRenderProxy.h"
#include "Rendering/StaticMeshVertexBuffer.h"
#include "Rendering/PositionVertexBuffer.h"
#include "Rendering/ColorVertexBuffer.h"
#include "StaticMeshResources.h"
#include "Templates/Greater.h"       // TGreater：RemoveInstances 按下标从大到小删
#include "Templates/Sorting.h"       // RadixSort32：实例按 Morton 码排
#include "UObject/UObjectIterator.h" // TObjectIterator：CVar 变了之后逐个组件换布局
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderUtils.h"      // UseGPUScene：Nanite 路的平台前提
#include "RHI.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedMesh, Log, All);

// 经典剔除路的可见缓冲形态（InstanceCullingOptimization.md §5）。建常驻缓冲时读一次、写进 FCSGpuInstancedGpuLayout，
// 代理照布局走；改它的那一刻所有已注册的组件就地换布局（HandleVisibleSourceIndexCVarChanged），所以也能在运行中做 A/B。
//
// 默认 1（2026-10-02 起）：交错 A/B 16 个场景 × 5 个机位 × 3 对，常驻显存 −35% ～ −95%，帧时间持平或略好，
// 画面逐像素相同（Docs/InstanceCulling/VisibleSourceIndex_20261002.md）。0 留着当对照，不是给产品用的回退。
static TAutoConsoleVariable<int32> CVarCSGpuInstancedVisibleSourceIndex(
	TEXT("r.CSGpuInstanced.VisibleSourceIndex"),
	1,
	TEXT("UCSGpuInstancedMeshComponent 经典剔除路的可见缓冲形态。\n")
	TEXT(" 0: 剔除把每个可见实例的完整属性抄进可见缓冲（88 B / 槽 / LOD），绘制读那份副本（旧布局，留作对照）。\n")
	TEXT(" 1: 剔除只写 4 B 的源行号，绘制经它直接读源实例行（默认）。\n")
	TEXT("改动会让所有已注册的实例组件重建常驻缓冲与代理（每个组件一次阻塞刷新）。"),
	FConsoleVariableDelegate::CreateStatic(&UCSGpuInstancedMeshComponent::HandleVisibleSourceIndexCVarChanged),
	ECVF_Default);

namespace
{
	/** Same packing as CSDirectMesh.usf / RoadBuilder.usf so the tangent stream stays one format
	 *  across every GPU mesh path (VET_PackedNormal / PF_R8G8B8A8_SNORM). */
	uint32 PackSnorm8888(const FVector4f& V)
	{
		const int32 X = FMath::RoundToInt(FMath::Clamp(V.X, -1.0f, 1.0f) * 127.0f);
		const int32 Y = FMath::RoundToInt(FMath::Clamp(V.Y, -1.0f, 1.0f) * 127.0f);
		const int32 Z = FMath::RoundToInt(FMath::Clamp(V.Z, -1.0f, 1.0f) * 127.0f);
		const int32 W = FMath::RoundToInt(FMath::Clamp(V.W, -1.0f, 1.0f) * 127.0f);
		return (uint32(X) & 0xFF) | ((uint32(Y) & 0xFF) << 8) | ((uint32(Z) & 0xFF) << 16) | ((uint32(W) & 0xFF) << 24);
	}

	/** Spreads the low 10 bits of V so three of them interleave into a 30-bit Morton code. */
	uint32 SpreadBits10(uint32 V)
	{
		V &= 0x3FF;
		V = (V | (V << 16)) & 0x030000FF;
		V = (V | (V << 8)) & 0x0300F00F;
		V = (V | (V << 4)) & 0x030C30C3;
		V = (V | (V << 2)) & 0x09249249;
		return V;
	}

	uint32 MortonCode(const FVector3f& Normalized)
	{
		const uint32 X = uint32(FMath::Clamp(Normalized.X, 0.0f, 1.0f) * 1023.0f);
		const uint32 Y = uint32(FMath::Clamp(Normalized.Y, 0.0f, 1.0f) * 1023.0f);
		const uint32 Z = uint32(FMath::Clamp(Normalized.Z, 0.0f, 1.0f) * 1023.0f);
		return SpreadBits10(X) | (SpreadBits10(Y) << 1) | (SpreadBits10(Z) << 2);
	}

	/** Deterministic 0..1 value per instance, fed to the material's PerInstanceRandom. */
	float InstanceRandom(int32 Index)
	{
		uint32 H = uint32(Index) * 747796405u + 2891336453u;
		H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
		H = (H >> 22) ^ H;
		return float(H & 0xFFFFFFu) / float(0x1000000u);
	}

	/**
	 * 基础网格上传前把顶点色 alpha 清零 —— 类注释那份「实例路顶点色通道字典」的执行面。
	 *
	 * 这一步不是可选的美化：材质两条路读的是 `PerInstanceRandom + VertexColor.A`，而这条等式
	 * 只有在「GPU 路的 A 恒为 0」时才成立。资产里 alpha 通常是 255，漏掉这一步的症状是
	 * 实例版比烘焙版整体亮一档（tint 多加了 1.0），**不报错**。
	 *
	 * 打包顺序照 `ToPackedARGB` 的字面意思：最高 8 位是 A（小端内存里就是 FColor 的 .A）。
	 */
	uint32 ClearPackedColorAlpha(uint32 PackedARGB)
	{
		return PackedARGB & 0x00FFFFFFu;
	}
}

UCSGpuInstancedMeshComponent::UCSGpuInstancedMeshComponent()
{
	// **本条路的阴影只走常规 CSM，永远进不了 VSM。**（2026-09-12 定论，前两版归因都不完整。）
	//
	// 关卡在 ShadowDepthRendering.cpp:2145：VSM 的 MeshSelectionMask 只收
	// `MeshBatch.VertexFactory->SupportsGPUScene()` 为真的 batch，而那个函数正是
	// `SupportsPrimitiveIdStream() && PrimitiveIdStreamIndex != INDEX_NONE`（VertexFactory.h:754）
	// —— 本工厂两个条件都不满足（故意的，见 InitRHI 里那条 checkf），于是 Process() 根本不被
	// 调用，一条 draw command 都不会生成。
	// 顺带否掉两条旧归因：① "GPU-Scene 覆盖 indirect args" 在这条路上不成立
	// （MeshPassProcessor.cpp:1304 的 bDoOverrideArgs 要求 PrimitiveIdStreamIndex >= 0）；
	// ② 与 r.Shadow.Virtual.Cache / NonNanite.Batch / NonNanite.UseHZB 三个开关逐字节无关
	// （2026-08-30 三张对照图 md5 完全相同）。
	//
	// 那为什么今天 VSM 开着还有影子 —— 因为**引擎自带这条回退**，2026-08-30「VSM 下一个影子
	// 像素都画不出来」的实测漏看了它：方向光开 VSM 时引擎照样建 CSM 级联，只把级联的
	// MeshSelectionMask 钉成 EShadowMeshSelection::SM（ShadowSetup.cpp:5606），于是级联里
	// **只画进不了 VSM 的那些网格** —— 正好就是本条路。默认被
	// `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps`（默认 1）整个砍掉，本工程在
	// Config/DefaultEngine.ini 里置 0 把它打开了，完整机制与出处写在那边。
	// ⇒ 不存在双重阴影：非实例路（UCSMeshRenderComponent）的工厂 SupportsGPUScene() 为真，
	//   只进 VSM；本条路为假，只进级联。两边的 selection mask 互斥。
	//
	// ⚠️ 要让本条路**原生**进 VSM，只有"把实例灌进 GPU-Scene"一条路（UCSGpuInstancedNaniteComponent
	//    那条替身就是这么干的，所以它今天有 VSM 阴影）。给本工厂加 primitive-id 流是行不通的：
	//    那与手取实例变换的前提互斥。
	// ⚠️ 另一条已知代价（CSM 时期就有）：可见实例集是 RunCulling 按**主视锥**压出来的
	//    （一族只跑一次），主视锥外的实例不进阴影图。要视锥外也投影得另备一份不做视锥剔除的 args。
	CastShadow = true;
	bUseAsOccluder = false;
}

// -----------------------------------------------------------------------------
// Render path
// -----------------------------------------------------------------------------

bool UCSGpuInstancedMeshComponent::IsNaniteRenderPath() const
{
	// 外部喂进来的 GPU 网格（SetBaseMeshFromGpuData）不是资产，没有 Nanite 设置可读。
	if (!BaseMesh || bBaseMeshIsExternal) return false;

	// 判据就是资产自己的 Nanite 开关（含 r.Nanite.ForceEnableMeshes）。不看"这台机器现在画不画得了
	// Nanite"：画不了时替身由引擎 ISM 代理画回退网格，引擎自己的 StaticMeshComponent 也是这么退的；
	// 那样换 r.Nanite 时本组件不必跟着来回拆建两套缓冲。
	if (!BaseMesh->IsNaniteEnabled()) return false;

	// 替身整条路都建在 GPU-Scene 实例上，平台没有 GPU-Scene 就一个实例都画不出来 —— 那才退回 GPU 剔除路。
	return UseGPUScene(GMaxRHIShaderPlatform, GMaxRHIFeatureLevel);
}

bool UCSGpuInstancedMeshComponent::IsClassicShadowCopyActive() const
{
	return bClassicShadowOnNanitePath && IsNaniteRenderPath();
}

void UCSGpuInstancedMeshComponent::SetClassicShadowOnNanitePath(bool bEnable)
{
	if (bClassicShadowOnNanitePath == bEnable) return;
	bClassicShadowOnNanitePath = bEnable;
	// 不走 Nanite 路时这一条什么都不改（RebuildGpuMesh 在经典路上照常建），省掉一次阻塞的重建。
	if (!IsNaniteRenderPath()) return;
	RebuildGpuMesh();   // 未注册时只记脏，OnRegister 补
	MarkRenderStateDirty();
}

void UCSGpuInstancedMeshComponent::SetDistanceCullViewOverride(FVector WorldLocation)
{
	DistanceCullViewOverride = WorldLocation;
}

void UCSGpuInstancedMeshComponent::ClearDistanceCullViewOverride()
{
	DistanceCullViewOverride.Reset();
}

bool UCSGpuInstancedMeshComponent::GetDistanceCullViewOverride(FVector& OutWorldLocation) const
{
	if (!DistanceCullViewOverride.IsSet()) return false;
	OutWorldLocation = DistanceCullViewOverride.GetValue();
	return true;
}

FBox UCSGpuInstancedMeshComponent::GetBaseLocalBounds() const
{
	if (BaseMeshSnapshot.IsValid()) return BaseMeshSnapshot.LocalBounds;
	// Nanite 路画的是资产本身，不需要读顶点 —— 资产没开 CPU 访问时快照根本建不起来，
	// 但实例的剔除球与整族包围盒照样要有。
	if (IsNaniteRenderPath()) return BaseMesh->GetBoundingBox();
	return FBox(ForceInit);
}

void UCSGpuInstancedMeshComponent::UpdateNaniteComponent()
{
	UWorld* World = GetWorld();
	if (!World) return;

	FCSGpuNaniteInstanceFeed Feed;
	if (GpuInstanceSource.IsValid()) Feed.Packed = GpuInstanceSource;
	else if (GpuPointSource.IsValid()) Feed.Points = GpuPointSource;
	else if (PackedInstances.Num() > 0) Feed.CpuRows = MakeShared<const TArray<FVector4f>, ESPMode::ThreadSafe>(PackedInstances);

	if (!NaniteComponent)
	{
		// TextExportTransient：拷贝粘贴 actor 时也不带它，理由同 DuplicateTransient。
		NaniteComponent = NewObject<UCSGpuInstancedNaniteComponent>(this, NAME_None, RF_Transient | RF_DuplicateTransient | RF_TextExportTransient);
		NaniteComponent->SetupAttachment(this);
	}

	// 先抄网格再给源：代理构造时要同时读到网格与槽数，SetFeed 可能触发重建。
	NaniteComponent->SyncFromOwner(*this);
	NaniteComponent->SetFeed(MoveTemp(Feed), LocalBounds);

	// 注册即建代理，CreateRenderState_Concurrent 在同一个调用里把实例写进去。
	if (!NaniteComponent->IsRegistered() && World->Scene) NaniteComponent->RegisterComponentWithWorld(World);
}

void UCSGpuInstancedMeshComponent::ReleaseNaniteComponent()
{
	if (IsValid(NaniteComponent)) NaniteComponent->DestroyComponent();
	NaniteComponent = nullptr;
}

// -----------------------------------------------------------------------------
// Base mesh
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::SetBaseMesh(UStaticMesh* InMesh)
{
	if (BaseMesh == InMesh && !bBaseMeshIsExternal) return;

	BaseMesh = InMesh;
	bBaseMeshIsExternal = false;
	RebuildBaseMeshSnapshot();
}

void UCSGpuInstancedMeshComponent::SetBaseMeshFromGpuData(const FCSGpuMeshCPUData& InMeshData)
{
	BaseMeshSnapshot.Reset();
	BaseMeshMaterials.Reset();
	bBaseMeshIsExternal = true;
#if WITH_EDITOR
	// 不再是资产了，资产的重建与这一族无关。
	UnbindBaseMeshRebuildEvent();
#endif

	if (!InMeshData.IsValid())
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("SetBaseMeshFromGpuData: mesh data failed IsValid(); instancing disabled."));
		RebuildInstanceData();
		MarkRenderStateDirty();
		return;
	}

	const int32 NumVerts = InMeshData.Positions.Num();
	const bool bPerCorner = InMeshData.AttrLayout == FCSGpuMeshCPUData::EAttrLayout::PerCorner;
	// 外部数据声明了几组 UV 就带几组。IsValid() 是按钳过的组数查各通道等长的，字段本身仍可能越界 ——
	// 拿它当 TexCoordChannels 的下标之前同样钳一次。
	const int32 NumSets = FMath::Clamp(InMeshData.NumTexCoordChannels, 1, FCSGpuMeshCPUData::MaxTexCoordChannels);

	BaseMeshSnapshot.Positions = InMeshData.Positions;
	BaseMeshSnapshot.NumTexCoordSets = NumSets;

	// 材质段：按 TriangleMaterialSlots 把三角**稳定地**归到各自的槽里，一槽一段、槽号升序。一个 draw 只能画一截
	// 连续索引，所以同槽三角在数据里不相邻时这里要重排索引 —— 画出来的三角集合不变，只是顺序变了。
	// 槽表长度对不上三角数（或根本没给）就整张一段、槽 0，与改动前逐位相同。
	{
		const int32 NumTriangles = InMeshData.Indices.Num() / 3;
		const bool bHasSlots = InMeshData.TriangleMaterialSlots.Num() == NumTriangles;
		TArray<int32> Slots;
		if (bHasSlots)
		{
			for (int32 Slot : InMeshData.TriangleMaterialSlots) Slots.AddUnique(FMath::Max(Slot, 0));
			Slots.Sort();
		}
		if (Slots.IsEmpty()) Slots.Add(0);
		// 槽太多装不进绘制表：多出来的槽并进最后一段，用那一段的材质画（只告警，不丢三角）。
		if (Slots.Num() > CS_GPU_INSTANCED_MAX_DRAWS)
		{
			UE_LOG(LogCSGpuInstancedMesh, Warning,
				TEXT("%s: base mesh data uses %d material slots, more than the %d draws one instanced family can issue; slots %d.. share the material of slot %d."),
				*GetPathName(), Slots.Num(), CS_GPU_INSTANCED_MAX_DRAWS, Slots[CS_GPU_INSTANCED_MAX_DRAWS - 1], Slots[CS_GPU_INSTANCED_MAX_DRAWS - 1]);
		}
		auto DrawOfSlot = [&Slots](int32 Slot)
		{
			const int32 Found = Algo::LowerBound(Slots, FMath::Max(Slot, 0));
			return FMath::Min(FMath::Min(Found, Slots.Num() - 1), CS_GPU_INSTANCED_MAX_DRAWS - 1);
		};
		const int32 NumDraws = FMath::Min(Slots.Num(), CS_GPU_INSTANCED_MAX_DRAWS);

		BaseMeshSnapshot.Indices.Reserve(NumTriangles * 3);
		for (int32 Draw = 0; Draw < NumDraws; ++Draw)
		{
			FCSGpuInstancedSection Section;
			Section.LodIndex = 0;
			Section.FirstIndex = uint32(BaseMeshSnapshot.Indices.Num());
			Section.MaterialIndex = Slots[Draw];
			for (int32 Triangle = 0; Triangle < NumTriangles; ++Triangle)
			{
				if ((bHasSlots ? DrawOfSlot(InMeshData.TriangleMaterialSlots[Triangle]) : 0) != Draw) continue;
				BaseMeshSnapshot.Indices.Append(&InMeshData.Indices[Triangle * 3], 3);
			}
			Section.NumIndices = uint32(BaseMeshSnapshot.Indices.Num()) - Section.FirstIndex;
			if (Section.NumIndices > 0) BaseMeshSnapshot.Sections.Add(Section);
		}
	}
	// 材质槽表跟着数据走。空槽保留空（解析时画默认材质），槽数少于用到的槽号也不补 —— 越界同样解析成空。
	for (const TObjectPtr<UMaterialInterface>& Material : InMeshData.Materials) BaseMeshMaterials.Add(Material);
	BaseMeshSnapshot.TangentBasis.SetNumUninitialized(NumVerts * 2);
	BaseMeshSnapshot.TexCoords.SetNumUninitialized(NumVerts * NumSets);
	BaseMeshSnapshot.Colors.SetNumUninitialized(NumVerts);

	// Per-corner attributes cannot be indexed by vertex; take the first corner that references
	// each vertex rather than silently reading out of range.
	TArray<int32> AttrIndexForVertex;
	if (bPerCorner)
	{
		AttrIndexForVertex.Init(INDEX_NONE, NumVerts);
		for (int32 Corner = 0; Corner < InMeshData.Indices.Num(); ++Corner)
		{
			const int32 V = int32(InMeshData.Indices[Corner]);
			if (AttrIndexForVertex.IsValidIndex(V) && AttrIndexForVertex[V] == INDEX_NONE) AttrIndexForVertex[V] = Corner;
		}
	}

	for (int32 V = 0; V < NumVerts; ++V)
	{
		const int32 A = bPerCorner ? AttrIndexForVertex[V] : V;

		FVector3f Normal(0.0f, 0.0f, 1.0f);
		FVector3f Tangent(1.0f, 0.0f, 0.0f);
		if (InMeshData.Normals.IsValidIndex(A)) Normal = InMeshData.Normals[A].GetSafeNormal(UE_SMALL_NUMBER, FVector3f(0.0f, 0.0f, 1.0f));
		if (InMeshData.Tangents.IsValidIndex(A)) Tangent = InMeshData.Tangents[A].GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.0f, 0.0f, 0.0f));
		const float Sign = InMeshData.BinormalSigns.IsValidIndex(A) ? InMeshData.BinormalSigns[A] : 1.0f;

		BaseMeshSnapshot.TangentBasis[V * 2 + 0] = PackSnorm8888(FVector4f(Tangent, 0.0f));
		BaseMeshSnapshot.TangentBasis[V * 2 + 1] = PackSnorm8888(FVector4f(Normal, Sign >= 0.0f ? 1.0f : -1.0f));

		for (int32 Set = 0; Set < NumSets; ++Set) BaseMeshSnapshot.TexCoords[V * NumSets + Set] = InMeshData.TexCoordChannels[Set].IsValidIndex(A) ? InMeshData.TexCoordChannels[Set][A] : FVector2f::ZeroVector;

		FLinearColor Color = FLinearColor::White;
		if (InMeshData.Colors.IsValidIndex(A))
		{
			const FVector4f& C = InMeshData.Colors[A];
			Color = FLinearColor(C.X, C.Y, C.Z, C.W);
		}
		// ToPackedARGB reproduces FColor's own B,G,R,A memory order on little-endian, which is
		// what the manual-fetch colour path expects (FMANUALFETCH_COLOR_COMPONENT_SWIZZLE = .bgra).
		// alpha 归本组件所有（逐实例随机的载体，见类注释那份通道字典），一律清零。
		BaseMeshSnapshot.Colors[V] = ClearPackedColorAlpha(Color.ToFColor(false).ToPackedARGB());
	}

	FBox MeshBounds(ForceInit);
	for (const FVector3f& P : BaseMeshSnapshot.Positions) MeshBounds += FVector(P);
	BaseMeshSnapshot.LocalBounds = MeshBounds;

	FCSGpuInstancedLODRange LOD0;
	LOD0.FirstIndex = 0;
	LOD0.NumIndices = uint32(BaseMeshSnapshot.Indices.Num());
	LOD0.BaseVertex = 0;
	LOD0.ScreenSize = 0.0f; // single LOD: always selected
	BaseMeshSnapshot.LODs.Add(LOD0);

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::RebuildBaseMeshSnapshot()
{
	if (bBaseMeshIsExternal) return;

#if WITH_EDITOR
	// 每条改 BaseMesh 的路（SetBaseMesh / PostLoad / 细节面板）都经过这里，换网格就跟着换盯的对象。
	BindBaseMeshRebuildEvent();
#endif

	BaseMeshSnapshot.Reset();
	// 资产路每次现读 BaseMesh 的材质槽（ResolveSlotMaterial），外部快照留下的表不许再被读到。
	BaseMeshMaterials.Reset();

	const FStaticMeshRenderData* RenderData = BaseMesh ? BaseMesh->GetRenderData() : nullptr;
	if (!RenderData || RenderData->LODResources.Num() == 0)
	{
		RebuildInstanceData();
		MarkRenderStateDirty();
		UpdateBounds();
		return;
	}

	const int32 NumLODs = FMath::Min(RenderData->LODResources.Num(), CS_GPU_INSTANCED_MAX_LODS);

	// UV 组数取**进快照的各 LOD 里最多的那个**，某个 LOD 缺的组补零。交错布局要求每个顶点等宽，只能统一一个数；
	// 按 LOD0 定的话，别的 LOD 多出来的组会被静默丢掉。
	int32 NumSets = 1;
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex) NumSets = FMath::Max(NumSets, int32(RenderData->LODResources[LODIndex].VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords()));
	NumSets = FMath::Min(NumSets, FCSGpuMeshCPUData::MaxTexCoordChannels);
	BaseMeshSnapshot.NumTexCoordSets = NumSets;

	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		const FStaticMeshLODResources& LOD = RenderData->LODResources[LODIndex];
		const uint32 NumVerts = LOD.VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 NumTangentVerts = LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumVertices();
		if (NumVerts == 0 || NumTangentVerts != NumVerts) break;

		TArray<uint32> LODIndices;
		LOD.IndexBuffer.GetCopy(LODIndices);
		if (LODIndices.Num() < 3) break;

		FCSGpuInstancedLODRange Range;
		Range.BaseVertex = uint32(BaseMeshSnapshot.Positions.Num());
		Range.FirstIndex = uint32(BaseMeshSnapshot.Indices.Num());
		Range.NumIndices = uint32(LODIndices.Num());

		// 这一级的材质段：资产 LOD 的 FStaticMeshSection 就是它索引缓冲里连续的几截，一截一个 draw、材质槽照抄。
		// 在搬顶点**之前**算：绘制表装不下这一级时整级不进快照（与上面两条 break 同一个口径），LODs / Sections 一起停。
		TArray<FCSGpuInstancedSection, TInlineAllocator<4>> LODSections;
		for (const FStaticMeshSection& Source : LOD.Sections)
		{
			const uint32 First = FMath::Min(Source.FirstIndex, uint32(LODIndices.Num()));
			const uint32 Count = FMath::Min(Source.NumTriangles * 3u, uint32(LODIndices.Num()) - First);
			if (Count < 3u) continue;
			FCSGpuInstancedSection& Section = LODSections.AddDefaulted_GetRef();
			Section.LodIndex = BaseMeshSnapshot.LODs.Num();
			Section.FirstIndex = Range.FirstIndex + First;
			Section.NumIndices = Count - Count % 3u;
			Section.MaterialIndex = Source.MaterialIndex;
		}
		if (LODSections.IsEmpty())
		{
			// 渲染数据没有段表（极少见）：整级一段、槽 0 —— 改动前的画法。
			FCSGpuInstancedSection& Whole = LODSections.AddDefaulted_GetRef();
			Whole.LodIndex = BaseMeshSnapshot.LODs.Num();
			Whole.FirstIndex = Range.FirstIndex;
			Whole.NumIndices = Range.NumIndices;
			Whole.MaterialIndex = 0;
		}
		if (BaseMeshSnapshot.Sections.Num() + LODSections.Num() > CS_GPU_INSTANCED_MAX_DRAWS)
		{
			if (LODIndex > 0)
			{
				UE_LOG(LogCSGpuInstancedMesh, Warning,
					TEXT("%s: '%s' LOD%d would take the instanced draw count past %d; drawing LOD0..LOD%d only."),
					*GetPathName(), *GetNameSafe(BaseMesh), LODIndex, CS_GPU_INSTANCED_MAX_DRAWS, LODIndex - 1);
				break;
			}
			// LOD0 自己就超了：多出来的段并进最后一段（段在索引里是首尾相接的，并完仍是一截连续区间），
			// 用那一段的材质画 —— 丢三角比错材质更难看出来。
			UE_LOG(LogCSGpuInstancedMesh, Warning,
				TEXT("%s: '%s' LOD0 has %d material sections, more than the %d draws one instanced family can issue; the last ones share one material."),
				*GetPathName(), *GetNameSafe(BaseMesh), LODSections.Num(), CS_GPU_INSTANCED_MAX_DRAWS);
			FCSGpuInstancedSection& Last = LODSections[CS_GPU_INSTANCED_MAX_DRAWS - 1];
			const FCSGpuInstancedSection& Tail = LODSections.Last();
			Last.NumIndices = (Tail.FirstIndex + Tail.NumIndices) - Last.FirstIndex;
			LODSections.SetNum(CS_GPU_INSTANCED_MAX_DRAWS);
		}
		// Screen size at which this LOD takes over. LOD0's threshold is never tested (it is the
		// fallback when nothing smaller matches), so only 1..N-1 matter.
		Range.ScreenSize = (LODIndex < MAX_STATIC_MESH_LODS) ? RenderData->ScreenSize[LODIndex].Default : 0.0f;

		const bool bHasColors = LOD.VertexBuffers.ColorVertexBuffer.GetNumVertices() == NumVerts;
		const int32 NumLODSets = int32(LOD.VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords());

		BaseMeshSnapshot.Positions.Reserve(BaseMeshSnapshot.Positions.Num() + int32(NumVerts));
		BaseMeshSnapshot.TangentBasis.Reserve(BaseMeshSnapshot.TangentBasis.Num() + int32(NumVerts) * 2);
		BaseMeshSnapshot.TexCoords.Reserve(BaseMeshSnapshot.TexCoords.Num() + int32(NumVerts) * NumSets);
		BaseMeshSnapshot.Colors.Reserve(BaseMeshSnapshot.Colors.Num() + int32(NumVerts));

		for (uint32 V = 0; V < NumVerts; ++V)
		{
			BaseMeshSnapshot.Positions.Add(LOD.VertexBuffers.PositionVertexBuffer.VertexPosition(V));

			const FVector3f TangentX = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(V);
			const FVector4f TangentZ = LOD.VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(V);
			BaseMeshSnapshot.TangentBasis.Add(PackSnorm8888(FVector4f(TangentX, 0.0f)));
			BaseMeshSnapshot.TangentBasis.Add(PackSnorm8888(TangentZ));

			for (int32 Set = 0; Set < NumSets; ++Set) BaseMeshSnapshot.TexCoords.Add(Set < NumLODSets ? LOD.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(V, uint32(Set)) : FVector2f::ZeroVector);
			// alpha 归本组件所有（逐实例随机的载体，见类注释那份通道字典），一律清零；
			// 没有色流的资产退白也一样要清，否则烘焙路与实例路差一个常数 1.0。
			BaseMeshSnapshot.Colors.Add(ClearPackedColorAlpha(
				bHasColors ? LOD.VertexBuffers.ColorVertexBuffer.VertexColor(V).ToPackedARGB() : 0xFFFFFFFFu));
		}

		// Indices are stored relative to the LOD's own vertex block; DrawIndexedIndirect adds
		// BaseVertexLocation, so they go in unmodified.
		BaseMeshSnapshot.Indices.Append(LODIndices);
		BaseMeshSnapshot.LODs.Add(Range);
		BaseMeshSnapshot.Sections.Append(LODSections);
	}

	// Nanite 路画的是资产本身，读不到顶点只影响烘焙出口 —— 那条出口自己会说"没有快照"，这里不刷屏。
	if (BaseMeshSnapshot.LODs.Num() == 0 && !IsNaniteRenderPath())
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: could not read vertex data from '%s'. Enable 'Allow CPU Access' on the mesh (required outside the editor)."),
			*GetPathName(), *GetNameSafe(BaseMesh));
	}
	else if (BaseMeshSnapshot.LODs.Num() > 0)
	{
		BaseMeshSnapshot.LocalBounds = BaseMesh->GetBoundingBox();
	}

	RebuildInstanceData();
	MarkRenderStateDirty();
	UpdateBounds();
}

// -----------------------------------------------------------------------------
// Instances — CPU source
// -----------------------------------------------------------------------------

int32 UCSGpuInstancedMeshComponent::AddInstance(const FTransform& InstanceTransform, bool bWorldSpace)
{
	const FTransform Local = bWorldSpace ? InstanceTransform.GetRelativeTransform(GetComponentTransform()) : InstanceTransform;
	EnsureInstanceIdentity();
	const int32 Index = PerInstanceTransforms.Add(Local);
	AllocateInstanceSlot(Index);

	NotifyInstancesChanged();
	return Index;
}

TArray<int32> UCSGpuInstancedMeshComponent::AddInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace)
{
	TArray<int32> Indices;
	Indices.Reserve(InstanceTransforms.Num());

	const FTransform ComponentTransform = GetComponentTransform();
	EnsureInstanceIdentity();
	for (const FTransform& T : InstanceTransforms)
	{
		const int32 Index = PerInstanceTransforms.Add(bWorldSpace ? T.GetRelativeTransform(ComponentTransform) : T);
		AllocateInstanceSlot(Index);
		Indices.Add(Index);
	}

	NotifyInstancesChanged();
	return Indices;
}

void UCSGpuInstancedMeshComponent::SetInstances(const TArray<FTransform>& InstanceTransforms, bool bWorldSpace)
{
	PerInstanceTransforms.Reset(InstanceTransforms.Num());

	const FTransform ComponentTransform = GetComponentTransform();
	for (const FTransform& T : InstanceTransforms) PerInstanceTransforms.Add(bWorldSpace ? T.GetRelativeTransform(ComponentTransform) : T);
	// 整套换掉：旧句柄全部失效，新实例按给入顺序占槽 0..N-1 —— 同一份输入每次拿到同一批逐实例随机数
	// （拖尺寸时生产者每帧 SetInstances 一遍，随机数跟着跳就是一片闪烁）。
	ResetInstanceIdentity(PerInstanceTransforms.Num());

	NotifyInstancesChanged();
}

bool UCSGpuInstancedMeshComponent::RemoveInstance(int32 InstanceIndex)
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	EnsureInstanceIdentity();
	// 槽当场回收、代数 +1：这个实例的句柄从这一行起失效，之后占了这个槽的是另一个实例。
	const int32 RemovedSlot = PerInstanceSlots[InstanceIndex];
	++SlotGenerations[RemovedSlot];
	SlotToIndex[RemovedSlot] = INDEX_NONE;
	FreeSlots.HeapPush(RemovedSlot);

	PerInstanceTransforms.RemoveAtSwap(InstanceIndex);
	PerInstanceSlots.RemoveAtSwap(InstanceIndex);
	// 末尾那个实例被换进了空位：它的**下标**变了，槽（身份、随机数）没变。
	if (PerInstanceSlots.IsValidIndex(InstanceIndex)) SlotToIndex[PerInstanceSlots[InstanceIndex]] = InstanceIndex;

	NotifyInstancesChanged();
	return true;
}

bool UCSGpuInstancedMeshComponent::UpdateInstanceTransform(int32 InstanceIndex, const FTransform& NewInstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty)
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	PerInstanceTransforms[InstanceIndex] = bWorldSpace ? NewInstanceTransform.GetRelativeTransform(GetComponentTransform()) : NewInstanceTransform;

	// bMarkRenderStateDirty=false 只记脏：排序、打包、上传都攒到下一次真要更新的调用（或 EndInstanceUpdate）。
	// 以前这里仍然当场整份重排打包（O(N log N)），只省了上传 —— 一串 8 个改动付 8 次排序。
	NotifyInstancesChanged(/*bApplyNow*/ bMarkRenderStateDirty);
	return true;
}

bool UCSGpuInstancedMeshComponent::GetInstanceTransform(int32 InstanceIndex, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return false;

	OutInstanceTransform = PerInstanceTransforms[InstanceIndex];
	if (bWorldSpace) OutInstanceTransform *= GetComponentTransform();
	return true;
}

void UCSGpuInstancedMeshComponent::ClearInstances()
{
	PerInstanceTransforms.Reset();
	ResetInstanceIdentity(0);
	NotifyInstancesChanged();
}

// -----------------------------------------------------------------------------
// Instances — 成批改（InstanceCullingOptimization.md §6.1）
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::BeginInstanceUpdate()
{
	++InstanceUpdateDepth;
}

void UCSGpuInstancedMeshComponent::EndInstanceUpdate()
{
	if (!ensureMsgf(InstanceUpdateDepth > 0, TEXT("%s: EndInstanceUpdate 没有对应的 BeginInstanceUpdate。"), *GetPathName())) return;
	if (--InstanceUpdateDepth > 0) return;
	if (bInstanceUploadPending) ApplyInstanceChanges();
}

int32 UCSGpuInstancedMeshComponent::RemoveInstances(const TArray<int32>& InstanceIndices)
{
	// 从大到小删：RemoveAtSwap 只搬末尾那一个实例，而它的下标一定 ≥ 当前被删的下标 —— 还没删的小下标指的实例一个都不动。
	TArray<int32> Sorted = InstanceIndices;
	Sorted.Sort(TGreater<int32>());

	FCSGpuInstancedUpdateScope Scope(this);
	int32 Removed = 0;
	int32 Previous = INDEX_NONE;
	for (const int32 Index : Sorted)
	{
		if (Index == Previous) continue;   // 重复的下标：第二次指的已经是被换进来的另一个实例
		Previous = Index;
		if (RemoveInstance(Index)) ++Removed;
	}
	return Removed;
}

void UCSGpuInstancedMeshComponent::NotifyInstancesChanged(bool bApplyNow)
{
	bInstancePackingDirty = true;
	bInstanceUploadPending = true;
	if (bApplyNow && InstanceUpdateDepth == 0) ApplyInstanceChanges();
}

void UCSGpuInstancedMeshComponent::ApplyInstanceChanges()
{
	bInstanceUploadPending = false;
	// RebuildGpuMesh 开头会补打包（EnsureInstancePacking），所以这里不必无条件重打一遍：
	// 区间里有人读过 packed 行的话，那一次已经是最新的了。
	RebuildGpuMesh();
	MarkRenderStateDirty();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::EnsureInstancePacking() const
{
	if (bInstancePackingDirty) const_cast<UCSGpuInstancedMeshComponent*>(this)->RebuildInstancePacking();
}

// -----------------------------------------------------------------------------
// Instances — 稳定句柄（FCSGpuInstanceHandle；InstanceCullingOptimization.md §3.1 / §4.2）
// -----------------------------------------------------------------------------

FCSGpuInstanceHandle UCSGpuInstancedMeshComponent::GetInstanceHandle(int32 InstanceIndex) const
{
	if (!PerInstanceTransforms.IsValidIndex(InstanceIndex)) return FCSGpuInstanceHandle();

	const_cast<UCSGpuInstancedMeshComponent*>(this)->EnsureInstanceIdentity();
	const int32 Slot = PerInstanceSlots[InstanceIndex];
	return FCSGpuInstanceHandle(Slot, SlotGenerations[Slot]);
}

int32 UCSGpuInstancedMeshComponent::GetInstanceIndexFromHandle(FCSGpuInstanceHandle Handle) const
{
	if (!Handle.IsSet()) return INDEX_NONE;

	UCSGpuInstancedMeshComponent* MutableThis = const_cast<UCSGpuInstancedMeshComponent*>(this);
	MutableThis->EnsureInstanceIdentity();
	// 代数对不上 = 那个实例已经删了（槽可能已经给了别的实例，那是另一个身份）。
	if (!SlotGenerations.IsValidIndex(Handle.Slot) || SlotGenerations[Handle.Slot] != Handle.Generation) return INDEX_NONE;

	int32 Index = SlotToIndex[Handle.Slot];
	if (Index != INDEX_NONE && (!PerInstanceSlots.IsValidIndex(Index) || PerInstanceSlots[Index] != Handle.Slot))
	{
		// 缓存与真值表对不上：有一条绕过 mutator 改了实例数组的路没把缓存标脏。不猜，按真值表重建一次再答 ——
		// 把缓存里的下标直接还回去，就是把状态挂到另一个实例身上。
		MutableThis->bInstanceIdentityCacheDirty = true;
		MutableThis->EnsureInstanceIdentity();
		if (!SlotGenerations.IsValidIndex(Handle.Slot) || SlotGenerations[Handle.Slot] != Handle.Generation) return INDEX_NONE;
		Index = SlotToIndex[Handle.Slot];
	}
	return Index;
}

bool UCSGpuInstancedMeshComponent::RemoveInstanceByHandle(FCSGpuInstanceHandle Handle)
{
	const int32 Index = GetInstanceIndexFromHandle(Handle);
	return Index != INDEX_NONE && RemoveInstance(Index);
}

bool UCSGpuInstancedMeshComponent::UpdateInstanceTransformByHandle(FCSGpuInstanceHandle Handle, const FTransform& NewInstanceTransform, bool bWorldSpace, bool bMarkRenderStateDirty)
{
	const int32 Index = GetInstanceIndexFromHandle(Handle);
	return Index != INDEX_NONE && UpdateInstanceTransform(Index, NewInstanceTransform, bWorldSpace, bMarkRenderStateDirty);
}

bool UCSGpuInstancedMeshComponent::GetInstanceTransformByHandle(FCSGpuInstanceHandle Handle, FTransform& OutInstanceTransform, bool bWorldSpace) const
{
	const int32 Index = GetInstanceIndexFromHandle(Handle);
	return Index != INDEX_NONE && GetInstanceTransform(Index, OutInstanceTransform, bWorldSpace);
}

void UCSGpuInstancedMeshComponent::EnsureInstanceIdentity()
{
	const int32 NumInstances = PerInstanceTransforms.Num();

	// O(1) 的不变量：槽表与实例一一对应、缓存与代数表等长、在用的槽数 = 实例数。读盘 / 撤销 / 复制绕过了 mutator，
	// 那几条路要么把 bInstanceIdentityCacheDirty 置真，要么让下面某一条不成立。
	if (!bInstanceIdentityCacheDirty
		&& PerInstanceSlots.Num() == NumInstances
		&& SlotToIndex.Num() == SlotGenerations.Num()
		&& SlotToIndex.Num() - FreeSlots.Num() == NumInstances)
	{
		return;
	}

	// --- 真值表自洽吗：每个实例一个槽、槽在范围内、互不重复 ---
	bool bTruthValid = PerInstanceSlots.Num() == NumInstances;
	SlotToIndex.Init(INDEX_NONE, SlotGenerations.Num());
	for (int32 Index = 0; bTruthValid && Index < NumInstances; ++Index)
	{
		const int32 Slot = PerInstanceSlots[Index];
		if (!SlotToIndex.IsValidIndex(Slot) || SlotToIndex[Slot] != INDEX_NONE) bTruthValid = false;
		else SlotToIndex[Slot] = Index;
	}

	if (!bTruthValid)
	{
		// 2026-10-02 以前存的资产没有槽表（两张表都是空的）：按下标发槽，随机数与改动前逐位相同，不出声。
		// 别的情形是表被改坏了：同样重发，但此前发出去的句柄指的是哪个实例已经说不清，全部作废并说出来。
		if (PerInstanceSlots.Num() > 0 || SlotGenerations.Num() > 0)
		{
			UE_LOG(LogCSGpuInstancedMesh, Warning,
				TEXT("%s: 实例身份表与实例数组对不上（%d 个实例、%d 项槽表、%d 个槽），已按下标重发槽号；此前的实例句柄全部失效。"),
				*GetPathName(), NumInstances, PerInstanceSlots.Num(), SlotGenerations.Num());
		}
		for (int32& Generation : SlotGenerations) ++Generation;
		if (SlotGenerations.Num() < NumInstances) SlotGenerations.SetNumZeroed(NumInstances);

		PerInstanceSlots.SetNumUninitialized(NumInstances);
		SlotToIndex.Init(INDEX_NONE, SlotGenerations.Num());
		for (int32 Index = 0; Index < NumInstances; ++Index)
		{
			PerInstanceSlots[Index] = Index;
			SlotToIndex[Index] = Index;
		}
	}

	// 空闲槽是个最小堆：新实例永远拿**最小**的空闲槽。这样发槽的结果只取决于两张真值表，
	// 缓存在读盘 / 撤销后重建过也好、一路增删过来也好，下一个加进来的实例拿到的槽（与随机数）是同一个。
	FreeSlots.Reset();
	for (int32 Slot = 0; Slot < SlotToIndex.Num(); ++Slot)
	{
		if (SlotToIndex[Slot] == INDEX_NONE) FreeSlots.Add(Slot);   // 升序本身就是合法的最小堆
	}
	bInstanceIdentityCacheDirty = false;
}

void UCSGpuInstancedMeshComponent::ResetInstanceIdentity(int32 NumInstances)
{
	// 只看真值表（此刻 PerInstanceTransforms 多半已经换成新的那一批，缓存的不变量不成立）：
	// 在用的槽里的实例没了，代数 +1。
	for (const int32 Slot : PerInstanceSlots)
	{
		if (SlotGenerations.IsValidIndex(Slot)) ++SlotGenerations[Slot];
	}
	if (SlotGenerations.Num() < NumInstances) SlotGenerations.SetNumZeroed(NumInstances);

	PerInstanceSlots.SetNumUninitialized(NumInstances);
	SlotToIndex.Init(INDEX_NONE, SlotGenerations.Num());
	for (int32 Index = 0; Index < NumInstances; ++Index)
	{
		PerInstanceSlots[Index] = Index;
		SlotToIndex[Index] = Index;
	}
	FreeSlots.Reset();
	for (int32 Slot = NumInstances; Slot < SlotGenerations.Num(); ++Slot) FreeSlots.Add(Slot);
	bInstanceIdentityCacheDirty = false;
}

void UCSGpuInstancedMeshComponent::AllocateInstanceSlot(int32 InstanceIndex)
{
	// 调用方先 EnsureInstanceIdentity() 再往 PerInstanceTransforms 里加，所以这里槽表正好缺新加的这一项。
	check(PerInstanceSlots.Num() == InstanceIndex);

	int32 Slot = INDEX_NONE;
	if (FreeSlots.Num() > 0)
	{
		FreeSlots.HeapPop(Slot, EAllowShrinking::No);
	}
	else
	{
		Slot = SlotGenerations.Add(0);
		SlotToIndex.Add(INDEX_NONE);
	}
	PerInstanceSlots.Add(Slot);
	SlotToIndex[Slot] = InstanceIndex;
}

// -----------------------------------------------------------------------------
// Instances — GPU source
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::SetInstanceSourceFromPoints(const FCSGpuInstancePointSourceGPU& InSource)
{
	GpuInstanceSource.Reset();
	GpuPointSource = InSource;
	RebuildInstanceData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::SetInstanceSourceGPU(const FCSGpuInstanceSourceGPU& InSource)
{
	GpuPointSource.Reset();
	GpuInstanceSource = InSource;
	RebuildInstanceData();

	// A GPU source is normally handed over right before a save or a readback, so the new proxy
	// has to exist immediately — MarkRenderStateDirty alone defers to the end-of-frame update.
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::ClearInstanceSourceGPU()
{
	if (!GpuInstanceSource.IsValid() && !GpuPointSource.IsValid()) return;

	GpuInstanceSource.Reset();
	GpuPointSource.Reset();
	RebuildInstanceData();
	RecreateRenderState_Concurrent();
	UpdateBounds();
}

void UCSGpuInstancedMeshComponent::NotifyInstanceSourceChanged()
{
	// 只有 Nanite 路的替身关心：它手上是源的一份拷贝，承诺了通知的源只在这里被叫到时才重写 GPU-Scene。
	if (NaniteComponent) NaniteComponent->NotifyFeedChanged();
}

// -----------------------------------------------------------------------------
// 诊断 / 验收（**阻塞**）—— 声明处那段注释是这两条为什么存在
// -----------------------------------------------------------------------------

int32 UCSGpuInstancedMeshComponent::DebugReadDrawnInstanceCountSync() const
{
	// 读的必须是**组件手上这一份**实例源，不是生产者自己那批 buffer：门框砖那个既有 bug 的
	// 现场就是两者不一致 —— 生产者以为自己清干净了，而组件被重新交接回同一批带着陈旧计数器
	// 的 buffer，剔除 pass 照着它又画了一遍上一代的实例。
	// 按值持一份引用而不是绑常量引用：回读期间生产者可能换掉成员，而三元表达式的结果本来
	// 就是个 prvalue —— 写成引用只是把"这里有一次拷贝"藏起来，读的人容易误以为它跟着成员走。
	TRefCountPtr<FRDGPooledBuffer> Counter;
	if (GpuInstanceSource.IsValid()) Counter = GpuInstanceSource.Counter;
	else if (GpuPointSource.IsValid()) Counter = GpuPointSource.Counter;
	if (!Counter.IsValid())
	{
		// Nanite 路的 CPU 数组：写入 pass 读的计数就是替身手上那份行数（CSGpuInstancedNanite_AddWritePasses
		// 那条 AddClearUAVPass(..., LiveCount)）。替身没注册 = 没有图元，那才是真的 0。
		if (IsNaniteRenderPath())
		{
			const bool bDrawing = NaniteComponent && NaniteComponent->IsRegistered() && NaniteComponent->GetGpuSceneSlotCount() > 0;
			return bDrawing ? PackedInstances.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S : 0;
		}

		// 没有 GPU 源时剔除 pass 用的是常量 `NumSourceInstances`（CSGpuInstancedMeshSceneProxy
		// 那条 `AddClearUAVPass(..., Layout.NumSourceInstances)`），如实返回它。没有分配 =
		// 组件根本不会建代理，一个实例都画不出来，那才是真的 0。
		const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
		if (!Resident.IsValid() || !Resident->IsAllocated() || !GpuLayout.IsValid()) return 0;
		return int32(GpuLayout.NumSourceInstances);
	}

	TArray<uint32> Values;
	// AuxVertex 的常态访问是 SRVMask —— 实例计数器平时就停在那里（生产者的
	// `SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask)`），读完必须放回去。
	if (!CSMeshReadback::ReadUintBufferSync(Counter, 1u, ECSGpuStreamRole::AuxVertex, Values) || Values.IsEmpty())
	{
		// **读不到不能当 0**：那会让"擦掉之后 GPU 计数器必须归零"这条断言在回读管线自己坏掉时
		// 假绿 —— 正是这一族 bug 最典型的失效方式。
		return -1;
	}
	const uint32 Capacity = GpuInstanceSource.IsValid() ? GpuInstanceSource.Capacity : GpuPointSource.Capacity;
	// GPU 的 counter 会数到越界丢弃的那些（InterlockedAdd 先加后判），按容量钳 —— 同石阶回读。
	// 超容量要出声（审查 B4b）：钳过的数对所有断言都像"刚好装满"，而丢了哪些实例由原子序决定。
	if (Values[0] > Capacity)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: 实例源超容量：counter=%u capacity=%u，超出的实例被静默丢弃。"),
			*GetPathName(), Values[0], Capacity);
	}
	return int32(FMath::Min(Values[0], FMath::Max(Capacity, 1u)));
}

FString UCSGpuInstancedMeshComponent::DebugGetDrawnAssetMismatchSync() const
{
	if (IsNaniteRenderPath())
	{
		if (!NaniteComponent) return TEXT("BaseMesh 开了 Nanite 但渲染替身没建起来（组件没注册过？一个实例都画不出来）");
		return NaniteComponent->DebugDescribeMismatchSync(*this);
	}

	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	if (!Resident.IsValid() || !Resident->IsAllocated())
	{
		return TEXT("GPU 网格没分配（组件不会建场景代理，一个实例都画不出来）");
	}

	// --- 基础网格：拿 GPU 上真的那份跟我们以为的那张对 ---
	//
	// CPU 侧的 `BaseMeshSnapshot` 只是"我们打算上传什么"。中间隔着 EnsureCapacitySync /
	// SetStreamLayoutSync / ShrinkCapacitySync 三道会失败且**只写日志不报错**的关口，
	// 任何一道漏掉，画面上就是上一张网格（或什么都没有），而所有计数断言照绿。
	uint32 GpuVerts = 0;
	uint32 GpuIndices = 0;
	if (!CSMeshReadback::ReadCountersSync(*Resident, GpuVerts, GpuIndices))
	{
		return TEXT("读不到 GPU 上的网格计数器（回读没完成）");
	}
	if (GpuVerts != uint32(BaseMeshSnapshot.Positions.Num()) || GpuIndices != uint32(BaseMeshSnapshot.Indices.Num()))
	{
		return FString::Printf(
			TEXT("GPU 上的基础网格不是快照那一张（GPU %u 顶点 / %u 索引，快照 %d / %d）"),
			GpuVerts, GpuIndices, BaseMeshSnapshot.Positions.Num(), BaseMeshSnapshot.Indices.Num());
	}
	if (GpuVerts < 3u || GpuIndices < 3u)
	{
		return TEXT("GPU 上的基础网格是空的（不足一个三角）");
	}

	// 快照又是从哪张资产建的：换掉 `BaseMesh` 时组件不一定重建，那时候画面上还是旧网格 ——
	// 与坑表里"CDO 默认值不传播到已存在实例"同一族的静默失效。外部喂进来的快照没有资产可对。
	if (!bBaseMeshIsExternal)
	{
		if (!BaseMesh) return TEXT("没有基础网格资产（BaseMesh 是 NULL）");
		const FStaticMeshRenderData* RenderData = BaseMesh->GetRenderData();
		if (!RenderData || RenderData->LODResources.Num() == 0)
		{
			return FString::Printf(TEXT("基础网格 '%s' 没有渲染数据（CPU 访问没开？）"), *GetNameSafe(BaseMesh));
		}
		const uint32 AssetLod0Verts = RenderData->LODResources[0].VertexBuffers.PositionVertexBuffer.GetNumVertices();
		const uint32 SnapshotLod0Verts = BaseMeshSnapshot.LODs.Num() > 0
			? (BaseMeshSnapshot.LODs.Num() > 1 ? BaseMeshSnapshot.LODs[1].BaseVertex : uint32(BaseMeshSnapshot.Positions.Num()))
			: 0u;
		if (AssetLod0Verts != SnapshotLod0Verts)
		{
			return FString::Printf(
				TEXT("GPU 上的基础网格与资产 '%s' 对不上（LOD0：GPU %u 顶点，资产 %u）——多半是换了资产但组件没重建"),
				*GetNameSafe(BaseMesh), SnapshotLod0Verts, AssetLod0Verts);
		}
	}

	// --- 材质：每一段画的材质，引擎会不会把它静默换掉 ---
	// 非阻塞的两条（解析出来非空、母材质勾了实例化用途）与各 actor 的 `Get*UndrawableReason` 共用一份。
	// `CreateSceneProxy` 不再把不合格的材质就地置空（那会把"整体覆盖"悄悄改成"用资产材质"），
	// 所以被换掉的材质在这里仍以原名出现、原因里说清是哪一条。
	const FString MaterialReason = GetMaterialUndrawableReason();
	if (!MaterialReason.IsEmpty()) return MaterialReason;

	// 比"勾没勾"再进一步：**编译产物**里到底有没有本顶点工厂那一份。勾是输入，着色器映射
	// 是输出 —— 母材质勾了但那一份没编出来（平台裁剪、编译失败）时，渲染器一样退默认材质。
	// 逐段各探一次（同一张材质只探一次），全部挤进一次阻塞刷新。
	//
	// ⚠️ 三态，不是两态：无头进程里材质的渲染线程着色器映射可能压根还没建起来，那时候
	// **判不了**。判不了就说判不了，不许当成通过 —— 本项目已经吃过"把判不了当绿"的亏。
	struct FProbe
	{
		const UMaterialInterface* Material = nullptr;
		bool bShaderMapKnown = false;
		bool bHasFactoryShaders = false;
	};
	TArray<FProbe, TInlineAllocator<4>> Probes;
	for (int32 Section = 0; Section < BaseMeshSnapshot.Sections.Num(); ++Section)
	{
		const UMaterialInterface* SectionMaterial = GetSectionMaterial(Section);
		if (SectionMaterial && !Probes.ContainsByPredicate([SectionMaterial](const FProbe& P) { return P.Material == SectionMaterial; }))
		{
			Probes.AddDefaulted_GetRef().Material = SectionMaterial;
		}
	}
	const ERHIFeatureLevel::Type FeatureLevel = GMaxRHIFeatureLevel;
	ENQUEUE_RENDER_COMMAND(CSGpuInstancedProbeMaterialShaderMap)(
		[&Probes, FeatureLevel](FRHICommandListImmediate&)
		{
			for (FProbe& Probe : Probes)
			{
				const FMaterialRenderProxy* Proxy = Probe.Material->GetRenderProxy();
				if (!Proxy) continue;
				const FMaterialRenderProxy* Fallback = nullptr;
				const FMaterial& Material = Proxy->GetMaterialWithFallback(FeatureLevel, Fallback);
				const FMaterialShaderMap* ShaderMap = Material.GetRenderingThreadShaderMap();
				if (!ShaderMap) continue;
				Probe.bShaderMapKnown = true;
				Probe.bHasFactoryShaders = ShaderMap->GetMeshShaderMap(&FCSGpuInstancedMeshVertexFactory::StaticType) != nullptr;
			}
		});
	UCSMesh::CountedBlockingFlush();
	for (const FProbe& Probe : Probes)
	{
		if (Probe.bShaderMapKnown && !Probe.bHasFactoryShaders)
		{
			return FString::Printf(
				TEXT("材质 '%s' 没有为本实例顶点工厂编出着色器（渲染器会退回默认材质）"),
				*GetNameSafe(Probe.Material));
		}
		if (!Probe.bShaderMapKnown)
		{
			// **判不了要说出来**：空串在调用方那里读作"通过"，而这一路其实什么都没证明。
			// 不打这行日志的话，这条判据会在无头环境里静默退化成"永远绿"，
			// 而那正是本项目已经吃过三次亏的失效方式。
			UE_LOG(LogCSGpuInstancedMesh, Log,
				TEXT("%s: 材质 '%s' 的渲染线程着色器映射还没建起来，「引擎会不会静默换材质」这一条判不了（不算通过）。"),
				*GetPathName(), *GetNameSafe(Probe.Material));
		}
	}

	return FString();
}

// -----------------------------------------------------------------------------
// 材质：逐段解析（整体覆盖 > 资产 / 快照材质槽）—— 类注释「材质」一节
// -----------------------------------------------------------------------------

UMaterialInterface* UCSGpuInstancedMeshComponent::ResolveSlotMaterial(int32 SlotIndex) const
{
	if (InstanceMaterial) return InstanceMaterial;
	if (bBaseMeshIsExternal) return BaseMeshMaterials.IsValidIndex(SlotIndex) ? BaseMeshMaterials[SlotIndex].Get() : nullptr;
	// 现读资产：资产上改了材质槽，下一次建代理就用上，不必等快照重建。越界时 GetMaterial 自己返回空。
	return BaseMesh ? BaseMesh->GetMaterial(SlotIndex) : nullptr;
}

UMaterialInterface* UCSGpuInstancedMeshComponent::GetSectionMaterial(int32 SectionIndex) const
{
	if (InstanceMaterial) return InstanceMaterial;
	const int32 Slot = BaseMeshSnapshot.Sections.IsValidIndex(SectionIndex) ? BaseMeshSnapshot.Sections[SectionIndex].MaterialIndex : 0;
	return ResolveSlotMaterial(Slot);
}

int32 UCSGpuInstancedMeshComponent::GetNumMaterials() const
{
	if (bBaseMeshIsExternal) return FMath::Max(BaseMeshMaterials.Num(), 1);
	return BaseMesh ? FMath::Max(BaseMesh->GetStaticMaterials().Num(), 1) : 1;
}

UMaterialInterface* UCSGpuInstancedMeshComponent::GetMaterial(int32 ElementIndex) const
{
	return ResolveSlotMaterial(ElementIndex);
}

void UCSGpuInstancedMeshComponent::GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials) const
{
	for (int32 Slot = 0; Slot < GetNumMaterials(); ++Slot)
	{
		if (UMaterialInterface* Material = ResolveSlotMaterial(Slot)) OutMaterials.AddUnique(Material);
	}
}

void UCSGpuInstancedMeshComponent::SetInstanceMaterial(UMaterialInterface* InMaterial)
{
	if (InstanceMaterial == InMaterial) return;
	InstanceMaterial = InMaterial;

	// 常驻网格的材质表是回读存盘取槽的地方，跟着换（只广播，不碰任何几何）。
	if (InstancedGpuMesh) InstancedGpuMesh->SetMaterial(0, GetSectionMaterial(0));
	// Nanite 路：替身每帧从本体拉覆盖材质，这里推一次免得晚一帧。
	if (NaniteComponent) NaniteComponent->SyncFromOwner(*this);
	// 经典路：代理构造时把每段的材质抄走了，光写属性看不出变化。
	MarkRenderStateDirty();
}

FString UCSGpuInstancedMeshComponent::GetMaterialUndrawableReason() const
{
	const bool bNanite = IsNaniteRenderPath();

	// 经典路按快照的段查（画的就是这些段）；Nanite 路画的是资产自己的每一个材质槽。
	TArray<int32, TInlineAllocator<8>> Slots;
	if (bNanite)
	{
		for (int32 Slot = 0; Slot < GetNumMaterials(); ++Slot) Slots.Add(Slot);
	}
	else
	{
		for (const FCSGpuInstancedSection& Section : BaseMeshSnapshot.Sections) Slots.AddUnique(Section.MaterialIndex);
	}
	if (Slots.IsEmpty()) return TEXT("基础网格快照里一个材质段都没有（没有基础网格，或读不到它的 LOD0）");

	const TCHAR* Source = InstanceMaterial ? TEXT("整体覆盖 InstanceMaterial") : TEXT("基础网格资产的材质槽");
	const TCHAR* Usage = bNanite ? TEXT("bUsedWithNanite") : TEXT("bUsedWithInstancedStaticMeshes");
	for (const int32 Slot : Slots)
	{
		const UMaterialInterface* Material = ResolveSlotMaterial(Slot);
		// **石阶那个坑**：材质为空时组件照样画，只是退回引擎默认表面材质 —— 画面一片灰，readback 断言照绿。
		if (!Material)
		{
			return FString::Printf(TEXT("材质槽 %d（%s）是空的（会用引擎默认表面材质画成一片灰）"), Slot, Source);
		}
		// 没勾这条渲染路要的用途标记，引擎在这条路上会**静默换成默认材质**，症状与"没绑材质"逐像素相同。
		// ⚠️ 编辑器里引擎会在第一次实例化使用时自己把标志勾回去并重编（并把包弄脏），所以这一条在编辑器里
		// 很难抵到东西，真正会发作的是烘培 / 非编辑器路径。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !(bNanite ? Base->bUsedWithNanite : Base->bUsedWithInstancedStaticMeshes))
		{
			return FString::Printf(TEXT("材质槽 %d（%s）：材质 '%s' 的母材质没有勾 %s（引擎会静默换成默认材质）"),
				Slot, Source, *Material->GetName(), Usage);
		}
		// 编译失败：引擎同样静默换成默认材质，上面两条一条都拦不住（2026-09-29：M_TG_Grass 的花分支在像素着色器里读了
		// PreSkinnedPosition，两个花 MI 整片灰白）。只看已经出结果的那份 —— 还在编译的不算失败。
		// 静态开关不同的 MI 各有各的一份资源（一张母材质下草能编、花编不过），所以按实际画的这张 MI 问，不按母材质问。
		if (const FMaterialResource* Resource = Material->GetMaterialResource(GMaxRHIShaderPlatform))
		{
			if (Resource->IsCompilationFinished() && Resource->GetCompileErrors().Num() > 0)
			{
				return FString::Printf(TEXT("材质槽 %d（%s）：材质 '%s' 编译失败（引擎会静默换成默认材质）：%s"),
					Slot, Source, *Material->GetName(), *Resource->GetCompileErrors()[0]);
			}
		}
	}
	return FString();
}

// -----------------------------------------------------------------------------
// 烘焙出口（裁决六 ①）—— 声明处那段注释是它为什么存在与覆盖到哪
// -----------------------------------------------------------------------------

namespace
{
	// unity 构建共享 TU，file-local 一律加模块前缀（上面那组老 helper 是同一条纪律的历史遗留，
	// 名字太泛，新加的一律用 CSGpuInstanced_ 开头）。

	/** PackSnorm8888 的逆。烘焙要把常驻的切线基还原成 float，不能靠 GPU 那份。 */
	FVector4f CSGpuInstanced_UnpackSnorm8888(uint32 Packed)
	{
		auto Component = [](uint32 Byte) { return float(int8(Byte & 0xFFu)) / 127.0f; };
		return FVector4f(
			Component(Packed), Component(Packed >> 8), Component(Packed >> 16), Component(Packed >> 24));
	}

	/** ToPackedARGB 的逆，只取线性域下的 0..1 分量（顶点色在常驻流里就是 8 位量化的）。 */
	FLinearColor CSGpuInstanced_UnpackColorARGB(uint32 Packed)
	{
		const FColor Color(uint8((Packed >> 16) & 0xFFu), uint8((Packed >> 8) & 0xFFu),
			uint8(Packed & 0xFFu), uint8((Packed >> 24) & 0xFFu));
		// bSRGB=false：上传时走的是 ToFColor(false) / 资产色流原样搬，两头必须同一口径，
		// 否则烘出来的顶点色会被 sRGB 曲线拧过一道，画面上是"摆件整体变亮"而不报错。
		return FLinearColor(Color.R / 255.0f, Color.G / 255.0f, Color.B / 255.0f, Color.A / 255.0f);
	}
}

bool UCSGpuInstancedMeshComponent::ReadLiveInstanceRowsSync(TArray<FVector4f>& OutRows) const
{
	OutRows.Reset();

	// --- CPU 实例数组：行早就打包好躺在内存里，一次回读都不用付 ---
	if (!GpuInstanceSource.IsValid() && !GpuPointSource.IsValid())
	{
		OutRows = GetPackedInstances();
		return true;
	}

	// --- 点云源：这条烘不出来，理由逐字见 SaveToStaticMesh 的声明注释 ---
	if (GpuPointSource.IsValid())
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: 点云实例源没有 CPU 侧的 packed 行（代理每帧在渲染图里现打），烘不出来。"),
			*GetPathName());
		return false;
	}

	// --- GPU 实例源：先读计数器再按计数取行 ---
	//
	// 先读计数器而不是一口气把整段容量拉回来，是因为容量与活着的实例数差着数量级（门框砖
	// 512 : 152，藤更悬殊），而这段是**逐 float 走 PCIe** 的。多付一次阻塞换少读几十倍的字节，
	// 在离线烘焙上是划算的；两次阻塞都会被计数器数到，也正是想要的。
	const int32 LiveCount = DebugReadDrawnInstanceCountSync();
	if (LiveCount < 0) return false;                       // 读不到 ≠ 0 个实例
	if (LiveCount == 0) return true;                       // 真的没有实例，空集是正确答案

	const uint32 Rows = uint32(LiveCount) * CS_GPU_INSTANCED_ROW_FLOAT4S;
	TArray<uint32> Raw;
	// AuxVertex：packed 行平时停在 SRVMask（生产者的 SetBufferAccessFinal(PackedRef, SRVMask)），
	// 读完必须放回去，否则下一帧的剔除 pass 在错误的访问状态上撞见它。
	if (!CSMeshReadback::ReadUintBufferSync(GpuInstanceSource.PackedInstances, Rows * 4u,
		ECSGpuStreamRole::AuxVertex, Raw) || Raw.Num() < int32(Rows * 4u))
	{
		return false;
	}

	OutRows.SetNumUninitialized(int32(Rows));
	// buffer 是 Buffer<float4>，回读通路只认 uint32 —— 逐位重解释，不做任何数值转换。
	FMemory::Memcpy(OutRows.GetData(), Raw.GetData(), Rows * 4u * sizeof(uint32));
	return true;
}

bool UCSGpuInstancedMeshComponent::DebugReadInstanceRandomsSync(TArray<float>& OutRandoms) const
{
	OutRandoms.Reset();

	TArray<FVector4f> Row;
	if (!ReadLiveInstanceRowsSync(Row)) return false;

	OutRandoms.Reserve(Row.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
	// 第 4 行的 .w 就是 PerInstanceRandom（LocalVertexFactory.ush 的 OriginBuffer 契约）。
	for (int32 Base = 0; Base + 4 < Row.Num(); Base += CS_GPU_INSTANCED_ROW_FLOAT4S) OutRandoms.Add(Row[Base + 3].W);
	return true;
}

bool UCSGpuInstancedMeshComponent::DebugReadVisibleCountsSync(TArray<int32>& OutPerLod) const
{
	OutPerLod.Reset();

	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	if (!Resident.IsValid() || !Resident->IsAllocated() || !GpuLayout.IsValid()) return false;

	const FCSMeshResident::FStream* Counters = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::LodCounters));
	if (!Counters || !Counters->Pooled.IsValid()) return false;

	// 计数器流固定 CS_GPU_INSTANCED_MAX_LODS 格（见 CSGpuInstancedBuildAuxStreamDescs），只取布局用到的那几级。
	TArray<uint32> Values;
	if (!CSMeshReadback::ReadUintBufferSync(Counters->Pooled, CS_GPU_INSTANCED_MAX_LODS, ECSGpuStreamRole::AuxVertex, Values)) return false;
	if (Values.Num() < int32(GpuLayout.NumLODs)) return false;

	// 游标是 InterlockedAdd 先加后判的，越界的那几次也数进去了 —— 按容量钳，与 BuildArgsCS 写进 args 的是同一个数。
	OutPerLod.Reserve(int32(GpuLayout.NumLODs));
	for (uint32 Lod = 0; Lod < GpuLayout.NumLODs; ++Lod) OutPerLod.Add(int32(FMath::Min(Values[int32(Lod)], GpuLayout.InstanceCapacity)));
	return true;
}

bool UCSGpuInstancedMeshComponent::DebugReadVisibleSourceIndicesSync(TArray<TArray<int32>>& OutPerLod) const
{
	OutPerLod.Reset();
	if (!GpuLayout.IsValid() || !GpuLayout.bVisibleSourceIndex) return false;

	TArray<int32> Counts;
	if (!DebugReadVisibleCountsSync(Counts)) return false;

	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	const FCSMeshResident::FStream* Indices = Resident.IsValid()
		? Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleSourceIndices))
		: nullptr;
	if (!Indices || !Indices->Pooled.IsValid()) return false;

	// 整条流读回来再按"第 Lod 级的区段从 Lod × 容量 起"切（与 BatchElement.UserIndex 同一个算法）。
	const uint32 TotalSlots = GpuLayout.InstanceCapacity * GpuLayout.NumLODs;
	TArray<uint32> Values;
	if (!CSMeshReadback::ReadUintBufferSync(Indices->Pooled, TotalSlots, ECSGpuStreamRole::AuxVertex, Values) || Values.Num() < int32(TotalSlots)) return false;

	OutPerLod.SetNum(Counts.Num());
	for (int32 Lod = 0; Lod < Counts.Num(); ++Lod)
	{
		const int32 RegionStart = Lod * int32(GpuLayout.InstanceCapacity);
		OutPerLod[Lod].Reserve(Counts[Lod]);
		for (int32 Slot = 0; Slot < Counts[Lod]; ++Slot) OutPerLod[Lod].Add(int32(Values[RegionStart + Slot]));
	}
	return true;
}

void UCSGpuInstancedMeshComponent::HandleVisibleSourceIndexCVarChanged(IConsoleVariable* Variable)
{
	// 引擎起来之前（读 ini 设 CVar 的那几次）还没有任何组件，也没有游戏线程可言。
	if (!Variable || !GIsRunning || !IsInGameThread()) return;

	const bool bWantIndex = Variable->GetInt() != 0;
	int32 Rebuilt = 0;
	for (TObjectIterator<UCSGpuInstancedMeshComponent> It; It; ++It)
	{
		UCSGpuInstancedMeshComponent* Component = *It;
		if (!IsValid(Component) || !Component->IsRegistered()) continue;
		if (!Component->GpuLayout.IsValid() || Component->GpuLayout.bVisibleSourceIndex == bWantIndex) continue;
		// 常驻流的声明要换（两种布局的可见缓冲不是同一批流），代理要换顶点工厂：重建 + 重建渲染状态。
		Component->RebuildGpuMesh();
		Component->MarkRenderStateDirty();
		++Rebuilt;
	}
	UE_LOG(LogCSGpuInstancedMesh, Log, TEXT("r.CSGpuInstanced.VisibleSourceIndex = %d：%d 个实例组件换了可见缓冲布局。"), bWantIndex ? 1 : 0, Rebuilt);
}

#if WITH_EDITOR

UStaticMesh* UCSGpuInstancedMeshComponent::SaveToStaticMesh(const FTransform& BakeSpace,
	const FString& AssetPathAndName, bool bReplaceExistingAsset, bool bSaveAsset, bool bEnableNanite)
{
	if (!BaseMeshSnapshot.IsValid() || BaseMeshSnapshot.LODs.Num() == 0)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: 没有可用的基础网格快照，烘不出东西。"), *GetPathName());
		return nullptr;
	}

	TArray<FVector4f> Row;
	if (!ReadLiveInstanceRowsSync(Row)) return nullptr;

	// 藏起来的实例（Origin.w < 0，组件级契约，见 FCSGpuInstanceSourceGPU）不烘：画面上没有它，资产里也不该有。
	// 以前这里照烘、随机数钳成 0 —— 烘出来的门框砖在转角墩下面多出一截，而烘焙件没有任何材质能再把它藏回去。
	{
		const int32 NumRows = Row.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S;
		int32 Kept = 0;
		for (int32 Instance = 0; Instance < NumRows; ++Instance)
		{
			const int32 From = Instance * CS_GPU_INSTANCED_ROW_FLOAT4S;
			if (Row[From + 3].W < 0.0f) continue;
			if (Kept != Instance)
			{
				for (int32 R = 0; R < CS_GPU_INSTANCED_ROW_FLOAT4S; ++R) Row[Kept * CS_GPU_INSTANCED_ROW_FLOAT4S + R] = Row[From + R];
			}
			++Kept;
		}
		Row.SetNum(Kept * CS_GPU_INSTANCED_ROW_FLOAT4S);
	}

	const int32 NumInstances = Row.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S;
	if (NumInstances <= 0)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: 这一族一个（可见的）实例都没有，不产资产。"), *GetPathName());
		return nullptr;
	}

	// **只烘 LOD0**。LOD 是"离远了画什么"，而资产该带的是最全的那一份；把 GPU 当前选中的 LOD
	// 烘下来会让烘焙结果取决于烘那一刻相机在哪 —— 那是最难复现的一类差异。
	const FCSGpuInstancedLODRange& LOD0 = BaseMeshSnapshot.LODs[0];
	const int32 LodVertexCount = (BaseMeshSnapshot.LODs.Num() > 1)
		? int32(BaseMeshSnapshot.LODs[1].BaseVertex - LOD0.BaseVertex)
		: (BaseMeshSnapshot.Positions.Num() - int32(LOD0.BaseVertex));
	if (LodVertexCount < 3 || LOD0.NumIndices < 3)
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning, TEXT("%s: LOD0 不足一个三角，烘不出东西。"), *GetPathName());
		return nullptr;
	}

	// 不叫 ComponentToWorld：USceneComponent 有一个同名成员，遮蔽会被 /WX 当错误。
	const FTransform BakeComponentToWorld = GetComponentTransform();
	const int32 TotalVerts = NumInstances * LodVertexCount;
	const int32 TotalIndices = NumInstances * int32(LOD0.NumIndices);

	// 裁决六 ②「多组 UV 必须随网格保住」：快照自己就带着全部 UV 组（交错，见 FCSGpuInstancedBaseMesh::TexCoords），
	// 直接从快照取 —— 资产喂的与 SetBaseMeshFromGpuData 喂的是同一个口径。
	// 2026-09-14 以前快照只有 UV0，额外的组要回资产 LOD0 去捞，外部快照只能停在一组；实例路支持 8 组后那条旁路删了。
	const int32 NumUVChannels = BaseMeshSnapshot.NumTexCoordSets;

	FCSGpuMeshCPUData MeshData;
	// 世界空间 + 逐顶点属性：落盘层按 SourceSpace 决定要不要烘回 BakeSpace 的局部空间，
	// 与岩壳/道路那两条出口同一条判据。
	MeshData.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	MeshData.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	MeshData.Positions.SetNumUninitialized(TotalVerts);
	MeshData.Normals.SetNumUninitialized(TotalVerts);
	MeshData.Tangents.SetNumUninitialized(TotalVerts);
	MeshData.BinormalSigns.SetNumUninitialized(TotalVerts);
	MeshData.NumTexCoordChannels = NumUVChannels;
	for (int32 Channel = 0; Channel < NumUVChannels; ++Channel) MeshData.TexCoordChannels[Channel].SetNumUninitialized(TotalVerts);
	MeshData.Colors.SetNumUninitialized(TotalVerts);
	MeshData.Indices.SetNumUninitialized(TotalIndices);

	// 材质：整体覆盖设了就一格、三角全落槽 0（老口径逐位不变）；否则把资产 / 快照的材质槽表原样带过去，
	// 每个三角落在它所在材质段的槽上 —— 烘出来的资产与实例路画面是同一套材质（裁决六 ②：通道随网格保住）。
	const int32 NumLod0Triangles = int32(LOD0.NumIndices / 3);
	TArray<UMaterialInterface*> Materials;
	TArray<int32> Lod0TriangleSlots;
	Lod0TriangleSlots.Init(0, NumLod0Triangles);
	if (InstanceMaterial)
	{
		Materials.Add(InstanceMaterial.Get());
	}
	else
	{
		for (int32 Slot = 0; Slot < GetNumMaterials(); ++Slot) Materials.Add(ResolveSlotMaterial(Slot));
		for (const FCSGpuInstancedSection& Section : BaseMeshSnapshot.Sections)
		{
			if (Section.LodIndex != 0 || Section.FirstIndex < LOD0.FirstIndex) continue;
			const int32 FirstTriangle = int32((Section.FirstIndex - LOD0.FirstIndex) / 3u);
			for (int32 Triangle = FirstTriangle; Triangle < FirstTriangle + int32(Section.NumIndices / 3u); ++Triangle)
			{
				if (Lod0TriangleSlots.IsValidIndex(Triangle)) Lod0TriangleSlots[Triangle] = FMath::Max(Section.MaterialIndex, 0);
			}
		}
	}
	MeshData.TriangleMaterialSlots.SetNumUninitialized(NumInstances * NumLod0Triangles);

	for (int32 Instance = 0; Instance < NumInstances; ++Instance)
	{
		const int32 Base = Instance * CS_GPU_INSTANCED_ROW_FLOAT4S;
		for (int32 Triangle = 0; Triangle < NumLod0Triangles; ++Triangle)
		{
			MeshData.TriangleMaterialSlots[Instance * NumLod0Triangles + Triangle] = Lod0TriangleSlots[Triangle];
		}
		const FVector4f R0 = Row[Base + 0];
		const FVector4f R1 = Row[Base + 1];
		const FVector4f R2 = Row[Base + 2];
		const FVector4f Origin = Row[Base + 3];

		// 行向量约定，与 LocalVertexFactory.ush 的 GetInstanceTransform() 逐字一致：
		// P' = P.x*R0 + P.y*R1 + P.z*R2 + Origin。写成列向量会把每个非对称的旋转烘成它的转置 ——
		// 画面上是"砖全都歪了但还都在拱上"，看着像排布算错，其实是这里错了。
		const FMatrix44f InstanceToComponent(
			FPlane4f(R0.X, R0.Y, R0.Z, 0.0f),
			FPlane4f(R1.X, R1.Y, R1.Z, 0.0f),
			FPlane4f(R2.X, R2.Y, R2.Z, 0.0f),
			FPlane4f(Origin.X, Origin.Y, Origin.Z, 1.0f));
		// 法线/切线走 3x3 的**逆转置**：逐实例非均匀缩放（石阶被压成 60×100×45）下直接乘
		// 会把法线拧歪，而歪掉的法线只表现为"这一批的明暗不太对"，没有任何断言看得见。
		const FMatrix44f NormalMatrix = InstanceToComponent.Inverse().GetTransposed();
		// **逐实例随机进顶点色 alpha** —— 类注释那份通道字典的另一半（GPU 路那一半在
		// ClearPackedColorAlpha）。烘完之后 PerInstanceRandom 恒为 0，不写这一条的症状是
		// 整片同色，而且一条断言都不会红。
		const float InstanceRandom01 = FMath::Clamp(Origin.W, 0.0f, 1.0f);

		const int32 DstVertexBase = Instance * LodVertexCount;
		for (int32 V = 0; V < LodVertexCount; ++V)
		{
			const int32 Src = int32(LOD0.BaseVertex) + V;
			const int32 Dst = DstVertexBase + V;

			const FVector4f Local4 = InstanceToComponent.TransformPosition(BaseMeshSnapshot.Positions[Src]);
			MeshData.Positions[Dst] = FVector3f(
				BakeComponentToWorld.TransformPosition(FVector(Local4.X, Local4.Y, Local4.Z)));

			const FVector4f TangentX = CSGpuInstanced_UnpackSnorm8888(BaseMeshSnapshot.TangentBasis[Src * 2 + 0]);
			const FVector4f TangentZ = CSGpuInstanced_UnpackSnorm8888(BaseMeshSnapshot.TangentBasis[Src * 2 + 1]);
			const FVector4f Normal4 = NormalMatrix.TransformVector(FVector3f(TangentZ.X, TangentZ.Y, TangentZ.Z));
			const FVector4f Tangent4 = InstanceToComponent.TransformVector(FVector3f(TangentX.X, TangentX.Y, TangentX.Z));
			// 组件变换那一段只取旋转：组件本身带非均匀缩放的话法线同样要走逆转置，但实例组件
			// 一律以父 actor 的变换挂着（缩放为 1），为一个不存在的情形多算一次逆矩阵不划算。
			MeshData.Normals[Dst] = FVector3f(BakeComponentToWorld
				.TransformVectorNoScale(FVector(Normal4.X, Normal4.Y, Normal4.Z)).GetSafeNormal());
			MeshData.Tangents[Dst] = FVector3f(BakeComponentToWorld
				.TransformVectorNoScale(FVector(Tangent4.X, Tangent4.Y, Tangent4.Z)).GetSafeNormal());
			MeshData.BinormalSigns[Dst] = TangentZ.W >= 0.0f ? 1.0f : -1.0f;

			for (int32 Channel = 0; Channel < NumUVChannels; ++Channel) MeshData.TexCoordChannels[Channel][Dst] = BaseMeshSnapshot.TexCoords[Src * NumUVChannels + Channel];

			const FLinearColor Color = CSGpuInstanced_UnpackColorARGB(BaseMeshSnapshot.Colors[Src]);
			MeshData.Colors[Dst] = FVector4f(Color.R, Color.G, Color.B, InstanceRandom01);
		}

		const int32 DstIndexBase = Instance * int32(LOD0.NumIndices);
		for (uint32 I = 0; I < LOD0.NumIndices; ++I)
		{
			// 索引在快照里是**相对本 LOD 顶点块**的（DrawIndexedIndirect 加 BaseVertexLocation），
			// 展开时既要去掉那个基址、又要加上本实例在输出数组里的基址。
			const uint32 Local = BaseMeshSnapshot.Indices[int32(LOD0.FirstIndex + I)];
			MeshData.Indices[DstIndexBase + int32(I)] = uint32(DstVertexBase) + Local;
		}
	}

	FCSGpuMeshConvertOptions ConvertOptions;
	ConvertOptions.TargetTransform = BakeSpace;
	ConvertOptions.bBakeToLocalSpace = true;

	FCSGpuMeshAssetOptions AssetOptions;
	AssetOptions.AssetPath = AssetPathAndName.TrimStartAndEnd();
	AssetOptions.bTransient = false;   // 这条出口存在的全部意义就是产资产
	AssetOptions.bReplaceExisting = bReplaceExistingAsset;
	AssetOptions.bSaveToDisk = bSaveAsset;
	AssetOptions.bEnableNanite = bEnableNanite;

	UE_LOG(LogCSGpuInstancedMesh, Log, TEXT("%s: 烘焙 %d 个实例 × %d 顶点 = %d 顶点 / %d 三角。"),
		*GetPathName(), NumInstances, LodVertexCount, TotalVerts, TotalIndices / 3);

	return UCSGpuMeshComponent::BuildStaticMesh(this, GetOwner(), MeshData, Materials, ConvertOptions, AssetOptions);
}

#endif // WITH_EDITOR

// -----------------------------------------------------------------------------
// Cluster build
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::RebuildInstanceData(bool bRebuildGpuMesh)
{
	RebuildInstancePacking();
	if (bRebuildGpuMesh) RebuildGpuMesh();
}

void UCSGpuInstancedMeshComponent::RebuildInstancePacking()
{
	bInstancePackingDirty = false;
	++InstancePackSerial;
	PackedInstances.Reset();
	ClusterBounds.Reset();
	PackedRowSlots.Reset();

	if (GpuInstanceSource.IsValid())
	{
		// The instance set lives on the GPU; only the bounds are the component's business.
		LocalBounds = GpuInstanceSource.LocalBounds;
		return;
	}

	// 快照有就是快照的（经典路的口径一字不变），Nanite 路没读顶点时退到资产包围盒。
	const FBox BaseBounds = GetBaseLocalBounds();

	if (GpuPointSource.IsValid())
	{
		// Point positions are world-space, so the bounds arrive world-space too; the base mesh's
		// own extent has to be added because an instance stands out of its point.
		const FBox WorldBounds = GpuPointSource.WorldBounds;
		LocalBounds = WorldBounds.IsValid ? WorldBounds.InverseTransformBy(GetComponentTransform()) : FBox(ForceInit);
		if (LocalBounds.IsValid && BaseBounds.IsValid)
		{
			const double Reach = BaseBounds.GetExtent().Size() * FMath::Max(GpuPointSource.InstanceScale, UE_KINDA_SMALL_NUMBER);
			LocalBounds = LocalBounds.ExpandBy(Reach);
		}
		return;
	}

	const int32 NumInstances = PerInstanceTransforms.Num();
	if (NumInstances == 0 || !BaseBounds.IsValid)
	{
		LocalBounds = FBox(ForceInit);
		return;
	}

	// Per-instance culling sphere: the base mesh's LOD0 sphere pushed through the instance
	// transform. Non-uniform scale is handled conservatively by the largest axis.
	const FVector BaseCentre = BaseBounds.GetCenter();
	const float BaseRadius = float(BaseBounds.GetExtent().Size());

	TArray<FVector3f> Centres;
	TArray<float> Radii;
	Centres.SetNumUninitialized(NumInstances);
	Radii.SetNumUninitialized(NumInstances);

	FBox TotalBounds(ForceInit);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		const FTransform& T = PerInstanceTransforms[i];
		const FVector Centre = T.TransformPosition(BaseCentre);
		const FVector AbsScale = T.GetScale3D().GetAbs();
		const float Radius = BaseRadius * float(FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z));

		Centres[i] = FVector3f(Centre);
		Radii[i] = Radius;
		TotalBounds += FBox(Centre - FVector(Radius), Centre + FVector(Radius));
	}
	LocalBounds = TotalBounds;

	// Morton order gives the clusters spatial locality, which is what makes the coarse cull
	// level worth running at all. A real BVH would reject more; this costs one sort.
	const FVector3f Origin = FVector3f(TotalBounds.Min);
	const FVector3f Size = FVector3f(TotalBounds.GetSize()).ComponentMax(FVector3f(UE_KINDA_SMALL_NUMBER));

	// 基数排序（稳定：同码的按数组下标）。以前是对下标数组做比较排序、每次比较间接读一次码表，是单次增删里
	// 最大的一块：10 万个实例一次增删 13.5 ms → 7.3 ms（2026-10-02 实测，Docs/InstanceCulling/Step3_20261002.md）。
	struct FMortonEntry
	{
		uint32 Code;
		int32 Index;
	};
	TArray<FMortonEntry> Unsorted;
	TArray<FMortonEntry> Order;
	Unsorted.SetNumUninitialized(NumInstances);
	Order.SetNumUninitialized(NumInstances);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		Unsorted[i].Code = MortonCode((Centres[i] - Origin) / Size);
		Unsorted[i].Index = i;
	}
	RadixSort32(Order.GetData(), Unsorted.GetData(), uint32(NumInstances), [](const FMortonEntry& Entry) { return Entry.Code; });

	const int32 ClusterSize = FMath::Clamp(InstancesPerCluster, 1, 4096);
	const int32 NumClusters = FMath::DivideAndRoundUp(NumInstances, ClusterSize);

	PackedInstances.SetNumUninitialized(NumInstances * CS_GPU_INSTANCED_ROW_FLOAT4S);
	ClusterBounds.SetNumUninitialized(NumClusters);
	// 行 → 稳定槽。逐实例随机数按槽取、不按数组下标取：RemoveAtSwap 把末尾实例换进空位，按下标取的话它的随机数就变了。
	EnsureInstanceIdentity();
	PackedRowSlots.SetNumUninitialized(NumInstances);

	for (int32 Cluster = 0; Cluster < NumClusters; ++Cluster)
	{
		const int32 First = Cluster * ClusterSize;
		const int32 Count = FMath::Min(ClusterSize, NumInstances - First);

		FVector3f Min(UE_BIG_NUMBER), Max(-UE_BIG_NUMBER);
		for (int32 Slot = 0; Slot < Count; ++Slot)
		{
			const int32 Src = Order[First + Slot].Index;
			const FTransform& T = PerInstanceTransforms[Src];
			const FMatrix44f M = FMatrix44f(T.ToMatrixWithScale());

			// Rows of the instance-to-component 3x3 + the origin, exactly the layout
			// LocalVertexFactory.ush's GetInstanceTransform() reconstructs.
			const int32 Dst = (First + Slot) * CS_GPU_INSTANCED_ROW_FLOAT4S;
			const int32 StableSlot = PerInstanceSlots[Src];
			PackedRowSlots[First + Slot] = StableSlot;
			PackedInstances[Dst + 0] = FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f);
			PackedInstances[Dst + 1] = FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f);
			PackedInstances[Dst + 2] = FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f);
			PackedInstances[Dst + 3] = FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], InstanceRandom(StableSlot));
			PackedInstances[Dst + 4] = FVector4f(Centres[Src], Radii[Src]);

			Min = Min.ComponentMin(Centres[Src] - FVector3f(Radii[Src]));
			Max = Max.ComponentMax(Centres[Src] + FVector3f(Radii[Src]));
		}

		const FVector3f Centre = (Min + Max) * 0.5f;
		ClusterBounds[Cluster] = FVector4f(Centre, (Max - Centre).Size());
	}
}

// -----------------------------------------------------------------------------
// The retained buffer set
// -----------------------------------------------------------------------------

uint32 UCSGpuInstancedMeshComponent::ResolveInstanceCapacity(uint32 LiveInstanceCount) const
{
	// A floor, so a handful of instances still gets a buffer worth having and the first few
	// AddInstance calls change nothing.
	constexpr uint32 MinCapacity = 64u;

	const uint32 Held = GpuLayout.InstanceCapacity;
	const bool bTooSmall = LiveInstanceCount > Held;
	// Hysteresis: only hand capacity back once three quarters of it are idle. Mirrors what
	// UCSMesh::ShrinkSlackRatio does for the geometry streams, and for the same reason — an
	// instance set that oscillates must not churn its buffers on every swing.
	const bool bMostlyIdle = LiveInstanceCount * 4u < Held;
	if (!bTooSmall && !bMostlyIdle) return FMath::Max(Held, LiveInstanceCount);

	return FMath::Max(MinCapacity, LiveInstanceCount + LiveInstanceCount / 2u);
}

void UCSGpuInstancedMeshComponent::ReleaseGpuMesh()
{
	GpuLayout = FCSGpuInstancedGpuLayout();
	if (!InstancedGpuMesh) return;

	// Not merely unbound: the buffers were sized for an instance set that no longer exists, and a
	// component with nothing to draw is exactly when the VRAM is worth handing back. The live proxy
	// keeps its own references to the pooled buffers, so it goes on drawing from valid memory until
	// the render-state recreation its caller is about to trigger replaces it.
	const FCSMeshResident* Resident = InstancedGpuMesh->GetResidentPtr();
	if (Resident && Resident->IsAllocated()) InstancedGpuMesh->ReleaseSync();
}

void UCSGpuInstancedMeshComponent::RebuildGpuMesh()
{
	// 成批区间里 / bMarkRenderStateDirty=false 攒下的改动还没打包的话，先补上：传上去的行必须是实例数组此刻的样子，
	// 不是上一次打包时的样子（CVar 换布局、重注册这些不经 mutator 的入口也会走到这里）。
	EnsureInstancePacking();

	// Deferred to OnRegister while unregistered. Nothing can draw an unregistered component, and
	// this blocks on a render flush — PostLoad rebuilds every one of these in the level, and a
	// render round trip each would show up as load time for no visible result.
	//
	// ⚠️ 这里以前是 ReleaseGpuMesh()，理由写的是"缓冲已经与实例集不符"。但 Release 本身就是
	// **一次阻塞刷新**（正是这段注释想省掉的那种），而且把常驻缓冲整套还回去，重注册时只能
	// AllocateSync + EditMeshSync 从头再来。编辑器改一个属性 ⇒ RerunConstructionScripts ⇒
	// 卸载全部组件 → 跑构造脚本 → 重注册，于是**拖尺寸的每一帧**都白付 3 次阻塞刷新
	// （实测 12 帧 36 次，与容量/包围盒/实例数全都无关）。
	// 正确做法是只记脏：不许画的东西不需要马上正确，OnRegister 会在建代理**之前**补上。
	if (!IsRegistered())
	{
		bGpuMeshDirty = true;
		return;
	}

	if (IsNaniteRenderPath())
	{
		UpdateNaniteComponent();
		if (!IsClassicShadowCopyActive())
		{
			// Nanite 路不画自己的常驻网格：基础网格是资产自己的 Nanite 数据，实例进 GPU-Scene。上一种模式
			// 留下的常驻缓冲按"没东西画"的口径还回去（没分配时 ReleaseGpuMesh 是空操作，不会每次都刷新）。
			ReleaseGpuMesh();
			bGpuMeshDirty = false;
			return;
		}
		// 影子副本（bClassicShadowOnNanitePath）：替身画主通道，下面照经典路建常驻网格与剔除，代理只投影。
		// 快照读的是资产的回退 LOD —— 编辑器里总读得到，编辑器外要资产开 Allow CPU Access（读不到时下面
		// 按"没东西画"退掉，只是没有影子，主通道不受影响）。
	}
	else
	{
		// 从 Nanite 切回来，或 BaseMesh 换成了外部快照：替身不该再画。
		ReleaseNaniteComponent();
	}

	const bool bPackedGpuSource = GpuInstanceSource.IsValid();
	const bool bPointGpuSource = GpuPointSource.IsValid();
	const bool bHasInstances = bPackedGpuSource || bPointGpuSource || PackedInstances.Num() > 0;
	if (!BaseMeshSnapshot.IsValid() || !bHasInstances)
	{
		ReleaseGpuMesh();
		return;
	}

	// --- what the buffers have to be sized for
	FCSGpuInstancedGpuLayout NewLayout;
	NewLayout.NumLODs = uint32(FMath::Clamp(BaseMeshSnapshot.LODs.Num(), 1, CS_GPU_INSTANCED_MAX_LODS));
	// 一个材质段一个 arg set。快照保证段数在 [1, CS_GPU_INSTANCED_MAX_DRAWS] 且每段的 LOD 都在 LODs 里（IsValid 查过）。
	NewLayout.NumDraws = uint32(FMath::Clamp(BaseMeshSnapshot.Sections.Num(), 1, CS_GPU_INSTANCED_MAX_DRAWS));
	if (bPackedGpuSource || bPointGpuSource)
	{
		// Instances live on the GPU: there is no cluster table to build from, so the coarse level is
		// skipped and every instance goes through the fine cull. The producer's buffer capacity is
		// already a capacity, so it needs no ratchet of ours; its counter carries the live count.
		NewLayout.InstanceCapacity = bPackedGpuSource ? GpuInstanceSource.Capacity : GpuPointSource.Capacity;
		NewLayout.NumSourceInstances = NewLayout.InstanceCapacity;
	}
	else
	{
		NewLayout.NumSourceInstances = uint32(PackedInstances.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
		NewLayout.ClusterSize = uint32(FMath::Clamp(InstancesPerCluster, 1, 4096));
		NewLayout.InstanceCapacity = ResolveInstanceCapacity(NewLayout.NumSourceInstances);
		NewLayout.NumClusters = uint32(FMath::DivideAndRoundUp(NewLayout.NumSourceInstances, NewLayout.ClusterSize));
	}
	if (!NewLayout.IsValid())
	{
		ReleaseGpuMesh();
		return;
	}
	// 可见缓冲的形态是布局的一部分：这里定下来，声明常驻流、建代理、剔除都只看布局，不再各自去读 CVar。
	NewLayout.bVisibleSourceIndex = CVarCSGpuInstancedVisibleSourceIndex.GetValueOnGameThread() != 0;

	if (!InstancedGpuMesh) InstancedGpuMesh = NewObject<UCSMesh>(this);
	// One entry, and not the material anything draws with — the proxy draws each section with the
	// material GetSectionMaterial resolves for it. This is where a save of the base mesh gets its
	// single slot from, which is the same table the material-id stream is cleared to index.
	InstancedGpuMesh->SetMaterial(0, GetSectionMaterial(0));

	// --- declare the stream set: one indirect arg set per material section (over all LODs), plus
	// this leaf's aux streams.
	//
	// Declared at the instance capacity the mesh ALREADY holds, not at the new one. A re-declaration
	// reallocates and copies every resident stream, the base-mesh geometry included, and the only
	// thing another instance changes is how many instances the aux streams have room for — which is
	// a per-stream resize, applied right below. So the declaration only moves when the shape of the
	// set moves: a LOD or a material section gained or lost, a different cluster size, a packed GPU
	// source appearing and taking the source-row stream down to a placeholder, or a base mesh with a
	// different UV set count.
	FCSGpuInstancedGpuLayout DeclaredLayout = NewLayout;
	if (GpuLayout.IsValid()) DeclaredLayout.InstanceCapacity = GpuLayout.InstanceCapacity;

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = DeclaredLayout.NumDraws;
	// UV 组数跟基础网格走：TexCoord 流加宽成每顶点 2×N 个 float（交错），代理按流宽把 NumTexCoords 设成 N
	// （CSGpuMeshSceneProxy.cpp 的 TexCoord 分支），材质的 TexCoord[1..N-1] 才读得到东西 —— 只声明一组的话，
	// manual fetch 把越界的组钳回最后一组，UV1/UV2 读出来全是 UV0，**不报错**。
	// 组数变了就是布局变了：SetStreamLayoutSync 重分配并拷贝，加宽的这条流回来是零，下面的上传整条重写。
	StreamLayout.NumTexCoordSets = uint32(BaseMeshSnapshot.NumTexCoordSets);
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, DeclaredLayout, bPackedGpuSource);

	// Refused rather than partially applied, and the return value is the only signal: a slot that
	// collides with a stream the standard set already owns is dropped, and a dropped stream is a
	// null buffer bound at draw time with nothing in the log. See ECSGpuInstancedAuxSlot.
	if (!InstancedGpuMesh->SetStreamLayoutSync(StreamLayout))
	{
		UE_LOG(LogCSGpuInstancedMesh, Error,
			TEXT("%s: the GPU mesh refused this leaf's stream layout (%u LODs, %u draws, %u instance slots). Nothing will be drawn."),
			*GetPathName(), DeclaredLayout.NumLODs, DeclaredLayout.NumDraws, DeclaredLayout.InstanceCapacity);
		ReleaseGpuMesh();
		return;
	}

	// --- bring the instance-sized streams to the capacity this rebuild wants, one buffer at a time.
	//
	// Built from the same descriptor builder the declaration uses, so the two can never disagree
	// about what a stream at a given capacity looks like; entries already at their target size are
	// no-ops, which is what makes a rebuild that changed nothing free. Resized streams come back
	// zeroed — the rows and the cluster spheres are re-uploaded by the edit below, and every other
	// one of them is rewritten in full by the cull before anything reads it.
	TArray<FCSGpuStreamDesc> WantedAux;
	CSGpuInstancedBuildAuxStreamDescs(WantedAux, NewLayout, bPackedGpuSource);

	TArray<FCSMeshStreamResize> Resizes;
	Resizes.Reserve(WantedAux.Num());
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		FCSMeshStreamResize& Resize = Resizes.AddDefaulted_GetRef();
		Resize.Role = Desc.Role;
		Resize.SlotIndex = Desc.TexCoordIndex;
		Resize.ElementCount = Desc.ElementsPerUnit;
	}
	if (!InstancedGpuMesh->ResizeStreamsSync(Resizes))
	{
		UE_LOG(LogCSGpuInstancedMesh, Error,
			TEXT("%s: the GPU mesh refused to resize this leaf's aux streams to %u instance slots. Nothing will be drawn."),
			*GetPathName(), NewLayout.InstanceCapacity);
		ReleaseGpuMesh();
		return;
	}

	// --- size it for the base mesh. Both counts are exact here (unlike the road, whose emitted size
	// only the GPU knows), which is what lets the shrink below run without a counter readback.
	const int32 NumVertices = BaseMeshSnapshot.Positions.Num();
	const int32 NumIndices = BaseMeshSnapshot.Indices.Num();
	if (!InstancedGpuMesh->EnsureCapacitySync(NumVertices, NumIndices))
	{
		UE_LOG(LogCSGpuInstancedMesh, Warning,
			TEXT("%s: capacity for %d vertices / %d indices was refused; the instanced mesh will not be drawn."),
			*GetPathName(), NumVertices, NumIndices);
		ReleaseGpuMesh();
		return;
	}

	// --- upload
	const FCSGpuInstancedBaseMesh& Mesh = BaseMeshSnapshot;
	const TArray<FVector4f>& Rows = PackedInstances;
	const TArray<FVector4f>& Clusters = ClusterBounds;
	const FBox DrawnWorldBounds = LocalBounds.IsValid ? LocalBounds.TransformBy(GetComponentTransform()) : FBox(ForceInit);
	const bool bUploadRows = !bPackedGpuSource && Rows.Num() > 0;

	bool bUploaded = false;
	InstancedGpuMesh->EditMeshSync([&Mesh, &Rows, &Clusters, &DrawnWorldBounds, bUploadRows, &bUploaded](FCSMeshEditContext& Context)
	{
		FRDGBuilder& GraphBuilder = Context.GraphBuilder;

		FRDGBufferRef Positions = Context.Positions();
		FRDGBufferRef Tangents = Context.Tangents();
		FRDGBufferRef TexCoords = Context.TexCoords();
		FRDGBufferRef Colors = Context.Colors();
		FRDGBufferRef Indices = Context.Indices();
		FRDGBufferRef IndirectArgs = Context.IndirectArgs();
		FRDGBufferRef MeshCounters = Context.Counters();
		if (!Positions || !Tangents || !TexCoords || !Colors) return;
		if (!Indices || !IndirectArgs || !MeshCounters) return;

		// ERDGInitialDataFlags::None makes RDG take its own copy, so these arrays only have to
		// outlive the call — which EditMeshSync's flush guarantees anyway.
		GraphBuilder.QueueBufferUpload(Positions, Mesh.Positions.GetData(), Mesh.Positions.Num() * sizeof(FVector3f), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Tangents, Mesh.TangentBasis.GetData(), Mesh.TangentBasis.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(TexCoords, Mesh.TexCoords.GetData(), Mesh.TexCoords.Num() * sizeof(FVector2f), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Colors, Mesh.Colors.GetData(), Mesh.Colors.Num() * sizeof(uint32), ERDGInitialDataFlags::None);
		GraphBuilder.QueueBufferUpload(Indices, Mesh.Indices.GetData(), Mesh.Indices.Num() * sizeof(uint32), ERDGInitialDataFlags::None);

		// The readback path (ReadbackMeshSync / save-to-StaticMesh) sees the single base-mesh copy,
		// not the instanced result — the instances only ever exist as transforms.
		const uint32 Counters[2] = { uint32(Mesh.Positions.Num()), uint32(Mesh.Indices.Num()) };
		GraphBuilder.QueueBufferUpload(MeshCounters, Counters, sizeof(Counters), ERDGInitialDataFlags::None);

		// Zeroed until the first cull pass runs, so an unculled frame draws nothing rather than
		// whatever the buffer pool's previous tenant left in those five uints per draw.
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IndirectArgs, PF_R32_UINT)), 0u);

		// This leaf writes no material ids, but every UCSMesh carries that stream and the save path
		// reads it back as the per-triangle slot. Left holding the pool's previous tenant, a
		// one-material base mesh would come out of the saver scattered across dozens of slots that
		// have no materials behind them. Zero is this mesh's only slot.
		FRDGBufferRef MaterialIds = Context.MaterialIds();
		if (MaterialIds) AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(MaterialIds, PF_R32_UINT)), 0u);

		// 「可见源行号」布局的那一行零（生产者没给 custom data 时材质读它）：池里拿出来的 buffer 不保证是零，
		// 而它存在的全部意义就是"读到 0"。旧布局没有这条流，Find 返回空。
		if (FRDGBufferRef CustomDataFallback = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::CustomDataFallback)))
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CustomDataFallback, PF_R32_UINT)), 0u);
		}

		FRDGBufferRef SourceInstances = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::SourceInstances));
		FRDGBufferRef ClusterBoundsBuffer = Context.Find(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::ClusterBounds));
		if (!SourceInstances || !ClusterBoundsBuffer) return;

		if (bUploadRows) GraphBuilder.QueueBufferUpload(SourceInstances, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
		if (Clusters.Num() > 0) GraphBuilder.QueueBufferUpload(ClusterBoundsBuffer, Clusters.GetData(), Clusters.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);

		// No SetStandardStreamAccessFinal and no GraphBuilder.Execute() here: both belong to
		// EditMeshSync, which restores every resident stream's access state for the whole edit.
		// Doing either by hand is the failure that has no symptom but "it stopped drawing".

		// The arg sets here are this leaf's own draw table (one per material section of each LOD, laid
		// out by the snapshot and consumed only by FCSGpuInstancedMeshSceneProxy), not UCSMesh's
		// material-run section table. Publishing one would have the render side pair arg set i with
		// UCSMesh material i, which for this mesh means nothing.
		UCSMeshOps::InvalidateSections(Context);
		// Stated exactly, which is what lets the shrink below skip the counter readback that a
		// GPU-decided size would force (a full stall) before it even reaches its own hysteresis.
		Context.SetKnownCounts(Mesh.Positions.Num(), Mesh.Indices.Num());
		// The streams hold one component-local copy of the base mesh; this box is where the drawn
		// instances are. They deliberately disagree — the instanced result has no vertices anywhere
		// — and this is the answer anything asking "where is this mesh" wants.
		Context.SetWorldBounds(DrawnWorldBounds);
		bUploaded = true;
	});

	if (!bUploaded)
	{
		UE_LOG(LogCSGpuInstancedMesh, Error, TEXT("%s: the base-mesh upload found the GPU mesh missing streams; nothing will be drawn."), *GetPathName());
		ReleaseGpuMesh();
		return;
	}

	// Retention introduces a ratchet the proxy-owned path never had: the buffers used to be
	// reallocated from scratch on every proxy rebuild, and now only EnsureCapacitySync moves them,
	// which never goes down. Without this, swapping a 100k-vertex base mesh for a 500-vertex one
	// keeps the larger allocation for the rest of the session.
	//
	// After the upload, not before it: the shrink refuses to go below the mesh's live counts, and
	// until the edit above ran those were the *previous* base mesh's — which is exactly the case
	// worth shrinking. The surviving contents are copied across, so the freshly uploaded mesh
	// arrives intact on the other side.
	if (NumVertices < InstancedGpuMesh->GetVertexCapacity() || NumIndices < InstancedGpuMesh->GetIndexCapacity())
	{
		InstancedGpuMesh->ShrinkCapacitySync(NumVertices, NumIndices);
	}

	GpuLayout = NewLayout;
	bGpuMeshDirty = false;
}

// -----------------------------------------------------------------------------
// UObject / UPrimitiveComponent
// -----------------------------------------------------------------------------

void UCSGpuInstancedMeshComponent::OnRegister()
{
	Super::OnRegister();

	// 只在**真有东西被跳过**时重建。GPU 网格是常驻的（InstancedGpuMesh 是组件自己的 UObject，
	// 卸载不动它），代理只是重新从常驻集取视图 —— 布局没变、缓冲还在的话一个字节都不用重传。
	// 无条件重建的代价是 RebuildGpuMesh 尾部那次**无条件的** EditMeshSync：一次阻塞刷新，
	// 每次重注册都付，而编辑器改任一属性都会走一遍卸载/重注册。
	if (bGpuMeshDirty || !GpuLayout.IsValid()) RebuildGpuMesh();
}

void UCSGpuInstancedMeshComponent::OnUnregister()
{
	// 只下线不销毁：重注册（编辑器改一个属性就会整体卸载 / 重注册一遍）时 UpdateNaniteComponent
	// 原样挂回去，源和设置都还在替身身上。
	if (NaniteComponent && NaniteComponent->IsRegistered()) NaniteComponent->UnregisterComponent();
	Super::OnUnregister();
}

void UCSGpuInstancedMeshComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{
#if WITH_EDITOR
	UnbindBaseMeshRebuildEvent();
#endif
	ReleaseNaniteComponent();

	// 显存当场还回去，不等 GC（2026-09-07 审查 B7）：本组件手上的实例源引用与 InstancedGpuMesh 的常驻流
	// （可见槽区 = 容量 × LOD 数 × 80 B）以前都拖到 UObject 被回收才放。**这是本组件那一份的唯一出口**：
	// 删 actor、DestroyComponent、GC 三条路都会走到这里，生产者不必（也不该）在销毁前替它撤源 ——
	// 生产者只放它自己分配的那批 buffer（ACSTinyGlade::ReleaseInstancedBuffers）。
	GpuInstanceSource.Reset();
	GpuPointSource.Reset();
	// ReleaseGpuMesh 是一次计数 flush。构造脚本建出来的组件在每次重跑构造脚本时都会被引擎销毁重建，
	// 那条路落在"改一个属性"上，一次 flush 都不许有 —— 那种组件只撂下引用，让 GC 走
	// UCSMesh::BeginDestroy 的无阻塞释放；其余（原生 / 实例组件）当场释放。
	if (CreationMethod == EComponentCreationMethod::UserConstructionScript) InstancedGpuMesh = nullptr;
	else ReleaseGpuMesh();
	GpuLayout = FCSGpuInstancedGpuLayout();
	Super::OnComponentDestroyed(bDestroyingHierarchy);
}

void UCSGpuInstancedMeshComponent::PostLoad()
{
	Super::PostLoad();

	if (BaseMesh) BaseMesh->ConditionalPostLoad();
	// 两张身份表是刚读进来的，缓存还是构造时的空壳：下面这次重建的第一步就会按真值表对齐（老资产没有槽表，在那里补发）。
	bInstanceIdentityCacheDirty = true;
	RebuildBaseMeshSnapshot();
}

#if WITH_EDITOR
void UCSGpuInstancedMeshComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UCSGpuInstancedMeshComponent, BaseMesh))
	{
		bBaseMeshIsExternal = false;
		RebuildBaseMeshSnapshot();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UCSGpuInstancedMeshComponent, InstancesPerCluster))
	{
		RebuildInstanceData();
		MarkRenderStateDirty();
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UCSGpuInstancedMeshComponent, bClassicShadowOnNanitePath))
	{
		// 影子副本要不要常驻网格变了：建 / 还一套经典缓冲（Nanite 路之外这一条什么都不改）。
		RebuildGpuMesh();
		MarkRenderStateDirty();
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UCSGpuInstancedMeshComponent::PostEditUndo()
{
	Super::PostEditUndo();
	// 事务把 PerInstanceTransforms / PerInstanceSlots / SlotGenerations 整套换回去了，没经过任何 mutator。
	// 这里只把缓存标脏（下一次查询或增删时按真值表重建），不重打 packed 行：那要一次阻塞刷新，
	// 而撤销任何一个 actor 属性都会走到它的每一个组件的这个钩子上。
	bInstanceIdentityCacheDirty = true;
}

void UCSGpuInstancedMeshComponent::BindBaseMeshRebuildEvent()
{
	if (RebuildEventMesh.Get() == BaseMesh && RebuildEventHandle.IsValid()) return;

	UnbindBaseMeshRebuildEvent();
	if (!BaseMesh || bBaseMeshIsExternal) return;

	RebuildEventHandle = BaseMesh->OnPostMeshBuild().AddUObject(this, &UCSGpuInstancedMeshComponent::HandleBaseMeshRebuilt);
	RebuildEventMesh = BaseMesh;
}

void UCSGpuInstancedMeshComponent::UnbindBaseMeshRebuildEvent()
{
	if (UStaticMesh* Mesh = RebuildEventMesh.Get()) Mesh->OnPostMeshBuild().Remove(RebuildEventHandle);
	RebuildEventHandle.Reset();
	RebuildEventMesh.Reset();

	FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.Remove(DeferredRefreshHandle);
	DeferredRefreshHandle.Reset();
}

void UCSGpuInstancedMeshComponent::HandleBaseMeshRebuilt(UStaticMesh* RebuiltMesh)
{
	if (RebuiltMesh != BaseMesh || bBaseMeshIsExternal) return;
	// 连着重建几次只挂一个：重判是幂等的，做一次就够。
	if (DeferredRefreshHandle.IsValid()) return;

	DeferredRefreshHandle = FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.AddUObject(this, &UCSGpuInstancedMeshComponent::HandleDeferredBaseMeshRefresh);
}

void UCSGpuInstancedMeshComponent::HandleDeferredBaseMeshRefresh(UWorld* InWorld)
{
	if (InWorld != GetWorld()) return;

	FWorldDelegates::OnWorldPreSendAllEndOfFrameUpdates.Remove(DeferredRefreshHandle);
	DeferredRefreshHandle.Reset();

	// 同一个入口重新读资产：Nanite 开关变了就换路（RebuildGpuMesh 里拆一边建一边），没变也把 GPU 剔除路的
	// 快照换成重建后的顶点。未注册时它只记脏，OnRegister 会补上。
	UE_LOG(LogCSGpuInstancedMesh, Log, TEXT("%s: base mesh '%s' was rebuilt; re-reading it (Nanite path: %s)."),
		*GetPathName(), *GetNameSafe(BaseMesh), IsNaniteRenderPath() ? TEXT("yes") : TEXT("no"));
	RebuildBaseMeshSnapshot();
}
#endif

FPrimitiveSceneProxy* UCSGpuInstancedMeshComponent::CreateSceneProxy()
{
	if (GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5) return nullptr;
	// Nanite 路画的是替身（UCSGpuInstancedNaniteComponent），本组件自己不进场景 —— 除非要一份只投影的经典副本，
	// 而那份在不投影时也不必存在（改 CastShadow 会重建渲染状态，到时再建）。
	if (IsNaniteRenderPath() && !(IsClassicShadowCopyActive() && CastShadow && bCastDynamicShadow)) return nullptr;

	// The one gate now, and it stands for all the old ones: RebuildGpuMesh releases the buffers
	// unless there is a valid base mesh, a non-empty instance set and a layout the mesh accepted, so
	// an allocation existing is the same statement as "there is something to draw". Note this is
	// only a *read* — a proxy creation must never build geometry, since it can run off the game
	// thread during the end-of-frame update where a blocking flush is not allowed.
	const FCSMeshResidentRef Resident = InstancedGpuMesh ? InstancedGpuMesh->GetResident() : FCSMeshResidentRef();
	if (!Resident.IsValid() || !Resident->IsAllocated() || !GpuLayout.IsValid()) return nullptr;

	// 每个材质段一张材质（整体覆盖 > 资产 / 快照材质槽）。The vertex factory only compiles for materials
	// flagged for instancing; this both flags the material in the editor and warns when it cannot be
	// flagged (same call ISM makes). 不合格的那一段画默认材质 —— **不再把 InstanceMaterial 就地置空**：
	// 那会把"整体覆盖"悄悄变成"用资产材质"，而用户在属性面板里看到的还是原来那张。
	TArray<UMaterialInterface*> DrawMaterials;
	DrawMaterials.Reserve(BaseMeshSnapshot.Sections.Num());
	for (int32 Section = 0; Section < BaseMeshSnapshot.Sections.Num(); ++Section)
	{
		UMaterialInterface* SectionMaterial = GetSectionMaterial(Section);
		if (SectionMaterial && !SectionMaterial->CheckMaterialUsage_Concurrent(MATUSAGE_InstancedStaticMeshes))
		{
			if (!bWarnedUnusableMaterial)
			{
				bWarnedUnusableMaterial = true;
				UE_LOG(LogCSGpuInstancedMesh, Warning,
					TEXT("%s: section %d material '%s' is not usable with instanced static meshes; that section draws with the default material."),
					*GetPathName(), Section, *GetNameSafe(SectionMaterial));
			}
			SectionMaterial = nullptr;
		}
		DrawMaterials.Add(SectionMaterial);   // null → the proxy substitutes the default surface material
	}

	// The per-frame cull passes are driven by a shared view extension; create it here rather than
	// from the proxy so registration stays on the game thread.
	FCSGpuInstancedMeshSceneProxy::EnsureCullServiceStarted();
	// 「可见源行号」布局画的是引擎着色器的打补丁副本：第一次用到时核一遍它是不是照着这台机器的引擎生成的。
	if (GpuLayout.bVisibleSourceIndex && IsInGameThread()) FCSGpuInstancedIndexedVertexFactory::VerifyShaderSourceUpToDate();

	return new FCSGpuInstancedMeshSceneProxy(this, Resident, DrawMaterials);
}

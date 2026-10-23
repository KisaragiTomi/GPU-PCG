#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedMeshSceneProxy.h" // ECSGpuInstancedAuxSlot + the aux stream declaration
#include "CSGpuInstancedMeshVertexFactory.h" // depth-prepass admission contract, asserted below
#include "CSHouseActor.h"                    // FrameBricksSurviveBake 借门框砖当被测对象
#include "CSMesh.h"

// 下面这几个只有烘焙与 TexCoordSets 两条用例要（真 world / 真资产）。unity 构建下它们恰好被邻居 TU 带进来，
// 漏写只有 -SingleFile 才照得出来（坑表里那条）。
#include "Components/SceneComponent.h"
#include "EditorAssetLibrary.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MaterialDomain.h"                  // MD_Surface（5.1 起不再由 Material.h 带进来）
#include "Materials/Material.h"
#include "Tests/AutomationEditorCommon.h"
#include "UObject/Package.h"

namespace
{
	/** Smallest mesh that passes FCSGpuMeshCPUData::IsValid(): one triangle, per-vertex attributes. */
	FCSGpuMeshCPUData MakeUnitTriangle()
	{
		FCSGpuMeshCPUData Data;
		Data.Positions = { FVector3f(0.0f, 0.0f, 0.0f), FVector3f(100.0f, 0.0f, 0.0f), FVector3f(0.0f, 100.0f, 0.0f) };
		Data.Normals = { FVector3f::ZAxisVector, FVector3f::ZAxisVector, FVector3f::ZAxisVector };
		Data.Tangents = { FVector3f::XAxisVector, FVector3f::XAxisVector, FVector3f::XAxisVector };
		Data.TexCoordChannels[0] = { FVector2f(0.0f, 0.0f), FVector2f(1.0f, 0.0f), FVector2f(0.0f, 1.0f) };
		Data.Indices = { 0, 1, 2 };
		Data.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
		Data.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
		return Data;
	}

	/** 第 Set 组 UV 在第 V 个顶点上的值。组号与顶点号都编进去，交错布局一旦错位（按组分段 / 步长写错）读出来就不一样。 */
	FVector2f ExpectedTexCoord(int32 Set, int32 V)
	{
		return FVector2f(float(Set * 10 + V), float(Set * 10 + V) + 0.5f);
	}

	/** MakeUnitTriangle 带上 NumSets 组 UV（NumTexCoordChannels 如实写 NumSets，越界的组数留给调用方测钳位）。 */
	FCSGpuMeshCPUData MakeUnitTriangleWithTexCoordSets(int32 NumSets)
	{
		FCSGpuMeshCPUData Data = MakeUnitTriangle();
		Data.NumTexCoordChannels = NumSets;
		for (int32 Set = 0; Set < FMath::Min(NumSets, FCSGpuMeshCPUData::MaxTexCoordChannels); ++Set)
		{
			Data.TexCoordChannels[Set].SetNum(Data.Positions.Num());
			for (int32 V = 0; V < Data.Positions.Num(); ++V) Data.TexCoordChannels[Set][V] = ExpectedTexCoord(Set, V);
		}
		return Data;
	}

	UCSGpuInstancedMeshComponent* MakeComponent(int32 InstancesPerCluster)
	{
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->InstancesPerCluster = InstancesPerCluster;
		Component->SetBaseMeshFromGpuData(MakeUnitTriangle());
		return Component;
	}

	/** Rebuilds the instance-to-component matrix the way LocalVertexFactory.ush's
	 *  GetInstanceTransform() does, from the 5-float4 packed layout. */
	FMatrix44f UnpackInstanceTransform(const TArray<FVector4f>& Packed, int32 Slot)
	{
		const FVector4f R0 = Packed[Slot * 5 + 0];
		const FVector4f R1 = Packed[Slot * 5 + 1];
		const FVector4f R2 = Packed[Slot * 5 + 2];
		const FVector4f Origin = Packed[Slot * 5 + 3];
		return FMatrix44f(
			FPlane4f(R0.X, R0.Y, R0.Z, 0.0f),
			FPlane4f(R1.X, R1.Y, R1.Z, 0.0f),
			FPlane4f(R2.X, R2.Y, R2.Z, 0.0f),
			FPlane4f(Origin.X, Origin.Y, Origin.Z, 1.0f));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshPackingAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Packing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The packed instance buffer is consumed by a shader we do not control: LocalVertexFactory.ush
// reads three float4 rows plus an origin and rebuilds the instance matrix from them. Get the row
// order or the .w slots wrong and nothing fails to compile - the instances just render in the
// wrong place, or (Transform1.w feeding GetInstanceSelected) vanish entirely. This pins the layout.
bool FCSGpuInstancedMeshPackingAutomationTest::RunTest(const FString& Parameters)
{
	UCSGpuInstancedMeshComponent* Component = MakeComponent(/*InstancesPerCluster*/ 64);
	if (!TestNotNull(TEXT("Component"), Component)) return false;

	TArray<FTransform> Sources;
	Sources.Add(FTransform(FRotator(0.0f, 45.0f, 0.0f), FVector(500.0, -200.0, 30.0), FVector(2.0, 2.0, 2.0)));
	Sources.Add(FTransform(FRotator(10.0f, 0.0f, -25.0f), FVector(-1000.0, 800.0, -50.0), FVector(1.0, 0.5, 3.0)));
	Sources.Add(FTransform::Identity);
	Component->SetInstances(Sources);

	TestEqual(TEXT("Instance count"), Component->GetInstanceCount(), Sources.Num());

	const TArray<FVector4f>& Packed = Component->GetPackedInstances();
	if (!TestEqual(TEXT("Packed float4 count"), Packed.Num(), Sources.Num() * 5)) return false;

	// Instances are stored in Morton order, so match each source by its origin rather than assuming
	// the array order survived the sort.
	for (const FTransform& Source : Sources)
	{
		const FVector3f ExpectedOrigin = FVector3f(Source.GetLocation());

		int32 FoundSlot = INDEX_NONE;
		for (int32 Slot = 0; Slot < Sources.Num(); ++Slot)
		{
			const FVector4f Origin = Packed[Slot * 5 + 3];
			if (FVector3f(Origin.X, Origin.Y, Origin.Z).Equals(ExpectedOrigin, 0.01f))
			{
				FoundSlot = Slot;
				break;
			}
		}
		if (!TestTrue(FString::Printf(TEXT("Instance at %s is present"), *Source.GetLocation().ToString()), FoundSlot != INDEX_NONE)) continue;

		const FMatrix44f Expected = FMatrix44f(Source.ToMatrixWithScale());
		TestTrue(TEXT("Unpacked instance matrix matches the source transform"),
			UnpackInstanceTransform(Packed, FoundSlot).Equals(Expected, 0.01f));

		// Transform1.w is hitproxy + 256 * selected in the shader; anything non-zero would make
		// GetInstanceSelected() report a selection this component never has.
		TestEqual(TEXT("Transform row 0 .w is zero"), Packed[FoundSlot * 5 + 0].W, 0.0f);
		TestEqual(TEXT("Transform row 1 .w is zero"), Packed[FoundSlot * 5 + 1].W, 0.0f);
		TestEqual(TEXT("Transform row 2 .w is zero"), Packed[FoundSlot * 5 + 2].W, 0.0f);

		// Origin.w is the material's PerInstanceRandom.
		const float Random = Packed[FoundSlot * 5 + 3].W;
		TestTrue(TEXT("Per-instance random is in [0,1)"), Random >= 0.0f && Random < 1.0f);

		// The cull sphere has to contain the transformed base mesh, or instances pop at the edges.
		const FVector4f Sphere = Packed[FoundSlot * 5 + 4];
		TestTrue(TEXT("Cull sphere has a positive radius"), Sphere.W > 0.0f);
		for (const FVector3f& P : Component->GetBaseMeshSnapshot().Positions)
		{
			const FVector3f World = FVector3f(Source.TransformPosition(FVector(P)));
			TestTrue(TEXT("Cull sphere contains every transformed base vertex"),
				(World - FVector3f(Sphere.X, Sphere.Y, Sphere.Z)).Size() <= Sphere.W + 0.01f);
		}
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshClusterAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Clusters",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// The coarse cull level rejects a whole cluster in one test, so a cluster sphere that does not
// enclose its members would drop visible instances. The shader derives an instance's cluster as
// InstanceIndex / ClusterSize, which only holds while clusters are contiguous fixed-size runs -
// both invariants are checked here.
bool FCSGpuInstancedMeshClusterAutomationTest::RunTest(const FString& Parameters)
{
	constexpr int32 ClusterSize = 8;
	constexpr int32 NumInstances = 37; // deliberately not a multiple of ClusterSize

	UCSGpuInstancedMeshComponent* Component = MakeComponent(ClusterSize);
	if (!TestNotNull(TEXT("Component"), Component)) return false;

	TArray<FTransform> Sources;
	Sources.Reserve(NumInstances);
	for (int32 i = 0; i < NumInstances; ++i)
	{
		// Spread over a 3D lattice so the Morton sort actually has something to order.
		const FVector Location(double(i % 5) * 400.0, double((i / 5) % 5) * 400.0, double(i / 25) * 400.0);
		Sources.Add(FTransform(FRotator::ZeroRotator, Location, FVector(1.0 + 0.1 * i)));
	}
	Component->SetInstances(Sources);

	const TArray<FVector4f>& Packed = Component->GetPackedInstances();
	const TArray<FVector4f>& Clusters = Component->GetClusterBounds();

	const int32 ExpectedClusters = FMath::DivideAndRoundUp(NumInstances, ClusterSize);
	TestEqual(TEXT("Cluster count"), Clusters.Num(), ExpectedClusters);
	TestEqual(TEXT("Packed float4 count"), Packed.Num(), NumInstances * 5);

	for (int32 Instance = 0; Instance < NumInstances; ++Instance)
	{
		// Exactly the mapping InstanceCullCS uses.
		const int32 Cluster = FMath::Min(Instance / ClusterSize, Clusters.Num() - 1);

		const FVector4f Sphere = Packed[Instance * 5 + 4];
		const FVector4f ClusterSphere = Clusters[Cluster];
		const float CentreDistance = (FVector3f(Sphere.X, Sphere.Y, Sphere.Z) - FVector3f(ClusterSphere.X, ClusterSphere.Y, ClusterSphere.Z)).Size();

		TestTrue(FString::Printf(TEXT("Cluster %d encloses instance %d"), Cluster, Instance),
			CentreDistance + Sphere.W <= ClusterSphere.W + 0.01f);
	}

	// Clearing has to leave nothing behind for the proxy to allocate capacity from.
	Component->ClearInstances();
	TestEqual(TEXT("Cleared instance count"), Component->GetInstanceCount(), 0);
	TestEqual(TEXT("Cleared packed data"), Component->GetPackedInstances().Num(), 0);
	TestEqual(TEXT("Cleared clusters"), Component->GetClusterBounds().Num(), 0);

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshStreamsAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.Streams",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// This leaf's aux streams (one per ECSGpuInstancedAuxSlot) live in a UCSMesh alongside the standard set, addressed by
// (Role, TexCoordIndex). The standard set owns AuxVertex slot 0 for its per-triangle material ids
// and offers no way to turn that off, so a leaf slot numbered from 0 collides — and a collision is
// not loud: SetStreamLayoutSync refuses the layout and returns false, after which the vertex factory
// is handed null instance SRVs. Nothing in the rendered result says which stream went missing.
//
// Declaring a layout on a mesh that has never been allocated is pure bookkeeping (no RHI, no render
// commands), so the whole contract can be pinned without a GPU.
bool FCSGpuInstancedMeshStreamsAutomationTest::RunTest(const FString& Parameters)
{
	// ---- 深度预通道的入场券（2026-08-30 的缺陷，症状与本测试的"静默失效"同一类）----
	//
	// 引擎选深度预通道着色器用的是**两套口径**：
	//   · `FDepthPassMeshProcessor::ShouldRender` 看**实例**的 SupportsPositionOnlyStream()；
	//   · `TDepthOnlyVS<true>::ShouldCompilePermutation` 看**类型**的 SupportsPositionOnly()。
	// 两者不一致时 GetDepthPassShaders 取不到着色器，整个 batch 被**静默地**踢出深度预通道：
	// 实例照常写颜色但不占深度，于是同一趟基通道里后画的任何不透明物都能把它整片盖掉
	// （实测：L_TerrainOpsDemo 的 216 级石阶只剩地形轮廓线外侧那十几块）。
	// 一行日志都不会有，所以这条契约必须由断言守着。
	{
		FCSGpuInstancedMeshVertexFactory VertexFactory(ERHIFeatureLevel::SM5, "StreamsTest");
		TestFalse(TEXT("The instanced factory type does not declare SupportsPositionOnly"),
			VertexFactory.GetType()->SupportsPositionOnly());
		// 所以实例也一个字都不能提 position-only —— 否则深度预通道就会去要一个没编译过的着色器。
		TestFalse(TEXT("...so the factory must not advertise a position-only stream either"),
			VertexFactory.SupportsPositionOnlyStream());
		TestFalse(TEXT("...nor a position-and-normal-only stream (same two-sided contract, shadow depth)"),
			VertexFactory.SupportsPositionAndNormalOnlyStream());

		// 「可见源行号」版的工厂是另一个注册类型（另一份着色器文件），同一条契约要各守各的：
		// 它继承上面那个只为复用 InitRHI，注册宏里的 flag 是照抄的 —— 抄漏一个，症状与上面一模一样。
		FCSGpuInstancedIndexedVertexFactory IndexedVertexFactory(ERHIFeatureLevel::SM5, "StreamsTestIndexed");
		TestTrue(TEXT("The indexed factory is registered as its own type"), IndexedVertexFactory.GetType() != VertexFactory.GetType());
		TestFalse(TEXT("The indexed factory type does not declare SupportsPositionOnly either"),
			IndexedVertexFactory.GetType()->SupportsPositionOnly());
		TestFalse(TEXT("...and advertises no position-only stream"), IndexedVertexFactory.SupportsPositionOnlyStream());
		TestFalse(TEXT("...nor a position-and-normal-only stream"), IndexedVertexFactory.SupportsPositionAndNormalOnlyStream());
		// 手取实例要求类型不带 primitive-id 流（带了就改走 GPU-Scene，见工厂的 InitRHI）。
		TestFalse(TEXT("Neither factory type supports the primitive-id stream"),
			VertexFactory.GetType()->SupportsPrimitiveIdStream() || IndexedVertexFactory.GetType()->SupportsPrimitiveIdStream());
	}

	// 两种可见缓冲布局（FCSGpuInstancedGpuLayout::bVisibleSourceIndex）各声明各的流，下面整段两种都过一遍。
	for (const bool bVisibleSourceIndex : { false, true })
	{
	FCSGpuInstancedGpuLayout Layout;
	Layout.NumLODs = 3;
	Layout.InstanceCapacity = 128;
	Layout.NumSourceInstances = 100;
	Layout.ClusterSize = 8;
	Layout.NumClusters = 13;
	Layout.bVisibleSourceIndex = bVisibleSourceIndex;

	// 本叶子声明的全部 aux 槽位，与 ECSGpuInstancedAuxSlot 一一对应。数量断言从这张表取、不另写魔数；
	// 加新流时在这里补一行，下面的"能解析到 / 避开 0 号槽"就自动覆盖它。
	const TArray<ECSGpuInstancedAuxSlot> Slots = bVisibleSourceIndex
		? TArray<ECSGpuInstancedAuxSlot>{
			ECSGpuInstancedAuxSlot::SourceInstances,
			ECSGpuInstancedAuxSlot::ClusterBounds,
			ECSGpuInstancedAuxSlot::ClusterVisible,
			ECSGpuInstancedAuxSlot::VisibleSourceIndices,
			ECSGpuInstancedAuxSlot::CustomDataFallback,
			ECSGpuInstancedAuxSlot::LodCounters,
		}
		: TArray<ECSGpuInstancedAuxSlot>{
			ECSGpuInstancedAuxSlot::SourceInstances,
			ECSGpuInstancedAuxSlot::ClusterBounds,
			ECSGpuInstancedAuxSlot::ClusterVisible,
			ECSGpuInstancedAuxSlot::VisibleTransforms,
			ECSGpuInstancedAuxSlot::VisibleOrigins,
			ECSGpuInstancedAuxSlot::VisibleLightmap,
			ECSGpuInstancedAuxSlot::LodCounters,
			ECSGpuInstancedAuxSlot::VisibleCustomData,
		};
	const int32 NumLeafSlots = Slots.Num();
	// 另一种布局的可见缓冲：这种布局下一条都不许声明（声明了就是白占显存，新布局省的就是这几条）。
	const TArray<ECSGpuInstancedAuxSlot> ForeignSlots = bVisibleSourceIndex
		? TArray<ECSGpuInstancedAuxSlot>{ ECSGpuInstancedAuxSlot::VisibleTransforms, ECSGpuInstancedAuxSlot::VisibleOrigins, ECSGpuInstancedAuxSlot::VisibleLightmap, ECSGpuInstancedAuxSlot::VisibleCustomData }
		: TArray<ECSGpuInstancedAuxSlot>{ ECSGpuInstancedAuxSlot::VisibleSourceIndices, ECSGpuInstancedAuxSlot::CustomDataFallback };

	TArray<FCSGpuStreamDesc> AuxStreams;
	CSGpuInstancedBuildAuxStreamDescs(AuxStreams, Layout, /*bExternalPackedSource*/ false);
	// 数不对**不许 return**：下面的检查全按槽位查（FindStream），多一条少一条都照样跑得动。
	// 09-09 加 VisibleCustomData 时这里还是 `if (!TestEqual(..., 7)) return false;`，于是这一条红了之后
	// 碰撞 / 尺寸 / 零字节检查被整段跳过了两天 —— 一个过期的数字把整条守卫静默关掉。
	TestEqual(TEXT("Aux stream count matches the slot table (a new stream must be added to Slots above)"), AuxStreams.Num(), NumLeafSlots);

	UCSMesh* Mesh = NewObject<UCSMesh>(GetTransientPackage());
	if (!TestNotNull(TEXT("Mesh"), Mesh)) return false;

	const FCSMeshResident* Resident = Mesh->GetResidentPtr();
	if (!TestNotNull(TEXT("Resident set"), Resident)) return false;
	// Counted rather than hard-coded: the point is that every leaf slot survives on top of whatever
	// the standard set happens to be, not that the standard set has a particular size today.
	const int32 NumStandardStreams = Resident->Streams.Num();

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = Layout.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, Layout, /*bExternalPackedSource*/ false);
	if (!TestTrue(TEXT("The mesh accepts this leaf's stream layout"), Mesh->SetStreamLayoutSync(StreamLayout))) return false;

	TestEqual(TEXT("Every aux stream was appended to the standard set"), Resident->Streams.Num(), NumStandardStreams + NumLeafSlots);
	TestEqual(TEXT("One indirect arg set per LOD"), Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));

	// The material-id stream is what the leaf slots must not land on, so its survival is half the
	// assertion — a layout that displaced it would leave the operator layer without it.
	TestNotNull(TEXT("The standard material-id stream still owns AuxVertex slot 0"),
		Resident->FindStream(ECSGpuStreamRole::AuxVertex, 0));

	for (ECSGpuInstancedAuxSlot Slot : Slots)
	{
		TestTrue(FString::Printf(TEXT("Aux slot %u is clear of the standard set"), uint32(Slot)), uint8(Slot) != 0);
		TestNotNull(FString::Printf(TEXT("Aux slot %u resolves in the resident set"), uint32(Slot)),
			Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot)));
	}

	// Each LOD draws out of its own fixed-size region and the vertex factory offsets into it by
	// Lod * InstanceCapacity, so a region short of the capacity would have the cull compacting
	// survivors into the next LOD's slots.
	const int32 VisibleSlots = int32(Layout.InstanceCapacity * Layout.NumLODs);
	auto AuxElementCount = [Resident](ECSGpuInstancedAuxSlot Slot)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot));
		return Stream ? int32(Stream->Desc.ElementsPerUnit) : 0;
	};
	for (ECSGpuInstancedAuxSlot Slot : ForeignSlots)
	{
		TestNull(FString::Printf(TEXT("Aux slot %u belongs to the other visible-buffer layout and is not declared in this one"), uint32(Slot)),
			Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot)));
	}
	if (bVisibleSourceIndex)
	{
		// 每个可见槽一个 uint 的源行号，分区与旧布局相同（每级 LOD 一整份容量）。着色器按 Buffer<uint> 取。
		TestEqual(TEXT("Visible source indices cover every LOD region"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleSourceIndices), VisibleSlots);
		if (const FCSMeshResident::FStream* Indices = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleSourceIndices)))
		{
			TestEqual(TEXT("Visible source indices are viewed as Buffer<uint> (PF_R32_UINT)"), int32(Indices->Desc.SrvFormat), int32(PF_R32_UINT));
			TestEqual(TEXT("Visible source indices are four bytes per slot"), int32(Indices->Desc.BytesPerElement), int32(sizeof(uint32)));
		}
		// 生产者没给 custom data 时材质读的那一行零：恰好一行（下标被钉成 0），大小与实例数无关；
		// 格式与真的 custom data 相同（引擎按 Buffer<float> 取）。
		TestEqual(TEXT("The custom-data fallback is exactly one row of floats"),
			AuxElementCount(ECSGpuInstancedAuxSlot::CustomDataFallback), int32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS));
		if (const FCSMeshResident::FStream* Fallback = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::CustomDataFallback)))
		{
			TestEqual(TEXT("The custom-data fallback is viewed as Buffer<float> (PF_R32_FLOAT)"), int32(Fallback->Desc.SrvFormat), int32(PF_R32_FLOAT));
		}
	}
	else
	{
	TestEqual(TEXT("Visible origins cover every LOD region"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleOrigins), VisibleSlots);
	TestEqual(TEXT("Visible lightmap covers every LOD region"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleLightmap), VisibleSlots);
	TestEqual(TEXT("Visible transforms are three float4 per slot"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleTransforms), VisibleSlots * 3);
	// 逐实例 custom data 与另外三条可见缓冲同一套分区（每 LOD 一段），每槽 CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS 个 float。
	// 格式必须是 PF_R32_FLOAT：引擎按 Buffer<float> 取（InstanceCustomDataBuffer），照抄邻居那几条的 float4
	// 视图读出来是错位数据 —— 画面上只是"藤叶的数值不对"，不报错。
	TestEqual(TEXT("Visible custom data covers every LOD region"),
		AuxElementCount(ECSGpuInstancedAuxSlot::VisibleCustomData), VisibleSlots * int32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS));
	if (const FCSMeshResident::FStream* CustomData = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(ECSGpuInstancedAuxSlot::VisibleCustomData)))
	{
		TestEqual(TEXT("Visible custom data is viewed as Buffer<float> (PF_R32_FLOAT)"), int32(CustomData->Desc.SrvFormat), int32(PF_R32_FLOAT));
		TestEqual(TEXT("Visible custom data is one float per element"), int32(CustomData->Desc.BytesPerElement), int32(sizeof(float)));
	}
	}
	// The source rows are CS_GPU_INSTANCED_ROW_FLOAT4S float4 per instance, over the capacity rather
	// than the live count — the buffer ratchets, so the cull's MaxSourceInstances is the capacity.
	TestEqual(TEXT("Source rows cover the instance capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::SourceInstances),
		int32(Layout.InstanceCapacity) * int32(CS_GPU_INSTANCED_ROW_FLOAT4S));
	// Cluster spheres are sized from the capacity too, but only NumClusters of them are ever filled.
	TestTrue(TEXT("Cluster spheres cover the live cluster count"),
		AuxElementCount(ECSGpuInstancedAuxSlot::ClusterBounds) >= int32(Layout.NumClusters));

	// A packed GPU source brings its own row buffer, so the mesh must not size one for it as well —
	// at large instance counts that is tens of megabytes nothing ever reads.
	TArray<FCSGpuStreamDesc> ExternalSourceStreams;
	CSGpuInstancedBuildAuxStreamDescs(ExternalSourceStreams, Layout, /*bExternalPackedSource*/ true);
	for (const FCSGpuStreamDesc& Desc : ExternalSourceStreams)
	{
		if (Desc.TexCoordIndex != uint8(ECSGpuInstancedAuxSlot::SourceInstances)) continue;
		TestEqual(TEXT("An external packed source leaves only a placeholder behind"), int32(Desc.ElementsPerUnit), 1);
	}

	// Nothing may declare zero bytes: the mesh refuses such a stream, which takes the whole layout
	// with it and leaves the component unallocated for a reason no log connects to the empty slot.
	for (const FCSGpuStreamDesc& Desc : AuxStreams)
	{
		TestTrue(TEXT("Every aux stream allocates something"), Desc.BytesPerElement > 0 && Desc.ElementsPerUnit > 0);
	}
	} // for bVisibleSourceIndex

	// 新布局存在的理由：同样的容量与 LOD 数，可见缓冲从每槽 88 字节降到 4 字节。把这个数钉住 ——
	// 以后谁往新布局里加一条逐可见槽的流，这里会要求他先看一眼这笔账。
	{
		auto VisibleBytes = [](bool bVisibleSourceIndex)
		{
			FCSGpuInstancedGpuLayout Layout;
			Layout.NumLODs = 3;
			Layout.InstanceCapacity = 128;
			Layout.NumSourceInstances = 100;
			Layout.ClusterSize = 8;
			Layout.NumClusters = 13;
			Layout.bVisibleSourceIndex = bVisibleSourceIndex;
			TArray<FCSGpuStreamDesc> Streams;
			CSGpuInstancedBuildAuxStreamDescs(Streams, Layout, /*bExternalPackedSource*/ false);
			uint64 Bytes = 0;
			for (const FCSGpuStreamDesc& Desc : Streams)
			{
				const uint8 Slot = Desc.TexCoordIndex;
				const bool bPerVisibleSlot = Slot == uint8(ECSGpuInstancedAuxSlot::VisibleTransforms) || Slot == uint8(ECSGpuInstancedAuxSlot::VisibleOrigins)
					|| Slot == uint8(ECSGpuInstancedAuxSlot::VisibleLightmap) || Slot == uint8(ECSGpuInstancedAuxSlot::VisibleCustomData)
					|| Slot == uint8(ECSGpuInstancedAuxSlot::VisibleSourceIndices);
				if (bPerVisibleSlot) Bytes += uint64(Desc.BytesPerElement) * Desc.ElementsPerUnit;
			}
			return Bytes;
		};
		const uint64 VisibleSlotCount = 128ull * 3ull;
		// 旧布局每槽：3 个 float4 的变换行 + 1 个原点 + 1 个 lightmap + custom data。
		const uint64 LegacyBytesPerSlot = uint64(3 + 1 + 1) * sizeof(FVector4f) + uint64(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS) * sizeof(float);
		TestEqual(TEXT("Legacy layout: 5 float4 + custom data per visible slot"), VisibleBytes(false), VisibleSlotCount * LegacyBytesPerSlot);
		TestEqual(TEXT("Visible-source-index layout: 4 bytes per visible slot"), VisibleBytes(true), VisibleSlotCount * uint64(sizeof(uint32)));
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshStreamResizeAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.StreamResize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// Another instance changes every one of this leaf's aux streams except LodCounters, and nothing else. Saying so used to mean
// re-declaring the whole layout, which reallocates and copies every resident stream — the base mesh
// included — so an AddInstance copied the entire mesh to make one buffer bigger.
// UCSMesh::ResizeStreamsSync is what the component uses instead, and this pins the bookkeeping half
// of it: that a capacity change lands exactly where CSGpuInstancedBuildAuxStreamDescs says it should,
// that the standard set and the draw layout do not move with it, and that the resident set ends up
// byte-identical to what a re-declaration at that capacity would have asked for — which is precisely
// the condition under which the component's next rebuild costs nothing.
//
// Declaring and resizing on a mesh that has never been allocated is pure bookkeeping (no RHI, no
// render commands), the same reason the Streams suite above needs no GPU.
bool FCSGpuInstancedMeshStreamResizeAutomationTest::RunTest(const FString& Parameters)
{
	// 两种可见缓冲布局各有各的逐可见槽流（旧：变换 / 原点 / lightmap / custom data；新：源行号），整段两种都过一遍。
	for (const bool bVisibleSourceIndex : { false, true })
	{
	// 这种布局下"跟着容量走的那条可见缓冲"的代表，以及它每槽几个元素。
	const ECSGpuInstancedAuxSlot VisibleSlot = bVisibleSourceIndex ? ECSGpuInstancedAuxSlot::VisibleSourceIndices : ECSGpuInstancedAuxSlot::VisibleOrigins;

	FCSGpuInstancedGpuLayout Layout;
	Layout.NumLODs = 3;
	Layout.InstanceCapacity = 128;
	Layout.NumSourceInstances = 100;
	Layout.ClusterSize = 8;
	Layout.NumClusters = 13;
	Layout.bVisibleSourceIndex = bVisibleSourceIndex;

	UCSMesh* Mesh = NewObject<UCSMesh>(GetTransientPackage());
	if (!TestNotNull(TEXT("Mesh"), Mesh)) return false;

	FCSMeshStreamLayout StreamLayout;
	StreamLayout.NumIndirectDraws = Layout.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(StreamLayout.ExtraStreams, Layout, /*bExternalPackedSource*/ false);
	if (!TestTrue(TEXT("The mesh accepts this leaf's stream layout"), Mesh->SetStreamLayoutSync(StreamLayout))) return false;

	const FCSMeshResident* Resident = Mesh->GetResidentPtr();
	if (!TestNotNull(TEXT("Resident set"), Resident)) return false;

	// Whatever the standard set happens to be today, verbatim: the resize must not touch a single
	// element of it, and the aux slots start at 16 precisely so it cannot.
	TArray<uint32> ElementsBefore;
	for (const FCSMeshResident::FStream& Stream : Resident->Streams) ElementsBefore.Add(Stream.Desc.ElementsPerUnit);
	const int32 StreamCountBefore = Resident->Streams.Num();

	// The instance set grew past its capacity; everything else about the component is unchanged.
	FCSGpuInstancedGpuLayout Grown = Layout;
	Grown.InstanceCapacity = 512;
	Grown.NumSourceInstances = 400;
	Grown.NumClusters = 50;

	TArray<FCSGpuStreamDesc> WantedAux;
	CSGpuInstancedBuildAuxStreamDescs(WantedAux, Grown, /*bExternalPackedSource*/ false);

	TArray<FCSMeshStreamResize> Resizes;
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		FCSMeshStreamResize& Resize = Resizes.AddDefaulted_GetRef();
		Resize.Role = Desc.Role;
		Resize.SlotIndex = Desc.TexCoordIndex;
		Resize.ElementCount = Desc.ElementsPerUnit;
	}
	if (!TestTrue(TEXT("A capacity change goes through as a per-stream resize"), Mesh->ResizeStreamsSync(Resizes))) return false;

	TestEqual(TEXT("A resize adds and removes no streams"), Resident->Streams.Num(), StreamCountBefore);
	for (const FCSGpuStreamDesc& Desc : WantedAux)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(Desc.Role, Desc.TexCoordIndex);
		if (!TestNotNull(*FString::Printf(TEXT("Aux slot %u survives the resize"), uint32(Desc.TexCoordIndex)), Stream)) continue;
		// Byte-identical to a fresh declaration at the grown capacity — which is what makes the
		// component's next SetStreamLayoutSync a no-op instead of a whole-mesh reallocation.
		TestEqual(*FString::Printf(TEXT("Aux slot %u is sized for the grown capacity"), uint32(Desc.TexCoordIndex)),
			int32(Stream->Desc.ElementsPerUnit), int32(Desc.ElementsPerUnit));
	}

	// The visible-instance regions are what the vertex factory strides through by Lod * capacity,
	// so a region short of the new capacity would have the cull compacting into the next LOD's slots.
	const int32 GrownVisibleSlots = int32(Grown.InstanceCapacity * Grown.NumLODs);
	auto AuxElementCount = [Resident](ECSGpuInstancedAuxSlot Slot)
	{
		const FCSMeshResident::FStream* Stream = Resident->FindStream(ECSGpuStreamRole::AuxVertex, uint8(Slot));
		return Stream ? int32(Stream->Desc.ElementsPerUnit) : 0;
	};
	TestEqual(TEXT("The per-visible-slot stream grew with the capacity"), AuxElementCount(VisibleSlot), GrownVisibleSlots);
	if (bVisibleSourceIndex)
	{
		// 那一行零与实例数无关，容量怎么变它都是一行。
		TestEqual(TEXT("The custom-data fallback does not follow the instance capacity"),
			AuxElementCount(ECSGpuInstancedAuxSlot::CustomDataFallback), int32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS));
	}
	else
	{
		TestEqual(TEXT("Visible transforms grew with the capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::VisibleTransforms), GrownVisibleSlots * 3);
	}
	TestEqual(TEXT("Source rows grew with the capacity"), AuxElementCount(ECSGpuInstancedAuxSlot::SourceInstances),
		int32(Grown.InstanceCapacity) * int32(CS_GPU_INSTANCED_ROW_FLOAT4S));
	// Fixed at CS_GPU_INSTANCED_MAX_LODS on purpose, so it never appears in a resize at all.
	TestEqual(TEXT("The LOD counters do not follow the instance capacity"),
		AuxElementCount(ECSGpuInstancedAuxSlot::LodCounters), CS_GPU_INSTANCED_MAX_LODS);

	// The half that used to be collateral damage.
	const uint8 AuxSlotFloor = uint8(ECSGpuInstancedAuxSlot::SourceInstances);
	bool bStandardSetIntact = Resident->Streams.Num() == ElementsBefore.Num();
	for (int32 Index = 0; bStandardSetIntact && Index < Resident->Streams.Num(); ++Index)
	{
		const FCSGpuStreamDesc& Desc = Resident->Streams[Index].Desc;
		if (Desc.Role == ECSGpuStreamRole::AuxVertex && Desc.TexCoordIndex >= AuxSlotFloor) continue;
		bStandardSetIntact = Desc.ElementsPerUnit == ElementsBefore[Index];
	}
	TestTrue(TEXT("An instance-capacity change leaves the standard streams untouched"), bStandardSetIntact);
	TestNotNull(TEXT("The material-id stream still owns AuxVertex slot 0"),
		Resident->FindStream(ECSGpuStreamRole::AuxVertex, 0));
	TestEqual(TEXT("An instance-capacity change leaves the draw layout alone"),
		Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));

	// Re-declaring at the capacity the resize arrived at is the path RebuildGpuMesh takes on every
	// rebuild after the first, so it has to be accepted and leave the set alone. That it also costs
	// no reallocation only shows on an allocated mesh, which is what GpuMeshObject.StreamResize pins.
	FCSMeshStreamLayout GrownLayout;
	GrownLayout.NumIndirectDraws = Grown.NumLODs;
	CSGpuInstancedBuildAuxStreamDescs(GrownLayout.ExtraStreams, Grown, /*bExternalPackedSource*/ false);
	TestTrue(TEXT("Declaring the layout the resize arrived at is accepted"), Mesh->SetStreamLayoutSync(GrownLayout));
	TestEqual(TEXT("...and changes nothing about the stream set"), Resident->Streams.Num(), StreamCountBefore);
	TestEqual(TEXT("...nor about the sizes the resize established"),
		AuxElementCount(VisibleSlot), GrownVisibleSlots);

	// Shrinking back is the same call: hysteresis is ResolveInstanceCapacity's job, not the mesh's.
	TArray<FCSMeshStreamResize> Shrink;
	FCSMeshStreamResize& ShrinkOrigins = Shrink.AddDefaulted_GetRef();
	ShrinkOrigins.Role = ECSGpuStreamRole::AuxVertex;
	ShrinkOrigins.SlotIndex = uint8(VisibleSlot);
	ShrinkOrigins.ElementCount = uint32(Layout.InstanceCapacity * Layout.NumLODs);
	if (TestTrue(TEXT("Shrinking a stream back uses the same entry point"), Mesh->ResizeStreamsSync(Shrink)))
	{
		TestEqual(TEXT("A shrink is exact, not rounded up"),
			AuxElementCount(VisibleSlot), int32(Layout.InstanceCapacity * Layout.NumLODs));
	}

	// The standard stream this leaf's slot numbering exists to protect. It is per-triangle, so its
	// size follows the mesh capacity and a resize of it would corrupt the readback's per-face stride.
	TestFalse(TEXT("The standard material-id stream cannot be resized this way"),
		Mesh->ResizeStreamSync(ECSGpuStreamRole::AuxVertex, 0, 32));
	TestFalse(TEXT("Neither can the per-LOD indirect args"),
		Mesh->ResizeStreamSync(ECSGpuStreamRole::IndirectArgs, 0, 5 * 4));
	TestEqual(TEXT("A refused resize leaves the draw layout alone"), Mesh->GetIndirectDrawCount(), int32(Layout.NumLODs));
	} // for bVisibleSourceIndex
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshTexCoordSetsAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.TexCoordSets",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/**
 * 实例路的基础网格要把**全部** UV 组带上 GPU（2026-09-14 起，上限 MaxTexCoordChannels = 8）。
 * 起因是 TG 叶卡：树冠材质读 UV1.xy / UV2.x 当卡片中心，而这条路以前只传 UV0。
 *
 * 失效是静默的：常驻流只声明一组时，引擎 manual fetch 把 TexCoord[1..] 钳回最后一组
 * （LocalVertexFactory.ush:733 的 ClampedCoordinateIndex），材质读到的 UV1 / UV2 全是 UV0 ——
 * 所有卡片塌向同一个"中心"，画面错、不报错、CPU 侧断言照绿。所以这里一路守到 GPU 回读：
 *   ① 快照按交错布局存 N 组（第 V 个顶点的第 S 组在 [V×N+S]），一组的网格布局与改动前逐位相同；
 *   ② 组数钳在 MaxTexCoordChannels，钳过的快照依然合法；
 *   ③ 注册后常驻 TexCoord 流真的加宽到 2N，回读逐组逐顶点对得上；换回一组的网格，流收窄回 2。
 */
bool FCSGpuInstancedMeshTexCoordSetsAutomationTest::RunTest(const FString& Parameters)
{
	// --- ① 快照：交错布局
	{
		UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Component->SetBaseMeshFromGpuData(MakeUnitTriangleWithTexCoordSets(3));
		const FCSGpuInstancedBaseMesh& Snapshot = Component->GetBaseMeshSnapshot();
		TestTrue(TEXT("三组 UV 的快照合法"), Snapshot.IsValid());
		TestEqual(TEXT("快照记下三组"), Snapshot.NumTexCoordSets, 3);
		const bool bSized = TestEqual(TEXT("TexCoords 长度 = 顶点数 × 组数"), Snapshot.TexCoords.Num(), Snapshot.Positions.Num() * 3);
		for (int32 Index = 0; bSized && Index < Snapshot.TexCoords.Num(); ++Index) TestTrue(*FString::Printf(TEXT("顶点 %d 的第 %d 组落在 [V×3+S]（实际 %s）"), Index / 3, Index % 3, *Snapshot.TexCoords[Index].ToString()), Snapshot.TexCoords[Index].Equals(ExpectedTexCoord(Index % 3, Index / 3), 1e-4f));

		// 一组的网格：布局必须与改动前逐位相同 —— 草 / 花 / 石阶都是这一类，不许为多组 UV 付一个字节。
		UCSGpuInstancedMeshComponent* Single = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Single->SetBaseMeshFromGpuData(MakeUnitTriangle());
		TestEqual(TEXT("一组 UV 的网格仍记一组"), Single->GetBaseMeshSnapshot().NumTexCoordSets, 1);
		TestEqual(TEXT("...TexCoords 仍是每顶点一条"), Single->GetBaseMeshSnapshot().TexCoords.Num(), Single->GetBaseMeshSnapshot().Positions.Num());
	}

	// --- ② 钳位
	{
		FCSGpuMeshCPUData TooMany = MakeUnitTriangleWithTexCoordSets(FCSGpuMeshCPUData::MaxTexCoordChannels);
		TooMany.NumTexCoordChannels = FCSGpuMeshCPUData::MaxTexCoordChannels + 4;
		UCSGpuInstancedMeshComponent* Clamped = NewObject<UCSGpuInstancedMeshComponent>(GetTransientPackage());
		Clamped->SetBaseMeshFromGpuData(TooMany);
		TestEqual(TEXT("声明的组数超过上限时钳到 MaxTexCoordChannels"), Clamped->GetBaseMeshSnapshot().NumTexCoordSets, FCSGpuMeshCPUData::MaxTexCoordChannels);
		TestTrue(TEXT("...钳过之后快照依然合法"), Clamped->GetBaseMeshSnapshot().IsValid());
		const int32 LastSet = FCSGpuMeshCPUData::MaxTexCoordChannels - 1;
		TestTrue(TEXT("...最后一组（UV7）照样带上"), Clamped->GetBaseMeshSnapshot().TexCoords[LastSet].Equals(ExpectedTexCoord(LastSet, 0), 1e-4f));
	}

	// --- ③ 真分配 + GPU 回读
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();

	UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Instanced->SetupAttachment(Root);
	Instanced->RegisterComponent();
	// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
	Instanced->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Instanced->SetBaseMeshFromGpuData(MakeUnitTriangleWithTexCoordSets(3));
	// 有实例才分配常驻网格；注册过的组件在这一步同步传完。
	Instanced->AddInstance(FTransform::Identity);

	auto TexCoordStreamWidth = [Instanced]()
	{
		const UCSMesh* Mesh = Instanced->GetGpuMesh();
		const FCSMeshResident* Resident = Mesh ? Mesh->GetResidentPtr() : nullptr;
		const FCSMeshResident::FStream* Stream = Resident ? Resident->FindStream(ECSGpuStreamRole::TexCoord, 0) : nullptr;
		return Stream ? int32(Stream->Desc.ElementsPerUnit) : 0;
	};
	if (!TestEqual(TEXT("常驻 TexCoord 流加宽到每顶点 2 × 3 个 float"), TexCoordStreamWidth(), 6)) return false;

	FCSGpuMeshCPUData Readback;
	if (!TestTrue(TEXT("基础网格能从 GPU 回读"), Instanced->GetGpuMesh()->ReadbackMeshSync(Readback))) return false;
	if (!TestEqual(TEXT("回读出三组 UV"), Readback.NumTexCoordChannels, 3)) return false;
	for (int32 Set = 0; Set < 3; ++Set)
	{
		if (!TestEqual(*FString::Printf(TEXT("第 %d 组回读出每个顶点一条"), Set), Readback.TexCoordChannels[Set].Num(), 3)) continue;
		for (int32 V = 0; V < 3; ++V) TestTrue(*FString::Printf(TEXT("GPU 上第 %d 组第 %d 个顶点对得上（实际 %s）"), Set, V, *Readback.TexCoordChannels[Set][V].ToString()), Readback.TexCoordChannels[Set][V].Equals(ExpectedTexCoord(Set, V), 1e-4f));
	}

	// 换回一组 UV 的网格：组数变了就是布局变了，流要收窄回 2，而不是留着上一张网格的宽度。
	Instanced->SetBaseMeshFromGpuData(MakeUnitTriangle());
	TestEqual(TEXT("换回一组 UV 的网格后常驻流收窄回 2"), TexCoordStreamWidth(), 2);

	Instanced->DestroyComponent();
	return true;
}

// -----------------------------------------------------------------------------
// 烘焙出口（裁决六 ①②）：门框砖走真正的 `SaveToStaticMesh`，通道与多组 UV 都要活下来
// -----------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshBakeAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.FrameBricksSurviveBake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 抄的是 `RockShell.CapSkirtSurvivesBake` 的形状：**真烘一遍**，再从烘出来的资产里读回来。
 *
 * 这条守的是三件事，每一件的失效方式都是"不报错、画面错"：
 *   ① 裁决六 ① —— 实例路以前**一个出口都没有**，整族（门框砖/接缝砖/藤/摆件）一件都存不出来；
 *   ② 裁决六 ② —— 顶点色通道与多组 UV 必须随网格一起保住；
 *   ③ 裁决六 ③ —— 烘完就没有 `PerInstanceRandom` 了（非实例图元的 RandomID 恒 0），
 *      材质里那条 `lerp(0.88, 1.12, rnd)` 会把整片砖/石/藤烘成**同一个色**。
 *      判据只能是"烘焙件里那条通道还有多少种取值"，三角数 / 包围盒 / 实例数**全都看不见它**。
 *
 * 用门框砖而不是石阶：它的前置最少（一栋房 + 一张砖网格 + 一份显式窗户列表，不需要地形与落笔），
 * 而判据最完整（砖数 CPU 侧有账、GPU 侧有计数器、烘焙件有三角数，三者能互相对）。
 */
bool FCSGpuInstancedMeshBakeAutomationTest::RunTest(const FString& Parameters)
{
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	// 引擎的 Cube 顶了 TG 的 brick：两者都是居中单位立方体（尺寸全靠逐实例非均匀缩放），
	// 而这条测的是出口不是资产 —— 用引擎自带件才不会因为 Content/ 没跟进 git 就跑不起来。
	UStaticMesh* BrickMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Frame brick base mesh"), BrickMesh)) return false;

	ACSHouseActor* House = World->SpawnActor<ACSHouseActor>(FVector::ZeroVector, FRotator::ZeroRotator);
	if (!TestNotNull(TEXT("House actor"), House)) return false;

	House->FrameBrickMesh = BrickMesh;
	House->bFrameEnabled = true;
	House->bWindowsEnabled = true;
	// 显式窗户列表 —— 这里**没有也不许有**任何触发规则（唯一没拍板的那条是门洞触发规则，
	// 本测试一行都不碰它）。三扇开在短墙上，理由同回归脚本 demo_house_window。
	auto MakeWindow = [](int32 Edge, float CenterS)
	{
		FCSHouseWindow Window;
		Window.EdgeIndex = Edge;
		Window.CenterS = CenterS;
		Window.Width = 78.0f;
		Window.SillZ = 90.0f;
		Window.Height = 110.0f;
		return Window;
	};
	House->Windows = { MakeWindow(1, 120.0f), MakeWindow(1, 232.0f), MakeWindow(3, 176.0f) };
	House->RebuildHouse();

	const int32 Bricks = House->GetFrameBrickCount();
	AddInfo(FString::Printf(TEXT("窗 %d 洞 %d 砖 %d"), House->GetWindowCount(), House->GetOpeningCount(), Bricks));
	if (!TestTrue(FString::Printf(TEXT("这栋房子砌出了门框砖（%d 块）"), Bricks), Bricks > 0)) return false;

	// GPU 上真的在画的那一份必须与 CPU 的账对得上，否则下面烘出来的是"另一批砖"而没人知道。
	const int32 GpuBricks = House->DebugReadFrameBrickCountGpuSync();
	if (!TestEqual(TEXT("GPU 计数器与 CPU 的砖账一致"), GpuBricks, Bricks)) return false;

	const FString BakePath = FString::Printf(
		TEXT("/Game/Automation/GpuInstanced/SM_FrameBricks_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

	int32 Triangles = 0, VertexInstances = 0, UVChannels = 0, DistinctRandoms = 0, GpuInstances = 0;
	bool bRandomsMatchGpu = false;
	const bool bBaked = House->DebugBakeFrameBricksSync(
		BakePath, Triangles, VertexInstances, UVChannels, DistinctRandoms, GpuInstances, bRandomsMatchGpu);
	AddInfo(FString::Printf(TEXT("烘成 StaticMesh：%d 三角 / %d 角点 / %d 组 UV；逐实例随机 %d 种（%d 个实例）"),
		Triangles, VertexInstances, UVChannels, DistinctRandoms, GpuInstances));
	if (!TestTrue(TEXT("实例路有一条走得通的 SaveToStaticMesh 出口（裁决六 ①）"), bBaked)) return false;

	// --- 几何：烘出来的必须是**整族**砖，不是一块 ---
	TestEqual(TEXT("烘出来的实例数 = GPU 上的实例数"), GpuInstances, Bricks);
	// 逐实例展开之后三角数必然是基础网格 LOD0 三角数的整数倍，且倍数就是实例数 ——
	// 差一个倍数说明索引的 BaseVertex/FirstIndex 换算错了，而那只表现为"少了几块砖"。
	// 三角数从资产现取，不写常数：引擎的 BasicShapes/Cube 换过面数（5.7 是 48 不是 12），
	// 写死会让这条断言在下次引擎升级时报一个与出口毫无关系的红。
	const int32 BaseTriangles = BrickMesh->GetNumTriangles(0);
	TestEqual(TEXT("三角数 = 实例数 × 基础网格三角数"), Triangles, Bricks * BaseTriangles);
	TestEqual(TEXT("角点数 = 三角数 × 3"), VertexInstances, Triangles * 3);

	// --- 裁决六 ②：通道与多组 UV 还在 ---
	// 「多组」不是修辞：Cube 带 UV0 + 光照图 UV1，出口必须把两组都带过去。少带一组不报错，
	// 症状是烘焙件在任何读 UV1 的材质下变成一整片同一个像素。
	const int32 BaseUVChannels = BrickMesh->GetNumTexCoords(0);
	TestEqual(TEXT("UV 组数与基础网格资产一致（裁决六 ②）"), UVChannels, BaseUVChannels);
	TestTrue(FString::Printf(TEXT("基础网格本来就不止一组 UV，这条断言才有内容（%d 组）"), BaseUVChannels),
		BaseUVChannels >= 2);
	// **这一条就是裁决六 ③ 的判据本身**。只有一种取值 = 整片同色 = 烘出来的东西不能看，
	// 而三角数、角点数、UV 组数、实例数上面四条**全都会照绿**。
	TestTrue(FString::Printf(TEXT("逐实例随机不止一种取值（%d 种 / %d 个实例）"), DistinctRandoms, Bricks),
		DistinctRandoms > 1);
	// 逐个对 GPU 上 packed 行的 Origin.w，不是"看着都挺随机"。
	TestTrue(TEXT("烘焙件里的逐实例随机与 GPU 上那一份逐个对得上"), bRandomsMatchGpu);

	UEditorAssetLibrary::DeleteAsset(BakePath);
	return true;
}

namespace
{
	/** StableIdentity 用例里盯着的一个实例：句柄 + 它应该在的位置 + 它第一次被看到时的逐实例随机数。 */
	struct FCSIdentityTest_Tracked
	{
		FCSGpuInstanceHandle Handle;
		FVector Location = FVector::ZeroVector;
		float Random = -1.0f;
	};

	/** 句柄指的实例此刻在 packed 行（Morton 序）里的原点与随机数。实例不在 / 行表里找不到它 = false。 */
	bool CSIdentityTest_ReadRow(const UCSGpuInstancedMeshComponent* Component, const FCSGpuInstanceHandle& Handle, FVector3f& OutOrigin, float& OutRandom)
	{
		if (!Component->IsInstanceHandleValid(Handle)) return false;
		const int32 Row = Component->GetPackedInstanceSlots().Find(Handle.Slot);
		if (Row == INDEX_NONE) return false;
		const FVector4f Origin = Component->GetPackedInstances()[Row * CS_GPU_INSTANCED_ROW_FLOAT4S + 3];
		OutOrigin = FVector3f(Origin.X, Origin.Y, Origin.Z);
		OutRandom = Origin.W;
		return true;
	}

	/** 把一个 UPROPERTY 数组清空 —— 用来伪造"2026-10-02 以前存的、没有身份表的资产"。 */
	bool CSIdentityTest_EmptyArrayProperty(UObject* Object, const TCHAR* PropertyName)
	{
		const FArrayProperty* Property = FindFProperty<FArrayProperty>(Object->GetClass(), PropertyName);
		if (!Property) return false;
		FScriptArrayHelper Helper(Property, Property->ContainerPtrToValuePtr<void>(Object));
		Helper.EmptyValues();
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshStableIdentityAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.StableIdentity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 实例身份契约（InstanceCullingOptimization.md §3、§4.2 的验收）：
//   追加、中间删除、尾部删除、槽复用、Morton 重排、成批增长、复制、整套重置之后，
//   同一个存活实例保住句柄、句柄 → 下标的映射与逐实例随机数；删掉的句柄访问不到后来占了同一个槽的新实例。
//
// 为什么非守不可：这条路上所有"跟着实例走"的状态以前都隐式挂在数组下标上，而 RemoveInstance 是把末尾实例
// 换进空位 —— 被换的那个实例下标变了，随机数（按下标取的）跟着变，画面上是"删掉一块砖，远处另一块砖换了个色"。
// 没有任何断言会响：实例数对、变换对、每个随机数都在 [0,1) 里。
bool FCSGpuInstancedMeshStableIdentityAutomationTest::RunTest(const FString& Parameters)
{
	UCSGpuInstancedMeshComponent* Component = MakeComponent(/*InstancesPerCluster*/ 4);
	if (!TestNotNull(TEXT("Component"), Component)) return false;

	TArray<FCSIdentityTest_Tracked> Alive;

	// 活着的每一个：句柄有效、句柄查到的下标上就是它（位置对）、packed 行里找得到它、行里的原点与随机数都是它自己的。
	auto ExpectAlive = [this, Component, &Alive](const TCHAR* Stage)
	{
		for (const FCSIdentityTest_Tracked& Tracked : Alive)
		{
			const int32 Index = Component->GetInstanceIndexFromHandle(Tracked.Handle);
			if (!TestTrue(FString::Printf(TEXT("[%s] 槽 %d 的句柄仍然有效"), Stage, Tracked.Handle.Slot), Index != INDEX_NONE)) continue;

			FTransform Transform;
			TestTrue(FString::Printf(TEXT("[%s] 槽 %d 按句柄取得到变换"), Stage, Tracked.Handle.Slot), Component->GetInstanceTransformByHandle(Tracked.Handle, Transform));
			TestTrue(FString::Printf(TEXT("[%s] 槽 %d 句柄查到的就是原来那个实例（位置没换人）"), Stage, Tracked.Handle.Slot), Transform.GetLocation().Equals(Tracked.Location, 0.01));
			TestTrue(FString::Printf(TEXT("[%s] 槽 %d 下标 → 句柄 → 下标 走得回来"), Stage, Tracked.Handle.Slot), Component->GetInstanceHandle(Index) == Tracked.Handle);

			FVector3f RowOrigin;
			float RowRandom = -1.0f;
			if (!TestTrue(FString::Printf(TEXT("[%s] 槽 %d 在 packed 行表里"), Stage, Tracked.Handle.Slot), CSIdentityTest_ReadRow(Component, Tracked.Handle, RowOrigin, RowRandom))) continue;
			TestTrue(FString::Printf(TEXT("[%s] 槽 %d 的行里是它自己的原点（行 ↔ 槽没有错位）"), Stage, Tracked.Handle.Slot), RowOrigin.Equals(FVector3f(Tracked.Location), 0.01f));
			TestEqual(FString::Printf(TEXT("[%s] 槽 %d 的逐实例随机数没变"), Stage, Tracked.Handle.Slot), RowRandom, Tracked.Random);
		}
		TestEqual(FString::Printf(TEXT("[%s] 行表与实例一样多"), Stage), Component->GetPackedInstanceSlots().Num(), Component->GetInstanceCount());
	};

	// 删掉的句柄：查不到下标、取不到变换、改不动、删不掉，而且这几次失败的调用一个实例都不碰。
	auto ExpectDead = [this, Component](const TCHAR* Stage, const FCSGpuInstanceHandle& Handle)
	{
		const int32 CountBefore = Component->GetInstanceCount();
		FTransform Transform;
		TestFalse(FString::Printf(TEXT("[%s] 旧句柄（槽 %d 代 %d）已失效"), Stage, Handle.Slot, Handle.Generation), Component->IsInstanceHandleValid(Handle));
		TestEqual(FString::Printf(TEXT("[%s] 旧句柄查不到下标"), Stage), Component->GetInstanceIndexFromHandle(Handle), int32(INDEX_NONE));
		TestFalse(FString::Printf(TEXT("[%s] 旧句柄取不到变换"), Stage), Component->GetInstanceTransformByHandle(Handle, Transform));
		TestFalse(FString::Printf(TEXT("[%s] 旧句柄改不动任何实例"), Stage), Component->UpdateInstanceTransformByHandle(Handle, FTransform(FVector(-9999.0, -9999.0, 0.0))));
		TestFalse(FString::Printf(TEXT("[%s] 旧句柄删不掉任何实例"), Stage), Component->RemoveInstanceByHandle(Handle));
		TestEqual(FString::Printf(TEXT("[%s] 失败的按句柄调用没有动实例数"), Stage), Component->GetInstanceCount(), CountBefore);
	};

	auto Track = [this, Component, &Alive](int32 Index, const FVector& Location)
	{
		FCSIdentityTest_Tracked Tracked;
		Tracked.Handle = Component->GetInstanceHandle(Index);
		Tracked.Location = Location;
		FVector3f RowOrigin;
		TestTrue(FString::Printf(TEXT("下标 %d 的新实例在 packed 行表里"), Index), CSIdentityTest_ReadRow(Component, Tracked.Handle, RowOrigin, Tracked.Random));
		Alive.Add(Tracked);
		return Tracked.Handle;
	};

	// ---------------------------------------------------------------------------
	// 起点：4 × 3 的格子，一次 SetInstances。只增不删的内容槽号 == 下标（随机数因此与改动前逐位相同）。
	// ---------------------------------------------------------------------------
	TArray<FTransform> Sources;
	for (int32 Y = 0; Y < 3; ++Y)
	{
		for (int32 X = 0; X < 4; ++X) Sources.Add(FTransform(FVector(X * 200.0, Y * 200.0, 0.0)));
	}
	Component->SetInstances(Sources);

	TArray<float> InitialRandoms;
	TSet<float> DistinctRandoms;
	for (int32 Index = 0; Index < Sources.Num(); ++Index)
	{
		const FCSGpuInstanceHandle Handle = Track(Index, Sources[Index].GetLocation());
		TestEqual(FString::Printf(TEXT("[起点] 下标 %d 的槽号就是下标"), Index), Handle.Slot, Index);
		TestEqual(FString::Printf(TEXT("[起点] 下标 %d 是第 0 代"), Index), Handle.Generation, 0);
		InitialRandoms.Add(Alive.Last().Random);
		DistinctRandoms.Add(Alive.Last().Random);
		TestTrue(TEXT("[起点] 逐实例随机数在 [0,1) 里"), Alive.Last().Random >= 0.0f && Alive.Last().Random < 1.0f);
	}
	TestTrue(FString::Printf(TEXT("[起点] 逐实例随机数不止一种取值（%d 种）"), DistinctRandoms.Num()), DistinctRandoms.Num() > 1);
	TestFalse(TEXT("[起点] 越界下标拿不到句柄"), Component->GetInstanceHandle(Sources.Num()).IsSet());
	ExpectAlive(TEXT("起点"));

	// ---------------------------------------------------------------------------
	// 追加一个很远的实例：包围盒在 Y 上拉长几百倍，Morton 编码的归一化范围跟着变 ⇒ packed 行整体重排。
	// ---------------------------------------------------------------------------
	const TArray<int32> RowsBeforeAppend = Component->GetPackedInstanceSlots();
	const FVector FarLocation(100.0, 200000.0, 0.0);
	const int32 FarIndex = Component->AddInstance(FTransform(FarLocation));
	TestEqual(TEXT("[追加] 新实例在数组末尾"), FarIndex, Sources.Num());
	const FCSGpuInstanceHandle FarHandle = Track(FarIndex, FarLocation);
	TestEqual(TEXT("[追加] 没有空闲槽 ⇒ 新开的槽号 == 下标"), FarHandle.Slot, FarIndex);
	{
		TArray<int32> RowsAfterAppend = Component->GetPackedInstanceSlots();
		RowsAfterAppend.Remove(FarHandle.Slot);
		// 判据要有内容：如果行序没变，下面"重排后随机数不变"就什么都没证明。
		TestTrue(TEXT("[追加] 既有实例的行序确实被重排了"), RowsAfterAppend != RowsBeforeAppend);
	}
	ExpectAlive(TEXT("追加 + Morton 重排"));

	// ---------------------------------------------------------------------------
	// 中间删除：末尾实例（刚加的那个）被换进空位 —— 它的下标变了，句柄与随机数不许变。
	// ---------------------------------------------------------------------------
	const FCSGpuInstanceHandle RemovedMiddle = Alive[3].Handle;
	Alive.RemoveAt(3);
	TestTrue(TEXT("[中间删除] 删掉下标 3"), Component->RemoveInstance(3));
	TestEqual(TEXT("[中间删除] 末尾实例被换进了下标 3"), Component->GetInstanceIndexFromHandle(FarHandle), 3);
	ExpectDead(TEXT("中间删除"), RemovedMiddle);
	ExpectAlive(TEXT("中间删除"));

	// ---------------------------------------------------------------------------
	// 尾部删除：没有实例被搬动。
	// ---------------------------------------------------------------------------
	const int32 TailIndex = Component->GetInstanceCount() - 1;
	const FCSGpuInstanceHandle RemovedTail = Component->GetInstanceHandle(TailIndex);
	Alive.RemoveAll([&RemovedTail](const FCSIdentityTest_Tracked& Tracked) { return Tracked.Handle == RemovedTail; });
	TestTrue(TEXT("[尾部删除] 删掉末尾"), Component->RemoveInstance(TailIndex));
	ExpectDead(TEXT("尾部删除"), RemovedTail);
	ExpectAlive(TEXT("尾部删除"));

	// ---------------------------------------------------------------------------
	// 槽复用：新实例拿最小的空闲槽（3），但那是另一个实例 —— 代数不同，旧句柄仍然访问不到它。
	// ---------------------------------------------------------------------------
	const FVector ReuseLocation(-400.0, -400.0, 0.0);
	const FCSGpuInstanceHandle Reused = Track(Component->AddInstance(FTransform(ReuseLocation)), ReuseLocation);
	TestEqual(TEXT("[槽复用] 新实例拿到的是最小的空闲槽"), Reused.Slot, RemovedMiddle.Slot);
	TestTrue(TEXT("[槽复用] 同一个槽，新的一代"), Reused.Generation > RemovedMiddle.Generation);
	TestTrue(TEXT("[槽复用] 新旧句柄不相等"), Reused != RemovedMiddle);
	ExpectDead(TEXT("槽复用"), RemovedMiddle);
	ExpectAlive(TEXT("槽复用"));

	// ---------------------------------------------------------------------------
	// 按句柄改 / 删。
	// ---------------------------------------------------------------------------
	{
		FCSIdentityTest_Tracked& Moved = Alive[5];
		Moved.Location = FVector(123.0, 456.0, 78.0);
		TestTrue(TEXT("[按句柄] 改变换"), Component->UpdateInstanceTransformByHandle(Moved.Handle, FTransform(Moved.Location)));
		ExpectAlive(TEXT("按句柄改变换"));

		const FCSGpuInstanceHandle RemovedByHandle = Alive[1].Handle;
		Alive.RemoveAt(1);
		TestTrue(TEXT("[按句柄] 删"), Component->RemoveInstanceByHandle(RemovedByHandle));
		ExpectDead(TEXT("按句柄删"), RemovedByHandle);
		ExpectAlive(TEXT("按句柄删"));
	}

	// ---------------------------------------------------------------------------
	// 成批增长：一次加 200 个（越过最小容量与好几个簇）。先用回收的槽，再开新的；此前活着的一个都不许动。
	// ---------------------------------------------------------------------------
	{
		TArray<FTransform> Batch;
		for (int32 Index = 0; Index < 200; ++Index) Batch.Add(FTransform(FVector(3000.0 + Index * 37.0, (Index % 7) * 150.0, 0.0)));
		const TArray<int32> Indices = Component->AddInstances(Batch);
		TestEqual(TEXT("[成批增长] 每个新实例都有下标"), Indices.Num(), Batch.Num());
		TSet<int32> Slots;
		for (int32 Index = 0; Index < Component->GetInstanceCount(); ++Index) Slots.Add(Component->GetInstanceHandle(Index).Slot);
		TestEqual(TEXT("[成批增长] 活着的实例槽号互不重复"), Slots.Num(), Component->GetInstanceCount());
		ExpectAlive(TEXT("成批增长"));
	}

	// ---------------------------------------------------------------------------
	// 复制（PIE / 复制 actor 走的就是这条）：两张身份表跟着拷过去，缓存不跟 —— 副本要自己按真值表重建，
	// 而且重建出来的发槽结果必须与一路增删过来的原件相同（空闲槽是最小堆，不看历史）。
	// ---------------------------------------------------------------------------
	{
		UCSGpuInstancedMeshComponent* Copy = DuplicateObject<UCSGpuInstancedMeshComponent>(Component, GetTransientPackage());
		if (TestNotNull(TEXT("[复制] 副本"), Copy))
		{
			for (const FCSIdentityTest_Tracked& Tracked : Alive)
			{
				TestEqual(FString::Printf(TEXT("[复制] 槽 %d 的句柄在副本里指向同一个下标"), Tracked.Handle.Slot),
					Copy->GetInstanceIndexFromHandle(Tracked.Handle), Component->GetInstanceIndexFromHandle(Tracked.Handle));
			}
			TestFalse(TEXT("[复制] 原件里已失效的句柄在副本里同样失效"), Copy->IsInstanceHandleValid(RemovedTail));

			// 原件再删一个、两边各加一个：副本的缓存是刚重建的，原件的是一路维护过来的。
			const FCSGpuInstanceHandle Victim = Alive[2].Handle;
			Alive.RemoveAt(2);
			TestTrue(TEXT("[复制] 原件按句柄删"), Component->RemoveInstanceByHandle(Victim));
			TestTrue(TEXT("[复制] 副本按同一个句柄删"), Copy->RemoveInstanceByHandle(Victim));
			const FCSGpuInstanceHandle FromOriginal = Component->GetInstanceHandle(Component->AddInstance(FTransform(FVector(-800.0, 0.0, 0.0))));
			const FCSGpuInstanceHandle FromCopy = Copy->GetInstanceHandle(Copy->AddInstance(FTransform(FVector(-800.0, 0.0, 0.0))));
			TestTrue(FString::Printf(TEXT("[复制] 同样的操作在两边发出同一个句柄（原件 槽 %d 代 %d，副本 槽 %d 代 %d）"),
				FromOriginal.Slot, FromOriginal.Generation, FromCopy.Slot, FromCopy.Generation), FromOriginal == FromCopy);
			ExpectAlive(TEXT("复制之后的原件"));

			// 老资产：没有身份表。按下标补发槽、第 0 代 —— 与"从没删过实例"的新内容同一个结果，随机数因此不变。
			TestTrue(TEXT("[老资产] 清掉槽表"), CSIdentityTest_EmptyArrayProperty(Copy, TEXT("PerInstanceSlots")));
			TestTrue(TEXT("[老资产] 清掉代数表"), CSIdentityTest_EmptyArrayProperty(Copy, TEXT("SlotGenerations")));
			bool bLegacyIdentity = true;
			for (int32 Index = 0; Index < Copy->GetInstanceCount(); ++Index)
			{
				const FCSGpuInstanceHandle Handle = Copy->GetInstanceHandle(Index);
				bLegacyIdentity &= Handle.Slot == Index && Handle.Generation == 0 && Copy->GetInstanceIndexFromHandle(Handle) == Index;
			}
			TestTrue(TEXT("[老资产] 没有身份表的实例数组按下标补发槽号"), bLegacyIdentity);
		}
	}

	// ---------------------------------------------------------------------------
	// 整套重置：同一份输入再 SetInstances 一次 —— 此前的句柄全部失效，新实例仍占槽 0..N-1，
	// 随机数与第一次逐位相同（生产者拖尺寸时每帧都这么调，随机数跟着跳就是一片闪烁）。
	// ---------------------------------------------------------------------------
	{
		const TArray<FCSIdentityTest_Tracked> Previous = Alive;
		Alive.Reset();
		Component->SetInstances(Sources);
		for (const FCSIdentityTest_Tracked& Tracked : Previous) TestFalse(FString::Printf(TEXT("[整套重置] 槽 %d 的旧句柄失效"), Tracked.Handle.Slot), Component->IsInstanceHandleValid(Tracked.Handle));
		for (int32 Index = 0; Index < Sources.Num(); ++Index)
		{
			const FCSGpuInstanceHandle Handle = Track(Index, Sources[Index].GetLocation());
			TestEqual(FString::Printf(TEXT("[整套重置] 下标 %d 的槽号又是下标"), Index), Handle.Slot, Index);
			TestTrue(FString::Printf(TEXT("[整套重置] 下标 %d 是新的一代"), Index), Handle.Generation > 0);
			TestEqual(FString::Printf(TEXT("[整套重置] 下标 %d 的随机数与第一次相同"), Index), Alive.Last().Random, InitialRandoms[Index]);
		}
		ExpectAlive(TEXT("整套重置"));
	}

	// ---------------------------------------------------------------------------
	// 清空。
	// ---------------------------------------------------------------------------
	{
		const TArray<FCSIdentityTest_Tracked> Previous = Alive;
		Alive.Reset();
		Component->ClearInstances();
		for (const FCSIdentityTest_Tracked& Tracked : Previous) TestFalse(FString::Printf(TEXT("[清空] 槽 %d 的旧句柄失效"), Tracked.Handle.Slot), Component->IsInstanceHandleValid(Tracked.Handle));
		TestEqual(TEXT("[清空] 行表也空了"), Component->GetPackedInstanceSlots().Num(), 0);
		TestFalse(TEXT("[清空] 没有实例可以取句柄"), Component->GetInstanceHandle(0).IsSet());
	}

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshBatchUpdateAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.BatchUpdate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

// 成批改实例（InstanceCullingOptimization.md §6.1）：BeginInstanceUpdate / EndInstanceUpdate 之间的增、删、改
// 只动 CPU 侧的实例数组与身份表，最外层 End 时统一重排、打包、上传一次。
//
// 守两件事：
//   ① 结果与逐个调用**逐位相同** —— packed 行、簇球、行表、句柄、随机数。合批只许省功夫，不许改答案；
//      发槽的次序（最小空闲槽）不依赖打包发生在什么时候，这里把它钉住。
//   ② 真的只做了一次 —— 打包次数（GetInstancePackSerial）与阻塞刷新次数（UCSMesh::GetBlockingFlushCount）。
//      "攒着传"以前只攒了上传那一半，排序照样每个改动一次（基线：10 万个实例连改 8 个 82 ms），而画面、
//      实例数、每个数值都对 —— 没有任何断言会响，只是慢。
bool FCSGpuInstancedMeshBatchUpdateAutomationTest::RunTest(const FString& Parameters)
{
	// 同一串操作：加一批、改两个、删两个（一个中间一个末尾）、再加两个（复用空槽）、按句柄改一个。
	// 返回按句柄改的那个实例的句柄，调用方拿它对两边。
	auto RunOps = [](UCSGpuInstancedMeshComponent* Component) -> FCSGpuInstanceHandle
	{
		TArray<FTransform> Batch;
		for (int32 Index = 0; Index < 40; ++Index) Batch.Add(FTransform(FVector((Index % 8) * 300.0, (Index / 8) * 300.0, 0.0)));
		Component->AddInstances(Batch);
		Component->UpdateInstanceTransform(3, FTransform(FVector(-500.0, 100.0, 20.0)));
		Component->UpdateInstanceTransform(17, FTransform(FRotator(0.0, 30.0, 0.0), FVector(900.0, -700.0, 0.0), FVector(2.0)));
		Component->RemoveInstance(10);
		Component->RemoveInstance(Component->GetInstanceCount() - 1);
		Component->AddInstance(FTransform(FVector(5000.0, 5000.0, 0.0)));
		const int32 Last = Component->AddInstance(FTransform(FVector(-5000.0, 0.0, 100.0)));
		const FCSGpuInstanceHandle Handle = Component->GetInstanceHandle(Last);
		Component->UpdateInstanceTransformByHandle(Handle, FTransform(FVector(-6000.0, 0.0, 100.0)));
		return Handle;
	};

	UCSGpuInstancedMeshComponent* OneByOne = MakeComponent(/*InstancesPerCluster*/ 4);
	UCSGpuInstancedMeshComponent* Batched = MakeComponent(/*InstancesPerCluster*/ 4);
	if (!TestNotNull(TEXT("Component"), OneByOne) || !TestNotNull(TEXT("Component"), Batched)) return false;

	const int32 OneByOnePacksBefore = OneByOne->GetInstancePackSerial();
	const FCSGpuInstanceHandle OneByOneHandle = RunOps(OneByOne);
	const int32 OneByOnePacks = OneByOne->GetInstancePackSerial() - OneByOnePacksBefore;

	const int32 BatchedPacksBefore = Batched->GetInstancePackSerial();
	FCSGpuInstanceHandle BatchedHandle;
	{
		FCSGpuInstancedUpdateScope Scope(Batched);
		TestTrue(TEXT("[区间内] IsInInstanceUpdate"), Batched->IsInInstanceUpdate());
		{
			// 嵌套：内层 End 什么都不落。
			FCSGpuInstancedUpdateScope Inner(Batched);
			BatchedHandle = RunOps(Batched);
		}
		TestTrue(TEXT("[区间内] 内层 End 之后仍在区间里"), Batched->IsInInstanceUpdate());
		TestEqual(TEXT("[区间内] 一次都没打包"), Batched->GetInstancePackSerial() - BatchedPacksBefore, 0);
		// 实例数、下标、句柄当场有效。
		TestEqual(TEXT("[区间内] 实例数当场反映改动"), Batched->GetInstanceCount(), OneByOne->GetInstanceCount());
		TestTrue(TEXT("[区间内] 新加的实例当场拿得到句柄"), Batched->IsInstanceHandleValid(BatchedHandle));
		FTransform Moved;
		TestTrue(TEXT("[区间内] 按句柄读得到刚改的变换"), Batched->GetInstanceTransformByHandle(BatchedHandle, Moved) && Moved.GetLocation().Equals(FVector(-6000.0, 0.0, 100.0), 0.01));
	}
	TestFalse(TEXT("[区间后] 最外层 End 之后不在区间里"), Batched->IsInInstanceUpdate());
	const int32 BatchedPacks = Batched->GetInstancePackSerial() - BatchedPacksBefore;

	AddInfo(FString::Printf(TEXT("同一串操作：逐个调用打包 %d 次，成批 %d 次"), OneByOnePacks, BatchedPacks));
	TestEqual(TEXT("成批：整串操作只打包一次"), BatchedPacks, 1);
	TestTrue(TEXT("逐个调用：每个改动各打包一次（对照组，判据有内容）"), OneByOnePacks >= 7);

	// ① 结果逐位相同。
	TestTrue(TEXT("句柄相同（发槽次序不依赖打包时机）"), OneByOneHandle == BatchedHandle);
	TestEqual(TEXT("实例数相同"), Batched->GetInstanceCount(), OneByOne->GetInstanceCount());
	TestTrue(TEXT("packed 行逐位相同"), Batched->GetPackedInstances() == OneByOne->GetPackedInstances());
	TestTrue(TEXT("簇球逐位相同"), Batched->GetClusterBounds() == OneByOne->GetClusterBounds());
	TestTrue(TEXT("行 → 稳定槽的表相同"), Batched->GetPackedInstanceSlots() == OneByOne->GetPackedInstanceSlots());
	for (int32 Index = 0; Index < OneByOne->GetInstanceCount(); ++Index)
	{
		if (!(Batched->GetInstanceHandle(Index) == OneByOne->GetInstanceHandle(Index)))
		{
			AddError(FString::Printf(TEXT("下标 %d 的句柄两边不同"), Index));
			break;
		}
	}

	// 区间里读 packed 行：当场补一次打包，读到的是此刻的实例数组；之后没有新改动的话 End 不再重打。
	{
		const int32 PacksBefore = Batched->GetInstancePackSerial();
		Batched->BeginInstanceUpdate();
		const int32 Added = Batched->AddInstance(FTransform(FVector(123.0, 456.0, 0.0)));
		TestEqual(TEXT("[区间内读] 行数当场跟上"), Batched->GetPackedInstances().Num(), Batched->GetInstanceCount() * CS_GPU_INSTANCED_ROW_FLOAT4S);
		TestTrue(TEXT("[区间内读] 行表里有新实例的槽"), Batched->GetPackedInstanceSlots().Contains(Batched->GetInstanceHandle(Added).Slot));
		TestEqual(TEXT("[区间内读] 读触发了一次打包"), Batched->GetInstancePackSerial() - PacksBefore, 1);
		Batched->EndInstanceUpdate();
		TestEqual(TEXT("[区间内读] End 没有再打一遍"), Batched->GetInstancePackSerial() - PacksBefore, 1);
	}

	// bMarkRenderStateDirty=false：一串改动只在最后那次打包。
	{
		const int32 PacksBefore = Batched->GetInstancePackSerial();
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Batched->UpdateInstanceTransform(Index, FTransform(FVector(Index * 50.0, -2000.0, 0.0)), /*bWorldSpace*/ false, /*bMarkRenderStateDirty*/ Index == 7);
		}
		TestEqual(TEXT("[攒着改] 8 个 UpdateInstanceTransform 只打包一次"), Batched->GetInstancePackSerial() - PacksBefore, 1);
		FVector3f Origin;
		float Random = 0.0f;
		TestTrue(TEXT("[攒着改] 第一个改动（没让更新的那些）也进了 packed 行"),
			CSIdentityTest_ReadRow(Batched, Batched->GetInstanceHandle(0), Origin, Random) && Origin.Equals(FVector3f(0.0f, -2000.0f, 0.0f), 0.01f));
	}

	// RemoveInstances：下标是调用那一刻的；重复、越界的跳过；一次打包。
	{
		const int32 CountBefore = Batched->GetInstanceCount();
		TArray<FCSGpuInstanceHandle> Doomed = { Batched->GetInstanceHandle(2), Batched->GetInstanceHandle(5), Batched->GetInstanceHandle(CountBefore - 1) };
		TArray<FCSGpuInstanceHandle> Survivors;
		for (int32 Index = 0; Index < CountBefore; ++Index)
		{
			if (Index != 2 && Index != 5 && Index != CountBefore - 1) Survivors.Add(Batched->GetInstanceHandle(Index));
		}
		const int32 PacksBefore = Batched->GetInstancePackSerial();
		const int32 Removed = Batched->RemoveInstances({ 2, CountBefore - 1, 5, 5, CountBefore + 100, -1 });
		TestEqual(TEXT("[成批删] 删掉三个（重复的、越界的不算）"), Removed, 3);
		TestEqual(TEXT("[成批删] 实例数少了三个"), Batched->GetInstanceCount(), CountBefore - 3);
		TestEqual(TEXT("[成批删] 一次打包"), Batched->GetInstancePackSerial() - PacksBefore, 1);
		for (const FCSGpuInstanceHandle& Handle : Doomed) TestFalse(TEXT("[成批删] 被删的句柄失效"), Batched->IsInstanceHandleValid(Handle));
		bool bSurvivorsAlive = true;
		for (const FCSGpuInstanceHandle& Handle : Survivors) bSurvivorsAlive &= Batched->IsInstanceHandleValid(Handle);
		TestTrue(TEXT("[成批删] 其余实例一个都没被误删"), bSurvivorsAlive);
	}

	// ② 注册在世界里（真的有 GPU 那一半）：区间内零次阻塞刷新、GPU 布局不动；End 之后一次到位。
	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();

	UCSGpuInstancedMeshComponent* Live = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Live->SetupAttachment(Root);
	Live->RegisterComponent();
	Live->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Live->SetBaseMeshFromGpuData(MakeUnitTriangle());
	{
		TArray<FTransform> Initial;
		for (int32 Index = 0; Index < 100; ++Index) Initial.Add(FTransform(FVector(Index * 150.0, 0.0, 0.0)));
		Live->SetInstances(Initial);
	}
	if (!TestEqual(TEXT("[GPU] 起点：布局里 100 个源实例"), int32(Live->GetGpuLayout().NumSourceInstances), 100)) return false;

	// 一个改动的阻塞刷新次数（对照）。
	const int64 FlushesBeforeSingle = UCSMesh::GetBlockingFlushCount();
	Live->AddInstance(FTransform(FVector(-300.0, 0.0, 0.0)));
	const int64 FlushesPerSingle = UCSMesh::GetBlockingFlushCount() - FlushesBeforeSingle;

	const int64 FlushesBeforeBatch = UCSMesh::GetBlockingFlushCount();
	Live->BeginInstanceUpdate();
	for (int32 Index = 0; Index < 16; ++Index) Live->AddInstance(FTransform(FVector(Index * 150.0, 400.0, 0.0)));
	Live->RemoveInstance(0);
	Live->UpdateInstanceTransform(1, FTransform(FVector(0.0, -400.0, 0.0)));
	TestEqual(TEXT("[GPU] 区间内一次阻塞刷新都没有"), int32(UCSMesh::GetBlockingFlushCount() - FlushesBeforeBatch), 0);
	TestEqual(TEXT("[GPU] 区间内 GPU 布局停在区间之前"), int32(Live->GetGpuLayout().NumSourceInstances), 101);
	Live->EndInstanceUpdate();
	const int64 FlushesPerBatch = UCSMesh::GetBlockingFlushCount() - FlushesBeforeBatch;
	AddInfo(FString::Printf(TEXT("[GPU] 单个 AddInstance %lld 次阻塞刷新；18 个改动成批 %lld 次"), FlushesPerSingle, FlushesPerBatch));
	TestTrue(TEXT("[GPU] 成批的阻塞刷新不比单个改动多"), FlushesPerBatch <= FlushesPerSingle);
	TestEqual(TEXT("[GPU] End 之后布局跟上了（101 + 16 − 1）"), int32(Live->GetGpuLayout().NumSourceInstances), 116);

	Host->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

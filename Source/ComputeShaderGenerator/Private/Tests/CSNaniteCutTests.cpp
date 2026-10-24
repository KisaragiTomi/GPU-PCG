#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSNaniteCut.h"
#include "CSNaniteCutHLODActor.h"
#include "CSStaticMeshAssetSink.h"

#include "AssetCompilingManager.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "Tests/AutomationEditorCommon.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MeshDescription.h"
#include "RenderUtils.h"
#include "StaticMeshAttributes.h"
#include "StaticMeshResources.h"
#include "UObject/Package.h"

namespace
{
	/** 16×16 格 = 512 三角：Nanite 切成 4 个左右的叶 cluster，往上两级收成一个根 —— 有层级可走，
	 *  又小到整张 DAG 通常都落在常驻的 root 页里，测试体里不用等流送就能拿到精确截面。 */
	constexpr int32 CSNaniteCutTest_Quads = 16;
	constexpr int32 CSNaniteCutTest_Triangles = CSNaniteCutTest_Quads * CSNaniteCutTest_Quads * 2;
	constexpr float CSNaniteCutTest_Size = 1000.0f;

	/**
	 * 起伏的网格面，左右两半各用一个材质槽，开 Nanite、同步构建。起伏让各级简化误差都大于 0，
	 * 否则平面会一路塌成两个三角，层级里没有东西可测。
	 */
	UStaticMesh* CSNaniteCutTest_MakeGrid(UMaterialInterface* MaterialA, UMaterialInterface* MaterialB)
	{
		FMeshDescription Description;
		FStaticMeshAttributes Attributes(Description);
		Attributes.Register();

		TVertexAttributesRef<FVector3f> Positions = Attributes.GetVertexPositions();
		TVertexInstanceAttributesRef<FVector3f> Normals = Attributes.GetVertexInstanceNormals();
		TVertexInstanceAttributesRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
		TPolygonGroupAttributesRef<FName> SlotNames = Attributes.GetPolygonGroupMaterialSlotNames();

		const FPolygonGroupID GroupA = Description.CreatePolygonGroup();
		const FPolygonGroupID GroupB = Description.CreatePolygonGroup();
		SlotNames[GroupA] = CSStaticMeshAsset::MaterialSlotName(0);
		SlotNames[GroupB] = CSStaticMeshAsset::MaterialSlotName(1);

		const int32 Side = CSNaniteCutTest_Quads + 1;
		TArray<FVertexID> Vertices;
		Vertices.Reserve(Side * Side);
		for (int32 Y = 0; Y < Side; ++Y)
		{
			for (int32 X = 0; X < Side; ++X)
			{
				const float U = float(X) / float(CSNaniteCutTest_Quads);
				const float V = float(Y) / float(CSNaniteCutTest_Quads);
				const float Height = 60.0f * FMath::Sin(U * UE_TWO_PI * 1.5f) * FMath::Cos(V * UE_TWO_PI);
				const FVertexID Vertex = Description.CreateVertex();
				Positions[Vertex] = FVector3f((U - 0.5f) * CSNaniteCutTest_Size, (V - 0.5f) * CSNaniteCutTest_Size, Height);
				Vertices.Add(Vertex);
			}
		}

		auto Corner = [&](int32 X, int32 Y)
		{
			const FVertexInstanceID Instance = Description.CreateVertexInstance(Vertices[Y * Side + X]);
			Normals[Instance] = FVector3f(0.0f, 0.0f, 1.0f);
			UVs.Set(Instance, 0, FVector2f(float(X) / float(CSNaniteCutTest_Quads), float(Y) / float(CSNaniteCutTest_Quads)));
			return Instance;
		};
		for (int32 Y = 0; Y < CSNaniteCutTest_Quads; ++Y)
		{
			for (int32 X = 0; X < CSNaniteCutTest_Quads; ++X)
			{
				const FPolygonGroupID Group = (X < CSNaniteCutTest_Quads / 2) ? GroupA : GroupB;
				Description.CreateTriangle(Group, { Corner(X, Y), Corner(X, Y + 1), Corner(X + 1, Y + 1) });
				Description.CreateTriangle(Group, { Corner(X, Y), Corner(X + 1, Y + 1), Corner(X + 1, Y) });
			}
		}

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSNaniteCutGrid")), RF_Transient);
		const TArray<UMaterialInterface*> Materials = { MaterialA, MaterialB };
		if (!CSStaticMeshAsset::PopulateFromDescription(Mesh, Description, Materials, {}, /*bCommitMeshDescription*/ true, /*bEnableNanite*/ true)) return nullptr;
		FAssetCompilingManager::Get().FinishAllCompilation();
		return Mesh;
	}

	/** 三角形的几何法线与三个角存的法线同向的比例：只拿来比较镜像前后是否一致，不预设哪个方向是"正"。 */
	float CSNaniteCutTest_FacingAgreement(const FCSGpuMeshCPUData& Data)
	{
		int32 Agree = 0;
		int32 Total = 0;
		for (int32 Tri = 0; Tri + 2 < Data.Indices.Num(); Tri += 3)
		{
			const uint32 A = Data.Indices[Tri + 0];
			const uint32 B = Data.Indices[Tri + 1];
			const uint32 C = Data.Indices[Tri + 2];
			if (!Data.Positions.IsValidIndex(int32(FMath::Max3(A, B, C))) || !Data.Normals.IsValidIndex(int32(FMath::Max3(A, B, C)))) continue;
			const FVector3f Geometric = FVector3f::CrossProduct(Data.Positions[B] - Data.Positions[A], Data.Positions[C] - Data.Positions[A]);
			if (Geometric.SizeSquared() < 1e-6f) continue;
			const FVector3f Stored = Data.Normals[A] + Data.Normals[B] + Data.Normals[C];
			++Total;
			if (FVector3f::DotProduct(Geometric, Stored) > 0.0f) ++Agree;
		}
		return Total > 0 ? float(Agree) / float(Total) : 0.0f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSNaniteCutScreenErrorAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.NaniteCut.CutErrorForScreenError",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

/** 距离 → CutError 的换算与 Nanite 自己的 LODScale 一致：1920 宽、水平 FOV 90° 时 LODScale = 960。 */
bool FCSNaniteCutScreenErrorAutomationTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("960 cm 处 1 像素 = 1 cm"), UCSNaniteCutOps::CutErrorForScreenError(960.0f), 1.0f, 1e-4f);
	TestEqual(TEXT("256 m 处 1 像素 ≈ 26.67 cm"), UCSNaniteCutOps::CutErrorForScreenError(25600.0f), 25600.0f / 960.0f, 1e-3f);
	TestEqual(TEXT("像素误差线性放大"), UCSNaniteCutOps::CutErrorForScreenError(9600.0f, 3.0f), 30.0f, 1e-3f);
	TestEqual(TEXT("4K 宽时减半"), UCSNaniteCutOps::CutErrorForScreenError(9600.0f, 1.0f, 3840.0f), 5.0f, 1e-3f);
	TestEqual(TEXT("负距离当 0"), UCSNaniteCutOps::CutErrorForScreenError(-5.0f), 0.0f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSNaniteCutExtractAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.NaniteCut.Extract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 端到端：真 Nanite 资源 → GPU 遍历 / 挑选 / 解码 → UCSMesh → 回读。
 *   ① CutError = 0 取到最细一级：三角数正好等于源网格，材质两槽都在、按指针并进目标表；
 *   ② CutError 极大只剩根 cluster（≤128 三角）；CutError 递增时三角数单调不增；
 *   ③ 位置都落在变换后的包围盒里（验变换真的烘进去了）；
 *   ④ 同样的输入写出逐位相同的网格（原子分配的顺序被排序抹掉了）；
 *   ⑤ 追加模式接在已有内容之后；
 *   ⑥ 镜像变换自动翻绕序：绕序与法线的关系和不镜像时一样；
 *   ⑦ 不是 Nanite 的网格报 NotNanite、什么都不写。
 * 新建的网格从没被渲染过，根页要靠算子自己推一次流送更新才上传 —— 能拿到结果本身就验了那一步。
 */
bool FCSNaniteCutExtractAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，截面抽取无从验起（跳过，不算通过）。"));
		return true;
	}

	UMaterialInterface* MaterialA = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInterface* MaterialB = UMaterialInstanceDynamic::Create(MaterialA, GetTransientPackage());
	UStaticMesh* Grid = CSNaniteCutTest_MakeGrid(MaterialA, MaterialB);
	if (!TestNotNull(TEXT("网格建出来了"), Grid)) return false;
	if (!TestTrue(TEXT("网格有 Nanite 数据"), Grid->HasValidNaniteData())) return false;

	const FTransform Transform(FRotator(0.0, 30.0, 0.0), FVector(5000.0, -3000.0, 200.0), FVector(2.0, 2.0, 1.0));
	const FBox WorldBounds = Grid->GetBoundingBox().TransformBy(Transform).ExpandBy(1.0);

	FCSNaniteCutOptions Replace;
	Replace.bAppend = false;

	// ① 最细一级
	UCSMesh* Target = UCSMeshOps::AllocateGpuMesh(GetTransientPackage());
	FCSNaniteCutResult Fine;
	UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Transform, 0.0f, Replace, Fine);
	if (!TestEqual(TEXT("一个源一个结果"), Fine.Sources.Num(), 1)) return false;
	const FCSNaniteCutSourceResult& FineSource = Fine.Sources[0];
	AddInfo(FString::Printf(TEXT("CutError 0：clusters %d、三角 %d、被迫 %d、状态 %d"),
		FineSource.Clusters, FineSource.Triangles, FineSource.ForcedClusters, int32(FineSource.Status)));
	if (!TestTrue(TEXT("抽到了东西"), FineSource.Status == ECSNaniteCutStatus::Complete || FineSource.Status == ECSNaniteCutStatus::Incomplete)) return false;
	if (FineSource.Status == ECSNaniteCutStatus::Complete)
	{
		TestEqual(TEXT("CutError 0 且完整时，三角数 = 源网格"), FineSource.Triangles, CSNaniteCutTest_Triangles);
		TestTrue(TEXT("最细一级不止一个 cluster（真的走了层级）"), FineSource.Clusters > 1);
	}
	else
	{
		AddWarning(TEXT("测试网格有页不在 root 页里、这次没流进来，'三角数 = 源网格' 这一条验不了。"));
	}
	TestEqual(TEXT("写出的三角数 = 统计"), Fine.WrittenTriangles, FineSource.Triangles);
	TestEqual(TEXT("目标网格的三角数"), Target->GetTriangleCountSync(), FineSource.Triangles);
	TestEqual(TEXT("两个材质按指针并进目标表"), Target->Materials.Num(), 2);

	FCSGpuMeshCPUData FineData;
	if (!TestTrue(TEXT("回读"), Target->ReadbackMeshSync(FineData))) return false;
	TestEqual(TEXT("回读的索引数"), FineData.Indices.Num(), FineSource.Triangles * 3);
	int32 OutsideBounds = 0;
	for (const FVector3f& Position : FineData.Positions) if (!WorldBounds.IsInsideOrOn(FVector(Position))) ++OutsideBounds;
	TestEqual(TEXT("所有顶点都在变换后的包围盒里"), OutsideBounds, 0);
	TSet<int32> UsedSlots;
	for (int32 Slot : FineData.TriangleMaterialSlots) UsedSlots.Add(Slot);
	TestTrue(TEXT("两个材质槽都用上了"), UsedSlots.Contains(0) && UsedSlots.Contains(1));
	TestEqual(TEXT("材质号只落在 0 / 1"), UsedSlots.Num(), 2);

	// ② 根与单调
	FCSNaniteCutResult Coarse;
	UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Transform, 1.0e9f, Replace, Coarse);
	TestEqual(TEXT("CutError 极大时只剩根 cluster"), Coarse.Sources[0].Clusters, 1);
	TestTrue(TEXT("根 cluster ≤ 128 三角"), Coarse.Sources[0].Triangles > 0 && Coarse.Sources[0].Triangles <= 128);
	TestTrue(TEXT("根一定在 root 页里：完整"), Coarse.Sources[0].Status == ECSNaniteCutStatus::Complete);

	int32 PreviousTriangles = MAX_int32;
	int32 NonMonotonic = 0;
	for (const float CutError : { 0.0f, 0.5f, 2.0f, 8.0f, 32.0f, 128.0f, 1.0e9f })
	{
		FCSNaniteCutResult Step;
		UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Transform, CutError, Replace, Step);
		if (Step.Sources[0].Triangles > PreviousTriangles) ++NonMonotonic;
		PreviousTriangles = Step.Sources[0].Triangles;
	}
	TestEqual(TEXT("CutError 递增时三角数单调不增"), NonMonotonic, 0);

	// ④ 确定性
	// 结果另开变量：FineSource 引用着 Fine.Sources，复用 Fine 会释放那块数组、让引用悬空。
	FCSNaniteCutResult FineAgain;
	UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Transform, 0.0f, Replace, FineAgain);
	FCSGpuMeshCPUData Again;
	if (TestTrue(TEXT("再回读一次"), Target->ReadbackMeshSync(Again)))
	{
		TestTrue(TEXT("同样的输入写出逐位相同的位置"), Again.Positions == FineData.Positions);
		TestTrue(TEXT("同样的输入写出逐位相同的索引"), Again.Indices == FineData.Indices);
	}

	// ⑤ 追加
	FCSNaniteCutOptions Append;
	Append.bAppend = true;
	FCSNaniteCutResult Appended;
	UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Transform, 0.0f, Append, Appended);
	TestEqual(TEXT("追加后三角数翻倍"), Target->GetTriangleCountSync(), FineSource.Triangles * 2);
	TestEqual(TEXT("追加不重复加材质"), Target->Materials.Num(), 2);

	// ⑥ 镜像
	FTransform Mirrored = Transform;
	Mirrored.SetScale3D(FVector(-2.0, 2.0, 1.0));
	FCSNaniteCutResult MirroredResult;
	UCSNaniteCutOps::AppendNaniteCut(Target, Grid, Mirrored, 0.0f, Replace, MirroredResult);
	FCSGpuMeshCPUData MirroredData;
	if (TestTrue(TEXT("镜像回读"), Target->ReadbackMeshSync(MirroredData)))
	{
		const float Plain = CSNaniteCutTest_FacingAgreement(FineData);
		const float Flipped = CSNaniteCutTest_FacingAgreement(MirroredData);
		AddInfo(FString::Printf(TEXT("绕序与法线同向的比例：不镜像 %.3f，镜像 %.3f"), Plain, Flipped));
		TestTrue(TEXT("不镜像时绕序与法线的关系是一致的"), FMath::Abs(Plain - 0.5f) > 0.4f);
		TestTrue(TEXT("镜像后绕序与法线的关系不变（自动翻了绕序）"), (Plain > 0.5f) == (Flipped > 0.5f) && FMath::Abs(Flipped - 0.5f) > 0.4f);
	}

	// ⑦ 非 Nanite
	UStaticMesh* PlainCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (PlainCube && !PlainCube->HasValidNaniteData())
	{
		FCSNaniteCutResult NotNanite;
		UCSNaniteCutOps::AppendNaniteCut(Target, PlainCube, FTransform::Identity, 0.0f, Append, NotNanite);
		TestTrue(TEXT("非 Nanite 网格报 NotNanite"), NotNanite.Sources[0].Status == ECSNaniteCutStatus::NotNanite);
		TestEqual(TEXT("非 Nanite 网格什么都不写"), NotNanite.WrittenTriangles, 0);
	}

	Target->ReleaseDeferred();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSNaniteCutHLODActorAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.NaniteCut.HLODActor",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 关卡内试验台 ACSNaniteCutHLODActor：
 *   ① 收集只收盒子里的、跳过带排除标签的与盒子外的；
 *   ② 抽取结果装进它自己的 GPU 网格、按材质分了 section；
 *   ③ ShowHLOD / ShowSources 只临时隐藏真进了 HLOD 的源（不碰被排除的、盒外的、非 Nanite 的），
 *      ClearHLOD 全部放出来。非 Nanite 的那条是回归：它在 HLOD 里没有替身，曾经被一起藏掉、凭空消失。
 */
bool FCSNaniteCutHLODActorAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，试验台无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UMaterialInterface* MaterialA = UMaterial::GetDefaultMaterial(MD_Surface);
	UMaterialInterface* MaterialB = UMaterialInstanceDynamic::Create(MaterialA, GetTransientPackage());
	UStaticMesh* Grid = CSNaniteCutTest_MakeGrid(MaterialA, MaterialB);
	if (!TestNotNull(TEXT("网格建出来了"), Grid) || !TestTrue(TEXT("网格有 Nanite 数据"), Grid->HasValidNaniteData())) return false;

	auto Spawn = [World, Grid](const FVector& Location) -> AStaticMeshActor*
	{
		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(AStaticMeshActor::StaticClass(), FTransform(Location), SpawnParameters);
		if (!Actor) return nullptr;
		Actor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
		Actor->GetStaticMeshComponent()->SetStaticMesh(Grid);
		return Actor;
	};
	AStaticMeshActor* InsideA = Spawn(FVector(0.0, 0.0, 0.0));
	AStaticMeshActor* InsideB = Spawn(FVector(1500.0, 0.0, 0.0));
	AStaticMeshActor* Excluded = Spawn(FVector(-1500.0, 0.0, 0.0));
	AStaticMeshActor* Outside = Spawn(FVector(20000.0, 0.0, 0.0));
	if (!InsideA || !InsideB || !Excluded || !Outside)
	{
		AddError(TEXT("源 actor 没生成出来"));
		return false;
	}
	Excluded->Tags.Add(UCSNaniteCutOps::DefaultExcludeTag());
	UStaticMesh* PlainCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	AStaticMeshActor* NonNanite = (PlainCube && !PlainCube->HasValidNaniteData()) ? Spawn(FVector(0.0, 1500.0, 0.0)) : nullptr;
	if (NonNanite) NonNanite->GetStaticMeshComponent()->SetStaticMesh(PlainCube);
	else AddWarning(TEXT("引擎 Cube 缺失或带 Nanite，'非 Nanite 的不藏' 这一条验不了。"));

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	ACSNaniteCutHLODActor* HLOD = World->SpawnActor<ACSNaniteCutHLODActor>(ACSNaniteCutHLODActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("试验台 actor"), HLOD)) return false;
	HLOD->GatherBox->SetBoxExtent(FVector(4000.0, 4000.0, 1000.0));
	HLOD->SwitchDistance = 0.0f;   // CutError 0：最细一级，三角数可以精确预言
	World->SendAllEndOfFrameUpdates();

	HLOD->BuildHLOD();
	TestEqual(TEXT("盒子里、没打排除标签的都交给抽取（非 Nanite 的也算一个源）"), HLOD->LastNumSources, NonNanite ? 3 : 2);
	TestEqual(TEXT("带排除标签的一个被数到"), HLOD->LastNumExcluded, 1);
	TestEqual(TEXT("源 actor 两个"), HLOD->GetSourceActors().Num(), 2);
	TestTrue(TEXT("HLOD 网格绑到了显示组件上"), HLOD->HLODMeshComponent->GetGpuMesh() == HLOD->GetHLODMesh() && HLOD->GetHLODMesh() != nullptr);
	if (HLOD->LastResult.bAllComplete)
	{
		TestEqual(TEXT("两份最细截面"), HLOD->LastResult.WrittenTriangles, 2 * CSNaniteCutTest_Triangles);
	}
	else
	{
		AddWarning(TEXT("测试网格有页不在 root 页里，'两份最细截面' 这一条验不了。"));
	}
	if (UCSMesh* Mesh = HLOD->GetHLODMesh())
	{
		TestEqual(TEXT("两种材质各一个 section"), Mesh->GetSections().Num(), 2);
	}

	TestTrue(TEXT("显示 HLOD 时收进来的源被临时隐藏"), InsideA->IsTemporarilyHiddenInEditor() && InsideB->IsTemporarilyHiddenInEditor());
	TestFalse(TEXT("被排除的不藏"), Excluded->IsTemporarilyHiddenInEditor());
	TestFalse(TEXT("盒子外的不藏"), Outside->IsTemporarilyHiddenInEditor());
	if (NonNanite) TestFalse(TEXT("非 Nanite 的没进 HLOD，不藏"), NonNanite->IsTemporarilyHiddenInEditor());

	HLOD->ShowSources();
	TestFalse(TEXT("ShowSources 放出源"), InsideA->IsTemporarilyHiddenInEditor());
	TestFalse(TEXT("ShowSources 藏起 HLOD"), HLOD->HLODMeshComponent->IsVisible());

	HLOD->ShowHLOD();
	TestTrue(TEXT("ShowHLOD 再藏源"), InsideB->IsTemporarilyHiddenInEditor());
	TestTrue(TEXT("ShowHLOD 显示 HLOD"), HLOD->HLODMeshComponent->IsVisible());

	HLOD->ClearHLOD();
	TestFalse(TEXT("ClearHLOD 放出全部源"), InsideA->IsTemporarilyHiddenInEditor() || InsideB->IsTemporarilyHiddenInEditor());
	TestNull(TEXT("ClearHLOD 解绑显示"), HLOD->HLODMeshComponent->GetGpuMesh());

	// 挑选标签：只收带 PickTag 的。标签写法故意与属性里填的不一致（少下划线、大小写不同、带空格），
	// 按用户要求这类差异要一视同仁。
	InsideA->Tags.Add(FName(TEXT("nanitecuthlodpick")));
	HLOD->PickTag = FName(TEXT("NaniteCutHLOD_Pick"));
	HLOD->BuildHLOD();
	TestEqual(TEXT("填了 PickTag 就只收带它的那一个"), HLOD->LastNumSources, 1);
	TestEqual(TEXT("源 actor 只剩带挑选标签的"), HLOD->GetSourceActors().Num(), 1);
	TestTrue(TEXT("留下的是 InsideA"), HLOD->GetSourceActors().Num() == 1 && HLOD->GetSourceActors()[0] == InsideA);

	// 两个标签一起：挑中的又被排除 ⇒ 不进，而且是按"排除"计数的。
	InsideA->Tags.Add(UCSNaniteCutOps::DefaultExcludeTag());
	HLOD->BuildHLOD();
	TestEqual(TEXT("挑中又被排除的不进"), HLOD->LastNumSources, 0);
	TestEqual(TEXT("它计在排除数里"), HLOD->LastNumExcluded, 1);

	HLOD->PickTag = NAME_None;
	HLOD->BuildHLOD();
	TestEqual(TEXT("PickTag 清空 = 不限制，回到只按排除标签筛"), HLOD->LastNumSources, NonNanite ? 2 : 1);
	HLOD->ClearHLOD();
	return true;
}

#endif

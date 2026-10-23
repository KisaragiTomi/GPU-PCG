#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedNaniteComponent.h"

#include "Components/SceneCaptureComponent2D.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "MaterialDomain.h"                  // MD_Surface（unity 构建下靠邻居带进来，-SingleFile 会露馅）
#include "Materials/Material.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "Tests/AutomationEditorCommon.h"
#include "TextureResource.h"
#include "UObject/Package.h"

namespace
{
	/** 相机离地高度与画幅。90° 视角下，地面一侧的半宽恰好等于高度。 */
	constexpr double CSNaniteTest_CameraHeight = 600.0;
	constexpr int32 CSNaniteTest_ImageSize = 64;
	/** 实例间距。Cube 是居中的 100cm 立方体，300 的间距在画面上留出 ~6 像素的空隙。 */
	constexpr double CSNaniteTest_Spacing = 300.0;

	/** 引擎 Cube 复制一份、开 Nanite、同步构建。测的是渲染路，不能赌 Content/ 里有没有 Nanite 资产。 */
	UStaticMesh* CSNaniteTest_MakeNaniteCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSNaniteModeCube")));
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		Mesh->SetNaniteSettings(Settings);
		// 传 OutErrors 会关掉异步编译 —— 测试要的就是这一行返回时渲染数据已经在了。
		TArray<FText> Errors;
		Mesh->Build(/*bInSilent*/ true, &Errors);
		return Mesh;
	}

	/** 「十字 + 四角」九个位置：十字那五个是要看见的，四角是用来藏的。
	 *  两组在任何翻转 / 90° 旋转下都各自映射回自己，所以判据不依赖相机的轴向约定。 */
	TArray<FVector> CSNaniteTest_CrossPositions()
	{
		const double S = CSNaniteTest_Spacing;
		return { FVector(0, 0, 0), FVector(S, 0, 0), FVector(-S, 0, 0), FVector(0, S, 0), FVector(0, -S, 0) };
	}

	TArray<FVector> CSNaniteTest_CornerPositions()
	{
		const double S = CSNaniteTest_Spacing;
		return { FVector(S, S, 0), FVector(S, -S, 0), FVector(-S, S, 0), FVector(-S, -S, 0) };
	}

	/** 按组件的打包约定（FCSGpuInstanceSourceGPU）把平移打成 5 float4 一行。 */
	void CSNaniteTest_AppendRow(TArray<FVector4f>& Rows, const FVector& Location, float Random)
	{
		const FMatrix44f M = FMatrix44f(FTransform(Location).ToMatrixWithScale());
		Rows.Add(FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f));
		Rows.Add(FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f));
		Rows.Add(FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f));
		Rows.Add(FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], Random));
		Rows.Add(FVector4f(FVector3f(Location), 87.0f)); // 剔除球：Nanite 不读，照约定填
	}

	/** 像生产者那样在 GPU 上备一份实例源：行 + 计数器，都停在 SRVMask。 */
	FCSGpuInstanceSourceGPU CSNaniteTest_MakeGpuSource(const TArray<FVector4f>& Rows, uint32 LiveCount, const FBox& LocalBounds)
	{
		FCSGpuInstanceSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSNaniteTestMakeSource)([&Source, Rows, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RowsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Rows.Num()), TEXT("CSNaniteTest.Rows"));
			GraphBuilder.QueueBufferUpload(RowsBuffer, Rows.GetData(), Rows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSNaniteTest.Count"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.PackedInstances = GraphBuilder.ConvertToExternalBuffer(RowsBuffer);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(RowsBuffer, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = uint32(Rows.Num() / 5);
		Source.LocalBounds = LocalBounds;
		return Source;
	}

	/** 点刷那样的点云源：世界空间位置 + 朝上的法线，计数器只放前 LiveCount 个。 */
	FCSGpuInstancePointSourceGPU CSNaniteTest_MakePointSource(const TArray<FVector>& WorldPositions, uint32 LiveCount)
	{
		TArray<FVector4f> Positions;
		TArray<FVector4f> Normals;
		FBox WorldBounds(ForceInit);
		for (const FVector& P : WorldPositions)
		{
			Positions.Add(FVector4f(FVector3f(P), 1.0f));
			Normals.Add(FVector4f(0.0f, 0.0f, 1.0f, 0.0f));
			WorldBounds += P;
		}

		FCSGpuInstancePointSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSNaniteTestMakePointSource)([&Source, Positions, Normals, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			auto Upload = [&GraphBuilder](const TArray<FVector4f>& Data, const TCHAR* Name)
			{
				FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Data.Num()), Name);
				GraphBuilder.QueueBufferUpload(Buffer, Data.GetData(), Data.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
				TRefCountPtr<FRDGPooledBuffer> Pooled = GraphBuilder.ConvertToExternalBuffer(Buffer);
				GraphBuilder.SetBufferAccessFinal(Buffer, ERHIAccess::SRVMask);
				return Pooled;
			};
			Source.Positions = Upload(Positions, TEXT("CSNaniteTest.PointPositions"));
			Source.Normals = Upload(Normals, TEXT("CSNaniteTest.PointNormals"));
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSNaniteTest.PointCount"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = uint32(WorldPositions.Num());
		Source.InstanceScale = 1.0f;
		Source.WorldBounds = WorldBounds;
		return Source;
	}

	/** 生产者"原地重写计数器、不重新交接"的那种改法。 */
	void CSNaniteTest_WriteCounter(const FCSGpuInstanceSourceGPU& Source, uint32 LiveCount)
	{
		ENQUEUE_RENDER_COMMAND(CSNaniteTestWriteCounter)([Counter = Source.Counter, LiveCount](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef CountBuffer = GraphBuilder.RegisterExternalBuffer(Counter);
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CountBuffer, PF_R32_UINT)), LiveCount);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
	}

	/** 从 Centre 正上方俯拍一张场景深度。真渲染一帧 —— GPU-Scene 的更新（包括我们的写入 pass）就发生在这里面。 */
	TArray<float> CSNaniteTest_CaptureDepth(UWorld* World, const FVector& Centre)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->RenderTargetFormat = RTF_RGBA32f;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(CSNaniteTest_ImageSize, CSNaniteTest_ImageSize);
		Target->UpdateResourceImmediate(true);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			Centre + FVector(0.0, 0.0, CSNaniteTest_CameraHeight), FRotator(-90.0, 0.0, 0.0), SpawnParameters);
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		Capture->TextureTarget = Target;
		Capture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		Capture->FOVAngle = 90.0f;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;

		// 帧末的组件更新（换代理、变换）在测试体里没人替我们跑。
		World->SendAllEndOfFrameUpdates();
		// 拍两次：Nanite 的根页在第一帧里才上传，不赌它同帧可见。
		Capture->CaptureScene();
		Capture->CaptureScene();
		FlushRenderingCommands();

		TArray<FLinearColor> Pixels;
		Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
		CaptureActor->Destroy();

		TArray<float> Depth;
		Depth.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels) Depth.Add(Pixel.R);
		return Depth;
	}

	/** 实例（相对拍摄中心）在画面上落在哪个像素 —— 取立方体高度中点的深度，3x3 邻域都在剪影内。 */
	FIntPoint CSNaniteTest_Project(const FVector& Relative)
	{
		const double Half = CSNaniteTest_ImageSize * 0.5;
		const double Scale = Half / CSNaniteTest_CameraHeight;
		// pitch -90 的相机：画面右 = +Y、画面上 = +X。九个位置对这个约定对称，写反了判据也不变。
		return FIntPoint(int32(FMath::RoundToDouble(Half + Relative.Y * Scale)), int32(FMath::RoundToDouble(Half - Relative.X * Scale)));
	}

	/** 3x3 邻域里有几何（深度落在相机与地面之间，背景是远平面）。 */
	bool CSNaniteTest_HasGeometry(const TArray<float>& Depth, const FIntPoint& Pixel, bool bRequireAll)
	{
		int32 Hits = 0, Samples = 0;
		for (int32 Dy = -1; Dy <= 1; ++Dy)
		{
			for (int32 Dx = -1; Dx <= 1; ++Dx)
			{
				const int32 X = FMath::Clamp(Pixel.X + Dx, 0, CSNaniteTest_ImageSize - 1);
				const int32 Y = FMath::Clamp(Pixel.Y + Dy, 0, CSNaniteTest_ImageSize - 1);
				const float D = Depth[Y * CSNaniteTest_ImageSize + X];
				++Samples;
				if (D > 1.0f && D < float(CSNaniteTest_CameraHeight) * 2.0f) ++Hits;
			}
		}
		return bRequireAll ? Hits == Samples : Hits > 0;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshNaniteModeAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.NaniteMode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * Nanite 路的端到端判据：**真渲染**，从深度图里数实例。另外守着"走不走 Nanite 由资产决定"这条规则：
 * 没开 Nanite 的资产不建替身；资产上把 Nanite 关掉并重建，组件要自己退回 GPU 剔除路。
 *
 * CPU 侧的断言（替身在、代理是 Nanite、写入发给了当前代理）只证明"我们以为发出去了"。GPU-Scene 的
 * 实例区间里到底写了什么，引擎没有公开的回读口 —— 所以唯一的真值是画面：该出现的位置有深度、
 * 该藏的位置没有、组件挪走之后旧位置空了。这三件事分别守着写入 pass 的三条分支：
 *   ① 活着的槽写实例变换（InitializeInstanceSceneDataLS）；
 *   ② 计数器之外的槽写 HIDDEN —— 容量 > 活着的个数时，尾巴是上一代或者垃圾；
 *   ③ 图元一动就重写 —— GPU-Scene 存的是世界变换，引擎对 GPU-only 实例从不重传。
 * 任何一条漏了，CPU 侧所有断言照绿，画面是错的（本项目的经典失效形状）。
 */
bool FCSGpuInstancedMeshNaniteModeAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，Nanite 路无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;

	UStaticMesh* NaniteCube = CSNaniteTest_MakeNaniteCube();
	if (!TestNotNull(TEXT("Nanite cube"), NaniteCube)) return false;
	if (!TestTrue(TEXT("复制出来的 Cube 有 Nanite 数据"), NaniteCube->HasValidNaniteData())) return false;

	// 离原点远远的：新关卡里就算有什么东西，也进不了俯拍的画面。
	const FVector Origin(0.0, 0.0, 20000.0);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(Origin);

	UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Instanced->SetupAttachment(Root);
	Instanced->RegisterComponent();
	// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
	Instanced->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	// 没有开关可设：先给一张没开 Nanite 的资产，必须留在 GPU 剔除路上。
	UStaticMesh* PlainCube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!TestNotNull(TEXT("Plain engine cube"), PlainCube)) return false;
	if (!PlainCube->IsNaniteEnabled())
	{
		Instanced->SetBaseMesh(PlainCube);
		TestFalse(TEXT("没开 Nanite 的资产 ⇒ 不走 Nanite 路"), Instanced->IsNaniteRenderPath());
		TestNull(TEXT("没开 Nanite 的资产 ⇒ 不建替身"), Instanced->GetNaniteComponent());
	}
	else
	{
		AddWarning(TEXT("引擎的 BasicShapes/Cube 开着 Nanite（或 r.Nanite.ForceEnableMeshes），'没开就不走' 这一条验不了。"));
	}

	// 换成开了 Nanite 的那张：什么都不用设，自己换路。
	Instanced->SetBaseMesh(NaniteCube);

	const TArray<FVector> Cross = CSNaniteTest_CrossPositions();
	const TArray<FVector> Corners = CSNaniteTest_CornerPositions();
	TArray<FVector> AllNine = Cross;
	AllNine.Append(Corners);

	auto ExpectPicture = [this](const TCHAR* Stage, const TArray<float>& Depth, const TArray<FVector>& Present, const TArray<FVector>& Absent)
	{
		for (const FVector& P : Present)
		{
			TestTrue(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处画出了实例"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ true));
		}
		for (const FVector& P : Absent)
		{
			TestFalse(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处什么都没有"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ false));
		}
	};

	// ---------------------------------------------------------------------------
	// CPU 实例数组：十字五个
	// ---------------------------------------------------------------------------
	{
		TArray<FTransform> Transforms;
		for (const FVector& P : Cross) Transforms.Add(FTransform(P));
		Instanced->SetInstances(Transforms);
	}

	TestTrue(TEXT("BaseMesh 开了 Nanite ⇒ 自动走 Nanite 路"), Instanced->IsNaniteRenderPath());
	UCSGpuInstancedNaniteComponent* Nanite = Instanced->GetNaniteComponent();
	if (!TestNotNull(TEXT("Nanite 替身建起来了"), Nanite)) return false;
	TestEqual(TEXT("GPU-Scene 槽数 = CPU 实例数"), Nanite->GetGpuSceneSlotCount(), uint32(Cross.Num()));
	TestEqual(TEXT("诊断计数 = CPU 实例数"), Instanced->DebugReadDrawnInstanceCountSync(), Cross.Num());
	{
		const FString Mismatch = Instanced->DebugGetDrawnAssetMismatchSync();
		TestTrue(FString::Printf(TEXT("画的就是这一族（%s）"), Mismatch.IsEmpty() ? TEXT("一致") : *Mismatch), Mismatch.IsEmpty());
	}
	TestNull(TEXT("本体自己不进场景（画的是替身）"), Instanced->GetSceneProxy());

	const uint32 WritesBeforeCpu = Nanite->DebugGetDispatchedWriteCount();
	const TArray<float> CpuDepth = CSNaniteTest_CaptureDepth(World, Origin);
	AddInfo(FString::Printf(TEXT("[CPU 源] 派发了 %u 次写入"), Nanite->DebugGetDispatchedWriteCount() - WritesBeforeCpu));
	TestTrue(TEXT("渲染时 GPU-Scene 写入 pass 真被派发了"), Nanite->DebugGetDispatchedWriteCount() > WritesBeforeCpu);
	ExpectPicture(TEXT("CPU 源"), CpuDepth, Cross, Corners);

	// ---------------------------------------------------------------------------
	// GPU packed 源：容量 9，先活 9 个，再原地把计数器改成 5 —— 四角必须重新藏起来
	// ---------------------------------------------------------------------------
	TArray<FVector4f> Rows;
	for (int32 Index = 0; Index < Cross.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Cross[Index], 0.1f * float(Index));
	for (int32 Index = 0; Index < Corners.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Corners[Index], 0.5f + 0.1f * float(Index));
	const FBox SourceBounds(FVector(-CSNaniteTest_Spacing - 100.0, -CSNaniteTest_Spacing - 100.0, -100.0), FVector(CSNaniteTest_Spacing + 100.0, CSNaniteTest_Spacing + 100.0, 100.0));
	const FCSGpuInstanceSourceGPU GpuSource = CSNaniteTest_MakeGpuSource(Rows, /*LiveCount*/ 9u, SourceBounds);
	Instanced->SetInstanceSourceGPU(GpuSource);

	TestTrue(TEXT("替身喂的是 GPU 源"), Nanite == Instanced->GetNaniteComponent() && Nanite->IsFeedingGpuSource());
	TestEqual(TEXT("GPU-Scene 槽数 = 源容量"), Nanite->GetGpuSceneSlotCount(), 9u);
	TestEqual(TEXT("诊断读到的是 GPU 计数器"), Instanced->DebugReadDrawnInstanceCountSync(), 9);
	{
		// 槽数 5 → 9 ⇒ 代理整个换掉，而且是在帧末更新里换的：FStaticMeshComponentBulkReregisterContext
		// 让新代理晚于 CreateRenderState_Concurrent 才出生，那个钩子发不出写入（2026-09-10 实测：这一拍
		// 派发 0 次、九个位置全空，重发一次才画出来）。补写靠 CaptureScene 开头那次 SendAllEndOfFrameUpdates
		// 的 Pre 广播 —— 与游戏里 BeginRenderingViewFamily 同一个时机。
		const uint32 WritesBefore = Nanite->DebugGetDispatchedWriteCount();
		const TArray<float> Depth = CSNaniteTest_CaptureDepth(World, Origin);
		const uint32 Dispatched = Nanite->DebugGetDispatchedWriteCount() - WritesBefore;
		TestTrue(FString::Printf(TEXT("[GPU 源 9/9] 帧末批量重建出来的新代理当帧就收到了写入（派发 %u 次）"), Dispatched), Dispatched > 0);
		ExpectPicture(TEXT("GPU 源 9/9"), Depth, AllNine, {});
	}

	CSNaniteTest_WriteCounter(GpuSource, 5u);
	// 游戏里是替身每帧的 Tick 重写；测试体里没有 Tick，重交接一次同一份源走的是同一个入口（槽数不变 ⇒ 当帧重写）。
	Instanced->SetInstanceSourceGPU(GpuSource);
	TestEqual(TEXT("诊断读到缩回来的计数"), Instanced->DebugReadDrawnInstanceCountSync(), 5);
	ExpectPicture(TEXT("GPU 源 5/9"), CSNaniteTest_CaptureDepth(World, Origin), Cross, Corners);

	// ---------------------------------------------------------------------------
	// 挪走整个 actor：实例必须跟着走，旧位置必须空
	// ---------------------------------------------------------------------------
	const FVector Moved = Origin + FVector(4000.0, 0.0, 0.0);
	Root->SetWorldLocation(Moved);
	ExpectPicture(TEXT("挪走后的新位置"), CSNaniteTest_CaptureDepth(World, Moved), Cross, Corners);
	ExpectPicture(TEXT("挪走后的旧位置"), CSNaniteTest_CaptureDepth(World, Origin), {}, AllNine);

	// ---------------------------------------------------------------------------
	// 点云源：点是世界空间的，Nanite 路先用经典路那个打包 pass 现打临时行、再写 GPU-Scene。
	// 放在挪走之后做：组件此时不在世界原点，WorldToComponent 那一步写反了就会整体偏走。
	// ---------------------------------------------------------------------------
	{
		TArray<FVector> WorldPoints;
		for (const FVector& P : AllNine) WorldPoints.Add(Moved + P);
		Instanced->SetInstanceSourceFromPoints(CSNaniteTest_MakePointSource(WorldPoints, /*LiveCount*/ 5u));
		TestEqual(TEXT("[点云源] GPU-Scene 槽数 = 点容量"), Nanite->GetGpuSceneSlotCount(), 9u);
		const uint32 WritesBefore = Nanite->DebugGetDispatchedWriteCount();
		const TArray<float> Depth = CSNaniteTest_CaptureDepth(World, Moved);
		TestTrue(TEXT("[点云源] 写入 pass 派发了"), Nanite->DebugGetDispatchedWriteCount() > WritesBefore);
		ExpectPicture(TEXT("点云源 5/9"), Depth, Cross, Corners);
	}

	// ---------------------------------------------------------------------------
	// 资产上把 Nanite 关掉并重建：组件要自己退回 GPU 剔除路，替身必须整个消失
	//
	// 走的是编辑器里真实的那条链：OnPostMeshBuild → 挂一次性 Pre-EOF → 下一次 SendAllEndOfFrameUpdates
	// 开头重判。同步 Build 也会经过 FinishBuildInternal，所以 PostMeshBuild 就在这一行里广播。
	// ---------------------------------------------------------------------------
	{
		FMeshNaniteSettings Settings = NaniteCube->GetNaniteSettings();
		Settings.bEnabled = false;
		NaniteCube->SetNaniteSettings(Settings);
		TArray<FText> Errors;
		NaniteCube->Build(/*bInSilent*/ true, &Errors);
		World->SendAllEndOfFrameUpdates();

		TestFalse(TEXT("资产关了 Nanite ⇒ 重建后不再走 Nanite 路"), Instanced->IsNaniteRenderPath());
		TestNull(TEXT("资产关了 Nanite ⇒ 替身被销毁"), Instanced->GetNaniteComponent());
		// 退回 GPU 剔除路之后那条路要真的接得住：点云源照样有常驻缓冲、有布局。
		TestTrue(TEXT("退回 GPU 剔除路后常驻缓冲建起来了"), Instanced->GetGpuLayout().IsValid());
	}

	Host->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshNaniteDistanceBandAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.NaniteDistanceBand",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 2026-09-29：Nanite 路的距离带与影子副本。
 *
 * ① 距离带的逐实例抖动在 GPU-Scene 写入 pass 里做（引擎的 Nanite 距离剔除只有硬边）。判据仍是画面：
 *    九个实例各给一个逐实例随机数，视点钉在十字中心，按 `边界 − Fade × 随机数` 逐个算出该不该在 ——
 *    外边界（近处组件）与内边界（远处组件）各验一轮。引擎的硬边刻意放得比相机实际距离宽，
 *    这样画面上缺的每一个都只能是写入 pass 藏的。
 * ② 影子副本（bClassicShadowOnNanitePath）：开着时本体自己也建一份经典代理、替身不投影；
 *    关投影 / 关开关时那份代理与常驻缓冲都要退掉。这里只验 CPU 能证明的那一半（代理在不在、替身投不投影），
 *    "只进阴影、不进主通道"由代理的相关性保证，画面判据见 Docs/TinyGlade/Ground_Cover.md 的出图。
 */
bool FCSGpuInstancedMeshNaniteDistanceBandAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，Nanite 路无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	UStaticMesh* NaniteCube = CSNaniteTest_MakeNaniteCube();
	if (!TestNotNull(TEXT("Nanite cube"), NaniteCube) || !NaniteCube->HasValidNaniteData()) return false;

	const FVector Origin(0.0, 0.0, 20000.0);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(Origin);

	UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Instanced->SetupAttachment(Root);
	Instanced->RegisterComponent();
	Instanced->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Instanced->SetBaseMesh(NaniteCube);

	// 九个实例：十字中心 0、四个臂 300、四个角 424（离视点的水平距离，视点钉在十字中心）。
	const double S = CSNaniteTest_Spacing;
	struct FProbe { FVector P; float Random; };
	const TArray<FProbe> Probes = {
		{ FVector(0, 0, 0),  0.10f },
		{ FVector(S, 0, 0),  0.90f }, { FVector(-S, 0, 0), 0.20f }, { FVector(0, S, 0), 0.95f }, { FVector(0, -S, 0), 0.30f },
		{ FVector(S, S, 0),  0.50f }, { FVector(S, -S, 0), 0.70f }, { FVector(-S, S, 0), 0.10f }, { FVector(-S, -S, 0), 0.80f },
	};
	TArray<FVector4f> Rows;
	for (const FProbe& Probe : Probes) CSNaniteTest_AppendRow(Rows, Probe.P, Probe.Random);
	const FBox SourceBounds(FVector(-S - 100.0, -S - 100.0, -100.0), FVector(S + 100.0, S + 100.0, 100.0));
	const FCSGpuInstanceSourceGPU GpuSource = CSNaniteTest_MakeGpuSource(Rows, uint32(Probes.Num()), SourceBounds);

	// 视点钉在十字中心（与 ACSGroundActor 给脚本机位钉的是同一个入口）。
	Instanced->SetDistanceCullViewOverride(Origin);

	auto RunStage = [&](const TCHAR* Stage, float Start, float End, float Fade)
	{
		Instanced->InstanceStartCullDistance = Start;
		Instanced->InstanceEndCullDistance = End;
		Instanced->InstanceCullFadeDistance = Fade;
		Instanced->SetInstanceSourceGPU(GpuSource);   // 同一份源再交接一次 ⇒ 替身重新抄设置、当帧重写

		TArray<FVector> Present, Absent;
		for (const FProbe& Probe : Probes)
		{
			const double Dist = FVector2D(Probe.P.X, Probe.P.Y).Size();
			const double Pull = double(Fade) * FMath::Clamp(double(Probe.Random), 0.0, 1.0);
			const bool bCut = (End > 0.0f && Dist > double(End) - Pull) || (Start > 0.0f && Dist < double(Start) - Pull);
			(bCut ? Absent : Present).Add(Probe.P);
		}
		AddInfo(FString::Printf(TEXT("[%s] 期望 %d 个在、%d 个被写入 pass 藏掉"), Stage, Present.Num(), Absent.Num()));
		TestTrue(FString::Printf(TEXT("[%s] 两种结局都出现了（判据有区分度）"), Stage), Present.Num() > 0 && Absent.Num() > 0);

		const TArray<float> Depth = CSNaniteTest_CaptureDepth(World, Origin);
		for (const FVector& P : Present)
		{
			TestTrue(FString::Printf(TEXT("[%s] (%.0f, %.0f) 在带内、画出来了"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ true));
		}
		for (const FVector& P : Absent)
		{
			TestFalse(FString::Printf(TEXT("[%s] (%.0f, %.0f) 越过自己那条抖动边界、被藏掉了"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ false));
		}
	};

	// 外边界（近处组件的形状）：End 1000 远大于俯拍相机到任何一个实例的距离（600–735），引擎的硬边一个都不切。
	// 臂 300：随机数 > 0.778 的被藏；角 424：> 0.640 的被藏；中心恒在。
	RunStage(TEXT("外边界 End=1000 Fade=900"), 0.0f, 1000.0f, 900.0f);
	UCSGpuInstancedNaniteComponent* Nanite = Instanced->GetNaniteComponent();
	TestNotNull(TEXT("Nanite 替身"), Nanite);

	// 内边界（远处组件的形状）：Start 400、Fade 200 ⇒ 引擎的硬边在 200，相机距离全在 600 以上，同样一个都不切。
	// 中心 0 恒被藏；臂 300：随机数 < 0.5 的被藏；角 424 恒在。
	RunStage(TEXT("内边界 Start=400 Fade=200"), 400.0f, 0.0f, 200.0f);

	// ---------------------------------------------------------------------------
	// 影子副本
	// ---------------------------------------------------------------------------
	Instanced->InstanceStartCullDistance = 0.0f;
	Instanced->InstanceEndCullDistance = 0.0f;
	Instanced->InstanceCullFadeDistance = 0.0f;
	Instanced->SetCastShadow(true);
	Instanced->SetClassicShadowOnNanitePath(true);
	World->SendAllEndOfFrameUpdates();
	TestTrue(TEXT("[影子副本] 开关开着且走 Nanite 路 ⇒ 副本生效"), Instanced->IsClassicShadowCopyActive());
	TestTrue(TEXT("[影子副本] 经典常驻缓冲建起来了"), Instanced->GetGpuLayout().IsValid());
	TestNotNull(TEXT("[影子副本] 本体自己也进场景（只投影的经典代理）"), Instanced->GetSceneProxy());
	if (Nanite)
	{
		Nanite->SyncFromOwner(*Instanced);   // 替身每帧在 Tick 里从本体抄设置；测试体里没有 Tick
		TestFalse(TEXT("[影子副本] 替身不投影（影子归经典副本，免得 VSM 里再落一份）"), Nanite->CastShadow);
	}
	// 画面上仍是同一族：主通道由替身画，副本不进主通道也不改变深度。
	TestTrue(TEXT("[影子副本] 十字中心照样画出来"), CSNaniteTest_HasGeometry(CSNaniteTest_CaptureDepth(World, Origin), CSNaniteTest_Project(FVector::ZeroVector), true));

	Instanced->SetCastShadow(false);
	World->SendAllEndOfFrameUpdates();
	TestNull(TEXT("[影子副本] 不投影时那份经典代理不必存在"), Instanced->GetSceneProxy());

	Instanced->SetCastShadow(true);
	Instanced->SetClassicShadowOnNanitePath(false);
	World->SendAllEndOfFrameUpdates();
	TestFalse(TEXT("[影子副本] 关掉开关 ⇒ 副本退场"), Instanced->IsClassicShadowCopyActive());
	TestFalse(TEXT("[影子副本] 关掉开关 ⇒ 经典常驻缓冲还回去"), Instanced->GetGpuLayout().IsValid());
	TestNull(TEXT("[影子副本] 关掉开关 ⇒ 本体不进场景"), Instanced->GetSceneProxy());
	if (Nanite)
	{
		Nanite->SyncFromOwner(*Instanced);
		TestTrue(TEXT("[影子副本] 关掉开关 ⇒ 替身重新投影"), Nanite->CastShadow);
	}

	Host->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshNaniteNotifyAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.NaniteNotify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * Nanite 路的 GPU 源"改了就通知"（InstanceCullingOptimization.md §7，FCSGpuInstanceSourceGPU::bNotifiesChanges）。
 *
 * 老契约是生产者原地重写 buffer、从不通知，所以替身每帧把整份源重抄进 GPU-Scene —— 数据一个字没变也抄，
 * VSM 跟着每帧把这批实例的缓存页作废。承诺了通知的源只在被通知时重写。这里守的是验收里的那几条：
 *   · 静止数据不再写（写入次数为 0），画面不丢；
 *   · 没承诺的源照旧每帧写（老生产者一个字不用改）；
 *   · 原地改了并通知 ⇒ 当帧可见，缩减计数后没有幽灵实例；
 *   · 图元一动照样重写（GPU-Scene 里存的是世界变换）；
 *   · 距离带跟相机走：视点不动不写，挪够了才写。
 * "承诺了却没通知 ⇒ 画面停在上一次"也钉在这里 —— 它不是想要的行为，是这份契约的代价，钉住是为了证明
 * "跳过"真的发生了（否则上面那条"写入次数为 0"可以靠别的巧合成立）。
 */
bool FCSGpuInstancedMeshNaniteNotifyAutomationTest::RunTest(const FString& Parameters)
{
	if (!UseNanite(GMaxRHIShaderPlatform))
	{
		AddWarning(TEXT("这台机器 / 这个 RHI 不支持 Nanite，Nanite 路无从验起（跳过，不算通过）。"));
		return true;
	}

	UWorld* World = FAutomationEditorCommonUtils::CreateNewMap();
	if (!TestNotNull(TEXT("Editor test world"), World)) return false;
	UStaticMesh* NaniteCube = CSNaniteTest_MakeNaniteCube();
	if (!TestNotNull(TEXT("Nanite cube"), NaniteCube) || !NaniteCube->HasValidNaniteData()) return false;

	const FVector Origin(0.0, 0.0, 20000.0);
	FActorSpawnParameters SpawnParameters;
	SpawnParameters.ObjectFlags = RF_Transient;
	AActor* Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
	if (!TestNotNull(TEXT("Host actor"), Host)) return false;
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Host->SetRootComponent(Root);
	Root->RegisterComponent();
	Root->SetWorldLocation(Origin);

	UCSGpuInstancedMeshComponent* Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
	Instanced->SetupAttachment(Root);
	Instanced->RegisterComponent();
	Instanced->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	Instanced->SetBaseMesh(NaniteCube);

	const TArray<FVector> Cross = CSNaniteTest_CrossPositions();
	const TArray<FVector> Corners = CSNaniteTest_CornerPositions();
	TArray<FVector> AllNine = Cross;
	AllNine.Append(Corners);

	TArray<FVector4f> Rows;
	for (int32 Index = 0; Index < Cross.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Cross[Index], 0.1f * float(Index));
	for (int32 Index = 0; Index < Corners.Num(); ++Index) CSNaniteTest_AppendRow(Rows, Corners[Index], 0.5f + 0.1f * float(Index));
	const FBox SourceBounds(FVector(-CSNaniteTest_Spacing - 100.0, -CSNaniteTest_Spacing - 100.0, -100.0), FVector(CSNaniteTest_Spacing + 100.0, CSNaniteTest_Spacing + 100.0, 100.0));
	const FCSGpuInstanceSourceGPU Silent = CSNaniteTest_MakeGpuSource(Rows, /*LiveCount*/ 9u, SourceBounds);
	FCSGpuInstanceSourceGPU Notifying = Silent;   // 同一批 buffer，只是生产者承诺了通知
	Notifying.bNotifiesChanges = true;

	auto ExpectPicture = [this](const TCHAR* Stage, const TArray<float>& Depth, const TArray<FVector>& Present, const TArray<FVector>& Absent)
	{
		for (const FVector& P : Present)
		{
			TestTrue(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处画出了实例"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ true));
		}
		for (const FVector& P : Absent)
		{
			TestFalse(FString::Printf(TEXT("[%s] (%.0f, %.0f) 处什么都没有"), Stage, P.X, P.Y), CSNaniteTest_HasGeometry(Depth, CSNaniteTest_Project(P), /*bRequireAll*/ false));
		}
	};

	Instanced->SetInstanceSourceGPU(Silent);
	UCSGpuInstancedNaniteComponent* Nanite = Instanced->GetNaniteComponent();
	if (!TestNotNull(TEXT("Nanite 替身建起来了"), Nanite)) return false;

	// 一"帧" = 替身 Tick 一次（测试体里没有世界 Tick）+ 真渲染一次（写入 pass 在渲染里才派发）。返回这几帧里派发了几次写入。
	FVector Centre = Origin;
	TArray<float> LastDepth;
	auto RunFrames = [&](int32 Frames) -> uint32
	{
		const uint32 Before = Nanite->DebugGetDispatchedWriteCount();
		for (int32 Frame = 0; Frame < Frames; ++Frame)
		{
			Nanite->TickComponent(1.0f / 60.0f, LEVELTICK_All, nullptr);
			LastDepth = CSNaniteTest_CaptureDepth(World, Centre);
		}
		return Nanite->DebugGetDispatchedWriteCount() - Before;
	};

	// ---------------------------------------------------------------------------
	// 老契约：没承诺通知 ⇒ 每帧都写
	// ---------------------------------------------------------------------------
	RunFrames(1);   // 交接后的第一帧：新代理、新区间
	ExpectPicture(TEXT("不通知的源"), LastDepth, AllNine, {});
	{
		const uint32 Writes = RunFrames(3);
		AddInfo(FString::Printf(TEXT("[不通知的源] 3 帧派发了 %u 次写入"), Writes));
		TestTrue(TEXT("[不通知的源] 数据没变也照旧每帧写（老生产者不受影响）"), Writes >= 3u);
	}

	// ---------------------------------------------------------------------------
	// 承诺了通知：静止数据一次都不写，画面不丢
	// ---------------------------------------------------------------------------
	Instanced->SetInstanceSourceGPU(Notifying);
	TestTrue(TEXT("替身还是那一个"), Nanite == Instanced->GetNaniteComponent());
	RunFrames(1);   // 交接本身算一次改动
	ExpectPicture(TEXT("会通知的源：交接后"), LastDepth, AllNine, {});
	{
		const uint32 Writes = RunFrames(4);
		TestEqual(TEXT("[会通知的源] 数据没变的 4 帧里一次都没写"), Writes, 0u);
		ExpectPicture(TEXT("会通知的源：静止 4 帧"), LastDepth, AllNine, {});
	}

	// ---------------------------------------------------------------------------
	// 原地把计数器从 9 改成 5：没通知 ⇒ 没写（契约的代价）；通知 ⇒ 当帧重写，四角藏起来
	// ---------------------------------------------------------------------------
	CSNaniteTest_WriteCounter(Notifying, 5u);
	{
		const uint32 Writes = RunFrames(2);
		TestEqual(TEXT("[改了没通知] 没有写入"), Writes, 0u);
		ExpectPicture(TEXT("改了没通知：画面停在上一次写入"), LastDepth, AllNine, {});
	}
	Instanced->NotifyInstanceSourceChanged();
	{
		const uint32 Writes = RunFrames(1);
		TestTrue(TEXT("[通知] 当帧写了一次"), Writes >= 1u);
		ExpectPicture(TEXT("通知之后：缩减计数没有幽灵实例"), LastDepth, Cross, Corners);
	}
	TestEqual(TEXT("[通知] 之后又安静下来"), RunFrames(3), 0u);

	// ---------------------------------------------------------------------------
	// 图元一动照样重写：实例跟着走，旧位置空
	// ---------------------------------------------------------------------------
	{
		const FVector Moved = Origin + FVector(4000.0, 0.0, 0.0);
		Root->SetWorldLocation(Moved);
		Centre = Moved;
		const uint32 Writes = RunFrames(1);
		TestTrue(TEXT("[挪走] 图元动了 ⇒ 重写"), Writes >= 1u);
		ExpectPicture(TEXT("挪走后的新位置"), LastDepth, Cross, Corners);
		ExpectPicture(TEXT("挪走后的旧位置"), CSNaniteTest_CaptureDepth(World, Origin), {}, AllNine);
		TestEqual(TEXT("[挪走] 之后又安静下来"), RunFrames(3), 0u);
	}

	// ---------------------------------------------------------------------------
	// 距离带：带要跟相机走。视点不动不写；挪不到 10 cm 不写；挪够了写。
	// ---------------------------------------------------------------------------
	{
		Instanced->InstanceEndCullDistance = 100000.0f;
		Instanced->InstanceCullFadeDistance = 100.0f;
		Instanced->SetDistanceCullViewOverride(Centre);
		Instanced->SetInstanceSourceGPU(Notifying);   // 重新交接一次：替身抄设置、当帧重写
		RunFrames(1);
		ExpectPicture(TEXT("距离带（边界远在外面）"), LastDepth, Cross, Corners);
		TestEqual(TEXT("[距离带] 视点不动：不写"), RunFrames(3), 0u);

		Instanced->SetDistanceCullViewOverride(Centre + FVector(5.0, 0.0, 0.0));
		TestEqual(TEXT("[距离带] 视点挪了 5 cm：不写"), RunFrames(2), 0u);

		Instanced->SetDistanceCullViewOverride(Centre + FVector(80.0, 0.0, 0.0));
		TestTrue(TEXT("[距离带] 视点挪了 80 cm：重写"), RunFrames(1) >= 1u);
		TestEqual(TEXT("[距离带] 之后又安静下来"), RunFrames(2), 0u);
	}

	Host->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

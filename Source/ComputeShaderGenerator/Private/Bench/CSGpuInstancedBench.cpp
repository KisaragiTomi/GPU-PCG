// 实例路的性能基线 / A-B 跑测（InstanceCullingOptimization.md §4.1 与 §9.1）。
//
// 用法（**独立进程**，`-game`，不是编辑器里的命令）：
//   UnrealEditor.exe <uproject> /Engine/Maps/Entry?game=/Script/Engine.GameModeBase -game -windowed -ResX=1920 -ResY=1080
//       -unattended -nosplash -nosound -ExecCmds="CSGpuInstanced.Bench suite=baseline out=<目录>"
// 跑完写 `<目录>/summary.json` 与每个机位一份 CsvProfile 的 `.csv`，然后自己退出（`exit=0` 可以留着进程）。
// 现成的包装脚本：Scripts/RunGpuInstancedBench.ps1。
//
// 量什么：同一台机器、同一个机位、同一批实例下，各条渲染路的整帧耗时与分段 GPU 耗时。
//   · 整帧：CsvProfile 的 FrameTime / GameThreadTime / RenderThreadTime / GPUTime；
//   · 分段：`r.GPUCsvStatsEnabled 1` 之后进同一张 CSV 的 GPU/ 分类 —— 引擎自己的（Basepass / Prepass /
//     ShadowDepths / NaniteVisBuffer …）加上本插件的 CSGpuInstanced*（见 CSGpuInstancedMeshSceneProxy.cpp）；
//   · CPU 侧：CSGpuInstanced/ 分类（渲染线程建剔除图的毫秒、派发行数、GPU-Scene 写入次数）；
//   · 显存：常驻流按描述符算出来的字节数（确切值）+ 适配器实测的占用差（含 RDG 池与驱动开销，只作旁证）；
//   · 经典路的可见数 / LOD 分布：每个机位量完回读一次压缩游标。
//
// 为什么是 `-game` 而不是编辑器里的场景捕获：整帧口径要的是真的一帧（一个主视图、没有 Slate 编辑器 UI、
// 没有帧率节流），CsvProfile 的 FrameTime / GPUTime 才是别人说"帧时间"时指的那个数。
//
// 真实内容：`suite=map` 不造场景，量**已经加载的那张地图**里现成的实例组件（把 Entry 换成要量的地图，`delay=秒`
// 等它的生产者把实例建完）。机位照样是那五个，绕着这些组件的包围盒摆；`look=x/y/z dist=厘米` 再加一个对着指定位置的
// （日志里逐个列出了认领到的组件与它们的位置）。合成场景量不到的东西 —— 真材质、真的生产者、带 custom data 的藤叶 —— 在这里。
//
// 交错 A/B：`variants=A名:命令;命令~B名:命令;命令` + `pairs=N`（命令里的空格写成 `@`：`r.Foo@1`）。每个机位按
// A B / B A / A B … 的次序各量一遍，
// 每次切换都重建一遍实例组件的渲染状态（在代理构造时读 CVar 的开关因此生效）。判据看各对同号，不看单次读数。
//
// ⚠️ 这是开发期工具：非 Shipping 的编辑器二进制才编译（Nanite 测试网格要走编辑器的网格构建，取数靠 CsvProfile）。

#include "CoreMinimal.h"
#include "ProfilingDebugging/CsvProfiler.h"

#if !UE_BUILD_SHIPPING && WITH_EDITOR && CSV_PROFILER

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuInstancedNaniteComponent.h"
#include "CSGpuMemoryBudget.h"
#include "CSGpuMeshComponent.h"
#include "CSMesh.h"
#include "CSStaticMeshAssetSink.h"

#include "AssetCompilingManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Materials/Material.h"
#include "MaterialDomain.h"
#include "Math/RandomStream.h"
#include "MeshDescription.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RenderUtils.h"
#include "RHI.h"
#include "ShaderCompiler.h"
#include "StaticMeshResources.h"
#include "UnrealClient.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedBench, Log, All);

namespace CSGpuInstancedBench
{
	// -------------------------------------------------------------------------
	// 场景描述
	// -------------------------------------------------------------------------

	enum class ERoute : uint8 { Classic, Nanite, ISM, HISM, Map };
	enum class ESource : uint8 { Cpu, GpuPacked, GpuPoints };
	enum class EMeshKind : uint8 { Cube, Sphere };

	const TCHAR* ToString(ERoute Route)
	{
		switch (Route)
		{
		case ERoute::Classic: return TEXT("classic");
		case ERoute::Nanite: return TEXT("nanite");
		case ERoute::ISM: return TEXT("ism");
		case ERoute::Map: return TEXT("map");
		default: return TEXT("hism");
		}
	}

	const TCHAR* ToString(ESource Source)
	{
		switch (Source)
		{
		case ESource::Cpu: return TEXT("cpu");
		case ESource::GpuPacked: return TEXT("gpu_packed");
		default: return TEXT("gpu_points");
		}
	}

	struct FScenario
	{
		FString Name;
		ERoute Route = ERoute::Classic;
		ESource Source = ESource::Cpu;
		EMeshKind Mesh = EMeshKind::Cube;
		int32 NumInstances = 100000;
		/** GPU 源：缓冲容量 = 活跃数 × 这个倍数（"容量远大于活跃范围"）。CPU 源的容量由组件自己棘轮。 */
		int32 CapacityFactor = 1;
		/** Sphere 才有：1..4 级，逐级约 1/4 三角。 */
		int32 NumLODs = 1;
		/** Sphere 才有：1 或 2 个材质段（上 / 下半球）。 */
		int32 NumSections = 1;
		/** 把实例按空间瓦片分给这么多个组件（取整到平方数）。 */
		int32 NumComponents = 1;
		/** 用开了 Nanite 的那张 Cube。Nanite 路恒为真；ISM 上设它 = 引擎原生的 Nanite 实例（对照组）。 */
		bool bNaniteMesh = false;
		bool bCastShadow = true;
		/** GPU 源承诺"改了就通知"（FCSGpuInstanceSourceGPU::bNotifiesChanges）。跑测里的源从不改，所以承诺了 = Nanite 路一次都不重写。 */
		bool bNotifiesChanges = false;
		float StartCullDistance = 0.0f;
		float EndCullDistance = 0.0f;
		float CullFadeDistance = 0.0f;
	};

	struct FVariant
	{
		FString Name;
		TArray<FString> Commands;
	};

	struct FPose
	{
		FString Name;
		FVector Location = FVector::ZeroVector;
		FRotator Rotation = FRotator::ZeroRotator;
		/** 绕场心转圈（每帧固定角步长，与帧率无关 ⇒ 每一帧的画面内容可复现）。 */
		bool bOrbit = false;
		double OrbitRadius = 0.0;
		double OrbitHeight = 0.0;
	};

	struct FColumnStats
	{
		double Median = 0.0;
		double P95 = 0.0;
		double Mean = 0.0;
		int32 Samples = 0;
	};

	struct FMeasurement
	{
		FString Pose;
		FString Variant;
		int32 Pair = 0;
		int32 Frames = 0;
		FString CsvFile;
		TArray<int32> VisiblePerLod;      // 经典路才有；空 = 读不到
		int64 ResidentBytes = 0;          // 量这一步时经典组件常驻流的字节数（变体会换布局，所以逐步记）
		TMap<FString, FColumnStats> Stats;
	};

	struct FScenarioResult
	{
		FScenario Scenario;
		bool bSpawned = false;
		FString Note;
		bool bNanitePath = false;
		FString Mismatch;
		int32 Components = 0;
		int64 Capacity = 0;
		int64 LiveInstances = 0;
		int32 BaseVertices = 0;
		int32 BaseTriangles = 0;
		int32 LODs = 0;
		int32 Sections = 0;
		int64 ResidentBytes = 0;      // 常驻流（基础网格 + 可见槽 + 源行 …）按描述符算的字节数
		int64 SourceBytes = 0;        // GPU 源自己那批 buffer（生产者的，不在常驻流里）
		int64 AdapterUsedDelta = 0;   // 适配器实测：起场景前后的占用差
		double FieldHalfExtent = 0.0;
		double WarmupSeconds = 0.0;
		TArray<FMeasurement> Measurements;
	};

	struct FCpuOpResult
	{
		FString Route;
		int32 NumInstances = 0;
		FString Op;
		int32 Calls = 0;
		double MeanMs = 0.0;
		double MaxMs = 0.0;
		double FlushesPerCall = 0.0;
	};

	// -------------------------------------------------------------------------
	// 套件
	// -------------------------------------------------------------------------

	FScenario MakeScenario(const TCHAR* Name, ERoute Route, ESource Source, int32 NumInstances)
	{
		FScenario S;
		S.Name = Name;
		S.Route = Route;
		S.Source = Source;
		S.NumInstances = NumInstances;
		S.bNaniteMesh = Route == ERoute::Nanite;
		return S;
	}

	/**
	 * 代表性组合，不是全排列（§9.1：先用代表性组合定位瓶颈，再往最相关的边界上加）。
	 * 加场景请追加在后面并保持既有名字不变 —— 结果是按名字跨次比较的。
	 */
	TArray<FScenario> BuildSuite(const FString& Suite)
	{
		TArray<FScenario> Out;

		// 数量阶梯：经典路 + CPU 数组（两级剔除都跑）。
		Out.Add(MakeScenario(TEXT("classic_cpu_10k"), ERoute::Classic, ESource::Cpu, 10000));
		Out.Add(MakeScenario(TEXT("classic_cpu_100k"), ERoute::Classic, ESource::Cpu, 100000));
		Out.Add(MakeScenario(TEXT("classic_cpu_1m"), ERoute::Classic, ESource::Cpu, 1000000));

		// 同一批实例换渲染路：Nanite 替身、引擎原生 ISM / HISM（非 Nanite 网格）、引擎 ISM + Nanite 网格。
		Out.Add(MakeScenario(TEXT("nanite_cpu_100k"), ERoute::Nanite, ESource::Cpu, 100000));
		Out.Add(MakeScenario(TEXT("ism_100k"), ERoute::ISM, ESource::Cpu, 100000));
		Out.Add(MakeScenario(TEXT("hism_100k"), ERoute::HISM, ESource::Cpu, 100000));
		{
			FScenario S = MakeScenario(TEXT("ism_nanitemesh_100k"), ERoute::ISM, ESource::Cpu, 100000);
			S.bNaniteMesh = true;
			Out.Add(S);
		}
		Out.Add(MakeScenario(TEXT("nanite_cpu_1m"), ERoute::Nanite, ESource::Cpu, 1000000));
		Out.Add(MakeScenario(TEXT("ism_1m"), ERoute::ISM, ESource::Cpu, 1000000));

		// GPU 源：容量 = 活跃数，与容量 ≫ 活跃数（无簇表、按容量派发、Nanite 路每帧整份重写）。
		Out.Add(MakeScenario(TEXT("classic_gpu_100k"), ERoute::Classic, ESource::GpuPacked, 100000));
		{
			FScenario S = MakeScenario(TEXT("classic_gpu_100k_cap8"), ERoute::Classic, ESource::GpuPacked, 100000);
			S.CapacityFactor = 8;
			Out.Add(S);
		}
		Out.Add(MakeScenario(TEXT("classic_gpu_1m"), ERoute::Classic, ESource::GpuPacked, 1000000));
		Out.Add(MakeScenario(TEXT("nanite_gpu_100k"), ERoute::Nanite, ESource::GpuPacked, 100000));
		{
			FScenario S = MakeScenario(TEXT("nanite_gpu_100k_cap8"), ERoute::Nanite, ESource::GpuPacked, 100000);
			S.CapacityFactor = 8;
			Out.Add(S);
		}
		Out.Add(MakeScenario(TEXT("nanite_gpu_1m"), ERoute::Nanite, ESource::GpuPacked, 1000000));

		// 点云源：剔除 / 写入前每帧先打包一遍。
		Out.Add(MakeScenario(TEXT("classic_points_100k"), ERoute::Classic, ESource::GpuPoints, 100000));
		Out.Add(MakeScenario(TEXT("nanite_points_100k"), ERoute::Nanite, ESource::GpuPoints, 100000));

		// 距离带（地被的真实形状）：容量 100 万、只画 60 m 以内，过渡带 20 m。
		{
			FScenario S = MakeScenario(TEXT("classic_gpu_1m_band60m"), ERoute::Classic, ESource::GpuPacked, 1000000);
			S.EndCullDistance = 6000.0f;
			S.CullFadeDistance = 2000.0f;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("nanite_gpu_1m_band60m"), ERoute::Nanite, ESource::GpuPacked, 1000000);
			S.EndCullDistance = 6000.0f;
			S.CullFadeDistance = 2000.0f;
			Out.Add(S);
		}

		// LOD：4 级（可见缓冲 = 容量 × 4 级）；高顶点数（顶点阶段的取数成本在这里才看得见）。
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_100k_lod4"), ERoute::Classic, ESource::Cpu, 100000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 4;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_1m_lod4"), ERoute::Classic, ESource::Cpu, 1000000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 4;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_10k_highpoly"), ERoute::Classic, ESource::Cpu, 10000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 1;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("ism_10k_highpoly"), ERoute::ISM, ESource::Cpu, 10000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 1;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("hism_100k_lod4"), ERoute::HISM, ESource::Cpu, 100000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 4;
			Out.Add(S);
		}

		// 多材质段：同一级 LOD 的各段共用剔除结果，只多 draw。
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_100k_2sections"), ERoute::Classic, ESource::Cpu, 100000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 2;
			S.NumSections = 2;
			Out.Add(S);
		}

		// 很多小组件：每个代理各提交一套 pass。
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_100k_256comps"), ERoute::Classic, ESource::Cpu, 100000);
			S.NumComponents = 256;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("ism_100k_256comps"), ERoute::ISM, ESource::Cpu, 100000);
			S.NumComponents = 256;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("nanite_cpu_100k_256comps"), ERoute::Nanite, ESource::Cpu, 100000);
			S.NumComponents = 256;
			Out.Add(S);
		}

		// 阴影关：与 classic_cpu_100k 成对，差值就是这条路的阴影成本。
		{
			FScenario S = MakeScenario(TEXT("classic_cpu_100k_noshadow"), ERoute::Classic, ESource::Cpu, 100000);
			S.bCastShadow = false;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("nanite_cpu_100k_noshadow"), ERoute::Nanite, ESource::Cpu, 100000);
			S.bCastShadow = false;
			Out.Add(S);
		}

		// §7：承诺了通知的 GPU 源（数据静止 ⇒ Nanite 路不再每帧重写 GPU-Scene）。与同名不带 _notify 的场景成对。
		for (const int32 Count : { 100000, 1000000 })
		{
			FScenario S = MakeScenario(Count == 100000 ? TEXT("nanite_gpu_100k_notify") : TEXT("nanite_gpu_1m_notify"), ERoute::Nanite, ESource::GpuPacked, Count);
			S.bNotifiesChanges = true;
			Out.Add(S);
		}
		{
			FScenario S = MakeScenario(TEXT("nanite_gpu_1m_band60m_notify"), ERoute::Nanite, ESource::GpuPacked, 1000000);
			S.EndCullDistance = 6000.0f;
			S.CullFadeDistance = 2000.0f;
			S.bNotifiesChanges = true;
			Out.Add(S);
		}

		if (Suite == TEXT("smoke"))
		{
			// 冒烟：每条路一个小场景，只为验证这套工具自己。
			Out.Reset();
			Out.Add(MakeScenario(TEXT("classic_cpu_10k"), ERoute::Classic, ESource::Cpu, 10000));
			Out.Add(MakeScenario(TEXT("nanite_gpu_10k"), ERoute::Nanite, ESource::GpuPacked, 10000));
			Out.Add(MakeScenario(TEXT("classic_points_10k"), ERoute::Classic, ESource::GpuPoints, 10000));
			Out.Add(MakeScenario(TEXT("ism_10k"), ERoute::ISM, ESource::Cpu, 10000));
			FScenario S = MakeScenario(TEXT("classic_cpu_10k_lod4"), ERoute::Classic, ESource::Cpu, 10000);
			S.Mesh = EMeshKind::Sphere;
			S.NumLODs = 4;
			S.NumSections = 2;
			Out.Add(S);
		}
		return Out;
	}

	// -------------------------------------------------------------------------
	// 测试网格
	// -------------------------------------------------------------------------

	/** UV 球的一级 LOD。三角绕序按引擎口径（见 UCSGpuMeshComponent::BuildGpuMeshDescription：几何法线 = (P2−P0)×(P1−P0)）朝外。 */
	FCSGpuMeshCPUData MakeSphereData(int32 Segments, int32 Rings, float Radius, int32 NumSections)
	{
		FCSGpuMeshCPUData Data;
		Data.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
		Data.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;

		for (int32 Ring = 0; Ring <= Rings; ++Ring)
		{
			const float Phi = UE_PI * float(Ring) / float(Rings);
			for (int32 Segment = 0; Segment <= Segments; ++Segment)
			{
				const float Theta = 2.0f * UE_PI * float(Segment) / float(Segments);
				const FVector3f Normal(FMath::Sin(Phi) * FMath::Cos(Theta), FMath::Sin(Phi) * FMath::Sin(Theta), FMath::Cos(Phi));
				Data.Positions.Add(Normal * Radius);
				Data.Normals.Add(Normal);
				Data.Tangents.Add(FVector3f(-FMath::Sin(Theta), FMath::Cos(Theta), 0.0f));
				Data.TexCoords().Add(FVector2f(float(Segment) / float(Segments), float(Ring) / float(Rings)));
			}
		}

		auto AddTriangle = [&Data, NumSections](uint32 A, uint32 B, uint32 C)
		{
			const FVector3f P0 = Data.Positions[int32(A)];
			const FVector3f P1 = Data.Positions[int32(B)];
			const FVector3f P2 = Data.Positions[int32(C)];
			const FVector3f Face = FVector3f::CrossProduct(P2 - P0, P1 - P0);
			if (Face.SizeSquared() < 1e-6f) return;   // 极点处的退化三角
			const FVector3f Centroid = (P0 + P1 + P2) / 3.0f;
			if (FVector3f::DotProduct(Face, Centroid) < 0.0f) Swap(B, C);
			Data.Indices.Add(A);
			Data.Indices.Add(B);
			Data.Indices.Add(C);
			Data.TriangleMaterialSlots.Add((NumSections > 1 && Centroid.Z < 0.0f) ? 1 : 0);
		};

		const uint32 Stride = uint32(Segments + 1);
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			for (int32 Segment = 0; Segment < Segments; ++Segment)
			{
				const uint32 A = uint32(Ring) * Stride + uint32(Segment);
				const uint32 B = A + 1u;
				const uint32 C = A + Stride;
				const uint32 D = C + 1u;
				AddTriangle(A, C, B);
				AddTriangle(B, C, D);
			}
		}
		return Data;
	}

	/** 程序化的多级 LOD 球（直径 100，与引擎 Cube 同一个包围尺度）。fast build：各级 LOD 就是给进去的那几份几何。 */
	UStaticMesh* MakeSphereMesh(int32 NumLODs, int32 NumSections)
	{
		NumLODs = FMath::Clamp(NumLODs, 1, CS_GPU_INSTANCED_MAX_LODS);
		NumSections = FMath::Clamp(NumSections, 1, 2);

		TArray<FMeshDescription> Descriptions;
		Descriptions.SetNum(NumLODs);
		for (int32 Lod = 0; Lod < NumLODs; ++Lod)
		{
			const int32 Segments = FMath::Max(8, 64 >> Lod);
			const FCSGpuMeshCPUData Data = MakeSphereData(Segments, Segments / 2, 50.0f, NumSections);
			if (!UCSGpuMeshComponent::BuildGpuMeshDescription(Data, FTransform::Identity, false, Descriptions[Lod])) return nullptr;
		}

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSBenchSphere")), RF_Transient);
		Mesh->bAllowCPUAccess = true;
		UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
		for (int32 Slot = 0; Slot < NumSections; ++Slot)
		{
			const FName SlotName = CSStaticMeshAsset::MaterialSlotName(Slot);
			Mesh->GetStaticMaterials().Add(FStaticMaterial(DefaultMaterial, SlotName, SlotName));
		}

		TArray<const FMeshDescription*> Pointers;
		for (const FMeshDescription& Description : Descriptions) Pointers.Add(&Description);

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bCommitMeshDescription = false;
		Params.bBuildSimpleCollision = false;
		Params.bAllowCpuAccess = true;
		if (!Mesh->BuildFromMeshDescriptions(Pointers, Params)) return nullptr;

		// fast build 把阈值定成 0.75^LOD，实例只有几十厘米大，那样永远停在最后一级。按 30 cm 实例在 10 / 30 / 90 m 换级定。
		if (FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
		{
			const float Thresholds[CS_GPU_INSTANCED_MAX_LODS] = { 1.0f, 0.05f, 0.015f, 0.005f };
			for (int32 Lod = 0; Lod < NumLODs; ++Lod) RenderData->ScreenSize[Lod].Default = Thresholds[Lod];
		}
		return Mesh;
	}

	/** 引擎 Cube 复制一份、开 Nanite、同步构建（与 GpuInstancedMesh.NaniteMode 那条测试同一个做法）。 */
	UStaticMesh* MakeNaniteCube()
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		if (!Cube) return nullptr;

		UStaticMesh* Mesh = DuplicateObject<UStaticMesh>(Cube, GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSBenchNaniteCube")));
		FMeshNaniteSettings Settings = Mesh->GetNaniteSettings();
		Settings.bEnabled = true;
		Mesh->SetNaniteSettings(Settings);
		TArray<FText> Errors;
		Mesh->Build(/*bInSilent*/ true, &Errors);
		return Mesh->HasValidNaniteData() ? Mesh : nullptr;
	}

	// -------------------------------------------------------------------------
	// 实例
	// -------------------------------------------------------------------------

	constexpr double InstanceSpacing = 100.0;

	/** √N × √N 的抖动网格，固定种子。返回场地半边长。 */
	double MakeTransforms(int32 NumInstances, TArray<FTransform>& OutTransforms)
	{
		const int32 Side = FMath::CeilToInt(FMath::Sqrt(double(NumInstances)));
		const double Half = double(Side) * InstanceSpacing * 0.5;

		FRandomStream Random(20261002);
		OutTransforms.Reset(NumInstances);
		for (int32 Index = 0; Index < NumInstances; ++Index)
		{
			const int32 X = Index % Side;
			const int32 Y = Index / Side;
			const double Scale = Random.FRandRange(0.25f, 0.4f);
			const FVector Location(
				(double(X) + 0.5 + Random.FRandRange(-0.3f, 0.3f)) * InstanceSpacing - Half,
				(double(Y) + 0.5 + Random.FRandRange(-0.3f, 0.3f)) * InstanceSpacing - Half,
				50.0 * Scale);
			OutTransforms.Add(FTransform(FRotator(0.0, Random.FRandRange(0.0f, 360.0f), 0.0), Location, FVector(Scale)));
		}
		return Half;
	}

	/** 按组件的打包约定（FCSGpuInstanceSourceGPU）把一个变换打成 5 float4。球半径照经典路 CPU 打包的口径算。 */
	void AppendPackedRow(TArray<FVector4f>& Rows, const FTransform& Transform, float Random, const FVector& BaseCentre, float BaseRadius)
	{
		const FMatrix44f M = FMatrix44f(Transform.ToMatrixWithScale());
		Rows.Add(FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f));
		Rows.Add(FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f));
		Rows.Add(FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f));
		Rows.Add(FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], Random));
		const FVector AbsScale = Transform.GetScale3D().GetAbs();
		Rows.Add(FVector4f(FVector3f(Transform.TransformPosition(BaseCentre)), BaseRadius * float(FMath::Max3(AbsScale.X, AbsScale.Y, AbsScale.Z))));
	}

	/** 像生产者那样在 GPU 上备一份实例源：行 + 计数器，停在 SRVMask。容量可以大于活跃数 —— 多出来的行不初始化：
	 *  剔除与 GPU-Scene 写入都止步于计数器，从不读它们（真的生产者也是这样留着上一代的行）。 */
	FCSGpuInstanceSourceGPU MakeGpuSource(const TArray<FVector4f>& LiveRows, uint32 Capacity, const FBox& LocalBounds)
	{
		const uint32 LiveCount = uint32(LiveRows.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
		Capacity = FMath::Max(Capacity, LiveCount);

		FCSGpuInstanceSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSBenchMakeGpuSource)([&Source, &LiveRows, LiveCount, Capacity](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RowsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSBench.Rows"));
			GraphBuilder.QueueBufferUpload(RowsBuffer, LiveRows.GetData(), LiveRows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSBench.Count"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.PackedInstances = GraphBuilder.ConvertToExternalBuffer(RowsBuffer);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(RowsBuffer, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = Capacity;
		Source.LocalBounds = LocalBounds;
		return Source;
	}

	/** 点刷那样的点云源：世界空间位置 + 朝上的法线。 */
	FCSGpuInstancePointSourceGPU MakePointSource(const TArray<FTransform>& Transforms, uint32 Capacity, float InstanceScale)
	{
		const uint32 LiveCount = uint32(Transforms.Num());
		Capacity = FMath::Max(Capacity, LiveCount);

		TArray<FVector4f> Positions;
		TArray<FVector4f> Normals;
		Positions.Init(FVector4f(0.0f, 0.0f, 0.0f, 1.0f), int32(Capacity));
		Normals.Init(FVector4f(0.0f, 0.0f, 1.0f, 0.0f), int32(Capacity));
		FBox WorldBounds(ForceInit);
		for (int32 Index = 0; Index < Transforms.Num(); ++Index)
		{
			Positions[Index] = FVector4f(FVector3f(Transforms[Index].GetLocation()), 1.0f);
			WorldBounds += Transforms[Index].GetLocation();
		}

		FCSGpuInstancePointSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSBenchMakePointSource)([&Source, &Positions, &Normals, LiveCount](FRHICommandListImmediate& RHICmdList)
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
			Source.Positions = Upload(Positions, TEXT("CSBench.PointPositions"));
			Source.Normals = Upload(Normals, TEXT("CSBench.PointNormals"));
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSBench.PointCount"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = Capacity;
		Source.InstanceScale = InstanceScale;
		Source.WorldBounds = WorldBounds;
		return Source;
	}

	// -------------------------------------------------------------------------
	// CSV → 统计
	// -------------------------------------------------------------------------

	/** 只留整帧那几列、GPU/ 分段与本插件自己的分类；其余几百列（各线程的命名事件）不进 summary，原始 CSV 里都在。 */
	bool IsInterestingColumn(const FString& Name)
	{
		return !Name.Contains(TEXT("/")) || Name.StartsWith(TEXT("GPU/")) || Name.StartsWith(TEXT("CSGpuInstanced/")) || Name.StartsWith(TEXT("DrawCall/"));
	}

	/**
	 * CsvProfile 的文件：首行与末尾各一行表头（`EVENTS,列名…`，末尾那行才是全的 —— 采集中途新出现的统计只补在后面），
	 * 中间每帧一行，最后一行是 `[键],值` 的元数据。丢掉每次采集的前 SkipFrames 帧（GPU 分段统计是帧末才到的）。
	 */
	bool ParseCsv(const FString& File, int32 SkipFrames, TMap<FString, FColumnStats>& OutStats, int32& OutFrames)
	{
		OutStats.Reset();
		OutFrames = 0;

		TArray<FString> Lines;
		if (!FFileHelper::LoadFileToStringArray(Lines, *File)) return false;

		int32 HeaderLine = INDEX_NONE;
		for (int32 Index = Lines.Num() - 1; Index >= 0; --Index)
		{
			if (Lines[Index].StartsWith(TEXT("EVENTS,")) || Lines[Index] == TEXT("EVENTS"))
			{
				HeaderLine = Index;
				break;
			}
		}
		if (HeaderLine == INDEX_NONE) return false;

		TArray<FString> Header;
		Lines[HeaderLine].ParseIntoArray(Header, TEXT(","), /*bCullEmpty*/ false);

		TArray<TArray<double>> Columns;
		Columns.SetNum(Header.Num());

		int32 Frame = 0;
		for (int32 Index = 0; Index < HeaderLine; ++Index)
		{
			const FString& Line = Lines[Index];
			if (Line.IsEmpty() || Line.StartsWith(TEXT("EVENTS")) || Line.StartsWith(TEXT("["))) continue;
			if (Frame++ < SkipFrames) continue;

			TArray<FString> Cells;
			Line.ParseIntoArray(Cells, TEXT(","), /*bCullEmpty*/ false);
			// 第 0 列是事件文本，不是数。
			for (int32 Column = 1; Column < Cells.Num() && Column < Columns.Num(); ++Column) Columns[Column].Add(FCString::Atod(*Cells[Column]));
			++OutFrames;
		}

		for (int32 Column = 1; Column < Header.Num(); ++Column)
		{
			TArray<double>& Values = Columns[Column];
			if (Values.IsEmpty() || !IsInterestingColumn(Header[Column])) continue;

			double Sum = 0.0;
			for (const double Value : Values) Sum += Value;
			Values.Sort();

			FColumnStats& Stats = OutStats.Add(Header[Column]);
			Stats.Samples = Values.Num();
			Stats.Mean = Sum / double(Values.Num());
			Stats.Median = Values[Values.Num() / 2];
			Stats.P95 = Values[FMath::Min(Values.Num() - 1, int32(double(Values.Num()) * 0.95))];
		}
		return true;
	}

	// -------------------------------------------------------------------------
	// JSON（手写：本模块不依赖 Json，为一份报表加一条模块依赖不划算）
	// -------------------------------------------------------------------------

	FString JsonString(const FString& Value)
	{
		FString Escaped = Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
		Escaped.ReplaceInline(TEXT("\r"), TEXT(" "));
		Escaped.ReplaceInline(TEXT("\n"), TEXT(" "));
		return FString::Printf(TEXT("\"%s\""), *Escaped);
	}

	FString JsonNumber(double Value)
	{
		if (!FMath::IsFinite(Value)) return TEXT("null");
		return FString::Printf(TEXT("%.6g"), Value);
	}

	// -------------------------------------------------------------------------
	// 跑测本体
	// -------------------------------------------------------------------------

	class FRunner
	{
	public:
		static FRunner& Get()
		{
			static FRunner Runner;
			return Runner;
		}

		bool IsRunning() const { return Phase != EPhase::Idle; }

		void Start(UWorld* InWorld, const TArray<FString>& Args)
		{
			if (IsRunning())
			{
				UE_LOG(LogCSGpuInstancedBench, Warning, TEXT("已经在跑了。"));
				return;
			}
			if (!InWorld || !InWorld->IsGameWorld())
			{
				UE_LOG(LogCSGpuInstancedBench, Error, TEXT("要在 -game 进程里跑（整帧口径要一个真的游戏视口），见文件头的用法。"));
				return;
			}

			World = InWorld;
			Suite = TEXT("baseline");
			OutDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("InstanceCullOpt"), TEXT("Bench"), FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
			MeasureFrames = 200;
			SettleFrames = 30;
			WarmupFrames = 90;
			Pairs = 1;
			bExitWhenDone = true;
			bRunCpuOps = true;
			StartDelaySeconds = 0.0;
			PoseOrigin = FVector::ZeroVector;
			bHasLookAt = false;
			LookDistance = 2000.0;
			Filter.Reset();
			Variants.Reset();
			FString Resolution = TEXT("1920x1080");

			for (const FString& Arg : Args)
			{
				FString Key, Value;
				if (!Arg.Split(TEXT("="), &Key, &Value)) continue;
				if (Key == TEXT("suite")) Suite = Value;
				else if (Key == TEXT("out")) OutDir = Value;
				else if (Key == TEXT("frames")) MeasureFrames = FMath::Max(FCString::Atoi(*Value), 20);
				else if (Key == TEXT("settle")) SettleFrames = FMath::Max(FCString::Atoi(*Value), 5);
				else if (Key == TEXT("warmup")) WarmupFrames = FMath::Max(FCString::Atoi(*Value), 10);
				else if (Key == TEXT("pairs")) Pairs = FMath::Max(FCString::Atoi(*Value), 1);
				else if (Key == TEXT("exit")) bExitWhenDone = FCString::Atoi(*Value) != 0;
				else if (Key == TEXT("cpuops")) bRunCpuOps = FCString::Atoi(*Value) != 0;
				else if (Key == TEXT("res")) Resolution = Value;
				else if (Key == TEXT("delay")) StartDelaySeconds = FMath::Max(FCString::Atod(*Value), 0.0);
				else if (Key == TEXT("dist")) LookDistance = FMath::Max(FCString::Atod(*Value), 100.0);
				else if (Key == TEXT("look"))
				{
					// 分量用 `/` 隔开：-ExecCmds 拿逗号分命令，写逗号的话这条命令到第一个逗号就断了。
					TArray<FString> Parts;
					Value.ParseIntoArray(Parts, TEXT("/"), true);
					if (Parts.Num() == 3)
					{
						LookAt = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
						bHasLookAt = true;
					}
				}
				else if (Key == TEXT("filter")) Value.ParseIntoArray(Filter, TEXT("+"), true);
				else if (Key == TEXT("variants")) ParseVariants(Value);
			}
			if (Variants.IsEmpty()) Variants.AddDefaulted_GetRef().Name = TEXT("base");
			FPaths::NormalizeDirectoryName(OutDir);
			IFileManager::Get().MakeDirectory(*OutDir, /*Tree*/ true);

			if (Suite == TEXT("map"))
			{
				// 一个场景：这张地图自己。名字取地图名，结果按名字跨次比较。
				Scenarios.Reset();
				FScenario& MapScenario = Scenarios.Add_GetRef(MakeScenario(*FString::Printf(TEXT("map_%s"), *InWorld->GetMapName()), ERoute::Map, ESource::Cpu, 0));
				MapScenario.bNaniteMesh = false;
				bRunCpuOps = false;
			}
			else
			{
				Scenarios = BuildSuite(Suite);
			}
			if (!Filter.IsEmpty())
			{
				Scenarios.RemoveAll([this](const FScenario& Scenario)
				{
					// 记号按子串匹配；以 `!` 结尾的要求整个名字相同（`ism_100k!` 不会把 hism_100k / ism_100k_256comps 也带进来）。
					for (const FString& Token : Filter)
					{
						if (Token.EndsWith(TEXT("!")) ? Scenario.Name == Token.LeftChop(1) : Scenario.Name.Contains(Token)) return false;
					}
					return true;
				});
			}
			Results.Reset();
			CpuOps.Reset();
			ScenarioIndex = 0;

			// 整帧口径：不限帧、不等垂直同步、不平滑帧率；GPU 分段统计进 CSV。
			// 分辨率在这里钉死：-ResX/-ResY 会被桌面大小钳住（远程桌面的虚拟显示器常常很小），读数就不可比了。
			ExecAll({ TEXT("t.MaxFPS 0"), TEXT("r.VSync 0"), TEXT("r.GPUCsvStatsEnabled 1"), TEXT("r.ScreenPercentage 100"),
				FString::Printf(TEXT("r.SetRes %sw"), *Resolution) });
			if (GEngine)
			{
				GEngine->bSmoothFrameRate = false;
				GEngine->bUseFixedFrameRate = false;
			}

			UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench] suite=%s scenarios=%d variants=%d pairs=%d frames=%d out=%s"),
				*Suite, Scenarios.Num(), Variants.Num(), Pairs, MeasureFrames, *OutDir);

			Phase = EPhase::BeginScenario;
			StartAtSeconds = FPlatformTime::Seconds() + StartDelaySeconds;
			TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FRunner::Tick));
		}

	private:
		enum class EPhase : uint8 { Idle, BeginScenario, Warmup, BeginPose, Settle, Capturing, WaitCsv, AfterShot, Teardown, CoolDown, Finish };

		void ParseVariants(const FString& Spec)
		{
			TArray<FString> Entries;
			// 不用 `|`：控制台把它当命令分隔符。
			Spec.ParseIntoArray(Entries, TEXT("~"), true);
			for (const FString& Entry : Entries)
			{
				FVariant& Variant = Variants.AddDefaulted_GetRef();
				FString Commands;
				if (!Entry.Split(TEXT(":"), &Variant.Name, &Commands)) Variant.Name = Entry;
				Commands.ParseIntoArray(Variant.Commands, TEXT(";"), true);
				// 控制台命令的参数按空白切，变体里的命令没法带空格 —— 约定用 `@` 代替。
				for (FString& Command : Variant.Commands) Command.ReplaceInline(TEXT("@"), TEXT(" "));
			}
		}

		void ExecAll(const TArray<FString>& Commands)
		{
			if (!GEngine) return;
			for (const FString& Command : Commands) GEngine->Exec(World.Get(), *Command);
		}

		// ---- 一步 ----

		bool Tick(float)
		{
			if (!World.IsValid())
			{
				UE_LOG(LogCSGpuInstancedBench, Error, TEXT("[Bench] world 没了，中止。"));
				Phase = EPhase::Idle;
				return false;
			}

			// `delay=`：地图上的生产者（房子、地被……）要几帧到几秒才把实例建完，建完之前认领不到它们的组件。
			if (FPlatformTime::Seconds() < StartAtSeconds) return true;

			switch (Phase)
			{
			case EPhase::BeginScenario: TickBeginScenario(); break;
			case EPhase::Warmup: TickWarmup(); break;
			case EPhase::BeginPose: TickBeginPose(); break;
			case EPhase::Settle: TickSettle(); break;
			case EPhase::Capturing: TickCapturing(); break;
			case EPhase::WaitCsv: TickWaitCsv(); break;
			case EPhase::AfterShot: if (++FrameCounter >= 3) AdvanceStep(); break;
			case EPhase::Teardown: TickTeardown(); break;
			case EPhase::CoolDown: if (++FrameCounter >= 20) { ++ScenarioIndex; Phase = EPhase::BeginScenario; } break;
			case EPhase::Finish: TickFinish(); return false;
			default: return false;
			}
			return true;
		}

		void TickBeginScenario()
		{
			if (!Scenarios.IsValidIndex(ScenarioIndex))
			{
				Phase = EPhase::Finish;
				return;
			}

			FScenarioResult& Result = Results.AddDefaulted_GetRef();
			Result.Scenario = Scenarios[ScenarioIndex];
			UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench] (%d/%d) %s"), ScenarioIndex + 1, Scenarios.Num(), *Result.Scenario.Name);

			AdapterAvailableBefore = CSGpuMemoryBudget::QueryMemorySnapshot().AvailableVideoMemory;
			Result.bSpawned = SpawnScenario(Result);
			if (!Result.bSpawned)
			{
				UE_LOG(LogCSGpuInstancedBench, Warning, TEXT("[Bench] %s 起不来：%s"), *Result.Scenario.Name, *Result.Note);
				Phase = EPhase::Teardown;
				return;
			}

			PhaseStartSeconds = FPlatformTime::Seconds();
			FrameCounter = 0;
			PoseIndex = 0;
			StepIndex = 0;
			ApplyPose(Poses[0], 0);
			Phase = EPhase::Warmup;
		}

		void TickWarmup()
		{
			++FrameCounter;
			const double Elapsed = FPlatformTime::Seconds() - PhaseStartSeconds;

			bool bBusy = (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) || FAssetCompilingManager::Get().GetNumRemainingAssets() > 0;
			for (const TWeakObjectPtr<UInstancedStaticMeshComponent>& Component : IsmComponents)
			{
				const UHierarchicalInstancedStaticMeshComponent* Hism = Cast<UHierarchicalInstancedStaticMeshComponent>(Component.Get());
				if (Hism && (Hism->IsAsyncBuilding() || !Hism->IsTreeFullyBuilt())) bBusy = true;
			}
			// 帧数与时间都要够：轻场景 200+ fps，90 帧还不到半秒，VSM 缓存 / Lumen / Nanite 流送都没稳。
			if ((FrameCounter < WarmupFrames || Elapsed < 2.0 || bBusy) && Elapsed < 120.0) return;

			FScenarioResult& Result = Results.Last();
			Result.WarmupSeconds = Elapsed;
			CollectStaticInfo(Result);
			Phase = EPhase::BeginPose;
		}

		/** 一个机位下的一"步" = (第几对, 这一对里第几个变体)。奇数对倒着量：A B / B A / A B …（交错）。 */
		int32 StepsPerPose() const { return Pairs * Variants.Num(); }

		int32 VariantOfStep(int32 Step) const
		{
			const int32 Pair = Step / Variants.Num();
			const int32 Slot = Step % Variants.Num();
			return (Pair % 2 == 0) ? Slot : (Variants.Num() - 1 - Slot);
		}

		void TickBeginPose()
		{
			const FVariant& Variant = Variants[VariantOfStep(StepIndex)];
			if (Variants.Num() > 1 || !Variant.Commands.IsEmpty())
			{
				ExecAll(Variant.Commands);
				// 在代理构造时读 CVar 的开关要重建渲染状态才生效；Nanite 替身跟着本体的下一次 Tick 走。
				for (const TWeakObjectPtr<UCSGpuInstancedMeshComponent>& Component : ClassicComponents)
				{
					if (Component.IsValid()) Component->MarkRenderStateDirty();
				}
			}
			FrameCounter = 0;
			PhaseStartSeconds = FPlatformTime::Seconds();
			ApplyPose(Poses[PoseIndex], 0);
			Phase = EPhase::Settle;
		}

		void TickSettle()
		{
			ApplyPose(Poses[PoseIndex], FrameCounter);
			if (++FrameCounter < SettleFrames || FPlatformTime::Seconds() - PhaseStartSeconds < 0.4) return;

			const FScenarioResult& Result = Results.Last();
			CurrentCsvName = FString::Printf(TEXT("%s__%s__%s__p%d.csv"), *Result.Scenario.Name, *Poses[PoseIndex].Name,
				*Variants[VariantOfStep(StepIndex)].Name, StepIndex / Variants.Num());
			FCsvProfiler::Get()->BeginCapture(-1, OutDir, CurrentCsvName);
			OrbitFrameBase = FrameCounter;
			FrameCounter = 0;
			PhaseStartSeconds = FPlatformTime::Seconds();
			Phase = EPhase::Capturing;
		}

		void TickCapturing()
		{
			ApplyPose(Poses[PoseIndex], OrbitFrameBase + FrameCounter);
			++FrameCounter;

			// 重场景（一帧几十毫秒）不必量满：至少 60 帧，超过 6 秒就收。
			const double Elapsed = FPlatformTime::Seconds() - PhaseStartSeconds;
			if (FrameCounter < MeasureFrames && !(FrameCounter >= 60 && Elapsed > 6.0)) return;

			CsvFuture = FCsvProfiler::Get()->EndCapture();
			FrameCounter = 0;
			Phase = EPhase::WaitCsv;
		}

		void TickWaitCsv()
		{
			if (!CsvFuture.IsValid() || !CsvFuture.IsReady())
			{
				if (++FrameCounter > 2000)
				{
					UE_LOG(LogCSGpuInstancedBench, Error, TEXT("[Bench] 等不到 CSV %s"), *CurrentCsvName);
					AdvanceStep();
				}
				return;
			}

			FScenarioResult& Result = Results.Last();
			FMeasurement& Measurement = Result.Measurements.AddDefaulted_GetRef();
			Measurement.Pose = Poses[PoseIndex].Name;
			Measurement.Variant = Variants[VariantOfStep(StepIndex)].Name;
			Measurement.Pair = StepIndex / Variants.Num();
			Measurement.CsvFile = CurrentCsvName;
			FString CsvPath = CsvFuture.Get();
			if (!FPaths::FileExists(CsvPath)) CsvPath = FPaths::Combine(OutDir, CurrentCsvName);
			if (!ParseCsv(CsvPath, /*SkipFrames*/ 5, Measurement.Stats, Measurement.Frames))
			{
				UE_LOG(LogCSGpuInstancedBench, Warning, TEXT("[Bench] 解析不了 %s"), *CsvPath);
			}

			// 经典路：上一帧剔除压出来的逐 LOD 可见数（各组件相加）。转圈机位的画面一直在变，这个数只对固定机位有意义。
			if (!Poses[PoseIndex].bOrbit)
			{
				TArray<int32> Total;
				bool bAnyRead = false;
				for (const TWeakObjectPtr<UCSGpuInstancedMeshComponent>& Component : ClassicComponents)
				{
					TArray<int32> PerLod;
					if (!Component.IsValid() || !Component->DebugReadVisibleCountsSync(PerLod)) continue;
					bAnyRead = true;
					if (Total.Num() < PerLod.Num()) Total.SetNumZeroed(PerLod.Num());
					for (int32 Lod = 0; Lod < PerLod.Num(); ++Lod) Total[Lod] += PerLod[Lod];
				}
				if (bAnyRead) Measurement.VisiblePerLod = MoveTemp(Total);
			}
			for (const TWeakObjectPtr<UCSGpuInstancedMeshComponent>& Component : ClassicComponents)
			{
				const UCSMesh* GpuMesh = Component.IsValid() ? Component->GetGpuMesh() : nullptr;
				const FCSMeshResidentRef Resident = GpuMesh ? GpuMesh->GetResident() : FCSMeshResidentRef();
				if (Resident.IsValid()) Measurement.ResidentBytes += Resident->GetAllocatedBytes();
			}

			const FColumnStats* FrameTime = Measurement.Stats.Find(TEXT("FrameTime"));
			const FColumnStats* GpuTime = Measurement.Stats.Find(TEXT("GPUTime"));
			UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench]   %-8s %-10s frames=%d FrameTime=%.3f GPUTime=%.3f"),
				*Measurement.Pose, *Measurement.Variant, Measurement.Frames,
				FrameTime ? FrameTime->Median : -1.0, GpuTime ? GpuTime->Median : -1.0);

			// 留图。只有一个变体：每个场景每个机位一张，是"这个场景确实画出来了"的证据。
			// 多个变体：固定机位的头两对里每一步都留（GpuInstancedBenchImageDiff.py 拿**同一个变体两次拍的差**当噪声底、
			// **同一对里两个变体的差**当信号 —— TSR / Lumen / 曝光都不会在两次量测之间停在同一个状态上，单比一对分不出来）。
			const bool bMultiVariant = Variants.Num() > 1;
			if (bMultiVariant ? (Measurement.Pair < 2 && !Poses[PoseIndex].bOrbit) : StepIndex == 0)
			{
				const FString ShotFile = FPaths::Combine(OutDir, bMultiVariant
					? FString::Printf(TEXT("%s__%s__%s__p%d.png"), *Result.Scenario.Name, *Poses[PoseIndex].Name, *Measurement.Variant, Measurement.Pair)
					: FString::Printf(TEXT("%s__%s.png"), *Result.Scenario.Name, *Poses[PoseIndex].Name));
				FScreenshotRequest::RequestScreenshot(ShotFile, /*bShowUI*/ false, /*bAddFilenameSuffix*/ false);
				FrameCounter = 0;
				Phase = EPhase::AfterShot;
				return;
			}
			AdvanceStep();
		}

		void AdvanceStep()
		{
			if (++StepIndex < StepsPerPose())
			{
				Phase = EPhase::BeginPose;
				return;
			}
			StepIndex = 0;
			if (++PoseIndex < Poses.Num())
			{
				Phase = EPhase::BeginPose;
				return;
			}
			Phase = EPhase::Teardown;
		}

		void TickTeardown()
		{
			for (const TWeakObjectPtr<AActor>& Actor : Actors)
			{
				if (Actor.IsValid()) Actor->Destroy();
			}
			Actors.Reset();
			ClassicComponents.Reset();
			IsmComponents.Reset();
			GpuSources.Reset();
			PointSources.Reset();
			Meshes.Reset();
			FlushRenderingCommands();
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
			FrameCounter = 0;
			Phase = EPhase::CoolDown;
		}

		void TickFinish()
		{
			if (bRunCpuOps) RunCpuOps();
			WriteSummary();
			UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench] 完成：%s"), *FPaths::Combine(OutDir, TEXT("summary.json")));
			Phase = EPhase::Idle;
			if (bExitWhenDone) FPlatformMisc::RequestExit(/*Force*/ false, TEXT("CSGpuInstanced.Bench"));
		}

		// ---- 场景 ----

		void ApplyPose(const FPose& Pose, int32 Frame)
		{
			if (!Camera.IsValid()) return;
			// 机位都是相对场地中心写的；合成场景的中心在原点，地图的在它那些实例组件的包围盒中心（PoseOrigin）。
			if (!Pose.bOrbit)
			{
				Camera->SetActorLocationAndRotation(PoseOrigin + Pose.Location, Pose.Rotation);
				return;
			}
			// 240 帧一圈。
			const double Angle = 2.0 * UE_DOUBLE_PI * double(Frame) / 240.0;
			const FVector Location(FMath::Cos(Angle) * Pose.OrbitRadius, FMath::Sin(Angle) * Pose.OrbitRadius, Pose.OrbitHeight);
			Camera->SetActorLocationAndRotation(PoseOrigin + Location, (FVector(0.0, 0.0, 0.0) - Location).Rotation());
		}

		void BuildPoses(double Half)
		{
			Poses.Reset();
			{
				// 俯视全场：全部实例都在视锥里。水平视角 90°、16:9 时竖直半角只有 ~29.4°（tan = 0.5625），
				// 所以高度要 ≥ Half / 0.5625 才装得下整块场地 —— 取 1.85 倍留一圈边。
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("top");
				Pose.Location = FVector(0.0, 0.0, Half * 1.85 + 200.0);
				Pose.Rotation = FRotator(-90.0, 0.0, 0.0);
			}
			{
				// 站在场边往里看：大半在视锥里，远近都有。
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("edge");
				Pose.Location = FVector(-Half - 300.0, 0.0, FMath::Max(300.0, Half * 0.08));
				Pose.Rotation = FRotator(-8.0, 0.0, 0.0);
			}
			{
				// 站在场心平视：约四分之一。
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("center");
				Pose.Location = FVector(0.0, 0.0, 180.0);
				Pose.Rotation = FRotator(0.0, 0.0, 0.0);
			}
			{
				// 站在场边往外看：几乎都在视锥外。
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("away");
				Pose.Location = FVector(-Half + 500.0, 0.0, 180.0);
				Pose.Rotation = FRotator(0.0, 180.0, 0.0);
			}
			{
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("orbit");
				Pose.bOrbit = true;
				Pose.OrbitRadius = Half * 0.6;
				Pose.OrbitHeight = Half * 0.15 + 300.0;
			}
		}

		/** 相机 actor（跨场景复用）并把玩家视角接上去。 */
		bool EnsureCamera(FScenarioResult& Result)
		{
			UWorld* WorldPtr = World.Get();
			if (!Camera.IsValid())
			{
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.ObjectFlags = RF_Transient;
				ACameraActor* CameraActor = WorldPtr->SpawnActor<ACameraActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
				if (!CameraActor)
				{
					Result.Note = TEXT("相机 actor 建不出来");
					return false;
				}
				CameraActor->GetCameraComponent()->SetFieldOfView(90.0f);
				CameraActor->GetCameraComponent()->bConstrainAspectRatio = false;
				Camera = CameraActor;
			}
			APlayerController* Controller = WorldPtr->GetFirstPlayerController();
			if (!Controller)
			{
				Result.Note = TEXT("没有 PlayerController（不是 -game 进程？）");
				return false;
			}
			Controller->SetViewTarget(Camera.Get());
			return true;
		}

		/** `suite=map`：不造任何东西，认领这张地图里已经注册的实例组件，机位绕着它们的包围盒摆。 */
		bool AdoptMap(FScenarioResult& Result)
		{
			UWorld* WorldPtr = World.Get();
			FBox Bounds(ForceInit);
			int32 NanitePath = 0;
			for (TObjectIterator<UCSGpuInstancedMeshComponent> It; It; ++It)
			{
				UCSGpuInstancedMeshComponent* Component = *It;
				if (!IsValid(Component) || Component->GetWorld() != WorldPtr || !Component->IsRegistered()) continue;
				// 没有实例源也没有实例的空壳（生产者还没交接 / 这一族这张图里没用上）不算。
				if (!Component->HasInstanceSourceGPU() && Component->GetInstanceCount() == 0) continue;
				ClassicComponents.Add(Component);
				Bounds += Component->Bounds.GetBox();
				NanitePath += Component->IsNaniteRenderPath() ? 1 : 0;
				const int32 Live = FMath::Max(Component->DebugReadDrawnInstanceCountSync(), 0);
				Result.LiveInstances += Live;
				// 逐个列出来：要给某一族单独摆机位（look=）时，位置从这里抄。
				UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench]   %s.%s：%d 个实例，%s，中心 (%.0f, %.0f, %.0f) 半径 %.0f"),
					*GetNameSafe(Component->GetOwner()), *Component->GetName(), Live,
					Component->IsNaniteRenderPath() ? (Component->IsClassicShadowCopyActive() ? TEXT("Nanite 路 + 经典影子副本") : TEXT("Nanite 路")) : TEXT("经典路"),
					Component->Bounds.Origin.X, Component->Bounds.Origin.Y, Component->Bounds.Origin.Z, Component->Bounds.SphereRadius);
			}
			if (ClassicComponents.IsEmpty() || !Bounds.IsValid)
			{
				Result.Note = TEXT("这张地图里没有带实例的 UCSGpuInstancedMeshComponent（生产者还没建完的话加大 delay=）");
				return false;
			}

			// 机位的"地面"取包围盒中心的高度：地被的盒子是按"地面 ± 最坏起伏"上下对称放大的，中心就是地面；
			// 取盒底的话 center / away 两个机位会埋进地里。
			PoseOrigin = Bounds.GetCenter();
			const double Half = FMath::Max3(Bounds.GetExtent().X, Bounds.GetExtent().Y, 500.0);
			Result.FieldHalfExtent = Half;
			BuildPoses(Half);
			if (bHasLookAt)
			{
				// `look=x/y/z` + `dist=厘米`：对着世界里的一个点再拍一个机位（斜上方 30° 俯视）。绝对坐标，不跟 PoseOrigin 走。
				FPose& Pose = Poses.AddDefaulted_GetRef();
				Pose.Name = TEXT("look");
				const FVector Eye = LookAt + FVector(-LookDistance * 0.866, 0.0, LookDistance * 0.5);
				Pose.Location = Eye - PoseOrigin;
				Pose.Rotation = (LookAt - Eye).Rotation();
			}
			Result.Components = ClassicComponents.Num();
			Result.Note = FString::Printf(TEXT("地图 %s：%d 个实例组件（其中 %d 个走 Nanite 路），包围盒中心 (%.0f, %.0f, %.0f) 半边 %.0f"),
				*WorldPtr->GetMapName(), ClassicComponents.Num(), NanitePath, PoseOrigin.X, PoseOrigin.Y, PoseOrigin.Z, Half);
			UE_LOG(LogCSGpuInstancedBench, Display, TEXT("[Bench] %s"), *Result.Note);
			return EnsureCamera(Result);
		}

		bool SpawnScenario(FScenarioResult& Result)
		{
			const FScenario& Scenario = Result.Scenario;
			UWorld* WorldPtr = World.Get();
			if (Scenario.Route == ERoute::Map) return AdoptMap(Result);
			PoseOrigin = FVector::ZeroVector;

			// --- 网格 ---
			UStaticMesh* Mesh = nullptr;
			if (Scenario.bNaniteMesh)
			{
				if (!UseNanite(GMaxRHIShaderPlatform))
				{
					Result.Note = TEXT("这台机器 / 这个 RHI 不支持 Nanite");
					return false;
				}
				Mesh = MakeNaniteCube();
			}
			else if (Scenario.Mesh == EMeshKind::Sphere)
			{
				Mesh = MakeSphereMesh(Scenario.NumLODs, Scenario.NumSections);
			}
			else
			{
				Mesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			}
			if (!Mesh)
			{
				Result.Note = TEXT("测试网格建不出来");
				return false;
			}
			Meshes.Add(TStrongObjectPtr<UStaticMesh>(Mesh));

			// --- 实例 ---
			TArray<FTransform> Transforms;
			const double Half = MakeTransforms(Scenario.NumInstances, Transforms);
			Result.FieldHalfExtent = Half;
			Result.LiveInstances = Transforms.Num();
			BuildPoses(Half);

			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = RF_Transient;

			// --- 灯、地面、相机 ---
			if (ADirectionalLight* Light = WorldPtr->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 1000.0), FRotator(-50.0, 30.0, 0.0), SpawnParameters))
			{
				Light->GetLightComponent()->SetMobility(EComponentMobility::Movable);
				Light->GetLightComponent()->SetIntensity(6.0f);
				Light->GetLightComponent()->SetCastShadows(true);
				Actors.Add(Light);
			}
			if (AStaticMeshActor* Ground = WorldPtr->SpawnActor<AStaticMeshActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters))
			{
				UStaticMeshComponent* GroundComponent = Ground->GetStaticMeshComponent();
				GroundComponent->SetMobility(EComponentMobility::Movable);
				GroundComponent->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
				const double GroundScale = (Half * 2.0 + 4000.0) / 100.0;
				Ground->SetActorScale3D(FVector(GroundScale, GroundScale, 1.0));
				Actors.Add(Ground);
			}
			if (!EnsureCamera(Result)) return false;

			// --- 宿主 actor ---
			AActor* Host = WorldPtr->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
			if (!Host)
			{
				Result.Note = TEXT("宿主 actor 建不出来");
				return false;
			}
			USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
			Host->SetRootComponent(Root);
			Root->RegisterComponent();
			Actors.Add(Host);

			// --- 按空间瓦片分给各组件 ---
			const int32 TilesPerSide = FMath::Max(1, FMath::RoundToInt(FMath::Sqrt(double(FMath::Max(Scenario.NumComponents, 1)))));
			TArray<TArray<FTransform>> Tiles;
			Tiles.SetNum(TilesPerSide * TilesPerSide);
			for (const FTransform& Transform : Transforms)
			{
				const FVector Location = Transform.GetLocation();
				const int32 TileX = FMath::Clamp(int32((Location.X + Half) / (Half * 2.0) * TilesPerSide), 0, TilesPerSide - 1);
				const int32 TileY = FMath::Clamp(int32((Location.Y + Half) / (Half * 2.0) * TilesPerSide), 0, TilesPerSide - 1);
				Tiles[TileY * TilesPerSide + TileX].Add(Transform);
			}

			UMaterialInterface* DefaultMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
			const FBox MeshBounds = Mesh->GetBoundingBox();
			const FVector BaseCentre = MeshBounds.GetCenter();
			const float BaseRadius = float(MeshBounds.GetExtent().Size());

			for (const TArray<FTransform>& Tile : Tiles)
			{
				if (Tile.IsEmpty()) continue;

				if (Scenario.Route == ERoute::ISM || Scenario.Route == ERoute::HISM)
				{
					UInstancedStaticMeshComponent* Component = Scenario.Route == ERoute::HISM
						? NewObject<UHierarchicalInstancedStaticMeshComponent>(Host, NAME_None, RF_Transient)
						: NewObject<UInstancedStaticMeshComponent>(Host, NAME_None, RF_Transient);
					Component->SetupAttachment(Root);
					Component->SetMobility(EComponentMobility::Movable);
					Component->SetStaticMesh(Mesh);
					for (int32 Slot = 0; Slot < Mesh->GetStaticMaterials().Num(); ++Slot) Component->SetMaterial(Slot, DefaultMaterial);
					Component->SetCastShadow(Scenario.bCastShadow);
					Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					if (Scenario.EndCullDistance > 0.0f) Component->SetCullDistances(int32(Scenario.StartCullDistance), int32(Scenario.EndCullDistance));
					Component->RegisterComponent();
					Component->AddInstances(Tile, /*bShouldReturnIndices*/ false, /*bWorldSpace*/ false);
					if (UHierarchicalInstancedStaticMeshComponent* Hism = Cast<UHierarchicalInstancedStaticMeshComponent>(Component)) Hism->BuildTreeIfOutdated(/*Async*/ true, /*ForceUpdate*/ false);
					IsmComponents.Add(Component);
					continue;
				}

				UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
				Component->SetupAttachment(Root);
				Component->InstanceMaterial = DefaultMaterial;
				Component->SetCastShadow(Scenario.bCastShadow);
				Component->InstanceStartCullDistance = Scenario.StartCullDistance;
				Component->InstanceEndCullDistance = Scenario.EndCullDistance;
				Component->InstanceCullFadeDistance = Scenario.CullFadeDistance;
				Component->RegisterComponent();
				Component->SetBaseMesh(Mesh);

				if (Scenario.Source == ESource::Cpu)
				{
					Component->SetInstances(Tile);
				}
				else if (Scenario.Source == ESource::GpuPacked)
				{
					TArray<FVector4f> Rows;
					Rows.Reserve(Tile.Num() * CS_GPU_INSTANCED_ROW_FLOAT4S);
					FRandomStream Random(77);
					FBox Bounds(ForceInit);
					for (const FTransform& Transform : Tile)
					{
						AppendPackedRow(Rows, Transform, Random.FRand(), BaseCentre, BaseRadius);
						Bounds += Transform.GetLocation();
					}
					const uint32 Capacity = uint32(Tile.Num()) * uint32(FMath::Max(Scenario.CapacityFactor, 1));
					FCSGpuInstanceSourceGPU Source = MakeGpuSource(Rows, Capacity, Bounds.ExpandBy(100.0));
					Source.bNotifiesChanges = Scenario.bNotifiesChanges;
					GpuSources.Add(Source);
					Result.SourceBytes += int64(Capacity) * CS_GPU_INSTANCED_ROW_FLOAT4S * int64(sizeof(FVector4f)) + int64(sizeof(uint32));
					Component->SetInstanceSourceGPU(Source);
				}
				else
				{
					const uint32 Capacity = uint32(Tile.Num()) * uint32(FMath::Max(Scenario.CapacityFactor, 1));
					const FCSGpuInstancePointSourceGPU Source = MakePointSource(Tile, Capacity, 0.3f);
					PointSources.Add(Source);
					Result.SourceBytes += int64(Capacity) * 2 * int64(sizeof(FVector4f)) + int64(sizeof(uint32));
					Component->SetInstanceSourceFromPoints(Source);
				}
				ClassicComponents.Add(Component);
			}

			Result.Components = ClassicComponents.Num() + IsmComponents.Num();
			return Result.Components > 0;
		}

		/** 预热完才问：代理、常驻流、Nanite 数据都已经落地。 */
		void CollectStaticInfo(FScenarioResult& Result)
		{
			for (const TWeakObjectPtr<UCSGpuInstancedMeshComponent>& WeakComponent : ClassicComponents)
			{
				const UCSGpuInstancedMeshComponent* Component = WeakComponent.Get();
				if (!Component) continue;

				Result.bNanitePath |= Component->IsNaniteRenderPath();
				if (const UCSMesh* GpuMesh = Component->GetGpuMesh())
				{
					const FCSMeshResidentRef Resident = GpuMesh->GetResident();
					if (Resident.IsValid()) Result.ResidentBytes += Resident->GetAllocatedBytes();
				}
				const FCSGpuInstancedGpuLayout& Layout = Component->GetGpuLayout();
				if (Layout.IsValid()) Result.Capacity += Layout.InstanceCapacity;
				else if (const UCSGpuInstancedNaniteComponent* Nanite = Component->GetNaniteComponent()) Result.Capacity += Nanite->GetGpuSceneSlotCount();

				const FCSGpuInstancedBaseMesh& Snapshot = Component->GetBaseMeshSnapshot();
				Result.BaseVertices = Snapshot.Positions.Num();
				Result.BaseTriangles = Snapshot.Indices.Num() / 3;
				Result.LODs = Snapshot.LODs.Num();
				Result.Sections = Snapshot.Sections.Num();
			}
			// Nanite 路要单独记"是不是真的 Nanite 代理"（§1：回退到引擎 ISM 代理的不算 Nanite 测试结果）。只问第一个组件：
			// 这是阻塞诊断，256 个组件各问一遍没有必要。经典路不问 —— 它的材质判据会把引擎默认材质报成"没勾实例化用途"
			// （默认材质没有用途标记但为所有用途都编了着色器），在这里只是噪声。
			if (Result.bNanitePath && ClassicComponents.Num() > 0 && ClassicComponents[0].IsValid())
			{
				Result.Mismatch = ClassicComponents[0]->DebugGetDrawnAssetMismatchSync();
			}
			for (const TWeakObjectPtr<UInstancedStaticMeshComponent>& WeakComponent : IsmComponents)
			{
				const UInstancedStaticMeshComponent* Component = WeakComponent.Get();
				if (!Component) continue;
				Result.Capacity += Component->GetInstanceCount();
				if (const UStaticMesh* Mesh = Component->GetStaticMesh())
				{
					if (const FStaticMeshRenderData* RenderData = Mesh->GetRenderData())
					{
						Result.LODs = RenderData->LODResources.Num();
						if (RenderData->LODResources.Num() > 0)
						{
							Result.BaseVertices = RenderData->LODResources[0].GetNumVertices();
							Result.BaseTriangles = RenderData->LODResources[0].GetNumTriangles();
							Result.Sections = RenderData->LODResources[0].Sections.Num();
						}
					}
					Result.bNanitePath |= Mesh->IsNaniteEnabled();
				}
			}

			const int64 AvailableNow = CSGpuMemoryBudget::QueryMemorySnapshot().AvailableVideoMemory;
			Result.AdapterUsedDelta = (AdapterAvailableBefore > 0 && AvailableNow > 0) ? (AdapterAvailableBefore - AvailableNow) : 0;
		}

		// ---- CPU 实例数组的增删成本（不看画面，只量游戏线程上一次调用的毫秒与阻塞刷新次数） ----

		void RunCpuOps()
		{
			UWorld* WorldPtr = World.Get();
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = RF_Transient;

			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			UStaticMesh* NaniteCube = UseNanite(GMaxRHIShaderPlatform) ? MakeNaniteCube() : nullptr;
			TStrongObjectPtr<UStaticMesh> KeepNanite(NaniteCube);

			for (int32 RouteIndex = 0; RouteIndex < 2; ++RouteIndex)
			{
				UStaticMesh* Mesh = RouteIndex == 0 ? Cube : NaniteCube;
				if (!Mesh) continue;
				const TCHAR* RouteName = RouteIndex == 0 ? TEXT("classic") : TEXT("nanite");

				for (const int32 NumInstances : { 10000, 100000 })
				{
					AActor* Host = WorldPtr->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
					USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
					Host->SetRootComponent(Root);
					Root->RegisterComponent();
					UCSGpuInstancedMeshComponent* Component = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
					Component->SetupAttachment(Root);
					Component->InstanceMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
					Component->RegisterComponent();
					Component->SetBaseMesh(Mesh);

					TArray<FTransform> Transforms;
					MakeTransforms(NumInstances, Transforms);

					auto Measure = [this, RouteName, NumInstances](const TCHAR* Op, int32 Calls, TFunctionRef<void(int32)> Body)
					{
						FCpuOpResult& Result = CpuOps.AddDefaulted_GetRef();
						Result.Route = RouteName;
						Result.NumInstances = NumInstances;
						Result.Op = Op;
						Result.Calls = Calls;
						const int64 FlushesBefore = UCSMesh::GetBlockingFlushCount();
						double Total = 0.0;
						for (int32 Call = 0; Call < Calls; ++Call)
						{
							const double Start = FPlatformTime::Seconds();
							Body(Call);
							const double Ms = (FPlatformTime::Seconds() - Start) * 1000.0;
							Total += Ms;
							Result.MaxMs = FMath::Max(Result.MaxMs, Ms);
						}
						Result.MeanMs = Total / double(Calls);
						Result.FlushesPerCall = double(UCSMesh::GetBlockingFlushCount() - FlushesBefore) / double(Calls);
					};

					Measure(TEXT("SetInstances"), 3, [&](int32) { Component->SetInstances(Transforms); });
					Measure(TEXT("AddInstance"), 8, [&](int32 Call) { Component->AddInstance(FTransform(FVector(Call * 10.0, -500.0, 50.0))); });
					Measure(TEXT("UpdateInstanceTransform"), 8, [&](int32 Call) { Component->UpdateInstanceTransform(Call * 7, FTransform(FVector(Call * 10.0, -700.0, 50.0))); });
					Measure(TEXT("UpdateInstanceTransform_batched8"), 1, [&](int32)
					{
						for (int32 Index = 0; Index < 8; ++Index) Component->UpdateInstanceTransform(Index * 11, FTransform(FVector(Index * 10.0, -900.0, 50.0)), false, Index == 7);
					});
					Measure(TEXT("RemoveInstance_middle"), 8, [&](int32) { Component->RemoveInstance(Component->GetInstanceCount() / 2); });
					Measure(TEXT("RemoveInstance_tail"), 8, [&](int32) { Component->RemoveInstance(Component->GetInstanceCount() - 1); });
					Measure(TEXT("AddInstances_batch8"), 1, [&](int32)
					{
						TArray<FTransform> Batch;
						for (int32 Index = 0; Index < 8; ++Index) Batch.Add(FTransform(FVector(Index * 10.0, -1100.0, 50.0)));
						Component->AddInstances(Batch);
					});
					// 成批区间（BeginInstanceUpdate / EndInstanceUpdate，§6.1）：任意混合的一串改动，一次重排一次上传。
					Measure(TEXT("AddInstance_scope8"), 1, [&](int32)
					{
						FCSGpuInstancedUpdateScope Scope(Component);
						for (int32 Index = 0; Index < 8; ++Index) Component->AddInstance(FTransform(FVector(Index * 10.0, -1300.0, 50.0)));
					});
					Measure(TEXT("MixedOps_scope24"), 1, [&](int32)
					{
						FCSGpuInstancedUpdateScope Scope(Component);
						for (int32 Index = 0; Index < 8; ++Index)
						{
							Component->AddInstance(FTransform(FVector(Index * 10.0, -1500.0, 50.0)));
							Component->RemoveInstance(Index * 13);
							Component->UpdateInstanceTransform(Index * 17, FTransform(FVector(Index * 10.0, -1700.0, 50.0)));
						}
					});
					Measure(TEXT("RemoveInstances_8"), 1, [&](int32)
					{
						TArray<int32> Indices;
						for (int32 Index = 0; Index < 8; ++Index) Indices.Add(Index * 19);
						Component->RemoveInstances(Indices);
					});

					Host->Destroy();
					FlushRenderingCommands();
				}
			}
			CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		}

		// ---- 报表 ----

		static FString CVarValue(const TCHAR* Name)
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
			return Variable ? Variable->GetString() : FString(TEXT("<none>"));
		}

		void WriteSummary() const
		{
			FString Json;
			Json += TEXT("{\n");

			// --- 环境 ---
			FIntPoint Resolution(0, 0);
			if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport) Resolution = GEngine->GameViewport->Viewport->GetSizeXY();
			const CSGpuMemoryBudget::FMemorySnapshot Memory = CSGpuMemoryBudget::QueryMemorySnapshot();

			Json += TEXT("  \"run\": {\n");
			Json += FString::Printf(TEXT("    \"suite\": %s,\n"), *JsonString(Suite));
			Json += FString::Printf(TEXT("    \"time\": %s,\n"), *JsonString(FDateTime::Now().ToIso8601()));
			Json += FString::Printf(TEXT("    \"engine\": %s,\n"), *JsonString(FEngineVersion::Current().ToString()));
			Json += FString::Printf(TEXT("    \"build_config\": %s,\n"), *JsonString(LexToString(FApp::GetBuildConfiguration())));
			Json += FString::Printf(TEXT("    \"gpu\": %s,\n"), *JsonString(GRHIAdapterName));
			Json += FString::Printf(TEXT("    \"driver\": %s,\n"), *JsonString(GRHIAdapterUserDriverVersion));
			Json += FString::Printf(TEXT("    \"rhi\": %s,\n"), *JsonString(GDynamicRHI ? GDynamicRHI->GetName() : TEXT("?")));
			Json += FString::Printf(TEXT("    \"video_memory_total\": %lld,\n"), Memory.TotalVideoMemory);
			Json += FString::Printf(TEXT("    \"resolution\": [%d, %d],\n"), Resolution.X, Resolution.Y);
			Json += FString::Printf(TEXT("    \"measure_frames\": %d,\n"), MeasureFrames);
			Json += FString::Printf(TEXT("    \"pairs\": %d,\n"), Pairs);
			Json += TEXT("    \"variants\": [");
			for (int32 Index = 0; Index < Variants.Num(); ++Index)
			{
				Json += FString::Printf(TEXT("%s{\"name\": %s, \"commands\": %s}"), Index ? TEXT(", ") : TEXT(""),
					*JsonString(Variants[Index].Name), *JsonString(FString::Join(Variants[Index].Commands, TEXT("; "))));
			}
			Json += TEXT("],\n");
			Json += TEXT("    \"cvars\": {");
			const TCHAR* CVarNames[] = {
				TEXT("r.Nanite"), TEXT("r.Shadow.Virtual.Enable"), TEXT("r.Shadow.Virtual.ForceOnlyVirtualShadowMaps"),
				TEXT("r.InstanceCulling.OcclusionCull"), TEXT("r.DynamicGlobalIlluminationMethod"), TEXT("r.ReflectionMethod"),
				TEXT("r.AntiAliasingMethod"), TEXT("r.ScreenPercentage"), TEXT("r.RayTracing"), TEXT("r.Substrate"),
				TEXT("r.VSync"), TEXT("t.MaxFPS"), TEXT("r.GPUCsvStatsEnabled"), TEXT("r.Shadow.CSM.MaxCascades"),
			};
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(CVarNames); ++Index)
			{
				Json += FString::Printf(TEXT("%s%s: %s"), Index ? TEXT(", ") : TEXT(""), *JsonString(CVarNames[Index]), *JsonString(CVarValue(CVarNames[Index])));
			}
			Json += TEXT("},\n");
			Json += FString::Printf(TEXT("    \"commandline\": %s\n"), *JsonString(FCommandLine::Get()));
			Json += TEXT("  },\n");

			// --- 场景 ---
			Json += TEXT("  \"scenarios\": [\n");
			for (int32 ResultIndex = 0; ResultIndex < Results.Num(); ++ResultIndex)
			{
				const FScenarioResult& Result = Results[ResultIndex];
				const FScenario& Scenario = Result.Scenario;
				Json += TEXT("    {\n");
				Json += FString::Printf(TEXT("      \"name\": %s,\n"), *JsonString(Scenario.Name));
				Json += FString::Printf(TEXT("      \"route\": %s, \"source\": %s, \"mesh\": %s,\n"),
					*JsonString(ToString(Scenario.Route)), *JsonString(ToString(Scenario.Source)),
					*JsonString(Scenario.Mesh == EMeshKind::Sphere ? TEXT("sphere") : TEXT("cube")));
				Json += FString::Printf(TEXT("      \"instances\": %d, \"capacity_factor\": %d, \"components_requested\": %d, \"cast_shadow\": %s,\n"),
					Scenario.NumInstances, Scenario.CapacityFactor, Scenario.NumComponents, Scenario.bCastShadow ? TEXT("true") : TEXT("false"));
				Json += FString::Printf(TEXT("      \"cull_start\": %s, \"cull_end\": %s, \"cull_fade\": %s,\n"),
					*JsonNumber(Scenario.StartCullDistance), *JsonNumber(Scenario.EndCullDistance), *JsonNumber(Scenario.CullFadeDistance));
				Json += FString::Printf(TEXT("      \"spawned\": %s, \"note\": %s,\n"), Result.bSpawned ? TEXT("true") : TEXT("false"), *JsonString(Result.Note));
				Json += FString::Printf(TEXT("      \"nanite_path\": %s, \"mismatch\": %s,\n"), Result.bNanitePath ? TEXT("true") : TEXT("false"), *JsonString(Result.Mismatch));
				Json += FString::Printf(TEXT("      \"components\": %d, \"capacity\": %lld, \"live_instances\": %lld,\n"), Result.Components, Result.Capacity, Result.LiveInstances);
				Json += FString::Printf(TEXT("      \"base_vertices\": %d, \"base_triangles\": %d, \"lods\": %d, \"sections\": %d,\n"),
					Result.BaseVertices, Result.BaseTriangles, Result.LODs, Result.Sections);
				Json += FString::Printf(TEXT("      \"resident_bytes\": %lld, \"source_bytes\": %lld, \"adapter_used_delta\": %lld,\n"),
					Result.ResidentBytes, Result.SourceBytes, Result.AdapterUsedDelta);
				Json += FString::Printf(TEXT("      \"field_half_extent\": %s, \"warmup_seconds\": %s,\n"), *JsonNumber(Result.FieldHalfExtent), *JsonNumber(Result.WarmupSeconds));
				Json += TEXT("      \"measurements\": [\n");
				for (int32 MeasurementIndex = 0; MeasurementIndex < Result.Measurements.Num(); ++MeasurementIndex)
				{
					const FMeasurement& Measurement = Result.Measurements[MeasurementIndex];
					Json += FString::Printf(TEXT("        {\"pose\": %s, \"variant\": %s, \"pair\": %d, \"frames\": %d, \"csv\": %s, \"resident_bytes\": %lld, \"visible_per_lod\": ["),
						*JsonString(Measurement.Pose), *JsonString(Measurement.Variant), Measurement.Pair, Measurement.Frames, *JsonString(Measurement.CsvFile), Measurement.ResidentBytes);
					for (int32 Lod = 0; Lod < Measurement.VisiblePerLod.Num(); ++Lod) Json += FString::Printf(TEXT("%s%d"), Lod ? TEXT(", ") : TEXT(""), Measurement.VisiblePerLod[Lod]);
					Json += TEXT("], \"stats\": {");

					TArray<FString> Keys;
					Measurement.Stats.GetKeys(Keys);
					Keys.Sort();
					for (int32 KeyIndex = 0; KeyIndex < Keys.Num(); ++KeyIndex)
					{
						const FColumnStats& Stats = Measurement.Stats[Keys[KeyIndex]];
						Json += FString::Printf(TEXT("%s%s: {\"med\": %s, \"p95\": %s, \"mean\": %s}"), KeyIndex ? TEXT(", ") : TEXT(""),
							*JsonString(Keys[KeyIndex]), *JsonNumber(Stats.Median), *JsonNumber(Stats.P95), *JsonNumber(Stats.Mean));
					}
					Json += FString::Printf(TEXT("}}%s\n"), MeasurementIndex + 1 < Result.Measurements.Num() ? TEXT(",") : TEXT(""));
				}
				Json += TEXT("      ]\n");
				Json += FString::Printf(TEXT("    }%s\n"), ResultIndex + 1 < Results.Num() ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("  ],\n");

			// --- CPU 增删 ---
			Json += TEXT("  \"cpu_ops\": [\n");
			for (int32 Index = 0; Index < CpuOps.Num(); ++Index)
			{
				const FCpuOpResult& Op = CpuOps[Index];
				Json += FString::Printf(TEXT("    {\"route\": %s, \"instances\": %d, \"op\": %s, \"calls\": %d, \"mean_ms\": %s, \"max_ms\": %s, \"flushes_per_call\": %s}%s\n"),
					*JsonString(Op.Route), Op.NumInstances, *JsonString(Op.Op), Op.Calls, *JsonNumber(Op.MeanMs), *JsonNumber(Op.MaxMs), *JsonNumber(Op.FlushesPerCall),
					Index + 1 < CpuOps.Num() ? TEXT(",") : TEXT(""));
			}
			Json += TEXT("  ]\n}\n");

			FFileHelper::SaveStringToFile(Json, *FPaths::Combine(OutDir, TEXT("summary.json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}

		// ---- 状态 ----

		TWeakObjectPtr<UWorld> World;
		FTSTicker::FDelegateHandle TickerHandle;
		EPhase Phase = EPhase::Idle;

		FString Suite;
		FString OutDir;
		TArray<FString> Filter;
		TArray<FVariant> Variants;
		int32 MeasureFrames = 200;
		int32 SettleFrames = 30;
		int32 WarmupFrames = 90;
		int32 Pairs = 1;
		bool bExitWhenDone = true;
		bool bRunCpuOps = true;

		TArray<FScenario> Scenarios;
		TArray<FScenarioResult> Results;
		TArray<FCpuOpResult> CpuOps;
		int32 ScenarioIndex = 0;

		TArray<FPose> Poses;
		int32 PoseIndex = 0;
		int32 StepIndex = 0;
		int32 FrameCounter = 0;
		int32 OrbitFrameBase = 0;
		double PhaseStartSeconds = 0.0;
		/** `delay=` 秒；到点之前 Tick 什么都不做。 */
		double StartDelaySeconds = 0.0;
		double StartAtSeconds = 0.0;
		/** 机位的原点（场地中心）。合成场景恒为零。 */
		FVector PoseOrigin = FVector::ZeroVector;
		/** `suite=map` 的额外机位：`look=x/y/z`（世界坐标）+ `dist=`（离它多远，cm）。 */
		bool bHasLookAt = false;
		FVector LookAt = FVector::ZeroVector;
		double LookDistance = 2000.0;
		int64 AdapterAvailableBefore = 0;

		FString CurrentCsvName;
		TSharedFuture<FString> CsvFuture;

		TWeakObjectPtr<ACameraActor> Camera;
		TArray<TWeakObjectPtr<AActor>> Actors;
		TArray<TWeakObjectPtr<UCSGpuInstancedMeshComponent>> ClassicComponents;
		TArray<TWeakObjectPtr<UInstancedStaticMeshComponent>> IsmComponents;
		/** 生产者那一侧的引用：真的生产者一直攥着自己的 buffer，组件只是借。 */
		TArray<FCSGpuInstanceSourceGPU> GpuSources;
		TArray<FCSGpuInstancePointSourceGPU> PointSources;
		TArray<TStrongObjectPtr<UStaticMesh>> Meshes;
	};

	FAutoConsoleCommandWithWorldAndArgs GBenchCommand(
		TEXT("CSGpuInstanced.Bench"),
		TEXT("实例路性能基线 / A-B（-game 进程里用）：suite=baseline|smoke|map out=<目录> frames=N pairs=N res=1920x1080 filter=a+b variants=名:命令;命令~名:… delay=秒 look=x/y/z dist=厘米 exit=0/1 cpuops=0/1"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			FRunner::Get().Start(World, Args);
		}));
}

#endif // !UE_BUILD_SHIPPING && WITH_EDITOR && CSV_PROFILER

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshComponent.h"      // BuildGpuMeshDescription
#include "CSGpuSharedLayout.ush"     // 行 stride / custom data 步长
#include "CSStaticMeshAssetSink.h"   // CSStaticMeshAsset::MaterialSlotName

#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneComponent.h"
#include "Engine/SceneCapture2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "MaterialDomain.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionPerInstanceCustomData.h"
#include "MeshDescription.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "StaticMeshResources.h"
#include "Tests/AutomationEditorCommon.h"
#include "TextureResource.h"
#include "UObject/Package.h"

// 「可见源行号」布局（InstanceCullingOptimization.md §5，`r.CSGpuInstanced.VisibleSourceIndex`）的画面判据。
//
// 这一步换的是"绘制从哪儿读实例属性"：旧布局读剔除 pass 抄出来的副本，新布局经 4 字节的源行号直接读源实例行。
// 写错了不报错，只是画面错 —— 实例画在别人的位置上、LOD 区段错位读到另一级的行号、custom data 张冠李戴 ——
// 而实例数、可见数、每个数值的取值范围全都照常。所以判据只能是画面，而且要两头都守：
//   ① 新布局的画面与 GPU 自己回读的"每个可见槽指向哪一行、落在哪一级"逐实例对得上（它是对的）；
//   ② 旧布局同一个场景的画面与它逐像素相同（它没有变）。
// 场景捕获没有时域效果（没有持久的 view state ⇒ 没有 TAA 抖动），同一个场景拍两次逐像素相同 —— 用例里先验这一条。
namespace
{
	/** 相机离拍摄中心的高度与画幅。90° 视角下，中心平面上的半宽恰好等于高度。 */
	constexpr double CSIndexTest_CameraHeight = 2000.0;
	constexpr int32 CSIndexTest_ImageSize = 512;

	/** 各级 LOD 的盒子高度（底面在 z = 0，占地都是 100 × 100）：俯拍的深度图上，每个实例画的是哪一级一眼分得出。 */
	constexpr float CSIndexTest_LodHeights[] = { 100.0f, 60.0f, 30.0f };
	/** 换级阈值（屏占比）。配合下面三档实例缩放，俯拍一张图里三级都有。 */
	constexpr float CSIndexTest_LodScreenSizes[] = { 1.0f, 0.022f, 0.012f };
	constexpr int32 CSIndexTest_NumLods = UE_ARRAY_COUNT(CSIndexTest_LodHeights);

	/** 探针材质把 custom data[0] 乘上它当 WPO.z —— 顶点着色器那条读取路径在深度图上看得见。 */
	constexpr float CSIndexTest_LiftPerUnit = 100.0f;

	/** 占地 100 × 100、z ∈ [0, Height] 的盒子，每面四个顶点。 */
	FCSGpuMeshCPUData CSIndexTest_MakeBox(float Height)
	{
		FCSGpuMeshCPUData Data;
		Data.SourceSpace = FCSGpuMeshCPUData::ESpace::ComponentLocal;
		Data.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;

		const FVector3f Centre(0.0f, 0.0f, Height * 0.5f);
		const FVector3f Half(50.0f, 50.0f, Height * 0.5f);
		const float Corners[4][2] = { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } };
		for (int32 Axis = 0; Axis < 3; ++Axis)
		{
			for (int32 Side = 0; Side < 2; ++Side)
			{
				FVector3f Normal(ForceInitToZero);
				FVector3f U(ForceInitToZero);
				FVector3f V(ForceInitToZero);
				Normal[Axis] = Side == 0 ? -1.0f : 1.0f;
				U[(Axis + 1) % 3] = 1.0f;
				V[(Axis + 2) % 3] = 1.0f;

				const uint32 Base = uint32(Data.Positions.Num());
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					Data.Positions.Add(Centre + (Normal + U * Corners[Corner][0] + V * Corners[Corner][1]) * Half);
					Data.Normals.Add(Normal);
					Data.Tangents.Add(U);
					Data.TexCoords().Add(FVector2f(Corners[Corner][0] * 0.5f + 0.5f, Corners[Corner][1] * 0.5f + 0.5f));
				}

				// 绕序按引擎口径：几何法线 = (P2 − P0) × (P1 − P0) 朝外（见 UCSGpuMeshComponent::BuildGpuMeshDescription）。
				auto AddTriangle = [&Data, &Normal](uint32 A, uint32 B, uint32 C)
				{
					const FVector3f Face = FVector3f::CrossProduct(Data.Positions[int32(C)] - Data.Positions[int32(A)], Data.Positions[int32(B)] - Data.Positions[int32(A)]);
					if (FVector3f::DotProduct(Face, Normal) < 0.0f) Swap(B, C);
					Data.Indices.Add(A);
					Data.Indices.Add(B);
					Data.Indices.Add(C);
				};
				AddTriangle(Base, Base + 1u, Base + 2u);
				AddTriangle(Base, Base + 2u, Base + 3u);
			}
		}
		return Data;
	}

	/** 三级 LOD 的盒子网格。fast build：各级 LOD 就是给进去的那几份几何，不经减面。 */
	UStaticMesh* CSIndexTest_MakeLodBoxMesh()
	{
		TArray<FMeshDescription> Descriptions;
		Descriptions.SetNum(CSIndexTest_NumLods);
		for (int32 Lod = 0; Lod < CSIndexTest_NumLods; ++Lod)
		{
			if (!UCSGpuMeshComponent::BuildGpuMeshDescription(CSIndexTest_MakeBox(CSIndexTest_LodHeights[Lod]), FTransform::Identity, false, Descriptions[Lod])) return nullptr;
		}

		UStaticMesh* Mesh = NewObject<UStaticMesh>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UStaticMesh::StaticClass(), TEXT("SM_CSIndexTestLodBox")), RF_Transient);
		Mesh->bAllowCPUAccess = true;
		const FName SlotName = CSStaticMeshAsset::MaterialSlotName(0);
		Mesh->GetStaticMaterials().Add(FStaticMaterial(UMaterial::GetDefaultMaterial(MD_Surface), SlotName, SlotName));

		TArray<const FMeshDescription*> Pointers;
		for (const FMeshDescription& Description : Descriptions) Pointers.Add(&Description);

		UStaticMesh::FBuildMeshDescriptionsParams Params;
		Params.bFastBuild = true;
		Params.bCommitMeshDescription = false;
		Params.bBuildSimpleCollision = false;
		Params.bAllowCpuAccess = true;
		if (!Mesh->BuildFromMeshDescriptions(Pointers, Params)) return nullptr;

		FStaticMeshRenderData* RenderData = Mesh->GetRenderData();
		if (!RenderData) return nullptr;
		for (int32 Lod = 0; Lod < CSIndexTest_NumLods; ++Lod) RenderData->ScreenSize[Lod].Default = CSIndexTest_LodScreenSizes[Lod];
		return Mesh;
	}

	/**
	 * 探针材质：把逐实例属性画出来。
	 *   BaseColor = (PerInstanceRandom, CustomData[0], CustomData[1])   —— 像素着色器那条读取路径
	 *   WPO.z     = CustomData[0] × CSIndexTest_LiftPerUnit             —— 顶点着色器那条读取路径
	 * 两条路径在引擎的 MaterialTemplate 里取下标的办法不一样（顶点：InstanceId + InstanceOffset；像素：PerInstanceParams.w），
	 * 生成的着色器对这两处各打了一个补丁，所以两条都要有判据。
	 */
	UMaterial* CSIndexTest_MakeProbeMaterial()
	{
		UMaterial* Material = NewObject<UMaterial>(GetTransientPackage(),
			MakeUniqueObjectName(GetTransientPackage(), UMaterial::StaticClass(), TEXT("M_CSIndexTestProbe")), RF_Transient);
		Material->bUsedWithInstancedStaticMeshes = true;

		// 这两种节点的类没有导出（UCLASS 不带 MinimalAPI），NewObject<T> 链接不上 StaticClass —— 按路径找类。
		auto NewByPath = [Material](const TCHAR* ClassPath) -> UMaterialExpression*
		{
			UClass* Class = FindObject<UClass>(nullptr, ClassPath);
			if (!Class) return nullptr;
			UMaterialExpression* Expression = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transient);
			Material->GetExpressionCollection().AddExpression(Expression);
			return Expression;
		};
		auto NewCustomData = [&NewByPath](uint32 Index) -> UMaterialExpression*
		{
			UMaterialExpression* Expression = NewByPath(TEXT("/Script/Engine.MaterialExpressionPerInstanceCustomData"));
			if (Expression)
			{
				UMaterialExpressionPerInstanceCustomData* CustomData = static_cast<UMaterialExpressionPerInstanceCustomData*>(Expression);
				CustomData->DataIndex = Index;
				CustomData->ConstDefaultValue = 0.0f;
			}
			return Expression;
		};
		auto NewTyped = [Material](UClass* Class) -> UMaterialExpression*
		{
			UMaterialExpression* Expression = NewObject<UMaterialExpression>(Material, Class, NAME_None, RF_Transient);
			Material->GetExpressionCollection().AddExpression(Expression);
			return Expression;
		};

		UMaterialExpression* Random = NewByPath(TEXT("/Script/Engine.MaterialExpressionPerInstanceRandom"));
		UMaterialExpression* Data0Pixel = NewCustomData(0);
		UMaterialExpression* Data1Pixel = NewCustomData(1);
		UMaterialExpression* Data0Vertex = NewCustomData(0);
		if (!Random || !Data0Pixel || !Data1Pixel || !Data0Vertex) return nullptr;

		UMaterialExpressionAppendVector* RG = static_cast<UMaterialExpressionAppendVector*>(NewTyped(UMaterialExpressionAppendVector::StaticClass()));
		RG->A.Connect(0, Random);
		RG->B.Connect(0, Data0Pixel);
		UMaterialExpressionAppendVector* RGB = static_cast<UMaterialExpressionAppendVector*>(NewTyped(UMaterialExpressionAppendVector::StaticClass()));
		RGB->A.Connect(0, RG);
		RGB->B.Connect(0, Data1Pixel);

		UMaterialExpressionMultiply* Lift = static_cast<UMaterialExpressionMultiply*>(NewTyped(UMaterialExpressionMultiply::StaticClass()));
		Lift->A.Connect(0, Data0Vertex);
		Lift->ConstB = CSIndexTest_LiftPerUnit;
		UMaterialExpressionConstant2Vector* Zero2 = static_cast<UMaterialExpressionConstant2Vector*>(NewTyped(UMaterialExpressionConstant2Vector::StaticClass()));
		Zero2->R = 0.0f;
		Zero2->G = 0.0f;
		UMaterialExpressionAppendVector* Offset = static_cast<UMaterialExpressionAppendVector*>(NewTyped(UMaterialExpressionAppendVector::StaticClass()));
		Offset->A.Connect(0, Zero2);
		Offset->B.Connect(0, Lift);

		UMaterialEditorOnlyData* EditorData = Material->GetEditorOnlyData();
		if (!EditorData) return nullptr;
		EditorData->BaseColor.Connect(0, RGB);
		EditorData->WorldPositionOffset.Connect(0, Offset);

		Material->PreEditChange(nullptr);
		Material->PostEditChange();
		// 编辑器按需编译着色器：不等齐的话头几次拍到的是回退用的默认材质。
		Material->EnsureIsComplete();
		return Material;
	}

	/** 一个实例该画成什么样。Location 相对拍摄中心；下标就是它的身份（CPU 源 = 稳定槽，GPU 源 = 源行号）。 */
	struct FCSIndexTest_Probe
	{
		FVector Location = FVector::ZeroVector;
		float Scale = 1.0f;
		/** 该不该出现在画面里：视锥外的、被生产者藏起来的（随机数 < 0）、已经删掉的都是 false。 */
		bool bExpectVisible = true;
		/** 探针材质下的期望颜色：R = 逐实例随机数，G / B = custom data[0] / [1]。 */
		float Random = 0.0f;
		float Data0 = 0.0f;
		float Data1 = 0.0f;
	};

	struct FCSIndexTest_Picture
	{
		TArray<float> Depth;             // 视空间深度，cm；没有几何的像素是远平面
		TArray<FLinearColor> BaseColor;  // 只有要了才拍
	};

	/** 从 Centre 正上方俯拍。真渲染：剔除 pass 就发生在这里面，拍完回读到的可见缓冲就是这张图用的那一份。 */
	FCSIndexTest_Picture CSIndexTest_Capture(UWorld* World, const FVector& Centre, bool bWithBaseColor)
	{
		UTextureRenderTarget2D* Target = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		Target->RenderTargetFormat = RTF_RGBA32f;
		Target->ClearColor = FLinearColor::Black;
		Target->InitAutoFormat(CSIndexTest_ImageSize, CSIndexTest_ImageSize);
		Target->UpdateResourceImmediate(true);

		FActorSpawnParameters SpawnParameters;
		SpawnParameters.ObjectFlags = RF_Transient;
		ASceneCapture2D* CaptureActor = World->SpawnActor<ASceneCapture2D>(
			Centre + FVector(0.0, 0.0, CSIndexTest_CameraHeight), FRotator(-90.0, 0.0, 0.0), SpawnParameters);
		USceneCaptureComponent2D* Capture = CaptureActor->GetCaptureComponent2D();
		Capture->TextureTarget = Target;
		Capture->FOVAngle = 90.0f;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;

		// 帧末的组件更新（换代理）在测试体里没人替我们跑。
		World->SendAllEndOfFrameUpdates();

		FCSIndexTest_Picture Picture;
		TArray<FLinearColor> Pixels;
		if (bWithBaseColor)
		{
			Capture->CaptureSource = ESceneCaptureSource::SCS_BaseColor;
			Capture->CaptureScene();
			FlushRenderingCommands();
			Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Picture.BaseColor);
		}
		// 深度放在最后拍：调用方接着回读的可见缓冲属于最后一次渲染。
		Capture->CaptureSource = ESceneCaptureSource::SCS_SceneDepth;
		Capture->CaptureScene();
		FlushRenderingCommands();
		Target->GameThread_GetRenderTargetResource()->ReadLinearColorPixels(Pixels);
		CaptureActor->Destroy();

		Picture.Depth.Reserve(Pixels.Num());
		for (const FLinearColor& Pixel : Pixels) Picture.Depth.Add(Pixel.R);
		return Picture;
	}

	/** 相对拍摄中心的一个点落在哪个像素。pitch −90 的相机：画面右 = +Y、画面上 = +X；90° 视角 ⇒ 归一化坐标 = 水平偏移 / 深度。 */
	FIntPoint CSIndexTest_Project(const FVector& Relative)
	{
		const double Half = CSIndexTest_ImageSize * 0.5;
		const double Depth = CSIndexTest_CameraHeight - Relative.Z;
		return FIntPoint(FMath::FloorToInt32(Half + Relative.Y / Depth * Half), FMath::FloorToInt32(Half - Relative.X / Depth * Half));
	}

	bool CSIndexTest_IsGeometry(float Depth)
	{
		return Depth > 1.0f && Depth < float(CSIndexTest_CameraHeight) * 2.0f;
	}

	/** 两张深度图差出 Tolerance 以上的像素数。 */
	int32 CSIndexTest_CountDepthDifferences(const TArray<float>& A, const TArray<float>& B, float Tolerance, float& OutMaxDifference)
	{
		OutMaxDifference = 0.0f;
		if (A.Num() != B.Num()) return FMath::Max(A.Num(), B.Num());
		int32 Differing = 0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const float Difference = FMath::Abs(A[Index] - B[Index]);
			OutMaxDifference = FMath::Max(OutMaxDifference, Difference);
			if (Difference > Tolerance) ++Differing;
		}
		return Differing;
	}

	int32 CSIndexTest_CountColorDifferences(const TArray<FLinearColor>& A, const TArray<FLinearColor>& B, float Tolerance, float& OutMaxDifference)
	{
		OutMaxDifference = 0.0f;
		if (A.Num() != B.Num()) return FMath::Max(A.Num(), B.Num());
		int32 Differing = 0;
		for (int32 Index = 0; Index < A.Num(); ++Index)
		{
			const float Difference = FMath::Max3(FMath::Abs(A[Index].R - B[Index].R), FMath::Abs(A[Index].G - B[Index].G), FMath::Abs(A[Index].B - B[Index].B));
			OutMaxDifference = FMath::Max(OutMaxDifference, Difference);
			if (Difference > Tolerance) ++Differing;
		}
		return Differing;
	}

	/** 切可见缓冲布局。CVar 的回调当场把已注册的组件重建成那种布局，代理在下一次帧末更新里换。析构时还原。 */
	struct FCSIndexTest_LayoutScope
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CSGpuInstanced.VisibleSourceIndex"));
		int32 Saved = CVar ? CVar->GetInt() : 0;

		bool Set(bool bIndex)
		{
			if (!CVar) return false;
			CVar->Set(bIndex ? 1 : 0, ECVF_SetByCode);
			return (CVar->GetInt() != 0) == bIndex;
		}

		~FCSIndexTest_LayoutScope()
		{
			if (CVar) CVar->Set(Saved, ECVF_SetByCode);
		}
	};

	/** 按组件的打包约定（FCSGpuInstanceSourceGPU）把一个探针打成一行。随机数 < 0 = 生产者要藏起来的行。 */
	void CSIndexTest_AppendRow(TArray<FVector4f>& Rows, const FVector& Location, float Scale, float Random)
	{
		const FMatrix44f M = FMatrix44f(FTransform(FRotator::ZeroRotator, Location, FVector(Scale)).ToMatrixWithScale());
		Rows.Add(FVector4f(M.M[0][0], M.M[0][1], M.M[0][2], 0.0f));
		Rows.Add(FVector4f(M.M[1][0], M.M[1][1], M.M[1][2], 0.0f));
		Rows.Add(FVector4f(M.M[2][0], M.M[2][1], M.M[2][2], 0.0f));
		Rows.Add(FVector4f(M.M[3][0], M.M[3][1], M.M[3][2], Random));
		// 剔除球：LOD0 盒子的包围球（中心在半高处）跟着这一行的变换走。
		const float BaseHalfHeight = CSIndexTest_LodHeights[0] * 0.5f;
		const float BaseRadius = FVector3f(50.0f, 50.0f, BaseHalfHeight).Size();
		Rows.Add(FVector4f(FVector3f(Location) + FVector3f(0.0f, 0.0f, BaseHalfHeight * Scale), BaseRadius * Scale));
	}

	/** 像生产者那样在 GPU 上备一份实例源：行 + 计数器（+ 可选的 custom data），都停在 SRVMask。容量可以大于活跃数。 */
	FCSGpuInstanceSourceGPU CSIndexTest_MakeGpuSource(const TArray<FVector4f>& LiveRows, const TArray<float>* CustomData, uint32 Capacity, const FBox& LocalBounds)
	{
		const uint32 LiveCount = uint32(LiveRows.Num() / CS_GPU_INSTANCED_ROW_FLOAT4S);
		Capacity = FMath::Max(Capacity, LiveCount);

		FCSGpuInstanceSourceGPU Source;
		ENQUEUE_RENDER_COMMAND(CSIndexTestMakeSource)([&Source, &LiveRows, CustomData, LiveCount, Capacity](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef RowsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSIndexTest.Rows"));
			GraphBuilder.QueueBufferUpload(RowsBuffer, LiveRows.GetData(), LiveRows.Num() * sizeof(FVector4f), ERDGInitialDataFlags::None);
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSIndexTest.Count"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.PackedInstances = GraphBuilder.ConvertToExternalBuffer(RowsBuffer);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(RowsBuffer, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			if (CustomData)
			{
				FRDGBufferRef CustomDataBuffer = GraphBuilder.CreateBuffer(
					FRDGBufferDesc::CreateBufferDesc(sizeof(float), Capacity * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS), TEXT("CSIndexTest.CustomData"));
				GraphBuilder.QueueBufferUpload(CustomDataBuffer, CustomData->GetData(), CustomData->Num() * sizeof(float), ERDGInitialDataFlags::None);
				Source.CustomData = GraphBuilder.ConvertToExternalBuffer(CustomDataBuffer);
				GraphBuilder.SetBufferAccessFinal(CustomDataBuffer, ERHIAccess::SRVMask);
			}
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = Capacity;
		Source.LocalBounds = LocalBounds;
		return Source;
	}

	/** 点刷那样的点云源：世界空间位置 + 朝上的法线，计数器只放前 LiveCount 个。 */
	FCSGpuInstancePointSourceGPU CSIndexTest_MakePointSource(const TArray<FVector>& WorldPositions, uint32 LiveCount, float InstanceScale)
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
		ENQUEUE_RENDER_COMMAND(CSIndexTestMakePointSource)([&Source, &Positions, &Normals, LiveCount](FRHICommandListImmediate& RHICmdList)
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
			Source.Positions = Upload(Positions, TEXT("CSIndexTest.PointPositions"));
			Source.Normals = Upload(Normals, TEXT("CSIndexTest.PointNormals"));
			FRDGBufferRef CountBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSIndexTest.PointCount"));
			GraphBuilder.QueueBufferUpload(CountBuffer, &LiveCount, sizeof(uint32), ERDGInitialDataFlags::None);
			Source.Counter = GraphBuilder.ConvertToExternalBuffer(CountBuffer);
			GraphBuilder.SetBufferAccessFinal(CountBuffer, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});
		FlushRenderingCommands();
		Source.Capacity = uint32(WorldPositions.Num());
		Source.InstanceScale = InstanceScale;
		Source.WorldBounds = WorldBounds;
		return Source;
	}

	/** 点云源的逐实例随机数：PackPointInstancesCS 里那个哈希（CSGpuInstancedMesh.usf 的 InstanceRandom），按点的下标取。 */
	float CSIndexTest_PointRandom(uint32 Index)
	{
		uint32 H = Index * 747796405u + 2891336453u;
		H = ((H >> ((H >> 28) + 4u)) ^ H) * 277803737u;
		H = (H >> 22) ^ H;
		return float(H & 0xFFFFFFu) / float(0x1000000u);
	}

	/** 测试场景的公共部分：新关卡 + 一个离原点远远的宿主 + 一个注册好的实例组件。 */
	struct FCSIndexTest_Scene
	{
		UWorld* World = nullptr;
		AActor* Host = nullptr;
		UCSGpuInstancedMeshComponent* Instanced = nullptr;
		// 离原点远远的：新关卡里就算有什么东西，也进不了俯拍的画面。
		FVector Origin = FVector(0.0, 0.0, 20000.0);

		/** 先建世界：换关卡会跑一次 GC，在它之前建的临时网格 / 材质没人引用，会被收走。 */
		bool CreateWorld(FAutomationTestBase& Test)
		{
			World = FAutomationEditorCommonUtils::CreateNewMap();
			return Test.TestNotNull(TEXT("Editor test world"), World);
		}

		bool Spawn(FAutomationTestBase& Test, UStaticMesh* Mesh, UMaterialInterface* Material)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.ObjectFlags = RF_Transient;
			Host = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, SpawnParameters);
			if (!Test.TestNotNull(TEXT("Host actor"), Host)) return false;
			USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
			Host->SetRootComponent(Root);
			Root->RegisterComponent();
			Root->SetWorldLocation(Origin);

			Instanced = NewObject<UCSGpuInstancedMeshComponent>(Host, NAME_None, RF_Transient);
			Instanced->SetupAttachment(Root);
			Instanced->InstancesPerCluster = 8;
			Instanced->RegisterComponent();
			Instanced->InstanceMaterial = Material;
			Instanced->SetBaseMesh(Mesh);
			return Test.TestEqual(TEXT("基础网格带进来了三级 LOD"), Instanced->GetBaseMeshSnapshot().LODs.Num(), CSIndexTest_NumLods);
		}
	};

	/**
	 * 「可见源行号」布局下，把上一次剔除压出来的可见槽换成"探针下标 → 画在哪一级"。
	 * CPU 源：行号经行表（GetPackedInstanceSlots）换成稳定槽；GPU 源：行号就是探针下标。
	 * 同一个探针出现两次（同级或跨级）、行号越界都算错。返回 false = 读不到或者表本身就不自洽。
	 */
	bool CSIndexTest_ReadVisibleLods(FAutomationTestBase& Test, const TCHAR* Stage, const UCSGpuInstancedMeshComponent* Instanced,
		bool bCpuSource, int32 NumProbes, TMap<int32, int32>& OutLodOfProbe, TArray<int32>& OutCountPerLod)
	{
		OutLodOfProbe.Reset();
		OutCountPerLod.Reset();

		TArray<TArray<int32>> RowsPerLod;
		if (!Test.TestTrue(FString::Printf(TEXT("[%s] 可见源行号读得回来"), Stage), Instanced->DebugReadVisibleSourceIndicesSync(RowsPerLod))) return false;

		const TArray<int32>& RowSlots = Instanced->GetPackedInstanceSlots();
		int32 Bad = 0;
		for (int32 Lod = 0; Lod < RowsPerLod.Num(); ++Lod)
		{
			OutCountPerLod.Add(RowsPerLod[Lod].Num());
			for (const int32 Row : RowsPerLod[Lod])
			{
				const int32 Probe = bCpuSource ? (RowSlots.IsValidIndex(Row) ? RowSlots[Row] : INDEX_NONE) : Row;
				if (Probe < 0 || Probe >= NumProbes || OutLodOfProbe.Contains(Probe))
				{
					++Bad;
					continue;
				}
				OutLodOfProbe.Add(Probe, Lod);
			}
		}
		return Test.TestEqual(FString::Printf(TEXT("[%s] 可见槽里的行号都落在源行范围内、每个实例只出现一次"), Stage), Bad, 0);
	}

	/**
	 * 画面与"哪些实例可见、各在哪一级"逐实例对：
	 *   · 该看见的都在表里、不该看见的都不在；
	 *   · 每个可见实例的顶面中心，深度 = 相机高度 − (缩放 × 那一级的盒高 + custom data[0] 抬起来的那一截)；
	 *   · bCheckColor：那个像素的 BaseColor = (随机数, custom data[0], custom data[1])；
	 *   · 藏起来 / 删掉、但位置在画面内的实例：它的位置上没有几何。
	 */
	void CSIndexTest_ExpectPicture(FAutomationTestBase& Test, const TCHAR* Stage, const FCSIndexTest_Picture& Picture,
		const TArray<FCSIndexTest_Probe>& Probes, const TMap<int32, int32>& LodOfProbe, bool bCheckColor)
	{
		const int32 NumPixels = CSIndexTest_ImageSize * CSIndexTest_ImageSize;
		if (!Test.TestEqual(FString::Printf(TEXT("[%s] 深度图拍到了"), Stage), Picture.Depth.Num(), NumPixels)) return;
		if (bCheckColor && !Test.TestEqual(FString::Printf(TEXT("[%s] BaseColor 拍到了"), Stage), Picture.BaseColor.Num(), NumPixels)) return;

		int32 ExpectedVisible = 0, Missing = 0, Unexpected = 0, WrongDepth = 0, WrongColor = 0, Ghosts = 0;
		FString FirstProblem;
		auto Note = [&FirstProblem](const FString& Problem) { if (FirstProblem.IsEmpty()) FirstProblem = Problem; };

		for (int32 Index = 0; Index < Probes.Num(); ++Index)
		{
			const FCSIndexTest_Probe& Probe = Probes[Index];
			const int32* Lod = LodOfProbe.Find(Index);

			if (!Probe.bExpectVisible)
			{
				if (Lod)
				{
					++Unexpected;
					Note(FString::Printf(TEXT("探针 %d 不该可见，却在第 %d 级的可见槽里"), Index, *Lod));
				}
				// 它要是被画出来，不管落在哪一级、被 WPO 抬了多少，都在"底面到 LOD0 顶面再加满抬升"这根竖轴上。
				// 透视下竖轴投成一小段径向线（越高越往外），沿它采几个点，一个都不许有几何。相邻实例隔着 250 以上，够不着。
				const double MaxTop = double(Probe.Scale) * CSIndexTest_LodHeights[0] + (bCheckColor ? double(CSIndexTest_LiftPerUnit) : 0.0);
				constexpr int32 AxisSamples = 12;
				for (int32 Sample = 0; Sample <= AxisSamples; ++Sample)
				{
					const FIntPoint Pixel = CSIndexTest_Project(Probe.Location + FVector(0.0, 0.0, MaxTop * double(Sample) / double(AxisSamples)));
					if (Pixel.X < 0 || Pixel.Y < 0 || Pixel.X >= CSIndexTest_ImageSize || Pixel.Y >= CSIndexTest_ImageSize) continue;
					const float Depth = Picture.Depth[Pixel.Y * CSIndexTest_ImageSize + Pixel.X];
					if (!CSIndexTest_IsGeometry(Depth)) continue;
					++Ghosts;
					Note(FString::Printf(TEXT("探针 %d 不该可见，它的位置上却有几何（深度 %.1f）"), Index, Depth));
					break;
				}
				continue;
			}

			++ExpectedVisible;
			if (!Lod)
			{
				++Missing;
				Note(FString::Printf(TEXT("探针 %d 该可见，却不在任何一级的可见槽里"), Index));
				continue;
			}
			if (!ensure(*Lod >= 0 && *Lod < CSIndexTest_NumLods)) continue;

			const double Top = Probe.Location.Z + double(Probe.Scale) * CSIndexTest_LodHeights[*Lod] + double(Probe.Data0) * CSIndexTest_LiftPerUnit;
			const FIntPoint Pixel = CSIndexTest_Project(FVector(Probe.Location.X, Probe.Location.Y, Top));
			if (Pixel.X < 0 || Pixel.Y < 0 || Pixel.X >= CSIndexTest_ImageSize || Pixel.Y >= CSIndexTest_ImageSize)
			{
				++Missing;
				Note(FString::Printf(TEXT("探针 %d 该可见，但它的顶面中心投到了画面外 (%d, %d)"), Index, Pixel.X, Pixel.Y));
				continue;
			}

			const int32 PixelIndex = Pixel.Y * CSIndexTest_ImageSize + Pixel.X;
			const float ExpectedDepth = float(CSIndexTest_CameraHeight - Top);
			if (FMath::Abs(Picture.Depth[PixelIndex] - ExpectedDepth) > 0.5f)
			{
				++WrongDepth;
				Note(FString::Printf(TEXT("探针 %d（第 %d 级，缩放 %.2f）顶面深度应为 %.1f，实际 %.1f"), Index, *Lod, Probe.Scale, ExpectedDepth, Picture.Depth[PixelIndex]));
			}
			if (bCheckColor)
			{
				const FLinearColor Color = Picture.BaseColor[PixelIndex];
				if (FMath::Abs(Color.R - Probe.Random) > 0.03f || FMath::Abs(Color.G - Probe.Data0) > 0.03f || FMath::Abs(Color.B - Probe.Data1) > 0.03f)
				{
					++WrongColor;
					Note(FString::Printf(TEXT("探针 %d 的 BaseColor 应为 (%.3f, %.3f, %.3f)，实际 (%.3f, %.3f, %.3f)"),
						Index, Probe.Random, Probe.Data0, Probe.Data1, Color.R, Color.G, Color.B));
				}
			}
		}

		Test.AddInfo(FString::Printf(TEXT("[%s] %d 个探针，%d 个该可见；可见槽里 %d 个"), Stage, Probes.Num(), ExpectedVisible, LodOfProbe.Num()));
		Test.TestTrue(FString::Printf(TEXT("[%s] 判据有内容（至少有一个该可见的实例）"), Stage), ExpectedVisible > 0);
		Test.TestEqual(FString::Printf(TEXT("[%s] 该可见却没进可见槽的实例数"), Stage), Missing, 0);
		Test.TestEqual(FString::Printf(TEXT("[%s] 不该可见却进了可见槽的实例数"), Stage), Unexpected, 0);
		Test.TestEqual(FString::Printf(TEXT("[%s] 不该可见、位置上却有几何的实例数"), Stage), Ghosts, 0);
		Test.TestEqual(FString::Printf(TEXT("[%s] 顶面深度不对的实例数（位置 / 缩放 / LOD 区段 / WPO 里读到的 custom data）"), Stage), WrongDepth, 0);
		if (bCheckColor) Test.TestEqual(FString::Printf(TEXT("[%s] BaseColor 不对的实例数（逐实例随机数 / 像素里读到的 custom data）"), Stage), WrongColor, 0);
		if (!FirstProblem.IsEmpty()) Test.AddError(FString::Printf(TEXT("[%s] 第一处不符：%s"), Stage, *FirstProblem));
	}

	/** 两种布局的画面逐像素相同（深度差 ≤ 0.01 cm，颜色差 ≤ 1e-4）。 */
	void CSIndexTest_ExpectSamePicture(FAutomationTestBase& Test, const TCHAR* Stage, const FCSIndexTest_Picture& A, const FCSIndexTest_Picture& B)
	{
		float MaxDepth = 0.0f;
		const int32 DepthPixels = CSIndexTest_CountDepthDifferences(A.Depth, B.Depth, 0.01f, MaxDepth);
		Test.TestEqual(FString::Printf(TEXT("[%s] 深度不同的像素数（最大差 %.4f cm）"), Stage, MaxDepth), DepthPixels, 0);
		if (A.BaseColor.Num() > 0 || B.BaseColor.Num() > 0)
		{
			float MaxColor = 0.0f;
			const int32 ColorPixels = CSIndexTest_CountColorDifferences(A.BaseColor, B.BaseColor, 1e-4f, MaxColor);
			Test.TestEqual(FString::Printf(TEXT("[%s] BaseColor 不同的像素数（最大差 %.5f）"), Stage, MaxColor), ColorPixels, 0);
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshVisibleIndexPictureAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.VisibleSourceIndex.Picture",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * CPU 实例源、三级 LOD、带簇、有一圈实例在视锥外：位置 / 缩放 / LOD 区段 / 剔除压缩这一条链。
 *
 * 484 个实例排成 22 × 22 的格子，俯拍相机只看得见中间 16 × 16；三档缩放让一张图里三级 LOD 都有人。
 * 顺序：新布局（对 GPU 回读的表）→ 旧布局（逐像素对新布局）→ 再回新布局（同一个场景拍两次逐像素相同，
 * 否则上面那些"相同"什么都没证明）→ 追加一个很远的实例让 Morton 行序整体重排（可见的**稳定槽**集合不许变）→
 * 按句柄删掉画面中间的一个实例（末尾实例被换进空位，别的实例一个都不许动）。
 */
bool FCSGpuInstancedMeshVisibleIndexPictureAutomationTest::RunTest(const FString& Parameters)
{
	FCSIndexTest_LayoutScope Layout;
	if (!TestNotNull(TEXT("r.CSGpuInstanced.VisibleSourceIndex 存在"), Layout.CVar)) return false;
	if (!TestTrue(TEXT("切到可见源行号布局"), Layout.Set(true))) return false;

	FCSIndexTest_Scene Scene;
	if (!Scene.CreateWorld(*this)) return false;
	UStaticMesh* Mesh = CSIndexTest_MakeLodBoxMesh();
	if (!TestNotNull(TEXT("三级 LOD 的盒子网格"), Mesh)) return false;
	// 引擎默认材质为所有用途都编好了着色器，不会在测试里等一轮材质编译。
	if (!Scene.Spawn(*this, Mesh, UMaterial::GetDefaultMaterial(MD_Surface))) return false;
	UCSGpuInstancedMeshComponent* Instanced = Scene.Instanced;

	// 22 × 22，间距 250：坐标 ±125、±375 … ±2625。视锥（90°）在这个高度上的半宽约 1950，|坐标| ≤ 1875 的在里面，
	// 下一圈 2125 连包围球都够不着视锥面 —— 离边界都有一百多厘米，不赌浮点。
	constexpr int32 Side = 22;
	constexpr double Spacing = 250.0;
	constexpr double VisibleLimit = 1900.0;
	const float Scales[3] = { 1.0f, 0.6f, 0.35f };

	TArray<FCSIndexTest_Probe> Probes;
	TArray<FTransform> Transforms;
	for (int32 Y = 0; Y < Side; ++Y)
	{
		for (int32 X = 0; X < Side; ++X)
		{
			FCSIndexTest_Probe Probe;
			Probe.Location = FVector((double(X) - (Side - 1) * 0.5) * Spacing, (double(Y) - (Side - 1) * 0.5) * Spacing, 0.0);
			Probe.Scale = Scales[(X + 2 * Y) % 3];
			Probe.bExpectVisible = FMath::Abs(Probe.Location.X) < VisibleLimit && FMath::Abs(Probe.Location.Y) < VisibleLimit;
			Probes.Add(Probe);
			Transforms.Add(FTransform(FRotator::ZeroRotator, Probe.Location, FVector(Probe.Scale)));
		}
	}
	Instanced->SetInstances(Transforms);
	if (!TestTrue(TEXT("常驻缓冲按可见源行号布局建的"), Instanced->GetGpuLayout().IsValid() && Instanced->GetGpuLayout().bVisibleSourceIndex)) return false;
	// 只增不删的内容槽号 == 下标（StableIdentity 守着），所以探针下标就是稳定槽。
	TestTrue(TEXT("探针下标就是稳定槽"), Instanced->GetInstanceHandle(Probes.Num() - 1).Slot == Probes.Num() - 1);

	// ---------------------------------------------------------------------------
	// 新布局：画面对 GPU 回读的表
	// ---------------------------------------------------------------------------
	const FCSIndexTest_Picture IndexPicture = CSIndexTest_Capture(Scene.World, Scene.Origin, /*bWithBaseColor*/ false);
	TMap<int32, int32> IndexLods;
	TArray<int32> IndexCounts;
	if (!CSIndexTest_ReadVisibleLods(*this, TEXT("新布局"), Instanced, /*bCpuSource*/ true, Probes.Num(), IndexLods, IndexCounts)) return false;
	CSIndexTest_ExpectPicture(*this, TEXT("新布局"), IndexPicture, Probes, IndexLods, /*bCheckColor*/ false);
	if (TestEqual(TEXT("[新布局] 三级 LOD 各有一张表"), IndexCounts.Num(), CSIndexTest_NumLods))
	{
		AddInfo(FString::Printf(TEXT("[新布局] 逐 LOD 可见数 %d / %d / %d"), IndexCounts[0], IndexCounts[1], IndexCounts[2]));
		// 判据要有内容：只有一级有人的话，"LOD 区段错位"这种错这张图照不出来。
		for (int32 Lod = 0; Lod < CSIndexTest_NumLods; ++Lod) TestTrue(FString::Printf(TEXT("[新布局] 第 %d 级有实例"), Lod), IndexCounts[Lod] > 0);
	}
	{
		TArray<int32> Counts;
		TestTrue(TEXT("[新布局] 压缩游标读得回来"), Instanced->DebugReadVisibleCountsSync(Counts));
		TestTrue(TEXT("[新布局] 行号表的长度就是压缩游标"), Counts == IndexCounts);
	}

	// ---------------------------------------------------------------------------
	// 剔除的建图方式来回切（r.CSGpuInstanced.LeanCullSetup，§6.4）：画面与逐 LOD 可见数一个都不许变。
	// 两种建图方式登记的常驻流不同、实例数的来路不同（计数 buffer / uniform），来回切的那一帧最容易露出
	// "这条流上一帧是另一种方式留下的访问状态"之类的问题。
	// ---------------------------------------------------------------------------
	if (IConsoleVariable* LeanSetup = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CSGpuInstanced.LeanCullSetup")))
	{
		const int32 SavedLeanSetup = LeanSetup->GetInt();
		// 位标志：1 只登记用到的流、2 实例数走 uniform；0 = 旧做法，3 = 全开（默认）。
		for (const int32 Mode : { 0, 3, 0, 3, 1, 2, 3 })
		{
			LeanSetup->Set(Mode, ECVF_SetByCode);
			const FString Stage = FString::Printf(TEXT("建图方式 %d"), Mode);
			CSIndexTest_ExpectSamePicture(*this, *Stage, CSIndexTest_Capture(Scene.World, Scene.Origin, false), IndexPicture);
			TArray<int32> Counts;
			TestTrue(FString::Printf(TEXT("[%s] 可见数读得回来"), *Stage), Instanced->DebugReadVisibleCountsSync(Counts));
			TestTrue(FString::Printf(TEXT("[%s] 逐 LOD 可见数不变"), *Stage), Counts == IndexCounts);
		}
		LeanSetup->Set(SavedLeanSetup, ECVF_SetByCode);
	}
	else
	{
		AddError(TEXT("r.CSGpuInstanced.LeanCullSetup 不存在"));
	}

	// ---------------------------------------------------------------------------
	// 旧布局：同一个场景，逐像素对新布局
	// ---------------------------------------------------------------------------
	if (!TestTrue(TEXT("切回旧布局"), Layout.Set(false))) return false;
	TestFalse(TEXT("[旧布局] CVar 一改，已注册的组件当场换了布局"), Instanced->GetGpuLayout().bVisibleSourceIndex);
	{
		const FCSIndexTest_Picture LegacyPicture = CSIndexTest_Capture(Scene.World, Scene.Origin, false);
		CSIndexTest_ExpectSamePicture(*this, TEXT("旧布局 对 新布局"), LegacyPicture, IndexPicture);
		TArray<int32> Counts;
		TestTrue(TEXT("[旧布局] 压缩游标读得回来"), Instanced->DebugReadVisibleCountsSync(Counts));
		TestTrue(TEXT("[旧布局] 逐 LOD 可见数与新布局相同"), Counts == IndexCounts);
		TArray<TArray<int32>> Rows;
		TestFalse(TEXT("[旧布局] 没有可见源行号可读"), Instanced->DebugReadVisibleSourceIndicesSync(Rows));
	}

	// ---------------------------------------------------------------------------
	// 再回新布局：同一个场景拍两次逐像素相同（这种拍法的噪声底是零）
	// ---------------------------------------------------------------------------
	if (!TestTrue(TEXT("再切到可见源行号布局"), Layout.Set(true))) return false;
	TestTrue(TEXT("[重拍] 布局换回来了"), Instanced->GetGpuLayout().bVisibleSourceIndex);
	CSIndexTest_ExpectSamePicture(*this, TEXT("新布局重拍"), CSIndexTest_Capture(Scene.World, Scene.Origin, false), IndexPicture);

	// ---------------------------------------------------------------------------
	// Morton 重排：追加一个很远的实例，packed 行整体换序。可见槽里存的行号全变了，换成稳定槽之后一个都不许变。
	// ---------------------------------------------------------------------------
	{
		const TArray<int32> RowsBefore = Instanced->GetPackedInstanceSlots();
		FCSIndexTest_Probe Far;
		Far.Location = FVector(100.0, 400000.0, 0.0);
		Far.bExpectVisible = false;
		const int32 FarIndex = Instanced->AddInstance(FTransform(Far.Location));
		TestEqual(TEXT("[重排] 新实例的稳定槽接在后面"), Instanced->GetInstanceHandle(FarIndex).Slot, Probes.Num());
		Probes.Add(Far);

		TArray<int32> RowsAfter = Instanced->GetPackedInstanceSlots();
		RowsAfter.Remove(Probes.Num() - 1);
		TestTrue(TEXT("[重排] 既有实例的行序确实被重排了"), RowsAfter != RowsBefore);

		const FCSIndexTest_Picture Picture = CSIndexTest_Capture(Scene.World, Scene.Origin, false);
		TMap<int32, int32> Lods;
		TArray<int32> Counts;
		if (CSIndexTest_ReadVisibleLods(*this, TEXT("重排"), Instanced, true, Probes.Num(), Lods, Counts))
		{
			CSIndexTest_ExpectPicture(*this, TEXT("重排"), Picture, Probes, Lods, false);
			TestTrue(TEXT("[重排] 每个可见实例（按稳定槽认）还在原来那一级"), Lods.OrderIndependentCompareEqual(IndexLods));
		}
		CSIndexTest_ExpectSamePicture(*this, TEXT("重排前后"), Picture, IndexPicture);
	}

	// ---------------------------------------------------------------------------
	// 按句柄删掉画面中间的一个：末尾实例（很远的那个）被换进它的下标，别的实例一个都不许动。两种布局都验。
	// ---------------------------------------------------------------------------
	{
		const int32 Victim = (Side / 2) * Side + Side / 2;
		TestTrue(TEXT("[删除] 被删的实例本来看得见"), Probes[Victim].bExpectVisible && IndexLods.Contains(Victim));
		TestTrue(TEXT("[删除] 按句柄删"), Instanced->RemoveInstanceByHandle(Instanced->GetInstanceHandle(Victim)));
		Probes[Victim].bExpectVisible = false;

		const FCSIndexTest_Picture Picture = CSIndexTest_Capture(Scene.World, Scene.Origin, false);
		TMap<int32, int32> Lods;
		TArray<int32> Counts;
		if (CSIndexTest_ReadVisibleLods(*this, TEXT("删除"), Instanced, true, Probes.Num(), Lods, Counts))
		{
			CSIndexTest_ExpectPicture(*this, TEXT("删除"), Picture, Probes, Lods, false);
			TestEqual(TEXT("[删除] 可见实例少了一个"), Lods.Num(), IndexLods.Num() - 1);
		}

		if (TestTrue(TEXT("[删除] 切回旧布局"), Layout.Set(false)))
		{
			CSIndexTest_ExpectSamePicture(*this, TEXT("删除后 旧布局 对 新布局"), CSIndexTest_Capture(Scene.World, Scene.Origin, false), Picture);
		}
	}

	Scene.Host->Destroy();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCSGpuInstancedMeshVisibleIndexAttributesAutomationTest,
	"PCGPlugins.ComputeShaderGenerator.GpuInstancedMesh.VisibleSourceIndex.Attributes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter | EAutomationTestFlags::NonNullRHI)

/**
 * 逐实例属性：随机数与 custom data，三种实例源各走一遍，每一种都是"新布局对期望值、旧布局逐像素对新布局"。
 *
 *   ① packed GPU 源 + custom data：每三行藏一行（随机数 < 0）⇒ 可见槽与源行错开，custom data 取错下标就张冠李戴。
 *      旧布局是剔除 pass 把它抄进可见缓冲；新布局是顶点 / 像素着色器按源行号直接读生产者那块 buffer。
 *   ② 同一份行、不给 custom data：新布局读的是那一行零（下标钉成 0），材质读到 0 而不是别的缓冲里的东西。
 *   ③ CPU 实例数组：随机数按稳定槽取，行在常驻流里。
 *   ④ 点云源：行是剔除前现打进常驻流的，随机数按点的下标取。
 */
bool FCSGpuInstancedMeshVisibleIndexAttributesAutomationTest::RunTest(const FString& Parameters)
{
	FCSIndexTest_LayoutScope Layout;
	if (!TestNotNull(TEXT("r.CSGpuInstanced.VisibleSourceIndex 存在"), Layout.CVar)) return false;
	if (!TestTrue(TEXT("切到可见源行号布局"), Layout.Set(true))) return false;

	FCSIndexTest_Scene Scene;
	if (!Scene.CreateWorld(*this)) return false;
	UStaticMesh* Mesh = CSIndexTest_MakeLodBoxMesh();
	if (!TestNotNull(TEXT("三级 LOD 的盒子网格"), Mesh)) return false;
	UMaterial* Material = CSIndexTest_MakeProbeMaterial();
	if (!TestNotNull(TEXT("探针材质"), Material)) return false;
	if (!TestFalse(TEXT("探针材质编译通过"), Material->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform))) return false;
	if (!Scene.Spawn(*this, Mesh, Material)) return false;
	UCSGpuInstancedMeshComponent* Instanced = Scene.Instanced;
	{
		const FString Reason = Instanced->GetMaterialUndrawableReason();
		if (!TestTrue(FString::Printf(TEXT("探针材质这条路画得了（%s）"), Reason.IsEmpty() ? TEXT("可以") : *Reason), Reason.IsEmpty())) return false;
		TestTrue(TEXT("画的就是探针材质"), Instanced->GetSectionMaterial(0) == Material);
	}

	// 6 × 6，间距 500，全在画面里。缩放两档；数值按 7 / 11 的倍数打散 —— 与行号、与彼此都不同序，取错下标一定对不上。
	constexpr int32 Side = 6;
	constexpr int32 NumProbes = Side * Side;
	constexpr double Spacing = 500.0;
	TArray<FCSIndexTest_Probe> BaseProbes;
	for (int32 Index = 0; Index < NumProbes; ++Index)
	{
		FCSIndexTest_Probe Probe;
		Probe.Location = FVector((double(Index % Side) - (Side - 1) * 0.5) * Spacing, (double(Index / Side) - (Side - 1) * 0.5) * Spacing, 0.0);
		Probe.Scale = (Index % 2 == 0) ? 1.0f : 0.5f;
		Probe.Random = 0.15f + 0.7f * float(Index) / float(NumProbes - 1);
		Probe.Data0 = 0.1f + 0.8f * float((Index * 7) % NumProbes) / float(NumProbes - 1);
		Probe.Data1 = 0.1f + 0.8f * float((Index * 11) % NumProbes) / float(NumProbes - 1);
		BaseProbes.Add(Probe);
	}

	// 一个阶段 = 新布局拍一张对期望值，旧布局拍一张逐像素对它。
	auto RunStage = [this, &Layout, &Scene, Instanced](const TCHAR* Stage, const TArray<FCSIndexTest_Probe>& Probes, bool bCpuSource)
	{
		if (!TestTrue(FString::Printf(TEXT("[%s] 切到可见源行号布局"), Stage), Layout.Set(true))) return;
		TestTrue(FString::Printf(TEXT("[%s] 常驻缓冲按可见源行号布局建的"), Stage), Instanced->GetGpuLayout().IsValid() && Instanced->GetGpuLayout().bVisibleSourceIndex);
		const FCSIndexTest_Picture IndexPicture = CSIndexTest_Capture(Scene.World, Scene.Origin, /*bWithBaseColor*/ true);
		TMap<int32, int32> Lods;
		TArray<int32> IndexCounts;
		if (!CSIndexTest_ReadVisibleLods(*this, Stage, Instanced, bCpuSource, Probes.Num(), Lods, IndexCounts)) return;
		CSIndexTest_ExpectPicture(*this, Stage, IndexPicture, Probes, Lods, /*bCheckColor*/ true);

		const FString LegacyStage = FString::Printf(TEXT("%s：旧布局 对 新布局"), Stage);
		if (!TestTrue(FString::Printf(TEXT("[%s] 切回旧布局"), Stage), Layout.Set(false))) return;
		TestFalse(FString::Printf(TEXT("[%s] 旧布局生效"), Stage), Instanced->GetGpuLayout().bVisibleSourceIndex);
		CSIndexTest_ExpectSamePicture(*this, *LegacyStage, CSIndexTest_Capture(Scene.World, Scene.Origin, true), IndexPicture);
		TArray<int32> Counts;
		TestTrue(FString::Printf(TEXT("[%s] 旧布局的压缩游标读得回来"), *LegacyStage), Instanced->DebugReadVisibleCountsSync(Counts));
		TestTrue(FString::Printf(TEXT("[%s] 逐 LOD 可见数相同"), *LegacyStage), Counts == IndexCounts);
	};

	const FBox SourceBounds(FVector(-2000.0, -2000.0, -100.0), FVector(2000.0, 2000.0, 300.0));
	// 容量比活着的多四行：多出来的行不初始化，剔除止步于计数器、从不读它们。
	constexpr uint32 Capacity = uint32(NumProbes) + 4u;

	// ---------------------------------------------------------------------------
	// ① packed GPU 源 + custom data，每三行藏一行
	// ---------------------------------------------------------------------------
	TArray<FCSIndexTest_Probe> GpuProbes = BaseProbes;
	TArray<FVector4f> Rows;
	TArray<float> CustomData;
	for (int32 Index = 0; Index < NumProbes; ++Index)
	{
		FCSIndexTest_Probe& Probe = GpuProbes[Index];
		Probe.bExpectVisible = Index % 3 != 1;
		CSIndexTest_AppendRow(Rows, Probe.Location, Probe.Scale, Probe.bExpectVisible ? Probe.Random : -1.0f);
		CustomData.Add(Probe.Data0);
		CustomData.Add(Probe.Data1);
	}
	static_assert(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS == 2, "探针材质与这里的 custom data 都按两个 float 写的");
	Instanced->SetInstanceSourceGPU(CSIndexTest_MakeGpuSource(Rows, &CustomData, Capacity, SourceBounds));
	RunStage(TEXT("GPU 源 + custom data"), GpuProbes, /*bCpuSource*/ false);

	// ---------------------------------------------------------------------------
	// ② 同一份行，不给 custom data
	// ---------------------------------------------------------------------------
	{
		TArray<FCSIndexTest_Probe> Probes = GpuProbes;
		for (FCSIndexTest_Probe& Probe : Probes) Probe.Data0 = Probe.Data1 = 0.0f;
		Instanced->SetInstanceSourceGPU(CSIndexTest_MakeGpuSource(Rows, nullptr, Capacity, SourceBounds));
		RunStage(TEXT("GPU 源、没有 custom data"), Probes, false);
	}

	// ---------------------------------------------------------------------------
	// ③ CPU 实例数组：随机数按稳定槽取
	// ---------------------------------------------------------------------------
	{
		Instanced->ClearInstanceSourceGPU();
		TArray<FTransform> Transforms;
		for (const FCSIndexTest_Probe& Probe : BaseProbes) Transforms.Add(FTransform(FRotator::ZeroRotator, Probe.Location, FVector(Probe.Scale)));
		Instanced->SetInstances(Transforms);

		TArray<FCSIndexTest_Probe> Probes = BaseProbes;
		const TArray<int32>& RowSlots = Instanced->GetPackedInstanceSlots();
		const TArray<FVector4f>& Packed = Instanced->GetPackedInstances();
		TSet<float> Distinct;
		bool bAllFound = RowSlots.Num() == NumProbes;
		for (int32 Index = 0; Index < NumProbes; ++Index)
		{
			const int32 Row = RowSlots.Find(Index);
			if (Row == INDEX_NONE) { bAllFound = false; continue; }
			Probes[Index].Random = Packed[Row * CS_GPU_INSTANCED_ROW_FLOAT4S + 3].W;
			Probes[Index].Data0 = Probes[Index].Data1 = 0.0f;
			Distinct.Add(Probes[Index].Random);
		}
		TestTrue(TEXT("[CPU 源] 每个稳定槽在行表里都找得到"), bAllFound);
		TestTrue(FString::Printf(TEXT("[CPU 源] 逐实例随机数各不相同（%d 种 / %d 个）"), Distinct.Num(), NumProbes), Distinct.Num() == NumProbes);
		RunStage(TEXT("CPU 源"), Probes, /*bCpuSource*/ true);
	}

	// ---------------------------------------------------------------------------
	// ④ 点云源：前 30 个点活着，后 6 个在计数器之外
	// ---------------------------------------------------------------------------
	{
		constexpr uint32 LivePoints = 30u;
		constexpr float PointScale = 0.8f;
		TArray<FVector> WorldPoints;
		TArray<FCSIndexTest_Probe> Probes = BaseProbes;
		for (int32 Index = 0; Index < NumProbes; ++Index)
		{
			WorldPoints.Add(Scene.Origin + Probes[Index].Location);
			Probes[Index].Scale = PointScale;
			Probes[Index].Random = CSIndexTest_PointRandom(uint32(Index));
			Probes[Index].Data0 = Probes[Index].Data1 = 0.0f;
			Probes[Index].bExpectVisible = uint32(Index) < LivePoints;
		}
		Instanced->SetInstanceSourceFromPoints(CSIndexTest_MakePointSource(WorldPoints, LivePoints, PointScale));
		RunStage(TEXT("点云源"), Probes, /*bCpuSource*/ false);
	}

	Scene.Host->Destroy();
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

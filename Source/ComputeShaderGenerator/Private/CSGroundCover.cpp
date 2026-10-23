#include "CSGroundCover.h"

#include "Algo/Reverse.h"

#include "CSGpuInstancedMeshComponent.h"   // CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS
#include "CSGpuMeshTypes.h"
#include "CSGroundShaperField.h"
#include "CSMesh.h"                    // UCSMesh::CountedBlockingFlush —— 阻塞刷新的唯一计数入口
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "ShaderParameterStruct.h"
#include "TextureResource.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGroundCover, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSCover_ 前缀（与 CSStairs_ / CSShaperSteps_ /
// CSGround_ 必须都不同，否则 unity blob 里同名符号打架，而报错位置会指向一个跟改动无关的文件）。

constexpr int32 CSCover_GroupSizeX = 8;
constexpr int32 CSCover_GroupSizeY = 8;

class FCSGroundCoverScatterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSGroundCoverScatterCS);
	SHADER_USE_PARAMETER_STRUCT(FCSGroundCoverScatterCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		// 名字必须与 CSGroundShaperField.ush 里的声明逐字相同：这是与地面位移 pass / 石阶扫描
		// 共享的那一份高度场输入，改名等于把绑定悄悄拆掉（不报错，只是永远读成平地）。
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, GroundShaperParams)
		SHADER_PARAMETER(uint32, GroundShaperCount)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CoverGroundColors)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWCoverInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWCoverCounter)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float>, RWCoverCustomData)

		SHADER_PARAMETER(FMatrix44f, CoverWorldToComponent)
		SHADER_PARAMETER(FVector2f, CoverGridOriginXY)
		SHADER_PARAMETER(float, CoverCellSize)
		SHADER_PARAMETER(FUintVector2, CoverGridDims)
		SHADER_PARAMETER(FIntPoint, CoverCellOffset)
		SHADER_PARAMETER(FUintVector2, CoverGlobalDims)
		SHADER_PARAMETER(FVector2f, CoverGroundOriginXY)
		SHADER_PARAMETER(float, CoverGroundCellSize)
		SHADER_PARAMETER(FUintVector2, CoverGroundVerts)
		SHADER_PARAMETER(float, CoverGroundBaseZ)
		SHADER_PARAMETER(FVector4f, CoverMaskChannel)
		SHADER_PARAMETER(float, CoverMaskStart)
		SHADER_PARAMETER(float, CoverMaskEnd)
		SHADER_PARAMETER(float, CoverMaskShorten)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D<float>, CoverGroundMask)
		SHADER_PARAMETER(uint32, CoverUseGroundMask)
		SHADER_PARAMETER(FVector2f, CoverMaskWarp)
		SHADER_PARAMETER(float, CoverChance)
		SHADER_PARAMETER(float, CoverJitter)
		SHADER_PARAMETER(float, CoverMinSlopeCos)
		SHADER_PARAMETER(float, CoverSink)
		SHADER_PARAMETER(float, CoverHeightOffset)
		SHADER_PARAMETER(FVector2f, CoverScaleRange)
		SHADER_PARAMETER(float, CoverHeightJitter)
		SHADER_PARAMETER(float, CoverLeanMaxRad)
		SHADER_PARAMETER(float, CoverAlignToNormal)
		SHADER_PARAMETER(float, CoverClumpSize)
		SHADER_PARAMETER(float, CoverClumpRadialChance)
		SHADER_PARAMETER(float, CoverClumpAlignment)
		SHADER_PARAMETER(float, CoverScaleClumpShare)
		SHADER_PARAMETER(FVector2f, CoverBendRange)
		SHADER_PARAMETER(float, CoverRadialBendScale)
		SHADER_PARAMETER(uint32, CoverSeed)
		SHADER_PARAMETER(uint32, CoverSalt)
		SHADER_PARAMETER(uint32, CoverMaxInstances)
		SHADER_PARAMETER(FVector3f, CoverBaseSphereCentre)
		SHADER_PARAMETER(float, CoverBaseSphereRadius)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, CoverBuildings)
		SHADER_PARAMETER(uint32, CoverBuildingCount)
		SHADER_PARAMETER(float, CoverRingInner)
		SHADER_PARAMETER(float, CoverRingWidth)
		SHADER_PARAMETER(float, CoverRingNoiseCell)
		SHADER_PARAMETER(float, CoverRingNoiseThreshold)
		SHADER_PARAMETER(float, CoverRingMinScale)
		SHADER_PARAMETER(float, CoverOverlayChance)
		SHADER_PARAMETER(float, CoverPatchSize)
		SHADER_PARAMETER(float, CoverPatchCoverage)
		SHADER_PARAMETER(float, CoverPatchSoftness)
		SHADER_PARAMETER(float, CoverPatchSparse)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSCover_GroupSizeX);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_Y"), CSCover_GroupSizeY);
		OutEnvironment.SetDefine(TEXT("CSCOVER_BUILDING_STRIDE"), CSGroundCover::BuildingStride);
		OutEnvironment.SetDefine(TEXT("CSCOVER_BUILDING_MAX_VERTS"), CSGroundCover::BuildingMaxVerts);
		// custom data 的步长与行 stride 由 .usf 自己 include CSGpuSharedLayout.ush 取得（与 C++ 是同一份
		// #define），不再从这里注入 —— 注入会和头里的定义撞成重复定义。
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSGroundCoverScatterCS, "/Plugin/PCGPlugins/Shaders/Private/CSGroundCover.usf", "ScatterGroundCoverCS", SF_Compute);
}

namespace CSGroundCover
{
bool MakeGridForDensity(
	const FBox2D& WorldRectXY, float DensityPerSqM, int32 MaxCells,
	FVector2f& OutOriginXY, float& OutCellSize, FIntPoint& OutGridDims)
{
	OutOriginXY = FVector2f(float(WorldRectXY.Min.X), float(WorldRectXY.Min.Y));
	const FVector2D Span = WorldRectXY.GetSize();
	const int32 CellBudget = FMath::Max(MaxCells, 1);

	// 密度是"每平方米几株"，格是正方形 ⇒ 格距 = 100 cm / √密度。密度 ≤ 0 视作"这一支关掉"：
	// 给一个 1×1 的退化格并把格距撑满，配合调用方把 Chance 归零，一趟只写 counter 的 0。
	const float Density = FMath::Max(DensityPerSqM, 0.0f);
	if (Density <= UE_KINDA_SMALL_NUMBER || Span.X <= 0.0 || Span.Y <= 0.0)
	{
		OutCellSize = FMath::Max(float(FMath::Max(Span.X, Span.Y)), 1.0f);
		OutGridDims = FIntPoint(1, 1);
		return false;
	}

	float Cell = 100.0f / FMath::Sqrt(Density);
	auto DimsFor = [&Span](float InCell)
	{
		return FIntPoint(
			FMath::Max(FMath::CeilToInt32(Span.X / InCell), 1),
			FMath::Max(FMath::CeilToInt32(Span.Y / InCell), 1));
	};
	FIntPoint Dims = DimsFor(Cell);

	// 退让：格数超预算就把格距按 √(cells / budget) 放大。一次算出来还可能因为 ceil 差一格，
	// 所以再收敛几轮 —— 循环有硬上限，不会因为浮点抖动卡住。
	bool bClamped = false;
	for (int32 Guard = 0; Guard < 8 && int64(Dims.X) * int64(Dims.Y) > int64(CellBudget); ++Guard)
	{
		const double Over = double(Dims.X) * double(Dims.Y) / double(CellBudget);
		Cell *= float(FMath::Sqrt(Over)) * 1.001f;   // 乘一点点余量，避免 ceil 之后又刚好超
		Dims = DimsFor(Cell);
		bClamped = true;
	}

	OutCellSize = Cell;
	OutGridDims = Dims;
	return bClamped;
}

bool EnsureBuffers(FCoverBuffers& Buffers, uint32 Capacity)
{
	const uint32 Want = FMath::Max(Align(Capacity, 64u), 64u);
	if (Buffers.IsValid() && Buffers.Capacity >= Want) return true;   // 交互期走的就是这条：零 enqueue

	// 走到这里一定阻塞一次（AllocatePooledBuffer 必须在渲染线程，而结果要回到游戏线程侧）。
	// 固定容量的意义就是让这一趟只发生在注册 / 加载 / 改配置时，散布路径永远碰不到它。
	FCoverBuffers Work = Buffers;
	ENQUEUE_RENDER_COMMAND(CSGroundCoverEnsureBuffers)(
		[&Work, Want](FRHICommandListImmediate&)
		{
			// typed buffer（不是 structured）：实例组件的剔除 pass 用 Buffer<float4> / Buffer<uint> 视图。
			Work.PackedInstances = AllocatePooledBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), Want * CS_GPU_INSTANCED_ROW_FLOAT4S), TEXT("CSGroundCover.PackedInstances"));
			Work.Counter = AllocatePooledBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("CSGroundCover.Counter"));
			// custom data **恒分配**：剔除 pass 的 UAV 是无条件绑定的（RDG 不接受空参数），
			// 而且组件那边只看 `CustomData.IsValid()` 决定要不要把 SRV 交给顶点工厂。
			Work.CustomData = AllocatePooledBuffer(
				FRDGBufferDesc::CreateBufferDesc(sizeof(float), Want * uint32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS)),
				TEXT("CSGroundCover.CustomData"));
			Work.Capacity = Want;
		});
	// **必须走计数通道**：裸 FlushRenderingCommands 不进 CSMesh 的计数器，"交互期零阻塞"的
	// 断言就看不见它 —— 而固定容量这个设计的全部意义恰恰是"让交互期碰不到这一趟"。
	UCSMesh::CountedBlockingFlush();

	Buffers = MoveTemp(Work);
	return Buffers.IsValid();
}

void PackBuildingPolygons(TConstArrayView<TArray<FVector2D>> Polygons, TArray<FVector4f>& OutPacked)
{
	OutPacked.Reset();
	OutPacked.Reserve(Polygons.Num() * BuildingStride);
	for (const TArray<FVector2D>& Polygon : Polygons)
	{
		if (Polygon.Num() < 3) continue;
		FBox2D Box(ForceInit);
		for (const FVector2D& V : Polygon) Box += V;

		// 顶点太多就退成 AABB 矩形：保守（环带只会更大），而且房子的外皮本来就只有几条边。
		TArray<FVector2D, TInlineAllocator<BuildingMaxVerts>> Verts;
		if (Polygon.Num() > BuildingMaxVerts) Verts = { Box.Min, FVector2D(Box.Max.X, Box.Min.Y), Box.Max, FVector2D(Box.Min.X, Box.Max.Y) };
		else Verts.Append(Polygon);

		// kernel 按"逆时针 ⇒ 右手侧是外面"判内外：顺时针进来会把里外整个颠倒，灌木长进屋里。
		double Area2 = 0.0;
		for (int32 I = 0; I < Verts.Num(); ++I) Area2 += FVector2D::CrossProduct(Verts[I], Verts[(I + 1) % Verts.Num()]);
		if (Area2 < 0.0) Algo::Reverse(Verts);

		OutPacked.Add(FVector4f(float(Box.Min.X), float(Box.Min.Y), float(Box.Max.X), float(Box.Max.Y)));
		OutPacked.Add(FVector4f(float(Verts.Num()), 0.0f, 0.0f, 0.0f));
		for (int32 Pair = 0; Pair < BuildingMaxVerts / 2; ++Pair)
		{
			const int32 A = Pair * 2;
			const FVector2D VA = Verts.IsValidIndex(A) ? Verts[A] : FVector2D::ZeroVector;
			const FVector2D VB = Verts.IsValidIndex(A + 1) ? Verts[A + 1] : FVector2D::ZeroVector;
			OutPacked.Add(FVector4f(float(VA.X), float(VA.Y), float(VB.X), float(VB.Y)));
		}
	}
}

bool Scatter(
	const FCSMeshResidentRef& GroundResident,
	const TArray<FCoverBuffers>& Buffers,
	const TArray<FScatterParams>& Params,
	const TArray<FVector4f>& ShaperParams,
	const TArray<FVector4f>& BuildingPolygons,
	UTextureRenderTarget2D* GroundMask)
{
	if (!GroundResident.IsValid()) return false;
	// 长度不同 ⇒ 整趟拒绝。错位的症状是"花长成草的密度"，没有任何报错 —— 宁可一株都不长。
	if (Buffers.Num() != Params.Num() || Buffers.IsEmpty()) return false;
	for (const FCoverBuffers& One : Buffers)
	{
		if (!One.IsValid()) return false;
	}

	// 空 palette 也要跑：塑形物被删光时正是"全 0 高度场"把地被推回平地的那一趟。
	TArray<FVector4f> UploadParams = ShaperParams;
	const int32 ShaperCount = CSGroundShaperField::NumShapers(UploadParams);   // 前缀里读：池子跟在头后面，不能再按长度除
	if (UploadParams.IsEmpty()) UploadParams.Add(FVector4f::Zero());   // 结构化 buffer 不能是 0 长度

	TArray<FVector4f> UploadBuildings = BuildingPolygons;
	const int32 BuildingCount = UploadBuildings.Num() / BuildingStride;
	if (UploadBuildings.IsEmpty()) UploadBuildings.Add(FVector4f::Zero());   // 同上：一栋都没有也要绑一个

	// 地面遮罩的资源指针在游戏线程取、渲染线程用（渲染目标的标准交接方式）。它由地面 actor 在同一条
	// 渲染命令 FIFO 里先写好，所以这里读到的恒是这一笔之后的遮罩。
	FTextureRenderTargetResource* GroundMaskResource = GroundMask ? GroundMask->GameThread_GetRenderTargetResource() : nullptr;

	ENQUEUE_RENDER_COMMAND(CSGroundCoverScatter)(
		[Resident = GroundResident, Work = Buffers, All = Params, UploadParams, ShaperCount, UploadBuildings, BuildingCount, GroundMaskResource](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGroundCover.Scatter"));

			// 地面遮罩；没有就给一张清零的 1×1 占位（参数结构里的 SRV 不能空着，kernel 在 Use = 0 时不读它，
			// 但没被写过的 RDG 贴图没有生产者，照样要清）。
			FRHITexture* GroundMaskRHI = GroundMaskResource ? GroundMaskResource->GetRenderTargetTexture() : nullptr;
			FRDGTextureRef GroundMaskTexture = nullptr;
			if (GroundMaskRHI)
			{
				GroundMaskTexture = RegisterExternalTexture(GraphBuilder, GroundMaskRHI, TEXT("CSGround.GroundMask"));
				// 地面材质在基础 pass 里还要采它：读完留在 SRV（RDG 的默认收尾也是这个，写明白免得被人改掉）。
				GraphBuilder.SetTextureAccessFinal(GroundMaskTexture, ERHIAccess::SRVMask);
			}
			else
			{
				GroundMaskTexture = GraphBuilder.CreateTexture(
					FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV),
					TEXT("CSGroundCover.NoGroundMask"));
				AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(GroundMaskTexture), 0.0f);
			}
			const FRDGTextureSRVRef GroundMaskSRV = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::Create(GroundMaskTexture));

			TArray<FRDGBufferRef> PackedRefs;
			TArray<FRDGBufferRef> CounterRefs;
			TArray<FRDGBufferRef> CustomRefs;
			TArray<FRDGBufferUAVRef> PackedUAVs;
			TArray<FRDGBufferUAVRef> CounterUAVs;
			TArray<FRDGBufferUAVRef> CustomUAVs;
			PackedRefs.Reserve(Work.Num());
			CounterRefs.Reserve(Work.Num());
			PackedUAVs.Reserve(Work.Num());
			CounterUAVs.Reserve(Work.Num());

			for (const FCoverBuffers& One : Work)
			{
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(One.PackedInstances, TEXT("CSGroundCover.PackedInstances"));
				FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(One.Counter, TEXT("CSGroundCover.Counter"));
				FRDGBufferRef CustomRef = GraphBuilder.RegisterExternalBuffer(One.CustomData, TEXT("CSGroundCover.CustomData"));
				PackedRefs.Add(PackedRef);
				CounterRefs.Add(CounterRef);
				CustomRefs.Add(CustomRef);
				CustomUAVs.Add(GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CustomRef, PF_R32_FLOAT)));
				PackedUAVs.Add(GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F)));
				CounterUAVs.Add(GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT)));
				// 组件永远不会替你清 counter：不先清零，每次重扫都在上一趟的值上继续叠，表现是
				// 草越画越少（槽位一路涨过容量后全被静默丢弃）。清零同时是**关掉一个物种的
				// 唯一路径** —— Chance = 0 时 kernel 一个字节都不写，只有清零让上一趟真的消失。
				AddClearUAVPass(GraphBuilder, CounterUAVs.Last(), 0u);
			}

			CSHelper::FRDGStructuredBufferRefs ShaperRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, UploadParams, TEXT("CSGroundCover.ShaperParams"), false, true);
			CSHelper::FRDGStructuredBufferRefs BuildingRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, UploadBuildings, TEXT("CSGroundCover.Buildings"), false, true);
			if (!ShaperRefs.SRV || !BuildingRefs.SRV)
			{
				GraphBuilder.Execute();
				return;
			}

			{
				// 地面网格的常驻流：进出都走 mesh 层自己的入口，访问状态由 ~FCSMeshRenderThreadEdit
				// 恢复。手工写流再手工恢复是同一条规则的第二份拷贝，而漂掉的那份不报错 ——
				// 留在 RDG 默认 epilogue（SRVMask）的流对索引 / indirect 用途是非法的，只会安静地不画。
				FCSMeshRenderThreadEdit Edit(GraphBuilder, *Resident);
				FRDGBufferRef Colors = Edit->Colors();
				if (Colors)
				{
					FRDGBufferSRVRef ColorSRV = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(Colors, PF_R32_UINT));
					TShaderMapRef<FCSGroundCoverScatterCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));

					for (int32 Index = 0; Index < All.Num(); ++Index)
					{
						const FScatterParams& P = All[Index];
						if (P.GridDims.X <= 0 || P.GridDims.Y <= 0 || P.CellSize <= 0.0f) continue;
						// Chance ≤ 0 的物种连 dispatch 都不发：counter 已经清零了，"关掉"这件事
						// 到此已经完成，再跑一趟只是白烧几万个必然早退的线程。
						if (P.Chance <= 0.0f) continue;

						FCSGroundCoverScatterCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSGroundCoverScatterCS::FParameters>();
						PassParams->GroundShaperParams = ShaperRefs.SRV;
						PassParams->GroundShaperCount = uint32(FMath::Max(ShaperCount, 0));
						PassParams->CoverGroundColors = ColorSRV;
						PassParams->RWCoverInstances = PackedUAVs[Index];
						PassParams->RWCoverCounter = CounterUAVs[Index];
						PassParams->RWCoverCustomData = CustomUAVs[Index];
						PassParams->CoverWorldToComponent = P.WorldToComponent;
						PassParams->CoverGridOriginXY = P.GridOriginXY;
						PassParams->CoverCellSize = P.CellSize;
						PassParams->CoverGridDims = FUintVector2(uint32(P.GridDims.X), uint32(P.GridDims.Y));
						// 窗口散布（近处单根草）：随机源用整张网格里的格号，窗口挪动时同一格的草不变。
						// 没给整张网格的格数 = 整张网格散布本身（偏移 0、格数同 GridDims），与改动前逐位相同。
						PassParams->CoverCellOffset = P.CellOffset;
						PassParams->CoverGlobalDims = (P.GlobalDims.X > 0 && P.GlobalDims.Y > 0)
							? FUintVector2(uint32(P.GlobalDims.X), uint32(P.GlobalDims.Y))
							: FUintVector2(uint32(P.GridDims.X), uint32(P.GridDims.Y));
						PassParams->CoverGroundOriginXY = P.GroundOriginXY;
						PassParams->CoverGroundCellSize = P.GroundCellSize;
						PassParams->CoverGroundVerts = FUintVector2(uint32(FMath::Max(P.GroundVerts.X, 0)), uint32(FMath::Max(P.GroundVerts.Y, 0)));
						PassParams->CoverGroundBaseZ = P.GroundBaseZ;
						PassParams->CoverMaskChannel = P.MaskChannel;
						PassParams->CoverMaskStart = P.MaskStart;
						// End 不许 ≤ Start：smoothstep 在那种参数下退化成阶跃（甚至反向），
						// 表现是"路边一刀切"或"整片草消失"，而两个数各自看起来都正常。
						PassParams->CoverMaskEnd = FMath::Max(P.MaskEnd, P.MaskStart + 1e-3f);
						PassParams->CoverMaskShorten = FMath::Clamp(P.MaskShorten, 0.0f, 1.0f);
						PassParams->CoverGroundMask = GroundMaskSRV;
						// 没传地面遮罩时退回色流：读占位贴图的话全是 0，路上照样长满草，而且不报错。
						PassParams->CoverUseGroundMask = (P.bUseGroundMask && GroundMaskRHI) ? 1u : 0u;
						PassParams->CoverMaskWarp = FVector2f(FMath::Max(P.MaskWarpStrength, 0.0f), 1.0f / FMath::Max(P.MaskWarpWavelength, 1.0f));
						PassParams->CoverChance = FMath::Clamp(P.Chance, 0.0f, 1.0f);
						PassParams->CoverJitter = FMath::Clamp(P.Jitter, 0.0f, 1.0f);
						PassParams->CoverMinSlopeCos = FMath::Clamp(P.MinSlopeCos, 0.0f, 1.0f);
						PassParams->CoverSink = P.Sink;
						PassParams->CoverHeightOffset = P.HeightOffset;
						// 下限不许超过上限：lerp 照样算，但结果是"最大的反而最小"，一个断言都不会红，
						// 只在画面上表现为尺寸分布反着来（同石子那条）。
						PassParams->CoverScaleRange = FVector2f(
							FMath::Max(P.ScaleRange.X, 0.0f),
							FMath::Max(P.ScaleRange.Y, FMath::Max(P.ScaleRange.X, 0.0f)));
						PassParams->CoverHeightJitter = FMath::Clamp(P.HeightJitter, 0.0f, 0.95f);
						PassParams->CoverLeanMaxRad = FMath::Max(P.LeanMaxRad, 0.0f);
						PassParams->CoverAlignToNormal = FMath::Clamp(P.AlignToNormal, 0.0f, 1.0f);
						// 簇格 ≤ 0 会让 kernel 里的 floor 除以零 —— 钳到 1 cm（等价于"每叶自成一簇"，
						// 也就是退回逐叶随机，而不是把整趟散布写成 NaN）。
						PassParams->CoverClumpSize = FMath::Max(P.ClumpSize, 1.0f);
						PassParams->CoverClumpRadialChance = FMath::Clamp(P.ClumpRadialChance, 0.0f, 1.0f);
						PassParams->CoverClumpAlignment = FMath::Clamp(P.ClumpAlignment, 0.0f, 1.0f);
						PassParams->CoverScaleClumpShare = FMath::Clamp(P.ScaleClumpShare, 0.0f, 1.0f);
						// 下限不许超上限，同 ScaleRange 那条：lerp 照样算，只是"大的反而小"，无断言可见。
						PassParams->CoverBendRange = FVector2f(
							FMath::Max(P.BendRange.X, 0.0f),
							FMath::Max(P.BendRange.Y, FMath::Max(P.BendRange.X, 0.0f)));
						PassParams->CoverRadialBendScale = FMath::Max(P.RadialBendScale, 0.0f);
						PassParams->CoverSeed = P.Seed;
						PassParams->CoverSalt = P.Salt;
						PassParams->CoverMaxInstances = Work[Index].Capacity;
						PassParams->CoverBaseSphereCentre = P.BaseSphereCentre;
						PassParams->CoverBaseSphereRadius = P.BaseSphereRadius;
						PassParams->CoverBuildings = BuildingRefs.SRV;
						PassParams->CoverBuildingCount = uint32(FMath::Max(BuildingCount, 0));
						PassParams->CoverRingInner = FMath::Max(P.RingInner, 0.0f);
						PassParams->CoverRingWidth = P.RingWidth;
						// 噪声格 ≤ 0 会让 kernel 里的除法炸成 NaN（整条环带一株不长、无报错）—— 钳到 1 cm。
						PassParams->CoverRingNoiseCell = FMath::Max(P.RingNoiseCell, 1.0f);
						PassParams->CoverRingNoiseThreshold = FMath::Clamp(P.RingNoiseThreshold, 0.0f, 1.0f);
						PassParams->CoverRingMinScale = FMath::Clamp(P.RingMinScale, 0.0f, 1.0f);
						PassParams->CoverOverlayChance = FMath::Clamp(P.OverlayChance, 0.0f, 1.0f);
						// 聚散：尺寸 ≤ 0 = 关（kernel 里整段跳过，与加这道门控之前逐位相同）。
						PassParams->CoverPatchSize = FMath::Max(P.PatchSize, 0.0f);
						PassParams->CoverPatchCoverage = FMath::Clamp(P.PatchCoverage, 0.0f, 1.0f);
						PassParams->CoverPatchSoftness = FMath::Clamp(P.PatchSoftness, 0.0f, 1.0f);
						PassParams->CoverPatchSparse = FMath::Clamp(P.PatchSparseDensity, 0.0f, 1.0f);

						FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSGroundCover.Scatter[%d]", Index), Shader, PassParams,
							FComputeShaderUtils::GetGroupCount(FIntPoint(P.GridDims.X, P.GridDims.Y),
								FIntPoint(CSCover_GroupSizeX, CSCover_GroupSizeY)));
					}
				}
			}

			// 剔除 pass 只读这两个 buffer，且明说不负责恢复它们的状态 —— producer 自己留在 SRVMask。
			for (FRDGBufferRef Ref : PackedRefs) GraphBuilder.SetBufferAccessFinal(Ref, ERHIAccess::SRVMask);
			for (FRDGBufferRef Ref : CounterRefs) GraphBuilder.SetBufferAccessFinal(Ref, ERHIAccess::SRVMask);
			for (FRDGBufferRef Ref : CustomRefs) GraphBuilder.SetBufferAccessFinal(Ref, ERHIAccess::SRVMask);

			GraphBuilder.Execute();
		});

	return true;
}

void ReleaseOnRenderThread(FCoverBuffers& Buffers)
{
	if (!Buffers.PackedInstances.IsValid() && !Buffers.Counter.IsValid() && !Buffers.CustomData.IsValid())
	{
		Buffers.Reset();
		return;
	}

	ENQUEUE_RENDER_COMMAND(CSGroundCoverRelease)(
		[Released = MoveTemp(Buffers)](FRHICommandListImmediate&) mutable
		{
			Released.Reset();
		});
	Buffers.Reset();
}

int32 DebugReadInstancesSync(const FCoverBuffers& Buffers, TArray<FVector>* OutOrigins, TArray<FVector4f>* OutRows,
	TArray<float>* OutCustomData)
{
	if (OutOrigins) OutOrigins->Reset();
	if (OutRows) OutRows->Reset();
	if (OutCustomData) OutCustomData->Reset();
	if (!Buffers.IsValid()) return 0;

	int32 Count = 0;
	TArray<FVector4f> Rows;
	TArray<float> Custom;
	ENQUEUE_RENDER_COMMAND(CSGroundCoverDebugReadback)(
		[&Count, &Rows, &Custom, Work = Buffers, bWantCustom = (OutCustomData != nullptr),
		 bWantRows = (OutOrigins != nullptr || OutRows != nullptr)](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSGroundCover.DebugReadback"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work.Counter, TEXT("CSGroundCover.Counter"));

			FRHIGPUBufferReadback CounterReadback(TEXT("CSGroundCover.CounterReadback"));
			FRHIGPUBufferReadback RowReadback(TEXT("CSGroundCover.RowReadback"));
			FRHIGPUBufferReadback CustomReadback(TEXT("CSGroundCover.CustomReadback"));
			AddEnqueueCopyPass(GraphBuilder, &CounterReadback, CounterRef, sizeof(uint32));

			// 这些 buffer 被 Scatter 留在 SRVMask（给剔除 pass 用）；回读要 CopySrc，
			// 所以读完必须自己把它们放回去，否则下一帧的剔除 pass 会在错误的状态上撞见它们。
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			const uint32 RowBytes = Work.Capacity * CS_GPU_INSTANCED_ROW_FLOAT4S * sizeof(FVector4f);
			if (bWantRows)
			{
				FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work.PackedInstances, TEXT("CSGroundCover.PackedInstances"));
				AddEnqueueCopyPass(GraphBuilder, &RowReadback, PackedRef, RowBytes);
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			}
			const uint32 CustomBytes = Work.Capacity * uint32(CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS) * sizeof(float);
			if (bWantCustom)
			{
				FRDGBufferRef CustomRef = GraphBuilder.RegisterExternalBuffer(Work.CustomData, TEXT("CSGroundCover.CustomData"));
				AddEnqueueCopyPass(GraphBuilder, &CustomReadback, CustomRef, CustomBytes);
				GraphBuilder.SetBufferAccessFinal(CustomRef, ERHIAccess::SRVMask);
			}
			GraphBuilder.Execute();

			RHICmdList.SubmitAndBlockUntilGPUIdle();
			if (const uint32* Value = static_cast<const uint32*>(CounterReadback.Lock(sizeof(uint32))))
			{
				// GPU 的 counter 会数到越界丢弃的那些（InterlockedAdd 先加后判），按容量钳 —— 但超容量本身
				// 要出声：丢了哪一格由线程组完成顺序决定，而钳过的计数对所有断言都像"刚好装满"（审查 B4b）。
				if (*Value > Work.Capacity)
				{
					UE_LOG(LogCSGroundCover, Warning,
						TEXT("[CSGroundCover] 地被散布超容量：counter=%u capacity=%u，超出的格被静默丢弃（丢哪些不确定）。"),
						*Value, Work.Capacity);
				}
				Count = int32(FMath::Min(*Value, Work.Capacity));
				CounterReadback.Unlock();
			}
			if (bWantRows && Count > 0)
			{
				if (const FVector4f* Data = static_cast<const FVector4f*>(RowReadback.Lock(RowBytes)))
				{
					Rows.Append(Data, int32(Work.Capacity) * CS_GPU_INSTANCED_ROW_FLOAT4S);
					RowReadback.Unlock();
				}
			}
			if (bWantCustom && Count > 0)
			{
				if (const float* Data = static_cast<const float*>(CustomReadback.Lock(CustomBytes)))
				{
					Custom.Append(Data, int32(Work.Capacity) * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS);
					CustomReadback.Unlock();
				}
			}
		});
	// 诊断回读：按设计就是阻塞的，但也要计数 —— 计数器要么数到每一次阻塞，要么就不能拿它当证据。
	UCSMesh::CountedBlockingFlush();

	if (OutOrigins)
	{
		OutOrigins->Reserve(Count);
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const int32 Row = Index * CS_GPU_INSTANCED_ROW_FLOAT4S + 3;   // 第 4 行 = 原点 + 每实例随机数
			if (!Rows.IsValidIndex(Row)) break;
			OutOrigins->Add(FVector(Rows[Row].X, Rows[Row].Y, Rows[Row].Z));
		}
	}
	if (OutRows && Rows.Num() >= Count * CS_GPU_INSTANCED_ROW_FLOAT4S)
	{
		// 只带出活跃实例那一段：容量之外是上一趟的残值，谁读谁误判。
		OutRows->Append(Rows.GetData(), Count * CS_GPU_INSTANCED_ROW_FLOAT4S);
	}
	if (OutCustomData && Custom.Num() >= Count * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS)
	{
		OutCustomData->Append(Custom.GetData(), Count * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS);
	}
	return Count;
}
}

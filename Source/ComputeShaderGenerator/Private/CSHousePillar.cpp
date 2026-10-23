#include "CSHousePillar.h"

#include "CSHouseVine.h"   // IdentityHash / Hash01 —— 随机的口径全项目只有这一份
#include "ComputeShaderGenerateHelper.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "ShaderParameterStruct.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHousePillar_ 前缀。

constexpr int32 CSHousePillar_GroupSize = 64;

class FCSHousePillarPackCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCSHousePillarPackCS);
	SHADER_USE_PARAMETER_STRUCT(FCSHousePillarPackCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, PillarRecords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWPillarInstances)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWPillarCounter)
		SHADER_PARAMETER(FMatrix44f, PillarWorldToComponent)
		SHADER_PARAMETER(FVector3f, PillarBaseSphereCentre)
		SHADER_PARAMETER(FVector3f, PillarBlockSize)
		SHADER_PARAMETER(float, PillarBaseSphereRadius)
		SHADER_PARAMETER(uint32, PillarRecordCount)
		SHADER_PARAMETER(uint32, PillarMaxInstances)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUPSIZE_X"), CSHousePillar_GroupSize);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCSHousePillarPackCS, "/Plugin/PCGPlugins/Shaders/Private/CSHousePack.usf", "PackHousePillarCS", SF_Compute);
}

namespace CSHousePillar
{
void BuildBricks(const TArray<FVector>& Centers, const TArray<float>& Lengths,
	const FTransform& World, const FParams& Params, TArray<FBrick>& OutBricks)
{
	OutBricks.Reset();
	if (Centers.Num() != Lengths.Num()) return;

	const float Course = FMath::Max(Params.CourseHeight, 1.0f);
	const float Width = FMath::Max(Params.BrickWidth, 1.0f);
	const int32 BracketCourses = FMath::Clamp(Params.BracketCourses, 0, 16);

	const FVector UpWorld = World.TransformVectorNoScale(FVector::UpVector).GetSafeNormal();

	for (int32 P = 0; P < Centers.Num(); ++P)
	{
		const float Length = Lengths[P];
		if (Length <= Course * 0.5f) continue;

		// 层数按柱长取整，**再把层高摊回去**：直接用固定层高会让最上层要么穿进房底、
		// 要么留一条缝，而缝正好开在最显眼的柱顶。
		const int32 CourseCount = FMath::Max(1, FMath::RoundToInt(Length / Course));
		const float ActualCourse = Length / float(CourseCount);

		for (int32 C = 0; C < CourseCount; ++C)
		{
			// 局部：z = 0 是房底，柱子往下长（与 `ComputePillars` 的口径一致）。
			// 砖心在这一层的正中。
			const double CentreZ = -(double(C) + 0.5) * double(ActualCourse);
			const FVector LocalCentre(Centers[P].X, Centers[P].Y, CentreZ);

			// 托架：顶上几层逐层出挑。`C` 从 0 开始数、0 就是最上面那层（贴着房底），
			// 所以出挑量按 C 线性收回。
			float Widen = 1.0f;
			if (BracketCourses > 0 && C < BracketCourses)
			{
				const float T = 1.0f - float(C) / float(BracketCourses);   // 顶层 1、往下到 0
				Widen = 1.0f + FMath::Max(Params.BracketOverhang, 0.0f) * T;
			}

			const uint32 Id = CSHouseVine::IdentityHash(P, C, 0, 17u, Params.Seed);
			// Keep a continuous square bearing section. The old 0.72 alternating
			// rectangles made each course retreat by 28%; that number has no TG
			// decompilation evidence. Rotate the same beveled stone by quarter turns
			// for variety, as the TG brick VS does, without narrowing the support.
			const float Yaw = float(C & 3) * HALF_PI
				+ (CSHouseVine::Hash01(Id) - 0.5f) * 2.0f * FMath::Clamp(Params.YawJitter, 0.0f, 0.05f);
			const float Jit = 1.0f + (CSHouseVine::Hash01(Id ^ 0x9E3779B9u) - 0.5f)
				* 2.0f * FMath::Clamp(Params.SizeJitter, 0.0f, 0.05f);
			const float SizeX = Width * Widen * Jit;
			const float SizeY = SizeX;

			const float CosY = FMath::Cos(Yaw), SinY = FMath::Sin(Yaw);
			const FVector LocalX(CosY, SinY, 0.0);
			const FVector LocalY(-SinY, CosY, 0.0);

			FBrick Brick;
			Brick.Origin = FVector3f(World.TransformPosition(LocalCentre));
			Brick.AxisX = FVector3f(World.TransformVectorNoScale(LocalX).GetSafeNormal() * double(SizeX));
			Brick.AxisY = FVector3f(World.TransformVectorNoScale(LocalY).GetSafeNormal() * double(SizeY));
			// 高度轴不抖：层与层之间靠 `ActualCourse` 严丝合缝，抖它就会开缝。
			Brick.AxisZ = FVector3f(UpWorld * double(ActualCourse));
			Brick.Random01 = CSHouseVine::Hash01(CSHouseVine::IdentityHash(P, C, 0, 19u, Params.Seed));
			OutBricks.Add(Brick);
		}
	}
}

bool Pack(const TArray<FBrick>& Bricks, const CSShaperSteps::FPaletteBuffers& Palette,
	const FMatrix44f& WorldToComponent)
{
	if (!Palette.IsValid()) return false;

	TArray<FVector4f> Rows;
	Rows.Reset(Bricks.Num() * 4);
	for (const FBrick& B : Bricks)
	{
		Rows.Add(FVector4f(B.Origin.X, B.Origin.Y, B.Origin.Z, B.Random01));
		Rows.Add(FVector4f(B.AxisX.X, B.AxisX.Y, B.AxisX.Z, 0.0f));
		Rows.Add(FVector4f(B.AxisY.X, B.AxisY.Y, B.AxisY.Z, 0.0f));
		Rows.Add(FVector4f(B.AxisZ.X, B.AxisZ.Y, B.AxisZ.Z, 0.0f));
	}

	ENQUEUE_RENDER_COMMAND(CSHousePillarPack)(
		[Records = MoveTemp(Rows), Work = Palette, WorldToComponent](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("CSHousePillar.Pack"));

			FRDGBufferRef PackedRef = GraphBuilder.RegisterExternalBuffer(Work.PackedInstances, TEXT("CSHousePillar.PackedInstances"));
			FRDGBufferRef CounterRef = GraphBuilder.RegisterExternalBuffer(Work.Counter, TEXT("CSHousePillar.Counter"));
			FRDGBufferUAVRef PackedUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(PackedRef, PF_A32B32G32R32F));
			FRDGBufferUAVRef CounterUAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(CounterRef, PF_R32_UINT));

			const uint32 Count = uint32(Records.Num() / 4);
			// 空表必须显式清零：kernel 一个线程都不跑的话 counter 会留着上一次的值，
			// 症状是"柱子已经没了但画面上还在"，而且只在从有到无那一次出现。
			// （藤蔓那边正是漏了这一条才出现管子与分段并存。）
			if (Count == 0)
			{
				AddClearUAVPass(GraphBuilder, CounterUAV, 0u);
				GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
				GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
				GraphBuilder.Execute();
				return;
			}

			CSHelper::FRDGStructuredBufferRefs RecordRefs = CSHelper::CreateUploadedStructuredBuffer<FVector4f>(
				GraphBuilder, Records, TEXT("CSHousePillar.Records"), false, true);
			if (!RecordRefs.SRV) { GraphBuilder.Execute(); return; }

			FCSHousePillarPackCS::FParameters* PassParams = GraphBuilder.AllocParameters<FCSHousePillarPackCS::FParameters>();
			PassParams->PillarRecords = RecordRefs.SRV;
			PassParams->RWPillarInstances = PackedUAV;
			PassParams->RWPillarCounter = CounterUAV;
			PassParams->PillarWorldToComponent = WorldToComponent;
			PassParams->PillarBaseSphereCentre = Work.BaseSphereCentre;
			PassParams->PillarBlockSize = Work.BlockSize;
			PassParams->PillarBaseSphereRadius = Work.BaseSphereRadius;
			PassParams->PillarRecordCount = Count;
			PassParams->PillarMaxInstances = Work.Capacity;

			TShaderMapRef<FCSHousePillarPackCS> Shader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("CSHousePillar.Pack"), Shader, PassParams,
				FComputeShaderUtils::GetGroupCount(int32(Count), CSHousePillar_GroupSize));

			GraphBuilder.SetBufferAccessFinal(PackedRef, ERHIAccess::SRVMask);
			GraphBuilder.SetBufferAccessFinal(CounterRef, ERHIAccess::SRVMask);
			GraphBuilder.Execute();
		});

	return true;
}
}

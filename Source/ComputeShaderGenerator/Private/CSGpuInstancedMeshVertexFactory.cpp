#include "CSGpuInstancedMeshVertexFactory.h"
#include "CSGpuSharedLayout.ush"   // CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS（unity 构建下靠邻居带进来，-SingleFile 会露馅）

#include "MeshBatch.h"
#include "MeshDrawShaderBindings.h"
#include "MeshMaterialShader.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "RHIStaticStates.h"
#include "ShaderCore.h"            // GetShaderSourceFilePath：把着色器的虚拟路径换成磁盘路径
#include "ShaderParameterUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogCSGpuInstancedVF, Log, All);

IMPLEMENT_TYPE_LAYOUT(FCSGpuInstancedMeshVFShaderParameters);
IMPLEMENT_TYPE_LAYOUT(FCSGpuInstancedIndexedVFShaderParameters);

bool FCSGpuInstancedMeshVertexFactory::ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters)
{
	// Same gate as the engine's instanced factory: only materials that opted into instancing
	// pay for this extra permutation. Reusing the existing "Used with Instanced Static Meshes"
	// flag keeps the material setup identical to HISM's.
	return (Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes || Parameters.MaterialParameters.bIsSpecialEngineMaterial)
		&& FLocalVertexFactory::ShouldCompilePermutation(Parameters);
}

void FCSGpuInstancedMeshVertexFactory::ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
{
	FLocalVertexFactory::ModifyCompilationEnvironment(Parameters, OutEnvironment);

	// Force the manual-fetch instancing path unconditionally. FInstancedStaticMeshVertexFactory
	// picks USE_INSTANCE_CULLING (GPU Scene) whenever UseGPUScene() is true, which would take the
	// instance transforms out of our hands; VF_SUPPORTS_PRIMITIVE_SCENE_DATA is already 0 for this
	// factory because it is registered without SupportsPrimitiveIdStream.
	OutEnvironment.SetDefine(TEXT("USE_INSTANCING"), TEXT("1"));

	// No dithered LOD cross-fade: LOD is picked per instance by the cull pass and the instance
	// simply moves to another draw, so there is no transition to dither. Defining it to 0 also
	// keeps NEEDS_PER_INSTANCE_PARAMS off the RandomID-driven code path.
	OutEnvironment.SetDefine(TEXT("USE_DITHERED_LOD_TRANSITION_FOR_INSTANCED"), TEXT("0"));
}

void FCSGpuInstancedMeshVertexFactory::InitRHI(FRHICommandListBase& RHICmdList)
{
	// The base builds the declaration and the LocalVF uniform buffer (manual vertex fetch).
	// AddPrimitiveIdStreamElement inside it is a no-op for this factory type, and the instance
	// attributes are not vertex streams here — they are fetched from InstanceVF.
	FLocalVertexFactory::InitRHI(RHICmdList);

	// 两件事同时挂在这条断言上，而两件事的失效都是无声的：
	//  ① 实例变换必须从 InstanceVF 的 SRV 里手取 —— 有了 primitive-id 流，
	//     LocalVertexFactory.ush 会改走 GPU Scene，而 GPU Scene 的实例数据只能从 CPU 灌，
	//     整个 cull-on-GPU 的前提就没了。
	//  ② UCSGpuInstancedMeshComponent 的 CastShadow = true 能有影子（2026-08-30 实测），
	//     **全靠**这个索引是 -1，而且是**两头都靠**：
	//       · MeshPassProcessor.cpp:1304 的 bDoOverrideArgs 要求 PrimitiveIdStreamIndex >= 0，
	//         一旦 >= 0，阴影通路就用 GPU-Scene 的 args 顶掉 cull pass 写的那份，而我们在
	//         GPU-Scene 里没有实例 ⇒ 实例数 0 ⇒ 阴影里画 0 个三角形。
	//       · 索引是 -1 ⇒ `SupportsGPUScene()` 为假（VertexFactory.h:754）⇒ 本条路的 batch
	//         被 ShadowDepthRendering.cpp:2145 分给 EShadowMeshSelection::**SM**，也就是方向光
	//         开 VSM 时那份"只收进不了 VSM 的网格"的 CSM 回退级联（ShadowSetup.cpp:5606，
	//         靠 `r.Shadow.Virtual.ForceOnlyVirtualShadowMaps=0` 才会被创建，见
	//         Config/DefaultEngine.ini）。索引一旦 >= 0，batch 改被分给 VSM，然后死在上一条。
	//     所以"让它进 VSM"不是给这个 flag 能救的：那与①直接冲突。非实例化那条路没有①这个
	//     约束，2026-09-07 已用另一条办法（阴影视图直接绘制 + CPU 侧计数）修好。
	// 症状会是"主 pass 一切正常，只是影子没了"，没人会回到注册宏或基类的 InitRHI 上找原因。
	checkf(GetPrimitiveIdStreamIndex(GetFeatureLevel(), EVertexInputStreamType::Default) == INDEX_NONE,
		TEXT("FCSGpuInstancedMeshVertexFactory picked up a primitive-id stream. Manual-fetch instancing ")
		TEXT("and shadow casting through the conventional CSM fallback cascade both depend on it being ")
		TEXT("absent - see the EVertexFactoryFlags list at the bottom of this file."));

	// 深度预通道的**入场券**，它没了的症状是「实例只在天空背景上看得见」—— 见头文件里
	// SupportsPositionOnlyStream / SupportsPositionAndNormalOnlyStream 那两条 override 的长注释。
	//
	// 断言的是引擎那条**两套口径**的契约：`FDepthPassMeshProcessor::ShouldRender` 按**实例**的
	// SupportsPositionOnlyStream() 选 position-only 着色器，而 `TDepthOnlyVS<true>` 只为声明了
	// `EVertexFactoryFlags::SupportsPositionOnly` 的**类型**编译。两者一旦不一致，
	// GetDepthPassShaders 取不到着色器，整个 batch 就被**静默地**踢出深度预通道 ——
	// 没有日志、没有断言，只有"后画的不透明物把它整片盖掉"这一个远端症状。
	//
	// 这一条会在删掉那两条 override（或反过来只给类型加 flag）时立刻报红。
	checkf(!SupportsPositionOnlyStream() && !SupportsPositionAndNormalOnlyStream(),
		TEXT("FCSGpuInstancedMeshVertexFactory advertises a position-only vertex stream. The depth ")
		TEXT("prepass then asks for TDepthOnlyVS<true>, which is only compiled for vertex factory ")
		TEXT("types flagged SupportsPositionOnly - this one is not - so the batch is dropped from ")
		TEXT("the prepass without a word and the instances stop occupying depth."));

	FInstancedStaticMeshVertexFactoryUniformShaderParameters UniformParameters;
	UniformParameters.VertexFetch_InstanceOriginBuffer = InstanceOriginSRV;
	UniformParameters.VertexFetch_InstanceTransformBuffer = InstanceTransformSRV;
	UniformParameters.VertexFetch_InstanceLightmapBuffer = InstanceLightmapSRV;
	// 逐实例 custom data。没有真 SRV 时退回 origin 那条（只为"非空"这个硬要求）并把
	// 条数置 0 —— 条数是材质侧的读取步长，0 意味着材质里的 `Per Instance Custom Data`
	// 一律读 0，而不是去读那条冒名顶替的缓冲。
	UniformParameters.InstanceCustomDataBuffer = InstanceCustomDataSRV ? InstanceCustomDataSRV : InstanceOriginSRV;
	UniformParameters.NumCustomDataFloats = InstanceCustomDataSRV ? CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS : 0;
	InstanceUniformBuffer = TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters>::CreateUniformBufferImmediate(
		UniformParameters, UniformBuffer_MultiFrame, EUniformBufferValidation::None);
}

void FCSGpuInstancedMeshVertexFactory::ReleaseRHI()
{
	InstanceUniformBuffer.SafeRelease();
	FLocalVertexFactory::ReleaseRHI();
}

void FCSGpuInstancedMeshVFShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	const auto* InstancedVF = static_cast<const FCSGpuInstancedMeshVertexFactory*>(VertexFactory);

	// The batch never sets VertexFactoryUserData, so fall back to the factory's own LocalVF
	// uniform buffer (which manual vertex fetch needs).
	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	if (!VertexFactoryUniformBuffer) VertexFactoryUniformBuffer = InstancedVF->GetUniformBuffer();

	FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
		Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement,
		VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	// Start of this batch's region in the visible-instance buffers; SV_InstanceID is added to it.
	ShaderBindings.Add(InstanceOffsetParameter, (uint32)FMath::Max(BatchElement.UserIndex, 0));

	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(),
		InstancedVF->GetInstanceUniformBuffer());
	ShaderBindings.Add(VertexFetch_InstanceOriginBufferParameter, InstancedVF->GetInstanceOriginSRV());
	ShaderBindings.Add(VertexFetch_InstanceTransformBufferParameter, InstancedVF->GetInstanceTransformSRV());
	ShaderBindings.Add(VertexFetch_InstanceLightmapBufferParameter, InstancedVF->GetInstanceLightmapSRV());

	// Per-LOD pivot correction (HISM uses it for its LOD-range batches); nothing to offset here.
	ShaderBindings.Add(InstancingOffsetParameter, FVector4f(ForceInit));

	// InstancingFadeOutParams / view-Z compare live here; the leaf fills them per batch so that
	// PerInstanceParams resolves to "fully visible, no fade" (see FCSGpuInstancedMeshSceneProxy).
	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVFLooseUniformShaderParameters>(),
		BatchElement.LooseParametersUniformBuffer);
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Vertex, FCSGpuInstancedMeshVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedMeshVertexFactory, SF_Pixel, FCSGpuInstancedMeshVFShaderParameters);

// No SupportsPrimitiveIdStream: that flag is what would switch LocalVertexFactory.ush over to
// GPU-Scene instance data. No SupportsCachingMeshDrawCommands / SupportsStaticLighting either —
// this factory only ever draws through the dynamic-relevance path of FCSGpuMeshSceneProxy.
IMPLEMENT_VERTEX_FACTORY_TYPE(FCSGpuInstancedMeshVertexFactory, "/Engine/Private/LocalVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsManualVertexFetch
	| EVertexFactoryFlags::DoesNotSupportNullPixelShader
	| EVertexFactoryFlags::SupportsPSOPrecaching
);

// -----------------------------------------------------------------------------
// FCSGpuInstancedIndexedVertexFactory —— 可见源行号版
// -----------------------------------------------------------------------------

void FCSGpuInstancedIndexedVFShaderParameters::GetElementShaderBindings(
	const class FSceneInterface* Scene,
	const FSceneView* View,
	const FMeshMaterialShader* Shader,
	const EVertexInputStreamType InputStreamType,
	ERHIFeatureLevel::Type FeatureLevel,
	const FVertexFactory* VertexFactory,
	const FMeshBatchElement& BatchElement,
	FMeshDrawSingleShaderBindings& ShaderBindings,
	FVertexInputStreamArray& VertexStreams) const
{
	const auto* IndexedVF = static_cast<const FCSGpuInstancedIndexedVertexFactory*>(VertexFactory);

	FRHIUniformBuffer* VertexFactoryUniformBuffer = static_cast<FRHIUniformBuffer*>(BatchElement.VertexFactoryUserData);
	if (!VertexFactoryUniformBuffer) VertexFactoryUniformBuffer = IndexedVF->GetUniformBuffer();

	FLocalVertexFactoryShaderParametersBase::GetElementShaderBindingsBase(
		Scene, View, Shader, InputStreamType, FeatureLevel, VertexFactory, BatchElement,
		VertexFactoryUniformBuffer, ShaderBindings, VertexStreams);

	// 这一级 LOD 在可见源行号缓冲里的区段起点；着色器拿 SV_InstanceID 加上它去查源行号。
	ShaderBindings.Add(InstanceOffsetParameter, (uint32)FMath::Max(BatchElement.UserIndex, 0));
	ShaderBindings.Add(InstanceRowsParameter, IndexedVF->GetInstanceRowsSRV());
	ShaderBindings.Add(VisibleSourceIndicesParameter, IndexedVF->GetVisibleIndicesSRV());
	ShaderBindings.Add(CustomDataIndexMaskParameter, IndexedVF->GetCustomDataIndexMask());

	// 材质的 Per Instance Custom Data 仍经引擎的 InstanceVF 取（下标已是源行号，见生成的着色器）。
	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVertexFactoryUniformShaderParameters>(),
		IndexedVF->GetInstanceUniformBuffer());

	ShaderBindings.Add(InstancingOffsetParameter, FVector4f(ForceInit));
	ShaderBindings.Add(Shader->GetUniformBufferParameter<FInstancedStaticMeshVFLooseUniformShaderParameters>(),
		BatchElement.LooseParametersUniformBuffer);
}

void FCSGpuInstancedIndexedVertexFactory::VerifyShaderSourceUpToDate()
{
#if WITH_EDITOR
	static bool bChecked = false;
	if (bChecked) return;
	bChecked = true;

	const FString EnginePath = GetShaderSourceFilePath(TEXT("/Engine/Private/LocalVertexFactory.ush"));
	const FString GeneratedPath = GetShaderSourceFilePath(TEXT("/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedIndexedVertexFactory.ush"));

	FString EngineSource;
	FString Generated;
	// 读不到 = 没有源码可比（比如只带了编好的着色器），那就没有"过期"可言。
	if (!FFileHelper::LoadFileToString(EngineSource, *EnginePath) || !FFileHelper::LoadFileToString(Generated, *GeneratedPath)) return;

	// 与生成脚本同一口径：换行归一成 LF 之后的 UTF-8 字节。
	EngineSource.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
	const FTCHARToUTF8 Utf8(*EngineSource);
	FSHA1 Sha;
	Sha.Update(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Sha.Final();
	uint8 Digest[FSHA1::DigestSize];
	Sha.GetHash(Digest);
	const FString Actual = BytesToHex(Digest, FSHA1::DigestSize).ToLower();

	const FString Marker = TEXT("// SourceSHA1: ");
	const int32 MarkerAt = Generated.Find(Marker, ESearchCase::CaseSensitive);
	const FString Recorded = MarkerAt != INDEX_NONE ? Generated.Mid(MarkerAt + Marker.Len(), 40).ToLower() : FString();
	if (Recorded != Actual)
	{
		UE_LOG(LogCSGpuInstancedVF, Error,
			TEXT("CSGpuInstancedIndexedVertexFactory.ush 是照着另一份 LocalVertexFactory.ush 生成的（记录 %s，这台机器的引擎是 %s）。")
			TEXT("引擎那份里改过的地方这边没跟上 —— 跑 Plugins/PCGPlugins/Scripts/GenerateGpuInstancedVertexFactoryShader.py 重新生成。"),
			*Recorded, *Actual);
	}
#endif
}

IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedIndexedVertexFactory, SF_Vertex, FCSGpuInstancedIndexedVFShaderParameters);
IMPLEMENT_VERTEX_FACTORY_PARAMETER_TYPE(FCSGpuInstancedIndexedVertexFactory, SF_Pixel, FCSGpuInstancedIndexedVFShaderParameters);

// 标志与上面那个工厂逐项相同（同样不要 SupportsPrimitiveIdStream，理由见那边），只换着色器文件。
IMPLEMENT_VERTEX_FACTORY_TYPE(FCSGpuInstancedIndexedVertexFactory, "/Plugin/PCGPlugins/Shaders/Private/CSGpuInstancedIndexedVertexFactory.ush",
	  EVertexFactoryFlags::UsedWithMaterials
	| EVertexFactoryFlags::SupportsDynamicLighting
	| EVertexFactoryFlags::SupportsPrecisePrevWorldPos
	| EVertexFactoryFlags::SupportsManualVertexFetch
	| EVertexFactoryFlags::DoesNotSupportNullPixelShader
	| EVertexFactoryFlags::SupportsPSOPrecaching
);

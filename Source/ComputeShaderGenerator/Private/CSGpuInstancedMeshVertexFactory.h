#pragma once

#include "CoreMinimal.h"
#include "LocalVertexFactory.h"
#include "Engine/InstancedStaticMesh.h" // FInstancedStaticMeshVertexFactoryUniformShaderParameters ("InstanceVF")

/**
 * Vertex factory for UCSGpuInstancedMeshComponent: one GPU-resident copy of the base mesh,
 * drawn N times with per-instance transforms that the vertex shader MANUAL-FETCHES from
 * plain GPU buffers indexed by SV_InstanceID.
 *
 * It reuses the engine's /Engine/Private/LocalVertexFactory.ush unchanged. That file has two
 * instancing paths:
 *   USE_INSTANCE_CULLING — instance data comes from GPU Scene (what ISM/HISM get on desktop).
 *   USE_INSTANCING       — instance data comes from InstanceVF.VertexFetch_Instance*Buffer,
 *                          i.e. from SRVs the vertex factory supplies (the ES3.1 ISM path).
 * FInstancedStaticMeshVertexFactory picks the first whenever UseGPUScene() is true, which is
 * always on SM5 — and GPU Scene instance data can only be filled from the CPU. This factory
 * therefore forces USE_INSTANCING and declares itself WITHOUT SupportsPrimitiveIdStream so
 * VF_USE_PRIMITIVE_SCENE_DATA compiles to 0: the shader then reads the instance transform out
 * of *our* compute-written buffers, which is the whole point — the visible-instance list is
 * produced on the GPU by the cull pass and never round-trips to the CPU.
 *
 * That also matches what FCSGpuMeshSceneProxy already does: it draws through dynamic relevance
 * with an FDynamicPrimitiveUniformBuffer rather than a GPU-Scene primitive, so dropping the
 * primitive-id stream costs nothing here.
 *
 * Instance layout expected by LocalVertexFactory.ush (see GetInstanceTransform there):
 *   TransformBuffer: Buffer<float4>, 3 per instance — rows of the instance-to-component 3x3,
 *                    .w of row 0 carries hitproxy/selection (write 0).
 *   OriginBuffer:    Buffer<float4>, 1 per instance — xyz = instance origin in component space,
 *                    w = per-instance random (material's PerInstanceRandom).
 *   LightmapBuffer:  Buffer<float4>, 1 per instance — lightmap/shadowmap UV bias (write 0).
 * The final vertex transform is InstanceToComponent * Primitive.LocalToWorld, so instance
 * transforms are component-local exactly like HISM's.
 */
struct FCSGpuInstancedMeshVertexFactory : public FLocalVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FCSGpuInstancedMeshVertexFactory);

public:
	FCSGpuInstancedMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
		: FLocalVertexFactory(InFeatureLevel, InDebugName)
	{
	}

	static bool ShouldCompilePermutation(const FVertexFactoryShaderPermutationParameters& Parameters);
	static void ModifyCompilationEnvironment(const FVertexFactoryShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment);

	/** Hand the factory the per-instance SRVs. Must be called before InitResource(); the
	 *  InstanceVF uniform buffer is built from them in InitRHI. */
	void SetInstanceStreams(FRHIShaderResourceView* InOriginSRV, FRHIShaderResourceView* InTransformSRV, FRHIShaderResourceView* InLightmapSRV,
		FRHIShaderResourceView* InCustomDataSRV = nullptr)
	{
		InstanceOriginSRV = InOriginSRV;
		InstanceTransformSRV = InTransformSRV;
		InstanceLightmapSRV = InLightmapSRV;
		InstanceCustomDataSRV = InCustomDataSRV;
	}

	//~ FRenderResource interface
	virtual void InitRHI(FRHICommandListBase& RHICmdList) override;
	virtual void ReleaseRHI() override;

	/**
	 * 这两条 override 是**深度预通道的入场券**，删掉的症状是「实例只在天空背景上看得见」。
	 *
	 * `FLocalVertexFactory::InitRHI` 会顺手建出 PositionOnly / PositionAndNormalOnly 两套顶点
	 * 声明，于是基类的 `SupportsPositionOnlyStream()` 返回 true。而深度预通道的选路是**两套口径**：
	 *   · `FDepthPassMeshProcessor::ShouldRender`（`DepthRendering.cpp:962`）看的是**实例**的
	 *     `MeshBatch.VertexFactory->SupportsPositionOnlyStream()` —— true 就选 position-only 着色器；
	 *   · `TDepthOnlyVS<true>::ShouldCompilePermutation`（`DepthRendering.h:85`）看的是**类型**的
	 *     `VertexFactoryType->SupportsPositionOnly()` —— 本工厂的 `IMPLEMENT_VERTEX_FACTORY_TYPE`
	 *     里没有这个 flag（见本文件末尾），所以那个着色器**从来没被编译过**。
	 * 两边一冲突，`GetDepthPassShaders<true>` 取不到着色器 ⇒ `Process<true>` 返回 false ⇒
	 * **整个 batch 被静默地踢出深度预通道**，一条日志都没有。
	 *
	 * 后果不是"画不出来"而是"画了但不占深度"：本工程是全深度预通道（Lumen/VSM 强制
	 * `DDM_AllOpaque`），基通道因此是 `DepthRead` + `CF_DepthNearOrEqual`，**谁都不写深度**。
	 * 实例的颜色照常写进去，但深度缓冲里没有它 —— 于是同一趟基通道里**后画的任何不透明物**
	 * （地面、墙）都能通过深度测试把它整片盖掉。实测症状：`L_TerrainOpsDemo` 的 216 级石阶
	 * 只剩地形轮廓线外侧那十几块（背后是天空、没有后画的东西盖它）。
	 *
	 * 引擎自己的 `FInstancedStaticMeshVertexFactory` 没有这个问题，因为它**重写了 InitRHI**、
	 * 压根不建这两套声明（`InstancedStaticMesh.cpp:778` 那行注释原文：
	 * "we don't need per-vertex shadow or lightmap rendering"）。本工厂复用基类的 InitRHI，
	 * 所以必须在这里把口径对齐 —— 两条 override 的效果就是让深度预通道走
	 * `Process<false>`（完整顶点声明 + 默认材质），也就是基通道用的那条、已经被证明正确的路。
	 *
	 * 代价：预通道多绑一份完整顶点声明。手取顶点（SupportsManualVertexFetch）下这份声明
	 * 实际只有 position 一个元素，所以代价接近零；且**只影响本实例工厂**，
	 * `FCSGpuMeshSceneProxy` 的非实例路仍用原生 `FLocalVertexFactory`，一个字节都没动。
	 */
	virtual bool SupportsPositionOnlyStream() const override { return false; }
	virtual bool SupportsPositionAndNormalOnlyStream() const override { return false; }

	FRHIShaderResourceView* GetInstanceOriginSRV() const { return InstanceOriginSRV; }
	FRHIShaderResourceView* GetInstanceTransformSRV() const { return InstanceTransformSRV; }
	FRHIShaderResourceView* GetInstanceLightmapSRV() const { return InstanceLightmapSRV; }
	FRHIShaderResourceView* GetInstanceCustomDataSRV() const { return InstanceCustomDataSRV; }
	FRHIUniformBuffer* GetInstanceUniformBuffer() const { return InstanceUniformBuffer.GetReference(); }

private:
	// Not owned: these live on the scene proxy's stream set, which outlives the factory.
	FRHIShaderResourceView* InstanceOriginSRV = nullptr;
	FRHIShaderResourceView* InstanceTransformSRV = nullptr;
	FRHIShaderResourceView* InstanceLightmapSRV = nullptr;
	FRHIShaderResourceView* InstanceCustomDataSRV = nullptr;

	TUniformBufferRef<FInstancedStaticMeshVertexFactoryUniformShaderParameters> InstanceUniformBuffer;
};

/**
 * Binds the per-instance SRVs / InstanceVF uniform buffer on top of the standard local-VF
 * bindings. FInstancedStaticMeshVertexFactoryShaderParameters cannot be reused: it skips every
 * instance binding when UseGPUScene() is true, which is exactly the case we are overriding.
 *
 * FMeshBatchElement::UserIndex carries the instance-buffer offset for the batch — the instanced
 * leaf gives each LOD its own fixed region in the visible-instance buffers and passes that
 * region's start here, since SV_InstanceID always restarts at 0 per draw.
 */
class FCSGpuInstancedMeshVFShaderParameters : public FLocalVertexFactoryShaderParametersBase
{
	DECLARE_TYPE_LAYOUT(FCSGpuInstancedMeshVFShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);

		InstancingOffsetParameter.Bind(ParameterMap, TEXT("InstancingOffset"));
		VertexFetch_InstanceOriginBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceOriginBuffer"));
		VertexFetch_InstanceTransformBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceTransformBuffer"));
		VertexFetch_InstanceLightmapBufferParameter.Bind(ParameterMap, TEXT("VertexFetch_InstanceLightmapBuffer"));
		InstanceOffsetParameter.Bind(ParameterMap, TEXT("InstanceOffset"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const;

private:
	LAYOUT_FIELD(FShaderParameter, InstancingOffsetParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceOriginBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceTransformBufferParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VertexFetch_InstanceLightmapBufferParameter);
	LAYOUT_FIELD(FShaderParameter, InstanceOffsetParameter);
};

/**
 * 「可见源行号」版的实例顶点工厂（InstanceCullingOptimization.md §5，2026-10-02）。
 *
 * 上面那个工厂画的是剔除 pass **抄出来**的可见属性：每个可见槽 3 + 1 + 1 个 float4 加 custom data，88 字节，
 * 每级 LOD 各预留一整份容量。这个工厂画的是**源行本身**：剔除只往可见槽里写一个 4 字节的源行号，
 * 顶点着色器经它去读源实例行（CPU 源 / 点云源是常驻流里的 `SourceInstances`，packed GPU 源是生产者自己那块 buffer）。
 * 可见缓冲从 `88 × 容量 × LOD 数` 降到 `4 × 容量 × LOD 数`，剔除每个可见实例少写 84 字节。
 *
 * 着色器不是引擎的 LocalVertexFactory.ush，而是它的一份打补丁副本
 * `Shaders/Private/CSGpuInstancedIndexedVertexFactory.ush`（由 Scripts/GenerateGpuInstancedVertexFactoryShader.py 生成，
 * 为什么不能直接复用引擎那份写在脚本头）。引擎升级后那份副本要重新生成 —— VerifyShaderSourceUpToDate 守着这一条。
 *
 * 继承上面那个工厂只为复用它的 InitRHI（两条断言 + InstanceVF uniform buffer）：材质读逐实例 custom data
 * 走的仍是引擎 MaterialTemplate 里 `InstanceVF.InstanceCustomDataBuffer[下标 × 步长 + i]` 那条路，这个工厂给它绑的是
 * **源** custom data（下标是源行号），生产者没给时绑一行零并把下标钉成 0（CSCustomDataIndexMask）。
 * InstanceVF 里那三条 VertexFetch_Instance*Buffer 在打过补丁的着色器里没人读，绑源行只是因为 uniform buffer 不收空 SRV。
 *
 * 两个工厂同时存在是为了交错 A/B（`r.CSGpuInstanced.VisibleSourceIndex`，见 UCSGpuInstancedMeshComponent）。
 */
struct FCSGpuInstancedIndexedVertexFactory : public FCSGpuInstancedMeshVertexFactory
{
	DECLARE_VERTEX_FACTORY_TYPE(FCSGpuInstancedIndexedVertexFactory);

public:
	FCSGpuInstancedIndexedVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const char* InDebugName)
		: FCSGpuInstancedMeshVertexFactory(InFeatureLevel, InDebugName)
	{
	}

	/**
	 * InRowsSRV            Buffer<float4>，源实例行（CS_GPU_INSTANCED_ROW_FLOAT4S 个 / 行）
	 * InVisibleIndicesSRV  Buffer<uint>，每个可见槽一个源行号
	 * InCustomDataSRV      Buffer<float>，bSourceCustomData 时是生产者的逐源行 custom data，否则是一行零
	 * Must be called before InitResource().
	 */
	void SetIndexedInstanceStreams(FRHIShaderResourceView* InRowsSRV, FRHIShaderResourceView* InVisibleIndicesSRV,
		FRHIShaderResourceView* InCustomDataSRV, bool bSourceCustomData)
	{
		// 基类拿这四条建 InstanceVF；前三条在这份着色器里没人读（见类注释）。
		SetInstanceStreams(InRowsSRV, InRowsSRV, InRowsSRV, InCustomDataSRV);
		InstanceRowsSRV = InRowsSRV;
		VisibleIndicesSRV = InVisibleIndicesSRV;
		CustomDataIndexMask = bSourceCustomData ? MAX_uint32 : 0u;
	}

	FRHIShaderResourceView* GetInstanceRowsSRV() const { return InstanceRowsSRV; }
	FRHIShaderResourceView* GetVisibleIndicesSRV() const { return VisibleIndicesSRV; }
	uint32 GetCustomDataIndexMask() const { return CustomDataIndexMask; }

	/**
	 * 生成的着色器是不是照着这台机器上引擎的 LocalVertexFactory.ush 生成的（比文件头里记的 SHA-1）。
	 * 对不上只报一次 Error，不拦渲染：多半仍然编得过，但引擎那份里改过的地方这边没跟上 —— 那种错是画面错，不是编译错。
	 * 游戏线程，读两个小文件；只在编辑器里查（打包后没有引擎着色器源码可比）。
	 */
	static void VerifyShaderSourceUpToDate();

private:
	// Not owned: the rows live on the resident set or on the producer's buffer, both of which the proxy keeps alive.
	FRHIShaderResourceView* InstanceRowsSRV = nullptr;
	FRHIShaderResourceView* VisibleIndicesSRV = nullptr;
	uint32 CustomDataIndexMask = 0;
};

/** 可见源行号版的绑定：区段起点（InstanceOffset）、源行、可见源行号、custom data 下标掩码，外加 InstanceVF 与 loose UB。 */
class FCSGpuInstancedIndexedVFShaderParameters : public FLocalVertexFactoryShaderParametersBase
{
	DECLARE_TYPE_LAYOUT(FCSGpuInstancedIndexedVFShaderParameters, NonVirtual);

public:
	void Bind(const FShaderParameterMap& ParameterMap)
	{
		FLocalVertexFactoryShaderParametersBase::Bind(ParameterMap);

		InstancingOffsetParameter.Bind(ParameterMap, TEXT("InstancingOffset"));
		InstanceOffsetParameter.Bind(ParameterMap, TEXT("InstanceOffset"));
		InstanceRowsParameter.Bind(ParameterMap, TEXT("CSInstanceRows"));
		VisibleSourceIndicesParameter.Bind(ParameterMap, TEXT("CSVisibleSourceIndices"));
		CustomDataIndexMaskParameter.Bind(ParameterMap, TEXT("CSCustomDataIndexMask"));
	}

	void GetElementShaderBindings(
		const class FSceneInterface* Scene,
		const FSceneView* View,
		const FMeshMaterialShader* Shader,
		const EVertexInputStreamType InputStreamType,
		ERHIFeatureLevel::Type FeatureLevel,
		const FVertexFactory* VertexFactory,
		const FMeshBatchElement& BatchElement,
		FMeshDrawSingleShaderBindings& ShaderBindings,
		FVertexInputStreamArray& VertexStreams) const;

private:
	LAYOUT_FIELD(FShaderParameter, InstancingOffsetParameter);
	LAYOUT_FIELD(FShaderParameter, InstanceOffsetParameter);
	LAYOUT_FIELD(FShaderResourceParameter, InstanceRowsParameter);
	LAYOUT_FIELD(FShaderResourceParameter, VisibleSourceIndicesParameter);
	LAYOUT_FIELD(FShaderParameter, CustomDataIndexMaskParameter);
};

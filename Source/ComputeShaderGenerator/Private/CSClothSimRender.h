#pragma once

#include "CoreMinimal.h"
#include "CSClothSim.h"
#include "RenderGraphResources.h"

#include <atomic>

class FRHIGPUBufferReadback;
struct FCSMeshResident;

/**
 * 布料的 GPU 状态（D1 的 FCSClothState）：组件自持的一套 pooled buffer，**不进 UCSMesh 的流集合**。
 *
 * 不做成 AuxVertex 流挂进网格的理由见计划 D1：粒子数 ≠ 渲染顶点数，速度 buffer 不该被
 * CopyToStaticMesh 看见；网格保持「任何算子可跑」，布料侧靠 Generation 检测失配停摆。
 *
 * 线程：buffer 引用在 setup 的渲染命令里写、之后只在渲染线程读；尺寸在构造时定死。
 * 回读对象只能在渲染线程碰（Lock 走 immediate command list），所以最后一份引用必须在渲染线程放掉 ——
 * 组件的释放路径把引用交给一条渲染命令，见 UCSClothSimComponent::ReleaseSimulation。
 */
struct FCSClothGpuState
{
	uint32 NumParticles = 0;
	uint32 NumConstraints = 0;
	uint32 NumTriangles = 0;
	uint32 NumRenderVertices = 0;
	uint32 NumPinGroups = 1;

	TRefCountPtr<FRDGPooledBuffer> Positions;
	TRefCountPtr<FRDGPooledBuffer> PrevPositions;
	TRefCountPtr<FRDGPooledBuffer> RestPositions;
	TRefCountPtr<FRDGPooledBuffer> PinGroups;
	TRefCountPtr<FRDGPooledBuffer> NormalAccum;
	TRefCountPtr<FRDGPooledBuffer> ParticleNormals;
	TRefCountPtr<FRDGPooledBuffer> RestNormals;
	TRefCountPtr<FRDGPooledBuffer> Constraints;
	TRefCountPtr<FRDGPooledBuffer> Lambdas;
	TRefCountPtr<FRDGPooledBuffer> TriangleParticles;
	TRefCountPtr<FRDGPooledBuffer> CornerToParticle;
	TRefCountPtr<FRDGPooledBuffer> RestTangents;
	TRefCountPtr<FRDGPooledBuffer> Status;

	bool IsInitialized() const { return Positions.IsValid() && Status.IsValid(); }

	// --- 渲染线程独占 ---------------------------------------------------------------

	/** 每种颜色在 Constraints 里的 [Offset, Count)，按求解顺序（FCSClothSetupData::ColorRanges 的拷贝）。 */
	TArray<FIntPoint> ColorRanges;

	/** 上一个 substep 的步长：帧长变化时速度 = 位移 / 上一步的步长。0 = 还没跑过。 */
	float LastSubstepDt = 0.0f;

	/** 状态 buffer 的非阻塞回读，在途时为非空。 */
	FRHIGPUBufferReadback* StatusReadback = nullptr;

	// --- 跨线程读数（渲染线程写、游戏线程读）------------------------------------------

	/** 最近一次落地的回读里，写回时遇到的非有限粒子数。> 0 就是炸了。 */
	std::atomic<uint32> NonFiniteParticles{ 0 };

	/** 渲染线程上实际跑完的帧数（含被拒之外的所有帧），测试用它确认命令确实执行过。 */
	std::atomic<uint32> FramesSimulated{ 0 };

	~FCSClothGpuState();
};

using FCSClothGpuStateRef = TSharedPtr<FCSClothGpuState, ESPMode::ThreadSafe>;

/** 一帧的全部参数：游戏线程打包好、按值进渲染命令。 */
struct FCSClothFrameParams
{
	int32 Substeps = 8;
	int32 Iterations = 1;
	float SubstepDt = 1.0f / 480.0f;

	float DampingFactor = 1.0f;
	float MaxSpeed = 5000.0f;
	FVector3f Gravity = FVector3f(0.0f, 0.0f, -980.0f);

	bool bWind = false;
	FVector3f WindVelocity = FVector3f::ZeroVector;
	float WindTurbulence = 0.0f;
	FVector3f WindNoiseOffset = FVector3f::ZeroVector;
	float WindNoiseFrequency = 0.01f;
	float WindNormalDrag = 2.0f;
	float WindTangentialDrag = 0.1f;

	float StretchCompliance = 0.0f;
	float CompressionCompliance = 0.0f;
	float BendCompliance = 0.0f;
	float TetherCompliance = 0.0f;
	float MaxCorrection = 20.0f;

	float CollisionThickness = 1.0f;
	float Friction = 0.2f;
	/** CS_CLOTH_COLLIDER_FLOAT4S 个 float4 / 碰撞体（上一帧形状 + 本帧形状）。 */
	TArray<FVector4f> Colliders;

	/** CS_CLOTH_PIN_GROUP_FLOAT4S 个 float4 / 钉组（上一帧 3x4 + 本帧 3x4）。长度必须 = 钉组数 × 6。 */
	TArray<FVector4f> PinTransforms;

	bool bTeleport = false;
	FVector4f TeleportRows[3] = { FVector4f(1, 0, 0, 0), FVector4f(0, 1, 0, 0), FVector4f(0, 0, 1, 0) };

	/** sim 空间原点（世界坐标，float）。写回时加回去。 */
	FVector3f SimOrigin = FVector3f::ZeroVector;
};

namespace CSClothRender
{
	/**
	 * setup 的 GPU 半程：建全部 pooled buffer 并上传 CPU 建好的数据，从网格的切线流原样拷一份
	 * rest 切线基，再用运行期同一套法线 pass 算出 rest 粒子法线 —— 同一套 pass、同一组输入、
	 * 整数原子累加与顺序无关，所以 rest 姿态下写回的旋转严格是恒等。渲染线程。
	 */
	void InitState_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
		const FCSClothSetupData& Data, FCSMeshResident& Resident);

	/** 一帧：自建一张图跑完全部 substep，图尾借 FCSMeshRenderThreadEdit 写回网格流，执行。渲染线程。 */
	void SimulateFrame_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
		const FCSClothFrameParams& Params, FCSMeshResident& Resident);

	/** 把粒子摆到给定位置（sim 空间，w = InvMass）、速度清零，并当场写回网格流。渲染线程。 */
	void ResetToPositions_RenderThread(FRHICommandListImmediate& RHICmdList, FCSClothGpuState& State,
		const TArray<FVector4f>& Positions, FCSMeshResident& Resident, const FVector3f& SimOrigin);

	/**
	 * 把粒子位置（sim 空间）读回 CPU。**测试 / 诊断专用**：游戏线程，阻塞，并计入
	 * UCSMesh::GetBlockingFlushCount。正常运行路径一次都不许走它。
	 */
	bool ReadbackParticlesSync(const FCSClothGpuStateRef& State, TArray<FVector4f>& OutPositions);
}

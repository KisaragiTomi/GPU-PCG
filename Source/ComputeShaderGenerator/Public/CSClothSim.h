#pragma once

#include "CoreMinimal.h"
#include "CSClothSim.generated.h"

class AActor;
struct FCSGpuMeshCPUData;

COMPUTESHADERGENERATOR_API DECLARE_LOG_CATEGORY_EXTERN(LogCSCloth, Log, All);

// -----------------------------------------------------------------------------
// GPU 布料：XPBD 常驻算子逐帧驱动 UCSMesh 形变（Docs/GpuClothSim_Plan.md）。
//
// 本头文件是与 RHI 无关的那一半：编辑面板上的数据类型，以及 setup 的 CPU 半程（焊接 → 粒子表 →
// 约束 → 质量 / 钉点 / tether）。运行期的 GPU 状态与 pass 在 Private/CSClothSimRender.h，
// 宿主是 UCSClothSimComponent（CSClothSimComponent.h）。拆开的理由是测试：拓扑与约束的正确性
// 不需要 GPU 就能断言，而 GPU 测试必须真 RHI（-NullRHI 下本插件的 compute shader 不存在）。
// -----------------------------------------------------------------------------

/** 解析碰撞体的形状（D4 的 analytic 层）。 */
UENUM(BlueprintType)
enum class ECSClothColliderType : uint8
{
	Sphere,
	Capsule,
	/** 无限大半空间：Center 是平面上一点，Axis 是朝外的法线，布料被推回法线一侧。 */
	Plane,
};

/**
 * 一个解析碰撞体。每帧在游戏线程按 FollowActor 的当前变换解析成 sim 空间形状上传，
 * substep 之间在上一帧与本帧之间插值 —— 快速移动的碰撞体因此不会一帧之内整个穿过布料。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSClothCollider
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	ECSClothColliderType Type = ECSClothColliderType::Sphere;

	/** 球心 / 胶囊中点 / 平面上一点。FollowActor 为空时是世界坐标，否则是它的局部坐标。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	FVector Center = FVector::ZeroVector;

	/** 胶囊的轴向 / 平面的朝外法线（与 Center 同一空间，内部会单位化）。球不用它。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	FVector Axis = FVector::UpVector;

	/** 球 / 胶囊半径（cm）。会按 FollowActor 的最大缩放分量放大。平面不用它。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth", meta = (ClampMin = "0.0"))
	float Radius = 50.0f;

	/** 胶囊：两端球心之间距离的一半（cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth", meta = (ClampMin = "0.0"))
	float HalfHeight = 50.0f;

	/** 形状跟着这个 actor 走（编辑器里拖它，布料当场响应）。为空 = 世界空间里固定不动。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	TObjectPtr<AActor> FollowActor = nullptr;
};

/**
 * 钉盒：落在盒内的粒子 InvMass = 0，按运动学跟随。
 *
 * 盒子写在**宿主 actor 的局部空间**里（布料是在那里摆的，世界盒子拖一下 actor 就对不上了）。
 * 钉住的粒子跟谁走由 AttachActor 决定：空 = 宿主 actor 自己（旗杆随 actor 动，旗面自由摆）；
 * 否则跟这个 actor 自 setup 以来的相对运动（两个人各攥一角的那种）。
 */
USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSClothPinBox
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	FBox Box = FBox(FVector(-10.0), FVector(10.0));

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Cloth")
	TObjectPtr<AActor> AttachActor = nullptr;
};

/** 顶点色钉点（P1）：哪个通道当钉点遮罩。 */
UENUM(BlueprintType)
enum class ECSClothPinColorChannel : uint8
{
	None,
	Red,
	Green,
	Blue,
	Alpha,
};

// -----------------------------------------------------------------------------
// setup 的 CPU 半程（D5 第 2~4 步）
// -----------------------------------------------------------------------------

struct COMPUTESHADERGENERATOR_API FCSClothSetupOptions
{
	/** 焊接容差（cm）：相距在它以内的渲染顶点并成一个粒子。UV 缝 / 硬边两侧的副本逐位相等，
	 *  默认值只为吃掉浮点噪声；调大会把本来分开的两片布缝到一起。 */
	float WeldTolerance = 0.01f;

	/** 面密度 kg/m²。质量按相邻三角面积的 1/3 归到粒子上（棉布约 0.15~0.3）。 */
	float ArealDensity = 0.2f;

	/** sim 空间原点（世界坐标）。粒子位置一律存成相对它的偏移：大世界坐标下 float 的分辨率
	 *  会吃进约束的边长里（1e5 cm 处 ulp ≈ 0.008 cm，对 2 cm 的边是 0.4% 的抖动）。 */
	FVector SimOrigin = FVector::ZeroVector;

	/** 已解析到世界空间的钉盒。Group 是钉组号，0 = 宿主 actor。 */
	struct FPinBox
	{
		FBox WorldBox = FBox(ForceInit);
		uint32 Group = 0;
	};
	TArray<FPinBox> PinBoxes;

	/** 顶点色钉点：该通道 ≥ PinColorThreshold 的渲染顶点所在的粒子钉到组 0。 */
	ECSClothPinColorChannel PinColorChannel = ECSClothPinColorChannel::None;
	float PinColorThreshold = 0.5f;

	/** 钉组总数（≥ 1）。BuildFromSnapshot 按它确定每组统计量的长度。 */
	int32 NumPinGroups = 1;

	/** 长程 tether（long range attachment）：每个能沿网格连到钉点的自由粒子一条，
	 *  上限 = 到最近钉点的测地距离 × TetherScale。抗下垂的廉价手段，只在拉长时起作用。 */
	bool bBuildTethers = true;
	float TetherScale = 1.02f;
};

/**
 * 建好的拓扑与约束，即将原样上传的那份（D1 的约束域与映射域）。
 *
 * 约束统一成一种记录：uint4 (A, B, asuint(RestLength), Type)，三类都是距离约束。存放顺序是**颜色序**
 * （见 ColorRanges），不是类型序 —— 按类型筛请看 .W。
 */
struct COMPUTESHADERGENERATOR_API FCSClothSetupData
{
	int32 NumParticles = 0;
	int32 NumRenderVertices = 0;

	/** 每粒子 sim 空间 rest 位置，w = InvMass（0 = 钉住）。 */
	TArray<FVector4f> RestPositions;

	/** 每粒子钉组号；CS_CLOTH_FREE_PARTICLE = 自由。 */
	TArray<uint32> PinGroups;

	/** 每渲染顶点 → 粒子（D1 的 CornerToParticle；写回 pass 按它 scatter）。 */
	TArray<uint32> CornerToParticle;

	/** 每三角 3 个粒子号。焊接后退化的三角、以及与前面某个三角同一组粒子的重复三角（双面网格的
	 *  背面）已剔除：背面若参与法线累加，正反两面的面法线正好互相抵消，粒子法线归零。 */
	TArray<uint32> TriangleParticles;

	/** (A, B, asuint(RestLength), Type)，按颜色连续存放。 */
	TArray<FUintVector4> Constraints;
	int32 NumStretch = 0;
	int32 NumBend = 0;
	int32 NumTether = 0;

	/**
	 * Gauss-Seidel 上色：每种颜色一段 [X = Offset, Y = Count)，按求解顺序排（拉伸 + 弯曲的颜色在前，tether 的在最后）。
	 *
	 * 同一种颜色里的约束两两不共享可动粒子，一次派发内每条约束直接改写两端位置，没有竞争。计划 D3 原先选的是
	 * Jacobi + 定点原子累加（上色列在 P4），2026-10-26 实测不过 P0 的验收：每个粒子的修正要除以挂在它身上的
	 * 约束数（拉伸 6 + 弯曲 6 + tether 1），拉伸一个 substep 只修回约 1/9，21² 的悬挂布最大边长误差 12.8%、
	 * 181² 的超过 100%。上色后的实测见 Docs/GpuClothSim_Plan.md 的「实施状态」（21² 布最大 3.25%）。
	 */
	TArray<FIntPoint> ColorRanges;

	/** 每钉组：组内钉住粒子的 rest 包围盒（sim 空间），以及挂在这一组上的自由粒子离它最远能到多远（cm）。
	 *  包围盒 = 各组「钉住部分 × 当前组变换，再外扩 Reach」的并 —— 布料再怎么摆也出不了这个范围。 */
	TArray<FBox3f> PinGroupRestBounds;
	TArray<float> PinGroupReach;

	/** 连不到任何钉点的粒子（整片自由落体的布，或与钉点不连通的碎片）的 rest 包围盒。 */
	FBox3f UnanchoredRestBounds = FBox3f(ForceInit);

	FBox3f RestBounds = FBox3f(ForceInit);
	double TotalMassKg = 0.0;
	int32 NumPinned = 0;

	bool IsValid() const { return NumParticles > 0 && TriangleParticles.Num() >= 3; }

	/** 上传后常驻的显存（字节），供预算预检。与 CSClothSimRender.cpp 的分配式同步。 */
	int64 GetGpuBytes() const;
};

namespace CSClothSetup
{
	/**
	 * 从回读快照建粒子、约束与映射。快照必须是逐顶点属性（UCSMesh::ReadbackMeshSync 的产出，
	 * SourceSpace = World）；逐角点快照被拒。失败返回 false 并写日志，OutData 不可用。
	 */
	COMPUTESHADERGENERATOR_API bool BuildFromSnapshot(
		const FCSGpuMeshCPUData& Snapshot, const FCSClothSetupOptions& Options, FCSClothSetupData& OutData);

	/**
	 * 一块规则网格布（XY 平面、法线 +Z、中心在原点，再整体乘 Transform），写成可以直接
	 * UCSMeshOps::CopyFromMeshSnapshot 上传的快照。程序化布料与测试共用。
	 *
	 * bDoubleSided 追加一份翻转绕序、法线朝 −Z 的背面：引擎默认材质是单面的，没有背面的布从背后看是透明的。
	 * 背面与正面焊到同一批粒子上，setup 会把它们从约束与法线累加里剔掉（见 TriangleParticles）。
	 */
	COMPUTESHADERGENERATOR_API void BuildGridSnapshot(
		const FVector2D& Size, const FIntPoint& Quads, const FTransform& Transform, bool bDoubleSided,
		FCSGpuMeshCPUData& OutSnapshot);
}

#pragma once

#include "CoreMinimal.h"
#include "CSClothSim.h"
#include "CSGpuMeshTypes.h"
#include "CSClothBandSplit.generated.h"

// -----------------------------------------------------------------------------
// 带状分块：把一张三角网格沿「到起点的测地距离」切成一圈圈条带，每条带是一块独立的布。
//
// 移植自 Houdini 场景 AnimationFallSplit 的 CharSplit 链（Docs/HoudiniFallSplit_Port.md 有逐节点的对照）：
//   起点捕获（kalou_attribfromgeometry）→ 测地距离（distancealonggeometry）→
//   split = floor((5·aanoise(P·39) + 222·dist) × 0.1)（attribvop2）→ 面取三角最小值（attribpromote min）→
//   沿 split 变化处切开（splitpoints）→ 切口曲线平滑并带动附近的点（attribblur + attribwrangle18）→
//   连通分量与沿条带的参数（connectivity + kalou_createcurveuv）。
// 产物除了切开的网格，还有喂给 UCSClothSimComponent 的逐顶点绑定：焊接键（条带号，切口不被焊回去）、
// 锚点（原网格上的点，宿主每帧交原网格的动画位置，锚点就跟着动画走）、(High, 随机数, 正负号)、旋流方向。
// -----------------------------------------------------------------------------

USTRUCT(BlueprintType)
struct COMPUTESHADERGENERATOR_API FCSClothBandSplitSettings
{
	GENERATED_BODY()

	/** 网格上离起点 SeedRadius 以内的顶点是测地距离的源（Houdini：手画在身上的一条笔画，捕获半径 1 cm）。
	 *  写在源网格的局部空间里（cm）。为空时取最高的那个顶点（头顶）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split")
	TArray<FVector> SeedPoints;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.1", Units = "cm"))
	float SeedRadius = 2.0f;

	/** 条带宽度（沿测地距离）。Houdini：split = floor((5·n + 222·dist_m) × 0.1) ⇒ 10/222 m ≈ 4.5 cm。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.5", Units = "cm"))
	float BandWidth = 4.5f;

	/** 条带边界的噪声抖动：测地距离加上 ±它（Houdini：5/222 m × aanoise）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.0", Units = "cm"))
	float BoundaryNoise = 2.25f;

	/** 噪声的空间周期（Houdini：频率 39 /m ⇒ 约 2.6 cm）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.1", Units = "cm"))
	float BoundaryNoisePeriod = 2.6f;

	/** 切口曲线沿自身做多少次拉普拉斯平滑（Houdini attribblur：9 次、步长 0.643）。0 = 不平滑，切口沿三角边走、呈锯齿。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0", ClampMax = "64"))
	int32 BoundarySmoothIterations = 9;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BoundarySmoothStep = 0.643f;

	/** 切口平滑带动附近顶点的范围（线性衰减；Houdini attribwrangle18 的 maxdist 0.044 m）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split", meta = (ClampMin = "0.0", Units = "cm"))
	float SmoothFalloff = 4.4f;

	/** 噪声相位与逐点随机数（锚点绳长）的种子（Houdini attribrandomize 的种子是 8278）。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split")
	int32 Seed = 8278;

	/** 追加翻转的背面：条带会翻过来，单面材质从背后看是透明的。背面焊到同一批粒子上，不进约束。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Band Split")
	bool bDoubleSided = true;
};

struct COMPUTESHADERGENERATOR_API FCSClothBandSplitResult
{
	/** 切开后的网格（逐顶点布局，与输入同一空间）。 */
	FCSGpuMeshCPUData Mesh;

	/** 每输出顶点的绑定：WeldKeys = 条带号；AnchorSources / AnchorWeights = 输入网格的渲染顶点及权重（宿主每帧按
	 *  输入网格的顶点序交位置）；AnchorParams = (High, RandomU, ReleaseSign, 0)；SwirlDirs = 沿条带的旋流方向。 */
	FCSClothVertexBinding Binding;

	/** 每输出顶点：测地距离（cm）与所在块号（连通分量）。调试与测试用。 */
	TArray<float> Distance;
	TArray<int32> PieceIds;

	int32 NumBands = 0;
	int32 NumPieces = 0;
	int32 NumSeedVertices = 0;
	/** 被切口平滑挪动过的顶点数（它们的锚点落在原网格的三角内部）。 */
	int32 NumSmoothedVertices = 0;
};

namespace CSClothBandSplit
{
	/**
	 * Source：逐顶点布局的三角网格；SeedPoints 与它同一空间（覆盖 Settings.SeedPoints，调用方负责变换）。
	 * 失败（网格无效、全退化）返回 false 并写日志。
	 */
	COMPUTESHADERGENERATOR_API bool Split(const FCSGpuMeshCPUData& Source, const FCSClothBandSplitSettings& Settings,
		const TArray<FVector>& SeedPoints, FCSClothBandSplitResult& OutResult);
}

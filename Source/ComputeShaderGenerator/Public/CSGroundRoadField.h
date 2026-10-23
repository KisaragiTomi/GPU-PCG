#pragma once

#include "CoreMinimal.h"
#include "CSMesh.h"
#include "RenderGraphResources.h"

class FRDGBuilder;
class UTextureRenderTarget2D;

/**
 * 地面「路场」：把地面常驻色流的一个通道（默认 R = 道路权重）糊成一张平滑场，一个地面格点一个 float
 * （V = Y * VertsX + X，与色流同一张格）。两趟可分离高斯，kernel 在 `CSGroundRoadField.usf`。
 *
 * 路权重是逐顶点画的，格距 50 cm ⇒ 路缘在一格之内从 0 跳到 1，谁直接读色流谁就在路缘一刀切。
 * 两个消费者共用这一份（分两份的话，将来只有一份会跟着核的口径改，漂掉的那份不报错）：
 *   · 岩壳：`PreGain = RoadFade` 先截足迹再糊（顺序承重，见 usf 头注释），`PostGain = 1`，场在图内临时 buffer；
 *   · 地面遮罩：`PreGain = 1` 直接糊，`PostGain = 2` 把画出来的路缘推回 1、再与源值取 max ⇒ 路缘以内恒 1、
 *     路缘以外平滑衰减。写进 `ACSGroundActor::GroundMask` 这张贴图的 R，地面材质与地被 / 灌木都读它；
 *     G 另存只糊不加工的场（路缘处 0.5、往路中间升到 1）= TG 的 path_mask_blurred，材质拿它做深浅土的高度竞争。
 *
 * 从 GPU 色流现糊、不走 CPU 上传 —— 色流就是笔刷双写出来的那份权威投影，"画面上看到的路"与
 * 消费方的判据因此构造上同源。第三条路迟早与另外两条错开一帧。
 */
namespace CSGroundRoadField
{
/** 单侧抽头上限（格）。50 cm 格距下 = 16 m 伸展，远超任何有意义的缓坡；只挡"半径填错一个数量级"。 */
constexpr int32 MaxTaps = 32;

struct FBlurDesc
{
	/** 地面格点数（= 色流的逻辑二维形状）。 */
	FIntPoint GroundVerts = FIntPoint(0, 0);
	float GroundCellSize = 50.0f;
	/** 读色流的哪一个通道：one-hot（RGBA）。 */
	FVector4f Channel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);
	/** 读出来先 `saturate(值 × PreGain)` 再糊（岩壳 = RoadFade，把足迹截出来）。 */
	float PreGain = 1.0f;
	/** 糊完再 `saturate(值 × PostGain)`（地面遮罩 = 2，把糊开后的 0.5 等值线 = 路缘推回 1）。 */
	float PostGain = 1.0f;
	/** 核的单侧伸展（cm），σ = 本值 / 3，截在 3σ；0 = 不糊（只做两道增益）。按 `MaxTaps` 格截顶。 */
	float Radius = 0.0f;
	/** 结果与源值（`saturate(通道 × PreGain)`）取 max：延伸只往外加，不把窄路、路头糊浅。 */
	bool bMaxWithSource = false;
};

/**
 * 录两趟模糊，返回场（`Buffer<float>`，`GroundVerts.X × GroundVerts.Y` 个）。录完直接返回，不阻塞。
 *
 * 格点数退化（< 2×2）或色流装不下整张格时不糊，直接给一块清零的场 —— 读到 0 就是"没有路"。
 * ⚠️ **不能不写**：没被写过的 RDG buffer 被读，内容是池子上一位租客的字节。
 *
 * `DebugName` 必须活过整张图（传字面量）。
 */
COMPUTESHADERGENERATOR_API FRDGBufferRef AddBlurPasses(
	FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors, const FBlurDesc& Desc, const TCHAR* DebugName);

/**
 * 同上，但 Y 趟直接写进 `Target`（尺寸必须 = `GroundVerts`，RG 两通道 float 格式、可建 UAV）：
 * R = 与 buffer 出口逐位相同的成品，G = `saturate(模糊)`（不 ×PostGain、不与源值取 max）。
 * 格点数退化或尺寸对不上时把 `Target` 清零。
 */
COMPUTESHADERGENERATOR_API void AddBlurPassesToTexture(
	FRDGBuilder& GraphBuilder, FRDGBufferRef GroundColors, const FBlurDesc& Desc, FRDGTextureRef Target);

/**
 * 地面遮罩的整趟更新：开一张 RDG 图，读 `Ground` 的色流、糊好写进 `Target`，末态留在 SRV（材质采样）。
 * 录完直接返回，不阻塞。`Target` 必须 `bCanCreateUAV` 且尺寸 = `Desc.GroundVerts`。返回是否录了图。
 */
COMPUTESHADERGENERATOR_API bool UpdateMaskTexture(
	const FCSMeshResidentRef& Ground, UTextureRenderTarget2D* Target, const FBlurDesc& Desc);
}

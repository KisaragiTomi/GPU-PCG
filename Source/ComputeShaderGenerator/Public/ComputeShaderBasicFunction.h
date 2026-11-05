#pragma once
//
#include "CoreMinimal.h"
#include "ComputeShaderGeneral.h"
#include "GenericPlatform/GenericPlatformMisc.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/BoxComponent.h"
#include "Components/SplineComponent.h"
#include "TextureResource.h"
#include "RenderGraphBuilder.h"
#include "Util/ColorConstants.h"

class ACSCliffGenerateCapture;
class ACSPlaneRangeGenerator;

#include "ComputeShaderBasicFunction.generated.h"


/**
 * Unified blend mode for all CS landscape edit layers (Temp / Layer / Landscape).
 * Integer order MUST stay in sync with the TempLayerBlend branch in CSLandscape.usf.
 */
UENUM(BlueprintType)
enum class ECSLandscapeBlendMode : uint8
{
	Alpha         UMETA(DisplayName = "Alpha Lerp (Lerp toward generated height)"),
	Override      UMETA(DisplayName = "Override (Replace within alpha mask)"),
	Additive      UMETA(DisplayName = "Additive (Height += Delta * Alpha)"),
	Subtract      UMETA(DisplayName = "Subtract (Height -= Delta * Alpha)"),
	Multiply      UMETA(DisplayName = "Multiply (Height *= Generated * Alpha + (1-Alpha))"),
	MaterialDrive UMETA(DisplayName = "Material Driven (Per-pixel blend from material texture)")
};

USTRUCT()
struct COMPUTESHADERGENERATOR_API FCSReadLandscapeData
{
	GENERATED_BODY()
	TArray<FLinearColor> Colors;
	TArray<FFloat16Color> Colors16;
	TArray<FLinearColor> ValidColors;
	FIntVector4 ReadRange = FIntVector4(0, 0, 0, 0);
	FIntVector2 TextureSize = FIntVector2(0, 0);
	FIntVector2 TextureValidSize = FIntVector2(0, 0);
	FVector2f ValidUVRange = FVector2f::Zero();
	FVector MapMin = FVector::ZeroVector;
	FVector MapMax = FVector::ZeroVector;
	FVector ValidMapMin = FVector::ZeroVector;
	FVector ValidMapMax = FVector::ZeroVector;
	FTransform Transform = FTransform::Identity;
	FBoxSphereBounds ValidTextureBounds;
	FBoxSphereBounds TextureBounds;
};

UCLASS()
class COMPUTESHADERGENERATOR_API UComputeShaderBasicFunction : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorsToRenderTarget32(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> Colors);


	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorsToRenderTarget16(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> Colors);
	
	static void DrawFFloat16ColorsToRenderTarget(UTextureRenderTarget2D* InTextureTarget, TArray<FFloat16Color> Colors16);
	
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void ConnectivityPixel(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* InConnectivityMap, UTextureRenderTarget2D
	                              * InDebugView, int32 Channel = 2, int32 TextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void BlurTexture(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutBlurTexture, float BlurScale = 1);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void BlurNormalTexture(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutBlurTexture, float BlurScale = .5);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void UpPixelsMask(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* OutUpTexture, float Threshold = .8, int32 Channel = 0);
	
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void ExtentMaskFast(UTextureRenderTarget2D* InTextureTarget, UTextureRenderTarget2D* InDebugView, int32 Channel = 0, int32 NumExtend = 1);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void DrawLinearColorToRenderTarget(UTextureRenderTarget2D* InTextureTarget, TArray<FLinearColor> InColors);
	
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CalTreeWindTexture(UTextureRenderTarget2D* InWindTexture_PivotIndex, UTextureRenderTarget2D* InWindTexture_DirExtent, UTextureRenderTarget2D
	                               * InDebugView, TArray<FVector4f> RootCenter4f, TArray<FVector4f> RootNormal4f, int32 TextureSizeX, int32 TextureSizeY);

	//SampleDistRotate: xy = dir, z = dist, w = rotate
	//SampleGradientHeight: xy = gradient, z = height
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static TArray<FTransform> SampleSpline(UTextureRenderTarget2D* InSampleDistRotate, UTextureRenderTarget2D* InSampleGradientHeight,
	                                       UTextureRenderTarget2D* InDebugView, TArray<USplineComponent*>
	                                       InSplineComponents, FBoxSphereBounds Bounds, int32 TextureSize = 256);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void GenerateMapCliff(TSubclassOf<ACSPlaneRangeGenerator> ManagerClass, TArray<TSubclassOf<ACSCliffGenerateCapture>> GeneratorClass, ACSPlaneRangeGenerator
	                             *& GeneratorManager);
	
	// ─── 通用平滑曲线（三次 B-Spline）：曲线逼近控制点，段间 C2 连续 ───────────

	/** RDG 流程版：在 GPU 上对一组控制点做三次 B-Spline 重采样。
	 *  ControlPoints: xyz=世界位置（w 备用）；NumSamples: 输出采样点数。
	 *  输出两个 buffer：OutPositions(xyz=位置,w=有效) 与 OutTangents(xyz=切线方向)。
	 *
	 *  ⚠️ **2026-08-30「裁决一」点名要删这个函数，但它没删** —— TinyGlade 那两个消费者
	 *  （门框旧路 `BuildFramePlan`、地形石阶旧路 `CSShaperSteps::Scatter`）确实都删掉了，
	 *  可它还有第三个、与石阶无关的消费者：下面那个蓝图可调的 `DrawSmoothSplinePoints`，
	 *  而 `Content/SpaceColonization/NewBlueprint.uasset` 的图里有一个 `K2Node_CallFunction`
	 *  真的在调它。删掉会让那张蓝图编译不过。要不要连 `DrawSmoothSplinePoints` 一起删，
	 *  超出裁决一的范围，留给驱动方拍板。 */
	static void RDG_SmoothSpline(
		FRDGBuilder& GraphBuilder,
		const TArray<FVector4f>& ControlPoints,
		int32 NumSamples,
		FRDGBufferRef& OutPositions,
		FRDGBufferRef& OutTangents);

	/** 蓝图版：输入一条 Spline，在 GPU 上做三次 B-Spline 平滑重采样，
	 *  读回结果后用 DrawDebugPoint 画位置、DrawDebugArrow 画切线方向，默认停留 5 秒。 */
	UFUNCTION(BlueprintCallable, Category = "ComputeShader", meta = (WorldContext = "WorldContextObject"))
	static void DrawSmoothSplinePoints(
		UObject* WorldContextObject,
		USplineComponent* InSpline,
		int32 NumSamples = 64,
		float Duration = 5.f,
		float PointSize = 12.f,
		float ArrowLength = 50.f);

	// ─── GSDF 蓝图入口（纯异步，fire-and-forget）─────────────────────
	// GSDF 参数数据只存在于渲染器内部的真 FViewInfo 上，且只在 base pass 之后才就绪，
	// 任何手工构造的 FSceneView 都拿不到。因此这两个入口只负责把采样请求投递给
	// FGDFSampleService（内部 SceneViewExtension + 异步 readback），结果就绪后在游戏
	// 线程回调里用 DrawDebug 点阵/线段直接可视化。它们不返回值、不写 RenderTarget。

	/** 盒内 Resolution^3 均匀点采样 GSDF，结果就绪后按阈值用 DrawDebugPoint 画出。 */
	UFUNCTION(BlueprintCallable, Category = "DistanceField", meta = (WorldContext = "WorldContextObject"))
	static void DebugDrawDistanceFieldInBox(
		UObject* WorldContextObject,
		FVector BoxCenter,
		FVector BoxExtent,
		int32 Resolution = 16,
		float DistanceThreshold = 50.0f,
		float PointSize = 8.0f,
		float Duration = 5.0f,
		bool bColorByDistance = true);

	/** 旋转盒内体素化 GSDF，把每 XY 列的空腔压成 cavity span（CSR），结果就绪后用竖直线段画出。 */
	UFUNCTION(BlueprintCallable, Category = "DistanceField", meta = (WorldContext = "WorldContextObject"))
	static void DebugDrawVoxelCavitySpansInBox(
		UObject* WorldContextObject,
		FVector BoxCenter,
		FVector BoxExtent,
		FRotator BoxRotation,
		int32 GridX = 64,
		int32 GridZ = 64,
		float OccupancyThreshold = 0.0f,
		float Duration = 5.0f,
		float LineThickness = 2.0f);

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void CopyTexture(UTextureRenderTarget2D* InOrig, UTextureRenderTarget2D* InCopy);
	

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void Test(UTexture2DArray* InArray, UTexture2D* InTexture, UTextureRenderTarget2D* InDebugView);
#endif

	UFUNCTION(BlueprintCallable, Category = "ComputeShader")
	static void SyncRenderThread();
	
};


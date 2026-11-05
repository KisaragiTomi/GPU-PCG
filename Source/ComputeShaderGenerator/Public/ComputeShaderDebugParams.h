// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ComputeShaderDebugParams.generated.h"

// =============================================================================
// VisVine Debug Parameter Structs (moved from GeometryScriptExtraEditor)
// =============================================================================

USTRUCT(BlueprintType, meta = (DisplayName = "GPU Projection Voxel Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineGPUProjectionDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bDrawGPUProjectionVoxelDebugPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 GPUProjectionVoxelDebugPointLimit = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelDebugDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bGPUProjectionVoxelDebugPointsPersistent = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelCenterPointSize = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float GPUProjectionVoxelTargetPointSize = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor GPUProjectionVoxelCenterColor = FLinearColor(1.0f, 0.85f, 0.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor GPUProjectionVoxelTargetColor = FLinearColor(1.0f, 0.0f, 0.1f, 1.0f);
};

USTRUCT(BlueprintType, meta = (DisplayName = "Surface Voxel Debug"))
struct COMPUTESHADERGENERATOR_API FVisVineSurfaceVoxelDebugOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelArrowColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowDuration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelArrowThickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelArrowPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawVoxelCenters = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelCenterColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelCenterPointSize = 6.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawWeightedTargets = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelWeightedTargetColor = FLinearColor::Red;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0.0"))
	float SurfaceVoxelWeightedTargetPointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	bool bSurfaceVoxelDrawCenterToTargetLines = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug")
	FLinearColor SurfaceVoxelCenterToTargetColor = FLinearColor(0.0f, 1.0f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VisVine|Debug", meta = (ClampMin = "0"))
	int32 SurfaceVoxelMaxArrowsToDraw = 0;
};

// Which intermediate stage of the VisVineGPU pipeline to read back and visualize as the
// center line. The SurfaceTarget center line evolves across these GPU passes:
//   FP  : after the final surface projection (FinalProject)
//   RS  : after the arc-length resample (ResampleSurface)
//   B   : after the smoothing ping-pong (final geometry center line)
//
// 目前 C++ 侧没有任何消费者：选中阶段的唯一去处是 FVineMeshSceneProxy 画的中心线叠加，那层
// 叠加随 proxy 一起退休了。保留本枚举纯粹是因为 BP_VineSource 仍然引用它——删掉会让蓝图断连，
// 而它本身不占运行时开销。哪天要恢复中心线可视化，从这里接回去。
UENUM(BlueprintType)
enum class EVisVineGPUDebugStage : uint8
{
	FinalProject UMETA(DisplayName = "FP - 最终投射后"),
	Resample     UMETA(DisplayName = "RS - 弧长重采样后"),
	Smooth       UMETA(DisplayName = "B - 平滑后 (最终中心线)"),
	None         UMETA(DisplayName = "无 - 只输出最终网格 (不画任何调试线)")
};

// FVisVineSplineDebugOptions 已删除。它唯一的消费者是 FVineMeshSceneProxy 自持顶点工厂画的那层
// GPU 中心线叠加；藤蔓几何迁到 UCSMesh 后该 proxy 不复存在，绑定别人网格的渲染组件也没地方放这条
// 额外的线段流。

// =============================================================================
// ComputeShaderMeshGenerator Debug Parameter Structs
// =============================================================================

/** Debug draw options for DrawDebugLastSurfaceVoxelDirections. */
USTRUCT(BlueprintType, meta = (DisplayName = "CS Last Voxel Direction Debug"))
struct COMPUTESHADERGENERATOR_API FCSDebugLastVoxelDirectionOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float DirectionLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor DirectionColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Duration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Thickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bDrawPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor PointColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float PointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0"))
	int32 MaxDirectionsToDraw = 0;
};

/** Debug draw options for DrawDebugBoxSceneSurfaceVoxelDirections. */
USTRUCT(BlueprintType, meta = (DisplayName = "CS Box Voxel Direction Debug"))
struct COMPUTESHADERGENERATOR_API FCSDebugBoxVoxelDirectionOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.001"))
	float VoxelSize = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float DirectionLength = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor DirectionColor = FLinearColor::Blue;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Duration = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float Thickness = 2.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bPersistentLines = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	bool bDrawPoints = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw")
	FLinearColor PointColor = FLinearColor::Yellow;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0.0"))
	float PointSize = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CS Debug|Draw", meta = (ClampMin = "0"))
	int32 MaxDirectionsToDraw = 0;
};


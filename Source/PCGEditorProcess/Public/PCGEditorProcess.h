// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Modules/ModuleManager.h"
#include "ComputeShaderDebugParams.h"

class AComputeShaderMeshGenerator;
class ACSCityFieldActor;
class ACSGroundActor;
class ACSPointBrushActor;
class AGPUSkeletalTree;
class FCSHouseResizeSelectionWatcher;
class FViewEditCategoryViewportOverlay;

class FPCGEditorProcessModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void InitializeEditorUI();
	void StartInstanceBrush(AComputeShaderMeshGenerator* TargetActor);
	void StartPointBrush(ACSPointBrushActor* TargetActor);
	void StartGroundPaint(ACSGroundActor* TargetActor);
	void StartCityFieldPaint(ACSCityFieldActor* TargetActor);

	/** 应答 `ACSHouseActor::OnWindowBrushRequest`：激活窗笔刷并把目标房交给它。 */
	void StartWindowBrush(class ACSHouseActor* TargetActor);
	void GenerateGPUSkeletalTree(AGPUSkeletalTree* TargetActor);

	FDelegateHandle PostEngineInitHandle;
	/** D5 拉尺寸：失选即调宿主房 ExitResizeMode。runtime 请求、editor 应答。 */
	TUniquePtr<FCSHouseResizeSelectionWatcher> HouseResizeSelectionWatcher;
	TUniquePtr<FViewEditCategoryViewportOverlay> ViewEditCategoryViewportOverlay;
	bool bEditorModeRegistered = false;
	bool bPointBrushModeRegistered = false;
	bool bGroundPaintModeRegistered = false;
	bool bCityFieldPaintModeRegistered = false;
	bool bWindowBrushModeRegistered = false;
};

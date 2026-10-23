#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "CSStaticMeshBuildLibrary.generated.h"

class UStaticMesh;

/**
 * 静态网格构建方式的编辑器小工具（2026-09-29）。
 */
UCLASS()
class COMPUTESHADERGENERATOR_API UCSStaticMeshBuildLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()

public:
	/**
	 * 把一张网格从「快速构建」里放出来，按标准路径重建一次。编辑器专用；返回它之前是不是快速构建（不是就什么都不做）。
	 *
	 * 为什么需要：`UStaticMesh::bDoFastBuild` 是一面**存盘**的旗子。脚本用
	 * `build_from_static_mesh_descriptions(..., fast_build=True)` 建过一次，它就恒为真，之后任何重建（包括在资产上勾 Nanite）
	 * 都走快速路径 —— 直接写渲染数据，**不建 Nanite 数据**：`NaniteTriangles` 恒为 0，Nanite 开关开着却退回引擎 ISM 代理，
	 * 日志里连 `NaniteBuild` 那一行都没有（`SM_TG_GrassClump` 实测）。引擎只在构造函数里清它；Python 读写不到（protected），
	 * 编辑器里 `SET` 命令也被禁了，所以在这里用反射清。
	 *
	 * 清完还会重新提交一次 MeshDescription（换一个 GUID）：快速路径建出来的那份也写进了 DDC、与标准路径同一个缓存键，
	 * 不换键的话标准路径直接命中那份没有 Nanite 的缓存。
	 *
	 * 前提：源模型里有 MeshDescription（快速构建默认会提交它）；没有就不动、打一条警告。
	 */
	UFUNCTION(BlueprintCallable, Category = "CS|Static Mesh", meta = (DevelopmentOnly))
	static bool ClearFastBuildAndRebuild(UStaticMesh* Mesh);
};

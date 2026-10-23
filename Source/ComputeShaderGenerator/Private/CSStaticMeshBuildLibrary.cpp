#include "CSStaticMeshBuildLibrary.h"

#include "Engine/StaticMesh.h"
#include "UObject/UnrealType.h"   // FBoolProperty / FindFProperty

DEFINE_LOG_CATEGORY_STATIC(LogCSStaticMeshBuild, Log, All);

bool UCSStaticMeshBuildLibrary::ClearFastBuildAndRebuild(UStaticMesh* Mesh)
{
#if WITH_EDITOR
	if (!Mesh) return false;

	// protected 的位域 bool：反射按名字找，FBoolProperty 自己处理位掩码。
	const FBoolProperty* Flag = FindFProperty<FBoolProperty>(UStaticMesh::StaticClass(), TEXT("bDoFastBuild"));
	if (!Flag)
	{
		UE_LOG(LogCSStaticMeshBuild, Warning, TEXT("UStaticMesh 上找不到 bDoFastBuild（引擎改名了？），%s 不动。"), *GetNameSafe(Mesh));
		return false;
	}
	if (!Flag->GetPropertyValue_InContainer(Mesh)) return false;

	if (!Mesh->IsMeshDescriptionValid(0))
	{
		UE_LOG(LogCSStaticMeshBuild, Warning, TEXT("%s 是快速构建的，但源模型里没有 MeshDescription —— 标准路径没东西可建，不动。"), *GetNameSafe(Mesh));
		return true;
	}

	Mesh->Modify();
	Flag->SetPropertyValue_InContainer(Mesh, false);
	// ⚠️ 只清旗子不够：快速路径建出来的那份也写进了 DDC，而且与标准路径**同一个缓存键**（键里没有这面旗子）——
	// 标准路径会直接命中那份没有 Nanite 的缓存，日志里连 NaniteBuild 都不出（2026-09-29 SM_TG_GrassClump 实测）。
	// 重新提交一次 MeshDescription：默认参数不拿哈希当 GUID ⇒ 新 GUID ⇒ 新键 ⇒ 真建一次。
	Mesh->CommitMeshDescription(0, UStaticMesh::FCommitMeshDescriptionParams());
	// 标准路径重建：Nanite 开着就建 Nanite 数据；构建结束广播 OnPostMeshBuild，引用它的实例组件自己重判走哪条路。
	Mesh->Build(/*bInSilent*/ true);
	Mesh->MarkPackageDirty();
	UE_LOG(LogCSStaticMeshBuild, Log, TEXT("%s：清掉快速构建标记并按标准路径重建（Nanite=%d）。"), *GetNameSafe(Mesh), Mesh->IsNaniteEnabled() ? 1 : 0);
	return true;
#else
	return false;
#endif
}

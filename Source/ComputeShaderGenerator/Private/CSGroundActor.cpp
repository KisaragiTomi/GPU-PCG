#include "CSGroundActor.h"
#include "CSGroundCollisionComponent.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundRoadField.h"   // 地面遮罩：色流 R → 平滑 + 延伸，写进 GroundMask
#include "CSGroundRockShell.h"
#include "CSGroundShaperActor.h"
#include "CSGroundShaperField.h"
#include "CSHouseVine.h"      // BuildBaseMesh：裙边摆件与房子那四家共用同一份基础网格读取器
#include "CSMesh.h"
#include "CSMeshRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"   // 地面遮罩贴图
#include "EngineUtils.h"
#include "TextureResource.h"                 // 地面遮罩的诊断回读（FTextureRenderTargetResource）
#include "Materials/Material.h"          // bUsedWithInstancedStaticMeshes —— 三条"静默换材质"里的一条
#include "Materials/MaterialInstanceDynamic.h"   // 岩壳绘制材质：RockShellMaterial 的子实例，带 RockShellPatternScale
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"                    // 裙边摆件的幂等哈希（unity 会掩盖，-SingleFile 才照得出来）
#include "UObject/ConstructorHelpers.h"  // 地被远处组合草的默认资产（SM_TG_GrassClump / MI_TG_GrassFar）
#if WITH_EDITOR
#include "StaticMeshAttributes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeGround, Log, All);

FCSGroundPaintEditorRequest ACSGroundActor::OnGroundPaintEditorRequest;
FCSGroundChanged ACSGroundActor::OnAnyGroundRebuilt;

namespace
{
// Unity/jumbo builds share a TU, so file-local names carry a CSGround_ prefix.

/** CPU twin of PaintVertexColorsSphereCS's falloff weight. Same constants, same smoothstep —
 *  the mirror stays bit-stable against the GPU because both read 8-bit storage, run this exact
 *  math, and round back to 8 bits. Change it together with the kernel and ECSMeshPaintBlendOp. */
float CSGround_BrushWeight(float Distance, float Radius, float Falloff)
{
	const float HardRadius = Radius * (1.0f - Falloff);
	const float T = FMath::Clamp((Radius - Distance) / FMath::Max(Radius - HardRadius, 1e-3f), 0.0f, 1.0f);
	return T * T * (3.0f - 2.0f * T);
}

/** CPU twin of the kernel's per-channel blend. C/Paint/E are RGBA in 0..1. */
FLinearColor CSGround_BlendColor(const FLinearColor& C, const FLinearColor& Paint, const FLinearColor& E, ECSMeshPaintBlendOp Op)
{
	auto Blend = [Op](float Channel, float PaintChannel, float Amount)
	{
		switch (Op)
		{
		case ECSMeshPaintBlendOp::Replace: return FMath::Lerp(Channel, PaintChannel, Amount);
		case ECSMeshPaintBlendOp::Add:     return Channel + PaintChannel * Amount;
		case ECSMeshPaintBlendOp::Max:     return FMath::Max(Channel, PaintChannel * Amount);
		case ECSMeshPaintBlendOp::Erase:   return Channel * (1.0f - Amount);
		default:                           return Channel;
		}
	};
	return FLinearColor(
		Blend(C.R, Paint.R, E.R),
		Blend(C.G, Paint.G, E.G),
		Blend(C.B, Paint.B, E.B),
		Blend(C.A, Paint.A, E.A));
}

/** Mirror storage quantisation, the CPU twin of PackColorBGRA's round(saturate(x) * 255). */
FColor CSGround_QuantizeColor(const FLinearColor& Value)
{
	auto PackByte = [](float Channel)
	{
		return uint8(FMath::RoundToInt(FMath::Clamp(Channel, 0.0f, 1.0f) * 255.0f));
	};
	return FColor(PackByte(Value.R), PackByte(Value.G), PackByte(Value.B), PackByte(Value.A));
}

FLinearColor CSGround_UnquantizeColor(const FColor& Stored)
{
	return FLinearColor(
		float(Stored.R) / 255.0f,
		float(Stored.G) / 255.0f,
		float(Stored.B) / 255.0f,
		float(Stored.A) / 255.0f);
}
}

ACSGroundActor::ACSGroundActor()
{
	// 草与花共用 `FCSGroundCoverSpecies`，而那个结构体的 `bCastShadow` 声明默认值是 true
	// （2026-09-12 裁决：花要有投影）。草满密度 50 株/m²，投影是这条路上最贵的一项，所以
	// 草这一份在这里压回 false —— 逐成员默认值没有别的落点。完整理由见
	// `FCSGroundCoverSpecies::bCastShadow` 的注释。
	// ⚠️ 别把这一行换成"给 Flowers 塞一个默认元素"：空数组正是"没有花"的表达方式。
	Grass.bCastShadow = false;

	// 草默认分近 / 远两档（2026-09-22 用户："远处使用组合草，近处使用单根草，远处只有法线运动没有 WPO"）。
	// 远处的两样资产由 `Scripts/TinyGladeMakeGrassClump.py` 造；找不到就是 null ⇒ 两档不生效、退回整张地面一档，
	// 不会出现"远处一根草都没有"。`FObjectFinderOptional`：找不到只是没有默认值，不把 CDO 构造带崩。
	Grass.LOD.bEnabled = true;
	{
		static ConstructorHelpers::FObjectFinderOptional<UStaticMesh> ClumpMesh(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Meshes/SM_TG_GrassClump"));
		static ConstructorHelpers::FObjectFinderOptional<UMaterialInterface> FarMaterial(TEXT("/PCGPlugins/HouseTest/TinyGladeAsset/Materials/MI_TG_GrassFar"));
		Grass.LOD.FarMesh = ClumpMesh.Get();
		Grass.LOD.FarMaterial = FarMaterial.Get();
	}

	// 地被近处窗口要跟相机（`Tick` → `UpdateGroundCoverView`）。没有开两档的物种时 Tick 只比一下就返回。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	GroundCollision = CreateDefaultSubobject<UCSGroundCollisionComponent>(TEXT("GroundCollision"));
	GroundCollision->SetupAttachment(RootComponent);
}

FVector2D ACSGroundActor::ResolveCoverViewXY() const
{
	if (CoverViewOverride.IsSet()) return CoverViewOverride.GetValue();
	if (bCoverViewValid) return CoverViewXY;
	return GetWorldRect2D().GetCenter();
}

void ACSGroundActor::SetGroundCoverViewOverride(FVector WorldLocation)
{
	CoverViewOverride = FVector2D(WorldLocation.X, WorldLocation.Y);
	CoverViewOverrideLocation = WorldLocation;
	// Nanite 路的距离带也按这个机位判（场景捕获不往世界的视点表里填，替身自己找不到相机）。
	// 组件重建时 EnsureCoverComponents 会再推一次；这里先推给现有的，窗口哈希没变、不重撒时也生效。
	for (UCSGpuInstancedMeshComponent* Component : CoverComponents) if (IsValid(Component)) Component->SetDistanceCullViewOverride(WorldLocation);
#if WITH_EDITOR
	RebuildGroundCover();
#endif
}

void ACSGroundActor::ClearGroundCoverViewOverride()
{
	if (!CoverViewOverride.IsSet()) return;
	CoverViewOverride.Reset();
	for (UCSGpuInstancedMeshComponent* Component : CoverComponents) if (IsValid(Component)) Component->ClearDistanceCullViewOverride();
#if WITH_EDITOR
	RebuildGroundCover();
#endif
}

FVector ACSGroundActor::GetGroundCoverViewCenter() const
{
	const FVector2D XY = ResolveCoverViewXY();
	return FVector(XY.X, XY.Y, GetActorLocation().Z);
}

void ACSGroundActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	UpdateGroundCoverView();
}

void ACSGroundActor::UpdateGroundCoverView()
{
#if WITH_EDITOR
	if (!bGroundCoverEnabled || CoverViewOverride.IsSet() || IsTemplate()) return;

	// 重撒步长取开着两档的物种里最小的那个（没有开两档的物种 ⇒ 什么都不做）。
	float Step = TNumericLimits<float>::Max();
	auto Consider = [&Step](const FCSGroundCoverSpecies& One)
	{
		if (One.Mesh && One.LOD.bEnabled && One.LOD.FarMesh) Step = FMath::Min(Step, FMath::Max(One.LOD.RescatterDistance, 100.0f));
	};
	Consider(Grass);
	for (const FCSGroundCoverSpecies& One : Flowers) Consider(One);
	if (Step == TNumericLimits<float>::Max()) return;

	// 上一帧画过这个世界的透视视点（编辑器透视视口 / 游戏视口都往这里填；正交视口不填）。
	// ⚠️ 它在 UWorld::Tick 的末尾清空、画的时候重填 ⇒ actor Tick 时读到的正是上一帧的相机。
	const UWorld* World = GetWorld();
	if (!World || World->ViewLocationsRenderedLastFrame.IsEmpty()) return;
	// ⚠️ **不能直接取 [0]**：同一个世界开着多个透视视口时这张表里有好几项，而它们的**次序随绘制
	// 次序变**。取 [0] 会让窗口在两个机位之间来回跳 —— 每跳一次就是整窗重撒一趟，编辑器逐帧卡死，
	// 而且没有任何报错。取**离当前窗口最近的那一项**：次序怎么变都选中同一个视口，而用户真把某个
	// 视口开走时，最近的那一项自然跟着它走。
	FVector2D View(World->ViewLocationsRenderedLastFrame[0].X, World->ViewLocationsRenderedLastFrame[0].Y);
	if (bCoverViewValid)
	{
		double Best = TNumericLimits<double>::Max();
		for (const FVector& One : World->ViewLocationsRenderedLastFrame)
		{
			const FVector2D Candidate(One.X, One.Y);
			const double Distance = FVector2D::DistSquared(Candidate, CoverViewXY);
			if (Distance >= Best) continue;
			Best = Distance;
			View = Candidate;
		}
	}
	if (bCoverViewValid && FVector2D::Distance(View, CoverViewXY) < double(Step)) return;

	CoverViewXY = View;
	bCoverViewValid = true;
	// 整趟重散（一张 RDG 图、不阻塞：窗口大小不变 ⇒ 容量与包围盒都不变 ⇒ 不交接）。
	// 窗口贴着地面边时相机再往外走窗口不动 ⇒ 哈希不变 ⇒ 短路，不算一次搬动。
	const uint32 Before = CoverBuiltHash;
	RebuildGroundCover();
	if (CoverBuiltHash != Before) ++CoverWindowMoveCount;
#endif
}

void ACSGroundActor::RefreshGroundCollision()
{
	if (GroundCollision) GroundCollision->RebuildFromMirror(Mirror);
}

void ACSGroundActor::StartVertexColorPaint()
{
	OnGroundPaintEditorRequest.Broadcast(this);
}

bool ACSGroundActor::EnsureMirrorInitialized()
{
	// ⚠️ 模板（CDO / 蓝图原型）**永远不碰镜像**。镜像是按增量序列化的：关卡里的实例只存"与原型
	// 不同"的字段，格数与原型相同时 `NumVertsX/Y` 根本不落盘，加载时取原型的值。原型的镜像一旦
	// 变了尺寸（09-21 实例：蓝图 CDO 经 PostEditChangeProperty 从 257² 长成 1025²），那些实例
	// 加载后就是"原型的尺寸 + 自己的数组"，判为未初始化、整片重置 —— 画过的路全没了。
	// 地面蓝图 CDO 里现存的那份 257² 平地镜像是早先留下的，**别清它**：它正是现存关卡实例的增量基准。
	if (IsTemplate()) return false;

	const int32 WantVertsX = NumCellsX + 1;
	const int32 WantVertsY = NumCellsY + 1;
	const bool bMatches = Mirror.IsInitialized()
		&& Mirror.NumVertsX == WantVertsX
		&& Mirror.NumVertsY == WantVertsY
		&& FMath::IsNearlyEqual(Mirror.CellSize, CellSize);
	if (bMatches)
	{
		// MaxAbsHeight 是 transient 的：反序列化出的镜像在这里恢复它，RaycastGround 的
		// 平面/march 分路才不会对着旧值走错。
		MaxAbsHeight = 0.0f;
		for (const float Height : Mirror.Heights) MaxAbsHeight = FMath::Max(MaxAbsHeight, FMath::Abs(Height));
		return false;
	}

	// 只改格数、格距不变：同一张网格从左下角（actor 原点）往外长或往里缩，重叠区逐顶点原样搬过去，
	// 新长出来的部分是平地 + 底色 —— 这里不涉及重采样，扩大地面不该把已经画好的路清掉。
	// 格距变了才真的没有可靠的重采样语义（计划文档 D1），那时照旧整片重置为平地 + 底色。
	const bool bKeepOverlap = Mirror.IsInitialized() && FMath::IsNearlyEqual(Mirror.CellSize, CellSize);
	const FCSGroundMirror Old = bKeepOverlap ? MoveTemp(Mirror) : FCSGroundMirror();

	Mirror.NumVertsX = WantVertsX;
	Mirror.NumVertsY = WantVertsY;
	Mirror.CellSize = CellSize;
	Mirror.Heights.Init(0.0f, WantVertsX * WantVertsY);
	Mirror.Colors.Init(CSGround_QuantizeColor(BaseColor), WantVertsX * WantVertsY);
	MaxAbsHeight = 0.0f;

	if (bKeepOverlap)
	{
		const int32 CopyX = FMath::Min(Old.NumVertsX, WantVertsX);
		const int32 CopyY = FMath::Min(Old.NumVertsY, WantVertsY);
		for (int32 Y = 0; Y < CopyY; ++Y)
		{
			for (int32 X = 0; X < CopyX; ++X)
			{
				const float Height = Old.Heights[Old.VertexIndex(X, Y)];
				Mirror.Heights[Mirror.VertexIndex(X, Y)] = Height;
				Mirror.Colors[Mirror.VertexIndex(X, Y)] = Old.Colors[Old.VertexIndex(X, Y)];
				MaxAbsHeight = FMath::Max(MaxAbsHeight, FMath::Abs(Height));
			}
		}
	}
	++PaintRevision;   // 顶点色的范围变了（整片重置时连道路都没了），岩壳要重新浮上来
	return true;
}

FVector ACSGroundActor::VertexWorldPosition(int32 X, int32 Y) const
{
	const FVector Origin = GetActorLocation();
	return FVector(
		Origin.X + X * Mirror.CellSize,
		Origin.Y + Y * Mirror.CellSize,
		Origin.Z + Mirror.Heights[Mirror.VertexIndex(X, Y)]);
}

void ACSGroundActor::BuildSnapshotFromMirror(FCSGpuMeshCPUData& OutSnapshot) const
{
	OutSnapshot.Reset();
	if (!Mirror.IsInitialized()) return;

	const int32 VertsX = Mirror.NumVertsX;
	const int32 VertsY = Mirror.NumVertsY;
	const int32 VertexCount = VertsX * VertsY;
	const float Cell = Mirror.CellSize;
	const float SafeUVPeriod = FMath::Max(UVWorldPeriod, 1.0f);

	OutSnapshot.Positions.SetNumUninitialized(VertexCount);
	OutSnapshot.Normals.SetNumUninitialized(VertexCount);
	OutSnapshot.Tangents.SetNumUninitialized(VertexCount);
	OutSnapshot.TexCoords().SetNumUninitialized(VertexCount);
	OutSnapshot.Colors.SetNumUninitialized(VertexCount);

	for (int32 Y = 0; Y < VertsY; ++Y)
	{
		for (int32 X = 0; X < VertsX; ++X)
		{
			const int32 Vertex = Mirror.VertexIndex(X, Y);
			OutSnapshot.Positions[Vertex] = FVector3f(VertexWorldPosition(X, Y));

			// 中心差分法线；边界用单侧差分（clamp 邻居）。平地退化为 +Z。
			const int32 XPrev = FMath::Max(X - 1, 0);
			const int32 XNext = FMath::Min(X + 1, VertsX - 1);
			const int32 YPrev = FMath::Max(Y - 1, 0);
			const int32 YNext = FMath::Min(Y + 1, VertsY - 1);
			const float DzDx = (Mirror.Heights[Mirror.VertexIndex(XNext, Y)] - Mirror.Heights[Mirror.VertexIndex(XPrev, Y)]) / (Cell * float(XNext - XPrev));
			const float DzDy = (Mirror.Heights[Mirror.VertexIndex(X, YNext)] - Mirror.Heights[Mirror.VertexIndex(X, YPrev)]) / (Cell * float(YNext - YPrev));
			const FVector3f Normal = FVector3f(-DzDx, -DzDy, 1.0f).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitZ());
			OutSnapshot.Normals[Vertex] = Normal;
			// 沿 +X 的切线；CopyFromMeshSnapshot 会再正交化一遍。
			OutSnapshot.Tangents[Vertex] = FVector3f::CrossProduct(FVector3f::UnitY(), Normal).GetSafeNormal(UE_SMALL_NUMBER, FVector3f::UnitX());

			OutSnapshot.TexCoords()[Vertex] = FVector2f(X * Cell / SafeUVPeriod, Y * Cell / SafeUVPeriod);

			const FLinearColor Color = CSGround_UnquantizeColor(Mirror.Colors[Vertex]);
			OutSnapshot.Colors[Vertex] = FVector4f(Color.R, Color.G, Color.B, Color.A);
		}
	}

	// 每格两个三角，按**引擎绕序**排角点（正面 = cross(C-A, B-A) 朝 +Z）。
	// 注意这与 CSMeshBuild::ResidentFaceNormalScaled 的"从缓冲推法线"口径差一个负号：
	// 渲染路径吃的是引擎绕序（CopyFromStaticMesh 默认 bFlipWinding=0 直传 StaticMesh 索引就能正常
	// 渲染即为实证），按 cross(B-A,C-A)=外法线 去排角点会让整片地面朝里 —— 实测确认过。
	OutSnapshot.Indices.Reserve((VertsX - 1) * (VertsY - 1) * 6);
	for (int32 Y = 0; Y < VertsY - 1; ++Y)
	{
		for (int32 X = 0; X < VertsX - 1; ++X)
		{
			const uint32 V00 = uint32(Mirror.VertexIndex(X, Y));
			const uint32 V10 = uint32(Mirror.VertexIndex(X + 1, Y));
			const uint32 V01 = uint32(Mirror.VertexIndex(X, Y + 1));
			const uint32 V11 = uint32(Mirror.VertexIndex(X + 1, Y + 1));
			OutSnapshot.Indices.Append({ V00, V01, V10, V10, V01, V11 });
		}
	}

	OutSnapshot.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	OutSnapshot.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	if (GroundMaterial) OutSnapshot.Materials.Add(GroundMaterial);
}

void ACSGroundActor::RebuildGroundMesh()
{
	// 模板不建网格也不碰镜像（理由见 EnsureMirrorInitialized）。改蓝图默认值会经
	// PostEditChangeProperty 走到这里。
	if (IsTemplate()) return;
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	FCSGpuMeshCPUData Snapshot;
	BuildSnapshotFromMirror(Snapshot);

	const TArray<TObjectPtr<UMaterialInterface>> Materials = { GroundMaterial };
	if (!UploadTinyGladeSnapshot(Snapshot, Materials)) return;
	// 地面遮罩：常驻流整份换了（色流在里面），遮罩必须重算；格子尺寸 / UV 周期可能也变了，材质的动态实例
	// 跟着重绑。排在下面所有派生链之前 —— 地被 / 灌木读的就是它。
	EnsureGroundMaskTexture();
	BindGroundMaterial();
	GroundMaskBuiltHash = 0;
	RebuildGroundMask();
	MeshBuiltAtLocation = GetActorLocation();
	bLastChangeCommitted = true;   // 全量重建永远算提交
	// 碰撞重烘是异步 cook，要晚几帧才换上；在广播之前发起，好让它尽早开始。
	RefreshGroundCollision();
	const FBox GroundBox = ComputeGroundWorldBox();
	OnGroundChanged.Broadcast(this, GroundBox);
	// 还没找到地面的消费者（地面比它们晚出现）只能从这条类级广播得知"现在有地面了"。
	OnAnyGroundRebuilt.Broadcast(this, GroundBox);
	// 全量重建换掉了整份常驻流（石阶扫描与岩壳披挂都要读它的色流），所以这里必须重扫一遍。
	RebuildStairs();
	// 岩壳的哈希在这里清零：本函数也是"怀疑状态不同步时手动点一次即对齐"的入口，
	// 而披挂的短路是纯哈希的 —— 不清零的话，因缺资产失败过一次就再也不会重试。
	RockShellBuiltHash = 0;
	RebuildRockShell();
	// 裙边摆件同样在这里清哈希：它的短路也是纯哈希的，因缺资产失败过一次就再也不会重试。
	SkirtDecorHash = 0;
	RebuildSkirtDecor();
	// 地被第三次重复同一条：清哈希再散。这里换掉了整份常驻流，而地被的遮罩读的正是它的色流。
	CoverBuiltHash = 0;
	RebuildGroundCover();
	// 灌木同理（同一份色流 + 高度场，外加房屋外皮）。
	BushBuiltHash = 0;
	RebuildBuildingBushes();
}

FBox ACSGroundActor::ComputeGroundWorldBox() const
{
	const FBox2D Rect = GetWorldRect2D();
	const double GroundZ = GetActorLocation().Z;
	return FBox(
		FVector(Rect.Min.X, Rect.Min.Y, GroundZ - MaxAbsHeight - 1.0),
		FVector(Rect.Max.X, Rect.Max.Y, GroundZ + MaxAbsHeight + 1.0));
}

// -----------------------------------------------------------------------------
// 地面遮罩（2026-09-29 用户："地形 mask 决定草；经过处理的地面 mask 作为地面 mesh 的遮罩控制贴图"）
//
// 笔刷画的路权重（顶点色 R）→ GPU 上糊开 + 向外延伸 → 一张贴图。地面材质按它混三层贴图，草 / 花 /
// 灌木按它拒绝、压矮、弯曲。以前两边各读一遍原始顶点色，只要其中一边加了处理，土路边与草线就各走各的。
// -----------------------------------------------------------------------------

bool ACSGroundActor::EnsureGroundMaskTexture()
{
	const int32 Width = Mirror.NumVertsX;
	const int32 Height = Mirror.NumVertsY;
	if (Width < 2 || Height < 2) return false;
	if (GroundMask && GroundMask->SizeX == Width && GroundMask->SizeY == Height && GroundMask->bCanCreateUAV
		&& GroundMask->RenderTargetFormat == RTF_RG32f) return true;

	// 尺寸变了就换一张新的，而不是改旧的尺寸：材质的动态实例靠贴图指针变了才重取资源。旧的那张等动态实例
	// 换绑之后没人引用，交给 GC（它的资源由 BeginDestroy 在渲染线程上放，不在这里手动放 —— 本帧可能还在画它）。
	GroundMask = NewObject<UTextureRenderTarget2D>(this,
		MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("GroundMask")), RF_Transient);
	// R = 成品（延伸 0 时要与顶点色逐位相同，半精度存不下 k/255 ⇒ 32 位），G = 只糊的场（浅土路的阈值读它）。
	GroundMask->RenderTargetFormat = RTF_RG32f;
	GroundMask->ClearColor = FLinearColor::Black;
	GroundMask->bCanCreateUAV = true;
	GroundMask->bAutoGenerateMips = false;
	GroundMask->AddressX = TA_Clamp;
	GroundMask->AddressY = TA_Clamp;
	GroundMask->Filter = TF_Bilinear;
	GroundMask->InitAutoFormat(Width, Height);
	GroundMask->UpdateResourceImmediate(true);
	GroundMaskBuiltHash = 0;
	return true;
}

void ACSGroundActor::BindGroundMaterial()
{
	if (!TinyGladeMesh) return;

	UMaterialInterface* DrawMaterial = GroundMaterial;
	if (GroundMaterial)
	{
		if (!GroundMaterialInstance || GroundMaterialInstance->Parent != GroundMaterial)
		{
			GroundMaterialInstance = UMaterialInstanceDynamic::Create(GroundMaterial, this);
		}
		// 遮罩 UV：顶点 X 的 UV0.x = X·格距/周期（`BuildSnapshotFromMirror`），要落在 texel 中心 (X + 0.5)/宽
		// ⇒ u = UV0.x × 周期 / (格距·宽) + 0.5 / 宽。y 同理。周期的下限与快照里的 SafeUVPeriod 同一个口径。
		const float Period = FMath::Max(UVWorldPeriod, 1.0f);
		const float Cell = FMath::Max(Mirror.CellSize, 1e-3f);
		const float Width = float(FMath::Max(Mirror.NumVertsX, 1));
		const float Height = float(FMath::Max(Mirror.NumVertsY, 1));
		GroundMaterialInstance->SetTextureParameterValue(CSGroundMaskMaterial::TextureParameter, GroundMask);
		GroundMaterialInstance->SetVectorParameterValue(CSGroundMaskMaterial::UVTransformParameter,
			FLinearColor(Period / (Cell * Width), Period / (Cell * Height), 0.5f / Width, 0.5f / Height));
		GroundMaterialInstance->SetScalarParameterValue(CSGroundMaskMaterial::EnabledParameter, GroundMask ? 1.0f : 0.0f);
		// 土跟着草走：深土层用草的拒绝曲线（同 kernel 里 `smoothstep(MaskStart, MaskEnd)` 的两端，End 不许 ≤ Start
		// 的钳位也同 CSGroundCover::Scatter）⇒ 草长满处不露土，土只铺到草开始变稀的地方。
		GroundMaterialInstance->SetScalarParameterValue(CSGroundMaskMaterial::SoilStartParameter, Grass.MaskStart);
		GroundMaterialInstance->SetScalarParameterValue(CSGroundMaskMaterial::SoilEndParameter, FMath::Max(Grass.MaskEnd, Grass.MaskStart + 1e-3f));
		// 采样扰动：材质与地被 / 灌木按同一对参数挪采样点（`CSGroundMaskWarp.ush`），草线与土边一起抖。
		// w = 周期：材质拿 UV0 × 周期还原地面局部坐标（cm），与散布那边的 P − 地面原点是同一个原点。
		GroundMaterialInstance->SetVectorParameterValue(CSGroundMaskMaterial::WarpParameter,
			FLinearColor(FMath::Max(GroundMaskWarpStrength, 0.0f), 1.0f / FMath::Max(GroundMaskWarpWavelength, 1.0f), Period, 0.0f));
		DrawMaterial = GroundMaterialInstance;
	}
	else GroundMaterialInstance = nullptr;
	// 组件画 MID，网格槽放资产本身（烘焙抓的是槽，transient 的 MID 不能进资产）—— 同岩壳。
	BindMeshSlotMaterials(TinyGladeMeshComponent, TinyGladeMesh, { GroundMaterial }, DrawMaterial);
}

void ACSGroundActor::RebuildGroundMask()
{
	if (IsTemplate() || !GetWorld()) return;
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;
	const FCSMeshResidentRef Resident = TinyGladeMesh->GetResident();
	if (!Resident.IsValid()) return;   // 网格还没分配：地面重建完会再走一次这条路
	if (!EnsureGroundMaskTexture()) return;

	// 幂等：路（落笔计数）、格子、延伸距离、贴图本身都没变就不重算。全量重建换了常驻流时调用方先清零。
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };
	uint32 Hash = ::GetTypeHash(PaintRevision);
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsX));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsY));
	Hash = HashFloat(Hash, Mirror.CellSize);
	Hash = HashFloat(Hash, GroundMaskExtendDistance);
	Hash = HashCombine(Hash, ::GetTypeHash(GroundMask.Get()));
	if (Hash == 0) Hash = 1;   // 0 是"还没算过"的哨兵
	if (Hash == GroundMaskBuiltHash) return;

	CSGroundRoadField::FBlurDesc Desc;
	Desc.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Desc.GroundCellSize = Mirror.CellSize;
	Desc.Channel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);   // R = 道路权重
	// 延伸距离 E → 核：PostGain = 2 时遮罩按 erfc(d / σ√2) 衰减，取 σ = E / 2 ⇒ 离路 E 处约剩 5%；
	// `CSGroundRoadField` 的半径口径是 σ = 半径 / 3，所以半径 = 1.5E。E = 0 时不糊、增益 1 ⇒ 遮罩 = 原始路权重。
	const float Extend = FMath::Max(GroundMaskExtendDistance, 0.0f);
	Desc.Radius = 1.5f * Extend;
	Desc.PostGain = Extend > 0.0f ? 2.0f : 1.0f;
	Desc.bMaxWithSource = true;   // 只往外加：窄路、路头糊开后不足 1 也不许变"浅"
	if (!CSGroundRoadField::UpdateMaskTexture(Resident, GroundMask, Desc)) return;
	GroundMaskBuiltHash = Hash;
}

int32 ACSGroundActor::DebugReadGroundMaskSync(TArray<float>& OutValues, TArray<float>* OutInner) const
{
	OutValues.Reset();
	if (OutInner) OutInner->Reset();
	if (!GroundMask) return 0;
	FTextureRenderTargetResource* Resource = GroundMask->GameThread_GetRenderTargetResource();
	if (!Resource) return 0;

	// 诊断回读按设计就是阻塞的，但要进计数器（计数器要么数到每一次阻塞，要么就不能拿它当证据）。
	UCSMesh::CountedBlockingFlush();
	TArray<FLinearColor> Pixels;
	// G32R32F 的读回转换不做 MinMax / Unorm 重映射（RHISurfaceDataConversion.h），读到的就是写进去的值。
	if (!Resource->ReadLinearColorPixels(Pixels)) return 0;
	OutValues.Reserve(Pixels.Num());
	for (const FLinearColor& One : Pixels) OutValues.Add(One.R);
	if (OutInner)
	{
		OutInner->Reserve(Pixels.Num());
		for (const FLinearColor& One : Pixels) OutInner->Add(One.G);
	}
	return OutValues.Num();
}

void ACSGroundActor::ResetPaint()
{
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	for (FColor& Stored : Mirror.Colors) Stored = CSGround_QuantizeColor(BaseColor);
	++PaintRevision;
	RebuildGroundMesh();
	MarkPackageDirty();
}

void ACSGroundActor::BeginPaintStroke()
{
	bPaintStrokeOpen = true;
	StrokeDirtyBounds = FBox(ForceInit);
}

void ACSGroundActor::EndPaintStroke()
{
	if (!bPaintStrokeOpen) return;
	bPaintStrokeOpen = false;

	// 收笔：把队列彻底清空。EdMode 退出后没人再每帧推，剩下的笔不推就永远停在上一帧的画面。
	FlushPaintToGpu(/*bBlockIfNeeded*/ true);

	// 逐笔的广播都是未提交的（房子只落座、不写接缝）；收笔这里补一次**提交**广播，接缝在这一下跨房写入。
	if (StrokeDirtyBounds.IsValid)
	{
		MarkPackageDirty();
		bLastChangeCommitted = true;
		OnGroundChanged.Broadcast(this, StrokeDirtyBounds);
	}
}

void ACSGroundActor::FlushPaintToGpu(bool bBlockIfNeeded)
{
	if (PendingPaintDabs.IsEmpty() || !TinyGladeMesh) return;

	// 在途：这一帧不推，队列原样留到下一帧（顺序不变，混合结果不变）。收笔时不能这样拖着。
	if (TinyGladeMesh->IsEditInFlight())
	{
		if (!bBlockIfNeeded) return;
		// 同步路径 FIFO 排在异步之后，会一直阻塞到两者都跑完 —— 一次 stroke 至多付一次。
		for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : PendingPaintDabs)
		{
			UCSMeshOps::PaintVertexColorsSphereInRegion(TinyGladeMesh, Dab.Center, Dab.Radius, Dab.Falloff, Dab.Strength,
				Dab.Color, Dab.ChannelMask, Dab.BlendOp, Dab.VertsX, Dab.VertsY, Dab.RegionMin, Dab.RegionMax);
		}
		PendingPaintDabs.Reset();
		RebuildGroundMask();
		RebuildStairs();
		RebuildRockShell();
		RebuildSkirtDecor();
		RebuildGroundCover();
		RebuildBuildingBushes();
		return;
	}

	// EditFunc 是 owned：整批落笔按值搬进 lambda，不能留下指向本帧栈/成员的引用。
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> Batch = MoveTemp(PendingPaintDabs);
	PendingPaintDabs.Reset();

	// 拷一份留作兜底：EditMeshAsync 拒了就必须自己把这批笔补上，否则镜像有色而 GPU 没有 ——
	// 那正是本 actor 第一纪律（镜像/GPU 双写不许分叉）要防的分叉，且再也不会自愈。
	TArray<UCSMeshOps::FCSMeshPaintSphereDab> Fallback = Batch;
	const bool bAccepted = TinyGladeMesh->EditMeshAsync(
		[Dabs = MoveTemp(Batch)](FCSMeshEditContext& Context)
		{
			for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : Dabs) UCSMeshOps::AddPaintSpherePasses(Context, Dab);
		});
	if (!bAccepted)
	{
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s async paint flush refused; falling back to sync."), *GetName());
		for (const UCSMeshOps::FCSMeshPaintSphereDab& Dab : Fallback)
		{
			UCSMeshOps::PaintVertexColorsSphereInRegion(TinyGladeMesh, Dab.Center, Dab.Radius, Dab.Falloff, Dab.Strength,
				Dab.Color, Dab.ChannelMask, Dab.BlendOp, Dab.VertsX, Dab.VertsY, Dab.RegionMin, Dab.RegionMax);
		}
	}

	// 地面遮罩从 **GPU 色流**现算，理由同下面的石阶；而且它必须排在地被 / 灌木之前（它们读的就是它），
	// 材质也在画它 —— 排错位置的症状是"土路边与草都慢一笔"。
	RebuildGroundMask();
	// 石阶的道路门控读的是**GPU 色流**，而落笔只写镜像 + 排队 —— 所以重扫必须排在这里，
	// 不能排在 ApplyPaintStroke 里。渲染命令 FIFO 保证扫描看到的是刚推上去的那一笔；
	// 排错位置的症状是"石阶比路慢一笔"，只在快速涂抹时才看得见。
	RebuildStairs();
	// 岩壳同理，而且它的症状更隐蔽：壳在路上是**连续下沉**（裁决五），慢一笔看起来只是
	// "沉得不够深"，不像石阶那样明显缺一截。
	RebuildRockShell();
	// 裙边摆件读的是**镜像**（落笔已同步写进去了），不是 GPU 色流，所以它其实不依赖上面
	// 那条 FIFO；跟着排在这里只是为了让"改了地面 ⇒ 三条派生链一起对齐"只有一个位置要维护。
	RebuildSkirtDecor();
	// 地被的遮罩与石阶一样读 **GPU 色流**，所以它对上面那条 FIFO 的依赖是真的：排错位置的
	// 症状是"路上的草慢一笔才消失"，只在快速涂抹时才看得见。
	RebuildGroundCover();
	// 灌木的路门控读同一份色流，排在同一个位置。
	RebuildBuildingBushes();
}

void ACSGroundActor::ApplyPaintStroke(FVector WorldCenter)
{
	if (!TinyGladeMesh || !Mirror.IsInitialized()) return;
	if (BrushRadius <= 0.0f || BrushStrength <= 0.0f) return;
	if (!bPaintStrokeOpen) BeginPaintStroke();   // 脚本/测试直接落笔时自开括号

	// 区域派发：默认半径 300 / 格距 50 只覆盖 13×13 = 169 个格点，而全网格版会按顶点容量
	// 开线程（256² 是 66,049，1024² 是 1,050,625）—— 派发量随地面尺寸线性膨胀而实际工作量恒定。
	FIntPoint RectMin, RectMax;
	if (!ComputeBrushGridRect(WorldCenter, RectMin, RectMax)) return;

	// **落笔不碰 GPU**：只写镜像（权威）+ 把这一笔排进队列，由 FlushPaintToGpu 每帧一次异步推送。
	// 早先这里每次鼠标移动就是一次 EditMeshSync = 一次 FlushRenderingCommands，交互热路径
	// 因此彻底取消了 GT/RT 流水并行（危害不是停顿本身，是帧率被钉在 GT + RT 的串行和上）。
	UCSMeshOps::FCSMeshPaintSphereDab Dab;
	Dab.Center = WorldCenter;
	Dab.Radius = BrushRadius;
	Dab.Falloff = BrushFalloff;
	Dab.Strength = BrushStrength;
	Dab.Color = PaintColor;
	Dab.ChannelMask = PaintChannelMask;
	Dab.BlendOp = PaintBlendOp;
	Dab.VertsX = Mirror.NumVertsX;
	Dab.VertsY = Mirror.NumVertsY;
	Dab.RegionMin = RectMin;
	Dab.RegionMax = RectMax;
	PendingPaintDabs.Add(Dab);

	ApplyPaintToMirror(WorldCenter);

	const FVector Extent(BrushRadius);
	const FBox TickBounds(WorldCenter - Extent, WorldCenter + Extent);
	StrokeDirtyBounds += TickBounds;

	// 逐笔直推：画的过程中消费者（房屋）就实时重判，不等 stroke 结束。**未提交**：收笔时 EndPaintStroke 再补一次提交广播。
	bLastChangeCommitted = false;
	OnGroundChanged.Broadcast(this, TickBounds);
}

bool ACSGroundActor::ComputeBrushGridRect(const FVector& WorldCenter, FIntPoint& OutMin, FIntPoint& OutMax) const
{
	if (!Mirror.IsInitialized() || Mirror.CellSize <= 0.0f) return false;

	const FVector Origin = GetActorLocation();
	const float Cell = Mirror.CellSize;
	// 未 clamp 的矩形先判出界：clamp 后的空矩形与"贴边一格"长得一样，先判再夹。
	const int32 RawMinX = FMath::FloorToInt32((WorldCenter.X - Origin.X - BrushRadius) / Cell);
	const int32 RawMaxX = FMath::CeilToInt32((WorldCenter.X - Origin.X + BrushRadius) / Cell);
	const int32 RawMinY = FMath::FloorToInt32((WorldCenter.Y - Origin.Y - BrushRadius) / Cell);
	const int32 RawMaxY = FMath::CeilToInt32((WorldCenter.Y - Origin.Y + BrushRadius) / Cell);
	if (RawMaxX < 0 || RawMinX > Mirror.NumVertsX - 1 || RawMaxY < 0 || RawMinY > Mirror.NumVertsY - 1) return false;

	OutMin = FIntPoint(FMath::Clamp(RawMinX, 0, Mirror.NumVertsX - 1), FMath::Clamp(RawMinY, 0, Mirror.NumVertsY - 1));
	OutMax = FIntPoint(FMath::Clamp(RawMaxX, 0, Mirror.NumVertsX - 1), FMath::Clamp(RawMaxY, 0, Mirror.NumVertsY - 1));
	return true;
}

void ACSGroundActor::ApplyPaintToMirror(const FVector& WorldCenter)
{
	FIntPoint RectMin, RectMax;
	if (!ComputeBrushGridRect(WorldCenter, RectMin, RectMax)) return;
	const int32 MinX = RectMin.X, MaxX = RectMax.X, MinY = RectMin.Y, MaxY = RectMax.Y;

	const float Strength = FMath::Clamp(BrushStrength, 0.0f, 1.0f);
	const FLinearColor Mask(
		FMath::Clamp(PaintChannelMask.R, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.G, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.B, 0.0f, 1.0f),
		FMath::Clamp(PaintChannelMask.A, 0.0f, 1.0f));

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			// 与 kernel 相同的三维距离（含高度），别退化成 2D —— 那会在坡地上分叉。
			const float Distance = float(FVector::Dist(VertexWorldPosition(X, Y), WorldCenter));
			if (Distance >= BrushRadius) continue;

			const float Weight = CSGround_BrushWeight(Distance, BrushRadius, FMath::Clamp(BrushFalloff, 0.0f, 1.0f));
			const FLinearColor E = Mask * (Weight * Strength);

			FColor& Stored = Mirror.Colors[Mirror.VertexIndex(X, Y)];
			const FLinearColor Blended = CSGround_BlendColor(CSGround_UnquantizeColor(Stored), PaintColor, E, PaintBlendOp);
			Stored = CSGround_QuantizeColor(Blended);
		}
	}

	// 道路权重变了 ⇒ 岩壳的下沉量变了。逐笔哈希 257² 个字节太贵，一个单调计数器
	// 给出同样的"变了没有"判定（见 PaintRevision 的注释）。
	++PaintRevision;
}

bool ACSGroundActor::WorldToGrid(const FVector2D& WorldXY, FVector2D& OutGrid) const
{
	const FVector Origin = GetActorLocation();
	const FVector2D Grid((WorldXY.X - Origin.X) / Mirror.CellSize, (WorldXY.Y - Origin.Y) / Mirror.CellSize);
	OutGrid.X = FMath::Clamp(Grid.X, 0.0, double(Mirror.NumVertsX - 1));
	OutGrid.Y = FMath::Clamp(Grid.Y, 0.0, double(Mirror.NumVertsY - 1));
	return Grid.X >= 0.0 && Grid.Y >= 0.0 && Grid.X <= double(Mirror.NumVertsX - 1) && Grid.Y <= double(Mirror.NumVertsY - 1);
}

bool ACSGroundActor::SampleBilinear(const FVector2D& WorldXY, float& OutHeight, FLinearColor& OutColor) const
{
	if (!Mirror.IsInitialized()) return false;

	FVector2D Grid;
	if (!WorldToGrid(WorldXY, Grid)) return false;

	const int32 X0 = FMath::Clamp(FMath::FloorToInt32(Grid.X), 0, Mirror.NumVertsX - 2);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32(Grid.Y), 0, Mirror.NumVertsY - 2);
	const float FracX = FMath::Clamp(float(Grid.X - X0), 0.0f, 1.0f);
	const float FracY = FMath::Clamp(float(Grid.Y - Y0), 0.0f, 1.0f);

	auto Lerp2 = [FracX, FracY](float V00, float V10, float V01, float V11)
	{
		return FMath::Lerp(FMath::Lerp(V00, V10, FracX), FMath::Lerp(V01, V11, FracX), FracY);
	};

	const int32 I00 = Mirror.VertexIndex(X0, Y0);
	const int32 I10 = Mirror.VertexIndex(X0 + 1, Y0);
	const int32 I01 = Mirror.VertexIndex(X0, Y0 + 1);
	const int32 I11 = Mirror.VertexIndex(X0 + 1, Y0 + 1);

	OutHeight = Lerp2(Mirror.Heights[I00], Mirror.Heights[I10], Mirror.Heights[I01], Mirror.Heights[I11]);

	const FLinearColor C00 = CSGround_UnquantizeColor(Mirror.Colors[I00]);
	const FLinearColor C10 = CSGround_UnquantizeColor(Mirror.Colors[I10]);
	const FLinearColor C01 = CSGround_UnquantizeColor(Mirror.Colors[I01]);
	const FLinearColor C11 = CSGround_UnquantizeColor(Mirror.Colors[I11]);
	OutColor = FLinearColor(
		Lerp2(C00.R, C10.R, C01.R, C11.R),
		Lerp2(C00.G, C10.G, C01.G, C11.G),
		Lerp2(C00.B, C10.B, C01.B, C11.B),
		Lerp2(C00.A, C10.A, C01.A, C11.A));
	return true;
}

float ACSGroundActor::SampleHeight(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color;
	if (!SampleBilinear(WorldXY, Height, Color)) return GetActorLocation().Z;
	return GetActorLocation().Z + Height;
}

bool ACSGroundActor::TrySampleHeight(FVector2D WorldXY, float& OutWorldZ) const
{
	float Height = 0.0f;
	FLinearColor Color;
	if (!SampleBilinear(WorldXY, Height, Color)) return false;
	OutWorldZ = GetActorLocation().Z + Height;
	return true;
}

float ACSGroundActor::SampleRoadWeight(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color;
	if (!SampleBilinear(WorldXY, Height, Color)) return 0.0f;
	return Color.R;
}

FLinearColor ACSGroundActor::SampleColor(FVector2D WorldXY) const
{
	float Height = 0.0f;
	FLinearColor Color = FLinearColor::Black;
	SampleBilinear(WorldXY, Height, Color);
	return Color;
}

FBox2D ACSGroundActor::GetWorldRect2D() const
{
	const FVector Origin = GetActorLocation();
	const FVector2D Min(Origin.X, Origin.Y);
	if (!Mirror.IsInitialized()) return FBox2D(Min, Min);
	return FBox2D(Min, Min + FVector2D((Mirror.NumVertsX - 1) * Mirror.CellSize, (Mirror.NumVertsY - 1) * Mirror.CellSize));
}

bool ACSGroundActor::RaycastGround(const FVector& RayOrigin, const FVector& RayDirection, FVector& OutHit) const
{
	if (!Mirror.IsInitialized()) return false;

	const FVector Direction = RayDirection.GetSafeNormal();
	if (Direction.IsNearlyZero()) return false;

	const FBox2D Rect = GetWorldRect2D();
	const double GroundZ = GetActorLocation().Z;

	// 平地快路径：当前没有任何高度写入路径（MaxAbsHeight 恒 0），这条就是全部。
	if (MaxAbsHeight <= UE_KINDA_SMALL_NUMBER)
	{
		if (FMath::Abs(Direction.Z) < UE_DOUBLE_SMALL_NUMBER) return false;
		const double T = (GroundZ - RayOrigin.Z) / Direction.Z;
		if (T < 0.0) return false;
		const FVector Hit = RayOrigin + Direction * T;
		if (!Rect.IsInside(FVector2D(Hit.X, Hit.Y))) return false;
		OutHit = Hit;
		return true;
	}

	// 起伏路径：先裁到地面 AABB，再按半格步进找"射线穿到地表下"的符号翻转，翻转区间内
	// 线性插值一次收敛（半格步长下误差远小于格距）。
	const FBox GroundBox(
		FVector(Rect.Min.X, Rect.Min.Y, GroundZ - MaxAbsHeight - 1.0),
		FVector(Rect.Max.X, Rect.Max.Y, GroundZ + MaxAbsHeight + 1.0));
	FVector Entry = RayOrigin;
	if (!GroundBox.IsInsideOrOn(RayOrigin))
	{
		const FVector RayEnd = RayOrigin + Direction * double(HALF_WORLD_MAX);
		FVector HitNormal = FVector::UpVector;
		float HitTime = 0.0f;
		if (!FMath::LineExtentBoxIntersection(GroundBox, RayOrigin, RayEnd, FVector::ZeroVector, Entry, HitNormal, HitTime)) return false;
	}

	const double Step = Mirror.CellSize * 0.5;
	const double MaxDistance = GroundBox.GetSize().Size() + Step;
	double PrevT = 0.0;
	double PrevDelta = Entry.Z - double(SampleHeight(FVector2D(Entry.X, Entry.Y)));
	for (double T = Step; T <= MaxDistance; T += Step)
	{
		const FVector P = Entry + Direction * T;
		const double Delta = P.Z - double(SampleHeight(FVector2D(P.X, P.Y)));
		if (Delta <= 0.0 && PrevDelta > 0.0)
		{
			const double Alpha = PrevDelta / FMath::Max(PrevDelta - Delta, UE_DOUBLE_SMALL_NUMBER);
			const FVector Hit = Entry + Direction * FMath::Lerp(PrevT, T, Alpha);
			if (!Rect.IsInside(FVector2D(Hit.X, Hit.Y))) return false;
			OutHit = FVector(Hit.X, Hit.Y, SampleHeight(FVector2D(Hit.X, Hit.Y)));
			return true;
		}
		PrevT = T;
		PrevDelta = Delta;
	}
	return false;
}

void ACSGroundActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;
	ResolveShapers();
	RebuildGroundMesh();
}

void ACSGroundActor::GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const
{
	// 诊断与烘焙用。显存不走这里：组件销毁时自己撤源，生产者那一份由 `ReleaseInstancedBuffers` 放。
	OutFamilies.Add({ StairComponent, TEXT("石阶"), TEXT("Stairs") });
	OutFamilies.Add({ StairPebbleComponent, TEXT("石子"), TEXT("StairPebbles") });
	for (int32 Index = 0; Index < SkirtDecorComponents.Num(); ++Index) OutFamilies.Add({ SkirtDecorComponents[Index], FString::Printf(TEXT("裙边摆件[%d]"), Index), FString::Printf(TEXT("SkirtDecor%d"), Index), CurrentSkirtDecorInstanceCount > 0 });
	// 地被（下标 0 = 草，1.. = 花）：组件只为网格非空的物种建，建了就有基础网格。
	// ⚠️ 一片草能有十几万实例，烘出来的资产是几百万三角：烘焙出口是给"交付静态场景"用的。
	for (int32 Index = 0; Index < CoverComponents.Num(); ++Index) OutFamilies.Add({ CoverComponents[Index], FString::Printf(TEXT("地被[%d]"), Index), FString::Printf(TEXT("Cover%d"), Index) });
	// 建筑周边灌木：0 = 本体，1 = 花。
	for (int32 Index = 0; Index < BushComponents.Num(); ++Index) OutFamilies.Add({ BushComponents[Index], FString::Printf(TEXT("灌木[%d]"), Index), FString::Printf(TEXT("BuildingBush%d"), Index) });
}

void ACSGroundActor::ReleaseInstancedBuffers()
{
	// 只放本 actor 分配的生产者那一份（排在已入队的命令之后，见 CSShaperSteps::ReleaseOnRenderThread）。
	// 组件手上的实例源、组件自己的常驻网格、岩壳网格的显存，各由组件在 OnComponentDestroyed 里放。
	CSGroundStairs::ReleaseOnRenderThread(StairBuffers);
	CSShaperSteps::ReleaseOnRenderThread(SkirtDecorGpuBuffers);
	for (CSGroundCover::FCoverBuffers& One : CoverBuffers) CSGroundCover::ReleaseOnRenderThread(One);
	for (CSGroundCover::FCoverBuffers& One : BushBuffers) CSGroundCover::ReleaseOnRenderThread(One);
	BushHandover.Reset();
	// 缓冲都没了 ⇒ 交接缓存跟着作废，下次 Ensure* 按新分配的那批重交。
	StairHandover.Reset();
	PebbleHandover.Reset();
	SkirtDecorHandover.Reset();
	CoverHandover.Reset();
}

// -----------------------------------------------------------------------------
// Shapers（计划 D9：高度的唯一权威在镜像，塑形物只是它的输入）
// -----------------------------------------------------------------------------

void ACSGroundActor::ResolveShapers()
{
	Shapers.Reset();
	if (!GetWorld()) return;
	for (TActorIterator<ACSGroundShaperActor> It(GetWorld()); It; ++It)
	{
		// 台高 = 塑形物相对**地面**的 Z：它自己还没来得及解析地面时，这里先把指针补上，
		// 否则这一趟会按"相对世界 0"打包，地面不在 0 高度时整座台高都差一截。
		if (!It->Ground) It->Ground = this;
		Shapers.Add(*It);
	}
}

void ACSGroundActor::RegisterShaper(ACSGroundShaperActor* Shaper)
{
	if (!Shaper) return;
	Shapers.RemoveAll([](const TWeakObjectPtr<ACSGroundShaperActor>& Weak) { return !Weak.IsValid(); });
	if (Shapers.Contains(Shaper)) return;   // 已登记：区域更新由塑形物自己发，别在这里再跑一次全域
	Shapers.Add(Shaper);
	RebuildHeightsFromShapers();
	// ⚠️ **不能只靠上面那句**：`RefreshHeightsInRegion` 有一道 `if (!bChanged) return`，而
	// 加载期高度早就随关卡序列化好了 ⇒ 登记一座塑形物**不会**改变任何高度 ⇒ 那条早退成立，
	// 裙边摆件于是永远停在"零座塑形物"的那一版。症状是"加载后土台在、摆件不在，手点一次
	// RebuildGroundMesh 就出来了"。裙边摆件的输入是**塑形物集合本身**，不是高度差。
	RebuildSkirtDecor();
}

void ACSGroundActor::UnregisterShaper(ACSGroundShaperActor* Shaper)
{
	Shapers.RemoveAll([Shaper](const TWeakObjectPtr<ACSGroundShaperActor>& Weak) { return !Weak.IsValid() || Weak.Get() == Shaper; });
	// 同上：注销这一侧连高度重导出都不发（那是塑形物自己 `UnregisterFromGround` 的活），
	// 少了这一句，删掉塑形物之后那一圈摆件会留在原地飘着。
	RebuildSkirtDecor();
}

void ACSGroundActor::BuildShaperGpuParams(TArray<FVector4f>& OutParams) const
{
	// 头 + mask 距离场池打成一份（布局见 CSGroundShaperField.h）。CPU 镜像重导出读的也是这一份，
	// 所以 GPU 位移、石阶、岩壳、地被与镜像连"合成的顺序"都一致。
	TArray<CSGroundShaperField::FShaperHeader> Headers;
	TArray<TConstArrayView<float>> Sdfs;
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;
		Headers.Add(Shaper->GetFieldHeader());
		Sdfs.Add(Shaper->GetMaskSdf());
	}
	CSGroundShaperField::PackField(Headers, Sdfs, OutParams);
}

void ACSGroundActor::RebuildHeightsFromShapers()
{
	RefreshHeightsInRegion(GetWorldRect2D());
}

void ACSGroundActor::RefreshHeightsInRegion(const FBox2D& WorldRectXY, bool bCommitted)
{
	EnsureMirrorInitialized();
	if (!Mirror.IsInitialized()) return;

	const FVector Origin = GetActorLocation();

	// 世界矩形 → 格点闭区间，向外取整（同 ApplyPaintToMirror 的 Floor/Ceil，不能用 WorldToGrid：
	// 它把出界点夹回边界，会把区域悄悄缩掉）。
	const int32 X0 = FMath::Clamp(FMath::FloorToInt32((WorldRectXY.Min.X - Origin.X) / Mirror.CellSize), 0, Mirror.NumVertsX - 1);
	const int32 Y0 = FMath::Clamp(FMath::FloorToInt32((WorldRectXY.Min.Y - Origin.Y) / Mirror.CellSize), 0, Mirror.NumVertsY - 1);
	const int32 X1 = FMath::Clamp(FMath::CeilToInt32((WorldRectXY.Max.X - Origin.X) / Mirror.CellSize), X0, Mirror.NumVertsX - 1);
	const int32 Y1 = FMath::Clamp(FMath::CeilToInt32((WorldRectXY.Max.Y - Origin.Y) / Mirror.CellSize), Y0, Mirror.NumVertsY - 1);

	// 镜像（CPU 权威，供 SampleHeight/拾取/房子落座查询）。公式是绝对式的"基底 0 与全部
	// 塑形物取 max"，所以在区域内重算与全量重算逐位相同 —— 撤掉旧隆起靠的是重算，不是减法。
	// 合成（平滑 max）必须与 GPU 同序同式 ⇒ 直接在 GPU 那份打包缓冲上求值（CSGroundShaperField::EvalField）。
	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	bool bChanged = false;
	float NewMaxAbsHeight = 0.0f;
	for (int32 Y = Y0; Y <= Y1; ++Y)
	{
		for (int32 X = X0; X <= X1; ++X)
		{
			const FVector2D World(Origin.X + X * Mirror.CellSize, Origin.Y + Y * Mirror.CellSize);
			const float Height = CSGroundShaperField::EvalField(FVector2f(float(World.X), float(World.Y)), ShaperParams);
			float& Slot = Mirror.Heights[Mirror.VertexIndex(X, Y)];
			if (FMath::IsNearlyEqual(Slot, Height, 0.01f)) continue;
			Slot = Height;
			bChanged = true;
		}
	}
	// 幂等短路：加载后重导出结果与序列化值一致时不重建镜像、不标脏。
	//
	// ⚠️ **但"镜像没变"不等于"派生链不用动"，这里必须放它们过去**（2026-08-31 实测修复）。
	// 加载时序是：塑形物先向本 actor 注册、**后**落自己的变换 ⇒ 那一瞬间打包出来的
	// `GetFieldHeader().Profile.xy` 还是 (0,0) ⇒ 岩壳/石阶已经按"塑形物在原点"
	// 披挂过一趟。等变换落定、`RebuildHeightsFromShapers()` 再进来时，镜像是**序列化的、
	// 本来就对**的 ⇒ `bChanged == false` ⇒ 从前在这里直接 return，三条派生链就永远停在
	// 那趟陈旧位移上。
	//
	// 实测症状（`L_TerrainOpsDemo`，塑形物在 (6400, 6400)）：加载后岩壳只有 293 个活三角、
	// 质心落在 (577, 671)，**离土台 81.7 m**，而 `SampleHeight` 一切正常、演示回归全绿
	// —— 因为回归在量岩壳之前画过路（`PaintRevision` 变 ⇒ 走另一条重建路），把它顺手治好了。
	// 手动点一次 `RebuildRockShell()` 就对：1112 个活三角、质心 (6387, 6388)。
	//
	// 三条链各自有幂等哈希守卫（`RockShellBuiltHash` / `SkirtDecorHash`；石阶是定容单
	// dispatch、每笔落笔本来就在跑），所以真没变时这三次调用是纯空转 —— 这正是计划 D3
	// 「消费者无条件重求值、靠幂等哈希兜底」那条纪律，早退门当初漏了它。
	if (!bChanged)
	{
		RebuildStairs();
		RebuildRockShell();
		RebuildSkirtDecor();
		RebuildGroundCover();
		RebuildBuildingBushes();
		return;
	}

	// 拾取的平面/march 分路看的是全局上界；高度恒 = max(0, 各塑形物台高)，直接从参数取，
	// 不用扫全表（区域更新的意义就在于不碰区域外的格点）。
	for (int32 Index = 0; Index < CSGroundShaperField::NumShapers(ShaperParams); ++Index)
	{
		// 上界取**峰值**（打包头里的台高已除过二次抬升，乘回去才是台顶）；裙边噪声只会往下啃、
		// 平滑合成被封顶在峰高，所以都不用留余量。
		// 取绝对值：坑（负台高）的深度同样撑开拾取与包围盒的上界。
		NewMaxAbsHeight = FMath::Max(NewMaxAbsHeight, FMath::Abs(CSGroundShaperField::PeakHeight(CSGroundShaperField::GetHeader(ShaperParams, Index).Top)));
	}
	MaxAbsHeight = NewMaxAbsHeight;
	MarkPackageDirty();

	// GPU 投影：一个 compute pass 原地改 Z 与法线。网格还没建、或 actor 被拖过导致常驻流
	// （世界空间）与当前位置脱节时，退回全量重建对齐。
	if (!TinyGladeMesh || !MeshBuiltAtLocation.Equals(Origin))
	{
		RebuildGroundMesh();
		return;
	}

	++GpuDisplaceCount;
	UCSMeshOps::DisplaceGroundShapers(
		TinyGladeMesh, ShaperParams,
		FVector2f(float(Origin.X), float(Origin.Y)), Mirror.CellSize, float(Origin.Z),
		Mirror.NumVertsX, Mirror.NumVertsY, FIntPoint(X0, Y0), FIntPoint(X1, Y1));

	const FBox ChangedBox(
		FVector(Origin.X + X0 * Mirror.CellSize, Origin.Y + Y0 * Mirror.CellSize, Origin.Z - MaxAbsHeight - 1.0),
		FVector(Origin.X + X1 * Mirror.CellSize, Origin.Y + Y1 * Mirror.CellSize, Origin.Z + MaxAbsHeight + 1.0));
	bLastChangeCommitted = bCommitted;   // 塑形物拖动途中传 false，松手 / 改参 / 注销传 true
	// 碰撞只在提交时重烘：cook 虽是异步的，拖动途中逐帧发起也只会一次次取消在途的那份，
	// 而放置射线本来就只在松手之后才有意义。
	if (bCommitted) RefreshGroundCollision();
	OnGroundChanged.Broadcast(this, ChangedBox);

	// 高度场变了 ⇒ 等值线变了。石阶不订阅 OnGroundChanged（它就归本 actor，订阅自己的广播
	// 是个自环），直接排在广播之后重扫一遍。
	RebuildStairs();
	// 岩壳与地面位移**同一趟**（计划 D9 链 B 的接线）：两者读同一份 GroundShaperParams，
	// 排在 GPU 位移之后就天然同帧一致，不需要任何跨 actor 的同步。
	RebuildRockShell();
	// 裙边摆件是第三条同源派生链：高度场一动，那一圈锚点的落高与"被邻座埋了没有"都要重判。
	RebuildSkirtDecor();
	// 地被是第四条：高度场一动，每一株的落高与坡度门控都要重判（土台拉陡了草就该退掉）。
	RebuildGroundCover();
	// 灌木的落高与坡度门控同理。
	RebuildBuildingBushes();
}

// -----------------------------------------------------------------------------
// Stairs（计划「石阶改造：100% GPU 决策 + 零回读」的 S1/S2/S3）
//
// **全项目唯一的一条石阶路**：塑形物自持的旧路（`ACSGroundShaperActor::RebuildSteps` +
// `CSShaperSteps` + `CSGroundSteps.usf`）已随 2026-08-30「裁决一」整条删除。
// 这条路上 CPU 不接触单个台阶：层 / 弧段 / 摆位全部由 GPU 从合成后的高度场推导。
// -----------------------------------------------------------------------------

bool ACSGroundActor::EnsureStairComponent()
{
	// 关掉石阶：连组件带显存一起收掉。留着一个空组件只会在 details 面板里留个误导性的槽位。
	// 石子是石阶的从属支线（它只在摆出一级台阶的那一段等值线上抽签），所以跟着一起收 ——
	// 单独留着它会得到一地没有台阶的石头。组件手上那份实例源与它自己的显存，由组件销毁时自己放。
	if (!StairMesh)
	{
		if (IsValid(StairComponent)) StairComponent->DestroyComponent();
		StairComponent = nullptr;
		if (IsValid(StairPebbleComponent)) StairPebbleComponent->DestroyComponent();
		StairPebbleComponent = nullptr;
		CSGroundStairs::ReleaseOnRenderThread(StairBuffers);
		StairHandover.Reset();
		PebbleHandover.Reset();
		return false;
	}

	CSShaperSteps::EnsureInstancedComponent(this, StairComponent);
	// 空 = 逐段画石阶网格资产自带的材质；设了才整体覆盖。变了才让代理重建。
	StairComponent->SetInstanceMaterial(StairMaterial);
	StairComponent->SetBaseMesh(StairMesh);    // 同一张网格时内部直接早退

	// 从网格推导三个量：包围球（剔除用）、块缩放、抬升。
	const FBox Local = StairMesh->GetBoundingBox();
	StairBaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	StairBaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;

	// 某一轴填 0（或网格该轴退化）就保持网格自带的尺寸 —— 喂"真实尺寸的石阶网格"与喂
	// "居中单位立方体字典 mesh"走的是同一条式子，不需要两套口径。
	const FVector Size = Local.IsValid ? Local.GetSize() : FVector::OneVector;
	auto AxisScale = [](double Want, double Have) -> float
	{
		return (Want > UE_KINDA_SMALL_NUMBER && Have > UE_KINDA_SMALL_NUMBER) ? float(Want / Have) : 1.0f;
	};
	StairBlockScale = FVector3f(
		AxisScale(StairBlockSize.X, Size.X),
		AxisScale(StairBlockSize.Y, Size.Y),
		AxisScale(StairBlockSize.Z, Size.Z));
	// S2 的长度轴跟着弦长走（世界 cm），kernel 需要网格自身的 Y 尺寸才能换算回缩放。
	StairBaseSizeY = (Local.IsValid && Size.Y > UE_KINDA_SMALL_NUMBER) ? float(Size.Y) : 1.0f;

	// 半个身位抬升：盒心直接落在等值线上的话石块一半埋在地里（原型如此，旧路的实测修正）。
	StairRise = Local.IsValid ? float(-Local.Min.Z) * StairBlockScale.Z : 0.0f;

	// ---- 小石子（TG 的 15% 支线）：同样从网格推导，网格为空则缩放归零 = 这一支关掉 ----
	// 归零而不是"跳过绑定"：石子的 UAV 在 RDG 里是无条件绑定的，真正的开关只有 `PebbleChance`
	// 与这里的缩放；留一个非零缩放配空网格，画出来的是一地引擎默认球。
	if (StairPebbleMesh)
	{
		CSShaperSteps::EnsureInstancedComponent(this, StairPebbleComponent);
		StairPebbleComponent->SetInstanceMaterial(StairPebbleMaterial);   // 空 = 石子网格资产自带的材质
		StairPebbleComponent->SetBaseMesh(StairPebbleMesh);

		const FBox PebbleLocal = StairPebbleMesh->GetBoundingBox();
		StairPebbleSphereCentre = PebbleLocal.IsValid ? FVector3f(PebbleLocal.GetCenter()) : FVector3f::ZeroVector;
		StairPebbleSphereRadius = PebbleLocal.IsValid ? float(PebbleLocal.GetExtent().Size()) : 0.0f;
		// 均匀缩放，所以换算基准取网格**最长**的那一轴：石子是团块，用哪一轴都行，
		// 取最长轴才让 `StairPebbleSize` 读作"这颗石头最大有多大"而不是某个看不见的轴。
		const FVector PebbleSize = PebbleLocal.IsValid ? PebbleLocal.GetSize() : FVector::OneVector;
		const double PebbleLongest = FMath::Max3(PebbleSize.X, PebbleSize.Y, PebbleSize.Z);
		const double Lo = FMath::Max(FMath::Min(StairPebbleSize.X, StairPebbleSize.Y), 0.0);
		const double Hi = FMath::Max(StairPebbleSize.X, StairPebbleSize.Y);
		StairPebbleScaleMin = (PebbleLongest > UE_KINDA_SMALL_NUMBER) ? float(Lo / PebbleLongest) : 0.0f;
		StairPebbleScaleMax = (PebbleLongest > UE_KINDA_SMALL_NUMBER) ? float(Hi / PebbleLongest) : 0.0f;
	}
	else
	{
		if (IsValid(StairPebbleComponent)) StairPebbleComponent->DestroyComponent();
		StairPebbleComponent = nullptr;
		PebbleHandover.Reset();
		StairPebbleSphereCentre = FVector3f::ZeroVector;
		StairPebbleSphereRadius = 0.0f;
		StairPebbleScaleMin = 0.0f;
		StairPebbleScaleMax = 0.0f;
	}
	return true;
}

void ACSGroundActor::RebuildStairs()
{
	if (IsTemplate() || !GetWorld()) return;
	if (!EnsureStairComponent()) return;
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;

	const FCSMeshResidentRef Resident = TinyGladeMesh->GetResident();
	if (!Resident.IsValid()) return;   // 网格还没分配：地面重建完会再走一次这条路

	// 固定容量：只在配置真的变了时分配（一次阻塞）。交互期（画笔刷、拖塑形物）永远走
	// "已经够大"的零成本分支 —— 这正是把已删旧路那种"每次重排记录都重算需要多少格、
	// 于是逐笔扩容"的做法换掉之后拿到的东西。
	// 石子的容量**从石阶容量推导，不另开一个属性**：它是 15% 的伯努利抽样，N = 4096 时均值 614、
	// σ ≈ 23，取一半（2048）是 60 σ 以上的余量，而且谁调大 MaxStairInstances 石子就跟着涨 ——
	// 两个各自可调的容量迟早会配错，症状是"石阶好好的，石子在某个尺寸以上开始缺"，没人查得到。
	const uint32 StairCapacity = uint32(FMath::Max(MaxStairInstances, 64));
	if (!CSGroundStairs::EnsureBuffers(StairBuffers, StairCapacity, FMath::Max(StairCapacity / 2u, 64u))) return;

	const FVector Origin = GetActorLocation();
	const FBox2D Rect = GetWorldRect2D();
	const FVector2D Span = Rect.GetSize();
	const float Cell = FMath::Max(StairCellSize, 10.0f);

	CSGroundStairs::FScanParams Params;
	// 组件挂在根上、相对变换为单位阵 ⇒ 组件空间就是 actor 空间。
	Params.WorldToComponent = FMatrix44f(GetActorTransform().ToInverseMatrixWithScale());
	Params.GridOriginXY = FVector2f(float(Rect.Min.X), float(Rect.Min.Y));
	Params.CellSize = Cell;
	Params.GridDims = FIntPoint(
		FMath::Max(FMath::CeilToInt32(Span.X / Cell), 1),
		FMath::Max(FMath::CeilToInt32(Span.Y / Cell), 1));
	Params.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
	Params.GroundCellSize = Mirror.CellSize;
	Params.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Params.GroundBaseZ = float(Origin.Z);
	Params.StepHeight = StairStepHeight;
	Params.RoadThreshold = StairRoadThreshold;
	Params.MinMoundHeight = FMath::Max(StairMinMoundHeight, 0.0f);
	Params.bDropTopStep = bStairDropTopStep;
	Params.Embed = StairEmbed;
	Params.Rise = StairRise;
	Params.ZOffset = StairZOffset;
	Params.MaxLayersPerCell = uint32(FMath::Clamp(StairMaxLayersPerCell, 1, 256));
	Params.BaseSphereCentre = StairBaseSphereCentre;
	Params.BaseSphereRadius = StairBaseSphereRadius;
	Params.BlockSize = StairBlockScale;
	Params.BaseSizeY = StairBaseSizeY;
	Params.LengthBloat = FMath::Max(StairLengthBloat, 1.0f);
	Params.LengthJitter = FMath::Max(StairLengthJitter, 0.0f);
	Params.SizeJitter = FMath::Clamp(StairSizeJitter, 0.0f, 0.5f);
	Params.YawJitterRad = FMath::DegreesToRadians(FMath::Max(StairYawJitter, 0.0f));
	Params.JitterSeed = uint32(StairJitterSeed);
	// 网格缺席时概率直接归零：kernel 于是一个字节都不写，counter 被 clear 归零，
	// 上一趟的石子当场消失 —— 不需要任何"注销"路径（同岩壳裁决二的形状）。
	Params.PebbleChance = StairPebbleMesh ? FMath::Clamp(StairPebbleChance, 0.0f, 1.0f) : 0.0f;
	Params.PebbleScaleMin = StairPebbleScaleMin;
	Params.PebbleScaleMax = StairPebbleScaleMax;
	Params.PebbleSphereCentre = StairPebbleSphereCentre;
	Params.PebbleSphereRadius = StairPebbleSphereRadius;

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	if (!CSGroundStairs::Scan(Resident, StairBuffers, Params, ShaperParams)) return;

	// **包围盒不能靠实例算**（只有 GPU 知道摆了哪些），所以按"地面矩形 × MaxAbsHeight"写死 ——
	// 与岩壳用 NaN 顶点污染包围盒是同一条处置。组件空间的保守盒，且要自己把基础网格的尺寸
	// 算进去（packed 路径不代劳）。
	// 保守包围盒的最坏缩放。**只能由配置算出来，一个实例都不能读** —— 一旦掺进实例数据，
	// 包围盒就会随落笔漂移，`bNeedHandover` 每 dab 都成立，阻塞的 SetInstanceSourceGPU
	// 会把"交互期零阻塞"整条纪律退化掉（这正是拉尺寸那一轮踩过的坑）。
	// 长度轴的最坏值 = 最长可能弦长（格对角线 √2 × 格距）× 胀大 × (1 + 单侧抖动)。
	const float WorstLenWorld = FMath::Max(Cell * UE_SQRT_2, StairBlockScale.Y * StairBaseSizeY)
		* FMath::Max(StairLengthBloat, 1.0f) * (1.0f + FMath::Max(StairLengthJitter, 0.0f));
	const float WorstScaleY = (StairBaseSizeY > UE_KINDA_SMALL_NUMBER) ? (WorstLenWorld / StairBaseSizeY) : StairBlockScale.Y;
	const float WorstScaleXZ = FMath::Max(StairBlockScale.X, StairBlockScale.Z)
		* (1.0f + FMath::Clamp(StairSizeJitter, 0.0f, 0.5f));
	const double Reach = double(StairBaseSphereRadius) * double(FMath::Max(WorstScaleY, WorstScaleXZ));
	// MaxAbsHeight 随塑形**连续**变（拖 LiftHeight 时每帧都在变），直接进盒子就是每帧一次阻塞交接 ——
	// 过一次 QuantizeUp 吸成台阶（同裙边摆件那行），拖动中绝大多数帧盒子逐位不变（2026-09-07 审查 B5）。
	const double HeightReach = CSShaperSteps::QuantizeUp(double(MaxAbsHeight));
	const FVector LocalMin = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - HeightReach)) - FVector(Reach + FMath::Abs(StairEmbed));
	const FVector LocalMax = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + HeightReach)) + FVector(Reach + FMath::Abs(StairEmbed));
	const FBox LocalBounds(LocalMin.ComponentMin(LocalMax), LocalMin.ComponentMax(LocalMax));

	// 交接是阻塞的（内部 SetStreamLayoutSync/ResizeStreamsSync + 立刻重建 render state）。
	// 容量固定、包围盒只跟地面尺寸与量化后的 MaxAbsHeight 走，所以画路那种高频路径上它永远不触发。
	// 两条都**不并旧盒**（不走 `MergeHandoverBounds`）：盒子完全由配置算出，本来就不随落笔
	// 漂移，并旧盒只会让它单调变大。
	CSShaperSteps::FHandoverSource StairSource;
	StairSource.Component = StairComponent;
	StairSource.PackedInstances = StairBuffers.PackedInstances;   // 保留自己的引用，重扫还要用
	StairSource.Counter = StairBuffers.Counter;
	StairSource.Capacity = StairBuffers.Capacity;
	CSShaperSteps::FHandoverOptions StairOptions;
	StairOptions.LogLabel = TEXT("石阶");
	CSShaperSteps::HandOverInstanceSource(StairSource, LocalBounds, StairHandover, StairOptions, this);

	// 石子的判据必须与石阶逐字相同（容量固定 + 包围盒只由配置算）—— 它的包围盒能直接沿用
	// 石阶那个保守盒：石子落在同一条等值线上、缩放比石阶小一个量级，伸不出石阶已经算进去的
	// 那圈余量。共用一个盒子还顺带保证两条交接同时稳态、同时触发。
	CSShaperSteps::FHandoverSource PebbleSource;
	PebbleSource.Component = StairPebbleComponent;
	PebbleSource.PackedInstances = StairBuffers.PebbleInstances;
	PebbleSource.Counter = StairBuffers.PebbleCounter;
	PebbleSource.Capacity = StairBuffers.PebbleCapacity;
	CSShaperSteps::FHandoverOptions PebbleOptions;
	PebbleOptions.LogLabel = TEXT("石子");
	CSShaperSteps::HandOverInstanceSource(PebbleSource, LocalBounds, PebbleHandover, PebbleOptions, this);
}

int32 ACSGroundActor::DebugReadStairRowsSync(TArray<FVector4f>& OutRows)
{
	OutRows.Reset();
	if (!StairBuffers.IsValid()) return 0;
	return CSGroundStairs::DebugReadInstancesSync(StairBuffers, nullptr, &OutRows);
}

int32 ACSGroundActor::DebugReadStairsSync(TArray<FVector>& OutWorldOrigins)
{
	OutWorldOrigins.Reset();
	if (!StairBuffers.IsValid()) return 0;

	TArray<FVector> LocalOrigins;
	const int32 Count = CSGroundStairs::DebugReadInstancesSync(StairBuffers, &LocalOrigins);
	const FTransform ToWorld = GetActorTransform();
	OutWorldOrigins.Reserve(LocalOrigins.Num());
	for (const FVector& Local : LocalOrigins) OutWorldOrigins.Add(ToWorld.TransformPosition(Local));
	return Count;
}

int32 ACSGroundActor::DebugReadStairPebbleRowsSync(TArray<FVector4f>& OutRows)
{
	OutRows.Reset();
	if (!StairBuffers.HasPebbles()) return 0;
	return CSGroundStairs::DebugReadInstancesSync(StairBuffers, nullptr, &OutRows, /*bPebbles=*/true);
}

int32 ACSGroundActor::DebugReadStairPebblesSync(TArray<FVector>& OutWorldOrigins)
{
	OutWorldOrigins.Reset();
	if (!StairBuffers.HasPebbles()) return 0;

	TArray<FVector> LocalOrigins;
	const int32 Count = CSGroundStairs::DebugReadInstancesSync(StairBuffers, &LocalOrigins, nullptr, /*bPebbles=*/true);
	const FTransform ToWorld = GetActorTransform();
	OutWorldOrigins.Reserve(LocalOrigins.Num());
	for (const FVector& Local : LocalOrigins) OutWorldOrigins.Add(ToWorld.TransformPosition(Local));
	return Count;
}

// -----------------------------------------------------------------------------
// GPU 侧真值（诊断 / 验收专用，阻塞）—— 声明处那段注释是这几条为什么存在
// -----------------------------------------------------------------------------

int32 ACSGroundActor::DebugReadStairCountGpuSync() const
{
	// 组件不在（石阶关着 / 蓝图重跑了构造脚本）时读到的就是 0：那一路确实一个实例都不画。
	return IsValid(StairComponent) ? StairComponent->DebugReadDrawnInstanceCountSync() : 0;
}

int32 ACSGroundActor::DebugReadStairPebbleCountGpuSync() const
{
	return IsValid(StairPebbleComponent) ? StairPebbleComponent->DebugReadDrawnInstanceCountSync() : 0;
}

int32 ACSGroundActor::DebugReadRockShellDrawIndexCountGpuSync() const
{
	if (!RockShellMesh) return 0;
	const FCSMeshResidentRef Resident = RockShellMesh->GetResident();
	if (!Resident.IsValid() || !Resident->IsAllocated()) return 0;

	const FCSMeshResident::FStream* Args = Resident->FindStream(ECSGpuStreamRole::IndirectArgs);
	if (!Args || !Args->Pooled.IsValid()) return -1;

	// DrawIndexedIndirect 的第 0 组参数，[0] = IndexCountPerInstance —— 绘制真正消费的就是它。
	// 读完必须放回 ERHIAccess::IndirectArgs（`FinalAccessForRole` 那张表），否则下一次绘制会
	// 在 CopySrc 状态上撞见这个 buffer。
	TArray<uint32> Values;
	if (!CSMeshReadback::ReadUintBufferSync(Args->Pooled, 1u, ECSGpuStreamRole::IndirectArgs, Values) || Values.IsEmpty())
	{
		return -1;
	}
	return int32(FMath::Min<uint32>(Values[0], uint32(MAX_int32)));
}

// -----------------------------------------------------------------------------
// Rock Shell（计划 D9「侧面碎石：Tiny Glade 式披挂岩壳」的链 B）
//
// 裁决二：碎石归**地面**，不归塑形物 —— 图案覆盖整张地面、与任何单座塑形物无关，
// mask 由全部塑形物合成后的坡度决定。删掉塑形物 → 高度场塌回 → 坡度降到阈下 →
// 那批胞腔自己写 NaN，**归属簿记整个消失**，没有一行注销代码。
// -----------------------------------------------------------------------------

CSRockShell::FPatternParams ACSGroundActor::MakeRockShellPatternParams() const
{
	CSRockShell::FPatternParams Pattern;
	Pattern.CellSize = RockShellCellSize;
	Pattern.SeedJitter = RockShellSeedJitter;
	Pattern.ClusterWarp = RockShellClusterWarp;
	Pattern.ClusterWavelength = RockShellClusterWavelength;
	Pattern.SizeVariance = RockShellSizeVariance;
	Pattern.Seed = uint32(RockShellSeed);
	Pattern.BoundaryPoints = RockShellBoundaryPoints;
	return Pattern.Sanitized();
}

void ACSGroundActor::PostLoad()
{
	Super::PostLoad();

	// 旧属性迁移（2026-09-30 程序化图案取代 TG 原件那张 tile）：存盘里有旧缩放（≠ −1）⇒ 换算成胞腔格距，
	// 密度逐位不变（0.35 ⇒ 193.6 cm）。锚点那两项（bRockShellCustomPatternCentre / RockShellPatternCentre）
	// 在程序化图案下没有意义，直接删了：图案锚在地面原点，任何 XY 都有石头可长。
	if (RockShellPatternScale > 0.0f)
	{
		RockShellCellSize = FMath::Clamp(RockShellPatternScale * CSRockShell::TGNativeCellSpacingCm, 20.0f, 5000.0f);
		UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s 迁移旧岩壳缩放 %.3f ⇒ RockShellCellSize %.1f cm。"),
			*GetName(), RockShellPatternScale, RockShellCellSize);
		RockShellPatternScale = -1.0f;
	}
}

uint32 ACSGroundActor::RockShellInputHash() const
{
	// float 走位模式哈希：GetTypeHash(float) 对 −0.0 / NaN 的处理不是我们要的语义，而这里
	// 只需要"变了没有"。
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };

	uint32 Hash = ::GetTypeHash(bRockShell);
	// 法线模式改的是切线流不是位置，但披挂是唯一会写切线的地方 ⇒ 不入哈希就"改了没反应"。
	Hash = HashCombine(Hash, ::GetTypeHash(bRockShellSmoothNormals));
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellSmoothAngleDeg));
	// 落笔计数：壳在路上是连续下沉（裁决五），下沉量由道路权重驱动 ⇒ 画一笔就得重披挂。
	Hash = HashCombine(Hash, ::GetTypeHash(PaintRevision));
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellMaterial.Get()));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsX));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsY));
	Hash = HashFloat(Hash, Mirror.CellSize);
	Hash = HashFloat(Hash, UVWorldPeriod);
	const FVector Origin = GetActorLocation();
	Hash = HashFloat(Hash, float(Origin.X));
	Hash = HashFloat(Hash, float(Origin.Y));
	Hash = HashFloat(Hash, float(Origin.Z));
	// 程序化图案：几何哈希覆盖格距 / 抖动 / 扭曲 / 功率权重 / 种子 / M，池上限另算（它改的是截断）。
	Hash = HashCombine(Hash, MakeRockShellPatternParams().GeometryHash());
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellMaxCells));
	Hash = HashFloat(Hash, RockShellReliefFloor);
	// 隆起参数都改顶点位置 ⇒ 全部入哈希。漏一个的症状是"改了没反应"（静默无效，没有任何断言看得见）。
	Hash = HashFloat(Hash, RockShellCellExpand);
	Hash = HashFloat(Hash, RockShellSkirtTilt);
	Hash = HashFloat(Hash, RockShellRiseMultiplier);
	Hash = HashFloat(Hash, RockShellRiseNoiseAmount);
	Hash = HashFloat(Hash, RockShellRiseNoiseWavelength);
	Hash = HashFloat(Hash, RockShellRiseExtend);
	Hash = HashFloat(Hash, RockShellEdgeCeiling);
	Hash = HashFloat(Hash, RockShellBaseLift);
	Hash = HashFloat(Hash, RockShellBaseSink);
	Hash = HashFloat(Hash, RockShellSlopeLo);
	Hash = HashFloat(Hash, RockShellSlopeHi);
	Hash = HashFloat(Hash, RockShellRoadFade);
	Hash = HashFloat(Hash, RockShellRoadSink);
	Hash = HashFloat(Hash, RockShellRoadBlurRadius);
	Hash = HashFloat(Hash, RockShellCellJitter);
	Hash = HashFloat(Hash, RockShellCellRelief);
	Hash = HashFloat(Hash, RockShellNoiseAmount);
	Hash = HashFloat(Hash, RockShellNoiseWavelength);
	Hash = HashFloat(Hash, RockShellChipAmount);
	Hash = HashFloat(Hash, RockShellChipWavelength);
	Hash = HashCombine(Hash, ::GetTypeHash(RockShellSeed));

	// 塑形物：哈希的是**打包后的高度场参数**而不是 actor 指针 —— 拖动一座塑形物时指针不变、
	// 参数变，而壳恰恰要跟着参数走（激活也一样：它读的就是这份参数）。
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;
		const CSGroundShaperField::FShaperHeader Header = Shaper->GetFieldHeader();
		// 换一张同尺寸的 mask 时头逐位不变，距离场的指纹才看得出来。
		Hash = HashCombine(Hash, ::GetTypeHash(Shaper->GetMaskSdfKey()));
		for (const FVector4f& Packed : { Header.Profile, Header.Top, Header.Noise, Header.Frame, Header.Shape })
		{
			Hash = HashFloat(Hash, Packed.X);
			Hash = HashFloat(Hash, Packed.Y);
			Hash = HashFloat(Hash, Packed.Z);
			Hash = HashFloat(Hash, Packed.W);
		}
	}
	return Hash;
}

namespace
{
/** 空槽的哨兵格键（格键本身可以是任何 int32，但 (MAX, MAX) 在任何地面尺寸下都到不了）。 */
const FIntPoint CSGround_RockShellFreeSlot(MAX_int32, MAX_int32);
}

bool ACSGroundActor::EnsureRockShellMesh(int32 NeededCells, bool& bOutRebuilt)
{
	bOutRebuilt = false;
	auto ResetPool = [this]()
	{
		RockShellPoolGeometryHash = 0;
		RockShellPoolRect = FBox2D(ForceInit);
		RockShellPoolCapacity = 0;
		RockShellPoolTrianglesPerCell = 0;
		RockShellSlotOfCell.Reset();
		RockShellCellOfSlot.Reset();
		RockShellFreeSlots.Reset();
		bRockShellGrowPending = false;
	};

	// 关掉岩壳：连组件带显存一起收掉。留着一个空组件只会在 details 面板里留个误导性的槽位。
	if (!bRockShell)
	{
		// 岩壳网格归岩壳组件所有：组件销毁时自己把显存还掉（不阻塞），这里只撂下引用。
		// ⚠️ 别先 SetGpuMesh(nullptr)：解绑之后组件就不知道它拥有过哪张网格了。
		if (IsValid(RockShellComponent)) RockShellComponent->DestroyComponent();
		RockShellComponent = nullptr;
		RockShellMaterialInstance = nullptr;
		RockShellMesh = nullptr;
		ResetPool();
		return false;
	}
	if (!Mirror.IsInitialized()) return false;

	// 蓝图 actor 重跑构造脚本会把组件销毁，指针会失效 —— 先判有效再复用。
	if (!IsValid(RockShellComponent))
	{
		RockShellComponent = NewObject<UCSMeshRenderComponent>(this, TEXT("RockShellMesh"), RF_Transient);
		RockShellComponent->SetupAttachment(RootComponent);
		RockShellComponent->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格
	}
	const CSRockShell::FPatternParams Pattern = MakeRockShellPatternParams();
	const float Scale = Pattern.PatternScale();

	// 绘制材质 = RockShellMaterial 的动态子实例，只多带 RockShellPatternScale（假倒角把 TG 原生的
	// 图案口径换算到世界用，见 Docs/TinyGlade/RockShell_EdgeBevel.md）。父材质换了才重建实例，
	// 缩放变了只改标量（值没变时 SetScalarParameterValue 自己早退）。
	UMaterialInterface* DrawMaterial = RockShellMaterial;
	if (RockShellMaterial)
	{
		const bool bNewInstance = !RockShellMaterialInstance || RockShellMaterialInstance->Parent != RockShellMaterial;
		if (bNewInstance) RockShellMaterialInstance = UMaterialInstanceDynamic::Create(RockShellMaterial, this);
		RockShellMaterialInstance->SetScalarParameterValue(FName(CSRockShell::VertexColor::PatternScaleParameterName), Scale);
		DrawMaterial = RockShellMaterialInstance;
	}
	else RockShellMaterialInstance = nullptr;
	// 组件画 MID，网格的 Materials[0] 放资产本身（烘焙抓的是槽，transient 的 MID 不能进资产）。
	BindMeshSlotMaterials(RockShellComponent, RockShellMesh, { RockShellMaterial }, DrawMaterial);

	const FBox2D Rect = GetWorldRect2D();
	const uint32 GeometryHash = Pattern.GeometryHash();
	const int32 MaxCells = FMath::Clamp(RockShellMaxCells, 64, 8192);
	// 池的身份：图案几何（多边形是它的纯函数）+ 地面矩形（包围盒写死用；矩形的 Min 就是网格原点）。
	const bool bSameIdentity = RockShellMesh != nullptr
		&& RockShellPoolGeometryHash == GeometryHash
		&& RockShellPoolRect.bIsValid
		&& RockShellPoolRect.Min.Equals(Rect.Min, 1.0)
		&& RockShellPoolRect.Max.Equals(Rect.Max, 1.0)
		&& RockShellPoolCapacity > 0
		&& RockShellComponent->GetGpuMesh() == RockShellMesh
		// 删除后撤销：网格对象复活了，显存却已被组件还掉 —— 只比指针会把空网格当成建好了。
		&& IsSlotMeshLive(RockShellMesh);
	bool bNeedBuild = !bSameIdentity;
	if (bSameIdentity && NeededCells > RockShellPoolCapacity && RockShellPoolCapacity < MaxCells)
	{
		// 容量不够：拖动期不扩（扩容 = 阻塞 + 全部槽重生成），记下来松手补齐。
		if (!bLastChangeCommitted)
		{
			bRockShellGrowPending = true;
			return true;
		}
		bNeedBuild = true;
	}
	if (!bNeedBuild) return true;

	const int32 Capacity = FMath::Clamp(FMath::Max(FMath::CeilToInt32(float(NeededCells) * 1.5f), 256), 64, MaxCells);
	EnsureSlotMesh(RockShellComponent, RockShellMesh);   // 归岩壳组件所有：组件销毁时它自己还显存

	// **包围盒按地面矩形写死**（kernel 用 NaN 关掉看不见的三角，NaN 会污染任何从顶点算出来的包围盒）。
	// Z 的半高只由地面尺寸推导、不看 MaxAbsHeight —— 掺进塑形物状态的话拖一座塑形物就会改变包围盒 ⇒
	// 每帧重走这条阻塞的建池路径。
	const FVector Origin = GetActorLocation();
	const FVector2D Span = Rect.GetSize();
	const double ZHalf = FMath::Max(2000.0, FMath::Max(Span.X, Span.Y) * 0.25);
	const FBox Hard(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - ZHalf),
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + ZHalf));
	if (!CSRockShell::BuildMesh(RockShellMesh, Pattern, Capacity, Hard))
	{
		ResetPool();
		return false;
	}

	// 新建的网格槽表是空的：再绑一次（变了才广播），然后交给组件。
	BindMeshSlotMaterials(RockShellComponent, RockShellMesh, { RockShellMaterial }, DrawMaterial);
	RockShellComponent->SetGpuMesh(RockShellMesh);

	ResetPool();
	RockShellPoolGeometryHash = GeometryHash;
	RockShellPoolRect = Rect;
	RockShellPoolCapacity = Capacity;
	RockShellPoolTrianglesPerCell = Pattern.TrianglesPerCell();
	RockShellCellOfSlot.Init(CSGround_RockShellFreeSlot, Capacity);
	RockShellFreeSlots.Reserve(Capacity);
	for (int32 Slot = Capacity - 1; Slot >= 0; --Slot) RockShellFreeSlots.Add(Slot);   // 降序：从尾部取 ⇒ 先用小号
	bOutRebuilt = true;

	UE_LOG(LogTinyGladeGround, Log,
		TEXT("[TinyGladeGround] %s 岩壳槽位池已建：%d 槽 × %d 三角（胞腔 %.1f cm，M = %d，需要 %d 槽）。"),
		*GetName(), Capacity, Pattern.TrianglesPerCell(), Pattern.CellSize, Pattern.BoundaryPoints, NeededCells);
	return true;
}

void ACSGroundActor::RebuildRockShell()
{
	// ⚠️ **第一句就是哈希比较**：短路必须发生在昂贵计算**之前**。唯一的例外是拖动期欠下的扩容：
	// 松手那一趟输入可能与最后一帧逐位相同，但它必须把暂缺的胞腔补齐。
	const uint32 Hash = RockShellInputHash();
	const bool bGrowNow = bRockShellGrowPending && bLastChangeCommitted;
	if (RockShellBuiltHash != 0 && RockShellBuiltHash == Hash && !bGrowNow) return;

	if (IsTemplate() || !GetWorld()) return;
	if (!bRockShell)
	{
		bool bUnused = false;
		EnsureRockShellMesh(0, bUnused);   // 收掉组件与显存
		RockShellBuiltHash = Hash;
		return;
	}
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;   // 地面网格还没建：重建完会再走一次这条路
	const FCSMeshResidentRef GroundResident = TinyGladeMesh->GetResident();
	if (!GroundResident.IsValid()) return;

	const CSRockShell::FPatternParams Pattern = MakeRockShellPatternParams();
	const float Scale = Pattern.PatternScale();
	const FVector Origin = GetActorLocation();
	const FVector2D GridOrigin(Origin.X, Origin.Y);
	const FBox2D Rect = GetWorldRect2D();

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);

	// --- 1) 激活：哪些胞腔**可能**长石头（裙边环带 ⊕ 余量；只许过标不许漏标）---
	if (RockShellRadiusCacheHash != Pattern.GeometryHash())
	{
		RockShellRadiusCache.Reset();
		RockShellRadiusCacheHash = Pattern.GeometryHash();
	}
	// 余量 = 坡度差分跨距（一个地面格）+ 胀缩 / 扩张 / 倾斜能把顶点推出多远 + 5 cm。
	const float Margin = Mirror.CellSize
		+ (FMath::Max(RockShellCellJitter, 0.0f) + FMath::Max(RockShellCellExpand, 0.0f) + FMath::Max(RockShellSkirtTilt, 0.0f)) * Scale
		+ 5.0f;
	// 活着的坡度门槛：Rock = smoothstep(Lo, Hi, s) > 0.1 ⇔ t > 0.196（smoothstep 的逆）。只用于中心隆起的整盘判定。
	const float AliveSlope = RockShellSlopeLo + 0.196f * FMath::Max(RockShellSlopeHi - RockShellSlopeLo, 1e-3f);
	TArray<FIntPoint> Active;
	CSRockShell::CollectActiveCells(Pattern, GridOrigin, Rect, ShaperParams, Margin, AliveSlope, RockShellRadiusCache, Active);
	const int32 MaxCells = FMath::Clamp(RockShellMaxCells, 64, 8192);
	if (Active.Num() > MaxCells)
	{
		UE_LOG(LogTinyGladeGround, Warning,
			TEXT("[TinyGladeGround] %s 岩壳激活了 %d 个胞腔，超过上限 RockShellMaxCells = %d —— 按格键截断，截掉的那些不长石头。"),
			*GetName(), Active.Num(), MaxCells);
		Active.SetNum(MaxCells);
	}

	// --- 2) 槽位池（图案 / 矩形变了就重建，容量不够就扩；拖动期不扩）---
	bool bRebuilt = false;
	if (!EnsureRockShellMesh(Active.Num(), bRebuilt))
	{
		// 失败也记下哈希：同一份输入下不会自愈，每一 dab 重试一次只是把日志刷爆。
		RockShellBuiltHash = Hash;
		return;
	}

	// --- 3) 槽位差分：失效 → 还槽；新增 → 取槽（小号优先）；披挂 = 全部活跃（P1：全量重披挂）---
	CSRockShell::FJobs Jobs;
	TSet<FIntPoint> ActiveSet;
	ActiveSet.Reserve(Active.Num());
	for (const FIntPoint& Cell : Active) ActiveSet.Add(Cell);
	TArray<int32> Freed;
	for (auto It = RockShellSlotOfCell.CreateIterator(); It; ++It)
	{
		if (ActiveSet.Contains(It.Key())) continue;
		Freed.Add(It.Value());
		RockShellCellOfSlot[It.Value()] = CSGround_RockShellFreeSlot;
		It.RemoveCurrent();
	}
	if (!Freed.IsEmpty())
	{
		RockShellFreeSlots.Append(Freed);
		RockShellFreeSlots.Sort([](int32 A, int32 B) { return A > B; });
	}
	int32 Missing = 0;
	for (const FIntPoint& Cell : Active)
	{
		if (RockShellSlotOfCell.Contains(Cell)) continue;
		if (RockShellFreeSlots.IsEmpty()) { ++Missing; continue; }
		const int32 Slot = RockShellFreeSlots.Pop(EAllowShrinking::No);
		RockShellSlotOfCell.Add(Cell, Slot);
		RockShellCellOfSlot[Slot] = Cell;
		Jobs.NewCells.Add(FUintVector4(uint32(Slot), uint32(Cell.X), uint32(Cell.Y), 0u));
	}
	// 被还回去、这一趟又没被别的胞腔领走的槽才需要关（被领走的会被新胞腔整片重写）。
	for (const int32 Slot : Freed)
	{
		if (RockShellCellOfSlot[Slot] == CSGround_RockShellFreeSlot) Jobs.Kill.Add(uint32(Slot));
	}
	if (Missing > 0)
	{
		if (!bLastChangeCommitted) bRockShellGrowPending = true;   // 拖动期：松手补齐
		else if (RockShellPoolCapacity >= MaxCells)
		{
			UE_LOG(LogTinyGladeGround, Warning,
				TEXT("[TinyGladeGround] %s 岩壳槽位池已到上限 %d，还有 %d 个胞腔分不到槽。"), *GetName(), MaxCells, Missing);
		}
	}
	else if (bLastChangeCommitted)
	{
		bRockShellGrowPending = false;
	}
	Jobs.Drape.Reserve(RockShellSlotOfCell.Num());
	for (const TPair<FIntPoint, int32>& Pair : RockShellSlotOfCell) Jobs.Drape.Add(uint32(Pair.Value));
	Jobs.Drape.Sort();

	// --- 4) 一趟派发。**平面上的 TG 长度乘 PatternScale**（胀缩 / 扩张 / 倾斜、起伏噪声的域；唇宽在建池时）——
	// 它们要跟多边形尺寸配套。**竖向的厚度 / 基准偏移 / 起伏是世界 cm**：2026-09-30 一度也按胞腔缩，
	// 默认 1.94 m 的石头只剩 10.5 cm 厚，用户看图判「露出的部分变少」，改回不缩。---
	CSRockShell::FDisplaceParams Params;
	Params.Pattern = Pattern;
	Params.GridOrigin = FVector2f(float(GridOrigin.X), float(GridOrigin.Y));
	Params.UVWorldPeriod = UVWorldPeriod;
	Params.DomainMin = FVector2f(float(Rect.Min.X), float(Rect.Min.Y));
	Params.DomainMax = FVector2f(float(Rect.Max.X), float(Rect.Max.Y));
	Params.bSmoothNormals = bRockShellSmoothNormals;
	Params.SmoothAngleDeg = RockShellSmoothAngleDeg;
	Params.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
	Params.GroundCellSize = Mirror.CellSize;
	Params.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Params.GroundBaseZ = float(Origin.Z);
	Params.SlopeLo = FMath::Max(RockShellSlopeLo, 0.0f);
	Params.SlopeHi = RockShellSlopeHi;
	Params.RoadFade = FMath::Max(RockShellRoadFade, 1.0f);
	Params.RoadBlurRadius = FMath::Max(RockShellRoadBlurRadius, 0.0f);
	Params.RoadSink = FMath::Max(RockShellRoadSink, 0.0f);
	Params.CellJitter = FMath::Max(RockShellCellJitter, 0.0f) * Scale;
	Params.CellRelief = FMath::Max(RockShellCellRelief, 0.0f);
	Params.ReliefFloor = FMath::Clamp(RockShellReliefFloor, 0.0f, 1.0f);
	Params.CellExpand = FMath::Max(RockShellCellExpand, 0.0f) * Scale;
	Params.SkirtTilt = FMath::Max(RockShellSkirtTilt, 0.0f) * Scale;
	Params.BaseLift = FMath::Max(RockShellBaseLift, 0.0f);
	Params.BaseSink = FMath::Max(RockShellBaseSink, 0.0f);
	Params.NoiseAmp = FMath::Max(RockShellNoiseAmount, 0.0f);
	Params.NoiseFrequency = 1.0f / (FMath::Max(RockShellNoiseWavelength, 1.0f) * Scale);
	Params.RiseMultiplier = FMath::Max(RockShellRiseMultiplier, 0.0f);
	Params.RiseNoiseAmp = FMath::Max(RockShellRiseNoiseAmount, 0.0f);
	Params.RiseNoiseFrequency = 1.0f / FMath::Max(RockShellRiseNoiseWavelength, 1.0f);
	Params.RiseExtend = FMath::Max(RockShellRiseExtend, 0.0f);
	Params.EdgeCeiling = FMath::Clamp(RockShellEdgeCeiling, 0.0f, 0.999f);
	// ⑧ 的参考高度用现成的峰值台账（`RefreshHeightsInRegion` 里维护），不在这里重扫一遍塑形物。
	Params.PeakHeight = FMath::Max(MaxAbsHeight, 1.0f);
	Params.ChipAmount = FMath::Max(RockShellChipAmount, 0.0f);
	Params.ChipFrequency = 1.0f / FMath::Max(RockShellChipWavelength, 1.0f);
	Params.Seed = uint32(RockShellSeed);

	if (!CSRockShell::Displace(RockShellMesh, GroundResident, Params, ShaperParams, Jobs)) return;

	++RockShellDisplaceCount;
	RockShellBuiltHash = Hash;
	UE_LOG(LogTinyGladeGround, Verbose,
		TEXT("[TinyGladeGround] %s 岩壳：活跃 %d / 池 %d 槽%s；本趟新增 %d、关掉 %d、披挂 %d。"),
		*GetName(), RockShellSlotOfCell.Num(), RockShellPoolCapacity, bRebuilt ? TEXT("（池刚重建）") : TEXT(""),
		Jobs.NewCells.Num(), Jobs.Kill.Num(), Jobs.Drape.Num());
}

bool ACSGroundActor::IsRockShellDrawable(FString& OutReason) const
{
	OutReason = GetRockShellUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，所以这里必须自己打一行日志**（实测）：UE Python 把
		// "bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，**不可画时拿到 `None`**，
		// 原因串直接丢了。脚本要拿原因请调 GetRockShellUndrawableReason()。
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s rock shell not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSGroundActor::GetRockShellUndrawableReason() const
{
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bRockShell) { OutReason = TEXT("bRockShell 关着"); return OutReason; }
	if (!IsValid(RockShellComponent)) { OutReason = TEXT("没有渲染组件"); return OutReason; }
	if (!RockShellComponent->IsRegistered()) { OutReason = TEXT("渲染组件没注册"); return OutReason; }
	if (!RockShellComponent->IsVisible()) { OutReason = TEXT("渲染组件不可见"); return OutReason; }
	if (!RockShellMesh) { OutReason = TEXT("没有岩壳网格对象"); return OutReason; }
	if (RockShellComponent->GetGpuMesh() != RockShellMesh)
	{
		OutReason = TEXT("渲染组件绑的不是本 actor 的岩壳网格");
		return OutReason;
	}

	const FCSMeshResidentRef Resident = RockShellMesh->GetResident();
	if (!Resident.IsValid() || !Resident->IsAllocated()) { OutReason = TEXT("常驻流没分配"); return OutReason; }
	if (Resident->VertexCapacity < 3u) { OutReason = TEXT("顶点容量不足一个三角"); return OutReason; }
	if (!Resident->WorldBounds.IsValid) { OutReason = TEXT("包围盒无效（会被剔除掉）"); return OutReason; }

	// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
	// 画面上是一片灰，而所有 readback 断言照绿。所以把"有材质"做成显式判据。
	const bool bHasMaterial = RockShellComponent->MeshMaterial != nullptr
		|| (RockShellMesh->Materials.IsValidIndex(0) && RockShellMesh->Materials[0] != nullptr);
	if (!bHasMaterial) { OutReason = TEXT("没有绑材质（会用引擎默认表面材质画成一片灰）"); return OutReason; }

	if (RockShellDisplaceCount <= 0) { OutReason = TEXT("披挂 pass 一次都没跑过"); return OutReason; }
	return FString();
}

bool ACSGroundActor::GetRockShellPoolStats(int32& OutActiveCells, int32& OutCapacityCells, int32& OutTrianglesPerCell) const
{
	OutActiveCells = RockShellSlotOfCell.Num();
	OutCapacityCells = RockShellPoolCapacity;
	OutTrianglesPerCell = RockShellPoolTrianglesPerCell;
	return RockShellMesh != nullptr && RockShellPoolCapacity > 0;
}

int32 ACSGroundActor::DebugGetRockShellCellSlot(FIntPoint Cell) const
{
	const int32* Slot = RockShellSlotOfCell.Find(Cell);
	return Slot ? *Slot : INDEX_NONE;
}

void ACSGroundActor::DebugGetRockShellActiveCells(TArray<FIntPoint>& OutCells) const
{
	RockShellSlotOfCell.GenerateKeyArray(OutCells);
	OutCells.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.Y != B.Y ? A.Y < B.Y : A.X < B.X; });
}

int32 ACSGroundActor::DebugReadRockShellSync(TArray<FVector>& OutWorldPositions)
{
	OutWorldPositions.Reset();
	if (!RockShellMesh) return 0;

	FCSGpuMeshCPUData Readback;
	if (!RockShellMesh->ReadbackMeshSync(Readback)) return 0;
	// 常驻流就是世界空间（同地面），不需要变换。NaN 原样带出去 ——
	// 调用方靠它判三角死活（只看第 0 个顶点，见 CSGroundRockShellTests 里那段注释）。
	OutWorldPositions.Reserve(Readback.Positions.Num());
	for (const FVector3f& P : Readback.Positions) OutWorldPositions.Add(FVector(P));
	return OutWorldPositions.Num();
}

int32 ACSGroundActor::DebugReadRockShellCapSplitSync(int32& OutCapVerts, int32& OutSkirtVerts)
{
	OutCapVerts = 0;
	OutSkirtVerts = 0;
	if (!RockShellMesh) return 0;

	FCSGpuMeshCPUData Readback;
	if (!RockShellMesh->ReadbackMeshSync(Readback)) return 0;

	// 判据取中点而不是"== CapValue"：顶点色在常驻流里是 8 位量化过的（PackColorBGRA），
	// 回读回来是 255/255 与 0/255，浮点等号在别的通道宽度下会静默全判成裙。
	for (const FVector4f& C : Readback.Colors)
	{
		((C.X > 0.5f) ? OutCapVerts : OutSkirtVerts)++;
	}
	return Readback.Colors.Num();
}


// -----------------------------------------------------------------------------
// 塑形物裙边摆件（D12 锚点层的第五家）
//
// **归地面，不归塑形物** —— 三条依据逐条写在 `CSGroundDecor.h` 的文件头，一句话是
// 「塑形物只提供高度场，地面负责派生几何」。锚点取法也在那里：解析剖面的等值带上按弧长布点。
// 这一侧只做接线：读世界 → 生产锚点 → 共用的规划段 → 共用的打包 kernel。
// -----------------------------------------------------------------------------

void ACSGroundActor::BuildSkirtDecorSite(CSGroundDecor::FSite& OutSite) const
{
	OutSite.Rings.Reset();
	OutSite.BaseZ = GetActorLocation().Z;

	// ⚠️ 高度场参数走 `GetFieldHeader`，**不从 actor 属性再算一遍** ——
	// GPU 位移 pass、石阶扫描、CPU 镜像重导出读的都是这一份打包结果。各自再算一遍的症状是
	// "摆件那圈的半径与土台差了一点点"，只在改过 `SecondaryLiftScale` 之类之后才显形。
	for (const TWeakObjectPtr<ACSGroundShaperActor>& Weak : Shapers)
	{
		const ACSGroundShaperActor* Shaper = Weak.Get();
		if (!Shaper) continue;

		CSGroundDecor::FShaperRing Ring;
		Ring.Header = Shaper->GetFieldHeader();
		Ring.Sdf = TArray<float>(Shaper->GetMaskSdf());
		// 身份取 **actor 名**：随关卡序列化、拖动不变、加载顺序无关。数组下标与世界坐标
		// 为什么都不行，逐条写在 `CSGroundDecor.h` 的文件头（S1 在槽位上栽过一次）。
		Ring.Key = CSGroundDecor::RingKey(Shaper->GetName());
		OutSite.Rings.Add(Ring);
	}

	// 落高与道路排除走**镜像**（CPU 权威，与 `SampleHeight` / `SampleRoadWeight` 同一份）。
	// 这条链一次回读都不会有 —— 计划 D12 里那次异步回读是给复杂度场准备的，锚点这一半不需要。
	OutSite.SampleGroundZ = [this](const FVector2D& XY) { return SampleHeight(XY); };
	OutSite.SampleRoadWeight = [this](const FVector2D& XY) { return SampleRoadWeight(XY); };
}

CSHouseDecor::FParams ACSGroundActor::MakeSkirtDecorParams() const
{
	CSHouseDecor::FParams Params;
	Params.SkirtSpacing = SkirtDecorSpacing;
	Params.SkirtBandT = SkirtDecorBandT;
	Params.MinSpacing = SkirtDecorMinSpacing;
	Params.RoadReject = SkirtDecorRoadReject;
	Params.BaseScale = SkirtDecorScale;
	Params.ScaleJitter = SkirtDecorScaleJitter;
	Params.Seed = SkirtDecorSeed;
	return Params;
}

bool ACSGroundActor::EnsureSkirtDecorComponents(bool& bOutHandedOver)
{
	bOutHandedOver = false;
	TArray<UStaticMesh*> Wanted;
	for (const TObjectPtr<UStaticMesh>& Mesh : SkirtDecorMeshes)
	{
		if (Mesh) Wanted.Add(Mesh);
	}

	// palette 段：只有裙边那一格非空。其余四家的载体是房子，地面对它们一无所知 ——
	// 留 `{0, 0}` 就是 `BuildPlan` 里那句"这一家没配网格 ⇒ 一件都不长"。
	SkirtDecorPaletteRanges.SetNum(int32(CSHouseDecor::EFamily::Count));
	for (CSHouseDecor::FPaletteRange& Range : SkirtDecorPaletteRanges) Range = CSHouseDecor::FPaletteRange();
	SkirtDecorPaletteRanges[int32(CSHouseDecor::EFamily::Skirt)] = { 0, Wanted.Num() };

	// ⚠️ 组件数一变就必须重建基础网格快照：palette 与组件是**按下标**对齐的，少一个就全体错位。
	if (CSShaperSteps::EnsureInstancedComponents(this, SkirtDecorComponents, Wanted.Num())) bSkirtDecorBaseMeshReady = false;
	// 空 = 每个 palette 逐段画自己那张网格资产挂的材质（材质表随 `CSHouseVine::BuildBaseMesh` 抄进快照）；
	// 设了才整体覆盖。变了才让代理重建。
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents) Component->SetInstanceMaterial(SkirtDecorMaterial);

	if (SkirtDecorGpuBuffers.Num() != Wanted.Num())
	{
		CSShaperSteps::ReleaseOnRenderThread(SkirtDecorGpuBuffers);
		SkirtDecorGpuBuffers.SetNum(Wanted.Num());
		SkirtDecorHandover.Capacities.Reset();
		bSkirtDecorBaseMeshReady = false;
	}
	if (Wanted.Num() == 0) return false;

	bool bMeshesChanged = SkirtDecorMeshesBuiltFrom.Num() != Wanted.Num();
	for (int32 Index = 0; !bMeshesChanged && Index < Wanted.Num(); ++Index)
	{
		bMeshesChanged = SkirtDecorMeshesBuiltFrom[Index] != Wanted[Index];
	}
	if (bMeshesChanged) bSkirtDecorBaseMeshReady = false;

	if (!bSkirtDecorBaseMeshReady)
	{
		SkirtDecorMeshesBuiltFrom.SetNum(Wanted.Num());
		bool bAllOk = true;
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			FCSGpuMeshCPUData Data;
			// ⚠️ **复用藤蔓那份读取器，与房子那四家同一条**：它判"法线/UV 读出来合不合法"
			// （有流不等于有数据）、缺了就现补、并把**顶点色**搬进快照。顶点色对 clutter 是决定性的
			// —— `Content/HouseTest/TinyGladeAsset/Textures/` 里一张 clutter 贴图都没有，颜色全烘在顶点流里。
			// 长度轴传 2（不换轴）：摆件本来就以 +Z 为上。
			const bool bOk = CSHouseVine::BuildBaseMesh(Wanted[Index], 2, Data);
			if (bOk)
			{
				SkirtDecorComponents[Index]->SetBaseMeshFromGpuData(Data);

				FBox3f Local(ForceInit);
				for (const FVector3f& P : Data.Positions) Local += P;
				CSShaperSteps::FPaletteBuffers& Buffers = SkirtDecorGpuBuffers[Index];
				Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
				Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
				// clutter 网格自带真实尺寸（不是石阶那种单位立方体字典 mesh），块尺寸留 1 ——
				// 缩放全部活在记录的 Scale/ScaleZ 里。
				Buffers.BlockSize = FVector3f(1.0f, 1.0f, 1.0f);
			}
			SkirtDecorMeshesBuiltFrom[Index] = bOk ? Wanted[Index] : nullptr;
			bAllOk &= bOk;
		}
		bSkirtDecorBaseMeshReady = bAllOk;
	}
	if (!bSkirtDecorBaseMeshReady) return false;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是半径的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 —— 拖半径时
	// 每涨过一个间距就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	CSGroundDecor::FSite Site;
	BuildSkirtDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeSkirtDecorParams();
	const uint32 MaxRecords = uint32(FMath::Clamp(
		CSShaperSteps::ReserveCount(CSGroundDecor::MaxRecordsBound(Site, Params)), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(SkirtDecorGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤蔓 / 房子摆件那三段逐字相同
	// （1 cm 阈值会让拖动时每帧都重走一次阻塞的 SetInstanceSourceGPU）。
	// 组件挂在根上、相对变换为单位阵 ⇒ 组件空间就是 actor 空间（同石阶那条）。
	double MeshReach = 0.0;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : SkirtDecorGpuBuffers)
	{
		MeshReach = FMath::Max(MeshReach, double(Buffers.BaseSphereRadius));
	}
	MeshReach *= FMath::Max(double(SkirtDecorScale) * (1.0 + double(SkirtDecorScaleJitter)), 0.05);

	// 伸展取"最远那一圈离本 actor 原点多远"，而不是地面矩形 —— 塑形物完全可以站在地面外面
	// （高度场按 max 合成，出界那部分只是采不到镜像），拿地面矩形当上界会把它剔掉。
	const FVector Origin = GetActorLocation();
	double Reach = 0.0;
	double Top = 0.0;
	for (const CSGroundDecor::FShaperRing& Ring : Site.Rings)
	{
		const double Radius = double(CSGroundDecor::RingRadius(Ring, Params));
		const double DistX = FMath::Abs(double(Ring.Header.Profile.X) - Origin.X);
		const double DistY = FMath::Abs(double(Ring.Header.Profile.Y) - Origin.Y);
		Reach = FMath::Max(Reach, FMath::Max(DistX, DistY) + Radius + MeshReach);
		Top = FMath::Max(Top, double(CSGroundShaperField::PeakHeight(Ring.Header.Top)) + MeshReach);
	}
	Reach = CSShaperSteps::QuantizeUp(FMath::Max(Reach, double(MaxAbsHeight) + MeshReach + 1.0));
	Top = CSShaperSteps::QuantizeUp(Top + MeshReach + 1.0);
	// 下界取 −Reach：摆件落在**地面**上，而地面可以低于 actor 原点（被别人挖过 / 本来就有起伏）。
	FBox LocalBounds(FVector(-Reach, -Reach, -Reach), FVector(Reach, Reach, Top));
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, SkirtDecorHandover);

	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<8>> Sources;
	for (int32 Index = 0; Index < SkirtDecorGpuBuffers.Num(); ++Index)
	{
		Sources.Add(CSShaperSteps::MakeHandoverSource(
			SkirtDecorComponents.IsValidIndex(Index) ? ToRawPtr(SkirtDecorComponents[Index]) : nullptr,
			SkirtDecorGpuBuffers[Index], /*bWithCustomData*/ false));
	}
	const CSShaperSteps::EHandoverResult Result = CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, SkirtDecorHandover);
	bOutHandedOver = Result == CSShaperSteps::EHandoverResult::HandedOver;
	return Result != CSShaperSteps::EHandoverResult::NotReady;
}

void ACSGroundActor::RebuildSkirtDecor()
{
	if (IsTemplate() || !GetWorld()) return;

	bool bAnyMesh = false;
	for (const TObjectPtr<UStaticMesh>& Mesh : SkirtDecorMeshes) bAnyMesh |= (Mesh != nullptr);

	if (!bSkirtDecorEnabled || !bAnyMesh)
	{
		if (CurrentSkirtDecorInstanceCount != 0 || CurrentSkirtDecorAnchorCount != 0 || !SkirtDecorComponents.IsEmpty())
		{
			// ⚠️ 撤实例源之前先清 counter：不清的话下一次 `EnsureSkirtDecorComponents` 把同一批
			// 带陈旧计数器的 buffer 交回组件，剔除 pass 照着它再画一遍上一代的摆件 ——
			// 而 CPU 计数、三角形数、零阻塞断言**全部照绿**（同 RebuildVine / RebuildFrame / RebuildDecor）。
			CSShaperSteps::ZeroCounters(SkirtDecorGpuBuffers);
			for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents)
			{
				if (IsValid(Component)) Component->ClearInstanceSourceGPU();
			}
			SkirtDecorHandover.Reset();
			CurrentSkirtDecorAnchorCount = 0;
			CurrentSkirtDecorInstanceCount = 0;
			SkirtDecorHash = 0;
		}
		return;
	}
	if (!Mirror.IsInitialized()) return;
	bool bSkirtDecorHandedOver = false;
	if (!EnsureSkirtDecorComponents(bSkirtDecorHandedOver)) return;

	CSGroundDecor::FSite Site;
	BuildSkirtDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeSkirtDecorParams();

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSGroundDecor::BuildSkirtAnchors(Site, Params, Anchors);

	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, SkirtDecorPaletteRanges, SkirtDecorGpuBuffers.Num(), Plan);

	// 幂等短路。⚠️ 短路点在生产 + 规划**之后**是有意的（同房子那四家）：两步都是纯 CPU、
	// 微秒量级，而锚点表是哈希的**唯一诚实来源** —— 拿参数拼哈希而不跑生产，会在
	// "参数没动但塑形物被拖了 / 路被画到裙边上了"时静默漏更新，而这两件事恰恰都会改锚点。
	// 哈希直接盖住锚点的身份与量化后的位置，土台抬高一厘米也逃不掉。
	//
	// ⚠️ 岩壳那条"短路必须在昂贵计算之前"在这里**不适用**，别照抄：它短路掉的是一趟
	// 181 行规划 + 一次 dispatch，这里短路掉的是几十个三角函数。真正贵的那一步
	// （容量与包围盒交接）在 `EnsureSkirtDecorComponents` 里，它自己有零成本的稳态分支。
	TArray<int32> HashInput;
	HashInput.Reserve(Anchors.Num() * 5 + 16);
	HashInput.Append({ Anchors.Num(), Plan.TotalRecords(), SkirtDecorGpuBuffers.Num(),
		FMath::RoundToInt(SkirtDecorSpacing * 2.0f), FMath::RoundToInt(SkirtDecorBandT * 1000.0f),
		FMath::RoundToInt(SkirtDecorMinSpacing * 2.0f), FMath::RoundToInt(SkirtDecorRoadReject * 100.0f),
		FMath::RoundToInt(SkirtDecorScale * 100.0f), FMath::RoundToInt(SkirtDecorScaleJitter * 100.0f),
		SkirtDecorSeed });
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		// ⚠️ 逐个显式截回 int32：`FMath::RoundToInt(double)` 在 UE5 里返回的是 **int64**，
		// 位置量到 cm 早就在 int32 里放得下，但不写这层转换直接编译不过。
		HashInput.Append({ Anchor.AnchorId,
			int32(FMath::RoundToInt(Anchor.Location.X)), int32(FMath::RoundToInt(Anchor.Location.Y)),
			int32(FMath::RoundToInt(Anchor.Location.Z)), int32(FMath::RoundToInt(Anchor.Facing.X * 1000.0)) });
	}
	const uint32 NewHash = FCrc::MemCrc32(HashInput.GetData(), HashInput.Num() * sizeof(int32));

	bool bBuffersReady = SkirtDecorGpuBuffers.Num() == SkirtDecorComponents.Num();
	for (const CSShaperSteps::FPaletteBuffers& Buffers : SkirtDecorGpuBuffers) bBuffersReady &= Buffers.IsValid();
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重打包，哈希没变也不能早退（审查 B1）。
	if (!bSkirtDecorHandedOver && NewHash == SkirtDecorHash && SkirtDecorHandover.Capacities.Num() == SkirtDecorGpuBuffers.Num() && bBuffersReady) return;

	SkirtDecorHash = NewHash;
	CurrentSkirtDecorAnchorCount = Anchors.Num();
	CurrentSkirtDecorInstanceCount = Plan.TotalRecords();

	// 各 palette 的组件是同一棵挂接树上的兄弟、变换相同，取第 0 个即可。
	const FMatrix44f WorldToComponent = FMatrix44f(
		SkirtDecorComponents[0]->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseDecor::Pack(Plan, SkirtDecorGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeGround, Log, TEXT("[TinyGladeGround] %s 裙边摆件：锚点 %d 件 %d palette %d（塑形物 %d 座）"),
		*GetName(), CurrentSkirtDecorAnchorCount, CurrentSkirtDecorInstanceCount,
		SkirtDecorGpuBuffers.Num(), Site.Rings.Num());
}

bool ACSGroundActor::IsSkirtDecorDrawable(FString& OutReason) const
{
	OutReason = GetSkirtDecorUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsDecorDrawable / IsRockShellDrawable
		// 同一条）：UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本一律调 `GetSkirtDecorUndrawableReason()`。
		UE_LOG(LogTinyGladeGround, Warning, TEXT("[TinyGladeGround] %s 裙边摆件画不出来：%s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSGroundActor::GetSkirtDecorUndrawableReason() const
{
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（石阶那个坑：`StairMesh`
	// 恒 NULL、画面一撮黑块、而单测与回归全绿）。执行面照抄 `ACSHouseActor::GetDecorUndrawableReason`。
	if (!bSkirtDecorEnabled) return TEXT("bSkirtDecorEnabled 关着");
	if (SkirtDecorComponents.IsEmpty()) return TEXT("一个 palette 都没有（SkirtDecorMeshes 是空的）");
	if (!bSkirtDecorBaseMeshReady) return TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）");
	if (Shapers.IsEmpty()) return TEXT("这张地面上一座塑形物都没有登记（没有裙边，自然没有裙边摆件）");
	if (CurrentSkirtDecorAnchorCount <= 0) return TEXT("一个锚点都没生产出来（台高为 0？整圈都被邻座埋了或被路排掉了？）");
	if (CurrentSkirtDecorInstanceCount <= 0) return TEXT("锚点全被填充概率/最小间距球筛掉了");

	for (int32 Index = 0; Index < SkirtDecorComponents.Num(); ++Index)
	{
		const UCSGpuInstancedMeshComponent* Component = SkirtDecorComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("palette %d：没有渲染组件"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("palette %d：渲染组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("palette %d：渲染组件不可见"), Index);
		if (!Component->HasInstanceSourceGPU()) return FString::Printf(TEXT("palette %d：实例源没交接"), Index);
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			return FString::Printf(TEXT("palette %d：基础网格快照是空的"), Index);
		}
		if (!Component->GetGpuMesh()) return FString::Printf(TEXT("palette %d：GPU 网格没分配"), Index);

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 —— 画面上是一片灰，
		// 而所有 readback 断言照绿。⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在
		// 实例路径上会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。
		// 查的是**每一段解析出来的**材质（`SkirtDecorMaterial` 留空时是资产自己的），判据收在组件里。
		const FString MaterialReason = Component->GetMaterialUndrawableReason();
		if (!MaterialReason.IsEmpty()) return FString::Printf(TEXT("palette %d：%s"), Index, *MaterialReason);
	}
	return FString();
}

int32 ACSGroundActor::DebugReadSkirtDecorInstanceCountGpuSync() const
{
	// −1 = 一个组件都没有，与"真的是 0 个实例"分开：把读不到当 0 会让守着
	// "关掉之后必须归零"的断言在管线坏掉时假绿（同 DebugReadStairCountGpuSync 的口径）。
	if (SkirtDecorComponents.IsEmpty()) return -1;
	int32 Total = 0;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : SkirtDecorComponents)
	{
		if (!IsValid(Component)) continue;
		const int32 One = Component->DebugReadDrawnInstanceCountSync();
		if (One < 0) return -1;
		Total += One;
	}
	return Total;
}

#if WITH_EDITOR

// -----------------------------------------------------------------------------
// 地被：草 + 花（第六条派生链）
//
// 归属与三条纪律写在 `CSGroundCover.h` 的文件头。这里只做四件事：把 `Grass` + `Flowers`
// 收成物种表、按表保证组件/显存、把参数换算成 kernel 的口径、交接实例源。
//
// **一株一个 instance**（TG 的草也是每叶一个 instance，没有"一簇"这个概念）；
// CPU 全程不知道长了几株 —— 不排记录、不回读一个字节。
// -----------------------------------------------------------------------------

bool ACSGroundActor::ComputeCoverNearWindow(const FCSGroundCoverSpecies& Species, FCoverNearWindow& Out) const
{
	Out = FCoverNearWindow();
	const FBox2D Rect = GetWorldRect2D();
	const FVector2D Size = Rect.GetSize();
	if (Species.DensityPerSqM <= 0.0f || Size.X <= 1.0 || Size.Y <= 1.0) return false;

	// 整张网格：满密度、不退让（退让是整张地面一档那条路的事；窗口装不下时收的是窗口，不是密度）。
	Out.CellSize = 100.0f / FMath::Sqrt(Species.DensityPerSqM);
	Out.Origin = FVector2f(float(Rect.Min.X), float(Rect.Min.Y));
	Out.GlobalDims = FIntPoint(
		FMath::Max(1, FMath::CeilToInt32(float(Size.X) / Out.CellSize)),
		FMath::Max(1, FMath::CeilToInt32(float(Size.Y) / Out.CellSize)));

	// 窗口：以相机所在格为心、半宽 = 近处画距 + 重撒距离（相机在窗口心附近任意挪动 RescatterDistance 以内，
	// 画距内的草都还在窗口里）。格数恒定 ⇒ 容量恒定 ⇒ 窗口挪动只重撒、不交接（交接是阻塞的）。
	const float Half = FMath::Max(Species.LOD.NearDistance, 100.0f) + FMath::Max(Species.LOD.RescatterDistance, 0.0f);
	Out.HalfCells = FMath::Max(1, FMath::CeilToInt32(Half / Out.CellSize));
	const int32 Budget = FMath::Clamp(Species.MaxInstances, 64, 1048576);
	const int32 MaxHalf = FMath::Max(1, (FMath::FloorToInt32(FMath::Sqrt(float(Budget))) - 1) / 2);
	if (Out.HalfCells > MaxHalf)
	{
		Out.HalfCells = MaxHalf;
		Out.bShrunk = true;
	}
	const int32 Side = 2 * Out.HalfCells + 1;
	Out.Dims = FIntPoint(FMath::Min(Side, Out.GlobalDims.X), FMath::Min(Side, Out.GlobalDims.Y));

	// 近处真正画到多远：窗口没收小时就是 NearDistance；收小了就退到"窗口边 − 一格 − 重撒距离"，
	// 保证画距内永远在窗口里（否则相机一挪就看见窗口的方边）。
	const float Reach = float(Out.HalfCells - 1) * Out.CellSize - FMath::Max(Species.LOD.RescatterDistance, 0.0f);
	Out.Boundary = FMath::Max(FMath::Min(FMath::Max(Species.LOD.NearDistance, 100.0f), Reach), 100.0f);
	return true;
}

FIntPoint ACSGroundActor::CoverNearWindowOffset(const FCoverNearWindow& Window) const
{
	const FVector2D View = ResolveCoverViewXY();
	const FIntPoint Centre(
		FMath::FloorToInt32(float((View.X - double(Window.Origin.X)) / double(Window.CellSize))),
		FMath::FloorToInt32(float((View.Y - double(Window.Origin.Y)) / double(Window.CellSize))));
	// 夹在地面内：相机在地面边上 / 地面外时窗口贴边而不是一半落空（落空的那一半格 kernel 里直接退掉，浪费容量）。
	return FIntPoint(
		FMath::Clamp(Centre.X - Window.HalfCells, 0, FMath::Max(Window.GlobalDims.X - Window.Dims.X, 0)),
		FMath::Clamp(Centre.Y - Window.HalfCells, 0, FMath::Max(Window.GlobalDims.Y - Window.Dims.Y, 0)));
}

void ACSGroundActor::CollectCoverEntries(TArray<FCoverEntry>& OutEntries, TArray<int32>* OutFarIndexOfSpecies) const
{
	OutEntries.Reset();
	if (OutFarIndexOfSpecies) OutFarIndexOfSpecies->Reset();
	if (!bGroundCoverEnabled) return;

	// 下标 0 恒为草。网格为空的物种**整条跳过** —— 留一个空槽位只会得到一个绑着引擎默认
	// 网格的组件（一地白球），而且它还会占住容量。
	TArray<const FCSGroundCoverSpecies*> Species;
	if (Grass.Mesh) Species.Add(&Grass);
	for (const FCSGroundCoverSpecies& One : Flowers)
	{
		if (One.Mesh) Species.Add(&One);
	}

	for (const FCSGroundCoverSpecies* One : Species)
	{
		FCoverEntry& Entry = OutEntries.AddDefaulted_GetRef();
		Entry.Species = *One;
		// 两档要 FarMesh 非空才生效：没有远处网格就退回整张地面一档（远处照样有单根草，只是稀）。
		if (One->LOD.bEnabled && One->LOD.FarMesh && ComputeCoverNearWindow(*One, Entry.Window))
		{
			Entry.Role = ECoverRole::Near;
			Entry.Boundary = Entry.Window.Boundary;
			Entry.Fade = FMath::Clamp(One->LOD.FadeDistance, 0.0f, Entry.Boundary);
		}
	}

	// 远处组合草排在**全部物种之后**：物种自己的下标（0 = 草，1.. = 花）因此不变，脚本与诊断照旧。
	const int32 NumSpecies = OutEntries.Num();
	if (OutFarIndexOfSpecies) OutFarIndexOfSpecies->Init(INDEX_NONE, NumSpecies);
	for (int32 Index = 0; Index < NumSpecies; ++Index)
	{
		if (OutEntries[Index].Role != ECoverRole::Near) continue;
		const FCSGroundCoverSpecies& Near = OutEntries[Index].Species;
		FCoverEntry Far;
		Far.Species = Near;
		Far.Role = ECoverRole::Far;
		Far.Boundary = OutEntries[Index].Boundary;
		Far.Fade = OutEntries[Index].Fade;
		FCSGroundCoverSpecies& F = Far.Species;
		F.Mesh = Near.LOD.FarMesh;
		// 远处材质空 = 用物种自己的（那就带着 WPO，只剩网格换了）。
		F.Material = Near.LOD.FarMaterial ? Near.LOD.FarMaterial : Near.Material;
		F.DensityPerSqM = Near.LOD.FarDensityPerSqM;
		F.MaxInstances = Near.LOD.FarMaxInstances;
		F.ScaleRange = Near.LOD.FarScaleRange;
		// 一簇是一片草，整簇倾倒会读成"一块斜着的草皮"：簇内草叶自己带着倾倒，实例不再倒。
		F.LeanDegrees = 0.0f;
		// 盐与近处错开：否则远处每一簇都落在某一株近处草的格心上（两档逐格相关）。
		F.Salt = Near.Salt * 7919 + 104729;
		F.LOD = FCSGroundCoverLOD();
		if (OutFarIndexOfSpecies) (*OutFarIndexOfSpecies)[Index] = OutEntries.Num();
		OutEntries.Add(MoveTemp(Far));
	}
}

uint32 ACSGroundActor::CoverInputHash(const TArray<FCoverEntry>& Entries) const
{
	// float 走位模式哈希，与 `RockShellInputHash` 同一份：GetTypeHash(float) 对 −0.0 / NaN
	// 的处理不是我们要的语义，而这里只需要"变了没有"。
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };

	uint32 Hash = ::GetTypeHash(bGroundCoverEnabled);
	Hash = HashCombine(Hash, ::GetTypeHash(Entries.Num()));
	Hash = HashCombine(Hash, ::GetTypeHash(GroundCoverSeed));
	// 落笔计数：遮罩由顶点色算来，画一笔路就必须重散一次。逐笔哈希 257² 个字节太贵，
	// 一个单调计数器给出同样的判定（同岩壳那条）。地面遮罩的处理参数与贴图本身（尺寸变了会换一张）也算输入。
	Hash = HashCombine(Hash, ::GetTypeHash(PaintRevision));
	Hash = HashFloat(Hash, GroundMaskExtendDistance);
	Hash = HashFloat(Hash, GroundMaskWarpStrength);
	Hash = HashFloat(Hash, GroundMaskWarpWavelength);
	Hash = HashCombine(Hash, ::GetTypeHash(GroundMask.Get()));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsX));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsY));
	Hash = HashFloat(Hash, Mirror.CellSize);
	const FVector Origin = GetActorLocation();
	Hash = HashFloat(Hash, float(Origin.X));
	Hash = HashFloat(Hash, float(Origin.Y));
	Hash = HashFloat(Hash, float(Origin.Z));

	// 塑形物：哈希**打包后的高度场参数**而不是 actor 指针 —— 拖动一座时指针不变、参数变，
	// 而坡度门控与落高恰恰要跟着参数走（同岩壳那条的理由）。
	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	for (const FVector4f& Packed : ShaperParams)
	{
		Hash = HashFloat(Hash, Packed.X);
		Hash = HashFloat(Hash, Packed.Y);
		Hash = HashFloat(Hash, Packed.Z);
		Hash = HashFloat(Hash, Packed.W);
	}

	// 每个物种的**全部**配置都入哈希：漏一个的症状是"改了参数没反应"，静默无效，
	// 没有任何断言看得见（D14 开篇 FrameMaterial 那条的同型）。
	for (const FCoverEntry& Entry : Entries)
	{
		const FCSGroundCoverSpecies* One = &Entry.Species;
		// 两档：身份、距离带、近处窗口的位置（相机挪出重撒距离时就是它变了 ⇒ 重撒）。
		Hash = HashCombine(Hash, ::GetTypeHash(uint8(Entry.Role)));
		Hash = HashFloat(Hash, Entry.Boundary);
		Hash = HashFloat(Hash, Entry.Fade);
		if (Entry.Role == ECoverRole::Near)
		{
			const FIntPoint Offset = CoverNearWindowOffset(Entry.Window);
			Hash = HashCombine(Hash, HashCombine(::GetTypeHash(Offset.X), ::GetTypeHash(Offset.Y)));
			Hash = HashCombine(Hash, HashCombine(::GetTypeHash(Entry.Window.Dims.X), ::GetTypeHash(Entry.Window.Dims.Y)));
			Hash = HashFloat(Hash, Entry.Window.CellSize);
		}
		Hash = HashCombine(Hash, ::GetTypeHash(One->Mesh));
		Hash = HashCombine(Hash, ::GetTypeHash(One->Material));
		Hash = HashFloat(Hash, One->DensityPerSqM);
		Hash = HashCombine(Hash, ::GetTypeHash(One->MaxInstances));
		Hash = HashCombine(Hash, ::GetTypeHash(uint8(One->MaskChannel)));
		Hash = HashFloat(Hash, One->MaskStart);
		Hash = HashFloat(Hash, One->MaskEnd);
		Hash = HashFloat(Hash, One->MaskShorten);
		Hash = HashFloat(Hash, One->MaxSlopeDegrees);
		Hash = HashFloat(Hash, One->Jitter);
		Hash = HashFloat(Hash, float(One->ScaleRange.X));
		Hash = HashFloat(Hash, float(One->ScaleRange.Y));
		Hash = HashFloat(Hash, One->HeightJitter);
		Hash = HashFloat(Hash, One->LeanDegrees);
		Hash = HashFloat(Hash, One->AlignToNormal);
		Hash = HashFloat(Hash, One->ClumpSize);
		Hash = HashFloat(Hash, One->ClumpRadialChance);
		Hash = HashFloat(Hash, One->ClumpAlignment);
		Hash = HashFloat(Hash, One->ScaleClumpShare);
		Hash = HashFloat(Hash, float(One->BendRange.X));
		Hash = HashFloat(Hash, float(One->BendRange.Y));
		Hash = HashFloat(Hash, One->RadialBendScale);
		Hash = HashFloat(Hash, One->Sink);
		Hash = HashFloat(Hash, One->HeightOffset);
		Hash = HashCombine(Hash, ::GetTypeHash(One->bCastShadow));
		Hash = HashCombine(Hash, ::GetTypeHash(One->Salt));
		Hash = HashFloat(Hash, One->PatchSize);
		Hash = HashFloat(Hash, One->PatchCoverage);
		Hash = HashFloat(Hash, One->PatchSoftness);
		Hash = HashFloat(Hash, One->PatchSparseDensity);
	}
	// WPO 淡出不参与散布，但它写在组件上（EnsureCoverComponents），而那一步排在这条短路后面 —— 不进哈希就是"改了没反应"。
	Hash = HashFloat(Hash, CoverWPOFadeLength);
	// 0 是"还没散过"的哨兵，真算出 0 时挪开一格，否则那一轮的短路永远不成立。
	return Hash == 0 ? 1u : Hash;
}

bool ACSGroundActor::EnsureCoverComponents(const TArray<FCoverEntry>& Entries)
{
	// 旧代码按"物种指针表"写的；组件表的下标与条目一一对应（远处条目排在最后），其余口径不变。
	TArray<const FCSGroundCoverSpecies*> Species;
	Species.Reserve(Entries.Num());
	for (const FCoverEntry& Entry : Entries) Species.Add(&Entry.Species);

	// 关掉（或一个物种都没有）：连组件带显存一起收掉。留着空组件只会在 details 面板里
	// 留个误导性的槽位。组件那一份显存由组件销毁时自己放，这里只放生产者自己的 buffer。
	if (Species.IsEmpty())
	{
		for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : CoverComponents) if (IsValid(Component)) Component->DestroyComponent();
		CoverComponents.Reset();
		for (CSGroundCover::FCoverBuffers& One : CoverBuffers) CSGroundCover::ReleaseOnRenderThread(One);
		CoverBuffers.Reset();
		CoverMeshesBuiltFrom.Reset();
		CoverBaseSphereCentres.Reset();
		CoverBaseSphereRadii.Reset();
		CoverHandover.Reset();
		return false;
	}

	// 物种数或某一张网格变了 ⇒ 组件与物种表按**下标**对齐的前提没了，整批重建。
	// ⚠️ 不能只看物种数：在细节面板里换掉网格时数量不变，只看数量会得到"换了资产但什么都
	// 没发生"（裙边摆件那条踩过同一个坑）。
	bool bLayoutChanged = CoverComponents.Num() != Species.Num() || CoverMeshesBuiltFrom.Num() != Species.Num();
	for (int32 Index = 0; !bLayoutChanged && Index < Species.Num(); ++Index)
	{
		if (CoverMeshesBuiltFrom[Index] != Species[Index]->Mesh) bLayoutChanged = true;
		if (!IsValid(CoverComponents[Index])) bLayoutChanged = true;   // 蓝图重跑构造脚本会把组件销毁
	}

	if (bLayoutChanged)
	{
		for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : CoverComponents) if (IsValid(Component)) Component->DestroyComponent();
		CoverComponents.Reset();
		CoverComponents.SetNum(Species.Num());
		CoverMeshesBuiltFrom.Reset();
		CoverMeshesBuiltFrom.SetNum(Species.Num());
		// 先放引用再缩表。不是安全要求（`TRefCountPtr<FRDGPooledBuffer>` 在游戏线程析构只是减一次原子计数，
		// 池子恒持一份、在途帧读的 buffer 抽不走 —— 2026-09-07 审查 B7），只是让释放排在已入队的命令之后。
		// 物种数不变时这一趟也照做 —— 组件已经重建过了，旧 buffer 上没有任何消费者，
		// 重新分配一次是配置变更该付的代价。
		for (CSGroundCover::FCoverBuffers& One : CoverBuffers) CSGroundCover::ReleaseOnRenderThread(One);
		CoverBuffers.Reset();
		// 组件重建 ⇒ 新组件身上没有实例源，交接缓存必须一起作废，否则"缓存说交接过了"
		// 而组件其实是空的，画面永远是空的且没有任何报错。
		CoverHandover.Reset();
		CoverHandover.Capacities.SetNumZeroed(Species.Num());
	}

	CoverBaseSphereCentres.SetNum(Species.Num());
	CoverBaseSphereRadii.SetNum(Species.Num());
	CoverBuffers.SetNum(Species.Num());
	CoverHandover.Capacities.SetNum(Species.Num());

	// WPO 淡出的交界 = 草的近 / 远边界（近处窗口真正画到的距离，窗口被预算收小时跟着收）；草没分两档时取它的 NearDistance。
	// 草与每一种花共用这一条（2026-09-29 用户："直至交界处时彻底消失"），见 `CoverWPOFadeLength`。
	float WPOFadeEnd = FMath::Max(Grass.LOD.NearDistance, 100.0f);
	if (Grass.Mesh && Entries.Num() > 0 && Entries[0].Role == ECoverRole::Near) WPOFadeEnd = FMath::Max(Entries[0].Boundary, 100.0f);
	const float WPOFadeStart = FMath::Max(WPOFadeEnd - FMath::Max(CoverWPOFadeLength, 0.0f), 0.0f);

	for (int32 Index = 0; Index < Species.Num(); ++Index)
	{
		const FCSGroundCoverSpecies& One = *Species[Index];
		if (!IsValid(CoverComponents[Index]))
		{
			CoverComponents[Index] = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			CoverComponents[Index]->SetupAttachment(RootComponent);
			CoverComponents[Index]->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格
		}
		CoverComponents[Index]->InstanceMaterial = One.Material;
		// 影子副本排在换网格之前：网格一换组件就重建一次，先定好要不要那套经典缓冲，免得同一趟建两回。
		// 投影的物种（花）在 Nanite 路上把影子留给经典副本 —— 影子因此仍落在 CSM 那份软度里（2026-09-29 用户选的方案）。
		// 不投影的（草）不要副本；不走 Nanite 路时这一条什么都不做。
		CoverComponents[Index]->SetClassicShadowOnNanitePath(One.bCastShadow);
		CoverComponents[Index]->SetBaseMesh(One.Mesh);     // 同一张网格时内部直接早退
		// 逐物种的投影开关，默认关（见 `FCSGroundCoverSpecies::bCastShadow`）。组件是 Transient、
		// 每次重建都是新对象，而构造函数里 `CastShadow = true` ⇒ 这一行必须每趟都写，不能只在新建时写。
		CoverComponents[Index]->SetCastShadow(One.bCastShadow);
		CoverMeshesBuiltFrom[Index] = One.Mesh;

		// WPO：近处与单档条目在 [交界 − 淡出长度, 交界] 里由材质淡到 0，交界之外引擎整个不求 WPO（Nanite 的逐实例
		// WPO 关闭距离）。远处组合草的材质本来就没有 WPO：两个下标写 0 = 材质不淡出、组件不关（关了也没东西可关）。
		{
			const bool bFarClump = Entries[Index].Role == ECoverRole::Far;
			UCSGpuInstancedMeshComponent* Component = CoverComponents[Index];
			Component->SetCustomPrimitiveDataFloat(CoverWPOFadeStartIndex, bFarClump ? 0.0f : WPOFadeStart);
			Component->SetCustomPrimitiveDataFloat(CoverWPOFadeEndIndex, bFarClump ? 0.0f : WPOFadeEnd);
			Component->InstanceWPODisableDistance = bFarClump ? 0 : FMath::CeilToInt32(WPOFadeEnd);
			// Nanite 路的距离带按哪个视点判：脚本钉住了窗口就按钉住的机位，否则替身自己跟相机。
			if (CoverViewOverride.IsSet()) Component->SetDistanceCullViewOverride(CoverViewOverrideLocation);
			else Component->ClearDistanceCullViewOverride();
		}

		// 近 / 远两档的距离带：近处 End = 边界、远处 Start = 边界，同一条过渡带 ⇒ 交界互补、没有硬圈。
		// 普通条目三个都归零（从两档切回一档时要把上一轮写上去的清掉）。代理构造时抄走这三个数，变了才重建渲染状态。
		{
			const FCoverEntry& Entry = Entries[Index];
			const float WantEnd = Entry.Role == ECoverRole::Near ? Entry.Boundary : 0.0f;
			const float WantStart = Entry.Role == ECoverRole::Far ? Entry.Boundary : 0.0f;
			const float WantFade = Entry.Role == ECoverRole::Plain ? 0.0f : Entry.Fade;
			UCSGpuInstancedMeshComponent* Component = CoverComponents[Index];
			if (Component->InstanceEndCullDistance != WantEnd || Component->InstanceStartCullDistance != WantStart
				|| Component->InstanceCullFadeDistance != WantFade)
			{
				Component->InstanceEndCullDistance = WantEnd;
				Component->InstanceStartCullDistance = WantStart;
				Component->InstanceCullFadeDistance = WantFade;
				Component->MarkRenderStateDirty();
			}
		}

		const FBox Local = One.Mesh->GetBoundingBox();
		CoverBaseSphereCentres[Index] = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
		CoverBaseSphereRadii[Index] = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;
		// ⚠️ 这里**不再**按包围盒算坐底修正（原先的 `bSeatOnBase`，2026-09-09 拆掉）：网格原点
		// 就是落点，高出地面是作者摆的、也可能正是想要的，要挪走 `HeightOffset` 那个显式旋钮。
	}
	return true;
}

void ACSGroundActor::RebuildGroundCover()
{
	if (IsTemplate() || !GetWorld()) return;

	TArray<FCoverEntry> Entries;
	TArray<int32> FarIndexOfSpecies;
	CollectCoverEntries(Entries, &FarIndexOfSpecies);

	// 关掉时也要走一趟 Ensure —— 那是把上一轮的组件与显存收掉的唯一路径（同石阶
	// `EnsureStairComponent` 里 `!StairMesh` 那一支）。收完哈希清零，重新打开时才会再散。
	if (Entries.IsEmpty())
	{
		EnsureCoverComponents(Entries);
		CoverFarIndexOfSpecies.Reset();
		CoverBuiltHash = 0;
		return;
	}
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;

	const FCSMeshResidentRef Resident = TinyGladeMesh->GetResident();
	if (!Resident.IsValid()) return;   // 网格还没分配：地面重建完会再走一次这条路

	// 幂等短路，排在任何昂贵计算之前（同岩壳 / 裙边摆件）。哈希里带 PaintRevision，
	// 所以画笔一落它必然不成立 —— "路上不长草"要的就是这个时序。
	const uint32 Hash = CoverInputHash(Entries);
	UE_LOG(LogTinyGladeGround, Verbose, TEXT("[TinyGladeGround] %s RebuildGroundCover hash=%u built=%u entries=%d"),
		*GetName(), Hash, CoverBuiltHash, Entries.Num());
	if (Hash == CoverBuiltHash && !CoverComponents.IsEmpty()) return;

	if (!EnsureCoverComponents(Entries)) return;
	CoverFarIndexOfSpecies = FarIndexOfSpecies;

	const FVector Origin = GetActorLocation();
	const FBox2D Rect = GetWorldRect2D();
	const FMatrix44f WorldToComponent = FMatrix44f(GetActorTransform().ToInverseMatrixWithScale());

	TArray<CSGroundCover::FScatterParams> AllParams;
	AllParams.Reserve(Entries.Num());
	double WorstReach = 0.0;

	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		const FCoverEntry& Entry = Entries[Index];
		const FCSGroundCoverSpecies& One = Entry.Species;

		// 格数上限 = 容量：一格最多一株，所以这一个旋钮同时钳住显存与 dispatch 规模。
		// 超预算时 MakeGridForDensity 让密度退让（草变稀），而不是把编辑器跑挂。
		const int32 CellBudget = FMath::Clamp(One.MaxInstances, 64, 1048576);
		CSGroundCover::FScatterParams P;
		bool bClamped = false;
		if (Entry.Role == ECoverRole::Near)
		{
			// 近处：相机周围一块窗口、满密度。随机源用整张网格里的格号（kernel 里 + CellOffset），
			// 窗口挪动时同一格的草不变。窗口大小恒定 ⇒ 容量恒定 ⇒ 挪窗口只重撒、不交接。
			P.GridOriginXY = Entry.Window.Origin;
			P.CellSize = Entry.Window.CellSize;
			P.GridDims = Entry.Window.Dims;
			P.CellOffset = CoverNearWindowOffset(Entry.Window);
			P.GlobalDims = Entry.Window.GlobalDims;
			if (Entry.Window.bShrunk && Entry.Window.Dims != CoverLastLoggedNearDims)
			{
				CoverLastLoggedNearDims = Entry.Window.Dims;
				UE_LOG(LogTinyGladeGround, Log,
					TEXT("[TinyGladeGround] %s 地被物种 %d 近处窗口被 MaxInstances=%d 收小：%d×%d 格，近处只画到 %.0f cm（NearDistance=%.0f）"),
					*GetName(), Index, CellBudget, Entry.Window.Dims.X, Entry.Window.Dims.Y, Entry.Boundary, One.LOD.NearDistance);
			}
		}
		else
		{
			bClamped = CSGroundCover::MakeGridForDensity(
				Rect, One.DensityPerSqM, CellBudget, P.GridOriginXY, P.CellSize, P.GridDims);
		}
		if (bClamped)
		{
			// 每一趟都打会在拖地面尺寸时刷屏；但完全不打的话"我把密度调到 200 却没变密"
			// 就成了一个没有任何线索的现象。折中：只在真的退让时打，且带上换算后的实际密度。
			const float Actual = 10000.0f / FMath::Max(P.CellSize * P.CellSize, 1.0f);
			UE_LOG(LogTinyGladeGround, Log,
				TEXT("[TinyGladeGround] %s 地被物种 %d 密度退让：%.1f -> %.1f 株/m²（格数被 MaxInstances=%d 钳住）"),
				*GetName(), Index, One.DensityPerSqM, Actual, CellBudget);
		}

		const uint32 Capacity = uint32(FMath::Max(P.GridDims.X * P.GridDims.Y, 64));
		if (!CSGroundCover::EnsureBuffers(CoverBuffers[Index], Capacity)) return;

		P.WorldToComponent = WorldToComponent;
		P.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
		P.GroundCellSize = Mirror.CellSize;
		P.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
		P.GroundBaseZ = float(Origin.Z);

		// 遮罩通道 → one-hot。dot 出来的就是那一个通道，kernel 里因此没有分支。
		switch (One.MaskChannel)
		{
		case ECSGroundCoverMaskChannel::Green: P.MaskChannel = FVector4f(0.0f, 1.0f, 0.0f, 0.0f); break;
		case ECSGroundCoverMaskChannel::Blue:  P.MaskChannel = FVector4f(0.0f, 0.0f, 1.0f, 0.0f); break;
		case ECSGroundCoverMaskChannel::Alpha: P.MaskChannel = FVector4f(0.0f, 0.0f, 0.0f, 1.0f); break;
		default:                               P.MaskChannel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f); break;
		}
		P.MaskStart = One.MaskStart;
		P.MaskEnd = One.MaskEnd;
		P.MaskShorten = One.MaskShorten;
		// R（道路权重）读地面遮罩 —— 地面材质混贴图用的也是它（"地形 mask 决定草"）；选了别的通道的物种照旧读色流原值。
		P.bUseGroundMask = One.MaskChannel == ECSGroundCoverMaskChannel::Red && GroundMask != nullptr;
		P.MaskWarpStrength = GroundMaskWarpStrength;   // 与地面材质同一对扰动参数（BindGroundMaterial）
		P.MaskWarpWavelength = GroundMaskWarpWavelength;
		// 密度已经烘进格距里了，所以概率恒为 1 —— 它在这条路上只剩"这个物种开着吗"这一个语义。
		P.Chance = 1.0f;
		P.Jitter = One.Jitter;
		// 最大坡度角 → 法线 .z 的下限。角度是给人看的，kernel 要的是 cos。
		P.MinSlopeCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(One.MaxSlopeDegrees, 0.0f, 89.0f)));
		P.Sink = FMath::Max(One.Sink, 0.0f);
		P.HeightOffset = One.HeightOffset;
		P.ScaleRange = FVector2f(float(One.ScaleRange.X), float(One.ScaleRange.Y));
		P.HeightJitter = One.HeightJitter;
		P.LeanMaxRad = FMath::DegreesToRadians(FMath::Max(One.LeanDegrees, 0.0f));
		P.AlignToNormal = One.AlignToNormal;
		P.ClumpSize = One.ClumpSize;
		P.ClumpRadialChance = One.ClumpRadialChance;
		P.ClumpAlignment = One.ClumpAlignment;
		P.ScaleClumpShare = One.ScaleClumpShare;
		P.BendRange = FVector2f(float(One.BendRange.X), float(One.BendRange.Y));
		P.RadialBendScale = One.RadialBendScale;
		// 聚散（花）。远处组合草复制的是草那一份（草默认 0 = 关），不会平白长出一片一片的草。
		P.PatchSize = One.PatchSize;
		P.PatchCoverage = One.PatchCoverage;
		P.PatchSoftness = One.PatchSoftness;
		P.PatchSparseDensity = One.PatchSparseDensity;
		P.Seed = uint32(GroundCoverSeed);
		// 盐里混进下标：两行花配了同一个 Salt 时仍然分得开。用户填的 Salt 仍然生效
		// （它决定"同一下标下换个花样"），只是不再是唯一的分离手段。
		P.Salt = uint32(One.Salt) * 2654435761u + uint32(Index) * 2246822519u;
		P.BaseSphereCentre = CoverBaseSphereCentres[Index];
		P.BaseSphereRadius = CoverBaseSphereRadii[Index];

		// 保守包围盒的最坏伸展。**只能由配置算出来，一个实例都不能读** —— 掺进实例数据的话
		// 包围盒会随落笔漂移，`bNeedHandover` 每一笔都成立，阻塞的 SetInstanceSourceGPU
		// 会把"交互期零阻塞"整条纪律退化掉（拉石阶尺寸那一轮踩过）。
		const float WorstScale = P.ScaleRange.Y * (1.0f + P.HeightJitter);
		// `HeightOffset` 把整片上下挪，必须算进去（不乘缩放，它就是厘米数）；取绝对值是因为
		// 它可正可负而包围盒两头都要留。漏掉这类整体位移的症状是俯视时边缘那一圈花被剔掉 ——
		// 只在特定机位出现，最难复现。
		WorstReach = FMath::Max(WorstReach,
			double(P.BaseSphereRadius) * double(WorstScale) + double(P.Sink) + double(FMath::Abs(P.HeightOffset)));

		AllParams.Add(P);
	}

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	if (!CSGroundCover::Scatter(Resident, CoverBuffers, AllParams, ShaperParams, TArray<FVector4f>(), GroundMask)) return;

	// 包围盒按"地面矩形 × MaxAbsHeight"写死（同石阶）：只有 GPU 知道摆了哪些，CPU 不能读。
	// 全部物种共用一个盒子 —— 它已经按最坏物种放大过，共用还顺带保证所有交接同时稳态、
	// 同时触发（各算各的话，改一个物种的缩放会让别的物种也重新交接一次）。
	// MaxAbsHeight 过一次 QuantizeUp，理由同石阶：它随塑形连续变，不吸成台阶就是拖 LiftHeight 时每帧交接。
	const double HeightReach = CSShaperSteps::QuantizeUp(double(MaxAbsHeight));
	const FVector LocalMin = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - HeightReach)) - FVector(WorstReach);
	const FVector LocalMax = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + HeightReach)) + FVector(WorstReach);
	const FBox LocalBounds(LocalMin.ComponentMin(LocalMax), LocalMin.ComponentMax(LocalMax));

	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<8>> Sources;
	for (int32 Index = 0; Index < CoverBuffers.Num(); ++Index)
	{
		CSShaperSteps::FHandoverSource One;
		One.Component = CoverComponents.IsValidIndex(Index) ? ToRawPtr(CoverComponents[Index]) : nullptr;
		One.PackedInstances = CoverBuffers[Index].PackedInstances;   // 保留自己的引用，重散还要用
		One.Counter = CoverBuffers[Index].Counter;
		// 逐实例 custom data：组件只看它是否有效来决定要不要把 SRV 交给顶点工厂，
		// 不交的话材质里的 `Per Instance Custom Data` 恒读 0 —— 弯曲整条静默失效。
		One.CustomData = CoverBuffers[Index].CustomData;
		One.Capacity = CoverBuffers[Index].Capacity;
		Sources.Add(MoveTemp(One));
	}
	// 逐物种各判各的：物种之间互不相干，一个网格没就绪不该拖住别的。**不并旧盒** ——
	// 盒子按"地面矩形 × MaxAbsHeight"由配置算死，不会随落笔漂移。
	CSShaperSteps::FHandoverOptions Options;
	Options.Mode = CSShaperSteps::EHandoverMode::PerPalette;
	Options.LogLabel = TEXT("地被物种");
	// 改了就通知（Nanite 路因此不再每帧重写 GPU-Scene）。CoverBuffers 只有上面那一处 CSGroundCover::Scatter 在写，
	// 写完必到这里；容量变了走 EnsureBuffers 换新 buffer，那是重新交接。2026-10-02 数过，加新的写入路径要一起改。
	Options.bNotifiesChanges = true;
	CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, CoverHandover, Options, this);
	CoverBuiltHash = Hash;
}

// -----------------------------------------------------------------------------
// 建筑周边灌木（第七条派生链，照 TG `_garden_spawn_bushes_buildings.cs`）
// -----------------------------------------------------------------------------

void ACSGroundActor::SetBuildingFootprint(const UObject* Key, TConstArrayView<FVector2D> WorldPolygon)
{
	if (!Key) return;
	if (WorldPolygon.Num() < 3)
	{
		RemoveBuildingFootprint(Key);
		return;
	}
	// 房子每次重求值都会推一遍：没变就当场返回，逐帧拖动时才真的变。容差 0.5 cm 吸收浮点抖动。
	TArray<FVector2D>& Stored = BuildingFootprints.FindOrAdd(TObjectKey<UObject>(Key));
	bool bSame = Stored.Num() == WorldPolygon.Num();
	for (int32 Index = 0; bSame && Index < Stored.Num(); ++Index) bSame = Stored[Index].Equals(WorldPolygon[Index], 0.5);
	if (bSame) return;
	Stored = TArray<FVector2D>(WorldPolygon.GetData(), WorldPolygon.Num());
	RebuildBuildingBushes();
}

void ACSGroundActor::RemoveBuildingFootprint(const UObject* Key)
{
	if (BuildingFootprints.Remove(TObjectKey<UObject>(Key)) > 0) RebuildBuildingBushes();
}

void ACSGroundActor::CollectBushLayers(TArray<UStaticMesh*>& OutMeshes, TArray<UMaterialInterface*>& OutMaterials, TArray<float>& OutOverlayChance) const
{
	OutMeshes.Reset();
	OutMaterials.Reset();
	OutOverlayChance.Reset();
	if (!bBuildingBushesEnabled || !BuildingBushes.Mesh) return;
	OutMeshes.Add(BuildingBushes.Mesh);
	OutMaterials.Add(BuildingBushes.Material);
	OutOverlayChance.Add(1.0f);
	// 花是叠加层：与本体同一套参数、同一个盐 ⇒ 落点逐位相同，kernel 里再抽一次签只留一部分。
	if (BuildingBushes.FlowerMesh && BuildingBushes.FlowerChance > 0.0f)
	{
		OutMeshes.Add(BuildingBushes.FlowerMesh);
		OutMaterials.Add(BuildingBushes.FlowerMaterial);
		OutOverlayChance.Add(BuildingBushes.FlowerChance);
	}
}

uint32 ACSGroundActor::BushInputHash(const TArray<UStaticMesh*>& Meshes, const TArray<UMaterialInterface*>& Materials) const
{
	auto HashFloat = [](uint32 Seed, float Value) { return HashCombine(Seed, *reinterpret_cast<const uint32*>(&Value)); };

	uint32 Hash = ::GetTypeHash(bBuildingBushesEnabled);
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		Hash = HashCombine(Hash, ::GetTypeHash(Meshes[Index]));
		Hash = HashCombine(Hash, ::GetTypeHash(Materials[Index]));
	}
	const FCSGroundBuildingBushes& B = BuildingBushes;
	Hash = HashFloat(Hash, B.FlowerChance);
	Hash = HashFloat(Hash, B.DensityPerSqM);
	Hash = HashFloat(Hash, B.RingInner);
	Hash = HashFloat(Hash, B.RingWidth);
	Hash = HashFloat(Hash, B.NoiseCellSize);
	Hash = HashFloat(Hash, B.NoiseThreshold);
	Hash = HashFloat(Hash, B.MinRingScale);
	Hash = HashFloat(Hash, float(B.ScaleRange.X));
	Hash = HashFloat(Hash, float(B.ScaleRange.Y));
	Hash = HashFloat(Hash, B.HeightJitter);
	Hash = HashFloat(Hash, B.MaxSlopeDegrees);
	Hash = HashFloat(Hash, B.RoadMaskStart);
	Hash = HashFloat(Hash, B.RoadMaskEnd);
	Hash = HashFloat(Hash, B.Sink);
	Hash = HashFloat(Hash, B.HeightOffset);
	Hash = HashCombine(Hash, ::GetTypeHash(B.MaxInstances));
	Hash = HashCombine(Hash, ::GetTypeHash(B.bCastShadow));
	Hash = HashCombine(Hash, ::GetTypeHash(B.Salt));
	Hash = HashCombine(Hash, ::GetTypeHash(GroundCoverSeed));

	// 地面这一侧的输入与地被相同：路（顶点色，一个单调计数器代表；读的是它处理成的地面遮罩）、格子、位置、高度场参数。
	Hash = HashCombine(Hash, ::GetTypeHash(PaintRevision));
	Hash = HashFloat(Hash, GroundMaskExtendDistance);
	Hash = HashFloat(Hash, GroundMaskWarpStrength);
	Hash = HashFloat(Hash, GroundMaskWarpWavelength);
	Hash = HashCombine(Hash, ::GetTypeHash(GroundMask.Get()));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsX));
	Hash = HashCombine(Hash, ::GetTypeHash(Mirror.NumVertsY));
	Hash = HashFloat(Hash, Mirror.CellSize);
	const FVector Origin = GetActorLocation();
	Hash = HashFloat(Hash, float(Origin.X));
	Hash = HashFloat(Hash, float(Origin.Y));
	Hash = HashFloat(Hash, float(Origin.Z));
	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	for (const FVector4f& Packed : ShaperParams)
	{
		Hash = HashFloat(Hash, Packed.X);
		Hash = HashFloat(Hash, Packed.Y);
		Hash = HashFloat(Hash, Packed.Z);
		Hash = HashFloat(Hash, Packed.W);
	}

	// 房屋外皮：量化到厘米（与登记时的 0.5 cm 容差同一量级）。
	Hash = HashCombine(Hash, ::GetTypeHash(BuildingFootprints.Num()));
	for (const TPair<TObjectKey<UObject>, TArray<FVector2D>>& Pair : BuildingFootprints)
	{
		Hash = HashCombine(Hash, ::GetTypeHash(Pair.Value.Num()));
		for (const FVector2D& V : Pair.Value)
		{
			Hash = HashCombine(Hash, ::GetTypeHash(FMath::RoundToInt(V.X)));
			Hash = HashCombine(Hash, ::GetTypeHash(FMath::RoundToInt(V.Y)));
		}
	}
	return Hash == 0 ? 1u : Hash;
}

bool ACSGroundActor::EnsureBushComponents(const TArray<UStaticMesh*>& Meshes, const TArray<UMaterialInterface*>& Materials)
{
	// 关掉：连组件带显存一起收掉（同地被）。
	if (Meshes.IsEmpty())
	{
		for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : BushComponents) if (IsValid(Component)) Component->DestroyComponent();
		BushComponents.Reset();
		for (CSGroundCover::FCoverBuffers& One : BushBuffers) CSGroundCover::ReleaseOnRenderThread(One);
		BushBuffers.Reset();
		BushMeshesBuiltFrom.Reset();
		BushBaseSphereCentres.Reset();
		BushBaseSphereRadii.Reset();
		BushHandover.Reset();
		return false;
	}

	// 层数或某一层的网格变了 ⇒ 组件与层按下标对齐的前提没了，整批重建（同地被那条）。
	bool bLayoutChanged = BushComponents.Num() != Meshes.Num() || BushMeshesBuiltFrom.Num() != Meshes.Num();
	for (int32 Index = 0; !bLayoutChanged && Index < Meshes.Num(); ++Index)
	{
		if (BushMeshesBuiltFrom[Index] != Meshes[Index] || !IsValid(BushComponents[Index])) bLayoutChanged = true;
	}
	if (bLayoutChanged)
	{
		for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : BushComponents) if (IsValid(Component)) Component->DestroyComponent();
		BushComponents.Reset();
		BushComponents.SetNum(Meshes.Num());
		BushMeshesBuiltFrom.Reset();
		BushMeshesBuiltFrom.SetNum(Meshes.Num());
		for (CSGroundCover::FCoverBuffers& One : BushBuffers) CSGroundCover::ReleaseOnRenderThread(One);
		BushBuffers.Reset();
		// 新组件身上没有实例源：交接缓存必须一起作废，否则"缓存说交接过了"而画面永远是空的。
		BushHandover.Reset();
		BushHandover.Capacities.SetNumZeroed(Meshes.Num());
	}

	BushBaseSphereCentres.SetNum(Meshes.Num());
	BushBaseSphereRadii.SetNum(Meshes.Num());
	BushBuffers.SetNum(Meshes.Num());
	BushHandover.Capacities.SetNum(Meshes.Num());

	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		if (!IsValid(BushComponents[Index]))
		{
			BushComponents[Index] = NewObject<UCSGpuInstancedMeshComponent>(this, NAME_None, RF_Transient);
			BushComponents[Index]->SetupAttachment(RootComponent);
			BushComponents[Index]->RegisterComponent();   // 未注册时任何变更都会释放 GPU 网格
		}
		// 覆盖材质留空 = 用资产自带材质（组件逐 section 取）。资产开了 Nanite 时组件自动交给 Nanite 画。
		BushComponents[Index]->InstanceMaterial = Materials[Index];
		BushComponents[Index]->SetBaseMesh(Meshes[Index]);
		BushComponents[Index]->SetCastShadow(BuildingBushes.bCastShadow);
		BushMeshesBuiltFrom[Index] = Meshes[Index];

		const FBox Local = Meshes[Index]->GetBoundingBox();
		BushBaseSphereCentres[Index] = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
		BushBaseSphereRadii[Index] = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;
	}
	return true;
}

void ACSGroundActor::RebuildBuildingBushes()
{
	if (IsTemplate() || !GetWorld()) return;

	// 登记过的房子被 GC 掉却没来得及注销（崩溃恢复、强删）：它的外皮不该继续长灌木。
	for (auto It = BuildingFootprints.CreateIterator(); It; ++It)
	{
		if (!It.Key().ResolveObjectPtr()) It.RemoveCurrent();
	}

	TArray<UStaticMesh*> Meshes;
	TArray<UMaterialInterface*> Materials;
	TArray<float> OverlayChance;
	CollectBushLayers(Meshes, Materials, OverlayChance);
	if (Meshes.IsEmpty())
	{
		EnsureBushComponents(Meshes, Materials);
		BushBuiltHash = 0;
		return;
	}
	if (!Mirror.IsInitialized() || !TinyGladeMesh) return;
	const FCSMeshResidentRef Resident = TinyGladeMesh->GetResident();
	if (!Resident.IsValid()) return;   // 地面网格还没分配：重建完会再走一次这条路

	// 幂等短路，排在任何昂贵计算之前。
	const uint32 Hash = BushInputHash(Meshes, Materials);
	if (Hash == BushBuiltHash && !BushComponents.IsEmpty()) return;
	if (!EnsureBushComponents(Meshes, Materials)) return;

	const FVector Origin = GetActorLocation();
	const FBox2D Rect = GetWorldRect2D();
	const FCSGroundBuildingBushes& B = BuildingBushes;

	// 候选格铺满整块地面：房子在哪儿都长得出来，而格子本身不随房子变（容量与包围盒因此也不变，
	// 拖房子不会触发阻塞的交接）。格数不设退让上限 —— 512 m 地面、0.25 株/m² 也才 6.5 万格。
	CSGroundCover::FScatterParams Base;
	CSGroundCover::MakeGridForDensity(Rect, B.DensityPerSqM, 1 << 24, Base.GridOriginXY, Base.CellSize, Base.GridDims);
	// 容量按"能长多少"定，不按格数：只有房子周围那一圈长得出来。越界的静默丢弃。
	const uint32 Capacity = uint32(FMath::Clamp(FMath::Min(Base.GridDims.X * Base.GridDims.Y, B.MaxInstances), 64, 1048576));

	Base.WorldToComponent = FMatrix44f(GetActorTransform().ToInverseMatrixWithScale());
	Base.GroundOriginXY = FVector2f(float(Origin.X), float(Origin.Y));
	Base.GroundCellSize = Mirror.CellSize;
	Base.GroundVerts = FIntPoint(Mirror.NumVertsX, Mirror.NumVertsY);
	Base.GroundBaseZ = float(Origin.Z);
	Base.MaskChannel = FVector4f(1.0f, 0.0f, 0.0f, 0.0f);   // R = 道路权重
	Base.MaskStart = B.RoadMaskStart;
	Base.MaskEnd = B.RoadMaskEnd;
	Base.MaskShorten = 0.0f;                 // 路边不压矮：灌木在路上直接不长
	Base.bUseGroundMask = GroundMask != nullptr;   // 同地被：地面遮罩上露土的地方不长
	Base.MaskWarpStrength = GroundMaskWarpStrength;
	Base.MaskWarpWavelength = GroundMaskWarpWavelength;
	Base.Chance = 1.0f;
	Base.Jitter = 1.0f;                      // TG：格心 ± 1 m（格距 2 m）
	Base.MinSlopeCos = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(B.MaxSlopeDegrees, 0.0f, 89.0f)));
	Base.Sink = FMath::Max(B.Sink, 0.0f);
	Base.HeightOffset = B.HeightOffset;
	Base.ScaleRange = FVector2f(float(B.ScaleRange.X), float(B.ScaleRange.Y));
	Base.HeightJitter = B.HeightJitter;
	// 灌木不倾倒、不贴坡、不成簇：TG 只绕竖轴转一个随机角（`3.14 × hash`）。
	Base.LeanMaxRad = 0.0f;
	Base.AlignToNormal = 0.0f;
	Base.ClumpRadialChance = 0.0f;
	Base.ClumpAlignment = 0.0f;
	Base.ScaleClumpShare = 0.0f;
	Base.BendRange = FVector2f(0.0f, 0.0f);
	Base.Seed = uint32(GroundCoverSeed);
	// **本体与花共用同一个盐**（不像地被那样把下标混进去）：花必须落在本体同一批格子、同一个变换上。
	Base.Salt = uint32(B.Salt) * 2654435761u;
	Base.RingInner = B.RingInner;
	Base.RingWidth = FMath::Max(B.RingWidth, 1.0f);
	Base.RingNoiseCell = B.NoiseCellSize;
	Base.RingNoiseThreshold = B.NoiseThreshold;
	Base.RingMinScale = B.MinRingScale;

	TArray<CSGroundCover::FScatterParams> AllParams;
	double WorstReach = 0.0;
	for (int32 Index = 0; Index < Meshes.Num(); ++Index)
	{
		if (!CSGroundCover::EnsureBuffers(BushBuffers[Index], Capacity)) return;
		CSGroundCover::FScatterParams P = Base;
		P.OverlayChance = OverlayChance[Index];
		P.BaseSphereCentre = BushBaseSphereCentres[Index];
		P.BaseSphereRadius = BushBaseSphereRadii[Index];
		const float WorstScale = P.ScaleRange.Y * (1.0f + P.HeightJitter);
		WorstReach = FMath::Max(WorstReach, double(P.BaseSphereRadius) * double(WorstScale) + double(P.Sink) + double(FMath::Abs(P.HeightOffset)));
		AllParams.Add(P);
	}

	TArray<TArray<FVector2D>> Polygons;
	Polygons.Reserve(BuildingFootprints.Num());
	for (const TPair<TObjectKey<UObject>, TArray<FVector2D>>& Pair : BuildingFootprints) Polygons.Add(Pair.Value);
	TArray<FVector4f> PackedBuildings;
	CSGroundCover::PackBuildingPolygons(Polygons, PackedBuildings);

	TArray<FVector4f> ShaperParams;
	BuildShaperGpuParams(ShaperParams);
	if (!CSGroundCover::Scatter(Resident, BushBuffers, AllParams, ShaperParams, PackedBuildings, GroundMask)) return;

	// 包围盒按"地面矩形 × MaxAbsHeight"由配置算死（同地被）：灌木跟着房子到处跑，盒子却不动，
	// 拖房子因此不会触发阻塞的 SetInstanceSourceGPU。
	const double HeightReach = CSShaperSteps::QuantizeUp(double(MaxAbsHeight));
	const FVector LocalMin = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Min.X, Rect.Min.Y, Origin.Z - HeightReach)) - FVector(WorstReach);
	const FVector LocalMax = GetActorTransform().InverseTransformPosition(
		FVector(Rect.Max.X, Rect.Max.Y, Origin.Z + HeightReach)) + FVector(WorstReach);
	const FBox LocalBounds(LocalMin.ComponentMin(LocalMax), LocalMin.ComponentMax(LocalMax));

	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<2>> Sources;
	for (int32 Index = 0; Index < BushBuffers.Num(); ++Index)
	{
		CSShaperSteps::FHandoverSource One;
		One.Component = BushComponents.IsValidIndex(Index) ? ToRawPtr(BushComponents[Index]) : nullptr;
		One.PackedInstances = BushBuffers[Index].PackedInstances;
		One.Counter = BushBuffers[Index].Counter;
		One.CustomData = BushBuffers[Index].CustomData;
		One.Capacity = BushBuffers[Index].Capacity;
		Sources.Add(MoveTemp(One));
	}
	CSShaperSteps::FHandoverOptions Options;
	Options.Mode = CSShaperSteps::EHandoverMode::PerPalette;
	Options.LogLabel = TEXT("灌木");
	// 同地被：BushBuffers 只有上面那一处 Scatter 在写，写完必到这里。
	Options.bNotifiesChanges = true;
	CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, BushHandover, Options, this);
	BushBuiltHash = Hash;
}

int32 ACSGroundActor::DebugReadBuildingBushOriginsSync(int32 Layer, TArray<FVector>& OutWorldOrigins) const
{
	OutWorldOrigins.Reset();
	if (!BushBuffers.IsValidIndex(Layer) || !BushBuffers[Layer].IsValid()) return -1;
	const int32 Count = CSGroundCover::DebugReadInstancesSync(BushBuffers[Layer], &OutWorldOrigins, nullptr);
	const FTransform Transform = GetActorTransform();
	for (FVector& One : OutWorldOrigins) One = Transform.TransformPosition(One);
	return Count;
}

bool ACSGroundActor::IsGroundCoverDrawable(FString& OutReason) const
{
	OutReason = GetGroundCoverUndrawableReason();
	return OutReason.IsEmpty();
}

FString ACSGroundActor::GetGroundCoverUndrawableReason() const
{
	if (!bGroundCoverEnabled) return TEXT("bGroundCoverEnabled = false（整条地被关掉了）");
	if (!Grass.Mesh && Flowers.FindByPredicate([](const FCSGroundCoverSpecies& S) { return S.Mesh != nullptr; }) == nullptr)
	{
		return TEXT("一个物种都没有网格（Grass.Mesh 与 Flowers 全空）");
	}
	if (!Mirror.IsInitialized()) return TEXT("镜像还没初始化（先点 RebuildGroundMesh）");
	if (CoverComponents.IsEmpty()) return TEXT("还没建过组件（先点 RebuildGroundCover）");

	for (int32 Index = 0; Index < CoverComponents.Num(); ++Index)
	{
		const UCSGpuInstancedMeshComponent* Component = CoverComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("物种 %d：组件失效"), Index);
		if (!Component->HasInstanceSourceGPU()) return FString::Printf(TEXT("物种 %d：没有实例源"), Index);

		const UMaterialInterface* Material = Component->InstanceMaterial;
		if (!Material) return FString::Printf(TEXT("物种 %d：没有绑材质（会用引擎默认表面材质画成一片灰）"), Index);
		// ⚠️ 没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上会被引擎**静默替换**成
		// 默认材质，症状与"没绑材质"逐像素相同（同裙边摆件那条）。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !Base->bUsedWithInstancedStaticMeshes)
		{
			return FString::Printf(
				TEXT("物种 %d：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
				Index, *Material->GetName());
		}
	}
	return FString();
}

int32 ACSGroundActor::DebugReadGroundCoverCountGpuSync(int32 SpeciesIndex) const
{
	// −1 = 那个物种没有缓冲，与"真的是 0 株"分开：把读不到当 0 会让守着"关掉之后必须归零"
	// 的断言在管线坏掉时假绿（同 DebugReadStairCountGpuSync 的口径）。
	if (!CoverBuffers.IsValidIndex(SpeciesIndex)) return -1;
	return CSGroundCover::DebugReadInstancesSync(CoverBuffers[SpeciesIndex], nullptr, nullptr);
}

int32 ACSGroundActor::DebugReadGroundCoverOriginsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins) const
{
	OutWorldOrigins.Reset();
	if (!CoverBuffers.IsValidIndex(SpeciesIndex)) return -1;
	const int32 Count = CSGroundCover::DebugReadInstancesSync(CoverBuffers[SpeciesIndex], &OutWorldOrigins, nullptr);
	// kernel 写的是**组件空间**的原点（packed 行按组件空间存），带出来的要是世界坐标才好断言。
	const FTransform Transform = GetActorTransform();
	for (FVector& One : OutWorldOrigins) One = Transform.TransformPosition(One);
	return Count;
}

int32 ACSGroundActor::DebugReadGroundCoverHeightsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins,
	TArray<float>& OutHeightScale) const
{
	OutWorldOrigins.Reset();
	OutHeightScale.Reset();
	if (!CoverBuffers.IsValidIndex(SpeciesIndex)) return -1;

	TArray<FVector4f> Rows;
	const int32 Count = CSGroundCover::DebugReadInstancesSync(CoverBuffers[SpeciesIndex], nullptr, &Rows);
	const FTransform Transform = GetActorTransform();
	OutWorldOrigins.Reserve(Count);
	OutHeightScale.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Base = Index * CS_GPU_INSTANCED_ROW_FLOAT4S;
		if (!Rows.IsValidIndex(Base + 3)) break;
		OutWorldOrigins.Add(Transform.TransformPosition(FVector(Rows[Base + 3].X, Rows[Base + 3].Y, Rows[Base + 3].Z)));
		// 第 3 行 = LocalZ = 组件空间的株轴 × HeightScale（含遮罩压矮）；地面只允许平移 ⇒ 长度就是 HeightScale。
		OutHeightScale.Add(FVector3f(Rows[Base + 2].X, Rows[Base + 2].Y, Rows[Base + 2].Z).Size());
	}
	return Count;
}

int32 ACSGroundActor::DebugReadGroundCoverFacingsSync(int32 SpeciesIndex, TArray<FVector>& OutWorldOrigins,
	TArray<FVector2D>& OutFacingXY) const
{
	OutWorldOrigins.Reset();
	OutFacingXY.Reset();
	if (!CoverBuffers.IsValidIndex(SpeciesIndex)) return -1;

	TArray<FVector4f> Rows;
	const int32 Count = CSGroundCover::DebugReadInstancesSync(CoverBuffers[SpeciesIndex], nullptr, &Rows);
	const FTransform Transform = GetActorTransform();
	OutWorldOrigins.Reserve(Count);
	OutFacingXY.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 Base = Index * CS_GPU_INSTANCED_ROW_FLOAT4S;
		if (!Rows.IsValidIndex(Base + 3)) break;
		// 第 4 行 = 原点（组件空间）；第 2 行 = LocalY = 朝向轴（已被均匀缩放乘过）。
		OutWorldOrigins.Add(Transform.TransformPosition(FVector(Rows[Base + 3].X, Rows[Base + 3].Y, Rows[Base + 3].Z)));
		// 只取水平分量：倾倒会把 Z 掰出来，而"朝向"这件事说的就是水平那一半。
		const FVector2D Flat(Rows[Base + 1].X, Rows[Base + 1].Y);
		const double Len = Flat.Size();
		OutFacingXY.Add(Len > UE_KINDA_SMALL_NUMBER ? (Flat / Len) : FVector2D::ZeroVector);
	}
	return Count;
}

int32 ACSGroundActor::DebugReadGroundCoverCustomDataSync(int32 SpeciesIndex, TArray<float>& OutBendAmp,
	TArray<float>& OutReserved, TArray<FVector>& OutWorldOrigins) const
{
	OutBendAmp.Reset();
	OutReserved.Reset();
	OutWorldOrigins.Reset();
	if (!CoverBuffers.IsValidIndex(SpeciesIndex)) return -1;

	TArray<FVector4f> Rows;
	TArray<float> Custom;
	const int32 Count = CSGroundCover::DebugReadInstancesSync(CoverBuffers[SpeciesIndex], nullptr, &Rows, &Custom);
	const FTransform Transform = GetActorTransform();
	OutBendAmp.Reserve(Count);
	OutReserved.Reserve(Count);
	OutWorldOrigins.Reserve(Count);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const int32 CBase = Index * CS_GPU_INSTANCED_CUSTOM_DATA_FLOATS;
		const int32 RBase = Index * CS_GPU_INSTANCED_ROW_FLOAT4S + 3;   // 第 4 行 = 原点（组件空间）+ 每实例随机数
		if (!Custom.IsValidIndex(CBase + 1) || !Rows.IsValidIndex(RBase)) break;
		OutBendAmp.Add(Custom[CBase + 0]);
		OutReserved.Add(Custom[CBase + 1]);
		OutWorldOrigins.Add(Transform.TransformPosition(FVector(Rows[RBase].X, Rows[RBase].Y, Rows[RBase].Z)));
	}
	return Count;
}

bool ACSGroundActor::DebugBakeSkirtDecorSync(const FString& AssetPath, int32& OutTriangles,
	int32& OutVertexInstances, int32& OutUVChannels, int32& OutDistinctUVs, int32& OutDistinctBakedRandoms,
	int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu)
{
	OutTriangles = 0;
	OutVertexInstances = 0;
	OutUVChannels = 0;
	OutDistinctUVs = 0;
	OutDistinctBakedRandoms = 0;
	OutGpuInstanceCount = 0;
	bOutRandomsMatchGpu = false;
	if (SkirtDecorComponents.IsEmpty() || !IsValid(SkirtDecorComponents[0])) return false;
	UCSGpuInstancedMeshComponent* Component = SkirtDecorComponents[0];

	// 先把 GPU 那一侧的真值取到手，再烘 —— 顺序反过来的话，烘焙本身若不小心动了实例源，
	// 后读到的就是被自己改过的值，断言等于自证（同 DebugBakeFrameBricksSync）。
	TArray<float> GpuRandoms;
	if (!Component->DebugReadInstanceRandomsSync(GpuRandoms)) return false;
	OutGpuInstanceCount = GpuRandoms.Num();

	// 走组件自己的出口，不另拼一条：要证的正是"**那条**出口带不带得走通道"。
	// BakeSpace 取本 actor 的变换（同石阶 / 岩壳 / 道路那三条出口的口径）。
	UStaticMesh* Baked = Component->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
	if (!Baked) return false;

	const FMeshDescription* Description = Baked->GetMeshDescription(0);
	if (!Description) return false;

	FStaticMeshConstAttributes Attributes(*Description);
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	TVertexInstanceAttributesConstRef<FVector2f> UVs = Attributes.GetVertexInstanceUVs();
	if (!Colors.IsValid() || !UVs.IsValid()) return false;

	OutTriangles = Description->Triangles().Num();
	OutVertexInstances = Description->VertexInstances().Num();
	OutUVChannels = UVs.GetNumChannels();

	// 8 位量化后去重：顶点色最终就存成 FColor，比"浮点相等"更贴近资产里真正留下的东西。
	auto Quantize = [](float Value) { return int32(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f)); };
	TSet<int32> BakedRandoms;
	// UV 是不是退化的：全 (0,0) 一样不报错，症状只是烘焙件的贴图变成一整片同一个像素。
	// 量化到 1/1024 再去重 —— 判的是"有没有内容"，不是"精度对不对"。
	TSet<int32> BakedUVs;
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		BakedRandoms.Add(Quantize(Colors[InstanceID].W));
		const FVector2f UV = UVs.Get(InstanceID, 0);
		BakedUVs.Add(int32(FMath::RoundToInt(UV.X * 1024.0f)) * 8191 + int32(FMath::RoundToInt(UV.Y * 1024.0f)));
	}
	OutDistinctBakedRandoms = BakedRandoms.Num();
	OutDistinctUVs = BakedUVs.Num();

	// 逐个对：GPU 上每一行的 `Origin.w` 都必须在烘焙件的 alpha 集合里找得到。
	// 反向不查 —— 同一个 8 位桶可能被两个实例共用，那不是缺陷；
	// "GPU 有而烘焙件没有"才是通道被丢掉的证据。
	bOutRandomsMatchGpu = GpuRandoms.Num() > 0;
	for (const float Random : GpuRandoms)
	{
		const int32 Bucket = Quantize(Random);
		if (!BakedRandoms.Contains(Bucket) && !BakedRandoms.Contains(Bucket - 1) && !BakedRandoms.Contains(Bucket + 1))
		{
			bOutRandomsMatchGpu = false;
			break;
		}
	}
	return true;
}

bool ACSGroundActor::DebugBakeRockShellCapSplitSync(const FString& AssetPath, int32& OutCapCorners,
	int32& OutSkirtCorners, int32& OutTriangles)
{
	OutCapCorners = 0;
	OutSkirtCorners = 0;
	OutTriangles = 0;
	if (!IsValid(RockShellComponent) || !RockShellMesh) return false;

	// 走组件自己的出口，不另拼一条：这里要证的正是"**那条**出口带不带得走顶点色"，
	// 自己拼一条等价路径只会证明另一条路能行。BakeSpace 取本 actor 的变换 ——
	// 壳的常驻流是世界空间的，烘回局部空间才能摆回原处（同 RoadMeshSaveTests 的口径）。
	UStaticMesh* Baked = RockShellComponent->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
	if (!Baked) return false;

	const FMeshDescription* Description = Baked->GetMeshDescription(0);
	if (!Description) return false;

	FStaticMeshConstAttributes Attributes(*Description);
	TVertexInstanceAttributesConstRef<FVector4f> Colors = Attributes.GetVertexInstanceColors();
	if (!Colors.IsValid()) return false;

	OutTriangles = Description->Triangles().Num();
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		((Colors[InstanceID].X > 0.5f) ? OutCapCorners : OutSkirtCorners)++;
	}
	return true;
}

#endif // WITH_EDITOR

#if WITH_EDITOR
void ACSGroundActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 形状属性 = 决定顶点位置或索引的量（计划 D14 的哈希纪律，这里是它的属性面板版本）。
	// GroundMaterial 与 BaseColor 都不在其列：前者只重绑材质，后者压根不参与快照（权威是
	// Mirror.Colors，要铺新底色请点 ResetPaint）。早先把两者算进来的代价是一次纯空转的
	// 全量重建 + 全场唤醒。
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	UE_LOG(LogTinyGladeGround, Verbose, TEXT("[TinyGladeGround] %s PostEditChangeProperty prop=%s member=%s"),
		*GetName(), *PropertyName.ToString(),
		PropertyChangedEvent.MemberProperty ? *PropertyChangedEvent.MemberProperty->GetName() : TEXT("<null>"));
	const bool bShapeProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsX) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, NumCellsY) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, CellSize) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, UVWorldPeriod);
	if (bShapeProperty) RebuildGroundMesh();   // 内部末尾会重扫石阶
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(ACSGroundActor, GroundMaterial)) BindGroundMaterial();
	// 其余属性一律重扫石阶 + 重披挂岩壳：两条链各自就是一次 dispatch，为了省它去维护一张
	// "哪些属性算石阶/岩壳属性"的名单，收益远小于名单漏一条时"改了参数没反应"的排查成本。
	// 岩壳自己的哈希会把真正没变的那些吃掉。地面遮罩排最前（地被 / 灌木读它），同样靠自己的哈希短路；
	// 材质参数跟着重喂一次（草的 MaskStart / MaskEnd 决定土铺到哪，没变时 MID 自己早退）。
	else { RebuildGroundMask(); BindGroundMaterial(); RebuildStairs(); RebuildRockShell(); RebuildSkirtDecor(); RebuildGroundCover(); RebuildBuildingBushes(); }
}

void ACSGroundActor::PostEditUndo()
{
	Super::PostEditUndo();

	// Mirror 是 NonTransactional，所以撤销永远不会动它 —— 但被撤销的可能是格数/格距这类
	// 形状属性，镜像与配置就此错配。EnsureMirrorInitialized 检出错配后重置，再全量对齐。
	// 放在这里是因为 AActor::PostEditUndo 既不调 PostEditMove、也不保证跑构造脚本。
	EnsureMirrorInitialized();
	RebuildGroundMesh();
}

void ACSGroundActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	if (!TinyGladeMesh)
	{
		RebuildGroundMesh();
		return;
	}

	// 常驻流是世界空间：拖动期间增量平移（便宜、无重建卡顿），松手后全量重建对齐，
	// 顺带把拖动过程中累计的浮点误差清零。
	const FVector Delta = GetActorLocation() - MeshBuiltAtLocation;
	if (bFinished)
	{
		if (!Delta.IsNearlyZero()) RebuildGroundMesh();
		return;
	}
	if (Delta.IsNearlyZero()) return;
	UCSMeshOps::TranslateMesh(TinyGladeMesh, Delta);
	MeshBuiltAtLocation = GetActorLocation();
	// 地面挪了，压在上面的查询结果全变——拖动中也逐帧直推（未提交；松手那次走 RebuildGroundMesh = 提交）。
	bLastChangeCommitted = false;
	OnGroundChanged.Broadcast(this, ComputeGroundWorldBox());
}
#endif

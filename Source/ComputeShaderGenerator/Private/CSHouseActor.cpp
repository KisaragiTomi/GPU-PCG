#include "CSHouseActor.h"

#include "CSGpuInstancedMeshComponent.h"
#include "CSStairsActor.h"
#include "CSStairsConnection.h"
#include "CSGpuMeshTypes.h"
#include "CSGroundActor.h"
#include "CSHouseQuoin.h"
#include "CSHouseResize.h"
#include "CSHouseHeightHandleActor.h"   // D5 高度框（unity 构建下别指望别人替你带进来）
#include "CSHouseResizeHandleActor.h"   // D5 水平锥子（同上）
#include "CSHouseRidgeHandleActor.h"    // 脊长箭头（同上）
#include "CSHouseFeatureMarker.h"       // D8 附属物：重建后要回推裁决 + 按锚点吸附
#include "CSHouseBrickWall.h"           // D4 两层之 A：砖层 = 一摞包边带
#include "CSHouseContact.h"   // D7 接触记录（unity 构建下别指望别人替你带进来）
#include "CSHouseSeam.h"   // IdLess —— 标记登记表的确定次序（unity 构建下别指望别人替你带进来）
#include "CSHouseRoof.h"
#include "CSHouseTrim.h"
#include "CSHouseLibrary.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "CSMesh.h"
#include "CSMeshOps.h"
#include "CSMeshRenderComponent.h"
#include "CSHousePillar.h"   // 砖石柱（unity 构建下别指望别人替你带进来）
#include "CSVineTube.h"   // 折线 → 管子的对外入口（unity 构建下别指望别人替你带进来）
#include "CSWall.h"       // 墙的纯函数核心：房子是一圈围合的墙（2026-09-22「统一房子和墙的逻辑」）
#include "CSWallMesh.h"   // FCSWallMeshWriter —— 房子与墙共用的三角形写手
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Templates/SharedPointer.h"
#if WITH_EDITOR
// 烘焙判据要从 GetMeshDescription(0) 里读顶点色与 UV 组数（DebugBakeFrameBricksSync）。
// unity 构建下这两个恰好被邻居 TU 带进来，漏写只有 -SingleFile 才照得出来（坑表里那条）。
#include "StaticMeshAttributes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogTinyGladeHouse, Log, All);

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSHouse_ 前缀。

constexpr float CSHouse_UVScale = CSWallMesh::UVScale;   // 世界 cm → UV 的平铺周期（与墙的写手同一个值）
constexpr float CSHouse_MinSpring = 15.0f;  // 拱脚竖直段最低高度

// 半圆拱的段数不再写死：按弦高容差自适应，见 CSHouse_ProfileSegments（CSHouseProfile.h）。

// 三角形写手与语义色（通道字典）2026-09-22 起住在 `CSWallMesh.h`（`FCSWallMeshWriter` / `CSWallMesh::Semantic`），
// 房子与样条墙共用一份 —— 原来这里的 `FCSHouseMeshWriter` / `CSHouse_Semantic` 是逐字搬过去的，画面逐位不变。
static_assert(uint8(ECSHousePart::Wall) == CSWallMesh::PartWall, "墙的构件色号：CSWallMesh::PartWall 必须等于 ECSHousePart::Wall");

// `FCSHouseEdgeFrame` / `CSHouse_GetEdge` 已上提到 `CSHouseProfile.h`：谓词（`CSHouse_QueryOpening`）
// 与单测也要问"这面墙在哪、有多长"，而墙在哪只能有一个真源。

/** 墙空间 (沿边弧长 S, 高度 Z) → UV。内外脸、过梁带、窗台带共用同一套参数化。 */
FVector2f CSHouse_WallUV(float SAlong, float ZUp)
{
	return FVector2f(SAlong / CSHouse_UVScale, ZUp / CSHouse_UVScale);
}


uint32 CSHouse_Hash(const TArray<int32>& Values)
{
	return Values.Num() ? FCrc::MemCrc32(Values.GetData(), Values.Num() * sizeof(int32)) : 0;
}

int32 CSHouse_Q(double Value, double Quantum) { return int32(FMath::RoundToInt(Value / Quantum)); }

/**
 * footprint 进哈希：顶点数 + 逐顶点量化（口径与理由见 `FCSHouseFootprint::AppendQuantizedHash`）。
 *
 * 原来各家哈希写的是 `CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1)` —— 两个数只能
 * 描述居中矩形。⚠️ 2026-09-14 起顶点量子从 1 cm 改成 0.5 cm：矩形上 1 cm 的顶点量子等于 2 cm 的尺寸量子，
 * 比折线化之前粗了一倍。量化只留一份，单测直接钉那个成员函数。
 */
void CSHouse_AppendFootprintHash(TArray<int32>& H, const FCSHouseFootprint& Footprint)
{
	Footprint.AppendQuantizedHash(H);
}

/**
 * 屋脊三件套进哈希（2026-10-02）：脊长参数、脊向、山墙样式。吃屋面形状的每一家（房体的山墙、瓦、尖顶）都拼它 ——
 * 漏掉的症状是"拖了脊长抓手，瓦重排了而山墙没升上去"这种半截更新。
 * 脊长量到 0.1%：10 m 的房子上是 1 cm，与 footprint 的量子同档。自动（< 0）恒量成同一个负数。
 * ⚠️ 「恰好到 1」单占一位：0.9996 与 1 量化后同值，而前者是四坡、后者是山墙 —— 不分开的话最后那一小步不重建。
 */
void CSHouse_AppendRoofRidgeHash(TArray<int32>& H, float RidgeLength01, ECSRoofRidgeAxis Axis, ECSGableRoofStyle Style)
{
	H.Append({ RidgeLength01 < 0.0f ? -1 : CSHouse_Q(RidgeLength01, 0.001), int32(RidgeLength01 >= 1.0f), int32(Axis), int32(Style) });
}

// 藤蔓管子的环半径系数 2026-09-22 起是 `CSHouseVine::TubeCircleScale`（房子与样条墙共用，理由见那里）。

/**
 * 砖路**墙框架**的哈希，量在 `CSHouseFrame::Scatter` 真正写进去的那个空间（组件空间）里。
 *
 * ⚠️ **少了这一条，拉尺寸时门框砖整段不重排**（2026-08-31 实测定位，疑案 A 的几何那一半）：
 * `RebuildFrame` 的早退门原先只看 `FElement::Path` + 砖参数，而 `FPath` 全是**边局部**量
 * （`CenterS` / `LeftS` / `RightS` / `TotalLen` / `Radius` / `BaseZ` / `TopZ`）。
 * 把 Y 从 400 拉到 800 时，**X 边的长度一个字都没变** ⇒ 那条边上的洞的边局部量逐位相同
 * ⇒ 哈希不变 ⇒ 一次 `Scatter` 都不录；而那面墙本身已经沿 ±Y 走了 200 cm，
 * 砖于是留在上一代的墙面位置上。症状极具误导性：砖数（`GetFrameBrickCount` / GPU 回读）、
 * 三角形数、洞数、零阻塞四条断言**全部照绿**，只有像素能看见 —— 拱圈上一块砖都没有，
 * 而上一代的砖在屋里悬着。纯平移抓不到它（框架与组件一起走，组件空间里逐位不变），
 * 所以已有的"平移零阻塞"三条断言天生测不到这条路。
 *
 * 量在组件空间而不是世界空间，是为了保住"纯平移不重排"这条性质：世界坐标会让每一次
 * 平移都判成"变了"，白录一趟 pass（不阻塞，但也没有必要）。
 */
uint32 CSHouse_HashElementFrames(const TArray<CSHouseFrame::FElement>& Elements, const FMatrix44f& WorldToComponent)
{
	TArray<int32> H;
	H.Reserve(Elements.Num() * 9);
	for (const CSHouseFrame::FElement& E : Elements)
	{
		const FVector3f O(WorldToComponent.TransformPosition(E.Frame.Origin));
		const FVector3f U(WorldToComponent.TransformVector(E.Frame.AxisU));
		const FVector3f N(WorldToComponent.TransformVector(E.Frame.AxisN));
		H.Append({ CSHouse_Q(O.X, 1), CSHouse_Q(O.Y, 1), CSHouse_Q(O.Z, 1),
			CSHouse_Q(U.X, 0.01), CSHouse_Q(U.Y, 0.01), CSHouse_Q(U.Z, 0.01),
			CSHouse_Q(N.X, 0.01), CSHouse_Q(N.Y, 0.01), CSHouse_Q(N.Z, 0.01) });
	}
	return CSHouse_Hash(H);
}

/**
 * openings 表的全序：(边, 沿边位置, 身份)。
 *
 * **末位那个 `SourceId` 不是装饰**：`TArray::Sort` 不稳定，同一 (边, CenterS) 上的两个洞
 * 没有全序时两次重求值可以给出不同的顺序 ⇒ 形状哈希抖动 ⇒ 幂等短路失效。门的 SourceId
 * 恒为全零（子段本来就不会同位），窗的从列表槽位派生 —— 两边都是确定的。
 */
bool CSHouse_OpeningLess(const FCSWallOpening& A, const FCSWallOpening& B)
{
	if (A.EdgeIndex != B.EdgeIndex) return A.EdgeIndex < B.EdgeIndex;
	if (A.CenterS != B.CenterS) return A.CenterS < B.CenterS;
	if (A.SourceId.A != B.SourceId.A) return A.SourceId.A < B.SourceId.A;
	if (A.SourceId.B != B.SourceId.B) return A.SourceId.B < B.SourceId.B;
	if (A.SourceId.C != B.SourceId.C) return A.SourceId.C < B.SourceId.C;
	return A.SourceId.D < B.SourceId.D;
}

// --- 拖尺寸期的稳态化：把"随 FootprintSize 连续变化的量"吸成阶梯 -------------------------
//
// ⚠️ **实现已搬到 `CSShaperSteps::QuantizeUp` / `CSShaperSteps::ReserveCount`**（2026-08-31）：
// 地面那一侧的裙边摆件走的是同一条纪律，留在这个匿名命名空间里就必然被抄成第二份，
// 而两份常数分叉的症状是"房子那边零阻塞、地面这边每帧一次"——两边的断言各自都绿。
// 起因、可陈述的保证、三个常数的依据全部逐字搬过去了，本文件改成直接按全名调。
}

ACSHouseActor::ACSHouseActor()
{
	FlatRoofParapetComponent = CreateDefaultSubobject<UCSGpuInstancedMeshComponent>(TEXT("FlatRoofParapet"));
	FlatRoofParapetComponent->SetupAttachment(RootComponent);
	// 合批唤醒的兑现点（见 `RequestReevaluate`）。基类默认 `bCanEverTick = false`，这里自己打开；
	// 平时关着、有欠账才开。编辑器里要真的 tick，还得配 `ShouldTickIfViewportsOnly`（同特征标记）。
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;

	PillarMeshComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("PillarMesh"));
	PillarMeshComponent->SetupAttachment(RootComponent);

	// 藤蔓管子。几何在世界空间 ⇒ 相对变换必须钉成恒等（组件的变换已被构造函数标成绝对，
	// 所以"相对"在这里就是世界）。与 `CSVineTube::BuildTubeIntoMesh` 里的
	// `VineWorldToLocal = Identity` 是同一条约定的两半，必须一起动。
	VineTubeComponent = CreateDefaultSubobject<UCSMeshRenderComponent>(TEXT("VineTubeMesh"));
	VineTubeComponent->SetupAttachment(RootComponent);
	VineTubeComponent->SetRelativeTransform(FTransform::Identity);
}

// -----------------------------------------------------------------------------
// Ground wiring
// -----------------------------------------------------------------------------

void ACSHouseActor::ResolveGroundAndSubscribe()
{
	if (!Ground && GetWorld())
	{
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!Ground)
	{
		// 还没有地面：先挂在类级广播上等它出现。实例级订阅要先有地面指针，而地面晚于房子
		// 出现时没人会再来叫醒这栋房子 —— 它的藤会一直按"脚下无地面 = 悬空"秃着。
		if (!AnyGroundRebuiltHandle.IsValid()) AnyGroundRebuiltHandle = ACSGroundActor::OnAnyGroundRebuilt.AddUObject(this, &ACSHouseActor::HandleAnyGroundRebuilt);
		return;
	}
	UnsubscribeAnyGroundRebuilt();   // 找到了，换成下面的实例级订阅
	if (GroundChangedHandle.IsValid()) return;   // 已订阅（换地面场景先 Unsubscribe 再来）
	GroundChangedHandle = Ground->OnGroundChanged.AddUObject(this, &ACSHouseActor::HandleGroundChanged);
}

void ACSHouseActor::UnsubscribeGround()
{
	if (IsValid(Ground) && GroundChangedHandle.IsValid()) Ground->OnGroundChanged.Remove(GroundChangedHandle);
	GroundChangedHandle.Reset();
	UnsubscribeAnyGroundRebuilt();
	// 房子不在了（卸载 / PIE 结束 / 销毁）：它周围的灌木跟着消失。
	if (IsValid(Ground)) Ground->RemoveBuildingFootprint(this);
}

void ACSHouseActor::PublishFootprintToGround()
{
	if (!IsValid(Ground)) return;
	// GetFootprint 已经是严格凸、逆时针的外皮（凹的按凸包用，同房体）。
	const FCSHouseFootprint Footprint = GetFootprint();
	if (!Footprint.IsValidFootprint())
	{
		Ground->RemoveBuildingFootprint(this);
		return;
	}
	const FTransform World = GetBuildTransform();
	TArray<FVector2D, TInlineAllocator<16>> WorldPolygon;
	for (const FVector2D& V : Footprint.Verts) WorldPolygon.Add(FVector2D(World.TransformPosition(FVector(V, 0.0))));
	Ground->SetBuildingFootprint(this, WorldPolygon);
}

void ACSHouseActor::UnsubscribeAnyGroundRebuilt()
{
	if (AnyGroundRebuiltHandle.IsValid()) ACSGroundActor::OnAnyGroundRebuilt.Remove(AnyGroundRebuiltHandle);
	AnyGroundRebuiltHandle.Reset();
}

void ACSHouseActor::HandleAnyGroundRebuilt(ACSGroundActor* RebuiltGround, const FBox& /*ChangedBounds*/)
{
	// 类级广播：编辑器世界与 PIE 世界的地面都会来，只认自己世界的。
	if (!IsValid(RebuiltGround) || RebuiltGround->GetWorld() != GetWorld()) return;
	// 只标脏：解析地面、换订阅都在下一次 ReevaluateSite 里做，不在别人的广播途中改委托表。
	RequestReevaluate();
}

void ACSHouseActor::HandleGroundChanged(ACSGroundActor* ChangedGround, const FBox& /*ChangedBounds*/)
{
	// v1 直推：不过滤变更盒，无条件重求值——无效唤醒由哈希短路吸收（计划 D3）。
	// 走合批：一次落笔会让地面把这条广播给每一栋受影响的房子，而画笔是按住不放的
	// —— 同一栋房子一帧里收到多次是常态。
	// 接缝（D7 09-16）：塑形物拖动 / 落笔途中的广播是未提交的 —— 房子照常落座，但跨房的接缝写入
	// 等提交广播（松手 / 收笔 / 显式重建），否则拖一次塑形物就是每帧写邻居。
	bInGroundDrag = IsValid(ChangedGround) && !ChangedGround->IsLastChangeCommitted();
	RequestReevaluate();
}

// -----------------------------------------------------------------------------
// Reevaluate
// -----------------------------------------------------------------------------

double ACSHouseActor::ComputeSeatZ() const
{
	const FVector Loc = GetActorLocation();
	if (!Ground) return Loc.Z;

	// 落座规则（计划 D4）：max 取 footprint 全域——正中冒起的塑形物也要能顶起房子；
	// 绝对式而非增量式，隆起与塌陷天然对称。
	const float Yaw = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(Yaw), FMath::Sin(Yaw));
	const FVector2D AxY(-FMath::Sin(Yaw), FMath::Cos(Yaw));
	const FCSHouseFootprint Footprint = GetFootprint();
	const FBox2D Box = Footprint.GetBounds();
	if (!Box.bIsValid) return Loc.Z;
	const FVector2D Size = Box.GetSize();
	const double Step = 50.0;   // 与地面镜像默认格距同阶；max 采样对格距不敏感

	double MaxGround = TNumericLimits<double>::Lowest();
	auto SampleAt = [&](const FVector2D& L)
	{
		const FVector2D P = FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y;
		MaxGround = FMath::Max(MaxGround, double(Ground->SampleHeight(P)));
	};
	const int32 NX = FMath::Max(1, int32(FMath::CeilToInt(Size.X / Step)));
	const int32 NY = FMath::Max(1, int32(FMath::CeilToInt(Size.Y / Step)));
	for (int32 ix = 0; ix <= NX; ++ix)
	{
		for (int32 iy = 0; iy <= NY; ++iy)
		{
			const FVector2D L(Box.Min.X + Size.X * ix / NX, Box.Min.Y + Size.Y * iy / NY);
			// 包围盒网格只采落在折线内（含边界）的点：非矩形折线的包围盒角上是房外的空地，
			// 采了会把房子顶到那块地的高度上去。矩形的网格点全在内部或边上，一个都不会被跳过。
			if (Footprint.ContainsPoint(L, 0.5)) SampleAt(L);
		}
	}
	// 顶点单独补采：锐角顶点可能夹在两个网格点之间，漏掉它就漏掉角上那块凸起。
	// 矩形的四个角本来就是网格点，补采不改最大值。
	for (const FVector2D& V : Footprint.Verts) SampleAt(V);
	if (MaxGround <= TNumericLimits<double>::Lowest() * 0.5) return Loc.Z;
	return MaxGround + HeightOffset;
}

void ACSHouseActor::ReanchorMarkersToPreserveWorld()
{
	const FTransform Build = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();

	// **只在墙几何变了的时候做。** 纯平移 / 旋转不做 —— 标记 attach 在房子下，场景图已经带着
	// 它一起走了；这时再"守恒世界位置"等于把窗从房子上扯下来留在原地。
	const bool bGeomChanged = !MarkerRefFootprint.EqualsApprox(Footprint, 0.01)
		|| !FMath::IsNearlyEqual(MarkerRefThickness, WallThickness, 0.01f);

	if (bMarkerRefValid && bGeomChanged)
	{
		for (FCSMarkerWindow& Entry : MarkerWindows)
		{
			FCSWallAnchor A = Entry.Anchor;
			if (!A.IsValidAnchor()) continue;

			// ① 旧墙上那个点，取到世界里（旧 footprint + 旧构建变换）。
			const FCSHouseEdgeFrame OldF = CSHouse_GetEdge(A.EdgeIndex, MarkerRefFootprint, MarkerRefThickness);
			const float OldS = CSHouse_AnchorS(A, MarkerRefFootprint, MarkerRefThickness);
			const FVector2D OldP2 = OldF.Start + OldF.U * double(OldS);
			const FVector OldWorld = MarkerRefBuild.TransformPosition(FVector(OldP2.X, OldP2.Y, 0.0));

			// ② 投到新墙上，只取**沿墙**分量。法向那一维故意丢掉：墙沿自己的法线挪时窗必须跟着走
			//    （否则窗会留在半空），要守恒的只有"窗在这面墙上的哪个位置"。
			const FCSHouseEdgeFrame NewF = CSHouse_GetEdge(A.EdgeIndex, Footprint, WallThickness);
			const FVector NewStart = Build.TransformPosition(FVector(NewF.Start.X, NewF.Start.Y, 0.0));
			const FVector NewU = Build.TransformVectorNoScale(FVector(NewF.U.X, NewF.U.Y, 0.0)).GetSafeNormal();
			const float NewS = FMath::Clamp(float(FVector::DotProduct(OldWorld - NewStart, NewU)), 0.0f, NewF.Len);

			// ③ 重新表达。近角约定照旧（存哪一端只是度量方式，物理点没变）。
			A.bFromEndCorner = NewS > NewF.Len * 0.5f;
			A.DistFromCorner = A.bFromEndCorner ? (NewF.Len - NewS) : NewS;
			// ⚠️ **口径必须一起改写成斜接**：`NewS` 是在斜接框架上量的。旧锚点（口径 0，斜接之前存的档）
			// 留着 0 的话，`CSHouse_AnchorS` 在奇数边上会把这个新距离**再补一个 T** ——
			// 每改一次墙几何（拖边的每一帧），东西墙上的窗就沿墙滑一个墙厚，而且越滑越远。
			// `OldS` 那一步已经按旧口径换算过了，所以这里恒写 1。单测 `House.LegacyAnchorSurvivesResize`。
			A.SConvention = 1;

			Entry.Anchor = A;
			if (ACSHouseFeatureMarker* M = Entry.Marker.Get()) M->Reanchor(A);
		}
	}

	MarkerRefFootprint = Footprint;
	MarkerRefThickness = WallThickness;
	MarkerRefBuild = Build;
	bMarkerRefValid = true;
}

void ACSHouseActor::BuildWindowOpenings(TArray<FCSWallOpening>& OutCandidates) const
{
	OutCandidates.Reset();
	if (!bWindowsEnabled) return;

	// 循环外取一次：标记那半要逐条把锚点解成弧长，每条都重建一遍折线没有意义。
	const FCSHouseFootprint Footprint = GetFootprint();

	OutCandidates.Reserve(Windows.Num());
	for (int32 Index = 0; Index < Windows.Num(); ++Index)
	{
		const FCSHouseWindow& Request = Windows[Index];
		FCSWallOpening Opening;
		Opening.Type = ECSOpeningType::Window;
		Opening.Shape = Request.Shape;
		Opening.EdgeIndex = Request.EdgeIndex;
		Opening.CenterS = Request.CenterS;
		Opening.Width = Request.Width;
		Opening.Z0 = Request.SillZ;
		Opening.Z1 = Request.SillZ + Request.Height;
		Opening.bDoorForm = Request.bDoorForm;
		// AxisUS / Skew 一律留默认：对普通窗恒为 (0,1) 与 0，它们是给楼梯洞与将来的转角窗
		// 预留的（CSHouseProfile.h 的字段注释写明了）。现在拿来用，转角窗真做时语义会打架。

		// **身份从列表槽位派生**，不是每次重求值现掷一个 GUID：SourceId 同时是
		//   ① 谓词里"自己不与自己冲突"的键、② openings 排序的末位键（同 (边, CenterS) 时的全序）。
		// 现掷的话两次重求值会给同一扇窗不同的身份 ⇒ 排序不稳定 ⇒ 形状哈希抖动 ⇒ 幂等短路失效，
		// 而且不会有任何断言报红（同 S1 那个"槽位当随机源"的坑）。
		Opening.SourceId = FGuid(0x57494E44u /*'WIND'*/, uint32(Index), uint32(Request.EdgeIndex), 0u);
		// Tag 进顶点色 G 通道做悬停高亮。门写的是子段号，窗从 0x80 起编，两者不会撞
		// （D14 的通道字典冻结前，这只是"别互相覆盖"的最小约定，不是最终字典）。
		Opening.Tag = uint8(0x80 | (Index & 0x7F));
		OutCandidates.Add(Opening);
	}

	// 标记登记的那一半（D8）。与上面那半**完全同构**，只有身份的来源不同：
	// 属性面板那份没有别的稳定身份，只能拿槽位；标记自己带 GUID，直接用它 ——
	// 计划 D8 明写 `SourceId = 标记 GUID`。用槽位会让"删掉列表中间一个标记"把后面每一扇窗的
	// 身份都平移一格 ⇒ 谓词的"自己不与自己冲突"错位、排序全序翻转，而且不会有断言报红。
	OutCandidates.Reserve(OutCandidates.Num() + MarkerWindows.Num());
	for (int32 Index = 0; Index < MarkerWindows.Num(); ++Index)
	{
		const FCSMarkerWindow& Entry = MarkerWindows[Index];
		FCSWallOpening Opening;
		Opening.Type = ECSOpeningType::Window;
		Opening.Shape = Entry.Window.Shape;
		Opening.EdgeIndex = Entry.Window.EdgeIndex;
		// ⚠️ **弧长从锚点现算，不用登记时缓存的那一份**（2026-09-06 修）。`CenterS` 是 footprint
		// 的函数，缓存下来一改尺寸就过期，而没有任何东西会去刷新它 —— 症状是框跑到新位置、洞
		// 留在原地，再复评多少次都不会自愈。宽/高/形状/窗台高与 footprint 无关，照旧用缓存的。
		Opening.CenterS = Entry.Anchor.IsValidAnchor()
			? CSHouse_AnchorS(Entry.Anchor, Footprint, WallThickness)
			: Entry.Window.CenterS;
		Opening.Width = Entry.Window.Width;
		Opening.Z0 = Entry.Window.SillZ;
		Opening.Z1 = Entry.Window.SillZ + Entry.Window.Height;
		Opening.bDoorForm = Entry.Window.bDoorForm;
		Opening.SourceId = Entry.MarkerId;
		// Tag 与属性面板那半共用同一段（0x80 起），从列表尾部往回编，两半在 128 扇窗以内不会撞。
		Opening.Tag = uint8(0x80 | ((0x7F - Index) & 0x7F));
		OutCandidates.Add(Opening);
	}
}

FCSWallHit ACSHouseActor::RayHitWall(const FVector& WorldOrigin, const FVector& WorldDir, float MaxDistance) const
{
	const FTransform Build = GetBuildTransform();
	return CSHouse_RayHitWall(
		Build.InverseTransformPosition(WorldOrigin),
		Build.InverseTransformVectorNoScale(WorldDir.GetSafeNormal()),
		GetFootprint(), WallThickness, WallHeight, MaxDistance);
}

FCSWallHit ACSHouseActor::NearestWall(const FVector& WorldPoint, float MaxDistance) const
{
	return CSHouse_NearestWall(
		GetBuildTransform().InverseTransformPosition(WorldPoint),
		GetFootprint(), WallThickness, WallHeight, MaxDistance);
}

FTransform ACSHouseActor::AnchorToWorld(const FCSWallAnchor& InAnchor, float HalfHeight, float Standoff) const
{
	// `A * B` = 先 A 后 B ⇒ 局部到世界是 `Local * Build`。写反了房子一旦离开原点，窗就飞了。
	return CSHouse_AnchorToLocal(InAnchor, GetFootprint(), WallThickness, HalfHeight, Standoff)
		* GetBuildTransform();
}

void ACSHouseActor::RegisterFeatureMarker(const FGuid& MarkerId, const FCSHouseWindow& Demand,
	ACSHouseFeatureMarker* Marker)
{
	if (!MarkerId.IsValid()) return;

	// **按 MarkerId 升序保序插入**，不是追加：这张表喂进 openings，而 openings 的次序进形状
	// 哈希。按登记先后排的话，同一份世界状态换个加载顺序就换一份哈希 ⇒ 幂等短路时灵时不灵。
	int32 Index = 0;
	while (Index < MarkerWindows.Num() && CSHouseSeam::IdLess(MarkerWindows[Index].MarkerId, MarkerId)) ++Index;

	if (MarkerWindows.IsValidIndex(Index) && MarkerWindows[Index].MarkerId == MarkerId)
	{
		// 反引每次都刷新：同一个 MarkerId 的 actor 可能被撤销/重做换成新实例，
		// 攥着旧指针的话重建时就推不回去了（弱引用只会静静地失效，不报红）。
		if (Marker)
		{
			MarkerWindows[Index].Marker = Marker;
			MarkerWindows[Index].Anchor = Marker->GetAnchor();
		}
		if (MarkerWindows[Index].Window == Demand) return;   // 诉求没变：连标脏都不必
		MarkerWindows[Index].Window = Demand;
	}
	else
	{
		FCSMarkerWindow Entry;
		Entry.MarkerId = MarkerId;
		Entry.Window = Demand;
		Entry.Marker = Marker;
		if (Marker) Entry.Anchor = Marker->GetAnchor();
		MarkerWindows.Insert(MoveTemp(Entry), Index);
	}
	// **合批**：gizmo 多选拖 N 个窗时，这里一帧会被叫 N 次（每个标记的 Tick 各一次），
	// 而前 N−1 次的结果都会被后一次盖掉。见 `RequestReevaluate` 的注释。
	RequestReevaluate();
}

void ACSHouseActor::UnregisterFeatureMarker(const FGuid& MarkerId)
{
	const int32 Removed = MarkerWindows.RemoveAll(
		[&MarkerId](const FCSMarkerWindow& Entry) { return Entry.MarkerId == MarkerId; });
	// 合批：一次删多个窗（框选 + Delete）会一帧内连来 N 次，同 RegisterFeatureMarker。
	if (Removed > 0) RequestReevaluate();
}

FCSOpeningSite ACSHouseActor::MakeOpeningSite() const
{
	FCSOpeningSite Site;
	Site.Footprint = GetFootprint();
	Site.WallThickness = WallThickness;
	Site.WallHeight = WallHeight;
	Site.LintelBand = LintelBand;
	Site.CornerMargin = CornerMargin;
	Site.PierWidth = PierWidth;
	Site.OpeningClearance = OpeningClearance;
	Site.MinSillZ = WindowMinSillZ;
	Site.bPierStyleEnabled = bPierStyleEnabled;
	// 高阈：墩的迟回还没算（`ResolvePierSpans` 在窗过完谓词之后才跑），所以按"有没有可能
	// 被判成墩"保守判。理由完整地写在 `CSHouse_QueryOpening` 上面。
	Site.PierRestoreWidth = FMath::Max(PierStyleRestoreWidth, PierStyleMaxWidth);
	Site.Openings = CurrentOpenings;
	return Site;
}

uint32 ACSHouseActor::ComputeDoors()
{
	// 门与窗**都是派生物**，每轮从权威源重算：门来自道路推导，窗来自 `Windows` 那份显式列表。
	// 只有第三方注入的洞（楼梯穿墙等，当前无人生产）才原样留着 —— 它们没有权威源可以重导出。
	TArray<FCSWallOpening> Kept;
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		if (O.Type != ECSOpeningType::Door && O.Type != ECSOpeningType::Window) Kept.Add(O);
	}
	CurrentOpenings.Reset();
	TArray<FCSDoorRunMemory> NewMemory;

	const FVector Loc = GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(YawRad), FMath::Sin(YawRad));
	const FVector2D AxY(-FMath::Sin(YawRad), FMath::Cos(YawRad));
	auto ToWorld2D = [&](const FVector2D& L) { return FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y; };

	const float MaxDoorHeight = FMath::Min(DoorHeight, WallHeight - LintelBand);
	if (Ground && MaxDoorHeight > DoorMinWidth * 0.5f)
	{
		// 2026-09-04 重做：**门宽 = 路在墙上截出的弦长**（TG 的
		// `ArchSegment (*)(WallPathSegment)`，逐条证据见 `CSHouseDoorRuns.h` 文件头）。
		// 旧口径是"等分槽 + 覆盖率二值投票"，路只能决定这一格开不开，宽度与位置都与路无关。
		// **四条边接成一条闭合周界**（2026-09-04 下午，用户："根据 4 条边组合一个环形边"）。
		// TG 那边墙本来就是一条闭合曲线（`Rectangle2d::circular_slice` 是环形切片），所以
		// 一条压过转角的路在那边得到的是**一条跨角的段**；四条边各自求解只会把它切成两个
		// 互不相干、还各自被护角推开的洞。转角实拍见 `img/tiny-glade-ref-corner-arch-passage.png`：
		// **两道拱共用一根角柱**，角柱不是障碍，它就是中墩。
		const FCSHouseFootprint Footprint = GetFootprint();
		const int32 NumEdges = Footprint.NumEdges();
		TArray<FCSHouseEdgeFrame, TInlineAllocator<8>> Frames;
		TArray<float, TInlineAllocator<8>> EdgeStart;
		Frames.SetNum(NumEdges);
		EdgeStart.SetNumZeroed(NumEdges);
		float Perimeter = 0.0f;
		for (int32 Edge = 0; Edge < NumEdges; ++Edge)
		{
			Frames[Edge] = CSHouse_GetEdge(Edge, Footprint, WallThickness);
			EdgeStart[Edge] = Perimeter;
			Perimeter += Frames[Edge].Len;
		}

		if (Perimeter > DoorMinWidth)
		{
			// 闭环采样：覆盖 [0, Perimeter)，**末点不重复首点**（求解器的闭环口径）。
			const int32 Steps = FMath::Max(8, FMath::CeilToInt(Perimeter / FMath::Max(DoorSampleStep, 1.0f)));
			const float Step = Perimeter / Steps;

			// 环参数 → 哪条边 + 边内弧长。转角恰好落在边界上时归**后**一条边（半开区间）。
			auto RingToEdge = [&](float Ring, int32& OutEdge, float& OutLocal)
			{
				float Wrapped = FMath::Fmod(Ring, Perimeter);
				if (Wrapped < 0.0f) Wrapped += Perimeter;
				OutEdge = NumEdges - 1;
				for (int32 Edge = 0; Edge < NumEdges; ++Edge)
				{
					if (Wrapped < EdgeStart[Edge] + Frames[Edge].Len) { OutEdge = Edge; break; }
				}
				OutLocal = Wrapped - EdgeStart[OutEdge];
			};

			TArray<float> Weights;
			TArray<float> Gaps;
			Weights.Reserve(Steps);
			Gaps.Reserve(Steps);
			for (int32 K = 0; K < Steps; ++K)
			{
				int32 Edge = 0;
				float Local = 0.0f;
				RingToEdge(Step * K, Edge, Local);
				const FCSHouseEdgeFrame& F = Frames[Edge];
				const FVector2D OutWorld = AxX * (-F.In.X) + AxY * (-F.In.Y);   // 这一段的世界系外法线
				const FVector2D WP = ToWorld2D(F.Start + F.U * Local);
				// 内外两条探测线取较大者：路铺到墙根就算经过，不要求压过墙心。
				Weights.Add(FMath::Max(
					Ground->SampleRoadWeight(WP + OutWorld * DoorProbeOffset),
					Ground->SampleRoadWeight(WP - OutWorld * DoorProbeOffset)));
				Gaps.Add(float(Loc.Z - Ground->SampleHeight(WP + OutWorld * DoorProbeOffset)));
			}

			// 上一帧的区间（滞回靠交叠继承，不靠编号）。记忆现在存的是**环参数**，
			// 所以 `EdgeIndex` 恒 -1；旧的逐边记忆读进来会全部失配 ⇒ 换版本那一帧滞回失效一次，
			// 与"拉尺寸跨 round 边界"那条老坑同型但只发生一次，可接受。
			TArray<FCSDoorRun> Prev;
			for (const FCSDoorRunMemory& M : DoorRunMemory) Prev.Add(FCSDoorRun{ M.S0, M.S1 });

			FCSDoorRunParams P;
			P.OnWeight = DoorOnWeight;
			P.MinWidth = DoorMinWidth;
			P.KeepWidth = DoorMinWidth * DoorKeepWidthRatio;
			P.MaxWidth = 0.0f;      // 过宽的切分**放到按边切完之后**做，免得一个拱骑在转角上
			P.PierWidth = PierWidth;
			// 碎环段并掉。放在环上做才对：转角两边各一条窄路，在环上是"中间夹一小段墙"，
			// 并掉之后成为一条跨角段 ⇒ 按边切回去就是共用角柱的两道拱。
			P.MinWallSegment = DoorMinWallSegment;
			P.Hi = Perimeter;
			P.bClosed = true;

			TArray<FCSDoorRun> RingRuns;
			CSHouse_SolveRoadRuns(Weights, 0.0f, Step, P, Prev, RingRuns);

			// 环上的段 → 逐边的洞。跨角的段在这里被拆成两片，两片都打上转角标记。
			struct FEdgePiece { int32 Edge; FCSDoorRun Run; };
			TArray<FEdgePiece> Pieces;
			for (const FCSDoorRun& Ring : RingRuns)
			{
				NewMemory.Add([&] { FCSDoorRunMemory M; M.EdgeIndex = -1; M.S0 = Ring.S0; M.S1 = Ring.S1; return M; }());

				// 段可能跨越 0 点，所以按"环上 [S0, S1] 与每条边的 [start, start+len] 求交"来切，
				// 边的区间同时试 −Perimeter / 0 / +Perimeter 三个副本，覆盖绕回的情况。
				for (int32 Edge = 0; Edge < NumEdges; ++Edge)
				{
					for (int32 Rep = -1; Rep <= 1; ++Rep)
					{
						const float A = EdgeStart[Edge] + Rep * Perimeter;
						const float B = A + Frames[Edge].Len;
						const float C0 = FMath::Max(Ring.S0, A);
						const float C1 = FMath::Min(Ring.S1, B);
						if (C1 - C0 < 1.0f) continue;
						FEdgePiece Piece;
						Piece.Edge = Edge;
						Piece.Run = FCSDoorRun{ C0 - A, C1 - A };   // 转回边内弧长
						// 顶到边界的那一片不再单独打标记（2026-09-06 删掉了转角位）：它靠"顶着墙端"
						// 这个几何事实在 `ResolvePierSpans` 里与对面那片配成墩，与拱廊的墩同一套口径。
						Pieces.Add(Piece);
					}
				}
			}

			for (int32 PieceIndex = 0; PieceIndex < Pieces.Num(); ++PieceIndex)
			{
				const int32 Edge = Pieces[PieceIndex].Edge;
				const FCSHouseEdgeFrame& F = Frames[Edge];

				// 过宽的切分放在这里：切出来的一排拱因此**不会骑在转角上**。
				TArray<FCSDoorRun> Runs;
				CSHouse_SplitRun(Pieces[PieceIndex].Run, DoorMaxWidth, PierWidth,
					DoorMinWidth * DoorKeepWidthRatio, Runs);

			for (int32 RunIndex = 0; RunIndex < Runs.Num(); ++RunIndex)
			{
				const FCSDoorRun& Run = Runs[RunIndex];
				float Width = Run.Width();

				// 离地收窄降级成一个乘数（旧口径里它是唯一的宽度源）。平地上恒 1。
				if (bDoorGroundNarrowing)
				{
					// 段内最大落差：整条门取最坏的一处，免得门一半悬空一半贴地。
					float GapMax = 0.0f;
					// 环参数下标：段是边内弧长，先加回这条边的环起点，再按环采样步长取样本区间。
					const float RingA = EdgeStart[Edge] + Run.S0;
					const float RingB = EdgeStart[Edge] + Run.S1;
					const int32 First = FMath::FloorToInt(RingA / Step);
					const int32 Last = FMath::CeilToInt(RingB / Step);
					for (int32 K = First; K <= Last; ++K)
					{
						GapMax = FMath::Max(GapMax, Gaps[((K % Steps) + Steps) % Steps]);
					}
					Width *= ComputeDoorWidthScale(GapMax, DoorGapFull, DoorGapZero);
				}

				const float Height = MaxDoorHeight;

				// 量化后再进哈希：路是连续场，端点每帧亚厘米地抖，不量化则哈希永不相等 ⇒ 每帧全量重建。
				Width = FMath::RoundToFloat(Width / DoorWidthQuantum) * DoorWidthQuantum;
				float CenterS = FMath::RoundToFloat(Run.Center() / DoorCenterQuantum) * DoorCenterQuantum;

				// ⚠️ **量化之后必须夹回边内**（2026-09-05，实测缺陷）。洞只存中心与宽度，两者是
				// **各自**四舍五入的，端点由 `CenterS ∓ Width/2` 反算 ⇒ 端点可以被推出去
				// (中心步长 + 宽度步长)/2（默认 2 cm）。中间的门无所谓，但**转角片有一端本来就是
				// 被墙端切出来的**：推出去那一截落在墙外，任何面板都盖不到，而 `CSHouse_BuildBodySoup`
				// 的"装不下"判据拿的是名义 `Width` ⇒ 差一点点就 `continue`，症状是**砖拱砌得好好的、
				// 墙却没挖洞**，而且一声不吭（L_HouseGroundDemo 的转角上实测差 0.10 cm）。
				{
					const float ClampedS0 = FMath::Max(CenterS - Width * 0.5f, 0.0f);
					const float ClampedS1 = FMath::Min(CenterS + Width * 0.5f, F.Len);
					Width = ClampedS1 - ClampedS0;
					CenterS = (ClampedS0 + ClampedS1) * 0.5f;
				}
				if (Width < DoorMinWidth) continue;

				// 拱高（2026-09-04 起与洞宽解耦）：`DoorMaxArchRise` 为 0 时退回半宽 = 正半圆。
				// 起拱段至少留 `CSHouse_MinSpring`，所以拱高不能吃掉全部门高。
				// **用夹过之后的宽度算**：夹之前算，转角片的拱会比它自己的洞还宽一点。
				const float FullRise = FMath::Max(Height - CSHouse_MinSpring, 1.0f);
				float Rise = (DoorMaxArchRise > UE_KINDA_SMALL_NUMBER)
					? FMath::Min(DoorMaxArchRise, Width * 0.5f)   // 比半圆还高没有意义，拱只会更扁不会更尖
					: Width * 0.5f;
				Rise = FMath::Clamp(Rise, 1.0f, FullRise);
				// ⚠️ 拱高解耦之后，**洞宽不再被门高限制**（半圆时"半宽 ≤ 门高 − 起拱段"那条约束
				// 是半径与半宽相等带来的，现在半径分成了两根半轴）。宽度只受路与 DoorMaxWidth 管。

				FCSWallOpening Door;
				Door.Type = ECSOpeningType::Door;
				Door.Shape = ECSOpeningShape::Arch;
				Door.EdgeIndex = Edge;
				Door.CenterS = CenterS;
				Door.Width = Width;
				Door.Z0 = 0.0f;              // 门恒贴地；窗台高走 Z0 > 0（D8）
				Door.Z1 = Height;
				Door.ArchRise = Rise;
				Door.Tag = uint8(RunIndex & 0xFF);
				// 转角片不打任何位：它的墩位由 `ResolvePierSpans` 跨角配对时打上，不装门扇也从墩位读。
				// 拱廊：一条路被切成多拱时，每个子拱都是敞开的（用户实测："两道门并排时只有拱没有木门"）。
				if (Runs.Num() > 1) Door.StyleFlags |= CSHouse_StyleArcade;
				CurrentOpenings.Add(Door);
			}
			}
		}
	}
	DoorRunMemory = MoveTemp(NewMemory);

	// **让位规则**：门拱优先于特征标记（D6）—— 子段被点亮后，与之相交的窗判为不可行，
	// 避免拱窗互切。所以窗要在门全部落位之后再逐条过谓词。
	TArray<FCSWallOpening> WindowCandidates;
	BuildWindowOpenings(WindowCandidates);
	Kept.Append(WindowCandidates);   // 注入洞（如果有）排在前面，窗跟在后面
	CurrentWindowCount = 0;
	CurrentWindowRejectCount = 0;
	CurrentFeatureVerdicts.Reset();
	for (const FCSWallOpening& Feature : Kept)
	{
		// 裁决**就在这里**记下来，别事后再问一遍谓词：这一刻的 `CurrentOpenings` 才是它当时
		// 面对的那一份，事后问会拿"已经把自己放进去了"的表去判自己。
		const ECSFeatureReject Reason = QueryFeatureReject(Feature);
		if (Feature.SourceId.IsValid()) CurrentFeatureVerdicts.Add(Feature.SourceId, Reason);
		if (Reason != ECSFeatureReject::None)
		{
			if (Feature.Type == ECSOpeningType::Window) ++CurrentWindowRejectCount;
			continue;
		}
		// **按序插入**，不是追加到末尾：谓词里"墩跨度不接受窗"那一段靠"相邻两项即同边相邻
		// 两洞"来找跨度，一旦表乱序，下一个候选就会拿隔了一个洞的两端当跨度。
		int32 At = 0;
		while (At < CurrentOpenings.Num() && CSHouse_OpeningLess(CurrentOpenings[At], Feature)) ++At;
		CurrentOpenings.Insert(Feature, At);
		if (Feature.Type == ECSOpeningType::Window) ++CurrentWindowCount;
	}
	// 上面已经保证有序（门按边号/子段号递增加入、其余按序插入），这一下是不变量的兜底。
	// 末位键是 `SourceId`：同一 (边, CenterS) 上的两个洞若没有全序，不稳定排序会让两次
	// 重求值给出不同的顺序 ⇒ 形状哈希抖动 ⇒ 幂等短路失效（而且不会有任何断言报红）。
	CurrentOpenings.Sort([](const FCSWallOpening& A, const FCSWallOpening& B) { return CSHouse_OpeningLess(A, B); });

	// 拱间墩（D6，2026-08-30 实拍裁决）：排好序才谈得上"相邻两洞之间"。
	// 位置有两条硬约束 —— ① 在窗过完谓词之后：墩是最终洞集合的函数，先算就会把被拒的窗算进去；
	// ② 在下面那份形状哈希**之前**：样式决定几何（墩跨度不砌灰泥面板），晚一步就成了
	// "样式翻了、哈希没变 ⇒ 房体不重建"的静默失效。
	ResolvePierSpans();

	// 房体**形状**哈希：几何参数 + 门集合，**不含世界变换**（那份归 ComputePlacementHash）。
	// 纪律：desc 哈希只接受"决定顶点位置或索引的量"——材质/颜色/高亮一律走 D14 的外观通道。
	TArray<int32> H;
	CSHouse_AppendFootprintHash(H, GetFootprint());
	H.Append({ CSHouse_Q(WallHeight, 1), CSHouse_Q(WallThickness, 0.5),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1), int32(IsFlatRoof()) });
	// 山墙是房体的几何：脊长参数到 1（或换脊向）时两端的墙升成三角形，上沿跟着坡度走（坡度已在上面）；
	// 阶梯式的上沿还吃屋面整体偏移（墙顶比屋面线高 46 cm，屋面线含 `RoofHeightOffset`）。
	CSHouse_AppendRoofRidgeHash(H, RoofRidgeLength01, RoofRidgeAxis, GableRoofStyle);
	H.Add(CSHouse_Q(RoofHeightOffset, 0.1));
	// 阶梯山墙上沿那一圈由砖砌时，墙板只做到屋面线（`FCSHouseGableWall::PlasterDrop`）。
	H.Add(int32(HasSteppedGableBricks()));
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		H.Append({ O.EdgeIndex, int32(O.Shape), CSHouse_Q(O.CenterS, 1), CSHouse_Q(O.Width, DoorWidthQuantum),
			CSHouse_Q(O.Z0, 1), CSHouse_Q(O.Z1, 1), CSHouse_Q(O.Skew, 0.01),
			// 拱高进哈希：漏掉它 = 改了 `DoorMaxArchRise` 洞形变了、房体却不重建（静默失效）。
			CSHouse_Q(O.ArchRise, 0.5), CSHouse_Q(O.Rise(), 0.5),
			CSHouse_Q(O.AxisUS.X, 0.01), CSHouse_Q(O.AxisUS.Y, 0.01),
			// StyleFlags 是**决定顶点位置**的量（墩侧的面板格收到洞缘、跨度只从墩顶往上砌），
			// 不是外观通道，所以它必须在这份哈希里。漏掉它 = 迟回翻了但房体不重建。
			int32(O.StyleFlags),
			// 门形态决定谓词放不放它（窗台下限），进哈希与 `Type` 同一条理由。
			int32(O.bDoorForm) });
	}
	H.Append({ CSHouse_Q(OpeningChordTolerance, 0.01) });   // 容差决定分段数 ⇒ 决定索引数
	return CSHouse_Hash(H);
}

void ACSHouseActor::ResolvePierSpans()
{
	// 整表重算（同 DoorRunMemory）：判据是当前洞集合的纯函数，留着旧键只会让"这条边多开一个拱"
	// 之后的编号错位继承到别的跨度上去。
	TMap<uint32, bool> NewState;
	CurrentPierSpanCount = 0;
	// ⚠️ **只清墩那两位，别整份清零**：`CSHouse_StyleArcade` 是 `ComputeDoors` 在更早一步
	// 定的，而本函数在它之后跑 —— 整份清零会把它擦掉，症状是拱廊照样装门扇而且**没有任何报错**
	// （2026-09-04 转角位被这么擦掉过一次，判据 ⑥ 抓到的）。
	constexpr uint8 PierBits = CSHouse_StylePierBefore | CSHouse_StylePierAfter;
	for (FCSWallOpening& Opening : CurrentOpenings) Opening.StyleFlags &= ~PierBits;
	// 转角数 = 边数：闭合折线上每个顶点恰好是一个角。
	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumCorners = Footprint.NumEdges();
	CornerPierTopZ.Init(0.0f, NumCorners);

	FCSHousePierStyle Style;
	Style.bEnabled = bPierStyleEnabled;
	Style.MaxWidth = PierStyleMaxWidth;
	Style.RestoreWidth = PierStyleRestoreWidth;

	// CurrentOpenings 进来时已按 (边, CenterS) 排好（ComputeDoors 末尾那一下），所以同边相邻两项
	// 就是相邻两洞 —— 一趟线性扫描即可，不必再分组排序。
	int32 Begin = 0;
	while (Begin < CurrentOpenings.Num())
	{
		int32 End = Begin;
		while (End < CurrentOpenings.Num() && CurrentOpenings[End].EdgeIndex == CurrentOpenings[Begin].EdgeIndex) ++End;

		// 这条边的洞数进 key：多开/少开一个拱会把整条边的
		// 跨度重新编号，不把编号基准放进 key 就会把旧跨度的样式误继承给完全不同的一段墙。
		const uint32 Count = uint32(FMath::Min(End - Begin, 0xFF));
		for (int32 Index = Begin; Index + 1 < End; ++Index)
		{
			FCSWallOpening& Left = CurrentOpenings[Index];
			FCSWallOpening& Right = CurrentOpenings[Index + 1];
			float Span = 0.0f, TopZ = 0.0f;
			// 只有"两侧都是落地的拱"才谈得上墩（墩顶 = 起拱线，别的洞型没有这条线）——
			// 判据与理由都在 CSHouse_PierSpanBetween 里，这里不重写。
			if (!CSHouse_PierSpanBetween(Left, Right, Span, TopZ)) continue;

			const uint32 Key = (uint32(Left.EdgeIndex & 0xFF) << 24) | (Count << 16) | uint32((Index - Begin) & 0xFFFF);
			const bool bWasPier = PierSpanIsPier.FindRef(Key);
			const bool bIsPier = CSHouse_SpanIsPier(Style, Span, bWasPier);
			NewState.Add(Key, bIsPier);
			if (!bIsPier) continue;

			// 结论粘在洞上而不是另起一张表：铺墙板的 CSHouse_BuildBodySoup 是纯函数、只吃一份 desc，
			// 而洞是唯一同时流过两边且顺序天然一致的东西（见 FCSWallOpening::StyleFlags 的注释）。
			Left.StyleFlags |= CSHouse_StylePierAfter;
			Right.StyleFlags |= CSHouse_StylePierBefore;
			++CurrentPierSpanCount;
		}
		Begin = End;
	}

	// 跨角配对（2026-09-06 用户裁决：**转角就是一个墩**，两道拱只是不在同一条边上）。
	// k 号边最后一洞顶着 `Len`、k+1 号边第一洞顶着 0 ⇒ 它们之间只隔那块转角方块，跨度 = 墙厚，
	// 走同一套双阈迟回。配上就打墩位，下游一个字都不用为转角单写：门樘在转角侧自动关掉
	// （`BuildEdgeElements`）、门扇不装（`CSHouse_StyleNoLeafMask`）、墙板的格咬到洞缘
	// （`CSHouse_BuildBodySoup`）；只有"中间那根柱子"是转角特有的 —— 同边的墩由
	// `BuildEdgeElements` 沿边出，转角的墩立在角点上，由 `BuildCornerPierBricks` 读
	// `CornerPierTopZ` 出，角石（`BuildQuoinBricks`）读同一份数值在起拱线以下让路。
	for (int32 Corner = 0; Corner < NumCorners; ++Corner)
	{
		// 角 k 夹在「k 号边的远端」与「k+1 号边的近端」之间，与 `CSHouse_GetCorner` 同号。
		const int32 FarEdge = Corner;
		const int32 NearEdge = (Corner + 1) % NumCorners;
		const float FarLen = CSHouse_GetEdge(FarEdge, Footprint, WallThickness).Len;
		FCSWallOpening* Far = nullptr;
		FCSWallOpening* Near = nullptr;
		for (FCSWallOpening& O : CurrentOpenings)
		{
			// 与 `CSHouse_PierSpanBetween` 同一条前置："两侧都是落地的拱"才谈得上墩。
			if (!O.IsValid() || O.Shape != ECSOpeningShape::Arch || O.Z0 > UE_KINDA_SMALL_NUMBER) continue;
			// 2 cm 容差：`ComputeDoors` 已把转角片夹回墙端，这里只是防浮点。
			if (O.EdgeIndex == FarEdge && FMath::Abs(O.S1() - FarLen) <= 2.0f) Far = &O;
			if (O.EdgeIndex == NearEdge && FMath::Abs(O.S0()) <= 2.0f) Near = &O;
		}
		if (!Far || !Near) continue;

		// key 的顶字节 0xC0 与边号（< 0xC0）不共域：跨角跨度与同边跨度永远不互相继承样式。
		const uint32 Key = (0xC0u << 24) | uint32(Corner);
		const bool bWasPier = PierSpanIsPier.FindRef(Key);
		const bool bIsPier = CSHouse_SpanIsPier(Style, WallThickness, bWasPier);
		NewState.Add(Key, bIsPier);
		if (!bIsPier) continue;

		Far->StyleFlags |= CSHouse_StylePierAfter;
		Near->StyleFlags |= CSHouse_StylePierBefore;
		// 墩顶 = 两条起拱线的较低者（与 `CSHouse_PierSpanBetween` 同一个取法）。起拱线用
		// `Z1 − Rise()`：2026-09-04 拱高解耦之后它才是墙上裁剪场真正的那条线，半宽只在正半圆时相等。
		CornerPierTopZ[Corner] = FMath::Max(FMath::Min(Far->Z1 - Far->Rise(), Near->Z1 - Near->Rise()), 0.0f);
		++CurrentPierSpanCount;
	}

	PierSpanIsPier = MoveTemp(NewState);
}

int32 ACSHouseActor::GetOpenDoorCount() const
{
	FlushPendingReevaluate();
	int32 Count = 0;
	for (const FCSWallOpening& O : CurrentOpenings) if (O.Type == ECSOpeningType::Door) ++Count;
	return Count;
}

bool ACSHouseActor::QueryFeaturePlacement(const FCSWallOpening& Candidate) const
{
	return QueryFeatureReject(Candidate) == ECSFeatureReject::None;
}

ECSFeatureReject ACSHouseActor::QueryFeatureReject(const FCSWallOpening& Candidate) const
{
	// 判据本体在 `CSHouse_QueryOpening`（`CSHouseProfile.h`）—— 这里只负责把房子的属性打包。
	// **抽出去是为了让纯 CPU 单测能调同一条判据**：谓词与几何同维（C1 = 甲）这件事只有拿
	// 「谓词说能放 ⇒ 跑一趟 CSHouse_BuildBodySoup 看洞在不在」去证，才不是在证一份镜像。
	return CSHouse_QueryOpening(MakeOpeningSite(), Candidate);
}

float ACSHouseActor::ComputeDoorWidthScale(float GapMax, float GapFull, float GapZero)
{
	const float Span = FMath::Max(GapZero - GapFull, 1.0f);   // 参数被填反/相等时退化成硬阈，不除零
	return 1.0f - FMath::Clamp((GapMax - GapFull) / Span, 0.0f, 1.0f);
}

FCSRoofDesc ACSHouseActor::MakePitchedRoofDesc() const
{
	FCSRoofDesc Desc;
	Desc.Footprint = GetFootprint();
	Desc.EaveZ = WallHeight + RoofHeightOffset;
	Desc.Pitch = RoofPitch;
	Desc.Overhang = RoofOverhang;
	// 脊长是显式参数时把它落成逐边坡度倍率 / 山墙边；自动（< 0）与异形 footprint 留空 = 等坡度直骨架。
	if (RoofRidgeLength01 >= 0.0f)
	{
		CSHouseRoof_ApplyRidge(Desc, RoofRidgeLength01, int32(RoofRidgeAxis),
			CSHouseRoof_GableOverhang(GableRoofStyle, RoofOverhang));
	}
	return Desc;
}

FCSRoofDesc ACSHouseActor::GetRoofDesc() const
{
	FCSRoofDesc Desc = MakePitchedRoofDesc();
	Desc.bFlat = float(Desc.MaxInset()) * Desc.TanPitch() <= FMath::Max(FlatRoofThreshold, 0.0f);
	if (Desc.bFlat)
	{
		// 露台没有脊也没有山墙：倍率一并清掉，下游拿到的就是一块平的等坡度 desc。
		Desc.EaveZ = WallHeight;
		Desc.Pitch = 0.0f;
		Desc.Overhang = 0.0f;
		Desc.EdgeSteep.Reset();
		Desc.GableOverhang = 0.0f;
	}
	return Desc;
}

uint32 ACSHouseActor::ComputePlacementHash() const
{
	const FVector Loc = GetActorLocation();
	const TArray<int32> H = { CSHouse_Q(Loc.X, 1), CSHouse_Q(Loc.Y, 1), CSHouse_Q(Loc.Z, 0.5), CSHouse_Q(GetActorRotation().Yaw, 0.1) };
	return CSHouse_Hash(H);
}

FTransform ACSHouseActor::GetBuildTransform() const
{
	return FTransform(FRotator(0, GetActorRotation().Yaw, 0), GetActorLocation());
}

uint32 ACSHouseActor::ComputePillars(TArray<FVector>& OutPillarCenters, TArray<float>& OutPillarLengths) const
{
	OutPillarCenters.Reset();
	OutPillarLengths.Reset();
	if (!Ground) return 0;

	const FVector Loc = GetActorLocation();
	const float YawRad = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const FVector2D AxX(FMath::Cos(YawRad), FMath::Sin(YawRad));
	const FVector2D AxY(-FMath::Sin(YawRad), FMath::Cos(YawRad));

	// 支撑点：墙厚中线那一圈（柱子落在墙体正下方），每个角 + 每边按间距等分。
	//
	// 中线的角点就是深度 T/2 处的斜接点：外角点沿本边 U 让出 `InsetStart / 2`、再沿 In 进 T/2。
	// 矩形上它逐位等于原来的「四角各内缩半个墙厚」—— 直角让出量恰好是 T。
	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumEdges = Footprint.NumEdges();
	TArray<FVector2D, TInlineAllocator<8>> Corners;
	for (int32 E = 0; E < NumEdges; ++E)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(E, Footprint, WallThickness);
		Corners.Add(F.Start + F.U * (F.InsetStart * 0.5) + F.In * (WallThickness * 0.5));
	}
	TArray<FVector2D> Points;
	for (int32 E = 0; E < NumEdges; ++E)
	{
		const FVector2D A = Corners[E], B = Corners[(E + 1) % NumEdges];
		const float Len = float(FVector2D::Distance(A, B));
		const int32 Count = FMath::Max(1, int32(Len / PillarSpacing));
		for (int32 J = 0; J < Count; ++J) Points.Add(FMath::Lerp(A, B, float(J) / Count));   // 含起角，终角归下一条边
	}

	TArray<int32> H;
	for (const FVector2D& L : Points)
	{
		const FVector2D W = FVector2D(Loc.X, Loc.Y) + AxX * L.X + AxY * L.Y;
		const float Gap = float(Loc.Z - Ground->SampleHeight(W));
		if (Gap <= PillarMinGap) continue;
		const float Length = Gap + PillarEmbed;
		OutPillarCenters.Add(FVector(L.X, L.Y, 0));   // 局部：z=0 为房底
		OutPillarLengths.Add(Length);
		H.Append({ CSHouse_Q(L.X, 1), CSHouse_Q(L.Y, 1), CSHouse_Q(Length, 2) });
	}
	// 柱**形状**哈希：局部布点 + 柱长 + 截面，**不含世界变换**。柱长本来就吃了世界 Z
	// （Gap = 房底 Z − 地面高度），所以平地上纯 XY 平移不会改它 —— 那正是能走 TransformMesh 的情形。
	H.Append({ CSHouse_Q(PillarSize, 0.5), CSHouse_Q(PillarEmbed, 0.5) });
	return CSHouse_Hash(H);
}

void ACSHouseActor::RequestReevaluate()
{
	// 只标脏。一帧里来多少次都只是同一个布尔，兑现只在 Tick 里发生一次（N vs N² 的账见头文件）。
	bReevaluatePending = true;
	if (!IsTemplate()) SetActorTickEnabled(true);
}

void ACSHouseActor::FlushPendingReevaluate() const
{
	if (!bReevaluatePending) return;
	// `const_cast`：本函数做的是"把已经欠下的那一次重求值当场补上"，补完之后对外可见的状态
	// 与"合批没生效、当场就跑了"逐位相同 —— 那正是 const 想保证的东西。而 getter 必须是
	// const（`BlueprintPure` 要求），补票只能在这里做。
	const_cast<ACSHouseActor*>(this)->ReevaluateSite();
}

void ACSHouseActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// 拖动兜底**自己兑现**（D7 09-16 晚）：松手事件没来就没人再叫醒这栋房，只在下次重求值里查超时等于永远不查。
	// 超时 = 按松手处理：清标志、强制全量重建一次（同 `PostEditMove(true)`）、重求值里接缝照常提交。
	if (bInGizmoDrag && FPlatformTime::Seconds() - GizmoDragTouchedAt > FMath::Max(GizmoDragIdleSeconds, 0.0f))
	{
		bInGizmoDrag = false;
		bForceFullRebuild = true;
		bReevaluatePending = true;
	}
	// **先关再跑**：重建途中若又来了通知，`RequestReevaluate` 会把 tick 重新打开、留给下一帧。
	// 顺序反过来，那次打开就被这一行关掉，新诉求要等到下一次别的唤醒才兑现。还在拖（等超时）就留着。
	SetActorTickEnabled(bInGizmoDrag);
	FlushPendingReevaluate();
}

void ACSHouseActor::ReevaluateSite()
{
	if (bInReevaluate || IsTemplate() || !GetWorld()) return;
	TGuardValue<bool> Guard(bInReevaluate, true);
	// 欠账**在开头**清：重建途中新到的通知会重新置位，由下一次 Tick 兑现（见 bReevaluatePending）。
	bReevaluatePending = false;

	ResolveGroundAndSubscribe();

	// 形状不是严格凸时按凸包使用（`GetFootprint`），这里只负责"出声一次"。
	WarnIfFootprintShapeFixed();

	// ① 落座：绝对式，升降对称（计划 D4）。
	const double SeatZ = ComputeSeatZ();
	FVector Loc = GetActorLocation();
	if (FMath::Abs(Loc.Z - SeatZ) > 0.5)
	{
		Loc.Z = SeatZ;
		SetActorLocation(Loc);
	}

	// ①.5 摆位定了：外皮推给地面（建筑周边灌木）。拖动的每一帧都推，地面没变就短路。
	PublishFootprintToGround();

	const uint32 PlacementHash = ComputePlacementHash();

	// ② 房体（门同时依赖 Colors 与 Heights）。
	//
	// **顺序纪律**：必须先照常算完门、再比 ShapeHash，绝不能"位置没变就跳过算门" —— 门的
	// 存亡由 SampleRoadWeight(世界 XY) 决定、门宽由 GapMax = 房底 Z − SampleHeight(世界 XY)
	// 连续决定，**门集合本来就是世界摆位的函数**。ShapeHash 不是"局部量"，是已把世界采样吸收
	// 进去的派生量。算门很便宜（约 340 次镜像双线性），省它不划算还会出错。
	//
	// 接缝裁剪必须排在门**之前**只有一个理由：它要在同一次重求值里进同一份房体形状哈希。
	// 它与门互不影响 —— 接缝只吃两房的摆位，门只吃道路与落差（裁决二："其它任何内容都是独立的"）。
	// ⚠️ **必须排在 `ComputeDoors()` 之前**：洞的弧长是从锚点现解的（见 `FCSMarkerWindow::Anchor`），
	// 排在后面的话洞会用上一轮的锚点算，而框已经按新锚点摆好了 —— 又是一次框洞分家。
	ReanchorMarkersToPreserveWorld();

	const uint32 SeamCutHash = ComputeSeamCuts();
	const uint32 BodyHash = CSHouse_Hash({ int32(ComputeDoors()), int32(SeamCutHash), int32(bBottomFace) });
	// "网格在不在"不够：删除后撤销，复活的网格对象还在、显存却已被渲染组件还掉了（IsSlotMeshLive）。
	ReconcileMeshSlot(BodySlot, BodyHash, PlacementHash, bForceFullRebuild || !IsSlotMeshLive(TinyGladeMesh),
		[this]() { RebuildBodyMesh(); }, [this]() { return ApplyBodyPlacement(); });

	// ③ 柱（独立组件——纯地形变化只走到这，不碰房体）。
	TArray<FVector> Centers;
	TArray<float> Lengths;
	const uint32 PillarHash = ComputePillars(Centers, Lengths);
	ReconcileMeshSlot(PillarSlot, PillarHash, PlacementHash, bForceFullRebuild || (Centers.Num() > 0 && !IsSlotMeshLive(PillarMesh)),
		[&]() { RebuildPillarMesh(Centers, Lengths); }, [this]() { return ApplyPillarPlacement(); });
	CurrentPillarCount = Centers.Num();

	// ④ 门框砖：clip 的配套件，洞集合一变就要重排（哈希短路吸收无效唤醒）。
	if (bForceFullRebuild) FrameDescHash = 0;
	RebuildFrame();

	// ⑤ 藤蔓：与门框砖同一档的“墙的配套件”，同样靠哈希短路吸收无效唤醒。
	//    排在门框之后是因为它读 `CurrentOpenings`（避让墙洞），那份表由 ComputeDoors 定。
	if (bForceFullRebuild) VineDescHash = 0;
	RebuildVine();

	// ⑥ 屋面瓦：四坡的屋面**全部**由它铺成，房体三角汤里一片屋面都没有。
	//    只吃屋面 desc + 摆位，不读门、不读地面 —— 所以排在哪儿都对，放这里只是顺着"墙 → 屋面"读。
	if (bForceFullRebuild) RoofTileDescHash = 0;
	RebuildRoofTiles();
	RebuildFlatRoof();

	//    尖顶跟在瓦后面：它要盖住的正是瓦在脊端点上的破口，读的也是同一份屋面 desc。
	if (bForceFullRebuild) RoofFinialDescHash = 0;
	RebuildRoofFinials();

	//    门扇：读的是 ③ 定下来的 `CurrentOpenings`，所以必须排在 ComputeDoors 之后。
	//    与门框砖并列（都是"洞的配套件"），但走普通静态网格组件而不是实例化路径。
	if (bForceFullRebuild) DoorLeafDescHash = 0;
	RebuildDoorLeaves();

	// ⑦ 装饰摆件（D12 的**锚点那一半**）：排在最后，因为它锚在前面每一样东西上 ——
	//    门（③ 的 `CurrentOpenings`）、墙脚（②）、檐口/屋脊（屋面 desc），还要读地面镜像
	//    落高与排除道路。TG 的 `populate_autoclutter_regions` 同样排在建筑系统之后。
	if (bForceFullRebuild) DecorDescHash = 0;
	RebuildDecor();

	// ⑧ 附属物（D8）：回推裁决 + 按锚点吸附。**必须排在最后** —— 它要读 ⑦ 之前定下来的
	//    `CurrentFeatureVerdicts`，而那份表由 ② 的落位循环产出。
	NotifyMarkersRebuilt();

	// ⑨ 拉尺寸模式里边数变了（改 `FootprintShape`、撤销……）：锥子按新边数对齐。不在模式里 / 已经对上时零成本。
	//    放在最后是因为它只读 footprint、会摆放抓手，不影响上面任何一样派生物。
	SyncEdgeHandlesToFootprint();

	bForceFullRebuild = false;
	++ReevaluateCount;   // 合批的观测量（`GetReevaluateCount`），只数真正跑完的
	TArray<int32> TerraceInput = { int32(PlacementHash), CSHouse_Q(WallHeight, 0.01), CSHouse_Q(WallThickness, 0.01), int32(IsFlatRoof()) };
	CSHouse_AppendFootprintHash(TerraceInput, GetFootprint());
	const uint32 NewTerraceHash = CSHouse_Hash(TerraceInput);
	if (NewTerraceHash != TerraceSiteHash)
	{
		TerraceSiteHash = NewTerraceHash;
		OnTerraceChanged.Broadcast(this);
	}
}

void ACSHouseActor::WarnIfFootprintShapeFixed()
{
	if (FootprintShape.Num() < 3)
	{
		FootprintShapeWarnedHash = 0;
		return;
	}

	ECSFootprintShapeFix Fix = ECSFootprintShapeFix::Kept;
	const FCSHouseFootprint Used = FCSHouseFootprint::FromShape(FootprintShape, FootprintSize, &Fix);
	if (Fix == ECSFootprintShapeFix::Kept || Fix == ECSFootprintShapeFix::Rect)
	{
		FootprintShapeWarnedHash = 0;
		return;
	}

	// 只哈希**形状本身**（不含尺寸）：正缩放保凸、保共线，拖 FootprintSize 不改变"要不要取凸包"，
	// 所以拖尺寸的每一帧都不该重复告警。处置方式也拼进去：同一个形状从"凸包"变成"退化"要再说一次。
	uint32 Hash = FCrc::MemCrc32(FootprintShape.GetData(), FootprintShape.Num() * sizeof(FVector2D));
	Hash = HashCombine(Hash, uint32(Fix) + 1u);
	if (Hash == FootprintShapeWarnedHash) return;
	FootprintShapeWarnedHash = Hash;

	if (Fix == ECSFootprintShapeFix::Hulled)
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 的 FootprintShape 不是严格凸的（凹角 / 共线点 / 重复点 / 乱序）：按凸包使用，顶点数 %d → %d。")
			TEXT("属性本身不改写；凹 footprint 不支持（2026-09-14 裁决：取凸包）。"),
			*GetName(), FootprintShape.Num(), Used.NumEdges());
	}
	else
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 的 FootprintShape 退化（%d 个点的凸包不足 3 个顶点或面积为零）：按矩形使用。"),
			*GetName(), FootprintShape.Num());
	}
}

FCSHouseWindowBrushRequest ACSHouseActor::OnWindowBrushRequest;

void ACSHouseActor::StartWindowBrush()
{
	OnWindowBrushRequest.Broadcast(this);
}

void ACSHouseActor::NotifyMarkersRebuilt()
{
	// 反引失效的顺手清掉：标记被删时会自己 `UnregisterFeatureMarker`，但"actor 没了却没走
	// 注销"这条路是存在的（关卡卸载的销毁次序不保证），留着会让 `GetFeatureMarkerCount`
	// 报出一个根本不存在的窗。
	MarkerWindows.RemoveAll([](const FCSMarkerWindow& Entry) {
		return Entry.Marker.IsStale();
	});

	for (const FCSMarkerWindow& Entry : MarkerWindows)
	{
		ACSHouseFeatureMarker* Marker = Entry.Marker.Get();
		if (!Marker) continue;

		// ① 裁决回执。没有它，拉尺寸/改墙高把窗挤掉之后标记还在说"我切出洞了" ——
		//    这两条路都不经过 `OnHandleDrag`，而回执原本只在那里写。
		const ECSFeatureReject* Reason = CurrentFeatureVerdicts.Find(Marker->GetMarkerId());
		Marker->ApplyHostVerdict(Reason ? *Reason : ECSFeatureReject::None);

		// ② 按锚点吸附（TG `move_decorators_following_anchors`）。
		//    ⚠️ 正在被 gizmo 拖的那一个**不写** —— 标记不 attach 在房子下，写它纯粹是和 gizmo
		//    抢方向盘。（拉尺寸抓手 attach 着，2026-09-06 改成每次都重摆，成因见 SnapResizeHandles。）
		if (!Marker->IsBeingDragged()) Marker->SnapToAnchor();
	}
}

void ACSHouseActor::RebuildHouse()
{
	bForceFullRebuild = true;
	// 两张迟回表一起清：留着其中一张就成了"门从头判、墩却接着上一代的记忆"，
	// 同一个世界状态会因为上一次的历史给出两种房子。
	DoorRunMemory.Empty();
	PierSpanIsPier.Empty();
	ReevaluateSite();
}

// -----------------------------------------------------------------------------
// 拉尺寸（D5）：单边推拉的机制入口
// -----------------------------------------------------------------------------

float ACSHouseActor::PushEdge(int32 EdgeIndex, float Offset, bool bFinished)
{
	// 抓手拖动 = 接缝冻结期（D7 09-16），松手提交；与 PostEditMove 同一对标志。
	NoteGizmoDrag(bFinished);

	FVector2D NewSize = FootprintSize;
	FVector NewCentre = GetActorLocation();
	TArray<FVector2D> NewShape;
	float Applied = 0.0f;

	// 矩形（形状为空或退回矩形）走矩形版：那条的算术被 `House.EdgePush` 按位钉着，旧存档一位都不许变。
	const FCSHouseFootprint Current = GetFootprint();
	const bool bShaped = FootprintShape.Num() >= 3
		&& !Current.EqualsApprox(FCSHouseFootprint::MakeRect(FootprintSize), 0.0);
	if (!bShaped)
	{
		Applied = CSHouse_ApplyEdgePush(NewSize, NewCentre, EdgeIndex,
			float(GetActorRotation().Yaw), Offset, MinFootprint);
	}
	else
	{
		// 异形：推完的折线已经按包围盒居中 ⇒ 它的包围盒就是新尺寸、它本身就是新形状
		// （`FromShape` 会再拉伸一次，比例恰好是 1）。
		// ⚠️ `Current` 是**凸包之后**的折线：输入里被凸包丢掉的点（凹角处 / 共线 / 重复）在推边之后不再回来 ——
		// 推边是显式的几何编辑，写回的就是被推的那个凸包。与"凸包只在使用时取、不回写"不矛盾：
		// 那条挡的是细节面板逐个加顶点的中间态，拖锥子不经过那条路。
		FCSHouseFootprint Local = Current;
		Applied = CSHouse_ApplyEdgePushPolyline(Local, NewCentre, EdgeIndex,
			float(GetActorRotation().Yaw), Offset, MinFootprint);
		NewSize = Local.GetBounds().GetSize();
		NewShape = MoveTemp(Local.Verts);
	}

	// 尺寸没动就一步都不走：推到 MinFootprint 下限之后每帧都会走到这里，照常重求值的话
	// 那一整段"墙拖不动"的时间里房子仍在无谓地重算门、砖、藤、摆件。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	if (Applied != 0.0f)
	{
		FootprintSize = NewSize;
		if (bShaped) FootprintShape = MoveTemp(NewShape);
		// 中心与尺寸必须**同一帧**落地：只改其中一个，画面上就是"对侧墙也跟着走"，
		// 与计划 D5 那个"拖 1 m 走 2 m"的父子回路缺陷逐像素相同，极易误诊到别处。
		SetActorLocation(NewCentre);
	}

	// 松手 = gizmo 的 PostEditMove(bFinished=true)：把拖动期为了零阻塞留下的容量 / 包围盒余量重新收紧。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

float ACSHouseActor::PushHeight(float Offset, bool bFinished)
{
	// 抓手拖动 = 接缝冻结期（D7 09-16），松手提交；与 PostEditMove 同一对标志。
	NoteGizmoDrag(bFinished);

	const float Desired = FMath::Max(WallHeight + Offset, FMath::Max(MinWallHeight, 1.0f));
	const float Applied = Desired - WallHeight;

	// 高度没动就一步都不走：压到下限之后每帧都会走到这里，照常重求值的话那一整段
	// "墙压不下去"的时间里房子仍在无谓地重算门、砖、瓦、藤、摆件。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	WallHeight = Desired;

	// 墙高波及檐口、屋面、门的离地收窄、窗的 AboveEave 谓词、藤与摆件的锚点 —— 只重建墙板
	// 是不够的，必须走完整的重求值。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

float ACSHouseActor::GetRoofRise() const
{
	// 脊高由主坡面定：等坡度时是半短边 × tan，脊向钉在短轴上（`RoofRidgeAxis`）时是半长边 × tan。
	const FCSRoofDesc Roof = MakePitchedRoofDesc();
	return float(Roof.MaxInset()) * Roof.TanPitch();
}

bool ACSHouseActor::IsFlatRoof() const
{
	return GetRoofRise() <= FMath::Max(FlatRoofThreshold, 0.0f);
}

FCSHouseTerraceChanged ACSHouseActor::OnTerraceChanged;

FCSStairsTerraceSite ACSHouseActor::GetStairsTerraceSite() const
{
	FCSStairsTerraceSite Site;
	Site.Roof = GetRoofDesc();
	Site.Roof.bFlat &= !IsActorBeingDestroyed();
	Site.World = GetBuildTransform();
	Site.WallThickness = WallThickness;
	return Site;
}

float ACSHouseActor::PushRoofHeight(float Offset, bool bFinished)
{
	NoteGizmoDrag(bFinished);
	const double Inset = MakePitchedRoofDesc().MaxInset();
	const float Before = GetRoofRise();
	const float Pitch = CSHouseRoof_PitchFromRise(Before + Offset, Inset);
	if (Pitch == RoofPitch && !bFinished) return 0.0f;
	Modify(); // 编辑器 gizmo 的事务必须记录宿主属性，抓手自身是 transient。
	RoofPitch = Pitch;
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return GetRoofRise() - Before;
}

bool ACSHouseActor::GetRoofRidgeFrame(FCSHouseRidgeFrame& Out) const
{
	const FCSHouseFootprint Footprint = GetFootprint();
	if (!CSHouseRoof_IsRectangle(Footprint)) return false;

	// 顺脊的那对边：显式时读 `RoofRidgeAxis`；自动时脊沿长边（等长归 0 号边 —— 矩形时代「平局归 X」）。
	const FCSHouseEdgeFrame Edge0 = CSHouse_GetEdge(0, Footprint, 0.0f);
	const FCSHouseEdgeFrame Edge1 = CSHouse_GetEdge(1, Footprint, 0.0f);
	Out.Axis = RoofRidgeLength01 >= 0.0f ? (int32(RoofRidgeAxis) & 1) : (Edge0.Len >= Edge1.Len ? 0 : 1);
	Out.Direction = Out.Axis == 0 ? Edge0.U : Edge1.U;
	Out.Along = double(Out.Axis == 0 ? Edge0.Len : Edge1.Len);
	Out.Across = double(Out.Axis == 0 ? Edge1.Len : Edge0.Len);
	// 矩形的中心 = 对角两个顶点的中点（`FromShape` 居中过，矩形上就是原点；这里不假定）。
	Out.Centre = (Footprint.Verts[0] + Footprint.Verts[2]) * 0.5;

	// 脊长 cm：自动 = 等坡度四坡的推论（长边 − 短边）；显式 = TG 的 `RoofRidgeDims::calc`，不超过 0.1 m 的脊是一个尖。
	if (RoofRidgeLength01 < 0.0f)
	{
		Out.Length = FMath::Max(Out.Along - Out.Across, 0.0);
	}
	else
	{
		Out.Length = CSHouseRoof_RidgeLengthFrom01(double(RoofRidgeLength01), Out.Along);
		if (Out.Length <= CSHouseRoof_RidgeMin) Out.Length = 0.0;
	}
	return true;
}

float ACSHouseActor::GetRoofRidgeLength() const
{
	FCSHouseRidgeFrame Ridge;
	if (GetRoofRidgeFrame(Ridge)) return float(Ridge.Length);

	// 异形房子：脊长不是参数，报骨架最高那段脊有多长。
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(MakePitchedRoofDesc(), Skeleton);
	return float(FVector2D::Distance(Skeleton.TopA, Skeleton.TopB));
}

bool ACSHouseActor::IsGableRoof() const
{
	return !IsFlatRoof() && MakePitchedRoofDesc().HasGable();
}

float ACSHouseActor::PushRidgeLength(float Offset, bool bFinished)
{
	NoteGizmoDrag(bFinished);

	FCSHouseRidgeFrame Ridge;
	// 脊长只对矩形房子是参数（TG 同）；露台没有脊。
	if (!GetRoofRidgeFrame(Ridge) || IsFlatRoof()) return 0.0f;

	// 厘米 → TG 的参数：`inverse_ridge_01_param` 钳在 [0, 1]，拖过房子两头时钳出来的那个**恰好的 1** 就是山墙。
	// 自动的房子从它现在的样子（脊长 = 长边 − 短边）起步，第一次真动了才把脊长与脊向一起钉成显式。
	const bool bExplicit = RoofRidgeLength01 >= 0.0f;
	const double BeforeLength = bExplicit
		? CSHouseRoof_RidgeLengthFrom01(double(RoofRidgeLength01), Ridge.Along)
		: FMath::Max(Ridge.Along - Ridge.Across, CSHouseRoof_RidgeMin);
	const float Before01 = bExplicit ? RoofRidgeLength01 : float(CSHouseRoof_Ridge01FromLength(BeforeLength, Ridge.Along));
	const float Wanted01 = float(CSHouseRoof_Ridge01FromLength(BeforeLength + double(Offset), Ridge.Along));

	// 没动就一步都不走：顶在 0 或 1 上每帧都会走到这里（同 `PushHeight`）。
	if (Wanted01 == Before01 && !bFinished) return 0.0f;

	if (Wanted01 != Before01)
	{
		Modify(); // 编辑器 gizmo 的事务必须记录宿主属性，抓手自身是 transient。
		if (!bExplicit) RoofRidgeAxis = Ridge.Axis == 0 ? ECSRoofRidgeAxis::X : ECSRoofRidgeAxis::Y;
		RoofRidgeLength01 = Wanted01;
	}
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return float(CSHouseRoof_RidgeLengthFrom01(double(Wanted01), Ridge.Along) - BeforeLength);
}

void ACSHouseActor::SwitchRoofRidgeDirection()
{
	FCSHouseRidgeFrame Ridge;
	if (!GetRoofRidgeFrame(Ridge)) return;

	Modify();
	// 自动的房子先把现状钉成显式（等坡度的那个脊长），再换向 —— 否则换完向脊长仍按"自动"读，等于没换。
	if (RoofRidgeLength01 < 0.0f)
	{
		RoofRidgeLength01 = float(CSHouseRoof_Ridge01FromLength(FMath::Max(Ridge.Along - Ridge.Across, CSHouseRoof_RidgeMin), Ridge.Along));
	}
	// 参数是比例，换向后原样沿用：山墙换完还是山墙，只是脊转了 90°（顺脊边长换成了另一条）。
	RoofRidgeAxis = Ridge.Axis == 0 ? ECSRoofRidgeAxis::Y : ECSRoofRidgeAxis::X;
	bForceFullRebuild = true;
	ReevaluateSite();
	SnapResizeHandles();
}

void ACSHouseActor::ResetRoofRidgeToTinyGladeDefault()
{
	const FCSHouseFootprint Footprint = GetFootprint();
	if (!CSHouseRoof_IsRectangle(Footprint)) return;

	Modify();
	// TG 新建矩形房子的口径：脊沿长边，脊长参数取 `ridge_length_01_from_rectangle_ratio`。
	const double Len0 = double(CSHouse_GetEdge(0, Footprint, 0.0f).Len);
	const double Len1 = double(CSHouse_GetEdge(1, Footprint, 0.0f).Len);
	RoofRidgeAxis = Len0 >= Len1 ? ECSRoofRidgeAxis::X : ECSRoofRidgeAxis::Y;
	RoofRidgeLength01 = CSHouseRoof_DefaultRidgeLength01(Len0, Len1);
	bForceFullRebuild = true;
	ReevaluateSite();
	SnapResizeHandles();
}

ACSHouseHeightHandleActor* ACSHouseActor::GetRoofHandle() const
{
	for (ACSHouseHandleActor* Handle : ResizeHandles)
	{
		ACSHouseHeightHandleActor* Height = Cast<ACSHouseHeightHandleActor>(Handle);
		if (IsValid(Height) && Height->GetSide() == ECSHouseHeightHandleSide::Roof) return Height;
	}
	return nullptr;
}

float ACSHouseActor::PushBase(float Offset, bool bFinished)
{
	// 抓手拖动 = 接缝冻结期（D7 09-16），松手提交；与 PostEditMove 同一对标志。
	NoteGizmoDrag(bFinished);

	// 底动顶不动（用户裁决 2026-09-14）：房底抬 Δ ⇔ `HeightOffset += Δ`、`WallHeight -= Δ`。
	// 两条下限同时夹，生效量取两者都允许的那一段：
	//  · 往上受墙高下限管：墙最多矮到 `MinWallHeight`；已经矮于下限的旧存档往上一步都不给（不许再矮），往下照常。
	//  · 往下受贴地管：`HeightOffset` 最低到 `min(0, 当前值)` —— 旧存档里已经是负数的以当前值为界，
	//    **不许一抓就跳到 0**（那一跳在画面上是房子自己蹦起来）。
	const float WallFloor = FMath::Max(MinWallHeight, 1.0f);
	const float MaxRaise = FMath::Max(WallHeight - WallFloor, 0.0f);
	const float OffsetFloor = FMath::Min(HeightOffset, 0.0f);
	const float MaxLower = FMath::Max(HeightOffset - OffsetFloor, 0.0f);
	const float Applied = FMath::Clamp(Offset, -MaxLower, MaxRaise);

	// 顶在下限上每帧都会走到这里：没动就一步都不走（同 PushHeight）。
	if (Applied == 0.0f && !bFinished) return 0.0f;

	// **同一次调用里两个量一起改完**再重求值：分两次改的话中间那一刻檐口会先掉 Δ、再回来，
	// 重求值若恰好插在中间（合批 tick、快扫），屋顶会闪一帧。
	HeightOffset += Applied;
	WallHeight -= Applied;

	// 波及面是 PushHeight 的超集（墙高 + 落座 Z ⇒ 柱、门的离地收窄、藤的悬空判据、摆件落高），走完整重求值。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();
	return Applied;
}

// -----------------------------------------------------------------------------
// 拉尺寸模式（D5 交互层）：抓手 actor 的生成 / 回位 / 销毁
// -----------------------------------------------------------------------------

FCSHouseResizeModeChanged ACSHouseActor::OnResizeModeChanged;

namespace
{
/**
 * 抓手的生成参数（推墙箭头与高度框共用一份）。
 *
 * `RF_Transient` 不存盘（计划 D5）。**顺带丢掉 `RF_Transactional`**：抓手的位移本身没有
 * 撤销语义——真正该被撤销的是 FootprintSize / WallHeight / HeightOffset，而它们由 Push* 直接写。
 * 两者各记一半的话 Ctrl+Z 会撤回抓手却留下尺寸，下一次拖动从一个自相矛盾的状态起步。
 */
FActorSpawnParameters CSHouse_HandleSpawnParams(AActor* Owner)
{
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	SpawnParams.Owner = Owner;
	return SpawnParams;
}
}

ACSHouseResizeHandleActor* ACSHouseActor::SpawnEdgeHandle(int32 EdgeIndex)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	ACSHouseResizeHandleActor* Handle = World->SpawnActor<ACSHouseResizeHandleActor>(
		GetActorLocation(), GetActorRotation(), CSHouse_HandleSpawnParams(this));
	if (!Handle) return nullptr;

	// ⚠️ 顺序在两种抓手里都一样：**先 attach 再 Initialize**。`InitializeHandle` 末尾的
	// `SnapToCanonical` 写的是世界位置，attach 会把它换算成相对量；反过来的话抓手的相对位置
	// 会被算成"世界原点到规范位置"，房子一移动抓手就飞了。
	Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	Handle->InitializeHandle(this, EdgeIndex);
#if WITH_EDITOR
	Handle->SetActorLabel(FString::Printf(TEXT("%s_ResizeHandle_%d"), *GetActorLabel(), EdgeIndex));
#endif
	ResizeHandles.Add(Handle);
	return Handle;
}

ACSHouseHeightHandleActor* ACSHouseActor::SpawnHeightHandle(ECSHouseHeightHandleSide Side)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	ACSHouseHeightHandleActor* Handle = World->SpawnActor<ACSHouseHeightHandleActor>(
		GetActorLocation(), GetActorRotation(), CSHouse_HandleSpawnParams(this));
	if (!Handle) return nullptr;

	Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	Handle->InitializeHandle(this, Side);
#if WITH_EDITOR
	Handle->SetActorLabel(FString::Printf(TEXT("%s_%s"), *GetActorLabel(),
		Side == ECSHouseHeightHandleSide::Base ? TEXT("BaseHandle") : (Side == ECSHouseHeightHandleSide::Roof ? TEXT("RoofHandle") : TEXT("HeightHandle"))));
#endif
	ResizeHandles.Add(Handle);
	return Handle;
}

ACSHouseRidgeHandleActor* ACSHouseActor::SpawnRidgeHandle(int32 End)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	ACSHouseRidgeHandleActor* Handle = World->SpawnActor<ACSHouseRidgeHandleActor>(
		GetActorLocation(), GetActorRotation(), CSHouse_HandleSpawnParams(this));
	if (!Handle) return nullptr;

	// 先 attach 再 Initialize（同另外两种抓手）。
	Handle->AttachToActor(this, FAttachmentTransformRules::KeepWorldTransform);
	Handle->InitializeHandle(this, End);
#if WITH_EDITOR
	Handle->SetActorLabel(FString::Printf(TEXT("%s_RidgeHandle_%d"), *GetActorLabel(), End));
#endif
	ResizeHandles.Add(Handle);
	return Handle;
}

ACSHouseRidgeHandleActor* ACSHouseActor::GetRidgeHandle(int32 End) const
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseRidgeHandleActor* Ridge = Cast<ACSHouseRidgeHandleActor>(Handle);
		if (IsValid(Ridge) && Ridge->GetEnd() == (End & 1)) return Ridge;
	}
	return nullptr;
}

void ACSHouseActor::EnterResizeMode()
{
	if (!GetWorld()) return;

	// 幂等（计划 D5）：先把已经失效的格子摘掉，还剩抓手就只对齐边数、归位，不再生一组。
	// 用户在详情面板上连点两下这个按钮是常态，不挡的话场景里会攒出两组锥子。
	ResizeHandles.RemoveAll([](const TObjectPtr<ACSHouseHandleActor>& H) { return !IsValid(H); });
	if (ResizeHandles.Num() > 0)
	{
		SyncEdgeHandlesToFootprint();
		SnapResizeHandles();
		return;
	}

	// 边数 = **取完凸包之后**的 footprint 边数（用户裁决 2026-09-14："折线有多少折手柄就有多少个"）。
	const int32 NumEdges = GetFootprint().NumEdges();
	ResizeHandles.Reserve(NumEdges + 5);

	// ① 每面墙一个锥子：水平推拉，每条边一个独立自由度 ⇒ 每条边一个 actor（矩形四个）。
	for (int32 Edge = 0; Edge < NumEdges; ++Edge) SpawnEdgeHandle(Edge);

	// ② 三个套在房子外面的矩形框：檐口那个上下拖改墙高，房底那个上下拖改房底（底动顶不动），屋顶小框改起伏。
	//    **一个框只有一个自由度 ⇒ 一个框一个 actor**（拆成四根的话用户抓哪根都在改同一个量，
	//    四个 gizmo 互相打架）。将来再加竖直抓手也只是在这里多调一次 `SpawnHeightHandle`。
	SpawnHeightHandle(ECSHouseHeightHandleSide::Eave);
	SpawnHeightHandle(ECSHouseHeightHandleSide::Base);
	SpawnHeightHandle(ECSHouseHeightHandleSide::Roof);

	// ③ 脊两端各一只箭头：顺脊拖改脊长，拖到与房子一样长就是双坡 + 山墙（TG `ui_edit_roof_ridge_length`）。
	//    两只改的是同一个量（脊居中、两头一起伸缩）；异形房子 / 露台上它们自己藏起来，不必在这里分支。
	SpawnRidgeHandle(0);
	SpawnRidgeHandle(1);

	if (ResizeHandles.Num() > 0)
	{
		OnResizeModeChanged.Broadcast(this, true);
	}
}

void ACSHouseActor::SyncEdgeHandlesToFootprint()
{
	if (ResizeHandles.Num() == 0 || bSyncingResizeHandles) return;
	TGuardValue<bool> Guard(bSyncingResizeHandles, true);

	const FCSHouseFootprint Footprint = GetFootprint();
	const int32 NumEdges = Footprint.NumEdges();
	TArray<ACSHouseResizeHandleActor*> Cones;
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseResizeHandleActor* Cone = Cast<ACSHouseResizeHandleActor>(Handle);
		if (IsValid(Cone)) Cones.Add(Cone);
	}

	// 抓手摆位吃的几何：footprint 顶点 + 墙厚 + 墙高（锥子挂在墙外皮中点、`WallHeight × HandleHeightFraction`，
	// 框吃包围盒与檐口）。边数没变而它们变了（细节面板里挪一个顶点、改墙高、撤销……）只需重摆，不需要重建。
	TArray<int32> GeometryInput;
	Footprint.AppendQuantizedHash(GeometryInput);
	GeometryInput.Append({ int32(FMath::RoundToInt(WallThickness * 2.0f)), int32(FMath::RoundToInt(WallHeight * 2.0f)) });
	// 脊端箭头挂在脊的两头、屋脊之上：在细节面板里改脊长参数 / 脊向 / 坡度时它们也要跟着走。
	GeometryInput.Append({ int32(FMath::RoundToInt(GetRoofRidgeLength() * 2.0f)), int32(FMath::RoundToInt(GetRoofRise() * 2.0f)),
		int32(RoofRidgeAxis), int32(RoofRidgeLength01 >= 0.0f) });
	const uint32 GeometryHash = FCrc::MemCrc32(GeometryInput.GetData(), GeometryInput.Num() * sizeof(int32));

	// 稳态：锥子数 = 边数，且边号恰好铺满 0..N-1。每次重求值都会走到这里，稳态下不许有任何动作。
	if (Cones.Num() == NumEdges)
	{
		TBitArray<> Seen(false, NumEdges);
		bool bMatched = true;
		for (const ACSHouseResizeHandleActor* Cone : Cones)
		{
			const int32 Edge = Cone->GetEdgeIndex();
			if (Edge < 0 || Edge >= NumEdges || Seen[Edge]) { bMatched = false; break; }
			Seen[Edge] = true;
		}
		if (bMatched)
		{
			if (GeometryHash != ResizeHandleGeometryHash)
			{
				ResizeHandleGeometryHash = GeometryHash;
				SnapResizeHandles();
			}
			return;
		}
	}
	ResizeHandleGeometryHash = GeometryHash;

	// 按边号排好，**复用**前 min(旧, 新) 个（改认 0..k-1 号边），多的销毁、缺的补生。
	// 复用是为了让被选中的锥子留得住：销毁一个选中的 actor 会触发选中集变化，编辑器侧的失选监听
	// 在选中集里找不到归属者时就把整个模式退掉 —— 用户只是改了一下形状，锥子却全没了。
	// （`TArray<T*>::Sort` 会解引用，谓词拿到的是对象引用。）
	Cones.Sort([](const ACSHouseResizeHandleActor& A, const ACSHouseResizeHandleActor& B)
	{
		return A.GetEdgeIndex() < B.GetEdgeIndex();
	});
	const int32 Keep = FMath::Min(Cones.Num(), NumEdges);
	for (int32 Edge = 0; Edge < Keep; ++Edge)
	{
		Cones[Edge]->InitializeHandle(this, Edge);
#if WITH_EDITOR
		Cones[Edge]->SetActorLabel(FString::Printf(TEXT("%s_ResizeHandle_%d"), *GetActorLabel(), Edge));
#endif
	}
	for (int32 Index = Keep; Index < Cones.Num(); ++Index)
	{
		// **先摘表再销毁**：`Destroy` 会同步走到抓手的 `Destroyed` → `NotifyResizeHandleDestroyed`，
		// 表里还有它的话会被当成"模式里最后一个抓手没了"去判退出。
		ResizeHandles.Remove(Cones[Index]);
		Cones[Index]->Destroy();
	}
	for (int32 Edge = Keep; Edge < NumEdges; ++Edge) SpawnEdgeHandle(Edge);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s resize cones re-synced to %d edges (had %d)"),
		*GetName(), NumEdges, Cones.Num());

	// 边数变了，框的包围盒、其余锥子的规范位置多半也变了。
	SnapResizeHandles();
}

void ACSHouseActor::ExitResizeMode()
{
	if (ResizeHandles.Num() == 0) return;

	// 先把表清空再销毁：`Destroy()` 会同步走到抓手的 `Destroyed()`，那边回调
	// `NotifyResizeHandleDestroyed` —— 表还在的话会在循环中途被改，且退出事件要广播四次。
	TArray<TObjectPtr<ACSHouseHandleActor>> Doomed = MoveTemp(ResizeHandles);
	ResizeHandles.Reset();

	for (const TObjectPtr<ACSHouseHandleActor>& Handle : Doomed)
	{
		if (IsValid(Handle)) Handle->Destroy();
	}

	OnResizeModeChanged.Broadcast(this, false);
}

void ACSHouseActor::SnapResizeHandles()
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		// `SnapToCanonical` 是基类虚函数：推墙箭头摆回墙外、框摆回檐口并重算四条边。
		// 房子在这里**不区分**是哪一种抓手 —— 那正是公共基类的意义。
		if (!IsValid(Handle)) continue;
		Handle->SnapToCanonical();
	}
}

void ACSHouseActor::NotifyResizeHandleDestroyed(ACSHouseHandleActor* Handle)
{
	// 表里没有它 = `ExitResizeMode` 已经摘干净并广播过了，这里再广播就是第二次。
	if (ResizeHandles.Remove(Handle) == 0) return;

	if (ResizeHandles.Num() == 0)
	{
		OnResizeModeChanged.Broadcast(this, false);
	}
}

TArray<ACSHouseHandleActor*> ACSHouseActor::GetResizeHandles() const
{
	TArray<ACSHouseHandleActor*> Out;
	Out.Reserve(ResizeHandles.Num());
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		if (IsValid(Handle)) Out.Add(Handle);
	}
	return Out;
}

TArray<ACSHouseResizeHandleActor*> ACSHouseActor::GetEdgeHandles() const
{
	TArray<ACSHouseResizeHandleActor*> Out;
	Out.Reserve(ResizeHandles.Num());
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		if (ACSHouseResizeHandleActor* Edge = Cast<ACSHouseResizeHandleActor>(Handle)) Out.Add(Edge);
	}
	return Out;
}

ACSHouseHeightHandleActor* ACSHouseActor::GetHeightHandle() const
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseHeightHandleActor* Height = Cast<ACSHouseHeightHandleActor>(Handle);
		if (IsValid(Height) && Height->GetSide() == ECSHouseHeightHandleSide::Eave) return Height;
	}
	return nullptr;
}

ACSHouseHeightHandleActor* ACSHouseActor::GetBaseHandle() const
{
	for (const TObjectPtr<ACSHouseHandleActor>& Handle : ResizeHandles)
	{
		ACSHouseHeightHandleActor* Base = Cast<ACSHouseHeightHandleActor>(Handle);
		if (IsValid(Base) && Base->GetSide() == ECSHouseHeightHandleSide::Base) return Base;
	}
	return nullptr;
}

// -----------------------------------------------------------------------------
// Geometry
// -----------------------------------------------------------------------------

void CSHouse_BuildBodySoup(const FCSHouseBodyDesc& Desc, FCSGpuMeshCPUData& S)
{
	// ---- 墙：房子是一圈**围合的墙**（外皮 = footprint），走墙的面板规划 + `Faceted` 出面 ----
	//
	// 2026-09-22「统一房子和墙的逻辑」：原来这里逐边的那一整套（洞按格切面板、墩跨度、接缝段合并、斜接棱柱、窗台盒、房底）
	// 逐字搬进了 `CSWall::BuildBody`，样条墙走同一个面板规划器（`Continuous` 出面）。房子这边的画面逐位不变 ——
	// `Wall.HouseBodyMatchesLegacy` 拿冻结的旧实现逐个 float 对照。墙顶是**平顶**（墙体自己的顶面，TG 的房子墙没有压顶 /
	// 垛口，见 `CSWall::FTopParams::Plain`）；平屋顶的露台垛口归屋顶（`RebuildFlatRoof`）。
	FCSWallPath Ring;
	FCSWallSkins Skins;
	if (CSWall::BuildEnclosureRing(Desc.Footprint, Desc.WallThickness, 0.0, double(Desc.WallHeight), Ring, Skins))
	{
		CSWall::FBodyParams Body;
		Body.Surface = ECSWallSurface::Faceted;
		Body.World = Desc.World;
		Body.Openings = Desc.Openings;
		Body.SeamCuts = Desc.SeamCuts;
		Body.PierWidth = Desc.PierWidth;
		Body.bInteriorFloor = Desc.bBottomFace;   // 2026-09-17「房子产生底面」：墙内皮围出的那块，朝下的单面
		CSWall::BuildBody(Ring, Skins, Body, S);
	}

	FCSWallMeshWriter Writer{ S, Desc.World };
	const float H = Desc.WallHeight;

	// ---- 山墙（2026-10-02）：脊与房子一样长时，两端的墙沿屋面升成三角形 ----
	//
	// TG：`Roof::is_gable`（脊长参数 = 1）时发 `RecalculateGableRoofWallHeights`，那两面墙的砖一直砌到屋面底下。
	// 这里是墙顶之上的一块实心棱柱：外皮、内皮各一片（与下面的墙板共面、同一套 (S, Z) 贴图坐标，贴图连着铺上去），
	// 上沿逐段是斜面，照着 `Profile` 走 —— 那条线由屋面求值器沿墙线取，所以墙顶逐点贴着屋面。
	// 不斜接：山墙压在整条边上（含两头的转角方块），两头高度为 0，没有端面可露。洞不上山墙（窗的谓词卡在檐口以下）。
	for (const FCSHouseGableWall& Gable : Desc.Gables)
	{
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Gable.EdgeIndex, Desc.Footprint, Desc.WallThickness);
		if (F.Len <= 0.0f || Gable.Profile.Num() < 2) continue;
		const FVector U(F.U.X, F.U.Y, 0.0), In(F.In.X, F.In.Y, 0.0);
		const FVector Start(F.Start.X, F.Start.Y, 0.0);
		const FVector Deep = In * double(Desc.WallThickness);
		Writer.SetPanel(Start, U, FCSOpeningClipField(), ECSHousePart::Wall, 0);

		auto Point = [&](double Along, double Rise) { return Start + U * Along + FVector(0.0, 0.0, double(H) + Rise); };
		auto UV = [&](double Along, double Rise) { return FVector2f(float(Along) / CSHouse_UVScale, float(double(H) + Rise) / CSHouse_UVScale); };
		// 墙板做到哪：墙顶线往下 `PlasterDrop`（阶梯山墙上沿那一圈交给砖时），不低于墙顶。
		auto Plaster = [&](const FVector2D& P) { return FVector2D(P.X, FMath::Max(P.Y - double(Gable.PlasterDrop), 0.0)); };
		constexpr int32 SlotWall = 0;
		for (int32 Index = 0; Index + 1 < Gable.Profile.Num(); ++Index)
		{
			const FVector2D A = Plaster(Gable.Profile[Index]), B = Plaster(Gable.Profile[Index + 1]);
			if (B.X - A.X <= UE_KINDA_SMALL_NUMBER) continue;
			const FVector A0 = Point(A.X, 0.0), B0 = Point(B.X, 0.0), A1 = Point(A.X, A.Y), B1 = Point(B.X, B.Y);

			// 外皮（法线 = U × Up = −In，朝房外）与内皮（反绕）。高度为 0 的那一头三角形自然退化，不单独判。
			if (B.Y > UE_KINDA_SMALL_NUMBER)
			{
				Writer.AddTri(A0, B0, B1, SlotWall, UV(A.X, 0.0), UV(B.X, 0.0), UV(B.X, B.Y));
				Writer.AddTri(A0 + Deep, B1 + Deep, B0 + Deep, SlotWall, UV(A.X, 0.0), UV(B.X, B.Y), UV(B.X, 0.0));
			}
			if (A.Y > UE_KINDA_SMALL_NUMBER)
			{
				Writer.AddTri(A0, B1, A1, SlotWall, UV(A.X, 0.0), UV(B.X, B.Y), UV(A.X, A.Y));
				Writer.AddTri(A0 + Deep, A1 + Deep, B1 + Deep, SlotWall, UV(A.X, 0.0), UV(A.X, A.Y), UV(B.X, B.Y));
			}
			// 上沿的斜面（朝上，贴在瓦底下）。
			Writer.AddQuad4(A1, B1, B1 + Deep, A1 + Deep, SlotWall);
		}
		// 两头：`Profile` 在矩形上首尾都是 0 高；不是 0 的（给了别的上沿）才有端面要封。
		const FVector2D First = Plaster(Gable.Profile[0]), Last = Plaster(Gable.Profile.Last());
		if (First.Y > UE_KINDA_SMALL_NUMBER)
		{
			Writer.AddQuad4(Point(First.X, 0.0), Point(First.X, First.Y), Point(First.X, First.Y) + Deep, Point(First.X, 0.0) + Deep, SlotWall);
		}
		if (Last.Y > UE_KINDA_SMALL_NUMBER)
		{
			Writer.AddQuad4(Point(Last.X, 0.0), Point(Last.X, 0.0) + Deep, Point(Last.X, Last.Y) + Deep, Point(Last.X, Last.Y), SlotWall);
		}
	}

	// ---- 屋顶：坡面全是瓦片。房体三角汤里**一片屋面都不产**（2026-08-31）。 ----
	//
	// 这里原本是「两块实体坡板 + 两端山墙棱柱 + 两条檐口封口楔形」那一整套双坡结构，整段删除：
	// TG 的屋顶是**四个坡面**、整面**由瓦铺成**（实拍俯视 + `roof_shape::ridge_length_01_from_
	// rectangle_ratio`），既没有山墙这个构件，屋面也不是实体板。连带作废的还有"墙顶该砌到哪"
	// 那条纪律（咬入量 / `SoffitTopZ` / 封口楔形）—— 没有板底可咬，四面墙顶一律平在 WallHeight，
	// 墙顶与屋面之间那条缝在 TG 里本来就是露着的（室内实拍可见漏光）。
	//
	// 屋面几何改由**瓦片实例**承担（`Content/HouseTest/TinyGladeAsset/Meshes/roof_tile`），走 GPU 实例组件那条路
	// （与藤蔓 / 门框砖同构），不进这份三角汤。屋面方程仍然只有 `CSHouseRoof.h` 一份真源：
	// 铺瓦、铺梁、尖顶、雪、以及 D8「落屋顶 → 不生成」谓词全部调它。

	if (Desc.bFlatRoof)
	{
		Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(), ECSHousePart::Roof, 0);
		auto DeckPoint = [&](int32 Corner)
		{
			const FVector2D XY = CSHouse_GetCorner(Corner, Desc.Footprint).Point;
			return FVector(XY.X, XY.Y, H + 0.5f); // 避开墙板上沿的共面三角。
		};
		auto DeckUV = [](const FVector& P) { return FVector2f(float(P.X) / CSHouse_UVScale, float(P.Y) / CSHouse_UVScale); };
		for (int32 K = 1; K + 1 < Desc.Footprint.NumEdges(); ++K)
		{
			const FVector A = DeckPoint(0), B = DeckPoint(K), C = DeckPoint(K + 1);
			Writer.AddTri(A, B, C, 1, DeckUV(A), DeckUV(B), DeckUV(C));
		}
	}

	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;
	S.NumTexCoordChannels = 2;   // UV0 贴图 / UV1 解析裁剪场
}

void ACSHouseActor::BuildGableWalls(TArray<FCSHouseGableWall>& Out) const
{
	Out.Reset();
	const FCSRoofDesc Roof = GetRoofDesc();
	if (Roof.bFlat || !Roof.HasGable()) return;

	// 上沿 = 屋面沿墙线的那条剖面线（等效内距 × tan(pitch) = 屋面比檐口高出多少），从墙顶起算。
	//  · 悬挑式（TG 的 c = 0）：墙顶贴着屋面线。屋面整体的 `RoofHeightOffset` 不加进来 —— 山墙顶与瓦底之间留的那条缝，
	//    和檐墙顶与瓦底之间的是同一条；两头高度为 0，与檐墙顶齐平。
	//  · 阶梯式（TG 的 c = 0.46 m）：墙顶比**屋面线**（含 `RoofHeightOffset`）高出 46 cm，两头也抬着 ——
	//    山墙像一道女儿墙立在屋面两头，屋面缩在它里面（`CSHouseRoof_GableOverhang` 的负值）。
	//    那 46 cm 在 TG 里是露着的砖（灰泥只抹到屋面线）：有砖可出时墙板就只做到屋面线（`PlasterDrop`），
	//    上面由砖逐层收成台阶；没有砖可出时墙板自己升到墙顶线。
	const double TanP = double(Roof.TanPitch());
	const bool bStepped = GableRoofStyle == ECSGableRoofStyle::Stepped;
	const double Lift = bStepped ? double(RoofHeightOffset) + double(CSHouseRoof_SteppedGableRise) : 0.0;
	const float PlasterDrop = bStepped && HasSteppedGableBricks() ? CSHouseRoof_SteppedGableRise : 0.0f;
	for (int32 Edge = 0; Edge < Roof.Footprint.NumEdges(); ++Edge)
	{
		FCSHouseGableWall Gable;
		if (!CSHouseRoof_BuildGableProfile(Roof, Edge, Gable.Profile)) continue;
		for (FVector2D& Point : Gable.Profile) Point.Y = FMath::Max(Point.Y * TanP + Lift, 0.0);
		Gable.EdgeIndex = Edge;
		Gable.PlasterDrop = PlasterDrop;
		Out.Add(MoveTemp(Gable));
	}
}

bool ACSHouseActor::HasSteppedGableBricks() const
{
	if (GableRoofStyle != ECSGableRoofStyle::Stepped || !bFrameEnabled || !FrameBrickMesh) return false;
	return bBrickWallEnabled || (bTrimEnabled && bTrimGable);
}

void ACSHouseActor::RebuildBodyMesh()
{
	FCSHouseBodyDesc Desc;
	Desc.Footprint = GetFootprint();
	Desc.WallThickness = WallThickness;
	Desc.WallHeight = WallHeight;
	Desc.PierWidth = PierWidth;
	Desc.Openings = CurrentOpenings;
	Desc.SeamCuts = CurrentSeamCuts;
	Desc.bBottomFace = bBottomFace;
	Desc.bFlatRoof = IsFlatRoof();
	Desc.World = GetBuildTransform();
	BuildGableWalls(Desc.Gables);

	FCSGpuMeshCPUData S;
	CSHouse_BuildBodySoup(Desc, S);

	const int32 TriangleCount = S.Indices.Num() / 3;
	BodySlot.BuiltAt = Desc.World;
	SubmitBodyMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s body rebuilt: tris=%d openings=%d (doors=%d)"),
		*GetName(), TriangleCount, CurrentOpenings.Num(), GetOpenDoorCount());
}

void ACSHouseActor::SubmitBodyMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	FCSMeshSlotUpload Upload;
	// ⚠️ 槽 1（屋顶）现在**一个三角都没有**（四坡改瓦片实例）—— 空分段只是画不出东西，无害；
	// 槽位留着是为了不动这张 P2 冻结的槽表，`RoofMaterial` 本身归瓦片那条路用。
	Upload.Materials = { WallMaterial, FlatRoofMaterial ? FlatRoofMaterial : WallMaterial };
	Upload.NumTexCoordSets = 2;   // UV1 传裁剪场
	Upload.bSortSections = true;
	SubmitMeshSlotAsync(TinyGladeMeshComponent, TinyGladeMesh, BodySlot, Snapshot, Upload, [this]() { OnBodyEditComplete(); });
}

void ACSHouseActor::OnBodyEditComplete()
{
	// 最新态合并：在途期间攒下的最后一份目标现在补发。
	if (const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = TakePending(BodySlot.Pending))
	{
		SubmitBodyMesh(Next);
		return;
	}

	// 在途期间被推迟的摆位增量在这里补上。没有这一步，"拖动中恰好撞上一次形状重建"的那一帧
	// 位移就永远丢了 —— 而快扫看到的变换没再变，不会来第二次唤醒。增量相对 BodySlot.BuiltAt
	// 算，所以补送的是累计量，不是重放。
	if (ApplyBodyPlacement()) BodySlot.PlacementHash = ComputePlacementHash();
}

bool ACSHouseActor::ApplyBodyPlacement()
{
	return ApplyMeshSlotPlacement(GetTinyGladeMesh(), BodySlot, GetBuildTransform(), [this]() { OnBodyEditComplete(); });
}

namespace
{
/**
 * 交接包围盒从**构建空间**（`GetBuildTransform()`，只取 yaw）映射到**组件空间**（实例原点所在的空间）。
 * 房子没有 pitch / roll / 缩放时是恒等变换，逐位不改现状；有的话不映射，剔除盒就偏、视锥边缘闪
 * （2026-09-07 审查 B5：五家里以前只有门框砖做了这一步）。只有 yaw 时也因此不会多出一次交接。
 */
FBox CSHouse_BuildBoundsToComponent(const FTransform& BuildTransform, const USceneComponent* Component, const FBox& Bounds)
{
	if (!Component || !Bounds.IsValid) return Bounds;
	const FTransform BuildToComponent = BuildTransform * Component->GetComponentTransform().Inverse();
	return BuildToComponent.Equals(FTransform::Identity, 1.0e-4) ? Bounds : Bounds.TransformBy(BuildToComponent);
}
}

void ACSHouseActor::EnsurePillarBrickComponent()
{
	// 新组件身上没有实例源，交接缓存必须一起作废 —— 缓存说"交接过了"而组件是空的，
	// 下一趟就会被判成稳态而跳过，画面永远空白且不报错。
	if (CSShaperSteps::EnsureInstancedComponent(this, PillarBrickComponent)) PillarHandover.Capacities.Reset();
	// 空 = 画砖网格资产自己的材质（逐 section）；设了才整体覆盖。变了才让代理重建。
	PillarBrickComponent->SetInstanceMaterial(PillarMaterial);
	PillarBrickComponent->SetBaseMesh(PillarBrickMesh);   // 同一张网格时内部直接早退

	if (PillarGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(PillarGpuBuffers);
		PillarGpuBuffers.SetNum(1);
		PillarHandover.Capacities.Reset();
	}

	// 剔除球与三轴块尺寸都从基础网格的包围盒推。⚠️ `brick` 是 1×1×1 的**居中**字典 mesh，
	// 所以 BlockSize 恒等于 (1,1,1) —— 记录里的轴已经带着真实尺寸了（见 CSHousePillar.usf）。
	// 留着这一步是为了换资产时不用改 kernel。
	const FBox Local = PillarBrickMesh ? PillarBrickMesh->GetBoundingBox() : FBox(ForceInit);
	PillarGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	PillarGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;
	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	PillarGpuBuffers[0].BlockSize = FVector3f(
		float(1.0 / FMath::Max(MeshSize.X, UE_KINDA_SMALL_NUMBER)),
		float(1.0 / FMath::Max(MeshSize.Y, UE_KINDA_SMALL_NUMBER)),
		float(1.0 / FMath::Max(MeshSize.Z, UE_KINDA_SMALL_NUMBER)));

	if (!PillarBrickMesh || !PillarBrickComponent) return;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律，同门框砖 / 藤蔓）。
	// 上限 = 周界柱位数 × 每根最多几层。柱长由地面空隙定，没有上界，所以层数要自己钉一个：
	// 500 cm 的悬空已经远超任何合理场景，再高就截断 —— 少砌几块砖远好过在拖动的某一帧
	// 付一次设备同步。
	const FCSHouseFootprint Footprint = GetFootprint();
	const double Perimeter = Footprint.GetPerimeter();
	const int32 MaxPillars = FMath::CeilToInt(Perimeter / FMath::Max(PillarSpacing, 20.0f)) + Footprint.NumEdges();
	const int32 MaxCourses = FMath::CeilToInt(500.0f / FMath::Max(PillarCourseHeight, 4.0f));
	const uint32 MaxBricks = uint32(FMath::Clamp(
		CSShaperSteps::ReserveCount(MaxPillars * MaxCourses), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(PillarGpuBuffers, MaxBricks);

	// 交接包围盒：量化 + 只涨不缩（理由同门框砖那段 —— 拖尺寸时 1 cm 阈值会让每帧重走一次
	// 阻塞的 SetInstanceSourceGPU）。柱子往**下**长，所以下界要给足。
	const double Reach = CSShaperSteps::QuantizeUp(Footprint.GetCenteredSpan() * 0.6 + PillarSize);
	const double Depth = CSShaperSteps::QuantizeUp(500.0 + PillarEmbed);
	FBox LocalBounds(FVector(-Reach, -Reach, -Depth), FVector(Reach, Reach, PillarSize));
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), PillarBrickComponent, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, PillarHandover, bForceFullRebuild);
	// 柱砖的 kernel（CSHousePillar.usf）不写 custom data，柱材质也不读：不交，省掉剔除 pass 里逐可见槽
	// 抄一遍全零的那一趟（2026-09-07 审查 B1 的柱子那条：以前交的是一块从未被 kernel 写过的 buffer）。
	CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(PillarBrickComponent, PillarGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, PillarHandover);
}

void ACSHouseActor::RebuildPillarMesh(const TArray<FVector>& Centers, const TArray<float>& Lengths)
{
	// ── 砖石柱（2026-09-06 裁决）────────────────────────────────────────────────
	if (bPillarUseBricks && PillarBrickMesh)
	{
		// ⚠️ 另一条必须显式清掉，否则方盒与砖同时画 —— 藤蔓换管子时正是漏了这一条。
		// 连在途待发的那份方盒一起作废（ClearMeshSlot），否则完成回调会把它补发回来。
		ClearMeshSlot(PillarMeshComponent, PillarMesh, PillarSlot.Pending);

		EnsurePillarBrickComponent();
		if (!PillarGpuBuffers.Num() || !PillarGpuBuffers[0].IsValid()) return;

		CSHousePillar::FParams PillarParams;
		PillarParams.BrickWidth = PillarSize;
		PillarParams.CourseHeight = PillarCourseHeight;
		PillarParams.YawJitter = PillarYawJitter;
		PillarParams.BracketCourses = PillarBracketCourses;
		PillarParams.BracketOverhang = PillarBracketOverhang;
		PillarParams.Seed = VineSeed;   // 与藤共用用户种子：同一栋房子的随机看得出同源

		TArray<CSHousePillar::FBrick> Bricks;
		CSHousePillar::BuildBricks(Centers, Lengths, GetBuildTransform(), PillarParams, Bricks);

		// ⚠️ 用**组件自己的变换**求逆，不用 actor 的（同藤蔓那条：房子被 pitch/roll
		// 或缩放时 actor 的完整逆变换与 GetBuildTransform 的只取 yaw 会打架）。
		const FMatrix44f WorldToComponent = FMatrix44f(
			PillarBrickComponent->GetComponentTransform().ToInverseMatrixWithScale());
		CSHousePillar::Pack(Bricks, PillarGpuBuffers[0], WorldToComponent);

		UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars rebuilt: count=%d bricks=%d"),
			*GetName(), Centers.Num(), Bricks.Num());
		return;
	}

	// ── 旧路：摞方盒（占位，砖那条定型后连同开关一起删）──────────────────────
	// 反向也要清：从砖切回方盒时砖的 counter 得归零，否则两套并存。
	if (PillarGpuBuffers.Num() && PillarGpuBuffers[0].IsValid()) CSShaperSteps::ZeroCounters(PillarGpuBuffers);

	if (!PillarMeshComponent) return;
	if (Centers.IsEmpty())
	{
		// 下次有柱时重建，别让摆位快路径去搬一份空网格；在途待发的旧柱一并作废。
		ClearMeshSlot(PillarMeshComponent, PillarMesh, PillarSlot.Pending);
		UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars cleared"), *GetName());
		return;
	}

	FCSGpuMeshCPUData S;
	const FTransform BuildTransform = GetBuildTransform();
	FCSWallMeshWriter Writer{ S, BuildTransform };
	// 柱子也走 `SetPanel` —— 写法只留一种，下一个人就不会再把 B 通道漏成 0（= 合法形状 id
	// `Arch`）。这里的写手是新的，裁剪场本来就是无洞哨兵，所以这一句纯粹是把口径钉死。
	Writer.SetPanel(FVector::ZeroVector, FVector::ForwardVector, FCSOpeningClipField(),
		ECSHousePart::Pillar, 0);
	for (int32 i = 0; i < Centers.Num(); ++i)
	{
		const FVector C = Centers[i];
		const float Len = Lengths[i];
		Writer.AddBox(FVector(C.X - PillarSize * 0.5f, C.Y - PillarSize * 0.5f, -Len),
			FVector(PillarSize, 0, 0), FVector(0, PillarSize, 0), FVector(0, 0, Len), 0);
	}
	S.SourceSpace = FCSGpuMeshCPUData::ESpace::World;
	S.AttrLayout = FCSGpuMeshCPUData::EAttrLayout::PerVertex;

	const int32 TriangleCount = S.Indices.Num() / 3;
	PillarSlot.BuiltAt = BuildTransform;
	SubmitPillarMesh(MakeShared<FCSGpuMeshCPUData, ESPMode::ThreadSafe>(MoveTemp(S)));
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s pillars rebuilt: count=%d tris=%d"), *GetName(), Centers.Num(), TriangleCount);
}

namespace
{
/**
 * 方盒柱（`bPillarUseBricks` 关掉时的占位路）画什么材质：`PillarMaterial` 设了就用它；空 = 砖柱那张网格资产
 * 0 号材质槽上挂的材质。方盒是砖柱的替身，这条路上没有自己的资产，"空 = 用资产材质"就取它顶替的那一张
 * —— 否则清掉 `PillarMaterial` 之后切到方盒对照会是一片默认灰（2026-09-15）。
 */
UMaterialInterface* CSHouse_PillarBoxMaterial(const ACSHouseActor& House)
{
	if (House.PillarMaterial) return House.PillarMaterial;
	return House.PillarBrickMesh ? House.PillarBrickMesh->GetMaterial(0) : nullptr;
}

/**
 * 普通静态网格组件（尖顶 / 门扇）的材质口径，与实例组件的 `InstanceMaterial` 同一个意思：
 * `Override` 设了就盖住**每一个**材质槽；空 = 清掉组件上的覆盖，逐槽画网格资产自己挂的材质。
 * 以前只写 0 号槽、留空时什么都不做 —— 多槽资产（`balcony_door_rank*` = 木 + 铁）只盖了一半，
 * 而复用的组件在属性被清空之后还带着上一次写进去的那张（2026-09-15）。调用前必须已经 SetStaticMesh。
 */
void CSHouse_ApplyMeshComponentMaterial(UStaticMeshComponent& Component, UMaterialInterface* Override)
{
	if (!Override)
	{
		if (Component.OverrideMaterials.Num() > 0) Component.EmptyOverrideMaterials();
		return;
	}
	for (int32 Slot = 0; Slot < Component.GetNumMaterials(); ++Slot)
	{
		if (Component.GetMaterial(Slot) != Override) Component.SetMaterial(Slot, Override);
	}
}

/** 普通静态网格组件每个材质槽都有材质吗；空串 = 都有。`Label` 是原因串前缀（"第 0 根" / "门扇 1"）。 */
FString CSHouse_MeshComponentMaterialReason(const UStaticMeshComponent& Component, const FString& Label)
{
	for (int32 Slot = 0; Slot < Component.GetNumMaterials(); ++Slot)
	{
		// **石阶那个坑的同一条**：材质槽为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		if (!Component.GetMaterial(Slot)) return FString::Printf(TEXT("%s：%d 号材质槽是空的（会画成一片灰）"), *Label, Slot);
	}
	return FString();
}
}

void ACSHouseActor::SubmitPillarMesh(TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Snapshot)
{
	// 柱子只有一个材质槽，不需要排序分段 —— 一次上传就是全部工作。容量同房体按台阶预留：
	// 柱数会随周界（=FootprintSize）跳变，按精确数要就是每次跳变一次阻塞重分配。
	FCSMeshSlotUpload Upload;
	Upload.Materials = { CSHouse_PillarBoxMaterial(*this) };
	SubmitMeshSlotAsync(PillarMeshComponent, PillarMesh, PillarSlot, Snapshot, Upload, [this]() { OnPillarEditComplete(); });
}

void ACSHouseActor::OnPillarEditComplete()
{
	if (const TSharedPtr<FCSGpuMeshCPUData, ESPMode::ThreadSafe> Next = TakePending(PillarSlot.Pending))
	{
		SubmitPillarMesh(Next);
		return;
	}
	if (ApplyPillarPlacement()) PillarSlot.PlacementHash = ComputePlacementHash();
}

// 藤的生长相位（`CSHouseVine::ResolveSpawnTimes`）、管子递交（`SubmitVineTube` / `OnVineTubeEditComplete`）
// 2026-09-22 起在墙的基类 `ACSWallBase` 里，房子与样条墙共用一份（原文逐字搬过去了）。

bool ACSHouseActor::ApplyPillarPlacement()
{
	return ApplyMeshSlotPlacement(PillarMesh, PillarSlot, GetBuildTransform(), [this]() { OnPillarEditComplete(); });
}

// -----------------------------------------------------------------------------
// Frame（门框砖）
// -----------------------------------------------------------------------------

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureFrameComponent()
{
	CSShaperSteps::EnsureInstancedComponent(this, FrameComponent);
	// 空 = 画砖网格资产自己的材质；设了才整体覆盖。转角墩要剔的砖靠组件级的"负随机数 = 隐藏"契约藏
	// （剔除 pass / GPU-Scene 写入），不再依赖 `M_TinyGladeBrick` 的 OpacityMask，所以换成资产材质也不会冒出来。
	FrameComponent->SetInstanceMaterial(FrameMaterial);
	FrameComponent->SetBaseMesh(FrameBrickMesh);   // 同一张网格时内部直接早退

	if (FrameGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(FrameGpuBuffers);
		FrameGpuBuffers.SetNum(1);
		FrameHandover.Capacities.Reset();
	}

	// 剔除球与三轴块尺寸都从基础网格的包围盒推：TG 的 brick 是 100³ 居中盒，所以
	// BlockSize = 想要的尺寸 / 网格自身尺寸，最终实例尺寸才等于我们写的那几个 cm 值。
	const FBox Local = FrameBrickMesh ? FrameBrickMesh->GetBoundingBox() : FBox(ForceInit);
	FrameGpuBuffers[0].BaseSphereCentre = Local.IsValid ? FVector3f(Local.GetCenter()) : FVector3f::ZeroVector;
	FrameGpuBuffers[0].BaseSphereRadius = Local.IsValid ? float(Local.GetExtent().Size()) : 0.0f;

	const FVector MeshSize = Local.IsValid ? Local.GetSize() : FVector(1.0);
	// 自动档：墙厚 + 两面各凸 `FrameBrickProtrude`（理由见那个字段的注释）。显式给了厚度就照给的用。
	const float Thickness = FrameBrickThickness > 0.5f ? FrameBrickThickness : WallThickness + 2.0f * FrameBrickProtrude;
	// 长度轴乘胀大系数（TG 那一步，理由与轴的对位见 FrameBrickBloat 的字段注释）：砖心由
	// `CSHouseFrame::SolveRun` 按未胀大的 FrameBrickLength 定下、这里只放大**渲染尺寸**，
	// 相邻砖因此必然互相穿插。别改成把系数折进砖长 —— 那会连砖数和位置一起变，就不是"胀大"
	// 而是"砖变长"，块数跳变照样露缝。
	const float Bloat = FMath::Max(FrameBrickBloat, 1.0f);
	FrameGpuBuffers[0].BlockSize = FVector3f(
		float(FrameBrickDepth / FMath::Max(MeshSize.X, 1.0)),
		float(FrameBrickLength * Bloat / FMath::Max(MeshSize.Y, 1.0)),
		float(Thickness / FMath::Max(MeshSize.Z, 1.0)));

	if (!FrameBrickMesh || !FrameComponent) return CSShaperSteps::EHandoverResult::UpToDate;

	// **容量与实例源交接都在这里一次付清** —— 两者都是阻塞的（前者要在渲染线程分配，
	// 后者内部走 SetStreamLayoutSync + 立刻重建 render state）。留给 RebuildFrame 去做的话，
	// 它们会落在"画路的某一笔"上：那一笔恰好是第一次长出砖、或砖数第一次超过容量的那一笔，
	// 取决于用户画到哪里，既掉帧又难复现。这里做完，交互期的 RebuildFrame 就只剩录 pass。
	CSShaperSteps::ReserveCapacity(FrameGpuBuffers, uint32(EffectiveFrameCapacity()));

	// 实例源的包围盒是**组件空间**的保守盒，且必须自己把基础网格的尺寸算进去（packed 路径不代劳）。
	//
	// 量化 + 只涨不缩（理由与保证见 CSShaperSteps::QuantizeUp 上面那段）：原来这两个量直接就是
	// FootprintSize / WallHeight 的连续函数，而下面 bNeedHandover 的阈值是 1 cm ——
	// 拖尺寸时每帧都判"包围盒变了"，于是每帧重走一次阻塞的 SetInstanceSourceGPU。
	// 全量重建时不并旧盒，让它重新收紧（否则一次误拉大就永久留着保守盒）。
	const double Reach = CSShaperSteps::QuantizeUp(GetFootprint().GetCenteredSpan() * 0.6 + FrameBrickLength);
	const double Top = CSShaperSteps::QuantizeUp(WallHeight + FrameBrickLength);
	FBox LocalBounds(FVector(-Reach, -Reach, -FrameBrickLength), FVector(Reach, Reach, Top));

	// ⚠️ 这个盒是按 footprint 在**构建空间**（`GetBuildTransform()`，只取 yaw）里量的，而实例
	// 原点存在**组件空间**里 —— 两者只有在房子没有 pitch/roll/缩放时才重合。状态文件
	// 「已知潜伏问题」那条说的就是这个不对称（裁决一要求"按同一个变换口径写死"）：
	// 盒子跟着做一次 构建空间 → 组件空间 的映射，剩下的两处口径就自洽了（五家同一个 helper）。
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), FrameComponent, LocalBounds);

	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, FrameHandover, bForceFullRebuild);
	// 门框砖没有生长动画，不吃逐实例 custom data —— 交了只会让剔除 pass 多抄一遍全零。
	return CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(FrameComponent, FrameGpuBuffers[0], /*bWithCustomData*/ false),
		LocalBounds, FrameHandover);
}

uint32 ACSHouseActor::BuildFrameArches(TArray<CSHouseFrame::FElement>& OutElements, int32& OutBrickCount) const
{
	OutElements.Reset();
	OutBrickCount = 0;
	if (!bFrameEnabled || !FrameBrickMesh || CurrentOpenings.IsEmpty()) return 0;

	const FTransform World = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();
	const float T = WallThickness;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	Params.CapitalScale = PierCapitalScale;     // 只有门框这条路出柱头；接缝柱 / 角石沿用默认 0
	Params.CapitalHeight = PierCapitalHeight;
	// **容量恒定**：注册期的 `ReserveCapacity` 已经一次付清，这里只按它截断、绝不扩容。
	// 门框这条路上因此没有任何一次可扩容调用 —— 那正是"交互热路径零设备同步"这条纪律要的：
	// 扩容是阻塞刷新，落在用户恰好画到的那一笔上。
	Params.MaxBricks = EffectiveFrameCapacity();

	// `CurrentOpenings` 进来时已按 (边, CenterS) 排好（`ComputeDoors` 末尾那一下），同一条边的
	// 洞因此是**连续片段**；墩的样式位（`ResolvePierSpans`）也建立在同一个顺序上。别在这里重排。
	int32 Begin = 0;
	while (Begin < CurrentOpenings.Num())
	{
		int32 End = Begin;
		while (End < CurrentOpenings.Num() && CurrentOpenings[End].EdgeIndex == CurrentOpenings[Begin].EdgeIndex) ++End;

		// **与房体面板同一份 `CSHouse_GetEdge`**：墙在哪儿只能有一个真源（同 BuildVineStrips）。
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(CurrentOpenings[Begin].EdgeIndex, Footprint, T);
		// 砖路走**墙厚正中**：砖的 +Z 尺寸 = 墙厚时，正好把断口两侧都封住（同旧路）。
		const FVector Mid(F.Start.X + F.In.X * (T * 0.5), F.Start.Y + F.In.Y * (T * 0.5), 0.0);

		CSHouseFrame::FWallFrame Frame;
		Frame.Origin = FVector3f(World.TransformPosition(Mid));
		Frame.AxisU = FVector3f(World.TransformVectorNoScale(FVector(F.U.X, F.U.Y, 0.0))).GetSafeNormal();
		Frame.AxisV = FVector3f(World.TransformVectorNoScale(FVector::UpVector)).GetSafeNormal();
		Frame.AxisN = (-FVector3f(World.TransformVectorNoScale(FVector(F.In.X, F.In.Y, 0.0)))).GetSafeNormal();

		OutBrickCount += CSHouseFrame::BuildEdgeElements(Frame,
			MakeArrayView(CurrentOpenings.GetData() + Begin, End - Begin), Params, OutElements);
		Begin = End;
	}

	// 截断是**有意**的（容量恒定 ⇒ 不扩容 ⇒ 不阻塞），但必须出声：不然"墙太长/砖太小"时
	// 最后几条砖路会静默地少几块砖，看着像洞缘没砌完。抬 `FrameReserveCapacity` 即可。
	if (OutBrickCount >= Params.MaxBricks)
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s frame hit the constant capacity (%d bricks). Raise FrameReserveCapacity."),
			*GetName(), Params.MaxBricks);
	}

	// desc 哈希 = "会改变砖摆在哪儿的一切"。逐路只需要标量：砖数、弧长、铺装缩放、路的形状，
	// 逐砖的位置是它们的纯函数（正是解析推导的定义），所以不必、也无从逐砖入哈希。
	TArray<int32> Hash;
	for (const CSHouseFrame::FElement& E : OutElements)
	{
		Hash.Append({ E.BrickCount, CSHouse_Q(E.Path.TotalLen(), 1), CSHouse_Q(E.LayoutScale, 0.01),
			CSHouse_Q(E.Path.CenterS, 1), CSHouse_Q(E.Path.Radius, 1),
			CSHouse_Q(E.Path.BaseZ, 1), CSHouse_Q(E.Path.TopZ, 1),
			CSHouse_Q(E.Path.LeftS, 1), CSHouse_Q(E.Path.RightS, 1),
			int32(E.Path.MidKind) | (E.Path.bLeftJamb ? 0x10 : 0) | (E.Path.bRightJamb ? 0x20 : 0) });
	}
	// 胀大系数与三轴尺寸只改 `BlockSize`、一条路都不改，而 `RebuildFrame` 的早退门看的就是
	// 这个哈希 —— 不把它们算进来，改了系数就只会静默无效（与 D14 开篇 FrameMaterial 同型）。
	// `FrameSeed` 在单条目 palette 下已经不影响任何东西（`SolveRun` 里没有随机），留着是为了
	// 将来加 palette 时不会静默跳过重建。
	Hash.Append({ CSHouse_Q(PierCapitalScale, 0.01), CSHouse_Q(PierCapitalHeight, 0.5),
		CSHouse_Q(FrameBrickProtrude, 0.5),
		CSHouse_Q(FrameBrickLength, 0.5), CSHouse_Q(FrameBrickDepth, 0.5),
		CSHouse_Q(FrameBrickThickness, 0.5), CSHouse_Q(FrameBrickGap, 0.1),
		CSHouse_Q(FrameBrickBloat, 0.001), FrameSeed });
	return CSHouse_Hash(Hash);
}

void ACSHouseActor::RebuildFrame()
{
	const CSShaperSteps::EHandoverResult FrameHandoverResult = EnsureFrameComponent();

	TArray<CSHouseFrame::FElement> Elements;
	int32 BrickCount = 0;
	// 门框砖先、接缝砖后：全局砖序号跨两者连续（整栋房子一个 dispatch），而**次序是承重的** ——
	// 反过来的话每开一扇门都会把所有接缝砖的槽位推一格，从而推掉它们的逐实例随机数。
	// （接缝砖的随机数已经从接缝身份派生、与槽位无关，但门框砖那边仍然是槽位。）
	const uint32 ArchHash = BuildFrameArches(Elements, BrickCount);
	const uint32 SeamHash = BuildSeamBricks(Elements, BrickCount);
	// 转角墩（转角配成墩的角上那根柱础/柱身/柱头）：随洞表变，随机数从房子身份派生，
	// 排在门框砖之后任何位置都无害；放在角石之前是因为角石要在它的墩顶以下让路，读同一份高度。
	const uint32 CornerPierHash = BuildCornerPierBricks(Elements, BrickCount);
	// 角石最后：它的砖数只随 footprint / 墙高变，排在最后就不会因为开一扇门或来一个邻居
	// 而把别人的槽位推走（反过来它自己被推走无害 —— 随机数已从房子身份派生）。
	const uint32 QuoinHash = BuildQuoinBricks(Elements, BrickCount);
	// 包边最后：它随 footprint / 墙高 / 洞表变，排在最后就不会推走别人的槽位。
	const uint32 TrimHash = BuildTrimBricks(Elements, BrickCount);
	// 砖层**真的**最后：它是砖数最多的一家（一栋 6×4 m 的房约 1400 块），排在最后
	// ⇒ 撞容量上限时先截断的是它自己，门框 / 接缝 / 角石 / 包边一块都不少。
	const uint32 BrickWallHash = BuildBrickWallBricks(Elements, BrickCount);

	// **五家加起来**撞上限也要出声。单家那条警告（`BuildFrameArches` 里）只看得见自己，
	// 而砖层排在最后 ⇒ 真正被截掉的总是它，症状是"墙砌到一半"而所有断言全绿。
	if (BrickCount >= EffectiveFrameCapacity())
	{
		UE_LOG(LogTinyGladeHouse, Warning,
			TEXT("[TinyGladeHouse] %s 砖数撞上常驻容量（%d 块，其中砖层 %d）。容量注册期一次付清、"
				"**只截断不扩容** —— 把 FrameReserveCapacity 调大（上限 65536）再重开关卡。"),
			*GetName(), EffectiveFrameCapacity(), CurrentBrickWallBrickCount);
	}
	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的旧路混用
	// `GetBuildTransform()`（只取 yaw）与 `GetActorTransform().ToInverseMatrixWithScale()`
	// （完整变换），正是状态文件「已知潜伏问题」里那条不对称。这里与藤蔓/摆件同一个口径，
	// 三条路从此只有一份写法。**必须在哈希之前算**：早退门要拿它把墙框架量到同一个空间里。
	const FMatrix44f WorldToComponent = IsValid(FrameComponent)
		? FMatrix44f(FrameComponent->GetComponentTransform().ToInverseMatrixWithScale())
		: FMatrix44f::Identity;
	// 墙框架也进哈希：`FPath` 全是边局部量，长边不变而房子往垂直方向拉时它一个字都不变，
	// 而砖的组件空间位置已经变了。理由与实测症状见 `CSHouse_HashElementFrames` 的注释。
	const uint32 FrameHash = CSHouse_HashElementFrames(Elements, WorldToComponent);

	// ⚠️ **三个都是 0（这栋房一块砖都没有）时合出来的必须还是 0**，不能直接 `CSHouse_Hash({0,0,0})`
	// —— 那是一个非零常数，于是"没有砖"的房子每次重求值都判成"变了"：走一趟
	// `ClearInstanceSourceGPU()`（它把 `FrameHandover` 清空），下一次 `EnsureFrameComponent`
	// 就得重走阻塞的 `SetInstanceSourceGPU`。**实测代价是每轮 2 次阻塞刷新**，四条零阻塞断言
	// （画一笔 / 拖带柱的房子 / 带藤拖 / 带摆件拖 —— 全是没有门因而没有砖的那栋）当场从 0 变成 2。
	// 有砖的房子看不见这条，因为它的哈希本来就非零。
	// （砖表空时 `FrameHash` 恒为 0 —— `CSHouse_Hash` 对空表返回 0 —— 所以只判前两个就够，
	// 但仍把它写进合并里，免得将来有人改了空表约定而这里静默失配。）
	const uint32 NewHash = (ArchHash == 0 && SeamHash == 0 && CornerPierHash == 0 && QuoinHash == 0 && TrimHash == 0 && BrickWallHash == 0)
		? 0u : CSHouse_Hash({ int32(ArchHash), int32(SeamHash), int32(CornerPierHash), int32(QuoinHash), int32(TrimHash), int32(BrickWallHash), int32(FrameHash) });

	bool bBuffersReady = FrameGpuBuffers.Num() == 1 && FrameHandover.Capacities.Num() == 1;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : FrameGpuBuffers) bBuffersReady &= Buffers.IsValid();
	// 刚交接过（扩容换了清零的新 buffer，或包围盒变了）就必须重排一次：哈希没变也不能拿它当"砖还在"
	// （2026-09-07 审查 B1：以前这道门只看哈希与数量，扩容那一轮画的是池子残值）。有砖才强制 ——
	// 没砖时 counter 本来就该是 0，强制反而会走进下面的撤源分支，把交接缓存清空，下一轮再交、再撤，
	// 每轮白付两次阻塞（正是上面那段注释里那种失败形状）。
	const bool bMustRescatter = FrameHandoverResult == CSShaperSteps::EHandoverResult::HandedOver && BrickCount > 0;
	if (!bMustRescatter && NewHash == FrameDescHash && CurrentFrameBrickCount == BrickCount && (BrickCount == 0 || bBuffersReady)) return;

	FrameDescHash = NewHash;
	CurrentFrameBrickCount = BrickCount;
	// 越过早退门 = 这一趟砖真的会被重排（下面两条分支都会写 GPU）。判据见 GetFrameScatterCount。
	++FrameScatterCount;

	if (BrickCount == 0)
	{
		// ⚠️ **撤实例源之前必须先把 counter 清零。**
		//
		// 撤掉之后 `FrameHandover` 被清空，于是**下一轮 `EnsureFrameComponent` 会把同一批
		// buffer 重新交接回去** —— 而那批 buffer 的 counter 还留着上一次的砖数，组件照着它又画了
		// 一遍上一代的实例。症状是"砖数已经是 0 了、画面上砖还立着"，而
		// `GetFrameBrickCount()` / 三角形数 / 零阻塞**四条断言全部照绿**。
		//
		// 实测现场（2026-08-31，D7 出图抓到）：把 `bSeamEnabled` 关掉之后，两根接缝砖柱在
		// `seam_off_corner.png` 里原样立着，12 层砖一块不少。这条不是接缝引入的 —— 门框砖
		// 走的是同一段代码（画一笔路开拱、再擦掉路），只是从来没人给那个状态出过图。
		//
		// `CSHouseFrame::Scatter` 的空表分支就是干这个的，它自己的注释早就预言过这个症状
		// （"一块砖都没有时必须**显式清零**……只在从有到无那一次出现"）—— 只是从没被调到过。
		if (IsValid(FrameComponent) && FrameGpuBuffers.Num() == 1 && FrameGpuBuffers[0].IsValid())
		{
			CSHouseFrame::Scatter(Elements, FrameGpuBuffers, WorldToComponent);
		}
		if (FrameComponent) FrameComponent->ClearInstanceSourceGPU();
		FrameHandover.Reset();
		return;
	}

	if (!IsValid(FrameComponent)) return;

	// **一个字节都不分配**：砖数已经在 BuildFrameArches 里按常驻容量截断，扩容那一次阻塞
	// 刷新在这条路上永远不会发生 —— 容量恒定正是解析推导的红利，别退化掉。
	CSHouseFrame::Scatter(Elements, FrameGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s frame scattered: bricks=%d paths=%d seam=%d quoin=%d trim=%d"),
		*GetName(), BrickCount, Elements.Num(), CurrentSeamBrickCount, CurrentQuoinBrickCount, CurrentTrimBrickCount);
}

// -----------------------------------------------------------------------------
// 接缝（D7，2026-09-16「接触记录」）—— 共享记录 + 写入口验 + 谁动谁发 + 松手提交
//
// 这一段只做四件事：把自己翻译成 `CSHouseSeam::FHouse`、跑接触协议（UpdateContacts / ReceiveContactFrom）、
// 从竖缝接触取我这一端的裁剪段与柱、以及把它们记进各自的哈希。几何全在 `CSHouseSeam.h` 与
// `CSHouseContact.cpp`；没有接缝 actor、没有 subsystem 仲裁、没有快扫依赖。
// -----------------------------------------------------------------------------

CSHouseSeam::FHouse ACSHouseActor::MakeSeamHouse() const
{
	CSHouseSeam::FHouse H;
	H.Id = HouseId;
	const FVector Loc = GetActorLocation();
	H.Center = FVector2D(Loc.X, Loc.Y);
	// 只取 yaw，与 `GetBuildTransform()` 同口径 —— 房体面板与门框砖都建在那个变换里，
	// 接缝换一个口径就会在有 pitch/roll 的房子上与它们错开（「已知潜伏问题」那条的同族）。
	H.Yaw = float(GetActorRotation().Yaw);
	H.Footprint = GetFootprint();
	H.BaseZ = float(Loc.Z);
	H.WallHeight = WallHeight;
	H.WallThickness = WallThickness;
	return H;
}

CSHouseSeam::FPostParams ACSHouseActor::MakeSeamPostParams() const
{
	CSHouseSeam::FPostParams P;
	P.VertexClearance = SeamVertexClearance;
	P.ParallelDot = SeamParallelDot;
	P.MergeDistance = SeamPostMergeDistance;
	P.MinHeight = float(FMath::Max(SeamMinPostBricks, 0)) * FMath::Max(FrameBrickLength, 1.0f);   // TG：柱高不足 N 块砖不砌
	return P;
}

void ACSHouseActor::GatherSeamCandidates(TArray<ACSHouseActor*>& Out) const
{
	Out.Reset();
	if (!HouseId.IsValid()) return;
	const CSHouseSeam::FHouse Self = MakeSeamHouse();
	// 名单就是 world 本身（`UCSHouseLibrary::GetHouses` = `TActorRange`，按 GUID 升序；2026-09-16 起没有登记表）：
	// 只在提交这一刻翻一次，用来发现从没碰过我的陌生房子 —— 旧记录只认识老对端。
	TArray<ACSHouseActor*> Houses;
	UCSHouseLibrary::GetHouses(GetWorld(), Houses);
	for (ACSHouseActor* Other : Houses)
	{
		if (!IsValid(Other) || Other == this) continue;
		const FGuid OtherId = Other->GetHouseId();
		// 同 GUID 的副本（结构审查 13：复制走 HouseId）不配对 —— 规范序打平会让两栋都以为自己出砖。
		if (!OtherId.IsValid() || OtherId == HouseId) continue;
		if (!CSHouseSeam::WithinReach(Self, Other->MakeSeamHouse())) continue;
		Out.Add(Other);
	}
}

uint32 ACSHouseActor::ComputeSeamKey() const
{
	const FVector Loc = GetActorLocation();
	TArray<int32> H = {
		CSHouse_Q(Loc.X, 1), CSHouse_Q(Loc.Y, 1), CSHouse_Q(Loc.Z, 0.5), CSHouse_Q(GetActorRotation().Yaw, 0.1),
		CSHouse_Q(WallHeight, 1), CSHouse_Q(WallThickness, 0.5),
		int32(bSeamEnabled), int32(CanBuildSeamBricks()), CSHouse_Q(BearingTolerance, 1) };
	CSHouse_AppendFootprintHash(H, GetFootprint());
	return CSHouse_Hash(H);
}

bool ACSHouseActor::CanCommitSeams()
{
	// 拖动标志卡住（松手事件没来）：`GizmoDragIdleSeconds` 无位移超时兜底，照特征标记 `DragIdleSeconds` 的先例。
	// 这里查是为了重求值恰好先于 `Tick` 时不白等一帧；真正保证会查的是 `Tick`（`NoteGizmoDrag` 开着它）。
	if (bInGizmoDrag && FPlatformTime::Seconds() - GizmoDragTouchedAt > FMath::Max(GizmoDragIdleSeconds, 0.0f)) bInGizmoDrag = false;
	return !bInGizmoDrag && !bInGroundDrag;
}

void ACSHouseActor::NoteGizmoDrag(bool bFinished)
{
	bInGizmoDrag = !bFinished;
	GizmoDragTouchedAt = FPlatformTime::Seconds();
	if (bInGizmoDrag && !IsTemplate()) SetActorTickEnabled(true);
}

bool ACSHouseActor::IsHouseIdTakenByOther(const FGuid& Id) const
{
	const UWorld* World = GetWorld();
	if (!Id.IsValid() || !World) return false;
	for (const ACSHouseActor* Other : TActorRange<ACSHouseActor>(World))
	{
		if (Other != this && IsValid(Other) && !Other->IsTemplate() && Other->HouseId == Id) return true;
	}
	return false;
}

void ACSHouseActor::UpdateContacts(bool bCanCommit)
{
	// 验：丢掉不活的（对端失效 / 对端关了开关）。写入口也验，这里再验一遍是钩子漏调时的兜底。
	Contacts.RemoveAll([](const TSharedPtr<FCSHouseContact>& C) { return !C.IsValid() || !C->IsAlive(); });

	const uint32 Key = ComputeSeamKey();
	if (!bCanCommit || (bSeamKeyValid && Key == LastSeamKey)) return;

	// 我动了：把我的接触全部作废重发。旧对端也要收到一份（不再接触就发空），否则它手里的旧记录
	// 只能等下次被别人写时才清掉，墙上会留一块 clip 出来的缺口。遍历的是副本：对端在写入口里只标脏、
	// 不会反写，但这条纪律不该靠对端守。
	TArray<TSharedPtr<FCSHouseContact>> Old = MoveTemp(Contacts);
	Contacts.Reset();

	TArray<ACSHouseActor*> Partners;
	for (const TSharedPtr<FCSHouseContact>& C : Old)
	{
		ACSHouseActor* Other = C.IsValid() ? C->Other(this) : nullptr;
		if (IsValid(Other)) Partners.AddUnique(Other);
	}
	TArray<ACSHouseActor*> Candidates;
	GatherSeamCandidates(Candidates);
	for (ACSHouseActor* Y : Candidates) Partners.AddUnique(Y);
	// 发送顺序按 GUID 定死：结果本来就与顺序无关，这里只是让日志与柱的下标可复现。
	Partners.Sort([](const ACSHouseActor& L, const ACSHouseActor& R) { return CSHouseSeam::IdLess(L.GetHouseId(), R.GetHouseId()); });

	for (ACSHouseActor* Y : Partners)
	{
		TSharedPtr<FCSHouseContact> Contact = FCSHouseContact::Classify(this, Y);
		if (Contact.IsValid()) Contacts.Add(Contact);
		Y->ReceiveContactFrom(this, Contact);
	}
	LastSeamKey = Key;
	bSeamKeyValid = true;
	UE_LOG(LogTinyGladeHouse, Verbose, TEXT("[TinyGladeHouse] %s seam commit: key=%u partners=%d contacts=%d"),
		*GetName(), Key, Partners.Num(), Contacts.Num());
}

void ACSHouseActor::ReceiveContactFrom(ACSHouseActor* From, TSharedPtr<FCSHouseContact> Contact)
{
	if (!From || From == this || IsTemplate()) return;
	// 验 → 改：`From` 与我之间以前的记录一律作废（它手里的是新的全集），顺手丢掉不活的。
	Contacts.RemoveAll([From](const TSharedPtr<FCSHouseContact>& C) { return !C.IsValid() || C->Involves(From) || !C->IsAlive(); });
	if (Contact.IsValid()) Contacts.Add(Contact);
	// 只标脏。绝不在这里同步重求值 —— `From` 正在遍历它自己的表；也不调 `From` 的任何 getter（会 Flush）。
	RequestReevaluate();
}

bool ACSHouseActor::HoldsContact(const FCSHouseContact* Contact) const
{
	for (const TSharedPtr<FCSHouseContact>& C : Contacts) if (C.Get() == Contact) return true;
	return false;
}

void ACSHouseActor::DetachContacts()
{
	if (Contacts.IsEmpty()) return;
	TArray<TSharedPtr<FCSHouseContact>> Old = MoveTemp(Contacts);
	Contacts.Reset();
	// 世界卸载 / 引擎退出：对端可能也在销毁，不碰。弱引用失效后它自己会在下次「验」时丢掉。
	UWorld* World = GetWorld();
	if (!World || World->bIsTearingDown || IsEngineExitRequested()) return;
	for (const TSharedPtr<FCSHouseContact>& C : Old)
	{
		ACSHouseActor* Other = C.IsValid() ? C->Other(this) : nullptr;
		if (IsValid(Other)) Other->ReceiveContactFrom(this, nullptr);
	}
}

void ACSHouseActor::HandleRootTransformUpdated(USceneComponent* /*UpdatedComponent*/, EUpdateTransformFlags /*UpdateTransformFlags*/, ETeleportType /*Teleport*/)
{
	// 任何来源的 transform 变化（gizmo / 脚本 SetActorLocation / Alt 拖复制 / Undo 回滚 / attach）都从这里标脏，
	// 所以脚本直设 transform 不需要显式的 CommitEdit()，也不需要 subsystem 的快扫。
	// 落座 `SetActorZ` 是重求值自己改的：`bInReevaluate` 期间忽略，否则每次落座多提交一轮。
	if (bInReevaluate || IsTemplate() || !GetWorld()) return;
	RequestReevaluate();
}

uint32 ACSHouseActor::ComputeSeamCuts()
{
	CurrentSeamCuts.Reset();
	CurrentSeamCornerCount = 0;
	// 协议先跑：即便自己关着开关也要跑 —— 关掉那一下要把空发给对端，它的裁剪才会消失。
	UpdateContacts(CanCommitSeams());
	if (!bSeamEnabled) return 0;

	for (const TSharedPtr<FCSHouseContact>& C : Contacts)
	{
		if (!C.IsValid() || C->Kind != ECSHouseContactKind::Seam) continue;
		const FCSHouseSeamContact& Seam = static_cast<const FCSHouseSeamContact&>(*C);
		CurrentSeamCuts.Append(Seam.CutsFor(this));
		CurrentSeamCornerCount += Seam.Corners.Num();
	}
	// 排序：裁剪段来自哪条接触、什么时候收到的，都不该改房体哈希。
	CurrentSeamCuts.Sort([](const FCSWallCut& L, const FCSWallCut& R)
	{
		return L.EdgeIndex != R.EdgeIndex ? L.EdgeIndex < R.EdgeIndex : L.MinS < R.MinS;
	});

	// 形状哈希：裁剪段决定面板怎么切 ⇒ 决定顶点位置与索引数，必须入房体哈希。
	// 交点数不入 —— 它一个顶点都不改（砖是独立实例，走门框那份哈希）。
	TArray<int32> H;
	for (const FCSWallCut& Cut : CurrentSeamCuts)
	{
		H.Append({ Cut.EdgeIndex, CSHouse_Q(Cut.MinS, 1), CSHouse_Q(Cut.MaxS, 1),
			CSHouse_Q(Cut.BottomZ, 1), CSHouse_Q(Cut.TopZ, 1) });
	}
	return CSHouse_Hash(H);
}

uint32 ACSHouseActor::BuildSeamBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentSeamBrickCount = 0;
	CurrentSeamOwnedPostCount = 0;
	if (!bSeamEnabled || !CanBuildSeamBricks() || Contacts.IsEmpty()) return 0;

	// 只砌自己是出砖方的竖缝（一条缝只砌一次）。按规范序 GUID 对排序：柱的输出顺序只由几何决定，
	// 不由记录到达的先后决定。
	TArray<const FCSHouseSeamContact*> Owned;
	for (const TSharedPtr<FCSHouseContact>& C : Contacts)
	{
		if (!C.IsValid() || C->Kind != ECSHouseContactKind::Seam || C->Owner() != this) continue;
		Owned.Add(static_cast<const FCSHouseSeamContact*>(C.Get()));
	}
	if (Owned.IsEmpty()) return 0;
	Owned.Sort([](const FCSHouseSeamContact& L, const FCSHouseSeamContact& R)
	{
		return CSHouseSeam::IdLess(L.Ids[0], R.Ids[0]) || (L.Ids[0] == R.Ids[0] && CSHouseSeam::IdLess(L.Ids[1], R.Ids[1]));
	});

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框砖共用同一份常驻容量**：接缝砖只是同一个组件里排在后面的那些行，超了一起截断。
	Params.MaxBricks = EffectiveFrameCapacity();
	const CSHouseSeam::FPostParams PostParams = MakeSeamPostParams();
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	const int32 Before = Cursor;

	TArray<int32> H;
	TArray<CSHouseSeam::FPost> Posts;
	for (const FCSHouseSeamContact* Seam : Owned)
	{
		// 一条接触一组柱，**永不与别的接触合并**（2026-09-16 晚用户裁决「数量上必须要对应」）：
		// A 在下、B 摞在上、C 横穿两栋时，C 上是 A–C 与 B–C 两组柱，不是一根通高的柱。
		Seam->BuildPosts(PostParams, Posts);
		const uint32 Seed = Seam->Seed();
		// 种子进哈希：柱位没变而对端换了身份（复制出新 GUID）时，砖的随机花样也要跟着重砌。
		if (!Posts.IsEmpty()) H.Add(int32(Seed));
		for (int32 Index = 0; Index < Posts.Num(); ++Index)
		{
			const CSHouseSeam::FPost& Post = Posts[Index];
			// 随机数基 = 这条接触的种子 + 它自己的柱号：别的接触增减不会让这条缝的砖换花样。
			CSHouseFrame::AppendColumn(Post.Point, Post.Outward, Post.BottomZ, Post.TopZ,
				CSHouseFrame::PathRandomBase(Seed, CSHouseFrame::EPathFamily::Seam, Index), Params, InOutElements, Cursor);
			++CurrentSeamOwnedPostCount;
			// 哈希只记标量：柱位 + 柱高。逐砖的位置是它们的纯函数。
			H.Append({ CSHouse_Q(Post.Point.X, 1), CSHouse_Q(Post.Point.Y, 1), CSHouse_Q(Post.Outward.X, 0.01), CSHouse_Q(Post.Outward.Y, 0.01),
				CSHouse_Q(Post.BottomZ, 1), CSHouse_Q(Post.TopZ, 1) });
		}
	}
	CurrentSeamBrickCount = Cursor - Before;
	InOutBrickCount += CurrentSeamBrickCount;
	if (CurrentSeamBrickCount > 0) H.Add(CurrentSeamBrickCount);
	return CSHouse_Hash(H);
}

TArray<FString> ACSHouseActor::AuditContacts() const
{
	FlushPendingReevaluate();
	TArray<FString> Problems;
	TSet<FGuid> Seen;
	for (const TSharedPtr<FCSHouseContact>& C : Contacts)
	{
		if (!C.IsValid()) { Problems.Add(TEXT("接触表里有空指针")); continue; }
		const FString Pair = FString::Printf(TEXT("%s-%s"), *C->Ids[0].ToString(EGuidFormats::Short), *C->Ids[1].ToString(EGuidFormats::Short));
		if (!C->IsAlive()) { Problems.Add(FString::Printf(TEXT("%s：死记录（对端失效或关了开关）"), *Pair)); continue; }
		const ACSHouseActor* Other = C->Other(this);
		if (!IsValid(Other)) { Problems.Add(FString::Printf(TEXT("%s：不涉及本房或对端为空"), *Pair)); continue; }
		if (!Other->HoldsContact(C.Get())) Problems.Add(FString::Printf(TEXT("%s：对端 %s 没有持有同一个对象"), *Pair, *Other->GetName()));
		bool bDuplicate = false;
		Seen.Add(Other->GetHouseId(), &bDuplicate);
		if (bDuplicate) Problems.Add(FString::Printf(TEXT("%s：同一对房子有两条接触"), *Pair));
		const TSharedPtr<FCSHouseContact> Now = FCSHouseContact::Classify(const_cast<ACSHouseActor*>(this), const_cast<ACSHouseActor*>(Other));
		if (!Now.IsValid()) Problems.Add(FString::Printf(TEXT("%s：两房已不接触，记录还在"), *Pair));
		else if (Now->Kind != C->Kind) Problems.Add(FString::Printf(TEXT("%s：接触种类已变（记录 %d，现在 %d）"), *Pair, int32(C->Kind), int32(Now->Kind)));
	}
	if (!bInGizmoDrag && !bInGroundDrag && (!bSeamKeyValid || ComputeSeamKey() != LastSeamKey)) Problems.Add(TEXT("欠着一次提交（不在拖拽却 Key ≠ LastSeamKey）"));
	// 数量对应（09-16 晚裁决）：我砌的柱数 = 我是出砖方的那些竖缝各自的柱数之和，一根不多一根不少。
	if (bSeamEnabled && CanBuildSeamBricks())
	{
		const CSHouseSeam::FPostParams PostParams = MakeSeamPostParams();
		TArray<CSHouseSeam::FPost> Posts;
		int32 Expected = 0;
		for (const TSharedPtr<FCSHouseContact>& C : Contacts)
		{
			if (!C.IsValid() || C->Kind != ECSHouseContactKind::Seam || C->Owner() != this) continue;
			Expected += static_cast<const FCSHouseSeamContact&>(*C).BuildPosts(PostParams, Posts);
		}
		if (Expected != CurrentSeamOwnedPostCount)
		{
			Problems.Add(FString::Printf(TEXT("柱数不对应：砌了 %d 根，我出砖的接触合计 %d 根"), CurrentSeamOwnedPostCount, Expected));
		}
	}
	return Problems;
}

// -----------------------------------------------------------------------------
// 转角墩（2026-09-06）—— 转角配成墩的角上那根柱础 / 柱身 / 柱头，与拱廊的墩同一副样子
// -----------------------------------------------------------------------------

uint32 ACSHouseActor::BuildCornerPierBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentCornerPierCount = 0;
	if (!bFrameEnabled || !FrameBrickMesh) return 0;
	bool bAny = false;
	for (const float TopZ : CornerPierTopZ) bAny |= TopZ > UE_KINDA_SMALL_NUMBER;
	if (!bAny) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	Params.CapitalScale = PierCapitalScale;     // 与拱廊的墩同一对参数：转角墩就是那种墩，只是立在角上
	Params.CapitalHeight = PierCapitalHeight;
	Params.MaxBricks = EffectiveFrameCapacity();

	// 墩心放在两面墙**墙厚中线的交点**（`PointAtDepth(T/2)`，直角上是沿平分线内缩 T/√2）：
	// 门樘砖走墙厚正中（`BuildFrameArches` 的 Mid），拱廊的墩也在那条线上。角石（Inset 0）留在外角当面层。
	// 角点与角平分线与角石同一个真源 `CSHouse_GetCorner`。墩不看 `IsQuoinCorner`：凹角 / 锐角上
	// 不包角石，但两道拱照样可以在那里配成墩。
	const float BaseZ = float(GetActorLocation().Z);
	const FTransform World = GetBuildTransform();
	const FCSHouseFootprint Footprint = GetFootprint();

	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	int32 Cursor = CSHouseFrame::NextBrickSlot(InOutElements);
	TArray<int32> H;
	for (int32 Corner = 0; Corner < CornerPierTopZ.Num() && Corner < Footprint.NumEdges(); ++Corner)
	{
		const float TopZ = CornerPierTopZ[Corner];
		if (TopZ <= UE_KINDA_SMALL_NUMBER) continue;
		const FCSHouseCornerFrame CF = CSHouse_GetCorner(Corner, Footprint);
		const FVector2D Local = CF.PointAtDepth(WallThickness * 0.5);
		const FVector WorldPoint = World.TransformPosition(FVector(Local.X, Local.Y, 0.0));
		const FVector WorldOut = World.TransformVectorNoScale(FVector(CF.Outward.X, CF.Outward.Y, 0.0));
		const FVector2D Point(WorldPoint.X, WorldPoint.Y);
		const FVector2D Outward = FVector2D(WorldOut.X, WorldOut.Y).GetSafeNormal();
		const int32 Added = CSHouseFrame::AppendCornerPier(Point, Outward, BaseZ, BaseZ + TopZ,
			Seed, Corner, Params, InOutElements, Cursor);
		if (Added <= 0) continue;
		++CurrentCornerPierCount;
		InOutBrickCount += Added;
		// 哈希只记标量：角序号、砖数、柱心、墩顶。逐砖位置是它们的纯函数。
		H.Append({ Corner, Added, CSHouse_Q(Point.X, 1), CSHouse_Q(Point.Y, 1), CSHouse_Q(TopZ, 1) });
	}
	return H.IsEmpty() ? 0u : CSHouse_Hash(H);
}

// -----------------------------------------------------------------------------
// 角石（D7 的墙自身转角）—— 纯函数：footprint 四角 → 竖直砖柱，与接缝柱共用发射器
// -----------------------------------------------------------------------------

uint32 ACSHouseActor::BuildQuoinBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentQuoinColumnCount = 0;
	CurrentQuoinBrickCount = 0;
	// 与接缝砖同一条前置：没有组件宿主 / 没有砖网格就整条不出。`bFrameEnabled` 是三家共同的总闸。
	if (!bQuoinEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框砖 / 接缝砖共用同一份常驻容量**：角石只是同一个组件里排在最后的那些行。
	Params.MaxBricks = EffectiveFrameCapacity();
	// 角石的 TG 基准厘米在 BuildQuoinElements 里按层高折算。
	// 原函数随机改变长边，并按路内层号换向；不能把随机数加到角平分线位置上。
	Params.Jitter = FMath::Max(QuoinJitter, 0.0f);
	Params.SplitJitter = FMath::Max(QuoinSplitJitter, 0.0f);

	// 与房体面板、门框砖、藤蔓同一个变换口径（只取 yaw）。换口径就会在有 pitch/roll 的房子上
	// 与它们错开 —— 那正是「已知潜伏问题」里 `GetBuildTransform()` vs `ToInverseMatrixWithScale()`
	// 那条不对称的同族。
	TArray<CSHouseQuoin::FQuoin> Quoins;
	CurrentQuoinColumnCount = CSHouseQuoin::BuildQuoins(GetBuildTransform(), GetFootprint(), WallThickness,
		float(GetActorLocation().Z), WallHeight, QuoinInset, Quoins);
	if (Quoins.IsEmpty()) return 0;

	// 山墙两头的角：角石跟着墙顶砌上去。TG `add_wall_corners` 141215A91..141215B58 的柱高取「角处的墙顶」，
	// 而山墙的角点是被抬高的（阶梯式抬 46 cm + 屋面偏移；悬挑式两头高度为 0，这里就是恒等）。
	{
		TArray<FCSHouseGableWall> GableWalls;
		BuildGableWalls(GableWalls);
		const int32 NumCorners = GetFootprint().NumEdges();
		for (CSHouseQuoin::FQuoin& Q : Quoins)
		{
			double Rise = 0.0;
			for (const FCSHouseGableWall& Gable : GableWalls)
			{
				if (Gable.Profile.Num() < 2) continue;
				// 角 k 夹在「k 号边的远端」与「k+1 号边的近端」之间。
				if (Gable.EdgeIndex == Q.CornerIndex) Rise = FMath::Max(Rise, Gable.Profile.Last().Y);
				if (Gable.EdgeIndex == (Q.CornerIndex + 1) % NumCorners) Rise = FMath::Max(Rise, Gable.Profile[0].Y);
			}
			Q.TopZ += float(Rise);
		}
	}

	// 转角配成墩的角上，角石在墩顶以下让路：那一截由 `BuildCornerPierBricks` 的柱础/柱身/柱头
	// 顶替（2026-09-06 用户裁决：转角就是一个墩）。判据与高度都只有 `ResolvePierSpans` 写的那一份
	// `CornerPierTopZ`，这里不再自己找洞 —— 两处各判一次就会出现"墩砌到 A 高度、角石剔到 B 高度"。
	// `FQuoin::BottomZ` 已经是世界 Z，剔除线跟它同一个口径。
	{
		const float BaseZ = float(GetActorLocation().Z);
		// 按角号对，不按数组下标：不出角石的角（凹角 / 锐角）被跳过，下标与角号会错开。
		for (CSHouseQuoin::FQuoin& Q : Quoins)
		{
			if (CornerPierTopZ.IsValidIndex(Q.CornerIndex) && CornerPierTopZ[Q.CornerIndex] > UE_KINDA_SMALL_NUMBER)
			{
				Q.CullBelowZ = BaseZ + CornerPierTopZ[Q.CornerIndex];
			}
		}
	}

	// 随机数基取**房子身份**，不取槽位：槽位会被"这栋房多开一扇门"整体推走（门框砖排在前面），
	// 于是将来谁给砖材质接上 `PerInstanceRandom` 色差，开一扇门就会让四个角整体换色，
	// 而所有几何断言全绿。与接缝砖那条是同一个理由的另一半。
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const FVector MeshSize = FrameBrickMesh->GetBoundingBox().GetSize();
	const FVector2f MeshInvSize(1.0 / FMath::Max(MeshSize.X, 1.0), 1.0 / FMath::Max(MeshSize.Z, 1.0));
	const int32 Added = CSHouseQuoin::BuildQuoinElements(Quoins, Seed, Params, InOutElements, MeshInvSize);
	CurrentQuoinBrickCount = Added;
	InOutBrickCount += Added;

	// 参数也进入哈希：只改随机幅度、层高或种子同样必须重新散布。
	// QuoinInset 已经包含在 Point 中。
	TArray<int32> H;
	H.Append({ Added, int32(Seed), CSHouse_Q(Params.Length, 0.001), CSHouse_Q(Params.Gap, 0.001),
		CSHouse_Q(Params.Jitter, 0.001), CSHouse_Q(Params.SplitJitter, 0.001) });
	for (const CSHouseQuoin::FQuoin& Q : Quoins)
	{
		H.Append({ CSHouse_Q(Q.Point.X, 1), CSHouse_Q(Q.Point.Y, 1),
			CSHouse_Q(Q.Outward.X, 0.01), CSHouse_Q(Q.Outward.Y, 0.01), CSHouse_Q(Q.HalfTurnCos, 0.001),
			CSHouse_Q(Q.BottomZ, 1), CSHouse_Q(Q.TopZ, 1),
			// 剔除高度也得进：它决定哪些砖被写成负值随机数 ⇒ 决定画面。漏掉它 =
			// 转角门开了/关了而角柱照旧，且**没有任何报错**（同 `StyleFlags` 那条）。
			CSHouse_Q(Q.CullBelowZ, 1) });
	}
	return CSHouse_Hash(H);
}

// -----------------------------------------------------------------------------
// 包边石（D7 第三样）—— 墙顶压顶 + 墙脚勒脚，两条带共用一套算法
// -----------------------------------------------------------------------------

/** 砖容量的硬上限（用户 2026-09-06 定）。5 个 float4 = 80 B/块 ⇒ 65536 块 ≈ 5.2 MB/房。 */
static constexpr int32 CSHouse_MaxFrameCapacity = 65536;

int32 ACSHouseActor::EffectiveFrameCapacity() const
{
	const int32 Authored = FMath::Max(FrameReserveCapacity, 64);
	if (!bBrickWallEnabled) return FMath::Min(Authored, CSHouse_MaxFrameCapacity);

	// 砖层的量级是 footprint 的函数（6 × 4 m、檐高 3 m 就要 1110 块），让用户手算等于把
	// "墙砌到一半"的责任推给他。所以砖层自己按上界加够，**authored 那份原样留给其余四家**
	// （门框 / 接缝 / 角石 / 包边）——两边互不挤占，谁超了都能从数字上看出来。
	return FMath::Clamp(Authored + GetBrickWallBrickBudget(), 64, CSHouse_MaxFrameCapacity);
}

int32 ACSHouseActor::GetBrickWallBrickBudget() const
{
	const CSHouseBrickWall::FCourses Courses =
		CSHouseBrickWall::PlanCourses(WallHeight, BrickWallCourseHeight);
	const FCSHouseFootprint Footprint = GetFootprint();
	int32 Budget = CSHouseBrickWall::EstimateBricks(Footprint, WallThickness, Courses, FMath::Max(FrameBrickLength, 1.0f));

	// 山墙上续摞的那几层也要进预算：容量只截断不扩容，漏掉它 = 把脊拖到头的那一下山墙尖砌不完。
	TArray<FCSHouseGableWall> GableWalls;
	BuildGableWalls(GableWalls);
	if (GableWalls.Num() > 0)
	{
		TArray<CSHouseBrickWall::FGable, TInlineAllocator<4>> Gables;
		for (const FCSHouseGableWall& Gable : GableWalls) Gables.Add({ Gable.EdgeIndex, Gable.Profile });
		Budget += CSHouseBrickWall::EstimateGableBricks(Footprint, WallThickness, Gables, Courses, FMath::Max(FrameBrickLength, 1.0f));
	}
	return Budget;
}

uint32 ACSHouseActor::BuildBrickWallBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentBrickWallBrickCount = 0;
	CurrentBrickWallCourseCount = 0;
	if (!bBrickWallEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;

	const CSHouseBrickWall::FCourses Courses =
		CSHouseBrickWall::PlanCourses(WallHeight, BrickWallCourseHeight);
	if (Courses.Count <= 0) return 0;
	CurrentBrickWallCourseCount = Courses.Count;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框 / 接缝 / 角石 / 包边共用同一份常驻容量**：砖层只是同一个组件里排在最后的那些行。
	// 撞上限就停发，绝不扩容 —— 扩容是一次阻塞刷新，落在用户恰好画到的那一笔上。
	Params.MaxBricks = EffectiveFrameCapacity();

	const FTransform World = GetBuildTransform();
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const float Clearance = FMath::Max(BrickWallOpeningClearance, 0.0f);

	TArray<int32> H;
	TArray<CSHouseTrim::FRun> Runs;

	// 哈希只记标量：层高 + 每层的砖数与各段的 (边号, S 区间)。逐砖位置是它们的纯函数。
	// ⚠️ 必须逐层记，不能只记总砖数：把一扇窗左右挪半米，总数可以一块不差而位置全变了。
	H.Append({ Courses.Count, CSHouse_Q(Courses.Height, 0.5) });
	auto HashCourse = [&H](int32 CourseIndex, const CSHouseTrim::FBand& Band, int32 CourseBricks,
		const TArray<CSHouseTrim::FRun>& CourseRuns)
	{
		H.Append({ CourseIndex, CourseBricks, CSHouse_Q(Band.CenterZ, 1) });
		for (const CSHouseTrim::FRun& R : CourseRuns)
		{
			H.Append({ R.EdgeIndex, CSHouse_Q(R.S0, 1), CSHouse_Q(R.S1, 1) });
		}
	};
	int32 Added = CSHouseBrickWall::BuildWall(World, GetFootprint(), WallThickness, Courses,
		Clearance, MakeArrayView(CurrentOpenings), Seed, Params, Runs, InOutElements, HashCourse);

	// 山墙（双坡时两面）：墙顶之上接着摞，逐层沿屋面线收成台阶（TG `trim_rows`，见 `CSHouseBrickWall::GableCourseSpan`）。
	TArray<FCSHouseGableWall> GableWalls;
	BuildGableWalls(GableWalls);
	if (GableWalls.Num() > 0)
	{
		TArray<CSHouseBrickWall::FGable, TInlineAllocator<4>> Gables;
		for (const FCSHouseGableWall& Gable : GableWalls) Gables.Add({ Gable.EdgeIndex, Gable.Profile });
		Added += CSHouseBrickWall::BuildGableCourses(World, GetFootprint(), WallThickness, Courses, Gables,
			Seed, Params, Runs, InOutElements, HashCourse);
	}

	CurrentBrickWallBrickCount = Added;
	InOutBrickCount += Added;
	if (Added <= 0) return 0;

	UE_LOG(LogTinyGladeHouse, Verbose,
		TEXT("[TinyGladeHouse] %s brick wall: courses=%d bricks=%d budget=%d capacity=%d"),
		*GetName(), Courses.Count, Added, GetBrickWallBrickBudget(), Params.MaxBricks);
	return CSHouse_Hash(H);
}

uint32 ACSHouseActor::BuildTrimBricks(TArray<CSHouseFrame::FElement>& InOutElements, int32& InOutBrickCount)
{
	CurrentTrimTopRunCount = 0;
	CurrentTrimBaseRunCount = 0;
	CurrentTrimBrickCount = 0;
	CurrentTrimGableBrickCount = 0;
	if (!bTrimEnabled || !bFrameEnabled || !FrameBrickMesh) return 0;
	if (WallHeight <= 0.0f) return 0;

	CSHouseFrame::FBrickParams Params;
	Params.Length = FMath::Max(FrameBrickLength, 1.0f);
	Params.Gap = FMath::Max(FrameBrickGap, 0.0f);
	// **与门框 / 接缝 / 角石共用同一份常驻容量**：包边只是同一个组件里排在最后的那些行。
	Params.MaxBricks = EffectiveFrameCapacity();

	// 课程半高 = 砖的进深轴（`AppendFlatRun` 里 AxisX 朝上）。它只参与"洞挡不挡得住"的判据。
	const float HalfHeight = FMath::Max(FrameBrickDepth, 1.0f) * 0.5f;
	const FTransform World = GetBuildTransform();
	const uint32 Seed = HouseId.IsValid() ? GetTypeHash(HouseId) : uint32(GetUniqueID());
	const float Clearance = FMath::Max(TrimOpeningClearance, 0.0f);

	TArray<int32> H;
	TArray<CSHouseTrim::FRun> Runs;

	auto RunBand = [&](bool bWanted, float CenterZ, uint32 Salt, int32& OutRunCount)
	{
		OutRunCount = 0;
		if (!bWanted) return;
		CSHouseTrim::FBand Band;
		Band.CenterZ = CenterZ;
		Band.HalfHeight = HalfHeight;
		const int32 Added = CSHouseTrim::BuildBand(World, GetFootprint(), WallThickness, Band, Clearance,
			MakeArrayView(CurrentOpenings), Seed, Salt, Params, Runs, InOutElements);
		OutRunCount = Runs.Num();
		CurrentTrimBrickCount += Added;
		InOutBrickCount += Added;
		// 哈希只记标量：带高 + 每段的边号与 S 区间 + 砖数。逐砖位置是它们的纯函数。
		H.Append({ Added, CSHouse_Q(CenterZ, 1) });
		for (const CSHouseTrim::FRun& R : Runs)
		{
			H.Append({ R.EdgeIndex, CSHouse_Q(R.S0, 1), CSHouse_Q(R.S1, 1) });
		}
	};

	// 顺序固定：先顶后底。换序会推走对方的槽位（两者的随机数都已从身份派生，所以只是纪律）。
	RunBand(bTrimTop, WallHeight + TrimTopOffset, CSHouseFrame::EPathFamily::TrimTop, CurrentTrimTopRunCount);
	RunBand(bTrimBase, TrimBaseOffset, CSHouseFrame::EPathFamily::TrimBase, CurrentTrimBaseRunCount);

	// 阶梯山墙上沿的露砖（2026-10-02）：墙板只做到屋面线（`BuildGableWalls` 的 `PlasterDrop`），上面那一圈在这里砌。
	// 层的划分与砖层是同一份 `PlanCourses` —— 开不开砖层，山墙上的砖缝都落在同样的高度上；开了砖层就由它整面砌，这里不出。
	// 排在两条带之后：它只随屋顶形状变，不会推走别人的槽位。
	if (bTrimGable && !bBrickWallEnabled && GableRoofStyle == ECSGableRoofStyle::Stepped)
	{
		TArray<FCSHouseGableWall> GableWalls;
		BuildGableWalls(GableWalls);
		const CSHouseBrickWall::FCourses Courses = CSHouseBrickWall::PlanCourses(WallHeight, BrickWallCourseHeight);
		if (GableWalls.Num() > 0 && Courses.Count > 0)
		{
			TArray<CSHouseBrickWall::FGable, TInlineAllocator<4>> Gables;
			for (const FCSHouseGableWall& Gable : GableWalls) Gables.Add({ Gable.EdgeIndex, Gable.Profile });
			const int32 Added = CSHouseBrickWall::BuildGableCourses(World, GetFootprint(), WallThickness, Courses, Gables,
				Seed, Params, Runs, InOutElements,
				[&H](int32 CourseIndex, const CSHouseTrim::FBand& Band, int32 CourseBricks, const TArray<CSHouseTrim::FRun>& CourseRuns)
				{
					H.Append({ CourseIndex, CourseBricks, CSHouse_Q(Band.CenterZ, 1) });
					for (const CSHouseTrim::FRun& R : CourseRuns) H.Append({ R.EdgeIndex, CSHouse_Q(R.S0, 1), CSHouse_Q(R.S1, 1) });
				},
				CSHouseRoof_SteppedGableRise, CSHouseFrame::EPathFamily::GableCoping);
			CurrentTrimGableBrickCount = Added;
			CurrentTrimBrickCount += Added;
			InOutBrickCount += Added;
		}
	}

	return CSHouse_Hash(H);
}

bool ACSHouseActor::IsSeamDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
	OutReason = GetSeamUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsWindowDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetSeamUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s seam not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetSeamUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— 交点数 / 砖数 / 裁剪段数三条数值断言对这些一个字都说不了。
	if (!bSeamEnabled) return TEXT("bSeamEnabled 关着");
	if (Contacts.IsEmpty()) return TEXT("这栋房没有任何接触（触发条件是 footprint 真重叠，不是靠得近；或任一端关了开关）");
	if (CurrentSeamCornerCount <= 0) return TEXT("只有横缝（承托 / 悬挑）没有竖缝：本轮横缝不出产物");

	// ---- 洞那一半：插进邻居房间里的那截墙由墙材质逐像素 discard 抹掉 ----
	if (CurrentSeamCuts.IsEmpty()) return TEXT("相交了却一段墙都没抹掉（每条边都与邻居 footprint 全无交集，不该发生）");
	if (!IsValid(TinyGladeMeshComponent)) return TEXT("房体：没有渲染组件");
	if (!TinyGladeMeshComponent->IsRegistered()) return TEXT("房体：渲染组件没注册");
	if (!TinyGladeMeshComponent->IsVisible()) return TEXT("房体：渲染组件不可见");
	if (!TinyGladeMesh) return TEXT("房体：常驻网格还没建起来");
	if (!WallMaterial) return TEXT("墙材质是空的（洞与墙都会退回引擎默认表面材质）");
	// **这一条是致命项**：接缝处那个洞不是几何，是墙材质用 OpacityMask 逐像素 discard 切出来的
	// （裁决三：避免所有真几何洞）。墙材质一旦不是 Masked，两栋房的墙就原样穿插着互相插进对方
	// 房间，而交点数 / 砖数 / 裁剪段数 / 零阻塞四条断言**全部照绿**。
	if (WallMaterial->GetBlendMode() != BLEND_Masked)
	{
		return FString::Printf(TEXT("墙材质 '%s' 不是 Masked（相交处的洞由 OpacityMask 逐像素切出，非 Masked 下那截墙照样画）"),
			*WallMaterial->GetName());
	}

	// ---- 砖那一半：只有 Owner（规范序里第一个能出砖的）那一端砌，非出砖方的 0 块砖是正确状态 ----
	if (CurrentSeamOwnedPostCount <= 0)
	{
		// 我不出砖：柱由对端砌。只剩一种要出声的情形 —— 两端都出不了砖。
		for (const TSharedPtr<FCSHouseContact>& C : Contacts)
		{
			if (C.IsValid() && C->Kind == ECSHouseContactKind::Seam && !C->Owner())
			{
				return TEXT("两端都出不了砖（bFrameEnabled 关着或没有 FrameBrickMesh），交点处会露出两条裁剪断口");
			}
		}
		return FString();
	}
	if (CurrentSeamBrickCount <= 0) return TEXT("有该我砌的柱却一块砖都没排出来（柱高不足门槛，或砖数已被 FrameReserveCapacity 截断）");
	if (!IsValid(FrameComponent)) return TEXT("接缝砖：没有渲染组件");
	if (!FrameComponent->IsRegistered()) return TEXT("接缝砖：渲染组件没注册");
	if (!FrameComponent->IsVisible()) return TEXT("接缝砖：渲染组件不可见");
	if (!FrameComponent->HasInstanceSourceGPU()) return TEXT("接缝砖：实例源没交接");
	if (FrameComponent->GetBaseMeshSnapshot().Positions.Num() < 3) return TEXT("接缝砖：基础网格快照是空的");
	// Nanite 路（砖资产开了 Nanite）从不建 InstancedGpuMesh，画的是替身（结构审查 33：09-11 起回归里的两条假红）——
	// 那条路问替身在不在；经典路才问 GPU 网格。
	if (FrameComponent->IsNaniteRenderPath())
	{
		if (!FrameComponent->GetNaniteComponent()) return TEXT("接缝砖：Nanite 路没有替身组件");
	}
	else if (!FrameComponent->GetGpuMesh()) return TEXT("接缝砖：GPU 网格没分配");
	// ⚠️ 没勾实例化用途的材质在实例路径上会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。
	// 所以"材质支持实例化"必须是显式判据，不能只查材质非空。逐段查、覆盖材质与资产材质同一口径（组件那一份）。
	const FString BrickMaterialReason = FrameComponent->GetMaterialUndrawableReason();
	if (!BrickMaterialReason.IsEmpty()) return FString::Printf(TEXT("接缝砖：%s"), *BrickMaterialReason);
	return FString();
}

// -----------------------------------------------------------------------------
// 窗（D8）—— 诉求走显式列表，几何全在房体与门框砖里
// -----------------------------------------------------------------------------

bool ACSHouseActor::IsWindowDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
	OutReason = GetWindowUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsVineDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetWindowUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s window not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetWindowUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（同 IsVineDrawable / IsDecorDrawable）。
	if (!bWindowsEnabled) return TEXT("bWindowsEnabled 关着");
	if (Windows.IsEmpty()) return TEXT("Windows 列表是空的（窗只从这份显式列表来）");
	if (CurrentWindowCount <= 0)
	{
		return FString::Printf(TEXT("%d 个窗诉求全被谓词拒了（原因逐条调 QueryFeatureReject；门拱优先于窗）"),
			CurrentWindowRejectCount);
	}

	// ---- 洞那一半：房体网格 + 墙材质 ----
	if (!IsValid(TinyGladeMeshComponent)) return TEXT("房体：没有渲染组件");
	if (!TinyGladeMeshComponent->IsRegistered()) return TEXT("房体：渲染组件没注册");
	if (!TinyGladeMeshComponent->IsVisible()) return TEXT("房体：渲染组件不可见");
	if (!TinyGladeMesh) return TEXT("房体：常驻网格还没建起来");
	if (!WallMaterial) return TEXT("墙材质是空的（洞与墙都会退回引擎默认表面材质）");
	// **这一条是窗独有的致命项**：洞不是几何，是墙材质用 OpacityMask 逐像素 discard 切出来的
	// （裁决三：避免所有真几何洞）。墙材质一旦不是 Masked，画面上一个洞都没有，而洞数 / 砖数 /
	// 三角形数 / 零阻塞四条断言**全部照绿** —— 与"没绑材质"那一枪同型，只是更隐蔽。
	if (WallMaterial->GetBlendMode() != BLEND_Masked)
	{
		return FString::Printf(TEXT("墙材质 '%s' 不是 Masked（洞由 OpacityMask 逐像素切出，非 Masked 下窗洞根本不存在）"),
			*WallMaterial->GetName());
	}

	// ---- 洞缘那一半：不归房子，所以一道砖判据都不设 ----
	//
	// ⚠️ **窗周围没有任何砖**：2026-09-06 起窗不出框砖（洞缘归标记自带的 `OpeningMesh`，见
	// `ACSWindowMarker`），2026-09-10 起也不再把砖层算作窗的洞缘补全。别把门框砖 / 砖层的健康
	// 挂回这里 —— 演示关卡里门恰好在排砖，窗会**因为错误的理由通过**；关掉门 / 角石 / 包边之后，
	// 一栋只有窗的房子又会被误判成"不可画"（回归里"只剩窗"那一枪钉的就是它）。砖组件本身的
	// 健康也不归窗管：接缝判据查它的全链（有接缝时），`DebugGetGpuAssetMismatchSync` 查画的是不是那块砖。
	//
	// ⚠️ 属性面板 `Windows` 那一份没有标记、没有网格 ⇒ 洞缘是**裸的** —— 这是已知并接受的代价
	// （那条路在计划里一直写着是"授权 / 测试用的便利入口"），所以它**不构成**"不可画"。
	return FString();
}

// -----------------------------------------------------------------------------
// 藤蔓（D13）
// -----------------------------------------------------------------------------

bool ACSHouseActor::GetWallPath(FCSWallPath& OutPath, FCSWallSkins& OutSkins, FTransform& OutWorld) const
{
	// 房子的墙 = footprint（外皮）围成的一圈，局部空间：墙脚 0、墙顶 WallHeight。与房体面板 / 角石 / 藤同一个真源。
	OutWorld = GetBuildTransform();
	return CSWall::BuildEnclosureRing(GetFootprint(), WallThickness, 0.0, double(WallHeight), OutPath, OutSkins);
}

FCSWallVineSettings ACSHouseActor::GetVineSettings() const
{
	// 38 个平铺属性逐个对到结构体上（2026-09-22 统一时不迁数据）。加字段时两边一起加，漏一个就是"房子的藤不认这个参数"。
	FCSWallVineSettings V;
	V.bEnabled = bVineEnabled;
	V.BranchMesh = VineBranchMesh;
	V.LeafMesh = VineLeafMesh;
	V.FlowerMesh = VineFlowerMesh;
	V.BranchMaterial = VineBranchMaterial;
	V.LeafMaterial = VineLeafMaterial;
	V.FlowerMaterial = VineFlowerMaterial;
	V.Season = VineSeason;
	V.StrandSpacing = VineStrandSpacing;
	V.SegmentLength = VineSegmentLength;
	V.MaxSegments = VineMaxSegments;
	V.Wander = VineWander;
	V.MaxLean = VineMaxLean;
	V.MaxTurn = VineMaxTurn;
	V.Bloat = VineBloat;
	V.Thickness = VineThickness;
	V.StandOff = VineStandOff;
	V.HoleClearance = VineHoleClearance;
	V.LeafChance = VineLeafChance;
	V.LeafSize = VineLeafSize;
	V.LeafSizeJitter = VineLeafSizeJitter;
	V.FlowerChance = VineFlowerChance;
	V.FlowerFromFrac = VineFlowerFromFrac;
	V.FlowerSize = VineFlowerSize;
	V.JumpChance = VineJumpChance;
	V.bUseTube = bVineUseTube;
	V.TubeSegments = VineTubeSegments;
	V.TubeSubdivide = VineTubeSubdivide;
	V.GrowSpeed = VineGrowSpeed;
	V.LeafGrowDelay = VineLeafGrowDelay;
	V.FlowerGrowDelay = VineFlowerGrowDelay;
	V.GrowFadeSeconds = VineGrowFadeSeconds;
	V.bGrowOnLoad = bVineGrowOnLoad;
	V.TipTaperLength = VineTipTaperLength;
	V.TipTaperMin = VineTipTaperMin;
	V.MaxGroundGap = VineMaxGroundGap;
	V.GroundSampleSpacing = VineGroundSampleSpacing;
	V.Seed = VineSeed;
	return V;
}

void ACSHouseActor::BuildVineStrips(TArray<CSHouseVine::FWallStrip>& OutStrips) const
{
	// 藤的墙面 = 围合墙（`ECSWallKind::Enclosure`）的墙面：外皮逐边一条平面带，屋里不长藤。2026-09-22 起与样条墙
	// 同一个 `CSWall::BuildVineStrips`（原来这里的那一份逐字搬过去了，逐位不变）；路径来源与房体面板同一个（`GetWallPath`）。
	OutStrips.Reset();
	FCSWallPath Ring;
	FCSWallSkins Skins;
	FTransform World;
	if (!GetWallPath(Ring, Skins, World)) return;

	CSWall::FVineStripParams Params;
	Params.Kind = GetWallKind();
	Params.World = World;
	// 地面空隙与承重柱**同一个量**（`Gap = 房底 Z − 地面高度`，见 ComputePillars）。
	Params.GroundRefZ = GetActorLocation().Z;
	Params.GroundSampleSpacing = VineGroundSampleSpacing;
	// ⚠️ 必须用 `TrySampleHeight`：`SampleHeight` 在范围外退回地面 actor 的 Z，等于在房子底下凭空垫一块无限大的平地
	// （脚下没有地面 ⇒ 悬空、这段墙不长藤，用户裁决 2026-09-21）。
	CSWall::BuildVineStrips(Ring, Skins, Params,
		[this](const FVector2D& XY, float& OutZ) { return Ground && Ground->TrySampleHeight(XY, OutZ); }, OutStrips);
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureVineComponents()
{
	// 组件 / 季节与生长 MID / 基础网格快照 / 交接：墙的基类 `ACSWallBase::EnsureVineRig`（房子与样条墙共用一份）。
	// 这里只给房子自己的两样：常驻容量与交接包围盒（按 footprint 周长、屋脊高算）。
	return EnsureVineRig(GetVineSettings(), [this](uint32& OutMaxRecords, FBox& OutLocalBounds)
	{
		// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。上限是纯配置量：
		// 四面墙的总周长 / 间距 × 每根最多几段。规划真的排超了就在 kernel 里截断 ——
		// 少画几段藤，远好过在拖动的某一帧上付一次设备同步。
		const FCSHouseFootprint Footprint = GetFootprint();
		const double Perimeter = Footprint.GetPerimeter();
		const int32 MaxStrands = FMath::CeilToInt(Perimeter / FMath::Max(VineStrandSpacing, 20.0f)) + Footprint.NumEdges();
		// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把这个数直接喂给 ReserveCapacity。
		// 上限本身是 FootprintSize 的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 ——
		// 拖尺寸时每涨过一根藤的间距就重新分配一次，实测每次 **5 次阻塞刷新**，一段拖动累计 21 次。
		// 这与门框砖包围盒那条是同一个失败模式（"值连续变 ⇒ 资源每帧重来"），解法也同一个：
		// 留 50% 余量 + 对齐到 4096。代价是显存多留一点（一行 80 B，4096 行 = 320 KB）。
		const uint32 MaxRecords = uint32(FMath::Clamp(
			CSShaperSteps::ReserveCount(MaxStrands * FMath::Clamp(VineMaxSegments, 1, 128)), 64, 1 << 16));

		// 交接包围盒：量化 + 只涨不缩，理由与门框砖那段逐字相同（拖尺寸时 1 cm 阈值会让
		// 每帧都重走一次阻塞的 SetInstanceSourceGPU）。藤爬满整面墙，所以取整个 footprint。
		const double Reach = CSShaperSteps::QuantizeUp(Footprint.GetCenteredSpan() * 0.6
			+ FMath::Max(VineLeafSize, VineFlowerSize) + VineStandOff);
		// ⚠️ 上界取**屋脊高**而不是墙高：藤自己只爬到檐口，但瓦 / 尖顶那一层将来也吃这只盒子，
		// 停在墙高的话斜看时会成片被剔掉（症状是"转个视角就闪没"，最难复现的那一类）。
		// `CSShaperSteps::QuantizeUp` 只涨不缩，所以这一项不是每帧变的量。
		const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc())
			+ FMath::Max(VineLeafSize, VineFlowerSize));
		FBox LocalBounds(FVector(-Reach, -Reach, -VineLeafSize), FVector(Reach, Reach, Top));
		// 三个调色板的组件是同一棵挂接树上的兄弟、变换相同，取叶那一个即可（藤没有叶就不会走到这里）。
		LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), VineLeafComponent.Get(), LocalBounds);
		OutMaxRecords = MaxRecords;
		OutLocalBounds = LocalBounds;
	}, bForceFullRebuild);
}

void ACSHouseActor::RebuildVine()
{
	// ⚠️ 管子模式下**不再要求** `VineBranchMesh`：枝已经不是实例，那个资产只服务旧路。
	// 忘了改这一条的症状是"把枝网格清空之后连叶子也没了"，而两条路各自都没错。
	if (!bVineEnabled || (!bVineUseTube && !VineBranchMesh) || !VineLeafMesh)
	{
		if (CurrentVineSegmentCount != 0 || CurrentVineLeafCount != 0 || CurrentVineFlowerCount != 0)
		{
			// 先清 counter 再撤实例源、管子与交接缓存（顺序的理由见 `ACSWallBase::ClearVineRig`）。
			ClearVineRig();
			CurrentVineSegmentCount = 0;
			CurrentVineLeafCount = 0;
			CurrentVineFlowerCount = 0;
			VineDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult VineHandoverResult = EnsureVineComponents();
	if (!bVineBaseMeshReady) return;

	TArray<CSHouseVine::FWallStrip> Strips;
	BuildVineStrips(Strips);

	// 参数表与样条墙同一份（`FCSWallVineSettings` → `FParams`，花网格没配时花的概率钉 0）。
	const FCSWallVineSettings Vine = GetVineSettings();
	const CSHouseVine::FParams Params = MakeVineParams(Vine);

	CSHouseVine::FPlan Plan;
	CSHouseVine::BuildPlan(Strips, CurrentOpenings, Params, Plan);

	// 哈希用的空隙摘要：取四面墙采样里的**最大值**。最大值正好是"最悬空的那一点"，
	// 也就是判据真正会翻转的那个量；取平均会让一角翘起被别处摊平。
	double GroundGapSummary = 0.0;
	for (const CSHouseVine::FWallStrip& S : Strips)
	{
		for (float G : S.GroundGaps) GroundGapSummary = FMath::Max(GroundGapSummary, double(G));
	}

	// 幂等短路。哈希覆盖"会改变藤的形态的一切"：摆位 + 尺寸 + 洞集合 + 参数。
	// ⚠️ 短路点在 BuildPlan **之后**是有意的：规划是纯 CPU、微秒量级，而它同时是哈希的
	// 唯一诚实来源 —— 拿参数拼一个哈希而不跑规划，会在"参数没变但洞变了"时静默漏更新。
	const TArray<int32> HashInput = {
		int32(Plan.Branch.Num()), int32(Plan.Leaf.Num()), int32(Plan.Flower.Num()),
		CSHouse_Q(FootprintSize.X, 1), CSHouse_Q(FootprintSize.Y, 1), CSHouse_Q(WallHeight, 1),
		CSHouse_Q(VineStrandSpacing, 0.5), CSHouse_Q(VineSegmentLength, 0.5), VineMaxSegments,
		CSHouse_Q(VineWander, 0.01), CSHouse_Q(VineMaxLean, 0.01), CSHouse_Q(VineMaxTurn, 0.01),
		CSHouse_Q(VineBloat, 0.01),
		CSHouse_Q(VineThickness, 0.1), CSHouse_Q(VineStandOff, 0.1), CSHouse_Q(VineLeafSize, 0.1),
		CSHouse_Q(VineLeafChance, 0.01), CSHouse_Q(VineLeafSizeJitter, 0.01), VineSeed,
		CSHouse_Q(VineFlowerChance, 0.01), CSHouse_Q(VineFlowerFromFrac, 0.01), CSHouse_Q(VineFlowerSize, 0.1),
		CSHouse_Q(VineJumpChance, 0.01), CSHouse_Q(VineMaxGroundGap, 1),
		CSHouse_Q(VineTipTaperLength, 1), CSHouse_Q(VineTipTaperMin, 0.01),
		// 速度进哈希：它参与 `ResolveVineSpawnTimes` 的"前沿到哪儿了"判断，改它等于换一套相位。
		CSHouse_Q(VineGrowSpeed, 1),
		// ⚠️ 空隙本身必须进哈希：它由**地面**决定，而地面变化只发一个"重求值"的广播，
		// 房子的摆位与参数一个都没变。不进哈希的症状是"把地面拉低、柱子冒出来了、藤还在"。
		CSHouse_Q(GroundGapSummary, 1),
		// 四坡以后四面墙顶一律平在 WallHeight（已在上面进哈希），屋面参数不再决定藤怎么排。
		int32(ComputePlacementHash() & 0x7FFFFFFF) };
	// 顶点表另拼：藤条沿每条边排，形状变了而包围盒尺寸没变时（异形房子改 `FootprintShape`）也必须重排。
	TArray<int32> FootprintHash;
	CSHouse_AppendFootprintHash(FootprintHash, GetFootprint());
	const uint32 NewHash = HashCombine(CSHouse_Hash(HashInput), CSHouse_Hash(FootprintHash));

	const bool bBuffersReady = AreVineBuffersReady();
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重打包，哈希没变也不能早退（审查 B1）。
	const bool bMustRepack = VineHandoverResult == CSShaperSteps::EHandoverResult::HandedOver;
	if (!bMustRepack && NewHash == VineDescHash && VineHandover.Capacities.Num() == CSHouseVine::Palette_Num && bBuffersReady) return;

	VineDescHash = NewHash;
	CurrentVineSegmentCount = Plan.Branch.Num();
	CurrentVineLeafCount = Plan.Leaf.Num();
	CurrentVineFlowerCount = Plan.Flower.Num();

	// 生长相位 → 叶 / 花回填 → 打包实例 → 枝的管子：墙的基类 `ACSWallBase::PackVine`（样条墙同一份）。
	// 计数（CurrentVineSegmentCount）在上面已经取过 —— 管子模式下它会把枝记录摘掉，日志与哈希都不受影响。
	PackVine(Vine, Params, Strips, Plan);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s vine packed: strips=%d strands=%d branches=%d leaves=%d flowers=%d"),
		*GetName(), Strips.Num(), Plan.Strands.Num(), CurrentVineSegmentCount, CurrentVineLeafCount, CurrentVineFlowerCount);
}

bool ACSHouseActor::IsVineDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
	OutReason = GetVineUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测，与 IsDecorDrawable 同一条）：
		// UE Python 把"bool 返回值 + 一个 out 参数"收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到 `None`**，原因串直接丢了。脚本要拿原因请调 GetVineUndrawableReason()。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s vine not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

bool ACSHouseActor::IsVineSuppressedByGroundGap() const
{
	// 与 `CSHouseVine::BuildPlan` 里那条判据**同一个量、同一个阈值**：藤脚处的地面空隙。
	// 这里取四面墙采样的最大值 —— 只要还有一处贴地，藤就该长得出来，那时零藤才是真缺陷。
	TArray<CSHouseVine::FWallStrip> Strips;
	BuildVineStrips(Strips);
	if (Strips.IsEmpty()) return false;

	float MinGap = TNumericLimits<float>::Max();
	for (const CSHouseVine::FWallStrip& S : Strips)
	{
		// 空采样 = 不知道 = 按贴地处理（同 `FWallStrip::SampleGroundGap` 的口径）。`BuildVineStrips`
		// 如今总会填满（无地面处写 `NoGroundGap`），这条只剩防御意义。
		if (S.GroundGaps.IsEmpty()) return false;
		for (float G : S.GroundGaps) MinGap = FMath::Min(MinGap, G);
	}
	return MinGap > VineMaxGroundGap;
}

FString ACSHouseActor::GetVineUndrawableReason() const
{
	FlushPendingReevaluate();
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bVineEnabled) { OutReason = TEXT("bVineEnabled 关着"); return OutReason; }

	// **悬空是合法的"没有藤"，不是"藤画不出来"**（用户裁决 2026-09-06）。
	// 这一环必须排在所有资产/渲染判据**之前**：悬空的房子既没有实例也没有管子，
	// 后面每一条都会挨个报红，而它们说的都是同一件不成立的事。
	//
	// ⚠️ 用**规划的结果**（一根藤都没排出来）配合空隙判据，而不是只看空隙 ——
	// 半边悬空的房子仍然该长藤，那时空隙超阈但藤是有的，不能一并放过。
	if (CurrentVineSegmentCount <= 0 && IsVineSuppressedByGroundGap())
	{
		return OutReason;   // 空串 = 没问题
	}

	if (!bVineUseTube && !VineBranchMesh) { OutReason = TEXT("没有 VineBranchMesh"); return OutReason; }
	if (!VineLeafMesh) { OutReason = TEXT("没有 VineLeafMesh"); return OutReason; }
	if (!bVineBaseMeshReady) { OutReason = TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）"); return OutReason; }
	if (CurrentVineSegmentCount <= 0) { OutReason = TEXT("一段藤都没排出来"); return OutReason; }

	const UCSGpuInstancedMeshComponent* Components[CSHouseVine::Palette_Num] =
		{ VineBranchComponent, VineLeafComponent, VineFlowerComponent };
	const TCHAR* Names[CSHouseVine::Palette_Num] = { TEXT("枝"), TEXT("叶"), TEXT("花") };
	for (int32 Index = 0; Index < CSHouseVine::Palette_Num; ++Index)
	{
		// 花是可选的：没配网格就整条跳过，而不是报"画不出来"—— 那会把一栋**正确**的
		// 无花房子判成红灯，判据一旦有假红，下一次真红就没人信了。
		if (Index == CSHouseVine::Palette_Flower && !VineFlowerMesh) continue;
		const UCSGpuInstancedMeshComponent* Component = Components[Index];
		if (!IsValid(Component)) { OutReason = FString::Printf(TEXT("%s：没有渲染组件"), Names[Index]); return OutReason; }
		if (!Component->IsRegistered()) { OutReason = FString::Printf(TEXT("%s：渲染组件没注册"), Names[Index]); return OutReason; }
		if (!Component->IsVisible()) { OutReason = FString::Printf(TEXT("%s：渲染组件不可见"), Names[Index]); return OutReason; }
		if (!Component->HasInstanceSourceGPU()) { OutReason = FString::Printf(TEXT("%s：实例源没交接"), Names[Index]); return OutReason; }
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			OutReason = FString::Printf(TEXT("%s：基础网格快照是空的"), Names[Index]);
			return OutReason;
		}
		if (!Component->GetGpuMesh()) { OutReason = FString::Printf(TEXT("%s：GPU 网格没分配"), Names[Index]); return OutReason; }

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 ——
		// 画面上是一片灰，而所有 readback 断言照绿。
		const UMaterialInterface* Material = Component->InstanceMaterial;
		if (!Material)
		{
			OutReason = FString::Printf(TEXT("%s：没有绑材质（会用引擎默认表面材质画成一片灰）"), Names[Index]);
			return OutReason;
		}
		// ⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在实例路径上
		// 会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。现成的 `MI_ivy_*`
		// 全都挂在 `M_TG_Texture` 下，而它恰恰没勾 —— 这是本模块最容易中的一枪。
		const UMaterial* Base = Material->GetMaterial();
		if (!Base || !Base->bUsedWithInstancedStaticMeshes)
		{
			OutReason = FString::Printf(
				TEXT("%s：材质 '%s' 的母材质没有勾 bUsedWithInstancedStaticMeshes（引擎会静默换成默认材质）"),
				Names[Index], *Material->GetName());
			return OutReason;
		}
		// 🆕 **第三条"静默换默认材质"**（2026-08-31 现场踩到）：母材质**编译失败**时引擎同样
		// 静默退回默认材质，与上面两条逐像素同症状 —— 叶子整片暗蓝黑，而上面那两条全绿。
		// 现场是三季混合里 `MaterialExpressionClamp` 的针脚接不上（MEL 的连线函数**返回 false
		// 不抛异常**），材质带着 `Missing Clamp input` 存了盘，日志里只有一条 LogMaterial Warning。
		// ⚠️ 这一环也会在**异步编译还没完**时为真。措辞因此把两种情况一起说 —— 对调用方来说
		// 两者的动作是同一个（等一下再看 / 去看 LogMaterial），把它们拆开反而会诱导人忽略前者。
		if (const_cast<UMaterial*>(Base)->IsCompilingOrHadCompileError(GMaxRHIShaderPlatform))
		{
			OutReason = FString::Printf(
				TEXT("%s：母材质 '%s' 还在编译、或者编译失败（引擎会静默换成默认材质；去看 LogMaterial）"),
				Names[Index], *Base->GetName());
			return OutReason;
		}
	}
	return FString();
}

// -----------------------------------------------------------------------------
// 装饰摆件（D12 的锚点那一半）
//
// ⚠️ 计划 D12 的「复杂度场 `RT_DecorField` + tile-argmax」**不在这里**，也不在别处 ——
// 它在 TG 里没有对位物，取舍是挂起的决策 **C2**。本节只做锚点：围着已经存在的构件长。
// 详见 `CSHouseDecor.h` 的文件头。
// -----------------------------------------------------------------------------

void ACSHouseActor::BuildDecorSite(CSHouseDecor::FSite& OutSite) const
{
	// 墙矩形与藤蔓共用同一份 `BuildVineStrips`（它自己又走 `CSHouse_GetEdge`）——
	// 墙在哪儿只能有一个真源，各抄一份的症状是"摆件悬在离墙半个墙厚的空中"，
	// 而且只在改过 WallThickness 之后才显形。
	BuildVineStrips(OutSite.Strips);
	OutSite.Openings = CurrentOpenings;
	// 屋面 desc 也只有一个真源（计划 D4）：檐口/屋脊的高度由 `CSHouseRoof_*` 求值，
	// 这里一条屋顶方程都不写。
	OutSite.Roof = GetRoofDesc();
	OutSite.World = GetBuildTransform();
	OutSite.BaseZ = GetActorLocation().Z;

	// 落高与道路排除走地面镜像的 CPU 采样器 —— 与 `ComputeDoors` 同一条路，是权威层，
	// 而且**纯 CPU**：装饰这条链一次回读都不会有（计划 D12 的异步回读是给场准备的，
	// 锚点这一半根本不需要）。
	if (const ACSGroundActor* G = Ground)
	{
		OutSite.SampleGroundZ = [G](const FVector2D& XY) { return G->SampleHeight(XY); };
		OutSite.SampleRoadWeight = [G](const FVector2D& XY) { return G->SampleRoadWeight(XY); };
	}
}

CSHouseDecor::FParams ACSHouseActor::MakeDecorParams() const
{
	CSHouseDecor::FParams Params;
	Params.WallFootSpacing = DecorWallFootSpacing;
	Params.EaveSpacing = DecorEaveSpacing;
	// 屋脊比檐口稀一档：脊线只有**一条**而檐口有两条，按同一密度摆会在屋顶正中排成一串珠子。
	Params.RidgeSpacing = DecorEaveSpacing * 1.13f;
	Params.MinSpacing = DecorMinSpacing;
	Params.RoadReject = DecorRoadReject;
	Params.BaseScale = DecorScale;
	Params.ScaleJitter = DecorScaleJitter;
	Params.Seed = DecorSeed;
	return Params;
}

// -----------------------------------------------------------------------------
// 屋面瓦（四坡的屋面本体）
// -----------------------------------------------------------------------------

CSHouseTile::FParams ACSHouseActor::MakeRoofTileParams() const
{
	CSHouseTile::FParams Params;
	Params.RowPitch = RoofTileRowPitch;
	Params.ColumnPitch = RoofTileColumnPitch;
	Params.RowOverlap = RoofTileRowOverlap;
	Params.ColumnOverlap = RoofTileColumnOverlap;
	Params.Thickness = RoofTileThickness;
	Params.SizeScale = RoofTileSizeScale;
	Params.RidgeCapScale = RoofRidgeCapScale;
	Params.StandOff = RoofTileStandOff;
	Params.ScaleJitter = RoofTileScaleJitter;
	Params.LengthJitter = RoofTileLengthJitter;
	Params.YawJitter = RoofTileYawJitter;
	Params.LiftJitter = RoofTileLiftJitter;
	Params.Seed = RoofTileSeed;
	Params.Axes = RoofTileAxes;
	return Params;
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureRoofTileComponent()
{
	// 新组件身上没有基础网格快照，得重建。
	if (CSShaperSteps::EnsureInstancedComponent(this, RoofTileComponent)) bRoofTileBaseMeshReady = false;
	RoofTileComponent->SetInstanceMaterial(RoofMaterial);

	if (RoofTileGpuBuffers.Num() != 1)
	{
		CSShaperSteps::ReleaseOnRenderThread(RoofTileGpuBuffers);
		RoofTileGpuBuffers.SetNum(1);
		RoofTileHandover.Capacities.Reset();
		bRoofTileBaseMeshReady = false;
	}

	// 换过网格资产就必须重建快照（同 `VineBranchMeshBuiltFrom` 的字段注释：只靠一个 bool
	// 会出现"换了资产但什么都没发生"）。
	if (RoofTileMeshBuiltFrom != RoofTileMesh) bRoofTileBaseMeshReady = false;

	if (!bRoofTileBaseMeshReady)
	{
		// Preserve the asset and its Nanite resources so GPU instances participate in VSM.
		const bool bOk = IsValid(RoofTileMesh);
		if (bOk)
		{
			RoofTileComponent->SetBaseMesh(RoofTileMesh);
			RoofTileComponent->SetCastShadow(true);

			const FBox3f Local(RoofTileMesh->GetBoundingBox());
			const FVector3f Size = Local.IsValid ? Local.GetSize() : FVector3f(1.0f, 1.0f, 1.0f);

			CSShaperSteps::FPaletteBuffers& Buffers = RoofTileGpuBuffers[0];
			Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
			Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
			// 记录里的尺寸是**世界 cm**，所以块尺寸取 1/网格自身尺寸（同藤蔓那条手法）——
			// 逐排改瓦的大小就不必回头动 BlockSize。
			Buffers.BlockSize = FVector3f(
				1.0f / FMath::Max(Size.X, 0.01f),
				1.0f / FMath::Max(Size.Y, 0.01f),
				1.0f / FMath::Max(Size.Z, 0.01f));

			// 法线轴从包围盒判：瓦是一块**扁片** ⇒ 最薄的一轴就是屋面法线。
			const float Ext[3] = { Size.X, Size.Y, Size.Z };
			int32 NormalAxis = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (Ext[Axis] < Ext[NormalAxis]) NormalAxis = Axis;
			}
			RoofTileAxes.Normal = NormalAxis;
			RoofTileAxes.NormalSign = 1.0f;
			RoofTileAxes.NativeSize = Size;
			// 枢轴不在中心的瓦资产靠这一项对齐（见 `FMeshAxes::NativeCentre`）。
			RoofTileAxes.NativeCentre = Buffers.BaseSphereCentre;
			UE_LOG(LogTinyGladeHouse, Log,
				TEXT("[TinyGladeHouse] %s roof tile mesh: size=(%.1f, %.1f, %.1f) normal axis=%d"),
				*GetName(), Size.X, Size.Y, Size.Z, NormalAxis);
		}
		RoofTileMeshBuiltFrom = bOk ? RoofTileMesh : nullptr;
		bRoofTileBaseMeshReady = bOk;
	}
	if (!bRoofTileBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 面内那两轴**每次都重算**，不能留在上面那个只跑一次的快照块里：`bRoofTileSwapAxes` 是
	// 交互旋钮，判定留在快照里的话勾了它什么都不会发生（"换了资产但什么都没发生"的同族失效）。
	// 哪根朝坡上从形状分不出来（两者都在瓦面内），默认把**长的**给上坡向。
	{
		const float Ext[3] = { RoofTileAxes.NativeSize.X, RoofTileAxes.NativeSize.Y, RoofTileAxes.NativeSize.Z };
		const int32 A = (RoofTileAxes.Normal + 1) % 3;
		const int32 B = (RoofTileAxes.Normal + 2) % 3;
		const bool bTakeA = (Ext[A] >= Ext[B]) != bRoofTileSwapAxes;
		RoofTileAxes.UpSlope = bTakeA ? A : B;
		RoofTileAxes.AlongRow = bTakeA ? B : A;
	}

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是 FootprintSize 的**连续函数**（边长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一列就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	const CSHouseTile::FParams Params = MakeRoofTileParams();
	const FCSRoofDesc Roof = GetRoofDesc();
	const int32 Bound = CSHouseTile::MaxTilesBound(Roof, Params);
	const uint32 MaxRecords = uint32(FMath::Clamp(CSShaperSteps::ReserveCount(Bound), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(RoofTileGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤 / 摆件那三段逐字相同（1 cm 阈值会让
	// 拖尺寸时每帧都重走一次阻塞的 SetInstanceSourceGPU）。
	// MeshReach 是"一块瓦最多伸多远"的**保守**估计：拿目标间距 × 重叠 × 抖动上界再乘 2，
	// 反正它只用来给剔除盒留边，宁可大一点也不要在视锥边缘闪掉整片屋面。
	const double TileReach = 2.0 * double(FMath::Max(
		FMath::Max(RoofTileRowPitch, RoofTileColumnPitch),
		FMath::Max(RoofTileAxes.NativeAlongSlope(), RoofTileAxes.NativeAcrossRow())))
		* double(FMath::Max(RoofTileRowOverlap, RoofTileColumnOverlap))
		* (1.0 + double(RoofTileScaleJitter)) * (1.0 + double(RoofTileLengthJitter));
	// `M_TinyGladeRoof` 的 WPO（瓦端补抬 ≤ 3 cm + 水平抖动每轴 ±3 cm）远小于 TileReach，不另加余量。
	const double Reach = CSShaperSteps::QuantizeUp(
		GetFootprint().GetCenteredSpan() * 0.5 + double(RoofOverhang) + TileReach);
	const double Top = CSShaperSteps::QuantizeUp(
		double(CSHouseRoof_RidgeZ(Roof)) + double(RoofTileStandOff) + double(RoofTileLiftJitter) + TileReach);
	FBox LocalBounds(FVector(-Reach, -Reach, -TileReach), FVector(Reach, Reach, Top));
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(), RoofTileComponent, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, RoofTileHandover, bForceFullRebuild);
	return CSShaperSteps::HandOverInstanceSource(
		CSShaperSteps::MakeHandoverSource(RoofTileComponent, RoofTileGpuBuffers[0], /*bWithCustomData*/ true),
		LocalBounds, RoofTileHandover);
}

void ACSHouseActor::RebuildFlatRoof()
{
	if (!FlatRoofParapetComponent) return;
	TArray<FCSRoofParapetBlock> Blocks;
	TArray<FCSRoofParapetOpening> Openings;
	if (IsFlatRoof())
	{
		for (TActorIterator<ACSStairsActor> It(GetWorld()); It; ++It)
		{
			TArray<FCSStairsTerraceLink> Links;
			It->GetTerraceConnections(Links);
			for (const FCSStairsTerraceLink& Link : Links) if (Link.House.Get() == this) Openings.Add(Link.Connection.Opening);
		}
		if (FrameBrickMesh) CSHouseRoof_BuildParapet(GetRoofDesc(), WallThickness, FlatRoofParapetHeight, Blocks, Openings);
	}
	CurrentTerraceEntranceCount = Openings.Num();
	TArray<int32> HashInput;
	HashInput.Append({ int32(GetTypeHash(FrameBrickMesh)), int32(GetTypeHash(FrameMaterial)), Blocks.Num() });
	const FTransform World = GetBuildTransform();
	const FTransform Component = FlatRoofParapetComponent->GetComponentTransform();
	for (const FTransform& T : { World, Component })
	{
		for (const FVector& V : { T.GetLocation(), T.GetRotation().Euler(), T.GetScale3D() })
		{
			HashInput.Append({ CSHouse_Q(V.X, 0.001), CSHouse_Q(V.Y, 0.001), CSHouse_Q(V.Z, 0.001) });
		}
	}
	TArray<FTransform> Transforms;
	if (FrameBrickMesh)
	{
		const FBox Box = FrameBrickMesh->GetBoundingBox();
		const FVector MeshSize = Box.GetSize().ComponentMax(FVector(0.01));
		for (const FCSRoofParapetBlock& B : Blocks)
		{
			const FVector Scale = B.Size / MeshSize;
			Transforms.Add(FTransform(B.Rotation, B.Center - B.Rotation.RotateVector(Box.GetCenter() * Scale), Scale) * World);
			HashInput.Append({ CSHouse_Q(B.Center.X, 0.01), CSHouse_Q(B.Center.Y, 0.01), CSHouse_Q(B.Center.Z, 0.01),
				CSHouse_Q(B.Size.X, 0.01), CSHouse_Q(B.Size.Y, 0.01), CSHouse_Q(B.Size.Z, 0.01), CSHouse_Q(B.Rotation.GetAngle(), 0.001) });
		}
	}
	const uint32 Hash = CSHouse_Hash(HashInput);
	if (Hash == FlatRoofParapetHash && FlatRoofParapetComponent->GetInstanceCount() == Transforms.Num()
		&& FlatRoofParapetComponent->BaseMesh == FrameBrickMesh) return;
	FlatRoofParapetHash = Hash;
	FlatRoofParapetComponent->SetBaseMesh(FrameBrickMesh);
	FlatRoofParapetComponent->SetInstanceMaterial(FrameMaterial);
	if (Transforms.IsEmpty()) FlatRoofParapetComponent->ClearInstances();
	else FlatRoofParapetComponent->SetInstances(Transforms, true);
}

void ACSHouseActor::RebuildRoofTiles()
{
	if (IsFlatRoof() || !bRoofTilesEnabled || !RoofTileMesh)
	{
		if (CurrentRoofTileCount != 0)
		{
			// ⚠️ 同 `RebuildVine` / `RebuildDecor`：撤实例源之前先清 counter，
			// 否则下一次 `EnsureRoofTileComponent` 把同一批带陈旧计数器的 buffer 交回组件。
			CSShaperSteps::ZeroCounters(RoofTileGpuBuffers);
			if (IsValid(RoofTileComponent)) RoofTileComponent->ClearInstanceSourceGPU();
			RoofTileHandover.Reset();
			CurrentRoofTileCount = 0;
			RoofTileDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult RoofTileHandoverResult = EnsureRoofTileComponent();
	if (!bRoofTileBaseMeshReady || RoofTileGpuBuffers.Num() != 1) return;

	const FCSRoofDesc Roof = GetRoofDesc();
	const CSHouseTile::FParams Params = MakeRoofTileParams();

	// 幂等短路：瓦是 (屋面 desc, 参数, 摆位) 的**纯函数** —— 不读门、不读地面、不读道路，
	// 所以拿输入拼哈希是诚实的（摆件那条不行，它的锚点吃地面采样，只能拿产物拼）。
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1), CSHouse_Q(RoofHeightOffset, 0.1),
		CSHouse_Q(RoofTileRowPitch, 0.1), CSHouse_Q(RoofTileColumnPitch, 0.1),
		CSHouse_Q(RoofTileRowOverlap, 0.01), CSHouse_Q(RoofTileColumnOverlap, 0.01),
		CSHouse_Q(RoofTileThickness, 0.1), CSHouse_Q(RoofTileSizeScale, 0.01),
		CSHouse_Q(RoofRidgeCapScale, 0.01),
		CSHouse_Q(RoofTileStandOff, 0.1), CSHouse_Q(RoofTileScaleJitter, 0.01), CSHouse_Q(RoofTileLengthJitter, 0.01),
		CSHouse_Q(RoofTileYawJitter, 0.01), CSHouse_Q(RoofTileLiftJitter, 0.1), RoofTileSeed,
		// 网格换了（尺寸/轴向跟着变）也要重排 —— 只认指针的话换成同尺寸的另一张瓦不会重排。
		RoofTileAxes.UpSlope, RoofTileAxes.AlongRow, RoofTileAxes.Normal,
		CSHouse_Q(RoofTileAxes.NativeSize.X, 0.1), CSHouse_Q(RoofTileAxes.NativeSize.Y, 0.1),
		CSHouse_Q(RoofTileAxes.NativeSize.Z, 0.1),
		CSHouse_Q(RoofTileAxes.NativeCentre.X, 0.1), CSHouse_Q(RoofTileAxes.NativeCentre.Y, 0.1),
		CSHouse_Q(RoofTileAxes.NativeCentre.Z, 0.1) };
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	CSHouse_AppendRoofRidgeHash(HashInput, RoofRidgeLength01, RoofRidgeAxis, GableRoofStyle);
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// 交接结果也进早退门（审查 B1）：容量比对挡的是"扩容后没重排"，交接结果还多挡一种 ——
	// 包围盒变了、同一批 buffer 重新交出去的那一趟，重排一次是便宜的保险。
	if (RoofTileHandoverResult != CSShaperSteps::EHandoverResult::HandedOver
		&& NewHash == RoofTileDescHash && RoofTileHandover.Capacities.Num() == 1
		&& RoofTileHandover.Capacities[0] == RoofTileGpuBuffers[0].Capacity
		&& RoofTileGpuBuffers[0].IsValid())
	{
		return;
	}
	RoofTileDescHash = NewHash;

	TArray<CSHouseTile::FRecord> Tiles;
	CSHouseTile::BuildPlan(Roof, GetBuildTransform(), Params, Tiles);
	CurrentRoofTileCount = Tiles.Num();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的（同摆件那段：已删的门框旧路
	// 混用 `GetBuildTransform()` 与 actor 的完整逆变换，房子一旦被 pitch/roll 或缩放就错位）。
	const FMatrix44f WorldToComponent = FMatrix44f(
		RoofTileComponent->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseTile::Pack(Tiles, RoofTileGpuBuffers[0], WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s roof tiles packed: tiles=%d capacity=%u"),
		*GetName(), CurrentRoofTileCount, RoofTileGpuBuffers[0].Capacity);
}

FString ACSHouseActor::GetRoofTileUndrawableReason() const
{
	if (IsFlatRoof()) return TEXT("露台形态不铺瓦");
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bRoofTilesEnabled) return TEXT("bRoofTilesEnabled 关着");
	if (!RoofTileMesh) return TEXT("没有 RoofTileMesh");
	if (!bRoofTileBaseMeshReady) return TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）");
	if (CurrentRoofTileCount <= 0) return TEXT("一块瓦都没排出来");
	if (!IsValid(RoofTileComponent)) return TEXT("没有渲染组件");
	if (!RoofTileComponent->IsRegistered()) return TEXT("渲染组件没注册");
	if (!RoofTileComponent->IsVisible()) return TEXT("渲染组件不可见");
	if (!RoofTileComponent->HasInstanceSourceGPU()) return TEXT("实例源没交接");
	if (RoofTileComponent->IsNaniteRenderPath())
	{
		if (!RoofTileComponent->GetNaniteComponent()) return TEXT("屋瓦 Nanite 替身未创建");
	}
	else if (!RoofTileComponent->GetGpuMesh()) return TEXT("GPU 网格没分配");
	const FString MaterialReason = RoofTileComponent->GetMaterialUndrawableReason();
	if (!MaterialReason.IsEmpty()) return MaterialReason;
	return FString();
}

void ACSHouseActor::RebuildRoofFinials()
{
	// 收工：网格撤了就把组件全销毁。**不是隐藏** —— 留着的话换资产时会先画一帧旧网格。
	auto Discard = [this](int32 KeepCount)
	{
		for (int32 Index = RoofFinialComponents.Num() - 1; Index >= KeepCount; --Index)
		{
			if (IsValid(RoofFinialComponents[Index])) RoofFinialComponents[Index]->DestroyComponent();
			RoofFinialComponents.RemoveAt(Index);
		}
	};

	const FCSRoofDesc Roof = GetRoofDesc();
	// 双坡不立尖顶（2026-10-02）：脊的两端压着山墙（阶梯式的山墙还高出屋面 46 cm），尖顶立上去只会和山墙穿在一起。
	// TG 更严 —— `place_roof_spires` 的闭包 1419EEA30 只给**没有脊**的屋顶（脊矩形长宽比 < 1.01）放一个塔尖，
	// 有脊就一个都不放；四坡脊端的两根尖顶是本项目 2026-08-31 自己的裁决，这里不动它。
	if (Roof.bFlat || !RoofFinialMesh || Roof.HasGable())
	{
		if (CurrentRoofFinialCount != 0 || RoofFinialComponents.Num() != 0)
		{
			Discard(0);
			CurrentRoofFinialCount = 0;
			RoofFinialDescHash = 0;
		}
		return;
	}

	// 尖顶立在最高那段脊的两端（骨架的 TopA / TopB）。脊长收到 0（正方形、正多边形）时两端重合
	// ⇒ 只立一根，金字塔尖不需要特例。阈值取 1 cm：比这更短的"脊"两根尖顶会互相穿模，而肉眼分不出一根两根。
	FCSRoofSkeleton Skeleton;
	CSHouseRoof_BuildSkeleton(Roof, Skeleton);
	const int32 WantCount = FVector2D::Distance(Skeleton.TopA, Skeleton.TopB) > 1.0 ? 2 : 1;

	// 竖直轴从包围盒判：尖顶是**一根细长的东西** ⇒ 最长的一轴就是它朝上那根。
	// （TG 的 `roof_spire` 是 y-up 导出的，不判轴直接进 UE 会躺倒。）
	const FBox Local = RoofFinialMesh->GetBoundingBox();
	const FVector Size = Local.GetSize();
	const double Ext[3] = { Size.X, Size.Y, Size.Z };
	int32 UpAxis = 0;
	for (int32 Axis = 1; Axis < 3; ++Axis)
	{
		if (Ext[Axis] > Ext[UpAxis]) UpAxis = Axis;
	}

	const FTransform Build = GetBuildTransform();
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofHeightOffset, 0.1),
		CSHouse_Q(RoofFinialScale, 0.001), CSHouse_Q(RoofFinialSink, 0.1),
		// 资产换了必须重建（同 `RoofTileMeshBuiltFrom` 的教训：只认一个 bool 会"换了资产什么都没发生"）。
		int32(GetTypeHash(RoofFinialMesh) & 0x7FFFFFFF),
		int32(GetTypeHash(RoofFinialMaterial) & 0x7FFFFFFF),
		UpAxis, WantCount };
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	// 脊端点跟着脊长 / 脊向 / 山墙悬挑走（悬挑量又吃 `RoofOverhang`）。
	CSHouse_AppendRoofRidgeHash(HashInput, RoofRidgeLength01, RoofRidgeAxis, GableRoofStyle);
	HashInput.Add(CSHouse_Q(RoofOverhang, 1));
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// ⚠️ 组件数也要进短路条件：蓝图重跑构造脚本会把 transient 组件销毁，而形态哈希一个字没变 ——
	// 只比哈希的话尖顶会**在重跑构造脚本后永久消失**（同 `EnsureRoofTileComponent` 开头那条）。
	if (NewHash == RoofFinialDescHash && RoofFinialComponents.Num() == WantCount)
	{
		bool bAllValid = true;
		for (const TObjectPtr<UStaticMeshComponent>& Component : RoofFinialComponents)
		{
			if (!IsValid(Component)) { bAllValid = false; break; }
		}
		if (bAllValid) return;
	}
	RoofFinialDescHash = NewHash;

	Discard(WantCount);

	// 资产的"上"送到局部 +Z。尖顶绕自身轴对称 ⇒ 剩下那两根随便配一组右手基即可，
	// 不必去求"它原本朝哪"。FMatrix(X, Y, Z, O) 把 e0/e1/e2 分别送到三个轴参数上，
	// 所以把 +Z 摆在 `UpAxis` 那一格，就得到 M·(资产的上) = 局部 +Z。
	const int32 SideAxis = (UpAxis + 1) % 3;
	const int32 ThirdAxis = (UpAxis + 2) % 3;
	FVector Basis[3];
	Basis[UpAxis] = FVector::UpVector;
	Basis[SideAxis] = FVector::ForwardVector;
	Basis[ThirdAxis] = FVector::CrossProduct(Basis[UpAxis], Basis[SideAxis]);
	const FQuat Stand = FMatrix(Basis[0], Basis[1], Basis[2], FVector::ZeroVector).ToQuat();

	const float RidgeZ = CSHouseRoof_RidgeZ(Roof);
	for (int32 Index = 0; Index < WantCount; ++Index)
	{
		if (!RoofFinialComponents.IsValidIndex(Index))
		{
			UStaticMeshComponent* Created = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Created->SetupAttachment(RootComponent);
			Created->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // 纯装饰，别挡住玩家
			Created->RegisterComponent();
			RoofFinialComponents.Add(Created);
		}
		UStaticMeshComponent* Component = RoofFinialComponents[Index];
		if (!IsValid(Component)) continue;

		Component->SetStaticMesh(RoofFinialMesh);
		// 留空 = 逐槽用网格资产自带的材质（清掉覆盖，不是 `SetMaterial(nullptr)` —— 那会清成空槽画成灰）；
		// 设了 = 盖住每一个槽。
		CSHouse_ApplyMeshComponentMaterial(*Component, RoofFinialMaterial);

		const FVector2D XY = (WantCount == 2) ? ((Index == 0) ? Skeleton.TopA : Skeleton.TopB)
			: (Skeleton.TopA + Skeleton.TopB) * 0.5;
		const FVector LocalPos(XY.X, XY.Y, double(RidgeZ) - double(RoofFinialSink));
		Component->SetWorldTransform(FTransform(
			Build.GetRotation() * Stand,
			Build.TransformPosition(LocalPos),
			FVector(double(FMath::Max(RoofFinialScale, 0.01f)))));
	}

	CurrentRoofFinialCount = WantCount;
	UE_LOG(LogTinyGladeHouse, Log,
		TEXT("[TinyGladeHouse] %s roof finials: count=%d up axis=%d native=(%.1f, %.1f, %.1f) scale=%.2f"),
		*GetName(), CurrentRoofFinialCount, UpAxis, Size.X, Size.Y, Size.Z, RoofFinialScale);
}

void ACSHouseActor::RebuildDoorLeaves()
{
	// 与尖顶同一档设施：一洞一个普通 `UStaticMeshComponent` + 形态哈希短路。
	// **不走实例化路径** —— 一栋房子的门是个位数，实例化那套（母材质要勾
	// `bUsedWithInstancedStaticMeshes`，勾漏了引擎静默换默认材质画成一片灰）不值得。
	auto Discard = [this](int32 KeepCount)
	{
		for (int32 Index = DoorLeafComponents.Num() - 1; Index >= KeepCount; --Index)
		{
			if (IsValid(DoorLeafComponents[Index])) DoorLeafComponents[Index]->DestroyComponent();
			DoorLeafComponents.RemoveAt(Index);
		}
	};

	// 档位表：按每张网格**自己的**包围盒宽度升序。宽度轴与竖直轴逐张自判（见下面 PickRank）。
	struct FLeafRank
	{
		UStaticMesh* Mesh = nullptr;
		FBox Local = FBox(ForceInit);
		int32 UpAxis = 2;
		int32 WidthAxis = 0;
		int32 DepthAxis = 1;
		double NativeWidth = 0.0;
	};
	TArray<FLeafRank> Ranks;
	if (bDoorLeafEnabled)
	{
		for (const TObjectPtr<UStaticMesh>& Mesh : DoorLeafMeshes)
		{
			if (!Mesh) continue;
			FLeafRank Rank;
			Rank.Mesh = Mesh;
			Rank.Local = Mesh->GetBoundingBox();
			const FVector Size = Rank.Local.GetSize();
			const double Ext[3] = { Size.X, Size.Y, Size.Z };
			// 竖直轴 = 最长的一轴；宽度轴 = 剩下两轴里较长的那根（同 `RebuildRoofFinials` 的口径，
			// 换一张 y-up 直进的资产也立得起来）。
			Rank.UpAxis = 0;
			for (int32 Axis = 1; Axis < 3; ++Axis)
			{
				if (Ext[Axis] > Ext[Rank.UpAxis]) Rank.UpAxis = Axis;
			}
			Rank.WidthAxis = (Rank.UpAxis + 1) % 3;
			const int32 Other = (Rank.UpAxis + 2) % 3;
			if (Ext[Other] > Ext[Rank.WidthAxis]) Rank.WidthAxis = Other;
			Rank.DepthAxis = 3 - Rank.UpAxis - Rank.WidthAxis;
			Rank.NativeWidth = Ext[Rank.WidthAxis];
			if (Rank.NativeWidth > UE_KINDA_SMALL_NUMBER) Ranks.Add(Rank);
		}
		Ranks.Sort([](const FLeafRank& A, const FLeafRank& B) { return A.NativeWidth < B.NativeWidth; });
	}

	// 门扇只长在**门**上，窗和第三方注入的洞不算。
	TArray<const FCSWallOpening*> Doors;
	if (!Ranks.IsEmpty())
	{
		for (const FCSWallOpening& O : CurrentOpenings)
		{
			// 墩侧的拱（含转角一对）与拱廊子拱都不装门扇（见 `CSHouse_StyleNoLeafMask` 的注释与实拍）。
			if (O.Type != ECSOpeningType::Door || !O.IsValid()) continue;
			if ((O.StyleFlags & CSHouse_StyleNoLeafMask) != 0) continue;
			Doors.Add(&O);
		}
	}
	const int32 WantCount = Doors.Num();

	// 挑档：native 宽度不超过门扇宽的**最大**一档；一档都不够窄就用最小那档（再靠缩放收进去）。
	auto PickRank = [&Ranks](float LeafWidth) -> const FLeafRank&
	{
		int32 Best = 0;
		for (int32 Index = 0; Index < Ranks.Num(); ++Index)
		{
			if (Ranks[Index].NativeWidth <= double(LeafWidth)) Best = Index;
		}
		return Ranks[Best];
	};

	if (WantCount == 0 || Ranks.IsEmpty())
	{
		if (CurrentDoorLeafCount != 0 || DoorLeafComponents.Num() != 0)
		{
			Discard(0);
			CurrentDoorLeafCount = 0;
			DoorLeafDescHash = 0;
		}
		return;
	}

	const FCSHouseFootprint Footprint = GetFootprint();
	TArray<int32> HashInput = {
		int32(ComputePlacementHash() & 0x7FFFFFFF),
		CSHouse_Q(WallThickness, 0.5),
		CSHouse_Q(DoorLeafWidthRatio, 0.001), CSHouse_Q(DoorLeafRise, 0.001),
		CSHouse_Q(DoorLeafMaxStretch, 0.001), CSHouse_Q(DoorLeafInset, 0.1),
		int32(GetTypeHash(DoorLeafMaterial) & 0x7FFFFFFF),
		WantCount };
	CSHouse_AppendFootprintHash(HashInput, Footprint);
	// 档位表进哈希：换一张网格、加一档、或换了顺序都必须重建（同 `RoofTileMeshBuiltFrom` 的教训）。
	for (const FLeafRank& Rank : Ranks) HashInput.Add(int32(GetTypeHash(Rank.Mesh) & 0x7FFFFFFF));
	// 洞变了门扇就得跟着变。**逐洞进哈希**，只记洞数会漏掉"同样两个洞、其中一个变宽了"。
	for (const FCSWallOpening* Door : Doors)
	{
		HashInput.Append({ Door->EdgeIndex, CSHouse_Q(Door->CenterS, 1),
			CSHouse_Q(Door->Width, DoorWidthQuantum), CSHouse_Q(Door->Z1, 1) });
	}
	const uint32 NewHash = CSHouse_Hash(HashInput);
	// ⚠️ 组件数进短路条件：蓝图重跑构造脚本会销毁 transient 组件而哈希一个字没变
	// （同 `RebuildRoofFinials` 那条），只比哈希门扇会在重跑构造脚本后永久消失。
	if (NewHash == DoorLeafDescHash && DoorLeafComponents.Num() == WantCount)
	{
		bool bAllValid = true;
		for (const TObjectPtr<UStaticMeshComponent>& Component : DoorLeafComponents)
		{
			if (!IsValid(Component)) { bAllValid = false; break; }
		}
		if (bAllValid) return;
	}
	DoorLeafDescHash = NewHash;

	Discard(WantCount);

	const FTransform Build = GetBuildTransform();
	for (int32 Index = 0; Index < WantCount; ++Index)
	{
		if (!DoorLeafComponents.IsValidIndex(Index))
		{
			UStaticMeshComponent* Created = NewObject<UStaticMeshComponent>(this, NAME_None, RF_Transient);
			Created->SetupAttachment(RootComponent);
			Created->SetCollisionEnabled(ECollisionEnabled::NoCollision);   // 洞是逐像素 clip 出来的，门扇也不该挡人
			// ⚠️ **必须关 Nanite，否则一次注册多个门扇会把编辑器打崩**（2026-09-04 实测，
			// `DoorMaxWidth` 调到会切拱廊、同一帧要建 2 个以上门扇那一版）：
			//   批量注册走 `FStaticMeshComponentBulkReregisterContext` → `FScene::BatchAddPrimitives`
			//   → `ShouldCreateNaniteProxy` → `AuditMaterials` → `CheckMaterialUsage(MATUSAGE_Nanite)`
			//   → 材质缺这个用途标记 ⇒ `SetMaterialUsage` 重编译 ⇒ **`FlushRenderingCommands()`**
			//   ⇒ 在 post-tick 组件更新阶段抽干任务队列、把在途的 `EditMeshAsync` 完成回调拉进来
			//   ⇒ `MarkActorComponentForNeededEndOfFrameUpdate` 的 `!bPostTickComponentUpdate` 断言炸。
			// 门扇是二十几个顶点的板子，Nanite 对它零收益；关掉直接砍掉整条审计分支。
			// 这与"母材质没勾 `bUsedWithInstancedStaticMeshes` 会被静默换成默认材质"是同一族坑：
			// **材质用途标记要在资产上预先备好，不能等运行时注册的时候现补。**
			Created->bDisallowNanite = true;
			Created->RegisterComponent();
			DoorLeafComponents.Add(Created);
		}
		UStaticMeshComponent* Component = DoorLeafComponents[Index];
		if (!IsValid(Component)) continue;

		const FCSWallOpening& Door = *Doors[Index];
		const FCSHouseEdgeFrame F = CSHouse_GetEdge(Door.EdgeIndex, Footprint, WallThickness);

		// 门扇高：默认齐**起拱线**（拱顶那半圆留空，读作气窗）。矩形门扇顶到拱顶必然切角，
		// 所以 `DoorLeafRise` 默认 0，要顶满得先有拱形门扇网格。
		const float SpringZ = FMath::Max(Door.Z1 - Door.Rise(), 0.0f);
		const float LeafHeight = FMath::Max(FMath::Lerp(SpringZ, Door.Z1, DoorLeafRise), 1.0f);
		const float LeafWidth = FMath::Max(Door.Width * DoorLeafWidthRatio, 1.0f);

		// 挑档（TG 的 `balcony_door_rank1/2/3` 就是这么用的），再把余下的差值交给缩放。
		const FLeafRank& Rank = PickRank(LeafWidth);
		const FVector RankSize = Rank.Local.GetSize();
		const double Ext[3] = { RankSize.X, RankSize.Y, RankSize.Z };
		const int32 UpAxis = Rank.UpAxis, WidthAxis = Rank.WidthAxis, DepthAxis = Rank.DepthAxis;

		Component->SetStaticMesh(Rank.Mesh);
		// 留空 = 逐槽用网格资产自带的材质（`balcony_door_rank*` 是木 + 铁两槽）；设了 = 盖住每一个槽。
		// 同一个组件会换挂不同档的网格，所以每次都按当前网格的槽数重新套一遍。
		CSHouse_ApplyMeshComponentMaterial(*Component, DoorLeafMaterial);

		// 资产的"上"送到局部 +Z、"宽"送到局部 +X。`FMatrix(X, Y, Z, O)` 把 e0/e1/e2 分别送到三个
		// 轴参数上。深度那根取 Z×X 保右手，否则门扇会镜像。**逐档算** —— 不同档可能轴向不同。
		FVector Basis[3];
		Basis[UpAxis] = FVector::UpVector;
		Basis[WidthAxis] = FVector::ForwardVector;
		Basis[DepthAxis] = FVector::CrossProduct(Basis[UpAxis], Basis[WidthAxis]);
		const FQuat Stand = FMatrix(Basis[0], Basis[1], Basis[2], FVector::ZeroVector).ToQuat();

		// 缩放：宽按洞宽、高按门扇高。纵横比被拉得太狠时**只缩不拉** —— 板条拉成面条比门扇
		// 小一圈难看。缩放作用在**资产自己的**局部空间（Stand 之前），所以按资产轴下标写。
		float ScaleWidth = (Ext[WidthAxis] > UE_KINDA_SMALL_NUMBER) ? LeafWidth / float(Ext[WidthAxis]) : 1.0f;
		float ScaleUp = (Ext[UpAxis] > UE_KINDA_SMALL_NUMBER) ? LeafHeight / float(Ext[UpAxis]) : 1.0f;
		const float Stretch = FMath::Max(DoorLeafMaxStretch, 1.0f);
		if (ScaleUp > ScaleWidth * Stretch) ScaleUp = ScaleWidth * Stretch;
		if (ScaleWidth > ScaleUp * Stretch) ScaleWidth = ScaleUp * Stretch;
		// **厚度必须留在墙里**：门扇要靠墙的外表面遮住溢出的部分，探出去就穿帮了
		// （宽门那档 ScaleWidth 会 > 1，`balcony_door_rank*` 原生 31.6 cm 厚，
		//  乘上去比 24 cm 的墙还厚 —— 2026-09-04 顺手抓到的）。两侧各留 2 cm。
		const double NativeDepth = FMath::Max(Ext[DepthAxis], double(UE_KINDA_SMALL_NUMBER));
		const float DepthRoom = FMath::Max(WallThickness - 4.0f, 2.0f);
		const float ScaleDepth = FMath::Min(ScaleWidth, float(DepthRoom / NativeDepth));

		FVector Scale3D;
		Scale3D[WidthAxis] = ScaleWidth;
		Scale3D[DepthAxis] = ScaleDepth;
		Scale3D[UpAxis] = ScaleUp;

		// 摆位：洞心沿边。厚度方向压在**墙中面**上（`F.Start` 在外表面、`F.In` 朝内 ⇒ 中面是
		// `+In * T/2`），`DoorLeafInset` 正值再沿**外**法线（= −In）往外挪。
		const FVector2D LocalXY = F.Start + F.U * Door.CenterS
			+ F.In * (WallThickness * 0.5 - double(DoorLeafInset));
		// 竖直：把资产在"上"轴上的最小值压到 Z0（枢轴在包围盒中心时该值为负，乘缩放正好抬起来）。
		const double BaseZ = double(Door.Z0) - Rank.Local.Min[UpAxis] * ScaleUp;
		const FVector LocalPos(LocalXY.X, LocalXY.Y, BaseZ);

		// 朝向：立正之后再绕 Z 转到这面墙上 —— 门扇的宽度方向贴着 `F.U`，正面朝外。
		const FQuat Face(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(F.U.Y, F.U.X)), 0.0));
		Component->SetWorldTransform(FTransform(
			Build.GetRotation() * Face * Stand,
			Build.TransformPosition(LocalPos),
			Scale3D));
	}

	CurrentDoorLeafCount = WantCount;
	FString RankText;
	for (const FLeafRank& Rank : Ranks)
	{
		RankText += FString::Printf(TEXT("%s(%.0f) "), *Rank.Mesh->GetName(), Rank.NativeWidth);
	}
	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s door leaves: count=%d ranks=[ %s]"),
		*GetName(), CurrentDoorLeafCount, *RankText);
}

FString ACSHouseActor::GetDoorLeafUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧（同 `GetRoofFinialUndrawableReason`）——"洞开了"证明不了"门扇画出来了"。
	if (!bDoorLeafEnabled) return TEXT("bDoorLeafEnabled 关着");
	if (DoorLeafMeshes.IsEmpty()) return TEXT("DoorLeafMeshes 是空的（留空 = 有意不长门扇）");
	int32 DoorCount = 0;
	for (const FCSWallOpening& O : CurrentOpenings)
	{
		if (O.Type == ECSOpeningType::Door && O.IsValid()) ++DoorCount;
	}
	if (DoorCount <= 0) return TEXT("这栋房一个门洞都没有（路没经过任何一面墙）");
	if (CurrentDoorLeafCount <= 0) return TEXT("有门洞却一扇门都没立起来");
	if (DoorLeafComponents.Num() != CurrentDoorLeafCount)
	{
		return FString::Printf(TEXT("组件数 %d 与记账数 %d 对不上"), DoorLeafComponents.Num(), CurrentDoorLeafCount);
	}
	for (int32 Index = 0; Index < DoorLeafComponents.Num(); ++Index)
	{
		const UStaticMeshComponent* Component = DoorLeafComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("门扇 %d：组件无效"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("门扇 %d：组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("门扇 %d：组件不可见"), Index);
		if (!Component->GetStaticMesh()) return FString::Printf(TEXT("门扇 %d：网格是空的"), Index);
		if (Component->GetComponentScale().IsNearlyZero()) return FString::Printf(TEXT("门扇 %d：缩放退化成 0"), Index);
		const FString MaterialReason = CSHouse_MeshComponentMaterialReason(*Component, FString::Printf(TEXT("门扇 %d"), Index));
		if (!MaterialReason.IsEmpty()) return MaterialReason;
	}
	return FString();
}

FString ACSHouseActor::GetRoofFinialUndrawableReason() const
{
	FlushPendingReevaluate();
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（同 `GetRoofTileUndrawableReason`）。
	if (!RoofFinialMesh) return TEXT("没有 RoofFinialMesh");
	if (CurrentRoofFinialCount <= 0) return TEXT("一根尖顶都没立起来");
	if (RoofFinialComponents.Num() != CurrentRoofFinialCount)
	{
		return FString::Printf(TEXT("组件数 %d 与记账数 %d 对不上"), RoofFinialComponents.Num(), CurrentRoofFinialCount);
	}
	for (int32 Index = 0; Index < RoofFinialComponents.Num(); ++Index)
	{
		const UStaticMeshComponent* Component = RoofFinialComponents[Index];
		if (!IsValid(Component)) return FString::Printf(TEXT("第 %d 根没有组件"), Index);
		if (!Component->IsRegistered()) return FString::Printf(TEXT("第 %d 根的组件没注册"), Index);
		if (!Component->IsVisible()) return FString::Printf(TEXT("第 %d 根不可见"), Index);
		if (Component->GetStaticMesh() != RoofFinialMesh) return FString::Printf(TEXT("第 %d 根挂的不是 RoofFinialMesh"), Index);
		// 逐槽查（不只 0 号槽）：留空时画的是资产自己的每个槽，任何一个槽空着都是一片灰。
		const FString MaterialReason = CSHouse_MeshComponentMaterialReason(*Component, FString::Printf(TEXT("第 %d 根"), Index));
		if (!MaterialReason.IsEmpty()) return MaterialReason;
	}
	return FString();
}

CSShaperSteps::EHandoverResult ACSHouseActor::EnsureDecorComponents()
{
	// palette 的排布恒为「门 → 墙脚 → 屋顶」，三家各占一段；檐口与屋脊**共用**屋顶那一段
	// （TG 的 `add_birdnests` 本来就是一家两处）。窗户那家留空 ⇒ `Count == 0` ⇒ 一件都不长。
	TArray<UStaticMesh*> Wanted;
	DecorPaletteRanges.SetNum(int32(CSHouseDecor::EFamily::Count));
	for (CSHouseDecor::FPaletteRange& Range : DecorPaletteRanges) Range = CSHouseDecor::FPaletteRange();

	auto AppendFamily = [&Wanted](const TArray<TObjectPtr<UStaticMesh>>& Source, CSHouseDecor::FPaletteRange& OutRange)
	{
		OutRange.First = Wanted.Num();
		for (const TObjectPtr<UStaticMesh>& Mesh : Source)
		{
			if (Mesh) Wanted.Add(Mesh);
		}
		OutRange.Count = Wanted.Num() - OutRange.First;
	};
	AppendFamily(DecorGateMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::Gate)]);
	AppendFamily(DecorWallFootMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::WallFoot)]);
	AppendFamily(DecorRoofMeshes, DecorPaletteRanges[int32(CSHouseDecor::EFamily::Eave)]);
	DecorPaletteRanges[int32(CSHouseDecor::EFamily::Ridge)] = DecorPaletteRanges[int32(CSHouseDecor::EFamily::Eave)];

	// ⚠️ 组件数一变就必须重建基础网格快照：palette 与组件是**按下标**对齐的，少一个就全体错位。
	if (CSShaperSteps::EnsureInstancedComponents(this, DecorComponents, Wanted.Num())) bDecorBaseMeshReady = false;
	// 空 = 每个 palette 逐段画自己那张网格资产挂的材质（`barrel` = 贴图木头 + 顶点色铁箍）；设了才整体覆盖。
	// 材质表随 `CSHouseVine::BuildBaseMesh` 抄进快照，变了才让代理重建。
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents) Component->SetInstanceMaterial(DecorMaterial);

	if (DecorGpuBuffers.Num() != Wanted.Num())
	{
		CSShaperSteps::ReleaseOnRenderThread(DecorGpuBuffers);
		DecorGpuBuffers.SetNum(Wanted.Num());
		DecorHandover.Capacities.Reset();
		bDecorBaseMeshReady = false;
	}
	if (Wanted.Num() == 0) return CSShaperSteps::EHandoverResult::UpToDate;

	// 换过网格资产就必须重建快照（同 `VineBranchMeshBuiltFrom` 的字段注释）。
	bool bMeshesChanged = DecorMeshesBuiltFrom.Num() != Wanted.Num();
	for (int32 Index = 0; !bMeshesChanged && Index < Wanted.Num(); ++Index)
	{
		bMeshesChanged = DecorMeshesBuiltFrom[Index] != Wanted[Index];
	}
	if (bMeshesChanged) bDecorBaseMeshReady = false;

	if (!bDecorBaseMeshReady)
	{
		DecorMeshesBuiltFrom.SetNum(Wanted.Num());
		bool bAllOk = true;
		for (int32 Index = 0; Index < Wanted.Num(); ++Index)
		{
			FCSGpuMeshCPUData Data;
			// ⚠️ **复用藤蔓那份读取器，不是图省事**：它做的事对 clutter 同样必要 ——
			// 判"法线/UV 读出来合不合法"（有流不等于有数据）、缺了就现补、并把**顶点色**搬进快照。
			// 顶点色这一条对杂物是决定性的：`Content/HouseTest/TinyGladeAsset/Textures/` 里**没有一张 clutter 贴图**
			// （459 张贴图与 459 个 MI 一一对应，clutter 一个都不在其中），它们的颜色全烘在顶点流里。
			// 长度轴传 2（不换轴）：摆件本来就以 +Z 为上，藤那两张才需要换。
			const bool bOk = CSHouseVine::BuildBaseMesh(Wanted[Index], 2, Data);
			if (bOk)
			{
				DecorComponents[Index]->SetBaseMeshFromGpuData(Data);

				FBox3f Local(ForceInit);
				for (const FVector3f& P : Data.Positions) Local += P;
				CSShaperSteps::FPaletteBuffers& Buffers = DecorGpuBuffers[Index];
				Buffers.BaseSphereCentre = Local.IsValid ? Local.GetCenter() : FVector3f::ZeroVector;
				Buffers.BaseSphereRadius = Local.IsValid ? Local.GetExtent().Size() : 0.0f;
				// clutter 网格自带真实尺寸（不是石阶那种单位立方体字典 mesh），所以块尺寸留 1 ——
				// 缩放全部活在记录的 Scale/ScaleZ 里。
				Buffers.BlockSize = FVector3f(1.0f, 1.0f, 1.0f);
			}
			DecorMeshesBuiltFrom[Index] = bOk ? Wanted[Index] : nullptr;
			bAllOk &= bOk;
		}
		bDecorBaseMeshReady = bAllOk;
	}
	if (!bDecorBaseMeshReady) return CSShaperSteps::EHandoverResult::UpToDate;

	// 容量按**配置上限**一次付清，之后永不扩容（零阻塞纪律）。
	// ⚠️ **必须再走一次 `CSShaperSteps::ReserveCount` 的台阶**，不能把上限直接喂给 ReserveCapacity：
	// 上限是 FootprintSize 的**连续函数**（周长 / 间距），而 ReserveCapacity 只对齐到 64 ——
	// 拖尺寸时每涨过一个间距就重新分配一次。藤蔓那轮正是漏了这一步，实测一段拖动 21 次阻塞刷新。
	const CSHouseDecor::FParams Params = MakeDecorParams();
	const int32 Bound = CSHouseDecor::MaxRecordsBound(GetFootprint(), RoofOverhang, PierWidth, Params);
	const uint32 MaxRecords = uint32(FMath::Clamp(CSShaperSteps::ReserveCount(Bound), 64, 1 << 16));
	CSShaperSteps::ReserveCapacity(DecorGpuBuffers, MaxRecords);

	// 交接包围盒：量化 + 只涨不缩，理由与门框砖 / 藤蔓那两段逐字相同（1 cm 阈值会让拖尺寸时
	// 每帧都重走一次阻塞的 SetInstanceSourceGPU）。摆件最远伸到门前引道，最高到屋脊上的鸟窝。
	double MeshReach = 0.0;
	for (const CSShaperSteps::FPaletteBuffers& Buffers : DecorGpuBuffers)
	{
		MeshReach = FMath::Max(MeshReach, double(Buffers.BaseSphereRadius));
	}
	MeshReach *= FMath::Max(double(DecorScale) * (1.0 + double(DecorScaleJitter)), 0.05);
	const double Reach = CSShaperSteps::QuantizeUp(GetFootprint().GetCenteredSpan() * 0.6
		+ Params.GateApproachDistance + Params.GateApproachSpread + MeshReach);
	const double Top = CSShaperSteps::QuantizeUp(CSHouseRoof_RidgeZ(GetRoofDesc()) + Params.RoofStandOff + MeshReach);
	// 下界也取 -Reach：摆件落在**地面**上，而房子坐在 footprint 内的最高点，
	// 所以墙脚那一圈可以比房底低不少（低多少由地形说了算，不是常数）。
	FBox LocalBounds(FVector(-Reach, -Reach, -Reach), FVector(Reach, Reach, Top));
	// 各 palette 的组件是兄弟、变换相同，取第 0 个即可。
	LocalBounds = CSHouse_BuildBoundsToComponent(GetBuildTransform(),
		DecorComponents.IsValidIndex(0) ? DecorComponents[0].Get() : nullptr, LocalBounds);
	LocalBounds = CSShaperSteps::MergeHandoverBounds(LocalBounds, DecorHandover, bForceFullRebuild);

	TArray<CSShaperSteps::FHandoverSource, TInlineAllocator<8>> Sources;
	for (int32 Index = 0; Index < DecorGpuBuffers.Num(); ++Index)
	{
		Sources.Add(CSShaperSteps::MakeHandoverSource(
			DecorComponents.IsValidIndex(Index) ? ToRawPtr(DecorComponents[Index]) : nullptr,
			DecorGpuBuffers[Index], /*bWithCustomData*/ false));
	}
	return CSShaperSteps::HandOverInstanceSources(Sources, LocalBounds, DecorHandover);
}

void ACSHouseActor::RebuildDecor()
{
	auto HasAny = [](const TArray<TObjectPtr<UStaticMesh>>& Meshes)
	{
		for (const TObjectPtr<UStaticMesh>& Mesh : Meshes)
		{
			if (Mesh) return true;
		}
		return false;
	};
	const bool bAnyMesh = HasAny(DecorGateMeshes) || HasAny(DecorWallFootMeshes) || HasAny(DecorRoofMeshes);

	if (!bDecorEnabled || !bAnyMesh)
	{
		if (CurrentDecorInstanceCount != 0 || CurrentDecorAnchorCount != 0)
		{
			// ⚠️ 同 `RebuildVine` / `RebuildFrame`：撤实例源之前先清 counter，
			// 否则下一次 `EnsureDecorComponents` 把同一批带陈旧计数器的 buffer 交回组件。
			CSShaperSteps::ZeroCounters(DecorGpuBuffers);
			for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents)
			{
				if (IsValid(Component)) Component->ClearInstanceSourceGPU();
			}
			DecorHandover.Reset();
			CurrentDecorInstanceCount = 0;
			CurrentDecorAnchorCount = 0;
			CurrentDecorGateAnchorCount = 0;
			DecorDescHash = 0;
		}
		return;
	}

	const CSShaperSteps::EHandoverResult DecorHandoverResult = EnsureDecorComponents();
	if (!bDecorBaseMeshReady || DecorGpuBuffers.Num() == 0) return;

	CSHouseDecor::FSite Site;
	BuildDecorSite(Site);
	const CSHouseDecor::FParams Params = MakeDecorParams();

	TArray<CSHouseDecor::FAnchor> Anchors;
	CSHouseDecor::BuildAnchors(Site, Params, Anchors);

	CSHouseDecor::FPlan Plan;
	CSHouseDecor::BuildPlan(Anchors, Params, DecorPaletteRanges, DecorGpuBuffers.Num(), Plan);

	// 幂等短路。⚠️ 短路点在生产 + 规划**之后**是有意的（同藤蔓）：两步都是纯 CPU、微秒量级，
	// 而锚点表是哈希的**唯一诚实来源** —— 拿参数拼哈希而不跑生产，会在"参数没动但门开了 /
	// 地面被塑高了"时静默漏更新，而这两件事恰恰都会改锚点（门那一家的存亡、每个锚的落高）。
	// 所以哈希直接盖住锚点的身份与量化后的位置，地形抬高一厘米也逃不掉。
	TArray<int32> HashInput;
	HashInput.Reserve(Anchors.Num() * 5 + 20);
	HashInput.Append({ int32(ComputePlacementHash() & 0x7FFFFFFF), Anchors.Num(), Plan.TotalRecords(),
		CSHouse_Q(WallHeight, 1),
		CSHouse_Q(RoofPitch, 0.1), CSHouse_Q(RoofOverhang, 1),
		CSHouse_Q(DecorWallFootSpacing, 0.5), CSHouse_Q(DecorEaveSpacing, 0.5),
		CSHouse_Q(DecorMinSpacing, 0.5), CSHouse_Q(DecorRoadReject, 0.01),
		CSHouse_Q(DecorScale, 0.01), CSHouse_Q(DecorScaleJitter, 0.01), DecorSeed });
	CSHouse_AppendFootprintHash(HashInput, GetFootprint());
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		HashInput.Append({ int32(Anchor.Family), Anchor.AnchorId,
			CSHouse_Q(Anchor.Location.X, 1), CSHouse_Q(Anchor.Location.Y, 1), CSHouse_Q(Anchor.Location.Z, 1) });
	}
	const uint32 NewHash = CSHouse_Hash(HashInput);

	bool bBuffersReady = DecorGpuBuffers.Num() == DecorComponents.Num();
	for (const CSShaperSteps::FPaletteBuffers& Buffers : DecorGpuBuffers) bBuffersReady &= Buffers.IsValid();
	// 刚交接过（扩容换了清零的新 buffer / 包围盒变了）就必须重打包，哈希没变也不能早退（审查 B1）。
	const bool bMustRepack = DecorHandoverResult == CSShaperSteps::EHandoverResult::HandedOver;
	if (!bMustRepack && NewHash == DecorDescHash && DecorHandover.Capacities.Num() == DecorGpuBuffers.Num() && bBuffersReady) return;

	DecorDescHash = NewHash;
	CurrentDecorAnchorCount = Anchors.Num();
	CurrentDecorGateAnchorCount = 0;
	for (const CSHouseDecor::FAnchor& Anchor : Anchors)
	{
		if (Anchor.Family == CSHouseDecor::EFamily::Gate) ++CurrentDecorGateAnchorCount;
	}
	CurrentDecorInstanceCount = Plan.TotalRecords();

	// 世界 → 组件。⚠️ **用组件自己的变换求逆**，不用 actor 的：已删的门框旧路混用
	// `GetBuildTransform()`（只取 yaw）与 actor 的完整逆变换，正是状态文件「已知潜伏问题」
	// 里那条（房子一旦被 pitch/roll 或缩放就错位）。这里不重复它。
	// 各 palette 的组件是同一棵挂接树上的兄弟、变换相同，取第 0 个即可。
	const FMatrix44f WorldToComponent = FMatrix44f(
		DecorComponents[0]->GetComponentTransform().ToInverseMatrixWithScale());
	CSHouseDecor::Pack(Plan, DecorGpuBuffers, WorldToComponent);

	UE_LOG(LogTinyGladeHouse, Log, TEXT("[TinyGladeHouse] %s decor packed: anchors=%d instances=%d palettes=%d"),
		*GetName(), CurrentDecorAnchorCount, CurrentDecorInstanceCount, DecorGpuBuffers.Num());
}

bool ACSHouseActor::IsDecorDrawable(FString& OutReason) const
{
	FlushPendingReevaluate();
	OutReason = GetDecorUndrawableReason();
	if (!OutReason.IsEmpty())
	{
		// ⚠️ **原因串到不了脚本里，必须自己打一行日志**（实测）：UE Python 把
		// "bool 返回值 + 一个 out 参数"的调用收成单一返回值 —— 可画时拿到空串，
		// **不可画时拿到的是 `None`，原因串直接丢了**。于是出图/回归的红灯只能说
		// "画不出来"而说不出为什么，而这个函数存在的全部价值就在那句原因上。
		// `IsVineDrawable` / `IsRockShellDrawable` 同形、同样盲，2026-08-30 已一并补上
		// `Get*UndrawableReason()`：三处的脚本一律调那一版，原因才真的到得了日志与断言里。
		UE_LOG(LogTinyGladeHouse, Warning, TEXT("[TinyGladeHouse] %s decor not drawable: %s"), *GetName(), *OutReason);
	}
	return OutReason.IsEmpty();
}

FString ACSHouseActor::GetDecorUndrawableReason() const
{
	FlushPendingReevaluate();
	FString OutReason;
	// 逐环检查渲染那一侧 —— readback 断言对这些一个字都说不了（见头文件里那段教训）。
	if (!bDecorEnabled) { OutReason = TEXT("bDecorEnabled 关着"); return OutReason; }
	if (DecorComponents.Num() == 0) { OutReason = TEXT("一个 palette 都没有（三家的网格都空着）"); return OutReason; }
	if (!bDecorBaseMeshReady) { OutReason = TEXT("基础网格快照没建起来（读不到 LOD0 顶点？）"); return OutReason; }
	if (CurrentDecorAnchorCount <= 0) { OutReason = TEXT("一个锚点都没生产出来"); return OutReason; }
	if (CurrentDecorInstanceCount <= 0) { OutReason = TEXT("锚点全被填充概率/间距球筛掉了"); return OutReason; }

	for (int32 Index = 0; Index < DecorComponents.Num(); ++Index)
	{
		const UCSGpuInstancedMeshComponent* Component = DecorComponents[Index];
		if (!IsValid(Component)) { OutReason = FString::Printf(TEXT("palette %d：没有渲染组件"), Index); return OutReason; }
		if (!Component->IsRegistered()) { OutReason = FString::Printf(TEXT("palette %d：渲染组件没注册"), Index); return OutReason; }
		if (!Component->IsVisible()) { OutReason = FString::Printf(TEXT("palette %d：渲染组件不可见"), Index); return OutReason; }
		if (!Component->HasInstanceSourceGPU()) { OutReason = FString::Printf(TEXT("palette %d：实例源没交接"), Index); return OutReason; }
		if (Component->GetBaseMeshSnapshot().Positions.Num() < 3)
		{
			OutReason = FString::Printf(TEXT("palette %d：基础网格快照是空的"), Index);
			return OutReason;
		}
		if (!Component->GetGpuMesh()) { OutReason = FString::Printf(TEXT("palette %d：GPU 网格没分配"), Index); return OutReason; }

		// **这一条就是石阶那个坑**：材质为空时组件仍然会画，只是退回引擎默认表面材质 —— 画面上是一片灰，
		// 而所有 readback 断言照绿。⚠️ 比石阶那条**多一环**：没勾 `bUsedWithInstancedStaticMeshes` 的材质在
		// 实例路径上会被引擎**静默替换**成默认材质，症状与"没绑材质"逐像素相同。
		// 2026-09-15 起摆件默认逐段画资产自己的材质（`DecorMaterial` 留空），所以要查的是**每一段解析出来的**那张，
		// 不再只是 `InstanceMaterial` —— 判据收在组件里（`GetMaterialUndrawableReason`），各 actor 共用一份。
		const FString MaterialReason = Component->GetMaterialUndrawableReason();
		if (!MaterialReason.IsEmpty())
		{
			OutReason = FString::Printf(TEXT("palette %d：%s"), Index, *MaterialReason);
			return OutReason;
		}
	}
	return FString();
}

// -----------------------------------------------------------------------------
// GPU 侧真值（诊断 / 验收专用，阻塞）—— 声明处那段注释是这几条为什么存在
// -----------------------------------------------------------------------------

namespace
{
/** 组件不在（还没建 / 被构造脚本销毁）时读到的就是 0：那一路确实一个实例都不画。 */
int32 CSHouse_ReadGpuInstanceCount(const UCSGpuInstancedMeshComponent* Component)
{
	return IsValid(Component) ? Component->DebugReadDrawnInstanceCountSync() : 0;
}
} // namespace

int32 ACSHouseActor::DebugReadFrameBrickCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(FrameComponent);
}

int32 ACSHouseActor::DebugReadVineBranchCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineBranchComponent);
}

int32 ACSHouseActor::DebugReadVineLeafCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineLeafComponent);
}

int32 ACSHouseActor::DebugReadVineFlowerCountGpuSync() const
{
	FlushPendingReevaluate();
	return CSHouse_ReadGpuInstanceCount(VineFlowerComponent);
}

int32 ACSHouseActor::DebugReadDecorInstanceCountGpuSync() const
{
	FlushPendingReevaluate();
	int32 Total = 0;
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents)
	{
		const int32 Count = CSHouse_ReadGpuInstanceCount(Component);
		// 一路读不到就整体作废：把它当 0 加进去会让总数看着"少了一点"而不是"坏了"，
		// 而这一族 bug 最典型的失效方式就是被当成小偏差放过去。
		if (Count < 0) return -1;
		Total += Count;
	}
	return Total;
}

FString ACSHouseActor::DebugGetGpuAssetMismatchSync() const
{
	FlushPendingReevaluate();
	return Super::DebugGetGpuAssetMismatchSync();
}

void ACSHouseActor::GetInstancedFamilies(TArray<FCSInstancedFamily>& OutFamilies) const
{
	// ⚠️ 不许在这里补票（FlushPendingReevaluate）：族表必须无副作用，补票放在诊断入口里（见上）。
	// 门框砖 + 接缝砖 + 角石 + 包边 + 砖层共用这一个组件。
	OutFamilies.Add({ FrameComponent, TEXT("门框砖"), TEXT("FrameBricks"), CurrentFrameBrickCount > 0 });
	OutFamilies.Add({ VineBranchComponent, TEXT("藤枝"), TEXT("VineBranch"), CurrentVineSegmentCount > 0 });
	OutFamilies.Add({ VineLeafComponent, TEXT("藤叶"), TEXT("VineLeaf"), CurrentVineSegmentCount > 0 });
	// 花单独判：一栋不长花的房子（`VineFlowerMesh` 留空）是**合法**的，拿枝数当门会把它误判。
	OutFamilies.Add({ VineFlowerComponent, TEXT("藤花"), TEXT("VineFlower"), CurrentVineFlowerCount > 0 });
	for (int32 Index = 0; Index < DecorComponents.Num(); ++Index) OutFamilies.Add({ DecorComponents[Index], FString::Printf(TEXT("摆件[%d]"), Index), FString::Printf(TEXT("Decor%d"), Index), CurrentDecorInstanceCount > 0 });
	OutFamilies.Add({ RoofTileComponent, TEXT("屋瓦"), TEXT("RoofTiles"), CurrentRoofTileCount > 0 });
	OutFamilies.Add({ FlatRoofParapetComponent, TEXT("露台垛口"), TEXT("FlatRoofParapet"), IsFlatRoof() && FlatRoofParapetComponent && FlatRoofParapetComponent->GetInstanceCount() > 0 });
	OutFamilies.Add({ PillarBrickComponent, TEXT("柱砖"), TEXT("PillarBricks"), bPillarUseBricks && PillarBrickMesh && CurrentPillarCount > 0 });
}

void ACSHouseActor::ReleaseInstancedBuffers()
{
	// 只放本 actor 分配的生产者那一份（排在已入队的命令之后，见 CSShaperSteps::ReleaseOnRenderThread）。
	// 组件手上的实例源、组件自己的常驻网格、槽网格的显存，各由组件在 OnComponentDestroyed 里放。
	CSShaperSteps::ReleaseOnRenderThread(FrameGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(PillarGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(VineGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(RoofTileGpuBuffers);
	CSShaperSteps::ReleaseOnRenderThread(DecorGpuBuffers);
	// 缓冲都没了 ⇒ 交接缓存跟着作废，下次 Ensure* 按新分配的那批重交。
	FrameHandover.Reset();
	PillarHandover.Reset();
	VineHandover.Reset();
	RoofTileHandover.Reset();
	DecorHandover.Reset();
	// ⚠️ 编辑器里删除可以撤销 —— 复活的是**同一批对象**：缓冲刚交回去，槽网格与实例组件的显存也已被
	// 各自的组件还掉，而各家的早退门只看输入哈希。复活后的第一次重求值因此必须全量重建（它会把各族的
	// desc 哈希清零）。
	bForceFullRebuild = true;
}

#if WITH_EDITOR

bool ACSHouseActor::DebugBakeFrameBricksSync(const FString& AssetPath, int32& OutTriangles,
	int32& OutVertexInstances, int32& OutUVChannels, int32& OutDistinctBakedRandoms,
	int32& OutGpuInstanceCount, bool& bOutRandomsMatchGpu)
{
	FlushPendingReevaluate();
	OutTriangles = 0;
	OutVertexInstances = 0;
	OutUVChannels = 0;
	OutDistinctBakedRandoms = 0;
	OutGpuInstanceCount = 0;
	bOutRandomsMatchGpu = false;
	if (!IsValid(FrameComponent)) return false;

	// 先把 GPU 那一侧的真值取到手，再烘 —— 顺序反过来的话，烘焙本身若不小心动了实例源，
	// 后读到的就是被自己改过的值，断言等于自证。
	TArray<float> GpuRandoms;
	if (!FrameComponent->DebugReadInstanceRandomsSync(GpuRandoms)) return false;
	OutGpuInstanceCount = GpuRandoms.Num();

	// 走组件自己的出口，不另拼一条：要证的正是"**那条**出口带不带得走通道"。
	// BakeSpace 取本 actor 的变换 —— 实例原点是组件空间的，出口内部先升世界再烘回这里的局部，
	// 资产摆在同一个变换上就能复现画面（同 RockShell / RoadMesh 的口径）。
	UStaticMesh* Baked = FrameComponent->SaveToStaticMesh(
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

	// 8 位量化后去重：顶点色最终就存成 FColor，比"浮点相等"更贴近资产里真正留下的东西，
	// 也让下面与 GPU 值的比对有一条明确的容差（±1/255），不必猜浮点误差。
	auto Quantize = [](float Value) { return int32(FMath::RoundToInt(FMath::Clamp(Value, 0.0f, 1.0f) * 255.0f)); };
	TSet<int32> BakedRandoms;
	for (const FVertexInstanceID InstanceID : Description->VertexInstances().GetElementIDs())
	{
		BakedRandoms.Add(Quantize(Colors[InstanceID].W));
	}
	OutDistinctBakedRandoms = BakedRandoms.Num();

	// 逐个对：GPU 上每一行的 `Origin.w` 都必须在烘焙件的 alpha 集合里找得到。
	// 反向不查 —— 同一个 8 位桶可能被两个实例共用（512 个实例挤 256 个桶），
	// 那不是缺陷；"GPU 有而烘焙件没有"才是通道被丢掉的证据。
	bOutRandomsMatchGpu = GpuRandoms.Num() > 0;
	for (const float Random : GpuRandoms)
	{
		// 负值 = 藏起来的砖（转角墩剔除的哨兵，组件级契约）：出口按契约不烘它，烘焙件里本来就不该有这一桶。
		if (Random < 0.0f) continue;
		const int32 Bucket = Quantize(Random);
		if (!BakedRandoms.Contains(Bucket) && !BakedRandoms.Contains(Bucket - 1) && !BakedRandoms.Contains(Bucket + 1))
		{
			bOutRandomsMatchGpu = false;
			break;
		}
	}
	return true;
}

int32 ACSHouseActor::SaveInstancedToStaticMeshes(const FString& BakeFolder, bool bSaveAssets)
{
	FlushPendingReevaluate();
	return Super::SaveInstancedToStaticMeshes(BakeFolder, bSaveAssets);
}

UStaticMesh* ACSHouseActor::DebugBakeVineBranchesSync(const FString& AssetPath)
{
	FlushPendingReevaluate();
	if (!IsValid(VineBranchComponent)) return nullptr;
	return VineBranchComponent->SaveToStaticMesh(
		GetActorTransform(), AssetPath, /*bReplaceExistingAsset*/ true, /*bSaveAsset*/ false);
}

void ACSHouseActor::DebugSetVineBranchInstancesHidden(bool bHideInstances)
{
	// SetHiddenInGame 在编辑器视口/离屏捕获里不起作用（那两条走的是编辑器可见性），
	// 所以用 SetVisibility —— 它两边都算数。
	if (IsValid(VineBranchComponent)) VineBranchComponent->SetVisibility(!bHideInstances, /*bPropagateToChildren*/ true);
}

#endif // WITH_EDITOR

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSHouseActor::BindHouseMaterials()
{
	BindTinyGladeMaterials({ WallMaterial, FlatRoofMaterial ? FlatRoofMaterial : WallMaterial });
	// 瓦与房体的屋顶槽共用 `RoofMaterial`：它们是同一样东西的两半（槽 1 现在没有三角，
	// 屋面全在瓦上）。⚠️ 实例路径要求母材质勾了 `bUsedWithInstancedStaticMeshes`，
	// 没勾会被引擎**静默换成默认材质** —— `GetRoofTileUndrawableReason` 专门查这一条。
	// 与下面门框砖同理：代理构造时抄走了材质，换 RoofMaterial 要自己脏一次渲染状态。
	if (IsValid(RoofTileComponent) && RoofTileComponent->InstanceMaterial != RoofMaterial)
	{
		RoofTileComponent->SetInstanceMaterial(RoofMaterial);
		RoofTileComponent->MarkRenderStateDirty();
	}
	if (PillarMesh) BindMeshSlotMaterials(PillarMeshComponent, PillarMesh, { CSHouse_PillarBoxMaterial(*this) });

	// 门框砖 / 柱砖 / 摆件走另一条组件（实例化），漏了它们的症状与 D14 开篇描述的一模一样：在细节面板里改
	// 材质静默无效，必须手点 RebuildHouse()。这里补上，重绑不重建的纪律才算完整。
	// 代理在构造时就把每段的材质抄走了，光写属性不重建代理是看不出变化的 —— `SetInstanceMaterial`
	// 变了才写、变了才脏一次渲染状态；传空 = 退回各自网格资产的材质。
	if (IsValid(FrameComponent)) FrameComponent->SetInstanceMaterial(FrameMaterial);
	if (IsValid(FlatRoofParapetComponent)) FlatRoofParapetComponent->SetInstanceMaterial(FrameMaterial);
	if (IsValid(PillarBrickComponent)) PillarBrickComponent->SetInstanceMaterial(PillarMaterial);
	for (const TObjectPtr<UCSGpuInstancedMeshComponent>& Component : DecorComponents)
	{
		if (IsValid(Component)) Component->SetInstanceMaterial(DecorMaterial);
	}
}

void ACSHouseActor::PostLoad()
{
	Super::PostLoad();
	// ⚠️ **模板（蓝图 CDO / 原型）不迁、口径字段也不写**。`WallSConvention` 是普通属性，关卡里的实例
	// 按**原型**做增量存盘，而蓝图 CDO 同样会走到这里（`UClass::PostLoadDefaultObject`）：
	//   · CDO 若先被置 1，磁盘上根本没有这个字段的旧实例就从原型继承到 1 ⇒ 跳过迁移；
	//   · 实例若先于 CDO 构造，迁完写回的 1 又与之后被置 1 的 CDO 相等 ⇒ 不落盘 ⇒ 下次加载再迁一遍
	//     （奇数边的窗每存读一轮就挪一个墙厚）。
	// 两种加载次序各错一边。模板恒为 0，实例的 1 就永远与原型不同、必定落盘，迁不迁只由实例自己的
	// 存盘数据决定。代价：斜接之前存的蓝图若在 CDO 里带着奇数边的默认 `Windows`，从它新拖出来的
	// 实例会继承未换算的值 —— 现有资产没有这种默认值（2026-09-14 核对 BP_TinyGladeHouse）。
	if (IsTemplate() || WallSConvention >= 1) return;

	// 直角对接 → 斜接（2026-09-13）。只迁**存下来的绝对弧长**：锚点自带口径字段、在
	// `CSHouse_AnchorS` 里就地换算，门的位置每轮从道路重算，都不需要在这里动。
	//
	// ⚠️ 不 `Modify()`：这里在加载序列里，不是用户编辑。迁完的值随下一次存盘落地；不存盘就
	// 下次加载再迁一遍，读的仍是磁盘上那份旧值 —— 幂等。
	const float T = WallThickness;
	for (FCSHouseWindow& Window : Windows)
	{
		// 旧口径奇数边从外角点往里 T 处量起 ⇒ 同一个物理点在斜接口径下远一个 T。
		// 旧存档只可能是矩形（边号 0..3），奇偶判据成立。
		if ((Window.EdgeIndex & 1) != 0) Window.CenterS += T;
	}
	// 环参数随奇数边变长而整体错位，旧记忆没有意义；清掉由下一轮重判（至多一次迟回决策）。
	DoorRunMemory.Empty();
	PierSpanIsPier.Empty();
	WallSConvention = 1;
}

void ACSHouseActor::PostActorCreated()
{
	Super::PostActorCreated();
	WallSConvention = 1;
}

void ACSHouseActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	if (IsTemplate() || !GetWorld()) return;

	// 根组件的 TransformUpdated：任何来源的 transform 变化（gizmo / 脚本 / 复制 / Undo）都从这里标脏
	// （D7 09-16），不靠 subsystem 的快扫，脚本也不需要显式 CommitEdit()。
	if (USceneComponent* Root = GetRootComponent(); Root && !RootTransformUpdatedHandle.IsValid())
	{
		RootTransformUpdatedHandle = Root->TransformUpdated.AddUObject(this, &ACSHouseActor::HandleRootTransformUpdated);
	}

	// 没有登记表（2026-09-16 删掉 `UCSHouseSubsystem`）：世界本身就是名单，见 `UCSHouseLibrary::GetHouses`。
	// 身份：无效（新生成 / T3D 复制跳过了它）就发新的；与同 world 里别的房子撞了（此前存盘的旧副本）也换 ——
	// 撞 GUID 的两栋永远不出缝。换的这栋取路径派生的确定性 GUID：不标脏包，下次加载同一栋仍得同一个值，
	// 砖的随机花样不会每次加载都变；存一次盘就落定。
	if (!HouseId.IsValid()) HouseId = FGuid::NewGuid();
	else if (IsHouseIdTakenByOther(HouseId)) HouseId = FGuid::NewDeterministicGuid(GetPathName());
	ReevaluateSite();
}

void ACSHouseActor::Destroyed()
{
	OnTerraceChanged.Broadcast(this);
	// **编辑器 world 里只有这一条会来**（那个 world 没有 begun play，`DestroyActor` 不发
	// `EndPlay`）。漏掉它的症状是"删掉房子，四个锥子还浮在原地" —— 而抓手的宿主弱引用
	// 此刻已经失效，它们既不认识任何房子又能被选中拖动，纯粹的噪声。
	// 接缝（D7 09-16）：删除时对每个旧对端发空 —— 对端下一帧清掉涉及我的记录、重砌不含这条缝的砖。
	DetachContacts();
	ExitResizeMode();
	// 编辑器里删房子只走这里（没有 EndPlay）：周围的灌木跟着消失；撤销删除时重求值会再推回来。
	if (IsValid(Ground)) Ground->RemoveBuildingFootprint(this);
	Super::Destroyed();
}

void ACSHouseActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 覆盖 PIE 结束与关卡卸载 —— 这两条 `Destroyed` 收不到。`ExitResizeMode` / `DetachContacts` 幂等，两处都调。
	DetachContacts();
	ExitResizeMode();
	if (USceneComponent* Root = GetRootComponent(); Root && RootTransformUpdatedHandle.IsValid()) Root->TransformUpdated.Remove(RootTransformUpdatedHandle);
	RootTransformUpdatedHandle.Reset();
	UnsubscribeGround();
	Super::EndPlay(EndPlayReason);
}

void ACSHouseActor::BeginDestroy()
{
	if (RootTransformUpdatedHandle.IsValid())
	{
		if (USceneComponent* Root = GetRootComponent()) Root->TransformUpdated.Remove(RootTransformUpdatedHandle);
		RootTransformUpdatedHandle.Reset();
	}
	UnsubscribeGround();
	Super::BeginDestroy();
}

#if WITH_EDITOR
void ACSHouseActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// 材质是纯外观量：只重绑，不重建（计划 D14）。走 ReevaluateSite 的话 BodyDescHash 不含
	// 任何材质 ⇒ 哈希不变 ⇒ 跳过重建 ⇒ 材质从未被重新绑定，画面零变化。
	const FName Name = PropertyChangedEvent.GetPropertyName();
	if (Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, WallMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, RoofMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, FlatRoofMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, PillarMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, FrameMaterial)
		|| Name == GET_MEMBER_NAME_CHECKED(ACSHouseActor, DecorMaterial))
	{
		BindHouseMaterials();
	}
}

void ACSHouseActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);

	// 接缝冻结期（D7 09-16）：拖动帧不算、不发、不重建，松手那一次才提交（CanCommitSeams）。
	NoteGizmoDrag(bFinished);

	// 连续 N 次增量 TransformMesh 会攒浮点误差：松手做一次全量重建对齐并清零
	// （ACSGroundActor::PostEditMove 已经是这个模式，照抄）。拖动中走摆位快路径。
	if (bFinished) bForceFullRebuild = true;
	ReevaluateSite();

	// 整栋被拖动时抓手靠 attach 跟着走，位置本来就是对的；松手补一次归位是为了兜住
	// **旋转与缩放** —— 那两样会改变墙外皮中心的世界位置，而 attach 只保相对量不变。
	if (bFinished) SnapResizeHandles();
}

void ACSHouseActor::PostEditUndo()
{
	Super::PostEditUndo();

	// 编辑器 world 里撤销本来就会经 UObject::PostEditUndo → PostEditChange →
	// AActor::PostEditChangeProperty 重跑构造脚本（→ 基类 OnConstruction → ReevaluateSite）；
	// 这里兜的是不重跑的那些情形（PIE world：ReregisterComponentsWhenModified 为假）。
	// 重求值幂等，编辑器里多来一次无害 —— 而不来的话，撤销一次移动之后房子会画在旧位置。
	ReevaluateSite();
}
#endif

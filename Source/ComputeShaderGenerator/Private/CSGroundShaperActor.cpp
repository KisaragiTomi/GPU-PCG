#include "CSGroundShaperActor.h"

#include "CSGroundActor.h"
#include "CSGroundShaperField.h"
#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "ImageCore.h"
#include "Serialization/CustomVersion.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSShaper_ 前缀。

/** 存盘版本：2026-09-29 形状改 mask、台高改 actor 高度（Radius / LiftHeight 迁移用）。 */
const FGuid CSShaper_VersionGuid(0x5A1C7E21, 0x4B8D4F03, 0x9E61C2B7, 0x3D0F8A94);
enum : int32
{
	CSShaper_InitialVersion = 0,
	CSShaper_MaskAndActorHeight = 1,
	CSShaper_LatestVersion = CSShaper_MaskAndActorHeight,
};
FCustomVersionRegistration CSShaper_VersionRegistration(CSShaper_VersionGuid, CSShaper_LatestVersion, TEXT("CSGroundShaperActor"));

uint32 CSShaper_HashFloat(uint32 Hash, float Value)
{
	return HashCombine(Hash, ::GetTypeHash(Value));
}
}

ACSGroundShaperActor::ACSGroundShaperActor()
{
	PrimaryActorTick.bCanEverTick = false;

#if WITH_EDITORONLY_DATA
	SpriteComponent = CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"));
	if (SpriteComponent)
	{
		// 地形图标，显示尺寸是原先 S_Actor 的 3 倍（用户指定 2026-09-18）。精灵大小按**贴图像素**算
		// （× 组件缩放 × 0.25，见引擎 FSpriteSceneProxy）：S_Terrain 256² 对 S_Actor 128²，换图本身就大了
		// 2 倍，所以缩放取 1.5 而不是 3。
		static ConstructorHelpers::FObjectFinderOptional<UTexture2D> SpriteTexture(TEXT("/Engine/EditorResources/S_Terrain.S_Terrain"));
		SpriteComponent->Sprite = SpriteTexture.Get();
		SpriteComponent->SetupAttachment(RootComponent);
		// 图标位于 actor（台顶或坑底）上方 1m。
		SpriteComponent->SetRelativeLocation(FVector(0, 0, 100.0));
		SpriteComponent->SetRelativeScale3D(FVector(1.5));
		SpriteComponent->bIsScreenSizeScaled = true;
		SpriteComponent->SetHiddenInGame(true);
	}
#endif
}

// -----------------------------------------------------------------------------
// Ground wiring
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::ResolveGroundAndRegister()
{
	if (!Ground && GetWorld())
	{
		for (TActorIterator<ACSGroundActor> It(GetWorld()); It; ++It) { Ground = *It; break; }
	}
	if (!Ground) return;

	// ⚠️ **不订阅 `OnGroundChanged`**：这条订阅当初只为"别人画路 -> 本座重摆石阶"存在，
	// 随旧路（`RebuildSteps`）一起删了。塑形物现在只写高度场、不读回地面，是单向的。
	Ground->RegisterShaper(this);   // 已登记时是零成本的（区域更新由 RebuildTerrain 自己发）
}

void ACSGroundShaperActor::UnregisterFromGround(bool bRefreshGround)
{
	if (!IsValid(Ground)) return;
	Ground->UnregisterShaper(this);
	if (bRefreshGround) Ground->RefreshHeightsInRegion(LastAppliedFootprint.bIsValid ? LastAppliedFootprint : GetFootprintRect2D());
}

// -----------------------------------------------------------------------------
// Shape
// -----------------------------------------------------------------------------

double ACSGroundShaperActor::ComputeTopHeight() const
{
	// 台顶 = actor 相对地面的高度（用户 2026-09-29："actor 在 600，地形最高就是 600"）。
	// 没有地面（纯 CPU 单测）时相对世界 0。
	// 可以为负（2026-09-30）：actor 在地面以下 = 坑，坑底就是 actor 的高度。
	const double GroundZ = Ground ? Ground->GetActorLocation().Z : 0.0;
	return GetActorLocation().Z - GroundZ;
}

float ACSGroundShaperActor::GetContourRadius() const
{
	return MaskSdfN >= 2 && MaskSdf.Num() == MaskSdfN * MaskSdfN ? MaskSdfContourRadius : 0.5f * MaskSize;
}

float ACSGroundShaperActor::SampleShapeHeight(const FVector2D& WorldXY) const
{
	// 走的是与 GPU 逐行对照的那一份公式（Public/CSGroundShaperField.h ↔ CSGroundShaperField.ush），
	// 而且喂进去的是**同一份打包头 + 同一批 SDF 浮点数** —— 早先这里各写各的 double 版本，噪声一进来
	// 就没法保证两侧同数了（症状：房子/石阶浮在坡面上方几厘米，不报任何错）。
	return CSGroundShaperField::EvalShaper(FVector2f(float(WorldXY.X), float(WorldXY.Y)), GetFieldHeader(), GetMaskSdf());
}

CSGroundShaperField::FShaperHeader ACSGroundShaperActor::GetFieldHeader() const
{
	const FVector Loc = GetActorLocation();
	const bool bHasSdf = MaskSdfN >= 2 && MaskSdf.Num() == MaskSdfN * MaskSdfN;
	const float Lift = FMath::Max(SecondaryLiftScale, 0.0f);

	CSGroundShaperField::FShaperHeader H;
	H.Profile = FVector4f(float(Loc.X), float(Loc.Y), GetContourRadius(), FMath::Max(FalloffDistance, UE_KINDA_SMALL_NUMBER));
	// 频率在这里算完再打包，而不是两侧各除一次：除法结果差 1 ulp 就够让 CPU/GPU 一致性断言报红。
	// 台高先除 (1 + 二次抬升)：剖面在台顶取 1 + 二次抬升 ⇒ 台顶恰好 = actor 高度。
	const float Frequency = 1.0f / FMath::Max(SkirtNoiseWavelength, 1.0f);
	H.Top = FVector4f(float(ComputeTopHeight() / (1.0 + double(Lift))), FMath::Max(SkirtNoiseAmount, 0.0f), Frequency, Lift);
	// 种子过 float 再回 uint，所以只留 20 bit：更大的整数在 float 里就不是精确值了，
	// CPU 与 GPU 会各自截断到不同的数。
	H.Noise = FVector4f(float(uint32(SkirtNoiseSeed) & 0xFFFFFu), FMath::Max(Smoothing, 0.0f), bHasSdf ? float(MaskSdfN) : 0.0f, 0.0f);
	const double Yaw = FMath::DegreesToRadians(GetActorRotation().Yaw);
	const float Cell = FMath::Max(MaskSdfCellSize, UE_KINDA_SMALL_NUMBER);
	H.Frame = FVector4f(float(FMath::Cos(Yaw)), float(FMath::Sin(Yaw)), 1.0f / Cell, Cell);
	// 中心隆起的尺子：零等值线内最深处离边多远（圆盘 = 半径）。
	H.Shape = FVector4f(FMath::Clamp(CenterBulge, 0.0f, 1.0f), bHasSdf ? MaskSdfDeepest : 0.5f * MaskSize, 0.0f, 0.0f);
	return H;
}

FBox2D ACSGroundShaperActor::GetFootprintRect2D() const
{
	const FVector Loc = GetActorLocation();
	const double Reach = double(GetContourRadius()) + FalloffDistance;
	return FBox2D(FVector2D(Loc.X - Reach, Loc.Y - Reach), FVector2D(Loc.X + Reach, Loc.Y + Reach));
}

// -----------------------------------------------------------------------------
// Mask → SDF
// -----------------------------------------------------------------------------

uint32 ACSGroundShaperActor::ComputeMaskSdfKey() const
{
	uint32 Key = 0x5DF0u;
#if WITH_EDITOR
	if (Mask)
	{
		Key = HashCombine(Key, GetTypeHash(Mask->GetPathName()));
		Key = HashCombine(Key, GetTypeHash(Mask->Source.GetId()));
	}
#endif
	Key = CSShaper_HashFloat(Key, MaskSize);
	Key = CSShaper_HashFloat(Key, FalloffDistance);
	Key = CSShaper_HashFloat(Key, Smoothing);
	Key = CSShaper_HashFloat(Key, SdfCellSize);
	return Key;
}

void ACSGroundShaperActor::EnsureMaskSdf()
{
#if WITH_EDITOR
	if (!Mask)
	{
		// 圆盘模式：不占存盘空间。
		if (MaskSdfN != 0 || MaskSdf.Num() > 0) { MaskSdf.Empty(); MaskSdfN = 0; MaskSdfKey = 0; }
		return;
	}
	const uint32 Key = ComputeMaskSdfKey();
	if (Key == MaskSdfKey && MaskSdfN >= 2 && MaskSdf.Num() == MaskSdfN * MaskSdfN) return;

	FImage Source;
	if (!Mask->Source.IsValid() || !Mask->Source.GetMipImage(Source, 0))
	{
		UE_LOG(LogTemp, Warning, TEXT("[CSGroundShaper] %s: 读不到 mask %s 的源数据，沿用上一次烘好的距离场。"), *GetName(), *Mask->GetName());
		return;
	}
	FImage Gray;
	Source.CopyTo(Gray, ERawImageFormat::R32F, EGammaSpace::Linear);
	const TArrayView64<float> Pixels = Gray.AsR32F();

	CSGroundShaperField::FMaskSdf Baked;
	CSGroundShaperField::BakeMaskSdf(TConstArrayView<float>(Pixels.GetData(), int32(Pixels.Num())), Gray.SizeX, Gray.SizeY,
		MaskSize, FalloffDistance, Smoothing, SdfCellSize, Baked);
	MaskSdf = MoveTemp(Baked.Values);
	MaskSdfN = Baked.N;
	MaskSdfCellSize = Baked.CellSize;
	MaskSdfContourRadius = Baked.ContourRadius;
	MaskSdfDeepest = Baked.DeepestInside;
	MaskSdfKey = Key;
#endif
}

// -----------------------------------------------------------------------------
// Rebuild
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::ReevaluateSite()
{
	RebuildTerrain();
}

void ACSGroundShaperActor::RebuildTerrain(bool bCommitted)
{
	if (IsTemplate() || !GetWorld()) return;   // CDO / 未进世界的模板不碰场景
	ResolveGroundAndRegister();
	EnsureMaskSdf();

	if (Ground)
	{
		// 区域更新只重算 union(旧足迹, 新足迹) —— 撤掉旧位置的隆起与压出新位置的隆起是同一趟。
		const FBox2D NewFootprint = GetFootprintRect2D();
		FBox2D Region = NewFootprint;
		if (LastAppliedFootprint.bIsValid) Region += LastAppliedFootprint;
		LastAppliedFootprint = NewFootprint;
		Ground->RefreshHeightsInRegion(Region, bCommitted);   // 高度真变了才广播（地面自己的石阶随之重扫）
	}
}

// -----------------------------------------------------------------------------
// AActor
// -----------------------------------------------------------------------------

void ACSGroundShaperActor::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);
	Ar.UsingCustomVersion(CSShaper_VersionGuid);
}

void ACSGroundShaperActor::PostLoad()
{
	Super::PostLoad();
	if (GetLinkerCustomVersion(CSShaper_VersionGuid) >= CSShaper_MaskAndActorHeight) return;

	// 老关卡迁移（2026-09-29 前的存盘）：圆盘半径 → 直径；「相对 actor 再抬 LiftHeight、台顶再照抬
	// 二次抬升」→ 把 actor 本身抬到旧台顶。剖面按新公式（台高 / (1 + 二次抬升) × ...）与旧的逐项相同，
	// 画面不变。模板（蓝图 CDO）只迁尺寸，不动位置。
	MaskSize = FMath::Max(2.0f * Radius, 1.0f);
	if (!IsTemplate() && RootComponent && LiftHeight != 0.0f)
	{
		const double Raise = double(LiftHeight) * (1.0 + double(FMath::Max(SecondaryLiftScale, 0.0f)));
		RootComponent->SetRelativeLocation_Direct(RootComponent->GetRelativeLocation() + FVector(0.0, 0.0, Raise));
	}
}

void ACSGroundShaperActor::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();
	// 加载/放置后主动登记一次：广播只覆盖"之后的变化"，赶不上地面自己那次重建。
	RebuildTerrain();
}

void ACSGroundShaperActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromGround(/*bRefreshGround=*/false);
	Super::EndPlay(EndPlayReason);
}

void ACSGroundShaperActor::Destroyed()
{
	// 删掉塑形物 = 地形塌回去：这条必须刷新地面，否则隆起会留在镜像里。
	UnregisterFromGround(/*bRefreshGround=*/true);
	Super::Destroyed();
}

#if WITH_EDITOR
void ACSGroundShaperActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	RebuildTerrain();
}

void ACSGroundShaperActor::PostEditMove(bool bFinished)
{
	Super::PostEditMove(bFinished);
	// v1 直推：拖动中每帧重导出，但只重算受影响的矩形 + 一个 GPU compute pass（见计划 D3/D9）。
	// 拖动帧的广播标成未提交：房子每帧落座，但接缝的跨房写入等松手（D7）。
	RebuildTerrain(/*bCommitted=*/bFinished);
}
#endif

#include "CSGroundDecor.h"

#include "Misc/Crc.h"   // RingKey：unity 构建会替本 TU 带进来，-SingleFile 才照得出漏写

namespace
{
// Unity/jumbo 构建共享 TU，file-local 一律 CSGroundDecor_ 前缀
//（与 CSHouseDecor.cpp 的 CSHouseDecor_、CSGroundActor.cpp 的 CSGround_ 都不同）。

/**
 * 本座在 P 点的高度贡献，以及**合成场**在同一点的值。
 *
 * 分开返回是因为判据只有一条：合成场明显高于本座 ⇒ 这一段地表是邻座的坡，不是本座的裙。
 * 合成律是"取 max"（计划 D9），所以合成场永远 ≥ 本座，两者相等就说明这一段确实归本座。
 */
void CSGroundDecor_EvalPair(const TArray<CSGroundDecor::FShaperRing>& Rings, int32 Self,
	const FVector2f& P, float& OutOwn, float& OutField)
{
	OutOwn = CSGroundShaperField::EvalShaper(P, Rings[Self].Header, Rings[Self].Sdf);
	OutField = OutOwn;
	for (int32 Index = 0; Index < Rings.Num(); ++Index)
	{
		if (Index == Self) continue;
		OutField = FMath::Max(OutField, CSGroundShaperField::EvalShaper(P, Rings[Index].Header, Rings[Index].Sdf));
	}
}

/**
 * mask 形状上的一个锚点：沿 `Dir` 从台心往外，找距离场 = `Target` 的**最外**一个交点
 * （凹形 mask 一条射线可能穿好几次，取最外那次 ⇒ 锚点总在裙边外沿那一侧）。
 * 找不到（这个方向整条都在里面 / 外面）返回 false。朝向 = 距离场梯度（等值带的外法向）。
 */
bool CSGroundDecor_FindBandPoint(const CSGroundDecor::FShaperRing& Ring, const FVector2f& Centre, const FVector2f& Dir,
	float Target, float MaxR, FVector2f& OutP, FVector2f& OutNormal)
{
	auto Sd = [&Ring](const FVector2f& P)
	{
		return CSGroundShaperField::SignedDistance(P, Ring.Header,
			[&Ring](uint32 Index) { return Ring.Sdf.IsValidIndex(int32(Index)) ? Ring.Sdf[int32(Index)] : 0.0f; });
	};
	const float Step = FMath::Max(Ring.Header.Frame.W, 5.0f);
	float Lo = -1.0f;
	float Prev = Sd(Centre) - Target;
	for (float R = Step; R <= MaxR + Step; R += Step)
	{
		const float Cur = Sd(Centre + Dir * R) - Target;
		if (Prev <= 0.0f && Cur > 0.0f) Lo = R - Step;   // 由内到外穿过一次；继续走，留最外那次
		Prev = Cur;
	}
	if (Lo < 0.0f) return false;
	float A = Lo, B = Lo + Step;
	for (int32 Iter = 0; Iter < 16; ++Iter)
	{
		const float M = 0.5f * (A + B);
		if (Sd(Centre + Dir * M) - Target > 0.0f) B = M; else A = M;
	}
	OutP = Centre + Dir * (0.5f * (A + B));
	const float E = 0.5f * Step;
	const FVector2f G(
		Sd(OutP + FVector2f(E, 0.0f)) - Sd(OutP - FVector2f(E, 0.0f)),
		Sd(OutP + FVector2f(0.0f, E)) - Sd(OutP - FVector2f(0.0f, E)));
	OutNormal = G.SizeSquared() > 1e-12f ? G.GetSafeNormal() : Dir;
	return true;
}

/**
 * 邻座压过本座多少 cm 才算"这一段不是本座的裙"。
 *
 * 不能取 0：两座等大等高时，中垂线上两边**精确相等**，浮点噪声会让整条线上的锚点随机地
 * 一半留一半丢，症状是接合处的摆件在每次重求值时闪烁（而且看着像随机撒点，不像 bug）。
 * 2 cm 相对台高（几百 cm）是零头，相对浮点误差是三个数量级的余量。
 */
constexpr float CSGroundDecor_BuriedMargin = 2.0f;
}

namespace CSGroundDecor
{
uint32 RingKey(const FString& ShaperName)
{
	// 名字的 CRC 而不是 `FName` 的比较下标：后者是**注册顺序**派生的，同一份关卡两次加载
	// 可以给出不同的值 —— 那正是"下标当身份"那条坑换了个马甲。
	return ShaperName.IsEmpty() ? 0u : FCrc::StrCrc32(*ShaperName);
}

float RingRadius(const FShaperRing& Ring, const CSHouseDecor::FParams& Params)
{
	const float Radius = FMath::Max(Ring.Header.Profile.Z, 0.0f);
	const float Falloff = FMath::Max(Ring.Header.Profile.W, 0.0f);
	return Radius + Falloff * FMath::Clamp(Params.SkirtBandT, 0.0f, 1.0f);
}

void BuildSkirtAnchors(const FSite& Site, const CSHouseDecor::FParams& Params,
	TArray<CSHouseDecor::FAnchor>& OutAnchors)
{
	OutAnchors.Reset();
	if (Site.Rings.IsEmpty()) return;

	// 遍历序钉死为键序（见头文件）：`Rings` 是世界扫描 + 登记攒出来的，顺序不可靠，
	// 而最小间距是"先放的挤掉后放的" ⇒ 顺序一抖，同一份世界两次规划就给出不同的摆位。
	TArray<FShaperRing> Ordered = Site.Rings;
	Ordered.Sort([](const FShaperRing& A, const FShaperRing& B) { return A.Key < B.Key; });

	const float Spacing = FMath::Max(Params.SkirtSpacing, 20.0f);

	for (int32 RingIndex = 0; RingIndex < Ordered.Num(); ++RingIndex)
	{
		const FShaperRing& Ring = Ordered[RingIndex];
		const float R = RingRadius(Ring, Params);
		if (R <= UE_KINDA_SMALL_NUMBER) continue;
		// 台高为 0 的塑形物压根不隆起，它没有裙 —— 摆一圈桶在平地上读起来像凭空掉的。
		if (CSGroundShaperField::PeakHeight(Ring.Header.Top) <= UE_KINDA_SMALL_NUMBER) continue;

		const int32 Count = FMath::Max(1, FMath::RoundToInt(UE_TWO_PI * R / Spacing));
		const FVector2f Centre(Ring.Header.Profile.X, Ring.Header.Profile.Y);
		const bool bMaskShape = Ring.Header.Noise.Z >= 2.0f && !Ring.Sdf.IsEmpty();
		const float BandTarget = FMath::Max(Ring.Header.Profile.W, 0.0f) * FMath::Clamp(Params.SkirtBandT, 0.0f, 1.0f);

		for (int32 Index = 0; Index < Count; ++Index)
		{
			// 半格偏移：整圈的第 0 个不落在 +X 轴上，否则一排塑形物的摆件会在同一条线上对齐。
			const double Angle = UE_TWO_PI * (double(Index) + 0.5) / double(Count);
			const FVector2f Dir(float(FMath::Cos(Angle)), float(FMath::Sin(Angle)));
			// 圆盘：解析圆，逐位同改 mask 之前。mask：沿这个方向找同一条等值带。
			FVector2f P = Centre + Dir * R;
			FVector2f Outward = Dir;
			if (bMaskShape && !CSGroundDecor_FindBandPoint(Ring, Centre, Dir, BandTarget, R, P, Outward)) continue;
			const FVector2D WorldXY(P.X, P.Y);

			// ① 这一段地表是不是本座的裙（合成场取 max ⇒ 邻座更高就不是）。
			float Own = 0.0f, Field = 0.0f;
			CSGroundDecor_EvalPair(Ordered, RingIndex, P, Own, Field);
			if (Field > Own + CSGroundDecor_BuriedMargin) continue;

			// ② 道路排除。TG 的对位物是 `PathRaster` 那条 mask 订阅；这里与石阶严格互补 ——
			// 路穿过裙边的那一段长的是石阶，摆件让开，不然一堆桶正堵在台阶上。
			if (Site.SampleRoadWeight && Site.SampleRoadWeight(WorldXY) > Params.RoadReject) continue;

			// ③ 落高。镜像是权威（与 `ComputeDoors` / 房子那四家同一条路）；
			// 采样器缺席时用解析场补，理由见头文件（退回平地会让单测的落高断言变成空判据）。
			const double GroundZ = Site.SampleGroundZ
				? double(Site.SampleGroundZ(WorldXY))
				: Site.BaseZ + double(Field);

			CSHouseDecor::FAnchor Anchor;
			Anchor.Family = CSHouseDecor::EFamily::Skirt;
			// 身份 = (本座的稳定键, 环上第几号)，夹到非负 30 bit —— 负的 AnchorId 在
			// `IdentityHash` 里是合法的，但读日志时像 bug，没必要留这个歧义。
			Anchor.AnchorId = int32(((Ring.Key * 0x9E3779B1u) ^ (uint32(Index) * 0x85EBCA77u)) & 0x3FFFFFFFu);
			// 埋深：斜坡上摆件的底面与地表只在一点相切，不埋就有一半悬空（见 `SkirtEmbed`）。
			Anchor.Location = FVector(WorldXY.X, WorldXY.Y, GroundZ - double(FMath::Max(Params.SkirtEmbed, 0.0f)));
			// 朝外 = 背离台心，与墙脚那一家的 `Strip.N` 同一个读法。
			Anchor.Facing = FVector(Outward.X, Outward.Y, 0.0).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
			// 立直，不跟着坡歪：歪着的桶读起来像穿模（屋顶那两家也是这么定的）。
			Anchor.Up = FVector::UpVector;
			OutAnchors.Add(Anchor);
		}
	}
}

int32 MaxRecordsBound(const FSite& Site, const CSHouseDecor::FParams& Params)
{
	const float Spacing = FMath::Max(Params.SkirtSpacing, 20.0f);
	int32 Bound = 0;
	for (const FShaperRing& Ring : Site.Rings)
	{
		// 每座按**整圈**算：道路与邻座只会让锚点变少，上界不许依赖它们 ——
		// 依赖的话画一笔路就可能越过一次容量台阶，当场付一次设备同步。
		Bound += FMath::CeilToInt(UE_TWO_PI * double(RingRadius(Ring, Params)) / double(Spacing)) + 1;
	}
	// 一座都没有时也留一格：容量是"只涨不缩"的，从 0 涨上去那一次同样是阻塞刷新，
	// 而"场景里第一次放下一座塑形物"恰恰是交互操作。
	return FMath::Max(Bound, 1);
}
}

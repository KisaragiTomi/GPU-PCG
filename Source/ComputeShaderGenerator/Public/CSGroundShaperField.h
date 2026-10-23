#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "CSGpuSharedLayout.ush"   // CS_GROUND_SHAPER_FLOAT4S（与 .ush 同一份）

/**
 * 地形塑形物高度场的 **CPU 孪生**，与 `Shaders/Private/CSGroundShaperField.ush` 逐行对照。
 *
 * 为什么要有这一份：GPU 侧决定地面网格与石阶长什么样，CPU 侧决定房子落座、拾取、道路查询
 * 读到的高度。两侧分叉不报任何错 —— 症状是石阶浮在坡面上方几厘米、房子陷进土台几厘米，
 * 只在裙边中段看得出来，而且两条路各自都"自洽"。所以这里的每一行都必须是 ush 的转写：
 *
 *   · 只用 uint32 环绕算术 + 加减乘除 + sqrt。这几样在 C++ 与 HLSL 里是同一套语义。
 *   · **不用 sin/cos/pow/exp**，也不用 HLSL 内置 `noise()`（CPU 侧根本没有对应物）；
 *     超越函数两边的实现不同，差值会大到断言看得见。
 *   · 全程 float，不是 double：GPU 只有 float，CPU 用 double 算完再截断不是同一个数。
 *
 * 单测 `PCGPlugins.ComputeShaderGenerator.GroundShaper.CpuGpuFieldParity` 把两侧在整张地面上
 * 逐顶点比一遍；改了 ush 而忘了改这里（或反过来）会立刻报红。
 */
namespace CSGroundShaperField
{
/**
 * 打包布局（GPU 侧 `GroundShaperParams`，与 ush 的 `CSGSF_Header` / `CSGSF_PoolFetch` 逐字对应）：
 *
 *   [0]                    前缀    = (塑形物数, SDF 池起点的 float4 下标, 0, 0)
 *   [1 + 5i + 0] Profile   = (中心 X, 中心 Y, 零等值线最远半径, FalloffDistance)
 *   [1 + 5i + 1] Top       = (有符号台高 / (1 + 二次抬升), 裙边噪声幅度(归一化), 噪声频率 = 1/波长, 二次抬升系数)；台高 < 0 = 坑
 *   [1 + 5i + 2] Noise     = (噪声种子, 合成平滑宽度 cm, SDF 边长 N（0 = 解析圆盘，半径 = Profile.z）, SDF 在池里的起点(float 下标))
 *   [1 + 5i + 3] Frame     = (cos yaw, sin yaw, 1/格距, 格距)
 *   [1 + 5i + 4] Shape     = (中心隆起比例 0..1, 零等值线内最深距离 cm, 0, 0)
 *   [池起点 ..]            各座 N×N 的 SDF（cm，内负外正），首尾相接，每个 float4 装 4 个 texel
 *
 * 下标宏在 CSGpuSharedLayout.ush。**别再用 `Num() / Float4sPerShaper` 数座数**（池子跟在后面），
 * 一律走 `NumShapers`。
 */
inline constexpr int32 PrefixFloat4s = CS_GROUND_SHAPER_PREFIX_FLOAT4S;
inline constexpr int32 Float4sPerShaper = CS_GROUND_SHAPER_FLOAT4S;

/** 一座塑形物的五个头 float4（布局见上）。`Noise.Z == 0` ⇒ 解析圆盘，不读 SDF；`Shape` 全 0 ⇒ 平顶。 */
struct FShaperHeader
{
	FVector4f Profile = FVector4f(0.0f, 0.0f, 1.0f, 1.0f);
	FVector4f Top = FVector4f::Zero();
	FVector4f Noise = FVector4f::Zero();
	FVector4f Frame = FVector4f(1.0f, 0.0f, 1.0f, 1.0f);
	FVector4f Shape = FVector4f::Zero();
};

/** fbm 倍频数。与 ush 的 CSGSF_NOISE_OCTAVES 同步。 */
inline constexpr uint32 NoiseOctaves = 3;

inline uint32 Hash(uint32 Cx, uint32 Cy, uint32 Seed)
{
	uint32 H = Cx * 0x9E3779B1u;
	H ^= Cy * 0x85EBCA77u;
	H ^= Seed * 0xC2B2AE3Du;
	H ^= H >> 15;
	H *= 0x2545F491u;
	H ^= H >> 13;
	return H;
}

/** [0,1)。低 24 bit / 2^24：float 尾数正好 24 bit，商在两侧都是精确值。 */
inline float Hash01(uint32 Cx, uint32 Cy, uint32 Seed)
{
	return float(Hash(Cx, Cy, Seed) & 0x00FFFFFFu) * (1.0f / 16777216.0f);
}

/** 整数格点 value noise，t²(3−2t) 插值，返回 [-1, 1)。 */
inline float ValueNoise(float Px, float Py, uint32 Seed)
{
	const float Fx = FMath::FloorToFloat(Px);
	const float Fy = FMath::FloorToFloat(Py);
	const float Tx = Px - Fx;
	const float Ty = Py - Fy;
	const float Ux = Tx * Tx * (3.0f - 2.0f * Tx);
	const float Uy = Ty * Ty * (3.0f - 2.0f * Ty);
	// floor 之后再转 int 才对负坐标成立；int → uint 两侧都是模 2^32 的位保持转换。
	const uint32 Cx = uint32(int32(Fx));
	const uint32 Cy = uint32(int32(Fy));

	const float H00 = Hash01(Cx, Cy, Seed);
	const float H10 = Hash01(Cx + 1u, Cy, Seed);
	const float H01 = Hash01(Cx, Cy + 1u, Seed);
	const float H11 = Hash01(Cx + 1u, Cy + 1u, Seed);

	// 展开写而不用 FMath::Lerp：求值顺序必须与 ush 逐字相同。
	const float A = H00 + (H10 - H00) * Ux;
	const float B = H01 + (H11 - H01) * Ux;
	return (A + (B - A) * Uy) * 2.0f - 1.0f;
}

/** 归一化 fbm。常数照 Tiny Glade 实测（gain 0.55 / lacunarity 1.9 / 每层旋转 36.87°）。 */
inline float Turbulence(float Px, float Py, uint32 Seed)
{
	float Sum = 0.0f;
	float Amp = 1.0f;
	float Norm = 0.0f;
	float Qx = Px;
	float Qy = Py;
	for (uint32 Octave = 0; Octave < NoiseOctaves; ++Octave)
	{
		Sum += ValueNoise(Qx, Qy, Seed + Octave * 101u) * Amp;
		Norm += Amp;
		Amp *= 0.55f;
		const float Rx = Qx * 0.8f - Qy * 0.6f;
		const float Ry = Qx * 0.6f + Qy * 0.8f;
		Qx = Rx * 1.9f;
		Qy = Ry * 1.9f;
	}
	return Sum / Norm;
}

/**
 * 有符号距离（ush `CSGSF_SignedDistance` 的转写）。`Fetch(i)` 返回本座 SDF 的第 i 个 texel ——
 * 模板化是为了让"读打包池"与"读 actor 自己那份数组"走**同一份**算术（两者是同一批 float）。
 */
template <typename FetchT>
float SignedDistance(const FVector2f& P, const FShaperHeader& H, FetchT&& Fetch)
{
	const float Dx = P.X - H.Profile.X;
	const float Dy = P.Y - H.Profile.Y;
	if (H.Noise.Z < 2.0f) return FMath::Sqrt(Dx * Dx + Dy * Dy) - H.Profile.Z;

	const float N = H.Noise.Z;
	const float Lx = Dx * H.Frame.X + Dy * H.Frame.Y;
	const float Ly = Dy * H.Frame.X - Dx * H.Frame.Y;
	const float Half = (N - 1.0f) * 0.5f;
	const float Gx = Lx * H.Frame.Z + Half;
	const float Gy = Ly * H.Frame.Z + Half;
	const float Cx = FMath::Clamp(Gx, 0.0f, N - 1.0f);
	const float Cy = FMath::Clamp(Gy, 0.0f, N - 1.0f);
	const float Ex = Gx - Cx;
	const float Ey = Gy - Cy;
	const float Outside = FMath::Sqrt(Ex * Ex + Ey * Ey) * H.Frame.W;

	const float Fx = FMath::Min(FMath::FloorToFloat(Cx), N - 2.0f);
	const float Fy = FMath::Min(FMath::FloorToFloat(Cy), N - 2.0f);
	const float Tx = Cx - Fx;
	const float Ty = Cy - Fy;
	const uint32 Nu = uint32(N);
	const uint32 Base = uint32(Fy) * Nu + uint32(Fx);   // 相对本座起点；池版在 Fetch 里加 Noise.W
	const float S00 = Fetch(Base);
	const float S10 = Fetch(Base + 1u);
	const float S01 = Fetch(Base + Nu);
	const float S11 = Fetch(Base + Nu + 1u);
	const float Lo = S00 + (S10 - S00) * Tx;
	const float Hi = S01 + (S11 - S01) * Tx;
	return Lo + (Hi - Lo) * Ty + Outside;
}

/**
 * 单座的剖面值 p'（0 .. 1 + 二次抬升）。ush 的 `GroundShaperProfileEx` 的转写 ——
 * 逐条注释在 ush 那一侧，这里只留必须就地知道的两点。
 */
template <typename FetchT>
float ProfileWith(const FVector2f& P, const FShaperHeader& H, FetchT&& Fetch, float RadiusBonus = 0.0f)
{
	const FVector4f& A = H.Profile;
	const FVector4f& B = H.Top;
	const FVector4f& C = H.Noise;
	const float Sd = SignedDistance(P, H, Fetch) - RadiusBonus;
	const float Skirt = FMath::Max(0.0f, Sd);
	if (Skirt >= A.W) return 0.0f;

	const float T = A.W > 1e-6f ? Skirt / A.W : 0.0f;
	const float W = 1.0f - T;
	float S = W * W * (3.0f - 2.0f * W);

	// 只减不加（abs 之后做减法），权重 (1−S) 让台顶不受扰、裙边最碎。噪声域是本座局部坐标。
	if (B.Y > 0.0f)
	{
		const float Qx = (P.X - A.X) * B.Z;
		const float Qy = (P.Y - A.Y) * B.Z;
		const uint32 Seed = uint32(FMath::Max(C.X, 0.0f));
		S = FMath::Clamp(S - FMath::Abs(Turbulence(Qx, Qy, Seed) * (1.0f - S)) * B.Y, 0.0f, 1.0f);
	}

	// S·sqrt(S) 而不是 pow(S, 1.5)：GPU 的 pow 走 exp2/log2，与 CPU 的 pow 不是同一个数。
	const float Lift = FMath::Max(B.W, 0.0f);
	float Profile = S + Lift * S * FMath::Sqrt(S);

	// 中心隆起：裙边外沿 → 零等值线内最深处整段一条 smoothstep 山丘，按比例与平台剖面混合。
	const float Bulge = FMath::Clamp(H.Shape.X, 0.0f, 1.0f);
	if (Bulge > 0.0f)
	{
		const float U = FMath::Clamp((A.W - Sd) / (A.W + FMath::Max(H.Shape.Y, 1e-3f)), 0.0f, 1.0f);
		const float Hill = U * U * (3.0f - 2.0f * U);
		Profile = (1.0f - Bulge) * Profile + Bulge * (1.0f + Lift) * Hill;
	}
	return Profile;
}

/** 单座的有符号高度贡献（坑为负）。ush `GroundShaperEvalOneEx`。 */
template <typename FetchT>
float EvalShaperWith(const FVector2f& P, const FShaperHeader& H, FetchT&& Fetch, float RadiusBonus = 0.0f)
{
	return H.Top.X * ProfileWith(P, H, Fetch, RadiusBonus);
}

/** 读 actor 自己那份 SDF 数组的版本（解析圆盘时 `Sdf` 可以为空）。 */
inline float EvalShaper(const FVector2f& P, const FShaperHeader& H, TConstArrayView<float> Sdf, float RadiusBonus = 0.0f)
{
	return EvalShaperWith(P, H, [Sdf](uint32 Index) { return Sdf.IsValidIndex(int32(Index)) ? Sdf[int32(Index)] : 0.0f; }, RadiusBonus);
}

/**
 * 该座的**有符号**峰值（台顶 / 坑底，含二次抬升；打包已归一化 ⇒ ≈ actor 相对地面的高度）。
 * 当包围盒 / MaxAbsHeight 的上界用时取绝对值。
 */
inline float PeakHeight(const FVector4f& B)
{
	return B.X * (1.0f + FMath::Max(0.0f, B.W));
}

/** ush `CSGSF_SmoothMax` 的转写（多项式 smooth-max 倒角，夹 min(a,b)、封顶到峰高）。 */
inline float SmoothMax(float A, float B, float K, float Cap)
{
	const float M = FMath::Max(A, B);
	const float Ke = FMath::Min(K, FMath::Min(A, B));
	const float Gap = FMath::Abs(A - B);
	if (Ke <= 0.0f || Gap >= Ke) return M;
	const float T = (Ke - Gap) / Ke;
	return FMath::Min(M + Ke * T * T * 0.25f, Cap);
}

// ---- 打包缓冲上的读写（与 GPU 读同一份数组） --------------------------------------------

/** 打包缓冲里有几座（读前缀；空数组 ⇒ 0）。 */
inline int32 NumShapers(TConstArrayView<FVector4f> Packed)
{
	return Packed.Num() > 0 ? FMath::Max(0, int32(Packed[0].X)) : 0;
}

inline FShaperHeader GetHeader(TConstArrayView<FVector4f> Packed, int32 Shaper)
{
	const int32 Base = PrefixFloat4s + Shaper * Float4sPerShaper;
	FShaperHeader H;
	H.Profile = Packed[Base + 0];
	H.Top = Packed[Base + 1];
	H.Noise = Packed[Base + 2];
	H.Frame = Packed[Base + 3];
	H.Shape = Packed[Base + 4];
	return H;
}

/** ush `CSGSF_PoolFetch` 的转写：池里第 FloatIndex 个 texel。 */
inline float FetchPool(TConstArrayView<FVector4f> Packed, uint32 FloatIndex)
{
	const int32 Slot = int32(Packed[0].Y) + int32(FloatIndex >> 2u);
	return Packed.IsValidIndex(Slot) ? Packed[Slot][int32(FloatIndex & 3u)] : 0.0f;
}

/** 打包缓冲里第 i 座的高度贡献。 */
inline float EvalShaperPacked(const FVector2f& P, TConstArrayView<FVector4f> Packed, int32 Shaper, float RadiusBonus = 0.0f)
{
	const FShaperHeader H = GetHeader(Packed, Shaper);
	const uint32 Offset = uint32(FMath::Max(H.Noise.W, 0.0f));
	return EvalShaperWith(P, H, [Packed, Offset](uint32 Index) { return FetchPool(Packed, Offset + Index); }, RadiusBonus);
}

/** 打包缓冲里第 i 座的剖面值（坑的权重 = 它 / (1 + 二次抬升)）。 */
inline float ProfilePacked(const FVector2f& P, TConstArrayView<FVector4f> Packed, int32 Shaper, float RadiusBonus = 0.0f)
{
	const FShaperHeader H = GetHeader(Packed, Shaper);
	const uint32 Offset = uint32(FMath::Max(H.Noise.W, 0.0f));
	return ProfileWith(P, H, [Packed, Offset](uint32 Index) { return FetchPool(Packed, Offset + Index); }, RadiusBonus);
}

/**
 * 整份打包参数上的合成场（ush `GroundShaperComposite` 的转写）：
 * ① 隆起逐座求值 + 平滑 max；② 坑逐座把高度按剖面权重拉向（绝对的）坑底，只往下挖。
 */
inline float EvalField(const FVector2f& P, TConstArrayView<FVector4f> Packed)
{
	float H = 0.0f;
	float Cap = 0.0f;
	float K = 0.0f;
	const int32 Count = NumShapers(Packed);
	for (int32 Shaper = 0; Shaper < Count; ++Shaper)
	{
		const int32 Base = PrefixFloat4s + Shaper * Float4sPerShaper;
		const float Peak = PeakHeight(Packed[Base + 1]);
		if (Peak <= 0.0f) continue;
		const float h = EvalShaperPacked(P, Packed, Shaper);
		if (h <= 0.0f) continue;
		Cap = FMath::Max(Cap, Peak);
		K = FMath::Max(K, FMath::Max(Packed[Base + 2].Y, 0.0f));
		H = SmoothMax(H, h, K, Cap);
	}
	for (int32 Shaper = 0; Shaper < Count; ++Shaper)
	{
		const int32 Base = PrefixFloat4s + Shaper * Float4sPerShaper;
		const float Peak = PeakHeight(Packed[Base + 1]);
		if (Peak >= 0.0f) continue;
		const float Weight = ProfilePacked(P, Packed, Shaper) / (1.0f + FMath::Max(Packed[Base + 1].W, 0.0f));
		if (Weight <= 0.0f) continue;
		H = FMath::Min(H, H + (Peak - H) * Weight);
	}
	return H;
}

/**
 * 把各座的头 + 各自的 SDF 打成一份 GPU / CPU 共用的缓冲（写前缀、改写每座的池起点 Noise.W）。
 * `Sdfs[i]` 为空 ⇒ 那一座按解析圆盘走（Noise.Z 置 0）。结果永不为空（至少有前缀）。
 */
COMPUTESHADERGENERATOR_API void PackField(TConstArrayView<FShaperHeader> Headers,
	TConstArrayView<TConstArrayView<float>> Sdfs, TArray<FVector4f>& OutPacked);

/** 一次 mask → SDF 烘焙的结果（`BakeMaskSdf` 的输出）。 */
struct FMaskSdf
{
	int32 N = 0;                  // 边长（0 = 没烘出来 ⇒ 调用方退回解析圆盘）
	float CellSize = 1.0f;        // cm
	float ContourRadius = 0.0f;   // 零等值线到中心的最远距离 cm（足迹 / 摆件圈的上界）
	float DeepestInside = 0.0f;   // 零等值线内最深处离轮廓多远 cm（**模糊之后**量；中心隆起的归一化尺子）
	TArray<float> Values;         // N×N，行主序（行 = 局部 +Y），cm，内负外正
};

/**
 * mask → 有符号距离场，**圆滑在这里做**（CPU、只在改 mask / 尺寸 / 平滑时跑一次）。
 *
 *   ① mask 按 `MaskSize` 铺在本座局部 XY（u 沿 +X、v 沿 +Y，贴图第 0 行在 −Y 端），
 *      双线性取值后按 0.5 二值化；
 *   ② 精确欧氏距离变换（Felzenszwalb 两趟），内外各一次拼成有符号距离；
 *   ③ 对距离场做可分离高斯（半径 = `Smoothing`，σ = 半径 / 3，与岩壳路模糊同一口径）。
 *      距离场在直边处是线性的，高斯对线性函数是恒等 ⇒ 直边原地不动；只有凸角（星形尖角）
 *      被削圆、凹角被填圆，连带把裙边上从凹角发出的折痕（距离场的梯度断线）一起抹平。
 *      这是 TG `_terrain_derivative` 里那趟 GaussBlur 的对位物，只是做在距离域而不是高度域，
 *      所以场仍然能在任意点求值（石阶 / 岩壳 / 地被都在任意 XY 上采它）。
 *
 * `MaskPixels` 为 W×H 的 [0,1] 灰度（行主序，第 0 行 = 贴图顶行）。网格覆盖 mask + Falloff + 平滑的余量。
 */
COMPUTESHADERGENERATOR_API void BakeMaskSdf(TConstArrayView<float> MaskPixels, int32 Width, int32 Height,
	float MaskSize, float FalloffDistance, float Smoothing, float TargetCellSize, FMaskSdf& Out);
}

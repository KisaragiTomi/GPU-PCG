// C++ side of the CS2 road port: Colossal.Mathematics value types, the generated shared
// layout (Shaders/Private/CSRoad/CSRoadTypes.ush) and the operators the decompiled code uses.
#pragma once

#include "CSRoadMath.h"

namespace CSRoad
{
struct Bounds1
{
	float min = 0.0f, max = 0.0f;
	constexpr Bounds1() = default;
	constexpr Bounds1(float InMin, float InMax) : min(InMin), max(InMax) {}
};

struct Bounds2
{
	float2 min, max;
	constexpr Bounds2() = default;
	constexpr Bounds2(float2 InMin, float2 InMax) : min(InMin), max(InMax) {}
};

struct Bounds3
{
	float3 min, max;
	constexpr Bounds3() = default;
	constexpr Bounds3(float3 InMin, float3 InMax) : min(InMin), max(InMax) {}
};

struct Bezier4x2
{
	float2 a, b, c, d;
	constexpr Bezier4x2() = default;
	constexpr Bezier4x2(float2 A, float2 B, float2 C, float2 D) : a(A), b(B), c(C), d(D) {}
};

struct Bezier4x3
{
	float3 a, b, c, d;
	constexpr Bezier4x3() = default;
	constexpr Bezier4x3(float3 A, float3 B, float3 C, float3 D) : a(A), b(B), c(C), d(D) {}
	Bezier4x2 xz() const { return Bezier4x2(a.xz(), b.xz(), c.xz(), d.xz()); }
	Bezier4x2 xy() const { return Bezier4x2(a.xy(), b.xy(), c.xy(), d.xy()); }
};

inline bool operator==(Bounds1 l, Bounds1 r) { return l.min == r.min && l.max == r.max; }
inline bool operator!=(Bounds1 l, Bounds1 r) { return !(l == r); }
inline Bounds1 operator-(Bounds1 r) { return Bounds1(-r.min, -r.max); }
inline Bounds1 operator|(Bounds1 l, Bounds1 r) { return Bounds1(math::min(l.min, r.min), math::max(l.max, r.max)); }
inline Bounds1 operator|(Bounds1 l, float r) { return Bounds1(math::min(l.min, r), math::max(l.max, r)); }
inline Bounds1 operator|(float l, Bounds1 r) { return Bounds1(math::min(l, r.min), math::max(l, r.max)); }
inline Bounds1 operator&(Bounds1 l, Bounds1 r) { return Bounds1(math::max(l.min, r.min), math::min(l.max, r.max)); }
inline Bounds1 operator+(float l, Bounds1 r) { return Bounds1(l + r.min, l + r.max); }
inline Bounds1 operator+(Bounds1 l, float r) { return Bounds1(l.min + r, l.max + r); }
inline Bounds1 operator-(Bounds1 l, float r) { return Bounds1(l.min - r, l.max - r); }
inline Bounds1& operator|=(Bounds1& l, Bounds1 r) { return l = l | r; }
inline Bounds1& operator|=(Bounds1& l, float r) { return l = l | r; }

inline Bounds3 operator|(Bounds3 l, Bounds3 r) { return Bounds3(math::min(l.min, r.min), math::max(l.max, r.max)); }
inline Bounds3 operator|(Bounds3 l, float3 r) { return Bounds3(math::min(l.min, r), math::max(l.max, r)); }
inline Bounds3 operator+(Bounds3 l, float3 r) { return Bounds3(l.min + r, l.max + r); }
inline Bounds3& operator|=(Bounds3& l, Bounds3 r) { return l = l | r; }
inline Bounds3& operator|=(Bounds3& l, float3 r) { return l = l | r; }

// Leaf overloads for the generated CSRoadFlatten (golden-data comparison order).
inline void CSRoadFlatten(float V, TArray<double>& Out) { Out.Add(V); }
inline void CSRoadFlatten(uint V, TArray<double>& Out) { Out.Add(V); }
inline void CSRoadFlatten(int32 V, TArray<double>& Out) { Out.Add(V); }
inline void CSRoadFlatten(float2 V, TArray<double>& Out) { Out.Add(V.x); Out.Add(V.y); }
inline void CSRoadFlatten(float3 V, TArray<double>& Out) { Out.Add(V.x); Out.Add(V.y); Out.Add(V.z); }
inline void CSRoadFlatten(float4 V, TArray<double>& Out) { Out.Add(V.x); Out.Add(V.y); Out.Add(V.z); Out.Add(V.w); }
inline void CSRoadFlatten(const Bounds1& V, TArray<double>& Out) { Out.Add(V.min); Out.Add(V.max); }
inline void CSRoadFlatten(const Bounds2& V, TArray<double>& Out) { CSRoadFlatten(V.min, Out); CSRoadFlatten(V.max, Out); }
inline void CSRoadFlatten(const Bounds3& V, TArray<double>& Out) { CSRoadFlatten(V.min, Out); CSRoadFlatten(V.max, Out); }
inline void CSRoadFlatten(const Bezier4x2& V, TArray<double>& Out) { CSRoadFlatten(V.a, Out); CSRoadFlatten(V.b, Out); CSRoadFlatten(V.c, Out); CSRoadFlatten(V.d, Out); }
inline void CSRoadFlatten(const Bezier4x3& V, TArray<double>& Out) { CSRoadFlatten(V.a, Out); CSRoadFlatten(V.b, Out); CSRoadFlatten(V.c, Out); CSRoadFlatten(V.d, Out); }
inline void CSRoadFlatten(const Float3x4& V, TArray<double>& Out) { CSRoadFlatten(V.c0, Out); CSRoadFlatten(V.c1, Out); CSRoadFlatten(V.c2, Out); CSRoadFlatten(V.c3, Out); }
inline void CSRoadFlatten(const Float4x4& V, TArray<double>& Out) { CSRoadFlatten(V.c0, Out); CSRoadFlatten(V.c1, Out); CSRoadFlatten(V.c2, Out); CSRoadFlatten(V.c3, Out); }
}

#include "CSRoad/CSRoadTypes.ush"

namespace CSRoad
{
inline CompositionFlags MakeFlags(uint General, uint Left, uint Right) { CompositionFlags f; f.m_General = General; f.m_Left = Left; f.m_Right = Right; return f; }
inline CompositionFlags operator|(CompositionFlags l, CompositionFlags r) { return MakeFlags(l.m_General | r.m_General, l.m_Left | r.m_Left, l.m_Right | r.m_Right); }
inline CompositionFlags operator&(CompositionFlags l, CompositionFlags r) { return MakeFlags(l.m_General & r.m_General, l.m_Left & r.m_Left, l.m_Right & r.m_Right); }
inline CompositionFlags operator~(CompositionFlags f) { return MakeFlags(~f.m_General, ~f.m_Left, ~f.m_Right); }
inline bool operator==(CompositionFlags l, CompositionFlags r) { return l.m_General == r.m_General && l.m_Left == r.m_Left && l.m_Right == r.m_Right; }
inline bool operator!=(CompositionFlags l, CompositionFlags r) { return !(l == r); }
inline CompositionFlags& operator|=(CompositionFlags& l, CompositionFlags r) { return l = l | r; }
inline CompositionFlags& operator&=(CompositionFlags& l, CompositionFlags r) { return l = l & r; }
inline bool IsDefault(CompositionFlags f) { return f.m_General == 0 && f.m_Left == 0 && f.m_Right == 0; }
inline uint32 GetTypeHash(const CompositionFlags& f) { return HashCombine(HashCombine(::GetTypeHash(f.m_General), ::GetTypeHash(f.m_Left)), ::GetTypeHash(f.m_Right)); }

// CompositionFlags.nodeMask / optionMask (CompositionFlags.cs).
inline CompositionFlags CompositionFlagsNodeMask()
{
	const uint General = CompositionFlags_General_Node | CompositionFlags_General_DeadEnd | CompositionFlags_General_Intersection | CompositionFlags_General_Roundabout | CompositionFlags_General_LevelCrossing | CompositionFlags_General_MedianBreak | CompositionFlags_General_TrafficLights | CompositionFlags_General_RemoveTrafficLights | CompositionFlags_General_AllWayStop | CompositionFlags_General_FixedNodeSize | CompositionFlags_General_StraightNodeEnd;
	const uint Side = CompositionFlags_Side_LowTransition | CompositionFlags_Side_HighTransition;
	return MakeFlags(General, Side, Side);
}

inline CompositionFlags CompositionFlagsOptionMask()
{
	const uint General = CompositionFlags_General_WideMedian | CompositionFlags_General_PrimaryMiddleBeautification | CompositionFlags_General_SecondaryMiddleBeautification;
	const uint Side = CompositionFlags_Side_PrimaryBeautification | CompositionFlags_Side_SecondaryBeautification | CompositionFlags_Side_WideSidewalk;
	return MakeFlags(General, Side, Side);
}

namespace MathUtils
{
inline float Center(Bounds1 b) { return math::lerp(b.min, b.max, 0.5f); }
inline float3 Center(Bounds3 b) { return math::lerp(b.min, b.max, 0.5f); }
inline float3 Size(Bounds3 b) { return b.max - b.min; }

inline float3 Position(const Bezier4x3& curve, float t)
{
	const float3 f(t, 1.0f - t, 3.0f * t);
	// (yzzx * yyxx) * yyyx, in the original evaluation order.
	const float4 w = float4(f.y, f.z, f.z, f.x) * float4(f.y, f.y, f.x, f.x) * float4(f.y, f.y, f.y, f.x);
	return curve.a * w.x + curve.b * w.y + curve.c * w.z + curve.d * w.w;
}

inline float3 Tangent(const Bezier4x3& curve, float t)
{
	const float4 f(t, 1.0f - t, 3.0f * t, -t);
	const float4 w = float4(f.y, f.y, f.x, f.x) * (float4(-1.0f, 1.0f, 2.0f, 0.0f) - float4(f.w, f.z, f.z, f.w)) * 3.0f;
	return curve.a * w.x + curve.b * w.y + curve.c * w.z + curve.d * w.w;
}

inline float Length(const Bezier4x3& curve)
{
	float num = 0.0f;
	float3 x = curve.a;
	for (int32 i = 1; i <= 16; i++)
	{
		const float3 p = Position(curve, static_cast<float>(i) / 16.0f);
		num += math::distance(x, p);
		x = p;
	}
	return num;
}

inline Bezier4x3 Invert(const Bezier4x3& curve) { return Bezier4x3(curve.d, curve.c, curve.b, curve.a); }
inline Bezier4x3 Lerp(const Bezier4x3& c1, const Bezier4x3& c2, float t) { return Bezier4x3(math::lerp(c1.a, c2.a, t), math::lerp(c1.b, c2.b, t), math::lerp(c1.c, c2.c, t), math::lerp(c1.d, c2.d, t)); }
inline float3 StartTangent(const Bezier4x3& curve) { return curve.b - curve.a; }
inline float2 Left(float2 forward) { return float2(-forward.y, forward.x); }
inline float2 Right(float2 forward) { return float2(forward.y, -forward.x); }

inline float2 EncodeOctahedral(float3 n)
{
	float x = n.x;
	float y = n.y;
	const float z = n.z;
	const float sum = math::abs(x) + math::abs(y) + math::abs(z);
	const float inv = sum == 0.0f ? 0.0f : 1.0f / sum;
	x *= inv;
	y *= inv;
	return float2(z >= 0.0f ? x : (1.0f - math::abs(y)) * (x >= 0.0f ? 1.0f : -1.0f), z >= 0.0f ? y : (1.0f - math::abs(x)) * (y >= 0.0f ? 1.0f : -1.0f));
}

inline int32 QuantizeSnorm(float v, int32 N)
{
	const float scale = static_cast<float>((1 << (N - 1)) - 1);
	const float half = v >= 0.0f ? 0.5f : -0.5f;
	v = math::clamp(v, -1.0f, 1.0f);
	return static_cast<int32>(v * scale + half);
}

inline uint NormalToOctahedral(float3 n)
{
	const float2 e = EncodeOctahedral(n);
	const int32 a = QuantizeSnorm(e.x, 16);
	const int32 b = QuantizeSnorm(e.y, 16);
	return static_cast<uint>((a & 0xFFFF) | ((b & 0xFFFF) << 16));
}

inline uint TangentToOctahedral(float4 t)
{
	const float2 e = EncodeOctahedral(t.xyz());
	const int32 a = QuantizeSnorm(e.x, 15);
	const int32 b = QuantizeSnorm(e.y, 15);
	return static_cast<uint>((a & 0x7FFF) | ((b & 0x7FFF) << 16) | (t.w < 0.0f ? INT32_MIN : 0));
}
}
}

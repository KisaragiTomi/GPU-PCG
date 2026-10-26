// Curve helpers of the lane graph and the decal setup, CPU side: the Colossal.Mathematics and
// Game originals they call (MathUtils.Cut / Intersect, NetUtils.FitCurve / IsTurn / StraightCurve,
// LaneSystem.ModifyCurveHeight, BatchDataHelpers.BuildCurveMatrix / BuildCurveParams), kept in the
// evaluation order of the decompiled C#.
#pragma once

#include "CSRoadCore.h"

namespace CSRoad
{
namespace math
{
// Unity.Mathematics evaluates the transcendentals in double (System.Math) and rounds to float;
// the float overloads in CSRoadMath.h use the float C runtime functions.
inline float tan_d(float v) { return static_cast<float>(std::tan(static_cast<double>(v))); }
inline float acos_d(float v) { return static_cast<float>(std::acos(static_cast<double>(v))); }
inline float2 clamp(float2 v, float a, float b) { return clamp(v, float2(a), float2(b)); }
inline float4 lerp(float4 a, float4 b, float4 t) { return a + t * (b - a); }
inline float4 saturate(float4 v) { return float4(saturate(v.x), saturate(v.y), saturate(v.z), saturate(v.w)); }
}

struct Line2
{
	float2 a, b;
};

struct Line3
{
	float3 a, b;
	Line2 xz() const { return Line2{ a.xz(), b.xz() }; }
};

struct Line3Segment
{
	float3 a, b;
};

namespace MathUtils
{
inline float3 EndTangent(const Bezier4x3& curve) { return curve.d - curve.c; }

inline Bezier4x3 Cut(const Bezier4x3& curve, float2 t)
{
	const float4 f(t.x, t.y, 1.0f - t.x, 1.0f - t.y);
	const float4 zzzw(f.z, f.z, f.z, f.w), zzww(f.z, f.z, f.w, f.w), zwww(f.z, f.w, f.w, f.w);
	const float4 xxxy(f.x, f.x, f.x, f.y), xxyy(f.x, f.x, f.y, f.y), xyyy(f.x, f.y, f.y, f.y);
	const float4 w0 = zzzw * zzww * zwww;
	const float4 w1 = xxxy * zzww * zwww + zzzw * xxyy * zwww + zzzw * zzww * xyyy;
	const float4 w2 = xxxy * xxyy * zwww + zzzw * xxyy * xyyy + xxxy * zzww * xyyy;
	const float4 w3 = xxxy * xxyy * xyyy;
	Bezier4x3 r;
	r.a = w0.x * curve.a + w1.x * curve.b + w2.x * curve.c + w3.x * curve.d;
	r.b = w0.y * curve.a + w1.y * curve.b + w2.y * curve.c + w3.y * curve.d;
	r.c = w0.z * curve.a + w1.z * curve.b + w2.z * curve.c + w3.z * curve.d;
	r.d = w0.w * curve.a + w1.w * curve.b + w2.w * curve.c + w3.w * curve.d;
	return r;
}

inline Bezier4x3 Cut(const Bezier4x3& curve, Bounds1 bounds) { return Cut(curve, float2(bounds.min, bounds.max)); }

inline Line3Segment Cut(const Line3& line, float2 t) { return Line3Segment{ math::lerp(line.a, line.b, t.x), math::lerp(line.a, line.b, t.y) }; }

inline float3 Tangent(const Line3Segment& line) { return line.b - line.a; }

inline bool Intersect(const Line2& line1, const Line2& line2, float2& t)
{
	const float2 f = line1.b - line1.a;
	const float2 xy = line2.b - line2.a;
	const float2 f2 = line1.a - line2.a;
	const float2 f3 = f * float2(xy.y, xy.x);
	const float4 f4 = float4(f2.y, f2.x, f2.y, f2.x) * float4(xy.x, xy.y, f.x, f.y);
	const float num = f3.x - f3.y;
	t = (float2(f4.x, f4.z) - float2(f4.y, f4.w)) / num;
	return num != 0.0f;
}
}

namespace NetUtils
{
inline Bezier4x3 StraightCurve(float3 startPos, float3 endPos)
{
	return Bezier4x3(startPos, math::lerp(startPos, endPos, 1.0f / 3.0f), math::lerp(startPos, endPos, 2.0f / 3.0f), endPos);
}

inline Bezier4x3 FitCurve(const Line3Segment& startLine, const Line3Segment& endLine)
{
	float3 f = MathUtils::Tangent(startLine);
	float3 f2 = MathUtils::Tangent(endLine);
	float num = math::length(f.xz());
	float num2 = math::length(f2.xz());
	if (num != 0.0f) f /= num;
	else
	{
		const float2 xz = endLine.b.xz() - startLine.a.xz();
		f.x = xz.x;
		f.z = xz.y;
		num = math::length(f.xz());
		if (num != 0.0f) f /= num;
	}
	if (num2 != 0.0f) f2 /= num2;
	else
	{
		const float2 xz = startLine.b.xz() - endLine.a.xz();
		f2.x = xz.x;
		f2.z = xz.y;
		num2 = math::length(f2.xz());
		if (num2 != 0.0f) f2 /= num2;
	}
	f.y = math::clamp(f.y, -1.0f, 1.0f);
	f2.y = math::clamp(f2.y, -1.0f, 1.0f);
	const float num3 = math::acos_d(math::saturate(0.0f - math::dot(f.xz(), f2.xz())));
	const float num4 = math::tan_d(num3 / 2.0f);
	float num5 = (num + num2) * (1.0f / 6.0f);
	num5 = !(num4 >= 0.0001f) ? num5 * 2.0f : num5 * (4.0f * math::tan_d(num3 / 4.0f) / num4);
	return Bezier4x3(startLine.a, startLine.a + f * math::min(num, num5), endLine.a + f2 * math::min(num2, num5), endLine.a);
}

inline Bezier4x3 FitCurve(float3 startPos, float3 startTangent, float3 endTangent, float3 endPos)
{
	const float num = math::distance(startPos.xz(), endPos.xz());
	const Line3 line{ startPos, startPos + startTangent };
	const Line3 line2{ endPos, endPos - endTangent };
	float2 start;
	start = !MathUtils::Intersect(line.xz(), line2.xz(), start) ? float2(num * 0.75f) : math::clamp(start, num * 0.01f, num);
	const float num2 = math::dot(startTangent.xz(), endTangent.xz());
	if (num2 > 0.0f) start = math::lerp(start, float2(num / math::sqrt(2.0f * num2 + 2.0f)), math::min(1.0f, num2 * num2));
	else if (num2 < 0.0f) start = math::lerp(start, float2(num * 1.2071068f), math::min(1.0f, num2 * num2));
	return FitCurve(MathUtils::Cut(line, float2(0.0f, start.x)), MathUtils::Cut(line2, float2(0.0f, start.y)));
}

inline bool IsTurn(float2 startPosition, float2 startDirection, float2 endPosition, float2 endDirection, bool& right, bool& gentle, bool& uturn)
{
	const float2 x = MathUtils::Right(startDirection);
	float4 start(0.0f);
	start.y = math::dot(startDirection, endDirection);
	start.w = math::dot(x, endDirection);
	const float num = math::distance(startPosition, endPosition);
	if (num > 0.1f)
	{
		const float2 y = (startPosition - endPosition) / num;
		start.x = math::dot(startDirection, y);
		start.z = math::dot(x, y);
	}
	else
	{
		start.x = -1.0f;
		start.z = 0.0f;
	}
	const float2 product = start.z * start.w * float2(-2.0f, -4.0f);
	start = math::lerp(start, float4(-1.0f, -1.0f, start.w, start.z), math::saturate(float4(product.x, product.y, start.x, start.y)));
	start = math::select(start, float4(start.y, start.x, start.w, start.z), start.y > start.x);
	right = start.z < 0.0f;
	gentle = (start.x > -0.9335804f) & (start.x <= -0.4848096f);
	uturn = start.x > 0.7547096f;
	return start.x > -0.9335804f;
}
}

namespace LaneSystem
{
inline void ModifyCurveHeight(Bezier4x3& curve, float startBaseHeight, float endBaseHeight, const NetCompositionData& startCompositionData, const NetCompositionData& endCompositionData)
{
	const float num = startBaseHeight + startCompositionData.m_SurfaceHeight.min;
	const float num2 = endBaseHeight + endCompositionData.m_SurfaceHeight.min;
	const float num3 = math::max(curve.a.y, curve.d.y);
	const float num4 = math::min(curve.a.y, curve.d.y);
	const uint Shaped = CompositionFlags_General_Roundabout | CompositionFlags_General_LevelCrossing;
	if ((startCompositionData.m_Flags.m_General & Shaped) != 0) curve.b.y += (math::max(0.0f, num4 - curve.b.y) - math::max(0.0f, curve.b.y - num3)) * (2.0f / 3.0f);
	if ((endCompositionData.m_Flags.m_General & Shaped) != 0) curve.c.y += (math::max(0.0f, num4 - curve.c.y) - math::max(0.0f, curve.c.y - num3)) * (2.0f / 3.0f);
	curve.b.y += math::max(0.0f, num - math::max(curve.a.y, curve.b.y)) * 1.3333334f;
	curve.c.y += math::max(0.0f, num2 - math::max(curve.d.y, curve.c.y)) * 1.3333334f;
}
}

namespace BatchDataHelpers
{
// BuildCurveMatrix with the default (zero) transform: the curve points and, in .w, the cubic that
// maps the curve parameter to the tiled texture distance.
inline Float4x4 BuildCurveMatrix(const Bezier4x3& bezier, float curveLength, float4 size, int32 tilingCount)
{
	float2 f(math::distance(bezier.a, bezier.b), math::distance(bezier.c, bezier.d));
	f /= curveLength;
	float4 f2(0.0f, f.x, 1.0f - f.y, 1.0f);
	float num = curveLength / math::max(1.0f, size.z);
	num = tilingCount != 0 ? math::round(num * static_cast<float>(tilingCount)) / static_cast<float>(tilingCount) : num;
	f2 *= num;
	Float4x4 r;
	r.c0 = float4(bezier.a, f2.x);
	r.c1 = float4(bezier.b, f2.y);
	r.c2 = float4(bezier.c, f2.z);
	r.c3 = float4(bezier.d, f2.w);
	return r;
}

// BuildCurveParams(size, NodeLane).zw: the share of each end among the lanes meeting there.
inline float2 SharedWeights(int32 SharedStartCount, int32 SharedEndCount)
{
	return math::select(1.0f / float2(1.0f + static_cast<float>(SharedStartCount), 1.0f + static_cast<float>(SharedEndCount)), float2(-1.0f),
		bool2(SharedStartCount == 255, SharedEndCount == 255));
}

// BuildCurveParams(size, EdgeLane).zw: whether anything connects at either end.
inline float2 ConnectedEnds(int32 ConnectedStartCount, int32 ConnectedEndCount)
{
	return math::select(float2(1.0f), float2(0.0f), bool2(ConnectedStartCount == 0, ConnectedEndCount == 0));
}
}
}

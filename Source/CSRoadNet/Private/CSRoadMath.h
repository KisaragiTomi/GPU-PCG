// Mirror of the Unity.Mathematics subset used by the Cities: Skylines II road code.
// The ported C++ keeps the decompiled C# shape line by line, so the vector types,
// swizzles and math:: functions follow Unity's semantics (math::select(false, true, c)).
#pragma once

#include "CoreMinimal.h"
#include <cmath>
#include <cfloat>

namespace CSRoad
{
using uint = uint32;

#define CSROAD_VEC2_OPS(T, S, B) \
	inline T operator+(T a, T b) { return T(a.x + b.x, a.y + b.y); } \
	inline T operator-(T a, T b) { return T(a.x - b.x, a.y - b.y); } \
	inline T operator*(T a, T b) { return T(a.x * b.x, a.y * b.y); } \
	inline T operator/(T a, T b) { return T(a.x / b.x, a.y / b.y); } \
	inline T operator+(T a, S b) { return T(a.x + b, a.y + b); } \
	inline T operator-(T a, S b) { return T(a.x - b, a.y - b); } \
	inline T operator*(T a, S b) { return T(a.x * b, a.y * b); } \
	inline T operator/(T a, S b) { return T(a.x / b, a.y / b); } \
	inline T operator+(S a, T b) { return T(a + b.x, a + b.y); } \
	inline T operator-(S a, T b) { return T(a - b.x, a - b.y); } \
	inline T operator*(S a, T b) { return T(a * b.x, a * b.y); } \
	inline T operator/(S a, T b) { return T(a / b.x, a / b.y); } \
	inline T operator-(T a) { return T(-a.x, -a.y); } \
	inline T& operator+=(T& a, T b) { return a = a + b; } \
	inline T& operator-=(T& a, T b) { return a = a - b; } \
	inline T& operator*=(T& a, T b) { return a = a * b; } \
	inline T& operator/=(T& a, T b) { return a = a / b; } \
	inline T& operator+=(T& a, S b) { return a = a + b; } \
	inline T& operator-=(T& a, S b) { return a = a - b; } \
	inline T& operator*=(T& a, S b) { return a = a * b; } \
	inline T& operator/=(T& a, S b) { return a = a / b; } \
	inline B operator<(T a, T b) { return B(a.x < b.x, a.y < b.y); } \
	inline B operator<=(T a, T b) { return B(a.x <= b.x, a.y <= b.y); } \
	inline B operator>(T a, T b) { return B(a.x > b.x, a.y > b.y); } \
	inline B operator>=(T a, T b) { return B(a.x >= b.x, a.y >= b.y); } \
	inline B operator==(T a, T b) { return B(a.x == b.x, a.y == b.y); } \
	inline B operator!=(T a, T b) { return B(a.x != b.x, a.y != b.y); }

#define CSROAD_VEC3_OPS(T, S, B) \
	inline T operator+(T a, T b) { return T(a.x + b.x, a.y + b.y, a.z + b.z); } \
	inline T operator-(T a, T b) { return T(a.x - b.x, a.y - b.y, a.z - b.z); } \
	inline T operator*(T a, T b) { return T(a.x * b.x, a.y * b.y, a.z * b.z); } \
	inline T operator/(T a, T b) { return T(a.x / b.x, a.y / b.y, a.z / b.z); } \
	inline T operator+(T a, S b) { return T(a.x + b, a.y + b, a.z + b); } \
	inline T operator-(T a, S b) { return T(a.x - b, a.y - b, a.z - b); } \
	inline T operator*(T a, S b) { return T(a.x * b, a.y * b, a.z * b); } \
	inline T operator/(T a, S b) { return T(a.x / b, a.y / b, a.z / b); } \
	inline T operator+(S a, T b) { return T(a + b.x, a + b.y, a + b.z); } \
	inline T operator-(S a, T b) { return T(a - b.x, a - b.y, a - b.z); } \
	inline T operator*(S a, T b) { return T(a * b.x, a * b.y, a * b.z); } \
	inline T operator/(S a, T b) { return T(a / b.x, a / b.y, a / b.z); } \
	inline T operator-(T a) { return T(-a.x, -a.y, -a.z); } \
	inline T& operator+=(T& a, T b) { return a = a + b; } \
	inline T& operator-=(T& a, T b) { return a = a - b; } \
	inline T& operator*=(T& a, T b) { return a = a * b; } \
	inline T& operator/=(T& a, T b) { return a = a / b; } \
	inline T& operator+=(T& a, S b) { return a = a + b; } \
	inline T& operator-=(T& a, S b) { return a = a - b; } \
	inline T& operator*=(T& a, S b) { return a = a * b; } \
	inline T& operator/=(T& a, S b) { return a = a / b; } \
	inline B operator<(T a, T b) { return B(a.x < b.x, a.y < b.y, a.z < b.z); } \
	inline B operator<=(T a, T b) { return B(a.x <= b.x, a.y <= b.y, a.z <= b.z); } \
	inline B operator>(T a, T b) { return B(a.x > b.x, a.y > b.y, a.z > b.z); } \
	inline B operator>=(T a, T b) { return B(a.x >= b.x, a.y >= b.y, a.z >= b.z); } \
	inline B operator==(T a, T b) { return B(a.x == b.x, a.y == b.y, a.z == b.z); } \
	inline B operator!=(T a, T b) { return B(a.x != b.x, a.y != b.y, a.z != b.z); }

#define CSROAD_VEC4_OPS(T, S, B) \
	inline T operator+(T a, T b) { return T(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w); } \
	inline T operator-(T a, T b) { return T(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w); } \
	inline T operator*(T a, T b) { return T(a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w); } \
	inline T operator/(T a, T b) { return T(a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w); } \
	inline T operator+(T a, S b) { return T(a.x + b, a.y + b, a.z + b, a.w + b); } \
	inline T operator-(T a, S b) { return T(a.x - b, a.y - b, a.z - b, a.w - b); } \
	inline T operator*(T a, S b) { return T(a.x * b, a.y * b, a.z * b, a.w * b); } \
	inline T operator/(T a, S b) { return T(a.x / b, a.y / b, a.z / b, a.w / b); } \
	inline T operator+(S a, T b) { return T(a + b.x, a + b.y, a + b.z, a + b.w); } \
	inline T operator-(S a, T b) { return T(a - b.x, a - b.y, a - b.z, a - b.w); } \
	inline T operator*(S a, T b) { return T(a * b.x, a * b.y, a * b.z, a * b.w); } \
	inline T operator/(S a, T b) { return T(a / b.x, a / b.y, a / b.z, a / b.w); } \
	inline T operator-(T a) { return T(-a.x, -a.y, -a.z, -a.w); } \
	inline T& operator+=(T& a, T b) { return a = a + b; } \
	inline T& operator-=(T& a, T b) { return a = a - b; } \
	inline T& operator*=(T& a, T b) { return a = a * b; } \
	inline T& operator/=(T& a, T b) { return a = a / b; } \
	inline T& operator+=(T& a, S b) { return a = a + b; } \
	inline T& operator-=(T& a, S b) { return a = a - b; } \
	inline T& operator*=(T& a, S b) { return a = a * b; } \
	inline T& operator/=(T& a, S b) { return a = a / b; } \
	inline B operator<(T a, T b) { return B(a.x < b.x, a.y < b.y, a.z < b.z, a.w < b.w); } \
	inline B operator<=(T a, T b) { return B(a.x <= b.x, a.y <= b.y, a.z <= b.z, a.w <= b.w); } \
	inline B operator>(T a, T b) { return B(a.x > b.x, a.y > b.y, a.z > b.z, a.w > b.w); } \
	inline B operator>=(T a, T b) { return B(a.x >= b.x, a.y >= b.y, a.z >= b.z, a.w >= b.w); } \
	inline B operator==(T a, T b) { return B(a.x == b.x, a.y == b.y, a.z == b.z, a.w == b.w); } \
	inline B operator!=(T a, T b) { return B(a.x != b.x, a.y != b.y, a.z != b.z, a.w != b.w); }

struct bool2
{
	bool x = false, y = false;
	constexpr bool2() = default;
	constexpr bool2(bool v) : x(v), y(v) {}
	constexpr bool2(bool InX, bool InY) : x(InX), y(InY) {}
	struct bool4 xyxy() const;
};

struct bool3
{
	bool x = false, y = false, z = false;
	constexpr bool3() = default;
	constexpr bool3(bool v) : x(v), y(v), z(v) {}
	constexpr bool3(bool InX, bool InY, bool InZ) : x(InX), y(InY), z(InZ) {}
};

struct bool4
{
	bool x = false, y = false, z = false, w = false;
	constexpr bool4() = default;
	constexpr bool4(bool v) : x(v), y(v), z(v), w(v) {}
	constexpr bool4(bool InX, bool InY, bool InZ, bool InW) : x(InX), y(InY), z(InZ), w(InW) {}
};

inline bool4 bool2::xyxy() const { return bool4(x, y, x, y); }

inline bool2 operator|(bool2 a, bool2 b) { return bool2(a.x || b.x, a.y || b.y); }
inline bool2 operator&(bool2 a, bool2 b) { return bool2(a.x && b.x, a.y && b.y); }
inline bool2 operator^(bool2 a, bool2 b) { return bool2(a.x != b.x, a.y != b.y); }
inline bool2 operator!(bool2 a) { return bool2(!a.x, !a.y); }
inline bool2 operator==(bool2 a, bool2 b) { return bool2(a.x == b.x, a.y == b.y); }
inline bool2 operator!=(bool2 a, bool2 b) { return bool2(a.x != b.x, a.y != b.y); }
inline bool2& operator|=(bool2& a, bool2 b) { return a = a | b; }
inline bool2& operator&=(bool2& a, bool2 b) { return a = a & b; }
inline bool3 operator|(bool3 a, bool3 b) { return bool3(a.x || b.x, a.y || b.y, a.z || b.z); }
inline bool3 operator&(bool3 a, bool3 b) { return bool3(a.x && b.x, a.y && b.y, a.z && b.z); }
inline bool3 operator!(bool3 a) { return bool3(!a.x, !a.y, !a.z); }
inline bool4 operator|(bool4 a, bool4 b) { return bool4(a.x || b.x, a.y || b.y, a.z || b.z, a.w || b.w); }
inline bool4 operator&(bool4 a, bool4 b) { return bool4(a.x && b.x, a.y && b.y, a.z && b.z, a.w && b.w); }
inline bool4 operator!(bool4 a) { return bool4(!a.x, !a.y, !a.z, !a.w); }

struct float2
{
	float x = 0.0f, y = 0.0f;
	constexpr float2() = default;
	constexpr float2(float v) : x(v), y(v) {}
	constexpr float2(float InX, float InY) : x(InX), y(InY) {}
	float2 yx() const { return float2(y, x); }
	float2 xx() const { return float2(x, x); }
	float2 yy() const { return float2(y, y); }
	float& operator[](int32 i) { return (&x)[i]; }
	float operator[](int32 i) const { return (&x)[i]; }
};

struct float3
{
	float x = 0.0f, y = 0.0f, z = 0.0f;
	constexpr float3() = default;
	constexpr float3(float v) : x(v), y(v), z(v) {}
	constexpr float3(float InX, float InY, float InZ) : x(InX), y(InY), z(InZ) {}
	constexpr float3(float2 InXY, float InZ) : x(InXY.x), y(InXY.y), z(InZ) {}
	float2 xy() const { return float2(x, y); }
	float2 xz() const { return float2(x, z); }
	float2 yz() const { return float2(y, z); }
	float2 zx() const { return float2(z, x); }
	float3 yzx() const { return float3(y, z, x); }
	float3 zxy() const { return float3(z, x, y); }
	void set_xz(float2 v) { x = v.x; z = v.y; }
	void set_xy(float2 v) { x = v.x; y = v.y; }
	float& operator[](int32 i) { return (&x)[i]; }
	float operator[](int32 i) const { return (&x)[i]; }
};

struct float4
{
	float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
	constexpr float4() = default;
	constexpr float4(float v) : x(v), y(v), z(v), w(v) {}
	constexpr float4(float InX, float InY, float InZ, float InW) : x(InX), y(InY), z(InZ), w(InW) {}
	constexpr float4(float3 InXYZ, float InW) : x(InXYZ.x), y(InXYZ.y), z(InXYZ.z), w(InW) {}
	constexpr float4(float2 InXY, float2 InZW) : x(InXY.x), y(InXY.y), z(InZW.x), w(InZW.y) {}
	constexpr float4(float2 InXY, float InZ, float InW) : x(InXY.x), y(InXY.y), z(InZ), w(InW) {}
	float2 xy() const { return float2(x, y); }
	float2 zw() const { return float2(z, w); }
	float2 xz() const { return float2(x, z); }
	float2 yw() const { return float2(y, w); }
	float2 yx() const { return float2(y, x); }
	float2 wz() const { return float2(w, z); }
	float2 zy() const { return float2(z, y); }
	float3 xyz() const { return float3(x, y, z); }
	float4 yxwz() const { return float4(y, x, w, z); }
	float4 zwxy() const { return float4(z, w, x, y); }
	float4 wzyx() const { return float4(w, z, y, x); }
	void set_xy(float2 v) { x = v.x; y = v.y; }
	void set_zw(float2 v) { z = v.x; w = v.y; }
	void set_yz(float2 v) { y = v.x; z = v.y; }
	float& operator[](int32 i) { return (&x)[i]; }
	float operator[](int32 i) const { return (&x)[i]; }
};

struct int2
{
	int32 x = 0, y = 0;
	constexpr int2() = default;
	constexpr int2(int32 v) : x(v), y(v) {}
	constexpr int2(int32 InX, int32 InY) : x(InX), y(InY) {}
};

struct int3
{
	int32 x = 0, y = 0, z = 0;
	constexpr int3() = default;
	constexpr int3(int32 v) : x(v), y(v), z(v) {}
	constexpr int3(int32 InX, int32 InY, int32 InZ) : x(InX), y(InY), z(InZ) {}
};

struct int4
{
	int32 x = 0, y = 0, z = 0, w = 0;
	constexpr int4() = default;
	constexpr int4(int32 v) : x(v), y(v), z(v), w(v) {}
	constexpr int4(int32 InX, int32 InY, int32 InZ, int32 InW) : x(InX), y(InY), z(InZ), w(InW) {}
	void set_xy(int2 v) { x = v.x; y = v.y; }
	void set_yw(int2 v) { y = v.x; w = v.y; }
	void set_xyw(int3 v) { x = v.x; y = v.y; w = v.z; }
};

CSROAD_VEC2_OPS(float2, float, bool2)
CSROAD_VEC3_OPS(float3, float, bool3)
CSROAD_VEC4_OPS(float4, float, bool4)
CSROAD_VEC2_OPS(int2, int32, bool2)
CSROAD_VEC3_OPS(int3, int32, bool3)
CSROAD_VEC4_OPS(int4, int32, bool4)

inline int3& operator++(int3& a) { ++a.x; ++a.y; ++a.z; return a; }

// Unity matrices are columns; keep the same members so c0..c3 accesses port unchanged.
struct Float3x4
{
	float3 c0, c1, c2, c3;
};

struct Float4x4
{
	float4 c0, c1, c2, c3;
};

namespace math
{
inline bool any(bool2 v) { return v.x || v.y; }
inline bool any(bool3 v) { return v.x || v.y || v.z; }
inline bool any(bool4 v) { return v.x || v.y || v.z || v.w; }
inline bool all(bool2 v) { return v.x && v.y; }
inline bool all(bool3 v) { return v.x && v.y && v.z; }
inline bool all(bool4 v) { return v.x && v.y && v.z && v.w; }

// Unity: select(falseValue, trueValue, test).
inline float select(float a, float b, bool c) { return c ? b : a; }
inline int32 select(int32 a, int32 b, bool c) { return c ? b : a; }
inline float2 select(float2 a, float2 b, bool c) { return c ? b : a; }
inline float3 select(float3 a, float3 b, bool c) { return c ? b : a; }
inline float4 select(float4 a, float4 b, bool c) { return c ? b : a; }
inline int2 select(int2 a, int2 b, bool c) { return c ? b : a; }
inline int3 select(int3 a, int3 b, bool c) { return c ? b : a; }
inline int4 select(int4 a, int4 b, bool c) { return c ? b : a; }
inline float2 select(float2 a, float2 b, bool2 c) { return float2(c.x ? b.x : a.x, c.y ? b.y : a.y); }
inline float3 select(float3 a, float3 b, bool3 c) { return float3(c.x ? b.x : a.x, c.y ? b.y : a.y, c.z ? b.z : a.z); }
inline float4 select(float4 a, float4 b, bool4 c) { return float4(c.x ? b.x : a.x, c.y ? b.y : a.y, c.z ? b.z : a.z, c.w ? b.w : a.w); }
inline int2 select(int2 a, int2 b, bool2 c) { return int2(c.x ? b.x : a.x, c.y ? b.y : a.y); }
inline int3 select(int3 a, int3 b, bool3 c) { return int3(c.x ? b.x : a.x, c.y ? b.y : a.y, c.z ? b.z : a.z); }

inline float min(float a, float b) { return a < b ? a : b; }
inline float max(float a, float b) { return a > b ? a : b; }
inline int32 min(int32 a, int32 b) { return a < b ? a : b; }
inline int32 max(int32 a, int32 b) { return a > b ? a : b; }
inline float2 min(float2 a, float2 b) { return float2(min(a.x, b.x), min(a.y, b.y)); }
inline float2 max(float2 a, float2 b) { return float2(max(a.x, b.x), max(a.y, b.y)); }
inline float3 min(float3 a, float3 b) { return float3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }
inline float3 max(float3 a, float3 b) { return float3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
inline float4 min(float4 a, float4 b) { return float4(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z), min(a.w, b.w)); }
inline float4 max(float4 a, float4 b) { return float4(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z), max(a.w, b.w)); }
inline int2 min(int2 a, int2 b) { return int2(min(a.x, b.x), min(a.y, b.y)); }
inline int2 max(int2 a, int2 b) { return int2(max(a.x, b.x), max(a.y, b.y)); }

inline float clamp(float v, float a, float b) { return max(a, min(b, v)); }
inline int32 clamp(int32 v, int32 a, int32 b) { return max(a, min(b, v)); }
inline float2 clamp(float2 v, float2 a, float2 b) { return max(a, min(b, v)); }
inline float3 clamp(float3 v, float3 a, float3 b) { return max(a, min(b, v)); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float2 saturate(float2 v) { return float2(saturate(v.x), saturate(v.y)); }
inline float3 saturate(float3 v) { return float3(saturate(v.x), saturate(v.y), saturate(v.z)); }

inline float abs(float v) { return std::fabs(v); }
inline int32 abs(int32 v) { return v < 0 ? -v : v; }
inline float2 abs(float2 v) { return float2(abs(v.x), abs(v.y)); }
inline float3 abs(float3 v) { return float3(abs(v.x), abs(v.y), abs(v.z)); }
inline float4 abs(float4 v) { return float4(abs(v.x), abs(v.y), abs(v.z), abs(v.w)); }

inline float cmin(float2 v) { return min(v.x, v.y); }
inline float cmin(float3 v) { return min(min(v.x, v.y), v.z); }
inline float cmin(float4 v) { return min(min(v.x, v.y), min(v.z, v.w)); }
inline float cmax(float2 v) { return max(v.x, v.y); }
inline float cmax(float3 v) { return max(max(v.x, v.y), v.z); }
inline float cmax(float4 v) { return max(max(v.x, v.y), max(v.z, v.w)); }
inline float csum(float2 v) { return v.x + v.y; }
inline float csum(float3 v) { return v.x + v.y + v.z; }
inline float csum(float4 v) { return v.x + v.y + v.z + v.w; }

inline bool isnan(float v) { return std::isnan(v); }
inline bool2 isnan(float2 v) { return bool2(isnan(v.x), isnan(v.y)); }
inline bool3 isnan(float3 v) { return bool3(isnan(v.x), isnan(v.y), isnan(v.z)); }
inline bool isfinite(float v) { return std::isfinite(v); }
inline bool3 isfinite(float3 v) { return bool3(isfinite(v.x), isfinite(v.y), isfinite(v.z)); }

inline float lerp(float a, float b, float t) { return a + t * (b - a); }
inline float2 lerp(float2 a, float2 b, float t) { return a + t * (b - a); }
inline float3 lerp(float3 a, float3 b, float t) { return a + t * (b - a); }
inline float4 lerp(float4 a, float4 b, float t) { return a + t * (b - a); }
inline float2 lerp(float2 a, float2 b, float2 t) { return a + t * (b - a); }
inline float3 lerp(float3 a, float3 b, float3 t) { return a + t * (b - a); }

inline float dot(float2 a, float2 b) { return a.x * b.x + a.y * b.y; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float dot(float4 a, float4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
inline float sqrt(float v) { return std::sqrt(v); }
inline float rsqrt(float v) { return 1.0f / std::sqrt(v); }
inline float lengthsq(float2 v) { return dot(v, v); }
inline float lengthsq(float3 v) { return dot(v, v); }
inline float length(float2 v) { return sqrt(dot(v, v)); }
inline float length(float3 v) { return sqrt(dot(v, v)); }
inline float distance(float2 a, float2 b) { return length(b - a); }
inline float distance(float3 a, float3 b) { return length(b - a); }
inline float distancesq(float2 a, float2 b) { return lengthsq(b - a); }
inline float distancesq(float3 a, float3 b) { return lengthsq(b - a); }
inline float3 cross(float3 a, float3 b) { return float3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
inline float2 normalize(float2 v) { return v * rsqrt(dot(v, v)); }
inline float3 normalize(float3 v) { return v * rsqrt(dot(v, v)); }
// Unity: returns defaultValue when the squared length is not above FLT_MIN.
inline float2 normalizesafe(float2 v, float2 d = float2(0.0f)) { float l = dot(v, v); return select(d, v * rsqrt(l), l > FLT_MIN); }
inline float3 normalizesafe(float3 v, float3 d = float3(0.0f)) { float l = dot(v, v); return select(d, v * rsqrt(l), l > FLT_MIN); }

inline float floor(float v) { return std::floor(v); }
inline float ceil(float v) { return std::ceil(v); }
inline float round(float v) { return std::nearbyint(v); }
inline float sin(float v) { return std::sin(v); }
inline float cos(float v) { return std::cos(v); }
inline float tan(float v) { return std::tan(v); }
inline float acos(float v) { return std::acos(v); }
inline float asin(float v) { return std::asin(v); }
inline float atan2(float y, float x) { return std::atan2(y, x); }
inline float radians(float v) { return v * 0.0174532925f; }
inline float sign(float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); }
inline float smoothstep(float a, float b, float x) { float t = saturate((x - a) / (b - a)); return t * t * (3.0f - 2.0f * t); }
}

// Unity Mathf.RoundToInt / MathF.Round round half to even.
inline int32 RoundToInt(float v) { return static_cast<int32>(std::nearbyint(v)); }
}

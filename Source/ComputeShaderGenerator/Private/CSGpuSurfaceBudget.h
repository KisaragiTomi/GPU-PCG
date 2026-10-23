#pragma once

#include <cstdint>

// Logical buffer allocations, rounded individually to a conservative D3D12 64 KiB unit.
// Excludes source render resources, Nanite streaming pages and driver/RDG pool caching.
namespace CSGpuSurfaceBudget
{
	constexpr uint64_t Alignment = 64ull * 1024ull;
	constexpr uint64_t MaxBytes = 128ull * 1024ull * 1024ull;

	constexpr uint64_t BufferBytes(uint64_t Bytes)
	{
		return (Bytes + Alignment - 1ull) / Alignment * Alignment;
	}

	inline uint64_t SurfaceBytes(uint32_t Triangles, uint32_t OtherTriangles = 0,
		uint32_t LandscapeTriangles = 0, uint64_t MaterialMapBytes = 0, uint64_t NaniteRetainedBytes = 0)
	{
		if (Triangles == 0) return 0;
		const uint64_t N = Triangles;
		uint64_t SortCount = 1;
		while (SortCount < N) SortCount *= 2;
		const uint64_t Nodes = 2 * N - 1;
		const uint64_t AtomicSlots = N > 1 ? 3 * (N - 1) : 1;
		// Position soup, six LBVH buffers, and the binding dummy. Sum without assuming aliasing.
		uint64_t Bytes = BufferBytes(48 * N) + BufferBytes(32 * Nodes) + BufferBytes(4 * Nodes)
			+ 2 * BufferBytes(4 * AtomicSlots) + 2 * BufferBytes(4 * SortCount) + BufferBytes(16);
		if (OtherTriangles > 0)
		{
			const uint64_t Other = OtherTriangles;
			// Existing ordinary-mesh collector: five float4 streams, UV, material/reference IDs,
			// counter, dummy reference point, and one material map per resolved request.
			Bytes += 5 * BufferBytes(48 * Other) + BufferBytes(24 * Other)
				+ 2 * BufferBytes(4 * Other) + 2 * BufferBytes(16) + MaterialMapBytes;
		}
		if (LandscapeTriangles > 0) Bytes += 2 * BufferBytes(48ull * LandscapeTriangles) + BufferBytes(4);
		return Bytes + NaniteRetainedBytes;
	}

	inline uint64_t NaniteRetainedBytes(uint64_t Vertices, uint64_t Triangles)
	{
		return Triangles > 0 ? BufferBytes(12 * Vertices) + BufferBytes(12 * Triangles) : 0;
	}

	// Mixed-batch capacity: ordinary geometry is known; the remaining slots are Nanite.
	// Decode admission also checks actual vertex capacity, so even unusual clusters cannot exceed bytes.
	inline uint32_t TriangleCapacity(uint64_t Budget, uint32_t OtherTriangles = 0,
		uint32_t LandscapeTriangles = 0, uint64_t MaterialMapBytes = 0, bool bReserveNanite = false)
	{
		uint32_t Low = OtherTriangles;
		uint32_t High = uint32_t(Budget / 48);
		if (Low > High || SurfaceBytes(Low, OtherTriangles, LandscapeTriangles, MaterialMapBytes) > Budget) return 0;
		while (Low < High)
		{
			const uint32_t Mid = Low + (High - Low + 1) / 2;
			const uint64_t Nanite = bReserveNanite ? uint64_t(Mid - OtherTriangles) : 0;
			const uint64_t Bytes = SurfaceBytes(Mid, OtherTriangles, LandscapeTriangles, MaterialMapBytes,
				NaniteRetainedBytes(3 * Nanite, Nanite));
			if (Bytes <= Budget) Low = Mid;
			else High = Mid - 1;
		}
		return Low;
	}
}

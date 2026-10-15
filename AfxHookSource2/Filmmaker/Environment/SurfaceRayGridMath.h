#pragma once

// Uniform-grid acceleration for map-collision raycasts.
//
// SurfaceRaycast in EnvironmentSurfaceMath.h walks every triangle in the map. That is fine for
// the handful of rays the environment editor fires on demand, and ruinous for per-frame callers,
// which fires one wall probe per physics body per frame per corpse. A prepared CS2 map runs to
// hundreds of thousands of triangles, so fifteen bodies at 64 fps was tens of millions of
// ray/triangle tests a second on the main thread -- the stutter that shows up as "the higher
// settings feel laggy".
//
// Every ray those callers fire is SHORT (18-24 units) against a map-sized mesh, which is the
// case a uniform grid answers best: bucket the triangles once per map, then a ray only ever
// touches the few cells it actually crosses.
//
// Engine-free and header-only so Automation/tests/surface-ray-grid-math-tests.cpp can check it
// against the brute-force result on the same geometry.

#include "EnvironmentSurfaceMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::Environment {

struct SurfaceRayGrid {
	Vec3 low{}, high{};
	// Reciprocal cell size, so traversal multiplies instead of dividing per step.
	float inverseCell[3] = {0, 0, 0};
	float cell[3] = {0, 0, 0};
	int dim[3] = {0, 0, 0};
	// cellStart has one extra entry, so cell i owns entries [cellStart[i], cellStart[i+1]).
	std::vector<uint32_t> cellStart;
	std::vector<uint32_t> triangles;

	bool Valid() const {
		return dim[0] > 0 && dim[1] > 0 && dim[2] > 0
			&& cellStart.size() == (size_t)dim[0] * dim[1] * dim[2] + 1;
	}
	size_t CellCount() const { return cellStart.empty() ? 0 : cellStart.size() - 1; }
	int Index(int x, int y, int z) const { return (z * dim[1] + y) * dim[0] + x; }
};

// Total cells and per-axis resolution are both capped: a long thin map must not turn one
// generous axis into a hundred million empty buckets.
inline constexpr size_t kSurfaceRayGridMaxCells = 1u << 21;
inline constexpr int kSurfaceRayGridMaxAxis = 256;

namespace detail {

inline int GridAxisSteps(float extent, float target) {
	if (!(extent > 0.f) || !(target > 0.f)) return 1;
	const int steps = (int)std::floor(extent / target);
	return std::min(kSurfaceRayGridMaxAxis, std::max(1, steps));
}

inline void TriangleCellRange(const Vec3 &a, const Vec3 &b, const Vec3 &c,
	const SurfaceRayGrid &grid, int lo[3], int hi[3]) {
	const float mins[3] = {std::min(a.x, std::min(b.x, c.x)), std::min(a.y, std::min(b.y, c.y)),
		std::min(a.z, std::min(b.z, c.z))};
	const float maxs[3] = {std::max(a.x, std::max(b.x, c.x)), std::max(a.y, std::max(b.y, c.y)),
		std::max(a.z, std::max(b.z, c.z))};
	const float origin[3] = {grid.low.x, grid.low.y, grid.low.z};
	for (int k = 0; k < 3; ++k) {
		lo[k] = (int)std::floor((mins[k] - origin[k]) * grid.inverseCell[k]);
		hi[k] = (int)std::floor((maxs[k] - origin[k]) * grid.inverseCell[k]);
		lo[k] = std::max(0, std::min(grid.dim[k] - 1, lo[k]));
		hi[k] = std::max(0, std::min(grid.dim[k] - 1, hi[k]));
	}
}

} // namespace detail

// Buckets each triangle into every cell its bounding box touches. Two passes (count, then fill)
// keep this to two flat allocations instead of a vector per cell.
inline bool BuildSurfaceRayGrid(const OcclusionMesh &world, SurfaceRayGrid &out,
	float targetTrianglesPerCell = 8.f) {
	out = SurfaceRayGrid{};
	const size_t triangleCount = world.indices.size() / 3;
	if (triangleCount == 0 || world.positions.empty()) return false;

	Vec3 low = world.positions[0], high = world.positions[0];
	for (const Vec3 &p : world.positions) {
		if (!Finite(p)) return false;
		low = {std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
		high = {std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
	}
	// A perfectly flat axis (a single ground plane) still needs a non-zero cell size.
	const float extent[3] = {std::max(1.f, high.x - low.x), std::max(1.f, high.y - low.y),
		std::max(1.f, high.z - low.z)};
	const double volume = (double)extent[0] * extent[1] * extent[2];
	const double wanted = std::max(1.0, (double)triangleCount / std::max(1.f, targetTrianglesPerCell));
	const float target = (float)std::cbrt(volume / wanted);

	for (int k = 0; k < 3; ++k) out.dim[k] = detail::GridAxisSteps(extent[k], target);
	while ((size_t)out.dim[0] * out.dim[1] * out.dim[2] > kSurfaceRayGridMaxCells) {
		const int widest = (out.dim[0] >= out.dim[1] && out.dim[0] >= out.dim[2]) ? 0
			: (out.dim[1] >= out.dim[2] ? 1 : 2);
		if (out.dim[widest] <= 1) break;
		out.dim[widest] = std::max(1, out.dim[widest] / 2);
	}
	out.low = low;
	out.high = high;
	for (int k = 0; k < 3; ++k) {
		out.cell[k] = extent[k] / out.dim[k];
		out.inverseCell[k] = 1.f / out.cell[k];
	}

	const size_t cells = (size_t)out.dim[0] * out.dim[1] * out.dim[2];
	std::vector<uint32_t> counts(cells + 1, 0);
	int lo[3], hi[3];
	for (size_t t = 0; t < triangleCount; ++t) {
		const uint32_t i0 = world.indices[t * 3], i1 = world.indices[t * 3 + 1],
			i2 = world.indices[t * 3 + 2];
		if (i0 >= world.positions.size() || i1 >= world.positions.size()
			|| i2 >= world.positions.size())
			return false;
		detail::TriangleCellRange(world.positions[i0], world.positions[i1], world.positions[i2],
			out, lo, hi);
		for (int z = lo[2]; z <= hi[2]; ++z)
			for (int y = lo[1]; y <= hi[1]; ++y)
				for (int x = lo[0]; x <= hi[0]; ++x) ++counts[out.Index(x, y, z)];
	}
	out.cellStart.assign(cells + 1, 0);
	uint32_t running = 0;
	for (size_t i = 0; i < cells; ++i) {
		out.cellStart[i] = running;
		running += counts[i];
	}
	out.cellStart[cells] = running;
	out.triangles.assign(running, 0);
	std::vector<uint32_t> cursor(out.cellStart.begin(), out.cellStart.end() - 1);
	for (size_t t = 0; t < triangleCount; ++t) {
		detail::TriangleCellRange(world.positions[world.indices[t * 3]],
			world.positions[world.indices[t * 3 + 1]],
			world.positions[world.indices[t * 3 + 2]], out, lo, hi);
		for (int z = lo[2]; z <= hi[2]; ++z)
			for (int y = lo[1]; y <= hi[1]; ++y)
				for (int x = lo[0]; x <= hi[0]; ++x)
					out.triangles[cursor[out.Index(x, y, z)]++] = (uint32_t)t;
	}
	return out.Valid();
}

// Same contract and same answer as SurfaceRaycast, walking only the cells the ray crosses.
// Falls back to the brute-force sweep when the grid is absent, so a caller never silently
// loses collision because a build failed.
inline bool SurfaceRaycastGrid(const OcclusionMesh &world, const SurfaceRayGrid &grid,
	Vec3 origin, Vec3 direction, float range, SurfaceHit &hit) {
	if (!grid.Valid()) return SurfaceRaycast(world, origin, direction, range, hit);
	direction = Normalize(direction);
	if (!Finite(origin) || !Finite(direction) || !(range > 0.f)) return false;

	const float from[3] = {origin.x, origin.y, origin.z};
	const float dir[3] = {direction.x, direction.y, direction.z};
	const float low[3] = {grid.low.x, grid.low.y, grid.low.z};
	const float high[3] = {grid.high.x, grid.high.y, grid.high.z};

	// Clip the segment to the grid box first; a ray that starts outside still has to enter.
	float enter = 0.f, leave = range;
	for (int k = 0; k < 3; ++k) {
		if (std::fabs(dir[k]) < 1e-9f) {
			if (from[k] < low[k] || from[k] > high[k]) return false;
			continue;
		}
		float t0 = (low[k] - from[k]) / dir[k];
		float t1 = (high[k] - from[k]) / dir[k];
		if (t0 > t1) std::swap(t0, t1);
		enter = std::max(enter, t0);
		leave = std::min(leave, t1);
		if (enter > leave) return false;
	}

	const float start[3] = {from[0] + dir[0] * enter, from[1] + dir[1] * enter,
		from[2] + dir[2] * enter};
	int cell[3], step[3];
	float nextBoundary[3], delta[3];
	for (int k = 0; k < 3; ++k) {
		cell[k] = (int)std::floor((start[k] - low[k]) * grid.inverseCell[k]);
		cell[k] = std::max(0, std::min(grid.dim[k] - 1, cell[k]));
		if (std::fabs(dir[k]) < 1e-9f) {
			step[k] = 0;
			nextBoundary[k] = leave + 1.f;
			delta[k] = leave + 1.f;
		} else {
			step[k] = dir[k] > 0 ? 1 : -1;
			const float edge = low[k] + (cell[k] + (dir[k] > 0 ? 1 : 0)) * grid.cell[k];
			nextBoundary[k] = enter + (edge - start[k]) / dir[k];
			delta[k] = std::fabs(grid.cell[k] / dir[k]);
		}
	}

	bool found = false;
	float nearest = range;
	// A triangle bucketed into several cells must not be tested several times.
	static thread_local std::vector<uint32_t> stamp;
	static thread_local uint32_t generation = 0;
	const size_t triangleCount = world.indices.size() / 3;
	if (stamp.size() != triangleCount) {
		stamp.assign(triangleCount, 0);
		generation = 0;
	}
	if (++generation == 0) {
		std::fill(stamp.begin(), stamp.end(), 0);
		generation = 1;
	}

	for (;;) {
		const int index = grid.Index(cell[0], cell[1], cell[2]);
		for (uint32_t s = grid.cellStart[index]; s < grid.cellStart[index + 1]; ++s) {
			const uint32_t t = grid.triangles[s];
			if (t >= triangleCount || stamp[t] == generation) continue;
			stamp[t] = generation;
			const Vec3 a = world.positions[world.indices[t * 3]];
			const Vec3 b = world.positions[world.indices[t * 3 + 1]];
			const Vec3 c = world.positions[world.indices[t * 3 + 2]];
			const Vec3 e1 = b - a, e2 = c - a, p = Cross(direction, e2);
			const float determinant = Dot(e1, p);
			if (std::fabs(determinant) < 1e-7f) continue;
			const float inverse = 1 / determinant;
			const Vec3 offset = origin - a;
			const float u = Dot(offset, p) * inverse;
			if (u < 0 || u > 1) continue;
			const Vec3 q = Cross(offset, e1);
			const float v = Dot(direction, q) * inverse;
			if (v < 0 || u + v > 1) continue;
			const float distance = Dot(e2, q) * inverse;
			if (distance <= .01f || distance >= nearest) continue;
			nearest = distance;
			hit = {origin + direction * distance, Normalize(Cross(e1, e2)), distance};
			if (Dot(hit.normal, direction) > 0) hit.normal = hit.normal * -1;
			found = true;
		}
		// Only safe to stop once the whole current cell is behind the best hit, because a
		// triangle overlapping this cell may still be nearer than one found earlier.
		const int axis = (nextBoundary[0] < nextBoundary[1])
			? (nextBoundary[0] < nextBoundary[2] ? 0 : 2)
			: (nextBoundary[1] < nextBoundary[2] ? 1 : 2);
		if (found && nearest <= nextBoundary[axis]) break;
		if (nextBoundary[axis] > leave || step[axis] == 0) break;
		cell[axis] += step[axis];
		if (cell[axis] < 0 || cell[axis] >= grid.dim[axis]) break;
		nextBoundary[axis] += delta[axis];
	}
	return found;
}

} // namespace Filmmaker::Environment

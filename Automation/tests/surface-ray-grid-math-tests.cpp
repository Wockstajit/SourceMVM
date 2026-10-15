// The grid raycast exists purely to be faster, so every test here compares it against the
// brute-force sweep it replaces on the same geometry. A speed-up that quietly changes which
// surface a foot or a wall probe finds would be worse than the stutter it fixes.
#include "../../AfxHookSource2/Filmmaker/Environment/SurfaceRayGridMath.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>

using namespace Filmmaker::Environment;

namespace {

void Check(bool ok, const char *message) {
	if (!ok) {
		std::cerr << "FAIL: " << message << '\n';
		std::exit(1);
	}
}

uint32_t Random(uint32_t &state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return state;
}

float RandomFloat(uint32_t &state, float low, float high) {
	return low + (high - low) * ((Random(state) & 0xffffff) / (float)0xffffff);
}

// A floor grid plus a few upright walls and a raised platform: the shapes the ragdoll probes
// actually meet, and enough vertical overlap that a lazy grid would return the wrong surface.
OcclusionMesh BuildTestMap(int tiles, float tileSize) {
	OcclusionMesh mesh;
	auto quad = [&mesh](Vec3 a, Vec3 b, Vec3 c, Vec3 d) {
		const uint32_t base = (uint32_t)mesh.positions.size();
		mesh.positions.push_back(a);
		mesh.positions.push_back(b);
		mesh.positions.push_back(c);
		mesh.positions.push_back(d);
		const uint32_t order[6] = {base, base + 1, base + 2, base, base + 2, base + 3};
		for (uint32_t i : order) mesh.indices.push_back(i);
	};
	for (int y = 0; y < tiles; ++y) {
		for (int x = 0; x < tiles; ++x) {
			const float x0 = x * tileSize, y0 = y * tileSize;
			const float z = ((x + y) % 7 == 0) ? 6.f : 0.f; // occasional step
			quad({x0, y0, z}, {x0 + tileSize, y0, z}, {x0 + tileSize, y0 + tileSize, z},
				{x0, y0 + tileSize, z});
		}
	}
	const float span = tiles * tileSize;
	for (int i = 1; i < tiles; i += 5) {
		const float x0 = i * tileSize;
		quad({x0, 0, 0}, {x0, span, 0}, {x0, span, 70}, {x0, 0, 70});
	}
	quad({0, 0, 120}, {span, 0, 120}, {span, span, 120}, {0, span, 120}); // ceiling
	mesh.low = {0, 0, 0};
	mesh.high = {span, span, 120};
	return mesh;
}

bool SameHit(const SurfaceHit &a, const SurfaceHit &b) {
	return std::fabs(a.distance - b.distance) < 1e-3f
		&& std::fabs(a.position.x - b.position.x) < 1e-2f
		&& std::fabs(a.position.y - b.position.y) < 1e-2f
		&& std::fabs(a.position.z - b.position.z) < 1e-2f
		&& std::fabs(a.normal.x - b.normal.x) < 1e-3f
		&& std::fabs(a.normal.y - b.normal.y) < 1e-3f
		&& std::fabs(a.normal.z - b.normal.z) < 1e-3f;
}

} // namespace

int main() {
	// --- degenerate input fails closed rather than building a broken grid -----------------
	SurfaceRayGrid empty;
	OcclusionMesh nothing;
	Check(!BuildSurfaceRayGrid(nothing, empty), "Empty mesh builds no grid");
	SurfaceHit unused;
	Check(!SurfaceRaycastGrid(nothing, empty, {0, 0, 0}, {0, 0, -1}, 10, unused),
		"Raycast against an empty mesh misses");

	// --- a single flat plane still needs a usable grid on the collapsed axis ---------------
	OcclusionMesh plane;
	plane.positions = {{0, 0, 0}, {64, 0, 0}, {64, 64, 0}, {0, 64, 0}};
	plane.indices = {0, 1, 2, 0, 2, 3};
	plane.low = {0, 0, 0};
	plane.high = {64, 64, 0};
	SurfaceRayGrid planeGrid;
	Check(BuildSurfaceRayGrid(plane, planeGrid), "Flat plane builds a grid");
	SurfaceHit planeHit;
	Check(SurfaceRaycastGrid(plane, planeGrid, {32, 32, 20}, {0, 0, -1}, 40, planeHit),
		"Downward ray hits a flat plane");
	Check(std::fabs(planeHit.distance - 20.f) < 1e-3f && planeHit.normal.z > 0.99f,
		"Flat plane hit distance and upward normal");

	// --- full agreement with brute force over many random rays -----------------------------
	const OcclusionMesh map = BuildTestMap(48, 32.f);
	SurfaceRayGrid grid;
	Check(BuildSurfaceRayGrid(map, grid), "Map builds a grid");
	Check(grid.Valid() && grid.CellCount() > 1, "Grid has cells");
	Check(grid.triangles.size() >= map.indices.size() / 3, "Every triangle is bucketed");

	uint32_t state = 0x5eed1234;
	int agreed = 0, hits = 0;
	const float span = 48 * 32.f;
	for (int i = 0; i < 4000; ++i) {
		const Vec3 origin{RandomFloat(state, 0, span), RandomFloat(state, 0, span),
			RandomFloat(state, 1, 110)};
		Vec3 direction{RandomFloat(state, -1, 1), RandomFloat(state, -1, 1),
			RandomFloat(state, -1, 1)};
		if (std::fabs(direction.x) + std::fabs(direction.y) + std::fabs(direction.z) < 1e-3f)
			direction = {0, 0, -1};
		// Mostly the short probes the ragdoll fires, with some long ones for coverage.
		const float range = (i % 4) ? 24.f : 512.f;
		SurfaceHit brute{}, fast{};
		const bool bruteHit = SurfaceRaycast(map, origin, direction, range, brute);
		const bool fastHit = SurfaceRaycastGrid(map, grid, origin, direction, range, fast);
		Check(bruteHit == fastHit, "Grid and brute force agree on hit or miss");
		if (bruteHit) {
			++hits;
			Check(SameHit(brute, fast), "Grid and brute force return the same surface");
		}
		++agreed;
	}
	Check(hits > 500, "Random rays actually hit geometry");
	Check(agreed == 4000, "All rays compared");

	// --- rays that start outside the grid box must still enter it ---------------------------
	SurfaceHit outsideBrute{}, outsideFast{};
	const Vec3 outside{-500, span * .5f, 40};
	const bool ob = SurfaceRaycast(map, outside, {1, 0, 0}, 4096, outsideBrute);
	const bool of = SurfaceRaycastGrid(map, grid, outside, {1, 0, 0}, 4096, outsideFast);
	Check(ob == of, "Outside-origin ray agrees on hit or miss");
	if (ob) Check(SameHit(outsideBrute, outsideFast), "Outside-origin ray finds the same surface");

	// --- a ray fully outside the box misses without walking cells ---------------------------
	SurfaceHit away{};
	Check(!SurfaceRaycastGrid(map, grid, {-500, -500, 400}, {0, 0, 1}, 100, away),
		"Ray that never enters the grid misses");

	// --- an invalid grid falls back to brute force rather than losing collision -------------
	SurfaceRayGrid broken;
	SurfaceHit fallback{};
	Check(SurfaceRaycastGrid(map, broken, {200, 200, 60}, {0, 0, -1}, 200, fallback),
		"Invalid grid falls back to the brute-force sweep");

	// --- the whole point: short probes must be far cheaper than the sweep -------------------
	const int probes = 3000;
	Vec3 probeOrigin[3000];
	for (int i = 0; i < probes; ++i)
		probeOrigin[i] = {RandomFloat(state, 32, span - 32), RandomFloat(state, 32, span - 32),
			RandomFloat(state, 20, 90)};
	auto clock = std::chrono::steady_clock::now;
	SurfaceHit sink{};
	const auto bruteStart = clock();
	for (int i = 0; i < probes; ++i) SurfaceRaycast(map, probeOrigin[i], {0, 0, -1}, 24.f, sink);
	const double bruteMs = std::chrono::duration<double, std::milli>(clock() - bruteStart).count();
	const auto fastStart = clock();
	for (int i = 0; i < probes; ++i)
		SurfaceRaycastGrid(map, grid, probeOrigin[i], {0, 0, -1}, 24.f, sink);
	const double fastMs = std::chrono::duration<double, std::milli>(clock() - fastStart).count();
	std::printf("short probes x%d: brute %.2f ms, grid %.2f ms (%.1fx)\n", probes, bruteMs, fastMs,
		fastMs > 0 ? bruteMs / fastMs : 0.0);
	// Deliberately loose: this asserts the acceleration exists at all, not a machine-specific
	// ratio. The measured speed-up on this mesh is far larger.
	Check(fastMs * 5.0 < bruteMs, "Grid raycast is at least 5x faster on short probes");

	std::printf("surface ray grid tests passed (%zu cells, %zu bucket entries for %zu triangles)\n",
		grid.CellCount(), grid.triangles.size(), map.indices.size() / 3);
	return 0;
}

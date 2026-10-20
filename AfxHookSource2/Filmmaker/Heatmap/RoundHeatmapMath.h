#pragma once

// ROUND HEAT MAP: where the players walked during one round, painted on the floor.
//
// FilmmakerDemoInfo's -paths mode (FilmmakerDemoInfoGo/paths.go) writes every living player's feet
// position through every round. This header reads that file, finds the round at a demo tick and
// rasterises the round into a floor grid around the camera. Each texel counts how many separate
// times a player's body (the 32-unit-wide hull) covered it, so a texel nobody stood on stays 0 and a
// path is exactly as wide as the people who used it. Two height layers per texel keep an upper
// floor's traffic off the floor under it. RoundHeatmap.cpp owns loading and threads,
// RoundHeatmapRender.cpp paints the grid. Engine-free; Automation/tests/round-heatmap-math-tests.cpp.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::Heatmap {

constexpr uint32_t kPathsVersion = 1;
enum SampleFlags : uint8_t { kStrokeStart = 1, kOnGround = 2, kDucking = 4 };

struct PathSample {
	float x = 0, y = 0, z = 0;
	uint8_t flags = 0;
};
struct PlayerPath {
	uint32_t accountId = 0;
	uint8_t team = 0; // 2 = T, 3 = CT
	std::vector<PathSample> samples;
};
struct RoundPaths {
	int startTick = 0, endTick = 0; // demo ticks, end exclusive
	bool warmup = false;
	std::vector<PlayerPath> players;
};
struct DemoPaths {
	std::string map;
	int sampleStep = 2; // demo ticks between two samples
	std::vector<RoundPaths> rounds;
};

inline bool LooksLikePaths(const void* data, size_t size) {
	return data && size >= 4 && std::memcmp(data, "FMRP", 4) == 0;
}

// Reads the helper's little-endian file. Any truncation or out-of-range count fails the whole file.
inline bool ParsePaths(const void* data, size_t size, DemoPaths& out, std::string& error) {
	out = DemoPaths{};
	const uint8_t* p = static_cast<const uint8_t*>(data);
	size_t o = 0;
	bool ok = true;
	auto take = [&](void* v, size_t n) {
		if (!ok || size - o < n) { ok = false; std::memset(v, 0, n); return; }
		std::memcpy(v, p + o, n);
		o += n;
	};
	auto u8 = [&] { uint8_t v; take(&v, 1); return v; };
	auto u16 = [&] { uint16_t v; take(&v, 2); return v; };
	auto u32 = [&] { uint32_t v; take(&v, 4); return v; };
	auto i16 = [&] { int16_t v; take(&v, 2); return v; };
	auto i32 = [&] { int32_t v; take(&v, 4); return v; };

	if (!LooksLikePaths(data, size)) { error = "not a round paths file"; return false; }
	o = 4;
	if (u32() != kPathsVersion) { error = "round paths file has another version"; return false; }
	out.sampleStep = u16();
	const uint16_t mapLen = u16();
	if (ok && size - o >= mapLen) { out.map.assign(reinterpret_cast<const char*>(p + o), mapLen); o += mapLen; }
	else ok = false;
	const uint32_t rounds = u32();
	if (!ok || out.sampleStep < 1 || rounds > 4096) { error = "round paths header is damaged"; return false; }
	out.rounds.resize(rounds);
	for (auto& r : out.rounds) {
		r.startTick = i32();
		r.endTick = i32();
		r.warmup = u8() != 0;
		const uint16_t players = u16();
		if (!ok || players > 256 || r.endTick < r.startTick) { error = "round paths round is damaged"; return false; }
		r.players.resize(players);
		for (auto& pl : r.players) {
			pl.accountId = u32();
			pl.team = u8();
			const uint32_t count = u32();
			if (!ok || count > (size - o) / 7) { error = "round paths samples are truncated"; return false; }
			pl.samples.resize(count);
			for (auto& s : pl.samples) {
				s.x = i16();
				s.y = i16();
				s.z = i16();
				s.flags = u8();
			}
		}
	}
	if (!ok) { error = "round paths file is truncated"; return false; }
	if (o != size) { error = "round paths file has trailing data"; return false; }
	return true;
}

// The round playing at a demo tick: the last one that started at or before it. A tick past the last
// round's end still reads as that round (the demo's closing seconds); before the first one is -1.
inline int RoundAt(const DemoPaths& paths, int tick) {
	int found = -1;
	for (size_t i = 0; i < paths.rounds.size(); ++i) {
		if (paths.rounds[i].startTick > tick) break;
		found = int(i);
	}
	return found;
}

// The round's number in the match itself (1-based), the way the scoreboard counts: warmup and the
// rounds before the match went live are 0, and they are left out of the count.
inline int MatchRoundNumber(const DemoPaths& paths, int index) {
	if (index < 0 || index >= int(paths.rounds.size()) || paths.rounds[size_t(index)].warmup) return 0;
	int n = 0;
	for (int i = 0; i <= index; ++i) n += paths.rounds[size_t(i)].warmup ? 0 : 1;
	return n;
}
inline int MatchRoundCount(const DemoPaths& paths) {
	int n = 0;
	for (const auto& r : paths.rounds) n += r.warmup ? 0 : 1;
	return n;
}

// ---- the floor grid ----

constexpr int kGridMax = 1024;       // texels per side
constexpr float kCellMin = 2.f;      // world units per texel at the finest
constexpr float kBodyRadius = 16.f;  // half of CS2's 32-unit player hull: the strip a walk covers
constexpr float kLayerGap = 64.f;    // stamps further apart in height are on different floors
constexpr float kRevisitSeconds = 1.5f; // back on a spot after this long counts as another visit
constexpr float kRadiusMin = 256.f, kRadiusMax = 4096.f, kRadiusDefault = 1500.f;
constexpr float kFullVisits = 6.f;   // this many visits paint full red
constexpr float kTeleportUnits = 96.f; // two samples further apart are not joined by a walk

inline float ClampRadius(float r) {
	if (!std::isfinite(r)) return kRadiusDefault;
	return (std::min)(kRadiusMax, (std::max)(kRadiusMin, r));
}

// The square of world XY the grid covers. It reaches past the shown radius so a short camera move
// keeps it, and its corner sits on a multiple of the cell so two grids at different centres share
// texel edges (the paint does not shimmer when the grid is rebuilt).
struct Window {
	float x0 = 0, y0 = 0, cell = 0;
	int size = 0;
	float cx = 0, cy = 0, radius = 0;
	bool Valid() const { return size > 0 && cell > 0; }
	float Extent() const { return cell * float(size); }
};

inline Window WindowAround(float cx, float cy, float radius) {
	Window w;
	radius = ClampRadius(radius);
	const float half = radius * 1.25f + kBodyRadius;
	w.cell = (std::max)(kCellMin, 2.f * half / float(kGridMax - 2));
	w.size = (std::min)(kGridMax, int(std::ceil(2.f * half / w.cell)) + 2);
	w.x0 = std::floor((cx - half) / w.cell) * w.cell;
	w.y0 = std::floor((cy - half) / w.cell) * w.cell;
	w.cx = cx;
	w.cy = cy;
	w.radius = radius;
	return w;
}

// The grid no longer covers what is shown: the camera's disc left it, or the radius changed.
inline bool NeedsRebuild(const Window& w, float cx, float cy, float radius) {
	if (!w.Valid()) return true;
	if (std::fabs(ClampRadius(radius) - w.radius) > .5f) return true;
	const float reach = w.radius + kBodyRadius;
	return cx - reach < w.x0 || cy - reach < w.y0 || cx + reach > w.x0 + w.Extent() || cy + reach > w.y0 + w.Extent();
}

struct Grid {
	Window window;
	float baseZ = 0;            // texel heights are stored relative to this (half floats on the GPU)
	std::vector<float> texels;  // size * size * 4: visits A, height A - baseZ, visits B, height B - baseZ
	float maxVisits = 0;
	size_t segments = 0;        // segments rasterised into the window, for the log
	int players = 0;            // players with at least one stamp in the window
};

namespace detail {
struct Work {
	int size = 0;
	std::vector<float> visits[2], zSum[2], zCount[2];
	std::vector<uint32_t> last; // (player + 1) << 24 | sample index of the last stamp
	explicit Work(int n) : size(n) {
		const size_t count = size_t(n) * size_t(n);
		for (int l = 0; l < 2; ++l) {
			visits[l].assign(count, 0.f);
			zSum[l].assign(count, 0.f);
			zCount[l].assign(count, 0.f);
		}
		last.assign(count, 0u);
	}
	int Layer(size_t i, float z) const {
		if (zCount[0][i] == 0) return 0;
		const float d0 = std::fabs(z - zSum[0][i] / zCount[0][i]);
		if (d0 <= kLayerGap) return 0;
		if (zCount[1][i] == 0) return 1;
		const float d1 = std::fabs(z - zSum[1][i] / zCount[1][i]);
		if (d1 <= kLayerGap) return 1;
		return d0 <= d1 ? 0 : 1;
	}
	void Deposit(size_t i, float z, uint32_t marker, uint32_t revisit) {
		const uint32_t prev = last[i];
		const bool fresh = (prev >> 24) != (marker >> 24) || (marker & 0xFFFFFFu) - (prev & 0xFFFFFFu) > revisit;
		const int l = Layer(i, z);
		zSum[l][i] += z;
		zCount[l][i] += 1;
		if (fresh || visits[l][i] == 0) visits[l][i] += 1;
		last[i] = marker;
	}
};

// Every texel whose centre lies within r of the segment a-b (a capsule; a == b stamps a disc). The
// height at a texel is the segment's height at its closest point.
inline void StampSegment(Work& work, const Window& w, float ax, float ay, float az, float bx, float by, float bz,
	float r, uint32_t marker, uint32_t revisit) {
	const float minX = (std::min)(ax, bx) - r, maxX = (std::max)(ax, bx) + r;
	const float minY = (std::min)(ay, by) - r, maxY = (std::max)(ay, by) + r;
	const int i0 = (std::max)(0, int(std::floor((minX - w.x0) / w.cell)));
	const int i1 = (std::min)(w.size - 1, int(std::floor((maxX - w.x0) / w.cell)));
	const int j0 = (std::max)(0, int(std::floor((minY - w.y0) / w.cell)));
	const int j1 = (std::min)(w.size - 1, int(std::floor((maxY - w.y0) / w.cell)));
	if (i0 > i1 || j0 > j1) return;
	const float dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy, r2 = r * r;
	for (int j = j0; j <= j1; ++j) {
		const float py = w.y0 + (float(j) + .5f) * w.cell;
		for (int i = i0; i <= i1; ++i) {
			const float px = w.x0 + (float(i) + .5f) * w.cell;
			float t = len2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.f;
			t = (std::min)(1.f, (std::max)(0.f, t));
			const float qx = ax + t * dx - px, qy = ay + t * dy - py;
			if (qx * qx + qy * qy > r2) continue;
			work.Deposit(size_t(j) * size_t(w.size) + size_t(i), az + t * (bz - az), marker, revisit);
		}
	}
}
} // namespace detail

// The floor height a sample stands for. On the ground it is the feet. In the air (a jump, a fall)
// it is the lower of the feet and the last ground the player left, so a jump does not lift its
// stretch of path off the floor and a fall paints the floor it lands towards.
inline float FloorHeight(const PathSample& s, float& lastGround, bool& haveGround) {
	if (s.flags & kOnGround) {
		lastGround = s.z;
		haveGround = true;
		return s.z;
	}
	return haveGround ? (std::min)(s.z, lastGround) : s.z;
}

// Rasterises one round into the window. teams: 0 = everyone, 2 = T only, 3 = CT only.
inline void BuildGrid(const RoundPaths& round, int sampleStep, const Window& w, Grid& out, int teams = 0) {
	out = Grid{};
	out.window = w;
	if (!w.Valid()) return;
	const float reach = kBodyRadius + w.cell;
	const float minX = w.x0 - reach, minY = w.y0 - reach, maxX = w.x0 + w.Extent() + reach, maxY = w.y0 + w.Extent() + reach;
	auto inside = [&](float x, float y) { return x >= minX && x <= maxX && y >= minY && y <= maxY; };

	// Heights go to the GPU as half floats relative to the middle of what is in the window.
	float zLow = 1e30f, zHigh = -1e30f;
	for (const auto& pl : round.players) {
		if (teams && pl.team != teams) continue;
		for (const auto& s : pl.samples)
			if (inside(s.x, s.y)) { zLow = (std::min)(zLow, s.z); zHigh = (std::max)(zHigh, s.z); }
	}
	if (zLow > zHigh) return;
	out.baseZ = std::round((zLow + zHigh) * .5f);

	detail::Work work(w.size);
	const uint32_t revisit = uint32_t((std::max)(1.f, kRevisitSeconds * 64.f / float((std::max)(1, sampleStep))));
	for (size_t pi = 0; pi < round.players.size() && pi < 255; ++pi) {
		const auto& pl = round.players[pi];
		if (teams && pl.team != teams) continue;
		const uint32_t who = uint32_t(pi + 1) << 24;
		bool stamped = false, haveGround = false;
		float lastGround = 0, px = 0, py = 0, pz = 0;
		bool havePrev = false;
		// The visit clock counts samples, and jumps past the revisit window where a stroke breaks: the
		// gap behind a new stroke (a respawn, a reconnect) can be any length, so coming back from it is
		// always a new visit.
		uint32_t clock = 0;
		for (size_t si = 0; si < pl.samples.size(); ++si) {
			const auto& s = pl.samples[si];
			if (s.flags & kStrokeStart) { havePrev = false; haveGround = false; }
			const float z = FloorHeight(s, lastGround, haveGround);
			if (havePrev) {
				const float dx = s.x - px, dy = s.y - py, dz = s.z - pz;
				if (dx * dx + dy * dy + dz * dz > kTeleportUnits * kTeleportUnits) havePrev = false;
			}
			clock += (si > 0 && !havePrev) ? revisit + 1 : 1;
			// A stroke's first sample stamps a disc, every later one the capsule back to the previous.
			const float ax = havePrev ? px : s.x, ay = havePrev ? py : s.y, az = havePrev ? pz : z;
			if ((std::max)(ax, s.x) >= minX && (std::min)(ax, s.x) <= maxX
				&& (std::max)(ay, s.y) >= minY && (std::min)(ay, s.y) <= maxY) {
				detail::StampSegment(work, w, ax, ay, az, s.x, s.y, z, kBodyRadius, who | (clock & 0xFFFFFFu), revisit);
				++out.segments;
				stamped = true;
			}
			px = s.x; py = s.y; pz = z;
			havePrev = true;
		}
		if (stamped) ++out.players;
	}

	const size_t count = size_t(w.size) * size_t(w.size);
	out.texels.assign(count * 4, 0.f);
	for (size_t i = 0; i < count; ++i) {
		for (int l = 0; l < 2; ++l) {
			const float v = work.visits[l][i];
			if (v <= 0) continue;
			out.texels[i * 4 + l * 2] = v;
			out.texels[i * 4 + l * 2 + 1] = work.zSum[l][i] / work.zCount[l][i] - out.baseZ;
			out.maxVisits = (std::max)(out.maxVisits, v);
		}
	}
}

// How strongly a texel with this many visits is painted, 0..1. Logarithmic, so one walk-through
// reads clearly and a choke point that everyone crossed is the full colour.
inline float Intensity(float visits) {
	if (!(visits > 0)) return 0.f;
	return (std::min)(1.f, std::log2(1.f + visits) / std::log2(1.f + kFullVisits));
}

} // namespace Filmmaker::Heatmap

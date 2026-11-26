#pragma once

// Pure engine-free math for Per-Player Mattes: 3D skeleton/capsule Voronoi partitioning of
// player-depth pixels so a single chosen player can be isolated cleanly even in a crowded shot.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace Filmmaker::PlayerMatte {

inline constexpr int kMaxTargetBones = 16;
inline constexpr int kMaxOtherPlayers = 16;

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct BoneSphere {
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float radius = 14.0f; // world units (includes gear / held gun tolerance)
};

struct PlayerCapsule {
	float feetX = 0.0f, feetY = 0.0f, feetZ = 0.0f;
	float height = 74.0f;
	float radius = 22.0f;
};

struct HighlightSettings {
	bool enabled = false;
	float dimWorld = 0.65f;       // 0..1 darkening/desaturation of non-selected pixels
	float desaturateWorld = 0.75f;// 0..1 world desaturation while highlighting
	float outlineStrength = 0.55f;// 0..1 edge highlight around the picked player
	float tint[3] = { 1.0f, 0.82f, 0.32f };
};

// GPU-ready snapshot published from main thread -> render thread.
struct RenderSnapshot {
	bool hasTarget = false;
	int controller = -1;
	int pawn = -1;
	int targetBoneCount = 0;
	// xyz = bone position, w = sphere radius around that bone
	float targetBones[kMaxTargetBones][4] = {};
	// Fallback vertical capsule if bone read was unavailable (xyz = feet, w = height)
	float targetCapsule[4] = {};
	float targetBoundsCenter[4] = {}; // xyz = center, w = max bounding radius
	int otherCount = 0;
	// xyz = other player feet, w = height
	float otherCapsules[kMaxOtherPlayers][4] = {};
	HighlightSettings highlight;
};

inline float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

inline float PointToSegmentDistSq(Vec3 p, Vec3 a, Vec3 b) {
	const float abx = b.x - a.x;
	const float aby = b.y - a.y;
	const float abz = b.z - a.z;
	const float apx = p.x - a.x;
	const float apy = p.y - a.y;
	const float apz = p.z - a.z;
	const float denom = abx * abx + aby * aby + abz * abz;
	float t = denom > 1e-6f ? (apx * abx + apy * aby + apz * abz) / denom : 0.0f;
	t = Clamp(t, 0.0f, 1.0f);
	const float cx = a.x + abx * t - p.x;
	const float cy = a.y + aby * t - p.y;
	const float cz = a.z + abz * t - p.z;
	return cx * cx + cy * cy + cz * cz;
}

inline float SignedDistToCapsule(Vec3 p, float feetX, float feetY, float feetZ, float height, float radius) {
	const float pad = (std::min)(radius, height * 0.45f);
	const Vec3 a{ feetX, feetY, feetZ + pad };
	const Vec3 b{ feetX, feetY, feetZ + (std::max)(pad, height - pad) };
	return std::sqrt(PointToSegmentDistSq(p, a, b)) - radius;
}

// Evaluates how strongly a 3D world point `p` (already verified to be a player pixel via
// depth-vs-world) belongs to the selected player in `snap`. Returns 0..1.
// Uses signed distance to the target player's 3D bone cloud (or vertical capsule fallback)
// compared against all other players' 3D capsules (Voronoi partition in 3D world space).
inline float EvaluateOwnership(Vec3 p, const RenderSnapshot& snap) {
	if (!snap.hasTarget)
		return 0.0f;

	// Fast bounding-sphere reject (prevents distant players or loose props from claiming ownership).
	const float dx = p.x - snap.targetBoundsCenter[0];
	const float dy = p.y - snap.targetBoundsCenter[1];
	const float dz = p.z - snap.targetBoundsCenter[2];
	const float maxR = snap.targetBoundsCenter[3] > 1.0f ? snap.targetBoundsCenter[3] : 96.0f;
	if (dx * dx + dy * dy + dz * dz > maxR * maxR)
		return 0.0f;

	float dTarget = 1e9f;
	if (snap.targetBoneCount > 0) {
		for (int i = 0; i < snap.targetBoneCount; ++i) {
			const float bx = p.x - snap.targetBones[i][0];
			const float by = p.y - snap.targetBones[i][1];
			const float bz = p.z - snap.targetBones[i][2];
			const float r = snap.targetBones[i][3] > 1.0f ? snap.targetBones[i][3] : 16.0f;
			const float d = std::sqrt(bx * bx + by * by + bz * bz) - r;
			if (d < dTarget) dTarget = d;
		}
	} else {
		dTarget = SignedDistToCapsule(p, snap.targetCapsule[0], snap.targetCapsule[1],
			snap.targetCapsule[2], snap.targetCapsule[3] > 1.0f ? snap.targetCapsule[3] : 74.0f, 22.0f);
	}

	// Hard outer limit so stray smoke/debris far from the target skeleton never leaks in.
	constexpr float kMaxMargin = 32.0f;
	if (dTarget > kMaxMargin)
		return 0.0f;

	float dOther = 1e9f;
	for (int i = 0; i < snap.otherCount; ++i) {
		const float h = snap.otherCapsules[i][3] > 1.0f ? snap.otherCapsules[i][3] : 74.0f;
		const float d = SignedDistToCapsule(p, snap.otherCapsules[i][0], snap.otherCapsules[i][1],
			snap.otherCapsules[i][2], h, 18.0f);
		if (d < dOther) dOther = d;
	}

	// Smooth 3-unit transition at tight 3D player-to-player contact boundaries.
	const float advantage = dOther - dTarget;
	if (snap.otherCount > 0) {
		if (advantage <= -1.5f) return 0.0f;
		if (advantage < 1.5f) return Clamp((advantage + 1.5f) / 3.0f, 0.0f, 1.0f);
	}
	return 1.0f;
}

inline void ComputeTargetBounds(RenderSnapshot& snap) {
	if (!snap.hasTarget) {
		snap.targetBoundsCenter[0] = snap.targetBoundsCenter[1] = snap.targetBoundsCenter[2] = 0.0f;
		snap.targetBoundsCenter[3] = 0.0f;
		return;
	}
	if (snap.targetBoneCount > 0) {
		float minX = snap.targetBones[0][0], maxX = minX;
		float minY = snap.targetBones[0][1], maxY = minY;
		float minZ = snap.targetBones[0][2], maxZ = minZ;
		for (int i = 1; i < snap.targetBoneCount; ++i) {
			minX = (std::min)(minX, snap.targetBones[i][0]);
			maxX = (std::max)(maxX, snap.targetBones[i][0]);
			minY = (std::min)(minY, snap.targetBones[i][1]);
			maxY = (std::max)(maxY, snap.targetBones[i][1]);
			minZ = (std::min)(minZ, snap.targetBones[i][2]);
			maxZ = (std::max)(maxZ, snap.targetBones[i][2]);
		}
		const float cx = 0.5f * (minX + maxX);
		const float cy = 0.5f * (minY + maxY);
		const float cz = 0.5f * (minZ + maxZ);
		float maxDist = 0.0f;
		for (int i = 0; i < snap.targetBoneCount; ++i) {
			const float dx = snap.targetBones[i][0] - cx;
			const float dy = snap.targetBones[i][1] - cy;
			const float dz = snap.targetBones[i][2] - cz;
			const float r = std::sqrt(dx * dx + dy * dy + dz * dz) + snap.targetBones[i][3] + 34.0f;
			if (r > maxDist) maxDist = r;
		}
		snap.targetBoundsCenter[0] = cx;
		snap.targetBoundsCenter[1] = cy;
		snap.targetBoundsCenter[2] = cz;
		snap.targetBoundsCenter[3] = (std::max)(maxDist, 64.0f);
	} else {
		const float h = snap.targetCapsule[3] > 1.0f ? snap.targetCapsule[3] : 74.0f;
		snap.targetBoundsCenter[0] = snap.targetCapsule[0];
		snap.targetBoundsCenter[1] = snap.targetCapsule[1];
		snap.targetBoundsCenter[2] = snap.targetCapsule[2] + h * 0.5f;
		snap.targetBoundsCenter[3] = (std::max)(h * 0.65f + 42.0f, 84.0f);
	}
}

} // namespace Filmmaker::PlayerMatte

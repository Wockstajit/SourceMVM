#pragma once

// Pure engine-free math and data model for World-Space Decals & Wall Media:
//   * Text, Logo/Image, Video on walls & floors (OBB deferred projection)
//   * Procedural Arrows and Animated Painted Routes drawing across the ground
//   * Attachment to any Scene Object so projections move with the shot

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace Filmmaker::WorldDecals {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;
inline constexpr int kMaxRoutePoints = 32;

enum class DecalKind : int {
	Text = 0,
	Image = 1,
	Video = 2,
	Arrow = 3,
	Route = 4,
	Count
};

enum class BlendMode : int {
	Alpha = 0,
	Additive = 1,
	Multiply = 2,
	LitPaint = 3,
	Count
};

inline const char* KindToken(DecalKind k) {
	switch (k) {
	case DecalKind::Text: return "text";
	case DecalKind::Image: return "image";
	case DecalKind::Video: return "video";
	case DecalKind::Arrow: return "arrow";
	case DecalKind::Route: return "route";
	default: return "text";
	}
}

inline bool KindFromToken(const char* s, DecalKind& out) {
	if (!s) return false;
	for (int i = 0; i < (int)DecalKind::Count; ++i)
		if (0 == _stricmp(s, KindToken((DecalKind)i))) { out = (DecalKind)i; return true; }
	if (0 == _stricmp(s, "logo") || 0 == _stricmp(s, "poster")) { out = DecalKind::Image; return true; }
	if (0 == _stricmp(s, "line") || 0 == _stricmp(s, "path")) { out = DecalKind::Route; return true; }
	return false;
}

inline const char* BlendToken(BlendMode b) {
	switch (b) {
	case BlendMode::Alpha: return "alpha";
	case BlendMode::Additive: return "additive";
	case BlendMode::Multiply: return "multiply";
	case BlendMode::LitPaint: return "lit";
	default: return "alpha";
	}
}

inline bool BlendFromToken(const char* s, BlendMode& out) {
	if (!s) return false;
	for (int i = 0; i < (int)BlendMode::Count; ++i)
		if (0 == _stricmp(s, BlendToken((BlendMode)i))) { out = (BlendMode)i; return true; }
	return false;
}

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

inline Vec3 Add(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3 Scale(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(Vec3 a, Vec3 b) {
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline float Length(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline Vec3 Normalize(Vec3 a) {
	const float l = Length(a);
	return l > 1e-6f ? Scale(a, 1.0f / l) : Vec3{ 1.0f, 0.0f, 0.0f };
}

inline float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

// Source 2 Euler angles (pitch, yaw, roll in degrees) -> orthonormal basis:
//   forward = projection direction (into wall/floor)
//   right   = decal +U axis
//   up      = decal -V axis (top of image/text)
inline void BasisFromAngles(const float angles[3], Vec3& forward, Vec3& right, Vec3& up) {
	const float p = angles[0] * kDegToRad;
	const float y = angles[1] * kDegToRad;
	const float r = angles[2] * kDegToRad;
	const float sp = std::sin(p), cp = std::cos(p);
	const float sy = std::sin(y), cy = std::cos(y);
	const float sr = std::sin(r), cr = std::cos(r);
	forward = Normalize({ cp * cy, cp * sy, -sp });
	right = Normalize({ sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, sr * cp });
	// Keep right pointing to viewer's right (Source right = -left)
	right = Scale(right, -1.0f);
	up = Normalize(Cross(right, forward));
}

// Computes Source 2 Euler angles (pitch, yaw, roll=0) whose forward vector points along `dir`.
inline void AnglesFromForward(Vec3 dir, float outAngles[3]) {
	const Vec3 d = Normalize(dir);
	const float xy = std::sqrt(d.x * d.x + d.y * d.y);
	outAngles[0] = std::atan2(-d.z, xy) / kDegToRad;
	outAngles[1] = xy > 1e-5f ? (std::atan2(d.y, d.x) / kDegToRad) : 0.0f;
	outAngles[2] = 0.0f;
}

struct Decal {
	int id = 0;
	std::string name;
	DecalKind kind = DecalKind::Text;
	bool enabled = true;
	bool render = true;

	// World-space (or resolved attached) projection box
	float position[3] = { 0.0f, 0.0f, 0.0f };
	float angles[3] = { 0.0f, 0.0f, 0.0f };
	float width = 96.0f;   // world units along decal U
	float height = 64.0f;  // world units along decal V
	float depth = 48.0f;   // projection box depth along forward

	float color[4] = { 1.0f, 1.0f, 1.0f, 0.95f };
	BlendMode blend = BlendMode::Alpha;
	float normalFade = 0.35f; // fade out on surfaces perpendicular to projection
	bool skipPlayers = true;

	// Text decal properties
	std::string text = "SITE A";
	int fontSize = 72;
	bool bold = true;
	bool outline = true;

	// Image / Logo / Video properties
	std::string mediaPath;
	float videoSpeed = 1.0f;
	float videoStartTime = 0.0f;
	bool videoLoop = true;

	// Arrow properties
	float arrowHead = 0.36f;
	float arrowShaft = 0.22f;
	bool arrowDashed = false;
	float arrowPulse = 0.0f;

	// Animated ground route / painted line properties
	std::vector<Vec3> routePoints;
	float routeWidth = 14.0f;
	float routeProgress = 1.0f; // manual 0..1 progress when routeAnimate is false
	bool routeAnimate = false;
	float routeStartTime = 0.0f;
	float routeDuration = 3.0f;
	bool routeDashed = false;
	bool routeArrowCap = true;

	// Scene object attachment (-1 = world space)
	int attachObjectId = -1;
	float attachOffset[3] = { 0.0f, 0.0f, 0.0f };
	float attachAngles[3] = { 0.0f, 0.0f, 0.0f };

	uint64_t textureRevision = 1;
};

// Puts a decal on a surface. `normal` is the surface normal facing the viewer. The decal projects
// into the surface (forward = -normal) and its centre sits on the hit point, so it stays glued to
// that surface and does not turn toward whoever looks at it later. A floor or ceiling keeps the
// viewer's yaw at placement, so text reads upright from where it was put. A wall keeps world up
// (roll 0), so it never tilts.
inline void OrientDecalToSurface(Vec3 hit, Vec3 normal, float viewerYawDeg, Decal& d) {
	normal = Normalize(normal);
	d.position[0] = hit.x; d.position[1] = hit.y; d.position[2] = hit.z;
	if (std::fabs(normal.z) > 0.85f) {
		d.angles[0] = normal.z > 0.0f ? 89.5f : -89.5f; // looking down onto a floor, up onto a ceiling
		d.angles[1] = viewerYawDeg;
		d.angles[2] = 0.0f;
	} else {
		AnglesFromForward(Scale(normal, -1.0f), d.angles);
	}
}

// Moves a decal to a surface point while it is dragged. Unlike placing, a wall decal keeps its roll
// and a decal already lying on a floor keeps its turn, so dragging only moves it.
inline void DragDecalToSurface(Vec3 hit, Vec3 normal, float cameraYawDeg, Decal& d) {
	const bool wasFlat = std::fabs(d.angles[0]) > 80.0f;
	const float roll = d.angles[2];
	OrientDecalToSurface(hit, normal, wasFlat ? d.angles[1] : cameraYawDeg, d);
	if (std::fabs(Normalize(normal).z) <= 0.85f) d.angles[2] = roll;
}

inline bool ProjectWorldToDecal(Vec3 worldPos, const Decal& d, float& outU, float& outV, float& outLocalZ);

// The decal a click on surface point `p` lands on: the smallest one whose box holds it (with some
// slack along its depth), so a small decal over a large one can still be picked. Routes have no
// box and are not picked. -1 when none.
inline int PickDecalAt(Vec3 p, const std::vector<Decal>& decals, float depthSlack = 16.0f) {
	int best = -1;
	float bestArea = 1e30f;
	for (const Decal& d : decals) {
		if (!d.enabled || d.kind == DecalKind::Route) continue;
		Decal grown = d;
		grown.depth = d.depth + 2.0f * depthSlack;
		float u = 0, v = 0, z = 0;
		if (!ProjectWorldToDecal(p, grown, u, v, z)) continue;
		const float area = d.width * d.height;
		if (area < bestArea) { bestArea = area; best = d.id; }
	}
	return best;
}

// Projects a world-space point into a decal's local box.
// Returns true iff inside [-0.5, 0.5] along U, V, and projection depth.
inline bool ProjectWorldToDecal(Vec3 worldPos, const Decal& d,
	float& outU, float& outV, float& outLocalZ) {
	Vec3 fwd, right, up;
	BasisFromAngles(d.angles, fwd, right, up);
	const Vec3 rel = Sub(worldPos, { d.position[0], d.position[1], d.position[2] });
	const float hw = (std::max)(d.width * 0.5f, 1.0f);
	const float hh = (std::max)(d.height * 0.5f, 1.0f);
	const float hd = (std::max)(d.depth * 0.5f, 1.0f);
	const float lx = Dot(rel, right);
	const float ly = Dot(rel, up);
	const float lz = Dot(rel, fwd);
	if (std::fabs(lx) > hw || std::fabs(ly) > hh || std::fabs(lz) > hd)
		return false;
	outU = lx / (2.0f * hw) + 0.5f;
	outV = 0.5f - ly / (2.0f * hh);
	outLocalZ = lz / hd;
	return true;
}

// Evaluates current route draw progress in [0, 1] at `demoTime`.
inline float EvaluateRouteProgress(const Decal& d, double demoTime) {
	if (!d.routeAnimate)
		return Clamp(d.routeProgress, 0.0f, 1.0f);
	const float dur = (std::max)(d.routeDuration, 0.05f);
	return Clamp(((float)demoTime - d.routeStartTime) / dur, 0.0f, 1.0f);
}

// Total arc length of a 3D polyline.
inline float RouteTotalLength(const std::vector<Vec3>& pts) {
	float total = 0.0f;
	for (size_t i = 1; i < pts.size(); ++i)
		total += Length(Sub(pts[i], pts[i - 1]));
	return total;
}

// Evaluates shortest distance from `worldPos` to the drawn portion (`progress` in 0..1) of a
// polyline. Also outputs `outArcDist` (distance from start) and `outDrawnLen`.
inline float DistanceToAnimatedRoute(Vec3 worldPos, const std::vector<Vec3>& pts,
	float progress, float& outArcDist, float& outDrawnLen) {
	outArcDist = 0.0f;
	outDrawnLen = 0.0f;
	if (pts.size() < 2) return 1e9f;
	const float total = RouteTotalLength(pts);
	const float drawn = total * Clamp(progress, 0.0f, 1.0f);
	outDrawnLen = drawn;
	if (drawn <= 1e-4f) return 1e9f;

	float bestDist = 1e9f;
	float accum = 0.0f;
	for (size_t i = 1; i < pts.size() && accum < drawn; ++i) {
		const Vec3 a = pts[i - 1];
		Vec3 b = pts[i];
		const Vec3 ab = Sub(b, a);
		const float segLen = Length(ab);
		if (segLen < 1e-5f) continue;
		const float remain = drawn - accum;
		const float usedLen = (std::min)(segLen, remain);
		if (usedLen < segLen)
			b = Add(a, Scale(ab, usedLen / segLen));
		const Vec3 seg = Sub(b, a);
		const float segSq = Dot(seg, seg);
		const float t = segSq > 1e-6f ? Clamp(Dot(Sub(worldPos, a), seg) / segSq, 0.0f, 1.0f) : 0.0f;
		const Vec3 closest = Add(a, Scale(seg, t));
		const float dist = Length(Sub(worldPos, closest));
		if (dist < bestDist) {
			bestDist = dist;
			outArcDist = accum + usedLen * t;
		}
		accum += usedLen;
	}
	return bestDist;
}

// Composes a decal's local offset/angles onto a parent Scene Object's world pose.
inline void ComposeAttachedPose(const float parentPos[3], const float parentAngles[3],
	const float localOffset[3], const float localAngles[3],
	float outPos[3], float outAngles[3]) {
	Vec3 fwd, right, up;
	BasisFromAngles(parentAngles, fwd, right, up);
	const Vec3 worldOffset = Add(
		Add(Scale(fwd, localOffset[0]), Scale(right, localOffset[1])),
		Scale(up, localOffset[2]));
	outPos[0] = parentPos[0] + worldOffset.x;
	outPos[1] = parentPos[1] + worldOffset.y;
	outPos[2] = parentPos[2] + worldOffset.z;
	outAngles[0] = parentAngles[0] + localAngles[0];
	outAngles[1] = parentAngles[1] + localAngles[1];
	outAngles[2] = parentAngles[2] + localAngles[2];
}

} // namespace Filmmaker::WorldDecals

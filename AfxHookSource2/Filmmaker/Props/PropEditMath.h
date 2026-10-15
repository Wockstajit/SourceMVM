#pragma once

// Engine-free maths for the in-world prop editor: the ray under the cursor, the turn rings, the
// prop's on-screen bounds and click picking. Viewport coordinates are normalized (0..1, Y down)
// over the full rendered frame, the same convention as Environment::GizmoProject.

#include "../Environment/EnvironmentGizmoMath.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::Props {

using Environment::Matrix;
using Environment::Vec3;

// Source's AngleQuaternion (mathlib): QAngle (pitch, yaw, roll) in degrees -> quaternion x,y,z,w.
inline void AngleQuaternion(double pitch, double yaw, double roll, float q[4]) {
	const double d2r = 3.14159265358979323846 / 180.0;
	const double sp = std::sin(pitch * d2r * 0.5), cp = std::cos(pitch * d2r * 0.5);
	const double sy = std::sin(yaw * d2r * 0.5), cy = std::cos(yaw * d2r * 0.5);
	const double sr = std::sin(roll * d2r * 0.5), cr = std::cos(roll * d2r * 0.5);
	const double srXcp = sr * cp, crXsp = cr * sp;
	const double crXcp = cr * cp, srXsp = sr * sp;
	q[0] = (float)(srXcp * cy - crXsp * sy);
	q[1] = (float)(crXsp * cy + srXcp * sy);
	q[2] = (float)(crXcp * sy - srXsp * cy);
	q[3] = (float)(crXcp * cy + srXsp * sy);
}

// Direction from the eye through viewport point (u, v).
inline bool CursorRay(const Matrix& worldToClip, Vec3 eye, float u, float v, Vec3& direction) {
	Matrix inverse;
	if (!std::isfinite(u) || !std::isfinite(v) || !Environment::Inverse(worldToClip, inverse)) return false;
	const Vec3 point = Environment::Transform(inverse, {u * 2 - 1, 1 - v * 2, .5f});
	if (!Environment::Finite(point)) return false;
	const Vec3 d = point - eye;
	if (Environment::Dot(d, d) < 1e-8f) return false;
	direction = Environment::Normalize(d);
	return Environment::Finite(direction);
}

// Angle wrapped to (-180, 180].
inline float WrapDegrees(float a) {
	a = std::fmod(a, 360.0f);
	if (a <= -180) a += 360;
	else if (a > 180) a -= 360;
	return a;
}

// Turn snapping: the nearest multiple of `step` degrees, wrapped to (-180, 180].
inline float SnapDegrees(float a, float step) {
	if (!(step > 0) || !std::isfinite(a)) return a;
	return WrapDegrees(std::round(a / step) * step);
}

// Floor snapping: the origin height that rests the prop's lowest point (model-space minsZ, scaled)
// on a surface at floorZ. Most CS2 props have their origin at the base (minsZ ~ 0); a model centred
// on its origin would otherwise sink half into the floor.
inline float RestingZ(float floorZ, float minsZ, float scale) { return floorZ - minsZ * scale; }

// How high above a prop's origin the downward floor probe starts: enough to clear a kerb or step the
// drag crossed, low enough not to land on a balcony overhead.
constexpr float kFloorProbeLift = 48.0f;

// The eight corners of a prop's model-space box [mins, maxs], placed and turned like the prop.
inline void BoxCorners(Vec3 position, Vec3 angles, float scale, Vec3 mins, Vec3 maxs, Vec3 out[8]) {
	const Matrix model = Environment::ModelMatrix(position, angles, {scale, scale, scale});
	for (int i = 0; i < 8; ++i)
		out[i] = Environment::Transform(model, {i & 1 ? maxs.x : mins.x, i & 2 ? maxs.y : mins.y, i & 4 ? maxs.z : mins.z});
}

// Screen rectangle of a set of world points. Invalid when any point is behind the camera, so a
// prop the camera stands inside is never picked through its own back face.
struct ScreenRect {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool valid = false;
	bool Contains(float u, float v) const { return valid && u >= x0 && u <= x1 && v >= y0 && v <= y1; }
};
inline ScreenRect ProjectRect(const Matrix& worldToClip, const Vec3* points, int count) {
	ScreenRect r;
	for (int i = 0; i < count; ++i) {
		const auto p = Environment::GizmoProject(worldToClip, points[i]);
		const float w = worldToClip[12] * points[i].x + worldToClip[13] * points[i].y + worldToClip[14] * points[i].z + worldToClip[15];
		if (!(w > .001f) || !std::isfinite(p.x) || !std::isfinite(p.y)) return {};
		if (i == 0) { r.x0 = r.x1 = p.x; r.y0 = r.y1 = p.y; }
		else { r.x0 = std::min(r.x0, p.x); r.x1 = std::max(r.x1, p.x); r.y0 = std::min(r.y0, p.y); r.y1 = std::max(r.y1, p.y); }
	}
	r.valid = count > 0;
	return r;
}

// A click picks the nearest prop whose screen rectangle holds the cursor; 0 when none does.
struct PickCandidate {
	int id = 0;
	ScreenRect rect;
	float distance = 0; // eye to prop origin
};
inline int Pick(const std::vector<PickCandidate>& candidates, float u, float v) {
	int best = 0;
	float bestDistance = 0;
	for (const auto& c : candidates) {
		if (!c.rect.Contains(u, v)) continue;
		if (!best || c.distance < bestDistance) { best = c.id; bestDistance = c.distance; }
	}
	return best;
}

// ---- the three turn rings -----------------------------------------------------------------------
// Source angles (pitch, yaw, roll) turn a model by yaw about world Z, then pitch about its turned
// Y axis, then roll about its own forward axis. A ring per angle, about that angle's axis, means a
// drag on a ring changes exactly one angle: Turn (yaw), Tilt (pitch) and Roll.
enum class Ring { Turn = 0, Tilt = 1, Roll = 2 };

// The axis ring `r` turns about, for a prop at `angles` (degrees).
inline Vec3 RingAxis(Vec3 angles, Ring r) {
	const float p = angles.x * 0.017453292519943295f, y = angles.y * 0.017453292519943295f;
	switch (r) {
	case Ring::Tilt: return {-std::sin(y), std::cos(y), 0};
	case Ring::Roll: return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), -std::sin(p)};
	case Ring::Turn:
	default: return {0, 0, 1};
	}
}

// Two unit vectors spanning the plane square to `axis`, ordered so that a positive angle from
// `a` towards `b` is a right-handed turn about `axis`.
inline void RingBasis(Vec3 axis, Vec3& a, Vec3& b) {
	const Vec3 n = Environment::Normalize(axis);
	const Vec3 helper = std::abs(n.z) < 0.9f ? Vec3{0, 0, 1} : Vec3{1, 0, 0};
	a = Environment::Normalize(Environment::Cross(helper, n));
	b = Environment::Cross(n, a);
}

// Point i of n on the ring of radius `radius` about `axis` through `center`.
inline Vec3 AxisRingPoint(Vec3 center, Vec3 axis, float radius, int i, int n) {
	Vec3 a, b;
	RingBasis(axis, a, b);
	const float t = 6.283185307179586f * float(i) / float(n);
	return center + a * (std::cos(t) * radius) + b * (std::sin(t) * radius);
}

// The angle (degrees, right-handed about `axis`) of the point where the ray meets the ring's plane.
// False when the ray runs along the plane, points away from it, or hits the centre.
inline bool RingAngle(Vec3 eye, Vec3 direction, Vec3 center, Vec3 axis, float& degrees) {
	const Vec3 n = Environment::Normalize(axis);
	const float along = Environment::Dot(direction, n);
	if (std::abs(along) < 1e-4f) return false;
	const float t = Environment::Dot(center - eye, n) / along;
	if (!std::isfinite(t) || t <= 0) return false;
	const Vec3 d = eye + direction * t - center;
	Vec3 a, b;
	RingBasis(n, a, b);
	const float x = Environment::Dot(d, a), y = Environment::Dot(d, b);
	if (x * x + y * y < 1e-6f) return false;
	degrees = std::atan2(y, x) * 57.29577951308232f;
	return true;
}

// The angles after turning `deltaDegrees` (right-handed) on ring `r`: a plain change of the one
// angle that ring stands for (checked against ModelMatrix in prop-edit-math-tests.cpp).
inline Vec3 TurnOnRing(Vec3 angles, Ring r, float deltaDegrees) {
	switch (r) {
	case Ring::Tilt: angles.x = WrapDegrees(angles.x + deltaDegrees); break;
	case Ring::Roll: angles.z = WrapDegrees(angles.z + deltaDegrees); break;
	case Ring::Turn:
	default: angles.y = WrapDegrees(angles.y + deltaDegrees); break;
	}
	return angles;
}

// ---- the paint brush -------------------------------------------------------------------------------
// Painting scatters copies of a model inside a circle on the ground. Spacing keeps any two copies
// (new or already painted) at least that far apart on the ground plane, which is what makes a
// stroke read as a natural patch rather than a pile: painting over a spot that is already full
// adds nothing.

// A small deterministic generator (xorshift32): the same seed paints the same stroke.
inline float Random01(uint32_t& state) {
	if (!state) state = 0x9e3779b9u;
	state ^= state << 13; state ^= state >> 17; state ^= state << 5;
	return float(state & 0xffffff) / 16777216.0f;
}

// Up to `maxNew` ground points (z = center.z) inside the circle, each at least `spacing` from the
// others and from `existing`. `attempts` bounds the work when the circle is already full.
inline std::vector<Vec3> Scatter(Vec3 center, float radius, float spacing, int maxNew, uint32_t& seed,
                                 const std::vector<Vec3>& existing, int attempts = 64) {
	std::vector<Vec3> out;
	if (!(radius > 0) || maxNew <= 0) return out;
	const float minD2 = spacing > 0 ? spacing * spacing : 0.0f;
	auto clear = [&](Vec3 p) {
		for (const Vec3& e : existing) {
			const float dx = e.x - p.x, dy = e.y - p.y;
			if (dx * dx + dy * dy < minD2) return false;
		}
		for (const Vec3& e : out) {
			const float dx = e.x - p.x, dy = e.y - p.y;
			if (dx * dx + dy * dy < minD2) return false;
		}
		return true;
	};
	for (int i = 0; i < attempts && int(out.size()) < maxNew; ++i) {
		// Uniform over the disc: radius from the square root of a uniform number.
		const float r = radius * std::sqrt(Random01(seed)), a = 6.283185307179586f * Random01(seed);
		const Vec3 p{center.x + std::cos(a) * r, center.y + std::sin(a) * r, center.z};
		if (clear(p)) out.push_back(p);
	}
	return out;
}

// Brush DENSITY: how full one stroke may make the circle, 0.05..1 of what the spacing lets fit.
// At 1 a held brush fills every gap (the old behaviour); lower, a stroke stops adding copies once
// the circle holds that share of its capacity, so the patch stays sparse however long it is held.
// Capacity is the disc's area over the area one copy claims (a disc of half the spacing per copy),
// packed at kPaintPacking. A random fill that keeps a minimum spacing jams at about 55% (random
// sequential adsorption) and, with a bounded number of tries per spray, settles near 45%. The old
// 70% was a target no brush could reach, so everything from ~65% to 100% painted the same patch
// and the Density slider looked dead at the top.
constexpr float kPaintPacking = 0.42f;
inline int PaintCapacity(float radius, float spacing) {
	if (!(radius > 0) || !(spacing > 0)) return 0;
	const float perCopy = 3.14159265f * (spacing * 0.5f) * (spacing * 0.5f);
	return std::max(1, int(std::floor(kPaintPacking * 3.14159265f * radius * radius / perCopy)));
}
// Tries one spray makes to find gaps: enough to reach the packing above in a held brush.
constexpr int kPaintScatterAttempts = 256;

// Where the ground probe under a painted copy starts, above the brush centre: as high as the brush
// is wide, so a copy on ground that rises inside the circle (up to 45 degrees) still finds it. A
// quarter of the radius started under rising ground and those copies were silently dropped.
inline float PaintProbeLift(float radius) { return std::max(kFloorProbeLift, radius); }

// Which of a mixed brush's `count` models the next copy is.
inline int PaintPick(int count, uint32_t& seed) {
	if (count <= 1) return 0;
	return std::min(count - 1, int(Random01(seed) * count));
}

// How many new copies the next scatter may add: what density still allows in the circle, at most
// `batch`. `inside` = this stroke's copies already within the circle.
inline int PaintBudget(float radius, float spacing, float density, int inside, int batch) {
	density = std::max(0.05f, std::min(1.0f, density));
	const int target = std::max(1, int(std::ceil(density * PaintCapacity(radius, spacing))));
	return std::max(0, std::min(batch, target - inside));
}

// A painted copy's scale: 1 plus or minus `jitter` (0..1), times the brush's base size.
inline float PaintScale(float base, float jitter, uint32_t& seed) {
	jitter = std::max(0.0f, std::min(0.9f, jitter));
	return base * (1.0f + jitter * (Random01(seed) * 2 - 1));
}

// A painted copy's yaw: a random turn of up to `spread` degrees either way (180 = any direction).
inline float PaintYaw(float spread, uint32_t& seed) {
	spread = std::max(0.0f, std::min(180.0f, spread));
	return WrapDegrees(spread * (Random01(seed) * 2 - 1));
}

// ---- placing against a surface ------------------------------------------------------------------
// Where a copy sits on the surface under the cursor, from that surface's normal and the model's own
// bounds (model space, before scale):
//   floor (normal within 60 degrees of up)  upright, keeping `yaw`, its bottom on the surface;
//   ceiling (within 60 degrees of down)     upright, keeping `yaw`, its top against the surface;
//   wall (anything steeper)                 upright, flat against the wall: the horizontal model
//                                           axis it is thinnest along faces out of the wall, the
//                                           way the model extends further pointing outward, and its
//                                           back on the wall, centred on the cursor in height.
// Wall cards (ivy, vines, posters) have no back face, so this is what keeps their front toward
// the room; the thinnest-axis rule finds that front whichever way the artist modelled it.
struct SurfacePose {
	Vec3 position, angles;
	bool wall = false;
};
inline SurfacePose PoseOnSurface(Vec3 hit, Vec3 normal, Vec3 mins, Vec3 maxs, float scale, float yaw) {
	SurfacePose out;
	const float nLen = std::sqrt(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z);
	const Vec3 n = nLen > 1e-4f ? Vec3{normal.x / nLen, normal.y / nLen, normal.z / nLen} : Vec3{0, 0, 1};
	out.angles = {0, yaw, 0};
	out.position = hit;
	if (n.z >= 0.5f) { out.position.z = hit.z - mins.z * scale; return out; }
	if (n.z <= -0.5f) { out.position.z = hit.z - maxs.z * scale; return out; }
	out.wall = true;
	const float hLen = std::sqrt(n.x * n.x + n.y * n.y);
	const float hx = n.x / hLen, hy = n.y / hLen; // out of the wall, horizontal
	const float facing = std::atan2(hy, hx) * 57.29577951308232f;
	const bool alongX = (maxs.x - mins.x) <= (maxs.y - mins.y);
	const float lo = alongX ? mins.x : mins.y, hi = alongX ? maxs.x : maxs.y;
	const bool positiveOut = hi >= -lo; // the model reaches further this way: that side faces the room
	// Yaw that turns the chosen model axis (+X, or +Y which sits 90 degrees on) onto the outward
	// direction, or the opposite axis for a model that reaches the other way.
	float y = facing - (alongX ? 0.0f : 90.0f) + (positiveOut ? 0.0f : 180.0f);
	out.angles = {0, WrapDegrees(y), 0};
	// The back plane on the wall: local `lo` (or `hi` when flipped) along the outward direction.
	const float back = (positiveOut ? lo : -hi) * scale;
	out.position = {hit.x - hx * back, hit.y - hy * back, hit.z - (mins.z + maxs.z) * 0.5f * scale};
	return out;
}

} // namespace Filmmaker::Props

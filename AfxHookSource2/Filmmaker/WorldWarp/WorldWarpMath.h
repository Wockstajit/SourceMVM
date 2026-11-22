#pragma once

// Pure engine-free math for World Effects (WorldWarp):
//   * Little Planet   -- 360-degree conformal stereographic projection (street/site at center,
//                        buildings fanning radially outward with recognizable rooftops, sky rim)
//   * Inception       -- upward-curving city street bending overhead
//   * Rolling Horizon -- bend the far end of a street upward or downward, flat near the subject
//   * World Wave      -- traveling wave along an adjustable direction or radial from a scene origin
//   * World Fold      -- lift one side of the scene around a movable 3D hinge axis

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace Filmmaker::WorldWarp {

inline constexpr float kPi = 3.14159265358979323846f;
inline constexpr float kDegToRad = kPi / 180.0f;

enum class Mode : int {
	Off = 0,
	LittlePlanet = 1,
	Inception = 2,
	RollingHorizon = 3,
	WorldWave = 4,
	WorldFold = 5,
	Count
};

inline const char* ModeToken(Mode m) {
	switch (m) {
	case Mode::Off: return "off";
	case Mode::LittlePlanet: return "littleplanet";
	case Mode::Inception: return "inception";
	case Mode::RollingHorizon: return "horizon";
	case Mode::WorldWave: return "wave";
	case Mode::WorldFold: return "fold";
	default: return "off";
	}
}

inline const char* ModeLabel(Mode m) {
	switch (m) {
	case Mode::Off: return "Off";
	case Mode::LittlePlanet: return "Little Planet";
	case Mode::Inception: return "Inception";
	case Mode::RollingHorizon: return "Rolling Horizon";
	case Mode::WorldWave: return "World Wave";
	case Mode::WorldFold: return "World Fold";
	default: return "Off";
	}
}

inline bool ModeFromToken(const char* s, Mode& out) {
	if (!s) return false;
	for (int i = 0; i < (int)Mode::Count; ++i) {
		if (0 == _stricmp(s, ModeToken((Mode)i))) { out = (Mode)i; return true; }
	}
	if (0 == _stricmp(s, "planet") || 0 == _stricmp(s, "little_planet")) { out = Mode::LittlePlanet; return true; }
	if (0 == _stricmp(s, "rolling_horizon") || 0 == _stricmp(s, "rollinghorizon")) { out = Mode::RollingHorizon; return true; }
	if (0 == _stricmp(s, "world_wave") || 0 == _stricmp(s, "worldwave")) { out = Mode::WorldWave; return true; }
	if (0 == _stricmp(s, "world_fold") || 0 == _stricmp(s, "worldfold")) { out = Mode::WorldFold; return true; }
	return false;
}

struct Vec3 {
	float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Settings {
	Mode mode = Mode::Off;

	// Little Planet (conformal stereographic 360 projection)
	float planetScale = 1.15f;       // 0.35..3.0 -- larger brings rooftops closer & sharper
	float planetSpin = 0.0f;         // -180..180 deg rotation around vertical nadir axis
	float planetTilt = 0.0f;         // -45..45 deg pitch shift of pole
	float planetConformal = 0.85f;   // 0..1 blend (1 = pure conformal stereographic, keeps facades recognizable)
	float planetRimSoftness = 0.25f; // 0..1 outer sky rim atmospheric softness

	// Rolling Horizon & Inception
	float horizonBend = 0.85f;       // -2.0 (downward drop) .. +2.0 (upward Inception curl)
	float horizonFlatRadius = 260.0f;// 0..2000 world units kept flat around subject/origin
	float horizonCurve = 2.0f;       // 1.2..3.0 curve exponent (2.0 = smooth parabolic roll)

	// World Wave
	float waveHeight = 140.0f;       // 0..600 world units vertical amplitude
	float waveLength = 650.0f;       // 80..3000 world units per wave cycle
	float waveSpeed = 2.5f;          // -15..15 rad/s traveling speed (synced to demo clock)
	float waveDirection = 0.0f;      // 0..360 compass angle
	bool waveRadial = false;         // false = directional plane wave, true = radial ripple from origin
	float waveFlatRadius = 120.0f;   // 0..1000 near-camera stability radius

	// World Fold
	float foldAngle = 72.0f;         // -180..180 deg lift angle (90 = vertical wall rises horizontal overhead)
	float foldAxisYaw = 0.0f;        // 0..360 deg compass orientation of the hinge line
	float foldOffset = 280.0f;       // -2000..2000 signed offset from origin along hinge normal
	float foldSoftness = 160.0f;     // 0..800 smooth hinge bend fillet radius
	bool foldFlipSide = false;       // lifts the opposite side of the hinge line

	// Shared world origin (for Rolling Horizon / World Wave / World Fold)
	bool useSceneOrigin = false;
	float origin[3] = { 0.0f, 0.0f, 0.0f };
};

inline float Clamp(float v, float lo, float hi) {
	return v < lo ? lo : (v > hi ? hi : v);
}

inline float Smoothstep(float edge0, float edge1, float x) {
	if (edge1 <= edge0) return x >= edge1 ? 1.0f : 0.0f;
	const float t = Clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

inline Settings PresetFor(Mode m) {
	Settings s;
	s.mode = m;
	if (m == Mode::LittlePlanet) {
		s.planetScale = 1.15f;
		s.planetSpin = 0.0f;
		s.planetTilt = 0.0f;
		s.planetConformal = 0.85f;
		s.planetRimSoftness = 0.25f;
	} else if (m == Mode::Inception) {
		s.horizonBend = 1.35f;
		s.horizonFlatRadius = 320.0f;
		s.horizonCurve = 2.15f;
	} else if (m == Mode::RollingHorizon) {
		s.horizonBend = 0.75f;
		s.horizonFlatRadius = 300.0f;
		s.horizonCurve = 2.0f;
	} else if (m == Mode::WorldWave) {
		s.waveHeight = 140.0f;
		s.waveLength = 650.0f;
		s.waveSpeed = 2.5f;
		s.waveDirection = 0.0f;
		s.waveRadial = false;
		s.waveFlatRadius = 120.0f;
	} else if (m == Mode::WorldFold) {
		s.foldAngle = 78.0f;
		s.foldAxisYaw = 0.0f;
		s.foldOffset = 280.0f;
		s.foldSoftness = 160.0f;
		s.foldFlipSide = false;
	}
	return s;
}

// Inverse stereographic projection for Little Planet.
// Input `ndc` is aspect-corrected screen coordinate around center (0,0), where radius ~1 sits at
// the shorter screen edge. Output is a unit direction vector in local planet space where:
//   (0, 0, -1) = nadir (straight down at the street / bombsite in the center)
//   (x, y,  0) = horizon (buildings fanning outward radially 360 deg around center)
//   (0, 0, +1) = zenith (sky wrapping around the outer rim)
inline Vec3 LittlePlanetDirection(float nx, float ny, const Settings& s) {
	const float r = std::sqrt(nx * nx + ny * ny);
	const float scale = (std::max)(s.planetScale, 0.15f);
	const float spin = s.planetSpin * kDegToRad;
	const float phi = std::atan2(ny, nx) + spin;

	// Conformal stereographic polar angle from nadir: theta = 2 * atan(r / scale).
	// Blending slightly toward equidistant (r / scale) widens the horizon band where building
	// faces and rooftops sit so they remain clear and recognizable near the edges.
	const float thetaStereo = 2.0f * std::atan(r / scale);
	const float thetaEqui = Clamp((r / scale) * (kPi * 0.5f), 0.0f, kPi);
	const float conformal = Clamp(s.planetConformal, 0.0f, 1.0f);
	const float theta = thetaEqui + (thetaStereo - thetaEqui) * conformal;

	const float sinT = std::sin(theta);
	const float cosT = std::cos(theta);
	// Local vector: x = forward, y = left, z = up; theta=0 -> (0,0,-1) nadir, theta=pi -> (0,0,+1) zenith.
	Vec3 d{ sinT * std::cos(phi), sinT * std::sin(phi), -cosT };

	// Optional pole tilt around Y axis (pitch shift).
	if (std::fabs(s.planetTilt) > 1e-4f) {
		const float t = s.planetTilt * kDegToRad;
		const float ct = std::cos(t), st = std::sin(t);
		const float rx = d.x * ct + d.z * st;
		const float rz = -d.x * st + d.z * ct;
		d.x = rx;
		d.z = rz;
	}
	const float len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
	if (len > 1e-6f) { d.x /= len; d.y /= len; d.z /= len; }
	return d;
}

// Rolling Horizon & Inception: bends distant geometry vertically up or down while keeping a flat
// region of radius `horizonFlatRadius` around the anchor.
inline Vec3 DeformRollingHorizon(Vec3 p, Vec3 anchor, const Settings& s) {
	const float dx = p.x - anchor.x;
	const float dy = p.y - anchor.y;
	const float d = std::sqrt(dx * dx + dy * dy);
	const float flat = (std::max)(0.0f, s.horizonFlatRadius);
	if (d <= flat) return p;
	const float excess = (d - flat) / 1000.0f;
	const float exp = Clamp(s.horizonCurve, 1.1f, 3.5f);
	const float lift = s.horizonBend * 480.0f * std::pow(excess, exp);
	return { p.x, p.y, p.z + lift };
}

// World Wave: sends a rolling vertical bend across the map along `waveDirection` (or radially)
// from `origin` with controllable height, wavelength, speed, and near-camera stability zone.
inline Vec3 DeformWorldWave(Vec3 p, Vec3 origin, Vec3 camera, double demoTime, const Settings& s) {
	const float dx = p.x - origin.x;
	const float dy = p.y - origin.y;
	const float dirRad = s.waveDirection * kDegToRad;
	const float coord = s.waveRadial
		? std::sqrt(dx * dx + dy * dy)
		: (dx * std::cos(dirRad) + dy * std::sin(dirRad));
	const float wl = (std::max)(s.waveLength, 32.0f);
	const float phase = (2.0f * kPi * coord / wl) - s.waveSpeed * (float)demoTime;
	const float camDx = p.x - camera.x;
	const float camDy = p.y - camera.y;
	const float camD = std::sqrt(camDx * camDx + camDy * camDy);
	const float ramp = s.waveFlatRadius > 1.0f ? Smoothstep(0.0f, s.waveFlatRadius, camD) : 1.0f;
	return { p.x, p.y, p.z + s.waveHeight * std::sin(phase) * ramp };
}

// World Fold: lifts one side of the world around a movable 3D hinge line in XY so an entire row
// of buildings can rotate smoothly upward and overhead.
inline Vec3 DeformWorldFold(Vec3 p, Vec3 anchor, const Settings& s) {
	const float yaw = s.foldAxisYaw * kDegToRad;
	// Unit tangent along hinge line and unit perpendicular pointing into the lifted half-space.
	const float tx = std::cos(yaw);
	const float ty = std::sin(yaw);
	const float sideSign = s.foldFlipSide ? -1.0f : 1.0f;
	const float nx = -ty * sideSign;
	const float ny = tx * sideSign;

	const float hingeX = anchor.x + nx * s.foldOffset;
	const float hingeY = anchor.y + ny * s.foldOffset;
	const float hingeZ = anchor.z;

	const float relX = p.x - hingeX;
	const float relY = p.y - hingeY;
	const float u = relX * nx + relY * ny; // signed perpendicular distance from hinge line
	if (u <= 0.0f) return p;

	const float fullAngle = s.foldAngle * kDegToRad;
	const float soft = (std::max)(0.0f, s.foldSoftness);
	const float blend = soft > 1e-3f ? Smoothstep(0.0f, soft, u) : 1.0f;
	const float angle = fullAngle * blend;

	const float dz = p.z - hingeZ;
	const float ca = std::cos(angle);
	const float sa = std::sin(angle);
	// Rotate (u, dz) in the vertical plane perpendicular to the hinge line.
	const float uNew = u * ca - dz * sa;
	const float zNew = hingeZ + u * sa + dz * ca;

	const float du = uNew - u;
	return { p.x + nx * du, p.y + ny * du, zNew };
}

inline Vec3 DeformPoint(Vec3 p, Vec3 camera, double demoTime, const Settings& s) {
	const Vec3 anchor = s.useSceneOrigin ? Vec3{ s.origin[0], s.origin[1], s.origin[2] } : camera;
	switch (s.mode) {
	case Mode::Inception:
	case Mode::RollingHorizon:
		return DeformRollingHorizon(p, anchor, s);
	case Mode::WorldWave:
		return DeformWorldWave(p, anchor, camera, demoTime, s);
	case Mode::WorldFold:
		return DeformWorldFold(p, anchor, s);
	default:
		return p;
	}
}

} // namespace Filmmaker::WorldWarp

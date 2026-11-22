#pragma once

// Water on the glass uses the demo clock and the camera's motion.
// Centres are normalized screen coordinates. Radii, velocities and runoff lengths
// are measured in frame heights so the same water works at any output aspect.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::LensRain {

struct Params {
	float intensity = 1; // scene rain, 0..1
	float amount = 26;   // independent lens density, 0..100
	float size = 0.7f;   // 0.3..3
	float slide = 1;     // 0..2
	float impact = 0.35f; // 0..2
};

struct Motion {
	float right = 0, up = 0, forward = 0; // camera velocity in its own frame, units per second
	float yawRate = 0, pitchRate = 0;     // degrees per second (yaw > 0 turns left, pitch > 0 looks down)
	float gravity[2] = { 0, 1 };          // world down on the glass: x right, y down
};

enum class Kind { Bead, Rivulet, Splash };

constexpr int kRunoffKnots = 24;
constexpr int kRunoffPoints = kRunoffKnots + 1; // fixed samples followed by the current head
constexpr float kRunoffSpacing = 0.012f;
constexpr float kRunoffLifetime = 1.0f;

struct RunoffKnot {
	float x = 0, y = 0; // normalized position on the glass
	float age = 0, distance = 0;
};

struct Runoff {
	std::array<RunoffKnot, kRunoffKnots> knots{};
	int count = 0;
	float distance = 0;
	float lastMovement = 0;
};

struct Drop {
	float x = 0, y = 0;
	float r = 0;          // unscaled radius
	float vx = 0, vy = 0;
	float age = 0, life = 1;
	float seed = 0;
	Kind kind = Kind::Bead;
	Runoff runoff;
};

constexpr int kMaxDrops = 64;
constexpr float kRunSpeed = 250.0f;

inline float Smooth(float a, float b, float x) {
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
}

// Deposit stationary knots at equal distances along the bead's actual path.
inline void RecordRunoff(Drop& d, float oldX, float oldY, float dt, float moved) {
	if (moved <= 1e-6f) return;
	auto& path = d.runoff;
	if (path.count == 0) path.knots[path.count++] = {oldX, oldY, d.age - dt, 0};
	const float end = path.distance + moved;
	for (float s = path.knots[path.count - 1].distance + kRunoffSpacing; s <= end; s += kRunoffSpacing) {
		if (path.count == kRunoffKnots) {
			std::move(path.knots.begin() + 1, path.knots.end(), path.knots.begin());
			--path.count;
		}
		const float t = (s - path.distance) / moved;
		path.knots[path.count++] = {oldX + (d.x - oldX) * t, oldY + (d.y - oldY) * t,
			d.age - dt * (1.0f - t), s};
	}
	path.distance = end;
	path.lastMovement = d.age;
}

struct WetRunoff {
	struct Point { float x, y, coverage, distance; };
	std::array<Point, kRunoffPoints> points{};
	int count = 0;
};

inline WetRunoff VisibleRunoff(const Drop& d, float trails) {
	WetRunoff wet;
	const auto& path = d.runoff;
	if (trails <= 0 || path.count == 0 || path.distance < kRunoffSpacing) return wet;
	const float length = 0.034f + 0.236f * trails;
	const float lifetime = 0.32f + 0.68f * trails;
	if (d.age - path.lastMovement >= lifetime) return wet;

	int first = 0;
	while (first + 1 < path.count) {
		const auto& next = path.knots[first + 1];
		if (path.distance - next.distance < length && d.age - next.age < lifetime) break;
		++first;
	}
	auto append = [&](const RunoffKnot& knot) {
		const float fromHead = path.distance - knot.distance;
		const float old = d.age - knot.age;
		const float coverage = (1 - Smooth(0.60f * length, length, fromHead))
			* (1 - Smooth(0.24f * lifetime, lifetime, old))
			* Smooth(0, 0.16f, trails);
		wet.points[wet.count++] = {knot.x, knot.y, coverage, knot.distance};
	};
	for (int i = first; i < path.count; ++i) append(path.knots[i]);
	if (path.distance - path.knots[path.count - 1].distance > 1e-6f)
		append({d.x, d.y, path.lastMovement, path.distance});
	return wet;
}

inline float Radius(const Drop& d, const Params& p) {
	return d.r * p.size;
}

inline float VisualRadius(const Drop& d, float size) {
	const float r = d.r * size;
	return d.kind == Kind::Splash ? (std::min)(0.055f, 5.5f * r) : r;
}

// Fade the coverage of a landing drop smoothly without a sharp size change.
inline float Opacity(const Drop& d) {
	return Smooth(0.0f, 0.12f, d.age) * Smooth(0.0f, 0.32f, d.life - d.age);
}

// Camera velocity and turn rate from two poses `dt` seconds apart (pitch, yaw, roll in degrees, the
// rotation Rz(yaw) Ry(pitch) Rx(roll) with columns forward, left, up). A jump (a cut, a seek, a
// teleport) reads as no motion rather than a gale that throws every drop off the frame.
inline Motion MotionFrom(const double prevOrigin[3], const double prevAngles[3],
	const double origin[3], const double angles[3], double dt) {
	Motion m;
	const double d2r = 3.14159265358979323846 / 180.0;
	const double cp = std::cos(angles[0] * d2r), sp = std::sin(angles[0] * d2r);
	const double cy = std::cos(angles[1] * d2r), sy = std::sin(angles[1] * d2r);
	const double cr = std::cos(angles[2] * d2r), sr = std::sin(angles[2] * d2r);
	const double fwd[3] = { cp * cy, cp * sy, -sp };
	const double left[3] = { sp * sr * cy - cr * sy, sp * sr * sy + cr * cy, sr * cp };
	const double up[3] = { sp * cr * cy + sr * sy, sp * cr * sy - sr * cy, cr * cp };
	// World down (0, 0, -1) on the glass: right = -left, screen down = -up.
	m.gravity[0] = (float)left[2];
	m.gravity[1] = (float)up[2];
	if (!(dt > 1e-6)) return m;
	const double v[3] = { (origin[0] - prevOrigin[0]) / dt, (origin[1] - prevOrigin[1]) / dt, (origin[2] - prevOrigin[2]) / dt };
	const double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	auto wrap = [](double a) { return std::remainder(a, 360.0); }; // one step, whatever the size
	const double yaw = wrap(angles[1] - prevAngles[1]) / dt, pitch = wrap(angles[0] - prevAngles[0]) / dt;
	// Faster than a fall or a jump pad: a cut. Turning faster than a flick: a cut too.
	if (speed > 3500.0 || std::fabs(yaw) > 2000.0 || std::fabs(pitch) > 2000.0) return m;
	m.forward = (float)(v[0] * fwd[0] + v[1] * fwd[1] + v[2] * fwd[2]);
	m.right = -(float)(v[0] * left[0] + v[1] * left[1] + v[2] * left[2]);
	m.up = (float)(v[0] * up[0] + v[1] * up[1] + v[2] * up[2]);
	m.yawRate = (float)yaw;
	m.pitchRate = (float)pitch;
	return m;
}

class Sim {
public:
	explicit Sim(uint32_t seed = 0x5eed1234u) { Reset(seed); }

	void Reset(uint32_t seed = 0x5eed1234u) {
		m_drops.clear();
		m_rng = seed ? seed : 1u;
		m_spawn = 0;
	}

	const std::vector<Drop>& Drops() const { return m_drops; }

	void Step(float dt, float aspect, const Motion& motion, const Params& params) {
		if (!(dt > 0)) return;
		dt = (std::min)(dt, 0.1f);
		Move(dt, aspect, motion, params);
		Cull(aspect, params);
		Spawn(dt, motion, params);
	}

	static void Push(const Drop& d, float aspect, const Motion& m, const Params& p, float out[2]) {
		const float weight = Smooth(0.0011f, 0.0042f, Radius(d, p));
		const float release = Smooth(0.015f, 0.14f, d.age);
		const float fall = weight * release * (1.4f + 4.6f * p.intensity);
		auto signedSquare = [](float velocity) {
			const float v = std::clamp(velocity / kRunSpeed, -1.6f, 1.6f);
			return v * std::fabs(v);
		};
		const float fwd = (std::max)(0.0f, signedSquare(m.forward));
		out[0] = p.slide * (m.gravity[0] * fall - 0.73f * signedSquare(m.right)
			+ (d.x - 0.5f) * aspect * fwd + 0.50f * std::clamp(m.yawRate / 360.0f, -2.0f, 2.0f));
		out[1] = p.slide * (m.gravity[1] * fall + 0.73f * signedSquare(m.up)
			+ (d.y - 0.5f) * fwd - 0.50f * std::clamp(m.pitchRate / 360.0f, -2.0f, 2.0f));
		if (d.kind == Kind::Rivulet) {
			// A sliding bead pulls sideways as its contact with the glass changes.
			const float distance = d.runoff.distance;
			const float phase = d.seed * 6.283185f;
			const float pull = p.slide * fall * (0.25f * std::sin(distance * 36 + phase)
				+ 0.12f * std::sin(distance * 91 + phase * 1.7f));
			out[0] += m.gravity[1] * pull;
			out[1] -= m.gravity[0] * pull;
		}
	}

private:
	float Rand() {
		m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
		return (float)(m_rng >> 8) * (1.0f / 16777216.0f);
	}

	void Spawn(float dt, const Motion& m, const Params& p) {
		const float rush = 1.0f + p.impact * 0.40f * std::clamp(m.forward / kRunSpeed, 0.0f, 1.5f);
		m_spawn += (p.amount / 100.0f) * p.intensity * 42.0f * rush * dt;
		while (m_spawn >= 1.0f) {
			m_spawn -= 1.0f;
			if (m_drops.size() >= kMaxDrops) continue;
			const float size = Rand(), shape = Rand();
			Drop d;
			d.r = 0.0019f + 0.0038f * size * size;
			if (shape < 0.06f) {
				d.kind = Kind::Splash;
				d.r = 0.0038f + 0.0028f * size;
			} else if (shape < 0.22f) {
				d.kind = Kind::Rivulet;
				d.r = 0.0026f + 0.0025f * size;
			}
			d.life = 0.85f + 0.95f * Rand();
			d.x = Rand();
			d.y = Rand();
			d.seed = Rand();
			m_drops.push_back(d);
		}
	}

	static bool OutsideFrame(const Drop& d, float aspect, const Params& p) {
		const float r = VisualRadius(d, p.size) * 1.6f;
		const float reachX = r / aspect;
		return d.x < -reachX || d.x > 1 + reachX || d.y < -r || d.y > 1 + r;
	}

	void Move(float dt, float aspect, const Motion& m, const Params& p) {
		const float damping = 4.6f;
		const float drag = std::exp(-damping * dt);
		const float velocityIntegral = (1.0f - drag) / damping;
		const float forceIntegral = (dt - velocityIntegral) / damping;
		for (Drop& d : m_drops) {
			const bool ended = d.age >= d.life || OutsideFrame(d, aspect, p);
			d.age += dt;
			if (ended) continue;
			float a[2];
			Push(d, aspect, m, p, a);
			const float grip = 0.20f * 0.0028f / Radius(d, p);
			if (p.slide <= 0 || (std::hypot(d.vx, d.vy) < 0.012f && std::hypot(a[0], a[1]) < grip)) {
				d.vx = d.vy = 0;
				a[0] = a[1] = 0;
			}
			const float dx = d.vx * velocityIntegral + a[0] * forceIntegral;
			const float dy = d.vy * velocityIntegral + a[1] * forceIntegral;
			d.vx = d.vx * drag + a[0] * velocityIntegral;
			d.vy = d.vy * drag + a[1] * velocityIntegral;
			const float oldX = d.x, oldY = d.y;
			d.x += dx / aspect;
			d.y += dy;
			if (d.kind == Kind::Rivulet) RecordRunoff(d, oldX, oldY, dt, std::hypot(dx, dy));
		}
	}

	void Cull(float aspect, const Params& p) {
		m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [&](const Drop& d) {
			if (d.runoff.count > 0 && d.age - d.runoff.lastMovement < kRunoffLifetime) return false;
			return d.age >= d.life || OutsideFrame(d, aspect, p);
		}), m_drops.end());
	}

	std::vector<Drop> m_drops;
	uint32_t m_rng = 1;
	float m_spawn = 0;
};

} // namespace Filmmaker::LensRain

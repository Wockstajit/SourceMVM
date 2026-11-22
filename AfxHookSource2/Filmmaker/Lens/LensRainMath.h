#pragma once

// Water on the glass uses the demo clock and the camera's motion.
// Centres are normalized screen coordinates. Radii, velocities and runoff lengths
// are measured in frame heights so the same water works at any output aspect.

#include <algorithm>
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

struct Drop {
	float x = 0, y = 0;
	float r = 0;          // unscaled radius
	float vx = 0, vy = 0;
	float age = 0, life = 1;
	float seed = 0;
	Kind kind = Kind::Bead;
	float trailX = 0, trailY = 0;
};

constexpr int kMaxDrops = 64;
constexpr float kRunSpeed = 250.0f;
constexpr float kMaxRunoff = 0.055f;

inline float Smooth(float a, float b, float x) {
	const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
	return t * t * (3 - 2 * t);
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

	void Move(float dt, float aspect, const Motion& m, const Params& p) {
		const float damping = 4.6f;
		const float drag = std::exp(-damping * dt);
		const float velocityIntegral = (1.0f - drag) / damping;
		const float forceIntegral = (dt - velocityIntegral) / damping;
		const float trailDecay = std::exp(-13.0f * dt);
		for (Drop& d : m_drops) {
			d.age += dt;
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
			d.x += dx / aspect;
			d.y += dy;
			if (d.kind == Kind::Rivulet) {
				d.trailX = trailDecay * d.trailX + dx;
				d.trailY = trailDecay * d.trailY + dy;
				const float length = std::hypot(d.trailX, d.trailY);
				const float scale = kMaxRunoff / (std::max)(kMaxRunoff, length);
				d.trailX *= scale;
				d.trailY *= scale;
			}
		}
	}

	void Cull(float aspect, const Params& p) {
		m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [&](const Drop& d) {
			const float r = VisualRadius(d, p.size) * 1.6f;
			const float reachX = (r + std::fabs(d.trailX)) / aspect;
			const float reachY = r + std::fabs(d.trailY);
			return d.age >= d.life || d.x < -reachX || d.x > 1 + reachX || d.y < -reachY || d.y > 1 + reachY;
		}), m_drops.end());
	}

	std::vector<Drop> m_drops;
	uint32_t m_rng = 1;
	float m_spawn = 0;
};

} // namespace Filmmaker::LensRain

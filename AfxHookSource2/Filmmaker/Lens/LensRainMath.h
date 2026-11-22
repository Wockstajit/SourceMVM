#pragma once

// LensRainMath: water drops on the front of the lens, engine-free so it can be tested
// (Automation/tests/lens-rain-math-tests.cpp). Lens/LensRain.cpp steps it once per main-thread frame
// with the demo's own time and draws the result.
//
// SCREEN SPACE. A drop lives on the glass: x runs 0..aspect (frame widths are height * aspect), y runs
// 0..1 downwards, so a radius means the same on every frame size. Velocities are frame heights per
// second.
//
// WHAT MOVES A DROP. Four forces act on it, all in screen space:
// - gravity: world down seen on the glass (Motion::gravity). Looking level it points straight down the
//   frame; looking at the floor it is nearly zero, so the drops stay put. Only drops past a critical
//   size feel it, the way small drops cling to a window;
// - the air the camera runs into: moving forward blows the drops outward from the middle of the
//   frame, moving sideways blows them the other way across it, both growing with the square of speed,
//   the way rain runs off a windscreen;
// - the turn: panning flings the drops the other way across the frame;
// - friction: a drop stays where it is until the push beats its grip, which is stronger for a small
//   drop, and a moving drop slows down.
// A moving drop leaves small drops behind it (rain.trails), and two drops that touch merge into one
// with both volumes, so drops grow, slide, leave a trail and run off the frame.
//
// DETERMINISM. Everything comes from the steps it is given and its own random sequence, so a render
// of the same frames at the same rate gets the same drops.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Filmmaker::LensRain {

struct Params {
	float amount = 40;      // 0..100: how much rain reaches the lens
	float size = 1;         // 0.3..3: drop size
	float slide = 1;        // 0..2: how easily motion and gravity move the drops
	float trails = 0.5f;    // 0..1: drops left behind by a moving drop
	float impact = 1;       // 0..2: how many more drops hit the lens while the camera moves forward
};

struct Motion {
	float right = 0, up = 0, forward = 0; // camera velocity in its own frame, units per second
	float yawRate = 0, pitchRate = 0;     // degrees per second (yaw > 0 turns left, pitch > 0 looks down)
	float gravity[2] = { 0, 1 };          // world down on the glass: x right, y down, length 0..1
};

struct Drop {
	float x = 0, y = 0;   // centre, screen space
	float r = 0;          // radius, frame heights
	float vx = 0, vy = 0; // frame heights per second
	float age = 0;        // seconds
	float seed = 0;       // 0..1, a per-drop constant for the shader (shape wobble)
};

constexpr int kMaxDrops = 384;
constexpr float kRunSpeed = 250.0f;     // units per second: a running player, the scale of the air push
constexpr float kCriticalRadius = 0.006f; // frame heights: drops past this feel gravity
constexpr float kMinRadius = 0.0009f;   // smaller drops are gone (evaporated)

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

	// One step of `dt` seconds on a frame `aspect` wide (width / height). dt 0 (a paused demo) leaves
	// every drop where it is.
	void Step(float dt, float aspect, const Motion& m, const Params& p) {
		if (!(dt > 0.0f)) return;
		dt = (std::min)(dt, 0.1f);
		aspect = (std::max)(0.5f, (std::min)(aspect, 4.0f));
		Spawn(dt, aspect, m, p);
		Move(dt, aspect, m, p);
		Merge();
		Cull(aspect);
	}

	// The speed a drop is pushed at from rest this step, frame heights per second squared (for the tests).
	static void Push(const Drop& d, float aspect, const Motion& m, const Params& p, float out[2]) {
		const float slide = (std::max)(0.0f, p.slide);
		// Gravity on the glass, only for drops past the critical size.
		const float heavy = Smooth(kCriticalRadius * 0.7f, kCriticalRadius * 1.6f, d.r);
		float ax = m.gravity[0] * 1.6f * heavy, ay = m.gravity[1] * 1.6f * heavy;
		// The air: sideways and vertical motion blows across the frame (camera right -> drops left,
		// camera up -> drops down), forward motion out from the middle.
		auto sq = [](float v) { const float s = v / kRunSpeed; return s * std::fabs(s); };
		ax += -sq(m.right) * 1.8f;
		ay += sq(m.up) * 1.8f;
		const float fwd = (std::max)(0.0f, m.forward);
		if (fwd > 0) {
			const float k = (fwd / kRunSpeed) * (fwd / kRunSpeed) * 1.4f;
			ax += (d.x - aspect * 0.5f) * 2.0f * k;
			ay += (d.y - 0.5f) * 2.0f * k;
		}
		// The turn: panning left (yaw > 0) swings the glass left, so the drops lag to the right.
		ax += m.yawRate / 360.0f * 1.2f;
		ay += -m.pitchRate / 360.0f * 1.2f;
		out[0] = ax * slide;
		out[1] = ay * slide;
	}

private:
	static float Smooth(float a, float b, float x) {
		const float t = (std::max)(0.0f, (std::min)(1.0f, (x - a) / (b - a)));
		return t * t * (3 - 2 * t);
	}

	float Rand() { // xorshift32, 0..1
		m_rng ^= m_rng << 13; m_rng ^= m_rng >> 17; m_rng ^= m_rng << 5;
		return (float)(m_rng >> 8) * (1.0f / 16777216.0f);
	}

	void Add(float x, float y, float r, float vx, float vy) {
		if ((int)m_drops.size() >= kMaxDrops) {
			// Full: the smallest drop gives up its place.
			auto smallest = std::min_element(m_drops.begin(), m_drops.end(), [](const Drop& a, const Drop& b) { return a.r < b.r; });
			if (smallest == m_drops.end() || smallest->r >= r) return;
			m_drops.erase(smallest);
		}
		Drop d;
		d.x = x; d.y = y; d.r = r; d.vx = vx; d.vy = vy; d.seed = Rand();
		m_drops.push_back(d);
	}

	void Spawn(float dt, float aspect, const Motion& m, const Params& p) {
		const float amount = (std::max)(0.0f, p.amount) / 100.0f;
		if (amount <= 0) return;
		const float rush = 1.0f + (std::max)(0.0f, p.impact) * (std::max)(0.0f, m.forward) / kRunSpeed;
		m_spawn += amount * 36.0f * rush * dt; // drops per second at full rain, standing still
		while (m_spawn >= 1.0f) {
			m_spawn -= 1.0f;
			const float u = Rand();
			// Most drops are small; a few are big enough to run at once.
			const float r = (0.0015f + 0.0055f * u * u * u + (Rand() < 0.06f ? 0.0035f : 0.0f)) * (std::max)(0.3f, (std::min)(p.size, 3.0f));
			Add(Rand() * aspect, Rand(), r, 0, 0);
		}
	}

	void Move(float dt, float aspect, const Motion& m, const Params& p) {
		const size_t n = m_drops.size();
		for (size_t i = 0; i < n; ++i) {
			Drop& d = m_drops[i];
			d.age += dt;
			float a[2];
			Push(d, aspect, m, p, a);
			const float force = std::sqrt(a[0] * a[0] + a[1] * a[1]);
			const float speed = std::sqrt(d.vx * d.vx + d.vy * d.vy);
			// Grip: a small drop holds on harder (its weight shrinks with r^3, the edge it sticks by with r).
			const float grip = 0.55f * (kCriticalRadius / (std::max)(d.r, 1e-4f));
			if (speed < 0.01f && force < grip) {
				d.vx = d.vy = 0;
			} else {
				d.vx += a[0] * dt;
				d.vy += a[1] * dt;
				const float drag = std::exp(-3.0f * dt);
				d.vx *= drag; d.vy *= drag;
			}
			const float moved = std::sqrt(d.vx * d.vx + d.vy * d.vy) * dt;
			d.x += d.vx * dt;
			d.y += d.vy * dt;
			// A sliding drop leaves water behind: small drops along its path, paid for from its own volume.
			if (moved > 0 && p.trails > 0 && d.r > kMinRadius * 2.5f) {
				const float chance = p.trails * moved / (d.r * 1.5f);
				if (Rand() < chance) {
					const float rt = d.r * (0.22f + 0.18f * Rand());
					const float remain = d.r * d.r * d.r - rt * rt * rt;
					if (remain > 0) {
						const float bx = d.x - d.vx * dt * 1.5f, by = d.y - d.vy * dt * 1.5f;
						d.r = std::cbrt(remain);
						Add(bx, by, rt, 0, 0);
					}
				}
			}
			// A still drop slowly dries; the smallest go first.
			if (speed < 0.01f) d.r -= dt * 0.00005f * (kCriticalRadius / (std::max)(d.r, 1e-4f));
		}
	}

	void Merge() {
		for (size_t i = 0; i < m_drops.size(); ++i) {
			for (size_t j = i + 1; j < m_drops.size();) {
				Drop& a = m_drops[i];
				const Drop& b = m_drops[j];
				const float dx = a.x - b.x, dy = a.y - b.y, reach = (a.r + b.r) * 0.85f;
				if (dx * dx + dy * dy < reach * reach) {
					const float va = a.r * a.r * a.r, vb = b.r * b.r * b.r, v = va + vb;
					a.x = (a.x * va + b.x * vb) / v;
					a.y = (a.y * va + b.y * vb) / v;
					a.vx = (a.vx * va + b.vx * vb) / v;
					a.vy = (a.vy * va + b.vy * vb) / v;
					a.r = std::cbrt(v);
					a.age = (std::min)(a.age, b.age);
					m_drops.erase(m_drops.begin() + (std::ptrdiff_t)j);
				} else {
					++j;
				}
			}
		}
	}

	void Cull(float aspect) {
		m_drops.erase(std::remove_if(m_drops.begin(), m_drops.end(), [&](const Drop& d) {
			return d.r < kMinRadius || d.x < -d.r * 2 || d.x > aspect + d.r * 2 || d.y < -d.r * 2 || d.y > 1 + d.r * 2;
		}), m_drops.end());
	}

	std::vector<Drop> m_drops;
	uint32_t m_rng = 1;
	float m_spawn = 0;
};

} // namespace Filmmaker::LensRain

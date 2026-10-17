#pragma once

// How a camera-path shake changes between its keys, so a shot can start with a slow, gentle shake
// and build up without a jolt.
//
// A run is consecutive camera keys carrying the same shake style. Inside a run, intensity and
// speed glide from one key's value to the next along an eased ramp (smoothstep: no sudden change
// of rate at either key). Speed is a time scale (50 = the style's authored speed), and the shake
// is a function of its OWN clock. That clock must be the integral of the speed over time, not
// "time since the run started x current speed": with the product, raising the speed four seconds
// into a run sweeps the oscillator through extra cycles while the ramp runs, which is exactly the
// jarring rush this avoids. Integrated, a speed change only bends the clock's rate.
//
// Engine-free and header-only; Automation/tests/rage-shake-math-tests.cpp pins it, and the
// timeline (GraphEditorJs.h) draws the same eased ramp between Intensity / Speed markers.

namespace Filmmaker {
namespace ShakeRamp {

inline double Clamp01(double u) { return u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u); }
// Ease of a ramp at fraction u of its segment.
inline double Ease(double u) { u = Clamp01(u); return u * u * (3.0 - 2.0 * u); }
// The value at fraction u between a key's value a and the next key's b.
inline double Value(double a, double b, double u) { return a + (b - a) * Ease(u); }
// Integral of Value(a, b, x) for x in [0, u], in units of the whole segment.
inline double Integral(double a, double b, double u) {
	u = Clamp01(u);
	const double u3 = u * u * u;
	return a * u + (b - a) * (u3 - 0.5 * u3 * u);
}

// One camera key as the shake clock sees it: its tick, its speed, and whether the run carries on
// to the next key (the next key carries the same style), so the speed ramps toward it.
struct ClockKey {
	double tick = 0.0;
	double speed = 50.0;
	bool rampsToNext = false;
};

// The shake clock in seconds at lookupTick, for the run that starts at keys[first]: the speed's
// integral from that key on, 50 = real time. Past the run's last key (rampsToNext false) the
// speed holds, which is also how an outgoing shake keeps running through a style handover.
inline double ClockSeconds(const ClockKey* keys, int n, int first, double lookupTick, double secondsPerTick) {
	double out = 0.0;
	for (int i = first; i >= 0 && i < n && keys[i].tick <= lookupTick; ++i) {
		const double a = keys[i].speed / 50.0;
		const bool ramp = keys[i].rampsToNext && i + 1 < n && keys[i + 1].tick > keys[i].tick;
		if (ramp) {
			const double span = keys[i + 1].tick - keys[i].tick, b = keys[i + 1].speed / 50.0;
			if (keys[i + 1].tick <= lookupTick) { out += span * secondsPerTick * 0.5 * (a + b); continue; }
			out += span * secondsPerTick * Integral(a, b, (lookupTick - keys[i].tick) / span);
		} else {
			out += (lookupTick - keys[i].tick) * secondsPerTick * a;
		}
		break;
	}
	return out;
}

} // namespace ShakeRamp
} // namespace Filmmaker

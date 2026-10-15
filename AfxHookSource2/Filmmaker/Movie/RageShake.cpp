#include "RageShake.h"

#include <cmath>

namespace Filmmaker {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRadToDeg = 57.295779513082320876;
// Shake translation is authored in METRES. Source/CS2 world units are inches.
constexpr double kMetresToUnits = 39.3700787401575;

// A randomly seeded `phase < 0` would shake the same shot differently every viewing.
// A rendered movie has to be reproducible, so a negative phase resolves here instead -- to an
// irrational-stride offset per component index, which decorrelates the channels (the whole point
// of the random seed) without ever changing between runs.
double ResolvePhase(double authored, int componentIndex) {
	if (authored >= 0.0)
		return authored;
	const double golden = 0.61803398874989484820;
	double p = (componentIndex + 1) * golden;
	return p - std::floor(p);
}

// --- envelope curve types ---
// Each returns a 0->1 ramp. The release phase uses (1 - curve(x)); see ReleaseResponse below.

double SlowIn(double x) { return x * x; }
double SlowOut(double x) { return x * (2.0 - x); }
double SlowInOut(double x) { return x * x * (3.0 - 2.0 * x); }

double CurveRamp(RageCurve c, double x) {
	if (x <= 0.0) return 0.0;
	if (x >= 1.0) return 1.0;
	switch (c) {
	case RageCurve::SlowIn:                  return SlowIn(x);
	case RageCurve::SlowOut:                 return SlowOut(x);
	case RageCurve::SlowInOut:               return SlowInOut(x);
	case RageCurve::VerySlowIn:              return SlowIn(SlowIn(x));
	case RageCurve::VerySlowOut:             return SlowOut(SlowOut(x));
	case RageCurve::VerySlowInSlowOut:       return SlowIn(SlowInOut(x));
	case RageCurve::SlowInVerySlowOut:       return SlowOut(SlowInOut(x));
	case RageCurve::VerySlowInVerySlowOut:   return SlowInOut(SlowInOut(x));
	case RageCurve::QuadraticEaseIn:         return x * x;
	case RageCurve::QuadraticEaseOut:        return 1.0 - (1.0 - x) * (1.0 - x);
	case RageCurve::QuadraticEaseInOut:
		return (x < 0.5) ? (2.0 * x * x) : (1.0 - 2.0 * (1.0 - x) * (1.0 - x));
	case RageCurve::CubicEaseIn:             return x * x * x;
	case RageCurve::CubicEaseOut: {
		const double u = 1.0 - x; return 1.0 - u * u * u;
	}
	case RageCurve::CubicEaseInOut: {
		if (x < 0.5) return 4.0 * x * x * x;
		const double u = 1.0 - x; return 1.0 - 4.0 * u * u * u;
	}
	case RageCurve::QuarticEaseIn:           return x * x * x * x;
	case RageCurve::QuarticEaseOut: {
		const double u = 1.0 - x; return 1.0 - u * u * u * u;
	}
	case RageCurve::QuarticEaseInOut: {
		if (x < 0.5) return 8.0 * x * x * x * x;
		const double u = 1.0 - x; return 1.0 - 8.0 * u * u * u * u;
	}
	case RageCurve::CircularEaseIn:          return 1.0 - std::sqrt(1.0 - x * x);
	case RageCurve::CircularEaseOut: {
		const double u = x - 1.0; return std::sqrt(1.0 - u * u);
	}
	case RageCurve::CircularEaseInOut: {
		if (x < 0.5) return 0.5 * (1.0 - std::sqrt(1.0 - 4.0 * x * x));
		const double u = -2.0 * x + 2.0;
		return 0.5 * (std::sqrt(1.0 - u * u) + 1.0);
	}
	case RageCurve::Exponential:
		// Only ever used as a RELEASE curve in RAGE, where the response is exp(-7x) directly
		// rather than 1 - ramp(x). ReleaseResponse special-cases it; as an ATTACK curve RAGE
		// falls through to `default: baseLevel = phaseProgress`, i.e. linear.
		return x;
	case RageCurve::Linear:
	default:
		return x;
	}
}

// Release response -- note EXPONENTIAL is not 1-ramp(x).
double ReleaseResponse(RageCurve c, double x) {
	if (c == RageCurve::Exponential)
		return std::exp(-7.0 * x); // below 0.001 by x = 1
	return 1.0 - CurveRamp(c, x);
}

// The ADSHR phase machine, evaluated at an absolute time instead of stepped. Attack targets 1.0
// only when a decay phase exists. `finished` reports past the end of a non-looping release.
double EnvelopeLevel(const RageEnvelope& e, double tSec, bool* finished) {
	if (finished) *finished = false;
	if (tSec < 0.0) return 0.0;

	double t = tSec * 1000.0; // RAGE envelope durations are milliseconds

	// PREDELAY
	if (e.preDelayMs > 0) {
		if (t < (double)e.preDelayMs) return 0.0;
		t -= (double)e.preDelayMs;
	}

	// ATTACK
	if (e.attackMs > 0) {
		if (t < (double)e.attackMs) {
			const double p = t / (double)e.attackMs;
			const double target = (e.decayMs > 0) ? 1.0 : e.sustainLevel;
			return CurveRamp(e.attackCurve, p) * target;
		}
		t -= (double)e.attackMs;
	}

	// DECAY: lerp 1.0 -> sustainLevel
	if (e.decayMs > 0) {
		if (t < (double)e.decayMs) {
			const double p = t / (double)e.decayMs;
			return 1.0 + (e.sustainLevel - 1.0) * p;
		}
		t -= (double)e.decayMs;
	}

	// HOLD (negative = infinite: the looping case)
	if (e.holdMs < 0)
		return e.sustainLevel;
	if (e.holdMs > 0) {
		if (t < (double)e.holdMs) return e.sustainLevel;
		t -= (double)e.holdMs;
	}

	// RELEASE, from whatever level hold was sitting at.
	if (e.releaseMs > 0) {
		if (t < (double)e.releaseMs) {
			const double p = t / (double)e.releaseMs;
			return ReleaseResponse(e.releaseCurve, p) * e.sustainLevel;
		}
	}

	if (finished) *finished = true;
	return 0.0;
}

// Oscillator, closed form. See the header for why this is analytic. `tSec` has already
// had the time scale applied by the caller, so there is no separate frequency scale here -- that
// is the composition we want (the scale lands on the timestep, upstream of everything).
double OscillatorLevel(const RageOscillator& o, double tSec, int componentIndex) {
	const double phase = ResolvePhase(o.phase, componentIndex);
	const double angle = 2.0 * kPi * (o.frequency * tSec + phase);

	double level = 0.0;
	switch (o.waveform) {
	case RageWaveform::DampedSine:
		if (o.frequency > 1e-6) {
			// The decay sweep comes from timestep/frequency -- a SLOWER oscillator decays
			// over a LONGER window, including the sign of the mapping:
			// fDecay rides [0,1] and is fed through exp(-decay * fDecay), so the envelope of the
			// burst breathes rather than falling monotonically.
			//
			// NOTE the practical consequence, because it bit this module once: at t=0 the sweep
			// sits at d=0.5, so the burst STARTS at exp(-decay/2) of its nominal amplitude. With a
			// large `decay` that is a near-total mute on the very frames a one-shot is supposed to
			// hit hardest. Keep authored decay values small and let the envelope's release do the
			// decaying, which is what the shipped styles here do.
			const double decayAngle = 2.0 * kPi * (tSec / o.frequency);
			const double d = std::sin(decayAngle) * 0.5 + 0.5;
			level = o.amplitude * std::exp(-o.decay * d) * std::sin(angle);
		}
		break;
	case RageWaveform::Sine:
	default:
		level = o.amplitude * std::sin(angle);
		break;
	}
	return level + o.dcOffset;
}

void ApplyToPose(RageShakePose& pose, RageShakeComp comp, double v) {
	switch (comp) {
	case RageShakeComp::WorldPosX: pose.worldX += v * kMetresToUnits; break;
	case RageShakeComp::WorldPosY: pose.worldY += v * kMetresToUnits; break;
	case RageShakeComp::WorldPosZ: pose.worldZ += v * kMetresToUnits; break;
	case RageShakeComp::RelPosX:   pose.right += v * kMetresToUnits; break;
	case RageShakeComp::RelPosY:   pose.fwd += v * kMetresToUnits; break;
	case RageShakeComp::RelPosZ:   pose.up += v * kMetresToUnits; break;
		// RAGE's camera X axis points right and a positive RotateLocalX pitches the view UP.
		// Source view angles use the opposite sign convention (positive pitch looks DOWN), so
		// this one channel is negated; yaw and roll share their sign with RAGE.
	case RageShakeComp::RotX:      pose.pitch += -v * kRadToDeg; break;
	case RageShakeComp::RotY:      pose.roll += v * kRadToDeg; break;
	case RageShakeComp::RotZ:      pose.yaw += v * kRadToDeg; break;
	case RageShakeComp::Fov:       pose.fov += v; break;
	default: break;
	}
}

} // namespace

bool RageShakePose::Any() const {
	return worldX != 0.0 || worldY != 0.0 || worldZ != 0.0
		|| right != 0.0 || fwd != 0.0 || up != 0.0
		|| pitch != 0.0 || yaw != 0.0 || roll != 0.0 || fov != 0.0;
}

void RageShakePose::AddScaled(const RageShakePose& o, double k) {
	worldX += o.worldX * k; worldY += o.worldY * k; worldZ += o.worldZ * k;
	right += o.right * k; fwd += o.fwd * k; up += o.up * k;
	pitch += o.pitch * k; yaw += o.yaw * k; roll += o.roll * k;
	fov += o.fov * k;
}

RageShakePose RageShake_Eval(const RageShakeDef& def, double tSec, double amplitude, double timeScale) {
	RageShakePose pose;
	if (!def.components || def.componentCount <= 0 || amplitude == 0.0)
		return pose;
	if (tSec < 0.0)
		return pose;
	if (timeScale <= 0.0)
		timeScale = 1.0;

	// The time scale is applied ONCE, here, to the shake's whole clock -- the oscillators and
	// every envelope then read the same warped time. Mirrors RAGE scaling the timestep upstream.
	const double t = tSec * timeScale;

	double overall = 1.0;
	if (def.hasOverallEnvelope) {
		bool done = false;
		overall = EnvelopeLevel(def.overallEnvelope, t, &done);
		if (done)
			return pose; // the shake stops once the overall envelope completes
	}

	for (int i = 0; i < def.componentCount; ++i) {
		const RageShakeComponent& c = def.components[i];
		double compEnv = 1.0;
		if (c.hasEnvelope) {
			bool done = false;
			compEnv = EnvelopeLevel(c.env, t, &done);
			if (done)
				continue; // this component's envelope has completed
		}
		// shakeValue = oscillator * componentEnvelope * overallEnvelope * amplitude
		const double v = OscillatorLevel(c.osc, t, i) * compEnv * overall * amplitude;
		if (v != 0.0)
			ApplyToPose(pose, c.component, v);
	}
	return pose;
}

bool RageShake_Finished(const RageShakeDef& def, double tSec, double timeScale) {
	if (!def.components || def.componentCount <= 0)
		return true;
	if (timeScale <= 0.0) timeScale = 1.0;
	const double t = tSec * timeScale;
	if (def.hasOverallEnvelope) {
		bool done = false;
		EnvelopeLevel(def.overallEnvelope, t, &done);
		if (done) return true;
	}
	for (int i = 0; i < def.componentCount; ++i) {
		const RageShakeComponent& c = def.components[i];
		if (!c.hasEnvelope)
			return false; // an un-enveloped component runs forever
		bool done = false;
		EnvelopeLevel(c.env, t, &done);
		if (!done) return false;
	}
	return true;
}

} // namespace Filmmaker

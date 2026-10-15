#include "HandheldCam.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Active / MvmDebugLog_Linef
#include "ViewFxVm.h"

#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

constexpr double kPi = 3.14159265358979323846;

double Clamp(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
double ClampIntensity(double p) { return Clamp(p, 0.0, kShakeMaxIntensity); }
double Lerp(double frac, double from, double to) { return from + (to - from) * frac; }

double Smoothstep(double t) {
	t = Clamp(t, 0.0, 1.0);
	return t * t * (3.0 - 2.0 * t);
}

// Frame-rate-independent smoothing.
//
// Both source addons smooth with a FIXED per-frame alpha (`Lerp(0.1, a, b)`), which silently
// ties the effect's character to the frame rate -- fatal for a movie tool, where the same demo
// gets rendered at 60 and at 300 fps and must look identical. Every such constant is converted
// to a time constant tau via `tau = -(1/60) / ln(1 - alpha)`, i.e. "the alpha the original had
// at 60 Hz", and applied as an exponential decay instead. At exactly 60 fps this reproduces the
// original numbers; at any other rate it reproduces the original *look*.
double SmoothAlpha(double dt, double tau) {
	if (dt <= 0.0) return 0.0;
	if (tau <= 0.0) return 1.0;
	return 1.0 - std::exp(-dt / tau);
}
constexpr double kTauA10 = 0.158188; // original Lerp(0.10, ...)
constexpr double kTauA15 = 0.102552; // original Lerp(0.15, ...)
constexpr double kTauA08 = 0.199884; // original Lerp(0.08, ...) -- FOV wobble
constexpr double kTauTilt = 0.25;    // original Lerp(clamp(dt*4), ...)
constexpr double kTauCurveLowPass = 0.12; // Shakify's SMOOTH_TIME, already dt-correct upstream

// ---- DRIFT layer reference tuning (camcorder_shake.lua) ------------------------------------
//
// The source addon's raw amplitudes are enormous for CS2 footage -- 5.5 degrees of slow drift
// reads as seasickness, not as handheld. Ported verbatim, the slider's only useful range would
// be its bottom fifth, which makes for a bad knob. So 100% maps to this fraction of the raw
// tuning: a strong-but-usable handheld look, with 150% approaching the addon's raw feel.
constexpr double kDriftReferenceScale = 0.35;

// Seeded when the master switch is turned on and both layers are still at 0 (see SetEnabled).
// Deliberately modest: the curve layer carries the real handheld character, drift is the
// procedural top-up, and a first-time user should get something filmable rather than something
// seasick.
constexpr double kDefaultDriftPct = 35.0;
constexpr double kDefaultCurvePct = 75.0;

// Jitter is re-rolled at a FIXED rate rather than once per rendered frame (the original rolls
// per frame, so it noticeably changes character with frame rate). 60 Hz reproduces the
// original's cadence.
constexpr double kJitterResampleHz = 60.0;

// Walk-bob gate + running threshold, in units/sec (the addon's own numbers).
constexpr double kDriftWalkMinSpeed = 5.0;
constexpr double kDriftRunSpeed = 180.0;

// ---- CURVE layer state thresholds ----------------------------------------------------------
//
// Retuned for CS2, which is much slower than GMod: a rifle run is ~215 u/s and a shift-walk
// ~130 u/s, where the addon assumed 200 walk / 400 run. Below idle the camera is treated as
// standing still.
constexpr double kStateIdleSpeed = 20.0;
constexpr double kStateRunSpeed = 140.0;
constexpr double kStateDebounce = 0.12;   // seconds a new state must hold before switching
constexpr double kStateFade = 0.5;        // crossfade seconds

// Re-grip: which curves are eligible to be stolen from for a fumble, matching the source
// addon's REGRIP_DEFAULT_ON set (the handheld-looking ones, not the vehicle/spaceship ones).
const char* const kRegripCurves[] = { "THE_CLOSEUP", "THE_WEDDING", "OUT_CAR_WINDOW", "INVESTIGATION" };
constexpr int kRegripCurveCount = 4;

int StateIndex(HandheldState s) { return (int)s; }

} // namespace

int HandheldCurveIndex(const char* key) {
	if (!key || !key[0]) return -1;
	for (int i = 0; i < g_handheldCurveCount; ++i)
		if (0 == _stricmp(g_handheldCurves[i].key, key)) return i;
	return -1;
}

HandheldCam::HandheldCam() {
	// Same state->curve mapping the source addon ships: a slow investigative handheld while
	// standing, a walking rig while walking, a jogging rig while running.
	m_stateCurve[StateIndex(HandheldState::Idle)] = HandheldCurveIndex("INVESTIGATION");
	m_stateCurve[StateIndex(HandheldState::Walk)] = HandheldCurveIndex("WALK_TO_THE_STORE");
	m_stateCurve[StateIndex(HandheldState::Run)] = HandheldCurveIndex("HANDYCAM_RUN");
	m_rngState = 0x9E3779B97F4A7C15ull;
}

HandheldCam& HandheldCamRef() {
	static HandheldCam s_instance;
	return s_instance;
}

// ============================== settings ===========================================

// Mirrors the member initialisers in HandheldCam.h -- keep the two in step.
void HandheldCam::ResetSettings() {
	m_enabled = false;
	m_viewmodelDecoupled = false;
	m_centerLockEnabled = false;
	m_centerLockPct = 100.0;
	m_driftPct = 0.0;
	m_driftDirDeg = 0.0;
	m_curvePct = 0.0;
	m_tiltPct = 100.0;
	m_regripPct = 100.0;
	m_speedPct = 100.0;
	m_variancePct = 40.0;
	m_regripPeriod = 9.5;
	m_stateCurve[StateIndex(HandheldState::Idle)] = HandheldCurveIndex("INVESTIGATION");
	m_stateCurve[StateIndex(HandheldState::Walk)] = HandheldCurveIndex("WALK_TO_THE_STORE");
	m_stateCurve[StateIndex(HandheldState::Run)] = HandheldCurveIndex("HANDYCAM_RUN");
	Reset(); // drop the integrated motion so the camera does not keep the old wobble
}

bool HandheldCam::SettingsAreDefault() const {
	return !m_enabled && !m_viewmodelDecoupled && !m_centerLockEnabled
		&& std::fabs(m_centerLockPct - 100.0) <= 0.000001
		&& std::fabs(m_driftPct) <= 0.000001 && std::fabs(m_driftDirDeg) <= 0.000001
		&& std::fabs(m_curvePct) <= 0.000001 && std::fabs(m_tiltPct - 100.0) <= 0.000001
		&& std::fabs(m_regripPct - 100.0) <= 0.000001 && std::fabs(m_speedPct - 100.0) <= 0.000001
		&& std::fabs(m_variancePct - 40.0) <= 0.000001 && std::fabs(m_regripPeriod - 9.5) <= 0.000001
		&& m_stateCurve[StateIndex(HandheldState::Idle)] == HandheldCurveIndex("INVESTIGATION")
		&& m_stateCurve[StateIndex(HandheldState::Walk)] == HandheldCurveIndex("WALK_TO_THE_STORE")
		&& m_stateCurve[StateIndex(HandheldState::Run)] == HandheldCurveIndex("HANDYCAM_RUN");
}

void HandheldCam::SetEnabled(bool on) {
	if (on == m_enabled) return;
	m_enabled = on;
	// Turning the master switch on while BOTH layers sit at 0% produces no visible motion at
	// all (only the ambient tilt), which reads as "the toggle is broken". Seed a usable
	// starting point in that case; any non-zero layer means the user has already dialled it in
	// and we leave their values alone.
	if (on && m_driftPct <= 0.0 && m_curvePct <= 0.0) {
		m_driftPct = kDefaultDriftPct;
		m_curvePct = kDefaultCurvePct;
	}
	Reset();
}

void HandheldCam::SetViewmodelDecoupled(bool on) {
	m_viewmodelDecoupled = on;
	// A pattern miss degrades to nothing rather than crashing; say so instead of leaving the
	// toggle silently inert (the per-frame compensation path also retries the install).
	if (on && !ViewFxVm_EnsureCameraCompensationHook())
		advancedfx::Warning(
			"mirv_filmmaker: handheld viewmodel decoupling could not install the viewmodel hook "
			"(CalcViewModelView caller not resolved) -- it will retry while enabled.\n");
}

void HandheldCam::SetCenterLockStrength(double percent) {
	if (percent < 0.0) percent = 0.0;
	else if (percent > 100.0) percent = 100.0;
	m_centerLockPct = percent;
}

void HandheldCam::SetDriftIntensity(double p) { m_driftPct = ClampIntensity(p); }
void HandheldCam::SetDriftDirection(double deg) {
	deg = std::fmod(deg, 360.0);
	if (deg < 0.0) deg += 360.0;
	m_driftDirDeg = deg;
}
void HandheldCam::SetCurveIntensity(double p) { m_curvePct = ClampIntensity(p); }
void HandheldCam::SetTiltIntensity(double p) { m_tiltPct = ClampIntensity(p); }
void HandheldCam::SetRegripIntensity(double p) { m_regripPct = ClampIntensity(p); }
void HandheldCam::SetSpeed(double p) { m_speedPct = Clamp(p, 25.0, 300.0); }
void HandheldCam::SetVariance(double p) { m_variancePct = Clamp(p, 0.0, 100.0); }
void HandheldCam::SetRegripPeriod(double s) { m_regripPeriod = Clamp(s, 2.0, 60.0); }

bool HandheldCam::SetStateCurve(HandheldState state, const char* key) {
	const int idx = HandheldCurveIndex(key);
	if (idx < 0) return false;
	const int slot = StateIndex(state);
	if (m_stateCurve[slot] == idx) return true;
	m_stateCurve[slot] = idx;
	// Force the auto-switcher to respawn the layer with the new curve on the next frame.
	m_haveState = false;
	return true;
}

const char* HandheldCam::StateCurveKey(HandheldState state) const {
	const int idx = m_stateCurve[StateIndex(state)];
	if (idx < 0 || idx >= g_handheldCurveCount) return "";
	return g_handheldCurves[idx].key;
}

// ============================== deterministic RNG ==================================

// xorshift64*. Seeded from a fixed constant on Reset() and advanced only on accepted samples,
// so replaying the same demo segment from the same seek point reproduces the same shake -- an
// exported movie must not differ between takes.
double HandheldCam::Rand01() {
	if (m_rngState == 0) m_rngState = 0x9E3779B97F4A7C15ull;
	m_rngState ^= m_rngState >> 12;
	m_rngState ^= m_rngState << 25;
	m_rngState ^= m_rngState >> 27;
	const unsigned long long v = m_rngState * 0x2545F4914F6CDD1Dull;
	return (double)(v >> 11) * (1.0 / 9007199254740992.0); // 53-bit mantissa -> [0,1)
}

double HandheldCam::RandRange(double lo, double hi) { return lo + (hi - lo) * Rand01(); }

// ============================== layer pool =========================================

int HandheldCam::AddLayer(int curve, double now, double influence, double scale, double rotScale,
                          double speed, double offset, double fadeIn) {
	if (curve < 0 || curve >= g_handheldCurveCount) return -1;
	int slot = -1;
	for (int i = 0; i < kMaxLayers; ++i) {
		if (!m_layers[i].active) { slot = i; break; }
	}
	if (slot < 0) return -1; // pool exhausted: drop the request rather than stomp a live layer

	Layer& L = m_layers[slot];
	L = Layer();
	L.active = true;
	L.curve = curve;
	L.startTime = now - offset;
	L.influence = influence;
	L.scale = scale;
	L.rotScale = rotScale;
	L.speed = speed;
	L.fadeIn = fadeIn;
	L.varSeed1 = RandRange(0.0, 1000.0);
	L.varSeed2 = RandRange(0.0, 1000.0);
	L.varFreq1 = RandRange(0.05, 0.11);
	L.varFreq2 = RandRange(0.13, 0.21);
	return slot;
}

void HandheldCam::RemoveLayer(int slot, double now, double fadeSeconds) {
	if (slot < 0 || slot >= kMaxLayers) return;
	Layer& L = m_layers[slot];
	if (!L.active) return;
	if (fadeSeconds > 0.0) {
		L.fadeOutAt = now;
		L.fadeOutDur = fadeSeconds;
		L.fadeOutFrom = L.influence;
		L.removeAt = now + fadeSeconds;
	} else {
		L.active = false;
	}
}

// ============================== curve sampling =====================================

ShakePose HandheldCam::SampleCurves(const ShakeInput& in) {
	ShakePose out;
	const double now = in.demoTime;
	const double varAmt = m_variancePct / 100.0;

	for (int i = 0; i < kMaxLayers; ++i) {
		Layer& L = m_layers[i];
		if (!L.active) continue;
		if (L.removeAt >= 0.0 && now >= L.removeAt) { L.active = false; continue; }

		double infl = L.influence;
		if (L.fadeOutAt >= 0.0 && L.fadeOutDur > 0.0) {
			infl = Lerp(Smoothstep((now - L.fadeOutAt) / L.fadeOutDur), L.fadeOutFrom, 0.0);
		} else if (L.fadeIn > 0.0) {
			infl *= Smoothstep((now - L.startTime) / L.fadeIn);
		}
		if (infl <= 0.0) continue;

		// Loop-to-loop drift: two slow incommensurate sines per layer nudge playback speed and
		// amplitude, so a 5-second curve stops reading as a 5-second loop.
		const double speedDrift = 1.0 + varAmt * 0.18 * (
			std::sin(now * L.varFreq1 + L.varSeed1) * 0.6 +
			std::sin(now * L.varFreq2 + L.varSeed2) * 0.4);
		const double scaleDrift = 1.0 + varAmt * 0.12 *
			std::sin(now * L.varFreq2 * 0.7 + L.varSeed1 * 1.7);

		const HandheldCurve& c = g_handheldCurves[L.curve];
		const double t = (now - L.startTime) * L.speed * speedDrift;

		double frame = std::fmod(t * (double)c.fps, (double)c.frames);
		if (frame < 0.0) frame += (double)c.frames;
		const int i0 = (int)std::floor(frame);
		const int i1 = (i0 + 1) % c.frames;
		const double f = frame - (double)i0;
		const float* a = c.data + (size_t)i0 * 6;
		const float* b = c.data + (size_t)i1 * 6;

		const double posScale = L.scale * scaleDrift * infl;
		const double rotScale = L.rotScale * infl;
		out.right += Lerp(f, a[0], b[0]) * posScale;
		out.up    += Lerp(f, a[1], b[1]) * posScale;
		out.fwd   += Lerp(f, a[2], b[2]) * posScale;
		out.pitch += Lerp(f, a[3], b[3]) * rotScale;
		out.yaw   += Lerp(f, a[4], b[4]) * rotScale;
		out.roll  += Lerp(f, a[5], b[5]) * rotScale;
	}
	return out;
}

// ============================== state machine ======================================

void HandheldCam::UpdateStateMachine(const ShakeInput& in) {
	const double now = in.demoTime;

	if (m_curvePct <= 0.0) {
		if (m_stateSlot >= 0) { RemoveLayer(m_stateSlot, now, kStateFade); m_stateSlot = -1; }
		m_haveState = false;
		return;
	}

	HandheldState wanted = HandheldState::Idle;
	if (in.speed >= kStateRunSpeed) wanted = HandheldState::Run;
	else if (in.speed >= kStateIdleSpeed) wanted = HandheldState::Walk;

	if (!m_haveState || wanted != m_state) {
		// Debounce: brushing past a threshold must not restart the curve.
		if (m_haveState && wanted != m_pendingState) {
			m_pendingState = wanted;
			m_pendingSince = now;
			return;
		}
		if (m_haveState && (now - m_pendingSince) < kStateDebounce) return;

		if (m_stateSlot >= 0) RemoveLayer(m_stateSlot, now, kStateFade);
		m_state = wanted;
		m_haveState = true;
		++m_dbgSwitches;

		const double infl = m_curvePct / 100.0;
		m_stateSlot = AddLayer(m_stateCurve[StateIndex(wanted)], now, infl, 1.0, 1.0,
		                       m_speedPct / 100.0, 0.0, kStateFade);
		return;
	}

	m_pendingState = wanted;
	// Live-follow the sliders without respawning the layer (respawning would restart the curve
	// mid-shot, which pops).
	if (m_stateSlot >= 0 && m_layers[m_stateSlot].active) {
		Layer& L = m_layers[m_stateSlot];
		L.speed = m_speedPct / 100.0;
		if (L.fadeOutAt < 0.0) L.influence = m_curvePct / 100.0;
	}
}

// ============================== re-grip ============================================

void HandheldCam::UpdateRegrip(const ShakeInput& in) {
	const double now = in.demoTime;

	if (m_regripPct <= 0.0) {
		if (m_regripSlot >= 0) { RemoveLayer(m_regripSlot, now, 0.3); m_regripSlot = -1; }
		m_nextRegrip = now + m_regripPeriod;
		return;
	}

	// Retire a finished fumble.
	if (m_regripSlot >= 0 && m_regripEndsAt >= 0.0 && now >= m_regripEndsAt) {
		RemoveLayer(m_regripSlot, now, 0.5);
		m_regripSlot = -1;
		m_regripEndsAt = -1.0;
	}

	if (now < m_nextRegrip) return;

	const double strength = m_regripPct / 100.0;
	const bool isLong = Rand01() < 0.3;
	const double duration = isLong ? RandRange(2.8, 4.2) : RandRange(0.9, 2.0);

	// Steal a random slice of a real captured curve and play it back fast, rather than
	// inventing new shake maths -- that is what makes a fumble read as human.
	const int pick = (int)(Rand01() * kRegripCurveCount);
	const int curve = HandheldCurveIndex(kRegripCurves[pick < kRegripCurveCount ? pick : 0]);
	if (curve >= 0) {
		const HandheldCurve& c = g_handheldCurves[curve];
		const double offset = RandRange(0.0, (double)c.frames / (double)c.fps);
		const double playSpeed = isLong ? RandRange(1.6, 2.2) : RandRange(2.4, 3.4);

		if (m_regripSlot >= 0) RemoveLayer(m_regripSlot, now, 0.2);
		m_regripSlot = AddLayer(curve, now, strength,
		                        RandRange(1.6, 2.4) * strength,
		                        RandRange(1.8, 2.8) * strength,
		                        playSpeed, offset, RandRange(0.25, 0.4));
		m_regripEndsAt = now + duration;
		++m_dbgRegrips;
	}

	// Fumbles bunch up while the operator is jogging to keep up.
	const double scale = m_regripPeriod / 9.5;
	double lo = 5.0 * scale, hi = 14.0 * scale;
	if (in.speed > 150.0) { lo *= 0.25; hi *= 0.25; }
	m_nextRegrip = now + RandRange(lo, hi);
}

// ============================== drift layer ========================================

ShakePose HandheldCam::SampleDrift(const ShakeInput& in) {
	ShakePose out;
	const double frac = (m_driftPct / 100.0) * kDriftReferenceScale;
	const double t = in.demoTime;

	// Slow wander -- the "operator isn't a tripod" term.
	double driftP = std::sin(t * 0.55) * 5.5 + std::sin(t * 0.25) * 1.2;
	double driftY = std::cos(t * 0.50) * 6.0 + std::cos(t * 0.30) * 1.5;
	const double driftR = std::sin(t * 0.40 + 1.3) * 3.2;
	// Positional wander. The source treats this as a world-axis offset; expressing it in the
	// camera's own basis instead keeps it stable as the camera turns (x->right, y->fwd, z->up).
	double driftPosR = std::sin(t * 0.45) * 3.8 + std::sin(t * 0.20) * 1.1;
	const double driftPosF = std::cos(t * 0.55 + 2.0) * 2.5 + std::cos(t * 0.30) * 0.7;
	double driftPosU = std::sin(t * 0.35 + 0.5) * 2.8;

	// Steer the lean. Pitch/yaw and right/up are each a 2-D pair in the operator's frame, so ONE
	// rotation applied to both keeps the angular and positional wander pointing the same way --
	// the camera leans and translates toward the same quarter of the frame, which is what a real
	// operator's posture does. Roll and forward are left alone: neither has a "side" to rotate
	// into, and rotating forward into right would turn a dolly into a slide.
	if (m_driftDirDeg != 0.0) {
		const double th = m_driftDirDeg * kPi / 180.0;
		const double cs = std::cos(th), sn = std::sin(th);
		const double p0 = driftP, y0 = driftY;
		driftP = p0 * cs - y0 * sn;
		driftY = p0 * sn + y0 * cs;
		const double r0 = driftPosR, u0 = driftPosU;
		driftPosR = r0 * cs - u0 * sn;
		driftPosU = r0 * sn + u0 * cs;
	}

	// Jitter: intensity re-rolled on its own 0.04-0.22 s cooldown, direction re-rolled at a
	// fixed 60 Hz (see kJitterResampleHz).
	m_jitterCooldown -= in.dt;
	if (m_jitterCooldown <= 0.0) {
		m_jitterIntensity = RandRange(0.5, 2.5);
		m_jitterCooldown = RandRange(0.04, 0.22);
	}
	const double ji = m_jitterIntensity;
	// FIXED-RATE re-roll, holding the sample in between.
	//
	// The previous loop ran `resamples` times and overwrote jitP/jitY/jitR on every pass, so
	// every iteration but the last was thrown away: it re-rolled exactly ONCE PER FRAME however
	// many times it spun. That is precisely the frame-rate dependence kJitterResampleHz exists
	// to remove -- at 300 fps the jitter changed direction five times more often than at 60, so
	// the effect's character moved with the frame rate (and with it, preview vs export).
	//
	// Now the clock accumulates real time and a new direction is taken only when it crosses the
	// 1/60 s step; between crossings the previous direction is HELD. The roll cap keeps a long
	// hitch from drawing an unbounded number of samples.
	m_jitterResampleClock += in.dt;
	const double kJitterStep = 1.0 / kJitterResampleHz;
	for (int rolls = 0; m_jitterResampleClock >= kJitterStep && rolls < 8; ++rolls) {
		m_jitterResampleClock -= kJitterStep;
		++m_dbgJitterRolls;
		m_jitterDirP = RandRange(-ji, ji);
		m_jitterDirY = RandRange(-ji, ji);
		m_jitterDirR = RandRange(-ji * 0.6, ji * 0.6);
	}
	if (m_jitterResampleClock > 1.0) m_jitterResampleClock = 0.0; // long stall: do not bank time
	const double jitP = m_jitterDirP, jitY = m_jitterDirY, jitR = m_jitterDirR;

	// High-frequency tremble -- muscle noise, not hand movement.
	const double tremP = (std::sin(t * 50.0) + std::sin(t * 70.0)) * 0.18;
	const double tremY = (std::cos(t * 60.0) + std::cos(t * 80.0)) * 0.18;
	const double tremR = (std::sin(t * 40.0 + 3.0) + std::cos(t * 55.0 + 1.0)) * 0.12;

	// Walk bob, scaled by the spectated player's own speed.
	double bobP = 0.0, bobY = 0.0, bobR = 0.0, bobRight = 0.0, bobUp = 0.0;
	if (in.speed > kDriftWalkMinSpeed) {
		const bool running = in.speed > kDriftRunSpeed;
		const double bobFreq = Clamp(in.speed / 200.0, 1.5, running ? 6.0 : 3.0);
		const double bt = t * bobFreq * kPi * 2.0 + m_walkPhaseOffset;
		const double vertAmp = running ? 10.0 : 6.5;
		const double sideAmp = running ? 5.0 : 3.8;
		const double pitchAmp = running ? 4.5 : 3.8;
		const double rollAmp = running ? 5.5 : 4.5;
		const double yawAmp = running ? 3.5 : 2.5;

		// The 0.25 factor is the source's `LerpVector(0.25, <zero>, target)` -- it lerps from a
		// freshly zeroed local every frame, so it only ever scales the target.
		bobUp = std::fabs(std::sin(bt) + std::sin(bt * 1.3)) * vertAmp * 0.9 * 0.25;
		bobRight = std::sin(bt * 2.0 + std::cos(t * 5.0)) * sideAmp * 0.25;
		bobP = std::sin(bt * 1.8) * pitchAmp * 0.25;
		bobR = std::cos(bt * 1.5) * rollAmp * 0.25;
		bobY = std::sin(bt * 1.2 + std::sin(t * 7.0)) * yawAmp * 0.25;
	}

	// Two-stage smoothing chain -- this lag is what separates "handheld" from "noise".
	const double a10 = SmoothAlpha(in.dt, kTauA10);
	const double a15 = SmoothAlpha(in.dt, kTauA15);
	m_tgtAngP = Lerp(a10, m_tgtAngP, bobP + driftP + jitP + tremP);
	m_tgtAngY = Lerp(a10, m_tgtAngY, bobY + driftY + jitY + tremY);
	m_tgtAngR = Lerp(a10, m_tgtAngR, bobR + driftR + jitR + tremR);
	m_tgtPosX = Lerp(a10, m_tgtPosX, bobRight + driftPosR);
	m_tgtPosY = Lerp(a10, m_tgtPosY, driftPosF);
	m_tgtPosZ = Lerp(a10, m_tgtPosZ, bobUp + driftPosU);

	m_outAngP = Lerp(a15, m_outAngP, m_tgtAngP);
	m_outAngY = Lerp(a15, m_outAngY, m_tgtAngY);
	m_outAngR = Lerp(a15, m_outAngR, m_tgtAngR);
	m_outPosX = Lerp(a15, m_outPosX, m_tgtPosX);
	m_outPosY = Lerp(a15, m_outPosY, m_tgtPosY);
	m_outPosZ = Lerp(a15, m_outPosZ, m_tgtPosZ);

	// FOV breathing. The source smooths the ABSOLUTE fov toward `fov + wobble`; smoothing the
	// delta instead is equivalent while the base FOV is steady and does not fight a real
	// FOV change (zoom, scope) when it isn't.
	const double wobble = std::sin(t * 1.8 + std::sin(t * 3.0)) * (0.7 + ji * 0.25);
	m_fovWobble = Lerp(SmoothAlpha(in.dt, kTauA08), m_fovWobble, wobble);

	return DriftPoseFromState();
}

ShakePose HandheldCam::DriftPoseFromState() const {
	ShakePose out;
	const double frac = (m_driftPct / 100.0) * kDriftReferenceScale;
	if (frac <= 0.0) return out;
	out.pitch = m_outAngP * frac;
	out.yaw = m_outAngY * frac;
	out.roll = m_outAngR * frac;
	out.right = m_outPosX * frac;
	out.fwd = m_outPosY * frac;
	out.up = m_outPosZ * frac;
	out.fov = m_fovWobble * frac;
	return out;
}

ShakePose HandheldCam::CurvePoseFromState() const {
	ShakePose out;
	out.right = m_smRight; out.up = m_smUp; out.fwd = m_smFwd;
	out.pitch = m_smPitch; out.yaw = m_smYaw; out.roll = m_smRoll;
	return out;
}

// ============================== per-frame ==========================================

ShakePose HandheldCam::Advance(const ShakeInput& in) {
	ShakePose out;
	if (!m_enabled) return out;

	// A fresh sample means the pose just jumped (seek, spectator switch, big gap). Re-anchor
	// everything and contribute nothing this frame -- a jump must never read as a shake spike.
	if (in.fresh) {
		Reset();
		m_walkPhaseOffset = RandRange(0.0, 10.0);
		m_tiltPhase = RandRange(0.0, 10.0);
		m_nextRegrip = in.demoTime + m_regripPeriod;
		return out;
	}
	if (in.dt <= 0.0) {
		// Same-tick render frame: hold the previous output rather than re-integrating, matching
		// ViewFx's hold behaviour.
		//
		// This is ALSO the staircase: the demo clock ticks at 64 Hz, so at any higher frame rate
		// most frames land here and reuse the previous offset. Counted (not changed) so the log
		// can say what the real ratio is before anyone decides whether to interpolate.
		++m_dbgHeldFrames;
		out = CurvePoseFromState();
		out.Add(DriftPoseFromState());
		return out;
	}

	++m_dbgSteps;
	++m_dbgWinSteps;
	m_dbgWinWall += in.wallDt;

	// One line per demo-second, never per frame (the recorder flushes per line). Carries the two
	// numbers the report actually needs: how many render frames HELD a stale offset for each one
	// that stepped it, and whether the jitter re-roll is running at its promised fixed rate
	// instead of once per frame. Everything else here is context for reading those two.
	if (MvmDebugLog_Active() && in.demoTime - m_dbgLastLogTime >= 1.0) {
		const double span = m_dbgWinWall;
		m_dbgLastLogTime = in.demoTime;
		MvmDebugLog_Linef("handheld.rate",
			"demoTime=%.2f dt=%.4f steps=%llu held=%llu heldPerStep=%.2f jitterRolls=%llu "
			"rollsPerSec=%.1f (target %.0f) speed=%.0f",
			in.demoTime, in.dt, m_dbgWinSteps, m_dbgHeldFrames,
			m_dbgWinSteps ? (double)m_dbgHeldFrames / (double)m_dbgWinSteps : 0.0,
			m_dbgJitterRolls, span > 0.0 ? (double)m_dbgJitterRolls / span : 0.0,
			kJitterResampleHz, in.speed);
		m_dbgWinSteps = 0; m_dbgHeldFrames = 0; m_dbgJitterRolls = 0; m_dbgWinWall = 0.0;
	}

	UpdateStateMachine(in);
	UpdateRegrip(in);

	// Ambient tilt: a slow lean, the thing that stops a locked camera reading as locked.
	{
		const double target = (m_tiltPct > 0.0)
			? std::sin(in.demoTime * 0.4 + m_tiltPhase + 1.3) * 3.2 * (m_tiltPct / 100.0)
			: 0.0;
		m_tiltRoll = Lerp(SmoothAlpha(in.dt, kTauTilt), m_tiltRoll, target);
	}

	ShakePose curves = SampleCurves(in);
	curves.roll += m_tiltRoll;

	// Final low-pass over the summed curve output. The source addon warns explicitly against
	// removing this even though it looks redundant with the per-layer fades: it is what kills
	// the pop when a re-grip or a state switch starts mid-curve at a large value.
	const double lp = SmoothAlpha(in.dt, kTauCurveLowPass);
	m_smRight = Lerp(lp, m_smRight, curves.right);
	m_smUp    = Lerp(lp, m_smUp,    curves.up);
	m_smFwd   = Lerp(lp, m_smFwd,   curves.fwd);
	m_smPitch = Lerp(lp, m_smPitch, curves.pitch);
	m_smYaw   = Lerp(lp, m_smYaw,   curves.yaw);
	m_smRoll  = Lerp(lp, m_smRoll,  curves.roll);

	out = CurvePoseFromState();
	if (m_driftPct > 0.0) out.Add(SampleDrift(in));
	return out;
}

void HandheldCam::Reset() {
	for (int i = 0; i < kMaxLayers; ++i) m_layers[i] = Layer();
	m_stateSlot = -1;
	m_regripSlot = -1;
	m_regripEndsAt = -1.0;
	m_haveState = false;
	m_state = HandheldState::Idle;
	m_pendingState = HandheldState::Idle;
	m_pendingSince = 0.0;
	m_tiltRoll = 0.0;
	m_nextRegrip = 0.0;

	m_jitterCooldown = 0.0;
	// Held jitter direction + its clock: a seek must re-anchor these like everything else, or the
	// first frame after the jump carries the direction from before it.
	m_jitterResampleClock = 0.0;
	m_jitterDirP = 0.0;
	m_jitterDirY = 0.0;
	m_jitterDirR = 0.0;
	m_jitterIntensity = 0.0;
	m_tgtAngP = m_tgtAngY = m_tgtAngR = 0.0;
	m_tgtPosX = m_tgtPosY = m_tgtPosZ = 0.0;
	m_outAngP = m_outAngY = m_outAngR = 0.0;
	m_outPosX = m_outPosY = m_outPosZ = 0.0;
	m_fovWobble = 0.0;
	m_smRight = m_smUp = m_smFwd = 0.0;
	m_smPitch = m_smYaw = m_smRoll = 0.0;

	m_rngState = 0x9E3779B97F4A7C15ull; // fixed re-seed: takes must be reproducible
	++m_dbgResets;
}

// ============================== reporting ==========================================

void HandheldCam::AppendStateJson(std::ostringstream& o) const {
	o << ",\"handheldOn\":" << (m_enabled ? "true" : "false")
		<< ",\"hhVmDecoupled\":" << (m_viewmodelDecoupled ? "true" : "false")
		<< ",\"hhCenterLockOn\":" << (m_centerLockEnabled ? "true" : "false")
		<< ",\"hhCenterLock\":" << (int)std::lround(m_centerLockPct)
		<< ",\"hhDrift\":" << (int)std::lround(m_driftPct)
		<< ",\"hhDriftDir\":" << (int)std::lround(m_driftDirDeg)
		<< ",\"hhCurve\":" << (int)std::lround(m_curvePct)
		<< ",\"hhTilt\":" << (int)std::lround(m_tiltPct)
		<< ",\"hhRegrip\":" << (int)std::lround(m_regripPct)
		<< ",\"hhSpeed\":" << (int)std::lround(m_speedPct)
		<< ",\"hhVariance\":" << (int)std::lround(m_variancePct)
		<< ",\"hhPeriod\":" << (int)std::lround(m_regripPeriod)
		<< ",\"hhIdle\":\"" << StateCurveKey(HandheldState::Idle) << "\""
		<< ",\"hhWalk\":\"" << StateCurveKey(HandheldState::Walk) << "\""
		<< ",\"hhRun\":\"" << StateCurveKey(HandheldState::Run) << "\"";
}

std::string HandheldCam::DebugStateJson() const {
	std::ostringstream o;
	int live = 0;
	for (int i = 0; i < kMaxLayers; ++i) if (m_layers[i].active) ++live;
	o << "{\"on\":" << (m_enabled ? "true" : "false")
		<< ",\"viewmodelDecoupled\":" << (m_viewmodelDecoupled ? "true" : "false")
		<< ",\"centerLockOn\":" << (m_centerLockEnabled ? "true" : "false")
		<< ",\"centerLock\":" << (float)m_centerLockPct
		<< ",\"drift\":" << (float)m_driftPct
		<< ",\"curve\":" << (float)m_curvePct
		<< ",\"tilt\":" << (float)m_tiltPct
		<< ",\"regrip\":" << (float)m_regripPct
		<< ",\"speed\":" << (float)m_speedPct
		<< ",\"variance\":" << (float)m_variancePct
		<< ",\"state\":\"" << (m_state == HandheldState::Run ? "run" : m_state == HandheldState::Walk ? "walk" : "idle") << "\""
		<< ",\"liveLayers\":" << live
		<< ",\"tiltRoll\":" << (float)m_tiltRoll
		<< ",\"steps\":" << m_dbgSteps
		<< ",\"resets\":" << m_dbgResets
		<< ",\"regrips\":" << m_dbgRegrips
		<< ",\"switches\":" << m_dbgSwitches << "}";
	return o.str();
}

// ============================== console ============================================

namespace {

// Shared by every numeric sub-command: parse, apply, then echo unless the caller passed
// "quiet". The Config sliders always pass "quiet" -- one console line per drag notch floods the
// console and (via the state re-render) can feed back into the slider. See ConfigHudWidgetsJs.h.
bool QuietArg(int argc, advancedfx::ICommandArgs* args, int index) {
	return argc >= index + 1 && 0 == _stricmp(args->ArgV(index), "quiet");
}

void EchoPercent(const char* label, double applied, bool quiet) {
	if (quiet) return;
	if (applied > 0.0) advancedfx::Message("mirv_filmmaker: %s = %d%%.\n", label, (int)std::lround(applied));
	else advancedfx::Message("mirv_filmmaker: %s = off.\n", label);
}

} // namespace

void HandheldCam_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	HandheldCam& hh = HandheldCamRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(action, "reset")) {
		hh.ResetSettings();
		if (!QuietArg(argc, args, 3))
			advancedfx::Message("mirv_filmmaker: handheld camera reset to defaults.\n");
		return;
	}

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off") || 0 == _stricmp(action, "toggle")) {
		const bool on = (0 == _stricmp(action, "toggle")) ? !hh.Enabled() : (0 == _stricmp(action, "on"));
		hh.SetEnabled(on);
		if (!QuietArg(argc, args, 3))
			advancedfx::Message("mirv_filmmaker: handheld camera = %s.\n", on ? "on" : "off");
		return;
	}

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[handheld][state] %s\n", hh.DebugStateJson().c_str());
		return;
	}

	if (0 == _stricmp(action, "viewmodel")) {
		const bool toggle = 0 == _stricmp(value, "toggle") || *value == '\0';
		const bool on = 0 == _stricmp(value, "on") || 0 == _stricmp(value, "1");
		const bool off = 0 == _stricmp(value, "off") || 0 == _stricmp(value, "0");
		hh.SetViewmodelDecoupled(toggle ? !hh.ViewmodelDecoupled() : (off ? false : on));
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: handheld viewmodel decoupling = %s.\n",
				hh.ViewmodelDecoupled() ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(action, "centerlock")) {
		const bool toggle = 0 == _stricmp(value, "toggle") || *value == '\0';
		const bool on = 0 == _stricmp(value, "on") || 0 == _stricmp(value, "1");
		const bool off = 0 == _stricmp(value, "off") || 0 == _stricmp(value, "0");
		hh.SetCenterLockEnabled(toggle ? !hh.CenterLockEnabled() : (off ? false : on));
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: handheld center lock = %s.\n",
				hh.CenterLockEnabled() ? "ON" : "off");
		return;
	}
	if (0 == _stricmp(action, "centerlockpct")) {
		if (*value) hh.SetCenterLockStrength(std::atof(value));
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: handheld center lock strength = %.0f%%.\n",
				hh.CenterLockStrength());
		return;
	}

	if (0 == _stricmp(action, "curves")) {
		advancedfx::Message("mirv_filmmaker: handheld curves (%d):\n", g_handheldCurveCount);
		for (int i = 0; i < g_handheldCurveCount; ++i)
			advancedfx::Message("  %-20s \"%s\" (%d frames @ %.0f fps)\n",
				g_handheldCurves[i].key, g_handheldCurves[i].name,
				g_handheldCurves[i].frames, (double)g_handheldCurves[i].fps);
		return;
	}

	// Intensity knobs (0-150 percent, or "off").
	const bool isDrift = 0 == _stricmp(action, "drift");
	const bool isCurve = 0 == _stricmp(action, "curve");
	const bool isTilt = 0 == _stricmp(action, "tilt");
	const bool isRegrip = 0 == _stricmp(action, "regrip");
	if (isDrift || isCurve || isTilt || isRegrip) {
		double percent = isDrift ? hh.DriftIntensity() : isCurve ? hh.CurveIntensity()
			: isTilt ? hh.TiltIntensity() : hh.RegripIntensity();
		if (*value) percent = (0 == _stricmp(value, "off")) ? 0.0 : std::atof(value);
		if (isDrift) hh.SetDriftIntensity(percent);
		else if (isCurve) hh.SetCurveIntensity(percent);
		else if (isTilt) hh.SetTiltIntensity(percent);
		else hh.SetRegripIntensity(percent);
		const double applied = isDrift ? hh.DriftIntensity() : isCurve ? hh.CurveIntensity()
			: isTilt ? hh.TiltIntensity() : hh.RegripIntensity();
		const char* label = isDrift ? "handheld drift" : isCurve ? "handheld curve"
			: isTilt ? "handheld ambient tilt" : "handheld re-grip";
		EchoPercent(label, applied, QuietArg(argc, args, 4));
		return;
	}

	if (0 == _stricmp(action, "driftdir")) {
		double deg = hh.DriftDirection();
		if (*value) deg = std::atof(value);
		hh.SetDriftDirection(deg);
		if (!QuietArg(argc, args, 4))
			advancedfx::Message("mirv_filmmaker: handheld drift direction = %.0f deg.\n",
				hh.DriftDirection());
		return;
	}

	if (0 == _stricmp(action, "speed") || 0 == _stricmp(action, "variance") || 0 == _stricmp(action, "period")) {
		const bool isSpeed = 0 == _stricmp(action, "speed");
		const bool isVar = 0 == _stricmp(action, "variance");
		double v = isSpeed ? hh.Speed() : isVar ? hh.Variance() : hh.RegripPeriod();
		if (*value) v = std::atof(value);
		if (isSpeed) hh.SetSpeed(v);
		else if (isVar) hh.SetVariance(v);
		else hh.SetRegripPeriod(v);
		if (!QuietArg(argc, args, 4)) {
			const double applied = isSpeed ? hh.Speed() : isVar ? hh.Variance() : hh.RegripPeriod();
			if (isSpeed) advancedfx::Message("mirv_filmmaker: handheld curve speed = %d%%.\n", (int)std::lround(applied));
			else if (isVar) advancedfx::Message("mirv_filmmaker: handheld variance = %d%%.\n", (int)std::lround(applied));
			else advancedfx::Message("mirv_filmmaker: handheld re-grip period = %d s.\n", (int)std::lround(applied));
		}
		return;
	}

	if (0 == _stricmp(action, "preset")) {
		// handheld preset <idle|walk|run> <CURVE_KEY>
		const char* which = value;
		const char* key = (argc >= 5) ? args->ArgV(4) : "";
		HandheldState state;
		if (0 == _stricmp(which, "idle")) state = HandheldState::Idle;
		else if (0 == _stricmp(which, "walk")) state = HandheldState::Walk;
		else if (0 == _stricmp(which, "run")) state = HandheldState::Run;
		else {
			advancedfx::Message("mirv_filmmaker: handheld preset <idle|walk|run> <curve>. Use \"%s handheld curves\" to list.\n", cmd);
			return;
		}
		if (!hh.SetStateCurve(state, key)) {
			advancedfx::Message("mirv_filmmaker: unknown handheld curve \"%s\". Use \"%s handheld curves\" to list.\n", key ? key : "", cmd);
			return;
		}
		if (!QuietArg(argc, args, 5))
			advancedfx::Message("mirv_filmmaker: handheld %s curve = %s.\n", which, hh.StateCurveKey(state));
		return;
	}

	advancedfx::Message(
		"%s handheld on|off|toggle - handheld camera master switch (rides POV and Follow attach preview).\n"
		"%s handheld viewmodel [on|off|toggle] - keep the viewmodel independent from handheld camera motion.\n"
		"%s handheld centerlock [on|off|toggle] - keep recorded aim at screen centre.\n"
		"%s handheld centerlockpct [<0-100>] - center-lock strength.\n"
		"%s handheld drift [<0-150>|off] - procedural sway/jitter/tremble layer, as an intensity percent.\n"
		"%s handheld driftdir [<0-360>] - which way the slow drift leans, in degrees.\n"
		"%s handheld curve [<0-150>|off] - baked motion-capture layer (auto idle/walk/run), as an intensity percent.\n"
		"%s handheld tilt [<0-150>|off] - slow ambient lean, as an intensity percent.\n"
		"%s handheld regrip [<0-150>|off] - periodic operator re-grip fumbles, as an intensity percent.\n"
		"%s handheld speed [<25-300>] - curve playback speed percent.\n"
		"%s handheld variance [<0-100>] - loop-to-loop drift so a looping curve stops reading as a loop.\n"
		"%s handheld period [<2-60>] - mean seconds between re-grip fumbles.\n"
		"%s handheld preset <idle|walk|run> <curve> - assign a curve to a movement state.\n"
		"%s handheld curves - list the available curves.\n"
		"%s handheld state - print current settings + diagnostics.\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

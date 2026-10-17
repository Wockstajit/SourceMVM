#pragma once

// Weapon Bob maths -- engine-free so it can be unit tested (Automation/tests/weapon-bob-math-tests.cpp).
//
// Three procedural terms that together make a first-person weapon feel CARRIED rather than
// welded to the camera. All ported from ARC9 (github.com/HaodongMo/ARC-9, lua/weapons/arc9_base/
// cl_sway.lua + cl_vmposition.lua), the GMod weapon base the "ARC9 Modern Warfare 2019" pack runs
// on -- and that pack is the reference look.
//
// CORRECTION (2026-09-03): an earlier version of this comment claimed the MW2019 pack customises
// nothing about bob. It does, in its OWN base class, which the first survey missed by grepping
// only the shared pack and the per-weapon files:
//
//     arc9_cod2019_base.lua:73   BobWalkMult   = 0.5
//     arc9_cod2019_base.lua:75   BobSprintMult = 0.1
//
// So MW2019 runs the walk figure at HALF strength and the sprint figure at a TENTH. Running ARC9
// stock, as this file first did, is roughly twice the walk bob and ten times the sprint bob of
// the actual reference -- which is what "too bouncy" was. Those multipliers now live per weapon
// class in the table below, together with the hinge.
//
//   1. WALK BOB      -- ARC9 bob style 0, "Bread & Darsu" (`ArcticBreadDarsuBob`), the convar
//                       default and therefore what MW2019 ships with. Amplitude-modulated sines:
//                       each channel's carrier is multiplied by a slow second sine, so the figure
//                       drifts organically instead of repeating.
//   2. LOOK SWAY     -- `GetViewModelSway` + `GetViewModelInertia`. The gun lags behind the aim:
//                       smoothed aim velocity, clamped, rotated about a hinge on the weapon by the
//                       NEGATIVE of the turn. A second, slower copy overshoots (inertia).
//   3. IDLE BREATH   -- the at-rest drift from GetViewModelPosition, so a stationary gun still
//                       lives. CS2's own idle is subtle enough that this reads as absent.
//
// PHASE 0 FINDING: CS2 applies a small viewmodel bob of its own (muzzle camera-relative motion
// rises with speed, ~0.2u at rest to ~1.1u running). The user's verdict on v1, which scaled ARC9
// down to allow for that, was "just wiggling it" -- so v2 runs ARC9 at reference strength and
// lets Amount go to 150% on top.
//
// Determinism: every function here is PURE. The two framerate hacks in the ARC9 source are
// deliberately NOT ported -- `sightmult = 0.5 + clamp(1/ft/100, 0, 5)` and the per-frame aim
// DELTAS the sway is built on both scale with render rate. Here sway takes aim VELOCITY (deg/s)
// and multiplies by a fixed reference frame time, so the numbers match ARC9 at 100 fps and a
// rendered movie is identical at any rate.

#include "WeaponPivot.h" // the hinge: one shared answer, see that header

#include <algorithm>
#include <cmath>

namespace Filmmaker {

constexpr double kWeaponBobPi = 3.14159265358979323846;

// One frame's output in the weapon's own frame: camera-basis position (forward/right/up, world
// units) plus rotation (degrees, engine convention: +pitch down, +yaw left, +roll clockwise).
struct WeaponBobOffset {
	double fwd = 0.0, right = 0.0, up = 0.0;
	double pitch = 0.0, yaw = 0.0, roll = 0.0;

	bool Any() const {
		return fwd != 0.0 || right != 0.0 || up != 0.0
			|| pitch != 0.0 || yaw != 0.0 || roll != 0.0;
	}
	void Add(const WeaponBobOffset& o) {
		fwd += o.fwd; right += o.right; up += o.up;
		pitch += o.pitch; yaw += o.yaw; roll += o.roll;
	}
};

inline double WeaponBobClamp(double v, double lo, double hi) {
	// NaN-rejecting comparison chain rather than std::clamp: a NaN from a bad velocity read
	// resolves to the low bound instead of propagating.
	if (!(v > lo)) return lo;
	return v > hi ? hi : v;
}

inline double WeaponBobLerp(double t, double a, double b) { return a + (b - a) * t; }

// ARC9 normalises speed against 350 u/s, which is GMod's sprint. CS2's fastest run is ~250, so
// the same figure normalised against 250 puts a CS2 run at d = 1 -- the top of ARC9's range --
// which is where MW's sprint sway lives. Deliberate: a CS2 run should read as a MW sprint.
constexpr double kWeaponBobReferenceSpeed = 250.0;

// The reference frame time the sway numbers are calibrated at. See the header note.
constexpr double kWeaponBobRefDt = 1.0 / 100.0;

// Where the weapon HINGES, and how hard it bobs -- BOTH per weapon class.
//
// ARC9 rotates about `CustomizeRotateAnchor` with X scaled by 0.75, and that anchor is authored
// PER WEAPON because it is a point on the model. Two measured MW2019 anchors, in GMod's
// (fwd, left, up) frame:
//
//     M4     Vector( 14, -3,    -4)  ->  10.5 fwd, 3.0 right, 4 down   (rifle, held out front)
//     knife  Vector(-10, -2.25, -4)  ->  -7.5 fwd, 2.25 right, 4 down  (held close, hinge BEHIND)
//
// The knife's hinge is behind the camera. That sign flip is the whole reason a knife needs its own
// entry: rotating a close-held model about a point 10 units in FRONT of the eye swings it across
// the screen, which is exactly the reported "the knife jumps around a lot". Same mechanism, less
// extreme, for pistols and SMGs held with the hands close together.
// The class vocabulary and the hinge both live in WeaponPivot.h now -- the pivot is a fact
// about the weapon that Weapon Recoil needs too, and two copies of it is how the orb ended
// up drawn somewhere the gun was not actually rotating about.
using WeaponBobClass = WeaponPivotClass;

// Bob-specific per-class scaling. The HINGE is deliberately not in here: it is shared with
// Weapon Recoil and lives in WeaponPivotStore.
struct WeaponBobClassProfile {
	double walkMult = 1.0;   // scales the walk figure
	double sprintMult = 1.0; // scales the sprint figure
};

// Defaults. The two MEASURED entries are rifle and knife; the rest are interpolated by how far
// from the eye the weapon is actually held, which is what the hinge represents.
//
// WHY sprintMult EQUALS walkMult, AND WHY MW2019's 0.1 IS NOT COPIED HERE.
// An earlier version of this table gave rifles sprintMult 0.55, reasoning from MW2019's
// `BobSprintMult = 0.1`. That halved the bob in normal play and read as "barely moving even with
// maxed out settings" -- measured, p95 position travel fell from 1.76u to 0.87u while the sprint
// blend sat at 0.70, i.e. the reduction was active roughly 70% of the time.
//
// The mistake was a modelling one. MW2019's sprint multiplier applies to a TACTICAL SPRINT: a
// distinct state where the weapon is lowered across the body and a dedicated animation takes
// over, so the procedural figure is deliberately suppressed to stay out of its way. CS2 has no
// such state. A CS2 player at 250 u/s is doing what MW calls WALKING, and gets MW's walk
// treatment. The sprint BRANCH of the figure is still used (it is the larger, more interesting
// shape, and a CS2 run should look like one) but it is not scaled down.
//
// sprintMult stays in the struct as a tuning knob, defaulted equal to walkMult. Anyone who wants
// the literal MW2019 numbers can reach them with `weaponbob classmult 0.5 0.1`.
//
// The per-class WALK differences are the part that survived from live feedback: pistols and SMGs
// are held with the hands close together and read as too bouncy at rifle strength, and a knife
// more so again.
inline WeaponBobClassProfile WeaponBobProfileFor(WeaponBobClass c) {
	switch (c) {
	case WeaponBobClass::Rifle:   return { 1.00, 1.00 };
	case WeaponBobClass::Smg:     return { 0.62, 0.62 };
	case WeaponBobClass::Pistol:  return { 0.55, 0.55 };
	case WeaponBobClass::Shotgun: return { 0.92, 0.92 };
	case WeaponBobClass::Sniper:  return { 0.88, 0.88 };
	case WeaponBobClass::Knife:   return { 0.38, 0.38 };
	default:                      return { 0.55, 0.55 };
	}
}

inline const char* WeaponBobClassName(WeaponBobClass c) { return WeaponPivotClassName(c); }

// ============================== 1. WALK BOB ================================================

// ARC9's `ease.InExpo`.
inline double WeaponBobInExpo(double x) {
	return x <= 0.0 ? 0.0 : std::pow(2.0, 10.0 * x - 10.0);
}

// The walk clock rate. ARC9: lerp(d, 1, 2.75), pulled toward 0.75 in the air.
inline double WeaponBobStepRate(double speedFrac, double airborne) {
	// BOTH inputs clamped here, not just airborne: this feeds the phase ACCUMULATOR, so unlike the
	// evaluator a NaN reaching the return value is permanent.
	const double d = WeaponBobClamp(speedFrac, 0.0, 1.0);
	const double ground = WeaponBobLerp(d, 1.0, 2.75);
	return WeaponBobLerp(WeaponBobClamp(airborne, 0.0, 1.0), ground, 0.75);
}

inline double WeaponBobAdvancePhase(double phase, double dt, double speedFrac, double airborne) {
	if (!(dt > 0.0)) return phase;
	phase += dt * WeaponBobStepRate(speedFrac, airborne);
	if (phase > 1.0e6) phase -= 1.0e6;
	return phase;
}

// The stride frequency: ARC9's `step`, 9.25 standing and 6 crouched.
inline double WeaponBobStepFreq(double crouch) {
	return WeaponBobLerp(WeaponBobClamp(crouch, 0.0, 1.0), 9.25, 6.0);
}

// The CARRIER accumulator. ARC9 evaluates the gait as sin(BobCT * step) and snaps `step` on a
// hard crouch boolean, so the argument jumps exactly once. Here crouch is SMOOTHED, and with
// BobCT in the hundreds a `step` sliding 9.25 -> 6 over a third of a second sweeps the argument
// through ~300 radians -- the gun visibly thrashed while crouching (reported live 2026-09-03).
// Integrating the carrier as its own accumulator makes a frequency change alter only the SPEED of
// the cycle, never its position, which is the continuity a smoothed input needs.
inline double WeaponBobAdvanceStepArg(double stepArg, double dt, double speedFrac,
	double airborne, double crouch)
{
	if (!(dt > 0.0)) return stepArg;
	stepArg += dt * WeaponBobStepRate(speedFrac, airborne) * WeaponBobStepFreq(crouch);
	// Wrap on a whole number of fundamentals. Every sub-carrier is a rational multiple of the
	// fundamental (0.5, 0.3, 0.12), so 50 fundamentals is an exact period for all of them.
	const double kWrap = 2.0 * kWeaponBobPi * 50.0;
	if (stepArg > kWrap) stepArg -= kWrap;
	return stepArg;
}

// The jump/land term. ARC9: a clamped InExpo blend of rising and falling vertical speed, then
// smoothed by the caller; landing (negative vz) dominates.
//   vz in units/sec (+up). Returns ARC9's `jumpmove`, pre-smoothing.
inline double WeaponBobJumpMove(double vz) {
	const double fall = WeaponBobInExpo(WeaponBobClamp(vz, -150.0, 0.0) / -150.0) * 0.5;
	const double rise = WeaponBobInExpo(WeaponBobClamp(vz, 0.0, 350.0) / 350.0) * -50.0;
	return WeaponBobClamp(fall + rise, -4.0, 2.5) * 0.5;
}

// ---- am I in the air? -------------------------------------------------------------------------
//
// The gait fades out and the walk clock slows while the player is airborne. What "airborne" is
// decided from is the whole story behind the reported stair / floor-weapon bug: it used to be
// |vertical speed of the camera| / 180, and a staircase moves the eye up at a sustained 100+ u/s
// (a weapon or a kerb gives a sharp spike), which read as ~65% airborne -- the bob thinned out and
// slowed ("stops and smooths out") and the same vertical speed fed the jump term, tipping the gun
// several degrees ("exaggerates"). The engine knows whether the player is on the ground, so that
// flag is the input; the position-derived estimate below is only for when the pawn cannot be read.

// Ground contact has to be lost for this long before the player counts as airborne, then ramps in
// over the second interval. Walking down stairs drops FL_ONGROUND for a tick or two at a time;
// a jump or a fall stays off the ground for hundreds of milliseconds.
constexpr double kWeaponBobAirDebounce = 0.06;
constexpr double kWeaponBobAirRamp = 0.06;

// Seconds continuously off the ground. Held (not advanced) on a zero-dt frame.
inline double WeaponBobAirTimeAdvance(double airTime, double dt, bool onGround) {
	if (onGround) return 0.0;
	if (!(dt > 0.0)) return airTime;
	const double t = airTime + dt;
	return t > 10.0 ? 10.0 : t;
}

// 0..1 airborne target from the time spent off the ground.
inline double WeaponBobAirborneTarget(double airTime) {
	return WeaponBobClamp((airTime - kWeaponBobAirDebounce) / kWeaponBobAirRamp, 0.0, 1.0);
}

// Fallback when the pawn's own state is unavailable: the part of the camera's vertical speed that
// walking on a slope cannot explain. Stairs and ramps climb at a fixed fraction of the planar
// speed (up to about 0.6 in CS2), a jump does not, so only the excess over that fraction plus a
// margin counts. Signed like vz (+up).
inline double WeaponBobFreeVz(double vz, double planarSpeed) {
	const double slope = 0.6 * WeaponBobClamp(planarSpeed, 0.0, 400.0) + 60.0;
	const double mag = std::fabs(WeaponBobClamp(vz, -2000.0, 2000.0));
	if (!(mag > slope)) return 0.0;
	return vz > 0.0 ? mag - slope : -(mag - slope);
}

// 0..1 airborne estimate from WeaponBobFreeVz.
inline double WeaponBobAirborneEstimate(double vz, double planarSpeed) {
	return WeaponBobClamp(std::fabs(WeaponBobFreeVz(vz, planarSpeed)) / 120.0, 0.0, 1.0);
}

// The bob for this frame -- ARC9 style 0, walk and sprint branches blended by speed.
//
//   phase       walk clock (seconds, already advanced)
//   speedFrac   planar speed / reference, 0..1
//   sprint      0..1 blend into the sprint branch (caller derives from speedFrac)
//   airborne    0..1
//   crouch      0..1
//   ads         0..1 (ARC9's sightamount; CS2 feeds 0 -- no true ADS)
//   sideMove    ARC9's smoothed `sidemove`: (vel . right / maxSpeed) * 4 * (1.5 - sight)
//   jumpMove    ARC9's smoothed `jumpmove` (see WeaponBobJumpMove)
//   swayPct     position channels, 0..150
//   tiltPct     rotation channels, 0..150
//   amountPct   master, 0..150
//   phase       the AM-envelope clock (seconds, WeaponBobAdvancePhase)
//   stepArg     the carrier accumulator (radians, WeaponBobAdvanceStepArg)
inline WeaponBobOffset WeaponBobEvaluate(
	double phase, double stepArg, double speedFrac, double sprint, double airborne, double crouch,
	double ads, double sideMove, double jumpMove,
	double swayPct, double tiltPct, double amountPct, const WeaponBobClassProfile& profile)
{
	WeaponBobOffset out;
	const double master = WeaponBobClamp(amountPct, 0.0, 150.0) / 100.0;
	if (master <= 0.0) return out;
	const double sway = (WeaponBobClamp(swayPct, 0.0, 150.0) / 100.0) * master;
	const double tilt = (WeaponBobClamp(tiltPct, 0.0, 150.0) / 100.0) * master;

	const double sight = WeaponBobClamp(ads, 0.0, 1.0);
	// ARC9 subtracts 0.1 from sightamount when not peeking, so hipfire sits slightly below 0.
	const double sightamount = sight - 0.1;
	const double cr = WeaponBobClamp(crouch, 0.0, 1.0);
	const double sp = WeaponBobClamp(sprint, 0.0, 1.0);

	double d = WeaponBobClamp(speedFrac, 0.0, 1.0);
	d *= WeaponBobLerp(sightamount, 1.0, 0.03);
	// Airborne kills the gait; ARC9 does this through the step rate alone, but with the phase
	// idling the amplitude must also fade or the gun keeps swinging in mid-air.
	d *= 1.0 - 0.85 * WeaponBobClamp(airborne, 0.0, 1.0);
	const double mag = d * 2.0;

	// ARC9: crouching multiplies the vertical channel HARD -- crouch-walking is where the gun
	// visibly rocks. (Its other crouch effect, the shorter stride, lives in the carrier rate.)
	const double crouchmult = WeaponBobLerp(cr, 1.0, 3.5 + sightamount * 3.0);

	const double jump2 = WeaponBobClamp(jumpMove, -0.3, 0.01) * (1.5 - sightamount) * 2.0;

	const double p = phase;   // envelope clock
	const double a = stepArg; // carrier

	// ---- walk branch (ARC9 verbatim, then scaled by the knobs) ----
	double wUp = -std::sin(a) * 0.1 * ((std::sin(p * 3.515) * 0.2) + 1.5) * mag * crouchmult
		- (sideMove * -0.05) - (jump2 * 0.2);
	double wFwd = std::sin(a * 0.3) * 0.11 * ((std::sin(p * 0.615) * 0.2) + 1.0) * mag;
	double wRight = (std::sin(a * 0.5) + std::cos(a * 0.5)) * 0.2 * mag;
	double wRoll = std::sin(a * 0.5) * ((std::sin(p * 6.151) * 0.2) + 1.0) * 5.0 * d + sideMove;
	double wPitch = std::sin(a * 0.12) * ((std::sin(p * 1.521) * 0.2) + 1.0) * 0.1 * d
		+ jump2 * 5.0;
	double wYaw = 0.0;

	// ---- sprint branch (ARC9 verbatim) ----
	double sUp = -std::sin(a) * 0.45 * ((std::sin(p * 3.515) * 0.2) + 1.0) * mag;
	double sFwd = std::sin(a * 0.3) * 0.13 * 2.0 * ((std::sin(p * 0.615) * 0.2) + 2.0) * mag;
	double sRight = (std::sin(a * 0.5) + std::cos(a * 0.5)) * 0.55 * mag;
	double sRoll = std::sin(a * 0.5) * ((std::sin(p * 6.151) * 0.2) + 1.0) * 6.0 * d + sideMove * 1.5;
	double sPitch = std::sin(a * 0.12) * ((std::sin(p * 1.521) * 0.2) + 1.0) * 1.0 * d
		+ jump2 * 5.0;
	double sYaw = std::sin(a * 0.5) * ((std::sin(p * 1.521) * 0.2) + 1.0) * 3.0 * d;

	// ARC9's BobWalkMult / BobSprintMult, per class: the walk and sprint branches carry their own
	// scale, so a weapon can be calm while walking and still settle down hard at a run.
	const double wM = WeaponBobClamp(profile.walkMult, 0.0, 4.0);
	const double sM = WeaponBobClamp(profile.sprintMult, 0.0, 4.0);
	const double posK = WeaponBobLerp(sp, wM, sM) * sway;
	const double rotK = WeaponBobLerp(sp, wM, sM) * tilt;

	out.up    = WeaponBobLerp(sp, wUp, sUp) * posK;
	out.fwd   = WeaponBobLerp(sp, wFwd, sFwd) * posK;
	out.right = WeaponBobLerp(sp, wRight, sRight) * posK;
	out.roll  = WeaponBobLerp(sp, wRoll, sRoll) * rotK;
	out.pitch = WeaponBobLerp(sp, wPitch, sPitch) * rotK;
	out.yaw   = WeaponBobLerp(sp, wYaw, sYaw) * rotK;
	return out;
}

// ============================== 2. LOOK SWAY ===============================================

// The residual translation of the hinge point under a rotation -- (d - R*d) -- so that rotating
// the viewmodel about the CAMERA (all the hook can do) looks like rotating it about the hinge.
// Same exact form Weapon Recoil uses; not the small-angle approximation.
inline void WeaponBobHingeCompensate(double pitchDeg, double yawDeg,
	const WeaponPivotPoint& hinge, double& fwd, double& right, double& up)
{
	if (pitchDeg == 0.0 && yawDeg == 0.0) return;
	constexpr double kDeg = kWeaponBobPi / 180.0;
	const double dF = hinge.fwd, dR = hinge.right, dU = hinge.up;
	const double cp = std::cos(pitchDeg * kDeg), sp = std::sin(pitchDeg * kDeg);
	const double cy = std::cos(yawDeg * kDeg), sy = std::sin(yawDeg * kDeg);
	// Camera-local: X fwd, Y right, Z up. Engine convention: +pitch tips forward DOWN, +yaw
	// swings it LEFT (toward -right).
	const double x1 = dF * cy + dR * sy;
	const double y1 = -dF * sy + dR * cy;
	const double z1 = dU;
	const double x2 = x1 * cp + z1 * sp;
	const double z2 = -x1 * sp + z1 * cp;
	fwd += (dF - x2);
	right += (dR - y1);
	up += (dU - z2);
}

// Aim samples arrive on the same render clock as the interpolated view angles. The raw
// demo clock can hold, go backwards, or advance only 0.4 ms while the view moves a full
// frame (Inferno 7:29-7:30). Dividing that move by raw dt manufactured violent look sway.
struct WeaponBobAimState {
    bool have = false;
    double pitch = 0.0, yaw = 0.0;
    double held = 0.0;                 // seconds the angle has not moved since it last changed
    double velPitch = 0.0, velYaw = 0.0; // the velocity measured at that change
};
// A spectated player's view angle moves once per demo tick (64 a second) while frames render
// faster, so most frames repeat the last angle. Up to this long without a change the turn is
// taken to continue at its last measured speed; past it the player has stopped turning.
constexpr double kWeaponBobAimHoldSeconds = 2.5 / 64.0;

inline double WeaponBobWrapDeg(double a) {
    a = std::fmod(a, 360.0);
    if (a > 180.0) a -= 360.0;
    else if (a < -180.0) a += 360.0;
    return a;
}

// Aim velocity is a derivative of the rendered angle, not an oscillator phase.
// Phase-clock catch-up can issue tiny/zero steps while the view still advances:
// using it as the denominator makes a steady turn jerk or lose whole samples.
// Playback uses the real render interval at the selected demo speed; export keeps
// exact demo intervals and paused playback always rebases without a kick.
inline double WeaponBobAimInterval(double demoDt,double wallDt,double rate,bool recording,bool paused) {
    if(recording)return std::isfinite(demoDt)&&demoDt>0?demoDt:0;
    if(paused||!std::isfinite(wallDt)||wallDt<=0||!std::isfinite(rate)||rate<=0)return 0;
    return wallDt*rate;
}

// Returns true on a spectator cut so the caller can clear its sway filter. A paused frame
// rebases the angle and zeroes the turn: it must never carry into a later sample.
//
// TICK-STEPPED ANGLES. Measured 2026-10-17 (weaponbob.frame, MoNaTy demo): during a steady turn
// the per-frame velocity read (-9.6, 129) -> (-9.1, 93.4) -> (0, 0) -> (0, 0): frames between two
// ticks repeat the angle and read zero, then the tick frame packs the whole tick's turn into one
// short frame. Sway followed that zero / spike pattern, which is the laggy, wrong carry. A frame
// that repeats the angle now keeps the last measured velocity (up to kWeaponBobAimHoldSeconds),
// and a change is measured over the whole time since the previous change.
inline bool WeaponBobAimAdvance(WeaponBobAimState& st, double sampleDt,
    double pitch, double yaw, double& pitchVelocity, double& yawVelocity)
{
    pitchVelocity = yawVelocity = 0.0;
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) { st = {}; return true; }
    const double dP = WeaponBobWrapDeg(pitch - st.pitch);
    const double dY = WeaponBobWrapDeg(yaw - st.yaw);
    const bool cut = st.have && (std::fabs(dP) > 45.0 || std::fabs(dY) > 45.0);
    if (!st.have || cut || !(sampleDt > 0.0) || !std::isfinite(sampleDt)) {
        st.have = true; st.pitch = pitch; st.yaw = yaw;
        st.held = 0.0; st.velPitch = st.velYaw = 0.0;
        return cut;
    }
    if (std::fabs(dP) < 1e-6 && std::fabs(dY) < 1e-6) {
        st.held += sampleDt;
        if (st.held <= kWeaponBobAimHoldSeconds) { pitchVelocity = st.velPitch; yawVelocity = st.velYaw; }
        else st.velPitch = st.velYaw = 0.0;
        return false;
    }
    // After a stop the change still happened within the last tick, not within this one frame.
    const double interval = st.held <= kWeaponBobAimHoldSeconds ? st.held + sampleDt
        : (std::max)(sampleDt, 1.0 / 64.0);
    pitchVelocity = st.velPitch = dP / interval;
    yawVelocity = st.velYaw = dY / interval;
    st.held = 0.0;
    st.pitch = pitch; st.yaw = yaw;
    return false;
}

// Look sway state: the smoothed aim-velocity terms ARC9 keeps in module-level locals.
struct WeaponBobSwayState {
	double pitch = 0.0; // smootheyeang.p  (smoothed per-ref-frame aim delta, degrees)
	double yaw = 0.0;   // smootheyeang.y
	double roll = 0.0;  // smoothswayroll (a second smoothing of yaw -> roll)
	double inertiaP = 0.0; // ViewModelSwayInertia
	double inertiaY = 0.0;
};

inline double WeaponBobApproach(double cur, double target, double dt, double halfLife) {
	if (!(dt > 0.0) || !(halfLife > 0.0)) return target;
	return cur + (target - cur) * (1.0 - std::pow(0.5, dt / halfLife));
}

// ARC9 lerps with `clamp(ft * 24, 0.075, 1)`: a rate of 24/s. Half-life = ln2 / 24.
constexpr double kWeaponBobSwayHalfLife = 0.6931 / 24.0;
// The heavy overshoot's half-life. ~8x slower than the fast sway, so it is still swinging when
// the fast term has already settled -- that gap IS the weight.
constexpr double kWeaponBobInertiaHalfLife = 0.22;

// Advances the sway state from this frame's aim velocity. Deterministic: velocity in deg/s is
// turned into the per-frame delta ARC9 sees at the reference frame time, so the smoothed values
// land where ARC9's would at 100 fps whatever the render rate.
//
//   aimVelPitch / aimVelYaw   degrees per second (engine convention)
inline void WeaponBobSwayAdvance(WeaponBobSwayState& st, double dt,
	double aimVelPitch, double aimVelYaw)
{
	if (!(dt > 0.0)) return;
	const double dP = WeaponBobClamp(aimVelPitch, -3600.0, 3600.0) * kWeaponBobRefDt;
	const double dY = WeaponBobClamp(aimVelYaw, -3600.0, 3600.0) * kWeaponBobRefDt;

	st.pitch = WeaponBobApproach(st.pitch, dP, dt, kWeaponBobSwayHalfLife);
	st.yaw = WeaponBobApproach(st.yaw, dY, dt, kWeaponBobSwayHalfLife);
	st.roll = WeaponBobApproach(st.roll, st.yaw, dt, kWeaponBobSwayHalfLife);

	// GetViewModelInertia -- the INTENT, not the source. ARC9 approaches its target at a rate
	// proportional to the value itself (`vsi/10 * ft/0.5`), and the value starts at zero, so the
	// step is 0 * anything = 0 and math.ApproachAngle returns 0 forever: the reference term can
	// never leave rest. Verified by the unit test on a faithful port (with even a small floor it
	// reached ~0.01 after a 0.3 s turn and decayed to nothing). What the term is FOR is a slow,
	// heavy overshoot that keeps swinging after the fast sway has settled -- so that is what this
	// is: the same clamped target, smoothed with a long half-life. Builds through a turn, then
	// takes ~0.2 s per halving to let go.
	const double tP = WeaponBobClamp(dP / 4.0, -1.0, 1.0);
	const double tY = WeaponBobClamp(dY / 4.0, -1.0, 1.0);
	st.inertiaP = WeaponBobApproach(st.inertiaP, tP, dt, kWeaponBobInertiaHalfLife);
	st.inertiaY = WeaponBobApproach(st.inertiaY, tY, dt, kWeaponBobInertiaHalfLife);
}

// This frame's look-sway offset from the state.
//
//   lookPct      GetViewModelSway strength, 0..150 (100 = ARC9 at 100 fps)
//   inertiaPct   GetViewModelInertia strength, 0..150
//   ads          0..1
// Rotation is ABOUT THE HINGE: the returned rotation is applied by the hook about the camera,
// and the returned position already carries the hinge compensation.
inline WeaponBobOffset WeaponBobSwayEvaluate(const WeaponBobSwayState& st,
	double lookPct, double inertiaPct, double ads, const WeaponPivotPoint& hinge)
{
	WeaponBobOffset out;
	const double look = WeaponBobClamp(lookPct, 0.0, 150.0) / 100.0;
	const double inertia = WeaponBobClamp(inertiaPct, 0.0, 150.0) / 100.0;
	if (look <= 0.0 && inertia <= 0.0) return out;
	const double sight = WeaponBobClamp(ads, 0.0, 1.0);

	// sightmult at the reference frame time: 0.5 + 1/(0.01*100) = 1.5, then lerp(sight, 1, 0.25).
	const double sightmult = 1.5 * WeaponBobLerp(sight, 1.0, 0.25) * look;

	// Clamps from GetViewModelSway. Roll's `(0.5 + clamp(ft*64, 0, 4))` is another fps term;
	// at the reference rate it is 0.5 + 0.64 = 1.14.
	const double p = WeaponBobClamp(st.pitch * 0.95, -10.0, 10.0);
	const double y = WeaponBobClamp(st.yaw * 0.9, -4.0, 4.0);
	const double r = WeaponBobClamp(st.roll * 1.14, -2.0, 2.0);

	// Position lag first (ARC9 adds these before the rotate).
	out.up += p * 0.075 * sightmult;
	out.right += y * -0.1 * sightmult;

	// RotateAroundPoint2 multiplies by the INVERSE of the offset angle: the gun turns AGAINST the
	// aim, i.e. lags behind it. Engine sign convention on the way out.
	double rotPitch = -p * sightmult;
	double rotYaw = -y * sightmult;
	double rotRoll = -r * sightmult;

	// GetViewModelInertia: 12 degrees per unit, scaled by (1 - sight). Its pitch is negated in
	// the source (RotateAroundAxis(Right, -vsi.p)); yaw is not.
	const double dI = (1.0 - sight) * inertia;
	rotYaw += st.inertiaY * 12.0 * dI;
	rotPitch += -st.inertiaP * 12.0 * dI;

	out.pitch = rotPitch;
	out.yaw = rotYaw;
	out.roll = rotRoll;
	WeaponBobHingeCompensate(rotPitch, rotYaw, hinge, out.fwd, out.right, out.up);
	return out;
}

// ============================== 3. IDLE BREATH =============================================

// The at-rest drift from GetViewModelPosition. `curTime * pi * lerp(sight, 1, 0.5)` is the
// clock; amplitudes are small, and this runs REGARDLESS of movement -- it is what stops a
// standing gun from looking pinned.
inline WeaponBobOffset WeaponBobIdleEvaluate(double timeSeconds, double ads, double idlePct) {
	WeaponBobOffset out;
	const double idle = WeaponBobClamp(idlePct, 0.0, 150.0) / 100.0;
	if (idle <= 0.0) return out;
	const double sight = WeaponBobClamp(ads, 0.0, 1.0);
	const double sighted = WeaponBobLerp(sight, 1.0, 0.1) * idle;
	const double ct = timeSeconds * kWeaponBobPi * WeaponBobLerp(sight, 1.0, 0.5);
	out.right = -(sighted * std::sin(ct * 0.8) * 0.01);
	out.up = -(sighted * std::cos(ct * 0.84) * 0.02);
	out.fwd = -(sighted * std::cos(ct * 0.84) * 0.02);
	out.pitch = sighted * std::sin(ct * 0.84) * -0.07;
	out.yaw = sighted * std::cos(ct * -0.65) * -0.07;
	out.roll = sighted * std::sin(ct * 0.5) * 0.25;
	return out;
}

// ============================== footstep sync ==============================================

// Pulls the walk CARRIER toward the nearest footfall: the vertical channel is -sin(stepArg),
// at its minimum where sin = +1, i.e. stepArg = pi/2 + 2*pi*n. A partial pull, never a snap.
inline double WeaponBobSyncToFootstep(double stepArg, double strength) {
	const double k = WeaponBobClamp(strength, 0.0, 1.0);
	if (k <= 0.0) return stepArg;
	const double period = 2.0 * kWeaponBobPi;
	const double quarter = period * 0.25;
	const double cycles = (stepArg - quarter) / period;
	const double nearest = std::floor(cycles + 0.5) * period + quarter;
	return stepArg + (nearest - stepArg) * k * 0.34;
}

} // namespace Filmmaker

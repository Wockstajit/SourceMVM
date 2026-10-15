#include "WeaponBob.h"

#include "ViewFxVm.h" // ViewFxVm_EnsureCameraCompensationHook: the channel the gun moves on

#include "CameraShakeStack.h"   // CameraShake_SpectatedWeapon
#include "WeaponPivot.h"        // the shared hinge + the one weapon classifier
#include "RecoilPivotMarker.h"  // the card's orb: part of whether its Reset may dim

#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdlib>
#include <string>

namespace Filmmaker {
namespace {

double Clamp(double v, double lo, double hi) {
	if (!(v > lo)) return lo;
	return v > hi ? hi : v;
}

// Half-life smoothing so the same demo smooths identically at any render rate.
double Approach(double current, double target, double dt, double halfLife) {
	return WeaponBobApproach(current, target, dt, halfLife);
}

constexpr double kDeg = kWeaponBobPi / 180.0;

// ARC9 lerps side/jump with `clamp(ft * 8, 0, 1)`: rate 8/s. Jump keeps that. Side does NOT:
// GMod velocity changes through acceleration, CS2 counter-strafing reverses it in one tick, and
// sidemove feeds ROLL directly at up to +-6.4 degrees -- at 8/s a quick A/D tap rate became a
// +-6 degree roll thrash (reported live 2026-09-03). ~0.2 s keeps the lean on a held strafe
// and averages the taps out.
constexpr double kJumpHalfLife = 0.6931 / 8.0;
constexpr double kSideHalfLife = 0.20;
// ARC9 approaches ViewModelNotOnGround by ft / 0.1: a 0.1 s ramp.
constexpr double kAirborneHalfLife = 0.05;

} // namespace

WeaponBob::WeaponBob() {
	for (int i = 0; i < 7; ++i) m_profiles[i] = WeaponBobProfileFor((WeaponBobClass)i);
}

void WeaponBob::SetClassWalk(WeaponBobClass c, double mult) {
	m_profiles[(int)c].walkMult = Clamp(mult, 0.0, 4.0);
}
void WeaponBob::SetClassSprint(WeaponBobClass c, double mult) {
	m_profiles[(int)c].sprintMult = Clamp(mult, 0.0, 4.0);
}
// Kept so `weaponbob hinge` still works; it now edits the SHARED pivot, which is what
// Weapon Recoil and the orb read too.
void WeaponBob::SetClassHinge(WeaponBobClass c, double fwd, double right, double up) {
	WeaponPivotBobRef().SetOverride(c, WeaponPivotPoint{ fwd, right, up });
}
void WeaponBob::SetClassMult(double walkMult, double sprintMult) {
	for (int i = 0; i < 7; ++i) {
		const WeaponBobClassProfile base = WeaponBobProfileFor((WeaponBobClass)i);
		m_profiles[i].walkMult = Clamp(base.walkMult * walkMult, 0.0, 4.0);
		m_profiles[i].sprintMult = Clamp(base.sprintMult * sprintMult, 0.0, 4.0);
	}
}

WeaponBob& WeaponBobRef() {
	static WeaponBob s_instance;
	return s_instance;
}

void WeaponBob::SetEnabled(bool on) {
	if (on == m_enabled) return;
	m_enabled = on;
	Reset();
	// INSTALL THE VIEWMODEL HOOK. Everything this module computes reaches the gun through the
	// CalcViewModelView input detour, and that detour is installed LAZILY by whoever needs it --
	// previously only Weapon Recoil and Handheld Camera. With just Weapon Bob enabled nothing
	// installed it, so ViewFxVm_SetWeaponKick wrote to atomics no one read and the weapon did not
	// move at all, while the camera share (which writes the view directly) and the pivot marker
	// (published by the stack) both moved -- exactly the "pivot moves but the gun does not"
	// report. Reported live 2026-09-04.
	if (on && !ViewFxVm_EnsureCameraCompensationHook())
		advancedfx::Warning(
			"mirv_filmmaker: weapon bob could not install the viewmodel hook "
			"(CalcViewModelView caller not resolved) -- the gun will not move.\n");
}

void WeaponBob::ResetSettings() {
	m_enabled = false;
	m_amount = 100.0;
	m_sway = 100.0;
	m_tilt = 100.0;
	m_lookSway = 100.0;
	m_inertia = 100.0;
	m_idle = 100.0;
	m_cameraShare = 12.0;
	m_stepSync = 60.0;
	m_rate = 100.0;
	for (int i = 0; i < 7; ++i) m_profiles[i] = WeaponBobProfileFor((WeaponBobClass)i);
	Reset();
}

bool WeaponBob::SettingsAreDefault() const {
	// The hinge and the marker are controls on the WEAPON BOB card but live in their own
	// modules, so a check of this module alone reported "already default" while the pivot was
	// moved and the orb was lit -- which DIMMED the card's Reset in exactly the state that
	// needed it. What the card shows is what the card's Reset must answer for.
	if (!WeaponPivotBobRef().IsDefault()) return false;
	if (RecoilPivotMarkerRef().Enabled(WeaponPivotOwner::Bob)) return false;
	return !m_enabled && m_amount == 100.0 && m_sway == 100.0 && m_tilt == 100.0
		&& m_lookSway == 100.0 && m_inertia == 100.0 && m_idle == 100.0
		&& m_cameraShare == 12.0 && m_stepSync == 60.0 && m_rate == 100.0;
}

void WeaponBob::SetAmount(double percent) { m_amount = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetSway(double percent) { m_sway = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetTilt(double percent) { m_tilt = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetLookSway(double percent) { m_lookSway = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetInertia(double percent) { m_inertia = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetIdle(double percent) { m_idle = Clamp(percent, 0.0, 150.0); }
void WeaponBob::SetCameraShare(double percent) { m_cameraShare = Clamp(percent, 0.0, 100.0); }
void WeaponBob::SetStepSync(double percent) { m_stepSync = Clamp(percent, 0.0, 100.0); }
void WeaponBob::SetRate(double percent) { m_rate = Clamp(percent, 25.0, 400.0); }

void WeaponBob::Reset() {
	m_phase = 0.0;
	m_stepArg = 0.0;
	m_idleClock = 0.0;
	m_speedFrac = 0.0;
	m_sprint = 0.0;
	m_airborne = 0.0;
	m_airTime = 0.0;
	m_crouch = 0.0;
	m_sideMove = 0.0;
	m_jumpMove = 0.0;
	m_swayState = WeaponBobSwayState{};
	m_aimState = {};
	m_aimVelPitch = m_aimVelYaw = 0.0;
	m_pose = m_poseBob = m_poseSway = m_poseIdle = WeaponBobOffset{};
	++m_resets;
}

void WeaponBob::OnFootstep() {
	if (!Enabled() || m_stepSync <= 0.0) return;
	++m_footsteps;
	m_stepArg = WeaponBobSyncToFootstep(m_stepArg, m_stepSync / 100.0);
}

ShakePose WeaponBob::Advance(const ShakeInput& in) {
	m_pose = m_poseBob = m_poseSway = m_poseIdle = WeaponBobOffset{};
	if (!Enabled()) return ShakePose{};

	++m_advances;

	// A fresh sample is a seek or a spectator switch. Drop everything.
	if (in.fresh) {
		Reset();
		return ShakePose{};
	}

	const double dt = in.dt;

	// The view angles interpolate each render frame, including frames with raw demo dt=0.
	// Aim uses the rendered-angle interval, without the oscillator's phase correction.
	// The recorder supplies exact demo intervals through aimDt.
	const double phaseDt = in.phaseDt;
	if (WeaponBobAimAdvance(m_aimState, in.aimDt, in.pitch, in.yaw, m_aimVelPitch, m_aimVelYaw))
		m_swayState = WeaponBobSwayState{};

	// ---- movement signals, smoothed the way ARC9 smooths them ----
	// ARC9: ViewModelBobVelocity approaches at ft*10000 -- effectively instant. The stack's
	// planar velocity is already low-passed, so a short half-life here only rounds off tick
	// quantisation without adding lag of its own.
	const double rawSpeedFrac = Clamp(in.speed / kWeaponBobReferenceSpeed, 0.0, 1.0);
	m_speedFrac = Approach(m_speedFrac, rawSpeedFrac, dt, 0.03);

	// Sprint branch blend. CS2 has no sprint key; a full-speed run IS the sprint. Blends in
	// over the top of the speed range so a walk stays on the walk figure.
	const double rawSprint = Clamp((m_speedFrac - 0.62) / (0.92 - 0.62), 0.0, 1.0);
	m_sprint = Approach(m_sprint, rawSprint, dt, 0.10);

	// Airborne, crouched, and the vertical speed the jump term reads. With the pawn resolved these
	// come straight from the engine (FL_ONGROUND / FL_DUCKING / its own velocity): the eye moves up
	// and down on a staircase, over a weapon lying on the floor or off a kerb without the player
	// leaving the ground or ducking, and reading those as a jump or a crouch is what made the bob
	// thin out and slow on stairs and lurch over floor weapons. Without a pawn the position-derived
	// estimate stands in, with the part of the climb a slope explains taken out (WeaponBobMath.h).
	double rawAir, rawCrouch, jumpVz;
	if (in.haveBody) {
		m_airTime = WeaponBobAirTimeAdvance(m_airTime, dt, in.onGround);
		rawAir = WeaponBobAirborneTarget(m_airTime);
		rawCrouch = in.ducking ? 1.0 : 0.0;
		// On the ground the vertical velocity is whatever the step or slope under the feet is doing.
		jumpVz = in.onGround ? 0.0 : in.bodyVz;
	} else {
		m_airTime = 0.0;
		jumpVz = WeaponBobFreeVz(in.vz, in.speed);
		rawAir = WeaponBobAirborneEstimate(in.vz, in.speed);
		// The eye-height baseline; a full CS2 duck drops the eye ~18 units.
		rawCrouch = Clamp(-in.eyeRise / 18.0, 0.0, 1.0);
	}
	m_airborne = Approach(m_airborne, rawAir, dt, kAirborneHalfLife);
	m_crouch = Approach(m_crouch, rawCrouch, dt, 0.10);

	// ARC9 sidemove: (velocity . right / maxSpeed) * 4 * (1.5 - sightamount), then lerped at 8/s.
	// sightamount for hipfire is -0.1 (see WeaponBobMath), so the factor is 1.6.
	const double yawRad = in.yaw * kDeg;
	const double rightX = std::sin(yawRad);
	const double rightY = -std::cos(yawRad);
	const double lateral = (in.vx * rightX + in.vy * rightY) / kWeaponBobReferenceSpeed;
	const double rawSide = Clamp(lateral, -1.0, 1.0) * 4.0 * 1.6;
	m_sideMove = Approach(m_sideMove, rawSide, dt, kSideHalfLife);

	const double rawJump = WeaponBobJumpMove(jumpVz);
	m_jumpMove = Approach(m_jumpMove, rawJump, dt, kJumpHalfLife);

	// ---- clocks, on the DEMO clock ----
	// The oscillators take `phaseDt`, not `dt`. Both are demo time and both sum to the same
	// total across any tick, but `dt` is 0 on a render frame that shares a demo tick -- which
	// froze the figure and made the gun repeat a frame and then jump. At 110 fps against a
	// 64-tick demo that is roughly 40% of frames, and because the frame rate drifts the
	// hold/step pattern is uneven: the reported jitter. The smoothed inputs above stay on `dt`,
	// because a held frame carries no new sample for them to chase.
	const double rate = m_rate / 100.0;
	m_phase = WeaponBobAdvancePhase(m_phase, phaseDt * rate, m_speedFrac, m_airborne);
	m_stepArg = WeaponBobAdvanceStepArg(m_stepArg, phaseDt * rate, m_speedFrac, m_airborne, m_crouch);
	if (phaseDt > 0.0) m_idleClock += phaseDt;
	WeaponBobSwayAdvance(m_swayState, in.aimDt, m_aimVelPitch, m_aimVelYaw);

	// Which weapon is on screen, for THIS module's own profile and hinge lookup below.
	// Resolved every frame rather than cached on a weapon-switch event: in demo playback the
	// spectated player changes without any event we own, and a stale class would put the rifle
	// hinge on a knife -- the exact artifact this exists to fix.
	//
	// It deliberately does NOT publish to the pivot stores any more. This function returns early
	// when Weapon Bob is off and again on a fresh sample (a spectator switch), so publishing from
	// here left the panel's pivot strip stale in exactly the cases that matter. One publisher
	// owns that now, in CameraShake_RunFrame, which runs unconditionally.
	{
		std::string live;
		if (CameraShake_SpectatedWeapon(live) && !live.empty()) {
			m_activeWeapon = live;
			m_activeClass = WeaponPivotClassOf(live);
		}
	}
	const WeaponBobClassProfile& profile = m_profiles[(int)m_activeClass];
	const WeaponPivotPoint hinge = WeaponPivotBobRef().Resolve(m_activeClass);

	// ADS is fed zero: CS2 has no true aim-down-sights outside scoped snipers.
	constexpr double kAds = 0.0;

	m_poseBob = WeaponBobEvaluate(m_phase, m_stepArg, m_speedFrac, m_sprint, m_airborne, m_crouch,
		kAds, m_sideMove, m_jumpMove, m_sway, m_tilt, m_amount, profile);
	// Look sway and idle are gated by Amount too (it is the master), but scaled by their own
	// knobs rather than by Sway/Tilt, which belong to the walk figure.
	const double master = m_amount / 100.0;
	m_poseSway = WeaponBobSwayEvaluate(m_swayState, m_lookSway * master, m_inertia * master, kAds,
		hinge);
	m_poseIdle = WeaponBobIdleEvaluate(m_idleClock, kAds, m_idle * master);

	m_pose = m_poseBob;
	m_pose.Add(m_poseSway);
	m_pose.Add(m_poseIdle);

	if (MvmDebugLog_Active()) {
		static unsigned s_throttle = 0;
		if ((s_throttle++ % 15) == 0) {
			MvmDebugLog_Linef("weaponbob.frame",
				"dt=%.4f aimDt=%.4f phaseDt=%.4f phase=%.3f arg=%.2f speed=%.1f frac=%.3f sprint=%.2f air=%.2f crouch=%.2f "
				"side=%.2f jump=%.2f aimVel=(%.1f %.1f) sway=(%.2f %.2f %.2f) "
				"bob=(%.3f %.3f %.3f | %.2f %.2f %.2f) look=(%.3f %.3f %.3f | %.2f %.2f %.2f) "
				"idle=(%.3f | %.2f) steps=%llu cls=%s(w%.2f s%.2f h%.1f) wep=%s",
				dt, in.aimDt, phaseDt, m_phase, m_stepArg, in.speed, m_speedFrac, m_sprint, m_airborne, m_crouch,
				m_sideMove, m_jumpMove, m_aimVelPitch, m_aimVelYaw,
				m_swayState.pitch, m_swayState.yaw, m_swayState.inertiaY,
				m_poseBob.fwd, m_poseBob.right, m_poseBob.up,
				m_poseBob.pitch, m_poseBob.yaw, m_poseBob.roll,
				m_poseSway.fwd, m_poseSway.right, m_poseSway.up,
				m_poseSway.pitch, m_poseSway.yaw, m_poseSway.roll,
				m_poseIdle.up, m_poseIdle.roll,
				(unsigned long long)m_footsteps,
				WeaponBobClassName(m_activeClass), profile.walkMult, profile.sprintMult,
				hinge.fwd, m_activeWeapon.empty() ? "-" : m_activeWeapon.c_str());
		}
	}

	// The camera's share. Position gets a smaller share than rotation: moving the view ORIGIN
	// is what reads as view bob and makes people sick.
	const double share = m_cameraShare / 100.0;
	ShakePose cam;
	cam.right = m_pose.right * share * 0.35;
	cam.up = m_pose.up * share * 0.35;
	cam.fwd = m_pose.fwd * share * 0.35;
	cam.pitch = m_pose.pitch * share;
	cam.yaw = m_pose.yaw * share;
	cam.roll = m_pose.roll * share;
	return cam;
}

void WeaponBob::WeaponPose(double& fwd, double& right, double& up,
	double& pitch, double& yaw, double& roll) const
{
	// Guarded on Enabled() rather than trusting the cached pose: when every shake layer is off
	// the stack early-returns before Advance runs, and a stale m_pose would make the `state`
	// readout report motion for a disabled module.
	if (!Enabled()) {
		fwd = right = up = pitch = yaw = roll = 0.0;
		return;
	}
	fwd = m_pose.fwd;
	right = m_pose.right;
	up = m_pose.up;
	pitch = m_pose.pitch;
	yaw = m_pose.yaw;
	roll = m_pose.roll;
}

void WeaponBob::AppendStateJson(std::ostringstream& o) const {
	o << ",\"wbOn\":" << (m_enabled ? "true" : "false")
		<< ",\"wbAmount\":" << (int)std::lround(m_amount)
		<< ",\"wbSway\":" << (int)std::lround(m_sway)
		<< ",\"wbTilt\":" << (int)std::lround(m_tilt)
		<< ",\"wbLook\":" << (int)std::lround(m_lookSway)
		<< ",\"wbInertia\":" << (int)std::lround(m_inertia)
		<< ",\"wbIdle\":" << (int)std::lround(m_idle)
		<< ",\"wbCam\":" << (int)std::lround(m_cameraShare)
		<< ",\"wbStep\":" << (int)std::lround(m_stepSync)
		<< ",\"wbRate\":" << (int)std::lround(m_rate)
		<< ",\"wbClass\":\"" << WeaponBobClassName(m_activeClass) << "\"";
}

std::string WeaponBob::DebugStateJson() const {
	double pFwd = 0.0, pRight = 0.0, pUp = 0.0, pPitch = 0.0, pYaw = 0.0, pRoll = 0.0;
	WeaponPose(pFwd, pRight, pUp, pPitch, pYaw, pRoll);
	std::ostringstream o;
	o << "{\"enabled\":" << (m_enabled ? "true" : "false")
		<< ",\"amount\":" << m_amount
		<< ",\"sway\":" << m_sway
		<< ",\"tilt\":" << m_tilt
		<< ",\"lookSway\":" << m_lookSway
		<< ",\"inertia\":" << m_inertia
		<< ",\"idle\":" << m_idle
		<< ",\"cameraShare\":" << m_cameraShare
		<< ",\"stepSync\":" << m_stepSync
		<< ",\"rate\":" << m_rate
		<< ",\"class\":\"" << WeaponBobClassName(m_activeClass) << "\""
		<< ",\"weapon\":\"" << m_activeWeapon << "\""
		<< ",\"classWalk\":" << m_profiles[(int)m_activeClass].walkMult
		<< ",\"classSprint\":" << m_profiles[(int)m_activeClass].sprintMult
		<< ",\"classHinge\":[" << WeaponPivotBobRef().ResolveActive().fwd << ","
			<< WeaponPivotBobRef().ResolveActive().right << ","
			<< WeaponPivotBobRef().ResolveActive().up << "]"
		<< ",\"phase\":" << m_phase
		<< ",\"speedFrac\":" << m_speedFrac
		<< ",\"sprint\":" << m_sprint
		<< ",\"airborne\":" << m_airborne
		<< ",\"crouch\":" << m_crouch
		<< ",\"sideMove\":" << m_sideMove
		<< ",\"aimVel\":[" << m_aimVelPitch << "," << m_aimVelYaw << "]"
		<< ",\"swayState\":[" << m_swayState.pitch << "," << m_swayState.yaw << ","
			<< m_swayState.roll << "," << m_swayState.inertiaP << "," << m_swayState.inertiaY << "]"
		<< ",\"pos\":[" << pFwd << "," << pRight << "," << pUp << "]"
		<< ",\"ang\":[" << pPitch << "," << pYaw << "," << pRoll << "]"
		<< ",\"bobAng\":[" << m_poseBob.pitch << "," << m_poseBob.yaw << "," << m_poseBob.roll << "]"
		<< ",\"lookAng\":[" << m_poseSway.pitch << "," << m_poseSway.yaw << "," << m_poseSway.roll << "]"
		<< ",\"advances\":" << m_advances
		<< ",\"footsteps\":" << m_footsteps
		<< ",\"resets\":" << m_resets
		<< "}";
	return o.str();
}

// ================================ console ===================================================

namespace {

bool ParsePercent(const char* v, double& out) {
	if (!v || !*v) return false;
	char* end = nullptr;
	const double d = std::strtod(v, &end);
	if (end == v) return false;
	out = d;
	return true;
}

} // namespace

void WeaponBob_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	WeaponBob& wb = WeaponBobRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";

	if (0 == _stricmp(action, "on") || 0 == _stricmp(action, "off")
		|| 0 == _stricmp(action, "toggle")) {
		const bool on = (0 == _stricmp(action, "toggle")) ? !wb.Enabled()
			: (0 == _stricmp(action, "on"));
		wb.SetEnabled(on);
		advancedfx::Message("mirv_filmmaker: weapon bob = %s.\n", on ? "on" : "off");
		return;
	}
	if (0 == _stricmp(action, "reset")) {
		wb.ResetSettings();
		advancedfx::Message("mirv_filmmaker: weapon bob reset to defaults.\n");
		return;
	}
	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[weaponbob][state] %s\n", wb.DebugStateJson().c_str());
		return;
	}

	struct Knob { const char* name; void (WeaponBob::*set)(double); double (WeaponBob::*get)() const; const char* label; };
	static const Knob kKnobs[] = {
		{ "amount",   &WeaponBob::SetAmount,      &WeaponBob::Amount,      "amount" },
		{ "sway",     &WeaponBob::SetSway,        &WeaponBob::Sway,        "sway" },
		{ "tilt",     &WeaponBob::SetTilt,        &WeaponBob::Tilt,        "tilt" },
		{ "looksway", &WeaponBob::SetLookSway,    &WeaponBob::LookSway,    "look sway" },
		{ "inertia",  &WeaponBob::SetInertia,     &WeaponBob::Inertia,     "inertia" },
		{ "idle",     &WeaponBob::SetIdle,        &WeaponBob::Idle,        "idle breath" },
		{ "camera",   &WeaponBob::SetCameraShare, &WeaponBob::CameraShare, "camera share" },
		{ "stepsync", &WeaponBob::SetStepSync,    &WeaponBob::StepSync,    "step sync" },
		{ "rate",     &WeaponBob::SetRate,        &WeaponBob::Rate,        "rate" },
	};
	// Per-class knobs: `class <name> <walk> [sprint]` and `hinge <name> <fwd> <right> <up>`.
	if (0 == _stricmp(action, "class") || 0 == _stricmp(action, "hinge")) {
		const char* cname = (argc >= 4) ? args->ArgV(3) : "";
		WeaponBobClass c = WeaponBobClass::Rifle; bool found = false;
		for (int i = 0; i < 7 && !found; ++i)
			if (0 == _stricmp(cname, WeaponBobClassName((WeaponBobClass)i))) { c = (WeaponBobClass)i; found = true; }
		if (!found) {
			advancedfx::Message("mirv_filmmaker: unknown weapon class '%s'. "
				"Use rifle|smg|pistol|shotgun|sniper|knife|utility.\n", cname);
			return;
		}
		if (0 == _stricmp(action, "class")) {
			double w = 0.0, sp = 0.0;
			if (argc >= 5 && ParsePercent(args->ArgV(4), w)) {
				wb.SetClassWalk(c, w);
				if (argc >= 6 && ParsePercent(args->ArgV(5), sp)) wb.SetClassSprint(c, sp);
			}
			// The hinge is shared state now: report it from the pivot store, which owns it.
			advancedfx::Message("mirv_filmmaker: weapon bob class %s -- walk x%.2f sprint x%.2f "
				"(active class right now: %s, hinge %.1f fwd)\n",
				WeaponBobClassName(c), w, sp,
				WeaponBobClassName(wb.ActiveClass()), WeaponPivotBobRef().ResolveActive().fwd);
			return;
		}
		double f = 0.0, r = 0.0, u = 0.0;
		if (argc >= 7 && ParsePercent(args->ArgV(4), f) && ParsePercent(args->ArgV(5), r)
			&& ParsePercent(args->ArgV(6), u)) {
			wb.SetClassHinge(c, f, r, u);
			advancedfx::Message("mirv_filmmaker: weapon bob hinge %s = (%.1f fwd, %.1f right, %.1f up).\n",
				WeaponBobClassName(c), f, r, u);
		} else {
			advancedfx::Message("%s weaponbob hinge <class> <fwd> <right> <up> - rotation point, "
				"camera-local units. Negative fwd puts it BEHIND the eye (knives).\n", cmd);
		}
		return;
	}
	if (0 == _stricmp(action, "classmult")) {
		double w = 1.0, sp = 1.0;
		if (argc >= 5 && ParsePercent(args->ArgV(3), w) && ParsePercent(args->ArgV(4), sp)) {
			wb.SetClassMult(w, sp);
			advancedfx::Message("mirv_filmmaker: weapon bob class multipliers scaled by "
				"walk x%.2f / sprint x%.2f.\n", w, sp);
		} else {
			advancedfx::Message("%s weaponbob classmult <walk> <sprint> - scale every class at once "
				"(0.5 0.1 matches the Modern Warfare reference).\n", cmd);
		}
		return;
	}

	double v = 0.0;
	for (const Knob& k : kKnobs) {
		if (0 == _stricmp(action, k.name) && ParsePercent(value, v)) {
			(wb.*k.set)(v);
			advancedfx::Message("mirv_filmmaker: weapon bob %s = %.0f%%.\n", k.label, (wb.*k.get)());
			return;
		}
	}

	advancedfx::Message(
		"%s weaponbob on|off|toggle - the carried-weapon feel (moves the WEAPON, not the view).\n"
		"%s weaponbob amount|sway|tilt <0-150> - walk bob: master / position / rotation.\n"
		"%s weaponbob looksway|inertia <0-150> - the gun lags and rotates behind the aim.\n"
		"%s weaponbob idle <0-150> - at-rest breathing drift.\n"
		"%s weaponbob camera <0-100> - how much of it the camera also receives.\n"
		"%s weaponbob stepsync <0-100> - pull the walk clock onto real footfalls.\n"
		"%s weaponbob rate <25-400> - walk-cycle speed without changing amplitude.\n"
		"%s weaponbob class <rifle|smg|pistol|shotgun|sniper|knife|utility> <walk> [sprint]\n"
		"%s weaponbob hinge <class> <fwd> <right> <up> - where the weapon rotates about.\n"
		"%s weaponbob classmult <walk> <sprint> - scale every class at once.\n"
		"%s weaponbob state|reset\n",
		cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

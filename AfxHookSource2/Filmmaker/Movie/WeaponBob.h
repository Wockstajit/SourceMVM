#pragma once

#include "CameraShakeTypes.h"
#include "WeaponBobMath.h"

#include <sstream>
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// Weapon Bob: everything that makes the first-person weapon feel CARRIED. Three terms, all from
// ARC9 (the base the "ARC9 Modern Warfare 2019" pack runs on unmodified -- see WeaponBobMath.h):
//
//   walk bob     -- the gait figure while moving
//   look sway    -- the gun lags behind the aim, rotating about a hinge on the weapon, with a
//                   slower "inertia" overshoot on top. This is the turn-and-rotate the user
//                   asked for; v1 had no aim-driven term at all.
//   idle breath  -- slow at-rest drift so a standing gun is never pinned
//
// NOT view bob. The GoldSrc View Bob modifier (camera + matching viewmodel follow) was removed
// 2026-09-01 and is not coming back. This moves only the weapon, through the same
// CalcViewModelView input hook Weapon Recoil uses, so CS2's animation composes on top.
//
// Separate module from WeaponRecoil, exactly as HandheldCam is: recoil is an impulse response to
// a discrete event, this is a continuous function of movement and aim. They share the transport
// (CameraShakeStack sums both into the one ViewFxVm_SetWeaponKick write per frame) and the
// stack's pose tracker, and both hinge the gun about the same point on the weapon.
//
// Gait and idle use the smoothed demo phase clock. Look sway uses the rendered aim sample
// interval at demo speed during playback, and exact demo dt during export. Pause holds still.
class WeaponBob {
public:
	WeaponBob();

	void SetEnabled(bool on);
	bool Enabled() const { return m_enabled && m_amount > 0.0; }

	// Back to constructed defaults. Not persisted by the settings store (same as WeaponRecoil),
	// so it owns its own reset.
	void ResetSettings();

	// ---- walk bob. 0-150%, 100% = ARC9 style 0 at reference strength ----
	void SetAmount(double percent);  // master, gates EVERYTHING below
	void SetSway(double percent);    // bob position channels
	void SetTilt(double percent);    // bob rotation channels
	double Amount() const { return m_amount; }
	double Sway() const { return m_sway; }
	double Tilt() const { return m_tilt; }

	// ---- look sway. 0-150%, 100% = ARC9 GetViewModelSway / GetViewModelInertia at 100 fps ----
	void SetLookSway(double percent);  // the fast aim lag
	void SetInertia(double percent);   // the slow heavy overshoot
	double LookSway() const { return m_lookSway; }
	double Inertia() const { return m_inertia; }

	// ---- idle breath. 0-150% ----
	void SetIdle(double percent);
	double Idle() const { return m_idle; }

	// 0-100%: how much of the whole figure the CAMERA also receives. Small by design.
	void SetCameraShare(double percent);
	double CameraShare() const { return m_cameraShare; }

	// 0-100%: how hard footfalls pull the walk clock onto the player's real cadence.
	void SetStepSync(double percent);
	double StepSync() const { return m_stepSync; }

	// 25-400%: walk-cycle rate without changing amplitude.
	void SetRate(double percent);
	double Rate() const { return m_rate; }

	// Per-weapon-class scaling. A knife is held at the shoulder and a rifle out in front, so one
	// amplitude and one rotation hinge cannot serve both -- see the measured anchors in
	// WeaponBobMath.h. Resolved every frame from the weapon the spectated player is holding.
	WeaponBobClass ActiveClass() const { return m_activeClass; }
	double DebugAirborne() const { return m_airborne; }
	double DebugCrouch() const { return m_crouch; }
	double DebugSpeedFrac() const { return m_speedFrac; }
	double DebugJump() const { return m_jumpMove; }
	const WeaponBobClassProfile& ActiveProfile() const { return m_profiles[(int)m_activeClass]; }
	void SetClassWalk(WeaponBobClass c, double mult);
	void SetClassSprint(WeaponBobClass c, double mult);
	void SetClassHinge(WeaponBobClass c, double fwd, double right, double up);
	// Scales EVERY class's walk/sprint multiplier at once (0.5 / 0.1 reproduces MW2019's own).
	void SetClassMult(double walkMult, double sprintMult);

	// Drops the clocks and every smoothed signal (seek, spectator switch, demo change, enable).
	void Reset();

	// Advances one frame and returns the CAMERA's share. WeaponPose() gives the full weapon
	// transform from the same advance, so gun and camera can never disagree about the frame.
	ShakePose Advance(const ShakeInput& in);

	// This frame's weapon offset: camera-basis position (world units) + rotation (degrees,
	// engine convention). Zero while disabled, whether or not Advance ran.
	void WeaponPose(double& fwd, double& right, double& up,
		double& pitch, double& yaw, double& roll) const;

	// A footstep from the spectated player. Pulls the walk clock toward the footfall.
	void OnFootstep();

	bool SettingsAreDefault() const;
	void AppendStateJson(std::ostringstream& o) const;
	std::string DebugStateJson() const;

private:
	bool m_enabled = false;
	double m_amount = 100.0;
	double m_sway = 100.0;
	double m_tilt = 100.0;
	double m_lookSway = 100.0;
	double m_inertia = 100.0;
	double m_idle = 100.0;
	double m_cameraShare = 12.0;
	double m_stepSync = 60.0;
	double m_rate = 100.0;
	WeaponBobClassProfile m_profiles[7];

	// ---- live state ----
	WeaponBobClass m_activeClass = WeaponBobClass::Rifle;
	std::string m_activeWeapon;
	double m_phase = 0.0;       // walk clock, seconds
	double m_stepArg = 0.0;     // walk CARRIER, radians (see WeaponBobAdvanceStepArg)
	double m_idleClock = 0.0;   // idle-breath clock, seconds (demo time, resets on Reset)
	double m_speedFrac = 0.0;   // ARC9 ViewModelBobVelocity / reference, smoothed
	double m_sprint = 0.0;      // smoothed blend into the sprint branch
	double m_airborne = 0.0;    // ARC9 ViewModelNotOnGround, smoothed 0..1
	double m_airTime = 0.0;     // seconds continuously off the ground (pawn state only)
	double m_crouch = 0.0;      // smoothed 0..1
	double m_sideMove = 0.0;    // ARC9 smoothsidemove
	double m_jumpMove = 0.0;    // ARC9 smoothjumpmove
	WeaponBobSwayState m_swayState;

	// Aim tracking follows the interpolated playback clock, including held demo ticks.
	WeaponBobAimState m_aimState;
	double m_aimVelPitch = 0.0, m_aimVelYaw = 0.0; // deg/s, this frame

	WeaponBobOffset m_pose;      // this frame's summed weapon offset
	WeaponBobOffset m_poseBob;   // components kept for the debug log
	WeaponBobOffset m_poseSway;
	WeaponBobOffset m_poseIdle;

	unsigned long long m_advances = 0;
	unsigned long long m_footsteps = 0;
	unsigned long long m_resets = 0;
};

WeaponBob& WeaponBobRef();

// Console: "mirv_filmmaker weaponbob ...".
void WeaponBob_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

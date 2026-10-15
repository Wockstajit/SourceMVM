#pragma once

// Shared value types for the camera-shake layer stack (HandheldCam / WeaponRecoil).
//
// Deliberately a tiny standalone header so each effect module depends only on this contract and
// never on CameraShakeStack.h (or on each other). The stack owns the pose tracker, the
// camera-mode gate and the summation; the modules own nothing but their own maths.
//
// Angles are DEGREES. Offsets are world units expressed in the CAMERA'S OWN BASIS -- the stack
// rotates them into world space with the final view angles, matching the source addons'
// `pos + ang:Right()*rt + ang:Up()*up + ang:Forward()*fwd`.

namespace Filmmaker {

// Every intensity knob in this subsystem is a single continuous 0-150% slider (0 = off,
// 100 = the reference tuning ported from the source addon), matching ViewFx's convention so the
// Config panel can drive all of them with one widget.
constexpr double kShakeMaxIntensity = 150.0;

// The frame's camera state, computed once by CameraShakeStack and handed to every module.
struct ShakeInput {
	double demoTime = 0.0; // demo playback clock, seconds. Drives ALL phase clocks so an
	                       // exported movie is reproducible frame-for-frame and a paused demo
	                       // holds a still frame. Jumps on seek (see `fresh`).
	double dt = 0.0;       // seconds since the previous accepted sample; 0 on a same-tick hold.
	double phaseDt = 0.0;  // demo seconds for OSCILLATOR phase only. Same as `dt` across any tick
	                       // boundary, but subdivided so a same-tick hold still advances instead
	                       // of repeating the previous pose -- that staircase is the Weapon Bob
	                       // jitter. Smoothers of tick-sampled signals (speed, crouch, aim
	                       // velocity) must stay on `dt`: they have no new information to chase
	                       // on a held frame. A capture never holds, so this equals `dt` exactly
	                       // while recording. See CameraShakePhaseClockMath.h.
	double aimDt = 0.0;    // render-angle sample interval in demo seconds; no oscillator catch-up.
	double wallDt = 0.0;   // WALL-clock seconds since the previous Apply call, clamped small.
	                       // ONLY for interactive previews that must animate on a paused demo
	                       // (the recoil `test` kicks). Real playback effects must never
	                       // integrate on this -- it would break render determinism.
	double x = 0.0, y = 0.0, z = 0.0;              // camera origin this frame
	double pitch = 0.0, yaw = 0.0, roll = 0.0;     // camera angles this frame
	double vx = 0.0, vy = 0.0; // smoothed world-space planar velocity, units/sec
	double speed = 0.0;    // |vx, vy|, units/sec
	double vz = 0.0;       // smoothed vertical speed, units/sec (+up)
	double eyeRise = 0.0;  // eye Z relative to a slow baseline, units. Negative = crouched.
	// What the engine says the viewed pawn is doing, when the pawn could be resolved. The position
	// derived speed/vz/eyeRise above cannot tell a staircase or a weapon on the floor from a jump or
	// a crouch (all of them move the eye vertically), so anything that needs GROUND or DUCK state
	// reads these and falls back to the derived estimate only when haveBody is false.
	bool haveBody = false;
	bool onGround = true;  // FL_ONGROUND
	bool ducking = false;  // FL_DUCKING
	double bodyVz = 0.0;   // the pawn's own vertical velocity, units/sec (+up)
	bool fresh = false;    // first sample after a reset (seek / spectator switch / big gap).
	                       // Modules must not integrate anything on a fresh sample.
};

// A module's contribution for this frame. All-zero means "nothing to add".
struct ShakePose {
	double right = 0.0, up = 0.0, fwd = 0.0; // camera-basis position offset, world units
	double pitch = 0.0, yaw = 0.0, roll = 0.0; // degrees
	double fov = 0.0;                          // degrees, added to the view FOV

	bool Any() const {
		return right != 0.0 || up != 0.0 || fwd != 0.0
			|| pitch != 0.0 || yaw != 0.0 || roll != 0.0 || fov != 0.0;
	}

	void Add(const ShakePose& o) {
		right += o.right; up += o.up; fwd += o.fwd;
		pitch += o.pitch; yaw += o.yaw; roll += o.roll;
		fov += o.fov;
	}
};

// Which camera system owns the pose this frame. Classified in main.cpp from the override stack
// and passed to CameraShake_Apply, which decides what the shake is allowed to ride on top of.
enum class ShakeCamMode {
	Pov,          // plain engine POV / in-eye spectate -- nothing else claimed the pose
	Attach,       // FollowCamera attach preview
	FreeCam,      // mirv free cam -- hand-flown, never shaken
	CameraPath,   // baked camera path / cam import -- hand-authored, never shaken
	Other         // Rust hook or anything else that claimed the pose
};

} // namespace Filmmaker

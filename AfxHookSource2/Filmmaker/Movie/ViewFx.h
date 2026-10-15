#pragma once

#include "ViewFxTypes.h"
#include "ViewFxWeaponSway.h"
#include "ViewFxWeaponFloat.h"
#include "ViewFxDeadzoneAim.h"

#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

// The ViewFx stack: viewmodel "feel" modifiers layered on top of whatever pose the engine (or
// another Filmmaker camera system) already computed this frame.
//
// This class owns exactly TWO things -- the shared pose tracker and the summation. Every effect
// is its own module with its own knobs and its own maths, and none of them know about each other:
//
//   ViewFxDeadzoneAim  - decoupled viewmodel / free-aim cone                -> pitch, yaw, vm
//   ViewFxWeaponSway   - movement-scaled walk drift on the gun             -> vm
//   ViewFxWeaponFloat  - gun chases the aim, swings and settles            -> vm
//
// The View Dynamics group (strafe roll, turn roll, hipfire/ADS roll scale, idle sway) and View
// Bob were removed 2026-09-01. Turn roll outlived them briefly as a Smooth View sub-effect and
// was removed in turn on 2026-09-02; none of the group had a CS2 equivalent worth keeping.
//
// WHY THE TRACKER IS SHARED. All three need the same derived signals -- smoothed planar velocity,
// wrapped aim deltas, and the "is this a real tick step or a same-tick render frame" decision --
// computed from a pose that can jump arbitrarily on a seek or spectator switch. Computing that
// once here is the same reason CameraShakeStack owns the tracker for HandheldCam/WeaponRecoil,
// and it keeps the modules independent of each other rather than sharing state directly.
//
// APPLICATION SITES (this file owns state + math only, no engine sig-scanning of its own):
//   * pitch / yaw (deadzone camera lag) -- the view-setup trampoline (main.cpp), and ONLY when
//     nothing else already owns the pose that frame (default POV / third-person spectate).
//   * viewmodel offsets                 -- the mirv_viewmodel detour (ViewModel.cpp).

class ViewFx {
public:
	// ---- effect access. Each module owns its own knobs; the stack just hands them out. ----
	ViewFxWeaponSway& WeaponSway() { return m_weaponSway; }
	ViewFxWeaponFloat& WeaponFloat() { return m_weaponFloat; }
	ViewFxDeadzoneAim& DeadzoneAim() { return m_deadzoneAim; }
	const ViewFxWeaponSway& WeaponSway() const { return m_weaponSway; }
	const ViewFxWeaponFloat& WeaponFloat() const { return m_weaponFloat; }
	const ViewFxDeadzoneAim& DeadzoneAim() const { return m_deadzoneAim; }

	// Called once per view-setup frame, ONLY when nothing else already owns the camera pose this
	// frame -- (x, y) is the planar view origin, (pitchDeg, yawDeg) the view angles, all still
	// the engine's own unmodified pose at the call site. Feeds the tracker and returns the pose
	// deltas to ADD this frame. All-zero when the effects are off or right after a reset (seek,
	// spectator switch, big time gap), so a pose jump can never read as a velocity/aim spike.
	ViewFxPoseDeltas TrackAndPoseDeltas(float curTime, double x, double y, double pitchDeg,
		double yawDeg);

	// Total viewmodel offset for this frame (sway + float), in mirv_viewmodel's own axes, ADDED
	// on top of whatever base offset the caller already computed -- never overwrites.
	// nowSeconds is WALL clock (so sway behaves the same whether the demo plays or is paused).
	void SwayOffset(float nowSeconds, float& outX, float& outY, float& outZ);

	// Deadzone viewmodel shift (same axes/contract as SwayOffset), kept separate because
	// ViewModel.cpp applies it at a different point and suppresses it when the true-rotation
	// experiment is engaged.
	void DeadzoneViewmodelShift(float& outX, float& outY, float& outZ);

	// Smoothed planar speed (units/sec) most recently tracked.
	double PlanarSpeed() const { return m_speed; }

	// Diagnostics: main.cpp calls this on view-setup frames where another camera system already
	// owned the pose (so tracking was skipped). Counted so "viewfx state" can show WHY the speed
	// estimate is not updating (gated vs teleport-looping vs simply paused).
	void NoteGatedByOverride() { ++m_dbgGated; }

	std::string DebugStateJson() const;

private:
	void ResetTracker(float curTime, double x, double y, double pitchDeg, double yawDeg);
	// Fills `out` from the effects' HELD values, for a same-tick render frame or a rejected
	// teleport -- recomputing would integrate a bogus sample.
	ViewFxPoseDeltas HeldDeltas() const;

	ViewFxWeaponSway m_weaponSway;
	ViewFxWeaponFloat m_weaponFloat;
	ViewFxDeadzoneAim m_deadzoneAim;

	// ---- shared pose tracker ----
	bool m_haveTrackSample = false;
	float m_lastTrackTime = 0.0f;
	double m_lastX = 0.0, m_lastY = 0.0;
	double m_velX = 0.0, m_velY = 0.0; // smoothed world-space planar velocity
	double m_speed = 0.0;              // |velX, velY|, smoothed
	double m_lastAimPitch = 0.0, m_lastAimYaw = 0.0; // accepted stream's previous aim
	int m_teleportStreak = 0;          // consecutive rejected jumps (see .cpp escape hatch)
	double m_pendingDt = 0.0;          // banked time from no-movement samples (see .cpp)
	unsigned long long m_speedSerial = 0; // bumped whenever m_speed is (re)computed

	// Diagnostics for "viewfx state" (single writer = render thread; racy console reads are fine).
	unsigned long long m_dbgCalls = 0;     // TrackAndPoseDeltas invocations
	unsigned long long m_dbgGapResets = 0; // fresh-sample resets (first call / seek / big gap)
	unsigned long long m_dbgHolds = 0;     // same-tick holds (curTime unchanged)
	unsigned long long m_dbgSteps = 0;     // real integration steps
	unsigned long long m_dbgTeleports = 0; // rejected teleport-sized jumps
	unsigned long long m_dbgGated = 0;     // view-setup frames another system owned the pose
};

ViewFx& ViewFxRef();

// Console command entry: handles "mirv_filmmaker viewfx ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "viewfx").
void ViewFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

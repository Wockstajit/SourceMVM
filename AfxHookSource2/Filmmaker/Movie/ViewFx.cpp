#include "ViewFx.h"

#include "FollowCameraMath.h"
#include "ViewFxVm.h"

#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace Filmmaker {

namespace {

constexpr float kTrackResetGap = 0.25f;  // seconds; a bigger jump EITHER WAY = fresh sample
constexpr double kSmoothHalfTime = 0.10; // velocity smoothing half-time, seconds
// The view-setup clock (g_MirvTime.curtime_get()) advances every RENDER frame, but the
// spectated POV's view origin only moves on demo-TICK boundaries. So most samples show
// dt > 0 with dx == 0 -- integrating those as "velocity 0" hammered the smoothed estimate
// toward zero between ticks (live-measured: speed sawtoothing 10 -> 1e-21 while the player
// sprinted), which killed the roll and kept the walk-bob at zero. Instead, a no-movement
// sample only ACCUMULATES its dt; the next sample with real displacement integrates over the
// whole accumulated window. Only when no movement arrives for kStillTimeout (several ticks)
// is the player treated as genuinely stationary and the velocity decayed.
constexpr double kMoveEpsilon = 0.01;    // units; below this a sample counts as "no movement"
constexpr double kStillTimeout = 0.06;   // seconds (~4 ticks at 64tps) of no movement = truly still
// One-sample view-origin travel farther than this means the POV jumped (spectator switch /
// teleport / a different view rendering through the same hook), NOT real movement.
constexpr double kTeleportDistance = 600.0;

bool NearlyEqual(double a, double b) { return std::fabs(a - b) <= 0.000001; }

} // namespace

ViewFx& ViewFxRef() {
	static ViewFx s_instance;
	return s_instance;
}

void ViewFx::ResetTracker(float curTime, double x, double y, double pitchDeg, double yawDeg) {
	m_haveTrackSample = true;
	m_lastTrackTime = curTime;
	m_lastX = x; m_lastY = y;
	m_velX = m_velY = 0.0; m_speed = 0.0;
	++m_speedSerial; // a reset IS a speed update (to 0); lets sway fade cleanly, not stale-freeze
	m_teleportStreak = 0;
	m_pendingDt = 0.0;
	m_lastAimPitch = pitchDeg; m_lastAimYaw = yawDeg; // aim starts centered (no lag)

	m_weaponSway.Reset();
	m_weaponFloat.Reset();
	m_deadzoneAim.Reset();
}

ViewFxPoseDeltas ViewFx::HeldDeltas() const {
	ViewFxPoseDeltas out;
	out.pitch = m_deadzoneAim.CamDeltaPitch();
	out.yaw = m_deadzoneAim.CamDeltaYaw();
	return out;
}

ViewFxPoseDeltas ViewFx::TrackAndPoseDeltas(float curTime, double x, double y, double pitchDeg,
	double yawDeg) {
	++m_dbgCalls;
	ViewFxPoseDeltas out;

	// Fresh sample on the first call OR any big clock jump -- IN EITHER DIRECTION. A demo
	// seek BACKWARD moves curTime behind m_lastTrackTime; a forward-only gap check missed that,
	// so the same-tick hold below then returned "held" forever (until playback crawled past the
	// pre-seek clock) -- roll/sway looked permanently dead after a forward-then-back scrub.
	const float gap = curTime - m_lastTrackTime;
	if (!m_haveTrackSample || gap > kTrackResetGap || gap < -kTrackResetGap) {
		++m_dbgGapResets;
		ResetTracker(curTime, x, y, pitchDeg, yawDeg);
		return out; // all-zero: never let a seek read as a velocity/aim spike
	}

	// curTime only advances when the demo actually steps; between ticks (and while paused)
	// render frames re-enter with the same clock. Hold the current outputs -- recomputing
	// would integrate zero-velocity samples and flicker the effects off.
	if (curTime <= m_lastTrackTime) {
		++m_dbgHolds;
		return HeldDeltas();
	}

	const double dx = x - m_lastX, dy = y - m_lastY;
	const double dist2 = dx * dx + dy * dy;

	// Outlier rejection. A jump bigger than any real one-tick movement is either (a) a
	// spectator switch/teleport, or (b) a DIFFERENT view rendered through the same view-setup
	// hook this frame (preview scenes, etc.) at a far-away origin. For (b) it is critical to
	// NOT adopt the outlier position/time: adopting it corrupted the tracker so the real POV's
	// next sample also read as a teleport, ping-ponging the estimate to a permanent 0 (the
	// "strafe roll never engages / sway never speeds up" symptom). Rejecting without adopting
	// keeps the accepted stream intact regardless of the per-frame view order. For (a) the
	// streak escape below hard-resets onto the new position after ~20 ticks.
	if (dist2 > (kTeleportDistance * kTeleportDistance)) {
		++m_dbgTeleports;
		if (++m_teleportStreak > 20) { // genuine POV change: adopt the new stream cleanly
			++m_dbgGapResets;
			ResetTracker(curTime, x, y, pitchDeg, yawDeg);
			return out;
		}
		return HeldDeltas();
	}
	m_teleportStreak = 0;

	const double dt = (double)(curTime - m_lastTrackTime);
	m_lastTrackTime = curTime;

	// ---- planar-velocity tracker (feeds weapon sway) ----
	if (dist2 < kMoveEpsilon * kMoveEpsilon) {
		// No displacement: the POV origin is still parked on the previous demo tick's position.
		// Bank the elapsed time and keep the current estimate; only a sustained still period
		// decays the velocity (see kMoveEpsilon/kStillTimeout notes above).
		m_pendingDt += dt;
		if (m_pendingDt >= kStillTimeout) {
			const double alphaStill = FollowHalfTimeAlpha(kSmoothHalfTime, m_pendingDt);
			m_velX -= m_velX * alphaStill;
			m_velY -= m_velY * alphaStill;
			m_speed = std::sqrt(m_velX * m_velX + m_velY * m_velY);
			++m_speedSerial;
			m_pendingDt = 0.0;
			++m_dbgSteps;
		} else {
			++m_dbgHolds;
		}
	} else {
		// Real movement: integrate over the whole window since the last MOVING sample.
		const double window = dt + m_pendingDt;
		m_pendingDt = 0.0;
		m_lastX = x; m_lastY = y;
		++m_dbgSteps;

		const double alpha = FollowHalfTimeAlpha(kSmoothHalfTime, window);
		m_velX += (dx / window - m_velX) * alpha;
		m_velY += (dy / window - m_velY) * alpha;
		m_speed = std::sqrt(m_velX * m_velX + m_velY * m_velY);
		++m_speedSerial;
	}

	// ---- build this frame's shared input, then let each module do its own maths ----
	ViewFxInput in;
	in.dt = dt;
	in.x = x; in.y = y;
	in.pitchDeg = pitchDeg; in.yawDeg = yawDeg;
	in.dPitch = FollowWrapDegrees(pitchDeg - m_lastAimPitch);
	in.dYaw = FollowWrapDegrees(yawDeg - m_lastAimYaw);
	in.velX = m_velX; in.velY = m_velY;
	in.speed = m_speed;
	m_lastAimPitch = pitchDeg; m_lastAimYaw = yawDeg;

	m_weaponFloat.Advance(in);
	m_deadzoneAim.Advance(in);
	out.pitch = m_deadzoneAim.CamDeltaPitch();
	out.yaw = m_deadzoneAim.CamDeltaYaw();

	// True-rotation experiment (ViewFxVm): the weapon must rotate BY +freeAim relative to the
	// lagged camera to point at the true aim. No-op unless a working write site/field was
	// configured via "viewfx vmtest".
	ViewFxVm_SetFreeAim(m_deadzoneAim.FreeAimPitch(), m_deadzoneAim.FreeAimYaw());

	return out;
}

void ViewFx::SwayOffset(float nowSeconds, float& outX, float& outY, float& outZ) {
	ViewFxVmOffset o = m_weaponSway.Offset(nowSeconds, m_speed, m_speedSerial);
	// Weapon float rides on top of the walk-cycle sway.
	o.Add(m_weaponFloat.VmOffset());
	outX = (float)o.x; outY = (float)o.y; outZ = (float)o.z;
}

void ViewFx::DeadzoneViewmodelShift(float& outX, float& outY, float& outZ) {
	outX = outY = outZ = 0.0f;
	if (ViewFxVm_RotationActive()) return; // true rotation engaged: the translation hint is off
	const ViewFxVmOffset o = m_deadzoneAim.VmOffset();
	outX = (float)o.x; outY = (float)o.y; outZ = (float)o.z;
}

std::string ViewFx::DebugStateJson() const {
	std::ostringstream o;
	o << "{\"swayPct\":" << (float)m_weaponSway.Intensity()
		// The sway's APPLIED amplitude against the one it is chasing. A gap between them is the
		// smoother absorbing a step in the speed estimate -- which is the whole point of it -- and
		// `swayStale` says the speed stream stopped updating (paused demo, or another camera
		// system owning the pose). "Sway looks wrong" is nearly always one of these three
		// disagreeing with what the player is visibly doing.
		<< ",\"swayAmp\":" << (float)m_weaponSway.AppliedAmplitude()
		<< ",\"swayTarget\":" << (float)m_weaponSway.TargetAmplitude()
		<< ",\"swayStale\":" << (m_weaponSway.Stale() ? "true" : "false")
		<< ",\"deadzonePct\":" << (float)m_deadzoneAim.Intensity()
		<< ",\"speed\":" << (float)m_speed
		<< ",\"camLag\":[" << (float)m_deadzoneAim.CamDeltaPitch() << "," << (float)m_deadzoneAim.CamDeltaYaw() << "]"
		<< ",\"calls\":" << m_dbgCalls
		<< ",\"steps\":" << m_dbgSteps
		<< ",\"holds\":" << m_dbgHolds
		<< ",\"gapResets\":" << m_dbgGapResets
		<< ",\"teleports\":" << m_dbgTeleports
		<< ",\"gated\":" << m_dbgGated << "}";
	return o.str();
}

void ViewFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	ViewFx& fx = ViewFxRef();
	const char* action = (argc >= 3) ? args->ArgV(2) : "";
	const char* value = (argc >= 4) ? args->ArgV(3) : "";
	const bool quiet = (argc >= 5 && 0 == _stricmp(args->ArgV(4), "quiet"));

	if (0 == _stricmp(action, "state")) {
		advancedfx::Message("[viewfx][state] %s\n", fx.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(action, "vmtest")) {
		// True viewmodel-rotation experiment (write-site/field probing); see ViewFxVm.h.
		ViewFxVm_RunCommand(argc, args, cmd);
		return;
	}
	// ---- real-unit knobs (degrees / rate constants), each routed to its own module ----
	{
		struct RealKnob {
			const char* action; const char* label;
			void (*set)(ViewFx&, double); double (*get)(const ViewFx&);
		};
		static const RealKnob kReal[] = {
			{ "float", "weapon float tightness",
				[](ViewFx& f, double v) { f.WeaponFloat().SetTightness(v); },
				[](const ViewFx& f) { return f.WeaponFloat().Tightness(); } },
			{ "floatamt", "weapon float amount",
				[](ViewFx& f, double v) { f.WeaponFloat().SetAmount(v); },
				[](const ViewFx& f) { return f.WeaponFloat().Amount(); } },
		};
		for (const RealKnob& k : kReal) {
			if (0 != _stricmp(action, k.action)) continue;
			if (*value) k.set(fx, std::atof(value));
			if (quiet) return;
			advancedfx::Message("mirv_filmmaker: %s = %.2f.\n", k.label, k.get(fx));
			return;
		}
	}

	// ---- deadzone lock-crosshair sub-command (before the intensity knobs, which share "deadzone") ----
	if (0 == _stricmp(action, "deadzone") && argc >= 4 && 0 == _stricmp(args->ArgV(3), "xhair")) {
		const char* v = (argc >= 5) ? args->ArgV(4) : "toggle";
		const bool on = (0 == _stricmp(v, "toggle")) ? !fx.DeadzoneAim().LockCrosshair()
			: (0 == _stricmp(v, "on") || 0 == _stricmp(v, "1"));
		fx.DeadzoneAim().SetLockCrosshair(on);
		advancedfx::Message("mirv_filmmaker: deadzone lock crosshair = %s (when ON, only the gun free-aims - crosshair stays on true aim).\n",
			on ? "ON" : "off");
		return;
	}

	// ---- 0-150% intensity knobs ----
	{
		struct PctKnob {
			const char* action; const char* label;
			void (*set)(ViewFx&, double); double (*get)(const ViewFx&);
		};
		static const PctKnob kPct[] = {
			{ "sway", "weapon sway",
				[](ViewFx& f, double v) { f.WeaponSway().SetIntensity(v); },
				[](const ViewFx& f) { return f.WeaponSway().Intensity(); } },
			{ "deadzone", "aim deadzone (decoupled viewmodel)",
				[](ViewFx& f, double v) { f.DeadzoneAim().SetIntensity(v); },
				[](const ViewFx& f) { return f.DeadzoneAim().Intensity(); } },
		};
		for (const PctKnob& k : kPct) {
			if (0 != _stricmp(action, k.action)) continue;
			double percent = k.get(fx);
			if (*value) percent = (0 == _stricmp(value, "off")) ? 0.0 : std::atof(value);
			k.set(fx, percent);
			// "quiet" (used by the Config sliders, which fire on every drag step): no echo --
			// a console line per drag notch floods the console.
			if (quiet) return;
			const double applied = k.get(fx);
			if (applied > 0.0)
				advancedfx::Message("mirv_filmmaker: %s = %d%%.\n", k.label, (int)std::lround(applied));
			else
				advancedfx::Message("mirv_filmmaker: %s = off.\n", k.label);
			return;
		}
	}

	advancedfx::Message(
		"%s viewfx sway [<0-150>|off] - weapon idle sway (slow drift), as an intensity percent. Movement-gated; stationary = perfectly still.\n"
		"%s viewfx deadzone [<0-150>|off] - decoupled viewmodel: weapon leads inside an aim cone, camera catches up.\n"
		"%s viewfx deadzone xhair [on|off|toggle] - lock crosshair: deadzone only moves the GUN, camera/crosshair stays at true aim.\n"
		"%s viewfx float [<0-40>] - weapon float: how tightly the gun tracks your aim (LOW = floaty/laggy, 0 = off).\n"
		"%s viewfx floatamt [<0-3>] - weapon float: how far the gun swings.\n"
		"%s viewfx state - print the current intensities + tracker diagnostics.\n",
		cmd, cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

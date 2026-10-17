#include "CamPlayback.h"

#include "CameraBridge.h"
#include "ScrubFrameCache.h" // while the world chases a drag, the camera flies the world's own tick
#include "ScrubFrameCache.h" // while the world chases a drag, the camera flies the world's own tick
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>
#include <cmath>

namespace Filmmaker {

namespace {
	// Playback smoothing (fixes slow-timescale jitter + makes Smooth/Linear glide).
	constexpr double kClockRateTau = 0.25;    // s: smooth the demo-time RATE estimate
	constexpr double kClockCorrectTau = 0.30; // s: gently pull the predicted clock to truth
	constexpr double kPoseSmoothTau = 0.06;   // s: low-pass on the output camera pose

	constexpr int kTimingFreeze = 1;          // CameraPath::Timing::Freeze

	// Wrap a degree delta to [-180,180] so angle smoothing always takes the short way.
	double WrapDeg(double d) {
		while (d > 180.0) d -= 360.0;
		while (d < -180.0) d += 360.0;
		return d;
	}
}

double CamPlayback::WallDt() {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	if (m_wallLastQpc == 0) m_wallLastQpc = now.QuadPart;
	double dt = (double)(now.QuadPart - m_wallLastQpc) / (double)freq.QuadPart;
	m_wallLastQpc = now.QuadPart;
	if (dt < 0.0) dt = 0.0; else if (dt > 0.1) dt = 0.1;
	return dt;
}

void CamPlayback::Reset() {
	m_playT = 0.0;
	m_playSeg = 0;
	m_lastLogSeg = -1;
	m_logAccum = 0.0;
	m_wallLastQpc = 0;
	m_liveClock = -1.0;
	m_liveClockRate = -1.0;
	m_prevDemoNow = -1.0;
	m_prevLiveClock = -1.0;
	m_rateDemoAccum = 0.0;
	m_rateWallAccum = 0.0;
	m_poseSmoothInit = false;
	m_engaged = false;
	m_engageApplied = false;
	m_liveEndSettling = false;
	m_liveEndSettleFrames = 0;
}

void CamPlayback::StartPlay(double duration, bool rangeGated, double startTiming) {
	Reset();
	m_duration = duration;
	m_playT = startTiming < 0.0 ? 0.0 : (startTiming > duration ? duration : startTiming);
	m_playing = true;
	m_scrubbing = false;
	m_rangeGated = rangeGated;
}

void CamPlayback::Stop() {
	m_playing = false;
	m_scrubbing = false;
}

void CamPlayback::BeginScrub(double tick) {
	m_playing = false;
	m_scrubbing = true;
	m_scrubTick = tick;
	m_poseSmoothInit = false; // scrub pushes raw poses; drop any stale smoothing state
}

bool CamPlayback::TickPlay(const CamMarkers& mk, const CamPathEval& eval, const Context& ctx) {
	if (!m_playing) {
		if (ctx.debug) {
			static unsigned s_notPlayingTrace = 0;
			if ((s_notPlayingTrace++ % 15) == 0)
				advancedfx::Message("[campath][tick] skipped: playback engine not playing.\n");
		}
		return false;
	}
	const int n = mk.Count();
	if (n < 2 || m_duration <= 0.0) {
		if (ctx.debug)
			advancedfx::Message("[campath][tick] invalid playback data: markers=%d duration=%.3f.\n", n, m_duration);
		return true; // nothing sensible to play -> signal "end" so the facade stops
	}

	const double wallDt = WallDt();
	const bool freeze = (ctx.timing == kTimingFreeze);

	double demoNow = 0.0; g_MirvTime.GetCurrentDemoTime(demoNow);
	int demoTickInt = 0; g_MirvTime.GetCurrentDemoTick(demoTickInt);
	const double demoTick = (double)demoTickInt;
	bool atEnd = false;
	CamPathEval::Pose pose;
	// Where we are on the path, in DEMO TICKS. The per-keyframe shake layer is keyed and clocked
	// off this (never off wall time), which is what makes it identical on every replay/render.
	double shakeTick = 0.0;

	if (!freeze) {
		// PREDICTIVE demo clock in TICK SPACE -- two fixes in one. (a) marker.time can
		// go STALE after retiming, while marker.tick is the axis the editor/timeline /
		// scrubber are all built on; so Live evaluates by TICK to stay locked to the
		// real playhead. (b) the engine's demo tick still arrives COARSE and sometimes
		// reversed at low timescale, so we keep the predict/correct/snap machinery that
		// killed the slow-timescale jitter -- just driven by ticks instead of seconds:
		// (1) estimate the tick RATE (low-passed), (2) ADVANCE at that rate each frame,
		// (3) gently CORRECT toward the true tick, and (4) SNAP on a genuine seek.
		constexpr double kSeekTicks = 16.0; // ~0.25s @ 64tick: a real seek snaps the clock
		if (m_liveClock < 0.0 || std::fabs(demoTick - m_liveClock) > kSeekTicks) {
			// init / seek
			m_liveClock = demoTick; m_liveClockRate = -1.0; m_prevDemoNow = demoTick;
			m_prevLiveClock = demoTick; m_rateDemoAccum = 0.0; m_rateWallAccum = 0.0;
		} else {
			double dDemo = demoTick - m_prevDemoNow; // >= 0 when advancing
			m_prevDemoNow = demoTick;

			// Unbiased rate: accumulate the tick advance AND the wall time across
			// frames, then rate = ticksAdvanced / wallElapsed over the window. Counting
			// the FLAT frames (where the coarse demo tick didn't change) in the
			// denominator is what keeps the estimate honest -- sampling dDemo/wallDt
			// only on the tick frames reads a whole tick landing in one frame as
			// near real-time, so the predictor races ahead and the corrector hauls it
			// back: the visible back-and-forth wiggle.
			if (dDemo > 0.0) m_rateDemoAccum += dDemo; // skip tiny backward blips
			m_rateWallAccum += wallDt;
			if (m_rateWallAccum >= kClockRateTau) {
				double inst = m_rateDemoAccum / m_rateWallAccum;
				m_liveClockRate = (m_liveClockRate < 0.0) ? inst
					: m_liveClockRate + (inst - m_liveClockRate) * 0.5;
				m_rateDemoAccum = 0.0; m_rateWallAccum = 0.0;
			}

			double rate = (m_liveClockRate > 0.0) ? m_liveClockRate : 0.0;
			m_liveClock += rate * wallDt; // predict at the (now unbiased) tick rate
			m_liveClock += (demoTick - m_liveClock) * (1.0 - std::exp(-wallDt / kClockCorrectTau));

			// Monotonic guard: a momentary over-predict + the correction pull must
			// never dip the clock below its last value (would read as the camera
			// stuttering backwards). Holding keeps motion one-directional; a real
			// seek is caught by the snap branch above.
			if (m_liveClock < m_prevLiveClock) m_liveClock = m_prevLiveClock;
			m_prevLiveClock = m_liveClock;
		}
		const double clk = m_liveClock; // predicted demo TICK
		const double tFirst = (double)mk.Front().tick, tLast = (double)mk.Back().tick;
		atEnd = (demoTick >= tLast - 0.5) || (clk >= tLast - 0.5);
		if (atEnd) m_liveEndSettling = true;
		else { m_liveEndSettling = false; m_liveEndSettleFrames = 0; }

		// Range-gated (timeline play): hand the camera back to the normal demo view
		// until the REAL playhead reaches the first marker's tick, so the dolly never
		// moves / holds before its keyframe range. Use the raw demoTick (not the
		// predicted clock) for the gate so the camera can't anticipate the start. Once
		// the range has been reached, keep ownership through the last-key stop frame;
		// otherwise a lagging predicted clock can let the demo roll past the final key
		// while the range gate has already released the view.
		if (m_rangeGated) {
			const bool beforeRange = demoTick < tFirst - 0.5;
			const bool wantEngage = !beforeRange;
			if (!m_engageApplied || wantEngage != m_engaged) {
				CameraBridge_SetFreeCamEnabled(wantEngage);
				m_engaged = wantEngage;
				m_engageApplied = true;
				m_poseSmoothInit = false;
			}
			if (ctx.debug) {
				advancedfx::Message(
					"[campath][gate] demoTick=%.1f first=%.1f last=%.1f wantEngage=%d freecam=%d\n",
					demoTick, tFirst, tLast, wantEngage ? 1 : 0,
					CameraBridge_GetFreeCamEnabled() ? 1 : 0);
			}
			if (beforeRange) {
				m_playSeg = 0;
				return false; // outside the path's tick range (not the end)
			}
		}

		shakeTick = atEnd ? tLast : clk;
		pose = eval.EvalAtTick(mk, shakeTick);
	} else {
		// Freeze: advance along the speed-mode TIMING axis on wall-clock.
		m_playT += wallDt;
		if (m_playT >= m_duration) { m_playT = m_duration; atEnd = true; }
		pose = eval.EvalAtTiming(mk, m_playT);
		// Freeze runs on the TIMING axis, but the shake layer keys BOTH its per-keyframe lookup
		// and its clock off path ticks, so map the evaluated point back onto the tick axis. Same
		// point on the path => same shake, whichever timing mode got us there.
		if (pose.valid && pose.seg >= 0 && pose.seg + 1 < n) {
			const double a = (double)mk.At(pose.seg).tick;
			const double b = (double)mk.At(pose.seg + 1).tick;
			shakeTick = a + (b - a) * pose.p;
		} else if (n > 0) {
			shakeTick = (double)mk.Back().tick;
		}
	}
	m_playSeg = pose.seg;
	m_lastEvalTick.store(shakeTick, std::memory_order_relaxed); // for the view-stage shake layer

	double poseX = 0, poseY = 0, poseZ = 0, posePitch = 0, poseYaw = 0, poseFov = 0;
	bool pushedPose = false;
	bool endSettled = !atEnd || freeze;
	if (pose.valid) {
		// Low-pass the OUTPUT pose: rounds the velocity "corner" at each marker so the
		// move glides through keyframes and mops up any residual clock roughness.
		// Angles smooth along the SHORTEST arc so the +-180 wrap never spins.
		if (!m_poseSmoothInit) {
			m_sX = pose.x; m_sY = pose.y; m_sZ = pose.z;
			m_sPitch = pose.pitch; m_sYaw = pose.yaw; m_sRoll = pose.roll; m_sFov = pose.fov;
			m_poseSmoothInit = true;
		} else {
			const double poseTau = m_liveEndSettling ? 0.025 : kPoseSmoothTau;
			double a = 1.0 - std::exp(-wallDt / poseTau);
			m_sX += (pose.x - m_sX) * a; m_sY += (pose.y - m_sY) * a; m_sZ += (pose.z - m_sZ) * a;
			m_sFov += (pose.fov - m_sFov) * a;
			m_sPitch += WrapDeg(pose.pitch - m_sPitch) * a;
			m_sYaw   += WrapDeg(pose.yaw   - m_sYaw)   * a;
			m_sRoll  += WrapDeg(pose.roll  - m_sRoll)  * a;
		}
		// The pose published here is the AUTHORED move, unshaken. The per-keyframe shake is a
		// frame shaker applied at the view stage (main.cpp), never here: a shaken pose published
		// from the path would be read back by marker placement, the graph editor's defaults and
		// the spectated-pawn match -- which is how a cosmetic wobble ends up permanently baked in.
		CameraBridge_SetCameraPose(m_sX, m_sY, m_sZ, m_sPitch, m_sYaw, m_sRoll, m_sFov);
		pushedPose = true;
		poseX = m_sX; poseY = m_sY; poseZ = m_sZ; posePitch = m_sPitch; poseYaw = m_sYaw; poseFov = m_sFov;
		// Live playback plays a take of the shot: its frames can be kept for a drag back
		// (ScrubFrameCache.h). The settle at the last key holds the camera while the clock stops.
		if (!freeze && !m_liveEndSettling) {
			const double shown[7] = { m_sX, m_sY, m_sZ, m_sPitch, m_sYaw, m_sRoll, m_sFov };
			const double path[7] = { pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll, pose.fov };
			ScrubFrameCache_NoteCamera(shakeTick, shown, path, true);
		}
		if (m_liveEndSettling) {
			++m_liveEndSettleFrames;
			const double posErr = std::sqrt((pose.x - m_sX) * (pose.x - m_sX)
				+ (pose.y - m_sY) * (pose.y - m_sY) + (pose.z - m_sZ) * (pose.z - m_sZ));
			const double angErr = std::fmax(std::fabs(WrapDeg(pose.pitch - m_sPitch)),
				std::fmax(std::fabs(WrapDeg(pose.yaw - m_sYaw)), std::fabs(WrapDeg(pose.roll - m_sRoll))));
			const double fovErr = std::fabs(pose.fov - m_sFov);
			endSettled = (m_liveEndSettleFrames >= 2 && posErr <= 2.0 && angErr <= 0.25 && fovErr <= 0.25)
				|| m_liveEndSettleFrames >= 90;
			if (endSettled) {
				m_sX = pose.x; m_sY = pose.y; m_sZ = pose.z;
				m_sPitch = pose.pitch; m_sYaw = pose.yaw; m_sRoll = pose.roll; m_sFov = pose.fov;
				CameraBridge_SetCameraPose(m_sX, m_sY, m_sZ, m_sPitch, m_sYaw, m_sRoll, m_sFov);
				poseX = m_sX; poseY = m_sY; poseZ = m_sZ; posePitch = m_sPitch; poseYaw = m_sYaw; poseFov = m_sFov;
			}
		}
	}

	// Debug: log on every segment change + a throttled progress line.
	const int seg = pose.seg;
	// Live: camClk is the predicted demo TICK; behind = how many ticks the camera
	// trails the raw playhead. Freeze: camClk is the wall-clock timing cursor (s).
	const double camClk = freeze ? m_playT : m_liveClock;
	const double behind = freeze ? 0.0 : (demoTick - camClk);
	const int tgtSeg = (seg + 1 < n) ? seg + 1 : n - 1;
	const float segSpeed = (ctx.speedMode == 1 /*Constant*/) ? ctx.constSpeed
		: (ctx.speedMode == 2 /*PerSegment*/ ? mk.At(seg).speedMul : 1.0f);
	bool segChanged = (seg != m_lastLogSeg);
	m_logAccum += 1.0;
	if (ctx.debug && (segChanged || m_logAccum >= 15.0 || atEnd || !pushedPose)) {
		advancedfx::Message(
			"[campath][tick] clock=%s seg=%d/%d prog=%.0f%% mk #%d->#%d interp=%s speed=%s x%.2f "
			"camClk=%.2f demoTime=%.2f behind=%+.2f target(tick=%d time=%.2f) "
			"poseValid=%d pushed=%d atEnd=%d rangeGated=%d freecam=%d "
			"pos=(%.1f %.1f %.1f) pitch=%.1f yaw=%.1f fov=%.1f%s.\n",
			(freeze ? "Freeze(wall)" : "Live(replay)"),
			seg + 1, (n > 1 ? n - 1 : 1), pose.p * 100.0,
			seg, tgtSeg, ctx.interpName, ctx.speedModeName, segSpeed,
			camClk, demoNow, behind, mk.At(tgtSeg).tick, mk.At(tgtSeg).time,
			pose.valid ? 1 : 0, pushedPose ? 1 : 0, atEnd ? 1 : 0, m_rangeGated ? 1 : 0,
			CameraBridge_GetFreeCamEnabled() ? 1 : 0,
			poseX, poseY, poseZ, posePitch, poseYaw, poseFov,
			pose.valid ? "" : " (no pose)");
		// [push]: proves a pose is handed to the bridge on a continuing cadence, and reports
		// the free-cam ownership flag the view-setup hook keys on (freecam=0 here would mean
		// the dolly isn't owning the view, so CameraPathOwnsView would not block TrueView).
		advancedfx::Message(
			"[campath][push] live=%d camClk=%.2f seg=%d valid=%d pos=(%.1f %.1f %.1f) fov=%.1f freecam=%d\n",
			freeze ? 0 : 1, camClk, seg, pose.valid ? 1 : 0,
			poseX, poseY, poseZ, poseFov, CameraBridge_GetFreeCamEnabled() ? 1 : 0);
		m_lastLogSeg = seg;
		m_logAccum = 0.0;
	}

	return atEnd && endSettled;
}

void CamPlayback::TickScrub(const CamMarkers& mk, const CamPathEval& eval) {
	if (!m_scrubbing)
		return;
	// Deterministic: evaluate the pure path at the requested tick and push it raw.
	// No predictive clock, no pose low-pass -> identical tick yields identical pose. With the scrub
	// frame cache on, the tick is the world's own while it chases the drag, so the camera and the
	// world in every frame agree and the frame can be kept (ScrubFrameCache.h).
	const double tick = ScrubFrameCache_CameraTick(m_scrubTick);
	CamPathEval::Pose pose = eval.EvalAtTick(mk, tick);
	if (!pose.valid)
		return;
	m_playSeg = pose.seg;
	m_lastEvalTick.store(tick, std::memory_order_relaxed); // view-stage shake previews here
	CameraBridge_SetCameraPose(pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll, pose.fov);
	const double noted[7] = { pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll, pose.fov };
	ScrubFrameCache_NoteCamera(tick, noted);
}

} // namespace Filmmaker

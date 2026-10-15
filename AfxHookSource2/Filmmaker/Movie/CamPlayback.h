#pragma once

// CamPlayback: the playback / preview controller carved out of the old
// CameraPath god-object. It owns everything that is about MOTION OVER TIME:
//   * the predictive Live demo-clock (kills slow-timescale jitter),
//   * the Freeze wall-clock cursor,
//   * the output-pose low-pass (glides through markers), and
//   * the new deterministic SCRUB mode (plan Part C).
//
// It reads poses from CamPathEval (the pure evaluator) and pushes them through
// CameraBridge. Playback uses the smoothed clock + pose low-pass; SCRUB bypasses
// BOTH and pushes the pure EvalAtTick pose, so the same tick always lands on the
// exact same pose.
//
// The editing-mode state machine (Editing / Reposition / Preview*) stays in the
// CameraPath facade; this class is just the engine of playback + scrubbing.

#include "CamPathEval.h"

#include <atomic>

namespace Filmmaker {

class CamPlayback {
public:
	// Cosmetic / mode context for the per-frame playback driver + debug log.
	struct Context {
		int timing = 0;                  // CameraPath::Timing (0 Live, 1 Freeze)
		int speedMode = 0;               // CameraPath::SpeedMode (0 Manual, 1 Constant, 2 PerSegment)
		const char* interpName = "";
		const char* speedModeName = "";
		float constSpeed = 1.0f;
		bool debug = false;              // gate the verbose [campath]/[push] playback logs
	};

	bool Playing() const { return m_playing; }
	bool Scrubbing() const { return m_scrubbing; }
	bool LiveEndSettling() const { return m_liveEndSettling; }
	int Segment() const { return m_playSeg; }
	double ScrubTick() const { return m_scrubTick; }
	// Where the dolly sits on its path, in demo ticks, as of the last TickPlay/TickScrub. The
	// view-stage shake layer (CamPathShake) reads this instead of recomputing the clock: the
	// shake must key off the same point on the path the pose was evaluated at, or a preview and
	// a render of the same frame disagree. Valid only while Playing() or Scrubbing().
	double LastEvalTick() const { return m_lastEvalTick.load(std::memory_order_relaxed); }

	void Reset();                      // clear clock + smoothing (call when (re)arming)
	// Begin playback over [0, duration] on the timing axis. rangeGated (timeline play):
	// in Live mode the dolly only drives the camera once the demo reaches the first
	// marker's tick and releases it (normal demo view) before that / after the last.
	void StartPlay(double duration, bool rangeGated = false, double startTiming = 0.0);
	void Stop();                       // stop playback AND scrub

	void BeginScrub(double tick);      // enter scrub at a tick (facade issues gototick/pause)
	void SetScrubTick(double tick) { m_scrubTick = tick; }
	void EndScrub() { m_scrubbing = false; }

	// Per-frame playback: advance the clock, evaluate, smooth, push the pose.
	// Returns true once the path end is reached (the facade then stops + notifies).
	bool TickPlay(const CamMarkers& markers, const CamPathEval& eval, const Context& ctx);

	// Per-frame scrub: deterministic pure EvalAtTick -> push raw pose (no smoothing).
	void TickScrub(const CamMarkers& markers, const CamPathEval& eval);

private:
	double WallDt(); // clamped wall-clock delta for the playback/preview clocks

	bool m_playing = false;
	bool m_scrubbing = false;
	bool m_rangeGated = false;   // Live: only drive the camera within the marker tick range
	bool m_engaged = false;      // currently driving the free cam (range-gated playback)
	bool m_engageApplied = false;// false => force the free-cam state on the next frame
	bool m_liveEndSettling = false; // Live reached the last key; output pose is catching up
	int m_liveEndSettleFrames = 0;
	double m_scrubTick = 0.0;
	// ATOMIC: written here on the main thread, read from the view-setup trampoline, which is not
	// guaranteed to be the same thread. A plain double would be a data race (and a torn read on
	// any platform where an 8-byte load is not atomic).
	std::atomic<double> m_lastEvalTick{ 0.0 }; // path position of the most recent evaluation (ticks)

	double m_duration = 0.0;
	double m_playT = 0.0;          // Freeze-mode wall-clock cursor on the timing axis

	// Predictive Live demo-clock (smoothed; -1 => uninitialised).
	double m_liveClock = -1.0;
	double m_liveClockRate = -1.0; // low-passed demo-secs per wall-sec
	double m_prevDemoNow = -1.0;   // previous frame's raw demo time
	double m_prevLiveClock = -1.0; // monotonic guard (predicted clock never reverses)
	// Windowed rate estimate. The demo advance and the wall time are BOTH accumulated
	// across frames (including flat ones where the demo didn't tick) so the rate is
	// demoAdvanced/wallElapsed -- the true average -- instead of one tick's worth
	// sampled in a single frame, which biased the old estimate hugely high at low
	// timescale and made the predicted clock oscillate.
	double m_rateDemoAccum = 0.0;
	double m_rateWallAccum = 0.0;

	// Low-passed OUTPUT pose (playback only).
	bool m_poseSmoothInit = false;
	double m_sX = 0.0, m_sY = 0.0, m_sZ = 0.0;
	double m_sPitch = 0.0, m_sYaw = 0.0, m_sRoll = 0.0, m_sFov = 0.0;

	int m_playSeg = 0;
	long long m_wallLastQpc = 0;   // 0 => (re)initialise on next WallDt()
	double m_logAccum = 0.0;       // throttle for the per-frame debug log
	int m_lastLogSeg = -1;
};

} // namespace Filmmaker

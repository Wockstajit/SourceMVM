#pragma once

// Demo aim resampler -- engine glue around DemoAimResamplerMath.h.
//
// Standalone module. It owns its own sampling, clock, settings, command and diagnostics, and
// depends on nothing in SmoothView.
//
// SmoothView applies it to the free-aim RESIDUAL, AFTER the camera pose has been written -- so it
// smooths what the viewmodel and the 3D crosshair are steered by without moving the view. Do not
// move it back onto the camera's anchor; that was measured and it made the 1:1 gap worse in both
// directions (see the note on m_enabled below).
//
// The measurements that motivated it, and why this is a regression rather than a filter or an
// interpolator, are in DemoAimResamplerMath.h.

#include "DemoAimResamplerMath.h"
#include <string>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class DemoAimResampler {
public:
	struct Result {
		bool valid = false;      // false => caller keeps the engine's raw anchor untouched
		double pitch = 0.0;
		double yaw = 0.0;        // wrapped to [-180,180] for the caller
		double pitchRate = 0.0;  // deg/s -- the synthetic "mouse movement" signal
		double yawRate = 0.0;
	};

	void SetEnabled(bool on);
	bool Enabled() const { return m_enabled; }

	// Regression window, milliseconds of DEMO time. Longer rejects more noise but starts to round
	// off genuine acceleration; shorter tracks harder but lets more noise through. The default is
	// about four ticks, which at any frame rate above ~60 gives the quadratic fit enough points to
	// average while staying well inside the duration of a real flick.
	void SetWindowMs(double v);
	double WindowMs() const { return m_windowMs; }

	// How far behind the newest sample the fit is evaluated, in milliseconds. Zero evaluates at
	// the endpoint, which has no latency but the highest noise gain; a little lag buys a
	// noticeably steadier estimate. Cheap in a replay -- the chase filter already spends ~24 ms.
	void SetLagMs(double v);
	double LagMs() const { return m_lagMs; }

	// Feed the engine's per-frame anchor. Returns the reconstructed anchor, or an invalid result
	// (caller keeps the raw value) while disabled, outside a demo, or before the window has filled.
	// Idempotent within a render frame: repeated calls in one frame return the cached result
	// rather than adding a duplicate sample.
	Result Update(double rawPitch, double rawYaw, int povPawnIndex, bool paused);

	void Reset();

	// Bounded per-frame trace, same convention as SmoothView::StartTrace.
	void StartTrace(double seconds);

	std::string DebugStateJson() const;

private:
	void Push(double t, double pitch, double yawContinuous);

	// DEFAULT ON, verified 2026-09-01 with a paired same-tick A/B (seek back, capture OFF, seek
	// back again, capture ON -- so both legs replay the identical recorded motion) on two
	// unrelated flicks in a match730 demo, gun-travel-per-degree (the metric this module exists
	// to fix; live reference 0.0444):
	//   flick 1:  off 0.0651 -> on(90/15) 0.0541 -> on(180/60) 0.0425
	//   flick 2:  off 0.0690 ->                     on(180/60) 0.0403
	// The originally-shipped 90/15 window only closed about a third of the gap; widening to
	// 180/60 (below) closes it fully on both flicks with the live number to spare. Do not
	// re-narrow the window without a paired capture proving it still holds.
	//
	// An earlier version fed it the camera's ANCHOR instead and had to be defaulted off: smoothing
	// the anchor made the chase filter track an estimate rather than the aim, which pushed 85% MORE
	// motion into the viewmodel than live (0.375 vs 0.202) and inverted the camera's phase on the
	// worst 10% of frames (follow fraction -0.440). Same filter, wrong consumer. This module only
	// ever touches the free-aim RESIDUAL, after the camera pose is final -- it cannot move the view.
	bool m_enabled = true;
	// 180 ms / 60 ms (window/lag): wide enough that the quadratic fit averages a genuinely noisy
	// demo-reconstructed flick instead of chasing its per-frame noise. A narrower 90/15 window
	// (the original guess) still let noise dominate the fit; see the A/B above. SmoothView.cpp
	// re-clamps this module's output to the free-aim cone regardless of these values, so a local
	// fit overshooting its own window can never re-open the "gun leaves the cone" bug -- only the
	// jitter magnitude is at stake here, not the hard bound.
	double m_windowMs = 180.0;
	double m_lagMs = 60.0;

	// Per-frame history, oldest first. Sized for the longest window at a high frame rate
	// (250 ms at 300 fps is 75 samples); the window, not the capacity, decides what is used.
	static const int kMaxSamples = 96;
	double m_t[kMaxSamples];
	double m_pitch[kMaxSamples];
	double m_yaw[kMaxSamples];
	int m_count = 0;

	DemoAimClock m_clock;

	// Continuous (unwrapped) yaw accumulator. Fitting wrapped yaw would fling the camera across
	// the +-180 seam.
	bool m_haveYaw = false;
	double m_yawContinuous = 0.0;
	double m_lastRawYaw = 0.0;

	int m_lastTick = 0;
	bool m_haveTick = false;
	int m_povPawnIndex = -1;

	// Per-frame idempotency.
	int m_lastFrame = -1;
	Result m_lastResult;

	// Diagnostics.
	double m_traceLeft = 0.0;
	int m_lastUsed = 0;
	int m_lastDegree = 0;
	unsigned long long m_resets = 0;
	unsigned long long m_framesReconstructed = 0;
};

DemoAimResampler& DemoAimResamplerRef();

// "mirv_filmmaker aimresample ..."
void DemoAimResampler_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

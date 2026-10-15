#include "DemoAimResampler.h"
#include "../../MirvTime.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// Is a demo being PLAYED BACK right now?
//
// Not the same question as "does a demo file object exist". CMirvTime::GetCurrentDemoTick returns
// true whenever GetDemoFile() is non-null, which it is in a live match too -- so using that as the
// demo test silently made this module apply during live play, where the angles already come from
// the player's mouse and there is nothing to reconstruct. Confirmed in-game: a live session
// reported lastTick 2763 and framesReconstructed climbing.
//
// IsPlayingDemo() reports false transiently during a demo_gototick seek (see the note in
// CameraPath.cpp). That is harmless here -- a few observe-only frames across a seek, which is
// already the reset path -- so no debounce is needed.
bool IsDemoPlayback() {
	if (!g_pEngineToClient) return false;
	if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
	return false;
}

} // namespace

DemoAimResampler& DemoAimResamplerRef() { static DemoAimResampler s; return s; }

void DemoAimResampler::SetEnabled(bool on) {
	if (on == m_enabled) return;
	m_enabled = on;
	Reset();
}

void DemoAimResampler::SetWindowMs(double v) {
	m_windowMs = DemoAimClamp(v, 0.0, 250.0);
	Reset();
}

void DemoAimResampler::SetLagMs(double v) {
	m_lagMs = DemoAimClamp(v, 0.0, 120.0);
	Reset();
}

void DemoAimResampler::Reset() {
	m_count = 0;
	m_clock = DemoAimClock();
	m_haveYaw = false;
	m_yawContinuous = 0.0;
	m_lastRawYaw = 0.0;
	m_haveTick = false;
	m_lastTick = 0;
	m_lastFrame = -1;
	m_lastResult = Result();
	m_lastUsed = 0;
	m_lastDegree = 0;
	++m_resets;
}

void DemoAimResampler::StartTrace(double seconds) {
	m_traceLeft = DemoAimClamp(seconds, 0.0, 600.0);
}

void DemoAimResampler::Push(double t, double pitch, double yawContinuous) {
	if (m_count == kMaxSamples) {
		for (int i = 1; i < kMaxSamples; ++i) {
			m_t[i - 1] = m_t[i];
			m_pitch[i - 1] = m_pitch[i];
			m_yaw[i - 1] = m_yaw[i];
		}
		--m_count;
	}
	m_t[m_count] = t;
	m_pitch[m_count] = pitch;
	m_yaw[m_count] = yawContinuous;
	++m_count;
}

DemoAimResampler::Result DemoAimResampler::Update(
	double rawPitch, double rawYaw, int povPawnIndex, bool paused) {
	Result invalid;
	if (!(m_windowMs > 0.0)) return invalid;

	// NOTE: being disabled does NOT short-circuit here. The module keeps sampling, fitting and
	// tracing; only the final apply is skipped (see `apply` below). Returning early on !m_enabled
	// also silenced this module's own trace, which made it impossible to A/B itself -- an off run
	// produced no comparable rows at all. Observability must not depend on the thing being on.

	// In a LIVE match the view angles come from the player's own mouse at render rate -- that is
	// the reference signal this module exists to reproduce, so there is nothing to fix and nothing
	// is applied. Everything else still runs: sampling, the fit and the trace. That is deliberate.
	// The only way to know whether a demo has been made to look like live is to measure BOTH legs
	// through an identical pipeline, and the live leg is the ground truth.
	int tick = 0;
	g_MirvTime.GetCurrentDemoTick(tick);
	const bool inDemo = IsDemoPlayback();

	const double tickLen = (double)g_MirvTime.interval_per_tick_get();
	if (!(tickLen > 0.0) || !std::isfinite(tickLen)) return invalid;
	if (!std::isfinite(rawPitch) || !std::isfinite(rawYaw)) return invalid;

	// Repeated calls within one render frame must not add a duplicate sample or advance the clock
	// twice; both would silently reweight the fit.
	const int frame = g_MirvTime.framecount_get();
	if (frame == m_lastFrame) return m_lastResult;
	m_lastFrame = frame;

	// Switching the spectated player swaps to an unrelated aim history.
	if (povPawnIndex != m_povPawnIndex) {
		m_povPawnIndex = povPawnIndex;
		Reset();
		m_lastFrame = frame;
	}

	// A backward or large forward tick jump is a seek, a scope transition (SmoothView returns
	// early while zoomed, so no samples are taken) or a hitch. Fitting across the gap would sweep
	// the camera through it. Eight ticks still leaves headroom for normal playback down to about
	// 8 fps before this degrades to passthrough. Live has no seeks, and its tick counter is the
	// server's, so this check is demo-only; the input-jump guard below covers both.
	if (inDemo) {
		if (m_haveTick && (tick < m_lastTick || tick - m_lastTick > 8)) {
			Reset();
			m_lastFrame = frame;
		}
		m_haveTick = true;
		m_lastTick = tick;
	}

	// Yaw is accumulated continuously so a +-180 crossing is a small step. Pitch is engine-clamped
	// to about +-89 and never wraps, so it is used directly.
	if (!m_haveYaw) {
		m_haveYaw = true;
		m_yawContinuous = rawYaw;
		m_lastRawYaw = rawYaw;
	} else {
		const double stepYaw = DemoAimWrapDegrees(rawYaw - m_lastRawYaw);
		const double stepPitch = rawPitch - (m_count ? m_pitch[m_count - 1] : rawPitch);
		m_yawContinuous += stepYaw;
		m_lastRawYaw = rawYaw;

		// A genuine cut in the recorded aim -- a round change or a respawn that did not also move
		// the tick or the pawn. Fitting across it would smear the cut into a fast pan.
		//
		// The threshold is deliberately far above anything a hand can produce: 45 degrees in one
		// frame is over 3000 deg/s at 70 fps, where the fastest flick in the logs was 690 deg/s.
		// It is checked on the INPUT for exactly that reason. An earlier version guarded the
		// OUTPUT instead, tripping when the fit lagged the raw value by more than 25 degrees --
		// which a real 1200 deg/s flick reaches on its own, so it would have reset (and visibly
		// popped) during precisely the motion it was supposed to protect.
		if (std::fabs(stepYaw) > 45.0 || std::fabs(stepPitch) > 45.0) {
			Reset();
			m_lastFrame = frame;
			m_haveTick = true;
			m_lastTick = tick;
			m_haveYaw = true;
			m_yawContinuous = rawYaw;
			m_lastRawYaw = rawYaw;
			return invalid;
		}
	}

	const double renderDt = (double)g_MirvTime.absoluteframetime_get();
	const double playTime = m_clock.Advance(renderDt, paused);

	// A paused demo must not animate: hold the previous output and add no samples, so resuming
	// starts from the same estimate rather than from a window full of identical values.
	if (paused) return m_lastResult;

	Push(playTime, rawPitch, m_yawContinuous);

	// Sampling above is unconditional and cheap, so enabling the module is instant rather than
	// waiting for a window to refill. The two least-squares solves below are the only real cost,
	// and nothing needs them when the module is off and no trace is running.
	if (!m_enabled && !(m_traceLeft > 0.0)) {
		m_lastResult = invalid;
		return invalid;
	}

	const double window = m_windowMs * 0.001;
	const double tEval = playTime - m_lagMs * 0.001;

	const DemoAimFitResult fp = DemoAimLocalFit(m_t, m_pitch, m_count, tEval, window);
	const DemoAimFitResult fy = DemoAimLocalFit(m_t, m_yaw, m_count, tEval, window);
	m_lastUsed = fy.used;
	m_lastDegree = fy.degree;

	// Until the window holds enough points for a real fit, pass the engine value through rather
	// than hand back a half-formed estimate. Two points fit a line exactly, so they reject no
	// noise at all and would only buy the evaluation lag; three is the first count that averages
	// anything. (At 60-90 fps the 90 ms window normally holds five to eight.)
	if (!fp.valid || !fy.valid || fy.degree < 1 || fp.degree < 1 || fy.used < 3 || fp.used < 3) {
		m_lastResult = invalid;
		return invalid;
	}

	Result out;
	out.valid = true;
	out.pitch = fp.value;
	out.yaw = DemoAimWrapDegrees(fy.value);
	out.pitchRate = fp.rate;
	out.yawRate = fy.rate;

	if (!std::isfinite(out.pitch) || !std::isfinite(out.yaw)) {
		Reset();
		m_lastResult = invalid;
		return invalid;
	}

	// Applied only during demo playback, and only when enabled. Live, the angles already come from
	// the player's mouse at render rate -- that is the signal this module reproduces, so there is
	// nothing to fix and applying would only add lag.
	const bool apply = m_enabled && inDemo;
	if (apply) ++m_framesReconstructed;

	// `leg` and `applied` are what make the halves of the experiment comparable: identical fields,
	// identical code path, two words saying which side produced them and whether the fit was used.
	if (m_traceLeft > 0.0 && MvmDebugLog_Active()) {
		if (renderDt > 0.0 && renderDt <= 0.5) m_traceLeft -= renderDt;
		MvmDebugLog_Linef(
			"camera.aimresample.trace",
			"leg=%s applied=%d frame=%d renderDt=%.5f tick=%d windowMs=%.1f lagMs=%.1f "
			"used=%d degree=%d play=%.5f engineInterp=%.4f "
			"raw=(%.4f %.4f) out=(%.4f %.4f) vel=(%.2f %.2f) pawn=%d",
			inDemo ? "demo" : "live", apply ? 1 : 0,
			frame, renderDt, tick, m_windowMs, m_lagMs, fy.used, fy.degree,
			playTime, (double)g_MirvTime.interpolation_amount_get(),
			rawPitch, rawYaw, out.pitch, out.yaw, out.pitchRate, out.yawRate,
			povPawnIndex);
	}

	if (!apply) {
		m_lastResult = invalid;
		return invalid;
	}

	m_lastResult = out;
	return out;
}

std::string DemoAimResampler::DebugStateJson() const {
	std::ostringstream o;
	o << "{\"on\":" << (m_enabled ? "true" : "false")
	  << ",\"windowMs\":" << m_windowMs
	  << ",\"lagMs\":" << m_lagMs
	  << ",\"samples\":" << m_count
	  << ",\"used\":" << m_lastUsed
	  << ",\"degree\":" << m_lastDegree
	  << ",\"lastTick\":" << m_lastTick
	  << ",\"pawn\":" << m_povPawnIndex
	  << ",\"framesReconstructed\":" << m_framesReconstructed
	  << ",\"resets\":" << m_resets
	  << "}";
	return o.str();
}

void DemoAimResampler_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	DemoAimResampler& rs = DemoAimResamplerRef();
	const char* a = argc >= 3 ? args->ArgV(2) : "";

	if (0 == _stricmp(a, "on") || 0 == _stricmp(a, "off") || 0 == _stricmp(a, "toggle")) {
		const bool on = 0 == _stricmp(a, "toggle") ? !rs.Enabled() : (0 == _stricmp(a, "on"));
		rs.SetEnabled(on);
		advancedfx::Message("mirv_filmmaker: demo aim resampling %s.\n", on ? "ON" : "OFF");
		return;
	}
	if (0 == _stricmp(a, "window")) {
		if (argc >= 4) rs.SetWindowMs(atof(args->ArgV(3)));
		advancedfx::Message("mirv_filmmaker: demo aim resampling window = %.1f ms.\n", rs.WindowMs());
		return;
	}
	if (0 == _stricmp(a, "lag")) {
		if (argc >= 4) rs.SetLagMs(atof(args->ArgV(3)));
		advancedfx::Message("mirv_filmmaker: demo aim resampling lag = %.1f ms.\n", rs.LagMs());
		return;
	}
	if (0 == _stricmp(a, "trace")) {
		const double secs = argc >= 4 ? atof(args->ArgV(3)) : 20.0;
		rs.StartTrace(secs);
		advancedfx::Message("mirv_filmmaker: demo aim resample trace armed for %.1f s "
			"(needs mvm_debug running; writes camera.aimresample.trace).\n", secs);
		return;
	}
	if (0 == _stricmp(a, "state")) {
		advancedfx::Message("[aimresample][state] %s\n", rs.DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(a, "reset")) {
		rs.Reset();
		advancedfx::Message("mirv_filmmaker: demo aim resampling reset.\n");
		return;
	}

	advancedfx::Message(
		"%s aimresample on|off|toggle - rebuild the demo's recorded aim into a clean per-frame\n"
		"    signal, so Smooth View, the viewmodel and the 3D crosshair stop inheriting the\n"
		"    engine's per-frame reconstruction noise.\n"
		"%s aimresample window [0-250] - regression window in ms (default 90). Higher = steadier,\n"
		"    lower = tracks harder.\n"
		"%s aimresample lag [0-120]    - evaluate this far behind the newest sample, ms\n"
		"    (default 15). Higher = steadier, at the cost of latency.\n"
		"%s aimresample trace [s]      - bounded per-frame trace to the mvm_debug log.\n"
		"%s aimresample state|reset\n",
		cmd, cmd, cmd, cmd, cmd);
}

} // namespace Filmmaker

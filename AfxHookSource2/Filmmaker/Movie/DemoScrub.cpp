#include "DemoScrub.h"

#include "DemoScrubMath.h"
#include "DemoTimescale.h"
#include "ScrubFrameCache.h"           // backward drags over played ticks are shown from the cache
#include "../Debug/MvmDebugLog.h"
#include "../Demo/PlayingDemoPath.h"
#include "../Demo/DemoProjectStoreMath.h" // IsKeyframedCopyFileName: either slot of a keyframed copy
#include "../AltEnvironment/AltEnvironment.h" // IsKeyframedCopy
#include "../Render/RenderCommand.h" // Render_IsJobActive: a render job owns the demo clock
#include "../../ViewportScaler.h"     // AfxViewportScaler::SetHoldPreview: no raw restore on screen

#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

// A command is "in flight" from the frame it is issued until the demo shows its effect. Commands
// execute on the next frame and IsDemoPaused lags a frame, so a few frames are always waited; a
// stuck command is released after kMaxBusyFrames. A backward seek on a demo WITHOUT keyframes
// replays over MANY frames (ragdolls.dem, 3000 ticks back: 158 frames, 4 s), so the cap is a safety
// net far above any real landing.
constexpr int kMinBusyFrames = 2;
constexpr int kMaxBusyFrames = 1200;

enum class Pending { None, Pause, Resume, Step, Seek };

bool g_active = false;
bool g_released = false;
bool g_settled = false;      // released and the settle seek went out (DemoScrubMath.h)
double g_target = 0.0;
bool g_draggingBack = false; // the drag's last move went backward (DemoScrubMath.h: hold, never glide forward)
// The preview shows its last settled frame from a seek until the world has been played into and
// the controller is idle again (AfxViewportScaler::SetHoldPreview): a restore is an un-simulated
// world -- bind-pose or missing players -- and the frames of the play-in step after it run forward.
bool g_hold = false;
// After a backward drag's play-in step lands: frames to show (and let the render thread capture)
// that settled, played world before the next restore. Without the gap a continuous backward drag
// went seek-step-seek with the hold never dropping, and the preview froze until the hold's cap.
int g_showFrames = 0;
constexpr int kShowFramesAfterStep = 2;
bool g_needsPlayIn = false; // a backward drag's restore went out and has not been played into yet
bool g_wasPlaying = false;   // the demo was playing when the drag began: resume after landing
Pending g_pending = Pending::None;
int g_busyFrames = 0;
int g_expectTick = -1;
bool g_repaused = false;     // a seek landed playing and demo_pause was sent once
int g_seekFrom = -1;         // the tick a pending seek started from (JudgeSeek)
int g_playingFrames = 0;     // frames in a row the pending command has seen the demo playing
int g_heldFrames = 0;        // frames in a row the tick has not changed while it is pending
int g_pendingTick = -1;      // the tick seen last frame while it is pending
bool g_keyframed = false;    // the playing file is a keyframed copy: restores take a frame or two
double g_timescale = 1.0;    // the demo_timescale the scrub last set
int g_seeks = 0, g_steps = 0, g_plays = 0;
// IsDemoPaused reads true a frame or two before the demo clock stops: a step issued then started
// from a still-moving demo and ran ~2 ticks past its goal, and the re-aim seek/step repeated that
// for ~10 s ("Space loops the first few ticks", 2026-10-15). A pause only counts once the clock
// has held still for kStillFrames frames.
constexpr int kStillFrames = 2;
double g_prevNow = -1.0;
int g_stillFrames = 0;
// Re-aims after a lost step or seek. Past kMaxReaims the scrub lands where it is instead of
// looping: a tick or two off beats replaying the same ticks over and over.
constexpr int kMaxReaims = 3;
int g_reaims = 0;
bool g_serving = false;      // the last frame was shown from the scrub frame cache (for the log)

void Cmd(const char* c) {
	if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
}

// Offset between the client clock (curtime * 64) and the demo tick, learned whenever the demo is
// paused on a whole tick (it is the demo's server start tick; 3173 in ragdolls.dem).
bool g_haveClockOffset = false;
double g_clockOffset = 0.0;

// The world's demo time in ticks, FRACTIONAL. A paused demo can rest between two ticks
// (DemoScrubMath.h) and a slow drag has to see that. The client clock is the source: measured
// 2026-09-27, curtime * 64 reads exactly tick + offset when paused on a whole tick, holds the
// fraction when paused between ticks, and advances continuously while playing. (MirvTime's
// interpolation_amount reads 0 in CS2 -- a CS:GO-era offset -- so it cannot be used.)
bool DemoState(int& tick, double& now, bool& paused) {
	tick = 0; now = 0.0; paused = true;
	if (!g_pEngineToClient) return false;
	auto* demo = g_pEngineToClient->GetDemoFile();
	if (!demo || !(demo->IsPlayingDemo() || demo->IsDemoPaused())) return false;
	paused = demo->IsDemoPaused();
	if (!g_MirvTime.GetCurrentDemoTick(tick)) return false;
	const double rel = (double)g_MirvTime.curtime_get() * kScrubTickRate - tick;
	if (paused && std::fabs(rel - std::floor(rel + 0.5)) < 0.002) {
		g_clockOffset = std::floor(rel + 0.5);
		g_haveClockOffset = true;
	}
	// Before an offset is learned, tolerate the clock's small backward jitter at a tick boundary.
	const double offset = g_haveClockOffset ? g_clockOffset : std::floor(rel + 0.02);
	double frac = rel - offset;
	if (!(frac >= 0.0)) frac = 0.0;
	if (frac > 0.999) frac = 0.999;
	now = tick + frac;
	return true;
}

void Log(const char* fmt, ...) {
	if (!MvmDebugLog_Active()) return;
	char buf[256];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	MvmDebugLog_LinefAlways("demo.scrub", "%s", buf);
}

void Begin() {
	if (g_active) return;
	g_haveClockOffset = false; // per demo: relearned on the first paused whole tick
	int tick = 0; double now = 0.0; bool paused = true;
	if (!DemoState(tick, now, paused)) return;
	g_active = true;
	g_pending = Pending::None;
	g_busyFrames = 0;
	g_seeks = g_steps = g_plays = 0;
	g_reaims = 0;
	g_stillFrames = 0; g_prevNow = -1.0;
	g_wasPlaying = !paused;
	g_timescale = DemoTimescale_Get();
	// Per scrub, not per frame: resolving the playing file is not free.
	const std::wstring file = ResolvePlayingDemoPath();
	const size_t slash = file.find_last_of(L"\\/");
	g_keyframed = IsKeyframedCopyFileName(slash == std::wstring::npos ? file : file.substr(slash + 1))
		|| AltEnvironment::IsKeyframedCopy(file); // the other-map copy of a keyframed copy keeps its keyframes
	Log("begin now=%.3f wasPlaying=%d keyframed=%d", now, g_wasPlaying ? 1 : 0, g_keyframed ? 1 : 0);
}

void Finish(double now) {
	// Back to the user's own speed, and play if they were playing.
	DemoTimescale_Set(DemoTimescale_Get());
	if (g_wasPlaying) Cmd("demo_resume");
	// Landed on a cached tick: keep showing the recorded take's frame there (ScrubFrameCache_Park).
	else if (g_released) ScrubFrameCache_Park(ScrubGoal(g_target, true));
	Log("done now=%.3f target=%.3f seeks=%d steps=%d plays=%d resumed=%d", now, g_target, g_seeks, g_steps,
		g_plays, g_wasPlaying ? 1 : 0);
	g_active = false;
	g_released = false;
	g_settled = false;
	g_pending = Pending::None;
}

void Issue(Pending pending, int expectTick, int fromTick = -1) {
	g_pending = pending;
	g_seekFrom = fromTick;
	g_busyFrames = 0;
	g_playingFrames = 0;
	g_heldFrames = 0;
	g_pendingTick = -1;
	g_expectTick = expectTick;
	g_repaused = false;
}

} // namespace

void DemoScrub_Drag(double tick) {
	if (Render_IsJobActive()) return;
	const bool wasActive = g_active;
	Begin();
	if (!g_active) return;
	// Direction of the drag, from the playhead's own moves (the world lags it). A fresh drag
	// starts from where the world is.
	double from = g_target;
	if (!wasActive) {
		int worldTick = 0; bool worldPaused = true;
		if (!DemoState(worldTick, from, worldPaused)) from = tick;
	}
	if (tick < from - 1e-3) g_draggingBack = true;
	else if (tick > from + 1e-3) g_draggingBack = false;
	g_target = tick;
	g_released = false;
	g_settled = false;
}

void DemoScrub_Release(double tick, bool resume) {
	if (Render_IsJobActive()) return;
	Begin();
	if (!g_active) return;
	if (!resume) g_wasPlaying = false;
	g_target = tick;
	g_released = true;
	g_reaims = 0; // a new destination gets its own retries
}

bool DemoScrub_Active() { return g_active; }

bool DemoScrub_Dragging() { return g_active && !g_released; }

bool DemoScrub_Target(double& tick) {
	if (!g_active) return false;
	tick = g_target;
	return true;
}

void DemoScrub_Cancel() {
	if (!g_active) return;
	DemoTimescale_Set(DemoTimescale_Get());
	Log("cancelled target=%.3f seeks=%d steps=%d", g_target, g_seeks, g_steps);
	g_active = false;
	g_released = false;
	g_settled = false;
	g_pending = Pending::None;
}

void DemoScrub_RunFrame() {
	// Published on every path out, including the early ones: a hold must never outlive the scrub.
	struct PublishHold { ~PublishHold() { if (!g_active) g_hold = false; AfxViewportScaler::SetHoldPreview(g_hold); } } publish;
	if (!g_active) return;
	int tick = 0; double now = 0.0; bool paused = true;
	if (!DemoState(tick, now, paused) || Render_IsJobActive()) {
		// The demo went away (or a render took the clock): drop the scrub without touching it.
		g_active = false; g_pending = Pending::None;
		return;
	}
	if (paused && g_prevNow >= 0.0 && std::fabs(now - g_prevNow) < 1e-4) ++g_stillFrames;
	else g_stillFrames = 0;
	g_prevNow = now;
	const bool stopped = paused && g_stillFrames >= kStillFrames;

	// SCRUB FRAME CACHE: a playhead behind the world over frames the current take has played is
	// shown from the cache (ScrubFrameCache.h); the world stays where it is and no restore goes out.
	// A command already in flight still finishes below, but nothing new is decided while serving.
	ScrubCacheInput cache;
	cache.enabled = ScrubFrameCache_Enabled();
	cache.released = g_released;
	cache.target = ScrubGoal(g_target, g_released);
	cache.now = now;
	cache.recording = ScrubFrameCache_Recording();
	cache.cached = cache.enabled && ScrubFrameCache_Lookup(cache.target);
	const bool serving = ScrubServeFromCache(cache);
	if (serving) {
		ScrubFrameCache_Serve(cache.target);
		// The hold is left as it is. A serve request reaches the render thread with the frame's own
		// scale blit, a frame after the hold flag: dropping the hold here let the one present in
		// between show the restore still in flight, and it was kept as the frame to fall back on --
		// a bind-pose player on the first served frame (2026-10-16). Served presents bypass the hold.
		g_showFrames = 0;
		if (!g_serving) Log("serving from cache at %.3f (world %.3f)", g_target, now);
	} else if (g_serving) {
		Log("cache serve ends at %.3f (world %.3f released=%d)", g_target, now, g_released ? 1 : 0);
	}
	g_serving = serving;
	// A release is shown from the cache while the world still lands there for real (play and edits
	// need the real world), so it goes on to the landing below.
	const bool servingDrag = serving && !g_released;
	if (g_pending != Pending::None) {
		++g_busyFrames;
		g_playingFrames = paused ? 0 : g_playingFrames + 1;
		g_heldFrames = tick == g_pendingTick ? g_heldFrames + 1 : 0;
		g_pendingTick = tick;
		bool landed = false;
		switch (g_pending) {
		case Pending::Pause: landed = stopped; break;
		case Pending::Resume: landed = !paused; break;
		case Pending::Step:
			// A step only ever moves forward from where it started; anywhere else, another seek
			// moved the world and the step will never finish.
			if (tick < g_seekFrom - 1 || tick > g_expectTick + 1) {
				Log("step to %d from %d lost: tick=%d; re-aiming", g_expectTick, g_seekFrom, tick);
				g_pending = Pending::None;
				if (g_released) g_settled = false;
				// A backward drag's play-in that ran long has still played the world in. Stepping
				// forward AGAIN from there (the old behaviour) ran the world further away from a
				// playhead moving back: 5810 -> 5816 -> 5818 while the drag went to 5806.
				if (!g_released) g_needsPlayIn = false;
				// demo_step_tick sometimes runs ~3 ticks long right after a restore (measured
				// 2026-10-15: 8 ticks from 5748 ended on 5759, twice running). Retry once; a
				// second overshoot just past the goal is where the step lands, so stay there.
				const bool overshot = tick > g_expectTick + 1 && tick - g_target <= 4.0;
				if (g_released && overshot && g_reaims >= 1) {
					if (!paused) Cmd("demo_pause");
					Log("step overshot again: landing at %d (goal %.0f)", tick, g_target);
					Finish(now);
					return;
				}
				if (g_released && ++g_reaims > kMaxReaims) {
					if (!paused) Cmd("demo_pause");
					Log("giving up after %d re-aims: landing at %d", g_reaims - 1, tick);
					Finish(now);
					return;
				}
				break;
			}
			landed = stopped && tick >= g_expectTick - 1;
			break;
		case Pending::Seek: {
			// A seek must land ON its tick (+/-1): mid-replay a backward seek reads the OLD tick,
			// paused, which is past the target and not a landing.
			const bool there = tick >= g_expectTick - 1 && tick <= g_expectTick + 1;
			// The seek command does not include demo_pause: it toggles, and sent while the demo is
			// still paused it starts playback (timeline/graph reports 2026-10-13: any time change
			// plays the demo). A seek that lands playing (something resumed the demo meanwhile) is
			// paused once, only after the tick has landed and the demo is observed playing.
			if (there && !paused && !g_repaused && g_busyFrames >= 2) {
				Cmd("demo_pause");
				g_repaused = true;
				Log("seek landed playing at %d (expect %d): pausing", tick, g_expectTick);
			}
			const SeekWait wait = JudgeSeek(g_seekFrom, g_expectTick, tick, paused, g_busyFrames, g_playingFrames, g_heldFrames,
				g_keyframed);
			if (wait == SeekWait::Lost) {
				// Another seek (CS2's own bar, a click) moved the world, or ours was swallowed: stop
				// waiting and aim again from where the world is (DemoScrubMath.h JudgeSeek).
				Log("seek %d from %d lost after %d frame(s): tick=%d paused=%d; re-aiming", g_expectTick, g_seekFrom,
					g_busyFrames, tick, paused ? 1 : 0);
				g_pending = Pending::None;
				if (g_released) g_settled = false; // the settle seek did not happen: do it again
				if (g_released && ++g_reaims > kMaxReaims) {
					if (!paused) Cmd("demo_pause");
					Log("giving up after %d re-aims: landing at %d", g_reaims - 1, tick);
					Finish(now);
					return;
				}
				break;
			}
			landed = wait == SeekWait::Landed && stopped;
			break;
		}
		default: break;
		}
		const int minFrames = (g_pending == Pending::Resume || g_pending == Pending::Pause) ? 1 : kMinBusyFrames;
		if ((landed && g_busyFrames >= minFrames) || g_busyFrames >= kMaxBusyFrames) {
			if (!landed)
				Log("command timed out (kind %d) tick=%d expect=%d paused=%d", (int)g_pending, tick, g_expectTick, paused ? 1 : 0);
			const bool backStepLanded = landed && g_pending == Pending::Step && g_draggingBack && !g_released;
			g_pending = Pending::None;
			if (backStepLanded && !servingDrag) {
				// The world has been played into: show it before restoring again.
				g_needsPlayIn = false;
				g_hold = false;
				g_showFrames = kShowFramesAfterStep;
				return;
			}
		} else {
			return;
		}
	}
	if (servingDrag) {
		// Nothing to steer: the world waits. A glide that was running when the drag turned back
		// is paused (demo_pause toggles: only while playing).
		if (!paused) {
			Cmd("demo_pause");
			Issue(Pending::Pause, -1);
		}
		return;
	}
	if (g_showFrames > 0 && !g_released) {
		--g_showFrames;
		return;
	}
	g_showFrames = 0;

	ScrubInput in;
	in.target = g_target;
	in.now = now;
	in.paused = paused;
	in.released = g_released;
	in.settled = g_settled;
	const float frame = g_MirvTime.absoluteframetime_get();
	in.frameSeconds = (frame > 0.0f && frame < 0.25f) ? frame : 1.0 / 60.0;
	in.timescale = g_timescale;
	in.draggingBack = g_draggingBack && !g_released;
	if (!in.draggingBack) g_needsPlayIn = false; // forward drags glide; a release settles by itself
	in.restoredNotPlayed = g_needsPlayIn;
	const ScrubDecision d = DecideScrub(in);
	// Hold from a seek through its play-in step and pauses; let go when the world glides (those
	// frames are played) or when there is nothing left to do.
	if (d.action == ScrubAction::Seek) { g_hold = true; if (in.draggingBack) g_needsPlayIn = true; }
	else if (d.action == ScrubAction::Play || d.action == ScrubAction::None || d.action == ScrubAction::Done) g_hold = false;
	char c[96];
	switch (d.action) {
	case ScrubAction::Play:
		// Glide: the world plays toward the playhead, slowing as it gets close (DemoScrubMath.h).
		g_timescale = d.timescale;
		if (paused) {
			_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.4f;demo_resume", d.timescale);
			Issue(Pending::Resume, -1);
		} else {
			_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.4f", d.timescale);
		}
		Cmd(c);
		if ((g_plays++ % 16) == 0)
			Log("play %.3fx from %.3f target=%.3f", d.timescale, now, g_target);
		break;
	case ScrubAction::Pause:
		// Only ever decided while playing: demo_pause is a toggle.
		Cmd("demo_pause");
		Issue(Pending::Pause, -1);
		Log("pause at %.3f target=%.3f released=%d", now, g_target, g_released ? 1 : 0);
		break;
	case ScrubAction::Step:
		// Play the last whole ticks for real, paused exactly on the goal.
		_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_timescale %.3f;demo_step_tick %d", d.timescale, d.ticks);
		Cmd(c);
		g_timescale = d.timescale;
		Issue(Pending::Step, tick + d.ticks, tick);
		++g_steps;
		Log("step %d ticks at %.2fx from %.3f target=%.3f released=%d", d.ticks, d.timescale, now, g_target, g_released ? 1 : 0);
		break;
	case ScrubAction::Seek:
		// Always from a paused demo (DecideScrub pauses first), and a paused seek lands paused on
		// its tick either way (DemoScrubMath.h). A forward seek used to resume first; the world
		// then played on past every click, the seek was judged lost two ticks past its target,
		// and a backward restore followed (2026-10-19). If something else resumes the demo
		// meanwhile, the handler above pauses it once.
		_snprintf_s(c, sizeof(c), _TRUNCATE, "demo_gototick %d", d.seekTick);
		Cmd(c);
		Issue(Pending::Seek, d.seekTick, tick);
		ScrubFrameCache_NoteSeek(); // a new take starts where this lands
		++g_seeks;
		if (g_released) g_settled = true;
		Log("seek %d from %.3f target=%.3f released=%d", d.seekTick, now, g_target, g_released ? 1 : 0);
		break;
	case ScrubAction::Done:
		Finish(now);
		break;
	default:
		break;
	}
}

bool DemoScrub_WorldTime(int& tick, double& now, bool& paused) {
	return DemoState(tick, now, paused);
}

bool DemoScrub_SeekInFlight() {
	return g_active && g_pending == Pending::Seek;
}

void DemoScrub_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "status";
	if (0 == _stricmp(sub, "cache")) {
		ScrubFrameCache_RunCommand(argc, args, cmd);
		return;
	}
	if ((0 == _stricmp(sub, "drag") || 0 == _stricmp(sub, "release")) && argc >= 4) {
		const double t = atof(args->ArgV(3));
		if (0 == _stricmp(sub, "drag")) DemoScrub_Drag(t);
		else DemoScrub_Release(t);
		return; // silent: every drag frame calls this
	}
	int tick = 0; double now = 0.0; bool paused = true;
	const bool demo = DemoState(tick, now, paused);
	advancedfx::Message("%s scrub clock: tick=%d interp=%.4f curtime=%.5f curtime*64=%.4f\n", cmd, tick,
		g_MirvTime.interpolation_amount_get(), g_MirvTime.curtime_get(), g_MirvTime.curtime_get() * kScrubTickRate);
	advancedfx::Message("%s scrub: %s target=%.3f world=%.3f%s released=%d pending=%d seeks=%d steps=%d plays=%d\n"
		"%s scrub drag <tick>     - the world follows a timeline drag (glides forward, restores back)\n"
		"%s scrub release <tick>  - land exactly on the tick, restore speed and play state\n"
		"%s scrub cache [on|off|clear|budget <MB>] - show backward drags over played ticks from the frame cache\n",
		cmd, g_active ? "ACTIVE" : "idle", g_target, now, demo ? (paused ? " (paused)" : " (playing)") : " (no demo)",
		g_released ? 1 : 0, (int)g_pending, g_seeks, g_steps, g_plays, cmd, cmd, cmd);
}

} // namespace Filmmaker

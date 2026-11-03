#include "ScrubFrameCache.h"

#include "ScrubFrameCacheMath.h"
#include "ScrubFrameCacheHost.h"           // what the cache asks the game: CS2's answers or CS:S's
#include "DemoScrub.h"
#include "../Debug/MvmDebugLog.h"

#include "../../../Shared/AfxConsole.h"

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <windows.h>

namespace Filmmaker {
namespace {

// How far path playback's predicted clock may trail the world and its frame still be kept (CamPlayback
// starts ~13 ticks behind while it learns the tick rate; beyond its own seek snap, 16, it is a seek).
constexpr double kPlaybackLagTicks = 16.0;

bool g_enabled = true;
bool g_configured = false;
int g_budgetMb = ScrubCache::kDefaultBudgetMb;

ScrubCache::TakeTracker g_takes;
uint64_t g_revision = 0;
bool g_haveWorld = false;
double g_now = 0.0;
int g_tick = 0;

// The camera drivers' note for THIS frame, and whether the path flew the camera last frame (DemoScrub
// runs before the drivers, so its lookups go by the previous frame).
bool g_camNoted = false, g_camNotedLast = false;
double g_camTick = -1.0;
bool g_camClock = false;
float g_camPose[ScrubCache::kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };
float g_camPathPose[ScrubCache::kPoseValues] = { 0, 0, 0, 0, 0, 0, 0 };

bool g_serve = false;
double g_serveTick = 0.0;

// The path flew the camera within the last frames: path playback stops noting a few frames before it
// lands on the last key, and a click's release frames are flown by the surface that was clicked.
constexpr int kPathCameraFrames = 30;
int g_framesSinceNote = 1 << 20;

// PARKED: a release landed on a cached tick, and the preview keeps showing the recorded take's frame
// there while the world sits on that tick (a click shows what a drag showed). Let go on play, a new
// drag, the world moving off the tick, the camera moving, or the frame no longer being valid.
bool g_parked = false;
double g_parkTick = 0.0;
double g_parkCam[7] = { 0, 0, 0, 0, 0, 0, 0 };
constexpr double kParkCameraTolerance = 0.5; // units / degrees

// Counters for the log and `health`.
unsigned long long g_hits = 0, g_misses = 0, g_poseDrops = 0, g_revisionDrops = 0, g_takesStarted = 0;
unsigned long long g_servedFrames = 0, g_notedFrames = 0, g_cacheableFrames = 0, g_parks = 0;

// The toggle lives in the game's settings (it may load after the first frame, and the UI writes it),
// so it is read every frame; a change reaches the render side at once.
void ReadToggle() {
	const bool want = ScrubCacheHost_Enabled();
	if (g_configured && want == g_enabled) return;
	g_configured = true;
	g_enabled = want;
	ScrubFrames_Configure(g_enabled, g_budgetMb);
}

void Log(const char* fmt, ...) {
	if (!MvmDebugLog_Active()) return;
	char buf[256];
	va_list va;
	va_start(va, fmt);
	vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	MvmDebugLog_LinefAlways("scrub.cache", "%s", buf);
}

// Read at the start of the frame (DemoScrub's lookups) and again at its end: an edit made during
// the frame (a graph drag bakes into the path) must not stamp this frame with the old revision.
void UpdateRevision() {
	uint64_t rev = ScrubCacheHost_PictureRevision();
	if (!rev) rev = 1; // 0 means none yet
	if (rev == g_revision) return;
	const bool first = g_revision == 0;
	g_revision = rev;
	const int n = ScrubFrames_DropOtherRevisions(rev);
	g_revisionDrops += (unsigned long long)n;
	if (n > 0 && !first) Log("revision changed: dropped %d frame(s)", n);
}

void ReadCamera(double out[7]) { ScrubCacheHost_Camera(out); }

void Unpark(const char* why) {
	if (!g_parked) return;
	g_parked = false;
	Log("unparked from %.0f: %s", g_parkTick, why);
}

// The frame for `tick`, if it is cached for the current revision and the path still flies the pose
// it was kept with. `pathCamera`: the caller knows the camera is the path's.
bool Find(double tick, bool pathCamera) {
	if (!g_enabled || !g_haveWorld) return false;
	if (!pathCamera && !(g_framesSinceNote <= kPathCameraFrames || ScrubCacheHost_PathScrubbing())) return false;
	ScrubCache::Index::Entry e;
	const int slot = ScrubFrames_Find(tick, g_revision, &e);
	if (slot < 0) return false;
	// The revision follows every path edit; this catches anything it missed: the path must still
	// fly the pose the frame was taken with.
	double p[7];
	if (ScrubCacheHost_PathPose(e.worldTime, p)) {
		float fp[ScrubCache::kPoseValues];
		for (int i = 0; i < ScrubCache::kPoseValues; ++i) fp[i] = (float)p[i];
		if (!ScrubCache::PoseNear(fp, e.pathPose)) {
			ScrubFrames_DropSlot(slot);
			++g_poseDrops;
			Log("pose changed at tick %d: dropped", e.tick);
			return false;
		}
	}
	return true;
}

} // namespace

bool ScrubFrameCache_Enabled() {
	return g_enabled;
}

void ScrubFrameCache_SetEnabled(bool enabled) {
	ScrubCacheHost_SetEnabled(enabled);
	const bool changed = enabled != g_enabled;
	ReadToggle();
	if (changed) Log("toggle %s", enabled ? "on" : "off");
}

void ScrubFrameCache_BeginFrame() {
	ReadToggle();
	g_camNotedLast = g_camNoted;
	if (g_camNoted) g_framesSinceNote = 0;
	else if (g_framesSinceNote < (1 << 20)) ++g_framesSinceNote;
	g_camNoted = false;
	g_serve = false;
	bool paused = true;
	g_haveWorld = g_enabled && DemoScrub_WorldTime(g_tick, g_now, paused);
	if (!g_haveWorld) {
		g_takes.Reset();
		Unpark("no demo or cache off");
		return;
	}
	UpdateRevision();
	const ScrubCache::TakeTracker::Event ev = g_takes.Observe(g_tick, g_now, DemoScrub_SeekInFlight());
	if (ev.newTake) {
		// Nothing is dropped: the recorded take stays until a pass from the shot's start re-records it.
		++g_takesStarted;
		Log("take %d starts at %.3f", g_takes.take, ev.startTick);
	}
	if (g_parked) {
		double cam[7];
		ReadCamera(cam);
		bool moved = false;
		for (int i = 0; i < 7; ++i) moved = moved || std::fabs(cam[i] - g_parkCam[i]) > kParkCameraTolerance;
		if (DemoScrub_Active()) Unpark("scrub");
		else if (!paused) Unpark("playing");
		else if (std::fabs(g_tick - g_parkTick) > 2.0) Unpark("world moved");
		else if (moved) Unpark("camera moved");
	}
}

bool ScrubFrameCache_Recording() {
	double first = 0.0;
	if (!ScrubCacheHost_PathFirstTick(first)) return true;
	return ScrubCache::Recording(g_takes.start, first);
}

void ScrubFrameCache_Park(double tick) {
	if (!Find(tick, true)) { Unpark("release off the cached span"); return; }
	g_parked = true;
	g_parkTick = tick;
	ReadCamera(g_parkCam);
	++g_parks;
	Log("parked on the cached frame at %.0f", tick);
}

double ScrubFrameCache_CameraTick(double playheadTick) {
	// Only while DRAGGING. After a release the camera flies the tick it was released on while the
	// world settles there: the camera timeline and the graph stop driving the camera once the world
	// lands, and a camera left at the travelling world's time ended a jump to the first key a few
	// ticks short of it (2026-10-16).
	if (!g_enabled || !g_haveWorld || !DemoScrub_Dragging()) return playheadTick;
	return g_now;
}

void ScrubFrameCache_NoteCamera(double cameraTick, const double pose[7], const double* pathPose, bool clockTime) {
	g_camNoted = true;
	g_camTick = cameraTick;
	g_camClock = clockTime;
	for (int i = 0; i < ScrubCache::kPoseValues; ++i) {
		g_camPose[i] = (float)pose[i];
		g_camPathPose[i] = (float)(pathPose ? pathPose[i] : pose[i]);
	}
}

bool ScrubFrameCache_Lookup(double tick) {
	// The camera must be the path's: it flew it within the last frames, or a path surface (the camera
	// timeline or the graph) is being dragged right now -- the first frame of a drag after playback
	// or a release, before the drag's own driver has run. CS2's demo bar does not fly the path.
	const bool hit = Find(tick, false);
	if (hit) ++g_hits; else ++g_misses;
	return hit;
}

void ScrubFrameCache_Serve(double tick) {
	g_serve = true;
	g_serveTick = tick;
}

void ScrubFrameCache_NoteSeek() {
	g_takes.NoteSeek();
}

void ScrubFrameCache_EndFrame() {
	ScrubFramePublish f;
	if (g_parked && !g_serve) {
		if (Find(g_parkTick, true)) { g_serve = true; g_serveTick = g_parkTick; }
		else Unpark("frame no longer valid");
	}
	if (g_haveWorld) {
		UpdateRevision();
		f.id.valid = true;
		f.id.worldTime = g_now;
		f.id.cameraTick = g_camNoted ? g_camTick : -1.0;
		// Path playback runs its own predicted clock, behind the world (measured
		// 2026-10-16: nearly every playback frame failed a one-tick window). What it shows is the
		// world with the playback camera as it was: keep it under the world's time, validated
		// against the path's pose there. The clock starts ~13 ticks behind while its rate settles;
		// beyond kPlaybackLagTicks it is a seek snap, not playback.
		if (g_camNoted && g_camClock) {
			double p[7];
			if (std::fabs(g_camTick - g_now) <= kPlaybackLagTicks && ScrubCacheHost_PathPose(g_now, p)) {
				f.id.cameraTick = g_now;
				for (int i = 0; i < ScrubCache::kPoseValues; ++i) g_camPathPose[i] = (float)p[i];
			} else {
				f.id.cameraTick = -1.0;
			}
		}
		f.id.played = g_takes.played;
		f.id.take = g_takes.take;
		f.id.takeStart = g_takes.start;
		f.id.recording = ScrubFrameCache_Recording();
		f.id.revision = g_revision;
		for (int i = 0; i < ScrubCache::kPoseValues; ++i) { f.id.pose[i] = g_camPose[i]; f.id.pathPose[i] = g_camPathPose[i]; }
		f.serve = g_serve;
		f.serveTick = g_serveTick;
		double target = g_now;
		DemoScrub_Target(target);
		f.nearTick = target;
		if (g_serve) ++g_servedFrames;
		if (g_camNoted) ++g_notedFrames;
		if (ScrubCache::Cacheable(f.id)) ++g_cacheableFrames;
	}
	ScrubFrames_Publish(f);

	// Once a second while anything happened: the main-thread half of the story (the render thread
	// logs stored/served per present as `scrub.cache present`).
	static ULONGLONG s_last = 0;
	static unsigned long long s_hits = 0, s_misses = 0, s_served = 0, s_noted = 0, s_cacheable = 0;
	const ULONGLONG nowMs = GetTickCount64();
	if (MvmDebugLog_Active() && nowMs - s_last >= 1000 && (g_hits != s_hits || g_misses != s_misses || g_servedFrames != s_served
		|| g_notedFrames != s_noted)) {
		s_last = nowMs;
		Log("frame hits=%llu misses=%llu servedFrames=%llu noted=%llu cacheable=%llu take=%d played=%d world=%.2f camTick=%.2f clock=%d",
			g_hits - s_hits, g_misses - s_misses, g_servedFrames - s_served, g_notedFrames - s_noted, g_cacheableFrames - s_cacheable,
			g_takes.take, g_takes.played ? 1 : 0, g_now, g_camTick, g_camClock ? 1 : 0);
		s_hits = g_hits; s_misses = g_misses; s_served = g_servedFrames; s_noted = g_notedFrames; s_cacheable = g_cacheableFrames;
	}
}

void ScrubFrameCache_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// mirv_filmmaker scrub cache [on|off|clear|budget <MB>]
	const char* sub = argc >= 4 ? args->ArgV(3) : "status";
	if (0 == _stricmp(sub, "on") || 0 == _stricmp(sub, "off")) {
		ScrubFrameCache_SetEnabled(0 == _stricmp(sub, "on"));
	} else if (0 == _stricmp(sub, "clear")) {
		const int n = ScrubFrames_Clear();
		advancedfx::Message("%s scrub cache: cleared %d frame(s)\n", cmd, n);
		return;
	} else if (0 == _stricmp(sub, "budget") && argc >= 5) {
		int mb = atoi(args->ArgV(4));
		if (mb < 16) mb = 16;
		if (mb > 4096) mb = 4096;
		g_budgetMb = mb;
		ScrubFrames_Configure(g_enabled, g_budgetMb);
	} else if (0 != _stricmp(sub, "status")) {
		advancedfx::Message("%s scrub cache on|off|clear|budget <MB>\n", cmd);
		return;
	}
	ScrubFrameCache_PrintHealth();
}

void ScrubFrameCache_PrintHealth() {
	ScrubFrameStats cs;
	ScrubFrames_GetStats(cs);
	advancedfx::Message("  scrub frame cache            : %s, %d/%d frames %ux%u, %llu MB VRAM (budget %d MB), take %d\n"
		"                                 hits %llu misses %llu, stored %llu served %llu (missing %llu), evicted %llu,\n"
		"                                 dropped: edits %llu, pose %llu; pose mismatches %llu; parks %llu%s\n",
		g_enabled ? "ON" : "off", cs.frames, cs.capacity, cs.width, cs.height, cs.bytes >> 20, g_budgetMb, g_takes.take,
		g_hits, g_misses, cs.stores, cs.served, cs.serveMissing, cs.evictions, g_revisionDrops, g_poseDrops,
		cs.poseMismatches, g_parks, g_parked ? " (parked now)" : "");
}

} // namespace Filmmaker

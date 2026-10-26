#include "stdafx.h"

#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceMainThread.h"

#include "Ui/PathOverlay.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CamMarkerPickMath.h>
#include <AfxHookSource2/Filmmaker/Movie/CamPathEval.h>
#include <AfxHookSource2/Filmmaker/Movie/CamPlayback.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <Shared/AfxConsole.h>
#include <Shared/CamPath.h>

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern WrpVEngineClient* g_VEngineClient;

// The spline CamPathEval builds the path on (HLAE's CamPath). It is the path's own: mirv_campath edits
// HLAE's separate one in the view hook (Hook_VClient_RenderView::m_CamPath), so the two never meet.
CamPath g_CamPath;

namespace SourceFilmmaker {

namespace {

using namespace Filmmaker;
using Clock = std::chrono::steady_clock;

constexpr int kTimingLive = 0, kTimingFreeze = 1;
const char* const kSpeedModeNames[] = { "manual", "constant", "persegment" };
const char* const kInterpNames[] = { "linear", "smooth" };
const char* const kTimingNames[] = { "live", "freeze" };

struct State {
	CamMarkers markers;
	PathSettings settings;
	CamPathEval eval;
	CamPlayback play;
	PathMode mode = PathMode::Editing;
	int aimed = -1;
	int resumeGuard = 0;      // Live: frames that re-resume a demo the seek paused (CS2's m_liveResumeGuard)
	int startPauseGuard = 0;  // frames in which a stray pause cannot stop a start
	const char* notice = "";
	Clock::time_point noticeAt;
	std::string demo;         // the demo the markers were placed in
	std::vector<double> trajectory; // the in-world line: x, y, z along the path
};
State g;

void Command(const char* command) {
	if (g_VEngineClient) g_VEngineClient->ClientCmd_Unrestricted(command);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("campath", "command %s", command);
}

void Notify(const char* text) {
	g.notice = text;
	g.noticeAt = Clock::now();
	advancedfx::Message("[campath] %s\n", text);
}

// The path, and its in-world line sampled along each segment with the evaluator that flies it, so the
// line follows the Smooth spline and the graph's curves as the camera will.
void Rebuild() {
	g.eval.Rebuild(g.markers, g.settings);
	g.trajectory.clear();
	if (g.markers.Count() < 2 || !g.eval.CanEval()) return;
	constexpr int kSamplesPerSegment = 24;
	for (int seg = 0; seg < g.eval.SegmentCount(); ++seg) {
		for (int k = seg == 0 ? 0 : 1; k <= kSamplesPerSegment; ++k) { // a segment's start is the last one's end
			const CamPathEval::Pose p = g.eval.EvalSegP(g.markers, seg, (double)k / kSamplesPerSegment);
			if (!p.valid) continue;
			g.trajectory.push_back(p.x);
			g.trajectory.push_back(p.y);
			g.trajectory.push_back(p.z);
		}
	}
}

CamPlayback::Context MakeContext() {
	CamPlayback::Context ctx;
	ctx.timing = g.settings.timing;
	ctx.speedMode = g.settings.speedMode;
	ctx.interpName = kInterpNames[g.settings.interp == 1 ? 1 : 0];
	ctx.speedModeName = kSpeedModeNames[g.settings.speedMode >= 0 && g.settings.speedMode < 3 ? g.settings.speedMode : 0];
	ctx.constSpeed = g.settings.constSpeed;
	return ctx;
}

void PushPose(const CamMarker& m) {
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
}

void SetMode(PathMode mode, const char* why) {
	if (g.mode == mode) return;
	g.mode = mode;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("campath", "mode=%s (%s)", mode == PathMode::Editing ? "editing" : (mode == PathMode::Armed ? "armed" : "playing"), why);
}

// The end of the path: in Live the demo lands paused on the last marker; the view goes back to the
// game's own camera, as CS2 hands back to its camera at the last key.
void FinishAtLastKey() {
	const int lastTick = g.markers.Empty() ? -1 : g.markers.Back().tick;
	g.play.Stop();
	SetMode(PathMode::Editing, "end of path");
	if (g.settings.timing == kTimingLive && lastTick >= 0) DemoBar_SeekTo(lastTick, 1);
	CameraBridge_SetFreeCamEnabled(false);
	advancedfx::Message("[campath] end of path: the view is back on the game's camera.\n");
}

void RestartPath() {
	g.play.Stop();
	if (g.settings.timing == kTimingLive && !g.markers.Empty()) {
		DemoBar_SeekTo(g.markers.Front().tick, 0);
		g.resumeGuard = 120;
	}
	g.play.StartPlay(g.eval.Duration(), g.settings.timing == kTimingLive);
	SetMode(PathMode::Playing, "loop");
}

bool Parse01(const char* s, bool& out) {
	if (0 == _stricmp(s, "1") || 0 == _stricmp(s, "on")) { out = true; return true; }
	if (0 == _stricmp(s, "0") || 0 == _stricmp(s, "off")) { out = false; return true; }
	return false;
}

} // namespace

const CamMarkers& CameraPath_Markers() { return g.markers; }

PathState CameraPath_State() {
	PathState s;
	s.count = g.markers.Count();
	s.selected = g.markers.Selected();
	s.aimed = g.aimed;
	s.mode = g.mode;
	s.settings = g.settings;
	s.duration = g.eval.Duration();
	s.notice = Clock::now() - g.noticeAt < std::chrono::milliseconds(3500) ? g.notice : "";
	return s;
}

bool CameraPath_OwnsView() { return g.mode != PathMode::Editing; }

bool CameraPath_Overlay(Ui::PathOverlayModel& out, float width, float height) {
	if (g.markers.Empty() || g.mode == PathMode::Playing) return false;
	out.markers.resize((size_t)g.markers.Count());
	for (int i = 0; i < g.markers.Count(); ++i) {
		const CamMarker& m = g.markers.At(i);
		Ui::PathOverlayMarker& o = out.markers[(size_t)i];
		o.pos[0] = m.x; o.pos[1] = m.y; o.pos[2] = m.z;
		o.ang[0] = m.pitch; o.ang[1] = m.yaw; o.ang[2] = m.roll;
		o.fov = View_ScreenFov(m.fov, width, height); // the frame the marker films, as on screen
	}
	out.trajectory = g.trajectory;
	out.aimed = g.aimed;
	out.freeze = g.settings.timing == kTimingFreeze;
	// The playhead: where the path puts the camera at the demo's tick, inside the path's ticks.
	const DemoState demo = Demo_State();
	out.head = false;
	if (demo.playing && g.markers.Count() >= 2 && g.eval.CanEval()
		&& demo.tick >= g.markers.Front().tick && demo.tick <= g.markers.Back().tick) {
		const CamPathEval::Pose p = g.eval.EvalAtTick(g.markers, demo.tick);
		if (p.valid) {
			out.head = true;
			out.headPos[0] = p.x; out.headPos[1] = p.y; out.headPos[2] = p.z;
		}
	}
	return true;
}

void CameraPath_Place() {
	CamMarker m;
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	CameraBridge_GetDemoClock(m.time, m.tick);
	SanitizeMarker(m);
	// One marker per tick (CS2): placing on a tick that has one re-poses it, keeping its speed and ease.
	for (int i = 0; i < g.markers.Count(); ++i) {
		CamMarker& existing = g.markers.At(i);
		if (existing.tick != m.tick) continue;
		existing.x = m.x; existing.y = m.y; existing.z = m.z;
		existing.pitch = m.pitch; existing.yaw = m.yaw; existing.roll = m.roll; existing.fov = m.fov;
		g.markers.SetSelected(i);
		Rebuild();
		advancedfx::Message("[campath] marker #%d re-posed @ tick %d (%.1f %.1f %.1f) fov %.1f -- one marker per tick.\n",
			i, m.tick, m.x, m.y, m.z, m.fov);
		return;
	}
	const int index = g.markers.Add(m);
	Rebuild();
	advancedfx::Message("[campath] marker #%d placed @ tick %d (%.1f %.1f %.1f) fov %.1f.\n", index, m.tick, m.x, m.y, m.z, m.fov);
}

void CameraPath_Delete(int index) {
	if (!g.markers.DeleteIndex(index)) return;
	if (CameraPath_OwnsView()) CameraPath_Stop();
	Rebuild();
	advancedfx::Message("[campath] marker #%d deleted, %d left.\n", index, g.markers.Count());
}

bool CameraPath_DeleteAimed() {
	if (g.aimed < 0) return false;
	CameraPath_Delete(g.aimed);
	g.aimed = -1;
	return true;
}

void CameraPath_DeleteAll() {
	CameraPath_Stop();
	g.markers.DeleteAll();
	Rebuild();
	advancedfx::Message("[campath] all markers deleted.\n");
}

void CameraPath_Select(int index) {
	if (!g.markers.ValidIndex(index)) return;
	g.markers.SetSelected(index);
}

void CameraPath_Arm() {
	const int n = g.markers.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		return;
	}
	g.markers.SetSelected(0);
	Rebuild();
	if (!g.eval.CanEval()) {
		Notify("Camera path could not be built.");
		return;
	}
	g.play.Stop();
	DemoBar_SeekTo(g.markers.Front().tick, 1); // the first marker, paused, so the start frame holds still
	CameraBridge_SetFreeCamEnabled(true);
	g.play.Reset();
	SetMode(PathMode::Armed, "arm");
	advancedfx::Message("[campath] path armed at the first marker (tick %d): SPACE or J plays, ESC cancels.\n", g.markers.Front().tick);
}

void CameraPath_Play() {
	if (g.mode != PathMode::Armed) {
		CameraPath_Arm();
		if (g.mode != PathMode::Armed) return;
	}
	Rebuild();
	if (!g.eval.CanEval()) return;
	g.play.StartPlay(g.eval.Duration());
	SetMode(PathMode::Playing, "play");
	g.startPauseGuard = 30;
	CameraBridge_SetFreeCamEnabled(true);
	if (g.settings.timing == kTimingLive) {
		Command("demo_resume");
		g.resumeGuard = 120;
	}
	advancedfx::Message("[campath] PLAY: %d markers, speed %s, curve %s, timing %s, %.2f s.\n", g.markers.Count(),
		kSpeedModeNames[g.settings.speedMode], kInterpNames[g.settings.interp == 1 ? 1 : 0], kTimingNames[g.settings.timing == 1 ? 1 : 0],
		g.eval.Duration());
}

void CameraPath_Pause() {
	if (g.startPauseGuard > 0 || g.mode != PathMode::Playing) return; // a held Space cannot stop a start
	g.play.Stop();
	if (g.settings.timing == kTimingLive) Command("demo_pause");
	SetMode(PathMode::Editing, "pause");
	advancedfx::Message("[campath] playback paused at the current tick.\n");
}

void CameraPath_Stop() {
	const bool was = g.mode != PathMode::Editing;
	g.play.Stop();
	g.resumeGuard = 0;
	SetMode(PathMode::Editing, "stop");
	if (was) advancedfx::Message("[campath] preview stopped (back to editing).\n");
}

void CameraPath_Frame() {
	if (!Demo_State().playing) {
		if (g.mode != PathMode::Editing) CameraPath_Stop();
		g.aimed = -1;
		return;
	}
	// Markers belong to the demo they were placed in: another demo starts with an empty path. (A
	// backward seek replays the same file and keeps them.)
	const std::string demo = DemoIndex_Get().file;
	if (!demo.empty() && demo != g.demo) {
		if (!g.demo.empty() && !g.markers.Empty()) {
			g.markers.DeleteAll();
			Rebuild();
			advancedfx::Message("[campath] another demo: its camera path starts empty.\n");
		}
		g.demo = demo;
	}
	if (g.startPauseGuard > 0) --g.startPauseGuard;

	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	g.aimed = g.mode == PathMode::Editing ? CamMarkerPick::Aimed(g.markers.All(), o, a) : -1;

	if (g.mode == PathMode::Armed && !g.markers.Empty()) {
		PushPose(g.markers.Front()); // hold the start frame
	} else if (g.mode == PathMode::Playing) {
		if (g.resumeGuard > 0) {
			// The seek's own pause can land a few frames after play: in Live a paused demo here is
			// always the seek, never the user (Space leaves this mode).
			if (g.settings.timing == kTimingLive && Demo_State().paused) Command("demo_resume");
			--g.resumeGuard;
		}
		const bool done = g.play.TickPlay(g.markers, g.eval, MakeContext());
		if (g.settings.timing == kTimingLive && g.play.LiveEndSettling() && !done && !Demo_State().paused) Command("demo_pause");
		if (done && DemoBar_LoopOn() && g.markers.Count() >= 2 && g.eval.CanEval()) RestartPath(); // LOOP replays the path
		else if (done) FinishAtLastKey();
	}
}

bool CameraPath_OnKey(int vk, bool down, bool repeat) {
	const bool fresh = down && !repeat;
	auto act = [fresh](void (*fn)()) { if (fresh) MainThread_Run(fn); };
	switch (g.mode) {
	case PathMode::Armed:
		if (vk == VK_SPACE || vk == 'J') { act(&CameraPath_Play); return true; }
		if (vk == VK_ESCAPE) { act(&CameraPath_Stop); return true; }
		if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) return true; // held: the start frame stays
		return false;
	case PathMode::Playing:
		if (vk == VK_SPACE) { act(&CameraPath_Pause); return true; }
		if (vk == 'J' || vk == VK_ESCAPE) { act(&CameraPath_Stop); return true; }
		if (vk == 'W' || vk == 'A' || vk == 'S' || vk == 'D') { // stop the path, and the free cam moves on the same press
			act(&CameraPath_Stop);
			return false;
		}
		return false;
	default:
		if (vk == 'K') { act(&CameraPath_Place); return true; }
		if (vk == 'J') { act(&CameraPath_Arm); return true; }
		if (vk == 'L') {
			const int aimed = g.aimed;
			if (aimed < 0) return false; // nothing aimed at: the key is the game's
			if (fresh) MainThread_Run([aimed] { CameraPath_Delete(aimed); });
			return true;
		}
		return false;
	}
}

void CameraPath_Command(int argc, advancedfx::ICommandArgs* args) {
	const char* sub = argc >= 3 ? args->ArgV(2) : "list";
	const char* value = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(sub, "place")) CameraPath_Place();
	else if (0 == _stricmp(sub, "delete") && argc >= 4) CameraPath_Delete(atoi(value));
	else if (0 == _stricmp(sub, "deleteall") && 0 == _stricmp(value, "confirm")) CameraPath_DeleteAll();
	else if (0 == _stricmp(sub, "select") && argc >= 4) {
		const int sel = g.markers.Selected();
		if (0 == _stricmp(value, "next")) CameraPath_Select(sel + 1);
		else if (0 == _stricmp(value, "prev")) CameraPath_Select(sel - 1);
		else CameraPath_Select(atoi(value));
	} else if (0 == _stricmp(sub, "arm") || 0 == _stricmp(sub, "preview")) CameraPath_Arm();
	else if (0 == _stricmp(sub, "play")) CameraPath_Play();
	else if (0 == _stricmp(sub, "pause")) CameraPath_Pause();
	else if (0 == _stricmp(sub, "stop") || 0 == _stricmp(sub, "previewstop")) CameraPath_Stop();
	else if (0 == _stricmp(sub, "timing")) {
		int& t = g.settings.timing;
		if (0 == _stricmp(value, "live")) t = kTimingLive;
		else if (0 == _stricmp(value, "freeze")) t = kTimingFreeze;
		else t = t == kTimingLive ? kTimingFreeze : kTimingLive;
		Rebuild();
	} else if (0 == _stricmp(sub, "interp")) {
		int& i = g.settings.interp;
		if (0 == _stricmp(value, "linear")) i = 0;
		else if (0 == _stricmp(value, "smooth")) i = 1;
		else i = i == 0 ? 1 : 0;
		Rebuild();
	} else if (0 == _stricmp(sub, "speedmode")) {
		for (int i = 0; i < 3; ++i)
			if (0 == _stricmp(value, kSpeedModeNames[i]) || (i == 2 && 0 == _stricmp(value, "segment"))) g.settings.speedMode = i;
		Rebuild();
	} else if (0 == _stricmp(sub, "constspeed") && argc >= 4) {
		const float v = (float)atof(value);
		g.settings.constSpeed = v < 0.2f ? 0.2f : (v > 1.0f ? 1.0f : v);
		Rebuild();
	} else if (0 == _stricmp(sub, "speedmul") && argc >= 4 && g.markers.ValidIndex(g.markers.Selected())) {
		const float v = (float)atof(value);
		g.markers.At(g.markers.Selected()).speedMul = v > 0.0f ? v : 1.0f;
		Rebuild();
	} else if (0 == _stricmp(sub, "autosnap")) {
		bool on = !g.settings.autoSnap;
		Parse01(value, on);
		g.settings.autoSnap = on;
	} else if (0 != _stricmp(sub, "list")) {
		advancedfx::Message(
			"mirv_filmmaker marker place | delete <i> | deleteall confirm | select <i>|next|prev | arm | play | pause | stop\n"
			"mirv_filmmaker marker timing live|freeze|toggle | interp linear|smooth|cycle | speedmode manual|constant|persegment\n"
			"mirv_filmmaker marker constspeed <0.2..1> | speedmul <v> | autosnap [0|1] | list\n");
		return;
	}
	const PathState s = CameraPath_State();
	advancedfx::Message("camera path: %d markers, selected %d, aimed %d, %s, timing %s, curve %s, speed %s (const %.2f), %.2f s\n",
		s.count, s.selected, s.aimed, s.mode == PathMode::Editing ? "editing" : (s.mode == PathMode::Armed ? "armed" : "playing"),
		kTimingNames[s.settings.timing == 1 ? 1 : 0], kInterpNames[s.settings.interp == 1 ? 1 : 0],
		kSpeedModeNames[s.settings.speedMode >= 0 && s.settings.speedMode < 3 ? s.settings.speedMode : 0], s.settings.constSpeed, s.duration);
	for (int i = 0; i < g.markers.Count(); ++i) {
		const CamMarker& m = g.markers.At(i);
		advancedfx::Message("  #%d tick %d (%.1f %.1f %.1f) (%.1f %.1f %.1f) fov %.1f speed %.2f ease %d\n", i, m.tick, m.x, m.y, m.z,
			m.pitch, m.yaw, m.roll, m.fov, m.speedMul, (int)m.ease);
	}
}

void CameraPath_PrintHealth() {
	const PathState s = CameraPath_State();
	advancedfx::Message("  camera path: %d markers, %s, timing %s, aimed %d\n", s.count,
		s.mode == PathMode::Editing ? "editing" : (s.mode == PathMode::Armed ? "armed" : "playing"),
		kTimingNames[s.settings.timing == 1 ? 1 : 0], s.aimed);
}

} // namespace SourceFilmmaker

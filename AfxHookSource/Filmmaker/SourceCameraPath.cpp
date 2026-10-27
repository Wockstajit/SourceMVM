#include "stdafx.h"

#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceFollowCamera.h"
#include "SourceMainThread.h"
#include "SourceOldDemos.h"
#include "SourceTimelineDock.h"

#include "Ui/PathOverlay.h"

#include "WrpVEngineClient.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Demo/DemoProjectStore.h>
#include <AfxHookSource2/Filmmaker/Platform/TextEncoding.h>
#include <AfxHookSource2/Filmmaker/Movie/CamMarkerPickMath.h>
#include <AfxHookSource2/Filmmaker/Movie/CamPathEval.h>
#include <AfxHookSource2/Filmmaker/Movie/CamPathShake.h>
#include <AfxHookSource2/Filmmaker/Movie/CamPathShakeHost.h>
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
	std::string demo;         // the demo the markers were placed in, as the engine names it
	std::wstring project;     // the demo the user played, whose project.mvm keeps the path
	bool dirty = false;       // an edit not saved yet
	std::vector<double> trajectory; // the in-world line: x, y, z along the path
	// The editor's selection: the camera whose pose holds while the seek to its tick lands.
	bool menuOpen = false;    // the CAMERA MARKER card
	bool autoPath = false;    // AUTO on the TIMELINE dock's CAMERA PATH layer
	bool autoPlaying = false; // the path is playing because of AUTO
	int holdIndex = -1;
	int holdFrames = 0;       // at most this many more frames
	int holdSettled = 0;      // frames the demo has been on the camera's tick
	// Value edits and retimes, one undo step each (CS2's curve undo).
	struct Snapshot {
		std::vector<CamMarker> markers;
		int selected = -1;
	};
	bool editActive = false;
	bool editChanged = false;
	Snapshot editStart;
	std::vector<Snapshot> undo, redo;
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
// An edit to save: the path goes to the demo's project.mvm at the end of the frame, as CS2's does.
void MarkDirty() { g.dirty = true; }

// The demo's project, as CS2 keeps it (Demo/DemoProjectStore.h) under %APPDATA%\HLAE\source_mvm\demos\css\:
// keyed on the file the user played, the original of a converted old demo.
std::wstring ProjectDemoPath(const std::string& engineName) {
	const char* dir = g_VEngineClient ? g_VEngineClient->GetGameDirectory() : nullptr;
	if (!dir || !*dir || engineName.empty()) return {};
	std::string path = std::string(dir) + "/" + OldDemos_PlayedName(engineName);
	for (char& c : path) if (c == '/') c = '\\';
	return Utf8ToWide(path);
}

void Rebuild();

void Save() {
	g.dirty = false;
	if (g.project.empty()) return; // no demo: memory only
	DemoProjectStore_WriteSection(ProjectGame::Css, g.project, ProjectSection::Campath, g.markers.Serialize(g.settings, g.markers.Selected()));
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("campath", "saved %d markers to %s", g.markers.Count(), WideToUtf8(g.project).c_str());
}

// The playing demo's path from its project, or an empty one.
void Load() {
	g.markers.DeleteAll();
	g.undo.clear();
	g.redo.clear();
	g.menuOpen = false;
	std::string payload;
	PathSettings settings = g.settings;
	int selected = -1;
	if (!g.project.empty() && DemoProjectStore_ReadSection(ProjectGame::Css, g.project, ProjectSection::Campath, payload)
		&& !payload.empty() && g.markers.Parse(payload, settings, selected)) {
		g.settings = settings;
		advancedfx::Message("[campath] loaded %d markers from the demo's project.mvm.\n", g.markers.Count());
	}
	Rebuild();
	g.dirty = false;
}

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
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("campath", "mode=%s (%s)", PathModeName(mode), why);
}

State::Snapshot TakeSnapshot() { return { g.markers.All(), g.markers.Selected() }; }

void PushSnapshot(std::vector<State::Snapshot>& stack, State::Snapshot snapshot) {
	if (stack.size() >= 64) stack.erase(stack.begin());
	stack.push_back(std::move(snapshot));
}

void PushUndo() {
	PushSnapshot(g.undo, TakeSnapshot());
	g.redo.clear(); // a fresh edit drops what was undone
}

double* ChannelOf(CamMarker& m, int channel) {
	switch (channel) {
	case kChannelX: return &m.x;
	case kChannelY: return &m.y;
	case kChannelZ: return &m.z;
	case kChannelPitch: return &m.pitch;
	case kChannelYaw: return &m.yaw;
	case kChannelRoll: return &m.roll;
	case kChannelFov: return &m.fov;
	default: return nullptr;
	}
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

void EndAutoPath(const char* why) {
	if (!g.autoPlaying) return;
	g.autoPlaying = false;
	g.play.Stop();
	g.resumeGuard = 0;
	SetMode(PathMode::Editing, "auto end");
	CameraBridge_SetFreeCamEnabled(false);
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("campath.auto", "end (%s): the view is the game's again", why);
}

// CS2's RunAutoPath: with AUTO on and the TIMELINE dock open, Live playback through the path's keys
// flies them, range-gated (CamPlayback: the view is the game's before the first key).
void RunAutoPath() {
	const DemoState d = Demo_State();
	int seekTarget = 0;
	const bool seeking = DemoBar_SeekTarget(seekTarget); // a backward seek replays from the start: not playback
	const bool following = FollowCamera_OwnsView();
	const bool want = g.autoPath && TimelineDock_Enabled() && g.settings.timing == kTimingLive && g.markers.Count() >= 2
		&& d.playing && !d.paused && !seeking && !following;
	if (g.autoPlaying) {
		if (g.mode != PathMode::Playing) { g.autoPlaying = false; return; } // J, Esc or a pause ended it
		if (!want) EndAutoPath(!g.autoPath ? "AUTO off" : !TimelineDock_Enabled() ? "the TIMELINE dock closed"
			: g.settings.timing != kTimingLive ? "Freeze timing" : g.markers.Count() < 2 ? "under two camera keys"
			: !d.playing ? "the demo stopped" : seeking ? "a seek" : following ? "the follow camera" : "the demo paused");
		return;
	}
	if (!want || g.mode != PathMode::Editing || g.holdFrames > 0) return;
	if (d.tick >= g.markers.Back().tick) return; // past it this pass
	Rebuild();
	if (!g.eval.CanEval()) return;
	g.play.StartPlay(g.eval.Duration(), /*rangeGated*/ true);
	SetMode(PathMode::Playing, "auto");
	g.autoPlaying = true;
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("campath.auto", "start at tick %d: the view follows the path inside %d..%d", d.tick,
			g.markers.Front().tick, g.markers.Back().tick);
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

const char* PathModeName(PathMode mode) {
	switch (mode) {
	case PathMode::Editing: return "editing";
	case PathMode::Reposition: return "repositioning";
	case PathMode::Armed: return "armed";
	default: return "playing";
	}
}

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
	s.menuOpen = CameraPath_MenuOpen();
	return s;
}

bool CameraPath_MenuOpen() { return g.menuOpen && g.mode == PathMode::Editing && !g.markers.Empty(); }

void CameraPath_OpenMenu(int index) {
	if (!g.markers.ValidIndex(index)) return;
	if (g.mode != PathMode::Editing) CameraPath_Stop();
	g.markers.SetSelected(index);
	CameraBridge_SetFreeCamEnabled(true);
	g.menuOpen = true;
	advancedfx::Message("[campath] edit menu open for marker #%d.\n", index);
}

void CameraPath_CloseMenu() { g.menuOpen = false; }

void CameraPath_SelectStep(int delta) {
	if (g.markers.Empty()) return;
	const int base = g.markers.Selected() < 0 ? 0 : g.markers.Selected();
	g.markers.SetSelected(base + delta);
	const int sel = g.markers.Selected();
	if (g.settings.autoSnap) PushPose(g.markers.At(sel)); // only with auto-snap does the view follow
	advancedfx::Message("[campath] selected marker #%d%s.\n", sel, g.settings.autoSnap ? "" : " (auto-snap off, view not moved)");
}

// AUTO is playing but the demo is not inside the path yet: the view, and the mouse, are the game's.
bool AutoWaiting() { return g.autoPlaying && g.mode == PathMode::Playing && !CameraBridge_GetFreeCamEnabled(); }

bool CameraPath_OwnsView() { return g.mode == PathMode::Armed || (g.mode == PathMode::Playing && !AutoWaiting()); }

void CameraPath_ReplaceMarkersFromGraph(const std::vector<CamMarker>& markers, bool pushUndo) {
	if (pushUndo) PushUndo();
	int sel = g.markers.Selected();
	if (sel >= (int)markers.size()) sel = (int)markers.size() - 1;
	if (!markers.empty() && sel < 0) sel = 0;
	g.markers.Restore(markers, sel);
	Rebuild();
	MarkDirty();
}

bool CameraPath_EvalPoseAtTick(double tick, double out[7]) {
	if (g.markers.Count() < 2 || !g.eval.CanEval()) return false;
	const CamPathEval::Pose p = g.eval.EvalAtTick(g.markers, tick);
	if (!p.valid) return false;
	out[0] = p.x; out[1] = p.y; out[2] = p.z;
	out[3] = p.pitch; out[4] = p.yaw; out[5] = p.roll;
	out[6] = p.fov;
	return true;
}

bool CameraPath_AutoPath() { return g.autoPath; }

void CameraPath_SetAutoPath(bool on) {
	g.autoPath = on;
	advancedfx::Message("[campath] AUTO %s: %s\n", on ? "on" : "off", on
		? "while the timeline plays, the view follows the camera path inside its keys."
		: "Space plays the demo; J plays the camera path.");
}

bool CameraPath_Repositioning() { return g.mode == PathMode::Reposition; }

bool CameraPath_Overlay(Ui::PathOverlayModel& out, float width, float height) {
	if (g.markers.Empty() || g.mode == PathMode::Playing || FollowCamera_OwnsView()) return false; // nothing over a live shot
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
		MarkDirty();
		advancedfx::Message("[campath] marker #%d re-posed @ tick %d (%.1f %.1f %.1f) fov %.1f -- one marker per tick.\n",
			i, m.tick, m.x, m.y, m.z, m.fov);
		return;
	}
	const bool curves = CamMarkersHaveCurves(g.markers);
	const int index = g.markers.Add(m);
	if (curves && g.settings.interp == 1) CamMarkersAutoTangent(g.markers, index); // joins the curve, no corner
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d placed @ tick %d (%.1f %.1f %.1f) fov %.1f.\n", index, m.tick, m.x, m.y, m.z, m.fov);
}

void CameraPath_Delete(int index) {
	if (!g.markers.DeleteIndex(index)) return;
	if (CameraPath_OwnsView()) CameraPath_Stop();
	Rebuild();
	MarkDirty();
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
	g.menuOpen = false;
	g.markers.DeleteAll();
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] all markers deleted.\n");
}

void CameraPath_Select(int index) {
	if (!g.markers.ValidIndex(index)) return;
	g.markers.SetSelected(index);
}

void CameraPath_SelectForEditor(int index) {
	if (g.markers.Empty()) return;
	CameraPath_EditEnd();
	if (g.mode != PathMode::Editing) CameraPath_Stop();
	g.markers.SetSelected(index);
	const CamMarker& m = g.markers.At(g.markers.Selected());
	PushPose(m);
	DemoBar_SeekTo(m.tick, 1); // paused on the camera's tick
	// The pose holds until the seek lands (a backward seek restarts the demo first), then the free
	// cam is the user's again: selecting must not pin the view to the camera.
	g.holdIndex = g.markers.Selected();
	g.holdFrames = 1200;
	g.holdSettled = 0;
	advancedfx::Message("[campath] editor selected camera #%d @ tick %d.\n", g.markers.Selected(), m.tick);
}

void CameraPath_SelectDelta(int delta) {
	if (g.markers.Empty()) return;
	const int base = g.markers.Selected() < 0 ? 0 : g.markers.Selected();
	int next = base + delta;
	if (next < 0) next = 0;
	if (next >= g.markers.Count()) next = g.markers.Count() - 1;
	CameraPath_SelectForEditor(next);
}

void CameraPath_MoveKey(int index, int tick) {
	if (!g.markers.ValidIndex(index)) return;
	// Kept between the neighbours' ticks, so the cameras stay in tick order.
	const int lo = index > 0 ? g.markers.At(index - 1).tick + 1 : 0;
	if (tick < lo) tick = lo;
	if (index < g.markers.Count() - 1) {
		const int hi = g.markers.At(index + 1).tick - 1;
		if (tick > hi) tick = hi;
	}
	if (tick < 0) tick = 0;
	if (tick == g.markers.At(index).tick) return;
	CameraPath_EditEnd();
	PushUndo();
	CamMarker& m = g.markers.At(index);
	m.tick = tick;
	const double interval = Demo_State().intervalPerTick;
	if (interval > 0) m.time = tick * interval;
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d moved to tick %d.\n", index, tick);
}

void CameraPath_EditBegin() {
	if (g.editActive) return;
	g.editStart = TakeSnapshot();
	g.editActive = true;
	g.editChanged = false;
}

void CameraPath_EditValue(int index, int channel, double value) {
	if (!g.markers.ValidIndex(index)) return;
	if (!g.editActive) CameraPath_EditBegin();
	CamMarker& m = g.markers.At(index);
	double* target = ChannelOf(m, channel);
	if (!target || *target == value) return;
	*target = value;
	SanitizeMarker(m);
	g.editChanged = true;
	Rebuild();
	PushPose(m); // the view shows the camera as it is edited
}

void CameraPath_EditEnd() {
	if (!g.editActive) return;
	if (g.editChanged) {
		PushSnapshot(g.undo, std::move(g.editStart));
		g.redo.clear();
		MarkDirty();
	}
	g.editActive = false;
	g.editChanged = false;
}

void CameraPath_Undo() {
	CameraPath_EditEnd();
	if (g.undo.empty()) {
		Notify("Nothing to undo.");
		return;
	}
	PushSnapshot(g.redo, TakeSnapshot());
	const State::Snapshot s = g.undo.back();
	g.undo.pop_back();
	g.markers.Restore(s.markers, s.selected);
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] undo: %d markers.\n", g.markers.Count());
}

void CameraPath_Redo() {
	CameraPath_EditEnd();
	if (g.redo.empty()) {
		Notify("Nothing to redo.");
		return;
	}
	PushSnapshot(g.undo, TakeSnapshot());
	const State::Snapshot s = g.redo.back();
	g.redo.pop_back();
	g.markers.Restore(s.markers, s.selected);
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] redo: %d markers.\n", g.markers.Count());
}

void CameraPath_SetEase(int index, Ease ease) {
	if (!g.markers.ValidIndex(index)) return;
	g.markers.At(index).ease = ease; // eased at evaluation: the line keeps its shape
	MarkDirty();
	advancedfx::Message("[campath] marker #%d ease -> %d.\n", index, (int)ease);
}

void CameraPath_SetSpeedMul(int index, float mul) {
	if (!g.markers.ValidIndex(index)) return;
	if (index == g.markers.Count() - 1) {
		Notify("Last marker has no outgoing segment.");
		return;
	}
	g.markers.At(index).speedMul = mul < 0.2f ? 0.2f : (mul > 1.0f ? 1.0f : mul);
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d outgoing segment speed x%.2f.\n", index, g.markers.At(index).speedMul);
}

void CameraPath_SetConstSpeed(float mul) {
	g.settings.constSpeed = mul < 0.2f ? 0.2f : (mul > 1.0f ? 1.0f : mul);
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] constant speed x%.2f (whole path).\n", g.settings.constSpeed);
}

void CameraPath_SetInterp(int interp) {
	g.settings.interp = interp == 1 ? 1 : 0;
	// A path that follows the graph's curves takes the mode as a reshape of those curves, the way the
	// graph's own Smooth and Linear do; a path the graph never shaped keeps its spline.
	if (CamMarkersHaveCurves(g.markers)) {
		if (g.settings.interp == 1) {
			CamMarkersAutoTangent(g.markers, -1);
		} else {
			for (int k = 0; k < g.markers.Count(); ++k)
				for (int c = 0; c < 7; ++c) {
					g.markers.At(k).in[c] = GraphCurve::Handle();
					g.markers.At(k).out[c] = GraphCurve::Handle();
				}
		}
	}
	TimelineDock_MirrorPathInterp(g.settings.interp == 1);
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] interpolation -> %s.\n", kInterpNames[g.settings.interp]);
}

void CameraPath_SetSpeedMode(int mode) {
	g.settings.speedMode = mode >= 0 && mode < 3 ? mode : 0;
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] speed mode -> %s.\n", kSpeedModeNames[g.settings.speedMode]);
}

void CameraPath_SetTiming(int timing) {
	g.settings.timing = timing == kTimingFreeze ? kTimingFreeze : kTimingLive;
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] timing -> %s (all cameras).\n", kTimingNames[g.settings.timing]);
}

void CameraPath_SetAutoSnap(bool on) {
	g.settings.autoSnap = on;
	MarkDirty();
	advancedfx::Message("[campath] auto-snap to selected camera -> %s.\n", on ? "ON" : "OFF");
}

void CameraPath_BeginReposition() {
	const int sel = g.markers.Selected();
	if (!g.markers.ValidIndex(sel)) {
		Notify("Select a camera to reposition.");
		return;
	}
	g.menuOpen = false;
	if (g.mode != PathMode::Editing) CameraPath_Stop();
	CameraPath_EditEnd();
	g.holdFrames = 0;
	// At the camera's own moment: the demo goes to its tick first.
	DemoBar_SeekTo(g.markers.At(sel).tick, 1);
	CameraBridge_SetFreeCamEnabled(true);
	SetMode(PathMode::Reposition, "reposition");
	advancedfx::Message("[campath] repositioning marker #%d @ tick %d: fly the free cam there, then F or a left click places it (Esc cancels).\n",
		sel, g.markers.At(sel).tick);
}

void CameraPath_PlaceReposition() {
	if (g.mode != PathMode::Reposition) return;
	SetMode(PathMode::Editing, "placed");
	const int sel = g.markers.Selected();
	if (!g.markers.ValidIndex(sel)) return;
	CamMarker m = g.markers.At(sel);
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	// The demo's tick now, unless another camera already has it (one per tick).
	double time = 0;
	int tick = m.tick;
	CameraBridge_GetDemoClock(time, tick);
	bool taken = false;
	for (int i = 0; i < g.markers.Count(); ++i) taken = taken || (i != sel && g.markers.At(i).tick == tick);
	if (!taken && tick >= 0) {
		m.tick = tick;
		m.time = time;
	}
	PushUndo();
	g.markers.DeleteIndex(sel);
	const int index = g.markers.Add(m); // back in tick order
	Rebuild();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d repositioned @ tick %d (%.1f %.1f %.1f).\n", index, m.tick, m.x, m.y, m.z);
}

void CameraPath_CancelReposition() {
	if (g.mode != PathMode::Reposition) return;
	SetMode(PathMode::Editing, "reposition cancelled");
	advancedfx::Message("[campath] reposition cancelled.\n");
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
	if (g.mode == PathMode::Reposition) {
		CameraPath_CancelReposition();
		return;
	}
	const bool was = g.mode != PathMode::Editing;
	g.play.Stop();
	g.resumeGuard = 0;
	SetMode(PathMode::Editing, "stop");
	if (was) advancedfx::Message("[campath] preview stopped (back to editing).\n");
}

void CameraPath_Frame() {
	Filmmaker::CamPathShake_RunFrame(); // the keys' shakes follow this path's demo and retimes
	if (g.dirty && !g.editActive) Save(); // last frame's edits, once a drag has ended
	if (!Demo_State().playing) {
		if (g.mode != PathMode::Editing) CameraPath_Stop();
		g.aimed = -1;
		return;
	}
	// Markers belong to the demo they were placed in, saved in its project.mvm: another demo brings
	// its own path back, or starts empty. (A backward seek replays the same file and keeps them.)
	const std::string demo = DemoIndex_Get().file;
	if (!demo.empty() && demo != g.demo) {
		if (g.dirty) Save(); // the outgoing demo's last edit
		if (g.mode != PathMode::Editing) CameraPath_Stop();
		g.demo = demo;
		g.project = ProjectDemoPath(demo);
		Load();
	}
	if (g.startPauseGuard > 0) --g.startPauseGuard;

	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	g.aimed = g.mode == PathMode::Editing ? CamMarkerPick::Aimed(g.markers.All(), o, a) : -1;

	if (g.holdFrames > 0) { // the editor's selection, until its seek lands
		if (g.mode != PathMode::Editing || !g.markers.ValidIndex(g.holdIndex)) {
			g.holdFrames = 0;
		} else {
			const CamMarker& m = g.markers.At(g.holdIndex);
			PushPose(m);
			--g.holdFrames;
			const DemoState d = Demo_State();
			if (d.tick >= m.tick - 1 && d.tick <= m.tick + 1 && ++g.holdSettled >= 10) g.holdFrames = 0;
		}
	}

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
		if (g.autoPlaying) {
			if (done) EndAutoPath("end of path"); // the demo plays on, the game's camera takes the view
		} else {
			if (g.settings.timing == kTimingLive && g.play.LiveEndSettling() && !done && !Demo_State().paused) Command("demo_pause");
			if (done && DemoBar_LoopOn() && g.markers.Count() >= 2 && g.eval.CanEval()) RestartPath(); // LOOP replays the path
			else if (done) FinishAtLastKey();
		}
	}
	RunAutoPath();
}

bool CameraPath_OnKey(int vk, bool down, bool repeat) {
	const bool fresh = down && !repeat;
	auto act = [fresh](void (*fn)()) { if (fresh) MainThread_Run(fn); };
	switch (g.mode) {
	case PathMode::Reposition:
		if (vk == 'F' || vk == VK_RETURN) { act(&CameraPath_PlaceReposition); return true; }
		if (vk == VK_ESCAPE) { act(&CameraPath_CancelReposition); return true; }
		return false; // W A S D and the rest fly the free cam
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
		if (CameraPath_MenuOpen()) { // the card is up: F and Esc close it, nothing else is the path's
			if (vk == 'F' || vk == VK_ESCAPE) { act(&CameraPath_CloseMenu); return true; }
			return false;
		}
		if (vk == 'F') {
			const int aimed = g.aimed;
			if (aimed < 0) return false; // nothing aimed at: the key is the game's
			if (fresh) MainThread_Run([aimed] { CameraPath_OpenMenu(aimed); });
			return true;
		}
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
	const char* value2 = argc >= 5 ? args->ArgV(4) : "";
	const int sel = g.markers.Selected();
	if (0 == _stricmp(sub, "place")) CameraPath_Place();
	else if (0 == _stricmp(sub, "delete") && argc >= 4) CameraPath_Delete(atoi(value));
	else if (0 == _stricmp(sub, "deleteall") && 0 == _stricmp(value, "confirm")) CameraPath_DeleteAll();
	else if (0 == _stricmp(sub, "select") && argc >= 4) {
		if (0 == _stricmp(value, "next")) CameraPath_SelectDelta(1);
		else if (0 == _stricmp(value, "prev")) CameraPath_SelectDelta(-1);
		else CameraPath_SelectForEditor(atoi(value));
	} else if (0 == _stricmp(sub, "selectdelta")) CameraPath_SelectDelta(argc >= 4 ? atoi(value) : 1);
	else if (0 == _stricmp(sub, "movekey") && argc >= 5) CameraPath_MoveKey(atoi(value), atoi(value2));
	else if (0 == _stricmp(sub, "setval") && argc >= 6) { // one value as one undo step
		CameraPath_EditBegin();
		CameraPath_EditValue(atoi(value), atoi(value2), atof(args->ArgV(5)));
		CameraPath_EditEnd();
	} else if ((0 == _stricmp(sub, "fov") || 0 == _stricmp(sub, "roll")) && argc >= 4 && g.markers.ValidIndex(sel)) {
		CameraPath_EditBegin();
		CameraPath_EditValue(sel, 0 == _stricmp(sub, "fov") ? kChannelFov : kChannelRoll, atof(value));
		CameraPath_EditEnd();
	} else if (0 == _stricmp(sub, "undo")) CameraPath_Undo();
	else if (0 == _stricmp(sub, "redo")) CameraPath_Redo();
	else if (0 == _stricmp(sub, "ease") && argc >= 5) {
		Ease e = Ease::None;
		if (0 == _stricmp(value2, "in")) e = Ease::In;
		else if (0 == _stricmp(value2, "out")) e = Ease::Out;
		else if (0 == _stricmp(value2, "inout")) e = Ease::InOut;
		CameraPath_SetEase(atoi(value), e);
	} else if (0 == _stricmp(sub, "speed") && argc >= 5) CameraPath_SetSpeedMul(atoi(value), (float)atof(value2));
	else if (0 == _stricmp(sub, "speedmul") && argc >= 4) CameraPath_SetSpeedMul(sel, (float)atof(value));
	else if (0 == _stricmp(sub, "next")) CameraPath_SelectStep(1);
	else if (0 == _stricmp(sub, "prev")) CameraPath_SelectStep(-1);
	else if (0 == _stricmp(sub, "open")) CameraPath_OpenMenu(argc >= 4 ? atoi(value) : (g.aimed >= 0 ? g.aimed : sel));
	else if (0 == _stricmp(sub, "close")) CameraPath_CloseMenu();
	else if (0 == _stricmp(sub, "reposition")) CameraPath_BeginReposition();
	else if (0 == _stricmp(sub, "placereposition")) CameraPath_PlaceReposition();
	else if (0 == _stricmp(sub, "cancelreposition")) CameraPath_CancelReposition();
	else if (0 == _stricmp(sub, "arm") || 0 == _stricmp(sub, "preview")) CameraPath_Arm();
	else if (0 == _stricmp(sub, "play")) CameraPath_Play();
	else if (0 == _stricmp(sub, "pause")) CameraPath_Pause();
	else if (0 == _stricmp(sub, "stop") || 0 == _stricmp(sub, "previewstop")) CameraPath_Stop();
	else if (0 == _stricmp(sub, "timing")) {
		if (0 == _stricmp(value, "live")) CameraPath_SetTiming(kTimingLive);
		else if (0 == _stricmp(value, "freeze")) CameraPath_SetTiming(kTimingFreeze);
		else CameraPath_SetTiming(g.settings.timing == kTimingLive ? kTimingFreeze : kTimingLive);
	} else if (0 == _stricmp(sub, "interp")) {
		if (0 == _stricmp(value, "linear")) CameraPath_SetInterp(0);
		else if (0 == _stricmp(value, "smooth")) CameraPath_SetInterp(1);
		else CameraPath_SetInterp(g.settings.interp == 0 ? 1 : 0);
	} else if (0 == _stricmp(sub, "speedmode")) {
		int mode = (g.settings.speedMode + 1) % 3;
		for (int i = 0; i < 3; ++i)
			if (0 == _stricmp(value, kSpeedModeNames[i]) || (i == 2 && 0 == _stricmp(value, "segment"))) mode = i;
		CameraPath_SetSpeedMode(mode);
	} else if (0 == _stricmp(sub, "constspeed") && argc >= 4) CameraPath_SetConstSpeed((float)atof(value));
	else if (0 == _stricmp(sub, "autosnap")) {
		bool on = !g.settings.autoSnap;
		Parse01(value, on);
		CameraPath_SetAutoSnap(on);
	} else if (0 != _stricmp(sub, "list")) {
		advancedfx::Message(
			"mirv_filmmaker marker place | delete <i> | deleteall confirm | select <i>|next|prev | selectdelta <d> | movekey <i> <tick>\n"
			"mirv_filmmaker marker setval <i> <channel 0-6> <v> | fov <v> | roll <v> | undo | redo | ease <i> none|in|out|inout\n"
			"mirv_filmmaker marker speed <i> <0.2..1> | speedmul <v> | constspeed <0.2..1> | reposition | placereposition | cancelreposition\n"
			"mirv_filmmaker marker open [i] | close | next | prev | arm | play | pause | stop | timing live|freeze|toggle\n"
			"mirv_filmmaker marker interp linear|smooth|cycle\n"
			"mirv_filmmaker marker speedmode manual|constant|persegment | autosnap [0|1] | list\n");
		return;
	}
	const PathState s = CameraPath_State();
	advancedfx::Message("camera path: %d markers, selected %d, aimed %d, %s, timing %s, curve %s, speed %s (const %.2f), %.2f s, undo %d, redo %d\n",
		s.count, s.selected, s.aimed, PathModeName(s.mode),
		kTimingNames[s.settings.timing == 1 ? 1 : 0], kInterpNames[s.settings.interp == 1 ? 1 : 0],
		kSpeedModeNames[s.settings.speedMode >= 0 && s.settings.speedMode < 3 ? s.settings.speedMode : 0], s.settings.constSpeed, s.duration,
		(int)g.undo.size(), (int)g.redo.size());
	for (int i = 0; i < g.markers.Count(); ++i) {
		const CamMarker& m = g.markers.At(i);
		advancedfx::Message("  #%d tick %d (%.1f %.1f %.1f) (%.1f %.1f %.1f) fov %.1f speed %.2f ease %d\n", i, m.tick, m.x, m.y, m.z,
			m.pitch, m.yaw, m.roll, m.fov, m.speedMul, (int)m.ease);
	}
}

void CameraPath_PrintHealth() {
	const PathState s = CameraPath_State();
	advancedfx::Message("  camera path: %d markers, %s, timing %s, selected %d, aimed %d\n", s.count, PathModeName(s.mode),
		kTimingNames[s.settings.timing == 1 ? 1 : 0], s.selected, s.aimed);
}

} // namespace SourceFilmmaker

// Path shake's host (CamPathShakeHost.h): this path's keys, its playback tick and its demo's project.
// CS:S has no camera objects to look through.
namespace Filmmaker {

const std::vector<CamMarker>& PathShakeHost_Markers() { return SourceFilmmaker::g.markers.All(); }

bool PathShakeHost_PathTick(double& outTick) {
	if (!SourceFilmmaker::g.play.Playing() && !SourceFilmmaker::g.play.Scrubbing()) return false;
	outTick = SourceFilmmaker::g.play.LastEvalTick();
	return true;
}

double PathShakeHost_ObjectTick() { return 0.0; }

std::wstring PathShakeHost_DemoPath() { return SourceFilmmaker::g.project; }

ProjectGame PathShakeHost_Game() { return ProjectGame::Css; }

double PathShakeHost_IntervalPerTick() {
	const double interval = SourceFilmmaker::Demo_State().intervalPerTick;
	return interval > 0.0 ? interval : 0.015;
}

} // namespace Filmmaker

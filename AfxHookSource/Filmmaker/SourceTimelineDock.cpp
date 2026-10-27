#include "stdafx.h"

#include "SourceTimelineDock.h"
#include "SourceCameraPath.h"
#include "SourceDemoBar.h"
#include "SourceDemoIndex.h"
#include "SourceEngine.h"
#include "SourceFollowCamera.h"
#include "SourceTimelinePlayhead.h"
#include "SourceUi.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraBridge.h>
#include <Shared/AfxConsole.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace SourceFilmmaker {

namespace {

using Filmmaker::CamMarker;
using Filmmaker::GraphExpModel;
using Filmmaker::CameraBridge_GetCurrentCamera;
using Filmmaker::CameraBridge_GetFreeCamEnabled;
using Filmmaker::CameraBridge_SetCameraPose;
using Filmmaker::CameraBridge_SetFreeCamEnabled;

struct State {
	GraphExpModel model;
	bool enabled = false;
	bool wasEnabled = false;
	bool drive = true;
	unsigned rev = 0;
	unsigned viewEpoch = 0;
	bool viewInit = false;
	double viewT0 = 0, viewT1 = 1;
	std::vector<CamMarker> pathSeen; // the markers this dock last seeded or baked
	bool pathSeenValid = false;
	bool pathUndoPending = false;    // EditBegin: the next bake is one path undo step
	int editDriveFrames = 0;         // show the path's pose for a couple of frames after a bake
	bool releaseDrive = false;       // a release with AUTO on: fly the released tick until the demo lands
	std::string demo;                // the demo the model belongs to
};
State g;

void BumpRev() { ++g.rev; }

std::string KeyCounts() {
	std::string s;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c) {
		if (c) s += ',';
		s += std::to_string(g.model.GetChannel(c).keys.size());
	}
	return s;
}

bool SmoothPath() { return CameraPath_State().settings.interp == 1; }

double IntervalPerTick() {
	const double ipt = Demo_State().intervalPerTick;
	return ipt > 0 ? ipt : 0.015;
}

// A path change the dock did not make: seed an empty model, reconcile a kept one (keys that still
// exist keep their handles and selection; the undo history goes, so an undo never bakes an old
// marker set back).
bool AdoptPathChanges(const char* why) {
	const std::vector<CamMarker>& markers = CameraPath_Markers().All();
	if (g.pathSeenValid && GraphExpModel::SameMarkers(markers, g.pathSeen)) return false;
	const int keysBefore = g.model.TotalKeys();
	const bool smooth = SmoothPath();
	bool changed;
	if (keysBefore == 0) {
		g.model.SeedFromMarkers(markers);
		// Seeded keys read as Linear; a Smooth path the graph never shaped opens on its Smooth curve.
		if (smooth && !GraphExpModel::MarkersHaveCurves(markers)) g.model.SetInterpAll(true, false);
		g.viewInit = false;
		++g.viewEpoch;
		changed = g.model.TotalKeys() > 0;
	} else {
		changed = g.model.ReconcileWithMarkers(markers, smooth);
	}
	g.pathSeen = markers;
	g.pathSeenValid = true;
	if (changed) BumpRev();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("graph.adopt", "why=%s pathCount=%d keysBefore=%d keys=%s changed=%d seeded=%d", why,
			(int)markers.size(), keysBefore, KeyCounts().c_str(), changed ? 1 : 0, keysBefore == 0 ? 1 : 0);
	return changed;
}

// Every edit: the keys become the path's markers (one per whole tick, each channel's handles
// carried, segments split exactly where a channel has no key), so the camera flies what the dock
// draws.
void SyncToCameraPath() {
	if (g.model.RefreshAutoTangents()) BumpRev();
	if (!g.enabled || !Demo_State().playing) {
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("graph.sync", "skipped enabled=%d demo=%d keys=%s", g.enabled ? 1 : 0,
				Demo_State().playing ? 1 : 0, KeyCounts().c_str());
		return;
	}
	AdoptPathChanges("sync");
	const std::vector<CamMarker> baked = g.model.BakeMarkers(CameraPath_Markers().All(), IntervalPerTick());
	const bool pushUndo = g.pathUndoPending;
	g.pathUndoPending = false;
	const int before = CameraPath_Markers().Count();
	CameraPath_ReplaceMarkersFromGraph(baked, pushUndo);
	g.pathSeen = CameraPath_Markers().All();
	g.pathSeenValid = true;
	g.editDriveFrames = 2;
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned n = 0;
		static size_t lastBaked = (size_t)-1;
		const bool structural = baked.size() != lastBaked;
		lastBaked = baked.size();
		if (structural || (n++ % 8) == 0)
			Filmmaker::MvmDebugLog_Linef("graph.sync", "baked keys=%s markers=%d pathCount=%d->%d undo=%d tick0=%d tickN=%d",
				KeyCounts().c_str(), (int)baked.size(), before, CameraPath_Markers().Count(), pushUndo ? 1 : 0,
				baked.empty() ? -1 : baked.front().tick, baked.empty() ? -1 : baked.back().tick);
	}
}

void OnEnter() {
	// The model is kept between visits so graph-only shape survives a trip to the other dock tab,
	// but the path may have changed meanwhile: adopt it.
	AdoptPathChanges("enter");
	g.viewInit = false;
	++g.viewEpoch;
	TimelinePlayhead_EndScrub();
	g.drive = true;
	BumpRev();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("graph.mode", "enter keys=%s total=%d pathCount=%d", KeyCounts().c_str(),
			g.model.TotalKeys(), CameraPath_Markers().Count());
}

void OnExit() {
	g.releaseDrive = false;
	TimelinePlayhead_EndScrub();
	if (Filmmaker::MvmDebugLog_Active())
		Filmmaker::MvmDebugLog_Linef("graph.mode", "exit keys=%s total=%d", KeyCounts().c_str(), g.model.TotalKeys());
}

// A scrub flies the path only with AUTO on; an edit and a release always show it.
bool ScrubDrives() { return TimelinePlayhead_Scrubbing() && CameraPath_AutoPath(); }
bool DrivingNow() { return ScrubDrives() || g.editDriveFrames > 0 || g.releaseDrive; }

void DriveCameraThisFrame() {
	const bool editDrive = g.editDriveFrames > 0 || g.releaseDrive;
	if (g.editDriveFrames > 0) --g.editDriveFrames;
	int target = 0;
	if (g.releaseDrive && !DemoBar_SeekTarget(target)) g.releaseDrive = false; // this frame still flies it
	if (!g.enabled || !g.drive || g.model.TotalKeys() == 0) return;
	if (!ScrubDrives() && !editDrive) return;
	if (CameraPath_OwnsView() || FollowCamera_OwnsView()) return; // the path's own playback, or the follow camera, has the view
	// Only inside the path's ticks: outside them the path would pin the view to its end key.
	const Filmmaker::CamMarkers& markers = CameraPath_Markers();
	const double tick = TimelinePlayhead_Tick();
	if (markers.Count() >= 2 && (tick < markers.Front().tick || tick > markers.Back().tick)) return;
	double origin[3], angles[3], fov;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	const double defaults[7] = { origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov };
	double pose[7];
	// One camera, one curve: the path's pose, which is what plays and renders. The model only when the
	// path cannot evaluate (under two markers).
	const bool onPath = CameraPath_EvalPoseAtTick(tick, pose);
	if (!onPath) g.model.SamplePose(tick, defaults, pose);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned n = 0;
		if ((n++ % 30) == 0)
			Filmmaker::MvmDebugLog_Linef("graph.drive", "t=%.2f scrub=%d onPath=%d out=%.1f,%.1f,%.1f/%.1f,%.1f,%.1f/%.1f", tick,
				TimelinePlayhead_Scrubbing() ? 1 : 0, onPath ? 1 : 0, pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
	}
}

void Edit(void (*op)()) {
	g.model.BeginEdit();
	g.pathUndoPending = true;
	op();
}

} // namespace

void TimelineDock_SetEnabled(bool on) { g.enabled = on; }
bool TimelineDock_Enabled() { return g.enabled; }

const GraphExpModel& TimelineDock_Model() { return g.model; }

void TimelineDock_Frame() {
	const DemoState d = Demo_State();
	const std::string demo = d.playing ? DemoIndex_Get().file : std::string();
	if (!d.playing || (!demo.empty() && demo != g.demo)) {
		// The demo went (or another came): the next one seeds fresh rather than reconciling this
		// shot's handles onto a different one.
		if (!d.playing && g.wasEnabled) {
			OnExit();
			g.wasEnabled = false;
		}
		if (g.model.TotalKeys() > 0) g.model.Clear();
		g.viewInit = false;
		++g.viewEpoch;
		g.pathSeenValid = false;
		TimelinePlayhead_Reset();
		if (!demo.empty()) g.demo = demo;
		if (!d.playing) return;
	}
	if (g.enabled && !g.wasEnabled) {
		OnEnter();
		g.wasEnabled = true;
	} else if (!g.enabled && g.wasEnabled) {
		OnExit();
		g.wasEnabled = false;
	}
	if (!g.enabled) return;
	AdoptPathChanges("path"); // the path can change while the dock is open: K, the inspector, an undo
	DriveCameraThisFrame();
}

TimelineDockInfo TimelineDock_Info() {
	TimelineDockInfo info;
	const DemoState d = Demo_State();
	info.rev = g.rev;
	info.drive = g.drive;
	info.autoPath = CameraPath_AutoPath();
	info.playhead = TimelinePlayhead_Tick();
	info.scrubbing = TimelinePlayhead_Scrubbing();
	info.playing = d.playing && !d.paused;
	info.pathPlaying = CameraPath_OwnsView();
	info.tickRate = 1.0 / IntervalPerTick();
	info.smoothPath = SmoothPath();
	// The suggested window: every key, padded 4 % + 64, else the playhead +- 1000.
	if (!g.viewInit) {
		if (g.model.TotalKeys() > 0) {
			double lo = g.model.MinTick(), hi = g.model.MaxTick();
			if (hi <= lo) hi = lo + 1.0;
			const double pad = (hi - lo) * 0.04 + 64.0;
			g.viewT0 = (std::max)(0.0, lo - pad);
			g.viewT1 = hi + pad;
		} else {
			g.viewT0 = info.playhead - 1000.0;
			g.viewT1 = info.playhead + 1000.0;
		}
		g.viewInit = true;
	}
	info.viewEpoch = g.viewEpoch;
	info.viewT0 = g.viewT0;
	info.viewT1 = g.viewT1;
	if (d.totalTicks > 0) {
		info.domainT0 = 0;
		info.domainT1 = d.totalTicks;
	} else {
		info.domainT0 = 0;
		info.domainT1 = (std::max)(g.model.MaxTick(), info.playhead) + 2000.0;
	}
	const ViewPose view = View_Rendered();
	for (int i = 0; i < 3; ++i) {
		info.live[i] = view.origin[i];
		info.live[3 + i] = view.angles[i];
	}
	info.live[6] = view.fov;
	return info;
}

void TimelineDock_EditBegin() {
	g.model.BeginEdit();
	g.pathUndoPending = true;
}

void TimelineDock_Select(int ch, int id, bool add) { g.model.Select(ch, id, add); BumpRev(); }
void TimelineDock_SelectAdd(int ch, int id) { g.model.SelectAdd(ch, id); BumpRev(); }
void TimelineDock_SelectAll() { g.model.SelectAll(); BumpRev(); }

void TimelineDock_SelectSet(const std::vector<std::pair<int, int>>& keys) {
	g.model.ClearSelection();
	for (const auto& k : keys) g.model.SelectAdd(k.first, k.second);
	BumpRev();
}
void TimelineDock_SelectClear() { g.model.ClearSelection(); BumpRev(); }

void TimelineDock_MoveSelectedBy(double dTick, double dValue) {
	g.model.MoveSelectedBy(dTick, dValue);
	BumpRev();
	SyncToCameraPath();
}

int TimelineDock_AddKey(int ch, double tick, double value) {
	const int id = g.model.AddKey(ch, tick, value);
	g.model.Select(ch, id, false);
	BumpRev();
	SyncToCameraPath();
	return id;
}

void TimelineDock_DeleteSelected() {
	g.model.BeginEdit();
	g.pathUndoPending = true;
	std::vector<std::pair<int, int>> doomed;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const auto& k : g.model.GetChannel(c).keys)
			if (g.model.IsSelected(c, k.id)) doomed.emplace_back(c, k.id);
	for (const auto& k : doomed) g.model.DeleteKey(k.first, k.second);
	g.model.ClearSelection();
	BumpRev();
	SyncToCameraPath();
}

void TimelineDock_SetHandle(int ch, int id, int side, double tx, double dv, bool reflect) {
	g.model.SetHandle(ch, id, side, tx, dv, reflect);
	BumpRev();
	SyncToCameraPath();
}

void TimelineDock_Ease(int mode, bool selectedOnly) {
	TimelineDock_EditBegin();
	g.model.SetEase(mode, selectedOnly);
	BumpRev();
	SyncToCameraPath();
}

void TimelineDock_SetInterp(bool smooth, bool selectedOnly) {
	TimelineDock_EditBegin();
	g.model.SetInterpAll(smooth, selectedOnly);
	if (!selectedOnly) CameraPath_SetInterp(smooth ? 1 : 0); // the whole path's mode too
	BumpRev();
	SyncToCameraPath();
}

void TimelineDock_MirrorPathInterp(bool smooth) {
	if (g.model.TotalKeys() == 0) return;
	g.model.SetInterpAll(smooth, false);
	BumpRev();
}

void TimelineDock_AlignToTick(double tick) {
	if (g.model.SelectionCount() == 0) return;
	TimelineDock_EditBegin();
	g.model.AlignSelectionToTick(tick);
	BumpRev();
	SyncToCameraPath();
}

void TimelineDock_AlignToKey(int ch, int id) {
	if (const GraphExpModel::Keyframe* k = g.model.FindKeyConst(ch, id)) TimelineDock_AlignToTick(k->tick);
}

void TimelineDock_Undo() { g.model.Undo(); BumpRev(); SyncToCameraPath(); }
void TimelineDock_Redo() { g.model.Redo(); BumpRev(); SyncToCameraPath(); }

void TimelineDock_Reseed() {
	const Filmmaker::CamMarkers& markers = CameraPath_Markers();
	g.model.SeedFromMarkers(markers.All());
	if (SmoothPath() && !GraphExpModel::MarkersHaveCurves(markers.All())) g.model.SetInterpAll(true, false);
	g.pathSeen = markers.All();
	g.pathSeenValid = true;
	g.viewInit = false;
	++g.viewEpoch;
	BumpRev();
}

void TimelineDock_SetDrive(bool on) {
	g.drive = on;
	BumpRev();
}

void TimelineDock_Playhead(double tick, bool release) {
	if (release) {
		const bool wasScrubbing = TimelinePlayhead_Scrubbing();
		DemoBar_SeekTo((int)std::floor(tick + 0.5)); // the world goes there first, then the hold
		TimelinePlayhead_EndScrub();
		if (wasScrubbing && CameraPath_AutoPath()) g.releaseDrive = true;
	} else {
		TimelinePlayhead_BeginScrub(tick);
	}
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned n = 0;
		if (release || (n++ % 8) == 0)
			Filmmaker::MvmDebugLog_Linef("graph.playhead", "t=%.2f release=%d scrubbing=%d playhead=%.0f", tick, release ? 1 : 0,
				TimelinePlayhead_Scrubbing() ? 1 : 0, TimelinePlayhead_Tick());
	}
}

bool TimelineDock_OwnsView() {
	return g.enabled && g.drive && g.model.TotalKeys() > 0 && CameraBridge_GetFreeCamEnabled() && DrivingNow();
}

void TimelineDock_PrintHealth() {
	advancedfx::Message("  timeline:   dock %s, keys %s, selected %d, drive %s, AUTO %s, playhead %.0f%s, undo %d\n",
		g.enabled ? "open" : "closed", KeyCounts().c_str(), g.model.SelectionCount(), g.drive ? "on" : "off",
		CameraPath_AutoPath() ? "on" : "off", TimelinePlayhead_Tick(), TimelinePlayhead_Scrubbing() ? " (scrubbing)" : "",
		g.model.CanUndo() ? 1 : 0);
}

void TimelineDock_Command(int argc, advancedfx::ICommandArgs* args) {
	const char* verb = argc >= 3 ? args->ArgV(2) : "state";
	auto arg = [&](int i) { return argc > i ? args->ArgV(i) : ""; };
	auto onOff = [&](const char* s, bool current) {
		if (0 == _stricmp(s, "on") || 0 == _stricmp(s, "1")) return true;
		if (0 == _stricmp(s, "off") || 0 == _stricmp(s, "0")) return false;
		return !current;
	};
	if (0 == _stricmp(verb, "on") || 0 == _stricmp(verb, "off") || 0 == _stricmp(verb, "toggle")) {
		const bool on = onOff(verb, g.enabled);
		Ui_ShowTimelineDock(on);
		g.enabled = on && Ui_WorkspaceOpen(); // the UI frame keeps it from here
	} else if (0 == _stricmp(verb, "drive")) {
		TimelineDock_SetDrive(onOff(arg(3), g.drive));
	} else if (0 == _stricmp(verb, "autopath")) {
		CameraPath_SetAutoPath(onOff(arg(3), CameraPath_AutoPath()));
	} else if (0 == _stricmp(verb, "select") && argc >= 5) {
		TimelineDock_Select(atoi(arg(3)), atoi(arg(4)), 0 == _stricmp(arg(5), "add"));
	} else if (0 == _stricmp(verb, "selset")) {
		std::vector<std::pair<int, int>> keys;
		for (int i = 3; i + 1 < argc; i += 2) keys.emplace_back(atoi(arg(i)), atoi(arg(i + 1)));
		TimelineDock_SelectSet(keys);
	} else if (0 == _stricmp(verb, "selall")) {
		TimelineDock_SelectAll();
	} else if (0 == _stricmp(verb, "selclear")) {
		TimelineDock_SelectClear();
	} else if (0 == _stricmp(verb, "editbegin")) {
		TimelineDock_EditBegin();
	} else if (0 == _stricmp(verb, "movesel") && argc >= 5) {
		TimelineDock_MoveSelectedBy(atof(arg(3)), atof(arg(4)));
	} else if (0 == _stricmp(verb, "addkey") && argc >= 6) {
		TimelineDock_AddKey(atoi(arg(3)), atof(arg(4)), atof(arg(5)));
	} else if (0 == _stricmp(verb, "delsel")) {
		TimelineDock_DeleteSelected();
	} else if (0 == _stricmp(verb, "handle") && argc >= 8) {
		TimelineDock_SetHandle(atoi(arg(3)), atoi(arg(4)), 0 == _stricmp(arg(5), "left") ? -1 : 1, atof(arg(6)), atof(arg(7)),
			argc >= 9 ? atoi(arg(8)) != 0 : true);
	} else if (0 == _stricmp(verb, "ease") && argc >= 4) {
		const int mode = 0 == _stricmp(arg(3), "in") ? 0 : (0 == _stricmp(arg(3), "out") ? 1 : 2);
		TimelineDock_Ease(mode, 0 != _stricmp(arg(4), "all"));
	} else if (0 == _stricmp(verb, "smooth") || 0 == _stricmp(verb, "linear")) {
		TimelineDock_SetInterp(0 == _stricmp(verb, "smooth"), 0 == _stricmp(arg(3), "sel"));
	} else if (0 == _stricmp(verb, "align") && argc >= 4) {
		if (0 == _stricmp(arg(3), "playhead")) TimelineDock_AlignToTick(std::floor(TimelinePlayhead_Tick() + 0.5));
		else if (argc >= 5) TimelineDock_AlignToKey(atoi(arg(3)), atoi(arg(4)));
	} else if (0 == _stricmp(verb, "undo")) {
		TimelineDock_Undo();
	} else if (0 == _stricmp(verb, "redo")) {
		TimelineDock_Redo();
	} else if (0 == _stricmp(verb, "reseed")) {
		TimelineDock_Reseed();
	} else if (0 == _stricmp(verb, "playhead") && argc >= 4) {
		if (0 == _stricmp(arg(3), "release")) TimelineDock_Playhead(TimelinePlayhead_Tick(), true);
		else TimelineDock_Playhead(atof(arg(3)), false);
	} else if (0 != _stricmp(verb, "state")) {
		advancedfx::Message(
			"mirv_filmmaker grapheditor on|off|toggle | drive on|off|toggle | autopath on|off|toggle | state\n"
			"mirv_filmmaker grapheditor select <ch> <id> [add] | selset <ch> <id> ... | selall | selclear\n"
			"mirv_filmmaker grapheditor editbegin | movesel <dTick> <dValue> | addkey <ch> <tick> <value> | delsel\n"
			"mirv_filmmaker grapheditor handle <ch> <id> left|right <tx> <dv> [reflect] | ease in|out|inout [all]\n"
			"mirv_filmmaker grapheditor smooth|linear [sel] | align <ch> <id>|playhead | undo | redo | reseed\n"
			"mirv_filmmaker grapheditor playhead <tick>|release   (channels: 0 x, 1 y, 2 z, 3 pitch, 4 yaw, 5 roll, 6 FOV)\n");
		return;
	}
	TimelineDock_PrintHealth();
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c) {
		const auto& keys = g.model.GetChannel(c).keys;
		if (keys.empty()) continue;
		std::string line;
		for (const auto& k : keys) {
			char buf[96];
			std::snprintf(buf, sizeof(buf), " %d@%.1f=%.2f%s", k.id, k.tick, k.value, g.model.IsSelected(c, k.id) ? "*" : "");
			line += buf;
		}
		advancedfx::Message("    ch%d:%s\n", c, line.c_str());
	}
}

} // namespace SourceFilmmaker

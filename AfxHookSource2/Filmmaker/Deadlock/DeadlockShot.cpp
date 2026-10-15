#include "DeadlockShot.h"

#include "../Movie/CamMarkers.h"
#include "../Movie/CamPathEval.h"
#include "DeadlockDemoClockMath.h"
#include "DeadlockShotRulesMath.h"
#include "DeadlockShotSaveMath.h"
#include "DeadlockReplaySessionMath.h"
#include "DeadlockGraph.h"
#include "../Movie/CameraBridge.h"
#include "../../Deadlock/DeadlockCameraRuntime.h"
#include "../Debug/MvmDebugLog.h"
#include "../Demo/DemoProjectStore.h"
#include "../Platform/TextEncoding.h"
#include "../../Deadlock/DeadlockEngine.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/CamPath.h"
#include "../../../Shared/MirvCampath.h"
#include "../../Deadlock/DeadlockCameraCommandMath.h"
#include "../../Deadlock/DeadlockDemo.h"

#include <cstdio>
#include <string>
#include <windows.h>

// The shared curve store CamPathEval rebuilds into. CS2 defines this in main.cpp; this target
// defines it here, so the natural cubic position spline and the quaternion rotation curve in
// Shared/CamPath + Shared/AfxMath are reused rather than reimplemented.
CamPath g_CamPath;

namespace Filmmaker {

namespace {

CamMarkers g_markers;
PathSettings g_settings;

// A time-keyed view for the HLAE parser. CamPathEval's g_CamPath is keyed by chord
// length and is rebuilt every edit, so it must never receive console mutations.
CamPath g_commandPath;
unsigned long long g_markerRevision = 1, g_commandRevision = 0;
bool g_commandChanged = false;

// VIEW options, not shot data: they change nothing that is written to the sidecar, so they sit
// beside the shot rather than inside PathSettings, whose fields are all persisted.
// On by default: CS2 draws its path gizmo whenever a path exists, with no switch in the UI
// (CameraPath::EnsureDrawState). The console flags remain as an override.
bool g_drawPath = true;
bool g_drawMarkers = true;
bool g_freeze = false;
CamPathEval g_eval;
bool g_evalDirty = true;

bool g_scrubbing = false;
double g_scrubTick = 0.0;

bool g_playing = false;
double g_playStartTick = 0.0;

// The playback clock. Same shared unit the view hook uses, so a measured tolerance lives in one
// place.
DemoClock g_clock;
double g_playTime = 0.0;
double g_currentPathTick = 0.0;

std::wstring PlayingDemoPathWide();
DeadlockShotSaves g_saves;
DeadlockReplayIdentity g_replay;

// Cancel a camera gesture without its normal release side effects: deleted/replaced keys
// have no valid release tick, so cancellation must not seek or push their old pose.
void CancelShotMotion(const char* reason) {
	if ((g_scrubbing || g_playing) && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.shot.cancel", "reason=%s scrubbing=%d playing=%d",
			reason, g_scrubbing, g_playing);
	g_scrubbing = false;
	g_playing = false;
	g_scrubTick = g_playStartTick = g_playTime = g_currentPathTick = 0.0;
	g_clock.Reset();
}

void MarkDirty() {
	++g_markerRevision;
	g_evalDirty = true;
	if (!Deadlock::ShotRules::CanScrub(g_markers.Count())) CancelShotMotion("too few markers");
	// Capture data AND identity while they still describe this edit. A failed save
	// must not serialize whichever replay happens to be on screen at retry time.
	const std::wstring demo = PlayingDemoPathWide();
	if (!demo.empty()) g_saves.Stage(demo, g_markers.Serialize(g_settings, g_markers.Selected()));
}

bool WriteShotSnapshot(const std::wstring& demo, const std::string& payload) {
	return DemoProjectStore_WriteSection(ProjectGame::Deadlock, demo, ProjectSection::Campath, payload);
}

void AutoSave() {
	const auto result = g_saves.FlushOne(GetTickCount64(), WriteShotSnapshot);
	if (result.attempted() && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.shot.save", "autosave %s demo=%ls bytes=%zu pending=%zu retryMs=%d",
			result.succeeded ? "ok" : "FAILED", result.demo.c_str(), result.bytes,
			g_saves.Count(), result.succeeded ? 0 : 5000);
}

void EnsureEval() {
	if (!g_evalDirty) return;
	g_eval.Rebuild(g_markers, g_settings);
	g_evalDirty = false;
}

// One step of the playback clock. A real seek stops playback rather than teleporting the shot: the
// user just moved the world out from under the camera move, and carrying on from the old cursor
// would look like the path had broken.
double FrameDelta(bool& outSeeked) {
	const DemoClockStep step = g_clock.Sample(Deadlock::Engine_DemoTime());
	outSeeked = step.shouldReset();
	return step.delta;
}

std::wstring PlayingDemoPathWide() {
	const std::string demo = Deadlock::Engine_DemoFilePath();
	if (demo.empty()) return {};
	return Utf8ToWide(demo);
}

} // namespace

namespace {
void CommandPathChanged(void*) { g_commandChanged = true; }

void SyncCommandPath() {
	if (g_commandRevision == g_markerRevision) return;
	const double offset = g_commandPath.GetOffset();
	g_commandPath.SelectNone();
	g_commandPath.Clear();
	g_commandPath.SetOffset(offset);
	for (int i = 0; i < g_markers.Count(); ++i) {
		const auto& m = g_markers.At(i);
		CamPathValue value(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
		value.Selected = i == g_markers.Selected();
		g_commandPath.Add(m.time - offset, value);
	}
	g_commandRevision = g_markerRevision;
}
}

void Shot_CampathCommand(advancedfx::ICommandArgs* args, IMirvCampath_Time* time, IMirvCampath_Camera* camera) {
	if (args->ArgC() > 1 && !_stricmp(args->ArgV(1), "add")
		&& (Deadlock::Demo_Tick() < 0 || Deadlock::Demo_IsSeeking())) {
		advancedfx::Warning("mirv_campath: load a replay and wait for any seek to finish before adding a keyframe.\n");
		return;
	}
	// ONE ADD AND ONE CLEAR. The plain forms are the editor's own verbs, so `mirv_campath add` and
	// `mirv_filmmaker marker add` (K) can never guard, snapshot or persist differently.
	if (args->ArgC() == 2 && !_stricmp(args->ArgV(1), "add")) {
		Shot_AddMarkerHere();
		return;
	}
	if (args->ArgC() == 2 && !_stricmp(args->ArgV(1), "clear")) {
		Shot_Clear();
		return;
	}
	if (args->ArgC() > 1 && !_stricmp(args->ArgV(1), "draw")) {
		if (args->ArgC() == 4 && !_stricmp(args->ArgV(2), "enabled"))
			Shot_SetDrawPath(atoi(args->ArgV(3)) != 0);
		else advancedfx::Message("mirv_campath draw enabled 0|1 - editor path drawing. Axis/frustum gizmos are not available in Deadlock.\n");
		return;
	}
	SyncCommandPath();
	const double previousOffset = g_commandPath.GetOffset();
	g_commandChanged = false;
	g_commandPath.OnChangedAdd(CommandPathChanged, nullptr);
	MirvCampath_ConCommand(args, advancedfx::Message, advancedfx::Warning, &g_commandPath, time, camera, nullptr);
	g_commandPath.OnChangedRemove(CommandPathChanged, nullptr);
	if (!g_commandChanged) return;
	if (!std::isfinite(g_commandPath.GetOffset())) {
		advancedfx::Warning("mirv_campath: offset must be finite.\n");
		g_commandPath.SetOffset(previousOffset);
		g_commandRevision = 0;
		SyncCommandPath();
		return;
	}
	std::vector<CamMarker> markers;
	int selected = -1;
	for (auto it = g_commandPath.GetBegin(); it != g_commandPath.GetEnd(); ++it) {
		const auto value = it.GetValue();
		const auto angles = value.R.ToQREulerAngles().ToQEulerAngles();
		CamMarker m;
		m.time = it.GetTime() + g_commandPath.GetOffset();
		// Retain editor easing/speed for unchanged keys; new keys use editor defaults.
		for (const auto& old : g_markers.All()) if (std::fabs(old.time - m.time) < 1e-6) { m = old; break; }
		m.time = it.GetTime() + g_commandPath.GetOffset();
		m.x = value.X; m.y = value.Y; m.z = value.Z;
		m.pitch = angles.Pitch; m.yaw = angles.Yaw; m.roll = angles.Roll; m.fov = value.Fov;
		if (!std::isfinite(m.x) || !std::isfinite(m.y) || !std::isfinite(m.z)
			|| !std::isfinite(m.pitch) || !std::isfinite(m.yaw) || !std::isfinite(m.roll)
			|| !Deadlock::ValidCameraFov(m.fov)
			|| !Deadlock::CameraTimeToTick(m.time, Deadlock::Demo_Time(), Deadlock::Demo_Tick(),
				Deadlock::Demo_IntervalPerTick(), m.tick)) {
			advancedfx::Warning("mirv_campath: edit rejected; keys must have valid replay times and finite camera values.\n");
			g_commandPath.SetOffset(previousOffset);
			g_commandRevision = 0;
			SyncCommandPath();
			return;
		}
		if (value.Selected && selected < 0) selected = (int)markers.size();
		markers.push_back(m);
	}
	g_markers.Restore(markers, selected);
	MarkDirty();
	g_commandRevision = g_markerRevision;
	if (MvmDebugLog_Active()) MvmDebugLog_LinefAlways("deadlock.campath", "editor markers=%d enabled=%d", g_markers.Count(), g_commandPath.Enabled_get());
}

bool Shot_CommandPathActive() {
	if (Deadlock::Demo_Tick() < 0 || g_playing || g_scrubbing || !g_commandPath.Enabled_get()) return false;
	SyncCommandPath();
	if (!g_commandPath.CanEval()) return false;
	const double t = Deadlock::Demo_Time() - g_commandPath.GetOffset();
	return g_commandPath.GetHold() || (t >= g_commandPath.GetLowerBound() && t <= g_commandPath.GetUpperBound());
}

bool Shot_ApplyCommandPath(float& x, float& y, float& z, float& pitch, float& yaw, float& roll, float& fov) {
	if (!Shot_CommandPathActive()) return false;
	double t = Deadlock::Demo_Time() - g_commandPath.GetOffset();
	if (t < g_commandPath.GetLowerBound()) t = g_commandPath.GetLowerBound();
	if (t > g_commandPath.GetUpperBound()) t = g_commandPath.GetUpperBound();
	const auto value = g_commandPath.Eval(t);
	const auto angles = value.R.ToQREulerAngles().ToQEulerAngles();
	x = (float)value.X; y = (float)value.Y; z = (float)value.Z;
	pitch = (float)angles.Pitch; yaw = (float)angles.Yaw; roll = (float)angles.Roll; fov = (float)value.Fov;
	return true;
}

int Shot_AddMarkerHere() {
	double origin[3], angles[3], fov;
	CameraBridge_GetCurrentCamera(origin, angles, fov);

	CamMarker m;
	m.x = origin[0];
	m.y = origin[1];
	m.z = origin[2];
	m.pitch = angles[0];
	m.yaw = angles[1];
	m.roll = angles[2];
	m.fov = fov;
	m.tick = Deadlock::Engine_DemoTick();
	m.time = Deadlock::Engine_DemoTime();

	const int index = g_markers.Add(m);
	MarkDirty();

	advancedfx::Message("AfxHookDeadlock: marker %d at tick %d (%.0f %.0f %.0f)\n",
	                    index, m.tick, m.x, m.y, m.z);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.shot",
			"marker add index=%d tick=%d time=%.3f pos=(%.2f %.2f %.2f) "
			"ang=(%.2f %.2f %.2f) fov=%.2f",
			index, m.tick, m.time, m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
	return index;
}

bool Shot_UpdateSelectedFromCamera() {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;

	double origin[3], angles[3], fov;
	CameraBridge_GetCurrentCamera(origin, angles, fov);

	// The POSE only. The marker keeps its tick, because "put this camera here" and "move this
	// camera to a different moment" are different edits and conflating them means nudging a framing
	// silently re-times the shot.
	CamMarker& m = g_markers.At(sel);
	m.x = origin[0]; m.y = origin[1]; m.z = origin[2];
	m.pitch = angles[0]; m.yaw = angles[1]; m.roll = angles[2];
	m.fov = fov;
	MarkDirty();

	advancedfx::Message("AfxHookDeadlock: marker %d updated to (%.0f %.0f %.0f) at tick %d\n",
	                    sel, m.x, m.y, m.z, m.tick);
	return true;
}

bool Shot_DeleteSelected() {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;
	const bool ok = g_markers.DeleteIndex(sel);
	if (ok) MarkDirty();
	return ok;
}

void Shot_Clear() {
	g_markers.DeleteAll();
	MarkDirty();
}

int Shot_MarkerCount() { return g_markers.Count(); }
int Shot_Selected() { return g_markers.Selected(); }

void Shot_Select(int index) {
	const int before = g_markers.Selected();
	g_markers.SetSelected(index);
	if (before != g_markers.Selected()) MarkDirty();
}

// --- the selected marker's own values ---------------------------------------------------------

bool Shot_SetSelectedFov(double fov) {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;
	if (fov < 20.0) fov = 20.0; else if (fov > 130.0) fov = 130.0;
	g_markers.At(sel).fov = fov;
	MarkDirty();
	return true;
}

bool Shot_SetSelectedRoll(double roll) {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;
	if (roll < -45.0) roll = -45.0; else if (roll > 45.0) roll = 45.0;
	g_markers.At(sel).roll = roll;
	MarkDirty();
	return true;
}

bool Shot_SetSelectedEase(int ease) {
	return Shot_SetMarkerEase(g_markers.Selected(), ease);
}

bool Shot_SetSelectedSpeedMul(double mul) {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;
	if (mul < 0.2) mul = 0.2; else if (mul > 1.0) mul = 1.0;
	g_markers.At(sel).speedMul = (float)mul;
	MarkDirty();
	return true;
}

bool Shot_SetMarkerEase(int sel, int ease) {
	if (!g_markers.ValidIndex(sel)) return false;
	if (ease < 0) ease = 0; else if (ease > 3) ease = 3;
	g_markers.At(sel).ease = (Ease)ease;
	MarkDirty();
	return true;
}

int Shot_SelectStep(int delta) {
	const int n = g_markers.Count();
	if (n <= 0) return -1;
	int sel = g_markers.Selected();
	if (sel < 0) sel = 0;
	// Wrapping, so stepping past the end lands on the first marker rather than sticking. The
	// stepper is the only way to reach a marker with the timeline closed, and one that stops at
	// the end reads as broken.
	int next = ((sel + delta) % n + n) % n;
	Shot_SelectForEditor(next);
	return next;
}

bool Shot_SelectForEditor(int index) {
	if (!g_markers.ValidIndex(index)) return false;
	Shot_Select(index);
	const CamMarker& m = g_markers.At(index);
	// CS2 (CameraPath::SelectForEditor): the editor always navigates to the selected camera. Pause
	// first so the seek lands on a still frame, then the one-shot free-cam placement -- the free
	// camera has to be ON for the pushed pose to be applied rather than left pending.
	if (!Deadlock::Engine_IsDemoPaused())
		Deadlock::Engine_ExecuteClientCmd("demo_pause");
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", m.tick);
	Deadlock::Engine_ExecuteClientCmd(cmd);
	Deadlock::CameraRuntime_SetFreeCam(true, "marker select");
	CameraBridge_SetCameraPose(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("deadlock.shot.select", "editor select #%d tick=%d pose=(%.1f %.1f %.1f)",
			index, m.tick, m.x, m.y, m.z);
	return true;
}

bool Shot_RetimeSelectedToCurrentTick() {
	const int sel = g_markers.Selected();
	if (!g_markers.ValidIndex(sel)) return false;
	const int now = Deadlock::Demo_Tick();
	if (now < 0) return false;
	const bool hasPrev = sel > 0, hasNext = sel + 1 < g_markers.Count();
	const Deadlock::ShotRules::RetimeClamp c = Deadlock::ShotRules::ClampRetimeTick(
		hasPrev, hasPrev ? g_markers.At(sel - 1).tick : 0,
		hasNext, hasNext ? g_markers.At(sel + 1).tick : 0,
		g_markers.At(sel).tick, now);
	if (!c.changed) {
		advancedfx::Message("marker: tick -> here: marker %d already at tick %d.\n", sel, c.tick);
		return true;
	}
	CamMarker& m = g_markers.At(sel);
	m.tick = c.tick;
	const float interval = (float)Deadlock::Demo_IntervalPerTick();
	if (Deadlock::ShotRules::RetimeAlsoUpdatesTime(interval))
		m.time = Deadlock::ShotRules::RetimedMarkerTime(c.tick, interval);
	MarkDirty();
	advancedfx::Message("marker: tick -> here: marker %d now at tick %d%s.\n", sel, c.tick,
		c.tick != now ? " (clamped between its neighbours)" : "");
	return true;
}

int g_hoveredMarker = -1;
void Shot_SetHoveredMarker(int index) { g_hoveredMarker = g_markers.ValidIndex(index) ? index : -1; }
int Shot_HoveredMarker() { return g_markers.ValidIndex(g_hoveredMarker) ? g_hoveredMarker : -1; }

int Shot_SamplePath(double* outXyz, int maxPoints) {
	if (!outXyz || maxPoints < 2 || g_markers.Count() < 2) return 0;
	EnsureEval();
	if (!g_eval.CanEval()) return 0;
	const int first = g_markers.Front().tick, last = g_markers.Back().tick;
	if (last <= first) return 0;
	// Enough samples that a smooth curve reads as a curve under an 8px stroke, capped by the buffer.
	int points = (g_markers.Count() - 1) * 32 + 1;
	if (points > maxPoints) points = maxPoints;
	int written = 0;
	for (int i = 0; i < points; ++i) {
		const double tick = first + (double)(last - first) * (double)i / (double)(points - 1);
		const CamPathEval::Pose pose = g_eval.EvalAtTick(g_markers, tick);
		if (!pose.valid) continue;
		outXyz[written * 3 + 0] = pose.x;
		outXyz[written * 3 + 1] = pose.y;
		outXyz[written * 3 + 2] = pose.z;
		++written;
	}
	return written;
}

// --- path-wide settings -----------------------------------------------------------------------

void Shot_SetSpeedMode(int mode) {
	if (mode < 0) mode = 0; else if (mode > 2) mode = 2;
	g_settings.speedMode = mode;
	MarkDirty();
}
int Shot_SpeedMode() { return g_settings.speedMode; }

void Shot_SetConstSpeed(double mul) {
	if (mul < 0.2) mul = 0.2; else if (mul > 1.0) mul = 1.0;
	g_settings.constSpeed = (float)mul;
	MarkDirty();
}
// Same ladder as CS2's CameraPath::CycleConstSpeed: 1.0 -> 0.8 -> 0.5 -> 0.2 -> 1.0.
void Shot_CycleConstSpeed() {
	double next;
	if (g_settings.constSpeed > 0.9f) next = 0.8;
	else if (g_settings.constSpeed > 0.65f) next = 0.5;
	else if (g_settings.constSpeed > 0.35f) next = 0.2;
	else next = 1.0;
	Shot_SetConstSpeed(next);
}
double Shot_ConstSpeed() { return g_settings.constSpeed; }

void Shot_SetDrawPath(bool on) { g_drawPath = on; }
bool Shot_DrawPath() { return g_drawPath; }
void Shot_SetDrawMarkers(bool on) { g_drawMarkers = on; }
bool Shot_DrawMarkers() { return g_drawMarkers; }
void Shot_SetFreeze(bool on) { g_freeze = on; }
bool Shot_Freeze() { return g_freeze; }

void Shot_AppendCameraStateJson(std::string& out) {
	const int sel = g_markers.Selected();
	const bool valid = g_markers.ValidIndex(sel);
	// With nothing selected the panel still needs numbers for its tiles. The defaults are the
	// engine's, not zeroes: a FOV tile reading 0 looks like a broken feed rather than an empty shot.
	const double fov = valid ? g_markers.At(sel).fov : 90.0;
	const double roll = valid ? g_markers.At(sel).roll : 0.0;
	const int ease = valid ? (int)g_markers.At(sel).ease : 0;
	// The last marker has no outgoing segment, so its speedMul is meaningless -- the Amount
	// stepper falls back to 1.0 exactly like CS2's stepMul() does for st.sel == null.
	const bool hasOutgoing = valid && sel < g_markers.Count() - 1;
	const double speedMul = hasOutgoing ? g_markers.At(sel).speedMul : 1.0;

	char buf[320];
	std::snprintf(buf, sizeof(buf),
		",\"cam\":{\"fov\":%.2f,\"roll\":%.2f,\"ease\":%d,\"speedMode\":%d"
		",\"constSpeed\":%.2f,\"speedMul\":%.2f"
		",\"freeze\":%s,\"drawPath\":%s,\"drawMarkers\":%s}",
		fov, roll, ease, g_settings.speedMode,
		g_settings.constSpeed, speedMul,
		g_freeze ? "true" : "false",
		g_drawPath ? "true" : "false",
		g_drawMarkers ? "true" : "false");
	out += buf;
}

const CamMarker* Shot_Marker(int index) {
	if (!g_markers.ValidIndex(index)) return nullptr;
	return &g_markers.At(index);
}

int Shot_FirstTick() {
	if (g_markers.Empty()) return 0;
	return g_markers.Front().tick;
}

int Shot_LastTick() {
	if (g_markers.Empty()) return 0;
	return g_markers.Back().tick;
}

void Shot_BeginScrub(double tick) {
	if (!std::isfinite(tick)) {
		advancedfx::Warning("AfxHookDeadlock: scrub tick must be finite.\n");
		return;
	}
	// Revalidate on EVERY drag update. Clear/delete/load can change the path while a
	// gesture is active; a latched scrub flag is not permission to read Front/Back.
	if (!Deadlock::ShotRules::CanScrub(g_markers.Count())) {
		CancelShotMotion("too few markers");
		advancedfx::Message("AfxHookDeadlock: need at least 2 markers to scrub the path.\n");
		return;
	}
	EnsureEval();
	if (!g_eval.CanEval()) {
		CancelShotMotion("path not evaluable");
		return;
	}
	// Pause the world and enable freecam only when entering a valid scrub.
	if (!g_scrubbing) {
		if (!Deadlock::Engine_IsDemoPaused()) Deadlock::Engine_ExecuteClientCmd("demo_pause");
		Deadlock::CameraRuntime_SetFreeCam(true, "scrub");
	}
	// CS2's ClampToPathTick: the camera timeline spans the path, so a scrub never leaves it.
	const double first = g_markers.Front().tick, last = g_markers.Back().tick;
	if (tick < first) tick = first; else if (tick > last) tick = last;
	g_scrubbing = true;
	g_scrubTick = tick;
}

void Shot_EndScrub() {
	if (!g_scrubbing) return;
	EnsureEval();
	g_scrubbing = false;

	// Both halves. Ending the scrub without the seek leaves the demo tick stale, and every view
	// that falls back to it then disagrees with the one that did the dragging.
	char cmd[64];
	std::snprintf(cmd, sizeof(cmd), "demo_gototick %d", (int)(g_scrubTick + 0.5));
	Deadlock::Engine_ExecuteClientCmd(cmd);

	// And the camera STAYS on the path, in free cam. The drag drew its poses through the shot's own
	// view slot, which lets go the moment the scrub ends; the release pose goes into the free camera
	// (on since the scrub began) or the view would jump to wherever free cam last was.
	if (g_eval.CanEval()) {
		const CamPathEval::Pose pose = g_eval.EvalAtTick(g_markers, g_scrubTick);
		if (pose.valid)
			CameraBridge_SetCameraPose(pose.x, pose.y, pose.z, pose.pitch, pose.yaw, pose.roll, pose.fov);
	}
}

bool Shot_Scrubbing() { return g_scrubbing; }

double Shot_PlayheadTick() {
	if (g_scrubbing) return g_scrubTick;
	const int tick = Deadlock::Engine_DemoTick();
	return tick < 0 ? 0.0 : (double)tick;
}

void Shot_Play() {
	if (g_markers.Count() < 2) {
		advancedfx::Warning("AfxHookDeadlock: a shot needs at least two markers.\n");
		return;
	}
	EnsureEval();
	if (!g_eval.CanEval()) {
		advancedfx::Warning("AfxHookDeadlock: the path is not evaluable.\n");
		return;
	}
	// FROM WHEREVER THE PLAYHEAD IS. That is the point of a separate playhead: drag the track to
	// the moment you care about, release, press play, and the shot runs from there rather than from
	// its first marker.
	g_playStartTick = Shot_PlayheadTick();
	g_playTime = g_eval.TimingAtTick(g_markers, g_playStartTick);
	g_playing = true;
	g_clock.Reset();

	// AND THE WORLD PLAYS WITH IT. The shot is driven by the DEMO clock -- that is what makes a
	// second run of the same shot land on the same frames -- so a paused demo means a shot that
	// starts, advances by zero every frame and looks broken. Playing the camera move means playing
	// the footage it was cut against.
	if (Deadlock::Engine_IsDemoPaused())
		Deadlock::Engine_ExecuteClientCmd("demo_resume");

	advancedfx::Message("AfxHookDeadlock: playing the shot from tick %.0f.\n", g_playStartTick);
}

void Shot_Pause() {
	if (!g_playing) return;
	g_playing = false;
	// Symmetrical with Play: stopping the camera move stops the footage under it. Leaving the demo
	// running would carry on past the shot with the game's own camera, which reads as the pause
	// having done nothing.
	if (!Deadlock::Engine_IsDemoPaused())
		Deadlock::Engine_ExecuteClientCmd("demo_pause");
}

bool Shot_Playing() { return g_playing; }

void Shot_RunFrame() {
	EnsureEval();
	if (!g_eval.CanEval()) {
		CancelShotMotion("path not evaluable");
		return;
	}

	// While scrubbing, preview the pose at the dragged tick without moving the world. This is the
	// whole reason the playhead is separate from the demo tick.
	if (g_scrubbing && g_eval.CanEval()) {
		CamPathEval::Pose pose = g_eval.EvalAtTick(g_markers, g_scrubTick);
		if (pose.valid) {
			g_currentPathTick = g_scrubTick;
			Deadlock::CameraRuntime_PushOwnedPose(Deadlock::ViewOwner::Shot, pose.x, pose.y, pose.z,
			                                     pose.pitch, pose.yaw, pose.roll, pose.fov);
		}
		return;
	}

	if (!g_playing) return;

	bool seeked = false;
	g_playTime += FrameDelta(seeked);
	if (seeked) {
		// The world moved. Re-anchor the cursor to where the demo now is instead of continuing
		// from a timing value that no longer corresponds to anything on screen.
		g_playTime = g_eval.TimingAtTick(g_markers, Shot_PlayheadTick());
	}
	if (g_playTime > g_eval.Duration()) {
		g_playTime = g_eval.Duration();
		g_playing = false;
	}

	CamPathEval::Pose pose = g_eval.EvalAtTiming(g_markers, g_playTime);
	if (pose.valid) {
		if (pose.seg >= 0 && pose.seg + 1 < g_markers.Count()) {
			const CamMarker& a = g_markers.At(pose.seg);
			const CamMarker& b = g_markers.At(pose.seg + 1);
			g_currentPathTick = a.tick + pose.p * (double)(b.tick - a.tick);
		}
		// Pushed EVERY frame while the shot runs, deliberately: a pose that survived a dropped
		// frame would pin the camera silently.
		Deadlock::CameraRuntime_PushOwnedPose(Deadlock::ViewOwner::Shot, pose.x, pose.y, pose.z,
		                                     pose.pitch, pose.yaw, pose.roll, pose.fov);
	}
}

bool Shot_OwnsView() {
	// OWNING THE VIEW IS A RESULT, NOT A MODE. Playing or scrubbing with fewer than two markers
	// pushes no pose at all, and answering "yes" there suppressed the follow camera for the whole
	// drag -- so scrubbing the timeline with an empty path froze a follow camera that had every
	// right to keep tracking. The question every caller is really asking is "is something already
	// writing the camera this frame", and only an evaluable path is.
	if (!(g_playing || g_scrubbing)) return false;
	EnsureEval();
	return g_eval.CanEval();
}

bool Shot_CurrentPathTick(double& outTick) {
	if (!Shot_OwnsView()) return false;
	outTick = g_scrubbing ? g_scrubTick : g_currentPathTick;
	return true;
}

bool Shot_Save() {
	const std::wstring demo = PlayingDemoPathWide();
	if (demo.empty()) {
		advancedfx::Warning("AfxHookDeadlock: no demo loaded, nowhere to save the shot.\n");
		return false;
	}
	g_saves.Stage(demo, g_markers.Serialize(g_settings, g_markers.Selected()));
	if (!g_saves.SaveNow(demo, GetTickCount64(), WriteShotSnapshot).succeeded) {
		advancedfx::Warning("AfxHookDeadlock: could not write the shot section; retained for retry.\n");
		return false;
	}
	advancedfx::Message("AfxHookDeadlock: shot saved (%d markers).\n", g_markers.Count());
	return true;
}

bool Shot_Load() {
	const std::wstring demo = PlayingDemoPathWide();
	if (demo.empty()) return false;
	std::string payload;
	// Pending edits are newer than disk. Loading another project must not discard
	// them, and returning to this one must not resurrect its older saved contents.
	if (const auto* pending = g_saves.Pending(demo)) payload = *pending;
	else if (!DemoProjectStore_ReadSection(ProjectGame::Deadlock, demo, ProjectSection::Campath, payload))
		return false;
	int selected = -1;
	if (!g_markers.Parse(payload, g_settings, selected)) return false;
	CancelShotMotion("shot loaded");
	g_markers.SetSelected(selected);
	++g_markerRevision;
	g_evalDirty = true;
	advancedfx::Message("AfxHookDeadlock: shot loaded (%d markers).\n", g_markers.Count());
	return true;
}

bool Shot_UpdateReplay(bool demoLoaded) {
	// Also runs with UI disabled; persistence and replay identity are backend state.
	AutoSave();
	if (!g_replay.Observe(demoLoaded, PlayingDemoPathWide())) return false;
	CancelShotMotion("replay changed");
	g_markers = CamMarkers();
	g_settings = PathSettings();
	g_hoveredMarker = -1;
	++g_markerRevision;
	g_evalDirty = true;
	g_commandRevision = 0;
	g_commandPath.Enabled_set(false);
	g_commandPath.SetHold(false);
	g_commandPath.SetOffset(0);
	g_commandPath.SelectNone();
	g_commandPath.Clear();
	g_commandChanged = false;
	const bool loaded = !g_replay.Path().empty() && Shot_Load();
	Graph_SetReplay(g_replay.Path());
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("deadlock.shot.replay", "demo=%ls loaded=%d markers=%d pending=%zu",
			g_replay.Path().c_str(), loaded, g_markers.Count(), g_saves.Count());
	return true;
}

void Shot_Report() {
	EnsureEval();
	advancedfx::Message("  shot project: %ls\n", g_replay.Path().c_str());
	advancedfx::Message("  shot saves: pending=%zu bytes=%zu\n", g_saves.Count(), g_saves.Bytes());
	advancedfx::Message(
		"  shot:      markers=%d selected=%d ticks=%d..%d evaluable=%s duration=%.2fs\n",
		g_markers.Count(), g_markers.Selected(), Shot_FirstTick(), Shot_LastTick(),
		g_eval.CanEval() ? "yes" : "no", g_eval.Duration());
	advancedfx::Message(
		"  playhead:  tick=%.1f scrubbing=%s playing=%s\n",
		Shot_PlayheadTick(), g_scrubbing ? "yes" : "no", g_playing ? "yes" : "no");
}

} // namespace Filmmaker

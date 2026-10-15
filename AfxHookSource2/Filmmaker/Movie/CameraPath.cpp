#include "CameraPath.h"

#include "PlaybackLoop.h"
#include "DemoScrub.h"
#include "../Render/RenderCommand.h"

#include "CameraBridge.h"
#include "TimelinePlayhead.h" // the ONE cursor shared with the graph editor
#include "FollowCamera.h"
#include "../Filmmaker.h"            // PlayingDemoPath()
#include "../Demo/DemoProjectStore.h"
#include "../Panorama/GraphEditorExperimentHud.h"
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_*
#include "../../MirvTime.h"

#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h>
#include <cmath>
#include <sstream>

// Engine pointer (same one MovieMode uses) for demo pause/seek during preview/scrub.
extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {
	bool DemoIsPlaying() {
		if (!g_pEngineToClient) return false;
		if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsPlayingDemo();
		return false;
	}
	bool DemoIsPaused() {
		if (!g_pEngineToClient) return false;
		if (auto pDemo = g_pEngineToClient->GetDemoFile()) return pDemo->IsDemoPaused();
		return false;
	}
	void DemoCmd(const char* c) {
		if (g_pEngineToClient) g_pEngineToClient->ExecuteClientCmd(0, c, true);
	}
}

CameraPath& CameraPathRef() {
	static CameraPath s_instance;
	return s_instance;
}

// --- name / read helpers ---

const char* CameraPath::SpeedModeName() const {
	switch (m_speedMode) {
	case SpeedMode::Constant: return "Constant";
	case SpeedMode::PerSegment: return "Per-Segment";
	default: return "Manual";
	}
}
const char* CameraPath::TimingName() const { return m_timing == Timing::Freeze ? "Freeze" : "Live"; }
// "Bezier" is the natural-cubic mode; the UI label is "Smooth" (no draggable handles).
const char* CameraPath::InterpName() const { return m_interp == Interp::Bezier ? "Smooth" : "Linear"; }

// The cubic needs three markers to bend at all (CamPathEval::Rebuild). A two-marker
// Smooth path still eases its timing, so it is not simply Linear -- the name stays
// Smooth and this flag is what the UI dims to say "no curve until a third key".
bool CameraPath::InterpDegraded() const {
	return m_interp == Interp::Bezier && m_data.Count() < 3;
}
const char* CameraPath::EffectiveInterpName() const {
	return InterpName();
}
const char* CameraPath::ModeName() const {
	switch (GetMode()) {
	case Mode::Reposition: return "Reposition";
	case Mode::PreviewArmed: return "PreviewArmed";
	case Mode::PreviewPlaying: return "PreviewPlaying";
	default: return "Editing";
	}
}

bool CameraPath::PreviewHudHidden() const {
	Mode m = GetMode();
	// Only the user's Tab toggle hides HUD. Playing the path by itself must not hide
	// the HUD, and editor/timeline auto-play must keep the editor visible.
	return m_hudHidden && (m == Mode::PreviewArmed || (m == Mode::PreviewPlaying && !m_editorPlay));
}

int CameraPath::FirstPathTick() const {
	return m_data.Empty() ? 0 : m_data.Front().tick;
}

int CameraPath::LastPathTick() const {
	return m_data.Empty() ? 0 : m_data.Back().tick;
}

int CameraPath::ClampToPathTick(int tick) const {
	if (!HasPathRange())
		return tick < 0 ? 0 : tick;
	const int first = FirstPathTick();
	const int last = LastPathTick();
	if (tick < first) return first;
	if (tick > last) return last;
	return tick;
}

float CameraPath::SelectedSpeedMul() const {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) return 1.0f;
	return m_data.At(sel).speedMul;
}

PathSettings CameraPath::MakeSettings() const {
	PathSettings s;
	s.speedMode = (int)m_speedMode;
	s.timing = (int)m_timing;
	s.interp = (int)m_interp;
	s.constSpeed = m_constSpeed;
	s.autoSnap = m_autoSnap;
	return s;
}

CamPlayback::Context CameraPath::MakeContext() const {
	CamPlayback::Context c;
	c.timing = (int)m_timing;
	c.speedMode = (int)m_speedMode;
	c.interpName = InterpName();
	c.speedModeName = SpeedModeName();
	c.constSpeed = m_constSpeed;
	c.debug = m_debug;
	return c;
}

// True while the dolly is actively driving the final view: PreviewPlaying + playing AND
// free cam engaged. The free-cam requirement keeps range-gated playpath honest -- it
// releases free cam outside the marker range, so OwnsView() is false there (the normal
// demo view is kept); inside the range (free cam on) the dolly owns the view.
bool CameraPath::OwnsView() const {
	return GetMode() == Mode::PreviewPlaying && m_play.Playing()
		&& CameraBridge_GetFreeCamEnabled();
}

// --- internal frame helpers ---

void CameraPath::RebuildCamPath() {
	m_eval.Rebuild(m_data, MakeSettings());
}

void CameraPath::EnsureDrawState() {
	// Show in-world path visuals while editing, but hide them while any live camera owns the
	// shot. Master m_Draw gates the shared drawer, so this removes path gizmos for path playback
	// and for Follow/Attach live previews.
	//
	// The follow-target highlight rides the same pass, so a selected target is a second reason to
	// run it -- asked for here rather than set directly by the follow code, because this function
	// only writes m_Draw on a change and anything writing it behind that cache stays stuck. Both
	// gates below still apply: an editing aid has no business in a playing shot or a live preview.
	const bool wantHighlight = FollowCameraRef().WantsTargetHighlight();
	bool want = (!m_data.Empty() || wantHighlight) && GetMode() != Mode::PreviewPlaying && !FollowCameraRef().OwnsView();
	if (want != m_drawOn) {
		CameraBridge_SetPathDrawEnabled(want);
		m_drawOn = want;
	}
}

void CameraPath::SyncDrawerStyle() {
	CameraBridge_SetMarkerStyle(!m_data.Empty(), m_timing == Timing::Freeze, m_hovered.load());
}

double CameraPath::NowSec() {
	LARGE_INTEGER freq, now;
	QueryPerformanceFrequency(&freq);
	QueryPerformanceCounter(&now);
	return (double)now.QuadPart / (double)freq.QuadPart;
}

void CameraPath::Notify(const char* msg) {
	m_notice = msg ? msg : "";
	m_noticeExpireSec = NowSec() + 3.5;
}

const char* CameraPath::Notice() const { return m_notice.c_str(); }

void CameraPath::PushPose(const CamMarker& m) {
	CameraBridge_SetCameraPose(m.x, m.y, m.z, m.pitch, m.yaw, m.roll, m.fov);
}

void CameraPath::SeekDemoTick(int tick) {
	if (tick < 0) tick = 0;
	DemoScrub_Cancel();
	std::ostringstream oss;
	oss << "demo_gototick " << tick;
	DemoCmd(oss.str().c_str());
}

void CameraPath::HoldEditorAtMarker(int index) {
	if (!m_data.ValidIndex(index))
		return;
	const CamMarker& marker = m_data.At(index);
	m_data.SetSelected(index);
	CameraBridge_SetFreeCamEnabled(true);
	PushPose(marker);
	m_editorPoseHoldFrames = 4;
	if (m_data.Count() >= 2)
		m_play.BeginScrub(marker.tick);
}

// End of the path with LOOP on: rewind to the first marker and play the dolly again.
//
// Deliberately NOT the tick loop. A camera path already knows its own range -- the markers ARE
// the range -- so looping it means replaying the path, not seeking to some separately-set
// region. PlaybackLoop stands down entirely while a path is playing for the same reason: two
// things seeking the same playhead would fight, and the dolly is the one that owns the shot.
void CameraPath::RestartPathPlayback() {
	const int firstTick = FirstPathTick();
	m_play.EndScrub();
	if (m_timing == Timing::Live && firstTick >= 0) {
		SeekDemoTick(firstTick);
		// The seek auto-pauses; resume, and hold the guard open long enough to outlast it (the
		// same reason PlayPath sets it).
		DemoCmd("demo_resume");
		m_liveResumeGuard = 120;
	}
	m_play.StartPlay(m_eval.Duration(), /*rangeGated*/ m_timing == Timing::Live);
	SetMode(Mode::PreviewPlaying);
}

void CameraPath::FinishPlaybackAtLastKey() {
	const int lastIndex = m_data.Count() - 1;
	const int lastTick = LastPathTick();
	if (m_timing == Timing::Live) {
		DemoCmd("demo_pause");
		m_liveResumeGuard = 0;
		SeekDemoTick(lastTick);
	}
	StopPreview();
	HoldEditorAtMarker(lastIndex);
}

// --- editing actions ---

void CameraPath::PlaceMarker() {
	CamMarker m;
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	int tick = 0; if (g_MirvTime.GetCurrentDemoTick(tick)) m.tick = tick;
	double dt = 0.0; if (g_MirvTime.GetCurrentDemoTime(dt)) m.time = dt;
	m.speedMul = 1.0f; m.ease = Ease::None;

	int idx = m_data.Add(m);
	RebuildCamPath();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d placed @ tick %d (%.1f %.1f %.1f) fov %.1f.\n",
		idx, m.tick, m.x, m.y, m.z, m.fov);
}

void CameraPath::DeleteIndex(int index) {
	if (!m_data.ValidIndex(index)) {
		advancedfx::Message("[campath] delete: no marker #%d.\n", index);
		return;
	}
	m_data.DeleteIndex(index);
	if (m_data.Empty()) m_menuOpen = false;
	RebuildCamPath();
	GraphEditorExperimentHudRef().CmdReseed();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d deleted (%d left).\n", index, Count());
}

void CameraPath::DeleteAll(bool confirmed) {
	if (!confirmed) {
		advancedfx::Message("[campath] deleteall: pass 'confirm' to clear all markers.\n");
		return;
	}
	int c = Count();
	m_data.DeleteAll();
	m_menuOpen = false;
	m_play.Stop();
	SetMode(Mode::Editing);
	RebuildCamPath();
	GraphEditorExperimentHudRef().CmdClear();
	EnsureDrawState();
	MarkDirty();
	advancedfx::Message("[campath] all markers deleted (%d).\n", c);
}

void CameraPath::SelectIndex(int index, bool teleport) {
	if (m_data.Empty()) return;
	m_data.SetSelected(index);
	int sel = m_data.Selected();
	// Only move the viewer to the marker when Auto-Snap is on.
	if (teleport && m_autoSnap) {
		CameraBridge_SetFreeCamEnabled(true);
		PushPose(m_data.At(sel));
	}
	advancedfx::Message("[campath] selected marker #%d%s.\n", sel,
		(teleport && !m_autoSnap) ? " (auto-snap off, viewer not moved)" : "");
}

void CameraPath::SelectDelta(int delta) {
	if (m_data.Empty()) return;
	int base = (m_data.Selected() < 0) ? 0 : m_data.Selected();
	SelectIndex(base + delta, /*teleport*/ true);
}

void CameraPath::SelectForEditor(int index) {
	if (m_data.Empty()) return;
	EndCurveValueEdit();
	m_timelinePlayPending = false;
	m_editorPlayPending = false;
	m_editorPlay = false;
	SetMode(Mode::Editing);
	m_data.SetSelected(index);
	const CamMarker& marker = m_data.At(m_data.Selected());

	// The editor always navigates to the selected camera, independent of the optional
	// marker-menu Auto-Snap setting.
	CameraBridge_SetFreeCamEnabled(true);
	// Through the scrub controller, not a raw demo_gototick: a forward gototick on a paused demo
	// never runs, so clicking a diamond right of the playhead did not move the world.
	DemoScrub_Release(marker.tick, /*resume*/ false);
	PushPose(marker);
	m_editorPoseHoldFrames = 4;

	// Rebuilding invalidates the drawer's cached trajectory/keyframes. This prevents a
	// stale camera-path draw packet from surviving an editor jump until the view moves.
	RebuildCamPath();
	CameraBridge_SetPathDrawEnabled(true);
	m_drawOn = true;

	if (m_data.Count() >= 2) {
		m_play.BeginScrub(marker.tick);
		// Hold the pose only while the seek above settles, then hand the free cam back --
		// selecting a marker must not pin the viewport to that key indefinitely.
		ArmScrubRelease(marker.tick);
	} else {
		m_play.Stop();
		m_scrubReleasePending = false;
	}

	advancedfx::Message("[campath] editor selected camera #%d @ tick %d.\n",
		m_data.Selected(), marker.tick);
}

void CameraPath::SelectEditorDelta(int delta) {
	if (m_data.Empty()) return;
	int base = m_data.Selected() < 0 ? 0 : m_data.Selected();
	int next = base + delta;
	if (next < 0) next = 0;
	if (next >= m_data.Count()) next = m_data.Count() - 1;
	SelectForEditor(next);
}

void CameraPath::OpenMenu(int index) {
	if (m_data.Empty()) return;
	SetMode(Mode::Editing);
	SelectIndex(index, /*teleport*/ false);
	CameraBridge_SetFreeCamEnabled(true);
	m_menuOpen = true;
	EnsureDrawState();
	advancedfx::Message("[campath] edit menu open for marker #%d.\n", m_data.Selected());
}

void CameraPath::CloseMenu() { m_menuOpen = false; }

// --- settings ---

void CameraPath::SetSpeedMode(SpeedMode m) { m_speedMode = m; RebuildCamPath(); MarkDirty(); advancedfx::Message("[campath] speed mode -> %s.\n", SpeedModeName()); }
void CameraPath::SetTiming(Timing t) { m_timing = t; MarkDirty(); advancedfx::Message("[campath] timing -> %s (all cameras).\n", TimingName()); }
void CameraPath::SetInterp(Interp i) { m_interp = i; RebuildCamPath(); MarkDirty(); advancedfx::Message("[campath] interpolation -> %s.\n", InterpName()); }
void CameraPath::CycleSpeedMode() { SetSpeedMode((SpeedMode)(((int)m_speedMode + 1) % 3)); }
void CameraPath::ToggleTiming() { SetTiming(m_timing == Timing::Live ? Timing::Freeze : Timing::Live); }
void CameraPath::CycleInterp() { SetInterp(m_interp == Interp::Linear ? Interp::Bezier : Interp::Linear); }

void CameraPath::SetSelectedSpeedMul(float mul) {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) return;
	if (m_data.SelectedIsLast()) {
		Notify("Last marker has no outgoing segment.");
		advancedfx::Message("[campath] marker #%d is the last marker: no outgoing segment.\n", sel);
		return;
	}
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_data.At(sel).speedMul = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d outgoing segment speed x%.2f.\n", sel, mul);
}

void CameraPath::SetConstSpeed(float mul) {
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_constSpeed = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] constant speed x%.2f (whole path).\n", m_constSpeed);
}

void CameraPath::CycleConstSpeed() {
	// 1.0 -> 0.8 -> 0.5 -> 0.2 -> 1.0
	float next;
	if (m_constSpeed > 0.9f) next = 0.8f;
	else if (m_constSpeed > 0.65f) next = 0.5f;
	else if (m_constSpeed > 0.35f) next = 0.2f;
	else next = 1.0f;
	SetConstSpeed(next);
}

void CameraPath::SetAutoSnap(bool on) {
	m_autoSnap = on;
	MarkDirty();
	advancedfx::Message("[campath] auto-snap to selected camera -> %s.\n", on ? "ON" : "OFF");
}
void CameraPath::ToggleAutoSnap() { SetAutoSnap(!m_autoSnap); }

// --- reposition ---

void CameraPath::BeginReposition() {
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) {
		advancedfx::Message("[campath] reposition: select/aim at a marker first.\n");
		return;
	}
	// Seek the demo to the selected marker's tick FIRST, so you reposition the camera at
	// the exact moment it is supposed to be placed (issue: reposition didn't seek).
	SeekDemoTick(m_data.At(sel).tick);
	m_menuOpen = false;
	CameraBridge_SetFreeCamEnabled(true);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(false);
	SetMode(Mode::Reposition);
	advancedfx::Message("[campath] repositioning marker #%d @ tick %d: move + left-click to place (Esc cancels).\n",
		sel, m_data.At(sel).tick);
}

void CameraPath::PlaceReposition() {
	if (GetMode() != Mode::Reposition) return;
	int sel = m_data.Selected();
	if (!m_data.ValidIndex(sel)) { SetMode(Mode::Editing); return; }
	CamMarker& m = m_data.At(sel);
	double o[3], a[3], fov;
	CameraBridge_GetCurrentCamera(o, a, fov);
	m.x = o[0]; m.y = o[1]; m.z = o[2];
	m.pitch = a[0]; m.yaw = a[1]; m.roll = a[2]; m.fov = fov;
	int tick = 0; if (g_MirvTime.GetCurrentDemoTick(tick)) m.tick = tick;
	double dt = 0.0; if (g_MirvTime.GetCurrentDemoTime(dt)) m.time = dt;
	RebuildCamPath();
	MarkDirty();
	SetMode(Mode::Editing);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[campath] marker #%d repositioned @ tick %d (%.1f %.1f %.1f).\n",
		sel, m.tick, m.x, m.y, m.z);
}

void CameraPath::CancelReposition() {
	if (GetMode() != Mode::Reposition) return;
	SetMode(Mode::Editing);
	if (CameraEditor_Active())
		CameraEditor_SetCursorMode(true);
	advancedfx::Message("[campath] reposition cancelled.\n");
}

// --- camera-path playback ---

void CameraPath::ArmPreview() {
	const int n = m_data.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		advancedfx::Message("[campath] preview refused: need >=2 markers (have %d).\n", n);
		return;
	}

	m_data.SetSelected(0);     // marker #1 is always the start of the rebuilt path
	RebuildCamPath();
	if (!m_eval.CanEval()) {
		Notify("Camera path could not be built.");
		advancedfx::Message("[campath] preview refused: path not evaluable (add markers).\n");
		return;
	}

	m_menuOpen = false;
	m_hudHidden = false;
	m_editorPlay = false; // J preview is the FULL preview (banner shown), not an editor test
	m_timelinePlayPending = false;
	m_editorPlayPending = false;
	m_play.Stop();

	// Seek to the first marker and pause so the start frame is stable while composing.
	SeekDemoTick(m_data.Front().tick);
	if (DemoIsPlaying() && !DemoIsPaused()) DemoCmd("demo_pause");

	CameraBridge_SetFreeCamEnabled(true);
	m_play.Reset();
	SetMode(Mode::PreviewArmed);

	advancedfx::Message(
		"[campath] preview ARMED: jumped to first marker (tick %d). Press SPACE to play, Esc to cancel.\n",
		m_data.Front().tick);
}

void CameraPath::StartPreviewPlay() {
	if (GetMode() != Mode::PreviewArmed) {
		ArmPreview();
		if (GetMode() != Mode::PreviewArmed) return; // arm failed (e.g. <2 markers)
	}
	RebuildCamPath();
	if (!m_eval.CanEval()) { advancedfx::Message("[campath] cannot evaluate path.\n"); return; }

	m_play.StartPlay(m_eval.Duration());
	SetMode(Mode::PreviewPlaying);
	m_liveStartPauseGuard = 30; // brief window where a stray pause (held/repeated Space) can't kill the start

	// Insurance: the dolly drives the camera through MirvInput's free-cam override, so make
	// sure free cam is on before we hand poses to it. Without this, if free cam was dropped
	// between arming and playing, TickPlay's poses would no-op and the demo would play from
	// the normal spectator view -- the "demo plays but the camera doesn't fly the path" bug.
	CameraBridge_SetFreeCamEnabled(true);

	// The pause state can lag immediately after demo_gototick. Resuming an already
	// running demo is harmless; doing it unconditionally prevents an armed path from
	// remaining frozen when playback starts. The guard re-resumes for the next ~2s in
	// case the seek's auto-pause lands a frame or two later (see m_liveResumeGuard).
	if (m_timing == Timing::Live) { DemoCmd("demo_resume"); m_liveResumeGuard = 120; }

	advancedfx::Message(
		"[campath] PLAY: %d markers, speedMode=%s, interp=%s, timing=%s, duration=%.2fs.\n",
		Count(), SpeedModeName(), InterpName(), TimingName(), m_eval.Duration());
}

void CameraPath::PlayFromTimeline() {
	// Match J then Space, but automatically: arm/seek first, let the asynchronous
	// demo_gototick settle for a couple of frames, then start playback.
	ArmPreview();
	if (GetMode() != Mode::PreviewArmed)
		return;
	m_editorPlay = true; // keep the timeline HUD visible; suppress the armed prompt
	m_timelinePlayPending = true;
	m_timelinePlayWaitFrames = 0;
}

// Timeline "play": play the path from the demo's CURRENT position without jumping to
// the first marker. In Live timing the dolly only takes over once the playhead reaches
// the first marker's tick (CamPlayback range-gating), so the camera isn't forced onto
// the path before its first keyframe; in Freeze it glides on wall-clock as usual.
void CameraPath::PlayPath() {
	const int n = m_data.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		advancedfx::Message("[campath] play refused: need >=2 markers (have %d).\n", n);
		return;
	}
	RebuildCamPath();
	if (!m_eval.CanEval()) {
		Notify("Camera path could not be built.");
		return;
	}
	m_menuOpen = false;
	m_hudHidden = false;
	m_editorPlay = false; // timeline Play button is a normal play (banner shown)
	// A leftover scrub (from dragging the timeline/curve playhead) leaves the demo PAUSED
	// and the playback state in scrub mode. Clear it before playing or the dolly never
	// advances -- this is why pressing Play after scrubbing looked "frozen".
	m_play.EndScrub();
	// Freeze plays self-contained on wall-clock and needs the free cam on now; Live
	// range-gating toggles the free cam itself as the playhead enters/leaves the range.
	if (m_timing == Timing::Freeze) CameraBridge_SetFreeCamEnabled(true);
	m_play.StartPlay(m_eval.Duration(), /*rangeGated*/ m_timing == Timing::Live);
	SetMode(Mode::PreviewPlaying);
	m_liveStartPauseGuard = 30; // brief window where a stray pause (held/repeated Space) can't kill the start
	// Resume UNCONDITIONALLY in Live: the demo is usually paused here (from a prior scrub)
	// and the cached pause check could miss it, leaving the playhead stuck. demo_resume on
	// an already-playing demo is a harmless no-op. The guard re-resumes for ~2s to defeat
	// a lingering seek auto-pause (see m_liveResumeGuard).
	if (m_timing == Timing::Live) { DemoCmd("demo_resume"); m_liveResumeGuard = 120; }
	advancedfx::Message(
		"[campath] timeline PLAY: %d markers, timing=%s, interp=%s (no jump; dolly active within marker range).\n",
		Count(), TimingName(), InterpName());
}

// Editor TEST playback (Space inside the editor / the "Test" button). Seeks the demo to
// the CURRENT editor playhead tick, then plays the demo AND the dolly together from that
// exact tick so you can preview the camera live, synchronized with the replay. Unlike J's
// ArmPreview this does NOT show the big "Camera Path Preview" banner (m_editorPlay), does
// not jump to the first marker, and engages the dolly immediately (not range-gated).
void CameraPath::PlayFromEditor(int startTick) {
	EndCurveValueEdit();
	const int n = m_data.Count();
	if (n < 2) {
		Notify("Need at least 2 camera markers to play path.");
		advancedfx::Message("[campath] editor play refused: need >=2 markers (have %d).\n", n);
		return;
	}
	RebuildCamPath();
	if (!m_eval.CanEval()) { Notify("Camera path could not be built."); return; }

	// Current editor tick = the scrub playhead if we were scrubbing, else the live tick.
	int tick = m_data.Front().tick;
	const bool scrubbing = m_play.Scrubbing();
	const double scrubTick = m_play.ScrubTick();
	int demoTickRaw = -1;
	bool demoTickOk = false;
	if (startTick >= 0) tick = startTick;
	else if (scrubbing) tick = (int)(scrubTick + 0.5);
	else { int dt = 0; demoTickOk = g_MirvTime.GetCurrentDemoTick(dt); if (demoTickOk) { demoTickRaw = dt; tick = dt; } }
	const int tickBeforeClamp = tick;
	tick = ClampToPathTick(tick);
	const int firstTick = m_data.Front().tick;
	const int lastTick = m_data.Back().tick;
	const bool wrappedFromEnd = startTick < 0 && tick >= lastTick;
	// WHERE DID THE START TICK COME FROM? A second play after a pause was observed wrapping to
	// the first keyframe as if the playhead were past the end, when it was in the middle -- and
	// the state line said scrubbing=0, so the usual stale-scrub suspect is not it. Print every
	// input to that decision rather than inferring it from the outcome.
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("campath.playstart",
			"scrubbing=%d scrubTick=%.1f demoTickOk=%d demoTick=%d preClamp=%d clamped=%d first=%d last=%d wrapped=%d",
			scrubbing ? 1 : 0, scrubTick, demoTickOk ? 1 : 0, demoTickRaw,
			tickBeforeClamp, tick, firstTick, lastTick, wrappedFromEnd ? 1 : 0);
	if (wrappedFromEnd)
		tick = firstTick;

	m_menuOpen = false;
	m_hudHidden = false;
	m_editorPlay = true; // keep the editor HUD visible; suppress preview banners
	m_timelinePlayPending = false;
	m_editorPlayPending = true;
	m_editorPlayTargetTick = tick;
	m_editorPlayWaitFrames = 0;
	m_editorPlayStartTiming = m_eval.TimingAtTick(m_data, tick);

	// Keep deterministic scrub active while the asynchronous seek settles. Playback starts from
	// RunFrame only after the requested tick has landed. Backward (the wrap to the first key) a
	// plain demo_gototick lands in one restore; forward on a paused demo it never runs, so the
	// scrub controller plays in instead.
	m_play.Stop();
	m_liveResumeGuard = 0;
	int nowTick = -1;
	if (g_MirvTime.GetCurrentDemoTick(nowTick) && tick < nowTick) SeekDemoTick(tick);
	else DemoScrub_Release(tick, /*resume*/ false);
	CameraBridge_SetFreeCamEnabled(true);
	m_play.BeginScrub(tick);
	m_scrubReleasePending = false; // editor play owns this scrub until its own handshake ends it
	SetMode(Mode::Editing);

	advancedfx::Message(
		"[campath][INPUT] editor play queued: start tick=%d, plays to LAST keyframe tick=%d (#%d), then stops; waiting for seek.\n",
		tick, lastTick, n);
	if (wrappedFromEnd)
		advancedfx::Message("[campath][INPUT] playhead was at/after the last keyframe -- wrapped to first keyframe tick %d.\n",
			firstTick);
}

void CameraPath::PausePreview() {
	// Swallow the stray pause that a held / auto-repeating Space fires the instant the mode
	// flips to PreviewPlaying (MovieMode's PreviewPlaying branch emits "camtl pause"). Without
	// this, Live playback paused itself one frame after starting. Esc (StopPreview) still cancels.
	if (m_liveStartPauseGuard > 0) {
		advancedfx::Message("[campath] ignored early pause during start guard (%d frames left).\n",
			m_liveStartPauseGuard);
		return;
	}
	if (GetMode() != Mode::PreviewPlaying && !PlaybackPending())
		return;
	m_play.Stop();
	if (m_timing == Timing::Live)
		DemoCmd("demo_pause");
	m_hudHidden = false;
	m_editorPlay = false;
	m_timelinePlayPending = false;
	m_editorPlayPending = false;
	SetMode(Mode::Editing);
	advancedfx::Message("[campath] playback paused at current tick.\n");
}

void CameraPath::StopPreview() {
	EndCurveValueEdit();
	bool was = (GetMode() == Mode::PreviewArmed || GetMode() == Mode::PreviewPlaying);
	m_play.Stop();
	m_hudHidden = false;
	m_editorPlay = false;
	m_timelinePlayPending = false;
	m_editorPlayPending = false;
	SetMode(Mode::Editing);
	if (was) advancedfx::Message("[campath] preview stopped (back to editing).\n");
}

void CameraPath::TogglePreviewHud() {
	Mode m = GetMode();
	if (m != Mode::PreviewArmed && m != Mode::PreviewPlaying) return;
	m_hudHidden = !m_hudHidden;
	advancedfx::Message("[campath] preview HUD %s.\n", m_hudHidden ? "hidden" : "shown");
}

// --- camera timeline / curve editor ---

void CameraPath::ScrubToTick(double tick, bool seek) {
	if (m_data.Count() < 2) {
		Notify("Need at least 2 markers to scrub the path.");
		return;
	}
	tick = (double)ClampToPathTick((int)(tick + 0.5));

	// Deterministic scrub happens while PAUSED so the same tick always lands the same
	// pose. The pure EvalAtTick path (TickScrub) bypasses the clock + pose low-pass, so
	// dragging glides the CAMERA smoothly; the WORLD follows through DemoScrub (it plays forward
	// and seeks only backward -- a per-tick demo_gototick was the choppy part). On release the
	// scrub lands exactly on the tick. Rebuild/pause/free-cam only when ENTERING scrub so
	// per-drag previews stay cheap.
	if (!m_play.Scrubbing()) {
		RebuildCamPath();
		if (!m_eval.CanEval()) return;
		if (DemoIsPlaying() && !DemoIsPaused()) DemoCmd("demo_pause");
		CameraBridge_SetFreeCamEnabled(true);
	}
	if (seek) DemoScrub_Release(tick);
	else DemoScrub_Drag(tick);
	m_play.BeginScrub(tick);
	// Publish to the SHARED cursor so the graph editor's ruler follows this drag too. On release
	// (seek=true) the demo has just been moved to `tick`, so the cursor can hand back to the demo
	// tick -- which is now the same number, leaving both panels agreeing with no special case.
	if (seek) {
		TimelinePlayhead_EndScrub();
		// Release the scrub once the seek lands (RunFrame handshake) instead of latching the
		// viewport to the path pose until some unrelated action calls StopScrub.
		ArmScrubRelease((int)(tick + 0.5));
	} else {
		TimelinePlayhead_BeginScrub(tick);
		m_scrubReleasePending = false; // drag continues; a pending release must not fire mid-drag
	}
}

void CameraPath::ArmScrubRelease(int targetTick) {
	m_scrubReleasePending = true;
	m_scrubReleaseTargetTick = targetTick;
	m_scrubReleaseWaitFrames = 0;
}

void CameraPath::StopScrub() {
	m_scrubReleasePending = false;
	m_play.EndScrub();
	TimelinePlayhead_EndScrub();
}

void CameraPath::PushCurveUndo() {
	if (m_curveUndo.size() >= 64)
		m_curveUndo.erase(m_curveUndo.begin());
	m_curveUndo.push_back({ m_data.All(), m_data.Selected() });
	m_curveRedo.clear(); // a fresh edit invalidates the redo stack
}

double* CameraPath::ChannelTarget(CamMarker& marker, int channel) {
	switch (channel) {
	case 0: return &marker.x;
	case 1: return &marker.y;
	case 2: return &marker.z;
	case 3: return &marker.pitch;
	case 4: return &marker.yaw;
	case 5: return &marker.roll;
	case 6: return &marker.fov;
	default: return nullptr;
	}
}

void CameraPath::BeginCurveValueEdit() {
	if (m_curveEditActive) return;
	m_curveEditStart = { m_data.All(), m_data.Selected() };
	m_curveEditActive = true;
	m_curveEditChanged = false;
}

void CameraPath::PreviewChannelValue(int index, int channel, double v) {
	if (!m_data.ValidIndex(index)) return;
	if (!m_curveEditActive) BeginCurveValueEdit();
	CamMarker& marker = m_data.At(index);
	double* target = ChannelTarget(marker, channel);
	if (!target || *target == v) return;
	*target = v;
	m_curveEditChanged = true;
	RebuildCamPath();
	CameraBridge_SetFreeCamEnabled(true);
	PushPose(marker);
	if (m_play.Scrubbing()) m_play.SetScrubTick(marker.tick);
}

void CameraPath::EndCurveValueEdit() {
	if (!m_curveEditActive) return;
	if (m_curveEditChanged) {
		if (m_curveUndo.size() >= 64) m_curveUndo.erase(m_curveUndo.begin());
		m_curveUndo.push_back(m_curveEditStart);
		m_curveRedo.clear(); // a fresh edit invalidates the redo stack
		MarkDirty();
	}
	m_curveEditActive = false;
	m_curveEditChanged = false;
}

void CameraPath::UndoCurveEdit() {
	EndCurveValueEdit();
	if (m_curveUndo.empty()) {
		Notify("Nothing to undo.");
		return;
	}
	if (m_curveRedo.size() >= 64) m_curveRedo.erase(m_curveRedo.begin());
	m_curveRedo.push_back({ m_data.All(), m_data.Selected() }); // current state, so Redo can return here
	CurveUndoState state = m_curveUndo.back();
	m_curveUndo.pop_back();
	m_data.Restore(state.markers, state.selected);
	m_play.EndScrub();
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] curve edit undone.\n");
}

void CameraPath::RedoCurveEdit() {
	if (m_curveRedo.empty()) {
		Notify("Nothing to redo.");
		return;
	}
	if (m_curveUndo.size() >= 64) m_curveUndo.erase(m_curveUndo.begin());
	m_curveUndo.push_back({ m_data.All(), m_data.Selected() }); // so this redo is itself undoable
	CurveUndoState state = m_curveRedo.back();
	m_curveRedo.pop_back();
	m_data.Restore(state.markers, state.selected);
	m_play.EndScrub();
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] curve edit redone.\n");
}

void CameraPath::MoveKey(int index, int newTick) {
	if (!m_data.ValidIndex(index)) return;
	const int n = m_data.Count();
	// Keep markers ordered: clamp between the neighbours' ticks.
	int lo = (index > 0) ? m_data.At(index - 1).tick + 1 : 0;
	if (newTick < lo) newTick = lo;
	if (index < n - 1) { int hi = m_data.At(index + 1).tick - 1; if (newTick > hi) newTick = hi; }
	if (newTick < 0) newTick = 0;
	if (newTick == m_data.At(index).tick) return;

	PushCurveUndo();
	CamMarker& m = m_data.At(index);
	m.tick = newTick;
	float ipt = g_MirvTime.interval_per_tick_get();
	if (ipt > 0.0f) m.time = (double)newTick * ipt;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d moved to tick %d.\n", index, newTick);
}

void CameraPath::SetChannelValue(int index, int channel, double v) {
	if (!m_data.ValidIndex(index)) return;
	CamMarker& m = m_data.At(index);
	double* target = ChannelTarget(m, channel);
	if (!target) return;
	if (*target == v) return;
	PushCurveUndo();
	*target = v;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d channel %d -> %.3f.\n", index, channel, v);
}

void CameraPath::ReplaceMarkersFromGraph(const std::vector<CamMarker>& markers, bool pushUndo) {
	const int countBefore = m_data.Count();
	const int selIn = m_data.Selected();
	if (pushUndo) PushCurveUndo();
	int sel = m_data.Selected();
	if (sel >= (int)markers.size()) sel = (int)markers.size() - 1;
	if (!markers.empty() && sel < 0) sel = 0;
	m_data.Restore(markers, sel);
	RebuildCamPath();
	EnsureDrawState();
	MarkDirty();
	// The graph editor is the only caller. It bakes at drag rate, so this samples -- except
	// when the marker count changed, which is the structural event. Logged after the rebuild
	// so a "markers arrived but the curve did not rebuild" case is distinguishable.
	if (MvmDebugLog_Active()) {
		static unsigned s_n = 0;
		static int s_lastCount = -1;
		const bool structural = ((int)markers.size() != s_lastCount);
		s_lastCount = (int)markers.size();
		if (structural || (s_n++ % 8) == 0)
			MvmDebugLog_Linef("path.graphbake",
				"in=%zu count=%d->%d sel=%d->%d pushUndo=%d tick0=%d tickN=%d",
				markers.size(), countBefore, m_data.Count(), selIn, m_data.Selected(),
				pushUndo ? 1 : 0,
				markers.empty() ? -1 : markers.front().tick,
				markers.empty() ? -1 : markers.back().tick);
	}
}

void CameraPath::SetEaseIndex(int index, Ease e) {
	if (!m_data.ValidIndex(index)) return;
	m_data.At(index).ease = e;
	MarkDirty(); // easing is applied at eval time; no geometry rebuild needed
	advancedfx::Message("[campath] marker #%d ease -> %d.\n", index, (int)e);
}

void CameraPath::SetSpeedMulIndex(int index, float mul) {
	if (!m_data.ValidIndex(index)) return;
	if (index == m_data.Count() - 1) {
		Notify("Last marker has no outgoing segment.");
		return;
	}
	if (mul < 0.2f) mul = 0.2f; else if (mul > 1.0f) mul = 1.0f;
	m_data.At(index).speedMul = mul;
	RebuildCamPath();
	MarkDirty();
	advancedfx::Message("[campath] marker #%d outgoing segment speed x%.2f.\n", index, mul);
}

bool CameraPath::CurrentPathTick(double& outTick) const {
	if (!m_play.Playing() && !m_play.Scrubbing())
		return false;
	outTick = m_play.LastEvalTick();
	return true;
}

bool CameraPath::EvalPoseAtTick(double tick, double out[7]) const {
	CamPathEval::Pose p = m_eval.EvalAtTick(m_data, tick);
	if (!p.valid) return false;
	out[0] = p.x; out[1] = p.y; out[2] = p.z;
	out[3] = p.pitch; out[4] = p.yaw; out[5] = p.roll;
	out[6] = p.fov;
	return true;
}

// --- hover picking: CameraPath::UpdateHover lives in CameraPathPicking.cpp (the
// viewport-dependent crosshair pick; everything else in this file is viewport-free) ---

// --- per-frame ---

void CameraPath::RunFrame() {
	// Auto-load when the active demo changes. Keyed off the demo the ENGINE is actually
	// playing (PlayingDemoPath), so the SAME .dem gets the SAME markers whether it was opened
	// from our Downloaded tab, CS2's native Your Matches tab, or a console playdemo.
	std::wstring cur = PlayingDemoPath();
	if (cur != m_demoPath) {
		// Flush any unsaved edits to the OUTGOING demo's project.mvm before we wipe the markers.
		// Per-frame autosave (end of RunFrame) almost always beats us here, but an edit and a
		// demo switch in the same frame would otherwise silently drop the edit. Save() keys off
		// m_demoPath, so this must run while it still points at the previous demo.
		if (m_dirty) { Save(); m_dirty = false; }
		m_demoPath = cur;
		m_data.DeleteAll();
		m_curveUndo.clear(); m_curveRedo.clear();
		m_menuOpen = false;
		m_play.Stop();
		m_hudHidden = false;
		m_editorPlay = false;
		m_timelinePlayPending = false;
		m_editorPlayPending = false;
		SetMode(Mode::Editing);
		Load();              // no-op if the project has no campath section
		RebuildCamPath();
		EnsureDrawState();
	}

	// Count down the post-start pause guard so PausePreview() ignores the stray "camtl pause"
	// a held / repeated Space fires right after playback begins, then accepts pauses normally.
	if (m_liveStartPauseGuard > 0) --m_liveStartPauseGuard;

	// Fail-safe: if a mode/scrub was left active when the demo stopped, return to a
	// clean editing state so MirvInput isn't left suspended with no closable panel.
	// IsPlayingDemo() reports false transiently during a demo_gototick seek, so debounce:
	// only tear down after the demo has been stopped for several consecutive frames. A
	// single mid-seek blip must NOT kill a play/scrub that just started.
	if (GetMode() != Mode::Editing || m_menuOpen || m_play.Playing() || m_play.Scrubbing()) {
		if (!DemoIsPlaying()) ++m_demoStoppedFrames; else m_demoStoppedFrames = 0;
		if (m_demoStoppedFrames >= 20) {
			m_menuOpen = false;
			m_play.Stop();
			m_hudHidden = false;
			m_editorPlay = false;
			m_timelinePlayPending = false;
			m_editorPlayPending = false;
			m_liveResumeGuard = 0;
			SetMode(Mode::Editing);
		}
	} else {
		m_demoStoppedFrames = 0;
	}

	// Expire the transient notice banner.
	if (!m_notice.empty() && NowSec() >= m_noticeExpireSec) m_notice.clear();

	UpdateHover();

	if (m_editorPlayPending) {
		// The scrub controller is still landing the start tick: starting now would resume a demo it
		// is pausing and stepping.
		if (!DemoScrub_Active()) ++m_editorPlayWaitFrames;
		int tick = -1;
		const bool atTarget = g_MirvTime.GetCurrentDemoTick(tick)
			&& std::abs(tick - m_editorPlayTargetTick) <= 1;
		// Start as soon as the seek lands, but NEVER cancel into a stuck scrub if it doesn't
		// settle exactly -- after a short fallback, start anyway. During a match tech-pause the
		// demo tick churns (CGameRules pause/unpause every tick) so an exact-tick settle can
		// never match; the old "seek did not settle" cancel left the editor stuck in SCRUBBING
		// with the play button frozen on the play icon. Live playback binds the camera to demo
		// time, so starting from wherever the demo actually is still flies the path; the resume
		// guard keeps the demo rolling.
		if (!DemoScrub_Active() && ((atTarget && m_editorPlayWaitFrames >= 2) || m_editorPlayWaitFrames >= 30)) {
			m_editorPlayPending = false;
			m_play.EndScrub();
			CameraBridge_SetFreeCamEnabled(true);
			m_play.StartPlay(m_eval.Duration(), /*rangeGated*/ false, m_editorPlayStartTiming);
			SetMode(Mode::PreviewPlaying);
			m_liveStartPauseGuard = 30; // brief window where a stray pause (held/repeated Space) can't kill the start
			if (m_timing == Timing::Live) { DemoCmd("demo_resume"); m_liveResumeGuard = 120; }
			advancedfx::Message("[campath] editor play started from tick %d (atTarget=%d, waited %d frames, freecam=%d, guard=%d).\n",
				m_editorPlayTargetTick, atTarget ? 1 : 0, m_editorPlayWaitFrames,
				CameraBridge_GetFreeCamEnabled() ? 1 : 0, m_liveStartPauseGuard);
		}
	}

	Mode mode = GetMode();
	if (m_debug) {
		static Mode s_lastLoggedMode = (Mode)-1;
		if (mode != s_lastLoggedMode) {
			s_lastLoggedMode = mode;
			advancedfx::Message("[campath][state] mode=%s timing=%s playing=%d pending=%d scrubbing=%d guard=%d\n",
				ModeName(), TimingName(), m_play.Playing() ? 1 : 0, PlaybackPending() ? 1 : 0,
				m_play.Scrubbing() ? 1 : 0, m_liveStartPauseGuard);
		}
	}
	if (mode == Mode::PreviewArmed && !m_data.Empty()) {
		PushPose(m_data.Front()); // hold the camera at the start frame
		if (m_timelinePlayPending) {
			++m_timelinePlayWaitFrames;
			int tick = -1;
			const bool atStart = g_MirvTime.GetCurrentDemoTick(tick)
				&& std::abs(tick - m_data.Front().tick) <= 1;
			// Wait for the seek to actually land before playing. The "Demo Skipping" stall
			// can run for hundreds of ms (~20+ frames), so the blind fallback must be long
			// enough not to fire mid-skip -- doing so unpaused the demo for a single tick
			// and then the seek's own auto-pause refroze it. 240 frames is the safety net
			// for a seek that never reports the target tick.
			if ((atStart && m_timelinePlayWaitFrames >= 2) || m_timelinePlayWaitFrames >= 240) {
				m_timelinePlayPending = false;
				StartPreviewPlay();
				mode = GetMode();
			}
		}
	} else if (mode == Mode::PreviewPlaying) {
		// Editor / timeline transport playback re-claims free-cam ownership every frame:
		// while the demo advances in Live timing the engine re-establishes the spectator
		// view each frame, which can override the pushed path pose so the dolly only
		// "appears" to update when the demo pauses. Gated to m_editorPlay so it never
		// fights the range-gated playpath mode (which intentionally toggles free cam as the
		// playhead enters/leaves the marker range).
		if (m_editorPlay) CameraBridge_SetFreeCamEnabled(true);
		// Keep the demo running in Live timing: the lingering demo_gototick auto-pause
		// (and any one-frame seek pause) would otherwise freeze the dolly right after it
		// starts. Re-resume while the guard is active and the demo is paused -- nothing the
		// USER does pauses during PreviewPlaying (Space is swallowed; the HUD pause button
		// leaves this mode), so a paused demo here is always the seek, never intentional.
		if (m_liveResumeGuard > 0) {
			if (m_timing == Timing::Live && DemoIsPaused()) DemoCmd("demo_resume");
			--m_liveResumeGuard;
		}
		if (m_debug) {
			static unsigned s_playFrameLogThrottle = 0;
			if ((s_playFrameLogThrottle++ % 30) == 0) {
				int tick = -1;
				double time = 0.0;
				g_MirvTime.GetCurrentDemoTick(tick);
				g_MirvTime.GetCurrentDemoTime(time);
				advancedfx::Message(
					"[campath][frame] timing=%s demoTick=%d demoTime=%.2f paused=%d freecam=%d resumeGuard=%d\n",
					TimingName(), tick, time, DemoIsPaused() ? 1 : 0,
					CameraBridge_GetFreeCamEnabled() ? 1 : 0, m_liveResumeGuard);
			}
		}
		const bool playDone = m_play.TickPlay(m_data, m_eval, MakeContext());
		if (m_timing == Timing::Live && m_play.LiveEndSettling() && !playDone && !DemoIsPaused())
			DemoCmd("demo_pause");
		if (playDone && PlaybackLoop_Enabled() && m_data.Count() >= 2 && m_eval.CanEval()) {
			// LOOP is on: play the path again instead of stopping on the last keyframe. Same
			// switch as the tick loop, because "loop" is one idea to the person pressing it --
			// what it loops is simply whatever is playing.
			RestartPathPlayback();
		} else if (playDone) {
			// Reached the LAST keyframe tick. The timeline only covers the marker range, so
			// HALT the demo here (Live) instead of letting it roll past the path -- the whole
			// point of the timeline is to bound playback to where the cam path ends. Freeze is
			// self-contained on wall-clock and needs no demo pause.
			const int lastTick = m_data.Empty() ? -1 : m_data.Back().tick;
			advancedfx::Message("[campath] play reached end of path (%d markers, last keyframe tick=%d) -- stopping at last keyframe.\n",
				m_data.Count(), lastTick);
			FinishPlaybackAtLastKey();
		}
	} else if (m_play.Scrubbing() && Render_IsJobActive()) {
		// A render job drives the path camera itself (RenderSessionDriver::DrivePathCamera); a held
		// scrub pose pushed here would override every captured frame.
		StopScrub();
	} else if (m_play.Scrubbing()) {
		m_play.TickScrub(m_data, m_eval); // deterministic tick-perfect preview (paused)
		// Release-after-settle: a slider release / marker select armed this. Keep holding the
		// path pose while the demo_gototick lands, then end the scrub so the user gets the free
		// cam back at that pose. PlaybackPending() means editor play owns the scrub -- stand by.
		if (m_scrubReleasePending && !PlaybackPending()) {
			++m_scrubReleaseWaitFrames;
			int tick = -1;
			const bool atTarget = g_MirvTime.GetCurrentDemoTick(tick)
				&& std::abs(tick - m_scrubReleaseTargetTick) <= 1;
			// Same reasoning as the editor-play handshake: never wait forever on an exact tick
			// (pause churn can make the settle unreachable) -- after the cap, release anyway. The
			// world lands through DemoScrub (a backward landing is a seek plus a short played lead).
			if ((atTarget && !DemoScrub_Active() && m_scrubReleaseWaitFrames >= 2) || m_scrubReleaseWaitFrames >= 300) {
				m_scrubReleasePending = false;
				m_play.EndScrub();
				TimelinePlayhead_EndScrub();
				advancedfx::Message(
					"[campath] scrub released after settle (target=%d atTarget=%d waited=%d) -- free cam handed back.\n",
					m_scrubReleaseTargetTick, atTarget ? 1 : 0, m_scrubReleaseWaitFrames);
			}
		}
	} else if (m_editorPoseHoldFrames > 0 && m_data.ValidIndex(m_data.Selected())) {
		PushPose(m_data.At(m_data.Selected()));
		--m_editorPoseHoldFrames;
	}

	EnsureDrawState(); // per-frame: tracks mode so visuals hide while playing, show while editing
	SyncDrawerStyle();

	if (m_dirty) { Save(); m_dirty = false; }
}

// --- persistence (delegates the schema to CamMarkers) ---

std::string CameraPath::SerializedPath() const {
	return m_data.Serialize(MakeSettings(), m_data.Selected());
}

void CameraPath::Save() {
	if (m_demoPath.empty()) return; // unknown demo -> memory only
	DemoProjectStore_WriteSection(ProjectGame::Cs2, m_demoPath, ProjectSection::Campath, SerializedPath());
}

void CameraPath::Load() {
	if (m_demoPath.empty()) return;
	m_curveUndo.clear(); m_curveRedo.clear();
	PathSettings st = MakeSettings(); // seed with current defaults
	int sel = m_data.Selected();
	std::string payload;
	const bool ok = DemoProjectStore_ReadSection(ProjectGame::Cs2, m_demoPath,
		ProjectSection::Campath, payload) && m_data.Parse(payload, st, sel);
	if (ok) {
		m_speedMode = (SpeedMode)st.speedMode;
		m_timing = (Timing)st.timing;
		m_interp = (Interp)st.interp;
		m_constSpeed = st.constSpeed;
		m_autoSnap = st.autoSnap;
		// The evaluated curve must follow the new markers: without this, `marker load` from the
		// console left g_CamPath on the previous path, and a Camera Path render evaluated the old
		// curve against the new marker ticks (a frozen pose from the wrong shot).
		RebuildCamPath();
		EnsureDrawState();
		advancedfx::Message("[campath] loaded %d markers from project.mvm.\n", Count());
	}
}

} // namespace Filmmaker

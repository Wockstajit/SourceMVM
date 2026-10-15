#pragma once

// CameraPath: a Black Ops 2 Theater-style "dolly camera" path system, built as a
// front-end over HLAE's existing camera path engine (g_CamPath in main.cpp) +
// keyframe drawer (g_CampathDrawer).
//
// This class is now a THIN FACADE. The heavy lifting lives in three modules it
// owns and wires together:
//   * CamMarkers  - the authoritative marker list + JSON sidecar (plain data).
//   * CamPathEval - rebuilds g_CamPath on the speed-INDEPENDENT shape axis and
//                   exposes PURE pose evaluators (EvalAtTick/Timing/DemoTime).
//   * CamPlayback - the predictive Live clock + Freeze wall-clock + output
//                   low-pass + the deterministic SCRUB mode.
// The facade keeps the editing-mode state machine, hover picking, drawer styling,
// the transient notice, persistence orchestration, and the public API the rest of
// the filmmaker code already calls (FilmmakerCommand, MovieMode, MarkerHud,
// CameraTimelineHud, main.cpp's suspend hook).
//
// Freeze vs Live is a GLOBAL mode: all markers share m_timing.
//   * Freeze: the demo is paused and the dolly glides on wall-clock time.
//   * Live:   the demo keeps playing and the camera follows recorded demo time.
//
// Threading: every mutating method runs on the main/UI thread (invoked from the
// mirv_filmmaker console dispatch). Fields read off-thread by MovieMode::OnKey
// (WndProc thread) are atomic: m_hovered, m_mode and m_menuOpen.

#include "CamMarkers.h"
#include "CamPathEval.h"
#include "CamPlayback.h"

#include <atomic>
#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class CameraPath {
public:
	enum class SpeedMode { Manual = 0, Constant = 1, PerSegment = 2 };
	enum class Timing { Live = 0, Freeze = 1 };
	enum class Interp { Linear = 0, Bezier = 1 }; // "Bezier" == natural-cubic "Smooth" (UI label)
	enum class Mode { Editing = 0, Reposition = 1, PreviewArmed = 2, PreviewPlaying = 3 };

	// Main/UI thread, once per frame (from Filmmaker::RunMainThreadFrame).
	void RunFrame();

	// --- editing actions (main thread; dispatched from the console cmd) ---
	void PlaceMarker();                 // K
	void DeleteIndex(int index);        // L (aimed) / menu
	void DeleteAll(bool confirmed);     // menu (with confirm)
	void SelectIndex(int index, bool teleport);
	void SelectDelta(int delta);        // menu arrows (teleports to the camera)
	void SelectForEditor(int index);    // timeline/curve: seek + hold the exact camera pose
	void SelectEditorDelta(int delta);
	void OpenMenu(int index);           // F (aimed)
	void CloseMenu();

	// --- reposition (move the selected marker interactively) ---
	void BeginReposition();             // menu "Reposition" button
	void PlaceReposition();             // left-click while repositioning
	void CancelReposition();            // Esc while repositioning

	// --- camera-path playback ---
	void ArmPreview();                  // J: jump to first marker + pause; wait for Space
	void StartPreviewPlay();            // Space: begin playback (BO2 arm flow)
	void PlayFromTimeline();            // timeline Play: same path start as J+Space, editor stays visible
	void PlayPath();                    // timeline play: Live, no jump, dolly only in marker range
	void PlayFromEditor();              // Space / "Test" button: seek to the editor tick, then play
	                                    // the demo + dolly together from there (no preview banner)
	void Play() { ArmPreview(); }       // console/dashboard alias for the J arm step
	void PausePreview();                // timeline pause: stop at current tick, keep position
	void StopPreview();                 // Esc: stop + return to editing
	void TogglePreviewHud();            // Tab: hide/show HUD during playback

	// --- settings ---
	void SetSpeedMode(SpeedMode m);
	void SetTiming(Timing t);
	void SetInterp(Interp i);
	void CycleSpeedMode();
	void ToggleTiming();
	void CycleInterp();
	void SetSelectedSpeedMul(float mul); // selected marker's OUTGOING segment speed
	void SetConstSpeed(float mul);       // global constant-speed multiplier
	void CycleConstSpeed();
	void SetAutoSnap(bool on);           // teleport viewer to a marker when selected?
	void ToggleAutoSnap();

	// --- camera timeline / curve editor (plan Part C/D) ---
	// Scrub to a tick. seek=true also moves the demo world (demo_gototick) -- do this on
	// slider RELEASE. seek=false only previews the CAMERA along the path (pure EvalAtTick,
	// no engine seek) -- do this while DRAGGING so the camera glides smoothly.
	void ScrubToTick(double tick, bool seek = true);
	void StopScrub();                    // leave scrub mode (resume normal control)
	void ArmScrubRelease(int targetTick); // end scrub automatically once the seek settles
	bool IsScrubbing() const { return m_play.Scrubbing(); }
	double ScrubTick() const { return m_play.ScrubTick(); }
	void MoveKey(int index, int newTick);                 // retime a marker (curve drag)
	void SetChannelValue(int index, int channel, double v); // 0..6 = x,y,z,pitch,yaw,tilt(roll),fov
	// Replace the whole marker list from the graph editor's baked curves (graph -> path
	// unification). pushUndo adds ONE camera-path curve-undo entry for the owning gesture.
	// Deliberately does NOT reseed the graph editor (that's the caller; a reseed here would
	// wipe its selection/undo mid-gesture) and prints nothing (fires per-frame during drags).
	void ReplaceMarkersFromGraph(const std::vector<CamMarker>& markers, bool pushUndo);
	void BeginCurveValueEdit();
	void PreviewChannelValue(int index, int channel, double v);
	void EndCurveValueEdit();
	void UndoCurveEdit();
	void RedoCurveEdit();
	void SetEaseIndex(int index, Ease e);
	void SetSpeedMulIndex(int index, float mul);
	// Pure pose at a demo tick (curve sampling + scrub readout). false => off-path.
	bool EvalPoseAtTick(double tick, double out[7]) const;
	// Where the dolly is on its path RIGHT NOW, in demo ticks. True only while the path is
	// actually driving (playing or scrubbing). The view-stage shake layer reads this so it keys
	// off the same point on the path the pose was evaluated at.
	bool CurrentPathTick(double& outTick) const;

	// --- reads for the UI/HUD (main thread) ---
	int Count() const { return m_data.Count(); }
	int Selected() const { return m_data.Selected(); }
	int Hovered() const { return m_hovered.load(); }
	int HoveredAtomic() const { return m_hovered.load(); } // safe from WndProc thread
	Mode GetMode() const { return (Mode)m_mode.load(); }   // safe from WndProc thread
	bool IsPlaying() const { return m_play.Playing(); }
	bool PlaybackPending() const { return m_editorPlayPending || m_timelinePlayPending; }
	// True while the dolly is actively driving the FINAL view (PreviewPlaying + playing +
	// free cam engaged). main.cpp's view-setup hook reads this (via CameraPathOwnsView) to
	// block CS2's demo-view-override / TrueView from re-owning the view mid-playback.
	bool OwnsView() const;
	bool Debug() const { return m_debug; }       // verbose [campath]/[setupview] logging
	void SetDebug(bool on) { m_debug = on; }
	// True while an EDITOR-TEST playback (Space / Test button) is running -- the HUD
	// suppresses the big "Playing Camera Path" preview banner for these.
	bool EditorPlay() const {
		Mode m = GetMode();
		return m_editorPlay && (m == Mode::PreviewArmed || m == Mode::PreviewPlaying);
	}
	bool MenuOpen() const { return m_menuOpen.load() && GetMode() == Mode::Editing; }
	bool WantsCursor() const { return MenuOpen(); } // suspend free-cam look + show cursor
	bool HudHidden() const { return m_hudHidden; }
	bool PreviewHudHidden() const;                  // HUD masked during preview
	SpeedMode GetSpeedMode() const { return m_speedMode; }
	Timing GetTiming() const { return m_timing; }
	Interp GetInterp() const { return m_interp; }
	float SelectedSpeedMul() const;
	float ConstSpeed() const { return m_constSpeed; }
	bool AutoSnap() const { return m_autoSnap; }
	bool SelectedIsLast() const { return m_data.SelectedIsLast(); }
	int PlaySegment() const { return m_play.Segment(); } // current segment while playing (0-based)
	bool HasPathRange() const { return m_data.Count() >= 2; }
	int FirstPathTick() const;
	int LastPathTick() const;
	int ClampToPathTick(int tick) const;
	const char* Notice() const;                     // transient on-screen message ("" if none)
	const char* SpeedModeName() const;
	const char* TimingName() const;
	const char* InterpName() const;        // what the user SELECTED
	// What the path is ACTUALLY evaluating. Every UI that shows the mode must show THIS one.
	const char* EffectiveInterpName() const;
	// Smooth selected, but the cubic curve needs >=3 markers (CamPathEval::Rebuild), so a
	// 2-marker path only eases its timing along the straight chord. Leaving this flag off is
	// how "it says Smooth but it isn't smooth" happens -- the UI dims Smooth on it.
	bool InterpDegraded() const;
	const char* ModeName() const;

	// Read-only marker access for the timeline / curve editor.
	const std::vector<CamMarker>& Markers() const { return m_data.All(); }

	// Persistence (the "campath" section of the demo's project.mvm under
	// %APPDATA%\HLAE\source_mvm\demos\cs2\).
	// SerializedPath is that section's JSON (markers + path settings).
	std::string SerializedPath() const;
	void Save();
	void Load();

private:
	void RebuildCamPath();   // m_eval.Rebuild(m_data, settings)
	void MarkDirty() { m_dirty = true; }
	void EnsureDrawState();  // toggle the in-world gizmos with marker presence
	void SyncDrawerStyle();  // push Freeze/Live colour + aimed-highlight to the drawer
	void UpdateHover();      // crosshair pick -> m_hovered
	void PushPose(const CamMarker& m); // push a marker pose to the camera this frame
	void SeekDemoTick(int tick);
	void HoldEditorAtMarker(int index);
	void FinishPlaybackAtLastKey();
	// Same end-of-path moment as FinishPlaybackAtLastKey, but rewinds and plays again. Used
	// while the shared LOOP switch is on -- see the call site in RunFrame.
	void RestartPathPlayback();
	double NowSec();         // monotonic seconds (QPC) for the notice timer
	void Notify(const char* msg);      // raise a transient on-screen message
	void PushCurveUndo();
	double* ChannelTarget(CamMarker& marker, int channel);
	void SetMode(Mode m) { m_mode.store((int)m); }
	PathSettings MakeSettings() const; // gather the persisted settings for eval/save
	CamPlayback::Context MakeContext() const; // cosmetic/mode context for the driver

	CamMarkers m_data;
	CamPathEval m_eval;
	CamPlayback m_play;

	struct CurveUndoState {
		std::vector<CamMarker> markers;
		int selected = -1;
	};
	std::vector<CurveUndoState> m_curveUndo;
	std::vector<CurveUndoState> m_curveRedo; // states popped by Undo, for Redo (cleared on a new edit)

	SpeedMode m_speedMode = SpeedMode::Manual;
	Timing m_timing = Timing::Live;
	Interp m_interp = Interp::Linear; // Linear default so 2-3 markers can play
	float m_constSpeed = 1.0f;        // global multiplier for Constant speed mode
	bool m_autoSnap = false;          // selecting a marker teleports the viewer to it (default OFF)

	// Read off-thread by OnKey (key routing) -> atomic.
	std::atomic<int> m_hovered{ -1 };
	std::atomic<int> m_mode{ (int)Mode::Editing };
	// Read off-thread (OnKey ESC + main.cpp GetSuspendMirvInput) -> atomic.
	std::atomic<bool> m_menuOpen{ false };
	bool m_hudHidden = false;       // while armed, Tab hides other UI but keeps preview prompt
	bool m_editorPlay = false;      // current playback was started as an editor TEST (no banner)
	bool m_timelinePlayPending = false; // wait for the first-marker seek to settle before play
	int m_timelinePlayWaitFrames = 0;
	bool m_editorPlayPending = false;
	int m_editorPlayTargetTick = 0;
	int m_editorPlayWaitFrames = 0;
	double m_editorPlayStartTiming = 0.0;
	int m_editorPoseHoldFrames = 0; // keep selection stable across asynchronous demo seeks
	// Scrub release-after-settle (the "editor scrub stays latched" fix): a slider release or a
	// marker select still holds the deterministic scrub pose while its demo_gototick settles,
	// then ENDS the scrub so the free cam is handed back instead of being pinned to the path
	// indefinitely. Same handshake shape as m_editorPlayPending above.
	bool m_scrubReleasePending = false;
	int m_scrubReleaseTargetTick = -1;
	int m_scrubReleaseWaitFrames = 0;
	// demo_gototick leaves the demo auto-paused and the seek (a multi-100ms "Demo Skipping"
	// stall) re-asserts that pause a frame or two after our demo_resume. While Live playback
	// is starting we re-issue demo_resume for a short window so the dolly actually advances
	// instead of unpausing for a single tick and freezing (the "Play does nothing" bug).
	int m_liveResumeGuard = 0;
	// Frames after a play START during which PausePreview() is ignored. Defeats the stray
	// "camtl pause" that a held / auto-repeating Space fires the instant the mode flips to
	// PreviewPlaying (MovieMode's PreviewPlaying branch emits it), which otherwise killed
	// Live playback one frame in. Set ~30 at each start site; counted down per frame in RunFrame.
	int m_liveStartPauseGuard = 0;
	// The RunFrame fail-safe resets to Editing when the demo stops. IsPlayingDemo() blips
	// false MID-SEEK, so require several consecutive stopped frames before tearing down a
	// preview -- otherwise a freshly started play is killed one frame in by the seek itself.
	int m_demoStoppedFrames = 0;
	bool m_debug = false; // verbose [campath]/[setupview] logging ([state]/[push]/[owner]); toggle via "camtl debug 0|1"
	bool m_curveEditActive = false;
	bool m_curveEditChanged = false;
	CurveUndoState m_curveEditStart;
	bool m_drawOn = false;
	bool m_dirty = false;

	std::string m_notice;          // transient on-screen message
	double m_noticeExpireSec = 0.0;// NowSec() after which the notice clears

	std::wstring m_demoPath;       // current demo path; empty => no auto-save
};

CameraPath& CameraPathRef();

// Console command entry: handles "mirv_filmmaker marker ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "marker").
void Marker_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

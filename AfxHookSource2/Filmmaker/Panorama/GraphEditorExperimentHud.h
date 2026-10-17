#pragma once

// GraphEditorExperimentHud: the EXPERIMENTAL After-Effects-style camera graph editor.
//
// This is an OPT-IN, fully isolated overlay: default OFF, toggled by the "Experiment"
// button (or `mirv_filmmaker grapheditor toggle`). It mirrors the proven CameraEditorHud /
// CameraTimelineHud bridge pattern (PanoramaBridge pinned to the in-game HUD panel, a panel
// built ONCE from embedded JS, a small state JSON pushed each frame, JS issues console
// commands). It owns its own GraphExpModel (per-channel keyframes + Bezier handles) seeded
// read-only from the stable camera path, and it NEVER mutates the stable
// CameraPath/CamMarkers — turning it off (or deleting these files) leaves the regular editor
// exactly as it was.
//
// Live-drive: while enabled + driving, each frame it samples a pose from its own curves at
// the playhead tick and pushes it via CameraBridge_SetCameraPose (the same per-frame pose
// push the stable path uses), and reports OwnsView() so CS2's demo-view-override yields.
//
//   C++ -> JS : attribute "state" (channels/keyframes/handles/selection + streamed mouse),
//               then $.GraphExp.render().
//   JS  -> C++: "mirv_filmmaker grapheditor ..." console commands.

#include "PanoramaBridge.h"
#include "../Movie/CamMarkers.h"
#include "../Movie/GraphExpModel.h"

#include <string>
#include <vector>

namespace advancedfx { class ICommandArgs; }

namespace Filmmaker {

class GraphEditorExperimentHud {
public:
	// Main/UI thread, once per frame from Filmmaker::RunMainThreadFrame (after CameraEditor).
	void RunFrame();

	void SetEnabled(bool v) { m_enabled = v; }
	void Toggle() { m_enabled = !m_enabled; }
	bool Enabled() const { return m_enabled; }

	void SetDrive(bool v) { m_drive = v; }
	void ToggleDrive() { m_drive = !m_drive; }
	bool Drive() const { return m_drive; }
	// Read-only curves (keys + Bezier handles per channel), for the cam-path share.
	const GraphExpModel& Model() const { return m_model; }

	// View ownership while live-driving (read by main.cpp's setup-view hook, OR'd with the
	// stable path's owner) + cursor routing (read by MovieMode's click-consume / suspend).
	bool OwnsView() const;
	// A playhead drag flies the camera path: only while the path layer's AUTO is on.
	bool ScrubDrives() const;
	bool WantsCursor() const { return m_enabled; }

	// --- command surface (called from FilmmakerCommand's "grapheditor" dispatch) ---
	void CmdChannel(int ch, int op);          // op: 0 hide,1 show,2 solo,3 unsolo
	void CmdSelect(int ch, int id, bool add);
	void CmdSelectAll();
	void CmdSelectSet(const char* csv);       // "ch:id,ch:id,..." (drag-select result)
	void CmdSelectClear();
	void CmdEditBegin();                      // undo snapshot for the upcoming gesture
	void CmdMoveSelectedBy(double dTick, double dValue);
	void CmdMoveKeyAbs(int ch, int id, double tick, double value);
	void CmdSetValue(int ch, int id, double value);
	void CmdAddKey(int ch, double tick, double value);
	void CmdDeleteKey(int ch, int id);
	void CmdDeleteSelected();
	void CmdClear();
	void CmdSetHandle(int ch, int id, int side, double tx, double dv, bool reflect);
	void CmdClearHandles(int ch, int id);
	void CmdEase(int mode, bool selectedOnly); // 0 ease-in, 1 ease-out, 2 ease-in/out (selection or all)
	void CmdSetInterp(bool smooth, bool selectedOnly); // graph-wide (or selection) Smooth / Linear toggle; graph-wide also sets CameraPath::Interp
	// Reshape the graph's handles to a path-side interp change (camtl interp) WITHOUT baking or
	// pushing undo. No-op on a never-seeded model (a sync there would bake zero keys over the path).
	void MirrorPathInterp(bool smooth);
	void CmdAlignSelection(int ch, int id);    // snap every selected key to (ch,id)'s tick (right-click Align)
	void CmdAlignSelectionToTick(double tick); // snap every selected key to an explicit tick
	void CmdAlignSelectionToPlayhead() { CmdAlignSelectionToTick(PlayheadTick()); }
	void CmdPlayhead(double tick, bool seek);
	void CmdUndo();
	void CmdRedo();
	void CmdReseed();                         // re-seed from the current stable path

	void RequestEval(const std::string& js) { m_evalQueue.push_back(js); }

	// One-line dump of the whole editor+model state for the mvm_debug log (`state.graph`,
	// emitted by MvmCommandTrace after any `mirv_filmmaker grapheditor ...`). The console
	// cannot carry this out of the game, so the log file is the only channel -- see
	// Docs/agent/mvm-debug-instrumentation.md.
	std::string DebugStateLine() const;

private:
	void* FindRoot();
	bool BuildIfNeeded();
	void Teardown();
	void OnEnter();   // adopt path changes, sync drive mode, stop stable scrub only when graph has keys
	void OnExit();    // stop driving + tear down
	// Seed (empty model) or reconcile (kept model) from the camera path when its markers differ
	// from the last set this graph seeded from or baked. `why` names the caller in the log.
	// Returns true if the model changed.
	bool AdoptPathChanges(const char* why);
	std::string BuildStateJson();
	void DriveCameraThisFrame();
	bool DrivingNow() const; // a playhead scrub or a key edit is in progress (the only times the graph flies the camera)
	void BumpRev() { ++m_rev; }
	double PlayheadTick() const; // the SHARED cursor (Movie/TimelinePlayhead.h), not a private one
	// UNIFICATION: project the graph model back into the stable camera path (CameraPath
	// markers), so the Camera timeline / playback / sidecar always mirror the curves the user
	// edited in the graph. Markers are baked at the UNION of all channels' key ticks, each
	// channel sampled at those ticks. Called from every data-mutating command (per-frame
	// during drags, so the Camera timeline moves live). One camera-path curve-undo entry is
	// pushed per graph gesture (armed by CmdEditBegin / internal BeginEdit callers).
	void SyncToCameraPath();
	// mvm_debug probe for a model mutation: formats `detail` and appends the resulting model
	// shape (per-channel key counts, totals, selection, rev, gate state). No-op when the log
	// is off. Drag-rate callers throttle at the call site.
	void DebugLogEdit(const char* op, const char* fmt, ...);

	PanoramaBridge m_bridge;
	GraphExpModel m_model;
	bool m_pathUndoPending = false; // arm one CameraPath curve-undo push for the next sync
	// The camera path's markers as of this graph's last seed or bake. Anything else that differs
	// from this changed the path behind the graph's back (see AdoptPathChanges).
	std::vector<CamMarker> m_pathSeen;
	bool m_pathSeenValid = false;
	int m_editDriveFrames = 0;    // frames left of "show the edit" drive after a bake
	bool m_releaseDrive = false;  // our playhead was released: keep flying the released tick until the world lands

	void* m_hudPanel = nullptr;
	void* m_root = nullptr;       // #GraphExpRoot
	short m_symState = -1;
	bool m_built = false;
	bool m_enabled = false;       // graphEditorExperimentEnabled (default OFF)
	bool m_drive = true;          // live-drive the camera from the experimental curves
	bool m_wasEnabled = false;    // enter/exit edge detection

	// (Scrub state is NOT stored here. The playhead is shared with the camera timeline --
	//  see Movie/TimelinePlayhead.h -- because a private copy is exactly what let the two
	//  panels disagree about where the cursor was.)

	// SUGGESTED visible tick window. C++ only proposes it (key span, or the playhead when there
	// are no keys); the JS owns pan/zoom after it seeds. m_viewEpoch advances when the suggestion
	// should be re-applied (enter/reseed/clear/demo change), which is how a re-seed is requested
	// without ever moving the window in response to the playhead.
	double m_viewT0 = 0.0, m_viewT1 = 1.0;
	bool m_viewInit = false;
	unsigned m_viewEpoch = 0;

	unsigned m_rev = 0;           // bumped on every model edit (JS gates curve rebuild on it)
	std::string m_lastState;
	std::vector<std::string> m_evalQueue;
};

GraphEditorExperimentHud& GraphEditorExperimentHudRef();

// Console command entry: handles "mirv_filmmaker grapheditor ...". argc/args/cmd are forwarded
// from FilmmakerCommand.cpp's dispatcher (args->ArgV(0) == "mirv_filmmaker", ArgV(1) == "grapheditor").
void GraphEditor_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd);

} // namespace Filmmaker

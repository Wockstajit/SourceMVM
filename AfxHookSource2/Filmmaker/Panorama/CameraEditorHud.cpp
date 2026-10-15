#include "CameraEditorHud.h"

#include "InspectorWidth.h"

#include "PanoramaFindPanel.h"
#include "PanoramaPerfProbe.h"

#include "CameraEditorJs.h"
#include "../Cosmetics/CosmeticUiQueries.h"
#include "CameraTimelineHud.h"
#include "ConfigHud.h"
#include "../Movie/HudBlackout.h" // F8: clear every overlay // ConfigHud_Enabled: don't clear the shared viewport-scaler request while Config owns it
#include "GraphEditorExperimentHud.h"
#include "MovieHud.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Movie/CameraPath.h"
#include "../Movie/SceneKeys.h"
#include "../Movie/TimelineDock.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/FollowCamera.h"
#include "../Movie/MovieMode.h" // restore the user's camera mode when the editor closes
#include "../Cosmetics/CosmeticOverrideSystem.h"
#include "../Debug/MvmDebugLog.h"
#include "../Render/RenderCommand.h"
#include "../Render/RenderOutput.h" // RenderOutput_PumpMainThread (folder-picker handover)
#include "../Render/RenderResolution.h" // RenderResolution_PumpMainThread (render.res probe)
#include "../Render/RenderRange.h" // Filmmaker::MvmDebugLog_Active (gates JS preview diagnostics)

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../ClientEntitySystem.h" // AfxGetLocalObserverState (spectator gating for Customize)
#include "../../MirvTime.h"
#include "../../SchemaSystem.h"
#include "WorkspacePreview.h" // the single owner of the scaled-preview viewport
#include "../../ViewportScaler.h" // AfxViewportScaler::GetLastBlit (debug overlay readout only)
#include "../../WrpConsole.h" // advancedfx::Message (debug-overlay-gated [vpscale] diagnostics)

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}

bool PlayingDemoRaw() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo: IsPlayingDemo() flips to false the
			// moment the demo is paused (e.g. SPACE). Gating the editor on it alone made
			// the whole workspace force-exit (OnExit) on every pause, which also dropped
			// the input gating and let scroll/click/ESC leak to the game. Keep paused = up.
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

// BOTH flags also report false transiently during a demo_gototick seek (the documented engine
// blip the cosmetics store and ConfigHud debounce). Un-debounced, a scrub could force-exit the
// whole editor workspace via the !hud branch below. Only a SUSTAINED false is a real close.
bool PlayingDemo() {
	const bool raw = PlayingDemoRaw();
	static int s_falseFrames = 0;
	if (raw) { s_falseFrames = 0; return true; }
	if (s_falseFrames < 0x40000000) ++s_falseFrames;
	return s_falseFrames <= 30;
}

} // namespace

CameraEditorHud& CameraEditorHudRef() {
	static CameraEditorHud s_instance;
	return s_instance;
}

void* CameraEditorHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "CamEditorRoot");
}

bool CameraEditorHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kCameraEditorJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_symDebugPanels = m_bridge.MakeSymbol("debugpanels");
	m_symRenderWorkspace = m_bridge.MakeSymbol("renderworkspace");
	m_symRenderHit = m_bridge.MakeSymbol("renderhit");
	m_symCustomizeOpen = m_bridge.MakeSymbol("customizeopen");
	m_symTextCapture = m_bridge.MakeSymbol("textcapture");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void CameraEditorHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#CamEditorRoot'); if(e) e.DeleteAsync(0); var d=$('#CamEditorDebugRoot'); if(d) d.DeleteAsync(0);"
			" var c=$('#CamEditorConfirmRoot'); if(c) c.DeleteAsync(0); var s=$('#CamEditorSettingsRoot'); if(s) s.DeleteAsync(0);"
			" var r=$('#CamEditorRenderRoot'); if(r) r.DeleteAsync(0);"
			// Customize and its reset confirm are their own roots (CameraEditorCustomizeJs.h), so
			// deleting CamEditorRoot no longer takes them with it.
			" var u=$('#CamEditorCustomizeRoot'); if(u) u.DeleteAsync(0);"
			" var q=$('#CamEditorCustomizeConfirmRoot'); if(q) q.DeleteAsync(0); $.CamEditor=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
	m_customizeOpen = false; // don't leave input permanently swallowed if the editor tears down mid-modal
	m_textCapture = false;   // ...same for a focused text field
}

// Declared in Filmmaker.cpp.
bool GraphEditorExperiment_Enabled();
void GraphEditorExperiment_Set(bool enabled);
const char* CameraEditor_HudViewName();

// One-shot enter: host the timeline, hide the floating movie-director cards, and select a key
// for the inspector. Reusable workspace choices (bottom panel and preview scaling) are preserved
// across closes and restored from the settings store on restart. Free cam is never forced on.
void CameraEditorHud::OnEnter() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);

	tl.SetEditorHosted(true);
	tl.SetVisible(false); // RunFrame applies the remembered bottom mode later this frame.
	tl.SetCursor(true); // start in UI-cursor so the inspector is immediately clickable

	TimelineDock_Claim(DockHost::CameraEditor);
	GraphEditorExperiment_Set(TimelineDock_Tab() == DockTab::Graph);

	// Do NOT hijack the camera on open. Opening the editor only shows the UI -- it must not
	// enable free cam, pause, seek, or jump to a camera. Pre-select a key for the inspector
	// read-out ONLY (no teleport), and only when nothing is already selected, so the current
	// view and camera mode are left exactly as they were. Jumping to / previewing / editing a
	// camera now requires an explicit action (nav arrows, preview, edit -> SelectForEditor).
	if (cp.Count() > 0 && cp.Selected() < 0) cp.SelectIndex(0, /*teleport*/ false);
}

// One-shot exit: restore everything the enter step changed and tear down the chrome.
void CameraEditorHud::OnExit() {
	CameraTimelineHud& tl = CameraTimelineHudRef();
	CameraPath& cp = CameraPathRef();

	// Releasing parks the dock (hosted off, overlays down) for whoever comes next.
	TimelineDock_Release(DockHost::CameraEditor);
	tl.SetEditorHosted(false);
	tl.SetVisible(false);
	GraphEditorExperiment_Set(false);
	// Only kill a running Follow Live preview when the workspace is really gone -- opening
	// Config closes the editor too (they share the right column), but Config's scaled preview
	// keeps showing the follow-driven camera, so the preview must survive that swap.
	if (!ConfigHud_Enabled())
		FollowCameraRef().StopPreview("camera editor closed");
	// If the render modal was open when the editor closes (e.g., switching to Config), close
	// it now so the editor does not restart in render-workspace layout on the next open.
	Render_SetModalOpen(false);
	cp.StopScrub();

	// Hand the camera back. Opening the editor deliberately does not touch the camera, but its
	// ACTIONS do: selecting a marker, scrubbing, previewing a path and the graph drive all turn
	// free cam on. Nothing turned it off again -- 25 call sites enable it, only MovieMode's own
	// mode switch disables it. So a session that touched any of those left the view detached
	// after the editor closed, and because AfxGetSpectatedUserId()
	// resolves the spectated player by MATCHING the camera against a pawn's render eye, a
	// detached camera silently resolves to 'nobody is being spectated'. Everything keyed off that
	// then stops without saying why: Customize Player greys itself out (povReady=false) and the
	// first-person effects (recoil, Smooth View) refuse to fire.
	//
	// Re-applying MovieMode's CURRENT mode is the right restore rather than a blind
	// SetFreeCamEnabled(false): if the user was already flying in free cam before opening the
	// editor, that is the mode they should get back, not first person.
	if (!ConfigHud_Enabled())
		MovieModeRef().SetMode(MovieModeRef().GetMode(), /*keepCursor*/ true);

	// Do NOT restore the floating movie-director card when Config is taking over. Opening Config
	// closes the editor, and RunMainThreadFrame runs ConfigHud BEFORE CameraEditorHud: Config's
	// OnEnter has already captured "hidden" and hidden it again, then this line put it straight
	// back because the EDITOR captured "visible" when it opened. That is the F8 card reappearing
	// the moment Config is opened. Config restores it itself when it closes.
	if (!ConfigHud_Enabled())
		MovieHudRef().SetVisible(m_prevMovieHud);

	// NOTE: cosmetic overrides are keyed by SteamID and persist across editor open/close on
	// purpose now (they follow the player through the whole demo). Closing the editor no longer
	// clears them; use "mirv_filmmaker cosmetics clear" or the Customize modal to remove them.

	// Give up the preview viewport. No Config special-case is needed any more: the owner with
	// the higher precedence wins at Commit time, and an owner that stops submitting is
	// released automatically that same frame.
	WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);

	Teardown();
}

std::string CameraEditorHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);
	const CameraPath::Mode pathMode = cp.GetMode();
	const bool pathPlaying = cp.IsPlaying() || cp.PlaybackPending();
	const bool pathLive = pathPlaying || pathMode == CameraPath::Mode::PreviewPlaying;

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	double camOrigin[3] = { 0,0,0 }, camAngles[3] = { 0,0,0 }, camFov = 0.0;
	CameraBridge_GetCurrentCamera(camOrigin, camAngles, camFov);

	CameraTimelineHud& tl = CameraTimelineHudRef();

	std::ostringstream o;
	o << "{";
	// F8 blackout reports the workspace as not-drawn WITHOUT closing it: m_enabled is the
	// user's choice about what is open, which is a different question from what is on screen.
	// A second F8 therefore brings the editor back exactly as it was, selection included.
	o << "\"enabled\":" << ((m_enabled && !HudBlackout_Active()) ? "true" : "false");
	o << ",\"graphExp\":" << (GraphEditorExperiment_Enabled() ? "true" : "false");
	o << ",\"graphDrive\":" << (GraphEditorExperimentHudRef().Drive() ? "true" : "false");
	o << ",\"bottomMode\":\"" << TimelineDock_TabName() << "\"";
	o << ",\"dockH\":" << TimelineDock_Height();
	// The operator-dragged inspector width, so this panel and the other two lay out to the
	// same column. One reader, one clamp -- see InspectorWidth.h.
	InspectorWidth_AppendJson(o);
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\""; // game-UI visibility picker
	o << ",\"debug\":" << (m_debugOverlay ? "true" : "false");
	o << ",\"uiEditorInspector\":\""
		<< FilmmakerSettings_UiString("editor.inspector", "path") << "\"";
	o << ",\"sceneKeys\":" << SceneKeys_StateJson();
	// Every stored UI key, so the inspector restores each tab's selected rail card and each
	// card's sub-page generically (editor.<tab>.sel / editor.<card>.page).
	FilmmakerSettings_AppendUiJson(o);
	if (m_debugOverlay) {
		// Render-layer numbers for the viewport debug overlay: the ACTUAL world-blit rect (px) +
		// the backbuffer/render-target size, so JS can compare them against the Panorama-side
		// preview rect and prove the custom viewport matches the game viewport 1:1.
		int bbW = 0, bbH = 0; float vx = 0, vy = 0, vw = 0, vh = 0;
		const bool blitRan = AfxViewportScaler::GetLastBlit(bbW, bbH, vx, vy, vw, vh);
		o << ",\"dbg\":{\"blitRan\":" << (blitRan ? "true" : "false")
			<< ",\"bbW\":" << bbW << ",\"bbH\":" << bbH
			<< ",\"vx\":" << r2(vx) << ",\"vy\":" << r2(vy) << ",\"vw\":" << r2(vw) << ",\"vh\":" << r2(vh)
			<< ",\"scaleReq\":" << (m_scaleEnabled ? "true" : "false")
			<< ",\"previewValid\":" << (m_previewValid ? "true" : "false");
		std::string panelsJson = "[]";
		if (m_root && m_symDebugPanels >= 0) {
			std::string rawPanels = m_bridge.GetAttributeString(m_root, m_symDebugPanels, "[]");
			if (!rawPanels.empty() && rawPanels[0] == '[')
				panelsJson = rawPanels;
		}
		o << ",\"panels\":" << panelsJson << "}";
	}
	o << ",\"cursor\":" << (tl.Cursor() ? "true" : "false");
	o << ",\"flashToken\":" << m_flashToken;
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	bool paused = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			paused = pDemo->IsDemoPaused();
	}
	o << ",\"paused\":" << (paused ? "true" : "false");
	// So the Customize preview's JS-side diagnostics (previewLog, see CameraEditorCustomizeJs.h) only
	// fire while `mvm_debug start` is active -- reuses the existing session log instead of adding a
	// second debug toggle; running `mvm_debug start` while the modal is open now also captures rich
	// preview state (selection, model paths, item ids, panel geometry) via the tier0 console capture.
	o << ",\"mvmDebug\":" << (Filmmaker::MvmDebugLog_Active() ? "true" : "false");
	// Live OS-cursor pipe (reused from the experimental graph editor's drag mechanism -- Panorama has
	// no mouse-move event in this in-game HUD context, so CameraBridge_GetUiCursor polls GetCursorPos
	// each frame + edge-captures button state from the WndProc; see main.cpp g_UiCursorProbe). Lets the
	// Customize preview implement click-and-drag panning entirely in JS without any new C++ plumbing.
	{
		int cmx = 0, cmy = 0; bool clmb = false, crmb = false, cshift = false; unsigned cseq = 0;
		CameraBridge_GetUiCursor(cmx, cmy, clmb, crmb, cshift, cseq);
		o << ",\"mx\":" << cmx << ",\"my\":" << cmy << ",\"lmb\":" << (clmb ? "true" : "false") << ",\"mseq\":" << cseq;
		o << ",\"shift\":" << (cshift ? "true" : "false");
	}
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"interp\":\"" << cp.InterpName() << "\"";
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"pathMode\":\"" << cp.ModeName() << "\"";
	o << ",\"pathPlaying\":" << (pathPlaying ? "true" : "false");
	o << ",\"pathLive\":" << (pathLive ? "true" : "false");
	o << ",\"timelineView\":\"" << (tl.View() == 1 ? "curve" : "timeline") << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"freeCam\":" << (CameraBridge_GetFreeCamEnabled() ? "true" : "false");
	o << ",\"freeCamSpeed\":" << r2(CameraBridge_GetFreeCamSpeed());
	// Spectator state for the "Customize" gating + modal: obsMode 2 (in-eye/first) or 3
	// (chase/third) means we're watching a player; 4 (roaming) is freecam. obsTarget is the
	// spectated entity index (-1 if none) so the modal can identify whose loadout to edit.
	{
		int obsTargetIndex = -1;
		uint8_t obsMode = AfxGetLocalObserverState(&obsTargetIndex);
		// AfxGetLocalObserverState's target reads -1 in POV/GOTV demos (so customizeTarget would be
		// null and the modal would fall back to a fuzzy nearest-player guess). Use the robust eye-match
		// resolver so customizeTarget is deterministically the player actually being viewed. Returns -1
		// in free cam (no single spectated player) -> customizeTarget null, fallback as before.
		// Held, not raw: the raw eye match drops out for single frames, and on those frames the modal
		// fell back to another player -- the Customize target (and its [mvmpickup] debug line)
		// flipped between two players every frame and an open modal kept discarding unstaged picks.
		const int observerTarget = obsTargetIndex;
		const int rawSpectated = AfxGetSpectatedPawnIndex();
		int spectated = AfxGetSpectatedPawnIndexHeld();
		if (spectated >= 0) obsTargetIndex = spectated;
		if (Filmmaker::MvmDebugLog_Active()) {
			static int s_lastPublished = -2, s_lastRaw = -2;
			static unsigned s_frames = 0, s_rawChanges = 0, s_pubChanges = 0;
			static ULONGLONG s_windowMs = 0;
			++s_frames;
			if (rawSpectated != s_lastRaw) { ++s_rawChanges; s_lastRaw = rawSpectated; }
			if (obsTargetIndex != s_lastPublished) {
				++s_pubChanges;
				Filmmaker::MvmDebugLog_Linef("ui.customize.target", "published=%d (was %d) raw=%d held=%d observer=%d mode=%u",
					obsTargetIndex, s_lastPublished, rawSpectated, spectated, observerTarget, (unsigned)obsMode);
				s_lastPublished = obsTargetIndex;
			}
			const ULONGLONG now = GetTickCount64();
			if (now - s_windowMs >= 2000) {
				Filmmaker::MvmDebugLog_LinefAlways("ui.customize.target.window", "frames=%u rawChanges=%u publishedChanges=%u",
					s_frames, s_rawChanges, s_pubChanges);
				s_windowMs = now; s_frames = s_rawChanges = s_pubChanges = 0;
			}
		}
		o << ",\"obsMode\":" << (int)obsMode;
		o << ",\"obsTarget\":" << obsTargetIndex;
		o << ",\"customizeTarget\":" << BuildCustomizeTargetJson(obsTargetIndex);
		o << ",\"customizePlayers\":" << BuildCustomizePlayersJson();
	}
	o << ",\"cam\":{\"x\":" << r2(camOrigin[0]) << ",\"y\":" << r2(camOrigin[1]) << ",\"z\":" << r2(camOrigin[2])
		<< ",\"pitch\":" << r2(camAngles[0]) << ",\"yaw\":" << r2(camAngles[1]) << ",\"roll\":" << r2(camAngles[2])
		<< ",\"fov\":" << r2(camFov) << "}";
	o << ",\"follow\":" << FollowCameraRef().BuildStateJson();
	if (selValid) {
		const CamMarker& m = mk[sel];
		o << ",\"sel\":{\"tick\":" << m.tick
			<< ",\"x\":" << r2(m.x) << ",\"y\":" << r2(m.y) << ",\"z\":" << r2(m.z)
			<< ",\"pitch\":" << r2(m.pitch) << ",\"yaw\":" << r2(m.yaw) << ",\"roll\":" << r2(m.roll)
			<< ",\"fov\":" << r2(m.fov) << ",\"ease\":" << (int)m.ease << ",\"speedMul\":" << r2(m.speedMul)
			<< ",\"isLast\":" << ((sel == n - 1) ? "true" : "false") << "}";
	} else {
		o << ",\"sel\":null";
	}
	// The render block is owned by the render module (RenderCommand.cpp): this file already
	// carries the camera-path, follow, cosmetics and debug blocks, and does not need to know the
	// render model's field set as well.
	o << ",\"render\":" << Render_BuildStateJson();
	o << "}";
	return o.str();
}

void CameraEditorHud::RunFrame() {
	m_bridge.Init();

	// Hand over anything the (worker-thread) output folder picker finished choosing. Must run on
	// the main thread before the state JSON is built, or the new folder shows up a frame late.
	RenderOutput_PumpMainThread();
	RenderResolution_PumpMainThread();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (MvmDebugLog_Active()) {
		static int s_lastEnabled = -1;
		static int s_lastConfig = -1;
		static int s_lastRenderOpen = -1;
		static int s_lastBuilt = -1;
		const int enabled = m_enabled ? 1 : 0;
		const int config = ConfigHud_Enabled() ? 1 : 0;
		const int renderOpen = Render_IsModalOpen() ? 1 : 0;
		const int built = m_built ? 1 : 0;
		if (enabled != s_lastEnabled || config != s_lastConfig
			|| renderOpen != s_lastRenderOpen || built != s_lastBuilt) {
			MvmDebugLog_Linef("ui.render.lifecycle",
				"editor=%d config=%d open=%d built=%d root=%p hud=%p",
				enabled, config, renderOpen, built, m_root, hud);
			s_lastEnabled = enabled;
			s_lastConfig = config;
			s_lastRenderOpen = renderOpen;
			s_lastBuilt = built;
		}
	}

	// Demo not playing (or HUD gone): force-exit editor mode cleanly so we never leave
	// the gameplay HUD hidden or the timeline orphaned.
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		else WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
		m_openCustomizeRequested = false;
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	// Latch the previous frame's flag before any early-out can skip it.
	const bool debugWasOn = m_debugOverlayWasOn;
	m_debugOverlayWasOn = m_debugOverlay;

	if (!m_enabled && !m_debugOverlay) {
		// The debug panel hides ITSELF from the pushed state (CameraEditorDebugJs::updateDebug:
		// `if (!st.debug) { dbgPanel.visible = false; return; }`), and that only runs when
		// render() runs. Returning the instant the flag clears means the JS never sees
		// debug:false, so a panel that was showing while the editor is CLOSED stays orphaned
		// on screen (green readout, top-left) until a HUD rebuild -- `editor debug off` reports
		// "off" and visibly does nothing. Push one final state so it can hide itself, then
		// resume the cheap early-out on every later frame.
		if (debugWasOn && m_built) {
			m_root = FindRoot();
			if (m_root) {
				std::string state = BuildStateJson();
				m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
				m_lastState = state;
				m_bridge.RunScriptCached("$.CamEditor && $.CamEditor.render();", "mvm/cameditor.render");
			} else {
				m_built = false;
			}
		}
		// ConfigHud (which ran earlier this frame) may own the scaler request now -- leave it.
		if (!ConfigHud_Enabled())
			WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
		return;
	}

	if (!m_enabled && m_debugOverlay) {
		const bool configOwnsScaler = ConfigHud_Enabled();
		if (!BuildIfNeeded()) {
			WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
			return;
		}
		m_root = FindRoot();
		if (!m_root) {
			m_built = false;
			WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
			return;
		}
		std::string state = BuildStateJson();
		if (state != m_lastState) {
			m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
			m_lastState = state;
		}
		m_bridge.RunScriptCached("$.CamEditor && $.CamEditor.render();", "mvm/cameditor.render");
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
		return;
	}

	// While enabled, re-assert hosting every frame (cheap) so a stray timeline close or HUD
	// recreation can't leave the workspace half-torn-down. Native mode leaves CS2's own demo
	// timeline visible; custom camera timeline / graph are explicit bottom overlays.
	TimelineDock_Claim(DockHost::CameraEditor);
	TimelineDock_Apply(DockHost::CameraEditor);

	if (!BuildIfNeeded()) {
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
		return;
	}

	// Live REPL (editor eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) {
		m_built = false;
		WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
		return;
	}

	std::string state;
	{ PanoramaPerf_Scope _p("cameditor.state"); state = BuildStateJson(); }
	if (state != m_lastState) {
		PanoramaPerf_Scope _p("cameditor.push");
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Always render while enabled so the chrome re-asserts its layout each frame. render()
	// (re)publishes the "previewrect" attribute -- the normalised preview-rect fractions --
	// as the single source of truth shared with the D3D blit.
	{ PanoramaPerf_Scope _p("cameditor.js"); m_bridge.RunScriptCached("$.CamEditor && $.CamEditor.render();", "mvm/cameditor.render"); }
	if (MvmDebugLog_Active()) {
		static std::string s_lastProjection;
		const std::string projected = m_bridge.GetAttributeString(
			m_root, m_symRenderWorkspace, "missing");
		const std::string hitGeometry = m_bridge.GetAttributeString(
			m_root, m_symRenderHit, "missing");
		const std::string snapshot =
			std::string(Render_IsModalOpen() ? "1:" : "0:") + projected + ":" + hitGeometry;
		if (snapshot != s_lastProjection) {
			MvmDebugLog_Linef("ui.render.projection",
				"requested=%d projected=%s hit='%s' root=%p",
				Render_IsModalOpen() ? 1 : 0, projected.c_str(), hitGeometry.c_str(), m_root);
			s_lastProjection = snapshot;
		}
	}
	if (m_openCustomizeRequested) {
		m_bridge.RunScript("$.CamEditor && $.CamEditor.openCustomize();");
		m_openCustomizeRequested = false;
	}

	// Feed any wheel/typed-character input captured off the WndProc thread into the modal JS (only
	// does work while the modal is open + something is queued). Runs after render() so the panel it
	// scrolls/filters is laid out for this frame.
	DispatchCustomizeInput();
	// Same contract for the inspector column's own wheel, which is live whenever the editor is
	// (unlike the modal's, which only exists while the modal is open).
	DispatchWheel();

	// TRUE scaled preview: forward the rect render() just published to the viewport scaler.
	// The blit only actually runs engine-side when not recording (full-screen capture wins).
	UpdateScaleRequest();
	UpdateCustomizeModalState();
	// While the Customize modal is up, keep the flashbang whiteout suppressed so it never washes
	// out the modal UI or the 3D player preview (the flash post-effect covers Panorama too).
	if (m_customizeOpen)
		CustomizeSuppressFlashTick();
}

// Reads the "previewrect" fractions the editor JS published this frame and forwards them to
// the render-layer scaler. When scaling is off (or the rect is missing/degenerate) the request
// is cleared, so the preview falls back to the Panorama crop.
void CameraEditorHud::UpdateScaleRequest() {
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	std::string pr;
	bool parsed = false, valid = false;

	// Read the published rect whenever the editor is open (cheap), independent of the scale
	// toggle, so diagnostics show the JS->C++ round-trip status even with scaling off.
	if (m_root) {
		pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
		std::istringstream is(pr);
		parsed = (bool)(is >> x0 >> y0 >> x1 >> y1);
		valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
			&& x0 >= 0 && y0 >= 0 && x1 <= 1.0001f && y1 <= 1.0001f;
	}

	// Cache the rect so the timeline HUD can scale the native game HUD into the same region.
	m_previewValid = valid;
	if (valid) { m_previewX0 = x0; m_previewY0 = y0; m_previewX1 = x1; m_previewY1 = y1; }

	// A workspace preview is ALWAYS scaled. It used to be gated on a persisted per-user toggle,
	// which is how the Camera Editor ended up showing an unscaled CROP -- more of the world than
	// the frame it claims to preview, with the native HUD outside the box -- while the Config
	// panel on the same screen (which never had a toggle) scaled correctly. Owning a valid
	// preview rect is now the whole condition, matching Config exactly.
	m_scaleEffective = valid;
	const bool active = valid;

	// Diagnostics: only when the decision inputs CHANGE (no per-frame spam, silent in normal use).
	static int s_prev = -1;
	int now = (valid ? 2 : 0) | (Render_IsModalOpen() ? 4 : 0);
	if (now != s_prev) {
		s_prev = now;
		advancedfx::Message("[vpscale] req: raw='%s' parsed=%d valid=%d render=%d\n",
			pr.c_str(), (int)parsed, (int)valid, Render_IsModalOpen() ? 1 : 0);
	}

	// ---- INVARIANT: when the editor is active but the preview rect has been missing for
	//     3+ consecutive frames, the layout is stuck/broken — warn once per episode.
	static int s_emptyFrameCount = 0;
	static bool s_emptyWarned = false;
	if (m_scaleEffective && m_wasEnabled) {
		if (!valid || (parsed && x1 - x0 < 0.001f)) {
			s_emptyFrameCount++;
			if (s_emptyFrameCount == 3 && !s_emptyWarned) {
				advancedfx::Warning("[vpscale] INVARIANT: preview rect missing for %d consecutive frames "
					"while scale is ON. Layout may be stuck. raw='%s'\n",
					s_emptyFrameCount, pr.c_str());
				s_emptyWarned = true;
			}
		} else {
			s_emptyFrameCount = 0;
			s_emptyWarned = false;
		}
	} else {
		s_emptyFrameCount = 0;
		s_emptyWarned = false;
	}

	// Submit rather than command: WorkspacePreview decides between the workspaces and issues the
	// one SetRequest per frame (see WorkspacePreview.h for why this used to be a race).
	if (active) WorkspacePreviewRef().Submit(WorkspacePreview::Owner::CameraEditor, x0, y0, x1, y1);
	else WorkspacePreviewRef().Release(WorkspacePreview::Owner::CameraEditor);
}

void CameraEditorHud::PushCustomizeWheel(int notches, int x, int y) {
	if (notches == 0) return;
	std::lock_guard<std::mutex> lk(m_custInputMutex);
	if (m_custWheelEvents.size() < 128)
		m_custWheelEvents.push_back({ notches, x, y });
}

void CameraEditorHud::PushCustomizeChar(unsigned charCode) {
	std::lock_guard<std::mutex> lk(m_custInputMutex);
	// Guard against a stuck/huge backlog if the modal ever stops draining (it drains every frame).
	if (m_custChars.size() < 512) m_custChars.push_back(charCode);
}

// Main thread. Drain the wheel + typed-character queue captured off the WndProc thread and hand it
// to the modal JS. Batches per frame: one custWheel(net) call and one custChars('c,c,...') call.
// WndProc/input thread. Bounded so a stuck queue can never turn into an enormous jump.
void CameraEditor_PushWheel(int notches) { CameraEditorHudRef().PushWheel(notches); }

void CameraEditorHud::PushWheel(int notches) {
	std::lock_guard<std::mutex> lk(m_wheelMutex);
	int next = m_pendingWheel + notches;
	if (next > 32) next = 32;
	if (next < -32) next = -32;
	m_pendingWheel = next;
}

// Main thread. Hands the accumulated notches to the panel's JS, which scrolls its own column
// (the inspector is a manual transform-scroller -- native overflow:scroll cannot work here,
// because the wheel never reaches Panorama in this context).
void CameraEditorHud::DispatchWheel() {
	int notches = 0;
	{
		std::lock_guard<std::mutex> lk(m_wheelMutex);
		notches = m_pendingWheel;
		m_pendingWheel = 0;
	}
	if (0 == notches || !m_root) return;
	std::ostringstream o;
	o << "$.CamEditor && $.CamEditor.wheel && $.CamEditor.wheel(" << notches << ");";
	m_bridge.RunScript(o.str().c_str());
}

void CameraEditorHud::DispatchCustomizeInput() {
	std::vector<CustomizeWheelEvent> wheels;
	std::vector<unsigned> chars;
	{
		std::lock_guard<std::mutex> lk(m_custInputMutex);
		wheels.swap(m_custWheelEvents);
		chars.swap(m_custChars);
	}
	if (!m_root) return;
	for (const CustomizeWheelEvent& ev : wheels) {
		std::ostringstream o;
		o << "$.CamEditor && $.CamEditor.custWheel(" << ev.notches << "," << ev.x << "," << ev.y << ");";
		m_bridge.RunScript(o.str().c_str());
	}
	if (!chars.empty()) {
		std::ostringstream o;
		o << "$.CamEditor && $.CamEditor.custChars('";
		for (size_t i = 0; i < chars.size(); ++i) { if (i) o << ','; o << chars[i]; }
		o << "');";
		m_bridge.RunScript(o.str().c_str());
	}
}

// Reads the "customizeopen" flag the Customize modal's openCustomize()/closeCustomize()/render()
// publish (see CameraEditorCustomizeJs.h) so the input layer (MovieMode, GetSuspendMirvInput) can
// treat the modal as an exclusive surface. m_root is only non-null while the workspace is built,
// so this naturally reads false (no attribute to read) whenever the editor itself is closed.
void CameraEditorHud::UpdateCustomizeModalState() {
	m_customizeOpen = m_root && (m_bridge.GetAttributeString(m_root, m_symCustomizeOpen, "0") == "1");
	// Same contract for the render workspace's typed fields. Cleared automatically whenever the
	// workspace tears down, because m_root goes null and there is no attribute left to read.
	m_textCapture = m_root && (m_bridge.GetAttributeString(m_root, m_symTextCapture, "0") == "1");
}

} // namespace Filmmaker

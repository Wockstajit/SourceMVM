#include "CameraTimelineHud.h"

#include "InspectorWidth.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Active / _LinefAlways (teardown probe)
#include "../Movie/PlaybackLoop.h"
#include "../Movie/ScrubFrameCache.h" // the transport's Cache toggle
#include "../Movie/HudBlackout.h" // F8: clear every overlay

#include "PanoramaFindPanel.h"
#include "PanoramaPerfProbe.h"

#include "CameraTimelineJs.h"
#include "../Movie/CamPathShake.h" // per-keyframe GTA V shake: state array + the style list push
#include "../Movie/CameraBridge.h" // CameraBridge_GetUiCursor (raw drag input for the speed scrubber)
#include "../Movie/CameraPath.h"
#include "../Movie/TimelinePlayhead.h" // the ONE cursor shared with the graph editor
#include "../Movie/RageShake.h"    // RageShake_StyleName/Count for the one-time style list
#include "../Movie/DemoTimescale.h" // one demo speed, shared by all three scrubbers
#include "../Movie/SceneKeys.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Render/RenderCommand.h"
#include "../Render/RenderRange.h"
#include "../Render/RenderSessionDriver.h" // WantsChromeHidden (capture chrome gate)

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../MirvTime.h"
#include "../../RenderSystemDX11Hooks.h" // AfxStreams_IsRcording (CSS/D3D scale paths must match)

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstdio>
#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) { double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0; }

bool PlayingDemoRaw() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo: IsPlayingDemo() flips to false the
			// moment the demo is paused (e.g. SPACE), so gating the HUD on it alone tore
			// the timeline down on every pause. Treat paused as active so the UI stays up.
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

// BOTH flags additionally report false transiently during a demo_gototick seek (documented
// engine blip; the cosmetics store debounces the same thing). Here the blip was expensive: a
// single false frame ran Teardown(), which DELETES this panel's JS while the native demo bar
// is still DOCKED (narrowed to sit left of a workspace inspector). Nothing was left alive to
// undock it, so the bar stayed short until the user opened and closed a workspace again --
// the "timeline is smaller than the whole length, fixed itself after going in and out" report
// (2026-08-28). Only a SUSTAINED false is a real demo close.
bool PlayingDemo() {
	const bool raw = PlayingDemoRaw();
	static int s_falseFrames = 0;
	if (raw) { s_falseFrames = 0; return true; }
	if (s_falseFrames < 0x40000000) ++s_falseFrames;
	const bool alive = s_falseFrames <= 30;
	if (!alive && s_falseFrames == 31 && MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("camtl.teardown",
			"demo reported closed for %d consecutive frames -> tearing down the timeline panel",
			s_falseFrames);
	return alive;
}

} // namespace

CameraTimelineHud& CameraTimelineHudRef() {
	static CameraTimelineHud s_instance;
	return s_instance;
}

void* CameraTimelineHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "CamTimelineRoot");
}

bool CameraTimelineHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kCameraTimelineJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symMenuOpen = m_bridge.MakeSymbol("menuopen");
	m_symCamMode = m_bridge.MakeSymbol("cammode");
	m_symBarRect = m_bridge.MakeSymbol("barrect");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	if (m_built)
		PushShakeStyles(); // one-shot; the HUD rebuilds (and so re-pushes) if CS2 recreates it
	return m_built;
}

void CameraTimelineHud::Teardown() {
	m_nativeCameraMode = -1;
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		// Hand CS2's own demo bar back FIRST. Our dock writes width/horizontalAlign/background
		// onto native panels; deleting our root does not revert them, so skipping this orphans
		// the bar at its docked (short) width with nothing alive to restore it.
		m_bridge.RunScript(
			"(function(){try{if($.CamTimeline&&$.CamTimeline.restoreNative)$.CamTimeline.restoreNative();}catch(e){}"
			"var e2=$('#CamTimelineRoot'); if(e2) e2.DeleteAsync(0);"
			" var r=$('#CamTimelineRangeRoot'); if(r) r.DeleteAsync(0); $.CamTimeline=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
	m_barValid.store(false);
	m_barRectText.clear();
}

void CameraTimelineHud::ReadBarRect() {
	const std::string text = m_root ? m_bridge.GetAttributeString(m_root, m_symBarRect, "") : std::string();
	if (text == m_barRectText) return;
	m_barRectText = text;
	int r[4] = {};
	const bool ok = 4 == sscanf_s(text.c_str(), "%d %d %d %d", &r[0], &r[1], &r[2], &r[3]) && r[2] > r[0] && r[3] > r[1];
	m_barValid.store(false);
	for (int i = 0; i < 4; ++i) m_bar[i].store(r[i]);
	m_barValid.store(ok);
}

// Declared in Filmmaker.cpp; queried so the timeline yields the curve zone to the experimental
// graph editor (forces compact scrub view + hides its own curve-editor toggle) while it is on.
bool GraphEditorExperiment_Enabled();
const char* CameraEditor_HudViewName(); // declared in Filmmaker.cpp; game-HUD visibility while hosted
bool CameraEditor_ScaledHud(float& x0, float& y0, float& x1, float& y1); // scaled-preview rect + active
bool ConfigHud_Enabled(); // declared in ConfigHud.cpp; the Game UI picker also applies while Config is open
bool CameraEditor_Enabled(); // declared in Filmmaker.cpp; the editor's INTENT flag (see BuildStateJson)
bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1); // Config panel's scaled-preview rect

std::string CameraTimelineHud::BuildStateJson() {
	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& mk = cp.Markers();
	const int n = (int)mk.size();
	// THE CAMERA TIMELINE SPANS THE CAMERA PATH. Nothing else.
	//
	// This used to publish the UNION of the marker range and the render range, which collapses the
	// thing you are actually editing the moment those two are far apart. Measured live: a 4-key
	// path over ticks 17660-17857 (197 ticks) with the render range at 2800-2895 produced a
	// 15,057-tick bar -- the whole path occupied 1.3% of it and every diamond was crushed against
	// the right edge, unclickable and unreadable. Choosing "Full Demo" made it worse again.
	//
	// The render range is a different question ("what will I export?"), it has its own overlay,
	// and it belongs on the REGULAR timeline where the span is the demo. Here the span is the
	// shot. The render range is only used as a FALLBACK, for the case where there is no path yet
	// and the bar would otherwise be degenerate.
	int tickMin = 0;
	int tickMax = 0;
	if (n > 0) {
		tickMin = mk.front().tick;
		tickMax = mk.back().tick;
	} else {
		const RenderRange& rng = Render_GetRange();
		if (rng.Valid()) {
			tickMin = rng.startTick;
			tickMax = rng.endTickExclusive;
		} else {
			int demoStart = -1, demoEnd = -1;
			Render_GetDemoBounds(demoStart, demoEnd);
			if (demoStart >= 0 && demoEnd > demoStart) { tickMin = demoStart; tickMax = demoEnd; }
		}
	}

	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);
	const int sel = cp.Selected();
	const bool selValid = (sel >= 0 && sel < n);

	std::ostringstream o;
	o << "{";
	o << "\"open\":" << (m_visible ? "true" : "false");
	// HOSTED IS THE WORKSPACE'S INTENT, NOT LAST FRAME'S HOSTING FLAG.
	//
	// This panel is pumped BEFORE ConfigHud and CameraEditorHud (Filmmaker::RunMainThreadFrame),
	// but m_editorHosted is written by CameraEditorHud::OnEnter -- i.e. later in the SAME frame the
	// editor is switched on. Publishing the raw flag therefore made the Config -> Camera Editor swap
	// asymmetric with the reverse: the command closes Config immediately, so for exactly one frame
	// this panel saw hosted=false AND configOpen=false and took the "no workspace" path -- native bar
	// un-docked to full width, gameplay HUD un-scaled and re-shown, our bar redrawn as the floating
	// standalone card -- and the next frame put all of it back. That is the whole-screen flicker on
	// the way IN. Going the other way never flickered because OnExit is equally late, so hosted was
	// still true while configOpen had already flipped on, and the hosted branch held the dock.
	//
	// The editor's enabled flag is set by the command itself, so it is already correct here. OR-ing
	// it in makes the swap atomic from this panel's point of view in BOTH directions; on the way out
	// m_editorHosted carries the extra frame, which is what already kept the close smooth.
	const bool editorIntent = CameraEditor_Enabled();
	if (editorIntent && !m_editorHosted && MvmDebugLog_Active()) {
		// The one-frame window itself, so the fix is visible in a log rather than only in the
		// code: every line here is a frame that WOULD have flickered.
		static int s_saved = 0;
		MvmDebugLog_LinefAlways("camtl.hosted",
			"editor enabled but not yet hosting (pump order) -- holding the dock, save #%d", ++s_saved);
	}
	o << ",\"hosted\":" << ((m_editorHosted || editorIntent) ? "true" : "false");
	o << ",\"renderOpen\":" << (Render_IsModalOpen() ? "true" : "false");
	o << ",\"configOpen\":" << (ConfigHud_Enabled() ? "true" : "false"); // Config panel wants the hudView applied too
	// F8 blackout: the JS hides its own root, the native demo bar and the whole gameplay HUD
	// off this one flag, so the clean screen is decided in one place.
	o << ",\"blackout\":" << (HudBlackout_Active() ? "true" : "false");
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\"";
	// The operator-dragged inspector width, so this panel and the other two lay out to the
	// same column. One reader, one clamp -- see InspectorWidth.h.
	InspectorWidth_AppendJson(o); // game-HUD visibility while hosted
	{
		// Scaled-preview rect + active flag, so the JS scales the native game HUD into the same
		// rect the world blit uses (HUD then lines up with the shrunk world preview). The
		// Config panel publishes its own rect through the identical contract; the two panels
		// are mutually exclusive so at most one of these is active.
		float sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
		// While recording, the D3D world blit is skipped (AfxViewportScaler refuses while the
		// recorder runs) -- the CSS half must follow, or captured frames get a native HUD scaled
		// into a preview box that no longer exists (the two paths must match 1:1).
		bool hudScale = !AfxStreams_IsRcording() && CameraEditor_ScaledHud(sx0, sy0, sx1, sy1);
		if (!hudScale && !AfxStreams_IsRcording())
			hudScale = ConfigHud_ScaledHud(sx0, sy0, sx1, sy1);
		o << ",\"hudScale\":" << (hudScale ? "true" : "false");
		o << ",\"previewRect\":[" << sx0 << "," << sy0 << "," << sx1 << "," << sy1 << "]";
	}
	o << ",\"cursor\":" << (Cursor() ? "true" : "false");
	o << ",\"cursorForced\":" << (CursorForced() ? "true" : "false");
	// The native regular-timeline range overlay lives in this same Panorama script even when the
	// custom camera bar is hidden. It therefore needs the cursor pipe while playback Loop (or the
	// render range workspace) exposes draggable flags. Previously this was gated on m_visible,
	// making mx/my/lmb absent in every native-bar context: editor Regular Timeline, Config, and the
	// ordinary in-game demo bar. Camera flags worked only because the camera bar sets m_visible.
	const bool publishCursor = m_visible || PlaybackLoop_Enabled() || Render_IsModalOpen();
	if (publishCursor) {
		// Raw OS cursor, in CLIENT device px (the space GetPositionWithinWindow reports in). This
		// HUD context delivers no mouse-move event, so the speed scrubber in our transport row --
		// the same control the native demo bar carries -- has nothing else to drag from.
		//
		// The state string is only pushed when it changes. Publishing pointer motion costs that
		// per-frame push only while a visible draggable surface actually depends on it.
		int cmx = 0, cmy = 0; bool clmb = false, crmb = false, cshift = false; unsigned cseq = 0;
		CameraBridge_GetUiCursor(cmx, cmy, clmb, crmb, cshift, cseq);
		o << ",\"mx\":" << cmx << ",\"my\":" << cmy << ",\"lmb\":" << (clmb ? "true" : "false");
		// The engine's demo speed, so this dock's scrubber agrees with the native demo bar's and
		// the graph editor's. Panorama's own GetDemoControllerState() is not available on this
		// context panel -- see Movie/DemoTimescale.h.
		// NOT r2(): the scrubber is continuous, so 2 decimals would round 0.4137x to 0.41x and the
		// handle would visibly snap somewhere else the moment you let go of your own drag.
		o << ",\"timescale\":" << DemoTimescale_Get();
	}
	o << ",\"view\":\"timeline\"";
	o << ",\"uiEditorInspector\":\""
		<< FilmmakerSettings_UiString("editor.inspector", "path") << "\"";
	o << ",\"sceneKeys\":" << SceneKeys_MarkerStateJson();
	o << ",\"graphExp\":" << (GraphEditorExperiment_Enabled() ? "true" : "false");
	o << ",\"count\":" << n;
	o << ",\"selected\":" << sel;
	o << ",\"segment\":" << cp.PlaySegment();
	// EFFECTIVE, not selected: a 2-3 marker path set to Smooth actually evaluates Linear, and a
	// button reading "Smooth" over linear motion is the bug, not the fix. `interpDegraded` lets
	// the UI say why instead of just looking broken.
	o << ",\"interp\":\"" << cp.EffectiveInterpName() << "\"";
	o << ",\"interpDegraded\":" << (cp.InterpDegraded() ? "true" : "false");
	o << ",\"scrubCache\":" << (ScrubFrameCache_Enabled() ? "true" : "false");
	o << ",\"timing\":\"" << cp.TimingName() << "\"";
	o << ",\"speedMode\":\"" << cp.SpeedModeName() << "\"";
	o << ",\"constSpeed\":" << r2(cp.ConstSpeed());
	o << ",\"playing\":" << ((cp.IsPlaying() || cp.PlaybackPending()) ? "true" : "false");
	// Chrome-out-of-capture: an active render job hides the timeline chrome exactly like the
	// camera-path preview's Tab-hide does, riding the same JS gate.
	o << ",\"previewHudHidden\":" << ((cp.PreviewHudHidden() || RenderSessionDriver_WantsChromeHidden()) ? "true" : "false");
	// The SHARED cursor, not CameraPath's private scrub flag: the graph editor's ruler writes the
	// same one, so dragging there moves this bar's readout and slider too (Movie/TimelinePlayhead.h).
	o << ",\"scrubbing\":" << (TimelinePlayhead_Scrubbing() ? "true" : "false");
	o << ",\"tick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"tickMin\":" << tickMin;
	o << ",\"tickMax\":" << tickMax;
	{
		// Push the RESOLVED span (what the chosen mode means right now) so the timeline
		// overlays highlight the real region: FullDemo/PathBounds/Segment resolve in C++,
		// CurrentRound is pushed back by the JS after it reads the native round markers,
		// Custom is already concrete. Falls back to the raw stored ticks when unresolved.
		RenderRange resolved;
		const bool resOk = Render_ResolveRangeForDisplay(resolved) && resolved.Valid();
		o << ",\"renderRangeMode\":" << (int)Render_GetRange().mode;
		o << ",\"renderInTick\":" << resolved.startTick;
		o << ",\"renderOutTickExclusive\":" << resolved.endTickExclusive;
		o << ",\"renderRangeActive\":" << (resOk ? "true" : "false");
		int demoStart = -1, demoEnd = -1;
		Render_GetDemoBounds(demoStart, demoEnd);
		o << ",\"demoStartTick\":" << demoStart;
		o << ",\"demoEndTick\":" << demoEnd;
	}
	// Loop region. Emitted unconditionally (not inside the render-workspace block above): the
	// loop is a REVIEW control that works whether or not the Render workspace is open, and it is
	// deliberately not the render range -- see Movie/PlaybackLoop.h.
	PlaybackLoop_AppendStateJson(o);
	o << ",\"scrubTick\":" << (long long)(TimelinePlayhead_Tick() + 0.5);
	o << ",\"selEase\":" << (selValid ? (int)mk[sel].ease : 0);
	o << ",\"selSpeedMul\":" << (selValid ? r2(mk[sel].speedMul) : 1.0);
	o << ",\"selIsLast\":" << ((selValid && sel == n - 1) ? "true" : "false");
	o << ",\"markers\":[";
	for (int i = 0; i < n; ++i) {
		if (i) o << ",";
		const CamMarker& m = mk[i];
		o << "{\"tick\":" << m.tick
			<< ",\"x\":" << r2(m.x) << ",\"y\":" << r2(m.y) << ",\"z\":" << r2(m.z)
			<< ",\"pitch\":" << r2(m.pitch) << ",\"yaw\":" << r2(m.yaw) << ",\"roll\":" << r2(m.roll)
			<< ",\"fov\":" << r2(m.fov) << ",\"ease\":" << (int)m.ease << ",\"speedMul\":" << r2(m.speedMul) << "}";
	}
	o << "]";
	// Per-keyframe GTA V shake. Only keys that HAVE one are listed, so a path with no shakes
	// costs 13 bytes here and the state string keeps diffing to "unchanged" as it did before.
	o << ",\"shakes\":" << CamPathShake_StateJsonArray();
	o << "}";
	return o.str();
}

// The shake style list, pushed ONCE per panel build rather than ridden along in the per-frame
// state: it is ~2.5 KB of names that never change, and the state string is compared in full on
// every frame. Sending it here also means the menu's style INDEX is the engine's index by
// construction -- there is no second copy of the list in the JS to fall out of sync.
void CameraTimelineHud::PushShakeStyles() {
	std::ostringstream js;
	// Two lists, one push:
	//   menu  - the curated CAMERA styles, in menu order: {i: engine style index, l: label}.
	//           This is what the arrows walk and what the dropdown lists.
	//   label - every style's display label, keyed by engine index, so a shake set from the
	//           console to an off-menu style still shows a name instead of a bare number.
	js << "$.CamTimeline && $.CamTimeline.setShakeStyles([";
	const int menuN = RageShake_MenuCount();
	for (int i = 0; i < menuN; ++i) {
		const int si = RageShake_MenuStyle(i);
		if (i) js << ",";
		js << "{\"i\":" << si << ",\"l\":\"" << RageShake_StyleLabel(si) << "\"}";
	}
	js << "]," << RageShake_RockstarEditorCount() << ",{";
	const int n = RageShake_StyleCount();
	for (int i = 0; i < n; ++i) {
		if (i) js << ",";
		js << "\"" << i << "\":\"" << RageShake_StyleLabel(i) << "\"";
	}
	js << "});";
	m_bridge.RunScript(js.str().c_str());
}

void CameraTimelineHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) { Teardown(); return; }
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	if (!BuildIfNeeded())
		return;

	// Live REPL (camtl eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	std::string state;
	{ PanoramaPerf_Scope _p("camtl.state"); state = BuildStateJson(); }
	if (state != m_lastState) {
		PanoramaPerf_Scope _p("camtl.push");
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}

	// Timeline geometry is derived from Panorama's live root layout, not just
	// from the state JSON. Render every frame so opening during an unsettled
	// layout cannot leave the bar stuck at its fallback width.
	{ PanoramaPerf_Scope _p("camtl.js"); m_bridge.RunScriptCached("$.CamTimeline && $.CamTimeline.render();", "mvm/camtl.render"); }

	// Read back whether the keyframe menu is up. The JS publishes it on the root, and it
	// decides who the wheel belongs to (MovieMode::OnMouseWheel).
	m_menuOpen = m_root
		&& m_bridge.GetAttributeString(m_root, m_symMenuOpen, "0") == "1";
	m_nativeCameraMode = m_root ? atoi(m_bridge.GetAttributeString(m_root, m_symCamMode, "-1").c_str()) : -1;
	ReadBarRect();

	// After render(), so the list it scrolls has this frame's layout.
	DispatchWheel();
}

// WndProc/input thread. Coalesced into one integer: a fast flick becomes one proportional
// scroll rather than a queue that drains over several frames.
void CameraTimelineHud::PushWheel(int notches) {
	std::lock_guard<std::mutex> lk(m_wheelMutex);
	int next = m_pendingWheel + notches;
	if (next > 32) next = 32;
	if (next < -32) next = -32;
	m_pendingWheel = next;
}

// Main thread. Hands the accumulated notches to the panel, which scrolls its own list.
void CameraTimelineHud::DispatchWheel() {
	int notches = 0;
	{
		std::lock_guard<std::mutex> lk(m_wheelMutex);
		notches = m_pendingWheel;
		m_pendingWheel = 0;
	}
	if (0 == notches || !m_root) return;
	std::ostringstream o;
	o << "$.CamTimeline && $.CamTimeline.wheel && $.CamTimeline.wheel(" << notches << ");";
	m_bridge.RunScript(o.str().c_str());
}

} // namespace Filmmaker

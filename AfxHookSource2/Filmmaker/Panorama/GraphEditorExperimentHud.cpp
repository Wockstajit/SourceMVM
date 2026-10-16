#include "GraphEditorExperimentHud.h"
#include "../Movie/DemoScrub.h"

#include "InspectorWidth.h"

#include "../Movie/TimelinePlayhead.h" // the ONE cursor shared with the camera timeline

#include "PanoramaFindPanel.h"

#include "GraphEditorJs.h"
#include "CameraTimelineHud.h"
#include "../Filmmaker.h"
#include "../Movie/CameraPath.h"
#include "../Movie/CameraBridge.h"
#include "../Movie/DemoTimescale.h" // one demo speed, shared by all three scrubbers
#include "../Movie/SceneKeys.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Render/RenderCommand.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_* (the only diagnostic channel; console prints nothing)

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../../MirvTime.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <cmath>
#include <utility>
#include <vector>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}
double r3(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0;
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 1000.0 + 0.5) / 1000.0;
}

bool PlayingDemo() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

// The demo is actually ADVANCING (loaded and not paused). The JS uses this to page the visible
// window so the playhead stays on screen while playing; a scrub must not trigger that.
bool DemoPlaying() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			return pDemo->IsPlayingDemo() && !pDemo->IsDemoPaused();
	}
	return false;
}

// --- mvm_debug helpers -------------------------------------------------------------------
// The graph now BAKES BACK into the stable camera path, so two things fail silently: an edit
// that never reaches the path (gated out in SyncToCameraPath), and a bake that produces the
// wrong markers. Every probe below therefore carries the model shape, the gate state that
// could have suppressed the sync, and the resulting path count -- not just the outcome.
std::string ChKeyCounts(const GraphExpModel& m) {
	std::ostringstream o;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c) {
		if (c) o << '/';
		o << m.GetChannel(c).keys.size();
	}
	return o.str();
}
// Bit c set = channel c visible / soloed. Compact enough to eyeball a solo-isolation bug.
unsigned ChVisMask(const GraphExpModel& m) {
	unsigned mask = 0;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		if (m.GetChannel(c).visible) mask |= (1u << c);
	return mask;
}
unsigned ChSoloMask(const GraphExpModel& m) {
	unsigned mask = 0;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		if (m.GetChannel(c).solo) mask |= (1u << c);
	return mask;
}

static const char* kChNames[7] = { "Pos X", "Pos Y", "Pos Z", "Pitch", "Yaw", "Roll", "FOV" };
// Keep in sync with S.axis in CameraEditorJs.h (see the rationale block there): one chroma across
// all seven hues, and Pitch off the accent-gold hue.
static const char* kChColors[7] = { "#ff8885", "#80d680", "#69a7ff", "#fd96d7", "#c89cff", "#25c8ff", "#ff9a5a" };
static const double kChMinSpan[7] = { 64.0, 64.0, 64.0, 20.0, 20.0, 20.0, 10.0 };

} // namespace

GraphEditorExperimentHud& GraphEditorExperimentHudRef() {
	static GraphEditorExperimentHud s_instance;
	return s_instance;
}

void* GraphEditorExperimentHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "GraphExpRoot");
}

bool GraphEditorExperimentHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kGraphEditorJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	return m_built;
}

void GraphEditorExperimentHud::Teardown() {
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#GraphExpRoot'); if(e) e.DeleteAsync(0); $.GraphExp=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
}

void GraphEditorExperimentHud::OnEnter() {
	// Seed our isolated model from the stable path (read-only) ONLY if we don't already have one.
	// Enabling fires every time the bottom panel flips back from the timeline; re-seeding there
	// would wipe the user's graph edits. The model is cleared when the demo changes (RunFrame),
	// so a genuinely new session still seeds fresh.
	if (m_model.TotalKeys() == 0) {
		m_model.SeedFromMarkers(CameraPathRef().Markers());
		// Seeded keys carry no handles, i.e. they read as Linear -- but the PATH may be in Smooth
		// mode. Shape the fresh handles to the path's interp so the graph opens showing the curve
		// the camera is actually flying (the other half of the Smooth/Linear sync).
		if (CameraPathRef().GetInterp() == CameraPath::Interp::Bezier)
			m_model.SetInterpAll(true, false);
	}
	m_viewInit = false;
	++m_viewEpoch; // ask the JS to take the fresh suggestion
	TimelinePlayhead_EndScrub();
	// Camera Editor's G key may have switched to game/free-look before this overlay
	// finished entering. Respect that cursor mode instead of re-taking the camera.
	m_drive = CameraEditor_Active() ? CameraTimelineHudRef().Cursor() : true;
	BumpRev();

	// The UI cursor is shown via our GraphEditorExperiment_WantsCursor() hook into MovieMode's
	// suspend/consume chain (so we do NOT poke the timeline's cursor flag, which would otherwise
	// stay stuck on after a standalone exit). Do not force free cam here: opening/clicking an empty
	// graph should leave the viewer in its current mode. DriveCameraThisFrame enables it only once
	// there are graph keys to sample, and the editor command path focuses an existing camera marker.
	if (m_model.TotalKeys() > 0)
		CameraPathRef().StopScrub();

	// Seeding is conditional (only an EMPTY model re-seeds), so "the graph opened with the
	// wrong curves" is answered by seeded= plus the key counts, not by guessing.
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("graph.mode", "enter keys=%s total=%d pathCount=%d drive=%d editorActive=%d cursor=%d",
			ChKeyCounts(m_model).c_str(), m_model.TotalKeys(), CameraPathRef().Count(),
			m_drive ? 1 : 0, CameraEditor_Active() ? 1 : 0, CameraTimelineHudRef().Cursor() ? 1 : 0);
}

void GraphEditorExperimentHud::OnExit() {
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("graph.mode", "exit keys=%s total=%d pathCount=%d drive=%d ownsView=%d",
			ChKeyCounts(m_model).c_str(), m_model.TotalKeys(), CameraPathRef().Count(),
			m_drive ? 1 : 0, OwnsView() ? 1 : 0);
	TimelinePlayhead_EndScrub();
	Teardown();
	// We intentionally leave free cam as-is: the regular Camera Editor (which usually hosts the
	// Experiment button) manages it. Live-drive simply stops because m_enabled is now false.
}

double GraphEditorExperimentHud::PlayheadTick() const {
	// The SHARED cursor, not a private one. This panel and the camera timeline are two views of
	// the same shot, so a drag in either must move the other. See Movie/TimelinePlayhead.h.
	return TimelinePlayhead_Tick();
}

bool GraphEditorExperimentHud::OwnsView() const {
	return m_enabled && m_drive && m_model.TotalKeys() > 0 && CameraBridge_GetFreeCamEnabled();
}

void GraphEditorExperimentHud::DriveCameraThisFrame() {
	if (!m_enabled || !m_drive || m_model.TotalKeys() == 0)
		return;
	// The dock only drives the camera while it is SHOWING the camera. In scene mode it is a
	// lighting/weather editor; writing a camera-path pose every frame from behind it is how
	// "the camera path plays while I am working on the scene graph" happened.
	if (FilmmakerSettings_UiString("editor.graph", "camera") == "scene")
		return;

	// ONLY INSIDE THE PATH'S OWN RANGE. EvalPoseAtTick never fails once there are >= 2 markers
	// -- EvalSegP clamps its segment parameter to [0,1] -- so OUTSIDE the range it happily
	// returns the first or last keyframe, and this per-frame write then pins the camera to that
	// keyframe for the entire rest of the demo. That is exactly the reported "the viewport can
	// suddenly snap back to a camera-path point". CamPlayback already range-gates its own
	// playback; this drive never did.
	{
		const CameraPath& cp = CameraPathRef();
		if (cp.HasPathRange()) {
			const double tick = PlayheadTick();
			if (tick < (double)cp.FirstPathTick() || tick > (double)cp.LastPathTick())
				return;
		}
	}
	double origin[3] = { 0,0,0 }, angles[3] = { 0,0,0 }, fov = 90.0;
	CameraBridge_GetCurrentCamera(origin, angles, fov);
	const double defaults[7] = { origin[0], origin[1], origin[2], angles[0], angles[1], angles[2], fov };
	double pose[7];

	// ONE CAMERA, ONE CURVE. Sample the CAMERA PATH, not this panel's own model.
	//
	// This panel used to fly the camera off GraphExpModel::SamplePose while the camera timeline,
	// playback and the renderer all flew it off CamPathEval/g_CamPath -- and those are not the
	// same curve in any respect:
	//
	//   graph model            camera path (what actually renders)
	//   ----------             -----------------------------------
	//   per-channel cubic      natural cubic spline (position/FOV) +
	//     Bezier, 1/3            quaternion qspline (rotation)
	//     Catmull-Rom handles
	//   parameterised by TICK  parameterised by CHORD LENGTH (the speed-independent
	//                            "shape" axis) -- different pacing between the same keys
	//   independent Euler      quaternion, so rotation takes a different arc entirely
	//   any key count          cubic uses >= 3 markers; two-key paths ease linear geometry
	//
	// So "Smooth" in the graph and "Smooth" on the timeline produced visibly different motion for
	// the same keys, which is exactly the desync being reported. The path wins because the path is
	// what the exported movie comes out of: whatever this panel shows has to be the thing that
	// renders, not a second opinion about it. Every graph edit already bakes into the path
	// (SyncToCameraPath), so the markers here are current.
	//
	// Falls back to the model ONLY when the path cannot evaluate at all (<2 markers), so a graph
	// with keys still previews something instead of freezing.
	if (!CameraPathRef().EvalPoseAtTick(PlayheadTick(), pose))
		m_model.SamplePose(PlayheadTick(), defaults, pose);
	CameraBridge_SetFreeCamEnabled(true);
	CameraBridge_SetCameraPose(pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);

	// Per-frame: heavily throttled. Carries the INPUT (playhead, scrub state, the camera pose
	// used as the per-channel default) next to the sampled OUTPUT, so "camera ignores the
	// curve" separates into wrong-tick / no-keys-on-that-channel / pose-not-written.
	if (MvmDebugLog_Active()) {
		static unsigned s_n = 0;
		if ((s_n++ % 30) == 0)
			MvmDebugLog_Linef("graph.drive",
				"t=%.2f scrub=%d keys=%s in=%.1f,%.1f,%.1f/%.1f,%.1f,%.1f/%.1f out=%.1f,%.1f,%.1f/%.1f,%.1f,%.1f/%.1f",
				PlayheadTick(), TimelinePlayhead_Scrubbing() ? 1 : 0, ChKeyCounts(m_model).c_str(),
				defaults[0], defaults[1], defaults[2], defaults[3], defaults[4], defaults[5], defaults[6],
				pose[0], pose[1], pose[2], pose[3], pose[4], pose[5], pose[6]);
	}
}

std::string GraphEditorExperimentHud::BuildStateJson() {
	int curTick = 0; g_MirvTime.GetCurrentDemoTick(curTick);
	double curTime = 0.0; g_MirvTime.GetCurrentDemoTime(curTime);

	// SUGGEST a starting window -- the JS owns pan/zoom after it seeds, and re-seeds only when
	// viewEpoch advances. Fit the keyframe range when there are keys; otherwise centre on the
	// playhead so the first diamond press lands in view. NEVER re-derive this from the playhead
	// per frame: a window anchored to the live playhead is exactly what dragged keyframes out
	// from under the pointer as you scrubbed.
	double mn = m_model.MinTick(), mx = m_model.MaxTick();
	if (mx <= mn) mx = mn + 1.0;
	if (!m_viewInit) {
		if (m_model.TotalKeys() > 0) {
			double pad = (mx - mn) * 0.04 + 1.0;
			m_viewT0 = mn - pad; m_viewT1 = mx + pad;
		} else {
			double p = PlayheadTick();
			m_viewT0 = p - 1000.0; m_viewT1 = p + 1000.0;
		}
		m_viewInit = true;
	}

	int selCh = -1, selId = -1;
	m_model.FirstSelected(selCh, selId);

	int mxp = 0, myp = 0; bool lmb = false, rmb = false, shiftDown = false; unsigned seq = 0;
	CameraBridge_GetUiCursor(mxp, myp, lmb, rmb, shiftDown, seq);

	std::ostringstream o;
	o << "{";
	o << "\"enabled\":" << (m_enabled ? "true" : "false");
	o << ",\"renderOpen\":" << (Render_IsModalOpen() ? "true" : "false");
	// The operator-dragged inspector width: the graph dock fills the screen LEFT of that column,
	// so it needs the same number CONFIG and the editor lay out to. See InspectorWidth.h.
	InspectorWidth_AppendJson(o);
	o << ",\"drive\":" << (m_drive ? "true" : "false");
	o << ",\"scrubbing\":" << (TimelinePlayhead_Scrubbing() ? "true" : "false");
	// Distinguishes PLAYBACK from a scrub: only playback pages the window to follow the playhead.
	o << ",\"playing\":" << (DemoPlaying() ? "true" : "false");
	o << ",\"playhead\":" << r2(PlayheadTick());
	// The shot's ONE interpolation mode, reported as the path is ACTUALLY evaluating it (the cubic
	// curve needs >=3 markers), so this panel's Smooth/Linear pair matches both the camera
	// timeline's pair and the motion you can see.
	o << ",\"interp\":\"" << CameraPathRef().EffectiveInterpName() << "\"";
	o << ",\"interpDegraded\":" << (CameraPathRef().InterpDegraded() ? "true" : "false");
	o << ",\"demoTick\":" << curTick;
	o << ",\"time\":" << r2(curTime);
	o << ",\"minTick\":" << r2(mn) << ",\"maxTick\":" << r2(mx);
	// The FULL navigable range for the timeline overview/navigator + bottom scrollbar (AE's comp
	// duration). This is the actual demo length, not the keyframe span above -- a shot with three
	// keys 30 seconds into a 5-minute demo still needs the whole 5 minutes representable, or the
	// navigator cannot show where in the demo those keys sit. Falls back to a generous window past
	// whatever IS known (keys, playhead) when the demo's own bounds are not resolvable yet (no
	// active demo, or its header has not been read), so the overview always has something to draw.
	{
		int demoStart = -1, demoEnd = -1;
		double domT0 = 0.0, domT1;
		if (Render_GetDemoBounds(demoStart, demoEnd) && demoEnd > demoStart) { domT0 = demoStart; domT1 = demoEnd; }
		else domT1 = ((mx > (double)curTick) ? mx : (double)curTick) + 2000.0;
		o << ",\"domainT0\":" << r2(domT0) << ",\"domainT1\":" << r2(domT1);
	}
	o << ",\"viewT0\":" << r2(m_viewT0) << ",\"viewT1\":" << r2(m_viewT1);
	// Advances only when the suggestion above should be re-applied (dock enter, reseed, clear,
	// demo change). The JS re-seeds on this and on a camera/scene mode switch; nothing else moves
	// the visible window.
	o << ",\"viewEpoch\":" << m_viewEpoch;
	o << ",\"rev\":" << m_rev;
	o << ",\"selCount\":" << m_model.SelectionCount();
	o << ",\"canUndo\":" << (m_model.CanUndo() ? "true" : "false") << ",\"canRedo\":" << (m_model.CanRedo() ? "true" : "false");
	o << ",\"selCh\":" << selCh << ",\"selId\":" << selId;
	o << ",\"mx\":" << mxp << ",\"my\":" << myp << ",\"lmb\":" << (lmb ? "true" : "false")
		<< ",\"rmb\":" << (rmb ? "true" : "false") << ",\"shift\":" << (shiftDown ? "true" : "false")
		<< ",\"ctrl\":" << (CameraBridge_GetCtrlDown() ? "true" : "false")
		<< ",\"alt\":" << (CameraBridge_GetAltDown() ? "true" : "false") << ",\"seq\":" << seq;
	// The engine's demo speed, so this dock's scrubber agrees with the other two. NOT r2()'d --
	// the control is continuous and 2 decimals would snap the handle after every drag.
	o << ",\"timescale\":" << DemoTimescale_Get();
	const std::string inspectorMode = FilmmakerSettings_UiString("editor.inspector", "path");
	o << ",\"uiEditorInspector\":\"" << inspectorMode << "\"";
	// WHICH MODEL THIS DOCK SHOWS IS ITS OWN SETTING, not a side effect of the inspector tab.
	// It used to key off inspectorMode, so opening SCENE KEYS in the column silently replaced the
	// camera graph -- a shot with camera paths down had no way left to see or edit their curves.
	// The dock now carries its own GRAPH / SCENE GRAPH switch and persists the choice.
	const std::string graphMode = FilmmakerSettings_UiString("editor.graph", "camera") == "scene" ? "scene" : "camera";
	o << ",\"graphMode\":\"" << graphMode << "\"";
	// TIME SNAP (editor.graph.snap): "off" | "auto" | a tick count. ONE shared value for both graph
	// modes; the JS quantises the playhead, key placement and key drags to it. Sanitised to
	// [a-z0-9-] so an odd stored value can never break this JSON.
	{
		const std::string raw = FilmmakerSettings_UiString("editor.graph.snap", "off");
		std::string safe;
		for (char c : raw)
			if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '-') safe += c;
		o << ",\"snap\":\"" << (safe.empty() ? "off" : safe) << "\"";
	}
	if (graphMode == "scene") {
		SceneKeys_AppendGraphState(o);
		o << "}";
		return o.str();
	}
	// The LIVE camera pose, per channel, so a row's diamond can place a key using "the value you
	// are looking at" -- the AE convention. Without it the first key on an empty channel had no
	// value to take and defaulted to zero-something, which is not what the viewport showed.
	double liveCam[7] = { 0, 0, 0, 0, 0, 0, 90.0 };
	{
		double o0[3] = { 0,0,0 }, a0[3] = { 0,0,0 }, fov0 = 90.0;
		CameraBridge_GetCurrentCamera(o0, a0, fov0);
		liveCam[0] = o0[0]; liveCam[1] = o0[1]; liveCam[2] = o0[2];
		liveCam[3] = a0[0]; liveCam[4] = a0[1]; liveCam[5] = a0[2]; liveCam[6] = fov0;
	}
	o << ",\"channels\":[";
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c) {
		if (c) o << ",";
		const GraphExpModel::Channel& ch = m_model.GetChannel(c);
		// Per-channel value range (padded), for the Y mapping in the JS.
		double lo = 1e30, hi = -1e30;
		for (const auto& k : ch.keys) { if (k.value < lo) lo = k.value; if (k.value > hi) hi = k.value; } // KEYFRAME values only -- handles do NOT pull on the scale, so you can drag them past the window edge and the view stays put (no zoom-out / refit for handles)
		if (ch.keys.empty()) { lo = 0.0; hi = 1.0; }
		double center = (lo + hi) * 0.5, span = hi - lo; if (span < 1e-6) span = 1.0;
		double vspan = span * 1.5; if (vspan < kChMinSpan[c]) vspan = kChMinSpan[c];
		lo = center - vspan * 0.5; hi = center + vspan * 0.5;

		o << "{\"ch\":" << c << ",\"name\":\"" << kChNames[c] << "\",\"color\":\"" << kChColors[c] << "\"";
		// vis drives what the JS DRAWS. Solo is an isolation view (AE-style): while any channel
		// is soloed, only the solo channel's curve/keys are drawn (plus its own eye-toggle still
		// applies). The eye toggle alone only hides; it never isolates the others.
		const bool drawVis = ch.visible && (!m_model.AnySolo() || ch.solo);
		o << ",\"vis\":" << (drawVis ? "true" : "false") << ",\"solo\":" << (ch.solo ? "true" : "false");
		o << ",\"edit\":" << (m_model.IsEditable(c) ? "true" : "false");
		o << ",\"cur\":" << r3(liveCam[c]);
		o << ",\"min\":" << r2(lo) << ",\"max\":" << r2(hi);
		o << ",\"keys\":[";
		for (size_t i = 0; i < ch.keys.size(); ++i) {
			if (i) o << ",";
			const GraphExpModel::Keyframe& k = ch.keys[i];
			o << "{\"id\":" << k.id << ",\"t\":" << r2(k.tick) << ",\"v\":" << r2(k.value)
				<< ",\"sel\":" << (m_model.IsSelected(c, k.id) ? "true" : "false")
				<< ",\"cl\":{\"a\":" << (k.cpLeft.active ? 1 : 0) << ",\"tx\":" << r3(k.cpLeft.tx) << ",\"dv\":" << r2(k.cpLeft.dv) << "}"
				<< ",\"cr\":{\"a\":" << (k.cpRight.active ? 1 : 0) << ",\"tx\":" << r3(k.cpRight.tx) << ",\"dv\":" << r2(k.cpRight.dv) << "}}";
		}
		o << "]}";
	}
	o << "]}";
	return o.str();
}

void GraphEditorExperimentHud::RunFrame() {
	m_bridge.Init();

	unsigned char* hud = PlayingDemo() ? AfxHookSource2_GetPanoramaHudPanel() : nullptr;
	if (!hud) {
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		// Demo gone: drop the model so the NEXT demo/session re-seeds fresh (OnEnter only seeds
		// an empty model, so without this it would keep showing the previous demo's curves).
		if (m_model.TotalKeys() > 0) { m_model.Clear(); m_viewInit = false; ++m_viewEpoch; }
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	if (!m_enabled)
		return;

	// Keys placed while the graph is open (K in the editor, the onboarding tutorial) never reached
	// it: OnEnter only seeds an EMPTY model, once. An empty model has no edits to lose, so it takes
	// the path's keys as soon as there are any, and the view is asked to frame them.
	if (m_model.TotalKeys() == 0 && CameraPathRef().Count() > 0) {
		m_model.SeedFromMarkers(CameraPathRef().Markers());
		if (CameraPathRef().GetInterp() == CameraPath::Interp::Bezier)
			m_model.SetInterpAll(true, false);
		m_viewInit = false;
		++m_viewEpoch;
		BumpRev();
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("graph.mode", "late seed total=%d pathCount=%d", m_model.TotalKeys(), CameraPathRef().Count());
	}

	if (!BuildIfNeeded())
		return;

	// Live REPL (grapheditor eval) in the panel context.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}

	m_root = FindRoot();
	if (!m_root) { m_built = false; return; }

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Layout depends on Panorama's live root measurements, which can be 0 on the
	// first frame after build. Re-render every frame so the dock corrects itself
	// once actuallayoutwidth / actuallayoutheight settle, even while paused.
	m_bridge.RunScriptCached("$.GraphExp && $.GraphExp.render();", "mvm/graphexp.render");

	// Live-drive runs EVERY frame (independent of the render-on-change gate) so the camera
	// keeps following the curves while the demo plays.
	DriveCameraThisFrame();
}

// ---- command surface --------------------------------------------------------

void GraphEditorExperimentHud::CmdChannel(int ch, int op) {
	switch (op) {
	case 0: m_model.SetVisible(ch, false); break;
	case 1: m_model.SetVisible(ch, true); break;
	case 2: m_model.SetSolo(ch, true); break;
	case 3: m_model.SetSolo(ch, false); break;
	}
	BumpRev();
	// vis/solo masks come from DebugLogEdit: solo isolation is "vis unchanged, solo set, and
	// the JS draws only the solo channel", which is only checkable with both masks present.
	DebugLogEdit("channel", "ch=%d op=%d", ch, op);
}
void GraphEditorExperimentHud::CmdSelect(int ch, int id, bool add) { m_model.Select(ch, id, add); BumpRev(); DebugLogEdit("select", "ch=%d id=%d add=%d", ch, id, add ? 1 : 0); }
void GraphEditorExperimentHud::CmdSelectAll() { m_model.SelectAll(); BumpRev(); DebugLogEdit("selectAll", ""); }
void GraphEditorExperimentHud::CmdSelectClear() { m_model.ClearSelection(); BumpRev(); DebugLogEdit("selectClear", ""); }

void GraphEditorExperimentHud::CmdSelectSet(const char* csv) {
	m_model.ClearSelection();
	if (csv && *csv) {
		const char* p = csv;
		while (*p) {
			int ch = std::atoi(p);
			const char* colon = std::strchr(p, ':');
			if (!colon) break;
			int id = std::atoi(colon + 1);
			m_model.SelectAdd(ch, id);
			const char* comma = std::strchr(colon, ',');
			if (!comma) break;
			p = comma + 1;
		}
	}
	BumpRev();
	DebugLogEdit("selectSet", "csv='%s'", csv ? csv : "");
}

// Project the graph model into the stable camera path so the Camera timeline, playback and
// the per-demo sidecar always mirror the curves edited here. Markers are baked at the union
// of all channels' key ticks (channels sampled at those ticks), so channels that are still in
// sync produce exactly one marker per shared key tick. Per-marker ease/speedMul (path-side
// concepts the graph does not model) carry over on an exact tick match.
void GraphEditorExperimentHud::SyncToCameraPath() {
	if (!m_enabled || !PlayingDemo()) {
		// Not an error, but it IS the number-one reason a graph edit appears to do nothing to
		// the Camera timeline, so it must be visible rather than inferred.
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("graph.sync", "skipped enabled=%d demo=%d keys=%s undoPend=%d",
				m_enabled ? 1 : 0, PlayingDemo() ? 1 : 0, ChKeyCounts(m_model).c_str(),
				m_pathUndoPending ? 1 : 0);
		return;
	}

	// Union of key ticks across channels (dedupe within half a tick).
	std::vector<double> ticks;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const auto& k : m_model.GetChannel(c).keys)
			ticks.push_back(k.tick);
	std::sort(ticks.begin(), ticks.end());
	ticks.erase(std::unique(ticks.begin(), ticks.end(),
		[](double a, double b) { return std::abs(a - b) < 0.5; }), ticks.end());

	CameraPath& cp = CameraPathRef();
	const std::vector<CamMarker>& old = cp.Markers();
	float ipt = g_MirvTime.interval_per_tick_get();

	std::vector<CamMarker> baked;
	baked.reserve(ticks.size());
	for (double t : ticks) {
		CamMarker m;
		m.tick = (int)std::floor(t + 0.5);
		m.time = (ipt > 0.0f) ? t * ipt : 0.0;
		double v = 0.0;
		m.x = m_model.SampleChannel(0, t, v) ? v : 0.0;
		m.y = m_model.SampleChannel(1, t, v) ? v : 0.0;
		m.z = m_model.SampleChannel(2, t, v) ? v : 0.0;
		m.pitch = m_model.SampleChannel(3, t, v) ? v : 0.0;
		m.yaw = m_model.SampleChannel(4, t, v) ? v : 0.0;
		m.roll = m_model.SampleChannel(5, t, v) ? v : 0.0;
		m.fov = m_model.SampleChannel(6, t, v) ? v : 90.0;
		for (const CamMarker& om : old) {
			if (std::abs((double)om.tick - t) < 0.5) { m.ease = om.ease; m.speedMul = om.speedMul; break; }
		}
		baked.push_back(m);
	}

	const bool pushUndo = m_pathUndoPending;
	m_pathUndoPending = false;
	const int pathBefore = cp.Count();
	const int selBefore = cp.Selected();
	cp.ReplaceMarkersFromGraph(baked, pushUndo);

	// Logged AFTER the write so one line carries inputs (union tick count, key counts, undo
	// arming) and the real effect (marker count + selection before/after). Drag-rate callers
	// hit this per mouse-move, so throttle -- but never skip a line where the marker COUNT
	// changed, since that is the structural event worth reconstructing.
	if (MvmDebugLog_Active()) {
		static unsigned s_n = 0;
		static size_t s_lastBaked = (size_t)-1;
		const bool structural = (baked.size() != s_lastBaked);
		s_lastBaked = baked.size();
		if (structural || (s_n++ % 8) == 0)
			MvmDebugLog_Linef("graph.sync",
				"baked keys=%s unionTicks=%zu markers=%zu pathCount=%d->%d sel=%d->%d undo=%d "
				"tick0=%d tickN=%d ipt=%.5f",
				ChKeyCounts(m_model).c_str(), ticks.size(), baked.size(),
				pathBefore, cp.Count(), selBefore, cp.Selected(), pushUndo ? 1 : 0,
				baked.empty() ? -1 : baked.front().tick, baked.empty() ? -1 : baked.back().tick,
				(double)ipt);
	}
}

void GraphEditorExperimentHud::DebugLogEdit(const char* op, const char* fmt, ...) {
	if (!MvmDebugLog_Active())
		return;
	char detail[192];
	detail[0] = 0;
	if (fmt && *fmt) {
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(detail, sizeof(detail), fmt, ap);
		va_end(ap);
	}
	MvmDebugLog_Linef("graph.edit",
		"op=%s %s keys=%s total=%d sel=%d rev=%u undoPend=%d canUndo=%d vis=%02x solo=%02x",
		op, detail, ChKeyCounts(m_model).c_str(), m_model.TotalKeys(), m_model.SelectionCount(),
		m_rev, m_pathUndoPending ? 1 : 0, m_model.CanUndo() ? 1 : 0,
		ChVisMask(m_model), ChSoloMask(m_model));
}

std::string GraphEditorExperimentHud::DebugStateLine() const {
	int selCh = -1, selId = -1;
	m_model.FirstSelected(selCh, selId);
	std::ostringstream o;
	o << "enabled=" << (m_enabled ? 1 : 0)
		<< " built=" << (m_built ? 1 : 0)
		<< " drive=" << (m_drive ? 1 : 0)
		<< " ownsView=" << (OwnsView() ? 1 : 0)
		<< " freeCam=" << (CameraBridge_GetFreeCamEnabled() ? 1 : 0)
		<< " scrubbing=" << (TimelinePlayhead_Scrubbing() ? 1 : 0)
		<< " scrubTick=" << TimelinePlayhead_Tick()
		<< " playhead=" << PlayheadTick()
		<< " keys=" << ChKeyCounts(m_model)
		<< " total=" << m_model.TotalKeys()
		<< " minTick=" << m_model.MinTick()
		<< " maxTick=" << m_model.MaxTick()
		<< " sel=" << m_model.SelectionCount()
		<< " selCh=" << selCh << " selId=" << selId
		<< " vis=" << ChVisMask(m_model) << " solo=" << ChSoloMask(m_model)
		<< " anySolo=" << (m_model.AnySolo() ? 1 : 0)
		<< " canUndo=" << (m_model.CanUndo() ? 1 : 0)
		<< " canRedo=" << (m_model.CanRedo() ? 1 : 0)
		<< " undoPend=" << (m_pathUndoPending ? 1 : 0)
		<< " rev=" << m_rev
		<< " view=" << m_viewT0 << ".." << m_viewT1 << " viewInit=" << (m_viewInit ? 1 : 0)
		<< " pathCount=" << CameraPathRef().Count()
		<< " pathSel=" << CameraPathRef().Selected();
	return o.str();
}

// Gesture boundary. Always logged (it is cheap and it is what makes the throttled drag
// samples below readable: everything between two editBegin lines is one undo step).
void GraphEditorExperimentHud::CmdEditBegin() { m_model.BeginEdit(); m_pathUndoPending = true; DebugLogEdit("editBegin", ""); }

// The next three fire once per mouse-move during a drag, so they sample 1-in-8. The settled
// end-of-gesture value is always available from `state.graph` (any mirv_filmmaker command).
void GraphEditorExperimentHud::CmdMoveSelectedBy(double dTick, double dValue) {
	m_model.MoveSelectedBy(dTick, dValue); BumpRev(); SyncToCameraPath();
	static unsigned s_n = 0;
	if ((s_n++ % 8) == 0) DebugLogEdit("moveSel", "dT=%.3f dV=%.3f", dTick, dValue);
}
void GraphEditorExperimentHud::CmdMoveKeyAbs(int ch, int id, double tick, double value) {
	m_model.MoveKeyAbs(ch, id, tick, value); BumpRev(); SyncToCameraPath();
	static unsigned s_n = 0;
	if ((s_n++ % 8) == 0) DebugLogEdit("moveAbs", "ch=%d id=%d t=%.3f v=%.3f", ch, id, tick, value);
}
void GraphEditorExperimentHud::CmdSetValue(int ch, int id, double value) {
	m_model.SetKeyValue(ch, id, value); BumpRev(); SyncToCameraPath();
	static unsigned s_n = 0;
	if ((s_n++ % 8) == 0) DebugLogEdit("setValue", "ch=%d id=%d v=%.3f", ch, id, value);
}

void GraphEditorExperimentHud::CmdAddKey(int ch, double tick, double value) {
	// No internal undo snapshot: callers that mutate per-frame (the number-field scrub, typed
	// commit) bracket the gesture with editbegin themselves, so this stays one undo step.
	int id = m_model.AddKey(ch, tick, value);
	m_model.Select(ch, id, false);
	BumpRev();
	SyncToCameraPath();
	DebugLogEdit("addKey", "ch=%d t=%.3f v=%.3f -> id=%d", ch, tick, value, id);
}
void GraphEditorExperimentHud::CmdDeleteKey(int ch, int id) { m_model.BeginEdit(); m_pathUndoPending = true; m_model.DeleteKey(ch, id); BumpRev(); SyncToCameraPath(); DebugLogEdit("deleteKey", "ch=%d id=%d", ch, id); }

void GraphEditorExperimentHud::CmdDeleteSelected() {
	m_model.BeginEdit();
	m_pathUndoPending = true;
	// Collect (ch,id) of selected first, then delete (deleting mutates the selection set).
	std::vector<std::pair<int, int>> doomed;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const auto& k : m_model.GetChannel(c).keys)
			if (m_model.IsSelected(c, k.id)) doomed.emplace_back(c, k.id);
	const size_t doomedCount = doomed.size();
	for (auto& d : doomed) m_model.DeleteKey(d.first, d.second);
	m_model.ClearSelection();
	BumpRev();
	SyncToCameraPath();
	DebugLogEdit("deleteSel", "doomed=%zu", doomedCount);
}

void GraphEditorExperimentHud::CmdClear() {
	m_model.BeginEdit();
	m_model.Clear();
	m_viewInit = false;
	++m_viewEpoch;
	TimelinePlayhead_EndScrub();
	BumpRev();
	// No SyncToCameraPath: CmdClear also runs on demo unload / CameraPath::DeleteAll, where
	// writing an empty marker list back would be wrong (or redundant). The graph's own model
	// is empty; the path is rebuilt by whoever triggered the clear.
	// NOTE for the log reader: no graph.sync line follows a clear -- that is deliberate, not a
	// missed sync (CmdClear also runs on demo unload).
	DebugLogEdit("clear", "noSync");
}

void GraphEditorExperimentHud::CmdSetHandle(int ch, int id, int side, double tx, double dv, bool reflect) {
	m_model.SetHandle(ch, id, side, tx, dv, reflect); BumpRev(); SyncToCameraPath();
	static unsigned s_n = 0; // handle drag: same 1-in-8 sampling as the key drags
	if ((s_n++ % 8) == 0) DebugLogEdit("setHandle", "ch=%d id=%d side=%d tx=%.3f dv=%.3f refl=%d", ch, id, side, tx, dv, reflect ? 1 : 0);
}
void GraphEditorExperimentHud::CmdClearHandles(int ch, int id) { m_model.BeginEdit(); m_pathUndoPending = true; m_model.ClearHandles(ch, id); BumpRev(); SyncToCameraPath(); DebugLogEdit("clearHandles", "ch=%d id=%d", ch, id); }

void GraphEditorExperimentHud::CmdPlayhead(double tick, bool seek) {
	// Writes the SHARED cursor, so the camera timeline's readout and slider follow this drag
	// even though it started on the graph's ruler.
	if (seek) TimelinePlayhead_EndScrub();   // release: follow the demo tick again (JS sends scrub release)
	else {
		TimelinePlayhead_BeginScrub(tick);   // drag: preview the camera at this tick...
		DemoScrub_Drag(tick);                // ...and the world follows (Movie/DemoScrub.h)
	}
	// The release (seek=1) is the interesting edge -- always log it; the drag samples.
	if (MvmDebugLog_Active()) {
		static unsigned s_n = 0;
		if (seek || (s_n++ % 8) == 0)
			MvmDebugLog_Linef("graph.playhead", "t=%.2f seek=%d scrubbing=%d demoTick=%.0f",
				tick, seek ? 1 : 0, TimelinePlayhead_Scrubbing() ? 1 : 0, PlayheadTick());
	}
}

void GraphEditorExperimentHud::CmdEase(int mode, bool selectedOnly) { m_model.BeginEdit(); m_pathUndoPending = true; m_model.SetEase(mode, selectedOnly); BumpRev(); SyncToCameraPath(); DebugLogEdit("ease", "mode=%d selOnly=%d", mode, selectedOnly ? 1 : 0); }
void GraphEditorExperimentHud::CmdSetInterp(bool smooth, bool selectedOnly) {
	m_model.BeginEdit(); m_pathUndoPending = true;
	m_model.SetInterpAll(smooth, selectedOnly);
	// Whole-graph Smooth/Linear IS the shot's interpolation, so it must reach the camera path's
	// own Interp too -- the timeline scrubs/plays through CamPathEval, which only knows THAT
	// setting. Without this, "Smooth" was true inside the graph and silently linear the moment
	// the bottom panel switched to the camera timeline (the shape is per-channel handles here vs
	// one global mode there, so they can only agree if both are told). Selection-only smoothing
	// has no global equivalent, so it stays graph-local. Set BEFORE the bake so the rebuild
	// inside SyncToCameraPath already runs with the new mode.
	if (!selectedOnly)
		CameraPathRef().SetInterp(smooth ? CameraPath::Interp::Bezier : CameraPath::Interp::Linear);
	BumpRev(); SyncToCameraPath();
	DebugLogEdit("interp", "smooth=%d selOnly=%d", smooth ? 1 : 0, selectedOnly ? 1 : 0);
}

void GraphEditorExperimentHud::MirrorPathInterp(bool smooth) {
	// The camera timeline's Linear/Smooth control changed CameraPath::Interp; reshape our
	// handles to match so reopening the graph shows the mode the user just picked. Handles only:
	// no path bake (the path is already correct), no undo push (this mirrors a path-side edit,
	// it is not a new graph edit). A never-seeded model has nothing to reshape -- and calling
	// SyncToCameraPath on one would bake ZERO keys over the real markers, so it must stay a
	// plain no-op here.
	if (m_model.TotalKeys() == 0)
		return;
	m_model.SetInterpAll(smooth, false);
	BumpRev();
}

void GraphEditorExperimentHud::CmdAlignSelection(int ch, int id) {
	const GraphExpModel::Keyframe* k = m_model.FindKeyConst(ch, id);
	if (!k) {
		// Right-clicking a key the model no longer has (stale JS id after a reseed/undo) is a
		// silent no-op from the UI side; make it visible.
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("graph.align", "skipped noKey ch=%d id=%d keys=%s",
				ch, id, ChKeyCounts(m_model).c_str());
		return;
	}
	CmdAlignSelectionToTick(k->tick);
}

void GraphEditorExperimentHud::CmdAlignSelectionToTick(double tick) {
	if (m_model.SelectionCount() == 0) {
		if (MvmDebugLog_Active())
			MvmDebugLog_Linef("graph.align", "skipped sel=0 tick=%.3f", tick);
		return;
	}
	const int selBefore = m_model.SelectionCount();
	m_model.BeginEdit();
	m_pathUndoPending = true;
	const int moved = m_model.AlignSelectionToTick(tick);
	BumpRev();
	SyncToCameraPath();
	// Post-condition, computed rather than assumed: every selected key must now sit on `tick`.
	// offTick != 0 IS the align bug (keys that refused to move), not a symptom of one.
	int offTick = 0;
	for (int c = 0; c < GraphExpModel::kChannelCount; ++c)
		for (const auto& k : m_model.GetChannel(c).keys)
			if (m_model.IsSelected(c, k.id) && std::abs(k.tick - tick) > 0.5) ++offTick;
	if (MvmDebugLog_Active())
		MvmDebugLog_Linef("graph.align", "tick=%.3f sel=%d moved=%d offTick=%d keys=%s",
			tick, selBefore, moved, offTick, ChKeyCounts(m_model).c_str());
}

void GraphEditorExperimentHud::CmdUndo() { m_model.Undo(); BumpRev(); SyncToCameraPath(); DebugLogEdit("undo", ""); }
void GraphEditorExperimentHud::CmdRedo() { m_model.Redo(); BumpRev(); SyncToCameraPath(); DebugLogEdit("redo", ""); }
void GraphEditorExperimentHud::CmdReseed() {
	const int pathCount = CameraPathRef().Count();
	m_model.SeedFromMarkers(CameraPathRef().Markers());
	// Same as the OnEnter seed: fresh keys have no handles, so a Smooth path must re-shape them
	// or the reseed silently downgrades the graph's view of the shot to Linear.
	if (CameraPathRef().GetInterp() == CameraPath::Interp::Bezier)
		m_model.SetInterpAll(true, false);
	m_viewInit = false; ++m_viewEpoch; BumpRev();
	DebugLogEdit("reseed", "fromPathCount=%d", pathCount);
}

} // namespace Filmmaker

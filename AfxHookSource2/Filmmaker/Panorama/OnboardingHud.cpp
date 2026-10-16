#include "OnboardingHud.h"

#include "OnboardingJs.h"
#include "PanoramaFindPanel.h"

#include "../Onboarding/Onboarding.h"
#include "../Movie/TimelineDock.h"
#include "../Environment/EnvironmentScene.h" // RenderedCamera: projects the tutorial's world markers
#include "../Filmmaker.h"                   // CameraEditor_Enabled, GraphEditorExperiment_Enabled
#include "ConfigHud.h"
#include "../Movie/CameraPath.h"
#include "../Movie/SceneKeys.h"
#include "GraphEditorExperimentHud.h"
#include "../Movie/WeaponRecoil.h"
#include "../Movie/WeaponBob.h"
#include "../Render/RenderCommand.h"
#include "../Render/RenderOutput.h"   // the chosen output folder
#include "../Render/RenderRange.h"    // the render range mode
#include "../Config/FilmmakerSettingsStore.h" // editor.inspector: which Camera Editor tab is up
#include "../../MirvTime.h"
#include "../Movie/DemoScrub.h"
#include "../Movie/MovieMode.h"
#include "../Movie/CameraBridge.h" // CameraBridge_GetFreeCamEnabled: our free cam over the view   // which demo camera is up (the camera step wants the free cam)
#include "WorkspacePreview.h" // the editor's preview box, where the world is drawn
#include "../Render/RenderSessionDriver.h"
#include "../Platform/JsonBuilder.h"
#include "../Debug/MvmDebugLog.h"
#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel / MainMenuPanel
#include "../../WindowInputHooks.h" // g_UiCursorProbe: game HWND
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <windows.h>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

OnboardingHud& OnboardingHudRef() {
	static OnboardingHud s_instance;
	return s_instance;
}

bool OnboardingHud::BuildIfNeeded() {
	if (m_built) return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel()) return false;
	if (!m_bridge.RunScript(kOnboardingJs)) return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_root = FindChildById(m_bridge.ContextPanel(), "MvmOnboardingRoot");
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	MvmDebugLog_LinefAlways("onboarding", "panel build %s root=%p", m_built ? "ok" : "FAILED", m_root);
	return m_built;
}

void OnboardingHud::Teardown() {
	if (m_built && m_context && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#MvmOnboardingRoot'); if(e) e.DeleteAsync(0); $.MvmOnboarding=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_context = nullptr;
	m_lastJson.clear();
}

void OnboardingHud::RunFrame() {
	const OnboardingState& st = Onboarding_State();
	// Never opened: no panel, no script, no work.
	if (!st.on && !m_built) { m_wasOn = false; return; }

	m_bridge.Init();

	// Same context choice as the easter-egg overlay: CS2 keeps a HUD pointer at the main menu,
	// but that tree has a 0x0 layout, so a loaded level decides.
	unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel();
	unsigned char* menu = AfxHookSource2_GetPanoramaMainMenuPanel();
	const char* level = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
	const bool loaded = level && level[0] && std::strcmp(level, "<empty>") != 0;
	unsigned char* ctx = loaded && hud ? hud : menu;
	if (!ctx) { m_built = false; m_root = nullptr; m_context = nullptr; return; }
	if (ctx != m_context) {
		if (m_context && (m_context == hud || m_context == menu)) Teardown();
		m_built = false; m_root = nullptr; m_context = ctx;
		MvmDebugLog_LinefAlways("onboarding", "context=%s panel=%p level=%s",
			ctx == hud ? "hud" : "menu", ctx, level ? level : "");
	}
	// Turned off in a context that never built it: nothing to hide.
	if (!st.on && !m_built) { m_wasOn = false; return; }
	m_bridge.SetContextPanel(ctx);
	const bool freshBuild = !m_built;
	if (!BuildIfNeeded()) return;
	// Onboarding runs on a paused demo: when the panel is built in a demo (a demo loaded under
	// the tutorial, or the tutorial opened mid-demo) and after every lesson seek.
	if ((freshBuild && ctx == hud) || st.seekSerial != m_seekSerial) {
		m_seekSerial = st.seekSerial;
		m_pauseOnDemo = true;
		m_pauseAfterMs = GetTickCount64() + 400;
		m_pausedSinceMs = 0;
		m_pauseUntilMs = GetTickCount64() + 30000;
	}

	m_root = FindChildById(ctx, "MvmOnboardingRoot");
	if (!m_root) { m_built = false; return; }

	// Cursor in client fractions, whether a pointer is on screen, and whether the left button is
	// held: the panel fades while the user works other controls and steps aside on a hover.
	double cx = -1.0, cy = -1.0;
	bool cvis = false, lmb = false;
	HWND hwnd = g_UiCursorProbe.hwnd;
	RECT rc{};
	if (hwnd && GetClientRect(hwnd, &rc) && rc.right > 0 && rc.bottom > 0) {
		POINT pt{};
		CURSORINFO ci{}; ci.cbSize = sizeof(ci);
		const bool foreground = GetForegroundWindow() == hwnd;
		if (GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) {
			cx = (double)pt.x / rc.right;
			cy = (double)pt.y / rc.bottom;
			cvis = foreground && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor
				&& cx >= 0.0 && cx <= 1.0 && cy >= 0.0 && cy <= 1.0;
		}
		lmb = foreground && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
	}

	const bool inHud = (ctx == hud);
	JsonBuilder j;
	j.BeginObject();
	j.BoolField("on", st.on);
	j.BoolField("closed", st.closed);
	j.BoolField("voice", st.voice);
	j.StringField("route", st.route);
	j.IntField("step", st.step);
	j.IntField("rev", (int64_t)st.rev);
	j.DoubleField("px", st.posX);
	j.DoubleField("py", st.posY);
	// A render whose capture could include UI: nothing of ours may reach the take.
	j.BoolField("hidden", RenderSessionDriver_WantsChromeHidden());
	j.StringField("ctx", inHud ? "hud" : "menu");
	// The bottom band he must sit above, in the dock's 1080-tall design units: CS2's demo bar or
	// our camera / graph dock in game, nothing at the main menu.
	j.IntField("dockH", inHud ? TimelineDock_Height() : 0);
	j.DoubleField("cx", cx);
	j.DoubleField("cy", cy);
	j.BoolField("cvis", cvis);
	j.BoolField("lmb", lmb);
	j.BoolField("dbg", MvmDebugLog_Active()); // the panel logs its first chatter sound only then
	// What the user has done, so a lesson step can move on by itself: the workspaces open, the
	// dock tab, how many camera keys exist, scene-key edits, the effects a step turns on, and
	// where the demo is. In game only; the main menu has none of it.
	if (inHud) {
		j.Key("app"); j.BeginObject();
		j.BoolField("editor", CameraEditor_Enabled());
		j.BoolField("config", ConfigHud_Enabled());
		j.BoolField("render", Render_IsModalOpen());
		// The Render workspace, for the render lesson: tab (0/1 RANGE & CAMERA, 2 PASSES, 3 OUTPUT),
		// camera source (4 = Camera Path), range mode (2 = Camera Path), whether a folder is
		// chosen, a job running, and the last job's end (11 completed, 12 failed, 14 canceled).
		j.IntField("renderTab", Render_GetModalTab());
		j.IntField("renderCam", (int)Render_GetCameraSource());
		j.IntField("renderRange", (int)Render_GetRange().mode);
		j.BoolField("renderFolder", !RenderOutputSettingsRef().folder.empty());
		j.BoolField("renderJob", Render_IsJobActive());
		j.IntField("renderState", (int)Render_GetSessionState());
		j.BoolField("graph", GraphEditorExperiment_Enabled());
		j.StringField("dock", TimelineDock_TabName());
		j.StringField("insp", FilmmakerSettings_UiString("editor.inspector", "path"));
		j.IntField("keys", CameraPathRef().Count());
		// The selected camera key and how the path is shaped (the camera-keys lesson).
		{
			const CameraPath& cp = CameraPathRef();
			const int sel = cp.Selected();
			const auto& ms = cp.Markers();
			const bool ok = sel >= 0 && sel < (int)ms.size();
			j.IntField("sel", ok ? sel : -1);
			j.DoubleField("selFov", ok ? ms[sel].fov : 0.0);
			j.IntField("selEase", ok ? (int)ms[sel].ease : 0);
			j.BoolField("smooth", cp.GetInterp() == CameraPath::Interp::Bezier);
		}
		// Scene keys: properties switched on, keys made (the revision also moves on a mere toggle).
		j.IntField("skArmed", SceneKeys_ArmedCount());
		j.IntField("skKeys", SceneKeys_KeyCount());
		// The graph: which graph, a key selected, a channel soloed, anything edited.
		{
			const auto& gm = GraphEditorExperimentHudRef().Model();
			j.StringField("graphMode", FilmmakerSettings_UiString("editor.graph", "camera"));
			j.IntField("graphSel", gm.SelectionCount());
			j.BoolField("graphSolo", gm.AnySolo());
			j.BoolField("graphUndo", gm.CanUndo());
		}
		j.BoolField("pathPlaying", CameraPathRef().IsPlaying());
		j.BoolField("freeCam", MovieModeRef().GetMode() == MovieMode::Mode::FreeCam);
		// In the player's eyes, no camera of ours over it: what the weapon lessons are seen through.
		j.BoolField("pov", MovieModeRef().GetMode() == MovieMode::Mode::Default && !CameraBridge_GetFreeCamEnabled());
		j.IntField("sceneRev", SceneKeys_Revision());
		j.BoolField("recoil", WeaponRecoilRef().Enabled());
		j.BoolField("bob", WeaponBobRef().Enabled());
		j.StringField("weapon", WeaponRecoilRef().LiveWeapon());
		bool demo = false, paused = false;
		if (auto pDemo = g_pEngineToClient ? g_pEngineToClient->GetDemoFile() : nullptr) {
			paused = pDemo->IsDemoPaused();
			demo = pDemo->IsPlayingDemo() || paused;
		}
		int tick = -1;
		if (!demo || !g_MirvTime.GetCurrentDemoTick(tick)) tick = -1;
		j.BoolField("demo", demo);
		j.BoolField("paused", paused);
		// A waiting lesson seek goes out once the demo is running: past its first ticks (a demo
		// that is still loading sits at tick 1), or after 4 s for one paused at its very start.
		if (st.seekTick >= 0 && demo && !DemoScrub_Active()) {
			const unsigned long long seekNow = GetTickCount64();
			if (!m_seekWaitSinceMs) m_seekWaitSinceMs = seekNow;
			if (tick > 8 || seekNow - m_seekWaitSinceMs > 4000) {
				DemoScrub_Release((double)st.seekTick, false);
				MvmDebugLog_LinefAlways("onboarding", "seek %d sent at tick %d (paused)", st.seekTick, tick);
				Onboarding_SeekIssued();
				m_seekWaitSinceMs = 0;
				m_pauseAfterMs = seekNow + 400; // the pause logic judges after the scrub, not before
			}
		} else if (st.seekTick < 0) m_seekWaitSinceMs = 0;
		// A lesson roll (`playto`): resume once, pause on the target tick.
		if (st.playToTick >= 0 && demo && g_pEngineToClient) {
			if (m_playToSent != st.playToTick) {
				m_playToSent = st.playToTick;
				m_playToSentMs = GetTickCount64();
				if (paused) g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			} else if (paused && tick + 2 < st.playToTick && GetTickCount64() - m_playToSentMs > 1500) {
				// The resume did not take (a seek was still landing): ask again.
				m_playToSentMs = GetTickCount64();
				g_pEngineToClient->ExecuteClientCmd(0, "demo_resume", true);
			} else if (tick >= st.playToTick || (paused && tick >= 0 && tick + 2 >= st.playToTick)) {
				if (!paused) g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
				m_playToSent = -1;
				Onboarding_PlayToDone();
				m_holdAfterMs = GetTickCount64() + 1000;
				MvmDebugLog_LinefAlways("onboarding", "playto reached tick %d", tick);
			}
		}
		// The hold: on a locked step that gives no way to play, the demo stays paused. Anything
		// that starts it anyway (a click on CS2's own play button, playback picking up after a
		// load) is paused again once it has played for a moment. A seek, a camera path playing
		// and a render are left alone.
		{
			const unsigned long long holdNow = GetTickCount64();
			const bool busy = DemoScrub_Active() || CameraPathRef().IsPlaying() || Render_IsJobActive();
			if (Onboarding_HoldsDemo() && demo && !paused && !busy && g_pEngineToClient) {
				if (!m_playingSinceMs) m_playingSinceMs = holdNow;
				if (holdNow - m_playingSinceMs >= 250 && holdNow >= m_holdAfterMs) {
					g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
					m_holdAfterMs = holdNow + 1000;
					MvmDebugLog_LinefAlways("onboarding", "hold: paused the demo (tick %d)", tick);
				}
			} else m_playingSinceMs = 0;
		}
		// demo_pause toggles, so it is sent only once the demo has settled: playing (a demo is
		// still loading for a while after the HUD appears), no seek in flight, and nothing moved
		// it for 400 ms. Measured: a seek can finish paused and the engine's own start of
		// playback then resumes it, and a toggle sent while the seek is finishing set it playing.
		const unsigned long long nowMs = GetTickCount64();
		if (m_pauseOnDemo && nowMs > m_pauseUntilMs) m_pauseOnDemo = false;
		if (m_pauseOnDemo && st.on && !st.closed && g_pEngineToClient) {
			// A loading demo reads as paused at tick 1 for seconds before the engine starts it, so
			// "paused" only counts once the demo has moved past its first ticks and held for 1.5 s.
			if (!demo || DemoScrub_Active()) { m_pauseAfterMs = nowMs + 400; m_pausedSinceMs = 0; m_freshPlayingSinceMs = 0; }
			else if (paused) {
				m_freshPlayingSinceMs = 0;
				if (tick <= 8) m_pausedSinceMs = 0;
				else if (!m_pausedSinceMs) m_pausedSinceMs = nowMs;
				else if (nowMs - m_pausedSinceMs >= 1500) m_pauseOnDemo = false;
			} else {
				m_pausedSinceMs = 0;
				// Playing for a moment, steadily: a seek that lands paused passes through "playing"
				// for a frame or two, and a toggle sent then sets it playing for good.
				if (!m_freshPlayingSinceMs) m_freshPlayingSinceMs = nowMs;
				if (nowMs >= m_pauseAfterMs && nowMs - m_freshPlayingSinceMs >= 300) {
					m_freshPlayingSinceMs = 0;
					g_pEngineToClient->ExecuteClientCmd(0, "demo_pause", true);
					m_pauseAfterMs = nowMs + 1000; // let the toggle land before judging again
					MvmDebugLog_LinefAlways("onboarding", "paused the demo for the tutorial (tick %d)", tick);
				}
			}
		}
		j.IntField("tick", tick);
		j.EndObject();
	}
	// With a workspace open the world is drawn shrunk into its preview box (screen fractions);
	// the markers have to be projected into that box, not onto the full screen.
	{
		float px0 = 0, py0 = 0, px1 = 1, py1 = 1;
		if (!inHud || !WorkspacePreviewRef().Active(px0, py0, px1, py1)) { px0 = 0; py0 = 0; px1 = 1; py1 = 1; }
		j.Key("pv"); j.BeginArray();
		j.ValueDouble(px0); j.ValueDouble(py0); j.ValueDouble(px1); j.ValueDouble(py1);
		j.EndArray();
	}
	// The rendered camera, so the panel can pin its "fly here" markers into the world and tell
	// how close the free camera is to each one. Only in game; the menu has no world to point at.
	{
		Environment::Matrix vp{};
		Environment::Vec3 eye{};
		if (inHud && Environment::RenderedCamera(vp, eye)) {
			j.Key("vp"); j.BeginArray();
			for (float v : vp) j.ValueDouble(v);
			j.EndArray();
			j.Key("eye"); j.BeginArray();
			j.ValueDouble(eye.x); j.ValueDouble(eye.y); j.ValueDouble(eye.z);
			j.EndArray();
		}
	}
	j.EndObject();

	if (j.Str() != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, j.Str().c_str());
		m_lastJson = j.Str();
	}
	// Per frame while he is up (typing, the clapper, blinking); one more frame after `off` lets
	// the script hide the root.
	if (st.on || m_wasOn)
		m_bridge.RunScriptCached("$.MvmOnboarding && $.MvmOnboarding.render();", "mvm/onboarding.render");
	m_wasOn = st.on;
}

} // namespace Filmmaker

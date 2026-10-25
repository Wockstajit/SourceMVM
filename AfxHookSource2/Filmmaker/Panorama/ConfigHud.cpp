#include "ConfigHud.h"
#include "../Props/Props.h"
#include "../Movie/SceneRig.h"
#include "../Movie/TimelineDock.h" // the dock this panel now hosts and can switch
#include "../Movie/CameraPath.h"   // key count for the dock ribbon's meta
#include "../Environment/EnvironmentScene.h"
#include "../Heatmap/RoundHeatmap.h" // ENVIRONMENT > Scene Objects: round heat map
#include "../AltEnvironment/AltEnvironment.h" // ENVIRONMENT > Map: the demo plays on another map

#include "PanoramaFindPanel.h"
#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Active / _LinefAlways (previewrect probe)

#include "ConfigHudJs.h"
#include "CameraTimelineHud.h"
#include "../Movie/CameraBridge.h" // OS-cursor pipe: relative slider drags (Shift = fine)
#include "MovieHud.h" // hidden while Config is open (F8 director card must not sit in the preview)
#include "AimCrosshairHud.h" // Smooth View page: honest recorded-aim reticle toggle
#include "../Movie/RecoilPivotMarker.h" // CAMERA tab: the two pivot marker toggles
#include "../Lens/LensFx.h" // CAMERA tab: depth of field + lens flare
#include "../Config/FilmmakerSettingsStore.h"
#include "InspectorWidth.h"
#include "../Config/EffectsPersistence.h" // EFFECTS footer: carry settings into the next demo
#include "../Movie/ViewFx.h" // MODIFIERS: weapon sway / deadzone levels
#include "../Movie/HandheldCam.h" // CAMERA tab: handheld drift + motion-curve layers
#include "../Movie/SmoothView.h" // CAMERA tab: Smooth View 2 camera module
#include "../Movie/CameraShakeStack.h" // CAMERA tab: stack-level aim lock
#include "../Movie/WeaponRecoil.h" // CAMERA tab: procedural viewmodel recoil
#include "../Movie/WeaponBob.h"   // CAMERA tab: the gun's walk cycle (sits under WEAPON RECOIL)
#include "../../ViewModel.h"      // CAMERA tab: VIEWMODEL card (per-player vs one universal)
#include "../../MirvCommands.h"   // CAMERA tab: VIEWMODEL card field of view (mirv_fov)
#include "../Movie/WeaponPivot.h" // CAMERA tab: the two per-weapon hinges (recoil, bob)
#include "../Movie/ParticleFx.h" // EFFECTS: per-category particle toggles
#include "../Movie/RagdollPhysics.h" // RAGDOLLS: gravity/friction/limit/collide
#include "../Movie/RagdollWeapon.h" // RAGDOLLS: held death weapon + visual discharge
#include "../Movie/DeathAnimations.h" // RAGDOLLS: ADVANCED RAGDOLLS card
#include "../Movie/NervousSystem.h" // RAGDOLLS: NERVOUS SYSTEM card
#include "../Movie/MethodActing.h" // RAGDOLLS: METHOD ACTING (living players; own module)
#include "../World/NightMode.h" // EFFECTS > LIGHTING: exposure / time-of-day
#include "../World/AutoExposure.h" // EFFECTS > LIGHTING: eye adaptation
#include "../World/SkyboxLibrary.h" // EFFECTS > WEATHER & SKY: skybox picker
#include "../World/CloudFx.h"       // EFFECTS > WEATHER & SKY: cloud density/size/drift
#include "../World/SkyboxBuilder.h" // EFFECTS > WEATHER & SKY: import
#include "../World/WorldModulation.h" // EFFECTS > ATMOSPHERE: geometry/world tint
#include "../World/LightFx.h"     // EFFECTS > LIGHTING: Sun tint
#include "../World/SunFx.h"       // runtime sun/material branch diagnostics
#include "../World/ShadowFx.h" // EFFECTS > LIGHTING: master engine-shadow switch
#include "../World/BakedShadowFx.h" // legacy keys / aliases
#include "../World/DynamicShadowFx.h"
#include "../World/CustomShadowFx.h" // Phase A HLSL contact shadows
#include "../World/SkyOrient.h"     // EFFECTS > WEATHER & SKY: live yaw / pitch / mirror
#include "../World/SmokeAppearance.h"
#include "../../MirvColors.h"   // EFFECTS > ATMOSPHERE: smoke colour state mirror
#include "../Render/RenderCommand.h"
#include "../Window/WindowResize.h"
#include "../Filmmaker.h" // CameraEditor_Active/Set (mutual exclusion), CameraEditor_HudViewName
#include "../MvmTest.h" // Insert live-test mode reuses this complete Camera + Effects panel
#include "../Movie/HudBlackout.h" // F8: clear every overlay

#include "../../ClientEntitySystem.h" // AfxGetSpectatedPawnIndex (POV-ready gate for the attach cams)
#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "WorkspacePreview.h" // the single owner of the scaled-preview viewport
#include "../../../Shared/AfxConsole.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


bool PlayingDemoRaw() {
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			// A PAUSED demo is still an active demo (see CameraEditorHud.cpp for the history:
			// IsPlayingDemo() alone flips false on pause and would force-exit the panel).
			return pDemo->IsPlayingDemo() || pDemo->IsDemoPaused();
	}
	return false;
}

// IsPlayingDemo()/IsDemoPaused() BOTH report false transiently during a demo_gototick seek --
// the same engine quirk the cosmetics store debounces. Reading that blip as "demo closed"
// dropped the workspace scale request (the preview zoomed to full screen for a few ticks on
// every scrub) and flipped the panel's JS layout to its live-insert variant, which could stick
// (fixed 2026-08-28). Only a SUSTAINED false means the demo is really gone.
bool PlayingDemo() {
	const bool raw = PlayingDemoRaw();
	static int s_falseFrames = 0;
	if (raw) { s_falseFrames = 0; return true; }
	if (s_falseFrames < 0x40000000) ++s_falseFrames;
	return s_falseFrames <= 30;
}

} // namespace

ConfigHud& ConfigHudRef() {
	static ConfigHud s_instance;
	return s_instance;
}

bool ConfigHud_Enabled() { return ConfigHudRef().Enabled(); }

bool ConfigHud_IsLive() { return ConfigHudRef().Enabled() && ConfigHudRef().IsLive(); }
bool ConfigHud_LiveCursor() { return ConfigHudRef().LiveCursor(); }
void ConfigHud_SetLiveCursor(bool on) { ConfigHudRef().SetLiveCursor(on); }
void ConfigHud_ToggleLiveCursor() { ConfigHudRef().SetLiveCursor(!ConfigHudRef().LiveCursor()); }

void ConfigHud_PushWheel(int notches) { ConfigHudRef().PushWheel(notches); }
void ConfigHud_PushChar(unsigned charCode) { ConfigHudRef().PushChar(charCode); }
void ConfigHud_PushKey(const char* name) { ConfigHudRef().PushKey(name); }
bool ConfigHud_TextCaptureActive() { return ConfigHudRef().TextCaptureActive(); }
bool ConfigHud_PopupOpen() { return ConfigHudRef().PopupOpen(); }
bool ConfigHud_MapViewOpen() { return ConfigHudRef().Enabled() && ConfigHudRef().MapViewOpen(); }
bool ConfigHud_ViewEditing() { return ConfigHudRef().ViewEditing(); }

bool ConfigHud_ScaledHud(float& x0, float& y0, float& x1, float& y1) {
	// Same committed state the Camera Editor's getter reads -- see WorkspacePreview.h. Kept as a
	// separate name only because the call sites read better that way; both now agree by
	// construction rather than by both panels happening to be right at the same moment.
	return WorkspacePreviewRef().Active(x0, y0, x1, y1);
}

void* ConfigHud::FindRoot() {
	void* ctx = m_bridge.ContextPanel();
	if (!ctx) return nullptr;
	return FindChildById(ctx, "ConfigHudRoot");
}

bool ConfigHud::BuildIfNeeded() {
	if (m_built)
		return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kConfigHudJs))
		return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symPreviewRect = m_bridge.MakeSymbol("previewrect");
	m_symTextCapture = m_bridge.MakeSymbol("textcapture");
	m_symPopupOpen = m_bridge.MakeSymbol("popupopen");
	m_symMapViewOpen = m_bridge.MakeSymbol("mapviewopen");
	m_symViewEdit = m_bridge.MakeSymbol("viewedit");
	m_root = FindRoot();
	m_built = (m_root != nullptr);
	m_lastState.clear();
	m_textCapture = false;
	m_popupOpen = false;
	m_mapViewOpen = false;
	m_viewEdit = false;
	return m_built;
}

void ConfigHud::Teardown() {
    Environment::StopPositionEditing();
    Props_StopEditing();
	if (m_built && m_hudPanel && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			// Close the death-animation picker FIRST: it holds a CaptureMouse deny-input handle that
			// would otherwise outlive its panel and keep the mouse away from the game.
			"(function(){if($.ConfigHudDap) try{$.ConfigHudDap.close();}catch(x){}"
			" var e=$('#ConfigHudRoot'); if(e) e.DeleteAsync(0);"
			// The Editor Settings fly-out is a TOP-LEVEL child of the context panel, so it does not
			// go with ConfigHudRoot. Same by-id teardown CameraEditorHud does for its own copy.
			" var s=$('#ConfigEditorSettingsRoot'); if(s) s.DeleteAsync(0);"
			// So is the popover overlay (tooltips, dropdowns, menus), for the same reason.
			" var o=$('#ConfigHudOverlayRoot'); if(o) o.DeleteAsync(0); $.ConfigHud=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_hudPanel = nullptr;
	m_lastState.clear();
	m_textCapture = false;
	m_popupOpen = false;
	m_mapViewOpen = false;
	m_viewEdit = false;
	{
		std::lock_guard<std::mutex> lk(m_charMutex);
		m_pendingChars.clear();
	}
}

void ConfigHud::OnEnter() {
	// LATCH which panel this is, once, at open time. Everything downstream keys off this rather
	// than calling PlayingDemo() per frame: that reading blips false during a demo_gototick seek
	// (see PlayingDemo above), and a panel that reclassified itself mid-seek would swap layout
	// AND cursor owner underneath the user.
	m_context = PlayingDemo() ? Context::Demo : Context::Live;

	// Start in UI-cursor mode so the panel is immediately clickable (same as the Camera Editor),
	// but each panel arms its OWN cursor. The live panel must not touch the camera timeline's
	// flag -- that is demo state, and borrowing it is what tied the two panels together.
	// The DEMO panel hosts the shared timeline dock; the live Insert panel does not
	// (there is no camera path or demo timeline to dock under a live match).
	if (!IsLive())
		TimelineDock_Claim(DockHost::Config);

	if (IsLive())
		m_liveCursor = true;
	else
		CameraTimelineHudRef().SetCursor(true);
	// Hide the floating movie-director card (the F8 CONTROLS panel) -- it would otherwise sit
	// inside the scaled preview. Restored to its previous visibility on exit (editor parity).
	m_prevMovieHud = MovieHudRef().Visible();
	MovieHudRef().SetVisible(false);
}

void ConfigHud::OnExit() {
	// Hand the mouse back to the game: the live panel clears its own flag.
	if (IsLive())
		m_liveCursor = false;
	// Parks the dock: the overlays must not outlive the workspace showing them.
	TimelineDock_Release(DockHost::Config);
	// The demo panel shares the timeline's UI-mouse mode. Once its dock is released the timeline
	// is hosted only if the Camera Editor owns it, and then the editor keeps the cursor. Otherwise
	// plain demo viewing has no UI mode (camtl cursor refuses it): left on, it made
	// WindowInputHooks skip every game click, so after leaving Config in UI mode -- the
	// ENVIRONMENT tab keeps the left button for editing -- neither button reached CS2.
	if (!IsLive() && !CameraTimelineHudRef().EditorHosted())
		CameraTimelineHudRef().SetCursor(false);
	// HudView is shared, persistent state -- CameraTimelineJs automatically restores the full
	// game HUD when neither the editor nor this panel is open, so nothing to reset here.
	MovieHudRef().SetVisible(m_prevMovieHud);
	// Drop any pending scaled-preview blit so the next full-screen frame renders normally.
	m_previewValid = false;
	m_demoScaleActive = false;
	WorkspacePreviewRef().Release(WorkspacePreview::Owner::Config);
	Teardown();
}

// Demo Config is a real workspace and uses the same scaled-preview bridge as the Camera
// Editor. Offline live-game Insert deliberately stays a sidebar overlay at full game size.
void ConfigHud::UpdateScaleRequest() {
	const bool demo = PlayingDemo();
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	bool valid = false;
	if (demo && m_root) {
		std::string pr = m_bridge.GetAttributeString(m_root, m_symPreviewRect, "");
		std::istringstream is(pr);
		const bool parsed = (bool)(is >> x0 >> y0 >> x1 >> y1);
		valid = parsed && (x1 - x0 > 0.01f) && (y1 - y0 > 0.01f)
			&& x0 >= 0 && y0 >= 0 && x1 <= 1.0001f && y1 <= 1.0001f;
	}
	m_demoScaleActive = demo && valid;
	m_previewValid = valid;
	if (valid) {
		m_previewX0 = x0; m_previewY0 = y0; m_previewX1 = x1; m_previewY1 = y1;
	}

	// ---- INVARIANT: when Config is active in a demo but the preview rect is stuck empty,
	//     the layout is broken — warn once per episode.
	static int s_emptyFrameCount = 0;
	static bool s_emptyWarned = false;
	if (demo && m_enabled) {
		if (!valid) {
			s_emptyFrameCount++;
			if (s_emptyFrameCount == 3 && !s_emptyWarned) {
				advancedfx::Warning("[confighud] INVARIANT: preview rect missing for %d consecutive frames "
					"while Config/demo active. Layout may be stuck.\n", s_emptyFrameCount);
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

	// Per-frame probe (throttled, change-gated). `previewrect` is published by the panel's JS
	// (layoutDemoWorkspace); an EMPTY raw string here means the JS never got that far -- on
	// 2026-08-28 a dangling `winFocusField` reference threw a ReferenceError on every render(),
	// so nothing after it ran and the world rendered full-screen with the inspector on top
	// (22805 JS exceptions vs 1 preview commit in the log). The console warning above says only
	// "missing"; this line says whether the ATTRIBUTE is empty (JS died / never laid out) or
	// merely unparseable/degenerate (a real layout maths bug), which is the difference between
	// reading the JS and reading the geometry.
	if (MvmDebugLog_Active()) {
		static std::string s_lastRaw = "\x01"; // impossible first value -> always logs once
		static bool s_lastValid = false, s_lastDemo = false, s_lastEnabled = false;
		std::string raw = (demo && m_root)
			? m_bridge.GetAttributeString(m_root, m_symPreviewRect, "") : std::string();
		if (raw != s_lastRaw || valid != s_lastValid || demo != s_lastDemo || m_enabled != s_lastEnabled) {
			s_lastRaw = raw; s_lastValid = valid; s_lastDemo = demo; s_lastEnabled = m_enabled;
			MvmDebugLog_LinefAlways("confighud.previewrect",
				"enabled=%d demo=%d root=%p rawAttr='%s' valid=%d rect=%.4f %.4f %.4f %.4f",
				m_enabled ? 1 : 0, demo ? 1 : 0, (void*)m_root,
				raw.empty() ? "(EMPTY - JS never published it)" : raw.c_str(),
				valid ? 1 : 0, x0, y0, x1, y1);
		}
	}
	// Submit rather than command -- WorkspacePreview arbitrates and issues the single request.
	if (m_demoScaleActive) WorkspacePreviewRef().Submit(WorkspacePreview::Owner::Config, x0, y0, x1, y1);
	else WorkspacePreviewRef().Release(WorkspacePreview::Owner::Config);
}

std::string ConfigHud::BuildStateJson() {
	std::ostringstream o;
	o << "{";
	// F8 blackout hides the sidebar without closing it (see CameraEditorHud for the reasoning).
	o << "\"enabled\":" << ((m_enabled && !HudBlackout_Active()) ? "true" : "false");
	// Report the LATCHED context, not a fresh PlayingDemo() reading: the JS picks its layout
	// from this, and a seek blip used to flip an open demo workspace to the live-insert variant
	// mid-scrub (and it could stick).
	o << ",\"demo\":" << (IsLive() ? "false" : "true");
	o << ",\"cursor\":" << ((IsLive() ? m_liveCursor : CameraTimelineHudRef().Cursor()) ? "true" : "false");
	// The OS-cursor pipe, same shape as CameraEditorHud publishes it.
	//
	// Panorama has no mouse-move event, so a RELATIVE drag -- which is the only way to offer
	// "Shift = finer" on a slider -- cannot be built from Panorama events at all; the native
	// Slider only ever maps the pointer's ABSOLUTE x onto its value. Publishing the raw cursor
	// here is what lets the CONFIG panel's ~68 sliders have the same fine-drag the Camera
	// Editor inspector already had, without a second input mechanism.
	{
		int cmx = 0, cmy = 0; bool clmb = false, crmb = false, cshift = false; unsigned cseq = 0;
		CameraBridge_GetUiCursor(cmx, cmy, clmb, crmb, cshift, cseq);
		o << ",\"mx\":" << cmx << ",\"my\":" << cmy << ",\"lmb\":" << (clmb ? "true" : "false");
		o << ",\"mseq\":" << cseq << ",\"shift\":" << (cshift ? "true" : "false");
		o << ",\"rmb\":" << (crmb ? "true" : "false");
		o << ",\"ctrl\":" << (CameraBridge_GetCtrlDown() ? "true" : "false");
	}
	o << ",\"hudView\":\"" << CameraEditor_HudViewName() << "\"";
	o << ",\"dockTab\":\"" << TimelineDock_TabName() << "\"";
	o << ",\"dockH\":" << TimelineDock_Height();
	// Same field name the editor publishes, so the shared ribbon reads one spelling.
	o << ",\"count\":" << (int)CameraPathRef().Markers().size();
	int winW = 0, winH = 0;
	WindowResize_GetClientSize(winW, winH);
	o << ",\"winW\":" << winW << ",\"winH\":" << winH;
	// Every stored UI key in one object. The collapsible cards restore themselves from this
	// generically (see cfgSections in ConfigHudWidgetsJs.h) rather than each needing a field here.
	FilmmakerSettings_AppendUiJson(o);
	// The operator-dragged inspector width, so this panel and the other two lay out to the
	// same column. One reader, one clamp -- see InspectorWidth.h.
	InspectorWidth_AppendJson(o);
	o << ",\"uiConfigTab\":\"" << FilmmakerSettings_UiString("config.tab", "camera") << "\"";
	o << ",\"uiConfigFxTab\":\"" << FilmmakerSettings_UiString("config.fx.tab", "vfx") << "\"";
	o << ",\"uiConfigSmoothTab\":\"" << FilmmakerSettings_UiString("config.smooth.tab", "general") << "\"";
	o << ",\"uiConfigSmoothOpen\":"
		<< (FilmmakerSettings_UiBool("config.section.smoothview", false) ? "true" : "false");
	o << ",\"uiConfigHandheldOpen\":"
		<< (FilmmakerSettings_UiBool("config.section.handheld", false) ? "true" : "false");
	// CAMERA tab: ViewFx viewmodel modifiers. This is only a read-only snapshot; JS writes each
	// setting back through the viewfx command dispatcher.
	o << ",\"swayPct\":" << ViewFxRef().WeaponSway().Intensity();
	o << ",\"deadzonePct\":" << ViewFxRef().DeadzoneAim().Intensity();
	o << ",\"dzXhair\":" << (ViewFxRef().DeadzoneAim().LockCrosshair() ? "true" : "false");
	o << ",\"floatTight\":" << ViewFxRef().WeaponFloat().Tightness();
	o << ",\"floatAmt\":" << ViewFxRef().WeaponFloat().Amount();
	// Fisheye lives on Smooth View's Effects page; it is its own
	// D3D pass with its own command, and Smooth View's Reset switches it off with the crosshair.
	o << ",\"fisheye\":" << (Fisheye_Enabled() ? "true" : "false");
	o << ",\"svOn\":" << (SmoothViewRef().Enabled() ? "true" : "false");
	o << ",\"svBreathX\":" << SmoothViewRef().BreathFreqX();
	o << ",\"svBreathY\":" << SmoothViewRef().BreathFreqY();
	o << ",\"svStandAmp\":" << SmoothViewRef().StandBreathAmp();
	o << ",\"svWalkAmp\":" << SmoothViewRef().WalkBreathAmp();
	o << ",\"svRunAmp\":" << SmoothViewRef().RunBreathAmp();
	o << ",\"svTiltEnabled\":" << (SmoothViewRef().TiltEnabled() ? "true" : "false");
	o << ",\"svTilt\":" << SmoothViewRef().TiltFactor();
	o << ",\"svTiltSpeed\":" << SmoothViewRef().TiltSmoothSpeed();
	o << ",\"svTiltInv\":" << (SmoothViewRef().TiltInvert() ? "true" : "false");
	o << ",\"svFalling\":" << (SmoothViewRef().FallingEnabled() ? "true" : "false");
	o << ",\"svJumping\":" << (SmoothViewRef().JumpingEnabled() ? "true" : "false");
	o << ",\"svHealthSway\":" << (SmoothViewRef().HealthSwayEnabled() ? "true" : "false");
	o << ",\"smoothViewDefault\":"
		<< (SmoothViewRef().SettingsAreDefault()
			&& !Fisheye_Enabled() ? "true" : "false");
	// Follow attach and third-person can only operate with a spectated player. In a GOTV demo
	// you start in free-roam, so the UI needs this state to explain why the action is unavailable
	// instead of appearing to ignore a click.
	o << ",\"povReady\":" << (m_povReady ? "true" : "false");
	o << ",\"renderOpen\":" << (Render_IsModalOpen() ? "true" : "false");
	o << ",\"renderJobActive\":" << (Render_IsJobActive() ? "true" : "false");
	// CAMERA tab, shake layers: each module appends its own fields (leading comma included), so
	// adding a knob is a one-line change in that module rather than an edit here.
	HandheldCamRef().AppendStateJson(o);
	Environment::AppendStateJson(o);
	Props_AppendStateJson(o);
	Heatmap::AppendStateJson(o);
	AltEnvironment::AppendStateJson(o);
	SceneRig_AppendStateJson(o);
	WeaponRecoilRef().AppendStateJson(o);
	WeaponBobRef().AppendStateJson(o);
	WeaponPivotRecoilRef().AppendStateJson(o, "pvr");
	WeaponPivotBobRef().AppendStateJson(o, "pvb");
	o << ",\"handheldDefault\":" << (HandheldCamRef().SettingsAreDefault() ? "true" : "false");
	// One orb flag per effect: each card's "Show Pivot Marker" pill governs only its own marker.
	o << ",\"wrPivotOrb\":"
		<< (RecoilPivotMarkerRef().Enabled(WeaponPivotOwner::Recoil) ? "true" : "false");
	o << ",\"wbPivotOrb\":"
		<< (RecoilPivotMarkerRef().Enabled(WeaponPivotOwner::Bob) ? "true" : "false");
	LensFx_AppendStateJson(o);
	o << ",\"wrPivotOrbShowing\":"
		<< (RecoilPivotMarkerRef().Showing(WeaponPivotOwner::Recoil) ? "true" : "false");
	o << ",\"wbPivotOrbShowing\":"
		<< (RecoilPivotMarkerRef().Showing(WeaponPivotOwner::Bob) ? "true" : "false");
	o << ",\"weaponRecoilDefault\":" << (WeaponRecoilRef().SettingsAreDefault() ? "true" : "false");
	o << ",\"weaponBobDefault\":" << (WeaponBobRef().SettingsAreDefault() ? "true" : "false");
	// CAMERA tab, VIEWMODEL card. `vmPerPlayer` is the switch the user reads: on means every
	// player keeps the viewmodel the game built for them, which is the inverse of the override.
	{
		const auto vm = Viewmodel_State();
		o << ",\"vmPerPlayer\":" << (vm.universal ? "false" : "true");
		o << ",\"vmX\":" << vm.offsetX << ",\"vmY\":" << vm.offsetY << ",\"vmZ\":" << vm.offsetZ;
		o << ",\"vmFov\":" << vm.fov;
		o << ",\"vmLeftHanded\":" << (vm.leftHanded ? "true" : "false");
		o << ",\"vmOwnX\":" << (vm.ownX ? "true" : "false")
		  << ",\"vmOwnY\":" << (vm.ownY ? "true" : "false")
		  << ",\"vmOwnZ\":" << (vm.ownZ ? "true" : "false")
		  << ",\"vmOwnFov\":" << (vm.ownFov ? "true" : "false")
		  << ",\"vmOwnLeftHanded\":" << (vm.ownLeftHanded ? "true" : "false");
		o << ",\"viewmodelDefault\":" << (Viewmodel_IsDefault() ? "true" : "false");
	}
	// VIEWMODEL card, Field of view: the shot's own FOV (mirv_fov), not the weapon's.
	// Whole degrees: the slider and the settings file both round, so the HUD must not reintroduce
	// a tenth that would then paint as 100.5 until the next drag.
	o << ",\"camFovOn\":" << (MirvFov_Enabled() ? "true" : "false");
	o << ",\"camFov\":" << (int)(MirvFov_Value() + (MirvFov_Value() >= 0.f ? 0.5f : -0.5f));
	// EFFECTS section: particle-effect toggles owned by ParticleFx.h -- read-only mirror here,
	// same contract as the MODIFIERS block above (JS writes back via console commands).
	{
		ParticleFx& fx = ParticleFxRef();
		o << ",\"fxOn\":" << (fx.Enabled() ? "true" : "false");
		o << ",\"fxReady\":" << (fx.Installed() && (!(fx.MollyLights() || fx.GrenadeLights(false) || fx.GrenadeLights(true)) || fx.MollyLightsReady()) ? "true" : "false");
		o << ",\"fxMoneyshot\":" << (fx.MoneyHeadshot() ? "true" : "false");
		o << ",\"fxFlashBlind\":" << (fx.FlashBlind() ? "true" : "false");
		o << ",\"fxMollyLights\":" << (fx.MollyLights() ? "true" : "false");
		o << ",\"fxHeLights\":" << (fx.GrenadeLights(false) ? "true" : "false");
		o << ",\"fxFlashLights\":" << (fx.GrenadeLights(true) ? "true" : "false");
		// Footer of every tab: does that tab carry into the next demo?
		o << ",\"keepCamera\":" << (EffectsPersistence_Keep("camera") ? "true" : "false");
		o << ",\"fxKeep\":" << (EffectsPersistence_Keep("effects") ? "true" : "false");
		o << ",\"keepRagdolls\":" << (EffectsPersistence_Keep("ragdolls") ? "true" : "false");
		// RAGDOLLS tab, PHYSICS card. Own module, own fields (leading comma included).
		RagdollPhysics_AppendStateJson(o);
		RagdollWeapon_AppendStateJson(o);
		DeathAnimations_AppendStateJson(o);
		NervousSystem_AppendStateJson(o);
		// RAGDOLLS tab, METHOD ACTING card. Acts on LIVING players, so it shares the tab with
		// the ragdoll cards but none of their code -- see Movie/MethodActing.h.
		MethodActing_AppendStateJson(o);
		// EFFECTS > LIGHTING. kExposureAuto (-1) means "engine auto-exposure"; the JS treats any
		// non-positive value as auto, so the sentinel needs no special encoding here.
		o << ",\"exposure\":" << NightMode_Exposure();
		o << ",\"exposureAdapt\":" << (AutoExposure_Enabled() ? "true" : "false");
		o << ",\"exposureSpeed\":" << AutoExposure_Speed();
		o << ",\"exposureCompensation\":" << AutoExposure_Compensation();
		o << ",\"exposureAdapted\":" << AutoExposure_CurrentScale();
		// EFFECTS > ATMOSPHERE. Mirrored from the existing mirv_colors backend rather than kept
		// as a second copy here -- see MirvColors.h.
		o << ",\"smokeT\":\"" << MirvColors_GetSmokeColorName(false) << "\"";
		o << ",\"smokeCT\":\"" << MirvColors_GetSmokeColorName(true) << "\"";
		{ std::string appearance; SmokeAppearance::AppendStateJson(appearance); o << appearance; }
		// EFFECTS > WEATHER & SKY. Appends skyList + skyCurrent. The list is read from disk once
		// and cached, so this is a cheap string build per frame, not a file read per frame.
		SkyboxLibrary_AppendStateJson(o);
		CloudFx_AppendStateJson(o);
		SkyboxBuilder_AppendStateJson(o);
		SkyOrient_AppendStateJson(o);
		LightFx_AppendStateJson(o);
		SunFx_AppendStateJson(o);
		ShadowFx_AppendStateJson(o);
		CustomShadowFx_AppendStateJson(o);
		WorldModulation_AppendStateJson(o);
		for (int c = 0; c < kFxCategoryCount; ++c)
			o << ",\"fx_" << FxCategoryKey((FxCategory)c) << "\":\""
			  << FxModeName(fx.Mode((FxCategory)c)) << "\"";
	}
	o << "}";
	return o.str();
}

void ConfigHud::RunFrame() {
	m_bridge.Init();

	// Mutual exclusion: the Camera Editor is the bigger workspace -- if it comes up while the
	// Config panel is open, the Config panel yields. (Opening Config closes the editor in the
	// command handler, so this only covers the editor-opened-later direction.)
	if (m_enabled && CameraEditor_Active())
		m_enabled = false;

	const bool supportedView = PlayingDemo() || MvmTest_CanUseMenu();
	unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel();

	// Demo not playing (or HUD gone): force-exit cleanly so we never leave a stale panel.
	if (!supportedView || !hud) {
		// Level teardown can destroy the old context before this frame. Releasing
		// cursor/preview ownership is still necessary, but scripts must only use
		// the current HUD, never the panel retained from the previous level.
		if (!hud || hud != m_hudPanel) {
			if (m_wasEnabled && MvmDebugLog_Active())
				MvmDebugLog_Linef("ui.config", "discard expired HUD old=%p current=%p", m_hudPanel, hud);
			m_bridge.SetContextPanel(nullptr);
			m_hudPanel = nullptr;
			m_root = nullptr;
		}
		if (m_wasEnabled) { m_enabled = false; OnExit(); m_wasEnabled = false; }
		m_built = false; m_root = nullptr; m_hudPanel = nullptr;
		m_bridge.SetContextPanel(nullptr);
		m_evalQueue.clear();
		return;
	}
	if (hud != m_hudPanel) { m_hudPanel = hud; m_built = false; }
	m_bridge.SetContextPanel(hud);

	// Live REPL (config eval), in this panel's context. Drained after the context panel is known
	// so a queued script always lands in the right place.
	if (!m_evalQueue.empty()) {
		for (const std::string& js : m_evalQueue)
			m_bridge.RunScript(js);
		m_evalQueue.clear();
	}
	// Death Animations picker data (clip catalog, preview frames), asked for by the picker itself.
	{
		std::string js;
		if (DeathAnimations_TakeScript(js)) m_bridge.RunScript(js);
	}

	// THE TWO PANELS ARE SEPARATE: a panel opened in one context does not survive into the
	// other. Without this, one shared `m_enabled` meant opening the live Insert panel and then
	// loading a demo (or leaving a demo for a live map) left it "open" in the other context's
	// layout, with the other context's cursor owner -- the class of bug where the two felt
	// wired together. Closing is the honest answer: the user re-opens the panel they want.
	//
	// Guarded on m_wasEnabled so this only judges a panel that is actually up, and it uses the
	// same debounced PlayingDemo() the rest of the file does, so a seek blip cannot trip it.
	if (m_enabled && m_wasEnabled) {
		const bool nowDemo = PlayingDemo();
		if (nowDemo == IsLive()) { // context changed under an open panel
			m_enabled = false;
			OnExit();
			m_wasEnabled = false;
			return;
		}
	}

	// Enter / exit edge transitions.
	if (m_enabled && !m_wasEnabled) { OnEnter(); m_wasEnabled = true; }
	else if (!m_enabled && m_wasEnabled) { OnExit(); m_wasEnabled = false; }

	if (!m_enabled)
		return;

	if (!BuildIfNeeded())
		return;

	m_root = FindRoot();
	if (!m_root) {
		m_built = false;
		return;
	}

	// Debounce the POV-ready check: AfxGetSpectatedPawnIndex can flicker between frames if the
	// spectator camera or observer target is momentarily ambiguous. Hold each reading for 15
	// consecutive frames before publishing, so the button never strobes "needs a player POV".
	{
		// AfxGetSpectatedUserId (not ...PawnIndex): it applies the same short hold that the
		// game-event consumers use, so the button and the effects agree about whether there is
		// a POV instead of disagreeing on the frames the raw match drops out.
		const bool raw = AfxGetSpectatedUserId() >= 0;
		if (raw == m_povReady) {
			if (m_povReadyHold < 15) ++m_povReadyHold;
		} else {
			if (m_povReadyHold > 0) --m_povReadyHold;
			else m_povReady = raw;
		}
	}

	std::string state = BuildStateJson();
	if (state != m_lastState) {
		m_bridge.SetAttributeString(m_root, m_symState, state.c_str());
		m_lastState = state;
	}
	// Demo render publishes a previewrect for the real scaled workspace. Live Insert clears it
	// and keeps the current full-size sidebar overlay.
	//
	// RE-CLAIM every frame, exactly as the Camera Editor does. The one-shot claim in OnEnter is
	// order-dependent: opening Config FROM the editor runs Config's OnEnter while the editor still
	// holds the dock (rank 1 < 2, so the claim fails) and the editor's OnExit then parks it -- and
	// without a per-frame re-claim nothing ever hosts the dock again, so this panel's REGULAR
	// TIMELINE / CAMERA / GRAPH tabs changed the highlight but showed nothing. Claiming here is
	// self-healing, and it still cannot steal the dock from an open editor (lower rank).
	if (!IsLive())
		TimelineDock_Claim(DockHost::Config);
	TimelineDock_Apply(DockHost::Config);
	m_bridge.RunScriptCached("$.ConfigHud && $.ConfigHud.render();", "mvm/confighud.render");
	UpdateTextCapture();
	DispatchChars();

	// After render(), so the column it scrolls has this frame's layout.
	DispatchWheel();

	UpdateScaleRequest();
}

// WndProc/input thread. Accumulates notches; the sign is all that matters, and coalescing them
// into one integer means a fast flick becomes one proportional scroll instead of a queue that
// drains over several frames.
void ConfigHud::PushWheel(int notches) {
	std::lock_guard<std::mutex> lk(m_wheelMutex);
	// Bound it so a stuck queue can never turn into an enormous jump.
	int next = m_pendingWheel + notches;
	if (next > 32) next = 32;
	if (next < -32) next = -32;
	m_pendingWheel = next;
}

void ConfigHud::PushChar(unsigned charCode) {
	std::lock_guard<std::mutex> lk(m_charMutex);
	if (m_pendingChars.size() < 128) m_pendingChars.push_back(charCode);
}

void ConfigHud::PushKey(const char* name) {
	std::lock_guard<std::mutex> lk(m_charMutex);
	if (m_pendingKeys.size() < 16) m_pendingKeys.push_back(name);
}

void ConfigHud::DispatchChars() {
	std::vector<unsigned> chars;
	std::vector<std::string> keys;
	{
		std::lock_guard<std::mutex> lk(m_charMutex);
		chars.swap(m_pendingChars);
		keys.swap(m_pendingKeys);
	}
	if (!m_root) return;
	for (const std::string& key : keys) {
		const std::string js = "$.ConfigHud && $.ConfigHud.key && $.ConfigHud.key('" + key + "');";
		m_bridge.RunScript(js.c_str());
	}
	if (chars.empty()) return;
	std::ostringstream o;
	o << "$.ConfigHud && $.ConfigHud.chars && $.ConfigHud.chars('";
	for (size_t i = 0; i < chars.size(); ++i) { if (i) o << ','; o << chars[i]; }
	o << "');";
	m_bridge.RunScript(o.str().c_str());
}

void ConfigHud::UpdateTextCapture() {
	m_textCapture = m_root && (m_bridge.GetAttributeString(m_root, m_symTextCapture, "0") == "1");
	// The MAP view (ConfigHudAltEnvironmentJs.h) covers the preview like a popup: its clicks, drags
	// and wheel are the view's, never the camera's.
	m_mapViewOpen = m_root && (m_bridge.GetAttributeString(m_root, m_symMapViewOpen, "0") == "1");
	m_popupOpen = m_mapViewOpen || (m_root && (m_bridge.GetAttributeString(m_root, m_symPopupOpen, "0") == "1"));
	m_viewEdit = m_root && (m_bridge.GetAttributeString(m_root, m_symViewEdit, "0") == "1");
}

// Main thread. Hands the accumulated notches to the panel's JS, which scrolls its own column
// (the inspector is a manual transform-scroller -- native overflow:scroll cannot work here
// because the wheel never reaches Panorama in this context).
void ConfigHud::DispatchWheel() {
	int notches = 0;
	{
		std::lock_guard<std::mutex> lk(m_wheelMutex);
		notches = m_pendingWheel;
		m_pendingWheel = 0;
	}
	if (0 == notches || !m_root) return;
	MvmDebugLog_Linef("config.input", "dispatch wheel=%d root=%p", notches, m_root);
	std::ostringstream o;
	o << "$.ConfigHud && $.ConfigHud.wheel && $.ConfigHud.wheel(" << notches << ");";
	m_bridge.RunScript(o.str().c_str());
}

void ConfigHud_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : "toggle";
	if (0 == _stricmp(arg, "state")) {
		advancedfx::Message("[confighud][state] %s\n", ConfigHudRef().DebugStateJson().c_str());
		return;
	}
	if (0 == _stricmp(arg, "viewmenu")) {
		// A quick right-click in the ENVIRONMENT view (MovieMode): the object under the pointer gets
		// its menu. The JS picks at the cursor and opens the Outliner's menu for it.
		ConfigHudRef().RequestEval("$.ConfigHud.viewMenu()");
		return;
	}
	if (0 == _stricmp(arg, "eval")) {
		if (argc < 4) { advancedfx::Warning("usage: %s config eval <panorama js>\n", cmd); return; }
		std::string js;
		for (int i = 3; i < argc; ++i) { if (i > 3) js += ' '; js += args->ArgV(i); }
		ConfigHudRef().RequestEval(js);
		return;
	}
	if (0 == _stricmp(arg, "on") || 0 == _stricmp(arg, "open") || 0 == _stricmp(arg, "1")) ConfigHudRef().SetEnabled(true);
	else if (0 == _stricmp(arg, "off") || 0 == _stricmp(arg, "close") || 0 == _stricmp(arg, "0")) ConfigHudRef().SetEnabled(false);
	else ConfigHudRef().Toggle();
	// Opening Config closes the Camera Editor (they draw the same right-side column area).
	if (ConfigHudRef().Enabled() && CameraEditor_Active())
		CameraEditor_Set(false);
	advancedfx::Message("mirv_filmmaker: config panel %s (demo or offline mvm_test map).\n",
		ConfigHudRef().Enabled() ? "ON" : "off");
}

} // namespace Filmmaker

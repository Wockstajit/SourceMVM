#include "DemoBarButtons.h"

#include "PanoramaFindPanel.h"

#include "DemoBarButtonsJs.h"
#include "DemoBarScrubJs.h"

#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel + PanoramaUIPanel offsets
#include "../Movie/CameraBridge.h" // CameraBridge_GetUiCursor (raw drag input for the scrubber)
#include "../Movie/DemoTimescale.h" // one demo speed, shared by all three scrubbers

#include <string>

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {


} // namespace

DemoBarButtons& DemoBarButtonsRef() {
	static DemoBarButtons s_instance;
	return s_instance;
}

bool DemoBarButtons::Inject() {
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel())
		return false;
	if (!m_bridge.RunScript(kDemoBarButtonsJs))
		return false;
	// Live scrubbing on the native slider: independent of the speed row, so its result is not
	// part of "injected" (Movie/DemoScrub.h).
	m_bridge.RunScript(kDemoBarScrubJs);
	// Confirm our row actually got created. If the native panel structure
	// changed and the JS couldn't find #SpeedControls, this stays false and we
	// retry next frame (native dropdown remains usable in the meantime).
	return FindChildById(m_bridge.ContextPanel(), "MirvSpeedRow") != nullptr;
}

void DemoBarButtons::RestoreNative() {
	// Delete our button row and un-hide the native dropdown. Only run while the
	// context panel is valid (a demo is playing); the caller guarantees that.
	if (m_injected && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var c=$.GetContextPanel();"
			"var r=c.FindChildTraverse('MirvSpeedRow'); if(r) r.DeleteAsync(0);"
			"var t=c.FindChildTraverse('TimeScale'); if(t) t.visible=true;"
			"var s=c.FindChildTraverse('SpeedSliderContainer'); if(s) s.visible=true;})();");
	}
	m_injected = false;
	m_anchor = nullptr;
}

void DemoBarButtons::RunFrame() {
	m_bridge.Init(); // resolve engine + RunScript (idempotent)

	bool playingDemo = false;
	if (g_pEngineToClient) {
		if (auto pDemo = g_pEngineToClient->GetDemoFile())
			playingDemo = pDemo->IsPlayingDemo();
	}
	if (!playingDemo) {
		// Demo gone -> the native bar is destroyed with the HUD; just drop state
		// (no script: the context panel may already be invalid).
		m_injected = false;
		m_anchor = nullptr;
		m_hudPanel = nullptr;
		return;
	}

	unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel();
	if (!hud) {
		m_injected = false;
		m_anchor = nullptr;
		m_hudPanel = nullptr;
		return;
	}
	if (hud != m_hudPanel) { // HUD (re)created -> reinject against the new context
		m_hudPanel = hud;
		m_injected = false;
		m_anchor = nullptr;
	}
	m_bridge.SetContextPanel(hud);

	if (!m_enabled) { // toggled off while a demo plays -> restore native dropdown
		RestoreNative();
		return;
	}

	// Locate the native speed-controls container. Until the demo bar is built,
	// do nothing -> the native dropdown stays fully functional (graceful fallback).
	void* anchor = FindChildById(hud, "SpeedControls");
	if (!anchor) {
		m_injected = false;
		m_anchor = nullptr;
		return;
	}
	if (anchor != m_anchor) { // demo bar (re)built -> (re)inject
		m_anchor = anchor;
		m_injected = false;
	}

	if (!m_injected) {
		m_injected = Inject();
		if (m_injected && m_symCursor < 0)
			m_symCursor = m_bridge.MakeSymbol("mirvcursor");
	}
	if (m_injected)
		PublishCursor(hud);
}

// Hand the injected script the raw OS cursor, once per frame.
//
// WHY NOT A SLIDER. The scrubber used a native Panorama Slider as its drag mechanism. That made
// the control feel delayed and intermittently "glitchy" on fast drags for two compounding
// reasons: CS2's slider eases its own thumb, so `sl.value` already trails the pointer, and the
// value only arrives on SliderValueChanged, whose cadence is not the frame rate -- so a quick
// flick delivered a few coarse jumps instead of a smooth sweep. Reading the cursor directly makes
// the drag exact and frame-locked.
//
// Coordinates are CLIENT device pixels (CameraBridge_GetUiCursor runs ScreenToClient), which is
// the same space Panorama's GetPositionWithinWindow() reports in -- so the script compares them
// without any uiscale conversion.
void DemoBarButtons::PublishCursor(void* hud) {
	if (m_symCursor < 0)
		return;
	// LOOK IT UP EVERY FRAME -- never cache the panel across frames.
	//
	// Anything else can replace #MirvSpeedRow without going through this class: the live-reload
	// tool (`mirv_filmmaker livejs`) re-runs the injected script, which deletes the old row and
	// builds a new one. A cached pointer then refers to freed memory and this function writes an
	// attribute straight into it every frame -- a native fault, so the process exits with no crash
	// dump. That was reproducible: `speedbar off/on` (which goes through this class and resets the
	// cache) survived repeatedly, while the FIRST livejs re-injection killed it every time.
	//
	// The lookup is a tree walk, but RunFrame already does one for "SpeedControls" each frame, so
	// this is the same order of cost and buys correctness against any external replacement.
	void* row = FindChildById(hud, "MirvSpeedRow");
	if (!row)
		return;
	int x = 0, y = 0; bool lmb = false, rmb = false, shift = false; unsigned seq = 0;
	CameraBridge_GetUiCursor(x, y, lmb, rmb, shift, seq);
	// Field 5 is the engine's demo speed, appended to this same per-frame string rather than given
	// its own attribute: the script already parses this one every frame, and the two values are
	// read together. It is here because the SAME scrubber is also mounted in the camera timeline
	// and the graph editor, and all three have to show one speed -- Panorama's own
	// GetDemoControllerState() is not available on this context panel (Movie/DemoTimescale.h).
	char buf[128];
	_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%d %d %d %u %g", x, y, lmb ? 1 : 0, seq,
		DemoTimescale_Get());
	m_bridge.SetAttributeString(row, m_symCursor, buf);
}

} // namespace Filmmaker

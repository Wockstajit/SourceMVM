#include "EasterEggOverlayHud.h"

#include "EasterEggOverlayJs.h"
#include "PanoramaFindPanel.h"

#include "../EasterEggs/EasterEggs.h"
#include "../EasterEggs/Osu/OsuGame.h"
#include "../EasterEggs/EasterEggSnake.h"
#include "../Debug/MvmDebugLog.h"
#include "../../DeathMsg.h" // AfxHookSource2_GetPanoramaHudPanel / MainMenuPanel
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

EasterEggOverlayHud& EasterEggOverlayHudRef() {
	static EasterEggOverlayHud s_instance;
	return s_instance;
}

bool EasterEggOverlayHud::BuildIfNeeded() {
	if (m_built) return true;
	if (!m_bridge.CanRunScript() || !m_bridge.ContextPanel()) return false;
	if (!m_bridge.RunScript(kEasterEggOverlayJs)) return false;
	m_symState = m_bridge.MakeSymbol("state");
	m_symOsuFrame = m_bridge.MakeSymbol("osuframe");
	m_symOsuObjs = m_bridge.MakeSymbol("osuobjs");
	m_symOsuList = m_bridge.MakeSymbol("osulist");
	m_symSnake = m_bridge.MakeSymbol("snake");
	// A fresh root has none of the osu attributes yet: resend everything.
	m_osuObjsRev = m_osuListRev = 0; m_osuWasActive = true;
	m_root = FindChildById(m_bridge.ContextPanel(), "MvmEggRoot");
	m_built = (m_root != nullptr);
	m_lastJson.clear();
	return m_built;
}

void EasterEggOverlayHud::Teardown() {
	if (m_built && m_context && m_bridge.ContextPanel()) {
		m_bridge.RunScript(
			"(function(){var e=$('#MvmEggRoot'); if(e) e.DeleteAsync(0); if($.MvmEggs&&$.MvmEggs.unwash) $.MvmEggs.unwash(); $.MvmEggs=null;})();");
	}
	m_built = false;
	m_root = nullptr;
	m_context = nullptr;
	m_lastJson.clear();
}

void EasterEggOverlayHud::RunFrame() {
	m_bridge.Init();

	// CS2 keeps a HUD pointer at the main menu, but that tree has a 0x0 layout.
	// Pointer existence does not mean a map is loaded or the HUD can display the overlay.
	unsigned char* hud = AfxHookSource2_GetPanoramaHudPanel();
	unsigned char* menu = AfxHookSource2_GetPanoramaMainMenuPanel();
	const char* level = g_pEngineToClient ? g_pEngineToClient->GetLevelNameShort() : nullptr;
	// Retail CS2 reports the literal "<empty>" for its menu's local background server.
	const bool loaded = level && level[0] && std::strcmp(level, "<empty>") != 0;
	unsigned char* ctx = loaded && hud ? hud : menu;
	if (!ctx) { m_built = false; m_root = nullptr; m_context = nullptr; return; }
	if (ctx != m_context) {
		// Menu and HUD can both survive the transition. Remove our old overlay before
		// creating another, but never execute a script against an expired context.
		if (m_context && (m_context == hud || m_context == menu)) Teardown();
		m_built = false; m_root = nullptr; m_context = ctx;
		MvmDebugLog_LinefAlways("egg.overlay", "context=%s panel=%p level=%s",
			ctx == hud ? "hud" : "menu", ctx, level ? level : "");
	}
	m_bridge.SetContextPanel(ctx);
	if (!BuildIfNeeded()) return;

	m_root = FindChildById(ctx, "MvmEggRoot");
	if (!m_root) { m_built = false; return; }

	std::string json;
	bool animating = EasterEggs_BuildOverlayJson(json);
	if (json != m_lastJson) {
		m_bridge.SetAttributeString(m_root, m_symState, json.c_str());
		m_lastJson = json;
	}
	// osu!: the frame every frame while open (and once more as it closes), the object window and
	// the library only when their revisions move.
	const bool osu = Osu_Active();
	if (osu || m_osuWasActive) {
		uint64_t objsRev = m_osuObjsRev, listRev = m_osuListRev;
		Osu_BuildJson(m_osuFrame, m_osuObjs, objsRev, m_osuList, listRev);
		m_bridge.SetAttributeString(m_root, m_symOsuFrame, m_osuFrame.c_str());
		if (objsRev != m_osuObjsRev) { m_bridge.SetAttributeString(m_root, m_symOsuObjs, m_osuObjs.c_str()); m_osuObjsRev = objsRev; }
		if (listRev != m_osuListRev) { m_bridge.SetAttributeString(m_root, m_symOsuList, m_osuList.c_str()); m_osuListRev = listRev; }
		animating = true;
	}
	m_osuWasActive = osu;
	// Snake: its state every frame while the board is up (and once more as it closes).
	const bool snake = Snake_Active();
	if (snake || m_snakeWasActive) {
		Snake_BuildJson(m_snakeJson);
		m_bridge.SetAttributeString(m_root, m_symSnake, m_snakeJson.c_str());
		animating = true;
	}
	m_snakeWasActive = snake;
	// Per-frame only while something moves; one extra frame after it stops lets the JS settle
	// (fade the last sparkle, hide the layer).
	if (animating || m_animating)
		m_bridge.RunScriptCached("$.MvmEggs && $.MvmEggs.render();", "mvm/eggs.render");
	m_animating = animating;
}

} // namespace Filmmaker

#include "Localization.h"

#include "LocalizationLanguages.h"
#include "../Config/FilmmakerSettingsStore.h"
#include "../Debug/MvmDebugLog.h"
#include "../Input/Keybinds.h"
#include "../Panorama/PanoramaBridge.h"
#include "../Panorama/PanoramaFindPanel.h"
#include "../Panorama/SettingsTabJs.h"
#include "../Panorama/UiLayout.h"

#include "../../DeathMsg.h"
#include "../../../Shared/AfxConsole.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cctype>
#include <cstring>
#include <functional>
#include <string>

namespace Filmmaker {
namespace {

// The runtime fragment, wrapped so it can also be run on its own before a table push.
const char* const kRuntimeOpen = "(function () {\n"
#include "../Panorama/LocalizationJs.h"
	;

constexpr const char* kSettingKey = "language";
constexpr unsigned kProbeEveryFrames = 60;

// One Panorama context (menu or HUD): the panel scripts run in, and what is known to be in it.
struct Context {
	const char* name;
	PanoramaBridge bridge;
	void* panel = nullptr;
	short symStamp = -1;
	short symGameLanguage = -1;
	unsigned frames = 0;
	std::string pushedStamp;
};

Context g_menu{ "menu" };
Context g_hud{ "hud" };

std::string g_setting = "auto";       // as stored
std::string g_gameLanguage;           // "" until Panorama told us
std::string g_active = "english";     // the table in use
std::string g_tableJson = "{}";       // that table's JSON text
std::string g_tableStamp = "english:0";   // language, table size and key names: what a context holds
std::string g_keyNames = "{}";            // Keybinds_NamesJson, pushed with the table
uint64_t g_epoch = 1;                 // bumped by Resolve the moment the language or a key name changes
// What the panel drivers see. It follows g_epoch once the old panel roots are gone (PumpRebuild),
// so a driver that rebuilds never finds the root it is replacing.
uint64_t g_publicEpoch = 1;
int g_rebuildFrames = -1;             // frames since the roots were deleted, or -1 when idle
bool g_resolved = false;

// The settings tab (menu context only).
void* g_tabContext = nullptr;
unsigned g_tabFrames = 0;
std::string g_tabState;

std::string UpperAscii(const char* text) {
	std::string out(text);
	for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
	return out;
}

// The table for `id` out of this DLL's resources (LocalizationTables.rc names them
// MVMLANG_<ID>), or false when there is none: English, or a build without the tables.
bool LoadTable(const char* id, std::string& out) {
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&LoadTable), &module) || !module)
		return false;
	const std::string name = "MVMLANG_" + UpperAscii(id);
	HRSRC resource = FindResourceA(module, name.c_str(), MAKEINTRESOURCEA(10) /* RT_RCDATA */);
	if (!resource) return false;
	HGLOBAL handle = LoadResource(module, resource);
	const DWORD size = SizeofResource(module, resource);
	const char* data = handle ? static_cast<const char*>(LockResource(handle)) : nullptr;
	if (!data || size < 2) return false;
	size_t skip = (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF) ? 3 : 0;   // BOM
	out.assign(data + skip, size - skip);
	return true;
}

// Picks the active language from the setting and the game's language, and loads its table. A
// change bumps the epoch so every panel rebuilds.
void Resolve() {
	g_setting = FilmmakerSettings_UiString(kSettingKey, "auto");
	const LocalizationLanguage* language = nullptr;
	if (g_setting != "auto") language = LocalizationLanguage_Find(g_setting.c_str());
	if (!language) {
		if (g_setting != "auto") g_setting = "auto";
		language = LocalizationLanguage_Find(g_gameLanguage.c_str());
	}
	const char* id = language ? language->id : "english";
	// A rebound shortcut changes the key names the panels print, so it rebuilds them the way a
	// language change does.
	const std::string keyNames = Keybinds_NamesJson();
	if (g_resolved && g_active == id) {
		if (keyNames == g_keyNames) return;
		g_keyNames = keyNames;
		g_tableStamp = g_active + ":" + std::to_string(g_tableJson.size()) + ":" + std::to_string(std::hash<std::string>{}(g_keyNames));
		++g_epoch;
		return;
	}
	g_keyNames = keyNames;

	std::string table;
	const bool loaded = 0 != std::strcmp(id, "english") && LoadTable(id, table);
	g_active = id;
	g_tableJson = loaded ? table : "{}";
	g_tableStamp = g_active + ":" + std::to_string(g_tableJson.size()) + ":" + std::to_string(std::hash<std::string>{}(g_keyNames));
	if (g_resolved) ++g_epoch;
	g_resolved = true;
	if (MvmDebugLog_Active()) {
		MvmDebugLog_Linef("i18n", "resolve setting=%s game=%s active=%s tableBytes=%zu loaded=%d epoch=%llu",
			g_setting.c_str(), g_gameLanguage.c_str(), g_active.c_str(), g_tableJson.size(),
			loaded ? 1 : 0, static_cast<unsigned long long>(g_epoch));
	}
}

// Asks the context what it holds (the game's language, and the stamp of the table its script
// global carries) and pushes the active table when that is not ours. Cheap enough to repeat: the
// probe is one short script, and it is what notices a context reload that kept the panel pointer
// but threw the script globals away.
void PumpContext(Context& context, void* panel) {
	if (!panel) { context.panel = nullptr; context.pushedStamp.clear(); context.frames = 0; return; }
	context.bridge.Init();
	context.bridge.SetContextPanel(panel);
	if (!context.bridge.CanRunScript()) return;

	const bool changed = panel != context.panel;
	context.panel = panel;
	if (changed) { context.pushedStamp.clear(); context.frames = 0; }
	const bool stale = context.pushedStamp != g_tableStamp;
	if (!changed && !stale && (++context.frames % kProbeEveryFrames) != 0) return;

	if (context.symStamp < 0) {
		context.symStamp = context.bridge.MakeSymbol("mvmi18nstamp");
		context.symGameLanguage = context.bridge.MakeSymbol("mvmgamelanguage");
	}
	if (!context.bridge.RunScript(
			"(function(){var p=$.GetContextPanel();"
			"p.SetAttributeString('mvmi18nstamp',($.MvmI18n&&$.MvmI18n.stamp)||'');"
			"var l='';try{l=''+$.Language();}catch(e){}"
			"p.SetAttributeString('mvmgamelanguage',l);})();"))
		return;

	if (g_gameLanguage.empty()) {
		const std::string reported = context.bridge.GetAttributeString(panel, context.symGameLanguage, "");
		if (!reported.empty()) { g_gameLanguage = reported; Resolve(); }
	}
	const std::string inContext = context.bridge.GetAttributeString(panel, context.symStamp, "");
	if (inContext == g_tableStamp) { context.pushedStamp = g_tableStamp; return; }

	std::string script;
	script.reserve(g_tableJson.size() + 8192);
	script += kRuntimeOpen;
	script += "$.MvmI18n.set(\"";
	script += g_active;
	script += "\", ";
	script += g_tableJson;
	script += ", \"";
	script += g_tableStamp;
	script += "\", ";
	script += g_keyNames;
	script += ");\n})();";
	const bool ok = context.bridge.RunScript(script);
	if (ok) context.pushedStamp = g_tableStamp;
	if (MvmDebugLog_Active()) {
		MvmDebugLog_Linef("i18n", "push context=%s panel=%p lang=%s stamp=%s had=\"%s\" ok=%d",
			context.name, panel, g_active.c_str(), g_tableStamp.c_str(), inContext.c_str(), ok ? 1 : 0);
	}
}

// A panel driver that rebuilds re-runs its script, and the script deletes its own old root first.
// That delete is asynchronous: until it lands two panels carry the id, the driver's lookup returns
// the OLD one, pushes the panel state to it, and the new root never gets it. The header bar and
// the director readout came back hidden or blank after a language change that way (measured in
// game). So the roots are deleted here first, and the drivers are told in the frame the engine
// reports them gone: each then builds exactly once, into a context with no old root in it.
// Telling them on a fixed delay was not enough: a driver that noticed its root missing rebuilt on
// its own, and the late epoch rebuilt it a second time straight into the same race.
// The ids are the ones each driver looks its root up by.
const char* const kRebuildRoots[] = {
	"AimXhairRoot", "CamEditorRoot", "CamTimelineRoot", "ConfigHudRoot", "MvmEggRoot", "FxDebugHudRoot",
	"GraphExpRoot", "MarkerHudRoot", "MovieHudRoot", "MvmOnboardingRoot", "MvmHeaderRoot",
	"FilmmakerMenuTab", "MvmPlayMenuHost", "MvmCommunityHost",
};

bool AnyRebuildRootLeft(void* contextPanel) {
	if (!contextPanel) return false;
	for (const char* id : kRebuildRoots) if (FindChildById(contextPanel, id)) return true;
	return false;
}

void PumpRebuild() {
	if (g_publicEpoch == g_epoch) { g_rebuildFrames = -1; return; }
	if (g_rebuildFrames < 0) {
		std::string script = "(function(){var ids=[";
		for (const char* id : kRebuildRoots) { script += '\''; script += id; script += "',"; }
		script += "];for(var i=0;i<ids.length;i++){try{var p=$('#'+ids[i]);if(p)p.DeleteAsync(0);}catch(e){}}})();";
		if (g_menu.panel && g_menu.bridge.CanRunScript()) g_menu.bridge.RunScript(script);
		if (g_hud.panel && g_hud.bridge.CanRunScript()) g_hud.bridge.RunScript(script);
		g_rebuildFrames = 0;
		if (MvmDebugLog_Active()) MvmDebugLog_Linef("i18n", "rebuild: roots deleted for epoch %llu", static_cast<unsigned long long>(g_epoch));
		return;
	}
	// Normally the next frame. The cap is for a panel something else keeps alive: better a late
	// rebuild than none.
	++g_rebuildFrames;
	if (g_rebuildFrames < 30 && (AnyRebuildRootLeft(g_menu.panel) || AnyRebuildRootLeft(g_hud.panel))) return;
	if (MvmDebugLog_Active()) MvmDebugLog_Linef("i18n", "rebuild: drivers told after %d frame(s)", g_rebuildFrames);
	g_publicEpoch = g_epoch;
	g_rebuildFrames = -1;
}

std::string TabStateJson() {
	std::string json = "{\"language\":\"" + g_setting + "\",\"gameLanguage\":\""
		+ (LocalizationLanguage_Find(g_gameLanguage.c_str()) ? LocalizationLanguage_Find(g_gameLanguage.c_str())->id : "english")
		+ "\",\"layout\":\"" + (UiLayout_Modular() ? "modular" : "workspace") + "\",\"binds\":" + Keybinds_PageJson()
		+ ",\"languages\":[";
	for (size_t i = 0; i < kLocalizationLanguageCount; ++i) {
		if (i) json += ',';
		json += "[\"";
		json += kLocalizationLanguages[i].id;
		json += "\",\"";
		json += kLocalizationLanguages[i].name;   // native names: no quotes or backslashes in them
		json += "\"]";
	}
	json += "]}";
	return json;
}

// The SOURCE:MVM tab lives in CS2's settings menu, which is part of the main-menu tree at the
// main menu and in a map (there it is the pause menu). The script is idempotent and re-ticks
// itself; it is re-run on a context change and now and then to repair a discarded script global.
void PumpSettingsTab() {
	if (!g_menu.panel || !g_menu.bridge.CanRunScript()) { g_tabContext = nullptr; g_tabFrames = 0; return; }
	const bool changed = g_menu.panel != g_tabContext;
	g_tabContext = g_menu.panel;
	++g_tabFrames;
	const bool repair = changed || (g_tabFrames % 120) == 0;
	if (repair && !g_menu.bridge.RunScript(kSettingsTabJs)) return;

	const std::string state = TabStateJson();
	if (!repair && state == g_tabState) return;
	g_tabState = state;
	g_menu.bridge.RunScript("$.MvmSettingsTab && $.MvmSettingsTab.setState(" + state + ");");
}

} // namespace

void Localization_RunMainThreadFrame() {
	Keybinds_MainThreadFrame();
	Resolve();
	PumpContext(g_menu, AfxHookSource2_GetPanoramaMainMenuPanel());
	PumpContext(g_hud, AfxHookSource2_GetPanoramaHudPanel());
	PumpRebuild();
	PumpSettingsTab();
}

bool Localization_TakeUiEpoch(uint64_t& seen) {
	if (seen == g_publicEpoch) return false;
	const bool first = seen == 0;
	seen = g_publicEpoch;
	return !first;
}

const char* Localization_ActiveLanguage() { return g_active.c_str(); }

void Localization_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* arg = (argc >= 3) ? args->ArgV(2) : nullptr;
	if (!arg) {
		advancedfx::Message("%s language is \"%s\" (showing %s, CS2 runs in %s).\nusage: %s language <auto|id>\nids:",
			cmd, g_setting.c_str(), g_active.c_str(), g_gameLanguage.empty() ? "?" : g_gameLanguage.c_str(), cmd);
		for (const LocalizationLanguage& language : kLocalizationLanguages) advancedfx::Message(" %s", language.id);
		advancedfx::Message("\n");
		return;
	}
	std::string value;
	if (0 == _stricmp(arg, "auto")) value = "auto";
	else if (const LocalizationLanguage* language = LocalizationLanguage_Find(arg)) value = language->id;
	else {
		advancedfx::Warning("%s language: unknown \"%s\". Run \"%s language\" for the list.\n", cmd, arg, cmd);
		return;
	}
	FilmmakerSettings_SetUi(kSettingKey, value.c_str());
	Resolve();
	advancedfx::Message("mirv_filmmaker: language = %s (showing %s).\n", g_setting.c_str(), g_active.c_str());
}

} // namespace Filmmaker

#include "GameplayHudLook.h"

#include "../Config/FilmmakerSettingsStore.h" // hud.viewerCvars: the viewer's values, across a quit or crash
#include "../Debug/MvmDebugLog.h"

#include "../../WrpConsole.h"

#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <cstdio>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

// The spectator-only differences, each with the value a playing player effectively has. The
// descriptions are CS2's own help text for these variables.
struct LookCvar {
	const char* name;
	const char* playerValue;
	std::string saved; // the viewer's value, read on the on edge; empty = could not read it
};

LookCvar g_cvars[] = {
	// "If set, the radar will be square when spectating." Off gives the round radar a player has.
	{ "cl_radar_square_when_spectating", "0", {} },
	// "Set all players visible on radar when spectating, regardless of whether they have been
	// spotted." Off leaves only the enemies the team spotted, as a player sees them.
	{ "cl_radar_show_all_players_when_spectating", "0", {} },
	// "Show the crosshair of the player being observed. 0 = off, 1 = friends and party only,
	// 2 = everyone." A demo's players are rarely the viewer's friends, so 2.
	{ "cl_show_observer_crosshair", "2", {} },
};

bool g_applied = false;

// A restore goes through the console buffer, so it lands a frame or more after Restore() asks for
// it, and until then the variables still read as the player values. A view that comes back inside
// that window must keep the viewer's values it already holds; reading them again would save the
// player values as the viewer's own and lose the viewer's settings on the next restore.
constexpr int kRestoreSettleFrames = 30;
int g_restorePending = 0; // frames left to wait for the last restore to show; 0 = settled

void ExecClientCmd(const std::string& cmd) {
	if (g_pEngineToClient)
		g_pEngineToClient->ExecuteClientCmd(0, cmd.c_str(), true);
}

// The current value as console text, for the integer and bool variables above. Empty when the
// variable is missing (a CS2 update renamed it) or of a type this does not read: then nothing is
// restored for it, rather than writing back a guess.
std::string ReadCvarText(const char* name) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return {};
	Cvar_s* c = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!c) return {};
	char buf[32];
	switch (c->m_eVarType) {
	case EConVarType_Bool:   return c->m_Value.m_bValue ? "1" : "0";
	case EConVarType_Int16:  snprintf(buf, sizeof(buf), "%d", (int)c->m_Value.m_i16Value); return buf;
	case EConVarType_UInt16: snprintf(buf, sizeof(buf), "%u", (unsigned)c->m_Value.m_u16Value); return buf;
	case EConVarType_Int32:  snprintf(buf, sizeof(buf), "%d", c->m_Value.m_i32Value); return buf;
	case EConVarType_UInt32: snprintf(buf, sizeof(buf), "%u", c->m_Value.m_u32Value); return buf;
	default: return {};
	}
}

// THE VIEWER'S VALUES OUTLIVE THE SESSION. A restore only runs on the off edge of the view, through
// the console buffer. Quit, Alt-F4 or a crash with the In-game view up leaves the player values in
// CS2, and the next session then read those back as "the viewer's own": the originals were gone.
// So the saved values are also kept in the settings file while they are applied ("1_1_2", one per
// variable above, "none" when nothing is owed). A session that starts with values owed puts them
// back on its first frame, and an Apply that finds values owed keeps them instead of reading CS2.
const char* const kOwedKey = "hud.viewerCvars";

std::string JoinSaved() {
	std::string out;
	for (const LookCvar& c : g_cvars) {
		if (c.saved.empty()) return {}; // one unreadable variable: nothing is recorded as owed
		if (!out.empty()) out += '_';
		out += c.saved;
	}
	return out;
}

// The values a previous session still owes the viewer, into `saved`. False when none are owed or
// the stored text is not exactly one integer per variable.
bool LoadOwed() {
	const std::string text = FilmmakerSettings_UiString(kOwedKey, "none");
	if (text.empty() || text == "none") return false;
	std::string parts[sizeof(g_cvars) / sizeof(g_cvars[0])];
	size_t n = 0;
	for (char ch : text) {
		if (ch == '_') { if (++n >= sizeof(parts) / sizeof(parts[0])) return false; continue; }
		if (!(ch >= '0' && ch <= '9') && !(ch == '-' && parts[n].empty())) return false;
		parts[n] += ch;
	}
	if (n + 1 != sizeof(parts) / sizeof(parts[0])) return false;
	for (const std::string& part : parts)
		if (part.empty() || part == "-") return false;
	for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) g_cvars[i].saved = parts[i];
	return true;
}

// True once every variable reads the viewer's value the last restore wrote.
bool RestoreLanded() {
	for (const LookCvar& c : g_cvars)
		if (!c.saved.empty() && ReadCvarText(c.name) != c.saved) return false;
	return true;
}

void Apply() {
	// The last restore has not shown yet, or an earlier session ended with the values applied:
	// either way CS2 reads the player values right now, and the viewer's are the ones already held.
	const bool keep = g_restorePending > 0 || LoadOwed();
	g_restorePending = 0;
	for (LookCvar& c : g_cvars) {
		if (!keep) c.saved = ReadCvarText(c.name);
		if (c.saved.empty()) {
			MvmDebugLog_Linef("hud.gameplay", "%s not found or unreadable; left alone", c.name);
			continue;
		}
		ExecClientCmd(std::string(c.name) + " " + c.playerValue);
	}
	const std::string owed = JoinSaved();
	if (!owed.empty()) FilmmakerSettings_SetUi(kOwedKey, owed.c_str());
	MvmDebugLog_Linef("hud.gameplay", "ON  %s=%s->%s %s=%s->%s %s=%s->%s%s",
		g_cvars[0].name, g_cvars[0].saved.c_str(), g_cvars[0].playerValue,
		g_cvars[1].name, g_cvars[1].saved.c_str(), g_cvars[1].playerValue,
		g_cvars[2].name, g_cvars[2].saved.c_str(), g_cvars[2].playerValue,
		keep ? " (viewer values kept: the last restore had not shown yet)" : "");
}

void Restore() {
	for (LookCvar& c : g_cvars) {
		if (!c.saved.empty())
			ExecClientCmd(std::string(c.name) + " " + c.saved);
	}
	MvmDebugLog_Linef("hud.gameplay", "OFF restored %s=%s %s=%s %s=%s",
		g_cvars[0].name, g_cvars[0].saved.c_str(),
		g_cvars[1].name, g_cvars[1].saved.c_str(),
		g_cvars[2].name, g_cvars[2].saved.c_str());
	g_restorePending = kRestoreSettleFrames;
}

} // namespace

void GameplayHudLook_RunFrame(bool inGameViewShown) {
	// Once per session: values an earlier session still owes the viewer (it quit or crashed with the
	// In-game view up). Put back now unless the view is about to come up again, which keeps them.
	static bool s_startupChecked = false;
	if (!s_startupChecked && !ReadCvarText(g_cvars[0].name).empty()) { // once the console variables exist
		s_startupChecked = true;
		if (!inGameViewShown && LoadOwed()) {
			MvmDebugLog_LinefAlways("hud.gameplay", "an earlier session left the player values in CS2; restoring the viewer's (%s)",
				FilmmakerSettings_UiString(kOwedKey, "none").c_str());
			Restore();
			return;
		}
	}
	if (inGameViewShown != g_applied) {
		g_applied = inGameViewShown;
		if (g_applied) Apply();
		else Restore();
		return;
	}
	// Watch the last restore land. The frame bound covers a value changed by hand meanwhile,
	// which would otherwise never match and keep a stale value forever.
	if (!g_applied && g_restorePending > 0) {
		if (RestoreLanded()) g_restorePending = 0;
		else --g_restorePending;
		// Landed, or changed by hand meanwhile: nothing is owed any more.
		if (g_restorePending == 0) FilmmakerSettings_SetUi(kOwedKey, "none");
	}
}

bool GameplayHudLook_Applied() { return g_applied; }

} // namespace Filmmaker

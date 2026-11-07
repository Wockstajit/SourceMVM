#include "GameplayHudLook.h"

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

void Apply() {
	for (LookCvar& c : g_cvars) {
		c.saved = ReadCvarText(c.name);
		if (c.saved.empty()) {
			MvmDebugLog_Linef("hud.gameplay", "%s not found or unreadable; left alone", c.name);
			continue;
		}
		ExecClientCmd(std::string(c.name) + " " + c.playerValue);
	}
	MvmDebugLog_Linef("hud.gameplay", "ON  %s=%s->%s %s=%s->%s %s=%s->%s",
		g_cvars[0].name, g_cvars[0].saved.c_str(), g_cvars[0].playerValue,
		g_cvars[1].name, g_cvars[1].saved.c_str(), g_cvars[1].playerValue,
		g_cvars[2].name, g_cvars[2].saved.c_str(), g_cvars[2].playerValue);
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
	for (LookCvar& c : g_cvars) c.saved.clear();
}

} // namespace

void GameplayHudLook_RunFrame(bool inGameViewShown) {
	if (inGameViewShown == g_applied) return;
	g_applied = inGameViewShown;
	if (g_applied) Apply();
	else Restore();
}

bool GameplayHudLook_Applied() { return g_applied; }

} // namespace Filmmaker

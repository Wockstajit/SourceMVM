#include "RagdollPhysics.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_Linef
#include "../../../Shared/AfxConsole.h"

// SourceSdkShared.h must precede cdll_int.h -- it defines SOURCESDK_abstract_class, without
// which cdll_int.h does not parse. Same include order as CameraBridge.cpp.
#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <Windows.h> // GetTickCount64 (re-assert cadence)
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {

namespace {

struct ParamSpec {
	const char* key;        // command/UI key
	const char* convar;     // CS2 convar name -- the ONLY version-coupled value in this module
	float       defaultValue;
	float       minValue;
	float       maxValue;
	bool        isBool;
	const char* label;
};

// Defaults are CS2's own, read from `find ragdoll` on build 2026-08-04. They are what
// RagdollPhysics_ResetAll() writes back, so if Valve changes a default these want updating --
// otherwise "reset" restores the old default rather than the new one.
const ParamSpec kSpecs[kRpParamCount] = {
	{ "gravity",  "ragdoll_gravity_scale",       1.0f,  -2.0f,  4.0f, false, "Gravity" },
	{ "friction", "ragdoll_friction_scale",      0.6f,   0.0f,  4.0f, false, "Friction" },
	{ "limit",    "cl_ragdoll_limit",           20.0f,  -1.0f, 64.0f, false, "Max bodies" },
	{ "collide",  "cl_player_ragdolls_collide",  0.0f,   0.0f,  1.0f, true,  "Bodies collide" },
	{ "minage",   "ragdoll_lru_min_age",        10.0f,   0.0f,300.0f, false, "Keep for" },
};

float g_value[kRpParamCount] = { 1.0f, 0.6f, 20.0f, 0.0f, 10.0f };
bool  g_overridden[kRpParamCount] = { false, false, false, false, false };
bool  g_cheatsUnlocked = false;
unsigned long long g_lastAssertMs = 0;

constexpr unsigned long long kReassertMs = 3000;

void ExecCmd(const char* command) {
	if (!g_pEngineToClient || !command) return;
	g_pEngineToClient->ExecuteClientCmd(0, command, true);
}

// Every ragdoll convar is cheat-flagged. Offline demo/movie work only -- this DLL is never
// wired for VAC-protected servers.
void EnsureCheatsUnlocked() {
	if (g_cheatsUnlocked) return;
	g_cheatsUnlocked = true;
	ExecCmd("mirv_cvar_unlock_sv_cheats");
	ExecCmd("sv_cheats 1");
	MvmDebugLog_Linef("ragdoll.phys", "cheats unlocked");
}

void ApplyOne(RagdollPhysParam p) {
	char buf[128];
	const ParamSpec& s = kSpecs[p];
	if (s.isBool)
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %s", s.convar, g_value[p] >= 0.5f ? "true" : "false");
	else if (s.convar[0] == 'c' && 0 == strcmp(s.convar, "cl_ragdoll_limit"))
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %d", s.convar, (int)g_value[p]); // integer convar
	else
		_snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s %.4f", s.convar, g_value[p]);
	ExecCmd(buf);
}

} // namespace

float RagdollPhysics_Get(RagdollPhysParam p) {
	if (p < 0 || p >= kRpParamCount) return 0.0f;
	return g_value[p];
}

bool RagdollPhysics_IsOverridden(RagdollPhysParam p) {
	if (p < 0 || p >= kRpParamCount) return false;
	return g_overridden[p];
}

float RagdollPhysics_Default(RagdollPhysParam p) {
	if (p < 0 || p >= kRpParamCount) return 0.0f;
	return kSpecs[p].defaultValue;
}

void RagdollPhysics_Set(RagdollPhysParam p, float value) {
	if (p < 0 || p >= kRpParamCount) return;
	const ParamSpec& s = kSpecs[p];

	// Hand-rolled clamp: windows.h defines min/max as macros, mangling std::min/std::max.
	float v = value;
	if (v < s.minValue) v = s.minValue;
	else if (v > s.maxValue) v = s.maxValue;

	EnsureCheatsUnlocked();
	g_value[p] = v;
	// "Overridden" tracks whether we differ from CS2's default, so ResetAll knows what to undo
	// and the UI can show which knobs are actually doing something.
	g_overridden[p] = (v != s.defaultValue);
	ApplyOne(p);
	g_lastAssertMs = GetTickCount64();
	MvmDebugLog_Linef("ragdoll.phys", "set %s=%.4f", s.convar, v);
}

void RagdollPhysics_ResetAll() {
	for (int i = 0; i < kRpParamCount; ++i) {
		g_value[i] = kSpecs[i].defaultValue;
		if (g_overridden[i]) {
			g_overridden[i] = false;
			ApplyOne((RagdollPhysParam)i);
		}
	}
	MvmDebugLog_Linef("ragdoll.phys", "reset all");
}

void RagdollPhysics_CleanupAll() {
	ExecCmd("ragdoll_cleanup_all");
	MvmDebugLog_Linef("ragdoll.phys", "cleanup_all");
}

void RagdollPhysics_RunFrame() {
	bool any = false;
	for (int i = 0; i < kRpParamCount; ++i) if (g_overridden[i]) { any = true; break; }
	if (!any) return;

	const unsigned long long now = GetTickCount64();
	if (now - g_lastAssertMs < kReassertMs) return;
	g_lastAssertMs = now;
	// Re-assert only what we actually changed. A map change or config exec can reset these, and
	// a look that silently reverts mid-edit is worse than a few redundant commands.
	for (int i = 0; i < kRpParamCount; ++i) if (g_overridden[i]) ApplyOne((RagdollPhysParam)i);
}

void RagdollPhysics_AppendStateJson(std::ostringstream& o) {
	for (int i = 0; i < kRpParamCount; ++i) {
		o << ",\"rp_" << kSpecs[i].key << "\":" << g_value[i];
	}
}

void RagdollPhysics_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	// argv: mirv_filmmaker ragdollphys [<key> <value> [quiet] | reset | cleanup]
	const char* sub = (argc >= 3) ? args->ArgV(2) : "";

	if (0 == _stricmp(sub, "reset")) {
		RagdollPhysics_ResetAll();
		advancedfx::Message("mirv_filmmaker: ragdoll physics reset to CS2 defaults.\n");
		return;
	}
	if (0 == _stricmp(sub, "cleanup")) {
		RagdollPhysics_CleanupAll();
		advancedfx::Message("mirv_filmmaker: removed all ragdolls currently in the world.\n");
		return;
	}

	for (int i = 0; i < kRpParamCount; ++i) {
		if (0 != _stricmp(sub, kSpecs[i].key)) continue;
		if (argc < 4) {
			advancedfx::Message("mirv_filmmaker ragdollphys %s = %.3f (default %.3f, range %.2f..%.2f) [%s]\n",
				kSpecs[i].key, g_value[i], kSpecs[i].defaultValue,
				kSpecs[i].minValue, kSpecs[i].maxValue, kSpecs[i].convar);
			return;
		}
		const bool quiet = (argc >= 5) && (0 == _stricmp(args->ArgV(4), "quiet"));
		RagdollPhysics_Set((RagdollPhysParam)i, (float)atof(args->ArgV(3)));
		if (!quiet)
			advancedfx::Message("mirv_filmmaker: ragdoll %s = %.3f.\n", kSpecs[i].key, g_value[i]);
		return;
	}

	advancedfx::Message("mirv_filmmaker ragdollphys - ragdoll physics (separate from `ragdoll`, which retunes death-body mass/damping).\n");
	for (int i = 0; i < kRpParamCount; ++i) {
		advancedfx::Message("  %-8s %-7.3f (default %-6.2f) %s\n",
			kSpecs[i].key, g_value[i], kSpecs[i].defaultValue, kSpecs[i].label);
	}
	advancedfx::Message(
		"Usage:\n"
		"  mirv_filmmaker ragdollphys <key> <value> - set one knob\n"
		"  mirv_filmmaker ragdollphys reset         - restore CS2 defaults\n"
		"  mirv_filmmaker ragdollphys cleanup       - remove all bodies currently in the world\n");
}

} // namespace Filmmaker

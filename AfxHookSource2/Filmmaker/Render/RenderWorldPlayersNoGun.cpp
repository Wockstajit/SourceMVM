#include "RenderWorldPlayersNoGun.h"

#include "../Debug/MvmDebugLog.h" // MvmDebugLog_*
#include "../Movie/ParticleFx.h"           // first-person weapon FX suppression

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ISource2EngineToClient* g_pEngineToClient;

namespace Filmmaker {
namespace {

constexpr const char* kDrawViewmodelCvar = "r_drawviewmodel";
// A POV switch or a freshly rebuilt spectated pawn can restore the cvar. Reasserting at this
// cadence keeps the pass closed without issuing a console command every frame.
constexpr unsigned kReassertFrames = 16;

std::atomic<bool> g_armed{false};
bool g_cvarApplied = false;
std::string g_previousCvar;
bool g_previousFxSuppression = false;
bool g_previousFxCaptured = false;
unsigned g_reassertFrame = 0;
char g_reason[64] = "";

void RunClientCmd(const char* cmd) {
	if (g_pEngineToClient && cmd)
		g_pEngineToClient->ExecuteClientCmd(0, cmd, true);
}

std::string ReadBoolCvarValue(const char* name, const char* fallback) {
	using namespace SOURCESDK::CS2;
	if (!g_pCVar) return fallback;
	Cvar_s* handle = g_pCVar->GetCvar(g_pCVar->FindConVar(name, false).Get());
	if (!handle) return fallback;
	return handle->m_Value.m_bValue ? "1" : "0";
}

void ApplyHidden() {
	g_previousCvar = ReadBoolCvarValue(kDrawViewmodelCvar, "1");
	g_previousFxSuppression = ParticleFx_FirstPersonFxSuppressed();
	g_previousFxCaptured = true;
	RunClientCmd("r_drawviewmodel false");
	// r_drawviewmodel does not prevent the engine from creating _fp/_fps weapon particles. The
	// existing gate is the narrowest way to keep muzzle flashes/smoke from being the only visible
	// remnant of the first-person weapon.
	ParticleFx_SetFirstPersonFxSuppressed(true);
	g_cvarApplied = true;
	g_reassertFrame = 0;
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldplayersnogun",
			"r_drawviewmodel 0 (previous=%s), first-person FX suppressed", g_previousCvar.c_str());
}

void RestoreVisible() {
	// Copy the snapshot before clearing it below. Logging a pointer into g_previousCvar after
	// clear() made the restore evidence print an empty value even though the cvar was restored.
	const std::string restoredValue = g_previousCvar.empty() ? "1" : g_previousCvar;
	char cmd[96];
	_snprintf_s(cmd, sizeof(cmd), _TRUNCATE, "%s %s", kDrawViewmodelCvar, restoredValue.c_str());
	RunClientCmd(cmd);
	if (g_previousFxCaptured)
		ParticleFx_SetFirstPersonFxSuppressed(g_previousFxSuppression);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldplayersnogun",
			"r_drawviewmodel restored to %s, first-person FX suppression restored", restoredValue.c_str());
	g_previousFxCaptured = false;
	g_previousCvar.clear();
	g_cvarApplied = false;
	g_reassertFrame = 0;
}

} // namespace

void RenderWorldPlayersNoGun_RunMainThreadFrame() {
	const bool armed = g_armed.load(std::memory_order_relaxed);
	if (armed != g_cvarApplied) {
		if (armed) ApplyHidden();
		else RestoreVisible();
		return;
	}
	if (!armed) return;

	// Keep the particle gate authoritative even if the detached-camera helper changes it later in
	// this frame. The exchange inside the helper is a no-op when it is already true.
	ParticleFx_SetFirstPersonFxSuppressed(true);
	if (++g_reassertFrame < kReassertFrames) return;
	g_reassertFrame = 0;
	if (ReadBoolCvarValue(kDrawViewmodelCvar, "0") == "0") return;
	RunClientCmd("r_drawviewmodel false");
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldplayersnogun",
			"reasserted r_drawviewmodel 0 after engine/POV state change");
}

void RenderWorldPlayersNoGun_SetArmed(bool armed, const char* reason) {
	const bool was = g_armed.load(std::memory_order_relaxed);
	if (was == armed) return;
	std::strncpy(g_reason, reason ? reason : "", sizeof(g_reason) - 1);
	g_reason[sizeof(g_reason) - 1] = '\0';
	g_armed.store(armed, std::memory_order_release);
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("render.worldplayersnogun", "%s reason='%s'",
			armed ? "ARM" : "DISARM", g_reason);
}

bool RenderWorldPlayersNoGun_IsArmed() {
	return g_armed.load(std::memory_order_acquire);
}

} // namespace Filmmaker

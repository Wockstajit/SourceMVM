#include "DynamicShadowFx.h"

#include "BakedShadowFx.h"
#include "../Debug/MvmDebugLog.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>

extern SOURCESDK::CS2::ICvar* SOURCESDK::CS2::g_pCVar;

namespace Filmmaker {
namespace {

// A zero live cascade count removes the per-frame sunlight shadow maps while leaving the sparse
// shadow tree / precomputed static contribution alone. The previous global shadow-casting gate
// killed both classes. Restore the map/engine value (normally -1 = automatic) when enabled.
// Mirage A-site corner (ClientVersion 2000879): live OFF barely moves the stairs/ground plant
// crops; those are owned by lb_enable_baked_shadows. Keep this module cascade-only.
constexpr const char* kCvarName = "csm_max_num_cascades_override";

bool g_enabled = true;
bool g_fillBorrow = false;
bool g_applied = false;
size_t g_index = (size_t)-1;
SOURCESDK::CS2::Cvar_s* g_cvar = nullptr;
int g_originalValue = -1;
bool g_haveOriginal = false;
std::string g_reason;
unsigned long long g_nextResolveMs = 0;

int DesiredCascadeValue() {
	// Product off normally forces 0. Fill-borrow (Phase B custom map) restores the map default
	// so the native atlas still receives depth while CustomShadowAtlas suppresses engine sampling.
	if (g_enabled || g_fillBorrow)
		return g_haveOriginal ? g_originalValue : -1;
	return 0;
}

bool ResolveCvar() {
	if (!SOURCESDK::CS2::g_pCVar) {
		g_reason = "ICvar is not ready.";
		return false;
	}
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar(kCvarName, false);
	if (!handle.IsValid()) {
		for (SOURCESDK::uint32 i = 0; i < SOURCESDK_CS2_MAX_VALID_CVARS; ++i) {
			SOURCESDK::CS2::Cvar_s* candidate = SOURCESDK::CS2::g_pCVar->GetCvar(i);
			if (!candidate) break;
			if (candidate->m_pszName && 0 == std::strcmp(candidate->m_pszName, kCvarName)) {
				candidate->m_nFlags &= ~(SOURCESDK::int64)(
					SOURCESDK_CS2_FCVAR_HIDDEN | SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY);
				handle.Set(i);
				break;
			}
		}
	}
	if (!handle.IsValid()) {
		g_reason = "csm_max_num_cascades_override was not found in the CS2 cvar registry.";
		return false;
	}
	SOURCESDK::CS2::Cvar_s* cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
	if (!cvar || cvar->m_eVarType != SOURCESDK::CS2::EConVarType_Int32) {
		g_reason = "csm_max_num_cascades_override resolved with an unexpected type.";
		return false;
	}
	g_index = handle.Get();
	g_cvar = cvar;
	g_reason.clear();
	MvmDebugLog_LinefAlways("world.dynamicshadows", "resolved cvar name=%s index=%zu ptr=%p",
		kCvarName, g_index, (void*)g_cvar);
	return true;
}

bool ApplyResolved() {
	if (!g_cvar || g_index == (size_t)-1 || !SOURCESDK::CS2::g_pCVar) return false;
    if ((g_enabled || g_fillBorrow) && !g_haveOriginal) { g_applied=true;return true; }
    if (!g_enabled && !g_fillBorrow && !g_haveOriginal) {
        g_originalValue=g_cvar->m_Value.m_i32Value;g_haveOriginal=true;
    }
    const int desired = DesiredCascadeValue();
    if (g_enabled || g_fillBorrow) g_haveOriginal=false;
    const int old = g_cvar->m_Value.m_i32Value;
	if (old == desired) { g_applied = true; return true; }
	SOURCESDK::CS2::CVValue_t oldValue = {}, newValue = {};
	oldValue.m_i32Value = old;
	newValue.m_i32Value = desired;
	g_cvar->m_Value.m_i32Value = desired;
	++g_cvar->m_iTimesChanged;
	SOURCESDK::CS2::ConVarHandle handle;
	handle.Set(g_index);
	SOURCESDK::CS2::g_pCVar->CallChangeCallback(handle, 0, &newValue, &oldValue);
	g_applied = true;
	MvmDebugLog_LinefAlways("world.dynamicshadows",
		"set cvar name=%s ptr=%p old=%d new=%d enabled=%d fillBorrow=%d",
		kCvarName, (void*)g_cvar, old, desired, g_enabled ? 1 : 0, g_fillBorrow ? 1 : 0);
	return true;
}

bool EnsureApplied() {
	const unsigned long long now = GetTickCount64();
	if (!g_cvar && now >= g_nextResolveMs) {
		g_nextResolveMs = now + 1000;
		if (!ResolveCvar()) return false;
	}
	if (!g_cvar) return false;
	return ApplyResolved();
}

} // namespace

void DynamicShadowFx_SetEnabled(bool enabled) {
	g_enabled = enabled;
	g_applied = false;
	EnsureApplied();
	// Kept for call-site symmetry; baked policy does not depend on live cascades.
	BakedShadowFx_OnLiveShadowsChanged();
}
bool DynamicShadowFx_Enabled() { return g_enabled; }

void DynamicShadowFx_SetFillBorrow(bool borrow) {
	if (g_fillBorrow == borrow) return;
	g_fillBorrow = borrow;
	g_applied = false;
	EnsureApplied();
	MvmDebugLog_LinefAlways("world.dynamicshadows", "fillBorrow=%d enabled=%d",
		borrow ? 1 : 0, g_enabled ? 1 : 0);
}
bool DynamicShadowFx_FillBorrow() { return g_fillBorrow; }

bool DynamicShadowFx_Applied() { return g_applied; }

bool DynamicShadowFx_Available(std::string* outReason) {
	if (EnsureApplied()) return true;
	if (outReason) *outReason = g_reason.empty() ? "live-shadow cvar has not resolved yet." : g_reason;
	return false;
}

void DynamicShadowFx_OnLevelInit() {
    bool desired=g_enabled;g_enabled=true;EnsureApplied();g_enabled=desired;
}

void DynamicShadowFx_RunFrame() { EnsureApplied(); }

void DynamicShadowFx_AppendStateJson(std::ostringstream& o) {
	std::string reason;
	o << ",\"dynamicShadows\":" << (g_enabled ? "true" : "false");
	o << ",\"dynamicShadowsApplied\":" << (g_applied ? "true" : "false");
	o << ",\"dynamicShadowsCvar\":\"" << (g_cvar ? kCvarName : "") << "\"";
	o << ",\"dynamicShadowsOk\":" << (DynamicShadowFx_Available(&reason) ? "true" : "false");
}

void DynamicShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "enable")) DynamicShadowFx_SetEnabled(true);
	else if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "disable")) DynamicShadowFx_SetEnabled(false);
	else if (0 == _stricmp(op, "toggle")) DynamicShadowFx_SetEnabled(!DynamicShadowFx_Enabled());
	else if (0 == _stricmp(op, "reset")) DynamicShadowFx_SetEnabled(true);
	else {
		advancedfx::Message("mirv_filmmaker world dynamicshadows on|off|toggle|reset\n");
		return;
	}
	advancedfx::Message("Dynamic shadows %s%s.\n", DynamicShadowFx_Enabled() ? "enabled" : "disabled",
		DynamicShadowFx_Applied() ? "" : " (pending engine cvar)");
}

} // namespace Filmmaker

#include "BakedShadowFx.h"

#include "../Debug/MvmDebugLog.h"
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../../../Shared/AfxConsole.h"

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

extern SOURCESDK::CS2::ICvar* SOURCESDK::CS2::g_pCVar;

namespace Filmmaker {
namespace {

// Mirage A-site corner A/B (CS2 ClientVersion 2000879, session 24a695):
// Protocol: demo_gototick 2936, freecam (-1561.1,-2215.5,-164.3) ang (10,-70,0), sun yaw 140 / pitch 30.
// lb_enable_baked_shadows=0 removes the stairs/ground plant-frond contribution
// (stairs crop mean dL ~+40, ground-palm ~+50) while fence/live CSM remains.
// Upper-wall soft residual barely moves (~0.03) at that sun — lightmap/AO, not this gate.
// Rejected for this toggle: sc_disable_baked_lighting, precomputed/SST alone,
// static-geo cascade override (strips live CSM), entity DisableShadows_t (no pixel effect).
struct CvarTarget {
	const char* name;
	SOURCESDK::CS2::EConVarType type;
	bool invertWhenDisabled = false;
	size_t index = (size_t)-1;
	SOURCESDK::CS2::Cvar_s* cvar = nullptr;
	bool originalBool = false;
	int originalInt = 0;
	bool haveOriginal = false;
};

CvarTarget g_targets[] = {
	// Baked OFF => write false (engine default original is normally true).
	{ "lb_enable_baked_shadows", SOURCESDK::CS2::EConVarType_Bool, false },
};

bool g_enabled = true;
bool g_applied = false;
std::string g_reason;
unsigned long long g_nextResolveMs = 0;

bool ReadEntityShadowOverride(CEntityInstance* entity, unsigned char* outValue) {
	if (!entity || !outValue || !g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags)
		return false;
	__try {
		*outValue = *(unsigned char*)((unsigned char*)entity
			+ g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		*outValue = 0;
		return false;
	}
}

bool WriteEntityShadowOverride(CEntityInstance* entity, unsigned char value) {
	if (!entity || !g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags)
		return false;
	__try {
		*(unsigned char*)((unsigned char*)entity
			+ g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags) = value;
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

CEntityInstance* ProbeEntityAt(int index) {
	if (index < 0 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	__try {
		return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

const char* ProbeEntityString(CEntityInstance* entity, int kind) {
	if (!entity) return nullptr;
	__try {
		if (kind == 0) return entity->GetDebugName();
		if (kind == 1) return entity->GetClassName();
		return entity->GetClientClassName();
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return nullptr;
	}
}

void ProbeEntityShadowOverride(int index, const char* mode) {
	if (!g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags) {
		advancedfx::Warning(
			"bakedshadows entity: C_BaseEntity::m_nSceneObjectOverrideFlags did not resolve.\n");
		return;
	}
	CEntityInstance* entity = ProbeEntityAt(index);
	if (!entity) {
		advancedfx::Warning("bakedshadows entity: index %d is not live.\n", index);
		return;
	}

	unsigned char oldValue = 0;
	if (!ReadEntityShadowOverride(entity, &oldValue)) {
		advancedfx::Warning("bakedshadows entity: could not read index %d.\n", index);
		return;
	}
	if (!mode || !mode[0] || 0 == _stricmp(mode, "get")) {
		advancedfx::Message(
			"entity %d override=%u debug='%s' class='%s' client='%s' offset=0x%zX\n",
			index, (unsigned)oldValue,
			ProbeEntityString(entity, 0) ? ProbeEntityString(entity, 0) : "",
			ProbeEntityString(entity, 1) ? ProbeEntityString(entity, 1) : "",
			ProbeEntityString(entity, 2) ? ProbeEntityString(entity, 2) : "",
			(size_t)g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags);
		return;
	}

	unsigned char desired = 0;
	if (0 == _stricmp(mode, "reset") || 0 == _stricmp(mode, "none")) desired = 0;
	else if (0 == _stricmp(mode, "all")) desired = 1;
	else if (0 == _stricmp(mode, "baked")) desired = 2;
	else if (0 == _stricmp(mode, "realtime") || 0 == _stricmp(mode, "live")) desired = 3;
	else if (0 == _stricmp(mode, "reallynone")) desired = 4;
	else {
		advancedfx::Warning(
			"bakedshadows entity: mode must be get|reset|all|baked|realtime|reallynone.\n");
		return;
	}

	const bool changed = oldValue != desired;
	if (changed && !WriteEntityShadowOverride(entity, desired)) {
		advancedfx::Warning("bakedshadows entity: could not write index %d.\n", index);
		return;
	}
	advancedfx::Message("entity %d override %u -> %u (%s).\n",
		index, (unsigned)oldValue, (unsigned)desired, mode);
	MvmDebugLog_LinefAlways("world.bakedshadows",
		"entity override idx=%d ent=%p offset=0x%zX old=%u new=%u changed=%d debug='%s' class='%s'",
		index, (void*)entity,
		(size_t)g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags,
		(unsigned)oldValue, (unsigned)desired, changed ? 1 : 0,
		ProbeEntityString(entity, 0) ? ProbeEntityString(entity, 0) : "",
		ProbeEntityString(entity, 1) ? ProbeEntityString(entity, 1) : "");
}

void ProbeEntityShadowOverrides(const char* filter) {
	if (!g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags) {
		advancedfx::Warning(
			"bakedshadows scan: C_BaseEntity::m_nSceneObjectOverrideFlags did not resolve.\n");
		return;
	}
	if (!g_pEntityList || !g_GetEntityFromIndex) {
		advancedfx::Warning("bakedshadows scan: entity list is unavailable.\n");
		return;
	}

	const bool filtered = filter && filter[0];
	int count = 0;
	for (int index = 0; index <= GetHighestEntityIndex(); ++index) {
		CEntityInstance* entity = ProbeEntityAt(index);
		if (!entity) continue;
		const char* debugName = ProbeEntityString(entity, 0);
		const char* className = ProbeEntityString(entity, 1);
		const char* clientClassName = ProbeEntityString(entity, 2);
		if (filtered
			&& (!className || !strstr(className, filter))
			&& (!clientClassName || !strstr(clientClassName, filter))
			&& (!debugName || !strstr(debugName, filter)))
			continue;
		unsigned char value = 0;
		if (!ReadEntityShadowOverride(entity, &value)) continue;
		float originX = 0.0f, originY = 0.0f, originZ = 0.0f;
		__try {
			entity->GetOrigin(originX, originY, originZ);
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			originX = originY = originZ = 0.0f;
		}
		const char* entityName = nullptr;
		__try {
			entityName = entity->GetName();
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			entityName = nullptr;
		}
		advancedfx::Message(
			"entity %d override=%u origin=(%.1f %.1f %.1f) name='%s' debug='%s' class='%s' client='%s'\n",
			index, (unsigned)value, originX, originY, originZ,
			entityName ? entityName : "", debugName ? debugName : "",
			className ? className : "", clientClassName ? clientClassName : "");
		++count;
	}
	advancedfx::Message("bakedshadows scan: %d matching live entities, filter='%s', offset=0x%zX.\n",
		count, filtered ? filter : "", (size_t)g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags);
	MvmDebugLog_LinefAlways("world.bakedshadows",
		"entity scan count=%d filter='%s' offset=0x%zX",
		count, filtered ? filter : "",
		(size_t)g_clientDllOffsets.C_BaseEntity.m_nSceneObjectOverrideFlags);
}

bool ResolveTarget(CvarTarget& target) {
	if (!SOURCESDK::CS2::g_pCVar) {
		g_reason = "ICvar is not ready.";
		return false;
	}
	SOURCESDK::CS2::ConVarHandle handle =
		SOURCESDK::CS2::g_pCVar->FindConVar(target.name, false);
	if (!handle.IsValid()) {
		for (SOURCESDK::uint32 i = 0; i < SOURCESDK_CS2_MAX_VALID_CVARS; ++i) {
			SOURCESDK::CS2::Cvar_s* candidate = SOURCESDK::CS2::g_pCVar->GetCvar(i);
			if (!candidate) break;
			if (candidate->m_pszName && 0 == std::strcmp(candidate->m_pszName, target.name)) {
				candidate->m_nFlags &= ~(SOURCESDK::int64)(
					SOURCESDK_CS2_FCVAR_HIDDEN | SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY);
				handle.Set(i);
				MvmDebugLog_LinefAlways("world.bakedshadows",
					"found hidden cvar name=%s index=%u flags=0x%llx type=%d",
					target.name, i, (unsigned long long)candidate->m_nFlags,
					(int)candidate->m_eVarType);
				break;
			}
		}
	}
	if (!handle.IsValid()) {
		g_reason = std::string(target.name) + " was not found in the CS2 cvar registry.";
		return false;
	}
	SOURCESDK::CS2::Cvar_s* cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
	if (!cvar || cvar->m_eVarType != target.type) {
		g_reason = std::string(target.name) + " resolved with an unexpected type.";
		return false;
	}
	target.index = handle.Get();
	target.cvar = cvar;
	g_reason.clear();
	return true;
}

bool ResolveTargets() {
	for (CvarTarget& target : g_targets)
		if (!target.cvar && !ResolveTarget(target)) return false;
	return true;
}

bool ApplyTarget(CvarTarget& target) {
	if (!target.cvar || target.index == (size_t)-1 || !SOURCESDK::CS2::g_pCVar) return false;
    if (g_enabled && !target.haveOriginal) return true;
    if (!g_enabled && !target.haveOriginal) {
        target.originalBool=target.cvar->m_Value.m_bValue;
        target.originalInt=target.cvar->m_Value.m_i32Value;
        target.haveOriginal=true;
    }
    SOURCESDK::CS2::CVValue_t oldValue = {};
    SOURCESDK::CS2::CVValue_t newValue = {};
    bool changed = false;
    if (target.type == SOURCESDK::CS2::EConVarType_Bool) {
		const bool desired = g_enabled ? target.originalBool : target.invertWhenDisabled;
		oldValue.m_bValue = target.cvar->m_Value.m_bValue;
		newValue.m_bValue = desired;
		changed = oldValue.m_bValue != desired;
		if (changed) target.cvar->m_Value.m_bValue = desired;
	} else {
		const int desired = g_enabled ? target.originalInt : 0;
		oldValue.m_i32Value = target.cvar->m_Value.m_i32Value;
		newValue.m_i32Value = desired;
		changed = oldValue.m_i32Value != desired;
		if (changed) target.cvar->m_Value.m_i32Value = desired;
	}
    if (g_enabled) target.haveOriginal=false;
	if (!changed) return true;
	++target.cvar->m_iTimesChanged;
	SOURCESDK::CS2::ConVarHandle handle;
	handle.Set(target.index);
	SOURCESDK::CS2::g_pCVar->CallChangeCallback(handle, 0, &newValue, &oldValue);
	MvmDebugLog_LinefAlways("world.bakedshadows",
		"direct cvar set name=%s ptr=%p index=%zu old=%lld new=%lld enabled=%d",
		target.name, (void*)target.cvar, target.index,
		(long long)oldValue.m_bValue, (long long)newValue.m_bValue, g_enabled ? 1 : 0);
	return true;
}

bool EnsureApplied() {
	const unsigned long long now = GetTickCount64();
	bool needResolve = false;
	for (const CvarTarget& target : g_targets) needResolve = needResolve || !target.cvar;
	if (needResolve && now >= g_nextResolveMs) {
		g_nextResolveMs = now + 1000;
		if (!ResolveTargets()) return false;
	}
	for (const CvarTarget& target : g_targets) if (!target.cvar) return false;
	bool allApplied = true;
	for (CvarTarget& target : g_targets)
		allApplied = ApplyTarget(target) && allApplied;
	g_applied = allApplied;
	return allApplied;
}

} // namespace

void BakedShadowFx_SetEnabled(bool enabled) {
	g_enabled = enabled;
	g_applied = false;
	EnsureApplied();
}

void BakedShadowFx_OnLiveShadowsChanged() {
	// Intentionally empty: Live (CSM cascades) must not drive baked lighting.
}

bool BakedShadowFx_Enabled() { return g_enabled; }
bool BakedShadowFx_Applied() { return g_applied; }

bool BakedShadowFx_Available(std::string* outReason) {
	if (EnsureApplied()) return true;
	if (outReason) *outReason = g_reason.empty()
		? std::string("baked-lighting cvar has not resolved yet.") : g_reason;
	return false;
}

void BakedShadowFx_OnLevelInit() {
    bool desired=g_enabled;g_enabled=true;EnsureApplied();g_enabled=desired;
}

void BakedShadowFx_RunFrame() { EnsureApplied(); }

void BakedShadowFx_AppendStateJson(std::ostringstream& o) {
	std::string reason;
	o << ",\"bakedShadows\":" << (g_enabled ? "true" : "false");
	o << ",\"bakedShadowsApplied\":" << (g_applied ? "true" : "false");
	o << ",\"bakedShadowsCvars\":\"lb_enable_baked_shadows\"";
	o << ",\"bakedShadowsOk\":" << (BakedShadowFx_Available(&reason) ? "true" : "false");
}

// Direct-write any named cvar (clears HIDDEN/DEVONLY, CallChangeCallback). Used for
// A/B isolation of baked vs live shadow gates — console assignment often no-ops on hidden.
bool ProbeSetCvar(const char* name, const char* valueText, bool readOnly) {
	if (!SOURCESDK::CS2::g_pCVar || !name || !name[0]) return false;
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar(name, false);
	SOURCESDK::CS2::Cvar_s* cvar = nullptr;
	if (handle.IsValid()) {
		cvar = SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
	} else {
		for (SOURCESDK::uint32 i = 0; i < SOURCESDK_CS2_MAX_VALID_CVARS; ++i) {
			SOURCESDK::CS2::Cvar_s* candidate = SOURCESDK::CS2::g_pCVar->GetCvar(i);
			if (!candidate) break;
			if (candidate->m_pszName && 0 == std::strcmp(candidate->m_pszName, name)) {
				candidate->m_nFlags &= ~(SOURCESDK::int64)(
					SOURCESDK_CS2_FCVAR_HIDDEN | SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY);
				handle.Set(i);
				cvar = candidate;
				break;
			}
		}
	}
	if (!cvar || !handle.IsValid()) {
		advancedfx::Warning("bakedshadows probe: cvar '%s' not found.\n", name);
		return false;
	}
	cvar->m_nFlags &= ~(SOURCESDK::int64)(
		SOURCESDK_CS2_FCVAR_HIDDEN | SOURCESDK_CS2_FCVAR_DEVELOPMENTONLY);

	SOURCESDK::CS2::CVValue_t oldValue = {};
	SOURCESDK::CS2::CVValue_t newValue = {};
	bool changed = false;
	const int type = (int)cvar->m_eVarType;

	if (readOnly) {
		if (type == (int)SOURCESDK::CS2::EConVarType_Bool)
			advancedfx::Message("probe get %s = %d (bool) flags=0x%llx\n", name,
				cvar->m_Value.m_bValue ? 1 : 0, (unsigned long long)cvar->m_nFlags);
		else if (type == (int)SOURCESDK::CS2::EConVarType_Int32)
			advancedfx::Message("probe get %s = %d (int) flags=0x%llx\n", name,
				cvar->m_Value.m_i32Value, (unsigned long long)cvar->m_nFlags);
		else if (type == (int)SOURCESDK::CS2::EConVarType_Float32)
			advancedfx::Message("probe get %s = %g (float) flags=0x%llx\n", name,
				(double)cvar->m_Value.m_flValue, (unsigned long long)cvar->m_nFlags);
		else
			advancedfx::Message("probe get %s type=%d (unsupported print) flags=0x%llx\n",
				name, type, (unsigned long long)cvar->m_nFlags);
		return true;
	}

	if (!valueText) {
		advancedfx::Warning("bakedshadows probe: missing value for '%s'.\n", name);
		return false;
	}
	if (type == (int)SOURCESDK::CS2::EConVarType_Bool) {
		const bool desired = 0 != atoi(valueText);
		oldValue.m_bValue = cvar->m_Value.m_bValue;
		newValue.m_bValue = desired;
		changed = oldValue.m_bValue != desired;
		if (changed) cvar->m_Value.m_bValue = desired;
	} else if (type == (int)SOURCESDK::CS2::EConVarType_Int32) {
		const int desired = atoi(valueText);
		oldValue.m_i32Value = cvar->m_Value.m_i32Value;
		newValue.m_i32Value = desired;
		changed = oldValue.m_i32Value != desired;
		if (changed) cvar->m_Value.m_i32Value = desired;
	} else if (type == (int)SOURCESDK::CS2::EConVarType_Float32) {
		const float desired = (float)atof(valueText);
		oldValue.m_flValue = cvar->m_Value.m_flValue;
		newValue.m_flValue = desired;
		changed = oldValue.m_flValue != desired;
		if (changed) cvar->m_Value.m_flValue = desired;
	} else {
		advancedfx::Warning("bakedshadows probe: unsupported type %d for '%s'.\n", type, name);
		return false;
	}

	if (changed) {
		++cvar->m_iTimesChanged;
		SOURCESDK::CS2::g_pCVar->CallChangeCallback(handle, 0, &newValue, &oldValue);
	}
	advancedfx::Message(
		"probe set %s type=%d changed=%d old_b=%d new_b=%d old_i=%d new_i=%d\n",
		name, type, changed ? 1 : 0,
		oldValue.m_bValue ? 1 : 0, newValue.m_bValue ? 1 : 0,
		oldValue.m_i32Value, newValue.m_i32Value);
	MvmDebugLog_LinefAlways("world.bakedshadows",
		"probe set name=%s type=%d changed=%d ptr=%p",
		name, type, changed ? 1 : 0, (void*)cvar);
	return true;
}

void BakedShadowFx_RunCommand(int argc, advancedfx::ICommandArgs* args, const char* cmd) {
	const char* op = argc >= 4 ? args->ArgV(3) : "";
	if (0 == _stricmp(op, "on") || 0 == _stricmp(op, "enable")) {
		BakedShadowFx_SetEnabled(true);
		advancedfx::Message("Baked shadows enabled%s.\n", BakedShadowFx_Applied() ? "" : " (pending)");
		return;
	}
	if (0 == _stricmp(op, "off") || 0 == _stricmp(op, "disable")) {
		BakedShadowFx_SetEnabled(false);
		advancedfx::Message("Baked shadows disabled%s.\n", BakedShadowFx_Applied() ? "" : " (pending)");
		return;
	}
	if (0 == _stricmp(op, "toggle")) {
		BakedShadowFx_SetEnabled(!BakedShadowFx_Enabled());
		advancedfx::Message("Baked shadows %s%s.\n", BakedShadowFx_Enabled() ? "enabled" : "disabled",
			BakedShadowFx_Applied() ? "" : " (pending)");
		return;
	}
	if (0 == _stricmp(op, "reset")) {
		BakedShadowFx_SetEnabled(true);
		advancedfx::Message("Baked shadows restored to engine default (enabled).\n");
		return;
	}
	if (0 == _stricmp(op, "entity")) {
		const int index = argc >= 5 ? atoi(args->ArgV(4)) : -1;
		const char* mode = argc >= 6 ? args->ArgV(5) : "get";
		ProbeEntityShadowOverride(index, mode);
		return;
	}
	if (0 == _stricmp(op, "scan")) {
		const char* filter = argc >= 5 ? args->ArgV(4) : "";
		ProbeEntityShadowOverrides(filter);
		return;
	}
	if (0 == _stricmp(op, "probe")) {
		const char* name = argc >= 5 ? args->ArgV(4) : "";
		if (!name[0]) {
			advancedfx::Message(
				"Usage: %s world bakedshadows probe <cvar> <value>\n"
				"       %s world bakedshadows probe get <cvar>\n"
				"Direct-writes hidden cvars (clears HIDDEN + CallChangeCallback).\n",
				cmd, cmd);
			return;
		}
		if (0 == _stricmp(name, "get")) {
			const char* cvarName = argc >= 6 ? args->ArgV(5) : "";
			ProbeSetCvar(cvarName, nullptr, true);
			return;
		}
		const char* value = argc >= 6 ? args->ArgV(5) : "";
		ProbeSetCvar(name, value, false);
		return;
	}
	std::string reason;
	advancedfx::Message(
		"mirv_filmmaker world bakedshadows - map baked shadows (lb_enable_baked_shadows).\n"
		"Usage: %s world bakedshadows on|off|toggle|reset|entity|scan|probe\n"
		"Entity probe: %s world bakedshadows scan [class-filter]\n"
		"              %s world bakedshadows entity <index> get|reset|baked|realtime\n"
		"Current: %s, applied=%s, ok=%s.\n",
		cmd, cmd, cmd, g_enabled ? "enabled" : "disabled", g_applied ? "yes" : "no",
		BakedShadowFx_Available(&reason) ? "yes" : reason.c_str());
}

} // namespace Filmmaker

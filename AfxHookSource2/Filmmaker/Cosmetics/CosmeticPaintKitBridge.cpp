// Cosmetic paintkit bridge: the read/write plumbing onto the engine's own
// `cl_paintkit_override` cvar. While enabled, the bridge mirrors the spectated player's
// override paint kit into the cvar every frame (UpdatePaintkitBridge, driven from
// RunFrame) and restores the user's original value when disabled or when no override
// applies. All raw cvar-value access is SEH-guarded: an engine-side Cvar_s layout change
// degrades to "bridge does nothing" instead of crashing.

#include "CosmeticOverrideSystem.h"

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/icvar.h"

#include <cstdio>
#include <cstdlib>

namespace Filmmaker {

namespace {

SOURCESDK::CS2::Cvar_s* FindPaintkitOverrideCvar() {
	if (!SOURCESDK::CS2::g_pCVar)
		return nullptr;
	SOURCESDK::CS2::ConVarHandle handle = SOURCESDK::CS2::g_pCVar->FindConVar("cl_paintkit_override", false);
	if (!handle.IsValid())
		return nullptr;
	return SOURCESDK::CS2::g_pCVar->GetCvar(handle.Get());
}

bool ReadCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int* out) {
	if (!cvar || !out)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			*out = cvar->m_Value.m_bValue ? 1 : 0;
			return true;
		case SOURCESDK::CS2::EConVarType_Int16:
			*out = (int)cvar->m_Value.m_i16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt16:
			*out = (int)cvar->m_Value.m_u16Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int32:
			*out = cvar->m_Value.m_i32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt32:
			*out = (int)cvar->m_Value.m_u32Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Int64:
			*out = (int)cvar->m_Value.m_i64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_UInt64:
			*out = (int)cvar->m_Value.m_u64Value;
			return true;
		case SOURCESDK::CS2::EConVarType_Float32:
			*out = (int)cvar->m_Value.m_flValue;
			return true;
		case SOURCESDK::CS2::EConVarType_Float64:
			*out = (int)cvar->m_Value.m_dbValue;
			return true;
		case SOURCESDK::CS2::EConVarType_String:
			*out = atoi(cvar->m_Value.m_szValue.Get());
			return true;
		default:
			return false;
		}
	} __except (1) {
		return false;
	}
}

bool WriteCvarInt(SOURCESDK::CS2::Cvar_s* cvar, int value) {
	if (!cvar)
		return false;
	__try {
		switch (cvar->m_eVarType) {
		case SOURCESDK::CS2::EConVarType_Bool:
			cvar->m_Value.m_bValue = value != 0;
			break;
		case SOURCESDK::CS2::EConVarType_Int16:
			cvar->m_Value.m_i16Value = (short)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt16:
			cvar->m_Value.m_u16Value = (uint16_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int32:
			cvar->m_Value.m_i32Value = value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt32:
			cvar->m_Value.m_u32Value = (uint32_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Int64:
			cvar->m_Value.m_i64Value = (int64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_UInt64:
			cvar->m_Value.m_u64Value = (uint64_t)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float32:
			cvar->m_Value.m_flValue = (float)value;
			break;
		case SOURCESDK::CS2::EConVarType_Float64:
			cvar->m_Value.m_dbValue = (double)value;
			break;
		case SOURCESDK::CS2::EConVarType_String:
		{
			char buf[32];
			std::snprintf(buf, sizeof(buf), "%d", value);
			cvar->m_Value.m_szValue.Set(buf);
			break;
		}
		default:
			return false;
		}
		++cvar->m_iTimesChanged;
		return true;
	} __except (1) {
		return false;
	}
}

} // namespace

void CosmeticOverrideSystem::SetPaintkitBridge(bool e) {
	if (m_paintkitBridge == e) {
		if (m_paintkitBridge)
			UpdatePaintkitBridge();
		return;
	}
	m_paintkitBridge = e;
	if (!m_paintkitBridge)
		RestorePaintkitBridgeCvar();
	else
		UpdatePaintkitBridge();
}

bool CosmeticOverrideSystem::SetPaintkitBridgeCvar(int value) {
	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (!cvar)
		return false;

	int current = 0;
	if (!ReadCvarInt(cvar, &current))
		return false;

	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeOriginalValue = current;
		m_paintkitBridgeHaveOriginal = true;
	}

	if (current != value && !WriteCvarInt(cvar, value))
		return false;

	m_paintkitBridgeLastValue = value;
	return true;
}

void CosmeticOverrideSystem::RestorePaintkitBridgeCvar() {
	if (!m_paintkitBridgeHaveOriginal) {
		m_paintkitBridgeLastValue = 0;
		return;
	}

	SOURCESDK::CS2::Cvar_s* cvar = FindPaintkitOverrideCvar();
	m_paintkitBridgeCvarFound = cvar != nullptr;
	if (cvar)
		WriteCvarInt(cvar, m_paintkitBridgeOriginalValue);
	m_paintkitBridgeHaveOriginal = false;
	m_paintkitBridgeLastValue = 0;
	m_lastStats.paintkitBridgeValue = 0;
}

void CosmeticOverrideSystem::UpdatePaintkitBridge() {
	if (!m_paintkitBridge) {
		m_lastStats.paintkitBridgeValue = 0;
		return;
	}

	const int paintKit = ResolveSpectatedPaintkitOverride();
	if (paintKit <= 0) {
		RestorePaintkitBridgeCvar();
		return;
	}

	if (SetPaintkitBridgeCvar(paintKit))
		m_lastStats.paintkitBridgeValue = paintKit;
	else
		m_lastStats.paintkitBridgeValue = 0;
}

} // namespace Filmmaker

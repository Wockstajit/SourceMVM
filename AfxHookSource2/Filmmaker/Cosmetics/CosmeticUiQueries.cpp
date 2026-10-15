#include "CosmeticUiQueries.h"

#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include "CosmeticCatalog.h"  // single source of truth for item-definition -> slot
#include "../Debug/MvmDebugLog.h" // mvm_debug lines for flash suppression + pickup state

#include "../../../Deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../../../Deps/release/prop/cs2/sdk_src/public/cdll_int.h"

#include <cstring>
#include <sstream>
#include <string>

namespace Filmmaker {

namespace {

// Local copy of CameraEditorHud.cpp's rounding helper (same 4-line implementation) -- kept
// file-local rather than shared via a header for a helper this small.
double r2(double v) {
	if (!(v == v) || v > 1e15 || v < -1e15) return 0.0; // NaN/inf -> keep JSON valid
	double s = (v < 0) ? -1.0 : 1.0; return s * (long long)(v * s * 100.0 + 0.5) / 100.0;
}

std::string JsonEscape(const char* value) {
	std::string out;
	if (!value) return out;
	for (const unsigned char* p = (const unsigned char*)value; *p; ++p) {
		switch (*p) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\b': out += "\\b"; break;
		case '\f': out += "\\f"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (*p >= 0x20) out += (char)*p;
			break;
		}
	}
	return out;
}

CEntityInstance* EntityFromIndex(int index) {
	if (index < 0 || !g_pEntityList || !*g_pEntityList || !g_GetEntityFromIndex)
		return nullptr;
	return (CEntityInstance*)g_GetEntityFromIndex(*g_pEntityList, index);
}

int ActiveWeaponDefIndex(CEntityInstance* pawn) {
	if (!pawn || !pawn->IsPlayerPawn() || !g_cosmeticsOffsetsOk)
		return 0;
	SOURCESDK::CS2::CBaseHandle weaponHandle = pawn->GetActiveWeaponHandle();
	if (!weaponHandle.IsValid())
		return 0;
	CEntityInstance* weapon = EntityFromIndex(weaponHandle.GetEntryIndex());
	if (!weapon)
		return 0;
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + offsets.C_EconEntity.m_AttributeManager + offsets.C_AttributeContainer.m_Item;
	return (int)*(uint16_t*)(itemView + offsets.C_EconItemView.m_iItemDefinitionIndex);
}

enum class CustomizeWeaponSlot {
	None,
	Primary,
	Secondary,
	Knife
};

struct CustomizeWeaponInfo {
	int entityIndex = -1;
	int defIndex = 0;
	int paintKit = 0;
	float wear = 0.0f;
	uint64_t ownerXuid = 0; // original owner (m_OriginalOwnerXuid*); 0 = unknown/unavailable
};

uint64_t ReadWeaponOwnerXuid(int weaponEntityIndex);
const char* NameForSteamId(uint64_t steamId);

constexpr int kPaintKitAttributeId = 6;
constexpr int kPaintWearAttributeId = 8;

bool LooksLikePointer(const void* value) {
	uintptr_t p = (uintptr_t)value;
	return p > 0x10000;
}

struct AttributeVectorView {
	unsigned char* data = nullptr;
	int count = 0;
	int stride = 0;
};

bool TryMakeAttributeVector(unsigned char* vectorField, AttributeVectorView& out) {
	out = AttributeVectorView{};
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	int stride = (int)offsets.CEconItemAttribute.m_size;
	int minStride = (int)offsets.CEconItemAttribute.m_flValue + (int)sizeof(float);
	if (stride < minStride)
		stride = minStride;
	if (!vectorField || stride <= 0)
		return false;

	// C_UtlVectorEmbeddedNetworkVar<T> in networked fields is observed as count + pointer,
	// while standard SDK CUtlVector<T> stores pointer first and count at +0x10. Accept either
	// so cosmetic reads degrade cleanly across CS2 layout changes.
	int countA = *(int*)vectorField;
	unsigned char* dataA = *(unsigned char**)(vectorField + 8);
	if (countA > 0 && countA <= 128 && LooksLikePointer(dataA)) {
		out.data = dataA;
		out.count = countA;
		out.stride = stride;
		return true;
	}

	unsigned char* dataB = *(unsigned char**)vectorField;
	int countB = *(int*)(vectorField + 16);
	if (countB > 0 && countB <= 128 && LooksLikePointer(dataB)) {
		out.data = dataB;
		out.count = countB;
		out.stride = stride;
		return true;
	}

	return false;
}

// Searches ONE CAttributeList (embedded at itemView + listOff) for attributeId, writing its float
// value to `out`. Returns false when the list offset is unresolved, the vector is empty, or the
// attribute is absent. SEH-guarded: a bad pointer/offset faults here and is swallowed.
bool TryReadAttrFromList(unsigned char* itemView, ptrdiff_t listOff, int attributeId, float& out) {
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	if (!itemView || listOff == 0
		|| offsets.C_AttributeList.m_Attributes == 0
		|| offsets.CEconItemAttribute.m_iAttributeDefinitionIndex == 0
		|| offsets.CEconItemAttribute.m_flValue == 0)
		return false;

	unsigned char* vectorField = itemView + listOff + offsets.C_AttributeList.m_Attributes;
	AttributeVectorView vec;
	if (!TryMakeAttributeVector(vectorField, vec))
		return false;

	const ptrdiff_t defOffset = offsets.CEconItemAttribute.m_iAttributeDefinitionIndex;
	const ptrdiff_t valueOffset = offsets.CEconItemAttribute.m_flValue;
	__try {
		for (int i = 0; i < vec.count; ++i) {
			unsigned char* attr = vec.data + (ptrdiff_t)i * vec.stride;
			int def = (int)*(uint16_t*)(attr + defOffset);
			if (def == attributeId) {
				out = *(float*)(attr + valueOffset);
				return true;
			}
		}
	} __except (1) {
		return false;
	}
	return false;
}

bool TryReadItemAttributeFloat(unsigned char* itemView, int attributeId, float& out) {
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	// A networked/spectated player's econ attributes (paint kit/wear/seed) live in
	// m_NetworkedDynamicAttributes; the local "cooked" m_AttributeList is typically empty for them.
	// Try the networked list first, then the local list (which holds fully-initialized local items).
	if (TryReadAttrFromList(itemView, offsets.C_EconItemView.m_NetworkedDynamicAttributes, attributeId, out))
		return true;
	if (TryReadAttrFromList(itemView, offsets.C_EconItemView.m_AttributeList, attributeId, out))
		return true;
	return false;
}

int ReadItemAttributeInt(unsigned char* itemView, int attributeId, int fallback) {
	float value = 0.0f;
	if (!TryReadItemAttributeFloat(itemView, attributeId, value))
		return fallback;
	return (int)(value + (value >= 0.0f ? 0.5f : -0.5f));
}

float ReadItemAttributeFloat(unsigned char* itemView, int attributeId, float fallback) {
	float value = 0.0f;
	if (!TryReadItemAttributeFloat(itemView, attributeId, value))
		return fallback;
	return value;
}

// Deliberately delegates to CosmeticCatalog rather than repeating the def-index lists: this
// switch used to be a hand-maintained duplicate of the native apply loop's tables. Both now
// resolve through the generated CosmeticDefTables.inc, so the UI and the apply loop cannot
// disagree about what a def index is. See Docs/agent/cs2-update-recovery.md.
CustomizeWeaponSlot SlotForWeaponDef(int defIndex) {
	switch (CosmeticCatalog::SlotForDefIndex(defIndex)) {
	case CosmeticSlot::Secondary: return CustomizeWeaponSlot::Secondary;
	case CosmeticSlot::Primary:   return CustomizeWeaponSlot::Primary;
	case CosmeticSlot::Knife:     return CustomizeWeaponSlot::Knife;
	default:                      return CustomizeWeaponSlot::None;
	}
}

bool LooksLikeWeaponEntity(CEntityInstance* entity) {
	const char* className = entity ? entity->GetClassName() : nullptr;
	const char* clientClass = entity ? entity->GetClientClassName() : nullptr;
	return (className && std::strstr(className, "weapon_"))
		|| (className && std::strstr(className, "Weapon"))
		|| (clientClass && std::strstr(clientClass, "Weapon"));
}

bool ReadWeaponInfo(int entityIndex, CustomizeWeaponInfo& out) {
	CEntityInstance* weapon = EntityFromIndex(entityIndex);
	if (!weapon || !LooksLikeWeaponEntity(weapon) || !g_cosmeticsOffsetsOk)
		return false;

	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	unsigned char* w = (unsigned char*)weapon;
	unsigned char* itemView = w + offsets.C_EconEntity.m_AttributeManager + offsets.C_AttributeContainer.m_Item;
	const int defIndex = (int)*(uint16_t*)(itemView + offsets.C_EconItemView.m_iItemDefinitionIndex);
	if (SlotForWeaponDef(defIndex) == CustomizeWeaponSlot::None)
		return false;

	out.entityIndex = entityIndex;
	out.defIndex = defIndex;
	out.paintKit = ReadItemAttributeInt(itemView, kPaintKitAttributeId, *(int32_t*)(w + offsets.C_EconEntity.m_nFallbackPaintKit));
	out.wear = ReadItemAttributeFloat(itemView, kPaintWearAttributeId, *(float*)(w + offsets.C_EconEntity.m_flFallbackWear));
	out.ownerXuid = ReadWeaponOwnerXuid(entityIndex);
	return true;
}

void AssignLoadoutSlot(CustomizeWeaponInfo slots[3], const CustomizeWeaponInfo& info) {
	switch (SlotForWeaponDef(info.defIndex)) {
	case CustomizeWeaponSlot::Primary:
		slots[0] = info;
		break;
	case CustomizeWeaponSlot::Secondary:
		slots[1] = info;
		break;
	case CustomizeWeaponSlot::Knife:
		slots[2] = info;
		break;
	default:
		break;
	}
}

void BuildLoadoutSlots(CEntityInstance* pawn, int pawnIndex, CustomizeWeaponInfo slots[3]) {
	if (!pawn)
		return;

	SOURCESDK::CS2::CBaseHandle activeWeaponHandle = pawn->GetActiveWeaponHandle();
	if (activeWeaponHandle.IsValid()) {
		CustomizeWeaponInfo active;
		if (ReadWeaponInfo(activeWeaponHandle.GetEntryIndex(), active))
			AssignLoadoutSlot(slots, active);
	}

	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* entity = EntityFromIndex(i);
		if (!entity || !LooksLikeWeaponEntity(entity))
			continue;
		SOURCESDK::CS2::CBaseHandle owner = entity->GetOwnerEntityHandle();
		if (!owner.IsValid() || owner.GetEntryIndex() != pawnIndex)
			continue;
		CustomizeWeaponInfo info;
		if (ReadWeaponInfo(i, info))
			AssignLoadoutSlot(slots, info);
	}
}

// Reads the spectated player's equipped glove item view. Unlike weapons, gloves do not have
// C_EconEntity fallback fields on the pawn; their finish is stored as econ attributes on
// C_EconItemView.m_AttributeList (paint kit = 6, wear = 8).
bool ReadGloveInfo(CEntityInstance* pawn, CustomizeWeaponInfo& out) {
	out = CustomizeWeaponInfo{};
	if (!pawn || !g_cosmeticsOffsetsOk)
		return false;
	const ClientDllOffsets_t& offsets = g_clientDllOffsets;
	if (offsets.C_CSPlayerPawn.m_EconGloves == 0
		|| offsets.C_EconItemView.m_iItemDefinitionIndex == 0)
		return false;
	unsigned char* p = (unsigned char*)pawn;
	unsigned char* gloveView = p + offsets.C_CSPlayerPawn.m_EconGloves;
	int defIndex = 0;
	// Entity slots can briefly expose a pawn-shaped instance while a demo is seeking or shutting
	// down. IsPlayerPawn() is not enough to prove that the complete derived object is readable in
	// that transition. Keep the editor-state poll from taking CS2 down if the embedded item view
	// crosses an unmapped page; the next poll will retry after the entity settles.
	__try {
		defIndex = (int)*(uint16_t*)(gloveView + offsets.C_EconItemView.m_iItemDefinitionIndex);
	} __except (1) {
		MvmDebugLog_LinefAlways("customize.gloves",
			"READ_FAULT pawn=%p glovesOff=0x%llx defOff=0x%llx address=%p",
			(void*)pawn,
			(unsigned long long)offsets.C_CSPlayerPawn.m_EconGloves,
			(unsigned long long)offsets.C_EconItemView.m_iItemDefinitionIndex,
			(void*)(gloveView + offsets.C_EconItemView.m_iItemDefinitionIndex));
		return false;
	}
	if (defIndex <= 0 || defIndex == 5028 || defIndex == 5029)
		return false;
	int paintKit = ReadItemAttributeInt(gloveView, kPaintKitAttributeId, -1);
	if (paintKit <= 0)
		return false;
	out.entityIndex = -1;
	out.defIndex = defIndex;
	out.paintKit = paintKit;
	out.wear = ReadItemAttributeFloat(gloveView, kPaintWearAttributeId, 0.22f);
	return true;
}

// Emits one loadout slot's JSON body, including per-slot pickup ownership (only meaningful for
// primary/secondary -- knives/gloves can't be picked up from another player, so allowPickup=false
// keeps them permanently warning-free). pickup is derived from the WEAPON ENTITY's original-owner
// xuid vs the HOLDER's steam id, so it stays correct regardless of which weapon is actively held.
void WriteWeaponSlotBody(std::ostringstream& o, const CustomizeWeaponInfo& info, uint64_t holderSteamId, bool allowPickup) {
	if (info.defIndex <= 0) {
		o << "null";
		return;
	}
	const bool pickup = allowPickup && holderSteamId != 0 && info.ownerXuid != 0 && info.ownerXuid != holderSteamId;
	const char* ownerName = pickup ? NameForSteamId(info.ownerXuid) : nullptr;
	o << "{";
	o << "\"entityIndex\":" << info.entityIndex;
	o << ",\"defIndex\":" << info.defIndex;
	o << ",\"paintKit\":" << info.paintKit;
	o << ",\"wear\":" << r2(info.wear);
	o << ",\"pickup\":" << (pickup ? "true" : "false");
	o << ",\"ownerSteamId\":\"" << (pickup ? std::to_string(info.ownerXuid) : "") << "\"";
	o << ",\"ownerName\":\"" << JsonEscape(ownerName ? ownerName : "") << "\"";
	o << "}";
}

void WriteWeaponSlotJson(std::ostringstream& o, const char* name, const CustomizeWeaponInfo& info, uint64_t holderSteamId, bool allowPickup) {
	o << ",\"" << name << "\":";
	WriteWeaponSlotBody(o, info, holderSteamId, allowPickup);
}

// Reads the player's agent/player-model path (read-only) via the model-state chain, for the modal's
// AGENT display. pawn + m_CBodyComponent(ptr deref) + m_skeletonInstance + m_modelState + m_ModelName.
// SEH-guarded: a bad pointer/offset faults here and is swallowed; returns false then.
bool ReadAgentModelPath(CEntityInstance* pawn, char* out, size_t outSize) {
	if (out && outSize) out[0] = '\0';
	if (!pawn || !pawn->IsPlayerPawn() || !out || outSize == 0)
		return false;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.ModelChain.m_CBodyComponent == 0 || o.ModelChain.m_skeletonInstance == 0
		|| o.ModelChain.m_modelState == 0 || o.ModelChain.m_ModelName == 0)
		return false;
	unsigned char* p = (unsigned char*)pawn;
	bool ok = false;
	__try {
		unsigned char* bodyComp = *(unsigned char**)(p + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)bodyComp > 0x10000) {
			unsigned char* modelState = bodyComp + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
			const char* name = *(const char**)(modelState + o.ModelChain.m_ModelName);
			if ((uintptr_t)name > 0x10000) {
				size_t i = 0;
				for (; name[i] && i + 1 < outSize; ++i) out[i] = name[i];
				out[i] = '\0';
				ok = (i > 0);
			}
		}
	} __except (1) {
		ok = false;
	}
	return ok;
}

uint64_t ReadWeaponOwnerXuid(int weaponEntityIndex) {
	CEntityInstance* weapon = EntityFromIndex(weaponEntityIndex);
	if (!weapon || !g_cosmeticsOffsetsOk)
		return 0;
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.C_EconEntity.m_OriginalOwnerXuidLow == 0 || o.C_EconEntity.m_OriginalOwnerXuidHigh == 0)
		return 0;
	unsigned char* w = (unsigned char*)weapon;
	__try {
		uint32_t xLow = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidLow);
		uint32_t xHigh = *(uint32_t*)(w + o.C_EconEntity.m_OriginalOwnerXuidHigh);
		return ((uint64_t)xHigh << 32) | (uint64_t)xLow;
	} __except (1) {
		return 0;
	}
}

const char* NameForSteamId(uint64_t steamId) {
	if (steamId == 0)
		return nullptr;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* ctrl = EntityFromIndex(i);
		if (!ctrl || !ctrl->IsPlayerController() || ctrl->GetSteamId() != steamId)
			continue;
		const char* name = ctrl->GetSanitizedPlayerName();
		if (name && *name)
			return name;
		name = ctrl->GetPlayerName();
		if (name && *name)
			return name;
		return nullptr;
	}
	return nullptr;
}

} // namespace

std::string BuildCustomizeTargetJson(int pawnIndex) {
	CEntityInstance* pawn = EntityFromIndex(pawnIndex);
	if (!pawn || !pawn->IsPlayerPawn())
		return "null";

	CEntityInstance* controller = nullptr;
	SOURCESDK::CS2::CBaseHandle controllerHandle = pawn->GetPlayerControllerHandle();
	if (controllerHandle.IsValid())
		controller = EntityFromIndex(controllerHandle.GetEntryIndex());

	const char* name = nullptr;
	uint64_t steamId = 0;
	if (controller && controller->IsPlayerController()) {
		name = controller->GetSanitizedPlayerName();
		if (!name || !*name) name = controller->GetPlayerName();
		steamId = controller->GetSteamId();
	}
	if (!name || !*name)
		name = "Player";

	int activeWeaponIndex = -1;
	SOURCESDK::CS2::CBaseHandle activeWeaponHandle = pawn->GetActiveWeaponHandle();
	if (activeWeaponHandle.IsValid())
		activeWeaponIndex = activeWeaponHandle.GetEntryIndex();

	CustomizeWeaponInfo loadout[3];
	BuildLoadoutSlots(pawn, pawnIndex, loadout);

	std::ostringstream o;
	o << "{";
	o << "\"pawnIndex\":" << pawnIndex;
	o << ",\"controllerIndex\":" << (controllerHandle.IsValid() ? controllerHandle.GetEntryIndex() : -1);
	o << ",\"key\":\"" << (steamId ? ("steam:" + std::to_string(steamId)) : ("pawn:" + std::to_string(pawnIndex))) << "\"";
	o << ",\"steamId\":\"" << (steamId ? std::to_string(steamId) : "") << "\"";
	o << ",\"name\":\"" << JsonEscape(name) << "\"";
	o << ",\"team\":" << pawn->GetTeam();
	o << ",\"activeWeaponIndex\":" << activeWeaponIndex;
	o << ",\"activeWeaponDefIndex\":" << ActiveWeaponDefIndex(pawn);
	uint64_t activeOwnerXuid = (activeWeaponIndex >= 0) ? ReadWeaponOwnerXuid(activeWeaponIndex) : 0;
	const bool activeWeaponPickup = (steamId != 0 && activeOwnerXuid != 0 && activeOwnerXuid != steamId);
	const char* activeOwnerName = activeWeaponPickup ? NameForSteamId(activeOwnerXuid) : nullptr;
	o << ",\"activeWeaponOwnerSteamId\":\"" << (activeOwnerXuid ? std::to_string(activeOwnerXuid) : "") << "\"";
	o << ",\"activeWeaponOwnerName\":\"" << JsonEscape(activeOwnerName ? activeOwnerName : "") << "\"";
	o << ",\"activeWeaponPickup\":" << (activeWeaponPickup ? "true" : "false");
	o << ",\"weapons\":{";
	o << "\"primary\":";
	WriteWeaponSlotBody(o, loadout[0], steamId, true);
	WriteWeaponSlotJson(o, "secondary", loadout[1], steamId, true);
	WriteWeaponSlotJson(o, "knife", loadout[2], steamId, false);
	CustomizeWeaponInfo gloveInfo;
	const bool hasGloves = ReadGloveInfo(pawn, gloveInfo);
	o << ",\"gloves\":";
	if (hasGloves)
		o << "{\"defIndex\":" << gloveInfo.defIndex << ",\"paintKit\":" << gloveInfo.paintKit << ",\"wear\":" << r2(gloveInfo.wear) << "}";
	else
		o << "null";
	o << "}";
	// Read-only agent/player-model path for the AGENT slot display (matched to the catalog in JS).
	char agentModel[256] = {};
	ReadAgentModelPath(pawn, agentModel, sizeof(agentModel));
	o << ",\"agentModel\":\"" << JsonEscape(agentModel) << "\"";
	o << "}";
	return o.str();
}

// Zeroes the flashbang whiteout fields (m_flFlashMaxAlpha / m_flFlashDuration) on every player
// pawn. Called once per main-thread frame by CameraEditorHud while the Customize modal is open, so
// the flash post-effect never washes out the customizer UI or the 3D preview. Demo playback
// re-networks these fields, so normal flash behavior resumes once the modal closes and playback
// continues. Offsets are optional: unresolved fields (0) silently disable the suppression.
void CustomizeSuppressFlashTick() {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (o.C_CSPlayerPawn.m_flFlashMaxAlpha == 0 && o.C_CSPlayerPawn.m_flFlashDuration == 0)
		return;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* pawn = EntityFromIndex(i);
		if (!pawn || !pawn->IsPlayerPawn())
			continue;
		unsigned char* p = (unsigned char*)pawn;
		__try {
			bool zeroed = false;
			if (o.C_CSPlayerPawn.m_flFlashMaxAlpha != 0) {
				float* v = (float*)(p + o.C_CSPlayerPawn.m_flFlashMaxAlpha);
				if (*v != 0.0f) { *v = 0.0f; zeroed = true; }
			}
			if (o.C_CSPlayerPawn.m_flFlashDuration != 0) {
				float* v = (float*)(p + o.C_CSPlayerPawn.m_flFlashDuration);
				if (*v != 0.0f) { *v = 0.0f; zeroed = true; }
			}
			if (zeroed)
				MvmDebugLog_Linef("customize.flash", "suppressed flash on pawn %d", i);
		} __except (1) {}
	}
}

std::string BuildCustomizePlayersJson() {
	std::ostringstream o;
	o << "{";
	bool first = true;
	const int highest = GetHighestEntityIndex();
	for (int i = 0; i <= highest; ++i) {
		CEntityInstance* entity = EntityFromIndex(i);
		if (!entity || !entity->IsPlayerPawn())
			continue;
		std::string target = BuildCustomizeTargetJson(i);
		if (target == "null")
			continue;
		if (!first) o << ",";
		first = false;
		o << "\"" << i << "\":" << target;
	}
	o << "}";
	return o.str();
}

} // namespace Filmmaker

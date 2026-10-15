#include "CosmeticDirectComposite.h"

#include "../Debug/MvmDebugLog.h"

#include "../../SchemaSystem.h"

#include "../../../Shared/binutils.h"

#include <windows.h>

#include <cstring>

namespace Filmmaker {

namespace {

typedef void (__fastcall* DirectUpdateCompositeMaterial_t)(void* compositeOwner, bool force);
typedef void (__fastcall* DirectUpdateCompositeMaterialSet_t)(void* weapon, bool force);
typedef void (__fastcall* DirectUpdateSkin_t)(void* weapon, bool force);
typedef void (__fastcall* DirectSetAttributeValueByName_t)(void* itemView, const char* attrName, float value);

struct DirectCompositeFns {
	DirectUpdateCompositeMaterial_t updateCompositeMaterial = nullptr;
	DirectUpdateCompositeMaterialSet_t updateCompositeMaterialSet = nullptr;
	DirectUpdateSkin_t updateSkin = nullptr;
	DirectSetAttributeValueByName_t setAttributeValueByName = nullptr;
	bool resolved = false;
};

uint32_t s_stageMask = DirectCompositeStage_All;

void* ResolveRelCall(size_t callAddr) {
	if (!callAddr)
		return nullptr;
	int32_t rel = *(int32_t*)(callAddr + 1);
	return (void*)(callAddr + 5 + rel);
}

size_t FindClientTextPattern(const char* pattern) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client)
		return 0;
	Afx::BinUtils::ImageSectionsReader sections(client);
	if (sections.Eof())
		return 0;
	Afx::BinUtils::MemRange result = Afx::BinUtils::FindPatternString(sections.GetMemRange(), pattern);
	return result.IsEmpty() ? 0 : result.Start;
}

const DirectCompositeFns& ResolveDirectCompositeFns() {
	static bool s_attempted = false;
	static DirectCompositeFns s_fns = {};
	if (s_attempted)
		return s_fns;
	s_attempted = true;

	size_t updateCompositeCall = FindClientTextPattern("E8 ?? ?? ?? ?? 48 8D 8B ?? ?? ?? ?? 48 89 BC 24");
	size_t setAttrCall = FindClientTextPattern("E8 ?? ?? ?? ?? 66 41 0F 6E D4");
	size_t updateSkin = FindClientTextPattern(
		// The trailing lea displacement is a member offset that moves with class layout
		// (0x1BC0 in build 14182, 0x1BD0 in 2000918), so it is wildcarded. Two copies of
		// this wrapper have the same calls and instruction body; either is valid.
		"48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 E8 ?? ?? ?? ?? F6 C3 01 74 0A 33 D2 48 8B CF E8 ?? ?? ?? ?? 48 8D 8F ?? ?? 00 00");

	s_fns.updateCompositeMaterial = (DirectUpdateCompositeMaterial_t)ResolveRelCall(updateCompositeCall);
	// UpdateSkin's forced-refresh branch calls the material-set refresh with
	// (weapon, false). Resolve that call rather than the callee's stack-frame
	// prologue, which changed in build 2000924. Both matched wrappers call the
	// same function; the anchored E8 sits 30 bytes into the wrapper.
	s_fns.updateCompositeMaterialSet = (DirectUpdateCompositeMaterialSet_t)ResolveRelCall(updateSkin ? updateSkin + 30 : 0);
	s_fns.updateSkin = (DirectUpdateSkin_t)updateSkin;
	s_fns.setAttributeValueByName = (DirectSetAttributeValueByName_t)ResolveRelCall(setAttrCall);
	s_fns.resolved = s_fns.updateCompositeMaterial && s_fns.updateCompositeMaterialSet && s_fns.updateSkin;
	MvmDebugLog_Linef("cosmetics.composite.resolve", "material=%p set=%p skin=%p attributes=%p resolved=%u",
		s_fns.updateCompositeMaterial,s_fns.updateCompositeMaterialSet,s_fns.updateSkin,s_fns.setAttributeValueByName,s_fns.resolved?1:0);
	return s_fns;
}

// ---- composite-material owner subobject --------------------------------------------------------------
//
// UpdateCompositeMaterial takes the weapon's embedded composite-material-owner base subobject, not the
// entity. Its position is a class-layout offset (0x608 through build 2000924) that moves whenever a base
// class grows -- CS2 build 2000930 moved it, every call faulted inside SEH and no skin texture was rebuilt.
// It is found from MSVC RTTI instead: the entity's complete-object locator links the class hierarchy
// descriptor, whose base-class array names every base with its displacement (mdisp) in the object.

bool InClientImage(uintptr_t p, uintptr_t base, uintptr_t size) {
	return p >= base && p < base + size;
}

ptrdiff_t FindCompositeOwnerOffsetUnsafe(unsigned char* weapon, uintptr_t base, uintptr_t size, bool log) {
	const uintptr_t vptr = *(uintptr_t*)weapon;
	if (!InClientImage(vptr, base, size))
		return -1;
	const uintptr_t col = *(uintptr_t*)(vptr - 8);
	if (!InClientImage(col, base, size) || *(uint32_t*)col != 1 || *(uint32_t*)(col + 4) != 0)
		return -1; // not the complete object's primary vtable
	const uint32_t chdRva = *(uint32_t*)(col + 16);
	if (chdRva == 0 || chdRva >= size)
		return -1;
	const uintptr_t chd = base + chdRva;
	const uint32_t numBases = *(uint32_t*)(chd + 8);
	const uint32_t arrayRva = *(uint32_t*)(chd + 12);
	if (numBases == 0 || numBases > 256 || arrayRva == 0 || arrayRva >= size)
		return -1;
	ptrdiff_t found = -1;
	for (uint32_t i = 0; i < numBases; ++i) {
		const uint32_t bcdRva = *(uint32_t*)(base + arrayRva + 4 * i);
		if (bcdRva == 0 || bcdRva >= size)
			continue;
		const uintptr_t bcd = base + bcdRva;
		const uint32_t typeRva = *(uint32_t*)bcd;
		if (typeRva == 0 || typeRva >= size)
			continue;
		const char* name = (const char*)(base + typeRva + 0x10);
		const int32_t mdisp = *(int32_t*)(bcd + 8);
		const int32_t pdisp = *(int32_t*)(bcd + 12);
		if (log && MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.composite.owner", "base[%u] mdisp=0x%x pdisp=%d %s",
				i, (unsigned)mdisp, pdisp, name);
		if (found < 0 && pdisp == -1 && mdisp > 0 && strstr(name, "CompositeMaterial"))
			found = mdisp;
	}
	return found;
}

ptrdiff_t s_ownerOffset = -1; // -1 = not resolved yet

ptrdiff_t ResolveCompositeOwnerOffset(unsigned char* weapon, ptrdiff_t fallback) {
	if (s_ownerOffset >= 0)
		return s_ownerOffset;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client || !weapon)
		return fallback;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)client
		+ ((const IMAGE_DOS_HEADER*)client)->e_lfanew);
	const uintptr_t base = (uintptr_t)client;
	const uintptr_t size = nt->OptionalHeader.SizeOfImage;
	ptrdiff_t found = -1;
	__try {
		found = FindCompositeOwnerOffsetUnsafe(weapon, base, size, true);
	} __except (1) {
		found = -1;
	}
	if (MvmDebugLog_Active())
		MvmDebugLog_LinefAlways("cosmetics.composite.owner", "resolved ownerOffset=0x%llx (%s)",
			(unsigned long long)(found >= 0 ? found : fallback), found >= 0 ? "rtti" : "fallback");
	if (found < 0)
		return fallback;
	s_ownerOffset = found;
	return found;
}

} // namespace

void SetDirectCompositeStageMask(uint32_t mask) {
	s_stageMask = mask & DirectCompositeStage_All;
}

uint32_t GetDirectCompositeStageMask() {
	return s_stageMask;
}

bool ReadCompositeOwnerCounts(unsigned char* entity, int* built, int* pending) {
	HMODULE client = GetModuleHandleA("client.dll");
	if (!entity || !client)
		return false;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)client
		+ ((const IMAGE_DOS_HEADER*)client)->e_lfanew);
	__try {
		const ptrdiff_t off = FindCompositeOwnerOffsetUnsafe(entity, (uintptr_t)client,
			nt->OptionalHeader.SizeOfImage, false);
		if (off <= 0)
			return false;
		// CCompositeMaterialOwner: built materials CUtlVector count @ +0x4A0, in-flight requests @ +0x4B8
		// (sub_1814E1BD0 / sub_1814E22F0 in build 2000930).
		if (built) *built = *(int*)(entity + off + 0x4A0);
		if (pending) *pending = *(int*)(entity + off + 0x4B8);
		return true;
	} __except (1) {
		return false;
	}
}

bool ReleaseEntityCompositeMaterials(unsigned char* entity) {
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	HMODULE client = GetModuleHandleA("client.dll");
	if (!entity || !client || !fns.updateCompositeMaterial)
		return false;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)client
		+ ((const IMAGE_DOS_HEADER*)client)->e_lfanew);
	__try {
		const ptrdiff_t off = FindCompositeOwnerOffsetUnsafe(entity, (uintptr_t)client,
			nt->OptionalHeader.SizeOfImage, false);
		if (off <= 0)
			return false;
		fns.updateCompositeMaterial(entity + off, true);
		return true;
	} __except (1) {
		return false;
	}
}

ptrdiff_t DirectCompositeOwnerOffset() {
	return s_ownerOffset;
}

bool DirectCompositeResolved() {
	return ResolveDirectCompositeFns().resolved;
}

bool FireNamedSkinAttributes(unsigned char* itemView, int paintKit, float wear, int seed, int statTrak) {
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	if (!itemView || !fns.setAttributeValueByName)
		return false;
	__try {
		fns.setAttributeValueByName(itemView, "set item texture prefab", (float)paintKit);
		fns.setAttributeValueByName(itemView, "set item texture wear", wear);
		fns.setAttributeValueByName(itemView, "set item texture seed", (float)seed);
		if (statTrak >= 0)
			fns.setAttributeValueByName(itemView, "kill eater", (float)statTrak);
		return true;
	} __except (1) {
		return false;
	}
}

static size_t RemoveCosmeticAttributeAddress() {
	// Installed CS2: CEconItemView::RemoveAttribute(uint16_t), derived from the
	// named setter's description invalidation callers. The engine destroys/moves
	// the attribute and sends network notifications; never truncate its vector.
	static const size_t address = FindClientTextPattern(
		"40 53 48 83 EC 20 48 63 81 ?? ?? ?? ?? 44 0F B7 CA 33 D2 48 8B D9 85 C0 7E ?? 48 8B C8 44 8B C2 48 8B 83");
	return address;
}

bool CanRemoveCosmeticLocalAttributes() {
	const size_t address = RemoveCosmeticAttributeAddress();
	const auto& o = g_clientDllOffsets;
	if (!address || !o.C_EconItemView.m_AttributeList || !o.C_AttributeList.m_Attributes
		|| !o.CEconItemAttribute.m_iAttributeDefinitionIndex || !o.CEconItemAttribute.m_size)
		return false;
	__try {
		if (*(int32_t*)(address + 9) != o.C_EconItemView.m_AttributeList + o.C_AttributeList.m_Attributes
			|| *(int32_t*)(address + 35) != o.C_EconItemView.m_AttributeList + o.C_AttributeList.m_Attributes + 8
			|| *(uint8_t*)(address + 42) != o.CEconItemAttribute.m_iAttributeDefinitionIndex
			|| *(uint8_t*)(address + 62) != o.CEconItemAttribute.m_size)
			return false;
		return true;
	} __except (1) { return false; }
}

bool RemoveCosmeticLocalAttribute(unsigned char* itemView, unsigned definition) {
	if (!itemView || !(definition == 6 || definition == 7 || definition == 8 || definition == 80)
		|| !CanRemoveCosmeticLocalAttributes())
		return false;
	__try {
		((void (__fastcall*)(void*, uint16_t))RemoveCosmeticAttributeAddress())(itemView, (uint16_t)definition);
		return true;
	} __except (1) { return false; }
}

DirectCompositeResult FireDirectCompositeRefresh(
	unsigned char* weapon,
	unsigned char* itemView,
	ptrdiff_t offRestoreMaterial,
	ptrdiff_t compositeOwnerOffset,
	int32_t paintKit,
	float wear,
	int32_t seed,
	bool writeNamedAttributes) {
	DirectCompositeResult out = {};
	const DirectCompositeFns& fns = ResolveDirectCompositeFns();
	out.resolved = fns.resolved;
	if (!weapon || !itemView || !fns.resolved)
		return out;

	unsigned char* compositeOwner = weapon + ResolveCompositeOwnerOffset(weapon, compositeOwnerOffset);
	__try {
		const uint32_t stages = s_stageMask;
		if (MvmDebugLog_Active())
			MvmDebugLog_LinefAlways("cosmetics.composite.stage", "BEGIN mask=0x%02x", stages);
		if (offRestoreMaterial)
			*(bool*)(itemView + offRestoreMaterial) = true;
		if (writeNamedAttributes && (stages & DirectCompositeStage_NamedAttributes) && fns.setAttributeValueByName) {
			fns.setAttributeValueByName(itemView, "set item texture prefab", (float)paintKit);
			fns.setAttributeValueByName(itemView, "set item texture wear", wear);
			fns.setAttributeValueByName(itemView, "set item texture seed", (float)seed);
			out.completedStages |= DirectCompositeStage_NamedAttributes;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.composite.stage", "DONE named-attributes");
		}
		if (stages & DirectCompositeStage_OwnerMaterial) {
			fns.updateCompositeMaterial(compositeOwner, true);
			out.completedStages |= DirectCompositeStage_OwnerMaterial;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.composite.stage", "DONE owner-material");
		}
		if (stages & DirectCompositeStage_MaterialSet) {
			fns.updateCompositeMaterialSet(weapon, false);
			out.completedStages |= DirectCompositeStage_MaterialSet;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.composite.stage", "DONE material-set");
		}
		if (stages & DirectCompositeStage_UpdateSkin) {
			fns.updateSkin(weapon, true);
			out.completedStages |= DirectCompositeStage_UpdateSkin;
			if (MvmDebugLog_Active())
				MvmDebugLog_LinefAlways("cosmetics.composite.stage", "DONE update-skin");
		}
		out.called = true;
	} __except (1) {
		out.faulted = true;
	}
	return out;
}

} // namespace Filmmaker

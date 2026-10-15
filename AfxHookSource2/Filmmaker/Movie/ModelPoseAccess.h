#pragma once
// Shared, SEH-guarded CS2 model access for preview poses and Method Acting.
#include "../../ClientEntitySystem.h"
#include "../../SchemaSystem.h"
#include <cstdint>
namespace Filmmaker { namespace ModelPoseAccess {
constexpr ptrdiff_t kModelBoneNames = 0x168;
constexpr ptrdiff_t kModelBoneParents = 0x180;
inline unsigned char* ModelStateOf(CEntityInstance* e) {
	const ClientDllOffsets_t& o = g_clientDllOffsets;
	if (!e || !o.ModelChain.m_CBodyComponent || !o.ModelChain.m_skeletonInstance || !o.ModelChain.m_modelState) return nullptr;
	__try {
		unsigned char* body = *(unsigned char**)((unsigned char*)e + o.ModelChain.m_CBodyComponent);
		if ((uintptr_t)body < 0x10000) return nullptr;
		return body + o.ModelChain.m_skeletonInstance + o.ModelChain.m_modelState;
	} __except (1) { return nullptr; }
}

// The CModel behind CModelState's model handle (the qword just before m_ModelName).
inline const void* ModelOf(unsigned char* ms) {
	const ptrdiff_t nameOff = g_clientDllOffsets.ModelChain.m_ModelName;
	if (!ms || !nameOff) return nullptr;
	__try {
		const void* handle = *(const void**)(ms + nameOff - 8);
		if ((uintptr_t)handle < 0x10000) return nullptr;
		const void* model = *(const void**)handle;
		return (uintptr_t)model < 0x10000 ? nullptr : model;
	} __except (1) { return nullptr; }
}

inline bool ReadBoneEntry(const char* const* names, const int16_t* parents, int n, char* name, int nameSize, int& parent) {
	__try {
		const char* s = names[n];
		if ((uintptr_t)s < 0x10000) return false;
		int len = 0;
		for (; len + 1 < nameSize && s[len]; ++len) {
			if (s[len] < 0x20 || s[len] > 0x7e) return false;
			name[len] = s[len];
		}
		if (len == 0 || s[len] != '\0') return false;
		name[len] = '\0';
		parent = parents[n];
		return true;
	} __except (1) { return false; }
}

inline bool ReadTablePointers(const void* model, const char* const*& names, const int16_t*& parents) {
	__try {
		names = *(const char* const**)((const unsigned char*)model + kModelBoneNames);
		parents = *(const int16_t**)((const unsigned char*)model + kModelBoneParents);
		return (uintptr_t)names >= 0x10000 && (uintptr_t)parents >= 0x10000;
	} __except (1) { return false; }
}


} }

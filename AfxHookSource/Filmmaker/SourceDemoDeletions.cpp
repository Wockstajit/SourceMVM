#include "stdafx.h"

#include "SourceDemoDeletions.h"
#include "SourceSdk2013.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>
#include <Shared/binutils.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace SourceFilmmaker {

namespace {

using namespace Sdk2013;

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
typedef void (*ReadDeletionsFn)(void* clientState, void* readInfo);
typedef void* (*GetClientNetworkableFn)(void* entityList, int index);

ReadDeletionsFn g_readDeletions = nullptr;
GetClientNetworkableFn g_getClientNetworkable = nullptr;
bool g_done = false;
int g_tries = 0;
std::string g_status = "not installed yet";
unsigned long long g_kept = 0;

// The entities the packet being read deletes but still sends, and the thread reading it (0: none).
const int kMaxKeep = 64;
int g_keep[kMaxKeep];
int g_keepCount = 0;
DWORD g_keepThread = 0;

int ReadBits(const uint32_t* data, int& position, int count) {
	int value = 0;
	for (int i = 0; i < count; ++i, ++position)
		value |= (int)((data[position >> 5] >> (position & 31)) & 1) << i;
	return value;
}

// Reads the packet's deletion list ahead without moving its read position, and keeps every listed
// entity the new frame still sends. Guarded: a layout that does not read as one keeps nothing.
void FindKept(const unsigned char* readInfo) {
	g_keepCount = 0;
	__try {
		const unsigned char* buffer = *(const unsigned char* const*)(readInfo + kReadInfoBuffer);
		const unsigned char* to = *(const unsigned char* const*)(readInfo + kReadInfoTo);
		if (!buffer || !to) return;
		const uint32_t* data = *(const uint32_t* const*)(buffer + kBitReadData);
		const int size = *(const int*)(buffer + kBitReadSize);
		int position = *(const int*)(buffer + kBitReadPosition);
		const uint32_t* transmit = (const uint32_t*)(to + kFrameTransmitBits);
		while (position + 1 + kEdictIndexBits <= size && ReadBits(data, position, 1)) {
			const int index = ReadBits(data, position, kEdictIndexBits);
			if (((transmit[index >> 5] >> (index & 31)) & 1) && g_keepCount < kMaxKeep) g_keep[g_keepCount++] = index;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		g_keepCount = 0;
	}
}

void HookReadDeletions(void* clientState, void* readInfo) {
	FindKept((const unsigned char*)readInfo);
	if (g_keepCount == 0) {
		g_readDeletions(clientState, readInfo);
		return;
	}
	g_keepThread = GetCurrentThreadId();
	g_readDeletions(clientState, readInfo);
	g_keepThread = 0;
	g_kept += (unsigned long long)g_keepCount;
	if (Filmmaker::MvmDebugLog_Active())
		for (int i = 0; i < g_keepCount; ++i)
			Filmmaker::MvmDebugLog_Linef("demo.deletions", "kept entity %d: the packet deletes it while its frame still sends it", g_keep[i]);
}

void* HookGetClientNetworkable(void* entityList, int index) {
	if (g_keepThread != 0 && g_keepThread == GetCurrentThreadId())
		for (int i = 0; i < g_keepCount; ++i)
			if (g_keep[i] == index) return nullptr;
	return g_getClientNetworkable(entityList, index);
}

bool InModule(const void* p, HMODULE module) {
	if (!p || !module) return false;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)module + dos->e_lfanew);
	const uintptr_t lo = (uintptr_t)module, hi = lo + nt->OptionalHeader.SizeOfImage;
	return (uintptr_t)p >= lo && (uintptr_t)p < hi;
}

// Whether the function loads the string "ReadDeletions" (lea rdx, [rip+disp32]) in its first bytes.
// Guarded.
bool NamesReadDeletions(const unsigned char* function, HMODULE engine) {
	static const char kName[] = "ReadDeletions";
	__try {
		for (int i = 0; i < 0x120; ++i) {
			if (function[i] != 0x48 || function[i + 1] != 0x8D || function[i + 2] != 0x15) continue;
			const char* text = (const char*)(function + i + 7 + *(const int32_t*)(function + i + 3));
			if (InModule(text, engine) && 0 == std::memcmp(text, kName, sizeof(kName))) return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

void Finish(const std::string& status) {
	g_done = true;
	g_status = status;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.deletions", "%s", status.c_str());
}

} // namespace

void DemoDeletions_Init() {
	if (g_done) return;
	HMODULE engineDll = GetModuleHandleW(L"engine.dll");
	HMODULE clientDll = GetModuleHandleW(L"client.dll");
	auto createInterface = clientDll ? (CreateInterfaceFn)GetProcAddress(clientDll, "CreateInterface") : nullptr;
	void* entityList = createInterface ? createInterface("VClientEntityList003", nullptr) : nullptr;
	if (!engineDll || !entityList) {
		if (++g_tries > 600) Finish("NOT installed: no engine.dll or no VClientEntityList003");
		return;
	}
	void** vtable = (void**)Afx::BinUtils::FindClassVtable(engineDll, ".?AVCClientState@@", 0, 0x0);
	int slot = -1;
	for (int i = 0; vtable && i < 64 && InModule(vtable[i], engineDll); ++i)
		if (NamesReadDeletions((const unsigned char*)vtable[i], engineDll)) { slot = i; break; }
	void** networkableSlot = &(*(void***)entityList)[Slot::EntityList_GetClientNetworkable];
	if (slot < 0 || !InModule(*networkableSlot, clientDll)) {
		Finish("NOT installed: CClientState's ReadDeletions or the entity list's GetClientNetworkable not found");
		return;
	}
	char status[200];
	std::snprintf(status, sizeof(status), "entities a packet deletes while still sending them are kept (ReadDeletions: CClientState slot %d, engine.dll+0x%llx)",
		slot, (unsigned long long)((uintptr_t)vtable[slot] - (uintptr_t)engineDll));
	// The entity list first, so g_getClientNetworkable is set before any wrapped ReadDeletions runs.
	if (!AfxDetourPtr((PVOID*)networkableSlot, (PVOID)&HookGetClientNetworkable, (PVOID*)&g_getClientNetworkable)
		|| !AfxDetourPtr((PVOID*)&vtable[slot], (PVOID)&HookReadDeletions, (PVOID*)&g_readDeletions)) {
		Finish("NOT installed: a pointer could not be written");
		return;
	}
	Finish(status);
}

void DemoDeletions_PrintHealth() {
	if (g_readDeletions)
		advancedfx::Message("  deletions:  %s, %llu kept\n", g_status.c_str(), g_kept);
	else
		advancedfx::Message("  deletions:  %s\n", g_status.c_str());
}

} // namespace SourceFilmmaker

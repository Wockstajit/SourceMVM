#include "stdafx.h"

#include "SourceDemoSkip.h"
#include "SourceEngine.h"
#include "SourceGameProfile.h"
#include "SourceSdk2013.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <Shared/AfxConsole.h>
#include <Shared/AfxDetours.h>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace SourceFilmmaker {

namespace {

using namespace Sdk2013;

typedef void* (*CreateInterfaceFn)(const char* name, int* returnCode);
typedef void* (*CreateClientClassFn)(int entnum, int serialNum);
typedef bool (*IsSkippingPlaybackFn)(void* engine);

CreateClientClassFn g_createRagdoll = nullptr;
IsSkippingPlaybackFn g_isSkipping = nullptr;
char g_className[64] = "";
bool g_done = false;
int g_tries = 0;
std::string g_status = "not installed yet";
// The thread running the ragdoll's create function (0: none), and whether the engine was skipping.
DWORD g_creatingThread = 0;
bool g_skipHidden = false;
unsigned long long g_kept = 0;

bool HookIsSkippingPlayback(void* engine) {
	const bool skipping = g_isSkipping(engine);
	if (skipping && g_creatingThread != 0 && g_creatingThread == GetCurrentThreadId()) {
		g_skipHidden = true;
		return false;
	}
	return skipping;
}

void* CreateRagdoll(int entnum, int serialNum) {
	g_creatingThread = GetCurrentThreadId();
	g_skipHidden = false;
	void* networkable = g_createRagdoll(entnum, serialNum);
	g_creatingThread = 0;
	if (g_skipHidden) {
		++g_kept;
		if (Filmmaker::MvmDebugLog_Active())
			Filmmaker::MvmDebugLog_Linef("demo.skip", "kept %s %d, created during a skip", g_className, entnum);
	}
	return networkable;
}

bool InModule(const void* p, HMODULE module) {
	if (!p || !module) return false;
	const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)module;
	const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)module + dos->e_lfanew);
	const uintptr_t lo = (uintptr_t)module, hi = lo + nt->OptionalHeader.SizeOfImage;
	return (uintptr_t)p >= lo && (uintptr_t)p < hi;
}

// The first class the profile calls a ragdoll, its name copied into g_className. Guarded: a class
// list that does not read as one finds nothing.
ClientClass* FindRagdollClass(const ClientClass* classes, bool (*isRagdoll)(const char*)) {
	__try {
		for (const ClientClass* c = classes; c; c = c->m_pNext) {
			if (!c->m_pNetworkName || !isRagdoll(c->m_pNetworkName)) continue;
			strncpy_s(g_className, c->m_pNetworkName, _TRUNCATE);
			return const_cast<ClientClass*>(c);
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return nullptr;
}

void Finish(const std::string& status) {
	g_done = true;
	g_status = status;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("demo.skip", "%s", status.c_str());
}

} // namespace

void DemoSkip_Init() {
	if (g_done) return;
	const GameProfile& profile = GameProfile_Current();
	if (!profile.isRagdollClass) {
		Finish("off: the game profile names no ragdoll class");
		return;
	}
	const ClientClass* classes = Engine_ClientClasses();
	HMODULE clientDll = GetModuleHandleW(L"client.dll");
	HMODULE engineDll = GetModuleHandleW(L"engine.dll");
	auto createInterface = engineDll ? (CreateInterfaceFn)GetProcAddress(engineDll, "CreateInterface") : nullptr;
	void* engine = createInterface ? createInterface("VEngineClient013", nullptr) : nullptr;
	ClientClass* ragdoll = classes ? FindRagdollClass(classes, profile.isRagdollClass) : nullptr;
	if (!ragdoll || !engine) {
		if (++g_tries > 600) Finish("NOT installed: no ragdoll client class or no VEngineClient013");
		return;
	}
	void** slot = &(*(void***)engine)[Slot::EngineClient_IsSkippingPlayback];
	if (!InModule(*slot, engineDll) || !InModule(ragdoll->m_pCreateFn, clientDll)) {
		Finish("NOT installed: IsSkippingPlayback or the ragdoll's create function is not where expected");
		return;
	}
	char status[200];
	std::snprintf(status, sizeof(status), "%s kept when made during a skip (create client.dll+0x%llx, IsSkippingPlayback engine.dll+0x%llx)",
		g_className, (unsigned long long)((uintptr_t)ragdoll->m_pCreateFn - (uintptr_t)clientDll),
		(unsigned long long)((uintptr_t)*slot - (uintptr_t)engineDll));
	// The engine slot first, so g_isSkipping is set before any ragdoll is made through the wrapper.
	if (!AfxDetourPtr((PVOID*)slot, (PVOID)&HookIsSkippingPlayback, (PVOID*)&g_isSkipping)
		|| !AfxDetourPtr((PVOID*)&ragdoll->m_pCreateFn, (PVOID)&CreateRagdoll, (PVOID*)&g_createRagdoll)) {
		Finish("NOT installed: a pointer could not be written");
		return;
	}
	Finish(status);
}

void DemoSkip_PrintHealth() {
	if (g_createRagdoll)
		advancedfx::Message("  demo skip:  %s, %llu kept\n", g_status.c_str(), g_kept);
	else
		advancedfx::Message("  demo skip:  %s\n", g_status.c_str());
}

} // namespace SourceFilmmaker

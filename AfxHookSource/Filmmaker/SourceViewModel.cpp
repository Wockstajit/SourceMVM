#include "stdafx.h"

#include "SourceViewModel.h"

#include <AfxHookSource2/Filmmaker/Debug/MvmDebugLog.h>
#include <AfxHookSource2/Filmmaker/Movie/CameraShakeStack.h>
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

// C_BaseViewModel::CalcViewModelView(C_BasePlayer* owner, const Vector& eyePosition, const QAngle& eyeAngles).
using CalcViewModelViewFn = void(__fastcall*)(void* self, void* owner, const float* eyePosition, const float* eyeAngles);

CalcViewModelViewFn g_original = nullptr;
bool g_done = false;
int g_tries = 0;
std::string g_status = "not installed yet";

// A CalcViewModelView call held for the view override: the viewmodel, its owner and the eye.
struct Held {
	void* self;
	void* owner;
	float origin[3];
	float angles[3];
};
constexpr int kMaxHeld = 4; // a player's viewmodels: CS:S draws one
Held g_held[kMaxHeld];
int g_heldCount = 0;

double g_kick[6] = {};  // world x y z, pitch yaw roll
double g_delta[6] = {};
unsigned long long g_heldCalls = 0, g_builtCalls = 0, g_passCalls = 0, g_lateCalls = 0;

bool InModule(const void* p, HMODULE module) {
	if (!p || !module) return false;
	const auto* dos = (const IMAGE_DOS_HEADER*)module;
	const auto* nt = (const IMAGE_NT_HEADERS*)((const unsigned char*)module + dos->e_lfanew);
	const uintptr_t base = (uintptr_t)module, end = base + nt->OptionalHeader.SizeOfImage;
	return (uintptr_t)p >= base && (uintptr_t)p < end;
}

// CalcViewModelView is the one C_BaseViewModel slot whose body lets the view shake reach the gun at a
// tenth of its strength, vieweffects->ApplyShake(vmorigin, vmangles, 0.1): movss xmm3, 0.1f.
// Guarded: a slot can point at code shorter than the window.
bool LoadsTenthIntoXmm3(const unsigned char* function, HMODULE client) {
	__try {
		for (int i = 0; i < 0x200; ++i) {
			if (function[i] != 0xF3 || function[i + 1] != 0x0F || function[i + 2] != 0x10 || function[i + 3] != 0x1D) continue;
			const float* value = (const float*)(function + i + 8 + *(const int32_t*)(function + i + 4));
			if (InModule(value, client) && *value == 0.1f) return true;
		}
	} __except (EXCEPTION_EXECUTE_HANDLER) {
	}
	return false;
}

void Finish(const std::string& status) {
	g_done = true;
	g_status = status;
	if (Filmmaker::MvmDebugLog_Active()) Filmmaker::MvmDebugLog_Linef("viewmodel", "%s", status.c_str());
}

void Build(const Held& h, bool moved) {
	float origin[3], angles[3];
	for (int i = 0; i < 3; ++i) {
		origin[i] = h.origin[i] + (moved ? (float)(g_delta[i] + g_kick[i]) : 0.0f);
		angles[i] = h.angles[i] + (moved ? (float)(g_delta[3 + i] + g_kick[3 + i]) : 0.0f);
	}
	g_original(h.self, h.owner, origin, angles);
}

void BuildHeld(bool moved) {
	for (int i = 0; i < g_heldCount; ++i) Build(g_held[i], moved);
	g_heldCount = 0;
}

void __fastcall HookCalcViewModelView(void* self, void* owner, const float* eyePosition, const float* eyeAngles) {
	if (!eyePosition || !eyeAngles || !Filmmaker::CameraShake_Active()) {
		++g_passCalls;
		g_original(self, owner, eyePosition, eyeAngles);
		return;
	}
	// Held again before a view override built it: build the earlier ones as the game would have.
	bool again = g_heldCount == kMaxHeld;
	for (int i = 0; i < g_heldCount; ++i) again = again || g_held[i].self == self;
	if (again) {
		g_lateCalls += (unsigned long long)g_heldCount;
		BuildHeld(false);
	}
	Held& h = g_held[g_heldCount++];
	h.self = self;
	h.owner = owner;
	for (int i = 0; i < 3; ++i) {
		h.origin[i] = eyePosition[i];
		h.angles[i] = eyeAngles[i];
	}
	++g_heldCalls;
}

} // namespace

void ViewModel_Init() {
	if (g_done) return;
	HMODULE client = GetModuleHandleW(L"client.dll");
	void** vtable = client ? (void**)Afx::BinUtils::FindClassVtable(client, ".?AVC_BaseViewModel@@", 0, 0x0) : nullptr;
	if (!vtable) {
		if (++g_tries > 600) Finish("NOT installed: no C_BaseViewModel vtable in client.dll");
		return;
	}
	int slot = -1;
	for (int i = 0; i < 400 && InModule(vtable[i], client); ++i)
		if (LoadsTenthIntoXmm3((const unsigned char*)vtable[i], client)) {
			slot = i;
			break;
		}
	if (slot < 0) {
		Finish("NOT installed: CalcViewModelView not found among C_BaseViewModel's slots");
		return;
	}
	// The function itself, not the slot: CS:S's viewmodels are C_PredictedViewModel, which inherits it.
	g_original = (CalcViewModelViewFn)vtable[slot];
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_original, (PVOID)&HookCalcViewModelView);
	if (NO_ERROR != DetourTransactionCommit()) {
		g_original = nullptr;
		Finish("NOT installed: CalcViewModelView could not be detoured");
		return;
	}
	char status[200];
	std::snprintf(status, sizeof(status), "CalcViewModelView detoured (C_BaseViewModel slot %d, client.dll+0x%llx)", slot,
		(unsigned long long)((uintptr_t)vtable[slot] - (uintptr_t)client));
	Finish(status);
}

bool ViewModel_Installed() { return g_original != nullptr; }

const char* ViewModel_Status() { return g_status.c_str(); }

void ViewModel_SetWeaponKick(double x, double y, double z, double pitch, double yaw, double roll) {
	g_kick[0] = x;
	g_kick[1] = y;
	g_kick[2] = z;
	g_kick[3] = pitch;
	g_kick[4] = yaw;
	g_kick[5] = roll;
}

void ViewModel_SetCameraDelta(double x, double y, double z, double pitch, double yaw, double roll) {
	g_delta[0] = x;
	g_delta[1] = y;
	g_delta[2] = z;
	g_delta[3] = pitch;
	g_delta[4] = yaw;
	g_delta[5] = roll;
}

void ViewModel_OnView() {
	if (!g_heldCount) return;
	if (Filmmaker::MvmDebugLog_Active()) {
		static unsigned throttle = 0;
		if ((throttle++ % 15) == 0)
			Filmmaker::MvmDebugLog_Linef("viewmodel.kick",
				"held=%d eye=(%.2f %.2f %.2f / %.2f %.2f %.2f) camera=(%.3f %.3f %.3f / %.3f %.3f %.3f) kick=(%.3f %.3f %.3f / %.3f %.3f %.3f)",
				g_heldCount, g_held[0].origin[0], g_held[0].origin[1], g_held[0].origin[2], g_held[0].angles[0], g_held[0].angles[1],
				g_held[0].angles[2], g_delta[0], g_delta[1], g_delta[2], g_delta[3], g_delta[4], g_delta[5], g_kick[0], g_kick[1], g_kick[2],
				g_kick[3], g_kick[4], g_kick[5]);
	}
	g_builtCalls += (unsigned long long)g_heldCount;
	BuildHeld(true);
}

void ViewModel_Frame() {
	if (!g_heldCount) return;
	g_lateCalls += (unsigned long long)g_heldCount;
	BuildHeld(false);
}

void ViewModel_PrintHealth() {
	if (g_original)
		advancedfx::Message("  viewmodel:  %s; %llu built after the view (%llu late), %llu passed through\n", g_status.c_str(), g_builtCalls,
			g_lateCalls, g_passCalls);
	else
		advancedfx::Message("  viewmodel:  %s\n", g_status.c_str());
}

} // namespace SourceFilmmaker

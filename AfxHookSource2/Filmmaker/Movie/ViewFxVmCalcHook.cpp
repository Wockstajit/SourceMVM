#include "ViewFxVmInternal.h"

#include "../Debug/MvmDebugLog.h"
#include "../../MirvTime.h"
#include "../../../Shared/AfxConsole.h"
#include "../../../Shared/binutils.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../../Deps/release/Detours/src/detours.h"

#include <cstdint>

namespace Filmmaker::ViewFxVmInternal {
namespace {

using VmCaller = void(__fastcall*)(void* player, float* eyeOrigin, float* eyeAngles, bool update);

VmCaller g_original = nullptr;
std::size_t g_helperAddress = 0;
bool g_installed = false;

void __fastcall Hook(void* player, float* eyeOrigin, float* eyeAngles, bool update) {
	// Recorded unconditionally (including the no-adjustment early-out below) so the camera-side
	// gap detector in ViewFxVm_SetCameraCompensation sees every frame the viewmodel really ran.
	g_lastVmCallerFrame.store(g_MirvTime.framecount_get(), std::memory_order_relaxed);
	const bool aimActive = g_site.load(std::memory_order_relaxed) == 4
		&& g_mode.load(std::memory_order_relaxed) == 1;
	const double compX = g_cameraCompX.load(std::memory_order_relaxed);
	const double compY = g_cameraCompY.load(std::memory_order_relaxed);
	const double compZ = g_cameraCompZ.load(std::memory_order_relaxed);
	const double compPitch = g_cameraCompPitch.load(std::memory_order_relaxed);
	const double compYaw = g_cameraCompYaw.load(std::memory_order_relaxed);
	const double compRoll = g_cameraCompRoll.load(std::memory_order_relaxed);
	// Weapon Recoil kick: ADDED (compensation is subtracted). Same replace-every-frame
	// contract as compensation, fed by CameraShakeStack.
	const double kickX = g_weaponKickX.load(std::memory_order_relaxed);
	const double kickY = g_weaponKickY.load(std::memory_order_relaxed);
	const double kickZ = g_weaponKickZ.load(std::memory_order_relaxed);
	const double kickPitch = g_weaponKickPitch.load(std::memory_order_relaxed);
	const double kickYaw = g_weaponKickYaw.load(std::memory_order_relaxed);
	const double kickRoll = g_weaponKickRoll.load(std::memory_order_relaxed);
	const bool hasAim = aimActive && (g_freeAimPitch != 0.0 || g_freeAimYaw != 0.0);
	const bool hasComp = compX != 0.0 || compY != 0.0 || compZ != 0.0
		|| compPitch != 0.0 || compYaw != 0.0 || compRoll != 0.0;
	const bool hasKickPos = kickX != 0.0 || kickY != 0.0 || kickZ != 0.0;
	const bool hasKickAng = kickPitch != 0.0 || kickYaw != 0.0 || kickRoll != 0.0;
	if (!hasAim && !hasComp && !hasKickPos && !hasKickAng) {
		g_original(player, eyeOrigin, eyeAngles, update);
		return;
	}

	// CalcViewModelView builds the complete animated viewmodel from these temporary inputs.
	// Restore the caller-owned values afterwards, making repeated calls non-accumulating.
	float originalOrigin[3] = {};
	float appliedOrigin[3] = {};
	float originalAngles[3] = {};
	float appliedAngles[3] = {};
	bool wroteOrigin = false;
	bool wroteAngles = false;
	double aimPitch = 0.0;
	double aimYaw = 0.0;
	__try {
		if (eyeOrigin && (hasComp || hasKickPos)) {
			for (int i = 0; i < 3; ++i) originalOrigin[i] = eyeOrigin[i];
			appliedOrigin[0] = (float)(originalOrigin[0] - compX + kickX);
			appliedOrigin[1] = (float)(originalOrigin[1] - compY + kickY);
			appliedOrigin[2] = (float)(originalOrigin[2] - compZ + kickZ);
			for (int i = 0; i < 3; ++i) eyeOrigin[i] = appliedOrigin[i];
			wroteOrigin = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		wroteOrigin = false;
	}
	__try {
		if (eyeAngles) {
			for (int i = 0; i < 3; ++i) originalAngles[i] = eyeAngles[i];
			aimPitch = aimActive ? g_freeAimPitch : 0.0;
			aimYaw = aimActive ? g_freeAimYaw : 0.0;
			appliedAngles[0] = (float)(originalAngles[0] + aimPitch - compPitch + kickPitch);
			appliedAngles[1] = (float)(originalAngles[1] + aimYaw - compYaw + kickYaw);
			appliedAngles[2] = (float)(originalAngles[2] - compRoll + kickRoll);
			for (int i = 0; i < 3; ++i) eyeAngles[i] = appliedAngles[i];
			wroteAngles = true;
		}
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		wroteAngles = false;
	}

	g_original(player, eyeOrigin, eyeAngles, update);

	if (wroteOrigin || wroteAngles) {
		__try {
			if (wroteOrigin)
				for (int i = 0; i < 3; ++i) eyeOrigin[i] = originalOrigin[i];
			if (wroteAngles)
				for (int i = 0; i < 3; ++i) eyeAngles[i] = originalAngles[i];
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	const unsigned callsThisFrame = SceneTransform_ObserveCallerFrame();

	if (MvmDebugLog_Active()) {
		static unsigned throttle = 0;
		if ((throttle++ % 15) == 0)
			MvmDebugLog_Linef("viewfx.vm.caller",
				"frame=%d this=%p originArg=%p anglesArg=%p extraArg=%p "
				"aim=(%.3f %.3f) "
				"compPos=(%.3f %.3f %.3f) compAng=(%.3f %.3f %.3f) "
				"kickPos=(%.3f %.3f %.3f) kickAng=(%.3f %.3f %.3f) "
				"inputPos=(%.3f %.3f %.3f) appliedPos=(%.3f %.3f %.3f) "
				"inputAng=(%.3f %.3f %.3f) appliedAng=(%.3f %.3f %.3f) "
				"restoredOrigin=%d restoredAngles=%d callsThisFrame=%u",
				g_MirvTime.framecount_get(), player, eyeOrigin, eyeAngles,
				(void*)(uintptr_t)(update ? 1 : 0),
				aimPitch, aimYaw,
				compX, compY, compZ, compPitch, compYaw, compRoll,
				kickX, kickY, kickZ, kickPitch, kickYaw, kickRoll,
				originalOrigin[0], originalOrigin[1], originalOrigin[2],
				appliedOrigin[0], appliedOrigin[1], appliedOrigin[2],
				originalAngles[0], originalAngles[1], originalAngles[2],
				appliedAngles[0], appliedAngles[1], appliedAngles[2],
				wroteOrigin ? 1 : 0, wroteAngles ? 1 : 0, callsThisFrame);
	}
}

unsigned char* FindEnclosingFunctionStart(unsigned char* sectionStart, unsigned char* at) {
	unsigned char* q = at;
	unsigned char* lo = at - 0x1000 > sectionStart ? at - 0x1000 : sectionStart;
	for (; q > lo; --q)
		if (q[-1] == 0xCC && q[-2] == 0xCC && q[0] != 0xCC)
			return q;
	return nullptr;
}

std::size_t FindCalcViewModelView() {
	if (!g_helperAddress) return 0;
	HMODULE client = GetModuleHandleA("client.dll");
	if (!client) return 0;
	unsigned char* offsetApplierStart = nullptr;
	Afx::BinUtils::ImageSectionsReader sections(client);
	for (; !sections.Eof(); sections.Next()) {
		Afx::BinUtils::MemRange range = sections.GetMemRange();
		unsigned char* p = (unsigned char*)range.Start;
		unsigned char* end = (unsigned char*)range.End - 5;
		for (; p < end; ++p) {
			if (*p != 0xE8) continue;
			const long long rel = *(int*)(p + 1);
			if ((std::size_t)(p + 5 + rel) != g_helperAddress) continue;
			if (unsigned char* start = FindEnclosingFunctionStart((unsigned char*)range.Start, p))
				offsetApplierStart = start;
		}
	}
	if (!offsetApplierStart) {
		advancedfx::Warning("[viewfxvm] no offset-applier call targeting the helper was found.\n");
		return 0;
	}

	Afx::BinUtils::ImageSectionsReader callers(client);
	for (; !callers.Eof(); callers.Next()) {
		Afx::BinUtils::MemRange range = callers.GetMemRange();
		unsigned char* p = (unsigned char*)range.Start;
		unsigned char* end = (unsigned char*)range.End - 5;
		for (; p < end; ++p) {
			if (*p != 0xE8) continue;
			const long long rel = *(int*)(p + 1);
			if (p + 5 + rel != offsetApplierStart) continue;
			if (unsigned char* start = FindEnclosingFunctionStart((unsigned char*)range.Start, p)) {
				advancedfx::Message(
					"[viewfxvm] helper=%p offset-applier=%p CalcViewModelView call=%p start=%p.\n",
					(void*)g_helperAddress, offsetApplierStart, p, start);
				return (std::size_t)start;
			}
		}
	}
	advancedfx::Warning("[viewfxvm] offset applier %p had no CalcViewModelView caller.\n",
		offsetApplierStart);
	return 0;
}

} // namespace

void CalcHook_NoteViewmodelHelper(std::size_t helperAddr) {
	g_helperAddress = helperAddr;
}

bool CalcHook_EnsureInstalled() {
	if (g_installed) return true;
	const std::size_t start = FindCalcViewModelView();
	if (!start) return false;
	g_original = (VmCaller)start;
	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)g_original, Hook);
	g_installed = NO_ERROR == DetourTransactionCommit();
	advancedfx::Message("[viewfxvm] caller detour %s.\n", g_installed ? "installed" : "FAILED");
	return g_installed;
}

bool CalcHook_Installed() {
	return g_installed;
}

} // namespace Filmmaker::ViewFxVmInternal
